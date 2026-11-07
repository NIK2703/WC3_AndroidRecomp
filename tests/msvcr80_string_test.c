/*
 * msvcr80_string_test.c - T0 contract test for the CRT string/memory subset.
 *
 * The two contract corrections are explicit: strncpy pads through n and
 * strstr("", "") returns the empty haystack.  Pointer-returning functions are
 * checked against guest addresses, not host pointers.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "crt_string_hle.h"

/* Test addresses start at 0x100; zero is the NULL pointer and modules check
 * it before dereferencing, so the fake arena can start at zero. */
#define GUEST_BASE 0x0000u
#define ARENA_SIZE 0x10000u
#define HEAP_OFF 0x8000u
#define MAX_ROWS 32u

static uint8_t g_mem[ARENA_SIZE];
static uint32_t g_heap_end;
static uint32_t g_args[4];

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
} Row;

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;

static void *guest_ptr(uint32_t addr, uint32_t size)
{
    const uint32_t off = addr;
    if (off > ARENA_SIZE || size > ARENA_SIZE - off) return NULL;
    return &g_mem[off];
}

static void *guest_at(uint32_t addr, uint32_t size)
{
    void *p = guest_ptr(addr, size);
    if (!p) {
        fprintf(stderr, "test bug: guest range %08X+%u\n", addr, size);
        abort();
    }
    return p;
}

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    if (!dll || !name || !impl) return -1;
    if (g_row_count >= MAX_ROWS) return -1;
    g_rows[g_row_count].dll = dll;
    g_rows[g_row_count].name = name;
    g_rows[g_row_count].impl = impl;
    g_row_count++;
    return 0;
}

