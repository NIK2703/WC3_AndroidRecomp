/*
 * T0: the direct RTS gesture mode applied after profile parsing must remove
 * every double-tap package with count 0 and must not enter a double-tap wait
 * on a single tap.  This is the regression guard for the profile-mode helper:
 * a NONE package has count 1, so the engine would still wait for a second tap
 * and the promised zero delay would silently disappear.
 *
 * The first half checks the effective profile in both modes.  The second half
 * drives the real touch engine: a tap in double-tap mode enters the wait, a
 * tap in direct mode executes the single tap immediately.  The timer override
 * clamp is checked against the authored profile at the end.
 */

#include "profile/loader.h"
#include "profile/convert.h"
#include "profile_mode.h"
#include "core/processor.h"
#include "core/state.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_INTERVAL_MS 16
#define SCREEN_W 2289
#define SCREEN_H 1080

extern uint64_t g_fake_ms;
int g_timer_interval_ms = TEST_INTERVAL_MS;
pthread_mutex_t g_state_mutex __attribute__((aligned(64)))
    = PTHREAD_MUTEX_INITIALIZER;

void udp_mouse_send(int flags, int dx, int dy, int wheel) {
    (void)flags;
    (void)dx;
    (void)dy;
    (void)wheel;
}

static int failures;

static void fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
}

static void expect_int(const char *name, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got %d want %d\n", name, got, want);
        failures++;
    }
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    char *buffer = malloc((size_t)size + 1u);
    if (!buffer) {
        fclose(file);
        return NULL;
    }
    if (fread(buffer, 1, (size_t)size, file) != (size_t)size) {
        free(buffer);
        fclose(file);
        return NULL;
    }
    buffer[size] = '\0';
    fclose(file);
    return buffer;
}

static const ProfileBindPackage *gesture(const ProfileData *profile,
                                         ProfileGestureType type) {
    return &profile->gesture_settings.gesture_actions[type];
}

static void expect_package(const char *name, const ProfileBindPackage *package,
                           int want_count, const char *want_binding) {
    expect_int(name, package->count, want_count);
    if (want_count > 0 && want_binding && package->count > 0) {
        if (strcmp(package->bindings[0].name, want_binding) != 0) {
            fprintf(stderr, "FAIL: %s: got %s want %s\n", name,
                    package->bindings[0].name, want_binding);
            failures++;
        }
    }
}

/* An explicit empty array becomes a NONE package.  The key must stay present:
 * deleting it makes the loader re-apply the second-finger defaults. */
static int package_is_none(const ProfileBindPackage *package) {
    return package->count == 1 &&
           strcmp(package->bindings[0].name, "NONE") == 0;
}

static void init_engine(const ProfileData *profile, bool direct_mode,
                        TouchProcessorConfig *config, TouchElement *elements) {
    profile_to_config(profile, SCREEN_W, SCREEN_H, 1.0f, config);
    config->direct_gesture_mode = direct_mode;
    touch_processor_init(config);

    int count = profile_to_elements(profile, SCREEN_W, SCREEN_H, elements,
                                    MAX_ELEMENTS);
    if (count <= 0) {
        fail("profile has no elements");
        return;
    }
    touch_processor_set_elements(elements, count);
    touch_processor_set_resolution_scale(1.0f);
    touch_processor_set_xform_scale(1.0f, 1.0f);
    touch_processor_set_sim_touch_screen(false);
}

static int tap_dt_waiting(const ProfileData *profile, bool direct_mode) {
    TouchProcessorConfig config;
    static TouchElement elements[MAX_ELEMENTS];
    memset(&config, 0, sizeof(config));

    init_engine(profile, direct_mode, &config, elements);
    g_fake_ms = 0;

    TouchActionResult down;
    TouchActionResult up;
    memset(&down, 0, sizeof(down));
    memset(&up, 0, sizeof(up));
    touch_processor_on_finger_down(&down, 0, 20.0f, 1060.0f, 100);
    touch_processor_on_finger_up(&up, 0, 20.0f, 1060.0f, 101);

    return g_state.dt_waiting_count;
}

static int has_action(const TouchActionResult *result, ActionType type) {
    for (int i = 0; i < result->count; i++) {
        if (result->actions[i].type == type) return 1;
    }
    return 0;
}

/* A one-finger drag is the touchscreen pointer.  It must not enter scroll
 * mode: scroll mode freezes the cursor at the touch-down point. */
