/*
 * T0: a swipe binding on a HOVER button must use the whole pointer path, not
 * only the part that stays inside the button.
 *
 * Regression: the hover state machine stopped forwarding MOVE as soon as the
 * pointer left the button and released the element.  A swipe that started near
 * the edge and crossed the gesture threshold outside the rect produced no
 * gesture action and could leak a tap on UP.  The engine now keeps the gesture
 * button engaged until UP, forwards outside MOVE samples to its element, and
 * cancels the deferred tap when the pointer lifts outside before the threshold.
 *
 * The same test also leaves a plain HOVER button (no element gesture or
 * long-press binding) and ticks past the global long-press timeout.  The
 * global timer must not fire for a touch that was owned by an element.
 */

#include "profile/loader.h"
#include "profile/convert.h"
#include "core/processor.h"
#include "core/state.h"
#include "core/element_helpers.h"

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
    g_fake_ms = tick * (uint64_t)g_timer_interval_ms;
}

static TouchActionResult down(int ptr_id, float x, float y, uint64_t tick) {
    set_tick(tick);
    TouchActionResult result;
    memset(&result, 0, sizeof(result));
    touch_processor_on_finger_down(&result, ptr_id, x, y, tick);
    return result;
}

static TouchActionResult move(int ptr_id, float x, float y, uint64_t tick) {
    set_tick(tick);
    TouchActionResult result;
    memset(&result, 0, sizeof(result));
    touch_processor_on_finger_move(&result, ptr_id, x, y, tick);
    return result;
}

static TouchActionResult up(int ptr_id, float x, float y, uint64_t tick) {
    set_tick(tick);
    TouchActionResult result;
    memset(&result, 0, sizeof(result));
    touch_processor_on_finger_up(&result, ptr_id, x, y, tick);
    return result;
}

static TouchActionResult tick(uint64_t tick) {
    set_tick(tick);
    TouchActionResult result;
    memset(&result, 0, sizeof(result));
    touch_processor_tick(&result, tick);
    return result;
}

static float gesture_threshold(void) {
    return g_state.cfg.gesture_threshold_px > 0
        ? (float)g_state.cfg.gesture_threshold_px
        : (float)g_state.cfg.gesture_threshold_px_fallback;
}

static int has_haptic(const TouchActionResult *result, int effect) {
    for (int i = 0; i < (int)result->count; i++) {
        const TouchAction *action = &result->actions[i];
        if (action->type == ACT_HAPTIC && action->haptic.effect == effect)
            return 1;
    }
    return 0;
}

static int has_press(const TouchActionResult *result) {
    for (int i = 0; i < (int)result->count; i++) {
        const TouchAction *action = &result->actions[i];
        if (action->type == ACT_KEY_PRESS || action->type == ACT_POINTER_BUTTON_PRESS)
            return 1;
        if (action->type == ACT_GAMEPAD_STATE && action->key.is_down)
            return 1;
    }
    return 0;
}

static TouchElement *find_gesture_button(void) {
    for (int i = 0; i < g_state.element_count; i++) {
        TouchElement *e = &g_state.elements[i];
        if (e->type != BUTTON || e->shape != RECT) continue;
        if (!(e->flags & TOUCH_BIT_IS_HOVER)) continue;
        if (!(e->flags & TOUCH_BIT_CACHED_HAS_GESTURE)) continue;
        if (e->flags & (TOUCH_BIT_CACHED_HAS_TOGGLE |
                        TOUCH_BIT_CACHED_GESTURE_HAS_TOGGLE)) continue;
        if (e->hw < 10.0f) continue;
        return e;
    }
    return NULL;
}

