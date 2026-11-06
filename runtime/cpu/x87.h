/*
 * x87.h - exact 80-bit x87 model for the CPU-struct recompiler.
 *
 * This is the replacement for the old `double st[8]` model. The arithmetic
 * core is Berkeley SoftFloat's extF80 implementation; `tests/x87_softfloat_probe.c`
 * checks add/sub/mul/div/sqrt and the status flags against the host x87 over
 * random normal operands.
 *
 * The generated code calls only the helpers below. Keep that boundary: a raw
 * C `double` operation on an x87 register is a silent precision bug.
 *
 * Not yet exact, and deliberately marked here:
 *   - transcendental microcode (fsin/fcos/fpatan/fyl2x/f2xm1) uses the host
 *     `long double`; the x87 microcode is not correctly rounded, so these need
 *     a trace-based replacement before the final gate;
 *   - fprem/fprem1 use IEEE remainder rather than the x87 partial-remainder
 *     algorithm with C2;
 *   - fnstenv/fnsave save zero FIP/FDP unless a guest environment image
 *     supplied them; the lifter does not yet update the instruction pointers.
 * MMX now aliases the low 64 bits of the same physical st[] registers through
 * `mm_get`; there is no separate MMX array.
 */
#ifndef PCRECOMP_X87_H
#define PCRECOMP_X87_H

#ifndef LITTLEENDIAN
#define LITTLEENDIAN 1
#endif
#ifndef SOFTFLOAT_FAST_INT64
#define SOFTFLOAT_FAST_INT64 1
#endif
#ifndef THREAD_LOCAL
#define THREAD_LOCAL _Thread_local
#endif
#include "softfloat.h"
#include <math.h>

/* ---- constants ---------------------------------------------------------- */

static inline extFloat80_t x87_zero(void) {
    extFloat80_t v = { 0, 0 };
    return v;
}
static inline extFloat80_t x87_one(void) {
    extFloat80_t v = { UINT64_C(0x8000000000000000), 0x3FFF };
    return v;
}
static inline extFloat80_t x87_pi(void) {
    extFloat80_t v = { UINT64_C(0xC90FDAA22168C235), 0x4000 };
    return v;
}
static inline extFloat80_t x87_l2e(void) {
    extFloat80_t v = { UINT64_C(0xB8AA3B295C17F0BC), 0x3FFF };
    return v;
}
static inline extFloat80_t x87_l2t(void) {
    extFloat80_t v = { UINT64_C(0xD49A784BCD1B8AFE), 0x4000 };
    return v;
}
static inline extFloat80_t x87_lg2(void) {
    extFloat80_t v = { UINT64_C(0x9A209A84FBCFF799), 0x3FFD };
    return v;
}
static inline extFloat80_t x87_ln2(void) {
    extFloat80_t v = { UINT64_C(0xB17217F7D1CF79AC), 0x3FFE };
    return v;
}

static inline extFloat80_t x87_from_f64(double d) {
    float64_t f;
    memcpy(&f.v, &d, 8);
    return f64_to_extF80(f);
}
static inline extFloat80_t x87_from_i16(int16_t i) { return i32_to_extF80((int32_t)i); }
static inline extFloat80_t x87_from_i32(int32_t i) { return i32_to_extF80(i); }
static inline extFloat80_t x87_from_i64(int64_t i) { return i64_to_extF80(i); }

/* ---- classification ----------------------------------------------------- */

static inline int x87_is_zero(extFloat80_t v) {
    return ((v.signExp & 0x7FFF) == 0) && (v.signif == 0);
}
static inline int x87_is_denorm(extFloat80_t v) {
    /* Exp 0 with any non-zero significand is a denormal operand for the
     * hardware, including the pseudo-denormal encoding where bit 63 is set. */
    return ((v.signExp & 0x7FFF) == 0) && (v.signif != 0);
}
static inline extFloat80_t x87_canonical(extFloat80_t v) {
    /* SoftFloat rejects a pseudo-denormal (exp 0, integer bit 1). The hardware
     * treats it as a denormal; shift it into the representable denormal form.
     * The discarded bit is below the real denormal resolution, so the only
     * observable loss is the exact value of a signalling 1-ulp case. */
    if (((v.signExp & 0x7FFF) == 0)
        && (v.signif & UINT64_C(0x8000000000000000)))
        v.signif >>= 1;
    return v;
}
/* Pseudo-denormals, unnormals and pseudo-NaNs are the encodings the x87 tag
 * word calls "special" and FXAM calls unsupported. They are not arithmetic
 * denormals and must not be normalised silently. */
