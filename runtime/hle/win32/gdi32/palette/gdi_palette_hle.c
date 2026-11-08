/*
 * gdi_palette_hle.c - target-side GDI32 palette objects.
 *
 * The palettes are guest-owned LOGPALETTE data plus one selected-palette slot
 * per HDC.  No pixels are rasterised here: CreatePalette/SetPaletteEntries
 * preserve the exact byte contract, SelectPalette returns the previous
 * selection, and RealizePalette reports the number of entries the target had
 * to change.  DeleteObject is chained after the object module so palette
 * handles are freed while regions and pens keep their existing behavior.
 */
#include "gdi_palette_hle.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"

#define PALETTE_HANDLE_BASE 0x00120000u
#define PALETTE_HANDLE_STRIDE 4u
#define PALETTE_MAX 64u
#define PALETTE_ENTRY_MAX 256u

#define DC_PALETTE_MAX 64u

#define LOGPALETTE_VERSION 0x0300u
#define SYSTEM_PALETTE_COUNT 20u

typedef struct GdiPalette {
    uint8_t used;
    uint32_t handle;
    uint16_t count;
    uint32_t revision;
    uint8_t red[PALETTE_ENTRY_MAX];
    uint8_t green[PALETTE_ENTRY_MAX];
    uint8_t blue[PALETTE_ENTRY_MAX];
    uint8_t flags[PALETTE_ENTRY_MAX];
} GdiPalette;

typedef struct PaletteDcState {
    uint8_t used;
    uint32_t hdc;
    uint32_t palette;
    uint32_t realized_revision;
    uint16_t realized_entries;
} PaletteDcState;

typedef struct SystemPaletteEntry {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} SystemPaletteEntry;

static const SystemPaletteEntry g_system_palette[SYSTEM_PALETTE_COUNT] = {
    { 0x00, 0x00, 0x00 },
    { 0x80, 0x00, 0x00 },
    { 0x00, 0x80, 0x00 },
    { 0x80, 0x80, 0x00 },
    { 0x00, 0x00, 0x80 },
    { 0x80, 0x00, 0x80 },
    { 0x00, 0x80, 0x80 },
    { 0xC0, 0xC0, 0xC0 },
    { 0xC0, 0xDC, 0xC0 },
    { 0xA6, 0xCA, 0xF0 },
    { 0x40, 0x20, 0x00 },
    { 0x60, 0x20, 0x20 },
    { 0x80, 0x20, 0x20 },
    { 0xA0, 0x20, 0x20 },
    { 0xC0, 0x20, 0x20 },
    { 0xE0, 0x20, 0x20 },
    { 0xFF, 0x20, 0x20 },
    { 0xFF, 0x40, 0x40 },
    { 0xFF, 0x60, 0x60 },
    { 0xFF, 0x80, 0x80 },
};

/* Lock order: g_dc_lock -> g_palette_table_lock -> g_palette_locks[slot].
 * The table lock owns g_palettes[i].used/.handle and first-free allocation;
 * a slot lock owns that palette's count/revision/red/green/blue/flags; and
 * g_dc_lock owns every field of g_dc_states.  Slot mutexes are initialised
 * once at install, before any handler is registered. */
static GdiPalette g_palettes[PALETTE_MAX];
static PaletteDcState g_dc_states[DC_PALETTE_MAX];
static pthread_mutex_t g_palette_table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_palette_locks[PALETTE_MAX];
static pthread_mutex_t g_dc_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_palette_locks_once = PTHREAD_ONCE_INIT;
static int g_palette_locks_ready;
static void (*g_previous_delete_object)(CPU *c);

static void init_palette_locks(void)
{
    int ready = 1;
    for (unsigned i = 0; i < PALETTE_MAX; i++) {
        if (pthread_mutex_init(&g_palette_locks[i], NULL) != 0)
            ready = 0;
    }
    g_palette_locks_ready = ready;
}

static int is_palette_handle(uint32_t handle)
{
    if (handle < PALETTE_HANDLE_BASE) return 0;
    const uint32_t offset = handle - PALETTE_HANDLE_BASE;
    return offset % PALETTE_HANDLE_STRIDE == 0u &&
           offset / PALETTE_HANDLE_STRIDE < PALETTE_MAX;
}

/* Caller must hold g_palette_table_lock.  The caller must take
 * g_palette_locks[slot] before reading or writing any payload field. */
