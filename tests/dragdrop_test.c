/*
 * dragdrop_test.c - T0 contract test for the empty Android drop list.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "dragdrop_hle.h"
#include "legacy_shim.h"

#define MAX_ROWS 8u

typedef struct AddRow {
    const char *name;
    int argc;
    void (*impl)(CPU *c);
} AddRow;

static AddRow g_additions[MAX_ROWS];
static unsigned g_addition_count;
static uint32_t g_args[4];

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
    return index < 4u ? g_args[index] : 0u;
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
    for (unsigned i = 0; i < 4u; i++) g_args[i] = 0u;
    for (unsigned i = 0; i < count && i < 4u; i++) g_args[i] = args[i];
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
    const int installed = wc3_hle_shell32_dragdrop_install();
    CHECK(installed == 2, "install count");
    CHECK(g_addition_count == 2u, "addition count");

    int argc = -1;
    CHECK(find_impl("DragAcceptFiles", &argc) != NULL, "accept import");
    CHECK(argc == 2, "accept argc");
    CHECK(find_impl("DragQueryFileA", &argc) != NULL, "query import");
    CHECK(argc == 4, "query argc");

    const uint32_t accept[2] = { 0x00020004u, 1u };
    CHECK(invoke("DragAcceptFiles", accept, 2u) == 0u, "accept files");
    const uint32_t accept_null[2] = { 0u, 0u };
    CHECK(invoke("DragAcceptFiles", accept_null, 2u) == 0u,
          "accept null window");

    const uint32_t count_query[4] = {
        0x00ABCDEFu, 0xFFFFFFFFu, 0u, 0u,
    };
    CHECK(invoke("DragQueryFileA", count_query, 4u) == 0u,
          "drop count is zero");

    const uint32_t name_query[4] = {
        0x00ABCDEFu, 0u, 0x00024000u, 260u,
    };
    CHECK(invoke("DragQueryFileA", name_query, 4u) == 0u,
          "drop name is empty");
    const uint32_t null_name[4] = {
        0x00ABCDEFu, 0u, 0u, 0u,
    };
    CHECK(invoke("DragQueryFileA", null_name, 4u) == 0u,
          "null name is empty");
    const uint32_t null_drop[4] = { 0u, 0u, 0u, 0u };
    CHECK(invoke("DragQueryFileA", null_drop, 4u) == 0u,
          "null drop is empty");

    printf("drag/drop OK (%d handlers)\n", installed);
    return 0;
}
