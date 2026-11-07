/*
 * msvcr80_rand_test.c - T0 contract test for the MSVCR80 srand/rand pair.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "rand_hle.h"

#define MAX_ROWS 4u

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
} Row;

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;
static uint32_t g_arg0;

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

#define CHECK(condition, message)                                         \
    do {                                                                  \
        if (!(condition)) {                                               \
            printf("FAIL: %s\n", message);                                \
            return 1;                                                     \
        }                                                                 \
    } while (0)

int main(void)
{
    const int installed = wc3_hle_msvcr80_rand_install();
    CHECK(installed == 2, "install count");
    CHECK(g_row_count == 2u, "row count");
    CHECK(find_impl("srand") != NULL, "srand row");
    CHECK(find_impl("rand") != NULL, "rand row");
    CHECK(strcmp(g_rows[0].dll, "MSVCR80.dll") == 0, "DLL name");

    CHECK(invoke("rand", 0) == 41u, "default sequence first");
    CHECK(invoke("rand", 0) == 18467u, "default sequence second");

    CHECK(invoke("srand", 0x150Bu) == 0u, "srand return");
    CHECK(invoke("rand", 0) == 17630u, "seeded sequence first");
    CHECK(invoke("rand", 0) == 8328u, "seeded sequence second");
    CHECK(invoke("rand", 0) == 15914u, "seeded sequence third");

    CHECK(invoke("srand", 1) == 0u, "srand reset return");
    CHECK(invoke("rand", 0) == 41u, "srand reset first");

    printf("msvcr80 rand OK (%d handlers)\n", installed);
    return 0;
}
