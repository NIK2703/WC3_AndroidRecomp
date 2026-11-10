/*
 * strerror_hle.c - target-side MSVCR80 strerror.
 *
 * The as-built handler returned a single "error" string for every errno value.
 * This slice carries the real MSVCR80 8.00.50727.762 message table: the strings
 * and their 0..42 order were verified against the licensed msvcr80.dll that
 * ships with the 1.27.1 fixture (index 12 is "Not enough space", not "Not
 * enough memory").  Unknown and negative values return "Unknown error".
 *
 * MSVCRT returns a pointer to a per-thread buffer that is overwritten by the
 * next strerror call on the same thread.  The HLE call runs on the guest
 * thread's host thread, so a host-TLS guest buffer has the same lifetime.  If
 * the guest heap cannot provide that buffer, the as-built observable result
 * (NULL) is kept instead of inventing a guest address.
 *
 * _errno is an addition.  It returns a per-thread four-byte guest cell; the
 * resolver previously bound it as an ordinary zero-returning call, so any
 * guest code that wrote through the result dereferenced NULL.
 */
#include "strerror_hle.h"

#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "hle_bridge.h"

#define STRERROR_BUFFER_SIZE 134u
#define UNKNOWN_MESSAGE "Unknown error"

/* Indexed by errno.  The order is the MSVCR80 table, not the POSIX numeric
 * order of every host. */
static const char *const g_messages[] = {
    "No error",
    "Operation not permitted",
    "No such file or directory",
    "No such process",
    "Interrupted function call",
    "Input/output error",
    "No such device or address",
    "Arg list too long",
    "Exec format error",
    "Bad file descriptor",
    "No child processes",
    "Resource temporarily unavailable",
    "Not enough space",
    "Permission denied",
    "Bad address",
    "Unknown error",
    "Resource device",
    "File exists",
    "Improper link",
    "No such device",
    "Not a directory",
    "Is a directory",
    "Invalid argument",
    "Too many open files in system",
    "Too many open files",
    "Inappropriate I/O control operation",
    "Unknown error",
    "File too large",
    "No space left on device",
    "Invalid seek",
    "Read-only file system",
    "Too many links",
    "Broken pipe",
    "Domain error",
    "Result too large",
    "Unknown error",
    "Resource deadlock avoided",
    "Unknown error",
    "Filename too long",
    "No locks available",
    "Function not implemented",
    "Directory not empty",
    "Illegal byte sequence",
};

static THREAD_LOCAL uint32_t g_message_buffer;
static THREAD_LOCAL uint32_t g_errno_cell;

static void h_errno(CPU *c)
{
    if (!g_errno_cell) g_errno_cell = wc3_legacy_guest_alloc0(4);
    c->eax = g_errno_cell &&
                     wc3_legacy_guest_ok(g_errno_cell, 4)
                 ? g_errno_cell
                 : 0;
}

static void h_strerror(CPU *c)
{
    const int32_t errnum = (int32_t)wc3_legacy_arg32(c, 0);
    const char *message =
        errnum >= 0 && (uint32_t)errnum < sizeof g_messages / sizeof g_messages[0]
            ? g_messages[errnum]
            : UNKNOWN_MESSAGE;

    if (!g_message_buffer)
        g_message_buffer = wc3_legacy_guest_alloc0(STRERROR_BUFFER_SIZE);
    if (!g_message_buffer ||
        !wc3_legacy_guest_ok(g_message_buffer, STRERROR_BUFFER_SIZE)) {
        c->eax = 0;
        return;
    }

    const size_t length = strlen(message);
    if (length >= STRERROR_BUFFER_SIZE) {
        c->eax = 0;
        return;
    }
    for (size_t i = 0; i < length; i++)
        wc3_legacy_wr8g(g_message_buffer + (uint32_t)i, (uint8_t)message[i]);
    wc3_legacy_wr8g(g_message_buffer + (uint32_t)length, 0u);
    c->eax = g_message_buffer;
}

static const struct {
    const char *name;
    void (*impl)(CPU *c);
} g_overrides[] = {
    { "strerror", h_strerror },
};

static const struct {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} g_additions[] = {
    { "_errno", 0, h_errno },
};

int msvcr80_error_install(void)
{
    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("MSVCR80.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("MSVCR80.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 1,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0] +
                 sizeof g_additions / sizeof g_additions[0]);
}
