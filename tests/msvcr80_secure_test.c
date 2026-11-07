/*
 * msvcr80_secure_test.c - T0 contract test for the MSVCR80 strcpy_s slice.
 *
 * The fake shim exposes one guest memory range and records the override row.
 * The test covers the successful copy, exact and short destinations, invalid
 * arguments, invalid guest ranges, and the RSIZE_MAX boundary.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "secure_hle.h"

#define MAX_ROWS 2u
#define GUEST_BASE 0x20000u
#define GUEST_SIZE 0x10000u
#define DST_ADDR (GUEST_BASE + 0x1000u)
#define SRC_ADDR (GUEST_BASE + 0x2000u)
#define INVALID_ADDR 0x1000u

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
} Row;

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;
static uint32_t g_args[8];
static uint8_t g_mem[GUEST_SIZE];
static int g_failures;

static void *guest_ptr(uint32_t addr, uint32_t size)
{
    if (addr < GUEST_BASE) return NULL;
    const uint32_t off = addr - GUEST_BASE;
    if (off > GUEST_SIZE || size > GUEST_SIZE - off) return NULL;
    return g_mem + off;
}

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    return guest_ptr(addr, size) != NULL;
}

uint8_t wc3_legacy_rd8g(uint32_t addr)
{
    const uint8_t *p = guest_ptr(addr, 1);
    return p ? *p : 0u;
}

void wc3_legacy_wr8g(uint32_t addr, uint8_t value)
{
    uint8_t *p = guest_ptr(addr, 1);
    if (p) *p = value;
}

uint32_t wc3_legacy_arg32(const CPU *cpu, unsigned index)
{
    (void)cpu;
    return index < sizeof g_args / sizeof g_args[0] ? g_args[index] : 0u;
}

void wc3_legacy_guest_copy(uint32_t dst, uint32_t src, uint32_t size)
{
    void *d = guest_ptr(dst, size);
    const void *s = guest_ptr(src, size);
    if (d && s) memmove(d, s, size);
}

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    if (!dll || !name || !impl || g_row_count >= MAX_ROWS) return -1;
    g_rows[g_row_count].dll = dll;
    g_rows[g_row_count].name = name;
    g_rows[g_row_count].impl = impl;
    g_row_count++;
    return 0;
}

static void check(int condition, const char *message)
{
    if (!condition) {
        printf("FAIL: %s\n", message);
        g_failures++;
    }
}

static void fill_guest(uint32_t addr, uint8_t value, uint32_t size)
{
    void *p = guest_ptr(addr, size);
    if (p) memset(p, value, size);
}

static void put_guest(uint32_t addr, const void *src, uint32_t size)
{
    void *p = guest_ptr(addr, size);
    if (p) memcpy(p, src, size);
}

static uint8_t get_guest8(uint32_t addr)
{
    const uint8_t *p = guest_ptr(addr, 1);
    return p ? *p : 0u;
}

static int same_guest(uint32_t addr, const void *src, uint32_t size)
{
    const void *p = guest_ptr(addr, size);
    return p && memcmp(p, src, size) == 0;
}

static uint32_t call_strcpy_s(void (*impl)(CPU *c), uint32_t dst,
                              uint32_t cap, uint32_t src)
{
    CPU c;
    memset(&c, 0, sizeof c);
    g_args[0] = dst;
    g_args[1] = cap;
    g_args[2] = src;
    c.eax = 0xDEADBEEFu;
    impl(&c);
    return c.eax;
}

int main(void)
{
    memset(g_mem, 0, sizeof g_mem);
    memset(g_args, 0, sizeof g_args);

    check(msvcr80_secure_install() == 1, "install returns one row");
    check(g_row_count == 1u, "install row count");
    if (g_row_count != 1u) return 1;
    check(strcmp(g_rows[0].dll, "MSVCR80.dll") == 0, "install dll");
    check(strcmp(g_rows[0].name, "strcpy_s") == 0, "install name");
    void (*impl)(CPU *c) = g_rows[0].impl;

    const char hello[] = "hello";

    fill_guest(DST_ADDR, 0xAAu, 16u);
    put_guest(SRC_ADDR, hello, sizeof hello);
    check(call_strcpy_s(impl, DST_ADDR, 16u, SRC_ADDR) == 0u,
          "valid return");
    check(same_guest(DST_ADDR, hello, sizeof hello), "valid copy");
    check(get_guest8(DST_ADDR + 6u) == 0xAAu, "valid no overrun");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(impl, DST_ADDR, 6u, SRC_ADDR) == 0u,
          "exact fit return");
    check(same_guest(DST_ADDR, hello, sizeof hello), "exact fit copy");
    check(get_guest8(DST_ADDR + 6u) == 0xAAu, "exact fit no overrun");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(impl, DST_ADDR, 5u, SRC_ADDR) == 34u,
          "short return ERANGE");
    check(get_guest8(DST_ADDR) == 0u, "short clears first byte");
    check(get_guest8(DST_ADDR + 1u) == 0xAAu,
          "short writes only the terminator");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(impl, DST_ADDR, 0u, SRC_ADDR) == 22u,
          "cap zero EINVAL");
    check(get_guest8(DST_ADDR) == 0xAAu, "cap zero no write");

    check(call_strcpy_s(impl, 0u, 16u, SRC_ADDR) == 22u,
          "null dst EINVAL");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(impl, DST_ADDR, 16u, 0u) == 22u,
          "null src EINVAL");
    check(get_guest8(DST_ADDR) == 0u, "null src clears first byte");
    check(get_guest8(DST_ADDR + 1u) == 0xAAu,
          "null src writes only the terminator");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(impl, DST_ADDR, 0x80000000u, SRC_ADDR) == 22u,
          "huge cap EINVAL");
    check(get_guest8(DST_ADDR) == 0xAAu, "huge cap no write");

    check(call_strcpy_s(impl, INVALID_ADDR, 16u, SRC_ADDR) == 22u,
          "invalid dst EINVAL");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(impl, DST_ADDR, 16u, INVALID_ADDR) == 22u,
          "invalid src EINVAL");
    check(get_guest8(DST_ADDR) == 0u,
          "invalid src clears first byte");

    fill_guest(SRC_ADDR, 'A', 8u);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(impl, DST_ADDR, 4u, SRC_ADDR) == 34u,
          "no terminator ERANGE");
    check(get_guest8(DST_ADDR) == 0u,
          "no terminator clears first byte");

    fill_guest(SRC_ADDR, 0u, 1u);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(impl, DST_ADDR, 1u, SRC_ADDR) == 0u,
          "empty source return");
    check(get_guest8(DST_ADDR) == 0u, "empty source copy");
    check(get_guest8(DST_ADDR + 1u) == 0xAAu,
          "empty source no overrun");

    put_guest(SRC_ADDR, hello, sizeof hello);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(impl, DST_ADDR, 0x7FFFFFFFu, SRC_ADDR) == 0u,
          "RSIZE_MAX cap return");
    check(same_guest(DST_ADDR, hello, sizeof hello),
          "RSIZE_MAX cap copy");

    const uint32_t last = GUEST_BASE + GUEST_SIZE - 1u;
    fill_guest(last, 0u, 1u);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(impl, DST_ADDR, 2u, last) == 0u,
          "last-byte source return");
    check(get_guest8(DST_ADDR) == 0u, "last-byte source copy");

    fill_guest(last, 'A', 1u);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(impl, DST_ADDR, 4u, last) == 22u,
          "source overrun EINVAL");
    check(get_guest8(DST_ADDR) == 0u,
          "source overrun clears first byte");

    fill_guest(last, 0xAAu, 1u);
    fill_guest(SRC_ADDR, 0u, 1u);
    check(call_strcpy_s(impl, last, 1u, SRC_ADDR) == 0u,
          "last-byte dst return");
    check(get_guest8(last) == 0u, "last-byte dst copy");

    if (g_failures) {
        printf("msvcr80 secure: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("msvcr80 secure OK (1 handler)\n");
    return 0;
}
