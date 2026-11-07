#include "core/internal.h"
#include <stdint.h>


static inline bool toggle_debouncing(const TouchElement* e, uint64_t now_tick) {
    return e->toggle_debounce_last_tick != 0
        && now_tick - e->toggle_debounce_last_tick < (uint64_t)g_state.cfg.toggle_debounce_ticks;
}

#define GAMEPAD_L3 (GAMEPAD_BASE + 8)
#define GAMEPAD_R3 (GAMEPAD_BASE + 9)
#define MOVE_DEADZONE_EPSILON 0.001f


static inline void clear_gesture_defer_state(TouchElement* e, TouchActionResult* restrict result) {
    int _ei = element_index(e);
    e->flags &= ~TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED;
    vis_clear(e, VF_LONG_TAP);
    if (!(e->flags & TOUCH_BIT_LP_TOGGLED))
        release_bindings_list(result, ELEM_LP(_ei), ELEM_LP_COUNT(_ei));
    update_visual_layers(e);
}

static inline void clear_gesture_and_release_lp(TouchElement* e, TouchActionResult* restrict result) {
    if (e->flags & TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED) {
        clear_gesture_defer_state(e, result);
    } else if (e->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED) {
        int _ei = element_index(e);
        e->flags &= ~TOUCH_BIT_GESTURE_SWIPE_TRIGGERED;
        vis_clear(e, VF_GESTURE);
        if (!(e->flags & TOUCH_BIT_GESTURE_TOGGLED))
            release_bindings_list(result, ELEM_GEST(_ei), ELEM_GEST_COUNT(_ei));
        update_visual_layers(e);
    }
}

// --- Shared helpers (replaces duplicated toggle release/flip patterns) ---

// Release toggle binding, clear selected, update visual. Used by re-press and UP.
static inline void button_toggle_release(TouchElement* e, bool has_primary,
                                          TouchActionResult* restrict result) {
    if (has_primary) release_binding(result, &e->bindings[0]);
    e->flags &= ~TOUCH_BIT_SELECTED;
    finish_visual_update(e);
}

// Flip toggle (selected = !selected), press or release binding accordingly.
// Guards against releasing while gesture toggle is active.
static inline void button_toggle_flip(TouchElement* e, bool has_primary,
                                       uint64_t now_tick, TouchActionResult* restrict result) {
    if (has_primary) e->flags ^= TOUCH_BIT_SELECTED;
    if (e->flags & TOUCH_BIT_SELECTED) {
        if (has_primary) press_binding(result, &e->bindings[0], true);
        suppress_element_gestures(e, result);
        update_visual_layers(e);
    } else {
        if (!element_has_gesture_toggle(e))
            button_toggle_release(e, has_primary, result);
        else
            update_visual_layers(e);
    }
}

// Handle toggle re-press: button is already selected and pressed again.
// Covers AR debounce, gesture-toggle arm, long-press defer, TRACK keep-on,
// and LOCK/HOVER debounce-then-release.
static inline void button_down_handle_toggle_repress(TouchElement* e, bool has_primary,
                                               uint64_t now_tick, TouchActionResult* restrict result) {
    if (e->flags & TOUCH_BIT_CACHED_HAS_AUTO_REPEAT) {
        if (toggle_debouncing(e, now_tick))
            return;
        if (element_has_gesture_toggle(e)) {
            update_visual_layers(e);
            if (e->flags & TOUCH_BIT_LP_TOGGLED) e->flags |= TOUCH_BIT_LONG_PRESS_ARM;
            return;
        }
        release_toggled_alternate_bindings(e, result, true);
    } else if (e->flags & TOUCH_BIT_CACHED_HAS_LONG_PRESS) {
        e->flags |= TOUCH_BIT_LONG_PRESS_ARM;
        e->flags |= TOUCH_BIT_DEFER_PRIMARY;
        finish_visual_update(e);
        return;
    } else if (!(e->flags & TOUCH_BIT_TOGGLE_REPRESS_KILLS)) {
        e->flags |= TOUCH_BIT_AUTO_REPEAT_PRIMARY_PRESSED;
        return;
    }
    if (toggle_debouncing(e, now_tick))
        return;
    button_toggle_release(e, has_primary, result);
    e->flags &= ~TOUCH_BIT_AUTO_REPEAT_PRIMARY_PRESSED;
    e->auto_repeat_last_tick = 0;
}

