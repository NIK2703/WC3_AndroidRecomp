/*
 * T0: direct-touch transport and recognizer.
 *
 * The queue is the host/HLE boundary: it must preserve order, drop cleanly
 * when full, and never expose a partial event.  The recognizer is the other
 * half: a tap is one event on release, a long press fires exactly once, a
 * second pointer cancels the tap, and all emitted coordinates are normalised
 * surface fractions.
 */
#include "direct_touch.h"
#include "direct_touch_event.h"
#include "hle_direct_touch_intent.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static Wc3DirectTouchEvent g_events[128];
static int g_event_count;
static int g_wake_count;

static void sink(const Wc3DirectTouchEvent *event, void *user)
{
    (void)user;
    assert(g_event_count < (int)(sizeof(g_events) / sizeof(g_events[0])));
    g_events[g_event_count++] = *event;
}

static void wake(void *user)
{
    (void)user;
    g_wake_count++;
}

static void events_clear(void)
{
    g_event_count = 0;
}

static void expect_near(const char *name, float got, float want)
{
    if (fabsf(got - want) > 0.0001f) {
        fprintf(stderr, "FAIL: %s: got %.6f want %.6f\n", name, got, want);
        assert(0);
    }
}

static void test_queue(void)
{
    Wc3DirectTouchEvent input;
    Wc3DirectTouchEvent output;
    memset(&input, 0, sizeof(input));
    memset(&output, 0, sizeof(output));

    wc3_direct_touch_clear();
    assert(wc3_direct_touch_pending() == 0);
    wc3_direct_touch_set_wake(wake, NULL);
    g_wake_count = 0;

    for (uint32_t i = 0; i < WC3_DIRECT_TOUCH_QUEUE_CAP; i++) {
        input.kind = WC3_DIRECT_TOUCH_TAP;
        input.pointer_id = i;
        input.x = (float)i / 100.0f;
        assert(wc3_direct_touch_push(&input) == 1);
    }
    assert(wc3_direct_touch_pending() == WC3_DIRECT_TOUCH_QUEUE_CAP);
    assert(g_wake_count == (int)WC3_DIRECT_TOUCH_QUEUE_CAP);
    assert(wc3_direct_touch_push(&input) == 0);
    assert(g_wake_count == (int)WC3_DIRECT_TOUCH_QUEUE_CAP);

    for (uint32_t i = 0; i < WC3_DIRECT_TOUCH_QUEUE_CAP; i++) {
        assert(wc3_direct_touch_pop(&output) == 1);
        assert(output.pointer_id == i);
        expect_near("queue x", output.x, (float)i / 100.0f);
    }
    assert(wc3_direct_touch_pop(&output) == 0);
    assert(wc3_direct_touch_pending() == 0);

    memset(&input, 0, sizeof(input));
    input.kind = WC3_DIRECT_TOUCH_LONG_PRESS;
    input.pointer_id = 77;
    assert(wc3_direct_touch_push(&input) == 1);
    assert(wc3_direct_touch_pending() == 1);
    wc3_direct_touch_clear();
    assert(wc3_direct_touch_pending() == 0);
    assert(wc3_direct_touch_pop(&output) == 0);

    wc3_direct_touch_set_wake(NULL, NULL);
}

static Wc3DirectTouch make_recognizer(void)
{
    Wc3DirectTouch dt;
    wc3_direct_touch_init(&dt, 1000.0f, 1000.0f, 20.0f, 60.0f, 450,
                          sink, NULL);
    return dt;
}

static void test_tap(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    wc3_direct_touch_down(&dt, 1, 100.0f, 200.0f, 1000);
    wc3_direct_touch_move(&dt, 1, 105.0f, 205.0f, 1010);
    wc3_direct_touch_up(&dt, 1, 105.0f, 205.0f, 1050);

    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_TAP);
    assert(g_events[0].pointer_id == 1);
    expect_near("tap x", g_events[0].x, 0.105f);
    expect_near("tap y", g_events[0].y, 0.205f);
    assert(g_events[0].time_ms == 1050);
}

static void test_tap_cancelled_by_motion(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    wc3_direct_touch_down(&dt, 2, 100.0f, 100.0f, 0);
    wc3_direct_touch_move(&dt, 2, 130.0f, 100.0f, 100);
    wc3_direct_touch_up(&dt, 2, 130.0f, 100.0f, 200);
    assert(g_event_count == 0);
}

static void test_up_without_move_uses_slop(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    /* A flick can arrive as DOWN+UP with no MOVE: the release point must
     * still be inside the tap slop, otherwise the tap is dropped. */
    wc3_direct_touch_down(&dt, 4, 100.0f, 100.0f, 0);
    wc3_direct_touch_up(&dt, 4, 130.0f, 100.0f, 100);
    assert(g_event_count == 0);

    wc3_direct_touch_down(&dt, 5, 200.0f, 200.0f, 200);
    wc3_direct_touch_up(&dt, 5, 210.0f, 205.0f, 250);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_TAP);
    expect_near("no-move tap x", g_events[0].x, 0.21f);

    /* A release after the long-press deadline but before any tick must not
     * degrade into a tap. */
    events_clear();
    wc3_direct_touch_down(&dt, 6, 300.0f, 300.0f, 300);
    wc3_direct_touch_up(&dt, 6, 300.0f, 300.0f, 800);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_LONG_PRESS);
    expect_near("late tick long x", g_events[0].x, 0.3f);
}

