#include "core/internal.h"
#include "tp_log.h"
#include "types.h"
#include "branch.h"
#include "scroll.h"


// =========================================================================
// GESTURE STATE MACHINE — STATE TRANSITIONS
// =========================================================================
//
// States (TouchFinger.state / GestureState):
//   IDLE                  — no gesture active; waiting for finger-down
//   TAP_WAITING           — finger-down confirmed; may transition to:
//                           → IDLE          (tap path complete / DT consumed)
//                           → DT_WAITING    (double-tap window opened)
//                           → LONG_PRESSING (long-press timer fires)
//                           → DRAGGING      (movement exceeds drag threshold)
//   DT_WAITING            — double-tap window open; finger-up fires D actions;
//                           timeout → IDLE (execute deferred single-tap)
//   LONG_PRESSING         — long-press held; may transition to:
//                           → DRAGGING  (movement exceeds drag threshold)
//                           → IDLE      (finger-up releases held actions)
//   DRAGGING              — drag in progress; finger-up releases held actions → IDLE
//   SCROLL_MODE           — scroll mode active; processed by scroll_mode_process;
//                           finger-up exits scroll → IDLE
//
// Second-finger roles:
//   ROLE_MAIN   — first finger down; becomes gesture_main_ptr_id
//   ROLE_SECOND — second finger down; activates two-finger gestures (D2, DTD, etc.)
//   ROLE_IGNORED — additional fingers ignored while second is active
//
// Tick events drive timer-based transitions (long-press, DT timeout, deferred single-tap)
// and do not change state directly — they schedule actions or cancel pending states.
// =========================================================================

// =========================================================================
// INLINE HELPERS (branch.c local, need g_state)
// =========================================================================

static inline bool finger_can_go_idle(const TouchFinger* main_finger) {
    if (main_finger->state == GESTURE_STATE_DRAGGING) return false;
    int idx = finger_index(main_finger);
    if (main_finger->state == GESTURE_STATE_TAP_WAITING
        && idx >= 0 && g_ctx[idx].deferred_double.count > 0)
        return false;
    return true;
}

static inline void reset_single_tap_hold_timer(TouchFinger* f) {
    f->single_tap_hold_delay_ticks = 0;
    f->single_tap_hold_timer_tick = 0;
}

static inline void reset_main_gesture_state(void) {
    g_state.gesture_main_ptr_id = INVALID_PTR_ID;
}

static inline void reset_second_gesture_state(void) {
    g_state.gesture_second_active = false;
}

// =========================================================================
// FORWARD DECLARATIONS
// =========================================================================
static void handle_down(TouchFinger* f, GestureFingerCtx* ctx,
                        TouchActionResult* restrict result, uint64_t now_tick);
static void handle_move(TouchFinger* f, GestureFingerCtx* ctx,
                        TouchActionResult* restrict result, uint64_t now_tick,
                        float dx, float dy);
static void handle_up(TouchFinger* f, GestureFingerCtx* ctx,
                      TouchActionResult* restrict result, uint64_t now_tick);
static void handle_tick(TouchFinger* f, GestureFingerCtx* ctx,
                        TouchActionResult* restrict result, uint64_t now_tick);

// =========================================================================
// DISPATCHER (public entry point)
// =========================================================================
void gesture_branch(TouchFinger* f, GestureFingerCtx* ctx,
                    TouchActionResult* restrict result, uint64_t now_tick,
                    GestureProcessEvent event,
                    float dx, float dy)
{
    if (event >= GESTURE_EVENT_COUNT) return;
    switch (event) {
        case GESTURE_EVENT_DOWN: handle_down(f, ctx, result, now_tick); break;
        case GESTURE_EVENT_MOVE: handle_move(f, ctx, result, now_tick, dx, dy); break;
        case GESTURE_EVENT_UP:   handle_up(f, ctx, result, now_tick); break;
        case GESTURE_EVENT_TICK: handle_tick(f, ctx, result, now_tick); break;
    }
}

// =========================================================================
// SHARED HELPERS (static, used by multiple handlers)
// =========================================================================

// Execute bindings only if no action is currently held.
static inline void execute_if_not_held(TouchActionResult* restrict result,
                                        const TouchBinding* bindings, int count)
{
    if (!g_state.gesture_is_action_held && count > 0) {
        execute_actions(result, bindings, count);
    }
}

static inline bool should_execute_deferred(
    const TouchBinding* bindings, int count,
    const TouchBinding* held, int held_count)
{
    return count > 0 && (!g_state.gesture_is_action_held
        || !bindings_equal(bindings, count, held, held_count));
}

static inline void reset_finger_to_idle(TouchFinger* f, GestureFingerCtx* ctx) {
    f->state = GESTURE_STATE_IDLE;
    *ctx = (GestureFingerCtx){0};
}

