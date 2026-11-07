/*
 * convert_hle.h - target-side MSVCR80 numeric conversion and multibyte ctype.
 *
 * This slice covers the imports that parse text without touching the heap:
 *
 *   - atof, strtol, strtoul: 32-bit MSVC conversion semantics, including
 *     saturation at LONG_MAX/LONG_MIN and ULONG_MAX instead of the host
 *     libc's 64-bit long;
 *   - _ismbblead and _ismbcspace: the code page is CP1251, which is a
 *     single-byte code page, so the lead-byte predicate is always false and
 *     the space predicate is the ASCII whitespace set.
 *
 * Formatting, scanning, strcpy_s, _fullpath and qsort stay in the quarantine.
 */
#ifndef WC3_HLE_MSVCR80_CONVERT_H
#define WC3_HLE_MSVCR80_CONVERT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the five rows over the as-built table.  Returns 5 on success, or
 * -1 when a row is missing.  Must run before guest threads start. */
int msvcr80_convert_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_MSVCR80_CONVERT_H */
