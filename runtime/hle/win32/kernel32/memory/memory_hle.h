/*
 * memory_hle.h - kernel32 heap/global memory imports.
 *
 * First bounded part of the H6.2 kernel32/memory module: the Windows heap and
 * the Global/Local families already have an allocator in the as-built heap.
 * VirtualAlloc/VirtualFree stay in the quarantine for now because their live
 * range table is private to the as-built boot; replacing them without moving
 * that ownership would break guest_ok().
 */
#ifndef WC3_HLE_WIN32_KERNEL32_MEMORY_H
#define WC3_HLE_WIN32_KERNEL32_MEMORY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Install over the as-built registry.  Must run after hle_android_start() and
 * before the first guest thread.  Returns the number of rows installed, or -1
 * when a mandatory row is missing. */
int wc3_hle_kernel32_memory_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_WIN32_KERNEL32_MEMORY_H */
