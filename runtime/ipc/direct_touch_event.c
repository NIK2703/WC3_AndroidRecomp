/*
 * direct_touch_event.c - single-producer/single-consumer direct-touch queue.
 *
 * The recognizer on the Android input thread is the only producer and the
 * main guest thread is the only consumer.  Head and tail are monotonically
 * increasing sequence numbers; the low bits select the slot.  A release
 * store of the sequence publishes the event, and an acquire load of the
 * opposite sequence makes the slot contents visible.  No host lock is taken
 * here: the input thread must never block behind the guest.
 */
#include "direct_touch_event.h"

#define WC3_DIRECT_TOUCH_QUEUE_MASK (WC3_DIRECT_TOUCH_QUEUE_CAP - 1u)

_Static_assert((WC3_DIRECT_TOUCH_QUEUE_CAP & WC3_DIRECT_TOUCH_QUEUE_MASK) == 0,
               "direct-touch queue capacity must be a power of two");

static Wc3DirectTouchEvent g_events[WC3_DIRECT_TOUCH_QUEUE_CAP];
static uint32_t g_head; /* producer sequence */
static uint32_t g_tail; /* consumer sequence */

static Wc3DirectTouchWake g_wake;
static void *g_wake_user;

void wc3_direct_touch_set_wake(Wc3DirectTouchWake wake, void *user)
{
    __atomic_store_n(&g_wake_user, user, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wake, wake, __ATOMIC_RELEASE);
}

int wc3_direct_touch_push(const Wc3DirectTouchEvent *event)
{
    if (!event) return 0;

    const uint32_t head = __atomic_load_n(&g_head, __ATOMIC_RELAXED);
    const uint32_t tail = __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE);
    if ((uint32_t)(head - tail) >= WC3_DIRECT_TOUCH_QUEUE_CAP) return 0;

    g_events[head & WC3_DIRECT_TOUCH_QUEUE_MASK] = *event;
    __atomic_store_n(&g_head, head + 1u, __ATOMIC_RELEASE);

    Wc3DirectTouchWake wake = __atomic_load_n(&g_wake, __ATOMIC_ACQUIRE);
    if (wake) wake(__atomic_load_n(&g_wake_user, __ATOMIC_RELAXED));
    return 1;
}

int wc3_direct_touch_pop(Wc3DirectTouchEvent *event)
{
    if (!event) return 0;

    const uint32_t tail = __atomic_load_n(&g_tail, __ATOMIC_RELAXED);
    const uint32_t head = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
    if (tail == head) return 0;

    *event = g_events[tail & WC3_DIRECT_TOUCH_QUEUE_MASK];
    __atomic_store_n(&g_tail, tail + 1u, __ATOMIC_RELEASE);
    return 1;
}

uint32_t wc3_direct_touch_pending(void)
{
    const uint32_t head = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
    const uint32_t tail = __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE);
    return head - tail;
}

void wc3_direct_touch_clear(void)
{
    const uint32_t head = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
    __atomic_store_n(&g_tail, head, __ATOMIC_RELEASE);
}
