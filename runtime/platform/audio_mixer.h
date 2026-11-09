/*
 * audio_mixer.h - host-owned LOOPING PCM voices, mixed in the AAudio
 * real-time callback.  C port of nfsu2-recompiled's PcmVoice / PcmMixer /
 * guest_audio.inc (the "upper half" of their audio architecture).
 *
 * nfsu2 has no WAVEHDR pool and no WOM_DONE: the guest owns one persistent
 * looping DirectSound buffer, publishes only the region it Lock/Unlock'ed,
 * polls GetCurrentPosition, and PcmMixer::render() PULLS and resamples inside
 * the AAudio data callback.  This file supplies exactly those mechanics for
 * the WC3 host layer, without touching the frozen guest ABI.
 *
 * Invariants (H7 / TA-7):
 *   - render() takes no host lock, allocates nothing, touches no guest memory;
 *   - the producer publishes a region under a per-slot seqlock (release),
 *     the consumer accepts only a stable even seqlock (acquire);
 *   - the PCM ring is circular, exactly like nfsu2's PcmVoice::samples: a
 *     buffer published once loops on that data every revolution; a slot that
 *     is being written at this instant produces silence, never torn PCM;
 *   - a streaming loop with no publish for a whole ring revolution is muted
 *     (nfsu2 stale policy) instead of replaying its last half second;
 *   - a one-shot voice stops at its buffer end and resets its cursor, exactly
 *     like PcmMixer::render();
 *   - voice removal marks the slot dying (used==3), waits for render_users to
 *     reach zero, then releases the sequence storage; only then is the slot
 *     reusable, so a new voice can never alias an in-flight render.
 *
 * No OS headers, no libm, no C++.  Used by both the Android sink and the
 * headless verification DUT, so they render byte-identical PCM.
 */
#ifndef WC3_PLATFORM_AUDIO_MIXER_H
#define WC3_PLATFORM_AUDIO_MIXER_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio_resampler.h"

#define WC3_MIXER_VOICES       128u
#define WC3_VOICE_SLOT_BYTES   1024u
/* 16 MiB / 1 KiB: the same upper bound as the nfsu2 DirectSound buffer. */
#define WC3_VOICE_MAX_SLOTS    16384u
#define WC3_MIX_CHUNK          256u