uint32_t wc3_legacy_arg32(const CPU *c, unsigned index)
{
    (void)c;
    return index < 4u ? g_args[index] : 0;
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

void wc3_legacy_guest_copy(uint32_t dst, uint32_t src, uint32_t size)
{
    if (!size) return;
    void *d = guest_ptr(dst, size);
    const void *s = guest_ptr(src, size);
    if (d && s) memcpy(d, s, size);
}

void wc3_legacy_guest_move(uint32_t dst, uint32_t src, uint32_t size)
{
    if (!size) return;
    void *d = guest_ptr(dst, size);
    const void *s = guest_ptr(src, size);
    if (d && s) memmove(d, s, size);
}

void wc3_legacy_guest_fill(uint32_t dst, uint8_t value, uint32_t size)
{
    if (!size) return;
    void *d = guest_ptr(dst, size);
    if (d) memset(d, value, size);
}

uint32_t wc3_legacy_guest_alloc(uint32_t size)
{
    if (!size) size = 1;
    size = (size + 7u) & ~7u;
    if (g_heap_end + 8u + size > ARENA_SIZE - HEAP_OFF) return 0;
    const uint32_t header = GUEST_BASE + HEAP_OFF + g_heap_end;
    g_heap_end += 8u + size;
    memcpy(guest_at(header, 4), &size, 4);
    return header + 8u;
}

uint32_t wc3_legacy_guest_size(uint32_t addr)
{
    if (addr < 8u) return 0;
    const void *p = guest_ptr(addr - 8u, 4);
    uint32_t size = 0;
    if (p) memcpy(&size, p, 4);
    return size;
}

static void (*find_impl(const char *name))(CPU *c)
{
    for (unsigned i = 0; i < g_row_count; i++)
        if (strcmp(g_rows[i].name, name) == 0) return g_rows[i].impl;
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

static void put_string(uint32_t addr, const char *s)
{
    const uint32_t n = (uint32_t)strlen(s) + 1u;
    memcpy(guest_at(addr, n), s, n);
}

static void put_bytes(uint32_t addr, const uint8_t *p, uint32_t n)
{
    memcpy(guest_at(addr, n), p, n);
}

static void fill(uint32_t addr, uint8_t value, uint32_t n)
{
    memset(guest_at(addr, n), value, n);
}

static int32_t as_i32(uint32_t v) { return (int32_t)v; }

#define CHECK(condition, message)                                         \
    do {                                                                  \
        if (!(condition)) {                                               \
            printf("FAIL: %s\n", message);                                \
            return 1;                                                     \
        }                                                                 \
    } while (0)

int main(void)
{
    const int installed = wc3_hle_msvcr80_string_install();
    CHECK(installed == 25, "install count");
    CHECK(g_row_count == 25u, "row count");
    CHECK(find_impl("memcpy") != NULL, "memcpy row");
    CHECK(find_impl("_strdup") != NULL, "_strdup row");
    CHECK(strcmp(g_rows[0].dll, "MSVCR80.dll") == 0, "DLL name");

    put_string(0x100, "abcdef");
    put_string(0x110, "abd");
    put_string(0x140, "AbC");
    put_string(0x150, "aBc");
    put_string(0x180, "abcabc");
    put_string(0x1A0, "hello world");
    put_string(0x1B0, "world");
    put_string(0x1D0, "xyz");
    put_string(0x1F0, "abc/def");
    put_string(0x200, "/:");
    put_string(0x210, "aaab");
    put_string(0x220, "a");
    put_string(0x230, "abc/def");
    put_string(0x240, "/");
    put_string(0x250, "abcdef");
    put_string(0x260, "aBc1");
    put_string(0x270, "AbC1");
    put_string(0x280, "hello");
    put_string(0x510, "abc");
    put_string(0x700, "42");
    put_string(0x710, "-17");
    put_string(0x720, "  +7x");
    put_string(0x730, "x");
    put_string(0x740, "999999999999");
    put_string(0x750, "-999999999999");

    const uint8_t high1[] = { 0x80, 0 };
    const uint8_t high2[] = { 0x7F, 0 };
    const uint8_t cp1[] = { 0xC0, 0 };
    const uint8_t cp2[] = { 0xE0, 0 };
    put_bytes(0x120, high1, sizeof high1);
    put_bytes(0x130, high2, sizeof high2);
    put_bytes(0x160, cp1, sizeof cp1);
    put_bytes(0x170, cp2, sizeof cp2);
    wc3_legacy_wr8g(0x264, 0xE0);
    wc3_legacy_wr8g(0x274, 0xC0);

    const uint32_t dst = 0x300;
    fill(dst, 0xAA, 8);
    CHECK(invoke("memcpy", dst, 0x100, 6) == dst, "memcpy return");
    CHECK(memcmp(guest_at(dst, 8), "abcdef", 6) == 0, "memcpy bytes");
    CHECK(wc3_legacy_rd8g(dst + 6) == 0xAAu, "memcpy tail");
    CHECK(invoke("memcpy", dst, 0, 0) == dst, "memcpy n=0");

    put_string(0x320, "abcdef");
    CHECK(invoke("memmove", 0x322, 0x320, 4) == 0x322u, "memmove return");
    CHECK(memcmp(guest_at(0x320, 6), "ababcd", 6) == 0, "memmove overlap");

    fill(0x340, 0xAA, 8);
    CHECK(invoke("memset", 0x340, 0x41, 4) == 0x340u, "memset return");
    CHECK(memcmp(guest_at(0x340, 4), "AAAA", 4) == 0, "memset bytes");
    CHECK(wc3_legacy_rd8g(0x344) == 0xAAu, "memset tail");
    CHECK(invoke("memset", 0x340, 0x42, 0) == 0x340u, "memset n=0");

    const uint32_t ncpy = 0x500;
    fill(ncpy, 0xAA, 8);
    CHECK(invoke("strncpy", ncpy, 0x510, 5) == ncpy, "strncpy return");
    CHECK(memcmp(guest_at(ncpy, 5), "abc\0\0", 5) == 0, "strncpy pads");
    CHECK(wc3_legacy_rd8g(ncpy + 5) == 0xAAu, "strncpy tail");
    fill(ncpy, 0xAA, 8);
    CHECK(invoke("strncpy", ncpy, 0x510, 2) == ncpy, "strncpy n=2");
    CHECK(memcmp(guest_at(ncpy, 2), "ab", 2) == 0, "strncpy truncates");
    CHECK(wc3_legacy_rd8g(ncpy + 2) == 0xAAu, "strncpy n=2 tail");
    fill(ncpy, 0xAA, 8);
    CHECK(invoke("strncpy", ncpy, 0x100, 0) == ncpy, "strncpy n=0");
    CHECK(wc3_legacy_rd8g(ncpy) == 0xAAu, "strncpy n=0 untouched");

    CHECK(as_i32(invoke("strncmp", 0x100, 0x110, 3)) < 0, "strncmp less");
    CHECK(invoke("strncmp", 0x100, 0x110, 2) == 0u, "strncmp equal prefix");
    CHECK(invoke("strncmp", 0x100, 0x110, 0) == 0u, "strncmp n=0");
    CHECK(as_i32(invoke("strncmp", 0x120, 0x130, 1)) > 0, "strncmp unsigned");

    CHECK(invoke("_strnicmp", 0x140, 0x150, 3) == 0u, "_strnicmp case");
    CHECK(as_i32(invoke("_strnicmp", 0x160, 0x170, 1)) < 0,
          "_strnicmp high bytes");

    CHECK(invoke("strchr", 0x180, 'b', 0) == 0x181u, "strchr first");
    CHECK(invoke("strchr", 0x180, 'z', 0) == 0u, "strchr missing");
    CHECK(invoke("strchr", 0x180, 0, 0) == 0x186u, "strchr terminator");
    CHECK(invoke("strchr", 0, 'a', 0) == 0u, "strchr NULL");

    CHECK(invoke("strrchr", 0x180, 'b', 0) == 0x184u, "strrchr last");
    CHECK(invoke("strrchr", 0x180, 'z', 0) == 0u, "strrchr missing");
    CHECK(invoke("strrchr", 0x180, 0, 0) == 0x186u, "strrchr terminator");

    CHECK(invoke("strstr", 0x1A0, 0x1B0, 0) == 0x1A6u, "strstr match");
    CHECK(invoke("strstr", 0x1A0, 0x1C0, 0) == 0x1A0u, "strstr empty needle");
    CHECK(invoke("strstr", 0x1A0, 0x1D0, 0) == 0u, "strstr missing");
    CHECK(invoke("strstr", 0x1E0, 0x1C0, 0) == 0x1E0u,
          "strstr empty haystack and needle");
    CHECK(invoke("strstr", 0, 0x1B0, 0) == 0u, "strstr NULL haystack");

    CHECK(invoke("strpbrk", 0x1F0, 0x200, 0) == 0x1F3u, "strpbrk match");
    CHECK(invoke("strpbrk", 0x1F0, 0x1D0, 0) == 0u, "strpbrk missing");
    CHECK(invoke("strpbrk", 0x1F0, 0x1C0, 0) == 0u, "strpbrk empty set");

    CHECK(invoke("strspn", 0x210, 0x220, 0) == 3u, "strspn count");
    CHECK(invoke("strspn", 0x210, 0x1C0, 0) == 0u, "strspn empty set");
    CHECK(invoke("strcspn", 0x230, 0x240, 0) == 3u, "strcspn count");
    CHECK(invoke("strcspn", 0x230, 0x1D0, 0) == 7u, "strcspn no match");

    CHECK(invoke("strnlen", 0x250, 4, 0) == 4u, "strnlen bounded");
    CHECK(invoke("strnlen", 0x250, 0, 0) == 0u, "strnlen n=0");
    CHECK(invoke("strnlen", 0x250, 10, 0) == 6u, "strnlen full");
    CHECK(invoke("strnlen", 0, 10, 0) == 0u, "strnlen NULL");

    CHECK(invoke("_strupr", 0x260, 0, 0) == 0x260u, "_strupr return");
    CHECK(memcmp(guest_at(0x260, 4), "ABC1", 4) == 0, "_strupr ascii");
    CHECK(wc3_legacy_rd8g(0x264) == 0xE0u, "_strupr high byte");
    CHECK(invoke("_strlwr", 0x270, 0, 0) == 0x270u, "_strlwr return");
    CHECK(memcmp(guest_at(0x270, 4), "abc1", 4) == 0, "_strlwr ascii");
    CHECK(wc3_legacy_rd8g(0x274) == 0xC0u, "_strlwr high byte");

    const uint32_t dup = invoke("_strdup", 0x280, 0, 0);
    CHECK(dup != 0u && wc3_legacy_guest_size(dup) >= 6u, "_strdup size");
    CHECK(memcmp(guest_at(dup, 6), "hello", 6) == 0, "_strdup bytes");
    CHECK(invoke("_strdup", 0, 0, 0) == 0u, "_strdup NULL");

    const uint32_t ibuf = 0x600;
    memset(guest_ptr(ibuf, 64), 0x55, 64);
    CHECK(invoke("_itoa", 0, ibuf, 10) == ibuf, "_itoa zero return");
    CHECK(strcmp((const char *)guest_ptr(ibuf, 2), "0") == 0, "_itoa zero");
    CHECK(((const uint8_t *)guest_ptr(ibuf, 64))[2] == 0x55u,
          "_itoa zero tail");

    memset(guest_ptr(ibuf, 64), 0x55, 64);
    CHECK(invoke("_itoa", (uint32_t)-123, ibuf, 10) == ibuf,
          "_itoa negative return");
    CHECK(strcmp((const char *)guest_ptr(ibuf, 5), "-123") == 0,
          "_itoa negative");

    memset(guest_ptr(ibuf, 64), 0x55, 64);
    CHECK(invoke("_itoa", 255, ibuf, 16) == ibuf, "_itoa hex return");
    CHECK(strcmp((const char *)guest_ptr(ibuf, 3), "ff") == 0, "_itoa hex");

    memset(guest_ptr(ibuf, 64), 0x55, 64);
    CHECK(invoke("_itoa", 10, ibuf, 2) == ibuf, "_itoa binary return");
    CHECK(strcmp((const char *)guest_ptr(ibuf, 5), "1010") == 0,
          "_itoa binary");

    memset(guest_ptr(ibuf, 64), 0x55, 64);
    CHECK(invoke("_itoa", (uint32_t)INT32_MIN, ibuf, 10) == ibuf,
          "_itoa INT32_MIN return");
    CHECK(strcmp((const char *)guest_ptr(ibuf, 12), "-2147483648") == 0,
          "_itoa INT32_MIN");

    memset(guest_ptr(ibuf, 64), 0x55, 64);
    CHECK(invoke("_itoa", 7, ibuf, 1) == 0u, "_itoa bad radix");
    CHECK(((const uint8_t *)guest_ptr(ibuf, 1))[0] == 0x55u,
          "_itoa bad radix untouched");
    CHECK(invoke("_itoa", 7, 0, 10) == 0u, "_itoa NULL buffer");

    CHECK(invoke("atoi", 0x700, 0, 0) == 42u, "atoi positive");
    CHECK((int32_t)invoke("atoi", 0x710, 0, 0) == -17, "atoi negative");
    CHECK(invoke("atoi", 0x720, 0, 0) == 7u, "atoi whitespace and suffix");
    CHECK(invoke("atoi", 0x730, 0, 0) == 0u, "atoi no digits");
    CHECK(invoke("atoi", 0, 0, 0) == 0u, "atoi NULL");
    CHECK(invoke("atoi", 0x740, 0, 0) == 2147483647u,
          "atoi positive overflow");
    CHECK((int32_t)invoke("atoi", 0x750, 0, 0) == INT32_MIN,
          "atoi negative overflow");
    CHECK((int32_t)invoke("atol", 0x710, 0, 0) == -17, "atol negative");
    CHECK((int32_t)invoke("atol", 0x750, 0, 0) == INT32_MIN,
          "atol negative overflow");

    CHECK(invoke("isdigit", '5', 0, 0) == 1u, "isdigit hit");
    CHECK(invoke("isdigit", 'a', 0, 0) == 0u, "isdigit miss");
    CHECK(invoke("isdigit", 0x135, 0, 0) == 1u, "isdigit byte cast");
    CHECK(invoke("isalpha", 'Z', 0, 0) == 1u, "isalpha hit");
    CHECK(invoke("isalpha", '1', 0, 0) == 0u, "isalpha miss");
    CHECK(invoke("isspace", ' ', 0, 0) == 1u, "isspace space");
    CHECK(invoke("isspace", '0', 0, 0) == 0u, "isspace miss");
    CHECK(invoke("iswspace", 0x20u, 0, 0) == 1u, "iswspace space");
    CHECK(invoke("iswspace", 0x3000u, 0, 0) == 0u, "iswspace miss");
    CHECK(invoke("toupper", 'a', 0, 0) == 'A', "toupper ascii");
    CHECK(invoke("toupper", 0xE0u, 0, 0) == 0xE0u, "toupper high byte");
    CHECK(invoke("tolower", 'A', 0, 0) == 'a', "tolower ascii");
    CHECK(invoke("tolower", 0xC0u, 0, 0) == 0xC0u, "tolower high byte");

    printf("msvcr80 string OK (%d handlers)\n", installed);
    return 0;
}
