/*
 * crt_string_hle.h - target-side MSVCR80 string/memory/ctype/numeric subset.
 *
 * These functions are byte-oriented and do not depend on the CRT heap except
 * _strdup, which uses the shim's guest allocator.  Formatting, scanning,
 * numeric conversion and qsort stay in the quarantine.
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
