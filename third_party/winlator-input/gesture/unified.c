#include "core/internal.h"
#include "types.h"


// ============================================================
// Per-finger gesture context
// ============================================================
GestureFingerCtx g_ctx[MAX_FINGERS];

// ============================================================
// Gesture pair slots — encode ALL differences as data
//   Slot 0: S/Sd   (main finger single tap)
//   Slot 1: D/Dd   (main finger double tap)
//   Slot 2: L/Ld   (main finger long press)
//   Slot 3: S2/Sd2 (second finger single tap)
//   Slot 4: D2/Dd2 (second finger double tap)
//
// Fields:
//   bindings_non_drag / bindings_drag — GestureType for binding lookup
//   is_d_slot    — D/Dd or D2/Dd2 slot (controls move re-bind + cleanup)
//   is_second    — S2/D2 slot (drives sdtw, fallback, hold_execute)
// ============================================================
const GesturePairSlot GESTURE_SLOTS[GESTURE_SLOT_COUNT] = {
    // S/Sd:   is_d=false is_second=false
    { SINGLE_TAP, SINGLE_TAP_DRAG,  false, false },
    // D/Dd:   is_d=true  is_second=false
    { DOUBLE_TAP, DOUBLE_TAP_DRAG,  true,  false },
    // L/Ld:   is_d=false is_second=false
    { LONG_PRESS, LONG_PRESS_DRAG,  false, false },
    // S2/Sd2: is_d=false is_second=true
    { SINGLE_2ND, SINGLE_DRAG_2ND,  false, true  },
    // D2/Dd2: is_d=true  is_second=true
    { DOUBLE_2ND, DOUBLE_DRAG_2ND,  true,  true  },
};

// ============================================================
// Binding resolution — 3 clean switches, zero function pointers
// ============================================================
#define GROUP_UNMAPPED        0
#define GROUP_SINGLE_TAP      1
#define GROUP_DOUBLE_TAP      2
#define GROUP_LONG_PRESS      3
#define GROUP_SINGLE_TAP_DRAG 4
#define GROUP_DOUBLE_TAP_DRAG 5
#define GROUP_LONG_PRESS_DRAG 6

// NOTE: group_ptr, group_cnt, and group_cached are structurally parallel
// dispatch tables mapping GROUP_* → field of FingerBindings or TouchFinger.
// They must be kept in sync: every GROUP_ case must appear in all three.
// A table-driven approach is not possible here because the struct layouts
// differ (FingerBindings holds pointers/counts, TouchFinger holds bools).
static const TouchBinding* group_ptr(int g, const FingerBindings* fb) {
    switch (g) {
    case GROUP_UNMAPPED: return NULL;
    case GROUP_SINGLE_TAP: return fb->single_tap;
    case GROUP_DOUBLE_TAP: return fb->double_tap;
    case GROUP_LONG_PRESS: return fb->long_press;
    case GROUP_SINGLE_TAP_DRAG: return fb->single_tap_drag;
    case GROUP_DOUBLE_TAP_DRAG: return fb->double_tap_drag;
    case GROUP_LONG_PRESS_DRAG: return fb->long_press_drag;
    default: return NULL;
    }
}

// Parallel to group_ptr: returns binding count for a given group.
static int group_cnt(int g, const FingerBindings* fb) {
    switch (g) {
    case GROUP_UNMAPPED: return 0;
    case GROUP_SINGLE_TAP: return fb->single_tap_count;
    case GROUP_DOUBLE_TAP: return fb->double_tap_count;
    case GROUP_LONG_PRESS: return fb->long_press_count;
    case GROUP_SINGLE_TAP_DRAG: return fb->single_tap_drag_count;
    case GROUP_DOUBLE_TAP_DRAG: return fb->double_tap_drag_count;
    case GROUP_LONG_PRESS_DRAG: return fb->long_press_drag_count;
    default: return 0;
    }
}

// Parallel to group_ptr/group_cnt: returns cached-active flag for a given group.
static bool group_cached(int g, const TouchFinger* f) {
    switch (g) {
    case GROUP_UNMAPPED: return false;
    case GROUP_SINGLE_TAP: return f->cached_has_active_single_tap;
    case GROUP_DOUBLE_TAP: return f->cached_has_active_double_tap;
    case GROUP_LONG_PRESS: return f->cached_has_active_long_press;
    case GROUP_SINGLE_TAP_DRAG: return f->cached_has_active_single_tap_drag;
    case GROUP_DOUBLE_TAP_DRAG: return f->cached_has_active_double_tap_drag;
    case GROUP_LONG_PRESS_DRAG: return f->cached_has_active_long_press_drag;
    default: return false;
    }
}

