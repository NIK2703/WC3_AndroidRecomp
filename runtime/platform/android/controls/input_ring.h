#ifndef WC3_INPUT_RING_H
#define WC3_INPUT_RING_H

/*
 * Fixed touch-event ring between the Android main/UI thread and the C input
 * thread.  This file deliberately has no Android types: the GUI callback
 * copies a small plain struct and wakes the input thread.
 *
 * Moves are allowed to fill only CAPACITY - CONTROL_RESERVE slots.  A burst of
 * MOVE events therefore cannot make the ring drop a later DOWN/UP.  Controls
 * may use the reserve and are only dropped if the entire ring is full of
 * controls; that path raises a fault so the adapter can cancel held actions.
 */

#include <pthread.h>
#include <stdint.h>

#include "controls.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WC3_INPUT_RING_CAPACITY 256
#define WC3_INPUT_RING_CONTROL_RESERVE 64
#define WC3_INPUT_RING_MOVE_LIMIT \
    (WC3_INPUT_RING_CAPACITY - WC3_INPUT_RING_CONTROL_RESERVE)

typedef struct {
    uint64_t event_time_ms;
    int32_t pointer_id;
    float x;
    float y;
    uint8_t phase;          /* Wc3TouchPhase */
    uint8_t _pad[3];
} Wc3TouchEvent;

typedef struct {
    uint64_t pushed;
    uint64_t popped;
    uint64_t dropped_moves;
    uint64_t dropped_controls;
    uint64_t faults;
    int count;
} Wc3InputRingStats;

typedef struct {
    Wc3TouchEvent events[WC3_INPUT_RING_CAPACITY];
    int head;
    int tail;
    int count;
    pthread_mutex_t lock;
    uint64_t pushed;
    uint64_t popped;
    uint64_t dropped_moves;
    uint64_t dropped_controls;
    uint64_t faults;
} Wc3InputRing;

void input_ring_init(Wc3InputRing *ring);
void input_ring_destroy(Wc3InputRing *ring);

/* 0 = accepted, 1 = a MOVE was dropped, -1 = a control event was dropped or
 * the arguments were invalid.  A dropped control also raises a fault. */
int input_ring_push(Wc3InputRing *ring, const Wc3TouchEvent *event);

/* Copies at most max events in FIFO order and returns the number copied. */
int input_ring_pop_batch(Wc3InputRing *ring, Wc3TouchEvent *out, int max);

/* 1 when no event is queued.  The input thread uses this as one of the exact
 * preconditions for changing the overlay profile. */
int input_ring_is_empty(Wc3InputRing *ring);

void input_ring_clear(Wc3InputRing *ring);
void input_ring_note_fault(Wc3InputRing *ring);
void input_ring_get_stats(Wc3InputRing *ring, Wc3InputRingStats *out);

#ifdef __cplusplus
}
#endif

#endif /* WC3_INPUT_RING_H */
