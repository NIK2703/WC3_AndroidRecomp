#ifndef WC3_OVERLAY_MODE_H
#define WC3_OVERLAY_MODE_H

#include <stdbool.h>
#include <stdint.h>

#include "game_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WC3_OVERLAY_MODE_FULL = 0,
    /* Menus and other non-gameplay states: no on-screen controls, single tap
     * is LMB.  The user closes the menu through the game's own interface. */
    WC3_OVERLAY_MODE_SIMPLE = 1,
    /* Cinematic: the same simplified gestures, but the authored ESC element is
     * kept so a touch can skip the scene. */
    WC3_OVERLAY_MODE_SIMPLE_ESC = 2
} Wc3OverlayMode;

static inline bool wc3_overlay_mode_is_simple(Wc3OverlayMode mode)
{
    return mode == WC3_OVERLAY_MODE_SIMPLE ||
           mode == WC3_OVERLAY_MODE_SIMPLE_ESC;
}

/*
 * Product policy.  Replay/observer are gameplay even when a cinematic scene is
 * active, so they outrank CINEMATIC.  Pause and game-over menus keep the
 * buttonless SIMPLE variant even if a cinematic bit is still latched.  The
 * bits are published only by exact owner transitions; this function never
 * inspects guest memory.
 */
static inline Wc3OverlayMode wc3_overlay_mode_for_state(uint32_t bits)
{
    if (!(bits & WC3_GAME_STATE_CGAMEUI))
        return WC3_OVERLAY_MODE_SIMPLE;
    if (bits & (WC3_GAME_STATE_REPLAY | WC3_GAME_STATE_OBSERVER))
        return WC3_OVERLAY_MODE_FULL;
    if (bits & (WC3_GAME_STATE_PAUSE | WC3_GAME_STATE_GAMEOVER))
        return WC3_OVERLAY_MODE_SIMPLE;
    /* The hidden-interface mode is the cinematic mode in this engine, so the
     * authored ESC element stays for the whole scene - including a timed
     * cinematic, whose fade timer must not clear the bit. */
    if (bits & WC3_GAME_STATE_CINEMATIC)
        return WC3_OVERLAY_MODE_SIMPLE_ESC;
    return WC3_OVERLAY_MODE_FULL;
}

#ifdef __cplusplus
}
#endif

#endif /* WC3_OVERLAY_MODE_H */
