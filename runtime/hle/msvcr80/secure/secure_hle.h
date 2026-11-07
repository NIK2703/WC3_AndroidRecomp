/*
 * secure_hle.h - target-side MSVCR80 strcpy_s.
 *
 * The handler is installed over the as-built row, so it keeps the row's arity
 * and cdecl calling convention.  The guest-visible contract is the MSVCRT one:
 * return 0 on success, EINVAL for an invalid argument, and ERANGE when the
 * destination cannot hold the source plus its terminator.
 */
#ifndef WC3_HLE_MSVCR80_SECURE_H
#define WC3_HLE_MSVCR80_SECURE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the strcpy_s row over the as-built table.  Returns 1 on success, or
 * -1 when the row is missing.  Must run before guest threads start. */
int msvcr80_secure_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_MSVCR80_SECURE_H */
