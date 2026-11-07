#ifndef ELEMENT_HELPERS_H
#define ELEMENT_HELPERS_H

#include "core/state.h"
#include <assert.h>
#include <math.h>
#include "tp_log.h"

__attribute__((always_inline)) static inline void add_action(TouchActionResult* restrict r, ActionType type, int param0, int param1, int param2) {
    int idx = r->count;
    if (__builtin_expect(idx >= MAX_TOUCH_RESULT_ACTIONS, 0)) {
        return;
    }
    r->count = idx + 1;
    TouchAction* act = &r->actions[idx];
    act->type = type;
    switch (type) {
        case ACT_POINTER_MOVE:
            act->pointer_move.x = param0;
            act->pointer_move.y = param1;
            break;
        case ACT_POINTER_MOVE_DELTA:
            act->pointer_delta.dx = param0;
            act->pointer_delta.dy = param1;
            break;
        case ACT_POINTER_BUTTON_PRESS:
        case ACT_POINTER_BUTTON_RELEASE:
            act->pointer_button.button = param0;
            break;
        case ACT_KEY_PRESS:
        case ACT_KEY_RELEASE:
            act->key.keycode = param0;
            act->key.is_down = param1;
            break;
        case ACT_MOUSE_EVENT:
            act->mouse_event.flags = param0;
            act->mouse_event.dx = param1;
            act->mouse_event.dy = param2;
            act->mouse_event.wheel = 0;
            break;
        case ACT_SCROLL:
            act->scroll.amount = param0;
            break;
        case ACT_START_MOUSE_MOVE:
            act->mouse_move.dx = param0;
            act->mouse_move.dy = param1;
            act->mouse_move.hold = param2;
            break;
        case ACT_STOP_MOUSE_MOVE:
            act->mouse_move.dx = 0;
            act->mouse_move.dy = 0;
            act->mouse_move.hold = 0;
            break;
        case ACT_GAMEPAD_STATE:
            act->key.keycode = param0;
            act->key.is_down = param1;
            break;
        case ACT_GAMEPAD_AXIS:
            act->gamepad_axis.is_left = param0;
            act->gamepad_axis.axis_x = param1;
            act->gamepad_axis.axis_y = param2;
            break;
        case ACT_HAPTIC:
            act->haptic.effect = param0;
            break;
        case ACT_SET_CURSOR_SPEED:
            act->cursor_speed.speed = param0;
            break;
        default:
            break;
    }
}

static inline __attribute__((always_inline)) bool element_is_toggle_active(const TouchElement* e) {
    return (e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE && e->flags & TOUCH_BIT_SELECTED)
        || (e->flags & TOUCH_BIT_LP_TOGGLED) || (e->flags & TOUCH_BIT_GESTURE_TOGGLED);
}

__attribute__((always_inline)) static inline void mark_element_dirty(TouchElement* e) {
    int idx = (int)(e - g_state.elements);
    if (__builtin_expect(idx < 0 || idx >= MAX_ELEMENTS, 0)) return;
    int word = idx >> 5;
    g_state.visual_dirty_mask[word] |= (1u << (idx & 31));
}

__attribute__((always_inline)) static inline void vis_set(TouchElement* e, uint8_t flag) {
    if (!(e->visual_flags & flag)) {
        e->visual_flags |= flag;
        mark_element_dirty(e);
    }
}

__attribute__((always_inline)) static inline void vis_clear(TouchElement* e, uint8_t flag) {
    if (e->visual_flags & flag) {
        e->visual_flags &= ~flag;
        mark_element_dirty(e);
    }
}

static inline void mark_all_dirty(void) {
    memset(g_state.visual_dirty_mask, 0xFF, sizeof(g_state.visual_dirty_mask));
}

static inline __attribute__((always_inline)) bool element_has_primary(const TouchElement* e) {
    return e->bindings[0].type != NONE;
}

static inline bool element_has_any_gesture_activity(const TouchElement* e) {
    return (e->visual_flags & (VF_GESTURE | VF_LONG_TAP)) != 0
        || (e->flags & TOUCH_BIT_GESTURE_TOGGLED) || (e->flags & TOUCH_BIT_LP_TOGGLED);
}

