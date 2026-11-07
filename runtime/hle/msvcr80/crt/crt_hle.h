/*
 * crt_hle.h - target-side MSVCR80 CRT runtime helpers.
 *
 * This subsystem owns small CRT internals that are neither string, math,
 * conversion nor error reporting.  It carries _vacopy (the x86 va_list copy
 * helper), the fatal runtime-control rows abort and _wassert, and the C-locale
 * setlocale row.  Installs must run before guest threads start.
 */
#ifndef WC3_HLE_MSVCR80_CRT_H
#define WC3_HLE_MSVCR80_CRT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the CRT helper rows.  Returns the number of rows installed, or -1
 * when the registry rejects one. */
int wc3_hle_msvcr80_crt_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_MSVCR80_CRT_H */
