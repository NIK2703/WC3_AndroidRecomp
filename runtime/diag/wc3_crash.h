/*
 * wc3_crash.h - fatal-only report for every build variant.
 *
 * A normal release run does not emit diagnostic logs.  A fatal guest or host
 * path calls wc3_crash_emit() once; the current boot log and a short report
 * are written to the public Download directory, and the report is mirrored to
 * logcat.  Nothing is written while the game is healthy.
 */
#ifndef WC3_CRASH_H
#define WC3_CRASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WC3_CRASH_DOWNLOAD_LOG "/storage/emulated/0/Download/wc3_log.txt"

void wc3_crash_set_log_path(const char *path);
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
