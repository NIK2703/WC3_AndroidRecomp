#include "core/internal.h"
#include "core/activation_mode.h"


#define PTR_SLOT(ptr_id) ((uint32_t)(ptr_id) % MAX_FINGERS)

// ─── Mode constants ──────────────────────────────────────────────────────────

const ActivationModeParams ACTIVATION_MODE_LOCK = {
    .move_button = BTN_MOVE_LOCK,
    .up_release = UP_RELEASE_ENGAGED,
    .up_clear_hovered = false,
    .up_reset_tracked = false,
};

const ActivationModeParams ACTIVATION_MODE_TRACK = {
    .move_button = BTN_MOVE_ACCUM,
    .up_release = UP_RELEASE_TRACKED,
    .up_clear_hovered = false,
    .up_reset_tracked = false,
};


const ActivationModeParams ACTIVATION_MODE_HOVER = {
    .move_button = BTN_MOVE_TRANS,
    .up_release = UP_RELEASE_ENGAGED,
    .up_clear_hovered = true,
    .up_reset_tracked = true,
};

// ─── Static helpers (moved from touch_processor_activation.c) ─────────────────

static inline void toggle_slide_over(TouchElement* btn, TouchActionResult* restrict result, uint64_t now_tick) {
    if (btn->flags & TOUCH_BIT_SELECTED) {
        if (element_has_gesture_toggle(btn)) {
            finish_visual_update(btn);
            return;
        }
        release_toggled_alternate_bindings(btn, result, true);
        release_bindings_list(result, btn->bindings, MAX_BINDINGS_PER_ELEMENT);
        _Pragma("GCC unroll 4")
        for (int k = 0; k < MAX_BINDINGS_PER_ELEMENT; k++) {
            TouchBinding* tb = &btn->bindings[k];
            if (tb->type == NONE) continue;
            if (tb->flags & TOUCH_BINDING_FLAG_TOGGLE)
                gesture_remove_toggled_action(tb->type, tb->keycode);
            else if (tb->flags & TOUCH_BINDING_FLAG_AUTO_REPEAT)
                gesture_remove_held_action(tb->type, tb->keycode);
        }
        if (g_state.gesture_held_count == 0)
            g_state.gesture_is_action_held = false;
        btn->flags &= ~TOUCH_BIT_SELECTED;
        update_visual_layers(btn);
    } else {
        press_bindings_list(result, btn->bindings, MAX_BINDINGS_PER_ELEMENT);
        btn->flags |= TOUCH_BIT_SELECTED;
        btn->auto_repeat_last_tick = now_tick;
        update_visual_layers(btn);
    }
    mark_element_dirty(btn);
}

static inline bool release_engaged_elements(TouchFinger* f, int ptr_id, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    bool handled = false;
    if (__builtin_expect(f == NULL, 0)) return false;
    uint8_t cnt = f->engaged_elem_count;
    for (uint8_t i = 0; i < cnt; i++) {
        TouchElement* e = &g_state.elements[f->engaged_elem_indices[i]];
        if (e->current_ptr_id == ptr_id) {
            handle_element_up(e, x, y, now_tick, result);
            handled = true;
        }
    }
    f->engaged_elem_count = 0;
    return handled;
}

// ─── MOVE (buttons): tracking / accumulation / transition ───────────────────

