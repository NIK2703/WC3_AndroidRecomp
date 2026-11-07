#ifndef BINDING_HELPERS_H
#define BINDING_HELPERS_H

#include "core/state.h"

// Binding dispatch function declarations (must precede inline functions that call them)
void add_action(TouchActionResult* restrict r, ActionType type, int param0, int param1, int param2);
void release_held_actions(TouchActionResult* restrict result, uint64_t now_tick);
bool is_modifier_binding(const TouchBinding* b);
void execute_actions(TouchActionResult* restrict result, const TouchBinding* actions, int count);
void execute_actions_hold(TouchActionResult* restrict result, const TouchBinding* actions, int count);
void release_binding(TouchActionResult* restrict result, const TouchBinding* b);
void press_binding(TouchActionResult* restrict result, const TouchBinding* b, bool hold);

#define BINDING_GAMEPAD_COUNT 24
#define GAMEPAD_RIGHT_STICK_BASE 16
#define GAMEPAD_RIGHT_STICK_COUNT 4

static inline bool current_mode_has_gestures(void) {
    return g_state.cfg.caps_mode_mask != 0;
}

static inline bool current_mode_has_gesture(GestureType t) {
    return (g_state.cfg.caps_mode_mask & GESTURE_MASK(t)) != 0;
}

static inline void copy_main_finger_bindings(FingerBindings* fb, const GestureModeBindings* mb) {
    fb->single_tap = mb->single_tap;            fb->single_tap_count = mb->single_tap_count;
    fb->long_press = mb->long_press;            fb->long_press_count = mb->long_press_count;
    fb->double_tap = mb->double_tap;            fb->double_tap_count = mb->double_tap_count;
    fb->single_tap_drag = mb->single_tap_drag;  fb->single_tap_drag_count = mb->single_tap_drag_count;
    fb->long_press_drag = mb->long_press_drag;  fb->long_press_drag_count = mb->long_press_drag_count;
    fb->double_tap_drag = mb->double_tap_drag;  fb->double_tap_drag_count = mb->double_tap_drag_count;
}

static inline void copy_second_finger_bindings(FingerBindings* fb, const GestureModeBindings* mb) {
    fb->single_tap = mb->single_2nd;          fb->single_tap_count = mb->single_2nd_count;
    fb->long_press = NULL;                     fb->long_press_count = 0;
    fb->double_tap = mb->double_2nd;          fb->double_tap_count = mb->double_2nd_count;
    fb->single_tap_drag = mb->single_drag_2nd; fb->single_tap_drag_count = mb->single_drag_2nd_count;
    fb->long_press_drag = NULL;                fb->long_press_drag_count = 0;
    fb->double_tap_drag = mb->double_drag_2nd; fb->double_tap_drag_count = mb->double_drag_2nd_count;
}

static inline void setup_main_finger_bindings(FingerBindings* fb) {
    copy_main_finger_bindings(fb, &g_state.cfg.cached_mode_bindings);
}

static inline void setup_second_finger_bindings(TouchFinger* f) {
    FingerBindings* fb = &f->bindings;
    if (!(g_state.cfg.caps_mode_mask & g_state.cfg.caps_second_mask)) {
        memset(fb, 0, sizeof(*fb));
        touch_finger_cache_bindings_state(f);
        return;
    }
    const GestureModeBindings* mb = &g_state.cfg.cached_mode_bindings;
    copy_second_finger_bindings(fb, mb);
    touch_finger_cache_bindings_state(f);
}

static inline bool bindings_equal(const TouchBinding* a, int a_count, const TouchBinding* b, int b_count) {
    if (a_count != b_count) return false;
    for (int i = 0; i < a_count; i++)
        if (a[i].type != b[i].type || a[i].keycode != b[i].keycode
            || a[i].modifiers != b[i].modifiers || a[i].flags != b[i].flags)
            return false;
    return true;
}

static inline void release_gamepad_axis(const TouchElement* e, TouchActionResult* restrict result) {
    if (e->flags & TOUCH_BIT_CACHED_BIND0_IS_GAMEPAD)
        add_action(result, ACT_GAMEPAD_AXIS, !(e->flags & TOUCH_BIT_CACHED_BIND0_IS_RIGHT_STICK), 0, 0);
}

static inline void emit_gamepad_axis(TouchElement* e, float x_val, float y_val, TouchActionResult* restrict result) {
    if (!e) return;
    int is_left = !(e->flags & TOUCH_BIT_CACHED_BIND0_IS_RIGHT_STICK);
    add_action(result, ACT_GAMEPAD_AXIS, is_left, (int)lroundf(x_val * GAMEPAD_AXIS_MAX), (int)lroundf(y_val * GAMEPAD_AXIS_MAX));
}

