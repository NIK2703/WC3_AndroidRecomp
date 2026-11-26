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

    /* A camera candidate never moves the shared cursor, so DOWN emits no
     * hover.  The action hover is emitted immediately before its click. */
    wc3_direct_touch_down(&dt, 40, 300.0f, 400.0f, 0);
    assert(g_hover_count == 0);
    assert(g_event_count == 0);

    wc3_direct_touch_move(&dt, 40, 310.0f, 410.0f, 10);
    assert(g_hover_count == 0);

    wc3_direct_touch_up(&dt, 40, 310.0f, 410.0f, 50);
    assert(g_hover_count == 1);
    assert(g_event_count == 1);
    assert(g_events[0].kind == WC3_DIRECT_TOUCH_TAP);
    assert(g_events[0].owner_role == WC3_DT_OWNER_ACTION);

    /* Two DOWN events emit no hover either; roles are resolved on MOVE or at
     * the long-press deadline.  Both field contacts stay camera candidates
     * until then, even when one of them is already a cursor. */
    events_clear();
    wc3_direct_touch_down(&dt, 41, 500.0f, 500.0f, 1000);
    wc3_direct_touch_down(&dt, 42, 600.0f, 600.0f, 1001);
    assert(g_hover_count == 0);
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
    assert(g_event_count == 3);
    assert(g_events[2].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE);
    expect_near("select move2 x", g_events[2].x, 0.16f);

    wc3_direct_touch_up(&dt, 9, 180.0f, 140.0f, 600);
    assert(g_event_count == 4);
    assert(g_events[3].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END);
    expect_near("select end x", g_events[3].x, 0.18f);
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
    assert(g_hover_count == 0);

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

    wc3_direct_touch_up(&dt, 12, 150.0f, 120.0f, 200);
    assert(g_event_count == 4);
    assert(g_events[3].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END);
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
    assert(g_hover_count == 0);

    /* The other finger reaches its long-press deadline and owns the native
     * left button while the camera keeps panning. */
    assert(wc3_direct_touch_tick(&dt, 130) == 1);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN);
    assert(g_events[1].owner_role == WC3_DT_OWNER_ACTION);
    assert(wc3_direct_touch_action_pointer(&dt) == 5);
    assert(g_hover_count == 1);

    wc3_direct_touch_move(&dt, 4, 160.0f, 100.0f, 200);
    assert(g_event_count == 3);
    assert(g_events[2].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE);
    assert(g_events[2].owner_role == WC3_DT_OWNER_CAMERA);

    wc3_direct_touch_up(&dt, 5, 520.0f, 520.0f, 250);
    assert(g_event_count == 4);
    assert(g_events[3].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END);

    wc3_direct_touch_up(&dt, 4, 160.0f, 100.0f, 300);
    assert(g_event_count == 5);
    assert(g_events[4].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END);
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
    assert(g_hover_count == 1);

    /* The cursor keeps its native selection drag while the camera pans. */
    wc3_direct_touch_move(&dt, 40, 220.0f, 210.0f, 260);
    assert(g_event_count == 3);
    assert(g_events[2].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE);
    wc3_direct_touch_move(&dt, 41, 760.0f, 320.0f, 280);
    assert(g_event_count == 4);
    assert(g_events[3].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE);
    assert(g_events[3].owner_role == WC3_DT_OWNER_CAMERA);

    wc3_direct_touch_up(&dt, 41, 760.0f, 320.0f, 300);
    wc3_direct_touch_up(&dt, 40, 220.0f, 210.0f, 320);
    assert(g_event_count == 6);
    assert(g_events[4].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END);
    assert(g_events[5].kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END);
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

    /* A second finger must be a full ACTION cursor immediately, not a
     * camera candidate and not a one-shot that is consumed by the pan. */
    wc3_direct_touch_down(&dt, 61, 500.0f, 500.0f, 60);
    assert(wc3_direct_touch_contact_role(&dt, 61) == WC3_DT_CONTACT_ACTION);
    assert(g_event_count == 1);
    assert(g_hover_count == 0);

    /* Its first tap reaches the semantic path while the camera continues. */
    wc3_direct_touch_up(&dt, 61, 500.0f, 500.0f, 80);
    assert(g_event_count == 2);
    assert(g_events[1].kind == WC3_DIRECT_TOUCH_TAP);
    assert(g_events[1].owner_role == WC3_DT_OWNER_ACTION);
    assert(g_events[1].route == WC3_DIRECT_TOUCH_ROUTE_FIELD);
    assert(g_hover_count == 1);

    wc3_direct_touch_move(&dt, 60, 160.0f, 120.0f, 100);
    assert(g_event_count == 3);
    assert(g_events[2].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE);

    wc3_direct_touch_up(&dt, 60, 160.0f, 120.0f, 120);
    assert(g_event_count == 4);
    assert(g_events[3].kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END);
    assert(wc3_direct_touch_active_count(&dt) == 0);
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

    /* Portrait is the same kind of HUD target: the displayed/selected unit
     * is resolved by the HLE, so the pure rule only gates live target mode,
     * portrait hit and movement. */
    assert(wc3_dt_command_portrait_target(1, 1, 1));
    assert(!wc3_dt_command_portrait_target(0, 1, 1));
    assert(!wc3_dt_command_portrait_target(1, 0, 1));
    assert(!wc3_dt_command_portrait_target(1, 1, 0));
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
    assert(!wc3_dt_event_cancels_field_click(
        WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN, 0));
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
    test_reset_discards_pending();
    test_pointer_snapshot();
    test_coordinate_mapping();
    test_tap_intent_resolution();
    test_route();
    test_physical_route();
    test_field_release_resolution();
    test_field_drag_keep();
    test_pointer_phases();
    test_portrait_gesture();
    test_beacon_release_resolution();
    test_command_release_resolution();
    test_command_target_mode();
    test_command_target_live();
    test_camera_mode_active();
    test_field_click_defer();
    test_event_cancels_field_click();
    test_command_movement_slop();
    test_console_rect_scale();
    test_minimap_local_point();
    test_minimap_input_rect_and_mapping();
    test_minimap_inverse();
    puts("direct_touch: ok");
    return 0;
}
