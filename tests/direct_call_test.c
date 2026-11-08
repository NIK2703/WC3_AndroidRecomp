/*
 * direct_call_test.c - verifies the P0-1 devirtualization contract.
 *
 * Builds standalone (no game assets, no Android):
 *   gcc -std=c11 -O2 -Wall -Wextra -Werror \
 *       -I runtime/guest tests/direct_call_test.c -o direct_call_test
 *
 * The fake CPU mirrors the ABI-4 fields owned by the real CPU.  The real
 * header is intentionally usable without the rest of cpu.h so the test can
 * stay asset-free and compile with one include path.
 */
#include <stdint.h>
#include <stdio.h>

#define WC3_CPU_DEFINED 1
#define WC3_DISP_PENDING_VA 0u
#define WC3_DISP_PENDING_FN 1u

typedef struct CPU {
    uint32_t disp_depth;
    uint32_t disp_pending_valid;
    uint32_t disp_pending_kind;
    uintptr_t disp_pending_target;
    uint32_t disp_draining;
    int log[64];
    int n;
    long hops;
} CPU;

#include "direct_call.h"

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

static int g_va_target;
static int g_va_hits;

/* Only VA pendings use the legacy dispatcher.  The test records the target
 * instead of pulling in the full boot dispatcher. */
void dispatch(CPU *c, uint32_t target)
{
    (void)c;
    g_va_target = (int)target;
    g_va_hits++;
}

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

/* A nested direct call inside a drain target must drain its own tail before
 * returning to the target that made the call (TA-13 / ABI-5.3). */
static void nested_leaf(CPU *c) { note(c, 8); }
static void nested_tail(CPU *c) {
    note(c, 7);
    WC3_DIRECT_TAIL(nested_leaf, c);
    return;
}
static void nested_caller(CPU *c) {
    note(c, 6);
    WC3_DIRECT_CALL(nested_tail, c);
    note(c, 9);
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
    CHECK(wc3_guest_depth_get(&c) == 0u);
    CHECK(c.disp_pending_valid == 0u);

    /* 2. nested direct calls keep their order and depth returns to zero */
    c.n = 0;
    WC3_DIRECT_CALL(middle, &c);
    CHECK(c.n == 3);
    CHECK(c.log[0] == 4 && c.log[1] == 5 && c.log[2] == 6);
    CHECK(wc3_guest_depth_get(&c) == 0u);

    /* 3. a nested tail target is drained before the nested caller resumes */
    c.n = 0;
    WC3_DIRECT_CALL(nested_caller, &c);
    CHECK(c.n == 4);
    CHECK(c.log[0] == 6 && c.log[1] == 7 && c.log[2] == 8 && c.log[3] == 9);
    CHECK(wc3_guest_depth_get(&c) == 0u);

    /* 4. a long tail chain must be a loop, not host-stack recursion */
    c.hops = 0;
    WC3_DIRECT_CALL(chain, &c);
    CHECK(c.hops == 100000L);
    CHECK(wc3_guest_depth_get(&c) == 0u);

    /* 5. top-level drain resolves a legacy VA pending through dispatch() */
    g_va_hits = 0;
    c.disp_pending_valid = 1u;
    c.disp_pending_kind = WC3_DISP_PENDING_VA;
    c.disp_pending_target = 0x1234u;
    WC3_GUEST_DRAIN(&c);
    CHECK(g_va_hits == 1);
    CHECK(g_va_target == 0x1234);
    CHECK(c.disp_pending_valid == 0u);

    /* 6. top-level drain is idempotent */
    WC3_GUEST_DRAIN(&c);
    CHECK(c.disp_pending_valid == 0u);

    printf("direct_call_test: OK (tail chain, nesting, 100000 hops)\n");
    return 0;
}
