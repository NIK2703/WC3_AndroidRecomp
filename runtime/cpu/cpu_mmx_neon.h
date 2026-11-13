/*
 * cpu_mmx_neon.h - AArch64 ASIMD implementations of the MMX helpers.
 *
 * Included by cpu.h only when compiling for aarch64.  The scalar helpers stay
 * in cpu.h for the x86-64 headless build and for any non-NEON target.  Every
 * operation here is integer or saturating integer: guest-visible FP stays in
 * x87.h/sse helpers and is not touched by this header.
 *
 * The 64-bit MMX values are moved through uint64x1/uint8x8/uint16x4/uint32x2
 * lanes.  vcreate_u64/vget_lane_u64 keep the representation as a plain
 * uint64_t at every call boundary, so the generated C sees the same signature
 * as the scalar helpers.
 */
#ifndef WC3_CPU_MMX_NEON_H
#define WC3_CPU_MMX_NEON_H

#include <arm_neon.h>

static inline uint16x4_t mmx_neon_u16(uint64_t x) {
    return vreinterpret_u16_u64(vcreate_u64(x));
}
static inline uint64_t mmx_neon_u64_u16(uint16x4_t v) {
    return vget_lane_u64(vreinterpret_u64_u16(v), 0);
}
static inline uint8x8_t mmx_neon_u8(uint64_t x) {
    return vreinterpret_u8_u64(vcreate_u64(x));
}
static inline uint64_t mmx_neon_u64_u8(uint8x8_t v) {
    return vget_lane_u64(vreinterpret_u64_u8(v), 0);
}
static inline uint32x2_t mmx_neon_u32(uint64_t x) {
    return vreinterpret_u32_u64(vcreate_u64(x));
}
static inline uint64_t mmx_neon_u64_u32(uint32x2_t v) {
    return vget_lane_u64(vreinterpret_u64_u32(v), 0);
}
static inline int16x4_t mmx_neon_s16(uint64_t x) {
    return vreinterpret_s16_u64(vcreate_u64(x));
}
static inline int8x8_t mmx_neon_s8(uint64_t x) {
    return vreinterpret_s8_u64(vcreate_u64(x));
}
static inline int32x2_t mmx_neon_s32(uint64_t x) {
    return vreinterpret_s32_u64(vcreate_u64(x));
}

