/*
 * user32_menu_test.c - T0 contract test for USER32 menu objects.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "menu_hle.h"

#define MAX_ROWS 32u
#define GUEST_BASE 0x10000000u
#define GUEST_SIZE 0x10000u

#define MIIM_STATE 0x00000001u
#define MIIM_ID 0x00000002u
#define MIIM_SUBMENU 0x00000004u
#define MIIM_TYPE 0x00000010u
#define MIIM_STRING 0x00000040u
#define MIIM_FTYPE 0x00000100u
#define MIIM_BITMAP 0x00000080u

#define MFS_CHECKED 0x00000008u
#define MF_BYPOSITION 0x00000400u
#define MF_BYCOMMAND 0x00000000u

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

static void put_string(uint32_t offset, const char *text)
{
    strcpy((char *)(g_memory + offset), text);
}

static void put_mii(uint32_t offset, uint32_t cb_size, uint32_t mask,
                    uint32_t type, uint32_t state, uint32_t id,
                    uint32_t submenu, uint32_t bitmap_checked,
                    uint32_t bitmap_unchecked, uint32_t item_data,
                    uint32_t text_address, uint32_t text_capacity,
                    uint32_t bitmap_item)
{
    put_u32(offset + 0u, cb_size);
    put_u32(offset + 4u, mask);
    put_u32(offset + 8u, type);
    put_u32(offset + 12u, state);
    put_u32(offset + 16u, id);
    put_u32(offset + 20u, submenu);
    put_u32(offset + 24u, bitmap_checked);
    put_u32(offset + 28u, bitmap_unchecked);
    put_u32(offset + 32u, item_data);
    put_u32(offset + 36u, text_address);
    put_u32(offset + 40u, text_capacity);
    put_u32(offset + 44u, bitmap_item);
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

int main(void)
{
    const int installed = wc3_hle_user32_menu_install();
    CHECK(installed == 12, "install count");
    CHECK(g_override_count == 5u, "override count");
    CHECK(g_addition_count == 7u, "addition count");

    struct {
        const char *name;
        int argc;
    } expected[] = {
        { "SetMenu", 2 },
        { "DrawMenuBar", 1 },
        { "InsertMenuItemA", 4 },
        { "SetMenuItemInfoA", 4 },
        { "GetMenuItemInfoA", 4 },
        { "DeleteMenu", 3 },
        { "TrackPopupMenu", 7 },
    };
    for (unsigned i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        int argc = -1;
        CHECK(find_impl(expected[i].name, &argc) != NULL, expected[i].name);
        CHECK(argc == expected[i].argc, "argc");
    }

    const uint32_t menu = invoke("CreateMenu", NULL, 0u);
    const uint32_t popup = invoke("CreatePopupMenu", NULL, 0u);
    CHECK(menu >= 0x00110000u, "create menu");
    CHECK(popup >= 0x00110000u && popup != menu, "create popup");

    put_string(0x2000u, "File");
    put_mii(0x1000u, 48u,
            MIIM_ID | MIIM_STRING | MIIM_FTYPE | MIIM_STATE,
            0u, 0u, 100u, 0u, 0u, 0u, 0u,
            guest(0x2000u), 16u, 0u);
    const uint32_t insert_file[4] = { menu, 0u, 1u, guest(0x1000u) };
    CHECK(invoke("InsertMenuItemA", insert_file, 4u) == 1u, "insert file");

    put_string(0x2010u, "Exit");
    put_mii(0x1000u, 48u,
            MIIM_ID | MIIM_STRING | MIIM_FTYPE | MIIM_STATE,
            0u, 0u, 101u, 0u, 0u, 0u, 0u,
            guest(0x2010u), 16u, 0u);
    const uint32_t insert_exit[4] = { menu, 1u, 1u, guest(0x1000u) };
    CHECK(invoke("InsertMenuItemA", insert_exit, 4u) == 1u, "insert exit");

    const uint32_t count_menu[1] = { menu };
    CHECK(invoke("GetMenuItemCount", count_menu, 1u) == 2u, "count two");

    put_mii(0x1000u, 48u,
            MIIM_ID | MIIM_STRING | MIIM_FTYPE | MIIM_STATE,
            0u, 0u, 0u, 0u, 0u, 0u, 0u,
            guest(0x2100u), 16u, 0u);
    const uint32_t get_file[4] = { menu, 0u, 1u, guest(0x1000u) };
    CHECK(invoke("GetMenuItemInfoA", get_file, 4u) == 1u, "get file");
    CHECK(get_u32(0x1010u) == 100u, "file id");
    CHECK(get_u32(0x1008u) == 0u, "file type");
    CHECK(get_u32(0x100Cu) == 0u, "file state");
    CHECK(get_u32(0x1010u) == 100u, "file id repeated");
    CHECK(get_u32(0x1028u) == 4u, "file text length");
    CHECK(strcmp((char *)(g_memory + 0x2100u), "File") == 0, "file text");

    put_mii(0x1000u, 48u,
            MIIM_ID | MIIM_STRING | MIIM_FTYPE | MIIM_STATE,
            0u, 0u, 0u, 0u, 0u, 0u, 0u,
            guest(0x2110u), 16u, 0u);
    const uint32_t get_exit[4] = { menu, 101u, 0u, guest(0x1000u) };
    CHECK(invoke("GetMenuItemInfoA", get_exit, 4u) == 1u, "get exit");
    CHECK(get_u32(0x1010u) == 101u, "exit id");
    CHECK(strcmp((char *)(g_memory + 0x2110u), "Exit") == 0, "exit text");

    put_mii(0x1000u, 48u, MIIM_STRING, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
            guest(0x2120u), 2u, 0u);
    const uint32_t get_small[4] = { menu, 101u, 0u, guest(0x1000u) };
    CHECK(invoke("GetMenuItemInfoA", get_small, 4u) == 0u,
          "small text buffer");
    CHECK(get_u32(0x1028u) == 5u, "required text length");

    put_mii(0x1000u, 48u, MIIM_STATE | MIIM_ID, 0u, MFS_CHECKED,
            200u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
    const uint32_t set_file[4] = { menu, 0u, 1u, guest(0x1000u) };
    CHECK(invoke("SetMenuItemInfoA", set_file, 4u) == 1u, "set file state");

    put_mii(0x1000u, 48u, MIIM_ID | MIIM_STATE | MIIM_STRING,
            0u, 0u, 0u, 0u, 0u, 0u, 0u, guest(0x2130u), 16u, 0u);
    const uint32_t get_command[4] = { menu, 200u, 0u, guest(0x1000u) };
    CHECK(invoke("GetMenuItemInfoA", get_command, 4u) == 1u,
          "get command 200");
    CHECK(get_u32(0x1010u) == 200u, "command id");
    CHECK(get_u32(0x100Cu) == MFS_CHECKED, "checked state");
    CHECK(strcmp((char *)(g_memory + 0x2130u), "File") == 0,
          "state text retained");

    put_string(0x2020u, "Sub");
    put_mii(0x1000u, 48u,
            MIIM_SUBMENU | MIIM_STRING | MIIM_FTYPE,
            0u, 0u, 0u, popup, 0u, 0u, 0u,
            guest(0x2020u), 16u, 0u);
    const uint32_t insert_sub[4] = { menu, 2u, 1u, guest(0x1000u) };
    CHECK(invoke("InsertMenuItemA", insert_sub, 4u) == 1u, "insert submenu");
    CHECK(invoke("GetMenuItemCount", count_menu, 1u) == 3u, "count three");

    put_mii(0x1000u, 48u,
            MIIM_SUBMENU | MIIM_STRING | MIIM_FTYPE,
            0u, 0u, 0u, 0u, 0u, 0u, 0u, guest(0x2140u), 16u, 0u);
    const uint32_t get_sub[4] = { menu, 2u, 1u, guest(0x1000u) };
    CHECK(invoke("GetMenuItemInfoA", get_sub, 4u) == 1u, "get submenu");
    CHECK(get_u32(0x1014u) == popup, "submenu handle");
    CHECK(strcmp((char *)(g_memory + 0x2140u), "Sub") == 0, "submenu text");

    const uint32_t hwnd = 0x00020000u;
    const uint32_t set_window_menu[2] = { hwnd, menu };
    CHECK(invoke("SetMenu", set_window_menu, 2u) == 1u, "set window menu");
    const uint32_t get_window_menu[1] = { hwnd };
    CHECK(invoke("GetMenu", get_window_menu, 1u) == menu, "get window menu");
    CHECK(invoke("DrawMenuBar", get_window_menu, 1u) == 1u,
          "draw menu bar");
    const uint32_t clear_menu[2] = { hwnd, 0u };
    CHECK(invoke("SetMenu", clear_menu, 2u) == 1u, "clear window menu");
    CHECK(invoke("GetMenu", get_window_menu, 1u) == 0u, "menu cleared");
    const uint32_t bad_window_menu[2] = { 0u, menu };
    CHECK(invoke("SetMenu", bad_window_menu, 2u) == 0u, "set zero hwnd");
    CHECK(invoke("DrawMenuBar", bad_window_menu, 1u) == 0u,
          "draw zero hwnd");
    const uint32_t bad_menu[2] = { hwnd, 0x00123456u };
    CHECK(invoke("SetMenu", bad_menu, 2u) == 0u, "set invalid menu");

    const uint32_t track[7] = { popup, 0u, 0u, 0u, 0u, hwnd, 0u };
    CHECK(invoke("TrackPopupMenu", track, 7u) == 0u, "track popup");

    const uint32_t delete_exit[3] = { menu, 1u, MF_BYPOSITION };
    CHECK(invoke("DeleteMenu", delete_exit, 3u) == 1u, "delete exit");
    CHECK(invoke("GetMenuItemCount", count_menu, 1u) == 2u,
          "count after exit");
    CHECK(invoke("GetMenuItemInfoA", get_exit, 4u) == 0u,
          "exit gone");

    const uint32_t delete_file[3] = { menu, 200u, MF_BYCOMMAND };
    CHECK(invoke("DeleteMenu", delete_file, 3u) == 1u, "delete file");
    CHECK(invoke("GetMenuItemCount", count_menu, 1u) == 1u,
          "count after file");

    const uint32_t delete_sub[3] = { menu, 0u, MF_BYPOSITION };
    CHECK(invoke("DeleteMenu", delete_sub, 3u) == 1u, "delete submenu item");
    CHECK(invoke("GetMenuItemCount", count_menu, 1u) == 0u,
          "menu empty");
    const uint32_t destroy_popup[1] = { popup };
    CHECK(invoke("DestroyMenu", destroy_popup, 1u) == 0u,
          "popup destroyed recursively");
    const uint32_t destroy_menu[1] = { menu };
    CHECK(invoke("DestroyMenu", destroy_menu, 1u) == 1u, "destroy menu");
    CHECK(invoke("DestroyMenu", destroy_menu, 1u) == 0u,
          "destroy menu twice");
    CHECK(invoke("GetMenuItemCount", count_menu, 1u) == 0xFFFFFFFFu,
          "count invalid menu");

    put_mii(0x1000u, 40u, MIIM_STRING, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
            guest(0x2150u), 16u, 0u);
    const uint32_t bad_info[4] = { menu, 0u, 1u, guest(0x1000u) };
    CHECK(invoke("InsertMenuItemA", bad_info, 4u) == 0u,
          "insert bad cbSize");
    CHECK(invoke("GetMenuItemInfoA", bad_info, 4u) == 0u,
          "get bad cbSize");

    printf("menu OK (%d handlers)\n", installed);
    return 0;
}
