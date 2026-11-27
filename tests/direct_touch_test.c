/*
 * T0: direct-touch transport and recognizer.
 *
 * The queue is the host/HLE boundary: it must preserve order, drop cleanly
 * when full, and never expose a partial event.  The recognizer is the other
 * half: a tap is one event on release, a long press presses the native
 * selection button exactly once at the deadline and releases it on UP, a
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
static int g_hover_count;
static int g_wake_count;
static int g_haptic_count;
static int g_haptic_effect;

static void sink(const Wc3DirectTouchEvent *event, void *user)
{
    (void)user;
    /* Field hover is a native-cursor phase consumed by the service before
     * semantic dispatch.  Count it separately so the gesture tests below
     * keep asserting the semantic stream (tap/selection/camera) only. */
    if (event->kind == WC3_DIRECT_TOUCH_FIELD_HOVER) {
        g_hover_count++;
        return;
    }
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
    g_hover_count = 0;
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
    wc3_direct_touch_init(&dt, 1000.0f, 1000.0f, 20.0f, 120,
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

static void test_field_hover(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    /* A field contact publishes its point on DOWN so a target/placement
     * preview can move before the release click, then publishes the final
     * point again on UP.  The service ignores both outside a live target
     * mode, so a camera candidate does not move the shared cursor. */
    wc3_direct_touch_down(&dt, 40, 300.0f, 400.0f, 0);
    assert(g_hover_count == 1);
    assert(g_event_count == 0);

    wc3_direct_touch_move(&dt, 40, 310.0f, 410.0f, 10);
    assert(g_hover_count == 1);

    wc3_direct_touch_up(&dt, 40, 310.0f, 410.0f, 50);
    assert(g_hover_count == 2);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_TAP);
    assert(g_events[0].owner_role == WC3_DT_OWNER_ACTION);

    /* Each field DOWN publishes its contact point.  Roles are still resolved
     * on MOVE or at the long-press deadline, and both contacts stay camera
     * candidates until then, even when one of them is already a cursor. */
    events_clear();
    wc3_direct_touch_down(&dt, 41, 500.0f, 500.0f, 1000);
    wc3_direct_touch_down(&dt, 42, 600.0f, 600.0f, 1001);
    assert(g_hover_count == 2);
    assert(wc3_direct_touch_contact_role(&dt, 41) ==
           WC3_DT_CONTACT_UNASSIGNED);
    assert(wc3_direct_touch_contact_role(&dt, 42) ==
           WC3_DT_CONTACT_UNASSIGNED);

    wc3_direct_touch_reset(&dt);
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
    assert(g_events[0].owner_role == WC3_DT_OWNER_CAMERA);
    assert(g_events[0].gesture_id != 0u);
    assert(wc3_direct_touch_field_drag_active(&dt) == 0);
    assert(wc3_direct_touch_camera_pointer(&dt) == 2);
    wc3_direct_touch_up(&dt, 2, 130.0f, 100.0f, 200);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END);
    assert(g_events[1].owner_role == WC3_DT_OWNER_CAMERA);
    assert(g_events[1].gesture_id == g_events[0].gesture_id);
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
     * degrade into a tap.  The press and release are emitted in one step so
     * the native left button is not left latched. */
    events_clear();
    wc3_direct_touch_down(&dt, 6, 300.0f, 300.0f, 300);
    wc3_direct_touch_up(&dt, 6, 300.0f, 300.0f, 800);
    assert(g_event_count == 2);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END);
    expect_near("late tick press x", g_events[0].x, 0.3f);
    expect_near("late tick release x", g_events[1].x, 0.3f);
    assert(g_haptic_count == 1);
    assert(g_haptic_effect == 3);
}

static void test_long_press_presses_once(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    wc3_direct_touch_down(&dt, 3, 500.0f, 600.0f, 2000);
    assert(wc3_direct_touch_tick(&dt, 2119) == 0);
    assert(g_event_count == 0);
    assert(g_haptic_count == 0);
    /* The deadline arms the hold, plays the legacy long-press haptic and
     * presses the native left button at the hold point.  The press stays
     * down until the finger is released. */
    assert(wc3_direct_touch_tick(&dt, 2120) == 1);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN);
    assert(g_events[0].route == WC3_DIRECT_TOUCH_ROUTE_FIELD);
    assert(g_events[0].pointer_id == 3);
    expect_near("long press x", g_events[0].x, 0.5f);
    expect_near("long press y", g_events[0].y, 0.6f);
    assert(g_events[0].time_ms == 2120);
    assert(g_haptic_count == 1);
    assert(g_haptic_effect == 3);
    assert(wc3_direct_touch_tick(&dt, 3000) == 0);
    wc3_direct_touch_up(&dt, 3, 500.0f, 600.0f, 3100);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END);
    assert(g_events[1].time_ms == 3100);
    assert(g_haptic_count == 1);
    assert(wc3_direct_touch_next_deadline_ms(&dt) == 0);
}

static void test_hold_drift_presses_at_last_point(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    /* 10 px is inside the 20 px legacy drag threshold: the hold survives and
     * presses at the long-press deadline with the last known point. */
    wc3_direct_touch_down(&dt, 8, 100.0f, 100.0f, 0);
    wc3_direct_touch_move(&dt, 8, 110.0f, 100.0f, 100);
    assert(wc3_direct_touch_tick(&dt, 119) == 0);
    assert(g_event_count == 0);
    assert(wc3_direct_touch_tick(&dt, 120) == 1);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN);
    expect_near("drift press x", g_events[0].x, 0.11f);
    assert(g_haptic_count == 1);
    wc3_direct_touch_up(&dt, 8, 110.0f, 100.0f, 500);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END);
    assert(g_haptic_count == 1);
}

static void test_hold_then_drag_selects(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    wc3_direct_touch_down(&dt, 9, 100.0f, 100.0f, 0);
    assert(wc3_direct_touch_tick(&dt, 120) == 1);
    assert(g_haptic_count == 1);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN);
    expect_near("select begin x", g_events[0].x, 0.10f);
    expect_near("select begin y", g_events[0].y, 0.10f);
    assert(wc3_direct_touch_field_drag_active(&dt) == 1);

    /* The native left button is already down.  Every move after the
     * long-press deadline is a native selection move, even inside the old
     * drift slop. */
    wc3_direct_touch_move(&dt, 9, 115.0f, 100.0f, 500);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE);
    expect_near("select move1 x", g_events[1].x, 0.115f);

    wc3_direct_touch_move(&dt, 9, 160.0f, 120.0f, 520);
    assert(g_event_count == 4);
    assert(g_events[2].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_DRAG_BEGIN);
    expect_near("select drag begin x", g_events[2].x, 0.10f);
    expect_near("select drag begin y", g_events[2].y, 0.10f);
    assert(g_events[3].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE);
    expect_near("select move2 x", g_events[3].x, 0.16f);

    wc3_direct_touch_up(&dt, 9, 180.0f, 140.0f, 600);
    assert(g_event_count == 5);
    assert(g_events[4].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END);
    expect_near("select end x", g_events[4].x, 0.18f);
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
    assert(g_events[0].owner_role == WC3_DT_OWNER_CAMERA);
    assert(g_events[0].gesture_id != 0u);
    /* The DOWN published one field hover before the camera claimed it. */
    assert(g_hover_count == 1);

    /* Once the camera drag owns the pointer, the hold deadline must not arm
     * a second gesture. */
    assert(wc3_direct_touch_tick(&dt, 500) == 0);
    assert(g_haptic_count == 0);

    wc3_direct_touch_move(&dt, 10, 160.0f, 120.0f, 510);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE);
    assert(g_events[1].owner_role == WC3_DT_OWNER_CAMERA);
    assert(g_events[1].gesture_id == g_events[0].gesture_id);

    wc3_direct_touch_up(&dt, 10, 170.0f, 130.0f, 600);
    assert(g_event_count == 3);
    assert(g_events[2].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END);
    assert(wc3_direct_touch_field_drag_active(&dt) == 0);
    assert(g_haptic_count == 0);
}

static void test_late_move_starts_select(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    /* A batch can deliver a MOVE after the long-press deadline but before
     * controls_tick().  The move must arm the hold and start a selection
     * drag, not classify the gesture as a quick camera swipe. */
    wc3_direct_touch_down(&dt, 12, 100.0f, 100.0f, 0);
    wc3_direct_touch_move(&dt, 12, 125.0f, 100.0f, 130);
    assert(g_haptic_count == 1);
    assert(g_haptic_effect == 3);
    assert(g_event_count == 2);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE);
    assert(g_events[0].route == WC3_DIRECT_TOUCH_ROUTE_FIELD);
    expect_near("late select begin x", g_events[0].x, 0.125f);
    expect_near("late select move x", g_events[1].x, 0.125f);

    wc3_direct_touch_move(&dt, 12, 140.0f, 110.0f, 140);
    assert(g_event_count == 3);
    assert(g_events[2].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE);

    /* The release itself crosses the threshold from the hold point, so the
     * recognizer must emit DRAG_BEGIN + MOVE + END even without an earlier
     * MOVE that reported the crossing. */
    wc3_direct_touch_up(&dt, 12, 150.0f, 120.0f, 200);
    assert(g_event_count == 6);
    assert(g_events[3].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_DRAG_BEGIN);
    expect_near("late drag begin x", g_events[3].x, 0.125f);
    expect_near("late drag begin y", g_events[3].y, 0.10f);
    assert(g_events[4].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE);
    expect_near("late drag move x", g_events[4].x, 0.15f);
    assert(g_events[5].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END);
    assert(wc3_direct_touch_tick(&dt, 300) == 0);
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

    /* The pause menu is also a native menu: a MENU contact must stay a raw
     * phase and never become a field camera/select or a long press. */
    wc3_direct_touch_down_route(&dt, 32, 100.0f, 100.0f, 2000,
                                WC3_DIRECT_TOUCH_ROUTE_MENU);
    wc3_direct_touch_move(&dt, 32, 200.0f, 100.0f, 2500);
    wc3_direct_touch_up(&dt, 32, 200.0f, 100.0f, 2600);

    assert(g_event_count == 0);
    assert(g_hover_count == 0);
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

