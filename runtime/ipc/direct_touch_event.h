/*
 * direct_touch_event.h - semantic direct-touch events across the host/HLE
 * boundary.
 *
 * The direct-touch recognizer in runtime/platform/android/controls runs on
 * the Android input thread.  It never calls guest code: it publishes one
 * fixed-size, pointer-free event here, and the main guest thread pops it at
 * the message-pump boundary and resolves it against the live game objects.
 * Keep this type POD and free of guest addresses; the queue must survive a
 * guest map change.
 */
#ifndef WC3_DIRECT_TOUCH_EVENT_H
#define WC3_DIRECT_TOUCH_EVENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WC3_DIRECT_TOUCH_NONE = 0,
    /* Semantic battlefield gestures: resolved by the guest-side executor
     * against the live selection/world state. */
    WC3_DIRECT_TOUCH_TAP = 1,
    /* Reserved.  The field long press is now a native left-button press
     * expressed as FIELD_SELECT_BEGIN at the deadline.  Keep the value for
     * queued events from an older build; the executor no longer runs a
     * semantic selection for it. */
    WC3_DIRECT_TOUCH_LONG_PRESS = 2,
    /* Raw pointer phases for native UI: minimap, command card, resource
     * bar, menus, and targeting/placement.  The guest-side service turns
     * these into the same Win32 mouse messages a real mouse produces; it
     * does not go through the overlay touch engine.  A minimap drag uses
     * these phases too, but the controls layer clamps them to the published
     * minimap rectangle so leaving the minimap does not end the drag.  The
     * selected-unit grid and the inventory bar also arrive as these phases.
     * The guest-side executor claims a gesture that starts on a grid button
     * (drives the native 0x50065 behavior on DOWN/MOVE) or on an occupied
     * inventory slot (replays the native left/right click pair on UP), and
     * otherwise posts the mouse click.  The portrait has its own
     * FOLLOW_START/HOLD_RELEASE events instead. */
    WC3_DIRECT_TOUCH_POINTER_DOWN = 3,
    WC3_DIRECT_TOUCH_POINTER_MOVE = 4,
    WC3_DIRECT_TOUCH_POINTER_UP = 5,
    WC3_DIRECT_TOUCH_POINTER_CANCEL = 6,
    /* Portrait follow ended.  Emitted on UP and on cancellation for every
     * portrait-owned pointer, so a released follow can never stay latched.
     * It is not tied to a long-press deadline. */
    WC3_DIRECT_TOUCH_HOLD_RELEASE = 7,
    /* Portrait follow started.  Emitted immediately on DOWN over the
     * visible portrait; the guest-side executor starts the native camera
     * follow for the displayed unit.  The follow stays active while that
     * finger is on the screen, even if it moves outside the portrait. */
    WC3_DIRECT_TOUCH_FOLLOW_START = 8,
    /* Field camera drag.  A quick swipe on the battlefield starts the same
     * native middle-button drag the legacy profile binds to
     * singleTapDragAction; the guest-side executor posts WM_MBUTTONDOWN/
     * WM_MOUSEMOVE/WM_MBUTTONUP.  BEGIN carries the point where the drag
     * crossed the legacy drag threshold, MOVE the later positions, and END
     * the release.  These events are only valid on the FIELD route. */
    WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN = 9,
    WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE = 10,
    WC3_DIRECT_TOUCH_FIELD_CAMERA_END = 11,
    /* Field selection press.  BEGIN is emitted at the long-press deadline:
     * the executor posts WM_LBUTTONDOWN and keeps it down.  MOVE carries
     * every later finger position and END carries the release, which posts
     * the matching WM_LBUTTONUP.  A stationary release is therefore a native
     * left click that selects one unit; a drag is the native selection
     * rectangle (legacy longPressDragAction).  These events are only valid
     * on the FIELD route. */
    WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN = 12,
    WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE = 13,
    WC3_DIRECT_TOUCH_FIELD_SELECT_END = 14
} Wc3DirectTouchKind;

