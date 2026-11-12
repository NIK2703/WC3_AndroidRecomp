/* Fixed low-address guest mappings. See host.h. */
#include "host.h"
#include "wc3_debug.h"
#include <stdio.h>
#include <string.h>

/* Ranges committed after startup.  guest_ok() (runtime/android/hle_android.c)
 * validates a guest pointer against a static table of the *layout*, and the
 * D3D9 staging arena is deliberately placed in a gap outside every entry of
 * that table.  The consequence was that cr_memcpy()
 * refused every copy into a locked vertex/index buffer or surface and returned
 * success anyway: the guest filled nothing, and the picture was built from
 * stale bytes.  Recording committed ranges here makes the validator exact -
 * these ranges are RW and mapped, so accepting them is not a loosening.
 *
 * The direct D3D9 path registers each buffer DXVK placed below 4 GB, so the
 * table also holds one entry per distinct dynamic buffer.  The buffer ranges
 * coalesce when they are adjacent, but 96 entries was sized for the staging
 * arena alone; 512 keeps a game with many small buffers from silently falling
 * back to the shadow copy. */
#define GUEST_EXTRA_MAX 512
static struct { uint32_t lo, hi; } g_extra[GUEST_EXTRA_MAX];
static int g_extra_n;   /* claimed slots; may run ahead of the filled ones */

/* The direct D3D9 path appends entries from a guest (render) thread while
 * cr_memcpy() reads the table on other guest threads, and a lazy guest arena
 * can be committed from another thread at the same time.  Two rules keep the
 * lock-free append exact:
 *
 *   - a slot is claimed with one atomic fetch_add, so two writers never fill
 *     the same slot with different bounds;
 *   - a reader treats lo == 0 or hi == 0 as "not filled yet" and skips it.
 *     guest_range_add() rejects such bounds, so a real entry never has them.
 *     That also covers the compiler publishing the claimed slot before the two
 *     field stores: the reader sees a zero, not a half-range.
 *
 * Coalescing only widens an existing entry, and two ranges that both overlap
 * the same entry are connected through it; any intermediate pair of old/new
 * bounds is a subset of their contiguous union.  A torn coalesced pair can
 * therefore only cause a false reject, never validate unmapped memory. */
static int guest_extra_count(void)
{
    int n = __atomic_load_n(&g_extra_n, __ATOMIC_RELAXED);
    return n < GUEST_EXTRA_MAX ? n : GUEST_EXTRA_MAX;
}

