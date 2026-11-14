/*
 * x86intrin.h - the nine SSE intrinsics DXVK actually uses, on aarch64.
 *
 * DXVK is an x86 project and reaches for <x86intrin.h> in exactly one place
 * (src/util/util_bit.h).  The whole surface it needs there is nine operations
 * on 16-byte vectors: compare-equal for bytes and floats, bitwise and, a
 * movemask, unaligned loads/stores and a spin hint.  clang's vector extension
 * expresses all of them directly, so no DXVK source has to change.
 *
 * `_mm_movemask_epi8` is on the DXVK render-thread hot path.  Two 64-bit
 * loads plus a multiply compress eight sign bits at a time; the previous
 * per-byte loop made the compiler extract all sixteen vector lanes.  The
 * multiply constant is the standard 8-to-8 bit gather and preserves the x86
 * byte order on little-endian targets (the only targets this shim supports).
 */
#ifndef WC3_ANDROID_DXVK_X86INTRIN_H
#define WC3_ANDROID_DXVK_X86INTRIN_H

#pragma once

#include <stdint.h>

typedef char  __attribute__((vector_size(16))) __m128i;
typedef float __attribute__((vector_size(16))) __m128;

static inline __m128i _mm_load_si128(const __m128i *p) { return *p; }
static inline void    _mm_store_si128(__m128i *p, __m128i v) { *p = v; }
static inline __m128  _mm_load_ps(const float *p)
{
    __m128 v;
    __builtin_memcpy(&v, p, 16);
    return v;
}
static inline void    _mm_store_ps(float *p, __m128 v) { __builtin_memcpy(p, &v, 16); }

static inline __m128i _mm_cmpeq_epi8(__m128i a, __m128i b) { return a == b; }
static inline __m128  _mm_cmpeq_ps(__m128 a, __m128 b) { return a == b; }
static inline __m128i _mm_and_si128(__m128i a, __m128i b) { return a & b; }
static inline __m128  _mm_and_ps(__m128 a, __m128 b)
{
    __m128i r = ((__m128i)a) & ((__m128i)b);
    return (__m128)r;
}

static inline unsigned wc3_movemask8(uint64_t v)
{
    return (unsigned)(((v & UINT64_C(0x8080808080808080)) *
                       UINT64_C(0x0002040810204081)) >> 56);
}

static inline int _mm_movemask_epi8(__m128i a)
{
    uint64_t lo, hi;
    __builtin_memcpy(&lo, (const void *)&a, sizeof lo);
    __builtin_memcpy(&hi, (const char *)&a + sizeof lo, sizeof hi);
    return (int)(wc3_movemask8(lo) | (wc3_movemask8(hi) << 8));
}

/* PAUSE is a spin-loop hint.  aarch64 spells it YIELD; a bare compiler
 * barrier would be correct but would spin harder on the same core. */
static inline void _mm_pause(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

/* A few other x86/MSVC helpers that leak through the same headers. */
static inline void _ReadWriteBarrier(void) { __asm__ __volatile__("" ::: "memory"); }
static inline void _mm_sfence(void) { __asm__ __volatile__("" ::: "memory"); }
static inline void _mm_mfence(void) { __asm__ __volatile__("" ::: "memory"); }
static inline void _mm_lfence(void) { __asm__ __volatile__("" ::: "memory"); }

#endif