static TouchElement *find_plain_hover_button(void) {
    for (int i = 0; i < g_state.element_count; i++) {
        TouchElement *e = &g_state.elements[i];
        if (e->type != BUTTON || e->shape != RECT) continue;
        if (!(e->flags & TOUCH_BIT_IS_HOVER)) continue;
        if (e->flags & (TOUCH_BIT_CACHED_HAS_GESTURE |
                        TOUCH_BIT_CACHED_HAS_LONG_PRESS |
                        TOUCH_BIT_CACHED_HAS_TOGGLE)) continue;
        if (e->bindings[0].type == NONE) continue;
        if (e->hw < 10.0f) continue;
        return e;
    }
    return NULL;
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
    profile_to_config(&profile, SCREEN_W, SCREEN_H, 1.0f, &config);
    touch_processor_init(&config);
    g_state.snapping_size = (float)(SCREEN_W / 100);
    int element_count = profile_to_elements(&profile, SCREEN_W, SCREEN_H,
                                            elements, MAX_ELEMENTS);
    touch_processor_set_elements(elements, element_count);
    touch_processor_set_resolution_scale(1.0f);
    touch_processor_set_xform_scale(1.0f, 1.0f);
    touch_processor_set_sim_touch_screen(false);

    TouchElement *btn = find_gesture_button();
    if (!btn) {
        fail("profile has no non-toggle HOVER button with a gesture binding");
        return 1;
    }

    float th = gesture_threshold();
    if (!(th > 5.0f)) {
        fail("unexpected gesture threshold");
        return 1;
    }
    if (!(btn->cached_left > th + 12.0f)) {
        fail("gesture button is too close to the left screen edge for the test");
        return 1;
    }
    if (btn->button_gesture_haptic <= 0) {
        fail("gesture haptic is disabled; the test needs it to identify the gesture action");
        return 1;
    }

    const float y = (float)btn->y;
    const float down_x = btn->cached_left + 2.0f;
    const float out_below_threshold_x = btn->cached_left - 2.0f;
    const float out_above_threshold_x = btn->cached_left - (th + 8.0f);
    if (!point_in_element(down_x, y, btn))
        fail("down point is not inside the gesture button");
    if (point_in_element(out_below_threshold_x, y, btn))
        fail("first outside point is still inside the gesture button");
    if (point_in_element(out_above_threshold_x, y, btn))
        fail("second outside point is still inside the gesture button");

    /* Leave the rect before the threshold, then cross it outside. */
    down(0, down_x, y, 100);
    if (btn->current_ptr_id != 0 || !(btn->flags & TOUCH_BIT_ENGAGED))
        fail("gesture button did not engage on down");

    TouchActionResult below = move(0, out_below_threshold_x, y, 101);
    if (btn->current_ptr_id != 0 || !(btn->flags & TOUCH_BIT_ENGAGED))
        fail("gesture button was released on the first outside sample");
    if (btn->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED)
        fail("swipe fired before the threshold was reached");
    if (has_haptic(&below, btn->button_gesture_haptic))
        fail("gesture haptic fired before the threshold was reached");

    TouchActionResult above = move(0, out_above_threshold_x, y, 102);
    if (!(btn->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED))
        fail("swipe outside the button did not reach the gesture threshold");
    if (!has_haptic(&above, btn->button_gesture_haptic))
        fail("gesture haptic missing after the outside swipe");
    if (!has_press(&above))
        fail("gesture bindings were not pressed after the outside swipe");

    TouchActionResult swipe_up = up(0, out_above_threshold_x, y, 103);
    if (has_press(&swipe_up))
        fail("swipe UP pressed a binding again");
    if (btn->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED)
        fail("swipe flag stayed set after UP");
    if (g_state.active_finger_count != 0)
        fail("finger stayed active after the swipe");

    /* Leave the rect below the threshold and lift outside: no tap, no haptic. */
    down(0, down_x, y, 200);
    TouchActionResult short_leave = move(0, out_below_threshold_x, y, 201);
    if (btn->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED)
        fail("short leave fired the swipe");
    if (has_haptic(&short_leave, btn->button_gesture_haptic))
        fail("short leave emitted the gesture haptic");

    TouchActionResult short_up = up(0, out_below_threshold_x, y, 202);
    if (has_press(&short_up))
        fail("outside UP leaked a deferred tap");
    if (has_haptic(&short_up, btn->button_gesture_haptic))
        fail("outside UP emitted a gesture haptic");
    if (g_state.active_finger_count != 0)
        fail("finger stayed active after the short leave");

    /* A coalesced fast swipe has no MOVE at all; the UP point is the only sample. */
    down(0, down_x, y, 300);
    TouchActionResult fast_up = up(0, out_above_threshold_x, y, 301);
    if (!has_haptic(&fast_up, btn->button_gesture_haptic))
        fail("coalesced swipe did not emit the gesture haptic");
    if (!has_press(&fast_up))
        fail("coalesced swipe did not press the gesture bindings");
    if (btn->flags & TOUCH_BIT_GESTURE_SWIPE_TRIGGERED)
        fail("coalesced swipe left the swipe flag set");
    if (g_state.active_finger_count != 0)
        fail("finger stayed active after the coalesced swipe");

    /*
     * A plain HOVER button releases on the first outside MOVE.  The finger
     * is then active but has no gesture state.  Its cached global long-press
     * timer must not fire after the button is gone; that leaked the global
     * long-press action and haptic when leaving ESC/Shift/Enter.
     */
    TouchElement *plain = find_plain_hover_button();
    if (!plain) {
        fail("profile has no plain HOVER button for the long-press leak test");
    } else {
        float py = (float)plain->y;
        float inside_x, outside_x;
        if (plain->cached_left > 2.0f) {
            inside_x = plain->cached_left + 2.0f;
            outside_x = plain->cached_left - 2.0f;
        } else {
            inside_x = plain->cached_right - 2.0f;
            outside_x = plain->cached_right + 2.0f;
        }
        if (!point_in_element(inside_x, py, plain))
            fail("plain button down point is not inside the button");
        if (point_in_element(outside_x, py, plain))
            fail("plain button outside point is still inside the button");

        down(0, inside_x, py, 400);
        if (plain->current_ptr_id != 0)
            fail("plain button did not engage on down");

        TouchActionResult plain_leave = move(0, outside_x, py, 401);
        (void)plain_leave;
        if (plain->current_ptr_id >= 0)
            fail("plain button did not release on the outside move");

        uint64_t after_timeout = 401
            + (uint64_t)g_state.cfg.long_press_timeout_ticks + 2;
        TouchActionResult leaked = tick(after_timeout);
        if (g_state.cfg.gesture_long_press_haptic > 0
            && has_haptic(&leaked, g_state.cfg.gesture_long_press_haptic))
            fail("global long-press haptic fired after leaving a plain button");
        if (has_press(&leaked))
            fail("global long-press action fired after leaving a plain button");

        TouchActionResult plain_up = up(0, outside_x, py, after_timeout + 1);
        if (has_press(&plain_up))
            fail("plain button outside UP pressed a binding");
        if (g_state.active_finger_count != 0)
            fail("finger stayed active after the plain button leave");
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("gesture_button_swipe: PASS\n");
    return 0;
}
