/*
 * string_hle_test.c - T0 contract test for the kernel32 narrow-string subset.
 *
 * The truncation cases mirror a probe of the Windows lstrcpynA contract:
 * n=0 writes nothing, n=1 writes only the terminator, and n=4 copies three
 * characters plus the terminator.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "string_hle.h"

#define GUEST_BASE 0x10000u
#define GUEST_SIZE 0x10000u
#define MAX_ROWS 16u

static uint8_t g_mem[GUEST_SIZE];
static uint32_t g_args[8];

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
} Row;

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;

static void *guest_ptr(uint32_t addr, uint32_t size)
{
    if (addr < GUEST_BASE) return NULL;
    const uint32_t off = addr - GUEST_BASE;
    if (off > GUEST_SIZE || size > GUEST_SIZE - off) return NULL;
    return &g_mem[off];
}

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    if (g_row_count >= MAX_ROWS) return -1;
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

uint32_t wc3_legacy_arg32(const CPU *c, unsigned index)
{
    (void)c;
    return index < 8u ? g_args[index] : 0;
}

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    return guest_ptr(addr, size) != NULL;
}

uint8_t wc3_legacy_rd8g(uint32_t addr)
{
    const void *p = guest_ptr(addr, 1);
    return p ? *(const uint8_t *)p : 0;
}

void wc3_legacy_wr8g(uint32_t addr, uint8_t value)
{
    void *p = guest_ptr(addr, 1);
    if (p) *(uint8_t *)p = value;
}

void wc3_legacy_wr32g(uint32_t addr, uint32_t value)
{
    void *p = guest_ptr(addr, 4);
    if (p) memcpy(p, &value, 4);
}

void wc3_legacy_guest_zero(uint32_t addr, uint32_t size)
{
    if (!size) return;
    void *p = guest_ptr(addr, size);
    if (p) memset(p, 0, size);
}

static void (*find_impl(const char *name))(CPU *c)
{
    for (unsigned i = 0; i < g_row_count; i++) {
        if (strcmp(g_rows[i].name, name) == 0) return g_rows[i].impl;
    }
    return NULL;
}

static uint32_t invoke(const char *name, uint32_t a0, uint32_t a1, uint32_t a2)
{
    void (*impl)(CPU *c) = find_impl(name);
    if (!impl) {
        printf("missing handler: %s\n", name);
        return 0xFFFFFFFFu;
    }
    memset(g_args, 0, sizeof g_args);
    g_args[0] = a0;
    g_args[1] = a1;
    g_args[2] = a2;
    CPU c;
    memset(&c, 0, sizeof c);
    impl(&c);
    return c.eax;
}

#define CHECK(condition, message)                                         \
    do {                                                                  \
        if (!(condition)) {                                               \
            printf("FAIL: %s\n", message);                                \
            return 1;                                                     \
        }                                                                 \
    } while (0)

int main(void)
{
    const int installed = wc3_hle_kernel32_string_install();
    CHECK(installed == 7, "install count");
    CHECK(g_row_count == 7u, "row count");
    CHECK(find_impl("lstrlenA") != NULL, "lstrlenA row");
    CHECK(find_impl("lstrcpynA") != NULL, "lstrcpynA row");
    CHECK(find_impl("lstrcatA") != NULL, "lstrcatA row");
    CHECK(find_impl("IsBadStringPtrA") != NULL, "IsBadStringPtrA row");
    CHECK(find_impl("GetACP") != NULL, "GetACP row");
    CHECK(find_impl("GetOEMCP") != NULL, "GetOEMCP row");
    CHECK(find_impl("GetCPInfo") != NULL, "GetCPInfo row");

    const uint32_t text = GUEST_BASE + 0x100u;
    const uint32_t dst = GUEST_BASE + 0x200u;
    memcpy(guest_ptr(text, 7), "abcdef", 7);

    CHECK(invoke("lstrlenA", text, 0, 0) == 6u, "lstrlenA value");
    CHECK(invoke("lstrlenA", 0, 0, 0) == 0u, "lstrlenA NULL");
    CHECK(invoke("lstrlenA", GUEST_BASE + GUEST_SIZE - 1u, 0, 0) == 0u,
          "lstrlenA terminator at end");

    memset(guest_ptr(dst, 16), 0x55, 16);
    CHECK(invoke("lstrcpynA", dst, text, 0) == dst, "lstrcpynA n=0 return");
    for (unsigned i = 0; i < 16; i++)
        CHECK(((uint8_t *)guest_ptr(dst, 16))[i] == 0x55u, "lstrcpynA n=0");

    memset(guest_ptr(dst, 16), 0x55, 16);
    CHECK(invoke("lstrcpynA", dst, text, 1) == dst, "lstrcpynA n=1 return");
    CHECK(((uint8_t *)guest_ptr(dst, 16))[0] == 0u, "lstrcpynA n=1");

    memset(guest_ptr(dst, 16), 0x55, 16);
    CHECK(invoke("lstrcpynA", dst, text, 4) == dst, "lstrcpynA n=4 return");
    CHECK(memcmp(guest_ptr(dst, 4), "abc\0", 4) == 0, "lstrcpynA n=4");
    CHECK(((uint8_t *)guest_ptr(dst, 16))[4] == 0x55u, "lstrcpynA n=4 tail");

    memset(guest_ptr(dst, 16), 0x55, 16);
    CHECK(invoke("lstrcpynA", dst, text, 7) == dst, "lstrcpynA n=7 return");
    CHECK(memcmp(guest_ptr(dst, 7), "abcdef\0", 7) == 0, "lstrcpynA n=7");

    memset(guest_ptr(dst, 16), 0x55, 16);
    CHECK(invoke("lstrcpynA", dst, text, 8) == dst, "lstrcpynA n=8 return");
    CHECK(memcmp(guest_ptr(dst, 7), "abcdef\0", 7) == 0, "lstrcpynA n=8");

    memset(guest_ptr(dst, 16), 0x55, 16);
    CHECK(invoke("lstrcpynA", dst, 0, 8) == dst, "lstrcpynA NULL source");
    CHECK(((uint8_t *)guest_ptr(dst, 16))[0] == 0u,
          "lstrcpynA NULL source terminator");

    const uint32_t src = GUEST_BASE + 0x180u;
    memcpy(guest_ptr(src, 4), "def", 4);

    memset(guest_ptr(dst, 16), 0x55, 16);
    memcpy(guest_ptr(dst, 4), "abc", 4);
    CHECK(invoke("lstrcatA", dst, src, 0) == dst, "lstrcatA return");
    CHECK(memcmp(guest_ptr(dst, 7), "abcdef\0", 7) == 0, "lstrcatA append");
    CHECK(((uint8_t *)guest_ptr(dst, 16))[7] == 0x55u, "lstrcatA tail");

    memset(guest_ptr(dst, 16), 0x55, 16);
    memcpy(guest_ptr(dst, 1), "", 1);
    CHECK(invoke("lstrcatA", dst, src, 0) == dst, "lstrcatA empty dst");
    CHECK(memcmp(guest_ptr(dst, 4), "def\0", 4) == 0, "lstrcatA empty dst text");

    const uint32_t empty = GUEST_BASE + 0x1C0u;
    memset(guest_ptr(empty, 1), 0, 1);
    memcpy(guest_ptr(dst, 4), "abc", 4);
    CHECK(invoke("lstrcatA", dst, empty, 0) == dst, "lstrcatA empty src");
    CHECK(memcmp(guest_ptr(dst, 4), "abc\0", 4) == 0, "lstrcatA empty src text");

    CHECK(invoke("lstrcatA", 0, src, 0) == 0u, "lstrcatA NULL dst");
    memcpy(guest_ptr(dst, 4), "abc", 4);
    CHECK(invoke("lstrcatA", dst, 0, 0) == dst, "lstrcatA NULL src");
    CHECK(memcmp(guest_ptr(dst, 4), "abc\0", 4) == 0,
          "lstrcatA NULL src terminator");

    CHECK(invoke("IsBadStringPtrA", text, 7, 0) == 0u,
          "IsBadStringPtrA valid");
    CHECK(invoke("IsBadStringPtrA", text, 3, 0) == 0u,
          "IsBadStringPtrA prefix");
    CHECK(invoke("IsBadStringPtrA", text, 0, 0) == 0u,
          "IsBadStringPtrA zero length");
    CHECK(invoke("IsBadStringPtrA", 0, 7, 0) == 1u,
          "IsBadStringPtrA NULL");
    ((uint8_t *)guest_ptr(GUEST_BASE + GUEST_SIZE - 1u, 1))[0] = 'A';
    CHECK(invoke("IsBadStringPtrA", GUEST_BASE + GUEST_SIZE - 1u, 2, 0) == 1u,
          "IsBadStringPtrA short range");

    CHECK(invoke("GetACP", 0, 0, 0) == 1251u, "GetACP value");
    CHECK(invoke("GetOEMCP", 0, 0, 0) == 866u, "GetOEMCP value");
    const uint32_t cpinfo = GUEST_BASE + 0x300u;
    memset(guest_ptr(cpinfo, 24), 0xAA, 24);
    CHECK(invoke("GetCPInfo", 1251, cpinfo, 0) == 1u, "GetCPInfo return");
    uint32_t max_char_size = 0;
    memcpy(&max_char_size, guest_ptr(cpinfo, 4), 4);
    CHECK(max_char_size == 1u, "CPINFO MaxCharSize 1251");
    const uint8_t *cp = (const uint8_t *)guest_ptr(cpinfo, 20);
    CHECK(cp[4] == '?' && cp[5] == 0u, "CPINFO default char");
    for (unsigned i = 6; i < 20; i++)
        CHECK(cp[i] == 0u, "CPINFO lead bytes");
    CHECK(((const uint8_t *)guest_ptr(cpinfo, 24))[20] == 0xAAu,
          "CPINFO tail untouched");

    memset(guest_ptr(cpinfo, 24), 0xAA, 24);
    CHECK(invoke("GetCPInfo", 65001, cpinfo, 0) == 1u,
          "GetCPInfo UTF-8 return");
    memcpy(&max_char_size, guest_ptr(cpinfo, 4), 4);
    CHECK(max_char_size == 4u, "CPINFO MaxCharSize UTF-8");

    memset(guest_ptr(cpinfo, 24), 0xAA, 24);
    CHECK(invoke("GetCPInfo", 1200, cpinfo, 0) == 1u,
          "GetCPInfo UTF-16 return");
    memcpy(&max_char_size, guest_ptr(cpinfo, 4), 4);
    CHECK(max_char_size == 2u, "CPINFO MaxCharSize UTF-16");

    CHECK(invoke("GetCPInfo", 1251, 0, 0) == 0u, "GetCPInfo NULL");
    CHECK(invoke("GetCPInfo", 1251, GUEST_BASE + GUEST_SIZE - 19u, 0) == 0u,
          "GetCPInfo truncated buffer");

    printf("kernel32 string OK (%d handlers)\n", installed);
    return 0;
}
