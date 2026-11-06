/*
 * guest_memory.c - fixed 32-bit guest mappings and range validation.
 *
 * This is the Android replacement for runtime/host/guest_mem.c.  It uses the
 * platform memory table, not POSIX directly, and it lives in runtime/guest, so
 * it deliberately has no mutex: the range table is an append-only atomic log.
 */
#define _GNU_SOURCE 1

#include "guest_memory.h"
#include "guest_layout.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef WC3_DEBUG
#define WC3_DEBUG 0
#endif

#define GUEST_RANGE_MAX 512u

static const Wc3Platform *g_platform;

/* Ranges the guest may touch.  This is the same boundary as the old
 * hle_android.c table, expressed from the canonical layout macros. */
static const struct {
    uint32_t lo;
    uint32_t hi;
} g_arenas[] = {
    {GUEST_IMAGE_WAR3, GUEST_IMAGE_WAR3 + 0x01000000u},
    {GUEST_HEAP_BASE, GUEST_HEAP_BASE + GUEST_HEAP_SIZE},
    {GUEST_HLE_HEAP_BASE, GUEST_HLE_HEAP_BASE + GUEST_HLE_HEAP_SIZE},
    {GUEST_VA_BASE, GUEST_VA_BASE + GUEST_VA_SIZE},
    {GUEST_IMAGE_STORM, GUEST_IMAGE_STORM + 0x00100000u},
    {GUEST_IMAGE_MSS, GUEST_IMAGE_MSS + 0x00100000u},
    {GUEST_IMAGE_MSSFAST, GUEST_IMAGE_MSSFAST + 0x00020000u},
    {GUEST_IMAGE_MP3DEC, GUEST_IMAGE_MP3DEC + 0x00040000u},
    {GUEST_STACK_BASE, GUEST_STACK_BASE + GUEST_STACK_SIZE},
    {GUEST_THREAD_STACK_BASE,
     GUEST_THREAD_STACK_BASE + GUEST_THREAD_MAX * GUEST_THREAD_STACK_SIZE},
    {GUEST_VA2_BASE, GUEST_VA2_BASE + GUEST_VA2_SIZE},
    {GUEST_VA3_BASE, GUEST_VA3_BASE + GUEST_VA3_SIZE},
    {GUEST_VA4_BASE, GUEST_VA4_BASE + GUEST_VA4_SIZE},
    {GUEST_VA5_BASE, GUEST_VA5_BASE + GUEST_VA5_SIZE},
    {GUEST_VA6_BASE, GUEST_VA6_BASE + GUEST_VA6_SIZE},
    {GUEST_VA7_BASE, GUEST_VA7_BASE + GUEST_VA7_SIZE},
    {GUEST_IMAGE_IJL, GUEST_IMAGE_IJL + 0x00100000u},
    {GUEST_IMAGE_GAME, GUEST_IMAGE_GAME + 0x01000000u},
    {GUEST_TEB_BASE, GUEST_TEB_BASE + 0x00010000u},
    {GUEST_THREAD_TEB_BASE, GUEST_THREAD_TEB_BASE + GUEST_THREAD_MAX * 0x1000u},
    {GUEST_HLE_BASE, GUEST_HLE_BASE + 0x00100000u},
};

/* The core mappings are made once, before guest startup. */
static const struct {
    uint32_t base;
    uint32_t size;
} g_core[] = {
    {GUEST_STACK_BASE, GUEST_STACK_SIZE},
    {GUEST_HEAP_BASE, GUEST_HEAP_SIZE},
    {GUEST_HLE_HEAP_BASE, GUEST_HLE_HEAP_SIZE},
    {GUEST_TEB_BASE, 0x00001000u},
    {GUEST_THREAD_STACK_BASE, GUEST_THREAD_MAX * GUEST_THREAD_STACK_SIZE},
    {GUEST_THREAD_TEB_BASE, GUEST_THREAD_MAX * 0x00001000u},
};

/* value = lo | ((uint64_t)size << 32); zero is an empty slot. */
static _Atomic uint64_t g_ranges[GUEST_RANGE_MAX];
static _Atomic uint32_t g_range_count;
static _Atomic uint32_t g_range_overflow;

static uint64_t range_encode(uint32_t base, uint32_t size)
{
    return (uint64_t)base | ((uint64_t)size << 32);
}

static void range_decode(uint64_t value, uint32_t *base, uint32_t *size)
{
    *base = (uint32_t)value;
    *size = (uint32_t)(value >> 32);
}