static bool handle_btn_move_transition(int ptr_id, float x, float y, TouchElement* btn,
                                       int curr_idx, TrackedButtons* tb, uint64_t now_tick,
                                       TouchActionResult* restrict result,
                                       const ActivationModeParams* params) {
    if (ptr_id < 0 || ptr_id >= MAX_FINGERS) return false;
    int pid_slot = ptr_id;
    int prev_idx = g_state.hovered_element_per_ptr[pid_slot];

    if (curr_idx >= 0 && curr_idx == prev_idx)
        return true;

    TouchFinger* f = find_finger(ptr_id);

    if (prev_idx >= 0 && prev_idx < g_state.element_count) {
        TouchElement* prev = &g_state.elements[prev_idx];
        release_element_bindings(prev, result, false);
        release_non_toggle_gestures(prev, result);
        clear_element_gesture_flags(prev, false);
        prev->current_ptr_id = INVALID_PTR_ID;
        prev->flags &= ~TOUCH_BIT_ENGAGED;
        finish_visual_update(prev);
    }

    if (curr_idx >= 0 && (btn->flags & TOUCH_BIT_CACHED_HAS_TOGGLE))
        if ((btn->flags & TOUCH_BIT_GESTURE_TIMER_ARMED) && btn->current_ptr_id < 0)
            btn->flags &= ~TOUCH_BIT_GESTURE_TIMER_ARMED;
    if (curr_idx >= 0 && (btn->flags & TOUCH_BIT_CACHED_HAS_TOGGLE) && !(btn->flags & TOUCH_BIT_GESTURE_TIMER_ARMED)
        && !(btn->flags & TOUCH_BIT_CACHED_HAS_LONG_PRESS) && !(btn->flags & TOUCH_BIT_CACHED_HAS_GESTURE)) {
        toggle_slide_over(btn, result, now_tick);
        btn->flags |= TOUCH_BIT_GESTURE_TIMER_ARMED;
        btn->current_ptr_id = ptr_id;
        btn->flags |= TOUCH_BIT_ENGAGED;
        if (f && f->engaged_elem_count < MAX_HELD_ACTIONS) {
            f->engaged_elem_indices[f->engaged_elem_count] = curr_idx;
            f->engaged_elem_count++;
        }
    } else if (curr_idx >= 0) {
        bool already_tracked = is_tracked(tb, curr_idx);
        if (!already_tracked && tb->count < MAX_TRACKED_PER_POINTER) {
            tb->element_indices[tb->count++] = curr_idx;
            tracked_set_bit(tb, curr_idx);
            reset_first_tracked_long_press(tb);
        }
        handle_element_down(btn, ptr_id, x, y, now_tick, result);
        if (tb->count > 1 || already_tracked)
            suppress_element_gestures(btn, result);

        if (btn->type == BUTTON && (btn->flags & TOUCH_BIT_DEFER_PRIMARY)) {
            if (btn->bindings[0].type != NONE)
                press_binding(result, &btn->bindings[0], false);
            btn->flags &= ~TOUCH_BIT_DEFER_PRIMARY;
            if (tb->count > 1 || already_tracked)
                btn->flags &= ~(TOUCH_BIT_LONG_PRESS_ARM | TOUCH_BIT_GESTURE_TIMER_ARMED);
        }
        finish_visual_update(btn);
    }

    g_state.hovered_element_per_ptr[pid_slot] = curr_idx >= 0 ? curr_idx : INVALID_PTR_ID;
    return false;
}

// Returns: current hovered element index, or -1 if none.
// The caller can use this for gesture state save/restore around the call.
__attribute__((hot))
int activation_mode_move_buttons(int ptr_id, float x, float y, uint64_t now_tick,
                                 TouchActionResult* result, const ActivationModeParams* params,
                                 TouchElement* cached_hit)
{
    TrackedButtons* tb = get_tracked_buttons(ptr_id);
    if (tb->count == 0) return -1;

    TouchElement* btn = cached_hit ? cached_hit : hit_test_element(x, y);
    int curr_idx = btn && btn->type == BUTTON ? (int)(btn - g_state.elements) : -1;


    switch (params->move_button) {
    case BTN_MOVE_LOCK:
        break;

    case BTN_MOVE_ACCUM: {
        if (curr_idx < 0) break;
        if (!(btn->flags & TOUCH_BIT_CACHED_HAS_TOGGLE)) {
            if (!is_tracked(tb, curr_idx) && tb->count < MAX_TRACKED_PER_POINTER) {
                tb->element_indices[tb->count++] = curr_idx;
                tracked_set_bit(tb, curr_idx);
                handle_element_down(btn, ptr_id, x, y, now_tick, result);
                if (tb->count > 1)
                    suppress_element_gestures(btn, result);
                reset_first_tracked_long_press(tb);
            }
        } else if (btn->current_ptr_id == ptr_id) {
            handle_element_move(btn, x, y, now_tick, result);
        }
        break;
    }

    case BTN_MOVE_TRANS:
        if (handle_btn_move_transition(ptr_id, x, y, btn, curr_idx, tb, now_tick, result, params))
            return curr_idx;
        break;
    }

    return curr_idx;
}

