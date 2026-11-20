/*
 * hle_direct_touch.h - guest-side executor for semantic direct-touch events.
 *
 * The overlay recognizer publishes pointer-free events through
 * runtime/ipc/direct_touch_event.h.  This module owns the other side of that
 * queue: it runs only on the main guest thread, at the message-pump boundary,
 * and resolves a tap or long press against the live CGameUI/CWorldFrameWar3.
 *
 * No synthetic WM_* message is created.  Orders go through the native world
 * click/order handlers, and selection goes through the native selection
 * builder, so the result is the same game action a real click would produce.
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

/* Drop any queued events when the game leaves the in-game UI. */
void wc3_hle_direct_touch_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_DIRECT_TOUCH_H */
