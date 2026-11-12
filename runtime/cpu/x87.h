/*
 * x87.h - native x87 model for the CPU-struct recompiler.
 *
 * The register file is a 16-byte slot shared by x87 and MMX.  x87 arithmetic
 * uses the low 64 bits as a native `double`; MMX uses the same low 64 bits as
 * an integer register.  The upper 8 bytes are padding so CPU.st[8] keeps the
 * layout the generated C expects.
 *
 * This is deliberately not a bit-exact x87.  It follows FEX's
 * X87ReducedPrecision model: 53-bit double arithmetic, PC ignored, f80 only at
 * the memory/save boundary, and only IE plus C0-C3 are tracked.  The accepted
 * deviations are recorded in docs/ADR.md (ADR-037) and docs/EXACTNESS.md E4.1.
 *
 * The generated code calls only the helpers below.  Keep that boundary:
 * generated C uses x87_ext, fst/fpush/fpop and these helpers, never the union
 * members directly.
 */
#ifndef PCRECOMP_X87_H
#define PCRECOMP_X87_H

#include <fenv.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(x87_ext) == 16, "x87 slot must stay 16 bytes");
_Static_assert(offsetof(x87_ext, v) == 0, "x87 value must be at offset 0");

/* ---- host floating-point environment ----------------------------------- */

/* RC is copied to the host only when the guest CW changes.  x87 arithmetic
 * itself is native double; doing a save/set/restore around every operation
 * would reintroduce the cost this path removes. */
static inline int x87_host_round(uint16_t cw) {
    switch ((cw >> 10) & 3u) {
    case 0:  return FE_TONEAREST;
    case 1:  return FE_DOWNWARD;
    case 2:  return FE_UPWARD;
    default: return FE_TOWARDZERO;
    }
}
#if defined(__aarch64__)
static inline void x87_clear_ftz(void) {
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr &= ~((UINT64_C(1) << 24) | (UINT64_C(1) << 25)); /* FZ, DN */
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
}
#else
static inline void x87_clear_ftz(void) { }
#endif
static inline void x87_apply_rounding(CPU *c) {
    (void)fesetround(x87_host_round(c->fpu_cw));
}

/* ---- bit classification ------------------------------------------------- */

static inline uint64_t x87_bits(double d) {
    uint64_t b;
    memcpy(&b, &d, sizeof b);
    return b;
}
static inline double x87_from_bits(uint64_t b) {
    double d;
    memcpy(&d, &b, sizeof d);
    return d;
}
static inline int x87_is_zero_bits(uint64_t b) {
    return (b & UINT64_C(0x7FFFFFFFFFFFFFFF)) == 0;
}
static inline int x87_is_inf_bits(uint64_t b) {
    return (b & UINT64_C(0x7FFFFFFFFFFFFFFF)) == UINT64_C(0x7FF0000000000000);
}
static inline int x87_is_nan_bits(uint64_t b) {
    return (b & UINT64_C(0x7FF0000000000000)) == UINT64_C(0x7FF0000000000000)
        && (b & UINT64_C(0x000FFFFFFFFFFFFF)) != 0;
}
static inline int x87_is_finite_bits(uint64_t b) {
    return (b & UINT64_C(0x7FF0000000000000)) != UINT64_C(0x7FF0000000000000);
}
static inline double x87_quiet_nan(double d) {
    return x87_from_bits(x87_bits(d) | UINT64_C(0x0008000000000000));
}
/* A signalling NaN has the quiet bit clear and a non-zero payload.  The
 * exponent check keeps infinity from being treated as a NaN. */
static inline int x87_is_snan_bits(uint64_t b) {
    return (b & UINT64_C(0x7FF0000000000000)) == UINT64_C(0x7FF0000000000000)
        && (b & UINT64_C(0x000FFFFFFFFFFFFF)) != 0
        && (b & UINT64_C(0x0008000000000000)) == 0;
}

/* IE for arithmetic without per-op SNaN/Inf tests on the common path.  x87
 * invalid arithmetic always produces a NaN; when it does, classify the inputs
 * (QNaN propagation is not invalid, SNaN and non-NaN invalid cases are). */
