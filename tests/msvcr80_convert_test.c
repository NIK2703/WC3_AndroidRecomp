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
#include "hle_bridge.h"

#define MAX_ROWS 16u
#define GUEST_BASE 0x20000u
#define GUEST_SIZE 0x10000u
#define TEXT_ADDR (GUEST_BASE + 0x100u)
#define ENDP_ADDR (GUEST_BASE + 0x1000u)
#define OUT_ADDR (GUEST_BASE + 0x2000u)
#define SCRATCH_ADDR (GUEST_BASE + 0xF000u)

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

uint16_t wc3_legacy_rd16g(uint32_t addr)
{
    const void *p = guest_ptr(addr, 2);
    if (!p) return 0u;
    uint16_t value;
    memcpy(&value, p, 2);
    return value;
}

void wc3_legacy_wr8g(uint32_t addr, uint8_t value)
{
    void *p = guest_ptr(addr, 1);
    if (p) *(uint8_t *)p = value;
}

void wc3_legacy_wr16g(uint32_t addr, uint16_t value)
{
    void *p = guest_ptr(addr, 2);
    if (p) memcpy(p, &value, 2);
}

void wc3_legacy_wr32g(uint32_t addr, uint32_t value)
{
    void *p = guest_ptr(addr, 4);
    if (p) memcpy(p, &value, 4);
}

uint32_t wc3_legacy_guest_alloc(uint32_t size)
{
    return size <= 0x1000u ? SCRATCH_ADDR : 0u;
}

