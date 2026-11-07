/*
 * user32_window_facts_test.c - T0 contract test for target window facts.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "window_facts_hle.h"

#define MAX_ROWS 32u
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
static uint32_t g_args[12];
static uint8_t g_memory[GUEST_SIZE];
static uint32_t g_next_atom = 0xC001u;
static uint32_t g_next_handle = 0x00020004u;
static unsigned g_old_register_calls;
static unsigned g_old_create_calls;
static unsigned g_old_destroy_calls;
static unsigned g_old_show_calls;
static unsigned g_old_setpos_calls;
static uint32_t g_last_error;

static void old_register_class(CPU *c)
{
    g_old_register_calls++;
    c->eax = g_next_atom++;
}

static void old_create_window(CPU *c)
{
    g_old_create_calls++;
    c->eax = g_next_handle;
    g_next_handle += 4u;
}

static void old_destroy_window(CPU *c)
{
    g_old_destroy_calls++;
    c->eax = 1u;
}

static void old_show_window(CPU *c)
{
    g_old_show_calls++;
    c->eax = 1u;
}

static void old_set_window_pos(CPU *c)
{
    g_old_setpos_calls++;
    c->eax = 1u;
}

static void (*old_for(const char *name))(CPU *c)
{
    if (strcmp(name, "RegisterClassExA") == 0) return old_register_class;
    if (strcmp(name, "CreateWindowExA") == 0) return old_create_window;
    if (strcmp(name, "DestroyWindow") == 0) return old_destroy_window;
    if (strcmp(name, "ShowWindow") == 0) return old_show_window;
    if (strcmp(name, "SetWindowPos") == 0) return old_set_window_pos;
    return NULL;
}

int wc3_legacy_hle_chain(const char *dll, const char *name,
                         void (*impl)(CPU *c),
                         void (**previous)(CPU *c))
{
    (void)dll;
    if (g_chain_count >= MAX_ROWS) return -1;
    void (*old)(CPU *c) = old_for(name);
    if (!old) return -1;
    g_chain[g_chain_count].name = name;
    g_chain[g_chain_count].impl = impl;
    g_chain[g_chain_count].previous = old;
    g_chain_count++;
    *previous = old;
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
    return index < 12u ? g_args[index] : 0u;
}

int wc3_legacy_current_display_mode(uint32_t *width, uint32_t *height)
{
    if (!width || !height) return 0;
    *width = 1024u;
    *height = 768u;
    return 1;
}

int wc3_legacy_default_display_mode(uint32_t *width, uint32_t *height)
{
    return wc3_legacy_current_display_mode(width, height);
}

void wc3_legacy_set_last_error(uint32_t code)
{
    g_last_error = code;
}

uint32_t wc3_legacy_get_last_error(void)
{
    return g_last_error;
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

static void put_wndclassex(uint32_t offset, const char *class_name)
{
    memset(g_memory + offset, 0, 48u);
    put_string(0x2000u, class_name);
    put_u32(offset + 40u, guest(0x2000u));
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

int main(void)
{
    const int installed = wc3_hle_user32_window_facts_install();
    CHECK(installed == 16, "install count");
    CHECK(g_chain_count == 5u, "chain count");
    CHECK(g_addition_count == 11u, "addition count");

    const char *chain_names[] = {
        "RegisterClassExA", "CreateWindowExA", "DestroyWindow",
        "ShowWindow", "SetWindowPos",
    };
    for (unsigned i = 0; i < 5u; i++)
        CHECK(find_impl(chain_names[i], NULL) != NULL, chain_names[i]);

    struct {
        const char *name;
        int argc;
    } expected[] = {
        { "GetTopWindow", 1 },
        { "GetClassNameA", 3 },
        { "IsWindow", 1 },
        { "GetWindowPlacement", 2 },
        { "IsIconic", 1 },
        { "AdjustWindowRectEx", 4 },
        { "UnregisterClassA", 2 },
        { "IsDialogMessageA", 2 },
        { "MapWindowPoints", 4 },
        { "MonitorFromWindow", 2 },
        { "GetMonitorInfoA", 2 },
    };
    for (unsigned i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        int argc = -1;
        CHECK(find_impl(expected[i].name, &argc) != NULL, expected[i].name);
        CHECK(argc == expected[i].argc, "argc");
    }

    put_wndclassex(0x1000u, "Warcraft");
    const uint32_t register_main[1] = { guest(0x1000u) };
    CHECK(invoke("RegisterClassExA", register_main, 1u) == 0xC001u,
          "register main class");
    CHECK(g_old_register_calls == 1u, "previous register called");

    const uint32_t create_main[12] = {
        0u, guest(0x2000u), guest(0x2010u), 0x00CF0000u,
        10u, 20u, 800u, 600u, 0u, 0u, 0u, 0u,
    };
    put_string(0x2010u, "Warcraft III");
    CHECK(invoke("CreateWindowExA", create_main, 12u) == 0x00020004u,
          "create main window");
    CHECK(g_old_create_calls == 1u, "previous create called");

    const uint32_t main_handle = 0x00020004u;
    const uint32_t desktop = 0x00010000u;
    const uint32_t is_main[1] = { main_handle };
    CHECK(invoke("IsWindow", is_main, 1u) == 1u, "main is window");
    const uint32_t is_unknown[1] = { 0x000200FFu };
    CHECK(invoke("IsWindow", is_unknown, 1u) == 0u, "unknown is not window");
    const uint32_t is_desktop[1] = { desktop };
    CHECK(invoke("IsWindow", is_desktop, 1u) == 1u, "desktop is window");
    const uint32_t is_zero[1] = { 0u };
    CHECK(invoke("IsWindow", is_zero, 1u) == 0u, "zero is not window");

    const uint32_t dialog_message[2] = { main_handle, guest(0x7000u) };
    CHECK(invoke("IsDialogMessageA", dialog_message, 2u) == 0u,
          "main window is not a dialog");
    const uint32_t dialog_message_null[2] = { 0u, 0u };
    CHECK(invoke("IsDialogMessageA", dialog_message_null, 2u) == 0u,
          "null dialog message");

    put_u32(0x7000u, 10u);
    put_u32(0x7004u, 20u);
    const uint32_t map_to_screen[4] = {
        main_handle, 0u, guest(0x7000u), 1u,
    };
    g_last_error = 0u;
    CHECK(invoke("MapWindowPoints", map_to_screen, 4u) == 1u,
          "map point to screen");
    CHECK(get_u32(0x7000u) == 20u && get_u32(0x7004u) == 40u,
          "mapped screen point");
    const uint32_t map_to_client[4] = {
        0u, main_handle, guest(0x7000u), 1u,
    };
    CHECK(invoke("MapWindowPoints", map_to_client, 4u) == 1u,
          "map point to client");
    CHECK(get_u32(0x7000u) == 10u && get_u32(0x7004u) == 20u,
          "mapped client point");
    put_u32(0x7008u, 0u);
    put_u32(0x700Cu, 0u);
    const uint32_t map_two[4] = {
        main_handle, 0u, guest(0x7000u), 2u,
    };
    CHECK(invoke("MapWindowPoints", map_two, 4u) == 2u,
          "map two points");
    CHECK(get_u32(0x7008u) == 10u && get_u32(0x700Cu) == 20u,
          "mapped second point");
    const uint32_t map_bad[4] = {
        0x000200FFu, 0u, guest(0x7000u), 1u,
    };
    g_last_error = 0u;
    CHECK(invoke("MapWindowPoints", map_bad, 4u) == 0u,
          "map bad window");
    CHECK(g_last_error == 1400u, "map bad window error");
    const uint32_t map_null[4] = {
        main_handle, 0u, 0u, 1u,
    };
    g_last_error = 0u;
    CHECK(invoke("MapWindowPoints", map_null, 4u) == 0u,
          "map null points");
    CHECK(g_last_error == 87u, "map null points error");
    const uint32_t map_none[4] = {
        main_handle, 0u, 0u, 0u,
    };
    CHECK(invoke("MapWindowPoints", map_none, 4u) == 0u,
          "map zero points");

    const uint32_t monitor_handle = 0x00190000u;
    const uint32_t monitor_from_null[2] = { 0u, 0u };
    CHECK(invoke("MonitorFromWindow", monitor_from_null, 2u) == 0u,
          "null window default-to-null");
    const uint32_t monitor_from_null_primary[2] = { 0u, 1u };
    CHECK(invoke("MonitorFromWindow", monitor_from_null_primary, 2u) ==
              monitor_handle,
          "null window primary");
    const uint32_t monitor_from_main[2] = { main_handle, 0u };
    CHECK(invoke("MonitorFromWindow", monitor_from_main, 2u) == monitor_handle,
          "main window monitor");
    const uint32_t monitor_from_unknown[2] = { 0x000200FFu, 0u };
    CHECK(invoke("MonitorFromWindow", monitor_from_unknown, 2u) == 0u,
          "unknown window default-to-null");
    const uint32_t monitor_from_unknown_nearest[2] = { 0x000200FFu, 2u };
    CHECK(invoke("MonitorFromWindow", monitor_from_unknown_nearest, 2u) ==
              monitor_handle,
          "unknown window nearest");
    const uint32_t monitor_from_desktop[2] = { desktop, 1u };
    CHECK(invoke("MonitorFromWindow", monitor_from_desktop, 2u) ==
              monitor_handle,
          "desktop monitor");

    put_u32(0x6000u, 40u);
    const uint32_t get_monitor[2] = { monitor_handle, guest(0x6000u) };
    g_last_error = 99u;
    CHECK(invoke("GetMonitorInfoA", get_monitor, 2u) == 1u,
          "get monitor info");
    CHECK(get_u32(0x6000u) == 40u, "monitor info size");
    CHECK(get_u32(0x6004u) == 0u && get_u32(0x6008u) == 0u,
          "monitor origin");
    CHECK(get_u32(0x600Cu) == 1024u && get_u32(0x6010u) == 768u,
          "monitor rect");
    CHECK(get_u32(0x6014u) == 0u && get_u32(0x6018u) == 0u,
          "work origin");
    CHECK(get_u32(0x601Cu) == 1024u && get_u32(0x6020u) == 768u,
          "work rect");
    CHECK(get_u32(0x6024u) == 1u, "primary monitor flag");
    CHECK(g_last_error == 99u, "successful monitor info keeps error");

    put_u32(0x6000u, 72u);
    g_last_error = 0u;
    CHECK(invoke("GetMonitorInfoA", get_monitor, 2u) == 1u,
          "get monitor info ex");
    CHECK(strcmp((char *)(g_memory + 0x6028u), "\\\\.\\DISPLAY1") == 0,
          "monitor device name");

    const uint32_t get_bad_monitor[2] = { 0x00180000u, guest(0x6000u) };
    g_last_error = 0u;
    CHECK(invoke("GetMonitorInfoA", get_bad_monitor, 2u) == 0u,
          "bad monitor handle");
    CHECK(g_last_error == 1461u, "bad monitor error");
    const uint32_t get_null_monitor[2] = { monitor_handle, 0u };
    g_last_error = 0u;
    CHECK(invoke("GetMonitorInfoA", get_null_monitor, 2u) == 0u,
          "null monitor info");
    CHECK(g_last_error == 87u, "null monitor info error");
    put_u32(0x6000u, 41u);
    g_last_error = 0u;
    CHECK(invoke("GetMonitorInfoA", get_monitor, 2u) == 0u,
          "bad monitor info size");
    CHECK(g_last_error == 87u, "bad monitor info size error");

    put_string(0x3000u, "unused");
    const uint32_t get_name[3] = { main_handle, guest(0x3000u), 64u };
    CHECK(invoke("GetClassNameA", get_name, 3u) == 8u, "class name length");
    CHECK(strcmp((char *)(g_memory + 0x3000u), "Warcraft") == 0,
          "class name");
    const uint32_t get_desktop_name[3] = { desktop, guest(0x3000u), 64u };
    CHECK(invoke("GetClassNameA", get_desktop_name, 3u) == 6u,
          "desktop class length");
    CHECK(strcmp((char *)(g_memory + 0x3000u), "#32769") == 0,
          "desktop class");
    const uint32_t get_unknown_name[3] = { 0x000200FFu, guest(0x3000u), 64u };
    CHECK(invoke("GetClassNameA", get_unknown_name, 3u) == 0u,
          "unknown class");
    const uint32_t get_small_name[3] = { main_handle, guest(0x3000u), 4u };
    CHECK(invoke("GetClassNameA", get_small_name, 3u) == 3u,
          "truncated class length");
    CHECK(strcmp((char *)(g_memory + 0x3000u), "War") == 0,
          "truncated class");

    const uint32_t top_null[1] = { 0u };
    CHECK(invoke("GetTopWindow", top_null, 1u) == main_handle,
          "top window null");
    const uint32_t top_desktop[1] = { desktop };
    CHECK(invoke("GetTopWindow", top_desktop, 1u) == main_handle,
          "top window desktop");
    const uint32_t top_main[1] = { main_handle };
    CHECK(invoke("GetTopWindow", top_main, 1u) == 0u,
          "main has no child");

    put_wndclassex(0x1100u, "Child");
    const uint32_t register_child[1] = { guest(0x1100u) };
    CHECK(invoke("RegisterClassExA", register_child, 1u) == 0xC002u,
          "register child class");
    put_string(0x2020u, "Child");
    const uint32_t create_child[12] = {
        0u, guest(0x2020u), guest(0x2030u), 0x40000000u,
        0u, 0u, 320u, 200u, main_handle, 0u, 0u, 0u,
    };
    put_string(0x2030u, "Child window");
    CHECK(invoke("CreateWindowExA", create_child, 12u) == 0x00020008u,
          "create child window");
    const uint32_t child_handle = 0x00020008u;
    CHECK(invoke("GetTopWindow", top_main, 1u) == child_handle,
          "top child");
    CHECK(invoke("GetTopWindow", top_null, 1u) == main_handle,
          "top-level ignores child");

    const uint32_t create_atom[12] = {
        0u, 0xC001u, 0u, 0x00CF0000u,
        0u, 0u, 800u, 600u, 0u, 0u, 0u, 0u,
    };
    CHECK(invoke("CreateWindowExA", create_atom, 12u) == 0x0002000Cu,
          "create by class atom");
    const uint32_t atom_handle = 0x0002000Cu;
    const uint32_t get_atom_name[3] = { atom_handle, guest(0x3000u), 64u };
    CHECK(invoke("GetClassNameA", get_atom_name, 3u) == 8u,
          "atom class length");
    CHECK(strcmp((char *)(g_memory + 0x3000u), "Warcraft") == 0,
          "atom class name");

    const uint32_t show_min[2] = { main_handle, 6u };
    CHECK(invoke("ShowWindow", show_min, 2u) == 1u, "show minimized");
    CHECK(g_old_show_calls == 1u, "previous show called");
    CHECK(invoke("IsIconic", is_main, 1u) == 1u, "is iconic");
    const uint32_t show_restore[2] = { main_handle, 9u };
    CHECK(invoke("ShowWindow", show_restore, 2u) == 1u, "show restored");
    CHECK(invoke("IsIconic", is_main, 1u) == 0u, "restored not iconic");
    CHECK(invoke("IsIconic", is_unknown, 1u) == 0u, "unknown not iconic");

    const uint32_t set_pos[7] = {
        main_handle, 0u, 100u, 200u, 1024u, 768u, 0u,
    };
    CHECK(invoke("SetWindowPos", set_pos, 7u) == 1u, "set window pos");
    CHECK(g_old_setpos_calls == 1u, "previous setpos called");

    const uint32_t placement = guest(0x4000u);
    const uint32_t get_placement[2] = { main_handle, placement };
    CHECK(invoke("GetWindowPlacement", get_placement, 2u) == 1u,
          "get placement");
    CHECK(get_u32(0x4000u) == 44u, "placement size");
    CHECK(get_u32(0x4008u) == 1u, "placement show normal");
    CHECK(get_u32(0x401Cu) == 100u, "placement left");
    CHECK(get_u32(0x4020u) == 200u, "placement top");
    CHECK(get_u32(0x4024u) == 1124u, "placement right");
    CHECK(get_u32(0x4028u) == 968u, "placement bottom");

    const uint32_t hide[2] = { main_handle, 0u };
    CHECK(invoke("ShowWindow", hide, 2u) == 1u, "hide window");
    CHECK(invoke("GetWindowPlacement", get_placement, 2u) == 1u,
          "placement hidden");
    CHECK(get_u32(0x4008u) == 0u, "placement show hidden");
    CHECK(invoke("ShowWindow", show_restore, 2u) == 1u, "restore hidden");

    const uint32_t rectangle = guest(0x5000u);
    put_u32(0x5000u, 0u);
    put_u32(0x5004u, 0u);
    put_u32(0x5008u, 800u);
    put_u32(0x500Cu, 600u);
    const uint32_t adjust[4] = { rectangle, 0x00CF0000u, 0u, 0u };
    CHECK(invoke("AdjustWindowRectEx", adjust, 4u) == 1u, "adjust rect");
    CHECK(get_u32(0x5000u) == 0u && get_u32(0x5008u) == 800u,
          "adjust rect identity");
    const uint32_t adjust_null[4] = { 0u, 0u, 0u, 0u };
    CHECK(invoke("AdjustWindowRectEx", adjust_null, 4u) == 0u,
          "adjust null rect");

    const uint32_t get_unknown_placement[2] = { 0x000200FFu, placement };
    CHECK(invoke("GetWindowPlacement", get_unknown_placement, 2u) == 0u,
          "unknown placement");
    const uint32_t get_desktop_placement[2] = { desktop, placement };
    CHECK(invoke("GetWindowPlacement", get_desktop_placement, 2u) == 0u,
          "desktop placement");

    const uint32_t destroy_child[1] = { child_handle };
    CHECK(invoke("DestroyWindow", destroy_child, 1u) == 1u, "destroy child");
    CHECK(g_old_destroy_calls == 1u, "previous destroy called");
    CHECK(invoke("IsWindow", &child_handle, 1u) == 0u, "child destroyed");
    CHECK(invoke("GetTopWindow", top_main, 1u) == 0u, "no child left");

    put_string(0x2040u, "Child");
    const uint32_t unregister_child[2] = { guest(0x2040u), 0u };
    CHECK(invoke("UnregisterClassA", unregister_child, 2u) == 1u,
          "unregister child class");
    CHECK(invoke("UnregisterClassA", unregister_child, 2u) == 0u,
          "unregister child twice");
    put_string(0x2050u, "Warcraft");
    const uint32_t unregister_main[2] = { guest(0x2050u), 0u };
    CHECK(invoke("UnregisterClassA", unregister_main, 2u) == 0u,
          "unregister used class");
    const uint32_t unregister_atom[2] = { 0xC001u, 0u };
    CHECK(invoke("UnregisterClassA", unregister_atom, 2u) == 0u,
          "unregister used atom");
    const uint32_t unregister_missing[2] = { guest(0x2060u), 0u };
    put_string(0x2060u, "Missing");
    CHECK(invoke("UnregisterClassA", unregister_missing, 2u) == 0u,
          "unregister missing class");

    printf("window facts OK (%d handlers)\n", installed);
    return 0;
}
