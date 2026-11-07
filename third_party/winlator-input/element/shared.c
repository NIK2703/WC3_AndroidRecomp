#include "core/internal.h"
#include <float.h>


_Static_assert(MAX_ELEMENTS <= INT16_MAX, "element index fits in int16_t");

#define PREFETCH_DISTANCE 4
#define MAX_ENGAGED_ELEMENTS 16
// Must match dispatch tables: ELEMENT_DOWN_HANDLERS, ELEMENT_MOVE_HANDLERS, ELEMENT_UP_HANDLERS.
// Add an entry to each table and increment ELEM_TYPE_COUNT if adding new element types.
#define ELEM_TYPE_COUNT 5

static inline int get_grid_cell(float fx, float fy, float min_x, float min_y);

__attribute__((hot)) TouchElement* hit_test_element(float x, float y) {
    TouchElement* const restrict base = __builtin_assume_aligned(g_state.elements, 64);
    (void)base;

    // Use spatial grid if available
    if (g_state.grid_cell_w > 0.0f && g_state.grid_cell_h > 0.0f) {
        int min_c, max_c, min_r, max_r;
        if (grid_neighbor_cells(x, y, &min_c, &max_c, &min_r, &max_r)) {
            for (int r = max_r; r >= min_r; r--) {
                for (int c = max_c; c >= min_c; c--) {
                    int cell_idx = r * GRID_COLS + c;
                    for (int j = g_state.grid_cell_start[cell_idx]; j < g_state.grid_cell_start[cell_idx + 1]; j++) {
                        TouchElement* e = grid_cell_element_at(cell_idx, j);
                        if (point_in_element(x, y, e))
                            return e;
                    }
                }
            }
        }
    }
    
    // Fallback: linear scan
    for (int i = g_state.element_count - 1; i >= 0; i--) {
        TouchElement* e = &g_state.elements[i];
        if (i >= PREFETCH_DISTANCE)
            __builtin_prefetch(&g_state.elements[i - PREFETCH_DISTANCE], 0, 1);
        if (point_in_element(x, y, e))
            return e;
    }

    return NULL;
}

#define BEZIER_LUT_SIZE 256
#define BEZIER_SEARCH_ITERS 16
#define BEZIER_P0X 0.075f
#define BEZIER_P1X 0.45f
#define BEZIER_P1Y 0.95f
#define BEZIER_P2Y 0.95f

static float g_bezier_lut[BEZIER_LUT_SIZE + 1];

static inline int get_grid_cell(float fx, float fy, float min_x, float min_y) {
    int col = (int)((fx - min_x) * g_state.grid_inv_cell_w);
    if (col < 0) col = 0; else if (col >= GRID_COLS) col = GRID_COLS - 1;
    int row = (int)((fy - min_y) * g_state.grid_inv_cell_h);
    if (row < 0) row = 0; else if (row >= GRID_ROWS) row = GRID_ROWS - 1;
    return row * GRID_COLS + col;
}

static void init_bezier_lut(void) {
    for (int i = 0; i <= BEZIER_LUT_SIZE; i++) {
        float x_target = (float)i / (float)BEZIER_LUT_SIZE;
        float lo = 0.0f, hi = 1.0f;
        for (int j = 0; j < BEZIER_SEARCH_ITERS; j++) {
            float t = (lo + hi) * 0.5f;
            float omt = 1.0f - t;
            float bx = 3.0f * omt * omt * t * BEZIER_P0X + 3.0f * omt * t * t * BEZIER_P1X + t * t * t;
            if (bx < x_target) lo = t;
            else hi = t;
        }
        float t = (lo + hi) * 0.5f;
        float omt = 1.0f - t;
        float by = 3.0f * omt * omt * t * BEZIER_P1Y + 3.0f * omt * t * t * BEZIER_P2Y + t * t * t;
        g_bezier_lut[i] = by;
    }
}

// Eager init: constructor runs before any touch events
__attribute__((constructor)) static void _auto_init_bezier(void) { init_bezier_lut(); }

