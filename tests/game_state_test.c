#include "game_state.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static int g_wake_count;
static uint32_t g_wake_bits;
static void *g_wake_user;

static void on_wake(void *user)
{
    g_wake_user = user;
    g_wake_bits = wc3_game_state_get();
    g_wake_count++;
}

int main(void)
{
    int token = 7;

    wc3_game_state_init();
    assert(wc3_game_state_get() == 0u);
    wc3_game_state_set_wake(on_wake, &token);

    wc3_game_state_hook(WC3_GAME_STATE_HOOK_CGAMEUI_CREATED, 0x1234u);
    assert(wc3_game_state_get() == WC3_GAME_STATE_CGAMEUI);
    assert(g_wake_count == 1);
    assert(g_wake_user == &token);
    assert((g_wake_bits & WC3_GAME_STATE_CGAMEUI) != 0u);

    wc3_game_state_hook(WC3_GAME_STATE_HOOK_PAUSE, 1u);
    assert((wc3_game_state_get() & WC3_GAME_STATE_PAUSE) != 0u);

    wc3_game_state_hook(WC3_GAME_STATE_HOOK_GAMEOVER, 1u);
    assert((wc3_game_state_get() & WC3_GAME_STATE_GAMEOVER) != 0u);

    wc3_game_state_hook(WC3_GAME_STATE_HOOK_PAUSE, 0u);
    assert((wc3_game_state_get() & WC3_GAME_STATE_PAUSE) == 0u);
    assert((wc3_game_state_get() & WC3_GAME_STATE_GAMEOVER) != 0u);

    wc3_game_state_hook(WC3_GAME_STATE_HOOK_GAMEOVER_CLEARED, 0u);
    assert((wc3_game_state_get() & WC3_GAME_STATE_GAMEOVER) == 0u);

    /* Both ShowInterface hidden branches set CINEMATIC; both restore branches
     * clear it. */
    wc3_game_state_hook(WC3_GAME_STATE_HOOK_CINEMATIC_ENTER_TIMED, 0u);
    assert((wc3_game_state_get() & WC3_GAME_STATE_CINEMATIC) != 0u);
    wc3_game_state_hook(WC3_GAME_STATE_HOOK_CINEMATIC_LEAVE_TIMED, 0u);
    assert((wc3_game_state_get() & WC3_GAME_STATE_CINEMATIC) == 0u);

    wc3_game_state_hook(WC3_GAME_STATE_HOOK_CINEMATIC_ENTER_INSTANT, 0u);
    assert((wc3_game_state_get() & WC3_GAME_STATE_CINEMATIC) != 0u);
    wc3_game_state_hook(WC3_GAME_STATE_HOOK_CINEMATIC_LEAVE_INSTANT, 0u);
    assert((wc3_game_state_get() & WC3_GAME_STATE_CINEMATIC) == 0u);

    /* The timed transition completion publishes the interface mode it settles
     * on.  A completed timed show (mode 4) leaves the interface visible, so
     * the authored controls must come back; a completed timed hide (mode 3)
     * leaves the interface hidden, which is the cinematic mode, so the
     * cinematic bit must survive it. */
    wc3_game_state_hook(WC3_GAME_STATE_HOOK_CINEMATIC_ENTER_TIMED, 0u);
    wc3_game_state_hook(WC3_GAME_STATE_HOOK_INTERFACE_TIMED_COMPLETE,
                        WC3_INTERFACE_MODE_SHOWN);
    assert((wc3_game_state_get() & WC3_GAME_STATE_CINEMATIC) == 0u);

    wc3_game_state_hook(WC3_GAME_STATE_HOOK_CINEMATIC_ENTER_TIMED, 0u);
    wc3_game_state_hook(WC3_GAME_STATE_HOOK_INTERFACE_TIMED_COMPLETE,
                        WC3_INTERFACE_MODE_HIDDEN);
    assert((wc3_game_state_get() & WC3_GAME_STATE_CINEMATIC) != 0u);

    /* The intro hides the interface through the instant branch, which arms no
     * timer, so the cinematic bit stays until the interface is restored. */
    wc3_game_state_hook(WC3_GAME_STATE_HOOK_CINEMATIC_ENTER_INSTANT, 0u);
    assert((wc3_game_state_get() & WC3_GAME_STATE_CINEMATIC) != 0u);
    wc3_game_state_hook(WC3_GAME_STATE_HOOK_CINEMATIC_LEAVE_INSTANT, 0u);
    assert((wc3_game_state_get() & WC3_GAME_STATE_CINEMATIC) == 0u);

    /* A replay/observer bit, when published by a later proven anchor, must not
     * be lost by a pause transition.  The hook path itself is tested here. */
    wc3_game_state_set(WC3_GAME_STATE_REPLAY, 0u);
    wc3_game_state_hook(WC3_GAME_STATE_HOOK_PAUSE, 1u);
    wc3_game_state_hook(WC3_GAME_STATE_HOOK_PAUSE, 0u);
    assert((wc3_game_state_get() & WC3_GAME_STATE_REPLAY) != 0u);

    int before = g_wake_count;
    wc3_game_state_set(WC3_GAME_STATE_REPLAY, 0u);
    assert(g_wake_count == before);

    wc3_game_state_hook(WC3_GAME_STATE_HOOK_CGAMEUI_SEEN, 0u);
    assert(wc3_game_state_get() == 0u);

    wc3_game_state_hook(WC3_GAME_STATE_HOOK_CINEMATIC_ENTER_INSTANT, 0u);
    assert((wc3_game_state_get() & WC3_GAME_STATE_CINEMATIC) != 0u);
    wc3_game_state_hook(WC3_GAME_STATE_HOOK_CGAMEUI_CREATED, 1u);
    assert((wc3_game_state_get() & WC3_GAME_STATE_CINEMATIC) == 0u);
    wc3_game_state_hook(WC3_GAME_STATE_HOOK_GAMEOVER, 1u);
    wc3_game_state_hook(WC3_GAME_STATE_HOOK_CGAMEUI_DESTROYED, 0u);
    assert(wc3_game_state_get() == 0u);

    wc3_game_state_hook(WC3_GAME_STATE_HOOK_CGAMEUI_CREATED, 1u);
    wc3_game_state_hook(WC3_GAME_STATE_HOOK_CGAMEUI_SEEN, 1u);
    assert(wc3_game_state_get() == WC3_GAME_STATE_CGAMEUI);

    wc3_game_state_hook(99u, 1u);
    assert(wc3_game_state_get() == WC3_GAME_STATE_CGAMEUI);

    wc3_game_state_set_wake(NULL, NULL);
    before = g_wake_count;
    wc3_game_state_set(0u, WC3_GAME_STATE_ALL);
    assert(g_wake_count == before);

    printf("game_state: okay\n");
    return 0;
}
