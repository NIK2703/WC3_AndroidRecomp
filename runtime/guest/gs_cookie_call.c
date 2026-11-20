/*
 * gs_cookie_call.c - shared frame for the lifted MSVC /GS cookie guard.
 *
 * See runtime/guest/gs_cookie_call.h.  The helper is deliberately external:
 * one copy in libwc3gameactivity.so replaces the per-call clean frame and
 * avoids a static copy in every corpus translation unit.
 */
#include "cpu.h"
#include "direct_call.h"
#include "gs_cookie_call.h"

void wc3_guest_call_gs_cookie(CPU *c, uint32_t cookie_va, wc3_lifted_fn fail)
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
