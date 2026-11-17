/*
 * parallel_triangle.h - deterministic chunked triangle intersection loop.
 *
 * L_6F125924 is the stride-3 fast path of the ray/triangle intersection
 * helper.  It is entered as a tail jump from L_6F125810, so it runs in the
 * caller's frame:
 *
 *   [ebp+0x0C]  vertex base (12-byte vertices)
 *   [ebp+0x14]  index count (converted to triangle count and decremented)
 *   [ebp+0x18]  index base (loaded into EBX, then reused by L_6F125AA0 as the
 *               scratch distance output for the current triangle)
 *   [ebp+0x20]  hit-index pointer
 *   [ebp-4]     ray direction/context pointer
 *   [ebp-8]     ray origin/context pointer
 *   ESI         best-distance pointer
 *
 * The loop reads only the index and vertex arrays.  It writes only the best
 * distance through ESI and the hit index through [ebp+0x20], both with a
 * strict "new distance < best distance" comparison.  A chunk therefore owns
 * a disjoint triangle range and a private pair of output words, and the
 * caller merges the results in increasing triangle order.  Ties keep the
 * lowest triangle index exactly like the sequential loop.
 *
 * The guard is placed at the top of L_6F125924.  Worker chunks and the final
 * epilogue call set CPU.parallel_triangle_bypass, so they execute the original
 * body instead of re-entering the dispatcher.  The wrapper runs the epilogue
 * on the caller by setting [ebp+0x14] to zero and calling the function once;
 * that reproduces the original `ret 0x1c`, the register pops and the hit flag
 * in EAX.
 *
 * The triangle path never nests inside a parallel raycast batch: that batch
 * holds the pool submit lock, so the trylock here fails and the original loop
 * runs on the calling worker.  This is intentional - the outer batch already
 * splits different candidates across the pool.
 *
 * WC3_PARALLEL_GAMEPLAY=0 disables this path together with the other pool
 * paths.  WC3_TRIANGLE=0 disables only this one.  A small batch, an invalid
 * range, a nested call or a worker setup failure runs the original loop on
 * the caller's CPU.  The wrapper's hot path is frameless: initialization,
 * validation and dispatch are out-of-line cold/slow functions.
 */
#ifndef WC3_PARALLEL_TRIANGLE_H
#define WC3_PARALLEL_TRIANGLE_H

#include "cpu.h"
#include "direct_call.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Debug-only frame and range checks, plus the sequential oracle.  The debug
 * app variant defines WC3_DEBUG=1 even though its native code is optimized;
 * the release variant defines WC3_DEBUG=0, so none of this reaches a product
 * build. */
#ifndef WC3_PARALLEL_TRIANGLE_VALIDATE
# if defined(WC3_DEBUG) && WC3_DEBUG
#  define WC3_PARALLEL_TRIANGLE_VALIDATE 1
# else
#  define WC3_PARALLEL_TRIANGLE_VALIDATE 0
# endif
#endif

/* Returns 1 when the batch ran on the pool and the caller's frame has already
 * been advanced exactly like the sequential epilogue.  Returns 0 when the
 * generated function must run normally. */
int wc3_parallel_triangle_try_impl(CPU *c, wc3_lifted_fn fn);

static inline int wc3_parallel_triangle_try(CPU *c, wc3_lifted_fn fn)
{
    if (!c || !fn) return 0;
    if (c->parallel_triangle_bypass) return 0;
    return wc3_parallel_triangle_try_impl(c, fn);
}

#ifdef __cplusplus
}
#endif

#endif /* WC3_PARALLEL_TRIANGLE_H */
