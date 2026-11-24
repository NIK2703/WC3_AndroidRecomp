#include "wc3_crash.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#define WC3_CRASH_PATH_MAX 1024u
#define WC3_CRASH_MESSAGE_MAX 1024u
#define WC3_CRASH_CAPTION_MAX 256u
#define WC3_CRASH_ASSERT_MAX 512u
#define WC3_CRASH_DETAIL_MAX 4096u
#define WC3_CRASH_REPORT_MAX 8192u

static char g_log_path[WC3_CRASH_PATH_MAX];
static char g_message[WC3_CRASH_MESSAGE_MAX];
static char g_caption[WC3_CRASH_CAPTION_MAX];
static char g_assert_expression[WC3_CRASH_ASSERT_MAX];
static char g_assert_file[WC3_CRASH_ASSERT_MAX];
static unsigned g_message_type;
static unsigned g_assert_line;
static uint32_t g_message_caller;
static int g_has_message;
static int g_has_assert;
static volatile int g_emitting;
/* Set while a copy is in progress.  The activity teardown and the native
 * end-of-loop path can both ask for the export; skipping a concurrent call
 * keeps one writer from truncating the other's temporary file. */
static atomic_int g_exporting;

static void copy_text(char *dst, size_t capacity, const char *src)
{
    size_t length;

    if (!dst || !capacity)
        return;
    if (!src)
        src = "";
    length = strlen(src);
    if (length >= capacity)
        length = capacity - 1u;
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static int write_full(int fd, const char *data, size_t length)
{
    while (length) {
        ssize_t written = write(fd, data, length);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!written)
            return -1;
        data += written;
        length -= (size_t)written;
    }
    return 0;
}

static int copy_log_to(int out, const char *path)
{
    char buffer[4096];
    int in;
    int result = 0;

    if (!path || !*path)
        return -1;
    in = open(path, O_RDONLY);
    if (in < 0)
        return -1;
    for (;;) {
        ssize_t got = read(in, buffer, sizeof buffer);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            result = -1;
            break;
        }
        if (!got)
            break;
        if (write_full(out, buffer, (size_t)got) != 0) {
            result = -1;
            break;
        }
    }
    close(in);
    return result;
}

static int open_download_path(const char *path, int flags)
{
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    return open(path, flags, 0666);
}

