#ifndef WC3_OVERLAY_ACTIONS_LEGACY_H
#define WC3_OVERLAY_ACTIONS_LEGACY_H

/*
 * Transitional bridge from the Winlator touch engine's TouchAction stream to
 * the target-side HLE bridge.
 *
 * The engine emits X11 keycodes (the values in profile/bind.c).  This adapter
 * is the only place that translates them into Windows virtual keys, and it
 * reaches the as-built HLE only through runtime/hle/hle_bridge.h.  It will be
 * replaced by a Wc3Platform InputApi consumer when user32 owns the message
 * queue; until then the bridge keeps the UI layer free of as-built headers.
 */

#include <stdbool.h>

#include "core/types/touch_action.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Receives the engine's ACT_HAPTIC effect values.  Effect 0 is off; 1..5 are
 * the Winlator tick/click/heavy/double/pulse values.  The Android host installs
 * a direct-JNI sink; the Windows action test installs a recorder. */
typedef void (*Wc3OverlayHapticSink)(int effect, void *user);

void overlay_actions_init(int screen_width, int screen_height);

/* Set before the input thread starts and clear after it is joined. */
void overlay_actions_set_haptic_sink(Wc3OverlayHapticSink sink, void *user);

/* Matches Wc3ActionSink from wc3_controls.h. */
void overlay_actions_handle(const TouchAction *actions, int count, void *user);

void overlay_actions_release_all(void);

/* Returns 0 for a non-keyboard keycode.  Modifier keys return the specific
 * left/right VK; hle_key_message_vk() maps those to the generic WM_KEYDOWN
 * wParam. */
int overlay_actions_keycode_to_vk(int keycode);

#ifdef __cplusplus
}
#endif

#endif /* WC3_OVERLAY_ACTIONS_LEGACY_H */