static inline uint32_t wc3_float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static inline float wc3_bits_float(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

typedef struct Wc3Voice {
    uint8_t *pcm;                       /* host-owned, slots * 1024 bytes */
    _Atomic uint32_t *seq;              /* one seqlock generation per slot */
    uint32_t bytes, frames, bpf, slots;
    uint32_t rate, channels, bits;
    _Atomic uint32_t frequency;
    _Atomic uint32_t left_bits, right_bits;
    _Atomic uint64_t published_frame;   /* one past the last published frame */
    _Atomic uint64_t play_frame;        /* absolute SOURCE frames played */
    _Atomic uint64_t seek_to;           /* pending SetCurrentPosition */
    _Atomic uint32_t seek_serial;
    uint32_t seek_seen;                 /* render-only */
    double position;                    /* render-only, fractional source frame */
    _Atomic int playing, loop, used;    /* used: 0 free, 1 live, 2 reserved, 3 dying */
    _Atomic uint32_t publishes;
    _Atomic uint32_t stale_reset;       /* bumped by every publish */
    _Atomic uint32_t stale_frames;      /* render snapshot, SOURCE frames */
    _Atomic int publishing;             /* one producer per voice */
    _Atomic int resetting;              /* control-path reinit in progress */
    double stale_source;                /* render-only, nfsu2 staleFrames */
    uint32_t reset_seen;                /* render-only */
} Wc3Voice;

typedef struct Wc3PcmMixer {
    Wc3Voice voices[WC3_MIXER_VOICES];
    Wc3SincKernel kernel;
    uint32_t rate;             /* actual AAudio device rate */
    uint32_t channels;
    int float_output;          /* AAUDIO_FORMAT_PCM_FLOAT else PCM_I16 */
    _Atomic uint32_t render_users;   /* voices must not be freed while > 0 */
    _Atomic uint32_t peak_bits;      /* float bits, exchanged by stats */
    _Atomic uint64_t clipped;
    _Atomic uint32_t callback_peak_us;
} Wc3PcmMixer;

static inline int wc3_mixer_init(Wc3PcmMixer *m, uint32_t device_rate,
                                 uint32_t channels, int float_output)
{
    if (!m || device_rate == 0u || channels == 0u) return -1;
    memset(m, 0, sizeof *m);
    for (unsigned v = 0; v < WC3_MIXER_VOICES; v++) {
        Wc3Voice *voice = &m->voices[v];
        atomic_init(&voice->frequency, 0u);
        atomic_init(&voice->left_bits, wc3_float_bits(1.0f));
        atomic_init(&voice->right_bits, wc3_float_bits(1.0f));
        atomic_init(&voice->published_frame, 0u);
        atomic_init(&voice->play_frame, 0u);
        atomic_init(&voice->seek_to, 0u);
        atomic_init(&voice->seek_serial, 0u);
        atomic_init(&voice->playing, 0);
        atomic_init(&voice->loop, 0);
        atomic_init(&voice->used, 0);
        atomic_init(&voice->publishes, 0u);
        atomic_init(&voice->stale_reset, 0u);
        atomic_init(&voice->stale_frames, 0u);
        atomic_init(&voice->publishing, 0);
        atomic_init(&voice->resetting, 0);
    }
    atomic_init(&m->render_users, 0u);
    atomic_init(&m->peak_bits, wc3_float_bits(0.0f));
    atomic_init(&m->clipped, 0u);
    atomic_init(&m->callback_peak_us, 0u);
    m->rate = device_rate;
    m->channels = channels;
    m->float_output = float_output;
    wc3_sinc_init(&m->kernel);
    return 0;
}

/* Call only after the real-time callback has stopped.  PCM buffers belong to
 * the platform and are freed by it; this releases the seqlock arrays. */
static inline void wc3_mixer_deinit(Wc3PcmMixer *m)
{
    if (!m) return;
    for (unsigned v = 0; v < WC3_MIXER_VOICES; v++) {
        Wc3Voice *voice = &m->voices[v];
        free(voice->seq);
        voice->seq = NULL;
        voice->pcm = NULL;
        atomic_store_explicit(&voice->used, 0, memory_order_release);
    }
}

/* The caller owns the PCM buffer for the life of the voice.  Returns a
 * 1-based handle so 0 can never be a valid guest-visible voice id. */
static inline int wc3_mixer_voice_add(Wc3PcmMixer *m, uint8_t *pcm,
                                      uint32_t bytes, uint32_t rate,
                                      uint32_t channels, uint32_t bits,
                                      uintptr_t *out_voice)
{
    if (!m || !pcm || !out_voice || rate == 0u || channels == 0u ||
        m->rate == 0u || (bits != 8u && bits != 16u)) return -1;
    uint32_t bpf = channels * (bits / 8u);
    if (bpf == 0u || (WC3_VOICE_SLOT_BYTES % bpf) != 0u ||
        bytes < bpf || (bytes % bpf) != 0u) return -1;
    uint32_t slots = (bytes + WC3_VOICE_SLOT_BYTES - 1u) /
                     WC3_VOICE_SLOT_BYTES;
    if (slots == 0u || slots > WC3_VOICE_MAX_SLOTS) return -1;

    for (unsigned i = 0; i < WC3_MIXER_VOICES; i++) {
        Wc3Voice *v = &m->voices[i];
        int expected = 0;
        if (!atomic_compare_exchange_strong_explicit(
                &v->used, &expected, 2, memory_order_acq_rel,
                memory_order_relaxed))
            continue;

        _Atomic uint32_t *seq = (_Atomic uint32_t *)calloc(
            slots, sizeof *seq);
        if (!seq) {
            atomic_store_explicit(&v->used, 0, memory_order_release);
            return -1;
        }
        for (uint32_t s = 0; s < slots; s++)
            atomic_init(&seq[s], 0u);

        v->pcm = pcm;
        v->seq = seq;
        v->bytes = bytes;
        v->bpf = bpf;
        v->frames = bytes / bpf;
        v->slots = slots;
        v->rate = rate;
        v->channels = channels;
        v->bits = bits;
        v->position = 0.0;
        v->stale_source = 0.0;
        v->reset_seen = 0u;
        v->seek_seen = 0u;
        atomic_store_explicit(&v->frequency, rate, memory_order_relaxed);
        atomic_store_explicit(&v->left_bits, wc3_float_bits(1.0f),
                              memory_order_relaxed);
        atomic_store_explicit(&v->right_bits, wc3_float_bits(1.0f),
                              memory_order_relaxed);
        /* nfsu2 initializes an 8-bit shadow to 128 (unsigned silence), not 0;
         * a zero byte would decode as -1.0 and make an unpublished region
         * full-scale negative. */
        memset(pcm, bits == 8u ? 128 : 0,
               (size_t)slots * WC3_VOICE_SLOT_BYTES);
        atomic_store_explicit(&v->published_frame, 0u, memory_order_release);
        atomic_store_explicit(&v->play_frame, 0u, memory_order_release);
        atomic_store_explicit(&v->seek_to, 0u, memory_order_release);
        atomic_store_explicit(&v->seek_serial, 0u, memory_order_release);
        atomic_store_explicit(&v->publishes, 0u, memory_order_release);
        atomic_store_explicit(&v->stale_reset, 0u, memory_order_release);
        atomic_store_explicit(&v->stale_frames, 0u, memory_order_release);
        atomic_store_explicit(&v->publishing, 0, memory_order_release);
        atomic_store_explicit(&v->resetting, 0, memory_order_release);
        atomic_store_explicit(&v->playing, 0, memory_order_release);
        atomic_store_explicit(&v->loop, 0, memory_order_release);
        atomic_store_explicit(&v->used, 1, memory_order_release);
        *out_voice = (uintptr_t)(i + 1u);
        return 0;
    }
    return -1;
}

static inline Wc3Voice *wc3_mixer_voice(Wc3PcmMixer *m, uintptr_t handle)
{
    if (!m || handle == 0u || handle > WC3_MIXER_VOICES) return NULL;
    Wc3Voice *v = &m->voices[(size_t)handle - 1u];
    if (atomic_load_explicit(&v->used, memory_order_acquire) != 1) return NULL;
    return v;
}

/* Mark the voice dead.  used==3 keeps the slot out of new render snapshots
 * and out of wc3_mixer_voice_add(), while an in-flight callback may still hold
 * the old PCM.  The caller waits for render_users() == 0, then calls
 * wc3_mixer_voice_release() (which makes the slot reusable). */
static inline int wc3_mixer_voice_remove(Wc3PcmMixer *m, uintptr_t handle)
{
    Wc3Voice *v = wc3_mixer_voice(m, handle);
    if (!v) return -1;
    atomic_store_explicit(&v->playing, 0, memory_order_release);
    atomic_store_explicit(&v->used, 3, memory_order_release);
    return 0;
}

static inline uint32_t wc3_mixer_render_users(const Wc3PcmMixer *m)
{
    return m ? atomic_load_explicit(&m->render_users, memory_order_acquire)
             : 0u;
}

/* Quiescent release of the seqlock array.  PCM is still owned by the caller.
 * The slot becomes reusable only after the caller has observed
 * render_users() == 0. */
static inline void wc3_mixer_voice_release(Wc3Voice *v)
{
    if (!v) return;
    free(v->seq);
    v->seq = NULL;
    v->pcm = NULL;
    atomic_store_explicit(&v->used, 0, memory_order_release);
}

/* Copy one region into the circular PCM ring.  `publishing` is already held by
 * the caller.  The logical frame index wraps the physical buffer exactly like
 * nfsu2's PcmVoice::samples. */
static inline void wc3_voice_copy_locked(Wc3Voice *v, uint64_t frame_base,
                                         const uint8_t *src, uint32_t bytes)
{
    uint64_t pos = frame_base % v->frames;
    uint32_t left = bytes;
    while (left) {
        uint32_t byte_off = (uint32_t)(pos * v->bpf);
        uint32_t slot = byte_off / WC3_VOICE_SLOT_BYTES;
        uint32_t in_slot = byte_off % WC3_VOICE_SLOT_BYTES;
        uint32_t room = WC3_VOICE_SLOT_BYTES - in_slot;
        uint32_t until_end = v->bytes - byte_off;
        uint32_t n = left;
        if (n > room) n = room;
        if (n > until_end) n = until_end;
        if (n == 0u) break;
        /* Even -> odd -> write -> even.  The consumer never needs to know the
         * write generation: a stable even seqlock is a complete slot, and the
         * circular PCM buffer is itself the loop state (nfsu2 semantics). */
        atomic_fetch_add_explicit(&v->seq[slot], 1u, memory_order_acq_rel);
        memcpy(v->pcm + (size_t)slot * WC3_VOICE_SLOT_BYTES + in_slot,
               src, n);
        atomic_fetch_add_explicit(&v->seq[slot], 1u, memory_order_release);
        src += n;
        left -= n;
        pos = (pos + n / v->bpf) % v->frames;
    }
}

/* nfsu2 PcmVoice::published(): clear the stale window and count the publish
 * only while playing.  Pre-filling a paused buffer must not make it look like
 * a live streaming ring.  The caller holds `publishing`.  Clear stale BEFORE
 * publishing the count so a render that observes the new count also observes
 * the stale reset. */
static inline void wc3_voice_published_locked(Wc3Voice *v)
{
    atomic_fetch_add_explicit(&v->stale_reset, 1u, memory_order_release);
    atomic_store_explicit(&v->stale_frames, 0u, memory_order_release);
    if (atomic_load_explicit(&v->playing, memory_order_acquire))
        atomic_fetch_add_explicit(&v->publishes, 1u, memory_order_release);
}

/* Commit the copied regions.  One guest Unlock() is one publish even when the
 * locked range wraps the end of the buffer and physically touches two spans:
 * nfsu2's PcmVoice::published() is called once per Unlock. */
static inline void wc3_voice_commit_locked(Wc3Voice *v, uint64_t end_frame)
{
    uint64_t old = atomic_load_explicit(&v->published_frame,
                                        memory_order_relaxed);
    while (end_frame > old &&
           !atomic_compare_exchange_weak_explicit(&v->published_frame, &old,
                                                  end_frame,
                                                  memory_order_release,
                                                  memory_order_relaxed)) {}
    wc3_voice_published_locked(v);
    atomic_store_explicit(&v->publishing, 0, memory_order_release);
}

/* Publish ONLY the region the guest wrote, at an absolute SOURCE frame. */
static inline uint32_t wc3_voice_publish(Wc3Voice *v, uint64_t frame_base,
                                         const void *frames, uint32_t bytes)
{
    const uint8_t *src = (const uint8_t *)frames;
    if (!v || !src || bytes == 0u || v->frames == 0u ||
        (bytes % v->bpf) != 0u) return 0u;
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &v->publishing, &expected, 1, memory_order_acq_rel,
            memory_order_relaxed))
        return 0u;
    /* The control path may have marked the voice dying after our caller's
     * lookup.  Reading used after winning `publishing` closes the release
     * race: voice_remove() waits for publishing before freeing seq. */
    if (atomic_load_explicit(&v->used, memory_order_acquire) != 1 ||
        atomic_load_explicit(&v->resetting, memory_order_acquire) ||
        !v->pcm || !v->seq) {
        atomic_store_explicit(&v->publishing, 0, memory_order_release);
        return 0u;
    }
    wc3_voice_copy_locked(v, frame_base, src, bytes);
    wc3_voice_commit_locked(v, frame_base + bytes / v->bpf);
    return bytes;
}

