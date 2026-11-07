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

/* Register an import row the as-built table did not define.  The resolver
 * creates a guessed-arity stub for an unknown import; this replaces that stub
 * (or appends a row when the import has not been bound yet) during bootstrap.
 * `cdecl_abi` selects the caller-pops convention, so a cdecl CRT import must
 * pass 1 even when `argc` is known.  Must run before the first guest thread;
 * returns 0 on success, -1 for a bad argument or a full registry. */
int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c));

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

/* Guest heap boundary (kernel32/memory).  The as-built heap already owns a
 * block header before every payload; these wrappers keep the header format in
 * one place instead of leaking `addr - 8` into target modules. */
uint32_t wc3_legacy_guest_alloc(uint32_t size);
uint32_t wc3_legacy_guest_alloc0(uint32_t size);
void     wc3_legacy_guest_free(uint32_t addr);
uint32_t wc3_legacy_guest_size(uint32_t addr);
void     wc3_legacy_guest_copy(uint32_t dst, uint32_t src, uint32_t size);
void     wc3_legacy_guest_move(uint32_t dst, uint32_t src, uint32_t size);
void     wc3_legacy_guest_fill(uint32_t dst, uint8_t value, uint32_t size);
void     wc3_legacy_guest_zero(uint32_t dst, uint32_t size);

/* app_boot.c: device-relative display-mode selection. */
void wc3_legacy_set_screen_size(uint32_t width, uint32_t height);
int  wc3_legacy_current_display_mode(uint32_t *width, uint32_t *height);
int  wc3_legacy_default_display_mode(uint32_t *width, uint32_t *height);
int  wc3_legacy_select_display_mode(uint32_t width, uint32_t height);

/* The as-built mode table, for USER32 display enumeration.  `display_mode_at`
 * returns the width/height of one entry; `display_mode_supported` says whether
 * a pair is in the same table; refresh is the fixed panel rate the guest sees. */
int      display_mode_at(uint32_t index, uint32_t *width, uint32_t *height);
int      display_mode_supported(uint32_t width, uint32_t height);
uint32_t display_refresh_hz(void);

/* host_shell.c and runtime/app/overlay_actions_legacy.c: the virtual cursor
 * and keyboard consumed by the guest.  These are the only entry points for
 * host-side input; overlay code never includes a legacy header. */
void wc3_legacy_mouse_move(float nx, float ny);
void wc3_legacy_mouse_move_delta(int dx, int dy);
void wc3_legacy_mouse_button(int down);
void wc3_legacy_mouse_button_ex(int button, int down);
void wc3_legacy_mouse_wheel(int delta);
void wc3_legacy_key_event(int vk, int down);
void wc3_legacy_release_all(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_LEGACY_SHIM_H */
