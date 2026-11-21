/*
 * direct_touch.h - host-side gesture recognizer for semantic direct touch.
 *
 * This layer owns only touch classification.  It does not know about guest
 * addresses, Win32 messages, or the overlay profile; the controls adapter
 * decides whether a pointer belongs here and forwards the resulting tap,
 * long-press or portrait-follow event to the guest-thread service.
 */
#ifndef WC3_DIRECT_TOUCH_H
#define WC3_DIRECT_TOUCH_H

#include <stdint.h>

#include "direct_touch_event.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WC3_DIRECT_TOUCH_MAX_POINTERS 4

typedef struct {
    int pointer_id;
    float down_x;
    float down_y;
    float last_x;
    float last_y;
    uint64_t down_ms;
    uint32_t route;      /* Wc3DirectTouchRoute that owns the gesture */
    uint8_t active;
    uint8_t moved;
    uint8_t multi;
    uint8_t long_fired;
} Wc3DirectTouchPointer;

typedef struct {
    float width;
    float height;
    float tap_slop_px;
    float long_press_slop_px;
    int long_press_ms;
    Wc3DirectTouchSink sink;
    void *user;
    Wc3DirectTouchPointer pointers[WC3_DIRECT_TOUCH_MAX_POINTERS];
} Wc3DirectTouch;

/*
 * Configure a recognizer instance.  All distances are surface pixels; the
 * controls adapter derives them from the smaller screen dimension, so the
 * thresholds scale with the device instead of assuming one DPI.  A
 * long_press_ms <= 0 still emits a long press at the next tick; the controls
 * layer passes the product timeout.
 */
void wc3_direct_touch_init(Wc3DirectTouch *dt, float width, float height,
                           float tap_slop_px, float long_press_slop_px,
                           int long_press_ms,
                           Wc3DirectTouchSink sink, void *user);

void wc3_direct_touch_set_sink(Wc3DirectTouch *dt, Wc3DirectTouchSink sink,
                               void *user);

void wc3_direct_touch_set_size(Wc3DirectTouch *dt, float width, float height);

/* Feed one pointer.  x,y are surface pixels.  The route is attached to every
 * semantic event the pointer produces; wc3_direct_touch_down() is the FIELD
 * shorthand.  A PORTRAIT down emits FOLLOW_START immediately and keeps that
 * pointer owned by the portrait until its UP, regardless of later movement.
 * Raw pointer phases always carry NATIVE. */
void wc3_direct_touch_down_route(Wc3DirectTouch *dt, int pointer_id,
                                 float x, float y, uint64_t now_ms,
                                 uint32_t route);
void wc3_direct_touch_down(Wc3DirectTouch *dt, int pointer_id,
                           float x, float y, uint64_t now_ms);
void wc3_direct_touch_move(Wc3DirectTouch *dt, int pointer_id,
                           float x, float y, uint64_t now_ms);
void wc3_direct_touch_up(Wc3DirectTouch *dt, int pointer_id,
                         float x, float y, uint64_t now_ms);

/* Cancel every active pointer and discard pending long presses.  A PORTRAIT
 * pointer emits HOLD_RELEASE so a camera follow cannot stay latched. */
void wc3_direct_touch_reset(Wc3DirectTouch *dt);

/*
 * Emit long presses whose deadline has expired.  Returns the number emitted.
 * Idempotent for one pointer: the long press fires once and the later release
 * does not also become a tap.
 */
int wc3_direct_touch_tick(Wc3DirectTouch *dt, uint64_t now_ms);

/* Earliest long-press deadline, or 0 when none is pending. */
uint64_t wc3_direct_touch_next_deadline_ms(const Wc3DirectTouch *dt);

/* Number of active pointers. */
int wc3_direct_touch_active_count(const Wc3DirectTouch *dt);

/* Copy one active pointer's last position.  Returns 1 on success. */
int wc3_direct_touch_pointer_at(const Wc3DirectTouch *dt, int index,
                                int *pointer_id, float *x, float *y);

/*
 * Forward one raw pointer phase to the guest sink.  Native UI touches use
 * this instead of the semantic recognizer: the guest-side service posts the
 * same Win32 mouse messages a real mouse produces, so command-card clicks,
 * menus and targeting/placement keep their native behaviour.  A minimap drag
 * uses this path too, but the controls adapter clamps its phases to the
 * published minimap rectangle so leaving the minimap does not end the drag.
 * The portrait does not use this path; it has its own FOLLOW_START/
 * HOLD_RELEASE events.  x,y are surface pixels, like the recognizer input.
 */
void wc3_direct_touch_emit_pointer(Wc3DirectTouch *dt,
                                   Wc3DirectTouchKind kind, int pointer_id,
                                   float x, float y, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* WC3_DIRECT_TOUCH_H */