static void test_two_finger_camera_and_action(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    /* Both field contacts stay camera candidates until one of them crosses
     * the drag threshold or reaches the long-press deadline. */
    wc3_direct_touch_down(&dt, 4, 100.0f, 100.0f, 0);
    wc3_direct_touch_down(&dt, 5, 500.0f, 500.0f, 10);
    assert(wc3_direct_touch_active_count(&dt) == 2);
    assert(wc3_direct_touch_contact_role(&dt, 4) == WC3_DT_CONTACT_UNASSIGNED);
    assert(wc3_direct_touch_contact_role(&dt, 5) == WC3_DT_CONTACT_UNASSIGNED);
    assert(wc3_direct_touch_camera_pointer(&dt) == -1);

    wc3_direct_touch_move(&dt, 4, 130.0f, 100.0f, 50);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN);
    assert(g_events[0].owner_role == WC3_DT_OWNER_CAMERA);
    assert(wc3_direct_touch_camera_pointer(&dt) == 4);
    assert(wc3_direct_touch_action_pointer(&dt) == -1);
    /* Both DOWN contacts published a field hover. */
    assert(g_hover_count == 2);

    /* The other finger reaches its long-press deadline and owns the native
     * left button while the camera keeps panning. */
    assert(wc3_direct_touch_tick(&dt, 130) == 1);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN);
    assert(g_events[1].owner_role == WC3_DT_OWNER_ACTION);
    assert(wc3_direct_touch_action_pointer(&dt) == 5);
    /* Two DOWN hovers plus the long-press arm hover. */
    assert(g_hover_count == 3);

    wc3_direct_touch_move(&dt, 4, 160.0f, 100.0f, 200);
    assert(g_event_count == 3);
    assert(g_events[2].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE);
    assert(g_events[2].owner_role == WC3_DT_OWNER_CAMERA);

    wc3_direct_touch_up(&dt, 5, 520.0f, 520.0f, 250);
    assert(g_event_count == 6);
    assert(g_events[3].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_DRAG_BEGIN);
    assert(g_events[4].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE);
    assert(g_events[5].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END);

    wc3_direct_touch_up(&dt, 4, 160.0f, 100.0f, 300);
    assert(g_event_count == 7);
    assert(g_events[6].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END);
    assert(wc3_direct_touch_active_count(&dt) == 0);
}

static void test_action_first_second_finger_camera(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    /* The first finger is already the ACTION cursor after a long press. */
    wc3_direct_touch_down(&dt, 40, 200.0f, 200.0f, 0);
    assert(wc3_direct_touch_tick(&dt, 130) == 1);
    assert(wc3_direct_touch_contact_role(&dt, 40) == WC3_DT_CONTACT_ACTION);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN);

    /* The second finger lands later and still owns a camera claim: a quick
     * swipe in the field must become the camera, not a second cursor. */
    wc3_direct_touch_down(&dt, 41, 700.0f, 300.0f, 200);
    assert(wc3_direct_touch_contact_role(&dt, 41) == WC3_DT_CONTACT_UNASSIGNED);
    wc3_direct_touch_move(&dt, 41, 730.0f, 300.0f, 240);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN);
    assert(g_events[1].owner_role == WC3_DT_OWNER_CAMERA);
    assert(g_events[1].gesture_id != 0u);
    assert(wc3_direct_touch_camera_pointer(&dt) == 41);
    assert(wc3_direct_touch_action_pointer(&dt) == 40);
    /* DOWN 40, the long-press arm, and DOWN 41 each published a hover. */
    assert(g_hover_count == 3);

    /* The cursor keeps its native selection drag while the camera pans. */
    wc3_direct_touch_move(&dt, 40, 220.0f, 210.0f, 260);
    assert(g_event_count == 4);
    assert(g_events[2].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_DRAG_BEGIN);
    assert(g_events[3].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE);
    wc3_direct_touch_move(&dt, 41, 760.0f, 320.0f, 280);
    assert(g_event_count == 5);
    assert(g_events[4].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE);
    assert(g_events[4].owner_role == WC3_DT_OWNER_CAMERA);

    wc3_direct_touch_up(&dt, 41, 760.0f, 320.0f, 300);
    wc3_direct_touch_up(&dt, 40, 220.0f, 210.0f, 320);
    assert(g_event_count == 7);
    assert(g_events[5].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END);
    assert(g_events[6].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END);
    assert(wc3_direct_touch_active_count(&dt) == 0);
}

static void test_camera_first_second_finger_tap(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    /* The camera finger is already panning. */
    wc3_direct_touch_down(&dt, 60, 100.0f, 100.0f, 0);
    wc3_direct_touch_move(&dt, 60, 130.0f, 100.0f, 20);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN);

    /* A second finger stays a full camera candidate while the first camera
     * drag is active.  A stationary release is still an ACTION tap, and a
     * quick swipe can instead join the camera pan (covered separately). */
    wc3_direct_touch_down(&dt, 61, 500.0f, 500.0f, 60);
    assert(wc3_direct_touch_contact_role(&dt, 61) ==
           WC3_DT_CONTACT_UNASSIGNED);
    assert(wc3_direct_touch_camera_pointer(&dt) == 60);
    assert(g_event_count == 1);
    /* DOWN 60 and DOWN 61 each published a field hover. */
    assert(g_hover_count == 2);

    /* Its first tap reaches the semantic path while the camera continues. */
    wc3_direct_touch_up(&dt, 61, 500.0f, 500.0f, 80);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_TAP);
    assert(g_events[1].owner_role == WC3_DT_OWNER_ACTION);
    assert(g_events[1].route == WC3_DIRECT_TOUCH_ROUTE_FIELD);
    /* The two DOWN hovers plus the release hover. */
    assert(g_hover_count == 3);

    wc3_direct_touch_move(&dt, 60, 160.0f, 120.0f, 100);
    assert(g_event_count == 3);
    assert(g_events[2].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE);

    wc3_direct_touch_up(&dt, 60, 160.0f, 120.0f, 120);
    assert(g_event_count == 4);
    assert(g_events[3].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END);
    assert(wc3_direct_touch_active_count(&dt) == 0);
}

static void test_second_finger_joins_camera(void)
{
    Wc3DirectTouch dt = make_recognizer();
    events_clear();

    wc3_direct_touch_down(&dt, 70, 100.0f, 100.0f, 0);
    wc3_direct_touch_move(&dt, 70, 130.0f, 100.0f, 20);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN);
    const uint32_t first_gid = g_events[0].gesture_id;
    assert(first_gid != 0u);

    /* The second finger stays a camera candidate even while the first pan
     * is live.  Its quick swipe claims its own camera gesture. */
    wc3_direct_touch_down(&dt, 71, 500.0f, 500.0f, 30);
    assert(wc3_direct_touch_contact_role(&dt, 71) ==
           WC3_DT_CONTACT_UNASSIGNED);
    wc3_direct_touch_move(&dt, 71, 540.0f, 500.0f, 50);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN);
    assert(g_events[1].owner_role == WC3_DT_OWNER_CAMERA);
    assert(g_events[1].pointer_id == 71);
    assert(g_events[1].gesture_id != 0u);
    assert(g_events[1].gesture_id != first_gid);
    const uint32_t second_gid = g_events[1].gesture_id;
    assert(wc3_direct_touch_contact_role(&dt, 70) ==
           WC3_DT_CONTACT_CAMERA);
    assert(wc3_direct_touch_contact_role(&dt, 71) ==
           WC3_DT_CONTACT_CAMERA);
    assert(wc3_direct_touch_camera_pointer(&dt) == 70);
    /* DOWN 70 and DOWN 71 each published a field hover. */
    assert(g_hover_count == 2);

    /* Both fingers keep their own camera stream. */
    wc3_direct_touch_move(&dt, 70, 160.0f, 120.0f, 70);
    assert(g_event_count == 3);
    assert(g_events[2].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE);
    assert(g_events[2].pointer_id == 70);
    assert(g_events[2].gesture_id == first_gid);
    wc3_direct_touch_move(&dt, 71, 580.0f, 520.0f, 90);
    assert(g_event_count == 4);
    assert(g_events[3].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE);
    assert(g_events[3].pointer_id == 71);
    assert(g_events[3].gesture_id == second_gid);

    /* Releasing the first finger must not cancel the second camera pan. */
    wc3_direct_touch_up(&dt, 70, 160.0f, 120.0f, 110);
    assert(g_event_count == 5);
    assert(g_events[4].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END);
    assert(g_events[4].pointer_id == 70);
    assert(g_events[4].gesture_id == first_gid);
    assert(wc3_direct_touch_active_count(&dt) == 1);
    assert(wc3_direct_touch_contact_role(&dt, 71) ==
           WC3_DT_CONTACT_CAMERA);
    assert(wc3_direct_touch_camera_pointer(&dt) == 71);

    wc3_direct_touch_move(&dt, 71, 620.0f, 540.0f, 130);
    assert(g_event_count == 6);
    assert(g_events[5].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE);
    assert(g_events[5].pointer_id == 71);
    assert(g_events[5].gesture_id == second_gid);

    wc3_direct_touch_up(&dt, 71, 620.0f, 540.0f, 150);
    assert(g_event_count == 7);
    assert(g_events[6].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END);
    assert(g_events[6].pointer_id == 71);
    assert(g_events[6].gesture_id == second_gid);
    assert(wc3_direct_touch_active_count(&dt) == 0);
    assert(wc3_direct_touch_camera_pointer(&dt) == -1);
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

static void test_unit_type_tap_order(void)
{
    /* Lock the big-endian rawcode dwords, not just the helper's internal
     * consistency: these are the dwords native GetUnitTypeId returns.
     * Device log 2026-11-27: egol arrived as 0x65676F6C. */
    assert(wc3_dt_unit_type_tap_order(0x65676F6Cu));
    assert(WC3_DT_UNIT_TYPE_ENTANGLED_GOLD_MINE == 0x65676F6Cu);
    assert(WC3_DT_UNIT_TYPE_HAUNTED_GOLD_MINE == 0x75676F6Cu);
    assert(WC3_DT_UNIT_TYPE_HUMAN_TRANSPORT == 0x68626F74u);
    assert(WC3_DT_UNIT_TYPE_ORC_TRANSPORT == 0x6F626F74u);
    assert(WC3_DT_UNIT_TYPE_UNDEAD_TRANSPORT == 0x75626F74u);
    assert(WC3_DT_UNIT_TYPE_NEUTRAL_TRANSPORT == 0x6E626F74u);
    assert(WC3_DT_UNIT_TYPE_NIGHT_ELF_TRANSPORT == 0x65747273u);
    assert(WC3_DT_UNIT_TYPE_UNDEAD_AIR_BARGE == 0x75617262u);
    assert(WC3_DT_UNIT_TYPE_GOBLIN_ZEPPELIN == 0x6E7A6570u);

    /* Base-game gold mines and cargo transports are the only controllable
     * targets that take the smart-order leg. */
    assert(wc3_dt_unit_type_tap_order(
        WC3_DT_UNIT_TYPE_ENTANGLED_GOLD_MINE));
    assert(wc3_dt_unit_type_tap_order(
        WC3_DT_UNIT_TYPE_HAUNTED_GOLD_MINE));
    assert(wc3_dt_unit_type_tap_order(WC3_DT_UNIT_TYPE_HUMAN_TRANSPORT));
    assert(wc3_dt_unit_type_tap_order(WC3_DT_UNIT_TYPE_ORC_TRANSPORT));
    assert(wc3_dt_unit_type_tap_order(WC3_DT_UNIT_TYPE_UNDEAD_TRANSPORT));
    assert(wc3_dt_unit_type_tap_order(WC3_DT_UNIT_TYPE_NEUTRAL_TRANSPORT));
    assert(wc3_dt_unit_type_tap_order(
        WC3_DT_UNIT_TYPE_NIGHT_ELF_TRANSPORT));
    assert(wc3_dt_unit_type_tap_order(WC3_DT_UNIT_TYPE_UNDEAD_AIR_BARGE));
    assert(wc3_dt_unit_type_tap_order(WC3_DT_UNIT_TYPE_GOBLIN_ZEPPELIN));

    /* The neutral gold mine is not controllable; its normal leg already
     * orders.  Ordinary units and a missing type stay on select. */
    assert(!wc3_dt_unit_type_tap_order(WC3_DT_RAWCODE('n', 'g', 'o', 'l')));
    assert(!wc3_dt_unit_type_tap_order(WC3_DT_RAWCODE('h', 'f', 'o', 'o')));
    assert(!wc3_dt_unit_type_tap_order(0));
}

