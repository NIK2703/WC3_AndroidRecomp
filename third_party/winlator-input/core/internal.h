#ifndef TOUCH_PROCESSOR_INTERNAL_H
#define TOUCH_PROCESSOR_INTERNAL_H

// Umbrella header — includes all sub-headers and remaining declarations.
// Files that previously included only touch_processor_internal.h get
// everything they need with zero include path changes.
#include "core/state.h"
#include "core/helpers.h"
#include "core/scroll.h"
#include "core/timer.h"
#include "core/timer_registry.h"
#include "gesture/types.h"

// Config helpers
void compute_gesture_caps(TouchProcessorConfig* cfg);

// Auto-repeat (auto_repeat.c)
void process_auto_repeat_burst(TouchActionResult* restrict result, TouchBinding* actions, int count, uint64_t* last_time, uint64_t now_tick);
void process_non_toggle_press_release(TouchElement* e, uint64_t now_tick, TouchActionResult* restrict result);
void process_button_auto_repeat(TouchElement* e, uint64_t now_tick, TouchActionResult* restrict result);

// Long-press (long_press.c)
void process_button_long_press(TouchElement* e, uint64_t now_tick, TouchActionResult* restrict result, TouchFinger* f);
void process_button_gesture_timer(TouchElement* e, uint64_t now_tick, TouchActionResult* restrict result, TouchFinger* f);

// Range button tick (range_tick.c)
void process_range_button_tick(uint64_t now_tick, TouchActionResult* restrict result);

// Mouse move + delayed actions (mouse_move_tick.c)
void process_delayed_actions(TouchActionResult* restrict result, uint64_t now_tick);

// Scheduled actions
void process_scheduled_actions(TouchActionResult* restrict result, uint64_t now_tick);

// Held action release (executor.c)
void release_held_actions(TouchActionResult* restrict result, uint64_t now_tick);

#endif // TOUCH_PROCESSOR_INTERNAL_H