static int one_finger_drag_keeps_cursor(const ProfileData *profile) {
    TouchProcessorConfig config;
    static TouchElement elements[MAX_ELEMENTS];
    memset(&config, 0, sizeof(config));

    init_engine(profile, true, &config, elements);
    g_fake_ms = 0;

    TouchActionResult down;
    TouchActionResult move;
    memset(&down, 0, sizeof(down));
    memset(&move, 0, sizeof(move));
    touch_processor_on_finger_down(&down, 0, 900.0f, 500.0f, 100);
    touch_processor_on_finger_move(&move, 0, 900.0f, 380.0f, 101);

    const TouchFinger *finger = g_state.finger_by_ptr_id[0];
    return finger && !finger->scroll_mode && has_action(&move, ACT_POINTER_MOVE);
}

/* Camera scrolling is a two-finger gesture (Sd2 slot).  It must still enter
 * scroll mode after the single-finger scroll slot is cleared. */
static int two_finger_drag_enters_scroll(const ProfileData *profile) {
    TouchProcessorConfig config;
    static TouchElement elements[MAX_ELEMENTS];
    memset(&config, 0, sizeof(config));

    init_engine(profile, true, &config, elements);
    g_fake_ms = 0;

    TouchActionResult down_first;
    TouchActionResult down_second;
    TouchActionResult move;
    memset(&down_first, 0, sizeof(down_first));
    memset(&down_second, 0, sizeof(down_second));
    memset(&move, 0, sizeof(move));
    touch_processor_on_finger_down(&down_first, 0, 900.0f, 500.0f, 100);
    touch_processor_on_finger_down(&down_second, 1, 1000.0f, 500.0f, 101);
    touch_processor_on_finger_move(&move, 1, 1000.0f, 380.0f, 102);

    const TouchFinger *finger = g_state.finger_by_ptr_id[1];
    return finger && finger->scroll_mode ? 1 : 0;
}

static int scroll_slot_has_binding(const ProfileData *profile,
                                   ProfileScrollSlot slot,
                                   ProfileScrollDirection dir,
                                   const char *name) {
    const ProfileBind *bind =
        &profile->gesture_settings.scroll_bindings[slot][dir];
    return bind->name[0] && strcmp(bind->name, name) == 0;
}

