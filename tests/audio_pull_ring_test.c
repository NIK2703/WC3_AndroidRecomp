/*
 * audio_pull_ring_test.c - T0 guard for the S-1 host-owned pull ring.
 *
 * Spec: docs/AUDIO_PULL_IMPLEMENTATION_SPEC.md (I-2, I-3, I-7, I-8).
 * The header under test is platform-neutral: it contains no OS headers, no
 * locks and no allocation, so this runs on the host C compiler.
 */
#include "audio_pull_ring.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES_PER_SLOT 256u   /* 1024 bytes / 4 bytes per stereo frame */
#define STRESS_ROUNDS   1000u
#define STRESS_FRAMES   64u

static _Atomic int g_failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL %s\n", what);
        atomic_fetch_add_explicit(&g_failures, 1, memory_order_relaxed);
    }
}

static Wc3PullRing g_ring;
static uint8_t g_pcm[WC3_PULL_RING_BYTES];

static void fill(uint8_t *dst, size_t bytes, unsigned seed)
{
    for (size_t i = 0; i < bytes; i++)
        dst[i] = (uint8_t)(seed * 31u + i * 7u);
}

static void *stress_producer(void *arg)
{
    (void)arg;
    uint8_t src[STRESS_FRAMES * 4u];
    for (unsigned i = 0; i < STRESS_ROUNDS; i++) {
        fill(src, sizeof src, i);
        check(wc3_pull_publish(&g_ring, (uint64_t)i * STRESS_FRAMES,
                               src, sizeof src) == sizeof src,
              "stress publish accepts whole region");
    }
    return NULL;
}

static void *stress_consumer(void *arg)
{
    (void)arg;
    uint8_t out[STRESS_FRAMES * 4u];
    for (unsigned i = 0; i < STRESS_ROUNDS; i++)
        check(wc3_pull_render(&g_ring, out, STRESS_FRAMES) ==
                  STRESS_FRAMES * 4u,
              "stress render writes full burst");
    return NULL;
}