static void test_tap_unit_intent(void)
{
    /* A controllable target is selected even while another controlled
     * selection is live.  An uncontrollable target is a smart order only
     * when the live selection can receive one. */
    assert(wc3_dt_tap_unit_intent(0, 0, 0) == WC3_DT_INTENT_SELECT_UNIT);
    assert(wc3_dt_tap_unit_intent(0, 1, 0) == WC3_DT_INTENT_SELECT_UNIT);
    assert(wc3_dt_tap_unit_intent(1, 0, 0) == WC3_DT_INTENT_ORDER_UNIT);
    assert(wc3_dt_tap_unit_intent(1, 1, 0) == WC3_DT_INTENT_SELECT_UNIT);

    /* The exception classes order a controlled selection, but a tap with no
     * order source still selects the target. */
    assert(wc3_dt_tap_unit_intent(1, 1, 1) == WC3_DT_INTENT_ORDER_UNIT);
    assert(wc3_dt_tap_unit_intent(0, 1, 1) == WC3_DT_INTENT_SELECT_UNIT);
    assert(wc3_dt_tap_unit_intent(1, 0, 1) == WC3_DT_INTENT_ORDER_UNIT);
    assert(wc3_dt_tap_unit_intent(0, 0, 1) == WC3_DT_INTENT_SELECT_UNIT);
}

static void test_unit_gesture_policy(void)
{
    Wc3DtUnitGesturePolicy policy;

    /* Without a controlled selection both gestures are selection/clear;
     * the executor does not defer the native press in this case. */
    policy = wc3_dt_unit_gesture_policy(0, 0, 0);
    assert(policy.tap == WC3_DT_INTENT_SELECT_UNIT);
    assert(policy.long_tap == WC3_DT_INTENT_SELECT_UNIT);
    policy = wc3_dt_unit_gesture_policy(0, 1, 0);
    assert(policy.tap == WC3_DT_INTENT_SELECT_UNIT);
    assert(policy.long_tap == WC3_DT_INTENT_SELECT_UNIT);
    policy = wc3_dt_unit_gesture_policy(0, 1, 1);
    assert(policy.tap == WC3_DT_INTENT_SELECT_UNIT);
    assert(policy.long_tap == WC3_DT_INTENT_SELECT_UNIT);

    /* Direct pair: an ordinary controllable unit selects on tap and orders
     * on a stationary long tap. */
    policy = wc3_dt_unit_gesture_policy(1, 1, 0);
    assert(policy.tap == WC3_DT_INTENT_SELECT_UNIT);
    assert(policy.long_tap == WC3_DT_INTENT_ORDER_UNIT);

    /* Swapped pair: the gold-mine/transport exception orders on tap and
     * selects on a stationary long tap. */
    policy = wc3_dt_unit_gesture_policy(1, 1, 1);
    assert(policy.tap == WC3_DT_INTENT_ORDER_UNIT);
    assert(policy.long_tap == WC3_DT_INTENT_SELECT_UNIT);

    /* Uncontrollable target: tap orders, long tap selects.  The exception
     * flag is irrelevant here.  This keeps both actions in the pair. */
    policy = wc3_dt_unit_gesture_policy(1, 0, 0);
    assert(policy.tap == WC3_DT_INTENT_ORDER_UNIT);
    assert(policy.long_tap == WC3_DT_INTENT_SELECT_UNIT);
    policy = wc3_dt_unit_gesture_policy(1, 0, 1);
    assert(policy.tap == WC3_DT_INTENT_ORDER_UNIT);
    assert(policy.long_tap == WC3_DT_INTENT_SELECT_UNIT);

    /* Architecture invariant: with a controlled selection every unit target
     * has exactly one select and one order gesture.  A future condition may
     * choose the direct or swapped pair, but it may not collapse both legs
     * into the same intent. */
    for (int controllable = 0; controllable <= 1; ++controllable) {
        for (int tap_order = 0; tap_order <= 1; ++tap_order) {
            policy = wc3_dt_unit_gesture_policy(1, controllable, tap_order);
            assert((policy.tap == WC3_DT_INTENT_SELECT_UNIT &&
                    policy.long_tap == WC3_DT_INTENT_ORDER_UNIT) ||
                   (policy.tap == WC3_DT_INTENT_ORDER_UNIT &&
                    policy.long_tap == WC3_DT_INTENT_SELECT_UNIT));
        }
    }
}

static void test_long_tap_unit_intent(void)
{
    /* Ordinary controllable unit: tap selects, long tap orders. */
    assert(wc3_dt_long_tap_unit_intent(1, 1, 0) ==
           WC3_DT_INTENT_ORDER_UNIT);

    /* Controllable exception: tap orders, long tap selects. */
    assert(wc3_dt_long_tap_unit_intent(1, 1, 1) ==
           WC3_DT_INTENT_SELECT_UNIT);

    /* Uncontrollable target: tap orders, long tap selects.  This is the
     * second leg of the pair, not a smart order. */
    assert(wc3_dt_long_tap_unit_intent(1, 0, 0) ==
           WC3_DT_INTENT_SELECT_UNIT);
    assert(wc3_dt_long_tap_unit_intent(1, 0, 1) ==
           WC3_DT_INTENT_SELECT_UNIT);

    /* Without a controlled selection the executor never defers; the native
     * press selects.  The helper stays consistent for that case. */
    assert(wc3_dt_long_tap_unit_intent(0, 1, 0) ==
           WC3_DT_INTENT_SELECT_UNIT);
    assert(wc3_dt_long_tap_unit_intent(0, 1, 1) ==
           WC3_DT_INTENT_SELECT_UNIT);
    assert(wc3_dt_long_tap_unit_intent(0, 0, 0) ==
           WC3_DT_INTENT_SELECT_UNIT);
    assert(wc3_dt_long_tap_unit_intent(0, 0, 1) ==
           WC3_DT_INTENT_SELECT_UNIT);
}

static void test_selection_can_order(void)
{
    /* Only playable slots 0..11 can be an order source.  Slots 12..15 are
     * neutral: selectable, but not controllable by the local player.  The
     * native predicate alone accepted a selected neutral Goblin Ammo Dump
     * (ngad, owner 15), so the owner gate is required. */
    assert(wc3_dt_selection_can_order(0u, 1));
    assert(wc3_dt_selection_can_order(11u, 1));
    assert(!wc3_dt_selection_can_order(12u, 1));
    assert(!wc3_dt_selection_can_order(15u, 1));
    assert(!wc3_dt_selection_can_order(0u, 0));

    /* A neutral target remains selectable; only the selection gate changes.
     * This locks the behavior that tapping egol with ngad selected selects
     * egol instead of issuing an order. */
    assert(wc3_dt_tap_unit_intent(0, 1, 1) == WC3_DT_INTENT_SELECT_UNIT);
}

static void test_tap_intent_resolution(void)
{
    /* A miss has no action. */
    assert(wc3_dt_tap_intent(WC3_DT_PICK_NONE, 0, 0, 0) ==
           WC3_DT_INTENT_NONE);

    /* A validated non-unit sprite remains a no-op without a controlled
     * selection; with one it becomes a right-click analogue so the native
     * handler can resolve the decoration or the ground under it. */
    assert(wc3_dt_tap_intent(WC3_DT_PICK_OBJECT, 1, 0, 0) ==
           WC3_DT_INTENT_NONE);
    assert(wc3_dt_tap_intent(WC3_DT_PICK_OBJECT, 1, 0, 1) ==
           WC3_DT_INTENT_ORDER_UNIT);

    /* An object that no longer validates is a terrain click.  Terrain
     * orders only when the selection can receive an order; otherwise it
     * clears.  A non-empty but uncontrollable selection is not an order
     * source. */
    assert(wc3_dt_tap_intent(WC3_DT_PICK_OBJECT, 0, 0, 0) ==
           WC3_DT_INTENT_CLEAR_SELECTION);
    assert(wc3_dt_tap_intent(WC3_DT_PICK_OBJECT, 0, 0, 1) ==
           WC3_DT_INTENT_ORDER_UNIT);
    assert(wc3_dt_tap_intent(WC3_DT_PICK_TERRAIN, 0, 0, 0) ==
           WC3_DT_INTENT_CLEAR_SELECTION);
    assert(wc3_dt_tap_intent(WC3_DT_PICK_TERRAIN, 0, 0, 1) ==
           WC3_DT_INTENT_ORDER_GROUND);
}

