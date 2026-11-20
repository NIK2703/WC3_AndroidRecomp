/*
 * gs_cookie_call_static.h - per-TU specialization for a guard-heavy chunk.
 *
 * A static helper lets the compiler propagate the constant guard operands into
 * the frame.  That is a win only when one translation unit contains enough
 * guard calls to amortize the helper body; leaf_call includes this header only
 * above its measured threshold.  Low-count chunks use the shared runtime helper
 * in gs_cookie_call.h instead.
 *
 * The frame and precondition are identical to gs_cookie_call.h.  Include after
 * cpu.h and direct_call.h.
 */
#ifndef WC3_GUEST_GS_COOKIE_CALL_STATIC_H
#define WC3_GUEST_GS_COOKIE_CALL_STATIC_H

#include <stdint.h>

#include "direct_call.h"

#ifdef __cplusplus
extern "C" {
#endif

static __attribute__((noinline, unused)) void
wc3_guest_call_gs_cookie(CPU *c, uint32_t cookie_va, wc3_lifted_fn fail)
{
    uint32_t saved_depth = c->disp_depth;
    uint32_t saved_draining = c->disp_draining;

    c->disp_depth = saved_depth + 1u;
    c->disp_draining = 0u;
    flags_sub(c, c->ecx, rd32((GVA(cookie_va))), 4);
    if (c->zf) {
        c->esp += 4u;
    } else {
        wc3_guest_set_pending_fn(c, fail);
    }
    c->disp_depth = saved_depth;
    wc3_guest_drain(c);
    c->disp_depth = saved_depth;
    c->disp_draining = saved_draining;
}

#define WC3_CALL_GS_COOKIE(c, cookie_va, fail_fn) \
    wc3_guest_call_gs_cookie((c), (uint32_t)(cookie_va), (fail_fn))

#ifdef __cplusplus
}
#endif

#endif /* WC3_GUEST_GS_COOKIE_CALL_STATIC_H */
