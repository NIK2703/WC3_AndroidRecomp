/*
 * kernel32_resource_test.c - T0 contract test for PE resource lookup.
 *
 * The test builds a minimal PE32 image in the fake guest arena: one resource
 * type (ID 256), one string name ("BLIZZARDKEY"), one language (1033), and
 * four bytes of data.  It exercises the same path Storm.dll uses for its
 * SYSTEMPALETTE resource.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "guest_layout.h"
#include "hle_bridge.h"
#include "resource_hle.h"

#define GUEST_BASE GUEST_IMAGE_WAR3
#define GUEST_SIZE 0x10000u
#define MAX_ROWS 16u
#define HEAP_OFFSET 0x8000u

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
} Row;

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;
static uint32_t g_args[3];
static uint8_t g_mem[GUEST_SIZE];
static uint32_t g_heap_next;

static void *guest_ptr(uint32_t address, uint32_t size)
{
    if (address < GUEST_BASE) return NULL;
    const uint32_t offset = address - GUEST_BASE;
    if (offset > GUEST_SIZE || size > GUEST_SIZE - offset) return NULL;
    return &g_mem[offset];
}

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    (void)argc;
    (void)cdecl_abi;
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
    (void)dll;
    (void)name;
    (void)impl;
    return -1;
}

uint32_t wc3_legacy_arg32(const CPU *c, unsigned index)
{
    (void)c;
    return index < 3u ? g_args[index] : 0;
}

int wc3_legacy_guest_ok(uint32_t address, uint32_t size)
{
    return guest_ptr(address, size) != NULL;
}

uint8_t wc3_legacy_rd8g(uint32_t address)
{
    const void *p = guest_ptr(address, 1u);
    return p ? *(const uint8_t *)p : 0;
}

uint16_t wc3_legacy_rd16g(uint32_t address)
{
    const void *p = guest_ptr(address, 2u);
    if (!p) return 0;
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

uint32_t wc3_legacy_rd32g(uint32_t address)
{
    const void *p = guest_ptr(address, 4u);
    if (!p) return 0;
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

void wc3_legacy_wr8g(uint32_t address, uint8_t value)
{
    void *p = guest_ptr(address, 1u);
    if (p) *(uint8_t *)p = value;
}

void wc3_legacy_wr32g(uint32_t address, uint32_t value)
{
    void *p = guest_ptr(address, 4u);
    if (!p) return;
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)value;
    b[1] = (uint8_t)(value >> 8);
    b[2] = (uint8_t)(value >> 16);
    b[3] = (uint8_t)(value >> 24);
}

uint32_t wc3_legacy_guest_alloc0(uint32_t size)
{
    if (!size || size > 0x1000u) return 0;
    const uint32_t aligned = (size + 15u) & ~15u;
    if (g_heap_next > 0x1000u - aligned) return 0;
    const uint32_t address = GUEST_BASE + HEAP_OFFSET + g_heap_next;
    g_heap_next += aligned;
    memset(guest_ptr(address, aligned), 0, aligned);
    return address;
}

void wc3_legacy_guest_free(uint32_t address)
{
    (void)address;
}

static void put16(uint32_t address, uint16_t value)
{
    wc3_legacy_wr8g(address, (uint8_t)value);
    wc3_legacy_wr8g(address + 1u, (uint8_t)(value >> 8));
}

static void put32(uint32_t address, uint32_t value)
{
    wc3_legacy_wr32g(address, value);
}

static void put_string(uint32_t address, const char *text)
{
    memcpy(guest_ptr(address, strlen(text) + 1u), text, strlen(text) + 1u);
}

static void build_pe(void)
{
    memset(g_mem, 0, sizeof g_mem);
    put16(GUEST_BASE, 0x5A4Du);              /* MZ */
    put32(GUEST_BASE + 0x3Cu, 0x80u);         /* e_lfanew */
    put32(GUEST_BASE + 0x80u, 0x00004550u);   /* PE\0\0 */
    put16(GUEST_BASE + 0x98u, 0x10Bu);        /* PE32 */
    put32(GUEST_BASE + 0x108u, 0x200u);       /* resource RVA */
    put32(GUEST_BASE + 0x10Cu, 0x100u);       /* resource size */

    const uint32_t root = GUEST_BASE + 0x200u;
    put16(root + 12u, 0u);                    /* named types */
    put16(root + 14u, 1u);                    /* ID types */
    put32(root + 16u, 256u);                  /* type ID */
    put32(root + 20u, 0x80000018u);           /* type directory */

    const uint32_t names = root + 0x18u;
    put16(names + 12u, 1u);                   /* named names */
    put16(names + 14u, 0u);
    put32(names + 16u, 0x80000040u);          /* "BLIZZARDKEY" */
    put32(names + 20u, 0x80000060u);          /* language directory */

    put16(root + 0x40u, 11u);                 /* string length */
    for (unsigned i = 0; i < 11u; i++)
        put16(root + 0x42u + i * 2u, (uint16_t)"BLIZZARDKEY"[i]);

    const uint32_t languages = root + 0x60u;
    put16(languages + 12u, 0u);
    put16(languages + 14u, 1u);
    put32(languages + 16u, 1033u);
    put32(languages + 20u, 0x80u);            /* data entry */

    put32(root + 0x80u, 0x300u);              /* data RVA */
    put32(root + 0x84u, 4u);                  /* data size */
    memcpy(guest_ptr(GUEST_BASE + 0x300u, 4u), "PAL!", 4u);
}

