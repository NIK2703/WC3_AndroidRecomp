#ifndef WC3_OVERLAY_MODE_H
#define WC3_OVERLAY_MODE_H

#include <stdint.h>

#include "game_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WC3_OVERLAY_MODE_FULL = 0,
    WC3_OVERLAY_MODE_SIMPLE = 1
} Wc3OverlayMode;

/*
 * Product policy.  Replay/observer are gameplay even when a cinematic scene is
 * active, so they outrank CINEMATIC.  The bits are published only by exact
 * owner transitions; this function never inspects guest memory.
 */
static inline Wc3OverlayMode wc3_overlay_mode_for_state(uint32_t bits)
{
    if (!(bits & WC3_GAME_STATE_CGAMEUI))
        return WC3_OVERLAY_MODE_SIMPLE;
    if (bits & (WC3_GAME_STATE_REPLAY | WC3_GAME_STATE_OBSERVER))
        return WC3_OVERLAY_MODE_FULL;
    if (bits & (WC3_GAME_STATE_PAUSE | WC3_GAME_STATE_GAMEOVER |
                WC3_GAME_STATE_CINEMATIC))
        return WC3_OVERLAY_MODE_SIMPLE;
    return WC3_OVERLAY_MODE_FULL;
}

#ifdef __cplusplus
}
#endif

#endif /* WC3_OVERLAY_MODE_H */