// Clear gesture timer on toggle elements not currently under a live finger.
// Prevents stale GESTURE_TIMER_ARMED from blocking toggle slide-over re-entry.
static inline void reset_unused_toggle_gesture_timers(void) {
    if (!g_state.cfg.caps_has_element_toggle) return;
    for (int i = 0; i < g_state.element_count; i++) {
        if (!(g_state.elements[i].flags & TOUCH_BIT_CACHED_HAS_TOGGLE)) continue;
        if (g_state.elements[i].current_ptr_id >= 0) continue;
        g_state.elements[i].flags &= ~TOUCH_BIT_GESTURE_TIMER_ARMED;
    }
}

// ─── UP: release elements per mode ──────────────────────────────────────────

bool activation_mode_up(int ptr_id, float x, float y, uint64_t now_tick,
                        TouchActionResult* result, const ActivationModeParams* params)
{
    if (__builtin_expect(g_state.element_count == 0, 0)) return false;
    if (__builtin_expect(ptr_id < 0 || ptr_id >= MAX_FINGERS, 0)) goto done;

    int pid_slot = ptr_id;
    bool handled = false;
    int prev_idx = g_state.hovered_element_per_ptr[pid_slot];

    if (params->up_release == UP_RELEASE_TRACKED) {
        TrackedButtons* tb = &g_state.tracked[pid_slot];
        if (__builtin_expect(tb->ptr_id != ptr_id, 0)) goto done;
        for (int j = 0; j < tb->count; j++) {
            int idx = tb->element_indices[j];
            if (idx < 0 || idx >= g_state.element_count) continue;
            handle_element_up(&g_state.elements[idx], x, y, now_tick, result);
            if (!element_is_toggle_active(&g_state.elements[idx])) {
                if (g_state.elements[idx].type != BUTTON)
                    vis_clear(&g_state.elements[idx], VF_TAP);
            }
        }
        reset_tracked_slot(tb, pid_slot);
        handled = true;
    } else {
        if (params->up_reset_tracked)
            reset_tracked_slot(&g_state.tracked[pid_slot], pid_slot);
        TouchFinger* f = find_finger(ptr_id);
        handled = release_engaged_elements(f, ptr_id, x, y, now_tick, result);
    }

done:
    reset_unused_toggle_gesture_timers();
    if (params->up_clear_hovered) {
        if (prev_idx >= 0 && prev_idx < g_state.element_count) {
            finish_visual_update(&g_state.elements[prev_idx]);
        }
        g_state.hovered_element_per_ptr[pid_slot] = INVALID_PTR_ID;
    }

    return handled;
}

// ─── Query / reset helpers (merged from touch_processor_activation.c) ────────

int activation_tracked_count(int ptr_id) {
    if (ptr_id < 0 || ptr_id >= MAX_FINGERS) return 0;
    TrackedButtons* tb = get_tracked_buttons(ptr_id);
    int count = tb->ptr_id == ptr_id ? tb->count : 0;
    return count;
}

int activation_hovered_for_ptr(int ptr_id) {
    if (ptr_id < 0 || ptr_id >= MAX_FINGERS) return -1;
    int idx = g_state.hovered_element_per_ptr[ptr_id];
    return idx;
}

__attribute__((cold)) void activation_reset(void) {
    for (int i = 0; i < MAX_FINGERS; i++) {
        reset_tracked_slot(&g_state.tracked[i], i);
    }
    for (int i = 0; i < g_state.element_count; i++) {
        TouchElement* e = &g_state.elements[i];
        if (!element_is_toggle_active(e)) {
            if (e->type == BUTTON) {
                update_visual_layers(e);
            } else {
                vis_clear(e, VF_TAP);
            }
        }
    }
    mark_all_dirty();
}
