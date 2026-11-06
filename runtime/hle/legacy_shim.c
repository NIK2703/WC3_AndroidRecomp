/*
 * legacy_shim.c - R15/H6.3 boundary implementation.
 *
 * This is the only target-tree translation unit that includes the quarantined
 * as-built headers.  Everything else consumes legacy_shim.h.
 */
#include "hle_registry.h"
#include "hle_android.h"

#include "legacy_shim.h"

_Static_assert(HLE_MAX == WC3_LEGACY_HLE_MAX,
               "legacy HLE registry size changed; update legacy_shim.h");
_Static_assert(HLE_STRIDE == WC3_LEGACY_HLE_STRIDE,
               "legacy HLE thunk stride changed; update legacy_shim.h");

void wc3_legacy_hle_call(uint32_t thunk, CPU *c)
{
    hle_call(thunk, c);
}

const char *wc3_legacy_hle_describe(uint32_t thunk)
{
    return hle_describe(thunk);
}

void wc3_legacy_set_screen_size(uint32_t width, uint32_t height)
{
    hle_android_set_screen_size(width, height);
}

int wc3_legacy_default_display_mode(uint32_t *width, uint32_t *height)
{
    return hle_android_default_display_mode(width, height);
}

int wc3_legacy_select_display_mode(uint32_t width, uint32_t height)
{
    return hle_android_select_display_mode(width, height);
}

void wc3_legacy_mouse_move(float nx, float ny)
{
    hle_android_mouse_move(nx, ny);
}

void wc3_legacy_mouse_button(int down)
{
    hle_android_mouse_button(down);
}
