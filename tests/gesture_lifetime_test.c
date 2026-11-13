/*
 * T0: a pointer that resolves a lifted main finger's double-tap window
 * outside the tap radius must start a new main touch, not a second finger.
 *
 * The regression is reproducible with the real Warcraft profile: hold an
 * overlay button with pointer 0, tap the field with pointer 1, release the
 * button, then tap the field with pointer 0 before pointer 1's double-tap
 * timeout.  Before the fix the cancelled window left the old main active in
 * IDLE, so the new tap emitted no pointer move and every later single tap was
 * treated as a second finger until a two-finger touch reused the stale id.  The
 * near-confirm path has the same lifetime requirement once the confirming
 * pointer goes up.
 */

#include "profile/loader.h"
#include "profile/convert.h"
#include "core/processor.h"
#include "core/state.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_INTERVAL_MS 16
#define PHASE_DOWN 0
#define PHASE_UP 1

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

static void set_tick(uint64_t tick) {
    g_fake_ms = tick * TEST_INTERVAL_MS;
}

static TouchActionResult touch(int phase, int ptr_id, float x, float y,
                               uint64_t tick) {
    set_tick(tick);
    TouchActionResult result;
    memset(&result, 0, sizeof(result));
    if (phase == PHASE_DOWN)
        touch_processor_on_finger_down(&result, ptr_id, x, y, tick);
    else
        touch_processor_on_finger_up(&result, ptr_id, x, y, tick);
    return result;
}

static void tick(uint64_t t) {
    set_tick(t);
    TouchActionResult result;
    memset(&result, 0, sizeof(result));
    touch_processor_tick(&result, t);
}

static int has_move_to(const TouchActionResult *result, int x, int y) {
    for (int i = 0; i < (int)result->count; i++) {
        const TouchAction *action = &result->actions[i];
        if (action->type == ACT_POINTER_MOVE
            && action->pointer_move.x == x
            && action->pointer_move.y == y)
            return 1;
    }
    return 0;
}

static int active_ptr(int ptr_id) {
    if (ptr_id < 0 || ptr_id >= MAX_POINTER_ID) return 0;
    return g_state.finger_by_ptr_id[ptr_id] != NULL;
}

static int find_button_anchor(const TouchElement *elements, int count,
                              float *x, float *y) {
    for (int i = 0; i < count; i++) {
        if (elements[i].type == BUTTON && elements[i].w > 0
            && elements[i].h > 0) {
            *x = (float)elements[i].x;
            *y = (float)elements[i].y;
            return 1;
        }
    }
    return 0;
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
    static TouchProcessorConfig config;
    static TouchElement elements[MAX_ELEMENTS];
    memset(&profile, 0, sizeof(profile));
    if (profile_load_from_string(json, &profile) != 0) {
        fprintf(stderr, "profile load failed: %s\n", profile_last_error());
        free(json);
        return 2;
    }
    free(json);

    g_timer_interval_ms = profile.timer_interval_ms > 0
        ? profile.timer_interval_ms : TEST_INTERVAL_MS;
    profile_to_config(&profile, 2289, 1080, 1.0f, &config);
    touch_processor_init(&config);
    int element_count = profile_to_elements(&profile, 2289, 1080, elements,
                                            MAX_ELEMENTS);
    touch_processor_set_elements(elements, element_count);
    touch_processor_set_resolution_scale(1.0f);
    touch_processor_set_xform_scale(1.0f, 1.0f);
    touch_processor_set_sim_touch_screen(false);

    float button_x = 0.0f;
    float button_y = 0.0f;
    if (!find_button_anchor(elements, element_count, &button_x, &button_y))
        fail("profile has no button element");

    /* Hold a button with pointer 0 while pointer 1 taps the field. */
    touch(PHASE_DOWN, 0, button_x, button_y, 100);
    if (g_state.fingers[0].engaged_elem_count == 0)
        fail("button anchor did not engage an element");
    touch(PHASE_DOWN, 1, 20.0f, 1060.0f, 101);
    touch(PHASE_UP, 1, 20.0f, 1060.0f, 102);
    touch(PHASE_UP, 0, button_x, button_y, 103);

    /* The far tap is inside pointer 1's double-tap window.  It must retire
     * pointer 1, move the cursor, and become the main finger. */
    TouchActionResult down = touch(PHASE_DOWN, 0, 400.0f, 800.0f, 104);
    if (!has_move_to(&down, 400, 800))
        fail("far tap did not move the cursor");
    if (g_state.gesture_main_ptr_id != 0)
        fail("far tap did not become the main finger");
    if (active_ptr(1))
        fail("lifted main pointer 1 stayed active");
    if (g_state.active_finger_count != 1)
        fail("unexpected active finger count after far tap");

    touch(PHASE_UP, 0, 400.0f, 800.0f, 105);
    for (uint64_t t = 106; t <= 120; t++) tick(t);

    /* Later single taps must remain first-finger gestures. */
    TouchActionResult next = touch(PHASE_DOWN, 0, 500.0f, 700.0f, 121);
    if (!has_move_to(&next, 500, 700))
        fail("next single tap did not move the cursor");
    if (g_state.gesture_main_ptr_id != 0)
        fail("next single tap was not the main finger");
    touch(PHASE_UP, 0, 500.0f, 700.0f, 122);
    for (uint64_t t = 123; t <= 140; t++) tick(t);
    if (g_state.active_finger_count != 0)
        fail("fingers did not settle");

    /* Near-confirm path: a new pointer inside the tap radius confirms the
     * double tap.  Once that pointer is up, its lifted owner must not remain
     * active without an UP event. */
    touch_processor_init(&config);
    touch_processor_set_elements(elements, element_count);
    touch_processor_set_resolution_scale(1.0f);
    touch_processor_set_xform_scale(1.0f, 1.0f);
    touch_processor_set_sim_touch_screen(false);

    touch(PHASE_DOWN, 0, 100.0f, 100.0f, 200);
    touch(PHASE_UP, 0, 100.0f, 100.0f, 201);
    touch(PHASE_DOWN, 1, 120.0f, 110.0f, 202);
    touch(PHASE_UP, 1, 120.0f, 110.0f, 203);
    if (active_ptr(0))
        fail("confirmed double-tap owner stayed active");
    if (g_state.gesture_main_ptr_id != INVALID_PTR_ID)
        fail("confirmed double-tap owner stayed main");
    if (g_state.active_finger_count != 0)
        fail("confirmed double-tap fingers did not settle");

    TouchActionResult after_confirm = touch(PHASE_DOWN, 0, 300.0f, 300.0f,
                                            220);
    if (!has_move_to(&after_confirm, 300, 300))
        fail("single tap after near confirmation did not move the cursor");
    touch(PHASE_UP, 0, 300.0f, 300.0f, 221);
    for (uint64_t t = 222; t <= 240; t++) tick(t);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("gesture_lifetime: PASS\n");
    return 0;
}
