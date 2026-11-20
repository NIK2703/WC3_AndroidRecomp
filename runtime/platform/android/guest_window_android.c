/*
 * guest_window_android.c - reserve one high 4 GiB window for the 32-bit guest.
 *
 * The guest layout is unchanged.  What changes is only where those guest VAs
 * live in the 64-bit host process:
 *
 *     host = g_guest_host_base + guest_va
 *
 * The preferred base is 4 GiB aligned, so (uint32_t)host == guest_va for every
 * pointer inside the window.  That keeps the existing HLE contract where a
 * 32-bit guest pointer can be recovered by truncating a host pointer.
 */
#define _GNU_SOURCE 1

#include "guest_window.h"
#include "guest_host.h"
#include "wc3_debug.h"
#include "wc3_lowmem.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0
#endif
#ifndef MADV_DONTNEED
#define MADV_DONTNEED 4
#endif

#define GW_SIZE       0x100000000ull
#define GW_ALIGN      0x100000000ull
#define GW_MAX_RANGES 8192u

typedef struct {
    uint32_t lo;
    uint32_t hi;
    int prot;
} GwRange;

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_init_result = -1;
static int g_active;
static int g_aligned;
static size_t g_page = 4096;
static GwRange g_ranges[GW_MAX_RANGES];
static GwRange g_scratch[GW_MAX_RANGES];
static unsigned g_range_count;
static int g_coarse_page_warned;
/* Bump allocator for VK_EXT_map_memory_placed ranges.  Protected by g_lock;
 * the DXVK shim calls wc3_guest_window_place() during device memory init. */
static uintptr_t g_lowmem_next = WC3_LOWMEM_BEGIN;
#if WC3_DEBUG && WC3_DEBUG_MEMORY
static unsigned long long g_place_calls, g_place_ok, g_place_fail;
#endif

static int gw_contains(uint32_t base, uint32_t size)
{
    if (size == 0u || base == 0u) return 0;
    return (uint64_t)base + size <= GW_SIZE;
}

static int gw_overlap(uint32_t alo, uint32_t ahi, uint32_t blo, uint32_t bhi)
{
    return alo < bhi && blo < ahi;
}

static void gw_remove_locked(uint32_t lo, uint32_t hi)
{
    unsigned out_n = 0;
    for (unsigned i = 0; i < g_range_count; i++) {
        GwRange e = g_ranges[i];
        if (!gw_overlap(e.lo, e.hi, lo, hi)) {
            if (out_n < GW_MAX_RANGES) g_scratch[out_n++] = e;
            continue;
        }
        if (e.lo < lo && out_n < GW_MAX_RANGES) {
            GwRange left = { e.lo, lo, e.prot };
            g_scratch[out_n++] = left;
        }
        if (hi < e.hi && out_n < GW_MAX_RANGES) {
            GwRange right = { hi, e.hi, e.prot };
            g_scratch[out_n++] = right;
        }
    }
    memcpy(g_ranges, g_scratch, out_n * sizeof(g_ranges[0]));
    g_range_count = out_n;
}

/* Change the protection of every range inside [lo,hi).  The caller has
 * already established that the whole range is owned by existing entries. */
static int gw_set_prot_locked(uint32_t lo, uint32_t hi, int prot)
{
    unsigned out_n = 0;
    for (unsigned i = 0; i < g_range_count; i++) {
        GwRange e = g_ranges[i];
        if (!gw_overlap(e.lo, e.hi, lo, hi)) {
            if (out_n < GW_MAX_RANGES) g_scratch[out_n++] = e;
            continue;
        }
        uint32_t a = e.lo > lo ? e.lo : lo;
        uint32_t b = e.hi < hi ? e.hi : hi;
        if (e.lo < a) {
            GwRange left = { e.lo, a, e.prot };
            if (out_n < GW_MAX_RANGES) g_scratch[out_n++] = left;
        }
        if (a < b) {
            GwRange mid = { a, b, prot };
            if (out_n < GW_MAX_RANGES) g_scratch[out_n++] = mid;
        }
        if (b < e.hi) {
            GwRange right = { b, e.hi, e.prot };
            if (out_n < GW_MAX_RANGES) g_scratch[out_n++] = right;
        }
    }
    if (out_n > GW_MAX_RANGES) return -1;
    memcpy(g_ranges, g_scratch, out_n * sizeof(g_ranges[0]));
    g_range_count = out_n;
    return 0;
}

