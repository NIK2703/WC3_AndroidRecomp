/*
 * hle_fast.h - the cutover adapter for the new HLE call bridge.
 *
 * The legacy registry in runtime/legacy/hle_android.c still owns the thunk
 * slots.  New HLE modules override selected slots through
 * wc3_legacy_hle_override(); those slots can use the clean hle_abi bridge,
 * while every other slot keeps the as-built call path (including its missing
 * import diagnostics).  The clean entry point is hle_abi_call(); the target
 * tree cannot also define hle_call() because the as-built quarantine already
 * exports that symbol.
 */
#ifndef WC3_HLE_FAST_H
#define WC3_HLE_FAST_H

#include <stdint.h>

#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bind a legacy slot to the new registry.  Called only from the shim while
 * the as-built entry is being overridden; startup only, no hot-path locks. */
void wc3_hle_fast_bind(uint32_t slot, void (*impl)(CPU *), uint16_t argc,
                       uint8_t cdecl_abi);

/* Returns 1 after the new bridge handled the thunk, 0 when the slot was never
 * bound and the caller must use the legacy hle_call(). */
int wc3_hle_fast_call(uint32_t thunk, CPU *c);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_FAST_H */
