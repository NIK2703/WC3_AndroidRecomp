/*
 * msvcr80_secure_test.c - T0 contract test for the MSVCR80 secure strings.
 *
 * The fake shim exposes one guest memory range and records the override rows.
 * The test covers strcpy_s and strncpy_s: successful copies, exact and short
 * destinations, invalid arguments, invalid guest ranges, count-limited and
 * _TRUNCATE forms, overlap behavior, and the observed huge-size acceptance.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "hle_bridge.h"
#include "secure_hle.h"

#define MAX_ROWS 3u
#define GUEST_BASE 0x20000u
#define GUEST_SIZE 0x10000u
#define DST_ADDR (GUEST_BASE + 0x1000u)
#define SRC_ADDR (GUEST_BASE + 0x2000u)
#define INVALID_ADDR 0x1000u

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
    const char *kind;
    int argc;
    int cdecl_abi;
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
    /* The as-built table contains strcpy_s only.  strncpy_s must take the add
     * path, which is the regression this fake shim pins for install. */
    if (!dll || !name || !impl || g_row_count >= MAX_ROWS) return -1;
    if (strcmp(name, "strcpy_s") != 0) return -1;
    g_rows[g_row_count].dll = dll;
    g_rows[g_row_count].name = name;
    g_rows[g_row_count].impl = impl;
    g_rows[g_row_count].kind = "override";
    g_rows[g_row_count].argc = 3;
    g_rows[g_row_count].cdecl_abi = 1;
    g_row_count++;
    return 0;
}

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    if (!dll || !name || !impl || g_row_count >= MAX_ROWS) return -1;
    if (argc < 0) return -1;
    g_rows[g_row_count].dll = dll;
    g_rows[g_row_count].name = name;
    g_rows[g_row_count].impl = impl;
    g_rows[g_row_count].kind = "add";
    g_rows[g_row_count].argc = argc;
    g_rows[g_row_count].cdecl_abi = cdecl_abi;
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

static uint32_t call_strncpy_s(void (*impl)(CPU *c), uint32_t dst,
                               uint32_t cap, uint32_t src, uint32_t count)
{
    CPU c;
    memset(&c, 0, sizeof c);
    g_args[0] = dst;
    g_args[1] = cap;
    g_args[2] = src;
    g_args[3] = count;
    c.eax = 0xDEADBEEFu;
    impl(&c);
    return c.eax;
}

