/*
 * audio_snapshot_test.c - T0 guard for the N-17/N-18 primary-voice contract.
 *
 * Plan: docs/AUDIO_NFSU2_PORT_PLAN.md (N-17, N-18).
 *
 * The test has four jobs:
 *   1. Reproduce C-1 on the legacy per-slot path: a slot left in the writing
 *      state turns a valid frame into silence.
 *   2. Prove the triple full-ring snapshot publishes a multi-slot region
 *      atomically: while the writer is between "active copied" and "active
 *      swapped", the reader still sees the previous complete version.
 *   3. Prove the reader never reads a snapshot the writer is reusing: hold a
 *      snapshot across three publishes, then exercise the load(active) ->
 *      ref -> recheck path with the writer publishing in between.
 *   4. Run a writer/reader race for 10^6 rendered frames and require every
 *      256-frame chunk to be one complete, non-zero version.
 *   5. Check the debug-only N-16b counters: C-1/`inflight_zero`, a
 *      deterministic `seq_retry`, stale snapshot silence and reset clearing.
 *
 * WC3_MIXER_TEST_HOOKS is test-only.  Production translation units compile the
 * same header without it, so both hooks disappear from the product.
 */
#define WC3_MIXER_TEST_HOOKS 1
#include "audio_mixer.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_RATE       44100u
#define TEST_FRAMES     1024u                       /* 4 slots */
#define TEST_BYTES      (TEST_FRAMES * 4u)          /* stereo s16 */
#define TEST_CHUNK      256u
#define STRESS_FRAMES   1000000u
#define STRESS_CHUNKS   (STRESS_FRAMES / TEST_CHUNK)
#define STRESS_MAX_VERSION 30000

static _Atomic int g_failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL %s\n", what);
        atomic_fetch_add_explicit(&g_failures, 1, memory_order_relaxed);
    }
}

static void fill_version(uint8_t *dst, uint32_t bytes, int16_t version)
{
    for (uint32_t i = 0; i + 4u <= bytes; i += 4u) {
        memcpy(dst + i, &version, sizeof version);
        memcpy(dst + i + 2u, &version, sizeof version);
    }
}

static void fill_pattern(uint8_t *dst, uint32_t bytes)
{
    for (uint32_t i = 0; i + 4u <= bytes; i += 4u) {
        int16_t left = (int16_t)(((i * 37u) & 0x3FFFu) + 1u);
        int16_t right = (int16_t)(((i * 53u + 977u) & 0x3FFFu) + 1u);
        memcpy(dst + i, &left, sizeof left);
        memcpy(dst + i + 2u, &right, sizeof right);
    }
}

/* The mixer renders float PCM at unity gain; with step == 1 the linear path
 * reproduces the source s16 exactly, so truncation is lossless here. */
static int16_t float_sample(const float *pcm, uint32_t frame, unsigned channel)
{
    return (int16_t)(pcm[frame * 2u + channel] * 32768.0f);
}

static int chunk_is_version(const float *pcm, uint32_t frames, int16_t version)
{
    int16_t first = float_sample(pcm, 0u, 0u);
    if (first != version) return 0;
    for (uint32_t f = 0u; f < frames; f++) {
        if (float_sample(pcm, f, 0u) != version) return 0;
        if (float_sample(pcm, f, 1u) != version) return 0;
    }
    return 1;
}

static int chunk_is_uniform(const float *pcm, uint32_t frames, int16_t *out)
{
    int16_t first = float_sample(pcm, 0u, 0u);
    if (first <= 0) {
        printf("stress chunk first=%d frame0 L=%d R=%d\n", first,
               (int)float_sample(pcm, 0u, 0u),
               (int)float_sample(pcm, 0u, 1u));
        return 0;
    }
    for (uint32_t f = 0u; f < frames; f++) {
        int16_t l = float_sample(pcm, f, 0u);
        int16_t r = float_sample(pcm, f, 1u);
        if (l != first || r != first) {
            printf("stress chunk mixed at frame %u: first=%d L=%d R=%d\n",
                   f, (int)first, (int)l, (int)r);
            return 0;
        }
    }
    if (out) *out = first;
    return 1;
}

static Wc3PcmMixer *new_mixer_rate(uint32_t rate)
{
    Wc3PcmMixer *mixer = (Wc3PcmMixer *)calloc(1, sizeof *mixer);
    if (!mixer) return NULL;
    if (wc3_mixer_init(mixer, rate, 2u, 1) != 0) {
        free(mixer);
        return NULL;
    }
    return mixer;
}

static Wc3PcmMixer *new_mixer(void)
{
    return new_mixer_rate(TEST_RATE);
}

