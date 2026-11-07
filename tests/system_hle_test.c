/*
 * system_hle_test.c - T0 contract test for the kernel32 system subset.
 *
 * The OSVERSIONINFOA case checks that writes stop at dwOSVersionInfoSize: the
 * old build wrote a wide-sized structure into an ANSI buffer and crossed the
 * guest stack cookie.  The SYSTEM_INFO and STARTUPINFOA cases check the exact
 * field offsets and the tail guard byte.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "system_hle.h"

#define GUEST_BASE 0x10000u
#define ARENA_SIZE 0x10000u
#define MAX_ROWS 16u

static uint8_t g_mem[ARENA_SIZE];
static uint32_t g_args[4];

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
} Row;

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;

static void *guest_ptr(uint32_t addr, uint32_t size)
{
    if (addr < GUEST_BASE) return NULL;
    const uint32_t off = addr - GUEST_BASE;
    if (off > ARENA_SIZE || size > ARENA_SIZE - off) return NULL;
    return &g_mem[off];
}

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

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    (void)argc;
    (void)cdecl_abi;
    return wc3_legacy_hle_override(dll, name, impl);
}

uint32_t wc3_legacy_arg32(const CPU *c, unsigned index)
{
    (void)c;
    return index < 4u ? g_args[index] : 0;
}

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    return guest_ptr(addr, size) != NULL;
}

uint32_t wc3_legacy_rd32g(uint32_t addr)
{
    const void *p = guest_ptr(addr, 4);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

uint16_t wc3_legacy_rd16g(uint32_t addr)
{
    const void *p = guest_ptr(addr, 2);
    uint16_t v = 0;
    if (p) memcpy(&v, p, 2);
    return v;
}

uint8_t wc3_legacy_rd8g(uint32_t addr)
{
    const void *p = guest_ptr(addr, 1);
    return p ? *(const uint8_t *)p : 0;
}

void wc3_legacy_wr32g(uint32_t addr, uint32_t value)
{
    void *p = guest_ptr(addr, 4);
    if (p) memcpy(p, &value, 4);
}

void wc3_legacy_wr16g(uint32_t addr, uint16_t value)
{
    void *p = guest_ptr(addr, 2);
    if (p) memcpy(p, &value, 2);
}

void wc3_legacy_wr8g(uint32_t addr, uint8_t value)
{
    void *p = guest_ptr(addr, 1);
    if (p) *(uint8_t *)p = value;
}

void wc3_legacy_guest_zero(uint32_t addr, uint32_t size)
{
    void *p = guest_ptr(addr, size);
    if (p) memset(p, 0, size);
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
    memset(g_args, 0, sizeof g_args);
    g_args[0] = arg;
    CPU c;
    memset(&c, 0, sizeof c);
    impl(&c);
    return c.eax;
}

static uint32_t invoke3(const char *name, uint32_t a0, uint32_t a1,
                        uint32_t a2)
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

static uint32_t peek32(uint32_t addr) { return wc3_legacy_rd32g(addr); }
static uint16_t peek16(uint32_t addr) { return wc3_legacy_rd16g(addr); }
static uint8_t peek8(uint32_t addr) { return wc3_legacy_rd8g(addr); }

static void put_wide(uint32_t addr, const uint16_t *text)
{
    for (uint32_t i = 0;; i++) {
        wc3_legacy_wr16g(addr + i * 2u, text[i]);
        if (!text[i]) break;
    }
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
    const int installed = wc3_hle_kernel32_system_install();
    CHECK(installed == 8, "install count");
    CHECK(g_row_count == 8u, "row count");
    CHECK(find_impl("GetVersion") != NULL, "GetVersion row");
    CHECK(find_impl("GetNativeSystemInfo") != NULL, "GetNativeSystemInfo row");
    CHECK(find_impl("GlobalMemoryStatus") != NULL, "GlobalMemoryStatus row");
    CHECK(find_impl("GetModuleHandleW") != NULL, "GetModuleHandleW row");
    CHECK(find_impl("MulDiv") != NULL, "MulDiv row");
    CHECK(strcmp(g_rows[0].dll, "KERNEL32.dll") == 0, "DLL name");

    CHECK(invoke("GetVersion", 0) == 0x0A280105u, "GetVersion");

    const uint32_t p1 = GUEST_BASE + 0x100u;
    memset(guest_ptr(p1, 0x120), 0xAA, 0x120);
    wc3_legacy_wr32g(p1, 0x94);
    CHECK(invoke("GetVersionExA", p1) == 1u, "GetVersionExA ANSI return");
    CHECK(peek32(p1 + 0) == 0x94u, "OSVERSIONINFO size");
    CHECK(peek32(p1 + 4) == 5u, "major");
    CHECK(peek32(p1 + 8) == 1u, "minor");
    CHECK(peek32(p1 + 12) == 2600u, "build");
    CHECK(peek32(p1 + 16) == 2u, "platform");
    CHECK(peek8(p1 + 20) == 0u, "CSD terminator");
    CHECK(peek8(p1 + 0x94) == 0xAAu, "ANSI tail untouched");

    const uint32_t p2 = GUEST_BASE + 0x300u;
    memset(guest_ptr(p2, 0x120), 0xAA, 0x120);
    wc3_legacy_wr32g(p2, 0x9C);
    CHECK(invoke("GetVersionExA", p2) == 1u, "GetVersionExA EX return");
    CHECK(peek16(p2 + 148) == 0u, "service pack major");
    CHECK(peek16(p2 + 150) == 0u, "service pack minor");
    CHECK(peek16(p2 + 152) == 0u, "suite mask");
    CHECK(peek8(p2 + 154) == 1u, "product type");
    CHECK(peek8(p2 + 155) == 0u, "reserved");
    CHECK(peek8(p2 + 0x9C) == 0xAAu, "EX tail untouched");

    const uint32_t p3 = GUEST_BASE + 0x500u;
    memset(guest_ptr(p3, 0x40), 0xAA, 0x40);
    wc3_legacy_wr32g(p3, 0x90);
    CHECK(invoke("GetVersionExA", p3) == 0u, "too-small buffer rejected");
    CHECK(peek32(p3 + 4) == 0xAAAAAAAAu, "too-small buffer untouched");
    wc3_legacy_wr32g(p3, 0x11D);
    CHECK(invoke("GetVersionExA", p3) == 0u, "too-large buffer rejected");
    CHECK(peek32(p3 + 4) == 0xAAAAAAAAu, "too-large buffer untouched");
    CHECK(invoke("GetVersionExA", 0) == 0u, "NULL version buffer");

    const uint32_t si = GUEST_BASE + 0x700u;
    memset(guest_ptr(si, 40), 0xAA, 40);
    CHECK(invoke("GetSystemInfo", si) == 0u, "GetSystemInfo return");
    CHECK(peek16(si + 0) == 0u, "arch");
    CHECK(peek16(si + 2) == 0u, "reserved");
    CHECK(peek32(si + 4) == 0x1000u, "page size");
    CHECK(peek32(si + 8) == 0x00400000u, "min address");
    CHECK(peek32(si + 12) == 0x7FFEFFFFu, "max address");
    CHECK(peek32(si + 16) == 3u, "processor mask");
    CHECK(peek32(si + 20) == 4u, "processor count");
    CHECK(peek32(si + 24) == 1u, "processor type");
    CHECK(peek32(si + 28) == 0x10000u, "granularity");
    CHECK(peek16(si + 32) == 6u, "processor level");
    CHECK(peek16(si + 34) == 0x3A09u, "processor revision");
    CHECK(peek8(si + 36) == 0xAAu, "SYSTEM_INFO tail untouched");

    const uint32_t mem = GUEST_BASE + 0x780u;
    memset(guest_ptr(mem, 36), 0xAA, 36);
    CHECK(invoke("GlobalMemoryStatus", mem) == 0u, "GlobalMemoryStatus return");
    CHECK(peek32(mem + 0) == 32u, "MEMORYSTATUS length");
    CHECK(peek32(mem + 4) == 55u, "memory load");
    CHECK(peek32(mem + 8) == 0x7FFFFFFFu, "total physical");
    CHECK(peek32(mem + 12) == 0x3FFFFFFFu, "available physical");
    CHECK(peek32(mem + 16) == 0xFFFFFFFFu, "total page file");
    CHECK(peek32(mem + 20) == 0x7FFFFFFFu, "available page file");
    CHECK(peek32(mem + 24) == 0x7FFFFFFFu, "total virtual");
    CHECK(peek32(mem + 28) == 0x3FFFFFFFu, "available virtual");
    CHECK(peek8(mem + 32) == 0xAAu, "MEMORYSTATUS tail untouched");
    CHECK(invoke("GlobalMemoryStatus", 0) == 0u, "NULL MEMORYSTATUS");

    const uint32_t nsi = GUEST_BASE + 0x800u;
    memset(guest_ptr(nsi, 40), 0xAA, 40);
    CHECK(invoke("GetNativeSystemInfo", nsi) == 0u,
          "GetNativeSystemInfo return");
    CHECK(peek32(nsi + 20) == 4u, "native processor count");
    CHECK(peek8(nsi + 36) == 0xAAu, "native tail untouched");

    const uint32_t startup = GUEST_BASE + 0x900u;
    memset(guest_ptr(startup, 72), 0xAA, 72);
    CHECK(invoke("GetStartupInfoA", startup) == 0u, "GetStartupInfoA return");
    for (unsigned i = 0; i < 68; i++)
        CHECK(peek8(startup + i) == 0u, "STARTUPINFOA zero");
    CHECK(peek8(startup + 68) == 0xAAu, "STARTUPINFOA tail untouched");
    CHECK(invoke("GetStartupInfoA", 0) == 0u, "NULL startup buffer");

    CHECK((int32_t)invoke3("MulDiv", 10, 3, 2) == 15, "MulDiv exact");
    CHECK((int32_t)invoke3("MulDiv", 1, 1, 2) == 1, "MulDiv half up");
    CHECK((int32_t)invoke3("MulDiv", 1, 1, 3) == 0, "MulDiv down");
    CHECK((int32_t)invoke3("MulDiv", 2, 1, 3) == 1, "MulDiv up");
    CHECK((int32_t)invoke3("MulDiv", 1, 0, 2) == 0, "MulDiv zero product");
    CHECK((int32_t)invoke3("MulDiv", (uint32_t)-1, 1, 2) == -1,
          "MulDiv negative half");
    CHECK((int32_t)invoke3("MulDiv", 1, 1, 0) == -1,
          "MulDiv zero denominator");
    CHECK((int32_t)invoke3("MulDiv", 0x7FFFFFFFu, 2, 1) == -1,
          "MulDiv overflow");
    CHECK((int32_t)invoke3("MulDiv", 0x80000000u, 1, 1) == INT32_MIN,
          "MulDiv INT32_MIN");
    CHECK((int32_t)invoke3("MulDiv", 0x7FFFFFFFu, 1, 1) == INT32_MAX,
          "MulDiv INT32_MAX");

    static const uint16_t war3[] =
        { 'w', 'a', 'r', '3', '.', 'e', 'x', 'e', 0 };
    static const uint16_t game[] =
        { 'G', 'a', 'm', 'e', '.', 'd', 'l', 'l', 0 };
    static const uint16_t game_path[] =
        { 'C', ':', '\\', 'g', 'a', 'm', 'e', 's', '\\', 'G', 'a', 'm', 'e',
          '.', 'D', 'L', 'L', 0 };
    static const uint16_t storm[] =
        { 'S', 't', 'o', 'r', 'm', '.', 'd', 'l', 'l', 0 };
    static const uint16_t mss[] =
        { 'm', 's', 's', '3', '2', '.', 'd', 'l', 'l', 0 };
    static const uint16_t mssfast[] =
        { 'm', 's', 's', 'f', 'a', 's', 't', '.', 'd', 'l', 'l', 0 };
    static const uint16_t mp3dec[] =
        { 'm', 'p', '3', 'd', 'e', 'c', '.', 'a', 's', 'i', 0 };
    static const uint16_t ijl[] =
        { 'i', 'j', 'l', '1', '5', '.', 'd', 'l', 'l', 0 };
    static const uint16_t unknown[] =
        { 'f', 'o', 'g', '.', 'd', 'l', 'l', 0 };
    static const uint16_t empty[] = { 0 };

    const uint32_t name_war3 = GUEST_BASE + 0xA00u;
    const uint32_t name_game = GUEST_BASE + 0xA20u;
    const uint32_t name_path = GUEST_BASE + 0xA40u;
    const uint32_t name_storm = GUEST_BASE + 0xA80u;
    const uint32_t name_mss = GUEST_BASE + 0xAA0u;
    const uint32_t name_mssfast = GUEST_BASE + 0xAC0u;
    const uint32_t name_mp3dec = GUEST_BASE + 0xAE0u;
    const uint32_t name_ijl = GUEST_BASE + 0xB00u;
    const uint32_t name_unknown = GUEST_BASE + 0xB20u;
    const uint32_t name_empty = GUEST_BASE + 0xB40u;
    put_wide(name_war3, war3);
    put_wide(name_game, game);
    put_wide(name_path, game_path);
    put_wide(name_storm, storm);
    put_wide(name_mss, mss);
    put_wide(name_mssfast, mssfast);
    put_wide(name_mp3dec, mp3dec);
    put_wide(name_ijl, ijl);
    put_wide(name_unknown, unknown);
    put_wide(name_empty, empty);

    CHECK(invoke("GetModuleHandleW", 0) == 0x00400000u,
          "GetModuleHandleW NULL");
    CHECK(invoke("GetModuleHandleW", name_war3) == 0x00400000u,
          "GetModuleHandleW war3");
    CHECK(invoke("GetModuleHandleW", name_game) == 0x6F000000u,
          "GetModuleHandleW game");
    CHECK(invoke("GetModuleHandleW", name_path) == 0x6F000000u,
          "GetModuleHandleW path");
    CHECK(invoke("GetModuleHandleW", name_storm) == 0x35000000u,
          "GetModuleHandleW storm");
    CHECK(invoke("GetModuleHandleW", name_mss) == 0x35100000u,
          "GetModuleHandleW mss");
    CHECK(invoke("GetModuleHandleW", name_mssfast) == 0x35200000u,
          "GetModuleHandleW mssfast");
    CHECK(invoke("GetModuleHandleW", name_mp3dec) == 0x35300000u,
          "GetModuleHandleW mp3dec");
    CHECK(invoke("GetModuleHandleW", name_ijl) == 0x60000000u,
          "GetModuleHandleW ijl");
    CHECK(invoke("GetModuleHandleW", name_unknown) == 0u,
          "GetModuleHandleW unknown");
    CHECK(invoke("GetModuleHandleW", name_empty) == 0u,
          "GetModuleHandleW empty");
    CHECK(invoke("GetModuleHandleW", 0xFFFFu) == 0u,
          "GetModuleHandleW bad pointer");

    printf("kernel32 system OK (%d handlers)\n", installed);
    return 0;
}
