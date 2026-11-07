/*
 * window_hle.h - target-side USER32 window/display facts.
 *
 * GetDesktopWindow, GetSystemMetrics and ShowCursor are the self-contained
 * part of USER32: they do not need the legacy window queue, focus table or
 * message pump.  Geometry still comes from the shim's display accessor.
 */
#ifndef WC3_HLE_USER32_WINDOW_H
#define WC3_HLE_USER32_WINDOW_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of rows installed, or -1 if a row is missing. */
int wc3_hle_user32_window_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_USER32_WINDOW_H */
