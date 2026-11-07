#ifndef PROFILE_UTIL_H
#define PROFILE_UTIL_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// Safe string copy with NUL termination
static inline void str_cpy(char* dst, const char* src, size_t dst_size) {
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

// Clamp int to [min, max]
static inline int int_clamp(int val, int min, int max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

// Clamp float to [min, max]
static inline float float_clamp(float val, float min, float max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

// Read entire file into allocated buffer. Caller must free() the buffer.
// Returns NULL on failure. Sets *out_size to file size.
static inline char* file_read_all(const char* path, size_t* out_size) {
    if (!path || !out_size) return NULL;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read] = '\0';
    *out_size = read;
    return buf;
}

#ifdef __cplusplus
}
#endif

#endif /* PROFILE_UTIL_H */
