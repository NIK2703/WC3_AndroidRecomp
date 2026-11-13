#ifndef WC3_CONTROLS_PROFILE_MODE_H
#define WC3_CONTROLS_PROFILE_MODE_H

#include <stdbool.h>

#include "profile/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Apply the product gesture mode after the .icp profile has been parsed.
 *
 * double_tap_enabled = true keeps the profile exactly as authored.
 *
 * double_tap_enabled = false makes the long press the right mouse button and
 * removes every double-tap package.  The packages are reset to count 0 rather
 * than filled with a NONE binding: the engine caches "has active double tap"
 * from count > 0, so a NONE package would still defer the single tap by the
 * double-tap timeout.  count 0 is what makes a single tap fire on finger-up.
 */
void wc3_profile_apply_gesture_mode(ProfileData *profile,
                                    bool double_tap_enabled);

/*
 * Apply product gesture timers after the .icp profile has been parsed.
 * A value <= 0 keeps the authored timer.  Positive values are clamped to
 * 100..200 ms, the range exposed by the launch settings.
 */
void wc3_profile_apply_gesture_timings(ProfileData *profile,
                                       int double_tap_timeout_ms,
                                       int long_press_timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* WC3_CONTROLS_PROFILE_MODE_H */
