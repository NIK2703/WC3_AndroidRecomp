/*
 * user32_window_facts_test.c - T0 contract test for target window facts.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "hle_bridge.h"
#include "platform.h"
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
static unsigned g_old_register_a_calls;
static unsigned g_old_create_calls;
static unsigned g_old_destroy_calls;
static unsigned g_old_show_calls;
static unsigned g_old_setpos_calls;
static unsigned g_old_settext_calls;
static uint32_t g_last_error;

static Wc3Platform g_platform;

static uintptr_t fake_thread_self(void *ctx)
{
    (void)ctx;
    return 1u;
}

static uint64_t fake_thread_id(void *ctx, Wc3ThreadHandle thread)
{
    (void)ctx;
    (void)thread;
    return 4242u;
}

const Wc3Platform *wc3_platform(void)
{
    return &g_platform;
}

static void old_register_class(CPU *c)
{
    g_old_register_calls++;
    c->eax = g_next_atom++;
}

static void old_register_class_a(CPU *c)
{
    g_old_register_a_calls++;
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

static void old_set_window_text(CPU *c)
{
    g_old_settext_calls++;
    c->eax = 1u;
}

static void (*old_for(const char *name))(CPU *c)
{
    if (strcmp(name, "RegisterClassExA") == 0) return old_register_class;
    if (strcmp(name, "RegisterClassA") == 0) return old_register_class_a;
    if (strcmp(name, "CreateWindowExA") == 0) return old_create_window;
    if (strcmp(name, "DestroyWindow") == 0) return old_destroy_window;
    if (strcmp(name, "ShowWindow") == 0) return old_show_window;
    if (strcmp(name, "SetWindowPos") == 0) return old_set_window_pos;
    if (strcmp(name, "SetWindowTextA") == 0) return old_set_window_text;
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

static void put_wndclassex(uint32_t offset, const char *class_name,
                           uint32_t name_offset)
{
    memset(g_memory + offset, 0, 48u);
    put_string(name_offset, class_name);
    put_u32(offset + 4u, 0x00CF0000u);
    put_u32(offset + 8u, 0x12345678u);
    put_u32(offset + 12u, 4u);
    put_u32(offset + 16u, 8u);
    put_u32(offset + 20u, 0x00400000u);
    put_u32(offset + 24u, 0x11111111u);
    put_u32(offset + 28u, 0x22222222u);
    put_u32(offset + 32u, 0x33333333u);
    put_u32(offset + 36u, guest(0x2400u));
    put_u32(offset + 40u, guest(name_offset));
    put_u32(offset + 44u, 0x44444444u);
}

static void put_wndclassa(uint32_t offset, const char *class_name,
                          uint32_t name_offset)
{
    memset(g_memory + offset, 0, 40u);
    put_string(name_offset, class_name);
    put_u32(offset + 0u, 0x10CF0000u);
    put_u32(offset + 4u, 0x35001234u);
    put_u32(offset + 8u, 2u);
    put_u32(offset + 12u, 6u);
    put_u32(offset + 16u, 0x35000000u);
    put_u32(offset + 20u, 0xAAAA0001u);
    put_u32(offset + 24u, 0xAAAA0002u);
    put_u32(offset + 28u, 0xAAAA0003u);
    put_u32(offset + 32u, guest(0x2500u));
    put_u32(offset + 36u, guest(name_offset));
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
    memset(&g_platform, 0, sizeof g_platform);
    g_platform.thread.self = fake_thread_self;
    g_platform.thread.id = fake_thread_id;

    const int installed = wc3_hle_user32_window_facts_install();
    CHECK(installed == 27, "install count");
    CHECK(g_chain_count == 7u, "chain count");
    CHECK(g_addition_count == 20u, "addition count");

    const char *chain_names[] = {
        "RegisterClassExA", "RegisterClassA", "CreateWindowExA",
        "DestroyWindow", "ShowWindow", "SetWindowPos", "SetWindowTextA",
    };
    for (unsigned i = 0; i < sizeof chain_names / sizeof chain_names[0]; i++)
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
        { "FindWindowExA", 4 },
        { "GetClassInfoA", 3 },
        { "GetWindowThreadProcessId", 2 },
        { "GetUpdateRgn", 3 },
        { "SetWindowPlacement", 2 },
        { "GetDlgCtrlID", 1 },
        { "GetDlgItem", 2 },
        { "GetDlgItemTextA", 4 },
        { "SetDlgItemTextA", 3 },
    };
    for (unsigned i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        int argc = -1;
        CHECK(find_impl(expected[i].name, &argc) != NULL, expected[i].name);
        CHECK(argc == expected[i].argc, "argc");
    }

    put_wndclassex(0x1000u, "Warcraft", 0x2000u);
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
    const uint32_t class_out = guest(0x6100u);
    memset(g_memory + 0x6100u, 0xAA, 40u);
    const uint32_t get_class[3] = { 0u, guest(0x2000u), class_out };
    g_last_error = 99u;
    CHECK(invoke("GetClassInfoA", get_class, 3u) == 1u,
          "get class info by name");
    CHECK(get_u32(0x6100u) == 0x00CF0000u, "class style");
    CHECK(get_u32(0x6104u) == 0x12345678u, "class wndproc");
    CHECK(get_u32(0x6108u) == 4u && get_u32(0x610Cu) == 8u,
          "class extra bytes");
    CHECK(get_u32(0x6110u) == 0x00400000u, "class instance");
    CHECK(get_u32(0x6114u) == 0x11111111u, "class icon");
    CHECK(get_u32(0x6118u) == 0x22222222u, "class cursor");
    CHECK(get_u32(0x611Cu) == 0x33333333u, "class background");
    CHECK(get_u32(0x6120u) == guest(0x2400u), "class menu name");
    CHECK(get_u32(0x6124u) == guest(0x2000u), "class name pointer");
    CHECK(g_last_error == 99u, "class info keeps error");
    const uint32_t get_class_atom[3] = { 0u, 0xC001u, class_out };
    CHECK(invoke("GetClassInfoA", get_class_atom, 3u) == 1u,
          "get class info by atom");
    CHECK(get_u32(0x6124u) == guest(0x2000u),
          "atom class name pointer");
    const uint32_t get_class_wrong_instance[3] = {
        0x00400001u, guest(0x2000u), class_out,
    };
    g_last_error = 0u;
    CHECK(invoke("GetClassInfoA", get_class_wrong_instance, 3u) == 0u,
          "class instance mismatch");
    CHECK(g_last_error == 1411u, "class instance error");
    put_string(0x2300u, "Missing");
    const uint32_t get_class_missing[3] = {
        0u, guest(0x2300u), class_out,
    };
    g_last_error = 0u;
    CHECK(invoke("GetClassInfoA", get_class_missing, 3u) == 0u,
          "missing class");
    CHECK(g_last_error == 1411u, "missing class error");
    const uint32_t get_class_null_out[3] = { 0u, guest(0x2000u), 0u };
    g_last_error = 0u;
    CHECK(invoke("GetClassInfoA", get_class_null_out, 3u) == 0u,
          "class null output");
    CHECK(g_last_error == 87u, "class null output error");
    const uint32_t get_class_null_name[3] = { 0u, 0u, class_out };
    g_last_error = 0u;
    CHECK(invoke("GetClassInfoA", get_class_null_name, 3u) == 0u,
          "class null name");
    CHECK(g_last_error == 87u, "class null name error");

    put_string(0x2310u, "Static");
    const uint32_t get_static[3] = {
        0x00400000u, guest(0x2310u), class_out,
    };
    memset(g_memory + 0x6100u, 0xAA, 40u);
    g_last_error = 99u;
    CHECK(invoke("GetClassInfoA", get_static, 3u) == 1u,
          "get system static class");
    CHECK(get_u32(0x6100u) == 0x88u, "static class style");
    CHECK(get_u32(0x6104u) == 0u, "static class wndproc");
    CHECK(get_u32(0x6108u) == 0u, "static class cls extra");
    CHECK(get_u32(0x610Cu) == 8u, "static class wnd extra");
    CHECK(get_u32(0x6110u) == 0x00400000u, "static class instance");
    CHECK(get_u32(0x6114u) == 0u, "static class icon");
    CHECK(get_u32(0x6118u) == 0u, "static class cursor");
    CHECK(get_u32(0x611Cu) == 0u, "static class background");
    CHECK(get_u32(0x6120u) == 0u, "static class menu");
    CHECK(get_u32(0x6124u) == guest(0x2310u), "static class name pointer");
    CHECK(g_last_error == 99u, "static class keeps error");
    const uint32_t get_static_null[3] = { 0u, guest(0x2310u), class_out };
    CHECK(invoke("GetClassInfoA", get_static_null, 3u) == 1u,
          "get system static with null instance");
    CHECK(get_u32(0x6110u) == 0u, "static null instance");
    put_string(0x2320u, "Button");
    const uint32_t get_button[3] = {
        0x12345678u, guest(0x2320u), class_out,
    };
    CHECK(invoke("GetClassInfoA", get_button, 3u) == 1u,
          "get system button class");
    CHECK(get_u32(0x6100u) == 0x8Bu, "button class style");
    CHECK(get_u32(0x610Cu) == 8u, "button class wnd extra");
    CHECK(get_u32(0x6110u) == 0x12345678u, "button class instance");
    CHECK(get_u32(0x6124u) == guest(0x2320u), "button class name pointer");

    const uint32_t is_main[1] = { main_handle };
    CHECK(invoke("IsWindow", is_main, 1u) == 1u, "main is window");
    const uint32_t is_unknown[1] = { 0x000200FFu };
    CHECK(invoke("IsWindow", is_unknown, 1u) == 0u, "unknown is not window");
    const uint32_t is_desktop[1] = { desktop };
    CHECK(invoke("IsWindow", is_desktop, 1u) == 1u, "desktop is window");
    const uint32_t is_zero[1] = { 0u };
    CHECK(invoke("IsWindow", is_zero, 1u) == 0u, "zero is not window");

    put_u32(0x6300u, 0xFFFFFFFFu);
    const uint32_t get_tid[2] = { main_handle, guest(0x6300u) };
    g_last_error = 99u;
    CHECK(invoke("GetWindowThreadProcessId", get_tid, 2u) == 4242u,
          "main thread id");
    CHECK(get_u32(0x6300u) == 4242u, "main process id");
    CHECK(g_last_error == 99u, "thread id keeps error");
    const uint32_t get_tid_null[2] = { main_handle, 0u };
    CHECK(invoke("GetWindowThreadProcessId", get_tid_null, 2u) == 4242u,
          "thread id without process pointer");
    const uint32_t get_tid_desktop[2] = { desktop, guest(0x6300u) };
    CHECK(invoke("GetWindowThreadProcessId", get_tid_desktop, 2u) == 4242u,
          "desktop thread id");
    CHECK(get_u32(0x6300u) == 4242u, "desktop process id");
    put_u32(0x6300u, 0x12345678u);
    const uint32_t get_tid_unknown[2] = { 0x000200FFu, guest(0x6300u) };
    g_last_error = 0u;
    CHECK(invoke("GetWindowThreadProcessId", get_tid_unknown, 2u) == 0u,
          "unknown thread id");
    CHECK(get_u32(0x6300u) == 0x12345678u,
          "unknown leaves process id");
    CHECK(g_last_error == 1400u, "unknown thread id error");

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

    put_u32(0x6400u, 0xDEADBEEFu);
    const uint32_t get_update_rgn[3] = {
        main_handle, guest(0x6400u), 1u,
    };
    g_last_error = 99u;
    CHECK(invoke("GetUpdateRgn", get_update_rgn, 3u) == 1u,
          "empty update region");
    CHECK(get_u32(0x6400u) == 0xDEADBEEFu,
          "update region unchanged");
    CHECK(g_last_error == 99u, "update region keeps error");
    const uint32_t get_update_desktop[3] = {
        desktop, guest(0x6400u), 0u,
    };
    CHECK(invoke("GetUpdateRgn", get_update_desktop, 3u) == 1u,
          "desktop update region");
    const uint32_t get_update_null[3] = { main_handle, 0u, 0u };
    g_last_error = 0u;
    CHECK(invoke("GetUpdateRgn", get_update_null, 3u) == 0u,
          "null update region");
    CHECK(g_last_error == 87u, "null update region error");
    const uint32_t get_update_unknown[3] = {
        0x000200FFu, guest(0x6400u), 0u,
    };
    g_last_error = 0u;
    CHECK(invoke("GetUpdateRgn", get_update_unknown, 3u) == 0u,
          "unknown update region");
    CHECK(g_last_error == 1400u, "unknown update region error");

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

    put_wndclassex(0x1100u, "Child", 0x2100u);
    const uint32_t register_child[1] = { guest(0x1100u) };
    CHECK(invoke("RegisterClassExA", register_child, 1u) == 0xC002u,
          "register child class");

    put_wndclassa(0x1200u, "Storm", 0x2200u);
    const uint32_t register_a[1] = { guest(0x1200u) };
    CHECK(invoke("RegisterClassA", register_a, 1u) == 0xC003u,
          "register A class");
    CHECK(g_old_register_a_calls == 1u, "previous register A called");
    const uint32_t get_class_a[3] = {
        0x35000000u, guest(0x2200u), class_out,
    };
    CHECK(invoke("GetClassInfoA", get_class_a, 3u) == 1u,
          "get A class info");
    CHECK(get_u32(0x6100u) == 0x10CF0000u, "A class style");
    CHECK(get_u32(0x6104u) == 0x35001234u, "A class wndproc");
    CHECK(get_u32(0x6108u) == 2u && get_u32(0x610Cu) == 6u,
          "A class extra bytes");
    CHECK(get_u32(0x6110u) == 0x35000000u, "A class instance");
    CHECK(get_u32(0x6114u) == 0xAAAA0001u, "A class icon");
    CHECK(get_u32(0x6118u) == 0xAAAA0002u, "A class cursor");
    CHECK(get_u32(0x611Cu) == 0xAAAA0003u, "A class background");
    CHECK(get_u32(0x6120u) == guest(0x2500u), "A class menu name");
    CHECK(get_u32(0x6124u) == guest(0x2200u), "A class name pointer");
    const uint32_t get_class_a_atom[3] = { 0u, 0xC003u, class_out };
    CHECK(invoke("GetClassInfoA", get_class_a_atom, 3u) == 1u,
          "get A class by atom");
    put_string(0x2210u, "storm");
    const uint32_t unregister_a[2] = { guest(0x2210u), 0x35000000u };
    CHECK(invoke("UnregisterClassA", unregister_a, 2u) == 1u,
          "unregister A class case-insensitively");
    g_last_error = 0u;
    CHECK(invoke("GetClassInfoA", get_class_a, 3u) == 0u,
          "unregistered A class");
    CHECK(g_last_error == 1411u, "unregistered A class error");

    put_string(0x2020u, "Child");
    const uint32_t create_child[12] = {
        0u, guest(0x2020u), guest(0x2030u), 0x40000000u,
        0u, 0u, 320u, 200u, main_handle, 100u, 0u, 0u,
    };
    put_string(0x2030u, "Child window");
    CHECK(invoke("CreateWindowExA", create_child, 12u) == 0x00020008u,
          "create child window");
    const uint32_t child_handle = 0x00020008u;
    CHECK(invoke("GetTopWindow", top_main, 1u) == child_handle,
          "top child");
    CHECK(invoke("GetTopWindow", top_null, 1u) == main_handle,
          "top-level ignores child");

    const uint32_t get_ctrl_id[1] = { child_handle };
    CHECK(invoke("GetDlgCtrlID", get_ctrl_id, 1u) == 100u,
          "child control id");
    const uint32_t get_ctrl_id_main[1] = { main_handle };
    CHECK(invoke("GetDlgCtrlID", get_ctrl_id_main, 1u) == 0u,
          "top-level control id");
    const uint32_t get_item[2] = { main_handle, 100u };
    g_last_error = 99u;
    CHECK(invoke("GetDlgItem", get_item, 2u) == child_handle,
          "get dialog item");
    CHECK(g_last_error == 99u, "get dialog item keeps error");
    const uint32_t get_item_missing[2] = { main_handle, 101u };
    g_last_error = 0u;
    CHECK(invoke("GetDlgItem", get_item_missing, 2u) == 0u,
          "missing dialog item");
    CHECK(g_last_error == 1421u, "missing dialog item error");
    const uint32_t get_item_desktop[2] = { desktop, 100u };
    g_last_error = 0u;
    CHECK(invoke("GetDlgItem", get_item_desktop, 2u) == 0u,
          "desktop dialog item");
    CHECK(g_last_error == 1421u, "desktop dialog item error");
    const uint32_t get_item_null[2] = { 0u, 100u };
    g_last_error = 0u;
    CHECK(invoke("GetDlgItem", get_item_null, 2u) == 0u,
          "null dialog parent");
    CHECK(g_last_error == 1400u, "null dialog parent error");
    const uint32_t get_item_unknown[2] = { 0x000200FFu, 100u };
    g_last_error = 0u;
    CHECK(invoke("GetDlgItem", get_item_unknown, 2u) == 0u,
          "unknown dialog parent");
    CHECK(g_last_error == 1400u, "unknown dialog parent error");

    const uint32_t dlg_text_out = guest(0x6500u);
    const uint32_t get_item_text[4] = {
        main_handle, 100u, dlg_text_out, 64u,
    };
    memset(g_memory + 0x6500u, 0xAA, 64u);
    g_last_error = 99u;
    CHECK(invoke("GetDlgItemTextA", get_item_text, 4u) == 12u,
          "dialog item text length");
    CHECK(strcmp((char *)(g_memory + 0x6500u), "Child window") == 0,
          "dialog item text");
    CHECK(g_last_error == 99u, "dialog item text keeps error");

    put_string(0x2090u, "Edited child");
    const uint32_t set_item_text[3] = {
        main_handle, 100u, guest(0x2090u),
    };
    g_last_error = 99u;
    CHECK(invoke("SetDlgItemTextA", set_item_text, 3u) == 1u,
          "set dialog item text");
    CHECK(g_last_error == 99u, "set dialog item text keeps error");
    memset(g_memory + 0x6500u, 0xAA, 64u);
    CHECK(invoke("GetDlgItemTextA", get_item_text, 4u) == 12u,
          "edited item text length");
    CHECK(strcmp((char *)(g_memory + 0x6500u), "Edited child") == 0,
          "edited item text");

    const uint32_t get_item_short[4] = {
        main_handle, 100u, dlg_text_out, 5u,
    };
    memset(g_memory + 0x6500u, 0xAA, 64u);
    g_last_error = 99u;
    CHECK(invoke("GetDlgItemTextA", get_item_short, 4u) == 4u,
          "short item text length");
    CHECK(strcmp((char *)(g_memory + 0x6500u), "Edit") == 0,
          "short item text");
    CHECK(g_last_error == 99u, "short item text keeps error");

    const uint32_t get_item_missing_text[4] = {
        main_handle, 101u, dlg_text_out, 64u,
    };
    g_last_error = 0u;
    CHECK(invoke("GetDlgItemTextA", get_item_missing_text, 4u) == 0u,
          "missing item text");
    CHECK(g_last_error == 1421u, "missing item text error");

    const uint32_t get_item_bad_parent[4] = {
        0x000200FFu, 100u, dlg_text_out, 64u,
    };
    g_last_error = 0u;
    CHECK(invoke("GetDlgItemTextA", get_item_bad_parent, 4u) == 0u,
          "bad item parent");
    CHECK(g_last_error == 1400u, "bad item parent error");

    const uint32_t get_item_null_buffer[4] = {
        main_handle, 100u, 0u, 64u,
    };
    g_last_error = 99u;
    CHECK(invoke("GetDlgItemTextA", get_item_null_buffer, 4u) == 0u,
          "null item buffer");
    CHECK(g_last_error == 99u, "null item buffer keeps error");

    const uint32_t get_item_zero_capacity[4] = {
        main_handle, 100u, dlg_text_out, 0u,
    };
    g_last_error = 99u;
    CHECK(invoke("GetDlgItemTextA", get_item_zero_capacity, 4u) == 0u,
          "zero item capacity");
    CHECK(g_last_error == 99u, "zero item capacity keeps error");

    const uint32_t set_item_missing[3] = {
        main_handle, 101u, guest(0x2090u),
    };
    g_last_error = 0u;
    CHECK(invoke("SetDlgItemTextA", set_item_missing, 3u) == 0u,
          "set missing item text");
    CHECK(g_last_error == 1421u, "set missing item text error");

    const uint32_t set_item_bad_parent[3] = {
        0x000200FFu, 100u, guest(0x2090u),
    };
    g_last_error = 0u;
    CHECK(invoke("SetDlgItemTextA", set_item_bad_parent, 3u) == 0u,
          "set bad item parent");
    CHECK(g_last_error == 1400u, "set bad item parent error");

    const uint32_t set_item_null[3] = { main_handle, 100u, 0u };
    g_last_error = 99u;
    CHECK(invoke("SetDlgItemTextA", set_item_null, 3u) == 1u,
          "clear dialog item text");
    CHECK(g_last_error == 99u, "clear dialog item text keeps error");
    memset(g_memory + 0x6500u, 0xAA, 64u);
    CHECK(invoke("GetDlgItemTextA", get_item_text, 4u) == 0u,
          "cleared item text length");
    CHECK(g_memory[0x6500u] == 0u, "cleared item text");
    const uint32_t restore_item_text[3] = {
        main_handle, 100u, guest(0x2030u),
    };
    CHECK(invoke("SetDlgItemTextA", restore_item_text, 3u) == 1u,
          "restore item text");

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

    const uint32_t find_main_title[4] = {
        0u, 0u, 0u, guest(0x2010u),
    };
    CHECK(invoke("FindWindowExA", find_main_title, 4u) == main_handle,
          "find main by title");
    const uint32_t find_class[4] = { 0u, 0u, guest(0x2000u), 0u };
    CHECK(invoke("FindWindowExA", find_class, 4u) == atom_handle,
          "find top-level by class");
    const uint32_t find_atom[4] = { 0u, 0u, 0xC001u, 0u };
    CHECK(invoke("FindWindowExA", find_atom, 4u) == atom_handle,
          "find top-level by class atom");
    const uint32_t find_child[4] = { main_handle, 0u, 0u, 0u };
    CHECK(invoke("FindWindowExA", find_child, 4u) == child_handle,
          "find child");
    const uint32_t find_child_title[4] = {
        main_handle, 0u, 0u, guest(0x2030u),
    };
    CHECK(invoke("FindWindowExA", find_child_title, 4u) == child_handle,
          "find child by title");
    const uint32_t find_after_atom[4] = { 0u, atom_handle, 0u, 0u };
    CHECK(invoke("FindWindowExA", find_after_atom, 4u) == main_handle,
          "find window below topmost");
    const uint32_t find_after_main[4] = { 0u, main_handle, 0u, 0u };
    CHECK(invoke("FindWindowExA", find_after_main, 4u) == 0u,
          "no window below main");
    const uint32_t find_unknown_parent[4] = {
        0x000200FFu, 0u, 0u, 0u,
    };
    CHECK(invoke("FindWindowExA", find_unknown_parent, 4u) == 0u,
          "find unknown parent");
    const uint32_t find_missing_class[4] = {
        0u, 0u, guest(0x2300u), 0u,
    };
    CHECK(invoke("FindWindowExA", find_missing_class, 4u) == 0u,
          "find missing class");
    put_string(0x2070u, "warcraft iii");
    const uint32_t find_ci_title[4] = { 0u, 0u, 0u, guest(0x2070u) };
    CHECK(invoke("FindWindowExA", find_ci_title, 4u) == main_handle,
          "find title case-insensitively");

    put_string(0x2080u, "Renamed");
    const uint32_t set_text[2] = { main_handle, guest(0x2080u) };
    CHECK(invoke("SetWindowTextA", set_text, 2u) == 1u,
          "set window text");
    CHECK(g_old_settext_calls == 1u, "previous set text called");
    const uint32_t find_renamed[4] = { 0u, 0u, 0u, guest(0x2080u) };
    CHECK(invoke("FindWindowExA", find_renamed, 4u) == main_handle,
          "find renamed window");

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

    memset(g_memory + 0x4100u, 0, 44u);
    put_u32(0x4100u, 44u);
    put_u32(0x4104u, 1u);
    put_u32(0x4108u, 3u);
    put_u32(0x410Cu, (uint32_t)-10);
    put_u32(0x4110u, (uint32_t)-20);
    put_u32(0x4114u, 5u);
    put_u32(0x4118u, 6u);
    put_u32(0x411Cu, 30u);
    put_u32(0x4120u, 40u);
    put_u32(0x4124u, 830u);
    put_u32(0x4128u, 640u);
    const uint32_t set_placement[2] = {
        main_handle, guest(0x4100u),
    };
    g_last_error = 99u;
    CHECK(invoke("SetWindowPlacement", set_placement, 2u) == 1u,
          "set placement");
    CHECK(g_last_error == 99u, "set placement keeps error");
    CHECK(invoke("GetWindowPlacement", get_placement, 2u) == 1u,
          "get set placement");
    CHECK(get_u32(0x4004u) == 1u, "placement flags");
    CHECK(get_u32(0x4008u) == 3u, "placement show maximized");
    CHECK((int32_t)get_u32(0x400Cu) == -10, "placement min x");
    CHECK((int32_t)get_u32(0x4010u) == -20, "placement min y");
    CHECK(get_u32(0x4014u) == 5u && get_u32(0x4018u) == 6u,
          "placement max position");
    CHECK(get_u32(0x401Cu) == 30u && get_u32(0x4020u) == 40u,
          "placement normal origin");
    CHECK(get_u32(0x4024u) == 830u && get_u32(0x4028u) == 640u,
          "placement normal size");
    CHECK(invoke("IsIconic", is_main, 1u) == 0u,
          "maximized is not iconic");

    put_u32(0x4108u, 2u);
    CHECK(invoke("SetWindowPlacement", set_placement, 2u) == 1u,
          "set minimized placement");
    CHECK(invoke("IsIconic", is_main, 1u) == 1u,
          "placement minimized");
    CHECK(invoke("ShowWindow", show_restore, 2u) == 1u,
          "restore placement minimized");
    CHECK(invoke("IsIconic", is_main, 1u) == 0u,
          "restored placement not iconic");

    put_u32(0x4100u, 43u);
    g_last_error = 0u;
    CHECK(invoke("SetWindowPlacement", set_placement, 2u) == 0u,
          "bad placement size");
    CHECK(g_last_error == 87u, "bad placement size error");
    put_u32(0x4100u, 44u);
    const uint32_t set_placement_null[2] = { main_handle, 0u };
    g_last_error = 0u;
    CHECK(invoke("SetWindowPlacement", set_placement_null, 2u) == 0u,
          "null placement");
    CHECK(g_last_error == 87u, "null placement error");
    const uint32_t set_placement_unknown[2] = {
        0x000200FFu, guest(0x4100u),
    };
    g_last_error = 0u;
    CHECK(invoke("SetWindowPlacement", set_placement_unknown, 2u) == 0u,
          "unknown set placement");
    CHECK(g_last_error == 1400u, "unknown set placement error");
    const uint32_t set_placement_desktop[2] = {
        desktop, guest(0x4100u),
    };
    g_last_error = 0u;
    CHECK(invoke("SetWindowPlacement", set_placement_desktop, 2u) == 0u,
          "desktop set placement");
    CHECK(g_last_error == 1400u, "desktop set placement error");

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
