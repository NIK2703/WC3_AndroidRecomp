/*
 * null_invariants.c - V5.9 observations for the null D3D9 frontend.
 *
 * The helpers here are deliberately separate from the canonical stream.  The
 * bridge records the COM call first, then reports what the call did to a
 * vertex/index buffer or a draw.  A missing observation therefore cannot be
 * mistaken for a passing one: the affected check becomes UNAVAILABLE.
 *
 * The buffer model is intentionally bounded.  It tracks written intervals,
 * not a full shadow copy, so a draw that is not covered by a non-zero write
 * is a real failure regardless of whether the bytes happened to be zero in
 * guest memory at the moment of the draw.  When the fixed interval table
 * overflows, the check degrades to UNAVAILABLE instead of claiming success.
 */
#include "null_internal.h"

#include <string.h>

static Wc3NullGfxResource *resource_find(Wc3NullGfx *gfx, uint32_t id)
{
    if (!gfx || id == 0u) return NULL;
    for (uint32_t i = 0; i < WC3_NULL_GFX_MAX_RESOURCES; i++)
        if (gfx->resource[i].used && gfx->resource[i].id == id)
            return &gfx->resource[i];
    return NULL;
}

static int resource_range_ok(const Wc3NullGfxResource *res, uint64_t offset,
                             uint64_t size)
{
    return res && offset <= res->size && size <= (uint64_t)res->size - offset;
}

static void clear_coverage(Wc3NullGfxResource *res)
{
    res->interval_count = 0u;
    res->coverage_unknown = 0u;
}

