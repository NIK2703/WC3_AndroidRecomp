/*
 * headless_services.c - the non-privileged half of the headless platform.
 *
 * Window, input, audio, clipboard, surface, log, config, assets, locale and
 * Vulkan are deliberately small.  The verification DUT has no display and no
 * sound device: the window is a size record, audio writes succeed and are
 * discarded, and the surface hands out a stable fake native handle.  Input is
 * a lock-free SPSC mailbox so a replay schedule can feed the guest thread.
 */
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

/* The headless DUT has no hardware clock.  A T1 test calls
 * wc3_headless_audio_advance() with a frame count, which renders the same
 * nfsu2 mixer used on Android and hashes the resulting PCM.  There is no
 * linear queue and no pull ring: DirectSound voices are the only input. */
typedef struct HeadlessAudio {
    uint32_t rate;
    uint32_t channels;
    uint32_t bits;
    Wc3PcmMixer mixer;
    uint8_t *voice_pcm[WC3_MIXER_VOICES];
    uintptr_t primary_voice;
    uint64_t consumed_frames;
    uint64_t pcm_hash;
    uint32_t render_calls;
} HeadlessAudio;

static HeadlessAudio *g_headless_audio_last;

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

/* Same sizing as the Android primary: seven 50 ms headers plus reset
 * headroom, rounded to the voice slot size, never below 64 KiB. */
static uint32_t headless_primary_bytes(uint32_t rate, uint32_t bpf)
{
    if (!rate || !bpf) return 0u;
    uint64_t bytes = ((uint64_t)rate / 20u) * bpf + bpf;
    uint64_t header = ((bytes + WC3_VOICE_SLOT_BYTES - 1u) /
                       WC3_VOICE_SLOT_BYTES) * WC3_VOICE_SLOT_BYTES;
    uint64_t ring = 7u * header;
    if (ring < WC3_VOICE_MIN_RING_BYTES) ring = WC3_VOICE_MIN_RING_BYTES;
    ring = ((ring + WC3_VOICE_SLOT_BYTES - 1u) /
            WC3_VOICE_SLOT_BYTES) * WC3_VOICE_SLOT_BYTES;
    if (ring > (uint64_t)WC3_VOICE_MAX_SLOTS * WC3_VOICE_SLOT_BYTES)
        return 0u;
    return (uint32_t)ring;
}

static HeadlessAudio *headless_audio(uintptr_t handle)
{
    return (HeadlessAudio *)handle;
}

static Wc3Voice *headless_voice(uintptr_t voice, HeadlessAudio **out_stream)
{
    HeadlessAudio *st = g_headless_audio_last;
    uintptr_t local = voice & 0xFFFFu;
    if (!st || !local) return NULL;
    Wc3Voice *v = wc3_mixer_voice(&st->mixer, local);
    if (v && out_stream) *out_stream = st;
    return v;
}

static int headless_audio_primary_add(HeadlessAudio *st, uint32_t rate,
                                      uint32_t channels, uint32_t bits,
                                      uint32_t bytes, uintptr_t *out_voice)
{
    if (!st || !out_voice) return -1;
    *out_voice = 0;
    uint32_t bpf = channels * (bits / 8u);
    if (bpf == 0u || (WC3_VOICE_SLOT_BYTES % bpf) != 0u ||
        bytes < bpf || (bytes % bpf) != 0u) return -1;
    uint32_t slots = (bytes + WC3_VOICE_SLOT_BYTES - 1u) /
                     WC3_VOICE_SLOT_BYTES;
    if (slots == 0u || slots > WC3_VOICE_MAX_SLOTS) return -1;
    size_t total = (size_t)slots * WC3_VOICE_SLOT_BYTES *
                   WC3_VOICE_SNAPSHOTS;
    uint8_t *pcm = (uint8_t *)calloc(1, total);
    if (!pcm) return -1;
    uintptr_t local = 0;
    if (wc3_mixer_voice_add_snapshot(&st->mixer, pcm, bytes, rate, channels,
                                     bits, &local) != 0) {
        free(pcm);
        return -1;
    }
    st->voice_pcm[(size_t)local - 1u] = pcm;
    *out_voice = local;
    return 0;
}