static uint32_t range_count(void)
{
    uint32_t n = atomic_load_explicit(&g_range_count, memory_order_acquire);
    return n > GUEST_RANGE_MAX ? GUEST_RANGE_MAX : n;
}

void wc3_guest_range_add(uint32_t base, uint32_t size)
{
    if (base == 0u || size == 0u) return;
    if ((uint64_t)base + size > 0x100000000ull) return;

    /* Coalesce with an existing overlapping/adjacent range when possible.  A
     * stale reader still sees the old, narrower range, which is safe. */
    uint32_t n = range_count();
    for (uint32_t i = 0; i < n; i++) {
        uint64_t old = atomic_load_explicit(&g_ranges[i], memory_order_acquire);
        if (old == 0u) continue;
        uint32_t lo, len;
        range_decode(old, &lo, &len);
        uint64_t hi = (uint64_t)lo + len;
        uint64_t new_hi = (uint64_t)base + size;
        if (new_hi < lo || base > hi) continue;
        uint32_t merged_lo = base < lo ? base : lo;
        uint64_t merged_hi = new_hi > hi ? new_hi : hi;
        uint64_t merged =
            range_encode(merged_lo, (uint32_t)(merged_hi - merged_lo));
        if (atomic_compare_exchange_strong_explicit(
                &g_ranges[i], &old, merged, memory_order_acq_rel,
                memory_order_acquire)) {
            return;
        }
        /* Lost the race: retry the scan with the new value. */
        i--;
    }

    uint32_t slot = atomic_fetch_add_explicit(&g_range_count, 1u,
                                              memory_order_acq_rel);
    if (slot >= GUEST_RANGE_MAX) {
        atomic_fetch_add_explicit(&g_range_overflow, 1u, memory_order_relaxed);
        return;
    }
    atomic_store_explicit(&g_ranges[slot], range_encode(base, size),
                          memory_order_release);
}

static int range_remove(uint32_t base, uint32_t size)
{
    if (base == 0u || size == 0u) return 0;
    uint64_t end = (uint64_t)base + size;
    if (end > 0x100000000ull) return 0;

    uint32_t n = range_count();
    int removed = 0;
    for (uint32_t i = 0; i < n; i++) {
        for (;;) {
            uint64_t old =
                atomic_load_explicit(&g_ranges[i], memory_order_acquire);
            if (old == 0u) break;

            uint32_t lo, len;
            range_decode(old, &lo, &len);
            uint64_t hi = (uint64_t)lo + len;
            if (end <= lo || base >= hi) break;

            int has_left = lo < base;
            int has_right = end < hi;
            uint64_t replacement = 0u;
            if (has_left && has_right) {
                /* Keep the left remainder in this slot and append the right
                 * remainder.  The removed gap keeps them from coalescing. */
                replacement = range_encode(lo, (uint32_t)(base - lo));
                if (!atomic_compare_exchange_strong_explicit(
                        &g_ranges[i], &old, replacement, memory_order_acq_rel,
                        memory_order_acquire))
                    continue;
                wc3_guest_range_add((uint32_t)end, (uint32_t)(hi - end));
                removed = 1;
                break;
            }
            if (has_left)
                replacement = range_encode(lo, (uint32_t)(base - lo));
            else if (has_right)
                replacement =
                    range_encode((uint32_t)end, (uint32_t)(hi - end));

            if (atomic_compare_exchange_strong_explicit(
                    &g_ranges[i], &old, replacement, memory_order_acq_rel,
                    memory_order_acquire)) {
                removed = 1;
                break;
            }
        }
    }
    return removed;
}

static int static_range_ok(uint32_t base, uint32_t size)
{
    uint64_t hi = (uint64_t)base + size;
    if (hi > 0x100000000ull) return 0;
    for (size_t i = 0; i < sizeof g_arenas / sizeof g_arenas[0]; i++) {
        if (base >= g_arenas[i].lo && hi <= g_arenas[i].hi) return 1;
    }
    return 0;
}

int wc3_guest_range_ok(uint32_t base, uint32_t size)
{
    uint64_t hi = (uint64_t)base + size;
    if (hi > 0x100000000ull) return 0;
    uint32_t n = range_count();
    for (uint32_t i = 0; i < n; i++) {
        uint64_t value = atomic_load_explicit(&g_ranges[i],
                                              memory_order_acquire);
        if (value == 0u) continue;
        uint32_t lo, len;
        range_decode(value, &lo, &len);
        if (base >= lo && hi <= (uint64_t)lo + len) return 1;
    }
    return 0;
}

