/* gdi_dc_test.c - T0 contract test for target GDI32 DC state. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "gdi_dc_hle.h"
#include "legacy_shim.h"

#define MAX_ROWS 24u
#define CLR_INVALID 0xFFFFFFFFu
#define GDI_ERROR 0xFFFFFFFFu

typedef struct Row {
    const char *dll;
    const char *name;
    int argc;
    void (*impl)(CPU *c);
} Row;

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;
static Row g_overrides[MAX_ROWS];
static unsigned g_override_count;
static uint32_t g_args[2];

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    (void)cdecl_abi;
    if (g_row_count >= MAX_ROWS) return -1;
    g_rows[g_row_count].dll = dll;
    g_rows[g_row_count].name = name;
    g_rows[g_row_count].argc = argc;
    g_rows[g_row_count].impl = impl;
    g_row_count++;
    return 0;
}

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    (void)dll;
    if (g_override_count >= MAX_ROWS) return -1;
    g_overrides[g_override_count].dll = dll;
    g_overrides[g_override_count].name = name;
    g_overrides[g_override_count].argc = 0;
    g_overrides[g_override_count].impl = impl;
    g_override_count++;
    return 0;
}

uint32_t wc3_legacy_arg32(const CPU *c, unsigned index)
{
    (void)c;
    return index < 2u ? g_args[index] : 0;
}

static void (*find_impl(const char *name, int *argc))(CPU *c)
{
    for (unsigned i = 0; i < g_row_count; i++) {
        if (strcmp(g_rows[i].name, name) == 0) {
            if (argc) *argc = g_rows[i].argc;
            return g_rows[i].impl;
        }
    }
    for (unsigned i = 0; i < g_override_count; i++) {
        if (strcmp(g_overrides[i].name, name) == 0) {
            if (argc) *argc = g_overrides[i].argc;
            return g_overrides[i].impl;
        }
    }
    return NULL;
}

static uint32_t invoke(const char *name, uint32_t a0, uint32_t a1)
{
    void (*impl)(CPU *c) = find_impl(name, NULL);
    if (!impl) return 0xFFFFFFFEu;
    g_args[0] = a0;
    g_args[1] = a1;
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
    g_override_count = 0;
    const int installed = wc3_hle_gdi32_dc_install();
    CHECK(installed == 10, "install count");
    CHECK(g_override_count == 2u, "override count");
    CHECK(g_row_count == 8u, "addition count");

    struct {
        const char *name;
        int argc;
    } expected[] = {
        { "SetBkColor", 2 },
        { "GetBkColor", 1 },
        { "GetBkMode", 1 },
        { "GetTextColor", 1 },
        { "SetTextAlign", 2 },
        { "GetTextAlign", 1 },
        { "CreateCompatibleDC", 1 },
        { "DeleteDC", 1 },
    };
    for (unsigned i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        int argc = -1;
        CHECK(find_impl(expected[i].name, &argc) != NULL, expected[i].name);
        CHECK(argc == expected[i].argc, "argc");
    }

    const uint32_t hdc = invoke("CreateCompatibleDC", 0, 0);
    CHECK(hdc != 0u, "create DC");
    CHECK(invoke("GetBkColor", hdc, 0) == 0x00FFFFFFu, "default bk color");
    CHECK(invoke("GetTextColor", hdc, 0) == 0u, "default text color");
    CHECK(invoke("GetBkMode", hdc, 0) == 2u, "default bk mode");
    CHECK(invoke("GetTextAlign", hdc, 0) == 0u, "default text align");

    CHECK(invoke("SetBkColor", hdc, 0x00112233u) == 0x00FFFFFFu,
          "set bk color previous");
    CHECK(invoke("GetBkColor", hdc, 0) == 0x00112233u, "get bk color");
    CHECK(invoke("SetBkColor", hdc, 0x00445566u) == 0x00112233u,
          "set bk color again");
    CHECK(invoke("SetTextColor", hdc, 0x00010203u) == 0u,
          "set text color previous");
    CHECK(invoke("GetTextColor", hdc, 0) == 0x00010203u, "get text color");
    CHECK(invoke("SetBkMode", hdc, 1) == 2u, "set bk mode previous");
    CHECK(invoke("GetBkMode", hdc, 0) == 1u, "get bk mode");
    CHECK(invoke("SetTextAlign", hdc, 6u) == 0u, "set text align previous");
    CHECK(invoke("GetTextAlign", hdc, 0) == 6u, "get text align");
    CHECK(invoke("SetTextAlign", hdc, 1u) == 6u, "set text align again");

    CHECK(invoke("GetBkColor", 0, 0) == CLR_INVALID, "bk color null DC");
    CHECK(invoke("GetTextColor", 0, 0) == CLR_INVALID, "text color null DC");
    CHECK(invoke("GetBkMode", 0, 0) == 0u, "bk mode null DC");
    CHECK(invoke("GetTextAlign", 0, 0) == GDI_ERROR, "text align null DC");

    CHECK(invoke("DeleteDC", hdc, 0) == 1u, "delete DC");
    CHECK(invoke("GetBkColor", hdc, 0) == CLR_INVALID, "get after delete");
    CHECK(invoke("DeleteDC", hdc, 0) == 0u, "delete twice");

    const uint32_t foreign = 0x00030000u;
    CHECK(invoke("SetBkColor", foreign, 0x0000FF00u) == 0x00FFFFFFu,
          "foreign DC initial");
    CHECK(invoke("GetBkColor", foreign, 0) == 0x0000FF00u, "foreign DC state");
    CHECK(invoke("DeleteDC", foreign, 0) == 0u, "delete foreign DC");
    CHECK(invoke("GetBkColor", foreign, 0) == 0x0000FF00u,
          "foreign DC survives delete");

    printf("gdi dc OK (%d handlers)\n", installed);
    return 0;
}