static int gw_covered_locked(uint32_t lo, uint32_t hi)
{
    /* A single reservation normally contains the whole subrange.  Accept
     * adjacent entries too, but never a hole. */
    uint32_t at = lo;
    while (at < hi) {
        int advanced = 0;
        for (unsigned i = 0; i < g_range_count; i++) {
            if (g_ranges[i].lo <= at && at < g_ranges[i].hi) {
                uint32_t end = g_ranges[i].hi < hi ? g_ranges[i].hi : hi;
                if (end > at) {
                    at = end;
                    advanced = 1;
                    break;
                }
            }
        }
        if (!advanced) return 0;
    }
    return 1;
}

static int gw_mprotect(uint32_t base, uint32_t size, int prot)
{
    if (!g_active) return -1;
    uint64_t start = (uint64_t)base & ~(uint64_t)(g_page - 1u);
    uint64_t end = ((uint64_t)base + size + g_page - 1u) &
                   ~(uint64_t)(g_page - 1u);
    if (end > GW_SIZE) end = GW_SIZE;
    if (end <= start) return -1;
    void *host = (void *)(g_guest_host_base + start);
    if (mprotect(host, (size_t)(end - start), prot) != 0) {
        fprintf(stderr, "[mem] guest window mprotect(%08X+%X, %d) -> %s\n",
                (uint32_t)start, (uint32_t)(end - start), prot,
                strerror(errno));
        return -1;
    }
    return 0;
}

static int gw_add_locked(uint32_t base, uint32_t size, int prot)
{
    if (!gw_contains(base, size)) return -1;
    uint32_t hi = base + size;
    for (unsigned i = 0; i < g_range_count; i++)
        if (gw_overlap(g_ranges[i].lo, g_ranges[i].hi, base, hi))
            return -1;
    if (g_range_count >= GW_MAX_RANGES) return -1;
    g_ranges[g_range_count].lo = base;
    g_ranges[g_range_count].hi = hi;
    g_ranges[g_range_count].prot = prot;
    g_range_count++;
    return 0;
}

static int gw_reserve_locked(uint32_t base, uint32_t size)
{
    return gw_add_locked(base, size, 0);
}

static int gw_map_locked(uint32_t base, uint32_t size, int prot)
{
    if (gw_add_locked(base, size, prot) != 0) return -1;
    if (gw_mprotect(base, size, prot) != 0) {
        gw_remove_locked(base, base + size);
        return -1;
    }
    return 0;
}

static void gw_try_once(void)
{
    const char *mode = getenv("WC3_GUEST_WINDOW");
    if (mode && (!strcmp(mode, "0") || !strcmp(mode, "identity"))) {
        g_active = 0;
        g_aligned = 1;
        g_init_result = 0;
        fprintf(stderr, "[mem] guest window: identity mode (WC3_GUEST_WINDOW=%s)\n",
                mode);
        return;
    }

    long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0) g_page = (size_t)ps;
    if (g_page > 4096u) {
        fprintf(stderr,
                "[mem] guest window: host page=%zu; guest 4 KiB protection "
                "will be host-page granular\n", g_page);
    }

    static const uintptr_t candidates[] = {
        0x0000000100000000ull, /* 4 GiB */
        0x0000000200000000ull, /* 8 GiB */
        0x0000000400000000ull, /* 16 GiB */
        0x0000000800000000ull, /* 32 GiB */
        0x0000001000000000ull, /* 64 GiB */
        0x0000002000000000ull, /* 128 GiB */
        0x0000004000000000ull, /* 256 GiB */
    };

    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        void *hint = (void *)candidates[i];
        void *p = mmap(hint, (size_t)GW_SIZE, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
                           MAP_FIXED_NOREPLACE,
                       -1, 0);
        if (p == MAP_FAILED) {
            if (errno != EEXIST && errno != EINVAL && errno != ENOMEM)
                break;
            continue;
        }
        if ((uintptr_t)p != candidates[i]) {
            munmap(p, (size_t)GW_SIZE);
            continue;
        }
        g_guest_host_base = (uintptr_t)p;
        g_active = 1;
        g_aligned = (g_guest_host_base & (GW_ALIGN - 1u)) == 0;
        g_init_result = 0;
#if WC3_DEBUG && WC3_DEBUG_MEMORY
        fprintf(stderr,
                "[mem] guest window base=%p size=0x%llX page=%zu aligned=%d\n",
                (void *)g_guest_host_base, (unsigned long long)GW_SIZE,
                g_page, g_aligned);
