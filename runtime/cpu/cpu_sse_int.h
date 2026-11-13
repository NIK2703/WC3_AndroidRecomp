/*
 * cpu_sse_int.h - integer/packed SSE helpers.
 *
 * These are the packed integer operations that the lifter used to emit as
 * scalar lane loops in every generated function.  AArch64 has a 128-bit
 * vector unit, so the same operation is one ASIMD instruction; the scalar
 * implementation is kept for the x86-64 host build and for any other target.
 *
 * Only integer and bitwise operations live here.  FP lanes stay in the
 * lifter's lane code: the guest's SSE single/double semantics (NaN payloads,
 * denormals, rounding) must not be changed by a vector rewrite.
 */
#ifndef WC3_CPU_SSE_INT_H
#define WC3_CPU_SSE_INT_H

#if defined(__aarch64__)
#include <arm_neon.h>

static inline uint8x16_t sse_neon_u8(const XMM *x) { return vld1q_u8(x->u8); }
static inline uint16x8_t sse_neon_u16(const XMM *x) { return vld1q_u16(x->u16); }
static inline uint32x4_t sse_neon_u32(const XMM *x) { return vld1q_u32(x->u32); }
static inline uint64x2_t sse_neon_u64(const XMM *x) { return vld1q_u64(x->u64); }
static inline int8x16_t sse_neon_s8(const XMM *x) { return vld1q_s8(x->i8); }
static inline int16x8_t sse_neon_s16(const XMM *x) { return vld1q_s16(x->i16); }
static inline int32x4_t sse_neon_s32(const XMM *x) { return vld1q_s32(x->i32); }
static inline void sse_neon_store_u8(XMM *x, uint8x16_t v) { vst1q_u8(x->u8, v); }
static inline void sse_neon_store_u16(XMM *x, uint16x8_t v) { vst1q_u16(x->u16, v); }
static inline void sse_neon_store_u32(XMM *x, uint32x4_t v) { vst1q_u32(x->u32, v); }
static inline void sse_neon_store_u64(XMM *x, uint64x2_t v) { vst1q_u64(x->u64, v); }
static inline void sse_neon_store_s8(XMM *x, int8x16_t v) { vst1q_s8(x->i8, v); }
static inline void sse_neon_store_s16(XMM *x, int16x8_t v) { vst1q_s16(x->i16, v); }
static inline void sse_neon_store_s32(XMM *x, int32x4_t v) { vst1q_s32(x->i32, v); }

