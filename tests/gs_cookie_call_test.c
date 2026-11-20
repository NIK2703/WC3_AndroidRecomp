/*
 * gs_cookie_call_test.c - semantics of the specialized MSVC /GS guard frame.
 *
 * Builds standalone against the real CPU and helper sources:
 *   cc -std=c11 -O2 -Wall -Wextra -Werror \
 *       -I runtime/cpu -I runtime/guest \
 *       tests/gs_cookie_call_test.c runtime/cpu/guest_host.c \
 *       runtime/guest/gs_cookie_call.c -o gs_cookie_call_test
 *
 * The test uses a host buffer as guest VA 0.  cpu.h's rd32() reaches it
 * through g_guest_host_base, so the helper runs its real flags_sub/rd32 path
 * instead of a mock.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "cpu.h"
#include "direct_call.h"
#include "gs_cookie_call.h"

static uint32_t cookie_slot;

static int log_value[8];
static int log_count;
static uint32_t fail_depth;
static uint32_t fail2_depth;
static uint32_t fail_draining;

/* Only FN pendings occur here; a VA pending would mean the guard helper
 * leaked a target into the legacy dispatcher. */
void dispatch(CPU *c, uint32_t target)
{
    (void)c;
    (void)target;
    abort();
}

static void fail2(CPU *c)
{
    log_value[log_count++] = 2;
    fail2_depth = c->disp_depth;
}

static void fail(CPU *c)
{
    log_value[log_count++] = 1;
    fail_depth = c->disp_depth;
    fail_draining = c->disp_draining;
    WC3_DIRECT_TAIL(fail2, c);
}

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

int main(void)
{
    CPU c = {0};

    g_guest_host_base = (uintptr_t)&cookie_slot;
    cookie_slot = 0x11223344u;

    /* Success: the helper performs the guard's guest ret and restores the
     * caller's depth/draining without publishing anything. */
    c.disp_depth = 3u;
    c.disp_draining = 1u;
    c.esp = 0x100u;
    c.ecx = cookie_slot;

    wc3_guest_call_gs_cookie(&c, 0u, fail);

    CHECK(c.esp == 0x104u);
    CHECK(c.disp_depth == 3u);
    CHECK(c.disp_draining == 1u);
    CHECK(c.disp_pending_valid == 0u);
    CHECK(c.zf == 1u);
    CHECK(log_count == 0);

    /* Mismatch: publish the exact fail function, drain it at the saved depth,
     * let its tail chain resolve, then restore the caller's state. */
    c.esp = 0x100u;
    c.ecx = cookie_slot + 1u;
    c.disp_draining = 0u;
    log_count = 0;

    wc3_guest_call_gs_cookie(&c, 0u, fail);

    CHECK(c.esp == 0x100u);
    CHECK(c.disp_depth == 3u);
    CHECK(c.disp_draining == 0u);
    CHECK(c.disp_pending_valid == 0u);
    CHECK(c.zf == 0u);
    CHECK(log_count == 2);
    CHECK(log_value[0] == 1 && log_value[1] == 2);
    CHECK(fail_depth == 3u);
    CHECK(fail2_depth == 3u);
    CHECK(fail_draining == 1u);

    puts("gs_cookie_call_test: OK (success, mismatch, esp, depth/drain order)");
    return 0;
}
