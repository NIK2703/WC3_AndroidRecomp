/*
 * guest_host.h - translation between the 32-bit guest address space and the
 * host address where that memory actually lives.
 *
 * The lifted corpus keeps guest pointers as uint32_t and uses them through
 * cpu.h.  Historically the guest VA was also the host VA (identity mapping).
 * A guest VA window changes only the host side:
 *
 *     host = g_guest_host_base + guest_va
 *
 * g_guest_host_base == 0 keeps the old identity mode.  Window mode sets it to
 * a 4 GiB-aligned host base before the first guest mapping.  Keeping the base
 * 4 GiB-aligned means (uint32_t)host_ptr == guest_va for every pointer inside
 * the window, which keeps the existing 32-bit pointer contract intact.
 */
#ifndef WC3_GUEST_HOST_H
#define WC3_GUEST_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uintptr_t g_guest_host_base;

static inline void *guest_host_ptr(uint32_t va)
{
    return (void *)(g_guest_host_base + (uintptr_t)va);
}

static inline int guest_host_contains(const void *p)
{
    uintptr_t v = (uintptr_t)p;
    return v >= g_guest_host_base &&
           v - g_guest_host_base < 0x100000000ull;
}

/* Guest VA for a pointer inside the window.  UINT32_MAX is a sentinel; callers
 * that need the boundary case must test guest_host_contains() first. */
static inline uint32_t guest_host_va(const void *p)
{
    return guest_host_contains(p)
        ? (uint32_t)((uintptr_t)p - g_guest_host_base)
        : 0xFFFFFFFFu;
}

#ifdef __cplusplus
}
#endif

#endif /* WC3_GUEST_HOST_H */
