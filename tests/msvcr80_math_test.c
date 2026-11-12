/*
 * msvcr80_math_test.c - T0 contract test for the MSVCR80 math slice.
 *
 * The test drives all three guest ABIs directly:
 *   - _CI* helpers consume and produce x87 stack values;
 *   - cdecl functions read two 32-bit words per double from the guest stack;
 *   - _libm_sse2_* helpers use xmm0/xmm1 and return in xmm0.
 *
 * The fake shim records the row names and supplies the cdecl argument words.
 * Expected values are exact where possible; transcendental cases use the same
 * host libm the as-built HLE used, so the test pins ABI and argument order rather
 * than claiming Windows CRT bit identity.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "math_hle.h"
#include "hle_bridge.h"

#define MAX_ROWS 64u
#define EXPECTED_ROWS 36u
#define EXP_ADDR 0x1000u
#define DTEST_ADDR 0x2000u

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
} Row;

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;
static uint32_t g_args[16];
static uint32_t g_exponent;
static uint32_t g_dtest_bits[2];
static int g_failures;

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
    if (!dll || !name || !impl) return -1;
    if (g_row_count >= MAX_ROWS) return -1;
    g_rows[g_row_count].dll = dll;
    g_rows[g_row_count].name = name;
    g_rows[g_row_count].impl = impl;
    g_row_count++;
    return 0;
}

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    if (addr == EXP_ADDR && size <= 4u) return 1;
    if (addr == DTEST_ADDR && size <= 8u) return 1;
    return 0;
}

void wc3_legacy_wr32g(uint32_t addr, uint32_t value)
{
    if (addr == EXP_ADDR) g_exponent = value;
}

uint32_t wc3_legacy_rd32g(uint32_t addr)
{
    if (addr == DTEST_ADDR) return g_dtest_bits[0];
    if (addr == DTEST_ADDR + 4u) return g_dtest_bits[1];
    return 0;
}

uint32_t wc3_legacy_arg32(const CPU *c, unsigned index)
{
    (void)c;
    return index < 16u ? g_args[index] : 0u;
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
        printf("FAIL %-28s got %.17g (0x%016llX) want %.17g (0x%016llX)\n",
               what, got, (unsigned long long)g, want,
               (unsigned long long)w);
        g_failures++;
    }
}

static void (*find_impl(const char *name))(CPU *c)
{
    for (unsigned i = 0; i < g_row_count; i++)
        if (strcmp(g_rows[i].name, name) == 0) return g_rows[i].impl;
    return NULL;
}

static void init_cpu(CPU *c)
{
    memset(c, 0, sizeof *c);
    cpu_x87_init(c);
}

static void set_arg_f64(unsigned index, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    g_args[index * 2u] = (uint32_t)bits;
    g_args[index * 2u + 1u] = (uint32_t)(bits >> 32);
}

static void clear_args(void)
{
    memset(g_args, 0, sizeof g_args);
}

static double pop_x87(CPU *c)
{
    return fpop(c).v.d;
}

static double call_x87_1(const char *name, double x)
{
    void (*impl)(CPU *c) = find_impl(name);
    if (!impl) {
        printf("FAIL: missing handler %s\n", name);
        g_failures++;
        return NAN;
    }
    CPU c;
    init_cpu(&c);
    fpush(&c, x87_from_f64(x));
    impl(&c);
    const double result = pop_x87(&c);
    check(c.fpu_top == 0, "x87 stack not balanced after unary call");
    check(c.fpu_tag == 0xFFFFu, "x87 tags not empty after unary call");
    return result;
}

/* The first value is pushed first and therefore ends up in ST(1); the second
 * value is ST(0).  _CIatan2 and _CIpow care about that distinction. */
static double call_x87_2(const char *name, double first, double second)
{
    void (*impl)(CPU *c) = find_impl(name);
    if (!impl) {
        printf("FAIL: missing handler %s\n", name);
        g_failures++;
        return NAN;
    }
    CPU c;
    init_cpu(&c);
    fpush(&c, x87_from_f64(first));
    fpush(&c, x87_from_f64(second));
    impl(&c);
    const double result = pop_x87(&c);
    check(c.fpu_top == 0, "x87 stack not balanced after binary call");
    check(c.fpu_tag == 0xFFFFu, "x87 tags not empty after binary call");
    return result;
}

static double call_cdecl_1(const char *name, double x)
{
    void (*impl)(CPU *c) = find_impl(name);
    if (!impl) {
        printf("FAIL: missing handler %s\n", name);
        g_failures++;
        return NAN;
    }
    clear_args();
    set_arg_f64(0u, x);
    CPU c;
    init_cpu(&c);
    impl(&c);
    return pop_x87(&c);
}

static double call_cdecl_2(const char *name, double x, double y)
{
    void (*impl)(CPU *c) = find_impl(name);
    if (!impl) {
        printf("FAIL: missing handler %s\n", name);
        g_failures++;
        return NAN;
    }
    clear_args();
    set_arg_f64(0u, x);
    set_arg_f64(1u, y);
    CPU c;
    init_cpu(&c);
    impl(&c);
    return pop_x87(&c);
}

