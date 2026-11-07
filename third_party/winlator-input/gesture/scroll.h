#ifndef GESTURE_SCROLL_H
#define GESTURE_SCROLL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "core/processor.h"

void scroll_mode_process(TouchFinger* f, TouchActionResult* restrict result);

#ifdef __cplusplus
}
#endif

#endif
