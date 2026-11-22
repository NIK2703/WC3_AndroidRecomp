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
static int g_haptic_count;
static int g_haptic_effect;

static void sink(const Wc3DirectTouchEvent *event, void *user)
{
    (void)user;
    assert(g_event_count < (int)(sizeof(g_events) / sizeof(g_events[0])));
    g_events[g_event_count++] = *event;
}

static void haptic(int effect, void *user)
{
    (void)user;
    g_haptic_effect = effect;
    g_haptic_count++;
}

static void wake(void *user)
{
    (void)user;
    g_wake_count++;
}

static void events_clear(void)
{
    g_event_count = 0;
    g_haptic_count = 0;
    g_haptic_effect = 0;
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
    wc3_direct_touch_init(&dt, 1000.0f, 1000.0f, 20.0f, 450,
                          sink, NULL);
    wc3_direct_touch_set_haptic(&dt, haptic, NULL, 3);
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
    assert(g_events[0].route == WC3_DIRECT_TOUCH_ROUTE_FIELD);
    assert(g_events[0].pointer_id == 1);
    expect_near("tap x", g_events[0].x, 0.105f);
    expect_near("tap y", g_events[0].y, 0.205f);
    assert(g_events[0].time_ms == 1050);
}

static void test_tap_cancelled_by_motion(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    /* Crossing the legacy drag threshold before the long-press deadline is
     * the singleTapDrag gesture: it becomes a native middle-button camera
     * drag, not a tap. */
    wc3_direct_touch_down(&dt, 2, 100.0f, 100.0f, 0);
    wc3_direct_touch_move(&dt, 2, 130.0f, 100.0f, 100);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN);
    assert(wc3_direct_touch_field_drag_active(&dt) == 1);
    wc3_direct_touch_up(&dt, 2, 130.0f, 100.0f, 200);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END);
    assert(wc3_direct_touch_field_drag_active(&dt) == 0);
    assert(g_haptic_count == 0);
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
     * degrade into a tap; it arms the hold, plays the legacy haptic and
     * emits the semantic long press. */
    events_clear();
    wc3_direct_touch_down(&dt, 6, 300.0f, 300.0f, 300);
    wc3_direct_touch_up(&dt, 6, 300.0f, 300.0f, 800);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_LONG_PRESS);
    expect_near("late tick long x", g_events[0].x, 0.3f);
    assert(g_haptic_count == 1);
    assert(g_haptic_effect == 3);
}

static void test_long_press_fires_once(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    wc3_direct_touch_down(&dt, 3, 500.0f, 600.0f, 2000);
    assert(wc3_direct_touch_tick(&dt, 2449) == 0);
    assert(g_event_count == 0);
    assert(g_haptic_count == 0);
    /* The deadline arms the hold and plays the legacy long-press haptic; the
     * semantic action waits for the release so a drag can still become a
     * selection instead. */
    assert(wc3_direct_touch_tick(&dt, 2450) == 1);
    assert(g_event_count == 0);
    assert(g_haptic_count == 1);
    assert(g_haptic_effect == 3);
    assert(wc3_direct_touch_tick(&dt, 3000) == 0);
    wc3_direct_touch_up(&dt, 3, 500.0f, 600.0f, 3100);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_LONG_PRESS);
    assert(g_events[0].route == WC3_DIRECT_TOUCH_ROUTE_FIELD);
    assert(g_events[0].pointer_id == 3);
    expect_near("long x", g_events[0].x, 0.5f);
    expect_near("long y", g_events[0].y, 0.6f);
    assert(g_haptic_count == 1);
    assert(wc3_direct_touch_next_deadline_ms(&dt) == 0);
}

static void test_hold_drift_stays_long_press(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    /* 10 px is inside the 20 px legacy drag threshold: the hold survives and
     * the release emits the semantic long press. */
    wc3_direct_touch_down(&dt, 8, 100.0f, 100.0f, 0);
    wc3_direct_touch_move(&dt, 8, 110.0f, 100.0f, 100);
    assert(wc3_direct_touch_tick(&dt, 449) == 0);
    assert(wc3_direct_touch_tick(&dt, 450) == 1);
    assert(g_event_count == 0);
    assert(g_haptic_count == 1);
    wc3_direct_touch_up(&dt, 8, 110.0f, 100.0f, 500);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_LONG_PRESS);
    expect_near("drift long x", g_events[0].x, 0.11f);
}

