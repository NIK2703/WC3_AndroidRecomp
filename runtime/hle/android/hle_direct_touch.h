/*
 * hle_direct_touch.h - guest-side executor for direct-touch events.
 *
 * The controls layer publishes pointer-free events through
 * runtime/ipc/direct_touch_event.h.  This module owns the other side of that
 * queue: it runs only on the main guest thread, at the message-pump boundary.
 *
 * The queue carries:
 *   - semantic TAP/LONG_PRESS: resolved against the live
 *     CGameUI/CWorldFrameWar3; orders go through the native world
 *     click/order handlers and selection through the native selection
 *     builder.  A semantic event tagged PORTRAIT instead resolves against the
 *     portrait's displayed unit: FOLLOW_START makes the native camera follow
 *     it immediately, and HOLD_RELEASE clears that follow.  The current
 *     recognizer does not emit a portrait tap; a stale queued TAP is still
 *     resolved for compatibility.
 *   - raw POINTER_DOWN/MOVE/UP/CANCEL: forwarded to the native Win32 mouse
 *     message path.  This is what makes minimap taps and drags, command-card
 *     buttons, menus and targeting/placement work without the overlay touch
 *     engine.  A minimap-owned drag is clamped to the published minimap
 *     rectangle by the controls layer, and the executor keeps its own
 *     backstop: a DOWN inside the published rectangle stays minimap-owned
 *     until that pointer's UP/CANCEL, every phase is inset into the frame,
 *     and the release is forced even if another path already cleared the
 *     logical button bit.  Leaving the minimap therefore cannot latch the
 *     native camera drag.
 *   - FIELD_CAMERA_BEGIN/MOVE/END: the quick battlefield swipe.  The executor
 *     presses the native middle button at the start point and releases it at
 *     the end, so the game performs its own camera drag.
 *   - FIELD_SELECT_BEGIN/MOVE/END: the held battlefield drag.  After the
 *     long-press deadline the executor presses the native left button and
 *     lets the game draw and commit its own selection rectangle.  A held
 *     finger that never crosses the drag threshold keeps the semantic
 *     LONG_PRESS instead.
 *   - the selected-unit panel grid and the inventory bar are the native-route
 *     gestures the executor claims itself.  Touching or dragging onto a grid
 *     button activates that subgroup or selects the individual unit
 *     immediately (and applies the unit as a target in target mode) instead
 *     of waiting for the native 0x50065 click on release.  A gesture that
 *     starts on an occupied inventory slot is replayed on release as the
 *     native left click (use) or right click + left click (swap/move/drop).
 *
 * The overlay engine remains only for overlay control elements.  It is never
 * used as a fallback for a native HUD or targeting touch in direct mode.
 */
#ifndef WC3_HLE_DIRECT_TOUCH_H
#define WC3_HLE_DIRECT_TOUCH_H

#include <stdint.h>

#include "cpu.h"
#include "direct_touch_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Record the main guest thread and connect the queue wake to the Win32
 * message wait.  Called once from hle_android_start(). */
void wc3_hle_direct_touch_start(void);

/* Producer sink installed into the overlay controls layer.  It is safe on the
 * Android input thread: it only enqueues a fixed-size event. */
void wc3_hle_direct_touch_enqueue(const Wc3DirectTouchEvent *event,
                                  void *user);

/* Consumer.  Must be called at the message-pump boundary on the main guest
 * thread.  width/height are the guest client size; they convert the event's
 * surface-normalised coordinates to client pixels and then through the same
 * 0x6F8B38A0 conversion the native mouse path uses before picking. */
void wc3_hle_direct_touch_service(CPU *c, uint32_t width, uint32_t height);

/* True when a queued event can be consumed by the calling thread right now:
 * it is the main guest thread and no direct dispatch is already in progress.
 * GetMessageA uses this to avoid spinning on the queue from a worker thread or
 * from a nested pump inside a direct guest call. */
int wc3_hle_direct_touch_can_service(void);

/* Release a direct-owned native mouse button, clear a latched camera follow,
 * and drop any queued events when the game leaves the in-game UI.  Safe from
 * any thread: the camera release is deferred to the main guest thread. */
void wc3_hle_direct_touch_reset(void);

/* Lock-free routing snapshot for the Android input thread, one of
 * Wc3DirectTouchRoute.  FIELD means the semantic battlefield recognizer,
 * including its native camera and selection drags; NATIVE means raw pointer
 * phases for the native UI (HUD, menus, targeting/placement); PORTRAIT means the touch is inside the published
 * portrait frame; LEGACY means the UI singleton is not up yet and the
 * controls layer must keep the pointer on the overlay path.  The controls
 * layer derives its MINIMAP owner from the separate minimap rectangle, not
 * from this route.  The guest-thread service updates it at every
 * message-pump boundary.  Safe to call from any thread. */
int wc3_hle_direct_touch_route(void);

/* Lock-free snapshot of the visible portrait frame, in surface-normalised
 * coordinates (0..1, top-left origin).  Returns 1 and fills all four bounds
 * when the portrait is on screen; returns 0 otherwise.  The guest-thread
 * service republishes it at every message-pump boundary, so the Android input
 * thread can claim a portrait touch without calling guest code. */
int wc3_hle_direct_touch_portrait_rect(float *left, float *top,
                                       float *right, float *bottom);

/* Lock-free snapshot of the visible minimap frame, same contract as the
 * portrait rectangle.  The controls layer claims a touch that starts inside
 * this rectangle and clamps its native pointer phases to it until release;
 * the guest-thread executor uses the same snapshot as a backstop so the
 * release stays inside the frame even when the controls route was NATIVE. */
int wc3_hle_direct_touch_minimap_rect(float *left, float *top,
                                      float *right, float *bottom);

/* Lock-free snapshot of the visible beacon button (MinimapSignal) frame,
 * same contract as the portrait rectangle.  The controls layer forces a touch
 * that starts inside it to the raw NATIVE route so the guest beacon gesture
 * claims the press before any native mouse button is posted. */
int wc3_hle_direct_touch_beacon_rect(float *left, float *top,
                                     float *right, float *bottom);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_DIRECT_TOUCH_H */
