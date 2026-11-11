#ifndef WC3_TIMER_REGISTRY_H
#define WC3_TIMER_REGISTRY_H

#include <stdint.h>

/*
 * Central timer registry for the detached touch engine.
 *
 * Timer state still lives in the existing engine structures.  The registry
 * keeps one cached absolute-tick deadline per timer group and recomputes a
 * group only after a public engine entry point marks the cache dirty.  The
 * host-facing query is O(number of groups), not O(number of elements).
 *
 * All functions are called with the touch processor state lock held (or
 * during single-threaded initialization).  The registry owns no locks.
 */

enum {
    WC3_TIMER_GROUP_SCHEDULED = 0,
    WC3_TIMER_GROUP_DELAYED,
    WC3_TIMER_GROUP_GESTURE,
    WC3_TIMER_GROUP_BUTTON,
    WC3_TIMER_GROUP_RANGE,
    WC3_TIMER_GROUP_MOUSE,
    WC3_TIMER_GROUP_GESTURE_REPEAT,
    WC3_TIMER_GROUP_FALLBACK,
    WC3_TIMER_GROUP_COUNT
};

/* Mark every group for lazy recomputation.  Called on any public engine
 * mutation that can arm, cancel, or re-time a timer. */
void wc3_timer_registry_invalidate(void);

/* Earliest cached deadline in engine ticks, or UINT64_MAX when no timer is
 * pending.  A deadline may already be due; callers run one tick and query
 * again, exactly like the previous state-scan implementation. */
uint64_t wc3_timer_registry_next_deadline(void);

#endif /* WC3_TIMER_REGISTRY_H */
