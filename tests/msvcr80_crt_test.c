/*
 * msvcr80_crt_test.c - T0 contract test for the MSVCR80 CRT runtime slice.
 *
 * The fake shim records the added rows and exposes a 32-bit guest cell plus a
 * small guest string area.  The test pins the measured _vacopy behavior
 * (source pointer stored into the destination va_list, EAX left equal to the
 * destination pointer), the C-locale setlocale row, and the fatal rows: abort
 * and _wassert must terminate with CRT exit code 3.  Termination is exercised
 * in a child process because it cannot return.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "cpu.h"
#include "crt_hle.h"
#include "legacy_shim.h"

#define MAX_ROWS 5u
#define CELL_ADDR 0x2000u
#define STRING_ADDR 0x2100u
#define STRING_SIZE 64u
#define LOCALE_ADDR 0x2200u
#define LOCALE_SIZE 2u
#define INVALID_ADDR 0x1000u

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
    int argc;
    int cdecl_abi;
} Row;

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;
static uint32_t g_args[4];
static uint32_t g_cell;
static uint8_t g_string[STRING_SIZE];
static uint8_t g_locale[LOCALE_SIZE];
static int g_failures;

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    if (!dll || !name || !impl || argc < 0) return -1;
    if (g_row_count >= MAX_ROWS) return -1;
    g_rows[g_row_count].dll = dll;
    g_rows[g_row_count].name = name;
    g_rows[g_row_count].impl = impl;
    g_rows[g_row_count].argc = argc;
    g_rows[g_row_count].cdecl_abi = cdecl_abi;
    g_row_count++;
    return 0;
}

uint32_t wc3_legacy_guest_alloc0(uint32_t size)
{
    if (size > LOCALE_SIZE) return 0;
    memset(g_locale, 0, sizeof g_locale);
    return LOCALE_ADDR;
}

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    if (addr == CELL_ADDR) return size <= 4u;
    if (addr >= STRING_ADDR && addr <= STRING_ADDR + STRING_SIZE)
        return size <= STRING_ADDR + STRING_SIZE - addr;
    if (addr >= LOCALE_ADDR && addr <= LOCALE_ADDR + LOCALE_SIZE)
        return size <= LOCALE_ADDR + LOCALE_SIZE - addr;
    return 0;
}

uint8_t wc3_legacy_rd8g(uint32_t addr)
{
    if (addr >= STRING_ADDR && addr < STRING_ADDR + STRING_SIZE)
        return g_string[addr - STRING_ADDR];
    if (addr >= LOCALE_ADDR && addr < LOCALE_ADDR + LOCALE_SIZE)
        return g_locale[addr - LOCALE_ADDR];
    return 0u;
}

void wc3_legacy_wr8g(uint32_t addr, uint8_t value)
{
    if (addr >= STRING_ADDR && addr < STRING_ADDR + STRING_SIZE)
        g_string[addr - STRING_ADDR] = value;
    else if (addr >= LOCALE_ADDR && addr < LOCALE_ADDR + LOCALE_SIZE)
        g_locale[addr - LOCALE_ADDR] = value;
}

uint16_t wc3_legacy_rd16g(uint32_t addr)
{
    (void)addr;
    return 0u;
}

void wc3_legacy_wr32g(uint32_t addr, uint32_t value)
{
    if (addr == CELL_ADDR) g_cell = value;
}

uint32_t wc3_legacy_arg32(const CPU *c, unsigned index)
{
    (void)c;
    return index < sizeof g_args / sizeof g_args[0] ? g_args[index] : 0u;
}

static void check(int condition, const char *message)
{
    if (!condition) {
        printf("FAIL: %s\n", message);
        g_failures++;
    }
}

static void (*find_impl(const char *name))(CPU *c)
{
    for (unsigned i = 0; i < g_row_count; i++)
        if (strcmp(g_rows[i].name, name) == 0) return g_rows[i].impl;
    return NULL;
}

static uint32_t call_vacopy(void (*impl)(CPU *c), uint32_t destination,
                            uint32_t source)
{
    CPU c;
    memset(&c, 0, sizeof c);
    g_args[0] = destination;
    g_args[1] = source;
    c.eax = 0xDEADBEEFu;
    impl(&c);
    return c.eax;
}

static void put_string(const char *text)
{
    memset(g_string, 0, sizeof g_string);
    const size_t length = strlen(text);
    const size_t copy = length < sizeof g_string ? length : sizeof g_string;
    memcpy(g_string, text, copy);
}

static int locale_is_c(void)
{
    return g_locale[0] == (uint8_t)'C' && g_locale[1] == 0u;
}

static uint32_t call_setlocale(void (*impl)(CPU *c), uint32_t category,
                               uint32_t locale)
{
    CPU c;
    memset(&c, 0, sizeof c);
    g_args[0] = category;
    g_args[1] = locale;
    c.eax = 0xDEADBEEFu;
    impl(&c);
    return c.eax;
}

static int child_mode(const char *mode)
{
    if (wc3_hle_msvcr80_crt_install() != 4) return 90;

    void (*impl)(CPU *c) = find_impl(mode);
    if (!impl) return 91;

    CPU c;
    memset(&c, 0, sizeof c);
    memset(g_args, 0, sizeof g_args);
    g_args[2] = 77u;
    impl(&c);
    return 92; /* the fatal handlers must not return */
}