// Handle first-press paths: auto-repeat only, toggle+AR, deferred, and
// default immediate press.
static void button_down_handle_first_press(TouchElement* e, bool has_primary,
                                            uint64_t now_tick, TouchActionResult* restrict result) {
    if (element_has_autorepeat_only(e)) {
        if (has_primary) {
            press_binding(result, &e->bindings[0], true);
            e->flags |= TOUCH_BIT_AUTO_REPEAT_PRIMARY_PRESSED;
        }
        e->auto_repeat_last_tick = now_tick;
        update_visual_layers(e);
        return;
    }

    if ((e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE) && (e->flags & TOUCH_BIT_CACHED_HAS_AUTO_REPEAT)) {
        {
            TouchBinding* tb0 = &e->bindings[0]; if (tb0->type != NONE) press_binding(result, tb0, true);
            TouchBinding* tb1 = &e->bindings[1]; if (tb1->type != NONE) press_binding(result, tb1, true);
            TouchBinding* tb2 = &e->bindings[2]; if (tb2->type != NONE) press_binding(result, tb2, true);
            TouchBinding* tb3 = &e->bindings[3]; if (tb3->type != NONE) press_binding(result, tb3, true);
        }
        e->toggle_debounce_last_tick = now_tick;
        e->flags |= TOUCH_BIT_SELECTED;
        e->flags |= TOUCH_BIT_AUTO_REPEAT_PRIMARY_PRESSED;
        e->auto_repeat_last_tick = now_tick;
        update_visual_layers(e);
        return;
    }

    if (e->flags & TOUCH_BIT_CACHED_HAS_LONG_PRESS)
        e->flags |= TOUCH_BIT_LONG_PRESS_ARM;
    else
        e->flags &= ~TOUCH_BIT_LONG_PRESS_ARM;

    if ((e->flags & TOUCH_BIT_CACHED_HAS_LONG_PRESS) || (e->flags & TOUCH_BIT_CACHED_HAS_GESTURE))
        e->flags |= TOUCH_BIT_DEFER_PRIMARY;
    else
        e->flags &= ~TOUCH_BIT_DEFER_PRIMARY;

    if (e->flags & TOUCH_BIT_DEFER_PRIMARY) {
        update_visual_layers(e);
        return;
    }

    if (has_primary) {
        if (e->bindings[0].type != NONE && (e->primary_sticky_mask & (1u << 0))) release_binding(result, &e->bindings[0]);
        if (e->bindings[1].type != NONE && (e->primary_sticky_mask & (1u << 1))) release_binding(result, &e->bindings[1]);
        if (e->bindings[2].type != NONE && (e->primary_sticky_mask & (1u << 2))) release_binding(result, &e->bindings[2]);
        if (e->bindings[3].type != NONE && (e->primary_sticky_mask & (1u << 3))) release_binding(result, &e->bindings[3]);

        press_binding(result, &e->bindings[0], true);
        if (e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE) {
            e->flags |= TOUCH_BIT_AUTO_REPEAT_PRIMARY_PRESSED;
            if (e->flags & TOUCH_BIT_TOGGLE_REPRESS_KILLS) {
                e->flags |= TOUCH_BIT_SELECTED;
                e->toggle_debounce_last_tick = now_tick;
            }
        }
        update_visual_layers(e);
    } else if (!(e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE)) {
        update_visual_layers(e);
    }
}