float cubic_bezier_interpolate_trackpad(float x) {
    float abs_x = fabsf(x);
    if (abs_x <= NEAR_ZERO_THRESHOLD) return 0.0f;
    if (abs_x >= 1.0f) return x > 0 ? 1.0f : -1.0f;
    float f = abs_x * (float)BEZIER_LUT_SIZE;
    int idx = (int)f;
    if (idx >= BEZIER_LUT_SIZE) return x > 0 ? 1.0f : -1.0f;
    float frac = f - (float)idx;
    float result = g_bezier_lut[idx] + frac * (g_bezier_lut[idx+1] - g_bezier_lut[idx]);
    return x > 0 ? result : -result;
}

void element_set_petals(TouchElement* e, float nx, float ny, float dead_zone, TouchActionResult* restrict result) {
    if (!(e->flags & TOUCH_BIT_CACHED_HAS_ANY_BINDING)) return;
    bool raw_up = ny <= -dead_zone;
    bool raw_right = nx >= dead_zone;
    bool raw_down = ny >= dead_zone;
    bool raw_left = nx <= -dead_zone;
    bool states[MAX_PETALS] = {raw_up, raw_right, raw_down, raw_left};
    uint8_t mask = e->cached_petal_mask;
    while (mask) {
        int i = __builtin_ctz(mask);
        mask &= mask - 1;
        const TouchBinding* b = &e->bindings[i];
        if (is_mouse_move_binding(b)) continue;
        bool active = states[i];
        if (active != e->petal_active[i]) {
            e->petal_active[i] = active;
            if (active) press_binding(result, b, true);
            else release_binding(result, b);
        }
    }
}

void finish_visual_update(TouchElement* e) {
    if (!e) return;
    // update_visual_layers() already calls mark_element_dirty() + touch_processor_regenerate_element()
    // when visual layers change. No need to duplicate that work here.
    update_visual_layers(e);
}



