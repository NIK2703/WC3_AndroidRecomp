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
#define GUEST_PAGE_SHIFT 12u
#define GUEST_PAGE_SIZE (1u << GUEST_PAGE_SHIFT)
#define GUEST_PAGE_COUNT (1u << (32u - GUEST_PAGE_SHIFT))
#define GUEST_PAGE_OVERLAP 0xFFu

static const Wc3Platform *g_platform;

/* Ranges the guest may touch.  This is the same boundary as the old
 * hle_android.c table, expressed from the canonical layout macros. */
static const struct {
    uint32_t lo;
    uint32_t hi;
} g_arenas[] = {
    /* Kept byte-for-byte equivalent to the as-built table in
     * runtime/legacy/hle_android.c: the WAR3 upper bound is absolute, not
     * GUEST_IMAGE_WAR3 + 0x01000000.  O-07 must not widen the accepted set. */
    {GUEST_IMAGE_WAR3, 0x01000000u},
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
    {GUEST_HLE_BASE, GUEST_HLE_BASE + 0x00100000u},
};

/* One byte per 4 KiB guest page.  A value in 1..arena_count identifies the
 * single arena that owns the page; zero means "not a static arena"; 0xFF means
 * two static arenas overlap on this page (GUEST_HLE_BASE sits inside VA4), so
 * the range must take the exact linear path.  The map is written once before
 * guest startup and read-only afterwards. */
static uint8_t g_page_arena[GUEST_PAGE_COUNT];
static int g_page_map_ready;
_Static_assert(sizeof g_arenas / sizeof g_arenas[0] < GUEST_PAGE_OVERLAP,
               "arena id must fit the page map");

/* O-07: one byte per 4 KiB page for the dynamic range table.  Zero means
 * "no fully covered dynamic page"; 1..254 is a range slot plus one; 0xFF means
 * two ranges overlap on this page or the slot index does not fit, so the exact
 * linear scan below remains the fallback.  Only pages wholly inside a range are
 * marked: a partially covered page must not become a positive hint.  The table
 * is a hint, not the authority; the selected range is still bounds-checked
 * against the complete [base, hi) request. */
#define GUEST_DYN_PAGE_MAX_ID 0xFEu
#define GUEST_DYN_PAGE_OVERFLOW 0xFFu
static _Atomic uint8_t g_dyn_page_id[GUEST_PAGE_COUNT];

static void dyn_page_mark(uint32_t base, uint32_t size, uint32_t id)
{
    if (size == 0u || id == 0u || id > GUEST_DYN_PAGE_MAX_ID) return;
    uint64_t first =
        ((uint64_t)base + GUEST_PAGE_SIZE - 1u) >> GUEST_PAGE_SHIFT;
    uint64_t last = ((uint64_t)base + size) >> GUEST_PAGE_SHIFT;
    for (uint64_t p = first; p < last; p++) {
        uint8_t cur = atomic_load_explicit(&g_dyn_page_id[p],
                                           memory_order_acquire);
        for (;;) {
            if (cur == (uint8_t)id) break;
            uint8_t want = cur == 0u ? (uint8_t)id : GUEST_DYN_PAGE_OVERFLOW;
            if (atomic_compare_exchange_weak_explicit(
                    &g_dyn_page_id[p], &cur, want, memory_order_acq_rel,
                    memory_order_acquire))
                break;
        }
    }
}

static void dyn_page_unmark(uint32_t base, uint32_t size, uint32_t id)
{
    if (size == 0u || id == 0u || id > GUEST_DYN_PAGE_MAX_ID) return;
    uint64_t first =
        ((uint64_t)base + GUEST_PAGE_SIZE - 1u) >> GUEST_PAGE_SHIFT;
    uint64_t last = ((uint64_t)base + size) >> GUEST_PAGE_SHIFT;
    for (uint64_t p = first; p < last; p++) {
        uint8_t cur = atomic_load_explicit(&g_dyn_page_id[p],
                                           memory_order_acquire);
        while (cur == (uint8_t)id) {
            if (atomic_compare_exchange_weak_explicit(
                    &g_dyn_page_id[p], &cur, 0u, memory_order_acq_rel,
                    memory_order_acquire))
                break;
        }
    }
}

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

/* Every visible change to g_ranges bumps this epoch with release.  A reader
 * that validates at an epoch can reuse its per-thread last range only while
 * the epoch is unchanged; any coalesce, append, or removal invalidates it. */
static _Atomic uint64_t g_range_epoch;

