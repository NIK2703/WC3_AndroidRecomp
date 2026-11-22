/*
 * parallel_phase.h - debug SPMD phase contract for the raycast batch.
 *
 * The phase runtime lives in runtime/hle/android/parallel_raycast.c and is
 * compiled only for WC3_DEBUG && WC3_DEBUG_PARALLEL.  Release has no phase
 * code.  A product caller is expected to be split by the lift pass into:
 *
 *   collect: reserve a slot, run the stateful prefix, then commit(...)
 *   batch:   wc3_phase_run()
 *   apply:   wc3_phase_result(i) and the original postfix
 *
 * The shadow collector in parallel_raycast.c uses the same request layout
 * without changing game state.  This header is the stable boundary for a
 * LiftPass-generated collect/apply pair; the runtime does not know which
 * caller owns the requests.
 *
 * A collect half cannot return a normal raycast result: the original caller
 * consumes hit/dist/pos immediately.  It runs its prefix, commits one request,
 * and returns without running its postfix.  Every caller on the path to the
 * phase boundary detects that the request count grew, commits its own level
 * snapshot, restores the entry CPU, and returns without running its postfix.
 * The apply half restores the level CPU and guest frame, runs the inner apply,
 * and then runs its own postfix.  Levels are numbered from the innermost
 * raycast (0) outward.  The generated chain is debug-only and is gated by the
 * arm flag so an unarmed tail target falls back to the original function.
 */
#ifndef WC3_PARALLEL_PHASE_H
#define WC3_PARALLEL_PHASE_H

#include "cpu.h"
#include "direct_call.h"
/* The corpus target does not carry runtime/diag on its include path; resolve
 * the tag registry relative to this header so both the corpus and the HLE
 * runtime see the same WC3_DEBUG_<TAG> values. */
#include "../diag/wc3_debug.h"

#if WC3_DEBUG && WC3_DEBUG_PARALLEL

#define WC3_PHASE_MAX_REQUESTS   256u
/* The raycast chain reads container+0x110, container+0x114 and the hit path
 * reads container+0x14c.  Copy only through the end of the last observed
 * word; the old 0x200 window copied 176 bytes that no caller reads. */
#define WC3_PHASE_CONTAINER_SNAP 0x00000150u

/* Largest guest frame a collect level may save.  The deepest chain frame is
 * below 0xd8 bytes; 0x100 keeps the full live frame and SEH record with
 * headroom while halving the per-level copy from the old 0x200 window. */
#define WC3_PHASE_FRAME_MAX      0x00000100u

/* Level 0 is the innermost raycast and lives in the request slot itself.
 * Levels 1..WC3_PHASE_MAX_LEVELS-1 are the caller continuations. */
#define WC3_PHASE_MAX_LEVELS     4u

/* Return values kept for the older text split and its self-test.  The current
 * collect chain propagates by comparing wc3_phase_count() across the call, so
 * these are not read by generated code. */
#define WC3_PHASE_PENDING 0xFFFFFFF0u
#define WC3_PHASE_FAILED  0xFFFFFFF1u

typedef struct Wc3PhaseRequest {
    uint32_t index;
    uint32_t container;          /* private container snapshot VA */
    uint32_t array;
    uint32_t count;
    uint32_t in1;                /* original VA, read by the replay */
    uint32_t in2;                /* original VA, read by the replay */
    uint32_t out_pos;
    uint32_t out_dist;
    uint32_t user_ctx;
    uint32_t hit;
    uint32_t dist_bits;
    uint32_t pos[3];
    uint32_t frame_va;           /* saved guest frame range, for apply */
    uint32_t frame_len;
    uint8_t frame[WC3_PHASE_FRAME_MAX];
} Wc3PhaseRequest;

typedef struct Wc3PhaseResult {
    uint32_t hit;
    uint32_t dist_bits;
    uint32_t pos[3];
} Wc3PhaseResult;

/* Apply half of one chain level.  `index` is the request slot; the function
 * restores its own level snapshot and frame, runs the inner apply, then runs
 * the original postfix.  The generated wrappers have this exact signature. */