__attribute__((hot)) void handle_element_down(TouchElement* e, int ptr_id, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {

    // Elements always take priority: cancel any pending double-tap gesture
    // so the touch engages the element instead of triggering gesture bindings.
    if (__builtin_expect(g_state.dt_waiting_count > 0, 0)) {
        gesture_cancel_double_tap_wait(result);
    }
    if (__builtin_expect(e->current_ptr_id >= 0, 0)) {
        return;
    }
    if (e->type >= ELEM_TYPE_COUNT) return;
    e->current_ptr_id = ptr_id;
    e->down_x = x;
    e->down_y = y;
    e->down_tick = now_tick;
    e->flags |= TOUCH_BIT_ENGAGED;
    vis_set(e, VF_TAP);
    if (e->type == STICK || e->type == RANGE_BUTTON) {
        e->visual_x = x;
        e->visual_y = y;
    } else {
        e->visual_x = (float)e->x;
        e->visual_y = (float)e->y;
    }
    e->flags &= ~(TOUCH_BIT_LONG_PRESS_ARM | TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED |
                  TOUCH_BIT_GESTURE_SWIPE_TRIGGERED | TOUCH_BIT_GESTURE_TIMER_ARMED |
                  TOUCH_BIT_GESTURE_SUPPRESSED);
    vis_clear(e, VF_LONG_TAP | VF_GESTURE);
    update_visual_layers(e);

    switch (e->type) {
        case BUTTON: element_button_down(e, ptr_id, x, y, now_tick, result); break;
        case D_PAD: element_dpad_down(e, ptr_id, x, y, now_tick, result); break;
        case RANGE_BUTTON: element_range_button_down(e, ptr_id, x, y, now_tick, result); break;
        case STICK: element_stick_down(e, ptr_id, x, y, now_tick, result); break;
        case TRACKPAD: element_trackpad_down(e, ptr_id, x, y, now_tick, result); break;
        default: break;
    }

    int16_t elem_idx = (int16_t)(e - g_state.elements);
    TouchFinger* f = find_finger(ptr_id);
    if (__builtin_expect(f != NULL, 1)) {
        uint8_t cnt = f->engaged_elem_count;
        if (__builtin_expect(cnt < MAX_ENGAGED_ELEMENTS, 1)) {
            f->engaged_elem_indices[cnt] = elem_idx;
            f->engaged_elem_count = cnt + 1;
        }
    }
}

// WHEN: handle_element_up — clean up non-toggle gesture presses that shouldn't persist
void release_non_toggle_gestures(TouchElement* e, TouchActionResult* restrict result) {
    int _ei = element_index(e);
    if (__builtin_expect((e->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED) && !(e->flags & TOUCH_BIT_GESTURE_TOGGLED), 0))
        release_bindings_list(result, ELEM_GEST(_ei), ELEM_GEST_COUNT(_ei));
    if (__builtin_expect((e->flags & TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED) && !(e->flags & TOUCH_BIT_LP_TOGGLED), 0))
        release_bindings_list(result, ELEM_LP(_ei), ELEM_LP_COUNT(_ei));
}

void toggle_alternate_bindings(TouchElement* e, uint32_t bit, bool supports_toggle,
                               TouchBinding* bindings, int count, bool has_primary,
                               TouchActionResult* restrict result)
{
    (void)e; (void)has_primary;
    if (e->flags & bit) {
        release_bindings_list(result, bindings, count);
        e->flags &= ~bit;
    } else {
        press_bindings_list(result, bindings, count);
        if (supports_toggle) e->flags |= bit;
    }
}

// --- Visual layer computation (pure C, no Java transition) ---
// Deterministically computes which render layers are active from the 3 visual flags.
// Called after any visual_flags mutation on BUTTON elements.
__attribute__((hot)) void update_visual_layers(TouchElement* e) {
    uint16_t key = (uint16_t)e->visual_flags
        | (((uint16_t)VISUAL_KEY_TOGGLE_SHIFT(e->flags) & 3u) << 3)
        | (((uint16_t)e->flags & (TOUCH_BIT_SELECTED | TOUCH_BIT_ENGAGED)) << 5)
        | ((uint16_t)e->type << 7);
    if (__builtin_expect(key == e->last_visual_key, 1)) return;
    e->last_visual_key = key;
    if (e->type == BUTTON) {
        uint8_t vf = e->visual_flags;
        bool tap      = (vf & VF_TAP) != 0;
        bool lt       = (vf & VF_LONG_TAP) != 0;
        bool ges      = (vf & VF_GESTURE) != 0;
        bool selected = (e->flags & TOUCH_BIT_SELECTED) != 0;

        bool gest_toggled = (e->flags & TOUCH_BIT_GESTURE_TOGGLED) != 0;
        bool lp_toggled   = (e->flags & TOUCH_BIT_LP_TOGGLED) != 0;

        bool has_action = element_has_any_action(e);

        // gesture suppresses fill until toggled (matches reference)
        bool show_fill = has_action && (tap || selected)
            && !(ges && !gest_toggled)
            && !(lt && !selected)
            && !(lp_toggled && !selected)
            && !(gest_toggled && !selected);

        bool show_stroke = !show_fill
            && (tap || lt || gest_toggled || lp_toggled);

        // GLOW: gesture glow halo. Shows on active gesture OR gesture-toggle persistence.
        bool show_glow = ges || gest_toggled;

        // OUTER_STROKE: long-press outer ring
        bool show_outer = lt || lp_toggled;

        uint32_t layers = 0;
        if (show_fill)   layers |= VISUAL_LAYER_FILL;
        if (show_stroke) layers |= VISUAL_LAYER_STROKE_TEXT;
        if (show_glow)   layers |= VISUAL_LAYER_GLOW;
        if (show_outer)  layers |= VISUAL_LAYER_OUTER_STROKE;

        uint32_t old_layers = e->visual_layers;

        if (layers == old_layers) return;
        e->visual_layers = layers;


        // Pack visual layer alphas: bits[31:24]=outer, [23:16]=stroke, [15:8]=fill, [7:0]=glow
        uint8_t fill_a   = show_fill   ? 255 : 0;
        uint8_t stroke_a = show_stroke ? (uint8_t)255 : 0;
        uint8_t glow_a   = show_glow   ? (uint8_t)255 : 0;
        uint8_t outer_a  = show_outer  ? (uint8_t)255 : 0;

        e->visual_alpha = ((uint32_t)outer_a << 24) | ((uint32_t)stroke_a << 16)
                        | ((uint32_t)fill_a << 8) | (uint32_t)glow_a;


        mark_element_dirty(e);
        return;
    }

    // Non-button elements (DPAD, STICK, TRACKPAD, RANGE_BUTTON):
    // Always visible with FILL + STROKE. GLOW added when engaged and has_action (TRACKPAD only).
    // These render directly (not through the multi-layer cache system).
    bool engaged = (e->flags & TOUCH_BIT_ENGAGED) != 0 && element_has_any_action(e);
    uint32_t layers = VISUAL_LAYER_FILL | VISUAL_LAYER_STROKE_TEXT;
    if (engaged && e->type == TRACKPAD) layers |= VISUAL_LAYER_GLOW;

    uint32_t old_layers = e->visual_layers;
    if (layers == old_layers) return;
    e->visual_layers = layers;


    uint8_t fill_a   = 255;
    uint8_t stroke_a = 255;
    uint8_t glow_a   = (layers & VISUAL_LAYER_GLOW) ? 255 : 0;
    uint8_t outer_a  = 0;

    e->visual_alpha = ((uint32_t)outer_a << 24) | ((uint32_t)stroke_a << 16)
                    | ((uint32_t)fill_a << 8) | (uint32_t)glow_a;


    mark_element_dirty(e);
}

void clear_element_gesture_flags(TouchElement* e, bool set_suppressed) {
    e->flags &= ~(TOUCH_BIT_LONG_PRESS_ARM | TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED |
                  TOUCH_BIT_GESTURE_SWIPE_TRIGGERED | TOUCH_BIT_GESTURE_TIMER_ARMED);
    vis_clear(e, VF_LONG_TAP | VF_GESTURE);
    if (set_suppressed) e->flags |= TOUCH_BIT_GESTURE_SUPPRESSED;
}

// WHEN: toggle-held bindings need release when trigger ends (e.g. double-tap toggle off)
void release_toggled_alternate_bindings(TouchElement* e, TouchActionResult* restrict result, bool check_triggered) {
    int _ei = element_index(e);
    if ((!check_triggered || !(e->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED)) && (e->flags & TOUCH_BIT_GESTURE_TOGGLED)) {
        release_bindings_list(result, ELEM_GEST(_ei), ELEM_GEST_COUNT(_ei));
        e->flags &= ~TOUCH_BIT_GESTURE_TOGGLED;
        vis_clear(e, VF_GESTURE);
    }
    if ((!check_triggered || !(e->flags & TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED)) && (e->flags & TOUCH_BIT_LP_TOGGLED)) {
        release_bindings_list(result, ELEM_LP(_ei), ELEM_LP_COUNT(_ei));
        e->flags &= ~TOUCH_BIT_LP_TOGGLED;
        vis_clear(e, VF_LONG_TAP);
    }
    finish_visual_update(e);
}

static inline bool button_should_clear_hover_vtap(const TouchElement* e, float x, float y) {
    return e->type == BUTTON
        && (e->flags & TOUCH_BIT_IS_HOVER)
        && !(e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE)
        && !point_in_element(x, y, e)
        && !(e->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED)
        && !(e->flags & TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED)
        && !(e->flags & TOUCH_BIT_GESTURE_TOGGLED)
        && !(e->flags & TOUCH_BIT_LP_TOGGLED);
}

void release_element_petals(TouchElement* e, TouchActionResult* restrict result) {
    if (!e) return;
    uint8_t mask = e->cached_petal_mask;
    while (mask) {
        int i = __builtin_ctz(mask);
        mask &= mask - 1;
        if (__builtin_expect(e->petal_active[i], 0)) {
            e->petal_active[i] = false;
            if (!(e->primary_sticky_mask & (1 << i)))
                release_binding(result, &e->bindings[i]);
        }
    }
}



// Unlike release_toggled_alternate_bindings, this unconditionally releases ALL bindings and clears selected state.
// WHEN: unconditional full deselect — another element grabs exclusive focus
void force_release_element_toggles(TouchElement* e, TouchActionResult* restrict result) {
    int _ei = element_index(e);
    if (e->flags & TOUCH_BIT_SELECTED) {
        for (int k = 0; k < MAX_PETALS; k++) {
            if (e->bindings[k].type != NONE)
                release_binding(result, &e->bindings[k]);
        }
        e->flags &= ~TOUCH_BIT_SELECTED;
        vis_clear(e, VF_TAP);
    }
    if (e->flags & TOUCH_BIT_GESTURE_TOGGLED) {
        release_bindings_list(result, ELEM_GEST(_ei), ELEM_GEST_COUNT(_ei));
        e->flags &= ~TOUCH_BIT_GESTURE_TOGGLED;
        vis_clear(e, VF_GESTURE);
    }
    if (e->flags & TOUCH_BIT_LP_TOGGLED) {
        release_bindings_list(result, ELEM_LP(_ei), ELEM_LP_COUNT(_ei));
        e->flags &= ~TOUCH_BIT_LP_TOGGLED;
        vis_clear(e, VF_LONG_TAP);
    }
    finish_visual_update(e);
}

void release_element_petals_and_axis(TouchElement* e, TouchActionResult* restrict result) {
    if (!e) return;
    if (e->flags & TOUCH_BIT_CACHED_HAS_ANY_BINDING) {
        release_element_petals(e, result);
    }
    release_gamepad_axis(e, result);
}

bool finger_remove_engaged(TouchFinger* f, int16_t elem_idx) {
    for (uint8_t i = 0; i < f->engaged_elem_count; i++) {
        if (f->engaged_elem_indices[i] == elem_idx) {
            f->engaged_elem_indices[i] = f->engaged_elem_indices[--f->engaged_elem_count];
            return true;
        }
    }
    return false;
}

TrackedButtons* get_tracked_buttons(int ptr_id) {
    // NOTE: hash collision silently overwrites — acceptable for single-touch primary finger tracking
    TrackedButtons* tb = &g_state.tracked[(uint32_t)ptr_id % MAX_FINGERS];
    if (__builtin_expect(tb->ptr_id != ptr_id, 0)) {
        tb->count = 0;
        memset(tb->tracked_bitmask, 0, sizeof(tb->tracked_bitmask));
        tb->ptr_id = ptr_id;
    }
    return tb;
}

__attribute__((hot)) void handle_element_move(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {

    mark_element_dirty(e);
    if (__builtin_expect(e->type == RANGE_BUTTON, 0)) {
        // Range button visual_x/y tracks finger position for visual placement
        e->visual_x = x;
        e->visual_y = y;
    }
    switch (e->type) {
        case BUTTON: element_button_move(e, x, y, now_tick, result); break;
        case D_PAD: element_dpad_move(e, x, y, now_tick, result); break;
        case RANGE_BUTTON: element_range_button_move(e, x, y, now_tick, result); break;
        case STICK: element_stick_move(e, x, y, now_tick, result); break;
        case TRACKPAD: element_trackpad_move(e, x, y, now_tick, result); break;
        default: break;
    }
    // HOVER mode: non-toggle buttons are only visually active when finger is inside
    // (unless gesture/LP is active — see concept: button without primary transfers
    // visual activation to gesture/LP firing).
    // NOTE: Button is checked directly here (not via dispatch table) because this
    // is a post-dispatch visual-only rule that applies exclusively to buttons.
    // Other element types (dpad, stick, etc.) always keep VF_TAP during move.
    if ((e->flags & TOUCH_BIT_IS_HOVER) && button_should_clear_hover_vtap(e, x, y)) {
        vis_clear(e, VF_TAP);
        finish_visual_update(e);
    }
}

__attribute__((hot)) void handle_element_up(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {

    int release_ptr_id = e->current_ptr_id;
    switch (e->type) {
        case BUTTON: element_button_up(e, x, y, now_tick, result); break;
        case D_PAD: element_dpad_up(e, x, y, now_tick, result); break;
        case RANGE_BUTTON: element_range_button_up(e, x, y, now_tick, result); break;
        case STICK: element_stick_up(e, x, y, now_tick, result); break;
        case TRACKPAD: element_trackpad_up(e, x, y, now_tick, result); break;
        default: break;
    }
    e->current_ptr_id = -1;
    vis_clear(e, VF_TAP);
    release_non_toggle_gestures(e, result);
    e->flags &= ~(TOUCH_BIT_ENGAGED | TOUCH_BIT_GESTURE_TIMER_ARMED | TOUCH_BIT_GESTURE_SWIPE_TRIGGERED
                   | TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED);
    if (!(e->flags & TOUCH_BIT_GESTURE_TOGGLED)) vis_clear(e, VF_GESTURE);
    if (!(e->flags & TOUCH_BIT_LP_TOGGLED)) vis_clear(e, VF_LONG_TAP);
    update_visual_layers(e);
    if (release_ptr_id >= 0) {
        TouchFinger* f = find_finger(release_ptr_id);
        if (__builtin_expect(f != NULL, 1))
            finger_remove_engaged(f, (int16_t)(e - g_state.elements));
    }
}

// WHEN: finger slides off element during gesture detection — preserves toggle state
void suppress_element_gestures(TouchElement* e, TouchActionResult* restrict result) {
    int _ei = element_index(e);
    if (!(e->flags & TOUCH_BIT_LP_TOGGLED) && (e->flags & TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED)) {
        release_bindings_list(result, ELEM_LP(_ei), ELEM_LP_COUNT(_ei));
    }
    if (!(e->flags & TOUCH_BIT_GESTURE_TOGGLED) && (e->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED)) {
        release_bindings_list(result, ELEM_GEST(_ei), ELEM_GEST_COUNT(_ei));
    }
    clear_element_gesture_flags(e, true);
    update_visual_layers(e);
}

// WHEN: general teardown — releases primary binding (if not sticky/toggle-held) and
// optionally clears gesture state. Used by element-specific cleanup paths.
void release_element_bindings(TouchElement* e, TouchActionResult* restrict result, bool release_gestures) {
    if (!e) return;
    int release_ptr_id = e->current_ptr_id;
    e->current_ptr_id = -1;
    e->flags &= ~TOUCH_BIT_ENGAGED;
    if (release_ptr_id >= 0) {
        TouchFinger* f = find_finger(release_ptr_id);
        if (__builtin_expect(f != NULL, 1))
            finger_remove_engaged(f, (int16_t)(e - g_state.elements));
    }

    if (!(e->flags & TOUCH_BIT_CACHED_HAS_ANY_BINDING) && !(e->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED) && !(e->flags & TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED))
        return;
    if (e->bindings[0].type != NONE && !(e->primary_sticky_mask & 1)
        && !(((e->bindings[0].flags & TOUCH_BINDING_FLAG_TOGGLE) || (e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE)) && (e->flags & TOUCH_BIT_SELECTED)))
        release_binding(result, &e->bindings[0]);
    if (release_gestures) {
        release_non_toggle_gestures(e, result);
        clear_element_gesture_flags(e, false);
    } else {
        e->flags &= ~(TOUCH_BIT_LONG_PRESS_ARM | TOUCH_BIT_GESTURE_TIMER_ARMED);
        vis_clear(e, VF_LONG_TAP);
    }
    if (!element_is_toggle_active(e)) {
        vis_clear(e, VF_TAP);
    }
    mark_element_dirty(e);
    update_visual_layers(e);
}

void touch_finger_cache_bindings_state(TouchFinger* f) {
    const FingerBindings* b = &f->bindings;
    f->cached_has_active_single_tap = b->single_tap_count > 0;
    f->cached_has_active_double_tap = b->double_tap_count > 0;
    f->cached_has_active_long_press = b->long_press_count > 0;
    f->cached_has_active_single_tap_drag = b->single_tap_drag_count > 0;
    f->cached_has_active_long_press_drag = b->long_press_drag_count > 0;
    f->cached_has_active_double_tap_drag = b->double_tap_drag_count > 0;
    f->cached_has_long_press_timer = b->long_press_count > 0 || b->long_press_drag_count > 0;
}

void build_spatial_grid(void) {
    if (g_state.element_count == 0) {
        g_state.grid_cell_w = 0.0f;
        return;
    }
    
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = (float)g_state.cfg.screen_w;
    float max_y = (float)g_state.cfg.screen_h;
    
    float range_x = max_x - min_x;
    float range_y = max_y - min_y;
    if (range_x < 1.0f) range_x = 1.0f;
    if (range_y < 1.0f) range_y = 1.0f;
    
    g_state.grid_min_x = min_x;
    g_state.grid_min_y = min_y;
    g_state.grid_max_x = max_x;
    g_state.grid_max_y = max_y;
    g_state.grid_cell_w = range_x / GRID_COLS;
    g_state.grid_cell_h = range_y / GRID_ROWS;
    g_state.grid_inv_cell_w = 1.0f / g_state.grid_cell_w;
    g_state.grid_inv_cell_h = 1.0f / g_state.grid_cell_h;
    
    memset(g_state.spatial_grid_count, 0, sizeof(g_state.spatial_grid_count));
    for (int i = 0; i < g_state.element_count; i++) {
        TouchElement* e = &g_state.elements[i];
        int cell = get_grid_cell((float)e->x, (float)e->y, min_x, min_y);
        int cnt = g_state.spatial_grid_count[cell];
        if (cnt < MAX_ELEMENTS)
            g_state.spatial_grid_count[cell] = cnt + 1;
    }

    int offset = 0;
    for (int cell = 0; cell < GRID_COLS * GRID_ROWS; cell++) {
        g_state.grid_cell_start[cell] = offset;
        offset += g_state.spatial_grid_count[cell];
    }
    // grid_cell_start must have GRID_COLS * GRID_ROWS + 1 elements (sentinel at end)
    g_state.grid_cell_start[GRID_COLS * GRID_ROWS] = offset;

    uint8_t temp_count[GRID_COLS * GRID_ROWS];
    memset(temp_count, 0, sizeof(temp_count));
    for (int i = 0; i < g_state.element_count; i++) {
        TouchElement* e = &g_state.elements[i];
        int cell = get_grid_cell((float)e->x, (float)e->y, min_x, min_y);
        if (temp_count[cell] < MAX_ELEMENTS)
            g_state.grid_cell_to_elems[g_state.grid_cell_start[cell] + temp_count[cell]++] = i;
    }
}

__attribute__((cold)) void element_reset_runtime(TouchElement* e) {
    e->current_ptr_id = -1;
    e->flags &= ~(TOUCH_BIT_ENGAGED | TOUCH_BIT_SELECTED | TOUCH_BIT_GESTURE_SUPPRESSED);
    e->visual_flags = 0;

    // Bulk-clear gesture runtime flags
    // NOTE: TOUCH_BIT_DEFER_PRIMARY is intentionally NOT cleared here.
    // Matches original Java: "defer_primary is intentionally NOT zeroed here."
    e->flags &= ~(TOUCH_BIT_LONG_PRESS_ARM | TOUCH_BIT_GESTURE_SWIPE_TRIGGERED |
                   TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED | TOUCH_BIT_GESTURE_TIMER_ARMED |
                   TOUCH_BIT_LP_TOGGLED | TOUCH_BIT_GESTURE_TOGGLED |
                   TOUCH_BIT_AUTO_REPEAT_PRIMARY_PRESSED);

    e->auto_repeat_last_tick = 0;
    e->toggle_debounce_last_tick = 0;
    e->stick_value_x = 0.0f;
    e->stick_value_y = 0.0f;
    e->trackpad_vel_x = 0.0f;
    e->trackpad_vel_y = 0.0f;
    e->range_scrolling = false;
    e->range_hold_pressed = false;
    e->range_pending_tap_release = false;
    memset(e->petal_active, 0, sizeof(e->petal_active));
    update_visual_layers(e);
}

