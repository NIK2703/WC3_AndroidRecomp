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
void controls_set_action_sink(Wc3ActionSink sink, void *user);

int controls_touch(Wc3TouchPhase phase, int pointer_id, float x, float y,
                       uint64_t event_time_ms);
int controls_tick(uint64_t now_ms);
uint64_t controls_next_deadline_ms(uint64_t now_ms);
void controls_cancel_all(void);

void controls_resize(int width, int height);

/*
 * Fill overlay metadata from the engine.  Call after init and after every
 * resize.  Returns 0 on success, -1 if the profile contains an unsupported
 * element type or more elements than the overlay can hold.
 */
int controls_populate_overlay(Wc3Overlay *overlay);

/* Copy only visual state; element count, geometry and text are unchanged. */
void controls_sync_visuals(Wc3Overlay *overlay);

const char *controls_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_CONTROLS_H */