static void test_long_press_fires_once(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    wc3_direct_touch_down(&dt, 3, 500.0f, 600.0f, 2000);
    assert(wc3_direct_touch_tick(&dt, 2449) == 0);
    assert(g_event_count == 0);
    assert(wc3_direct_touch_tick(&dt, 2450) == 1);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_LONG_PRESS);
    assert(g_events[0].pointer_id == 3);
    expect_near("long x", g_events[0].x, 0.5f);
    expect_near("long y", g_events[0].y, 0.6f);
    assert(wc3_direct_touch_tick(&dt, 3000) == 0);
    wc3_direct_touch_up(&dt, 3, 500.0f, 600.0f, 3100);
    assert(g_event_count == 1);
    assert(wc3_direct_touch_next_deadline_ms(&dt) == 0);
}

static void test_long_press_tolerates_tap_drift(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    /* 30 px is outside the 20 px tap slop but inside the 60 px long-press
     * slop: the tap is cancelled, the long press still fires. */
    wc3_direct_touch_down(&dt, 8, 100.0f, 100.0f, 0);
    wc3_direct_touch_move(&dt, 8, 130.0f, 100.0f, 100);
    assert(wc3_direct_touch_tick(&dt, 449) == 0);
    assert(wc3_direct_touch_tick(&dt, 450) == 1);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_LONG_PRESS);
    expect_near("drift long x", g_events[0].x, 0.13f);
    wc3_direct_touch_up(&dt, 8, 130.0f, 100.0f, 500);
    assert(g_event_count == 1);

    /* 70 px is outside the long-press slop: neither gesture fires. */
    events_clear();
    wc3_direct_touch_down(&dt, 9, 100.0f, 100.0f, 1000);
    wc3_direct_touch_move(&dt, 9, 170.0f, 100.0f, 1100);
    assert(wc3_direct_touch_tick(&dt, 1450) == 0);
    assert(wc3_direct_touch_next_deadline_ms(&dt) == 0);
    wc3_direct_touch_up(&dt, 9, 170.0f, 100.0f, 1500);
    assert(g_event_count == 0);
}

static void test_second_pointer_cancels(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    wc3_direct_touch_down(&dt, 4, 100.0f, 100.0f, 0);
    wc3_direct_touch_down(&dt, 5, 200.0f, 200.0f, 10);
    assert(wc3_direct_touch_active_count(&dt) == 2);
    assert(wc3_direct_touch_tick(&dt, 1000) == 0);
    wc3_direct_touch_up(&dt, 4, 100.0f, 100.0f, 1100);
    wc3_direct_touch_up(&dt, 5, 200.0f, 200.0f, 1110);
    assert(g_event_count == 0);
}

static void test_reset_discards_pending(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    wc3_direct_touch_down(&dt, 6, 100.0f, 100.0f, 0);
    wc3_direct_touch_reset(&dt);
    assert(wc3_direct_touch_active_count(&dt) == 0);
    assert(wc3_direct_touch_tick(&dt, 1000) == 0);
    wc3_direct_touch_up(&dt, 6, 100.0f, 100.0f, 1100);
    assert(g_event_count == 0);
}

static void test_pointer_snapshot(void)
{
    Wc3DirectTouch dt = make_recognizer();
    wc3_direct_touch_down(&dt, 7, 10.0f, 20.0f, 0);
    wc3_direct_touch_move(&dt, 7, 30.0f, 40.0f, 1);

    int id = -1;
    float x = 0.0f, y = 0.0f;
    assert(wc3_direct_touch_pointer_at(&dt, 0, &id, &x, &y) == 1);
    assert(id == 7);
    expect_near("snapshot x", x, 30.0f);
    expect_near("snapshot y", y, 40.0f);
    assert(wc3_direct_touch_pointer_at(&dt, 1, &id, &x, &y) == 0);
}

