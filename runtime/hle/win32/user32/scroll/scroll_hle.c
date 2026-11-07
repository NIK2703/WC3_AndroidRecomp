/*
 * scroll_hle.c - target-side USER32 scroll-bar state.
 *
 * The three imported entry points are pure state queries and updates.  Each
 * (window, bar) pair owns a range, page and position; SetScrollInfo returns
 * the previous position and GetScrollInfo fills only the fields selected by
 * fMask, as Win32 does.
 */
#include "scroll_hle.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"

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
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int valid_bar(uint32_t bar)
{
    return bar <= SB_CTL;
}

static ScrollState *find_state(uint32_t hwnd, uint32_t bar)
{
    for (unsigned i = 0; i < SCROLL_SLOTS; i++) {
        if (g_scroll[i].used && g_scroll[i].hwnd == hwnd &&
            g_scroll[i].bar == bar)
            return &g_scroll[i];
    }
    return NULL;
}

static ScrollState *ensure_state(uint32_t hwnd, uint32_t bar)
{
    ScrollState *state = find_state(hwnd, bar);
    if (state) return state;
    for (unsigned i = 0; i < SCROLL_SLOTS; i++) {
        if (g_scroll[i].used) continue;
        memset(&g_scroll[i], 0, sizeof g_scroll[i]);
        g_scroll[i].used = 1u;
        g_scroll[i].hwnd = hwnd;
        g_scroll[i].bar = bar;
        return &g_scroll[i];
    }
    return NULL;
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

    pthread_mutex_lock(&g_lock);
    ScrollState *state = ensure_state(hwnd, bar);
    if (!state) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    const uint32_t mask = wc3_legacy_rd32g(info + SCROLLINFO_MASK);
    const int32_t old_position = state->position;
    if (mask & SIF_RANGE) {
        state->minimum = (int32_t)wc3_legacy_rd32g(info + SCROLLINFO_MIN);
        state->maximum = (int32_t)wc3_legacy_rd32g(info + SCROLLINFO_MAX);
    }
    if (mask & SIF_PAGE)
        state->page = wc3_legacy_rd32g(info + SCROLLINFO_PAGE);
    if (mask & SIF_POS)
        state->position = (int32_t)wc3_legacy_rd32g(info + SCROLLINFO_POS);
    state->position = clamp_position(state, state->position);
    c->eax = (uint32_t)old_position;
    pthread_mutex_unlock(&g_lock);
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

    pthread_mutex_lock(&g_lock);
    ScrollState *state = ensure_state(hwnd, bar);
    if (!state) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    const uint32_t mask = wc3_legacy_rd32g(info + SCROLLINFO_MASK);
    wc3_legacy_wr32g(info + SCROLLINFO_CB_SIZE, SCROLLINFO_SIZE);
    if (mask & SIF_RANGE) {
        wc3_legacy_wr32g(info + SCROLLINFO_MIN, (uint32_t)state->minimum);
        wc3_legacy_wr32g(info + SCROLLINFO_MAX, (uint32_t)state->maximum);
    }
    if (mask & SIF_PAGE)
        wc3_legacy_wr32g(info + SCROLLINFO_PAGE, state->page);
    if (mask & SIF_POS)
        wc3_legacy_wr32g(info + SCROLLINFO_POS, (uint32_t)state->position);
    if (mask & SIF_TRACKPOS)
        wc3_legacy_wr32g(info + SCROLLINFO_TRACK,
                         (uint32_t)state->position);
    c->eax = 1u;
    pthread_mutex_unlock(&g_lock);
}

static void h_set_scroll_pos(CPU *c)
{
    const uint32_t hwnd = wc3_legacy_arg32(c, 0u);
    const uint32_t bar = wc3_legacy_arg32(c, 1u);
    const int32_t position = (int32_t)wc3_legacy_arg32(c, 2u);
    (void)wc3_legacy_arg32(c, 3u);

    c->eax = 0u;
    if (!hwnd || !valid_bar(bar)) return;

    pthread_mutex_lock(&g_lock);
    ScrollState *state = ensure_state(hwnd, bar);
    if (state) {
        const int32_t old_position = state->position;
        state->position = clamp_position(state, position);
        c->eax = (uint32_t)old_position;
    }
    pthread_mutex_unlock(&g_lock);
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
    pthread_mutex_lock(&g_lock);
    memset(g_scroll, 0, sizeof g_scroll);
    pthread_mutex_unlock(&g_lock);

    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("USER32.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 0,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_additions / sizeof g_additions[0]);
}
