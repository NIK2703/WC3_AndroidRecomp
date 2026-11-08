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
 *
 * The table has two lock levels.  g_dc_table_lock owns slot identity
 * (used/hdc) and allocation; g_dc_locks[i] owns slot i's colors, background
 * mode and text alignment.  A lookup pins a slot by taking its mutex while the
 * table lock is still held, so DeleteDC/install cannot clear or reuse the slot
 * underneath the caller.  Lock order is table -> slot; no slot lock is held
 * while taking the table lock.
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
static pthread_mutex_t g_dc_table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_dc_locks[GDI_DC_TABLE_SIZE];
static pthread_once_t g_dc_locks_once = PTHREAD_ONCE_INIT;

static void init_dc_locks(void)
{
    for (unsigned i = 0; i < GDI_DC_TABLE_SIZE; i++)
        pthread_mutex_init(&g_dc_locks[i], NULL);
}

static void ensure_dc_locks(void)
{
    (void)pthread_once(&g_dc_locks_once, init_dc_locks);
}

static int is_target_handle(uint32_t hdc)
{
    return hdc >= GDI_DC_HANDLE_BASE &&
           hdc < GDI_DC_HANDLE_BASE + GDI_DC_TABLE_SIZE * GDI_DC_HANDLE_STRIDE;
}

/* Identity helpers.  Callers hold g_dc_table_lock; these never read the
 * payload fields. */
static unsigned find_state_index_locked(uint32_t hdc)
{
    if (!hdc) return GDI_DC_TABLE_SIZE;
    for (unsigned i = 0; i < GDI_DC_TABLE_SIZE; i++) {
        if (g_dcs[i].used && g_dcs[i].hdc == hdc) return i;
    }
    return GDI_DC_TABLE_SIZE;
}

static unsigned free_slot_index_locked(void)
{
    for (unsigned i = 0; i < GDI_DC_TABLE_SIZE; i++) {
        if (!g_dcs[i].used) return i;
    }
    return GDI_DC_TABLE_SIZE;
}

/* Caller holds the table lock and the slot lock. */
static void init_state_locked(GdiDcState *state, uint32_t hdc)
{
    memset(state, 0, sizeof *state);
    state->used = 1;
    state->hdc = hdc;
    state->text_color = 0x00000000u;       /* black */
    state->background_color = 0x00FFFFFFu; /* white */
    state->background_mode = 2u;           /* OPAQUE */
    state->text_align = 0u;                /* TA_LEFT | TA_TOP | TA_NOUPDATECP */
}

/* Resolve hdc to a slot with its mutex held.  Mirrors the former ensure_state:
 * an existing entry wins, null and target handles are never created, and a new
 * foreign entry takes the first free slot.  Creation runs under the table lock
 * and the slot lock, so a new slot is published fully initialized. */
static GdiDcState *lock_state(uint32_t hdc)
{
    GdiDcState *state = NULL;

    pthread_mutex_lock(&g_dc_table_lock);
    unsigned index = find_state_index_locked(hdc);
    if (index < GDI_DC_TABLE_SIZE) {
        pthread_mutex_lock(&g_dc_locks[index]);
        state = &g_dcs[index];
    } else if (hdc && !is_target_handle(hdc)) {
        index = free_slot_index_locked();
        if (index < GDI_DC_TABLE_SIZE) {
            pthread_mutex_lock(&g_dc_locks[index]);
            init_state_locked(&g_dcs[index], hdc);
            state = &g_dcs[index];
        }
    }
    pthread_mutex_unlock(&g_dc_table_lock);
    return state;
}

static void unlock_state(GdiDcState *state)
{
    pthread_mutex_unlock(&g_dc_locks[(unsigned)(state - g_dcs)]);
}

static void h_set_text_color(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    const uint32_t color = wc3_legacy_arg32(c, 1u);
    c->eax = GDI_CLR_INVALID;

    GdiDcState *state = lock_state(hdc);
    if (!state) return;

    const uint32_t previous = state->text_color;
    state->text_color = color;
    unlock_state(state);
    c->eax = previous;
}

static void h_get_text_color(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    c->eax = GDI_CLR_INVALID;

    GdiDcState *state = lock_state(hdc);
    if (!state) return;

    const uint32_t color = state->text_color;
    unlock_state(state);
    c->eax = color;
}

static void h_set_bk_color(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    const uint32_t color = wc3_legacy_arg32(c, 1u);
    c->eax = GDI_CLR_INVALID;

    GdiDcState *state = lock_state(hdc);
    if (!state) return;

    const uint32_t previous = state->background_color;
    state->background_color = color;
    unlock_state(state);
    c->eax = previous;
}

