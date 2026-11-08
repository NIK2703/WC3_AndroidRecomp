/*
 * direct_call.h - the P0-1 direct-call contract (GUEST_ABI ABI-4/ABI-5).
 *
 * Why this exists
 * ---------------
 * dispatch_common() increments the guest-thread transition depth and drains a
 * pending tail target in a loop.  dispatch_jmp() relies on depth > 0 to defer
 * that target instead of recursing on the host stack.  Replacing
 * `dispatch(c, 0xVA)` with a bare `L_<va>(c)` removes that accounting: guest
 * state stays correct while the host call mechanism breaks.  This header keeps
 * the accounting without a hash lookup:
 *
 *   dispatch(c, 0xVA)              -> WC3_DIRECT_CALL(L_VA, c)
 *   dispatch_jmp(c, 0xVA); return; -> WC3_DIRECT_TAIL(L_VA, c); return;
 *
 * WC3_DIRECT_TAIL publishes a function pointer (not a VA), so the drain loop
 * calls it directly.  The loop, not recursion, bounds host stack growth.
 * Pending targets are drained at EVERY direct-call boundary, because an x86
 * tail jump from a callee returns to its caller, and that caller must not
 * continue until the tail target has run.
 *
 * State is per guest thread and lives in CPU (ABI-4).  Do not add a TLS copy
 * here: the corpus is split into translation units, and one header-level
 * `static` copy per TU would make a tail target from a neighbour invisible.
 * WC3_DIRECT_CALL also keeps a saved transition frame on the *host stack*
 * (TA-13): a nested direct call owns its pending slot, drains it before
 * returning, and cannot leak a tail target into the caller's drain loop.
 * The saved frame is a local of the macro, so call depth is limited by the
 * real host stack rather than by an arbitrary fixed array.
 *
 * Include this header after cpu.h.  The CPU must be zero-initialised by its
 * owner before the first dispatch (memset or `CPU c = {0}`).
 */
#ifndef WC3_GUEST_DIRECT_CALL_H
#define WC3_GUEST_DIRECT_CALL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WC3_CPU_DEFINED
#error "direct_call.h requires cpu.h first (WC3_CPU_DEFINED)"
#endif

typedef void (*wc3_lifted_fn)(CPU *);

/* Host-stack snapshot of CPU.disp_* (ABI-4).  WC3_DIRECT_CALL uses one local
 * per call; the fields are copied back on leave. */
typedef struct Wc3DispFrame {
    uint32_t depth;
    uint32_t pending_valid;
    uint32_t pending_kind;
    uintptr_t pending_target;
    uint32_t draining;
} Wc3DispFrame;

static inline uint32_t wc3_guest_depth_get(const CPU *c)
{
    return c->disp_depth;
}

static inline void wc3_guest_set_pending_fn(CPU *c, wc3_lifted_fn fn)
{
    c->disp_pending_valid = 1u;
    c->disp_pending_kind = WC3_DISP_PENDING_FN;
    c->disp_pending_target = (uintptr_t)fn;
}

static inline void wc3_guest_drain(CPU *c)
{
    /* Re-entrancy guard for this transition frame.  A nested direct call has
     * its own frame (see WC3_DIRECT_CALL), so it is not blocked here. */
    if (c->disp_draining) return;
    c->disp_draining = 1u;
    while (c->disp_pending_valid) {
        uint32_t kind = c->disp_pending_kind;
        uintptr_t target = c->disp_pending_target;
        c->disp_pending_valid = 0u;
        if (kind == WC3_DISP_PENDING_FN) {
            ((wc3_lifted_fn)(uintptr_t)target)(c);
        } else {
            /* A VA pending target came from a legacy dispatch_jmp.  Resolve it
             * through the one dispatcher, exactly as the indirect path does. */
            dispatch(c, (uint32_t)target);
        }
    }
    c->disp_draining = 0u;
}

static inline void wc3_guest_enter(CPU *c, Wc3DispFrame *saved)
{
    saved->depth = c->disp_depth;
    saved->pending_valid = c->disp_pending_valid;
    saved->pending_kind = c->disp_pending_kind;
    saved->pending_target = c->disp_pending_target;
    saved->draining = c->disp_draining;

    /* Enter the callee at the caller's depth + 1.  The callee gets a fresh
     * pending slot; the caller's slot is restored on leave. */
    c->disp_depth = saved->depth + 1u;
    c->disp_pending_valid = 0u;
    c->disp_pending_kind = WC3_DISP_PENDING_VA;
    c->disp_pending_target = 0u;
    c->disp_draining = 0u;
}

static inline void wc3_guest_leave(CPU *c, const Wc3DispFrame *saved)
{
    /* The callee's host frame is finished.  Drain its tail target at the
     * caller's depth, then restore the caller's transition state. */
    c->disp_depth = saved->depth;
    wc3_guest_drain(c);
    c->disp_depth = saved->depth;
    c->disp_pending_valid = saved->pending_valid;
    c->disp_pending_kind = saved->pending_kind;
    c->disp_pending_target = saved->pending_target;
    c->disp_draining = saved->draining;
}

#define WC3_DIRECT_CALL(fn, c)                                               \
    do {                                                                     \
        Wc3DispFrame wc3_saved_frame_;                                       \
        wc3_guest_enter((c), &wc3_saved_frame_);                             \
        (fn)(c);                                                             \
        wc3_guest_leave((c), &wc3_saved_frame_);                             \
    } while (0)

/* Generated C keeps its own `return;` immediately after this macro. */
#define WC3_DIRECT_TAIL(fn, c)                                               \
    do {                                                                     \
        wc3_guest_set_pending_fn((c), (fn));                                 \
    } while (0)

/* Top-level guest entry must drain any pending target left by an indirect
 * dispatch_jmp before the guest frame is considered finished. */
#define WC3_GUEST_DRAIN(c) wc3_guest_drain(c)

#ifdef __cplusplus
}
#endif

#endif /* WC3_GUEST_DIRECT_CALL_H */