static void test_hold_then_drag_selects(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    wc3_direct_touch_down(&dt, 9, 100.0f, 100.0f, 0);
    assert(wc3_direct_touch_tick(&dt, 450) == 1);
    assert(g_haptic_count == 1);
    assert(g_event_count == 0);

    /* Small drift while the hold is armed does not start the selection. */
    wc3_direct_touch_move(&dt, 9, 115.0f, 100.0f, 500);
    assert(g_event_count == 0);
    assert(wc3_direct_touch_field_drag_active(&dt) == 0);

    /* Crossing the threshold after the hold starts the native left-button
     * selection drag; the deferred long-press action is cancelled. */
    wc3_direct_touch_move(&dt, 9, 140.0f, 100.0f, 510);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN);
    assert(g_events[0].route == WC3_DIRECT_TOUCH_ROUTE_FIELD);
    assert(wc3_direct_touch_field_drag_active(&dt) == 1);

    wc3_direct_touch_move(&dt, 9, 160.0f, 120.0f, 520);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE);

    wc3_direct_touch_up(&dt, 9, 180.0f, 140.0f, 600);
    assert(g_event_count == 3);
    assert(g_events[2].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END);
    expect_near("select end x", g_events[2].x, 0.18f);
    assert(wc3_direct_touch_field_drag_active(&dt) == 0);
    assert(g_haptic_count == 1);
    assert(wc3_direct_touch_next_deadline_ms(&dt) == 0);
}

static void test_quick_swipe_camera(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    wc3_direct_touch_down(&dt, 10, 100.0f, 100.0f, 0);
    wc3_direct_touch_move(&dt, 10, 125.0f, 100.0f, 50);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN);
    assert(g_events[0].route == WC3_DIRECT_TOUCH_ROUTE_FIELD);

    /* Once the camera drag owns the pointer, the hold deadline must not arm
     * a second gesture. */
    assert(wc3_direct_touch_tick(&dt, 500) == 0);
    assert(g_haptic_count == 0);

    wc3_direct_touch_move(&dt, 10, 160.0f, 120.0f, 510);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE);

    wc3_direct_touch_up(&dt, 10, 170.0f, 130.0f, 600);
    assert(g_event_count == 3);
    assert(g_events[2].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END);
    assert(wc3_direct_touch_field_drag_active(&dt) == 0);
    assert(g_haptic_count == 0);
}

static void test_late_move_arms_hold(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    /* A batch can deliver a MOVE after the long-press deadline but before
     * controls_tick().  The move must arm the hold and start a selection
     * drag, not classify the gesture as a quick camera swipe. */
    wc3_direct_touch_down(&dt, 12, 100.0f, 100.0f, 0);
    wc3_direct_touch_move(&dt, 12, 125.0f, 100.0f, 460);
    assert(g_haptic_count == 1);
    assert(g_haptic_effect == 3);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN);
    assert(g_events[0].route == WC3_DIRECT_TOUCH_ROUTE_FIELD);

    wc3_direct_touch_move(&dt, 12, 140.0f, 110.0f, 470);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE);

    wc3_direct_touch_up(&dt, 12, 150.0f, 120.0f, 500);
    assert(g_event_count == 3);
    assert(g_events[2].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END);
    assert(wc3_direct_touch_tick(&dt, 600) == 0);
    assert(g_haptic_count == 1);
}

static void test_raw_routes_never_emit_semantics(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    /* NATIVE and MINIMAP pointers are forwarded by controls.c as raw
     * phases.  The recognizer must not turn their move/up into a field
     * drag, a tap or a long press, even when the deadline has passed. */
    wc3_direct_touch_down_route(&dt, 30, 100.0f, 100.0f, 0,
                                WC3_DIRECT_TOUCH_ROUTE_NATIVE);
    wc3_direct_touch_move(&dt, 30, 200.0f, 100.0f, 500);
    wc3_direct_touch_up(&dt, 30, 200.0f, 100.0f, 600);

    wc3_direct_touch_down_route(&dt, 31, 100.0f, 100.0f, 1000,
                                WC3_DIRECT_TOUCH_ROUTE_MINIMAP);
    wc3_direct_touch_move(&dt, 31, 200.0f, 100.0f, 1500);
    wc3_direct_touch_up(&dt, 31, 200.0f, 100.0f, 1600);

    assert(g_event_count == 0);
    assert(g_haptic_count == 0);
    assert(wc3_direct_touch_active_count(&dt) == 0);
}