/* One Unlock() spanning the physical end of the ring: both guest regions are
 * copied under one publishing claim and count as one publish.  `frame_base2`
 * is the ABSOLUTE continuation of the first region; because it maps to
 * physical frame 0 by construction, the ring wrap is handled by the frame
 * arithmetic instead of inventing a second guest write. */
static inline uint32_t wc3_voice_publish2(Wc3Voice *v,
                                          uint64_t frame_base1,
                                          const void *frames1, uint32_t bytes1,
                                          uint64_t frame_base2,
                                          const void *frames2, uint32_t bytes2)
{
    const uint8_t *src1 = (const uint8_t *)frames1;
    const uint8_t *src2 = (const uint8_t *)frames2;
    if (!v || v->frames == 0u || (bytes1 == 0u && bytes2 == 0u)) return 0u;
    if ((bytes1 != 0u && !src1) || (bytes2 != 0u && !src2)) return 0u;
    if ((bytes1 % v->bpf) != 0u || (bytes2 % v->bpf) != 0u) return 0u;
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &v->publishing, &expected, 1, memory_order_acq_rel,
            memory_order_relaxed))
        return 0u;
    if (atomic_load_explicit(&v->used, memory_order_acquire) != 1 ||
        atomic_load_explicit(&v->resetting, memory_order_acquire) ||
        !v->pcm || !v->seq) {
        atomic_store_explicit(&v->publishing, 0, memory_order_release);
        return 0u;
    }
    uint64_t end1 = frame_base1 + bytes1 / v->bpf;
    uint64_t end2 = frame_base2 + bytes2 / v->bpf;
    if (bytes1 != 0u) wc3_voice_copy_locked(v, frame_base1, src1, bytes1);
    if (bytes2 != 0u) wc3_voice_copy_locked(v, frame_base2, src2, bytes2);
    wc3_voice_commit_locked(v, end1 > end2 ? end1 : end2);
    return bytes1 + bytes2;
}

