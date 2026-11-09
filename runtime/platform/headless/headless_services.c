/*
 * headless_services.c - the non-privileged half of the headless platform.
 *
 * Window, input, audio, clipboard, surface, log, config, assets, locale and
 * Vulkan are deliberately small.  The verification DUT has no display and no
 * sound device: the window is a size record, audio writes succeed and are
 * discarded, and the surface hands out a stable fake native handle.  Input is
 * a lock-free SPSC mailbox so a replay schedule can feed the guest thread.
 */
#include "audio_pull_ring.h"
#include "audio_mixer.h"
#include "headless_internal.h"

#include <dirent.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

HeadlessState *g_headless;

/* ------------------------------------------------------------------ helpers */

int headless_copy_string(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0u) return -1;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= cap) return -1;
    memcpy(dst, src, n + 1u);
    return 0;
}

int headless_regular_file(const char *path)
{
    struct stat st;
    if (!path) return 0;
    if (stat(path, &st) != 0) return 0;
    if ((st.st_mode & S_IFREG) == 0) return 0;
    return access(path, R_OK) == 0 ? 1 : 0;
}

static int ascii_lower(int ch)
{
    return (ch >= 'A' && ch <= 'Z') ? ch + ('a' - 'A') : ch;
}

static int name_equal_ci(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int is_mpq_name(const char *name)
{
    size_t n = strlen(name);
    return n > 4u && name_equal_ci(name + n - 4u, ".mpq");
}

static int mpq_name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int headless_asset_roots(HeadlessState *st, const char *asset_root,
                         const char *save_root)
{
    if (!st) return -1;
    if (asset_root) {
        if (headless_copy_string(st->asset_root, sizeof st->asset_root,
                                 asset_root) != 0)
            return -1;
    } else {
        st->asset_root[0] = '\0';
    }
    if (save_root) {
        if (headless_copy_string(st->save_root, sizeof st->save_root,
                                 save_root) != 0)
            return -1;
    } else {
        st->save_root[0] = '\0';
    }

    st->mpq_count = 0u;
    if (st->asset_root[0] == '\0') return 0;

    DIR *dir = opendir(st->asset_root);
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && st->mpq_count < HEADLESS_MPQ_MAX) {
        if (!is_mpq_name(ent->d_name)) continue;
        size_t n = strlen(ent->d_name);
        if (n >= sizeof st->mpq[0]) continue;
        memcpy(st->mpq[st->mpq_count], ent->d_name, n + 1u);
        st->mpq_count++;
    }
    (void)closedir(dir);
    if (st->mpq_count > 1u)
        qsort(st->mpq, st->mpq_count, sizeof st->mpq[0], mpq_name_cmp);
    return 0;
}

static int config_key_seen(const HeadlessState *st, const char *key, size_t len)
{
    for (uint32_t i = 0; i < st->config_count; i++) {
        const char *entry = st->config[i];
        if (strncmp(entry, key, len) == 0 && entry[len] == '=') return 1;
    }
    return 0;
}

int headless_config_init(HeadlessState *st,
                         const Wc3HeadlessConfigEntry *entries,
                         uint32_t count)
{
    if (!st) return -1;
    if (count > HEADLESS_CONFIG_MAX) return -1;
    st->config = (char **)calloc(count ? count : 1u, sizeof(char *));
    if (!st->config) return -1;

    for (uint32_t i = 0; i < count; i++) {
        const char *key = entries[i].key;
        const char *value = entries[i].value;
        if (!key || !*key || strchr(key, '=')) break;
        if (!value) value = "";
        size_t klen = strlen(key);
        size_t vlen = strlen(value);
        if (klen > WC3_HEADLESS_PATH_MAX || vlen > WC3_HEADLESS_PATH_MAX)
            break;
        if (config_key_seen(st, key, klen)) break;

        char *copy = (char *)malloc(klen + vlen + 2u);
        if (!copy) break;
        memcpy(copy, key, klen);
        copy[klen] = '=';
        memcpy(copy + klen + 1u, value, vlen + 1u);
        st->config[st->config_count++] = copy;
    }

    if (st->config_count != count) {
        headless_config_free(st);
        return -1;
    }
    return 0;
}

void headless_config_free(HeadlessState *st)
{
    if (!st || !st->config) return;
    for (uint32_t i = 0; i < st->config_count; i++) free(st->config[i]);
    free(st->config);
    st->config = NULL;
    st->config_count = 0u;
}

/* ------------------------------------------------------------------- input */

int headless_input_kind_valid(uint32_t kind)
{
    return kind >= WC3_INPUT_POINTER_DOWN && kind <= WC3_INPUT_FOCUS;
}

void headless_input_reset(HeadlessState *st)
{
    if (!st) return;
    (void)pthread_mutex_lock(&st->input_push_lock);
    atomic_store_explicit(&st->input_head, 0u, memory_order_relaxed);
    atomic_store_explicit(&st->input_tail, 0u, memory_order_relaxed);
    atomic_store_explicit(&st->input_dropped, 0u, memory_order_relaxed);
    atomic_store_explicit(&st->input_focus, 0, memory_order_relaxed);
    st->script_next = 0u;
    (void)pthread_mutex_unlock(&st->input_push_lock);
}

int headless_input_ring_push(HeadlessState *st, const Wc3InputEvent *event)
{
    if (!st || !event || !headless_input_kind_valid(event->kind)) return -1;

    (void)pthread_mutex_lock(&st->input_push_lock);
    uint32_t head = atomic_load_explicit(&st->input_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&st->input_tail, memory_order_acquire);
    if (head - tail >= HEADLESS_INPUT_RING) {
        (void)pthread_mutex_unlock(&st->input_push_lock);
        return -1;
    }
    st->input[head & HEADLESS_INPUT_MASK] = *event;
    atomic_store_explicit(&st->input_head, head + 1u, memory_order_release);
    (void)pthread_mutex_unlock(&st->input_push_lock);
    return 0;
}

static int headless_input_push(void *ctx, const Wc3InputEvent *event)
{
    HeadlessState *st = (HeadlessState *)ctx;
    if (!st || !event || !headless_input_kind_valid(event->kind)) return -1;

    Wc3InputEvent copy = *event;
    if (copy.time_ns == 0u) copy.time_ns = headless_now_ns(st);
    if (headless_input_ring_push(st, &copy) != 0) {
        atomic_fetch_add_explicit(&st->input_dropped, 1u,
                                  memory_order_relaxed);
        return -1;
    }
    return 0;
}

void headless_input_publish_due(HeadlessState *st)
{
    if (!st || st->script_count == 0u) return;

    const uint64_t now = headless_now_ns(st);
    (void)pthread_mutex_lock(&st->script_lock);
    while (st->script_next < st->script_count &&
           st->script[st->script_next].publish_ns <= now) {
        if (headless_input_ring_push(
                st, &st->script[st->script_next].event.event) != 0)
            break; /* ring full: retry on the next poll/advance */
        st->script_next++;
    }
    (void)pthread_mutex_unlock(&st->script_lock);
}