static void test_reset_releases_field_drag(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    wc3_direct_touch_down(&dt, 11, 100.0f, 100.0f, 0);
    wc3_direct_touch_move(&dt, 11, 140.0f, 100.0f, 50);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN);

    wc3_direct_touch_reset(&dt);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END);
    assert(wc3_direct_touch_active_count(&dt) == 0);
    assert(wc3_direct_touch_field_drag_active(&dt) == 0);
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
    assert(g_haptic_count == 0);
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
    /* A miss has no action. */
    assert(wc3_dt_tap_intent(WC3_DT_PICK_NONE, 0, 0, 0) ==
           WC3_DT_INTENT_NONE);

    /* Without a controlled selection every unit tap is the left-click
     * analogue and selects that unit, including a hostile or neutral unit
     * and an ally whose owner has not shared control.  The resolver does not
     * take an owner/friendliness argument any more. */
    assert(wc3_dt_tap_intent(WC3_DT_PICK_OBJECT, 1, 1, 0) ==
           WC3_DT_INTENT_SELECT_UNIT);

    /* A validated non-unit sprite remains a no-op in phase 1. */
    assert(wc3_dt_tap_intent(WC3_DT_PICK_OBJECT, 1, 0, 0) ==
           WC3_DT_INTENT_NONE);

    /* A controlled selection makes every unit tap the right-click
     * analogue, friendly or hostile. */
    assert(wc3_dt_tap_intent(WC3_DT_PICK_OBJECT, 1, 1, 1) ==
           WC3_DT_INTENT_ORDER_UNIT);

    /* An object that failed 0x6F21B730 is a terrain click.  Terrain orders
     * only when the selection can receive an order; otherwise it clears.
     * A non-empty but uncontrollable selection is not an order source. */
    assert(wc3_dt_tap_intent(WC3_DT_PICK_OBJECT, 0, 0, 0) ==
           WC3_DT_INTENT_CLEAR_SELECTION);
    assert(wc3_dt_tap_intent(WC3_DT_PICK_OBJECT, 0, 0, 1) ==
           WC3_DT_INTENT_ORDER_GROUND);
    assert(wc3_dt_tap_intent(WC3_DT_PICK_TERRAIN, 0, 0, 0) ==
           WC3_DT_INTENT_CLEAR_SELECTION);
    assert(wc3_dt_tap_intent(WC3_DT_PICK_TERRAIN, 0, 0, 1) ==
           WC3_DT_INTENT_ORDER_GROUND);
}

static void test_route(void)
{
    /* Normal gameplay: CSelectMode is stacked and owns the top input. */
    assert(wc3_dt_route(1, 1, 0, 1) == WC3_DIRECT_TOUCH_ROUTE_FIELD);
    /* Targeting on top: native pointer phases place the target or building;
     * direct touch must not drop the tap and must not use the overlay. */
    assert(wc3_dt_route(1, 1, 1, 1) == WC3_DIRECT_TOUCH_ROUTE_NATIVE);
    /* Select mode is stacked, but another mode owns the top input. */
    assert(wc3_dt_route(1, 1, 0, 0) == WC3_DIRECT_TOUCH_ROUTE_NATIVE);
    /* Menu/loading/modal: the native UI is still the right recipient. */
    assert(wc3_dt_route(1, 0, 0, 0) == WC3_DIRECT_TOUCH_ROUTE_NATIVE);
    assert(wc3_dt_route(1, 0, 0, 1) == WC3_DIRECT_TOUCH_ROUTE_NATIVE);
    /* UI singleton not readable yet: direct touch cannot claim the pointer. */
    assert(wc3_dt_route(0, 0, 0, 0) == WC3_DIRECT_TOUCH_ROUTE_LEGACY);
}

