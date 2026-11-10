/*
 * heap_hle.c - target-side MSVCR80 CRT heap subset.
 *
 * The as-built CRT handlers allocated from the same guest heap but read the
 * block header directly and, on realloc failure, freed the old block.  Windows
 * realloc leaves the original allocation intact when the new allocation fails.
 * These handlers use the shim's header-aware size/copy helpers and preserve
 * that contract.
 *
 * malloc(0) and calloc(0, n) still return a one-byte block, matching the
 * Microsoft CRT and the as-built behavior.
 */
#include "heap_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "hle_bridge.h"

static void h_malloc(CPU *c)
{
    c->eax = wc3_legacy_guest_alloc(wc3_legacy_arg32(c, 0));
}

static void h_free(CPU *c)
{
    wc3_legacy_guest_free(wc3_legacy_arg32(c, 0));
    c->eax = 0;
}

static void h_realloc(CPU *c)
{
    const uint32_t old = wc3_legacy_arg32(c, 0);
    const uint32_t size = wc3_legacy_arg32(c, 1);

    if (!old) {
        c->eax = wc3_legacy_guest_alloc(size ? size : 1u);
        return;
    }
    if (!size) {
        wc3_legacy_guest_free(old);
        c->eax = 0;
        return;
    }

    const uint32_t old_size = wc3_legacy_guest_size(old);
    const uint32_t block = wc3_legacy_guest_alloc(size);
    if (!block) {
        /* Windows leaves the old block alive and returns NULL. */
        c->eax = 0;
        return;
    }
    wc3_legacy_guest_copy(block, old, old_size < size ? old_size : size);
    wc3_legacy_guest_free(old);
    c->eax = block;
}

static void h_calloc(CPU *c)
{
    const uint32_t count = wc3_legacy_arg32(c, 0);
    const uint32_t size = wc3_legacy_arg32(c, 1);

    if (count && size > UINT32_MAX / count) {
        c->eax = 0; /* the CRT returns NULL on multiplication overflow */
        return;
    }
    c->eax = wc3_legacy_guest_alloc0(count * size);
}

typedef struct CrtHeapOverride {
    const char *name;
    void (*impl)(CPU *c);
} CrtHeapOverride;

static const CrtHeapOverride g_overrides[] = {
    { "malloc", h_malloc },
    { "free", h_free },
    { "realloc", h_realloc },
    { "calloc", h_calloc },
    { "_malloc_crt", h_malloc },
    { "_calloc_crt", h_calloc },
};

int wc3_hle_msvcr80_heap_install(void)
{
    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("MSVCR80.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0]);
}
