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

/* Bind an existing as-built row to the fast HLE bridge without replacing its
 * handler.  `argc` and `cdecl_abi` come from the as-built HleEnt, so the
 * frozen stack-cleanup formula is unchanged and the as-built row itself is not
 * modified.  Must run before the first guest thread; returns 0 on success,
 * -1 when the row is absent or has no handler. */
int wc3_legacy_hle_fast_bind(const char *dll, const char *name);

/* Install a target handler and return the as-built handler it replaced.  The
 * wrapper may call the previous handler and then extend the observable state;
 * this is the strangler seam for calls whose side effects still live in the
 * quarantine (window creation and the input message pump). */
int wc3_legacy_hle_chain(const char *dll, const char *name,
                         void (*impl)(CPU *c),
                         void (**previous)(CPU *c));

/* The as-built last-error cell is thread-local static state.  These accessors
 * call its registered getter/setter so target handlers can report a Win32
 * error without duplicating the quarantine symbol. */
void wc3_legacy_set_last_error(uint32_t code);
uint32_t wc3_legacy_get_last_error(void);

/* Register an import row the as-built table did not define.  The resolver
 * creates a guessed-arity stub for an unknown import; this replaces that stub
 * (or appends a row when the import has not been bound yet) during bootstrap.
 * `cdecl_abi` selects the caller-pops convention, so a cdecl CRT import must
 * pass 1 even when `argc` is known.  Must run before the first guest thread;
 * returns 0 on success, -1 for a bad argument or a full registry. */
int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c));

/* Bind an ordinal-only import row.  The as-built loader creates such a row
 * with an empty name when the PE import has no name, then binds it by ordinal.
 * A target module cannot address that row through wc3_legacy_hle_add(), whose
 * key is the name; this call finds the existing empty-name row (or appends one
 * before the import walk) and pins the handler and arity in place.  It is the
 * ordinal counterpart of wc3_legacy_hle_override and must run before the first
 * guest thread.  Returns 0 on success, -1 on a bad argument or a full registry. */
int wc3_legacy_hle_bind_ordinal(const char *dll, uint16_t ordinal, int argc,
                                int cdecl_abi, void (*impl)(CPU *c));

/* Guest VA of a registry row, or 0 when the row is absent.  A row can be used
 * as a guest thread start address: dispatch() recognises the HLE window and
 * calls the registered handler with that thread's own CPU. */
uint32_t wc3_legacy_hle_thunk_of(const char *dll, const char *name);

/*
 * A-12.4 S2 / A-6.4: a genuine winmm callback thread.
 *
 * winmm delivers WOM_DONE from its own serialised thread, with its own TEB,
 * TLS and dispatcher state; Miles relies on that (waveOutProc must never call
 * waveOutWrite).  The as-built guest_thread_spawn() is static in the
 * read-only quarantine, so this shim reaches it through its registered
 * KERNEL32!CreateThread row instead of editing runtime/legacy.  A private HLE
 * row is registered as the thread's start address; its handler runs `body`
 * with the new thread's CPU.  The body is expected to loop for the process
 * lifetime and wait with wc3_legacy_guest_callback_wait().
 *
 * Returns 0 when the thread is running (or already running), -1 on failure.
 * Idempotent: exactly one callback thread exists.
 */
typedef void (*Wc3LegacyGuestLoop)(CPU *c, void *opaque);
int  wc3_legacy_guest_callback_thread_start(Wc3LegacyGuestLoop body,
                                            void *opaque);
/* Lock-free from the real-time audio path: only sem_post()/sem_wait(), no
 * mutex, no allocation, no guest memory access.  wait() returns 0 when woken
 * and -1 when the callback thread was never started. */
void wc3_legacy_guest_callback_wake(void);
int  wc3_legacy_guest_callback_wait(void);

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
/* Ask the guest thread to make the game write its in-memory preferences to
 * the registry.  Safe from any host thread; the save itself runs on the guest
 * thread at the next PeekMessage/GetMessage boundary. */
void wc3_android_request_settings_save(void);

void wc3_legacy_mouse_move(float nx, float ny);
void wc3_legacy_mouse_move_delta(int dx, int dy);
void wc3_legacy_mouse_button(int down);
void wc3_legacy_mouse_button_ex(int button, int down);
void wc3_legacy_mouse_wheel(int delta);
void wc3_legacy_key_event(int vk, int down);
void wc3_legacy_release_all(void);

/*
 * Soft-keyboard/text-field bridge.  The as-built HLE turns IMM focus into
 * show/hide callbacks and turns GameActivity text-state edits into guest
 * WM_CHAR/WM_KEYDOWN messages.  The host installs the callbacks and forwards
 * the platform's IME events; it never reaches into runtime/legacy directly.
 */
void wc3_legacy_set_keyboard_bridge(void (*show)(void), void (*hide)(void));
void wc3_legacy_set_keyboard_visible(int visible);
void wc3_legacy_post_char(uint32_t ch);
void wc3_legacy_post_backspace(void);
void wc3_legacy_post_enter(void);

/* One decoded UTF-16 unit of Java modified UTF-8.  Returns consumed bytes. */
int wc3_legacy_mutf8_next(const char *text, int length, int offset,
                          uint16_t *unit);

typedef struct {
    int old_start;
    int new_start;
    int removed;
    int added;
} Wc3LegacyTextDiff;

/* Same suffix-diff rule as the as-built hidden EditText bridge. */
void wc3_legacy_text_diff(const char *old_text, int old_length,
                          const char *new_text, int new_length,
                          Wc3LegacyTextDiff *diff);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_LEGACY_SHIM_H */
