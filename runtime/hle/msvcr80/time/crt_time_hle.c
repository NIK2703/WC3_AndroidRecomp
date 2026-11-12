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
 * from the host locale.  _ftime64 is an addition: it fills the 16-byte
 * __timeb64 record, deriving the timezone field from the same local calendar
 * and reporting dstflag as zero because the platform has no DST state.
 */
#include "crt_time_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "hle_bridge.h"
#include "platform.h"

#define NS_PER_SEC 1000000000ull
#define CTIME_BUFFER_SIZE 32u
#define TM_SIZE 36u

static const Wc3TimeApi *g_time;
/* Guest VA of this guest thread's asctime buffer.  The HLE call runs on the
 * guest thread's host thread, so host TLS is the correct lifetime boundary. */
static _Thread_local uint32_t g_ctime_buffer;
/* gmtime and localtime use separate per-thread results, matching the CRT's
 * thread-local static struct tm objects. */
static _Thread_local uint32_t g_gmtime_buffer;
static _Thread_local uint32_t g_localtime_buffer;

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

typedef struct GuestTm {
    int32_t sec;
    int32_t min;
    int32_t hour;
    int32_t mday;
    int32_t mon;
    int32_t year;
    int32_t wday;
    int32_t yday;
    int32_t isdst;
} GuestTm;

/* Howard Hinnant's civil_from_days: inverse of days_from_civil above. */
static void civil_from_days(int64_t days, int64_t *year, unsigned *month,
                            unsigned *day)
{
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe =
        (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int64_t y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    const unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
    const unsigned m = mp + (mp < 10u ? 3u : (unsigned)-9);
    *year = y + (m <= 2u);
    *month = m;
    *day = d;
}

static int utc_tm(int64_t seconds, GuestTm *out)
{
    int64_t days = seconds / 86400;
    int64_t rem = seconds % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }

    int64_t year = 0;
    unsigned month = 0, day = 0;
    civil_from_days(days, &year, &month, &day);
    if (year < (int64_t)INT32_MIN + 1900 ||
        year > (int64_t)INT32_MAX + 1900)
        return 0;

    out->sec = (int32_t)(rem % 60);
    out->min = (int32_t)((rem / 60) % 60);
    out->hour = (int32_t)(rem / 3600);
    out->mday = (int32_t)day;
    out->mon = (int32_t)month - 1;
    out->year = (int32_t)(year - 1900);
    out->wday = (int32_t)weekday_from_civil(year, month, day);
    out->yday = (int32_t)(days_from_civil(year, month, day) -
                          days_from_civil(year, 1, 1));
    out->isdst = 0;
    return 1;
}

/* Wc3TimeApi exposes the local calendar fields but not the DST flag.  The
 * fields already contain the offset-adjusted wall clock; the CRT's tm_isdst
 * indicator is therefore reported as zero rather than guessed. */
static int local_tm(int64_t seconds, GuestTm *out)
{
    if (!g_time || !g_time->local_time) return 0;
    if (seconds < 0 || (uint64_t)seconds > UINT64_MAX / NS_PER_SEC) return 0;

    int32_t year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    int32_t millisecond = 0;
    const uint64_t ns = (uint64_t)seconds * NS_PER_SEC;
    if (g_time->local_time(g_time->ctx, ns, &year, &month, &day, &hour,
                           &minute, &second, &millisecond) != 0)
        return 0;
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 ||
        hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60)
        return 0;
    if (year < INT32_MIN + 1900 || year > INT32_MAX - 1900) return 0;

    out->sec = second;
    out->min = minute;
    out->hour = hour;
    out->mday = day;
    out->mon = month - 1;
    out->year = year - 1900;
    out->wday = (int32_t)weekday_from_civil(year, (unsigned)month,
                                            (unsigned)day);
    out->yday = (int32_t)(days_from_civil(year, (unsigned)month,
                                          (unsigned)day) -
                          days_from_civil(year, 1, 1));
    out->isdst = 0;
    return 1;
}

static uint32_t tm_slot(uint32_t *slot)
{
    if (!*slot) *slot = wc3_legacy_guest_alloc(TM_SIZE);
    if (!*slot || !wc3_legacy_guest_ok(*slot, TM_SIZE)) return 0;
    return *slot;
}

static void write_tm(uint32_t addr, const GuestTm *tm)
{
    wc3_legacy_wr32g(addr + 0u, (uint32_t)tm->sec);
    wc3_legacy_wr32g(addr + 4u, (uint32_t)tm->min);
    wc3_legacy_wr32g(addr + 8u, (uint32_t)tm->hour);
    wc3_legacy_wr32g(addr + 12u, (uint32_t)tm->mday);
    wc3_legacy_wr32g(addr + 16u, (uint32_t)tm->mon);
    wc3_legacy_wr32g(addr + 20u, (uint32_t)tm->year);
    wc3_legacy_wr32g(addr + 24u, (uint32_t)tm->wday);
    wc3_legacy_wr32g(addr + 28u, (uint32_t)tm->yday);
    wc3_legacy_wr32g(addr + 32u, (uint32_t)tm->isdst);
}

