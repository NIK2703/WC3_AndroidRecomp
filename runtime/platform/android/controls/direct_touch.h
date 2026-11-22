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

typedef struct {
    int pointer_id;
    float down_x;
    float down_y;
    float last_x;
    float last_y;
    uint64_t down_ms;
    uint32_t route;      /* Wc3DirectTouchRoute that owns the gesture */
    uint8_t active;
    uint8_t moved;
    uint8_t multi;
    uint8_t long_fired;
    uint8_t mode;        /* Wc3DirectTouchFieldMode */
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
 * A FIELD down stays pending: a move beyond drag_threshold_px before the
 * long-press deadline emits FIELD_CAMERA_BEGIN.  At the deadline the pointer
 * emits FIELD_SELECT_BEGIN and stays in FIELD_SELECT until the finger is
 * released; every move in between emits FIELD_SELECT_MOVE and the release
 * emits FIELD_SELECT_END.  The executor turns BEGIN into a native left-button
 * DOWN, each MOVE into a native mouse move, and END into the matching UP, so a
 * stationary hold is a native click and a drag is a native box selection.
 * Raw pointer phases always carry NATIVE. */
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
 * of holds armed.  The pointer then stays in FIELD_SELECT: moves emit
 * FIELD_SELECT_MOVE and the release emits FIELD_SELECT_END, so the executor
 * can keep the native left button down for the whole gesture.
 */
int wc3_direct_touch_tick(Wc3DirectTouch *dt, uint64_t now_ms);

/* Earliest long-press deadline, or 0 when none is pending. */
uint64_t wc3_direct_touch_next_deadline_ms(const Wc3DirectTouch *dt);

/* Number of active pointers. */
int wc3_direct_touch_active_count(const Wc3DirectTouch *dt);

/* True while a FIELD pointer owns a native camera or selection drag.  The
 * controls layer uses it to swallow a second finger instead of handing the
 * gesture to the vendor engine, which would leave the native button held. */
int wc3_direct_touch_field_drag_active(const Wc3DirectTouch *dt);

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

#ifdef __cplusplus
}
#endif

#endif /* WC3_DIRECT_TOUCH_H */
