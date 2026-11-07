#ifndef GESTURE_TYPES_H
#define GESTURE_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "core/processor.h"

// ---- Constants ----
#ifndef MOUSE_BTN_LEFT
#define MOUSE_BTN_LEFT   0
#endif
#ifndef MOUSE_BTN_MIDDLE
#define MOUSE_BTN_MIDDLE 1
#endif
#ifndef MOUSE_BTN_RIGHT
#define MOUSE_BTN_RIGHT  2
#endif
#ifndef FALLBACK_MAX
#define FALLBACK_MAX 8
#endif
#ifndef GESTURE_SLOT_COUNT
#define GESTURE_SLOT_COUNT 5
#endif
// TP_EARLY_EXIT_MS removed — was unused
#define INVALID_PTR_ID (-1)
#define TIMER_ELAPSED(now, start, timeout) ((now) - (start) >= (uint64_t)(timeout))

// ============================================================
// Gesture pair plan — output of resolve_gesture_pair
// ============================================================
typedef struct {
    int16_t hold_delay_ticks;     // > 0: start timer; on expiry: execute
    uint8_t flags;            // bit 0=pulse_on_up, bit 1=press_on_drag, bit 2=drag_available
} GesturePairPlan;

#define PLAN_FLAG_PULSE_ON_UP   (1 << 0)
#define PLAN_FLAG_PRESS_ON_DRAG (1 << 1)
#define PLAN_FLAG_DRAG_AVAIL    (1 << 2)

// Gesture handler tick (processes long-press, single-tap-hold, double-tap, second-finger timeouts)
void gesture_tick(uint64_t now_tick, TouchActionResult* restrict result);

// Gesture handler state machine
void on_drag_start(TouchFinger* f, TouchActionResult* restrict result);
bool gesture_is_within_tap_distance(float x, float y);
void gesture_cancel_double_tap_wait(TouchActionResult* restrict result);
void double_tap_confirm_internal(TouchActionResult* restrict result, TouchFinger* f);

// Unified tap execution helpers (shared across base.c, handler.c)
void execute_tap_on_finger_down(TouchFinger* f, TouchActionResult* restrict result,
    uint64_t now_tick, GesturePairPlan plan, bool force_hold,
    const TouchBinding* x, int x_cnt);
bool confirm_double_tap(TouchActionResult* restrict result,
    GesturePairPlan d_plan,
    const TouchBinding* src, int src_count,
    TouchBinding* dst, int* dst_count, int dst_max,
    bool* out_post_dtd, bool has_scroll_bindings);

// Unified DT_WAITING entry — handles both main and second finger.
// When is_second=true, writes ctx->dt (start_tick, ref_x, ref_y).
// When is_second=false, writes ctx->dt (start_tick, tap_up_x, tap_up_y).
void enter_dt_waiting(TouchFinger* f, uint64_t now_tick, bool is_second,
    const TouchBinding* deferred_single, int deferred_single_count,
    const TouchBinding* deferred_double, int deferred_double_count);

// ============================================================
// BindingSlot — reusable container for a fixed-capacity binding array
// ============================================================
typedef struct {
    TouchBinding items[FALLBACK_MAX];
    int count;
} BindingSlot;

// ============================================================
// Per-finger double-tap waiting context
// Replaces 7 global variables with per-finger state.
// ============================================================
typedef struct {
    bool waiting;
    uint64_t start_tick;
    float ref_x, ref_y;            // second finger: SDTW reference position
    float tap_up_x, tap_up_y;      // main finger: last tap-up position
    bool post_drag;                 // post-double-tap-drag flag (per-finger)
} GestureDTWaitCtx;

// ============================================================
// Unified per-finger gesture context
// Replaces all per-finger global arrays (main vs second finger)
// ============================================================
typedef struct {
    bool dt_consumed;

    GestureDTWaitCtx dt;           // double-tap waiting state (replaces 7 globals)

    BindingSlot fallback;
    BindingSlot pending_double;
    BindingSlot deferred_double;
    BindingSlot pending_long_press;   // set by tick_lp_timer (pulse_on_up+drag), consumed by up_handle_long_pressing
} GestureFingerCtx;

