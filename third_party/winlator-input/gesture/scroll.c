#include "core/internal.h"
#include "types.h"
#include "scroll.h"


#define SCROLL_DIR_NONE (-1)
#define SCROLL_HOLD_STATE_NONE 0
#define SCROLL_HOLD_STATE_NEG  1
#define SCROLL_HOLD_STATE_POS  2

static void release_scroll_held_dir(TouchFinger* f, TouchActionResult* restrict result,
                                    int si, int new_dir)
{
    if (f->scroll_normal_held_dir >= 0 && f->scroll_normal_held_dir < SCROLL_DIR_COUNT && f->scroll_normal_held_dir != new_dir) {
        const GestureBindingSlot* old_slot = &g_state.cfg.scroll_bindings[si].dirs[f->scroll_normal_held_dir];
        for (int i = 0; i < old_slot->count; i++)
            release_binding(result, &old_slot->arr[i]);
        f->scroll_normal_held_dir = SCROLL_DIR_NONE;
    }
}

static void process_scroll_hold_axis(
    TouchFinger* f, TouchActionResult* restrict result,
    bool enabled, float displacement,
    int8_t* state_field, int dir_neg, int dir_pos)
{
    if (!enabled) return;
    float threshold = (float)g_state.cfg.scroll_hold_threshold_px;
    int8_t new_state;
    if (fabsf(displacement) < threshold) new_state = SCROLL_HOLD_STATE_NONE;
    else new_state = (displacement < 0) ? SCROLL_HOLD_STATE_NEG : SCROLL_HOLD_STATE_POS;

    if (new_state != *state_field) {
        if (g_state.cfg.scroll_hold_haptic > 0)
            add_action(result, ACT_HAPTIC, g_state.cfg.scroll_hold_haptic, 0, 0);

        if (*state_field != SCROLL_HOLD_STATE_NONE) {
            int old_dir = (*state_field == SCROLL_HOLD_STATE_NEG) ? dir_neg : dir_pos;
            const GestureBindingSlot* old_slot = &g_state.cfg.scroll_bindings[f->scroll_gesture_index].dirs[old_dir];
            for (int i = 0; i < old_slot->count; i++)
                release_binding(result, &old_slot->arr[i]);
        }

        if (new_state != SCROLL_HOLD_STATE_NONE) {
            int new_dir = (new_state == SCROLL_HOLD_STATE_NEG) ? dir_neg : dir_pos;
            const GestureBindingSlot* new_slot = &g_state.cfg.scroll_bindings[f->scroll_gesture_index].dirs[new_dir];
            for (int i = 0; i < new_slot->count; i++)
                press_binding(result, &new_slot->arr[i], true);
        }
        *state_field = new_state;
    }
}

void scroll_mode_process(TouchFinger* f, TouchActionResult* restrict result) {
    int si = f->scroll_gesture_index;
    if (si < 0 || si >= GESTURE_SLOT_COUNT) return;

    float dx = f->x - f->scroll_origin_x;
    float dy = f->y - f->scroll_origin_y;

    // Only compute hold displacement and process hold axis when hold mode is enabled
    if (__builtin_expect(g_state.cfg.scroll_hold_v[si] || g_state.cfg.scroll_hold_h[si], 0)) {
        float hdx = f->x - f->scroll_hold_origin_x;
        float hdy = f->y - f->scroll_hold_origin_y;
        process_scroll_hold_axis(f, result, g_state.cfg.scroll_hold_v[si], hdy,
                                 &f->scroll_hold_v_state, SCROLL_DIR_UP, SCROLL_DIR_DOWN);
        process_scroll_hold_axis(f, result, g_state.cfg.scroll_hold_h[si], hdx,
                                 &f->scroll_hold_h_state, SCROLL_DIR_LEFT, SCROLL_DIR_RIGHT);
    }

    // --- Normal scroll mode: fire on threshold ---
    float threshold = (float)g_state.cfg.scroll_threshold_px;
    int haptic_ms = g_state.cfg.scroll_bind_haptic;

    int dir = SCROLL_DIR_NONE;
    float dominant = 0.0f;
    float adx = fabsf(dx);
    float ady = fabsf(dy);
    if (ady > adx && ady > 0.0f) {
        dir = (dy < 0.0f) ? SCROLL_DIR_UP : SCROLL_DIR_DOWN;
        dominant = ady;
    } else if (adx > 0.0f) {
        dir = (dx < 0.0f) ? SCROLL_DIR_LEFT : SCROLL_DIR_RIGHT;
        dominant = adx;
    }
    if (dir < 0) return;

    bool is_vert = (dir == SCROLL_DIR_UP || dir == SCROLL_DIR_DOWN);
    if (is_vert ? g_state.cfg.scroll_hold_v[si] : g_state.cfg.scroll_hold_h[si]) {
        f->scroll_origin_x = f->x;
        f->scroll_origin_y = f->y;
        return; // hold mode handles this axis
    }

    const GestureBindingSlot* slot = &g_state.cfg.scroll_bindings[si].dirs[dir];
    if (slot->count == 0) {
        release_scroll_held_dir(f, result, si, dir);
        return;
    }

    f->scroll_accum[dir] += dominant;
    f->scroll_origin_x = f->x;
    f->scroll_origin_y = f->y;

    bool has_kbd = is_keyboard_binding(&slot->arr[0]);

    if (f->scroll_accum[dir] < threshold) return;
    f->scroll_accum[dir] -= threshold;
    if (haptic_ms > 0)
        add_action(result, ACT_HAPTIC, haptic_ms, 0, 0);

    if (has_kbd) {
        release_scroll_held_dir(f, result, si, dir);
        if (f->scroll_normal_held_dir < 0) {
            for (int i = 0; i < slot->count; i++)
                press_binding(result, &slot->arr[i], true);
            f->scroll_normal_held_dir = dir;
        } else {
            execute_actions(result, slot->arr, slot->count);
        }
    } else {
        release_scroll_held_dir(f, result, si, dir);
        execute_actions(result, slot->arr, slot->count);
    }
}
