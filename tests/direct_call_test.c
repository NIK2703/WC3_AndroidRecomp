/*
 * direct_call_test.c - verifies the P0-1 devirtualization contract.
 *
 * Builds standalone (no game assets, no Android):
 *   gcc -std=c11 -O2 -Wall -Wextra -Werror \
 *       -I runtime/guest tests/direct_call_test.c -o direct_call_test
 */
#include <stdio.h>

#include "direct_call.h"

struct CPU {
    int log[64];
    int n;
    long hops;
};

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

static void note(CPU *c, int v) {
    if (c->n < 64) c->log[c->n++] = v;
}

static void f_c(CPU *c) { note(c, 3); }
static void f_b(CPU *c) { note(c, 2); WC3_DIRECT_TAIL(f_c, c); return; }
static void f_a(CPU *c) { note(c, 1); WC3_DIRECT_TAIL(f_b, c); return; }

static void g_caller(CPU *c) {
    note(c, 0);
    WC3_DIRECT_CALL(f_a, c);
    /* Must run only AFTER the tail chain f_b -> f_c has been drained. */
    note(c, 9);
}

static void inner(CPU *c) { note(c, 5); }
static void middle(CPU *c) {
    note(c, 4);
    WC3_DIRECT_CALL(inner, c);
    note(c, 6);
}

static void chain(CPU *c) {
    c->hops++;
    if (c->hops < 100000L) WC3_DIRECT_TAIL(chain, c);
    return;
}

int main(void)
{
    CPU c = { .n = 0, .hops = 0 };

    /* 1. tail chain must be drained before the caller continues */
    WC3_DIRECT_CALL(g_caller, &c);
    CHECK(c.n == 5);
    CHECK(c.log[0] == 0 && c.log[1] == 1 && c.log[2] == 2 &&
          c.log[3] == 3 && c.log[4] == 9);
    CHECK(wc3_guest_depth_get() == 0u);
    CHECK(wc3_guest_pending_fn == 0);

    /* 2. nested direct calls keep their order and depth returns to zero */
    c.n = 0;
    WC3_DIRECT_CALL(middle, &c);
    CHECK(c.n == 3);
    CHECK(c.log[0] == 4 && c.log[1] == 5 && c.log[2] == 6);
    CHECK(wc3_guest_depth_get() == 0u);

    /* 3. a long tail chain must be a loop, not host-stack recursion */
    c.hops = 0;
    WC3_DIRECT_CALL(chain, &c);
    CHECK(c.hops == 100000L);
    CHECK(wc3_guest_depth_get() == 0u);

    /* 4. top-level drain is idempotent */
    WC3_GUEST_DRAIN(&c);
    CHECK(wc3_guest_pending_fn == 0);

    printf("direct_call_test: OK (tail chain, nesting, 100000 hops)\n");
    return 0;
}