static inline int x87_invalid_result(double r, double a, double b) {
    if (!isnan(r)) return 0;
    const uint64_t ab = x87_bits(a), bb = x87_bits(b);
    return x87_is_snan_bits(ab) || x87_is_snan_bits(bb)
        || (!x87_is_nan_bits(ab) && !x87_is_nan_bits(bb));
}

/* ---- f80 memory codec --------------------------------------------------- */

/* double -> 80-bit extended: explicit integer bit, sign in bit 15 of signExp.
 * Normal values are exact; the low 11 bits of a wider x87 significand do not
 * exist in a double and are therefore zero. */
static inline void x87_d_to_f80(double d, uint64_t *sig, uint16_t *se) {
    const uint64_t b = x87_bits(d);
    const uint16_t sign = (uint16_t)(((b >> 48) & UINT64_C(0x8000)));
    const uint16_t exp = (uint16_t)((b >> 52) & 0x7FFu);
    const uint64_t frac = b & UINT64_C(0x000FFFFFFFFFFFFF);

    if (exp == 0x7FFu) {
        *se = (uint16_t)(sign | 0x7FFFu);
        *sig = frac ? UINT64_C(0xC000000000000000)
                    : UINT64_C(0x8000000000000000);
        return;
    }
    if (exp != 0) {
        *sig = UINT64_C(0x8000000000000000) | (frac << 11);
        *se = (uint16_t)(sign | (exp + 15360u));
        return;
    }
    if (!frac) {
        *sig = 0;
        *se = sign;
        return;
    }
    {
        int k = 52;
        while (!(frac & (UINT64_C(1) << k))) k--;
        *sig = UINT64_C(0x8000000000000000) | (frac << (63 - k));
        *se = (uint16_t)(sign | (unsigned)(k + 15309));
    }
}

/* 80-bit extended -> double.  f80 denormals and unsupported encodings are
 * outside the double range or are not representable, so they become zero with
 * the sign preserved.  NaN becomes a canonical quiet NaN; Inf stays Inf. */
static inline double x87_f80_to_d(uint64_t sig, uint16_t se) {
    const uint16_t sign = se & 0x8000u;
    const uint16_t exp = se & 0x7FFFu;

    if (exp == 0x7FFFu) {
        if (sig == UINT64_C(0x8000000000000000))
            return sign ? -INFINITY : INFINITY;
        if (!(sig & UINT64_C(0x8000000000000000)))
            return sign ? -0.0 : 0.0;       /* pseudo-NaN */
        return x87_from_bits((UINT64_C(0x7FF8000000000000))
                             | ((uint64_t)(sign != 0) << 63));
    }
    if (exp == 0 || !(sig & UINT64_C(0x8000000000000000)))
        return sign ? -0.0 : 0.0;           /* denormal/unnormal/pseudo */
    {
        double d = ldexp((double)sig, (int)exp - 16446);
        return sign ? -d : d;
    }
}

static inline x87_ext x87_from_f64(double d) {
    x87_ext v;
    v.v.d = d;
    v.reserved = 0;
    return v;
}
static inline x87_ext x87_indefinite(void) {
    return x87_from_f64(x87_from_bits(UINT64_C(0xFFF8000000000000)));
}

/* ---- constants ---------------------------------------------------------- */

static inline x87_ext x87_zero(void) { return x87_from_f64(0.0); }
static inline x87_ext x87_one(void)  { return x87_from_f64(1.0); }
static inline x87_ext x87_pi(void)   { return x87_from_f64(0x1.921fb54442d18p+1); }
static inline x87_ext x87_l2e(void)  { return x87_from_f64(0x1.71547652b82fep+0); }
static inline x87_ext x87_l2t(void)  { return x87_from_f64(0x1.a934f0979a371p+1); }
static inline x87_ext x87_lg2(void)  { return x87_from_f64(0x1.34413509f79ffp-2); }
static inline x87_ext x87_ln2(void)  { return x87_from_f64(0x1.62e42fefa39efp-1); }

static inline x87_ext x87_from_i16(int16_t i) { return x87_from_f64((double)i); }
static inline x87_ext x87_from_i32(int32_t i) { return x87_from_f64((double)i); }
static inline x87_ext x87_from_i64(int64_t i) { return x87_from_f64((double)i); }