static int read_time64_arg(const CPU *c, unsigned index, int64_t *value)
{
    const uint32_t addr = wc3_legacy_arg32(c, index);
    if (!addr || !wc3_legacy_guest_ok(addr, 8)) return 0;
    *value = (int64_t)((uint64_t)wc3_legacy_rd32g(addr) |
                       ((uint64_t)wc3_legacy_rd32g(addr + 4u) << 32));
    return 1;
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

static void h_ftime64(CPU *c)
{
    const uint32_t out = wc3_legacy_arg32(c, 0);
    if (!out || !wc3_legacy_guest_ok(out, 16)) {
        c->eax = 0;
        return;
    }

    const uint64_t now_ns = g_time->realtime_ns(g_time->ctx);
    const uint64_t seconds = now_ns / NS_PER_SEC;
    const uint16_t millitm =
        (uint16_t)((now_ns % NS_PER_SEC) / 1000000ull);

    /* Wc3TimeApi gives the local wall-clock fields but not the UTC offset.
     * Derive it by comparing the local civil fields with the UTC epoch.  The
     * Win32 field is minutes west of UTC; dstflag stays zero because the
     * platform does not expose DST state. */
    int32_t year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    int32_t millisecond = 0;
    if (seconds > UINT64_MAX / NS_PER_SEC ||
        g_time->local_time(g_time->ctx, seconds * NS_PER_SEC, &year, &month,
                           &day, &hour, &minute, &second,
                           &millisecond) != 0 ||
        month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 ||
        hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
        c->eax = 0;
        return;
    }

    const int64_t local_seconds =
        days_from_civil(year, (unsigned)month, (unsigned)day) * 86400 +
        (int64_t)hour * 3600 + (int64_t)minute * 60 + (int64_t)second;
    int64_t timezone_minutes = -((local_seconds - (int64_t)seconds) / 60);
    if (timezone_minutes < INT16_MIN) timezone_minutes = INT16_MIN;
    if (timezone_minutes > INT16_MAX) timezone_minutes = INT16_MAX;

    wc3_legacy_wr32g(out, (uint32_t)seconds);
    wc3_legacy_wr32g(out + 4u, (uint32_t)(seconds >> 32));
    wc3_legacy_wr16g(out + 8u, millitm);
    wc3_legacy_wr16g(out + 10u, (uint16_t)(int16_t)timezone_minutes);
    wc3_legacy_wr16g(out + 12u, 0u);
    wc3_legacy_wr16g(out + 14u, 0u);
    c->eax = 0;
}

static void h_gmtime64(CPU *c)
{
    int64_t seconds = 0;
    GuestTm tm;

    if (!read_time64_arg(c, 0u, &seconds) || !utc_tm(seconds, &tm)) {
        c->eax = 0;
        return;
    }
    const uint32_t out = tm_slot(&g_gmtime_buffer);
    if (!out) {
        c->eax = 0;
        return;
    }
    write_tm(out, &tm);
    c->eax = out;
}

static void h_localtime64(CPU *c)
{
    int64_t seconds = 0;
    GuestTm tm;

    if (!read_time64_arg(c, 0u, &seconds) || !local_tm(seconds, &tm)) {
        c->eax = 0;
        return;
    }
    const uint32_t out = tm_slot(&g_localtime_buffer);
    if (!out) {
        c->eax = 0;
        return;
    }
    write_tm(out, &tm);
    c->eax = out;
}

static void h_localtime64_s(CPU *c)
{
    const uint32_t out = wc3_legacy_arg32(c, 0u);
    int64_t seconds = 0;
    GuestTm tm;

    if (!out || !wc3_legacy_guest_ok(out, TM_SIZE) ||
        !read_time64_arg(c, 1u, &seconds) || !local_tm(seconds, &tm)) {
        c->eax = 22u; /* EINVAL */
        return;
    }
    write_tm(out, &tm);
    c->eax = 0;
}

typedef struct CrtTimeOverride {
    const char *name;
    void (*impl)(CPU *c);
} CrtTimeOverride;

static const CrtTimeOverride g_overrides[] = {
    { "_time64", h_time64 },
    { "_ctime64", h_ctime64 },
};

typedef struct CrtTimeAddition {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} CrtTimeAddition;

static const CrtTimeAddition g_additions[] = {
    { "_ftime64", 1, h_ftime64 },
    { "_gmtime64", 1, h_gmtime64 },
    { "_localtime64", 1, h_localtime64 },
    { "_localtime64_s", 2, h_localtime64_s },
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
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("MSVCR80.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 1,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0] +
                 sizeof g_additions / sizeof g_additions[0]);
}