static inline int x87_is_unnormal(extFloat80_t v) {
    uint16_t e = v.signExp & 0x7FFF;
    if (e == 0x7FFF) return 0;
    if (e == 0) return (v.signif != 0)
        && ((v.signif & UINT64_C(0x8000000000000000)) != 0);
    return (v.signif & UINT64_C(0x8000000000000000)) == 0;
}
/* Everything Intel calls an unsupported encoding: pseudo-denormal (exp 0,
 * integer bit 1), unnormal (exp != 0, integer bit 0) and pseudo-NaN/infinity
 * (exp 7FFF, integer bit 0). Arithmetic on these raises IE and DE and returns
 * the indefinite QNaN; loading one into ST only marks it "special". */
static inline int x87_is_pseudo(extFloat80_t v) {
    uint16_t e = v.signExp & 0x7FFF;
    if (e == 0x7FFF)
        return (v.signif & UINT64_C(0x8000000000000000)) == 0;
    if (e == 0)
        return 0;      /* pseudo-denormal: accepted as a denormal by x87 */
    return (v.signif & UINT64_C(0x8000000000000000)) == 0;
}
static inline int x87_is_inf(extFloat80_t v) {
    return ((v.signExp & 0x7FFF) == 0x7FFF)
        && (v.signif == UINT64_C(0x8000000000000000));
}
static inline int x87_is_nan(extFloat80_t v) {
    return ((v.signExp & 0x7FFF) == 0x7FFF)
        && ((v.signif & UINT64_C(0x7FFFFFFFFFFFFFFF)) != 0);
}
static inline int x87_is_snan(extFloat80_t v) {
    return x87_is_nan(v)
        && ((v.signif & UINT64_C(0x4000000000000000)) == 0)
        && ((v.signif & UINT64_C(0x3FFFFFFFFFFFFFFF)) != 0);
}

/* ---- control/status ----------------------------------------------------- */

static inline uint8_t x87_round_mode(const CPU *c) {
    switch ((c->fpu_cw >> 10) & 3u) {
    case 1:  return softfloat_round_min;
    case 2:  return softfloat_round_max;
    case 3:  return softfloat_round_minMag;
    default: return softfloat_round_near_even;
    }
}
static inline uint8_t x87_precision(const CPU *c) {
    switch ((c->fpu_cw >> 8) & 3u) {
    case 0:  return 32;   /* 24-bit significand */
    case 2:  return 64;   /* 53-bit significand */
    default: return 80;   /* 64-bit significand; also the reserved encoding */
    }
}
static inline void x87_set_top(CPU *c) {
    c->fpu_sw = (c->fpu_sw & ~0x3800u) | ((uint32_t)(c->fpu_top & 7) << 11);
}
static inline void x87_begin(CPU *c) {
    softfloat_roundingMode = x87_round_mode(c);
    extF80_roundingPrecision = x87_precision(c);
    softfloat_exceptionFlags = 0;
}
static inline void x87_end(CPU *c) {
    uint_fast8_t f = softfloat_exceptionFlags;
    softfloat_exceptionFlags = 0;
    if (f & softfloat_flag_invalid)  c->fpu_sw |= 0x01;
    if (f & softfloat_flag_infinite) c->fpu_sw |= 0x04;   /* divide by zero */
    if (f & softfloat_flag_overflow) c->fpu_sw |= 0x08;
    if (f & softfloat_flag_underflow) c->fpu_sw |= 0x10;
    if (f & softfloat_flag_inexact)  c->fpu_sw |= 0x20;
}
static inline void x87_denorm_check(CPU *c, extFloat80_t a) {
    if (x87_is_denorm(a)) c->fpu_sw |= 0x02;
}
static inline extFloat80_t x87_indefinite(void) {
    extFloat80_t v = { UINT64_C(0xC000000000000000), 0xFFFF };
    return v;
}
/* An unsupported operand poisons the operation before SoftFloat sees it:
 * SoftFloat would silently normalise an unnormal, which the hardware does
 * not do. The hardware raises IE|DE, leaves C1 alone for arithmetic, and
 * returns the indefinite QNaN. */