/* ---- control/status ----------------------------------------------------- */

#define X87_IE  0x0001u
#define X87_C0  0x0100u
#define X87_C1  0x0200u
#define X87_C2  0x0400u
#define X87_C3  0x4000u

static inline void x87_raise_invalid(CPU *c) {
    c->fpu_sw |= X87_IE;
}
static inline void x87_set_top(CPU *c) {
    c->fpu_sw = (c->fpu_sw & ~0x3800u) | ((uint32_t)(c->fpu_top & 7) << 11);
}
static inline void x87_stack_fault(CPU *c) {
    c->fpu_sw |= X87_IE | 0x0040u;   /* IE | SF */
    c->fpu_sw |= X87_C1;
}
static inline void x87_set_c(CPU *c, uint32_t c3, uint32_t c2, uint32_t c0) {
    c->fpu_sw = (c->fpu_sw & ~0x4700u)
              | (c3 ? X87_C3 : 0) | (c2 ? X87_C2 : 0) | (c0 ? X87_C0 : 0);
}
static inline void x87_fnclex(CPU *c) {
    c->fpu_sw &= ~0x00FFu;   /* exception flags, ES, and stack fault */
}
static inline void x87_fldcw(CPU *c, uint32_t a) {
    const uint16_t cw = rd16(a);
    if (cw == c->fpu_cw) return;
    c->fpu_cw = cw;
    x87_apply_rounding(c);
}

/* ---- tag word ----------------------------------------------------------- */
/* Two bits per physical register: 0 valid, 1 zero, 2 special, 3 empty. */
static inline uint16_t x87_tag(const CPU *c, int p) {
    return (uint16_t)((c->fpu_tag >> (p * 2)) & 3u);
}
static inline void x87_set_tag(CPU *c, int p, uint16_t t) {
    c->fpu_tag = (uint16_t)((c->fpu_tag & ~(3u << (p * 2)))
                          | ((t & 3u) << (p * 2)));
}
static inline uint16_t x87_tag_for(x87_ext v) {
    const uint64_t b = x87_bits(v.v.d);
    if (x87_is_zero_bits(b)) return 1;
    if ((b & UINT64_C(0x7FF0000000000000)) == UINT64_C(0x7FF0000000000000))
        return 2;
    return 0;
}
/* Arithmetic helpers write .v.d directly, so the cached tag may still say
 * "zero" after a nonzero result.  Tags only gate empty/valid and the saved tag
 * word; refresh them at the environment boundary, not in the hot path. */
static inline void x87_refresh_tags(CPU *c) {
    for (int p = 0; p < 8; p++)
        if (x87_tag(c, p) != 3)
            x87_set_tag(c, p, x87_tag_for(c->st[p]));
}

/* ---- stack -------------------------------------------------------------- */

static inline void fpush(CPU *c, x87_ext v) {
    const int p = (c->fpu_top - 1) & 7;
    if (x87_tag(c, p) != 3) x87_stack_fault(c);   /* stack overflow */
    c->fpu_top = p;
    c->st[p] = v;
    x87_set_tag(c, p, x87_tag_for(v));
    x87_set_top(c);
}
static inline x87_ext fpop(CPU *c) {
    const int p = c->fpu_top;
    const x87_ext v = c->st[p];
    if (x87_tag(c, p) == 3) x87_stack_fault(c);   /* stack underflow */
    x87_set_tag(c, p, 3);
    c->fpu_top = (c->fpu_top + 1) & 7;
    x87_set_top(c);
    return v;
}
static inline x87_ext *fst(CPU *c, int i) {
    return &c->st[(c->fpu_top + i) & 7];
}
/* FST ST(i), ST(0) writes the destination register and its tag.  A plain slot
 * assignment would leave an empty destination tag empty and make a later
 * access see a stack fault. */