static int open_download(int append)
{
    return open_download_path(
        WC3_CRASH_DOWNLOAD_LOG,
        O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
}

static int open_download_tmp(char *path, size_t capacity)
{
    int length;

    /*
     * A per-process temporary keeps the two host processes (default and :roc)
     * from truncating each other's half-written file.  rename(2) then
     * publishes one complete log; the target name is shared as before.
     */
    length = snprintf(path, capacity, "%s.%ld.tmp",
                      WC3_CRASH_DOWNLOAD_LOG, (long)getpid());
    if (length <= 0 || (size_t)length >= capacity)
        return -1;
    return open_download_path(path, O_WRONLY | O_CREAT | O_TRUNC);
}

static size_t report_append(char *buffer, size_t capacity, size_t offset,
                            const char *format, ...)
{
    va_list args;
    int length;

    if (offset >= capacity)
        return capacity ? capacity - 1u : 0u;
    va_start(args, format);
    length = vsnprintf(buffer + offset, capacity - offset, format, args);
    va_end(args);
    if (length < 0)
        return offset;
    if ((size_t)length >= capacity - offset)
        return capacity - 1u;
    return offset + (size_t)length;
}

static void persist_report(const char *report, size_t length)
{
    const int same_file =
        g_log_path[0] &&
        !strcmp(g_log_path, WC3_CRASH_DOWNLOAD_LOG);
    int out;

    if (same_file) {
        out = open_download(1);
        if (out >= 0) {
            (void)write_full(out, report, length);
            close(out);
            return;
        }
        (void)write_full(2, report, length);
        return;
    }

    out = open_download(0);
    if (out >= 0) {
        (void)copy_log_to(out, g_log_path);
        (void)write_full(out, report, length);
        close(out);
    }
    (void)write_full(2, report, length);
}

void wc3_crash_set_log_path(const char *path)
{
    if (!path || !*path) {
        g_log_path[0] = '\0';
        return;
    }
    copy_text(g_log_path, sizeof g_log_path, path);
}

void wc3_crash_export_log(void)
{
    char tmp_path[WC3_CRASH_PATH_MAX];
    int expected = 0;
    int ok;
    int in;
    int out;

    if (!g_log_path[0] || g_emitting ||
        !strcmp(g_log_path, WC3_CRASH_DOWNLOAD_LOG))
        return;
    if (!atomic_compare_exchange_strong(&g_exporting, &expected, 1))
        return;

    /* The boot log writer is line-buffered.  Drain it before copying so the
     * normal-exit file includes the last line. */
    fflush(NULL);
    /* Probe the source first: a missing app-private log must not truncate an
     * older Download copy. */
    in = open(g_log_path, O_RDONLY);
    if (in < 0) {
        atomic_store(&g_exporting, 0);
        return;
    }
    close(in);

    out = open_download_tmp(tmp_path, sizeof tmp_path);
    if (out < 0) {
        atomic_store(&g_exporting, 0);
        return;
    }
    ok = copy_log_to(out, g_log_path) == 0;
    (void)fsync(out);
    if (close(out) != 0)
        ok = 0;
    /* A crash report writes the target directly; never replace it with the
     * plain log if the fatal path started while this copy was in flight. */
    if (g_emitting)
        ok = 0;
    /* Only a complete copy replaces the previous Download file.  A killed
     * process leaves the old file and at worst a stale .tmp behind. */
    if (!ok || rename(tmp_path, WC3_CRASH_DOWNLOAD_LOG) != 0)
        unlink(tmp_path);
    atomic_store(&g_exporting, 0);
}

void wc3_crash_note_message_box(const char *text, const char *caption,
                                unsigned type, uint32_t caller)
{
    copy_text(g_message, sizeof g_message, text);
    copy_text(g_caption, sizeof g_caption, caption);
    g_message_type = type;
    g_message_caller = caller;
    g_has_message = 1;
}

void wc3_crash_note_assert(const char *expression, const char *file,
                           unsigned line)
{
    copy_text(g_assert_expression, sizeof g_assert_expression, expression);
    copy_text(g_assert_file, sizeof g_assert_file, file);
    g_assert_line = line;
    g_has_assert = 1;
}

void wc3_crash_emit(const char *reason, unsigned code, const char *detail,
                    int signal_context)
{
    char report[WC3_CRASH_REPORT_MAX];
    size_t offset = 0;

    if (g_emitting)
        return;
    g_emitting = 1;

    offset = report_append(report, sizeof report, offset,
                           "\n===== wc3 crash report =====\n");
    offset = report_append(report, sizeof report, offset,
                           "reason: %s\n", reason && *reason ? reason : "fatal");
    offset = report_append(report, sizeof report, offset,
                           "code: %u\n", code);
    if (g_has_message) {
        offset = report_append(report, sizeof report, offset,
                               "message box text:\n%s\n", g_message);
        if (g_caption[0])
            offset = report_append(report, sizeof report, offset,
                                   "message box caption: %s\n", g_caption);
        offset = report_append(report, sizeof report, offset,
                               "message box type: %u caller=%08X\n",
                               g_message_type, g_message_caller);
    }
    if (g_has_assert) {
        offset = report_append(report, sizeof report, offset,
                               "assertion: %s\nfile: %s\nline: %u\n",
                               g_assert_expression, g_assert_file,
                               g_assert_line);
    }
    if (detail && *detail)
        offset = report_append(report, sizeof report, offset,
                               "context:\n%s%s",
                               detail,
                               detail[strlen(detail) - 1u] == '\n' ? "" : "\n");
    offset = report_append(report, sizeof report, offset,
                           "boot log: %s\n",
                           g_log_path[0] ? g_log_path : "(not redirected)");
    offset = report_append(report, sizeof report, offset,
                           "===== end crash report =====\n");

    persist_report(report, offset);
#if defined(__ANDROID__)
    if (!signal_context)
        __android_log_write(ANDROID_LOG_ERROR, "wc3crash", report);
#else
    (void)signal_context;
#endif
}
