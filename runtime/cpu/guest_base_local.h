/*
 * guest_base_local.h - base-aware guest-memory access for the local-base pass.
 *
 * g_guest_host_base is a write-once boot value: guest_window_android.c sets it
 * before the first guest mapping, and nothing changes it afterwards.  The
 * compiler cannot infer that from an ordinary mutable global, so every rd/wr
 * helper reloads the base after guest stores and calls.  tools/lift/guest_base
 * snapshots the value once at function entry and redirects the guest-memory
 * helpers to these explicit-base forms.
 *
 * This is an optimization detail, not a second addressing model.  The base
 * still comes from g_guest_host_base and the pointer arithmetic is identical:
 *
 *     host = base + (uintptr_t)guest_va
 *
 * Including this header is harmless; the pass only inserts it when it rewrites
 * at least one function in the translation unit.
 */
#ifndef WC3_GUEST_BASE_LOCAL_H
#define WC3_GUEST_BASE_LOCAL_H

#include <string.h>

#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* g_guest_host_base is written once during boot and only read afterwards.
 * The const attribute states that promise to the compiler: two calls in the
 * same function return the same value even across guest stores and calls.
 * This lets the optimizer keep the base in a register where it is profitable
 * without forcing a function-wide local snapshot.  It is not a second
 * addressing model: the value still comes from g_guest_host_base. */
#if defined(__clang__) || defined(__GNUC__)
__attribute__((const))
#endif
static inline uintptr_t wc3_guest_base(void)
{
    return g_guest_host_base;
}

static inline void *guest_host_ptr_at(uint32_t va, uintptr_t base)
{
    return (void *)(base + (uintptr_t)va);
}

static inline uint8_t rd8_at(uint32_t a, uintptr_t base)
{
    return *(uint8_t *)(uintptr_t)guest_host_ptr_at(a, base);
}

static inline uint16_t rd16_at(uint32_t a, uintptr_t base)
{
    return *(uint16_t *)(uintptr_t)guest_host_ptr_at(a, base);
}

static inline uint32_t rd32_at(uint32_t a, uintptr_t base)
{
    return *(uint32_t *)(uintptr_t)guest_host_ptr_at(a, base);
}

static inline uint64_t rd64_at(uint32_t a, uintptr_t base)
{
    uint64_t v;
    memcpy(&v, guest_host_ptr_at(a, base), 8);
    return v;
}

/* Same hook as the unscaled wr8/wr32 helpers: watch writes is a debug-only
 * contract and the address passed to it is the guest VA, not the host VA. */
static inline void wr8_at(uint32_t a, uint8_t v, uintptr_t base)
{
    WC3_WR32_HOOK(a, v);
    *(uint8_t *)(uintptr_t)guest_host_ptr_at(a, base) = v;
}

static inline void wr16_at(uint32_t a, uint16_t v, uintptr_t base)
{
    *(uint16_t *)(uintptr_t)guest_host_ptr_at(a, base) = v;
}

static inline void wr32_at(uint32_t a, uint32_t v, uintptr_t base)
{
    WC3_WR32_HOOK(a, v);
    *(uint32_t *)(uintptr_t)guest_host_ptr_at(a, base) = v;
}

static inline void wr64_at(uint32_t a, uint64_t v, uintptr_t base)
{
    memcpy(guest_host_ptr_at(a, base), &v, 8);
}

/* Stack helpers keep the guest-visible esp contract; only the host address of
 * the stack cell is computed from the snapshot. */
static inline void push32_at(CPU *c, uint32_t v, uintptr_t base)
{
    c->esp -= 4;
    wr32_at(STACK_BASE(c) + c->esp, v, base);
}

static inline uint32_t pop32_at(CPU *c, uintptr_t base)
{
    uint32_t v = rd32_at(STACK_BASE(c) + c->esp, base);
    c->esp += 4;
    return v;
}

static inline float rdss_at(uint32_t a, uintptr_t base)
{
    float f;
    memcpy(&f, guest_host_ptr_at(a, base), 4);
    return f;
}

static inline double rdsd_at(uint32_t a, uintptr_t base)
{
    double d;
    memcpy(&d, guest_host_ptr_at(a, base), 8);
    return d;
}

static inline XMM rdxm_at(uint32_t a, uintptr_t base)
{
    XMM x;
    memcpy(&x, guest_host_ptr_at(a, base), 16);
    return x;
}

static inline void wrss_at(uint32_t a, float v, uintptr_t base)
{
    memcpy(guest_host_ptr_at(a, base), &v, 4);
}

static inline void wrsd_at(uint32_t a, double v, uintptr_t base)
{
    memcpy(guest_host_ptr_at(a, base), &v, 8);
}

static inline void wrxm_at(uint32_t a, XMM v, uintptr_t base)
{
    memcpy(guest_host_ptr_at(a, base), &v, 16);
}

#ifdef __cplusplus
}
#endif

#endif /* WC3_GUEST_BASE_LOCAL_H */
