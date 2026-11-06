/*
 * legacy_shim.h - the only door from target code into the migration quarantine.
 *
 * R15/H6.3: as-built sources live under runtime/legacy and are read-only.
 * New translation units must not include their headers directly; they include
 * this stable surface instead.  legacy_shim.c is the single implementation
 * that is allowed to include the quarantine headers, and it pins the few
 * values that target code needs (HLE window size and thunk stride).
 */
#ifndef WC3_HLE_LEGACY_SHIM_H
#define WC3_HLE_LEGACY_SHIM_H

#include <stdint.h>

#include "cpu.h"
#include "guest_layout.h"

/* hle_registry.h owns the real registry limit.  Keep the number here in one
 * place and let legacy_shim.c's static assertions catch drift at build time. */
#define WC3_LEGACY_HLE_MAX 4096u
#define WC3_LEGACY_HLE_STRIDE GUEST_HLE_STRIDE

#ifdef __cplusplus
extern "C" {
#endif

/* dispatch_boot.c: enter an HLE thunk and name an address for diagnostics. */
void        wc3_legacy_hle_call(uint32_t thunk, CPU *c);
const char *wc3_legacy_hle_describe(uint32_t thunk);

/* app_boot.c: device-relative display-mode selection. */
void wc3_legacy_set_screen_size(uint32_t width, uint32_t height);
int  wc3_legacy_default_display_mode(uint32_t *width, uint32_t *height);
int  wc3_legacy_select_display_mode(uint32_t width, uint32_t height);

/* host_shell.c: the virtual cursor consumed by the guest. */
void wc3_legacy_mouse_move(float nx, float ny);
void wc3_legacy_mouse_button(int down);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_LEGACY_SHIM_H */