static int headless_audio_open(void *ctx, uint32_t sample_rate,
                               uint32_t channels, uint32_t bits,
                               uintptr_t *out)
{
    (void)ctx;
    if (!out) return -1;
    *out = 0;
    if (bits != 8u && bits != 16u) return -1;
    if (sample_rate < 8000u || sample_rate > 192000u) return -1;
    if (channels < 1u || channels > 8u) return -1;

    HeadlessAudio *st = (HeadlessAudio *)calloc(1, sizeof *st);
    if (!st) return -1;
    st->rate = sample_rate;
    st->channels = channels;
    st->bits = bits;

    uint32_t bpf = channels * (bits / 8u);
    uint32_t primary_bytes = headless_primary_bytes(sample_rate, bpf);
    if (primary_bytes == 0u ||
        wc3_mixer_init(&st->mixer, sample_rate, channels, 0) != 0 ||
        headless_audio_primary_add(st, sample_rate, channels, bits,
                                   primary_bytes, &st->primary_voice) != 0) {
        for (unsigned i = 0; i < WC3_MIXER_VOICES; i++)
            free(st->voice_pcm[i]);
        wc3_mixer_deinit(&st->mixer);
        free(st);
        return -1;
    }

    g_headless_audio_last = st;
    *out = (uintptr_t)st;
    return 0;
}

static int headless_audio_close(void *ctx, uintptr_t handle)
{
    (void)ctx;
    HeadlessAudio *st = headless_audio(handle);
    if (!st) return -1;
    for (unsigned i = 0; i < WC3_MIXER_VOICES; i++) free(st->voice_pcm[i]);
    wc3_mixer_deinit(&st->mixer);
    if (g_headless_audio_last == st) g_headless_audio_last = NULL;
    free(st);
    return 0;
}

static int headless_audio_voice_add(void *ctx, uintptr_t handle,
                                    uint32_t rate, uint32_t channels,
                                    uint32_t bits, uint32_t bytes,
                                    uintptr_t *out_voice)
{
    (void)ctx;
    HeadlessAudio *st = headless_audio(handle);
    if (!st || !out_voice || !bytes) return -1;
    *out_voice = 0;
    uint32_t bpf = channels * (bits / 8u);
    if (bpf == 0u || (WC3_VOICE_SLOT_BYTES % bpf) != 0u ||
        (bytes % bpf) != 0u) return -1;
    uint32_t slots = (bytes + WC3_VOICE_SLOT_BYTES - 1u) /
                     WC3_VOICE_SLOT_BYTES;
    if (slots == 0u || slots > WC3_VOICE_MAX_SLOTS) return -1;
    uint8_t *pcm = (uint8_t *)calloc(1,
        (size_t)slots * WC3_VOICE_SLOT_BYTES);
    if (!pcm) return -1;
    uintptr_t local = 0;
    if (wc3_mixer_voice_add(&st->mixer, pcm, bytes, rate, channels, bits,
                            &local) != 0) {
        free(pcm);
        return -1;
    }
    st->voice_pcm[(size_t)local - 1u] = pcm;
    *out_voice = local;
    return 0;
}

static int headless_audio_voice_remove(void *ctx, uintptr_t voice)
{
    (void)ctx;
    HeadlessAudio *st = NULL;
    Wc3Voice *v = headless_voice(voice, &st);
    if (!v) return -1;
    uintptr_t local = voice & 0xFFFFu;
    if (wc3_mixer_voice_remove(&st->mixer, local) != 0) return -1;
    headless_audio_wait_render_idle(&st->mixer);
    while (atomic_load_explicit(&v->publishing, memory_order_acquire) != 0)
        sched_yield();
    wc3_mixer_voice_release(v);
    free(st->voice_pcm[(size_t)local - 1u]);
    st->voice_pcm[(size_t)local - 1u] = NULL;
    return 0;
}

