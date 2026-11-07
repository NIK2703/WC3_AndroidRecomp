// Touch processor config update at runtime.
// Handles: touch_processor_update_config.

#include "core/internal.h"

extern TouchProcessorState g_state;

void touch_processor_update_config(const TouchProcessorConfig* config) {
    if (!config) { return; }
    TP_STATE_LOCK();
    ELEMENTS_LOCK();
    uint32_t old_gen = g_state.cfg.bindings_generation;
    memcpy(&g_state.cfg, config, sizeof(TouchProcessorConfig));
    g_state.cfg.bindings_generation = old_gen + 1;
    if (g_state.cfg.bindings_generation == 0) g_state.cfg.bindings_generation = 1;
    // If profile specifies a tick interval, apply it immediately.
    if (g_state.cfg.timer_interval_ms > 0) {
        g_timer_interval_ms = g_state.cfg.timer_interval_ms;
    }
    compute_gesture_caps(&g_state.cfg);

    gesture_clear_second_finger_state();
    g_state.gesture_second_active = false;
    g_state.gesture_second_ptr_id = -1;
    g_state.gesture_double_tap_consumed = false;
    g_state.gesture_deferred_second_finger_tap = false;
    g_state.gesture_main_ptr_id = -1;
    g_state.dt_waiting_count = 0;
    gesture_clear_deferred_tap();
    {
        TouchActionResult release_result; release_result.count = 0;
        release_held_actions(&release_result, 0);
    }

    for (int i = 0; i < MAX_FINGERS; i++) {
        TouchFinger* f = &g_state.fingers[i];
        if (!f->active) continue;
        if (f->is_second_finger) {
            setup_second_finger_bindings(f);
        } else {
            FingerBindings* fb = &f->bindings;
            setup_main_finger_bindings(fb);
        }
        f->bindings_generation = g_state.cfg.bindings_generation;
        touch_finger_cache_bindings_state(f);
    }

    for (int i = 0; i < g_state.element_count; i++)
        mark_element_dirty(&g_state.elements[i]);
    ELEMENTS_UNLOCK();
    TP_STATE_UNLOCK();
}
