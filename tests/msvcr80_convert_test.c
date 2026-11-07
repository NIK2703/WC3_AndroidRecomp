/*
 * msvcr80_convert_test.c - T0 contract test for the MSVCR80 convert slice.
 *
 * The test links convert_hle.c against a fake shim and a small guest mapping.
 * It pins the 32-bit saturation, base selection and endptr behaviour of
 * strtol/strtoul, the x87 return ABI of atof, and the CP1251 predicates.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "convert_hle.h"
#include "legacy_shim.h"

#define MAX_ROWS 8u
#define GUEST_BASE 0x20000u
#define GUEST_SIZE 0x10000u
#define TEXT_ADDR (GUEST_BASE + 0x100u)
#define ENDP_ADDR (GUEST_BASE + 0x1000u)

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
} Row;

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;
static uint32_t g_args[8];
static uint8_t g_mem[GUEST_SIZE];
static int g_failures;

static void *guest_ptr(uint32_t addr, uint32_t size)
{
    if (addr < GUEST_BASE) return NULL;
    const uint32_t off = addr - GUEST_BASE;
    if (off > GUEST_SIZE || size > GUEST_SIZE - off) return NULL;
    return g_mem + off;
}

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    return guest_ptr(addr, size) != NULL;
}

uint8_t wc3_legacy_rd8g(uint32_t addr)
{
    const void *p = guest_ptr(addr, 1);
    return p ? *(const uint8_t *)p : 0u;
}

void wc3_legacy_wr32g(uint32_t addr, uint32_t value)
{
    void *p = guest_ptr(addr, 4);
    if (p) memcpy(p, &value, 4);
}

uint32_t wc3_legacy_arg32(const CPU *cpu, unsigned index)
{
    (void)cpu;
    return index < sizeof g_args / sizeof g_args[0] ? g_args[index] : 0u;
}

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    if (!dll || !name || !impl || g_row_count >= MAX_ROWS) return -1;
    g_rows[g_row_count].dll = dll;
    g_rows[g_row_count].name = name;
    g_rows[g_row_count].impl = impl;
    g_row_count++;
    return 0;
}

static void check(int condition, const char *message)
{
    if (!condition) {
        printf("FAIL: %s\n", message);
        g_failures++;
    }
}

static void eqd(const char *what, double got, double want)
{
    uint64_t g, w;
    memcpy(&g, &got, sizeof g);
    memcpy(&w, &want, sizeof w);
    if (g != w) {
        printf("FAIL %-24s got %.17g want %.17g\n", what, got, want);
        g_failures++;
    }
}

static void (*find_impl(const char *name))(CPU *c)
{
    for (unsigned i = 0; i < g_row_count; i++)
        if (strcmp(g_rows[i].name, name) == 0) return g_rows[i].impl;
    return NULL;
}

static void place_string(const char *text)
{
    const size_t length = strlen(text);
    if (length + 1u > 512u) {
        check(0, "test string too long");
        return;
    }
    memset(guest_ptr(TEXT_ADDR, 512), 0, 512);
    memcpy(guest_ptr(TEXT_ADDR, (uint32_t)length + 1u), text, length + 1u);
}

static uint32_t read_u32(uint32_t addr)
{
    uint32_t value = 0;
    const void *p = guest_ptr(addr, 4);
    if (p) memcpy(&value, p, 4);
    return value;
}

static double pop_x87(CPU *c)
{
    float64_t f = extF80_to_f64(fpop(c));
    double value;
    memcpy(&value, &f.v, sizeof value);
    return value;
}

static double call_atof(const char *text)
{
    void (*impl)(CPU *c) = find_impl("atof");
    if (!impl) {
        check(0, "missing atof");
        return 0.0;
    }
    place_string(text);
    memset(g_args, 0, sizeof g_args);
    g_args[0] = TEXT_ADDR;
    CPU c;
    memset(&c, 0, sizeof c);
    cpu_x87_init(&c);
    impl(&c);
    const double value = pop_x87(&c);
    check(c.fpu_top == 0, "x87 stack not balanced after atof");
    return value;
}

static uint32_t call_strtol(const char *text, int base, uint32_t *end_out)
{
    void (*impl)(CPU *c) = find_impl("strtol");
    if (!impl) {
        check(0, "missing strtol");
        return 0;
    }
    place_string(text);
    wc3_legacy_wr32g(ENDP_ADDR, 0xDEADBEEFu);
    memset(g_args, 0, sizeof g_args);
    g_args[0] = TEXT_ADDR;
    g_args[1] = ENDP_ADDR;
    g_args[2] = (uint32_t)base;
    CPU c;
    memset(&c, 0, sizeof c);
    impl(&c);
    if (end_out) *end_out = read_u32(ENDP_ADDR);
    return c.eax;
}

static uint32_t call_strtoul(const char *text, int base, uint32_t *end_out)
{
    void (*impl)(CPU *c) = find_impl("strtoul");
    if (!impl) {
        check(0, "missing strtoul");
        return 0;
    }
    place_string(text);
    wc3_legacy_wr32g(ENDP_ADDR, 0xDEADBEEFu);
    memset(g_args, 0, sizeof g_args);
    g_args[0] = TEXT_ADDR;
    g_args[1] = ENDP_ADDR;
    g_args[2] = (uint32_t)base;
    CPU c;
    memset(&c, 0, sizeof c);
    impl(&c);
    if (end_out) *end_out = read_u32(ENDP_ADDR);
    return c.eax;
}

static uint32_t call_one(const char *name, uint32_t arg)
{
    void (*impl)(CPU *c) = find_impl(name);
    if (!impl) {
        check(0, "missing one-arg handler");
        return 0;
    }
    memset(g_args, 0, sizeof g_args);
    g_args[0] = arg;
    CPU c;
    memset(&c, 0, sizeof c);
    impl(&c);
    return c.eax;
}

static uint32_t end_offset(uint32_t end)
{
    return end >= TEXT_ADDR ? end - TEXT_ADDR : 0xFFFFFFFFu;
}

int main(void)
{
    const int installed = msvcr80_convert_install();
    check(installed == 5, "install count");
    check(g_row_count == 5u, "row count");
    for (unsigned i = 0; i < g_row_count; i++)
        check(strcmp(g_rows[i].dll, "MSVCR80.dll") == 0, "DLL name");
    check(find_impl("atof") != NULL, "atof row");
    check(find_impl("strtol") != NULL, "strtol row");
    check(find_impl("strtoul") != NULL, "strtoul row");
    check(find_impl("_ismbblead") != NULL, "_ismbblead row");
    check(find_impl("_ismbcspace") != NULL, "_ismbcspace row");

    eqd("atof zero", call_atof("0"), 0.0);
    eqd("atof fraction", call_atof("1.5"), 1.5);
    eqd("atof exponent", call_atof("-2.25e2"), -225.0);
    eqd("atof trailing", call_atof("  3.5abc"), 3.5);
    eqd("atof invalid", call_atof("abc"), 0.0);
    eqd("atof leading dot", call_atof("+.5"), 0.5);
    eqd("atof tiny", call_atof("1e-3"), 0.001);
    eqd("atof empty", call_atof(""), 0.0);

    {
        uint32_t end = 0;
        check(call_strtol("123", 10, &end) == 123u, "strtol decimal");
        check(end_offset(end) == 3u, "strtol decimal end");

        check(call_strtol("-2147483648", 10, &end) == 0x80000000u,
              "strtol LONG_MIN");
        check(end_offset(end) == 11u, "strtol LONG_MIN end");

        check(call_strtol("2147483647", 10, &end) == 0x7FFFFFFFu,
              "strtol LONG_MAX");
        check(end_offset(end) == 10u, "strtol LONG_MAX end");

        check(call_strtol("2147483648", 10, &end) == 0x7FFFFFFFu,
              "strtol positive saturation");
        check(end_offset(end) == 10u, "strtol positive saturation end");

        check(call_strtol("-2147483649", 10, &end) == 0x80000000u,
              "strtol negative saturation");
        check(end_offset(end) == 11u, "strtol negative saturation end");

        check(call_strtol("999999999999999999999999", 10, &end) ==
                  0x7FFFFFFFu,
              "strtol huge saturation");

        check(call_strtol("0x1A", 0, &end) == 26u, "strtol auto hex");
        check(end_offset(end) == 4u, "strtol auto hex end");

        check(call_strtol("0x1A", 16, &end) == 26u, "strtol explicit hex");
        check(end_offset(end) == 4u, "strtol explicit hex end");

        check(call_strtol("010", 0, &end) == 8u, "strtol auto octal");
        check(end_offset(end) == 3u, "strtol auto octal end");

        check(call_strtol("010", 10, &end) == 10u, "strtol decimal zero");
        check(end_offset(end) == 3u, "strtol decimal zero end");

        check(call_strtol("  +42xyz", 10, &end) == 42u,
              "strtol whitespace sign");
        check(end_offset(end) == 5u, "strtol whitespace sign end");

        check(call_strtol("xyz", 10, &end) == 0u, "strtol no digits");
        check(end_offset(end) == 0u, "strtol no digits end");

        check(call_strtol("  +", 10, &end) == 0u, "strtol sign only");
        check(end_offset(end) == 0u, "strtol sign only end");

        check(call_strtol("z", 36, &end) == 35u, "strtol base 36");
        check(end_offset(end) == 1u, "strtol base 36 end");

        check(call_strtol("0x", 0, &end) == 0u, "strtol bare hex prefix");
        check(end_offset(end) == 1u, "strtol bare hex prefix end");

        check(call_strtol("-1", 0, &end) == 0xFFFFFFFFu,
              "strtol negative one");
        check(end_offset(end) == 2u, "strtol negative one end");

        check(call_strtol("1", 1, &end) == 0u, "strtol base one");
        check(end_offset(end) == 0u, "strtol base one end");

        check(call_strtol("1", 37, &end) == 0u, "strtol base 37");
        check(end_offset(end) == 0u, "strtol base 37 end");
    }

    /* A null endptr is accepted, and an endptr outside guest memory is left
     * alone rather than written through a bad pointer. */
    check(call_strtol("77", 10, NULL) == 77u, "strtol null endptr");
    {
        void (*impl)(CPU *c) = find_impl("strtol");
        place_string("88");
        memset(g_args, 0, sizeof g_args);
        g_args[0] = TEXT_ADDR;
        g_args[1] = GUEST_BASE + GUEST_SIZE - 2u;
        g_args[2] = 10u;
        CPU c;
        memset(&c, 0, sizeof c);
        impl(&c);
        check(c.eax == 88u, "strtol bad endptr result");
    }

    {
        uint32_t end = 0;
        check(call_strtoul("4294967295", 10, &end) == 0xFFFFFFFFu,
              "strtoul max");
        check(end_offset(end) == 10u, "strtoul max end");

        check(call_strtoul("4294967296", 10, &end) == 0xFFFFFFFFu,
              "strtoul saturation");
        check(end_offset(end) == 10u, "strtoul saturation end");

        check(call_strtoul("999999999999999999999", 0, &end) == 0xFFFFFFFFu,
              "strtoul huge saturation");

        check(call_strtoul("-1", 10, &end) == 0xFFFFFFFFu,
              "strtoul negative one");
        check(end_offset(end) == 2u, "strtoul negative one end");

        check(call_strtoul("-4294967295", 10, &end) == 1u,
              "strtoul negate modulo");
        check(end_offset(end) == 11u, "strtoul negate modulo end");

        check(call_strtoul("-4294967296", 10, &end) == 0xFFFFFFFFu,
              "strtoul negative saturation");

        check(call_strtoul("0x10", 0, &end) == 16u, "strtoul auto hex");
        check(end_offset(end) == 4u, "strtoul auto hex end");

        check(call_strtoul("077", 0, &end) == 63u, "strtoul auto octal");
        check(end_offset(end) == 3u, "strtoul auto octal end");

        check(call_strtoul("1", 37, &end) == 0u, "strtoul base 37");
        check(end_offset(end) == 0u, "strtoul base 37 end");
    }

    check(call_strtoul("88", 10, NULL) == 88u, "strtoul null endptr");

    check(call_one("_ismbcspace", 0x20u) == 1u, "ismbcspace space");
    check(call_one("_ismbcspace", 0x09u) == 1u, "ismbcspace tab");
    check(call_one("_ismbcspace", 0x0Au) == 1u, "ismbcspace newline");
    check(call_one("_ismbcspace", 0x0Du) == 1u, "ismbcspace return");
    check(call_one("_ismbcspace", 'A') == 0u, "ismbcspace letter");
    check(call_one("_ismbcspace", 0x00u) == 0u, "ismbcspace NUL");
    check(call_one("_ismbcspace", 0xA0u) == 0u, "ismbcspace NBSP");
    check(call_one("_ismbcspace", 0xFFu) == 0u, "ismbcspace CP1251");

    check(call_one("_ismbblead", 0x00u) == 0u, "ismbblead NUL");
    check(call_one("_ismbblead", 'A') == 0u, "ismbblead ASCII");
    check(call_one("_ismbblead", 0x80u) == 0u, "ismbblead 0x80");
    check(call_one("_ismbblead", 0xC0u) == 0u, "ismbblead 0xC0");
    check(call_one("_ismbblead", 0xFFu) == 0u, "ismbblead 0xFF");

    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("msvcr80 convert OK (%d handlers)\n", installed);
    return 0;
}