/* nfsu2 PcmVoice::published() for a zero-byte Unlock: no PCM moves, but the
 * stale window is cleared and, while playing, the publish count advances.
 * Exactness matters because streaming() is `loop && publishes >= 4`. */
static inline int wc3_voice_published(Wc3Voice *v)
{
    if (!v || atomic_load_explicit(&v->used, memory_order_acquire) != 1)
        return -1;
    /* published() is called under the nfsu2 mixer mutex.  Claim the same
     * per-voice producer gate so it cannot interleave with a real publish or
     * a reset; a concurrent Unlock on one buffer is not valid guest use. */
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &v->publishing, &expected, 1, memory_order_acq_rel,
            memory_order_relaxed))
        return -1;
    if (atomic_load_explicit(&v->used, memory_order_acquire) != 1 ||
        atomic_load_explicit(&v->resetting, memory_order_acquire)) {
        atomic_store_explicit(&v->publishing, 0, memory_order_release);
        return -1;
    }
    wc3_voice_published_locked(v);
    atomic_store_explicit(&v->publishing, 0, memory_order_release);
    return 0;
}

/* waveOutReset(): discard every queued/old sample without freeing or
 * reallocating the voice.  The caller must first set `resetting` (render skips
 * the voice), wait render_users()==0 and publishing==0, and keep the producer
 * side serialized.  The layout is immutable, so a reset is just "silence +
 * cursor zero + publish generation zero". */
