/*
 * guest_window.h - host side of the 32-bit guest address space.
 *
 * The guest keeps its fixed 32-bit virtual addresses.  Android reserves one
 * 4 GiB host window and maps guest page [gva] at [window_base + gva].  This
 * removes every collision between a guest layout address (for example the
 * main stack at 0x31000000) and an ART/linker mapping in the app process.
 *
 * This module owns only the window and the mapping registry.  The guest range
 * validators stay in runtime/guest/guest_memory.c and the as-built HLE.
 */
#ifndef WC3_GUEST_WINDOW_H
#define WC3_GUEST_WINDOW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotent.  Returns 0 when the window is active, or when identity mode was
 * explicitly requested with WC3_GUEST_WINDOW=0.  Returns -1 when window mode
 * was requested but no 4 GiB aligned host range could be reserved. */
int wc3_guest_window_init(void);
int wc3_guest_window_active(void);
int wc3_guest_window_aligned(void);
uintptr_t wc3_guest_window_base(void);
size_t wc3_guest_window_host_page_size(void);
void *wc3_guest_window_host_ptr(uint32_t gva);
int wc3_guest_window_contains(uint32_t gva, uint32_t size);

/* Reserve a guest VA range in the low window [WC3_LOWMEM_BEGIN,
 * WC3_LOWMEM_END) for a Vulkan placed mapping.  The range is marked in the
 * window registry and its host pages are unmapped, so the driver can map
 * there with VK_EXT_map_memory_placed.  Returns 0 and stores the guest VA on
 * success, -1 when the window is inactive or has no aligned gap.  This is the
 * window-mode counterpart of the shim's low host probe. */
int wc3_guest_window_place(uint64_t size, uint64_t alignment, uint32_t *gva);

/* Reserve/commit guest ranges inside the window.  In identity mode these
 * return -1; callers use the historical mmap path there. */
int wc3_guest_window_reserve(uint32_t gva, uint32_t size);
int wc3_guest_window_commit(uint32_t gva, uint32_t size);
int wc3_guest_window_map(uint32_t gva, uint32_t size);
int wc3_guest_window_decommit(uint32_t gva, uint32_t size);
int wc3_guest_window_release(uint32_t gva, uint32_t size);
int wc3_guest_window_protect(uint32_t gva, uint32_t size, int host_prot);

/* Always-on boot/crash diagnostic, not gated by the subsystem debug tags. */
void wc3_guest_window_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_GUEST_WINDOW_H */
