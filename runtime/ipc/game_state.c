#include "game_state.h"

#include <stdatomic.h>

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

    default:
        break;
    }
}
