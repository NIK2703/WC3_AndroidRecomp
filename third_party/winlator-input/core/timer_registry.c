// Central timer registry for the persistent touch-engine state machine.
//
// The engine keeps timer fields in several existing structures.  This module
// caches one absolute deadline per logical timer group and only re-evaluates a
// group after a public engine entry point invalidates the cache.  The
// host-facing query therefore walks a fixed eight-entry table instead of all
// elements, while still deriving every deadline from the same state that
// touch_processor_tick() consumes.

#include "core/internal.h"

extern TouchProcessorState g_state;

#define TIMER_REGISTRY_ALL_DIRTY \
    ((1u << (unsigned)WC3_TIMER_GROUP_COUNT) - 1u)

static uint64_t g_group_deadline[WC3_TIMER_GROUP_COUNT];
static uint32_t g_dirty_groups = TIMER_REGISTRY_ALL_DIRTY;

static inline uint64_t deadline_min(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

/*
 * The timer code casts intervals to uint64_t before comparing.  A negative
 * interval therefore never expires; report no deadline instead of turning it
 * into a permanent "due now" wakeup.
 */
static inline uint64_t deadline_after(uint64_t start, int delay) {
    if (delay < 0) return TOUCH_PROCESSOR_DEADLINE_NONE;
    return start + (uint64_t)delay;
}

static inline uint64_t button_repeat_deadline(uint64_t last, int interval,
                                              bool toggle) {
    if (interval < 0)
        return last == 0 ? 0 : TOUCH_PROCESSOR_DEADLINE_NONE;
    if (interval == 0)
        return toggle ? (last == 0 ? 0 : last + 1)
                      : TOUCH_PROCESSOR_DEADLINE_NONE;
    return last == 0 ? 0 : last + (uint64_t)interval;
}

static inline uint64_t gesture_repeat_deadline(uint64_t last, int interval) {
    if (interval <= 0) return TOUCH_PROCESSOR_DEADLINE_NONE;
    return last == 0 ? 0 : last + (uint64_t)interval;
}

static inline bool binding_has_auto_repeat(const TouchBinding* b) {
    return b->type != NONE
        && (b->flags & TOUCH_BINDING_FLAG_AUTO_REPEAT)
        && !(b->flags & TOUCH_BINDING_FLAG_TOGGLE)
        && b->auto_repeat_interval_ticks > 0;
}

static uint64_t compute_scheduled(void) {
    int count = g_state.scheduled_action_count;
    if (count <= 0) return TOUCH_PROCESSOR_DEADLINE_NONE;
    if (count > MAX_SCHEDULED_ACTIONS) count = MAX_SCHEDULED_ACTIONS;

    uint64_t first = TOUCH_PROCESSOR_DEADLINE_NONE;
    bool any_active = false;
    for (int i = 0; i < count; i++) {
        const ScheduledAction* sa = &g_state.scheduled_actions[i];
        if (!sa->active) continue;
        any_active = true;
        first = deadline_min(first, sa->scheduled_tick);
    }

    // The tick compacts an all-inactive array.  One immediate tick makes the
    // held-action fallback see count 0 without polling thereafter.
    return any_active ? first : 0;
}

static uint64_t compute_delayed(void) {
    uint64_t best = TOUCH_PROCESSOR_DEADLINE_NONE;
    if (g_state.sim_click_press_tick != 0)
        best = deadline_min(best, g_state.sim_click_press_tick);
    if (g_state.sim_click_release_tick != 0)
        best = deadline_min(best, g_state.sim_click_release_tick);
    return best;
}

static uint64_t compute_gesture(void) {
    return gesture_next_deadline_tick();
}

static uint64_t compute_button(void) {
    bool has_ar = g_state.cfg.caps_has_auto_repeat_buttons;
    bool has_lp = g_state.cfg.caps_has_any_element_long_press;
    bool has_gest = g_state.cfg.caps_has_any_element_gesture;
    int button_count = g_state.button_count;
    if (button_count <= 0 || !(has_ar || has_lp || has_gest))
        return TOUCH_PROCESSOR_DEADLINE_NONE;

    uint64_t best = TOUCH_PROCESSOR_DEADLINE_NONE;
    TouchElement* elements = g_state.elements;
    for (int i = 0; i < button_count; i++) {
        int idx = g_state.button_indices[i];
        TouchElement* e = &elements[idx];

        if ((e->flags & TOUCH_BIT_ENGAGED) && e->current_ptr_id < 0) {
            best = 0;
        }

        if (has_ar && (e->flags & TOUCH_BIT_CACHED_HAS_AUTO_REPEAT)) {
            bool toggle = (e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE) != 0;
            bool guard = toggle
                ? element_is_toggle_active(e)
                : (e->current_ptr_id >= 0 &&
                   (e->flags & TOUCH_BIT_ENGAGED) != 0);
            if (guard) {
                best = deadline_min(best,
                    button_repeat_deadline(e->auto_repeat_last_tick,
                        e->cached_auto_repeat_interval_ticks, toggle));
            }
        }

        if (has_lp && (e->flags & TOUCH_BIT_CACHED_HAS_LONG_PRESS)
            && e->current_ptr_id >= 0
            && (e->flags & TOUCH_BIT_LONG_PRESS_ARM)
            && !(e->flags & TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED)
            && !finger_gesture_active(e)
            && ELEM_LP_COUNT(idx) > 0
            && ELEM_LP(idx)[0].type != NONE) {
            best = deadline_min(best,
                deadline_after(e->down_tick,
                    g_state.cfg.long_press_delay_ticks));
        }

        if (has_gest && (e->flags & TOUCH_BIT_CACHED_HAS_GESTURE)
            && e->current_ptr_id >= 0
            && (e->flags & TOUCH_BIT_GESTURE_TIMER_ARMED)
            && !(e->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED)
            && !(e->flags & TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED)
            && !finger_gesture_active(e)
            && ELEM_GEST_COUNT(idx) > 0
            && ELEM_GEST(idx)[0].type != NONE) {
            best = deadline_min(best,
                deadline_after(e->down_tick,
                    g_state.cfg.gesture_swipe_min_ticks));
        }
    }
    return best;
}

static uint64_t compute_range(void) {
    if (!g_state.has_active_ranges)
        return TOUCH_PROCESSOR_DEADLINE_NONE;

    int range_count = g_state.range_count;
    if (range_count == 0) {
        // Latch with no range elements: one tick clears it.
        return 0;
    }

    uint64_t best = TOUCH_PROCESSOR_DEADLINE_NONE;
    bool any_active = false;
    TouchElement* elements = g_state.elements;
    for (int i = 0; i < range_count; i++) {
        TouchElement* e = &elements[g_state.range_indices[i]];
        if (e->current_ptr_id < 0 && !e->range_pending_tap_release)
            continue;

        any_active = true;

        if (e->current_ptr_id >= 0
            && !e->range_hold_pressed
            && !e->range_scrolling
            && e->range_has_binding
            && e->range_initial_kc > 0) {
            best = deadline_min(best,
                deadline_after(e->down_tick,
                    g_state.cfg.long_press_delay_ticks));
        }

        if (e->range_pending_tap_release) {
            best = deadline_min(best, e->range_tap_release_tick);
        }
    }

    // A stale latch with no active element is cleared by one tick.
    if (!any_active) best = 0;
    return best;
}

static uint64_t compute_mouse(void) {
    if (!g_state.mouse_move_active
        && g_state.mouse_move_offset_x == 0.0f
        && g_state.mouse_move_offset_y == 0.0f) {
        return TOUCH_PROCESSOR_DEADLINE_NONE;
    }

    // Continuous movement is intentionally periodic: the next engine tick
    // advances the cursor.  Using last+1 also guarantees progress when the
    // first observed tick was tick zero.
    uint64_t last = g_state.last_tick_time;
    return last == UINT64_MAX ? last : last + 1;
}

static uint64_t compute_gesture_repeat(void) {
    uint64_t best = TOUCH_PROCESSOR_DEADLINE_NONE;

    for (int i = 0; i < g_state.gesture_toggled_count; i++) {
        const TouchBinding* b = &g_state.gesture_toggled_actions[i];
        if (!binding_has_auto_repeat(b)) continue;
        best = deadline_min(best,
            gesture_repeat_deadline(g_state.gesture_auto_repeat_last_tick[i],
                b->auto_repeat_interval_ticks));
    }

    if (g_state.gesture_is_action_held) {
        for (int i = 0; i < g_state.gesture_held_count; i++) {
            const TouchBinding* b = &g_state.gesture_held_actions[i];
            if (!binding_has_auto_repeat(b)) continue;
            best = deadline_min(best,
                gesture_repeat_deadline(
                    g_state.gesture_auto_repeat_last_tick_held[i],
                    b->auto_repeat_interval_ticks));
        }
    }
    return best;
}

static uint64_t compute_fallback(void) {
    if (g_state.scheduled_action_count == 0
        && g_state.gesture_is_action_held
        && g_state.active_finger_count == 0) {
        return 0;
    }
    return TOUCH_PROCESSOR_DEADLINE_NONE;
}

static uint64_t compute_group(int group) {
    switch (group) {
        case WC3_TIMER_GROUP_SCHEDULED:       return compute_scheduled();
        case WC3_TIMER_GROUP_DELAYED:         return compute_delayed();
        case WC3_TIMER_GROUP_GESTURE:         return compute_gesture();
        case WC3_TIMER_GROUP_BUTTON:          return compute_button();
        case WC3_TIMER_GROUP_RANGE:           return compute_range();
        case WC3_TIMER_GROUP_MOUSE:           return compute_mouse();
        case WC3_TIMER_GROUP_GESTURE_REPEAT:  return compute_gesture_repeat();
        case WC3_TIMER_GROUP_FALLBACK:        return compute_fallback();
        default:                              return TOUCH_PROCESSOR_DEADLINE_NONE;
    }
}

void wc3_timer_registry_invalidate(void) {
    g_dirty_groups = TIMER_REGISTRY_ALL_DIRTY;
}

uint64_t wc3_timer_registry_next_deadline(void) {
    uint32_t dirty = g_dirty_groups;
    if (dirty) {
        g_dirty_groups = 0;
        for (int i = 0; i < WC3_TIMER_GROUP_COUNT; i++) {
            if (dirty & (1u << (unsigned)i))
                g_group_deadline[i] = compute_group(i);
        }
    }

    uint64_t best = TOUCH_PROCESSOR_DEADLINE_NONE;
    for (int i = 0; i < WC3_TIMER_GROUP_COUNT; i++) {
        best = deadline_min(best, g_group_deadline[i]);
    }
    return best;
}

uint64_t touch_processor_next_deadline_tick(void) {
    return wc3_timer_registry_next_deadline();
}

int touch_processor_needs_tick(void) {
    return touch_processor_next_deadline_tick() !=
           TOUCH_PROCESSOR_DEADLINE_NONE;
}