static void exit_scroll_and_deferred_release(TouchFinger* f, GestureFingerCtx* ctx,
                                              TouchActionResult* restrict result) {
    scroll_mode_exit(f, result);
    if (should_execute_deferred(ctx->deferred_double.items, ctx->deferred_double.count,
            g_state.gesture_held_actions, g_state.gesture_held_count))
        execute_actions(result, ctx->deferred_double.items, ctx->deferred_double.count);
    release_held_actions(result, 0);
}

static void cleanup_main_finger(TouchFinger* f, GestureFingerCtx* ctx) {
    if (ctx->dt.waiting) g_state.dt_waiting_count--;
    reset_finger_to_idle(f, ctx);
    gesture_clear_deferred_tap();
    if (g_state.gesture_main_ptr_id == f->ptr_id)
        reset_main_gesture_state();
    g_state.gesture_double_tap_consumed = false;
    if (!g_state.gesture_second_active)
        g_state.gesture_deferred_second_finger_tap = false;
    deactivate_finger(f);
}

// Mark DT consumed, go idle, return HANDLED. Used by execute_tap_path paths 1 & 2.
static inline TapPathResult mark_dt_consumed_idle(TouchFinger* f, GestureFingerCtx* ctx)
{
    ctx->dt.post_drag = false;
    ctx->dt_consumed = true;
    g_state.gesture_double_tap_consumed = true;
    f->state = GESTURE_STATE_IDLE;
    return TAP_PATH_HANDLED;
}

// Path 1+2 merged: deferred D from DT confirm OR post-DT drag cleanup
static TapPathResult tap_path_dt_cleanup(
    TouchFinger* f, GestureFingerCtx* ctx,
    TouchActionResult* restrict result)
{
    if (ctx->deferred_double.count > 0) {
        g_state.gesture_pending_deferred_double_count = 0;
        if (!g_state.gesture_is_action_held)
            execute_actions(result, ctx->deferred_double.items, ctx->deferred_double.count);
        else
            release_held_actions(result, 0);
        return mark_dt_consumed_idle(f, ctx);
    }
    if (ctx->dt.post_drag)
        return mark_dt_consumed_idle(f, ctx);
    return TAP_PATH_SINGLE; // not handled
}

// Path 4: Normal first tap-up — enter DT or single tap
static TapPathResult tap_path_normal(
    TouchFinger* f, GestureFingerCtx* ctx,
    TouchActionResult* restrict result, uint64_t now_tick,
    const TapPathParams* params,
    const GesturePairSlot* slot)
{
    if (f->cached_has_active_double_tap) {
        enter_dt_waiting(f, now_tick, slot->is_second,
            slot->is_second ? NULL : params->non_drag,
            slot->is_second ? 0 : params->non_drag_count,
            slot->is_second ? NULL : f->bindings.double_tap,
            slot->is_second ? 0 : f->bindings.double_tap_count);
        return TAP_PATH_ENTER_DT;
    }

    if (f->cached_has_active_double_tap_drag) {
        enter_dt_waiting(f, now_tick, slot->is_second,
            slot->is_second ? NULL : params->non_drag,
            slot->is_second ? 0 : params->non_drag_count,
            NULL, 0);
        return TAP_PATH_ENTER_DT;
    }

    // Dd/Dd2 scroll-only: no D/Dd package bindings, but scroll bindings exist.
    // Enter DT_WAITING/SDTW so the second tap can confirm and activate scroll mode.
    if (g_state.cfg.caps_has_scroll_bindings
        && gesture_has_scroll_bindings((GestureType)slot->bindings_drag)) {
        execute_if_not_held(result, params->non_drag, params->non_drag_count);
        enter_dt_waiting(f, now_tick, slot->is_second, NULL, 0, NULL, 0);
        return TAP_PATH_ENTER_DT;
    }

    // Single tap
    if (slot->is_second) {
        execute_if_not_held(result, params->fallback, params->fallback_count);
    } else {
        execute_if_not_held(result, params->non_drag, params->non_drag_count);
    }
    f->state = GESTURE_STATE_IDLE;
    return TAP_PATH_SINGLE;
}

static TapPathResult execute_tap_path(
    TouchFinger* f, GestureFingerCtx* ctx,
    TouchActionResult* restrict result, uint64_t now_tick,
    const TapPathParams* params,
    const GesturePairSlot* slot)
{
    TapPathResult r;

    r = tap_path_dt_cleanup(f, ctx, result);
    if (r != TAP_PATH_SINGLE) return r;

    return tap_path_normal(f, ctx, result, now_tick, params, slot);
}

// =========================================================================
// TICK HELPERS
// =========================================================================

// Execute bindings: hold for main finger, normal for second finger.
static inline void execute_or_hold_plan(
    TouchActionResult* restrict result,
    bool should_hold,
    const TouchBinding* non_drag, int non_drag_count)
{
    if (should_hold)
        execute_actions_hold(result, non_drag, non_drag_count);
    else
        execute_actions(result, non_drag, non_drag_count);
}

