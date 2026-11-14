#include "core/internal.h"
#include "tp_log.h"
#include "core/activation_mode.h"
#include "branch.h"
#include "scroll.h"


static inline GestureFingerCtx* finger_ctx(TouchFinger* f) {
    return &g_ctx[(int)(f - g_state.fingers)];
}

static inline void mark_button_visual_dirty(TouchElement* e) {
    if (e->type == BUTTON) update_visual_layers(e);
    mark_element_dirty(e);
}

#define CURSOR_SENSITIVITY_DIVISOR 100.0f

static inline int pointer_index(const TouchFinger* f) {
    return (int)((uint32_t)f->ptr_id % MAX_FINGERS);
}

static inline void reset_scroll_state(void) {
    g_state.scrolling = false;
    g_state.scroll_accum_y = 0;
}

static bool is_single_tap_pointer_move_candidate(TouchFinger* finger) {
    if (!g_state.cfg.is_ts) return false;
    if (g_state.gesture_second_active) return false;
    if (finger->state == GESTURE_STATE_LONG_PRESSING) return false;
    if (finger->scroll_mode) return false;
    if (finger_ctx(finger)->dt.post_drag) return false;
    return finger->cached_has_active_single_tap
        && !finger->cached_has_active_single_tap_drag
        && !finger->cached_has_active_long_press_drag
        && !finger->cached_has_active_double_tap
        && !finger->cached_has_active_double_tap_drag
        && !finger->cached_has_long_press_timer
        && !g_state.cfg.caps_has_scroll_bindings;
}

static void gesture_repress_saved_bindings(TouchElement* element, bool had_gest_swipe, bool had_gest_lp, TouchActionResult* restrict result);

typedef struct {
    TouchElement *hit_element;
    bool hit_element_valid;
} HitCache;

typedef struct {
    bool has_gesture;
    bool lp_arm;
    bool gest_swipe;
    bool gest_lp;
    bool gest_timer;
    bool toggled_lp;
    bool toggled_gesture;
} HoverRestoreState;

typedef struct {
    uint32_t flags; /* bitmask of gesture runtime bits to save/restore */
    uint32_t visual_flags;
} ElementGestureState;

#define ELEMENT_GESTURE_FLAGS_MASK \
    (TOUCH_BIT_GESTURE_SWIPE_TRIGGERED | TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED | \
     TOUCH_BIT_GESTURE_TOGGLED | TOUCH_BIT_LP_TOGGLED | TOUCH_BIT_LONG_PRESS_ARM | \
     TOUCH_BIT_GESTURE_TIMER_ARMED)

static ElementGestureState save_element_gesture_state(const TouchElement* element) {
    return (ElementGestureState){
        .flags = element->flags & ELEMENT_GESTURE_FLAGS_MASK,
        .visual_flags = element->visual_flags,
    };
}

static void restore_element_gesture_state(TouchElement* element, const ElementGestureState* state) {
    element->flags = (element->flags & ~ELEMENT_GESTURE_FLAGS_MASK) | state->flags;
    element->visual_flags = state->visual_flags;
    mark_button_visual_dirty(element);
}

static void gesture_restore_first_button_state(TouchFinger* finger, int pointer_index, int saved_hovered_idx,
    const HoverRestoreState* state,
    TrackedButtons* tracked, TouchActionResult* restrict result)
{
    if (!state->has_gesture || saved_hovered_idx < 0
        || saved_hovered_idx >= g_state.element_count) return;
    int current_hovered = g_state.hovered_element_per_ptr[pointer_index];
    if (current_hovered == saved_hovered_idx) return;
    TouchElement* prev = &g_state.elements[saved_hovered_idx];
    if (prev != &g_state.elements[tracked->element_indices[0]]) return;
    prev->flags &= ~(TOUCH_BIT_LONG_PRESS_ARM | TOUCH_BIT_GESTURE_SWIPE_TRIGGERED |
                      TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED | TOUCH_BIT_GESTURE_TIMER_ARMED |
                      TOUCH_BIT_LP_TOGGLED | TOUCH_BIT_GESTURE_TOGGLED);
    if (state->lp_arm) prev->flags |= TOUCH_BIT_LONG_PRESS_ARM;
    if (state->gest_swipe) prev->flags |= TOUCH_BIT_GESTURE_SWIPE_TRIGGERED;
    if (state->gest_lp) prev->flags |= TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED;
    if (state->gest_timer) prev->flags |= TOUCH_BIT_GESTURE_TIMER_ARMED;
    if (state->toggled_lp) prev->flags |= TOUCH_BIT_LP_TOGGLED;
    if (state->toggled_gesture) prev->flags |= TOUCH_BIT_GESTURE_TOGGLED;
    if (state->gest_swipe || state->gest_lp) {
        gesture_repress_saved_bindings(prev, state->gest_swipe, state->gest_lp, result);
        prev->current_ptr_id = finger->ptr_id;
        prev->flags |= TOUCH_BIT_ENGAGED;
    }
    mark_button_visual_dirty(prev);
}

__attribute__((hot))
static bool check_confirm_dt_waiting(TouchFinger* restrict finger, TouchActionResult* restrict result,
    TouchFinger* restrict main_finger, bool use_main_bindings)
{
    if (!main_finger) return false;
    GestureFingerCtx* _main_ctx = finger_ctx(main_finger);
    if (!_main_ctx->dt.waiting) return false;
    if (!gesture_is_within_tap_distance(finger->x, finger->y)) {
        gesture_cancel_double_tap_wait(result);
        return false;
    }
    TouchFinger* dt_target = use_main_bindings ? main_finger : finger;
    double_tap_confirm_internal(result, dt_target);
    TouchFinger* reset_target = use_main_bindings ? main_finger : finger;
    if (reset_target) {
        reset_target->state = GESTURE_STATE_TAP_WAITING;
        reset_finger_tap_state(reset_target);
        if (use_main_bindings) {
            reset_target->down_x = reset_target->x;
            reset_target->down_y = reset_target->y;
        }
    }
    return true;
}

/*
 * A main finger that has received UP and entered the double-tap window is
 * still active only to own that window.  ts_up_main_finger() clears
 * double_tap_original_id_set, so this flag distinguishes it from a main
 * finger that is physically down.
 */
static bool is_lifted_dt_main(TouchFinger* main) {
    return main && main->active
        && !main->double_tap_original_id_set
        && finger_ctx(main)->dt.waiting;
}

