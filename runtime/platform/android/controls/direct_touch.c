#include "direct_touch.h"

#include <math.h>
#include <string.h>

static float clamp01(float value)
{
    if (!(value > 0.0f)) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static void emit_event(Wc3DirectTouch *dt, Wc3DirectTouchKind kind,
                       int pointer_id, float x, float y, uint64_t now_ms)
{
    if (!dt->sink) return;
    Wc3DirectTouchEvent event;
    memset(&event, 0, sizeof(event));
    event.kind = (uint32_t)kind;
    event.pointer_id = (uint32_t)pointer_id;
    event.x = clamp01(dt->width > 0.0f ? x / dt->width : 0.0f);
    event.y = clamp01(dt->height > 0.0f ? y / dt->height : 0.0f);
    event.time_ms = now_ms;
    dt->sink(&event, dt->user);
}

static float distance_sq(float ax, float ay, float bx, float by)
{
    const float dx = ax - bx;
    const float dy = ay - by;
    return dx * dx + dy * dy;
}

static int active_count(const Wc3DirectTouch *dt)
{
    int count = 0;
    for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
        if (dt->pointers[i].active) count++;
    }
    return count;
}

static Wc3DirectTouchPointer *find_pointer(Wc3DirectTouch *dt, int pointer_id)
{
    for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
        if (dt->pointers[i].active &&
            dt->pointers[i].pointer_id == pointer_id) {
            return &dt->pointers[i];
        }
    }
    return NULL;
}

static Wc3DirectTouchPointer *alloc_pointer(Wc3DirectTouch *dt)
{
    for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
        if (!dt->pointers[i].active) return &dt->pointers[i];
    }
    return NULL;
}

void wc3_direct_touch_init(Wc3DirectTouch *dt, float width, float height,
                           float tap_slop_px, float long_press_slop_px,
                           int long_press_ms,
                           Wc3DirectTouchSink sink, void *user)
{
    if (!dt) return;
    memset(dt, 0, sizeof(*dt));
    dt->width = width;
    dt->height = height;
    dt->tap_slop_px = tap_slop_px > 0.0f ? tap_slop_px : 1.0f;
    dt->long_press_slop_px = long_press_slop_px > 0.0f
        ? long_press_slop_px : dt->tap_slop_px;
    dt->long_press_ms = long_press_ms;
    dt->sink = sink;
    dt->user = user;
}

void wc3_direct_touch_set_sink(Wc3DirectTouch *dt, Wc3DirectTouchSink sink,
                               void *user)
{
    if (!dt) return;
    dt->sink = sink;
    dt->user = user;
}

void wc3_direct_touch_set_size(Wc3DirectTouch *dt, float width, float height)
{
    if (!dt) return;
    dt->width = width;
    dt->height = height;
}

void wc3_direct_touch_down(Wc3DirectTouch *dt, int pointer_id,
                           float x, float y, uint64_t now_ms)
{
    if (!dt) return;

    Wc3DirectTouchPointer *pointer = find_pointer(dt, pointer_id);
    if (pointer) {
        /* A duplicate DOWN for the same id starts a new contact. */
        memset(pointer, 0, sizeof(*pointer));
    } else {
        pointer = alloc_pointer(dt);
    }
    if (!pointer) return;

    const int others = active_count(dt);
    if (others > 0) {
        for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
            if (dt->pointers[i].active) dt->pointers[i].multi = 1;
        }
    }

    pointer->pointer_id = pointer_id;
    pointer->down_x = x;
    pointer->down_y = y;
    pointer->last_x = x;
    pointer->last_y = y;
    pointer->down_ms = now_ms;
    pointer->active = 1;
    pointer->multi = others > 0 ? 1 : 0;
}

void wc3_direct_touch_move(Wc3DirectTouch *dt, int pointer_id,
                           float x, float y, uint64_t now_ms)
{
    (void)now_ms;
    if (!dt) return;
    Wc3DirectTouchPointer *pointer = find_pointer(dt, pointer_id);
    if (!pointer) return;

    pointer->last_x = x;
    pointer->last_y = y;
    const float moved_sq =
        distance_sq(x, y, pointer->down_x, pointer->down_y);
    if (moved_sq > dt->tap_slop_px * dt->tap_slop_px) {
        /* Too much drift for a tap.  A long press has its own, larger
         * tolerance below. */
        pointer->moved = 1;
    }
    if (moved_sq > dt->long_press_slop_px * dt->long_press_slop_px) {
        /* Too much drift for a long press either: suppress both. */
        pointer->moved = 1;
        pointer->long_fired = 1;
    }
}

