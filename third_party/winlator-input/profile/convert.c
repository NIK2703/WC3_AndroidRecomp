#include "convert.h"
#include "loader.h"
#include "core/state.h"
#include "core/timer.h"
#include <string.h>
#define TAG "ProfileConv"

// Helper: tick interval with fallback for uninitialized state.
static inline int tick_interval_ms(void) {
    int ti = g_timer_interval_ms;
    return ti > 0 ? ti : 16;
}

// Helper: ceiling division for ms→ticks conversion.
static inline uint16_t ms_to_ticks(int ms) {
    if (ms <= 0) return 0;
    int ti = tick_interval_ms();
    return (uint16_t)(((unsigned)ms + ti - 1u) / (unsigned)ti);
}



// ------------------------------------------------------------------
// Internal: map ProfileRange enum to range_max value
// ------------------------------------------------------------------
static int range_ordinal_to_max(int ordinal) {
    switch (ordinal) {
        case PROFILE_RANGE_A_TO_Z:  return 26;
        case PROFILE_RANGE_0_TO_9:  return 10;
        case PROFILE_RANGE_F1_TO_F12: return 12;
        case PROFILE_RANGE_NP0_TO_NP9: return 10;
        default: return 0;
    }
}

// ------------------------------------------------------------------
// Internal: convert a single ProfileBind to TouchBinding
// ------------------------------------------------------------------
static void bind_to_touch_binding(const ProfileBind* src, TouchBinding* dst) {
    dst->type = (uint16_t)src->type_value;
    dst->keycode = (uint16_t)src->key_value;
    dst->modifiers = 0;
    dst->flags = 0;
    dst->auto_repeat_interval_ticks = 0;
    dst->bind_delay_ticks = 0;
}

// ------------------------------------------------------------------
// Internal: convert a ProfileBindPackage to GestureBindingSlot
// ------------------------------------------------------------------
static void bind_package_to_slot(const ProfileBindPackage* pkg, GestureBindingSlot* slot) {
    int count = pkg->count < MAX_DEFERRED_BINDINGS ? pkg->count : MAX_DEFERRED_BINDINGS;
    for (int i = 0; i < count; i++) {
        bind_to_touch_binding(&pkg->bindings[i], &slot->arr[i]);
        if (pkg->sticky[i]) slot->arr[i].modifiers = 1;
        if (pkg->toggle_switch) slot->arr[i].flags |= TOUCH_BINDING_FLAG_TOGGLE;
        if (pkg->auto_repeat) slot->arr[i].flags |= TOUCH_BINDING_FLAG_AUTO_REPEAT;
        if (pkg->auto_repeat) slot->arr[i].auto_repeat_interval_ticks = ms_to_ticks(pkg->auto_repeat_interval_ms);
        slot->arr[i].bind_delay_ticks = ms_to_ticks(pkg->bind_delays_ms[i]);
    }
    slot->count = (uint8_t)count;

    // Mark the last non-NONE bind as LAST_IN_PACKAGE so its RELEASE is
    // deferred to deactivation (finger UP / button release).
    if (count > 0) {
        int last_idx = count - 1;
        while (last_idx > 0 && slot->arr[last_idx].type == NONE) last_idx--;
        if (slot->arr[last_idx].type != NONE)
            slot->arr[last_idx].flags |= TOUCH_BINDING_FLAG_LAST_IN_PACKAGE;
    }
}

// ------------------------------------------------------------------
// Internal: convert a single ProfileBind to a count-1 GestureBindingSlot
// ------------------------------------------------------------------
static void scroll_bind_to_slot(const ProfileBind* src, GestureBindingSlot* slot) {
    if (src->type_value == 0 && src->key_value == 0) {
        slot->count = 0;
        return;
    }
    bind_to_touch_binding(src, &slot->arr[0]);
    slot->count = 1;
}

