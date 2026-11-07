/*
 * gdi_dc_hle.c - target-side GDI32 DC state.
 *
 * The game calls SetTextColor, SetBkColor, SetBkMode and SetTextAlign around
 * its text paths and then reads the values back to restore them.  The legacy
 * table only had constant-returning SetBkMode/SetTextColor handlers and no
 * SetBkColor/GetBkColor, so these rows supply the actual per-DC contract.
 *
 * HDCs created by the target (CreateCompatibleDC) live in a small handle table.
 * HDCs already handed out by the as-built GetDC are accepted as foreign keys
 * and get a lazy entry, which keeps the migration seam compatible while the
 * window/DC ownership is still legacy.
 */
#include "gdi_dc_hle.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"

#define GDI_CLR_INVALID 0xFFFFFFFFu
#define GDI_ERROR 0xFFFFFFFFu

#define GDI_DC_TABLE_SIZE 64u
#define GDI_DC_HANDLE_BASE 0x000D0000u
#define GDI_DC_HANDLE_STRIDE 4u

typedef struct GdiDcState {
    uint8_t used;
    uint32_t hdc;
    uint32_t text_color;
    uint32_t background_color;
    uint32_t background_mode;
    uint32_t text_align;
} GdiDcState;

static GdiDcState g_dcs[GDI_DC_TABLE_SIZE];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int is_target_handle(uint32_t hdc)
{
    return hdc >= GDI_DC_HANDLE_BASE &&
           hdc < GDI_DC_HANDLE_BASE + GDI_DC_TABLE_SIZE * GDI_DC_HANDLE_STRIDE;
}

static GdiDcState *find_state(uint32_t hdc)
{
    if (!hdc) return NULL;
    for (unsigned i = 0; i < GDI_DC_TABLE_SIZE; i++) {
        if (g_dcs[i].used && g_dcs[i].hdc == hdc) return &g_dcs[i];
    }
    return NULL;
}

static GdiDcState *free_slot(void)
{
    for (unsigned i = 0; i < GDI_DC_TABLE_SIZE; i++) {
        if (!g_dcs[i].used) return &g_dcs[i];
    }
    return NULL;
}

static void init_state(GdiDcState *state, uint32_t hdc)
{
    memset(state, 0, sizeof *state);
    state->used = 1;
    state->hdc = hdc;
    state->text_color = 0x00000000u;       /* black */
    state->background_color = 0x00FFFFFFu; /* white */
    state->background_mode = 2u;           /* OPAQUE */
    state->text_align = 0u;                /* TA_LEFT | TA_TOP | TA_NOUPDATECP */
}

static GdiDcState *ensure_state(uint32_t hdc)
{
    GdiDcState *state = find_state(hdc);
    if (state) return state;
    if (!hdc || is_target_handle(hdc)) return NULL;

    state = free_slot();
    if (!state) return NULL;
    init_state(state, hdc);
    return state;
}

static void h_set_text_color(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    const uint32_t color = wc3_legacy_arg32(c, 1u);
    c->eax = GDI_CLR_INVALID;

    pthread_mutex_lock(&g_lock);
    GdiDcState *state = ensure_state(hdc);
    if (state) {
        c->eax = state->text_color;
        state->text_color = color;
    }
    pthread_mutex_unlock(&g_lock);
}

static void h_get_text_color(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    c->eax = GDI_CLR_INVALID;

    pthread_mutex_lock(&g_lock);
    GdiDcState *state = ensure_state(hdc);
    if (state) c->eax = state->text_color;
    pthread_mutex_unlock(&g_lock);
}

static void h_set_bk_color(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    const uint32_t color = wc3_legacy_arg32(c, 1u);
    c->eax = GDI_CLR_INVALID;

    pthread_mutex_lock(&g_lock);
    GdiDcState *state = ensure_state(hdc);
    if (state) {
        c->eax = state->background_color;
        state->background_color = color;
    }
    pthread_mutex_unlock(&g_lock);
}

static void h_get_bk_color(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    c->eax = GDI_CLR_INVALID;

    pthread_mutex_lock(&g_lock);
    GdiDcState *state = ensure_state(hdc);
    if (state) c->eax = state->background_color;
    pthread_mutex_unlock(&g_lock);
}

static void h_set_bk_mode(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    const uint32_t mode = wc3_legacy_arg32(c, 1u);
    c->eax = 0;

    pthread_mutex_lock(&g_lock);
    GdiDcState *state = ensure_state(hdc);
    if (state) {
        c->eax = state->background_mode;
        state->background_mode = mode;
    }
    pthread_mutex_unlock(&g_lock);
}

static void h_get_bk_mode(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    c->eax = 0;

    pthread_mutex_lock(&g_lock);
    GdiDcState *state = ensure_state(hdc);
    if (state) c->eax = state->background_mode;
    pthread_mutex_unlock(&g_lock);
}

static void h_set_text_align(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    const uint32_t align = wc3_legacy_arg32(c, 1u);
    c->eax = GDI_ERROR;

    pthread_mutex_lock(&g_lock);
    GdiDcState *state = ensure_state(hdc);
    if (state) {
        c->eax = state->text_align;
        state->text_align = align;
    }
    pthread_mutex_unlock(&g_lock);
}

static void h_get_text_align(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    c->eax = GDI_ERROR;

    pthread_mutex_lock(&g_lock);
    GdiDcState *state = ensure_state(hdc);
    if (state) c->eax = state->text_align;
    pthread_mutex_unlock(&g_lock);
}

static void h_create_compatible_dc(CPU *c)
{
    c->eax = 0;

    pthread_mutex_lock(&g_lock);
    GdiDcState *state = free_slot();
    if (state) {
        const unsigned index = (unsigned)(state - g_dcs);
        const uint32_t hdc = GDI_DC_HANDLE_BASE + index * GDI_DC_HANDLE_STRIDE;
        init_state(state, hdc);
        c->eax = hdc;
    }
    pthread_mutex_unlock(&g_lock);
}

static void h_delete_dc(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    c->eax = 0;

    pthread_mutex_lock(&g_lock);
    if (is_target_handle(hdc)) {
        GdiDcState *state = find_state(hdc);
        if (state) {
            memset(state, 0, sizeof *state);
            c->eax = 1;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

typedef struct GdiDcOverride {
    const char *name;
    void (*impl)(CPU *c);
} GdiDcOverride;

static const GdiDcOverride g_overrides[] = {
    { "SetBkMode", h_set_bk_mode },
    { "SetTextColor", h_set_text_color },
};

typedef struct GdiDcAddition {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} GdiDcAddition;

static const GdiDcAddition g_additions[] = {
    { "SetBkColor", 2u, h_set_bk_color },
    { "GetBkColor", 1u, h_get_bk_color },
    { "GetBkMode", 1u, h_get_bk_mode },
    { "GetTextColor", 1u, h_get_text_color },
    { "SetTextAlign", 2u, h_set_text_align },
    { "GetTextAlign", 1u, h_get_text_align },
    { "CreateCompatibleDC", 1u, h_create_compatible_dc },
    { "DeleteDC", 1u, h_delete_dc },
};

int wc3_hle_gdi32_dc_install(void)
{
    memset(g_dcs, 0, sizeof g_dcs);
    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("GDI32.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("GDI32.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 0,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0] +
                 sizeof g_additions / sizeof g_additions[0]);
}
