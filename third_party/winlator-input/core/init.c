// Touch processor initialization.
// Handles: compute_gesture_caps, touch_processor_init.

#include "core/internal.h"
#include <stddef.h>

TouchProcessorState g_state;

// Processor state (singleton).
// Config values are fully populated by profile_to_config; no fallback macros needed.

void compute_gesture_caps(TouchProcessorConfig* cfg) {
    cfg->caps_mode_mask = 0;

    for (int g = 0; g < GESTURE_TYPE_COUNT; g++) {
        if (cfg->gestures[g].count > 0) {
            bool has_non_none = false;
            for (int i = 0; i < cfg->gestures[g].count; i++) {
                if (cfg->gestures[g].arr[i].type != NONE) {
                    has_non_none = true;
                    break;
                }
            }
            if (has_non_none) cfg->caps_mode_mask |= GESTURE_MASK(g);
        }
    }

    cfg->caps_has_gesture_bindings = cfg->caps_mode_mask != 0;
    cfg->caps_has_non_none_bindings = cfg->caps_has_gesture_bindings;

    uint32_t drag_mask =
        GESTURE_MASK(SINGLE_TAP_DRAG) | GESTURE_MASK(LONG_PRESS_DRAG) |
        GESTURE_MASK(DOUBLE_TAP_DRAG) | GESTURE_MASK(SINGLE_DRAG_2ND) |
        GESTURE_MASK(DOUBLE_DRAG_2ND);
    cfg->caps_has_drag_bindings = (cfg->caps_mode_mask & drag_mask) != 0;

    uint32_t dt_mask =
        GESTURE_MASK(DOUBLE_TAP) | GESTURE_MASK(DOUBLE_2ND) |
        GESTURE_MASK(DOUBLE_TAP_DRAG) | GESTURE_MASK(DOUBLE_DRAG_2ND);
    cfg->caps_has_double_tap = (cfg->caps_mode_mask & dt_mask) != 0;

    cfg->caps_has_long_press = (cfg->caps_mode_mask & GESTURE_MASK(LONG_PRESS)) != 0;

    cfg->caps_has_long_press_timer =
        (cfg->caps_mode_mask & (GESTURE_MASK(LONG_PRESS) | GESTURE_MASK(LONG_PRESS_DRAG))) != 0;

    cfg->caps_second_mask =
        GESTURE_MASK(SINGLE_2ND) | GESTURE_MASK(DOUBLE_2ND) |
        GESTURE_MASK(SINGLE_DRAG_2ND) | GESTURE_MASK(DOUBLE_DRAG_2ND);

    cfg->is_ts = (cfg->touch_mode == TOUCHSCREEN);
    cfg->is_tp = (cfg->touch_mode == TOUCHPAD);

    cfg->caps_has_scroll_bindings = false;
    for (int s = 0; s < GESTURE_SLOT_COUNT; s++) {
        for (int d = 0; d < SCROLL_DIR_COUNT; d++) {
            if (cfg->scroll_bindings[s].dirs[d].count > 0) {
                cfg->caps_has_scroll_bindings = true;
                break;
            }
        }
        if (cfg->caps_has_scroll_bindings) break;
    }

    {
        const GestureBindingSlot* slots = cfg->gestures;
        cfg->cached_mode_bindings.single_tap        = slots[SINGLE_TAP].arr;        cfg->cached_mode_bindings.single_tap_count        = slots[SINGLE_TAP].count;
        cfg->cached_mode_bindings.long_press        = slots[LONG_PRESS].arr;        cfg->cached_mode_bindings.long_press_count        = slots[LONG_PRESS].count;
        cfg->cached_mode_bindings.double_tap        = slots[DOUBLE_TAP].arr;        cfg->cached_mode_bindings.double_tap_count        = slots[DOUBLE_TAP].count;
        cfg->cached_mode_bindings.single_tap_drag   = slots[SINGLE_TAP_DRAG].arr;   cfg->cached_mode_bindings.single_tap_drag_count   = slots[SINGLE_TAP_DRAG].count;
        cfg->cached_mode_bindings.long_press_drag   = slots[LONG_PRESS_DRAG].arr;   cfg->cached_mode_bindings.long_press_drag_count   = slots[LONG_PRESS_DRAG].count;
        cfg->cached_mode_bindings.double_tap_drag   = slots[DOUBLE_TAP_DRAG].arr;   cfg->cached_mode_bindings.double_tap_drag_count   = slots[DOUBLE_TAP_DRAG].count;
        cfg->cached_mode_bindings.single_2nd        = slots[SINGLE_2ND].arr;        cfg->cached_mode_bindings.single_2nd_count        = slots[SINGLE_2ND].count;
        cfg->cached_mode_bindings.double_2nd        = slots[DOUBLE_2ND].arr;        cfg->cached_mode_bindings.double_2nd_count        = slots[DOUBLE_2ND].count;
        cfg->cached_mode_bindings.single_drag_2nd   = slots[SINGLE_DRAG_2ND].arr;   cfg->cached_mode_bindings.single_drag_2nd_count   = slots[SINGLE_DRAG_2ND].count;
        cfg->cached_mode_bindings.double_drag_2nd   = slots[DOUBLE_DRAG_2ND].arr;   cfg->cached_mode_bindings.double_drag_2nd_count   = slots[DOUBLE_DRAG_2ND].count;
    }
}

void touch_processor_init(const TouchProcessorConfig* config) {
    if (!config) {
        return;
    }
    TP_STATE_LOCK();
    wc3_timer_registry_invalidate();
    pthread_mutex_destroy(&g_state.elements_mutex);
    size_t pre_mutex = offsetof(TouchProcessorState, elements_mutex);
    memset(&g_state, 0, pre_mutex);
    size_t post_mutex = pre_mutex + sizeof(pthread_mutex_t);
    memset((uint8_t*)&g_state + post_mutex, 0, sizeof(g_state) - post_mutex);
    memset(g_ctx, 0, sizeof(g_ctx));
    pthread_mutex_init(&g_state.elements_mutex, NULL);
    memcpy(&g_state.cfg, config, sizeof(TouchProcessorConfig));
    g_state.main_ptr_id = -1;
    g_state.gesture_main_ptr_id = -1;
    g_state.gesture_second_ptr_id = -1;
    g_state.finger_pointer_left = -1;
    g_state.finger_pointer_right = -1;
    g_state.pointer_left_enabled = true;
    g_state.sim_click_ptr_id = -1;
    for (int i = 0; i < MAX_FINGERS; i++) g_state.hovered_element_per_ptr[i] = -1;
    if (g_state.cfg.xform_scale_x <= 0.0f) g_state.cfg.xform_scale_x = 1.0f;
    if (g_state.cfg.xform_scale_y <= 0.0f) g_state.cfg.xform_scale_y = 1.0f;
    g_state.cfg.bindings_generation = 1;
    compute_gesture_caps(&g_state.cfg);
    TP_STATE_UNLOCK();
}