static inline void x87_fst_reg(CPU *c, int i) {
    const int d = (c->fpu_top + i) & 7;
    const int s = c->fpu_top;
    c->st[d] = c->st[s];
    x87_set_tag(c, d, x87_tag(c, s));
}
static inline void x87_fxch(CPU *c, int i) {
    const int a = c->fpu_top, b = (c->fpu_top + i) & 7;
    const x87_ext t = c->st[a];
    const uint16_t tt = x87_tag(c, a);
    c->st[a] = c->st[b]; x87_set_tag(c, a, x87_tag(c, b));
    c->st[b] = t;        x87_set_tag(c, b, tt);
}
static inline void x87_ffree(CPU *c, int i) {
    x87_set_tag(c, (c->fpu_top + i) & 7, 3);
}
static inline void x87_fdecstp(CPU *c) {
    c->fpu_top = (c->fpu_top - 1) & 7; x87_set_top(c);
}
static inline void x87_fincstp(CPU *c) {
    c->fpu_top = (c->fpu_top + 1) & 7; x87_set_top(c);
}
static inline void x87_fcmov(CPU *c, int cond, int i) {
    if (!cond) return;
    const int s = (c->fpu_top + i) & 7;
    if (x87_tag(c, s) == 3) { x87_stack_fault(c); return; }
    c->st[c->fpu_top] = c->st[s];
    x87_set_tag(c, c->fpu_top, x87_tag(c, s));
}
static inline void x87_fchs(CPU *c) {
    (*fst(c, 0)).v.mm ^= UINT64_C(0x8000000000000000);
}
static inline void x87_fabs(CPU *c) {
    (*fst(c, 0)).v.mm &= UINT64_C(0x7FFFFFFFFFFFFFFF);
}

/* ---- memory loads/stores ------------------------------------------------ */

static inline x87_ext x87_ld_f32(uint32_t a) {
    float f;
    memcpy(&f, (void *)(uintptr_t)a, 4);
    return x87_from_f64((double)f);
}
static inline x87_ext x87_ld_f64(uint32_t a) {
    double d;
    memcpy(&d, (void *)(uintptr_t)a, 8);
    return x87_from_f64(d);
}
static inline x87_ext x87_ld_f80(uint32_t a) {
    uint64_t sig;
    uint16_t se;
    memcpy(&sig, (void *)(uintptr_t)a, 8);
    memcpy(&se, (void *)((uintptr_t)a + 8), 2);
    return x87_from_f64(x87_f80_to_d(sig, se));
}
static inline x87_ext x87_ld_i16(uint32_t a) {
    int16_t i; memcpy(&i, (void *)(uintptr_t)a, 2); return x87_from_i16(i);
}
static inline x87_ext x87_ld_i32(uint32_t a) {
    int32_t i; memcpy(&i, (void *)(uintptr_t)a, 4); return x87_from_i32(i);
}
static inline x87_ext x87_ld_i64(uint32_t a) {
    int64_t i; memcpy(&i, (void *)(uintptr_t)a, 8); return x87_from_i64(i);
}
static inline void x87_st_f32(CPU *c, uint32_t a, x87_ext v) {
    (void)c;   /* SNaN handling at the m32 boundary is accepted as a deviation */
    float f = (float)v.v.d;
    memcpy((void *)(uintptr_t)a, &f, 4);
}
static inline void x87_st_f64(CPU *c, uint32_t a, x87_ext v) {
    (void)c;
    memcpy((void *)(uintptr_t)a, &v.v.d, 8);
}
static inline void x87_st_f80(uint32_t a, x87_ext v) {
    uint64_t sig;
    uint16_t se;
    x87_d_to_f80(v.v.d, &sig, &se);
    memcpy((void *)(uintptr_t)a, &sig, 8);
    memcpy((void *)((uintptr_t)a + 8), &se, 2);
}

#define X87_DEFINE_STORE_INT(fn, type, minv, maxv, indef, truncate)           \
static inline void fn(CPU *c, uint32_t a, x87_ext v) {                        \
    type i;                                                                    \
    if (!x87_is_finite_bits(x87_bits(v.v.d))) {                                \
        x87_raise_invalid(c); i = (type)(indef);                               \
    } else {                                                                   \
        const double r = (truncate) ? trunc(v.v.d) : nearbyint(v.v.d);         \
        if (!(r >= (minv) && r < (maxv))) {                                    \
            x87_raise_invalid(c); i = (type)(indef);                           \
        } else i = (type)r;                                                    \
    }                                                                          \
    memcpy((void *)(uintptr_t)a, &i, sizeof i);                                \
}
X87_DEFINE_STORE_INT(x87_st_i16,    int16_t, -32768.0, 32768.0, 0x8000, 0)
X87_DEFINE_STORE_INT(x87_st_i16_rz, int16_t, -32768.0, 32768.0, 0x8000, 1)
X87_DEFINE_STORE_INT(x87_st_i32,    int32_t, -2147483648.0, 2147483648.0,
                     0x80000000u, 0)
