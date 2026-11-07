#ifndef PROFILE_CONVERT_H
#define PROFILE_CONVERT_H

#include "types.h"
#include "core/processor.h"

#ifdef __cplusplus
extern "C" {
#endif

// Convert a ProfileData to TouchProcessorConfig
void profile_to_config(const ProfileData* profile, int screen_w, int screen_h,
                       float global_cursor_speed, TouchProcessorConfig* out_config);

// Convert profile elements to TouchElement array
// screen_w/screen_h: pixel dimensions for x/y coordinate scaling from normalized to pixel
// Returns number of elements written (0 on error)
int profile_to_elements(const ProfileData* profile, int screen_w, int screen_h,
                        TouchElement* out_elements, int max_elements);

// One-shot: load profile from file and init touch processor
// Tick interval is read from profile (touchTickRate), default 16ms.
// Returns 0 on success, -1 on error
int profile_load_and_init(const char* file_path, int screen_w, int screen_h, float cursor_speed);

// Non-destructive: load profile and update config at runtime
// Preserves active touch state, xform, and elements.
// Tick interval is read from profile (touchTickRate), default 16ms.
// Returns 0 on success, -1 on error
int profile_load_and_update_config(const char* file_path, int screen_w, int screen_h, float cursor_speed);

#ifdef __cplusplus
}
#endif

#endif /* PROFILE_CONVERT_H */
