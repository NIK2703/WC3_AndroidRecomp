#ifndef TOUCH_CONFIG_H
#define TOUCH_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "bind.h"
#include "gesture_type.h"
#include "mouse_mode.h"

typedef struct {
    TouchBinding arr[MAX_DEFERRED_BINDINGS];
    uint8_t count;
} GestureBindingSlot;

typedef struct {
    GestureBindingSlot dirs[SCROLL_DIR_COUNT];
} ScrollGestureBindings;

typedef struct {
    const TouchBinding* single_tap;         int single_tap_count;
    const TouchBinding* long_press;         int long_press_count;
    const TouchBinding* double_tap;         int double_tap_count;
    const TouchBinding* single_tap_drag;    int single_tap_drag_count;
    const TouchBinding* long_press_drag;    int long_press_drag_count;
    const TouchBinding* double_tap_drag;    int double_tap_drag_count;
    const TouchBinding* single_2nd;         int single_2nd_count;
    const TouchBinding* double_2nd;         int double_2nd_count;
    const TouchBinding* single_drag_2nd;    int single_drag_2nd_count;
    const TouchBinding* double_drag_2nd;    int double_drag_2nd_count;
} GestureModeBindings;

typedef struct {
    MouseMode touch_mode;
    int long_press_timeout_ticks;
    int double_tap_timeout_ticks;
    int drag_threshold_px;
    int gesture_threshold_px;
    int double_tap_distance_px;
    int binding_delay_ticks;
    int long_press_delay_ticks;

    // --- Phantom delays promoted to profile-driven fields (was hardcoded) ---
    // Tick-based (converted from ms via ms_to_ticks like other delays).
    int inter_bind_min_gap_ticks;     // was executor.c hard-coded +1 tick spacing
    int bind_press_width_ticks;       // was executor.c hard-coded +1 tick PRESS->RELEASE width
    int release_hold_min_gap_ticks;   // was executor.c hard-coded +1 tick in release_held_actions
    int range_tap_release_ticks;      // was range_button.c hard-coded now_tick+2
    int sim_click_press_ticks;        // was handler.c/mouse_move_tick.c hard-coded now_tick+3
    int sim_click_release_ticks;      // was mouse_move_tick.c hard-coded now_tick+3
    int gesture_swipe_min_ticks;      // was long_press.c hard-coded >=3 tick swipe trigger
    int toggle_debounce_ticks;        // was button.c TOGGLE_DEBOUNCE_MS (now in ticks, not ms)
    int idle_debounce_threshold;      // was timer.c IDLE_DEBOUNCE_THRESHOLD 3
    int default_auto_repeat_interval_ticks; // was elements_setup/sync_dispatch DEFAULT_AUTO_REPEAT_INTERVAL_TICKS 6
    // Pixel-based (straight from profile, no ticks conversion).
    int range_scroll_threshold_px;    // was range_button.c RANGE_SCROLL_THRESHOLD 10
    int gesture_threshold_px_fallback; // was button.c GESTURE_THRESHOLD_FALLBACK 20
    int cursor_accel_threshold;       // was init.c DEFAULT_CURSOR_ACCEL_THRESHOLD 6
    float cursor_accel_factor;        // was init.c DEFAULT_CURSOR_ACCEL_FACTOR 1.25f
    int idle_interval_ms;             // was timer.c idle_interval_ms 100
    int timer_interval_ms;            // 0 = use global (from Java tick interval)
    int cursor_speed;
    int cursor_acceleration_threshold;
    float cursor_acceleration_factor;
    int screen_w, screen_h;
    float xform_scale_x;
    float xform_scale_y;
    float view_offset_x;
    float view_offset_y;
    int gesture_long_press_haptic;
    int scroll_bind_haptic;
    int scroll_hold_haptic;
    GestureBindingSlot gestures[GESTURE_TYPE_COUNT];
    uint32_t bindings_generation;
    ScrollGestureBindings scroll_bindings[GESTURE_SLOT_COUNT];
    int scroll_threshold_px;
    int scroll_hold_threshold_px;
    bool scroll_hold_v[GESTURE_SLOT_COUNT];
    bool scroll_hold_h[GESTURE_SLOT_COUNT];
    uint32_t color_primary;
    uint32_t color_secondary;
    float stroke_width_default;
    int fill_alpha_inactive_default;
    bool caps_has_gesture_bindings;
    bool caps_has_non_none_bindings;
    bool caps_has_drag_bindings;
    bool caps_has_double_tap;
    bool caps_has_long_press;
    bool caps_has_long_press_timer;
    bool caps_has_track_hover_buttons;
    uint32_t caps_mode_mask;
    bool is_ts;
    bool is_tp;
    uint32_t caps_second_mask;
    bool caps_has_element_toggle;
    bool caps_has_any_element_long_press;
    bool caps_has_any_element_gesture;
    bool caps_has_auto_repeat_buttons;
    bool caps_has_mouse_left_element;
    bool caps_has_passthrough_elements;
    bool caps_has_scroll_bindings;
    GestureModeBindings cached_mode_bindings;
} TouchProcessorConfig;

#endif