static inline __attribute__((always_inline)) bool element_has_gesture_toggle(const TouchElement* e) {
    return (e->flags & TOUCH_BIT_LP_TOGGLED) || (e->flags & TOUCH_BIT_GESTURE_TOGGLED);
}

static inline __attribute__((always_inline)) bool finger_gesture_active(const TouchElement* e) {
    if (e->current_ptr_id < 0 || e->current_ptr_id >= MAX_POINTER_ID) return false;
    TouchFinger* f = g_state.finger_by_ptr_id[e->current_ptr_id];
    return f != NULL && f->gesture_activated_in_touch;
}

static inline __attribute__((always_inline)) bool is_tracked(const TrackedButtons* tb, int idx) {
    if (__builtin_expect(idx < 0 || idx >= MAX_ELEMENTS, 0)) return false;
    return (tb->tracked_bitmask[idx >> 5] >> (idx & 31)) & 1u;
}

static inline void element_update_position_cache(TouchElement* e) {
    e->cached_left = e->x - e->hw;
    e->cached_right = e->x + e->hw;
    e->cached_top = e->y - e->hh;
    e->cached_bottom = e->y + e->hh;
    e->local_left   = -e->hw;
    e->local_top    = -e->hh;
    e->local_right  =  e->hw;
    e->local_bottom =  e->hh;
    e->cached_hw_sq = e->hw * e->hw;
}

static inline void element_get_relative_bbox(const TouchElement* e,
                                             float* l, float* t,
                                             float* r, float* b) {
    *l = e->local_left;
    *t = e->local_top;
    *r = e->local_right;
    *b = e->local_bottom;
}

__attribute__((always_inline)) static inline bool point_in_element(float px, float py, const TouchElement* e) {
    if (__builtin_expect(px < e->cached_left || px > e->cached_right, 0)) return false;
    if (__builtin_expect(py < e->cached_top || py > e->cached_bottom, 0)) return false;
    if (e->shape == CIRCLE) {
        float dx = px - e->x, dy = py - e->y;
        if (dx * dx + dy * dy > e->cached_hw_sq) return false;
    }
    return true;
}

// Java ControlElement.containsPoint(x, y): rectangle-only test with 0.5px tolerance
// (does NOT check CIRCLE shape — matches Java's Rect-based hit test)
static inline bool element_contains_point(float px, float py, const TouchElement* e) {
    return px >= e->cached_left - 0.5f && px <= e->cached_right + 0.5f &&
           py >= e->cached_top - 0.5f && py <= e->cached_bottom + 0.5f;
}

static inline void element_normalize_xy(float dx, float dy, float radius,
                                        float* restrict out_nx, float* restrict out_ny) {
    if (__builtin_expect(radius < NEAR_ZERO_THRESHOLD, 0)) { *out_nx = 0; *out_ny = 0; return; }
    float dist_sq = dx*dx + dy*dy;
    if (__builtin_expect(dist_sq < NEAR_ZERO_THRESHOLD * NEAR_ZERO_THRESHOLD, 0)) { *out_nx = 0; *out_ny = 0; return; }
    if (dist_sq > radius * radius) {
        float inv_dist = 1.0f / sqrtf(dist_sq);
        *out_nx = dx * inv_dist;
        *out_ny = dy * inv_dist;
    } else {
        float inv_radius = 1.0f / radius;
        *out_nx = dx * inv_radius;
        *out_ny = dy * inv_radius;
    }
}

__attribute__((always_inline)) static inline int element_index(const TouchElement* e) {
    int idx = (int)(e - g_state.elements);
    assert(idx >= 0 && idx < MAX_ELEMENTS);
    return idx;
}

static inline TouchBinding* ELEM_LP(int idx) { return g_state.g_element_long_press[idx]; }
static inline int ELEM_LP_COUNT(int idx) { return g_state.g_element_long_press_count[idx]; }
static inline TouchBinding* ELEM_GEST(int idx) { return g_state.g_element_gesture[idx]; }
static inline int ELEM_GEST_COUNT(int idx) { return g_state.g_element_gesture_count[idx]; }

static inline bool element_has_toggle(const TouchElement* e) { return e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE; }
static inline bool element_has_auto_repeat(const TouchElement* e) { return e->flags & TOUCH_BIT_CACHED_HAS_AUTO_REPEAT; }

