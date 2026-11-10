/*
 * hle_android.h - POSIX HLE surface for the lifted WC3 modules on Android.
 *
 * The Windows host keeps its Win32/CRT surface in hle.c.  This is the Android
 * counterpart: same idea (only OS and CRT services are replaced, every game
 * address still comes from the original PE and runs as lifted code), but it
 * implements the POSIX side of the services the launcher needs and reports
 * anything it does not implement instead of pretending it worked.
 */
#ifndef WC3_HLE_ANDROID_H
#define WC3_HLE_ANDROID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time setup: records the executable path used by GetModuleFileNameA and
 * GetCommandLineA, and allocates the CRT data cells (_acmdln, _p__fmode, ...)
 * in guest memory.  Must run after the guest arenas are mapped and before the
 * first import is bound. */
void hle_android_start(const char *exe_path, const char *cmdline);

/* Device-relative display modes.  Heights start at 360 pixels and grow in
 * 180-pixel steps up to the native surface height (1080 gives 360, 540, 720,
 * 900, 1080); each width is derived from the surface aspect ratio.  The
 * native height is always the last entry, even when it is not an exact
 * multiple of the step.  `set_screen_size` must be called before the guest
 * enumerates modes; the activity calls it as soon as the native window exists,
 * and the shell falls back to WC3_DXVK_WIDTH/HEIGHT (or 800x600).
 *
 * The current mode is what GetSystemMetrics, EnumDisplaySettings(-1) and
 * D3D9's GetAdapterDisplayMode report.  `select` accepts an exact table entry
 * only, so the guest's hardcoded fallback modes (800x600/640x480) cannot
 * silently replace the device-relative choice. */
#define HLE_ANDROID_DISPLAY_MODE_MAX 32
void     hle_android_set_screen_size(uint32_t width, uint32_t height);
uint32_t hle_android_display_generation(void);
uint32_t hle_android_display_mode_count(void);
int      hle_android_display_mode(uint32_t index, uint32_t *width, uint32_t *height);
int      hle_android_current_display_mode(uint32_t *width, uint32_t *height);
int      hle_android_default_display_mode(uint32_t *width, uint32_t *height);
int      hle_android_select_display_mode(uint32_t width, uint32_t height);
int      hle_android_display_mode_supported(uint32_t width, uint32_t height);
uint32_t hle_android_display_refresh_rate(void);

/* Platform layer: set the physical Android buffer geometry and publish the
 * same present size to DXVK's Win32 shim.  This must be the native surface
 * size, not the guest display mode: DXVK keeps the swap chain at this extent
 * and its blitter scales the guest back buffer to fill it.  Returns 0 on
 * success, or the negative errno from ANativeWindow_setBuffersGeometry.  A
 * windowless shell updates only the published size and returns 0. */
int wc3_android_set_surface_size(uint32_t width, uint32_t height);

/* Print the import report: resolved/unimplemented imports, call counts, and
 * the last calls that ran.  Safe to call from the crash handler (it only
 * formats into a local buffer and write(2)s it). */
void hle_android_report(void);

/* True when [addr, addr+size) is mapped guest memory (static arenas plus
 * ranges committed after startup).  Used by the thin D3D9 COM bridge before
 * a native COM method reads a guest pointer. */
int hle_android_guest_ok(uint32_t addr, uint32_t size);

/* Guest-heap helpers shared with the boot harness. */
uint32_t hle_guest_alloc(uint32_t size);
void     hle_guest_free(uint32_t addr);
uint32_t hle_guest_strdup(const char *s);
uint32_t hle_guest_strdup_w(const uint16_t *s);

/* Number of distinct imports that fell through to the generic stub. */
int         hle_missing_count(void);
const char *hle_missing_at(int i);
unsigned    hle_missing_calls(int i);

/* Boot milestone: when the guest asks for game.dll the probe has done its job.
 * Set to 0 to keep going instead (the caller then gets a NULL module). */
void hle_stop_on_game_dll(int on);
int  hle_reached_game_dll(void);

/* Real module loading.  hle_android.c does not own a PE mapper, so the probe
 * installs one: `load` maps the DLL, binds its imports, calls its DllMain and
 * returns 0 on success with the base and image filled in.  `export` resolves a
 * name or ordinal inside a previously loaded module.
 *
 * Without these, LoadLibraryA of anything but a system DLL fails and
 * GetProcAddress only answers for the HLE thunks - which is fine for the
 * launcher-only milestone but not for running game.dll. */