/*
 * A different pointer landing outside the double-tap radius is a new first
 * touch.  Cancel the old window, then retire its lifted owner before role
 * assignment.  Leaving it active made every later pointer look like a second
 * finger, so the cursor stopped moving and taps produced no first-finger
 * action until another pointer reused the stale id.
 */
static void cancel_lifted_dt_main_for_new_down(TouchFinger* finger,
        TouchActionResult* restrict result) {
    TouchFinger* main = find_finger(g_state.gesture_main_ptr_id);
    if (!main || main == finger || !is_lifted_dt_main(main)) return;
    if (gesture_is_within_tap_distance(finger->x, finger->y)) return;

    gesture_cancel_double_tap_wait(result);
    g_state.gesture_main_ptr_id = INVALID_PTR_ID;
    main->state = GESTURE_STATE_IDLE;
    deactivate_finger(main);
}

/*
 * The confirming double-tap pointer has finished.  If the owner of the
 * resolved window was already lifted, release its slot now; it has no UP
 * event left that could clean it up.
 */
static void retire_resolved_lifted_dt_main(TouchFinger* except) {
    TouchFinger* main = find_finger(g_state.gesture_main_ptr_id);
    if (!main || main == except) return;
    if (main->double_tap_original_id_set) return;
    if (finger_ctx(main)->dt.waiting) return;

    g_state.gesture_main_ptr_id = INVALID_PTR_ID;
    main->state = GESTURE_STATE_IDLE;
    deactivate_finger(main);
}

static void save_pending_resume_action(TouchFinger* main_finger, TouchActionResult* restrict result) {
    if (!main_finger || !main_finger->active) return;
    main_finger->pending_resume_action_count = 0;
    if (g_state.gesture_is_action_held) {
        int n = g_state.gesture_held_count < MAX_DEFERRED_BINDINGS ? g_state.gesture_held_count : MAX_DEFERRED_BINDINGS;
        memcpy(main_finger->pending_resume_action, g_state.gesture_held_actions, n * sizeof(TouchBinding));
        main_finger->pending_resume_action_count = n;
    }
    scroll_mode_save(main_finger, result);
}

static void cleanup_second_finger_up(TouchActionResult* restrict result) {
    TouchFinger* main = find_finger(g_state.gesture_main_ptr_id);
    if (main && main->pending_resume_action_count > 0) {
        execute_actions_hold(result, main->pending_resume_action, main->pending_resume_action_count);
        main->pending_resume_action_count = 0;
        main->state = GESTURE_STATE_DRAGGING;
    } else if (main && main->state == GESTURE_STATE_TAP_WAITING) {
        /*
         * The second finger consumed this touch.  If the main finger has no
         * held action to restore, do not let its eventual UP schedule a
         * second tap (that leaked a first-finger left click after a
         * second-finger tap).
         */
        main->state = GESTURE_STATE_IDLE;
        reset_finger_tap_state(main);
    }
    if (main && !main->scroll_mode) scroll_mode_restore(main, result);
    g_state.gesture_second_ptr_id = INVALID_PTR_ID;
}


static void setup_second_finger_state(void) {
    TouchFinger* second_finger = find_finger(g_state.gesture_second_ptr_id);
    if (second_finger) {
        second_finger->is_second_finger = true;
        setup_second_finger_bindings(second_finger);
    }
    g_state.gesture_second_active = true;
}

static void handle_ts_main_finger_down(TouchFinger* finger, float x, float y, uint64_t now_tick, TouchActionResult* restrict result, bool was_second_deferred) {
    {
        TouchFinger* _mf = find_finger(g_state.gesture_main_ptr_id);
        bool _dtw = _mf && finger_ctx(_mf)->dt.waiting;
        if (!_dtw && was_second_deferred && g_state.gesture_second_ptr_id >= 0)
            setup_second_finger_state();
    }

    finger->original_ptr_id = finger->ptr_id;
    finger->double_tap_original_id_set = true;
    if (g_state.cfg.is_ts) update_ts_pointer(x, y, result);

    gesture_branch(finger, finger_ctx(finger), result, now_tick, GESTURE_EVENT_DOWN, finger->x, finger->y);
}

static void handle_ts_second_finger_down(TouchFinger* finger, uint64_t now_tick, TouchActionResult* restrict result, TouchFinger* main_finger) {
    if (g_state.gesture_second_active) {
        finger->state = GESTURE_STATE_IDLE;
        return;
    }
    if (!(g_state.cfg.caps_mode_mask & g_state.cfg.caps_second_mask)
        && !g_state.cfg.caps_has_scroll_bindings) {
        bool _dtw = main_finger && finger_ctx(main_finger)->dt.waiting;
        if (!_dtw) {
            g_state.gesture_second_active = true;
            g_state.gesture_second_ptr_id = finger->ptr_id;
            finger->is_second_finger = true;
            finger->state = GESTURE_STATE_IDLE;
            return;
        }
    }

    if (check_confirm_dt_waiting(finger, result, main_finger, true)) {
        g_state.gesture_second_active = false;
        return;
    }

    save_pending_resume_action(main_finger, result);
    release_held_actions(result, 0);
    setup_second_finger_bindings(finger);
    gesture_branch(finger, finger_ctx(finger), result, now_tick, GESTURE_EVENT_DOWN, finger->x, finger->y);
}

static void handle_sim_touch_down(TouchFinger* finger, float x, float y, uint64_t now_tick) {
    int finger_count = active_finger_count();
    if (finger_count == 1) {
        g_state.sim_continue_click = true;
        g_state.sim_click_press_tick = now_tick + g_state.cfg.sim_click_press_ticks;
        g_state.sim_click_ptr_id = finger->ptr_id;
        int tx, ty;
        touch_transform_coords(x, y, &tx, &ty);
        g_state.last_touch_x = tx;
        g_state.last_touch_y = ty;
    } else if (finger_count == 2) {
        TouchFinger* first = NULL;
        for (int i = 0; i < MAX_FINGERS; i++) {
            if (g_state.fingers[i].active && g_state.fingers[i].ptr_id != finger->ptr_id) {
                first = &g_state.fingers[i]; break;
            }
        }
        if (first && (now_tick - first->down_tick) >= 3)
            g_state.sim_continue_click = true;
        else {
            g_state.sim_continue_click = false;
            g_state.sim_click_press_tick = 0;
        }
    }
}