static void test_field_drag_keep(void)
{
    /* An idle field can start a drag only while the route is FIELD. */
    assert(wc3_dt_field_drag_keep(WC3_DIRECT_TOUCH_ROUTE_FIELD, 0));
    assert(!wc3_dt_field_drag_keep(WC3_DIRECT_TOUCH_ROUTE_NATIVE, 0));
    assert(!wc3_dt_field_drag_keep(WC3_DIRECT_TOUCH_ROUTE_LEGACY, 0));

    /* The native press can change the active mode after BEGIN.  The owned
     * drag must survive that transient route change until its END. */
    assert(wc3_dt_field_drag_keep(WC3_DIRECT_TOUCH_ROUTE_NATIVE, 1));
    assert(wc3_dt_field_drag_keep(WC3_DIRECT_TOUCH_ROUTE_LEGACY, 1));
}

static void test_pointer_phases(void)
{
    Wc3DirectTouch dt;
    wc3_direct_touch_init(&dt, 100.0f, 200.0f, 8.0f, 450,
                          sink, NULL);
    events_clear();
    wc3_direct_touch_emit_pointer(&dt, WC3_DIRECT_TOUCH_POINTER_DOWN,
                                  7, 50.0f, 100.0f, 10);
    wc3_direct_touch_emit_pointer(&dt, WC3_DIRECT_TOUCH_POINTER_MOVE,
                                  7, 60.0f, 120.0f, 11);
    wc3_direct_touch_emit_pointer(&dt, WC3_DIRECT_TOUCH_POINTER_UP,
                                  7, 60.0f, 120.0f, 12);
    /* Semantic kinds are not accepted by the raw-pointer entry point. */
    wc3_direct_touch_emit_pointer(&dt, WC3_DIRECT_TOUCH_TAP,
                                  7, 60.0f, 120.0f, 13);

    assert(g_event_count == 3);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_POINTER_DOWN);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_POINTER_MOVE);
    assert(g_events[2].kind == WC3_DIRECT_TOUCH_POINTER_UP);
    assert(g_events[0].route == WC3_DIRECT_TOUCH_ROUTE_NATIVE);
    assert(g_events[1].route == WC3_DIRECT_TOUCH_ROUTE_NATIVE);
    assert(g_events[2].route == WC3_DIRECT_TOUCH_ROUTE_NATIVE);
    expect_near("pointer move x", g_events[1].x, 0.6f);
    expect_near("pointer move y", g_events[1].y, 0.6f);
}

static void test_portrait_gesture(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    /* The portrait follow starts on DOWN, not after a long-press deadline,
     * and the matching UP ends it. */
    wc3_direct_touch_down_route(&dt, 20, 100.0f, 850.0f, 1000,
                                WC3_DIRECT_TOUCH_ROUTE_PORTRAIT);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FOLLOW_START);
    assert(g_events[0].route == WC3_DIRECT_TOUCH_ROUTE_PORTRAIT);
    expect_near("portrait follow x", g_events[0].x, 0.1f);
    expect_near("portrait follow y", g_events[0].y, 0.85f);
    wc3_direct_touch_up(&dt, 20, 100.0f, 850.0f, 1050);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_HOLD_RELEASE);
    assert(g_events[1].route == WC3_DIRECT_TOUCH_ROUTE_PORTRAIT);

    /* Moving anywhere, including outside the portrait, does not cancel the
     * follow and does not create a long-press event.  Only the owning
     * finger's UP ends it. */
    events_clear();
    wc3_direct_touch_down_route(&dt, 21, 100.0f, 850.0f, 2000,
                                WC3_DIRECT_TOUCH_ROUTE_PORTRAIT);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FOLLOW_START);
    wc3_direct_touch_move(&dt, 21, 800.0f, 100.0f, 2100);
    assert(g_event_count == 1);
    assert(wc3_direct_touch_tick(&dt, 2450) == 0);
    assert(g_event_count == 1);
    wc3_direct_touch_up(&dt, 21, 800.0f, 100.0f, 2500);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_HOLD_RELEASE);
    assert(g_events[1].route == WC3_DIRECT_TOUCH_ROUTE_PORTRAIT);

    /* A cancelled gesture must not leave the follow latched. */
    events_clear();
    wc3_direct_touch_down_route(&dt, 23, 100.0f, 850.0f, 4000,
                                WC3_DIRECT_TOUCH_ROUTE_PORTRAIT);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FOLLOW_START);
    wc3_direct_touch_reset(&dt);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_HOLD_RELEASE);

    /* A second pointer does not cancel the follow owned by the first one;
     * the release still arrives for the portrait pointer. */
    events_clear();
    wc3_direct_touch_down_route(&dt, 24, 100.0f, 850.0f, 5000,
                                WC3_DIRECT_TOUCH_ROUTE_PORTRAIT);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FOLLOW_START);
    wc3_direct_touch_down(&dt, 25, 200.0f, 200.0f, 5010);
    assert(g_event_count == 1);
    wc3_direct_touch_up(&dt, 24, 100.0f, 850.0f, 5100);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_HOLD_RELEASE);
    wc3_direct_touch_up(&dt, 25, 200.0f, 200.0f, 5110);
    assert(g_event_count == 2);
}