static inline int wc3_voice_reset(Wc3Voice *v)
{
    if (!v || !v->pcm || !v->seq || v->frames == 0u) return -1;
    /* voice_remove() may have marked the slot dying after our platform lookup. */
    if (atomic_load_explicit(&v->used, memory_order_acquire) != 1) return -1;
    atomic_store_explicit(&v->playing, 0, memory_order_release);
    atomic_store_explicit(&v->published_frame, 0u, memory_order_release);
    atomic_store_explicit(&v->play_frame, 0u, memory_order_release);
    atomic_store_explicit(&v->seek_to, 0u, memory_order_release);
    atomic_fetch_add_explicit(&v->seek_serial, 1u, memory_order_release);
    atomic_store_explicit(&v->publishes, 0u, memory_order_release);
    atomic_fetch_add_explicit(&v->stale_reset, 1u, memory_order_release);
    atomic_store_explicit(&v->stale_frames, 0u, memory_order_release);
    v->position = 0.0;
    v->stale_source = 0.0;
    v->seek_seen = atomic_load_explicit(&v->seek_serial,
                                        memory_order_relaxed);
    v->reset_seen = atomic_load_explicit(&v->stale_reset,
                                         memory_order_relaxed);
    /* 8-bit PCM is unsigned: 128 is silence, 0 is full-scale negative. */
    memset(v->pcm, v->bits == 8u ? 128 : 0,
           (size_t)v->slots * WC3_VOICE_SLOT_BYTES);
    for (uint32_t s = 0; s < v->slots; s++)
        atomic_store_explicit(&v->seq[s], 0u, memory_order_release);
    return 0;
}

/* One 16-bit sample.  Stale / unpublished / in-flight slots are silence. */
static inline int16_t wc3_voice_sample(const Wc3Voice *v, uint64_t abs,
                                       unsigned channel)
{
    if (!v || !v->pcm || !v->seq || v->frames == 0u) return 0;
    uint64_t pub = atomic_load_explicit(&v->published_frame,
                                        memory_order_acquire);
    int loop = atomic_load_explicit(&v->loop, memory_order_relaxed);
    if (loop) {
        if (pub == 0u) return 0;
    } else if (abs >= pub) {
        return 0;
    }
    uint64_t pos = loop ? abs % v->frames : abs;
    if (pos >= v->frames) return 0;
    uint32_t byte_off = (uint32_t)(pos * v->bpf);
    uint32_t slot = byte_off / WC3_VOICE_SLOT_BYTES;
    uint32_t in_slot = byte_off % WC3_VOICE_SLOT_BYTES;
    uint32_t s1 = atomic_load_explicit(&v->seq[slot], memory_order_acquire);
    if (s1 & 1u) return 0;
    unsigned ch = channel < v->channels ? channel : v->channels - 1u;
    const uint8_t *p = v->pcm + (size_t)slot * WC3_VOICE_SLOT_BYTES +
                       in_slot + (size_t)ch * (v->bits / 8u);
    int16_t value;
    if (v->bits == 8u) value = (int16_t)(((int)p[0] - 128) << 8);
    else { uint16_t w = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
           memcpy(&value, &w, sizeof value); }
    uint32_t s2 = atomic_load_explicit(&v->seq[slot], memory_order_acquire);
    return (s1 == s2) ? value : 0;
}

