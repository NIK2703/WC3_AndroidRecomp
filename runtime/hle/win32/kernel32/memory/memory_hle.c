/*
 * memory_hle.c - kernel32 heap/global memory imports.
 *
 * These handlers keep the as-built guest heap as the single allocator.  They
 * add the Win32 semantics the old wrappers skipped:
 *
 *   - HeapReAlloc with HEAP_ZERO_MEMORY zeroes the extended tail;
 *   - HeapReAlloc(..., 0) frees and returns NULL, as on Windows;
 *   - HeapFree/HeapSize reject pointers without a live GBlk header;
 *   - GlobalUnlock returns FALSE for fixed blocks, which is what GMEM_FIXED
 *     GlobalAlloc produces here.
 *
 * VirtualAlloc/VirtualFree/VirtualProtect/VirtualQuery remain quarantined:
 * their live-range table is private and must move together with guest_ok().
 */
#include "memory_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "legacy_shim.h"

#define GMEM_ZEROINIT     0x0040u
#define HEAP_ZERO_MEMORY  0x00000008u
#define PROCESS_HEAP      ((uint32_t)0x00090000u)

static void h_global_alloc(CPU *c)
{
    const uint32_t flags = wc3_legacy_arg32(c, 0);
    const uint32_t size = wc3_legacy_arg32(c, 1);
    c->eax = (flags & GMEM_ZEROINIT) ? wc3_legacy_guest_alloc0(size)
                                     : wc3_legacy_guest_alloc(size);
}

static void h_global_free(CPU *c)
{
    const uint32_t p = wc3_legacy_arg32(c, 0);
    if (p) wc3_legacy_guest_free(p);
    c->eax = 0;
}

static void h_global_lock(CPU *c)
{
    c->eax = wc3_legacy_arg32(c, 0);
}

static void h_global_unlock(CPU *c)
{
    /* GlobalAlloc returns fixed blocks here, so their lock count is always
     * zero and Windows returns FALSE.  The old wrapper returned TRUE. */
    (void)c;
    c->eax = 0;
}

static void h_global_size(CPU *c)
{
    c->eax = wc3_legacy_guest_size(wc3_legacy_arg32(c, 0));
}

static void h_heap_alloc(CPU *c)
{
    const uint32_t flags = wc3_legacy_arg32(c, 1);
    const uint32_t size = wc3_legacy_arg32(c, 2);
    c->eax = (flags & HEAP_ZERO_MEMORY) ? wc3_legacy_guest_alloc0(size)
                                        : wc3_legacy_guest_alloc(size);
}

static void h_heap_free(CPU *c)
{
    const uint32_t p = wc3_legacy_arg32(c, 2);
    if (!p) {
        c->eax = 1;
        return;
    }
    if (!wc3_legacy_guest_size(p)) {
        c->eax = 0;
        return;
    }
    wc3_legacy_guest_free(p);
    c->eax = 1;
}

static void h_heap_realloc(CPU *c)
{
    const uint32_t flags = wc3_legacy_arg32(c, 1);
    const uint32_t p = wc3_legacy_arg32(c, 2);
    const uint32_t size = wc3_legacy_arg32(c, 3);

    if (size == 0) {
        if (p) wc3_legacy_guest_free(p);
        c->eax = 0;
        return;
    }
    if (!p) {
        c->eax = (flags & HEAP_ZERO_MEMORY) ? wc3_legacy_guest_alloc0(size)
                                            : wc3_legacy_guest_alloc(size);
        return;
    }

    const uint32_t old_size = wc3_legacy_guest_size(p);
    if (!old_size) {
        c->eax = 0;
        return;
    }
    const uint32_t q = (flags & HEAP_ZERO_MEMORY)
                           ? wc3_legacy_guest_alloc0(size)
                           : wc3_legacy_guest_alloc(size);
    if (!q) {
        c->eax = 0;
        return;
    }
    wc3_legacy_guest_copy(q, p, old_size < size ? old_size : size);
    wc3_legacy_guest_free(p);
    c->eax = q;
}

static void h_heap_size(CPU *c)
{
    c->eax = wc3_legacy_guest_size(wc3_legacy_arg32(c, 2));
}

static void h_get_process_heap(CPU *c)
{
    c->eax = PROCESS_HEAP;
}

static void h_get_process_heaps(CPU *c)
{
    const uint32_t count = wc3_legacy_arg32(c, 0);
    const uint32_t out = wc3_legacy_arg32(c, 1);
    /* NumberOfHeaps is a count of HANDLE entries, not bytes.  The old wrapper
     * required four, so a caller with a one-entry buffer got no handle. */
    if (out && count >= 1u && wc3_legacy_guest_ok(out, 4u))
        wc3_legacy_wr32g(out, PROCESS_HEAP);
    c->eax = 1;
}

static void h_virtual_lock(CPU *c)
{
    const uint32_t base = wc3_legacy_arg32(c, 0);
    const uint32_t size = wc3_legacy_arg32(c, 1);
    c->eax = (!size || (base && wc3_legacy_guest_ok(base, size))) ? 1u : 0u;
}

static void h_virtual_unlock(CPU *c)
{
    h_virtual_lock(c);
}

static void h_flush_instruction_cache(CPU *c)
{
    /* The lifted corpus is static and the target has coherent instruction
     * fetch for the way guest code is executed; the Win32 call is a no-op
     * success. */
    (void)c;
    c->eax = 1;
}

typedef struct MemoryOverride {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
} MemoryOverride;

static const MemoryOverride g_overrides[] = {
    { "KERNEL32.dll", "GlobalAlloc", h_global_alloc },
    { "KERNEL32.dll", "GlobalFree", h_global_free },
    { "KERNEL32.dll", "GlobalLock", h_global_lock },
    { "KERNEL32.dll", "GlobalUnlock", h_global_unlock },
    { "KERNEL32.dll", "GlobalSize", h_global_size },
    { "KERNEL32.dll", "LocalAlloc", h_global_alloc },
    { "KERNEL32.dll", "LocalFree", h_global_free },
    { "KERNEL32.dll", "HeapAlloc", h_heap_alloc },
    { "KERNEL32.dll", "HeapFree", h_heap_free },
    { "KERNEL32.dll", "HeapReAlloc", h_heap_realloc },
    { "KERNEL32.dll", "HeapSize", h_heap_size },
    { "KERNEL32.dll", "GetProcessHeap", h_get_process_heap },
    { "KERNEL32.dll", "GetProcessHeaps", h_get_process_heaps },
};

typedef struct MemoryAddition {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} MemoryAddition;

static const MemoryAddition g_additions[] = {
    { "VirtualLock", 2, h_virtual_lock },
    { "VirtualUnlock", 2, h_virtual_unlock },
    { "FlushInstructionCache", 3, h_flush_instruction_cache },
};

int wc3_hle_kernel32_memory_install(void)
{
    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override(g_overrides[i].dll, g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("KERNEL32.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 1,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0] +
                 sizeof g_additions / sizeof g_additions[0]);
}
