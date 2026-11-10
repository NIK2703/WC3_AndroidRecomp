/*
 * crt_string_hle.h - target-side MSVCR80 string/memory/ctype subset.
 *
 * These functions are byte-oriented and do not depend on the CRT heap except
 * _strdup, which uses the shim's guest allocator.  wcsrchr/wcsstr operate on
 * 16-bit Windows code units and stay in this slice; the remaining wide-
 * character conversion stays in the as-built HLE.
 */
#ifndef WC3_HLE_MSVCR80_STRING_H
#define WC3_HLE_MSVCR80_STRING_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of rows installed, or -1 if a row is missing. */
int wc3_hle_msvcr80_string_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_MSVCR80_STRING_H */
