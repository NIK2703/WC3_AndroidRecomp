/*
 * user32_window_test.c - T0 contract test for the USER32 window/display subset.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "window_hle.h"

#define MAX_ROWS 16u
#define GUEST_BASE 0x1000u
#define GUEST_SIZE 0x4000u

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
} Row;

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;
static uint32_t g_args[3];
static uint8_t g_guest[GUEST_SIZE];
static int g_mode_ok;
static uint32_t g_mode_w;
static uint32_t g_mode_h;

static int add_row(const char *dll, const char *name, void (*impl)(CPU *c))
{
    if (!dll || !name || !impl) return -1;
    if (g_row_count >= MAX_ROWS) return -1;
    g_rows[g_row_count].dll = dll;
    g_rows[g_row_count].name = name;
    g_rows[g_row_count].impl = impl;
    g_row_count++;
    return 0;
}

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    return add_row(dll, name, impl);
}

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    (void)argc;
    (void)cdecl_abi;
    return add_row(dll, name, impl);
}

uint32_t wc3_legacy_arg32(const CPU *c, unsigned index)
{
    (void)c;
    return index < 3u ? g_args[index] : 0u;
}

int wc3_legacy_guest_ok(uint32_t address, uint32_t size)
{
    if (address < GUEST_BASE) return 0;
    const uint32_t offset = address - GUEST_BASE;
    return size <= GUEST_SIZE && offset <= GUEST_SIZE - size;
}

uint8_t wc3_legacy_rd8g(uint32_t address)
{
    if (!wc3_legacy_guest_ok(address, 1u)) return 0;
    return g_guest[address - GUEST_BASE];
}

uint32_t wc3_legacy_rd32g(uint32_t address)
{
    if (!wc3_legacy_guest_ok(address, 4u)) return 0;
    const uint8_t *p = g_guest + (address - GUEST_BASE);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void wc3_legacy_wr32g(uint32_t address, uint32_t value)
{
    if (!wc3_legacy_guest_ok(address, 4u)) return;
    uint8_t *p = g_guest + (address - GUEST_BASE);
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

int wc3_legacy_current_display_mode(uint32_t *width, uint32_t *height)
{
    if (!g_mode_ok) return 0;
    if (width) *width = g_mode_w;
    if (height) *height = g_mode_h;
    return 1;
}

static void (*find_impl(const char *name))(CPU *c)
{
    for (unsigned i = 0; i < g_row_count; i++)
        if (strcmp(g_rows[i].name, name) == 0) return g_rows[i].impl;
    return NULL;
}

static uint32_t invoke3(const char *name, uint32_t arg0, uint32_t arg1,
                        uint32_t arg2)
{
    void (*impl)(CPU *c) = find_impl(name);
    if (!impl) {
        printf("missing handler: %s\n", name);
        return 0xFFFFFFFFu;
    }
    g_args[0] = arg0;
    g_args[1] = arg1;
    g_args[2] = arg2;
    CPU c;
    memset(&c, 0, sizeof c);
    impl(&c);
    return c.eax;
}

static uint32_t invoke(const char *name, uint32_t arg)
{
    return invoke3(name, arg, 0u, 0u);
}

static void put_name(uint32_t address, const char *text)
{
    if (address < GUEST_BASE) return;
    const size_t length = strlen(text) + 1u;
    if (length > GUEST_SIZE || address - GUEST_BASE > GUEST_SIZE - length)
        return;
    memcpy(g_guest + (address - GUEST_BASE), text, length);
}

static void put_rect(uint32_t address, int32_t left, int32_t top,
                     int32_t right, int32_t bottom)
{
    wc3_legacy_wr32g(address, (uint32_t)left);
    wc3_legacy_wr32g(address + 4u, (uint32_t)top);
    wc3_legacy_wr32g(address + 8u, (uint32_t)right);
    wc3_legacy_wr32g(address + 12u, (uint32_t)bottom);
}

static int32_t rect_field(uint32_t address)
{
    return (int32_t)wc3_legacy_rd32g(address);
}

static int32_t as_i32(uint32_t v) { return (int32_t)v; }

#define CHECK(condition, message)                                         \
    do {                                                                  \
        if (!(condition)) {                                               \
            printf("FAIL: %s\n", message);                                \
            return 1;                                                     \
        }                                                                 \
    } while (0)

int main(void)
{
    const int installed = wc3_hle_user32_window_install();
    CHECK(installed == 7, "install count");
    CHECK(g_row_count == 7u, "row count");
    CHECK(find_impl("GetDesktopWindow") != NULL, "GetDesktopWindow row");
    CHECK(find_impl("GetSystemMetrics") != NULL, "GetSystemMetrics row");
    CHECK(find_impl("ShowCursor") != NULL, "ShowCursor row");
    CHECK(find_impl("GetPropA") != NULL, "GetPropA row");
    CHECK(find_impl("SetPropA") != NULL, "SetPropA row");
    CHECK(find_impl("RemovePropA") != NULL, "RemovePropA row");
    CHECK(find_impl("IntersectRect") != NULL, "IntersectRect row");
    CHECK(strcmp(g_rows[0].dll, "USER32.dll") == 0, "DLL name");

    CHECK(invoke("GetDesktopWindow", 0) == 0x00010000u,
          "GetDesktopWindow handle");

    g_mode_ok = 1;
    g_mode_w = 1024;
    g_mode_h = 768;
    CHECK(invoke("GetSystemMetrics", 0) == 1024u, "SM_CXSCREEN");
    CHECK(invoke("GetSystemMetrics", 61) == 1024u, "SM_CXMAXIMIZED");
    CHECK(invoke("GetSystemMetrics", 78) == 1024u, "SM_CXVIRTUALSCREEN");
    CHECK(invoke("GetSystemMetrics", 1) == 768u, "SM_CYSCREEN");
    CHECK(invoke("GetSystemMetrics", 62) == 768u, "SM_CYMAXIMIZED");
    CHECK(invoke("GetSystemMetrics", 79) == 768u, "SM_CYVIRTUALSCREEN");
    CHECK(invoke("GetSystemMetrics", 80) == 1u, "SM_CMONITORS");
    CHECK(invoke("GetSystemMetrics", 5) == 800u, "unrelated metric");

    g_mode_ok = 0;
    CHECK(invoke("GetSystemMetrics", 0) == 800u, "fallback width");
    CHECK(invoke("GetSystemMetrics", 1) == 600u, "fallback height");

    CHECK(as_i32(invoke("ShowCursor", 1)) == 1, "ShowCursor first show");
    CHECK(as_i32(invoke("ShowCursor", 1)) == 2, "ShowCursor second show");
    CHECK(as_i32(invoke("ShowCursor", 0)) == 1, "ShowCursor first hide");
    CHECK(as_i32(invoke("ShowCursor", 0)) == 0, "ShowCursor second hide");
    CHECK(as_i32(invoke("ShowCursor", 0)) == -1, "ShowCursor third hide");
    CHECK(as_i32(invoke("ShowCursor", 1)) == 0, "ShowCursor show again");

    const uint32_t window = 0x00020004u;
    const uint32_t other_window = 0x00020008u;
    put_name(0x2000u, "OsGuiPointer");
    put_name(0x2040u, "osguipointer");
    put_name(0x2080u, "Other");
    CHECK(invoke3("GetPropA", window, 0x2000u, 0u) == 0u,
          "GetPropA missing");
    CHECK(invoke3("SetPropA", window, 0x2000u, 0x12345678u) == 1u,
          "SetPropA add");
    CHECK(invoke3("GetPropA", window, 0x2040u, 0u) == 0x12345678u,
          "GetPropA case-insensitive");
    CHECK(invoke3("SetPropA", window, 0x2040u, 0xABCDEF01u) == 1u,
          "SetPropA replace");
    CHECK(invoke3("GetPropA", window, 0x2000u, 0u) == 0xABCDEF01u,
          "GetPropA replaced value");
    CHECK(invoke3("GetPropA", other_window, 0x2000u, 0u) == 0u,
          "GetPropA window isolation");
    CHECK(invoke3("SetPropA", 0u, 0x2000u, 1u) == 0u,
          "SetPropA null window");
    CHECK(invoke3("GetPropA", window, 0u, 0u) == 0u,
          "GetPropA null name");
    CHECK(invoke3("RemovePropA", window, 0x2080u, 0u) == 0u,
          "RemovePropA missing");
    CHECK(invoke3("RemovePropA", window, 0x2000u, 0u) == 0xABCDEF01u,
          "RemovePropA value");
    CHECK(invoke3("GetPropA", window, 0x2000u, 0u) == 0u,
          "GetPropA after remove");
    CHECK(invoke3("RemovePropA", window, 0x2000u, 0u) == 0u,
          "RemovePropA twice");

    put_rect(0x3000u, 0, 0, 0, 0);
    put_rect(0x3020u, 10, 20, 110, 220);
    put_rect(0x3040u, 50, 10, 150, 100);
    CHECK(invoke3("IntersectRect", 0x3000u, 0x3020u, 0x3040u) == 1u,
          "IntersectRect overlap");
    CHECK(rect_field(0x3000u) == 50 && rect_field(0x3004u) == 20 &&
              rect_field(0x3008u) == 110 && rect_field(0x300Cu) == 100,
          "IntersectRect result");

    put_rect(0x3000u, 1, 2, 3, 4);
    put_rect(0x3020u, 0, 0, 10, 10);
    put_rect(0x3040u, 20, 20, 30, 30);
    CHECK(invoke3("IntersectRect", 0x3000u, 0x3020u, 0x3040u) == 0u,
          "IntersectRect disjoint");
    CHECK(rect_field(0x3000u) == 0 && rect_field(0x3004u) == 0 &&
              rect_field(0x3008u) == 0 && rect_field(0x300Cu) == 0,
          "IntersectRect empty result");
    CHECK(invoke3("IntersectRect", 0x10u, 0x3020u, 0x3040u) == 0u,
          "IntersectRect invalid destination");

    printf("user32 window OK (%d handlers)\n", installed);
    return 0;
}