/*
 * Where the Android input thread should send a non-overlay touch.  The guest
 * thread publishes this lock-free snapshot; the input thread never calls
 * guest code.
 *
 * LEGACY: direct touch cannot claim the pointer yet (UI singleton not up,
 *         the host has no sink), or the touch hits an overlay control
 *         element.  This is not a per-feature fallback.
 * FIELD:  the touch is in the battlefield and CSelectMode owns input.  Use
 *         the semantic tap recognizer, the native middle-button camera drag
 *         for a quick swipe, and the native left-button selection press at
 *         the long-press deadline.  The left button stays down until the
 *         finger is released, so a stationary hold is a native click and a
 *         drag is a native box selection.
 * NATIVE: the touch belongs to the native UI (HUD, menu, targeting or
 *         placement).  Forward raw pointer phases so the guest-side service
 *         can post native mouse messages.  The service claims a gesture that
 *         starts on a selected-unit grid button or an occupied inventory slot
 *         and drives the native command path itself; every other phase is
 *         forwarded unchanged.
 * PORTRAIT: the touch is inside the visible portrait frame.  Follow the
 *         displayed unit immediately on DOWN, keep following while that
 *         finger is down even if it leaves the rectangle, and clear the
 *         follow on UP/cancel.  This is a separate route because the native
 *         portrait frame has no click action of its own.
 * MINIMAP: the touch started inside the visible minimap frame.  The native
 *         pointer phases stay owned by the minimap for the whole gesture and
 *         are clamped to its rectangle, so dragging the finger across the
 *         screen keeps moving the camera until the finger is released.
 */
typedef enum {
    WC3_DIRECT_TOUCH_ROUTE_LEGACY = 0,
    WC3_DIRECT_TOUCH_ROUTE_FIELD = 1,
    WC3_DIRECT_TOUCH_ROUTE_NATIVE = 2,
    WC3_DIRECT_TOUCH_ROUTE_PORTRAIT = 3,
    WC3_DIRECT_TOUCH_ROUTE_MINIMAP = 4
} Wc3DirectTouchRoute;

typedef struct {
    uint32_t kind;       /* Wc3DirectTouchKind */
    uint32_t pointer_id; /* source pointer, diagnostics only */
    float x;             /* normalised surface x, 0..1, top-left origin */
    float y;             /* normalised surface y, 0..1, top-left origin */
    uint64_t time_ms;    /* monotonic ms (uptimeMillis/CLOCK_MONOTONIC) */
    uint32_t route;      /* Wc3DirectTouchRoute that produced the gesture */
} Wc3DirectTouchEvent;

/* The recognizer publishes through this sink.  It runs on the Android input
 * thread and must not call guest code. */
typedef void (*Wc3DirectTouchSink)(const Wc3DirectTouchEvent *event,
                                   void *user);

/* 512 events: raw pointer phases add one MOVE per touch frame, so a slow
 * guest pump must buffer a drag without losing its UP.  The producer is the
 * single Android input thread; the consumer is the main guest thread.  At
 * 120 Hz this is a 4 s stall; the service drains 16 events per pump. */
#define WC3_DIRECT_TOUCH_QUEUE_CAP 512u

/* Producer wake.  Called after a successful push with no queue lock held, so
 * the HLE can wake its blocking GetMessageA wait without this module
 * depending on the Win32 queue. */
typedef void (*Wc3DirectTouchWake)(void *user);
void wc3_direct_touch_set_wake(Wc3DirectTouchWake wake, void *user);

/*
 * Producer side.  Returns 1 when queued, 0 when the queue is full.  Called
 * from the Android input thread; the implementation is lock-free.
 */
int wc3_direct_touch_push(const Wc3DirectTouchEvent *event);

/* Consumer side.  Returns 1 when an event was removed. */
int wc3_direct_touch_pop(Wc3DirectTouchEvent *event);

/* Number of queued events.  Safe for a wake-up predicate on either side. */
uint32_t wc3_direct_touch_pending(void);

/* Drop all queued events.  Used when the game leaves the in-game UI or the
 * guest UI singleton changes, so a stale tap cannot hit the next map.
 * Consumer-only: call from the main guest thread, or while the producer is
 * stopped.  The producer must not clear a queue it is concurrently pushing. */
void wc3_direct_touch_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_DIRECT_TOUCH_EVENT_H */
