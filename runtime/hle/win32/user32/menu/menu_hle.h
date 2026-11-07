/*
 * menu_hle.h - target-side USER32 menu objects.
 *
 * The as-built table only creates fake menu handles, reports zero items and
 * always returns no menu for a window.  The game imports the menu-item info,
 * insertion and popup APIs, which were bound to stubs.  This slice owns one
 * consistent menu store so create/insert/update/enumerate/delete all agree.
 */
#ifndef WC3_HLE_USER32_MENU_H
#define WC3_HLE_USER32_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of rows installed, or -1 on a failed registration. */
int wc3_hle_user32_menu_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_USER32_MENU_H */
