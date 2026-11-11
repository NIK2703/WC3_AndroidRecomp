/*
 * T0: the input adapter must not tick merely because a timer exists.  It ticks
 * for a real batch/fault or when the reported deadline is already due, matching
 * the host loop's "wake, check, sleep again" contract.
 */

#include "input_adapter.h"

#include <stdio.h>
#include <string.h>

static Wc3InputRing g_ring;
static uint64_t g_deadline_ms;
static int g_tick_calls;
static int g_cancel_calls;

void controls_cancel_all(void) {
    g_cancel_calls++;
}

int controls_touch(Wc3TouchPhase phase, int pointer_id, float x, float y,
                   uint64_t event_time_ms) {
    (void)phase;
    (void)pointer_id;
    (void)x;
    (void)y;
    (void)event_time_ms;
    return 0;
}

int controls_tick(uint64_t now_ms) {
    (void)now_ms;
    g_tick_calls++;
    return 0;
}

uint64_t controls_next_deadline_ms(uint64_t now_ms) {
    (void)now_ms;
    return g_deadline_ms;
}

static int failures;

static void expect(const char *name, int got, int want) {
    if (got != want) {
        printf("FAIL %s: got %d want %d\n", name, got, want);
        failures++;
    }
}

static void reset(Wc3InputAdapter *adapter) {
    g_deadline_ms = 0;
    g_tick_calls = 0;
    g_cancel_calls = 0;
    input_ring_clear(&g_ring);
    input_adapter_init(adapter, &g_ring);
}

static void push_event(int phase, uint64_t event_ms) {
    Wc3TouchEvent event;
    memset(&event, 0, sizeof(event));
    event.event_time_ms = event_ms;
    event.pointer_id = 1;
    event.x = 10.0f;
    event.y = 20.0f;
    event.phase = (uint8_t)phase;
    (void)input_ring_push(&g_ring, &event);
}

int main(void) {
    Wc3InputAdapter adapter;

    input_ring_init(&g_ring);

    reset(&adapter);
    expect("idle drain", input_adapter_drain(&adapter, 100), 0);
    expect("idle no tick", g_tick_calls, 0);

    g_deadline_ms = 500;
    expect("far deadline drain", input_adapter_drain(&adapter, 100), 0);
    expect("far deadline no tick", g_tick_calls, 0);

    g_tick_calls = 0;
    g_deadline_ms = 100;
    expect("due deadline drain", input_adapter_drain(&adapter, 100), 0);
    expect("due deadline ticks", g_tick_calls, 1);

    g_tick_calls = 0;
    g_deadline_ms = 99;
    expect("past deadline drain", input_adapter_drain(&adapter, 100), 0);
    expect("past deadline ticks", g_tick_calls, 1);

    g_tick_calls = 0;
    g_deadline_ms = 101;
    expect("future deadline drain", input_adapter_drain(&adapter, 100), 0);
    expect("future deadline no tick", g_tick_calls, 0);

    reset(&adapter);
    push_event(WC3_TOUCH_DOWN, 100);
    expect("event drain", input_adapter_drain(&adapter, 100), 0);
    expect("event ticks", g_tick_calls, 1);
    expect("event counted", (int)adapter.stats.events, 1);
    expect("control recorded", adapter.stats.last_control_events, 1);

    reset(&adapter);
    input_ring_note_fault(&g_ring);
    expect("fault drain", input_adapter_drain(&adapter, 100), 0);
    expect("fault cancels", g_cancel_calls, 1);
    expect("fault ticks", g_tick_calls, 1);

    reset(&adapter);
    expect("next deadline no timer",
           (int)input_adapter_next_deadline_ms(&adapter, 100), 0);
    g_deadline_ms = 700;
    expect("next deadline far",
           (int)input_adapter_next_deadline_ms(&adapter, 100), 700);

    input_ring_destroy(&g_ring);

    if (failures == 0) printf("input adapter idle gate OK\n");
    return failures != 0;
}
