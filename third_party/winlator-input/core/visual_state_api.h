#ifndef VISUAL_STATE_API_H
#define VISUAL_STATE_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Hot-path getters for overlay renderer.
// All functions are thread-safe and lock-free.
// Intended for use from the Choreographer frame callback (no JNI needed).

// Returns pointer to the visual state buffer (VisualStateEntry[]).
// Buffer is updated by visual_state_flush() — only dirty elements are copied.
const void* visual_state_get_buffer(void);

// Returns current visual generation counter (atomic acquire read).
// Incremented after each visual_state_flush that copies any dirty elements.
uint32_t visual_state_get_gen(void);

// Returns number of configured elements (g_state.element_count).
int visual_state_get_count(void);

// Returns sizeof(VisualStateEntry) — stride for buffer element access.
int visual_state_get_vse_size(void);

#ifdef __cplusplus
}
#endif

#endif // VISUAL_STATE_API_H
