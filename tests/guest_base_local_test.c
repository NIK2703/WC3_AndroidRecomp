/*
 * guest_base_local_test.c - explicit-base guest-memory helpers.
 *
 * These helpers are the runtime half of tools/lift/csrc_local/guest_base.c.
 * The pass chooses a write-once base snapshot and redirects rd/wr/push/pop to
 * these forms; this test exercises every redirected width and the stack,
 * scalar, and vector helpers against a real guest-window base.
 *
 *   cc -std=c11 -O2 -Wall -Wextra -Werror \
 *       -I runtime/cpu -I runtime/guest \
 *       tests/guest_base_local_test.c runtime/cpu/guest_host.c \
 *       -o guest_base_local_test
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest_base_local.h"

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

static _Alignas(64) unsigned char arena[4096];

int main(void)
{
    CPU c = {0};
    XMM x;
    XMM y;
    uint64_t q;
    double d;
    float f;
    size_t i;

    g_guest_host_base = (uintptr_t)arena;
    memset(arena, 0, sizeof arena);

    CHECK(guest_host_ptr_at(0x20u, g_guest_host_base) == arena + 0x20);

    wr8_at(0x10u, 0xA5u, g_guest_host_base);
    CHECK(rd8_at(0x10u, g_guest_host_base) == 0xA5u);
    CHECK(arena[0x10] == 0xA5u);

    wr16_at(0x12u, 0xBEEFu, g_guest_host_base);
    CHECK(rd16_at(0x12u, g_guest_host_base) == 0xBEEFu);

    wr32_at(0x14u, 0x12345678u, g_guest_host_base);
    CHECK(rd32_at(0x14u, g_guest_host_base) == 0x12345678u);
    CHECK(arena[0x14] == 0x78u && arena[0x17] == 0x12u);

    wr64_at(0x18u, 0x0123456789ABCDEFull, g_guest_host_base);
    q = rd64_at(0x18u, g_guest_host_base);
    CHECK(q == 0x0123456789ABCDEFull);

    c.esp = 0x100u;
    push32_at(&c, 0xDEADBEEFu, g_guest_host_base);
    CHECK(c.esp == 0x0FCu);
    CHECK(rd32_at(0x0FCu, g_guest_host_base) == 0xDEADBEEFu);
    CHECK(pop32_at(&c, g_guest_host_base) == 0xDEADBEEFu);
    CHECK(c.esp == 0x100u);

    wrss_at(0x20u, 1.5f, g_guest_host_base);
    f = rdss_at(0x20u, g_guest_host_base);
    CHECK(f == 1.5f);

    wrsd_at(0x28u, -2.25, g_guest_host_base);
    d = rdsd_at(0x28u, g_guest_host_base);
    CHECK(d == -2.25);

    for (i = 0; i < sizeof x.u8; i++) x.u8[i] = (uint8_t)(i * 3u + 1u);
    wrxm_at(0x30u, x, g_guest_host_base);
    y = rdxm_at(0x30u, g_guest_host_base);
    CHECK(memcmp(&x, &y, sizeof x) == 0);

    g_guest_host_base = 0u;
    puts("guest_base_local_test: OK (widths, stack, scalar, vector)");
    return 0;
}
