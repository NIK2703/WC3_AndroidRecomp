#include "loader.h"
#include "types.h"
#include "bind.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

static char g_last_error[256] = {0};

const char* profile_last_error(void) {
    return g_last_error;
}

void profile_data_init(ProfileData* profile) {
    memset(profile, 0, sizeof(ProfileData));
    profile->cursor_speed = 1.0f;
    profile->long_press_delay_ms = 200;
    profile->button_long_press_haptic = 1;
    profile->button_gesture_haptic = 5;
    profile->gesture_long_press_haptic = 3;
    profile->scroll_hold_threshold_px = 100;
    profile->drag_threshold_px = 10;
    profile->double_tap_distance_px = 50;
    profile->gesture_threshold_px = 20;
    profile->stroke_width = 0.15f;
    profile->fill_alpha_inactive = 50;
    profile->corner_radius = 0.8f;

    // Phantom-delay defaults (previously hardcoded in the native layer).
    // Expressed in the same units the profile would carry them (ms / px).
    profile->inter_bind_min_gap_ms = 16;       // ~1 tick @16ms timer
    profile->bind_press_width_ms = 16;         // ~1 tick @16ms timer
    profile->release_hold_min_gap_ms = 16;     // ~1 tick @16ms timer
    profile->range_tap_release_ms = 32;        // ~2 ticks @16ms timer
    profile->sim_click_press_ms = 48;          // ~3 ticks @16ms timer
    profile->sim_click_release_ms = 48;        // ~3 ticks @16ms timer
    profile->gesture_swipe_min_ms = 48;        // ~3 ticks @16ms timer
    profile->toggle_debounce_ms = 50;          // was TOGGLE_DEBOUNCE_MS (now correctly ms)
    profile->idle_debounce_threshold = 3;      // was IDLE_DEBOUNCE_THRESHOLD
    profile->idle_interval_ms = 100;           // was idle_interval_ms
    profile->default_auto_repeat_interval_ms = 96; // 6 ticks @16ms timer
    profile->range_scroll_threshold_px = 10;   // was RANGE_SCROLL_THRESHOLD
    profile->gesture_threshold_px_fallback = 20; // was GESTURE_THRESHOLD_FALLBACK
    profile->cursor_accel_threshold = 6;       // was DEFAULT_CURSOR_ACCEL_THRESHOLD
    profile->cursor_accel_factor = 1.25f;      // was DEFAULT_CURSOR_ACCEL_FACTOR
    profile->timer_interval_ms = 0;            // 0 = use global default

    profile_gesture_settings_init(&profile->gesture_settings);
}

void profile_data_free(ProfileData* profile) {
    // Currently all fields are inline (no heap allocation).
    // This is a placeholder for future cleanup if dynamic fields are added.
    (void)profile;
}

void profile_element_init(ProfileControlElement* elem) {
    memset(elem, 0, sizeof(ProfileControlElement));
    elem->scale = 1.0f;
    elem->w = 8.0f;
    elem->h = 4.0f;
    elem->corner_radius = -1.0f;
    elem->dpad_corner_radius = 0.6f;
    elem->opacity = -1.0f;
}

void profile_bind_package_init(ProfileBindPackage* pkg) {
    memset(pkg, 0, sizeof(ProfileBindPackage));
    pkg->auto_repeat_interval_ms = 300;
}

// ------------------------------------------------------------------
// PRONG A: Resolve zero delays to the profile-level default for
// bindings at index > 0.  This handles backward compatibility with
// old profiles saved before the Java sentinel fix, where the JSON
// delays array contained all zeros (e.g. [0, 0, 0]).
// After resolution: index 0 stays 0, index > 0 becomes default_delay_ms.
// ------------------------------------------------------------------
static void profile_bind_package_resolve_delays(ProfileBindPackage* pkg, int default_delay_ms) {
    for (int i = 1; i < pkg->count && i < PROFILE_MAX_BINDINGS_PER_SLOT; i++) {
        if (pkg->bind_delays_ms[i] == 0) {
            pkg->bind_delays_ms[i] = default_delay_ms;
        }
    }
}

static const char* GESTURE_KEYS[PROFILE_GESTURE_COUNT] = {
    "singleTapAction",
    "longPressAction",
    "doubleTapAction",
    "singleTapDragAction",
    "longPressDragAction",
    "doubleTapDragAction",
    "singleTap2ndFingerAction",
    "doubleTap2ndFingerAction",
    "singleTap2ndFingerDragAction",
    "doubleTap2ndFingerDragAction"
};

static const char* GESTURE_DEFAULTS[PROFILE_GESTURE_COUNT] = {
    "MOUSE_LEFT_BUTTON", "MOUSE_LEFT_BUTTON", "NONE",
    "NONE", "NONE", "NONE",
    "MOUSE_RIGHT_BUTTON", "NONE",
    "MOUSE_LEFT_BUTTON", "NONE"
};

void profile_gesture_settings_init(ProfileGestureSettings* gs) {
    memset(gs, 0, sizeof(ProfileGestureSettings));
    gs->mouse_mode = PROFILE_MOUSE_MODE_TOUCHPAD;
    gs->drag_mode = PROFILE_DRAG_MODE_AUTO;
    gs->activation_mode = PROFILE_ACTIVATION_LOCK;
    gs->double_tap_timeout_ms = 150;
    gs->long_press_timeout_ms = 200;
    gs->scroll_threshold_px = 50;

    for (int i = 0; i < PROFILE_GESTURE_COUNT; i++) {
        profile_bind_package_init(&gs->gesture_actions[i]);
    }

    // Apply gesture action defaults (matching Java GESTURE_DEFAULTS)
    for (int i = 0; i < PROFILE_GESTURE_COUNT; i++) {
        if (GESTURE_DEFAULTS[i] != NULL && strcmp(GESTURE_DEFAULTS[i], "NONE") != 0) {
            ProfileBind default_bind;
            if (profile_bind_from_string(GESTURE_DEFAULTS[i], &default_bind) == 0) {
                gs->gesture_actions[i].bindings[0] = default_bind;
                gs->gesture_actions[i].count = 1;
            }
        }
    }
}

