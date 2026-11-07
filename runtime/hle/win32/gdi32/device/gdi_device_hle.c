/*
 * gdi_device_hle.c - target-side GDI display-device state.
 *
 * The game reads and restores a display gamma ramp.  The as-built table did
 * not own that state, so both calls were unresolved.  A ramp is three
 * 256-entry WORD tables (red, green, blue); keeping it per HDC preserves the
 * read/modify/write sequence without needing a real display device.
 */
#include "gdi_device_hle.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"

#define GAMMA_CHANNELS 3u
#define GAMMA_ENTRIES 256u
#define GAMMA_WORDS (GAMMA_CHANNELS * GAMMA_ENTRIES)
#define GAMMA_BYTES (GAMMA_WORDS * 2u)
#define GAMMA_HDC_MAX 16u

typedef struct GammaState {
    uint8_t used;
    uint32_t hdc;
    uint16_t ramp[GAMMA_CHANNELS][GAMMA_ENTRIES];
} GammaState;

static GammaState g_gamma[GAMMA_HDC_MAX];
static pthread_mutex_t g_gamma_lock = PTHREAD_MUTEX_INITIALIZER;

static GammaState *find_gamma(uint32_t hdc)
{
    for (unsigned i = 0; i < GAMMA_HDC_MAX; i++) {
        if (g_gamma[i].used && g_gamma[i].hdc == hdc)
            return &g_gamma[i];
    }
    return NULL;
}

static GammaState *ensure_gamma(uint32_t hdc)
{
    GammaState *state = find_gamma(hdc);
    if (state) return state;
    for (unsigned i = 0; i < GAMMA_HDC_MAX; i++) {
        if (g_gamma[i].used) continue;
        memset(&g_gamma[i], 0, sizeof g_gamma[i]);
        g_gamma[i].used = 1u;
        g_gamma[i].hdc = hdc;
        return &g_gamma[i];
    }
    return NULL;
}

static uint16_t linear_ramp_value(uint32_t index)
{
    return (uint16_t)(index * 257u);
}

static void h_set_device_gamma_ramp(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    const uint32_t ramp = wc3_legacy_arg32(c, 1u);
    if (!hdc || !wc3_legacy_guest_ok(ramp, GAMMA_BYTES)) {
        c->eax = 0u;
        return;
    }

    pthread_mutex_lock(&g_gamma_lock);
    GammaState *state = ensure_gamma(hdc);
    if (state) {
        for (uint32_t channel = 0; channel < GAMMA_CHANNELS; channel++) {
            for (uint32_t i = 0; i < GAMMA_ENTRIES; i++) {
                state->ramp[channel][i] =
                    wc3_legacy_rd16g(ramp + (channel * GAMMA_ENTRIES + i) * 2u);
            }
        }
    }
    pthread_mutex_unlock(&g_gamma_lock);
    c->eax = state ? 1u : 0u;
}

static void h_get_device_gamma_ramp(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    const uint32_t ramp = wc3_legacy_arg32(c, 1u);
    if (!hdc || !wc3_legacy_guest_ok(ramp, GAMMA_BYTES)) {
        c->eax = 0u;
        return;
    }

    pthread_mutex_lock(&g_gamma_lock);
    GammaState *state = find_gamma(hdc);
    for (uint32_t channel = 0; channel < GAMMA_CHANNELS; channel++) {
        for (uint32_t i = 0; i < GAMMA_ENTRIES; i++) {
            const uint16_t value = state
                ? state->ramp[channel][i]
                : linear_ramp_value(i);
            wc3_legacy_wr16g(ramp + (channel * GAMMA_ENTRIES + i) * 2u, value);
        }
    }
    pthread_mutex_unlock(&g_gamma_lock);
    c->eax = 1u;
}

static void h_get_device_caps(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    const uint32_t index = wc3_legacy_arg32(c, 1u);
    if (!hdc) {
        c->eax = 0u;
        return;
    }

    uint32_t width = 800u;
    uint32_t height = 600u;
    if (!wc3_legacy_current_display_mode(&width, &height)) {
        width = 800u;
        height = 600u;
    }

    switch (index) {
    case 2:  /* TECHNOLOGY */
        c->eax = 1u;  /* DT_RASDISPLAY */
        break;
    case 8:  /* HORZRES */
        c->eax = width;
        break;
    case 10: /* VERTRES */
        c->eax = height;
        break;
    case 12: /* BITSPIXEL */
        c->eax = 32u;
        break;
    case 14: /* PLANES */
        c->eax = 1u;
        break;
    case 24: /* NUMCOLORS: more than 8 bpp */
        c->eax = 0xFFFFFFFFu;
        break;
    case 88: /* LOGPIXELSX */
    case 90: /* LOGPIXELSY */
        c->eax = 96u;
        break;
    default:
        c->eax = 0u;
        break;
    }
}

typedef struct GdiDeviceAddition {
    const char *name;
    int argc;
    void (*impl)(CPU *c);
} GdiDeviceAddition;

static const GdiDeviceAddition g_additions[] = {
    { "SetDeviceGammaRamp", 2, h_set_device_gamma_ramp },
    { "GetDeviceGammaRamp", 2, h_get_device_gamma_ramp },
    { "GetDeviceCaps", 2, h_get_device_caps },
};

int wc3_hle_gdi32_device_install(void)
{
    pthread_mutex_lock(&g_gamma_lock);
    memset(g_gamma, 0, sizeof g_gamma);
    pthread_mutex_unlock(&g_gamma_lock);

    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("GDI32.dll", g_additions[i].name,
                               g_additions[i].argc, 0,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_additions / sizeof g_additions[0]);
}