static void process_element_hit(TouchElement* element, TouchFinger* finger,
    float x, float y, uint64_t now_tick, TouchActionResult* restrict result,
    bool has_passthrough, bool* found_lock, bool* handled, TouchElement** btn)
{
    if (has_passthrough && (element->flags & TOUCH_BIT_PASSTHROUGH_TOUCH))
        g_state.passthrough_active = true;

    if (element->flags & TOUCH_BIT_IS_LOCK) {
        if (!*found_lock) {
            handle_element_down(element, finger->ptr_id, x, y, now_tick, result);
            *found_lock = true;
        }
    } else if (element->type != BUTTON) {
        if (!*handled) {
            handle_element_down(element, finger->ptr_id, x, y, now_tick, result);
            if ((element->flags & TOUCH_BIT_ENGAGED) && !(element->flags & TOUCH_BIT_PASSTHROUGH_TOUCH)) *handled = true;
        }
    } else if (*btn == NULL) {
        *btn = element;
    }
}

static void hit_test_elements_down(TouchFinger* finger, float x, float y, uint64_t now_tick,
    TouchActionResult* restrict result, bool has_passthrough,
    bool* found_lock, bool* handled, TouchElement** btn)
{
    int min_c, max_c, min_r, max_r;
#ifndef NDEBUG
    int grid_hits = 0, linear_hits = 0;
#endif
    if (grid_neighbor_cells(x, y, &min_c, &max_c, &min_r, &max_r)) {
        for (int r = min_r; r <= max_r; r++) {
            for (int c = min_c; c <= max_c; c++) {
                int cell_idx = r * GRID_COLS + c;
                for (int j = g_state.grid_cell_start[cell_idx]; j < g_state.grid_cell_start[cell_idx + 1]; j++) {
                    TouchElement* entry = &g_state.elements[g_state.grid_cell_to_elems[j]];
                    if (!point_in_element(x, y, entry)) continue;

#ifndef NDEBUG
                    grid_hits++;
#endif
                    process_element_hit(entry, finger, x, y, now_tick, result,
                        has_passthrough, found_lock, handled, btn);
                    if (__builtin_expect(*found_lock && *handled && !g_state.passthrough_active, 0))
                        goto break_all;
                }
            }
        }
    }
    break_all:

    if (!*found_lock && !*handled) {
        for (int i = 0; i < g_state.element_count; i++) {
            TouchElement* entry = &g_state.elements[i];
            if (!point_in_element(x, y, entry)) continue;

#ifndef NDEBUG
            linear_hits++;
#endif
            process_element_hit(entry, finger, x, y, now_tick, result,
                has_passthrough, found_lock, handled, btn);
            if (__builtin_expect(*found_lock && *handled && !g_state.passthrough_active, 0))
                break;
        }
    }
}

// ============================================================
// handle_gesture_down — element hit-test & delegation
// ============================================================
static bool handle_gesture_down_element(TouchFinger* finger, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    const bool has_mouse_left = g_state.cfg.caps_has_mouse_left_element;
    const bool has_passthrough = g_state.cfg.caps_has_passthrough_elements;
    bool found_lock = false;
    bool handled = false;
    TouchElement* btn = NULL;

    g_state.passthrough_active = false;

    if (has_mouse_left && g_state.pointer_left_enabled) {
        for (int i = 0; i < g_state.element_count; i++) {
            if (g_state.elements[i].bindings[0].type == MOUSE_LEFT_BUTTON) {
                g_state.pointer_left_enabled = false;
                break;
            }
        }
    }

    hit_test_elements_down(finger, x, y, now_tick, result, has_passthrough, &found_lock, &handled, &btn);
    if (found_lock) {
        // Check if ANY engaged element is non-passthrough for this finger.
        // The global passthrough_active may be contaminated by other elements;
        // only block gesture routing if this finger actually owns a non-passthrough element.
        bool any_non_passthrough = false;
        for (uint8_t _ei = 0; _ei < finger->engaged_elem_count; _ei++) {
            TouchElement* _ee = &g_state.elements[finger->engaged_elem_indices[_ei]];
            if (_ee->current_ptr_id == finger->ptr_id && !(_ee->flags & TOUCH_BIT_PASSTHROUGH_TOUCH)) {
                any_non_passthrough = true;
                break;
            }
        }
        if (!any_non_passthrough)
            return handled;
        return true;
    }

    // Fallback: spatial-grid coordinate hit test when element scan didn't find a button.
    // Matches original Java: if (btn == NULL) btn = hit_test_element(x, y);
    if (btn == NULL) {
        btn = hit_test_element(x, y);
    }

    // TRACK/HOVER BUTTON
    if (btn && btn->type == BUTTON) {
        if (btn->flags & TOUCH_BIT_TOGGLE_SLIDE_OVER) {
            TrackedButtons* tracked = get_tracked_buttons(finger->ptr_id);
            int16_t btn_idx = (int16_t)element_index(btn);
            bool already = is_tracked(tracked, btn_idx);
            if (!already) {
                int16_t prev_ptr = btn->current_ptr_id;
                handle_element_down(btn, finger->ptr_id, x, y, now_tick, result);
                if ((btn->flags & TOUCH_BIT_CACHED_HAS_TOGGLE) && (btn->flags & TOUCH_BIT_CACHED_HAS_AUTO_REPEAT) && !(btn->flags & TOUCH_BIT_IS_HOVER)) {
                    arm_toggle_ar_gesture_timer(btn);
                } else if (prev_ptr != finger->ptr_id && btn->current_ptr_id == finger->ptr_id && tracked->count < MAX_TRACKED_PER_POINTER) {
                    tracked->element_indices[tracked->count++] = btn_idx;
                    tracked_set_bit(tracked, btn_idx);
                }
            }
            if (btn->flags & TOUCH_BIT_IS_HOVER)
                g_state.hovered_element_per_ptr[pointer_index(finger)] = btn_idx;
            if (!(btn->flags & TOUCH_BIT_PASSTHROUGH_TOUCH)) handled = true;
        } else if (btn) {
            handle_element_down(btn, finger->ptr_id, x, y, now_tick, result);
            if (!(btn->flags & TOUCH_BIT_PASSTHROUGH_TOUCH)) handled = true;
        }
    }

    // If any non-passthrough element was engaged but handled is still false
    // (e.g. passthrough contamination, LOCK fallthrough handled by above check),
    // promote to handled — the element owns the touch.
    if (!handled) {
        for (uint8_t _ei = 0; _ei < finger->engaged_elem_count; _ei++) {
            TouchElement* _ee = &g_state.elements[finger->engaged_elem_indices[_ei]];
            if (_ee->current_ptr_id == finger->ptr_id && !(_ee->flags & TOUCH_BIT_PASSTHROUGH_TOUCH)) {
                handled = true;
                break;
            }
        }
    }

    return handled;
}