// --- DOWN: unified for toggle and non-toggle ---
// All behavioral differences encoded in e->is_lock, e->toggle_repress_kills,
// e->toggle_slide_over, e->is_hover — zero activation_mode checks.
void element_button_down(TouchElement* e, int ptr_id, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    bool has_primary = element_has_primary(e);

    if ((e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE) && (e->flags & TOUCH_BIT_SELECTED)) {
        button_down_handle_toggle_repress(e, has_primary, now_tick, result);
        return;
    }

    button_down_handle_first_press(e, has_primary, now_tick, result);
}

static inline bool handle_gesture_swipe(TouchElement* e, float dx, float dy, bool has_primary, TouchActionResult* restrict result) {
    TouchFinger* gf = find_finger(e->current_ptr_id);
    int _ei = element_index(e);
    if (!(e->flags & TOUCH_BIT_GESTURE_SUPPRESSED) && !(e->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED) && !(e->flags & TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED)
        && !finger_gesture_active(e))
    {
        bool has_gesture = ELEM_GEST_COUNT(_ei) > 0 && ELEM_GEST(_ei)[0].type != NONE;
        if (has_gesture) {
            TouchProcessorState* s = &g_state;
            float gesture_threshold = s->cfg.gesture_threshold_px > 0 ?
                (float)s->cfg.gesture_threshold_px : (float)g_state.cfg.gesture_threshold_px_fallback;
            float dx_sq = dx * dx, dy_sq = dy * dy;
            float dist_sq = dx_sq + dy_sq;
            if (dist_sq > gesture_threshold * gesture_threshold) {
                e->flags |= TOUCH_BIT_GESTURE_SWIPE_TRIGGERED;
                vis_set(e, VF_GESTURE);
                if (gf) gf->gesture_activated_in_touch = true;
                e->flags &= ~(TOUCH_BIT_LONG_PRESS_ARM | TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED);
                if (e->button_gesture_haptic > 0)
                    add_action(result, ACT_HAPTIC, e->button_gesture_haptic, 0, 0);
                toggle_alternate_bindings(e, TOUCH_BIT_GESTURE_TOGGLED, (e->flags & TOUCH_BIT_CACHED_GESTURE_HAS_TOGGLE) != 0,
                    ELEM_GEST(_ei), ELEM_GEST_COUNT(_ei), has_primary, result);
                update_visual_layers(e);
                return true;
            }
        }
    }
    return false;
}

// --- MOVE: unified ---
void element_button_move(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    bool has_primary = element_has_primary(e);
    float dx = x - e->down_x;
    float dy = y - e->down_y;
    if (fabsf(dx) < MOVE_DEADZONE_EPSILON && fabsf(dy) < MOVE_DEADZONE_EPSILON) return;
    bool inside = point_in_element(x, y, e);


    if (handle_gesture_swipe(e, dx, dy, has_primary, result)) return;

    if ((e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE) && (e->flags & TOUCH_BIT_CACHED_HAS_AUTO_REPEAT)) {
        if (e->flags & TOUCH_BIT_SELECTED) vis_set(e, VF_TAP); else vis_clear(e, VF_TAP);
        update_visual_layers(e);
        return;
    }

    if ((e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE) && !(e->flags & TOUCH_BIT_CACHED_HAS_AUTO_REPEAT) && (e->flags & TOUCH_BIT_TOGGLE_SLIDE_OVER)) {
        if (e->flags & TOUCH_BIT_AUTO_REPEAT_PRIMARY_PRESSED) {
            update_visual_layers(e);
            return;
        }
        if (inside) {
            if (!(e->flags & TOUCH_BIT_SELECTED)) {
                if (!(e->flags & TOUCH_BIT_DEFER_PRIMARY)) {
                    if (has_primary) press_binding(result, &e->bindings[0], true);
                    e->flags |= TOUCH_BIT_SELECTED;
                    update_visual_layers(e);
                    e->flags |= TOUCH_BIT_GESTURE_TIMER_ARMED;
                    mark_element_dirty(e);
                } else {
                    update_visual_layers(e);
                }
                return;
            }
            if (element_has_gesture_toggle(e)) update_visual_layers(e);
            return;
        }
        if (e->flags & TOUCH_BIT_SELECTED) {
            if (!element_has_gesture_toggle(e))
                button_toggle_release(e, has_primary, result);
            return;
        }
        update_visual_layers(e);
        return;
    }

    if ((e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE) && !(e->flags & TOUCH_BIT_CACHED_HAS_AUTO_REPEAT)) {
        if (!element_is_toggle_active(e) && !(e->flags & TOUCH_BIT_LONG_PRESS_ARM) && !(e->flags & TOUCH_BIT_GESTURE_TIMER_ARMED))
            update_visual_layers(e);
        return;
    }

    if (element_has_autorepeat_only(e)) {
        if (!inside) update_visual_layers(e);
        return;
    }

    if (!inside) {
        e->flags &= ~(TOUCH_BIT_LONG_PRESS_ARM | TOUCH_BIT_GESTURE_TIMER_ARMED);
    }

    if (!has_primary && !(e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE)
        && !element_has_any_gesture_activity(e)
        && !(e->flags & TOUCH_BIT_GESTURE_TIMER_ARMED)) {
        update_visual_layers(e);
    }
}

