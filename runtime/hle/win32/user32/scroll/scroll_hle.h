/*
 * scroll_hle.h - target-side USER32 scroll-bar state.
 *
 * The game imports GetScrollInfo, SetScrollInfo and SetScrollPos but the
 * as-built table defines none of them.  The target keeps the observable
 * per-window, per-bar range/page/position state; Android has no native
 * scrollbar chrome, so drawing and scroll notifications stay outside this
 * slice.
 */
#ifndef WC3_HLE_USER32_SCROLL_H
#define WC3_HLE_USER32_SCROLL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of rows installed, or -1 on a failed registration. */
int wc3_hle_user32_scroll_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_USER32_SCROLL_H */