// ==================================================================
// profile_to_config
// ==================================================================
void profile_to_config(const ProfileData* profile, int screen_w, int screen_h,
                       float global_cursor_speed, TouchProcessorConfig* out_config) {
    memset(out_config, 0, sizeof(TouchProcessorConfig));

    // --- Mode ---
    out_config->touch_mode = (MouseMode)profile->gesture_settings.mouse_mode;

    // --- Timing ---
    out_config->long_press_timeout_ticks  = ms_to_ticks(profile->gesture_settings.long_press_timeout_ms);
    out_config->double_tap_timeout_ticks  = ms_to_ticks(profile->gesture_settings.double_tap_timeout_ms);
    out_config->drag_threshold_px      = profile->drag_threshold_px;
    out_config->double_tap_distance_px = profile->double_tap_distance_px;
    out_config->binding_delay_ticks    = ms_to_ticks(profile->binding_delay_ms);
    out_config->long_press_delay_ticks    = ms_to_ticks(profile->long_press_delay_ms);
    out_config->gesture_threshold_px   = profile->gesture_threshold_px;

    // --- Phantom delays promoted to profile-driven fields (was hardcoded) ---
    out_config->inter_bind_min_gap_ticks        = ms_to_ticks(profile->inter_bind_min_gap_ms);
    out_config->bind_press_width_ticks          = ms_to_ticks(profile->bind_press_width_ms);
    out_config->release_hold_min_gap_ticks      = ms_to_ticks(profile->release_hold_min_gap_ms);
    out_config->range_tap_release_ticks         = ms_to_ticks(profile->range_tap_release_ms);
    out_config->sim_click_press_ticks           = ms_to_ticks(profile->sim_click_press_ms);
    out_config->sim_click_release_ticks         = ms_to_ticks(profile->sim_click_release_ms);
    out_config->gesture_swipe_min_ticks         = ms_to_ticks(profile->gesture_swipe_min_ms);
    out_config->toggle_debounce_ticks           = ms_to_ticks(profile->toggle_debounce_ms);
    out_config->idle_debounce_threshold         = profile->idle_debounce_threshold;
    out_config->default_auto_repeat_interval_ticks = ms_to_ticks(profile->default_auto_repeat_interval_ms);
    out_config->range_scroll_threshold_px       = profile->range_scroll_threshold_px;
    out_config->gesture_threshold_px_fallback   = profile->gesture_threshold_px_fallback;
    out_config->idle_interval_ms                = profile->idle_interval_ms;
    out_config->timer_interval_ms               = profile->timer_interval_ms;

    // --- Cursor ---
    out_config->cursor_speed = (int)(profile->cursor_speed * global_cursor_speed * 100);
    out_config->cursor_acceleration_threshold = profile->cursor_accel_threshold;
    out_config->cursor_acceleration_factor = profile->cursor_accel_factor;
    out_config->screen_w = screen_w;
    out_config->screen_h = screen_h;

    // --- Gesture bindings ---
    for (int i = 0; i < PROFILE_GESTURE_COUNT; i++) {
        bind_package_to_slot(&profile->gesture_settings.gesture_actions[i],
                             &out_config->gestures[i]);
    }

    // Touchpad: replace single-tap-drag with a NONE binding (slot stays active, no drag)
    if (out_config->touch_mode == TOUCHPAD) {
        out_config->gestures[PROFILE_GESTURE_SINGLE_TAP_DRAG].arr[0].type = NONE;
        out_config->gestures[PROFILE_GESTURE_SINGLE_TAP_DRAG].arr[0].keycode = 0;
        out_config->gestures[PROFILE_GESTURE_SINGLE_TAP_DRAG].count = 1;
    }

    // --- Scroll mode bindings ---
    for (int s = 0; s < PROFILE_SCROLL_SLOT_COUNT && s < GESTURE_SLOT_COUNT; s++) {
        for (int d = 0; d < PROFILE_SCROLL_DIR_COUNT; d++) {
            scroll_bind_to_slot(&profile->gesture_settings.scroll_bindings[s][d],
                                &out_config->scroll_bindings[s].dirs[d]);
        }
        out_config->scroll_hold_v[s] = profile->gesture_settings.scroll_hold_v[s];
        out_config->scroll_hold_h[s] = profile->gesture_settings.scroll_hold_h[s];
    }
    out_config->scroll_threshold_px = profile->gesture_settings.scroll_threshold_px;
    out_config->scroll_hold_threshold_px = profile->scroll_hold_threshold_px;

    // --- Haptic ---
    out_config->gesture_long_press_haptic = profile->gesture_long_press_haptic;
    out_config->scroll_bind_haptic       = profile->scroll_bind_haptic;
    out_config->scroll_hold_haptic       = profile->scroll_hold_haptic;

    // --- Render defaults (xform/view set by Java later) ---
    out_config->xform_scale_x = 1.0f;
    out_config->xform_scale_y = 1.0f;
    out_config->view_offset_x = 0.0f;
    out_config->view_offset_y = 0.0f;
    out_config->color_primary             = 0xFFFFFFFF;
    out_config->color_secondary           = 0xFF0277BD;
    out_config->stroke_width_default      = profile->stroke_width;
    out_config->fill_alpha_inactive_default = profile->fill_alpha_inactive;

    // bindings_generation starts at 1 (matching touch_processor_init default)
    out_config->bindings_generation = 1;

    // --- Gesture caps (compute_gesture_caps called by touch_processor_init) ---
    // TouchProcessorConfig is fully populated; touch_processor_init will
    // memcpy it into g_state.cfg and call compute_gesture_caps() internally.
}