static void test_long_press_intent_resolution(void)
{
    assert(wc3_dt_long_press_intent(WC3_DT_PICK_NONE, 0, 0, 0) ==
           WC3_DT_LONG_INTENT_NONE);
    assert(wc3_dt_long_press_intent(WC3_DT_PICK_OBJECT, 1, 1, 1) ==
           WC3_DT_LONG_INTENT_SELECT_UNIT);
    assert(wc3_dt_long_press_intent(WC3_DT_PICK_OBJECT, 1, 0, 0) ==
           WC3_DT_LONG_INTENT_NONE);
    assert(wc3_dt_long_press_intent(WC3_DT_PICK_OBJECT, 1, 1, 0) ==
           WC3_DT_LONG_INTENT_ORDER_UNIT);
    assert(wc3_dt_long_press_intent(WC3_DT_PICK_OBJECT, 0, 0, 0) ==
           WC3_DT_LONG_INTENT_NONE);
    assert(wc3_dt_long_press_intent(WC3_DT_PICK_TERRAIN, 0, 0, 0) ==
           WC3_DT_LONG_INTENT_CLEAR_SELECTION);
}

static void test_beacon_release_resolution(void)
{
    /* Release on the button arms the next minimap tap, even after jitter.
     * A release over the minimap places there and wins over the field; a
     * field release places at the world point; any other drag cancels. */
    assert(wc3_dt_beacon_release(1, 0, 0, 0) == WC3_DT_BEACON_ARM);
    assert(wc3_dt_beacon_release(1, 0, 0, 1) == WC3_DT_BEACON_ARM);
    assert(wc3_dt_beacon_release(0, 1, 0, 0) == WC3_DT_BEACON_MINIMAP);
    assert(wc3_dt_beacon_release(0, 1, 1, 1) == WC3_DT_BEACON_MINIMAP);
    assert(wc3_dt_beacon_release(0, 0, 1, 1) == WC3_DT_BEACON_FIELD);
    assert(wc3_dt_beacon_release(0, 0, 0, 1) == WC3_DT_BEACON_NONE);
    assert(wc3_dt_beacon_release(0, 0, 0, 0) == WC3_DT_BEACON_ARM);
    /* A HUD button must win over the field if the rectangles overlap. */
    assert(wc3_dt_beacon_release(1, 1, 1, 1) == WC3_DT_BEACON_ARM);
}

static void test_minimap_inverse(void)
{
    /* Scale 2, translate (400,300): AddSignal maps world (0,0) to the
     * minimap point (400,300).  The inverse must recover the world point. */
    const float m[9] = { 2.0f, 0.0f, 0.0f,
                         0.0f, 2.0f, 0.0f,
                         400.0f, 300.0f, 1.0f };
    float wx = 0.0f, wy = 0.0f;
    assert(wc3_dt_minimap_screen_to_world(m, 400.0f, 300.0f, &wx, &wy));
    expect_near("minimap inverse origin x", wx, 0.0f);
    expect_near("minimap inverse origin y", wy, 0.0f);

    assert(wc3_dt_minimap_screen_to_world(m, 420.0f, 340.0f, &wx, &wy));
    expect_near("minimap inverse x", wx, 10.0f);
    expect_near("minimap inverse y", wy, 20.0f);

    /* Rotated 90 degrees and translated: (x,y) -> (100-2y, 50+2x). */
    const float r[9] = { 0.0f, 2.0f, 0.0f,
                         -2.0f, 0.0f, 0.0f,
                         100.0f, 50.0f, 1.0f };
    assert(wc3_dt_minimap_screen_to_world(r, 92.0f, 56.0f, &wx, &wy));
    expect_near("minimap rotate x", wx, 3.0f);
    expect_near("minimap rotate y", wy, 4.0f);

    const float singular[9] = { 1.0f, 2.0f, 0.0f,
                                2.0f, 4.0f, 0.0f,
                                0.0f, 0.0f, 1.0f };
    assert(!wc3_dt_minimap_screen_to_world(singular, 1.0f, 1.0f, &wx, &wy));

    const float bad[9] = { 1.0f, 0.0f, 0.0f,
                           0.0f, 1.0f, 0.0f,
                           NAN, 0.0f, 1.0f };
    assert(!wc3_dt_minimap_screen_to_world(bad, 1.0f, 1.0f, &wx, &wy));
}

