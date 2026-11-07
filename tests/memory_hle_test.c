/*
 * memory_hle_test.c - T0 contract test for kernel32/memory.
 *
 * The fake shim keeps the as-built heap header shape (two words before the
 * payload) and a bounded low-32-bit guest arena, so the test exercises the
 * same offsets and zeroing rules the production shim uses.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "memory_hle.h"

#define GUEST_BASE 0x10000000u
#define GUEST_SIZE 0x00010000u
#define MAX_OVERRIDES 32

static uint8_t g_guest[GUEST_SIZE];
static uint32_t g_arena_next = 8u;
static struct {
    char name[64];
    void (*impl)(CPU *);
} g_overrides[MAX_OVERRIDES];
static unsigned g_override_count;
static uint32_t g_args[8];

static int in_guest(uint32_t addr, uint32_t size)
{
    if (addr < GUEST_BASE || size > GUEST_SIZE) return 0;
    const uint32_t off = addr - GUEST_BASE;
    return off + size >= off && off + size <= GUEST_SIZE;
}

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    return in_guest(addr, size);
}

uint8_t wc3_legacy_rd8g(uint32_t addr)
{
    return g_guest[addr - GUEST_BASE];
}

void wc3_legacy_wr8g(uint32_t addr, uint8_t value)
{
    g_guest[addr - GUEST_BASE] = value;
}

uint32_t wc3_legacy_rd32g(uint32_t addr)
{
    uint32_t v;
    memcpy(&v, g_guest + (addr - GUEST_BASE), sizeof v);
    return v;
}

void wc3_legacy_wr32g(uint32_t addr, uint32_t value)
{
    memcpy(g_guest + (addr - GUEST_BASE), &value, sizeof value);
}

uint32_t wc3_legacy_arg32(const CPU *cpu, unsigned index)
{
    (void)cpu;
    return index < 8u ? g_args[index] : 0u;
}

uint32_t wc3_legacy_guest_alloc(uint32_t size)
{
    uint32_t n = size ? size : 1u;
    n = (n + 7u) & ~7u;
    if (g_arena_next + 8u + n > GUEST_SIZE) return 0;
    const uint32_t off = g_arena_next;
    g_arena_next += 8u + n;
    const uint32_t ptr = GUEST_BASE + off + 8u;
    wc3_legacy_wr32g(ptr - 8u, n);
    wc3_legacy_wr32g(ptr - 4u, 0u);
    return ptr;
}

uint32_t wc3_legacy_guest_alloc0(uint32_t size)
{
    const uint32_t n = size ? size : 1u;
    const uint32_t p = wc3_legacy_guest_alloc(n);
    if (p) memset(g_guest + (p - GUEST_BASE), 0, n);
    return p;
}

void wc3_legacy_guest_free(uint32_t addr)
{
    if (!addr) return;
    if (wc3_legacy_guest_size(addr)) wc3_legacy_wr32g(addr - 8u, 0u);
}

uint32_t wc3_legacy_guest_size(uint32_t addr)
{
    if (!addr || !in_guest(addr - 8u, 8u)) return 0;
    const uint32_t n = wc3_legacy_rd32g(addr - 8u);
    return n <= GUEST_SIZE ? n : 0;
}

void wc3_legacy_guest_copy(uint32_t dst, uint32_t src, uint32_t size)
{
    if (!in_guest(dst, size) || !in_guest(src, size)) return;
    memmove(g_guest + (dst - GUEST_BASE), g_guest + (src - GUEST_BASE), size);
}

void wc3_legacy_guest_zero(uint32_t dst, uint32_t size)
{
    if (!in_guest(dst, size)) return;
    memset(g_guest + (dst - GUEST_BASE), 0, size);
}

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    (void)dll;
    if (!name || !impl || g_override_count >= MAX_OVERRIDES) return -1;
    snprintf(g_overrides[g_override_count].name,
             sizeof g_overrides[g_override_count].name, "%s", name);
    g_overrides[g_override_count].impl = impl;
    g_override_count++;
    return 0;
}

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    (void)argc;
    (void)cdecl_abi;
    return wc3_legacy_hle_override(dll, name, impl);
}

static void (*find(const char *name))(CPU *)
{
    for (unsigned i = 0; i < g_override_count; i++)
        if (strcmp(g_overrides[i].name, name) == 0)
            return g_overrides[i].impl;
    return NULL;
}

static CPU g_cpu;
static int g_failed;

static int fail(const char *what)
{
    fprintf(stderr, "FAIL: %s\n", what);
    g_failed = 1;
    return 0;
}

static uint32_t call(const char *name, uint32_t a0, uint32_t a1, uint32_t a2,
                     uint32_t a3)
{
    void (*fn)(CPU *) = find(name);
    if (!fn) {
        fail(name);
        return 0;
    }
    g_args[0] = a0;
    g_args[1] = a1;
    g_args[2] = a2;
    g_args[3] = a3;
    memset(&g_cpu, 0, sizeof g_cpu);
    fn(&g_cpu);
    return g_cpu.eax;
}

#define CHECK(cond, what) do { if (!(cond)) fail(what); } while (0)

int main(void)
{
    const int installed = wc3_hle_kernel32_memory_install();
    CHECK(installed == 16, "install count");
    if (g_failed) return 1;

    CHECK(call("GetProcessHeap", 0, 0, 0, 0) == 0x00090000u, "GetProcessHeap");

    uint32_t p = call("GlobalAlloc", 0, 16, 0, 0);
    CHECK(p != 0, "GlobalAlloc");
    CHECK(wc3_legacy_guest_size(p) >= 16, "GlobalAlloc size");
    for (uint32_t i = 0; i < 16; i++) wc3_legacy_wr8g(p + i, 0xAA);
    CHECK(call("GlobalLock", p, 0, 0, 0) == p, "GlobalLock");
    CHECK(call("GlobalUnlock", p, 0, 0, 0) == 0, "GlobalUnlock is FALSE for fixed");
    CHECK(call("GlobalSize", p, 0, 0, 0) >= 16, "GlobalSize");
    CHECK(call("GlobalFree", p, 0, 0, 0) == 0, "GlobalFree");
    CHECK(call("GlobalSize", p, 0, 0, 0) == 0, "GlobalSize after free");
    CHECK(call("GlobalFree", 0, 0, 0, 0) == 0, "GlobalFree(NULL)");

    uint32_t z = call("GlobalAlloc", 0x0040u, 24, 0, 0);
    CHECK(z != 0, "GlobalAlloc zero");
    for (uint32_t i = 0; i < 24; i++)
        CHECK(wc3_legacy_rd8g(z + i) == 0, "GMEM_ZEROINIT");
    call("GlobalFree", z, 0, 0, 0);

    uint32_t l = call("LocalAlloc", 0x0040u, 8, 0, 0);
    CHECK(l != 0, "LocalAlloc");
    for (uint32_t i = 0; i < 8; i++)
        CHECK(wc3_legacy_rd8g(l + i) == 0, "LPTR zero");
    CHECK(call("LocalFree", l, 0, 0, 0) == 0, "LocalFree");

    uint32_t h = call("HeapAlloc", 0x00090000u, 0x00000008u, 32, 0);
    CHECK(h != 0, "HeapAlloc");
    for (uint32_t i = 0; i < 32; i++)
        CHECK(wc3_legacy_rd8g(h + i) == 0, "HEAP_ZERO_MEMORY");
    wc3_legacy_wr8g(h + 0, 'a');
    wc3_legacy_wr8g(h + 1, 'b');
    wc3_legacy_wr8g(h + 2, 'c');
    CHECK(call("HeapSize", 0x00090000u, 0, h, 0) >= 32, "HeapSize");

    uint32_t h2 = call("HeapReAlloc", 0x00090000u, 0x00000008u, h, 64);
    CHECK(h2 != 0, "HeapReAlloc");
    CHECK(wc3_legacy_rd8g(h2 + 0) == 'a' &&
              wc3_legacy_rd8g(h2 + 1) == 'b' &&
              wc3_legacy_rd8g(h2 + 2) == 'c',
          "HeapReAlloc preserves contents");
    for (uint32_t i = 32; i < 64; i++)
        CHECK(wc3_legacy_rd8g(h2 + i) == 0, "HeapReAlloc zeroed tail");
    CHECK(wc3_legacy_guest_size(h) == 0, "HeapReAlloc freed old block");

    CHECK(call("HeapReAlloc", 0x00090000u, 0, h2, 0) == 0,
          "HeapReAlloc(size 0)");
    CHECK(wc3_legacy_guest_size(h2) == 0, "HeapReAlloc(size 0) frees");

    CHECK(call("HeapFree", 0x00090000u, 0, 0, 0) == 1, "HeapFree(NULL)");
    CHECK(call("HeapFree", 0x00090000u, 0, GUEST_BASE + 0x40u, 0) == 0,
          "HeapFree rejects a stale pointer");
    CHECK(call("HeapSize", 0x00090000u, 0, GUEST_BASE + 0x40u, 0) == 0,
          "HeapSize rejects a stale pointer");

    const uint32_t heap_arr = GUEST_BASE + 0x7000u;
    CHECK(call("GetProcessHeaps", 1, heap_arr, 0, 0) == 1, "GetProcessHeaps");
    CHECK(wc3_legacy_rd32g(heap_arr) == 0x00090000u, "GetProcessHeaps value");

    CHECK(call("VirtualLock", heap_arr, 16, 0, 0) == 1, "VirtualLock");
    CHECK(call("VirtualLock", heap_arr, 0, 0, 0) == 1, "VirtualLock zero");
    CHECK(call("VirtualLock", 0, 16, 0, 0) == 0, "VirtualLock bad base");
    CHECK(call("VirtualLock", GUEST_BASE + GUEST_SIZE - 4u, 16, 0, 0) == 0,
          "VirtualLock short range");
    CHECK(call("VirtualUnlock", heap_arr, 16, 0, 0) == 1, "VirtualUnlock");
    CHECK(call("FlushInstructionCache", 0xFFFFFFFFu, heap_arr, 16, 0) == 1,
          "FlushInstructionCache");

    if (g_failed) return 1;
    printf("hle memory OK (%d handlers)\n", installed);
    return 0;
}
