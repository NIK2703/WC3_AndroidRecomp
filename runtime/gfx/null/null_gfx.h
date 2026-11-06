/*
 * null_gfx.h - canonical D3D9 stream and V5.9 invariants for the T1 DUT.
 *
 * This is the verification frontend only.  It is never linked into the APK:
 * the product uses the thin D3D9 -> DXVK bridge.  The null frontend draws no
 * pixels.  It records a host-independent canonical command stream and checks
 * the three semantic invariants from docs/VERIFICATION.md V5.9:
 *
 *   locked_range_written       every buffer range used by a draw was written
 *                              with at least one non-zero byte between Lock
 *                              and Unlock;
 *   declared_range_consistency numVertices/startIndex/primitiveCount agree
 *                              with the real resource ranges and indices;
 *   degenerate_primitive_ratio the fraction of degenerate primitives does
 *                              not exceed the threshold captured from G.
 *
 * Stream identity:
 *   - method is (iface, method) plus up to WC3_NULL_GFX_MAX_ARGS 32-bit
 *     canonical arguments;
 *   - the current state digest is mixed into every event;
 *   - host pointers, host timings and worker order are never part of the
 *     recording.  An argument that is a pointer must be a guest VA, and a
 *     float must be passed as its IEEE-754 bit pattern;
 *   - the digest is FNV-1a 64 with explicit little-endian encoding.  It is a
 *     deterministic stream identity, not a cryptographic hash and not a
 *     substitute for the V3 oracle identity.
 *
 * Invariant helpers are observations, not stream events.  The bridge records
 * the COM call with wc3_null_gfx_emit(), then calls the matching helper with
 * the guest-stable resource id.  Resource ids must be deterministic across
 * runs (a guest object VA or a creation counter), never a host pointer.
 *
 * All capacities are fixed.  Hitting a capacity marks the affected check
 * UNAVAILABLE; it never turns missing evidence into PASS (V1.2).
 */
#ifndef WC3_NULL_GFX_H
#define WC3_NULL_GFX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WC3_NULL_GFX_MAX_ARGS 8u

enum Wc3NullGfxResourceKind {
    WC3_NULL_GFX_VERTEX_BUFFER = 1,
    WC3_NULL_GFX_INDEX_BUFFER = 2
};

enum Wc3NullGfxLockFlags {
    WC3_NULL_GFX_LOCK_DISCARD = 1u << 0,
    WC3_NULL_GFX_LOCK_READONLY = 1u << 1,
    WC3_NULL_GFX_LOCK_NOOVERWRITE = 1u << 2
};

/* Matches D3DPRIMITIVETYPE. */
enum Wc3NullGfxPrimitiveType {
    WC3_NULL_GFX_POINTLIST = 1,
    WC3_NULL_GFX_LINELIST = 2,
    WC3_NULL_GFX_LINESTRIP = 3,
    WC3_NULL_GFX_TRIANGLELIST = 4,
    WC3_NULL_GFX_TRIANGLESTRIP = 5,
    WC3_NULL_GFX_TRIANGLEFAN = 6
};

typedef enum Wc3NullGfxCheck {
    WC3_NULL_GFX_CHECK_PASS = 0,
    WC3_NULL_GFX_CHECK_FAIL = 1,
    WC3_NULL_GFX_CHECK_UNAVAILABLE = 2
} Wc3NullGfxCheck;

typedef struct Wc3NullGfxReport {
    uint32_t draw_count;
    /* Draws where a referenced range was not covered by a non-zero write. */
    uint32_t locked_range_failures;
    /* Draws where declared ranges/indices disagreed with the resources. */
    uint32_t declared_range_failures;
    /* Draws whose own degenerate ratio exceeded the configured limit. */
    uint32_t degenerate_ratio_failures;
    /* A resource or its coverage no longer fits the fixed tables. */
    uint32_t coverage_unknown;
    /* Index bytes were missing or a primitive type could not be checked. */
    uint32_t index_unavailable;
    uint64_t primitive_total;
    uint64_t degenerate_total;
    uint32_t degenerate_ratio_ppm;
} Wc3NullGfxReport;