static void tick_lp_timer(TouchFinger* f, GestureFingerCtx* ctx,
                          TouchActionResult* restrict result, uint64_t now_tick)
{
    if (!f->cached_has_long_press_timer
        || f->cached_has_moved_beyond_threshold
        || g_state.gesture_is_action_held
        || f->gesture_activated_in_touch
        || !TIMER_ELAPSED(now_tick, f->down_tick, g_state.cfg.long_press_timeout_ticks))
        return;

    gesture_clear_second_finger_state();

    const GesturePairSlot* long_press_slot = SLOT_L();
    GesturePairResult gr = gesture_resolve(f, long_press_slot);
    GesturePairPlan long_press_plan = gr.plan;

    if ((long_press_plan.flags & PLAN_FLAG_PULSE_ON_UP) && (long_press_plan.flags & PLAN_FLAG_DRAG_AVAIL)) {
        copy_bindings_bounded(gr.non_drag, gr.non_drag_count,
            ctx->pending_long_press.items, &ctx->pending_long_press.count, FALLBACK_MAX);
    } else {
        execute_or_hold_plan(result, !(long_press_plan.flags & PLAN_FLAG_PULSE_ON_UP),
            gr.non_drag, gr.non_drag_count);
        if (long_press_plan.flags & PLAN_FLAG_PULSE_ON_UP)
            f->cached_has_active_single_tap = false;
    }
    f->cached_has_long_press_timer = false;
    f->single_tap_hold_delay_ticks = 0;
    f->state = GESTURE_STATE_LONG_PRESSING;
    if (g_state.cfg.gesture_long_press_haptic > 0)
        add_action(result, ACT_HAPTIC, g_state.cfg.gesture_long_press_haptic, 0, 0);
}

static void tick_s_hold_timer(TouchFinger* f, GestureFingerCtx* ctx,
                              TouchActionResult* restrict result, uint64_t now_tick,
                              const GesturePairSlot* slot)
{
    if (f->single_tap_hold_delay_ticks <= 0
        || !TIMER_ELAPSED(now_tick, f->single_tap_hold_timer_tick, f->single_tap_hold_delay_ticks)
        || g_state.gesture_is_action_held
        || f->cached_has_moved_beyond_threshold)
        return;

    GesturePairResult gr = gesture_resolve(f, slot);
    if (gr.non_drag_count == 0)
        return;

    f->single_tap_hold_delay_ticks = 0;
    execute_or_hold_plan(result, !slot->is_second, gr.non_drag, gr.non_drag_count);
    ctx->pending_double.count = 0;
    gesture_clear_deferred_tap();
    if (slot->is_second && f->ptr_id != g_state.gesture_main_ptr_id) {
        gesture_clear_second_finger_state();
    }
}

static void reset_dt_ctx(GestureFingerCtx* ctx) {
    ctx->dt.waiting = false;
    g_state.dt_waiting_count--;
    ctx->pending_double.count = 0;
    ctx->deferred_double.count = 0;
}

static inline void execute_and_cleanup_deferred(
    TouchActionResult* restrict result,
    const TouchBinding* bindings, int count,
    TouchFinger* f, GestureFingerCtx* ctx)
{
    if (count > 0)
        execute_actions(result, bindings, count);
    /* Scheduled PRESSes fire naturally via the timer tick. */
    deactivate_finger(f);
}

static void tick_dt_timeout(TouchFinger* f, GestureFingerCtx* ctx,
                            TouchActionResult* restrict result, uint64_t now_tick,
                            const GesturePairSlot* slot)
{
    if (!ctx->dt.waiting
        || !TIMER_ELAPSED(now_tick, ctx->dt.start_tick, g_state.cfg.double_tap_timeout_ticks))
        return;

    reset_dt_ctx(ctx);

    // Select deferred source + cleanup based on finger role
    const TouchBinding* deferred_src;
    int deferred_count;
    if (slot->is_second) {
        deferred_src = ctx->fallback.items;
        deferred_count = ctx->fallback.count;
    } else {
        deferred_src = g_state.gesture_deferred_tap;
        deferred_count = g_state.gesture_deferred_tap_count;
    }

    execute_and_cleanup_deferred(result, deferred_src, deferred_count, f, ctx);

    // execute_and_cleanup_deferred->execute_actions->schedule_bind_package
    // may re-arm gesture_is_action_held. Clear it so held actions don't
    // outlive the DT window and get stuck until the element finger lifts.
    release_held_actions(result, 0);

    // Cleanup: role-specific state reset
    if (slot->is_second) {
        ctx->fallback.count = 0;
        // Only reset second gesture state if this finger is still the registered
        // second finger — otherwise a new second finger may have been assigned
        // via handle_ts_second_finger_down or role transfer, and resetting would
        // corrupt its state (gesture_second_active, etc.)
        if (g_state.gesture_second_ptr_id == f->ptr_id
            || g_state.gesture_second_ptr_id == INVALID_PTR_ID) {
            reset_second_gesture_state();
            TouchFinger* main_finger = find_finger(g_state.gesture_main_ptr_id);
            if (main_finger && finger_can_go_idle(main_finger))
                main_finger->state = GESTURE_STATE_IDLE;
        }
    } else {
        gesture_clear_deferred_tap();
        reset_main_gesture_state();
    }
}

