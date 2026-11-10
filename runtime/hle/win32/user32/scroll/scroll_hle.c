/*
 * scroll_hle.c - target-side USER32 scroll-bar state.
 *
 * The three imported entry points are pure state queries and updates.  Each
 * (window, bar) pair owns a range, page and position; SetScrollInfo returns
 * the previous position and GetScrollInfo fills only the fields selected by
 * fMask, as Win32 does.
 *
 * The slot table has two lock levels.  g_scroll_table_lock owns slot identity
 * (used/hwnd/bar) and allocation; g_scroll_locks[i] owns slot i's range, page
 * and position.  A lookup resolves and pins a slot by taking its mutex before
 * dropping the table lock, so install() cannot clear or reuse the slot while
 * the caller still holds it.  Lock order is table -> slot; no slot lock is
 * held while taking the table lock.  Guest SCROLLINFO memory is read before
 * either lock and written after both are released.
 */
#include "scroll_hle.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "hle_bridge.h"

#define SCROLLINFO_SIZE 28u
#define SCROLLINFO_CB_SIZE 0u
#define SCROLLINFO_MASK 4u
#define SCROLLINFO_MIN 8u
#define SCROLLINFO_MAX 12u
#define SCROLLINFO_PAGE 16u
#define SCROLLINFO_POS 20u
#define SCROLLINFO_TRACK 24u

#define SIF_RANGE 0x0001u
#define SIF_PAGE 0x0002u
#define SIF_POS 0x0004u
#define SIF_DISABLENOSCROLL 0x0008u
#define SIF_TRACKPOS 0x0010u

#define SB_HORZ 0u
#define SB_VERT 1u
#define SB_CTL 2u

#define SCROLL_SLOTS 128u

typedef struct ScrollState {
    uint8_t used;
    uint32_t hwnd;
    uint32_t bar;
    int32_t minimum;
    int32_t maximum;
    uint32_t page;
    int32_t position;
} ScrollState;

static ScrollState g_scroll[SCROLL_SLOTS];
static pthread_mutex_t g_scroll_table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_scroll_locks[SCROLL_SLOTS];
static pthread_once_t g_scroll_locks_once = PTHREAD_ONCE_INIT;

static void init_scroll_locks(void)
{
    for (unsigned i = 0; i < SCROLL_SLOTS; i++)
        pthread_mutex_init(&g_scroll_locks[i], NULL);
}

static void ensure_scroll_locks(void)
{
    (void)pthread_once(&g_scroll_locks_once, init_scroll_locks);
}

static int valid_bar(uint32_t bar)
{
    return bar <= SB_CTL;
}

/* Caller holds g_scroll_table_lock.  Slot identity changes only under that
 * lock, so a resolved index can be pinned with g_scroll_locks[index] while the
 * table lock is still held. */
static unsigned find_state_index_locked(uint32_t hwnd, uint32_t bar)
{
    for (unsigned i = 0; i < SCROLL_SLOTS; i++) {
        if (g_scroll[i].used && g_scroll[i].hwnd == hwnd &&
            g_scroll[i].bar == bar)
            return i;
    }
    return SCROLL_SLOTS;
}

/* Caller holds g_scroll_table_lock.  Returns the matching slot with its slot
 * mutex held, allocating the first free slot when the pair is new, or NULL
 * when all slots are in use.  A new slot becomes visible only after its mutex
 * is held and it has been cleared, so a concurrent lookup cannot observe a
 * half-initialised slot. */
static ScrollState *lock_state_locked(uint32_t hwnd, uint32_t bar)
{
    unsigned index = find_state_index_locked(hwnd, bar);
    if (index == SCROLL_SLOTS) {
        for (unsigned i = 0; i < SCROLL_SLOTS; i++) {
            if (g_scroll[i].used) continue;
            index = i;
            break;
        }
        if (index == SCROLL_SLOTS) return NULL;
        pthread_mutex_lock(&g_scroll_locks[index]);
        memset(&g_scroll[index], 0, sizeof g_scroll[index]);
        g_scroll[index].used = 1u;
        g_scroll[index].hwnd = hwnd;
        g_scroll[index].bar = bar;
        return &g_scroll[index];
    }
    pthread_mutex_lock(&g_scroll_locks[index]);
    return &g_scroll[index];
}

/* Resolve a pair to a pinned slot.  The table lock is dropped before the
 * caller sees the pointer; the slot mutex stays held across the state update,
 * so install() cannot reset the table underneath the caller. */
static ScrollState *lock_state(uint32_t hwnd, uint32_t bar)
{
    pthread_mutex_lock(&g_scroll_table_lock);
    ScrollState *state = lock_state_locked(hwnd, bar);
    pthread_mutex_unlock(&g_scroll_table_lock);
    return state;
}

static void unlock_state(ScrollState *state)
{
    pthread_mutex_unlock(&g_scroll_locks[(unsigned)(state - g_scroll)]);
}

static int32_t max_position(const ScrollState *state)
{
    const int64_t span = (int64_t)state->maximum -
                         ((int64_t)state->page > 1 ? (int64_t)state->page - 1
                                                   : 0);
    if (span < state->minimum) return state->minimum;
    if (span > INT32_MAX) return INT32_MAX;
    return (int32_t)span;
}

static int32_t clamp_position(const ScrollState *state, int32_t position)
{
    if (position < state->minimum) return state->minimum;
    const int32_t maximum = max_position(state);
    if (position > maximum) return maximum;
    return position;
}

