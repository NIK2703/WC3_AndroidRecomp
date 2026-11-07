/*
 * math_hle.c - target-side MSVCR80 floating-point imports.
 *
 * The as-built handlers in the quarantine are host-libm wrappers, but three
 * different guest ABIs are involved and a generic integer stub cannot serve
 * any of them:
 *
 *   - cdecl functions take two 32-bit words per double on the guest stack and
 *     leave the result on the x87 stack;
 *   - the _CI* helpers consume their operands from the x87 stack and push the
 *     result back, so a handler that only writes eax leaves ST(0) stale;
 *   - the _libm_sse2_* helpers use xmm0 (and xmm1 for binary operations) and
 *     return in xmm0.
 *
 * This module preserves the as-built arithmetic exactly; it does not claim
 * bit identity with the Windows CRT for transcendental functions.  It does
 * remove the shared integer default and keeps the x87 stack balanced for every
 * call, which is the T0 contract this slice can prove.
 *
 * _dtest is the CRT's internal double classifier used by isnan/isinf-style
 * checks in the game.  It reads the 64-bit value through a guest pointer and
 * returns _FINITE (-1), _DENORM (-2), _INFCODE (1) or _NANCODE (2), with zero
 * reported as 0.  The value is classified from its raw IEEE-754 bits so the
 * host's floating-point environment cannot change the answer.
 */
#include "math_hle.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"

/* The guest x87 register file is an exact 80-bit SoftFloat value.  The
 * conversion through a host double is the same narrowing the quarantine used,
 * so moving the handler does not add a second rounding step. */
static double pop_f64(CPU *c)
{
    float64_t f = extF80_to_f64(fpop(c));
    double d;
    memcpy(&d, &f.v, sizeof d);
    return d;
}

static void push_f64(CPU *c, double value)
{
    fpush(c, x87_from_f64(value));
}

/* cdecl double argument.  Each double occupies two 32-bit stack words, low
 * word first, starting at esp+4. */
