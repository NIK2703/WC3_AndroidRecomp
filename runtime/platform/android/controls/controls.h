#ifndef WC3_CONTROLS_H
#define WC3_CONTROLS_H

/*
 * Detached Winlator touch engine adapter.
 *
 * This is the only runtime entry point for the detached control overlay.  It
 * owns the vendored C touch state machine and exposes plain touch phases plus
 * an action sink.  It does not know about the recompiler, HLE, GameActivity or
 * JNI.
 */

#include <stdbool.h>
#include <stdint.h>

#include "overlay.h"
#include "core/types/touch_action.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WC3_TOUCH_DOWN = 0,
    WC3_TOUCH_MOVE,
    WC3_TOUCH_UP,
    WC3_TOUCH_CANCEL
} Wc3TouchPhase;

typedef void (*Wc3ActionSink)(const TouchAction *actions, int count,
                              void *user);

/* Profile JSON must stay valid only for the duration of this call. */
int controls_init(const char *profile_json, int width, int height);

/*
 * Same as controls_init(), but applies the product gesture mode and timers
 * before the profile is converted to the engine config.  A timer <= 0 keeps
 * the authored .icp value.  The legacy controls_init() keeps the profile
 * unchanged, which is also what a direct diagnostic launch without the
 * settings bundle receives.
 */
int controls_init_ex(const char *profile_json, int width, int height,
                     bool double_tap_enabled, int double_tap_timeout_ms,
                     int long_press_timeout_ms);

void controls_set_action_sink(Wc3ActionSink sink, void *user);

int controls_touch(Wc3TouchPhase phase, int pointer_id, float x, float y,
                       uint64_t event_time_ms);
int controls_tick(uint64_t now_ms);

/*
 * 1 while the engine has a pending timer, auto-repeat, mouse movement, a
 * held action, or one-shot cleanup to perform.  An active finger alone is not
 * a tick reason.  When 0, the host may block until the next touch instead of
 * polling controls_tick() at the tick rate.
 */
int controls_needs_tick(void);

/*
 * Exact absolute CLOCK_MONOTONIC millisecond deadline for the next tick, or 0
 * when no timer is pending.  The host must treat 0 as "no deadline", not as
 * "due now".  A returned value <= now_ms means run one tick immediately and
 * query again.
 */
uint64_t controls_next_deadline_ms(uint64_t now_ms);
void controls_cancel_all(void);

void controls_resize(int width, int height);

/*
 * Fill overlay metadata from the engine.  Call after init and after every
 * resize.  Returns 0 on success, -1 if the profile contains an unsupported
 * element type or more elements than the overlay can hold.
 */
int controls_populate_overlay(Wc3Overlay *overlay);

/*
 * Copy only visual state; element count, geometry and text are unchanged.
 * Returns 0 on success and -1 when the overlay cannot be synced.  Change
 * detection belongs to overlay_visual_state_equal(); this function must not
 * become a second publish policy.  A successful sync consumes the engine's
 * visual-dirty marks.
 */
int controls_sync_visuals(Wc3Overlay *overlay);

/*
 * 1 when the touch engine marked element visuals dirty since the last
 * successful controls_sync_visuals(); 0 when the overlay cannot have changed.
 * This is a cheap gate for the host input loop, not a second publish policy.
 */
int controls_visual_dirty(void);

const char *controls_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_CONTROLS_H */