static inline void sse_paddb(XMM *d, const XMM *s) {
    sse_neon_store_u8(d, vaddq_u8(sse_neon_u8(d), sse_neon_u8(s)));
}
static inline void sse_paddw(XMM *d, const XMM *s) {
    sse_neon_store_u16(d, vaddq_u16(sse_neon_u16(d), sse_neon_u16(s)));
}
static inline void sse_paddd(XMM *d, const XMM *s) {
    sse_neon_store_u32(d, vaddq_u32(sse_neon_u32(d), sse_neon_u32(s)));
}
static inline void sse_paddq(XMM *d, const XMM *s) {
    sse_neon_store_u64(d, vaddq_u64(sse_neon_u64(d), sse_neon_u64(s)));
}
static inline void sse_psubb(XMM *d, const XMM *s) {
    sse_neon_store_u8(d, vsubq_u8(sse_neon_u8(d), sse_neon_u8(s)));
}
static inline void sse_psubw(XMM *d, const XMM *s) {
    sse_neon_store_u16(d, vsubq_u16(sse_neon_u16(d), sse_neon_u16(s)));
}
static inline void sse_psubd(XMM *d, const XMM *s) {
    sse_neon_store_u32(d, vsubq_u32(sse_neon_u32(d), sse_neon_u32(s)));
}
static inline void sse_psubq(XMM *d, const XMM *s) {
    sse_neon_store_u64(d, vsubq_u64(sse_neon_u64(d), sse_neon_u64(s)));
}
static inline void sse_paddsb(XMM *d, const XMM *s) {
    sse_neon_store_s8(d, vqaddq_s8(sse_neon_s8(d), sse_neon_s8(s)));
}
static inline void sse_psubsb(XMM *d, const XMM *s) {
    sse_neon_store_s8(d, vqsubq_s8(sse_neon_s8(d), sse_neon_s8(s)));
}
static inline void sse_paddsw(XMM *d, const XMM *s) {
    sse_neon_store_s16(d, vqaddq_s16(sse_neon_s16(d), sse_neon_s16(s)));
}
static inline void sse_psubsw(XMM *d, const XMM *s) {
    sse_neon_store_s16(d, vqsubq_s16(sse_neon_s16(d), sse_neon_s16(s)));
}
static inline void sse_paddusb(XMM *d, const XMM *s) {
    sse_neon_store_u8(d, vqaddq_u8(sse_neon_u8(d), sse_neon_u8(s)));
}
static inline void sse_psubusb(XMM *d, const XMM *s) {
    sse_neon_store_u8(d, vqsubq_u8(sse_neon_u8(d), sse_neon_u8(s)));
}
static inline void sse_paddusw(XMM *d, const XMM *s) {
    sse_neon_store_u16(d, vqaddq_u16(sse_neon_u16(d), sse_neon_u16(s)));
}
static inline void sse_psubusw(XMM *d, const XMM *s) {
    sse_neon_store_u16(d, vqsubq_u16(sse_neon_u16(d), sse_neon_u16(s)));
}
static inline void sse_pmullw(XMM *d, const XMM *s) {
    int16x8_t a = sse_neon_s16(d), b = sse_neon_s16(s);
    int32x4_t lo = vmull_s16(vget_low_s16(a), vget_low_s16(b));
    int32x4_t hi = vmull_high_s16(a, b);
    sse_neon_store_s16(d, vcombine_s16(vmovn_s32(lo), vmovn_s32(hi)));
}
static inline void sse_pmulhw(XMM *d, const XMM *s) {
    int16x8_t a = sse_neon_s16(d), b = sse_neon_s16(s);
    int32x4_t lo = vmull_s16(vget_low_s16(a), vget_low_s16(b));
    int32x4_t hi = vmull_high_s16(a, b);
    sse_neon_store_s16(d, vcombine_s16(vshrn_n_s32(lo, 16),
                                       vshrn_n_s32(hi, 16)));
}
static inline void sse_pmaddwd(XMM *d, const XMM *s) {
    int16x8_t a = sse_neon_s16(d), b = sse_neon_s16(s);
    int32x4_t lo = vmull_s16(vget_low_s16(a), vget_low_s16(b));
    int32x4_t hi = vmull_high_s16(a, b);
    sse_neon_store_s32(d, vpaddq_s32(lo, hi));
}
static inline void sse_pcmpeqb(XMM *d, const XMM *s) {
    sse_neon_store_u8(d, vceqq_u8(sse_neon_u8(d), sse_neon_u8(s)));
}
static inline void sse_pcmpeqw(XMM *d, const XMM *s) {
    sse_neon_store_u16(d, vceqq_u16(sse_neon_u16(d), sse_neon_u16(s)));
}
static inline void sse_pcmpeqd(XMM *d, const XMM *s) {
    sse_neon_store_u32(d, vceqq_u32(sse_neon_u32(d), sse_neon_u32(s)));
}
static inline void sse_pcmpgtb(XMM *d, const XMM *s) {
    sse_neon_store_s8(d, vcgtq_s8(sse_neon_s8(d), sse_neon_s8(s)));
}
static inline void sse_pcmpgtw(XMM *d, const XMM *s) {
    sse_neon_store_s16(d, vcgtq_s16(sse_neon_s16(d), sse_neon_s16(s)));
}
static inline void sse_pcmpgtd(XMM *d, const XMM *s) {
    sse_neon_store_s32(d, vcgtq_s32(sse_neon_s32(d), sse_neon_s32(s)));
}
static inline void sse_packsswb(XMM *d, const XMM *s) {
    int8x16_t v = vcombine_s8(vqmovn_s16(sse_neon_s16(d)),
                              vqmovn_s16(sse_neon_s16(s)));
    sse_neon_store_s8(d, v);
}
static inline void sse_packuswb(XMM *d, const XMM *s) {
    uint8x16_t v = vcombine_u8(vqmovun_s16(sse_neon_s16(d)),
                               vqmovun_s16(sse_neon_s16(s)));
    sse_neon_store_u8(d, v);
}
static inline void sse_packssdw(XMM *d, const XMM *s) {
    int16x8_t v = vcombine_s16(vqmovn_s32(sse_neon_s32(d)),
                               vqmovn_s32(sse_neon_s32(s)));
    sse_neon_store_s16(d, v);
}
static inline void sse_punpcklbw(XMM *d, const XMM *s) {
    sse_neon_store_u8(d, vzip1q_u8(sse_neon_u8(d), sse_neon_u8(s)));
}
static inline void sse_punpckhbw(XMM *d, const XMM *s) {
    sse_neon_store_u8(d, vzip2q_u8(sse_neon_u8(d), sse_neon_u8(s)));
}
static inline void sse_punpcklwd(XMM *d, const XMM *s) {
    sse_neon_store_u16(d, vzip1q_u16(sse_neon_u16(d), sse_neon_u16(s)));
}
static inline void sse_punpckhwd(XMM *d, const XMM *s) {
    sse_neon_store_u16(d, vzip2q_u16(sse_neon_u16(d), sse_neon_u16(s)));
}
static inline void sse_punpckldq(XMM *d, const XMM *s) {
    sse_neon_store_u32(d, vzip1q_u32(sse_neon_u32(d), sse_neon_u32(s)));
}
static inline void sse_punpckhdq(XMM *d, const XMM *s) {
    sse_neon_store_u32(d, vzip2q_u32(sse_neon_u32(d), sse_neon_u32(s)));
}
static inline void sse_punpcklqdq(XMM *d, const XMM *s) {
    sse_neon_store_u64(d, vzip1q_u64(sse_neon_u64(d), sse_neon_u64(s)));
}
static inline void sse_punpckhqdq(XMM *d, const XMM *s) {
    sse_neon_store_u64(d, vzip2q_u64(sse_neon_u64(d), sse_neon_u64(s)));
}
static inline void sse_pand(XMM *d, const XMM *s) {
    sse_neon_store_u32(d, vandq_u32(sse_neon_u32(d), sse_neon_u32(s)));
}
static inline void sse_por(XMM *d, const XMM *s) {
    sse_neon_store_u32(d, vorrq_u32(sse_neon_u32(d), sse_neon_u32(s)));
}
static inline void sse_pxor(XMM *d, const XMM *s) {
    sse_neon_store_u32(d, veorq_u32(sse_neon_u32(d), sse_neon_u32(s)));
}
static inline void sse_pandn(XMM *d, const XMM *s) {
    sse_neon_store_u32(d, vbicq_u32(sse_neon_u32(s), sse_neon_u32(d)));
}
static inline void sse_psllw(XMM *d, unsigned n) {
    if (n > 15) { sse_neon_store_u16(d, vdupq_n_u16(0)); return; }
    sse_neon_store_u16(d, vshlq_u16(sse_neon_u16(d), vdupq_n_s16((int16_t)n)));
}
static inline void sse_psrlw(XMM *d, unsigned n) {
    if (n > 15) { sse_neon_store_u16(d, vdupq_n_u16(0)); return; }
    sse_neon_store_u16(d, vshlq_u16(sse_neon_u16(d), vdupq_n_s16((int16_t)(-(int32_t)n))));
}
static inline void sse_psraw(XMM *d, unsigned n) {
    if (n > 15) n = 15;
    sse_neon_store_s16(d, vshlq_s16(sse_neon_s16(d), vdupq_n_s16((int16_t)(-(int32_t)n))));
}
static inline void sse_pslld(XMM *d, unsigned n) {
    if (n > 31) { sse_neon_store_u32(d, vdupq_n_u32(0)); return; }
    sse_neon_store_u32(d, vshlq_u32(sse_neon_u32(d), vdupq_n_s32((int32_t)n)));
}
static inline void sse_psrld(XMM *d, unsigned n) {
    if (n > 31) { sse_neon_store_u32(d, vdupq_n_u32(0)); return; }
    sse_neon_store_u32(d, vshlq_u32(sse_neon_u32(d), vdupq_n_s32((int32_t)(-(int32_t)n))));
}
static inline void sse_psrad(XMM *d, unsigned n) {
    if (n > 31) n = 31;
    sse_neon_store_s32(d, vshlq_s32(sse_neon_s32(d), vdupq_n_s32((int32_t)(-(int32_t)n))));
}
static inline void sse_psllq(XMM *d, unsigned n) {
    if (n > 63) { sse_neon_store_u64(d, vdupq_n_u64(0)); return; }
    sse_neon_store_u64(d, vshlq_u64(sse_neon_u64(d), vdupq_n_s64((int64_t)n)));
}
static inline void sse_psrlq(XMM *d, unsigned n) {
    if (n > 63) { sse_neon_store_u64(d, vdupq_n_u64(0)); return; }
    sse_neon_store_u64(d, vshlq_u64(sse_neon_u64(d), vdupq_n_s64((int64_t)(-(int32_t)n))));
}
static inline void sse_pslldq(XMM *d, unsigned n) {
    static const uint8_t lane[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    uint8x16_t v = sse_neon_u8(d);
    if (n == 0) return;
    if (n >= 16) { vst1q_u8(d->u8, vdupq_n_u8(0)); return; }
    /* TBL returns zero for every index >= 16, so i < n naturally yields the
     * high zero bytes without a second branch. */
    uint8x16_t idx = vsubq_u8(vld1q_u8(lane), vdupq_n_u8((uint8_t)n));
    vst1q_u8(d->u8, vqtbl1q_u8(v, idx));
}
static inline void sse_psrldq(XMM *d, unsigned n) {
    static const uint8_t lane[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    uint8x16_t v = sse_neon_u8(d);
    if (n == 0) return;
    if (n >= 16) { vst1q_u8(d->u8, vdupq_n_u8(0)); return; }
    uint8x16_t idx = vaddq_u8(vld1q_u8(lane), vdupq_n_u8((uint8_t)n));
    vst1q_u8(d->u8, vqtbl1q_u8(v, idx));
}
static inline void sse_pshufd(XMM *d, const XMM *s, unsigned imm) {
    XMM t;
    t.u32[0] = s->u32[(imm >> 0) & 3];
    t.u32[1] = s->u32[(imm >> 2) & 3];
    t.u32[2] = s->u32[(imm >> 4) & 3];
    t.u32[3] = s->u32[(imm >> 6) & 3];
    *d = t;
}
static inline void sse_pshuflw(XMM *d, const XMM *s, unsigned imm) {
    d->u64[1] = s->u64[1];
    d->u16[0] = s->u16[(imm >> 0) & 3];
    d->u16[1] = s->u16[(imm >> 2) & 3];
    d->u16[2] = s->u16[(imm >> 4) & 3];
    d->u16[3] = s->u16[(imm >> 6) & 3];
}
static inline void sse_pshufhw(XMM *d, const XMM *s, unsigned imm) {
    d->u64[0] = s->u64[0];
    d->u16[4] = s->u16[4 + ((imm >> 0) & 3)];
    d->u16[5] = s->u16[4 + ((imm >> 2) & 3)];
    d->u16[6] = s->u16[4 + ((imm >> 4) & 3)];
    d->u16[7] = s->u16[4 + ((imm >> 6) & 3)];
}
static inline uint32_t sse_pmovmskb(const XMM *s) {
    uint32_t r = 0;
    for (int i = 0; i < 16; i++) r |= (uint32_t)((s->u8[i] >> 7) & 1u) << i;
    return r;
}
static inline uint32_t sse_movmskps(const XMM *s) {
    return ((s->u32[0] >> 31) & 1u) | (((s->u32[1] >> 31) & 1u) << 1) |
           (((s->u32[2] >> 31) & 1u) << 2) | (((s->u32[3] >> 31) & 1u) << 3);
}
static inline uint32_t sse_movmskpd(const XMM *s) {
    return (uint32_t)((s->u64[0] >> 63) & 1u) |
           (uint32_t)(((s->u64[1] >> 63) & 1u) << 1);
}
static inline void sse_unpcklps(XMM *d, const XMM *s) {
    XMM t; t.u32[0] = d->u32[0]; t.u32[1] = s->u32[0];
    t.u32[2] = d->u32[1]; t.u32[3] = s->u32[1]; *d = t;
}
static inline void sse_unpckhps(XMM *d, const XMM *s) {
    XMM t; t.u32[0] = d->u32[2]; t.u32[1] = s->u32[2];
    t.u32[2] = d->u32[3]; t.u32[3] = s->u32[3]; *d = t;
}
static inline void sse_unpcklpd(XMM *d, const XMM *s) {
    XMM t; t.u64[0] = d->u64[0]; t.u64[1] = s->u64[0]; *d = t;
}
static inline void sse_unpckhpd(XMM *d, const XMM *s) {
    XMM t; t.u64[0] = d->u64[1]; t.u64[1] = s->u64[1]; *d = t;
}
static inline void sse_movlhps(XMM *d, const XMM *s) { d->u64[1] = s->u64[0]; }
static inline void sse_movhlps(XMM *d, const XMM *s) { d->u64[0] = s->u64[1]; }

#else /* !__aarch64__: portable scalar implementation */

static inline void sse_paddb(XMM *d, const XMM *s) {
    for (int i = 0; i < 16; i++) d->u8[i] = (uint8_t)(d->u8[i] + s->u8[i]);
}
static inline void sse_paddw(XMM *d, const XMM *s) {
    for (int i = 0; i < 8; i++) d->u16[i] = (uint16_t)(d->u16[i] + s->u16[i]);
}
static inline void sse_paddd(XMM *d, const XMM *s) {
    for (int i = 0; i < 4; i++) d->u32[i] = d->u32[i] + s->u32[i];
}
static inline void sse_paddq(XMM *d, const XMM *s) {
    for (int i = 0; i < 2; i++) d->u64[i] = d->u64[i] + s->u64[i];
}
static inline void sse_psubb(XMM *d, const XMM *s) {
    for (int i = 0; i < 16; i++) d->u8[i] = (uint8_t)(d->u8[i] - s->u8[i]);
}
static inline void sse_psubw(XMM *d, const XMM *s) {
    for (int i = 0; i < 8; i++) d->u16[i] = (uint16_t)(d->u16[i] - s->u16[i]);
}
static inline void sse_psubd(XMM *d, const XMM *s) {
    for (int i = 0; i < 4; i++) d->u32[i] = d->u32[i] - s->u32[i];
}
static inline void sse_psubq(XMM *d, const XMM *s) {
    for (int i = 0; i < 2; i++) d->u64[i] = d->u64[i] - s->u64[i];
}
static inline void sse_paddsb(XMM *d, const XMM *s) {
    for (int i = 0; i < 16; i++) d->i8[i] = sse_sat_i8((int32_t)d->i8[i] + s->i8[i]);
}
static inline void sse_psubsb(XMM *d, const XMM *s) {
    for (int i = 0; i < 16; i++) d->i8[i] = sse_sat_i8((int32_t)d->i8[i] - s->i8[i]);
}
static inline void sse_paddsw(XMM *d, const XMM *s) {
    for (int i = 0; i < 8; i++) d->i16[i] = sse_sat_i16((int32_t)d->i16[i] + s->i16[i]);
}
static inline void sse_psubsw(XMM *d, const XMM *s) {
    for (int i = 0; i < 8; i++) d->i16[i] = sse_sat_i16((int32_t)d->i16[i] - s->i16[i]);
}
static inline void sse_paddusb(XMM *d, const XMM *s) {
    for (int i = 0; i < 16; i++) d->u8[i] = sse_sat_u8((int32_t)d->u8[i] + s->u8[i]);
}
static inline void sse_psubusb(XMM *d, const XMM *s) {
    for (int i = 0; i < 16; i++) d->u8[i] = sse_sat_u8((int32_t)d->u8[i] - s->u8[i]);
}
static inline void sse_paddusw(XMM *d, const XMM *s) {
    for (int i = 0; i < 8; i++) d->u16[i] = sse_sat_u16((int32_t)d->u16[i] + s->u16[i]);
}
static inline void sse_psubusw(XMM *d, const XMM *s) {
    for (int i = 0; i < 8; i++) d->u16[i] = sse_sat_u16((int32_t)d->u16[i] - s->u16[i]);
}
static inline void sse_pmullw(XMM *d, const XMM *s) {
    for (int i = 0; i < 8; i++) d->u16[i] = (uint16_t)((int32_t)d->i16[i] * s->i16[i]);
}
static inline void sse_pmulhw(XMM *d, const XMM *s) {
    for (int i = 0; i < 8; i++)
        d->i16[i] = (int16_t)(((int32_t)d->i16[i] * s->i16[i]) >> 16);
}
static inline void sse_pmaddwd(XMM *d, const XMM *s) {
    for (int i = 0; i < 4; i++)
        d->i32[i] = (int32_t)d->i16[2 * i] * s->i16[2 * i] +
                    (int32_t)d->i16[2 * i + 1] * s->i16[2 * i + 1];
}
static inline void sse_pcmpeqb(XMM *d, const XMM *s) {
    for (int i = 0; i < 16; i++) d->u8[i] = d->u8[i] == s->u8[i] ? 0xFFu : 0u;
}
static inline void sse_pcmpeqw(XMM *d, const XMM *s) {
    for (int i = 0; i < 8; i++) d->u16[i] = d->u16[i] == s->u16[i] ? 0xFFFFu : 0u;
}
static inline void sse_pcmpeqd(XMM *d, const XMM *s) {
    for (int i = 0; i < 4; i++) d->u32[i] = d->u32[i] == s->u32[i] ? 0xFFFFFFFFu : 0u;
}
static inline void sse_pcmpgtb(XMM *d, const XMM *s) {
    for (int i = 0; i < 16; i++) d->u8[i] = d->i8[i] > s->i8[i] ? 0xFFu : 0u;
}
static inline void sse_pcmpgtw(XMM *d, const XMM *s) {
    for (int i = 0; i < 8; i++) d->u16[i] = d->i16[i] > s->i16[i] ? 0xFFFFu : 0u;
}
static inline void sse_pcmpgtd(XMM *d, const XMM *s) {
    for (int i = 0; i < 4; i++) d->u32[i] = d->i32[i] > s->i32[i] ? 0xFFFFFFFFu : 0u;
}
static inline void sse_packsswb(XMM *d, const XMM *s) {
    XMM t;
    for (int i = 0; i < 8; i++) t.i8[i] = sse_sat_i8(d->i16[i]);
    for (int i = 0; i < 8; i++) t.i8[8 + i] = sse_sat_i8(s->i16[i]);
    *d = t;
}
static inline void sse_packuswb(XMM *d, const XMM *s) {
    XMM t;
    for (int i = 0; i < 8; i++) t.u8[i] = sse_sat_u8(d->i16[i]);
    for (int i = 0; i < 8; i++) t.u8[8 + i] = sse_sat_u8(s->i16[i]);
    *d = t;
}
static inline void sse_packssdw(XMM *d, const XMM *s) {
    XMM t;
    for (int i = 0; i < 4; i++) t.i16[i] = sse_sat_i16(d->i32[i]);
    for (int i = 0; i < 4; i++) t.i16[4 + i] = sse_sat_i16(s->i32[i]);
    *d = t;
}
static inline void sse_punpcklbw(XMM *d, const XMM *s) {
    XMM t;
    for (int i = 0; i < 8; i++) { t.u8[2 * i] = d->u8[i]; t.u8[2 * i + 1] = s->u8[i]; }
    *d = t;
}
static inline void sse_punpckhbw(XMM *d, const XMM *s) {
    XMM t;
    for (int i = 0; i < 8; i++) { t.u8[2 * i] = d->u8[8 + i]; t.u8[2 * i + 1] = s->u8[8 + i]; }
    *d = t;
}
static inline void sse_punpcklwd(XMM *d, const XMM *s) {
    XMM t;
    for (int i = 0; i < 4; i++) { t.u16[2 * i] = d->u16[i]; t.u16[2 * i + 1] = s->u16[i]; }
    *d = t;
}
static inline void sse_punpckhwd(XMM *d, const XMM *s) {
    XMM t;
    for (int i = 0; i < 4; i++) { t.u16[2 * i] = d->u16[4 + i]; t.u16[2 * i + 1] = s->u16[4 + i]; }
    *d = t;
}
static inline void sse_punpckldq(XMM *d, const XMM *s) {
    XMM t;
    for (int i = 0; i < 2; i++) { t.u32[2 * i] = d->u32[i]; t.u32[2 * i + 1] = s->u32[i]; }
    *d = t;
}
static inline void sse_punpckhdq(XMM *d, const XMM *s) {
    XMM t;
    for (int i = 0; i < 2; i++) { t.u32[2 * i] = d->u32[2 + i]; t.u32[2 * i + 1] = s->u32[2 + i]; }
    *d = t;
}
static inline void sse_punpcklqdq(XMM *d, const XMM *s) {
    XMM t; t.u64[0] = d->u64[0]; t.u64[1] = s->u64[0]; *d = t;
}
static inline void sse_punpckhqdq(XMM *d, const XMM *s) {
    XMM t; t.u64[0] = d->u64[1]; t.u64[1] = s->u64[1]; *d = t;
}
static inline void sse_pand(XMM *d, const XMM *s) {
    d->u64[0] &= s->u64[0]; d->u64[1] &= s->u64[1];
}
static inline void sse_por(XMM *d, const XMM *s) {
    d->u64[0] |= s->u64[0]; d->u64[1] |= s->u64[1];
}
static inline void sse_pxor(XMM *d, const XMM *s) {
    d->u64[0] ^= s->u64[0]; d->u64[1] ^= s->u64[1];
}
static inline void sse_pandn(XMM *d, const XMM *s) {
    d->u64[0] = ~d->u64[0] & s->u64[0];
    d->u64[1] = ~d->u64[1] & s->u64[1];
}
static inline void sse_psllw(XMM *d, unsigned n) {
    if (n > 15) { d->u64[0] = d->u64[1] = 0; return; }
    for (int i = 0; i < 8; i++) d->u16[i] = (uint16_t)(d->u16[i] << n);
}
static inline void sse_psrlw(XMM *d, unsigned n) {
    if (n > 15) { d->u64[0] = d->u64[1] = 0; return; }
    for (int i = 0; i < 8; i++) d->u16[i] = (uint16_t)(d->u16[i] >> n);
}
static inline void sse_psraw(XMM *d, unsigned n) {
    if (n > 15) n = 15;
    for (int i = 0; i < 8; i++) d->i16[i] = (int16_t)(d->i16[i] >> n);
}
static inline void sse_pslld(XMM *d, unsigned n) {
    if (n > 31) { d->u64[0] = d->u64[1] = 0; return; }
    for (int i = 0; i < 4; i++) d->u32[i] <<= n;
}
static inline void sse_psrld(XMM *d, unsigned n) {
    if (n > 31) { d->u64[0] = d->u64[1] = 0; return; }
    for (int i = 0; i < 4; i++) d->u32[i] >>= n;
}
static inline void sse_psrad(XMM *d, unsigned n) {
    if (n > 31) n = 31;
    for (int i = 0; i < 4; i++) d->i32[i] >>= n;
}
static inline void sse_psllq(XMM *d, unsigned n) {
    if (n > 63) { d->u64[0] = d->u64[1] = 0; return; }
    d->u64[0] <<= n; d->u64[1] <<= n;
}
static inline void sse_psrlq(XMM *d, unsigned n) {
    if (n > 63) { d->u64[0] = d->u64[1] = 0; return; }
    d->u64[0] >>= n; d->u64[1] >>= n;
}
static inline void sse_pslldq(XMM *d, unsigned n) {
    XMM t; memset(&t, 0, sizeof t);
    if (n < 16) for (int i = (int)n; i < 16; i++) t.u8[i] = d->u8[i - n];
    *d = t;
}
static inline void sse_psrldq(XMM *d, unsigned n) {
    XMM t; memset(&t, 0, sizeof t);
    if (n < 16) for (int i = 0; i + n < 16; i++) t.u8[i] = d->u8[i + n];
    *d = t;
}
static inline void sse_pshufd(XMM *d, const XMM *s, unsigned imm) {
    XMM t;
    t.u32[0] = s->u32[(imm >> 0) & 3]; t.u32[1] = s->u32[(imm >> 2) & 3];
    t.u32[2] = s->u32[(imm >> 4) & 3]; t.u32[3] = s->u32[(imm >> 6) & 3];
    *d = t;
}
static inline void sse_pshuflw(XMM *d, const XMM *s, unsigned imm) {
    d->u64[1] = s->u64[1];
    d->u16[0] = s->u16[(imm >> 0) & 3]; d->u16[1] = s->u16[(imm >> 2) & 3];
    d->u16[2] = s->u16[(imm >> 4) & 3]; d->u16[3] = s->u16[(imm >> 6) & 3];
}
static inline void sse_pshufhw(XMM *d, const XMM *s, unsigned imm) {
    d->u64[0] = s->u64[0];
    d->u16[4] = s->u16[4 + ((imm >> 0) & 3)]; d->u16[5] = s->u16[4 + ((imm >> 2) & 3)];
    d->u16[6] = s->u16[4 + ((imm >> 4) & 3)]; d->u16[7] = s->u16[4 + ((imm >> 6) & 3)];
}
static inline uint32_t sse_pmovmskb(const XMM *s) {
    uint32_t r = 0;
    for (int i = 0; i < 16; i++) r |= (uint32_t)((s->u8[i] >> 7) & 1u) << i;
    return r;
}
static inline uint32_t sse_movmskps(const XMM *s) {
    return ((s->u32[0] >> 31) & 1u) | (((s->u32[1] >> 31) & 1u) << 1) |
           (((s->u32[2] >> 31) & 1u) << 2) | (((s->u32[3] >> 31) & 1u) << 3);
}
static inline uint32_t sse_movmskpd(const XMM *s) {
    return (uint32_t)((s->u64[0] >> 63) & 1u) |
           (uint32_t)(((s->u64[1] >> 63) & 1u) << 1);
}
static inline void sse_unpcklps(XMM *d, const XMM *s) {
    XMM t; t.u32[0] = d->u32[0]; t.u32[1] = s->u32[0];
    t.u32[2] = d->u32[1]; t.u32[3] = s->u32[1]; *d = t;
}
static inline void sse_unpckhps(XMM *d, const XMM *s) {
    XMM t; t.u32[0] = d->u32[2]; t.u32[1] = s->u32[2];
    t.u32[2] = d->u32[3]; t.u32[3] = s->u32[3]; *d = t;
}
static inline void sse_unpcklpd(XMM *d, const XMM *s) {
    XMM t; t.u64[0] = d->u64[0]; t.u64[1] = s->u64[0]; *d = t;
}
static inline void sse_unpckhpd(XMM *d, const XMM *s) {
    XMM t; t.u64[0] = d->u64[1]; t.u64[1] = s->u64[1]; *d = t;
}
static inline void sse_movlhps(XMM *d, const XMM *s) { d->u64[1] = s->u64[0]; }
static inline void sse_movhlps(XMM *d, const XMM *s) { d->u64[0] = s->u64[1]; }

#endif /* __aarch64__ */

#endif /* WC3_CPU_SSE_INT_H */
