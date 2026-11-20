/*
 * direct_touch_event.h - semantic direct-touch events across the host/HLE
 * boundary.
 *
 * The overlay recognizer runs on the Android input thread.  It never calls
 * guest code: it publishes one fixed-size, pointer-free event here, and the
 * main guest thread pops it at the message-pump boundary and resolves it
 * against the live game objects.  Keep this type POD and free of guest
 * addresses; the queue must survive a guest map change.
 */
#ifndef WC3_DIRECT_TOUCH_EVENT_H
#define WC3_DIRECT_TOUCH_EVENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WC3_DIRECT_TOUCH_NONE = 0,
    WC3_DIRECT_TOUCH_TAP = 1,
    WC3_DIRECT_TOUCH_LONG_PRESS = 2
} Wc3DirectTouchKind;

typedef struct {
    uint32_t kind;       /* Wc3DirectTouchKind */
    uint32_t pointer_id; /* source pointer, diagnostics only */
    float x;             /* normalised surface x, 0..1, top-left origin */
    float y;             /* normalised surface y, 0..1, top-left origin */
    uint64_t time_ms;    /* CLOCK_MONOTONIC at recognition */
} Wc3DirectTouchEvent;

/* The recognizer publishes through this sink.  It runs on the Android input
 * thread and must not call guest code. */
typedef void (*Wc3DirectTouchSink)(const Wc3DirectTouchEvent *event,
                                   void *user);

/* 64 events: a burst of taps cannot overflow, and the guest services at most
 * four discrete events per message-pump call.  The producer is the single
 * Android input thread; the consumer is the main guest thread. */
#define WC3_DIRECT_TOUCH_QUEUE_CAP 64u

/* Producer wake.  Called after a successful push with no queue lock held, so
 * the HLE can wake its blocking GetMessageA wait without this module
 * depending on the Win32 queue. */
typedef void (*Wc3DirectTouchWake)(void *user);
void wc3_direct_touch_set_wake(Wc3DirectTouchWake wake, void *user);

/*
 * Producer side.  Returns 1 when queued, 0 when the queue is full.  Called
 * from the Android input thread; the implementation is lock-free.
 */
int wc3_direct_touch_push(const Wc3DirectTouchEvent *event);

/* Consumer side.  Returns 1 when an event was removed. */
int wc3_direct_touch_pop(Wc3DirectTouchEvent *event);

/* Number of queued events.  Safe for a wake-up predicate on either side. */
uint32_t wc3_direct_touch_pending(void);

/* Drop all queued events.  Used when the game leaves the in-game UI or the
 * guest UI singleton changes, so a stale tap cannot hit the next map. */
void wc3_direct_touch_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_DIRECT_TOUCH_EVENT_H */
