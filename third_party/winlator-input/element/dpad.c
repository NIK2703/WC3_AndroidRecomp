#include "core/internal.h"


static inline void dpad_update(TouchElement* e, float x, float y, TouchActionResult* restrict result) {
    if (!(e->flags & TOUCH_BIT_CACHED_HAS_ANY_BINDING)) return;
    float radius = e->hw;
    if (__builtin_expect(radius < NEAR_ZERO_THRESHOLD, 0)) return;
    float dx = x - e->x;
    float dy = y - e->y;
    if (__builtin_expect(dx*dx + dy*dy < NEAR_ZERO_THRESHOLD * NEAR_ZERO_THRESHOLD, 0)) return;
    float inv_radius = 1.0f / radius;
    float nx = dx * inv_radius;
    float ny = dy * inv_radius;
    element_set_petals(e, nx, ny, DPAD_DEAD_ZONE, result);
}

void element_dpad_down(TouchElement* e, int ptr_id, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    (void)ptr_id; (void)now_tick;
    dpad_update(e, x, y, result);
}

void element_dpad_move(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    (void)now_tick;
    dpad_update(e, x, y, result);
}

void element_dpad_up(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    (void)x; (void)y; (void)now_tick;
    if (!(e->flags & TOUCH_BIT_CACHED_HAS_ANY_BINDING)) return;
    release_element_petals(e, result);
}
