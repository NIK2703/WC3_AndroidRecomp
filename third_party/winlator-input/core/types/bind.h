#ifndef BIND_H
#define BIND_H

#include <stdint.h>

#define MAX_DEFERRED_BINDINGS 8

typedef enum {
    NONE = 0,

    MOUSE_LEFT_BUTTON, MOUSE_RIGHT_BUTTON, MOUSE_MIDDLE_BUTTON,
    MOUSE_BUTTON4, MOUSE_BUTTON5,

    MOUSE_SCROLL_UP, MOUSE_SCROLL_DOWN,

    MOUSE_MOVE_LEFT, MOUSE_MOVE_RIGHT, MOUSE_MOVE_UP, MOUSE_MOVE_DOWN,

    KEYBOARD_FIRST = 0x100,
    KEYBOARD_LAST = 0x400,

    GAMEPAD_BASE = 0x500,
} Bind;

typedef struct {
    uint16_t type;
    uint16_t keycode;
    uint8_t modifiers;
    uint8_t flags;
    uint16_t auto_repeat_interval_ticks;
    uint16_t bind_delay_ticks;
} TouchBinding;

#define TOUCH_BINDING_FLAG_TOGGLE     (1u << 0)
#define TOUCH_BINDING_FLAG_AUTO_REPEAT (1u << 1)
#define TOUCH_BINDING_FLAG_LAST_IN_PACKAGE (1u << 2)

#endif
