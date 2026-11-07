#include "core/internal.h"
#include <android/log.h>

#define LOG_TAG "EIC2_TPAD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)


#define BIND_UP 0
#define BIND_RIGHT 1
#define BIND_DOWN 2
#define BIND_LEFT 3

// Hardcoded Java constants for trackpad acceleration (ControlElement.TRACKPAD_ACCELERATION_THRESHOLD=4)
#define TP_ACCEL_THRESHOLD 4.0f
#define TRACKPAD_NORMALIZATION_FACTOR 0.05f
#define TRACKPAD_DEADZONE_PX 1.0f // 1.0f in scaled (Wine screen) space; on high-DPI screens this maps to <1 physical pixel
#define TRACKPAD_DEADZONE_SQ (TRACKPAD_DEADZONE_PX * TRACKPAD_DEADZONE_PX)

static inline float normalize_with_sign(float value, float factor) {
    float n = fminf(1.0f, fabsf(value * factor));
    return (value < 0) ? -n : n;
}

static inline void trackpad_cursor_accel(float dx, float dy, float* out_vx, float* out_vy) {
    float abs_dx = fabsf(dx), abs_dy = fabsf(dy);
    float threshold = (float)g_state.cfg.cursor_acceleration_threshold;
    float factor = g_state.cfg.cursor_acceleration_factor;
    float clamp_factor = 1.0f / threshold;
    *out_vx = dx;
    *out_vy = dy;
    if (abs_dx > threshold) {
        float mult = 1.0f + (factor - 1.0f) * fminf((abs_dx - threshold) * clamp_factor, 1.0f);
        *out_vx = dx * mult;
    }
    if (abs_dy > threshold) {
        float mult = 1.0f + (factor - 1.0f) * fminf((abs_dy - threshold) * clamp_factor, 1.0f);
        *out_vy = dy * mult;
    }
}

static inline void trackpad_move_gamepad(TouchElement* e, float dx, float dy, TouchActionResult* restrict result) {
    // Java: TRACKPAD_ACCELERATION_THRESHOLD=4, STICK_SENSITIVITY=2.0f
    float abs_dx = fabsf(dx), abs_dy = fabsf(dy);
    float value_x = dx, value_y = dy;
    if (abs_dx > TP_ACCEL_THRESHOLD) {
        float mult = 1.0f + (STICK_SENSITIVITY - 1.0f) * (abs_dx - TP_ACCEL_THRESHOLD) / (abs_dx + TP_ACCEL_THRESHOLD);
        value_x = dx * mult;
    }
    if (abs_dy > TP_ACCEL_THRESHOLD) {
        float mult = 1.0f + (STICK_SENSITIVITY - 1.0f) * (abs_dy - TP_ACCEL_THRESHOLD) / (abs_dy + TP_ACCEL_THRESHOLD);
        value_y = dy * mult;
    }

    // Normalize by TRACKPAD_NORMALIZATION_FACTOR and clamp to [-1, 1]
    float nx = normalize_with_sign(value_x, TRACKPAD_NORMALIZATION_FACTOR);
    float ny = normalize_with_sign(value_y, TRACKPAD_NORMALIZATION_FACTOR);


    // Java CubicBezierInterpolator with control points (0.075, 0.95, 0.45, 0.95)
    // Use cubic bezier binary search for x -> y mapping
    float interp_x = cubic_bezier_interpolate_trackpad(nx);
    float interp_y = cubic_bezier_interpolate_trackpad(ny);

    emit_gamepad_axis(e, interp_x, interp_y, result);

    e->trackpad_vel_x = interp_x;
    e->trackpad_vel_y = interp_y;
}

static inline void trackpad_move_cursor(TouchElement* e, float dx, float dy, TouchActionResult* restrict result) {
    float value_x, value_y;
    trackpad_cursor_accel(dx, dy, &value_x, &value_y);

    LOGI("MOVE-CURSOR raw_dx=%.1f raw_dy=%.1f accel_dx=%.1f accel_dy=%.1f", dx, dy, value_x, value_y);

    int cursor_dx = 0, cursor_dy = 0;
    if (e->flags & TOUCH_BIT_CACHED_HAS_ANY_BINDING) {
        uint8_t mask = e->cached_petal_mask;
        while (mask) {
            int i = __builtin_ctz(mask);
            mask &= mask - 1;
            const TouchBinding* b = &e->bindings[i];
            if (!is_mouse_move_binding(b)) continue;
            float value = (i == BIND_RIGHT || i == BIND_LEFT) ? value_x : value_y;
            if (value > 32767.0f) value = 32767.0f;
            if (value < -32768.0f) value = -32768.0f;
            int delta = lroundf(value);
            if (i == BIND_RIGHT || i == BIND_LEFT) cursor_dx += delta;
            else cursor_dy += delta;
        }

        element_set_petals(e, dx, dy, TRACKPAD_MIN_SPEED, result);
    }

    if (cursor_dx != 0 || cursor_dy != 0) {
        LOGI("DELTA-OUT dx=%d dy=%d", cursor_dx, cursor_dy);
        add_action(result, ACT_POINTER_MOVE_DELTA, cursor_dx, cursor_dy, 0);
    } else {
        LOGI("DELTA-OUT zero (deadzone or no bindings)");
    }
}

void element_trackpad_down(TouchElement* e, int ptr_id, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    (void)ptr_id; (void)result;
    e->trackpad_last_x = x;
    e->trackpad_last_y = y;
    e->trackpad_last_tick = now_tick;
    e->petal_active[0] = 0; e->petal_active[1] = 0; e->petal_active[2] = 0; e->petal_active[3] = 0;
    e->trackpad_vel_x = 0;
    e->trackpad_vel_y = 0;
}

void element_trackpad_move(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    (void)now_tick;
    TouchProcessorState* s = &g_state;
    float dx = x - e->trackpad_last_x;
    float dy = y - e->trackpad_last_y;

    LOGI("MOVE-RAW x=%.1f y=%.1f last_x=%.1f last_y=%.1f raw_dx=%.1f raw_dy=%.1f",
         x, y, e->trackpad_last_x, e->trackpad_last_y, dx, dy);

    // Apply xform scale: maps view-pixels to Wine-screen-pixels (Java computeDeltaPoint)
    dx *= s->cfg.xform_scale_x;
    dy *= s->cfg.xform_scale_y;

    LOGI("MOVE-SCALED dx=%.1f dy=%.1f (xform=%.2f,%.2f)", dx, dy, s->cfg.xform_scale_x, s->cfg.xform_scale_y);

    if (dx*dx + dy*dy < TRACKPAD_DEADZONE_SQ) {
        LOGI("MOVE-DEADZONE dx=%.1f dy=%.1f sq=%.1f < %.1f", dx, dy, dx*dx+dy*dy, TRACKPAD_DEADZONE_SQ);
        return;
    }


    if (e->flags & TOUCH_BIT_CACHED_BIND0_IS_GAMEPAD)
        trackpad_move_gamepad(e, dx, dy, result);
    else
        trackpad_move_cursor(e, dx, dy, result);

    e->trackpad_last_x = x;
    e->trackpad_last_y = y;
}

void element_trackpad_up(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    (void)x; (void)y; (void)now_tick;
    release_element_petals_and_axis(e, result);
}