// ============================================================
// handle_gesture_down — passthrough logic
// ============================================================
static void handle_gesture_down_passthrough(TouchFinger* finger, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    (void)x; (void)y; (void)now_tick;
    if (!g_state.passthrough_active) return;
    finger->state = GESTURE_STATE_IDLE;
    if (g_state.gesture_main_ptr_id < 0) {
        g_state.gesture_main_ptr_id = finger->ptr_id;
        finger_ctx(finger)->dt.post_drag = false;
        g_state.gesture_double_tap_consumed = false;
        g_state.gesture_second_active = false;
    }
}

// ============================================================
// handle_gesture_down
// ============================================================
__attribute__((hot))
void handle_gesture_down(TouchFinger* finger, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    finger->gesture_activated_in_touch = false;
    g_state.main_ptr_id = finger->ptr_id;

    cancel_lifted_dt_main_for_new_down(finger, result);

    if (handle_gesture_down_element(finger, x, y, now_tick, result)) {
        return;
    }

    handle_gesture_down_passthrough(finger, x, y, now_tick, result);
    if (g_state.passthrough_active) {
        return;
    }

    TouchFinger* main_finger = NULL;
    if (g_state.gesture_main_ptr_id >= 0) {
        main_finger = find_finger(g_state.gesture_main_ptr_id);
        if (!main_finger || !main_finger->active) {
            g_state.gesture_main_ptr_id = INVALID_PTR_ID;
            g_state.gesture_second_active = false;
        }
    }
    bool is_first = (g_state.gesture_main_ptr_id < 0);
    bool is_second = !is_first && (finger->ptr_id != g_state.gesture_main_ptr_id);

    if (is_first) {
        finger_ctx(finger)->dt.post_drag = false;
        g_state.gesture_double_tap_consumed = false;
        g_state.gesture_second_active = false;
        g_state.gesture_pending_double_count = 0;

        if (g_state.cfg.is_tp) {
            reset_scroll_state();
            if (g_state.sim_touch_screen && g_state.cfg.caps_has_gesture_bindings)
                handle_sim_touch_down(finger, x, y, now_tick);
        }

        bool was_second_deferred = g_state.gesture_deferred_second_finger_tap;
        g_state.gesture_deferred_second_finger_tap = false;
        handle_ts_main_finger_down(finger, x, y, now_tick, result, was_second_deferred);
        return;
    }

    if (is_second) {
        handle_ts_second_finger_down(finger, now_tick, result, main_finger);
        return;
    }

    // Same ptr_id re-tapping while gesture_main_ptr_id is still set
    // (e.g., second tap of single-finger DT where Android reused the ptr_id).
    // branch.c handle_dt_waiting_on_down will confirm the DT.
    if (g_state.cfg.is_tp) {
        reset_scroll_state();
        if (g_state.sim_touch_screen)
            handle_sim_touch_down(finger, x, y, now_tick);
    }
    gesture_branch(finger, finger_ctx(finger),
                   result, now_tick, GESTURE_EVENT_DOWN, finger->x, finger->y);
}

static void release_gesture_alternate_bindings(TouchElement* element,
    const ElementGestureState* saved, TouchActionResult* restrict result)
{
    int _ei = element_index(element);
    if ((saved->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED) && !(saved->flags & TOUCH_BIT_GESTURE_TOGGLED))
        release_bindings_list(result, ELEM_GEST(_ei), ELEM_GEST_COUNT(_ei));
    if ((saved->flags & TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED) && !(saved->flags & TOUCH_BIT_LP_TOGGLED))
        release_bindings_list(result, ELEM_LP(_ei), ELEM_LP_COUNT(_ei));
}

static void press_gesture_alternate_bindings(TouchElement* element,
    const ElementGestureState* saved, TouchActionResult* restrict result)
{
    int _ei = element_index(element);
    if (saved->flags & TOUCH_BIT_GESTURE_TOGGLED)
        press_bindings_list(result, ELEM_GEST(_ei), ELEM_GEST_COUNT(_ei));
    if (saved->flags & TOUCH_BIT_LP_TOGGLED)
        press_bindings_list(result, ELEM_LP(_ei), ELEM_LP_COUNT(_ei));
    if ((saved->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED) && !(saved->flags & TOUCH_BIT_GESTURE_TOGGLED))
        press_bindings_list(result, ELEM_GEST(_ei), ELEM_GEST_COUNT(_ei));
    if ((saved->flags & TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED) && !(saved->flags & TOUCH_BIT_LP_TOGGLED))
        press_bindings_list(result, ELEM_LP(_ei), ELEM_LP_COUNT(_ei));
}

// ============================================================
// handle_toggle_entry — unified toggle entry logic for slide-over
// ============================================================
static void gesture_repress_saved_bindings(
    TouchElement* element,
    bool had_gest_swipe, bool had_gest_lp,
    TouchActionResult* restrict result)
{
    int _ei = element_index(element);
    if (had_gest_swipe && !(element->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED) && !(element->flags & TOUCH_BIT_GESTURE_TOGGLED))
        press_bindings_list(result, ELEM_GEST(_ei), ELEM_GEST_COUNT(_ei));
    if (had_gest_lp && !(element->flags & TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED) && !(element->flags & TOUCH_BIT_LP_TOGGLED))
        press_bindings_list(result, ELEM_LP(_ei), ELEM_LP_COUNT(_ei));
}