// =========================================================================
// DOWN HELPERS
// =========================================================================

// Shared time/position validation for DT confirmation paths.
// For SDTW: checks timer + distance from reference point.
// For local DT: time is always valid; checks distance from current position.
static bool validate_dt_time_pos(
    bool is_sdtw, uint64_t now_tick, uint64_t dt_start_tick,
    float ref_x, float ref_y, const TouchFinger* f)
{
    bool time_valid = !is_sdtw || !TIMER_ELAPSED(now_tick, dt_start_tick,
        g_state.cfg.double_tap_timeout_ticks);

    bool pos_valid;
    if (is_sdtw) {
        float dx = f->x - ref_x;
        float dy = f->y - ref_y;
        int dp = g_state.cfg.double_tap_distance_px;
        pos_valid = fabsf(dx) <= dp && fabsf(dy) <= dp;
    } else {
        pos_valid = gesture_is_within_tap_distance(f->x, f->y);
    }

    return time_valid && pos_valid;
}

// SDTW confirmation on finger-down: second finger's double-tap-waiting.
// Returns true if handled (finger placed into TAP_WAITING).
static bool confirm_sdtw_on_down(TouchFinger* f, GestureFingerCtx* ctx,
                                  TouchActionResult* restrict result, uint64_t now_tick,
                                  TouchFinger* sf, GestureFingerCtx* sf_ctx)
{
    // Guard: check pre-conditions
    if (!f->cached_has_active_double_tap && !f->cached_has_active_double_tap_drag
        && !(g_state.cfg.caps_has_scroll_bindings
             && gesture_has_scroll_bindings(DOUBLE_DRAG_2ND)))
        return false;

    // Time + position validity
    if (!validate_dt_time_pos(true, now_tick, sf_ctx->dt.start_tick,
            sf_ctx->dt.ref_x, sf_ctx->dt.ref_y, f)) {
        gesture_clear_second_finger_state();
        return false;
    }

    // Confirm
    const GesturePairSlot* dt_slot = SELECT_DT_SLOT(f);
    GesturePairPlan double_tap_plan = gesture_resolve(f, dt_slot).plan;
    const GesturePairSlot* s_slot = SELECT_S_SLOT(f);
    bool has_scroll = gesture_has_scroll_bindings((GestureType)dt_slot->bindings_drag)
        || gesture_has_scroll_bindings((GestureType)s_slot->bindings_drag);

    if (f->ptr_id != g_state.gesture_main_ptr_id)
        gesture_clear_second_finger_state();
    confirm_double_tap(result, double_tap_plan,
        f->bindings.double_tap, f->bindings.double_tap_count,
        sf_ctx->deferred_double.items, &sf_ctx->deferred_double.count,
        FALLBACK_MAX, &sf_ctx->dt.post_drag, has_scroll);

    ctx->dt.post_drag = sf_ctx->dt.post_drag;
    copy_bindings_bounded(sf_ctx->deferred_double.items, sf_ctx->deferred_double.count,
        ctx->deferred_double.items, &ctx->deferred_double.count, FALLBACK_MAX);

    f->cached_has_long_press_timer = false;
    f->down_tick = now_tick;
    f->state = GESTURE_STATE_TAP_WAITING;
    reset_single_tap_hold_timer(f);
    return true;
}

// Unified DT confirmation on finger-down — dispatches SDTW (second finger)
// to confirm_sdtw_on_down(), handles local DT_WAITING (main finger) directly.
//
// For SDTW (second finger): checks second finger's ctx->dt for time/position validity,
// uses f->bindings.double_tap as src, stores result in pending_second_double.
// For local DT_WAITING (main finger): checks ctx->dt.start_tick + gesture_is_within_tap_distance,
// uses ctx->pending_double as src, stores result in ctx->deferred_double.
static bool confirm_dt_on_down(TouchFinger* f, GestureFingerCtx* ctx,
                               TouchActionResult* restrict result, uint64_t now_tick)
{
    // SDTW path: second finger double-tap-waiting.
    if (f->is_second_finger && g_state.gesture_second_active) {
        TouchFinger* sf = find_finger(g_state.gesture_second_ptr_id);
        if (sf) {
            int sf_idx = finger_index(sf);
            if (sf_idx >= 0) {
                GestureFingerCtx* sf_ctx = &g_ctx[sf_idx];
                if (sf_ctx->dt.waiting)
                    return confirm_sdtw_on_down(f, ctx, result, now_tick, sf, sf_ctx);
            }
        }
    }

    // Local DT_WAITING path: main finger
    if (!ctx->dt.waiting)
        return false;

    // Time + position validity
    if (!validate_dt_time_pos(false, now_tick, 0, 0, 0, f)) {
        gesture_cancel_double_tap_wait(result);
        return false;
    }

    // Confirm
    const GesturePairSlot* dt_slot = SELECT_DT_SLOT(f);
    GesturePairResult gr = gesture_resolve(f, dt_slot);
    const GesturePairSlot* s_slot = SELECT_S_SLOT(f);
    bool has_scroll = gesture_has_scroll_bindings((GestureType)dt_slot->bindings_drag)
        || gesture_has_scroll_bindings((GestureType)s_slot->bindings_drag);

    confirm_double_tap(result, gr.plan,
        ctx->pending_double.items, ctx->pending_double.count,
        ctx->deferred_double.items, &ctx->deferred_double.count,
        FALLBACK_MAX, &ctx->dt.post_drag, has_scroll);

    ctx->dt.waiting = false;
    g_state.dt_waiting_count--;
    ctx->pending_double.count = 0;

    f->cached_has_long_press_timer = false;
    f->state = GESTURE_STATE_TAP_WAITING;
    reset_single_tap_hold_timer(f);
    return true;
}

