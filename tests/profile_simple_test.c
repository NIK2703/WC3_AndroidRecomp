#include "profile_mode.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "profile/bind.h"
#include "profile/types.h"

/* The vendor loader owns this helper, but it also pulls JSON/serialization
 * code that this unit does not exercise.  Keep the unit asset-free by using
 * the same documented initializer the loader uses. */
void profile_bind_package_init(ProfileBindPackage *pkg)
{
    memset(pkg, 0, sizeof(*pkg));
    pkg->auto_repeat_interval_ms = 300;
}

static void set_package(ProfileBindPackage *pkg, const char *name)
{
    profile_bind_package_init(pkg);
    assert(profile_bind_from_string(name, &pkg->bindings[0]) == 0);
    pkg->count = 1;
}

static void make_esc(ProfileControlElement *element)
{
    memset(element, 0, sizeof(*element));
    element->type = PROFILE_ELEM_BUTTON;
    element->shape = PROFILE_SHAPE_CIRCLE;
    element->x = 0.0917f;
    element->y = 0.111f;
    element->w = 0.07f;
    element->h = 0.07f;
    element->scale = 1.15f;
    element->corner_radius = 0.5f;
    element->opacity = 0.8f;
    strcpy(element->display_text, "ESC");
    for (int i = 0; i < PROFILE_MAX_BINDING_SLOTS; i++)
        profile_bind_package_init(&element->slot_packages[i]);
    set_package(&element->slot_packages[0], "KEY_ESC");
    set_package(&element->slot_packages[1], "KEY_1");
    set_package(&element->long_press_package, "KEY_F10");
    set_package(&element->gesture_package, "KEY_F11");
    assert(profile_bind_from_string("KEY_ESC", &element->binds[0]) == 0);
}

int main(void)
{
    static ProfileData authored;
    static ProfileData simple;
    static ProfileData simple_esc;
    memset(&authored, 0, sizeof(authored));

    ProfileControlElement *dpad = &authored.elements[0];
    memset(dpad, 0, sizeof(*dpad));
    dpad->type = PROFILE_ELEM_DPAD;
    for (int i = 0; i < PROFILE_MAX_BINDING_SLOTS; i++)
        profile_bind_package_init(&dpad->slot_packages[i]);
    set_package(&dpad->slot_packages[0], "KEY_UP");

    make_esc(&authored.elements[1]);
    authored.element_count = 2;
    authored.gesture_settings.mouse_mode = PROFILE_MOUSE_MODE_TOUCHSCREEN;

    profile_bind_package_init(
        &authored.gesture_settings.gesture_actions[PROFILE_GESTURE_LONG_PRESS]);
    set_package(
        &authored.gesture_settings.gesture_actions[PROFILE_GESTURE_LONG_PRESS],
        "MOUSE_RIGHT_BUTTON");
    assert(profile_bind_from_string(
               "MOUSE_SCROLL_UP",
               &authored.gesture_settings.scroll_bindings[0][0]) == 0);
    authored.gesture_settings.scroll_hold_v[0] = true;

    assert(wc3_profile_build_simple(&authored, &simple) == 0);
    assert(simple.element_count == 0);
    assert(simple.gesture_settings.gesture_actions[PROFILE_GESTURE_SINGLE_TAP]
               .count == 1);
    assert(strcmp(simple.gesture_settings
                      .gesture_actions[PROFILE_GESTURE_SINGLE_TAP]
                      .bindings[0]
                      .name,
                  "MOUSE_LEFT_BUTTON") == 0);
    for (int i = 0; i < PROFILE_GESTURE_COUNT; i++) {
        if (i == PROFILE_GESTURE_SINGLE_TAP) continue;
        assert(simple.gesture_settings.gesture_actions[i].count == 0);
    }
    assert(simple.gesture_settings.mouse_mode == PROFILE_MOUSE_MODE_TOUCHSCREEN);
    assert(simple.gesture_settings.scroll_bindings[0][0].name[0] == '\0');
    assert(!simple.gesture_settings.scroll_hold_v[0]);
    assert(simple.long_press_delay_ms == 0);

    /* The cinematic variant keeps exactly the authored ESC element and the
     * same simplified global gestures. */
    assert(wc3_profile_build_simple_esc(&authored, &simple_esc) == 0);
    assert(simple_esc.element_count == 1);
    assert(simple_esc.elements[0].type == PROFILE_ELEM_BUTTON);
    assert(simple_esc.elements[0].x == authored.elements[1].x);
    assert(simple_esc.elements[0].y == authored.elements[1].y);
    assert(simple_esc.elements[0].w == authored.elements[1].w);
    assert(simple_esc.elements[0].h == authored.elements[1].h);
    assert(simple_esc.elements[0].scale == authored.elements[1].scale);
    assert(strcmp(simple_esc.elements[0].display_text, "ESC") == 0);
    assert(simple_esc.elements[0].slot_packages[0].count == 1);
    assert(strcmp(simple_esc.elements[0].slot_packages[0].bindings[0].name,
                  "KEY_ESC") == 0);
    assert(simple_esc.elements[0].slot_packages[1].count == 0);
    assert(simple_esc.elements[0].long_press_package.count == 0);
    assert(simple_esc.elements[0].gesture_package.count == 0);
    assert(simple_esc.elements[0].binds[0].name[0] == '\0');
    assert(simple_esc.gesture_settings
               .gesture_actions[PROFILE_GESTURE_SINGLE_TAP]
               .count == 1);
    assert(strcmp(simple_esc.gesture_settings
                      .gesture_actions[PROFILE_GESTURE_SINGLE_TAP]
                      .bindings[0]
                      .name,
                  "MOUSE_LEFT_BUTTON") == 0);
    for (int i = 0; i < PROFILE_GESTURE_COUNT; i++) {
        if (i == PROFILE_GESTURE_SINGLE_TAP) continue;
        assert(simple_esc.gesture_settings.gesture_actions[i].count == 0);
    }
    assert(simple_esc.gesture_settings.scroll_bindings[0][0].name[0] == '\0');
    assert(!simple_esc.gesture_settings.scroll_hold_v[0]);
    assert(simple_esc.gesture_settings.mouse_mode ==
           PROFILE_MOUSE_MODE_TOUCHSCREEN);
    assert(simple_esc.long_press_delay_ms == 0);

    /* The authored profile is never modified. */
    assert(authored.element_count == 2);
    assert(strcmp(authored.elements[1].slot_packages[0].bindings[0].name,
                  "KEY_ESC") == 0);
    assert(authored.elements[1].long_press_package.count == 1);

    /* The authored ESC is irrelevant: SIMPLE drops every element. */
    authored.elements[1].slot_packages[0].count = 0;
    authored.elements[1].binds[0].name[0] = '\0';
    assert(wc3_profile_build_simple(&authored, &simple) == 0);
    assert(simple.element_count == 0);
    assert(wc3_profile_build_simple_esc(&authored, &simple_esc) == -2);

    assert(wc3_profile_build_simple(NULL, &simple) == -1);
    assert(wc3_profile_build_simple(&authored, NULL) == -1);
    assert(wc3_profile_build_simple(&authored, &authored) == -1);
    assert(wc3_profile_build_simple_esc(NULL, &simple) == -1);
    assert(wc3_profile_build_simple_esc(&authored, NULL) == -1);
    assert(wc3_profile_build_simple_esc(&authored, &authored) == -1);

    printf("profile_simple: okay\n");
    return 0;
}