// ============================================================
static void handle_toggle_entry(TouchElement* element, TouchActionResult* restrict result) {
    if (element_is_toggle_active(element)) {
        if (element_has_gesture_toggle(element)) {
            mark_button_visual_dirty(element);
            return;
        }
        release_toggled_alternate_bindings(element, result, true);
        for (int k = 0; k < MAX_BINDINGS_PER_ELEMENT; k++) {
            if (element->bindings[k].type == NONE) continue;
            release_binding(result, &element->bindings[k]);
        }
        element->flags &= ~TOUCH_BIT_SELECTED;
        mark_button_visual_dirty(element);
        return;
    }
    if (element_has_primary(element))
        press_binding(result, &element->bindings[0], true);
    element->flags |= TOUCH_BIT_SELECTED;
    element->flags |= TOUCH_BIT_GESTURE_TIMER_ARMED;
    mark_button_visual_dirty(element);
}

static bool save_hover_state(TouchFinger* finger, HoverRestoreState* out_state, int* out_saved_hovered_idx) {
    memset(out_state, 0, sizeof(*out_state));
    *out_saved_hovered_idx = -1;
    if (!g_state.cfg.caps_has_track_hover_buttons) return false;
    *out_saved_hovered_idx = g_state.hovered_element_per_ptr[pointer_index(finger)];
    TrackedButtons* tracked = get_tracked_buttons(finger->ptr_id);
    if (tracked->count > 0) {
        int first_idx = tracked->element_indices[0];
        if (first_idx >= 0 && first_idx < g_state.element_count) {
            TouchElement* first_btn = &g_state.elements[first_idx];
            if (first_btn->type == BUTTON && (first_btn->flags & TOUCH_BIT_IS_HOVER)) {
                out_state->lp_arm = (first_btn->flags & TOUCH_BIT_LONG_PRESS_ARM) != 0;
                out_state->gest_swipe = (first_btn->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED) != 0;
                out_state->gest_lp = (first_btn->flags & TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED) != 0;
                out_state->gest_timer = (first_btn->flags & TOUCH_BIT_GESTURE_TIMER_ARMED) != 0;
                out_state->toggled_lp = (first_btn->flags & TOUCH_BIT_LP_TOGGLED) != 0;
                out_state->toggled_gesture = (first_btn->flags & TOUCH_BIT_GESTURE_TOGGLED) != 0;
                out_state->has_gesture = true;
            }
        }
    }
    return out_state->has_gesture;
}

static void handle_toggle_slide_over(TouchFinger* finger, float x, float y, uint64_t now_tick, TouchActionResult* restrict result, HitCache* cache) {
    if (!cache->hit_element_valid) { cache->hit_element = hit_test_element(x, y); cache->hit_element_valid = true; }
    TouchElement* toggle_btn = cache->hit_element;
    if (toggle_btn && toggle_btn->type == BUTTON && (toggle_btn->flags & TOUCH_BIT_CACHED_HAS_TOGGLE)) {
        if (toggle_btn->flags & TOUCH_BIT_TOGGLE_SLIDE_OVER) {
            TrackedButtons* tracked = get_tracked_buttons(finger->ptr_id);
            bool already_tracked = is_tracked(tracked, element_index(toggle_btn));
            if (!already_tracked && !(toggle_btn->flags & TOUCH_BIT_GESTURE_TIMER_ARMED)) {
                if (toggle_btn->flags & TOUCH_BIT_CACHED_HAS_AUTO_REPEAT) {
                    if (!(toggle_btn->flags & TOUCH_BIT_IS_HOVER)) {
                        if (toggle_btn->current_ptr_id >= 0) {
                            int16_t eidx = (int16_t)element_index(toggle_btn);
                            finger_remove_engaged(finger, eidx);
                            toggle_btn->current_ptr_id = INVALID_PTR_ID;
                            toggle_btn->flags &= ~TOUCH_BIT_ENGAGED;
                        }
                        handle_element_down(toggle_btn, finger->ptr_id, x, y, now_tick, result);
                        arm_toggle_ar_gesture_timer(toggle_btn);
                        mark_element_dirty(toggle_btn);
                    }
                } else if (!(toggle_btn->flags & TOUCH_BIT_IS_HOVER)) {
                    handle_toggle_entry(toggle_btn, result);
                }
            }
        }
    } else {
        for (int i = 0; i < g_state.element_count; i++) {
            if (g_state.elements[i].flags & TOUCH_BIT_CACHED_HAS_TOGGLE
                && g_state.elements[i].current_ptr_id == finger->ptr_id)
                g_state.elements[i].flags &= ~TOUCH_BIT_GESTURE_TIMER_ARMED;
        }
    }
}

static void handle_track_hover_move(TouchFinger* finger, float x, float y, uint64_t now_tick, TouchActionResult* restrict result, HoverRestoreState* hover_state, int saved_hovered_idx, bool* had_element_move, HitCache* cache) {
    int ptr_idx = pointer_index(finger);
    TrackedButtons* tracked = get_tracked_buttons(finger->ptr_id);

    if (g_state.is_hover) {
        int prev_idx = g_state.hovered_element_per_ptr[ptr_idx];
        ActivationModeParams hover_params = ACTIVATION_MODE_HOVER;
        int curr_idx = activation_mode_move_buttons(finger->ptr_id, x, y, now_tick, result, &hover_params, cache->hit_element);

        bool was_transition = (curr_idx != prev_idx);

        if (was_transition && curr_idx >= 0 && tracked->count > 0
            && curr_idx == tracked->element_indices[0])
        {
            TouchElement* curr = &g_state.elements[curr_idx];
            bool already = is_tracked(tracked, curr_idx);
            if (already && !(curr->flags & TOUCH_BIT_GESTURE_TIMER_ARMED)) {
                if (!(curr->flags & TOUCH_BIT_CACHED_HAS_TOGGLE) || element_is_toggle_active(curr)) {
                    ElementGestureState saved = save_element_gesture_state(curr);
                    release_gesture_alternate_bindings(curr, &saved, result);
                    curr->current_ptr_id = INVALID_PTR_ID;
                    curr->flags &= ~TOUCH_BIT_ENGAGED;
                    handle_element_down(curr, finger->ptr_id, x, y, now_tick, result);
                    press_gesture_alternate_bindings(curr, &saved, result);
                    restore_element_gesture_state(curr, &saved);
                } else {
                    suppress_element_gestures(curr, result);
                }
            } else if (tracked->count > 1 || prev_idx < 0) {
                suppress_element_gestures(curr, result);
                if (element_is_toggle_active(curr) && (curr->flags & TOUCH_BIT_CACHED_HAS_TOGGLE))
                    curr->flags |= TOUCH_BIT_GESTURE_TIMER_ARMED;
            }
        }

        if (was_transition && prev_idx >= 0 && prev_idx < g_state.element_count) {
            memset(hover_state, 0, sizeof(*hover_state));
        }

        gesture_restore_first_button_state(finger, ptr_idx, saved_hovered_idx,
            hover_state, tracked, result);

        if (curr_idx >= 0 && tracked->count > 0 && curr_idx == tracked->element_indices[0]) {
            handle_element_move(&g_state.elements[curr_idx], x, y, now_tick, result);
            if (!(g_state.elements[curr_idx].flags & TOUCH_BIT_PASSTHROUGH_TOUCH))
                *had_element_move = true;
        }
    } else if (tracked->count > 0) {
        int first_idx = tracked->element_indices[0];
        activation_mode_move_buttons(finger->ptr_id, x, y, now_tick, result, &ACTIVATION_MODE_TRACK, cache->hit_element);

        gesture_restore_first_button_state(finger, ptr_idx, saved_hovered_idx,
            hover_state, tracked, result);

        if (first_idx >= 0 && first_idx < g_state.element_count) {
            TouchElement* first_btn = &g_state.elements[first_idx];
            if (first_btn->current_ptr_id == finger->ptr_id
                && first_btn->type == BUTTON && !(first_btn->flags & TOUCH_BIT_PASSTHROUGH_TOUCH)) {
                bool inside_first = point_in_element(x, y, first_btn);
                if (inside_first) {
                    handle_element_move(first_btn, x, y, now_tick, result);
                    *had_element_move = true;
                }
            }
        }
    }
}

