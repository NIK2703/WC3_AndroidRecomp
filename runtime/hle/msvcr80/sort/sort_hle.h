/*
 * sort_hle.h - target-side MSVCR80 qsort.
 *
 * The comparison callback is lifted guest code, so qsort has to enter the
 * guest dispatcher with a cdecl frame of two guest pointers.  The as-built
 * implementation already does that; this slice moves it behind the shim and
 * replaces its one-per-thread scratch buffer with a per-call guest allocation,
 * which also makes concurrent qsort calls safe during the migration.
 */
#ifndef WC3_HLE_MSVCR80_SORT_H
#define WC3_HLE_MSVCR80_SORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the qsort row over the as-built table.  Returns 1 on success, or
 * -1 when the row is missing.  Must run before guest threads start. */
int msvcr80_sort_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_MSVCR80_SORT_H */