static Wc3Voice *new_voice(Wc3PcmMixer *mixer, uint8_t **pcm, int snapshot)
{
    if (!mixer || !pcm) return NULL;
    uint32_t rate = mixer->rate;
    size_t total = (size_t)TEST_BYTES * (snapshot ? WC3_VOICE_SNAPSHOTS : 1u);
    uint8_t *buffer = (uint8_t *)calloc(1, total);
    uintptr_t handle = 0;
    if (!buffer) return NULL;
    int rc = snapshot
        ? wc3_mixer_voice_add_snapshot(mixer, buffer, TEST_BYTES, rate,
                                       2u, 16u, &handle)
        : wc3_mixer_voice_add(mixer, buffer, TEST_BYTES, rate, 2u, 16u,
                              &handle);
    if (rc != 0) {
        free(buffer);
        return NULL;
    }
    *pcm = buffer;
    Wc3Voice *voice = wc3_mixer_voice(mixer, handle);
    if (!voice) {
        free(buffer);
        return NULL;
    }
    wc3_voice_control(voice, 1, 1, rate, 0, 0);
    return voice;
}

/* ---- deterministic test hooks ---------------------------------------- */

static Wc3PcmMixer *g_writer_mixer;
static Wc3Voice *g_writer_voice;
static int g_writer_hold;
static int g_writer_hold_done;
static int g_writer_hold_failed;
static int16_t g_writer_expect;

static Wc3Voice *g_reader_voice;
static int g_reader_hold;
static int g_reader_hold_done;
static int g_reader_hold_failed;
static int g_reader_publish_count;
static int16_t g_reader_version;

void wc3_mixer_test_phase(const Wc3Voice *voice, int phase)
{
    if (phase == WC3_MIXER_PHASE_APPLIED && g_writer_hold &&
        !g_writer_hold_done && voice == g_writer_voice) {
        g_writer_hold_done = 1;
        float out[TEST_CHUNK * 2u];
        uint32_t got = wc3_mixer_render(g_writer_mixer, out, TEST_CHUNK);
        if (got != TEST_CHUNK * 2u * sizeof(float) ||
            !chunk_is_version(out, TEST_CHUNK, g_writer_expect))
            g_writer_hold_failed = 1;
    }
    if (phase == WC3_MIXER_PHASE_LOADED && g_reader_hold &&
        !g_reader_hold_done && voice == g_reader_voice) {
        g_reader_hold_done = 1;
        uint8_t src[TEST_BYTES];
        for (int i = 0; i < g_reader_publish_count; i++) {
            fill_version(src, sizeof src, g_reader_version++);
            if (wc3_voice_publish(g_reader_voice, 0u, src, sizeof src) !=
                sizeof src)
                g_reader_hold_failed = 1;
        }
    }
}

/* ---- tests ----------------------------------------------------------- */

static void test_legacy_negative_control(void)
{
    Wc3PcmMixer *mixer = new_mixer();
    uint8_t *pcm = NULL;
    Wc3Voice *voice = mixer ? new_voice(mixer, &pcm, 0) : NULL;
    check(mixer != NULL && voice != NULL, "legacy voice created");
    if (!voice) return;

    uint8_t src[TEST_BYTES];
    fill_pattern(src, sizeof src);
    check(wc3_voice_publish(voice, 0u, src, sizeof src) == sizeof src,
          "legacy publish full ring");
    check(wc3_voice_sample(voice, 0u, 0u, 0) ==
              (int16_t)(src[0] | ((uint16_t)src[1] << 8)),
          "legacy stable slot returns valid PCM");

    /* Exactly the C-1 state: the writer is inside a 1 KiB slot. */
    atomic_store_explicit(&voice->seq[0], 1u, memory_order_release);
    check(wc3_voice_sample(voice, 0u, 0u, 0) == 0,
          "legacy in-flight slot becomes silence (C-1 reproduced)");
#if WC3_DEBUG
    check(atomic_load_explicit(&voice->inflight_zero,
                               memory_order_relaxed) == 1u,
          "legacy in-flight sample increments inflight_zero");
#endif
    atomic_store_explicit(&voice->seq[0], 2u, memory_order_release);
    check(wc3_voice_sample(voice, 0u, 0u, 0) != 0,
          "legacy slot recovers after the write");

    wc3_mixer_voice_remove(mixer, 1u);
    wc3_mixer_voice_release(voice);
    free(pcm);
    wc3_mixer_deinit(mixer);
    free(mixer);
}

#if WC3_DEBUG
static void test_debug_stale_counters(void)
{
    Wc3PcmMixer *mixer = new_mixer();
    uint8_t *pcm = NULL;
    Wc3Voice *voice = mixer ? new_voice(mixer, &pcm, 1) : NULL;
    check(mixer != NULL && voice != NULL, "stale-counter voice created");
    if (!voice) return;

    wc3_voice_control(voice, 1, 1, TEST_RATE, 0, 0);
    check(wc3_voice_sample_snap(voice, voice->snap_pcm[0], 0u, 0u, 0u, 1) == 0,
          "snapshot without a publication is silent");
    check(atomic_load_explicit(&voice->stale_read,
                               memory_order_relaxed) == 1u,
          "stale snapshot sample increments stale_read");
    check(atomic_load_explicit(&voice->seq_retry,
                               memory_order_relaxed) == 0u,
          "stale sample is not a sequence retry");

    check(wc3_voice_reset(voice) == 0, "counter reset succeeds");
    check(atomic_load_explicit(&voice->stale_read,
                               memory_order_relaxed) == 0u,
          "voice reset clears stale_read");
    check(atomic_load_explicit(&voice->inflight_zero,
                               memory_order_relaxed) == 0u,
          "voice reset clears inflight_zero");
    check(atomic_load_explicit(&voice->seq_retry,
                               memory_order_relaxed) == 0u,
          "voice reset clears seq_retry");

    wc3_mixer_voice_remove(mixer, 1u);
    wc3_mixer_voice_release(voice);
    free(pcm);
    wc3_mixer_deinit(mixer);
    free(mixer);
}
#endif

