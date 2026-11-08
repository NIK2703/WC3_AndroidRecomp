/*
 * gdi_device_hle.c - target-side GDI display-device state.
 *
 * The game reads and restores a display gamma ramp.  The as-built table did
 * not own that state, so both calls were unresolved.  A ramp is three
 * 256-entry WORD tables (red, green, blue); keeping it per HDC preserves the
 * read/modify/write sequence without needing a real display device.
 *
 * The slot table has two lock levels.  g_gamma_table_lock owns slot identity
 * (used/hdc) and allocation; g_gamma_locks[i] owns slot i's ramp.  A lookup
 * pins the slot under the table lock and drops the table lock, so install()
 * cannot clear the table while the caller still holds the slot.  Lock order is
 * table -> slot.  Guest ramp memory is copied through a stack snapshot before
 * any lock is taken and written back only after all locks are released.
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
static pthread_mutex_t g_gamma_table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_gamma_locks[GAMMA_HDC_MAX];
static pthread_once_t g_gamma_locks_once = PTHREAD_ONCE_INIT;

static void init_gamma_locks(void)
{
    for (unsigned i = 0; i < GAMMA_HDC_MAX; i++)
        pthread_mutex_init(&g_gamma_locks[i], NULL);
}

static void ensure_gamma_locks(void)
{
    (void)pthread_once(&g_gamma_locks_once, init_gamma_locks);
}

/* Caller holds g_gamma_table_lock.  Slot identity changes only under that
 * lock, so a resolved index can be pinned with g_gamma_locks[index] while the
 * table lock is still held. */
static unsigned find_gamma_index_locked(uint32_t hdc)
{
    for (unsigned i = 0; i < GAMMA_HDC_MAX; i++) {
        if (g_gamma[i].used && g_gamma[i].hdc == hdc)
            return i;
    }
    return GAMMA_HDC_MAX;
}

/* Caller holds g_gamma_table_lock.  Returns the matching slot with its slot
 * mutex held, allocating the first free slot when the HDC is new, or NULL when
 * all slots are in use.  *created is set for a new slot; its ramp is cleared by
 * the caller after the table lock is dropped. */
static GammaState *lock_gamma_locked(uint32_t hdc, int *created)
{
    *created = 0;
    unsigned index = find_gamma_index_locked(hdc);
    if (index == GAMMA_HDC_MAX) {
        for (unsigned i = 0; i < GAMMA_HDC_MAX; i++) {
            if (g_gamma[i].used) continue;
            index = i;
            break;
        }
        if (index == GAMMA_HDC_MAX) return NULL;
        /* Publish only the table-owned identity before dropping the table
         * lock; the slot lock is taken while the table lock is still held, so
         * a finder that sees this identity blocks until the ramp clear below
         * completes. */
        g_gamma[index].used = 1u;
        g_gamma[index].hdc = hdc;
        pthread_mutex_lock(&g_gamma_locks[index]);
        *created = 1;
        return &g_gamma[index];
    }
    pthread_mutex_lock(&g_gamma_locks[index]);
    return &g_gamma[index];
}

/* Caller holds g_gamma_table_lock.  Pins an existing slot without allocating,
 * or returns NULL. */
static GammaState *find_gamma_locked(uint32_t hdc)
{
    const unsigned index = find_gamma_index_locked(hdc);
    if (index == GAMMA_HDC_MAX) return NULL;
    pthread_mutex_lock(&g_gamma_locks[index]);
    return &g_gamma[index];
}

/* Resolve an HDC to a pinned slot, allocating on Set when it is new.  The
 * table lock is dropped after the slot is pinned; a new slot's ramp is cleared
 * under that slot lock before returning.  The returned pointer stays valid
 * only while its slot lock is held. */
static GammaState *lock_gamma(uint32_t hdc)
{
    pthread_mutex_lock(&g_gamma_table_lock);
    int created = 0;
    GammaState *state = lock_gamma_locked(hdc, &created);
    pthread_mutex_unlock(&g_gamma_table_lock);
    if (state && created)
        memset(state->ramp, 0, sizeof state->ramp);
    return state;
}

static GammaState *find_gamma(uint32_t hdc)
{
    pthread_mutex_lock(&g_gamma_table_lock);
    GammaState *state = find_gamma_locked(hdc);
    pthread_mutex_unlock(&g_gamma_table_lock);
    return state;
}

static void unlock_gamma(GammaState *state)
{
    pthread_mutex_unlock(&g_gamma_locks[(unsigned)(state - g_gamma)]);
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

    uint16_t snapshot[GAMMA_CHANNELS][GAMMA_ENTRIES];
    for (uint32_t channel = 0; channel < GAMMA_CHANNELS; channel++) {
        for (uint32_t i = 0; i < GAMMA_ENTRIES; i++) {
            snapshot[channel][i] =
                wc3_legacy_rd16g(ramp + (channel * GAMMA_ENTRIES + i) * 2u);
        }
    }

    GammaState *state = lock_gamma(hdc);
    int stored = 0;
    if (state) {
        memcpy(state->ramp, snapshot, sizeof snapshot);
        stored = 1;
        unlock_gamma(state);
    }
    c->eax = stored ? 1u : 0u;
}

static void h_get_device_gamma_ramp(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    const uint32_t ramp = wc3_legacy_arg32(c, 1u);
    if (!hdc || !wc3_legacy_guest_ok(ramp, GAMMA_BYTES)) {
        c->eax = 0u;
        return;
    }

    uint16_t snapshot[GAMMA_CHANNELS][GAMMA_ENTRIES];
    GammaState *state = find_gamma(hdc);
    if (state) {
        memcpy(snapshot, state->ramp, sizeof snapshot);
        unlock_gamma(state);
    } else {
        for (uint32_t channel = 0; channel < GAMMA_CHANNELS; channel++) {
            for (uint32_t i = 0; i < GAMMA_ENTRIES; i++)
                snapshot[channel][i] = linear_ramp_value(i);
        }
    }

    for (uint32_t channel = 0; channel < GAMMA_CHANNELS; channel++) {
        for (uint32_t i = 0; i < GAMMA_ENTRIES; i++) {
            wc3_legacy_wr16g(ramp + (channel * GAMMA_ENTRIES + i) * 2u,
                             snapshot[channel][i]);
        }
    }
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
    ensure_gamma_locks();

    /* Bootstrap reset follows the runtime table -> slot order and gives the
     * clear the same exclusivity the former single lock had. */
    pthread_mutex_lock(&g_gamma_table_lock);
    for (unsigned i = 0; i < GAMMA_HDC_MAX; i++)
        pthread_mutex_lock(&g_gamma_locks[i]);
    memset(g_gamma, 0, sizeof g_gamma);
    for (unsigned i = 0; i < GAMMA_HDC_MAX; i++)
        pthread_mutex_unlock(&g_gamma_locks[i]);
    pthread_mutex_unlock(&g_gamma_table_lock);

    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("GDI32.dll", g_additions[i].name,
                               g_additions[i].argc, 0,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_additions / sizeof g_additions[0]);
}