static GdiPalette *find_palette(uint32_t handle)
{
    if (!is_palette_handle(handle)) return NULL;
    for (unsigned i = 0; i < PALETTE_MAX; i++) {
        if (g_palettes[i].used && g_palettes[i].handle == handle)
            return &g_palettes[i];
    }
    return NULL;
}

/* Caller must hold g_dc_lock. */
static PaletteDcState *find_dc_state(uint32_t hdc)
{
    for (unsigned i = 0; i < DC_PALETTE_MAX; i++) {
        if (g_dc_states[i].used && g_dc_states[i].hdc == hdc)
            return &g_dc_states[i];
    }
    return NULL;
}

/* Caller must hold g_dc_lock. */
static PaletteDcState *ensure_dc_state(uint32_t hdc)
{
    PaletteDcState *state = find_dc_state(hdc);
    if (state) return state;
    for (unsigned i = 0; i < DC_PALETTE_MAX; i++) {
        if (g_dc_states[i].used) continue;
        memset(&g_dc_states[i], 0, sizeof g_dc_states[i]);
        g_dc_states[i].used = 1u;
        g_dc_states[i].hdc = hdc;
        return &g_dc_states[i];
    }
    return NULL;
}

static void h_create_palette(CPU *c)
{
    const uint32_t logpalette = wc3_legacy_arg32(c, 0u);
    if (!logpalette || !wc3_legacy_guest_ok(logpalette, 4u)) {
        c->eax = 0u;
        return;
    }
    const uint16_t version = wc3_legacy_rd16g(logpalette);
    const uint16_t count = wc3_legacy_rd16g(logpalette + 2u);
    if (version != LOGPALETTE_VERSION || count == 0u ||
        count > PALETTE_ENTRY_MAX ||
        !wc3_legacy_guest_ok(logpalette, 4u + (uint32_t)count * 4u)) {
        c->eax = 0u;
        return;
    }

    /* Snapshot the guest LOGPALETTE bytes before taking the palette locks so
     * the critical sections perform no guest reads. */
    uint8_t red_snapshot[PALETTE_ENTRY_MAX];
    uint8_t green_snapshot[PALETTE_ENTRY_MAX];
    uint8_t blue_snapshot[PALETTE_ENTRY_MAX];
    uint8_t flags_snapshot[PALETTE_ENTRY_MAX];
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t entry = logpalette + 4u + i * 4u;
        red_snapshot[i] = wc3_legacy_rd8g(entry + 0u);
        green_snapshot[i] = wc3_legacy_rd8g(entry + 1u);
        blue_snapshot[i] = wc3_legacy_rd8g(entry + 2u);
        flags_snapshot[i] = wc3_legacy_rd8g(entry + 3u);
    }

    pthread_mutex_lock(&g_palette_table_lock);
    GdiPalette *palette = NULL;
    unsigned slot = 0u;
    for (unsigned i = 0; i < PALETTE_MAX; i++) {
        if (g_palettes[i].used) continue;
        palette = &g_palettes[i];
        slot = i;
        break;
    }
    uint32_t handle = 0u;
    if (palette) {
        handle = PALETTE_HANDLE_BASE + slot * PALETTE_HANDLE_STRIDE;
        /* Publish only the identity under the table lock and pin the slot
         * before dropping it.  Payload initialization below runs under the
         * slot lock only; a finder that sees the identity blocks here until
         * initialization completes. */
        palette->used = 1u;
        palette->handle = handle;
        pthread_mutex_lock(&g_palette_locks[slot]);
    }
    pthread_mutex_unlock(&g_palette_table_lock);
    if (palette) {
        memset(palette->red, 0, sizeof palette->red);
        memset(palette->green, 0, sizeof palette->green);
        memset(palette->blue, 0, sizeof palette->blue);
        memset(palette->flags, 0, sizeof palette->flags);
        palette->count = count;
        palette->revision = 1u;
        memcpy(palette->red, red_snapshot, (size_t)count);
        memcpy(palette->green, green_snapshot, (size_t)count);
        memcpy(palette->blue, blue_snapshot, (size_t)count);
        memcpy(palette->flags, flags_snapshot, (size_t)count);
        pthread_mutex_unlock(&g_palette_locks[slot]);
    }
    c->eax = handle;
}

