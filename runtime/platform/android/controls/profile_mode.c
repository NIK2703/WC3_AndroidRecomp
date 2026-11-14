#include "profile_mode.h"

#include <string.h>

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

static bool element_has_esc(const ProfileControlElement *element)
{
    if (!element)
        return false;

    for (int slot = 0; slot < PROFILE_MAX_BINDING_SLOTS; slot++) {
        const ProfileBindPackage *pkg = &element->slot_packages[slot];
        for (int i = 0; i < pkg->count; i++) {
            if (strcmp(pkg->bindings[i].name, "KEY_ESC") == 0)
                return true;
        }
    }
    for (int i = 0; i < PROFILE_MAX_BINDING_SLOTS; i++) {
        if (strcmp(element->binds[i].name, "KEY_ESC") == 0)
            return true;
    }
    return false;
}

static void clear_bind_package(ProfileBindPackage *package)
{
    profile_bind_package_init(package);
}

int wc3_profile_build_simple(const ProfileData *authored, ProfileData *simple)
{
    if (!authored || !simple || authored == simple)
        return -1;
    if (authored->element_count <= 0 ||
        authored->element_count > PROFILE_MAX_ELEMENTS)
        return -1;

    int esc_index = -1;
    for (int i = 0; i < authored->element_count; i++) {
        const ProfileControlElement *element = &authored->elements[i];
        if (element->type != PROFILE_ELEM_BUTTON || !element_has_esc(element))
            continue;
        if (esc_index >= 0)
            return -3; /* ambiguous authored ESC; keep FULL */
        esc_index = i;
    }
    if (esc_index < 0)
        return -2; /* no authored ESC; keep FULL */

    *simple = *authored;
    simple->element_count = 1;
    simple->elements[0] = authored->elements[esc_index];

    ProfileControlElement *esc = &simple->elements[0];
    for (int slot = 0; slot < PROFILE_MAX_BINDING_SLOTS; slot++)
        clear_bind_package(&esc->slot_packages[slot]);
    memset(esc->binds, 0, sizeof(esc->binds));
    clear_bind_package(&esc->long_press_package);
    clear_bind_package(&esc->gesture_package);
    esc->binding_count = 0;
    esc->toggle_switch = false;
    esc->has_gamepad_binding = false;

    /* The authored button keeps its exact ESC action.  The single tap is a
     * global gesture for taps outside the element; it must not replace the
     * element binding. */
    ProfileBind esc_bind;
    if (profile_bind_from_string("KEY_ESC", &esc_bind) != 0)
        return -1;
    esc->slot_packages[0].bindings[0] = esc_bind;
    esc->slot_packages[0].count = 1;

    /* SIMPLE has no edge gestures and no scrolling.  The single ESC element is
     * the only authored geometry; its visual properties are preserved above. */
    ProfileGestureSettings *gestures = &simple->gesture_settings;
    for (int i = 0; i < PROFILE_GESTURE_COUNT; i++)
        clear_bind_package(&gestures->gesture_actions[i]);
    ProfileBind left;
    if (profile_bind_from_string("MOUSE_LEFT_BUTTON", &left) != 0)
        return -1;
    gestures->gesture_actions[PROFILE_GESTURE_SINGLE_TAP].bindings[0] = left;
    gestures->gesture_actions[PROFILE_GESTURE_SINGLE_TAP].count = 1;
    memset(gestures->scroll_bindings, 0, sizeof(gestures->scroll_bindings));
    memset(gestures->scroll_hold_v, 0, sizeof(gestures->scroll_hold_v));
    memset(gestures->scroll_hold_h, 0, sizeof(gestures->scroll_hold_h));
    gestures->scroll_threshold_px = 0;
    simple->long_press_delay_ms = 0;

    return 0;
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