static void test_publish_atomicity(void)
{
    Wc3PcmMixer *mixer = new_mixer();
    uint8_t *pcm = NULL;
    Wc3Voice *voice = mixer ? new_voice(mixer, &pcm, 1) : NULL;
    check(mixer != NULL && voice != NULL, "snapshot voice created");
    if (!voice) return;
    check(voice->snap_count == WC3_VOICE_SNAPSHOTS,
          "snapshot voice has three rings");
    check(voice->pcm == voice->snap_pcm[0], "legacy pcm aliases snapshot 0");

    uint8_t v1[TEST_BYTES];
    fill_version(v1, sizeof v1, 1);
    check(wc3_voice_publish(voice, 0u, v1, sizeof v1) == sizeof v1,
          "snapshot publish v1");

    float out[TEST_CHUNK * 2u];
    check(wc3_mixer_render(mixer, out, TEST_CHUNK) ==
              TEST_CHUNK * 2u * sizeof(float),
          "snapshot render v1 size");
    check(chunk_is_version(out, TEST_CHUNK, 1), "snapshot render v1 content");

    /* Writer stops after copying active and before the region overlay.  The
     * reader must still see the complete previous version, not silence and
     * not a half-applied region. */
    wc3_voice_seek(voice, 0u);
    g_writer_mixer = mixer;
    g_writer_voice = voice;
    g_writer_expect = 1;
    g_writer_hold = 1;
    g_writer_hold_done = 0;
    g_writer_hold_failed = 0;
    uint8_t v2[TEST_BYTES / 2u];
    fill_version(v2, sizeof v2, 2);
    check(wc3_voice_publish(voice, 0u, v2, sizeof v2) == sizeof v2,
          "snapshot publish half-region v2");
    g_writer_hold = 0;
    check(g_writer_hold_done, "writer phase hook fired");
    check(!g_writer_hold_failed,
          "reader during publish sees the previous complete version");

    wc3_voice_seek(voice, 0u);
    check(wc3_mixer_render(mixer, out, TEST_CHUNK) ==
              TEST_CHUNK * 2u * sizeof(float),
          "snapshot render v2 size");
    check(chunk_is_version(out, TEST_CHUNK, 2),
          "published half-region is visible after the swap");
    check(wc3_mixer_render(mixer, out, TEST_CHUNK) ==
              TEST_CHUNK * 2u * sizeof(float),
          "snapshot render v2 second chunk");
    check(chunk_is_version(out, TEST_CHUNK, 2),
          "both slots of the multi-slot region are version 2");
    check(wc3_mixer_render(mixer, out, TEST_CHUNK) ==
              TEST_CHUNK * 2u * sizeof(float),
          "snapshot render untouched half size");
    check(chunk_is_version(out, TEST_CHUNK, 1),
          "untouched half keeps the old complete version");

    wc3_mixer_voice_remove(mixer, 1u);
    wc3_mixer_voice_release(voice);
    free(pcm);
    wc3_mixer_deinit(mixer);
    free(mixer);
}

static void test_held_snapshot_survives_three_publishes(void)
{
    Wc3PcmMixer *mixer = new_mixer();
    uint8_t *pcm = NULL;
    Wc3Voice *voice = mixer ? new_voice(mixer, &pcm, 1) : NULL;
    check(mixer != NULL && voice != NULL, "held-snapshot voice created");
    if (!voice) return;

    uint8_t src[TEST_BYTES];
    fill_version(src, sizeof src, 2);
    check(wc3_voice_publish(voice, 0u, src, sizeof src) == sizeof src,
          "held-snapshot publish v2");

    const uint8_t *held_pcm = NULL;
    uint64_t held_pub = 0u;
    int held = wc3_voice_snapshot_acquire(voice, &held_pcm, &held_pub);
    check(held >= 0 && held_pcm != NULL, "acquire held snapshot");
    check(held_pub == TEST_FRAMES, "held snapshot publishes high-water frames");
    check(atomic_load_explicit(&voice->snap_refs[held],
                               memory_order_acquire) == 1u,
          "held snapshot refcount is one");

    for (int16_t v = 3; v <= 5; v++) {
        fill_version(src, sizeof src, v);
        check(wc3_voice_publish(voice, 0u, src, sizeof src) == sizeof src,
              "publish while a snapshot is held");
    }
    check(memcmp(held_pcm, src, 4u) != 0,
          "held snapshot is not the newest bytes");
    int16_t held_version = (int16_t)(held_pcm[0] | ((uint16_t)held_pcm[1] << 8));
    check(held_version == 2, "held snapshot still contains version 2");
    check(held_pub == TEST_FRAMES, "held snapshot pub did not move");
    check(atomic_load_explicit(&voice->snap_refs[held],
                               memory_order_acquire) == 1u,
          "held snapshot refcount survived three publishes");

    wc3_voice_snapshot_release(voice, held);
    check(atomic_load_explicit(&voice->snap_refs[held],
                               memory_order_acquire) == 0u,
          "held snapshot released");

    const uint8_t *fresh_pcm = NULL;
    uint64_t fresh_pub = 0u;
    int fresh = wc3_voice_snapshot_acquire(voice, &fresh_pcm, &fresh_pub);
    check(fresh >= 0, "fresh acquire after release");
    int16_t fresh_version =
        (int16_t)(fresh_pcm[0] | ((uint16_t)fresh_pcm[1] << 8));
    check(fresh_version == 5, "fresh acquire sees the newest version");
    check(fresh_pub == TEST_FRAMES, "fresh snapshot keeps high-water");
    wc3_voice_snapshot_release(voice, fresh);

    wc3_mixer_voice_remove(mixer, 1u);
    wc3_mixer_voice_release(voice);
    free(pcm);
    wc3_mixer_deinit(mixer);
    free(mixer);
}