static void test_field_tap_intent(void)
{
    /* Unit leg: target controllability decides before selection
     * controllability.  T=1 selects with or without a controlled selection;
     * T=0 orders only when the live selection can receive one.  The final
     * argument marks the gold-mine/transport exception classes. */
    assert(wc3_dt_field_tap_intent(WC3_DT_PICK_OBJECT, 1, 1, 0, 1, 0) ==
           WC3_DT_INTENT_SELECT_UNIT);
    assert(wc3_dt_field_tap_intent(WC3_DT_PICK_OBJECT, 1, 1, 1, 1, 0) ==
           WC3_DT_INTENT_SELECT_UNIT);
    assert(wc3_dt_field_tap_intent(WC3_DT_PICK_OBJECT, 1, 1, 1, 0, 0) ==
           WC3_DT_INTENT_ORDER_UNIT);
    assert(wc3_dt_field_tap_intent(WC3_DT_PICK_OBJECT, 1, 1, 0, 0, 0) ==
           WC3_DT_INTENT_SELECT_UNIT);

    /* A controllable exception target orders only while the live selection
     * can receive one; without a selection it is selected normally. */
    assert(wc3_dt_field_tap_intent(WC3_DT_PICK_OBJECT, 1, 1, 1, 1, 1) ==
           WC3_DT_INTENT_ORDER_UNIT);
    assert(wc3_dt_field_tap_intent(WC3_DT_PICK_OBJECT, 1, 1, 0, 1, 1) ==
           WC3_DT_INTENT_SELECT_UNIT);
    assert(wc3_dt_field_tap_intent(WC3_DT_PICK_OBJECT, 1, 1, 1, 0, 1) ==
           WC3_DT_INTENT_ORDER_UNIT);
    assert(wc3_dt_field_tap_intent(WC3_DT_PICK_OBJECT, 1, 1, 0, 0, 1) ==
           WC3_DT_INTENT_SELECT_UNIT);

    /* Non-unit objects: a controlled selection orders, otherwise a
     * validated sprite is a no-op and an invalid object clears. */
    assert(wc3_dt_field_tap_intent(WC3_DT_PICK_OBJECT, 1, 0, 1, 0, 0) ==
           WC3_DT_INTENT_ORDER_UNIT);
    assert(wc3_dt_field_tap_intent(WC3_DT_PICK_OBJECT, 1, 0, 0, 0, 0) ==
           WC3_DT_INTENT_NONE);
    assert(wc3_dt_field_tap_intent(WC3_DT_PICK_OBJECT, 0, 0, 1, 0, 0) ==
           WC3_DT_INTENT_ORDER_UNIT);
    assert(wc3_dt_field_tap_intent(WC3_DT_PICK_OBJECT, 0, 0, 0, 0, 0) ==
           WC3_DT_INTENT_CLEAR_SELECTION);

    /* Terrain and miss. */
    assert(wc3_dt_field_tap_intent(WC3_DT_PICK_TERRAIN, 0, 0, 1, 0, 0) ==
           WC3_DT_INTENT_ORDER_GROUND);
    assert(wc3_dt_field_tap_intent(WC3_DT_PICK_TERRAIN, 0, 0, 0, 0, 0) ==
           WC3_DT_INTENT_CLEAR_SELECTION);
    assert(wc3_dt_field_tap_intent(WC3_DT_PICK_NONE, 0, 0, 0, 0, 0) ==
           WC3_DT_INTENT_NONE);
    assert(wc3_dt_field_tap_intent(WC3_DT_PICK_NONE, 0, 0, 1, 0, 0) ==
           WC3_DT_INTENT_NONE);
}

static void test_configured_long_press_and_threshold(void)
{
    Wc3DirectTouch dt;
    events_clear();

    /* The deadline and the drag threshold are configuration, not constants:
     * 50 ms and 11 px here, 120 ms and 20 px in make_recognizer(). */
    wc3_direct_touch_init(&dt, 1000.0f, 1000.0f, 11.0f, 50, sink, NULL);
    wc3_direct_touch_set_haptic(&dt, haptic, NULL, 3);
    wc3_direct_touch_down(&dt, 80, 100.0f, 100.0f, 0);
    assert(wc3_direct_touch_next_deadline_ms(&dt) == 50);
    assert(wc3_direct_touch_tick(&dt, 49) == 0);
    assert(g_event_count == 0);
    assert(wc3_direct_touch_tick(&dt, 50) == 1);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN);

    /* 10 px is inside the configured 11 px threshold: still a held click. */
    wc3_direct_touch_move(&dt, 80, 110.0f, 100.0f, 60);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE);

    /* 12 px from the hold point starts the selection rectangle. */
    wc3_direct_touch_move(&dt, 80, 112.0f, 100.0f, 70);
    assert(g_event_count == 4);
    assert(g_events[2].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_DRAG_BEGIN);
    expect_near("configured drag begin x", g_events[2].x, 0.10f);
    assert(g_events[3].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE);
    wc3_direct_touch_up(&dt, 80, 112.0f, 100.0f, 80);
    assert(g_event_count == 5);
    assert(g_events[4].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END);

    /* A 200 ms deadline must not arm at 120 ms. */
    events_clear();
    wc3_direct_touch_init(&dt, 1000.0f, 1000.0f, 11.0f, 200, sink, NULL);
    wc3_direct_touch_set_haptic(&dt, haptic, NULL, 3);
    wc3_direct_touch_down(&dt, 81, 300.0f, 300.0f, 1000);
    assert(wc3_direct_touch_next_deadline_ms(&dt) == 1200);
    assert(wc3_direct_touch_tick(&dt, 1120) == 0);
    assert(g_event_count == 0);
    assert(wc3_direct_touch_tick(&dt, 1200) == 1);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN);
    wc3_direct_touch_up(&dt, 81, 300.0f, 300.0f, 1210);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END);
}

static void test_smart_order_candidate(void)
{
    /* A long tap becomes a smart order only when every gate holds: a
     * controlled selection, the field rectangle, no HUD overlap, no live
     * target/placement mode, not paused and no other native button owner.
     * The target itself is resolved on release, so no pick is part of the
     * gate. */
    assert(wc3_dt_smart_order_candidate(1, 1, 0, 0, 0, 0));
    assert(!wc3_dt_smart_order_candidate(0, 1, 0, 0, 0, 0));
    assert(!wc3_dt_smart_order_candidate(1, 0, 0, 0, 0, 0));
    assert(!wc3_dt_smart_order_candidate(1, 1, 1, 0, 0, 0));
    assert(!wc3_dt_smart_order_candidate(1, 1, 0, 1, 0, 0));
    assert(!wc3_dt_smart_order_candidate(1, 1, 0, 0, 1, 0));
    assert(!wc3_dt_smart_order_candidate(1, 1, 0, 0, 0, 1));
}

static void test_order_route(void)
{
    /* A controllable unit comes back from the right pick as a CUnit, and
     * 0x6F21B730 rejects it.  It is still a unit target: the native right
     * click, never A + left click on a friendly unit. */
    assert(wc3_dt_order_route(WC3_DT_PICK_OBJECT, 0, 1) ==
           WC3_DT_ORDER_ROUTE_NATIVE);
    assert(wc3_dt_order_route(WC3_DT_PICK_OBJECT, 0, 0) ==
           WC3_DT_ORDER_ROUTE_NATIVE);
    /* A hostile/neutral unit or a decoration is a validated ghost. */
    assert(wc3_dt_order_route(WC3_DT_PICK_OBJECT, 1, 1) ==
           WC3_DT_ORDER_ROUTE_UNIT);
    assert(wc3_dt_order_route(WC3_DT_PICK_OBJECT, 1, 0) ==
           WC3_DT_ORDER_ROUTE_UNIT);
    /* Terrain: tap is a smart move, long tap is attack-move. */
    assert(wc3_dt_order_route(WC3_DT_PICK_TERRAIN, 0, 0) ==
           WC3_DT_ORDER_ROUTE_GROUND);
    assert(wc3_dt_order_route(WC3_DT_PICK_TERRAIN, 0, 1) ==
           WC3_DT_ORDER_ROUTE_ATTACK_MOVE);
    /* A failed pick: tap falls back to native, long tap keeps attack-move. */
    assert(wc3_dt_order_route(WC3_DT_PICK_NONE, 0, 0) ==
           WC3_DT_ORDER_ROUTE_NATIVE);
    assert(wc3_dt_order_route(WC3_DT_PICK_NONE, 0, 1) ==
           WC3_DT_ORDER_ROUTE_ATTACK_MOVE);
}

static void test_point_in_rect(void)
{
    /* Published HUD snapshots are half-open. */
    assert(wc3_dt_point_in_rect(0.5f, 0.5f, 0.1f, 0.1f, 0.9f, 0.9f));
    assert(wc3_dt_point_in_rect(0.1f, 0.1f, 0.1f, 0.1f, 0.9f, 0.9f));
    assert(!wc3_dt_point_in_rect(0.9f, 0.5f, 0.1f, 0.1f, 0.9f, 0.9f));
    assert(!wc3_dt_point_in_rect(0.5f, 0.9f, 0.1f, 0.1f, 0.9f, 0.9f));
    assert(!wc3_dt_point_in_rect(0.5f, 0.5f, 0.9f, 0.1f, 0.1f, 0.9f));
    assert(!wc3_dt_point_in_rect(0.5f, 0.5f, 0.1f, 0.9f, 0.9f, 0.1f));

    /* Button frames are half-open too: left/top belong to the cell, while
     * right/bottom belong to the next one. */
    assert(wc3_dt_button_rect_hit(0.1f, 0.5f, 0.1f, 0.1f, 0.9f, 0.9f));
    assert(wc3_dt_button_rect_hit(0.5f, 0.1f, 0.1f, 0.1f, 0.9f, 0.9f));
    assert(!wc3_dt_button_rect_hit(0.9f, 0.5f, 0.1f, 0.1f, 0.9f, 0.9f));
    assert(!wc3_dt_button_rect_hit(0.5f, 0.9f, 0.1f, 0.1f, 0.9f, 0.9f));
    assert(!wc3_dt_button_rect_hit(0.5f, 0.5f, 0.9f, 0.1f, 0.1f, 0.9f));
    assert(!wc3_dt_button_rect_hit(0.5f, 0.5f, 0.1f, 0.9f, 0.9f, 0.1f));
    assert(!wc3_dt_button_rect_hit(-0.1f, 0.5f, 0.1f, 0.1f, 0.9f, 0.9f));
    assert(!wc3_dt_button_rect_hit(0.5f, -0.1f, 0.1f, 0.1f, 0.9f, 0.9f));

    /* The field rectangle is inclusive: all four boundary points belong to
     * it, while a point outside any edge does not. */
    assert(wc3_dt_rect_contains_inclusive(0.1f, 0.5f, 0.1f, 0.1f, 0.9f, 0.9f));
    assert(wc3_dt_rect_contains_inclusive(0.5f, 0.1f, 0.1f, 0.1f, 0.9f, 0.9f));
    assert(wc3_dt_rect_contains_inclusive(0.9f, 0.5f, 0.1f, 0.1f, 0.9f, 0.9f));
    assert(wc3_dt_rect_contains_inclusive(0.5f, 0.9f, 0.1f, 0.1f, 0.9f, 0.9f));
    assert(!wc3_dt_rect_contains_inclusive(0.09f, 0.5f, 0.1f, 0.1f, 0.9f, 0.9f));
    assert(!wc3_dt_rect_contains_inclusive(0.5f, 0.91f, 0.1f, 0.1f, 0.9f, 0.9f));
}