int wc3_headless_input_pop(Wc3InputEvent *event)
{
    HeadlessState *st = g_headless;
    if (!st || !event) return -1;

    headless_input_publish_due(st);
    uint32_t tail = atomic_load_explicit(&st->input_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&st->input_head, memory_order_acquire);
    if (tail == head) return 0;

    *event = st->input[tail & HEADLESS_INPUT_MASK];
    atomic_store_explicit(&st->input_tail, tail + 1u, memory_order_release);
    return 1;
}

uint32_t wc3_headless_input_dropped(void)
{
    HeadlessState *st = g_headless;
    if (!st) return 0u;
    return atomic_load_explicit(&st->input_dropped, memory_order_relaxed);
}

static int headless_input_focus(void *ctx, int focused)
{
    HeadlessState *st = (HeadlessState *)ctx;
    if (!st) return -1;
    atomic_store_explicit(&st->input_focus, focused != 0, memory_order_release);
    return 0;
}

/* ------------------------------------------------------------------ window */

static int headless_window_create(void *ctx, uint32_t width, uint32_t height,
                                  const char *title, uintptr_t *out)
{
    HeadlessState *st = (HeadlessState *)ctx;
    if (!st || !out || width == 0u || height == 0u) return -1;
    st->width = width;
    st->height = height;
    (void)headless_copy_string(st->window_title, sizeof st->window_title,
                               title ? title : "wc3-headless");
    *out = (uintptr_t)st;
    return 0;
}

static void headless_window_destroy(void *ctx, uintptr_t window)
{
    (void)ctx;
    (void)window;
}

static int headless_window_size(void *ctx, uintptr_t window, uint32_t *width,
                                uint32_t *height)
{
    HeadlessState *st = (HeadlessState *)ctx;
    if (!st || window != (uintptr_t)st || !width || !height) return -1;
    *width = st->width;
    *height = st->height;
    return 0;
}

static int headless_window_set_title(void *ctx, uintptr_t window,
                                     const char *title)
{
    (void)window;
    HeadlessState *st = (HeadlessState *)ctx;
    if (!st) return -1;
    return headless_copy_string(st->window_title, sizeof st->window_title,
                                title ? title : "");
}

/* ------------------------------------------------------------------- audio */

/* A-12.4 S1: T1 parity.  The headless DUT has no hardware clock, so it keeps
 * an explicit virtual device clock: write() only queues bytes, advance() (a
 * test step) consumes them and fires the same on_consumed() pump as Android.
 * A difftest can therefore drive WOM_DONE deterministically. */
typedef struct HeadlessAudio {
    uint32_t rate;
    uint32_t channels;
    uint32_t bits;
    _Atomic uint64_t written;
    _Atomic uint64_t consumed;
    void (*on_consumed)(void *opaque, uint64_t total);
    void *consumed_opaque;
    pthread_mutex_t lock;

    /* S-1: deterministic pull twin.  pull_mode == 0 keeps the exact legacy
     * linear advance() contract so existing T0/T1 tests stay byte-identical. */
    Wc3PullRing pull;
    uint8_t *pull_pcm;
    int pull_mode;
    uint32_t burst_frames;
    uint64_t pcm_hash;

    /* Deterministic device clock for health snapshots.  Android uses real
     * monotonic time; the headless DUT must not make T1 replays wall-clock
     * dependent, so advance() moves this clock by the burst duration. */
    uint64_t virtual_ns;

    /* Full nfsu2 port: deterministic twin of the Android voice mixer.  Off by
     * default so the existing T0/T1 replays stay byte-identical; a T1 difftest
     * turns it on through wc3_headless_audio_enable_mixer(). */
    Wc3PcmMixer mixer;
    uint8_t *voice_pcm[WC3_MIXER_VOICES];
    uint64_t voice_last_publish_ns[WC3_MIXER_VOICES];
    uint64_t voice_max_gap_ns[WC3_MIXER_VOICES];
    uintptr_t primary_voice;
    int mixer_mode;
} HeadlessAudio;

/* Last opened stream, used only by the T1 verification setters below.  The
 * headless DUT owns one audio device per process; no production code reads
 * this pointer. */
static HeadlessAudio *g_headless_audio_last;

/* Set by wc3_headless_audio_enable_mixer() BEFORE waveOutOpen().  The stream
 * is created by open(), so a per-stream switch alone would be lost: the T1
 * replay sets this global, and the next open() applies it to the new stream. */
static int g_headless_mixer_default;

static void headless_audio_wait_render_idle(Wc3PcmMixer *mixer)
{
    const Wc3Platform *p = wc3_platform();
    while (wc3_mixer_render_users(mixer) != 0u) {
        if (p && p->thread.yield)
            p->thread.yield(p->thread.ctx);
        else
            sched_yield();
    }
}

static int headless_audio_open(void *ctx, uint32_t sample_rate,
                               uint32_t channels, uint32_t bits,
                               uintptr_t *out)
{
    (void)ctx;
    if (!out || sample_rate == 0u || channels == 0u || bits == 0u) return -1;
    HeadlessAudio *stream = (HeadlessAudio *)calloc(1, sizeof *stream);
    if (!stream) return -1;
    stream->rate = sample_rate;
    stream->channels = channels;
    stream->bits = bits;
    atomic_init(&stream->written, 0);
    atomic_init(&stream->consumed, 0);
    stream->pcm_hash = WC3_PULL_HASH_INIT;
    stream->burst_frames = 1u;
    if (pthread_mutex_init(&stream->lock, NULL) != 0) {
        free(stream);
        return -1;
    }
    /* The pull ring only accepts a format that tiles a 1 KiB slot.  Other
     * bit depths keep the legacy linear path; publish() then returns -1. */
    uint32_t bpf = channels * (bits / 8u);
    if (bpf != 0u && (WC3_PULL_SLOT_BYTES % bpf) == 0u) {
        stream->pull_pcm = (uint8_t *)calloc(1, WC3_PULL_RING_BYTES);
        if (stream->pull_pcm &&
            wc3_pull_ring_init(&stream->pull, stream->pull_pcm, bpf) == 0)
            stream->burst_frames = WC3_PULL_SLOT_BYTES / bpf;
        else {
            free(stream->pull_pcm);
            stream->pull_pcm = NULL;
        }
    }
    /* Full nfsu2 port: deterministic twin of the Android voice mixer.  It is
     * inert until a T1 replay calls wc3_headless_audio_enable_mixer(), so the
     * legacy linear and S-1 pull replays stay byte-identical. */
    uint32_t vbpf = channels * (bits / 8u);
    if (vbpf && (WC3_VOICE_SLOT_BYTES % vbpf) == 0u &&
        wc3_mixer_init(&stream->mixer, sample_rate, channels, 0) == 0) {
        uint8_t *pcm = (uint8_t *)calloc(1, WC3_PULL_RING_BYTES);
        uintptr_t local = 0;
        if (pcm && wc3_mixer_voice_add(&stream->mixer, pcm,
                                       WC3_PULL_RING_BYTES, sample_rate,
                                       channels, bits, &local) == 0) {
            stream->voice_pcm[(size_t)local - 1u] = pcm;
            stream->primary_voice = (1u << 16) | local;
        } else {
            free(pcm);
        }
    }
    /* Apply the replay-level switch before WINMM asks for primary_voice(). */
    stream->mixer_mode = (g_headless_mixer_default && stream->primary_voice)
        ? 1 : 0;
    *out = (uintptr_t)stream;
    g_headless_audio_last = stream;
    return 0;
}

