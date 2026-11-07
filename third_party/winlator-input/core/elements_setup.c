// Element setup and geometry computation.
// Handles: touch_processor_set_elements, element geometry, snapping, resolution, xform.

#include "core/internal.h"
#include "core/activation_mode.h"

extern TouchProcessorState g_state;

#define DEFAULT_OPACITY 0.5f
#define DEFAULT_CORNER_RADIUS 0.8f

static void init_element_visuals(TouchElement* e) {
    e->visual_x = (float)e->x;
    e->visual_y = (float)e->y;
    e->visual_flags = 0;
    e->visual_layers = 0;
    e->visual_alpha = 0;
    e->flags &= ~(TOUCH_BIT_ENGAGED | TOUCH_BIT_SELECTED |
                  TOUCH_BIT_GESTURE_SUPPRESSED | TOUCH_BIT_CACHED_HAS_TOGGLE |
                  TOUCH_BIT_CACHED_HAS_AUTO_REPEAT | TOUCH_BIT_CACHED_HAS_LONG_PRESS |
                  TOUCH_BIT_CACHED_HAS_GESTURE | TOUCH_BIT_CACHED_HAS_ANY_BINDING |
                  TOUCH_BIT_IS_LOCK | TOUCH_BIT_TOGGLE_REPRESS_KILLS |
                  TOUCH_BIT_TOGGLE_SLIDE_OVER | TOUCH_BIT_IS_HOVER);
    e->current_ptr_id = -1;
    if (e->opacity <= 0.0f) e->opacity = DEFAULT_OPACITY;
    if (e->corner_radius <= 0.0f) e->corner_radius = DEFAULT_CORNER_RADIUS;
    if (e->color_primary == 0) e->color_primary = g_state.cfg.color_primary;
    if (e->color_secondary == 0) e->color_secondary = g_state.cfg.color_secondary;
    if (e->stroke_width <= 0.0f) e->stroke_width = g_state.cfg.stroke_width_default;
    if (e->fill_alpha_inactive <= 0) e->fill_alpha_inactive = g_state.cfg.fill_alpha_inactive_default;
}

static void compute_element_caps(TouchElement* e, int index) {
    if (e->activation_mode == TRACK || e->activation_mode == HOVER)
        g_state.cfg.caps_has_track_hover_buttons = true;

    for (int k = 0; k < MAX_BINDINGS_PER_ELEMENT; k++) {
        if (e->bindings[k].flags & TOUCH_BINDING_FLAG_TOGGLE)
            e->flags |= TOUCH_BIT_CACHED_HAS_TOGGLE;
        if (e->bindings[k].flags & TOUCH_BINDING_FLAG_AUTO_REPEAT)
            e->flags |= TOUCH_BIT_CACHED_HAS_AUTO_REPEAT;
    }
    if (e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE)
        g_state.cfg.caps_has_element_toggle = true;
    if (ELEM_LP_COUNT(index) > 0 && ELEM_LP(index)[0].type != NONE)
        g_state.cfg.caps_has_any_element_long_press = true;
    if (ELEM_GEST_COUNT(index) > 0 && ELEM_GEST(index)[0].type != NONE)
        g_state.cfg.caps_has_any_element_gesture = true;
    if (e->flags & TOUCH_BIT_CACHED_HAS_AUTO_REPEAT)
        g_state.cfg.caps_has_auto_repeat_buttons = true;
    if (e->bindings[0].type == MOUSE_LEFT_BUTTON)
        g_state.cfg.caps_has_mouse_left_element = true;
    if (e->flags & TOUCH_BIT_PASSTHROUGH_TOUCH)
        g_state.cfg.caps_has_passthrough_elements = true;
    activation_mode_apply_flags(e);
}

static bool bindings_array_has_toggle(const TouchBinding* arr, int count) {
    for (int k = 0; k < count; k++)
        if (arr[k].flags & TOUCH_BINDING_FLAG_TOGGLE) return true;
    return false;
}