static void test_rect_union(void)
{
    Wc3DtRect dst = {0.0f, 0.0f, 0.0f, 0.0f};
    const Wc3DtRect first = {0.1f, 0.2f, 0.3f, 0.4f};
    wc3_dt_rect_union(&dst, &first);
    assert(dst.left == 0.1f && dst.top == 0.2f);
    assert(dst.right == 0.3f && dst.bottom == 0.4f);

    const Wc3DtRect second = {0.0f, 0.25f, 0.5f, 0.3f};
    wc3_dt_rect_union(&dst, &second);
    assert(dst.left == 0.0f && dst.top == 0.2f);
    assert(dst.right == 0.5f && dst.bottom == 0.4f);

    /* Touching edges extend the union; no gap is introduced. */
    const Wc3DtRect touching = {0.5f, 0.4f, 0.6f, 0.5f};
    wc3_dt_rect_union(&dst, &touching);
    assert(dst.left == 0.0f && dst.top == 0.2f);
    assert(dst.right == 0.6f && dst.bottom == 0.5f);

    /* An empty or inverted source is ignored. */
    const Wc3DtRect empty = {0.7f, 0.7f, 0.7f, 0.8f};
    wc3_dt_rect_union(&dst, &empty);
    assert(dst.right == 0.6f && dst.bottom == 0.5f);

    const Wc3DtRect inverted = {0.8f, 0.8f, 0.7f, 0.9f};
    wc3_dt_rect_union(&dst, &inverted);
    assert(dst.right == 0.6f && dst.bottom == 0.5f);

    /* Null is a no-op. */
    wc3_dt_rect_union(NULL, &first);
    wc3_dt_rect_union(&dst, NULL);
    assert(dst.right == 0.6f && dst.bottom == 0.5f);

    /* An empty destination adopts the first valid source. */
    Wc3DtRect empty_dst = {0.0f, 0.0f, 0.0f, 0.0f};
    wc3_dt_rect_union(&empty_dst, &first);
    assert(empty_dst.left == 0.1f && empty_dst.top == 0.2f);
    assert(empty_dst.right == 0.3f && empty_dst.bottom == 0.4f);
}

