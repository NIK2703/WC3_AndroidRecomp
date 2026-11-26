/*
 * direct_touch.h - host-side gesture recognizer for semantic direct touch.
 *
 * This layer owns only touch classification.  It does not know about guest
 * addresses, Win32 messages, or the overlay profile; the controls adapter
 * decides whether a pointer belongs here and forwards the resulting tap,
 * long-press or portrait-follow event to the guest-thread service.
 */
#ifndef WC3_DIRECT_TOUCH_H
#define WC3_DIRECT_TOUCH_H

#include <stdint.h>

#include "direct_touch_event.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(WC3_DEBUG) && WC3_DEBUG && \
    defined(WC3_DEBUG_INPUT) && WC3_DEBUG_INPUT
/* Debug-only file trace shared by the controls router and the recognizer.
 * Writes to touch_trace.txt under WC3_DATA_DIR; without that variable it is
 * a no-op, so host tests do not create files. */
void wc3_direct_touch_debug_log(const char *fmt, ...);
#endif

#define WC3_DIRECT_TOUCH_MAX_POINTERS 4

/* Field gesture state.  A quick move before the long-press deadline becomes
 * a camera drag; a move after the deadline becomes a selection drag.  The
 * legacy profile uses one drag threshold for both, so there is no separate
 * long-press slop. */
typedef enum {
    WC3_DIRECT_TOUCH_FIELD_PENDING = 0,
    WC3_DIRECT_TOUCH_FIELD_CAMERA,
    WC3_DIRECT_TOUCH_FIELD_SELECT
} Wc3DirectTouchFieldMode;

/*
 * A FIELD contact starts UNASSIGNED.  Any contact that crosses the drag
 * threshold before its long-press deadline claims the CAMERA role; a contact
 * that reaches the deadline instead becomes ACTION and owns the virtual
 * mouse.  Several contacts may own CAMERA at once, each with its own gesture
 * id, so a second quick swipe joins an already active camera pan instead of
 * being forced to ACTION.  CAMERA never touches the virtual mouse.
 */
typedef enum {
    WC3_DT_CONTACT_UNASSIGNED = 0,
    WC3_DT_CONTACT_ACTION     = 1,
    WC3_DT_CONTACT_CAMERA     = 2,
    WC3_DT_CONTACT_IGNORED    = 3
} Wc3DirectTouchContactRole;

typedef struct {
    int pointer_id;
    float down_x;
    float down_y;
    float last_x;
    float last_y;
    /* Point where the long-press deadline was recognized.  A post-deadline
     * move beyond drag_threshold_px from here is the first real drag, not a
     * long tap. */
    float hold_x;
    float hold_y;
    uint64_t down_ms;
    uint32_t route;      /* Wc3DirectTouchRoute that owns the gesture */
    uint32_t gesture_id; /* monotonic id of the claimed camera gesture */
    uint8_t active;
    uint8_t moved;
    uint8_t multi;
    uint8_t long_fired;
    uint8_t mode;        /* Wc3DirectTouchFieldMode */
    uint8_t role;        /* Wc3DirectTouchContactRole */
    uint8_t camera_claim;/* this FIELD contact may still claim the camera */
    uint8_t drag_started;/* FIELD_SELECT_DRAG_BEGIN already emitted */
} Wc3DirectTouchPointer;

/* Legacy haptic callback.  It runs on the Android input thread when the
 * long-press deadline expires; effect is the profile's
 * gestureLongPressHaptic value (3 in the authored Warcraft 3 profile). */
typedef void (*Wc3DirectTouchHaptic)(int effect, void *user);

typedef struct {
    float width;
    float height;
    float drag_threshold_px;
    int long_press_ms;
    int haptic_effect;
    Wc3DirectTouchSink sink;
    void *user;
    Wc3DirectTouchHaptic haptic;
    void *haptic_user;
    uint32_t g_camera_seq;       /* monotonic camera gesture id */
    Wc3DirectTouchPointer pointers[WC3_DIRECT_TOUCH_MAX_POINTERS];
} Wc3DirectTouch;

/*
 * Configure a recognizer instance.  drag_threshold_px is the legacy
 * dragThreshold (11 px in the authored profile); it decides tap versus
 * camera drag and hold versus selection drag.  long_press_ms is the
 * profile's gesture longPressTimeout (120 ms in the authored profile).  A
 * long_press_ms <= 0 arms the hold on the next tick.
 */
void wc3_direct_touch_init(Wc3DirectTouch *dt, float width, float height,
                           float drag_threshold_px, int long_press_ms,
                           Wc3DirectTouchSink sink, void *user);

void wc3_direct_touch_set_sink(Wc3DirectTouch *dt, Wc3DirectTouchSink sink,
                               void *user);

/* Install the legacy long-press haptic.  effect <= 0 disables it.  The
 * callback and effect survive set_size/init only when the controls layer
 * reapplies them; controls_set_direct_touch_haptic_sink() does that. */
void wc3_direct_touch_set_haptic(Wc3DirectTouch *dt,
                                 Wc3DirectTouchHaptic haptic, void *user,
                                 int effect);

void wc3_direct_touch_set_size(Wc3DirectTouch *dt, float width, float height);

