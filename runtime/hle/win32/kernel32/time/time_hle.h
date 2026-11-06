/*
 * time_hle.h - kernel32/winmm clock and calendar imports.
 *
 * M5 migration, H6.2 order step "kernel32/time".  The handlers use the
 * Wc3TimeApi from runtime/platform, so this module has no POSIX/Android
 * include.  During the strangler window the as-built table row is replaced
 * through legacy_shim (wc3_legacy_hle_override); its argc and call convention
 * stay authoritative, so the handlers below must keep the Win32 signatures.
 */
#ifndef WC3_HLE_WIN32_KERNEL32_TIME_H
#define WC3_HLE_WIN32_KERNEL32_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

/* Install the handlers over the as-built registry.  Must be called after
 * hle_android_start() populated the table and before the first guest thread.
 * Returns the number of installed rows, or -1 when the platform time API or a
 * mandatory row is missing.  A missing row is a startup configuration error,
 * never a silent fallback. */
int wc3_hle_kernel32_time_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_WIN32_KERNEL32_TIME_H */