int main(void)
{
    check(WC3_PULL_RING_BYTES == 65536u, "ring is 64 KiB");
    check(WC3_PULL_SLOTS == 64u, "64 slots");
    check(wc3_pull_stable_seq(0u) == 2u, "generation zero is even");
    check((wc3_pull_writing_seq(0u) & 1u) == 1u, "writing sequence is odd");

    check(wc3_pull_ring_init(&g_ring, g_pcm, 4u) == 0, "ring init");
    check(g_ring.frames_per_slot == FRAMES_PER_SLOT,
          "256 stereo frames per slot");
    check(wc3_pull_ring_init(&g_ring, g_pcm, 3u) == -1,
          "non-tiling format rejected");
    check(wc3_pull_ring_init(&g_ring, g_pcm, 4u) == 0, "ring re-init");

    /* One full slot, byte-exact. */
    uint8_t src[WC3_PULL_SLOT_BYTES];
    uint8_t out[WC3_PULL_SLOT_BYTES];
    fill(src, sizeof src, 1u);
    check(wc3_pull_publish(&g_ring, 0u, src, sizeof src) == sizeof src,
          "publish full slot");
    check(wc3_pull_render(&g_ring, out, FRAMES_PER_SLOT) == sizeof out,
          "render full slot");
    check(memcmp(src, out, sizeof src) == 0, "full slot PCM is byte-exact");
    check(atomic_load_explicit(&g_ring.stale_frames,
                               memory_order_relaxed) == 0u,
          "no stale frames in a fully published slot");
    check(atomic_load_explicit(&g_ring.consumed,
                               memory_order_acquire) == sizeof src,
          "device clock tracks consumed bytes");

    /* Region straddling a slot boundary, published exactly at the play
     * cursor: frames 256..755 span slot 1 and part of slot 2. */
    uint64_t base = atomic_load_explicit(&g_ring.play_frame,
                                         memory_order_acquire);
    uint8_t wide[500u * 4u];
    uint8_t wide_out[500u * 4u];
    fill(wide, sizeof wide, 2u);
    check(wc3_pull_publish(&g_ring, base, wide, sizeof wide) == sizeof wide,
          "publish straddling region");
    check(wc3_pull_render(&g_ring, wide_out, 500u) == sizeof wide_out,
          "render straddling region");
    check(memcmp(wide, wide_out, sizeof wide) == 0,
          "straddling region PCM is byte-exact");
    check(atomic_load_explicit(&g_ring.publish_regions,
                               memory_order_relaxed) == 2u,
          "publish regions counted");

    /* Publication starting part-way into a slot. */
    uint64_t part_base = atomic_load_explicit(&g_ring.play_frame,
                                              memory_order_acquire) + 12u;
    uint8_t part_src[100u * 4u];
    uint8_t part_out[100u * 4u];
    fill(part_src, sizeof part_src, 3u);
    check(wc3_pull_publish(&g_ring, part_base, part_src,
                           sizeof part_src) == sizeof part_src,
          "publish with non-zero slot offset");
    atomic_store_explicit(&g_ring.play_frame, part_base,
                          memory_order_release);
    check(wc3_pull_render(&g_ring, part_out, 100u) == sizeof part_out,
          "render non-zero slot offset");
    check(memcmp(part_src, part_out, sizeof part_src) == 0,
          "offset region PCM is byte-exact");

    /* Underrun: unwritten frames are silence but the device clock advances. */
    uint8_t silence[16u * 4u];
    uint32_t stale_before = atomic_load_explicit(&g_ring.stale_frames,
                                                  memory_order_relaxed);
    uint64_t clock_before = atomic_load_explicit(&g_ring.consumed,
                                                 memory_order_acquire);
    uint64_t play_before = atomic_load_explicit(&g_ring.play_frame,
                                                memory_order_acquire);
    check(wc3_pull_render(&g_ring, silence, 16u) == sizeof silence,
          "underrun render size");
    int all_zero = 1;
    for (size_t i = 0; i < sizeof silence; i++)
        if (silence[i] != 0) all_zero = 0;
    check(all_zero, "stale slot produces silence, never old PCM");
    check(atomic_load_explicit(&g_ring.stale_frames,
                               memory_order_relaxed) > stale_before,
          "stale frames counted");
    check(atomic_load_explicit(&g_ring.consumed,
                               memory_order_acquire) ==
              clock_before + sizeof silence,
          "device clock advances through underrun");
    check(atomic_load_explicit(&g_ring.play_frame,
                               memory_order_acquire) == play_before + 16u,
          "play cursor advances through underrun");

    /* A slot explicitly left in the writing state must not be read. */
    uint64_t next_frame = atomic_load_explicit(&g_ring.play_frame,
                                               memory_order_acquire);
    uint64_t next_gen = next_frame / FRAMES_PER_SLOT;
    uint32_t next_slot = (uint32_t)(next_gen % WC3_PULL_SLOTS);
    atomic_store_explicit(&g_ring.slot_seq[next_slot],
                          wc3_pull_writing_seq(next_gen),
                          memory_order_release);
    uint8_t odd_out[4u];
    uint32_t odd_stale = atomic_load_explicit(&g_ring.stale_frames,
                                              memory_order_relaxed);
    (void)wc3_pull_render(&g_ring, odd_out, 1u);
    check(atomic_load_explicit(&g_ring.stale_frames,
                               memory_order_relaxed) > odd_stale,
          "odd seqlock is treated as stale");

    /* Wrap: a later lap of the same slot has a strictly newer generation. */
    uint8_t wrap_src[4u] = {0x11u, 0x22u, 0x33u, 0x44u};
    uint64_t wrap_frame = (uint64_t)WC3_PULL_SLOTS * FRAMES_PER_SLOT;
    uint32_t before = atomic_load_explicit(&g_ring.slot_seq[0],
                                           memory_order_acquire);
    check(wc3_pull_publish(&g_ring, wrap_frame, wrap_src,
                           sizeof wrap_src) == sizeof wrap_src,
          "publish wrapped slot");
    check(atomic_load_explicit(&g_ring.slot_seq[0],
                               memory_order_acquire) > before,
          "wrapped slot generation is monotone");

    /* FNV-1a determinism. */
    uint8_t a[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    uint8_t b[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 9u};
    check(wc3_pull_hash(WC3_PULL_HASH_INIT, a, sizeof a) ==
              wc3_pull_hash(WC3_PULL_HASH_INIT, a, sizeof a),
          "PCM hash is deterministic");
    check(wc3_pull_hash(WC3_PULL_HASH_INIT, a, sizeof a) !=
              wc3_pull_hash(WC3_PULL_HASH_INIT, b, sizeof b),
          "PCM hash detects a one-byte change");

    /* Two threads: publish and render concurrently (TA-4). */
    check(wc3_pull_ring_init(&g_ring, g_pcm, 4u) == 0, "stress ring init");
    pthread_t producer, consumer;
    check(pthread_create(&producer, NULL, stress_producer, NULL) == 0,
          "stress producer start");
    check(pthread_create(&consumer, NULL, stress_consumer, NULL) == 0,
          "stress consumer start");
    (void)pthread_join(producer, NULL);
    (void)pthread_join(consumer, NULL);
    check(atomic_load_explicit(&g_ring.publish_regions,
                               memory_order_relaxed) == STRESS_ROUNDS,
          "stress publishes counted");
    check(atomic_load_explicit(&g_ring.render_calls,
                               memory_order_relaxed) == STRESS_ROUNDS,
          "stress renders counted");
    check(atomic_load_explicit(&g_ring.play_frame,
                               memory_order_acquire) ==
              (uint64_t)STRESS_ROUNDS * STRESS_FRAMES,
          "stress play cursor is monotone and exact");
    check(atomic_load_explicit(&g_ring.consumed,
                               memory_order_acquire) ==
              (uint64_t)STRESS_ROUNDS * STRESS_FRAMES * 4u,
          "stress device clock is monotone and exact");

    int failures = atomic_load_explicit(&g_failures, memory_order_relaxed);
    if (failures) {
        printf("audio_pull_ring: %d failure(s)\n", failures);
        return 1;
    }
    printf("audio_pull_ring: publish/render, wrap, seqlock, stale silence, "
           "device clock and 2-thread stress passed\n");
    return 0;
}
