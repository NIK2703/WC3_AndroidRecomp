/*
 * window_settle_test.c - T0 check for the cold-start settle rule.
 *
 * The Android boot thread needs a surface that has survived one 300 ms
 * interval.  The pure rule is shared with host_shell.c so it can be checked
 * without the NDK: a surface published before the thread starts must not add a
 * full extra interval, while a fresh or backwards-stamped surface must.
 */
#include <stdint.h>
#include <stdio.h>

#include "window_settle.h"

#define SETTLE_MS 300u

static int check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (check(wc3_window_settle_remaining_ms(0u, 1000u, SETTLE_MS) ==
                  SETTLE_MS,
              "unpublished surface waits full interval"))
        return 1;
    if (check(wc3_window_settle_remaining_ms(1000u, 1000u, SETTLE_MS) ==
                  SETTLE_MS,
              "just-published surface waits full interval"))
        return 1;
    if (check(wc3_window_settle_remaining_ms(1000u, 1200u, SETTLE_MS) ==
                  100u,
              "partially settled surface waits remainder"))
        return 1;
    if (check(wc3_window_settle_remaining_ms(1000u, 1300u, SETTLE_MS) == 0u,
              "settled surface is ready"))
        return 1;
    if (check(wc3_window_settle_remaining_ms(1000u, 2000u, SETTLE_MS) == 0u,
              "old surface is ready"))
        return 1;
    if (check(wc3_window_settle_remaining_ms(1000u, 900u, SETTLE_MS) ==
                  SETTLE_MS,
              "backwards clock is treated as fresh"))
        return 1;
    if (check(wc3_window_settle_remaining_ms(1000u, 1000u, 0u) == 0u,
              "zero settle interval is ready"))
        return 1;

    printf("window settle: ok\n");
    return 0;
}