X87_DEFINE_STORE_INT(x87_st_i32_rz, int32_t, -2147483648.0, 2147483648.0,
                     0x80000000u, 1)
X87_DEFINE_STORE_INT(x87_st_i64,    int64_t, -9223372036854775808.0,
                     9223372036854775808.0, INT64_MIN, 0)
X87_DEFINE_STORE_INT(x87_st_i64_rz, int64_t, -9223372036854775808.0,
                     9223372036854775808.0, INT64_MIN, 1)
#undef X87_DEFINE_STORE_INT

/* ---- packed BCD --------------------------------------------------------- */

static inline x87_ext x87_ld_bcd(CPU *c, uint32_t a) {
    uint8_t b[10];
    double value = 0.0;
    (void)c;
    memcpy(b, (void *)(uintptr_t)a, sizeof b);
    /* Invalid BCD digits have an undefined result; masking the nibbles is a
     * deterministic integer codec and, unlike IE, matches the no-exception
     * behaviour of the hardware. */
    for (int i = 17; i >= 0; i--) {
        const unsigned digit = (i & 1) ? (b[i >> 1] >> 4) : (b[i >> 1] & 0x0Fu);
        value = value * 10.0 + (double)digit;
    }
    if (b[9] & 0x80u) value = -value;
    return x87_from_f64(value);
}
static inline void x87_bcd_indefinite(uint32_t a) {
    uint8_t b[10];
    memset(b, 0, sizeof b);
    b[7] = 0xC0u;                 /* invalid packed BCD: 00..00 C0 FF FF */
    b[8] = b[9] = 0xFFu;
    memcpy((void *)(uintptr_t)a, b, sizeof b);
}
static inline void x87_st_bcd(CPU *c, uint32_t a, x87_ext v) {
    uint8_t b[10];
    const double d = v.v.d;
    double r;
    uint64_t n;

    /* NaN and Inf fail both comparisons.  The valid packed range is
     * -999999999999999999 .. +999999999999999999. */
    if (!(d > -1.0e18 && d < 1.0e18)) {
        x87_raise_invalid(c);
        x87_bcd_indefinite(a);
        return;
    }
    r = nearbyint(d);
    if (!(r > -1.0e18 && r < 1.0e18)) {
        x87_raise_invalid(c);
        x87_bcd_indefinite(a);
        return;
    }
    n = (uint64_t)(r < 0.0 ? -r : r);
    for (int i = 0; i < 9; i++) {
        b[i] = (uint8_t)((n % 10u) | (((n / 10u) % 10u) << 4));
        n /= 100u;
    }
    b[9] = (uint8_t)(signbit(d) ? 0x80u : 0u);
    memcpy((void *)(uintptr_t)a, b, sizeof b);
}

/* ---- protected-mode environment (32-bit) --------------------------------
 *
 * FNSAVE/FRSTOR move the complete 108-byte image. FNSTENV/FLDENV move the
 * 28-byte environment only and do not touch the register stack.  The layout is
 * seven 32-bit slots used by every PE32 WC3 module:
 *   +0 CW, +4 SW, +8 TAG, +12 FIP, +16 FCS/FOP, +20 FDP, +24 FDS.
 * Control, status and tag words occupy the low 16 bits of their slot; FOP is
 * not tracked and stays zero in the high half of the FCS slot.  Register
 * images follow the environment in ST(0)..ST(7) order. */
