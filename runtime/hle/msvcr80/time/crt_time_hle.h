/*
 * crt_time_hle.h - target-side MSVCR80 time imports.
 *
 * This is the msvcr80/time slice of the M5 migration.  It covers the two
 * imports the game uses:
 *
 *   - _time64: 64-bit Unix seconds, returned in edx:eax and optionally stored
 *     through a guest pointer;
 *   - _ctime64: the classic asctime-shaped local-time string.
 *
 * Both handlers read the clock through Wc3TimeApi; the module must not call
 * host time()/localtime() directly.  Install fails if the platform table is
 * missing the APIs, which keeps the startup contract explicit.
 */
#ifndef WC3_HLE_MSVCR80_TIME_H
#define WC3_HLE_MSVCR80_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the two rows over the as-built table.  Returns 2 on success, or -1
 * when the platform time API or a required row is missing.  Must run before
 * guest threads start. */
int wc3_hle_msvcr80_time_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_MSVCR80_TIME_H */