// --- UP sub-functions ---

// Returns true if caller should jump to cleanup (deferred path fully handled).
static inline bool button_up_handle_deferred(TouchElement* e, float x, float y,
                                       uint64_t now_tick, TouchActionResult* restrict result) {
    if (!(e->flags & TOUCH_BIT_DEFER_PRIMARY)) return false;
    bool has_primary = element_has_primary(e);

    if ((e->flags & TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED) || (e->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED)) {
        clear_gesture_and_release_lp(e, result);
        e->flags &= ~(TOUCH_BIT_DEFER_PRIMARY | TOUCH_BIT_GESTURE_TIMER_ARMED);
        return true;
    }

    if ((e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE) && !(e->flags & TOUCH_BIT_CACHED_HAS_AUTO_REPEAT)) {
        bool qualifies = true;
        if (e->flags & TOUCH_BIT_CACHED_HAS_LONG_PRESS) {
            uint64_t elapsed = now_tick - e->down_tick;
            int lp_delay = g_state.cfg.long_press_delay_ticks;
            if (lp_delay > 0 && elapsed >= (uint64_t)lp_delay) qualifies = false;
        }
        if (qualifies && (e->flags & TOUCH_BIT_CACHED_HAS_GESTURE)) {
            float ddx = x - e->down_x, ddy = y - e->down_y;
            float th = g_state.cfg.gesture_threshold_px > 0
                ? (float)g_state.cfg.gesture_threshold_px : (float)g_state.cfg.gesture_threshold_px_fallback;
            if (ddx * ddx + ddy * ddy > th * th) qualifies = false;
        }
        if (qualifies) {
            if (has_primary) e->flags ^= TOUCH_BIT_SELECTED;
            if (e->flags & TOUCH_BIT_SELECTED) {
                if (has_primary) press_binding(result, &e->bindings[0], true);
            } else {
                if (has_primary) release_binding(result, &e->bindings[0]);
            }
        }
        e->flags &= ~TOUCH_BIT_DEFER_PRIMARY;
        finish_visual_update(e);
        return true;
    }

    if (has_primary && e->primary_sticky_mask == 0) {
        press_binding(result, &e->bindings[0], true);
        release_binding(result, &e->bindings[0]);
    }
    e->flags &= ~TOUCH_BIT_DEFER_PRIMARY;
    return true;
}