// ==================================================================
// profile_to_elements
// ==================================================================
int profile_to_elements(const ProfileData* profile, int screen_w, int screen_h,
                       TouchElement* out_elements, int max_elements) {
    int count = profile->element_count;
    if (count > max_elements) count = max_elements;
    if (count <= 0) return 0;

    memset(out_elements, 0, (size_t)count * sizeof(TouchElement));

    for (int i = 0; i < count; i++) {
        const ProfileControlElement* src = &profile->elements[i];
        TouchElement* dst = &out_elements[i];

        // --- Type / shape ---
        dst->type  = (uint8_t)src->type;
        dst->shape = (uint8_t)src->shape;

        // --- Position (convert from normalized 0-1 to pixel coords) ---
        dst->x = (int)(src->x * screen_w + 0.5f);
        dst->y = (int)(src->y * screen_h + 0.5f);
        dst->w = src->w;
        dst->h = src->h;
        dst->scale = src->scale;

        // --- Visual ---
        dst->corner_radius     = src->corner_radius;
        dst->dpad_corner_radius = src->dpad_corner_radius;
        if (src->passthrough_touch) dst->flags |= TOUCH_BIT_PASSTHROUGH_TOUCH;
        dst->activation_mode = (uint8_t)profile->gesture_settings.activation_mode;
        dst->opacity = src->opacity;

        // --- Display text ---
        {
            int text_len = 0;
            while (text_len < 31 && src->display_text[text_len]) text_len++;
            memcpy(dst->display_text, src->display_text, (size_t)text_len);
            dst->display_text[text_len] = '\0';
        }

        // --- Icon ---
        dst->icon_id = (int8_t)src->icon_id;

        // --- Haptic (from profile-level settings) ---
        dst->button_long_press_haptic = profile->button_long_press_haptic;
        dst->button_gesture_haptic   = profile->button_gesture_haptic;

        // --- Range-button specifics ---
        if (src->type == PROFILE_ELEM_RANGE_BUTTON) {
            dst->range_ordinal       = (int)src->range;
            dst->range_max           = range_ordinal_to_max((int)src->range);
            dst->range_binding_count = src->binding_count;
            dst->range_orientation   = src->orientation;
        }

        // --- Primary bindings (4 slots) ---
        dst->primary_sticky_mask = 0;
        if (src->type == PROFILE_ELEM_BUTTON) {
            // Button: read from slot_packages (first binding per slot)
            for (int j = 0; j < 4 && j < PROFILE_MAX_BINDING_SLOTS; j++) {
                const ProfileBindPackage* pkg = &src->slot_packages[j];
                if (pkg->count > 0) {
                    bind_to_touch_binding(&pkg->bindings[0], &dst->bindings[j]);
                    if (pkg->count > 0)
                        dst->bindings[j].bind_delay_ticks = ms_to_ticks(pkg->bind_delays_ms[0]);
                    dst->bindings[j].flags |= TOUCH_BINDING_FLAG_LAST_IN_PACKAGE;
                }
                // Sticky: check if any binding in this slot has sticky set
                for (int k = 0; k < pkg->count && k < PROFILE_MAX_BINDINGS_PER_SLOT; k++) {
                    if (pkg->sticky[k]) {
                        dst->primary_sticky_mask |= (1 << j);
                        break;
                    }
                }
                // Toggle / auto-repeat apply to the slot's single binding
                if (pkg->toggle_switch)
                    dst->bindings[j].flags |= TOUCH_BINDING_FLAG_TOGGLE;
                if (pkg->auto_repeat) {
                    dst->bindings[j].flags |= TOUCH_BINDING_FLAG_AUTO_REPEAT;
                    dst->bindings[j].auto_repeat_interval_ticks = ms_to_ticks(pkg->auto_repeat_interval_ms);
                }
            }

            // --- Element long-press / gesture bindings (stored in g_state) ---
            {
                const ProfileBindPackage* lp_pkg = &src->long_press_package;
                int lp_count = lp_pkg->count < MAX_DEFERRED_BINDINGS ? lp_pkg->count : MAX_DEFERRED_BINDINGS;
                g_state.g_element_long_press_count[i] = (uint8_t)lp_count;
                for (int k = 0; k < lp_count; k++) {
                    bind_to_touch_binding(&lp_pkg->bindings[k], &g_state.g_element_long_press[i][k]);
                    if (lp_pkg->toggle_switch)
                        g_state.g_element_long_press[i][k].flags |= TOUCH_BINDING_FLAG_TOGGLE;
                    g_state.g_element_long_press[i][k].bind_delay_ticks = ms_to_ticks(lp_pkg->bind_delays_ms[k]);
                }
                if (lp_count > 0)
                    g_state.g_element_long_press[i][lp_count - 1].flags |= TOUCH_BINDING_FLAG_LAST_IN_PACKAGE;
            }
            {
                const ProfileBindPackage* gst_pkg = &src->gesture_package;
                int gst_count = gst_pkg->count < MAX_DEFERRED_BINDINGS ? gst_pkg->count : MAX_DEFERRED_BINDINGS;
                g_state.g_element_gesture_count[i] = (uint8_t)gst_count;
                for (int k = 0; k < gst_count; k++) {
                    bind_to_touch_binding(&gst_pkg->bindings[k], &g_state.g_element_gesture[i][k]);
                    if (gst_pkg->toggle_switch)
                        g_state.g_element_gesture[i][k].flags |= TOUCH_BINDING_FLAG_TOGGLE;
                    g_state.g_element_gesture[i][k].bind_delay_ticks = ms_to_ticks(gst_pkg->bind_delays_ms[k]);
                }
                if (gst_count > 0)
                    g_state.g_element_gesture[i][gst_count - 1].flags |= TOUCH_BINDING_FLAG_LAST_IN_PACKAGE;
            }
        } else {
            // Non-button: read from binds array directly
            for (int j = 0; j < 4 && j < PROFILE_MAX_BINDING_SLOTS; j++) {
                if (src->binds[j].type_value != 0 || src->binds[j].key_value != 0) {
                    bind_to_touch_binding(&src->binds[j], &dst->bindings[j]);
                    if (src->toggle_switch)
                        dst->bindings[j].flags |= TOUCH_BINDING_FLAG_TOGGLE;
                    dst->bindings[j].flags |= TOUCH_BINDING_FLAG_LAST_IN_PACKAGE;
                }
            }
        }

        // --- Runtime init ---
        dst->current_ptr_id = -1;
        for (int p = 0; p < MAX_PETALS; p++) dst->petal_active[p] = false;
    }

    return count;
}