int main(void)
{
    memset(g_mem, 0, sizeof g_mem);
    memset(g_args, 0, sizeof g_args);

    check(msvcr80_secure_install() == 2, "install returns two rows");
    check(g_row_count == 2u, "install row count");
    if (g_row_count != 2u) return 1;
    check(strcmp(g_rows[0].dll, "MSVCR80.dll") == 0, "install dll");
    check(strcmp(g_rows[1].dll, "MSVCR80.dll") == 0, "install second dll");
    check(strcmp(g_rows[0].name, "strcpy_s") == 0, "install strcpy_s name");
    check(strcmp(g_rows[1].name, "strncpy_s") == 0,
          "install strncpy_s name");
    check(strcmp(g_rows[0].kind, "override") == 0,
          "strcpy_s keeps the as-built row");
    check(g_rows[0].argc == 3 && g_rows[0].cdecl_abi == 1,
          "strcpy_s as-built arity");
    check(strcmp(g_rows[1].kind, "add") == 0,
          "strncpy_s is appended");
    check(g_rows[1].argc == 4 && g_rows[1].cdecl_abi == 1,
          "strncpy_s cdecl arity");
    void (*cpy)(CPU *c) = g_rows[0].impl;
    void (*ncpy)(CPU *c) = g_rows[1].impl;

    const char hello[] = "hello";

    fill_guest(DST_ADDR, 0xAAu, 16u);
    put_guest(SRC_ADDR, hello, sizeof hello);
    check(call_strcpy_s(cpy, DST_ADDR, 16u, SRC_ADDR) == 0u,
          "valid return");
    check(same_guest(DST_ADDR, hello, sizeof hello), "valid copy");
    check(get_guest8(DST_ADDR + 6u) == 0xAAu, "valid no overrun");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(cpy, DST_ADDR, 6u, SRC_ADDR) == 0u,
          "exact fit return");
    check(same_guest(DST_ADDR, hello, sizeof hello), "exact fit copy");
    check(get_guest8(DST_ADDR + 6u) == 0xAAu, "exact fit no overrun");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(cpy, DST_ADDR, 5u, SRC_ADDR) == 34u,
          "short return ERANGE");
    check(get_guest8(DST_ADDR) == 0u, "short clears first byte");
    check(get_guest8(DST_ADDR + 1u) == 0xAAu,
          "short writes only the terminator");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(cpy, DST_ADDR, 0u, SRC_ADDR) == 22u,
          "cap zero EINVAL");
    check(get_guest8(DST_ADDR) == 0xAAu, "cap zero no write");

    check(call_strcpy_s(cpy, 0u, 16u, SRC_ADDR) == 22u,
          "null dst EINVAL");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(cpy, DST_ADDR, 16u, 0u) == 22u,
          "null src EINVAL");
    check(get_guest8(DST_ADDR) == 0u, "null src clears first byte");
    check(get_guest8(DST_ADDR + 1u) == 0xAAu,
          "null src writes only the terminator");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(cpy, DST_ADDR, 0x80000000u, SRC_ADDR) == 0u,
          "huge cap accepted");
    check(same_guest(DST_ADDR, hello, sizeof hello),
          "huge cap copy");

    check(call_strcpy_s(cpy, INVALID_ADDR, 16u, SRC_ADDR) == 22u,
          "invalid dst EINVAL");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(cpy, DST_ADDR, 16u, INVALID_ADDR) == 22u,
          "invalid src EINVAL");
    check(get_guest8(DST_ADDR) == 0u,
          "invalid src clears first byte");

    fill_guest(SRC_ADDR, 'A', 8u);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(cpy, DST_ADDR, 4u, SRC_ADDR) == 34u,
          "no terminator ERANGE");
    check(get_guest8(DST_ADDR) == 0u,
          "no terminator clears first byte");

    fill_guest(SRC_ADDR, 0u, 1u);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(cpy, DST_ADDR, 1u, SRC_ADDR) == 0u,
          "empty source return");
    check(get_guest8(DST_ADDR) == 0u, "empty source copy");
    check(get_guest8(DST_ADDR + 1u) == 0xAAu,
          "empty source no overrun");

    put_guest(SRC_ADDR, hello, sizeof hello);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(cpy, DST_ADDR, 0xFFFFFFFFu, SRC_ADDR) == 0u,
          "max cap return");
    check(same_guest(DST_ADDR, hello, sizeof hello),
          "max cap copy");

    const uint32_t last = GUEST_BASE + GUEST_SIZE - 1u;
    fill_guest(last, 0u, 1u);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(cpy, DST_ADDR, 2u, last) == 0u,
          "last-byte source return");
    check(get_guest8(DST_ADDR) == 0u, "last-byte source copy");

    fill_guest(last, 'A', 1u);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strcpy_s(cpy, DST_ADDR, 4u, last) == 22u,
          "source overrun EINVAL");
    check(get_guest8(DST_ADDR) == 0u,
          "source overrun clears first byte");

    fill_guest(last, 0xAAu, 1u);
    fill_guest(SRC_ADDR, 0u, 1u);
    check(call_strcpy_s(cpy, last, 1u, SRC_ADDR) == 0u,
          "last-byte dst return");
    check(get_guest8(last) == 0u, "last-byte dst copy");

    /* ---- strncpy_s ---------------------------------------------------- */

    put_guest(SRC_ADDR, hello, sizeof hello);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 16u, SRC_ADDR, 5u) == 0u,
          "ncpy normal return");
    check(same_guest(DST_ADDR, hello, sizeof hello), "ncpy normal copy");
    check(get_guest8(DST_ADDR + 6u) == 0xAAu, "ncpy normal no overrun");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 16u, SRC_ADDR, 0u) == 0u,
          "ncpy count zero return");
    check(get_guest8(DST_ADDR) == 0u, "ncpy count zero copy");
    check(get_guest8(DST_ADDR + 1u) == 0xAAu,
          "ncpy count zero no overrun");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 16u, 0u, 0u) == 0u,
          "ncpy null src count zero");
    check(get_guest8(DST_ADDR) == 0u, "ncpy null src count zero copy");

    check(call_strncpy_s(ncpy, 0u, 0u, 0u, 0u) == 0u,
          "ncpy null dst cap zero count zero");
    check(call_strncpy_s(ncpy, 0u, 16u, SRC_ADDR, 0u) == 22u,
          "ncpy null dst count zero EINVAL");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 16u, SRC_ADDR, 2u) == 0u,
          "ncpy count two return");
    check(get_guest8(DST_ADDR) == 'h' && get_guest8(DST_ADDR + 1u) == 'e' &&
              get_guest8(DST_ADDR + 2u) == 0u,
          "ncpy count two copy");
    check(get_guest8(DST_ADDR + 3u) == 0xAAu, "ncpy count two no overrun");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 6u, SRC_ADDR, 5u) == 0u,
          "ncpy exact fit return");
    check(same_guest(DST_ADDR, hello, sizeof hello), "ncpy exact fit copy");
    check(get_guest8(DST_ADDR + 6u) == 0xAAu,
          "ncpy exact fit no overrun");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 1u, SRC_ADDR, 5u) == 34u,
          "ncpy cap one ERANGE");
    check(get_guest8(DST_ADDR) == 0u, "ncpy cap one clears first byte");
    check(get_guest8(DST_ADDR + 1u) == 0xAAu,
          "ncpy cap one stops at cap");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 5u, SRC_ADDR, 5u) == 34u,
          "ncpy cap equal count ERANGE");
    check(get_guest8(DST_ADDR) == 0u, "ncpy cap equal clears first byte");
    check(get_guest8(DST_ADDR + 1u) == 'e', "ncpy cap equal partial copy");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 5u, SRC_ADDR, 2u) == 0u,
          "ncpy short count return");
    check(get_guest8(DST_ADDR) == 'h' && get_guest8(DST_ADDR + 1u) == 'e' &&
              get_guest8(DST_ADDR + 2u) == 0u,
          "ncpy short count copy");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 16u, 0u, 2u) == 22u,
          "ncpy null src EINVAL");
    check(get_guest8(DST_ADDR) == 0u, "ncpy null src clears first byte");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 0u, SRC_ADDR, 2u) == 22u,
          "ncpy cap zero EINVAL");
    check(get_guest8(DST_ADDR) == 0xAAu, "ncpy cap zero no write");

    check(call_strncpy_s(ncpy, 0u, 16u, SRC_ADDR, 2u) == 22u,
          "ncpy null dst EINVAL");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 16u, INVALID_ADDR, 2u) == 22u,
          "ncpy invalid src EINVAL");
    check(get_guest8(DST_ADDR) == 0u, "ncpy invalid src clears first byte");
    check(call_strncpy_s(ncpy, INVALID_ADDR, 16u, SRC_ADDR, 2u) == 22u,
          "ncpy invalid dst EINVAL");

    put_guest(SRC_ADDR, "abcdefgh", 9u);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 16u, SRC_ADDR, 4u) == 0u,
          "ncpy count fits return");
    check(same_guest(DST_ADDR, "abcd", 5u), "ncpy count fits copy");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 5u, SRC_ADDR, 4u) == 0u,
          "ncpy cap minus one return");
    check(same_guest(DST_ADDR, "abcd", 5u), "ncpy cap minus one copy");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 4u, SRC_ADDR, 4u) == 34u,
          "ncpy count equal cap ERANGE");
    check(get_guest8(DST_ADDR) == 0u,
          "ncpy count equal cap clears first");
    check(get_guest8(DST_ADDR + 1u) == 'b',
          "ncpy count equal cap partial copy");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 2u, SRC_ADDR, 1u) == 0u,
          "ncpy count one return");
    check(get_guest8(DST_ADDR) == 'a' && get_guest8(DST_ADDR + 1u) == 0u,
          "ncpy count one copy");

    put_guest(SRC_ADDR, "abc", 4u);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 6u, SRC_ADDR, 100u) == 0u,
          "ncpy count past terminator return");
    check(same_guest(DST_ADDR, "abc", 4u),
          "ncpy count past terminator copy");

    put_guest(SRC_ADDR, "abcdefgh", 9u);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 6u, SRC_ADDR, 0xFFFFFFFFu) == 80u,
          "ncpy truncate return");
    check(same_guest(DST_ADDR, "abcde", 6u), "ncpy truncate copy");
    check(get_guest8(DST_ADDR + 6u) == 0xAAu,
          "ncpy truncate no overrun");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 3u, SRC_ADDR, 0xFFFFFFFFu) == 80u,
          "ncpy truncate short return");
    check(same_guest(DST_ADDR, "ab", 3u), "ncpy truncate short copy");

    fill_guest(SRC_ADDR, 0u, 1u);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 3u, SRC_ADDR, 0xFFFFFFFFu) == 0u,
          "ncpy truncate empty return");
    check(get_guest8(DST_ADDR) == 0u, "ncpy truncate empty copy");

    put_guest(SRC_ADDR, "abcdef", 7u);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 1u, SRC_ADDR, 0xFFFFFFFFu) == 80u,
          "ncpy truncate cap one return");
    check(get_guest8(DST_ADDR) == 0u, "ncpy truncate cap one copy");
    check(get_guest8(DST_ADDR + 1u) == 0xAAu,
          "ncpy truncate cap one no overrun");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 5u, 0u, 0xFFFFFFFFu) == 22u,
          "ncpy truncate null src EINVAL");
    check(get_guest8(DST_ADDR) == 0u,
          "ncpy truncate null src clears first");

    put_guest(SRC_ADDR, hello, sizeof hello);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 0x80000000u, SRC_ADDR, 5u) == 0u,
          "ncpy huge cap accepted");
    check(same_guest(DST_ADDR, hello, sizeof hello), "ncpy huge cap copy");

    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 0xFFFFFFFFu, SRC_ADDR, 5u) == 0u,
          "ncpy max cap accepted");
    check(same_guest(DST_ADDR, hello, sizeof hello), "ncpy max cap copy");

    put_guest(SRC_ADDR, "abcdefgh", 9u);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, SRC_ADDR + 1u, 16u, SRC_ADDR, 5u) == 0u,
          "ncpy forward overlap return");
    check(same_guest(SRC_ADDR + 1u, "aaaaa", 6u),
          "ncpy forward overlap copy");

    put_guest(SRC_ADDR, "abcdefgh", 9u);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, SRC_ADDR, 16u, SRC_ADDR + 1u, 5u) == 0u,
          "ncpy source after dst return");
    check(same_guest(SRC_ADDR, "bcdef", 6u),
          "ncpy source after dst copy");

    put_guest(SRC_ADDR, "abcdefgh", 9u);
    fill_guest(SRC_ADDR + 4u, 0xAAu, 12u);
    check(call_strncpy_s(ncpy, SRC_ADDR + 4u, 16u, SRC_ADDR, 5u) == 0u,
          "ncpy backward overlap return");
    check(same_guest(SRC_ADDR + 4u, "abcda", 6u),
          "ncpy backward overlap copy");

    fill_guest(last, 0u, 1u);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 2u, last, 1u) == 0u,
          "ncpy last-byte source return");
    check(get_guest8(DST_ADDR) == 0u, "ncpy last-byte source copy");

    fill_guest(last, 'A', 1u);
    fill_guest(DST_ADDR, 0xAAu, 16u);
    check(call_strncpy_s(ncpy, DST_ADDR, 4u, last, 2u) == 22u,
          "ncpy source overrun EINVAL");
    check(get_guest8(DST_ADDR) == 0u,
          "ncpy source overrun clears first byte");

    fill_guest(last, 0xAAu, 1u);
    fill_guest(SRC_ADDR, 0u, 1u);
    check(call_strncpy_s(ncpy, last, 1u, SRC_ADDR, 0u) == 0u,
          "ncpy last-byte dst count zero");
    check(get_guest8(last) == 0u, "ncpy last-byte dst count zero copy");

    fill_guest(last, 0xAAu, 1u);
    put_guest(SRC_ADDR, "A", 2u);
    check(call_strncpy_s(ncpy, last, 1u, SRC_ADDR, 1u) == 34u,
          "ncpy last-byte dst count one ERANGE");
    check(get_guest8(last) == 0u, "ncpy last-byte dst count one cleared");

    if (g_failures) {
        printf("msvcr80 secure: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("msvcr80 secure OK (2 handlers)\n");
    return 0;
}