typedef struct Wc3NullGfx Wc3NullGfx;

/* degenerate_ratio_ppm is the ceiling captured from G, in parts per million
 * (for example 10000 is 1%).  Zero is valid and means "no degenerate
 * primitives". */
Wc3NullGfx *wc3_null_gfx_create(uint32_t degenerate_ratio_ppm);
void wc3_null_gfx_destroy(Wc3NullGfx *gfx);

/* Clear the stream and every invariant observation. */
void wc3_null_gfx_reset(Wc3NullGfx *gfx);
/* Clear only stream identity; resources and invariant counts survive. */
void wc3_null_gfx_reset_stream(Wc3NullGfx *gfx);

/* Record one canonical D3D9 call.  Returns 0 on success, -1 for a bad
 * argument or capacity overflow (the event is then not recorded). */
int wc3_null_gfx_emit(Wc3NullGfx *gfx, uint16_t iface, uint16_t method,
                      const uint32_t *args, uint16_t argc);

/* Fold one render-state assignment into the live state digest. */
int wc3_null_gfx_state(Wc3NullGfx *gfx, uint32_t key, uint32_t value);

uint64_t wc3_null_gfx_stream_hash(const Wc3NullGfx *gfx);
uint64_t wc3_null_gfx_state_hash(const Wc3NullGfx *gfx);
uint64_t wc3_null_gfx_event_count(const Wc3NullGfx *gfx);

/* Register a buffer.  For a vertex buffer stride is the FVF stride; for an
 * index buffer it is the index width (2 or 4).  Creating an existing id is an
 * error; destroy it first.  Destroy fails while a lock is active. */
int wc3_null_gfx_resource(Wc3NullGfx *gfx, uint32_t id, uint32_t kind,
                          uint32_t size, uint32_t stride);
int wc3_null_gfx_resource_destroy(Wc3NullGfx *gfx, uint32_t id);

/* Note data supplied at creation or by UpdateSubresource, without a lock. */
int wc3_null_gfx_write(Wc3NullGfx *gfx, uint32_t id, uint32_t offset,
                       const void *data, uint32_t size);

/* Lock a buffer range.  size == 0 means "to the end of the resource".
 * Nested locks are rejected: the bridge must merge them before calling. */
int wc3_null_gfx_lock(Wc3NullGfx *gfx, uint32_t id, uint32_t offset,
                      uint32_t size, uint32_t flags);
/* Complete the active lock.  size must equal the locked size.  A READONLY
 * lock records no write.  Non-readonly data is scanned to distinguish a real
 * write from an untouched all-zero staging buffer. */
int wc3_null_gfx_unlock(Wc3NullGfx *gfx, uint32_t id, const void *data,
                        uint32_t size);

/* DrawIndexedPrimitive observation.  base_vertex is D3D9 BaseVertexIndex
 * (signed).  index_data points at start_index and index_bytes is its readable
 * size.  The data is read but never stored. */
int wc3_null_gfx_draw_indexed(
    Wc3NullGfx *gfx, uint32_t vertex_buffer, uint32_t index_buffer,
    int32_t base_vertex, uint32_t min_vertex, uint32_t num_vertices,
    uint32_t start_index, uint32_t primitive_count, uint32_t primitive_type,
    const void *index_data, uint32_t index_bytes);

void wc3_null_gfx_report(const Wc3NullGfx *gfx, Wc3NullGfxReport *out);

/* Local invariant verdicts.  UNAVAILABLE means the recording cannot decide
 * the question; it never means PASS.  Overall T1 verdict ownership stays with
 * docs/EXACTNESS.md E11. */
Wc3NullGfxCheck wc3_null_gfx_check_locked(const Wc3NullGfx *gfx);
Wc3NullGfxCheck wc3_null_gfx_check_declared(const Wc3NullGfx *gfx);
Wc3NullGfxCheck wc3_null_gfx_check_degenerate(const Wc3NullGfx *gfx);
const char *wc3_null_gfx_check_name(Wc3NullGfxCheck check);

#ifdef __cplusplus
}
#endif

#endif /* WC3_NULL_GFX_H */