static bool down_handle_global_dt(TouchFinger* f, GestureFingerCtx* ctx,
                                  TouchActionResult* restrict result)
{
    TouchFinger* main_dt_finger = find_finger(g_state.gesture_main_ptr_id);
    if (!main_dt_finger) {
        gesture_cancel_double_tap_wait(result);
        return false;
    }

    int main_idx = finger_index(main_dt_finger);
    if (main_idx < 0) return false;
    GestureFingerCtx* main_ctx = &g_ctx[main_idx];

    if (!main_ctx->dt.waiting)
        return false;

    if (gesture_is_within_tap_distance(f->x, f->y)) {
        double_tap_confirm_internal(result, main_dt_finger);
        main_dt_finger->state = GESTURE_STATE_TAP_WAITING;
        reset_finger_tap_state(main_dt_finger);
        main_dt_finger->down_x = main_dt_finger->x;
        main_dt_finger->down_y = main_dt_finger->y;
        g_ctx_init(ctx);
        f->state = GESTURE_STATE_TAP_WAITING;
        return true;
    }

    gesture_cancel_double_tap_wait(result);
    return false;
}

static bool down_handle_sdtw_and_global_dt(TouchFinger* f, GestureFingerCtx* ctx,
                                            TouchActionResult* restrict result)
{
    if (f->is_second_finger) {
        if (down_handle_global_dt(f, ctx, result))
            return true;
        if (f->ptr_id != g_state.gesture_main_ptr_id)
            gesture_clear_second_finger_state();
    }

    return false;
}

static DownRole assign_down_role(TouchFinger* f) {
    if (g_state.gesture_main_ptr_id == INVALID_PTR_ID) {
        if (g_state.gesture_second_active) {
            f->is_second_finger = true;
            setup_second_finger_bindings(f);
        } else {
            f->is_second_finger = false;
        }
        g_state.gesture_main_ptr_id = f->ptr_id;
        return ROLE_MAIN;
    }
    if (f->ptr_id != g_state.gesture_main_ptr_id) {
        if (!g_state.gesture_second_active) {
            g_state.gesture_second_active = true;
            g_state.gesture_second_ptr_id = f->ptr_id;
            f->is_second_finger = true;
            TouchFinger* main_finger = find_finger(g_state.gesture_main_ptr_id);
            if (main_finger) {
                g_state.gesture_second_main_ref_x = main_finger->x;
                g_state.gesture_second_main_ref_y = main_finger->y;
                main_finger->cached_has_long_press_timer = false;
            }
            return ROLE_SECOND;
        }
        f->state = GESTURE_STATE_IDLE;
        return ROLE_IGNORED;
    }
    return ROLE_MAIN;
}

// =========================================================================
// UP HELPERS
// =========================================================================

static bool up_handle_tap_waiting(TouchFinger* f, GestureFingerCtx* ctx,
                                  TouchActionResult* restrict result, uint64_t now_tick,
                                  const GesturePairSlot* slot,
                                  const TouchBinding* non_drag, int non_drag_count) {

    const TapPathParams tap_params = {
        .non_drag = non_drag,
        .non_drag_count = non_drag_count,
        .fallback = ctx->fallback.items,
        .fallback_count = ctx->fallback.count,
    };
    TapPathResult path = execute_tap_path(f, ctx, result, now_tick, &tap_params, slot);

    // TAP_PATH_HANDLED: DT consumed paths 1-3. State set to IDLE by execute_tap_path.
    if (path == TAP_PATH_HANDLED)
        return false;

    // TAP_PATH_ENTER_DT: entered DT/SDTW waiting.
    if (path == TAP_PATH_ENTER_DT) {
        release_held_actions(result, 0);
        if (slot->is_second)
            cleanup_second_finger(f, ctx, result);
        return true;
    }

    // TAP_PATH_SINGLE: single tap executed. State set to IDLE.
    if (slot->is_second) {
        cleanup_second_finger(f, ctx, result);
        return true;
    }

    release_held_actions(result, 0);
    return false;
}

