#ifndef TP_ACTIVATION_MODE_H
#define TP_ACTIVATION_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "core/processor.h"

typedef enum {
    BTN_MOVE_LOCK,      // LOCK: no new buttons on move
    BTN_MOVE_ACCUM,     // TRACK: accumulate buttons as finger slides
    BTN_MOVE_TRANS,     // HOVER: transition (release prev, activate curr)
} ButtonMoveMode;

typedef enum {
    UP_RELEASE_ENGAGED,     // LOCK/HOVER: release engaged elements only
    UP_RELEASE_TRACKED,     // TRACK: release all tracked buttons
} UpReleaseMode;

// Complete parameterization of activation mode behavior
// A mode is fully defined by:
//   down: how buttons engage on finger-down
//   move: how non-buttons and buttons behave during finger-move
//   up:   how elements release on finger-up
// Toggle/AR/passthrough behavior is encoded in per-element flags (activation_mode_apply_flags)
// and read ONLY by button.c — zero activation_mode enum checks in element code.
typedef struct {
    // --- Core mode parameters (read by activation_mode.c) ---
    ButtonMoveMode     move_button;

    UpReleaseMode      up_release;
    bool               up_clear_hovered;
    bool               up_reset_tracked;
} ActivationModeParams;

extern const ActivationModeParams ACTIVATION_MODE_LOCK;
extern const ActivationModeParams ACTIVATION_MODE_TRACK;
extern const ActivationModeParams ACTIVATION_MODE_HOVER;

// Apply behavioral flags to a TouchElement based on its activation mode.
// Call once at element init (after activation_mode is set from Java).
// button.c and toggle handler.c read ONLY these flags — zero activation_mode enum checks for element behavior.
static inline void activation_mode_apply_flags(TouchElement* e) {
    if (e->activation_mode == LOCK)
        e->flags |= TOUCH_BIT_IS_LOCK;
    if (e->activation_mode != TRACK)
        e->flags |= TOUCH_BIT_TOGGLE_REPRESS_KILLS;
    if (e->activation_mode == TRACK || e->activation_mode == HOVER)
        e->flags |= TOUCH_BIT_TOGGLE_SLIDE_OVER;
    if (e->activation_mode == HOVER)
        e->flags |= TOUCH_BIT_IS_HOVER;
}

// Unified button move handler (all 3 modes)
// Manages tracking lists, transitions, toggle-slide-over per *params.
// cached_hit: pre-computed hit_test_element result (may be NULL).
// Returns the hit button index at (x,y), or -1 if none.
__attribute__((hot))
int activation_mode_move_buttons(int ptr_id, float x, float y, uint64_t now_tick,
                                 TouchActionResult* result, const ActivationModeParams* params,
                                 TouchElement* cached_hit);

// Unified element-up handler (all 3 modes)
// Releases elements according to *params.
__attribute__((hot))
bool activation_mode_up(int ptr_id, float x, float y, uint64_t now_tick,
                        TouchActionResult* result, const ActivationModeParams* params);

// Reset all activation state (clear tracked, hovered, visual flags)
void activation_reset(void);

#endif // TP_ACTIVATION_MODE_H
