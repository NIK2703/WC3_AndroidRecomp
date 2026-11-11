// Touch processor timer/tick orchestrator.
// Delegates to sub-modules: auto_repeat, long_press, range_tick, mouse_move_tick.

#include "core/internal.h"
#include "core/activation_mode.h"
#include "xserver/core/x11_dispatch.h"

extern void udp_mouse_send(int flags, int dx, int dy, int wheel);
#define MF_MOVE 0x0001

extern TouchProcessorState g_state;

extern int g_current_finger_idx;

// M21: Pre-computed speed cache to avoid FP multiply every tick
static float cached_speed_x10 = 0.0f;
static float cached_speed = -1.0f;

__attribute__((hot)) void touch_processor_tick(TouchActionResult* restrict result, uint64_t now_tick) {
    wc3_timer_registry_invalidate();
    result->count = 0;
    g_current_finger_idx = -1;

    g_state.last_tick_time = now_tick;

    // C-side mouse move timer: analog offset path (InputControlsView MOUSE_MOVE bindings)
    float ox = g_state.mouse_move_offset_x;
    float oy = g_state.mouse_move_offset_y;
    if (ox != 0.0f || oy != 0.0f) {
        if (g_state.mouse_move_cursor_speed != cached_speed) {
            cached_speed = g_state.mouse_move_cursor_speed;
            cached_speed_x10 = cached_speed * 10.0f;
        }
        float speed = cached_speed_x10;
        int dx = (int)(ox * speed);
        int dy = (int)(oy * speed);
        if (dx != 0 || dy != 0) {
            udp_mouse_send(MF_MOVE, dx, dy, 0);
        }
    }
    // Digital path (dpad/stick via ACT_START_MOUSE_MOVE)
    else if (__builtin_expect(g_state.mouse_move_active, 0)) {
        add_action(result, ACT_MOUSE_EVENT,
                   0x0001 /*MOUSEEVENTF_MOVE*/,
                   g_state.mouse_move_dx,
                   g_state.mouse_move_dy);
    }

    process_scheduled_actions(result, now_tick);

    // Fallback: if actions are still held with no pending schedule, release.
    // Only fires when no finger is active (prevents premature release
    // during active drag where execute_actions_hold keeps adding to schedule).
    if (g_state.scheduled_action_count == 0 && g_state.gesture_is_action_held
        && g_state.active_finger_count == 0) {
        release_held_actions(result, now_tick);
    }

    // Gesture-level timeouts
    if (current_mode_has_gestures() || g_state.dt_waiting_count > 0) {
        gesture_tick(now_tick, result);
    }

    // Button elements: auto-repeat + long-press + gesture timer
    int button_count = g_state.button_count;
    TouchElement* elements = g_state.elements;
    int16_t* button_indices = g_state.button_indices;
    bool has_ar = g_state.cfg.caps_has_auto_repeat_buttons;
    bool has_lp = g_state.cfg.caps_has_any_element_long_press;
    bool has_gest = g_state.cfg.caps_has_any_element_gesture;
    if (button_count > 0 && (has_ar || has_lp || has_gest)) {
        ELEMENTS_LOCK();
        for (int _bi = 0; _bi < button_count; _bi++) {
            TouchElement* e = &elements[button_indices[_bi]];

            if (__builtin_expect((e->flags & TOUCH_BIT_ENGAGED) && e->current_ptr_id < 0, 0)) {
                e->flags &= ~TOUCH_BIT_ENGAGED;
                mark_element_dirty(e);
            }

            TouchFinger* f = find_finger(e->current_ptr_id);
            if (has_ar && (e->flags & TOUCH_BIT_CACHED_HAS_AUTO_REPEAT))
                process_button_auto_repeat(e, now_tick, result);
            if (has_lp && (e->flags & TOUCH_BIT_CACHED_HAS_LONG_PRESS))
                process_button_long_press(e, now_tick, result, f);
            if (has_gest && (e->flags & TOUCH_BIT_CACHED_HAS_GESTURE))
                process_button_gesture_timer(e, now_tick, result, f);

            update_visual_layers_inline(e);
        }
        ELEMENTS_UNLOCK();
    }

    // Gesture auto-repeat bursts
    process_auto_repeat_burst(result, g_state.gesture_toggled_actions, g_state.gesture_toggled_count, g_state.gesture_auto_repeat_last_tick, now_tick);
    if (__builtin_expect(g_state.gesture_is_action_held, 0))
        process_auto_repeat_burst(result, g_state.gesture_held_actions, g_state.gesture_held_count, g_state.gesture_auto_repeat_last_tick_held, now_tick);

    // Range button
    process_range_button_tick(now_tick, result);

    // Delayed actions
    process_delayed_actions(result, now_tick);
}
