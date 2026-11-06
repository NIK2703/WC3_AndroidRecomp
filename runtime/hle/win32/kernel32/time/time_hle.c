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
#include "legacy_shim.h"
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

/* ------------------------------------------------------------------ */
/* installation                                                        */
/* ------------------------------------------------------------------ */

typedef struct TimeOverride {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
} TimeOverride;

static const TimeOverride g_overrides[] = {
    { "KERNEL32.dll", "GetTickCount", h_get_tick_count },
    { "KERNEL32.dll", "QueryPerformanceCounter", h_query_perf_counter },
    { "KERNEL32.dll", "QueryPerformanceFrequency", h_query_perf_freq },
    { "KERNEL32.dll", "GetSystemTimeAsFileTime", h_get_system_time_as_file_time },
    { "KERNEL32.dll", "GetSystemTime", h_get_system_time },
    { "KERNEL32.dll", "GetLocalTime", h_get_local_time },
    { "KERNEL32.dll", "GetTimeZoneInformation", h_get_time_zone_information },
    { "KERNEL32.dll", "FileTimeToSystemTime", h_filetime_to_system_time },
    { "KERNEL32.dll", "FileTimeToLocalFileTime", h_filetime_to_local_file_time },
    { "KERNEL32.dll", "SystemTimeToFileTime", h_system_time_to_file_time },
    { "KERNEL32.dll", "CompareFileTime", h_compare_file_time },
    { "WINMM.dll", "timeGetTime", h_time_get_time },
};

int wc3_hle_kernel32_time_install(void)
{
    const Wc3Platform *platform = wc3_platform();
    if (!platform || !platform->time.monotonic_ns ||
        !platform->time.realtime_ns || !platform->time.local_time)
        return -1;
    g_time = &platform->time;

    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override(g_overrides[i].dll, g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0]);
}
