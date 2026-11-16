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

/* The global gesture set shared by both simplified variants: one tap is LMB,
 * no long press, no double tap and no scrolling. */
static int apply_simple_gestures(ProfileData *simple)
{
    ProfileGestureSettings *gestures = &simple->gesture_settings;
    for (int i = 0; i < PROFILE_GESTURE_COUNT; i++)
        profile_bind_package_init(&gestures->gesture_actions[i]);

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

/* -1: no authored ESC; -2: more than one ESC-bearing button. */
static int find_esc_index(const ProfileData *authored)
{
    int index = -1;
    for (int i = 0; i < authored->element_count; i++) {
        const ProfileControlElement *element = &authored->elements[i];
        if (element->type != PROFILE_ELEM_BUTTON || !element_has_esc(element))
            continue;
        if (index >= 0)
            return -2;
        index = i;
    }
    return index;
}

int wc3_profile_build_simple(const ProfileData *authored, ProfileData *simple)
{
    if (!authored || !simple || authored == simple)
        return -1;
    if (authored->element_count <= 0 ||
        authored->element_count > PROFILE_MAX_ELEMENTS)
        return -1;

    /* SIMPLE is the authored profile with every element removed.  Nothing is
     * derived from a button: the overlay has no on-screen controls, while the
     * global gestures below keep the menu usable. */
    *simple = *authored;
    simple->element_count = 0;
    return apply_simple_gestures(simple);
}

int wc3_profile_build_simple_esc(const ProfileData *authored,
                                 ProfileData *simple)
{
    if (!authored || !simple || authored == simple)
        return -1;
    if (authored->element_count <= 0 ||
        authored->element_count > PROFILE_MAX_ELEMENTS)
        return -1;

    int esc_index = find_esc_index(authored);
    if (esc_index == -1)
        return -2;
    if (esc_index == -2)
        return -3;

    /* Cinematic SIMPLE keeps exactly one authored element: the ESC button.
     * Its geometry, visual properties and custom icon are preserved, while all
     * other bindings and gestures are replaced by the deterministic set. */
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

    ProfileBind esc_bind;
    if (profile_bind_from_string("KEY_ESC", &esc_bind) != 0)
        return -1;
    esc->slot_packages[0].bindings[0] = esc_bind;
    esc->slot_packages[0].count = 1;

    return apply_simple_gestures(simple);
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
