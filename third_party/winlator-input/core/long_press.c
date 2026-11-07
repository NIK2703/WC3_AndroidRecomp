// Long-press and gesture timer processing for button elements.
// Handles: process_button_long_press, process_button_gesture_timer.

#include "core/internal.h"

extern TouchProcessorState g_state;

void process_button_long_press(TouchElement* e, uint64_t now_tick, TouchActionResult* restrict result, TouchFinger* _lp_f) {
    int _ei = element_index(e);
    if (__builtin_expect(e->current_ptr_id >= 0, 0) && (e->flags & TOUCH_BIT_LONG_PRESS_ARM) && !(e->flags & TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED)
        && ELEM_LP_COUNT(_ei) > 0 && ELEM_LP(_ei)[0].type != NONE
        && !finger_gesture_active(e))
    {
        uint64_t elapsed = now_tick - e->down_tick;
        if (elapsed >= (uint64_t)g_state.cfg.long_press_delay_ticks) {
            e->flags |= TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED;
            if (_lp_f) _lp_f->gesture_activated_in_touch = true;
            vis_set(e, VF_LONG_TAP);
            e->flags &= ~TOUCH_BIT_LONG_PRESS_ARM;
            toggle_alternate_bindings(e, TOUCH_BIT_LP_TOGGLED, (e->flags & TOUCH_BIT_CACHED_LP_HAS_TOGGLE) != 0,
                ELEM_LP(_ei), ELEM_LP_COUNT(_ei),
                e->bindings[0].type != NONE, result);
            finish_visual_update(e);
            if (e->button_long_press_haptic > 0 && ELEM_LP_COUNT(_ei) > 0)
                add_action(result, ACT_HAPTIC, e->button_long_press_haptic, 0, 0);
        }
    }
}

void process_button_gesture_timer(TouchElement* e, uint64_t now_tick, TouchActionResult* restrict result, TouchFinger* _gt_f) {
    int _ei = element_index(e);
    if (__builtin_expect(e->current_ptr_id >= 0, 0) && (e->flags & TOUCH_BIT_GESTURE_TIMER_ARMED) && !(e->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED)
        && !(e->flags & TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED)
        && ELEM_GEST_COUNT(_ei) > 0 && ELEM_GEST(_ei)[0].type != NONE
        && !finger_gesture_active(e))
    {
        if (now_tick - e->down_tick >= (uint64_t)g_state.cfg.gesture_swipe_min_ticks) {
            e->flags |= TOUCH_BIT_GESTURE_SWIPE_TRIGGERED;
            vis_set(e, VF_GESTURE);
            if (_gt_f) _gt_f->gesture_activated_in_touch = true;
            e->flags &= ~TOUCH_BIT_GESTURE_TIMER_ARMED;
            toggle_alternate_bindings(e, TOUCH_BIT_GESTURE_TOGGLED, (e->flags & TOUCH_BIT_CACHED_GESTURE_HAS_TOGGLE) != 0,
                ELEM_GEST(_ei), ELEM_GEST_COUNT(_ei),
                e->bindings[0].type != NONE, result);
            finish_visual_update(e);
            if (e->button_gesture_haptic > 0)
                add_action(result, ACT_HAPTIC, e->button_gesture_haptic, 0, 0);
        }
    }
}