static void cache_element_flags(TouchElement* e, int i) {
    int _ei = i;
    if (ELEM_LP_COUNT(_ei) > 0 && ELEM_LP(_ei)[0].type != NONE)
        e->flags |= TOUCH_BIT_CACHED_HAS_LONG_PRESS;
    if (ELEM_GEST_COUNT(_ei) > 0 && ELEM_GEST(_ei)[0].type != NONE)
        e->flags |= TOUCH_BIT_CACHED_HAS_GESTURE;
    e->cached_petal_mask = 0;
    for (int k = 0; k < MAX_BINDINGS_PER_ELEMENT; k++)
        if (e->bindings[k].type != NONE)
            e->cached_petal_mask |= (1 << k);
    if (e->cached_petal_mask != 0)
        e->flags |= TOUCH_BIT_CACHED_HAS_ANY_BINDING;
    e->cached_auto_repeat_interval_ticks = g_state.cfg.default_auto_repeat_interval_ticks;
    for (int k = 0; k < MAX_BINDINGS_PER_ELEMENT; k++) {
        if (((e->bindings[k].flags & TOUCH_BINDING_FLAG_TOGGLE) || (e->bindings[k].flags & TOUCH_BINDING_FLAG_AUTO_REPEAT)) && e->bindings[k].auto_repeat_interval_ticks > 0) {
            e->cached_auto_repeat_interval_ticks = e->bindings[k].auto_repeat_interval_ticks;
            break;
        }
    }
    if (bindings_array_has_toggle(ELEM_LP(_ei), ELEM_LP_COUNT(_ei)))
        e->flags |= TOUCH_BIT_CACHED_LP_HAS_TOGGLE;
    if (bindings_array_has_toggle(ELEM_GEST(_ei), ELEM_GEST_COUNT(_ei)))
        e->flags |= TOUCH_BIT_CACHED_GESTURE_HAS_TOGGLE;
    if (is_gamepad_binding(&e->bindings[0]))
        e->flags |= TOUCH_BIT_CACHED_BIND0_IS_GAMEPAD;
    if (is_right_stick_binding(e))
        e->flags |= TOUCH_BIT_CACHED_BIND0_IS_RIGHT_STICK;
}

static void compute_half_wh(TouchElement* e, float hs) {
    e->hw = e->w * hs * 0.5f * e->scale;
    e->hh = e->h * hs * 0.5f * e->scale;
}

static void element_compute_snapped_hwhh(TouchElement* e, float snap) {
    float hs = snap;
    switch (e->type) {
        case D_PAD: e->hw = hs * DPAD_RADIUS_MULTIPLIER * e->scale; e->hh = hs * DPAD_RADIUS_MULTIPLIER * e->scale; break;
        case STICK:
        case TRACKPAD: e->hw = hs * STICK_RADIUS_MULTIPLIER * e->scale; e->hh = hs * STICK_RADIUS_MULTIPLIER * e->scale; break;
        case BUTTON:
            if (e->shape == CIRCLE) { e->hw = hs * BUTTON_CIRCLE_RADIUS_MULT * e->scale; e->hh = hs * BUTTON_CIRCLE_RADIUS_MULT * e->scale; }
            else { compute_half_wh(e, hs); }
            break;
        case RANGE_BUTTON:
            e->hw = hs * ((e->range_binding_count * RANGE_BINDING_WIDTH_SNAP) / 2) * e->scale;
            e->hh = hs * RANGE_HEIGHT_MULTIPLIER * e->scale;
            if (e->range_orientation == 1) { float tmp = e->hw; e->hw = e->hh; e->hh = tmp; }
            break;
        default: compute_half_wh(e, hs); break;
    }
}

static void compute_element_geometry(TouchElement* e, int i) {
    float hs_snap = g_state.snapping_size > 0.0f ? g_state.snapping_size : 1.0f;
    element_compute_snapped_hwhh(e, hs_snap);
    e->cached_left = (float)e->x - e->hw - 0.5f;
    e->cached_right = (float)e->x + e->hw + 0.5f;
    e->cached_top = (float)e->y - e->hh - 0.5f;
    e->cached_bottom = (float)e->y + e->hh + 0.5f;
    e->local_left   = -e->hw;
    e->local_top    = -e->hh;
    e->local_right  =  e->hw;
    e->local_bottom =  e->hh;
    e->cached_hw_sq = e->hw * e->hw;
    int idx = i;
    if (e->type == BUTTON && g_state.button_count < MAX_ELEMENTS)
        g_state.button_indices[g_state.button_count++] = idx;
    else if (e->type == RANGE_BUTTON && g_state.range_count < MAX_ELEMENTS)
        g_state.range_indices[g_state.range_count++] = idx;
    if (e->type == RANGE_BUTTON) {
        e->cached_range_cw = hs_snap * (e->range_binding_count * 2) * e->scale;
        e->cached_range_ch = hs_snap * RANGE_HEIGHT_MULTIPLIER * e->scale;
        if (e->range_orientation == 1) { float _t = e->cached_range_cw; e->cached_range_cw = e->cached_range_ch; e->cached_range_ch = _t; }
        int rbc = e->range_binding_count > 0 ? e->range_binding_count : 1;
        e->cached_range_element_size = (e->range_orientation == 0 ? e->cached_range_cw * 2.0f : e->cached_range_ch * 2.0f) / (float)rbc;
    }
}

typedef struct {
    uint32_t selected_mask[DIRTY_MASK_WORDS];
    uint32_t gesture_toggled_mask[DIRTY_MASK_WORDS];
    uint32_t lp_toggled_mask[DIRTY_MASK_WORDS];
} ToggleState;

