/*
 * cpu_sse_fp.h - packed SSE floating-point helpers.
 *
 * The generated corpus used to expand every packed add/sub/mul/div/sqrt and
 * min/max into four scalar lane statements.  AArch64 has one ASIMD
 * instruction for the arithmetic forms, so the helpers below are the single
 * lowering target on both the product and the headless host build.
 *
 * Exactness:
 *  - The host FPCR is owned by the guest thread; FZ/DN are cleared by
 *    x87_clear_ftz() and rounding follows the guest x87 CW, exactly as the
 *    scalar lane code did.
 *  - add/sub/mul/div/sqrt fall back to the scalar lane code when the vector
 *    result contains a NaN.  That keeps the current NaN behaviour (payload,
 *    quieting and sign) identical to the scalar code instead of replacing it
 *    with AArch64's NaN propagation rules.  Finite, infinite, denormal and
 *    signed-zero results stay on the vector path.
 *  - MINPS/MAXPS are not VMIN/VMAX.  x86 defines them as
 *    (dst OP src) ? dst : src, so NaN, equality and +/-0 select the second
 *    operand.  The vector form is an ordered compare plus bit select, which
 *    preserves the source bits.
 *
 * This header is included after cpu_sse_int.h; it is always used, with no
 * target flag or opt-in switch.
 */
#ifndef WC3_CPU_SSE_FP_H
#define WC3_CPU_SSE_FP_H

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

/* ---- packed single precision ---- */

static inline void sse_addps(XMM *d, const XMM *s)
{
#if defined(__aarch64__)
    float32x4_t a = vld1q_f32(d->f32);
    float32x4_t b = vld1q_f32(s->f32);
    float32x4_t r = vaddq_f32(a, b);
    if (vminvq_u32(vceqq_f32(r, r)) == 0u) {
        for (int i = 0; i < 4; i++) d->f32[i] = d->f32[i] + s->f32[i];
        return;
    }
    vst1q_f32(d->f32, r);
#else
    for (int i = 0; i < 4; i++) d->f32[i] = d->f32[i] + s->f32[i];
#endif
}

static inline void sse_subps(XMM *d, const XMM *s)
{
#if defined(__aarch64__)
    float32x4_t a = vld1q_f32(d->f32);
    float32x4_t b = vld1q_f32(s->f32);
    float32x4_t r = vsubq_f32(a, b);
    if (vminvq_u32(vceqq_f32(r, r)) == 0u) {
        for (int i = 0; i < 4; i++) d->f32[i] = d->f32[i] - s->f32[i];
        return;
    }
    vst1q_f32(d->f32, r);
#else
    for (int i = 0; i < 4; i++) d->f32[i] = d->f32[i] - s->f32[i];
#endif
}

static inline void sse_mulps(XMM *d, const XMM *s)
{
#if defined(__aarch64__)
    float32x4_t a = vld1q_f32(d->f32);
    float32x4_t b = vld1q_f32(s->f32);
    float32x4_t r = vmulq_f32(a, b);
    if (vminvq_u32(vceqq_f32(r, r)) == 0u) {
        for (int i = 0; i < 4; i++) d->f32[i] = d->f32[i] * s->f32[i];
        return;
    }
    vst1q_f32(d->f32, r);
#else
    for (int i = 0; i < 4; i++) d->f32[i] = d->f32[i] * s->f32[i];
#endif
}

static inline void sse_divps(XMM *d, const XMM *s)
{
#if defined(__aarch64__)
    float32x4_t a = vld1q_f32(d->f32);
    float32x4_t b = vld1q_f32(s->f32);
    float32x4_t r = vdivq_f32(a, b);
    if (vminvq_u32(vceqq_f32(r, r)) == 0u) {
        for (int i = 0; i < 4; i++) d->f32[i] = d->f32[i] / s->f32[i];
        return;
    }
    vst1q_f32(d->f32, r);
#else
    for (int i = 0; i < 4; i++) d->f32[i] = d->f32[i] / s->f32[i];
#endif
}

static inline void sse_sqrtps(XMM *d, const XMM *s)
{
#if defined(__aarch64__)
    float32x4_t b = vld1q_f32(s->f32);
    float32x4_t r = vsqrtq_f32(b);
    if (vminvq_u32(vceqq_f32(r, r)) == 0u) {
        for (int i = 0; i < 4; i++) d->f32[i] = sqrtf(s->f32[i]);
        return;
    }
    vst1q_f32(d->f32, r);
#else
    for (int i = 0; i < 4; i++) d->f32[i] = sqrtf(s->f32[i]);
#endif
}

static inline void sse_minps(XMM *d, const XMM *s)
{
#if defined(__aarch64__)
    float32x4_t a = vld1q_f32(d->f32);
    float32x4_t b = vld1q_f32(s->f32);
    vst1q_f32(d->f32, vbslq_f32(vcltq_f32(a, b), a, b));
#else
    for (int i = 0; i < 4; i++) d->f32[i] = (d->f32[i] < s->f32[i]) ? d->f32[i] : s->f32[i];
#endif
}