static inline void x87_fnstenv(CPU *c, uint32_t a) {
    x87_refresh_tags(c);
    x87_set_top(c);
    wr32(a + 0,  c->fpu_cw);
    wr32(a + 4,  c->fpu_sw);
    wr32(a + 8,  c->fpu_tag);
    wr32(a + 12, c->fpu_ip);
    wr32(a + 16, c->fpu_cs);
    wr32(a + 20, c->fpu_dp);
    wr32(a + 24, c->fpu_ds);
}
static inline void x87_fnsave(CPU *c, uint32_t a) {
    x87_fnstenv(c, a);
    for (int i = 0; i < 8; i++)
        x87_st_f80(a + 28 + (uint32_t)i * 10, *fst(c, i));
    cpu_x87_init(c);
}
static inline void x87_fldenv(CPU *c, uint32_t a) {
    const uint16_t cw = (uint16_t)rd32(a + 0);
    c->fpu_sw  = (uint16_t)rd32(a + 4);
    c->fpu_top = (int)((c->fpu_sw >> 11) & 7u);
    c->fpu_tag = (uint16_t)rd32(a + 8);
    c->fpu_ip  = rd32(a + 12);
    c->fpu_cs  = (uint16_t)rd32(a + 16);
    c->fpu_dp  = rd32(a + 20);
    c->fpu_ds  = (uint16_t)rd32(a + 24);
    if (cw != c->fpu_cw) {
        c->fpu_cw = cw;
        x87_apply_rounding(c);
    }
}
static inline void x87_frstor(CPU *c, uint32_t a) {
    x87_fldenv(c, a);
    for (int i = 0; i < 8; i++)
        *fst(c, i) = x87_ld_f80(a + 28 + (uint32_t)i * 10);
}

/* FXAM: C3 C2 C0 = 000 unsupported, 001 NaN, 010 normal, 011 infinity,
 * 100 zero, 101 empty, 110 denormal. C1 carries the sign. */
static inline void x87_fxam(CPU *c) {
    const x87_ext v = *fst(c, 0);
    const uint64_t b = x87_bits(v.v.d);
    uint32_t sign = (uint32_t)(b >> 63);
    uint32_t c3 = 0, c2 = 0, c0 = 0;

    if (x87_tag(c, c->fpu_top) == 3) {
        c3 = 1; c2 = 0; c0 = 1;
        sign = 0;
    } else if (x87_is_zero_bits(b)) {
        c3 = 1; c2 = 0; c0 = 0;
    } else if (x87_is_nan_bits(b)) {
        c3 = 0; c2 = 0; c0 = 1;
    } else if (x87_is_inf_bits(b)) {
        c3 = 0; c2 = 1; c0 = 1;
    } else if ((b & UINT64_C(0x7FF0000000000000)) == 0) {
        c3 = 1; c2 = 1; c0 = 0;       /* double subnormal */
    } else {
        c3 = 0; c2 = 1; c0 = 0;       /* normal */
    }
    x87_set_c(c, c3, c2, c0);
    c->fpu_sw = (c->fpu_sw & ~X87_C1) | (sign ? X87_C1 : 0);
}

/* ---- arithmetic --------------------------------------------------------- */

static inline x87_ext x87_add(CPU *c, x87_ext a, x87_ext b) {
    const double r = a.v.d + b.v.d;
    if (x87_invalid_result(r, a.v.d, b.v.d)) x87_raise_invalid(c);
    a.v.d = r;
    a.reserved = 0;
    return a;
}
static inline x87_ext x87_sub(CPU *c, x87_ext a, x87_ext b) {
    const double r = a.v.d - b.v.d;
    if (x87_invalid_result(r, a.v.d, b.v.d)) x87_raise_invalid(c);
    a.v.d = r;
    a.reserved = 0;
    return a;
}
static inline x87_ext x87_mul(CPU *c, x87_ext a, x87_ext b) {
    const double r = a.v.d * b.v.d;
    if (x87_invalid_result(r, a.v.d, b.v.d)) x87_raise_invalid(c);
    a.v.d = r;
    a.reserved = 0;
    return a;
}
static inline x87_ext x87_div(CPU *c, x87_ext a, x87_ext b) {
    const double r = a.v.d / b.v.d;
    if (x87_invalid_result(r, a.v.d, b.v.d)) x87_raise_invalid(c);
    a.v.d = r;
    a.reserved = 0;
    return a;
}
static inline x87_ext x87_sqrt(CPU *c, x87_ext a) {
    const double r = sqrt(a.v.d);
    if (x87_invalid_result(r, a.v.d, a.v.d)) x87_raise_invalid(c);
    a.v.d = r;
    a.reserved = 0;
    return a;
}
static inline x87_ext x87_roundint(CPU *c, x87_ext a) {
    const double r = nearbyint(a.v.d);
    if (x87_invalid_result(r, a.v.d, a.v.d)) x87_raise_invalid(c);
    a.v.d = r;
    a.reserved = 0;
    return a;
}

