/*
 * gdi_dc_hle.h - target-side GDI32 DC state.
 *
 * The as-built table answered SetBkMode/SetTextColor with a constant and had
 * no SetBkColor/GetBkColor at all.  The game restores colors and modes around
 * text drawing, so the target keeps the small per-DC state that GDI promises:
 * text color, background color, background mode and text alignment.
 */
#ifndef WC3_HLE_GDI32_DC_H
#define WC3_HLE_GDI32_DC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of rows installed, or -1 if an override is missing. */
int wc3_hle_gdi32_dc_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_GDI32_DC_H */
