/*
 * scroll_hle_test.c - T0 contract test for USER32 scroll-bar state.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "scroll_hle.h"

#define MAX_ROWS 16u
#define GUEST_BASE 0x10000000u
#define GUEST_SIZE 0x10000u

#define SIF_RANGE 0x0001u
#define SIF_PAGE 0x0002u
#define SIF_POS 0x0004u
#define SIF_ALL 0x0017u

#define SB_HORZ 0u
#define SB_VERT 1u

typedef struct Row {
    const char *dll;
    const char *name;
    int argc;
    void (*impl)(CPU *c);
} Row;

static Row g_additions[MAX_ROWS];
static unsigned g_addition_count;
static uint32_t g_args[8];
static uint8_t g_memory[GUEST_SIZE];

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

static void put_info(uint32_t offset, uint32_t cb_size, uint32_t mask,
                     int32_t minimum, int32_t maximum, uint32_t page,
                     int32_t position, int32_t track)
{
    put_u32(offset + 0u, cb_size);
    put_u32(offset + 4u, mask);
    put_u32(offset + 8u, (uint32_t)minimum);
    put_u32(offset + 12u, (uint32_t)maximum);
    put_u32(offset + 16u, page);
    put_u32(offset + 20u, (uint32_t)position);
    put_u32(offset + 24u, (uint32_t)track);
}

static void (*find_impl(const char *name, int *argc))(CPU *c)
{
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

int main(void)
{
    const int installed = wc3_hle_user32_scroll_install();
    CHECK(installed == 3, "install count");
    CHECK(g_addition_count == 3u, "addition count");

    struct {
        const char *name;
        int argc;
    } expected[] = {
        { "GetScrollInfo", 3 },
        { "SetScrollInfo", 4 },
        { "SetScrollPos", 4 },
    };
    for (unsigned i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        int argc = -1;
        CHECK(find_impl(expected[i].name, &argc) != NULL, expected[i].name);
        CHECK(argc == expected[i].argc, "argc");
    }

    const uint32_t window = 0x00020000u;
    const uint32_t other = 0x00020004u;

    put_info(0x1000u, 28u, SIF_ALL, 0, 100, 10u, 20, 0);
    const uint32_t set_all[4] = { window, SB_VERT, guest(0x1000u), 1u };
    CHECK(invoke("SetScrollInfo", set_all, 4u) == 0u, "set initial info");

    put_info(0x1000u, 28u, SIF_ALL, 0, 0, 0u, 0, 0);
    const uint32_t get_info[3] = { window, SB_VERT, guest(0x1000u) };
    CHECK(invoke("GetScrollInfo", get_info, 3u) == 1u, "get initial info");
    CHECK(get_u32(0x1000u) == 28u, "cbSize");
    CHECK(get_u32(0x1008u) == 0u, "minimum");
    CHECK(get_u32(0x100Cu) == 100u, "maximum");
    CHECK(get_u32(0x1010u) == 10u, "page");
    CHECK(get_u32(0x1014u) == 20u, "position");
    CHECK(get_u32(0x1018u) == 20u, "track position");

    const uint32_t set_pos[4] = { window, SB_VERT, 50u, 1u };
    CHECK(invoke("SetScrollPos", set_pos, 4u) == 20u, "set position old");
    CHECK(invoke("GetScrollInfo", get_info, 3u) == 1u, "get position");
    CHECK(get_u32(0x1014u) == 50u, "position 50");

    const uint32_t set_high[4] = { window, SB_VERT, 200u, 1u };
    CHECK(invoke("SetScrollPos", set_high, 4u) == 50u, "set high old");
    CHECK(invoke("GetScrollInfo", get_info, 3u) == 1u, "get high");
    CHECK(get_u32(0x1014u) == 91u, "position clamped to page end");

    put_info(0x1000u, 28u, SIF_RANGE, 10, 20, 0u, 0, 0);
    const uint32_t set_range[4] = { window, SB_VERT, guest(0x1000u), 1u };
    CHECK(invoke("SetScrollInfo", set_range, 4u) == 91u, "set range old");
    put_info(0x1000u, 28u, SIF_ALL, 0, 0, 0u, 0, 0);
    CHECK(invoke("GetScrollInfo", get_info, 3u) == 1u, "get range");
    CHECK(get_u32(0x1008u) == 10u, "range minimum");
    CHECK(get_u32(0x100Cu) == 20u, "range maximum");
    CHECK(get_u32(0x1014u) == 11u, "range clamped position");

    put_info(0x1000u, 28u, SIF_PAGE, 0, 0, 0u, 0, 0);
    const uint32_t set_page[4] = { window, SB_VERT, guest(0x1000u), 1u };
    CHECK(invoke("SetScrollInfo", set_page, 4u) == 11u, "set page old");
    put_info(0x1000u, 28u, SIF_ALL, 0, 0, 0u, 0, 0);
    CHECK(invoke("GetScrollInfo", get_info, 3u) == 1u, "get page zero");
    CHECK(get_u32(0x1010u) == 0u, "page zero");
    CHECK(get_u32(0x1014u) == 11u, "position unchanged");

    put_info(0x1000u, 28u, SIF_POS, 0, 0, 0u, 16, 0);
    const uint32_t set_only_pos[4] = { window, SB_VERT, guest(0x1000u), 1u };
    CHECK(invoke("SetScrollInfo", set_only_pos, 4u) == 11u,
          "set only position old");
    put_info(0x1000u, 28u, SIF_ALL, 0, 0, 0u, 0, 0);
    CHECK(invoke("GetScrollInfo", get_info, 3u) == 1u, "get only position");
    CHECK(get_u32(0x1014u) == 16u, "only position");

    put_info(0x1000u, 28u, SIF_ALL, 0, 0, 0u, 0, 0);
    const uint32_t get_horizontal[3] = { window, SB_HORZ, guest(0x1000u) };
    CHECK(invoke("GetScrollInfo", get_horizontal, 3u) == 1u,
          "get horizontal");
    CHECK(get_u32(0x1008u) == 0u && get_u32(0x100Cu) == 0u &&
          get_u32(0x1010u) == 0u && get_u32(0x1014u) == 0u,
          "horizontal defaults");

    put_info(0x1000u, 28u, SIF_ALL, 0, 0, 0u, 0, 0);
    const uint32_t get_other[3] = { other, SB_VERT, guest(0x1000u) };
    CHECK(invoke("GetScrollInfo", get_other, 3u) == 1u, "get other window");
    CHECK(get_u32(0x1014u) == 0u, "other window independent");
    put_info(0x1000u, 28u, SIF_RANGE, 0, 20, 0u, 0, 0);
    const uint32_t set_other_range[4] = { other, SB_VERT, guest(0x1000u), 0u };
    CHECK(invoke("SetScrollInfo", set_other_range, 4u) == 0u,
          "set other range");
    const uint32_t set_other[4] = { other, SB_VERT, 7u, 0u };
    CHECK(invoke("SetScrollPos", set_other, 4u) == 0u, "set other window");
    put_info(0x1000u, 28u, SIF_ALL, 0, 0, 0u, 0, 0);
    CHECK(invoke("GetScrollInfo", get_other, 3u) == 1u,
          "get other after set");
    CHECK(get_u32(0x1014u) == 7u, "other window position");

    put_info(0x1000u, 28u, SIF_ALL, 0, 100, 10u, 20, 0);
    const uint32_t bad_bar[3] = { window, 3u, guest(0x1000u) };
    CHECK(invoke("GetScrollInfo", bad_bar, 3u) == 0u, "invalid bar get");
    const uint32_t set_bad_bar[4] = { window, 3u, guest(0x1000u), 0u };
    CHECK(invoke("SetScrollInfo", set_bad_bar, 4u) == 0u,
          "invalid bar set");
    CHECK(invoke("SetScrollPos", set_bad_bar, 4u) == 0u,
          "invalid bar position");

    const uint32_t bad_pointer[3] = { window, SB_VERT, 0u };
    CHECK(invoke("GetScrollInfo", bad_pointer, 3u) == 0u,
          "null info get");
    const uint32_t null_set[4] = { window, SB_VERT, 0u, 0u };
    CHECK(invoke("SetScrollInfo", null_set, 4u) == 0u, "null info set");

    put_info(0x1000u, 24u, SIF_ALL, 0, 100, 10u, 20, 0);
    CHECK(invoke("SetScrollInfo", set_all, 4u) == 0u, "small cbSize set");
    CHECK(invoke("GetScrollInfo", get_info, 3u) == 0u, "small cbSize get");

    printf("scroll OK (%d handlers)\n", installed);
    return 0;
}
