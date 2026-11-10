/*
 * comctl32_imagelist_test.c - T0 contract test for target image-list handles.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "imagelist_hle.h"
#include "hle_bridge.h"

#define MAX_ROWS 16u

typedef struct Row {
    const char *dll;
    const char *name;
    int argc;
    void (*impl)(CPU *c);
} Row;

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;
static uint32_t g_args[5];

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
    (void)name;
    (void)impl;
    return -1;
}

uint32_t wc3_legacy_arg32(const CPU *c, unsigned index)
{
    (void)c;
    return index < 5u ? g_args[index] : 0;
}

static void (*find_impl(const char *name, int *argc))(CPU *c)
{
    for (unsigned i = 0; i < g_row_count; i++) {
        if (strcmp(g_rows[i].name, name) == 0) {
            if (argc) *argc = g_rows[i].argc;
            return g_rows[i].impl;
        }
    }
    return NULL;
}

static uint32_t invoke(const char *name, uint32_t a0, uint32_t a1, uint32_t a2,
                       uint32_t a3, uint32_t a4)
{
    void (*impl)(CPU *c) = find_impl(name, NULL);
    if (!impl) return 0xFFFFFFFEu;
    g_args[0] = a0;
    g_args[1] = a1;
    g_args[2] = a2;
    g_args[3] = a3;
    g_args[4] = a4;
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
    const int installed = wc3_hle_comctl32_imagelist_install();
    CHECK(installed == 11, "install count");
    CHECK(g_row_count == 11u, "row count");

    struct {
        const char *name;
        int argc;
    } expected[] = {
        { "ImageList_Create", 5 },
        { "ImageList_Destroy", 1 },
        { "ImageList_GetImageCount", 1 },
        { "ImageList_Add", 3 },
        { "ImageList_Replace", 4 },
        { "ImageList_BeginDrag", 4 },
        { "ImageList_EndDrag", 0 },
        { "ImageList_DragEnter", 3 },
        { "ImageList_DragLeave", 1 },
        { "ImageList_DragMove", 2 },
        { "ImageList_DragShowNolock", 1 },
    };
    for (unsigned i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        int argc = -1;
        CHECK(find_impl(expected[i].name, &argc) != NULL,
              expected[i].name);
        CHECK(argc == expected[i].argc, "argc");
    }

    const uint32_t first = invoke("ImageList_Create", 16, 16, 1, 4, 4);
    const uint32_t second = invoke("ImageList_Create", 32, 32, 0, 2, 2);
    CHECK(first != 0u, "create first");
    CHECK(second != 0u, "create second");
    CHECK(first != second, "distinct handles");
    CHECK(invoke("ImageList_Create", 0, 16, 1, 1, 1) == 0u,
          "reject zero width");
    CHECK(invoke("ImageList_Create", 16, 0xFFFFFFFFu, 1, 1, 1) == 0u,
          "reject negative height");

    CHECK(invoke("ImageList_GetImageCount", first, 0, 0, 0, 0) == 0u,
          "initial count");
    CHECK(invoke("ImageList_Add", first, 0x100, 0, 0, 0) == 0u,
          "add first");
    CHECK(invoke("ImageList_Add", first, 0x200, 0, 0, 0) == 1u,
          "add second");
    CHECK(invoke("ImageList_GetImageCount", first, 0, 0, 0, 0) == 2u,
          "count after add");
    CHECK(invoke("ImageList_Add", 0u, 0x100, 0, 0, 0) == 0xFFFFFFFFu,
          "add bad handle");

    CHECK(invoke("ImageList_Replace", first, 0, 0x300, 0, 0) == 1u,
          "replace first");
    CHECK(invoke("ImageList_Replace", first, 1, 0x400, 0, 0) == 1u,
          "replace second");
    CHECK(invoke("ImageList_Replace", first, 2, 0x500, 0, 0) == 0u,
          "replace out of range");
    CHECK(invoke("ImageList_Replace", first, 0xFFFFFFFFu, 0x500, 0, 0) == 0u,
          "replace negative");

    CHECK(invoke("ImageList_BeginDrag", first, 1, 0, 0, 0) == 1u,
          "begin drag");
    CHECK(invoke("ImageList_BeginDrag", first, 2, 0, 0, 0) == 0u,
          "begin drag out of range");
    CHECK(invoke("ImageList_BeginDrag", second, 0, 0, 0, 0) == 0u,
          "begin drag empty list");
    CHECK(invoke("ImageList_DragEnter", 0x10000u, 5, 6, 0, 0) == 1u,
          "drag enter");
    CHECK(invoke("ImageList_DragLeave", 0x10000u, 0, 0, 0, 0) == 1u,
          "drag leave");
    CHECK(invoke("ImageList_DragMove", 7, 8, 0, 0, 0) == 1u,
          "drag move");
    CHECK(invoke("ImageList_DragShowNolock", 1, 0, 0, 0, 0) == 1u,
          "drag show");
    CHECK(invoke("ImageList_DragShowNolock", 0, 0, 0, 0, 0) == 1u,
          "drag hide");
    CHECK(invoke("ImageList_EndDrag", 0, 0, 0, 0, 0) == 0u,
          "end drag return");
    CHECK(invoke("ImageList_DragMove", 7, 8, 0, 0, 0) == 0u,
          "drag move after end");

    CHECK(invoke("ImageList_Destroy", first, 0, 0, 0, 0) == 1u,
          "destroy first");
    CHECK(invoke("ImageList_GetImageCount", first, 0, 0, 0, 0) == 0u,
          "count after destroy");
    CHECK(invoke("ImageList_BeginDrag", first, 0, 0, 0, 0) == 0u,
          "begin drag destroyed");
    CHECK(invoke("ImageList_Destroy", first, 0, 0, 0, 0) == 0u,
          "destroy twice");
    CHECK(invoke("ImageList_Destroy", second, 0, 0, 0, 0) == 1u,
          "destroy second");

    printf("comctl32 imagelist OK (%d handlers)\n", installed);
    return 0;
}
