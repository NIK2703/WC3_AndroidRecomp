/*
 * T0: the central timer registry must expose exactly the deadlines that
 * touch_processor_tick() can advance, and an active finger alone must not
 * request a tick.  The test links the real registry against a synthetic state
 * and stubs the tick body.
 */

#include "core/internal.h"

#include <stdio.h>
#include <string.h>

TouchProcessorState g_state;
GestureFingerCtx g_ctx[MAX_FINGERS];
int g_current_finger_idx;

static uint64_t g_test_gesture_deadline = UINT64_MAX;

/* tick.c references these from touch_processor_tick().  This test only calls
 * the registry query and the public needs-tick wrapper. */
void process_scheduled_actions(TouchActionResult* r, uint64_t t) { (void)r; (void)t; }
void gesture_tick(uint64_t t, TouchActionResult* r) { (void)t; (void)r; }
void process_auto_repeat_burst(TouchActionResult* r, TouchBinding* a, int c,
                               uint64_t* l, uint64_t t) {
    (void)r; (void)a; (void)c; (void)l; (void)t;
}
void process_range_button_tick(uint64_t t, TouchActionResult* r) { (void)t; (void)r; }
void udp_mouse_send(int f, int x, int y, int w) { (void)f; (void)x; (void)y; (void)w; }
void process_button_auto_repeat(TouchElement* e, uint64_t t,
                                TouchActionResult* r) { (void)e; (void)t; (void)r; }
void release_held_actions(TouchActionResult* r, uint64_t t) { (void)r; (void)t; }
void process_button_long_press(TouchElement* e, uint64_t t,
                               TouchActionResult* r, TouchFinger* f) {
    (void)e; (void)t; (void)r; (void)f;
}
void process_button_gesture_timer(TouchElement* e, uint64_t t,
                                  TouchActionResult* r, TouchFinger* f) {
    (void)e; (void)t; (void)r; (void)f;
}
void update_visual_layers(TouchElement* e) { (void)e; }
void process_delayed_actions(TouchActionResult* r, uint64_t t) { (void)r; (void)t; }

uint64_t gesture_next_deadline_tick(void) {
    return g_test_gesture_deadline;
}

static int failures;

