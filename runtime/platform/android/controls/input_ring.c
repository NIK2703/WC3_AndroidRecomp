#include "input_ring.h"

#include <string.h>

static int event_is_move(const Wc3TouchEvent *event)
{
    return event && event->phase == (uint8_t)WC3_TOUCH_MOVE;
}

void input_ring_init(Wc3InputRing *ring)
{
    if (!ring) return;
    memset(ring, 0, sizeof(*ring));
    pthread_mutex_init(&ring->lock, NULL);
}

void input_ring_destroy(Wc3InputRing *ring)
{
    if (!ring) return;
    pthread_mutex_destroy(&ring->lock);
}

int input_ring_push(Wc3InputRing *ring, const Wc3TouchEvent *event)
{
    if (!ring || !event) return -1;
    if (event->phase > (uint8_t)WC3_TOUCH_CANCEL) return -1;

    pthread_mutex_lock(&ring->lock);
    if (ring->count >= WC3_INPUT_RING_CAPACITY) {
        if (event_is_move(event)) {
            ring->dropped_moves++;
            pthread_mutex_unlock(&ring->lock);
            return 1;
        }
        ring->dropped_controls++;
        ring->faults++;
        pthread_mutex_unlock(&ring->lock);
        return -1;
    }
    if (event_is_move(event) && ring->count >= WC3_INPUT_RING_MOVE_LIMIT) {
        ring->dropped_moves++;
        pthread_mutex_unlock(&ring->lock);
        return 1;
    }

    ring->events[ring->tail] = *event;
    ring->tail = (ring->tail + 1) % WC3_INPUT_RING_CAPACITY;
    ring->count++;
    ring->pushed++;
    pthread_mutex_unlock(&ring->lock);
    return 0;
}

int input_ring_pop_batch(Wc3InputRing *ring, Wc3TouchEvent *out, int max)
{
    if (!ring || !out || max <= 0) return 0;

    pthread_mutex_lock(&ring->lock);
    int n = ring->count < max ? ring->count : max;
    for (int i = 0; i < n; i++)
        out[i] = ring->events[(ring->head + i) % WC3_INPUT_RING_CAPACITY];
    ring->head = (ring->head + n) % WC3_INPUT_RING_CAPACITY;
    ring->count -= n;
    ring->popped += (uint64_t)n;
    pthread_mutex_unlock(&ring->lock);
    return n;
}

void input_ring_clear(Wc3InputRing *ring)
{
    if (!ring) return;
    pthread_mutex_lock(&ring->lock);
    ring->head = 0;
    ring->tail = 0;
    ring->count = 0;
    pthread_mutex_unlock(&ring->lock);
}

void input_ring_note_fault(Wc3InputRing *ring)
{
    if (!ring) return;
    pthread_mutex_lock(&ring->lock);
    ring->faults++;
    pthread_mutex_unlock(&ring->lock);
}

void input_ring_get_stats(Wc3InputRing *ring, Wc3InputRingStats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!ring) return;
    pthread_mutex_lock(&ring->lock);
    out->pushed = ring->pushed;
    out->popped = ring->popped;
    out->dropped_moves = ring->dropped_moves;
    out->dropped_controls = ring->dropped_controls;
    out->faults = ring->faults;
    out->count = ring->count;
    pthread_mutex_unlock(&ring->lock);
}
