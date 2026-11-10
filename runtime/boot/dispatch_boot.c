/*
 * dispatch_boot.c - first executable dispatcher in the canonical tree.
 *
 * The generated corpus names one C function per guest VA (L_<va>).  This file
 * owns the mapping from a guest target to that function and the HLE window
 * check.  It deliberately has no dependency on the old host tree: the
 * generated inputs are boot_dispatch_list.h (the guest VA list) and, since
 * O-24, boot_dispatch_hash.h (the same open-addressing table as a constant).
 *
 * The boot list is generated from the active corpus manifest: the legacy
 * probe list in transitional mode, or the merged multi-module list when
 * WC3_RELIFTED_CORPUS_ROOT is set.  An unknown target is fatal instead of a
 * fabricated return value.
 */
#include "hle_bridge.h"
#include "direct_call.h"
#include "convert_hle.h"
#include "crt_hle.h"
#include "crt_string_hle.h"
#include "file_hle.h"
#include "gdi_dc_hle.h"
#include "gdi_device_hle.h"
#include "gdi_obj_hle.h"
#include "gdi_palette_hle.h"
#include "stdio_hle.h"
#include "strerror_hle.h"
#include "crt_time_hle.h"
#include "display_hle.h"
#include "dragdrop_hle.h"
#include "dsound_hle.h"
#include "heap_hle.h"
#include "imagelist_hle.h"
#include "kernel32_file_hle.h"
#include "math_hle.h"
#include "memory_hle.h"
#include "menu_hle.h"
#include "process_hle.h"
#include "rand_hle.h"
#include "registry_hle.h"
#include "resource_hle.h"
#include "scroll_hle.h"
#include "secure_hle.h"
#include "sort_hle.h"
#include "string_hle.h"
#include "system_hle.h"
#include "time_hle.h"
#include "user_object_hle.h"
#include "window_facts_hle.h"
#include "window_hle.h"
#include "mss_timer_hle.h"
#include "winmm_guard_hle.h"
#include "ws2_32_hle.h"

#include <stdio.h>
#include <stdlib.h>

#include "wc3_debug.h"

#if WC3_DEBUG && WC3_DEBUG_HLE
/* Temporary hang watchdog hook; see wc3_watch_dispatch() in hle_android.c. */
void wc3_watch_dispatch(CPU *c, uint32_t target);
#include "wc3_lock_trace.h"
#endif

#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
/* Exact-address Battle.net probes.  Keep this separate from the HLE hang
 * watchdog: the dispatcher must call it even when WC3_DEBUG_HLE is off. */
void wc3_battle_net_dispatch(CPU *c, uint32_t target);
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

/* O-24/O-53: the dispatch map used to be rebuilt from a separate g_lift[]
 * table on every startup.  tools/gen_boot_dispatch.py now publishes the same
 * open-addressing map as a constant, and each slot owns both the guest VA and
 * the lifted function pointer.  find_lifted() therefore makes one probe read
 * instead of reading g_lift_hash[e - 1] after g_lift[e - 1].  The Knuth
 * multiplier and linear probe order are unchanged.  No speed claim is made
 * until G-10/PM-19 measures cold_start. */
#include "boot_dispatch_hash.h"

void wc3_dispatch_init(void)
{
    /* M5 order H6.2: the first target HLE module replaces its as-built rows
     * before the dispatcher becomes visible to the guest.  A missing row is a
     * startup configuration error, not a fallback to the as-built handler.
     * The CRT heap subset is installed here because it shares the guest heap;
     * the math slice owns the x87/SSE2 return conventions, and the time slice
     * reads both clocks through Wc3TimeApi.  The rest of msvcr80 remains in
     * the as-built HLE for its later H6.2 step. */
#if WC3_DEBUG && WC3_DEBUG_HLE
#define WC3_INSTALL_STEP(fn) do { \
        int _r = (fn); \
        fprintf(stderr, "[hle] install %s -> %d\n", #fn, _r); \
        if (_r < 0) abort(); \
    } while (0)
#else
#define WC3_INSTALL_STEP(fn) do { \
        if ((fn) < 0) abort(); \
    } while (0)