typedef int      (*hle_dll_loader)(const char *name, uint32_t *out_base,
                                   const void **out_img);
typedef uint32_t (*hle_dll_export)(const void *img, const char *name,
                                   uint16_t ordinal);
void hle_set_dll_hooks(hle_dll_loader load, hle_dll_export exp);

/* Android-only one-shot hook.  The boot probe maps Mss32 before the game and
 * used to run its DllMain immediately; doing that before the D3D9 device is
 * created corrupts the Vulkan/DXVK init on this device.  The probe registers
 * a callback here and the D3D9 HLE calls it once after CreateDevice succeeds;
 * Mss32 has to be initialised before the game reaches AIL_startup, which
 * happens after the renderer is up.  `cpu` is the guest CPU that entered the
 * D3D9 import; the callback saves and restores its register state. */
void hle_android_set_after_d3d9(void (*cb)(void *cpu));
void hle_android_after_d3d9(void *cpu);

/* Wrap game.dll's AIL_startup import so the deferred Mss32 DllMain has run
 * before Miles is asked to start.  Returns the guest thunk; the thunk calls
 * `real` (the Mss32 export) and preserves Miles' stdcall convention. */
uint32_t hle_mss_hook_ail_startup(uint32_t real);

/* Diagnostic wrappers for the Miles exports the game calls.  Returns a guest
 * thunk when `name` is one of the traced exports, otherwise 0.  The wrapper
 * logs the stdcall arguments and the real export's return value. */
uint32_t hle_mss_hook_trace(const char *name, uint32_t real);

/* Touch input from the activity's AInputQueue.  Coordinates are normalised
 * to the client area: (0,0) is the top-left corner and (1,1) the bottom-right.
 * The HLE keeps a virtual Win32 cursor and left-button state, which is what
 * the guest polls through GetCursorPos/GetKeyState.  A button release is held
 * for a few frames so a tap shorter than one game tick is not lost. */
void hle_android_mouse_move(float nx, float ny);
/* Relative motion in client pixels, used by the touch engine's trackpad and
 * on-screen movement bindings. */
void hle_android_mouse_move_delta(int dx, int dy);
void hle_android_mouse_button(int down);
/* button: 0=left, 1=middle, 2=right (Winlator order). */
void hle_android_mouse_button_ex(int button, int down);
/* delta is in Windows wheel units: +/-120 per notch. */
void hle_android_mouse_wheel(int delta);
/* vk may be a specific left/right modifier VK; the HLE maps it to the generic
 * WM_KEYDOWN/WM_SYSKEYDOWN wParam and keeps GetKeyState coherent. */
void hle_android_key_event(int vk, int down);
/* Post UP messages and clear state for every synthetic key/button still down. */
void hle_android_release_all(void);
/* Wait until the guest's message loop has dispatched the requested button
 * state.  Returns 1 on success, 0 on timeout.  Used by the tap test hook so
 * it never sends UP before the game saw DOWN. */
int hle_android_wait_mouse(int down, int timeout_ms);

/* Soft-keyboard bridge.  The Java activity owns the invisible EditText; the
 * HLE shows/hides it from the guest's IMM calls and turns its edits into
 * WM_CHAR/WM_KEYDOWN messages in the same queue the mouse uses.  The APK
 * front end installs the callbacks; the shell leaves them NULL. */
void hle_android_set_keyboard_bridge(void (*show)(void), void (*hide)(void));
/* The platform reports the real IME visibility.  A keyboard dismissed by the
 * system/Back button must clear the HLE's shown flag or the next text-field
 * focus will not request it again. */
void hle_android_set_keyboard_visible(int visible);
void wc3_android_show_keyboard(void);
void wc3_android_hide_keyboard(void);
void hle_android_post_char(uint32_t ch);
void hle_android_post_backspace(void);
void hle_android_post_enter(void);

/* Register a module the probe mapped itself (war3.exe, Storm.dll) so that
 * GetModuleHandleA and GetProcAddress see it. */
void hle_register_module(const char *name, uint32_t base, const void *img);

#if WC3_DEBUG
/* Same flag the generated dispatcher reads. */
extern int g_dispatch_diag;
extern int g_stack_guard;
#endif

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_ANDROID_H */
