/*
 * parallel_vertex.h - deterministic chunked vertex attribute loops.
 *
 * Two generated main-thread loops account for most of the remaining vertex
 * cost:
 *
 *   L_6F1419D4  copies the per-vertex attributes (position + up to four
 *               optional streams) and advances the shared source pointers;
 *   L_6F1417F0  transforms the position by the selected matrix and copies
 *               the same optional streams.
 *
 * Both loops are pure functions of a read-only source range and a disjoint
 * destination range.  The only mutable shared state is the 13-dword block at
 * 0x6FD53938..0x6FD53968, which holds the vertex count, the six source
 * pointers, their strides and the matrix-index pointer.
 *
 * The lift pass rewrites only the GVA() references inside those two function
 * bodies to wc3_vertex_global(c, address).  The helper adds a per-CPU delta:
 * zero on the caller and on normal CPUs, or private_block - FIRST on a pool
 * worker.  The generated loop keeps its exact instruction order while each
 * worker advances private pointers, and the common path pays one ADD per
 * global access instead of a load-and-branch.  The wrapper merges the private
 * blocks back into the real globals once all chunks are complete.
 *
 * A batch is split into at most one chunk per pool worker, and the pool
 * creates one worker per online CPU.  The default split needs at least 256
 * vertices and at least 128 vertices per chunk, so small batches stay on the
 * caller.  WC3_VERTEX_THRESHOLD, WC3_VERTEX_CHUNKS and WC3_VERTEX_MIN_CHUNK
 * are diagnostic overrides; the raycast `chunks` setting does not limit this
 * path.
 *
 * WC3_PARALLEL_GAMEPLAY=0 disables this path together with the raycast pool.
 * A NULL function, a small batch, an invalid range or a nested call falls back
 * to the original function on the caller's CPU.  The wrapper's hot path is
 * frameless and has no TLS lookup; validation and dispatch live out of line.
 */
#ifndef WC3_PARALLEL_VERTEX_H
#define WC3_PARALLEL_VERTEX_H

#include "cpu.h"
#include "direct_call.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WC3_VERTEX_GLOBAL_FIRST 0x6FD53938u
#define WC3_VERTEX_GLOBAL_LAST  0x6FD5396Cu
#define WC3_VERTEX_GLOBAL_WORDS \
    ((WC3_VERTEX_GLOBAL_LAST - WC3_VERTEX_GLOBAL_FIRST) / 4u)

#define WC3_VERTEX_COPY      0u
#define WC3_VERTEX_TRANSFORM 1u

/* Debug-only range, overlap and frame checks.  The debug app variant defines
 * WC3_DEBUG=1 even though its native code is optimized; the release variant
 * defines WC3_DEBUG=0.  Keep validation on the debug side of that tag so it
 * cannot leak into a product build.  The generated loop's layout is verified
 * by the debug build and by the lift tool before the corpus is produced. */
#ifndef WC3_PARALLEL_VERTEX_VALIDATE
# if defined(WC3_DEBUG) && WC3_DEBUG
#  define WC3_PARALLEL_VERTEX_VALIDATE 1
# else
#  define WC3_PARALLEL_VERTEX_VALIDATE 0
# endif
#endif

/* The generated loop bodies call this with one of the addresses from the
 * 0x6FD53938..0x6FD53968 block.  The caller's delta is zero, so the expression
 * is identical to the original GVA(addr).  A worker's delta maps the same
 * address into its private block. */
static inline uint32_t wc3_vertex_global(CPU *c, uint32_t addr)
{
    return addr + c->parallel_vertex_delta;
}

/* Returns 1 when the batch ran on the pool and the caller's frame has already
 * been advanced exactly like the sequential epilogue.  Returns 0 when the
 * generated function must run normally. */
int wc3_parallel_vertex_try_impl(CPU *c, wc3_lifted_fn fn, unsigned kind);

static inline int wc3_parallel_vertex_try(CPU *c, wc3_lifted_fn fn,
                                          unsigned kind)
{
    if (!c || !fn) return 0;
    if (c->parallel_vertex_delta) return 0;
    return wc3_parallel_vertex_try_impl(c, fn, kind);
}

/* Diagnostic only.  The post-lift pass inserts one call at the top of
 * L_6F141680, the vertex batch setup, after `this` and `dest` are loaded and
 * before the first format test.  It classifies the sibling-loop decision
 * (CF0/BD0/AD0/generic) without changing the CPU or guest memory.  A release
 * build compiles the call out together with this declaration. */
#if WC3_DEBUG && WC3_DEBUG_PERF
void wc3_vertex_dispatch_note(const CPU *c);
#endif

#ifdef __cplusplus
}
#endif

#endif /* WC3_PARALLEL_VERTEX_H */