static void test_acquire_recheck_with_writer_inside(void)
{
    Wc3PcmMixer *mixer = new_mixer();
    uint8_t *pcm = NULL;
    Wc3Voice *voice = mixer ? new_voice(mixer, &pcm, 1) : NULL;
    check(mixer != NULL && voice != NULL, "recheck voice created");
    if (!voice) return;

    uint8_t src[TEST_BYTES];
    fill_version(src, sizeof src, 1);
    check(wc3_voice_publish(voice, 0u, src, sizeof src) == sizeof src,
          "recheck publish v1");

    /* The reader loads active, then the hook publishes v2/v3 before the ref.
     * Only two publishes are used first so the old active is definitely not
     * current: the reader must back off and retry, not read it. */
    g_reader_voice = voice;
    g_reader_hold = 1;
    g_reader_hold_done = 0;
    g_reader_hold_failed = 0;
    g_reader_publish_count = 2;
    g_reader_version = 2;
    const uint8_t *got_pcm = NULL;
    uint64_t got_pub = 0u;
    int got = wc3_voice_snapshot_acquire(voice, &got_pcm, &got_pub);
    g_reader_hold = 0;
    check(g_reader_hold_done, "reader phase hook fired");
    check(!g_reader_hold_failed, "writer inside acquire published cleanly");
    check(got >= 0, "acquire retried after active changed");
    int16_t got_version = (int16_t)(got_pcm[0] | ((uint16_t)got_pcm[1] << 8));
    check(got_version == 3, "acquire retry returned the newest complete version");
    check(got_pub == TEST_FRAMES, "retry snapshot pub is high-water");
    wc3_voice_snapshot_release(voice, got);

    /* ABA variant: three publishes can cycle back to the same buffer index.
     * The reader may observe the older triples only as complete versions;
     * here the final v6 is complete and uniform, so accepting only 4 or 6
     * still proves no torn buffer. */
    g_reader_hold = 1;
    g_reader_hold_done = 0;
    g_reader_hold_failed = 0;
    g_reader_publish_count = 3;
    g_reader_version = 4;
    got = wc3_voice_snapshot_acquire(voice, &got_pcm, &got_pub);
    g_reader_hold = 0;
    check(g_reader_hold_done, "ABA reader hook fired");
    check(!g_reader_hold_failed, "ABA writer published cleanly");
    check(got >= 0, "ABA acquire completed");
    got_version = (int16_t)(got_pcm[0] | ((uint16_t)got_pcm[1] << 8));
    check(got_version == 4 || got_version == 6,
          "ABA acquire returned a complete old or new version");
    int all_same = 1;
    for (uint32_t i = 0u; i + 4u <= TEST_BYTES; i += 4u) {
        int16_t l = (int16_t)(got_pcm[i] | ((uint16_t)got_pcm[i + 1u] << 8));
        int16_t r = (int16_t)(got_pcm[i + 2u] |
                              ((uint16_t)got_pcm[i + 3u] << 8));
        if (l != got_version || r != got_version) all_same = 0;
    }
    check(all_same, "ABA snapshot is a whole version, never torn");
    wc3_voice_snapshot_release(voice, got);

    wc3_mixer_voice_remove(mixer, 1u);
    wc3_mixer_voice_release(voice);
    free(pcm);
    wc3_mixer_deinit(mixer);
    free(mixer);
}