/* Last dynamic range accepted by this thread.  Zero is never a valid range
 * encoding (base and size are both non-zero), so zero means "empty". */
static _Thread_local uint64_t g_cached_range;
static _Thread_local uint64_t g_cached_epoch;

static void range_epoch_bump(void)
{
    atomic_fetch_add_explicit(&g_range_epoch, 1u, memory_order_release);
}

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
            dyn_page_mark(merged_lo, (uint32_t)(merged_hi - merged_lo), i + 1u);
            range_epoch_bump();
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
    dyn_page_mark(base, size, slot + 1u);
    range_epoch_bump();
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
                dyn_page_unmark(base, (uint32_t)(end - base), i + 1u);
                /* The right remainder is appended under a new slot; clear the
                 * old hint so the add can publish the new slot id cleanly. */
                dyn_page_unmark((uint32_t)end, (uint32_t)(hi - end), i + 1u);
                range_epoch_bump();
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
                dyn_page_unmark(base, (uint32_t)(end - base), i + 1u);
                range_epoch_bump();
                removed = 1;
                break;
            }
        }
    }
    return removed;
}

static void static_page_map_init(void)
{
    memset(g_page_arena, 0, sizeof g_page_arena);
    for (size_t i = 0; i < sizeof g_arenas / sizeof g_arenas[0]; i++) {
        uint32_t lo = g_arenas[i].lo;
        uint32_t hi = g_arenas[i].hi;
        if (lo >= hi) continue;
        uint32_t first = lo >> GUEST_PAGE_SHIFT;
        uint32_t last = (hi - 1u) >> GUEST_PAGE_SHIFT;
        uint8_t id = (uint8_t)(i + 1u);
        for (uint32_t p = first; p <= last; p++) {
            if (g_page_arena[p] == 0u)
                g_page_arena[p] = id;
            else if (g_page_arena[p] != id)
                g_page_arena[p] = GUEST_PAGE_OVERLAP;
        }
    }
    g_page_map_ready = 1;
}

/* O(1) replacement for the static-arena scan.  The page id names the arena
 * that owns the page; the same arena's bounds then decide the whole range.
 * A page shared by two arenas returns false and the caller falls back to the
 * exact linear form, so this cannot widen the accepted set. */
