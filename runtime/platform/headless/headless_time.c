/*
 * headless_time.c - Wc3TimeApi over host or virtual clocks.
 *
 * The default backend follows host wall/monotonic time.  A T1 schedule opts
 * into a virtual monotonic clock starting at zero and a fixed realtime epoch;
 * local_time() then uses the configured fixed UTC offset, not the host
 * timezone.  This keeps a difftest independent of the machine date and locale.
 */
#include "headless_internal.h"

#include <time.h>

static uint64_t headless_host_clock_ns(clockid_t id)
{
    struct timespec ts;
    if (clock_gettime(id, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t headless_now_ns(HeadlessState *st)
{
    if (st && st->virtual_clock)
        return atomic_load_explicit(&st->clock_ns, memory_order_acquire);
    return headless_host_clock_ns(CLOCK_MONOTONIC);
}

static uint64_t headless_monotonic_ns(void *ctx)
{
    return headless_now_ns((HeadlessState *)ctx);
}

static uint64_t headless_realtime_ns(void *ctx)
{
    HeadlessState *st = (HeadlessState *)ctx;
    if (st && st->virtual_clock) {
        const uint64_t now = headless_now_ns(st);
        if (UINT64_MAX - st->epoch_unix_ns < now) return UINT64_MAX;
        return st->epoch_unix_ns + now;
    }
    return headless_host_clock_ns(CLOCK_REALTIME);
}

static int headless_local_time(void *ctx, uint64_t unix_ns, int32_t *year,
                               int32_t *month, int32_t *day, int32_t *hour,
                               int32_t *minute, int32_t *second,
                               int32_t *millisecond)
{
    HeadlessState *st = (HeadlessState *)ctx;
    if (!year || !month || !day || !hour || !minute || !second ||
        !millisecond)
        return -1;

    time_t sec = (time_t)(unix_ns / 1000000000ull);
    struct tm tm;
    if (st && st->virtual_clock) {
        sec += (time_t)st->timezone_minutes * 60;
#ifdef _WIN32
        if (gmtime_s(&tm, &sec) != 0) return -1;
#else
        if (!gmtime_r(&sec, &tm)) return -1;
#endif
    } else {
#ifdef _WIN32
        if (localtime_s(&tm, &sec) != 0) return -1;
#else
        if (!localtime_r(&sec, &tm)) return -1;
#endif
    }
    *year = tm.tm_year + 1900;
    *month = tm.tm_mon + 1;
    *day = tm.tm_mday;
    *hour = tm.tm_hour;
    *minute = tm.tm_min;
    *second = tm.tm_sec;
    *millisecond = (int32_t)((unix_ns % 1000000000ull) / 1000000ull);
    return 0;
}

void headless_time_install(Wc3Platform *platform, HeadlessState *st)
{
    platform->time.ctx = st;
    platform->time.monotonic_ns = headless_monotonic_ns;
    platform->time.realtime_ns = headless_realtime_ns;
    platform->time.local_time = headless_local_time;
}