static inline int x87_unsupported(CPU *c, extFloat80_t v) {
    if (!x87_is_pseudo(v)) return 0;
    c->fpu_sw |= 0x01;      /* IE; the hardware does not also set DE here */
    return 1;
}
static inline void x87_stack_fault(CPU *c) {
    c->fpu_sw |= 0x41;             /* IE | SF */
    c->fpu_sw |= 0x0200;           /* C1 */
}
static inline void x87_set_c(CPU *c, uint32_t c3, uint32_t c2, uint32_t c0) {
    c->fpu_sw = (c->fpu_sw & ~0x4700u)
              | (c3 ? 0x4000u : 0) | (c2 ? 0x0400u : 0) | (c0 ? 0x0100u : 0);
}
static inline void x87_fnclex(CPU *c) {
    c->fpu_sw &= ~0x003Fu;
    softfloat_exceptionFlags = 0;
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
static inline uint16_t x87_tag_for(extFloat80_t v) {
    if (x87_is_zero(v)) return 1;
    /* Denormals are valid, not special. Tag 2 is for NaN, infinity and the
     * unsupported/unnormal encodings only. */
    if (x87_is_nan(v) || x87_is_inf(v) || x87_is_unnormal(v)) return 2;
    return 0;
}

/* ---- stack -------------------------------------------------------------- */
static inline void fpush(CPU *c, extFloat80_t v) {
    int p = (c->fpu_top - 1) & 7;
    if (x87_tag(c, p) != 3) x87_stack_fault(c);   /* stack overflow */
    c->fpu_top = p;
    c->st[p] = v;
    x87_set_tag(c, p, x87_tag_for(v));
    x87_set_top(c);
}
static inline extFloat80_t fpop(CPU *c) {
    int p = c->fpu_top;
    extFloat80_t v = c->st[p];
    if (x87_tag(c, p) == 3) x87_stack_fault(c);   /* stack underflow */
    x87_set_tag(c, p, 3);
    c->fpu_top = (c->fpu_top + 1) & 7;
    x87_set_top(c);
    return v;
}
static inline extFloat80_t *fst(CPU *c, int i) {
    return &c->st[(c->fpu_top + i) & 7];
}
static inline void x87_fxch(CPU *c, int i) {
    int a = c->fpu_top, b = (c->fpu_top + i) & 7;
    extFloat80_t t = c->st[a];
    uint16_t tt = x87_tag(c, a);
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
    int s = (c->fpu_top + i) & 7;
    if (x87_tag(c, s) == 3) { x87_stack_fault(c); return; }
    c->st[c->fpu_top] = c->st[s];
    x87_set_tag(c, c->fpu_top, x87_tag(c, s));
}
static inline void x87_fchs(CPU *c) {
    (*fst(c, 0)).signExp ^= 0x8000;
}
static inline void x87_fabs(CPU *c) {
    (*fst(c, 0)).signExp &= 0x7FFF;
}

/* ---- memory loads/stores ------------------------------------------------ */
static inline extFloat80_t x87_ld_f32(uint32_t a) {
    float32_t f; memcpy(&f.v, (void *)(uintptr_t)a, 4);
    return f32_to_extF80(f);
}
static inline extFloat80_t x87_ld_f64(uint32_t a) {
    float64_t f; memcpy(&f.v, (void *)(uintptr_t)a, 8);
    return f64_to_extF80(f);
}
static inline extFloat80_t x87_ld_f80(uint32_t a) {
    extFloat80_t v;
    memcpy(&v.signif, (void *)(uintptr_t)a, 8);
    memcpy(&v.signExp, (void *)((uintptr_t)a + 8), 2);
    return v;
}
static inline extFloat80_t x87_ld_i16(uint32_t a) {
    int16_t i; memcpy(&i, (void *)(uintptr_t)a, 2); return x87_from_i16(i);
}
static inline extFloat80_t x87_ld_i32(uint32_t a) {
    int32_t i; memcpy(&i, (void *)(uintptr_t)a, 4); return x87_from_i32(i);
}
static inline extFloat80_t x87_ld_i64(uint32_t a) {
    int64_t i; memcpy(&i, (void *)(uintptr_t)a, 8); return x87_from_i64(i);
}
static inline void x87_st_f32(CPU *c, uint32_t a, extFloat80_t v) {
    x87_begin(c);
    float32_t f = extF80_to_f32(v);
    memcpy((void *)(uintptr_t)a, &f.v, 4);
    x87_end(c);
}
static inline void x87_st_f64(CPU *c, uint32_t a, extFloat80_t v) {
    x87_begin(c);
    float64_t f = extF80_to_f64(v);
    memcpy((void *)(uintptr_t)a, &f.v, 8);
    x87_end(c);
}
static inline void x87_st_f80(uint32_t a, extFloat80_t v) {
    memcpy((void *)(uintptr_t)a, &v.signif, 8);
    memcpy((void *)((uintptr_t)a + 8), &v.signExp, 2);
}
static inline void x87_st_i16(CPU *c, uint32_t a, extFloat80_t v) {
    x87_begin(c);
    int32_t i = extF80_to_i32(v, x87_round_mode(c), true);
    if (i < -32768 || i > 32767) { c->fpu_sw |= 0x01; i = (int16_t)0x8000; }
    int16_t o = (int16_t)i;
    memcpy((void *)(uintptr_t)a, &o, 2);
    x87_end(c);
}
/* FISTTP (SSE3) always truncates toward zero, independent of the control
 * word. It is present in the war3.exe image and is kept out of the "unknown
 * x87" list even though the fixed Pentium 4 CPUID profile does not advertise
 * SSE3; unreachability still has to be proven by the trace harness. */
static inline void x87_st_i16_rz(CPU *c, uint32_t a, extFloat80_t v) {
    x87_begin(c);
    int32_t i = extF80_to_i32(v, softfloat_round_minMag, true);
    if (i < -32768 || i > 32767) { c->fpu_sw |= 0x01; i = (int16_t)0x8000; }
    int16_t o = (int16_t)i;
    memcpy((void *)(uintptr_t)a, &o, 2);
    x87_end(c);
}
static inline void x87_st_i32_rz(CPU *c, uint32_t a, extFloat80_t v) {
    x87_begin(c);
    int32_t i = extF80_to_i32(v, softfloat_round_minMag, true);
    memcpy((void *)(uintptr_t)a, &i, 4);
    x87_end(c);
}
static inline void x87_st_i64_rz(CPU *c, uint32_t a, extFloat80_t v) {
    x87_begin(c);
    int64_t i = extF80_to_i64(v, softfloat_round_minMag, true);
    memcpy((void *)(uintptr_t)a, &i, 8);
    x87_end(c);
}
static inline void x87_st_i32(CPU *c, uint32_t a, extFloat80_t v) {
    x87_begin(c);
    int32_t i = extF80_to_i32(v, x87_round_mode(c), true);
    memcpy((void *)(uintptr_t)a, &i, 4);
    x87_end(c);
}
static inline void x87_st_i64(CPU *c, uint32_t a, extFloat80_t v) {
    x87_begin(c);
    int64_t i = extF80_to_i64(v, x87_round_mode(c), true);
    memcpy((void *)(uintptr_t)a, &i, 8);
    x87_end(c);
}

/* ---- protected-mode environment (32-bit) --------------------------------
 *
 * FNSAVE/FRSTOR move the complete 108-byte image. FNSTENV/FLDENV move the
 * 28-byte environment only and do not touch the register stack. The layout is
 * the 32-bit protected-mode one used by every PE32 WC3 module:
 *   +0 cw, +2 sw, +4 tag, +6 FIP, +10 CS, +12 FDP, +16 DS.
 * Register images follow the environment in ST(0)..ST(7) order.
 */
static inline void x87_fnstenv(CPU *c, uint32_t a) {
    x87_set_top(c);
    wr16(a + 0, c->fpu_cw);
    wr16(a + 2, (uint16_t)c->fpu_sw);
    wr16(a + 4, c->fpu_tag);
    wr32(a + 6, c->fpu_ip);
    wr16(a + 10, c->fpu_cs);
    wr32(a + 12, c->fpu_dp);
    wr16(a + 16, c->fpu_ds);
}
static inline void x87_fnsave(CPU *c, uint32_t a) {
    int i;
    x87_fnstenv(c, a);
    for (i = 0; i < 8; i++)
        x87_st_f80(a + 28 + (uint32_t)i * 10, *fst(c, i));
    /* FNSAVE leaves the FPU in the FNINIT state. */
    cpu_x87_init(c);
}
static inline void x87_fldenv(CPU *c, uint32_t a) {
    c->fpu_cw  = rd16(a + 0);
    c->fpu_sw  = rd16(a + 2);
    c->fpu_top = (int)((c->fpu_sw >> 11) & 7u);
    c->fpu_tag = rd16(a + 4);
    c->fpu_ip  = rd32(a + 6);
    c->fpu_cs  = rd16(a + 10);
    c->fpu_dp  = rd32(a + 12);
    c->fpu_ds  = rd16(a + 16);
    softfloat_exceptionFlags = 0;
}
static inline void x87_frstor(CPU *c, uint32_t a) {
    int i;
    x87_fldenv(c, a);
    for (i = 0; i < 8; i++)
        *fst(c, i) = x87_ld_f80(a + 28 + (uint32_t)i * 10);
}

/* FXAM: C3 C2 C0 = 000 unsupported, 001 NaN, 010 normal, 011 infinity,
 * 100 zero, 101 empty, 110 denormal. C1 carries the sign. */
static inline void x87_fxam(CPU *c) {
    extFloat80_t v = *fst(c, 0);
    uint32_t sign = (v.signExp >> 15) & 1u;
    uint32_t c3 = 0, c2 = 0, c0 = 0;
    if (x87_tag(c, c->fpu_top) == 3) {
        c3 = 1; c2 = 0; c0 = 1;
        sign = 0;
    } else if (x87_is_zero(v)) {
        c3 = 1; c2 = 0; c0 = 0;
    } else if (x87_is_unnormal(v)) {
        c3 = 0; c2 = 0; c0 = 0;
    } else if (x87_is_nan(v)) {
        c3 = 0; c2 = 0; c0 = 1;
    } else if (x87_is_inf(v)) {
        c3 = 0; c2 = 1; c0 = 1;
    } else if (x87_is_denorm(v)) {
        c3 = 1; c2 = 1; c0 = 0;
    } else {
        c3 = 0; c2 = 1; c0 = 0;
    }
    x87_set_c(c, c3, c2, c0);
    c->fpu_sw = (c->fpu_sw & ~0x0200u) | (sign ? 0x0200u : 0);
}

/* ---- arithmetic --------------------------------------------------------- */
static inline extFloat80_t x87_prepare(CPU *c, extFloat80_t v) {
    if (x87_unsupported(c, v)) return v;
    x87_denorm_check(c, v);
    return x87_canonical(v);
}
/* NaN operands suppress the denormal-operand flag: hardware answers
 * QNaN+denormal with just the QNaN and status 0. */
static inline void x87_prepare_pair(CPU *c, extFloat80_t *a, extFloat80_t *b) {
    if (x87_is_nan(*a) || x87_is_nan(*b)) return;
    *a = x87_prepare(c, *a); *b = x87_prepare(c, *b);
}
static inline extFloat80_t x87_add(CPU *c, extFloat80_t a, extFloat80_t b) {
    if (x87_unsupported(c, a) || x87_unsupported(c, b)) return x87_indefinite();
    x87_prepare_pair(c, &a, &b);
    x87_begin(c); a = extF80_add(a, b); x87_end(c); return a;
}
static inline extFloat80_t x87_sub(CPU *c, extFloat80_t a, extFloat80_t b) {
    if (x87_unsupported(c, a) || x87_unsupported(c, b)) return x87_indefinite();
    x87_prepare_pair(c, &a, &b);
    x87_begin(c); a = extF80_sub(a, b); x87_end(c); return a;
}
static inline extFloat80_t x87_mul(CPU *c, extFloat80_t a, extFloat80_t b) {
    if (x87_unsupported(c, a) || x87_unsupported(c, b)) return x87_indefinite();
    x87_prepare_pair(c, &a, &b);
    x87_begin(c); a = extF80_mul(a, b); x87_end(c); return a;
}
static inline extFloat80_t x87_div(CPU *c, extFloat80_t a, extFloat80_t b) {
    if (x87_unsupported(c, a) || x87_unsupported(c, b)) return x87_indefinite();
    /* Divide-by-zero takes precedence: hardware reports ZE and does not also
     * report a denormal-operand exception for the numerator. */
    if (!x87_is_nan(a) && !x87_is_nan(b) && !x87_is_zero(b))
        x87_prepare_pair(c, &a, &b);
    x87_begin(c); a = extF80_div(a, b); x87_end(c); return a;
}
static inline extFloat80_t x87_sqrt(CPU *c, extFloat80_t a) {
    if (x87_unsupported(c, a)) return x87_indefinite();
    /* Invalid (negative) sqrt does not also set DE for a negative denormal. */
    if (!x87_is_nan(a) && !(a.signExp & 0x8000))
        a = x87_prepare(c, a);
    x87_begin(c); a = extF80_sqrt(a); x87_end(c); return a;
}
static inline extFloat80_t x87_roundint(CPU *c, extFloat80_t a) {
    if (x87_unsupported(c, a)) return x87_indefinite();
    if (!x87_is_nan(a)) a = x87_prepare(c, a);
    x87_begin(c); a = extF80_roundToInt(a, x87_round_mode(c), true);
    x87_end(c); return a;
}

/* x87 fprem/fprem1. This is the IEEE remainder approximation for now; the
 * x87 C2 quotient-bit semantics are not modelled. No Warcraft III game.dll
 * x87 instruction is in this set; Mss32 has six, tracked for the audio gate. */
static inline extFloat80_t x87_fprem(CPU *c, extFloat80_t a, extFloat80_t b) {
    if (x87_unsupported(c, a) || x87_unsupported(c, b)) return x87_indefinite();
    a = x87_prepare(c, a); b = x87_prepare(c, b);
    x87_begin(c); a = extF80_rem(a, b); x87_end(c); return a;
}

/* ---- comparison --------------------------------------------------------- */
static inline void x87_compare(CPU *c, extFloat80_t a, extFloat80_t b,
                               int signaling) {
    x87_begin(c);
    c->fpu_sw &= ~0x0200u;             /* C1 is cleared by FCOM/FUCOM */
    int an = x87_is_nan(a), bn = x87_is_nan(b);
    if (an || bn) {
        /* fcom raises invalid for any NaN; fucom only for an SNaN. */
        if (signaling || x87_is_snan(a) || x87_is_snan(b))
            softfloat_exceptionFlags |= softfloat_flag_invalid;
        x87_set_c(c, 1, 1, 1);
    } else if (extF80_lt(a, b)) {
        x87_set_c(c, 0, 0, 1);
    } else if (extF80_eq(a, b)) {
        x87_set_c(c, 1, 0, 0);
    } else {
        x87_set_c(c, 0, 0, 0);
    }
    x87_end(c);
}
static inline void x87_compare_eflags(CPU *c, extFloat80_t a, extFloat80_t b,
                                      int signaling) {
    x87_begin(c);
    c->fpu_sw &= ~0x0200u;             /* FCOMI clears C1 */
    int an = x87_is_nan(a), bn = x87_is_nan(b);
    if (an || bn) {
        if (signaling || x87_is_snan(a) || x87_is_snan(b))
            softfloat_exceptionFlags |= softfloat_flag_invalid;
        c->zf = c->pf = c->cf = 1;
    } else if (extF80_lt(a, b)) {
        c->zf = 0; c->pf = 0; c->cf = 1;
    } else if (extF80_eq(a, b)) {
        c->zf = 1; c->pf = 0; c->cf = 0;
    } else {
        c->zf = 0; c->pf = 0; c->cf = 0;
    }
    c->of = c->sf = c->af = 0;
    x87_end(c);
}

/* ---- integer/exponent helpers ------------------------------------------ */
static inline extFloat80_t x87_pow2(int e) {
    extFloat80_t v = { UINT64_C(0x8000000000000000), 0 };
    int x = e + 16383;
    if (x <= 0) {
        /* Subnormal power of two; shift the explicit integer bit. */
        if (x < -63) { v.signif = 0; v.signExp = 0; return v; }
        v.signif = UINT64_C(0x8000000000000000) >> (1 - x);
        v.signExp = 0;
        return v;
    }
    if (x >= 0x7FFF) { v.signif = UINT64_C(0x8000000000000000); v.signExp = 0x7FFF; return v; }
    v.signExp = (uint16_t)x;
    return v;
}
static inline void x87_fscale(CPU *c) {
    int e = (int)extF80_to_i32(*fst(c, 1), softfloat_round_minMag, false);
    extFloat80_t x = *fst(c, 0);
    extFloat80_t p = x87_pow2(e);
    x87_denorm_check(c, x);
    x87_begin(c);
    x = extF80_mul(x, p);
    x87_end(c);
    *fst(c, 0) = x;
}
static inline void x87_fxtract(CPU *c) {
    extFloat80_t x = *fst(c, 0);
    int e;
    extFloat80_t m;
    if (x87_is_zero(x) || x87_is_nan(x) || x87_is_inf(x)) {
        /* The hardware leaves these cases to the invalid/stack path. */
        x87_stack_fault(c);
        e = 0; m = x;
    } else if (x87_is_denorm(x)) {
        uint64_t sig = x.signif;
        int sh = 0;
        while (!(sig & UINT64_C(0x8000000000000000))) { sig <<= 1; sh++; }
        e = 1 - 16383 - sh;
        m.signif = sig;
        m.signExp = (uint16_t)((x.signExp & 0x8000) | 0x3FFF);
    } else {
        e = (x.signExp & 0x7FFF) - 16383;
        m.signif = x.signif;
        m.signExp = (uint16_t)((x.signExp & 0x8000) | 0x3FFF);
    }
    *fst(c, 0) = x87_from_i32(e);
    fpush(c, m);
    c->fpu_sw &= ~0x0200u;   /* C1=0 */
}

/* ---- host long double bridge (transcendentals only) --------------------- */
static inline long double x87_to_ld(extFloat80_t v) {
#if defined(__SIZEOF_LONG_DOUBLE__) && (__SIZEOF_LONG_DOUBLE__ == 16) \
    && defined(__LDBL_MANT_DIG__) && (__LDBL_MANT_DIG__ == 113)
    float128_t f = extF80_to_f128(v);
    long double d;
    memcpy(&d, f.v, sizeof d);
    return d;
#else
    long double d;
    memcpy(&d, &v, 10);
    return d;
#endif
}
static inline extFloat80_t x87_from_ld(long double d) {
#if defined(__SIZEOF_LONG_DOUBLE__) && (__SIZEOF_LONG_DOUBLE__ == 16) \
    && defined(__LDBL_MANT_DIG__) && (__LDBL_MANT_DIG__ == 113)
    float128_t f;
    memcpy(f.v, &d, sizeof d);
    return f128_to_extF80(f);
#else
    extFloat80_t v;
    memcpy(&v, &d, 10);
    return v;
#endif
}

/* Transcendentals: high-precision placeholders, flagged in the header comment.
 * They are not used by war3.exe, Storm.dll, game.dll or ijl15.dll. */
static inline extFloat80_t x87_f2xm1(CPU *c, extFloat80_t x) {
    (void)c;
    return x87_from_ld(powl(2.0L, x87_to_ld(x)) - 1.0L);
}
static inline extFloat80_t x87_fsin(CPU *c, extFloat80_t x) {
    (void)c;
    return x87_from_ld(sinl(x87_to_ld(x)));
}
static inline extFloat80_t x87_fcos(CPU *c, extFloat80_t x) {
    (void)c;
    return x87_from_ld(cosl(x87_to_ld(x)));
}
static inline extFloat80_t x87_fptan(CPU *c, extFloat80_t x) {
    (void)c;
    return x87_from_ld(tanl(x87_to_ld(x)));
}
static inline extFloat80_t x87_fpatan(CPU *c, extFloat80_t y, extFloat80_t x) {
    (void)c;
    return x87_from_ld(atan2l(x87_to_ld(y), x87_to_ld(x)));
}
static inline extFloat80_t x87_fyl2x(CPU *c, extFloat80_t x, extFloat80_t y) {
    (void)c;
    return x87_from_ld(x87_to_ld(y) * log2l(x87_to_ld(x)));
}
static inline extFloat80_t x87_fyl2xp1(CPU *c, extFloat80_t x, extFloat80_t y) {
    (void)c;
    return x87_from_ld(x87_to_ld(y) * log2l(x87_to_ld(x) + 1.0L));
}

#endif /* PCRECOMP_X87_H */
