/*
 * gdi_palette_test.c - T0 contract test for target GDI32 palettes.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "gdi_palette_hle.h"
#include "hle_bridge.h"

#define MAX_ROWS 16u
#define GUEST_BASE 0x10000000u
#define GUEST_SIZE 0x10000u

typedef struct ChainRow {
    const char *name;
    void (*impl)(CPU *c);
    void (*previous)(CPU *c);
} ChainRow;

typedef struct AddRow {
    const char *name;
    int argc;
    void (*impl)(CPU *c);
} AddRow;

static ChainRow g_chain[MAX_ROWS];
static unsigned g_chain_count;
static AddRow g_additions[MAX_ROWS];
static unsigned g_addition_count;
static uint32_t g_args[8];
static uint8_t g_memory[GUEST_SIZE];
static unsigned g_previous_delete_calls;

static void old_delete_object(CPU *c)
{
    g_previous_delete_calls++;
    c->eax = wc3_legacy_arg32(c, 0u) ? 1u : 0u;
}

int wc3_legacy_hle_chain(const char *dll, const char *name,
                         void (*impl)(CPU *c),
                         void (**previous)(CPU *c))
{
    (void)dll;
    if (strcmp(name, "DeleteObject") != 0 || g_chain_count >= MAX_ROWS)
        return -1;
    g_chain[g_chain_count].name = name;
    g_chain[g_chain_count].impl = impl;
    g_chain[g_chain_count].previous = old_delete_object;
    g_chain_count++;
    *previous = old_delete_object;
    return 0;
}

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    (void)dll;
    (void)cdecl_abi;
    if (g_addition_count >= MAX_ROWS) return -1;
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

void wc3_legacy_wr8g(uint32_t address, uint8_t value)
{
    g_memory[address - GUEST_BASE] = value;
}

static uint32_t guest(uint32_t offset)
{
    return GUEST_BASE + offset;
}

static void put_u8(uint32_t offset, uint8_t value)
{
    g_memory[offset] = value;
}

static void put_u16(uint32_t offset, uint16_t value)
{
    memcpy(g_memory + offset, &value, sizeof value);
}

static uint8_t get_u8(uint32_t offset)
{
    return g_memory[offset];
}

static void put_entry(uint32_t offset, uint8_t red, uint8_t green,
                      uint8_t blue, uint8_t flags)
{
    put_u8(offset + 0u, red);
    put_u8(offset + 1u, green);
    put_u8(offset + 2u, blue);
    put_u8(offset + 3u, flags);
}

static void put_palette(uint32_t offset, uint16_t version, uint16_t count)
{
    put_u16(offset + 0u, version);
    put_u16(offset + 2u, count);
}

static void (*find_impl(const char *name, int *argc))(CPU *c)
{
    for (unsigned i = 0; i < g_chain_count; i++) {
        if (strcmp(g_chain[i].name, name) == 0) {
            if (argc) *argc = 0;
            return g_chain[i].impl;
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

int main(void)
{
    const int installed = wc3_hle_gdi32_palette_install();
    CHECK(installed == 6, "install count");
    CHECK(g_chain_count == 1u, "chain count");
    CHECK(g_addition_count == 5u, "addition count");

    int argc = -1;
    CHECK(find_impl("DeleteObject", &argc) != NULL, "delete chain");
    struct {
        const char *name;
        int argc;
    } expected[] = {
        { "CreatePalette", 1 },
        { "SetPaletteEntries", 4 },
        { "SelectPalette", 3 },
        { "RealizePalette", 1 },
        { "GetSystemPaletteEntries", 4 },
    };
    for (unsigned i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        CHECK(find_impl(expected[i].name, &argc) != NULL, expected[i].name);
        CHECK(argc == expected[i].argc, "argc");
    }

    put_palette(0x1000u, 0x0300u, 4u);
    put_entry(0x1004u, 10u, 20u, 30u, 0u);
    put_entry(0x1008u, 40u, 50u, 60u, 1u);
    put_entry(0x100Cu, 70u, 80u, 90u, 2u);
    put_entry(0x1010u, 100u, 110u, 120u, 3u);

    const uint32_t create[1] = { guest(0x1000u) };
    const uint32_t palette1 = invoke("CreatePalette", create, 1u);
    CHECK(palette1 == 0x00120000u, "create palette handle");

    put_palette(0x1100u, 0x0300u, 2u);
    put_entry(0x1104u, 1u, 2u, 3u, 0u);
    put_entry(0x1108u, 4u, 5u, 6u, 0u);
    const uint32_t create2[1] = { guest(0x1100u) };
    const uint32_t palette2 = invoke("CreatePalette", create2, 1u);
    CHECK(palette2 == 0x00120004u, "second palette handle");

    put_palette(0x1200u, 0x0300u, 2u);
    put_entry(0x1204u, 7u, 8u, 9u, 0u);
    put_entry(0x1208u, 10u, 11u, 12u, 0u);
    const uint32_t create3[1] = { guest(0x1200u) };
    CHECK(invoke("CreatePalette", create3, 1u) == 0x00120008u,
          "third palette handle");

    put_palette(0x1300u, 0x0400u, 2u);
    const uint32_t bad_version[1] = { guest(0x1300u) };
    CHECK(invoke("CreatePalette", bad_version, 1u) == 0u, "bad version");
    put_palette(0x1310u, 0x0300u, 0u);
    const uint32_t zero_count[1] = { guest(0x1310u) };
    CHECK(invoke("CreatePalette", zero_count, 1u) == 0u, "zero count");
    put_palette(0x1320u, 0x0300u, 257u);
    const uint32_t too_many[1] = { guest(0x1320u) };
    CHECK(invoke("CreatePalette", too_many, 1u) == 0u, "too many entries");
    const uint32_t null_palette[1] = { 0u };
    CHECK(invoke("CreatePalette", null_palette, 1u) == 0u, "null palette");
    const uint32_t out_of_range[1] = { guest(0xFFFEu) };
    CHECK(invoke("CreatePalette", out_of_range, 1u) == 0u,
          "invalid palette pointer");

    put_entry(0x1400u, 200u, 201u, 202u, 4u);
    put_entry(0x1404u, 210u, 211u, 212u, 5u);
    const uint32_t set_entries[4] = {
        palette1, 1u, 2u, guest(0x1400u),
    };
    CHECK(invoke("SetPaletteEntries", set_entries, 4u) == 2u,
          "set palette entries");
    const uint32_t set_bad_start[4] = {
        palette1, 4u, 1u, guest(0x1400u),
    };
    CHECK(invoke("SetPaletteEntries", set_bad_start, 4u) == 0u,
          "bad start");
    const uint32_t set_bad_range[4] = {
        palette1, 3u, 2u, guest(0x1400u),
    };
    CHECK(invoke("SetPaletteEntries", set_bad_range, 4u) == 0u,
          "bad range");
    const uint32_t set_bad_handle[4] = {
        0x001200FFu, 0u, 1u, guest(0x1400u),
    };
    CHECK(invoke("SetPaletteEntries", set_bad_handle, 4u) == 0u,
          "bad palette handle");
    const uint32_t set_zero[4] = {
        palette1, 0u, 0u, guest(0x1400u),
    };
    CHECK(invoke("SetPaletteEntries", set_zero, 4u) == 0u, "zero set");
    const uint32_t set_bad_pointer[4] = {
        palette1, 0u, 1u, guest(0xFFFEu),
    };
    CHECK(invoke("SetPaletteEntries", set_bad_pointer, 4u) == 0u,
          "set invalid pointer");
    const uint32_t set_too_many[4] = {
        palette1, 0u, 257u, guest(0x1400u),
    };
    CHECK(invoke("SetPaletteEntries", set_too_many, 4u) == 0u,
          "set too many");

    const uint32_t hdc1 = 0x00030000u;
    const uint32_t hdc2 = 0x00030004u;
    const uint32_t hdc3 = 0x00030008u;
    const uint32_t select1[3] = { hdc1, palette1, 0u };
    CHECK(invoke("SelectPalette", select1, 3u) == 0u,
          "select first palette");
    const uint32_t select2[3] = { hdc2, palette1, 0u };
    CHECK(invoke("SelectPalette", select2, 3u) == 0u,
          "select palette on second dc");
    const uint32_t select_bad[3] = { hdc1, 0x001200FFu, 0u };
    CHECK(invoke("SelectPalette", select_bad, 3u) == 0u,
          "select invalid palette");
    const uint32_t select_zero_dc[3] = { 0u, palette1, 0u };
    CHECK(invoke("SelectPalette", select_zero_dc, 3u) == 0u,
          "select null dc");
    const uint32_t select_second[3] = { hdc1, palette2, 0u };
    CHECK(invoke("SelectPalette", select_second, 3u) == palette1,
          "select second palette returns previous");

    const uint32_t realize1[1] = { hdc1 };
    CHECK(invoke("RealizePalette", realize1, 1u) == 2u,
          "realize second palette");
    CHECK(invoke("RealizePalette", realize1, 1u) == 0u,
          "realize is idempotent");
    const uint32_t realize2[1] = { hdc2 };
    CHECK(invoke("RealizePalette", realize2, 1u) == 4u,
          "realize palette on second dc");
    CHECK(invoke("RealizePalette", realize2, 1u) == 0u,
          "second realize is idempotent");
    const uint32_t realize3[1] = { hdc3 };
    CHECK(invoke("RealizePalette", realize3, 1u) == 0u,
          "realize without palette");
    const uint32_t realize_zero[1] = { 0u };
    CHECK(invoke("RealizePalette", realize_zero, 1u) == 0u,
          "realize null dc");

    put_entry(0x1410u, 33u, 44u, 55u, 6u);
    const uint32_t update2[4] = {
        palette2, 0u, 1u, guest(0x1410u),
    };
    CHECK(invoke("SetPaletteEntries", update2, 4u) == 1u,
          "update selected palette");
    CHECK(invoke("RealizePalette", realize1, 1u) == 2u,
          "realize after update");

    const uint32_t system_output = guest(0x2000u);
    const uint32_t system_entries[4] = {
        hdc1, 0u, 256u, system_output,
    };
    CHECK(invoke("GetSystemPaletteEntries", system_entries, 4u) == 20u,
          "system palette count");
    CHECK(get_u8(0x2000u) == 0u && get_u8(0x2001u) == 0u &&
          get_u8(0x2002u) == 0u && get_u8(0x2003u) == 0u,
          "system palette black");
    CHECK(get_u8(0x2004u) == 0x80u && get_u8(0x2005u) == 0u &&
          get_u8(0x2006u) == 0u,
          "system palette maroon");
    CHECK(get_u8(0x2024u) == 0xA6u && get_u8(0x2025u) == 0xCAu &&
          get_u8(0x2026u) == 0xF0u,
          "system palette tenth entry");
    CHECK(get_u8(0x204Cu) == 0xFFu && get_u8(0x204Du) == 0x80u &&
          get_u8(0x204Eu) == 0x80u,
          "system palette last entry");

    const uint32_t system_tail[4] = {
        hdc1, 18u, 10u, guest(0x2100u),
    };
    CHECK(invoke("GetSystemPaletteEntries", system_tail, 4u) == 2u,
          "system palette tail count");
    CHECK(get_u8(0x2100u) == 0xFFu && get_u8(0x2101u) == 0x60u &&
          get_u8(0x2102u) == 0x60u,
          "system palette tail first");
    CHECK(get_u8(0x2104u) == 0xFFu && get_u8(0x2105u) == 0x80u &&
          get_u8(0x2106u) == 0x80u,
          "system palette tail second");
    const uint32_t system_bad_start[4] = {
        hdc1, 20u, 1u, guest(0x2100u),
    };
    CHECK(invoke("GetSystemPaletteEntries", system_bad_start, 4u) == 0u,
          "system palette bad start");
    const uint32_t system_zero[4] = {
        hdc1, 0u, 0u, guest(0x2100u),
    };
    CHECK(invoke("GetSystemPaletteEntries", system_zero, 4u) == 0u,
          "system palette zero count");
    const uint32_t system_null[4] = {
        hdc1, 0u, 1u, 0u,
    };
    CHECK(invoke("GetSystemPaletteEntries", system_null, 4u) == 0u,
          "system palette null buffer");
    const uint32_t system_zero_dc[4] = {
        0u, 0u, 1u, guest(0x2100u),
    };
    CHECK(invoke("GetSystemPaletteEntries", system_zero_dc, 4u) == 0u,
          "system palette null dc");
    const uint32_t system_bad_pointer[4] = {
        hdc1, 0u, 1u, guest(0xFFFEu),
    };
    CHECK(invoke("GetSystemPaletteEntries", system_bad_pointer, 4u) == 0u,
          "system palette invalid pointer");

    const uint32_t delete_palette2[1] = { palette2 };
    CHECK(invoke("DeleteObject", delete_palette2, 1u) == 0u,
          "delete selected palette");
    CHECK(g_previous_delete_calls == 0u, "delete palette does not chain");
    const uint32_t deselect1[3] = { hdc1, 0u, 0u };
    CHECK(invoke("SelectPalette", deselect1, 3u) == palette2,
          "deselect palette");
    CHECK(invoke("DeleteObject", delete_palette2, 1u) == 1u,
          "delete palette after deselect");
    CHECK(invoke("DeleteObject", delete_palette2, 1u) == 0u,
          "delete palette twice");
    const uint32_t delete_palette1[1] = { palette1 };
    CHECK(invoke("DeleteObject", delete_palette1, 1u) == 0u,
          "delete palette selected on second dc");
    const uint32_t deselect2[3] = { hdc2, 0u, 0u };
    CHECK(invoke("SelectPalette", deselect2, 3u) == palette1,
          "deselect second palette");
    CHECK(invoke("DeleteObject", delete_palette1, 1u) == 1u,
          "delete palette after second deselect");

    const uint32_t delete_region[1] = { 0x000F0000u };
    CHECK(invoke("DeleteObject", delete_region, 1u) == 1u,
          "non-palette object chains");
    CHECK(g_previous_delete_calls == 1u, "previous delete called");
    const uint32_t delete_zero[1] = { 0u };
    CHECK(invoke("DeleteObject", delete_zero, 1u) == 0u, "delete null");
    CHECK(g_previous_delete_calls == 2u, "null delete chains");

    printf("gdi palette OK (%d handlers)\n", installed);
    return 0;
}
