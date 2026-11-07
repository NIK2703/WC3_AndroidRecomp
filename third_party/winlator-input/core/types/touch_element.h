#ifndef TOUCH_ELEMENT_H
#define TOUCH_ELEMENT_H

#include <stdint.h>
#include <stdbool.h>
#include "bind.h"
#include "element_type.h"
#include "touch_activation_mode.h"

#define MAX_ELEMENTS 128

#define TOUCH_BIT_ENGAGED               (1u << 0)
#define TOUCH_BIT_SELECTED              (1u << 1)
#define TOUCH_BIT_PASSTHROUGH_TOUCH     (1u << 2)
#define TOUCH_BIT_GESTURE_SUPPRESSED    (1u << 3)
#define TOUCH_BIT_CACHED_HAS_TOGGLE     (1u << 4)
#define TOUCH_BIT_CACHED_HAS_AUTO_REPEAT (1u << 5)
#define TOUCH_BIT_CACHED_HAS_LONG_PRESS (1u << 6)
#define TOUCH_BIT_CACHED_HAS_GESTURE    (1u << 7)
#define TOUCH_BIT_CACHED_HAS_ANY_BINDING (1u << 8)
#define TOUCH_BIT_IS_LOCK               (1u << 9)
#define TOUCH_BIT_TOGGLE_REPRESS_KILLS  (1u << 10)
#define TOUCH_BIT_TOGGLE_SLIDE_OVER     (1u << 11)
#define TOUCH_BIT_IS_HOVER              (1u << 12)
#define TOUCH_BIT_LONG_PRESS_ARM        (1u << 13)
#define TOUCH_BIT_GESTURE_SWIPE_TRIGGERED (1u << 14)
#define TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED (1u << 15)
#define TOUCH_BIT_GESTURE_TIMER_ARMED   (1u << 16)
#define TOUCH_BIT_LP_TOGGLED            (1u << 17)
#define TOUCH_BIT_GESTURE_TOGGLED       (1u << 18)
#define ELEM_TOGGLE_BITS_MASK           (TOUCH_BIT_LP_TOGGLED | TOUCH_BIT_GESTURE_TOGGLED)
#define TOUCH_BIT_DEFER_PRIMARY         (1u << 19)
#define TOUCH_BIT_AUTO_REPEAT_PRIMARY_PRESSED (1u << 20)
#define TOUCH_BIT_CACHED_LP_HAS_TOGGLE  (1u << 21)
#define TOUCH_BIT_CACHED_GESTURE_HAS_TOGGLE (1u << 22)
#define TOUCH_BIT_CACHED_BIND0_IS_GAMEPAD (1u << 23)
#define TOUCH_BIT_CACHED_BIND0_IS_RIGHT_STICK (1u << 24)

#define VISUAL_LAYER_FILL            (1u << 0)
#define VISUAL_LAYER_STROKE_TEXT     (1u << 1)
#define VISUAL_LAYER_GLOW            (1u << 2)
#define VISUAL_LAYER_OUTER_STROKE    (1u << 3)
#define VISUAL_LAYER_TEXT_KNOCKOUT   (1u << 4)

#define VF_TAP       (1u << 0)
#define VF_LONG_TAP  (1u << 1)
#define VF_GESTURE   (1u << 2)

typedef struct {
    // === CACHE LINE 0 (64 bytes): HOT — every touch event ===
    float cached_left;
    float cached_right;
    float cached_top;
    float cached_bottom;
    float cached_hw_sq;
    float visual_x;
    float visual_y;
    float hw;
    float hh;
    float scale;
    int x;
    int y;
    uint32_t flags;
    int16_t current_ptr_id;
    uint8_t type;
    uint8_t shape;
    uint32_t visual_layers;
    uint32_t visual_alpha;

    // === CACHE LINE 1 (64 bytes): WARM — dispatch + gesture state ===
    float down_x;
    float down_y;
    uint64_t down_tick;
    uint8_t cached_petal_mask;
    uint8_t visual_flags;
    uint16_t last_visual_key;
    TouchBinding bindings[4];
    int16_t cached_auto_repeat_interval_ticks;

    // === CACHE LINE 2 (64 bytes): WARM — stick/trackpad continuous state ===
    int primary_sticky_mask;
    float stick_value_x;
    float stick_value_y;
    float trackpad_last_x;
    float trackpad_last_y;
    float trackpad_vel_x;
    float trackpad_vel_y;
    uint64_t trackpad_last_tick;
    uint8_t activation_mode;
    float local_left;
    float local_top;
    float local_right;
    float local_bottom;
    float w;
    float h;
    bool petal_active[MAX_PETALS];

    // === CACHE LINE 3+ (192+): COLD ===
    int button_long_press_haptic;
    int button_gesture_haptic;
    uint64_t auto_repeat_last_tick;
    uint64_t toggle_debounce_last_tick;
    int range_ordinal;
    int range_index;
    int range_max;
    int range_binding_count;
    int range_orientation;
    float range_scroll_offset;
    float range_current_offset;
    float range_last_position;
    bool range_scrolling;
    bool range_has_binding;
    bool range_hold_pressed;
    bool range_pending_tap_release;
    uint64_t range_tap_release_tick;
    int range_initial_kc;
    int32_t range_pressed_index;
    bool range_finger_down;
    float cached_range_cw;
    float cached_range_ch;
    float cached_range_element_size;
    float opacity;
    float corner_radius;
    float dpad_corner_radius;
    uint32_t color_primary;
    uint32_t color_secondary;
    float stroke_width;
    int fill_alpha_inactive;
    char display_text[32];
    int8_t icon_id;
} __attribute__((aligned(64))) TouchElement;

#endif
