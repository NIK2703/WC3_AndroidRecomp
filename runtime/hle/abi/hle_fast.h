/*
 * hle_fast.h - the cutover adapter for the new HLE call bridge.
 *
 * The as-built registry in runtime/hle/android/hle_android.c still owns the thunk
 * slots.  New HLE modules override selected slots through
 * wc3_legacy_hle_override(); those slots use the clean hle_abi bridge.  The
 * O-46 release path in hle_bridge.c converts the thunk once and calls an
 * unbound as-built handler directly, preserving its register/stack contract;
 * debug builds keep hle_call() so its call ring and missing/stack diagnostics
 * remain available.  The clean entry point is hle_abi_call(); the target tree
 * cannot also define hle_call() because the as-built HLE already
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

/* O-46: the caller has already converted the thunk to its slot.  This keeps
 * the release path to one subtraction/division and one slot lookup instead of
 * repeating the conversion inside the legacy hle_call(). */
int wc3_hle_fast_call_slot(uint32_t slot, uint32_t thunk, CPU *c);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_FAST_H */
