#include "strings.h"
#include <string.h>
#include <strings.h>

const char* profile_element_type_str(int type) {
    switch (type) {
        case 0: return "BUTTON";
        case 1: return "D_PAD";
        case 2: return "RANGE_BUTTON";
        case 3: return "STICK";
        case 4: return "TRACKPAD";
        default: return "BUTTON";
    }
}

const char* profile_element_shape_str(int shape) {
    switch (shape) {
        case 0: return "CIRCLE";
        case 1: return "RECT";
        case 2: return "SQUARE";
        case 3: return "ROUND_RECT";
        default: return "CIRCLE";
    }
}

const char* profile_range_str(int range) {
    switch (range) {
        case 0: return "FROM_A_TO_Z";
        case 1: return "FROM_0_TO_9";
        case 2: return "FROM_F1_TO_F12";
        case 3: return "FROM_NP0_TO_NP9";
        default: return "FROM_A_TO_Z";
    }
}

const char* profile_mouse_mode_str(int mode) {
    switch (mode) {
        case 0: return "TOUCHPAD";
        case 1: return "TOUCHSCREEN";
        default: return "TOUCHPAD";
    }
}

const char* profile_drag_mode_str(int mode) {
    switch (mode) {
        case 0: return "AUTO";
        case 1: return "ALWAYS";
        case 2: return "NONE";
        default: return "AUTO";
    }
}

const char* profile_activation_mode_str(int mode) {
    switch (mode) {
        case 0: return "LOCK";
        case 1: return "TRACK";
        case 2: return "HOVER";
        default: return "LOCK";
    }
}

int profile_element_type_from_str(const char* s) {
    if (strcasecmp(s, "BUTTON") == 0) return 0;
    if (strcasecmp(s, "D_PAD") == 0) return 1;
    if (strcasecmp(s, "RANGE_BUTTON") == 0) return 2;
    if (strcasecmp(s, "STICK") == 0) return 3;
    if (strcasecmp(s, "TRACKPAD") == 0) return 4;
    return 0;
}

int profile_element_shape_from_str(const char* s) {
    if (strcasecmp(s, "CIRCLE") == 0) return 0;
    if (strcasecmp(s, "SQUARE") == 0) return 2;
    if (strcasecmp(s, "RECT") == 0) return 1;
    if (strcasecmp(s, "ROUND_RECT") == 0) return 3;
    return 0;
}

int profile_range_from_str(const char* s) {
    if (strcasecmp(s, "FROM_A_TO_Z") == 0) return 0;
    if (strcasecmp(s, "FROM_0_TO_9") == 0) return 1;
    if (strcasecmp(s, "FROM_F1_TO_F12") == 0) return 2;
    if (strcasecmp(s, "FROM_NP0_TO_NP9") == 0) return 3;
    return 0;
}

int profile_mouse_mode_from_str(const char* s) {
    if (strcasecmp(s, "TOUCHPAD") == 0) return 0;
    if (strcasecmp(s, "TOUCHSCREEN") == 0) return 1;
    return 0;
}

int profile_drag_mode_from_str(const char* s) {
    if (strcasecmp(s, "AUTO") == 0) return 0;
    if (strcasecmp(s, "ALWAYS") == 0) return 1;
    if (strcasecmp(s, "NONE") == 0) return 2;
    return 0;
}

int profile_activation_mode_from_str(const char* s) {
    if (strcasecmp(s, "LOCK") == 0) return 0;
    if (strcasecmp(s, "TRACK") == 0) return 1;
    if (strcasecmp(s, "HOVER") == 0) return 2;
    return 0;
}
