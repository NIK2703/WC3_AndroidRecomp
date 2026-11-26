/*
 * native_input_gateway.h - the only mouse surface exposed to direct touch.
 *
 * There is no synchronous press entry point.  Every press is armed here and
 * published by the guest's idle GetCursorPos pass after its move was
 * dispatched.  A normal press has no LEFT UP cleanup; only the camera
 * triplet carries one, to reset the stale drag state before MIDDLE DOWN.
 * Raw button posting lives in hle_android_mouse_private.h and is reserved for
 * the legacy guest-import boundary.
 */
#ifndef WC3_NATIVE_INPUT_GATEWAY_H
#define WC3_NATIVE_INPUT_GATEWAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Move the virtual cursor.  Coordinates are surface-normalised. */
void hle_android_mouse_move(float nx, float ny);

/* Post one native press (optionally a whole click pair).  The HLE owns the
 * idle publication order; no extra release is posted at the new position. */
void hle_android_mouse_press(float nx, float ny, int button, int release);
int hle_android_mouse_cancel_pending_press(void);

/* Guest-thread replay of an already validated click (building/target
 * confirmation).  Unlike a touch press it must reach the guest in this pump:
 * the placement loop may never call GetCursorPos, so the idle gateway slot
 * cannot publish it.  Touch-originated presses still use the deferred API. */
void hle_android_mouse_press_replay(float nx, float ny, int button);

/* Camera sequence: LEFT UP, MIDDLE UP, MIDDLE DOWN behind the same idle pass. */
void hle_android_mouse_move_defer_camera_begin(float nx, float ny);
int hle_android_mouse_cancel_pending_camera(void);

/* Fallbacks for a release that arrives before the guest polls the cursor, and
 * for a context change that must drop the armed sequence. */
void hle_android_mouse_flush_pending_down(void);
int hle_android_mouse_cancel_pending_down(void);
/* Publish an armed+ready sequence from the message-pump boundary.  Needed
 * when the guest enters a modal loop that never calls GetCursorPos.  Returns 1
 * when a sequence was published, so the caller can defer the rest of the
 * batch: a native button needs one pump between DOWN and UP to activate. */
int hle_android_mouse_flush_pending_ready(void);

/* Read-only state queries and the release-only path. */
int hle_android_mouse_is_down(int button);
int hle_android_native_button_pending(void);
/* True while a press sequence is still armed and has not been published.
 * A caller that owns a matching release uses this to distinguish "waiting for
 * the idle pass" from "already in the queue"; a queued press is reported by
 * hle_android_native_button_pending()/hle_android_mouse_is_down() instead. */
int hle_android_mouse_press_pending(void);
void hle_android_mouse_button_up_force(int button);

#ifdef __cplusplus
}
#endif

#endif /* WC3_NATIVE_INPUT_GATEWAY_H */
