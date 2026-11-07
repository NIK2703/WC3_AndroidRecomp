#ifndef TP_STATE_H
#define TP_STATE_H

// Core state types and constants for the touch processor.
// This is the foundational header: no function declarations, only types and macros.

#include <pthread.h>
#include "core/processor.h"
#include "gesture/types.h"

#define MAX_TRACKED_PER_POINTER 8
#define TRACKED_BITMASK_WORDS ((MAX_ELEMENTS + 31) / 32)
#define MAX_TAP_TRAVEL 10

// Scheduled actions (non-blocking replacement for delay_ms)
#define MAX_SCHEDULED_ACTIONS 32
#define GRID_COLS 8
#define GRID_ROWS 8
#define DPAD_RADIUS_MULTIPLIER  7.0f
#define STICK_RADIUS_MULTIPLIER 6.0f
#define GAMEPAD_AXIS_MAX 32767
#define RANGE_HEIGHT_MULTIPLIER 2.0f
#define BUTTON_CIRCLE_RADIUS_MULT 3.0f
#define RANGE_BINDING_WIDTH_SNAP 4
#define MAX_BINDINGS_PER_ELEMENT 4
#define NEAR_ZERO_THRESHOLD 0.0001f
#define MAX_POINTER_ID 256
#define DIRTY_MASK_WORDS ((MAX_ELEMENTS + 31) / 32)
#define BITMASK_WORD_BITS 32
#define STICK_DEAD_ZONE 0.15f
#define DPAD_DEAD_ZONE 0.3f
#define STICK_SENSITIVITY 2.0f
#define TRACKPAD_MIN_SPEED 0.8f


typedef struct {
    uint64_t scheduled_tick;
    TouchBinding binding;
    int action_type;
    bool active;
    bool needs_toggle_record;
    int finger_idx;
} ScheduledAction;

typedef struct {
    int16_t ptr_id;
    int16_t element_indices[MAX_TRACKED_PER_POINTER];
    int8_t count;
    uint32_t tracked_bitmask[TRACKED_BITMASK_WORDS];
} TrackedButtons;

typedef struct {
    // === HOT_TICK: contiguous at offset 0 ===
    TouchFinger fingers[MAX_FINGERS] __attribute__((aligned(64)));
    int active_finger_count;
    int element_count;
    float ptr_x, ptr_y;
    float scroll_accum_y;
    int main_ptr_id;

    int gesture_main_ptr_id;
    int gesture_second_ptr_id;
    bool gesture_second_active;
    bool gesture_double_tap_consumed;
    bool gesture_is_down_event;
    int dt_waiting_count;
    float gesture_second_main_ref_x;
    float gesture_second_main_ref_y;
    bool passthrough_active;
    int free_finger_hint;

    int button_count;
    int range_count;
    bool has_active_ranges;
    int16_t button_indices[MAX_ELEMENTS];
    int16_t range_indices[MAX_ELEMENTS];

    TouchActivationMode activation_mode;
    bool is_track;
    bool is_hover;
    TrackedButtons tracked[MAX_FINGERS];
    int hovered_element_per_ptr[MAX_FINGERS];

    bool gesture_deferred_second_finger_tap;
    int last_touch_x, last_touch_y;
    bool scrolling;
    bool pointer_left_enabled;
    bool pointer_right_enabled;
    int finger_pointer_left;
    int finger_pointer_right;

    // === CONFIG (cold-ish) ===
    TouchProcessorConfig cfg;

    // === HIT-TESTING ===
    float grid_cell_w;
    float grid_cell_h;
    float grid_min_x;
    float grid_min_y;
    float grid_max_x;
    float grid_max_y;
    float snapping_size;
    float resolution_scale;
    float grid_inv_cell_w;
    float grid_inv_cell_h;
    int spatial_grid_count[GRID_COLS * GRID_ROWS];
    int grid_cell_to_elems[MAX_ELEMENTS];
    int grid_cell_start[GRID_COLS * GRID_ROWS + 1];

    // === COLD: deferred + scheduled ===
    TouchFinger* finger_by_ptr_id[MAX_POINTER_ID];

    TouchBinding gesture_deferred_tap[FALLBACK_MAX];
    int gesture_deferred_tap_count;
    TouchBinding gesture_pending_double[FALLBACK_MAX];
    int gesture_pending_double_count;
    TouchBinding gesture_pending_deferred_double[FALLBACK_MAX];
    int gesture_pending_deferred_double_count;
    TouchBinding gesture_held_actions[MAX_HELD_ACTIONS];
    int gesture_held_count;
    bool gesture_is_action_held;
    TouchBinding gesture_toggled_actions[MAX_HELD_ACTIONS];
    int gesture_toggled_count;
    uint64_t gesture_auto_repeat_last_tick[MAX_HELD_ACTIONS];
    uint64_t gesture_auto_repeat_last_tick_held[MAX_HELD_ACTIONS];

    uint64_t pending_left_release_tick;
    int pending_left_release_ptr_id;
    uint64_t pending_right_release_tick;
    int pending_right_release_ptr_id;
    bool sim_touch_screen;

    bool sim_continue_click;
    uint64_t sim_click_press_tick;
    int sim_click_ptr_id;
    uint64_t sim_click_release_tick;

    ScheduledAction scheduled_actions[MAX_SCHEDULED_ACTIONS];
    int scheduled_action_count;

    uint64_t last_tick_time;

    bool mouse_move_active;
    int16_t mouse_move_dx;
    int16_t mouse_move_dy;

    float mouse_move_offset_x;
    float mouse_move_offset_y;
    float mouse_move_cursor_speed;
    bool mouse_offset_active_x;
    bool mouse_offset_active_y;

    pthread_mutex_t elements_mutex __attribute__((aligned(64)));

    uint32_t visual_dirty_mask[DIRTY_MASK_WORDS];
    char __pad2[48];
    uint32_t visual_gen_counter;

    TouchBinding g_element_long_press[MAX_ELEMENTS][MAX_DEFERRED_BINDINGS];
    TouchBinding g_element_gesture[MAX_ELEMENTS][MAX_DEFERRED_BINDINGS];
    uint8_t g_element_long_press_count[MAX_ELEMENTS];
    uint8_t g_element_gesture_count[MAX_ELEMENTS];

    TouchElement elements[MAX_ELEMENTS];
} TouchProcessorState;

extern TouchProcessorState g_state;

// Lock/unlock helpers for elements_mutex (replaced rwlock — write-only usage).
// Always active — timer thread (tick.c) modifies elements while visual_state_flush reads them.
#define ELEMENTS_LOCK()   pthread_mutex_lock(&g_state.elements_mutex)
#define ELEMENTS_UNLOCK() pthread_mutex_unlock(&g_state.elements_mutex)

// Global mutex for concurrent g_state access between UI thread (touch handlers)
// and C timer thread (touch_processor_tick).
// Lock order: g_state_mutex -> elements_mutex -> ring push (lock-free)
extern pthread_mutex_t g_state_mutex;

// Current finger index, set at entry of each touch_processor_on_finger_* handler.
// Used by schedule_action() to associate scheduled actions with a specific finger.
extern int g_current_finger_idx;
#define TP_STATE_LOCK()   pthread_mutex_lock(&g_state_mutex)
#define TP_STATE_UNLOCK() pthread_mutex_unlock(&g_state_mutex)

#endif // TP_STATE_H