static double call_frexp(double x, uint32_t exponent_addr)
{
    void (*impl)(CPU *c) = find_impl("frexp");
    if (!impl) {
        printf("FAIL: missing handler frexp\n");
        g_failures++;
        return NAN;
    }
    clear_args();
    set_arg_f64(0u, x);
    g_args[2] = exponent_addr;
    CPU c;
    init_cpu(&c);
    impl(&c);
    return pop_x87(&c);
}

static uint32_t call_dtest(uint64_t bits)
{
    void (*impl)(CPU *c) = find_impl("_dtest");
    if (!impl) {
        printf("FAIL: missing handler _dtest\n");
        g_failures++;
        return 0xDEADBEEFu;
    }
    g_dtest_bits[0] = (uint32_t)bits;
    g_dtest_bits[1] = (uint32_t)(bits >> 32);
    clear_args();
    g_args[0] = DTEST_ADDR;
    CPU c;
    init_cpu(&c);
    impl(&c);
    return c.eax;
}

static double call_sse2_1(const char *name, double x)
{
    void (*impl)(CPU *c) = find_impl(name);
    if (!impl) {
        printf("FAIL: missing handler %s\n", name);
        g_failures++;
        return NAN;
    }
    CPU c;
    init_cpu(&c);
    c.xmm[0].f64[0] = x;
    impl(&c);
    return c.xmm[0].f64[0];
}

static double call_sse2_2(const char *name, double x, double y)
{
    void (*impl)(CPU *c) = find_impl(name);
    if (!impl) {
        printf("FAIL: missing handler %s\n", name);
        g_failures++;
        return NAN;
    }
    CPU c;
    init_cpu(&c);
    c.xmm[0].f64[0] = x;
    c.xmm[1].f64[0] = y;
    impl(&c);
    return c.xmm[0].f64[0];
}