#if WC3_DEBUG
static void test_seq_retry_counter(void)
{
    Wc3PcmMixer *mixer = new_mixer();
    uint8_t *pcm = NULL;
    Wc3Voice *voice = mixer ? new_voice(mixer, &pcm, 1) : NULL;
    check(mixer != NULL && voice != NULL, "seq-retry voice created");
    if (!voice) return;

    uint8_t src[TEST_BYTES];
    fill_version(src, sizeof src, 1);
    check(wc3_voice_publish(voice, 0u, src, sizeof src) == sizeof src,
          "seq-retry publish v1");

    /* A single publish moves snap_active to the other free slot, so the
     * reader's recheck must fail exactly once and retry. */
    g_reader_voice = voice;
    g_reader_hold = 1;
    g_reader_hold_done = 0;
    g_reader_hold_failed = 0;
    g_reader_publish_count = 1;
    g_reader_version = 2;
    const uint8_t *p = NULL;
    uint64_t pub = 0u;
    int got = wc3_voice_snapshot_acquire(voice, &p, &pub);
    g_reader_hold = 0;
    check(g_reader_hold_done, "seq-retry hook fired");
    check(!g_reader_hold_failed, "seq-retry writer published cleanly");
    check(got >= 0 && p != NULL, "seq-retry acquire succeeded");
    check(p && (int16_t)(p[0] | ((uint16_t)p[1] << 8)) == 2,
          "seq-retry read the new version");
    check(pub == TEST_FRAMES, "seq-retry high-water is intact");
    check(atomic_load_explicit(&voice->seq_retry,
                               memory_order_relaxed) >= 1u,
          "acquire recheck increments seq_retry");
    wc3_voice_snapshot_release(voice, got);

    wc3_mixer_voice_remove(mixer, 1u);
    wc3_mixer_voice_release(voice);
    free(pcm);
    wc3_mixer_deinit(mixer);
    free(mixer);
}
#endif

static void test_no_free_snapshot_returns_nomem(void)
{
    Wc3PcmMixer *mixer = new_mixer();
    uint8_t *pcm = NULL;
    Wc3Voice *voice = mixer ? new_voice(mixer, &pcm, 1) : NULL;
    check(mixer != NULL && voice != NULL, "NOMEM voice created");
    if (!voice) return;

    uint8_t src[TEST_BYTES];
    fill_version(src, sizeof src, 1);
    check(wc3_voice_publish(voice, 0u, src, sizeof src) == sizeof src,
          "NOMEM publish v1");

    const uint8_t *p1 = NULL, *p2 = NULL;
    uint64_t pub1 = 0u, pub2 = 0u;
    int s1 = wc3_voice_snapshot_acquire(voice, &p1, &pub1);
    fill_version(src, sizeof src, 2);
    check(wc3_voice_publish(voice, 0u, src, sizeof src) == sizeof src,
          "NOMEM publish v2");
    int s2 = wc3_voice_snapshot_acquire(voice, &p2, &pub2);
    fill_version(src, sizeof src, 3);
    check(wc3_voice_publish(voice, 0u, src, sizeof src) == sizeof src,
          "NOMEM publish v3");
    check(s1 >= 0 && s2 >= 0, "two snapshots held");
    uint32_t active_before = atomic_load_explicit(&voice->snap_active,
                                                  memory_order_acquire);
    uint64_t pub_before = atomic_load_explicit(&voice->published_frame,
                                               memory_order_acquire);
    fill_version(src, sizeof src, 4);
    check(wc3_voice_publish(voice, 0u, src, sizeof src) == 0u,
          "publish with no free snapshot returns 0, never overwrites");
    check(atomic_load_explicit(&voice->snap_active, memory_order_acquire) ==
              active_before,
          "NOMEM publish did not change the active snapshot");
    check(atomic_load_explicit(&voice->published_frame, memory_order_acquire) ==
              pub_before,
          "NOMEM publish did not move published_frame");
    check(atomic_load_explicit(&voice->snap_refs[s1],
                               memory_order_acquire) == 1u &&
              atomic_load_explicit(&voice->snap_refs[s2],
                                   memory_order_acquire) == 1u,
          "NOMEM publish kept both reader refs");
    wc3_voice_snapshot_release(voice, s1);
    wc3_voice_snapshot_release(voice, s2);
    check(wc3_voice_publish(voice, 0u, src, sizeof src) == sizeof src,
          "publish succeeds after the readers release");
    wc3_voice_seek(voice, 0u);
    float out[TEST_CHUNK * 2u];
    check(wc3_mixer_render(mixer, out, TEST_CHUNK) ==
              TEST_CHUNK * 2u * sizeof(float),
          "NOMEM recovery render size");
    check(chunk_is_version(out, TEST_CHUNK, 4),
          "NOMEM recovery render sees version 4");

    wc3_mixer_voice_remove(mixer, 1u);
    wc3_mixer_voice_release(voice);
    free(pcm);
    wc3_mixer_deinit(mixer);
    free(mixer);
}