static void h_set_palette_entries(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t start = wc3_legacy_arg32(c, 1u);
    const uint32_t count = wc3_legacy_arg32(c, 2u);
    const uint32_t entries = wc3_legacy_arg32(c, 3u);
    if (count == 0u || count > PALETTE_ENTRY_MAX || !entries ||
        !wc3_legacy_guest_ok(entries, count * 4u)) {
        c->eax = 0u;
        return;
    }

    /* Snapshot the guest entries before taking the palette locks.  The range
     * was validated above and reads have no side effects. */
    uint8_t red_snapshot[PALETTE_ENTRY_MAX];
    uint8_t green_snapshot[PALETTE_ENTRY_MAX];
    uint8_t blue_snapshot[PALETTE_ENTRY_MAX];
    uint8_t flags_snapshot[PALETTE_ENTRY_MAX];
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t entry = entries + i * 4u;
        red_snapshot[i] = wc3_legacy_rd8g(entry + 0u);
        green_snapshot[i] = wc3_legacy_rd8g(entry + 1u);
        blue_snapshot[i] = wc3_legacy_rd8g(entry + 2u);
        flags_snapshot[i] = wc3_legacy_rd8g(entry + 3u);
    }

    pthread_mutex_lock(&g_palette_table_lock);
    GdiPalette *palette = find_palette(handle);
    if (!palette) {
        pthread_mutex_unlock(&g_palette_table_lock);
        c->eax = 0u;
        return;
    }
    const unsigned slot = (unsigned)(palette - g_palettes);
    /* Pin the slot under the table lock, then drop the table: the range
     * check and payload writes need only the slot lock.  Delete and the
     * other finders block on this slot until the update is complete. */
    pthread_mutex_lock(&g_palette_locks[slot]);
    pthread_mutex_unlock(&g_palette_table_lock);
    if (start > palette->count ||
        count > (uint32_t)palette->count - start) {
        pthread_mutex_unlock(&g_palette_locks[slot]);
        c->eax = 0u;
        return;
    }
    memcpy(palette->red + start, red_snapshot, (size_t)count);
    memcpy(palette->green + start, green_snapshot, (size_t)count);
    memcpy(palette->blue + start, blue_snapshot, (size_t)count);
    memcpy(palette->flags + start, flags_snapshot, (size_t)count);
    /* RealizePalette compares revision, so this bump is the invalidation. */
    palette->revision++;
    pthread_mutex_unlock(&g_palette_locks[slot]);
    c->eax = count;
}

static void h_select_palette(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    const uint32_t handle = wc3_legacy_arg32(c, 1u);
    if (!hdc) {
        c->eax = 0u;
        return;
    }

    pthread_mutex_lock(&g_dc_lock);

    /* Ensure the DC slot first, as the split requires.  An invalid palette
     * handle must not leave a newly allocated g_dc_states entry behind: the
     * old single-lock code returned before ensure_dc_state(), so undo the
     * allocation under the same dc lock before returning. */
    const int had_state = find_dc_state(hdc) != NULL;
    PaletteDcState *state = ensure_dc_state(hdc);
    GdiPalette *palette = NULL;
    unsigned slot = 0u;
    int have_slot = 0;
    if (handle) {
        pthread_mutex_lock(&g_palette_table_lock);
        palette = find_palette(handle);
        if (!palette) {
            if (!had_state && state) memset(state, 0, sizeof *state);
            pthread_mutex_unlock(&g_palette_table_lock);
            pthread_mutex_unlock(&g_dc_lock);
            c->eax = 0u;
            return;
        }
        slot = (unsigned)(palette - g_palettes);
        /* Pin the slot under the table, then drop the table immediately.
         * The dc lock and the pinned slot cover the state update. */
        pthread_mutex_lock(&g_palette_locks[slot]);
        pthread_mutex_unlock(&g_palette_table_lock);
        have_slot = 1;
    }
    uint32_t previous = 0u;
    if (state) {
        previous = state->palette;
        state->palette = handle;
        state->realized_revision = 0u;
        state->realized_entries = 0u;
    }
    if (have_slot) pthread_mutex_unlock(&g_palette_locks[slot]);
    pthread_mutex_unlock(&g_dc_lock);
    c->eax = state ? previous : 0u;
}

