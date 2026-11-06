/*
 * headless_time.c - Wc3TimeApi over the host clocks.
 *
 * The verification target currently follows host wall/monotonic time.  A
 * deterministic virtual clock belongs to the T1 schedule driver, not to the
 * platform table; keeping real clocks here means a guest Sleep is observable
 * even before that driver exists.
 */
#include "headless_internal.h"

#include <time.h>

static uint64_t headless_clock_ns(clockid_t id)
{
    struct timespec ts;
    if (clock_gettime(id, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t headless_monotonic_ns(void *ctx)
{
    (void)ctx;
    return headless_clock_ns(CLOCK_MONOTONIC);
}

static uint64_t headless_realtime_ns(void *ctx)
{
    (void)ctx;
    return headless_clock_ns(CLOCK_REALTIME);
}

static int headless_local_time(void *ctx, uint64_t unix_ns, int32_t *year,
                               int32_t *month, int32_t *day, int32_t *hour,
                               int32_t *minute, int32_t *second,
                               int32_t *millisecond)
{
    (void)ctx;
    if (!year || !month || !day || !hour || !minute || !second ||
        !millisecond)
        return -1;

    time_t sec = (time_t)(unix_ns / 1000000000ull);
    struct tm tm;
#ifdef _WIN32
    if (localtime_s(&tm, &sec) != 0) return -1;
#else
    if (!localtime_r(&sec, &tm)) return -1;
#endif
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