static inline void wc3_voice_control(Wc3Voice *v, int playing, int loop,
                                     uint32_t frequency, int volume, int pan)
{
    if (!v) return;
    if (playing == 1) {
        /* nfsu2 Play(): a fresh start clears the stale counter.  Publish the
         * reset BEFORE playing=1 so a render that observes the new playing
         * state also observes stale_source=0 (release/acquire pairing). */
        atomic_fetch_add_explicit(&v->stale_reset, 1u, memory_order_release);
        atomic_store_explicit(&v->stale_frames, 0u, memory_order_release);
    }
    /* loop is visible before playing: a render that acquires playing==1 also
     * sees the loop flag that belongs to this Play/Stop transition. */
    if (loop >= 0)
        atomic_store_explicit(&v->loop, loop, memory_order_release);
    if (playing >= 0)
        atomic_store_explicit(&v->playing, playing, memory_order_release);
    uint32_t freq = frequency ? frequency : v->rate;
    if (freq) atomic_store_explicit(&v->frequency, freq, memory_order_relaxed);
    if (volume > 0) volume = 0;
    if (volume < -10000) volume = -10000;
    if (pan < -10000) pan = -10000;
    if (pan > 10000) pan = 10000;
    float gain = wc3_gain_from_attenuation((double)(-volume) / 2000.0);
    float left = gain * (pan > 0
        ? wc3_gain_from_attenuation((double)pan / 2000.0) : 1.0f);
    float right = gain * (pan < 0
        ? wc3_gain_from_attenuation((double)(-pan) / 2000.0) : 1.0f);
    atomic_store_explicit(&v->left_bits, wc3_float_bits(left),
                          memory_order_relaxed);
    atomic_store_explicit(&v->right_bits, wc3_float_bits(right),
                          memory_order_relaxed);
}

/* DirectSound SetCurrentPosition / seek.  The render callback applies it at
 * its next chunk, so the control path never writes render-owned state. */
static inline int wc3_voice_seek(Wc3Voice *v, uint64_t abs_frame)
{
    if (!v) return -1;
    atomic_store_explicit(&v->seek_to, abs_frame, memory_order_relaxed);
    atomic_fetch_add_explicit(&v->seek_serial, 1u, memory_order_release);
    atomic_fetch_add_explicit(&v->stale_reset, 1u, memory_order_release);
    atomic_store_explicit(&v->stale_frames, 0u, memory_order_release);
    atomic_store_explicit(&v->play_frame, abs_frame, memory_order_release);
    return 0;
}

static inline int wc3_voice_state(const Wc3Voice *v, uint32_t *playing,
                                  uint32_t *loop, uint64_t *position)
{
    if (!v) return -1;
    if (playing) *playing = (uint32_t)atomic_load_explicit(
        &v->playing, memory_order_acquire);
    if (loop) *loop = (uint32_t)atomic_load_explicit(
        &v->loop, memory_order_acquire);
    if (position) *position = atomic_load_explicit(
        &v->play_frame, memory_order_acquire);
    return 0;
}

/* DirectSound GetCurrentPosition: play cursor and the 10 ms write cursor,
 * measured in DEVICE time (nfsu2 uses soundMixer.rate here), both modulo the
 * looping buffer, plus the absolute source-frame cursor. */
static inline void wc3_voice_position(const Wc3Voice *v, uint32_t device_rate,
                                      uint32_t *play_bytes,
                                      uint32_t *write_bytes,
                                      uint64_t *play_frame)
{
    if (!v) {
        if (play_bytes) *play_bytes = 0u;
        if (write_bytes) *write_bytes = 0u;
        if (play_frame) *play_frame = 0u;
        return;
    }
    uint64_t pf = atomic_load_explicit(&v->play_frame, memory_order_acquire);
    if (v->frames == 0u || v->bytes == 0u) {
        if (play_bytes) *play_bytes = 0u;
        if (write_bytes) *write_bytes = 0u;
        if (play_frame) *play_frame = pf;
        return;
    }
    uint32_t play = (uint32_t)((pf % v->frames) * v->bpf);
    uint32_t ahead = ((device_rate ? device_rate : v->rate) / 100u) * v->bpf;
    if (ahead == 0u) ahead = v->bpf;
    if (play_bytes) *play_bytes = play;
    if (write_bytes) *write_bytes = (uint32_t)((play + ahead) % v->bytes);
    if (play_frame) *play_frame = pf;
}

/* nfsu2 serviceGuestSound(): non-RT health snapshot.  Futures are exchanged
 * so each report covers the interval since the previous one. */
