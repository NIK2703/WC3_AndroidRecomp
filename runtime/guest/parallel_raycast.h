/*
 * parallel_raycast.h - deterministic chunked execution of L_6F749F90.
 *
 * The generated leaf corpus calls L_6F749F90 directly from L_6F78D9B0.  That
 * function walks every candidate in the container stored in ECX, keeps the
 * first best hit and writes the hit flag, distance and position back through
 * the five stack arguments.  The candidate tests only read the container,
 * the candidate objects and the caller's read-only ray context.
 *
 * This wrapper replaces that one call site with a parallel dispatcher:
 *
 *   WC3_DIRECT_CALL_CLEAN(L_6F749F90, c)
 *     ->
 *   wc3_parallel_raycast(c, L_6F749F90);
 *
 * The original function still runs on private worker CPU contexts, once per
 * contiguous candidate range.  Results are merged in increasing range order
 * with the original strict `best_distance > candidate_distance` comparison, so
 * the first hit wins ties exactly as it does on the sequential path.
 *
 * WC3_PARALLEL_RAYCAST=0 disables the fast path at runtime.  The compile-time
 * option only controls whether the lift pass rewrites the call site.  A NULL
 * function pointer, an unavailable pool, a small candidate count or a nested
 * call all fall back to the original function on the caller's CPU.
 *
 * The default threshold is max(2 * min_chunk, 2 * workers) candidates and the
 * split keeps at least min_chunk (default 8) candidates per chunk.  With the
 * as-built eight workers that is a 16-candidate threshold.  The wrapper's
 * hot path is frameless: it checks the cached init result, tuning switches
 * and candidate threshold before reading the five stack arguments, validating
 * ranges or touching a job.  Initialization, tuning reload, validation and dispatch are out of
 * line.
 *
 * WC3_RAY_VERIFY=1 runs the original function on a private CPU after every
 * parallel batch and compares the hit flag, distance bits and position bits.
 * The sequential result wins on mismatch, the parallel path is disabled for
 * the rest of the process, and the mismatch is logged.
 */
#ifndef WC3_PARALLEL_RAYCAST_H
#define WC3_PARALLEL_RAYCAST_H

#include "cpu.h"
#include "direct_call.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Function-pointer form keeps the generated translation unit free of a
 * declaration of L_6F749F90 at the wrapper.  The pointer passed by the
 * rewrite is the same L_6F749F90 that the original direct call used. */
void wc3_parallel_raycast(CPU *c, wc3_lifted_fn fn);

#ifdef __cplusplus
}
#endif

#endif /* WC3_PARALLEL_RAYCAST_H */
