/*
 * guest_memory.h - fixed 32-bit guest address space for the Android target.
 *
 * The lifted corpus keeps guest virtual addresses in uint32_t and dereferences
 * them as host pointers.  This module owns the mappings that make that legal:
 * the fixed arenas from guest_layout.h, the per-image ranges, and the dynamic
 * ranges a guest VirtualAlloc commits later.
 *
 * The range table is append-only and lock-free.  runtime/guest must not take a
 * host lock (AGENTS.md), and the common path is bootstrap or one guest thread
 * reserving an arena slot.  Readers use acquire/release on the entry count.
 */
#ifndef WC3_GUEST_MEMORY_H
#define WC3_GUEST_MEMORY_H

#include <stdint.h>

#include "cpu.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the immutable static-arena page map without owning any mapping.  The
 * legacy boot path calls this before guest threads start; the new path gets
 * it from wc3_guest_memory_init(). */
void wc3_guest_static_init(void);

/* Map the core arenas required before any guest code runs.  Returns 0 on
 * success, -1 on a missing platform or an address conflict. */
int wc3_guest_memory_init(const Wc3Platform *platform);

/* Reserve [base, base+size) without making it accessible.  The caller commits
 * it later with wc3_guest_commit(). */
int wc3_guest_reserve(uint32_t base, uint32_t size);

/* Reserve and commit in one step.  Used for fixed arenas and image mappings. */
int wc3_guest_map(uint32_t base, uint32_t size);

int wc3_guest_commit(uint32_t base, uint32_t size);
int wc3_guest_decommit(uint32_t base, uint32_t size);
int wc3_guest_protect(uint32_t base, uint32_t size, uint32_t prot);
int wc3_guest_unmap(uint32_t base, uint32_t size);

/* Register a range that was mapped outside wc3_guest_map(), for example the
 * D3D9 staging arena.  The range is accepted by wc3_guest_ok() immediately. */
void wc3_guest_range_add(uint32_t base, uint32_t size);

/* All-or-nothing and clamp checks, matching the old HLE contract. */
int wc3_guest_ok(uint32_t addr, uint32_t size);
uint32_t wc3_guest_clamp(uint32_t addr, uint32_t size);

/* Exposed for tests and for callers that need the dynamic-range subset. */
int wc3_guest_range_ok(uint32_t base, uint32_t size);
uint32_t wc3_guest_range_clamp(uint32_t base, uint32_t size);

/* Guest accessors used by the HLE layer.  They are volatile loads/stores so a
 * bad address faults at the caller, not inside a library helper. */
uint8_t wc3_rd8g(uint32_t addr);
uint16_t wc3_rd16g(uint32_t addr);
uint32_t wc3_rd32g(uint32_t addr);
void wc3_wr8g(uint32_t addr, uint8_t value);
void wc3_wr16g(uint32_t addr, uint16_t value);
void wc3_wr32g(uint32_t addr, uint32_t value);

/* Argument n of a cdecl guest call: [esp+4+n*4]. */
uint32_t wc3_arg32(const CPU *cpu, unsigned n);

#ifdef __cplusplus
}
#endif

#endif /* WC3_GUEST_MEMORY_H */