static void handle_cursor_move(TouchFinger* finger, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    int active_count = active_finger_count();
    if (!g_state.cfg.is_tp || g_state.scrolling || active_count > 2) return;
    if (finger->scroll_mode) {
        return; // cursor frozen in scroll mode
    }
    if (finger->ptr_id != g_state.gesture_main_ptr_id) return; // only first finger moves cursor
    if (g_state.sim_touch_screen) {
        if (finger->travel_x > MAX_TAP_TRAVEL || finger->travel_y > MAX_TAP_TRAVEL)
            g_state.sim_continue_click = false;
        uint64_t elapsed = now_tick - finger->down_tick;
        if (elapsed > 3) {
            int tx, ty;
            touch_transform_coords(x, y, &tx, &ty);
            add_action(result, ACT_POINTER_MOVE, tx, ty, 0);
        }
    } else {
        float dx = x - finger->last_x;
        float dy = y - finger->last_y;
        float sx = g_state.cfg.xform_scale_x;
        float sy = g_state.cfg.xform_scale_y;
        float sens = g_state.cfg.cursor_speed / CURSOR_SENSITIVITY_DIVISOR;
        float accel_factor = g_state.cfg.cursor_acceleration_factor;
        float accel_threshold = (float)g_state.cfg.cursor_acceleration_threshold;
        dx *= sx;
        dy *= sy;
        dx *= sens;
        dy *= sens;
        if (accel_factor > 0.0f && accel_threshold > 0.0f) {
            float adx = fabsf(dx);
            if (adx > accel_threshold)
                dx = adx * accel_factor * (dx > 0 ? 1.0f : -1.0f);
            float ady = fabsf(dy);
            if (ady > accel_threshold)
                dy = ady * accel_factor * (dy > 0 ? 1.0f : -1.0f);
        }
        dx += finger->sub_pixel_x;
        dy += finger->sub_pixel_y;
        int id = lrintf(dx);
        int jd = lrintf(dy);
        finger->sub_pixel_x = dx - id;
        finger->sub_pixel_y = dy - jd;
        if (id != 0 || jd != 0) {
            add_action(result, ACT_POINTER_MOVE_DELTA, id, jd, 0);
        }
    }
}

static void handle_main_finger_propagate_to_second(float x, float y, uint64_t now_tick,
    TouchActionResult* restrict result)
{
    if (!g_state.gesture_second_active) return;
    TouchFinger* second_finger = find_finger(g_state.gesture_second_ptr_id);
    if (!second_finger) return;
    float second_dx = x - g_state.gesture_second_main_ref_x;
    float second_dy = y - g_state.gesture_second_main_ref_y;
    gesture_branch(second_finger, finger_ctx(second_finger), result, now_tick, GESTURE_EVENT_MOVE, second_dx, second_dy);
}

static void handle_second_finger_move(TouchFinger* finger, float x, float y, uint64_t now_tick,
    TouchActionResult* restrict result)
{
    if (!g_state.gesture_second_active || finger->ptr_id != g_state.gesture_second_ptr_id)
        return;
    if (finger->state < GESTURE_STATE_TAP_WAITING)
        return;

    float dx = x - finger->down_x;
    float dy = y - finger->down_y;

    if (g_state.cfg.caps_mode_mask & g_state.cfg.caps_second_mask)
        gesture_branch(finger, finger_ctx(finger), result, now_tick, GESTURE_EVENT_MOVE, dx, dy);
}

static void handle_gesture_move_processing(TouchFinger* finger, float x, float y, uint64_t now_tick,
    TouchActionResult* restrict result)
{
    const bool is_ts = g_state.cfg.is_ts;

    if (finger->ptr_id == g_state.gesture_main_ptr_id) {
        if (is_single_tap_pointer_move_candidate(finger)) {
            update_ts_pointer(x, y, result);
            finger->last_x = x;
            finger->last_y = y;
            return;
        }

        // If finger still has engaged non-passthrough elements, skip gesture
        // processing — the element should consume the touch, not the gesture system.
        // handle_engaged_element_move may have skipped them (e.g. TOGGLE_SLIDE_OVER,
        // PASSTHROUGH), but they are still engaged and prevent gesture routing.
        bool has_engaged_non_passthrough = false;
        for (uint8_t _ei = 0; _ei < finger->engaged_elem_count; _ei++) {
            TouchElement* _ee = &g_state.elements[finger->engaged_elem_indices[_ei]];
            if (_ee->current_ptr_id == finger->ptr_id
                && !(_ee->flags & TOUCH_BIT_PASSTHROUGH_TOUCH)) {
                has_engaged_non_passthrough = true;
                break;
            }
        }
        if (__builtin_expect(has_engaged_non_passthrough, 0)) {
            finger->last_x = x;
            finger->last_y = y;
            return;
        }

        float dx = x - finger->down_x;
        float dy = y - finger->down_y;

        gesture_branch(finger, finger_ctx(finger), result, now_tick, GESTURE_EVENT_MOVE, dx, dy);

        handle_main_finger_propagate_to_second(x, y, now_tick, result);
    }

    handle_second_finger_move(finger, x, y, now_tick, result);

    if (is_ts) {
        if ((g_state.gesture_main_ptr_id < 0 || finger->ptr_id == g_state.gesture_main_ptr_id)
            && !finger->scroll_mode)
            update_ts_pointer(x, y, result);
        finger->last_x = x;
        finger->last_y = y;
    }
}

