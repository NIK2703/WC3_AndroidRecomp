/*
 * crt_time_hle.c - target-side MSVCR80 time imports.
 *
 * The as-built handlers used host time()/localtime_r() and a fresh guest heap
 * allocation for every _ctime64 call.  This slice takes both clocks from
 * Wc3TimeApi and keeps one asctime buffer per host thread, mirroring the
 * per-thread static buffer of the Microsoft CRT.  Guest threads map one-to-one
 * to host threads, so the buffer identity follows the guest thread.
 *
 * _time64 returns a 64-bit value in edx:eax.  _ctime64 always emits the C
 * locale form "Www Mmm dd hh:mm:ss yyyy\n"; the platform supplies the local
 * calendar fields, and the weekday is derived from the civil date rather than
 * from the host locale.
 */
#include "crt_time_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "platform.h"

#define NS_PER_SEC 1000000000ull
#define CTIME_BUFFER_SIZE 32u

static const Wc3TimeApi *g_time;
/* Guest VA of this guest thread's asctime buffer.  The HLE call runs on the
 * guest thread's host thread, so host TLS is the correct lifetime boundary. */
static THREAD_LOCAL uint32_t g_ctime_buffer;

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2u;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy =
        (153u * (m + (m > 2u ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* 1970-01-01 was a Thursday; the C locale numbers Sunday as 0. */
static unsigned weekday_from_civil(int64_t year, unsigned month, unsigned day)
{
    int64_t w = (days_from_civil(year, month, day) + 4) % 7;
    if (w < 0) w += 7;
    return (unsigned)w;
}

static char *append_text(char *p, const char *text)
{
    while (*text) *p++ = *text++;
    return p;
}

static char *append_two_digits(char *p, unsigned value)
{
    *p++ = (char)('0' + (value / 10u) % 10u);
    *p++ = (char)('0' + value % 10u);
    return p;
}

/* MSVC's asctime uses %d for the year, so years below 1000 are not
 * zero-padded.  Keep that shape instead of forcing four digits. */
static char *append_decimal(char *p, unsigned value)
{
    char digits[10];
    unsigned count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count) *p++ = digits[--count];
    return p;
}

static int build_ctime(char *buf, int32_t year, int32_t month, int32_t day,
                       int32_t hour, int32_t minute, int32_t second)
{
    static const char *const weekday_name[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    static const char *const month_name[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    if (year < 0 || year > 9999 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 60)
        return -1;

    char *p = buf;
    p = append_text(p, weekday_name[weekday_from_civil(
        year, (unsigned)month, (unsigned)day)]);
    *p++ = ' ';
    p = append_text(p, month_name[month - 1]);
    *p++ = ' ';
    if (day < 10) {
        *p++ = ' ';
        *p++ = (char)('0' + day);
    } else {
        p = append_two_digits(p, (unsigned)day);
    }
    *p++ = ' ';
    p = append_two_digits(p, (unsigned)hour);
    *p++ = ':';
    p = append_two_digits(p, (unsigned)minute);
    *p++ = ':';
    p = append_two_digits(p, (unsigned)second);
    *p++ = ' ';
    p = append_decimal(p, (unsigned)year);
    *p++ = '\n';
    *p = '\0';
    return (int)(p - buf) + 1;
}

static uint64_t realtime_seconds(void)
{
    return g_time->realtime_ns(g_time->ctx) / NS_PER_SEC;
}

static void h_time64(CPU *c)
{
    if (!g_time || !g_time->realtime_ns) {
        c->eax = 0;
        c->edx = 0;
        return;
    }

    const uint64_t seconds = realtime_seconds();
    const uint32_t out = wc3_legacy_arg32(c, 0);
    if (out && wc3_legacy_guest_ok(out, 8)) {
        wc3_legacy_wr32g(out, (uint32_t)seconds);
        wc3_legacy_wr32g(out + 4, (uint32_t)(seconds >> 32));
    }
    c->eax = (uint32_t)seconds;
    c->edx = (uint32_t)(seconds >> 32);
}

static void h_ctime64(CPU *c)
{
    if (!g_time || !g_time->realtime_ns || !g_time->local_time) {
        c->eax = 0;
        return;
    }

    const uint32_t timer = wc3_legacy_arg32(c, 0);
    int64_t seconds;
    if (timer && wc3_legacy_guest_ok(timer, 8)) {
        seconds = (int64_t)((uint64_t)wc3_legacy_rd32g(timer) |
                            ((uint64_t)wc3_legacy_rd32g(timer + 4) << 32));
    } else {
        const uint64_t now = realtime_seconds();
        if (now > (uint64_t)INT64_MAX) {
            c->eax = 0;
            return;
        }
        seconds = (int64_t)now;
    }

    /* The platform API takes Unix nanoseconds in an unsigned 64-bit value.
     * Reject the negative CRT time range instead of wrapping it into a
     * meaningless date near the end of the calendar. */
    if (seconds < 0 ||
        (uint64_t)seconds > UINT64_MAX / NS_PER_SEC) {
        c->eax = 0;
        return;
    }

    int32_t year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    int32_t millisecond = 0;
    const uint64_t ns = (uint64_t)seconds * NS_PER_SEC;
    if (g_time->local_time(g_time->ctx, ns, &year, &month, &day, &hour,
                           &minute, &second, &millisecond) != 0) {
        c->eax = 0;
        return;
    }

    char buf[CTIME_BUFFER_SIZE];
    const int length = build_ctime(buf, year, month, day, hour, minute,
                                   second);
    if (length <= 0) {
        c->eax = 0;
        return;
    }

    if (!g_ctime_buffer)
        g_ctime_buffer = wc3_legacy_guest_alloc(CTIME_BUFFER_SIZE);
    if (!g_ctime_buffer || !wc3_legacy_guest_ok(g_ctime_buffer,
                                                (uint32_t)length)) {
        c->eax = 0;
        return;
    }
    for (int i = 0; i < length; i++)
        wc3_legacy_wr8g(g_ctime_buffer + (uint32_t)i, (uint8_t)buf[i]);
    c->eax = g_ctime_buffer;
}

typedef struct CrtTimeOverride {
    const char *name;
    void (*impl)(CPU *c);
} CrtTimeOverride;

static const CrtTimeOverride g_overrides[] = {
    { "_time64", h_time64 },
    { "_ctime64", h_ctime64 },
};

int wc3_hle_msvcr80_time_install(void)
{
    const Wc3Platform *platform = wc3_platform();
    if (!platform || !platform->time.realtime_ns ||
        !platform->time.local_time)
        return -1;
    g_time = &platform->time;

    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("MSVCR80.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0]);
}