/* x87 FPREM/FPREM1 are not the microcode partial-remainder loop.  C2 is
 * always zero (quotient complete) and the result is the IEEE remainder. */
static inline x87_ext x87_fprem(CPU *c, x87_ext a, x87_ext b) {
    const double r = fmod(a.v.d, b.v.d);
    if (x87_invalid_result(r, a.v.d, b.v.d)) x87_raise_invalid(c);
    a.v.d = r;
    a.reserved = 0;
    c->fpu_sw &= ~X87_C2;
    return a;
}
static inline x87_ext x87_fprem1(CPU *c, x87_ext a, x87_ext b) {
    const double r = remainder(a.v.d, b.v.d);
    if (x87_invalid_result(r, a.v.d, b.v.d)) x87_raise_invalid(c);
    a.v.d = r;
    a.reserved = 0;
    c->fpu_sw &= ~X87_C2;
    return a;
}

/* ---- comparison --------------------------------------------------------- */

static inline void x87_compare(CPU *c, x87_ext a, x87_ext b, int signaling) {
    const uint64_t ab = x87_bits(a.v.d), bb = x87_bits(b.v.d);
    const int an = x87_is_nan_bits(ab), bn = x87_is_nan_bits(bb);
    c->fpu_sw &= ~X87_C1;
    if (an || bn) {
        if (signaling || x87_is_snan_bits(ab) || x87_is_snan_bits(bb))
            x87_raise_invalid(c);
        x87_set_c(c, 1, 1, 1);
    } else if (a.v.d < b.v.d) {
        x87_set_c(c, 0, 0, 1);
    } else if (a.v.d == b.v.d) {
        x87_set_c(c, 1, 0, 0);
    } else {
        x87_set_c(c, 0, 0, 0);
    }
}
static inline void x87_compare_eflags(CPU *c, x87_ext a, x87_ext b,
                                      int signaling) {
    const uint64_t ab = x87_bits(a.v.d), bb = x87_bits(b.v.d);
    const int an = x87_is_nan_bits(ab), bn = x87_is_nan_bits(bb);
    c->fpu_sw &= ~X87_C1;
    if (an || bn) {
        if (signaling || x87_is_snan_bits(ab) || x87_is_snan_bits(bb))
            x87_raise_invalid(c);
        c->zf = c->pf = c->cf = 1;
    } else if (a.v.d < b.v.d) {
        c->zf = 0; c->pf = 0; c->cf = 1;
    } else if (a.v.d == b.v.d) {
        c->zf = 1; c->pf = 0; c->cf = 0;
    } else {
        c->zf = 0; c->pf = 0; c->cf = 0;
    }
    c->of = c->sf = c->af = 0;
}

/* ---- integer/exponent helpers ------------------------------------------ */