static void test_coordinate_mapping(void)
{
    /* Surface fraction -> client pixel float.  The +0.5 offset is the
     * existing mouse-path rounding; the native input event then truncates
     * to an integer pixel before 0x6F8B38A0. */
    expect_near("surface left", wc3_dt_surface_to_client(0.0f, 800), 0.5f);
    expect_near("surface mid", wc3_dt_surface_to_client(0.5f, 800), 400.0f);
    expect_near("surface right", wc3_dt_surface_to_client(1.0f, 800), 799.0f);
    expect_near("surface clamp", wc3_dt_surface_to_client(2.0f, 800), 799.0f);
    expect_near("surface negative", wc3_dt_surface_to_client(-1.0f, 800), 0.0f);

    expect_near("pixel left", wc3_dt_client_pixel(0.0f, 800), 0.0f);
    expect_near("pixel mid", wc3_dt_client_pixel(0.5f, 800), 400.0f);
    expect_near("pixel right", wc3_dt_client_pixel(1.0f, 800), 799.0f);
    expect_near("pixel y mid", wc3_dt_client_pixel(0.5f, 600), 300.0f);

    /* 0x6F8B38A0 multiplies by 0x6FAA9328 (0.8f) and 0x6FAA9324 (0.6f). */
    expect_near("pick x mid",
                wc3_dt_pick_axis(wc3_dt_client_pixel(0.5f, 800), 0.8f),
                320.0f);
    expect_near("pick y mid",
                wc3_dt_pick_axis(wc3_dt_client_pixel(0.5f, 600), 0.6f),
                180.0f);
    expect_near("pick x right",
                wc3_dt_pick_axis(wc3_dt_client_pixel(1.0f, 800), 0.8f),
                639.2f);
    expect_near("pick y bottom",
                wc3_dt_pick_axis(wc3_dt_client_pixel(1.0f, 600), 0.6f),
                359.4f);
}

static void test_owner_rule(void)
{
    assert(wc3_dt_owner_is_friendly(3, 3) == 1);
    assert(wc3_dt_owner_is_friendly(12, 3) == 1);
    assert(wc3_dt_owner_is_friendly(14, 3) == 1);
    assert(wc3_dt_owner_is_friendly(15, 3) == 1);
    assert(wc3_dt_owner_is_friendly(13, 3) == 0);
    assert(wc3_dt_owner_is_friendly(4, 3) == 0);
    assert(wc3_dt_owner_is_friendly(0, 3) == 0);
}

static void test_tap_intent_resolution(void)
{
    /* No pick, a validated own unit with no selection, and a selected own
     * unit all select.  A valid non-unit sprite is a phase-1 no-op. */
    assert(wc3_dt_tap_intent(WC3_DT_PICK_NONE, 0, 0, 0, 0) ==
           WC3_DT_INTENT_NONE);
    assert(wc3_dt_tap_intent(WC3_DT_PICK_OBJECT, 1, 1, 0, 0) ==
           WC3_DT_INTENT_SELECT_UNIT);
    assert(wc3_dt_tap_intent(WC3_DT_PICK_OBJECT, 1, 1, 1, 0) ==
           WC3_DT_INTENT_SELECT_UNIT);
    assert(wc3_dt_tap_intent(WC3_DT_PICK_OBJECT, 1, 0, 1, 0) ==
           WC3_DT_INTENT_NONE);

    /* An enemy/neutral-hostile unit always orders; with an empty selection
     * the native ghost handler is a no-op, and selecting the enemy would be
     * wrong. */
    assert(wc3_dt_tap_intent(WC3_DT_PICK_OBJECT, 1, 1, 0, 1) ==
           WC3_DT_INTENT_ORDER_UNIT);
    assert(wc3_dt_tap_intent(WC3_DT_PICK_OBJECT, 1, 1, 1, 1) ==
           WC3_DT_INTENT_ORDER_UNIT);

    /* An object that failed 0x6F21B730 is a terrain click, and terrain
     * orders only when something is selected. */
    assert(wc3_dt_tap_intent(WC3_DT_PICK_OBJECT, 0, 0, 0, 0) ==
           WC3_DT_INTENT_CLEAR_SELECTION);
    assert(wc3_dt_tap_intent(WC3_DT_PICK_OBJECT, 0, 0, 1, 0) ==
           WC3_DT_INTENT_ORDER_GROUND);
    assert(wc3_dt_tap_intent(WC3_DT_PICK_TERRAIN, 0, 0, 0, 0) ==
           WC3_DT_INTENT_CLEAR_SELECTION);
    assert(wc3_dt_tap_intent(WC3_DT_PICK_TERRAIN, 0, 0, 1, 0) ==
           WC3_DT_INTENT_ORDER_GROUND);
}

static void test_long_press_intent_resolution(void)
{
    assert(wc3_dt_long_press_intent(WC3_DT_PICK_NONE) ==
           WC3_DT_LONG_INTENT_NONE);
    assert(wc3_dt_long_press_intent(WC3_DT_PICK_OBJECT) ==
           WC3_DT_LONG_INTENT_ORDER_UNIT);
    assert(wc3_dt_long_press_intent(WC3_DT_PICK_TERRAIN) ==
           WC3_DT_LONG_INTENT_ORDER_GROUND);
}

int main(void)
{
    test_queue();
    test_tap();
    test_tap_cancelled_by_motion();
    test_up_without_move_uses_slop();
    test_long_press_fires_once();
    test_long_press_tolerates_tap_drift();
    test_second_pointer_cancels();
    test_reset_discards_pending();
    test_pointer_snapshot();
    test_coordinate_mapping();
    test_owner_rule();
    test_tap_intent_resolution();
    test_long_press_intent_resolution();
    puts("direct_touch: ok");
    return 0;
}
