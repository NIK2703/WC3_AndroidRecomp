/*
 * direct_call.h - reference contract for LiftPass.devirtualize (P0-1).
 *
 * Why this exists
 * ---------------
 * dispatch_common() keeps g_disp_depth and drains g_tail_pending in a loop.
 * dispatch_jmp() relies on depth > 0 to defer a tail target instead of
 * recursing on the host stack.  Replacing `dispatch(c, 0xVA)` with a bare
 * `L_<va>(c)` removes that accounting: guest state stays correct while the
 * host call mechanism breaks.  This header keeps the trampoline semantics
 * without a hash lookup:
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
 * State is thread-private: each guest thread owns exactly one CPU.  Keeping
 * it in CPU fields instead of TLS is a later micro-optimization; appending
 * fields to CPU does not break the corpus (source-level contract, ADR-019).
 *
 * Include AFTER cpu.h in generated translation units.  CPU is only forward
 * declared here so this header can also be used by standalone unit tests.
 */
#ifndef WC3_GUEST_DIRECT_CALL_H
#define WC3_GUEST_DIRECT_CALL_H

#include <stdint.h>

#ifndef WC3_CPU_DEFINED
typedef struct CPU CPU;
#endif

typedef void (*wc3_lifted_fn)(CPU *);

#if defined(_MSC_VER)
#define WC3_GUEST_TLS __declspec(thread)
#else
#define WC3_GUEST_TLS _Thread_local
#endif

/* Depth is shared with the legacy dispatcher for mixed direct/indirect paths;
 * it is diagnostic here, the drain loop is what guarantees correctness. */
static WC3_GUEST_TLS uint32_t wc3_guest_call_depth;
static WC3_GUEST_TLS wc3_lifted_fn wc3_guest_pending_fn;
static WC3_GUEST_TLS int wc3_guest_draining;

static inline uint32_t wc3_guest_depth_get(void) {
    return wc3_guest_call_depth;
}

static inline void wc3_guest_set_pending(wc3_lifted_fn fn) {
    wc3_guest_pending_fn = fn;
}

static inline void wc3_guest_drain(CPU *c) {
    /* Re-entrancy guard: a lifted function reached from the drain loop may
     * itself perform direct calls.  The outermost loop owns the pending slot. */
    if (wc3_guest_draining) return;
    wc3_guest_draining = 1;
    while (wc3_guest_pending_fn) {
        wc3_lifted_fn fn = wc3_guest_pending_fn;
        wc3_guest_pending_fn = (wc3_lifted_fn)0;
        fn(c);
    }
    wc3_guest_draining = 0;
}

static inline void wc3_guest_enter(CPU *c) {
    (void)c;
    ++wc3_guest_call_depth;
}

static inline void wc3_guest_leave(CPU *c) {
    if (wc3_guest_call_depth) --wc3_guest_call_depth;
    wc3_guest_drain(c);
}

#define WC3_DIRECT_CALL(fn, c) \
    do { wc3_guest_enter(c); fn(c); wc3_guest_leave(c); } while (0)

/* Generated C keeps its own `return;` immediately after this macro. */
#define WC3_DIRECT_TAIL(fn, c) \
    do { (void)(c); wc3_guest_set_pending(fn); } while (0)

/* Top-level guest entry must drain any pending target left by an indirect
 * dispatch_jmp before the guest frame is considered finished. */
#define WC3_GUEST_DRAIN(c) wc3_guest_drain(c)

#endif /* WC3_GUEST_DIRECT_CALL_H */