// ========== Helpers ==========

static int clamp_int(int val, int min, int max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

static const char* SCROLL_SLOT_KEYS[PROFILE_SCROLL_SLOT_COUNT] = {
    "sd", "ld", "dd", "sd2", "dd2"
};

static const char* SCROLL_DIR_KEYS[PROFILE_SCROLL_DIR_COUNT] = {
    "up", "down", "left", "right"
};

// Shape default dimensions (mirrors Java constants)
#define SQUARE_DEFAULT_SIZE   5.0f
#define ROUND_RECT_DEFAULT_W  8.0f
#define ROUND_RECT_DEFAULT_H  4.0f
#define ROUND_RECT_DEFAULT_C  2.0f
#define SQUARE_DEFAULT_C      0.75f
#define RECT_DEFAULT_W        8.0f
#define RECT_DEFAULT_H        4.0f

// Clamp ranges (mirrors Java constants)
#define DOUBLE_TAP_TIMEOUT_MIN  50
#define DOUBLE_TAP_TIMEOUT_MAX  500
#define LONG_PRESS_TIMEOUT_MIN  50
#define LONG_PRESS_TIMEOUT_MAX  1000
#define BINDING_DELAY_MIN       0
#define BINDING_DELAY_MAX       100   // inter-bind delay (sequence)
#define PROFILE_DELAY_MAX       50    // profile-level bindingDelay
#define LONG_PRESS_DELAY_MIN    50
#define LONG_PRESS_DELAY_MAX    1000
#define HAPTIC_MIN              0
#define HAPTIC_MAX              5
#define SCROLL_HOLD_THRESH_MIN  5
#define SCROLL_HOLD_THRESH_MAX  100
#define DRAG_THRESH_MIN         0
#define DRAG_THRESH_MAX         30
#define DOUBLE_TAP_DIST_MIN     10
#define DOUBLE_TAP_DIST_MAX     100
#define GESTURE_THRESH_MIN      10
#define GESTURE_THRESH_MAX      50
#define SCROLL_THRESH_MIN       5
#define SCROLL_THRESH_MAX       100

static int lookup_scroll_slot(const char* key) {
    for (int i = 0; i < PROFILE_SCROLL_SLOT_COUNT; i++) {
        if (strcmp(SCROLL_SLOT_KEYS[i], key) == 0) return i;
    }
    return -1;
}

static int lookup_scroll_dir(const char* key) {
    for (int i = 0; i < PROFILE_SCROLL_DIR_COUNT; i++) {
        if (strcmp(SCROLL_DIR_KEYS[i], key) == 0) return i;
    }
    return -1;
}

// ========== Enum parsers ==========

static ProfileElementType parse_elem_type(const char* s) {
    if (s == NULL) return PROFILE_ELEM_BUTTON;
    if (strcmp(s, "BUTTON") == 0) return PROFILE_ELEM_BUTTON;
    if (strcmp(s, "D_PAD") == 0) return PROFILE_ELEM_DPAD;
    if (strcmp(s, "RANGE_BUTTON") == 0) return PROFILE_ELEM_RANGE_BUTTON;
    if (strcmp(s, "STICK") == 0) return PROFILE_ELEM_STICK;
    if (strcmp(s, "TRACKPAD") == 0) return PROFILE_ELEM_TRACKPAD;
    return PROFILE_ELEM_BUTTON;
}

static ProfileElementShape parse_elem_shape(const char* s) {
    if (s == NULL) return PROFILE_SHAPE_RECT;
    if (strcmp(s, "CIRCLE") == 0) return PROFILE_SHAPE_CIRCLE;
    // SQUARE, ROUND_RECT map to RECT
    return PROFILE_SHAPE_RECT;
}

static ProfileRange parse_elem_range(const char* s) {
    if (s == NULL) return PROFILE_RANGE_A_TO_Z;
    if (strcmp(s, "FROM_A_TO_Z") == 0) return PROFILE_RANGE_A_TO_Z;
    if (strcmp(s, "FROM_0_TO_9") == 0) return PROFILE_RANGE_0_TO_9;
    if (strcmp(s, "FROM_F1_TO_F12") == 0) return PROFILE_RANGE_F1_TO_F12;
    if (strcmp(s, "FROM_NP0_TO_NP9") == 0) return PROFILE_RANGE_NP0_TO_NP9;
    return PROFILE_RANGE_A_TO_Z;
}

// ========== Binding parsers ==========

// Parse a JSON array of binding strings (e.g. ["MOUSE_LEFT_BUTTON", "NONE"])
// into the package. Skip NONE entries.
static void parse_bindings_array(TpJsonValue* arr, ProfileBindPackage* pkg) {
    if (arr == NULL) return;
    int len = tp_json_arr_len(arr);
    pkg->count = 0;
    for (int i = 0; i < len && pkg->count < PROFILE_MAX_BINDINGS_PER_SLOT; i++) {
        TpJsonValue* item = tp_json_arr_get(arr, i);
        if (!tp_json_is_string(item)) continue;
        const char* s = tp_json_get_string(item);
        if (s == NULL || strcmp(s, "NONE") == 0) continue;
        ProfileBind bind;
        if (profile_bind_from_string(s, &bind) == 0) {
            pkg->bindings[pkg->count] = bind;
            pkg->sticky[pkg->count] = false;
            pkg->count++;
        }
    }
}

// Parse a BindPackage JSON object with keys:
// "bindings" (array), "sticky" (array), "toggleSwitch" (bool),
// "autoRepeat" (bool), "autoRepeatIntervalMs" (int)
static void parse_bind_package(TpJsonValue* obj, ProfileBindPackage* pkg) {
    if (obj == NULL) return;
    profile_bind_package_init(pkg);

    TpJsonValue* bindings_arr = tp_json_obj_get(obj, "bindings");
    if (bindings_arr != NULL) {
        parse_bindings_array(bindings_arr, pkg);
    }

    TpJsonValue* sticky_arr = tp_json_obj_get(obj, "sticky");
    if (sticky_arr != NULL) {
        int slen = tp_json_arr_len(sticky_arr);
        for (int i = 0; i < slen && i < pkg->count; i++) {
            TpJsonValue* sval = tp_json_arr_get(sticky_arr, i);
            if (sval != NULL && tp_json_is_bool(sval)) {
                pkg->sticky[i] = tp_json_get_bool(sval);
            }
        }
    }

    pkg->toggle_switch = tp_json_get_bool_def(obj, "toggleSwitch", false);
    pkg->auto_repeat = tp_json_get_bool_def(obj, "autoRepeat", false);
    pkg->auto_repeat_interval_ms = tp_json_get_int_def(obj, "autoRepeatIntervalMs", 300);

    TpJsonValue* delays_arr = tp_json_obj_get(obj, "delays");
    if (delays_arr != NULL) {
        int dlen = tp_json_arr_len(delays_arr);
        for (int i = 0; i < dlen && i < pkg->count && i < PROFILE_MAX_BINDINGS_PER_SLOT; i++) {
            TpJsonValue* dval = tp_json_arr_get(delays_arr, i);
            if (dval != NULL && tp_json_is_number(dval)) {
                pkg->bind_delays_ms[i] = clamp_int(tp_json_get_int(dval), BINDING_DELAY_MIN, BINDING_DELAY_MAX);
            }
        }
    }
}

// Parse simple ["KEY_W", "KEY_D", "KEY_S", "KEY_A"] format into binds[]
static void parse_simple_binds(TpJsonValue* binds_arr, ProfileBind* binds, int max_slots) {
    if (binds_arr == NULL) return;
    int len = tp_json_arr_len(binds_arr);
    for (int i = 0; i < len && i < max_slots; i++) {
        TpJsonValue* item = tp_json_arr_get(binds_arr, i);
        if (!tp_json_is_string(item)) continue;
        const char* s = tp_json_get_string(item);
        if (s == NULL || strcmp(s, "NONE") == 0) continue;
        ProfileBind bind;
        if (profile_bind_from_string(s, &bind) == 0) {
            binds[i] = bind;
        }
    }
}

// Parse legacy "bindings" array into BindPackage array.
// Handles both flat format and nested format (mirrors Java parseBindingsFromJson).
// flat: ["MOUSE_LEFT_BUTTON", "NONE", ...]
// nested: [["MOUSE_LEFT_BUTTON"], ["KEY_W","KEY_D"], ...]
// Returns number of packages parsed (up to PROFILE_MAX_BINDING_SLOTS).
static int parse_legacy_bindings_array(TpJsonValue* arr, ProfileBindPackage* pkgs, bool toggle_switch) {
    if (arr == NULL) return 0;
    int len = tp_json_arr_len(arr);
    if (len == 0) return 0;
    int count = len < PROFILE_MAX_BINDING_SLOTS ? len : PROFILE_MAX_BINDING_SLOTS;

    TpJsonValue* first = tp_json_arr_get(arr, 0);
    bool is_flat = tp_json_is_string(first);

    for (int i = 0; i < count; i++) {
        TpJsonValue* item = tp_json_arr_get(arr, i);
        profile_bind_package_init(&pkgs[i]);
        pkgs[i].toggle_switch = toggle_switch;

        if (is_flat) {
            // Flat: each element is a single binding name
            if (tp_json_is_string(item)) {
                const char* s = tp_json_get_string(item);
                if (s != NULL && strcmp(s, "NONE") != 0) {
                    ProfileBind bind;
                    if (profile_bind_from_string(s, &bind) == 0) {
                        pkgs[i].bindings[0] = bind;
                        pkgs[i].count = 1;
                    }
                }
            }
        } else {
            // Nested: each element is an array of binding names
            if (tp_json_is_array(item)) {
                parse_bindings_array(item, &pkgs[i]);
            }
        }
    }
    return count;
}

// ========== Element parsing ==========

// Shape defaults lookup
static void apply_shape_defaults(ProfileControlElement* elem, const char* shape_str) {
    if (shape_str == NULL) return;
    if (strcmp(shape_str, "SQUARE") == 0) {
        elem->w = SQUARE_DEFAULT_SIZE;
        elem->h = SQUARE_DEFAULT_SIZE;
        elem->corner_radius = SQUARE_DEFAULT_C;
    } else if (strcmp(shape_str, "ROUND_RECT") == 0) {
        elem->w = ROUND_RECT_DEFAULT_W;
        elem->h = ROUND_RECT_DEFAULT_H;
        elem->corner_radius = ROUND_RECT_DEFAULT_C;
    } else if (strcmp(shape_str, "RECT") == 0) {
        elem->w = RECT_DEFAULT_W;
        elem->h = RECT_DEFAULT_H;
    }
}

// Parse a single element from JSON object
static int parse_element(TpJsonValue* elem_obj, ProfileControlElement* out_elem) {
    if (elem_obj == NULL) return -1;

    profile_element_init(out_elem);

    // --- Type ---
    const char* type_str = tp_json_get_string_def(elem_obj, "type", "BUTTON");
    out_elem->type = parse_elem_type(type_str);

    // --- Shape ---
    const char* shape_str = tp_json_get_string_def(elem_obj, "shape", "RECT");
    out_elem->shape = parse_elem_shape(shape_str);

    // Apply shape defaults (SQUARE, ROUND_RECT remap dimensions)
    apply_shape_defaults(out_elem, shape_str);

    // --- Dimensions ---
    out_elem->w = (float)tp_json_get_num_def(elem_obj, "elementWidth", (double)out_elem->w);
    out_elem->h = (float)tp_json_get_num_def(elem_obj, "elementHeight", (double)out_elem->h);
    out_elem->corner_radius = (float)tp_json_get_num_def(elem_obj, "cornerRadius", (double)out_elem->corner_radius);
    out_elem->dpad_corner_radius = (float)tp_json_get_num_def(elem_obj, "dpadCornerRadius", (double)out_elem->dpad_corner_radius);

    // --- Boolean flags ---
    out_elem->passthrough_touch = tp_json_get_bool_def(elem_obj, "passthroughTouch", false);

    // --- Opacity ---
    out_elem->opacity = (float)tp_json_get_num_def(elem_obj, "opacity", (double)out_elem->opacity);

    // --- Position (store as normalized 0-1 floats; conversion to pixels happens later) ---
    out_elem->x = (float)tp_json_get_num_def(elem_obj, "x", 0.0);
    out_elem->y = (float)tp_json_get_num_def(elem_obj, "y", 0.0);

    // --- Scale ---
    out_elem->scale = (float)tp_json_get_num_def(elem_obj, "scale", (double)out_elem->scale);

    // --- Text ---
    const char* text = tp_json_get_string_def(elem_obj, "text", "");
    strncpy(out_elem->display_text, text, sizeof(out_elem->display_text) - 1);
    out_elem->display_text[sizeof(out_elem->display_text) - 1] = '\0';

    // --- Icon ---
    out_elem->icon_id = tp_json_get_int_def(elem_obj, "iconId", 0);

    // --- Custom icon data ---
    const char* cid = tp_json_get_string_def(elem_obj, "customIconData", "");
    if (cid != NULL && strlen(cid) > 0) {
        strncpy(out_elem->custom_icon_data, cid, sizeof(out_elem->custom_icon_data) - 1);
        out_elem->custom_icon_data[sizeof(out_elem->custom_icon_data) - 1] = '\0';
        out_elem->has_custom_icon = true;
    }

    // --- Range (for RANGE_BUTTON) ---
    const char* range_str = tp_json_get_string_def(elem_obj, "range", NULL);
    if (range_str != NULL) {
        out_elem->range = parse_elem_range(range_str);
    }

    // --- Orientation ---
    out_elem->orientation = tp_json_get_int_def(elem_obj, "orientation", 0);

    // --- Bindings ---

    // Track whether any non-gamepad bind is present
    bool has_non_gamepad = false;

    // Helper to check a bind and update flag
    // (In Java: hasGamepadBinding starts true; if any bind is not gamepad, it becomes false)
    // So has_gamepad_binding = !has_non_gamepad

    if (out_elem->type == PROFILE_ELEM_BUTTON) {
        // BUTTON: slotPackages (array of BindPackage objects)
        bool migrated = false;
        TpJsonValue* slot_packages = tp_json_obj_get(elem_obj, "slotPackages");
        if (slot_packages != NULL) {
            int pkg_count = tp_json_arr_len(slot_packages);
            if (pkg_count > PROFILE_MAX_BINDING_SLOTS) pkg_count = PROFILE_MAX_BINDING_SLOTS;
            for (int j = 0; j < pkg_count; j++) {
                TpJsonValue* pkg_obj = tp_json_arr_get(slot_packages, j);
                if (pkg_obj != NULL) {
                    parse_bind_package(pkg_obj, &out_elem->slot_packages[j]);
                    // Check for non-gamepad bindings
                    for (int k = 0; k < out_elem->slot_packages[j].count; k++) {
                        if (out_elem->slot_packages[j].bindings[k].category != PROFILE_BIND_CAT_GAMEPAD) {
                            has_non_gamepad = true;
                        }
                    }
                }
            }
            migrated = true;
        }

        if (!migrated) {
            // Legacy: parse "bindings" flat/nested array
            TpJsonValue* legacy_binds = tp_json_obj_get(elem_obj, "bindings");
            if (legacy_binds != NULL) {
                bool toggle = tp_json_get_bool_def(elem_obj, "toggleSwitch", false);
                int cnt = parse_legacy_bindings_array(legacy_binds, out_elem->slot_packages, toggle);
                for (int j = 0; j < cnt; j++) {
                    for (int k = 0; k < out_elem->slot_packages[j].count; k++) {
                        if (out_elem->slot_packages[j].bindings[k].category != PROFILE_BIND_CAT_GAMEPAD) {
                            has_non_gamepad = true;
                        }
                    }
                }
            }
        }

        // longPressPackage
        TpJsonValue* lp = tp_json_obj_get(elem_obj, "longPressPackage");
        if (lp != NULL) {
            parse_bind_package(lp, &out_elem->long_press_package);
        }

        // gesturePackage
        TpJsonValue* gp = tp_json_obj_get(elem_obj, "gesturePackage");
        if (gp != NULL) {
            parse_bind_package(gp, &out_elem->gesture_package);
        }

        // Legacy longPressBindings (flat array)
        TpJsonValue* lp_binds = tp_json_obj_get(elem_obj, "longPressBindings");
        if (lp_binds != NULL && out_elem->long_press_package.count == 0) {
            parse_bindings_array(lp_binds, &out_elem->long_press_package);
        }

        // Legacy gestureBindings (flat array)
        TpJsonValue* g_binds = tp_json_obj_get(elem_obj, "gestureBindings");
        if (g_binds != NULL && out_elem->gesture_package.count == 0) {
            parse_bindings_array(g_binds, &out_elem->gesture_package);
        }
    } else {
        // Non-BUTTON (STICK, DPAD, RANGE_BUTTON, TRACKPAD)
        out_elem->toggle_switch = tp_json_get_bool_def(elem_obj, "toggleSwitch", false);

        // Try new simple "binds" format first
        TpJsonValue* binds_arr = tp_json_obj_get(elem_obj, "binds");
        if (binds_arr != NULL) {
            parse_simple_binds(binds_arr, out_elem->binds, PROFILE_MAX_BINDING_SLOTS);
            out_elem->binding_count = tp_json_arr_len(binds_arr);
            if (out_elem->binding_count > PROFILE_MAX_BINDING_SLOTS)
                out_elem->binding_count = PROFILE_MAX_BINDING_SLOTS;
        } else {
            // Migration: try slotPackages (read first bind from each)
            bool migrated = false;
            TpJsonValue* slot_packages = tp_json_obj_get(elem_obj, "slotPackages");
            if (slot_packages != NULL) {
                int pkg_count = tp_json_arr_len(slot_packages);
                if (pkg_count > PROFILE_MAX_BINDING_SLOTS) pkg_count = PROFILE_MAX_BINDING_SLOTS;
                out_elem->binding_count = pkg_count;
                for (int j = 0; j < pkg_count; j++) {
                    TpJsonValue* pkg_obj = tp_json_arr_get(slot_packages, j);
                    if (pkg_obj != NULL) {
                        ProfileBindPackage tmp;
                        profile_bind_package_init(&tmp);
                        parse_bind_package(pkg_obj, &tmp);
                        if (tmp.count > 0) {
                            out_elem->binds[j] = tmp.bindings[0];
                        }
                        for (int k = 0; k < tmp.count; k++) {
                            if (tmp.bindings[k].category != PROFILE_BIND_CAT_GAMEPAD) {
                                has_non_gamepad = true;
                            }
                        }
                    }
                }
                migrated = true;
            }

            if (!migrated) {
                // Legacy: parse "bindings" flat/nested array, extract first bind from each
                TpJsonValue* legacy_binds = tp_json_obj_get(elem_obj, "bindings");
                if (legacy_binds != NULL) {
                    bool toggle = tp_json_get_bool_def(elem_obj, "toggleSwitch", false);
                    ProfileBindPackage tmp_pkgs[PROFILE_MAX_BINDING_SLOTS];
                    int cnt = parse_legacy_bindings_array(legacy_binds, tmp_pkgs, toggle);
                    out_elem->binding_count = cnt;
                    for (int j = 0; j < cnt; j++) {
                        if (tmp_pkgs[j].count > 0) {
                            out_elem->binds[j] = tmp_pkgs[j].bindings[0];
                        }
                        for (int k = 0; k < tmp_pkgs[j].count; k++) {
                            if (tmp_pkgs[j].bindings[k].category != PROFILE_BIND_CAT_GAMEPAD) {
                                has_non_gamepad = true;
                            }
                        }
                    }
                }
            }
        }
    }

    // Count non-NONE bindings (for BUTTON, since slot_packages don't populate binds[])
    // Non-BUTTON elements set binding_count from JSON array length above.
    if (out_elem->type == PROFILE_ELEM_BUTTON) {
        out_elem->binding_count = 0;
        for (int i = 0; i < PROFILE_MAX_BINDING_SLOTS; i++) {
            if (out_elem->binds[i].type_value != 0 || out_elem->binds[i].key_value != 0) {
                out_elem->binding_count++;
            }
        }
    }

    // Determine has_gamepad_binding:
    // Java: starts true, set to false if any bind is not gamepad
    // Equivalent to: true if all binds are gamepad
    out_elem->has_gamepad_binding = !has_non_gamepad;

    return 0;
}

// ========== Gesture settings parsing ==========

static ProfileMouseMode parse_mouse_mode(const char* s) {
    if (s == NULL) return PROFILE_MOUSE_MODE_TOUCHPAD;
    if (strcasecmp(s, "TOUCHSCREEN") == 0) return PROFILE_MOUSE_MODE_TOUCHSCREEN;
    return PROFILE_MOUSE_MODE_TOUCHPAD;
}

static ProfileDragMode parse_drag_mode(const char* s) {
    if (s == NULL) return PROFILE_DRAG_MODE_AUTO;
    if (strcasecmp(s, "ALWAYS") == 0) return PROFILE_DRAG_MODE_ALWAYS;
    if (strcasecmp(s, "NONE") == 0) return PROFILE_DRAG_MODE_NONE;
    return PROFILE_DRAG_MODE_AUTO;
}

static ProfileActivationMode parse_activation_mode(const char* s) {
    if (s == NULL) return PROFILE_ACTIVATION_LOCK;
    if (strcasecmp(s, "TRACK") == 0) return PROFILE_ACTIVATION_TRACK;
    if (strcasecmp(s, "HOVER") == 0) return PROFILE_ACTIVATION_HOVER;
    return PROFILE_ACTIVATION_LOCK;
}

static void parse_gesture_settings(TpJsonValue* gs_obj, ProfileGestureSettings* gs) {
    if (gs_obj == NULL) return;

    profile_gesture_settings_init(gs);

    // --- Enums ---
    gs->mouse_mode = parse_mouse_mode(tp_json_get_string_def(gs_obj, "mouseMode", NULL));
    gs->drag_mode = parse_drag_mode(tp_json_get_string_def(gs_obj, "dragMode", NULL));
    gs->activation_mode = parse_activation_mode(tp_json_get_string_def(gs_obj, "touchActivationMode", NULL));

    // --- Timeouts ---
    gs->double_tap_timeout_ms = clamp_int(
        tp_json_get_int_def(gs_obj, "doubleTapTimeout", gs->double_tap_timeout_ms),
        DOUBLE_TAP_TIMEOUT_MIN, DOUBLE_TAP_TIMEOUT_MAX);
    gs->long_press_timeout_ms = clamp_int(
        tp_json_get_int_def(gs_obj, "longPressTimeout", gs->long_press_timeout_ms),
        LONG_PRESS_TIMEOUT_MIN, LONG_PRESS_TIMEOUT_MAX);

    // --- Gesture action keys ---
    for (int i = 0; i < PROFILE_GESTURE_COUNT; i++) {
        TpJsonValue* ga = tp_json_obj_get(gs_obj, GESTURE_KEYS[i]);
        if (ga != NULL) {
            if (tp_json_is_object(ga)) {
                // BindPackage object format
                parse_bind_package(ga, &gs->gesture_actions[i]);
            } else if (tp_json_is_array(ga)) {
                // Legacy flat array format
                ProfileBindPackage tmp;
                profile_bind_package_init(&tmp);
                parse_bindings_array(ga, &tmp);
                // Empty or all-NONE array: apply NONE so caps_mode_mask stays non-zero
                // (prevents phantom left-click while preserving cursor-on-tap)
                if (tmp.count == 0) {
                    ProfileBind none_bind;
                    if (profile_bind_from_string("NONE", &none_bind) == 0) {
                        tmp.bindings[0] = none_bind;
                        tmp.count = 1;
                    }
                }
                gs->gesture_actions[i] = tmp;
            }
            // Empty object/array after parsing: same NONE fallback
            if (gs->gesture_actions[i].count == 0) {
                ProfileBind none_bind;
                if (profile_bind_from_string("NONE", &none_bind) == 0) {
                    gs->gesture_actions[i].bindings[0] = none_bind;
                    gs->gesture_actions[i].count = 1;
                }
            }
        } else if (gs->gesture_actions[i].count == 0 && GESTURE_DEFAULTS[i] != NULL && strcmp(GESTURE_DEFAULTS[i], "NONE") != 0) {
            // Key not present in JSON: apply default
            ProfileBind default_bind;
            if (profile_bind_from_string(GESTURE_DEFAULTS[i], &default_bind) == 0) {
                gs->gesture_actions[i].bindings[0] = default_bind;
                gs->gesture_actions[i].count = 1;
            }
        }
    }

    // --- Scroll bindings ---
    TpJsonValue* scroll_data = tp_json_obj_get(gs_obj, "scrollBindings");
    if (scroll_data != NULL) {
        for (int s = 0; s < PROFILE_SCROLL_SLOT_COUNT; s++) {
            if (!tp_json_is_object(scroll_data)) break;
            TpJsonValue* slot_obj = tp_json_obj_get(scroll_data, SCROLL_SLOT_KEYS[s]);
            if (slot_obj == NULL) continue;
            for (int d = 0; d < PROFILE_SCROLL_DIR_COUNT; d++) {
                TpJsonValue* dir_val = tp_json_obj_get(slot_obj, SCROLL_DIR_KEYS[d]);
                if (dir_val == NULL) continue;
                if (tp_json_is_string(dir_val)) {
                    const char* bind_name = tp_json_get_string(dir_val);
                    if (bind_name != NULL && strcmp(bind_name, "NONE") != 0) {
                        ProfileBind bind;
                        if (profile_bind_from_string(bind_name, &bind) == 0) {
                            gs->scroll_bindings[s][d] = bind;
                        }
                    }
                } else if (tp_json_is_object(dir_val)) {
                    // Backward compatibility: old format was BindPackage JSON object
                    ProfileBindPackage tmp;
                    profile_bind_package_init(&tmp);
                    parse_bind_package(dir_val, &tmp);
                    if (tmp.count > 0) {
                        gs->scroll_bindings[s][d] = tmp.bindings[0];
                    }
                }
            }
        }
    }

    // --- Scroll threshold ---
    gs->scroll_threshold_px = clamp_int(
        tp_json_get_int_def(gs_obj, "scrollThreshold", gs->scroll_threshold_px),
        SCROLL_THRESH_MIN, SCROLL_THRESH_MAX);

    // --- Scroll hold arrays ---
    TpJsonValue* hold_v = tp_json_obj_get(gs_obj, "scrollHoldV");
    if (hold_v != NULL) {
        int vlen = tp_json_arr_len(hold_v);
        for (int i = 0; i < vlen && i < PROFILE_SCROLL_SLOT_COUNT; i++) {
            TpJsonValue* val = tp_json_arr_get(hold_v, i);
            if (val != NULL && tp_json_is_bool(val)) {
                gs->scroll_hold_v[i] = tp_json_get_bool(val);
            }
        }
    }

    TpJsonValue* hold_h = tp_json_obj_get(gs_obj, "scrollHoldH");
    if (hold_h != NULL) {
        int hlen = tp_json_arr_len(hold_h);
        for (int i = 0; i < hlen && i < PROFILE_SCROLL_SLOT_COUNT; i++) {
            TpJsonValue* val = tp_json_arr_get(hold_h, i);
            if (val != NULL && tp_json_is_bool(val)) {
                gs->scroll_hold_h[i] = tp_json_get_bool(val);
            }
        }
    }
}

// ========== Controller parsing ==========

static void parse_controller(TpJsonValue* ctrl_obj, ProfileExternalController* out_ctrl) {
    if (ctrl_obj == NULL) return;
    memset(out_ctrl, 0, sizeof(ProfileExternalController));

    const char* id = tp_json_get_string_def(ctrl_obj, "id", "");
    strncpy(out_ctrl->id, id, sizeof(out_ctrl->id) - 1);
    out_ctrl->id[sizeof(out_ctrl->id) - 1] = '\0';

    const char* name = tp_json_get_string_def(ctrl_obj, "name", "");
    strncpy(out_ctrl->name, name, sizeof(out_ctrl->name) - 1);
    out_ctrl->name[sizeof(out_ctrl->name) - 1] = '\0';

    TpJsonValue* bindings_arr = tp_json_obj_get(ctrl_obj, "controllerBindings");
    if (bindings_arr == NULL) return;

    int len = tp_json_arr_len(bindings_arr);
    if (len > PROFILE_MAX_CONTROLLER_BINDINGS) len = PROFILE_MAX_CONTROLLER_BINDINGS;

    for (int i = 0; i < len; i++) {
        TpJsonValue* cb = tp_json_arr_get(bindings_arr, i);
        if (cb == NULL) continue;

        out_ctrl->bindings[out_ctrl->binding_count].key_code = tp_json_get_int_def(cb, "keyCode", 0);

        const char* bind_str = tp_json_get_string_def(cb, "binding", "NONE");
        if (bind_str != NULL && strcmp(bind_str, "NONE") != 0) {
            ProfileBind bind;
            if (profile_bind_from_string(bind_str, &bind) == 0) {
                out_ctrl->bindings[out_ctrl->binding_count].bind = bind;
                out_ctrl->binding_count++;
            }
        }
    }
}

// ========== Top-level loaders ==========

int profile_load_from_string(const char* json_str, ProfileData* out_profile) {
    if (json_str == NULL) {
        snprintf(g_last_error, sizeof(g_last_error), "NULL JSON string");
        return -1;
    }

    profile_data_init(out_profile);

    TpJsonValue* root = tp_json_parse(json_str);
    if (root == NULL) {
        snprintf(g_last_error, sizeof(g_last_error), "Failed to parse JSON");
        return -1;
    }

    // --- Basic fields ---
    out_profile->id = tp_json_get_int_def(root, "id", 0);

    const char* name = tp_json_get_string_def(root, "name", "");
    strncpy(out_profile->name, name, sizeof(out_profile->name) - 1);
    out_profile->name[sizeof(out_profile->name) - 1] = '\0';

    out_profile->cursor_speed = (float)tp_json_get_num_def(root, "cursorSpeed", (double)out_profile->cursor_speed);

    // --- Scalar settings ---
    out_profile->binding_delay_ms = clamp_int(
        tp_json_get_int_def(root, "bindingDelay", 0),
        BINDING_DELAY_MIN, PROFILE_DELAY_MAX);

    out_profile->long_press_delay_ms = tp_json_get_int_def(root, "longPressDelay", out_profile->long_press_delay_ms);

    out_profile->button_long_press_haptic = clamp_int(
        tp_json_get_int_def(root, "buttonLongPressHaptic", out_profile->button_long_press_haptic),
        HAPTIC_MIN, HAPTIC_MAX);

    out_profile->button_gesture_haptic = clamp_int(
        tp_json_get_int_def(root, "buttonGestureHaptic", out_profile->button_gesture_haptic),
        HAPTIC_MIN, HAPTIC_MAX);

    out_profile->gesture_long_press_haptic = clamp_int(
        tp_json_get_int_def(root, "gestureLongPressHaptic", out_profile->gesture_long_press_haptic),
        HAPTIC_MIN, HAPTIC_MAX);

    out_profile->scroll_bind_haptic = clamp_int(
        tp_json_get_int_def(root, "scrollBindHaptic", 0),
        HAPTIC_MIN, HAPTIC_MAX);

    out_profile->scroll_hold_haptic = clamp_int(
        tp_json_get_int_def(root, "scrollHoldHaptic", 0),
        HAPTIC_MIN, HAPTIC_MAX);

    out_profile->scroll_hold_threshold_px = clamp_int(
        tp_json_get_int_def(root, "scrollHoldThreshold", out_profile->scroll_hold_threshold_px),
        SCROLL_HOLD_THRESH_MIN, SCROLL_HOLD_THRESH_MAX);

    out_profile->drag_threshold_px = clamp_int(
        tp_json_get_int_def(root, "dragThreshold", out_profile->drag_threshold_px),
        DRAG_THRESH_MIN, DRAG_THRESH_MAX);

    out_profile->double_tap_distance_px = clamp_int(
        tp_json_get_int_def(root, "doubleTapDistance", out_profile->double_tap_distance_px),
        DOUBLE_TAP_DIST_MIN, DOUBLE_TAP_DIST_MAX);

    out_profile->gesture_threshold_px = clamp_int(
        tp_json_get_int_def(root, "gestureThreshold", out_profile->gesture_threshold_px),
        GESTURE_THRESH_MIN, GESTURE_THRESH_MAX);

    // Phantom delays promoted to profile-driven sources.
    out_profile->inter_bind_min_gap_ms = tp_json_get_int_def(root, "interBindMinGap", out_profile->inter_bind_min_gap_ms);
    out_profile->bind_press_width_ms = tp_json_get_int_def(root, "bindPressWidth", out_profile->bind_press_width_ms);
    out_profile->release_hold_min_gap_ms = tp_json_get_int_def(root, "releaseHoldMinGap", out_profile->release_hold_min_gap_ms);
    out_profile->range_tap_release_ms = tp_json_get_int_def(root, "rangeTapRelease", out_profile->range_tap_release_ms);
    out_profile->sim_click_press_ms = tp_json_get_int_def(root, "simClickPress", out_profile->sim_click_press_ms);
    out_profile->sim_click_release_ms = tp_json_get_int_def(root, "simClickRelease", out_profile->sim_click_release_ms);
    out_profile->gesture_swipe_min_ms = tp_json_get_int_def(root, "gestureSwipeMin", out_profile->gesture_swipe_min_ms);
    out_profile->toggle_debounce_ms = tp_json_get_int_def(root, "toggleDebounce", out_profile->toggle_debounce_ms);
    out_profile->idle_debounce_threshold = tp_json_get_int_def(root, "idleDebounceThreshold", out_profile->idle_debounce_threshold);
    out_profile->idle_interval_ms = clamp_int(
        tp_json_get_int_def(root, "idleIntervalMs", out_profile->idle_interval_ms),
        1, 10000);
    out_profile->default_auto_repeat_interval_ms = clamp_int(
        tp_json_get_int_def(root, "defaultAutoRepeatInterval", out_profile->default_auto_repeat_interval_ms),
        0, 100000);
    out_profile->range_scroll_threshold_px = clamp_int(
        tp_json_get_int_def(root, "rangeScrollThreshold", out_profile->range_scroll_threshold_px),
        1, 1000);
    out_profile->gesture_threshold_px_fallback = clamp_int(
        tp_json_get_int_def(root, "gestureThresholdFallback", out_profile->gesture_threshold_px_fallback),
        1, 1000);
    out_profile->cursor_accel_threshold = tp_json_get_int_def(root, "cursorAccelThreshold", out_profile->cursor_accel_threshold);
    out_profile->cursor_accel_factor = (float)tp_json_get_num_def(root, "cursorAccelFactor", (double)out_profile->cursor_accel_factor);
    out_profile->timer_interval_ms = tp_json_get_int_def(root, "touchTickRate", 0);

    out_profile->stroke_width = (float)tp_json_get_num_def(root, "strokeWidth", (double)out_profile->stroke_width);
    out_profile->fill_alpha_inactive = tp_json_get_int_def(root, "fillAlphaInactive", out_profile->fill_alpha_inactive);
    out_profile->corner_radius = (float)tp_json_get_num_def(root, "cornerRadius", (double)out_profile->corner_radius);

    // --- Elements ---
    TpJsonValue* elements_arr = tp_json_obj_get(root, "elements");
    if (elements_arr != NULL) {
        int elem_count = tp_json_arr_len(elements_arr);
        if (elem_count > PROFILE_MAX_ELEMENTS) elem_count = PROFILE_MAX_ELEMENTS;
        for (int i = 0; i < elem_count; i++) {
            TpJsonValue* elem_obj = tp_json_arr_get(elements_arr, i);
            if (elem_obj == NULL) continue;
            if (parse_element(elem_obj, &out_profile->elements[out_profile->element_count]) == 0) {
                out_profile->element_count++;
            }
        }
    }

    // --- Gesture settings ---
    // Try unified "gestureSettings" first
    TpJsonValue* gs_obj = tp_json_obj_get(root, "gestureSettings");
    if (gs_obj != NULL) {
        parse_gesture_settings(gs_obj, &out_profile->gesture_settings);
    } else {
        // Legacy: try "touchscreenGestures"
        TpJsonValue* legacy_gs = tp_json_obj_get(root, "touchscreenGestures");
        if (legacy_gs != NULL) {
            parse_gesture_settings(legacy_gs, &out_profile->gesture_settings);
        }
    }

    // --- Controllers ---
    TpJsonValue* controllers_arr = tp_json_obj_get(root, "controllers");
    if (controllers_arr != NULL) {
        int ctrl_count = tp_json_arr_len(controllers_arr);
        if (ctrl_count > PROFILE_MAX_CONTROLLERS) ctrl_count = PROFILE_MAX_CONTROLLERS;
        for (int i = 0; i < ctrl_count; i++) {
            TpJsonValue* ctrl_obj = tp_json_arr_get(controllers_arr, i);
            if (ctrl_obj == NULL) continue;
            parse_controller(ctrl_obj, &out_profile->controllers[out_profile->controller_count]);
            out_profile->controller_count++;
        }
    }

    // ------------------------------------------------------------------
    // PRONG A: Backward-compatible delay resolution for old profiles.
    // Old profiles saved before the Java sentinel fix have delays arrays
    // like [0, 0, 0] because -1 sentinels were unconditionally resolved
    // to 0.  This pass upgrades index > 0 zero delays to the profile-
    // level binding_delay_ms, matching Java's sentinel semantics.
    // ------------------------------------------------------------------
    {
        int dd = out_profile->binding_delay_ms;

        // Element slot packages (button slots, stick/dpad simple slots)
        for (int e = 0; e < out_profile->element_count; e++) {
            ProfileControlElement* elem = &out_profile->elements[e];
            for (int s = 0; s < PROFILE_MAX_BINDING_SLOTS; s++) {
                profile_bind_package_resolve_delays(&elem->slot_packages[s], dd);
            }
            profile_bind_package_resolve_delays(&elem->long_press_package, dd);
            profile_bind_package_resolve_delays(&elem->gesture_package, dd);
        }

        // Gesture action packages (10 gesture types)
        for (int g = 0; g < PROFILE_GESTURE_COUNT; g++) {
            profile_bind_package_resolve_delays(&out_profile->gesture_settings.gesture_actions[g], dd);
        }
    }

    tp_json_free(root);
    return 0;
}

int profile_load_from_file(const char* file_path, ProfileData* out_profile) {
    if (file_path == NULL) {
        snprintf(g_last_error, sizeof(g_last_error), "NULL file path");
        return -1;
    }

    FILE* f = fopen(file_path, "rb");
    if (f == NULL) {
        snprintf(g_last_error, sizeof(g_last_error), "Cannot open file: %s", file_path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize < 0) {
        fclose(f);
        snprintf(g_last_error, sizeof(g_last_error), "Cannot determine file size");
        return -1;
    }
    fseek(f, 0, SEEK_SET);

    char* buf = (char*)malloc((size_t)fsize + 1);
    if (buf == NULL) {
        fclose(f);
        snprintf(g_last_error, sizeof(g_last_error), "Out of memory");
        return -1;
    }

    size_t bytes_read = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[bytes_read] = '\0';

    int result = profile_load_from_string(buf, out_profile);
    free(buf);
    return result;
}