static inline void sse_maxps(XMM *d, const XMM *s)
{
#if defined(__aarch64__)
    float32x4_t a = vld1q_f32(d->f32);
    float32x4_t b = vld1q_f32(s->f32);
    vst1q_f32(d->f32, vbslq_f32(vcgtq_f32(a, b), a, b));
#else
    for (int i = 0; i < 4; i++) d->f32[i] = (d->f32[i] > s->f32[i]) ? d->f32[i] : s->f32[i];
#endif
}

/* ---- packed double precision ---- */

static inline void sse_addpd(XMM *d, const XMM *s)
{
#if defined(__aarch64__)
    float64x2_t a = vld1q_f64(d->f64);
    float64x2_t b = vld1q_f64(s->f64);
    float64x2_t r = vaddq_f64(a, b);
    uint64x2_t ok = vceqq_f64(r, r);
    if (vgetq_lane_u64(ok, 0) == 0u || vgetq_lane_u64(ok, 1) == 0u) {
        for (int i = 0; i < 2; i++) d->f64[i] = d->f64[i] + s->f64[i];
        return;
    }
    vst1q_f64(d->f64, r);
#else
    for (int i = 0; i < 2; i++) d->f64[i] = d->f64[i] + s->f64[i];
#endif
}

static inline void sse_subpd(XMM *d, const XMM *s)
{
#if defined(__aarch64__)
    float64x2_t a = vld1q_f64(d->f64);
    float64x2_t b = vld1q_f64(s->f64);
    float64x2_t r = vsubq_f64(a, b);
    uint64x2_t ok = vceqq_f64(r, r);
    if (vgetq_lane_u64(ok, 0) == 0u || vgetq_lane_u64(ok, 1) == 0u) {
        for (int i = 0; i < 2; i++) d->f64[i] = d->f64[i] - s->f64[i];
        return;
    }
    vst1q_f64(d->f64, r);
#else
    for (int i = 0; i < 2; i++) d->f64[i] = d->f64[i] - s->f64[i];
#endif
}

static inline void sse_mulpd(XMM *d, const XMM *s)
{
#if defined(__aarch64__)
    float64x2_t a = vld1q_f64(d->f64);
    float64x2_t b = vld1q_f64(s->f64);
    float64x2_t r = vmulq_f64(a, b);
    uint64x2_t ok = vceqq_f64(r, r);
    if (vgetq_lane_u64(ok, 0) == 0u || vgetq_lane_u64(ok, 1) == 0u) {
        for (int i = 0; i < 2; i++) d->f64[i] = d->f64[i] * s->f64[i];
        return;
    }
    vst1q_f64(d->f64, r);
#else
    for (int i = 0; i < 2; i++) d->f64[i] = d->f64[i] * s->f64[i];
#endif
}

static inline void sse_divpd(XMM *d, const XMM *s)
{
#if defined(__aarch64__)
    float64x2_t a = vld1q_f64(d->f64);
    float64x2_t b = vld1q_f64(s->f64);
    float64x2_t r = vdivq_f64(a, b);
    uint64x2_t ok = vceqq_f64(r, r);
    if (vgetq_lane_u64(ok, 0) == 0u || vgetq_lane_u64(ok, 1) == 0u) {
        for (int i = 0; i < 2; i++) d->f64[i] = d->f64[i] / s->f64[i];
        return;
    }
    vst1q_f64(d->f64, r);
#else
    for (int i = 0; i < 2; i++) d->f64[i] = d->f64[i] / s->f64[i];
#endif
}

static inline void sse_sqrtpd(XMM *d, const XMM *s)
{
#if defined(__aarch64__)
    float64x2_t b = vld1q_f64(s->f64);
    float64x2_t r = vsqrtq_f64(b);
    uint64x2_t ok = vceqq_f64(r, r);
    if (vgetq_lane_u64(ok, 0) == 0u || vgetq_lane_u64(ok, 1) == 0u) {
        for (int i = 0; i < 2; i++) d->f64[i] = sqrt(s->f64[i]);
        return;
    }
    vst1q_f64(d->f64, r);
#else
    for (int i = 0; i < 2; i++) d->f64[i] = sqrt(s->f64[i]);
#endif
}

static inline void sse_minpd(XMM *d, const XMM *s)
{
#if defined(__aarch64__)
    float64x2_t a = vld1q_f64(d->f64);
    float64x2_t b = vld1q_f64(s->f64);
    vst1q_f64(d->f64, vbslq_f64(vcltq_f64(a, b), a, b));
#else
    for (int i = 0; i < 2; i++) d->f64[i] = (d->f64[i] < s->f64[i]) ? d->f64[i] : s->f64[i];
#endif
}

static inline void sse_maxpd(XMM *d, const XMM *s)
{
#if defined(__aarch64__)
    float64x2_t a = vld1q_f64(d->f64);
    float64x2_t b = vld1q_f64(s->f64);
    vst1q_f64(d->f64, vbslq_f64(vcgtq_f64(a, b), a, b));
#else
    for (int i = 0; i < 2; i++) d->f64[i] = (d->f64[i] > s->f64[i]) ? d->f64[i] : s->f64[i];
#endif
}

#endif /* WC3_CPU_SSE_FP_H */
