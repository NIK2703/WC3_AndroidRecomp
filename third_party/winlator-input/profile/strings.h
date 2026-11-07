#ifndef PROFILE_STRINGS_H
#define PROFILE_STRINGS_H

#ifdef __cplusplus
extern "C" {
#endif

// Enum-to-string conversions for profile types
const char* profile_element_type_str(int type);
const char* profile_element_shape_str(int shape);
const char* profile_range_str(int range);
const char* profile_mouse_mode_str(int mode);
const char* profile_drag_mode_str(int mode);
const char* profile_activation_mode_str(int mode);

// String-to-enum conversions
int profile_element_type_from_str(const char* s);
int profile_element_shape_from_str(const char* s);
int profile_range_from_str(const char* s);
int profile_mouse_mode_from_str(const char* s);
int profile_drag_mode_from_str(const char* s);
int profile_activation_mode_from_str(const char* s);

#ifdef __cplusplus
}
#endif

#endif /* PROFILE_STRINGS_H */