static void expect_u64(const char *name, uint64_t got, uint64_t want)
{
    if (got != want) {
        printf("FAIL %s: got %llu want %llu\n", name,
               (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static void expect(const char *name, int got, int want)
{
    if (got != want) {
        printf("FAIL %s: got %d want %d\n", name, got, want);
        failures++;
    }
}

static void reset_state(void)
{
    memset(&g_state, 0, sizeof(g_state));
    memset(g_ctx, 0, sizeof(g_ctx));
    g_test_gesture_deadline = TOUCH_PROCESSOR_DEADLINE_NONE;
}

static uint64_t query(void)
{
    wc3_timer_registry_invalidate();
    return touch_processor_next_deadline_tick();
}

static uint64_t query_cached(void)
{
    return touch_processor_next_deadline_tick();
}

static void add_button(void)
{
    g_state.button_count = 1;
    g_state.button_indices[0] = 0;
    g_state.elements[0].current_ptr_id = -1;
}

int main(void)
{
    reset_state();
    expect_u64("idle", query(), TOUCH_PROCESSOR_DEADLINE_NONE);
    expect("idle needs", touch_processor_needs_tick(), 0);

    g_state.active_finger_count = 1;
    expect_u64("active finger alone", query(), TOUCH_PROCESSOR_DEADLINE_NONE);
    g_state.active_finger_count = 0;

    g_state.scheduled_action_count = 1;
    g_state.scheduled_actions[0].active = true;
    g_state.scheduled_actions[0].scheduled_tick = 40;
    expect_u64("scheduled tick", query(), 40);

    /* The cache is not a polling path: an unmarked change stays invisible
     * until the public invalidation point runs. */
    g_state.scheduled_actions[0].scheduled_tick = 80;
    expect_u64("scheduled cached", query_cached(), 40);
    expect_u64("scheduled invalidated", query(), 80);

    g_state.scheduled_actions[0].active = false;
    expect_u64("scheduled all inactive", query(), 0);
    g_state.scheduled_action_count = 0;

    g_state.sim_click_press_tick = 50;
    expect_u64("sim press", query(), 50);
    g_state.sim_click_press_tick = 0;
    g_state.sim_click_release_tick = 60;
    expect_u64("sim release", query(), 60);
    g_state.sim_click_release_tick = 0;

    g_state.gesture_is_action_held = true;
    expect_u64("held release", query(), 0);
    g_state.active_finger_count = 1;
    expect_u64("held with finger", query(), TOUCH_PROCESSOR_DEADLINE_NONE);
    g_state.active_finger_count = 0;
    g_state.gesture_is_action_held = false;

    g_test_gesture_deadline = 70;
    expect_u64("gesture deadline", query(), 70);
    g_test_gesture_deadline = TOUCH_PROCESSOR_DEADLINE_NONE;

    g_state.gesture_toggled_count = 1;
    g_state.gesture_toggled_actions[0].type = MOUSE_LEFT_BUTTON;
    g_state.gesture_toggled_actions[0].flags = TOUCH_BINDING_FLAG_AUTO_REPEAT;
    g_state.gesture_toggled_actions[0].auto_repeat_interval_ticks = 4;
    g_state.gesture_auto_repeat_last_tick[0] = 100;
    expect_u64("gesture repeat", query(), 104);
    g_state.gesture_auto_repeat_last_tick[0] = 0;
    expect_u64("gesture repeat first", query(), 0);
    g_state.gesture_toggled_actions[0].flags = 0;
    expect_u64("gesture sticky toggle", query(), TOUCH_PROCESSOR_DEADLINE_NONE);
    g_state.gesture_toggled_count = 0;

    g_state.active_finger_count = 1;  // keep the held-release fallback off
    g_state.gesture_is_action_held = true;
    g_state.gesture_held_count = 1;
    g_state.gesture_held_actions[0].type = MOUSE_LEFT_BUTTON;
    g_state.gesture_held_actions[0].flags = TOUCH_BINDING_FLAG_AUTO_REPEAT;
    g_state.gesture_held_actions[0].auto_repeat_interval_ticks = 5;
    g_state.gesture_auto_repeat_last_tick_held[0] = 200;
    expect_u64("held repeat", query(), 205);
    g_state.gesture_is_action_held = false;
    expect_u64("held repeat inactive", query(), TOUCH_PROCESSOR_DEADLINE_NONE);
    g_state.gesture_held_count = 0;
    g_state.active_finger_count = 0;

    reset_state();
    add_button();
    g_state.elements[0].flags = TOUCH_BIT_CACHED_HAS_TOGGLE
        | TOUCH_BIT_CACHED_HAS_AUTO_REPEAT
        | TOUCH_BIT_SELECTED;
    expect_u64("button repeat without capability", query(),
               TOUCH_PROCESSOR_DEADLINE_NONE);

    g_state.cfg.caps_has_auto_repeat_buttons = true;
    g_state.elements[0].auto_repeat_last_tick = 10;
    g_state.elements[0].cached_auto_repeat_interval_ticks = 4;
    expect_u64("button toggle repeat", query(), 14);

    g_state.elements[0].cached_auto_repeat_interval_ticks = 0;
    expect_u64("button toggle zero interval", query(), 11);

    g_state.elements[0].flags = TOUCH_BIT_CACHED_HAS_AUTO_REPEAT
        | TOUCH_BIT_ENGAGED;
    g_state.elements[0].current_ptr_id = 1;
    g_state.elements[0].cached_auto_repeat_interval_ticks = 4;
    expect_u64("button non-toggle repeat", query(), 14);

    g_state.elements[0].cached_auto_repeat_interval_ticks = 0;
    expect_u64("button non-toggle zero interval", query(),
               TOUCH_PROCESSOR_DEADLINE_NONE);

    g_state.elements[0].flags = TOUCH_BIT_ENGAGED;
    g_state.elements[0].current_ptr_id = -1;
    expect_u64("stale engaged", query(), 0);

    reset_state();
    add_button();
    g_state.cfg.caps_has_any_element_long_press = true;
    g_state.elements[0].flags = TOUCH_BIT_CACHED_HAS_LONG_PRESS
        | TOUCH_BIT_LONG_PRESS_ARM;
    g_state.elements[0].current_ptr_id = 1;
    g_state.elements[0].down_tick = 20;
    g_state.g_element_long_press[0][0].type = MOUSE_LEFT_BUTTON;
    g_state.g_element_long_press_count[0] = 1;
    g_state.cfg.long_press_delay_ticks = 5;
    expect_u64("button long press", query(), 25);
    g_state.cfg.long_press_delay_ticks = -1;
    expect_u64("button long press disabled", query(),
               TOUCH_PROCESSOR_DEADLINE_NONE);

    reset_state();
    add_button();
    g_state.cfg.caps_has_any_element_gesture = true;
    g_state.elements[0].flags = TOUCH_BIT_CACHED_HAS_GESTURE
        | TOUCH_BIT_GESTURE_TIMER_ARMED;
    g_state.elements[0].current_ptr_id = 1;
    g_state.elements[0].down_tick = 30;
    g_state.g_element_gesture[0][0].type = MOUSE_LEFT_BUTTON;
    g_state.g_element_gesture_count[0] = 1;
    g_state.cfg.gesture_swipe_min_ticks = 6;
    expect_u64("button gesture timer", query(), 36);

    reset_state();
    g_state.has_active_ranges = true;
    expect_u64("stale range latch without ranges", query(), 0);

    g_state.range_count = 1;
    g_state.range_indices[0] = 0;
    g_state.elements[0].current_ptr_id = 1;
    g_state.elements[0].range_has_binding = true;
    g_state.elements[0].range_initial_kc = 1;
    g_state.elements[0].down_tick = 40;
    g_state.cfg.long_press_delay_ticks = 5;
    expect_u64("range long press", query(), 45);

    g_state.elements[0].current_ptr_id = -1;
    g_state.elements[0].range_pending_tap_release = true;
    g_state.elements[0].range_tap_release_tick = 50;
    expect_u64("range deferred release", query(), 50);

    g_state.elements[0].range_pending_tap_release = false;
    expect_u64("stale range latch", query(), 0);

    reset_state();
    g_state.last_tick_time = 60;
    g_state.mouse_move_offset_x = 1.0f;
    expect_u64("mouse movement", query(), 61);
    g_state.mouse_move_offset_x = 0.0f;

    if (failures == 0) printf("controls idle gate OK\n");
    return failures != 0;
}