int main(void)
{
    static const char *const expected[] = {
        "_CIsqrt", "_CIlog10", "_CIpow", "_CIfmod", "_CIatan2",
        "ceil", "floor", "fabs", "sqrt", "sin", "cos", "tan",
        "asin", "acos", "atan", "exp", "log", "log10", "pow", "fmod",
        "atan2",
        "_libm_sse2_sqrt_precise", "_libm_sse2_tan_precise",
        "_libm_sse2_log10_precise", "_libm_sse2_log_precise",
        "_libm_sse2_exp_precise", "_libm_sse2_sin_precise",
        "_libm_sse2_cos_precise", "_libm_sse2_atan_precise",
        "_libm_sse2_asin_precise", "_libm_sse2_acos_precise",
        "_libm_sse2_pow_precise", "_libm_sse2_atan2_precise",
        "_hypot", "frexp", "_dtest",
    };

    const int installed = wc3_hle_msvcr80_math_install();
    check(installed == (int)EXPECTED_ROWS, "install count");
    check(g_row_count == EXPECTED_ROWS, "recorded row count");
    check(sizeof expected / sizeof expected[0] == EXPECTED_ROWS,
          "expected name count");
    for (unsigned i = 0; i < EXPECTED_ROWS; i++) {
        check(find_impl(expected[i]) != NULL, expected[i]);
        for (unsigned j = 0; j < g_row_count; j++) {
            if (strcmp(g_rows[j].name, expected[i]) == 0)
                check(strcmp(g_rows[j].dll, "MSVCR80.dll") == 0, "DLL name");
        }
    }

    /* x87 helpers: operands are ST(1), ST(0). */
    eqd("_CIsqrt", call_x87_1("_CIsqrt", 6.25), 2.5);
    eqd("_CIlog10", call_x87_1("_CIlog10", 100.0), 2.0);
    eqd("_CIpow", call_x87_2("_CIpow", 2.0, 10.0), 1024.0);
    eqd("_CIfmod", call_x87_2("_CIfmod", 7.0, 3.0), 1.0);
    eqd("_CIatan2", call_x87_2("_CIatan2", 1.0, 0.0),
        atan2(1.0, 0.0));

    /* cdecl math: double words on the guest stack, x87 result. */
    eqd("ceil", call_cdecl_1("ceil", 2.25), 3.0);
    eqd("floor", call_cdecl_1("floor", 2.75), 2.0);
    eqd("fabs", call_cdecl_1("fabs", -3.5), 3.5);
    eqd("sqrt", call_cdecl_1("sqrt", 6.25), 2.5);
    eqd("sin", call_cdecl_1("sin", 0.0), 0.0);
    eqd("cos", call_cdecl_1("cos", 0.0), 1.0);
    eqd("tan", call_cdecl_1("tan", 0.0), 0.0);
    eqd("asin", call_cdecl_1("asin", 0.0), 0.0);
    eqd("acos", call_cdecl_1("acos", 1.0), 0.0);
    eqd("atan", call_cdecl_1("atan", 0.0), 0.0);
    eqd("exp", call_cdecl_1("exp", 0.0), 1.0);
    eqd("log", call_cdecl_1("log", 1.0), 0.0);
    eqd("log10", call_cdecl_1("log10", 100.0), 2.0);
    eqd("pow", call_cdecl_2("pow", 2.0, 10.0), 1024.0);
    eqd("fmod", call_cdecl_2("fmod", 7.0, 3.0), 1.0);
    eqd("atan2", call_cdecl_2("atan2", 1.0, 0.0), atan2(1.0, 0.0));

    eqd("_hypot", call_cdecl_2("_hypot", 3.0, 4.0), 5.0);
    eqd("_hypot zero", call_cdecl_2("_hypot", 0.0, 0.0), 0.0);

    {
        g_exponent = 0xDEADBEEFu;
        eqd("frexp 6.25", call_frexp(6.25, EXP_ADDR), 0.78125);
        check(g_exponent == 3u, "frexp exponent 6.25");

        g_exponent = 0xDEADBEEFu;
        eqd("frexp zero", call_frexp(0.0, EXP_ADDR), 0.0);
        check(g_exponent == 0u, "frexp exponent zero");

        g_exponent = 0xDEADBEEFu;
        eqd("frexp negative", call_frexp(-0.75, EXP_ADDR), -0.75);
        check(g_exponent == 0u, "frexp exponent negative");

        g_exponent = 0xDEADBEEFu;
        eqd("frexp bad pointer", call_frexp(6.25, 0xFFFFu), 0.78125);
        check(g_exponent == 0xDEADBEEFu, "frexp bad pointer untouched");
    }

    /* _dtest classifies the double pointed to by the guest argument. */
    check(call_dtest(0x0000000000000000ULL) == 0u, "_dtest +0");
    check(call_dtest(0x8000000000000000ULL) == 0u, "_dtest -0");
    check(call_dtest(0x3FF0000000000000ULL) == 0xFFFFFFFFu,
          "_dtest finite");
    check(call_dtest(0x7FEFFFFFFFFFFFFFULL) == 0xFFFFFFFFu,
          "_dtest dbl_max");
    check(call_dtest(0x0010000000000000ULL) == 0xFFFFFFFFu,
          "_dtest dbl_min");
    check(call_dtest(0x000FFFFFFFFFFFFFULL) == 0xFFFFFFFEu,
          "_dtest largest subnormal");
    check(call_dtest(0x0000000000000001ULL) == 0xFFFFFFFEu,
          "_dtest smallest subnormal");
    check(call_dtest(0x7FF0000000000000ULL) == 1u, "_dtest +inf");
    check(call_dtest(0xFFF0000000000000ULL) == 1u, "_dtest -inf");
    check(call_dtest(0x7FF8000000000000ULL) == 2u, "_dtest qnan");
    check(call_dtest(0xFFF8000000000000ULL) == 2u, "_dtest -qnan");
    check(call_dtest(0x7FF0000000000001ULL) == 2u, "_dtest snan");

    clear_args();
    g_args[0] = 0xFFFFu;
    {
        void (*impl)(CPU *c) = find_impl("_dtest");
        CPU c;
        init_cpu(&c);
        impl(&c);
        check(c.eax == 0u, "_dtest bad pointer");
    }

    /* SSE2 helpers: xmm0 (and xmm1) in, xmm0 out. */
    eqd("sse2 sqrt", call_sse2_1("_libm_sse2_sqrt_precise", 6.25), 2.5);
    eqd("sse2 tan", call_sse2_1("_libm_sse2_tan_precise", 0.0), 0.0);
    eqd("sse2 log10", call_sse2_1("_libm_sse2_log10_precise", 100.0), 2.0);
    eqd("sse2 log", call_sse2_1("_libm_sse2_log_precise", 1.0), 0.0);
    eqd("sse2 exp", call_sse2_1("_libm_sse2_exp_precise", 0.0), 1.0);
    eqd("sse2 sin", call_sse2_1("_libm_sse2_sin_precise", 0.0), 0.0);
    eqd("sse2 cos", call_sse2_1("_libm_sse2_cos_precise", 0.0), 1.0);
    eqd("sse2 atan", call_sse2_1("_libm_sse2_atan_precise", 0.0), 0.0);
    eqd("sse2 asin", call_sse2_1("_libm_sse2_asin_precise", 0.0), 0.0);
    eqd("sse2 acos", call_sse2_1("_libm_sse2_acos_precise", 1.0), 0.0);
    eqd("sse2 pow", call_sse2_2("_libm_sse2_pow_precise", 2.0, 10.0),
        1024.0);
    eqd("sse2 atan2", call_sse2_2("_libm_sse2_atan2_precise", 1.0, 0.0),
        atan2(1.0, 0.0));

    if (g_failures) {
        printf("msvcr80 math FAILED (%d checks)\n", g_failures);
        return 1;
    }
    printf("msvcr80 math OK (%d handlers)\n", installed);
    return 0;
}
