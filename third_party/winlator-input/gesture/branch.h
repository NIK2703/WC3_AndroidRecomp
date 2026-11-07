#ifndef GESTURE_BRANCH_H
#define GESTURE_BRANCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include "core/processor.h"
#include "types.h"

void gesture_branch(TouchFinger* f, GestureFingerCtx* ctx,
                    TouchActionResult* restrict result, uint64_t now_tick,
                    GestureProcessEvent event,
                    float dx, float dy);

MoveOutput resolve_move(const MoveInput* in);
void apply_move(const MoveOutput* out, TouchFinger* f,
                GestureFingerCtx* ctx, TouchActionResult* restrict result);

#ifdef __cplusplus
}
#endif

#endif
