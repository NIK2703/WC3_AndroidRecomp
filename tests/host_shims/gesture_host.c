/*
 * The test command renames clock_gettime() to wc3_test_clock_gettime() for the
 * vendored sources.  Undo that macro before <time.h> is included, otherwise
 * the toolchain's own declaration is rewritten and clashes with the definition
 * below.
 */
#ifdef clock_gettime
#undef clock_gettime
#endif

#include "open_memstream.h"

#include <stdint.h>
#include <time.h>

uint64_t g_fake_ms;

int wc3_test_clock_gettime(clockid_t clk_id, struct timespec *tp) {
    (void)clk_id;
    if (!tp) return -1;
    tp->tv_sec = (time_t)(g_fake_ms / 1000u);
    tp->tv_nsec = (long)((g_fake_ms % 1000u) * 1000000u);
    return 0;
}

#ifdef _WIN32
int clock_gettime64(clockid_t clk_id, struct _timespec64 *tp) {
    (void)clk_id;
    if (!tp) return -1;
    tp->tv_sec = (time_t)(g_fake_ms / 1000u);
    tp->tv_nsec = (long)((g_fake_ms % 1000u) * 1000000u);
    return 0;
}
#endif

/* MinGW has no open_memstream().  The vendored JSON serializer references it,
 * but the touch test never serializes a profile. */
FILE *wc3_host_open_memstream(char **ptr, size_t *sizeloc) {
    (void)ptr;
    (void)sizeloc;
    return NULL;
}
