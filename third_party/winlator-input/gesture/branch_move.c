#include "core/internal.h"
#include "types.h"
#include "scroll.h"


// ============================================================
// resolve_move — PURE function, no side effects
//
// Reads only from MoveInput. Writes only to MoveOutput.
// NO is_second_finger, NO is_d_slot, NO raw mode/slot data.
// All pair-specific logic is pre-computed by the caller.
// ============================================================

// Select drag > non_drag bindings for execute output.
// Clamps to execute array size (4).
static void select_drag_bindings(MoveOutput* out, const MoveInput* in) {
    if (in->drag_count > 0) {
        int n = in->drag_count < 4 ? in->drag_count : 4;
        memcpy(out->execute, in->drag, n * sizeof(TouchBinding));
        out->execute_count = n;
    } else if (in->non_drag_count > 0) {
        int n = in->non_drag_count < 4 ? in->non_drag_count : 4;
        memcpy(out->execute, in->non_drag, n * sizeof(TouchBinding));
        out->execute_count = n;
    }
}

MoveOutput resolve_move(const MoveInput* in) {
    MoveOutput out = {0};

    // ---- State gate: LONG_PRESSING or TAP_WAITING only ----
    if (in->state != GESTURE_STATE_LONG_PRESSING
        && in->state != GESTURE_STATE_TAP_WAITING)
        return out;

    // ---- Pre-mutations (apply regardless of state) ----
    if (in->dt_waiting)
        out.flags |= MOVEOUT_FLAG_CANCEL_DT;
    if (in->state == GESTURE_STATE_LONG_PRESSING)
        out.flags |= MOVEOUT_FLAG_CLEAR_LP;
    else if (in->flags & MOVEIN_FLAG_D_CLEANUP)
        out.flags |= MOVEOUT_FLAG_D_CLEANUP;

    // ---- Threshold check ----
    if (fabsf(in->dx) <= in->drag_threshold_px
        && fabsf(in->dy) <= in->drag_threshold_px) {
        return out;
    }

    // ---- TAP_WAITING-only gates (skipped for LONG_PRESSING) ----
    if (in->state == GESTURE_STATE_TAP_WAITING) {
        if (in->slot_mode == SLOT_MODE_SCROLL) {
            out.action = DRAG_SCROLL;
            out.scroll_type = in->scroll_type;
            out.flags |= MOVEOUT_FLAG_RELEASE_ALL;
            return out;
        }

        if (in->slot_mode == SLOT_MODE_NO_GESTURE
            || ((in->flags & MOVEIN_FLAG_SECOND_HELD) && in->drag_count == 0)) {
            out.action = DRAG_START;
            if ((in->flags & MOVEIN_FLAG_SECOND_HELD) && in->drag_count == 0)
                out.flags |= MOVEOUT_FLAG_CLEAR_PENDING;
            return out;
        }
    }

    // ---- Drag binding resolution (2-way: drag > non_drag) ----
    select_drag_bindings(&out, in);

    if (out.execute_count == 0) {
        return out;
    }

    // ---- Second-finger held: press binding, no drag state change needed ----
    if (in->flags & MOVEIN_FLAG_SECOND_HELD) {
        out.action = DRAG_HOLD;
        return out;
    }

    // ---- Normal drag (all assignments, no pair identity) ----
    out.action = DRAG_START;
    if ((in->drag_count == 0) && (in->flags & MOVEIN_FLAG_D_CLEANUP))
        out.flags |= MOVEOUT_FLAG_CLEAR_PENDING;
    if (in->flags & MOVEIN_FLAG_MARK_SECOND)
        out.flags |= MOVEOUT_FLAG_MARK_SECOND;
    return out;
}

// ============================================================
// apply_move — executes all side effects from MoveOutput
// ============================================================

