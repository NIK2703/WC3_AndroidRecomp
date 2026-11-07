// Auto-repeat processing for button elements and gesture bindings.
// Handles: process_auto_repeat_burst, process_non_toggle_press_release,
//          process_button_auto_repeat.

#include "core/internal.h"

extern TouchProcessorState g_state;

void process_auto_repeat_burst(TouchActionResult* restrict result, TouchBinding* actions, int count, uint64_t* last_time, uint64_t now_tick) {
    if (count == 0) return;
    for (int i = 0; i < count; i++) {
        TouchBinding* b = &actions[i];
        if (!(b->flags & TOUCH_BINDING_FLAG_AUTO_REPEAT)) continue;
        if (b->flags & TOUCH_BINDING_FLAG_TOGGLE) continue;
        int interval_ticks = b->auto_repeat_interval_ticks;
        if (interval_ticks <= 0) continue;

        uint64_t last = last_time[i];
        if (last == 0 || now_tick - last >= (uint64_t)interval_ticks) {
            press_binding(result, b, false);
            last_time[i] = now_tick;
        }
    }
}

void process_non_toggle_press_release(TouchElement* e, uint64_t now_tick, TouchActionResult* restrict result) {
    int interval_ticks = e->cached_auto_repeat_interval_ticks;
    if (interval_ticks <= 0) return;
    if (e->auto_repeat_last_tick == 0 || now_tick - e->auto_repeat_last_tick >= (uint64_t)interval_ticks) {
        e->flags ^= TOUCH_BIT_AUTO_REPEAT_PRIMARY_PRESSED;
        if (e->bindings[0].type != NONE) {
            if (e->flags & TOUCH_BIT_AUTO_REPEAT_PRIMARY_PRESSED)
                press_binding(result, &e->bindings[0], true);
            else
                release_binding(result, &e->bindings[0]);
        }
        e->auto_repeat_last_tick = now_tick;
    }
}

void process_button_auto_repeat(TouchElement* e, uint64_t now_tick, TouchActionResult* restrict result) {
    if (!(e->flags & TOUCH_BIT_CACHED_HAS_AUTO_REPEAT)) return;
    bool guard = false;
    if (e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE) {
        guard = element_is_toggle_active(e);
    } else {
        guard = (e->current_ptr_id >= 0 && (e->flags & TOUCH_BIT_ENGAGED));
    }
    if (!guard) return;
    if (e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE) {
        int interval_ticks = e->cached_auto_repeat_interval_ticks;
        if (e->auto_repeat_last_tick == 0 || now_tick - e->auto_repeat_last_tick >= (uint64_t)interval_ticks) {
            e->flags ^= TOUCH_BIT_AUTO_REPEAT_PRIMARY_PRESSED;
            for (int k = 0; k < MAX_BINDINGS_PER_ELEMENT; k++) {
                TouchBinding* tb = &e->bindings[k];
                if (tb->type == NONE) continue;
                if (!(tb->flags & TOUCH_BINDING_FLAG_AUTO_REPEAT)) continue;
                if (e->flags & TOUCH_BIT_AUTO_REPEAT_PRIMARY_PRESSED)
                    press_binding(result, tb, true);
                else
                    release_binding(result, tb);
            }
            e->auto_repeat_last_tick = now_tick;
        }
    } else {
        process_non_toggle_press_release(e, now_tick, result);
    }
}
