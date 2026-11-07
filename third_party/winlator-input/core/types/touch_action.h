#ifndef TOUCH_ACTION_H
#define TOUCH_ACTION_H

#include <stdint.h>

#define MAX_HELD_ACTIONS 16
#define MAX_TOUCH_RESULT_ACTIONS 64

typedef enum {
    ACT_NONE,
    ACT_POINTER_MOVE, ACT_POINTER_MOVE_DELTA,
    ACT_POINTER_BUTTON_PRESS, ACT_POINTER_BUTTON_RELEASE,
    ACT_KEY_PRESS, ACT_KEY_RELEASE,
    ACT_MOUSE_EVENT,
    ACT_SCROLL,
    ACT_HAPTIC,
    ACT_SET_CURSOR_SPEED,
    ACT_START_MOUSE_MOVE,
    ACT_STOP_MOUSE_MOVE,
    ACT_GAMEPAD_STATE,
    ACT_GAMEPAD_RELEASE,
    ACT_GAMEPAD_AXIS
} ActionType;

typedef struct {
    uint16_t type;
    union {
        struct { int16_t x, y; } pointer_move;
        struct { int16_t dx, dy; } pointer_delta;
        struct { int16_t button; } pointer_button;
        struct { int16_t keycode; bool is_down; } key;
        struct { int16_t flags, dx, dy, wheel; } mouse_event;
        struct { int16_t amount; } scroll;
        struct { int16_t effect; } haptic;
        struct { int16_t speed; } cursor_speed;
        struct { int16_t dx, dy; int16_t hold; } mouse_move;
        struct { int16_t is_left; int16_t axis_x; int16_t axis_y; } gamepad_axis;
    };
} TouchAction;

typedef struct {
    TouchAction actions[MAX_TOUCH_RESULT_ACTIONS];
    uint8_t count;
} TouchActionResult;

#endif
