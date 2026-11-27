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
    /* Field camera drag.  A quick swipe on the battlefield starts a native
     * camera drag, but the guest-side executor drives the drag manager
     * directly: these events own no virtual mouse button and post no
     * WM_*MOUSE* messages.  BEGIN carries the point where the drag crossed
     * the legacy drag threshold, MOVE the later positions, and END the
     * release.  owner_role is CAMERA and gesture_id identifies one
     * continuous drag.  Several fingers may own camera gestures at once;
     * each has its own gesture_id and contributes its relative movement. */
    WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN = 9,
    WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE = 10,
    WC3_DIRECT_TOUCH_FIELD_CAMERA_END = 11,
    /* Field selection press.  BEGIN is emitted at the long-press deadline.
     * Normally the executor posts WM_LBUTTONDOWN and keeps it down.  When the
     * target's long-tap leg is selection, the press starts at BEGIN, so a
     * stationary release selects the target and a drag extends the native
     * selection rectangle immediately (no drag-threshold wait).  For the
     * deferred order path the executor may withhold the press until
     * DRAG_BEGIN (the contact is a real drag) or END (a smart order or
     * attack-move), so a stationary release does not become a selection
     * click.  MOVE carries every later finger position and END carries the
     * release; a started native press posts the matching WM_LBUTTONUP.
     * These events are only valid on the FIELD route. */
    WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN = 12,
    WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE = 13,
    WC3_DIRECT_TOUCH_FIELD_SELECT_END = 14,
    /* The held contact first moved farther than the configured drag threshold
     * from the point where FIELD_SELECT_BEGIN was emitted.  Carries that hold
     * point, not the current finger point; the following FIELD_SELECT_MOVE
     * carries the current point.  Emitted once per contact.  The executor
     * uses it to start the native selection rectangle only when the gesture
     * is actually a drag, so a stationary long tap can become a smart
     * order. */
    WC3_DIRECT_TOUCH_FIELD_SELECT_DRAG_BEGIN = 18,
    /* Field press hover.  Emitted immediately before an action click (TAP or
     * FIELD_SELECT_BEGIN), after the contact's role is known.  A camera
     * contact emits no hover.  The service posts it as a native WM_MOUSEMOVE
     * when the live mode is target/transient, and stops the batch so the click
     * is a later message.  Without it the target mode keeps the previous
     * contact's cursor and the first click after a command card is resolved
     * against the old position.  In select mode this event is ignored. */
    WC3_DIRECT_TOUCH_FIELD_HOVER = 15,
    /* Direct key signal.  In direct mode an overlay key action is removed
     * from the keyboard action stream and sent here instead.  pointer_id
     * carries the Windows virtual key and route carries 1 for press and 0
     * for release.  This covers overlay buttons, their gestures and long
     * presses alike: every key binding the vendored engine fires goes
     * through this event.  The guest service invokes its own native signal
     * for the key (SetKeyState for arrows, the key-binding dispatcher for
     * other keys), so the action assigned to the key or key combination in
     * the game fires without a keyboard message. */
    WC3_DIRECT_TOUCH_KEY_SIGNAL = 16,
    /* Direct control-group action.  A chord binding from the vendored engine
     * is [KEY_N, MOD_CTRL] or [KEY_N, MOD_SHIFT].  Instead of publishing the
     * chord as a modifier state plus a key action, controls publishes one of
     * these events: pointer_id carries the group number (1..8) and route
     * carries the action (0 = recall, 1 = store, 2 = add to selection).  The
     * guest service resolves it with the game's own control-group senders;
     * no keyboard message and no separate modifier signal is involved. */
    WC3_DIRECT_TOUCH_GROUP_SIGNAL = 17
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
 *         the semantic tap recognizer, a cursor-free camera drag for a quick
 *         swipe, and the native left-button selection press at the long-press
 *         deadline.  The left button normally stays down until the finger is
 *         released, so a stationary hold is a native click and a drag is a
 *         native box selection.  When a controlled selection is held in the
 *         field the press is deferred: DRAG_BEGIN starts the box at the hold
 *         point, while a stationary release becomes a smart order (unit or
 *         decoration) or attack-move (terrain).  The camera drag does not
 *         own the mouse; a second finger can remain the virtual cursor or
 *         join the camera pan with its own quick swipe.
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
 * MENU:   the guest has an open native menu (the pause/ESC menu).  The input
 *         thread must use exactly the main-menu gesture set: raw pointer
 *         phases, no battlefield, portrait, minimap, command-card or beacon
 *         gesture, regardless of the point's geometry.  The service accepts
 *         only key signals and native pointer phases while this route is
 *         published; semantic world events are dropped.
 */
typedef enum {
    WC3_DIRECT_TOUCH_ROUTE_LEGACY = 0,
    WC3_DIRECT_TOUCH_ROUTE_FIELD = 1,
    WC3_DIRECT_TOUCH_ROUTE_NATIVE = 2,
    WC3_DIRECT_TOUCH_ROUTE_PORTRAIT = 3,
    WC3_DIRECT_TOUCH_ROUTE_MINIMAP = 4,
    WC3_DIRECT_TOUCH_ROUTE_MENU = 5
} Wc3DirectTouchRoute;

/*
 * Which gesture owns a semantic contact.  A camera contact must never post
 * mouse messages or move the shared cursor; an action contact is the only
 * owner of the virtual mouse.  Zero is treated as ACTION so events queued by
 * an older build stay harmless.
 */
typedef enum {
    WC3_DT_OWNER_ACTION = 1u,
    WC3_DT_OWNER_CAMERA = 2u
} Wc3DirectTouchOwnerRole;

typedef struct {
    uint32_t kind;       /* Wc3DirectTouchKind */
    uint32_t pointer_id; /* source pointer, diagnostics only */
    float x;             /* normalised surface x, 0..1, top-left origin */
    float y;             /* normalised surface y, 0..1, top-left origin */
    uint64_t time_ms;    /* monotonic ms (uptimeMillis/CLOCK_MONOTONIC) */
    uint32_t route;      /* Wc3DirectTouchRoute that produced the gesture */
    uint32_t gesture_id; /* monotonic camera gesture id, 0 for action events */
    uint32_t owner_role; /* Wc3DirectTouchOwnerRole, 0 means ACTION */
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
