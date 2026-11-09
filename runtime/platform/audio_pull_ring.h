/*
 * audio_pull_ring.h - platform-neutral host-owned PCM ring for S-1.
 *
 * Derived from docs/AUDIO_PULL_IMPLEMENTATION_SPEC.md (I-2/I-3/I-7/I-8).
 * This header is deliberately free of OS headers so the verification DUT and
 * the Android sink can share exactly one implementation, and a host T0 test
 * can exercise it without linking a platform source file.
 *
 * The producer (HLE waveOutWrite) publishes ONLY the region it wrote, at an
 * absolute frame index.  The consumer (AAudio real-time callback, or the
 * headless virtual clock) renders by absolute frame index.  Each 1 KiB slot
 * carries a seqlock generation: odd = a producer is writing, even = stable.
 * A slot whose generation is older than the frame being rendered produces
 * silence; stale PCM is never replayed and no mute heuristic changes content.
 *
 * No locks, no allocation, no logging and no guest memory on the render path.
 */
#ifndef WC3_PLATFORM_AUDIO_PULL_RING_H
#define WC3_PLATFORM_AUDIO_PULL_RING_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WC3_PULL_SLOT_BYTES  1024u
#define WC3_PULL_SLOTS       64u
#define WC3_PULL_RING_BYTES  (WC3_PULL_SLOTS * WC3_PULL_SLOT_BYTES)
#define WC3_PULL_HASH_INIT   1469598103934665603ull

_Static_assert(WC3_PULL_RING_BYTES == 65536u, "pull ring stays 64 KiB");
_Static_assert((WC3_PULL_SLOTS & (WC3_PULL_SLOTS - 1u)) == 0u,
               "slot count must be a power of two");

typedef struct Wc3PullRing {
    uint8_t *pcm;                 /* caller owns WC3_PULL_RING_BYTES bytes */
    _Atomic uint32_t slot_seq[WC3_PULL_SLOTS];
    _Atomic uint64_t play_frame;       /* absolute frame the device renders */
    _Atomic uint64_t published_frame;  /* absolute frame after last publish */
    _Atomic uint64_t consumed;         /* device clock in bytes, monotone */
    _Atomic uint32_t stale_frames;
    _Atomic uint32_t publish_regions;
    _Atomic uint32_t render_calls;
    uint32_t bytes_per_frame;
    uint32_t frames_per_slot;         /* WC3_PULL_SLOT_BYTES / bytes_per_frame */
} Wc3PullRing;

/* Even values are stable generations.  Zero means "never published" and is
 * older than any expected generation (gen 0 expects 2). */
static inline uint32_t wc3_pull_stable_seq(uint64_t gen)
{
    return (uint32_t)(2u * (gen + 1u));
}

static inline uint32_t wc3_pull_writing_seq(uint64_t gen)
{
    return wc3_pull_stable_seq(gen) + 1u;
}

static inline int wc3_pull_ring_init(Wc3PullRing *r, uint8_t *buffer,
                                     uint32_t bytes_per_frame)
{
    if (!r || !buffer || bytes_per_frame == 0u ||
        (WC3_PULL_SLOT_BYTES % bytes_per_frame) != 0u)
        return -1;
    r->pcm = buffer;
    r->bytes_per_frame = bytes_per_frame;
    r->frames_per_slot = WC3_PULL_SLOT_BYTES / bytes_per_frame;
    for (unsigned i = 0; i < WC3_PULL_SLOTS; i++)
        atomic_init(&r->slot_seq[i], 0u);
    atomic_init(&r->play_frame, 0u);
    atomic_init(&r->published_frame, 0u);
    atomic_init(&r->consumed, 0u);
    atomic_init(&r->stale_frames, 0u);
    atomic_init(&r->publish_regions, 0u);
    atomic_init(&r->render_calls, 0u);
    memset(buffer, 0, WC3_PULL_RING_BYTES);
    return 0;
}

/* Publish exactly one contiguous region.  Returns the accepted byte count,
 * or 0 on a malformed request.  A region may straddle a slot boundary. */