/* Feed one pointer.  x,y are surface pixels.  The route is attached to every
 * semantic event the pointer produces; wc3_direct_touch_down() is the FIELD
 * shorthand.  A PORTRAIT down emits FOLLOW_START immediately and keeps that
 * pointer owned by the portrait until its UP, regardless of later movement.
 * A FIELD down stays pending: a move beyond drag_threshold_px before its own
 * long-press deadline claims the CAMERA role and emits FIELD_CAMERA_BEGIN with
 * an owning gesture_id; that drag uses no virtual mouse.  This is true for any
 * FIELD pointer, including a second finger while another finger already owns
 * the camera or the ACTION cursor.  At the deadline the pointer becomes
 * ACTION and emits FIELD_SELECT_BEGIN.  The executor normally turns BEGIN
 * into a native left-button DOWN; for a controlled selection in the field it
 * may defer that press.  A post-deadline move beyond drag_threshold_px from
 * the hold point then emits FIELD_SELECT_DRAG_BEGIN once, carrying the hold
 * point, followed by FIELD_SELECT_MOVE; the release emits FIELD_SELECT_END.
 * The executor turns DRAG_BEGIN into the native DOWN at the hold point, each
 * MOVE into a native mouse move, and END into the matching UP, the smart
 * order, or the native fallback click.  A hover is emitted immediately before
 * an action click, never for the camera.  Raw pointer phases carry the
 * ACTION role and either NATIVE or MINIMAP as their physical owner. */
void wc3_direct_touch_down_route(Wc3DirectTouch *dt, int pointer_id,
                                 float x, float y, uint64_t now_ms,
                                 uint32_t route);
void wc3_direct_touch_down(Wc3DirectTouch *dt, int pointer_id,
                           float x, float y, uint64_t now_ms);
void wc3_direct_touch_move(Wc3DirectTouch *dt, int pointer_id,
                           float x, float y, uint64_t now_ms);
void wc3_direct_touch_up(Wc3DirectTouch *dt, int pointer_id,
                         float x, float y, uint64_t now_ms);

/* Cancel every active pointer.  A PORTRAIT pointer emits HOLD_RELEASE, and a
 * FIELD camera/selection drag emits its END event, so no native mouse button
 * can stay latched. */
void wc3_direct_touch_reset(Wc3DirectTouch *dt);

/*
 * Arm field holds whose deadline has expired, fire the haptic once per
 * pointer, and emit FIELD_SELECT_BEGIN at the hold point.  Returns the number
 * of holds armed.  The pointer then stays in FIELD_SELECT: a move beyond
 * drag_threshold_px from the hold point emits FIELD_SELECT_DRAG_BEGIN once,
 * later moves emit FIELD_SELECT_MOVE, and the release emits FIELD_SELECT_END.
 * The executor normally keeps the native left button down for the whole
 * gesture; for a deferred order it starts the press at DRAG_BEGIN or
 * resolves the smart order/attack-move/fallback on END.
 */
int wc3_direct_touch_tick(Wc3DirectTouch *dt, uint64_t now_ms);

/* Earliest long-press deadline, or 0 when none is pending. */
uint64_t wc3_direct_touch_next_deadline_ms(const Wc3DirectTouch *dt);

/* Number of active pointers. */
int wc3_direct_touch_active_count(const Wc3DirectTouch *dt);

/* True while a FIELD pointer is in the long-press selection phase.  That
 * includes a deferred press before DRAG_BEGIN, which does not own the native
 * button yet.  A camera drag does not own a mouse button, so it is not
 * included here. */
int wc3_direct_touch_field_drag_active(const Wc3DirectTouch *dt);

/* Role/gesture queries for the controls adapter.  -1 means "none". */
int wc3_direct_touch_camera_pointer(const Wc3DirectTouch *dt);
int wc3_direct_touch_action_pointer(const Wc3DirectTouch *dt);
uint32_t wc3_direct_touch_contact_role(const Wc3DirectTouch *dt, int pointer_id);
uint32_t wc3_direct_touch_gesture_id(const Wc3DirectTouch *dt, int pointer_id);

/* Copy one active pointer's last position.  Returns 1 on success. */
int wc3_direct_touch_pointer_at(const Wc3DirectTouch *dt, int index,
                                int *pointer_id, float *x, float *y);

/*
 * Forward one raw pointer phase to the guest sink.  Native UI touches use
 * this instead of the semantic recognizer: the guest-side service posts the
 * same Win32 mouse messages a real mouse produces, so command-card clicks,
 * menus and targeting/placement keep their native behaviour.  A minimap drag
 * uses this path too, but the controls adapter clamps its phases to the
 * published minimap rectangle so leaving the minimap does not end the drag.
 * The portrait does not use this path; it has its own FOLLOW_START/
 * HOLD_RELEASE events.  x,y are surface pixels, like the recognizer input.
 */
void wc3_direct_touch_emit_pointer(Wc3DirectTouch *dt,
                                   Wc3DirectTouchKind kind, int pointer_id,
                                   float x, float y, uint64_t now_ms);

/* Same raw pointer phases, but carry the physical owner in the event.  The
 * minimap uses MINIMAP so the guest can drop a stale minimap drag when a
 * native menu opens; every other caller uses the NATIVE wrapper above. */
void wc3_direct_touch_emit_pointer_route(Wc3DirectTouch *dt,
                                         Wc3DirectTouchKind kind,
                                         int pointer_id, float x, float y,
                                         uint64_t now_ms, uint32_t route);

#ifdef __cplusplus
}
#endif

#endif /* WC3_DIRECT_TOUCH_H */