static int headless_audio_voice_publish(void *ctx, uintptr_t voice,
                                        uint64_t frame_base,
                                        const void *frames, uint32_t bytes,
                                        uint32_t *written)
{
    (void)ctx;
    if (written) *written = 0;
    Wc3Voice *v = headless_voice(voice, NULL);
    if (!v || !frames || !bytes) return -1;
    uint32_t got = wc3_voice_publish(v, frame_base, frames, bytes);
    if (!got) return -1;
    if (written) *written = got;
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
    if (written) *written = 0;
    Wc3Voice *v = headless_voice(voice, NULL);
    if (!v || (bytes1 && !frames1) || (bytes2 && !frames2) ||
        (!bytes1 && !bytes2)) return -1;
    uint32_t got = wc3_voice_publish2(v, frame_base1, frames1, bytes1,
                                      frame_base2, frames2, bytes2);
    if (!got) return -1;
    if (written) *written = got;
    return 0;
}

static int headless_audio_voice_published(void *ctx, uintptr_t voice)
{
    (void)ctx;
    Wc3Voice *v = headless_voice(voice, NULL);
    if (!v) return -1;
    return wc3_voice_published(v);
}

static int headless_audio_voice_position(void *ctx, uintptr_t voice,
                                         uint32_t *play_bytes,
                                         uint32_t *write_bytes,
                                         uint64_t *play_frame)
{
    (void)ctx;
    HeadlessAudio *st = NULL;
    Wc3Voice *v = headless_voice(voice, &st);
    if (!v) return -1;
    wc3_voice_position(v, st->mixer.rate, play_bytes, write_bytes, play_frame);
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
    HeadlessAudio *st = NULL;
    if (!headless_voice(voice, &st)) return 0u;
    return st->mixer.rate;
}

static uint32_t headless_audio_device_rate(void *ctx, uintptr_t handle)
{
    (void)ctx;
    HeadlessAudio *st = headless_audio(handle);
    return st ? st->mixer.rate : 0u;
}

static uintptr_t headless_audio_primary_voice(void *ctx, uintptr_t handle)
{
    (void)ctx;
    HeadlessAudio *st = headless_audio(handle);
    if (!st) st = g_headless_audio_last;
    return st ? st->primary_voice : 0u;
}

/* T1 device step: render exactly `frames` output frames at the stream rate,
 * hash the complete int16 block and advance the virtual clock.  The mixer
 * itself owns voice resampling, looping and pan/volume. */
int wc3_headless_audio_advance(uintptr_t handle, uint32_t frames)
{
    HeadlessAudio *st = headless_audio(handle);
    if (!st || frames == 0u) return -1;
    uint32_t bpf = st->channels * 2u;
    uint32_t bytes = frames * bpf;
    uint8_t *pcm = (uint8_t *)malloc(bytes);
    if (!pcm) return -1;
    uint32_t got = wc3_mixer_render(&st->mixer, pcm, frames);
    if (got < bytes) memset(pcm + got, 0, bytes - got);
    uint64_t hash = 1469598103934665603ULL;
    for (uint32_t i = 0; i < bytes; i++) {
        hash ^= pcm[i];
        hash *= 1099511628211ULL;
    }
    free(pcm);
    st->pcm_hash = hash;
    st->consumed_frames += frames;
    st->render_calls++;
    return 0;
}

int wc3_headless_audio_stats(uintptr_t handle, Wc3HeadlessAudioStats *out)
{
    HeadlessAudio *st = headless_audio(handle);
    if (!st || !out) return -1;
    uint64_t clipped = 0u;
    float peak = 0.0f;
    uint32_t callback_peak = 0u;
    wc3_mixer_stats(&st->mixer, &out->active_voices, &clipped, &peak,
                    &callback_peak);
    out->consumed_frames = st->consumed_frames;
    out->pcm_hash = st->pcm_hash;
    out->render_calls = st->render_calls;
    out->clipped = clipped;
    out->peak = peak;
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
    platform->audio.close = headless_audio_close;
    platform->audio.voice_add = headless_audio_voice_add;
    platform->audio.voice_remove = headless_audio_voice_remove;
    platform->audio.voice_publish = headless_audio_voice_publish;
    platform->audio.voice_position = headless_audio_voice_position;
    platform->audio.voice_control = headless_audio_voice_control;
    platform->audio.voice_seek = headless_audio_voice_seek;
    platform->audio.voice_state = headless_audio_voice_state;
    platform->audio.voice_device_rate = headless_audio_voice_device_rate;
    platform->audio.voice_publish2 = headless_audio_voice_publish2;
    platform->audio.voice_published = headless_audio_voice_published;
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