void guest_range_add(uint32_t base, uint32_t size)
{
    if (!base || !size) return;
    uint32_t hi = base + size;
    if (hi < base) return;                    /* wrapped: not a range */
    int n = guest_extra_count();
    /* Coalesce with an adjacent or overlapping entry: the staging arena is
     * committed one page-run at a time as locks come in, so consecutive calls
     * extend the previous range rather than adding a new one. */
    for (int i = 0; i < n; i++) {
        if (!g_extra[i].lo || !g_extra[i].hi) continue;
        if (hi >= g_extra[i].lo && base <= g_extra[i].hi) {
            if (base < g_extra[i].lo) g_extra[i].lo = base;
            if (hi > g_extra[i].hi) g_extra[i].hi = hi;
            return;
        }
    }
    int slot = __atomic_fetch_add(&g_extra_n, 1, __ATOMIC_RELAXED);
    if (slot < GUEST_EXTRA_MAX) {
        g_extra[slot].lo = base;
        g_extra[slot].hi = hi;
    } else {
        /* Dropping a range here makes guest_ok() refuse real, mapped memory,
         * and the caller is told the copy succeeded - the exact failure mode
         * that made the staging arena unusable in the first place.  The
         * staging arena is a monotonic bump allocator, but the direct buffer
         * path can fill the table with many distinct ranges; either way a
         * drop must never be quiet. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[mem] range table full (%d): %08X+%X not recorded; "
                            "guest_ok() will refuse it\n",
                    GUEST_EXTRA_MAX, base, size);
        }
    }
}

int guest_range_ok(uint32_t base, uint32_t size)
{
    uint64_t lo = base, hi = (uint64_t)base + size;
    if (hi > 0x100000000ull) return 0;
    int n = guest_extra_count();
    for (int i = 0; i < n; i++) {
        uint32_t elo = g_extra[i].lo, ehi = g_extra[i].hi;
        if (!elo || !ehi) continue;
        if (lo >= elo && hi <= ehi) return 1;
    }
    return 0;
}

/* Register a range that is already mapped RW by a host allocator: the thin
 * D3D9 bridge hands the guest DXVK device memory directly when the driver
 * placed it below 4 GB.  Unlike guest_commit() this must not mprotect - the
 * pages belong to the Vulkan driver's mapping, and the mapping already has
 * the access the guest needs.  The return value says whether the validator
 * accepted the range, so a full table falls back to the shadow copy instead
 * of silently letting guest writes be dropped. */
int guest_range_register(uint32_t base, uint32_t size)
{
    if (!base || !size) return 0;
    if ((uint64_t)base + size > 0x100000000ull) return 0;
    guest_range_add(base, size);
    return guest_range_ok(base, size);
}

/* How much of [base, base+n) one recorded range can vouch for.  Same contract
 * as the layout-table clamp in hle_android.c. */
uint32_t guest_range_clamp(uint32_t base, uint32_t n)
{
    int count = guest_extra_count();
    for (int i = 0; i < count; i++) {
        uint32_t lo = g_extra[i].lo, hi = g_extra[i].hi;
        if (!lo || !hi) continue;
        if (base < lo || base >= hi) continue;
        uint32_t avail = hi - base;
        return n < avail ? n : avail;
    }
    return 0;
}

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void *guest_map(uint32_t base, uint32_t size) {
    void *p = VirtualAlloc((void *)(uintptr_t)base, size,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fprintf(stderr, "[mem] VirtualAlloc(%08X, %X) failed: %lu\n",
                    base, size, (unsigned long)GetLastError());
    return p;
}
void guest_unmap(uint32_t base, uint32_t size) {
    (void)size;
    if (base) VirtualFree((void *)(uintptr_t)base, 0, MEM_RELEASE);
}
int guest_commit(uint32_t base, uint32_t size) {
    if (!VirtualAlloc((void *)(uintptr_t)base, size, MEM_COMMIT, PAGE_READWRITE))
        return 0;
    guest_range_add(base, size);
    return 1;
}
#else
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

/* Which existing mapping is in the way.  A bare EEXIST from
 * MAP_FIXED_NOREPLACE says nothing, and the answer differs between a shell
 * process (where the whole low address space is empty) and an app process
 * (where the runtime has already placed something there), which is exactly the
 * difference that made the guest fail to start inside the APK. */
void guest_describe_conflict(uint32_t base, uint32_t size) {
#if WC3_DEBUG && WC3_DEBUG_MEMORY
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    uint64_t lo = base, hi = (uint64_t)base + size;
    while (fgets(line, sizeof line, f)) {
        unsigned long a = 0, b = 0;
        if (sscanf(line, "%lx-%lx", &a, &b) != 2) continue;
        if (b <= lo || a >= hi) continue;
        fprintf(stderr, "[mem]   occupied: %s", line);
    }
    fclose(f);
#else
    (void)base;
    (void)size;
#endif
}

void *guest_map(uint32_t base, uint32_t size) {
    void *want = (void *)(uintptr_t)base;
    void *p = mmap(want, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "[mem] mmap(%08X, %X) failed: %s\n", base, size, strerror(errno));
        guest_describe_conflict(base, size);
        return NULL;
    }
    if (p != want) {
        fprintf(stderr, "[mem] mmap(%08X) landed at %p\n", base, p);
        munmap(p, size);
        return NULL;
    }
    return p;
}
void guest_unmap(uint32_t base, uint32_t size) {
    if (base) munmap((void *)(uintptr_t)base, size);
}
/* Commit a reserved range: Windows spells this MEM_COMMIT, POSIX spells it
 * mprotect.  It is not a no-op even though guest_map() maps RW, because the
 * D3D9 bridge reserves its 32-bit staging arena with PROT_NONE and commits it
 * a page at a time as locks come in - a no-op here left every lock pointing at
 * an inaccessible page and the guest died on the first LockRect. */
int guest_commit(uint32_t base, uint32_t size) {
    if (!base || !size) return 0;
    if (mprotect((void *)(uintptr_t)base, size, PROT_READ | PROT_WRITE) != 0)
        return 0;
    guest_range_add(base, size);
    return 1;
}
#endif
