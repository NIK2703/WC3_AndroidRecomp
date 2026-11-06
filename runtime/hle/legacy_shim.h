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

/* M5 strangler seam (H6.2/H6.3).  A target module installs its own handler over
 * an as-built table row while the quarantined implementation is still linked.
 * The as-built row keeps argc and call convention, so the replacement must
 * match them.  Must run before the first guest thread; returns 0 on success,
 * -1 when the row does not exist.  The legacy file stays read-only. */
int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c));

/* Guest memory and argument accessors, served by the quarantine until the
 * guest layer moves into the target tree.  They keep one range table: a target
 * module must not link a second copy of runtime/guest during the migration. */
int      wc3_legacy_guest_ok(uint32_t addr, uint32_t size);
uint8_t  wc3_legacy_rd8g(uint32_t addr);
uint16_t wc3_legacy_rd16g(uint32_t addr);
uint32_t wc3_legacy_rd32g(uint32_t addr);
void     wc3_legacy_wr8g(uint32_t addr, uint8_t value);
void     wc3_legacy_wr16g(uint32_t addr, uint16_t value);
void     wc3_legacy_wr32g(uint32_t addr, uint32_t value);
uint32_t wc3_legacy_arg32(const CPU *cpu, unsigned index);

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
