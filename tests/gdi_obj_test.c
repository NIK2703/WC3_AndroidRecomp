/*
 * gdi_obj_test.c - T0 contract test for GDI32 regions, pens and line state.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "gdi_obj_hle.h"
#include "legacy_shim.h"

#define MAX_ROWS 32u
#define GUEST_BASE 0x10000000u
#define GUEST_SIZE 0x20000u

#define RGN_ERROR 0u
#define RGN_NULL 1u
#define RGN_SIMPLE 2u
#define RGN_COMPLEX 3u

#define RGN_AND 1u
#define RGN_OR 2u
#define RGN_XOR 3u
#define RGN_DIFF 4u
#define RGN_COPY 5u

typedef struct Row {
    const char *dll;
    const char *name;
    int argc;
    void (*impl)(CPU *c);
} Row;

static Row g_overrides[MAX_ROWS];
static unsigned g_override_count;
static Row g_additions[MAX_ROWS];
static unsigned g_addition_count;
static uint32_t g_args[8];
static uint8_t g_memory[GUEST_SIZE];

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    if (g_override_count >= MAX_ROWS) return -1;
    g_overrides[g_override_count].dll = dll;
    g_overrides[g_override_count].name = name;
    g_overrides[g_override_count].argc = 0;
    g_overrides[g_override_count].impl = impl;
    g_override_count++;
    return 0;
}

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    (void)cdecl_abi;
    if (g_addition_count >= MAX_ROWS) return -1;
    g_additions[g_addition_count].dll = dll;
    g_additions[g_addition_count].name = name;
    g_additions[g_addition_count].argc = argc;
    g_additions[g_addition_count].impl = impl;
    g_addition_count++;
    return 0;
}

uint32_t wc3_legacy_arg32(const CPU *c, unsigned index)
{
    (void)c;
    return index < 8u ? g_args[index] : 0u;
}

int wc3_legacy_guest_ok(uint32_t address, uint32_t size)
{
    if (size == 0u) return 1;
    if (address < GUEST_BASE) return 0;
    const uint64_t end = (uint64_t)address + size;
    return end <= (uint64_t)GUEST_BASE + GUEST_SIZE;
}

uint8_t wc3_legacy_rd8g(uint32_t address)
{
    return g_memory[address - GUEST_BASE];
}

uint16_t wc3_legacy_rd16g(uint32_t address)
{
    uint16_t value;
    memcpy(&value, g_memory + address - GUEST_BASE, sizeof value);
    return value;
}

uint32_t wc3_legacy_rd32g(uint32_t address)
{
    uint32_t value;
    memcpy(&value, g_memory + address - GUEST_BASE, sizeof value);
    return value;
}

void wc3_legacy_wr8g(uint32_t address, uint8_t value)
{
    g_memory[address - GUEST_BASE] = value;
}

void wc3_legacy_wr16g(uint32_t address, uint16_t value)
{
    memcpy(g_memory + address - GUEST_BASE, &value, sizeof value);
}

void wc3_legacy_wr32g(uint32_t address, uint32_t value)
{
    memcpy(g_memory + address - GUEST_BASE, &value, sizeof value);
}

static uint32_t guest(uint32_t offset)
{
    return GUEST_BASE + offset;
}

static void put_u32(uint32_t offset, uint32_t value)
{
    memcpy(g_memory + offset, &value, sizeof value);
}

static uint32_t get_u32(uint32_t offset)
{
    uint32_t value;
    memcpy(&value, g_memory + offset, sizeof value);
    return value;
}

static void put_rect(uint32_t offset, int32_t left, int32_t top,
                     int32_t right, int32_t bottom)
{
    put_u32(offset + 0u, (uint32_t)left);
    put_u32(offset + 4u, (uint32_t)top);
    put_u32(offset + 8u, (uint32_t)right);
    put_u32(offset + 12u, (uint32_t)bottom);
}

static void (*find_impl(const char *name, int *argc))(CPU *c)
{
    for (unsigned i = 0; i < g_override_count; i++) {
        if (strcmp(g_overrides[i].name, name) == 0) {
            if (argc) *argc = g_overrides[i].argc;
            return g_overrides[i].impl;
        }
    }
    for (unsigned i = 0; i < g_addition_count; i++) {
        if (strcmp(g_additions[i].name, name) == 0) {
            if (argc) *argc = g_additions[i].argc;
            return g_additions[i].impl;
        }
    }
    return NULL;
}

static uint32_t invoke(const char *name, const uint32_t *args, unsigned count)
{
    void (*impl)(CPU *c) = find_impl(name, NULL);
    if (!impl) return 0xFFFFFFFEu;
    for (unsigned i = 0; i < 8u; i++) g_args[i] = 0u;
    for (unsigned i = 0; i < count && i < 8u; i++) g_args[i] = args[i];
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

static uint32_t create_rect(int32_t left, int32_t top,
                            int32_t right, int32_t bottom)
{
    const uint32_t args[4] = { (uint32_t)left, (uint32_t)top,
                               (uint32_t)right, (uint32_t)bottom };
    return invoke("CreateRectRgn", args, 4u);
}

static uint32_t combine(uint32_t dest, uint32_t a, uint32_t b, uint32_t mode)
{
    const uint32_t args[4] = { dest, a, b, mode };
    return invoke("CombineRgn", args, 4u);
}

static int rect_in(uint32_t region, int32_t left, int32_t top,
                   int32_t right, int32_t bottom)
{
    put_rect(0x3000u, left, top, right, bottom);
    const uint32_t args[2] = { region, guest(0x3000u) };
    return invoke("RectInRegion", args, 2u) != 0u;
}

int main(void)
{
    const int installed = wc3_hle_gdi32_objects_install();
    CHECK(installed == 11, "install count");
    CHECK(g_override_count == 1u, "override count");
    CHECK(g_addition_count == 10u, "addition count");

    struct {
        const char *name;
        int argc;
    } expected[] = {
        { "CreateRectRgn", 4 },
        { "CreateRectRgnIndirect", 1 },
        { "CombineRgn", 4 },
        { "FillRgn", 3 },
        { "RectInRegion", 2 },
        { "CreatePen", 3 },
        { "MoveToEx", 4 },
        { "LineTo", 3 },
        { "Rectangle", 5 },
        { "GdiFlush", 0 },
    };
    for (unsigned i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        int argc = -1;
        CHECK(find_impl(expected[i].name, &argc) != NULL, expected[i].name);
        CHECK(argc == expected[i].argc, "argc");
    }

    const uint32_t r1 = create_rect(10, 10, 20, 20);
    const uint32_t r2 = create_rect(30, 30, 40, 40);
    const uint32_t big = create_rect(0, 0, 100, 100);
    const uint32_t hole = create_rect(40, 40, 60, 60);
    const uint32_t empty = create_rect(5, 5, 5, 5);
    const uint32_t inverted = create_rect(30, 30, 10, 10);
    const uint32_t dest = create_rect(0, 0, 0, 0);
    CHECK(r1 && r2 && big && hole && empty && inverted && dest,
          "create regions");
    CHECK(rect_in(r1, 5, 5, 15, 15), "rect overlap");
    CHECK(!rect_in(r1, 30, 30, 40, 40), "rect outside");
    CHECK(rect_in(inverted, 15, 15, 16, 16), "inverted normalized");

    put_rect(0x3100u, 30, 30, 10, 10);
    const uint32_t indirect_args[1] = { guest(0x3100u) };
    const uint32_t indirect = invoke("CreateRectRgnIndirect", indirect_args, 1u);
    CHECK(indirect != 0u, "create indirect");
    CHECK(rect_in(indirect, 15, 15, 16, 16), "indirect normalized");

    CHECK(combine(dest, r1, 0u, RGN_COPY) == RGN_SIMPLE, "copy");
    CHECK(rect_in(dest, 15, 15, 16, 16), "copy rect");

    CHECK(combine(dest, r1, r2, RGN_OR) == RGN_COMPLEX, "or");
    CHECK(rect_in(dest, 15, 15, 16, 16), "or first");
    CHECK(rect_in(dest, 35, 35, 36, 36), "or second");

    const uint32_t overlap = create_rect(15, 15, 25, 25);
    CHECK(combine(dest, r1, overlap, RGN_AND) == RGN_SIMPLE, "and");
    CHECK(rect_in(dest, 16, 16, 17, 17), "and inside");
    CHECK(!rect_in(dest, 22, 22, 23, 23), "and outside");

    CHECK(combine(dest, big, hole, RGN_DIFF) == RGN_COMPLEX, "diff");
    CHECK(rect_in(dest, 10, 10, 11, 11), "diff outside hole");
    CHECK(!rect_in(dest, 50, 50, 51, 51), "diff hole");
    CHECK(rect_in(dest, 50, 10, 51, 11), "diff above hole");

    CHECK(combine(dest, big, hole, RGN_XOR) == RGN_COMPLEX, "xor");
    CHECK(rect_in(dest, 10, 10, 11, 11), "xor outside");
    CHECK(!rect_in(dest, 50, 50, 51, 51), "xor hole");

    CHECK(combine(dest, empty, 0u, RGN_COPY) == RGN_NULL, "copy empty");
    CHECK(!rect_in(dest, 0, 0, 1, 1), "empty region");
    CHECK(combine(dest, r1, r2, 99u) == RGN_ERROR, "bad combine mode");

    const uint32_t pen_args[3] = { 0u, 1u, 0x0000FF00u };
    const uint32_t pen = invoke("CreatePen", pen_args, 3u);
    CHECK(pen != 0u, "create pen");
    const uint32_t fill_args[3] = { 0x00030000u, r1, pen };
    CHECK(invoke("FillRgn", fill_args, 3u) == 1u, "fill region");
    const uint32_t bad_fill[3] = { 0x00030000u, 0u, pen };
    CHECK(invoke("FillRgn", bad_fill, 3u) == 0u, "fill bad region");
    const uint32_t no_brush[3] = { 0x00030000u, r1, 0u };
    CHECK(invoke("FillRgn", no_brush, 3u) == 0u, "fill no brush");

    const uint32_t rect_args[5] = { 0x00030000u, 0u, 0u, 10u, 10u };
    CHECK(invoke("Rectangle", rect_args, 5u) == 1u, "rectangle");
    const uint32_t bad_rect[5] = { 0u, 0u, 0u, 10u, 10u };
    CHECK(invoke("Rectangle", bad_rect, 5u) == 0u, "rectangle no DC");
    const uint32_t flush_args[1] = { 0u };
    CHECK(invoke("GdiFlush", flush_args, 0u) == 1u, "gdi flush");

    put_u32(0x3200u, 0xAAAAAAAAu);
    put_u32(0x3204u, 0xBBBBBBBBu);
    const uint32_t move1[4] = { 0x00030000u, 5u, 6u, guest(0x3200u) };
    CHECK(invoke("MoveToEx", move1, 4u) == 1u, "move to");
    CHECK(get_u32(0x3200u) == 0u && get_u32(0x3204u) == 0u, "old origin");
    const uint32_t move2[4] = { 0x00030000u, 10u, 11u, guest(0x3200u) };
    CHECK(invoke("MoveToEx", move2, 4u) == 1u, "move to again");
    CHECK(get_u32(0x3200u) == 5u && get_u32(0x3204u) == 6u, "old position");
    const uint32_t line[3] = { 0x00030000u, 20u, 21u };
    CHECK(invoke("LineTo", line, 3u) == 1u, "line to");
    const uint32_t move3[4] = { 0x00030000u, 0u, 0u, guest(0x3200u) };
    CHECK(invoke("MoveToEx", move3, 4u) == 1u, "move after line");
    CHECK(get_u32(0x3200u) == 20u && get_u32(0x3204u) == 21u,
          "line position");
    const uint32_t bad_move[4] = { 0u, 0u, 0u, guest(0x3200u) };
    CHECK(invoke("MoveToEx", bad_move, 4u) == 0u, "move no DC");

    const uint32_t delete_pen[1] = { pen };
    CHECK(invoke("DeleteObject", delete_pen, 1u) == 1u, "delete pen");
    CHECK(invoke("DeleteObject", delete_pen, 1u) == 0u, "delete pen twice");
    const uint32_t delete_legacy[1] = { 0x00070000u };
    CHECK(invoke("DeleteObject", delete_legacy, 1u) == 1u,
          "delete legacy object");
    const uint32_t delete_r1[1] = { r1 };
    CHECK(invoke("DeleteObject", delete_r1, 1u) == 1u, "delete region");
    CHECK(invoke("DeleteObject", delete_r1, 1u) == 0u, "delete region twice");

    printf("gdi objects OK (%d handlers)\n", installed);
    return 0;
}
