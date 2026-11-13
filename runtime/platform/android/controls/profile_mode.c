#include "profile_mode.h"

#include "profile/bind.h"
#include "profile/types.h"

static void set_single_binding(ProfileBindPackage *package, const char *name)
{
    profile_bind_package_init(package);
    if (!name || !name[0])
        return;

    ProfileBind bind;
    if (profile_bind_from_string(name, &bind) == 0) {
        package->bindings[0] = bind;
        package->count = 1;
    }
}

static int clamp_timeout_ms(int value)
{
    if (value < 100)
        return 100;
    if (value > 200)
        return 200;
    return value;
}

/* These are the touch-gesture timers.  profile->long_press_delay_ms keeps
 * controlling the on-screen element long-press bindings separately. */
void wc3_profile_apply_gesture_timings(ProfileData *profile,
                                       int double_tap_timeout_ms,
                                       int long_press_timeout_ms)
{
    if (!profile)
        return;

    if (double_tap_timeout_ms > 0) {
        profile->gesture_settings.double_tap_timeout_ms =
            clamp_timeout_ms(double_tap_timeout_ms);
    }
    if (long_press_timeout_ms > 0) {
        profile->gesture_settings.long_press_timeout_ms =
            clamp_timeout_ms(long_press_timeout_ms);
    }
}

void wc3_profile_apply_gesture_mode(ProfileData *profile,
                                    bool double_tap_enabled)
{
    if (!profile || double_tap_enabled)
        return;

    ProfileGestureSettings *gestures = &profile->gesture_settings;

    /* The direct RTS scheme: a quick tap is the left click and a held press
     * is the right click.  Drag actions keep the profile mappings. */
    set_single_binding(&gestures->gesture_actions[PROFILE_GESTURE_LONG_PRESS],
                       "MOUSE_RIGHT_BUTTON");

    /* count 0, not NONE: see the header comment. */
    profile_bind_package_init(
        &gestures->gesture_actions[PROFILE_GESTURE_DOUBLE_TAP]);
    profile_bind_package_init(
        &gestures->gesture_actions[PROFILE_GESTURE_DOUBLE_TAP_DRAG]);
    profile_bind_package_init(
        &gestures->gesture_actions[PROFILE_GESTURE_DOUBLE_2ND]);
    profile_bind_package_init(
        &gestures->gesture_actions[PROFILE_GESTURE_DOUBLE_DRAG_2ND]);
}
