#ifndef PROFILE_TYPES_H
#define PROFILE_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============ ENUMS ============

typedef enum {
    PROFILE_MOUSE_MODE_TOUCHPAD = 0,
    PROFILE_MOUSE_MODE_TOUCHSCREEN = 1,
} ProfileMouseMode;

typedef enum {
    PROFILE_DRAG_MODE_AUTO = 0,
    PROFILE_DRAG_MODE_ALWAYS = 1,
    PROFILE_DRAG_MODE_NONE = 2,
} ProfileDragMode;

// Mirrors GestureType from touch_processor.h
typedef enum {
    PROFILE_GESTURE_SINGLE_TAP = 0,
    PROFILE_GESTURE_LONG_PRESS = 1,
    PROFILE_GESTURE_DOUBLE_TAP = 2,
    PROFILE_GESTURE_SINGLE_TAP_DRAG = 3,
    PROFILE_GESTURE_LONG_PRESS_DRAG = 4,
    PROFILE_GESTURE_DOUBLE_TAP_DRAG = 5,
    PROFILE_GESTURE_SINGLE_2ND = 6,
    PROFILE_GESTURE_DOUBLE_2ND = 7,
    PROFILE_GESTURE_SINGLE_DRAG_2ND = 8,
    PROFILE_GESTURE_DOUBLE_DRAG_2ND = 9,
    PROFILE_GESTURE_COUNT = 10,
} ProfileGestureType;

typedef enum {
    PROFILE_ACTIVATION_LOCK = 0,
    PROFILE_ACTIVATION_TRACK = 1,
    PROFILE_ACTIVATION_HOVER = 2,
} ProfileActivationMode;

typedef enum {
    PROFILE_ELEM_BUTTON = 0,
    PROFILE_ELEM_DPAD = 1,
    PROFILE_ELEM_RANGE_BUTTON = 2,
    PROFILE_ELEM_STICK = 3,
    PROFILE_ELEM_TRACKPAD = 4,
} ProfileElementType;

typedef enum {
    PROFILE_SHAPE_CIRCLE = 0,
    PROFILE_SHAPE_RECT = 1,
} ProfileElementShape;

typedef enum {
    PROFILE_RANGE_A_TO_Z = 0,
    PROFILE_RANGE_0_TO_9 = 1,
    PROFILE_RANGE_F1_TO_F12 = 2,
    PROFILE_RANGE_NP0_TO_NP9 = 3,
} ProfileRange;

typedef enum {
    PROFILE_BIND_CAT_NONE = 0,
    PROFILE_BIND_CAT_MOUSE = 1,
    PROFILE_BIND_CAT_KEYBOARD = 2,
    PROFILE_BIND_CAT_GAMEPAD = 3,
    PROFILE_BIND_CAT_MODIFIER = 4,
} ProfileBindCategory;

typedef enum {
    PROFILE_SCROLL_SLOT_SD = 0,
    PROFILE_SCROLL_SLOT_LD = 1,
    PROFILE_SCROLL_SLOT_DD = 2,
    PROFILE_SCROLL_SLOT_SD2 = 3,
    PROFILE_SCROLL_SLOT_DD2 = 4,
    PROFILE_SCROLL_SLOT_COUNT = 5,
} ProfileScrollSlot;

typedef enum {
    PROFILE_SCROLL_DIR_UP = 0,
    PROFILE_SCROLL_DIR_DOWN = 1,
    PROFILE_SCROLL_DIR_LEFT = 2,
    PROFILE_SCROLL_DIR_RIGHT = 3,
    PROFILE_SCROLL_DIR_COUNT = 4,
} ProfileScrollDirection;

// ============ CONSTANTS ============

#define PROFILE_MAX_ELEMENTS 128
#define PROFILE_MAX_BINDINGS_PER_SLOT 8
#define PROFILE_MAX_BINDING_SLOTS 4
#define PROFILE_GESTURE_SLOT_COUNT 5
#define PROFILE_SCROLL_DIRECTION_COUNT 4
#define PROFILE_MAX_GESTURE_BINDINGS 8
#define PROFILE_DISPLAY_TEXT_MAX 32
#define PROFILE_MAX_CONTROLLER_BINDINGS 32
#define PROFILE_MAX_CONTROLLERS 8

// ============ STRUCTS ============

// A single binding (replaces Java Bind)
typedef struct {
    int type_value;      // encoded type (from Bind.encodeTypeValue())
    int key_value;       // encoded key (from Bind.encodeKeyValue())
    ProfileBindCategory category;
    char name[48];       // e.g. "MOUSE_LEFT_BUTTON"
} ProfileBind;

// A package of bindings with metadata (replaces Java BindPackage)
typedef struct {
    ProfileBind bindings[PROFILE_MAX_BINDINGS_PER_SLOT];
    int count;
    bool sticky[PROFILE_MAX_BINDINGS_PER_SLOT];
    bool toggle_switch;
    bool auto_repeat;
    int auto_repeat_interval_ms;
    int bind_delays_ms[PROFILE_MAX_BINDINGS_PER_SLOT];
} ProfileBindPackage;