// ============================================================
// Gesture event type for unified process function
// ============================================================
typedef enum {
    GESTURE_EVENT_DOWN,
    GESTURE_EVENT_MOVE,
    GESTURE_EVENT_UP,
    GESTURE_EVENT_TICK,
    GESTURE_EVENT_COUNT
} GestureProcessEvent;

// ============================================================
// Gesture pair slot — encodes ALL differences between gesture
// pairs (S/Sd, D/Dd, L/Ld, S2/Sd2, D2/Dd2) as data.
// ============================================================
typedef struct {
    uint8_t bindings_non_drag;  // GestureType (values 0–9)
    uint8_t bindings_drag;      // GestureType (values 0–9)
    bool is_d_slot;     // D/Dd or D2/Dd2 (controls move re-bind + cleanup)
    bool is_second;     // S2/D2 slot (drives sdtw, fallback, hold_execute)
} GesturePairSlot;

// Per-finger context array (defined in unified.c)
extern GestureFingerCtx g_ctx[MAX_FINGERS];

// Init per-finger context
void g_ctx_init(GestureFingerCtx* ctx);

// ---- Unified slot / binding / plan helpers (defined in unified.c) ----
const GesturePairSlot* select_gesture_slot(const TouchFinger* f);

// Full pipeline: plan + bindings in one call
typedef struct {
    GesturePairPlan plan;
    const TouchBinding* non_drag;
    int non_drag_count;
    const TouchBinding* drag;
    int drag_count;
} GesturePairResult;

GesturePairResult gesture_resolve(
    const TouchFinger* f,
    const GesturePairSlot* slot);

// Lightweight binding-only resolve (skips plan computation).
// Used by handle_move where plan is not needed.
void gesture_resolve_bindings(
    const TouchFinger* f,
    const GesturePairSlot* slot,
    const TouchBinding** non_drag, int* non_drag_count,
    const TouchBinding** drag, int* drag_count);

// Global gesture slot table (defined in unified.c)
extern const GesturePairSlot GESTURE_SLOTS[GESTURE_SLOT_COUNT];

// ---- Named slot accessors ----
#define SLOT_S()   (&GESTURE_SLOTS[0])   // S/Sd  (main single tap)
#define SLOT_D()   (&GESTURE_SLOTS[1])   // D/Dd  (main double tap)
#define SLOT_L()   (&GESTURE_SLOTS[2])   // L/Ld  (main long press)
#define SLOT_S2()  (&GESTURE_SLOTS[3])   // S2/Sd2 (second single tap)
#define SLOT_D2()  (&GESTURE_SLOTS[4])   // D2/Dd2 (second double tap)

// ---- Slot selection helpers (defined in unified.c) ----
// Trivial selectors are macros (eliminates function call overhead).
#define SELECT_DT_SLOT(f) ((f)->is_second_finger ? SLOT_D2() : SLOT_D())
#define SELECT_S_SLOT(f) ((f)->is_second_finger ? SLOT_S2() : SLOT_S())
const GesturePairSlot* select_slot_common(const TouchFinger* f, const GestureFingerCtx* ctx, bool is_up_event);

// ---- Second-finger cleanup (defined in unified.c) ----
void cleanup_second_finger(TouchFinger* f, GestureFingerCtx* ctx,
                           TouchActionResult* restrict result);

// ---- Branch-local types (moved from branch.c) ----

typedef enum {
    TAP_PATH_HANDLED,
    TAP_PATH_ENTER_DT,
    TAP_PATH_SINGLE,
} TapPathResult;

typedef struct {
    const TouchBinding* non_drag;
    int non_drag_count;
    const TouchBinding* fallback;
    int fallback_count;
} TapPathParams;

typedef enum { ROLE_MAIN, ROLE_SECOND, ROLE_IGNORED } DownRole;

