// Touch input processing: finger down/move/up.
// Handles: finger_init, touch_processor_on_finger_down/move/up.

#include "core/internal.h"
#include "core/activation_mode.h"
#include "tp_log.h"
#include <assert.h>

extern TouchProcessorState g_state;
int g_current_finger_idx = -1;

static void finger_reset_state(TouchFinger* f) {
    f->state = GESTURE_STATE_IDLE;
    f->engaged_elem_count = 0;
    f->gesture_activated_in_touch = false;
    f->cached_has_moved_beyond_threshold = false;
    f->scroll_mode = 0;
    memset(f->scroll_accum, 0, sizeof(f->scroll_accum));
    f->is_second_finger = false;
    f->pending_resume_action_count = 0;
    f->double_tap_original_id_set = false;
}

static void finger_init(TouchFinger* f, int ptr_id) {
    if (ptr_id < 0 || ptr_id >= MAX_POINTER_ID) return;
    assert(!g_state.finger_by_ptr_id[ptr_id]);
    memset(f, 0, sizeof(TouchFinger));
    f->ptr_id = ptr_id;
    f->active = true;
    f->bindings_generation = 0;
    f->is_tap = true;
    g_state.finger_by_ptr_id[ptr_id] = f;
    g_state.active_finger_count++;
}

__attribute__((hot)) void touch_processor_on_finger_down(TouchActionResult* restrict result, int ptr_id, float x, float y, uint64_t now_tick) {
    wc3_timer_registry_invalidate();
    result->count = 0;
    TouchFinger* f = find_finger(ptr_id);
    if (!f) {
        f = find_free_finger();
        if (!f) {
            return;
        }
        finger_init(f, ptr_id);
    } else {
        finger_reset_state(f);
    }

    FingerBindings* fb = &f->bindings;
    uint32_t bindings_gen = g_state.cfg.bindings_generation;
    if (__builtin_expect(f->bindings_generation != bindings_gen, 0)) {
        setup_main_finger_bindings(fb);
        f->bindings_generation = bindings_gen;
        touch_finger_cache_bindings_state(f);
    }

    g_current_finger_idx = finger_index(f);
    f->x = x; f->y = y;
    f->down_x = x; f->down_y = y;
    f->last_x = x; f->last_y = y;
    f->sub_pixel_x = 0; f->sub_pixel_y = 0;
    f->down_tick = now_tick;
    f->travel_x = 0; f->travel_y = 0;
    f->is_tap = true;
    handle_gesture_down(f, x, y, now_tick, result);
    g_current_finger_idx = -1;
}

__attribute__((hot)) void touch_processor_on_finger_move(TouchActionResult* restrict result, int ptr_id, float x, float y, uint64_t now_tick) {
    wc3_timer_registry_invalidate();
    result->count = 0;
    TouchFinger* f = find_finger(ptr_id);
    if (__builtin_expect(!f, 0)) {
        return;
    }

    g_current_finger_idx = finger_index(f);
    if (f->is_tap) {
        f->travel_x += fabsf(x - f->x);
        f->travel_y += fabsf(y - f->y);
        if (__builtin_expect(f->travel_x > MAX_TAP_TRAVEL, 0) || __builtin_expect(f->travel_y > MAX_TAP_TRAVEL, 0))
            f->is_tap = false;
    }
    f->x = x; f->y = y;
    handle_gesture_move(f, x, y, now_tick, result);
    f->last_x = x; f->last_y = y;
    g_current_finger_idx = -1;
}

__attribute__((hot)) void touch_processor_on_finger_up(TouchActionResult* restrict result, int ptr_id, float x, float y, uint64_t now_tick) {
    wc3_timer_registry_invalidate();
    result->count = 0;
    TouchFinger* f = find_finger(ptr_id);
    if (__builtin_expect(!f, 0)) {
        return;
    }

    int f_idx = finger_index(f);
    g_current_finger_idx = f_idx;
    f->x = x; f->y = y;
    handle_gesture_up(f, x, y, now_tick, result);

    // On finger UP: let the timer process remaining scheduled actions naturally
    // so per-bind delays are respected. Only intervene for double-tap scenarios
    // where pending actions from the first tap must be cancelled.
    bool dt_pending = f->state == GESTURE_STATE_DOUBLE_TAP_WAITING;
    if (g_state.scheduled_action_count > 0 && dt_pending) {
        int n_sched = g_state.scheduled_action_count < MAX_SCHEDULED_ACTIONS ? g_state.scheduled_action_count : MAX_SCHEDULED_ACTIONS;
        int remaining = 0;
        for (int i = 0; i < n_sched; i++) {
            ScheduledAction* sa = &g_state.scheduled_actions[i];
            if (sa->active && (sa->finger_idx < 0 || sa->finger_idx == f_idx)) {
                sa->active = false;
            } else if (sa->active) {
                remaining++;
            }
        }
        g_state.scheduled_action_count = remaining;
    }

    if (__builtin_expect(f->state == GESTURE_STATE_DOUBLE_TAP_WAITING, 0)) {
        g_current_finger_idx = -1;
        return;
    }
    deactivate_finger(f);
    g_state.free_finger_hint = f_idx;
    g_current_finger_idx = -1;
}