#endif
    WC3_INSTALL_STEP(wc3_hle_kernel32_time_install());
    WC3_INSTALL_STEP(wc3_hle_comctl32_imagelist_install());
    WC3_INSTALL_STEP(wc3_hle_shell32_dragdrop_install());
    WC3_INSTALL_STEP(wc3_hle_kernel32_process_install());
    WC3_INSTALL_STEP(wc3_hle_kernel32_system_install());
    WC3_INSTALL_STEP(wc3_hle_kernel32_memory_install());
    WC3_INSTALL_STEP(wc3_hle_kernel32_file_install());
    WC3_INSTALL_STEP(wc3_hle_kernel32_resource_install());
    WC3_INSTALL_STEP(wc3_hle_kernel32_string_install());
    WC3_INSTALL_STEP(wc3_hle_advapi32_registry_install());
    WC3_INSTALL_STEP(wc3_hle_gdi32_dc_install());
    WC3_INSTALL_STEP(wc3_hle_gdi32_device_install());
    WC3_INSTALL_STEP(wc3_hle_gdi32_objects_install());
    WC3_INSTALL_STEP(wc3_hle_gdi32_palette_install());
    WC3_INSTALL_STEP(user32_display_install());
    WC3_INSTALL_STEP(wc3_hle_user32_menu_install());
    WC3_INSTALL_STEP(wc3_hle_user32_scroll_install());
    WC3_INSTALL_STEP(wc3_hle_user32_window_install());
    WC3_INSTALL_STEP(wc3_hle_user32_window_facts_install());
    WC3_INSTALL_STEP(wc3_hle_user32_user_object_install());
    WC3_INSTALL_STEP(wc3_hle_msvcr80_heap_install());
    WC3_INSTALL_STEP(wc3_hle_msvcr80_math_install());
    WC3_INSTALL_STEP(wc3_hle_msvcr80_crt_install());
    WC3_INSTALL_STEP(wc3_hle_msvcr80_file_install());
    WC3_INSTALL_STEP(wc3_hle_msvcr80_stdio_install());
    WC3_INSTALL_STEP(wc3_hle_msvcr80_rand_install());
    WC3_INSTALL_STEP(msvcr80_secure_install());
    WC3_INSTALL_STEP(msvcr80_error_install());
    WC3_INSTALL_STEP(msvcr80_sort_install());
    WC3_INSTALL_STEP(wc3_hle_msvcr80_string_install());
    WC3_INSTALL_STEP(msvcr80_convert_install());
    WC3_INSTALL_STEP(wc3_hle_msvcr80_time_install());
    WC3_INSTALL_STEP(wc3_hle_ws2_32_install());
    /* Mss32's 50 ms mixer timer is USER32 SetTimer, not waveOut.  This owns
     * only the timer/message/wait seam; it produces no PCM. */
    WC3_INSTALL_STEP(wc3_hle_mss_timer_install());
    /* The frozen corpus still imports the eleven WINMM waveOut entry points.
     * Bind them to the explicit no-driver guard so no waveOut sink can be
     * selected.  There is no rollback trigger. */
    WC3_INSTALL_STEP(wc3_hle_winmm_guard_install());
    /* ADR-034 / DirectSound product path: the COM contract and the
     * synthetic DSOUND.DLL loader chain are installed in EVERY build.  Mss32
     * reaches these rows through KERNEL32!LoadLibraryA/GetProcAddress, so no
     * corpus re-lift is required. */
    WC3_INSTALL_STEP(wc3_hle_dsound_install());
#undef WC3_INSTALL_STEP

#if WC3_DEBUG && WC3_DEBUG_HLE
    /* Ownership tracing for the registry lock: a frozen guest can show its
     * waiters but not the thread that never released the lock. */
    wc3_lock_trace_start();
#endif

    /* O-24/O-53: g_lift_hash is a generated constant map; there is nothing to
     * build here.  Hash and probe semantics are unchanged from the loop that
     * used to run at this point. */
}

void wc3_dispatch_shutdown(void)
{
}

static inline void (*find_lifted(uint32_t va))(CPU *)
{
    uint32_t h = (va * 2654435761u) >> (32 - LIFT_HASH_BITS);
    for (unsigned n = 0; n < LIFT_HASH_SIZE; n++) {
        const LiftEnt *e = &g_lift_hash[h];
        if (!e->va)
            return NULL;
        if (e->va == va)
            return e->fn;
        h = (h + 1) & (LIFT_HASH_SIZE - 1);
    }
    return NULL;
}