static inline bool element_has_autorepeat_only(const TouchElement* e) {
    return (e->flags & TOUCH_BIT_CACHED_HAS_AUTO_REPEAT) && !(e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE);
}

// Java ControlElement.hasAnyAction(): true if any binding slot, long-press, or gesture is configured
static inline __attribute__((always_inline)) bool element_has_any_action(const TouchElement* e) {
    if (e->flags & TOUCH_BIT_CACHED_HAS_ANY_BINDING) return true;
    if (e->flags & TOUCH_BIT_CACHED_HAS_LONG_PRESS) return true;
    if (e->flags & TOUCH_BIT_CACHED_HAS_GESTURE) return true;
    return false;
}

static inline __attribute__((always_inline)) bool passthrough_blocks_move(const TouchElement* e) {
    return (e->flags & TOUCH_BIT_PASSTHROUGH_TOUCH) && !(e->flags & TOUCH_BIT_IS_HOVER);
}

static inline __attribute__((always_inline)) void arm_toggle_ar_gesture_timer(TouchElement* e) {
    if ((e->flags & TOUCH_BIT_CACHED_HAS_TOGGLE) && (e->flags & TOUCH_BIT_CACHED_HAS_AUTO_REPEAT)
        && !(e->flags & TOUCH_BIT_CACHED_HAS_LONG_PRESS))
        e->flags |= TOUCH_BIT_GESTURE_TIMER_ARMED;
}

static inline void update_ts_pointer(float x, float y, TouchActionResult* restrict result) {
    float fx = (x - g_state.cfg.view_offset_x) * g_state.cfg.xform_scale_x;
    float fy = (y - g_state.cfg.view_offset_y) * g_state.cfg.xform_scale_y;
    g_state.ptr_x = fx;
    g_state.ptr_y = fy;
    add_action(result, ACT_POINTER_MOVE, (int)fx, (int)fy, 0);
}

static inline void tracked_set_bit(TrackedButtons* tb, int idx) {
    if (__builtin_expect(idx >= 0 && idx < MAX_ELEMENTS, 1))
        tb->tracked_bitmask[idx >> 5] |= (1u << (idx & 31));
}

static inline void tracked_clear_bit(TrackedButtons* tb, int idx) {
    if (__builtin_expect(idx >= 0 && idx < MAX_ELEMENTS, 1))
        tb->tracked_bitmask[idx >> 5] &= ~(1u << (idx & 31));
}

static inline void reset_tracked_slot(TrackedButtons* tb, int pid_slot) {
    tb->count = 0;
    memset(tb->tracked_bitmask, 0, sizeof(tb->tracked_bitmask));
    tb->ptr_id = INVALID_PTR_ID;
    g_state.hovered_element_per_ptr[pid_slot] = INVALID_PTR_ID;
}

static inline void reset_first_tracked_long_press(const TrackedButtons* tb) {
    if (tb->count >= 2) {
        TouchElement* first = &g_state.elements[tb->element_indices[0]];
        first->flags &= ~(TOUCH_BIT_LONG_PRESS_ARM | TOUCH_BIT_GESTURE_LONG_PRESS_TRIGGERED);
    }
}

static inline bool grid_neighbor_cells(float x, float y,
    int* min_c, int* max_c, int* min_r, int* max_r) {
    if (g_state.grid_cell_w <= 0.0f || g_state.grid_cell_h <= 0.0f) return false;
    if (x < g_state.grid_min_x || x > g_state.grid_max_x ||
        y < g_state.grid_min_y || y > g_state.grid_max_y) return false;
    int col = (int)((x - g_state.grid_min_x) * g_state.grid_inv_cell_w);
    int row = (int)((y - g_state.grid_min_y) * g_state.grid_inv_cell_h);
    *min_c = col > 0 ? col - 1 : 0;
    *max_c = col < GRID_COLS - 1 ? col + 1 : GRID_COLS - 1;
    *min_r = row > 0 ? row - 1 : 0;
    *max_r = row < GRID_ROWS - 1 ? row + 1 : GRID_ROWS - 1;
    return true;
}

static inline TouchElement* grid_cell_element_at(int cell_idx, int j) {
    if (j + 1 < g_state.grid_cell_start[cell_idx + 1])
        __builtin_prefetch(&g_state.elements[g_state.grid_cell_to_elems[j + 1]], 0, 1);
    return &g_state.elements[g_state.grid_cell_to_elems[j]];
}

