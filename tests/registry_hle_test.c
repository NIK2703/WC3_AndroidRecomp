/*
 * registry_hle_test.c - T0 contract test for the target ADVAPI32 registry.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "registry_hle.h"

#define MAX_ROWS 32u
#define GUEST_BASE 0x10000000u
#define GUEST_SIZE 0x20000u
#define HKCU 0x80000001u

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
static uint32_t g_args[12];
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
    return index < 12u ? g_args[index] : 0u;
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

void wc3_legacy_guest_zero(uint32_t address, uint32_t size)
{
    memset(g_memory + address - GUEST_BASE, 0, size);
}

int wc3_legacy_default_display_mode(uint32_t *width, uint32_t *height)
{
    if (width) *width = 1280u;
    if (height) *height = 720u;
    return 1;
}

static uint32_t guest(uint32_t offset)
{
    return GUEST_BASE + offset;
}

static void put_string(uint32_t offset, const char *text)
{
    strcpy((char *)(g_memory + offset), text);
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
    for (unsigned i = 0; i < 12u; i++) g_args[i] = 0u;
    for (unsigned i = 0; i < count && i < 12u; i++) g_args[i] = args[i];
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

static int has_name(char names[][64], unsigned count, const char *wanted)
{
    for (unsigned i = 0; i < count; i++) {
        if (strcmp(names[i], wanted) == 0) return 1;
    }
    return 0;
}

static unsigned enum_children(uint32_t key, char names[][64], unsigned capacity)
{
    unsigned count = 0u;
    for (unsigned index = 0; index < capacity; index++) {
        put_u32(0x1050u, 64u);
        const uint32_t args[8] = { key, index, guest(0x3000u), guest(0x1050u),
                                   0u, 0u, 0u, 0u };
        const uint32_t rc = invoke("RegEnumKeyExA", args, 8u);
        if (rc == 259u) break;
        if (rc != 0u) return 0u;
        const uint32_t length = get_u32(0x1050u);
        if (length >= 64u) return 0u;
        memcpy(names[count], g_memory + 0x3000u, length);
        names[count][length] = '\0';
        count++;
    }
    return count;
}

int main(void)
{
    const int installed = wc3_hle_advapi32_registry_install();
    CHECK(installed == 10, "install count");
    CHECK(g_override_count == 5u, "override count");
    CHECK(g_addition_count == 5u, "addition count");

    struct {
        const char *name;
        int argc;
    } expected[] = {
        { "RegDeleteKeyA", 2 },
        { "RegDeleteValueA", 2 },
        { "RegFlushKey", 1 },
        { "RegEnumKeyExA", 8 },
        { "RegQueryInfoKeyA", 12 },
    };
    for (unsigned i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        int argc = -1;
        CHECK(find_impl(expected[i].name, &argc) != NULL, expected[i].name);
        CHECK(argc == expected[i].argc, "argc");
    }

    put_string(0x0100u, "Software\\Blizzard Entertainment\\Warcraft III");
    const uint32_t open_root[5] = { HKCU, guest(0x0100u), 0u, 0u,
                                    guest(0x1000u) };
    CHECK(invoke("RegOpenKeyExA", open_root, 5u) == 0u, "open root");
    const uint32_t root = get_u32(0x1000u);
    CHECK(root >= 0x00150000u, "root handle");

    put_string(0x0200u,
               "Software\\Blizzard Entertainment\\Warcraft III\\Sound");
    const uint32_t open_sound[5] = { HKCU, guest(0x0200u), 0u, 0u,
                                     guest(0x1004u) };
    CHECK(invoke("RegOpenKeyExA", open_sound, 5u) == 0u, "open sound");
    const uint32_t sound = get_u32(0x1004u);
    CHECK(sound >= 0x00150000u, "sound handle");

    put_string(0x0300u, "music");
    put_u32(0x1010u, 0u);
    put_u32(0x1014u, 4u);
    const uint32_t query_music[6] = { sound, guest(0x0300u), 0u,
                                      guest(0x1010u), guest(0x2000u),
                                      guest(0x1014u) };
    CHECK(invoke("RegQueryValueExA", query_music, 6u) == 0u, "query music");
    CHECK(get_u32(0x1010u) == 4u, "music type");
    CHECK(get_u32(0x1014u) == 4u, "music length");
    CHECK(get_u32(0x2000u) == 1u, "music value");

    put_string(0x0310u, "MUSIC");
    const uint32_t query_upper[6] = { sound, guest(0x0310u), 0u,
                                      guest(0x1018u), guest(0x2004u),
                                      guest(0x101Cu) };
    put_u32(0x101Cu, 4u);
    CHECK(invoke("RegQueryValueExA", query_upper, 6u) == 0u,
          "query case-insensitive");
    CHECK(get_u32(0x2004u) == 1u, "upper music value");

    put_string(0x0320u, "missing");
    put_u32(0x1020u, 4u);
    const uint32_t query_missing[6] = { sound, guest(0x0320u), 0u,
                                        guest(0x1024u), guest(0x2008u),
                                        guest(0x1020u) };
    CHECK(invoke("RegQueryValueExA", query_missing, 6u) == 2u,
          "query missing");

    put_string(0x0400u, "custom");
    put_string(0x2100u, "abc");
    const uint32_t set_custom[6] = { sound, guest(0x0400u), 0u, 1u,
                                     guest(0x2100u), 4u };
    CHECK(invoke("RegSetValueExA", set_custom, 6u) == 0u, "set custom");

    put_u32(0x1030u, 2u);
    const uint32_t query_small[6] = { sound, guest(0x0400u), 0u,
                                      guest(0x1034u), guest(0x2200u),
                                      guest(0x1030u) };
    CHECK(invoke("RegQueryValueExA", query_small, 6u) == 234u,
          "query small buffer");
    CHECK(get_u32(0x1030u) == 4u, "required length");

    put_u32(0x1038u, 0u);
    const uint32_t query_size[6] = { sound, guest(0x0400u), 0u,
                                     guest(0x103Cu), 0u, guest(0x1038u) };
    CHECK(invoke("RegQueryValueExA", query_size, 6u) == 0u, "query size");
    CHECK(get_u32(0x1038u) == 4u, "size length");

    put_u32(0x1040u, 8u);
    const uint32_t query_custom[6] = { sound, guest(0x0400u), 0u,
                                       guest(0x1044u), guest(0x2300u),
                                       guest(0x1040u) };
    CHECK(invoke("RegQueryValueExA", query_custom, 6u) == 0u,
          "query custom");
    CHECK(get_u32(0x1044u) == 1u, "custom type");
    CHECK(get_u32(0x1040u) == 4u, "custom length");
    CHECK(memcmp(g_memory + 0x2300u, "abc", 4u) == 0, "custom data");

    const uint32_t delete_custom[2] = { sound, guest(0x0400u) };
    CHECK(invoke("RegDeleteValueA", delete_custom, 2u) == 0u,
          "delete custom");
    CHECK(invoke("RegQueryValueExA", query_custom, 6u) == 2u,
          "custom deleted");
    CHECK(invoke("RegDeleteValueA", delete_custom, 2u) == 2u,
          "delete missing value");

    put_string(0x0500u, "SubKey");
    const uint32_t create_sub[9] = { root, guest(0x0500u), 0u, 0u, 0u,
                                     0x000F003Fu, 0u, guest(0x1058u),
                                     guest(0x105Cu) };
    CHECK(invoke("RegCreateKeyExA", create_sub, 9u) == 0u, "create subkey");
    const uint32_t subkey = get_u32(0x1058u);
    CHECK(subkey >= 0x00150000u, "subkey handle");
    CHECK(get_u32(0x105Cu) == 1u, "created disposition");

    put_string(0x0510u, "x");
    put_u32(0x1060u, 42u);
    const uint32_t set_sub[6] = { subkey, guest(0x0510u), 0u, 4u,
                                  guest(0x1060u), 4u };
    CHECK(invoke("RegSetValueExA", set_sub, 6u) == 0u, "set subkey value");

    char names[16][64];
    const unsigned child_count = enum_children(root, names, 16u);
    CHECK(child_count == 3u, "child count");
    CHECK(has_name(names, child_count, "Sound"), "enum Sound");
    CHECK(has_name(names, child_count, "Video"), "enum Video");
    CHECK(has_name(names, child_count, "SubKey"), "enum SubKey");

    put_u32(0x1070u, 0u);
    const uint32_t enum_end[8] = { root, 3u, guest(0x3000u), guest(0x1070u),
                                   0u, 0u, 0u, 0u };
    CHECK(invoke("RegEnumKeyExA", enum_end, 8u) == 259u, "enum end");

    const uint32_t info[12] = { root, 0u, 0u, 0u, guest(0x1080u),
                                guest(0x1084u), guest(0x1088u),
                                guest(0x108Cu), guest(0x1090u),
                                guest(0x1094u), 0u, 0u };
    CHECK(invoke("RegQueryInfoKeyA", info, 12u) == 0u, "query info");
    CHECK(get_u32(0x1080u) == 3u, "info subkeys");
    CHECK(get_u32(0x1084u) == 6u, "info max subkey");
    CHECK(get_u32(0x1088u) == 0u, "info max class");
    CHECK(get_u32(0x108Cu) == 0u, "info values");
    CHECK(get_u32(0x1090u) == 0u, "info max value name");
    CHECK(get_u32(0x1094u) == 0u, "info max data");

    const uint32_t delete_sub[2] = { root, guest(0x0500u) };
    CHECK(invoke("RegDeleteKeyA", delete_sub, 2u) == 0u, "delete subkey");
    const unsigned after_delete = enum_children(root, names, 16u);
    CHECK(after_delete == 2u, "child count after delete");
    CHECK(!has_name(names, after_delete, "SubKey"), "subkey gone");
    CHECK(invoke("RegDeleteKeyA", delete_sub, 2u) == 2u,
          "delete missing subkey");

    const uint32_t flush[1] = { root };
    CHECK(invoke("RegFlushKey", flush, 1u) == 0u, "flush");
    const uint32_t close_sound[1] = { sound };
    CHECK(invoke("RegCloseKey", close_sound, 1u) == 0u, "close");
    CHECK(invoke("RegCloseKey", close_sound, 1u) == 6u, "close twice");

    printf("registry OK (%d handlers)\n", installed);
    return 0;
}