static int scroll_slot_empty(const ProfileData *profile, ProfileScrollSlot slot) {
    for (int d = 0; d < PROFILE_SCROLL_DIR_COUNT; d++) {
        if (profile->gesture_settings.scroll_bindings[slot][d].name[0])
            return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s profile.icp\n", argv[0]);
        return 2;
    }

    char *json = read_file(argv[1]);
    if (!json) {
        fprintf(stderr, "cannot read profile: %s\n", argv[1]);
        return 2;
    }

    static ProfileData profile;
    memset(&profile, 0, sizeof(profile));
    if (profile_load_from_string(json, &profile) != 0) {
        fprintf(stderr, "profile load failed: %s\n", profile_last_error());
        free(json);
        return 2;
    }
    free(json);

    g_timer_interval_ms = profile.timer_interval_ms > 0
        ? profile.timer_interval_ms : TEST_INTERVAL_MS;

    /* Camera scroll is a two-finger gesture.  A scroll binding on Sd turns
     * every one-finger touchscreen drag into scroll mode and freezes the
     * cursor at the down point. */
    if (!scroll_slot_empty(&profile, PROFILE_SCROLL_SLOT_SD))
        fail("single-finger scroll slot is not empty");
    if (!scroll_slot_has_binding(&profile, PROFILE_SCROLL_SLOT_SD2,
                                 PROFILE_SCROLL_DIR_UP, "KEY_PG_DOWN"))
        fail("two-finger camera up is not KEY_PG_DOWN");
    if (!scroll_slot_has_binding(&profile, PROFILE_SCROLL_SLOT_SD2,
                                 PROFILE_SCROLL_DIR_DOWN, "KEY_PG_UP"))
        fail("two-finger camera down is not KEY_PG_UP");
    if (!scroll_slot_has_binding(&profile, PROFILE_SCROLL_SLOT_SD2,
                                 PROFILE_SCROLL_DIR_LEFT, "KEY_DEL"))
        fail("two-finger camera left is not KEY_DEL");
    if (!scroll_slot_has_binding(&profile, PROFILE_SCROLL_SLOT_SD2,
                                 PROFILE_SCROLL_DIR_RIGHT, "KEY_INSERT"))
        fail("two-finger camera right is not KEY_INSERT");

    /* The second finger is reserved for the two-finger camera swipe.  Its
     * tap and drag packages must be NONE so a second-finger tap cannot click. */
    if (!package_is_none(gesture(&profile, PROFILE_GESTURE_SINGLE_2ND)))
        fail("second-finger tap action is not disabled");
    if (!package_is_none(gesture(&profile, PROFILE_GESTURE_SINGLE_DRAG_2ND)))
        fail("second-finger drag action is not disabled");
    if (!package_is_none(gesture(&profile, PROFILE_GESTURE_DOUBLE_2ND)))
        fail("second-finger double tap action is not disabled");
    if (!package_is_none(gesture(&profile, PROFILE_GESTURE_DOUBLE_DRAG_2ND)))
        fail("second-finger double tap drag action is not disabled");

    /* Authored profile: the double-tap scheme used by the product default. */
    if (gesture(&profile, PROFILE_GESTURE_DOUBLE_TAP)->count <= 0)
        fail("authored profile has no double-tap action");
    if (gesture(&profile, PROFILE_GESTURE_DOUBLE_TAP_DRAG)->count <= 0)
        fail("authored profile has no double-tap drag action");
    if (tap_dt_waiting(&profile, false) != 1)
        fail("double-tap mode did not enter the double-tap wait");

    /* Direct mode: long press is right click, all double-tap slots are empty. */
    wc3_profile_apply_gesture_mode(&profile, false);

    expect_package("long press", gesture(&profile, PROFILE_GESTURE_LONG_PRESS),
                   1, "MOUSE_RIGHT_BUTTON");
    expect_package("double tap", gesture(&profile, PROFILE_GESTURE_DOUBLE_TAP),
                   0, NULL);
    expect_package("double tap drag",
                   gesture(&profile, PROFILE_GESTURE_DOUBLE_TAP_DRAG), 0, NULL);
    expect_package("second double tap",
                   gesture(&profile, PROFILE_GESTURE_DOUBLE_2ND), 0, NULL);
    expect_package("second double tap drag",
                   gesture(&profile, PROFILE_GESTURE_DOUBLE_DRAG_2ND), 0, NULL);

    TouchProcessorConfig config;
    memset(&config, 0, sizeof(config));
    profile_to_config(&profile, SCREEN_W, SCREEN_H, 1.0f, &config);
    if (config.caps_has_double_tap)
        fail("direct mode still advertises double-tap capability");
    if (!config.caps_has_scroll_bindings)
        fail("direct mode lost the camera scroll bindings");

    if (tap_dt_waiting(&profile, true) != 0)
        fail("direct mode still entered the double-tap wait");
    if (one_finger_drag_keeps_cursor(&profile) != 1)
        fail("one-finger drag froze the touchscreen cursor");
    if (two_finger_drag_enters_scroll(&profile) != 1)
        fail("two-finger drag did not enter camera scroll mode");

    /* The same product helper carries the two timer overrides.  Zero keeps
     * the authored .icp value; positive values clamp to the UI range. */
    const int authored_dt = profile.gesture_settings.double_tap_timeout_ms;
    const int authored_lp = profile.gesture_settings.long_press_timeout_ms;
    wc3_profile_apply_gesture_timings(&profile, 0, 0);
    expect_int("zero keeps double tap timeout",
               profile.gesture_settings.double_tap_timeout_ms, authored_dt);
    expect_int("zero keeps long press timeout",
               profile.gesture_settings.long_press_timeout_ms, authored_lp);

    wc3_profile_apply_gesture_timings(&profile, 90, 240);
    expect_int("double tap timeout low clamp",
               profile.gesture_settings.double_tap_timeout_ms, 100);
    expect_int("long press timeout high clamp",
               profile.gesture_settings.long_press_timeout_ms, 200);

    wc3_profile_apply_gesture_timings(&profile, 160, 180);
    expect_int("double tap timeout override",
               profile.gesture_settings.double_tap_timeout_ms, 160);
    expect_int("long press timeout override",
               profile.gesture_settings.long_press_timeout_ms, 180);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("gesture_mode: PASS\n");
    return 0;
}