static int record_write(Wc3NullGfxResource *res, uint32_t offset,
                        const void *data, uint32_t size)
{
    if (size == 0u) return 0;
    if (!res || !data) {
        if (res) res->coverage_unknown = 1u;
        return -1;
    }
    if (!resource_range_ok(res, offset, size)) {
        res->coverage_unknown = 1u;
        return -1;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t nonzero = 0u;
    for (uint32_t i = 0; i < size; i++) {
        if (bytes[i] != 0u) {
            nonzero = 1u;
            break;
        }
    }

    const uint32_t end = offset + size;
    for (uint32_t i = 0; i < res->interval_count; i++) {
        Wc3NullGfxInterval *it = &res->interval[i];
        if (it->nonzero == nonzero && offset <= it->hi && end >= it->lo) {
            if (offset < it->lo) it->lo = offset;
            if (end > it->hi) it->hi = end;
            return 0;
        }
    }
    if (res->interval_count >= WC3_NULL_GFX_MAX_INTERVALS) {
        res->coverage_unknown = 1u;
        return 0;
    }
    Wc3NullGfxInterval *slot = &res->interval[res->interval_count++];
    slot->lo = offset;
    slot->hi = end;
    slot->nonzero = nonzero;
    return 0;
}

/* Return true when [lo, hi) is completely covered by intervals that each
 * recorded at least one non-zero byte.  Overlapping and out-of-order
 * intervals are accepted; the outer loop is bounded by the interval count. */
static int range_written(const Wc3NullGfxResource *res, uint64_t lo,
                         uint64_t hi)
{
    if (!res) return 0;
    if (hi <= lo) return 1;
    if (res->coverage_unknown) return 0;

    uint64_t cursor = lo;
    for (uint32_t step = 0; step <= res->interval_count; step++) {
        if (cursor >= hi) return 1;
        uint64_t next = cursor;
        for (uint32_t i = 0; i < res->interval_count; i++) {
            const Wc3NullGfxInterval *it = &res->interval[i];
            if (it->nonzero && (uint64_t)it->lo <= cursor &&
                (uint64_t)it->hi > next)
                next = it->hi;
        }
        if (next == cursor) return 0;
        cursor = next;
    }
    return 0;
}

int wc3_null_gfx_resource(Wc3NullGfx *gfx, uint32_t id, uint32_t kind,
                          uint32_t size, uint32_t stride)
{
    if (!gfx || id == 0u || size == 0u || stride == 0u) return -1;
    if (kind != WC3_NULL_GFX_VERTEX_BUFFER &&
        kind != WC3_NULL_GFX_INDEX_BUFFER)
        return -1;
    if (kind == WC3_NULL_GFX_INDEX_BUFFER) {
        if ((stride != 2u && stride != 4u) || (size % stride) != 0u)
            return -1;
    }
    if (resource_find(gfx, id)) return -1;

    for (uint32_t i = 0; i < WC3_NULL_GFX_MAX_RESOURCES; i++) {
        Wc3NullGfxResource *res = &gfx->resource[i];
        if (!res->used) {
            memset(res, 0, sizeof *res);
            res->id = id;
            res->size = size;
            res->stride = stride;
            res->kind = kind;
            res->used = 1u;
            return 0;
        }
    }
    return -1;
}

int wc3_null_gfx_resource_destroy(Wc3NullGfx *gfx, uint32_t id)
{
    Wc3NullGfxResource *res = resource_find(gfx, id);
    if (!res || res->active) return -1;
    memset(res, 0, sizeof *res);
    return 0;
}

int wc3_null_gfx_write(Wc3NullGfx *gfx, uint32_t id, uint32_t offset,
                       const void *data, uint32_t size)
{
    Wc3NullGfxResource *res = resource_find(gfx, id);
    if (!res || !resource_range_ok(res, offset, size)) return -1;
    if (size == 0u) return 0;
    return record_write(res, offset, data, size);
}

int wc3_null_gfx_lock(Wc3NullGfx *gfx, uint32_t id, uint32_t offset,
                      uint32_t size, uint32_t flags)
{
    Wc3NullGfxResource *res = resource_find(gfx, id);
    if (!res || res->active) return -1;
    if ((flags & WC3_NULL_GFX_LOCK_READONLY) &&
        (flags & WC3_NULL_GFX_LOCK_DISCARD))
        return -1;

    if (size == 0u) {
        if (offset > res->size) return -1;
        size = res->size - offset;
    }
    if (!resource_range_ok(res, offset, size)) return -1;

    if (flags & WC3_NULL_GFX_LOCK_DISCARD)
        clear_coverage(res);

    res->active = 1u;
    res->lock_offset = offset;
    res->lock_size = size;
    res->lock_flags = flags;
    return 0;
}

int wc3_null_gfx_unlock(Wc3NullGfx *gfx, uint32_t id, const void *data,
                        uint32_t size)
{
    Wc3NullGfxResource *res = resource_find(gfx, id);
    if (!res || !res->active || size != res->lock_size) return -1;

    const uint32_t offset = res->lock_offset;
    const uint32_t flags = res->lock_flags;
    res->active = 0u;
    res->lock_offset = 0u;
    res->lock_size = 0u;
    res->lock_flags = 0u;

    if (flags & WC3_NULL_GFX_LOCK_READONLY) return 0;
    if (size == 0u) return 0;
    if (!data) {
        res->coverage_unknown = 1u;
        return -1;
    }
    return record_write(res, offset, data, size);
}

static int index_count_for(uint32_t primitive_type, uint32_t primitive_count,
                           uint64_t *out)
{
    uint64_t count;
    switch (primitive_type) {
    case WC3_NULL_GFX_POINTLIST:
        count = primitive_count;
        break;
    case WC3_NULL_GFX_LINELIST:
        count = (uint64_t)primitive_count * 2u;
        break;
    case WC3_NULL_GFX_LINESTRIP:
        count = primitive_count ? (uint64_t)primitive_count + 1u : 0u;
        break;
    case WC3_NULL_GFX_TRIANGLELIST:
        count = (uint64_t)primitive_count * 3u;
        break;
    case WC3_NULL_GFX_TRIANGLESTRIP:
    case WC3_NULL_GFX_TRIANGLEFAN:
        count = primitive_count ? (uint64_t)primitive_count + 2u : 0u;
        break;
    default:
        return -1;
    }
    *out = count;
    return 0;
}

static uint32_t read_index(const uint8_t *data, uint64_t i, uint32_t width)
{
    const uint8_t *p = data + i * width;
    if (width == 2u)
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int same_triangle(uint32_t a, uint32_t b, uint32_t c)
{
    return a == b || b == c || a == c;
}

int wc3_null_gfx_draw_indexed(
    Wc3NullGfx *gfx, uint32_t vertex_buffer, uint32_t index_buffer,
    int32_t base_vertex, uint32_t min_vertex, uint32_t num_vertices,
    uint32_t start_index, uint32_t primitive_count, uint32_t primitive_type,
    const void *index_data, uint32_t index_bytes)
{
    if (!gfx) return -1;
    gfx->draw_count++;

    Wc3NullGfxResource *vb = resource_find(gfx, vertex_buffer);
    Wc3NullGfxResource *ib = resource_find(gfx, index_buffer);
    if (!vb || vb->kind != WC3_NULL_GFX_VERTEX_BUFFER || vb->stride == 0u ||
        !ib || ib->kind != WC3_NULL_GFX_INDEX_BUFFER ||
        (ib->stride != 2u && ib->stride != 4u)) {
        gfx->declared_range_failures++;
        gfx->index_unavailable++;
        return 0;
    }

    uint64_t index_count = 0u;
    if (index_count_for(primitive_type, primitive_count, &index_count) != 0) {
        gfx->declared_range_failures++;
        gfx->index_unavailable++;
        return 0;
    }

    const uint64_t width = ib->stride;
    const uint64_t index_offset = (uint64_t)start_index * width;
    const uint64_t index_need = index_count * width;
    const uint64_t vertex_count = vb->size / vb->stride;
    const int64_t vertex_start = (int64_t)base_vertex + (int64_t)min_vertex;
    const int64_t vertex_end = vertex_start + (int64_t)num_vertices;

    int declared_bad = 0;
    if (index_offset > ib->size || index_need > (uint64_t)ib->size - index_offset)
        declared_bad = 1;
    if (vertex_start < 0 || vertex_end < vertex_start ||
        (uint64_t)vertex_end > vertex_count)
        declared_bad = 1;

    const uint64_t min_end = (uint64_t)min_vertex + num_vertices;
    if (min_end < min_vertex) declared_bad = 1;

    if (index_need > index_bytes) {
        declared_bad = 1;
        gfx->index_unavailable++;
    }

    const uint8_t *indices = (const uint8_t *)index_data;
    const int can_scan = indices && index_need > 0u &&
                         (uint64_t)index_bytes >= index_need;

    if (can_scan) {
        for (uint64_t i = 0; i < index_count; i++) {
            const uint32_t index = read_index(indices, i, ib->stride);
            if ((uint64_t)index < min_vertex || (uint64_t)index >= min_end) {
                declared_bad = 1;
            }
            const int64_t actual = (int64_t)base_vertex + (int64_t)index;
            if (actual < 0 || (uint64_t)actual >= vertex_count)
                declared_bad = 1;
        }
    } else if (index_need > 0u) {
        gfx->index_unavailable++;
    }

    if (declared_bad) gfx->declared_range_failures++;

    /* Coverage is evaluated against the declared ranges, not the indices the
     * guest happened to write.  A broken declaration is already a failure,
     * but coverage still gives the more useful diagnostic when it is valid. */
    if (!declared_bad) {
        if (index_need > 0u) {
            if (ib->coverage_unknown) {
                gfx->coverage_unknown++;
            } else if (!range_written(ib, index_offset,
                                      index_offset + index_need)) {
                gfx->locked_range_failures++;
            }
        }
        if (num_vertices > 0u) {
            const uint64_t vertex_byte_lo =
                (uint64_t)vertex_start * vb->stride;
            const uint64_t vertex_byte_hi = vertex_byte_lo +
                                            (uint64_t)num_vertices * vb->stride;
            if (vb->coverage_unknown) {
                gfx->coverage_unknown++;
            } else if (!range_written(vb, vertex_byte_lo, vertex_byte_hi)) {
                gfx->locked_range_failures++;
            }
        }
    }

    if (!can_scan || index_bytes < index_need) return 0;

    uint64_t degenerate = 0u;
    switch (primitive_type) {
    case WC3_NULL_GFX_TRIANGLELIST:
        for (uint64_t p = 0; p < primitive_count; p++) {
            const uint64_t i = p * 3u;
            if (same_triangle(read_index(indices, i, ib->stride),
                              read_index(indices, i + 1u, ib->stride),
                              read_index(indices, i + 2u, ib->stride)))
                degenerate++;
        }
        break;
    case WC3_NULL_GFX_TRIANGLESTRIP:
        for (uint64_t p = 0; p < primitive_count; p++) {
            if (same_triangle(read_index(indices, p, ib->stride),
                              read_index(indices, p + 1u, ib->stride),
                              read_index(indices, p + 2u, ib->stride)))
                degenerate++;
        }
        break;
    case WC3_NULL_GFX_TRIANGLEFAN:
        for (uint64_t p = 0; p < primitive_count; p++) {
            if (same_triangle(read_index(indices, 0u, ib->stride),
                              read_index(indices, p + 1u, ib->stride),
                              read_index(indices, p + 2u, ib->stride)))
                degenerate++;
        }
        break;
    default:
        break;
    }

    gfx->primitive_total += primitive_count;
    gfx->degenerate_total += degenerate;
    if (primitive_count > 0u &&
        degenerate * 1000000ull >
            (uint64_t)primitive_count * gfx->degenerate_ratio_ppm)
        gfx->degenerate_ratio_failures++;

    return 0;
}

void wc3_null_gfx_report(const Wc3NullGfx *gfx, Wc3NullGfxReport *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!gfx) return;
    out->draw_count = gfx->draw_count;
    out->locked_range_failures = gfx->locked_range_failures;
    out->declared_range_failures = gfx->declared_range_failures;
    out->degenerate_ratio_failures = gfx->degenerate_ratio_failures;
    out->coverage_unknown = gfx->coverage_unknown;
    out->index_unavailable = gfx->index_unavailable;
    out->primitive_total = gfx->primitive_total;
    out->degenerate_total = gfx->degenerate_total;
    out->degenerate_ratio_ppm = gfx->degenerate_ratio_ppm;
}

Wc3NullGfxCheck wc3_null_gfx_check_locked(const Wc3NullGfx *gfx)
{
    if (!gfx) return WC3_NULL_GFX_CHECK_UNAVAILABLE;
    if (gfx->locked_range_failures) return WC3_NULL_GFX_CHECK_FAIL;
    if (gfx->draw_count == 0u || gfx->coverage_unknown)
        return WC3_NULL_GFX_CHECK_UNAVAILABLE;
    return WC3_NULL_GFX_CHECK_PASS;
}

Wc3NullGfxCheck wc3_null_gfx_check_declared(const Wc3NullGfx *gfx)
{
    if (!gfx) return WC3_NULL_GFX_CHECK_UNAVAILABLE;
    if (gfx->declared_range_failures) return WC3_NULL_GFX_CHECK_FAIL;
    if (gfx->draw_count == 0u) return WC3_NULL_GFX_CHECK_UNAVAILABLE;
    return WC3_NULL_GFX_CHECK_PASS;
}

Wc3NullGfxCheck wc3_null_gfx_check_degenerate(const Wc3NullGfx *gfx)
{
    if (!gfx) return WC3_NULL_GFX_CHECK_UNAVAILABLE;
    if (gfx->degenerate_ratio_failures) return WC3_NULL_GFX_CHECK_FAIL;
    if (gfx->primitive_total == 0u || gfx->index_unavailable)
        return WC3_NULL_GFX_CHECK_UNAVAILABLE;
    if (gfx->degenerate_total * 1000000ull >
        gfx->primitive_total * gfx->degenerate_ratio_ppm)
        return WC3_NULL_GFX_CHECK_FAIL;
    return WC3_NULL_GFX_CHECK_PASS;
}

const char *wc3_null_gfx_check_name(Wc3NullGfxCheck check)
{
    switch (check) {
    case WC3_NULL_GFX_CHECK_PASS: return "pass";
    case WC3_NULL_GFX_CHECK_FAIL: return "fail";
    case WC3_NULL_GFX_CHECK_UNAVAILABLE: return "unavailable";
    default: return "?";
    }
}
