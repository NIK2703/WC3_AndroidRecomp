/*
 * strerror_hle.h - target-side MSVCR80 strerror.
 *
 * The returned pointer names a per-host-thread guest buffer owned by this
 * module.  It stays valid until the next strerror call on the same guest
 * thread, matching the CRT's thread-local message buffer.
 */
#ifndef WC3_HLE_MSVCR80_ERROR_H
#define WC3_HLE_MSVCR80_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the strerror row over the as-built table.  Returns 1 on success, or
 * -1 when the row is missing.  Must run before guest threads start. */
int msvcr80_error_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_MSVCR80_ERROR_H */