// Element handler forward declarations
TouchElement* hit_test_element(float x, float y);
float cubic_bezier_interpolate_trackpad(float x);
void element_set_petals(TouchElement* e, float nx, float ny, float dead_zone, TouchActionResult* restrict result);
void release_element_bindings(TouchElement* e, TouchActionResult* restrict result, bool release_gestures);
void suppress_element_gestures(TouchElement* e, TouchActionResult* restrict result);
void release_non_toggle_gestures(TouchElement* e, TouchActionResult* restrict result);
void toggle_alternate_bindings(TouchElement* e, uint32_t bit, bool supports_toggle,
                               TouchBinding* bindings, int count, bool has_primary,
                               TouchActionResult* restrict result);

void handle_element_down(TouchElement* e, int ptr_id, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);
void handle_element_move(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);
void handle_element_up(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);

void element_button_down(TouchElement* e, int ptr_id, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);
void element_button_move(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);
void element_button_up(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);

void element_dpad_down(TouchElement* e, int ptr_id, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);
void element_dpad_move(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);
void element_dpad_up(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);

void element_stick_down(TouchElement* e, int ptr_id, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);
void element_stick_move(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);
void element_stick_up(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);

void element_trackpad_down(TouchElement* e, int ptr_id, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);
void element_trackpad_move(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);
void element_trackpad_up(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);

void element_range_button_down(TouchElement* e, int ptr_id, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);
void element_range_button_move(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);
void element_range_button_up(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);
int range_keycode(int ordinal, int index);

void handle_gesture_down(TouchFinger* f, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);
void handle_gesture_move(TouchFinger* f, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);
void handle_gesture_up(TouchFinger* f, float x, float y, uint64_t now_tick, TouchActionResult* restrict result);

void update_visual_layers(TouchElement* e);
void finish_visual_update(TouchElement* e);
void clear_element_gesture_flags(TouchElement* e, bool set_suppressed);
void release_toggled_alternate_bindings(TouchElement* e, TouchActionResult* restrict result, bool check_triggered);
void release_element_petals(TouchElement* e, TouchActionResult* restrict result);
void release_element_petals_and_axis(TouchElement* e, TouchActionResult* restrict result);
void force_release_element_toggles(TouchElement* e, TouchActionResult* restrict result);
bool finger_remove_engaged(TouchFinger* f, int16_t elem_idx);
TrackedButtons* get_tracked_buttons(int ptr_id);

void activation_reset(void);

static inline void start_drag_no_binding(TouchFinger* f, TouchActionResult* restrict result) {
    on_drag_start(f, result);
    f->state = GESTURE_STATE_DRAGGING;
}

static inline void mark_second_finger_dragging(TouchFinger* exclude, TouchActionResult* restrict result) {
    for (int _si = 0; _si < MAX_FINGERS; _si++) {
        TouchFinger* _sf = &g_state.fingers[_si];
        if (_sf->active && _sf->ptr_id == g_state.gesture_second_ptr_id && _sf != exclude) {
            start_drag_no_binding(_sf, result);
            break;
        }
    }
}

void process_scheduled_actions(TouchActionResult* restrict result, uint64_t now_tick);
void build_spatial_grid(void);
void element_reset_runtime(TouchElement* e);

// M28: Key-compute + early-return check for update_visual_layers.
// Copied from shared.c to provide a fast inline wrapper.
#define VISUAL_KEY_TOGGLE_SHIFT(f) (((f) & ELEM_TOGGLE_BITS_MASK) >> __builtin_ctz(TOUCH_BIT_LP_TOGGLED))

__attribute__((always_inline)) static inline void update_visual_layers_inline(TouchElement* e) {
    uint16_t key = (uint16_t)e->visual_flags
        | (((uint16_t)VISUAL_KEY_TOGGLE_SHIFT(e->flags) & 3u) << 3)
        | (((uint16_t)e->flags & (TOUCH_BIT_SELECTED | TOUCH_BIT_ENGAGED)) << 5)
        | ((uint16_t)e->type << 7);
    if (__builtin_expect(key == e->last_visual_key, 1)) return;
    update_visual_layers(e);
}

#endif