static inline void x87_fscale(CPU *c) {
    const x87_ext x = *fst(c, 0);
    const double y = fst(c, 1)->v.d;
    double r;
    int e;
    if (isnan(y)) {
        /* FSCALE with a NaN scale returns a NaN (Table 3-34).  Only an SNaN
         * source raises IE; the result is quieted like ordinary arithmetic. */
        const int invalid = x87_invalid_result(y, x.v.d, y);
        r = invalid ? x87_quiet_nan(y) : y;
        if (invalid) x87_raise_invalid(c);
        fst(c, 0)->v.d = r;
        fst(c, 0)->reserved = 0;
        c->fpu_sw &= ~X87_C1;
        return;
    } else if (y >= 2147483647.0) e = 2147483647;
    else if (y <= -2147483648.0) e = INT_MIN;
    else e = (int)trunc(y);
    r = scalbn(x.v.d, e);
    if (x87_invalid_result(r, x.v.d, y)) x87_raise_invalid(c);
    fst(c, 0)->v.d = r;
    fst(c, 0)->reserved = 0;
    c->fpu_sw &= ~X87_C1;
}
static inline void x87_fxtract(CPU *c) {
    x87_ext x = *fst(c, 0);
    const uint64_t b = x87_bits(x.v.d);
    if (x87_is_zero_bits(b)) {
        /* logb(+-0) is -inf, and the significand keeps the sign. */
        *fst(c, 0) = x87_from_f64(-INFINITY);
        fpush(c, x);
    } else if (x87_is_nan_bits(b)) {
        if (x87_is_snan_bits(b)) {
            x87_raise_invalid(c);
            x.v.d = x87_quiet_nan(x.v.d);
            x.reserved = 0;
        }
        *fst(c, 0) = x;
        fpush(c, x);
    } else if (x87_is_inf_bits(b)) {
        x87_raise_invalid(c);
        *fst(c, 0) = x87_indefinite();
        fpush(c, x87_indefinite());
    } else {
        int e = 0;
        double f = frexp(x.v.d, &e);
        f *= 2.0;
        e -= 1;
        *fst(c, 0) = x87_from_i32(e);
        fpush(c, x87_from_f64(f));
    }
    c->fpu_sw &= ~X87_C1;
}

/* ---- transcendentals ---------------------------------------------------- */

/* libm is the cold path; use the same one-check invalid rule as the arithmetic
 * helpers so SNaN, Inf and out-of-domain inputs still surface as IE. */
static inline x87_ext x87_unary_result(CPU *c, double r, double x) {
    if (x87_invalid_result(r, x, x)) x87_raise_invalid(c);
    return x87_from_f64(r);
}
static inline x87_ext x87_binary_result(CPU *c, double r, double a, double b) {
    if (x87_invalid_result(r, a, b)) x87_raise_invalid(c);
    return x87_from_f64(r);
}
static inline x87_ext x87_f2xm1(CPU *c, x87_ext x) {
    return x87_unary_result(c, exp2(x.v.d) - 1.0, x.v.d);
}
static inline x87_ext x87_fsin(CPU *c, x87_ext x) {
    return x87_unary_result(c, sin(x.v.d), x.v.d);
}
static inline x87_ext x87_fcos(CPU *c, x87_ext x) {
    return x87_unary_result(c, cos(x.v.d), x.v.d);
}
static inline x87_ext x87_fptan(CPU *c, x87_ext x) {
    return x87_unary_result(c, tan(x.v.d), x.v.d);
}
static inline x87_ext x87_fpatan(CPU *c, x87_ext y, x87_ext x) {
    return x87_binary_result(c, atan2(y.v.d, x.v.d), y.v.d, x.v.d);
}
static inline x87_ext x87_fyl2x(CPU *c, x87_ext x, x87_ext y) {
    return x87_binary_result(c, y.v.d * log2(x.v.d), x.v.d, y.v.d);
}
static inline x87_ext x87_fyl2xp1(CPU *c, x87_ext x, x87_ext y) {
    /* FYL2XP1 is defined only for x > -1.  At x == -1 log1p() returns -inf,
     * which x87 reports as #IA with the indefinite result, not as a valid
     * negative infinity. */
    if (x.v.d <= -1.0) {
        x87_raise_invalid(c);
        return x87_indefinite();
    }
    return x87_binary_result(c, y.v.d * log1p(x.v.d) * 0x1.71547652b82fep+0,
                             x.v.d, y.v.d);
}

/* ---- reset -------------------------------------------------------------- */

/* Shared FNINIT/FNSAVE reset.  cpu_x87_init() and cpu_x87_init_thread() in
 * cpu.h select the architectural and Windows default control words. */
static inline void x87_init(CPU *c, uint16_t cw) {
    memset(c->st, 0, sizeof c->st);
    c->fpu_top = 0;
    c->fpu_cw = cw;
    c->fpu_tag = 0xFFFFu;
    c->fpu_sw = 0;
    c->fpu_ip = c->fpu_dp = 0;
    c->fpu_cs = c->fpu_ds = 0;
    c->x87_reserved = 0;
    x87_clear_ftz();
    x87_apply_rounding(c);
}

#endif /* PCRECOMP_X87_H */