static void test_reset_zeroes_all_snapshots(void)
{
    Wc3PcmMixer *mixer = new_mixer();
    uint8_t *pcm = NULL;
    Wc3Voice *voice = mixer ? new_voice(mixer, &pcm, 1) : NULL;
    check(mixer != NULL && voice != NULL, "reset voice created");
    if (!voice) return;

    uint8_t src[TEST_BYTES];
    fill_version(src, sizeof src, 1);
    check(wc3_voice_publish(voice, 0u, src, sizeof src) == sizeof src,
          "reset publish v1");
    float out[TEST_CHUNK * 2u];
    check(wc3_mixer_render(mixer, out, TEST_CHUNK) ==
              TEST_CHUNK * 2u * sizeof(float) &&
              chunk_is_version(out, TEST_CHUNK, 1),
          "reset precondition render v1");

    check(wc3_voice_reset(voice) == 0, "voice reset succeeds");
    size_t ring = (size_t)voice->slots * WC3_VOICE_SLOT_BYTES;
    int zeroed = 1;
    for (uint32_t s = 0; s < voice->snap_count; s++) {
        for (size_t i = 0; i < ring; i++) {
            if (voice->snap_pcm[s][i] != 0u) zeroed = 0;
        }
        check(atomic_load_explicit(&voice->snap_pub[s],
                                   memory_order_acquire) == 0u,
              "reset clears every snap_pub");
    }
    check(zeroed, "reset zeroes all three full rings");
    check(atomic_load_explicit(&voice->snap_active, memory_order_acquire) == 0u,
          "reset makes snapshot 0 active");
    for (uint32_t s = 0; s < WC3_VOICE_SNAPSHOTS; s++)
        check(atomic_load_explicit(&voice->snap_refs[s],
                                   memory_order_acquire) == 0u,
              "reset leaves no snapshot refs");

    check(wc3_mixer_render(mixer, out, TEST_CHUNK) ==
              TEST_CHUNK * 2u * sizeof(float),
          "reset render size");
    int silent = 1;
    for (uint32_t f = 0u; f < TEST_CHUNK; f++) {
        if (float_sample(out, f, 0u) != 0 || float_sample(out, f, 1u) != 0)
            silent = 0;
    }
    check(silent, "reset render is silent");

    fill_version(src, sizeof src, 2);
    check(wc3_voice_publish(voice, 0u, src, sizeof src) == sizeof src,
          "publish after reset");
    wc3_voice_control(voice, 1, 1, TEST_RATE, 0, 0);
    wc3_voice_seek(voice, 0u);
    check(wc3_mixer_render(mixer, out, TEST_CHUNK) ==
              TEST_CHUNK * 2u * sizeof(float),
          "render after reset size");
    check(chunk_is_version(out, TEST_CHUNK, 2),
          "publish after reset plays the new version from frame 0");

    wc3_mixer_voice_remove(mixer, 1u);
    wc3_mixer_voice_release(voice);
    free(pcm);
    wc3_mixer_deinit(mixer);
    free(mixer);
}

static void test_remove_release_lifetime(void)
{
    Wc3PcmMixer *mixer = new_mixer();
    uint8_t *pcm = NULL;
    Wc3Voice *voice = mixer ? new_voice(mixer, &pcm, 1) : NULL;
    check(mixer != NULL && voice != NULL, "lifetime voice created");
    if (!voice) return;

    uint8_t src[TEST_BYTES];
    fill_version(src, sizeof src, 1);
    check(wc3_voice_publish(voice, 0u, src, sizeof src) == sizeof src,
          "lifetime publish v1");
    float out[TEST_CHUNK * 2u];
    check(wc3_mixer_render(mixer, out, TEST_CHUNK) ==
              TEST_CHUNK * 2u * sizeof(float),
          "lifetime precondition render");

    check(wc3_mixer_voice_remove(mixer, 1u) == 0, "voice remove succeeds");
    check(wc3_mixer_voice(mixer, 1u) == NULL,
          "removed voice is not visible to new lookups");
    check(wc3_mixer_render_users(mixer) == 0u,
          "no render users after the synchronous render returns");
    check(wc3_mixer_render(mixer, out, TEST_CHUNK) ==
              TEST_CHUNK * 2u * sizeof(float),
          "render after remove size");
    int silent = 1;
    for (uint32_t f = 0u; f < TEST_CHUNK; f++) {
        if (float_sample(out, f, 0u) != 0 || float_sample(out, f, 1u) != 0)
            silent = 0;
    }
    check(silent, "removed voice is excluded from the next render");

    wc3_mixer_voice_release(voice);
    uintptr_t handle = 0;
    check(wc3_mixer_voice_add_snapshot(mixer, pcm, TEST_BYTES, TEST_RATE,
                                       2u, 16u, &handle) == 0,
          "released slot is reusable");
    voice = wc3_mixer_voice(mixer, handle);
    check(voice != NULL, "re-added voice is live");
    if (voice) {
        wc3_voice_control(voice, 1, 1, TEST_RATE, 0, 0);
        fill_version(src, sizeof src, 3);
        check(wc3_voice_publish(voice, 0u, src, sizeof src) == sizeof src,
              "re-added voice publishes");
        check(wc3_mixer_render(mixer, out, TEST_CHUNK) ==
                  TEST_CHUNK * 2u * sizeof(float),
              "re-added voice render size");
        check(chunk_is_version(out, TEST_CHUNK, 3),
              "re-added voice plays the new PCM");
        wc3_mixer_voice_remove(mixer, handle);
        wc3_mixer_voice_release(voice);
    }

    free(pcm);
    wc3_mixer_deinit(mixer);
    free(mixer);
}