// Returns true if caller should jump to cleanup (non-AR toggle UP handled).
static inline bool button_up_handle_toggle_non_ar(TouchElement* e, uint64_t now_tick,
                                            TouchActionResult* restrict result) {
    bool has_primary = element_has_primary(e);
    if (!((e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE) && !(e->flags & TOUCH_BIT_CACHED_HAS_AUTO_REPEAT) && has_primary))
        return false;

    if ((e->flags & TOUCH_BIT_TOGGLE_SLIDE_OVER) && !(e->flags & TOUCH_BIT_IS_HOVER)) {
        if (e->flags & TOUCH_BIT_AUTO_REPEAT_PRIMARY_PRESSED) {
            if (!((e->flags & TOUCH_BIT_SELECTED) && element_has_gesture_toggle(e))
                && (e->toggle_debounce_last_tick == 0
                    || now_tick - e->toggle_debounce_last_tick >= (uint64_t)g_state.cfg.toggle_debounce_ticks))
                button_toggle_flip(e, has_primary, now_tick, result);
        } else if ((e->flags & TOUCH_BIT_SELECTED) && !element_has_gesture_toggle(e)) {
            button_toggle_release(e, has_primary, result);
        }
    } else if (e->flags & TOUCH_BIT_TOGGLE_SLIDE_OVER) {
        // HOVER: slide-over fully managed during MOVE; UP just cleans up
    } else if (e->flags & TOUCH_BIT_SELECTED) {
        suppress_element_gestures(e, result);
    }
    e->flags &= ~TOUCH_BIT_DEFER_PRIMARY;
    return true;
}

// --- UP: unified ---
// Toggle: skip release (binding stays held). Non-toggle: release binding.
void element_button_up(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    bool has_primary = element_has_primary(e);

    if ((e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE) && (e->flags & TOUCH_BIT_CACHED_HAS_AUTO_REPEAT)) {
        if (e->flags & TOUCH_BIT_SELECTED) {
            e->flags &= ~TOUCH_BIT_DEFER_PRIMARY;
            finish_visual_update(e);
            return;
        } else {
            if ((e->flags & TOUCH_BIT_AUTO_REPEAT_PRIMARY_PRESSED) && has_primary)
                release_binding(result, &e->bindings[0]);
            goto cleanup;
        }
    }

    if (!(e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE)) {
        int bt0 = e->bindings[0].type;
        if (__builtin_expect(bt0 == GAMEPAD_L3 || bt0 == GAMEPAD_R3, 0)) {
            bool was = (e->flags & TOUCH_BIT_SELECTED) != 0;
            if ((now_tick - e->down_tick) > (uint64_t)(g_state.cfg.long_press_delay_ticks + g_state.cfg.long_press_delay_ticks / 2))
                e->flags |= TOUCH_BIT_SELECTED;
            else
                e->flags &= ~TOUCH_BIT_SELECTED;
            if (was != ((e->flags & TOUCH_BIT_SELECTED) != 0)) update_visual_layers(e);
        }
    }

    if (button_up_handle_deferred(e, x, y, now_tick, result)) goto cleanup;
    if (button_up_handle_toggle_non_ar(e, now_tick, result)) goto cleanup;

    if (element_has_autorepeat_only(e)) {
        if ((e->flags & TOUCH_BIT_AUTO_REPEAT_PRIMARY_PRESSED) && has_primary)
            release_binding(result, &e->bindings[0]);
        e->flags &= ~(TOUCH_BIT_AUTO_REPEAT_PRIMARY_PRESSED | TOUCH_BIT_DEFER_PRIMARY
                       | TOUCH_BIT_GESTURE_TIMER_ARMED);
        goto cleanup;
    }

    if ((e->flags & TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED) || (e->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED)) {
        clear_gesture_and_release_lp(e, result);
    } else {
        if (has_primary && e->primary_sticky_mask == 0)
            release_binding(result, &e->bindings[0]);
    }

cleanup:
    e->flags &= ~(TOUCH_BIT_GESTURE_TIMER_ARMED | TOUCH_BIT_AUTO_REPEAT_PRIMARY_PRESSED);
}