static void test_native_left_busy(void)
{
    /* FULL: any owner blocks; a queued release with no hardware press does
     * not. */
    assert(wc3_dt_native_left_busy(WC3_DT_NATIVE_BUSY_FULL, 1, 0, 0, 0));
    assert(wc3_dt_native_left_busy(WC3_DT_NATIVE_BUSY_FULL, 0, 1, 0, 0));
    assert(wc3_dt_native_left_busy(WC3_DT_NATIVE_BUSY_FULL, 0, 0, 0, 1));
    assert(!wc3_dt_native_left_busy(WC3_DT_NATIVE_BUSY_FULL, 0, 0, 1, 1));
    assert(!wc3_dt_native_left_busy(WC3_DT_NATIVE_BUSY_FULL, 0, 0, 0, 0));

    /* DOWN_OR_UP ignores the hardware state and the queued-release flag. */
    assert(wc3_dt_native_left_busy(WC3_DT_NATIVE_BUSY_DOWN_OR_UP, 0, 1, 1, 0));
    assert(!wc3_dt_native_left_busy(WC3_DT_NATIVE_BUSY_DOWN_OR_UP, 0, 0, 0, 1));

    /* DOWN_ONLY ignores up_pending and the hardware state. */
    assert(wc3_dt_native_left_busy(WC3_DT_NATIVE_BUSY_DOWN_ONLY, 1, 0, 0, 0));
    assert(!wc3_dt_native_left_busy(WC3_DT_NATIVE_BUSY_DOWN_ONLY, 0, 1, 0, 0));

    /* HARDWARE: down or a hardware press, but no up_pending. */
    assert(wc3_dt_native_left_busy(WC3_DT_NATIVE_BUSY_HARDWARE, 1, 0, 0, 0));
    assert(wc3_dt_native_left_busy(WC3_DT_NATIVE_BUSY_HARDWARE, 0, 0, 0, 1));
    assert(!wc3_dt_native_left_busy(WC3_DT_NATIVE_BUSY_HARDWARE, 0, 1, 0, 0));
    assert(!wc3_dt_native_left_busy(WC3_DT_NATIVE_BUSY_HARDWARE, 0, 0, 1, 1));
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

static void test_physical_route(void)
{
    /* A live battlefield is FIELD regardless of the active input mode.
     * Target/placement and transient panel modes are resolved on release,
     * not by handing the DOWN to raw native phases. */
    assert(wc3_dt_physical_route(1, 1, 1) == WC3_DIRECT_TOUCH_ROUTE_FIELD);
    assert(wc3_dt_physical_route(1, 1, 0) == WC3_DIRECT_TOUCH_ROUTE_NATIVE);
    assert(wc3_dt_physical_route(1, 0, 1) == WC3_DIRECT_TOUCH_ROUTE_NATIVE);
    assert(wc3_dt_physical_route(1, 0, 0) == WC3_DIRECT_TOUCH_ROUTE_NATIVE);
    assert(wc3_dt_physical_route(0, 1, 1) == WC3_DIRECT_TOUCH_ROUTE_LEGACY);
}

static void test_menu_event_policy(void)
{
    /* Pause menu = main menu: key signals and raw native pointer phases are
     * the whole gesture set. */
    assert(wc3_dt_menu_event_allowed(WC3_DIRECT_TOUCH_KEY_SIGNAL,
                                     WC3_DIRECT_TOUCH_ROUTE_NATIVE));
    assert(wc3_dt_menu_event_allowed(WC3_DIRECT_TOUCH_KEY_SIGNAL,
                                     WC3_DIRECT_TOUCH_ROUTE_MENU));
    assert(wc3_dt_menu_event_allowed(WC3_DIRECT_TOUCH_POINTER_DOWN,
                                     WC3_DIRECT_TOUCH_ROUTE_NATIVE));
    assert(wc3_dt_menu_event_allowed(WC3_DIRECT_TOUCH_POINTER_MOVE,
                                     WC3_DIRECT_TOUCH_ROUTE_NATIVE));
    assert(wc3_dt_menu_event_allowed(WC3_DIRECT_TOUCH_POINTER_UP,
                                     WC3_DIRECT_TOUCH_ROUTE_NATIVE));
    /* Cancel is the one minimap-routed phase that must reach the native
     * button: it releases a press that was live when the menu opened. */
    assert(wc3_dt_menu_event_allowed(WC3_DIRECT_TOUCH_POINTER_CANCEL,
                                     WC3_DIRECT_TOUCH_ROUTE_MINIMAP));

    /* A raw minimap drag is a game gesture, not a menu click. */
    assert(!wc3_dt_menu_event_allowed(WC3_DIRECT_TOUCH_POINTER_DOWN,
                                      WC3_DIRECT_TOUCH_ROUTE_MINIMAP));
    assert(!wc3_dt_menu_event_allowed(WC3_DIRECT_TOUCH_POINTER_MOVE,
                                      WC3_DIRECT_TOUCH_ROUTE_MINIMAP));
    assert(!wc3_dt_menu_event_allowed(WC3_DIRECT_TOUCH_POINTER_UP,
                                      WC3_DIRECT_TOUCH_ROUTE_MINIMAP));

    /* Control-group chords and every semantic world/portrait gesture are
     * game input and are dropped while the menu is open. */
    assert(!wc3_dt_menu_event_allowed(WC3_DIRECT_TOUCH_GROUP_SIGNAL, 0));
    assert(!wc3_dt_menu_event_allowed(WC3_DIRECT_TOUCH_TAP,
                                      WC3_DIRECT_TOUCH_ROUTE_FIELD));
    assert(!wc3_dt_menu_event_allowed(WC3_DIRECT_TOUCH_LONG_PRESS,
                                      WC3_DIRECT_TOUCH_ROUTE_FIELD));
    assert(!wc3_dt_menu_event_allowed(WC3_DIRECT_TOUCH_FIELD_HOVER,
                                      WC3_DIRECT_TOUCH_ROUTE_FIELD));
    assert(!wc3_dt_menu_event_allowed(WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN,
                                      WC3_DIRECT_TOUCH_ROUTE_FIELD));
    assert(!wc3_dt_menu_event_allowed(WC3_DIRECT_TOUCH_FIELD_SELECT_END,
                                      WC3_DIRECT_TOUCH_ROUTE_FIELD));
    assert(!wc3_dt_menu_event_allowed(WC3_DIRECT_TOUCH_FOLLOW_START,
                                      WC3_DIRECT_TOUCH_ROUTE_PORTRAIT));
    assert(!wc3_dt_menu_event_allowed(WC3_DIRECT_TOUCH_HOLD_RELEASE,
                                      WC3_DIRECT_TOUCH_ROUTE_PORTRAIT));

    /* Pause menu pointer phases never enter the field tap deferral.  The
     * battlefield rectangle is still live underneath, but the pause menu
     * uses the same direct native gateway as the main menu; holding the
     * DOWN for the field resolver would drop the release. */
    assert(!wc3_dt_native_deferral_allowed(1));
    assert(wc3_dt_native_deferral_allowed(0));

    /* The live UI/world under CEscMenu is not a context switch.  Treating it
     * as one cancels the native DOWN on every pause event and leaves only
     * the hover move. */
    assert(!wc3_dt_context_switch_relevant(1));
    assert(wc3_dt_context_switch_relevant(0));
}

static void test_field_release_resolution(void)
{
    /* FIELD is the only semantic outcome. */
    assert(wc3_dt_field_release(1, 0, 1) == WC3_DT_FIELD_RELEASE_SEMANTIC);
    assert(wc3_dt_field_release(1, 1, 1) == WC3_DT_FIELD_RELEASE_SEMANTIC);
    assert(wc3_dt_field_release(1, 0, 0) == WC3_DT_FIELD_RELEASE_SEMANTIC);
    /* Map-load opens the separate replay path. */
    assert(wc3_dt_field_release(0, 1, 1) == WC3_DT_FIELD_RELEASE_DEFER);
    /* Target/transient/menu has a native recipient; never a silent NONE. */
    assert(wc3_dt_field_release(0, 0, 1) == WC3_DT_FIELD_RELEASE_NATIVE);
    /* No UI is an explicit drop. */
    assert(wc3_dt_field_release(0, 0, 0) == WC3_DT_FIELD_RELEASE_DROP);
    assert(wc3_dt_field_release(0, 1, 0) == WC3_DT_FIELD_RELEASE_DEFER);
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
    wc3_direct_touch_init(&dt, 100.0f, 200.0f, 8.0f, 120,
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

    /* A minimap-owned raw phase carries MINIMAP so the service can drop a
     * stale minimap drag when a menu opens.  Any other requested route is
     * clamped to NATIVE: a raw phase can never claim to be semantic. */
    wc3_direct_touch_emit_pointer_route(&dt, WC3_DIRECT_TOUCH_POINTER_CANCEL,
                                        8, 70.0f, 130.0f, 14,
                                        WC3_DIRECT_TOUCH_ROUTE_MINIMAP);
    wc3_direct_touch_emit_pointer_route(&dt, WC3_DIRECT_TOUCH_POINTER_DOWN,
                                        9, 80.0f, 140.0f, 15,
                                        WC3_DIRECT_TOUCH_ROUTE_MENU);
    assert(g_event_count == 5);
    assert(g_events[3].kind == WC3_DIRECT_TOUCH_POINTER_CANCEL);
    assert(g_events[3].route == WC3_DIRECT_TOUCH_ROUTE_MINIMAP);
    assert(g_events[4].kind == WC3_DIRECT_TOUCH_POINTER_DOWN);
    assert(g_events[4].route == WC3_DIRECT_TOUCH_ROUTE_NATIVE);
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
    /* The second finger is an action cursor, even while a portrait follow is
     * held: a quick tap is a real TAP, not a swallowed multi-touch contact. */
    wc3_direct_touch_up(&dt, 25, 200.0f, 200.0f, 5110);
    assert(g_event_count == 3);
    assert(g_events[2].kind == WC3_DIRECT_TOUCH_TAP);
    assert(g_events[2].owner_role == WC3_DT_OWNER_ACTION);
}

static void test_beacon_release_resolution(void)
{
    /* Release on the button arms the next minimap tap, even after jitter.
     * A release over the minimap places there; over the portrait places on
     * the displayed unit; over the field places at the world point.  Any
     * other drag cancels.  Portrait wins over field because it overlaps it. */
    assert(wc3_dt_beacon_release(1, 0, 0, 0, 0) == WC3_DT_BEACON_ARM);
    assert(wc3_dt_beacon_release(1, 0, 0, 0, 1) == WC3_DT_BEACON_ARM);
    assert(wc3_dt_beacon_release(0, 1, 0, 0, 0) == WC3_DT_BEACON_MINIMAP);
    assert(wc3_dt_beacon_release(0, 1, 1, 1, 1) == WC3_DT_BEACON_MINIMAP);
    assert(wc3_dt_beacon_release(0, 0, 1, 0, 0) == WC3_DT_BEACON_PORTRAIT);
    assert(wc3_dt_beacon_release(0, 0, 1, 1, 1) == WC3_DT_BEACON_PORTRAIT);
    assert(wc3_dt_beacon_release(0, 0, 0, 1, 1) == WC3_DT_BEACON_FIELD);
    assert(wc3_dt_beacon_release(0, 0, 0, 0, 1) == WC3_DT_BEACON_NONE);
    assert(wc3_dt_beacon_release(0, 0, 0, 0, 0) == WC3_DT_BEACON_ARM);
    /* A HUD button must win over the field if the rectangles overlap. */
    assert(wc3_dt_beacon_release(1, 1, 1, 1, 1) == WC3_DT_BEACON_ARM);

    /* Hold on the button, drag onto the minimap/portrait/world, release.
     * Movement comes from the endpoints, so a deferred/coalesced MOVE cannot
     * turn the drag into a button tap or cancel it. */
    assert(wc3_dt_beacon_release_from_points(0, 1, 0, 0,
                                             0.160f, 0.900f,
                                             0.080f, 0.830f, 0.02f) ==
           WC3_DT_BEACON_MINIMAP);
    assert(wc3_dt_beacon_release_from_points(0, 0, 1, 0,
                                             0.160f, 0.900f,
                                             0.250f, 0.900f, 0.02f) ==
           WC3_DT_BEACON_PORTRAIT);
    assert(wc3_dt_beacon_release_from_points(0, 0, 0, 1,
                                             0.160f, 0.900f,
                                             0.500f, 0.400f, 0.02f) ==
           WC3_DT_BEACON_FIELD);
    assert(wc3_dt_beacon_release_from_points(1, 0, 0, 0,
                                             0.160f, 0.900f,
                                             0.165f, 0.902f, 0.02f) ==
           WC3_DT_BEACON_ARM);
}

static void test_command_release_resolution(void)
{
    /* The DOWN already selected the command.  Only a drag from a targeted
     * command that ends over the field confirms the target; a tap, a
     * non-targeted command, or a release outside the field is a no-op. */
    assert(wc3_dt_command_release(0, 1, 1) == WC3_DT_COMMAND_NONE);
    assert(wc3_dt_command_release(1, 0, 1) == WC3_DT_COMMAND_NONE);
    assert(wc3_dt_command_release(1, 1, 0) == WC3_DT_COMMAND_NONE);
    assert(wc3_dt_command_release(1, 1, 1) == WC3_DT_COMMAND_TARGET);

    /* A drag that ends on a selected-unit grid button targets that unit when
     * a target mode is live; a tap or an instant command does not. */
    assert(wc3_dt_command_grid_target(1, 1, 1));
    assert(!wc3_dt_command_grid_target(0, 1, 1));
    assert(!wc3_dt_command_grid_target(1, 0, 1));
    assert(!wc3_dt_command_grid_target(1, 1, 0));

    /* A tap on a cell of the already-active subgroup selects that unit; a
     * target press, a drag that crossed cells, a release on a different cell,
     * and a press that activated another subgroup do not. */
    assert(wc3_dt_grid_release_select(0, 0, 1, 0));
    assert(!wc3_dt_grid_release_select(1, 0, 1, 0));
    assert(!wc3_dt_grid_release_select(0, 1, 1, 0));
    assert(!wc3_dt_grid_release_select(0, 0, 0, 0));
    assert(!wc3_dt_grid_release_select(0, 0, 1, 1));

    /* Portrait is the same kind of HUD target: the displayed/selected unit
     * is resolved by the HLE, so the pure rule only gates live target mode,
     * portrait hit and movement. */
    assert(wc3_dt_command_portrait_target(1, 1, 1));
    assert(!wc3_dt_command_portrait_target(0, 1, 1));
    assert(!wc3_dt_command_portrait_target(1, 0, 1));
    assert(!wc3_dt_command_portrait_target(1, 1, 0));
}

static void test_inventory_release_resolution(void)
{
    /* A stale source cancels before any destination rule. */
    assert(wc3_dt_inventory_release(-1, 0, 1, 1, 1, 0) ==
           WC3_DT_INV_CANCEL);
    assert(wc3_dt_inventory_release(-1, -1, 0, 1, 1, 0) ==
           WC3_DT_INV_CANCEL);

    /* Same slot is a tap/use even when every other condition would also
     * match a drop. */
    assert(wc3_dt_inventory_release(2, 2, 1, 0, 1, 1) ==
           WC3_DT_INV_USE);

    /* A destination slot wins over the tap slop: a short deliberate drag to
     * an adjacent slot must move, not use. */
    assert(wc3_dt_inventory_release(2, 3, 1, 1, 1, 1) ==
           WC3_DT_INV_MOVE_SLOT);

    /* A destination slot without a readable button rect must cancel, not
     * fall through to a world drop at the same point. */
    assert(wc3_dt_inventory_release(2, 3, 0, 0, 1, 0) ==
           WC3_DT_INV_CANCEL);
    assert(wc3_dt_inventory_release(2, 3, 0, 1, 1, 0) ==
           WC3_DT_INV_CANCEL);

    /* A tap that drifted out of the slot still uses the item. */
    assert(wc3_dt_inventory_release(2, -1, 0, 1, 1, 0) ==
           WC3_DT_INV_USE);
    assert(wc3_dt_inventory_release(2, -1, 0, 1, 0, 0) ==
           WC3_DT_INV_USE);

    /* A real drag to the battlefield drops only outside every HUD panel. */
    assert(wc3_dt_inventory_release(2, -1, 0, 0, 1, 0) ==
           WC3_DT_INV_DROP_FIELD);
    assert(wc3_dt_inventory_release(2, -1, 0, 0, 1, 1) ==
           WC3_DT_INV_CANCEL);
    assert(wc3_dt_inventory_release(2, -1, 0, 0, 0, 0) ==
           WC3_DT_INV_CANCEL);
}

static void test_command_target_mode(void)
{
    /* The command card's click handler selects a mode by the command data
     * type.  The selected-unit grid target (+0x210) and the four command
     * target modes all count; the select mode and a null mode do not. */
    assert(!wc3_dt_command_target_mode(0, 0x210, 0x228, 0x238, 0x23c,
                                       0x240));
    assert(!wc3_dt_command_target_mode(0x214, 0x210, 0x228, 0x238, 0x23c,
                                       0x240));
    assert(!wc3_dt_command_target_mode(0x300, 0, 0, 0, 0, 0));
    assert(wc3_dt_command_target_mode(0x210, 0x210, 0x228, 0x238, 0x23c,
                                      0x240));
    assert(wc3_dt_command_target_mode(0x228, 0x210, 0x228, 0x238, 0x23c,
                                      0x240));
    assert(wc3_dt_command_target_mode(0x238, 0x210, 0x228, 0x238, 0x23c,
                                      0x240));
    assert(wc3_dt_command_target_mode(0x23c, 0x210, 0x228, 0x238, 0x23c,
                                      0x240));
    assert(wc3_dt_command_target_mode(0x240, 0x210, 0x228, 0x238, 0x23c,
                                      0x240));
}

static void test_command_target_live(void)
{
    /* The command data word at +0x8 is not a reliable type (T2 saw pairs and
     * ASCII-like values on commands that did enter target mode), so the
     * release uses the live input mode.  A targeted command is live only after
     * the game leaves CSelectMode; the target object need not match any of the
     * four fixed CGameUI fields.  The 02:05 trace saw active=42B10088 while
     * select=42AD0088. */
    assert(!wc3_dt_command_target_live(0x214, 0x214));
    assert(!wc3_dt_command_target_live(0, 0x214));
    assert(!wc3_dt_command_target_live(0x300, 0));
    assert(wc3_dt_command_target_live(0x300, 0x214));
    assert(wc3_dt_command_target_live(0x42B10088, 0x42AD0088));
    assert(wc3_dt_command_target_live(0x43420088, 0x42AD0088));
    /* The helper only knows the live mode; the caller uses it only while a
     * command gesture or its queued confirmation owns the pointer. */
    assert(wc3_dt_command_target_live(0x43400088, 0x42AD0088));
}

static void test_target_mode_live(void)
{
    const uint32_t select_mode = 0x42AD0088u;
    const uint32_t target_mode = 0x43420088u;
    const uint32_t camera_mode = 0x42B10088u;
    const uint32_t other_mode = 0x42B20088u;

    /* Target/placement mode owns the next world click even when it closes the
     * click gate.  The live mode, not the gate, is the authority. */
    assert(wc3_dt_target_mode_live(target_mode, select_mode, target_mode,
                                   camera_mode));
    /* The command card can allocate a target object outside the fixed
     * CGameUI fields; any live non-select mode is still a target. */
    assert(wc3_dt_target_mode_live(other_mode, select_mode, target_mode,
                                   camera_mode));
    assert(!wc3_dt_target_mode_live(select_mode, select_mode, target_mode,
                                    camera_mode));
    /* Camera is also non-select but never confirms a target. */
    assert(!wc3_dt_target_mode_live(camera_mode, select_mode, target_mode,
                                    camera_mode));
    assert(!wc3_dt_target_mode_live(0, select_mode, target_mode, camera_mode));
}

static void test_camera_mode_active(void)
{
    /* Direct-touch camera is mode-neutral: it never calls SetInputMode, so
     * select/target mode stays live and a second finger can act immediately.
     * CGameUI+0x21C is still the dedicated camera mode and is never a command
     * target if a native or legacy source sets it. */
    const uint32_t select_mode = 0x42AD0088u;
    const uint32_t target_mode = 0x43420088u;
    const uint32_t camera_mode = 0x42B10088u;

    assert(wc3_dt_camera_mode_active(camera_mode, camera_mode));
    assert(!wc3_dt_camera_mode_active(target_mode, camera_mode));
    assert(!wc3_dt_camera_mode_active(select_mode, camera_mode));
    assert(!wc3_dt_camera_mode_active(camera_mode, 0u));

    /* The generic live-mode check cannot distinguish camera from target; the
     * camera guard above is what keeps a camera mode out of a command
     * confirmation. */
    assert(wc3_dt_command_target_live(camera_mode, select_mode));
    assert(!wc3_dt_command_target_live(select_mode, select_mode));
}

static void test_field_click_defer(void)
{
    /* inside, ui, world, select present, select active, gate, ready field */
    /* A field press is held only while the game has a world and the select
     * mode is active, but the exact service route is not FIELD yet (map
     * load/fade-in) or the click gate is still closed.  A menu, a field drag
     * and a command target mode are all select-inactive and stay native. */
    assert(!wc3_dt_field_click_defer(0, 1, 1, 1, 1, 1, 1));
    assert(!wc3_dt_field_click_defer(1, 0, 1, 1, 1, 1, 1));
    assert(!wc3_dt_field_click_defer(1, 1, 0, 1, 1, 1, 1));
    assert(!wc3_dt_field_click_defer(1, 1, 1, 0, 0, 1, 1));
    assert(!wc3_dt_field_click_defer(1, 1, 1, 1, 0, 1, 1));
    /* Target mode: select is not active, so the target confirmation must not
     * be deferred even though the exact route is NATIVE. */
    assert(!wc3_dt_field_click_defer(1, 1, 1, 1, 0, 1, 0));
    /* Ready select mode: the native field path runs normally. */
    assert(!wc3_dt_field_click_defer(1, 1, 1, 1, 1, 1, 1));
    /* First tap after a map load: the snapshot sees select, the exact guest
     * route is still not FIELD.  Hold and replay it. */
    assert(wc3_dt_field_click_defer(1, 1, 1, 1, 1, 1, 0));
    /* The gate is still closed during the load. */
    assert(wc3_dt_field_click_defer(1, 1, 1, 1, 1, 0, 0));
    assert(wc3_dt_field_click_defer(1, 1, 1, 1, 1, 0, 1));
}

static void test_native_release_replay(void)
{
    /* A live target/placement mode outside LEGACY keeps the immediate replay:
     * the modal loop may never poll GetCursorPos. */
    assert(wc3_dt_native_release_replay(WC3_DIRECT_TOUCH_ROUTE_NATIVE, 1));
    /* A menu, loading screen or HUD release has no live target.  It must use
     * the universal idle gateway, not the replay that hit-tests the stale hot
     * control. */
    assert(!wc3_dt_native_release_replay(WC3_DIRECT_TOUCH_ROUTE_NATIVE, 0));
    /* A LEGACY route has no semantic context; a stale target word must not
     * turn a menu click into a placement confirmation. */
    assert(!wc3_dt_native_release_replay(WC3_DIRECT_TOUCH_ROUTE_LEGACY, 1));
    assert(!wc3_dt_native_release_replay(WC3_DIRECT_TOUCH_ROUTE_LEGACY, 0));
    /* FIELD releases are semantic and never reach the native branch. */
    assert(wc3_dt_native_release_replay(WC3_DIRECT_TOUCH_ROUTE_FIELD, 1));
    assert(!wc3_dt_native_release_replay(WC3_DIRECT_TOUCH_ROUTE_FIELD, 0));
}

static void test_event_cancels_field_click(void)
{
    /* A new semantic gesture supersedes a held map-load tap.  A camera begin
     * joins only after the camera accepted it; a rejected begin starts no pan
     * and must leave the held tap for replay. */
    assert(wc3_dt_event_cancels_field_click(WC3_DIRECT_TOUCH_POINTER_DOWN, 0));
    assert(wc3_dt_event_cancels_field_click(WC3_DIRECT_TOUCH_TAP, 0));
    assert(wc3_dt_event_cancels_field_click(WC3_DIRECT_TOUCH_FIELD_HOVER, 0));
    assert(!wc3_dt_event_cancels_field_click(
        WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN, 0));
    assert(wc3_dt_event_cancels_field_click(
        WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN, 1));
    assert(!wc3_dt_event_cancels_field_click(
        WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE, 1));
    assert(!wc3_dt_event_cancels_field_click(
        WC3_DIRECT_TOUCH_FIELD_CAMERA_END, 1));
    /* A deferred selection begin posts no native button, so it must cancel a
     * held map-load tap explicitly.  DRAG_BEGIN is defense-in-depth for a
     * lost BEGIN. */
    assert(wc3_dt_event_cancels_field_click(
        WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN, 0));
    assert(wc3_dt_event_cancels_field_click(
        WC3_DIRECT_TOUCH_FIELD_SELECT_DRAG_BEGIN, 0));
    assert(!wc3_dt_event_cancels_field_click(
        WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE, 0));
}

static void test_event_bumps_input_epoch(void)
{
    /* A contact start begins a new command, so it must supersede a deferred
     * field click.  Portrait emits FOLLOW_START instead of POINTER_DOWN, and
     * a control-group chord emits GROUP_SIGNAL; both are new commands. */
    assert(wc3_dt_event_bumps_input_epoch(WC3_DIRECT_TOUCH_POINTER_DOWN));
    assert(wc3_dt_event_bumps_input_epoch(WC3_DIRECT_TOUCH_TAP));
    assert(wc3_dt_event_bumps_input_epoch(WC3_DIRECT_TOUCH_LONG_PRESS));
    assert(wc3_dt_event_bumps_input_epoch(WC3_DIRECT_TOUCH_FIELD_HOVER));
    assert(wc3_dt_event_bumps_input_epoch(
        WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN));
    assert(wc3_dt_event_bumps_input_epoch(
        WC3_DIRECT_TOUCH_FIELD_SELECT_DRAG_BEGIN));
    assert(wc3_dt_event_bumps_input_epoch(WC3_DIRECT_TOUCH_FOLLOW_START));
    assert(wc3_dt_event_bumps_input_epoch(WC3_DIRECT_TOUCH_GROUP_SIGNAL));

    /* Motion, release and camera phases continue an existing gesture and must
     * not cancel.  FIELD_CAMERA_BEGIN cancels only after the guest accepts the
     * pan, through the camera_started path, not through the epoch. */
    assert(!wc3_dt_event_bumps_input_epoch(WC3_DIRECT_TOUCH_POINTER_MOVE));
    assert(!wc3_dt_event_bumps_input_epoch(WC3_DIRECT_TOUCH_POINTER_UP));
    assert(!wc3_dt_event_bumps_input_epoch(WC3_DIRECT_TOUCH_POINTER_CANCEL));
    assert(!wc3_dt_event_bumps_input_epoch(WC3_DIRECT_TOUCH_HOLD_RELEASE));
    assert(!wc3_dt_event_bumps_input_epoch(
        WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN));
    assert(!wc3_dt_event_bumps_input_epoch(
        WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE));
    assert(!wc3_dt_event_bumps_input_epoch(
        WC3_DIRECT_TOUCH_FIELD_CAMERA_END));
    assert(!wc3_dt_event_bumps_input_epoch(
        WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE));
    assert(!wc3_dt_event_bumps_input_epoch(
        WC3_DIRECT_TOUCH_FIELD_SELECT_END));
    /* KEY_SIGNAL has no producer today and does not carry an unambiguous
     * direction in its kind; a future producer must route its down transition
     * through hle_android_key_event or extend this predicate. */
    assert(!wc3_dt_event_bumps_input_epoch(WC3_DIRECT_TOUCH_KEY_SIGNAL));
}

static void test_command_movement_slop(void)
{
    /* The command handler can miss a MOVE that the recognizer routes over
     * the field path, so the release recomputes movement from DOWN to UP. */
    assert(wc3_dt_command_moved(0.5f, 0.5f, 0.5f, 0.5f, 0.02f) == 0);
    assert(wc3_dt_command_moved(0.5f, 0.5f, 0.51f, 0.5f, 0.02f) == 0);
    assert(wc3_dt_command_moved(0.5f, 0.5f, 0.51f, 0.51f, 0.02f) == 0);
    assert(wc3_dt_command_moved(0.5f, 0.5f, 0.53f, 0.5f, 0.02f) == 1);
    assert(wc3_dt_command_moved(0.5f, 0.5f, 0.5f, 0.53f, 0.02f) == 1);
    assert(wc3_dt_command_moved(0.5f, 0.5f, 0.52f, 0.52f, 0.02f) == 1);
}

static void test_console_rect_scale(void)
{
    /* The 00:58 trace's command bar: the HLE-local rect
     * 0.6181,0.8690..0.7926,0.9980 is the visible card at
     * 0.7726,0.7817..0.9908,0.9967 on the surface.  The transform must
     * undo the 0.8x0.6 console scale, not just flip y. */
    float l = 0.6181f;
    float t = 0.8690f;
    float r = 0.7926f;
    float b = 0.9980f;
    wc3_dt_console_rect_scale(&l, &t, &r, &b);
    expect_near("console rect left", l, 0.7726f);
    expect_near("console rect top", t, 0.7817f);
    expect_near("console rect right", r, 0.9908f);
    expect_near("console rect bottom", b, 0.9967f);
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

    /* CMinimap+0x644 is CRect {minY,minX,maxY,maxX} in DDC space (Y up).
     * The beacon helper must map the surface top to maxY and the bottom to
     * minY; feeding the surface fraction straight through mirrors the signal
     * about the map's horizontal centre. */
    assert(wc3_dt_minimap_ddc_local_point(0.10f, 0.20f, 0.20f, 0.10f, 0.60f,
                                          0.50f, 40.0f, 20.0f, 140.0f,
                                          120.0f, &x, &y));
    expect_near("minimap ddc top-left x", x, 20.0f);
    expect_near("minimap ddc top-left y", y, 140.0f);

    assert(wc3_dt_minimap_ddc_local_point(0.30f, 0.40f, 0.20f, 0.10f, 0.60f,
                                          0.50f, 40.0f, 20.0f, 140.0f,
                                          120.0f, &x, &y));
    expect_near("minimap ddc centre x", x, 70.0f);
    expect_near("minimap ddc centre y", y, 90.0f);

    assert(wc3_dt_minimap_ddc_local_point(0.50f, 0.60f, 0.20f, 0.10f, 0.60f,
                                          0.50f, 40.0f, 20.0f, 140.0f,
                                          120.0f, &x, &y));
    expect_near("minimap ddc bottom-right x", x, 120.0f);
    expect_near("minimap ddc bottom-right y", y, 40.0f);
}

static void test_minimap_input_rect_and_mapping(void)
{
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    /* Native map frame: the lower part of the visible panel.  The command
     * card publishes the console top, which is the top of the whole panel. */
    assert(wc3_dt_minimap_input_rect(0.0094f, 0.8550f, 0.1488f, 0.9931f,
                                     1, 0.7817f,
                                     &left, &top, &right, &bottom));
    expect_near("minimap input left", left, 0.0094f);
    expect_near("minimap input top", top, 0.7817f);
    expect_near("minimap input right", right, 0.1488f);
    expect_near("minimap input bottom", bottom, 0.9931f);

    /* The strip 0.7817..0.85 belongs to the minimap even though it overlaps
     * the field rectangle's bottom.  The native +0xB4 frame does not cover
     * it, so using that frame as the ownership gate would start both a field
     * tap and a minimap drag. */
    assert(wc3_dt_minimap_input_contains(0.08f, 0.80f,
                                         left, top, right, bottom));
    assert(!wc3_dt_minimap_input_contains(0.08f, 0.80f,
                                          0.0094f, 0.8550f,
                                          0.1488f, 0.9931f));

    /* The live CGameUI+0x41C panel wins when it contains the map image; it
     * also has to cover the button row above/right of that image. */
    assert(wc3_dt_minimap_panel_prefers(0.0094f, 0.7750f, 0.1700f, 0.9931f,
                                        0.0094f, 0.8550f,
                                        0.1488f, 0.9931f));
    assert(wc3_dt_minimap_panel_prefers(0.0094f, 0.8550f, 0.1488f, 0.9931f,
                                        0.0094f, 0.8550f,
                                        0.1488f, 0.9931f));
    assert(!wc3_dt_minimap_panel_prefers(0.0094f, 0.7800f, 0.1000f, 0.9900f,
                                         0.0094f, 0.8550f,
                                         0.1488f, 0.9931f));
    assert(!wc3_dt_minimap_panel_prefers(0.0094f, 0.7750f, 0.0100f, 0.9931f,
                                         0.0094f, 0.8550f,
                                         0.1488f, 0.9931f));

    /* A whole-screen frame contains the minimap but must not become the
     * minimap gate; the button row above/right of the image is plausible. */
    assert(wc3_dt_minimap_panel_plausible(0.0094f, 0.7700f,
                                          0.1750f, 0.9931f,
                                          0.0094f, 0.8550f,
                                          0.1488f, 0.9931f));
    assert(!wc3_dt_minimap_panel_plausible(0.0000f, 0.0000f,
                                           1.0000f, 1.0000f,
                                           0.0094f, 0.8550f,
                                           0.1488f, 0.9931f));
    assert(!wc3_dt_minimap_panel_plausible(0.0094f, 0.6500f,
                                           0.1750f, 0.9931f,
                                           0.0094f, 0.8550f,
                                           0.1488f, 0.9931f));
    assert(wc3_dt_minimap_beacon_plausible(0.1500f, 0.7700f,
                                           0.1750f, 0.8200f,
                                           0.0094f, 0.8550f,
                                           0.1488f, 0.9931f));
    assert(wc3_dt_minimap_beacon_plausible(0.1500f, 0.8500f,
                                           0.1750f, 0.9000f,
                                           0.0094f, 0.8550f,
                                           0.1488f, 0.9931f));
    assert(!wc3_dt_minimap_beacon_plausible(0.5000f, 0.5000f,
                                            0.6000f, 0.6000f,
                                            0.0094f, 0.8550f,
                                            0.1488f, 0.9931f));

    float ul = 0.0094f;
    float ut = 0.7817f;
    float ur = 0.1488f;
    float ub = 0.9931f;
    wc3_dt_minimap_input_union(&ul, &ut, &ur, &ub,
                               0.1500f, 0.7700f, 0.1750f, 0.8200f);
    expect_near("minimap union left", ul, 0.0094f);
    expect_near("minimap union top", ut, 0.7700f);
    expect_near("minimap union right", ur, 0.1750f);
    expect_near("minimap union bottom", ub, 0.9931f);
    wc3_dt_minimap_input_union(&ul, &ut, &ur, &ub, 0.0f, 0.0f, 0.0f, 0.0f);
    expect_near("minimap union degenerate top", ut, 0.7700f);

    /* No console snapshot: fall back to the native frame, never to a
     * hardcoded guess. */
    assert(wc3_dt_minimap_input_rect(0.0094f, 0.8550f, 0.1488f, 0.9931f,
                                     0, 0.0f,
                                     &left, &top, &right, &bottom));
    expect_near("minimap input fallback top", top, 0.8550f);

    /* A bogus console top cannot swallow the battlefield. */
    assert(wc3_dt_minimap_input_rect(0.0094f, 0.8550f, 0.1488f, 0.9931f,
                                     1, 0.1f,
                                     &left, &top, &right, &bottom));
    expect_near("minimap input rejected console top", top, 0.8550f);

    /* Full-panel top maps to the native map top; panel bottom to map bottom.
     * This mapping is for beacon signals only; pointer phases stay in panel
     * space so the camera does not stop at the frame top first. */
    float mx = 0.0f;
    float my = 0.0f;
    assert(wc3_dt_minimap_local_point(0.0094f, 0.7817f,
                                      0.7817f, 0.0094f, 0.9931f, 0.1488f,
                                      0.8550f, 0.0094f, 0.9931f, 0.1488f,
                                      &mx, &my));
    expect_near("minimap panel top to map top x", mx, 0.0094f);
    expect_near("minimap panel top to map top y", my, 0.8550f);

    assert(wc3_dt_minimap_local_point(0.0791f, 0.9931f,
                                      0.7817f, 0.0094f, 0.9931f, 0.1488f,
                                      0.8550f, 0.0094f, 0.9931f, 0.1488f,
                                      &mx, &my));
    expect_near("minimap panel bottom to map bottom x", mx, 0.0791f);
    expect_near("minimap panel bottom to map bottom y", my, 0.9931f);

    /* A drag keeps its finger coordinate in container space.  Compressing
     * MOVEs into the +0xB4 child frame is what caused the ~65% barrier:
     * the panel top would become the child top (0.8550), not the map top. */
    wc3_dt_minimap_pointer_point(0.1171f, 0.8011f,
                                 0.0094f, 0.7817f, 0.1488f, 0.9931f,
                                 &mx, &my);
    expect_near("minimap drag keeps upper-panel y", my, 0.8011f);
    wc3_dt_minimap_pointer_point(0.1171f, 0.7100f,
                                 0.0094f, 0.7817f, 0.1488f, 0.9931f,
                                 &mx, &my);
    expect_near("minimap drag clamps to panel top", my, 0.7817f);
    wc3_dt_minimap_pointer_point(0.1171f, 1.2000f,
                                 0.0094f, 0.7817f, 0.1488f, 0.9931f,
                                 &mx, &my);
    expect_near("minimap drag clamps to panel bottom", my, 0.9931f);
}

int main(void)
{
    test_queue();
    test_tap();
    test_field_hover();
    test_tap_cancelled_by_motion();
    test_up_without_move_uses_slop();
    test_long_press_presses_once();
    test_hold_drift_presses_at_last_point();
    test_hold_then_drag_selects();
    test_quick_swipe_camera();
    test_late_move_starts_select();
    test_raw_routes_never_emit_semantics();
    test_reset_releases_field_drag();
    test_two_finger_camera_and_action();
    test_action_first_second_finger_camera();
    test_camera_first_second_finger_tap();
    test_second_finger_joins_camera();
    test_reset_discards_pending();
    test_pointer_snapshot();
    test_coordinate_mapping();
    test_unit_type_tap_order();
    test_tap_unit_intent();
    test_unit_gesture_policy();
    test_long_tap_unit_intent();
    test_selection_can_order();
    test_tap_intent_resolution();
    test_field_tap_intent();
    test_configured_long_press_and_threshold();
    test_smart_order_candidate();
    test_order_route();
    test_point_in_rect();
    test_rect_union();
    test_native_left_busy();
    test_route();
    test_physical_route();
    test_menu_event_policy();
    test_field_release_resolution();
    test_field_drag_keep();
    test_pointer_phases();
    test_portrait_gesture();
    test_beacon_release_resolution();
    test_command_release_resolution();
    test_inventory_release_resolution();
    test_command_target_mode();
    test_command_target_live();
    test_target_mode_live();
    test_camera_mode_active();
    test_field_click_defer();
    test_native_release_replay();
    test_event_cancels_field_click();
    test_event_bumps_input_epoch();
    test_command_movement_slop();
    test_console_rect_scale();
    test_minimap_local_point();
    test_minimap_input_rect_and_mapping();
    test_minimap_inverse();
    puts("direct_touch: ok");
    return 0;
}