static void dispatch_unknown(CPU *c, uint32_t target, int tail)
{
    /* The dump is diagnostic only.  Release keeps the fatal outcome above
     * all, but must not retain the target/stack formatter or its string
     * literals (BV-10, O-72). */
#if WC3_DEBUG && WC3_DEBUG_HLE
    uint32_t *sp = (uint32_t *)(uintptr_t)c->esp;
    fprintf(stderr,
            "[dispatch] %s unknown guest target %08X (eip=%08X esp=%08X ra=%08X %s)\n",
            tail ? "jmp" : "call", target, c->eip, c->esp,
            sp ? sp[0] : 0, wc3_legacy_hle_describe(c->eip));
    if (sp)
        fprintf(stderr, "[dispatch]   stk: %08X %08X %08X %08X %08X %08X\n",
                sp[0], sp[1], sp[2], sp[3], sp[4], sp[5]);
#else
    (void)c;
    (void)target;
    (void)tail;
#endif
    abort();
}

static void dispatch_common(CPU *c, uint32_t target, int tail,
                            wc3_lifted_fn resolved)
{
    int chained = 0;
    int have_fn = resolved != NULL;
    void (*pending_fn)(CPU *) = resolved;
#if WC3_DEBUG && WC3_DEBUG_HLE
    uint32_t lt_op = 0, lt_lock = 0, lt_arg = 0;
    uint32_t lt_ra = 0, lt_caller = 0, lt_gid = 0;
    int lt_tracked = wc3_lock_trace_enter(c, target, &lt_op, &lt_lock,
                                          &lt_arg, &lt_ra, &lt_caller,
                                          &lt_gid);
#endif
    c->disp_depth++;
    for (;;) {
        c->disp_pending_valid = 0;
        if (have_fn) {
            void (*fn)(CPU *) = pending_fn;
            have_fn = 0;
            pending_fn = NULL;
            fn(c);
        } else {
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
            wc3_battle_net_dispatch(c, target);
#endif
#if WC3_DEBUG && WC3_DEBUG_HLE
            wc3_watch_dispatch(c, target);
#endif
            if (target >= GUEST_HLE_BASE &&
                target < GUEST_HLE_BASE + WC3_LEGACY_HLE_MAX *
                                          WC3_LEGACY_HLE_STRIDE) {
                wc3_legacy_hle_call(target, c);
            } else {
                void (*fn)(CPU *) = find_lifted(target);
                if (!fn) {
                    dispatch_unknown(c, target, tail || chained);
                } else {
                    fn(c);
                }
            }
        }
        if (!c->disp_pending_valid)
            break;
        chained = 1;
        if (c->disp_pending_kind == WC3_DISP_PENDING_FN) {
            pending_fn = (wc3_lifted_fn)c->disp_pending_target;
            have_fn = 1;
        } else {
            target = (uint32_t)c->disp_pending_target;
        }
    }
    c->disp_depth--;
#if WC3_DEBUG && WC3_DEBUG_HLE
    if (lt_tracked)
        wc3_lock_trace_leave(lt_op, lt_lock, lt_arg, lt_ra, lt_caller,
                             lt_gid);
#endif
}

void dispatch(CPU *c, uint32_t target)
{
    dispatch_common(c, target, 0, NULL);
}

void dispatch_jmp(CPU *c, uint32_t target)
{
    if (c->disp_depth > 0) {
        c->disp_pending_valid = 1u;
        c->disp_pending_kind = WC3_DISP_PENDING_VA;
        c->disp_pending_target = target;
        return;
    }
    dispatch_common(c, target, 1, NULL);
}

wc3_lifted_fn wc3_dispatch_resolve(uint32_t target)
{
    /* HLE thunks live outside the generated lift table, so find_lifted()
     * already returns NULL for them; this is the same fail-closed result as
     * the normal dispatcher path. */
    return target ? find_lifted(target) : NULL;
}

void wc3_dispatch_resolved(CPU *c, wc3_lifted_fn fn)
{
    /* A NULL target must be handled by the caller with dispatch(); silently
     * skipping a callback would change guest-visible completion semantics. */
    if (!c || !fn) abort();
    dispatch_common(c, 0u, 0, fn);
}