static double arg_f64(const CPU *c, unsigned index)
{
    const uint64_t bits =
        (uint64_t)wc3_legacy_arg32(c, index * 2u)
        | ((uint64_t)wc3_legacy_arg32(c, index * 2u + 1u) << 32);
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

/* ---- x87 stack ABI ---------------------------------------------------- */

static void h_ci_sqrt(CPU *c)
{
    const double x = pop_f64(c);
    push_f64(c, sqrt(x));
}

static void h_ci_log10(CPU *c)
{
    const double x = pop_f64(c);
    push_f64(c, log10(x));
}

static void h_ci_pow(CPU *c)
{
    const double y = pop_f64(c);
    const double x = pop_f64(c);
    push_f64(c, pow(x, y));
}

static void h_ci_fmod(CPU *c)
{
    const double y = pop_f64(c);
    const double x = pop_f64(c);
    push_f64(c, fmod(x, y));
}

/* The caller leaves y in ST(1) and x in ST(0); fpatan replaces them with
 * atan2(y, x).  Reading stack arguments here would leave both inputs on the
 * x87 stack and corrupt every later calculation. */
static void h_ci_atan2(CPU *c)
{
    const double x = pop_f64(c);
    const double y = pop_f64(c);
    push_f64(c, atan2(y, x));
}

/* ---- cdecl double arguments, x87 result ------------------------------- */

#define MATH1(name, expr)                                                  \
    static void h_##name(CPU *c)                                           \
    {                                                                      \
        const double x = arg_f64(c, 0u);                                   \
        push_f64(c, (expr));                                               \
    }

#define MATH2(name, expr)                                                  \
    static void h_##name(CPU *c)                                           \
    {                                                                      \
        const double x = arg_f64(c, 0u);                                   \
        const double y = arg_f64(c, 1u);                                   \
        push_f64(c, (expr));                                               \
    }

MATH1(ceil, ceil(x))
MATH1(floor, floor(x))
MATH1(fabs, fabs(x))
MATH1(sqrt, sqrt(x))
MATH1(sin, sin(x))
MATH1(cos, cos(x))
MATH1(tan, tan(x))
MATH1(asin, asin(x))
MATH1(acos, acos(x))
MATH1(atan, atan(x))
MATH1(exp, exp(x))
MATH1(log, log(x))
MATH1(log10, log10(x))
MATH2(pow, pow(x, y))
MATH2(fmod, fmod(x, y))
MATH2(atan2, atan2(x, y))
MATH2(_hypot, hypot(x, y))

static void h_frexp(CPU *c)
{
    const double x = arg_f64(c, 0u);
    const uint32_t exponent = wc3_legacy_arg32(c, 2u);
    int value = 0;
    const double fraction = frexp(x, &value);

    if (exponent && wc3_legacy_guest_ok(exponent, 4))
        wc3_legacy_wr32g(exponent, (uint32_t)value);
    push_f64(c, fraction);
}

/* _dtest receives a guest pointer to a double, not the value itself.  The
 * CRT returns a short; EAX is sign-extended here to match the way MSVC callers
 * observe a negative short result. */
static void h__dtest(CPU *c)
{
    const uint32_t address = wc3_legacy_arg32(c, 0u);
    if (!address || !wc3_legacy_guest_ok(address, 8u)) {
        c->eax = 0u;
        return;
    }

    const uint32_t low = wc3_legacy_rd32g(address);
    const uint32_t high = wc3_legacy_rd32g(address + 4u);
    const uint32_t exponent = (high >> 20) & 0x7FFu;
    const uint64_t fraction =
        ((uint64_t)(high & 0x000FFFFFu) << 32) | (uint64_t)low;

    int32_t code;
    if (exponent == 0x7FFu)
        code = fraction ? 2 : 1;
    else if (exponent == 0u)
        code = fraction ? -2 : 0;
    else
        code = -1;

    c->eax = (uint32_t)code;
}

/* ---- SSE2 ABI --------------------------------------------------------- */

#define SSE2_1(name, expr)                                                 \
    static void h_sse2_##name(CPU *c)                                      \
    {                                                                      \
        const double x = c->xmm[0].f64[0];                                 \
        c->xmm[0].f64[0] = (expr);                                         \
    }

#define SSE2_2(name, expr)                                                 \
    static void h_sse2_##name(CPU *c)                                      \
    {                                                                      \
        const double x = c->xmm[0].f64[0];                                 \
        const double y = c->xmm[1].f64[0];                                 \
        c->xmm[0].f64[0] = (expr);                                         \
    }

SSE2_1(sqrt, sqrt(x))
SSE2_1(tan, tan(x))
SSE2_1(log10, log10(x))
SSE2_1(log, log(x))
SSE2_1(exp, exp(x))
SSE2_1(sin, sin(x))
SSE2_1(cos, cos(x))
SSE2_1(atan, atan(x))
SSE2_1(asin, asin(x))
SSE2_1(acos, acos(x))
SSE2_2(pow, pow(x, y))
SSE2_2(atan2, atan2(x, y))

typedef struct MathOverride {
    const char *name;
    void (*impl)(CPU *c);
} MathOverride;

/* Names here must match the as-built MSVCR80 rows exactly.  The table keeps
 * the call convention and stack cleanup; only the implementation is replaced. */
static const MathOverride g_overrides[] = {
    { "_CIsqrt", h_ci_sqrt },
    { "_CIlog10", h_ci_log10 },
    { "_CIpow", h_ci_pow },
    { "_CIfmod", h_ci_fmod },
    { "_CIatan2", h_ci_atan2 },

    { "ceil", h_ceil },
    { "floor", h_floor },
    { "fabs", h_fabs },
    { "sqrt", h_sqrt },
    { "sin", h_sin },
    { "cos", h_cos },
    { "tan", h_tan },
    { "asin", h_asin },
    { "acos", h_acos },
    { "atan", h_atan },
    { "exp", h_exp },
    { "log", h_log },
    { "log10", h_log10 },
    { "pow", h_pow },
    { "fmod", h_fmod },
    { "atan2", h_atan2 },

    { "_libm_sse2_sqrt_precise", h_sse2_sqrt },
    { "_libm_sse2_tan_precise", h_sse2_tan },
    { "_libm_sse2_log10_precise", h_sse2_log10 },
    { "_libm_sse2_log_precise", h_sse2_log },
    { "_libm_sse2_exp_precise", h_sse2_exp },
    { "_libm_sse2_sin_precise", h_sse2_sin },
    { "_libm_sse2_cos_precise", h_sse2_cos },
    { "_libm_sse2_atan_precise", h_sse2_atan },
    { "_libm_sse2_asin_precise", h_sse2_asin },
    { "_libm_sse2_acos_precise", h_sse2_acos },
    { "_libm_sse2_pow_precise", h_sse2_pow },
    { "_libm_sse2_atan2_precise", h_sse2_atan2 },
};

typedef struct MathAddition {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} MathAddition;

static const MathAddition g_additions[] = {
    { "_hypot", 4, h__hypot },
    { "frexp", 3, h_frexp },
    { "_dtest", 1, h__dtest },
};

int wc3_hle_msvcr80_math_install(void)
{
    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("MSVCR80.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("MSVCR80.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 1,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0] +
                 sizeof g_additions / sizeof g_additions[0]);
}
