#ifndef TOUCH_FINGER_H
#define TOUCH_FINGER_H

#include <stdint.h>
#include <stdbool.h>
#include "bind.h"
#include "gesture_type.h"

#define MAX_FINGERS 8

typedef enum {
    GESTURE_STATE_IDLE,
    GESTURE_STATE_TAP_WAITING,
    GESTURE_STATE_DOUBLE_TAP_WAITING,
    GESTURE_STATE_LONG_PRESSING,
    GESTURE_STATE_DRAGGING
} GestureState;

typedef struct {
    const TouchBinding* single_tap;
    const TouchBinding* long_press;
    const TouchBinding* double_tap;
    const TouchBinding* single_tap_drag;
    const TouchBinding* long_press_drag;
    const TouchBinding* double_tap_drag;
    int single_tap_count;
    int long_press_count;
    int double_tap_count;
    int single_tap_drag_count;
    int long_press_drag_count;
    int double_tap_drag_count;
} FingerBindings;

typedef struct {
    int16_t ptr_id;
    float x, y;
    float down_x, down_y;
    uint64_t down_tick;

    float last_x, last_y;
    float sub_pixel_x, sub_pixel_y;
    float travel_x, travel_y;
    uint32_t active : 1;
    uint32_t cached_has_non_passthrough_engaged : 1;
    uint32_t cached_has_active_single_tap : 1;
    uint32_t cached_has_active_double_tap : 1;
    uint32_t cached_has_active_long_press : 1;
    uint32_t cached_has_active_single_tap_drag : 1;
    uint32_t cached_has_active_long_press_drag : 1;
    uint32_t cached_has_active_double_tap_drag : 1;
    uint32_t cached_has_long_press_timer : 1;
    uint32_t cached_has_moved_beyond_threshold : 1;
    uint32_t is_tap : 1;
    uint32_t scroll_mode : 1;
    uint32_t is_second_finger : 1;
    uint32_t double_tap_original_id_set : 1;
    uint32_t gesture_activated_in_touch : 1;
    uint32_t pending_scroll_mode : 1;
    uint8_t state;
    FingerBindings bindings;
    float tap_up_x, tap_up_y;
    uint32_t bindings_generation;

    int16_t engaged_elem_indices[MAX_HELD_ACTIONS];
    uint8_t engaged_elem_count;

    TouchBinding pending_resume_action[MAX_DEFERRED_BINDINGS];
    uint8_t pending_resume_action_count;
    int16_t original_ptr_id;

    int16_t single_tap_hold_delay_ticks;
    uint64_t single_tap_hold_timer_tick;

    float scroll_origin_x;
    float scroll_origin_y;
    float scroll_hold_origin_x;
    float scroll_hold_origin_y;
    float scroll_accum[SCROLL_DIR_COUNT];

    int8_t scroll_gesture_index;

    int8_t scroll_hold_v_state;
    int8_t scroll_hold_h_state;

    int8_t scroll_normal_held_dir;

    float pending_scroll_origin_x;
    float pending_scroll_origin_y;
    float pending_scroll_hold_origin_x;
    float pending_scroll_hold_origin_y;
    float pending_scroll_accum[SCROLL_DIR_COUNT];

    int8_t pending_scroll_gesture_index;
    int8_t pending_scroll_hold_v_state;
    int8_t pending_scroll_hold_h_state;
    int8_t pending_scroll_normal_held_dir;
} TouchFinger;

#endif