uint32_t wc3_guest_range_clamp(uint32_t base, uint32_t size)
{
    uint32_t n = range_count();
    for (uint32_t i = 0; i < n; i++) {
        uint64_t value = atomic_load_explicit(&g_ranges[i],
                                              memory_order_acquire);
        if (value == 0u) continue;
        uint32_t lo, len;
        range_decode(value, &lo, &len);
        uint64_t hi = (uint64_t)lo + len;
        if (base < lo || (uint64_t)base >= hi) continue;
        uint64_t avail = hi - base;
        return size < avail ? size : (uint32_t)avail;
    }
    return 0;
}

int wc3_guest_ok(uint32_t addr, uint32_t size)
{
    return static_range_ok(addr, size) || wc3_guest_range_ok(addr, size);
}

uint32_t wc3_guest_clamp(uint32_t addr, uint32_t size)
{
    for (size_t i = 0; i < sizeof g_arenas / sizeof g_arenas[0]; i++) {
        if (addr < g_arenas[i].lo || addr >= g_arenas[i].hi) continue;
        uint32_t avail = g_arenas[i].hi - addr;
        return size < avail ? size : avail;
    }
    return wc3_guest_range_clamp(addr, size);
}

int wc3_guest_reserve(uint32_t base, uint32_t size)
{
    if (!g_platform || !g_platform->mem.reserve) return -1;
    if (base == 0u || size == 0u) return -1;
    return g_platform->mem.reserve(g_platform->mem.ctx, base, size);
}

int wc3_guest_commit(uint32_t base, uint32_t size)
{
    if (!g_platform || !g_platform->mem.commit) return -1;
    if (base == 0u || size == 0u) return -1;
    if (g_platform->mem.commit(g_platform->mem.ctx, base, size) != 0) return -1;
    wc3_guest_range_add(base, size);
    return 0;
}

int wc3_guest_map(uint32_t base, uint32_t size)
{
    if (wc3_guest_reserve(base, size) != 0) return -1;
    if (wc3_guest_commit(base, size) != 0) {
        if (g_platform->mem.release)
            g_platform->mem.release(g_platform->mem.ctx, base, size);
        return -1;
    }
    return 0;
}

int wc3_guest_decommit(uint32_t base, uint32_t size)
{
    if (!g_platform || !g_platform->mem.decommit) return -1;
    if (base == 0u || size == 0u) return -1;
    int rc = g_platform->mem.decommit(g_platform->mem.ctx, base, size);
    if (rc == 0) (void)range_remove(base, size);
    return rc;
}

int wc3_guest_protect(uint32_t base, uint32_t size, uint32_t prot)
{
    if (!g_platform || !g_platform->mem.protect) return -1;
    return g_platform->mem.protect(g_platform->mem.ctx, base, size, prot);
}

int wc3_guest_unmap(uint32_t base, uint32_t size)
{
    if (!g_platform || !g_platform->mem.release) return -1;
    if (base == 0u || size == 0u) return -1;
    int rc = g_platform->mem.release(g_platform->mem.ctx, base, size);
    if (rc == 0) (void)range_remove(base, size);
    return rc;
}

int wc3_guest_memory_init(const Wc3Platform *platform)
{
    if (!platform || !platform->mem.reserve || !platform->mem.commit ||
        !platform->mem.release)
        return -1;
    if (g_platform) return -1;
    g_platform = platform;

    for (size_t i = 0; i < sizeof g_core / sizeof g_core[0]; i++) {
        if (wc3_guest_map(g_core[i].base, g_core[i].size) != 0) {
            /* Roll back everything mapped by this init. */
            for (size_t k = 0; k < i; k++)
                (void)wc3_guest_unmap(g_core[k].base, g_core[k].size);
            g_platform = NULL;
            return -1;
        }
    }
    return 0;
}

uint8_t wc3_rd8g(uint32_t addr)
{
    return *(volatile uint8_t *)(uintptr_t)addr;
}

uint16_t wc3_rd16g(uint32_t addr)
{
    return *(volatile uint16_t *)(uintptr_t)addr;
}

uint32_t wc3_rd32g(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

void wc3_wr8g(uint32_t addr, uint8_t value)
{
    *(volatile uint8_t *)(uintptr_t)addr = value;
}

void wc3_wr16g(uint32_t addr, uint16_t value)
{
    *(volatile uint16_t *)(uintptr_t)addr = value;
}

void wc3_wr32g(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)addr = value;
}

uint32_t wc3_arg32(const CPU *cpu, unsigned n)
{
    return wc3_rd32g(cpu->esp + 4u + n * 4u);
}
