/*
 * display_hle.c - target-side USER32 display-mode imports.
 *
 * The guest sees a single Android display whose mode table is derived from the
 * native surface: heights start at 360 and grow in 180-pixel steps to the
 * native height, with the width chosen to keep the screen aspect ratio.  That
 * table and the selected mode live in the as-built HLE for now, so this slice
 * only translates between the Win32 structures and the shim accessors.
 *
 * ChangeDisplaySettingsA and ChangeDisplaySettingsExA do not share an argument
 * layout.  The Ex form inserts lpszDeviceName before the DEVMODEA and hwnd +
 * dwFlags after it.  The as-built table pointed both names at the A-shaped
 * handler, so the Ex call from game.dll read the device-name pointer as a
 * DEVMODEA.  The rows are split here.  Device name, hwnd and lParam are
 * intentionally ignored: Android has one display and no desktop to restore.
 */
#include "display_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "hle_bridge.h"

#define DEVMODEA_SIZE 156u
#define DEVMODE_DM_SIZE_OFFSET 36u
#define DEVMODE_DM_FIELDS_OFFSET 40u
#define DEVMODE_BITS_PER_PEL_OFFSET 104u
#define DEVMODE_PELS_WIDTH_OFFSET 108u
#define DEVMODE_PELS_HEIGHT_OFFSET 112u
#define DEVMODE_DISPLAY_FREQUENCY_OFFSET 120u

#define DM_BITSPERPEL 0x00040000u
#define DM_PELSWIDTH 0x00080000u
#define DM_PELSHEIGHT 0x00100000u
#define DM_DISPLAYFREQUENCY 0x00400000u

#define ENUM_CURRENT_SETTINGS 0xFFFFFFFFu
#define ENUM_REGISTRY_SETTINGS 0xFFFFFFFEu

#define DISP_CHANGE_SUCCESSFUL 0u
#define DISP_CHANGE_BADMODE 0xFFFFFFFEu
#define CDS_TEST 0x00000002u

/* The as-built handler filled 32 bytes with the adapter name at offset 0.
 * game.dll only passes the nested string (offset 4) back as lpszDeviceName,
 * which the display handlers ignore, so the smaller historical write is kept
 * until a T1 observation justifies changing the DISPLAY_DEVICEA fields. */
#define ENUM_DEVICE_BYTES 32u
#define DISPLAY_NAME "\\\\.\\DISPLAY1"

static int write_devmode(uint32_t dm, uint32_t width, uint32_t height)
{
    if (!dm || !wc3_legacy_guest_ok(dm, DEVMODEA_SIZE))
        return 0;

    wc3_legacy_guest_zero(dm, DEVMODEA_SIZE);
    wc3_legacy_wr16g(dm + DEVMODE_DM_SIZE_OFFSET, (uint16_t)DEVMODEA_SIZE);
    wc3_legacy_wr32g(dm + DEVMODE_DM_FIELDS_OFFSET,
                     DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                         DM_DISPLAYFREQUENCY);
    wc3_legacy_wr32g(dm + DEVMODE_BITS_PER_PEL_OFFSET, 32);
    wc3_legacy_wr32g(dm + DEVMODE_PELS_WIDTH_OFFSET, width);
    wc3_legacy_wr32g(dm + DEVMODE_PELS_HEIGHT_OFFSET, height);
    wc3_legacy_wr32g(dm + DEVMODE_DISPLAY_FREQUENCY_OFFSET,
                     display_refresh_hz());
    return 1;
}

static void enumerate_settings(CPU *c)
{
    const uint32_t mode = wc3_legacy_arg32(c, 1);
    const uint32_t dm = wc3_legacy_arg32(c, 2);
    uint32_t width = 0, height = 0;

    if (!dm || !wc3_legacy_guest_ok(dm, DEVMODEA_SIZE)) {
        c->eax = 0;
        return;
    }

    if (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS) {
        /* Android has no separate registry mode.  Both sentinels report the
         * selected mode; the native (last-table) mode is the fallback. */
        if (!wc3_legacy_current_display_mode(&width, &height) &&
            !wc3_legacy_default_display_mode(&width, &height)) {
            c->eax = 0;
            return;
        }
    } else if (!display_mode_at(mode, &width, &height)) {
        c->eax = 0;
        return;
    }

    c->eax = (uint32_t)write_devmode(dm, width, height);
}

static void enumerate_devices(CPU *c)
{
    const uint32_t index = wc3_legacy_arg32(c, 1);
    const uint32_t device = wc3_legacy_arg32(c, 2);

    if (index != 0 || !device || !wc3_legacy_guest_ok(device,
                                                      ENUM_DEVICE_BYTES)) {
        c->eax = 0;
        return;
    }

    wc3_legacy_guest_zero(device, ENUM_DEVICE_BYTES);
    for (unsigned i = 0; i < sizeof DISPLAY_NAME; i++)
        wc3_legacy_wr8g(device + i, (uint8_t)DISPLAY_NAME[i]);
    c->eax = 1;
}

static void apply_mode(CPU *c, uint32_t dm, uint32_t flags)
{
    if (!dm) {
        /* ChangeDisplaySettings(NULL, 0) means "back to the desktop".  There
         * is no separate desktop on Android, so keep the selected mode. */
        c->eax = DISP_CHANGE_SUCCESSFUL;
        return;
    }
    if (!wc3_legacy_guest_ok(dm, DEVMODEA_SIZE)) {
        c->eax = DISP_CHANGE_BADMODE;
        return;
    }

    const uint32_t width = wc3_legacy_rd32g(dm + DEVMODE_PELS_WIDTH_OFFSET);
    const uint32_t height = wc3_legacy_rd32g(dm + DEVMODE_PELS_HEIGHT_OFFSET);
    if (!width || !height) {
        c->eax = DISP_CHANGE_SUCCESSFUL;
        return;
    }

    /* The game's hardcoded fallback table is 800x600 and 640x480.  A pair
     * outside the device-relative table must fail here, otherwise the
     * fallback replaces the mode EnumDisplaySettings(ENUM_CURRENT_SETTINGS)
     * just returned. */
    if (!display_mode_supported(width, height)) {
        c->eax = DISP_CHANGE_BADMODE;
        return;
    }

    if (!(flags & CDS_TEST))
        wc3_legacy_select_display_mode(width, height);
    c->eax = DISP_CHANGE_SUCCESSFUL;
}

static void change_settings(CPU *c)
{
    apply_mode(c, wc3_legacy_arg32(c, 0), wc3_legacy_arg32(c, 1));
}

static void change_settings_ex(CPU *c)
{
    /* lpszDeviceName, lpDevMode, hwnd, dwFlags, lParam */
    apply_mode(c, wc3_legacy_arg32(c, 1), wc3_legacy_arg32(c, 3));
}

typedef struct DisplayOverride {
    const char *name;
    void (*impl)(CPU *c);
} DisplayOverride;

static const DisplayOverride g_overrides[] = {
    { "EnumDisplaySettingsA", enumerate_settings },
    { "EnumDisplaySettingsExA", enumerate_settings },
    { "EnumDisplayDevicesA", enumerate_devices },
    { "ChangeDisplaySettingsA", change_settings },
    { "ChangeDisplaySettingsExA", change_settings_ex },
};

int user32_display_install(void)
{
    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("USER32.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0]);
}