static int headless_audio_write(void *ctx, uintptr_t handle, const void *frames,
                                uint32_t bytes, uint32_t *written)
{
    (void)ctx;
    HeadlessAudio *stream = (HeadlessAudio *)(uintptr_t)handle;
    if (!stream || (!frames && bytes != 0u)) return -1;
    atomic_fetch_add_explicit(&stream->written, bytes, memory_order_relaxed);
    if (written) *written = bytes;
    return 0;
}

static Wc3Voice *headless_voice(uintptr_t voice, uintptr_t *out_local);

/* S-1: the producer publishes ONLY the region it wrote, at an absolute frame
 * index.  A successful publish switches this stream to pull semantics, so
 * advance() renders the same PCM the Android callback would render. */
static int headless_audio_publish(void *ctx, uintptr_t handle,
                                  uint64_t frame_base, const void *frames,
                                  uint32_t bytes, uint32_t *written)
{
    (void)ctx;
    HeadlessAudio *stream = (HeadlessAudio *)(uintptr_t)handle;
    if (!stream || !frames || bytes == 0u) return -1;
    if (stream->mixer_mode) {
        Wc3Voice *v = headless_voice(stream->primary_voice, NULL);
        if (!v) return -1;
        uint32_t got = wc3_voice_publish(v, frame_base, frames, bytes);
        if (!got) return -1;
        if (written) *written = got;
        return 0;
    }
    if (!stream->pull_pcm) return -1;
    uint32_t got = wc3_pull_publish(&stream->pull, frame_base, frames, bytes);
    if (got == 0u) return -1;
    stream->pull_mode = 1;
    if (written) *written = got;
    return 0;
}

/* ---- Full nfsu2 port: deterministic voice mixer twin ---------------- */
static Wc3Voice *headless_voice(uintptr_t voice, uintptr_t *out_local)
{
    uintptr_t local = voice & 0xFFFFu;
    HeadlessAudio *stream = g_headless_audio_last;
    if (!stream || !local) return NULL;
    Wc3Voice *v = wc3_mixer_voice(&stream->mixer, local);
    if (!v) return NULL;
    if (out_local) *out_local = local;
    return v;
}

static int headless_audio_voice_add(void *ctx, uintptr_t handle,
                                    uint32_t rate, uint32_t channels,
                                    uint32_t bits, uint32_t bytes,
                                    uintptr_t *out_voice)
{
    (void)ctx;
    HeadlessAudio *stream = (HeadlessAudio *)(uintptr_t)handle;
    if (!stream) stream = g_headless_audio_last;
    if (!stream || !out_voice) return -1;
    *out_voice = 0;
    uint32_t slots = (bytes + WC3_VOICE_SLOT_BYTES - 1u) /
                     WC3_VOICE_SLOT_BYTES;
    if (slots == 0u || slots > WC3_VOICE_MAX_SLOTS) return -1;
    uint8_t *pcm = (uint8_t *)malloc((size_t)slots * WC3_VOICE_SLOT_BYTES);
    if (!pcm) return -1;
    uintptr_t local = 0;
    if (wc3_mixer_voice_add(&stream->mixer, pcm, bytes, rate, channels,
                            bits, &local) != 0) {
        free(pcm);
        return -1;
    }
    stream->voice_pcm[(size_t)local - 1u] = pcm;
    stream->voice_last_publish_ns[(size_t)local - 1u] = 0u;
    stream->voice_max_gap_ns[(size_t)local - 1u] = 0u;
    *out_voice = (1u << 16) | local;
    return 0;
}

static int headless_audio_voice_remove(void *ctx, uintptr_t voice)
{
    (void)ctx;
    uintptr_t local = 0;
    Wc3Voice *v = headless_voice(voice, &local);
    if (!v) return -1;
    HeadlessAudio *stream = g_headless_audio_last;
    if (wc3_mixer_voice_remove(&stream->mixer, local) != 0) return -1;
    headless_audio_wait_render_idle(&stream->mixer);
    while (atomic_load_explicit(&v->publishing, memory_order_acquire) != 0)
        sched_yield();
    while (atomic_load_explicit(&v->resetting, memory_order_acquire) != 0)
        sched_yield();
    wc3_mixer_voice_release(v);
    free(stream->voice_pcm[(size_t)local - 1u]);
    stream->voice_pcm[(size_t)local - 1u] = NULL;
    stream->voice_last_publish_ns[(size_t)local - 1u] = 0u;
    stream->voice_max_gap_ns[(size_t)local - 1u] = 0u;
    return 0;
}

static int headless_audio_voice_publish(void *ctx, uintptr_t voice,
                                        uint64_t frame_base,
                                        const void *frames, uint32_t bytes,
                                        uint32_t *written)
{
    (void)ctx;
    uintptr_t local = 0;
    Wc3Voice *v = headless_voice(voice, &local);
    if (written) *written = 0;
    if (!v || !frames) return -1;
    uint32_t got = wc3_voice_publish(v, frame_base, frames, bytes);
    if (!got) return -1;
    HeadlessAudio *stream = g_headless_audio_last;
    if (stream) {
        uint64_t now = stream->virtual_ns;
        uint64_t last = stream->voice_last_publish_ns[(size_t)local - 1u];
        if (last && now > last) {
            uint64_t gap = now - last;
            if (gap > stream->voice_max_gap_ns[(size_t)local - 1u])
                stream->voice_max_gap_ns[(size_t)local - 1u] = gap;
        }
        stream->voice_last_publish_ns[(size_t)local - 1u] = now;
    }
    if (written) *written = got;
    if (stream) stream->mixer_mode = 1;
    return 0;
}

static int headless_audio_voice_publish2(void *ctx, uintptr_t voice,
                                         uint64_t frame_base1,
                                         const void *frames1, uint32_t bytes1,
                                         uint64_t frame_base2,
                                         const void *frames2, uint32_t bytes2,
                                         uint32_t *written)
{
    (void)ctx;
    uintptr_t local = 0;
    Wc3Voice *v = headless_voice(voice, &local);
    if (written) *written = 0;
    if (!v || (bytes1 != 0u && !frames1) ||
        (bytes2 != 0u && !frames2) || (bytes1 == 0u && bytes2 == 0u))
        return -1;
    uint32_t got = wc3_voice_publish2(v, frame_base1, frames1, bytes1,
                                      frame_base2, frames2, bytes2);
    if (!got) return -1;
    HeadlessAudio *stream = g_headless_audio_last;
    if (stream) {
        uint64_t now = stream->virtual_ns;
        uint64_t last = stream->voice_last_publish_ns[(size_t)local - 1u];
        if (last && now > last) {
            uint64_t gap = now - last;
            if (gap > stream->voice_max_gap_ns[(size_t)local - 1u])
                stream->voice_max_gap_ns[(size_t)local - 1u] = gap;
        }
        stream->voice_last_publish_ns[(size_t)local - 1u] = now;
    }
    if (written) *written = got;
    if (stream) stream->mixer_mode = 1;
    return 0;
}

