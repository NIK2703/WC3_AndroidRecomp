/*
 * dispatch_boot.c - first executable dispatcher in the canonical tree.
 *
 * The generated corpus names one C function per guest VA (L_<va>).  This file
 * owns the mapping from a guest target to that function and the HLE window
 * check.  It deliberately has no dependency on the old host tree: the only
 * generated input is boot_dispatch_list.h, which is a list of guest VAs.
 *
 * The boot list is generated from the active corpus manifest: the legacy
 * probe list in transitional mode, or the merged multi-module list when
 * WC3_RELIFTED_CORPUS_ROOT is set.  An unknown target is fatal instead of a
 * fabricated return value.
 */
#include "legacy_shim.h"
#include "convert_hle.h"
#include "crt_hle.h"
#include "crt_string_hle.h"
#include "file_hle.h"
#include "strerror_hle.h"
#include "crt_time_hle.h"
#include "display_hle.h"
#include "heap_hle.h"
#include "math_hle.h"
#include "memory_hle.h"
#include "process_hle.h"
#include "rand_hle.h"
#include "secure_hle.h"
#include "sort_hle.h"
#include "string_hle.h"
#include "system_hle.h"
#include "time_hle.h"
#include "window_hle.h"

#include <stdio.h>
#include <stdlib.h>

#ifndef WC3_DEBUG
#define WC3_DEBUG 0
#endif

#if WC3_DEBUG && WC3_DEBUG_HLE
/* Temporary hang watchdog hook; see wc3_watch_dispatch() in hle_android.c. */
void wc3_watch_dispatch(CPU *c, uint32_t target);
#endif

#define X(a) void L_##a(CPU *c);
#include "boot_dispatch_list.h"
LIFTED_FUNCS(X)
#undef X

uint32_t g_image_delta = 0;

typedef struct {
    uint32_t va;
    void (*fn)(CPU *);
} LiftEnt;

static const LiftEnt g_lift[] = {
#define X(a) { 0x##a##u, L_##a },
    LIFTED_FUNCS(X)
#undef X
};

#define LIFT_HASH_BITS 19
#define LIFT_HASH_SIZE (1u << LIFT_HASH_BITS)
static uint32_t g_lift_hash[LIFT_HASH_SIZE];

void wc3_dispatch_init(void)
{
    /* M5 order H6.2: the first target HLE module replaces its as-built rows
     * before the dispatcher becomes visible to the guest.  A missing row is a
     * startup configuration error, not a fallback to the quarantined handler.
     * The CRT heap subset is installed here because it shares the guest heap;
     * the math slice owns the x87/SSE2 return conventions, and the time slice
     * reads both clocks through Wc3TimeApi.  The rest of msvcr80 remains
     * quarantined for its later H6.2 step. */
#define WC3_INSTALL_STEP(fn) do { \
        int _r = (fn); \
        fprintf(stderr, "[hle] install %s -> %d\n", #fn, _r); \
        if (_r < 0) abort(); \
    } while (0)
    WC3_INSTALL_STEP(wc3_hle_kernel32_time_install());
    WC3_INSTALL_STEP(wc3_hle_kernel32_process_install());
    WC3_INSTALL_STEP(wc3_hle_kernel32_system_install());
    WC3_INSTALL_STEP(wc3_hle_kernel32_memory_install());
    WC3_INSTALL_STEP(wc3_hle_kernel32_string_install());
    WC3_INSTALL_STEP(user32_display_install());
    WC3_INSTALL_STEP(wc3_hle_user32_window_install());
    WC3_INSTALL_STEP(wc3_hle_msvcr80_heap_install());
    WC3_INSTALL_STEP(wc3_hle_msvcr80_math_install());
    WC3_INSTALL_STEP(wc3_hle_msvcr80_crt_install());
    WC3_INSTALL_STEP(wc3_hle_msvcr80_file_install());
    WC3_INSTALL_STEP(wc3_hle_msvcr80_rand_install());
    WC3_INSTALL_STEP(msvcr80_secure_install());
    WC3_INSTALL_STEP(msvcr80_error_install());
    WC3_INSTALL_STEP(msvcr80_sort_install());
    WC3_INSTALL_STEP(wc3_hle_msvcr80_string_install());
    WC3_INSTALL_STEP(msvcr80_convert_install());
    WC3_INSTALL_STEP(wc3_hle_msvcr80_time_install());
#undef WC3_INSTALL_STEP

    size_t count = sizeof g_lift / sizeof g_lift[0];
    for (size_t i = 0; i < count; i++) {
        uint32_t h = (g_lift[i].va * 2654435761u) >> (32 - LIFT_HASH_BITS);
        while (g_lift_hash[h])
            h = (h + 1) & (LIFT_HASH_SIZE - 1);
        g_lift_hash[h] = (uint32_t)(i + 1);
    }
}

void wc3_dispatch_shutdown(void)
{
}

static inline void (*find_lifted(uint32_t va))(CPU *)
{
    uint32_t h = (va * 2654435761u) >> (32 - LIFT_HASH_BITS);
    for (unsigned n = 0; n < LIFT_HASH_SIZE; n++) {
        uint32_t e = g_lift_hash[h];
        if (!e)
            return NULL;
        if (g_lift[e - 1].va == va)
            return g_lift[e - 1].fn;
        h = (h + 1) & (LIFT_HASH_SIZE - 1);
    }
    return NULL;
}

static THREAD_LOCAL int g_disp_depth;
static THREAD_LOCAL int g_tail_pending;
static THREAD_LOCAL uint32_t g_tail_target;

static void dispatch_unknown(CPU *c, uint32_t target, int tail)
{
    uint32_t *sp = (uint32_t *)(uintptr_t)c->esp;
    fprintf(stderr,
            "[dispatch] %s unknown guest target %08X (eip=%08X esp=%08X ra=%08X %s)\n",
            tail ? "jmp" : "call", target, c->eip, c->esp,
            sp ? sp[0] : 0, wc3_legacy_hle_describe(c->eip));
    if (sp)
        fprintf(stderr, "[dispatch]   stk: %08X %08X %08X %08X %08X %08X\n",
                sp[0], sp[1], sp[2], sp[3], sp[4], sp[5]);
    abort();
}

static void dispatch_common(CPU *c, uint32_t target, int tail)
{
    int chained = 0;
    g_disp_depth++;
    for (;;) {
        g_tail_pending = 0;
#if WC3_DEBUG && WC3_DEBUG_HLE
        wc3_watch_dispatch(c, target);
#endif
        if (target >= GUEST_HLE_BASE &&
            target < GUEST_HLE_BASE + WC3_LEGACY_HLE_MAX *
                                      WC3_LEGACY_HLE_STRIDE) {
            wc3_legacy_hle_call(target, c);
            break;
        }
        void (*fn)(CPU *) = find_lifted(target);
        if (fn) {
            fn(c);
            if (!g_tail_pending)
                break;
            chained = 1;
            target = g_tail_target;
            continue;
        }
        dispatch_unknown(c, target, tail || chained);
    }
    g_disp_depth--;
}

void dispatch(CPU *c, uint32_t target)
{
    dispatch_common(c, target, 0);
}

void dispatch_jmp(CPU *c, uint32_t target)
{
    if (g_disp_depth > 0) {
        g_tail_pending = 1;
        g_tail_target = target;
        return;
    }
    dispatch_common(c, target, 1);
}
