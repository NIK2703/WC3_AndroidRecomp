/*
 * headless_mem.c - Wc3MemApi for the verification target.
 *
 * Guest addresses are fixed 32-bit VAs.  Windows uses VirtualAlloc with
 * MEM_RESERVE / MEM_COMMIT so an image cannot silently replace an existing
 * mapping.  POSIX uses mmap with MAP_FIXED_NOREPLACE (and a safe hinted
 * fallback on old kernels) plus mprotect.
 */
#include "headless_internal.h"

#include <errno.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

static int valid_range(uint32_t base, uint32_t size)
{
    return size != 0u && (uint64_t)base + (uint64_t)size <= 0x100000000ull;
}

#ifdef _WIN32

static int headless_mem_reserve(void *ctx, uint32_t base, uint32_t size)
{
    (void)ctx;
    if (!valid_range(base, size)) return -1;
    return VirtualAlloc((void *)(uintptr_t)base, (size_t)size, MEM_RESERVE,
                        PAGE_NOACCESS)
               ? 0
               : -1;
}

static int headless_mem_commit(void *ctx, uint32_t base, uint32_t size)
{
    (void)ctx;
    if (!valid_range(base, size)) return -1;
    return VirtualAlloc((void *)(uintptr_t)base, (size_t)size, MEM_COMMIT,
                        PAGE_READWRITE)
               ? 0
               : -1;
}

static int headless_mem_decommit(void *ctx, uint32_t base, uint32_t size)
{
    (void)ctx;
    if (!valid_range(base, size)) return -1;
    return VirtualFree((void *)(uintptr_t)base, (size_t)size, MEM_DECOMMIT)
               ? 0
               : -1;
}

static int headless_mem_release(void *ctx, uint32_t base, uint32_t size)
{
    (void)ctx;
    (void)size;
    if (base == 0u) return -1;
    return VirtualFree((void *)(uintptr_t)base, 0, MEM_RELEASE) ? 0 : -1;
}

static int headless_mem_protect(void *ctx, uint32_t base, uint32_t size,
                                uint32_t prot)
{
    (void)ctx;
    if (!valid_range(base, size)) return -1;
    DWORD native;
    if ((prot & WC3_MEM_EXEC) != 0u) {
        native = (prot & WC3_MEM_WRITE) != 0u ? PAGE_EXECUTE_READWRITE
                 : (prot & WC3_MEM_READ) != 0u ? PAGE_EXECUTE_READ
                                               : PAGE_EXECUTE;
    } else if ((prot & WC3_MEM_WRITE) != 0u) {
        native = PAGE_READWRITE;
    } else if ((prot & WC3_MEM_READ) != 0u) {
        native = PAGE_READONLY;
    } else {
        native = PAGE_NOACCESS;
    }
    DWORD old;
    return VirtualProtect((void *)(uintptr_t)base, (size_t)size, native, &old)
               ? 0
               : -1;
}

#else /* POSIX */

static void *headless_map_fixed(uint32_t base, uint32_t size, int prot)
{
    if (!valid_range(base, size)) {
        errno = EINVAL;
        return MAP_FAILED;
    }

#ifdef MAP_FIXED_NOREPLACE
    void *p = mmap((void *)(uintptr_t)base, (size_t)size, prot,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
                       MAP_FIXED_NOREPLACE,
                   -1, 0);
    if (p != MAP_FAILED) return p;
    if (errno != EEXIST && errno != EINVAL) return MAP_FAILED;
#endif

    /* A hinted mapping without MAP_FIXED cannot clobber an existing one; if
     * the kernel places it elsewhere, reject the result. */
    void *hinted = mmap((void *)(uintptr_t)base, (size_t)size, prot,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (hinted == MAP_FAILED) return MAP_FAILED;
    if ((uintptr_t)hinted != (uintptr_t)base) {
        (void)munmap(hinted, (size_t)size);
        errno = EEXIST;
        return MAP_FAILED;
    }
    return hinted;
}

static int headless_mem_reserve(void *ctx, uint32_t base, uint32_t size)
{
    (void)ctx;
    return headless_map_fixed(base, size, PROT_NONE) == MAP_FAILED ? -1 : 0;
}

static int headless_mem_commit(void *ctx, uint32_t base, uint32_t size)
{
    (void)ctx;
    if (!valid_range(base, size)) return -1;
    return mprotect((void *)(uintptr_t)base, (size_t)size,
                    PROT_READ | PROT_WRITE) == 0
               ? 0
               : -1;
}

static int headless_mem_decommit(void *ctx, uint32_t base, uint32_t size)
{
    (void)ctx;
    if (!valid_range(base, size)) return -1;
#ifdef MADV_DONTNEED
    (void)madvise((void *)(uintptr_t)base, (size_t)size, MADV_DONTNEED);
#endif
    return mprotect((void *)(uintptr_t)base, (size_t)size, PROT_NONE) == 0
               ? 0
               : -1;
}

static int headless_mem_release(void *ctx, uint32_t base, uint32_t size)
{
    (void)ctx;
    if (!valid_range(base, size)) return -1;
    return munmap((void *)(uintptr_t)base, (size_t)size) == 0 ? 0 : -1;
}

static int headless_mem_protect(void *ctx, uint32_t base, uint32_t size,
                                uint32_t prot)
{
    (void)ctx;
    if (!valid_range(base, size)) return -1;
    int native = PROT_NONE;
    if ((prot & WC3_MEM_READ) != 0u) native |= PROT_READ;
    if ((prot & WC3_MEM_WRITE) != 0u) native |= PROT_WRITE;
    if ((prot & WC3_MEM_EXEC) != 0u) native |= PROT_EXEC;
    return mprotect((void *)(uintptr_t)base, (size_t)size, native) == 0 ? 0
                                                                         : -1;
}

#endif

void headless_mem_install(Wc3Platform *platform, HeadlessState *st)
{
    platform->mem.ctx = st;
    platform->mem.reserve = headless_mem_reserve;
    platform->mem.commit = headless_mem_commit;
    platform->mem.decommit = headless_mem_decommit;
    platform->mem.release = headless_mem_release;
    platform->mem.protect = headless_mem_protect;
}