static bool handle_engaged_element_move(TouchFinger* finger, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    uint8_t cnt = finger->engaged_elem_count;
    bool had_element_move = false;
    for (uint8_t i = 0; i < cnt; i++) {
        TouchElement* element = &g_state.elements[finger->engaged_elem_indices[i]];
        if (element->current_ptr_id != finger->ptr_id) continue;
        if ((element->flags & TOUCH_BIT_PASSTHROUGH_TOUCH) && !(element->flags & TOUCH_BIT_IS_HOVER))
            continue;
        /*
         * Slide-over buttons normally move through the hover/track state
         * machine, which only forwards MOVE while the pointer is still inside
         * the button and releases it on the first outside sample.  A button
         * with a swipe binding must keep ownership of the pointer until UP:
         * the gesture distance is measured from its original down point, and
         * the part of the path outside the rect counts too.  Once such a
         * button is left, feed it to element_button_move() directly so the
         * hover transition cannot cancel recognition before the threshold is
         * reached.  While the pointer is inside, keep the normal hover path.
         */
        if (element->type == BUTTON &&
            (element->flags & TOUCH_BIT_TOGGLE_SLIDE_OVER) &&
            (!(element->flags & TOUCH_BIT_CACHED_HAS_GESTURE) ||
             point_in_element(x, y, element)))
            continue;
        handle_element_move(element, x, y, now_tick, result);
        had_element_move = true;
    }
    return had_element_move;
}

static bool check_passthrough_engagement(TouchFinger* finger) {
    if (g_state.cfg.caps_has_passthrough_elements) {
        uint8_t count = finger->engaged_elem_count;
        for (uint8_t i = 0; i < count; i++) {
            TouchElement* e = &g_state.elements[finger->engaged_elem_indices[i]];
            if (passthrough_blocks_move(e)) {
                return true;
            }
        }
    }
    return false;
}

// ============================================================
// handle_gesture_move — element interaction phase
// ============================================================
static bool handle_move_button_interactions(
    TouchFinger* finger, float x, float y, uint64_t now_tick,
    TouchActionResult* restrict result,
    bool has_track_hover, bool has_element_toggle,
    HoverRestoreState* hover_state, int saved_hovered_idx,
    HitCache* cache)
{
    // Early exit when no elements exist — skip all element interactions
    if (__builtin_expect(g_state.element_count == 0, 0)) return false;

    bool had_element_move = handle_engaged_element_move(finger, x, y, now_tick, result);
    if (had_element_move) return true;

    bool is_gesture_zone_finger = finger->state > GESTURE_STATE_IDLE;
    bool has_passthrough_engagement = check_passthrough_engagement(finger);
    bool skip_element_activation = is_gesture_zone_finger || has_passthrough_engagement;

    if (has_element_toggle && !skip_element_activation)
        handle_toggle_slide_over(finger, x, y, now_tick, result, cache);

    if (has_track_hover && !skip_element_activation)
        handle_track_hover_move(finger, x, y, now_tick, result, hover_state, saved_hovered_idx, &had_element_move, cache);

    if (had_element_move) return true;

    if (has_track_hover) {
        TrackedButtons* tracked = get_tracked_buttons(finger->ptr_id);
        if (tracked->count > 0) {
            int first_idx = tracked->element_indices[0];
            if (first_idx >= 0 && first_idx < g_state.element_count) {
                TouchElement* first = &g_state.elements[first_idx];
                if (first->type == BUTTON && !(first->flags & TOUCH_BIT_PASSTHROUGH_TOUCH))
                    return true;
            }
        }
    }

    if (!cache->hit_element_valid) { cache->hit_element = hit_test_element(x, y); cache->hit_element_valid = true; }
    TouchElement* elem = cache->hit_element;
    if (elem && elem->current_ptr_id == finger->ptr_id && !(elem->flags & TOUCH_BIT_PASSTHROUGH_TOUCH)) {
        handle_element_move(elem, x, y, now_tick, result);
        return true;
    }

    return false;
}

// ============================================================
// handle_gesture_move — gesture + cursor phase
// ============================================================
static void handle_move_gesture_and_cursor(
    TouchFinger* finger, float x, float y, uint64_t now_tick,
    TouchActionResult* restrict result)
{
    handle_gesture_move_processing(finger, x, y, now_tick, result);
    handle_cursor_move(finger, x, y, now_tick, result);
}

// ============================================================
// handle_gesture_move
// ============================================================
__attribute__((hot))
void handle_gesture_move(TouchFinger* finger, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    // Fast-path: scroll mode skips all element interactions, hover, and gesture processing
    if (__builtin_expect(finger->scroll_mode, 0)) {
        scroll_mode_process(finger, result);
        finger->last_x = x; finger->last_y = y;
        return;
    }

    const bool has_track_hover = g_state.cfg.caps_has_track_hover_buttons;
    const bool has_element_toggle = g_state.cfg.caps_has_element_toggle;

    TouchElement* hit_element = NULL;
    bool hit_element_valid = false;

    if (!finger->cached_has_moved_beyond_threshold
        && finger->ptr_id == g_state.gesture_main_ptr_id
        && (fabsf(finger->x - finger->down_x) > g_state.cfg.drag_threshold_px || fabsf(finger->y - finger->down_y) > g_state.cfg.drag_threshold_px))
        finger->cached_has_moved_beyond_threshold = true;

    HoverRestoreState hover_state;
    int saved_hovered_idx = -1;
    if (has_track_hover)
        save_hover_state(finger, &hover_state, &saved_hovered_idx);

    HitCache hit_cache = { .hit_element = hit_element, .hit_element_valid = hit_element_valid };
    if (handle_move_button_interactions(finger, x, y, now_tick, result,
            has_track_hover, has_element_toggle, &hover_state, saved_hovered_idx,
            &hit_cache))
    {
        finger->last_x = x;
        finger->last_y = y;
        return;
    }

    handle_move_gesture_and_cursor(finger, x, y, now_tick, result);

    finger->last_x = x; finger->last_y = y;
}