static void up_handle_long_pressing(TouchFinger* f, GestureFingerCtx* ctx,
                                    TouchActionResult* restrict result) {
    if (ctx->pending_long_press.count > 0) {
        execute_actions(result, ctx->pending_long_press.items,
            ctx->pending_long_press.count);
        ctx->pending_long_press.count = 0;
    } else if (!g_state.gesture_is_action_held
        && f->cached_has_active_long_press)
        execute_actions(result, f->bindings.long_press,
            f->bindings.long_press_count);
    release_held_actions(result, 0);
}

static void up_handle_dragging(TouchFinger* f, GestureFingerCtx* ctx,
                               TouchActionResult* restrict result) {
    if (should_execute_deferred(ctx->deferred_double.items, ctx->deferred_double.count,
            g_state.gesture_held_actions, g_state.gesture_held_count))
        execute_actions(result, ctx->deferred_double.items, ctx->deferred_double.count);
    ctx->pending_long_press.count = 0;
    release_held_actions(result, 0);
}

static void up_handle_dt_waiting(TouchFinger* f, GestureFingerCtx* ctx,
                                  TouchActionResult* restrict result,
                                  const TouchBinding* non_drag, int non_drag_count) {
    if (non_drag_count > 0)
        execute_actions(result, non_drag, non_drag_count);
    release_held_actions(result, 0);
    ctx->dt.waiting = false;
    g_state.dt_waiting_count--;
}

static void up_handle_default(TouchFinger* f, GestureFingerCtx* ctx,
                               TouchActionResult* restrict result) {
    if (ctx->dt.post_drag)
        ctx->dt.post_drag = false;
    release_held_actions(result, 0);
}

static void up_prelude_second_finger(TouchFinger* f,
                                     TouchActionResult* restrict result,
                                     const GesturePairSlot* slot) {
    if (!f->is_second_finger || !g_state.gesture_second_active)
        return;
    if (g_state.gesture_is_action_held) {
        bool has_dt_for_sdtw = f->cached_has_active_double_tap
            && f->state != GESTURE_STATE_DRAGGING;
        if (has_dt_for_sdtw)
            release_held_actions(result, 0);
        if (f->ptr_id != g_state.gesture_main_ptr_id)
            gesture_clear_second_finger_state();
    }
}

// =========================================================================
// MOVE HELPERS
// =========================================================================

// P1+P12: Combined resolve — binding-only resolve + D-slot pre-processing
// Returns final non_drag/drag arrays ready for MoveInput population.
static void resolve_move_bindings(
    TouchFinger* f, GestureFingerCtx* ctx,
    const GesturePairSlot* slot,
    const TouchBinding** nd, int* nd_n,
    const TouchBinding** d, int* d_n)
{
    gesture_resolve_bindings(f, slot, nd, nd_n, d, d_n);

    if (!slot->is_d_slot) return;

    // D/Dd: re-bind from second finger's bindings (skip D2 self-rebind)
    if (g_state.gesture_second_active && !f->is_second_finger) {
        TouchFinger* sf = find_finger(g_state.gesture_second_ptr_id);
        if (sf)
            gesture_resolve_bindings(sf, slot, nd, nd_n, d, d_n);
    }

    // D2 fallback: absorb ctx->fallback into drag when empty
    if (f->is_second_finger && *d_n == 0) {
        int fb_n = ctx->fallback.count < MOVE_MAX_BINDINGS ? ctx->fallback.count : MOVE_MAX_BINDINGS;
        if (fb_n > 0) {
            *d = (const TouchBinding*)ctx->fallback.items;
            *d_n = fb_n;
        }
    }
}

// P9: Populate MoveInput from resolved bindings and context
static MoveInput populate_move_input(
    float dx, float dy, TouchFinger* f, GestureFingerCtx* ctx,
    const GesturePairSlot* slot,
    const TouchBinding* nd, int nd_n,
    const TouchBinding* d, int d_n)
{
    MoveInput in;
    in.dx = dx;
    in.dy = dy;
    in.non_drag = nd;
    in.drag = d;
    in.non_drag_count = nd_n < MOVE_MAX_BINDINGS ? nd_n : MOVE_MAX_BINDINGS;
    in.drag_count = d_n < MOVE_MAX_BINDINGS ? d_n : MOVE_MAX_BINDINGS;
    in.drag_threshold_px = g_state.cfg.drag_threshold_px;

    in.state = f->state;
    in.dt_waiting = ctx->dt.waiting;
    in.scroll_type = slot->bindings_drag;

    // Pre-computed decision flags
    in.flags = 0;
    if (slot->is_d_slot)
        in.flags |= MOVEIN_FLAG_D_CLEANUP;
    if (f->is_second_finger && g_state.gesture_is_action_held)
        in.flags |= MOVEIN_FLAG_SECOND_HELD;
    if (slot->is_d_slot && !slot->is_second
        && g_state.gesture_second_active)
        in.flags |= MOVEIN_FLAG_MARK_SECOND;

    // P8: Slot mode — replaces has_gesture_for_slot + has_scroll_for_slot
    bool has_scroll = g_state.cfg.caps_has_scroll_bindings
        && gesture_has_scroll_bindings((GestureType)slot->bindings_drag);
    bool has_gesture = current_mode_has_gesture((GestureType)slot->bindings_non_drag)
        || current_mode_has_gesture((GestureType)slot->bindings_drag);
    if (has_scroll)
        in.slot_mode = SLOT_MODE_SCROLL;
    else if (!has_gesture)
        in.slot_mode = SLOT_MODE_NO_GESTURE;
    else
        in.slot_mode = SLOT_MODE_GESTURE;

    return in;
}