void start_drag_with_binding(TouchFinger* f,
    TouchActionResult* restrict result,
    const TouchBinding* drag_binding, int drag_count)
{
    bool same_as_held = g_state.gesture_is_action_held;
    if (same_as_held && drag_binding && g_state.gesture_held_count > 0) {
        same_as_held = bindings_equal(drag_binding, drag_count,
                        g_state.gesture_held_actions, g_state.gesture_held_count);
    }
    if (!same_as_held) {
        TouchBinding saved_ar[MAX_HELD_ACTIONS];
        int saved_ar_count = 0;
        for (int i = 0; i < g_state.gesture_held_count; i++) {
            if (g_state.gesture_held_actions[i].flags & TOUCH_BINDING_FLAG_AUTO_REPEAT) {
                saved_ar[saved_ar_count++] = g_state.gesture_held_actions[i];
            }
        }
        // Flush pending scheduled RELEASEs to prevent orphaned key-down,
        // then cancel remaining scheduled actions.
        for (int i = 0; i < g_state.scheduled_action_count; i++) {
            ScheduledAction* sa = &g_state.scheduled_actions[i];
            if (!sa->active) continue;
            if (sa->action_type == ACT_KEY_RELEASE ||
                sa->action_type == ACT_POINTER_BUTTON_RELEASE ||
                sa->action_type == ACT_GAMEPAD_RELEASE) {
                release_binding(result, &sa->binding);
            }
            // If cancelling a PRESS whose binding is in held_actions, remove it
            // to prevent an orphan RELEASE (the DOWN never fired).
            if (sa->action_type == ACT_KEY_PRESS ||
                sa->action_type == ACT_POINTER_BUTTON_PRESS ||
                sa->action_type == ACT_GAMEPAD_STATE) {
                for (int j = 0; j < g_state.gesture_held_count; j++) {
                    if (g_state.gesture_held_actions[j].type == sa->binding.type &&
                        g_state.gesture_held_actions[j].keycode == sa->binding.keycode &&
                        g_state.gesture_held_actions[j].modifiers == sa->binding.modifiers &&
                        g_state.gesture_held_actions[j].flags == sa->binding.flags) {
                        // Remove by swapping with the last element
                        g_state.gesture_held_actions[j] = g_state.gesture_held_actions[g_state.gesture_held_count - 1];
                        g_state.gesture_held_count--;
                        break;
                    }
                }
            }
            sa->active = false;
        }
        g_state.scheduled_action_count = 0;
        // Release all held actions immediately (no scheduling — we're
        // transitioning to a new drag context)
        for (int j = g_state.gesture_held_count - 1; j >= 0; j--) {
            if (g_state.gesture_held_actions[j].type != NONE)
                release_binding(result, &g_state.gesture_held_actions[j]);
        }
        g_state.gesture_held_count = 0;
        g_state.gesture_is_action_held = false;
        memset(g_state.gesture_auto_repeat_last_tick_held, 0, sizeof(g_state.gesture_auto_repeat_last_tick_held));
        // Restore auto-repeat held actions
        for (int i = 0; i < saved_ar_count && g_state.gesture_held_count < MAX_HELD_ACTIONS; i++) {
            g_state.gesture_held_actions[g_state.gesture_held_count++] = saved_ar[i];
        }
        if (saved_ar_count > 0)
            g_state.gesture_is_action_held = true;
        execute_actions_hold(result, drag_binding, drag_count);
    }
    start_drag_no_binding(f, result);
}

void apply_move(const MoveOutput* out, TouchFinger* f,
                GestureFingerCtx* ctx, TouchActionResult* restrict result) {
    if (out->flags & MOVEOUT_FLAG_CANCEL_DT)
        gesture_cancel_double_tap_wait(result);

    if (out->flags & MOVEOUT_FLAG_CLEAR_LP)
        ctx->pending_long_press.count = 0;

    if (out->flags & MOVEOUT_FLAG_D_CLEANUP) {
        f->single_tap_hold_delay_ticks = 0;
        ctx->dt.post_drag = false;
    }

    switch (out->action) {

    case DRAG_SKIP:
        break;

    case DRAG_START:
        if (out->flags & MOVEOUT_FLAG_CLEAR_PENDING)
            ctx->pending_double.count = 0;
        if (out->execute_count > 0)
            start_drag_with_binding(f, result, out->execute, out->execute_count);
        else
            start_drag_no_binding(f, result);
        if (out->flags & MOVEOUT_FLAG_MARK_SECOND)
            mark_second_finger_dragging(f, result);
        break;

    case DRAG_SCROLL: {
        TouchFinger* scroll_finger = out->scroll_finger ? out->scroll_finger : f;
        GestureFingerCtx* scroll_ctx = (scroll_finger == f)
            ? ctx : &g_ctx[(int)(scroll_finger - g_state.fingers)];

        scroll_ctx->deferred_double.count = 0;
        if (scroll_finger != f)
            ctx->deferred_double.count = 0;

        if (out->flags & MOVEOUT_FLAG_RELEASE_ALL)
            release_held_actions(result, 0);

        scroll_mode_enter(scroll_finger, out->scroll_type);
        start_drag_no_binding(f, result);
        break;
    }

    case DRAG_HOLD:
        execute_actions_hold(result, out->execute, out->execute_count);
        start_drag_no_binding(f, result);
        break;

    }
}
