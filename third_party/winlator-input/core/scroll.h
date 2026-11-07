#ifndef TP_SCROLL_H
#define TP_SCROLL_H

// Scroll mode helpers: enter/exit/save/restore, held binding operations.
// Requires tp_state.h and tp_helpers.h (for release_binding/press_binding).

#include "core/state.h"
#include "tp_log.h"

// Map drag GestureType to scroll_bindings index (0=Sd,1=Ld,2=Dd,3=Sd2,4=Dd2)
static inline int scroll_gesture_index(GestureType gt) {
    static const int8_t lookup[GESTURE_TYPE_COUNT] = {
        -1, -1, -1,                        // SINGLE_TAP, LONG_PRESS, DOUBLE_TAP
        0, 1, 2,                           // SINGLE_TAP_DRAG, LONG_PRESS_DRAG, DOUBLE_TAP_DRAG
        -1, -1,                            // SINGLE_2ND, DOUBLE_2ND
        3, 4                               // SINGLE_DRAG_2ND, DOUBLE_DRAG_2ND
    };
    if (gt < 0 || gt >= GESTURE_TYPE_COUNT) return -1;
    return lookup[gt];
}

// Check if a drag gesture has any scroll bindings configured
static inline bool gesture_has_scroll_bindings(GestureType gt) {
    int idx = scroll_gesture_index(gt);
    if (idx < 0) return false;
    for (int d = 0; d < SCROLL_DIR_COUNT; d++) {
        if (g_state.cfg.scroll_bindings[idx].dirs[d].count > 0)
            return true;
    }
    return false;
}

// Enter scroll mode for a finger
static inline void scroll_mode_enter(TouchFinger* f, GestureType drag_gt) {
    int idx = scroll_gesture_index(drag_gt);
    if (idx < 0) return;
    f->scroll_mode = true;
    f->pending_scroll_mode = false;
    f->scroll_origin_x = f->x;
    f->scroll_origin_y = f->y;
    f->scroll_hold_origin_x = f->x;
    f->scroll_hold_origin_y = f->y;
    f->scroll_gesture_index = idx;
    f->scroll_hold_v_state = 0;
    f->scroll_hold_h_state = 0;
    f->scroll_normal_held_dir = -1;
    _Pragma("GCC unroll 4")
    for (int d = 0; d < SCROLL_DIR_COUNT; d++)
        f->scroll_accum[d] = 0.0f;
}

typedef void (*binding_op_fn)(TouchActionResult* restrict result, const TouchBinding* binding, bool held);

static inline void scroll_apply_held_bindings(TouchFinger* f, TouchActionResult* restrict result,
                                               int si, binding_op_fn op) {
    if (f->scroll_hold_v_state != 0) {
        int d = (f->scroll_hold_v_state == 1) ? SCROLL_DIR_UP : SCROLL_DIR_DOWN;
        const GestureBindingSlot* slot = &g_state.cfg.scroll_bindings[si].dirs[d];
        for (int i = 0; i < slot->count; i++)
            op(result, &slot->arr[i], true);
    }
    if (f->scroll_hold_h_state != 0) {
        int d = (f->scroll_hold_h_state == 1) ? SCROLL_DIR_LEFT : SCROLL_DIR_RIGHT;
        const GestureBindingSlot* slot = &g_state.cfg.scroll_bindings[si].dirs[d];
        for (int i = 0; i < slot->count; i++)
            op(result, &slot->arr[i], true);
    }
    if (f->scroll_normal_held_dir >= 0 && f->scroll_normal_held_dir < SCROLL_DIR_COUNT) {
        const GestureBindingSlot* slot = &g_state.cfg.scroll_bindings[si].dirs[f->scroll_normal_held_dir];
        for (int i = 0; i < slot->count; i++)
            op(result, &slot->arr[i], true);
    }
}