// P0: L/Ld fast-path — handles LONG_PRESSING with minimal fields
static void handle_move_long_pressing(
    TouchFinger* f, GestureFingerCtx* ctx,
    TouchActionResult* restrict result, float dx, float dy)
{
    // Sub-threshold: skip binding resolution entirely
    if (__builtin_expect(
        fabsf(dx) <= g_state.cfg.drag_threshold_px
        && fabsf(dy) <= g_state.cfg.drag_threshold_px, 1)) {
        return;
    }

    const GesturePairSlot* slot = SLOT_L();
    const TouchBinding* nd; int nd_n;
    const TouchBinding* d;  int d_n;
    gesture_resolve_bindings(f, slot, &nd, &nd_n, &d, &d_n);

    if (__builtin_expect(nd_n == 0 && d_n == 0, 0)) return;

    MoveInput in;
    in.dx = dx;
    in.dy = dy;
    in.non_drag = NULL;
    in.drag = d;
    in.non_drag_count = 0;
    in.drag_count = d_n < MOVE_MAX_BINDINGS ? d_n : MOVE_MAX_BINDINGS;
    in.drag_threshold_px = g_state.cfg.drag_threshold_px;

    in.state = GESTURE_STATE_LONG_PRESSING;
    in.dt_waiting = false;
    in.scroll_type = 0;
    in.flags = 0;
    in.slot_mode = SLOT_MODE_NO_GESTURE;

    MoveOutput out = resolve_move(&in);
    apply_move(&out, f, ctx, result);
}

// =========================================================================
// GESTURE_EVENT_DOWN
// =========================================================================
static void handle_down(TouchFinger* f, GestureFingerCtx* ctx,
                        TouchActionResult* restrict result, uint64_t now_tick)
{
    g_state.gesture_is_down_event = true;

    DownRole role = assign_down_role(f);
    if (role == ROLE_IGNORED)
        return;

    if (confirm_dt_on_down(f, ctx, result, now_tick))
        return;

    if (down_handle_sdtw_and_global_dt(f, ctx, result))
        return;

    g_ctx_init(ctx);

    if (!current_mode_has_gestures() && g_state.dt_waiting_count == 0
        && !g_state.cfg.caps_has_scroll_bindings) {
        f->state = GESTURE_STATE_IDLE;
        return;
    }

    const GesturePairSlot* slot = SELECT_S_SLOT(f);
    GesturePairResult gr = gesture_resolve(f, slot);

    if (f->is_second_finger && gr.non_drag_count > 0 && ctx->fallback.count == 0) {
        if (slot->bindings_non_drag != (uint8_t)SINGLE_2ND
            || f->cached_has_active_double_tap
            || !f->cached_has_active_double_tap_drag) {
            copy_bindings_bounded(gr.non_drag, gr.non_drag_count,
                ctx->fallback.items, &ctx->fallback.count, FALLBACK_MAX);
        }
    }

    f->state = GESTURE_STATE_TAP_WAITING;
    reset_single_tap_hold_timer(f);
    ctx->dt_consumed = false;

    GesturePairPlan plan = gr.plan;

    if (!g_state.gesture_is_action_held) {
        execute_tap_on_finger_down(f, result, now_tick, plan, true, gr.non_drag, gr.non_drag_count);
    }
}