// Canonical mapping: GestureType → group
static const int g_type_to_group[GESTURE_TYPE_COUNT] = {
    [SINGLE_TAP]      = GROUP_SINGLE_TAP,
    [DOUBLE_TAP]      = GROUP_DOUBLE_TAP,
    [LONG_PRESS]      = GROUP_LONG_PRESS,
    [SINGLE_TAP_DRAG] = GROUP_SINGLE_TAP_DRAG,
    [DOUBLE_TAP_DRAG] = GROUP_DOUBLE_TAP_DRAG,
    [LONG_PRESS_DRAG] = GROUP_LONG_PRESS_DRAG,
    [SINGLE_2ND]      = GROUP_SINGLE_TAP,
    [DOUBLE_2ND]      = GROUP_DOUBLE_TAP,
    [SINGLE_DRAG_2ND] = GROUP_SINGLE_TAP_DRAG,
    [DOUBLE_DRAG_2ND] = GROUP_DOUBLE_TAP_DRAG,
};

static inline int resolve_group(GestureType gt) {
    return (gt >= 0 && gt < GESTURE_TYPE_COUNT) ? g_type_to_group[gt] : GROUP_UNMAPPED;
}

// ============================================================
// Unified resolve — single pass, no redundant lookups
// ============================================================
// Returns both plan AND bindings in one call. Resolves each group
// ONCE: cached flag feeds the plan, ptr+count feed the bindings.
//
// hx  = non-drag binding active
// hxd = drag binding active OR scroll fallback
// comp = is_second | is_d_slot when competing with double-tap
//
// Truth table (5 named states):
//   NO_BINDING      (hx=0)                → do nothing
//   S_ONLY          (hx=1,hxd=0,comp=0)   → hold delay (main) or nothing (2nd)
//   S_WITH_DRAG     (hx=1,hxd=1,comp=0)   → pulse on up, drag available
//   S_WITH_COMP     (hx=1,hxd=0,comp=1)   → pulse on up, press on drag
//   S_WITH_BOTH     (hx=1,hxd=1,comp=1)   → pulse on up, drag available
GesturePairResult gesture_resolve(
    const TouchFinger* f,
    const GesturePairSlot* slot)
{
    GesturePairResult r = {0};
    const FingerBindings* fb = &f->bindings;
    int gnd = resolve_group((GestureType)slot->bindings_non_drag);
    int gd  = resolve_group((GestureType)slot->bindings_drag);

    // Fast path: no bindings configured for either group — skip plan
    if (gnd == GROUP_UNMAPPED && gd == GROUP_UNMAPPED)
        return r;

    if (gnd != GROUP_UNMAPPED) {
        r.non_drag       = group_ptr(gnd, fb);
        r.non_drag_count = group_cnt(gnd, fb);
    }

    if (gd != GROUP_UNMAPPED) {
        r.drag       = group_ptr(gd, fb);
        r.drag_count = group_cnt(gd, fb);
    }

    bool hx  = (gnd != GROUP_UNMAPPED) && group_cached(gnd, f);
    bool hxd = ((gd != GROUP_UNMAPPED) && group_cached(gd, f))
            || (g_state.cfg.caps_has_scroll_bindings
                && gesture_has_scroll_bindings((GestureType)slot->bindings_drag));

    bool comp = (slot->is_second || slot->is_d_slot)
                && (f->cached_has_active_double_tap || f->cached_has_active_double_tap_drag);

    uint8_t plan_flags = 0;
    if (hx & (comp | hxd)) plan_flags |= PLAN_FLAG_PULSE_ON_UP;
    if (hx & comp & !hxd) plan_flags |= PLAN_FLAG_PRESS_ON_DRAG;
    if (hxd) plan_flags |= PLAN_FLAG_DRAG_AVAIL;
    r.plan = (GesturePairPlan){
        .hold_delay_ticks = 0,
        .flags = plan_flags,
    };
    return r;
}

