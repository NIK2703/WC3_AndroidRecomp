/*
 * time_hle.c - kernel32/winmm time imports, first M5 module.
 *
 * Why this module exists
 * ----------------------
 * The as-built Android HLE mixed four time sources inside one translation
 * unit and filled SYSTEMTIME in `struct tm` order, not in the Windows order:
 * it wrote tm_mday at +4 (dayOfWeek), tm_hour at +6 (day), and so on.  Any
 * guest that parsed the structure saw a misaligned calendar.  EXACTNESS E3
 * also requires one monotonic source; this module takes every clock from
 * Wc3TimeApi and converts calendar fields with integer arithmetic, so the
 * result does not depend on the host C library or locale.
 *
 * Epoch constants.  FILETIME counts 100 ns ticks since 1601-01-01 UTC; the
 * Unix epoch is 11644473600 seconds later.
 */
#include "time_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "hle_bridge.h"
#include "platform.h"

#define FILETIME_UNIX_EPOCH 116444736000000000ull
#define NS_PER_SEC 1000000000ull
/* Largest real-world UTC offset is +14:00; keep FILETIME arithmetic inside
 * int64 for every value the API can represent. */
#define MAX_TZ_OFFSET_100NS (14ll * 3600ll * 10000000ll)

static const Wc3TimeApi *g_time;

/* ------------------------------------------------------------------ */
/* calendar arithmetic (proleptic Gregorian, no host time functions)   */
/* ------------------------------------------------------------------ */

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) q--;
    return q;
}

/* Howard Hinnant's days_from_civil.  Day 0 is 1970-01-01. */
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

static void civil_from_days(int64_t z, int64_t *year, unsigned *month,
                            unsigned *day)
{
    z += 719468;
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

/* 1970-01-01 was a Thursday; Windows numbers Sunday as 0. */
static unsigned weekday_from_days(int64_t days)
{
    int64_t w = (days + 4) % 7;
    if (w < 0) w += 7;
    return (unsigned)w;
}

static int64_t unix_seconds(int64_t days, unsigned hour, unsigned minute,
                            unsigned second)
{
    return days * 86400 + (int64_t)hour * 3600 + (int64_t)minute * 60 +
           (int64_t)second;
}

/* 0 on success; the offset is local - UTC in seconds at that instant, so DST
 * and timezone changes are taken from the platform rather than a fixed bias. */
static int local_offset_seconds(const Wc3TimeApi *t, uint64_t unix_ns,
                                int64_t *offset)
{
    int32_t year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    int32_t millisecond = 0;
    if (!t || !t->local_time) return -1;
    if (t->local_time(t->ctx, unix_ns, &year, &month, &day, &hour, &minute,
                      &second, &millisecond) != 0)
        return -1;
    const int64_t days =
        days_from_civil(year, (unsigned)month, (unsigned)day);
    *offset = unix_seconds(days, (unsigned)hour, (unsigned)minute,
                           (unsigned)second) -
              (int64_t)(unix_ns / NS_PER_SEC);
    return 0;
}

static int is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static unsigned days_in_month(int year, unsigned month)
{
    static const unsigned char days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month == 2 && is_leap_year(year)) return 29;
    return days[month - 1];
}

static void write_filetime(uint32_t dst, uint64_t value)
{
    if (!dst || !wc3_legacy_guest_ok(dst, 8)) return;
    wc3_legacy_wr32g(dst, (uint32_t)value);
    wc3_legacy_wr32g(dst + 4, (uint32_t)(value >> 32));
}

static uint64_t read_filetime(uint32_t src, int *ok)
{
    if (!src || !wc3_legacy_guest_ok(src, 8)) {
        *ok = 0;
        return 0;
    }
    *ok = 1;
    return (uint64_t)wc3_legacy_rd32g(src) |
           ((uint64_t)wc3_legacy_rd32g(src + 4) << 32);
}

