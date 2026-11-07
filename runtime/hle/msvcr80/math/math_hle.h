/*
 * math_hle.h - target-side MSVCR80 floating-point imports.
 *
 * This is the first slice of the msvcr80 migration.  It covers the three
 * calling conventions the game actually uses:
 *
 *   - cdecl double arguments on the guest stack, result on the x87 stack;
 *   - the compiler-internal _CI* helpers, which consume their operands from
 *     the x87 stack and push the result back;
 *   - the SSE2 _libm_sse2_* helpers, which take xmm0/xmm1 and return xmm0.
 *
 * FP control-word functions (_control87/_controlfp_s/_clearfp) are not part of
 * this slice: the as-built handlers only return the default control word and
 * do not model the guest FPU, so moving them would preserve a stub rather than
 * a contract.
 */
#ifndef WC3_HLE_MSVCR80_MATH_H
#define WC3_HLE_MSVCR80_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the math rows over the as-built table.  Returns the number of rows
 * installed, or -1 if any required row is missing.  Must run before guest
 * threads. */
int wc3_hle_msvcr80_math_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_MSVCR80_MATH_H */