static int headless_audio_voice_published(void *ctx, uintptr_t voice)
{
    (void)ctx;
    uintptr_t local = 0;
    Wc3Voice *v = headless_voice(voice, &local);
    HeadlessAudio *stream = g_headless_audio_last;
    if (!v || !stream) return -1;
    int rc = wc3_voice_published(v);
    if (rc != 0) return rc;
    uint64_t now = stream->virtual_ns;
    uint64_t last = stream->voice_last_publish_ns[(size_t)local - 1u];
    if (last && now > last) {
        uint64_t gap = now - last;
        if (gap > stream->voice_max_gap_ns[(size_t)local - 1u])
            stream->voice_max_gap_ns[(size_t)local - 1u] = gap;
    }
    stream->voice_last_publish_ns[(size_t)local - 1u] = now;
    return 0;
}

static int headless_audio_voice_reset(void *ctx, uintptr_t voice)
{
    (void)ctx;
    uintptr_t local = 0;
    Wc3Voice *v = headless_voice(voice, &local);
    HeadlessAudio *stream = g_headless_audio_last;
    if (!v || !stream) return -1;
    atomic_store_explicit(&v->resetting, 1, memory_order_release);
    headless_audio_wait_render_idle(&stream->mixer);
    while (atomic_load_explicit(&v->publishing, memory_order_acquire) != 0)
        sched_yield();
    int rc = wc3_voice_reset(v);
    stream->voice_last_publish_ns[(size_t)local - 1u] = 0u;
    stream->voice_max_gap_ns[(size_t)local - 1u] = 0u;
    atomic_store_explicit(&v->resetting, 0, memory_order_release);
    return rc;
}

static int headless_audio_voice_position(void *ctx, uintptr_t voice,
                                         uint32_t *play_bytes,
                                         uint32_t *write_bytes,
                                         uint64_t *play_frame)
{
    (void)ctx;
    Wc3Voice *v = headless_voice(voice, NULL);
    HeadlessAudio *stream = g_headless_audio_last;
    if (!v) return -1;
    wc3_voice_position(v, stream ? stream->mixer.rate : 0u, play_bytes,
                       write_bytes, play_frame);
    return 0;
}

static int headless_audio_voice_control(void *ctx, uintptr_t voice,
                                        int playing, int loop,
                                        uint32_t frequency, int volume,
                                        int pan)
{
    (void)ctx;
    Wc3Voice *v = headless_voice(voice, NULL);
    if (!v) return -1;
    wc3_voice_control(v, playing, loop, frequency, volume, pan);
    return 0;
}

static int headless_audio_voice_seek(void *ctx, uintptr_t voice,
                                     uint64_t abs_frame)
{
    (void)ctx;
    Wc3Voice *v = headless_voice(voice, NULL);
    if (!v) return -1;
    return wc3_voice_seek(v, abs_frame);
}

static int headless_audio_voice_format(void *ctx, uintptr_t voice,
                                       uint32_t rate, uint32_t channels,
                                       uint32_t bits)
{
    (void)ctx;
    Wc3Voice *v = headless_voice(voice, NULL);
    if (!v) return -1;
    if (rate < 100u || rate > 200000u || channels == 0u || channels > 2u ||
        (bits != 8u && bits != 16u))
        return -1;
    if (channels * (bits / 8u) != v->bpf) return -1;
    atomic_store_explicit(&v->frequency, rate, memory_order_relaxed);
    return 0;
}

static int headless_audio_voice_state(void *ctx, uintptr_t voice,
                                      uint32_t *playing, uint32_t *loop,
                                      uint64_t *position)
{
    (void)ctx;
    Wc3Voice *v = headless_voice(voice, NULL);
    if (!v) return -1;
    return wc3_voice_state(v, playing, loop, position);
}

static uint32_t headless_audio_voice_device_rate(void *ctx, uintptr_t voice)
{
    (void)ctx;
    if (!headless_voice(voice, NULL) || !g_headless_audio_last) return 0u;
    return g_headless_audio_last->mixer.rate;
}

static int headless_audio_voice_stats(void *ctx, uintptr_t voice,
                                      uint32_t *active, uint32_t *clipped,
                                      float *peak, uint64_t *publish_gap_us,
                                      uint32_t *ring_ms,
                                      uint32_t *callback_peak_us)
{
    (void)ctx;
    uintptr_t local = 0;
    Wc3Voice *v = headless_voice(voice, &local);
    HeadlessAudio *stream = g_headless_audio_last;
    if (!v || !stream) return -1;
    uint64_t clipped64 = 0u;
    wc3_mixer_stats(&stream->mixer, active, &clipped64, peak,
                    callback_peak_us);
    if (clipped) *clipped = (uint32_t)clipped64;
    if (publish_gap_us) {
        uint64_t gap = stream->voice_max_gap_ns[(size_t)local - 1u];
        stream->voice_max_gap_ns[(size_t)local - 1u] = 0u;
        *publish_gap_us = gap / 1000u;
    }
    if (ring_ms)
        *ring_ms = v->rate
            ? (uint32_t)((uint64_t)v->frames * 1000u / v->rate) : 0u;
    return 0;
}

static uint64_t headless_audio_play_frame(void *ctx, uintptr_t handle)
{
    (void)ctx;
    HeadlessAudio *stream = (HeadlessAudio *)(uintptr_t)handle;
    if (!stream) return 0u;
    Wc3Voice *v = headless_voice(stream->primary_voice, NULL);
    if (v)
        return atomic_load_explicit(&v->play_frame, memory_order_acquire);
    return atomic_load_explicit(&stream->pull.play_frame,
                                memory_order_acquire);
}

static uint32_t headless_audio_device_rate(void *ctx, uintptr_t handle)
{
    (void)ctx;
    HeadlessAudio *stream = (HeadlessAudio *)(uintptr_t)handle;
    return stream ? stream->mixer.rate : 0u;
}

static uintptr_t headless_audio_primary_voice(void *ctx, uintptr_t handle)
{
    (void)ctx;
    HeadlessAudio *stream = (HeadlessAudio *)(uintptr_t)handle;
    if (!stream) stream = g_headless_audio_last;
    return (stream && stream->mixer_mode) ? stream->primary_voice : 0u;
}

void wc3_headless_audio_enable_mixer(int enabled)
{
    g_headless_mixer_default = enabled ? 1 : 0;
    HeadlessAudio *stream = g_headless_audio_last;
    if (stream && stream->primary_voice) stream->mixer_mode = enabled ? 1 : 0;
}

static int headless_audio_set_consumed(
    void *ctx, uintptr_t handle,
    void (*on_consumed)(void *opaque, uint64_t total), void *opaque)
{
    (void)ctx;
    HeadlessAudio *stream = (HeadlessAudio *)(uintptr_t)handle;
    if (!stream) return -1;
    (void)pthread_mutex_lock(&stream->lock);
    stream->consumed_opaque = opaque;
    stream->on_consumed = on_consumed;
    (void)pthread_mutex_unlock(&stream->lock);
    return 0;
}

static uint64_t headless_audio_consumed_bytes(void *ctx, uintptr_t handle)
{
    (void)ctx;
    HeadlessAudio *stream = (HeadlessAudio *)(uintptr_t)handle;
    if (!stream) return 0;
    if (stream->mixer_mode)
        return atomic_load_explicit(&stream->consumed, memory_order_acquire);
    if (stream->pull_mode)
        return atomic_load_explicit(&stream->pull.consumed,
                                    memory_order_acquire);
    return atomic_load_explicit(&stream->consumed, memory_order_acquire);
}

