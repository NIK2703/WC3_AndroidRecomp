#include "core/internal.h"
#include "branch.h"


static bool validate_dt_confirm(GestureFingerCtx* ctx) {
    if (!ctx->dt.waiting) {
        return false;
    }
    ctx->dt.waiting = false;
    if (g_state.dt_waiting_count > 0) g_state.dt_waiting_count--;
    g_state.gesture_deferred_tap_count = 0;
    return true;
}

static void copy_deferred_double_to_all(const TouchBinding* src, int src_count, GestureFingerCtx* dt_context);

static void sync_dt_ctx(TouchFinger* f, GestureFingerCtx* ctx) {
    ctx->dt.waiting = false;
    ctx->pending_double.count = 0;
    // Keep deferred_double when D was deferred (pulse_on_up / post_dtd).
    // tap_path_deferred_double needs it to fire D on finger-up.
    if (!ctx->dt.post_drag)
        ctx->deferred_double.count = 0;
}

void double_tap_confirm_internal(TouchActionResult* result, TouchFinger* f) {
    int idx = finger_index(f);
    if (idx < 0) return;
    GestureFingerCtx* ctx = &g_ctx[idx];
    if (!validate_dt_confirm(ctx)) return;
    copy_deferred_double_to_all(g_state.gesture_pending_deferred_double,
        g_state.gesture_pending_deferred_double_count, &g_ctx[idx]);

    bool has_dt = g_state.gesture_pending_double_count > 0;
    bool has_dt_drag = f->bindings.double_tap_drag_count > 0;
    bool has_dt_scroll = gesture_has_scroll_bindings(DOUBLE_TAP_DRAG);

    if (has_dt) {
        GesturePairPlan double_plan = {
            .hold_delay_ticks = 0,
            .flags = PLAN_FLAG_PULSE_ON_UP
                   | (has_dt_drag || has_dt_scroll ? PLAN_FLAG_DRAG_AVAIL : 0),
        };

        confirm_double_tap(result, double_plan,
            g_state.gesture_pending_double, g_state.gesture_pending_double_count,
            g_state.gesture_pending_deferred_double,
            &g_state.gesture_pending_deferred_double_count, FALLBACK_MAX,
            &ctx->dt.post_drag, has_dt_scroll);

        g_state.gesture_pending_double_count = 0;
    } else {
        ctx->dt.post_drag = has_dt_drag || has_dt_scroll;
    }

    sync_dt_ctx(f, ctx);
}

void on_drag_start(TouchFinger* f, TouchActionResult* restrict result) {
    f->single_tap_hold_delay_ticks = 0;
    // Execute pending second-finger D2 before clearing, so D2 isn't silently
    // lost when the second finger transitions to DRAGGING (two-finger DTD).
    if (result) {
        TouchFinger* sf = find_finger(g_state.gesture_second_ptr_id);
        if (sf) {
            int si = finger_index(sf);
            if (si >= 0 && g_ctx[si].deferred_double.count > 0) {
                execute_actions(result, g_ctx[si].deferred_double.items,
                    g_ctx[si].deferred_double.count);
            }
        }
    }
    gesture_clear_second_finger_state();
    g_state.gesture_deferred_tap_count = 0;
    g_state.gesture_pending_double_count = 0;
    g_state.gesture_pending_deferred_double_count = 0;
    g_state.gesture_deferred_second_finger_tap = false;
}

bool gesture_is_within_tap_distance(float x, float y) {
    TouchFinger* main_f = find_finger(g_state.gesture_main_ptr_id);
    if (!main_f) return false;
    int idx = finger_index(main_f);
    if (idx < 0) return false;
    const GestureDTWaitCtx* dt = &g_ctx[idx].dt;
    return fabsf(x - dt->tap_up_x) <= g_state.cfg.double_tap_distance_px
        && fabsf(y - dt->tap_up_y) <= g_state.cfg.double_tap_distance_px;
}

void gesture_cancel_double_tap_wait(TouchActionResult* result) {
    TouchFinger* main_finger = find_finger(g_state.gesture_main_ptr_id);
    if (!main_finger) return;
    int idx = finger_index(main_finger);
    if (idx < 0) return;
    GestureFingerCtx* ctx = &g_ctx[idx];
    if (!ctx->dt.waiting) return;
    ctx->dt.waiting = false;
    if (g_state.dt_waiting_count > 0) g_state.dt_waiting_count--;
    if (g_state.gesture_deferred_tap_count > 0) {
        execute_actions(result, g_state.gesture_deferred_tap, g_state.gesture_deferred_tap_count);
        g_state.gesture_deferred_tap_count = 0;
    }
    gesture_clear_deferred_tap();
    main_finger->state = GESTURE_STATE_IDLE;
    ctx->dt_consumed = false;
}

void gesture_tick(uint64_t now_tick, TouchActionResult* result) {
    // caps gate: must run before the gesture-bindings early return so that
    // stale DT/SDTW flags are cleaned up even when switching to a no-gesture mode.
    if (!g_state.cfg.caps_has_double_tap) {
        gesture_clear_second_finger_state();
        g_state.gesture_pending_deferred_double_count = 0;
    }

    if (!g_state.cfg.caps_has_gesture_bindings) return;

    TouchFinger* fingers = g_state.fingers;
    for (int i = 0; i < MAX_FINGERS; i++) {
        TouchFinger* f = &fingers[i];
        if (!f->active) continue;
        if (f->engaged_elem_count > 0) continue;
        gesture_branch(f, &g_ctx[i], result, now_tick, GESTURE_EVENT_TICK, 0 /*x*/, 0 /*y*/);
    }
}


