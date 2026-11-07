/*
 * user32_window_test.c - T0 contract test for the USER32 window/display subset.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "window_hle.h"

#define MAX_ROWS 8u

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
} Row;

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;
static uint32_t g_arg0;
static int g_mode_ok;
static uint32_t g_mode_w;
static uint32_t g_mode_h;

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    if (!dll || !name || !impl) return -1;
    if (g_row_count >= MAX_ROWS) return -1;
    g_rows[g_row_count].dll = dll;
    g_rows[g_row_count].name = name;
    g_rows[g_row_count].impl = impl;
    g_row_count++;
    return 0;
}

uint32_t wc3_legacy_arg32(const CPU *c, unsigned index)
{
    (void)c;
    return index == 0u ? g_arg0 : 0u;
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

static uint32_t invoke(const char *name, uint32_t arg)
{
    void (*impl)(CPU *c) = find_impl(name);
    if (!impl) {
        printf("missing handler: %s\n", name);
        return 0xFFFFFFFFu;
    }
    g_arg0 = arg;
    CPU c;
    memset(&c, 0, sizeof c);
    impl(&c);
    return c.eax;
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
    CHECK(installed == 3, "install count");
    CHECK(g_row_count == 3u, "row count");
    CHECK(find_impl("GetDesktopWindow") != NULL, "GetDesktopWindow row");
    CHECK(find_impl("GetSystemMetrics") != NULL, "GetSystemMetrics row");
    CHECK(find_impl("ShowCursor") != NULL, "ShowCursor row");
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

    printf("user32 window OK (%d handlers)\n", installed);
    return 0;
}
