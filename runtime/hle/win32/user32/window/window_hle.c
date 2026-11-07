/*
 * window_hle.c - target-side USER32 window/display facts.
 *
 * GetSystemMetrics is the first USER32 handler to leave the quarantine: its
 * only external input is the current display mode, which the shim already
 * exposes.  The as-built default of 800 for unrelated metrics is preserved:
 * the game uses those metrics only as loose layout hints, and changing them
 * without a device measurement would be a guess.
 */
#include "window_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "legacy_shim.h"

#define DESKTOP_WINDOW_HANDLE 0x00010000u

static int32_t g_cursor_display_count;

static void h_get_desktop_window(CPU *c)
{
    c->eax = DESKTOP_WINDOW_HANDLE;
}

static void h_get_system_metrics(CPU *c)
{
    uint32_t width = 0, height = 0;
    if (!wc3_legacy_current_display_mode(&width, &height)) {
        width = 800;
        height = 600;
    }
    switch (wc3_legacy_arg32(c, 0)) {
    case 0:  /* SM_CXSCREEN */
    case 61: /* SM_CXMAXIMIZED */
    case 78: /* SM_CXVIRTUALSCREEN */
        c->eax = width;
        break;
    case 1:  /* SM_CYSCREEN */
    case 62: /* SM_CYMAXIMIZED */
    case 79: /* SM_CYVIRTUALSCREEN */
        c->eax = height;
        break;
    case 80: /* SM_CMONITORS */
        c->eax = 1;
        break;
    default:
        c->eax = 800;
        break;
    }
}

static void h_show_cursor(CPU *c)
{
    /* Win32 keeps a per-window-station display counter, initially 0 when a
     * mouse is installed.  Android has no system cursor, but the counter is
     * still guest-visible and the as-built "always return 1" was wrong after
     * the first call. */
    if (wc3_legacy_arg32(c, 0) != 0)
        g_cursor_display_count++;
    else
        g_cursor_display_count--;
    c->eax = (uint32_t)g_cursor_display_count;
}

typedef struct User32WindowOverride {
    const char *name;
    void (*impl)(CPU *c);
} User32WindowOverride;

static const User32WindowOverride g_overrides[] = {
    { "GetDesktopWindow", h_get_desktop_window },
    { "GetSystemMetrics", h_get_system_metrics },
    { "ShowCursor", h_show_cursor },
};

int wc3_hle_user32_window_install(void)
{
    g_cursor_display_count = 0;
    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("USER32.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0]);
}