static void h_realize_palette(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    if (!hdc) {
        c->eax = 0u;
        return;
    }

    pthread_mutex_lock(&g_dc_lock);
    PaletteDcState *state = find_dc_state(hdc);
    uint32_t count = 0u;
    if (state && state->palette) {
        pthread_mutex_lock(&g_palette_table_lock);
        GdiPalette *palette = find_palette(state->palette);
        unsigned slot = 0u;
        if (palette) {
            slot = (unsigned)(palette - g_palettes);
            /* Pin the slot under the table, then drop the table.  The
             * payload read below is covered by the slot lock. */
            pthread_mutex_lock(&g_palette_locks[slot]);
        }
        pthread_mutex_unlock(&g_palette_table_lock);
        if (palette) {
            if (state->realized_revision != palette->revision ||
                state->realized_entries != palette->count) {
                state->realized_revision = palette->revision;
                state->realized_entries = palette->count;
                count = palette->count;
            }
            pthread_mutex_unlock(&g_palette_locks[slot]);
        }
    }
    pthread_mutex_unlock(&g_dc_lock);
    c->eax = count;
}

static void h_get_system_palette_entries(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    const uint32_t start = wc3_legacy_arg32(c, 1u);
    uint32_t count = wc3_legacy_arg32(c, 2u);
    const uint32_t entries = wc3_legacy_arg32(c, 3u);
    if (!hdc || !entries || count == 0u ||
        start >= SYSTEM_PALETTE_COUNT) {
        c->eax = 0u;
        return;
    }
    if (count > SYSTEM_PALETTE_COUNT - start)
        count = SYSTEM_PALETTE_COUNT - start;
    if (!wc3_legacy_guest_ok(entries, count * 4u)) {
        c->eax = 0u;
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        const SystemPaletteEntry *source = &g_system_palette[start + i];
        const uint32_t destination = entries + i * 4u;
        wc3_legacy_wr8g(destination + 0u, source->red);
        wc3_legacy_wr8g(destination + 1u, source->green);
        wc3_legacy_wr8g(destination + 2u, source->blue);
        wc3_legacy_wr8g(destination + 3u, 0u);
    }
    c->eax = count;
}

static void h_delete_object(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    if (!is_palette_handle(handle)) {
        if (g_previous_delete_object)
            g_previous_delete_object(c);
        else
            c->eax = 0u;
        return;
    }

    pthread_mutex_lock(&g_dc_lock);
    int selected = 0;
    for (unsigned i = 0; i < DC_PALETTE_MAX; i++) {
        if (g_dc_states[i].used && g_dc_states[i].palette == handle) {
            selected = 1;
            break;
        }
    }
    pthread_mutex_lock(&g_palette_table_lock);
    GdiPalette *palette = find_palette(handle);
    int removed = 0;
    if (palette && !selected) {
        const unsigned slot = (unsigned)(palette - g_palettes);
        pthread_mutex_lock(&g_palette_locks[slot]);
        memset(palette, 0, sizeof *palette);
        pthread_mutex_unlock(&g_palette_locks[slot]);
        removed = 1;
    }
    pthread_mutex_unlock(&g_palette_table_lock);
    pthread_mutex_unlock(&g_dc_lock);
    c->eax = removed ? 1u : 0u;
}

typedef struct GdiPaletteAddition {
    const char *name;
    int argc;
    void (*impl)(CPU *c);
} GdiPaletteAddition;

static const GdiPaletteAddition g_additions[] = {
    { "CreatePalette", 1, h_create_palette },
    { "SetPaletteEntries", 4, h_set_palette_entries },
    { "SelectPalette", 3, h_select_palette },
    { "RealizePalette", 1, h_realize_palette },
    { "GetSystemPaletteEntries", 4, h_get_system_palette_entries },
};

int wc3_hle_gdi32_palette_install(void)
{
    if (pthread_once(&g_palette_locks_once, init_palette_locks) != 0 ||
        !g_palette_locks_ready)
        return -1;

    pthread_mutex_lock(&g_dc_lock);
    pthread_mutex_lock(&g_palette_table_lock);
    memset(g_palettes, 0, sizeof g_palettes);
    memset(g_dc_states, 0, sizeof g_dc_states);
    pthread_mutex_unlock(&g_palette_table_lock);
    pthread_mutex_unlock(&g_dc_lock);

    if (wc3_legacy_hle_chain("GDI32.dll", "DeleteObject",
                             h_delete_object,
                             &g_previous_delete_object) != 0)
        return -1;

    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("GDI32.dll", g_additions[i].name,
                               g_additions[i].argc, 0,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(1u + sizeof g_additions / sizeof g_additions[0]);
}
