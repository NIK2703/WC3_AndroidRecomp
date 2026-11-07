#ifndef MOUSE_MODE_H
#define MOUSE_MODE_H

#include <stdbool.h>

typedef enum {
    TOUCHPAD,
    TOUCHSCREEN
} MouseMode;

// Global relative mouse movement flag.
// When true: all pointer deltas are sent via UDP (MF_MOVE without ABSOLUTE)
// directly to Wine's winhandler, bypassing the X11 event ring.
// When false: normal absolute X11 pointer events are used.
extern volatile bool g_relative_mouse;

#endif