static void test_minimap_local_point(void)
{
    /* The published frame and the local clamp rect share the same units, so
     * only the fractions matter.  Frame: x 0.10..0.50, y 0.20..0.60; local:
     * x 20..120, y 40..140.  The centre and both extremes must map exactly. */
    float x = 0.0f, y = 0.0f;
    assert(wc3_dt_minimap_local_point(0.10f, 0.20f, 0.20f, 0.10f, 0.60f,
                                      0.50f, 40.0f, 20.0f, 140.0f, 120.0f,
                                      &x, &y));
    expect_near("minimap local top-left x", x, 20.0f);
    expect_near("minimap local top-left y", y, 40.0f);

    assert(wc3_dt_minimap_local_point(0.30f, 0.40f, 0.20f, 0.10f, 0.60f,
                                      0.50f, 40.0f, 20.0f, 140.0f, 120.0f,
                                      &x, &y));
    expect_near("minimap local centre x", x, 70.0f);
    expect_near("minimap local centre y", y, 90.0f);

    assert(wc3_dt_minimap_local_point(0.50f, 0.60f, 0.20f, 0.10f, 0.60f,
                                      0.50f, 40.0f, 20.0f, 140.0f, 120.0f,
                                      &x, &y));
    expect_near("minimap local bottom-right x", x, 120.0f);
    expect_near("minimap local bottom-right y", y, 140.0f);

    /* A release outside the frame is clamped to its edge, matching the
     * native AddSignal clamp. */
    assert(wc3_dt_minimap_local_point(-1.0f, 2.0f, 0.20f, 0.10f, 0.60f,
                                      0.50f, 40.0f, 20.0f, 140.0f, 120.0f,
                                      &x, &y));
    expect_near("minimap local clamp x", x, 20.0f);
    expect_near("minimap local clamp y", y, 140.0f);

    /* Degenerate frame or local rect: no mapping, no divide by zero. */
    assert(!wc3_dt_minimap_local_point(0.3f, 0.4f, 0.2f, 0.5f, 0.6f, 0.5f,
                                       40.0f, 20.0f, 140.0f, 120.0f,
                                       &x, &y));
    assert(!wc3_dt_minimap_local_point(0.3f, 0.4f, 0.2f, 0.1f, 0.6f, 0.5f,
                                       40.0f, 20.0f, 40.0f, 120.0f,
                                       &x, &y));
    assert(!wc3_dt_minimap_local_point(NAN, 0.4f, 0.2f, 0.1f, 0.6f, 0.5f,
                                       40.0f, 20.0f, 140.0f, 120.0f,
                                       &x, &y));
}

int main(void)
{
    test_queue();
    test_tap();
    test_tap_cancelled_by_motion();
    test_up_without_move_uses_slop();
    test_long_press_fires_once();
    test_hold_drift_stays_long_press();
    test_hold_then_drag_selects();
    test_quick_swipe_camera();
    test_late_move_arms_hold();
    test_raw_routes_never_emit_semantics();
    test_reset_releases_field_drag();
    test_second_pointer_cancels();
    test_reset_discards_pending();
    test_pointer_snapshot();
    test_coordinate_mapping();
    test_owner_rule();
    test_tap_intent_resolution();
    test_route();
    test_field_drag_keep();
    test_pointer_phases();
    test_portrait_gesture();
    test_long_press_intent_resolution();
    test_beacon_release_resolution();
    test_minimap_local_point();
    test_minimap_inverse();
    puts("direct_touch: ok");
    return 0;
}