static int static_page_ok(uint8_t id, uint32_t base, uint32_t size)
{
    uint64_t hi = (uint64_t)base + size;
    if (hi > 0x100000000ull) return 0;
    if (id == 0u || id == GUEST_PAGE_OVERLAP) return 0;
    size_t i = (size_t)id - 1u;
    if (i >= sizeof g_arenas / sizeof g_arenas[0]) return 0;
    return base >= g_arenas[i].lo && hi <= g_arenas[i].hi;
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

    const uint64_t epoch =
        atomic_load_explicit(&g_range_epoch, memory_order_acquire);
    const uint64_t cached = g_cached_range;
    if (cached != 0u && g_cached_epoch == epoch) {
        uint32_t lo, len;
        range_decode(cached, &lo, &len);
        if (base >= lo && hi <= (uint64_t)lo + len) return 1;
    }

    uint32_t n = range_count();

    /* O-07: the page hint names one range that fully covers the page.  It is
     * only a candidate: the whole request is still checked against that
     * range's exact bounds, so a stale hint can cause a fallback but never a
     * wider accepted set.  A page shared by two ranges is 0xFF and falls back
     * to the exact scan. */
    const uint8_t dyn =
        atomic_load_explicit(&g_dyn_page_id[base >> GUEST_PAGE_SHIFT],
                             memory_order_acquire);
    if (dyn != 0u && dyn != GUEST_DYN_PAGE_OVERFLOW) {
        uint32_t slot = (uint32_t)dyn - 1u;
        if (slot < n) {
            uint64_t value = atomic_load_explicit(&g_ranges[slot],
                                                  memory_order_acquire);
            if (value != 0u) {
                uint32_t lo, len;
                range_decode(value, &lo, &len);
                if (base >= lo && hi <= (uint64_t)lo + len) {
                    g_cached_range = value;
                    g_cached_epoch = epoch;
                    return 1;
                }
            }
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        uint64_t value = atomic_load_explicit(&g_ranges[i],
                                              memory_order_acquire);
        if (value == 0u) continue;
        uint32_t lo, len;
        range_decode(value, &lo, &len);
        if (base >= lo && hi <= (uint64_t)lo + len) {
            g_cached_range = value;
            g_cached_epoch = epoch;
            return 1;
        }
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
    /* Empty ranges keep the historical static_range_ok contract, which treats
     * addr == arena.hi as inside; the page map marks only pages touched by
     * [lo, hi), so it cannot express that boundary. */
    if (size == 0u)
        return static_range_ok(addr, 0u) || wc3_guest_range_ok(addr, 0u);

    /* The legacy boot path owns the mappings and never calls
     * wc3_guest_memory_init(), but the static arena table is immutable.  Build
     * its page map on first use so the shim can take the O(1) path without a
     * second owner for [0, 4 GiB). */
    if (!g_page_map_ready) static_page_map_init();
    const uint8_t id = g_page_arena[addr >> GUEST_PAGE_SHIFT];

    /* id == 0 means no static arena overlaps this page, so no arena can
     * contain a range starting here: skip the 20-arena scan.  A single
     * non-overlap id whose bounds do not contain the range cannot be rescued
     * by another arena either; only an overlapping page needs the exact
     * linear fallback. */
    if (id == 0u)
        return wc3_guest_range_ok(addr, size);
    if (static_page_ok(id, addr, size)) return 1;
    if (id == GUEST_PAGE_OVERLAP)
        return static_range_ok(addr, size) ||
               wc3_guest_range_ok(addr, size);
    return wc3_guest_range_ok(addr, size);
}

uint32_t wc3_guest_clamp(uint32_t addr, uint32_t size)
{
    /* The page map is built once and read-only afterwards, same as the
     * wc3_guest_ok path.  A single non-overlap id means exactly one static
     * arena owns this page, so it is also the first containing arena the
     * linear scan below would select.  Zero, overlap, an invalid id, or an
     * address outside the named arena falls through to that exact scan. */
    if (!g_page_map_ready) static_page_map_init();
    uint8_t id = g_page_arena[addr >> GUEST_PAGE_SHIFT];
    if (id != 0u && id != GUEST_PAGE_OVERLAP) {
        size_t i = (size_t)id - 1u;
        if (i < sizeof g_arenas / sizeof g_arenas[0] &&
            addr >= g_arenas[i].lo && addr < g_arenas[i].hi) {
            uint32_t avail = g_arenas[i].hi - addr;
            return size < avail ? size : avail;
        }
    }

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

void wc3_guest_static_init(void)
{
    if (!g_page_map_ready) static_page_map_init();
}

int wc3_guest_memory_init(const Wc3Platform *platform)
{
    if (!platform || !platform->mem.reserve || !platform->mem.commit ||
        !platform->mem.release)
        return -1;
    if (g_platform) return -1;
    g_platform = platform;
    wc3_guest_static_init();

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

/* E6.3: volatile orders the access but is not an aliasing contract.  The
 * accessors are used on guest memory that the compiler otherwise sees through
 * unrelated types, so each width gets a may_alias type.  The qualifier is on
 * the pointee, next to volatile: the load/store stays ordered and cannot be
 * merged or elided, while the compiler no longer assumes it cannot alias the
 * surrounding state. */
#if defined(__GNUC__) || defined(__clang__)
#define WC3_MAY_ALIAS __attribute__((may_alias))
#else
#define WC3_MAY_ALIAS
#endif

typedef uint8_t wc3_guest_u8 WC3_MAY_ALIAS;
typedef uint16_t wc3_guest_u16 WC3_MAY_ALIAS;
typedef uint32_t wc3_guest_u32 WC3_MAY_ALIAS;

uint8_t wc3_rd8g(uint32_t addr)
{
    return *(volatile wc3_guest_u8 *)(uintptr_t)addr;
}

uint16_t wc3_rd16g(uint32_t addr)
{
    return *(volatile wc3_guest_u16 *)(uintptr_t)addr;
}

uint32_t wc3_rd32g(uint32_t addr)
{
    return *(volatile wc3_guest_u32 *)(uintptr_t)addr;
}

void wc3_wr8g(uint32_t addr, uint8_t value)
{
    *(volatile wc3_guest_u8 *)(uintptr_t)addr = value;
}

void wc3_wr16g(uint32_t addr, uint16_t value)
{
    *(volatile wc3_guest_u16 *)(uintptr_t)addr = value;
}

void wc3_wr32g(uint32_t addr, uint32_t value)
{
    *(volatile wc3_guest_u32 *)(uintptr_t)addr = value;
}

uint32_t wc3_arg32(const CPU *cpu, unsigned n)
{
    return wc3_rd32g(cpu->esp + 4u + n * 4u);
}