#endif
        return;
    }

    /* Some kernels do not implement MAP_FIXED_NOREPLACE.  Reserve twice the
     * window and trim to a 4 GiB aligned subrange. */
    void *raw = mmap(NULL, (size_t)(GW_SIZE * 2u), PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw != MAP_FAILED) {
        uintptr_t start = (uintptr_t)raw;
        uintptr_t aligned = (start + GW_ALIGN - 1u) & ~(GW_ALIGN - 1u);
        uintptr_t raw_end = start + GW_SIZE * 2u;
        if (aligned + GW_SIZE <= raw_end) {
            if (aligned > start) munmap((void *)start, aligned - start);
            uintptr_t tail = raw_end - (aligned + GW_SIZE);
            if (tail) munmap((void *)(aligned + GW_SIZE), tail);
            g_guest_host_base = aligned;
            g_active = 1;
            g_aligned = 1;
            g_init_result = 0;
#if WC3_DEBUG && WC3_DEBUG_MEMORY
            fprintf(stderr,
                    "[mem] guest window base=%p size=0x%llX page=%zu aligned=1 "
                    "(trimmed)\n",
                    (void *)g_guest_host_base, (unsigned long long)GW_SIZE,
                    g_page);
#endif
            return;
        }
        munmap(raw, (size_t)(GW_SIZE * 2u));
    }

    fprintf(stderr, "[mem] guest window: no 4 GiB host range (errno=%d %s)\n",
            errno, strerror(errno));
    g_init_result = -1;
}

int wc3_guest_window_init(void)
{
    pthread_once(&g_once, gw_try_once);
    return g_init_result;
}

int wc3_guest_window_active(void)
{
    (void)wc3_guest_window_init();
    return g_active;
}

int wc3_guest_window_aligned(void)
{
    return g_aligned;
}

uintptr_t wc3_guest_window_base(void)
{
    return g_guest_host_base;
}

size_t wc3_guest_window_host_page_size(void)
{
    return g_page;
}

void *wc3_guest_window_host_ptr(uint32_t gva)
{
    return guest_host_ptr(gva);
}

int wc3_guest_window_contains(uint32_t gva, uint32_t size)
{
    return gw_contains(gva, size);
}