// A control element (replaces Java ControlElement)
typedef struct {
    ProfileElementType type;
    ProfileElementShape shape;
    float x;
    float y;
    float w;
    float h;
    float scale;
    float corner_radius;
    float dpad_corner_radius;
    bool passthrough_touch;
    float opacity;
    char display_text[PROFILE_DISPLAY_TEXT_MAX];
    int icon_id;
    bool has_custom_icon;
    char custom_icon_data[4096];
    
    // Range button specific
    ProfileRange range;
    int orientation;
    int binding_count;
    bool toggle_switch;
    
    // Bindings (up to 4 slots)
    ProfileBindPackage slot_packages[PROFILE_MAX_BINDING_SLOTS];
    ProfileBind binds[PROFILE_MAX_BINDING_SLOTS]; // simple binds (non-button)
    
    // Button-specific
    ProfileBindPackage long_press_package;
    ProfileBindPackage gesture_package;
    
    bool has_gamepad_binding;
} ProfileControlElement;

// External controller binding
typedef struct {
    int key_code;        // Android KeyEvent keyCode
    ProfileBind bind;
} ProfileControllerBinding;

// External controller (replaces Java ExternalController)
typedef struct {
    char id[128];
    char name[128];
    ProfileControllerBinding bindings[PROFILE_MAX_CONTROLLER_BINDINGS];
    int binding_count;
} ProfileExternalController;

// Gesture settings (from ControlsProfile gestureSettings)
typedef struct {
    ProfileMouseMode mouse_mode;
    ProfileDragMode drag_mode;
    ProfileActivationMode activation_mode;
    
    int double_tap_timeout_ms;
    int long_press_timeout_ms;
    
    // Gesture actions (10 types)
    ProfileBindPackage gesture_actions[PROFILE_GESTURE_COUNT];
    
    // Scroll bindings [slot][direction]
    ProfileBind scroll_bindings[PROFILE_SCROLL_SLOT_COUNT][PROFILE_SCROLL_DIR_COUNT];
    int scroll_threshold_px;
    bool scroll_hold_v[PROFILE_SCROLL_SLOT_COUNT];
    bool scroll_hold_h[PROFILE_SCROLL_SLOT_COUNT];
} ProfileGestureSettings;

// Full profile data (replaces Java ControlsProfile)
typedef struct {
    int id;
    char name[64];
    float cursor_speed;
    
    // Elements
    ProfileControlElement elements[PROFILE_MAX_ELEMENTS];
    int element_count;
    
    // External controllers
    ProfileExternalController controllers[PROFILE_MAX_CONTROLLERS];
    int controller_count;
    
    // Gesture settings
    ProfileGestureSettings gesture_settings;
    
    // Misc settings
    int binding_delay_ms;
    int long_press_delay_ms;

    // --- Phantom delays promoted to profile-driven sources (was hardcoded) ---
    // Time-based values are in milliseconds; converted to ticks in convert.c.
    int inter_bind_min_gap_ms;        // executor.c hard-coded +1 tick
    int bind_press_width_ms;          // executor.c hard-coded +1 tick PRESS->RELEASE width
    int release_hold_min_gap_ms;      // executor.c hard-coded +1 tick in release_held_actions
    int range_tap_release_ms;         // range_button.c hard-coded now_tick+2
    int sim_click_press_ms;           // handler.c/mouse_move_tick.c hard-coded now_tick+3
    int sim_click_release_ms;         // mouse_move_tick.c hard-coded now_tick+3
    int gesture_swipe_min_ms;         // long_press.c hard-coded >=3 tick swipe trigger
    int toggle_debounce_ms;           // button.c TOGGLE_DEBOUNCE_MS (now ms, previously compared to ticks)
    int idle_debounce_threshold;      // timer.c IDLE_DEBOUNCE_THRESHOLD 3 (dimensionless)
    int idle_interval_ms;             // timer.c idle_interval_ms 100
    int default_auto_repeat_interval_ms; // elements_setup/sync_dispatch DEFAULT_AUTO_REPEAT_INTERVAL_TICKS 6
    int range_scroll_threshold_px;    // range_button.c RANGE_SCROLL_THRESHOLD 10 (pixels)
    int gesture_threshold_px_fallback; // button.c GESTURE_THRESHOLD_FALLBACK 20 (pixels)
    int cursor_accel_threshold;       // was init.c DEFAULT_CURSOR_ACCEL_THRESHOLD 6
    float cursor_accel_factor;        // was init.c DEFAULT_CURSOR_ACCEL_FACTOR 1.25f
    int timer_interval_ms;            // 0 = use global default; ms per tick
    int button_long_press_haptic;
    int button_gesture_haptic;
    int gesture_long_press_haptic;
    int scroll_bind_haptic;
    int scroll_hold_haptic;
    int scroll_hold_threshold_px;
    int drag_threshold_px;
    int double_tap_distance_px;
    int gesture_threshold_px;
    float stroke_width;
    int fill_alpha_inactive;
    float corner_radius;
} ProfileData;

// ============ FUNCTIONS ============

void profile_data_init(ProfileData* profile);
void profile_data_free(ProfileData* profile);
void profile_element_init(ProfileControlElement* elem);
void profile_bind_package_init(ProfileBindPackage* pkg);
void profile_gesture_settings_init(ProfileGestureSettings* gs);

#ifdef __cplusplus
}
#endif

#endif /* PROFILE_TYPES_H */