// ==================================================================
// profile_load_and_init
// ==================================================================
int profile_load_and_init(const char* file_path, int screen_w, int screen_h, float cursor_speed) {
    ProfileData profile;
    memset(&profile, 0, sizeof(profile));

    
    if (profile_load_from_file(file_path, &profile) != 0) {
        
        return -1;
    }
    
    // Tick interval from profile (or default 16ms).
    g_timer_interval_ms = profile.timer_interval_ms > 0 ? profile.timer_interval_ms : 16;

    // --- Convert config and init processor ---
    TouchProcessorConfig config;
    profile_to_config(&profile, screen_w, screen_h, cursor_speed, &config);
    
    touch_processor_init(&config);
    

    // --- Set snapping size before element geometry computation ---
    // touch_processor_init() zeroes g_state, so snapping_size must be set
    // before touch_processor_set_elements() calls compute_element_geometry().
    int snap = screen_w / 100;
    if (snap <= 0) snap = 10;
    g_state.snapping_size = (float)snap;
    

    // --- Convert elements ---
    TouchElement elements[MAX_ELEMENTS];
    int elem_count = profile_to_elements(&profile, screen_w, screen_h, elements, MAX_ELEMENTS);
    
    if (elem_count > 0) {
        touch_processor_set_elements(elements, elem_count);
        
                                
    } else {
        
    }

    profile_data_free(&profile);
    return 0;
}

// ==================================================================
// profile_load_and_update_config
// ==================================================================
int profile_load_and_update_config(const char* file_path, int screen_w, int screen_h, float cursor_speed) {
    ProfileData profile;
    memset(&profile, 0, sizeof(profile));

    if (profile_load_from_file(file_path, &profile) != 0) {
        return -1;
    }

    // Tick interval from profile (or default 16ms).
    g_timer_interval_ms = profile.timer_interval_ms > 0 ? profile.timer_interval_ms : 16;

    TouchProcessorConfig config;
    profile_to_config(&profile, screen_w, screen_h, cursor_speed, &config);
    profile_data_free(&profile);

    config.xform_scale_x = g_state.cfg.xform_scale_x;
    config.xform_scale_y = g_state.cfg.xform_scale_y;
    config.view_offset_x = g_state.cfg.view_offset_x;
    config.view_offset_y = g_state.cfg.view_offset_y;

    touch_processor_update_config(&config);
    return 0;
}
