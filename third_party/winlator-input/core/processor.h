// Re-export header: all types are now in core/types/*.h.
// This file exists for backward compatibility — new code should include
// the specific type header(s) it needs directly from core/types/.

#ifndef TOUCH_PROCESSOR_H
#define TOUCH_PROCESSOR_H

#include "types/gesture_type.h"
#include "types/element_type.h"
#include "types/touch_activation_mode.h"
#include "types/mouse_mode.h"
#include "types/bind.h"
#include "types/touch_element.h"
#include "types/touch_config.h"
#include "types/touch_action.h"
#include "types/touch_finger.h"

// C++ lacks 'restrict' keyword — use compiler extension for compatibility
#ifdef __cplusplus
#ifndef restrict
#define restrict __restrict__
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// --- Public API ---

// Initialize processor with configuration
void touch_processor_init(const TouchProcessorConfig* config);

// Update configuration at runtime
void touch_processor_update_config(const TouchProcessorConfig* config);

// Set control elements (buttons, dpads, sticks, etc.)
void touch_processor_set_elements(const TouchElement* elements, int count);

// Set snapping size for element layout
void touch_processor_set_snapping_size(float size);

// Set resolution scale for coordinate transforms
void touch_processor_set_resolution_scale(float scale);

// Enable/disable simulated touch screen mode
void touch_processor_set_sim_touch_screen(bool enabled);

// Set xform scale for trackpad delta computation (view-pixels to Wine-screen-pixels)
void touch_processor_set_xform_scale(float scale_x, float scale_y);

// --- Touch event input ---

// Finger down
void touch_processor_on_finger_down(TouchActionResult* restrict result, int ptr_id, float x, float y, uint64_t now_tick);

// Finger move
void touch_processor_on_finger_move(TouchActionResult* restrict result, int ptr_id, float x, float y, uint64_t now_tick);

// Finger up
void touch_processor_on_finger_up(TouchActionResult* restrict result, int ptr_id, float x, float y, uint64_t now_tick);

// Timer tick — called periodically for long-press, double-tap timeouts
void touch_processor_tick(TouchActionResult* restrict result, uint64_t now_tick);

// Sentinel returned when the engine has no future tick work.
#define TOUCH_PROCESSOR_DEADLINE_NONE UINT64_MAX

// Absolute engine tick at which touch_processor_tick() must next run, or
// TOUCH_PROCESSOR_DEADLINE_NONE.  This is an exact timer query: an active
// finger alone is not a reason to tick.  A returned tick may already be due;
// the host must run one tick and query again.
uint64_t touch_processor_next_deadline_tick(void);

// Nonzero while touch_processor_next_deadline_tick() has a deadline.  Kept as
// a convenience for callers that do not need the timestamp.
int touch_processor_needs_tick(void);

// Full cancel: releases ALL held keys, element bindings, and clears all state.
void touch_processor_cancel_all(TouchActionResult* restrict result);

// --- X11 event ring buffer (SPSC: C producer → Java consumer) ---
#include "xserver/event/dispatch.h"

// --- Element visual update ---
// Regenerate visual state for a single element.
void touch_processor_regenerate_element(int elemIndex);

#ifdef __cplusplus
}
#endif

#endif // TOUCH_PROCESSOR_H