static void (*find_impl(const char *name))(CPU *c)
{
    for (unsigned i = 0; i < g_row_count; i++) {
        if (strcmp(g_rows[i].name, name) == 0) return g_rows[i].impl;
    }
    return NULL;
}

static uint32_t invoke(const char *name, uint32_t a0, uint32_t a1, uint32_t a2)
{
    void (*impl)(CPU *c) = find_impl(name);
    if (!impl) {
        printf("missing handler: %s\n", name);
        return 0xFFFFFFFFu;
    }
    memset(g_args, 0, sizeof g_args);
    g_args[0] = a0;
    g_args[1] = a1;
    g_args[2] = a2;
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
    const int installed = wc3_hle_kernel32_resource_install();
    CHECK(installed == 5, "install count");
    CHECK(g_row_count == 5u, "row count");
    CHECK(find_impl("FindResourceA") != NULL, "FindResourceA row");
    CHECK(find_impl("LoadResource") != NULL, "LoadResource row");
    CHECK(find_impl("LockResource") != NULL, "LockResource row");
    CHECK(find_impl("SizeofResource") != NULL, "SizeofResource row");
    CHECK(find_impl("FreeResource") != NULL, "FreeResource row");

    build_pe();
    const uint32_t name = GUEST_BASE + 0x400u;
    const uint32_t type = GUEST_BASE + 0x420u;
    put_string(name, "BLIZZARDKEY");
    put_string(type, "#256");

    const uint32_t record = invoke("FindResourceA", GUEST_BASE, name, type);
    CHECK(record != 0u, "FindResourceA string type");
    CHECK(record >= GUEST_BASE + HEAP_OFFSET, "resource record arena");
    CHECK(invoke("FindResourceA", GUEST_BASE, name, 256u) != 0u,
          "FindResourceA numeric type");
    CHECK(invoke("FindResourceA", GUEST_BASE, name, type + 1u) == 0u,
          "FindResourceA invalid type string");
    CHECK(invoke("FindResourceA", GUEST_BASE, name, 0u) == 0u,
          "FindResourceA NULL type");
    CHECK(invoke("FindResourceA", 0x1000u, name, type) == 0u,
          "FindResourceA bad module");

    const uint32_t missing = GUEST_BASE + 0x440u;
    put_string(missing, "SYSTEMPALETTE");
    CHECK(invoke("FindResourceA", GUEST_BASE, missing, type) == 0u,
          "FindResourceA missing name");

    CHECK(invoke("LoadResource", GUEST_BASE, record, 0u) == record,
          "LoadResource handle");
    CHECK(invoke("LockResource", record, 0u, 0u) == GUEST_BASE + 0x300u,
          "LockResource data");
    CHECK(memcmp(guest_ptr(GUEST_BASE + 0x300u, 4u), "PAL!", 4u) == 0,
          "resource payload");
    CHECK(invoke("SizeofResource", GUEST_BASE, record, 0u) == 4u,
          "SizeofResource size");
    CHECK(invoke("FreeResource", record, 0u, 0u) == 0u,
          "FreeResource valid");
    CHECK(invoke("LoadResource", GUEST_BASE, 0u, 0u) == 0u,
          "LoadResource NULL");
    CHECK(invoke("LockResource", 0u, 0u, 0u) == 0u, "LockResource NULL");
    CHECK(invoke("SizeofResource", GUEST_BASE, 0u, 0u) == 0u,
          "SizeofResource NULL");
    CHECK(invoke("FreeResource", 0u, 0u, 0u) == 1u, "FreeResource NULL");

    printf("kernel32 resource OK (%d handlers)\n", installed);
    return 0;
}
