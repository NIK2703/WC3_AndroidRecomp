/*
 * msvcr80_heap_test.c - T0 contract test for the CRT heap subset.
 *
 * The fake shim models a low-32-bit guest arena with the same 8-byte block
 * header the real guest heap uses.  The test checks the Windows-visible
 * contracts that differ from the as-built handlers: calloc overflow, realloc
 * failure preserving the old block, and realloc(..., 0) freeing.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "heap_hle.h"
#include "legacy_shim.h"

#define GUEST_BASE 0x10000000u
#define ARENA_SIZE 4096u
#define MAX_ROWS 16u

static uint8_t g_arena[ARENA_SIZE];
static uint32_t g_heap_end;
static uint32_t g_args[8];
static int g_fail_next_alloc;

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
} Row;

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;

static int guest_range(uint32_t addr, uint32_t size)
{
    if (addr < GUEST_BASE) return 0;
    const uint32_t off = addr - GUEST_BASE;
    return off <= ARENA_SIZE && size <= ARENA_SIZE - off;
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

uint32_t wc3_legacy_arg32(const CPU *c, unsigned index)
{
    (void)c;
    return index < 8u ? g_args[index] : 0;
}

uint32_t wc3_legacy_guest_alloc(uint32_t size)
{
    if (g_fail_next_alloc) {
        g_fail_next_alloc = 0;
        return 0;
    }
    if (!size) size = 1;
    size = (size + 7u) & ~7u;
    if (g_heap_end + 8u + size > ARENA_SIZE) return 0;
    const uint32_t header = g_heap_end;
    g_heap_end += 8u + size;
    const uint32_t payload = GUEST_BASE + header + 8u;
    memcpy(&g_arena[header], &size, 4);
    const uint32_t next = 0;
    memcpy(&g_arena[header + 4], &next, 4);
    return payload;
}

uint32_t wc3_legacy_guest_alloc0(uint32_t size)
{
    const uint32_t n = size ? size : 1u;
    const uint32_t p = wc3_legacy_guest_alloc(n);
    if (p && guest_range(p, n)) memset(&g_arena[p - GUEST_BASE], 0, n);
    return p;
}

void wc3_legacy_guest_free(uint32_t addr)
{
    if (!addr || addr < GUEST_BASE) return;
    const uint32_t off = addr - GUEST_BASE;
    if (off < 8u || off > ARENA_SIZE - 8u) return;
    uint32_t size = 0;
    memcpy(&size, &g_arena[off - 8u], 4);
    size = 0;
    memcpy(&g_arena[off - 8u], &size, 4);
}

uint32_t wc3_legacy_guest_size(uint32_t addr)
{
    if (!addr || addr < GUEST_BASE) return 0;
    const uint32_t off = addr - GUEST_BASE;
    if (off < 8u || off > ARENA_SIZE - 8u) return 0;
    uint32_t size = 0;
    memcpy(&size, &g_arena[off - 8u], 4);
    return size;
}

void wc3_legacy_guest_copy(uint32_t dst, uint32_t src, uint32_t size)
{
    if (!size || !guest_range(dst, size) || !guest_range(src, size)) return;
    memmove(&g_arena[dst - GUEST_BASE], &g_arena[src - GUEST_BASE], size);
}

static void *guest_ptr(uint32_t addr, uint32_t size)
{
    if (!guest_range(addr, size)) return NULL;
    return &g_arena[addr - GUEST_BASE];
}

static void (*find_impl(const char *name))(CPU *c)
{
    for (unsigned i = 0; i < g_row_count; i++) {
        if (strcmp(g_rows[i].name, name) == 0) return g_rows[i].impl;
    }
    return NULL;
}

static uint32_t invoke(const char *name, uint32_t a0, uint32_t a1)
{
    void (*impl)(CPU *c) = find_impl(name);
    if (!impl) {
        printf("missing handler: %s\n", name);
        return 0xFFFFFFFFu;
    }
    memset(g_args, 0, sizeof g_args);
    g_args[0] = a0;
    g_args[1] = a1;
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
    const int installed = wc3_hle_msvcr80_heap_install();
    CHECK(installed == 6, "install count");
    CHECK(g_row_count == 6u, "row count");
    CHECK(find_impl("malloc") != NULL, "malloc row");
    CHECK(find_impl("_calloc_crt") != NULL, "_calloc_crt row");
    CHECK(strcmp(g_rows[0].dll, "MSVCR80.dll") == 0, "DLL name");

    const uint32_t zero = invoke("malloc", 0, 0);
    CHECK(zero != 0u && wc3_legacy_guest_size(zero) == 8u, "malloc(0)");

    const uint32_t p = invoke("malloc", 5, 0);
    CHECK(p != 0u && wc3_legacy_guest_size(p) == 8u, "malloc(5) size");
    memcpy(guest_ptr(p, 5), "abcde", 5);

    const uint32_t crt = invoke("_malloc_crt", 17, 0);
    CHECK(crt != 0u && wc3_legacy_guest_size(crt) == 24u, "_malloc_crt size");

    const uint32_t c = invoke("calloc", 4, 5);
    CHECK(c != 0u && wc3_legacy_guest_size(c) == 24u, "calloc size");
    for (unsigned i = 0; i < 20; i++)
        CHECK(((uint8_t *)guest_ptr(c, 20))[i] == 0u, "calloc zero");

    CHECK(invoke("calloc", 0x80000000u, 2u) == 0u, "calloc overflow");
    CHECK(invoke("_calloc_crt", 2, 3) != 0u, "_calloc_crt");

    const uint32_t grown = invoke("realloc", p, 20);
    CHECK(grown != 0u && wc3_legacy_guest_size(grown) == 24u, "realloc size");
    CHECK(memcmp(guest_ptr(grown, 5), "abcde", 5) == 0, "realloc copy");
    CHECK(wc3_legacy_guest_size(p) == 0u, "realloc frees old");

    const uint32_t fresh = invoke("realloc", 0, 9);
    CHECK(fresh != 0u && wc3_legacy_guest_size(fresh) == 16u, "realloc(NULL)");

    CHECK(invoke("realloc", grown, 0) == 0u, "realloc(...,0)");
    CHECK(wc3_legacy_guest_size(grown) == 0u, "realloc(...,0) frees");

    const uint32_t keep = invoke("malloc", 8, 0);
    CHECK(keep != 0u, "failure setup");
    memcpy(guest_ptr(keep, 8), "preserve", 8);
    g_fail_next_alloc = 1;
    CHECK(invoke("realloc", keep, 64) == 0u, "realloc failure");
    CHECK(wc3_legacy_guest_size(keep) == 8u, "realloc failure keeps old");
    CHECK(memcmp(guest_ptr(keep, 8), "preserve", 8) == 0,
          "realloc failure keeps data");

    CHECK(invoke("free", 0, 0) == 0u, "free(NULL)");
    CHECK(invoke("free", keep, 0) == 0u, "free return");
    CHECK(wc3_legacy_guest_size(keep) == 0u, "free clears header");

    printf("msvcr80 heap OK (%d handlers)\n", installed);
    return 0;
}