void wc3_legacy_guest_free(uint32_t addr)
{
    (void)addr;
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

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    (void)argc;
    (void)cdecl_abi;
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

static void place_wide(const char *text)
{
    const size_t length = strlen(text);
    if (length + 1u > 512u) {
        check(0, "test wide string too long");
        return;
    }
    for (size_t i = 0; i <= length; i++)
        wc3_legacy_wr16g(TEXT_ADDR + (uint32_t)i * 2u, (uint16_t)text[i]);
}

static void place_wide_units(const uint16_t *units, size_t count)
{
    if (count > 512u) {
        check(0, "test wide unit list too long");
        return;
    }
    for (size_t i = 0; i < count; i++)
        wc3_legacy_wr16g(TEXT_ADDR + (uint32_t)i * 2u, units[i]);
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

static uint64_t call_strtoll(const char *text, int base, uint32_t *end_out)
{
    void (*impl)(CPU *c) = find_impl("strtoll");
    if (!impl) {
        check(0, "missing strtoll");
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
    return ((uint64_t)c.edx << 32) | c.eax;
}

static uint64_t call_strtoull(const char *text, int base, uint32_t *end_out)
{
    void (*impl)(CPU *c) = find_impl("strtoull");
    if (!impl) {
        check(0, "missing strtoull");
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
    return ((uint64_t)c.edx << 32) | c.eax;
}

static double call_strtod(const char *text, uint32_t *end_out)
{
    void (*impl)(CPU *c) = find_impl("strtod");
    if (!impl) {
        check(0, "missing strtod");
        return 0.0;
    }
    place_string(text);
    wc3_legacy_wr32g(ENDP_ADDR, 0xDEADBEEFu);
    memset(g_args, 0, sizeof g_args);
    g_args[0] = TEXT_ADDR;
    g_args[1] = ENDP_ADDR;
    CPU c;
    memset(&c, 0, sizeof c);
    cpu_x87_init(&c);
    impl(&c);
    if (end_out) *end_out = read_u32(ENDP_ADDR);
    const double value = pop_x87(&c);
    check(c.fpu_top == 0, "x87 stack not balanced after strtod");
    return value;
}

static uint64_t call_wide_integer(const char *name, const char *text, int base,
                                  uint32_t *end_out)
{
    void (*impl)(CPU *c) = find_impl(name);
    if (!impl) {
        check(0, "missing wide integer handler");
        return 0;
    }
    place_wide(text);
    wc3_legacy_wr32g(ENDP_ADDR, 0xDEADBEEFu);
    memset(g_args, 0, sizeof g_args);
    g_args[0] = TEXT_ADDR;
    g_args[1] = ENDP_ADDR;
    g_args[2] = (uint32_t)base;
    CPU c;
    memset(&c, 0, sizeof c);
    impl(&c);
    if (end_out) *end_out = read_u32(ENDP_ADDR);
    return ((uint64_t)c.edx << 32) | c.eax;
}

static double call_wcstod(const char *text, uint32_t *end_out)
{
    void (*impl)(CPU *c) = find_impl("wcstod");
    if (!impl) {
        check(0, "missing wcstod");
        return 0.0;
    }
    place_wide(text);
    wc3_legacy_wr32g(ENDP_ADDR, 0xDEADBEEFu);
    memset(g_args, 0, sizeof g_args);
    g_args[0] = TEXT_ADDR;
    g_args[1] = ENDP_ADDR;
    CPU c;
    memset(&c, 0, sizeof c);
    cpu_x87_init(&c);
    impl(&c);
    if (end_out) *end_out = read_u32(ENDP_ADDR);
    const double value = pop_x87(&c);
    check(c.fpu_top == 0, "x87 stack not balanced after wcstod");
    return value;
}

static uint32_t call_wcstombs_at(uint32_t dst, uint32_t src, uint32_t count)
{
    void (*impl)(CPU *c) = find_impl("wcstombs");
    if (!impl) {
        check(0, "missing wcstombs");
        return 0;
    }
    memset(g_args, 0, sizeof g_args);
    g_args[0] = dst;
    g_args[1] = src;
    g_args[2] = count;
    CPU c;
    memset(&c, 0, sizeof c);
    impl(&c);
    return c.eax;
}

static uint32_t call_wcstombs(uint32_t dst, const uint16_t *units,
                              size_t unit_count, uint32_t count)
{
    place_wide_units(units, unit_count);
    return call_wcstombs_at(dst, TEXT_ADDR, count);
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
    check(installed == 15, "install count");
    check(g_row_count == 15u, "row count");
    for (unsigned i = 0; i < g_row_count; i++)
        check(strcmp(g_rows[i].dll, "MSVCR80.dll") == 0, "DLL name");
    check(find_impl("atof") != NULL, "atof row");
    check(find_impl("strtol") != NULL, "strtol row");
    check(find_impl("strtoul") != NULL, "strtoul row");
    check(find_impl("_ismbblead") != NULL, "_ismbblead row");
    check(find_impl("_ismbcspace") != NULL, "_ismbcspace row");
    check(find_impl("strtod") != NULL, "strtod row");
    check(find_impl("strtoll") != NULL, "strtoll row");
    check(find_impl("strtoull") != NULL, "strtoull row");
    check(find_impl("_strtoi64") != NULL, "_strtoi64 row");
    check(find_impl("_strtoi64") == find_impl("strtoll"), "_strtoi64 alias");
    check(find_impl("wcstod") != NULL, "wcstod row");
    check(find_impl("wcstol") != NULL, "wcstol row");
    check(find_impl("wcstoul") != NULL, "wcstoul row");
    check(find_impl("wcstoll") != NULL, "wcstoll row");
    check(find_impl("wcstoull") != NULL, "wcstoull row");
    check(find_impl("wcstombs") != NULL, "wcstombs row");

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

    {
        uint32_t end = 0;

        check(call_strtoll("9223372036854775807", 10, &end) ==
                  (uint64_t)INT64_MAX,
              "strtoll INT64_MAX");
        check(end_offset(end) == 19u, "strtoll INT64_MAX end");

        check(call_strtoll("-9223372036854775808", 10, &end) ==
                  (uint64_t)INT64_MIN,
              "strtoll INT64_MIN");
        check(end_offset(end) == 20u, "strtoll INT64_MIN end");

        check(call_strtoll("9223372036854775808", 10, &end) ==
                  (uint64_t)INT64_MAX,
              "strtoll positive saturation");
        check(call_strtoll("-9223372036854775809", 10, &end) ==
                  (uint64_t)INT64_MIN,
              "strtoll negative saturation");
        check(call_strtoll("999999999999999999999999", 10, &end) ==
                  (uint64_t)INT64_MAX,
              "strtoll huge saturation");

        check(call_strtoll("0x7FFFFFFFFFFFFFFF", 0, &end) ==
                  (uint64_t)INT64_MAX,
              "strtoll auto hex");
        check(end_offset(end) == 18u, "strtoll auto hex end");

        check(call_strtoll("0x8000000000000000", 16, &end) ==
                  (uint64_t)INT64_MAX,
              "strtoll unsigned hex saturates");
        check(call_strtoll("-0x8000000000000000", 16, &end) ==
                  (uint64_t)INT64_MIN,
              "strtoll negative hex min");

        check(call_strtoll("xyz", 10, &end) == 0u, "strtoll no digits");
        check(end_offset(end) == 0u, "strtoll no digits end");
    }

    {
        uint32_t end = 0;

        check(call_strtoull("18446744073709551615", 10, &end) ==
                  UINT64_MAX,
              "strtoull UINT64_MAX");
        check(end_offset(end) == 20u, "strtoull UINT64_MAX end");

        check(call_strtoull("18446744073709551616", 10, &end) ==
                  UINT64_MAX,
              "strtoull saturation");
        check(call_strtoull("999999999999999999999999", 10, &end) ==
                  UINT64_MAX,
              "strtoull huge saturation");

        check(call_strtoull("-1", 10, &end) == UINT64_MAX,
              "strtoull negative one");
        check(end_offset(end) == 2u, "strtoull negative one end");

        check(call_strtoull("-18446744073709551615", 10, &end) == 1u,
              "strtoull negate modulo");
        check(call_strtoull("-18446744073709551616", 10, &end) ==
                  UINT64_MAX,
              "strtoull negative saturation");

        check(call_strtoull("0xFFFFFFFFFFFFFFFF", 0, &end) == UINT64_MAX,
              "strtoull auto hex");
        check(end_offset(end) == 18u, "strtoull auto hex end");
    }

    {
        void (*impl)(CPU *c) = find_impl("_strtoi64");
        check(impl != NULL, "_strtoi64 row");
        if (impl) {
            uint32_t end = 0;
            place_string("-42");
            memset(g_args, 0, sizeof g_args);
            g_args[0] = TEXT_ADDR;
            g_args[1] = ENDP_ADDR;
            g_args[2] = 10u;
            CPU c;
            memset(&c, 0, sizeof c);
            impl(&c);
            end = read_u32(ENDP_ADDR);
            check((((uint64_t)c.edx << 32) | c.eax) ==
                      (uint64_t)(int64_t)-42,
                  "_strtoi64 result");
            check(end_offset(end) == 3u, "_strtoi64 end");
        }
    }

    {
        uint32_t end = 0;

        eqd("strtod fraction", call_strtod("1.5", &end), 1.5);
        check(end_offset(end) == 3u, "strtod fraction end");

        eqd("strtod exponent", call_strtod("-2.25e2", &end), -225.0);
        check(end_offset(end) == 7u, "strtod exponent end");

        eqd("strtod trailing", call_strtod("  3.5abc", &end), 3.5);
        check(end_offset(end) == 5u, "strtod trailing end");

        eqd("strtod leading dot", call_strtod("+.5", &end), 0.5);
        check(end_offset(end) == 3u, "strtod leading dot end");

        eqd("strtod invalid", call_strtod("abc", &end), 0.0);
        check(end_offset(end) == 0u, "strtod invalid end");

        eqd("strtod tiny", call_strtod("1e-3", &end), 0.001);
        check(end_offset(end) == 4u, "strtod tiny end");
    }

    check(call_strtod("88", NULL) == 88.0, "strtod null endptr");

    {
        uint32_t end = 0;

        check((uint32_t)call_wide_integer("wcstol", "123", 10, &end) == 123u,
              "wcstol decimal");
        check(end_offset(end) == 6u, "wcstol decimal end");

        check((uint32_t)call_wide_integer("wcstol", "-2147483648", 10, &end) ==
                  0x80000000u,
              "wcstol LONG_MIN");
        check((uint32_t)call_wide_integer("wcstol", "0x1A", 0, &end) == 26u,
              "wcstol auto hex");
        check(end_offset(end) == 8u, "wcstol auto hex end");

        check((uint32_t)call_wide_integer("wcstol", "xyz", 10, &end) == 0u,
              "wcstol no digits");
        check(end_offset(end) == 0u, "wcstol no digits end");
        check((uint32_t)call_wide_integer("wcstol", "", 10, &end) == 0u,
              "wcstol empty");

        check((uint32_t)call_wide_integer("wcstoul", "4294967295", 10, &end) ==
                  0xFFFFFFFFu,
              "wcstoul max");
        check(end_offset(end) == 20u, "wcstoul max end");
        check((uint32_t)call_wide_integer("wcstoul", "-1", 10, &end) ==
                  0xFFFFFFFFu,
              "wcstoul negative one");

        check(call_wide_integer("wcstoll", "9223372036854775807", 10, &end) ==
                  (uint64_t)INT64_MAX,
              "wcstoll INT64_MAX");
        check(end_offset(end) == 38u, "wcstoll INT64_MAX end");
        check(call_wide_integer("wcstoll", "-9223372036854775808", 10, &end) ==
                  (uint64_t)INT64_MIN,
              "wcstoll INT64_MIN");

        check(call_wide_integer("wcstoull", "18446744073709551615", 10,
                                &end) == UINT64_MAX,
              "wcstoull UINT64_MAX");
        check(call_wide_integer("wcstoull", "-1", 10, &end) == UINT64_MAX,
              "wcstoull negative one");

        eqd("wcstod fraction", call_wcstod("1.5", &end), 1.5);
        check(end_offset(end) == 6u, "wcstod fraction end");

        eqd("wcstod trailing", call_wcstod("  3.5abc", &end), 3.5);
        check(end_offset(end) == 10u, "wcstod trailing end");

        eqd("wcstod invalid", call_wcstod("abc", &end), 0.0);
        check(end_offset(end) == 0u, "wcstod invalid end");
    }

    {
        static const uint16_t hello_w[] = {'h', 'e', 'l', 'l', 'o', 0};
        static const uint16_t invalid_first[] = {0x041F, 0};
        static const uint16_t invalid_mid[] = {'A', 0x0416, 'B', 0};
        static const uint16_t latin1[] = {0x00E9, 0};
        static const uint16_t ff[] = {0x00FF, 0};
        static const uint16_t over[] = {0x0100, 0};
        static const uint16_t surrogate[] = {0xD800, 0};
        static const uint16_t empty_w[] = {0};
        static const uint16_t one_w[] = {'A', 0};

        check(call_wcstombs(0u, hello_w, 6u, 0u) == 5u,
              "wcstombs null dst length");
        check(call_wcstombs(0u, hello_w, 6u, 8u) == 5u,
              "wcstombs null dst ignores count");
        check(call_wcstombs(0u, invalid_first, 2u, 8u) == 0xFFFFFFFFu,
              "wcstombs null dst invalid");
        check(call_wcstombs(0u, empty_w, 1u, 0u) == 0u,
              "wcstombs null dst empty");

        memset(guest_ptr(OUT_ADDR, 16u), 0xAA, 16u);
        check(call_wcstombs(OUT_ADDR, hello_w, 6u, 0u) == 0u,
              "wcstombs count zero return");
        check(wc3_legacy_rd8g(OUT_ADDR) == 0xAAu,
              "wcstombs count zero no write");

        memset(guest_ptr(OUT_ADDR, 16u), 0xAA, 16u);
        check(call_wcstombs(OUT_ADDR, hello_w, 6u, 5u) == 5u,
              "wcstombs exact count return");
        check(wc3_legacy_rd8g(OUT_ADDR) == 'h' &&
                  wc3_legacy_rd8g(OUT_ADDR + 4u) == 'o' &&
                  wc3_legacy_rd8g(OUT_ADDR + 5u) == 0xAAu,
              "wcstombs exact count bytes");

        memset(guest_ptr(OUT_ADDR, 16u), 0xAA, 16u);
        check(call_wcstombs(OUT_ADDR, hello_w, 6u, 6u) == 5u,
              "wcstombs terminated return");
        check(wc3_legacy_rd8g(OUT_ADDR) == 'h' &&
                  wc3_legacy_rd8g(OUT_ADDR + 4u) == 'o' &&
                  wc3_legacy_rd8g(OUT_ADDR + 5u) == 0u &&
                  wc3_legacy_rd8g(OUT_ADDR + 6u) == 0xAAu,
              "wcstombs terminated bytes");

        memset(guest_ptr(OUT_ADDR, 16u), 0xAA, 16u);
        check(call_wcstombs(OUT_ADDR, hello_w, 6u, 0xFFFFFFFFu) == 5u,
              "wcstombs max count return");
        check(wc3_legacy_rd8g(OUT_ADDR + 5u) == 0u,
              "wcstombs max count terminator");

        memset(guest_ptr(OUT_ADDR, 16u), 0xAA, 16u);
        check(call_wcstombs(OUT_ADDR, invalid_first, 2u, 8u) ==
                  0xFFFFFFFFu,
              "wcstombs invalid first return");
        check(wc3_legacy_rd8g(OUT_ADDR) == 0xAAu,
              "wcstombs invalid first no write");

        memset(guest_ptr(OUT_ADDR, 16u), 0xAA, 16u);
        check(call_wcstombs(OUT_ADDR, invalid_mid, 4u, 8u) == 0xFFFFFFFFu,
              "wcstombs invalid mid return");
        check(wc3_legacy_rd8g(OUT_ADDR) == 'A' &&
                  wc3_legacy_rd8g(OUT_ADDR + 1u) == 0xAAu,
              "wcstombs invalid mid partial write");

        memset(guest_ptr(OUT_ADDR, 16u), 0xAA, 16u);
        check(call_wcstombs(OUT_ADDR, latin1, 2u, 8u) == 1u,
              "wcstombs latin1 return");
        check(wc3_legacy_rd8g(OUT_ADDR) == 0xE9u &&
                  wc3_legacy_rd8g(OUT_ADDR + 1u) == 0u,
              "wcstombs latin1 bytes");

        memset(guest_ptr(OUT_ADDR, 16u), 0xAA, 16u);
        check(call_wcstombs(OUT_ADDR, ff, 2u, 8u) == 1u,
              "wcstombs ff return");
        check(wc3_legacy_rd8g(OUT_ADDR) == 0xFFu,
              "wcstombs ff byte");

        memset(guest_ptr(OUT_ADDR, 16u), 0xAA, 16u);
        check(call_wcstombs(OUT_ADDR, over, 2u, 8u) == 0xFFFFFFFFu,
              "wcstombs over return");
        check(wc3_legacy_rd8g(OUT_ADDR) == 0xAAu,
              "wcstombs over no write");
        check(call_wcstombs(OUT_ADDR, surrogate, 2u, 8u) == 0xFFFFFFFFu,
              "wcstombs surrogate return");

        memset(guest_ptr(OUT_ADDR, 16u), 0xAA, 16u);
        check(call_wcstombs(OUT_ADDR, empty_w, 1u, 0u) == 0u,
              "wcstombs empty count zero");
        check(wc3_legacy_rd8g(OUT_ADDR) == 0xAAu,
              "wcstombs empty count zero no write");
        check(call_wcstombs(OUT_ADDR, empty_w, 1u, 1u) == 0u,
              "wcstombs empty terminated");
        check(wc3_legacy_rd8g(OUT_ADDR) == 0u,
              "wcstombs empty terminator");

        const uint32_t last = GUEST_BASE + GUEST_SIZE - 1u;
        memset(guest_ptr(OUT_ADDR, 16u), 0xAA, 16u);
        check(call_wcstombs(last, one_w, 2u, 1u) == 1u,
              "wcstombs last dst return");
        check(wc3_legacy_rd8g(last) == 'A',
              "wcstombs last dst byte");
        check(call_wcstombs(last, one_w, 2u, 2u) == 0xFFFFFFFFu,
              "wcstombs last dst terminator return");
        check(wc3_legacy_rd8g(last) == 'A',
              "wcstombs last dst terminator no overrun");

        memset(guest_ptr(OUT_ADDR, 16u), 0xAA, 16u);
        wc3_legacy_wr16g(last - 1u, 0u);
        check(call_wcstombs_at(OUT_ADDR, last - 1u, 1u) == 0u,
              "wcstombs last source return");
        check(wc3_legacy_rd8g(OUT_ADDR) == 0u,
              "wcstombs last source terminator");
        memset(guest_ptr(OUT_ADDR, 16u), 0xAA, 16u);
        wc3_legacy_wr16g(last - 1u, 'A');
        check(call_wcstombs_at(OUT_ADDR, last - 1u, 2u) == 0xFFFFFFFFu,
              "wcstombs source overrun return");
        check(wc3_legacy_rd8g(OUT_ADDR) == 'A' &&
                  wc3_legacy_rd8g(OUT_ADDR + 1u) == 0xAAu,
              "wcstombs source overrun partial");
    }

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
