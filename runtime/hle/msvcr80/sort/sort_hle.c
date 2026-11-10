/*
 * sort_hle.c - target-side MSVCR80 qsort.
 *
 * The guest comparator is a lifted cdecl function.  It receives two guest
 * pointers and returns a signed integer in eax; the return address pushed
 * below them is the dispatcher sentinel the as-built HLE has always used.
 *
 * The as-built implementation kept a thread-local 256-byte guest scratch
 * block, which silently refused every element wider than 256 bytes.  game.dll's
 * qsort call site at 0x6F039AEA pushes width 0x228 (552), so that was a real
 * skipped sort, not a theoretical edge case.  Here the block comes from the
 * shim's guest heap for the duration of one call, so the element size is not
 * capped and two guest threads sorting at the same time cannot overwrite each
 * other's pivot element.
 *
 * The as-built insertion loop shifted the previous element only when the
 * comparator returned a positive value.  A standard qsort comparator returns
 * negative when the first argument sorts before the second, so that loop
 * produced descending order (and dropped elements on the first comparison).
 * This slice follows the CRT contract: compare the pending element against its
 * predecessor, shift while the predecessor sorts after it, and stop at the
 * first predecessor that does not.
 */
#include "sort_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "hle_bridge.h"

#define DISPATCH_RETURN 0x7EFFFFF1u

static int push(CPU *c, uint32_t value)
{
    if (c->esp < 4u || !wc3_legacy_guest_ok(c->esp - 4u, 4)) return 0;
    c->esp -= 4u;
    wc3_legacy_wr32g(c->esp, value);
    return 1;
}

static void h_qsort(CPU *c)
{
    const uint32_t base = wc3_legacy_arg32(c, 0);
    const uint32_t count = wc3_legacy_arg32(c, 1);
    const uint32_t width = wc3_legacy_arg32(c, 2);
    const uint32_t compare = wc3_legacy_arg32(c, 3);

    if (!base || count < 2u || !width ||
        count > UINT32_MAX / width ||
        !wc3_legacy_guest_ok(base, count * width)) {
        c->eax = 0;
        return;
    }

    const uint32_t scratch = wc3_legacy_guest_alloc0(width);
    if (!scratch) {
        c->eax = 0;
        return;
    }

    for (uint32_t i = 1; i < count; i++) {
        wc3_legacy_guest_copy(scratch, base + i * width, width);
        uint32_t j = i;
        while (j > 0) {
            const uint32_t saved = c->esp;
            if (!push(c, base + (j - 1u) * width) ||
                !push(c, scratch) ||
                !push(c, DISPATCH_RETURN)) {
                c->esp = saved;
                wc3_legacy_guest_free(scratch);
                c->eax = 0;
                return;
            }
            dispatch(c, compare);
            c->esp = saved;
            if ((int32_t)c->eax >= 0) break;
            wc3_legacy_guest_copy(base + j * width,
                                  base + (j - 1u) * width, width);
            j--;
        }
        wc3_legacy_guest_copy(base + j * width, scratch, width);
    }

    wc3_legacy_guest_free(scratch);
    c->eax = 0;
}

typedef struct SortOverride {
    const char *name;
    void (*impl)(CPU *c);
} SortOverride;

static const SortOverride g_overrides[] = {
    { "qsort", h_qsort },
};

int msvcr80_sort_install(void)
{
    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("MSVCR80.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0]);
}
