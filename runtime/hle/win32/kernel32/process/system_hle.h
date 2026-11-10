/*
 * system_hle.h - target-side kernel32 version/machine-information subset.
 *
 * These handlers are fixed machine facts and structure writers: they do not
 * depend on the legacy registry, handles, paths, or threads.  They are moved
 * together so the OSVERSIONINFOA size contract and the SYSTEM_INFO/MEMORYSTATUS
 * layouts live in target code with a T0 test instead of in the as-built HLE.
 *
 * GetModuleHandleW is an addition: it returns the fixed guest image bases for
 * the modules the corpus loads, rather than the resolver's zero stub.
 */
#ifndef WC3_HLE_KERNEL32_SYSTEM_H
#define WC3_HLE_KERNEL32_SYSTEM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of rows installed, or -1 if a row is missing. */
int wc3_hle_kernel32_system_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_KERNEL32_SYSTEM_H */