// =========================================================================
// GESTURE_EVENT_MOVE — resolve_move/apply_move pipeline
// =========================================================================
static void handle_move(TouchFinger* f, GestureFingerCtx* ctx,
                        TouchActionResult* restrict result, uint64_t now_tick,
                        float dx, float dy)
{
    if (f->scroll_mode) {
        scroll_mode_process(f, result);
        return;
    }

    // P0: L/Ld fast-path — LONG_PRESSING has trivial state space
    if (f->state == GESTURE_STATE_LONG_PRESSING) {
        handle_move_long_pressing(f, ctx, result, dx, dy);
        return;
    }

    // P0: DRAGGING fast-path — resolve_move returns DRAG_SKIP unconditionally,
    // all skipped functions are pure no-ops for this state.
    if (f->state == GESTURE_STATE_DRAGGING) {
        return;
    }

    // P0: IDLE/DT_WAITING fast-path — resolve_move returns DRAG_SKIP for these states,
    // avoiding full pipeline (slot select + binding resolve + MoveInput/MoveOutput).
    if (__builtin_expect(f->state == GESTURE_STATE_IDLE
        || f->state == GESTURE_STATE_DOUBLE_TAP_WAITING, 0)) {
        return;
    }

    // P0: Early-exit for sub-threshold movement — skip slot selection and binding resolution
    if (__builtin_expect(
        fabsf(dx) <= g_state.cfg.drag_threshold_px
        && fabsf(dy) <= g_state.cfg.drag_threshold_px, 1)) {
        return;
    }

    const GesturePairSlot* slot = select_slot_common(f, ctx, false);
    if (!slot) {
        return;
    }

    // P12: Combined resolve — bindings + D-slot pre-processing in one call
    const TouchBinding* nd; int nd_n;
    const TouchBinding* d;  int d_n;
    resolve_move_bindings(f, ctx, slot, &nd, &nd_n, &d, &d_n);

    // P9: Populate MoveInput in one helper call
    MoveInput in = populate_move_input(dx, dy, f, ctx, slot, nd, nd_n, d, d_n);

    MoveOutput out = resolve_move(&in);

    // P7: Pre-resolve scroll finger for DRAG_SCROLL
    if (out.action == DRAG_SCROLL && f->is_second_finger) {
        TouchFinger* main = find_finger(g_state.gesture_main_ptr_id);
        if (main && main->active)
            out.scroll_finger = main;
    }

    apply_move(&out, f, ctx, result);
}

// =========================================================================
// GESTURE_EVENT_UP
// =========================================================================
static void handle_up(TouchFinger* f, GestureFingerCtx* ctx,
                      TouchActionResult* restrict result, uint64_t now_tick)
{
    g_state.gesture_is_down_event = false;

    // Exit scroll mode if active on this finger
    if (f->scroll_mode) {
        exit_scroll_and_deferred_release(f, ctx, result);
        f->state = GESTURE_STATE_IDLE;
        cleanup_main_finger(f, ctx);
        return;
    }

    // For second-finger gestures, scroll mode is on the MAIN finger.
    // When the second finger goes up, exit scroll mode on the main finger.
    if (f->is_second_finger && g_state.gesture_main_ptr_id != INVALID_PTR_ID) {
        TouchFinger* main = find_finger(g_state.gesture_main_ptr_id);
        if (main && main->scroll_mode) {
            GestureFingerCtx* main_ctx = &g_ctx[(int)(main - g_state.fingers)];
            exit_scroll_and_deferred_release(main, main_ctx, result);
        }
    }

    f->tap_up_x = f->x;
    f->tap_up_y = f->y;

    const GesturePairSlot* slot = select_slot_common(f, ctx, true);
    if (!slot) {
        release_held_actions(result, 0);
        cleanup_main_finger(f, ctx);
        return;
    }
    GesturePairResult gr = gesture_resolve(f, slot);

    up_prelude_second_finger(f, result, slot);

    if (f->state == GESTURE_STATE_IDLE) {
        release_held_actions(result, 0);
        return;
    }

    switch (f->state) {
        case GESTURE_STATE_TAP_WAITING:
            if (up_handle_tap_waiting(f, ctx, result, now_tick, slot,
                gr.non_drag, gr.non_drag_count))
                return;
            break;
        case GESTURE_STATE_LONG_PRESSING:
            up_handle_long_pressing(f, ctx, result);
            break;
        case GESTURE_STATE_DRAGGING:
            up_handle_dragging(f, ctx, result);
            break;
        case GESTURE_STATE_DOUBLE_TAP_WAITING:
            up_handle_dt_waiting(f, ctx, result,
                gr.non_drag, gr.non_drag_count);
            break;
        default:
            up_handle_default(f, ctx, result);
            break;
    }

    if (slot->is_second) {
        release_held_actions(result, 0);
        cleanup_second_finger(f, ctx, result);
        return;
    }
    release_held_actions(result, 0);
    cleanup_main_finger(f, ctx);
}

// =========================================================================
// GESTURE_EVENT_TICK
// =========================================================================
static void handle_tick(TouchFinger* f, GestureFingerCtx* ctx,
                        TouchActionResult* restrict result, uint64_t now_tick)
{
    const GesturePairSlot* slot = select_gesture_slot(f);
    if (!slot) {
        return;
    }

    if (f->cached_has_long_press_timer
        && (f->cached_has_moved_beyond_threshold || g_state.gesture_is_action_held)) {
        f->cached_has_long_press_timer = false;
    }

    if (f->cached_has_long_press_timer)
        tick_lp_timer(f, ctx, result, now_tick);
    tick_s_hold_timer(f, ctx, result, now_tick, SELECT_S_SLOT(f));
    if (ctx->dt.waiting)
        tick_dt_timeout(f, ctx, result, now_tick, slot);
}