static void save_toggle_state(ToggleState* state, int count) {
    memset(state, 0, sizeof(*state));
    for (int i = 0; i < count; i++) {
        int word = i / BITMASK_WORD_BITS, bit = i % BITMASK_WORD_BITS;
        if (g_state.elements[i].flags & TOUCH_BIT_SELECTED) state->selected_mask[word] |= (1u << bit);
        if (g_state.elements[i].flags & TOUCH_BIT_GESTURE_TOGGLED) state->gesture_toggled_mask[word] |= (1u << bit);
        if (g_state.elements[i].flags & TOUCH_BIT_LP_TOGGLED) state->lp_toggled_mask[word] |= (1u << bit);
    }
}

static void restore_toggle_state(const ToggleState* state, int count) {
    for (int i = 0; i < count; i++) {
        int word = i / BITMASK_WORD_BITS, bit = i % BITMASK_WORD_BITS;
        if ((state->selected_mask[word] >> bit) & 1)
            g_state.elements[i].flags |= TOUCH_BIT_SELECTED;
        else
            g_state.elements[i].flags &= ~TOUCH_BIT_SELECTED;
        if ((state->gesture_toggled_mask[word] >> bit) & 1)
            g_state.elements[i].flags |= TOUCH_BIT_GESTURE_TOGGLED;
        else
            g_state.elements[i].flags &= ~TOUCH_BIT_GESTURE_TOGGLED;
        if ((state->lp_toggled_mask[word] >> bit) & 1)
            g_state.elements[i].flags |= TOUCH_BIT_LP_TOGGLED;
        else
            g_state.elements[i].flags &= ~TOUCH_BIT_LP_TOGGLED;
    }
}

void touch_processor_set_elements(const TouchElement* elements, int count) {
    if (!elements) { return; }
    if (count <= 0) { return; }
    if (count > MAX_ELEMENTS) { }
    int n = count < MAX_ELEMENTS ? count : MAX_ELEMENTS;

    int old_count = g_state.element_count;
    int save_n = old_count < n ? old_count : n;
    ToggleState saved_toggle;
    save_toggle_state(&saved_toggle, save_n);

    ELEMENTS_LOCK();

    memcpy(g_state.elements, elements, n * sizeof(TouchElement));
    g_state.element_count = n;
    g_state.button_count = 0;
    g_state.range_count = 0;
    g_state.cfg.caps_has_track_hover_buttons = false;
    g_state.cfg.caps_has_element_toggle = false;
    g_state.cfg.caps_has_any_element_long_press = false;
    g_state.cfg.caps_has_any_element_gesture = false;
    g_state.cfg.caps_has_auto_repeat_buttons = false;
    g_state.cfg.caps_has_mouse_left_element = false;
    g_state.cfg.caps_has_passthrough_elements = false;
    int found_activation = LOCK;
    for (int i = 0; i < n; i++) {
        TouchElement* e = &g_state.elements[i];
        e->last_visual_key = UINT16_MAX;
        if (e->activation_mode > found_activation) found_activation = e->activation_mode;
        init_element_visuals(e);
        compute_element_caps(e, i);
        cache_element_flags(e, i);
        compute_element_geometry(e, i);
        mark_element_dirty(e);
    }
    g_state.activation_mode = found_activation;
    g_state.is_track = (g_state.activation_mode == TRACK);
    g_state.is_hover = (g_state.activation_mode == HOVER);
    restore_toggle_state(&saved_toggle, save_n);
    mark_all_dirty();
    for (int i = 0; i < n; i++) {
        update_visual_layers(&g_state.elements[i]);
    }
    build_spatial_grid();

    ELEMENTS_UNLOCK();
}

void touch_processor_set_snapping_size(float size) {
    g_state.snapping_size = size;
    for (int i = 0; i < g_state.element_count; i++) {
        TouchElement* e = &g_state.elements[i];
        element_compute_snapped_hwhh(e, size);
        element_update_position_cache(e);
        mark_element_dirty(e);
    }
    mark_all_dirty();
    build_spatial_grid();
}

void touch_processor_set_resolution_scale(float scale) {
    g_state.resolution_scale = scale;
}

void touch_processor_set_sim_touch_screen(bool enabled) {
    g_state.sim_touch_screen = enabled;
}

void touch_processor_set_xform_scale(float scale_x, float scale_y) {
    g_state.cfg.xform_scale_x = scale_x > 0.0f ? scale_x : 1.0f;
    g_state.cfg.xform_scale_y = scale_y > 0.0f ? scale_y : 1.0f;
}

void touch_processor_set_view_offset(float offset_x, float offset_y) {
    g_state.cfg.view_offset_x = offset_x;
    g_state.cfg.view_offset_y = offset_y;
}
