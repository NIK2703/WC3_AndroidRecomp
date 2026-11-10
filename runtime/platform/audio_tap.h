/*
 * audio_tap.h - platform-neutral post-mix tap ring for N-16b diagnostics.
 *
 * The AAudio callback must never allocate, lock, log or touch guest memory.
 * This header is only the preallocated SPSC byte ring: the RT producer converts
 * the device-rate float/int16 buffer to interleaved s16 and pushes it; a normal
 * host thread pops and writes the debug fixture.  The ring is not a product
 * audio path and is never the DAC input.
 */
#ifndef WC3_PLATFORM_AUDIO_TAP_H
#define WC3_PLATFORM_AUDIO_TAP_H

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

typedef struct Wc3AudioTapRing {
    uint8_t *data;
    uint32_t capacity;              /* power of two bytes */
    _Atomic uint32_t read_pos;
    _Atomic uint32_t write_pos;
    _Atomic uint32_t dropped_bytes;
} Wc3AudioTapRing;

static inline int wc3_audio_tap_init(Wc3AudioTapRing *r, void *buffer,
                                     uint32_t capacity)
{
    if (!r || !buffer || capacity < 2u ||
        (capacity & (capacity - 1u)) != 0u ||
        capacity > 0x80000000u)
        return -1;
    r->data = (uint8_t *)buffer;
    r->capacity = capacity;
    atomic_init(&r->read_pos, 0u);
    atomic_init(&r->write_pos, 0u);
    atomic_init(&r->dropped_bytes, 0u);
    return 0;
}

static inline uint32_t wc3_audio_tap_dropped(const Wc3AudioTapRing *r)
{
    return r ? atomic_load_explicit(&r->dropped_bytes,
                                    memory_order_relaxed) : 0u;
}

/* Reserve a contiguous logical range.  The producer is the only writer of
 * write_pos, so a relaxed load is enough; the acquire read_pos load keeps the
 * consumer's release store visible. */
static inline int wc3_audio_tap_reserve(Wc3AudioTapRing *r, uint32_t bytes,
                                        uint32_t *pos)
{
    uint32_t write, read, used;
    if (!r || !pos || bytes == 0u) return -1;
    write = atomic_load_explicit(&r->write_pos, memory_order_relaxed);
    read = atomic_load_explicit(&r->read_pos, memory_order_acquire);
    used = write - read;
    if (used > r->capacity || bytes > r->capacity - used) {
        atomic_fetch_add_explicit(&r->dropped_bytes, bytes,
                                  memory_order_relaxed);
        return -1;
    }
    *pos = write;
    return 0;
}

static inline void wc3_audio_tap_commit(Wc3AudioTapRing *r, uint32_t pos,
                                        uint32_t bytes)
{
    atomic_store_explicit(&r->write_pos, pos + bytes, memory_order_release);
}

static inline uint16_t wc3_audio_tap_store_u16(uint8_t *dst, int16_t sample)
{
    uint16_t bits = (uint16_t)sample;
    memcpy(dst, &bits, sizeof bits);
    return bits;
}

static inline int16_t wc3_audio_tap_f32_to_s16(float sample)
{
    float scaled;
    int32_t value;
    if (!(sample == sample)) return 0;             /* NaN -> silence */
    if (sample <= -1.0f) return -32768;
    if (sample >= 1.0f) return 32767;
    scaled = sample * 32768.0f;
    value = (int32_t)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    return (int16_t)value;
}

/* Push interleaved s16.  Returns 0 on success, -1 when the ring is full. */
static inline int wc3_audio_tap_push_s16(Wc3AudioTapRing *r,
                                         const int16_t *samples,
                                         uint32_t count)
{
    uint32_t bytes, pos, off, first;
    if (!r || !samples || count == 0u) return count == 0u ? 0 : -1;
    if (count > r->capacity / 2u) {
        atomic_fetch_add_explicit(&r->dropped_bytes, r->capacity,
                                  memory_order_relaxed);
        return -1;
    }
    bytes = count * 2u;
    if (wc3_audio_tap_reserve(r, bytes, &pos) != 0) return -1;
    off = pos & (r->capacity - 1u);
    first = bytes < r->capacity - off ? bytes : r->capacity - off;
    memcpy(r->data + off, samples, first);
    if (bytes > first)
        memcpy(r->data, (const uint8_t *)samples + first, bytes - first);
    wc3_audio_tap_commit(r, pos, bytes);
    return 0;
}

/* Push device-rate float samples converted to interleaved s16.  The conversion
 * clamps and never calls libm; conversion is a diagnostic representation, not
 * the DAC output itself. */
static inline int wc3_audio_tap_push_f32(Wc3AudioTapRing *r,
                                         const float *samples,
                                         uint32_t count)
{
    uint32_t bytes, pos, off, first_samples, i;
    if (!r || !samples || count == 0u) return count == 0u ? 0 : -1;
    if (count > r->capacity / 2u) {
        atomic_fetch_add_explicit(&r->dropped_bytes, r->capacity,
                                  memory_order_relaxed);
        return -1;
    }
    bytes = count * 2u;
    if (wc3_audio_tap_reserve(r, bytes, &pos) != 0) return -1;
    off = pos & (r->capacity - 1u);
    first_samples = (r->capacity - off) / 2u;
    if (first_samples > count) first_samples = count;
    for (i = 0; i < first_samples; i++)
        wc3_audio_tap_store_u16(r->data + off + i * 2u,
                                wc3_audio_tap_f32_to_s16(samples[i]));
    for (; i < count; i++)
        wc3_audio_tap_store_u16(r->data + (i - first_samples) * 2u,
                                wc3_audio_tap_f32_to_s16(samples[i]));
    wc3_audio_tap_commit(r, pos, bytes);
    return 0;
}

/* Pop at most max_bytes.  The ring carries 16-bit samples, so an odd limit is
 * rounded down; this keeps every future producer offset sample-aligned. */
static inline uint32_t wc3_audio_tap_pop(Wc3AudioTapRing *r, void *dst,
                                         uint32_t max_bytes)
{
    uint32_t read, write, used, off, first;
    if (!r || !dst || max_bytes < 2u) return 0u;
    max_bytes &= ~1u;
    read = atomic_load_explicit(&r->read_pos, memory_order_acquire);
    write = atomic_load_explicit(&r->write_pos, memory_order_acquire);
    used = write - read;
    if (used == 0u) return 0u;
    if (used > max_bytes) used = max_bytes;
    off = read & (r->capacity - 1u);
    first = used < r->capacity - off ? used : r->capacity - off;
    memcpy(dst, r->data + off, first);
    if (used > first)
        memcpy((uint8_t *)dst + first, r->data, used - first);
    atomic_store_explicit(&r->read_pos, read + used, memory_order_release);
    return used;
}

#endif /* WC3_PLATFORM_AUDIO_TAP_H */
