/*
 * msvcr80_error_test.c - T0 contract test for the MSVCR80 strerror slice.
 *
 * The expected table is the MSVCR80 8.00.50727.762 table, independently
 * transcribed in the test so a module typo is visible.  The fake shim provides
 * one guest range and one per-process allocation, which is enough to observe
 * the per-thread buffer reuse and the allocation-failure path.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "hle_bridge.h"
#include "strerror_hle.h"

#define MAX_ROWS 4u
#define GUEST_BASE 0x20000u
#define GUEST_SIZE 0x10000u
#define BUFFER_ADDR (GUEST_BASE + 0x1000u)

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
} Row;

static const char *const g_expected[] = {
    "No error",
    "Operation not permitted",
    "No such file or directory",
    "No such process",
    "Interrupted function call",
    "Input/output error",
    "No such device or address",
    "Arg list too long",
    "Exec format error",
    "Bad file descriptor",
    "No child processes",
    "Resource temporarily unavailable",
    "Not enough space",
    "Permission denied",
    "Bad address",
    "Unknown error",
    "Resource device",
    "File exists",
    "Improper link",
    "No such device",
    "Not a directory",
    "Is a directory",
    "Invalid argument",
    "Too many open files in system",
    "Too many open files",
    "Inappropriate I/O control operation",
    "Unknown error",
    "File too large",
    "No space left on device",
    "Invalid seek",
    "Read-only file system",
    "Too many links",
    "Broken pipe",
    "Domain error",
    "Result too large",
    "Unknown error",
    "Resource deadlock avoided",
    "Unknown error",
    "Filename too long",
    "No locks available",
    "Function not implemented",
    "Directory not empty",
    "Illegal byte sequence",
};

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;
static uint32_t g_args[8];
static uint8_t g_mem[GUEST_SIZE];
static int g_failures;
static int g_fail_alloc;
static unsigned g_alloc_count;
static uint32_t g_alloc_size;
static uint32_t g_alloc_cursor;

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

uint32_t wc3_legacy_guest_alloc0(uint32_t size)
{
    if (g_fail_alloc) return 0;
    if (size > GUEST_SIZE) return 0;
    const uint32_t addr = g_alloc_cursor;
    void *p = guest_ptr(addr, size);
    if (!p) return 0;
    memset(p, 0, size);
    g_alloc_cursor += (size + 7u) & ~7u;
    g_alloc_count++;
    g_alloc_size = size;
    return addr;
}

void wc3_legacy_wr8g(uint32_t addr, uint8_t value)
{
    uint8_t *p = guest_ptr(addr, 1);
    if (p) *p = value;
}

void wc3_legacy_wr32g(uint32_t addr, uint32_t value)
{
    void *p = guest_ptr(addr, 4);
    if (p) memcpy(p, &value, 4);
}

uint32_t wc3_legacy_rd32g(uint32_t addr)
{
    const void *p = guest_ptr(addr, 4);
    if (!p) return 0;
    uint32_t value;
    memcpy(&value, p, 4);
    return value;
}

uint32_t wc3_legacy_arg32(const CPU *cpu, unsigned index)
{
    (void)cpu;
    return index < sizeof g_args / sizeof g_args[0] ? g_args[index] : 0u;
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

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    (void)argc;
    (void)cdecl_abi;
    return wc3_legacy_hle_override(dll, name, impl);
}

static void check(int condition, const char *message)
{
    if (!condition) {
        printf("FAIL: %s\n", message);
        g_failures++;
    }
}

static uint32_t call_strerror(void (*impl)(CPU *c), int32_t errnum)
{
    CPU c;
    memset(&c, 0, sizeof c);
    g_args[0] = (uint32_t)errnum;
    c.eax = 0xDEADBEEFu;
    impl(&c);
    return c.eax;
}

static void (*find_row(const char *name))(CPU *c)
{
    for (unsigned i = 0; i < g_row_count; i++)
        if (strcmp(g_rows[i].name, name) == 0) return g_rows[i].impl;
    return NULL;
}

static uint32_t call_errno(void (*impl)(CPU *c))
{
    CPU c;
    memset(&c, 0, sizeof c);
    c.eax = 0xDEADBEEFu;
    impl(&c);
    return c.eax;
}

static int guest_string_is(uint32_t addr, const char *expected)
{
    const size_t length = strlen(expected);
    if (!wc3_legacy_guest_ok(addr, (uint32_t)length + 1u)) return 0;
    const uint8_t *p = guest_ptr(addr, (uint32_t)length + 1u);
    return p && memcmp(p, expected, length + 1u) == 0;
}

int main(void)
{
    memset(g_mem, 0, sizeof g_mem);
    memset(g_args, 0, sizeof g_args);
    g_alloc_cursor = BUFFER_ADDR;

    check(msvcr80_error_install() == 2, "install returns two rows");
    check(g_row_count == 2u, "install row count");
    if (g_row_count != 2u) return 1;
    check(strcmp(g_rows[0].dll, "MSVCR80.dll") == 0, "install dll");
    check(strcmp(g_rows[0].name, "strerror") == 0, "install name");
    check(find_row("_errno") != NULL, "install _errno");
    void (*impl)(CPU *c) = g_rows[0].impl;

    g_fail_alloc = 1;
    check(call_strerror(impl, 0) == 0u, "allocation failure returns null");
    g_fail_alloc = 0;

    const uint32_t first = call_strerror(impl, 0);
    check(first == BUFFER_ADDR, "first call returns the buffer");
    check(g_alloc_count == 1u, "first call allocates once");
    check(g_alloc_size == 134u, "buffer size matches the CRT");
    check(guest_string_is(first, "No error"), "errno zero message");

    for (int32_t errnum = 0; errnum < (int32_t)(sizeof g_expected / sizeof g_expected[0]);
         errnum++) {
        const uint32_t p = call_strerror(impl, errnum);
        check(p == first, "every call reuses the thread buffer");
        check(guest_string_is(p, g_expected[errnum]), "known errno message");
    }
    check(g_alloc_count == 1u, "known messages allocate only once");

    const int32_t unknown[] = { 43, 999, -1, INT32_MIN };
    for (unsigned i = 0; i < sizeof unknown / sizeof unknown[0]; i++) {
        const uint32_t p = call_strerror(impl, unknown[i]);
        check(p == first, "unknown value uses the thread buffer");
        check(guest_string_is(p, "Unknown error"), "unknown value message");
    }
    check(g_alloc_count == 1u, "unknown messages allocate only once");

    void (*errno_impl)(CPU *c) = find_row("_errno");
    g_fail_alloc = 1;
    check(call_errno(errno_impl) == 0u, "_errno allocation failure");
    g_fail_alloc = 0;

    const uint32_t errno_cell = call_errno(errno_impl);
    check(errno_cell != 0u, "_errno returns a cell");
    check(errno_cell != first, "_errno cell is separate from strerror");
    check(g_alloc_count == 2u, "_errno allocates once");
    check(g_alloc_size == 4u, "_errno cell size");
    check(wc3_legacy_rd32g(errno_cell) == 0u, "_errno starts at zero");

    wc3_legacy_wr32g(errno_cell, 22u);
    check(call_errno(errno_impl) == errno_cell, "_errno reuses the thread cell");
    check(wc3_legacy_rd32g(errno_cell) == 22u, "_errno keeps the guest value");
    check(g_alloc_count == 2u, "_errno allocates only once");
    check(call_strerror(impl, 0) == first, "strerror reuses its own buffer");
    check(guest_string_is(first, "No error"),
          "_errno does not disturb the strerror buffer");

    if (g_failures) {
        printf("msvcr80 error: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("msvcr80 error OK (2 handlers)\n");
    return 0;
}
