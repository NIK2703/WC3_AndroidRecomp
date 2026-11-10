/*
 * heap_hle.h - target-side MSVCR80 CRT heap subset.
 *
 * The full msvcr80 migration is later in H6.2.  malloc/free/realloc/calloc are
 * a self-contained slice: they already use the guest heap, and moving them now
 * keeps the header arithmetic in the shim instead of in the as-built HLE.
 */
#ifndef WC3_HLE_MSVCR80_HEAP_H
#define WC3_HLE_MSVCR80_HEAP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the CRT heap rows over the as-built table.  Returns the number of
 * rows installed, or -1 if any row is missing.  Must run before guest threads. */
int wc3_hle_msvcr80_heap_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_MSVCR80_HEAP_H */