// Execute S on finger-down: start hold timer, defer to drag/up, or execute now.
// Pass resolved bindings (x = non-drag, x_cnt) instead of hardcoded single_tap.
void execute_tap_on_finger_down(TouchFinger* f, TouchActionResult* result,
    uint64_t now_tick, GesturePairPlan plan, bool force_hold,
    const TouchBinding* x, int x_cnt)
{
    if (plan.hold_delay_ticks > 0) {
        f->single_tap_hold_delay_ticks = plan.hold_delay_ticks;
        f->single_tap_hold_timer_tick = now_tick;
    } else if (plan.flags & (PLAN_FLAG_PULSE_ON_UP | PLAN_FLAG_PRESS_ON_DRAG)) {
        // deferred to check_start_drag / handle_tap_up
    } else {
        if (force_hold)
            execute_actions_hold(result, x, x_cnt);
        else
            execute_actions(result, x, x_cnt);
    }
}

static void copy_deferred_double_to_all(const TouchBinding* src, int src_count, GestureFingerCtx* dt_context) {
    copy_bindings_bounded(src, src_count,
        g_state.gesture_pending_double, &g_state.gesture_pending_double_count, FALLBACK_MAX);
    copy_bindings_bounded(src, src_count,
        dt_context->pending_double.items, &dt_context->pending_double.count, FALLBACK_MAX);
    copy_bindings_bounded(src, src_count,
        dt_context->deferred_double.items, &dt_context->deferred_double.count, FALLBACK_MAX);
}

// Enter DT_WAITING state — unified for main and second finger.
// When is_second=true: writes to ctx->dt (start_tick, ref_x, ref_y),
//   deferred_single/deferred_double are ignored (always NULL/0 for SDTW).
// When is_second=false: writes to ctx->dt, copies deferred_single to gesture_deferred_tap,
//   deferred_double to all DT destinations.
void enter_dt_waiting(TouchFinger* f, uint64_t now_tick, bool is_second,
    const TouchBinding* deferred_single, int deferred_single_count,
    const TouchBinding* deferred_double, int deferred_double_count)
{
    int idx = finger_index(f);
    if (idx < 0) return;
    GestureFingerCtx* dt_context = &g_ctx[idx];

    if (is_second) {
        dt_context->dt.ref_x = f->tap_up_x;
        dt_context->dt.ref_y = f->tap_up_y;
    } else {
        gesture_clear_deferred_tap();
        if (deferred_single && deferred_single_count > 0) {
            copy_bindings_bounded(deferred_single, deferred_single_count,
                g_state.gesture_deferred_tap, &g_state.gesture_deferred_tap_count, FALLBACK_MAX);
        }
        if (deferred_double && deferred_double_count > 0) {
            copy_deferred_double_to_all(deferred_double, deferred_double_count, dt_context);
            copy_bindings_bounded(deferred_double, deferred_double_count,
                g_state.gesture_pending_deferred_double, &g_state.gesture_pending_deferred_double_count, FALLBACK_MAX);
        }
        dt_context->dt.tap_up_x = f->tap_up_x;
        dt_context->dt.tap_up_y = f->tap_up_y;
    }

    dt_context->dt.waiting = true;
    dt_context->dt.start_tick = now_tick;
    g_state.dt_waiting_count++;
    f->cached_has_long_press_timer = false;
    f->state = GESTURE_STATE_DOUBLE_TAP_WAITING;
}

// Confirm double-tap: execute D now or defer to finger-up.
//
// Returns:
//   true  — was_executed_now: D actions were fired via execute_actions_hold.
//   false — deferred: D actions were NOT fired; caller must execute them on finger-up.
//
// out_post_dtd:
//   Set to true when post-double-tap-drag is enabled after this confirm.
//   When was_executed_now=true  → true means drag is available for the held D.
//   When was_executed_now=false → always true (deferred path always enables drag).
//
// dst/dst_count/dst_max:
//   D bindings are always copied here regardless of the branch, so that
//   check_start_drag can skip redundant Sd and PATH 1 fires on finger-up.
bool confirm_double_tap(TouchActionResult* result,
    GesturePairPlan d_plan,
    const TouchBinding* src, int src_count,
    TouchBinding* dst, int* dst_count, int dst_max,
    bool* out_post_dtd, bool has_scroll_bindings)
{
    copy_bindings_bounded(src, src_count, dst, dst_count, dst_max);

    bool has_bindings = (src_count > 0);
    bool post_dtd = has_bindings && (d_plan.flags & PLAN_FLAG_DRAG_AVAIL);

    // When no D package bindings exist but Dd or scroll bindings do,
    // enable post-double-tap-drag so Dd drag mode can be entered.
    if (!has_bindings && ((d_plan.flags & PLAN_FLAG_DRAG_AVAIL) || has_scroll_bindings))
        post_dtd = true;

    if (d_plan.flags & PLAN_FLAG_PULSE_ON_UP) {
        *out_post_dtd = post_dtd;
        return false; // deferred
    } else {
        if (has_bindings)
            execute_actions_hold(result, src, src_count);
        *out_post_dtd = post_dtd;
        return has_bindings; // executed now only if there are bindings
    }
}