void wc3_direct_touch_up(Wc3DirectTouch *dt, int pointer_id,
                         float x, float y, uint64_t now_ms)
{
    if (!dt) return;
    Wc3DirectTouchPointer *pointer = find_pointer(dt, pointer_id);
    if (!pointer) return;

    pointer->last_x = x;
    pointer->last_y = y;

    const float moved_sq = distance_sq(x, y, pointer->down_x, pointer->down_y);
    const uint64_t timeout = dt->long_press_ms > 0
        ? (uint64_t)dt->long_press_ms : 0u;
    const int held_long = now_ms >= pointer->down_ms &&
                          now_ms - pointer->down_ms >= timeout;
    const int beyond_long = moved_sq >
        dt->long_press_slop_px * dt->long_press_slop_px;

    if (!pointer->multi && !pointer->long_fired && !beyond_long) {
        if (held_long) {
            /* The input thread normally emits this from the deadline tick.
             * If the tick was late, the release still produces the long
             * press instead of degrading into a tap at the release point. */
            pointer->long_fired = 1;
            emit_event(dt, WC3_DIRECT_TOUCH_LONG_PRESS, pointer_id,
                       pointer->last_x, pointer->last_y, now_ms);
        } else if (!pointer->moved &&
                   moved_sq <= dt->tap_slop_px * dt->tap_slop_px) {
            /* A fast flick can arrive as DOWN+UP with no MOVE at all; the
             * release point must still be inside the tap slop. */
            emit_event(dt, WC3_DIRECT_TOUCH_TAP, pointer_id, x, y, now_ms);
        }
    }
    memset(pointer, 0, sizeof(*pointer));
}

void wc3_direct_touch_reset(Wc3DirectTouch *dt)
{
    if (!dt) return;
    memset(dt->pointers, 0, sizeof(dt->pointers));
}

int wc3_direct_touch_tick(Wc3DirectTouch *dt, uint64_t now_ms)
{
    if (!dt) return 0;
    int emitted = 0;
    for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
        Wc3DirectTouchPointer *pointer = &dt->pointers[i];
        if (!pointer->active || pointer->multi || pointer->long_fired) {
            continue;
        }
        const uint64_t timeout = dt->long_press_ms > 0
            ? (uint64_t)dt->long_press_ms : 0u;
        if (now_ms < pointer->down_ms ||
            now_ms - pointer->down_ms < timeout) {
            continue;
        }
        pointer->long_fired = 1;
        emit_event(dt, WC3_DIRECT_TOUCH_LONG_PRESS, pointer->pointer_id,
                   pointer->last_x, pointer->last_y, now_ms);
        emitted++;
    }
    return emitted;
}

uint64_t wc3_direct_touch_next_deadline_ms(const Wc3DirectTouch *dt)
{
    if (!dt) return 0;
    uint64_t next = 0;
    for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
        const Wc3DirectTouchPointer *pointer = &dt->pointers[i];
        if (!pointer->active || pointer->multi || pointer->long_fired) {
            continue;
        }
        const uint64_t timeout = dt->long_press_ms > 0
            ? (uint64_t)dt->long_press_ms : 0u;
        uint64_t deadline = pointer->down_ms + timeout;
        if (deadline < pointer->down_ms) deadline = UINT64_MAX;
        if (next == 0 || deadline < next) next = deadline;
    }
    return next;
}

int wc3_direct_touch_active_count(const Wc3DirectTouch *dt)
{
    return dt ? active_count(dt) : 0;
}

int wc3_direct_touch_pointer_at(const Wc3DirectTouch *dt, int index,
                                int *pointer_id, float *x, float *y)
{
    if (!dt || index < 0) return 0;
    for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
        const Wc3DirectTouchPointer *pointer = &dt->pointers[i];
        if (!pointer->active) continue;
        if (index-- != 0) continue;
        if (pointer_id) *pointer_id = pointer->pointer_id;
        if (x) *x = pointer->last_x;
        if (y) *y = pointer->last_y;
        return 1;
    }
    return 0;
}
