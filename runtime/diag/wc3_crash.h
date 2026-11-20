/*
 * wc3_crash.h - fatal report and normal-exit log export for every variant.
 *
 * The boot log stays app-private while the game runs.  A fatal guest or host
 * path calls wc3_crash_emit() once; it copies the current log and a short
 * report to the public Download directory, and mirrors the report to logcat.
 * A normal game exit calls wc3_crash_export_log(), which copies the same log
 * to Download without a report.  Nothing is written to public storage before
 * the game ends.
 */
#ifndef WC3_CRASH_H
#define WC3_CRASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The report name carries the build variant so release and debug do not
 * overwrite each other in Download.  CMake passes WC3_BUILD_VARIANT from
 * WC3_VARIANT; standalone builds behave like release. */
#ifndef WC3_BUILD_VARIANT
#define WC3_BUILD_VARIANT "release"
#endif

#define WC3_CRASH_LOG_FILE "wc3_log_" WC3_BUILD_VARIANT ".txt"
#define WC3_CRASH_DOWNLOAD_LOG \
    "/storage/emulated/0/Download/" WC3_CRASH_LOG_FILE

void wc3_crash_set_log_path(const char *path);
/* Copy the current boot log to Download on a normal game exit.  A fatal path
 * already calls wc3_crash_emit(), which writes the log and its report. */
void wc3_crash_export_log(void);
void wc3_crash_note_message_box(const char *text, const char *caption,
                                unsigned type, uint32_t caller);
void wc3_crash_note_assert(const char *expression, const char *file,
                           unsigned line);
void wc3_crash_emit(const char *reason, unsigned code, const char *detail,
                    int signal_context);

#ifdef __cplusplus
}
#endif

#endif