int wc3_guest_window_reserve(uint32_t gva, uint32_t size)
{
    if (wc3_guest_window_init() != 0 || !g_active) return -1;
    pthread_mutex_lock(&g_lock);
    int rc = gw_reserve_locked(gva, size);
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int wc3_guest_window_commit(uint32_t gva, uint32_t size)
{
    if (wc3_guest_window_init() != 0 || !g_active) return -1;
    if (!gw_contains(gva, size)) return -1;
    pthread_mutex_lock(&g_lock);
    int rc = -1;
    if (gw_covered_locked(gva, gva + size)) {
        rc = gw_mprotect(gva, size, PROT_READ | PROT_WRITE);
        if (rc == 0) rc = gw_set_prot_locked(gva, gva + size,
                                             PROT_READ | PROT_WRITE);
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int wc3_guest_window_map(uint32_t gva, uint32_t size)
{
    if (wc3_guest_window_init() != 0 || !g_active) return -1;
    pthread_mutex_lock(&g_lock);
    int rc = gw_map_locked(gva, size, PROT_READ | PROT_WRITE);
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int wc3_guest_window_decommit(uint32_t gva, uint32_t size)
{
    if (wc3_guest_window_init() != 0 || !g_active) return -1;
    if (!gw_contains(gva, size)) return -1;
    pthread_mutex_lock(&g_lock);
    int rc = -1;
    if (gw_covered_locked(gva, gva + size)) {
        rc = gw_mprotect(gva, size, PROT_NONE);
        if (rc == 0) rc = gw_set_prot_locked(gva, gva + size, 0);
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int wc3_guest_window_release(uint32_t gva, uint32_t size)
{
    if (wc3_guest_window_init() != 0 || !g_active) return -1;
    if (!gw_contains(gva, size)) return -1;
    pthread_mutex_lock(&g_lock);
    int rc = gw_mprotect(gva, size, PROT_NONE);
    if (rc == 0) gw_remove_locked(gva, gva + size);
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int wc3_guest_window_protect(uint32_t gva, uint32_t size, int host_prot)
{
    if (wc3_guest_window_init() != 0 || !g_active) return -1;
    if (!gw_contains(gva, size)) return -1;
    pthread_mutex_lock(&g_lock);
    int rc = -1;
    if (gw_covered_locked(gva, gva + size)) {
        rc = gw_mprotect(gva, size, host_prot);
        if (rc == 0) rc = gw_set_prot_locked(gva, gva + size, host_prot);
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

static uintptr_t lowmem_align_up(uintptr_t value, uintptr_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

int wc3_guest_window_place(uint64_t size, uint64_t alignment, uint32_t *gva)
{
    if (!gva) return -1;
    *gva = 0;
#if WC3_DEBUG && WC3_DEBUG_MEMORY
    g_place_calls++;
#endif
    if (wc3_guest_window_init() != 0 || !g_active) return -1;
    if (!size || size > (uint64_t)(WC3_LOWMEM_END - WC3_LOWMEM_BEGIN))
        return -1;
    if (!alignment) alignment = 4096u;
    if (alignment < 4096u || (alignment & (alignment - 1u)) != 0)
        return -1;
    /* The host mapping is page granular; never place a partial host page. */
    uintptr_t align = (uintptr_t)alignment;
    if (g_page > align) align = g_page;

    /* The driver requires the placed size to be a multiple of its alignment. */
    uint64_t span64 = (size + align - 1u) & ~(align - 1u);
    if (span64 > (uint64_t)(WC3_LOWMEM_END - WC3_LOWMEM_BEGIN))
        return -1;
    if (span64 > 0xFFFFFFFFull) return -1;
    uint32_t span = (uint32_t)span64;

    pthread_mutex_lock(&g_lock);
    for (unsigned pass = 0; pass < 2; pass++) {
        uintptr_t addr = (pass == 0)
            ? lowmem_align_up(g_lowmem_next, align)
            : lowmem_align_up((uintptr_t)WC3_LOWMEM_BEGIN, align);
        if (addr < WC3_LOWMEM_BEGIN) addr = WC3_LOWMEM_BEGIN;
        while (addr + span <= (uintptr_t)WC3_LOWMEM_END) {
            if (g_range_count >= GW_MAX_RANGES) break;
            if (gw_reserve_locked((uint32_t)addr, span) == 0) {
                /* The window is one long mapping.  Remove the selected range
                 * so the driver's MAP_FIXED mapping can take its place; the
                 * registry entry keeps the guest allocator out of it. */
                void *host = (void *)(g_guest_host_base + addr);
                if (munmap(host, span) != 0) {
                    gw_remove_locked((uint32_t)addr,
                                     (uint32_t)(addr + span));
                    pthread_mutex_unlock(&g_lock);
                    return -1;
                }
                g_lowmem_next = addr + span;
                pthread_mutex_unlock(&g_lock);
                *gva = (uint32_t)addr;
#if WC3_DEBUG && WC3_DEBUG_MEMORY
                g_place_ok++;
#endif
                return 0;
            }
            addr += align;
        }
    }
    g_lowmem_next = WC3_LOWMEM_BEGIN;
    pthread_mutex_unlock(&g_lock);
#if WC3_DEBUG && WC3_DEBUG_MEMORY
    g_place_fail++;
#endif
    return -1;
}

void wc3_guest_window_dump(void)
{
    if (wc3_guest_window_init() != 0) {
        fprintf(stderr, "[mem] guest window: unavailable\n");
        return;
    }
    if (!g_active) {
        fprintf(stderr, "[mem] guest window: identity mode\n");
        return;
    }
    pthread_mutex_lock(&g_lock);
    fprintf(stderr, "[mem] guest window base=%p page=%zu ranges=%u\n",
            (void *)g_guest_host_base, g_page, g_range_count);
#if WC3_DEBUG && WC3_DEBUG_MEMORY
    fprintf(stderr,
            "[mem] guest window place calls=%llu ok=%llu fail=%llu\n",
            g_place_calls, g_place_ok, g_place_fail);
#endif
    unsigned n = g_range_count < 48u ? g_range_count : 48u;
    for (unsigned i = 0; i < n; i++)
        fprintf(stderr, "[mem]   guest %08X+%08X -> host %p prot=%d\n",
                g_ranges[i].lo, g_ranges[i].hi - g_ranges[i].lo,
                guest_host_ptr(g_ranges[i].lo), g_ranges[i].prot);
    if (n < g_range_count)
        fprintf(stderr, "[mem]   ... %u more\n", g_range_count - n);
    pthread_mutex_unlock(&g_lock);
}