static inline void scroll_release_held_binding_op(TouchActionResult* restrict result, const TouchBinding* b, bool held) {
    (void)held;
    release_binding(result, b);
}

static inline void scroll_press_held_binding_op(TouchActionResult* restrict result, const TouchBinding* b, bool held) {
    (void)held;
    press_binding(result, b, true);
}

static inline void scroll_release_held_bindings(TouchFinger* f, TouchActionResult* restrict result, int si) {
    scroll_apply_held_bindings(f, result, si, scroll_release_held_binding_op);
}

static inline void scroll_press_held_bindings(TouchFinger* f, TouchActionResult* restrict result, int si) {
    scroll_apply_held_bindings(f, result, si, scroll_press_held_binding_op);
}

// Exit scroll mode for a finger — release only the actively held bindings
static inline void scroll_mode_exit(TouchFinger* f, TouchActionResult* restrict result) {
    if (!f->scroll_mode) return;
    int si = f->scroll_gesture_index;
    if (si >= 0 && si < GESTURE_SLOT_COUNT)
        scroll_release_held_bindings(f, result, si);
    f->scroll_mode = false;
    f->scroll_hold_v_state = 0;
    f->scroll_hold_h_state = 0;
    f->scroll_normal_held_dir = -1;
}

// Save scroll mode state (when second finger interrupts a scroll-mode gesture)
// Releases held bindings so they can be cleanly re-pressed on restore.
static inline void scroll_mode_save(TouchFinger* f, TouchActionResult* restrict result) {
    if (!f->scroll_mode) return;
    int si = f->scroll_gesture_index;
    if (si >= 0 && si < GESTURE_SLOT_COUNT)
        scroll_release_held_bindings(f, result, si);
    f->pending_scroll_mode = true;
    f->pending_scroll_origin_x = f->scroll_origin_x;
    f->pending_scroll_origin_y = f->scroll_origin_y;
    f->pending_scroll_hold_origin_x = f->scroll_hold_origin_x;
    f->pending_scroll_hold_origin_y = f->scroll_hold_origin_y;
    f->pending_scroll_gesture_index = f->scroll_gesture_index;
    f->pending_scroll_hold_v_state = f->scroll_hold_v_state;
    f->pending_scroll_hold_h_state = f->scroll_hold_h_state;
    f->pending_scroll_normal_held_dir = f->scroll_normal_held_dir;
    _Pragma("GCC unroll 4")
    for (int d = 0; d < SCROLL_DIR_COUNT; d++)
        f->pending_scroll_accum[d] = f->scroll_accum[d];
}

// Restore scroll mode state (when second finger lifts and first finger had scroll mode)
// Re-presses any bindings that were held before the interrupt.
static inline void scroll_mode_restore(TouchFinger* f, TouchActionResult* restrict result) {
    if (!f->pending_scroll_mode) return;
    f->scroll_mode = true;
    f->scroll_origin_x = f->pending_scroll_origin_x;
    f->scroll_origin_y = f->pending_scroll_origin_y;
    f->scroll_hold_origin_x = f->pending_scroll_hold_origin_x;
    f->scroll_hold_origin_y = f->pending_scroll_hold_origin_y;
    f->scroll_gesture_index = f->pending_scroll_gesture_index;
    f->scroll_hold_v_state = f->pending_scroll_hold_v_state;
    f->scroll_hold_h_state = f->pending_scroll_hold_h_state;
    f->scroll_normal_held_dir = f->pending_scroll_normal_held_dir;
    _Pragma("GCC unroll 4")
    for (int d = 0; d < SCROLL_DIR_COUNT; d++)
        f->scroll_accum[d] = f->pending_scroll_accum[d];
    f->pending_scroll_mode = false;
    f->state = GESTURE_STATE_DRAGGING;
    int si = f->scroll_gesture_index;
    if (si >= 0 && si < GESTURE_SLOT_COUNT)
        scroll_press_held_bindings(f, result, si);
}

#endif // TP_SCROLL_H