typedef void (*Wc3PhaseApplyFn)(CPU *c, unsigned index);

#ifdef __cplusplus
extern "C" {
#endif

/* Reserve one request slot before the collect half runs its stateful prefix.
 * Returns 0 when the phase is unavailable or the window is full; the collect
 * half then calls the original lifted function and returns its result. */
int wc3_phase_reserve(void);

/* Fill the reserved slot after the prefix.  The slot was reserved first, so
 * this call cannot fail for capacity reasons.  Invalid guest ranges abort in
 * debug: they mean the caller is about to corrupt the game state anyway. */
void wc3_phase_commit(CPU *c, wc3_lifted_fn fn, uint32_t container,
                      uint32_t array, uint32_t count, uint32_t in1,
                      uint32_t in2, uint32_t out_pos, uint32_t out_dist,
                      uint32_t user_ctx, uint32_t frame_va,
                      uint32_t frame_len);

/* Run the collected batch as one fork-join.  Returns 1 when the phase ran
 * and the results are valid, 0 when the caller must fall back. */
int wc3_phase_run(void);

/* Result of request `index` after a successful wc3_phase_run(). */
const Wc3PhaseRequest *wc3_phase_result(unsigned index);

/* CPU snapshot taken by wc3_phase_commit() for the same request.  The
 * generated apply half restores it before running the original postfix. */
const CPU *wc3_phase_request_cpu(unsigned index);

/* Restore the guest frame bytes saved by wc3_phase_commit().  The collect
 * half runs on the caller's stack; a later collect call can reuse the same
 * local slots, so the apply half must put the prefix frame back before the
 * postfix runs. */
void wc3_phase_restore_frame(unsigned index);

/* Caller continuation levels.  A collect half that saw the request count grow
 * calls wc3_phase_commit_level() with its own post-call CPU and a 0x200-byte
 * frame window ending at ebp + 8:
 *
 *     [ebp - (WC3_PHASE_FRAME_MAX - 8), ebp + 8)
 *
 * The window is anchored at ebp, not at the post-call esp: after the callee's
 * ret the post-call esp sits above the caller's locals, so an [esp, ebp+8)
 * range would be empty.  The window covers the caller's locals, saved
 * registers, saved ebp and return address.  Level 0 is the innermost raycast
 * and must not be passed here. */
void wc3_phase_commit_level(CPU *c, unsigned level, uint32_t frame_va,
                            uint32_t frame_len);
const CPU *wc3_phase_request_cpu_level(unsigned index, unsigned level);
void wc3_phase_restore_frame_level(unsigned index, unsigned level);

/* Number of requests committed in the current window.  A collect caller
 * samples this before the inner collect and treats a change as pending. */
unsigned wc3_phase_count(void);

/* True when the window cannot take another request.  The per-object wrapper
 * flushes the pending window before collecting a new object, so a full window
 * does not silently reorder the deferred postfixes. */
int wc3_phase_window_full(void);

/* Arm flag for tail-reached collect halves.  Only the per-object wrapper arms
 * it; an unarmed tail target falls back to the original lifted function. */
int wc3_phase_arm(int on);
int wc3_phase_armed(void);

/* Run the window through the pool, or sequentially if the pool cannot run it.
 * Returns 1 when every request has a valid result. */
int wc3_phase_run_or_seq(void);

/* Apply the whole window through the outer apply function and reset it.
 * Saves and restores the caller's control state, so the phase boundary can
 * resume the original function at the same CPU.  An apply postfix may re-enter
 * the object loop and append requests; those are drained before the reset, and
 * a nested boundary reached during the apply is a no-op (the outer drain owns
 * the window).  Returns 0 when there was no window or when called re-entrantly.
 * A failed run aborts in debug: the deferred postfixes cannot be dropped
 * without corrupting game state. */
int wc3_phase_apply_all(CPU *c, Wc3PhaseApplyFn apply);

/* Drop the collected window without running it. */
void wc3_phase_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_DEBUG && WC3_DEBUG_PARALLEL */

#endif /* WC3_PARALLEL_PHASE_H */