static int headless_audio_advance(void *ctx, uintptr_t handle, uint32_t bytes)
{
    (void)ctx;
    HeadlessAudio *stream = (HeadlessAudio *)(uintptr_t)handle;
    if (!stream) return -1;

    uint32_t clock_bpf = stream->mixer_mode ? stream->channels * 2u
        : (stream->pull_mode ? stream->pull.bytes_per_frame
                             : stream->channels * (stream->bits / 8u));
    if (clock_bpf && stream->rate)
        stream->virtual_ns += (uint64_t)(bytes / clock_bpf) *
                              1000000000ull / stream->rate;

    if (stream->mixer_mode) {
        /* T1 twin of the AAudio callback: same voice mixer, same render
         * order, same FNV-1a PCM hash, advanced by deterministic bursts. */
        uint32_t bpf = stream->channels * 2u;  /* PCM_I16 headless output */
        if (bpf == 0u || bytes == 0u || (bytes % bpf) != 0u) return -1;
        uint8_t scratch[WC3_VOICE_SLOT_BYTES];
        uint32_t cap = WC3_VOICE_SLOT_BYTES / bpf;
        uint32_t burst = stream->burst_frames;
        if (burst == 0u || burst > cap) burst = cap;
        uint32_t left = bytes / bpf;
        while (left) {
            uint32_t n = left < burst ? left : burst;
            uint32_t got = wc3_mixer_render(&stream->mixer, scratch, n);
            stream->pcm_hash = wc3_pull_hash(stream->pcm_hash, scratch, got);
            uint64_t total = atomic_fetch_add_explicit(&stream->consumed,
                                 (uint64_t)got, memory_order_acq_rel) + got;
            (void)pthread_mutex_lock(&stream->lock);
            void (*cb)(void *, uint64_t) = stream->on_consumed;
            void *opaque = stream->consumed_opaque;
            (void)pthread_mutex_unlock(&stream->lock);
            if (cb) cb(opaque, total);
            left -= n;
        }
        return 0;
    }

    if (stream->pull_mode) {
        /* I-8: render BEFORE publishing consumption, in bounded bursts, so a
         * T1 replay sees the same completion cadence as the AAudio callback.
         * Underrun frames still advance the device clock (I-3). */
        uint32_t bpf = stream->pull.bytes_per_frame;
        if (bpf == 0u || bytes == 0u || (bytes % bpf) != 0u) return -1;
        uint32_t cap_frames = WC3_PULL_SLOT_BYTES / bpf;
        uint32_t burst = stream->burst_frames;
        if (burst == 0u || burst > cap_frames) burst = cap_frames;
        uint8_t scratch[WC3_PULL_SLOT_BYTES];
        uint32_t left = bytes / bpf;

        while (left) {
            uint32_t n = left < burst ? left : burst;
            uint32_t got = wc3_pull_render(&stream->pull, scratch, n);
            stream->pcm_hash = wc3_pull_hash(stream->pcm_hash, scratch, got);
            uint64_t total = atomic_load_explicit(&stream->pull.consumed,
                                                  memory_order_acquire);
            (void)pthread_mutex_lock(&stream->lock);
            void (*cb)(void *, uint64_t) = stream->on_consumed;
            void *opaque = stream->consumed_opaque;
            (void)pthread_mutex_unlock(&stream->lock);
            if (cb) cb(opaque, total);
            left -= n;
        }
        return 0;
    }

    /* Legacy linear path: unchanged, byte-for-byte, for A/B parity. */
    uint64_t queued = atomic_load_explicit(&stream->written,
                                           memory_order_acquire);
    uint64_t done = atomic_load_explicit(&stream->consumed,
                                         memory_order_acquire);
    uint64_t take = queued > done ? queued - done : 0u;
    if (take > bytes) take = bytes;
    if (!take) return 0;
    atomic_store_explicit(&stream->consumed, done + take,
                          memory_order_release);
    (void)pthread_mutex_lock(&stream->lock);
    void (*cb)(void *, uint64_t) = stream->on_consumed;
    void *opaque = stream->consumed_opaque;
    (void)pthread_mutex_unlock(&stream->lock);
    if (cb) cb(opaque, done + take);
    return 0;
}

/* Headless has no hardware queue; the measured depth is always zero. */
static uint32_t headless_audio_latency_frames(void *ctx, uintptr_t handle)
{
    (void)ctx; (void)handle;
    return 0u;
}

static int headless_audio_close(void *ctx, uintptr_t handle)
{
    (void)ctx;
    HeadlessAudio *stream = (HeadlessAudio *)(uintptr_t)handle;
    if (!stream) return -1;
    pthread_mutex_destroy(&stream->lock);
    for (unsigned i = 0; i < WC3_MIXER_VOICES; i++) free(stream->voice_pcm[i]);
    wc3_mixer_deinit(&stream->mixer);
    free(stream->pull_pcm);
    if (g_headless_audio_last == stream) g_headless_audio_last = NULL;
    free(stream);
    return 0;
}

static int headless_audio_set_callback(
    void *ctx, uintptr_t handle,
    void (*callback)(void *opaque, void *frames, uint32_t bytes), void *opaque)
{
    HeadlessState *st = (HeadlessState *)ctx;
    HeadlessAudio *stream = (HeadlessAudio *)(uintptr_t)handle;
    if (!st || !stream) return -1;
    st->audio_callback = callback;
    st->audio_opaque = opaque;
    return 0;
}

void wc3_headless_audio_enable_pull(int enabled)
{
    HeadlessAudio *stream = g_headless_audio_last;
    if (!stream || !stream->pull_pcm) return;
    stream->pull_mode = enabled ? 1 : 0;
}

void wc3_headless_audio_set_burst(uint32_t frames)
{
    HeadlessAudio *stream = g_headless_audio_last;
    if (!stream || !stream->pull_pcm) return;
    uint32_t bpf = stream->pull.bytes_per_frame;
    uint32_t cap = bpf ? WC3_PULL_SLOT_BYTES / bpf : 1u;
    if (frames == 0u) frames = cap;
    stream->burst_frames = frames < cap ? frames : cap;
}

int wc3_headless_audio_pull_stats(uintptr_t stream_handle,
                                  Wc3HeadlessPullStats *out)
{
    HeadlessAudio *stream = (HeadlessAudio *)(uintptr_t)stream_handle;
    if (!stream || !out) return -1;
    memset(out, 0, sizeof *out);
    out->play_frame = atomic_load_explicit(&stream->pull.play_frame,
                                           memory_order_acquire);
    out->published_frame = atomic_load_explicit(&stream->pull.published_frame,
                                                memory_order_acquire);
    out->consumed = atomic_load_explicit(&stream->pull.consumed,
                                         memory_order_acquire);
    out->stale_frames = atomic_load_explicit(&stream->pull.stale_frames,
                                             memory_order_relaxed);
    out->publish_regions = atomic_load_explicit(&stream->pull.publish_regions,
                                                memory_order_relaxed);
    out->render_calls = atomic_load_explicit(&stream->pull.render_calls,
                                             memory_order_relaxed);
    out->pcm_hash = stream->pcm_hash;
    out->burst_frames = stream->burst_frames;
    out->pull_mode = stream->pull_mode;
    return 0;
}