static void test_byte_identical_rate(uint32_t rate)
{
    Wc3PcmMixer *legacy = new_mixer_rate(rate);
    Wc3PcmMixer *snapshot = new_mixer_rate(rate);
    uint8_t *legacy_pcm = NULL, *snapshot_pcm = NULL;
    Wc3Voice *lv = legacy ? new_voice(legacy, &legacy_pcm, 0) : NULL;
    Wc3Voice *sv = snapshot ? new_voice(snapshot, &snapshot_pcm, 1) : NULL;
    check(legacy && snapshot && lv && sv, "comparison voices created");
    if (!(legacy && snapshot && lv && sv)) {
        if (lv) wc3_mixer_voice_release(lv);
        if (sv) wc3_mixer_voice_release(sv);
        free(legacy_pcm);
        free(snapshot_pcm);
        if (legacy) { wc3_mixer_deinit(legacy); free(legacy); }
        if (snapshot) { wc3_mixer_deinit(snapshot); free(snapshot); }
        return;
    }

    uint8_t src[TEST_BYTES];
    fill_pattern(src, sizeof src);
    check(wc3_voice_publish(lv, 0u, src, sizeof src) == sizeof src &&
              wc3_voice_publish(sv, 0u, src, sizeof src) == sizeof src,
          "comparison publish");

    float a[TEST_CHUNK * 2u];
    float b[TEST_CHUNK * 2u];
    int equal = 1;
    for (int chunk = 0; chunk < 4; chunk++) {
        uint32_t ga = wc3_mixer_render(legacy, a, TEST_CHUNK);
        uint32_t gb = wc3_mixer_render(snapshot, b, TEST_CHUNK);
        if (ga != gb || memcmp(a, b, ga) != 0) equal = 0;
    }
    check(equal, "snapshot render is byte-identical to the legacy path");

    wc3_mixer_voice_remove(legacy, 1u);
    wc3_mixer_voice_remove(snapshot, 1u);
    wc3_mixer_voice_release(lv);
    wc3_mixer_voice_release(sv);
    free(legacy_pcm);
    free(snapshot_pcm);
    wc3_mixer_deinit(legacy);
    wc3_mixer_deinit(snapshot);
    free(legacy);
    free(snapshot);
}

static void test_byte_identical_nonconcurrent_render(void)
{
    static const uint32_t rates[] = { 22050u, 44100u, 48000u };
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++)
        test_byte_identical_rate(rates[i]);
}

/* ---- threaded 10^6-frame race ---------------------------------------- */

static Wc3PcmMixer *g_stress_mixer;
static Wc3Voice *g_stress_voice;
static uint8_t *g_stress_pcm;
static _Atomic int g_stress_latest;
static _Atomic int g_stress_published;
static _Atomic int g_stress_done;
static _Atomic int g_stress_failed;

static void *stress_writer(void *arg)
{
    (void)arg;
    uint8_t src[TEST_BYTES];
    int version = 1;
    while (!atomic_load_explicit(&g_stress_done, memory_order_acquire)) {
        fill_version(src, sizeof src, (int16_t)version);
        for (;;) {
            uint32_t got = wc3_voice_publish(g_stress_voice, 0u, src,
                                             sizeof src);
            if (got == sizeof src) break;
            if (atomic_load_explicit(&g_stress_failed,
                                     memory_order_relaxed))
                return NULL;
            if (atomic_load_explicit(&g_stress_done, memory_order_acquire))
                return NULL;
            sched_yield();
        }
        atomic_store_explicit(&g_stress_latest, version,
                              memory_order_release);
        atomic_fetch_add_explicit(&g_stress_published, 1,
                                  memory_order_relaxed);
        if (++version >= STRESS_MAX_VERSION) version = 1;
    }
    return NULL;
}

