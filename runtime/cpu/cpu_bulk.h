/*
 * cpu_bulk.h - bulk string-fill helpers.
 *
 * REP STOSW/STOSD used to write one element at a time.  The pattern is a
 * scalar value repeated across a buffer, which maps directly to a NEON
 * broadcast + unaligned store on AArch64 and to a short scalar loop on the
 * host.  REP STOSB already uses memset; this header deliberately covers only
 * the wider patterns, where memset would change the byte pattern.
 */
#ifndef WC3_CPU_BULK_H
#define WC3_CPU_BULK_H

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

static inline void rep_stosw(uint32_t dst, uint16_t val, uint32_t n)
{
#if defined(__aarch64__)
    uint16x8_t q = vdupq_n_u16(val);
    while (n >= 8) {
        vst1q_u16((uint16_t *)(uintptr_t)dst, q);
        dst += 16;
        n -= 8;
    }
#endif
    while (n--) {
        memcpy((void *)(uintptr_t)dst, &val, sizeof val);
        dst += 2;
    }
}

static inline void rep_stosd(uint32_t dst, uint32_t val, uint32_t n)
{
#if defined(__aarch64__)
    uint32x4_t q = vdupq_n_u32(val);
    while (n >= 4) {
        vst1q_u32((uint32_t *)(uintptr_t)dst, q);
        dst += 16;
        n -= 4;
    }
#endif
    while (n--) {
        memcpy((void *)(uintptr_t)dst, &val, sizeof val);
        dst += 4;
    }
}

#endif /* WC3_CPU_BULK_H */
