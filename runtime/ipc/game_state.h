/*
 * game_state.h - exact guest state bits published by the game owner.
 *
 * The lifted game code calls wc3_game_state_hook() after the exact stores that
 * change CGameUI, CEscMenu pause, CGameOverMenu and the ShowInterface mode.
 * The Android input thread consumes the resulting atomic bits; it never reads
 * guest memory and never polls the guest to guess a mode.
 */
#ifndef WC3_GAME_STATE_H
#define WC3_GAME_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    WC3_GAME_STATE_CGAMEUI   = 1u << 0,
    WC3_GAME_STATE_PAUSE     = 1u << 1,
    WC3_GAME_STATE_GAMEOVER  = 1u << 2,
    WC3_GAME_STATE_CINEMATIC = 1u << 3,
    WC3_GAME_STATE_REPLAY    = 1u << 4,
    WC3_GAME_STATE_OBSERVER  = 1u << 5
};

/*
 * Values of CGameUI+0x180 (the ShowInterface mode) published by the timed
 * transition completion handler.  These are mode values, not state bits.
 *
 * The handler runs when the timer of a timed ShowInterface call expires:
 * it writes 4 when the pending transition was a timed "show" (the interface
 * becomes visible, so the cinematic is over) and 3 when it was a timed "hide"
 * (the interface stays hidden).  The hidden-interface mode is the cinematic
 * mode in this engine - SetCinematicScene picks CCinematicPanel exactly when
 * the mode is 1 or 3 - so the hide completion must keep CINEMATIC.  The same
 * store-site therefore has two different meanings and the published value is
 * the only thing that distinguishes them.
 */
enum {
    WC3_INTERFACE_MODE_HIDDEN = 3u,
    WC3_INTERFACE_MODE_SHOWN  = 4u
};

#define WC3_GAME_STATE_ALL \
    (WC3_GAME_STATE_CGAMEUI | WC3_GAME_STATE_PAUSE | \
     WC3_GAME_STATE_GAMEOVER | WC3_GAME_STATE_CINEMATIC | \
     WC3_GAME_STATE_REPLAY | WC3_GAME_STATE_OBSERVER)

/*
 * Numeric ids are part of the lifted-code ABI: generated C calls
 * wc3_game_state_hook(<id>, value).  Keep them in sync with
 * config/state_hooks.1.27b.json.
 */
typedef enum {
    WC3_GAME_STATE_HOOK_CGAMEUI_CREATED = 0,
    WC3_GAME_STATE_HOOK_CGAMEUI_SEEN = 1,
    WC3_GAME_STATE_HOOK_CGAMEUI_DESTROYED = 2,
    WC3_GAME_STATE_HOOK_PAUSE = 3,
    WC3_GAME_STATE_HOOK_GAMEOVER = 4,
    WC3_GAME_STATE_HOOK_GAMEOVER_CLEARED = 5,
    WC3_GAME_STATE_HOOK_CINEMATIC_ENTER_TIMED = 6,
    WC3_GAME_STATE_HOOK_CINEMATIC_ENTER_INSTANT = 7,
    WC3_GAME_STATE_HOOK_CINEMATIC_LEAVE_TIMED = 8,
    WC3_GAME_STATE_HOOK_CINEMATIC_LEAVE_INSTANT = 9,
    /* Completion of a timed ShowInterface transition.  The value is the
     * written interface mode: WC3_INTERFACE_MODE_SHOWN or
     * WC3_INTERFACE_MODE_HIDDEN. */
    WC3_GAME_STATE_HOOK_INTERFACE_TIMED_COMPLETE = 10
} Wc3GameStateHookKind;

typedef void (*Wc3GameStateWake)(void *user);

void wc3_game_state_init(void);

uint32_t wc3_game_state_get(void);

/* Atomic set/clear.  Re-entrant from any guest thread. */
void wc3_game_state_set(uint32_t set_mask, uint32_t clear_mask);

/* Register the single wake consumer.  Passing NULL unregisters it. */
void wc3_game_state_set_wake(Wc3GameStateWake wake, void *user);

/* Called by lifted C after one of the pinned stores. */
void wc3_game_state_hook(unsigned kind, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif /* WC3_GAME_STATE_H */