static inline uint64_t mmx_paddw(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u16(vadd_u16(mmx_neon_u16(a), mmx_neon_u16(b)));
}
static inline uint64_t mmx_psubw(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u16(vsub_u16(mmx_neon_u16(a), mmx_neon_u16(b)));
}
static inline uint64_t mmx_paddd(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u32(vadd_u32(mmx_neon_u32(a), mmx_neon_u32(b)));
}
static inline uint64_t mmx_psubd(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u32(vsub_u32(mmx_neon_u32(a), mmx_neon_u32(b)));
}
static inline uint64_t mmx_paddsw(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u16(vreinterpret_u16_s16(
        vqadd_s16(mmx_neon_s16(a), mmx_neon_s16(b))));
}
static inline uint64_t mmx_psubsw(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u16(vreinterpret_u16_s16(
        vqsub_s16(mmx_neon_s16(a), mmx_neon_s16(b))));
}
static inline uint64_t mmx_pmulhw(uint64_t a, uint64_t b) {
    int32x4_t p = vmull_s16(mmx_neon_s16(a), mmx_neon_s16(b));
    return mmx_neon_u64_u16(vreinterpret_u16_s16(vshrn_n_s32(p, 16)));
}
static inline uint64_t mmx_pmullw(uint64_t a, uint64_t b) {
    int32x4_t p = vmull_s16(mmx_neon_s16(a), mmx_neon_s16(b));
    return mmx_neon_u64_u16(vreinterpret_u16_s16(vmovn_s32(p)));
}
static inline uint64_t mmx_pmaddwd(uint64_t a, uint64_t b) {
    int32x4_t p = vmull_s16(mmx_neon_s16(a), mmx_neon_s16(b));
    int32x2_t s = vpadd_s32(vget_low_s32(p), vget_high_s32(p));
    return vget_lane_u64(vreinterpret_u64_s32(s), 0);
}
static inline uint64_t mmx_punpcklwd(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u16(vzip1_u16(mmx_neon_u16(a), mmx_neon_u16(b)));
}
static inline uint64_t mmx_punpckhwd(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u16(vzip2_u16(mmx_neon_u16(a), mmx_neon_u16(b)));
}
static inline uint64_t mmx_punpcklbw(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u8(vzip1_u8(mmx_neon_u8(a), mmx_neon_u8(b)));
}
static inline uint64_t mmx_punpckhbw(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u8(vzip2_u8(mmx_neon_u8(a), mmx_neon_u8(b)));
}
static inline uint64_t mmx_punpckldq(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u32(vzip1_u32(mmx_neon_u32(a), mmx_neon_u32(b)));
}
static inline uint64_t mmx_punpckhdq(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u32(vzip2_u32(mmx_neon_u32(a), mmx_neon_u32(b)));
}
static inline uint64_t mmx_psllq(uint64_t a, uint32_t c) {
    return c > 63 ? 0 : a << c;
}
static inline uint64_t mmx_psrlq(uint64_t a, uint32_t c) {
    return c > 63 ? 0 : a >> c;
}
static inline uint64_t mmx_psllw(uint64_t a, uint32_t c) {
    if (c > 15) return 0;
    return mmx_neon_u64_u16(vshl_u16(mmx_neon_u16(a),
                                    vdup_n_s16((int16_t)c)));
}
static inline uint64_t mmx_psrlw(uint64_t a, uint32_t c) {
    if (c > 15) return 0;
    return mmx_neon_u64_u16(vshl_u16(mmx_neon_u16(a),
                                    vdup_n_s16((int16_t)(-(int32_t)c))));
}
static inline uint64_t mmx_psraw(uint64_t a, uint32_t c) {
    if (c > 15) c = 15;
    return mmx_neon_u64_u16(vreinterpret_u16_s16(
        vshl_s16(mmx_neon_s16(a), vdup_n_s16((int16_t)(-(int32_t)c)))));
}
static inline uint64_t mmx_pslld(uint64_t a, uint32_t c) {
    if (c > 31) return 0;
    return mmx_neon_u64_u32(vshl_u32(mmx_neon_u32(a),
                                    vdup_n_s32((int32_t)c)));
}
static inline uint64_t mmx_psrld(uint64_t a, uint32_t c) {
    if (c > 31) return 0;
    return mmx_neon_u64_u32(vshl_u32(mmx_neon_u32(a),
                                    vdup_n_s32((int32_t)(-(int32_t)c))));
}
static inline uint64_t mmx_psrad(uint64_t a, uint32_t c) {
    if (c > 31) c = 31;
    return mmx_neon_u64_u32(vreinterpret_u32_s32(
        vshl_s32(mmx_neon_s32(a), vdup_n_s32((int32_t)(-(int32_t)c)))));
}
static inline uint64_t mmx_pshufw(uint64_t a, uint32_t imm) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++)
        r |= MM_PUT_W(i, MM_W(a, (imm >> (2 * i)) & 3));
    return r;
}
static inline uint64_t mmx_packssdw(uint64_t a, uint64_t b) {
    int32x4_t v = vcombine_s32(mmx_neon_s32(a), mmx_neon_s32(b));
    return mmx_neon_u64_u16(vreinterpret_u16_s16(vqmovn_s32(v)));
}
static inline uint64_t mmx_packuswb(uint64_t a, uint64_t b) {
    int16x8_t v = vcombine_s16(mmx_neon_s16(a), mmx_neon_s16(b));
    return mmx_neon_u64_u8(vqmovun_s16(v));
}
static inline uint64_t mmx_pcmpeqw(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u16(vceq_u16(mmx_neon_u16(a), mmx_neon_u16(b)));
}
static inline uint64_t mmx_pcmpgtw(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u16(vreinterpret_u16_s16(
        vcgt_s16(mmx_neon_s16(a), mmx_neon_s16(b))));
}
static inline uint64_t mmx_pcmpeqd(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u32(vceq_u32(mmx_neon_u32(a), mmx_neon_u32(b)));
}
static inline uint64_t mmx_pcmpgtd(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u32(vreinterpret_u32_s32(
        vcgt_s32(mmx_neon_s32(a), mmx_neon_s32(b))));
}
static inline uint64_t mmx_paddb(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u8(vadd_u8(mmx_neon_u8(a), mmx_neon_u8(b)));
}
static inline uint64_t mmx_psubb(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u8(vsub_u8(mmx_neon_u8(a), mmx_neon_u8(b)));
}
static inline uint64_t mmx_paddsb(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u8(vreinterpret_u8_s8(
        vqadd_s8(mmx_neon_s8(a), mmx_neon_s8(b))));
}
static inline uint64_t mmx_psubsb(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u8(vreinterpret_u8_s8(
        vqsub_s8(mmx_neon_s8(a), mmx_neon_s8(b))));
}
static inline uint64_t mmx_paddusb(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u8(vqadd_u8(mmx_neon_u8(a), mmx_neon_u8(b)));
}
static inline uint64_t mmx_psubusb(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u8(vqsub_u8(mmx_neon_u8(a), mmx_neon_u8(b)));
}
static inline uint64_t mmx_paddusw(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u16(vqadd_u16(mmx_neon_u16(a), mmx_neon_u16(b)));
}
static inline uint64_t mmx_psubusw(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u16(vqsub_u16(mmx_neon_u16(a), mmx_neon_u16(b)));
}
static inline uint64_t mmx_pcmpeqb(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u8(vceq_u8(mmx_neon_u8(a), mmx_neon_u8(b)));
}
static inline uint64_t mmx_pcmpgtb(uint64_t a, uint64_t b) {
    return mmx_neon_u64_u8(vreinterpret_u8_s8(
        vcgt_s8(mmx_neon_s8(a), mmx_neon_s8(b))));
}
static inline uint64_t mmx_packsswb(uint64_t a, uint64_t b) {
    int16x8_t v = vcombine_s16(mmx_neon_s16(a), mmx_neon_s16(b));
    return mmx_neon_u64_u8(vreinterpret_u8_s8(vqmovn_s16(v)));
}

#endif /* WC3_CPU_MMX_NEON_H */
