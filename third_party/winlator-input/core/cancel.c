// Full cancel: releases ALL held keys, element bindings, and clears all state.

#include "core/internal.h"
#include "core/activation_mode.h"

extern TouchProcessorState g_state;

void touch_processor_cancel_all(TouchActionResult* restrict result) {
    wc3_timer_registry_invalidate();
    result->count = 0;

    ELEMENTS_LOCK();

    memset(g_ctx, 0, sizeof(g_ctx));

    g_state.mouse_move_active = false;
    g_state.mouse_move_offset_x = 0.0f;
    g_state.mouse_move_offset_y = 0.0f;
    g_state.mouse_move_cursor_speed = 0.0f;
    g_state.mouse_move_dx = 0;
    g_state.mouse_move_dy = 0;
    g_state.mouse_offset_active_x = false;
    g_state.mouse_offset_active_y = false;

    release_held_actions(result, 0);

    for (int i = 0; i < g_state.gesture_toggled_count; i++)
        release_binding(result, &g_state.gesture_toggled_actions[i]);
    g_state.gesture_toggled_count = 0;
    g_state.gesture_is_action_held = false;
    g_state.gesture_held_count = 0;

    for (int i = 0; i < g_state.element_count; i++) {
        TouchElement* e = &g_state.elements[i];
        if (e->current_ptr_id >= 0 || (e->flags & TOUCH_BIT_ENGAGED)
            || (e->flags & TOUCH_BIT_SELECTED) || (e->flags & TOUCH_BIT_GESTURE_TOGGLED) || (e->flags & TOUCH_BIT_LP_TOGGLED)) {
            release_element_bindings(e, result, true);
            force_release_element_toggles(e, result);
        }
        element_reset_runtime(e);
        mark_element_dirty(e);
    }

    for (int i = 0; i < MAX_FINGERS; i++) {
        if (g_state.fingers[i].active) {
            scroll_mode_exit(&g_state.fingers[i], result);
            deactivate_finger(&g_state.fingers[i]);
        }
    }
    g_state.active_finger_count = 0;
    g_state.free_finger_hint = 0;

    g_state.main_ptr_id = -1;
    g_state.gesture_main_ptr_id = -1;
    g_state.finger_pointer_left = -1;
    g_state.finger_pointer_right = -1;
    g_state.sim_click_ptr_id = -1;
    g_state.sim_click_press_tick = 0;
    g_state.sim_click_release_tick = 0;
    g_state.sim_continue_click = false;
    g_state.pointer_left_enabled = true;

    g_state.passthrough_active = false;
    g_state.scroll_accum_y = 0;
    g_state.scrolling = false;
    g_state.pointer_right_enabled = true;

    g_state.dt_waiting_count = 0;

    for (int i = 0; i < g_state.scheduled_action_count; i++)
        g_state.scheduled_actions[i].active = false;
    g_state.scheduled_action_count = 0;

    gesture_clear_deferred_tap();
    gesture_clear_second_finger_globals();
    gesture_clear_second_finger_state();

    activation_reset();

    ELEMENTS_UNLOCK();
}