static inline uint32_t wc3_pull_publish(Wc3PullRing *r, uint64_t frame_base,
                                        const void *frames, uint32_t bytes)
{
    const uint8_t *src = (const uint8_t *)frames;
    uint32_t bpf, left;
    uint64_t frame;

    if (!r || !r->pcm || !src || bytes == 0u) return 0u;
    bpf = r->bytes_per_frame;
    if (bpf == 0u || (bytes % bpf) != 0u) return 0u;

    frame = frame_base;
    left = bytes;
    while (left) {
        uint64_t gen = frame / r->frames_per_slot;
        uint32_t slot = (uint32_t)(gen & (uint64_t)(WC3_PULL_SLOTS - 1u));
        uint32_t off = (uint32_t)(frame % r->frames_per_slot);
        uint32_t room = WC3_PULL_SLOT_BYTES - off * bpf;
        uint32_t n = left < room ? left : room;
        atomic_store_explicit(&r->slot_seq[slot],
                              wc3_pull_writing_seq(gen),
                              memory_order_release);
        memcpy(r->pcm + (size_t)slot * WC3_PULL_SLOT_BYTES +
                   (size_t)off * bpf, src, n);
        atomic_store_explicit(&r->slot_seq[slot],
                              wc3_pull_stable_seq(gen),
                              memory_order_release);
        src += n;
        left -= n;
        frame += n / bpf;
    }
    atomic_store_explicit(&r->published_frame, frame, memory_order_release);
    atomic_fetch_add_explicit(&r->publish_regions, 1u, memory_order_relaxed);
    return bytes;
}

/* Render num_frames into dst (num_frames * bytes_per_frame bytes).
 * Always advances the device clock by the full request, including underrun
 * frames rendered as silence (I-3).  Returns the byte count written. */
static inline uint32_t wc3_pull_render(Wc3PullRing *r, void *dst,
                                       uint32_t num_frames)
{
    uint8_t *out = (uint8_t *)dst;
    uint32_t bpf, stale = 0u;
    uint64_t play, published;

    if (!r || !r->pcm || !out || num_frames == 0u) return 0u;
    bpf = r->bytes_per_frame;
    if (bpf == 0u) return 0u;
    play = atomic_load_explicit(&r->play_frame, memory_order_relaxed);
    published = atomic_load_explicit(&r->published_frame,
                                     memory_order_acquire);

    for (uint32_t f = 0; f < num_frames; f++) {
        uint64_t abs = play + f;
        uint8_t *o = out + (size_t)f * bpf;
        /* A stable slot generation is not enough: the producer may have
         * published only part of that slot.  Frames at or beyond the
         * published boundary are underrun, never stale PCM. */
        if (abs >= published) {
            memset(o, 0, bpf);
            stale++;
            continue;
        }
        uint64_t gen = abs / r->frames_per_slot;
        uint32_t slot = (uint32_t)(gen & (uint64_t)(WC3_PULL_SLOTS - 1u));
        uint32_t off = (uint32_t)(abs % r->frames_per_slot);
        uint32_t want = wc3_pull_stable_seq(gen);
        uint32_t s1 = atomic_load_explicit(&r->slot_seq[slot],
                                           memory_order_acquire);
        if ((s1 & 1u) != 0u || s1 < want) {
            memset(o, 0, bpf);
            stale++;
        } else {
            memcpy(o, r->pcm + (size_t)slot * WC3_PULL_SLOT_BYTES +
                       (size_t)off * bpf, bpf);
            uint32_t s2 = atomic_load_explicit(&r->slot_seq[slot],
                                               memory_order_acquire);
            if (s1 != s2) {
                memset(o, 0, bpf);
                stale++;
            }
        }
    }

    atomic_store_explicit(&r->play_frame, play + num_frames,
                          memory_order_release);
    atomic_fetch_add_explicit(&r->consumed,
                              (uint64_t)num_frames * bpf,
                              memory_order_release);
    atomic_fetch_add_explicit(&r->stale_frames, stale,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&r->render_calls, 1u, memory_order_relaxed);
    return num_frames * bpf;
}

/* FNV-1a over rendered PCM, for the deterministic headless difftest. */
static inline uint64_t wc3_pull_hash(uint64_t hash, const void *data,
                                     size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < bytes; i++) {
        hash ^= (uint64_t)p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

#endif /* WC3_PLATFORM_AUDIO_PULL_RING_H */
