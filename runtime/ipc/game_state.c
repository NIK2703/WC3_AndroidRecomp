#include "game_state.h"

#include <stdatomic.h>

#include "wc3_debug.h"

#if WC3_DEBUG && WC3_DEBUG_INPUT
#include <android/log.h>
#define GAME_STATE_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "wc3state", __VA_ARGS__)
#else
#define GAME_STATE_LOG(...) ((void)0)
#endif

#if WC3_DEBUG && WC3_DEBUG_INPUT
static const char *hook_name(unsigned kind)
{
    switch (kind) {
    case WC3_GAME_STATE_HOOK_CGAMEUI_CREATED:
        return "cgameui_created";
    case WC3_GAME_STATE_HOOK_CGAMEUI_SEEN:
        return "cgameui_seen";
    case WC3_GAME_STATE_HOOK_CGAMEUI_DESTROYED:
        return "cgameui_destroyed";
    case WC3_GAME_STATE_HOOK_PAUSE:
        return "pause";
    case WC3_GAME_STATE_HOOK_GAMEOVER:
        return "gameover";
    case WC3_GAME_STATE_HOOK_GAMEOVER_CLEARED:
        return "gameover_cleared";
    case WC3_GAME_STATE_HOOK_CINEMATIC_ENTER_TIMED:
        return "cinematic_enter_timed";
    case WC3_GAME_STATE_HOOK_CINEMATIC_ENTER_INSTANT:
        return "cinematic_enter_instant";
    case WC3_GAME_STATE_HOOK_CINEMATIC_LEAVE_TIMED:
        return "cinematic_leave_timed";
    case WC3_GAME_STATE_HOOK_CINEMATIC_LEAVE_INSTANT:
        return "cinematic_leave_instant";
    case WC3_GAME_STATE_HOOK_INTERFACE_TIMED_COMPLETE:
        return "interface_timed_complete";
    default:
        return "unknown";
    }
}
#endif

static _Atomic uint32_t g_state_bits;
static _Atomic(Wc3GameStateWake) g_wake;
static _Atomic(void *) g_wake_user;

static void notify_wake(void)
{
    Wc3GameStateWake wake = atomic_load_explicit(&g_wake, memory_order_acquire);
    if (!wake)
        return;
    void *user = atomic_load_explicit(&g_wake_user, memory_order_acquire);
    wake(user);
}

void wc3_game_state_init(void)
{
    atomic_store_explicit(&g_state_bits, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_wake_user, NULL, memory_order_relaxed);
    atomic_store_explicit(&g_wake, NULL, memory_order_release);
}

uint32_t wc3_game_state_get(void)
{
    return atomic_load_explicit(&g_state_bits, memory_order_acquire);
}

void wc3_game_state_set(uint32_t set_mask, uint32_t clear_mask)
{
    uint32_t old = atomic_load_explicit(&g_state_bits, memory_order_relaxed);
    uint32_t next;

    for (;;) {
        next = (old & ~clear_mask) | set_mask;
        if (next == old)
            return;
        if (atomic_compare_exchange_weak_explicit(
                &g_state_bits, &old, next, memory_order_acq_rel,
                memory_order_relaxed))
            break;
    }
    notify_wake();
}

void wc3_game_state_set_wake(Wc3GameStateWake wake, void *user)
{
    atomic_store_explicit(&g_wake_user, user, memory_order_relaxed);
    atomic_store_explicit(&g_wake, wake, memory_order_release);
}