static void h_set_scroll_info(CPU *c)
{
    const uint32_t hwnd = wc3_legacy_arg32(c, 0u);
    const uint32_t bar = wc3_legacy_arg32(c, 1u);
    const uint32_t info = wc3_legacy_arg32(c, 2u);
    (void)wc3_legacy_arg32(c, 3u);

    c->eax = 0u;
    if (!hwnd || !valid_bar(bar) || !info ||
        !wc3_legacy_guest_ok(info, SCROLLINFO_SIZE))
        return;
    if (wc3_legacy_rd32g(info + SCROLLINFO_CB_SIZE) < SCROLLINFO_SIZE)
        return;

    const uint32_t mask = wc3_legacy_rd32g(info + SCROLLINFO_MASK);
    int32_t minimum = 0;
    int32_t maximum = 0;
    uint32_t page = 0u;
    int32_t position = 0;
    if (mask & SIF_RANGE) {
        minimum = (int32_t)wc3_legacy_rd32g(info + SCROLLINFO_MIN);
        maximum = (int32_t)wc3_legacy_rd32g(info + SCROLLINFO_MAX);
    }
    if (mask & SIF_PAGE)
        page = wc3_legacy_rd32g(info + SCROLLINFO_PAGE);
    if (mask & SIF_POS)
        position = (int32_t)wc3_legacy_rd32g(info + SCROLLINFO_POS);

    ScrollState *state = lock_state(hwnd, bar);
    if (!state) return;

    const int32_t old_position = state->position;
    if (mask & SIF_RANGE) {
        state->minimum = minimum;
        state->maximum = maximum;
    }
    if (mask & SIF_PAGE)
        state->page = page;
    if (mask & SIF_POS)
        state->position = position;
    state->position = clamp_position(state, state->position);
    c->eax = (uint32_t)old_position;
    unlock_state(state);
}

static void h_get_scroll_info(CPU *c)
{
    const uint32_t hwnd = wc3_legacy_arg32(c, 0u);
    const uint32_t bar = wc3_legacy_arg32(c, 1u);
    const uint32_t info = wc3_legacy_arg32(c, 2u);

    c->eax = 0u;
    if (!hwnd || !valid_bar(bar) || !info ||
        !wc3_legacy_guest_ok(info, SCROLLINFO_SIZE))
        return;
    if (wc3_legacy_rd32g(info + SCROLLINFO_CB_SIZE) < SCROLLINFO_SIZE)
        return;

    const uint32_t mask = wc3_legacy_rd32g(info + SCROLLINFO_MASK);
    ScrollState *state = lock_state(hwnd, bar);
    if (!state) return;

    const int32_t minimum = state->minimum;
    const int32_t maximum = state->maximum;
    const uint32_t page = state->page;
    const int32_t position = state->position;
    unlock_state(state);

    wc3_legacy_wr32g(info + SCROLLINFO_CB_SIZE, SCROLLINFO_SIZE);
    if (mask & SIF_RANGE) {
        wc3_legacy_wr32g(info + SCROLLINFO_MIN, (uint32_t)minimum);
        wc3_legacy_wr32g(info + SCROLLINFO_MAX, (uint32_t)maximum);
    }
    if (mask & SIF_PAGE)
        wc3_legacy_wr32g(info + SCROLLINFO_PAGE, page);
    if (mask & SIF_POS)
        wc3_legacy_wr32g(info + SCROLLINFO_POS, (uint32_t)position);
    if (mask & SIF_TRACKPOS)
        wc3_legacy_wr32g(info + SCROLLINFO_TRACK, (uint32_t)position);
    c->eax = 1u;
}

static void h_set_scroll_pos(CPU *c)
{
    const uint32_t hwnd = wc3_legacy_arg32(c, 0u);
    const uint32_t bar = wc3_legacy_arg32(c, 1u);
    const int32_t position = (int32_t)wc3_legacy_arg32(c, 2u);
    (void)wc3_legacy_arg32(c, 3u);

    c->eax = 0u;
    if (!hwnd || !valid_bar(bar)) return;

    ScrollState *state = lock_state(hwnd, bar);
    if (state) {
        const int32_t old_position = state->position;
        state->position = clamp_position(state, position);
        c->eax = (uint32_t)old_position;
        unlock_state(state);
    }
}

typedef struct ScrollAddition {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} ScrollAddition;

static const ScrollAddition g_additions[] = {
    { "GetScrollInfo", 3u, h_get_scroll_info },
    { "SetScrollInfo", 4u, h_set_scroll_info },
    { "SetScrollPos", 4u, h_set_scroll_pos },
};

int wc3_hle_user32_scroll_install(void)
{
    ensure_scroll_locks();

    /* Install is a bootstrap step.  Holding the table lock and every slot lock
     * gives the reset the same exclusivity the former single lock had, and
     * follows the table -> slot order used at runtime. */
    pthread_mutex_lock(&g_scroll_table_lock);
    for (unsigned i = 0; i < SCROLL_SLOTS; i++)
        pthread_mutex_lock(&g_scroll_locks[i]);
    memset(g_scroll, 0, sizeof g_scroll);
    for (unsigned i = 0; i < SCROLL_SLOTS; i++)
        pthread_mutex_unlock(&g_scroll_locks[i]);
    pthread_mutex_unlock(&g_scroll_table_lock);

    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("USER32.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 0,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_additions / sizeof g_additions[0]);
}