/* --------------------------------------------------------------- clipboard */

static int headless_clipboard_get(void *ctx, char *dst, uint32_t cap,
                                  uint32_t *needed)
{
    HeadlessState *st = (HeadlessState *)ctx;
    if (!st) return -1;
    if (!dst && cap != 0u) return -1;
    (void)pthread_mutex_lock(&st->clipboard_lock);
    size_t len = st->clipboard ? strlen(st->clipboard) : 0u;
    if (needed) *needed = (uint32_t)len + 1u;
    if (dst && cap > 0u) {
        size_t n = len < (size_t)cap - 1u ? len : (size_t)cap - 1u;
        if (n) memcpy(dst, st->clipboard, n);
        dst[n] = '\0';
    }
    (void)pthread_mutex_unlock(&st->clipboard_lock);
    return 0;
}

static int headless_clipboard_set(void *ctx, const char *text)
{
    HeadlessState *st = (HeadlessState *)ctx;
    if (!st) return -1;
    size_t len = text ? strlen(text) : 0u;
    char *copy = len ? (char *)malloc(len + 1u) : NULL;
    if (len && !copy) return -1;
    if (copy) memcpy(copy, text, len + 1u);
    (void)pthread_mutex_lock(&st->clipboard_lock);
    free(st->clipboard);
    st->clipboard = copy;
    (void)pthread_mutex_unlock(&st->clipboard_lock);
    return 0;
}

/* ----------------------------------------------------------------- surface */

static int headless_surface_acquire(void *ctx, uintptr_t *native_window)
{
    HeadlessState *st = (HeadlessState *)ctx;
    if (!st || !native_window) return -1;
    *native_window = (uintptr_t)st;
    return 0;
}

static void headless_surface_release(void *ctx, uintptr_t native_window)
{
    (void)ctx;
    (void)native_window;
}

static void headless_surface_resize(void *ctx, uint32_t width, uint32_t height)
{
    HeadlessState *st = (HeadlessState *)ctx;
    if (!st || width == 0u || height == 0u) return;
    st->width = width;
    st->height = height;
}

static void headless_surface_lost(void *ctx)
{
    (void)ctx;
}

/* --------------------------------------------------------------------- log */

static void headless_log_write(void *ctx, int priority, const char *tag,
                               const char *text)
{
    (void)ctx;
    fprintf(stderr, "[%d] %s: %s\n", priority, tag ? tag : "wc3",
            text ? text : "");
    (void)fflush(stderr);
}

/* ------------------------------------------------------------------ config */

static const char *headless_config_get(void *ctx, const char *key)
{
    HeadlessState *st = (HeadlessState *)ctx;
    if (!st || !key || !*key) return NULL;
    size_t key_len = strlen(key);
    for (uint32_t i = 0; i < st->config_count; i++) {
        const char *entry = st->config[i];
        if (strncmp(entry, key, key_len) == 0 && entry[key_len] == '=') {
            return entry + key_len + 1u;
        }
    }
    return NULL;
}

static int headless_config_get_int(void *ctx, const char *key, int fallback)
{
    const char *value = headless_config_get(ctx, key);
    if (!value || !*value) return fallback;
    char *end = NULL;
    long parsed = strtol(value, &end, 0);
    if (end == value || *end != '\0') return fallback;
    return (int)parsed;
}

/* ------------------------------------------------------------------- asset */

static int headless_asset_init(void *ctx, const char *asset_root,
                               const char *save_root)
{
    HeadlessState *st = (HeadlessState *)ctx;
    if (!st || !asset_root || !*asset_root || !save_root || !*save_root)
        return -1;
    return headless_asset_roots(st, asset_root, save_root);
}

static const char *headless_asset_root(void *ctx)
{
    HeadlessState *st = (HeadlessState *)ctx;
    return st && st->asset_root[0] ? st->asset_root : NULL;
}

static const char *headless_save_root(void *ctx)
{
    HeadlessState *st = (HeadlessState *)ctx;
    return st && st->save_root[0] ? st->save_root : NULL;
}

static int headless_asset_enumerate(void *ctx, uint32_t index, char *name,
                                    size_t cap)
{
    HeadlessState *st = (HeadlessState *)ctx;
    if (!st || st->asset_root[0] == '\0' || !name || cap == 0u) return -1;
    if (index >= st->mpq_count) return 0;
    size_t n = strlen(st->mpq[index]);
    if (n >= cap) n = cap - 1u;
    memcpy(name, st->mpq[index], n);
    name[n] = '\0';
    return 1;
}

static int headless_asset_present(void *ctx, const char *logical_name)
{
    HeadlessState *st = (HeadlessState *)ctx;
    if (!st || st->asset_root[0] == '\0' || !logical_name ||
        !*logical_name)
        return -1;
    if (strchr(logical_name, '/') || strchr(logical_name, '\\')) return 0;

    char path[WC3_HEADLESS_PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/%s", st->asset_root, logical_name);
    if (n <= 0 || (size_t)n >= sizeof path) return -1;
    if (headless_regular_file(path)) return 1;

    DIR *dir = opendir(st->asset_root);
    if (!dir) return 0;
    int present = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!name_equal_ci(ent->d_name, logical_name)) continue;
        n = snprintf(path, sizeof path, "%s/%s", st->asset_root, ent->d_name);
        if (n > 0 && (size_t)n < sizeof path && headless_regular_file(path)) {
            present = 1;
            break;
        }
    }
    (void)closedir(dir);
    return present;
}

/* ------------------------------------------------------------------ locale */

static uint32_t headless_ansi_codepage(void *ctx)
{
    (void)ctx;
    return 1252u;
}

static uint32_t headless_oem_codepage(void *ctx)
{
    (void)ctx;
    return 437u;
}

static uint32_t headless_default_lcid(void *ctx)
{
    (void)ctx;
    return 0x0409u; /* en-US */
}

