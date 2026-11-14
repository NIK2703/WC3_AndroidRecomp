#include "overlay_mode.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    /* No CGameUI means glue/menu: SIMPLE. */
    assert(wc3_overlay_mode_for_state(0u) == WC3_OVERLAY_MODE_SIMPLE);
    assert(wc3_overlay_mode_for_state(WC3_GAME_STATE_PAUSE) ==
           WC3_OVERLAY_MODE_SIMPLE);

    /* Gameplay CGameUI: FULL. */
    assert(wc3_overlay_mode_for_state(WC3_GAME_STATE_CGAMEUI) ==
           WC3_OVERLAY_MODE_FULL);

    /* Menus and non-gameplay states: SIMPLE. */
    assert(wc3_overlay_mode_for_state(
               WC3_GAME_STATE_CGAMEUI | WC3_GAME_STATE_PAUSE) ==
           WC3_OVERLAY_MODE_SIMPLE);
    assert(wc3_overlay_mode_for_state(
               WC3_GAME_STATE_CGAMEUI | WC3_GAME_STATE_GAMEOVER) ==
           WC3_OVERLAY_MODE_SIMPLE);
    assert(wc3_overlay_mode_for_state(
               WC3_GAME_STATE_CGAMEUI | WC3_GAME_STATE_CINEMATIC) ==
           WC3_OVERLAY_MODE_SIMPLE);

    /* Replay/observer remain FULL even inside a cinematic. */
    assert(wc3_overlay_mode_for_state(
               WC3_GAME_STATE_CGAMEUI | WC3_GAME_STATE_REPLAY) ==
           WC3_OVERLAY_MODE_FULL);
    assert(wc3_overlay_mode_for_state(
               WC3_GAME_STATE_CGAMEUI | WC3_GAME_STATE_OBSERVER |
                   WC3_GAME_STATE_CINEMATIC) ==
           WC3_OVERLAY_MODE_FULL);

    /* Game-over cannot hide a replay/observer world. */
    assert(wc3_overlay_mode_for_state(
               WC3_GAME_STATE_CGAMEUI | WC3_GAME_STATE_REPLAY |
                   WC3_GAME_STATE_GAMEOVER | WC3_GAME_STATE_PAUSE |
                   WC3_GAME_STATE_CINEMATIC) ==
           WC3_OVERLAY_MODE_FULL);

    printf("overlay_mode: okay\n");
    return 0;
}
