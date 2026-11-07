/*
 * process_hle.h - kernel32 process/thread identity imports.
 *
 * Second target HLE module after kernel32/time (M5, H6.2 order).  The
 * pseudo-handle values are fixed by Win32; the two IDs come from Wc3ThreadApi
 * instead of the as-built constants (1234/5678), which made every guest thread
 * look like the same thread.
 */
#ifndef WC3_HLE_WIN32_KERNEL32_PROCESS_H
#define WC3_HLE_WIN32_KERNEL32_PROCESS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Install over the as-built registry.  Must run after hle_android_start() and
 * before the first guest thread.  Returns the number of rows installed, or -1
 * when the platform thread API or a mandatory row is missing. */
int wc3_hle_kernel32_process_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_WIN32_KERNEL32_PROCESS_H */
