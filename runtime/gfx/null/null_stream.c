/*
 * null_stream.c - canonical stream identity for the verification frontend.
 *
 * The stream carries no host pointer and no host clock.  The bridge is
 * responsible for passing guest VAs and float bit patterns as canonical
 * 32-bit arguments; this file only serialises those words with an explicit
 * little-endian encoding.
 */
#include "null_internal.h"

#include <stdlib.h>
#include <string.h>

#define WC3_NULL_STREAM_TAG 0x4E474658u /* "NGFX" */
#define WC3_NULL_STATE_TAG 0x53544154u  /* "STAT" */

Wc3NullGfx *wc3_null_gfx_create(uint32_t degenerate_ratio_ppm)
{
    Wc3NullGfx *gfx = (Wc3NullGfx *)calloc(1u, sizeof *gfx);
    if (!gfx) return NULL;
    gfx->degenerate_ratio_ppm = degenerate_ratio_ppm;
    wc3_null_gfx_reset(gfx);
    return gfx;
}

void wc3_null_gfx_destroy(Wc3NullGfx *gfx)
{
    free(gfx);
}

void wc3_null_gfx_reset(Wc3NullGfx *gfx)
{
    if (!gfx) return;
    const uint32_t ratio = gfx->degenerate_ratio_ppm;
    memset(gfx, 0, sizeof *gfx);
    gfx->degenerate_ratio_ppm = ratio;
    gfx->stream_hash = WC3_NULL_GFX_HASH_OFFSET;
    gfx->state_hash = WC3_NULL_GFX_HASH_OFFSET;
}

void wc3_null_gfx_reset_stream(Wc3NullGfx *gfx)
{
    if (!gfx) return;
    gfx->stream_hash = WC3_NULL_GFX_HASH_OFFSET;
    gfx->state_hash = WC3_NULL_GFX_HASH_OFFSET;
    gfx->event_count = 0u;
}

int wc3_null_gfx_emit(Wc3NullGfx *gfx, uint16_t iface, uint16_t method,
                      const uint32_t *args, uint16_t argc)
{
    if (!gfx || argc > WC3_NULL_GFX_MAX_ARGS) return -1;
    if (argc && !args) return -1;

    uint64_t h = gfx->stream_hash;
    h = wc3_null_hash_u32(h, WC3_NULL_STREAM_TAG);
    h = wc3_null_hash_u16(h, iface);
    h = wc3_null_hash_u16(h, method);
    h = wc3_null_hash_u16(h, argc);
    for (uint16_t i = 0; i < argc; i++)
        h = wc3_null_hash_u32(h, args[i]);
    h = wc3_null_hash_u64(h, gfx->state_hash);

    gfx->stream_hash = h;
    gfx->event_count++;
    return 0;
}

int wc3_null_gfx_state(Wc3NullGfx *gfx, uint32_t key, uint32_t value)
{
    if (!gfx) return -1;

    gfx->state_hash = wc3_null_hash_u32(gfx->state_hash, key);
    gfx->state_hash = wc3_null_hash_u32(gfx->state_hash, value);

    uint64_t h = gfx->stream_hash;
    h = wc3_null_hash_u32(h, WC3_NULL_STATE_TAG);
    h = wc3_null_hash_u32(h, key);
    h = wc3_null_hash_u32(h, value);
    gfx->stream_hash = h;
    return 0;
}

uint64_t wc3_null_gfx_stream_hash(const Wc3NullGfx *gfx)
{
    return gfx ? gfx->stream_hash : 0u;
}

uint64_t wc3_null_gfx_state_hash(const Wc3NullGfx *gfx)
{
    return gfx ? gfx->state_hash : 0u;
}

uint64_t wc3_null_gfx_event_count(const Wc3NullGfx *gfx)
{
    return gfx ? gfx->event_count : 0u;
}