static void h_get_bk_color(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    c->eax = GDI_CLR_INVALID;

    GdiDcState *state = lock_state(hdc);
    if (!state) return;

    const uint32_t color = state->background_color;
    unlock_state(state);
    c->eax = color;
}

static void h_set_bk_mode(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    const uint32_t mode = wc3_legacy_arg32(c, 1u);
    c->eax = 0;

    GdiDcState *state = lock_state(hdc);
    if (!state) return;

    const uint32_t previous = state->background_mode;
    state->background_mode = mode;
    unlock_state(state);
    c->eax = previous;
}

static void h_get_bk_mode(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    c->eax = 0;

    GdiDcState *state = lock_state(hdc);
    if (!state) return;

    const uint32_t mode = state->background_mode;
    unlock_state(state);
    c->eax = mode;
}

static void h_set_text_align(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    const uint32_t align = wc3_legacy_arg32(c, 1u);
    c->eax = GDI_ERROR;

    GdiDcState *state = lock_state(hdc);
    if (!state) return;

    const uint32_t previous = state->text_align;
    state->text_align = align;
    unlock_state(state);
    c->eax = previous;
}

static void h_get_text_align(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    c->eax = GDI_ERROR;

    GdiDcState *state = lock_state(hdc);
    if (!state) return;

    const uint32_t align = state->text_align;
    unlock_state(state);
    c->eax = align;
}

static void h_create_compatible_dc(CPU *c)
{
    uint32_t hdc = 0;

    pthread_mutex_lock(&g_dc_table_lock);
    const unsigned index = free_slot_index_locked();
    if (index < GDI_DC_TABLE_SIZE) {
        hdc = GDI_DC_HANDLE_BASE + index * GDI_DC_HANDLE_STRIDE;
        pthread_mutex_lock(&g_dc_locks[index]);
        /* Publish only table-owned identity while the table lock still makes
         * the slot choice atomic.  lock_state may find this identity after the
         * table lock is dropped, but it must block on the slot lock until the
         * payload defaults below are complete. */
        g_dcs[index].used = 1;
        g_dcs[index].hdc = hdc;
        pthread_mutex_unlock(&g_dc_table_lock);

        g_dcs[index].text_color = 0x00000000u;       /* black */
        g_dcs[index].background_color = 0x00FFFFFFu; /* white */
        g_dcs[index].background_mode = 2u;           /* OPAQUE */
        g_dcs[index].text_align = 0u;                /* TA_LEFT | TA_TOP | TA_NOUPDATECP */
        pthread_mutex_unlock(&g_dc_locks[index]);
    } else {
        pthread_mutex_unlock(&g_dc_table_lock);
    }
    c->eax = hdc;
}

static void h_delete_dc(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    uint32_t deleted = 0;

    /* Only target handles are owned by this table; foreign handles keep the
     * as-built behavior and are never deleted here. */
    if (is_target_handle(hdc)) {
        pthread_mutex_lock(&g_dc_table_lock);
        const unsigned index = find_state_index_locked(hdc);
        if (index < GDI_DC_TABLE_SIZE) {
            pthread_mutex_lock(&g_dc_locks[index]);
            /* Retire the identity while the table lock still owns it.  A
             * creator may select this slot after seeing used=0, but must wait
             * on the same slot lock before writing defaults. */
            g_dcs[index].used = 0u;
            g_dcs[index].hdc = 0u;
            pthread_mutex_unlock(&g_dc_table_lock);

            g_dcs[index].text_color = 0u;
            g_dcs[index].background_color = 0u;
            g_dcs[index].background_mode = 0u;
            g_dcs[index].text_align = 0u;
            pthread_mutex_unlock(&g_dc_locks[index]);
            deleted = 1u;
        } else {
            pthread_mutex_unlock(&g_dc_table_lock);
        }
    }
    c->eax = deleted;
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
    ensure_dc_locks();

    /* Bootstrap reset.  The table lock plus every slot lock gives the clear
     * the same exclusivity the former single lock had, in table -> slot
     * order. */
    pthread_mutex_lock(&g_dc_table_lock);
    for (unsigned i = 0; i < GDI_DC_TABLE_SIZE; i++)
        pthread_mutex_lock(&g_dc_locks[i]);
    memset(g_dcs, 0, sizeof g_dcs);
    for (unsigned i = GDI_DC_TABLE_SIZE; i > 0u; i--)
        pthread_mutex_unlock(&g_dc_locks[i - 1u]);
    pthread_mutex_unlock(&g_dc_table_lock);

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