static void check_child(const char *self, const char *mode,
                        const char *message)
{
    char command[1024];
    if (!self) {
        check(0, message);
        return;
    }
    snprintf(command, sizeof command, "\"%s\" %s", self, mode);
    const int status = system(command);
#ifdef _WIN32
    check(status == 3, message);
#else
    check(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 3,
          message);
#endif
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--abort-child") == 0)
        return child_mode("abort");
    if (argc > 1 && strcmp(argv[1], "--assert-child") == 0)
        return child_mode("_wassert");

    const int installed = wc3_hle_msvcr80_crt_install();
    check(installed == 4, "install count");
    check(g_row_count == 4u, "recorded row count");
    if (g_row_count != 4u) return 1;

    static const char *const names[] = {
        "_vacopy", "abort", "_wassert", "setlocale"
    };
    static const int arities[] = { 2, 0, 3, 2 };
    for (unsigned i = 0; i < 4u; i++) {
        check(strcmp(g_rows[i].dll, "MSVCR80.dll") == 0, "row dll");
        check(strcmp(g_rows[i].name, names[i]) == 0, "row name");
        check(g_rows[i].argc == arities[i], "row argc");
        check(g_rows[i].cdecl_abi == 1, "row cdecl");
    }

    void (*impl)(CPU *c) = find_impl("_vacopy");
    check(impl != NULL, "_vacopy handler");

    g_cell = 0xAAAAAAAAu;
    check(call_vacopy(impl, CELL_ADDR, 0x12345678u) == CELL_ADDR,
          "copy eax");
    check(g_cell == 0x12345678u, "copy source");

    g_cell = 0xAAAAAAAAu;
    check(call_vacopy(impl, CELL_ADDR, 0u) == CELL_ADDR, "zero eax");
    check(g_cell == 0u, "zero source");

    g_cell = 0xAAAAAAAAu;
    check(call_vacopy(impl, INVALID_ADDR, 0x12345678u) == INVALID_ADDR,
          "invalid eax");
    check(g_cell == 0xAAAAAAAAu, "invalid destination untouched");

    g_cell = 0xAAAAAAAAu;
    check(call_vacopy(impl, 0u, 0x12345678u) == 0u, "null eax");
    check(g_cell == 0xAAAAAAAAu, "null destination untouched");

    void (*locale)(CPU *c) = find_impl("setlocale");
    check(locale != NULL, "setlocale handler");

    memset(g_locale, 0xAA, sizeof g_locale);
    check(call_setlocale(locale, 0u, 0u) == LOCALE_ADDR,
          "query returns C");
    check(locale_is_c(), "query name is C");

    put_string("C");
    check(call_setlocale(locale, 2u, STRING_ADDR) == LOCALE_ADDR,
          "C accepted");
    put_string("");
    check(call_setlocale(locale, 0u, STRING_ADDR) == LOCALE_ADDR,
          "empty accepted");
    put_string("POSIX");
    check(call_setlocale(locale, 5u, STRING_ADDR) == LOCALE_ADDR,
          "POSIX accepted");
    put_string("en-US");
    check(call_setlocale(locale, 0u, STRING_ADDR) == 0u,
          "unsupported locale rejected");
    check(call_setlocale(locale, 6u, 0u) == 0u,
          "invalid category rejected");
    check(call_setlocale(locale, 0u, INVALID_ADDR) == 0u,
          "invalid string rejected");

    check_child(argv[0], "--abort-child", "abort exits with code 3");
    check_child(argv[0], "--assert-child", "_wassert exits with code 3");

    if (g_failures) {
        printf("msvcr80 crt: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("msvcr80 crt OK (4 handlers)\n");
    return 0;
}