static void ts_up_main_finger(TouchFinger* finger, uint64_t now_tick, TouchActionResult* restrict result) {
    g_state.gesture_deferred_second_finger_tap = g_state.gesture_second_active;
    finger->pending_resume_action_count = 0;
    finger->double_tap_original_id_set = false;
    gesture_branch(finger, finger_ctx(finger), result, now_tick, GESTURE_EVENT_UP, finger->x, finger->y);
}

static void deactivate_finger_to_idle(TouchFinger* finger) {
    finger->state = GESTURE_STATE_IDLE;
    deactivate_finger(finger);
}

static void ts_up_second_double_tap_waiting(TouchFinger* finger, TouchActionResult* restrict result) {
    gesture_clear_second_finger_state();
    deactivate_finger_to_idle(finger);
}

static void ts_up_original_id_set(TouchFinger* finger, TouchActionResult* restrict result) {
    if (!g_state.gesture_second_active) {
        release_held_actions(result, 0);
        finger->pending_resume_action_count = 0;
        finger->double_tap_original_id_set = false;
        g_state.gesture_main_ptr_id = INVALID_PTR_ID;
        g_state.gesture_second_active = false;
    } else {
        finger->double_tap_original_id_set = false;
    }
    GestureFingerCtx* _uctx = finger_ctx(finger);
    if (_uctx->dt.post_drag) {
        _uctx->dt.post_drag = false;
        release_held_actions(result, 0);
    }
    deactivate_finger_to_idle(finger);
}

static void ts_up_second_finger(TouchFinger* finger, uint64_t now_tick, TouchActionResult* restrict result) {
    gesture_branch(finger, finger_ctx(finger), result, now_tick, GESTURE_EVENT_UP, finger->x, finger->y);
    cleanup_second_finger_up(result);
}

static inline void ts_up_idle_double_tap(TouchFinger* finger, uint64_t now_tick, TouchActionResult* restrict result) {
    // If DT was just confirmed via check_confirm_dt_waiting (new-ptr_id second tap),
    // the deferred D bindings are on the main finger's context. Fire them now.
    TouchFinger* _mf2 = find_finger(g_state.gesture_main_ptr_id);
    GestureFingerCtx* _mf2_ctx = _mf2 ? finger_ctx(_mf2) : NULL;
    if (_mf2_ctx && _mf2_ctx->dt.post_drag) {
        if (_mf2_ctx->deferred_double.count > 0) {
            execute_actions(result, _mf2_ctx->deferred_double.items, _mf2_ctx->deferred_double.count);
            _mf2_ctx->deferred_double.count = 0;
        }
        _mf2_ctx->dt.post_drag = false;
        release_held_actions(result, 0);
        deactivate_finger_to_idle(finger);
    } else {
        gesture_branch(finger, finger_ctx(finger), result, now_tick, GESTURE_EVENT_UP, finger->x, finger->y);
        deactivate_finger_to_idle(finger);
    }
    retire_resolved_lifted_dt_main(finger);
}

static void handle_ts_up(TouchFinger* finger, uint64_t now_tick, TouchActionResult* restrict result) {
    if (finger->ptr_id == g_state.gesture_main_ptr_id) {
        ts_up_main_finger(finger, now_tick, result);
    } else {
        TouchFinger* _sf2 = find_finger(g_state.gesture_second_ptr_id);
        GestureFingerCtx* _sf2_ctx = _sf2 ? finger_ctx(_sf2) : NULL;
        if (_sf2_ctx && _sf2_ctx->dt.waiting) {
            ts_up_second_double_tap_waiting(finger, result);
        } else if (finger->double_tap_original_id_set && g_state.gesture_main_ptr_id >= 0
                   && finger->ptr_id == finger->original_ptr_id) {
            ts_up_original_id_set(finger, result);
        } else if (g_state.gesture_second_active && finger->ptr_id == g_state.gesture_second_ptr_id) {
            ts_up_second_finger(finger, now_tick, result);
        } else if (finger->state == GESTURE_STATE_IDLE
                   && (finger->cached_has_active_double_tap || finger->cached_has_active_double_tap_drag)) {
            ts_up_idle_double_tap(finger, now_tick, result);
        } else {
            deactivate_finger(finger);
        }
    }
}

// ============================================================
// handle_gesture_up
// ============================================================
__attribute__((hot))
void handle_gesture_up(TouchFinger* finger, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    int ptr_idx = pointer_index(finger);
    TrackedButtons* tracked = get_tracked_buttons(finger->ptr_id);
    bool had_tracked = false;
    if (g_state.cfg.caps_has_track_hover_buttons && tracked->ptr_id == finger->ptr_id && tracked->count > 0) {
        had_tracked = activation_mode_up(finger->ptr_id, x, y, now_tick, result,
            g_state.is_track
                ? &ACTIVATION_MODE_TRACK
                : &ACTIVATION_MODE_HOVER);
    }
    g_state.hovered_element_per_ptr[ptr_idx] = INVALID_PTR_ID;

    const int ptr_id = finger->ptr_id;
    bool had_element = false;
    uint8_t count = finger->engaged_elem_count;
    for (uint8_t i = 0; i < count; i++) {
        TouchElement* element = &g_state.elements[finger->engaged_elem_indices[i]];
        if (element->current_ptr_id == ptr_id) {
            handle_element_up(element, x, y, now_tick, result);
            had_element = true;
        }
    }
    finger->engaged_elem_count = 0;
    if (had_tracked || had_element) {
        if (g_state.gesture_main_ptr_id == ptr_id) {
            g_state.gesture_main_ptr_id = INVALID_PTR_ID;
            gesture_clear_second_finger_globals();
        }
        g_state.main_ptr_id = INVALID_PTR_ID;
        deactivate_finger(finger);
        return;
    }

    if (!current_mode_has_gestures() && g_state.dt_waiting_count == 0) {
        deactivate_finger(finger);
        g_state.main_ptr_id = INVALID_PTR_ID;
        return;
    }

    handle_ts_up(finger, now_tick, result);

    g_state.main_ptr_id = INVALID_PTR_ID;
}
