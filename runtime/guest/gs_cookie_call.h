/*
 * gs_cookie_call.h - specialized clean frame for the MSVC /GS cookie guard.
 *
 * The guard shape below occurs in the lifted corpus and is called from a
 * large number of small wrappers:
 *
 *   cmp ecx, dword ptr [cookie]
 *   jne fail
 *   ret
 *
 * Generated C keeps the generic clean frame at every call site.  That frame
 * saves depth/draining, enters the callee, restores depth, drains and restores
 * draining.  For this one guard the callee body is tiny and known, so the
 * frame lives in one runtime helper instead of being expanded at every call
 * site.  The helper is semantically the same sequence: enter depth + 1,
 * compare, pop the guest return address on success, publish the failure
 * function on mismatch, leave depth, drain, restore draining.
 *
 * Precondition: the caller has already executed push32(ret).  leaf_call emits
 * this form only for the exact guard body and only when the call site is the
 * generated `push32(...); [xmm spill;] WC3_DIRECT_CALL[_CLEAN](guard, c);`
 * sequence.  The failure function is passed directly, exactly like the
 * generated `WC3_DIRECT_TAIL(fail, c)` in the guard body; no VA lookup is
 * performed at runtime.
 *
 * Include after cpu.h and direct_call.h.  The helper is defined in
 * runtime/guest/gs_cookie_call.c, so a translation unit with no guard calls
 * pays no code-size cost.
 */
#ifndef WC3_GUEST_GS_COOKIE_CALL_H
#define WC3_GUEST_GS_COOKIE_CALL_H

#include <stdint.h>

#include "direct_call.h"

#ifdef __cplusplus
extern "C" {
#endif

void wc3_guest_call_gs_cookie(CPU *c, uint32_t cookie_va, wc3_lifted_fn fail);

#define WC3_CALL_GS_COOKIE(c, cookie_va, fail_fn) \
    wc3_guest_call_gs_cookie((c), (uint32_t)(cookie_va), (fail_fn))

#ifdef __cplusplus
}
#endif

#endif /* WC3_GUEST_GS_COOKIE_CALL_H */