// ============================================================
// Unified drag/non-drag branching — data pipeline
//
// resolve_move() is PURE: reads MoveInput, writes MoveOutput.
//   No f-> access, no g_state mutation, no side effects.
//
// apply_move() executes all side effects described by MoveOutput.
//
// Caller workflow for GESTURE_EVENT_MOVE:
//   1. If f->scroll_mode → call scroll_mode_process(), return.
//   2. Populate MoveInput from f, ctx, g_state, slot.
//   3. MoveOutput = resolve_move(&input).
//   4. apply_move(&output, f, ctx, result).
// ============================================================

#define MOVE_MAX_BINDINGS 8

// Slot mode: pre-resolved mode for TAP_WAITING move handling
typedef enum {
    SLOT_MODE_GESTURE,      // has gesture bindings (normal drag path)
    SLOT_MODE_SCROLL,       // has scroll bindings (DRAG_SCROLL path)
    SLOT_MODE_NO_GESTURE,   // no bindings at all (early DRAG_START)
} SlotMoveMode;

// MoveInput flag bitmask
#define MOVEIN_FLAG_SECOND_HELD    (1 << 0)
#define MOVEIN_FLAG_D_CLEANUP      (1 << 1)
#define MOVEIN_FLAG_MARK_SECOND    (1 << 2)

// Pure input snapshot — all data resolve_move needs.
// Caller extracts from f, ctx, g_state before calling resolve_move.
// NO is_second_finger, NO is_d_slot, NO raw mode data — all pre-computed.
typedef struct {
    float dx, dy;                    // 8 bytes

    // Pre-resolved bindings (pointers into caller-owned arrays)
    const TouchBinding* non_drag;    // 8 bytes
    const TouchBinding* drag;        // 8 bytes

    // Counts and threshold
    int non_drag_count;              // 4 bytes
    int drag_count;                  // 4 bytes
    int drag_threshold_px;           // 4 bytes

    // Packed small fields (1 byte each)
    uint8_t state;                   // GestureState (values 0–4)
    uint8_t slot_mode;               // SlotMoveMode (values 0–2)
    uint8_t scroll_type;             // GestureType (values 0–9)
    uint8_t flags;                   // bitmask: SECOND_HELD|D_CLEANUP|MARK_SECOND
    bool dt_waiting;                 // 1 byte
    uint8_t _pad[3];                 // alignment
} MoveInput;

// MoveOutput flag bitmask
#define MOVEOUT_FLAG_CLEAR_PENDING   (1 << 0)
#define MOVEOUT_FLAG_D_CLEANUP       (1 << 1)
#define MOVEOUT_FLAG_MARK_SECOND     (1 << 2)
#define MOVEOUT_FLAG_CANCEL_DT       (1 << 3)
#define MOVEOUT_FLAG_CLEAR_LP        (1 << 4)
#define MOVEOUT_FLAG_RELEASE_ALL     (1 << 5)

// Pure output — complete description of all side effects.
typedef struct {
    enum { DRAG_SKIP, DRAG_START, DRAG_SCROLL, DRAG_HOLD } action;  // 4 bytes

    // Bindings to execute/held when starting drag
    TouchBinding execute[4];         // 32 bytes (down from 8)
    int execute_count;               // 4 bytes

    // For DRAG_SCROLL
    TouchFinger* scroll_finger;      // 8 bytes — pre-resolved main finger for 2-finger scroll

    // Packed small fields (1 byte each)
    uint8_t scroll_type;             // GestureType (values 0–9)
    uint8_t flags;                   // bitmask of MOVEOUT_FLAG_*
    uint8_t _pad[4];                 // alignment
} MoveOutput;

// Pure: reads MoveInput, writes MoveOutput. No side effects.
MoveOutput resolve_move(const MoveInput* in);

// Executes all side effects described by MoveOutput.
// scroll_finger/scroll_ctx: for 2-finger scroll, pass main finger; else pass f/ctx.
void apply_move(const MoveOutput* out, TouchFinger* f,
                GestureFingerCtx* ctx, TouchActionResult* restrict result);

#ifdef __cplusplus
}
#endif

#endif // GESTURE_TYPES_H
