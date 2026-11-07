/*
 * msvcr80_sort_test.c - T0 contract test for the MSVCR80 qsort slice.
 *
 * The fake dispatcher reads the cdecl frame that qsort builds on the guest
 * stack and compares the first 32-bit word of each element.  The test checks
 * the comparison sequence, element movement for a wider record, argument
 * rejection, and that the per-call scratch block is released.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "sort_hle.h"

#define MAX_ROWS 4u
#define GUEST_BASE 0x20000u
#define GUEST_SIZE 0x10000u
#define ARRAY_ADDR (GUEST_BASE + 0x2000u)
#define SCRATCH_ADDR (GUEST_BASE + 0x4000u)
#define STACK_TOP (GUEST_BASE + 0x8000u)
#define CMP_VA 0x12345678u

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
static unsigned g_dispatch_count;
static unsigned g_free_count;
static unsigned g_bad_frame;
static uint32_t g_scratch_next = SCRATCH_ADDR;

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

uint32_t wc3_legacy_rd32g(uint32_t addr)
{
    uint32_t value = 0;
    const void *p = guest_ptr(addr, 4);
    if (p) memcpy(&value, p, 4);
    return value;
}

void wc3_legacy_wr32g(uint32_t addr, uint32_t value)
{
    void *p = guest_ptr(addr, 4);
    if (p) memcpy(p, &value, 4);
}

uint32_t wc3_legacy_arg32(const CPU *cpu, unsigned index)
{
    (void)cpu;
    return index < sizeof g_args / sizeof g_args[0] ? g_args[index] : 0u;
}

uint32_t wc3_legacy_guest_alloc0(uint32_t size)
{
    if (size > 512u) return 0;
    const uint32_t p = g_scratch_next;
    g_scratch_next += (size + 7u) & ~7u;
    memset(guest_ptr(p, size), 0, size);
    return p;
}

void wc3_legacy_guest_free(uint32_t addr)
{
    (void)addr;
    g_free_count++;
}

void wc3_legacy_guest_copy(uint32_t dst, uint32_t src, uint32_t size)
{
    void *d = guest_ptr(dst, size);
    const void *s = guest_ptr(src, size);
    if (d && s) memmove(d, s, size);
}

void dispatch(CPU *c, uint32_t target)
{
    if (target != CMP_VA) {
        g_bad_frame++;
        c->eax = 0;
        return;
    }
    if (wc3_legacy_rd32g(c->esp) != 0x7EFFFFF1u) g_bad_frame++;
    const uint32_t first = wc3_legacy_rd32g(c->esp + 4u);
    const uint32_t second = wc3_legacy_rd32g(c->esp + 8u);
    if (!wc3_legacy_guest_ok(first, 4) ||
        !wc3_legacy_guest_ok(second, 4)) {
        g_bad_frame++;
        c->eax = 0;
        return;
    }
    const int32_t a = (int32_t)wc3_legacy_rd32g(first);
    const int32_t b = (int32_t)wc3_legacy_rd32g(second);
    c->eax = (uint32_t)((a > b) - (a < b));
    g_dispatch_count++;
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

static void (*find_impl(const char *name))(CPU *c)
{
    for (unsigned i = 0; i < g_row_count; i++)
        if (strcmp(g_rows[i].name, name) == 0) return g_rows[i].impl;
    return NULL;
}

static void put_bytes(uint32_t addr, const void *data, uint32_t size)
{
    memcpy(guest_ptr(addr, size), data, size);
}

static void get_bytes(uint32_t addr, void *data, uint32_t size)
{
    memcpy(data, guest_ptr(addr, size), size);
}

static uint32_t call_qsort(uint32_t base, uint32_t count, uint32_t width,
                           uint32_t compare)
{
    void (*impl)(CPU *c) = find_impl("qsort");
    if (!impl) {
        check(0, "missing qsort");
        return 0xFFFFFFFFu;
    }
    memset(g_args, 0, sizeof g_args);
    g_args[0] = base;
    g_args[1] = count;
    g_args[2] = width;
    g_args[3] = compare;
    CPU c;
    memset(&c, 0, sizeof c);
    c.esp = STACK_TOP;
    impl(&c);
    check(c.esp == STACK_TOP, "qsort did not restore esp");
    return c.eax;
}

int main(void)
{
    const int installed = msvcr80_sort_install();
    check(installed == 1, "install count");
    check(g_row_count == 1u, "row count");
    check(strcmp(g_rows[0].dll, "MSVCR80.dll") == 0, "DLL name");
    check(find_impl("qsort") != NULL, "qsort row");

    {
        const int32_t input[] = { 5, 1, 4, 2, 3 };
        int32_t output[5];
        put_bytes(ARRAY_ADDR, input, sizeof input);
        check(call_qsort(ARRAY_ADDR, 5, 4, CMP_VA) == 0u, "qsort int");
        get_bytes(ARRAY_ADDR, output, sizeof output);
        static const int32_t expected[] = { 1, 2, 3, 4, 5 };
        check(memcmp(output, expected, sizeof expected) == 0,
              "qsort int order");
        check(g_dispatch_count == 9u, "qsort int comparisons");
        check(g_bad_frame == 0u, "qsort int frame");
        check(g_free_count == 1u, "qsort int scratch release");
    }

    {
        typedef struct Item {
            int32_t key;
            int32_t payload;
        } Item;
        const Item input[] = {
            { 3, 0x33 },
            { 1, 0x11 },
            { 2, 0x22 },
        };
        Item output[3];
        put_bytes(ARRAY_ADDR, input, sizeof input);
        check(call_qsort(ARRAY_ADDR, 3, sizeof(Item), CMP_VA) == 0u,
              "qsort record");
        get_bytes(ARRAY_ADDR, output, sizeof output);
        check(output[0].key == 1 && output[0].payload == 0x11,
              "qsort record first");
        check(output[1].key == 2 && output[1].payload == 0x22,
              "qsort record second");
        check(output[2].key == 3 && output[2].payload == 0x33,
              "qsort record third");
        check(g_free_count == 2u, "qsort record scratch release");
    }

    {
        const uint32_t before = g_dispatch_count;
        const int32_t input[] = { 7, 6 };
        put_bytes(ARRAY_ADDR, input, sizeof input);
        check(call_qsort(ARRAY_ADDR, 1, 4, CMP_VA) == 0u, "qsort count one");
        check(call_qsort(ARRAY_ADDR, 0, 4, CMP_VA) == 0u, "qsort count zero");
        check(g_dispatch_count == before, "qsort short arrays do not compare");
        check(g_free_count == 2u, "qsort short arrays do not allocate");
    }

    {
        const uint32_t before = g_dispatch_count;
        const uint32_t before_free = g_free_count;
        check(call_qsort(ARRAY_ADDR, 2, 0, CMP_VA) == 0u, "qsort width zero");
        check(call_qsort(ARRAY_ADDR, 2, 257, CMP_VA) == 0u,
              "qsort width too large");
        check(call_qsort(0, 2, 4, CMP_VA) == 0u, "qsort null base");
        check(call_qsort(GUEST_BASE + GUEST_SIZE - 4u, 2, 4, CMP_VA) == 0u,
              "qsort short array");
        check(call_qsort(ARRAY_ADDR, 0x40000000u, 8, CMP_VA) == 0u,
              "qsort size overflow");
        check(g_dispatch_count == before, "qsort rejected calls do not compare");
        check(g_free_count == before_free,
              "qsort rejected calls do not allocate");
    }

    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("msvcr80 sort OK (%d handler)\n", installed);
    return 0;
}