static inline bool is_mouse_move_binding(const TouchBinding* b) {
    return b->type >= MOUSE_MOVE_LEFT && b->type <= MOUSE_MOVE_DOWN;
}

static inline bool is_keyboard_binding(const TouchBinding* b) {
    return b->type >= KEYBOARD_FIRST && b->type <= KEYBOARD_LAST;
}

static inline bool is_gamepad_binding(const TouchBinding* b) {
    return b->type >= GAMEPAD_BASE && b->type < GAMEPAD_BASE + BINDING_GAMEPAD_COUNT;
}

static inline bool is_right_stick_binding(const TouchElement* e) {
    int bt = e->bindings[0].type - GAMEPAD_BASE;
    return bt >= GAMEPAD_RIGHT_STICK_BASE && bt < GAMEPAD_RIGHT_STICK_BASE + GAMEPAD_RIGHT_STICK_COUNT;
}

static inline void release_bindings_list(TouchActionResult* restrict result, TouchBinding* bindings, int count) {
    if (count == 0) return;
    for (int k = count - 1; k >= 0; k--) {
        if (bindings[k].type == NONE) continue;
        if (!is_modifier_binding(&bindings[k]))
            release_binding(result, &bindings[k]);
    }
    for (int k = count - 1; k >= 0; k--) {
        if (bindings[k].type == NONE) continue;
        if (is_modifier_binding(&bindings[k]))
            release_binding(result, &bindings[k]);
    }
}

static inline void press_bindings_list(TouchActionResult* restrict result, TouchBinding* bindings, int count) {
    if (count == 0) return;
    for (int k = 0; k < count; k++) {
        if (bindings[k].type == NONE) continue;
        if (is_modifier_binding(&bindings[k]))
            press_binding(result, &bindings[k], true);
    }
    for (int k = 0; k < count; k++) {
        if (bindings[k].type == NONE) continue;
        if (!is_modifier_binding(&bindings[k]))
            press_binding(result, &bindings[k], true);
    }
}

static inline bool remove_action_from_array(TouchBinding* actions, int* count,
    uint64_t* ar_times, int type, int keycode) {
    for (int t = 0; t < *count; t++) {
        if (actions[t].type == type && actions[t].keycode == keycode) {
            actions[t] = actions[--(*count)];
            if (ar_times) ar_times[t] = ar_times[*count];
            return true;
        }
    }
    return false;
}

static inline bool gesture_remove_toggled_action(int type, int keycode) {
    return remove_action_from_array(g_state.gesture_toggled_actions,
        &g_state.gesture_toggled_count, g_state.gesture_auto_repeat_last_tick,
        type, keycode);
}

static inline bool gesture_remove_held_action(int type, int keycode) {
    return remove_action_from_array(g_state.gesture_held_actions,
        &g_state.gesture_held_count, g_state.gesture_auto_repeat_last_tick_held,
        type, keycode);
}

static inline void gesture_clear_deferred_tap(void) {
    g_state.gesture_deferred_tap_count = 0;
    g_state.gesture_pending_double_count = 0;
    g_state.gesture_pending_deferred_double_count = 0;
}

static inline void gesture_clear_second_finger_globals(void) {
    g_state.gesture_second_active = false;
    g_state.gesture_second_ptr_id = INVALID_PTR_ID;
    g_state.gesture_deferred_second_finger_tap = false;
    g_state.gesture_second_main_ref_x = 0;
    g_state.gesture_second_main_ref_y = 0;
}

static inline void gesture_clear_second_finger_state(void) {
    TouchFinger* sf = find_finger(g_state.gesture_second_ptr_id);
    if (sf) {
        int idx = (int)(sf - g_state.fingers);
        if (idx >= 0 && idx < MAX_FINGERS) {
            if (g_ctx[idx].dt.waiting) {
                g_ctx[idx].dt.waiting = false;
                g_state.dt_waiting_count--;
            }
            g_ctx[idx].dt.post_drag = false;
            g_ctx[idx].deferred_double.count = 0;
            g_ctx[idx].pending_double.count = 0;
        }
    }
}

static inline void copy_bindings_bounded(const TouchBinding* src, int src_count, TouchBinding* dst, int* dst_count, int dst_max) {
    for (int _i = 0; _i < src_count && _i < dst_max; _i++)
        dst[_i] = src[_i];
    *dst_count = src_count > dst_max ? dst_max : src_count;
}

#endif