// ============================================================
// Binding-only resolve — skips plan computation
// Used by handle_move where GesturePairPlan is not consumed.
// ============================================================
void gesture_resolve_bindings(
    const TouchFinger* f,
    const GesturePairSlot* slot,
    const TouchBinding** non_drag, int* non_drag_count,
    const TouchBinding** drag, int* drag_count)
{
    const FingerBindings* fb = &f->bindings;
    int gnd = resolve_group((GestureType)slot->bindings_non_drag);
    int gd  = resolve_group((GestureType)slot->bindings_drag);

    if (gnd == GROUP_UNMAPPED && gd == GROUP_UNMAPPED) {
        *non_drag = NULL; *non_drag_count = 0;
        *drag = NULL; *drag_count = 0;
        return;
    }

    if (gnd != GROUP_UNMAPPED) {
        *non_drag       = group_ptr(gnd, fb);
        *non_drag_count = group_cnt(gnd, fb);
    } else {
        *non_drag       = NULL;
        *non_drag_count = 0;
    }

    if (gd != GROUP_UNMAPPED) {
        *drag       = group_ptr(gd, fb);
        *drag_count = group_cnt(gd, fb);
    } else {
        *drag       = NULL;
        *drag_count = 0;
    }
}

// ============================================================
// Slot selection
// ============================================================
const GesturePairSlot* select_gesture_slot(const TouchFinger* f) {
    if (f->is_second_finger) {
        if (f->cached_has_active_long_press || f->cached_has_active_long_press_drag)
            return NULL;
        if (f->cached_has_active_double_tap || f->cached_has_active_double_tap_drag)
            return SLOT_D2();
        return SLOT_S2();
    }
    if (f->cached_has_active_long_press || f->cached_has_active_long_press_drag)
        return SLOT_L();
    if (f->cached_has_active_double_tap || f->cached_has_active_double_tap_drag)
        return SLOT_D();
    return SLOT_S();
}

static inline bool has_actual_dt_state(const GestureFingerCtx* ctx) {
    return ctx->deferred_double.count > 0 || ctx->dt.post_drag;
}

const GesturePairSlot* select_slot_common(
    const TouchFinger* f, const GestureFingerCtx* ctx, bool is_up_event)
{
    if (f->state == GESTURE_STATE_LONG_PRESSING)
        return f->is_second_finger ? NULL : SLOT_L();
    if (f->is_second_finger)
        return has_actual_dt_state(ctx) ? SLOT_D2() : SLOT_S2();
    if (!is_up_event && has_actual_dt_state(ctx))
        return SLOT_D();
    return SLOT_S();
}

// ============================================================
// Second-finger cleanup
// ============================================================
static void execute_pending_second_dt(TouchFinger* f, TouchActionResult* restrict result) {
    if (!f) return;
    int idx = (int)(f - g_state.fingers);
    if (idx < 0 || idx >= MAX_FINGERS) return;
    GestureFingerCtx* ctx = &g_ctx[idx];
    if (ctx->deferred_double.count > 0
        && !g_state.gesture_is_action_held) {
        execute_actions(result, ctx->deferred_double.items,
            ctx->deferred_double.count);
    }
    ctx->deferred_double.count = 0;
}

static void release_second_finger_buttons(TouchActionResult* restrict result,
                                           bool skip_release) {
    if (!skip_release) {
        release_held_actions(result, 0);
        add_action(result, ACT_POINTER_BUTTON_RELEASE, MOUSE_BTN_LEFT, 0, 0);
        add_action(result, ACT_POINTER_BUTTON_RELEASE, MOUSE_BTN_MIDDLE, 0, 0);
    }
}

static void deactivate_second_finger(TouchFinger* f) {
    gesture_clear_second_finger_state();
    gesture_clear_second_finger_globals();
    f->state = GESTURE_STATE_IDLE;
    deactivate_finger(f);
}

void cleanup_second_finger(TouchFinger* f, GestureFingerCtx* ctx,
                           TouchActionResult* restrict result) {
    execute_pending_second_dt(f, result);
    if (ctx->dt.waiting) {
        release_second_finger_buttons(result, false);
        g_state.gesture_second_active = false;
        g_state.gesture_second_ptr_id = INVALID_PTR_ID;
        f->state = GESTURE_STATE_DOUBLE_TAP_WAITING;
        return;
    }
    bool skip_release = ctx->dt.post_drag && g_state.gesture_is_action_held;
    release_second_finger_buttons(result, skip_release);
    deactivate_second_finger(f);
}

// ============================================================
// G_ctx lifecycle
// ============================================================
void g_ctx_init(GestureFingerCtx* ctx) {
    memset(ctx, 0, sizeof(*ctx));
}