/* SYSTEMTIME: year, month, dayOfWeek, day, hour, minute, second, ms. */
static void write_system_time(uint32_t dst, int64_t unix_sec, unsigned ms)
{
    if (!dst || !wc3_legacy_guest_ok(dst, 16)) return;
    const int64_t days = floor_div(unix_sec, 86400);
    const int64_t sod = unix_sec - days * 86400;
    int64_t year = 0;
    unsigned month = 0, day = 0;
    civil_from_days(days, &year, &month, &day);
    wc3_legacy_wr16g(dst + 0, (uint16_t)year);
    wc3_legacy_wr16g(dst + 2, (uint16_t)month);
    wc3_legacy_wr16g(dst + 4, (uint16_t)weekday_from_days(days));
    wc3_legacy_wr16g(dst + 6, (uint16_t)day);
    wc3_legacy_wr16g(dst + 8, (uint16_t)(sod / 3600));
    wc3_legacy_wr16g(dst + 10, (uint16_t)((sod / 60) % 60));
    wc3_legacy_wr16g(dst + 12, (uint16_t)(sod % 60));
    wc3_legacy_wr16g(dst + 14, (uint16_t)ms);
}

static void write_system_time_local(uint32_t dst, int32_t year, int32_t month,
                                    int32_t day, int32_t hour, int32_t minute,
                                    int32_t second, int32_t millisecond)
{
    if (!dst || !wc3_legacy_guest_ok(dst, 16)) return;
    const int64_t days = days_from_civil(year, (unsigned)month, (unsigned)day);
    wc3_legacy_wr16g(dst + 0, (uint16_t)year);
    wc3_legacy_wr16g(dst + 2, (uint16_t)month);
    wc3_legacy_wr16g(dst + 4, (uint16_t)weekday_from_days(days));
    wc3_legacy_wr16g(dst + 6, (uint16_t)day);
    wc3_legacy_wr16g(dst + 8, (uint16_t)hour);
    wc3_legacy_wr16g(dst + 10, (uint16_t)minute);
    wc3_legacy_wr16g(dst + 12, (uint16_t)second);
    wc3_legacy_wr16g(dst + 14, (uint16_t)millisecond);
}

static void read_system_time(uint32_t src, int *year, unsigned *month,
                             unsigned *day, unsigned *hour, unsigned *minute,
                             unsigned *second, unsigned *millisecond)
{
    *year = (int)wc3_legacy_rd16g(src + 0);
    *month = wc3_legacy_rd16g(src + 2);
    *day = wc3_legacy_rd16g(src + 6);
    *hour = wc3_legacy_rd16g(src + 8);
    *minute = wc3_legacy_rd16g(src + 10);
    *second = wc3_legacy_rd16g(src + 12);
    *millisecond = wc3_legacy_rd16g(src + 14);
}

/* ------------------------------------------------------------------ */
/* date and time formatting                                            */
/* ------------------------------------------------------------------ */

#define DATE_SHORTDATE     0x00000001u
#define DATE_LONGDATE      0x00000002u
#define DATE_YEARMONTH     0x00000008u
#define DATE_MONTHDAY      0x00000080u
#define TIME_NOMINUTESORSECONDS 0x00000001u
#define TIME_NOSECONDS     0x00000002u
#define TIME_NOTIMEMARKER  0x00000004u
#define TIME_FORCE24HOURFORMAT 0x00000008u

/* CP1251, because GetDateFormatA/GetTimeFormatA return bytes in the system
 * ANSI codepage and the oracle desktop is ru-RU (0x0419).  Windows uses
 * nominative names for explicit MMM/MMMM tokens and genitive month names in
 * the DATE_LONGDATE pattern; both forms are kept here.  A probe of the oracle
 * API also showed that the Russian AM/PM strings are empty, so t/tt emit
 * nothing. */