static int utf8_decode(const char *src, int32_t len, uint32_t *out,
                       int32_t *used)
{
    unsigned char c = (unsigned char)*src;
    if (c < 0x80u) {
        *out = c;
        *used = 1;
        return 0;
    }
    uint32_t cp;
    int n;
    if ((c & 0xE0u) == 0xC0u) {
        cp = c & 0x1Fu;
        n = 1;
    } else if ((c & 0xF0u) == 0xE0u) {
        cp = c & 0x0Fu;
        n = 2;
    } else if ((c & 0xF8u) == 0xF0u) {
        cp = c & 0x07u;
        n = 3;
    } else {
        return -1;
    }
    if (len >= 0 && n > len) return -1;
    for (int i = 1; i <= n; i++) {
        unsigned char cc = (unsigned char)src[i];
        if ((cc & 0xC0u) != 0x80u) return -1;
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return -1;
    *out = cp;
    *used = n + 1;
    return 0;
}

static int utf8_encode(uint32_t cp, char *dst, int32_t cap)
{
    if (cp <= 0x7Fu) {
        if (cap < 1) return -1;
        dst[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FFu) {
        if (cap < 2) return -1;
        dst[0] = (char)(0xC0u | (cp >> 6));
        dst[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp <= 0xFFFFu) {
        if (cap < 3) return -1;
        dst[0] = (char)(0xE0u | (cp >> 12));
        dst[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        if (cap < 4) return -1;
        dst[0] = (char)(0xF0u | (cp >> 18));
        dst[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        dst[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    return -1;
}

static int headless_to_wide(void *ctx, uint32_t codepage, const char *src,
                            int32_t src_len, uint16_t *dst, int32_t dst_len)
{
    (void)ctx;
    if (!src || !dst || dst_len < 0) return -1;
    int32_t len = src_len < 0 ? (int32_t)strlen(src) : src_len;
    int32_t at = 0;
    int32_t out = 0;
    while (at < len) {
        uint32_t cp;
        if (codepage == 65001u) {
            int32_t used = 0;
            if (utf8_decode(src + at, len - at, &cp, &used) != 0) return -1;
            at += used;
        } else {
            cp = (unsigned char)src[at++];
        }
        if (cp > 0xFFFFu) {
            if (out + 2 > dst_len) return -1;
            cp -= 0x10000u;
            dst[out++] = (uint16_t)(0xD800u | (cp >> 10));
            dst[out++] = (uint16_t)(0xDC00u | (cp & 0x3FFu));
        } else {
            if (out + 1 > dst_len) return -1;
            dst[out++] = (uint16_t)cp;
        }
    }
    if (out < dst_len) dst[out] = 0u;
    return out;
}

static int headless_from_wide(void *ctx, uint32_t codepage,
                              const uint16_t *src, int32_t src_len, char *dst,
                              int32_t dst_len)
{
    (void)ctx;
    if (!src || !dst || dst_len < 0) return -1;
    int32_t len = src_len < 0 ? (int32_t)0 : src_len;
    if (src_len < 0) {
        while (src[len] != 0u) len++;
    }
    int32_t at = 0;
    int32_t out = 0;
    while (at < len) {
        uint32_t cp = src[at++];
        if (cp >= 0xD800u && cp <= 0xDBFFu && at < len) {
            uint32_t lo = src[at];
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                at++;
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
            }
        }
        if (codepage == 65001u) {
            int32_t need = cp <= 0x7Fu ? 1
                           : cp <= 0x7FFu ? 2
                           : cp <= 0xFFFFu ? 3
                                           : 4;
            if (out + need > dst_len) return -1;
            int n = utf8_encode(cp, dst + out, dst_len - out);
            if (n < 0) return -1;
            out += n;
        } else {
            if (out + 1 > dst_len) return -1;
            dst[out++] = cp <= 0xFFu ? (char)cp : '?';
        }
    }
    if (out < dst_len) dst[out] = '\0';
    return out;
}

/* ------------------------------------------------------------------ vulkan */

static int headless_vulkan_select(void *ctx, int driver)
{
    (void)ctx;
    (void)driver;
    return -1; /* no Vulkan device in the headless target */
}

static int headless_vulkan_version(void *ctx, uint32_t *version)
{
    (void)ctx;
    (void)version;
    return -1;
}

static int headless_vulkan_caps(void *ctx, uint32_t device_index,
                                Wc3VulkanCaps *caps)
{
    (void)ctx;
    (void)device_index;
    (void)caps;
    return -1;
}

/* ------------------------------------------------------------------ net */

/* The verification DUT is asset-free and has no network identity.  Keeping
 * every NetApi entry present but explicitly unsupported lets the winsock HLE
 * install without fabricating an external connection. */
static int headless_net_startup(void *ctx) { (void)ctx; return 0; }
static void headless_net_cleanup(void *ctx) { (void)ctx; }
static int headless_net_unsupported(void) { return -WC3_NET_ERR_OPNOTSUPP; }
static int headless_net_socket(void *ctx, int family, int type, int protocol,
                               Wc3NetSocket *out)
{
    (void)ctx; (void)family; (void)type; (void)protocol;
    if (out) *out = 0;
    return headless_net_unsupported();
}
static int headless_net_close(void *ctx, Wc3NetSocket socket)
{
    (void)ctx; (void)socket;
    return headless_net_unsupported();
}
static int headless_net_bind(void *ctx, Wc3NetSocket socket, const void *addr,
                             uint32_t length)
{
    (void)ctx; (void)socket; (void)addr; (void)length;
    return headless_net_unsupported();
}
static int headless_net_connect(void *ctx, Wc3NetSocket socket,
                                const void *addr, uint32_t length)
{
    (void)ctx; (void)socket; (void)addr; (void)length;
    return headless_net_unsupported();
}
static int headless_net_listen(void *ctx, Wc3NetSocket socket, int backlog)
{
    (void)ctx; (void)socket; (void)backlog;
    return headless_net_unsupported();
}
static int headless_net_accept(void *ctx, Wc3NetSocket socket,
                               Wc3NetSocket *out, void *addr,
                               uint32_t *length)
{
    (void)ctx; (void)socket; (void)addr; (void)length;
    if (out) *out = 0;
    return headless_net_unsupported();
}
static int headless_net_send(void *ctx, Wc3NetSocket socket, const void *buf,
                             uint32_t length, int flags, uint32_t *sent)
{
    (void)ctx; (void)socket; (void)buf; (void)length; (void)flags;
    if (sent) *sent = 0;
    return headless_net_unsupported();
}
static int headless_net_recv(void *ctx, Wc3NetSocket socket, void *buf,
                             uint32_t length, int flags, uint32_t *received)
{
    (void)ctx; (void)socket; (void)buf; (void)length; (void)flags;
    if (received) *received = 0;
    return headless_net_unsupported();
}
static int headless_net_sendto(void *ctx, Wc3NetSocket socket, const void *buf,
                               uint32_t length, int flags, const void *addr,
                               uint32_t addr_length, uint32_t *sent)
{
    (void)ctx; (void)socket; (void)buf; (void)length; (void)flags;
    (void)addr; (void)addr_length;
    if (sent) *sent = 0;
    return headless_net_unsupported();
}
static int headless_net_recvfrom(void *ctx, Wc3NetSocket socket, void *buf,
                                 uint32_t length, int flags, void *addr,
                                 uint32_t *addr_length, uint32_t *received)
{
    (void)ctx; (void)socket; (void)buf; (void)length; (void)flags;
    (void)addr; (void)addr_length;
    if (received) *received = 0;
    return headless_net_unsupported();
}
static int headless_net_shutdown(void *ctx, Wc3NetSocket socket, int how)
{
    (void)ctx; (void)socket; (void)how;
    return headless_net_unsupported();
}
static int headless_net_getpeername(void *ctx, Wc3NetSocket socket, void *addr,
                                    uint32_t *length)
{
    (void)ctx; (void)socket; (void)addr; (void)length;
    return headless_net_unsupported();
}
static int headless_net_getsockname(void *ctx, Wc3NetSocket socket, void *addr,
                                    uint32_t *length)
{
    (void)ctx; (void)socket; (void)addr; (void)length;
    return headless_net_unsupported();
}
static int headless_net_setsockopt(void *ctx, Wc3NetSocket socket, int option,
                                   const void *value, uint32_t length)
{
    (void)ctx; (void)socket; (void)option; (void)value; (void)length;
    return headless_net_unsupported();
}
static int headless_net_getsockopt(void *ctx, Wc3NetSocket socket, int option,
                                   void *value, uint32_t *length)
{
    (void)ctx; (void)socket; (void)option; (void)value; (void)length;
    return headless_net_unsupported();
}
static int headless_net_set_nonblocking(void *ctx, Wc3NetSocket socket,
                                        int enable)
{
    (void)ctx; (void)socket; (void)enable;
    return headless_net_unsupported();
}
static int headless_net_bytes_available(void *ctx, Wc3NetSocket socket,
                                        uint32_t *out)
{
    (void)ctx; (void)socket;
    if (out) *out = 0;
    return headless_net_unsupported();
}
static int headless_net_at_mark(void *ctx, Wc3NetSocket socket, int *out)
{
    (void)ctx; (void)socket;
    if (out) *out = 0;
    return headless_net_unsupported();
}
static int headless_net_select(void *ctx, Wc3NetFdSet *read,
                               Wc3NetFdSet *write, Wc3NetFdSet *except,
                               int64_t timeout_us)
{
    (void)ctx; (void)read; (void)write; (void)except; (void)timeout_us;
    return headless_net_unsupported();
}
static int headless_net_hostname(void *ctx, char *buf, uint32_t cap)
{
    (void)ctx;
    if (!buf || cap == 0u) return -WC3_NET_ERR_INVAL;
    static const char name[] = "headless";
    if (sizeof name > cap) return -WC3_NET_ERR_INVAL;
    memcpy(buf, name, sizeof name);
    return 0;
}
static int headless_net_resolve(void *ctx, const char *node,
                                const char *service,
                                const Wc3NetResolveHints *hints,
                                Wc3NetAddrInfo *out, uint32_t cap,
                                uint32_t *count)
{
    (void)ctx; (void)node; (void)service; (void)hints; (void)out; (void)cap;
    if (count) *count = 0;
    return -WC3_NET_ERR_NONAME;
}
static int headless_net_interfaces(void *ctx, Wc3NetInterface *out,
                                   uint32_t cap, uint32_t *count)
{
    (void)ctx; (void)out; (void)cap;
    if (count) *count = 0;
    return 0;
}

static void headless_net_install(Wc3NetApi *net)
{
    memset(net, 0, sizeof *net);
    net->startup = headless_net_startup;
    net->cleanup = headless_net_cleanup;
    net->socket = headless_net_socket;
    net->close = headless_net_close;
    net->bind = headless_net_bind;
    net->connect = headless_net_connect;
    net->listen = headless_net_listen;
    net->accept = headless_net_accept;
    net->send = headless_net_send;
    net->recv = headless_net_recv;
    net->sendto = headless_net_sendto;
    net->recvfrom = headless_net_recvfrom;
    net->shutdown = headless_net_shutdown;
    net->getpeername = headless_net_getpeername;
    net->getsockname = headless_net_getsockname;
    net->setsockopt = headless_net_setsockopt;
    net->getsockopt = headless_net_getsockopt;
    net->set_nonblocking = headless_net_set_nonblocking;
    net->bytes_available = headless_net_bytes_available;
    net->at_mark = headless_net_at_mark;
    net->select = headless_net_select;
    net->hostname = headless_net_hostname;
    net->resolve = headless_net_resolve;
    net->interfaces = headless_net_interfaces;
}

/* ------------------------------------------------------------------ install */

void headless_services_install(Wc3Platform *platform, HeadlessState *st)
{
    headless_net_install(&platform->net);
    platform->window.ctx = st;
    platform->window.create = headless_window_create;
    platform->window.destroy = headless_window_destroy;
    platform->window.size = headless_window_size;
    platform->window.set_title = headless_window_set_title;

    platform->input.ctx = st;
    platform->input.push = headless_input_push;
    platform->input.set_text_focus = headless_input_focus;

    platform->audio.ctx = st;
    platform->audio.open = headless_audio_open;
    platform->audio.write = headless_audio_write;
    platform->audio.close = headless_audio_close;
    platform->audio.set_callback = headless_audio_set_callback;
    platform->audio.set_consumed = headless_audio_set_consumed;
    platform->audio.consumed_bytes = headless_audio_consumed_bytes;
    platform->audio.advance = headless_audio_advance;
    platform->audio.latency_frames = headless_audio_latency_frames;
    platform->audio.publish = headless_audio_publish;
    /* Full nfsu2 port: identical method surface for the T1 DUT.  The mixer is
     * inert until wc3_headless_audio_enable_mixer() is called by a replay. */
    platform->audio.voice_add = headless_audio_voice_add;
    platform->audio.voice_remove = headless_audio_voice_remove;
    platform->audio.voice_publish = headless_audio_voice_publish;
    platform->audio.voice_position = headless_audio_voice_position;
    platform->audio.voice_control = headless_audio_voice_control;
    platform->audio.voice_seek = headless_audio_voice_seek;
    platform->audio.voice_format = headless_audio_voice_format;
    platform->audio.voice_stats = headless_audio_voice_stats;
    platform->audio.voice_state = headless_audio_voice_state;
    platform->audio.voice_device_rate = headless_audio_voice_device_rate;
    platform->audio.voice_publish2 = headless_audio_voice_publish2;
    platform->audio.voice_reset = headless_audio_voice_reset;
    platform->audio.voice_published = headless_audio_voice_published;
    platform->audio.play_frame = headless_audio_play_frame;
    platform->audio.device_rate = headless_audio_device_rate;
    platform->audio.primary_voice = headless_audio_primary_voice;

    platform->clipboard.ctx = st;
    platform->clipboard.get_text = headless_clipboard_get;
    platform->clipboard.set_text = headless_clipboard_set;

    platform->surface.ctx = st;
    platform->surface.acquire = headless_surface_acquire;
    platform->surface.release = headless_surface_release;
    platform->surface.resize = headless_surface_resize;
    platform->surface.lost = headless_surface_lost;

    platform->log.ctx = st;
    platform->log.write = headless_log_write;

    platform->config.ctx = st;
    platform->config.get = headless_config_get;
    platform->config.get_int = headless_config_get_int;

    platform->asset.ctx = st;
    platform->asset.init = headless_asset_init;
    platform->asset.asset_root = headless_asset_root;
    platform->asset.save_root = headless_save_root;
    platform->asset.enumerate_mpq = headless_asset_enumerate;
    platform->asset.present = headless_asset_present;

    platform->locale.ctx = st;
    platform->locale.ansi_codepage = headless_ansi_codepage;
    platform->locale.oem_codepage = headless_oem_codepage;
    platform->locale.default_lcid = headless_default_lcid;
    platform->locale.to_wide = headless_to_wide;
    platform->locale.from_wide = headless_from_wide;

    platform->vulkan_driver.ctx = st;
    platform->vulkan_driver.select = headless_vulkan_select;
    platform->vulkan_driver.instance_version = headless_vulkan_version;
    platform->vulkan_driver.device_caps = headless_vulkan_caps;
}

void headless_services_destroy(HeadlessState *st)
{
    if (!st) return;
    free(st->clipboard);
    st->clipboard = NULL;
    headless_config_free(st);
    st->mpq_count = 0u;
    st->audio_callback = NULL;
    st->audio_opaque = NULL;
}
