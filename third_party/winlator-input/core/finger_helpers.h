#ifndef FINGER_HELPERS_H
#define FINGER_HELPERS_H

#include "core/state.h"
#include "core/timer.h"

__attribute__((always_inline)) static inline TouchFinger* find_finger(int ptr_id) {
    if (__builtin_expect(ptr_id < 0 || ptr_id >= MAX_POINTER_ID, 0)) return NULL;
    return g_state.finger_by_ptr_id[ptr_id];
}

static inline TouchFinger* find_free_finger(void) {
    if (g_state.free_finger_hint >= 0 && g_state.free_finger_hint < MAX_FINGERS
        && !g_state.fingers[g_state.free_finger_hint].active)
        return &g_state.fingers[g_state.free_finger_hint];
    for (int i = 0; i < MAX_FINGERS; i++)
        if (!g_state.fingers[i].active) {
            g_state.free_finger_hint = i;
            return &g_state.fingers[i];
        }
    return NULL;
}

static inline int active_finger_count(void) {
    return g_state.active_finger_count;
}

__attribute__((always_inline)) static inline int finger_index(const TouchFinger* f) {
    int idx = (int)(f - g_state.fingers);
    if (__builtin_expect(idx < 0 || idx >= MAX_FINGERS, 0)) return -1;
    return idx;
}

__attribute__((always_inline)) static inline void deactivate_finger(TouchFinger* f) {
    if (__builtin_expect(!f->active, 0)) return;
    g_state.finger_by_ptr_id[f->ptr_id] = NULL;
    g_state.active_finger_count--;
    f->active = false;
    f->gesture_activated_in_touch = false;
    f->scroll_mode = false;
    f->scroll_hold_v_state = 0;
    f->scroll_hold_h_state = 0;
    f->scroll_normal_held_dir = -1;
    f->scroll_gesture_index = -1;
    _Pragma("GCC unroll 4")
    for (int _d = 0; _d < SCROLL_DIR_COUNT; _d++)
        f->scroll_accum[_d] = 0.0f;
    f->pending_scroll_mode = false;
}

static inline void reset_finger_tap_state(TouchFinger* f) {
    f->cached_has_long_press_timer = false;
    f->single_tap_hold_delay_ticks = 0;
    f->single_tap_hold_timer_tick = 0;
    f->cached_has_moved_beyond_threshold = false;
}

static inline uint64_t now_tick(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int ti = g_timer_interval_ms > 0 ? g_timer_interval_ms : 16;
    return ((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000u) / (uint64_t)ti;
}

__attribute__((always_inline)) static inline void touch_transform_coords(float x, float y, int* out_x, int* out_y) {
    *out_x = (int)((x - g_state.cfg.view_offset_x) * g_state.cfg.xform_scale_x);
    *out_y = (int)((y - g_state.cfg.view_offset_y) * g_state.cfg.xform_scale_y);
}

void touch_finger_cache_bindings_state(TouchFinger* f);

#endif