static void test_threaded_stress(void)
{
    g_stress_mixer = new_mixer();
    g_stress_pcm = NULL;
    g_stress_voice = g_stress_mixer
        ? new_voice(g_stress_mixer, &g_stress_pcm, 1) : NULL;
    check(g_stress_mixer != NULL && g_stress_voice != NULL,
          "stress voice created");
    if (!g_stress_voice) return;
    atomic_store_explicit(&g_stress_latest, 0, memory_order_relaxed);
    atomic_store_explicit(&g_stress_published, 0, memory_order_relaxed);
    atomic_store_explicit(&g_stress_done, 0, memory_order_relaxed);
    atomic_store_explicit(&g_stress_failed, 0, memory_order_relaxed);

    pthread_t writer;
    check(pthread_create(&writer, NULL, stress_writer, NULL) == 0,
          "stress writer started");

    while (atomic_load_explicit(&g_stress_latest,
                                memory_order_acquire) == 0)
        sched_yield();

    float out[TEST_CHUNK * 2u];
    int uniform = 1;
    int no_zero = 1;
    int16_t last_version = 0;
    int published_seen = atomic_load_explicit(&g_stress_published,
                                              memory_order_acquire);
    for (uint32_t chunk = 0u; chunk < STRESS_CHUNKS; chunk++) {
        /* Keep at least one publish between chunks: the stale window is one
         * ring revolution, and a descheduled writer must not turn a valid
         * chunk into the nfsu2 stale mute that this test is not measuring. */
        while (atomic_load_explicit(&g_stress_published,
                                    memory_order_acquire) == published_seen)
            sched_yield();
        published_seen = atomic_load_explicit(&g_stress_published,
                                              memory_order_acquire);
        uint32_t got = wc3_mixer_render(g_stress_mixer, out, TEST_CHUNK);
        if (got != TEST_CHUNK * 2u * sizeof(float)) {
            uniform = 0;
            break;
        }
        int16_t version = 0;
        if (!chunk_is_uniform(out, TEST_CHUNK, &version)) {
            printf("stress state chunk=%u active=%u refs=%u/%u/%u "
                   "pubs=%llu/%llu/%llu "
                   "published=%llu playing=%d loop=%d position=%.2f "
                   "stale=%u latest=%d done=%d count=%d\n",
                   chunk,
                   atomic_load_explicit(&g_stress_voice->snap_active,
                                        memory_order_acquire),
                   atomic_load_explicit(&g_stress_voice->snap_refs[0],
                                        memory_order_acquire),
                   atomic_load_explicit(&g_stress_voice->snap_refs[1],
                                        memory_order_acquire),
                   atomic_load_explicit(&g_stress_voice->snap_refs[2],
                                        memory_order_acquire),
                   (unsigned long long)atomic_load_explicit(
                       &g_stress_voice->snap_pub[0], memory_order_acquire),
                   (unsigned long long)atomic_load_explicit(
                       &g_stress_voice->snap_pub[1], memory_order_acquire),
                   (unsigned long long)atomic_load_explicit(
                       &g_stress_voice->snap_pub[2], memory_order_acquire),
                   (unsigned long long)atomic_load_explicit(
                       &g_stress_voice->published_frame, memory_order_acquire),
                   (int)atomic_load_explicit(&g_stress_voice->playing,
                                             memory_order_acquire),
                   (int)atomic_load_explicit(&g_stress_voice->loop,
                                             memory_order_acquire),
                   g_stress_voice->position,
                   atomic_load_explicit(&g_stress_voice->stale_frames,
                                        memory_order_relaxed),
                   atomic_load_explicit(&g_stress_latest,
                                        memory_order_acquire),
                   atomic_load_explicit(&g_stress_done,
                                        memory_order_acquire),
                   atomic_load_explicit(&g_stress_published,
                                        memory_order_relaxed));
            uniform = 0;
            break;
        }
        if (version <= 0) no_zero = 0;
        last_version = version;
    }
    atomic_store_explicit(&g_stress_done, 1, memory_order_release);
    check(uniform, "1M-frame stress: every chunk is one complete version");
    check(no_zero, "1M-frame stress: no silent valid chunk");
    check(last_version > 0, "1M-frame stress rendered published PCM");

    (void)pthread_join(writer, NULL);
    check(!atomic_load_explicit(&g_stress_failed, memory_order_relaxed),
          "stress writer completed without failure");
    check(atomic_load_explicit(&g_stress_published, memory_order_relaxed) > 0,
          "stress writer published throughout the render");
    check(atomic_load_explicit(&g_stress_latest, memory_order_relaxed) > 0,
          "stress writer reached a published version");
    for (uint32_t i = 0u; i < WC3_VOICE_SNAPSHOTS; i++)
        check(atomic_load_explicit(&g_stress_voice->snap_refs[i],
                                   memory_order_acquire) == 0u,
              "stress left no snapshot refs");

    wc3_mixer_voice_remove(g_stress_mixer, 1u);
    wc3_mixer_voice_release(g_stress_voice);
    free(g_stress_pcm);
    wc3_mixer_deinit(g_stress_mixer);
    free(g_stress_mixer);
    g_stress_voice = NULL;
    g_stress_pcm = NULL;
    g_stress_mixer = NULL;
}

int main(void)
{
    test_legacy_negative_control();
#if WC3_DEBUG
    test_debug_stale_counters();
    test_seq_retry_counter();
#endif
    test_publish_atomicity();
    test_held_snapshot_survives_three_publishes();
    test_acquire_recheck_with_writer_inside();
    test_no_free_snapshot_returns_nomem();
    test_reset_zeroes_all_snapshots();
    test_remove_release_lifetime();
    test_byte_identical_nonconcurrent_render();
    test_threaded_stress();

    int failures = atomic_load_explicit(&g_failures, memory_order_relaxed);
    if (failures) {
        printf("audio_snapshot: %d failure(s)\n", failures);
        return 1;
    }
    printf("audio_snapshot: C-1 reproduced on legacy path; triple snapshot "
           "atomicity, refs, NOMEM, byte parity and 1M-frame race passed\n");
    return 0;
}