static const char *const g_weekday_short[7] = {
    "\xC2\xF1", "\xCF\xED", "\xC2\xF2", "\xD1\xF0", "\xD7\xF2",
    "\xCF\xF2", "\xD1\xE1"
};
static const char *const g_weekday_long[7] = {
    "\xE2\xEE\xF1\xEA\xF0\xE5\xF1\xE5\xED\xFC\xE5", /* воскресенье */
    "\xEF\xEE\xED\xE5\xE4\xE5\xEB\xFC\xED\xE8\xEA", /* понедельник */
    "\xE2\xF2\xEE\xF0\xED\xE8\xEA",                 /* вторник */
    "\xF1\xF0\xE5\xE4\xE0",                         /* среда */
    "\xF7\xE5\xF2\xE2\xE5\xF0\xE3",                 /* четверг */
    "\xEF\xFF\xF2\xED\xE8\xF6\xE0",                 /* пятница */
    "\xF1\xF3\xE1\xE1\xEE\xF2\xE0"                  /* суббота */
};
static const char *const g_month_short[12] = {
    "\xFF\xED\xE2", "\xF4\xE5\xE2", "\xEC\xE0\xF0", "\xE0\xEF\xF0",
    "\xEC\xE0\xE9", "\xE8\xFE\xED", "\xE8\xFE\xEB", "\xE0\xE2\xE3",
    "\xF1\xE5\xED", "\xEE\xEA\xF2", "\xED\xEE\xFF", "\xE4\xE5\xEA"
};
static const char *const g_month_long[12] = {
    "\xD1\xED\xE2\xE0\xF0\xFC", "\xD4\xE5\xE2\xF0\xE0\xEB\xFC",
    "\xCC\xE0\xF0\xF2", "\xC0\xEF\xF0\xE5\xEB\xFC",
    "\xCC\xE0\xE9", "\xC8\xFE\xED\xFC", "\xC8\xFE\xEB\xFC",
    "\xC0\xE2\xE3\xF3\xF1\xF2", "\xD1\xE5\xED\xF2\xFF\xE1\xF0\xFC",
    "\xCE\xEA\xF2\xFF\xE1\xF0\xFC", "\xCD\xEE\xFF\xE1\xF0\xFC",
    "\xC4\xE5\xEA\xE0\xE1\xF0\xFC"
};
static const char *const g_month_genitive[12] = {
    "\xFF\xED\xE2\xE0\xF0\xFF", "\xF4\xE5\xE2\xF0\xE0\xEB\xFF",
    "\xEC\xE0\xF0\xF2\xE0", "\xE0\xEF\xF0\xE5\xEB\xFF",
    "\xEC\xE0\xFF", "\xE8\xFE\xED\xFF", "\xE8\xFE\xEB\xFF",
    "\xE0\xE2\xE3\xF3\xF1\xF2\xE0", "\xF1\xE5\xED\xF2\xFF\xE1\xF0\xFF",
    "\xEE\xEA\xF2\xFF\xE1\xF0\xFF", "\xED\xEE\xFF\xE1\xF0\xFF",
    "\xE4\xE5\xEA\xE0\xE1\xF0\xFF"
};

static int append_char(char *out, int cap, int *len, char ch)
{
    if (*len >= cap) return -1;
    out[(*len)++] = ch;
    return 0;
}

static int append_text(char *out, int cap, int *len, const char *text)
{
    while (*text) {
        if (append_char(out, cap, len, *text++)) return -1;
    }
    return 0;
}