void wc3_game_state_hook(unsigned kind, uint32_t value)
{
#if WC3_DEBUG && WC3_DEBUG_INPUT
    uint32_t before = wc3_game_state_get();
#endif
    switch (kind) {
    case WC3_GAME_STATE_HOOK_CGAMEUI_CREATED:
        if (value) {
            wc3_game_state_set(
                WC3_GAME_STATE_CGAMEUI,
                WC3_GAME_STATE_PAUSE | WC3_GAME_STATE_GAMEOVER |
                    WC3_GAME_STATE_CINEMATIC | WC3_GAME_STATE_REPLAY |
                    WC3_GAME_STATE_OBSERVER);
        } else {
            wc3_game_state_set(0u, WC3_GAME_STATE_ALL);
        }
        break;

    case WC3_GAME_STATE_HOOK_CGAMEUI_SEEN:
        /* The getter rewrites the same pointer on every call.  Only the clear
         * case is a state transition. */
        if (!value)
            wc3_game_state_set(0u, WC3_GAME_STATE_ALL);
        break;

    case WC3_GAME_STATE_HOOK_CGAMEUI_DESTROYED:
        if (value)
            wc3_game_state_set(WC3_GAME_STATE_CGAMEUI, 0u);
        else
            wc3_game_state_set(0u, WC3_GAME_STATE_ALL);
        break;

    case WC3_GAME_STATE_HOOK_PAUSE:
        if (value)
            wc3_game_state_set(WC3_GAME_STATE_PAUSE, 0u);
        else
            wc3_game_state_set(0u, WC3_GAME_STATE_PAUSE);
        break;

    case WC3_GAME_STATE_HOOK_GAMEOVER:
        if (value)
            wc3_game_state_set(WC3_GAME_STATE_GAMEOVER, 0u);
        else
            wc3_game_state_set(0u, WC3_GAME_STATE_GAMEOVER);
        break;

    case WC3_GAME_STATE_HOOK_GAMEOVER_CLEARED:
        if (value)
            wc3_game_state_set(WC3_GAME_STATE_GAMEOVER, 0u);
        else
            wc3_game_state_set(0u, WC3_GAME_STATE_GAMEOVER);
        break;

    /* ShowInterface(false, ...) is the engine's hidden-interface mode.  Both
     * branches write CGameUI+0x180 = 1 or 3 after the interface has been hidden
     * and CCinematicPanel has been shown. */
    case WC3_GAME_STATE_HOOK_CINEMATIC_ENTER_TIMED:
    case WC3_GAME_STATE_HOOK_CINEMATIC_ENTER_INSTANT:
        wc3_game_state_set(WC3_GAME_STATE_CINEMATIC, 0u);
        break;

    /* ShowInterface(true, ...) restores the interface. */
    case WC3_GAME_STATE_HOOK_CINEMATIC_LEAVE_TIMED:
    case WC3_GAME_STATE_HOOK_CINEMATIC_LEAVE_INSTANT:
        wc3_game_state_set(0u, WC3_GAME_STATE_CINEMATIC);
        break;

    /* Completion of a timed ShowInterface transition (timer id 0x1E).  The
     * handler writes the interface mode it settles on: 4 when a timed "show"
     * completed (the interface is visible again, so the cinematic is over and
     * the authored controls come back) and 3 when a timed "hide" completed.
     *
     * Mode 3 is the hidden-interface mode, and in this engine the hidden
     * interface *is* the cinematic mode: ShowInterface(false) is what hides the
     * interface, and CCinematicPanel is the active UI whenever the mode is 1 or
     * 3 (that is SetCinematicScene's own condition).  Clearing CINEMATIC here
     * used to strip the authored ESC button from every timed cinematic - the
     * map's in-game cutscenes - while the intro kept it, because the intro hides
     * the interface through the instant branch (duration 0) and arms no timer.
     * The hide completion only ends the fade, not the scene, so the cinematic
     * bit must survive it. */
    case WC3_GAME_STATE_HOOK_INTERFACE_TIMED_COMPLETE:
        if (value == WC3_INTERFACE_MODE_SHOWN) {
            wc3_game_state_set(0u, WC3_GAME_STATE_CINEMATIC);
        } else {
            wc3_game_state_set(WC3_GAME_STATE_CINEMATIC, 0u);
        }
        break;

    default:
        break;
    }

#if WC3_DEBUG && WC3_DEBUG_INPUT
    {
        uint32_t after = wc3_game_state_get();
        GAME_STATE_LOG("hook %s(kind=%u) value=%u bits %08x -> %08x",
                       hook_name(kind), kind, value, before, after);
    }
#endif
}
