#ifndef PROFILE_LOADER_H
#define PROFILE_LOADER_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Load a profile from a JSON string. Returns 0 on success, -1 on error.
// On success, out_profile is populated with parsed data.
int profile_load_from_string(const char* json_str, ProfileData* out_profile);

// Load a profile from a file path. Returns 0 on success, -1 on error.
int profile_load_from_file(const char* file_path, ProfileData* out_profile);

// Save a profile to a JSON string. Returns allocated string on success, NULL on error.
// Caller must free() the returned string.
char* profile_save_to_string(const ProfileData* profile);

// Save a profile to a file. Returns 0 on success, -1 on error.
int profile_save_to_file(const char* file_path, const ProfileData* profile);

// Get a human-readable error message for the last profile load error.
const char* profile_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* PROFILE_LOADER_H */
