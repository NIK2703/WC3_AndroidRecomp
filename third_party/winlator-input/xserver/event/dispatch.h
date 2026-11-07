#ifndef XS_DISPATCH_H
#define XS_DISPATCH_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

// --- X11 event types for ring buffer ---
#define XS_EVT_POINTER_MOVE         0
#define XS_EVT_POINTER_BUTTON_PRESS 1
#define XS_EVT_POINTER_BUTTON_RELEASE 2
#define XS_EVT_KEY_PRESS            3
#define XS_EVT_KEY_RELEASE          4
#define XS_EVT_SCROLL               5
#define XS_EVT_CURSOR_SPEED         6
#define XS_EVT_MAPPING_NOTIFY       7
#define XS_EVT_POINTER_MOVE_DELTA   8
#define XS_EVT_COUNT                9

// --- SPSC lock-free ring buffer ---
// Single producer (touch thread, C), single consumer (frame thread, Java drain)

#define XS_RING_SIZE 256  // Must be power of 2
#define XS_RING_MASK (XS_RING_SIZE - 1)

typedef struct {
    uint8_t  type;
    int16_t  a0;
    int16_t  a1;
} X11Event;

typedef struct {
    X11Event events[XS_RING_SIZE];
    _Alignas(64) atomic_uint head;  // written by producer (C)
    _Alignas(64) atomic_uint tail;  // written by consumer (Java via JNI)
    uint32_t dropped;               // overflow counter (producer only, no atomics needed)
} X11EventRing;

static inline void xs_ring_init(X11EventRing* r) {
    atomic_store_explicit(&r->head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
    r->dropped = 0;
}

static inline bool xs_ring_push(X11EventRing* r, uint8_t type, int16_t a0, int16_t a1) {
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    uint32_t next = (h + 1) & XS_RING_MASK;
    if (__builtin_expect(next == t, 0)) {
        r->dropped++;
        return false;
    }
    r->events[h].type = type;
    r->events[h].a0 = a0;
    r->events[h].a1 = a1;
    atomic_store_explicit(&r->head, next, memory_order_release);
    return true;
}

static inline bool xs_ring_pop(X11EventRing* r, X11Event* out) {
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    if (__builtin_expect(t == h, 0)) return false;
    *out = r->events[t];
    atomic_store_explicit(&r->tail, (t + 1) & XS_RING_MASK, memory_order_release);
    return true;
}

static inline int xs_ring_drain(X11EventRing* r, X11Event* out, int max) {
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    uint32_t count = (h - t) & XS_RING_MASK;
    if (count > (uint32_t)max) count = (uint32_t)max;
    if (count > 0) {
        uint32_t t_masked = t & XS_RING_MASK;
        if (t_masked + count <= XS_RING_SIZE) {
            memcpy(out, &r->events[t_masked], count * sizeof(X11Event));
        } else {
            uint32_t first = XS_RING_SIZE - t_masked;
            memcpy(out, &r->events[t_masked], first * sizeof(X11Event));
            memcpy(out + first, r->events, (count - first) * sizeof(X11Event));
        }
        atomic_store_explicit(&r->tail, (t + count) & XS_RING_MASK, memory_order_release);
    }
    return (int)count;
}

static inline int xs_ring_count(X11EventRing* r) {
    uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    return (h - t) & XS_RING_MASK;
}

#endif // XS_DISPATCH_H