static inline void wc3_mixer_stats(Wc3PcmMixer *m, uint32_t *active,
                                   uint64_t *clipped, float *peak,
                                   uint32_t *callback_peak_us)
{
    if (!m) return;
    uint32_t count = 0u;
    for (unsigned i = 0; i < WC3_MIXER_VOICES; i++) {
        const Wc3Voice *v = &m->voices[i];
        if (atomic_load_explicit(&v->used, memory_order_relaxed) == 1 &&
            atomic_load_explicit(&v->playing, memory_order_relaxed))
            count++;
    }
    if (active) *active = count;
    if (clipped) *clipped = atomic_exchange_explicit(
        &m->clipped, 0u, memory_order_relaxed);
    if (peak) *peak = wc3_bits_float(atomic_exchange_explicit(
        &m->peak_bits, wc3_float_bits(0.0f), memory_order_relaxed));
    if (callback_peak_us) *callback_peak_us = atomic_exchange_explicit(
        &m->callback_peak_us, 0u, memory_order_relaxed);
}

/* Mix `frames` device frames.  Returns the byte count written. */
static inline uint32_t wc3_mixer_render(Wc3PcmMixer *m, void *dst,
                                        uint32_t frames)
{
    if (!m || !dst || frames == 0u || m->rate == 0u) return 0u;
    atomic_fetch_add_explicit(&m->render_users, 1u, memory_order_acq_rel);

    unsigned active[WC3_MIXER_VOICES];
    unsigned nactive = 0u;
    for (unsigned i = 0; i < WC3_MIXER_VOICES; i++) {
        if (atomic_load_explicit(&m->voices[i].used,
                                 memory_order_acquire) == 1 &&
            !atomic_load_explicit(&m->voices[i].resetting,
                                  memory_order_acquire))
            active[nactive++] = i;
    }

    const unsigned out_ch = m->channels;
    /* Always compute a stereo mix: a mono device is a (L+R)/2 downmix at the
     * output stage, not "take only the left channel".  A mono voice is
     * duplicated by wc3_voice_sample(), so this is also correct there. */
    const unsigned mix_ch = 2u;
    uint8_t *out = (uint8_t *)dst;
    uint64_t clipped = 0u;
    float peak = 0.0f;
    uint32_t done = 0u;

    while (done < frames) {
        uint32_t n = frames - done;
        if (n > WC3_MIX_CHUNK) n = WC3_MIX_CHUNK;
        float mix[WC3_MIX_CHUNK * 2u];
        memset(mix, 0, sizeof mix);

        for (unsigned k = 0; k < nactive; k++) {
            Wc3Voice *v = &m->voices[active[k]];
            /* Acquire: Play publishes stale_reset and loop before playing. */
            if (atomic_load_explicit(&v->playing, memory_order_acquire) == 0)
                continue;

            uint32_t seek = atomic_load_explicit(&v->seek_serial,
                                                 memory_order_acquire);
            if (seek != v->seek_seen) {
                v->seek_seen = seek;
                v->position = (double)atomic_load_explicit(
                    &v->seek_to, memory_order_relaxed);
                v->stale_source = 0.0;
            }
            uint32_t publishes = atomic_load_explicit(
                &v->publishes, memory_order_acquire);
            uint32_t reset = atomic_load_explicit(&v->stale_reset,
                                                  memory_order_relaxed);
            if (reset != v->reset_seen) {
                v->reset_seen = reset;
                v->stale_source = 0.0;
            }

            int loop = atomic_load_explicit(&v->loop, memory_order_relaxed);
            uint32_t frequency = atomic_load_explicit(
                &v->frequency, memory_order_relaxed);
            if (!frequency) frequency = v->rate;
            double step = (double)frequency / (double)m->rate;
            int streaming = loop && publishes >= 4u;
            int muted = streaming && v->stale_source >= (double)v->frames;
            float gl = wc3_bits_float(atomic_load_explicit(
                &v->left_bits, memory_order_relaxed));
            float gr = wc3_bits_float(atomic_load_explicit(
                &v->right_bits, memory_order_relaxed));

            for (uint32_t f = 0; f < n; f++) {
                if (!loop && v->position >= (double)v->frames) {
                    atomic_store_explicit(&v->playing, 0,
                                          memory_order_release);
                    v->position = 0.0;
                    v->stale_source = 0.0;
                    break;
                }
                if (!muted) {
                    uint64_t a = (uint64_t)v->position;
                    double frac = v->position - (double)a;
                    if (step < 1.0) {
                        double phase = frac * (double)WC3_SINC_PHASES;
                        unsigned index = (unsigned)phase;
                        if (index >= WC3_SINC_PHASES)
                            index = WC3_SINC_PHASES - 1u;
                        float blend = (float)(phase - (double)index);
                        for (unsigned ch = 0; ch < mix_ch; ch++) {
                            float sum = 0.0f;
                            for (unsigned t = 0; t < WC3_SINC_TAPS; t++) {
                                int64_t src = (int64_t)a + (int)t - 7;
                                if (src < 0 || src >= (int64_t)v->frames) {
                                    if (loop) {
                                        src %= (int64_t)v->frames;
                                        if (src < 0)
                                            src += (int64_t)v->frames;
                                    } else {
                                        src = src < 0
                                            ? 0
                                            : (int64_t)v->frames - 1;
                                    }
                                }
                                float w = m->kernel.weights[index][t] +
                                    (m->kernel.weights[index + 1u][t] -
                                     m->kernel.weights[index][t]) * blend;
                                int16_t s = wc3_voice_sample(
                                    v, (uint64_t)src, ch);
                                sum += ((float)s / 32768.0f) * w;
                            }
                            mix[f * 2u + ch] += sum * (ch ? gr : gl);
                        }
                    } else {
                        uint64_t a1 = a + 1u;
                        if (!loop && a1 >= v->frames) a1 = v->frames - 1u;
                        for (unsigned ch = 0; ch < mix_ch; ch++) {
                            int16_t s0 = wc3_voice_sample(v, a, ch);
                            int16_t s1 = wc3_voice_sample(v, a1, ch);
                            float s = (float)s0 / 32768.0f +
                                ((float)s1 - (float)s0) / 32768.0f *
                                (float)frac;
                            mix[f * 2u + ch] += s * (ch ? gr : gl);
                        }
                    }
                }
                v->position += step;
                if (!muted) v->stale_source += step;
            }

            uint32_t seek_after = atomic_load_explicit(&v->seek_serial,
                                                       memory_order_acquire);
            if (seek_after == v->seek_seen) {
                atomic_store_explicit(&v->play_frame, (uint64_t)v->position,
                                      memory_order_release);
                /* A seek that landed between the check and the store must not
                 * be lost: put its target (the latest one) back.  The next
                 * chunk applies it to the mixer position. */
                if (atomic_load_explicit(&v->seek_serial,
                                         memory_order_acquire) != v->seek_seen)
                    atomic_store_explicit(
                        &v->play_frame,
                        atomic_load_explicit(&v->seek_to,
                                             memory_order_relaxed),
                        memory_order_release);
            }
            if (atomic_load_explicit(&v->stale_reset,
                                     memory_order_relaxed) == v->reset_seen)
                atomic_store_explicit(&v->stale_frames,
                                      v->stale_source > 4294967295.0
                                          ? 4294967295u
                                          : (uint32_t)v->stale_source,
                                      memory_order_relaxed);
        }

        for (uint32_t f = 0; f < n; f++) {
            float left = mix[f * 2u];
            float right = mix[f * 2u + 1u];
            for (unsigned ch = 0; ch < out_ch; ch++) {
                float x;
                if (out_ch == 1u) x = (left + right) * 0.5f;
                else x = ch < 2u ? (ch ? right : left) : 0.0f;
                float ax = x < 0.0f ? -x : x;
                if (ax > peak) peak = ax;
                if (ax > 1.0f) {
                    clipped++;
                    x = x < 0.0f ? -1.0f : 1.0f;
                }
                if (m->float_output) {
                    size_t off = (size_t)(done + f) * out_ch + ch;
                    memcpy(out + off * sizeof x, &x, sizeof x);
                } else {
                    int16_t s = (int16_t)(x * 32767.0f);
                    size_t off = (size_t)(done + f) * out_ch + ch;
                    memcpy(out + off * sizeof s, &s, sizeof s);
                }
            }
        }
        done += n;
    }

    if (clipped)
        atomic_fetch_add_explicit(&m->clipped, clipped, memory_order_relaxed);
    uint32_t old_peak = atomic_load_explicit(&m->peak_bits,
                                             memory_order_relaxed);
    while (peak > wc3_bits_float(old_peak) &&
           !atomic_compare_exchange_weak_explicit(
               &m->peak_bits, &old_peak, wc3_float_bits(peak),
               memory_order_relaxed, memory_order_relaxed)) {}
    atomic_fetch_sub_explicit(&m->render_users, 1u, memory_order_acq_rel);
    uint32_t bpf = m->float_output ? out_ch * 4u : out_ch * 2u;
    return frames * bpf;
}

#endif /* WC3_PLATFORM_AUDIO_MIXER_H */
