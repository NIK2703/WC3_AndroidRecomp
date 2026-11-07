#include "core/internal.h"


void element_stick_down(TouchElement* e, int ptr_id, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    (void)ptr_id;
    memset(e->petal_active, 0, sizeof(e->petal_active));
    // Java ControlElement.handleTouchDown delegates immediately to handleTouchMove,
    // sending axis values on touch-down (C was deferring to first ACTION_MOVE)
    element_stick_move(e, x, y, now_tick, result);
}

void element_stick_move(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    (void)now_tick;
    if (!(e->flags & TOUCH_BIT_CACHED_HAS_ANY_BINDING) && !(e->flags & TOUCH_BIT_CACHED_BIND0_IS_GAMEPAD)) return;
    TouchProcessorState* s = &g_state;
    float radius = s->snapping_size * STICK_RADIUS_MULTIPLIER * e->scale;
    if (radius < NEAR_ZERO_THRESHOLD) return;
    float dx = x - e->x;
    float dy = y - e->y;
    float dist_sq = dx*dx + dy*dy;
    if (dist_sq < NEAR_ZERO_THRESHOLD * NEAR_ZERO_THRESHOLD) return;
    float nx, ny;
    float radius_sq = radius * radius;
    if (dist_sq > radius_sq) {
        float inv_dist = 1.0f / sqrtf(dist_sq);
        nx = dx * inv_dist;
        ny = dy * inv_dist;
        e->visual_x = e->x + radius * nx;
        e->visual_y = e->y + radius * ny;
    } else {
        float inv_radius = 1.0f / radius;
        nx = dx * inv_radius;
        ny = dy * inv_radius;
        e->visual_x = e->x + dx;
        e->visual_y = e->y + dy;
        mark_element_dirty(e);
    }

    bool is_gamepad = (e->flags & TOUCH_BIT_CACHED_BIND0_IS_GAMEPAD) != 0;
    if (is_gamepad) {
        e->stick_value_x = nx;
        e->stick_value_y = ny;
        emit_gamepad_axis(e, nx, ny, result);
    } else {
        element_set_petals(e, nx, ny, STICK_DEAD_ZONE, result);
    }
}

void element_stick_up(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    (void)x; (void)y; (void)now_tick;
    e->visual_x = e->x;
    e->visual_y = e->y;
    mark_element_dirty(e);
    e->stick_value_x = 0;
    e->stick_value_y = 0;
    release_element_petals_and_axis(e, result);
}