static int append_dec(char *out, int cap, int *len, unsigned value, int width)
{
    char digits[12];
    int n = 0;
    do {
        digits[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (n < width) digits[n++] = '0';
    for (int i = n - 1; i >= 0; i--) {
        if (append_char(out, cap, len, digits[i])) return -1;
    }
    return 0;
}

static int read_guest_string(uint32_t addr, char *dst, int cap)
{
    if (!addr || cap < 2) return -1;
    int i = 0;
    while (i < cap - 1) {
        const uint32_t at = addr + (uint32_t)i;
        uint32_t chunk = (uint32_t)(cap - 1 - i);
        if (chunk > 64u) chunk = 64u;
        const uint64_t room = 0x100000000ull - at;
        if (chunk > room) chunk = (uint32_t)room;

        if (wc3_legacy_guest_ok(at, chunk)) {
            for (uint32_t j = 0; j < chunk; j++) {
                const char ch = (char)wc3_legacy_rd8g(at + j);
                dst[i++] = ch;
                if (!ch) return 0;
            }
        } else {
            /* A chunk that ends in an unmapped byte keeps the old
             * all-or-nothing result: walk it byte by byte. */
            for (uint32_t j = 0; j < chunk; j++) {
                const uint32_t byte = at + j;
                if (!wc3_legacy_guest_ok(byte, 1u)) return -1;
                const char ch = (char)wc3_legacy_rd8g(byte);
                dst[i++] = ch;
                if (!ch) return 0;
            }
        }
    }
    dst[cap - 1] = 0;
    return -1;
}

static int valid_date(int year, unsigned month, unsigned day)
{
    return year >= 1601 && year <= 30827 && month >= 1 && month <= 12 &&
           day >= 1 && day <= days_in_month(year, month);
}

static int valid_time(unsigned hour, unsigned minute, unsigned second,
                      unsigned millisecond)
{
    return hour <= 23 && minute <= 59 && second <= 59 && millisecond <= 999;
}

static int format_date(char *out, int cap, const char *fmt, int year,
                       unsigned month, unsigned day, int genitive_months)
{
    const unsigned weekday =
        weekday_from_days(days_from_civil(year, month, day));
    int len = 0;

    for (int i = 0; fmt[i];) {
        const char ch = fmt[i];
        if (ch == '\'') {
            if (fmt[i + 1] == '\'') {
                if (append_char(out, cap, &len, '\'')) return -1;
                i += 2;
            } else {
                i++;
                while (fmt[i] && fmt[i] != '\'') {
                    if (append_char(out, cap, &len, fmt[i++])) return -1;
                }
                if (fmt[i] == '\'') i++;
            }
            continue;
        }
        if (ch == 'd' || ch == 'M' || ch == 'y' || ch == 'g') {
            int run = 1;
            while (fmt[i + run] == ch) run++;
            int rc = 0;
            if (ch == 'd') {
                if (run == 1) rc = append_dec(out, cap, &len, day, 0);
                else if (run == 2) rc = append_dec(out, cap, &len, day, 2);
                else if (run == 3)
                    rc = append_text(out, cap, &len, g_weekday_short[weekday]);
                else
                    rc = append_text(out, cap, &len, g_weekday_long[weekday]);
            } else if (ch == 'M') {
                if (run == 1) rc = append_dec(out, cap, &len, month, 0);
                else if (run == 2) rc = append_dec(out, cap, &len, month, 2);
                else if (run == 3)
                    rc = append_text(out, cap, &len, g_month_short[month - 1u]);
                else
                    rc = append_text(out, cap, &len,
                                     genitive_months
                                         ? g_month_genitive[month - 1u]
                                         : g_month_long[month - 1u]);
            } else if (ch == 'y') {
                if (run == 1) rc = append_dec(out, cap, &len, (unsigned)year % 100u, 0);
                else if (run == 2)
                    rc = append_dec(out, cap, &len, (unsigned)year % 100u, 2);
                else
                    rc = append_dec(out, cap, &len, (unsigned)year, 4);
            } else {
                rc = append_text(out, cap, &len,
                                 "\xED\xE0\xF8\xE0 \xFD\xF0\xE0");
            }
            if (rc) return -1;
            i += run;
            continue;
        }
        if (append_char(out, cap, &len, ch)) return -1;
        i++;
    }
    return len;
}

static int format_time(char *out, int cap, const char *fmt, unsigned hour,
                       unsigned minute, unsigned second, uint32_t flags)
{
    int len = 0;
    for (int i = 0; fmt[i];) {
        const char ch = fmt[i];
        if (ch == '\'') {
            if (fmt[i + 1] == '\'') {
                if (append_char(out, cap, &len, '\'')) return -1;
                i += 2;
            } else {
                i++;
                while (fmt[i] && fmt[i] != '\'') {
                    if (append_char(out, cap, &len, fmt[i++])) return -1;
                }
                if (fmt[i] == '\'') i++;
            }
            continue;
        }
        if (ch == 'h' || ch == 'H' || ch == 'm' || ch == 's' || ch == 't') {
            int run = 1;
            while (fmt[i + run] == ch) run++;
            int rc = 0;
            if (ch == 'h' || ch == 'H') {
                unsigned h = hour;
                if (ch == 'h' && !(flags & TIME_FORCE24HOURFORMAT)) {
                    h %= 12u;
                    if (h == 0) h = 12;
                }
                rc = append_dec(out, cap, &len, h, run >= 2 ? 2 : 0);
            } else if (ch == 'm') {
                rc = append_dec(out, cap, &len, minute, run >= 2 ? 2 : 0);
            } else if (ch == 's') {
                rc = append_dec(out, cap, &len, second, run >= 2 ? 2 : 0);
            } else {
                /* ru-RU LOCALE_S1159/LOCALE_S2359 are empty.  The oracle
                 * probe returns "03:34:56" for "hh:mm:ss tt", so drop the
                 * separator that precedes the empty marker. */
                if (len > 0 && out[len - 1] == ' ') len--;
                (void)run;
            }
            if (rc) return -1;
            i += run;
            continue;
        }
        if (append_char(out, cap, &len, ch)) return -1;
        i++;
    }
    return len;
}

/* Win32 returns the required size, including the terminator, and writes
 * nothing when the caller's buffer is too small.  The as-built stub copied a
 * truncated pattern and reported a length without the terminator. */
static uint32_t emit_string(uint32_t buf, int cch, const char *text, int len)
{
    if (len < 0) return 0;
    if (cch == 0) return (uint32_t)len + 1u;
    if (cch < 0 || cch < len + 1 || !buf ||
        !wc3_legacy_guest_ok(buf, (uint32_t)cch))
        return 0;
    for (int i = 0; i < len; i++)
        wc3_legacy_wr8g(buf + (uint32_t)i, (uint8_t)text[i]);
    wc3_legacy_wr8g(buf + (uint32_t)len, 0);
    return (uint32_t)len + 1u;
}

/* ------------------------------------------------------------------ */
/* handlers                                                            */
/* ------------------------------------------------------------------ */

static void h_get_tick_count(CPU *c)
{
    const uint64_t ns = g_time->monotonic_ns(g_time->ctx);
    c->eax = (uint32_t)(ns / 1000000ull);
}

static void h_time_get_time(CPU *c)
{
    h_get_tick_count(c);
}

static void h_query_perf_counter(CPU *c)
{
    const uint32_t out = wc3_legacy_arg32(c, 0);
    const uint64_t value = g_time->monotonic_ns(g_time->ctx);
    write_filetime(out, value);
    c->eax = 1;
    c->edx = 0;
}

static void h_query_perf_freq(CPU *c)
{
    const uint32_t out = wc3_legacy_arg32(c, 0);
    write_filetime(out, NS_PER_SEC);
    c->eax = 1;
    c->edx = 0;
}

static void h_get_system_time_as_file_time(CPU *c)
{
    const uint32_t out = wc3_legacy_arg32(c, 0);
    const uint64_t ns = g_time->realtime_ns(g_time->ctx);
    write_filetime(out, FILETIME_UNIX_EPOCH + ns / 100ull);
    c->eax = 0;
}

static void h_get_system_time(CPU *c)
{
    const uint32_t out = wc3_legacy_arg32(c, 0);
    const uint64_t ns = g_time->realtime_ns(g_time->ctx);
    write_system_time(out, (int64_t)(ns / NS_PER_SEC),
                      (unsigned)((ns % NS_PER_SEC) / 1000000ull));
    c->eax = 0;
}

static void h_get_local_time(CPU *c)
{
    const uint32_t out = wc3_legacy_arg32(c, 0);
    int32_t year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    int32_t millisecond = 0;
    const uint64_t ns = g_time->realtime_ns(g_time->ctx);
    if (g_time->local_time &&
        g_time->local_time(g_time->ctx, ns, &year, &month, &day, &hour,
                           &minute, &second, &millisecond) == 0)
        write_system_time_local(out, year, month, day, hour, minute, second,
                                millisecond);
    c->eax = 0;
}

static void h_get_time_zone_information(CPU *c)
{
    const uint32_t out = wc3_legacy_arg32(c, 0);
    const uint64_t ns = g_time->realtime_ns(g_time->ctx);
    if (out && wc3_legacy_guest_ok(out, 172)) {
        for (uint32_t i = 0; i < 172; i += 4)
            wc3_legacy_wr32g(out + i, 0);
        int64_t offset = 0;
        if (local_offset_seconds(g_time, ns, &offset) == 0) {
            /* TIME_ZONE_INFORMATION.Bias is UTC - local, in minutes. */
            const int64_t bias_minutes = -offset / 60;
            if (bias_minutes >= INT32_MIN && bias_minutes <= INT32_MAX)
                wc3_legacy_wr32g(out, (uint32_t)(int32_t)bias_minutes);
        }
    }
    c->eax = 0;
}

static void h_filetime_to_system_time(CPU *c)
{
    const uint32_t ft = wc3_legacy_arg32(c, 0);
    const uint32_t st = wc3_legacy_arg32(c, 1);
    int ok = 0;
    const uint64_t value = read_filetime(ft, &ok);
    if (!ok || !st || !wc3_legacy_guest_ok(st, 16) ||
        value > (uint64_t)INT64_MAX) {
        c->eax = 0;
        return;
    }
    int64_t ticks = (int64_t)value - (int64_t)FILETIME_UNIX_EPOCH;
    const int64_t sec = floor_div(ticks, 10000000);
    const int64_t rem = ticks - sec * 10000000;
    write_system_time(st, sec, (unsigned)(rem / 10000));
    c->eax = 1;
}

static void h_filetime_to_local_file_time(CPU *c)
{
    const uint32_t in = wc3_legacy_arg32(c, 0);
    const uint32_t out = wc3_legacy_arg32(c, 1);
    int ok = 0;
    uint64_t value = read_filetime(in, &ok);
    if (!ok || !out || !wc3_legacy_guest_ok(out, 8) ||
        value > (uint64_t)INT64_MAX) {
        c->eax = 0;
        return;
    }
    const int64_t delta100 = (int64_t)value - (int64_t)FILETIME_UNIX_EPOCH;
    int64_t offset = 0;
    /* Wc3TimeApi takes Unix nanoseconds, so dates before 1970 pass through
     * unchanged instead of wrapping through an unsigned conversion. */
    if (delta100 >= 0 &&
        delta100 <= (INT64_MAX - MAX_TZ_OFFSET_100NS) / 100 &&
        local_offset_seconds(g_time, (uint64_t)delta100 * 100ull, &offset) == 0) {
        const int64_t ticks =
            (int64_t)FILETIME_UNIX_EPOCH + delta100 + offset * 10000000ll;
        if (ticks >= 0) value = (uint64_t)ticks;
    }
    write_filetime(out, value);
    c->eax = 1;
}

static void h_system_time_to_file_time(CPU *c)
{
    const uint32_t st = wc3_legacy_arg32(c, 0);
    const uint32_t ft = wc3_legacy_arg32(c, 1);
    if (!st || !wc3_legacy_guest_ok(st, 16) || !ft ||
        !wc3_legacy_guest_ok(ft, 8)) {
        c->eax = 0;
        return;
    }
    int year = 0;
    unsigned month = 0, day = 0, hour = 0, minute = 0, second = 0, ms = 0;
    read_system_time(st, &year, &month, &day, &hour, &minute, &second, &ms);
    if (year < 1601 || year > 30827 || month < 1 || month > 12 || day < 1 ||
        day > days_in_month(year, month) || hour > 23 || minute > 59 ||
        second > 59 || ms > 999) {
        c->eax = 0;
        return;
    }
    const int64_t sec = unix_seconds(days_from_civil(year, month, day), hour,
                                     minute, second);
    const int64_t ticks =
        (int64_t)FILETIME_UNIX_EPOCH + sec * 10000000ll + (int64_t)ms * 10000;
    if (ticks < 0) {
        c->eax = 0;
        return;
    }
    write_filetime(ft, (uint64_t)ticks);
    c->eax = 1;
}

static void h_compare_file_time(CPU *c)
{
    const uint32_t a = wc3_legacy_arg32(c, 0);
    const uint32_t b = wc3_legacy_arg32(c, 1);
    int oka = 0, okb = 0;
    const uint64_t va = read_filetime(a, &oka);
    const uint64_t vb = read_filetime(b, &okb);
    if (!oka || !okb) {
        c->eax = 0;
        return;
    }
    c->eax = va < vb ? (uint32_t)-1 : (va > vb ? 1u : 0u);
}

static void h_get_date_format(CPU *c)
{
    const uint32_t flags = wc3_legacy_arg32(c, 1);
    const uint32_t date_ptr = wc3_legacy_arg32(c, 2);
    const uint32_t format_ptr = wc3_legacy_arg32(c, 3);
    const uint32_t buf = wc3_legacy_arg32(c, 4);
    const int cch = (int)wc3_legacy_arg32(c, 5);

    int year = 0;
    unsigned month = 0, day = 0, hour = 0, minute = 0, second = 0, ms = 0;
    if (date_ptr) {
        if (!wc3_legacy_guest_ok(date_ptr, 16)) {
            c->eax = 0;
            return;
        }
        read_system_time(date_ptr, &year, &month, &day, &hour, &minute,
                         &second, &ms);
    } else {
        int32_t y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, milli = 0;
        const uint64_t now = g_time->realtime_ns(g_time->ctx);
        if (g_time->local_time(g_time->ctx, now, &y, &mo, &d, &h, &mi, &s,
                               &milli) != 0) {
            c->eax = 0;
            return;
        }
        year = y;
        month = (unsigned)mo;
        day = (unsigned)d;
        hour = (unsigned)h;
        minute = (unsigned)mi;
        second = (unsigned)s;
        ms = (unsigned)milli;
    }
    if (!valid_date(year, month, day)) {
        c->eax = 0;
        return;
    }

    char format[128];
    const char *fmt = "dd.MM.yyyy";
    int genitive_months = 0;
    if (format_ptr) {
        if (read_guest_string(format_ptr, format, (int)sizeof format) != 0) {
            c->eax = 0;
            return;
        }
        fmt = format;
    } else if (flags & DATE_MONTHDAY) {
        /* The oracle rejects DATE_MONTHDAY with no custom format. */
        c->eax = 0;
        return;
    } else if (flags & DATE_YEARMONTH) {
        fmt = "MMMM yyyy";
    } else if (flags & DATE_LONGDATE) {
        fmt = "d MMMM yyyy '\xE3.'";
        genitive_months = 1;
    }

    char out[512];
    const int len = format_date(out, (int)sizeof out, fmt, year, month, day,
                                genitive_months);
    c->eax = emit_string(buf, cch, out, len);
}

static void h_get_time_format(CPU *c)
{
    const uint32_t flags = wc3_legacy_arg32(c, 1);
    const uint32_t time_ptr = wc3_legacy_arg32(c, 2);
    const uint32_t format_ptr = wc3_legacy_arg32(c, 3);
    const uint32_t buf = wc3_legacy_arg32(c, 4);
    const int cch = (int)wc3_legacy_arg32(c, 5);

    int year = 0;
    unsigned month = 0, day = 0, hour = 0, minute = 0, second = 0, ms = 0;
    if (time_ptr) {
        if (!wc3_legacy_guest_ok(time_ptr, 16)) {
            c->eax = 0;
            return;
        }
        read_system_time(time_ptr, &year, &month, &day, &hour, &minute,
                         &second, &ms);
    } else {
        int32_t y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, milli = 0;
        const uint64_t now = g_time->realtime_ns(g_time->ctx);
        if (g_time->local_time(g_time->ctx, now, &y, &mo, &d, &h, &mi, &s,
                               &milli) != 0) {
            c->eax = 0;
            return;
        }
        year = y;
        month = (unsigned)mo;
        day = (unsigned)d;
        hour = (unsigned)h;
        minute = (unsigned)mi;
        second = (unsigned)s;
        ms = (unsigned)milli;
    }
    if (!valid_time(hour, minute, second, ms)) {
        c->eax = 0;
        return;
    }

    char format[128];
    const char *fmt = "H:mm:ss";
    if (format_ptr) {
        if (read_guest_string(format_ptr, format, (int)sizeof format) != 0) {
            c->eax = 0;
            return;
        }
        fmt = format;
    } else if (flags & TIME_NOMINUTESORSECONDS) {
        fmt = "H";
    } else if (flags & TIME_NOSECONDS) {
        fmt = "H:mm";
    }

    char out[512];
    const int len = format_time(out, (int)sizeof out, fmt, hour, minute,
                                second, flags);
    c->eax = emit_string(buf, cch, out, len);
}

/* ------------------------------------------------------------------ */
/* installation                                                        */
/* ------------------------------------------------------------------ */

typedef struct TimeOverride {
    const char *dll;
    const char *name;
    int argc;
    void (*impl)(CPU *c);
} TimeOverride;

static const TimeOverride g_overrides[] = {
    { "KERNEL32.dll", "GetTickCount", 0, h_get_tick_count },
    { "KERNEL32.dll", "QueryPerformanceCounter", 1, h_query_perf_counter },
    { "KERNEL32.dll", "QueryPerformanceFrequency", 1, h_query_perf_freq },
    { "KERNEL32.dll", "GetSystemTimeAsFileTime", 1, h_get_system_time_as_file_time },
    { "KERNEL32.dll", "GetSystemTime", 1, h_get_system_time },
    { "KERNEL32.dll", "GetLocalTime", 1, h_get_local_time },
    { "KERNEL32.dll", "GetTimeZoneInformation", 1, h_get_time_zone_information },
    { "KERNEL32.dll", "FileTimeToSystemTime", 2, h_filetime_to_system_time },
    { "KERNEL32.dll", "FileTimeToLocalFileTime", 2, h_filetime_to_local_file_time },
    { "KERNEL32.dll", "SystemTimeToFileTime", 2, h_system_time_to_file_time },
    { "KERNEL32.dll", "CompareFileTime", 2, h_compare_file_time },
    { "KERNEL32.dll", "GetDateFormatA", 6, h_get_date_format },
    { "KERNEL32.dll", "GetTimeFormatA", 6, h_get_time_format },
    { "WINMM.dll", "timeGetTime", 0, h_time_get_time },
};

/* The as-built waveOut sink used to own the WINMM!timeGetTime row.  With that
 * sink removed, the time module is the owner of the timer query as well, so it
 * adds the row when the bootstrap no longer provides one. */
static int time_bind_or_add(const TimeOverride *o)
{
    if (wc3_legacy_hle_override(o->dll, o->name, o->impl) == 0)
        return 0;
    return wc3_legacy_hle_add(o->dll, o->name, o->argc, 0, o->impl);
}

int wc3_hle_kernel32_time_install(void)
{
    const Wc3Platform *platform = wc3_platform();
    if (!platform || !platform->time.monotonic_ns ||
        !platform->time.realtime_ns || !platform->time.local_time)
        return -1;
    g_time = &platform->time;

    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (time_bind_or_add(&g_overrides[i]) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0]);
}
