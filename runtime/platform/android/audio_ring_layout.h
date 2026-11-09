/*
 * audio_ring_layout.h - cache-line layout of the AAudio SPSC ring counters.
 *
 * PASS11 O-108 measured that read_pos and write_pos shared one 64-byte line.
 * The producer writes write_pos, the AAudio RT callback writes read_pos, and
 * each side reads the other counter.  Keeping both counters in one line makes
 * every callback invalidate the line in the other core.
 *
 * The counters stay plain 32-bit atomics.  A 64-byte offset between them is
 * enough even without an over-aligned allocation: the object is at least
 * 4-byte aligned, so a 4-byte field at offset 0 and one at offset 64 always
 * fall into different 64-byte lines.  The trailing pad keeps the fields after
 * this struct out of the producer's line as well.  This avoids changing the
 * allocator contract in android_audio_open() (calloc only guarantees normal
 * maximum alignment).
 *
 * The ring capacity is part of the same contract: O-107 sized it to 64 KiB
 * after the measured 43.8 KiB peak, and the power-of-two mask in
 * platform_android.c depends on that constant.
 */
#ifndef WC3_PLATFORM_ANDROID_AUDIO_RING_LAYOUT_H
#define WC3_PLATFORM_ANDROID_AUDIO_RING_LAYOUT_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define WC3_AUDIO_CACHE_LINE_BYTES 64u
#define WC3_AUDIO_RING_BYTES       (1u << 16)

_Static_assert(sizeof(_Atomic uint32_t) == 4u,
               "the ring positions require 32-bit atomics");
_Static_assert(WC3_AUDIO_RING_BYTES == 64u * 1024u,
               "O-107 fixes the platform ring at 64 KiB");
_Static_assert((WC3_AUDIO_RING_BYTES & (WC3_AUDIO_RING_BYTES - 1u)) == 0u,
               "the ring mask requires a power-of-two capacity");

typedef struct Wc3AudioRingPositions {
    _Atomic uint32_t read_pos;
    uint8_t _pad0[WC3_AUDIO_CACHE_LINE_BYTES - 4u];
    _Atomic uint32_t write_pos;
    uint8_t _pad1[WC3_AUDIO_CACHE_LINE_BYTES - 4u];
} Wc3AudioRingPositions;

_Static_assert(offsetof(Wc3AudioRingPositions, write_pos) -
                   offsetof(Wc3AudioRingPositions, read_pos) ==
               WC3_AUDIO_CACHE_LINE_BYTES,
               "read_pos and write_pos must not share a cache line");
_Static_assert(sizeof(Wc3AudioRingPositions) ==
                   WC3_AUDIO_CACHE_LINE_BYTES * 2u,
               "the trailing pad must isolate following fields");

#endif /* WC3_PLATFORM_ANDROID_AUDIO_RING_LAYOUT_H */
