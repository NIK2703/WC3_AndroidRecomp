// Range button tick processing.
// Handles: process_range_button_tick.

#include "core/internal.h"

extern TouchProcessorState g_state;

void process_range_button_tick(uint64_t now_tick, TouchActionResult* restrict result) {
    if (g_state.range_count == 0) {
        /* No range elements can be active.  Clear the latch so the host does
         * not keep waking for a state the tick can no longer process. */
        g_state.has_active_ranges = false;
        return;
    }
    if (__builtin_expect(!g_state.has_active_ranges, 0)) return;

    bool any_active = false;
    int range_cnt = g_state.range_count;
    for (int _ri = 0; _ri < range_cnt; _ri++) {
        TouchElement* e = &g_state.elements[g_state.range_indices[_ri]];
        if (__builtin_expect(e->current_ptr_id >= 0, 0) && !e->range_hold_pressed && !e->range_scrolling && e->range_has_binding) {
            if (now_tick - e->down_tick >= (uint64_t)g_state.cfg.long_press_delay_ticks) {
                int kc = e->range_initial_kc;
                if (kc > 0) {
                    add_action(result, ACT_KEY_PRESS, kc, 1, 0);
                    e->range_hold_pressed = true;
                }
            }
        }

        if (__builtin_expect(e->range_pending_tap_release, 0) && now_tick >= e->range_tap_release_tick) {
            int kc = e->range_initial_kc;
            if (kc > 0)
                add_action(result, ACT_KEY_RELEASE, kc, 0, 0);
            e->range_pending_tap_release = false;
            e->range_scrolling = false;
            e->range_has_binding = false;
            e->range_hold_pressed = false;
        }

        if (e->current_ptr_id >= 0 || e->range_pending_tap_release)
            any_active = true;
    }
    if (!any_active) g_state.has_active_ranges = false;
}
