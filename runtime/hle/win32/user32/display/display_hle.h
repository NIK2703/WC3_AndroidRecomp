/*
 * display_hle.h - target-side USER32 display-mode imports.
 *
 * This is the user32/display slice of the M5 migration: the five Win32
 * entry points the game uses to enumerate adapters and modes and to request a
 * mode change.  Display state itself still lives in the quarantine until
 * app_boot moves; this module asks the shim for the device-relative table and
 * writes the Win32 DEVMODEA/DISPLAY_DEVICEA layouts.
 */
#ifndef WC3_HLE_USER32_DISPLAY_H
#define WC3_HLE_USER32_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the five USER32 rows over the as-built table.  Returns 5 on
 * success, or -1 when a row is missing.  Must run before guest threads. */
int user32_display_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_USER32_DISPLAY_H */
