/*
 * null_internal.h - private layout shared by the null frontend translation
 * units.  Nothing here is part of the recorder contract.
 */
#ifndef WC3_NULL_INTERNAL_H
#define WC3_NULL_INTERNAL_H

#include "null_gfx.h"

#include <stddef.h>

#define WC3_NULL_GFX_MAX_RESOURCES 256u
#define WC3_NULL_GFX_MAX_INTERVALS 64u
#define WC3_NULL_GFX_HASH_OFFSET 14695981039346656037ull
#define WC3_NULL_GFX_HASH_PRIME 1099511628211ull

typedef struct Wc3NullGfxInterval {
    uint32_t lo;
    uint32_t hi;
    uint32_t nonzero;
} Wc3NullGfxInterval;

typedef struct Wc3NullGfxResource {
    uint32_t id;
    uint32_t size;
    uint32_t stride;
    uint32_t kind;
    uint32_t used;
    uint32_t coverage_unknown;
    uint32_t interval_count;
    uint32_t active;
    uint32_t lock_offset;
    uint32_t lock_size;
    uint32_t lock_flags;
    Wc3NullGfxInterval interval[WC3_NULL_GFX_MAX_INTERVALS];
} Wc3NullGfxResource;

struct Wc3NullGfx {
    uint32_t degenerate_ratio_ppm;

    uint64_t stream_hash;
    uint64_t state_hash;
    uint64_t event_count;

    uint32_t draw_count;
    uint32_t locked_range_failures;
    uint32_t declared_range_failures;
    uint32_t degenerate_ratio_failures;
    uint32_t coverage_unknown;
    uint32_t index_unavailable;
    uint64_t primitive_total;
    uint64_t degenerate_total;

    Wc3NullGfxResource resource[WC3_NULL_GFX_MAX_RESOURCES];
};

static inline uint64_t wc3_null_hash_u8(uint64_t h, uint8_t v)
{
    return (h ^ (uint64_t)v) * WC3_NULL_GFX_HASH_PRIME;
}

static inline uint64_t wc3_null_hash_u16(uint64_t h, uint16_t v)
{
    h = wc3_null_hash_u8(h, (uint8_t)(v & 0xFFu));
    return wc3_null_hash_u8(h, (uint8_t)(v >> 8));
}

static inline uint64_t wc3_null_hash_u32(uint64_t h, uint32_t v)
{
    h = wc3_null_hash_u16(h, (uint16_t)(v & 0xFFFFu));
    return wc3_null_hash_u16(h, (uint16_t)(v >> 16));
}

static inline uint64_t wc3_null_hash_u64(uint64_t h, uint64_t v)
{
    h = wc3_null_hash_u32(h, (uint32_t)(v & 0xFFFFFFFFu));
    return wc3_null_hash_u32(h, (uint32_t)(v >> 32));
}

#endif /* WC3_NULL_INTERNAL_H */
