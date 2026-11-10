/*
 * hle_android.c - POSIX/Android HLE surface for the lifted WC3 1.27 launcher.
 *
 * Design rules (same as the Windows host):
 *
 *  - HLE replaces OS and CRT services only.  Game and library code is the
 *    lifted PE code and is never reimplemented here.
 *  - An import that is not implemented is reported, never silently invented.
 *    The generic stub logs the DLL, the symbol and the guest return address,
 *    so one run says exactly what the launcher still needs.
 *  - The Win32 ABI is honoured: stdcall imports pop their own arguments, cdecl
 *    imports leave that to the caller.  Getting this wrong shows up much later
 *    as a wild pointer, so every table entry carries an explicit arity.
 *
 * Memory model: the guest image, heap and stacks are mapped at fixed 32-bit
 * addresses (docs/ANDROID.md), so a guest VA is a host pointer and rd32g() is
 * a plain dereference.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>
#include <wchar.h>
#include <pthread.h>

#include "host.h"
#include "hle_android.h"
#include "hle_input_state.h"
#include "hle_registry.h"

#if WC3_DEBUG && WC3_DEBUG_HLE
#include "wc3_watch_state.h"
#endif

#if WC3_DEBUG && WC3_DEBUG_RESOLUTION
#define RES_LOG(...) do { \
    fprintf(stderr, "[resolution] " __VA_ARGS__); \
    fputc('\n', stderr); \
} while (0)
#else
#define RES_LOG(...) do { } while (0)
#endif

/* The APK front end (native_activity.c) installs the real soft-keyboard
 * bridge here.  The standalone shell links this file without that front end,
 * so the pointers stay NULL and the guest runs exactly as before.  A function
 * pointer also keeps the call out of the same translation unit as the empty
 * definition, which a compiler would otherwise be free to inline away. */
static void (*g_keyboard_show)(void);
static void (*g_keyboard_hide)(void);

void hle_android_set_keyboard_bridge(void (*show)(void), void (*hide)(void))
{
    g_keyboard_show = show;
    g_keyboard_hide = hide;
}

/* Generated dispatcher: runs a lifted function address on a CPU. */
void dispatch(CPU *c, uint32_t target);

/* Guest argument/string helpers; defined below but used by the Miles trace
 * wrappers near the top of the file. */
uint32_t arg32(CPU *c, unsigned n);
static const char *gstr(uint32_t a);
static int guest_ok(uint32_t addr, uint32_t size);
static const char *mod_windows_path(uint32_t base);

/* ------------------------------------------------------------------ */
/* registry                                                            */
/* ------------------------------------------------------------------ */

#define HLE_RING 64

/* HleEnt, HLE_MAX and HLE_STRIDE come from hle_registry.h: hle_d3d9.cpp (the
 * guest Direct3D 9 COM surface, C++) is linked into this image and walks the
 * same table, so the layout may only be declared once. */

/* Not static: hle_d3d9.cpp indexes it with `e - g_hle`. */
HleEnt   g_hle[HLE_MAX];
unsigned g_hle_count;
/* Distinct imports that reached the generic stub, with call counts. */
#define MISS_MAX 96
static char     g_miss[MISS_MAX][104];
static unsigned g_miss_calls[MISS_MAX];
static int      g_miss_count;

/* Last imports that ran, so a fault can be attributed to one of them. */
static struct { uint32_t ra; const char *name; uint32_t esp; } g_ring[HLE_RING];
static unsigned g_ring_pos, g_ring_len;

/* The CPU that last entered an import.  A SIGSEGV can land anywhere in lifted
 * code, and the signal context is host state, so this is the only handle the
 * crash handler has on the guest frame that faulted. */
static CPU *g_last_cpu;

/* One-shot post-renderer initialiser, registered by the boot probe.  Running
 * Mss32's DllMain before D3D9 CreateDevice wedges DXVK on this device; the
 * game does not touch Miles until after the device exists, so the probe
 * defers it until then. */
static void (*g_after_d3d9_cb)(void *);

void hle_android_set_after_d3d9(void (*cb)(void *))
{
    g_after_d3d9_cb = cb;
}

void hle_android_after_d3d9(void *cpu)
{
    if (g_after_d3d9_cb)
        g_after_d3d9_cb(cpu);
}

/* The game's first Miles call is AIL_startup.  Running Mss32's DllMain from
 * the D3D9 callback is normally early enough, but this wrapper makes the
 * ordering explicit: initialise Miles immediately before its own startup,
 * then jump into the real lifted export.  It is also a useful trace point for
 * distinguishing "DllMain too late" from "Miles cannot open a driver". */
static uint32_t g_real_ail_startup;
static uint32_t g_ail_hook_thunk;
static int g_in_ail_startup;

static void h_ail_startup(CPU *c)
{
    if (!g_real_ail_startup) {
        c->eax = 0;
        return;
    }
    if (!g_in_ail_startup) {
        g_in_ail_startup = 1;
        if (g_after_d3d9_cb)
            g_after_d3d9_cb(c);
#if WC3_DEBUG
        fprintf(stderr, "[mss] calling real AIL_startup %08X\n",
                g_real_ail_startup);
#endif
        dispatch(c, g_real_ail_startup);
#if WC3_DEBUG
        fprintf(stderr, "[mss] AIL_startup returned eax=%08X\n", c->eax);
#endif
        g_in_ail_startup = 0;
        return;
    }
    /* Re-entered from Miles itself; just run the real export. */
    dispatch(c, g_real_ail_startup);
}

uint32_t hle_mss_hook_ail_startup(uint32_t real)
{
    if (!g_ail_hook_thunk) {
        g_real_ail_startup = real;
        HleEnt *e = reg("mss32.dll", "AIL_startup", 0, h_ail_startup, 0, 0);
        g_ail_hook_thunk = GUEST_HLE_BASE +
                           (uint32_t)(e - g_hle) * HLE_STRIDE;
    }
    return g_ail_hook_thunk;
}

/* Diagnostic wrappers for the handful of Miles exports that decide whether a
 * digital/3D driver opens.  The game calls these through its IAT, so wrapping
 * the import is enough to see the real arguments and result without changing
 * Miles.  A wrapper keeps the guest's stdcall stack: it saves esp on entry and
 * restores it before returning, then hle_call removes the arguments once. */
typedef struct {
    const char *name;
    int argc;
    uint32_t real;
    uint32_t thunk;
} MssTrace;

static MssTrace g_mss_trace[] = {
    { "AIL_open_digital_driver", 4, 0, 0 },
    { "AIL_set_redist_directory", 1, 0, 0 },
    { "AIL_open_3D_provider", 1, 0, 0 },
    { "AIL_enumerate_3D_providers", 3, 0, 0 },
    { "AIL_open_3D_listener", 1, 0, 0 },
    { "AIL_close_digital_driver", 1, 0, 0 },
    { "AIL_last_error", 0, 0, 0 },
};

#if WC3_DEBUG
/* Last dispatch targets, filled by the generated dispatcher's trace hook.
 * The HLE ring alone cannot say which guest function was entered; this can. */
static uint32_t g_disp_ring[64];
static unsigned g_disp_pos, g_disp_len;
static int g_trace_mssfast_open;

/* Temporary sound-path probes.  The dispatcher can see a function's entry but
 * not its return, so arm the caller's resume address and report eax when that
 * exact address is dispatched again. */
static uint32_t g_mss_ret_watch;
static const char *g_mss_ret_name;
#endif

/* Guest thread state is defined later; the dispatcher and the hang watchdog
 * need the current thread before that definition is visible. */
struct GThread;
static __thread struct GThread *g_self_thread;
static uint32_t hle_current_guest_id(void);

#if WC3_DEBUG && WC3_DEBUG_HLE
/* Hang watchdog: the last guest target and HLE wait state per thread.  This is
 * deliberately read-only and lock-free; it must never perturb a hung guest. */
static volatile uint32_t g_main_last_target, g_main_last_eip;
static volatile uint32_t g_main_last_esp, g_main_last_ebp;
static volatile uint32_t g_main_dispatch_count;
static volatile uint32_t g_main_wait_ra, g_main_wait_h0, g_main_wait_h1;
static volatile int g_main_wait_active;
static void watch_note_wait(uint32_t ra, uint32_t h0, uint32_t h1, int active);
static void hang_watch_start(void);
#endif

static void h_mss_trace(CPU *c)
{
    MssTrace *t = NULL;
    for (unsigned i = 0; i < sizeof g_mss_trace / sizeof g_mss_trace[0]; i++) {
        if (g_mss_trace[i].thunk == c->eip) {
            t = &g_mss_trace[i];
            break;
        }
    }
    if (!t || !t->real) {
        c->eax = 0;
        return;
    }

    uint32_t esp0 = c->esp;
#if WC3_DEBUG
    uint32_t enumerate_a0 = 0, enumerate_a1 = 0, enumerate_a2 = 0;
    if (!strcmp(t->name, "AIL_open_digital_driver")) {
        fprintf(stderr, "[mss] AIL_open_digital_driver(rate=%u bits=%u "
                        "channels=%u flags=%08X)\n",
                arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3));
    } else if (!strcmp(t->name, "AIL_set_redist_directory")) {
        fprintf(stderr, "[mss] AIL_set_redist_directory(\"%s\")\n",
                gstr(arg32(c, 0)));
    } else if (!strcmp(t->name, "AIL_open_3D_provider")) {
        uint32_t open_name = arg32(c, 0);
        fprintf(stderr, "[mss] AIL_open_3D_provider(arg=%08X \"%s\")\n",
                open_name, gstr(open_name));
        for (unsigned i = 0; i < 4; i++) {
            uint32_t slot = 0x6FCE5524u + i * 0x14u;
            uint32_t slot_name = rd32g(slot);
            uint32_t slot_val = rd32g(slot + 8u);
            fprintf(stderr, "[mss] provider_slot%u name=%08X \"%s\" "
                            "val=%08X\n",
                    i, slot_name, gstr(slot_name), slot_val);
        }
        fprintf(stderr, "[mss] provider_count_at_6fce5520=%08X\n",
                rd32g(0x6FCE5520u));
    } else if (!strcmp(t->name, "AIL_enumerate_3D_providers")) {
        enumerate_a0 = arg32(c, 0);
        enumerate_a1 = arg32(c, 1);
        enumerate_a2 = arg32(c, 2);
        fprintf(stderr, "[mss] AIL_enumerate_3D_providers(arg0=%08X "
                        "arg1=%08X arg2=%08X)\n",
                enumerate_a0, enumerate_a1, enumerate_a2);
    } else if (!strcmp(t->name, "AIL_open_3D_listener")) {
        fprintf(stderr, "[mss] AIL_open_3D_listener(0x%08X)\n",
                arg32(c, 0));
    } else if (!strcmp(t->name, "AIL_close_digital_driver")) {
        fprintf(stderr, "[mss] AIL_close_digital_driver(0x%08X)\n",
                arg32(c, 0));
    } else {
        fprintf(stderr, "[mss] %s\n", t->name);
    }
    fflush(stderr);

    if (!strcmp(t->name, "AIL_open_3D_provider"))
        g_trace_mssfast_open = 1;
#endif
    dispatch(c, t->real);
#if WC3_DEBUG
    if (!strcmp(t->name, "AIL_open_3D_provider"))
        g_trace_mssfast_open = 0;

    if (!strcmp(t->name, "AIL_last_error"))
        fprintf(stderr, "[mss] AIL_last_error -> %08X \"%s\"\n",
                c->eax, gstr(c->eax));
    else if (!strcmp(t->name, "AIL_open_3D_provider")) {
        char tail[512];
        int n = snprintf(tail, sizeof tail,
                         "[mss] AIL_open_3D_provider returned %08X tail:",
                         c->eax);
        unsigned want = g_disp_len < 24 ? g_disp_len : 24;
        for (unsigned i = g_disp_len - want; i < g_disp_len; i++) {
            unsigned p = (g_disp_pos + 64 - g_disp_len + i) % 64;
            n += snprintf(tail + n, sizeof tail - (size_t)n,
                          " %08X", g_disp_ring[p]);
        }
        fprintf(stderr, "%s\n", tail);
    } else if (!strcmp(t->name, "AIL_enumerate_3D_providers")) {
        uint32_t count = 0, name = 0, next = 0;
        if (enumerate_a0 && guest_ok(enumerate_a0, 4))
            count = rd32g(enumerate_a0);
        if (enumerate_a1 && guest_ok(enumerate_a1, 4))
            name = rd32g(enumerate_a1);
        if (enumerate_a2 && guest_ok(enumerate_a2, 4))
            next = rd32g(enumerate_a2);
        fprintf(stderr, "[mss] AIL_enumerate_3D_providers returned %08X "
                        "count=%u name=%08X \"%s\" next=%u\n",
                c->eax, count, name, gstr(name), next);
    } else
        fprintf(stderr, "[mss] %s returned %08X\n", t->name, c->eax);
    fflush(stderr);
#endif

    c->esp = esp0;   /* hle_call removes the stdcall arguments. */
}

uint32_t hle_mss_hook_trace(const char *name, uint32_t real)
{
    if (!name) return 0;
    for (unsigned i = 0; i < sizeof g_mss_trace / sizeof g_mss_trace[0]; i++) {
        MssTrace *t = &g_mss_trace[i];
        if (!strstr(name, t->name)) continue;
        if (!t->thunk) {
            t->real = real;
            HleEnt *e = reg("mss32.dll", t->name, 0, h_mss_trace,
                            t->argc, 0);
            t->thunk = GUEST_HLE_BASE +
                       (uint32_t)(e - g_hle) * HLE_STRIDE;
        }
        return t->thunk;
    }
    return 0;
}

static const char *g_exe_path = "war3.exe";
/* The same path in the form Windows would report it: "Z:\a\b\war3.exe". */
static char g_exe_path_win[512] = "war3.exe";
static char        g_exe_dir[512] = ".";
static uint32_t    g_cmdline_cell;
static uint32_t    g_cmdline_str;
static uint32_t    g_fmode_cell;
static uint32_t    g_commode_cell;
static uint32_t    g_adjust_fdiv_cell;
static int         g_stop_game_dll = 1;
static int         g_reached_game_dll;
/* Win32 keeps the last error per thread, and the engine's TLS wrapper reads it
 * immediately after TlsGetValue - a global here would let a worker thread's
 * error fail a lookup on the main thread. */
static THREAD_LOCAL uint32_t g_last_error;
static uint32_t    g_tls[64];

/* The generated dispatcher references these even in a graphics-only debug
 * build, so they cannot live behind WC3_DEBUG. */
int g_dispatch_diag;
int g_stack_guard;

#if WC3_DEBUG
/* WC3_OBJ_WATCH=1 dumps every object that the generic `IUnknown::AddRef`
 * (0x6F06C390) hands back - see the dispatcher hook at the bottom of the file.
 * Declared here because hle_android_start() arms the per-dispatch hook. */
static int g_obj_watch;

/* WC3_FN=ADDR[,ADDR...] probes guest function *entries*.  A lifted `ret`
 * restores eip from the guest stack and never re-enters dispatch, so a
 * callee's return value is invisible to the dispatcher; what is visible is
 * the next call the caller makes, and at that instant eax still holds the
 * result (a caller between `call` and `call` only runs test/branch).  Hence
 * entry probes, and hence the register dump: for a __thiscall `ecx` is
 * `this`, for a __fastcall `ecx`/`edx` are the arguments, and `[esp+4..]`
 * are the stack ones.  Cheap, generic, and no rebuild of the corpus. */
static uint32_t g_fn_probe[16];
static unsigned g_fn_probe_n;
static unsigned g_fn_hits[16];

/* WC3_TEX=1 traces the team-colour texture resolver, which is the first
 * place the guest dereferences a NULL: L_6F3535D0 builds
 * "ReplaceableTextures\TeamColor\TeamColorNN", L_6F70DD90 appends ".tga"
 * then ".blp", and L_6F70D9B0 does the actual load.  It reports success
 * through eax but the caller only reads the out-struct, so a "successful"
 * load that wrote nothing is a NULL deref one instruction later - these
 * probes separate "the open failed" from "the open worked and the size
 * came back zero". */
static int g_tex_trace;

/* The BLP branch of L_6F70D9B0 has exactly one place that can fail after the
 * file was found: L_6F8E9660 (getMip) returns whatever its decoder returned,
 * and the caller only looks at eax.  A lifted `ret` does not re-enter
 * dispatch, so the decoder's result is visible only as `eax` at the *next*
 * call the decoder's caller makes - which is 0x6F092F70 at 6F8E96A7.  These
 * two states make that one value printable without touching the corpus:
 * 1 = getMip entered, 2 = decoder entered and its result still in eax. */
static int g_tex_mip;

/* Dimensions of the BLP decode currently between getMip and its return site.
 * The decoded buffer is the last point where the guest's alpha is still one
 * plain byte per pixel; the D3D upload path can then be compared against it. */
static uint32_t g_tex_mip_this, g_tex_mip_w, g_tex_mip_h, g_tex_mip_idx;
static uint32_t g_tex_mip_comp;
static unsigned g_tex_alpha_reports;

/* Trace selected imports: WC3_A_TRACE=name[,name...] or "all". */
static const char *g_trace;
static unsigned    g_trace_budget = 20000;
/* Bounded counter for the file-I/O trace: the archive open path is a handful
 * of seeks and reads, and an unbounded trace floods a 90 s run. */
static unsigned    g_io_trace;
#endif

/* ------------------------------------------------------------------ */
/* guest memory                                                        */
/* ------------------------------------------------------------------ */

/* Every range the guest may legally touch.  Built from the layout macros in
 * host.h rather than written out again: this table used to be a hand copy, and
 * a hand copy of a moved layout is a lie that only shows up as a refused
 * pointer much later.  The Android layout is different from the Windows one -
 * see host.h and docs/ANDROID_WSI.md section 10. */
static const struct { uint32_t lo, hi; } g_arenas[] = {
    { GUEST_IMAGE_WAR3,  0x01000000u },                              /* war3.exe + room */
    { GUEST_HEAP_BASE,   GUEST_HEAP_BASE + GUEST_HEAP_SIZE },        /* guest heap (Storm) */
    { GUEST_HLE_HEAP_BASE, GUEST_HLE_HEAP_BASE + GUEST_HLE_HEAP_SIZE }, /* HLE heap */
    { GUEST_VA_BASE,     GUEST_VA_BASE + GUEST_VA_SIZE },            /* VirtualAlloc arena 1 */
    { GUEST_IMAGE_STORM, GUEST_IMAGE_STORM + 0x00100000u },          /* Storm.dll */
    { GUEST_IMAGE_MSS,   GUEST_IMAGE_MSS + 0x00100000u },            /* Mss32.dll */
    { GUEST_IMAGE_MSSFAST, GUEST_IMAGE_MSSFAST + 0x00020000u },      /* Mssfast.m3d */
    { GUEST_IMAGE_MP3DEC, GUEST_IMAGE_MP3DEC + 0x00040000u },        /* Mp3dec.asi */
    { GUEST_STACK_BASE,  GUEST_STACK_BASE + GUEST_STACK_SIZE },      /* main guest stack */
    { GUEST_THREAD_STACK_BASE, GUEST_THREAD_STACK_BASE +
      GUEST_THREAD_MAX * GUEST_THREAD_STACK_SIZE },                  /* worker stacks */
    { GUEST_VA2_BASE,    GUEST_VA2_BASE + GUEST_VA2_SIZE },          /* VirtualAlloc arena 2 */
    { GUEST_VA3_BASE,    GUEST_VA3_BASE + GUEST_VA3_SIZE },          /* VirtualAlloc arena 3 */
    { GUEST_VA4_BASE,    GUEST_VA4_BASE + GUEST_VA4_SIZE },          /* VirtualAlloc arena 4 */
    { GUEST_VA5_BASE,    GUEST_VA5_BASE + GUEST_VA5_SIZE },          /* VirtualAlloc arena 5 */
    { GUEST_VA6_BASE,    GUEST_VA6_BASE + GUEST_VA6_SIZE },          /* VirtualAlloc arena 6 */
    { GUEST_VA7_BASE,    GUEST_VA7_BASE + GUEST_VA7_SIZE },          /* VirtualAlloc arena 7 */
    { GUEST_IMAGE_IJL,   GUEST_IMAGE_IJL + 0x00100000u },            /* ijl15.dll */
    { GUEST_IMAGE_GAME,  GUEST_IMAGE_GAME + 0x01000000u },           /* game.dll */
    { GUEST_TEB_BASE,    GUEST_TEB_BASE + 0x00010000u },             /* TEB page */
    { GUEST_HLE_BASE,    GUEST_HLE_BASE + 0x00100000u },             /* HLE thunks */
};

static int guest_ok(uint32_t addr, uint32_t size)
{
    uint64_t lo = addr, hi = (uint64_t)addr + size;
    if (hi > 0x100000000ull) return 0;
    for (unsigned i = 0; i < sizeof g_arenas / sizeof g_arenas[0]; i++)
        if (lo >= g_arenas[i].lo && hi <= g_arenas[i].hi) return 1;
    /* Ranges committed after startup are guest memory too, and they are not in
     * the layout table: the D3D9 staging arena lives in the gap immediately
     * above game.dll.  Without this, every guest copy into a locked buffer was
     * refused and the caller was told it had succeeded. */
    return guest_range_ok(addr, size);
}

/* The experimental thin D3D9 COM bridge (runtime/host/hle_d3d9_thin.cpp)
 * hands guest pointers straight to native DXVK COM methods.  It must reject
 * an unmapped pointer instead of letting the host fault, and it needs the
 * same validator the CRT imports use - including the fallback for ranges
 * committed after startup (the staging arena).  Export it read-only. */
int hle_android_guest_ok(uint32_t addr, uint32_t size)
{
    return guest_ok(addr, size);
}

/* Clamp `n` so that [a, a+n) stays inside one mapped arena.  guest_ok() is
 * all-or-nothing, and a caller that passes a 64 KB capacity for a 16-byte
 * buffer would otherwise be refused outright - which reads as "the write
 * happened nowhere", not as "the capacity was wrong". */
uint32_t guest_clamp(uint32_t a, uint32_t n)
{
    for (unsigned i = 0; i < sizeof g_arenas / sizeof g_arenas[0]; i++) {
        if (a < g_arenas[i].lo || a >= g_arenas[i].hi) continue;
        uint32_t avail = g_arenas[i].hi - a;
        return n < avail ? n : avail;
    }
    if (guest_range_ok(a, 1)) return guest_range_clamp(a, n);
    return 0;
}

/* Guest memory accessors.
 *
 * Not static: the D3D9 COM layer (runtime/host/hle_d3d9.cpp, C++) is linked
 * into this image and decodes guest arguments with the same helpers, so these
 * four plus arg32 are the ABI between the two translation units.  They stay
 * `static inline`-sized wrappers over a volatile load on purpose - a fault
 * here has to be attributable to the caller's address, not to a library. */
uint32_t rd32g(uint32_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
void wr32g(uint32_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }
static uint16_t rd16g(uint32_t a) { return *(volatile uint16_t *)(uintptr_t)a; }
static void wr16g(uint32_t a, uint16_t v) { *(volatile uint16_t *)(uintptr_t)a = v; }
static uint8_t rd8g(uint32_t a) { return *(volatile uint8_t *)(uintptr_t)a; }
static void wr8g(uint32_t a, uint8_t v) { *(volatile uint8_t *)(uintptr_t)a = v; }

uint32_t arg32(CPU *c, unsigned n) { return rd32g(c->esp + 4 + n * 4); }

/* Guest C string.  Returns "" for an unmapped pointer so a bad argument
 * produces a report instead of a host fault inside the HLE. */
static const char *gstr(uint32_t a)
{
    if (!a || !guest_ok(a, 1)) return "";
    return (const char *)(uintptr_t)a;
}

/* ------------------------------------------------------------------ */
/* guest heap: first-fit free list inside the HLE arena                */
/* ------------------------------------------------------------------ */

#define HLE_HEAP_BASE GUEST_HLE_HEAP_BASE
#define HLE_HEAP_SIZE GUEST_HLE_HEAP_SIZE

typedef struct { uint32_t size; uint32_t next; } GBlk;   /* size = payload */
static uint32_t g_heap_free;      /* head of free list (block header addr) */
static int      g_heap_ready;
static uint32_t g_heap_used, g_heap_peak;

/* The guest runs real worker threads (Storm's read-ahead, the engine's own),
 * and they allocate: the first run that reached Direct3D had a worker inside
 * free() walking a free list the main thread was concurrently splitting, and
 * died on a null dereference.  One lock around the whole allocator. */
static pthread_mutex_t g_heap_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t align8(uint32_t v) { return (v + 7u) & ~7u; }

static void heap_init(void)
{
    if (g_heap_ready) return;
    if (!guest_map(HLE_HEAP_BASE, HLE_HEAP_SIZE)) {
        fprintf(stderr, "[hle] cannot map the HLE heap\n");
        abort();
    }
    GBlk *b = (GBlk *)(uintptr_t)HLE_HEAP_BASE;
    b->size = HLE_HEAP_SIZE - sizeof(GBlk);
    b->next = 0;
    g_heap_free = HLE_HEAP_BASE;
    g_heap_ready = 1;
}

uint32_t hle_guest_alloc(uint32_t size)
{
    if (!g_heap_ready) heap_init();
    if (size == 0) size = 1;
    size = align8(size);
    pthread_mutex_lock(&g_heap_lock);
    uint32_t prev = 0;
    for (uint32_t cur = g_heap_free; cur; ) {
        GBlk *b = (GBlk *)(uintptr_t)cur;
        if (b->size >= size) {
            uint32_t rest = b->size - size;
            if (rest >= sizeof(GBlk) + 32u) {
                /* split */
                GBlk *nb = (GBlk *)(uintptr_t)(cur + sizeof(GBlk) + size);
                nb->size = rest - sizeof(GBlk);
                nb->next = b->next;
                b->size = size;
                if (prev) ((GBlk *)(uintptr_t)prev)->next = cur + sizeof(GBlk) + size;
                else g_heap_free = cur + sizeof(GBlk) + size;
            } else {
                if (prev) ((GBlk *)(uintptr_t)prev)->next = b->next;
                else g_heap_free = b->next;
            }
            g_heap_used += b->size;
            if (g_heap_used > g_heap_peak) g_heap_peak = g_heap_used;
            pthread_mutex_unlock(&g_heap_lock);
            return cur + sizeof(GBlk);
        }
        prev = cur;
        cur = b->next;
    }
    pthread_mutex_unlock(&g_heap_lock);
    fprintf(stderr, "[hle] guest heap exhausted (%u bytes live)\n", g_heap_used);
    return 0;
}

void hle_guest_free(uint32_t addr)
{
    if (!addr) return;
    pthread_mutex_lock(&g_heap_lock);
    uint32_t hdr = addr - sizeof(GBlk);
    if (!guest_ok(hdr, sizeof(GBlk))) { pthread_mutex_unlock(&g_heap_lock); return; }
    GBlk *b = (GBlk *)(uintptr_t)hdr;
    if (b->size > HLE_HEAP_SIZE) { pthread_mutex_unlock(&g_heap_lock); return; }
    g_heap_used -= b->size;
    /* insert sorted and coalesce with both neighbours */
    uint32_t prev = 0, cur = g_heap_free;
    while (cur && cur < hdr) { prev = cur; cur = ((GBlk *)(uintptr_t)cur)->next; }
    b->next = cur;
    if (prev) ((GBlk *)(uintptr_t)prev)->next = hdr;
    else g_heap_free = hdr;
    if (cur && hdr + sizeof(GBlk) + b->size == cur) {
        GBlk *nb = (GBlk *)(uintptr_t)cur;
        b->size += sizeof(GBlk) + nb->size;
        b->next = nb->next;
    }
    if (prev) {
        GBlk *pb = (GBlk *)(uintptr_t)prev;
        if (prev + sizeof(GBlk) + pb->size == hdr) {
            pb->size += sizeof(GBlk) + b->size;
            pb->next = b->next;
        }
    }
    pthread_mutex_unlock(&g_heap_lock);
}

static uint32_t hle_alloc0(uint32_t n)
{
    uint32_t p = hle_guest_alloc(n);
    if (p) memset((void *)(uintptr_t)p, 0, n);
    return p;
}

uint32_t hle_guest_strdup(const char *s)
{
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    uint32_t p = hle_guest_alloc((uint32_t)n);
    if (p) memcpy((void *)(uintptr_t)p, s, n);
    return p;
}

uint32_t hle_guest_strdup_w(const uint16_t *s)
{
    if (!s) s = (const uint16_t *)L"";
    size_t n = 0;
    while (s[n]) n++;
    uint32_t p = hle_guest_alloc((uint32_t)((n + 1) * 2));
    if (p) memcpy((void *)(uintptr_t)p, s, (n + 1) * 2);
    return p;
}

/* ------------------------------------------------------------------ */
/* VirtualAlloc arena                                                  */
/* ------------------------------------------------------------------ */

static uint32_t g_va_next = GUEST_VA_BASE;
static uint32_t g_va2_next = GUEST_VA2_BASE;
static uint32_t g_va3_next = GUEST_VA3_BASE;
static uint32_t g_va4_next = GUEST_VA4_BASE;
static uint32_t g_va5_next = GUEST_VA5_BASE;
static uint32_t g_va6_next = GUEST_VA6_BASE;
static uint32_t g_va7_next = GUEST_VA7_BASE;
#if WC3_DEBUG
/* WC3_VA_TRACE=1: every reserve and release.  The interesting failure is a
 * guest write into an arena page that is no longer mapped, and the only way
 * to tell "we freed it while the guest still owned it" from "we never mapped
 * it" is the sequence of the two calls. */
static int g_va_trace;
#endif

/* A bump pointer alone is not enough.  WC3 reserves and releases 64 KB blocks
 * continuously while it loads a map, and a monotonic pointer exhausts the
 * 64 MB first arena within a minute - after which the second arena walks into
 * whatever is already mapped there and mmap fails with EEXIST.  The guest then
 * gets a NULL it treats as fatal, which is exactly the "error 4096, Object:
 * .?AVCGhostImage@@" report.  So released ranges go on a first-fit list and
 * are handed out again before the pointer moves. */
#define VA_FREE_MAX  512
#define VA_LIVE_MAX 4096
/* Every size in these tables must be the size that was actually *mapped*.
 * mmap() rounds a request up to a page, so rounding the bookkeeping to 64 KB
 * while mapping 0x10 bytes made a later MEM_RELEASE munmap() 64 KB: it took
 * the neighbours with it, and those neighbours were live blocks.  The guest
 * then wrote into pages the table still called "live" - the one crash shape
 * the report could not explain.  VA_PAGE is the only rounding that is safe. */
#define VA_PAGE 0x1000u
static uint32_t va_round(uint32_t n)
{
    return (n + VA_PAGE - 1u) & ~(VA_PAGE - 1u);
}
static struct { uint32_t addr, size; } g_va_free[VA_FREE_MAX];
static unsigned g_va_free_n;
static struct { uint32_t addr, size; } g_va_live[VA_LIVE_MAX];
static unsigned g_va_live_n;

static void va_live_add(uint32_t addr, uint32_t size)
{
    for (unsigned i = 0; i < g_va_live_n; i++)
        if (!g_va_live[i].addr) { g_va_live[i].addr = addr; g_va_live[i].size = size; return; }
    if (g_va_live_n < VA_LIVE_MAX) {
        g_va_live[g_va_live_n].addr = addr;
        g_va_live[g_va_live_n].size = size;
        g_va_live_n++;
    }
}

/* Does [addr, addr+size) touch a block we already own?  Two live entries for
 * one range is worse than a leak: the guest releases it once, the first entry
 * is taken, the range is unmapped, and the second entry goes on claiming
 * memory that is gone.  The crash report then says "live" about a page that
 * faults, which is the one contradiction that sends triage the wrong way.
 *
 * The arithmetic is 64-bit on purpose: a 32-bit `addr + size` wraps, and a
 * reservation that ends past 4 GB then looks like it fits.  That is exactly
 * how run 12 reserved 256 MB at 0xFBAD7000 and let the guest walk off the top
 * of its own address space. */
static int va_live_overlaps(uint32_t addr, uint32_t size)
{
    if (!size) return 0;
    uint64_t lo = addr, hi = (uint64_t)addr + size;
    for (unsigned i = 0; i < g_va_live_n; i++) {
        if (!g_va_live[i].addr || !g_va_live[i].size) continue;
        uint64_t elo = g_va_live[i].addr;
        uint64_t ehi = elo + g_va_live[i].size;
        if (lo < ehi && elo < hi)
            return 1;
    }
    return 0;
}

/* Is the whole range inside one live VirtualAlloc block?  A MEM_COMMIT is
 * only legal on pages that were reserved first, so this is the question
 * h_virtual_alloc has to ask instead of returning success unconditionally. */
static int va_live_covers(uint32_t addr, uint32_t size)
{
    uint64_t lo = addr, hi = (uint64_t)addr + size;
    if (!size || hi > 0x100000000ull) return 0;
    for (unsigned i = 0; i < g_va_live_n; i++) {
        if (!g_va_live[i].addr || !g_va_live[i].size) continue;
        uint64_t elo = g_va_live[i].addr;
        uint64_t ehi = elo + g_va_live[i].size;
        if (lo >= elo && hi <= ehi) return 1;
    }
    return 0;
}

/* Does the range fall inside one of the bump arenas?  A MEM_COMMIT there is
 * only legal after a reservation recorded in g_va_live; without that check the
 * guest can "commit" pages that were never reserved and then use them. */
static int va_arena_range(uint32_t addr, uint32_t size)
{
    static const struct { uint32_t base, size; } a[] = {
        { GUEST_VA_BASE,  GUEST_VA_SIZE  },
        { GUEST_VA2_BASE, GUEST_VA2_SIZE },
        { GUEST_VA3_BASE, GUEST_VA3_SIZE },
        { GUEST_VA4_BASE, GUEST_VA4_SIZE },
        { GUEST_VA5_BASE, GUEST_VA5_SIZE },
        { GUEST_VA6_BASE, GUEST_VA6_SIZE },
        { GUEST_VA7_BASE, GUEST_VA7_SIZE },
    };
    uint64_t lo = addr, hi = (uint64_t)addr + size;
    for (unsigned i = 0; i < sizeof a / sizeof a[0]; i++) {
        uint64_t elo = a[i].base, ehi = elo + a[i].size;
        if (lo < ehi && elo < hi) return 1;
    }
    return 0;
}

/* Pages that are mapped for the lifetime of the process and are not part of
 * the VirtualAlloc arena.  Committing those is a no-op success. */
static int va_fixed_mapped(uint32_t addr, uint32_t size)
{
    return guest_ok(addr, size) && !va_arena_range(addr, size);
}

/* Win32 requires dwSize == 0 for MEM_RELEASE, so the size has to come from the
 * table rather than from the call. */
static uint32_t va_live_take(uint32_t addr)
{
    for (unsigned i = 0; i < g_va_live_n; i++) {
        if (g_va_live[i].addr != addr) continue;
        uint32_t s = g_va_live[i].size;
        g_va_live[i].addr = 0;
        return s;
    }
    return 0;
}

static void va_release(uint32_t addr, uint32_t size)
{
    if (!addr || !size) return;
    /* Round the end, not the size, in 64 bits: rounding 0xFFFFFFF0 up to
     * 0x100000000 wraps to zero and would put a bogus entry on the free list. */
    uint64_t end = ((uint64_t)addr + size + VA_PAGE - 1u) & ~(uint64_t)(VA_PAGE - 1u);
    if (end > 0x100000000ull) {
        fprintf(stderr, "[va] release %08X+%X crosses 4 GB, ignored\n", addr, size);
        return;
    }
    size = (uint32_t)(end - addr);
#if WC3_DEBUG
    if (g_va_trace)
        fprintf(stderr, "[va] free %08X+%X (list=%u)\n", addr, size, g_va_free_n);
#endif
    /* Releasing a range that a live block covers would put memory on the free
     * list that the guest still owns.  With page-exact sizes this cannot
     * happen; keeping the check means a future size bug shows up as a
     * dropped release instead of as corruption. */
    if (va_live_overlaps(addr, size)) {
        fprintf(stderr, "[va] release %08X+%X overlaps a live block, ignored\n",
                addr, size);
        return;
    }
    /* Coalesce with a neighbour: a map unload releases a run of adjacent
     * blocks and handing them back one at a time fragments the list. */
    for (unsigned i = 0; i < g_va_free_n; i++) {
        if (g_va_free[i].addr == addr && g_va_free[i].size == size) return;  /* duplicate */
        if ((uint64_t)g_va_free[i].addr + g_va_free[i].size == addr) { g_va_free[i].size += size; return; }
        if ((uint64_t)addr + size == g_va_free[i].addr) { g_va_free[i].addr = addr; g_va_free[i].size += size; return; }
    }
    if (g_va_free_n < VA_FREE_MAX) {
        g_va_free[g_va_free_n].addr = addr;
        g_va_free[g_va_free_n].size = size;
        g_va_free_n++;
    }
    /* A full list is not an error: the block stays unmapped and the bump
     * pointer simply keeps moving. */
}

/* "Is this arena address supposed to be mapped?" - the one question a
 * use-after-free in the guest and a bookkeeping bug on our side answer
 * differently, and the answer has to be in the crash report. */
void va_describe(uint32_t a, char *out, size_t cap)
{
    for (unsigned i = 0; i < g_va_live_n; i++)
        if (g_va_live[i].addr &&
            (uint64_t)a >= g_va_live[i].addr &&
            (uint64_t)a < (uint64_t)g_va_live[i].addr + g_va_live[i].size) {
            snprintf(out, cap, "live %08X+%X", g_va_live[i].addr, g_va_live[i].size);
            return;
        }
    for (unsigned i = 0; i < g_va_free_n; i++)
        if ((uint64_t)a >= g_va_free[i].addr &&
            (uint64_t)a < (uint64_t)g_va_free[i].addr + g_va_free[i].size) {
            snprintf(out, cap, "FREED %08X+%X", g_va_free[i].addr, g_va_free[i].size);
            return;
        }
    snprintf(out, cap, "untracked");
}

static uint32_t va_take(uint32_t size)
{
    for (unsigned i = 0; i < g_va_free_n; i++) {
        if (g_va_free[i].size < size) continue;
        uint32_t p = g_va_free[i].addr;
        /* A free entry that a live block still covers is stale bookkeeping.
         * guest_commit() below would succeed on it (mprotect cannot tell) and
         * hand out memory the guest already owns. */
        if (va_live_overlaps(p, size)) {
#if WC3_DEBUG
            if (g_va_trace) fprintf(stderr, "[va] stale free %08X+%X skipped\n", p, size);
#endif
            continue;
        }
        /* The range may still be mapped (a release that only decommitted), so
         * a failed map is not a reason to skip the block - a failed mprotect
         * is. */
        if (!guest_map(p, size)) {
            /* A free entry is always unmapped, because h_virtual_free()
             * munmaps exactly what the live entry recorded.  So EEXIST here
             * means the tables disagree; taking the block would hand out
             * memory that is already in use.  Skip it instead. */
#if WC3_DEBUG
            if (g_va_trace) fprintf(stderr, "[va] free %08X+%X still mapped, skipped\n", p, size);
#endif
            continue;
        }
#if WC3_DEBUG
        if (g_va_trace) fprintf(stderr, "[va] reuse %08X+%X\n", p, size);
#endif
        g_va_free[i].addr += size;
        g_va_free[i].size -= size;
        if (!g_va_free[i].size) g_va_free[i] = g_va_free[--g_va_free_n];
        return p;
    }
    return 0;
}

static uint32_t va_alloc(uint32_t size)
{
    /* Round in 64 bits: a 32-bit `size + 0xFFFF` wraps to a tiny value for a
     * request near 4 GB, and the allocator then hands out far less than the
     * guest asked for. */
    uint64_t rounded = ((uint64_t)size + 0xFFFFu) & ~0xFFFFull;
    if (!size) rounded = 0x1000;
    if (rounded > 0xFFFFFFFFull) return 0;
    size = (uint32_t)rounded;

    uint32_t p = va_take(size);
    if (p) { va_live_add(p, size); return p; }

    /* Every bound is 64-bit.  With 32-bit arithmetic `next + size` wrapped,
     * so an arena whose bump pointer sat near 4 GB accepted a 256 MB reserve
     * that ran past the end of the address space.  The guest then received
     * addresses that only exist after the wrap and faulted on the first
     * write (run 12: reserve at 0xFBAD7000, bucket array at 0xFFFFFF80). */
    static struct {
        uint32_t *next, base, size;
        const char *name;
    } arenas[] = {
        { &g_va_next,  GUEST_VA_BASE,  GUEST_VA_SIZE,  "bump"  },
        { &g_va2_next, GUEST_VA2_BASE, GUEST_VA2_SIZE, "bump2" },
        { &g_va3_next, GUEST_VA3_BASE, GUEST_VA3_SIZE, "bump3" },
        { &g_va4_next, GUEST_VA4_BASE, GUEST_VA4_SIZE, "bump4" },
        { &g_va5_next, GUEST_VA5_BASE, GUEST_VA5_SIZE, "bump5" },
        { &g_va6_next, GUEST_VA6_BASE, GUEST_VA6_SIZE, "bump6" },
        { &g_va7_next, GUEST_VA7_BASE, GUEST_VA7_SIZE, "bump7" },
    };

    /* Something else may own the range the bump pointer is sitting on.  Skip
     * forward instead of failing the allocation: a NULL here is fatal to the
     * guest, and the conflicting mapping is usually a few pages wide. */
    for (unsigned attempt = 0; attempt < 64; attempt++) {
        int skipped = 0;
        for (unsigned ai = 0; ai < sizeof arenas / sizeof arenas[0]; ai++) {
            uint32_t q = *arenas[ai].next;
            if ((uint64_t)q + size >
                (uint64_t)arenas[ai].base + arenas[ai].size)
                continue;
            *arenas[ai].next += size;
            if (guest_map(q, size)) {
#if WC3_DEBUG
                if (g_va_trace)
                    fprintf(stderr, "[va] %s %08X+%X\n", arenas[ai].name, q, size);
#endif
                va_live_add(q, size);
                return q;
            }
#if WC3_DEBUG
            if (g_va_trace)
                fprintf(stderr, "[va] %s %08X+%X BUSY\n", arenas[ai].name, q, size);
#endif
            skipped = 1;
        }
        if (!skipped) break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* handles                                                             */
/* ------------------------------------------------------------------ */

#define GH_MAX 512
#define GH_BASE 0x00000100u
#define GH_INVALID 0xFFFFFFFFu

/* Events, mutexes and semaphores are handed out from their own range, because
 * the launcher's CRT creates one per lock object and a fixed table of 64 made
 * CreateEventA start returning GH_INVALID after the first ~60 objects - the
 * guest then retried 2000 times and the boot stalled.
 *
 * The range is split in three: real mutexes get their own range, stateless
 * objects (semaphores, thread handles) come next, real events last.  They must
 * not overlap, or a mutex handle would resolve to somebody's event. */
#define MUTEX_BASE 0x00080000u
#define MUTEX_END  0x00088000u
#define SYNC_BASE  0x00088000u
#define SYNC_MID   0x00090000u
#define SYNC_END   0x000A0000u
static uint32_t g_sync_next;

static uint32_t gh_new_sync(void)
{
    uint32_t n = (SYNC_MID - SYNC_BASE) / 4u;
    return SYNC_BASE + (g_sync_next++ % n) * 4u;
}

typedef struct {
    int  used;
    int  kind;          /* 0 free, 1 file, 2 dir, 3 event/mutex, 4 map */
    int  fd;
    DIR *dir;
    char pattern[256];
    char dirpath[512];
    uint32_t state;
    uint32_t file_map_base;
    uint32_t file_map_size;
    uint32_t iocp_port;      /* CreateIoCompletionPort association */
    uint32_t iocp_key;
    int      save_trace;     /* opened under Save/ or a .w3z/.w3v path */
    int      overlapped;     /* FILE_FLAG_OVERLAPPED: use OVERLAPPED offsets */
} GHandle;

static GHandle g_handles[GH_MAX];
static GHandle g_sync_dummy;

static uint32_t gh_new(int kind)
{
    for (int i = 0; i < GH_MAX; i++) {
        if (g_handles[i].used) continue;
        memset(&g_handles[i], 0, sizeof g_handles[i]);
        g_handles[i].used = 1;
        g_handles[i].kind = kind;
        g_handles[i].fd = -1;
        return GH_BASE + (uint32_t)i * 4u;
    }
    return GH_INVALID;
}

static GHandle *gh_get(uint32_t h)
{
    if (h >= SYNC_BASE && h < SYNC_END) {
        /* A stateless sync object: re-arm the dummy on every lookup so that a
         * CloseHandle() memset cannot leave it looking free. */
        memset(&g_sync_dummy, 0, sizeof g_sync_dummy);
        g_sync_dummy.used = 1;
        g_sync_dummy.kind = 3;
        g_sync_dummy.fd = -1;
        return &g_sync_dummy;
    }
    if (h < GH_BASE) return NULL;
    uint32_t i = (h - GH_BASE) / 4u;
    if (i >= GH_MAX || !g_handles[i].used) return NULL;
    return &g_handles[i];
}

/* ------------------------------------------------------------------ */
/* path translation                                                    */
/* ------------------------------------------------------------------ */

static void win_path(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    if (!in) { snprintf(out, cap, "."); return; }
    /* "Z:\a\b" -> "/a/b" (Z: is the fake drive for the POSIX root, so the
     * absolute path the game builds from GetModuleFileNameA still resolves);
     * "C:\foo" -> "foo"; "C:" -> "."; "\\server" -> "/server" */
    if (isalpha((unsigned char)in[0]) && in[1] == ':') {
        if ((in[0] == 'Z' || in[0] == 'z') && (in[2] == '\\' || in[2] == '/'))
            out[o++] = '/';
        in += 2;
    }
    while (*in == '\\' || *in == '/') {
        /* a single leading slash is kept, a UNC prefix collapses */
        if (o == 0) { if (o + 1 < cap) out[o++] = '/'; }
        in++;
    }
    for (; *in && o + 1 < cap; in++) {
        char ch = *in;
        if (ch == '\\') ch = '/';
        out[o++] = ch;
    }
    out[o] = 0;
    if (o == 0) snprintf(out, cap, ".");
}

/* The inverse: "/data/x/war3.exe" -> "Z:\data\x\war3.exe".  Anything that hands
 * the guest an *absolute* path has to use this form.  The launcher takes the
 * directory of its own image by cutting at the last backslash, so a POSIX
 * string survives that cut whole and becomes "вЂ¦/war3.exe/War3.mpq". */
static void posix_to_win(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    if (!in) { snprintf(out, cap, "."); return; }
    if (in[0] == '/') { if (o + 2 < cap) { out[o++] = 'Z'; out[o++] = ':'; } }
    for (; *in && o + 1 < cap; in++)
        out[o++] = (*in == '/') ? '\\' : *in;
    out[o] = 0;
    if (o == 0) snprintf(out, cap, ".");
}

static uint32_t filetime_now(void){
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull +
                 (uint64_t)(ts.tv_nsec / 100);
    return (uint32_t)t;   /* low dword; callers write both halves */
}

static void wr_filetime(uint32_t dst, uint64_t t)
{
    wr32g(dst, (uint32_t)t);
    wr32g(dst + 4, (uint32_t)(t >> 32));
}

static uint64_t filetime_from_time_t(time_t t)
{
    return ((uint64_t)t + 11644473600ull) * 10000000ull;
}

static uint32_t st_attrs(const struct stat *st)
{
    if (S_ISDIR(st->st_mode)) return 0x10u;
    uint32_t a = 0x80u;
    if (!(st->st_mode & S_IWUSR)) a |= 0x01u;
    return a;
}

static void fill_find_data(uint32_t dst, const char *name, const struct stat *st)
{
    memset((void *)(uintptr_t)dst, 0, 318);
    wr32g(dst + 0, st ? st_attrs(st) : 0x80u);
    uint64_t ft = st ? filetime_from_time_t(st->st_mtime) : 0;
    wr_filetime(dst + 4, ft);
    wr_filetime(dst + 12, ft);
    wr_filetime(dst + 20, ft);
    uint64_t size = st ? (uint64_t)st->st_size : 0;
    wr32g(dst + 28, (uint32_t)(size >> 32));
    wr32g(dst + 32, (uint32_t)size);
    size_t n = strlen(name);
    if (n > 259) n = 259;
    memcpy((void *)(uintptr_t)(dst + 44), name, n);
    /* 8.3 alias: first six chars + '~' + extension, as the shell reports. */
    char alt[14]; memset(alt, 0, sizeof alt);
    size_t k = 0;
    for (size_t i = 0; i < n && k < 6 && name[i] != '.'; i++) alt[k++] = name[i];
    alt[k++] = '~'; alt[k++] = '1';
    const char *dot = strrchr(name, '.');
    if (dot && dot[1]) {
        alt[k++] = '.';
        for (size_t i = 1; dot[i] && k < 13; i++) alt[k++] = dot[i];
    }
    memcpy((void *)(uintptr_t)(dst + 304), alt, 13);
}

/* ------------------------------------------------------------------ */
/* misc helpers                                                        */
/* ------------------------------------------------------------------ */

static uint32_t g_fake_next = 0x00001000u;

static void report_begin(void);
static void report_end(void);
static int  g_in_report;

static void note_missing(const HleEnt *e)
{
    char buf[104];
    if (e->name[0]) snprintf(buf, sizeof buf, "%s!%s", e->dll, e->name);
    else            snprintf(buf, sizeof buf, "%s!#%u", e->dll, (unsigned)e->ord);
    for (int i = 0; i < g_miss_count; i++)
        if (!strcmp(g_miss[i], buf)) { g_miss_calls[i]++; return; }
    if (g_miss_count < MISS_MAX) {
        snprintf(g_miss[g_miss_count], sizeof g_miss[0], "%s", buf);
        g_miss_calls[g_miss_count] = 1;
        g_miss_count++;
    }
}

static void hle_stub(CPU *c)
{
    (void)c;
}

/* ------------------------------------------------------------------ */
/* KERNEL32                                                            */
/* ------------------------------------------------------------------ */

static void h_get_version(CPU *c) { c->eax = 0x0A280105u; } /* 5.1 build 2600, Win10 major/minor ignored */
static void h_is_processor_feature(CPU *c)
{
    /* PF_FLOATING_POINT_PRECISION_ERRATA == 0.  A CRT that is told "yes"
     * takes the slow path through every float comparison. */
    c->eax = arg32(c, 0) == 0 ? 0 : 1;
}
static void h_get_last_error(CPU *c) { c->eax = g_last_error; }
static void h_set_last_error(CPU *c) { g_last_error = arg32(c, 0); }
static void h_identity(CPU *c) { c->eax = arg32(c, 0); }
static void h_zero(CPU *c) { c->eax = 0; }
static void h_one(CPU *c) { c->eax = 1; }
static void h_noop(CPU *c) { (void)c; }

/* _calloc_crt(num, size): the CRT allocates its onexit table and the
 * __argv/__envp block through this.  Returning 0 leaves those tables null and
 * the CRT walks a null pointer later, so it must really allocate. */
static void h_calloc_crt(CPU *c)
{
    uint32_t n = arg32(c, 0), size = arg32(c, 1);
    uint32_t total = n * size;
    c->eax = hle_alloc0(total ? total : 1);
}

/* GetVersionExA.  dwOSVersionInfoSize is a promise about the caller's buffer,
 * and the caller means it: L_6F093120 does `sub esp, 0xA0` and puts an
 * OSVERSIONINFOEXA (0x9C) at [ebp-0xA0], so the stack cookie sits at [ebp-4],
 * exactly 156 bytes in.  The old stub always memset 276 bytes - the wide
 * OSVERSIONINFOEX layout - which ran 120 bytes past the buffer, through the
 * cookie, and every exit path of that function calls the cookie check
 * (0x6F78E855) first: `[lift] UNIMPLEMENTED 6F78F45A int 0x29` was
 * __report_gsfailure, not a feature probe.  Write exactly what was asked for. */
static void h_get_version_ex(CPU *c)
{
    uint32_t p = arg32(c, 0);
    if (!p || !guest_ok(p, 4)) { c->eax = 0; return; }
    uint32_t size = rd32g(p);
    if (size < 0x94u || size > 0x11Cu || !guest_ok(p, size)) { c->eax = 0; return; }
    memset((void *)(uintptr_t)p, 0, size);
    wr32g(p + 0, size);
    wr32g(p + 4, 5);             /* dwMajorVersion: XP   */
    wr32g(p + 8, 1);             /* dwMinorVersion       */
    wr32g(p + 12, 2600);         /* dwBuildNumber        */
    wr32g(p + 16, 2);            /* VER_PLATFORM_WIN32_NT; szCSDVersion stays "" */
    if (size >= 0x9Cu) {         /* OSVERSIONINFOEXA only */
        wr16g(p + 148, 0);       /* wServicePackMajor */
        wr16g(p + 150, 0);       /* wServicePackMinor */
        wr16g(p + 152, 0);       /* wSuiteMask        */
        wr8g(p + 154, 1);        /* wProductType = VER_NT_WORKSTATION */
        wr8g(p + 155, 0);        /* wReserved         */
    }
    c->eax = 1;
}

static void h_get_system_info(CPU *c)
{
    uint32_t p = arg32(c, 0);
    if (p && guest_ok(p, 36)) {
        memset((void *)(uintptr_t)p, 0, 36);
        wr16g(p + 0, 0);                 /* wProcessorArchitecture = x86 */
        wr32g(p + 4, 0x1000);            /* dwPageSize */
        wr32g(p + 8, 0x00400000);        /* lpMinimumApplicationAddress */
        wr32g(p + 12, 0x7FFEFFFF);       /* lpMaximumApplicationAddress */
        wr32g(p + 16, 3);                /* dwActiveProcessorMask */
        wr32g(p + 20, 4);                /* dwNumberOfProcessors */
        wr32g(p + 24, 1);                /* dwProcessorType = 586 */
        wr32g(p + 28, 0x10000);          /* dwAllocationGranularity */
        wr16g(p + 32, 6);                /* wProcessorLevel */
        wr16g(p + 34, 0x3A09);           /* wProcessorRevision */
    }
    c->eax = 0;
}

static void h_global_memory_status(CPU *c)
{
    uint32_t p = arg32(c, 0);
    if (p && guest_ok(p, 32)) {
        memset((void *)(uintptr_t)p, 0, 32);
        wr32g(p + 0, 2048);          /* dwLength */
        wr32g(p + 4, 55);            /* dwMemoryLoad */
        wr32g(p + 8, 0x7FFFFFFF);    /* total phys */
        wr32g(p + 12, 0x3FFFFFFF);   /* avail phys */
        wr32g(p + 16, 0xFFFFFFFF);   /* total page file */
        wr32g(p + 20, 0x7FFFFFFF);   /* avail page file */
        wr32g(p + 24, 0x7FFFFFFF);   /* total virtual */
        wr32g(p + 28, 0x3FFFFFFF);   /* avail virtual */
    }
    c->eax = 0;
}

static void h_get_startup_info(CPU *c)
{
    uint32_t p = arg32(c, 0);
    if (p && guest_ok(p, 68)) memset((void *)(uintptr_t)p, 0, 68);
    c->eax = 0;
}

/* GetCommandLineA returns the string itself.  g_cmdline_cell is only the
 * one-word _acmdln data import and must not be handed to callers: Storm's
 * option parser reads this return value and otherwise sees a pointer's bytes
 * instead of "-classic"/"-loadfile". */
static void h_get_command_line(CPU *c) { c->eax = g_cmdline_str ? g_cmdline_str : g_cmdline_cell; }

static void h_get_module_file_name(CPU *c)
{
    uint32_t hmod = arg32(c, 0), buf = arg32(c, 1), n = arg32(c, 2);
    /* Windows form on purpose: the game takes the directory by cutting at the
     * last backslash, and a POSIX path has none - it then treats war3.exe as a
     * directory and looks for War3.mpq inside it.
     *
     * Miles calls this with its own hInstance while its DllMain runs.  The
     * returned directory is cached in MSS+0x51610 and later used to scan
     * redist\miles\*.m3d.  Returning an empty string for every non-war3
     * module made that scan start at the filesystem root and miss the
     * provider. */
    const char *path = g_exe_path_win;
    if (hmod && hmod != GUEST_IMAGE_WAR3) {
        const char *module_path = mod_windows_path(hmod);
        path = module_path ? module_path : "";
    }
    if (!buf || !n || !guest_ok(buf, n)) { c->eax = 0; return; }
    size_t len = strlen(path);
    if (len >= n) len = n - 1;
    memcpy((void *)(uintptr_t)buf, path, len);
    ((char *)(uintptr_t)buf)[len] = 0;
    c->eax = (uint32_t)len;
}

static void h_get_windows_directory(CPU *c)
{
    uint32_t buf = arg32(c, 0), n = arg32(c, 1);
    const char *d = "C:\\WINDOWS";
    size_t len = strlen(d);
    if (!buf || !n || !guest_ok(buf, n)) { c->eax = 0; return; }
    if (len >= n) len = n - 1;
    memcpy((void *)(uintptr_t)buf, d, len + 1);
    c->eax = (uint32_t)len;
}

static void h_get_system_directory(CPU *c)
{
    uint32_t buf = arg32(c, 0), n = arg32(c, 1);
    const char *d = "C:\\WINDOWS\\SYSTEM32";
    size_t len = strlen(d);
    if (!buf || !n || !guest_ok(buf, n)) { c->eax = 0; return; }
    if (len >= n) len = n - 1;
    memcpy((void *)(uintptr_t)buf, d, len);
    ((char *)(uintptr_t)buf)[len] = 0;
    c->eax = (uint32_t)len;
}

static void h_get_current_directory(CPU *c)
{
    uint32_t n = arg32(c, 0), buf = arg32(c, 1);
    char host[512];
    if (!getcwd(host, sizeof host)) host[0] = 0;
    /* Report it with the Z: drive that win_path() maps back to the POSIX
     * root.  A C: path would be stripped as relative and then resolved
     * against the work directory a second time. */
    char win[600];
    posix_to_win(host, win, sizeof win);
    size_t len = strlen(win);
    if (!buf || !n || !guest_ok(buf, n)) { c->eax = (uint32_t)len + 1; return; }
    if (len >= n) { c->eax = (uint32_t)len + 1; return; }
    memcpy((void *)(uintptr_t)buf, win, len + 1);
    c->eax = (uint32_t)len;
}

static void h_set_current_directory(CPU *c)
{
    char host[512];
    win_path(gstr(arg32(c, 0)), host, sizeof host);
    int rc = chdir(host);
    c->eax = rc == 0 ? 1u : 0u;
}

static void h_get_full_path_name(CPU *c)
{
    uint32_t name = arg32(c, 0), n = arg32(c, 1), out = arg32(c, 2), last = arg32(c, 3);
    char host[512], full[1024];
    win_path(gstr(name), host, sizeof host);
    if (host[0] == '/') snprintf(full, sizeof full, "%s", host);
    else {
        char cwd[512];
        if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
        snprintf(full, sizeof full, "%s/%s", cwd, host);
    }
    char win[1100];
    posix_to_win(full, win, sizeof win);
    size_t len = strlen(win);
    if (last && guest_ok(last, 4)) wr32g(last, out + (uint32_t)len);
    if (!out || !n || !guest_ok(out, n)) { c->eax = (uint32_t)len; return; }
    size_t copy = len < n - 1 ? len : n - 1;
    memcpy((void *)(uintptr_t)out, win, copy);
    ((char *)(uintptr_t)out)[copy] = 0;
    c->eax = (uint32_t)copy;
}

/* Windows paths are case-insensitive, Android's are not.  The launcher asks for
 * "War3.mpq" while the pushed file is "war3.mpq", so every open and stat has to
 * resolve the real spelling first.  Only the leaf is matched: a recursive
 * per-component search would cost a directory scan per level for no benefit
 * here, since the game's data files all sit in the working directory. */
static int ci_resolve(const char *path, char *out, size_t cap)
{
    if (access(path, F_OK) == 0) { snprintf(out, cap, "%s", path); return 0; }

    const char *slash = strrchr(path, '/');
    const char *leaf = slash ? slash + 1 : path;
    if (!*leaf) { snprintf(out, cap, "%s", path); return -1; }

    char dirbuf[512];
    const char *dir = ".";
    if (slash) {
        size_t n = (size_t)(slash - path);
        if (n >= sizeof dirbuf) n = sizeof dirbuf - 1;
        memcpy(dirbuf, path, n);
        dirbuf[n] = 0;
        dir = dirbuf;
    }
    DIR *d = opendir(dir);
    if (!d) { snprintf(out, cap, "%s", path); return -1; }

    struct dirent *e;
    int rc = -1;
    while ((e = readdir(d)) != NULL) {
        if (strcasecmp(e->d_name, leaf)) continue;
        /* +1 keeps the separator: `slash - path` stops *before* the '/', so a
         * plain "%.*s%s" glues the leaf onto the directory name
         * ("/a/b" + "c.mpq" -> "/a/bc.mpq"). */
        if (slash) snprintf(out, cap, "%.*s%s", (int)(slash - path) + 1, path, e->d_name);
        else snprintf(out, cap, "%s", e->d_name);
        rc = 0;
        break;
    }
    closedir(d);
    if (rc != 0) snprintf(out, cap, "%s", path);
    return rc;
}

/* Save files live under .../Save or carry the .w3z/.w3v suffix.  They are
 * always logged: the normal open/read trace has a fixed budget that is long
 * exhausted by the time a player saves, which is exactly the interval this
 * probe has to cover. */
static int save_path(const char *p)
{
    if (!p) return 0;
    if (strstr(p, "/Save/") || strstr(p, "\\Save\\")) return 1;
    size_t n = strlen(p);
    if (n >= 4 && (strcasecmp(p + n - 4, ".w3z") == 0 ||
                   strcasecmp(p + n - 4, ".w3v") == 0))
        return 1;
    return 0;
}

static void h_create_file(CPU *c)
{
    const char *name = gstr(arg32(c, 0));
    uint32_t access = arg32(c, 1), disp = arg32(c, 4), flags = arg32(c, 5);
    char host[512], real[512];
    win_path(name, host, sizeof host);
    ci_resolve(host, real, sizeof real);
    int is_save = save_path(real);
    int oflags = 0;
    int rd = (access & 0x80000000u) != 0, wr = (access & 0x40000000u) != 0;
    switch (disp) {
    case 1: oflags = O_CREAT | O_EXCL | (rd && wr ? O_RDWR : (wr ? O_WRONLY : O_RDONLY)); break;
    case 2: oflags = O_CREAT | O_TRUNC | (rd && wr ? O_RDWR : (wr ? O_WRONLY : O_RDONLY)); break;
    case 3: oflags = (rd && wr ? O_RDWR : (wr ? O_WRONLY : O_RDONLY)); break;
    case 4: oflags = O_CREAT | (rd && wr ? O_RDWR : (wr ? O_WRONLY : O_RDONLY)); break;
    case 5: oflags = O_TRUNC | (rd && wr ? O_RDWR : (wr ? O_WRONLY : O_RDONLY)); break;
    default: oflags = O_RDONLY; break;
    }
    int fd = open(real, oflags, 0666);
    if (fd < 0) {
        g_last_error = (errno == ENOENT) ? 2u : (errno == EACCES ? 5u : 3u);
#if WC3_DEBUG && WC3_DEBUG_HLE
        if (is_save)
            fprintf(stderr, "[save] create failed \"%s\" access=%08X disp=%u "
                            "flags=%08X errno=%d\n",
                    real, access, disp, flags, errno);
#endif
#if WC3_DEBUG
        if (g_trace) fprintf(stderr, "[file] open failed %s (%s) errno=%d\n", name, real, errno);
#endif
        c->eax = GH_INVALID;
        return;
    }
    uint32_t h = gh_new(1);
    GHandle *g = gh_get(h);
    if (!g) { close(fd); c->eax = GH_INVALID; return; }
    g->fd = fd;
    g->save_trace = is_save;
    g->overlapped = (flags & 0x40000000u) != 0;   /* FILE_FLAG_OVERLAPPED */
    g_last_error = 0;
#if WC3_DEBUG && WC3_DEBUG_HLE
    if (is_save)
        fprintf(stderr, "[save] create \"%s\" access=%08X disp=%u flags=%08X "
                        "-> h=%08X fd=%d\n", real, access, disp, flags, h, fd);
#endif
    /* The archive open path is the one place where "found" and "opened" differ,
     * so name the handle as well as the file.  Every other open is logged too
     * (bounded), because "was the file ever opened at all" is the question a
     * failing GetFileAttributesA-then-open sequence turns on. */
#if WC3_DEBUG
    {
        static unsigned opens;
        if (strstr(name, ".mpq") || strstr(name, ".MPQ") || opens < 120) {
            if (opens < 120) opens++;
            fprintf(stderr, "[file] open \"%s\" -> handle=%08X fd=%d ra=%08X\n",
                    real, h, fd, rd32g(c->esp));
        }
    }
#endif
    c->eax = h;
}

static int ev_close(uint32_t h);    /* defined with the sync objects below */
static int ev_close_if_used(uint32_t h);
static int mutex_close(uint32_t h);
static int iocp_close(uint32_t h);  /* defined with the completion ports below */
static void file_view_release(GHandle *m);

static void h_close_handle(CPU *c)
{
    uint32_t h = arg32(c, 0);
    if (mutex_close(h)) { c->eax = 1; return; }
    if (ev_close_if_used(h)) { c->eax = 1; return; }
    /* Completion ports are handed out from the stateless range too, but they
     * own a queue and blocked waiters; close has to wake those waiters before
     * the generic "nothing to free" branch below swallows the handle. */
    if (iocp_close(h)) { c->eax = 1; return; }
    /* Semaphore and thread handles share the stateless range and have no
     * state to free here. */
    if (h >= SYNC_BASE && h < SYNC_MID && ((h - SYNC_BASE) % 4u) == 0) {
        c->eax = 1;
        return;
    }
    GHandle *g = gh_get(h);
    if (!g) { c->eax = 0; return; }
#if WC3_DEBUG && WC3_DEBUG_HLE
    if (g->save_trace)
        fprintf(stderr, "[save] close h=%08X fd=%d\n", h, g->fd);
#endif
    if (g->fd >= 0) close(g->fd);
    if (g->dir) closedir(g->dir);
    if (g->kind == 4) file_view_release(g);
    memset(g, 0, sizeof *g);
    c->eax = 1;
}

/* Defined with the completion-port block further down; ReadFile/WriteFile post
 * their completions through it so an overlapped caller is woken the same way a
 * real port would wake it. */
static void iocp_complete_file(uint32_t file, uint32_t bytes, uint32_t ov);

/* The guest opens save files with FILE_FLAG_OVERLAPPED and carries the file
 * position in OVERLAPPED.Offset/OffsetHigh, not in the host file pointer.
 * Honor it with pread/pwrite: the save writer lays out header at 0 and body at
 * 0x44, and a plain sequential write appends the header after the body. */
static int h_ov_offset(uint32_t ov, off_t *off)
{
    if (!ov || !guest_ok(ov, 0x14)) return 0;
    uint32_t lo = rd32g(ov + 8), hi = rd32g(ov + 0xc);
    if (lo == 0xFFFFFFFFu && hi == 0xFFFFFFFFu) return 0;  /* current position */
    *off = (off_t)(((uint64_t)hi << 32) | (uint64_t)lo);
    return 1;
}

static void h_read_file(CPU *c)
{
    uint32_t h = arg32(c, 0), buf = arg32(c, 1), n = arg32(c, 2), got = arg32(c, 3);
    uint32_t ov = arg32(c, 4);
    GHandle *g = gh_get(h);
    if (got && guest_ok(got, 4)) wr32g(got, 0);
    if (!g || g->fd < 0 || !guest_ok(buf, n)) { g_last_error = 6; c->eax = 0; return; }
    off_t off = -1;
    ssize_t r;
    if (g->overlapped && h_ov_offset(ov, &off))
        r = pread(g->fd, (void *)(uintptr_t)buf, n, off);
    else
        r = read(g->fd, (void *)(uintptr_t)buf, n);
#if WC3_DEBUG && WC3_DEBUG_HLE
    static unsigned save_reads;
    if (g->save_trace && save_reads++ < 400)
        fprintf(stderr, "[save] read h=%08X n=%u off=%lld -> %d errno=%d\n",
                h, n, (long long)off, (int)r, r < 0 ? errno : 0);
#endif
    if (r < 0) { g_last_error = 3; c->eax = 0; return; }
    if (got && guest_ok(got, 4)) wr32g(got, (uint32_t)r);
#if WC3_DEBUG
    if (g_io_trace < 400) { g_io_trace++; fprintf(stderr, "[file] read h=%08X n=%u -> %d\n", h, n, (int)r); }
#endif
    /* Win32 queues a completion packet only for an overlapped request.  A
     * synchronous ReadFile on a port-associated handle completes in place and
     * must not put an unexpected key into the worker's queue. */
    if (ov) iocp_complete_file(h, (uint32_t)r, ov);
    c->eax = 1;
}

static void h_write_file(CPU *c)
{
    uint32_t h = arg32(c, 0), buf = arg32(c, 1), n = arg32(c, 2), put = arg32(c, 3);
    uint32_t ov = arg32(c, 4);
    GHandle *g = gh_get(h);
    if (put && guest_ok(put, 4)) wr32g(put, 0);
    if (!g || g->fd < 0 || !guest_ok(buf, n)) { c->eax = 0; return; }
    off_t off = -1;
    ssize_t r;
    if (g->overlapped && h_ov_offset(ov, &off))
        r = pwrite(g->fd, (void *)(uintptr_t)buf, n, off);
    else
        r = write(g->fd, (void *)(uintptr_t)buf, n);
#if WC3_DEBUG && WC3_DEBUG_HLE
    static unsigned save_writes;
    if (g->save_trace && save_writes++ < 400)
        fprintf(stderr, "[save] write h=%08X n=%u off=%lld -> %d errno=%d\n",
                h, n, (long long)off, (int)r, r < 0 ? errno : 0);
#endif
    if (r < 0) { c->eax = 0; return; }
    if (put && guest_ok(put, 4)) wr32g(put, (uint32_t)r);
    if (ov) iocp_complete_file(h, (uint32_t)r, ov);
    c->eax = 1;
}

static void h_flush_file_buffers(CPU *c)
{
    GHandle *g = gh_get(arg32(c, 0));
    int rc = 0;
    if (g && g->fd >= 0) rc = fsync(g->fd);
#if WC3_DEBUG && WC3_DEBUG_HLE
    if (g && g->save_trace)
        fprintf(stderr, "[save] flush h=%08X fd=%d -> %d errno=%d\n",
                arg32(c, 0), g->fd, rc, rc < 0 ? errno : 0);
#endif
    c->eax = 1;
}

static void h_set_file_pointer(CPU *c)
{
    uint32_t h = arg32(c, 0), lo = arg32(c, 1), hi = arg32(c, 2), method = arg32(c, 3);
    GHandle *g = gh_get(h);
    if (!g || g->fd < 0) { c->eax = GH_INVALID; return; }
    int whence = method == 0 ? SEEK_SET : (method == 1 ? SEEK_CUR : SEEK_END);
    uint32_t hin = (hi && guest_ok(hi, 4)) ? rd32g(hi) : 0;
    off_t off = (off_t)(int32_t)lo;
    off_t r = lseek(g->fd, off, whence);
#if WC3_DEBUG && WC3_DEBUG_HLE
    if (g->save_trace)
        fprintf(stderr, "[save] seek h=%08X lo=%d hi=%08X method=%u -> %d errno=%d\n",
                h, (int)(int32_t)lo, hin, method, (int)r, r < 0 ? errno : 0);
#endif
    if (r < 0) { c->eax = GH_INVALID; return; }
    if (hi && guest_ok(hi, 4)) wr32g(hi, (uint32_t)((uint64_t)r >> 32));
#if WC3_DEBUG
    if (g_io_trace < 400) { g_io_trace++; fprintf(stderr, "[file] seek h=%08X lo=%d hi=%08X method=%u -> %d\n",
                                                  h, (int)(int32_t)lo, hin, method, (int)r); }
#endif
    c->eax = (uint32_t)r;
}

static void h_get_file_size(CPU *c)
{
    uint32_t h = arg32(c, 0), hi = arg32(c, 1);
    GHandle *g = gh_get(h);
    if (!g || g->fd < 0) { c->eax = GH_INVALID; return; }
    struct stat st;
    if (fstat(g->fd, &st) != 0) { c->eax = GH_INVALID; return; }
#if WC3_DEBUG && WC3_DEBUG_HLE
    if (g->save_trace)
        fprintf(stderr, "[save] size h=%08X -> %lld\n", h, (long long)st.st_size);
#endif
    if (hi && guest_ok(hi, 4)) wr32g(hi, (uint32_t)((uint64_t)st.st_size >> 32));
    c->eax = (uint32_t)st.st_size;
}

static void h_get_file_time(CPU *c)
{
    uint32_t h = arg32(c, 0);
    GHandle *g = gh_get(h);
    struct stat st;
    if (!g || g->fd < 0 || fstat(g->fd, &st) != 0) { c->eax = 0; return; }
    uint64_t ft = filetime_from_time_t(st.st_mtime);
    uint32_t cr = arg32(c, 1), ac = arg32(c, 2), wr = arg32(c, 3);
    if (cr && guest_ok(cr, 8)) wr_filetime(cr, ft);
    if (ac && guest_ok(ac, 8)) wr_filetime(ac, ft);
    if (wr && guest_ok(wr, 8)) wr_filetime(wr, ft);
    c->eax = 1;
}

static void h_set_file_time(CPU *c) { (void)c; c->eax = 1; }

static void h_set_end_of_file(CPU *c)
{
    GHandle *g = gh_get(arg32(c, 0));
    if (!g || g->fd < 0) { c->eax = 0; return; }
    off_t cur = lseek(g->fd, 0, SEEK_CUR);
    int rc = ftruncate(g->fd, cur);
#if WC3_DEBUG && WC3_DEBUG_HLE
    if (g->save_trace)
        fprintf(stderr, "[save] truncate h=%08X at=%lld -> %d errno=%d\n",
                arg32(c, 0), (long long)cur, rc, rc < 0 ? errno : 0);
#endif
    c->eax = rc == 0 ? 1u : 0u;
}

static void h_get_file_attributes(CPU *c)
{
    char host[512], real[512];
    struct stat st;
    const char *guest = gstr(arg32(c, 0));
    win_path(guest, host, sizeof host);
    ci_resolve(host, real, sizeof real);
    if (stat(real, &st) != 0) {
        /* The cwd is part of the answer: the guest resolves relative names
         * against its *current* directory, and game.dll calls
         * SetCurrentDirectoryA to its install path, so "the file is there but
         * not found" almost always means "it is there, in another directory". */
#if WC3_DEBUG
        char cwd[256];
        if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
        fprintf(stderr, "[file] attributes \"%s\" -> \"%s\": not found ra=%08X cwd=%s\n",
                guest, real, rd32g(c->esp), cwd);
#endif
        g_last_error = 2;
        c->eax = GH_INVALID;
        return;
    }
    /* The guest return address is what identifies the caller: the archive
     * check in game.dll and Storm's own SFileOpenArchive both ask, and only
     * the address tells them apart. */
#if WC3_DEBUG
    fprintf(stderr, "[file] attributes \"%s\" -> \"%s\": found attr=%08X ra=%08X\n",
            guest, real, st_attrs(&st), rd32g(c->esp));
#endif
    c->eax = st_attrs(&st);
}

static void h_set_file_attributes(CPU *c) { (void)c; c->eax = 1; }

static void h_delete_file(CPU *c)
{
    char host[512];
    win_path(gstr(arg32(c, 0)), host, sizeof host);
    int rc = unlink(host);
#if WC3_DEBUG && WC3_DEBUG_HLE
    if (save_path(host))
        fprintf(stderr, "[save] delete \"%s\" -> %d errno=%d\n",
                host, rc, rc < 0 ? errno : 0);
#endif
    c->eax = rc == 0 ? 1u : 0u;
}

static void h_move_file(CPU *c)
{
    char a[512], b[512];
    win_path(gstr(arg32(c, 0)), a, sizeof a);
    win_path(gstr(arg32(c, 1)), b, sizeof b);
    int rc = rename(a, b);
#if WC3_DEBUG && WC3_DEBUG_HLE
    if (save_path(a) || save_path(b))
        fprintf(stderr, "[save] move \"%s\" -> \"%s\" = %d errno=%d\n",
                a, b, rc, rc < 0 ? errno : 0);
#endif
    c->eax = rc == 0 ? 1u : 0u;
}

static void h_copy_file(CPU *c)
{
    char a[512], b[512];
    win_path(gstr(arg32(c, 0)), a, sizeof a);
    win_path(gstr(arg32(c, 1)), b, sizeof b);
    int in = open(a, O_RDONLY);
    if (in < 0) { c->eax = 0; return; }
    int out = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) { close(in); c->eax = 0; return; }
    char buf[65536];
    ssize_t n;
    int ok = 1;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) { ok = 0; break; }
    close(in); close(out);
    c->eax = ok ? 1u : 0u;
}

static void h_create_directory(CPU *c)
{
    char host[512];
    win_path(gstr(arg32(c, 0)), host, sizeof host);
    c->eax = mkdir(host, 0777) == 0 ? 1u : 0u;
}

static void h_remove_directory(CPU *c)
{
    char host[512];
    win_path(gstr(arg32(c, 0)), host, sizeof host);
    c->eax = rmdir(host) == 0 ? 1u : 0u;
}

static void h_find_first_file(CPU *c)
{
    const char *pat = gstr(arg32(c, 0));
    uint32_t dst = arg32(c, 1);
    char host[512], dirpath[512];
    win_path(pat, host, sizeof host);
    char *slash = strrchr(host, '/');
    const char *mask;
    if (slash) {
        size_t n = (size_t)(slash - host);
        if (n >= sizeof dirpath) n = sizeof dirpath - 1;
        memcpy(dirpath, host, n);
        dirpath[n] = 0;
        if (!n) snprintf(dirpath, sizeof dirpath, "/");
        mask = slash + 1;
    } else {
        snprintf(dirpath, sizeof dirpath, ".");
        mask = host;
    }
    if (!*mask) mask = "*";
#if WC3_DEBUG
    fprintf(stderr, "[find] first \"%s\" -> dir=\"%s\" mask=\"%s\"\n",
            pat, dirpath, mask);
#endif
    DIR *d = opendir(dirpath);
    if (!d) {
        fprintf(stderr, "[find]   opendir failed: %s\n", strerror(errno));
        g_last_error = 2;
        c->eax = GH_INVALID;
        return;
    }
    uint32_t h = gh_new(2);
    GHandle *g = gh_get(h);
    if (!g) { closedir(d); c->eax = GH_INVALID; return; }
    g->dir = d;
    snprintf(g->pattern, sizeof g->pattern, "%s", mask);
    snprintf(g->dirpath, sizeof g->dirpath, "%s", dirpath);
    /* first entry */
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (fnmatch(g->pattern, de->d_name, 0) != 0) continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", dirpath, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (dst && guest_ok(dst, 318)) fill_find_data(dst, de->d_name, &st);
#if WC3_DEBUG
        fprintf(stderr, "[find]   match \"%s\"\n", de->d_name);
#endif
        c->eax = h;
        return;
    }
    closedir(d);
    memset(g, 0, sizeof *g);
#if WC3_DEBUG
    fprintf(stderr, "[find]   no match\n");
#endif
    g_last_error = 2;
    c->eax = GH_INVALID;
}

static void h_find_next_file(CPU *c)
{
    GHandle *g = gh_get(arg32(c, 0));
    uint32_t dst = arg32(c, 1);
    if (!g || !g->dir) { c->eax = 0; return; }
    struct dirent *de;
    while ((de = readdir(g->dir)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (fnmatch(g->pattern, de->d_name, 0) != 0) continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s",
                 g->dirpath[0] ? g->dirpath : ".", de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) {
#if WC3_DEBUG
            fprintf(stderr, "[find] next pattern=\"%s\" skip \"%s\" "
                            "(stat failed)\n", g->pattern, de->d_name);
#endif
            continue;
        }
#if WC3_DEBUG
        fprintf(stderr, "[find] next pattern=\"%s\" match \"%s\"\n",
                g->pattern, de->d_name);
#endif
        if (dst && guest_ok(dst, 318)) fill_find_data(dst, de->d_name, &st);
        c->eax = 1;
        return;
    }
#if WC3_DEBUG
    fprintf(stderr, "[find] next pattern=\"%s\" done\n", g->pattern);
#endif
    c->eax = 0;
}

static void h_find_close(CPU *c)
{
    GHandle *g = gh_get(arg32(c, 0));
    if (g && g->dir) { closedir(g->dir); memset(g, 0, sizeof *g); }
    c->eax = 1;
}

static void h_get_disk_free_space(CPU *c)
{
    uint32_t a = arg32(c, 1), b = arg32(c, 2), cc = arg32(c, 3), d = arg32(c, 4);
    if (a && guest_ok(a, 4)) wr32g(a, 512);
    if (b && guest_ok(b, 4)) wr32g(b, 0x00010000);
    if (cc && guest_ok(cc, 4)) wr32g(cc, 0x00040000);
    if (d && guest_ok(d, 4)) wr32g(d, 0x00100000);
    c->eax = 1;
}

/* 64-bit variant.  The profile-creation path asks this before it creates the
 * new profile directory; a failure here sends it down an error path that
 * formats an object it has not built yet and crashes.  Report the same volume
 * geometry as GetDiskFreeSpaceA so the two answers agree. */
static void h_get_disk_free_space_ex(CPU *c)
{
    uint32_t avail = arg32(c, 1), total = arg32(c, 2), freeb = arg32(c, 3);
    const uint64_t bytes_per_cluster = 512ull * 0x10000ull;
    const uint64_t free_bytes = bytes_per_cluster * 0x40000ull;
    const uint64_t total_bytes = bytes_per_cluster * 0x100000ull;

    if (avail && guest_ok(avail, 8)) {
        wr32g(avail, (uint32_t)free_bytes);
        wr32g(avail + 4, (uint32_t)(free_bytes >> 32));
    }
    if (total && guest_ok(total, 8)) {
        wr32g(total, (uint32_t)total_bytes);
        wr32g(total + 4, (uint32_t)(total_bytes >> 32));
    }
    if (freeb && guest_ok(freeb, 8)) {
        wr32g(freeb, (uint32_t)free_bytes);
        wr32g(freeb + 4, (uint32_t)(free_bytes >> 32));
    }
    c->eax = 1;
}

static void h_get_logical_drive_strings(CPU *c)
{
    uint32_t n = arg32(c, 0), buf = arg32(c, 1);
    const char *s = "C:\\";
    size_t len = strlen(s) + 2;
    if (buf && n >= len && guest_ok(buf, (uint32_t)len)) {
        memcpy((void *)(uintptr_t)buf, s, strlen(s) + 1);
        ((char *)(uintptr_t)buf)[strlen(s) + 1] = 0;
    }
    c->eax = (uint32_t)len;
}

static void h_get_drive_type(CPU *c) { (void)c; c->eax = 3; }  /* DRIVE_FIXED */

/* The launcher identifies the install medium by volume label.  Reporting
 * success with the retail label is what a hard-disk install looks like; a
 * failure here makes 1.27 show "please insert the Warcraft III CD". */
static void h_get_volume_information(CPU *c)
{
    uint32_t name_buf = arg32(c, 1), name_len = arg32(c, 2);
    uint32_t serial = arg32(c, 3), maxcomp = arg32(c, 4), flags = arg32(c, 5);
    uint32_t fs_buf = arg32(c, 6), fs_len = arg32(c, 7);

    if (name_buf && name_len > 1 && guest_ok(name_buf, name_len)) {
        const char *label = "WARCRAFT3";
        size_t n = strlen(label) + 1;
        if (n > name_len) n = name_len;
        memcpy((void *)(uintptr_t)name_buf, label, n);
        ((char *)(uintptr_t)name_buf)[n - 1] = 0;
    }
    if (fs_buf && fs_len > 1 && guest_ok(fs_buf, fs_len)) {
        const char *fs = "FAT32";
        size_t n = strlen(fs) + 1;
        if (n > fs_len) n = fs_len;
        memcpy((void *)(uintptr_t)fs_buf, fs, n);
        ((char *)(uintptr_t)fs_buf)[n - 1] = 0;
    }
    if (serial && guest_ok(serial, 4)) wr32g(serial, 0x1A2B3C4Du);
    if (maxcomp && guest_ok(maxcomp, 4)) wr32g(maxcomp, 255);
    if (flags && guest_ok(flags, 4)) wr32g(flags, 0);
    c->eax = 1;
}

/* ---- memory ---- */
static void h_global_alloc(CPU *c)
{
    uint32_t flags = arg32(c, 0), size = arg32(c, 1);
    if (flags & 0x0040u) {                 /* GMEM_ZEROINIT */
        c->eax = hle_alloc0(size ? size : 1);
    } else {
        c->eax = hle_guest_alloc(size ? size : 1);
    }
}
static void h_global_free(CPU *c) { hle_guest_free(arg32(c, 0)); c->eax = 0; }
static void h_global_lock(CPU *c) { c->eax = arg32(c, 0); }
static void h_global_unlock(CPU *c) { (void)c; c->eax = 1; }
static void h_global_size(CPU *c)
{
    uint32_t p = arg32(c, 0);
    if (!p) { c->eax = 0; return; }
    uint32_t hdr = p - 8;
    if (!guest_ok(hdr, 8)) { c->eax = 0; return; }
    c->eax = rd32g(hdr);
}

/* A 16 MB-or-larger reserve is rare enough to log without a switch, and the
 * caller is the only way to tell a genuine game buffer from a stale live entry.
 * The guest stack still holds the return addresses of Storm's allocator and of
 * its caller at HLE entry, so print the code-looking words in the first 32
 * slots with a module label. */
#if WC3_DEBUG
static const char *va_module_name(uint32_t v)
{
    if (v >= 0x00400000u && v < 0x01000000u) return "war3";
    if (v >= GUEST_IMAGE_STORM && v < GUEST_IMAGE_MSS) return "storm";
    if (v >= GUEST_IMAGE_MSS && v < GUEST_IMAGE_MSSFAST) return "mss32";
    if (v >= GUEST_IMAGE_MSSFAST && v < GUEST_IMAGE_MSSFAST + 0x00100000u) return "mssfast";
    if (v >= GUEST_IMAGE_MP3DEC && v < GUEST_IMAGE_MP3DEC + 0x00040000u) return "mp3dec";
    if (v >= GUEST_IMAGE_GAME && v < GUEST_IMAGE_GAME + 0x01000000u) return "game";
    return NULL;
}

static void va_log_callers(CPU *c)
{
    uint32_t sp = c->esp;
    if (!guest_ok(sp, 32 * 4)) return;
    for (unsigned i = 0; i < 32; i++) {
        uint32_t v = rd32g(sp + i * 4);
        const char *m = va_module_name(v);
        if (m) fprintf(stderr, "[va]   stack[%u] %08X %s\n", i, v, m);
    }

    /* The raw stack scan above only finds return addresses that happened to
     * be spilled.  The EBP chain is the real call stack; the lifted Storm
     * allocator keeps frame pointers, so this names the game function that
     * asked for the huge block instead of stopping inside SMemAlloc. */
    uint32_t ebp = c->ebp;
    for (unsigned frame = 0; ebp && frame < 32 && guest_ok(ebp, 8); frame++) {
        uint32_t next = rd32g(ebp);
        uint32_t ret  = rd32g(ebp + 4);
        const char *m = va_module_name(ret);
        if (m || (ret >= 0x00400000u && ret < 0x10000000u))
            fprintf(stderr, "[va]   frame[%u] %08X %s\n",
                    frame, ret, m ? m : "?");
        if (next <= ebp || (uint64_t)next - ebp > 0x00100000u) break;
        ebp = next;
    }
}
#endif

static void h_virtual_alloc(CPU *c)
{
    uint32_t addr = arg32(c, 0), size = arg32(c, 1), type = arg32(c, 2), prot = arg32(c, 3);
    (void)prot;
#if WC3_DEBUG
    if (size >= 0x01000000u) {
        fprintf(stderr, "[va] BIG alloc ra=%08X addr=%08X size=%u type=%08X\n",
                guest_ok(c->esp, 4) ? rd32g(c->esp) : 0, addr, size, type);
        va_log_callers(c);
    }
#endif
    /* Win32 rejects a request whose range wraps, and a commit without a
     * reservation.  Returning an address for either is how run 12 walked off
     * the top of the address space: Storm committed 32 MB at 0xFFAD7000 inside
     * a 256 MB reserve that itself ended at 0x10BAD7000, and the heap's bump
     * pointer then wrapped to 0xFFFFFF80. */
    if (!size || (uint64_t)addr + size > 0x100000000ull) {
        fprintf(stderr, "[va] VirtualAlloc bad range addr=%08X size=%u type=%08X ra=%08X\n",
                addr, size, type, guest_ok(c->esp, 4) ? rd32g(c->esp) : 0);
        g_last_error = 87;                 /* ERROR_INVALID_PARAMETER */
        c->eax = 0;
        return;
    }
    int want_reserve = (type & 0x2000u) != 0;  /* MEM_RESERVE */
    int want_commit  = (type & 0x1000u) != 0;  /* MEM_COMMIT */

    if (addr) {
        if (want_commit && !want_reserve) {
            /* Commit-only: the pages must already be reserved. */
            if (va_live_covers(addr, size) || va_fixed_mapped(addr, size)) {
                c->eax = addr;
            } else {
#if WC3_DEBUG
                static int rejected;
                if (rejected++ < 32)
                    fprintf(stderr, "[va] commit reject %08X+%X type=%08X ra=%08X\n",
                            addr, size, type,
                            guest_ok(c->esp, 4) ? rd32g(c->esp) : 0);
#endif
                g_last_error = 487;        /* ERROR_INVALID_ADDRESS */
                c->eax = 0;
            }
            return;
        }
        /* A specific-address reserve: the range is already inside one of the
         * arenas, so hand it back mapped and remember the size - Win32 will
         * release it with dwSize 0.
         *
         * A failed map is normally EEXIST, i.e. the range is already mapped.
         * If it is also already *ours*, adding a second live entry is the bug
         * described at va_live_overlaps(): one release then unmaps a range
         * that another entry still owns.  Keep the existing entry instead. */
        if (guest_ok(addr, size)) {
            uint32_t rounded = va_round(size);
            if (va_live_overlaps(addr, rounded)) {
#if WC3_DEBUG
                if (g_va_trace)
                    fprintf(stderr, "[va] reserve %08X+%X already live, keeping it\n",
                            addr, rounded);
#endif
                c->eax = addr;
                return;
            }
            if (!guest_map(addr, rounded)) {
                /* The range belongs to someone else (the D3D9 bridge owns its
                 * staging arena).  Win32 reports failure and the guest is
                 * expected to retry elsewhere; recording it as live would hide
                 * the range from every later allocation. */
                fprintf(stderr, "[va] reserve %08X+%X FAILED\n", addr, rounded);
                c->eax = 0;
                return;
            }
#if WC3_DEBUG
            if (g_va_trace)
                fprintf(stderr, "[va] reserve %08X+%X\n", addr, rounded);
#endif
            va_live_add(addr, rounded);
            c->eax = addr;
            return;
        }
    } else if (want_commit && !want_reserve) {
        /* Windows accepts VirtualAlloc(NULL, n, MEM_COMMIT, ...) and treats it
         * as a fresh reservation (measured on the Windows host: it returns a
         * usable address).  Storm's war3.mpq buffer allocator uses exactly
         * this form, so falling through to va_alloc is the faithful path. */
#if WC3_DEBUG
        if (g_va_trace)
            fprintf(stderr, "[va] commit %u without an address, reserving\n", size);
#endif
    }
    c->eax = va_alloc(size);
    /* Storm's SMemAlloc turns a NULL here into SErrSetLastError(8), so every
     * failure has to be visible: the guest just returns 0 from the open. */
    if (!c->eax) {
        fprintf(stderr, "[va] VirtualAlloc(%08X, %u, %08X) FAILED "
                        "(arena1 %08X/%08X, arena2 %08X, arena3 %08X, "
                        "arena4 %08X, arena5 %08X, arena6 %08X, "
                        "arena7 %08X, live %u, free %u)\n",
                addr, size, type, g_va_next, GUEST_VA_BASE + GUEST_VA_SIZE,
                g_va2_next, g_va3_next, g_va4_next, g_va5_next,
                g_va6_next, g_va7_next, g_va_live_n, g_va_free_n);
        /* The sizes that matter are the few large blocks: a 480 MB live
         * reservation explains a failed 256 MB reserve much better than the
         * number of 64 KB Storm slots does. */
        for (unsigned i = 0; i < g_va_live_n; i++)
            if (g_va_live[i].addr && g_va_live[i].size >= 0x00100000u)
                fprintf(stderr, "[va]   live %08X+%X\n",
                        g_va_live[i].addr, g_va_live[i].size);
        for (unsigned i = 0; i < g_va_free_n; i++)
            if (g_va_free[i].size >= 0x00100000u)
                fprintf(stderr, "[va]   free %08X+%X\n",
                        g_va_free[i].addr, g_va_free[i].size);
    }
}

static void h_virtual_free(CPU *c)
{
    uint32_t addr = arg32(c, 0), size = arg32(c, 1), type = arg32(c, 2);
#if WC3_DEBUG
    if (g_va_trace || size >= 0x01000000u)
        fprintf(stderr, "[va] release req %08X+%X type=%08X ra=%08X\n",
                addr, size, type, guest_ok(c->esp, 4) ? rd32g(c->esp) : 0);
#endif
    if (type & 0x8000u) {                  /* MEM_RELEASE */
        uint32_t n = va_live_take(addr);
        if (!n) n = va_round(size);
        if (n && (uint64_t)addr + n > 0x100000000ull) n = 0;
        if (addr && n) {
            guest_unmap(addr, n);
            va_release(addr, n);
        }
    }
    c->eax = 1;
}

static void h_virtual_protect(CPU *c) { (void)c; c->eax = 1; }

/* MEMORY_BASIC_INFORMATION (32-bit) is 28 bytes: BaseAddress, AllocationBase,
 * AllocationProtect, RegionSize, State, Protect, Type.  The crash reporter and
 * the CRT use this to decide whether an address is readable; returning 0 made
 * them treat live guest memory as unmapped and then walk a bogus frame. */
static void h_virtual_query(CPU *c)
{
    uint32_t addr = arg32(c, 0), out = arg32(c, 1), len = arg32(c, 2);
    if (!addr || !out || len < 28 || !guest_ok(out, 28)) { c->eax = 0; return; }

    uint32_t base = addr & ~(VA_PAGE - 1u);
    uint32_t region = VA_PAGE;
    for (unsigned i = 0; i < g_va_live_n; i++) {
        uint64_t lo = g_va_live[i].addr;
        uint64_t hi = lo + g_va_live[i].size;
        if (!lo || !g_va_live[i].size || addr < lo || addr >= hi) continue;
        base = g_va_live[i].addr;
        region = g_va_live[i].size;
        break;
    }
    if (!guest_ok(addr, 1) && region == VA_PAGE) { c->eax = 0; return; }

    memset((void *)(uintptr_t)out, 0, 28);
    wr32g(out + 0, base);              /* BaseAddress */
    wr32g(out + 4, base);              /* AllocationBase */
    wr32g(out + 8, 0x04);              /* PAGE_READWRITE */
    wr32g(out + 12, region);           /* RegionSize */
    wr32g(out + 16, 0x1000);           /* MEM_COMMIT */
    wr32g(out + 20, 0x04);             /* PAGE_READWRITE */
    wr32g(out + 24, 0x20000);          /* MEM_PRIVATE */
    c->eax = 28;
}

/* IsBadReadPtr / IsBadWritePtr.  Deprecated on Windows and every real caller
 * uses SEH instead, but Storm's crash reporter still probes the stack with
 * them before it dumps it.  Returning 0 ("readable") made it walk a bogus
 * frame and then *call through a stack address*, which is the SIGABRT that
 * hid the real error report.  The question they actually ask is "is this
 * inside the guest's mapped space", and the arena table answers that. */
static void h_is_bad_ptr(CPU *c)
{
    uint32_t p = arg32(c, 0), n = arg32(c, 1);
    if (!p) { c->eax = 1; return; }
    if (!n) { c->eax = 0; return; }
    c->eax = guest_clamp(p, n) == n ? 0u : 1u;
}

/* OutputDebugStringA: the guest's only unfiltered channel for "what I think
 * went wrong".  Bounded, because a debug build in a loop can emit thousands. */
static void h_output_debug_string(CPU *c)
{
#if WC3_DEBUG
    static unsigned n;
    if (n++ < 40) fprintf(stderr, "[dbg] %s\n", gstr(arg32(c, 0)));
#endif
    c->eax = 0;
}

static void h_heap_create(CPU *c) { (void)c; c->eax = 0x00090000u + (g_fake_next += 4); }
static void h_heap_destroy(CPU *c) { (void)c; c->eax = 1; }
static void h_heap_alloc(CPU *c)
{
    uint32_t flags = arg32(c, 1), size = arg32(c, 2);
    c->eax = (flags & 8u) ? hle_alloc0(size ? size : 1)
                          : hle_guest_alloc(size ? size : 1);
}
static void h_heap_free(CPU *c) { hle_guest_free(arg32(c, 2)); c->eax = 1; }
static void h_heap_realloc(CPU *c)
{
    uint32_t p = arg32(c, 2), size = arg32(c, 3);
    if (!p) { c->eax = hle_guest_alloc(size ? size : 1); return; }
    uint32_t old = rd32g(p - 8);
    uint32_t q = hle_guest_alloc(size ? size : 1);
    if (!q) { c->eax = 0; return; }
    memcpy((void *)(uintptr_t)q, (void *)(uintptr_t)p, old < size ? old : size);
    hle_guest_free(p);
    c->eax = q;
}
static void h_heap_size(CPU *c)
{
    uint32_t p = arg32(c, 2);
    c->eax = p ? rd32g(p - 8) : 0;
}
static void h_get_process_heap(CPU *c) { c->eax = 0x00090000u; }
static void h_get_process_heaps(CPU *c)
{
    uint32_t n = arg32(c, 0), arr = arg32(c, 1);
    if (arr && n >= 4 && guest_ok(arr, 4)) wr32g(arr, 0x00090000u);
    c->eax = 1;
}

/* ---- sync ----------------------------------------------------------- */
/*
 * The guest really runs worker threads (CreateThread -> pthread_create), so
 * events must carry state.  They used to be stateless dummies and
 * WaitForSingleObject returned WAIT_OBJECT_0 at once.  That broke every MPQ
 * file read in the guest:
 *
 *   SFileReadFile (0x150230E0)
 *     -> 0x1501F200   CreateEventA(NULL, TRUE, FALSE)   completion event
 *                     SetEvent(0x84054)                 wake the read-ahead thread
 *                     WaitForSingleObject(done, INFINITE)   <-- returned instantly
 *     -> 0x1503F040   memcpy(buf, sector, 0)            nothing had been read
 *     -> SErrSetLastError(38 = ERROR_HANDLE_EOF)        so the read "hit EOF"
 *     -> LoadFile returns 0
 *
 * With no bytes read, font\font.gid came back empty, L_6F330AB0 produced a
 * zero-entry font list, L_6F046CF0's loop found count == 0, and
 * L_6F046800 showed "Warcraft III was unable to initialize." - and
 * ui\startupstrings.txt failed the same way, which is why the dialog text was
 * empty too.  One stub, two symptoms.
 */
#define EV_MAX 8192
typedef struct {
    int used;
    int manual;
    int signalled;
    int closing;    /* CloseHandle seen: wake every waiter, never reuse early */
    int refs;       /* live HLE accesses; destroy only when closing && refs == 0 */
    pthread_mutex_t m;
    pthread_cond_t  cv;
} GEvent;

static GEvent g_events[EV_MAX];
static pthread_mutex_t g_ev_lock = PTHREAD_MUTEX_INITIALIZER;
#if WC3_DEBUG
static int g_wait_no_block;      /* WC3_WAIT_NOBLOCK=1: never block (debug only) */
static unsigned g_wait_many_log;
#endif
static unsigned g_wait_timeouts;
#if WC3_DEBUG
static unsigned g_wait_many_timeouts;
#endif

static uint32_t ev_new(int manual, int initial)
{
    pthread_mutex_lock(&g_ev_lock);
    for (int i = 0; i < EV_MAX; i++) {
        if (g_events[i].used) continue;
        GEvent *e = &g_events[i];
        memset(e, 0, sizeof *e);
        pthread_mutex_init(&e->m, NULL);
        pthread_cond_init(&e->cv, NULL);
        e->used = 1;
        e->manual = manual;
        e->signalled = initial ? 1 : 0;
        pthread_mutex_unlock(&g_ev_lock);
        return SYNC_MID + (uint32_t)i * 4u;
    }
    pthread_mutex_unlock(&g_ev_lock);
    fprintf(stderr, "[hle] event table exhausted (max %d)\n", EV_MAX);
    return gh_new_sync();
}

static GEvent *ev_get(uint32_t h)
{
    if (h < SYNC_MID || h >= SYNC_END) return NULL;
    uint32_t i = (h - SYNC_MID) / 4u;
    if (i >= EV_MAX || !g_events[i].used) return NULL;
    return &g_events[i];
}

/*
 * A guest CloseHandle may race with another guest thread blocked in
 * WaitForSingleObject on the same event.  Destroying the pthread condvar under
 * that waiter corrupts the host stack and lets the guest continue into freed
 * game state.  Take a reference for every HLE access; CloseHandle marks the
 * object closing and broadcasts, and only the last reference destroys it.
 */
static GEvent *ev_ref(uint32_t h)
{
    if (h < SYNC_MID || h >= SYNC_END) return NULL;
    uint32_t i = (h - SYNC_MID) / 4u;
    if (i >= EV_MAX) return NULL;
    pthread_mutex_lock(&g_ev_lock);
    GEvent *e = &g_events[i];
    if (!e->used) {
        pthread_mutex_unlock(&g_ev_lock);
        return NULL;
    }
    e->refs++;
    pthread_mutex_unlock(&g_ev_lock);
    return e;
}

static void ev_unref(GEvent *e)
{
    pthread_mutex_lock(&g_ev_lock);
    if (--e->refs == 0 && e->closing) {
        pthread_mutex_destroy(&e->m);
        pthread_cond_destroy(&e->cv);
        memset(e, 0, sizeof *e);
    }
    pthread_mutex_unlock(&g_ev_lock);
}

#if WC3_DEBUG && WC3_DEBUG_HLE
static unsigned g_sync_close_log;
#endif

static int ev_close(uint32_t h)
{
    GEvent *e = ev_ref(h);
    if (!e) return 0;
#if WC3_DEBUG && WC3_DEBUG_HLE
    if (g_sync_close_log++ < 64)
        fprintf(stderr, "[hle] CloseHandle event h=%08X refs=%d closing=%d\n",
                h, e->refs, e->closing);
#endif
    pthread_mutex_lock(&e->m);
    e->closing = 1;
    e->signalled = 1;
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->m);
    ev_unref(e);
    return 1;
}

static int ev_close_if_used(uint32_t h)
{
    return ev_close(h);
}

/* ---- real guest mutexes ------------------------------------------------
 *
 * The guest uses WaitForSingleObject/ReleaseMutex to protect the Miles waveOut
 * buffer queue.  Treating every handle as an always-signalled dummy lets the
 * waveOut callback run concurrently with Miles' audio thread; the queue then
 * develops a cycle and the game's free-list grows without bound.  A Win32
 * mutex is recursive and owned by a thread, so keep exactly that state here.
 */
#define MUTEX_MAX ((MUTEX_END - MUTEX_BASE) / 4u)
typedef struct {
    int used;
    int owner_valid;
    pthread_t owner;
    int count;
    int closing;    /* CloseHandle seen: wake every waiter, never reuse early */
    int refs;       /* live HLE accesses; destroy only when closing && refs == 0 */
    pthread_mutex_t m;
    pthread_cond_t cv;
} GMutex;

static GMutex g_mutexes[MUTEX_MAX];
static pthread_mutex_t g_mutex_lock = PTHREAD_MUTEX_INITIALIZER;

static GMutex *mutex_get(uint32_t h)
{
    if (h < MUTEX_BASE || h >= MUTEX_END) return NULL;
    uint32_t i = (h - MUTEX_BASE) / 4u;
    if (i >= MUTEX_MAX || !g_mutexes[i].used) return NULL;
    return &g_mutexes[i];
}

/* Same lifetime rule as ev_ref: a waiter may still be inside the mutex when
 * CloseHandle arrives, so destroy only after the last reference leaves. */
static GMutex *mutex_ref(uint32_t h)
{
    if (h < MUTEX_BASE || h >= MUTEX_END) return NULL;
    uint32_t i = (h - MUTEX_BASE) / 4u;
    if (i >= MUTEX_MAX) return NULL;
    pthread_mutex_lock(&g_mutex_lock);
    GMutex *mx = &g_mutexes[i];
    if (!mx->used) {
        pthread_mutex_unlock(&g_mutex_lock);
        return NULL;
    }
    mx->refs++;
    pthread_mutex_unlock(&g_mutex_lock);
    return mx;
}

static void mutex_unref(GMutex *mx)
{
    pthread_mutex_lock(&g_mutex_lock);
    if (--mx->refs == 0 && mx->closing) {
        pthread_mutex_destroy(&mx->m);
        pthread_cond_destroy(&mx->cv);
        memset(mx, 0, sizeof *mx);
    }
    pthread_mutex_unlock(&g_mutex_lock);
}

#if WC3_DEBUG && WC3_DEBUG_HLE_SYNC
/* Temporary WC3_SYNC_TRACE probe.  Semaphore and thread handles share the
 * stateless range, so remember which allocation produced each handle; a wait
 * on an unknown stateless handle is exactly the defect this probe must expose.
 * This tag exists only for the HLE synchronization case; graphics tracing is
 * compiled under WC3_DEBUG_GRAPHICS and the two must not share a guard. */
#define SYNC_TRACE_SLOTS ((SYNC_MID - SYNC_BASE) / 4u)
#define SYNC_TRACE_LIMIT 4000u
#define SYNC_TID ((unsigned long)pthread_self())
enum {
    SYNC_TRACE_UNKNOWN,
    SYNC_TRACE_SEMAPHORE,
    SYNC_TRACE_THREAD,
    SYNC_TRACE_IOCP
};
static int g_sync_trace;
static unsigned g_sync_trace_log;
static unsigned char g_sync_trace_kind[SYNC_TRACE_SLOTS];
static uint32_t g_sync_trace_gid[SYNC_TRACE_SLOTS];

static void sync_trace_mark(uint32_t h, int kind, uint32_t gid)
{
    if (h < SYNC_BASE || h >= SYNC_MID) return;
    uint32_t i = (h - SYNC_BASE) / 4u;
    if (i < SYNC_TRACE_SLOTS) {
        g_sync_trace_kind[i] = (unsigned char)kind;
        g_sync_trace_gid[i] = gid;
    }
}

static uint32_t sync_trace_gid(uint32_t h)
{
    if (h < SYNC_BASE || h >= SYNC_MID) return 0;
    uint32_t i = (h - SYNC_BASE) / 4u;
    return i < SYNC_TRACE_SLOTS ? g_sync_trace_gid[i] : 0;
}

static const char *sync_trace_kind(uint32_t h)
{
    if (mutex_get(h)) return "mutex";
    if (ev_get(h)) return "event";
    if (h >= SYNC_BASE && h < SYNC_MID) {
        uint32_t i = (h - SYNC_BASE) / 4u;
        if (i < SYNC_TRACE_SLOTS) {
            switch (g_sync_trace_kind[i]) {
            case SYNC_TRACE_SEMAPHORE: return "semaphore";
            case SYNC_TRACE_THREAD:    return "thread";
            case SYNC_TRACE_IOCP:      return "iocp";
            }
        }
        return "stateless";
    }
    return "other";
}

#define SYNC_TRACE(...) do { \
    if (g_sync_trace && g_sync_trace_log < SYNC_TRACE_LIMIT) { \
        g_sync_trace_log++; \
        fprintf(stderr, "[sync] " __VA_ARGS__); \
    } \
} while (0)

static void sync_trace_wait1(uint32_t h, int32_t ms, uint32_t ra, uint32_t result)
{
    const char *kind = sync_trace_kind(h);
    /* Events and mutexes are the normal path and would flood the budget before
     * the credits screen.  Only semaphore/thread/unknown handles are evidence
     * for the case this probe exists to test. */
    if (!strcmp(kind, "event") || !strcmp(kind, "mutex")) return;
    SYNC_TRACE("WaitForSingleObject(h=%08X kind=%s gid=%u ms=%d ra=%08X) "
               "-> %08X tid=%lu\n",
               h, kind, sync_trace_gid(h), (int)ms, ra, result, SYNC_TID);
}
#endif

static uint32_t mutex_new(int initial_owner)
{
    pthread_mutex_lock(&g_mutex_lock);
    for (uint32_t i = 0; i < MUTEX_MAX; i++) {
        if (g_mutexes[i].used) continue;
        GMutex *mx = &g_mutexes[i];
        memset(mx, 0, sizeof *mx);
        pthread_mutex_init(&mx->m, NULL);
        pthread_cond_init(&mx->cv, NULL);
        mx->used = 1;
        if (initial_owner) {
            mx->owner = pthread_self();
            mx->owner_valid = 1;
            mx->count = 1;
        }
        pthread_mutex_unlock(&g_mutex_lock);
        return MUTEX_BASE + i * 4u;
    }
    pthread_mutex_unlock(&g_mutex_lock);
    fprintf(stderr, "[hle] mutex table exhausted (max %u)\n", MUTEX_MAX);
    return GH_INVALID;
}

static int mutex_close(uint32_t h)
{
    GMutex *mx = mutex_ref(h);
    if (!mx) return 0;
#if WC3_DEBUG && WC3_DEBUG_HLE
    if (g_sync_close_log++ < 64)
        fprintf(stderr, "[hle] CloseHandle mutex h=%08X refs=%d owner=%d closing=%d\n",
                h, mx->refs, mx->owner_valid, mx->closing);
#endif
    pthread_mutex_lock(&mx->m);
    mx->closing = 1;
    pthread_cond_broadcast(&mx->cv);
    pthread_mutex_unlock(&mx->m);
    mutex_unref(mx);
    return 1;
}

static void h_create_event(CPU *c)
{
    c->eax = ev_new(arg32(c, 1) != 0, arg32(c, 2) != 0);
}
static void h_set_event(CPU *c)
{
    GEvent *e = ev_ref(arg32(c, 0));
    if (e) {
        pthread_mutex_lock(&e->m);
        e->signalled = 1;
        pthread_cond_broadcast(&e->cv);
        pthread_mutex_unlock(&e->m);
        ev_unref(e);
    }
    c->eax = 1;
}
static void h_reset_event(CPU *c)
{
    GEvent *e = ev_ref(arg32(c, 0));
    if (e) {
        pthread_mutex_lock(&e->m);
        e->signalled = 0;
        pthread_mutex_unlock(&e->m);
        ev_unref(e);
    }
    c->eax = 1;
}
/* WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258, WAIT_FAILED = 0xFFFFFFFF.
 *
 * INFINITE is capped: a worker that never signals must not hang the boot, so
 * the wait gives up after WAIT_CAP_MS and the timeout is counted.  After a few
 * of those the cap collapses to zero, which restores the old pass-through
 * behaviour for guests that use events only as decoration. */
#define WAIT_CAP_MS 3000
static uint32_t hle_now_ms(void);

/* Guest thread handles are real joinable host threads.  The object itself is
 * declared with the thread arena below; these forward declarations let the
 * wait paths distinguish a thread handle from an unmodelled handle before the
 * thread implementation is visible. */
struct GThread;
static struct GThread *guest_thread_get(uint32_t h);
static int guest_thread_is_done(struct GThread *t);
static int guest_thread_wait(struct GThread *t, int32_t ms);

#if WC3_DEBUG && WC3_DEBUG_HLE_SLEEP
/* Case-specific probe for Sleep(0), SwitchToThread and the INFINITE wait cap.
 * It is deliberately separate from WC3_DEBUG_HLE_SYNC: the two cases answer
 * different questions and must not share an umbrella tag.  Everything here is
 * absent unless WC3_DEBUG_HLE_SLEEP is defined, and printing is off unless
 * WC3_SLEEP_TRACE is set at runtime. */
#define SLEEP_TID ((unsigned long)pthread_self())
static int g_sleep_trace_ready;
static int g_sleep_trace;
static unsigned g_sleep_log;
static unsigned g_wait_cap_log;
static unsigned g_sleep_summary;
static unsigned long long g_sleep_calls;
static unsigned g_sleep_zero_calls;

static void sleep_trace_state(void)
{
    if (!g_sleep_trace_ready) {
        g_sleep_trace = getenv("WC3_SLEEP_TRACE") != NULL;
        g_sleep_trace_ready = 1;
    }
}

static void sleep_trace_wait_cap(const char *who, uint32_t h, int32_t ms,
                                 uint32_t ra, uint32_t esp)
{
    sleep_trace_state();
    if (!g_sleep_trace) return;
    if (ms < 0 || ms > WAIT_CAP_MS) {
        if (g_wait_cap_log++ < 8)
            fprintf(stderr, "[sleep] cap-call %s h=%08X ms=%d ra=%08X esp=%08X "
                            "tid=%lu\n", who, h, (int)ms, ra, esp, SLEEP_TID);
    }
}

#endif

static void h_wait_mutex(CPU *c, GMutex *mx, int32_t ms, uint32_t ra)
{
    pthread_t self = pthread_self();

#if WC3_DEBUG
    if (ms == 0 || g_wait_no_block) {
#else
    if (ms == 0) {
#endif
        pthread_mutex_lock(&mx->m);
        if (mx->closing) {
            pthread_mutex_unlock(&mx->m);
            c->eax = 0;
            return;
        }
        if (!mx->owner_valid) {
            mx->owner = self;
            mx->owner_valid = 1;
            mx->count = 1;
            c->eax = 0;
        } else if (pthread_equal(mx->owner, self)) {
            mx->count++;
            c->eax = 0;
        } else {
            c->eax = 258;
        }
        pthread_mutex_unlock(&mx->m);
        return;
    }

#if WC3_DEBUG && WC3_DEBUG_HLE_SLEEP
    sleep_trace_wait_cap("mutex",
        MUTEX_BASE + (uint32_t)(mx - g_mutexes) * 4u, ms, ra, c->esp);
#endif
    if (ms < 0 || ms > WAIT_CAP_MS) ms = WAIT_CAP_MS;

    struct timespec end;
    clock_gettime(CLOCK_REALTIME, &end);
    end.tv_sec  += ms / 1000;
    end.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (end.tv_nsec >= 1000000000L) {
        end.tv_sec++;
        end.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&mx->m);
    while (mx->owner_valid && !pthread_equal(mx->owner, self)) {
        if (mx->closing) break;
        int rc = pthread_cond_timedwait(&mx->cv, &mx->m, &end);
        if (mx->closing) break;
        if (rc == ETIMEDOUT && mx->owner_valid &&
            !pthread_equal(mx->owner, self)) {
            pthread_mutex_unlock(&mx->m);
            if (g_wait_timeouts++ < 8)
                fprintf(stderr, "[hle] WaitForSingleObject(mutex %08X) timed out "
                                "after %d ms ra=%08X esp=%08X\n",
                        MUTEX_BASE + (uint32_t)(mx - g_mutexes) * 4u,
                        (int)ms, ra, c->esp);
            c->eax = 258;
            return;
        }
    }
    if (mx->closing) {
        pthread_mutex_unlock(&mx->m);
        c->eax = 0;
        return;
    }
    if (!mx->owner_valid) {
        mx->owner = self;
        mx->owner_valid = 1;
        mx->count = 1;
    } else {
        mx->count++;
    }
    pthread_mutex_unlock(&mx->m);
    c->eax = 0;
}

static void h_wait_one(CPU *c)
{
    uint32_t h = arg32(c, 0);
    int32_t  ms = (int32_t)arg32(c, 1);
    uint32_t ra = guest_ok(c->esp, 4) ? rd32g(c->esp) : 0;
#if WC3_DEBUG && WC3_DEBUG_HLE
    watch_note_wait(ra, h, 0, 1);
#endif
    GMutex *mx = mutex_ref(h);
    if (mx) {
        h_wait_mutex(c, mx, ms, ra);
        mutex_unref(mx);
#if WC3_DEBUG && WC3_DEBUG_HLE_SYNC
        sync_trace_wait1(h, ms, ra, c->eax);
#endif
        return;
    }
    GEvent *e = ev_ref(h);
    if (!e) {
        /* A thread handle becomes signalled when the guest thread returns.
         * Returning WAIT_OBJECT_0 for every unknown handle made the save path
         * believe its worker had finished, so it released a mutex the worker
         * still owned and spun forever (the pause-menu freeze). */
        struct GThread *gt = guest_thread_get(h);
        if (gt) {
            int done = guest_thread_wait(gt, ms);
            c->eax = done ? 0u : 258u;
#if WC3_DEBUG && WC3_DEBUG_HLE_SYNC
            sync_trace_wait1(h, ms, ra, c->eax);
#endif
            return;
        }
        /* Unknown thread/semaphore handle: nothing to wait on. */
#if WC3_DEBUG && WC3_DEBUG_HLE_SYNC
        sync_trace_wait1(h, ms, ra, 0);
#endif
        c->eax = 0;
        return;
    }

#if WC3_DEBUG
    if (ms == 0 || g_wait_no_block) {
#else
    if (ms == 0) {
#endif
        pthread_mutex_lock(&e->m);
        int closed = e->closing;
        int sig = e->signalled;
        if (sig && !e->manual) e->signalled = 0;
        pthread_mutex_unlock(&e->m);
        ev_unref(e);
        c->eax = closed ? 0u : (sig ? 0u : 258u);
#if WC3_DEBUG && WC3_DEBUG_HLE_SYNC
        sync_trace_wait1(h, ms, ra, c->eax);
#endif
        return;
    }

#if WC3_DEBUG && WC3_DEBUG_HLE_SLEEP
    sleep_trace_wait_cap("event", h, ms, ra, c->esp);
#endif
    if (ms < 0 || ms > WAIT_CAP_MS) ms = WAIT_CAP_MS;

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uint64_t end_ms = (uint64_t)start.tv_sec * 1000u +
                      (uint64_t)start.tv_nsec / 1000000u + (uint32_t)ms;

    int sig = 0;
    int closed = 0;
    pthread_mutex_lock(&e->m);
    for (;;) {
        if (e->closing) { closed = 1; break; }
        if (e->signalled) break;

        struct timespec now, slice;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_ms = (uint64_t)now.tv_sec * 1000u +
                          (uint64_t)now.tv_nsec / 1000000u;
        if (now_ms >= end_ms) break;

        uint64_t left = end_ms - now_ms;
        if (left > 10u) left = 10u;
        clock_gettime(CLOCK_REALTIME, &slice);
        slice.tv_sec  += (time_t)(left / 1000u);
        slice.tv_nsec += (long)(left % 1000u) * 1000000L;
        if (slice.tv_nsec >= 1000000000L) {
            slice.tv_sec++;
            slice.tv_nsec -= 1000000000L;
        }
        int rc = pthread_cond_timedwait(&e->cv, &e->m, &slice);
        if (e->closing) { closed = 1; break; }
        if (e->signalled) break;
    }
    if (!closed) {
        sig = e->signalled;
        if (sig && !e->manual) e->signalled = 0;
    }
    pthread_mutex_unlock(&e->m);
    ev_unref(e);

    if (closed) {
        /* The guest closed the handle while this thread was waiting.  Win32
         * leaves this undefined; returning WAIT_OBJECT_0 lets the audio thread
         * leave the mixer loop instead of touching the destroyed object. */
        c->eax = 0;
#if WC3_DEBUG && WC3_DEBUG_HLE_SYNC
        sync_trace_wait1(h, ms, ra, c->eax);
#endif
        return;
    }
    if (!sig) {
        if (g_wait_timeouts++ < 8)
            fprintf(stderr, "[hle] WaitForSingleObject(%08X) timed out after %d ms "
                            "ra=%08X esp=%08X\n",
                    h, (int)ms, ra, c->esp);
        /* A timeout is a *result*, not a failure: the guest is expected to
         * handle it.  This used to flip every later wait to pass-through after
         * the third one, which is what turned Storm's read-ahead thread into a
         * use-after-free - the cache teardown no longer waited for the reader
         * that still owned the block (SIGSEGV at 0x120A0000, "arena ...
         * FREED").  WC3_WAIT_NOBLOCK=1 still forces the old behaviour. */
        c->eax = 258;
#if WC3_DEBUG && WC3_DEBUG_HLE_SYNC
        sync_trace_wait1(h, ms, ra, c->eax);
#endif
        return;
    }
    c->eax = 0;
#if WC3_DEBUG && WC3_DEBUG_HLE_SYNC
    sync_trace_wait1(h, ms, ra, c->eax);
#endif
}
/* WaitForMultipleObjects: Miles' digital-driver output thread waits on
 * [shutdown/manual-reset event, mixer mutex] with bWaitAll = FALSE.  A stub
 * that returns 0 pretends the event is signalled, which is the thread's exit
 * condition, so the mixer never runs and no PCM is queued.  Track both object
 * kinds for real here.
 *
 * WAIT_OBJECT_0 + index is returned for the first signalled object; a mutex is
 * actually acquired (recursively, like Win32) before it is reported, because
 * the guest releases it after mixing.  Guest thread handles are signalled once
 * the worker has returned; other unsupported handles stay "always signalled",
 * matching h_wait_one's pass-through. */
enum { WK_EVENT, WK_MUTEX, WK_THREAD, WK_OTHER };

typedef struct {
    uint32_t h;
    int kind;
    GEvent *e;
    GMutex *mx;
    struct GThread *t;
} WaitItem;

static int wait_item_acquire(WaitItem *w)
{
    if (w->kind == WK_EVENT) {
        int sig;
        pthread_mutex_lock(&w->e->m);
        if (w->e->closing) {
            pthread_mutex_unlock(&w->e->m);
            return 1;          /* closed handle: report it as signalled */
        }
        sig = w->e->signalled;
        if (sig && !w->e->manual) w->e->signalled = 0;
        pthread_mutex_unlock(&w->e->m);
        return sig;
    }
    if (w->kind == WK_MUTEX) {
        int sig;
        pthread_mutex_lock(&w->mx->m);
        if (w->mx->closing) {
            pthread_mutex_unlock(&w->mx->m);
            return 1;          /* closed handle: let the waiter leave */
        }
        sig = !w->mx->owner_valid ||
              pthread_equal(w->mx->owner, pthread_self());
        if (sig) {
            w->mx->owner = pthread_self();
            w->mx->owner_valid = 1;
            w->mx->count++;
        }
        pthread_mutex_unlock(&w->mx->m);
        return sig;
    }
    if (w->kind == WK_THREAD)
        return guest_thread_is_done(w->t);
    return 1;
}

/* Undo a partial WaitAll acquisition.  Auto-reset events are reposted; mutex
 * recursion is unwound the same way ReleaseMutex does it. */
static void wait_item_release(WaitItem *w)
{
    if (w->kind == WK_EVENT) {
        pthread_mutex_lock(&w->e->m);
        if (!w->e->closing && !w->e->manual) w->e->signalled = 1;
        pthread_mutex_unlock(&w->e->m);
    } else if (w->kind == WK_MUTEX) {
        pthread_mutex_lock(&w->mx->m);
        if (!w->mx->closing && w->mx->owner_valid &&
            pthread_equal(w->mx->owner, pthread_self()) &&
            w->mx->count > 0) {
            if (--w->mx->count == 0) {
                w->mx->owner_valid = 0;
                pthread_cond_broadcast(&w->mx->cv);
            }
        }
        pthread_mutex_unlock(&w->mx->m);
    }
}

static void wait_items_unref(WaitItem *it, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (it[i].e) ev_unref(it[i].e);
        if (it[i].mx) mutex_unref(it[i].mx);
    }
}

static void h_wait_many(CPU *c)
{
    uint32_t n = arg32(c, 0);
    uint32_t hp = arg32(c, 1);
    int wait_all = arg32(c, 2) != 0;
    int32_t ms = (int32_t)arg32(c, 3);
    uint32_t ra = guest_ok(c->esp, 4) ? rd32g(c->esp) : 0;

    if (n == 0 || n > 64u || !guest_ok(hp, n * 4u)) {
#if WC3_DEBUG
        if (g_wait_many_log++ < 16)
            fprintf(stderr, "[hle] WaitForMultipleObjects bad args n=%u "
                            "handles=%08X all=%d ra=%08X\n",
                    n, hp, wait_all, ra);
#endif
        c->eax = 0xFFFFFFFFu;      /* WAIT_FAILED */
        return;
    }

    WaitItem it[64];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t h = rd32g(hp + i * 4u);
        it[i].h = h;
        it[i].e = ev_ref(h);
        it[i].mx = it[i].e ? NULL : mutex_ref(h);
        it[i].t = (it[i].e || it[i].mx) ? NULL : guest_thread_get(h);
        it[i].kind = it[i].e ? WK_EVENT :
                     (it[i].mx ? WK_MUTEX :
                      (it[i].t ? WK_THREAD : WK_OTHER));
    }

#if WC3_DEBUG && WC3_DEBUG_HLE
    watch_note_wait(ra, n > 0 ? it[0].h : 0, n > 1 ? it[1].h : 0, 1);
#endif

#if WC3_DEBUG && WC3_DEBUG_HLE_SYNC
    if (g_sync_trace && g_sync_trace_log < SYNC_TRACE_LIMIT) {
        int interesting = 0;
        for (uint32_t i = 0; i < n; i++) {
            const char *kind = sync_trace_kind(it[i].h);
            if (strcmp(kind, "event") && strcmp(kind, "mutex")) {
                interesting = 1;
                break;
            }
        }
        if (interesting) {
            g_sync_trace_log++;
            fprintf(stderr, "[sync] WaitForMultipleObjects n=%u all=%d ms=%d "
                            "ra=%08X tid=%lu", n, wait_all, (int)ms, ra, SYNC_TID);
            for (uint32_t i = 0; i < n; i++)
                fprintf(stderr, " h%u=%08X(kind=%s gid=%u)", i, it[i].h,
                        sync_trace_kind(it[i].h), sync_trace_gid(it[i].h));
            fputc('\n', stderr);
        }
    }
#endif

#if WC3_DEBUG
    int no_block = (ms == 0) || g_wait_no_block;
#else
    int no_block = (ms == 0);
#endif
    uint64_t deadline = 0;
    if (!no_block && ms > 0)
        deadline = (uint64_t)hle_now_ms() + (uint32_t)ms;

    for (;;) {
        if (!wait_all) {
            for (uint32_t i = 0; i < n; i++) {
                if (wait_item_acquire(&it[i])) {
#if WC3_DEBUG
                    if (g_wait_many_log++ < 16)
                        fprintf(stderr, "[hle] WaitForMultipleObjects n=%u "
                                        "all=0 -> %u h0=%08X h1=%08X ra=%08X\n",
                                n, i, n > 0 ? it[0].h : 0,
                                n > 1 ? it[1].h : 0, ra);
#endif
                    wait_items_unref(it, n);
                    c->eax = i;    /* WAIT_OBJECT_0 + index */
                    return;
                }
            }
        } else {
            uint32_t got = 0;
            while (got < n && wait_item_acquire(&it[got])) got++;
            if (got == n) {
#if WC3_DEBUG
                if (g_wait_many_log++ < 16)
                    fprintf(stderr, "[hle] WaitForMultipleObjects n=%u "
                                    "all=1 -> 0 ra=%08X\n", n, ra);
#endif
                wait_items_unref(it, n);
                c->eax = 0;        /* WAIT_OBJECT_0 */
                return;
            }
            while (got > 0) wait_item_release(&it[--got]);
        }

        if (no_block) {
            wait_items_unref(it, n);
            c->eax = 258;          /* WAIT_TIMEOUT */
            return;
        }

        if (deadline && (uint64_t)hle_now_ms() >= deadline) {
#if WC3_DEBUG
            if (g_wait_many_timeouts++ < 8)
                fprintf(stderr, "[hle] WaitForMultipleObjects n=%u all=%d "
                                "timed out after %d ms ra=%08X esp=%08X\n",
                        n, wait_all, (int)ms, ra, c->esp);
#endif
            wait_items_unref(it, n);
            c->eax = 258;
            return;
        }

        /* The service triple runs at the top of the next iteration, before
         * the wait is rechecked.  Running it here as well is redundant: it
         * adds up to three extra passes per 1 ms sleep without changing the
         * completion -> timer -> completion order. */
        usleep(1000);
    }
}
static void h_dup_handle(CPU *c)
{
    uint32_t out = arg32(c, 3);
    if (out && guest_ok(out, 4)) wr32g(out, arg32(c, 1));
    c->eax = 1;
}
static void h_release_sem(CPU *c)
{
    (void)c;
#if WC3_DEBUG && WC3_DEBUG_HLE_SYNC
    uint32_t h = arg32(c, 0);
    SYNC_TRACE("ReleaseSemaphore(h=%08X kind=%s count=%u prev=%08X) "
               "-> 1 (stub) tid=%lu\n",
               h, sync_trace_kind(h), arg32(c, 1), arg32(c, 2), SYNC_TID);
#endif
    c->eax = 1;
}
static void h_create_mutex(CPU *c)
{
    c->eax = mutex_new(arg32(c, 1) != 0);
}
static void h_open_mutex(CPU *c)
{
    (void)c;
    c->eax = mutex_new(0);
}
static void h_release_mutex(CPU *c)
{
    GMutex *mx = mutex_ref(arg32(c, 0));
    if (!mx) { c->eax = 0; return; }
    pthread_mutex_lock(&mx->m);
    if (!mx->closing && mx->owner_valid &&
        pthread_equal(mx->owner, pthread_self())) {
        if (--mx->count == 0) {
            mx->owner_valid = 0;
            pthread_cond_broadcast(&mx->cv);
        }
        c->eax = 1;
    } else {
        c->eax = 0;
    }
    pthread_mutex_unlock(&mx->m);
    mutex_unref(mx);
}
static void h_create_semaphore(CPU *c)
{
    uint32_t h = gh_new_sync();
#if WC3_DEBUG && WC3_DEBUG_HLE_SYNC
    sync_trace_mark(h, SYNC_TRACE_SEMAPHORE, 0);
    SYNC_TRACE("CreateSemaphoreA(init=%d max=%d name=%08X) -> h=%08X "
               "tid=%lu\n", (int)arg32(c, 1), (int)arg32(c, 2), arg32(c, 3),
               h, SYNC_TID);
#endif
    c->eax = h;
}
static void h_sleep(CPU *c)
{
    uint32_t ms = arg32(c, 0);
#if WC3_DEBUG && WC3_DEBUG_HLE_SLEEP
    sleep_trace_state();
    if (g_sleep_trace) {
        uint32_t ra = guest_ok(c->esp, 4) ? rd32g(c->esp) : 0;
        g_sleep_calls++;
        if (ms == 0) {
            g_sleep_zero_calls++;
            if (g_sleep_log++ < 256)
                fprintf(stderr, "[sleep] zero ra=%08X esp=%08X tid=%lu\n",
                        ra, c->esp, SLEEP_TID);
        }
        if ((g_sleep_calls & 4095u) == 0 && g_sleep_summary++ < 16)
            fprintf(stderr, "[sleep] calls=%llu zero=%u tid=%lu\n",
                    g_sleep_calls, g_sleep_zero_calls, SLEEP_TID);
    }
#endif
    usleep((useconds_t)(ms * 1000u));
}

static uint32_t hle_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u +
                      (uint64_t)ts.tv_nsec / 1000000u);
}

/* Win32's InterlockedCompareExchange(dest, exchange, comparand): the second
 * stack argument is the value to store, the third is the value to compare.
 * CDataRecycler's lock-free lists depend on this exact order; swapping the
 * two makes its push a silent no-op and sends Free into an endless growth
 * loop.  Use real atomics because the guest runs more than one thread. */
static void h_interlocked_xchg(CPU *c)
{
    uint32_t p = arg32(c, 0), v = arg32(c, 1);
    if (p && guest_ok(p, 4))
        c->eax = __atomic_exchange_n((uint32_t *)(uintptr_t)p, v, __ATOMIC_SEQ_CST);
    else c->eax = 0;
}
static void h_interlocked_cmp(CPU *c)
{
    uint32_t p = arg32(c, 0), x = arg32(c, 1), cmp = arg32(c, 2);
    if (p && guest_ok(p, 4)) {
        uint32_t old = __atomic_load_n((uint32_t *)(uintptr_t)p, __ATOMIC_SEQ_CST);
        while (old == cmp) {
            if (__atomic_compare_exchange_n((uint32_t *)(uintptr_t)p, &old, x, 0,
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                break;
        }
        c->eax = old;
    } else c->eax = 0;
}
static void h_interlocked_inc(CPU *c)
{
    uint32_t p = arg32(c, 0);
    if (p && guest_ok(p, 4))
        c->eax = __atomic_add_fetch((uint32_t *)(uintptr_t)p, 1, __ATOMIC_SEQ_CST);
    else c->eax = 0;
}
static void h_interlocked_dec(CPU *c)
{
    uint32_t p = arg32(c, 0);
    if (p && guest_ok(p, 4))
        c->eax = __atomic_sub_fetch((uint32_t *)(uintptr_t)p, 1, __ATOMIC_SEQ_CST);
    else c->eax = 0;
}

/* ---- time ---- */
static void h_get_tick_count(CPU *c)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    c->eax = (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
#if WC3_DEBUG
    {
        static int tick_n;
        if (tick_n++ < 24) {
            uint32_t ra = guest_ok(c->esp, 4) ? rd32g(c->esp) : 0;
            fprintf(stderr, "[hle] tick n=%d val=%u ra=%08X\n",
                    tick_n, c->eax, ra);
        }
    }
#endif
}
static void h_time_get_time(CPU *c) { h_get_tick_count(c); }
/* Both QPC entry points take an out pointer.  Returning the value only in
 * edx:eax is not the Win32 contract: Miles stores the frequency with
 * QueryPerformanceFrequency(&g_freq) and then divides by that cell, so leaving
 * it zero makes L_351020D0 compute 0 ms on every call and the timer queue
 * never advances. */
static void h_query_perf_counter(CPU *c)
{
    uint32_t p = arg32(c, 0);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t v = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (p && guest_ok(p, 8)) {
        wr32g(p, (uint32_t)v);
        wr32g(p + 4, (uint32_t)(v >> 32));
    }
    c->eax = 1;
    c->edx = 0;
#if WC3_DEBUG
    {
        static int qpc_n;
        static uint64_t qpc_last;
        if (qpc_n++ < 24) {
            uint32_t ra = guest_ok(c->esp, 4) ? rd32g(c->esp) : 0;
            fprintf(stderr, "[hle] qpc n=%d p=%08X lo=%08X hi=%08X d=%llu ra=%08X\n",
                    qpc_n, p, (uint32_t)v, (uint32_t)(v >> 32),
                    (unsigned long long)(v - qpc_last), ra);
            qpc_last = v;
        }
    }
#endif
}
static void h_query_perf_freq(CPU *c)
{
    uint32_t p = arg32(c, 0);
    if (p && guest_ok(p, 8)) {
        wr32g(p, 1000000000u);
        wr32g(p + 4, 0);
    }
    c->eax = 1;
    c->edx = 0;
}

static void h_get_system_time_as_file_time(CPU *c)
{
    uint32_t p = arg32(c, 0);
    if (p && guest_ok(p, 8)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        wr_filetime(p, ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull +
                       (uint64_t)(ts.tv_nsec / 100));
    }
    c->eax = 0;
}

static void sys_time_from_tm(uint32_t p, const struct tm *tm)
{
    if (!p || !guest_ok(p, 16)) return;
    wr16g(p + 0, (uint16_t)(tm->tm_year + 1900));
    wr16g(p + 2, (uint16_t)(tm->tm_mon + 1));
    wr16g(p + 4, (uint16_t)tm->tm_mday);
    wr16g(p + 6, (uint16_t)tm->tm_hour);
    wr16g(p + 8, (uint16_t)tm->tm_min);
    wr16g(p + 10, (uint16_t)tm->tm_sec);
    wr16g(p + 12, (uint16_t)tm->tm_wday);
    wr16g(p + 14, (uint16_t)tm->tm_yday);
}

static void h_get_system_time(CPU *c)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    sys_time_from_tm(arg32(c, 0), &tm);
    c->eax = 0;
}
static void h_get_local_time(CPU *c)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    sys_time_from_tm(arg32(c, 0), &tm);
    c->eax = 0;
}
static void h_get_time_zone_information(CPU *c)
{
    uint32_t p = arg32(c, 0);
    if (p && guest_ok(p, 172)) memset((void *)(uintptr_t)p, 0, 172);
    c->eax = 0;
}
static void h_filetime_to_system_time(CPU *c)
{
    uint32_t ft = arg32(c, 0), st = arg32(c, 1);
    if (!ft || !guest_ok(ft, 8)) { c->eax = 0; return; }
    uint64_t v = ((uint64_t)rd32g(ft + 4) << 32) | rd32g(ft);
    time_t t = (time_t)(v / 10000000ull) - 11644473600ll;
    struct tm tm;
    gmtime_r(&t, &tm);
    sys_time_from_tm(st, &tm);
    c->eax = 1;
}
static void h_filetime_to_local(CPU *c)
{
    uint32_t in = arg32(c, 0), out = arg32(c, 1);
    if (in && out && guest_ok(in, 8) && guest_ok(out, 8)) {
        wr32g(out, rd32g(in));
        wr32g(out + 4, rd32g(in + 4));
    }
    c->eax = 1;
}
static void h_systemtime_to_filetime(CPU *c)
{
    uint32_t st = arg32(c, 0), ft = arg32(c, 1);
    if (!st || !ft || !guest_ok(st, 16) || !guest_ok(ft, 8)) { c->eax = 0; return; }
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = rd16g(st + 0) - 1900;
    tm.tm_mon = rd16g(st + 2) - 1;
    tm.tm_mday = rd16g(st + 4);
    tm.tm_hour = rd16g(st + 6);
    tm.tm_min = rd16g(st + 8);
    tm.tm_sec = rd16g(st + 10);
    time_t t = timegm(&tm);
    wr_filetime(ft, filetime_from_time_t(t));
    c->eax = 1;
}
static void h_compare_filetime(CPU *c)
{
    uint32_t a = arg32(c, 0), b = arg32(c, 1);
    uint64_t va = ((uint64_t)rd32g(a + 4) << 32) | rd32g(a);
    uint64_t vb = ((uint64_t)rd32g(b + 4) << 32) | rd32g(b);
    c->eax = va < vb ? (uint32_t)-1 : (va > vb ? 1u : 0u);
}
static void h_get_date_format(CPU *c)
{
    uint32_t buf = arg32(c, 4), n = arg32(c, 5);
    const char *s = "MM/dd/yyyy";
    if (buf && n && guest_ok(buf, n)) {
        size_t len = strlen(s);
        if (len >= n) len = n - 1;
        memcpy((void *)(uintptr_t)buf, s, len + 1);
        c->eax = (uint32_t)len;
    } else c->eax = 0;
}
static void h_get_time_format(CPU *c)
{
    uint32_t buf = arg32(c, 4), n = arg32(c, 5);
    const char *s = "HH:mm:ss";
    if (buf && n && guest_ok(buf, n)) {
        size_t len = strlen(s);
        if (len >= n) len = n - 1;
        memcpy((void *)(uintptr_t)buf, s, len + 1);
        c->eax = (uint32_t)len;
    } else c->eax = 0;
}

/* ---- process / thread ---- */
static void h_get_current_process(CPU *c) { c->eax = 0xFFFFFFFFu; }
static void h_get_current_thread(CPU *c) { c->eax = 0xFFFFFFFEu; }
static void h_get_current_process_id(CPU *c) { c->eax = 1234; }
static void h_get_current_thread_id(CPU *c) { c->eax = 5678; }
static void h_is_debugger(CPU *c) { c->eax = 0; }

/* RaiseException(code, flags, nargs, args).
 *
 * The only raise on the audio path is the MSVC thread-naming exception
 * 0x406D1388: SetThreadName wraps it in __try/__except, so on a host without
 * a debugger the correct result is "continue after the call".  Returning
 * normally is that continuation, but the import must carry its real arity:
 * with argc=0 the stdcall cleanup popped only the return address, the SEH
 * epilogue then restored ebx/esi/edi from 16 bytes below their saves, and
 * the stream constructor returned 0 instead of its stream object. */
static void h_raise_exception(CPU *c)
{
    uint32_t code = arg32(c, 0);
#if WC3_DEBUG
    fprintf(stderr, "[hle] RaiseException code=%08X flags=%08X nargs=%u -> continue\n",
            code, arg32(c, 1), arg32(c, 2));
#endif
    (void)code;
    c->eax = 0;
}
static void h_unhandled_filter(CPU *c) { c->eax = 1; }
static void h_set_unhandled_filter(CPU *c) { c->eax = 0; }
static void h_terminate_process(CPU *c)
{
    fprintf(stderr, "[hle] TerminateProcess(code=%u)\n", arg32(c, 1));
    hle_android_report();
    _exit(3);
}
static void h_terminate_thread(CPU *c) { (void)c; c->eax = 1; }
/* A lifted thread cannot be stopped safely at an arbitrary instruction, so
 * SuspendThread is reported as success without stopping it.  The only guest
 * that depends on this is Miles' waveOut completion callback, which uses it to
 * keep its buffer queue stable; the real guest mutex around that queue now
 * provides the actual serialisation.  Returning failure here made the callback
 * skip the queue update entirely, so Miles never got its buffers back. */
static void h_suspend_thread(CPU *c) { (void)c; c->eax = 0; }
static void h_resume_thread(CPU *c) { (void)c; c->eax = 0; }
static void h_set_thread_priority(CPU *c) { (void)c; c->eax = 1; }
static void h_get_thread_priority(CPU *c) { (void)c; c->eax = 0; }
static void h_create_process(CPU *c) { (void)c; g_last_error = 2; c->eax = 0; }

/* Per-thread TLS block; defined with the thread layer below. */
static uint32_t *tls_slot(void);

/* The TLS calls carry an error-status contract the game *depends* on.
 * `0x6F096910` is the engine's own TlsGetValue wrapper and it reads:
 *
 *     esi = TlsGetValue(idx);
 *     if (GetLastError()) { report(0x57); return 0; }   // 0x57 = INVALID_PARAMETER
 *     return esi;
 *
 * so a stale non-zero last error left by *any* earlier import makes every TLS
 * lookup return 0.  The game then drops its per-thread array writes on the
 * floor (`0x6F06C1E0` is `blk = tls_get(i); if (blk) blk[i] = v;`) and later
 * dereferences a null slot.  Win32 documents the fix: these functions set
 * ERROR_SUCCESS on success and ERROR_INVALID_PARAMETER on a bad index. */
static void h_tls_alloc(CPU *c)
{
    for (int i = 0; i < 64; i++)
        if (!g_tls[i]) { g_tls[i] = 1; c->eax = (uint32_t)i; return; }
    g_last_error = 259;                 /* ERROR_NO_MORE_ITEMS */
    c->eax = 0xFFFFFFFFu;
}
static void h_tls_free(CPU *c)
{
    uint32_t i = arg32(c, 0);
    if (i < 64 && g_tls[i]) { g_tls[i] = 0; g_last_error = 0; c->eax = 1; return; }
    g_last_error = 87;                  /* ERROR_INVALID_PARAMETER */
    c->eax = 0;
}
static void h_tls_set(CPU *c)
{
    uint32_t idx = arg32(c, 0);
    if (idx < 64 && g_tls[idx]) {
        tls_slot()[idx] = arg32(c, 1);
        g_last_error = 0;
        c->eax = 1;
        return;
    }
    g_last_error = 87;
    c->eax = 0;
}
static void h_tls_get(CPU *c)
{
    uint32_t idx = arg32(c, 0);
    if (idx < 64 && g_tls[idx]) {
        c->eax = tls_slot()[idx];
        g_last_error = 0;
        return;
    }
    g_last_error = 87;
    c->eax = 0;
}

/* ---- module ---- */

/* System modules the launcher resolves by name.  A pseudo-HMODULE is enough:
 * GetProcAddress turns the pair back into an HLE thunk, so a CRT that looks up
 * an optional entry point gets a real (if unimplemented) function instead of a
 * null pointer. */
static const char *const g_sys_modules[] = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll",
    "msvcrt.dll", "shell32.dll", "winmm.dll", "wsock32.dll",
    "comctl32.dll", "imm32.dll", "comdlg32.dll", "wininet.dll",
    "msvcr80.dll", "blizzard.ax", "d3d9.dll",
};
#define SYS_MOD_BASE 0x00080000u

static int sys_module_index(uint32_t h)
{
    if (h < SYS_MOD_BASE) return -1;
    uint32_t i = (h - SYS_MOD_BASE) / 4u;
    if (i >= sizeof g_sys_modules / sizeof g_sys_modules[0]) return -1;
    return (int)i;
}

static uint32_t sys_module_handle(const char *name)
{
    for (unsigned i = 0; i < sizeof g_sys_modules / sizeof g_sys_modules[0]; i++)
        if (!strcasecmp(name, g_sys_modules[i])) return SYS_MOD_BASE + i * 4u;
    /* LoadLibraryA("user32") without the extension is the same request as
     * LoadLibraryA("user32.dll"); the game makes both forms while it probes for
     * optional APIs, and answering only the long one logged a "not found" that
     * looked like a missing module. */
    if (!strchr(name, '.')) {
        char full[64];
        if (snprintf(full, sizeof full, "%s.dll", name) < (int)sizeof full) {
            for (unsigned i = 0; i < sizeof g_sys_modules / sizeof g_sys_modules[0]; i++)
                if (!strcasecmp(full, g_sys_modules[i])) return SYS_MOD_BASE + i * 4u;
        }
    }
    return 0;
}

/* Modules the probe mapped for real.  The PE image lives in boot_probe.c, so
 * this table only carries an opaque pointer back to it. */
#define HLE_MOD_MAX 8
typedef struct {
    char       name[32];
    char       path[512];
    uint32_t   base;
    const void *img;
} HleMod;
static HleMod g_mods[HLE_MOD_MAX];
static int    g_mod_count;
static hle_dll_loader g_dll_load;
static hle_dll_export g_dll_export;
/* Set when game.dll has been mapped and published by the Android boot probe.
 * Before that point its data addresses are not safe to write. */
static int    g_game_module_mapped;

static HleMod *mod_by_name(const char *name);

void hle_set_dll_hooks(hle_dll_loader load, hle_dll_export exp)
{
    g_dll_load = load;
    g_dll_export = exp;
}

void hle_register_module(const char *name, uint32_t base, const void *img)
{
    if (!name || !base) return;
    HleMod *m = mod_by_name(name);
    if (!m) {
        if (g_mod_count >= HLE_MOD_MAX) return;
        m = &g_mods[g_mod_count++];
        snprintf(m->name, sizeof m->name, "%s", name);
    }
    m->base = base;
    m->img = img;
    /* Keep the on-disk path alive: pe_image::path points at a caller-local
     * buffer and is gone by the time Miles asks for its own module name. */
    m->path[0] = 0;
    const pe_image *pi = (const pe_image *)img;
    if (pi && pi->path && pi->path[0]) {
        char host[1024];
        if (pi->path[0] == '/') {
            snprintf(host, sizeof host, "%s", pi->path);
        } else {
            char cwd[512];
            if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
            snprintf(host, sizeof host, "%s/%s", cwd, pi->path);
        }
        posix_to_win(host, m->path, sizeof m->path);
    }
    /* The 1.27 client-rectangle fix writes a cached rectangle inside game.dll.
     * Remember that the image is mapped so a change before LoadLibrary completes
     * cannot touch an unmapped address. */
    if (!strcasecmp(name, "game.dll"))
        g_game_module_mapped = 1;
}

static HleMod *mod_by_name(const char *name)
{
    for (int i = 0; i < g_mod_count; i++)
        if (!strcasecmp(name, g_mods[i].name)) return &g_mods[i];
    return NULL;
}

static HleMod *mod_by_base(uint32_t base)
{
    for (int i = 0; i < g_mod_count; i++)
        if (g_mods[i].base == base) return &g_mods[i];
    return NULL;
}

static const char *mod_windows_path(uint32_t base)
{
    HleMod *m = mod_by_base(base);
    return (m && m->path[0]) ? m->path : NULL;
}

static void h_get_module_handle(CPU *c)
{
    const char *n = gstr(arg32(c, 0));
    if (!n[0]) { c->eax = GUEST_IMAGE_WAR3; return; }
    HleMod *m = mod_by_name(n);
    if (m) { c->eax = m->base; return; }
    if (!strcasecmp(n, "war3.exe")) { c->eax = GUEST_IMAGE_WAR3; return; }
    uint32_t h = sys_module_handle(n);
    if (h) { c->eax = h; return; }
    c->eax = 0;
}

static void h_load_library(CPU *c)
{
    const char *n = gstr(arg32(c, 0));
#if WC3_DEBUG
    if (strstr(n, ".m3d") || strstr(n, "Mssfast"))
        fprintf(stderr, "[find] LoadLibraryA(\"%s\")\n", n);
#endif

    HleMod *known = mod_by_name(n);
    if (known) { c->eax = known->base; return; }

    uint32_t h = sys_module_handle(n);
    if (h) { c->eax = h; return; }

    /* Cheap mode: stop at the launcher's own request and report, so a
     * regression in the war3.exe boot is still a one-minute test. */
    if (!strcasecmp(n, "game.dll")) {
        g_reached_game_dll = 1;
        if (g_stop_game_dll) {
#if WC3_DEBUG
            fprintf(stderr, "[hle] LoadLibraryA(\"game.dll\") ret=%08X\n", rd32g(c->esp));
#endif
            hle_android_report();
#if WC3_DEBUG
            printf("android boot probe: reached LoadLibraryA(\"game.dll\")\n");
            fflush(stdout);
#endif
            _exit(0);
        }
    }

    if (g_dll_load && strstr(n, ".")) {
        uint32_t base = 0;
        const void *img = NULL;
        if (g_dll_load(n, &base, &img) == 0) {
            hle_register_module(n, base, img);
            c->eax = base;
            return;
        }
    }

    fprintf(stderr, "[hle] LoadLibraryA(\"%s\") -> not found\n", n);
    g_last_error = 126;
    c->eax = 0;
}

static void h_free_library(CPU *c) { (void)c; c->eax = 1; }

static void h_get_proc_address(CPU *c)
{
    uint32_t hmod = arg32(c, 0);
    const char *name = gstr(arg32(c, 1));

    HleMod *m = mod_by_base(hmod);
    if (m) {
        uint16_t ord = 0;
        if (((uintptr_t)arg32(c, 1) >> 16) == 0) ord = (uint16_t)arg32(c, 1);
        uint32_t va = 0;
        if (g_dll_export) va = g_dll_export(m->img, name, ord);
#if WC3_DEBUG
        if (va) {
            fprintf(stderr, "[hle] GetProcAddress(%s, \"%s\") -> %08X\n",
                    m->name, name, va);
        } else if (name[0]) {
            fprintf(stderr, "[hle] GetProcAddress(%s, \"%s\") -> not exported\n",
                    m->name, name);
        }
#endif
        c->eax = va;
        return;
    }

    int idx = sys_module_index(hmod);
    if (idx < 0 || !name[0]) { c->eax = 0; return; }
    uint32_t t = hle_thunk(g_sys_modules[idx], name, 0);
    c->eax = t;
}

/* ---- strings / locale ---- */

/* The guest's ANSI code page is 1251 (GetACP below), and the load-game menu
 * stores/reads save and map names as UTF-8.  h_widechar_to_multibyte has
 * always produced UTF-8; the old read side widened every byte on its own, so
 * a UTF-8 name such as "об" (D0 BE D0 B1) appeared as the Latin-1 mojibake
 * "Ð¾Ð±".  Decode UTF-8 for the code pages the guest actually uses and keep
 * CP1251 as the ANSI fallback for non-UTF-8 data (for example a save copied
 * from a Windows profile). */
static const uint16_t wc3_cp1251_high[128] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x0098, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
};

static const uint16_t wc3_cp1252_high[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

static uint16_t ansi_to_unicode(uint32_t cp, uint8_t b)
{
    if (b < 0x80u) return b;
    if (cp == 1252u)
        return b < 0xA0u ? wc3_cp1252_high[b - 0x80u] : b;
    if (cp == 0u || cp == 1251u)
        return wc3_cp1251_high[b - 0x80u];
    return b;
}

/* Decode one UTF-8 code point.  Returns 0 for a malformed sequence; the
 * caller decides whether that is an error or a replacement character. */
static int utf8_decode_one(const uint8_t *s, uint32_t len,
                           uint16_t out[2], uint32_t *units,
                           uint32_t *used)
{
    uint8_t b = s[0];
    uint32_t cp, need;
    if (b < 0x80u) {
        out[0] = b;
        *units = 1;
        *used = 1;
        return 1;
    }
    if (b >= 0xC2u && b <= 0xDFu) {
        need = 2;
        cp = b & 0x1Fu;
    } else if (b >= 0xE0u && b <= 0xEFu) {
        need = 3;
        cp = b & 0x0Fu;
    } else if (b >= 0xF0u && b <= 0xF4u) {
        need = 4;
        cp = b & 0x07u;
    } else {
        return 0;
    }
    if (len < need) return 0;
    for (uint32_t i = 1; i < need; i++) {
        if ((s[i] & 0xC0u) != 0x80u) return 0;
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    if (need == 2 && cp < 0x80u) return 0;
    if (need == 3 && (cp < 0x800u ||
                      (cp >= 0xD800u && cp <= 0xDFFFu))) return 0;
    if (need == 4 && (cp < 0x10000u || cp > 0x10FFFFu)) return 0;
    if (cp >= 0x10000u) {
        uint32_t v = cp - 0x10000u;
        out[0] = (uint16_t)(0xD800u + (v >> 10));
        out[1] = (uint16_t)(0xDC00u + (v & 0x3FFu));
        *units = 2;
    } else {
        out[0] = (uint16_t)cp;
        *units = 1;
    }
    *used = need;
    return 1;
}

static int utf8_valid(const uint8_t *s, uint32_t len)
{
    uint32_t i = 0;
    while (i < len) {
        uint16_t wc[2];
        uint32_t units, used;
        if (!utf8_decode_one(s + i, len - i, wc, &units, &used))
            return 0;
        i += used;
    }
    return 1;
}

static uint32_t mb_to_wide(uint32_t cp, uint32_t flags, const uint8_t *s,
                           uint32_t len, uint32_t dst, uint32_t cap)
{
    int utf8 = 0;
    if (cp == 65001u) {
        utf8 = 1;
        if (!utf8_valid(s, len) && (flags & 8u))
            return 0;
    } else if (cp == 0u || cp == 1251u) {
        utf8 = utf8_valid(s, len);
    }
    int write = dst != 0 && cap != 0;
    uint32_t out = 0;
    for (uint32_t i = 0; i < len; ) {
        uint16_t wc[2];
        uint32_t units = 1;
        uint32_t used = 1;
        if (utf8) {
            if (!utf8_decode_one(s + i, len - i, wc, &units, &used)) {
                wc[0] = 0xFFFDu;
                units = 1;
                used = 1;
            }
        } else {
            wc[0] = ansi_to_unicode(cp, s[i]);
        }
        if (write) {
            for (uint32_t k = 0; k < units && out < cap; k++)
                wr16g(dst + out++ * 2, wc[k]);
            if (out >= cap) break;
        } else {
            out += units;
        }
        i += used;
    }
    return out;
}

static void h_multibyte_to_widechar(CPU *c)
{
    /* MultiByteToWideChar(cp, flags, src, cbMultiByte, dst, cchWideChar).
     * The guest writes save and map names as UTF-8; read them back as UTF-8
     * instead of widening each byte.  CP1251 remains the fallback for
     * non-UTF-8 ANSI data, so a Windows save copied to the device still
     * decodes. */
    uint32_t cp = arg32(c, 0), flags = arg32(c, 1), src = arg32(c, 2),
             n = arg32(c, 3), dst = arg32(c, 4), cap = arg32(c, 5);
    if (!src || !guest_ok(src, 1)) { c->eax = 0; return; }
    const uint8_t *s = (const uint8_t *)(uintptr_t)src;
    uint32_t len = (n == 0xFFFFFFFFu)
                       ? (uint32_t)strlen((const char *)s)
                       : n;
    if (len && !guest_ok(src, len)) { c->eax = 0; return; }
    if (cap > 0x7FFFFFFFu) { c->eax = 0; return; }
    uint32_t max_out = len < cap ? len : cap;
    if (dst && cap && max_out && !guest_ok(dst, max_out * 2)) {
        c->eax = 0;
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_HLE
    static unsigned mb2wc_logs;
    if (len && s[0] >= 0x80u && mb2wc_logs < 64) {
        mb2wc_logs++;
        fprintf(stderr,
                "[mb2wc] cp=%u flags=%u len=%u first=%02X%02X%02X%02X\n",
                cp, flags, len, s[0], len > 1 ? s[1] : 0,
                len > 2 ? s[2] : 0, len > 3 ? s[3] : 0);
    }
#endif
    c->eax = mb_to_wide(cp, flags, s, len, dst, cap);
}

static void h_widechar_to_multibyte(CPU *c)
{
    /* WideCharToMultiByte(cp, flags, src, cchWideChar, dst, cbMultiByte,
     *                     defaultChar, usedDefaultChar) - src is argument 2,
     * not 3.  Reading one slot late made src land on cchWideChar, so every
     * conversion returned 0 and wrote nothing; the launcher's fatal-error
     * dialog then displayed an empty message. */
    uint32_t cp = arg32(c, 0), src = arg32(c, 2), n = arg32(c, 3),
             dst = arg32(c, 4), cap = arg32(c, 5);
    (void)cp;
    if (!src || !guest_ok(src, 2)) { c->eax = 0; return; }
    uint32_t len = 0;
    if (n == 0xFFFFFFFFu) { while (rd16g(src + len * 2)) len++; }
    else len = n;
    if (!dst || !cap) { c->eax = len; return; }
    /* The host console is UTF-8, so anything above ASCII is encoded as UTF-8
     * rather than mapped to '?' - that keeps Cyrillic error text readable.
     *
     * Two rules the caller depends on, and the loop used to break both:
     *
     *  - a character is written when it *fits*, not when the buffer is at
     *    least four bytes bigger than what has been written.  The old guard
     *    was `o + 3 < cap`, so every buffer of 1-3 bytes produced no output at
     *    all and returned 0 - which is also the failure value.  ijl15's static
     *    CRT sizes its environment block from a one-character probe:
     *    __crtGetEnvironmentStringsA (0x600497D0) measured 1, allocated 1 and
     *    converted into 1, got 0 back, took the error branch at 0x60049889 and
     *    returned NULL.  _environ ([0x60053DF0]) stayed NULL, and the CRT's
     *    own walk of it at 0x600494E4 (`mov al, byte ptr [esi]`) faulted at
     *    address 0 - which is the whole round-7 crash.
     *  - the return value is the number of bytes written, and 0 has to mean
     *    "this failed", never "there was nothing to copy". */
    uint32_t o = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint16_t w = rd16g(src + i * 2);
        uint8_t b[3];
        uint32_t need;
        if (w < 0x80u) {
            b[0] = (uint8_t)w;
            need = 1;
        } else if (w < 0x800u) {
            b[0] = (uint8_t)(0xC0u | (w >> 6));
            b[1] = (uint8_t)(0x80u | (w & 0x3Fu));
            need = 2;
        } else {
            b[0] = (uint8_t)(0xE0u | (w >> 12));
            b[1] = (uint8_t)(0x80u | ((w >> 6) & 0x3Fu));
            b[2] = (uint8_t)(0x80u | (w & 0x3Fu));
            need = 3;
        }
        if (o + need > cap) break;
        for (uint32_t k = 0; k < need; k++) wr8g(dst + o + k, b[k]);
        o += need;
    }
    c->eax = o;
}

static void h_get_acp(CPU *c) { c->eax = 1251; }
static void h_get_user_default_lcid(CPU *c) { c->eax = 0x0419; }
static void h_get_system_default_langid(CPU *c) { c->eax = 0x0419; }
static void h_get_user_default_langid(CPU *c) { c->eax = 0x0419; }
/* Same LANGID as the lang-ID pair above: the desktop this recompilation is
 * checked against reports 0x0419.  The zero-arg ABI is what matters most - an
 * unbound import is treated as argc 0 and returns 0, which here is a valid
 * LANGID (neutral) and silently changes locale-dependent text. */
static void h_get_system_default_ui_language(CPU *c) { c->eax = 0x0419; }
static void h_get_user_default_ui_language(CPU *c) { c->eax = 0x0419; }

static void h_get_locale_info(CPU *c)
{
    uint32_t lcid = arg32(c, 0), type = arg32(c, 1), buf = arg32(c, 2), n = arg32(c, 3);
    (void)lcid;
    const char *s = "";
    switch (type & 0xFFFFu) {
    case 0x0002: s = "ru-RU"; break;      /* LOCALE_SLANGUAGE */
    case 0x0005: s = "Russian"; break;    /* LOCALE_SENGLANGUAGE */
    case 0x0059: s = "Russian"; break;    /* LOCALE_SNATIVELANGNAME */
    case 0x0001: s = "1251"; break;       /* LOCALE_SDECIMAL? keep simple */
    case 0x0003: s = ","; break;          /* LOCALE_SDECIMAL */
    case 0x000E: s = "."; break;          /* LOCALE_STHOUSAND */
    default: s = ""; break;
    }
    size_t len = strlen(s) + 1;
    if (buf && n && guest_ok(buf, n)) {
        size_t copy = len < n ? len : n;
        memcpy((void *)(uintptr_t)buf, s, copy);
        if (copy) ((char *)(uintptr_t)buf)[copy - 1] = 0;
    }
    c->eax = (uint32_t)len;
}

static void h_get_computer_name(CPU *c)
{
    uint32_t buf = arg32(c, 0), n = arg32(c, 1);
    const char *s = "WC3ANDROID";
    size_t len = strlen(s);
    if (buf && n && guest_ok(buf, n)) {
        size_t copy = len < n - 1 ? len : n - 1;
        memcpy((void *)(uintptr_t)buf, s, copy);
        ((char *)(uintptr_t)buf)[copy] = 0;
        if (n >= 4) wr32g(n, (uint32_t)copy);
    }
    c->eax = 1;
}

static void h_format_message(CPU *c)
{
    uint32_t buf = arg32(c, 4), n = arg32(c, 5), code = arg32(c, 0);
    char msg[128];
    snprintf(msg, sizeof msg, "error %u", code);
    if (buf && n && guest_ok(buf, n)) {
        size_t len = strlen(msg);
        if (len >= n) len = n - 1;
        memcpy((void *)(uintptr_t)buf, msg, len + 1);
        c->eax = (uint32_t)len;
    } else c->eax = 0;
}

/* ---- file mapping ----
 *
 * CreateFileMapping only records the file and the mapping length.  The view is
 * what owns guest VA, exactly as on Windows: MapViewOfFile allocates it (and
 * copies the requested range), UnmapViewOfFile gives it back, and CloseHandle
 * gives back a view that the guest forgot to unmap.  Both release paths go
 * through va_live_take so the recorded, page-rounded size is what is unmapped;
 * the old code either leaked the range (UnmapViewOfFile was a no-op) or
 * unmapped the bytes without returning them to the free list (CloseHandle). */
static GHandle *file_map_by_base(uint32_t base)
{
    if (!base) return NULL;
    for (int i = 0; i < GH_MAX; i++) {
        GHandle *m = &g_handles[i];
        if (m->used && m->kind == 4 && m->file_map_base == base) return m;
    }
    return NULL;
}

static void file_view_release(GHandle *m)
{
    if (!m || m->kind != 4 || !m->file_map_base) return;
    uint32_t n = va_live_take(m->file_map_base);
    if (!n) n = va_round(m->file_map_size);
    if (n) {
        guest_unmap(m->file_map_base, n);
        va_release(m->file_map_base, n);
    }
#if WC3_DEBUG
    if (g_va_trace)
        fprintf(stderr, "[va] unmap view %08X+%X\n", m->file_map_base, n);
#endif
    m->file_map_base = 0;
    m->file_map_size = 0;
}

static void h_create_file_mapping(CPU *c)
{
    uint32_t size = arg32(c, 3), hfile = arg32(c, 5);
    GHandle *g = gh_get(hfile);
    if (!g || g->fd < 0) { g_last_error = 6; c->eax = 0; return; }
    struct stat st;
    if (fstat(g->fd, &st) != 0) { c->eax = 0; return; }
    if (!size) size = (uint32_t)st.st_size;
    uint32_t h = gh_new(4);
    GHandle *m = gh_get(h);
    if (!m) { c->eax = 0; return; }
    /* The mapping outlives the file handle the guest passed in. */
    m->fd = dup(g->fd);
    if (m->fd < 0) { memset(m, 0, sizeof *m); c->eax = 0; return; }
    m->state = size;                /* mapping length, in bytes */
    c->eax = h;
}

static void h_map_view_of_file(CPU *c)
{
    GHandle *m = gh_get(arg32(c, 0));
    if (!m || m->kind != 4 || m->fd < 0) { g_last_error = 6; c->eax = 0; return; }
    if (m->file_map_base) { c->eax = m->file_map_base; return; }   /* one view */
    uint32_t off_hi = arg32(c, 2), off_lo = arg32(c, 3), want = arg32(c, 4);
    if (off_hi) { g_last_error = 87; c->eax = 0; return; }
    if (!want) {
        if (off_lo >= m->state) { g_last_error = 87; c->eax = 0; return; }
        want = m->state - off_lo;
    }
    uint32_t base = va_alloc(want);
    if (!base) { c->eax = 0; return; }
    ssize_t r = pread(m->fd, (void *)(uintptr_t)base, want, (off_t)off_lo);
    if (r < 0) {
        uint32_t n = va_live_take(base);
        if (!n) n = va_round(want);
        guest_unmap(base, n);
        va_release(base, n);
        c->eax = 0;
        return;
    }
    if ((uint32_t)r < want)
        memset((void *)(uintptr_t)(base + (uint32_t)r), 0, want - (uint32_t)r);
    m->file_map_base = base;
    m->file_map_size = want;
#if WC3_DEBUG
    if (g_va_trace)
        fprintf(stderr, "[va] map view %08X+%X off=%X\n", base, want, off_lo);
#endif
    c->eax = base;
}

static void h_unmap_view_of_file(CPU *c)
{
    file_view_release(file_map_by_base(arg32(c, 0)));
    c->eax = 1;
}

/* ---- IO completion: never used before the renderer ---- */
/* ---- I/O completion ports ----
 *
 * The engine runs its stream worker on a port.  `0x6F09FCC0(this, -1)` is that
 * worker's `wait()`: it calls GetQueuedCompletionStatus with INFINITE and then
 * uses the completion key **without re-checking the BOOL**, which is correct
 * Win32 behaviour only because the call really blocks until a packet arrives.
 * A stub that returns FALSE immediately therefore hands the worker an
 * uninitialised key, and it dereferences it (`mov eax, [ecx]` with ecx == -1).
 *
 * So the queue has to be real.  Our file I/O is synchronous, so the completion
 * is posted by ReadFile/WriteFile the moment the bytes are in the guest buffer,
 * which is exactly the packet an overlapped read would have produced.
 */
#define IOCP_MAX 8
#define IOCP_Q   1024

typedef struct { uint32_t bytes, key, ov; } IocpItem;

typedef struct {
    int used;
    int initialized;
    int closing;
    int refs;
    uint32_t handle;
    pthread_mutex_t m;
    pthread_cond_t cv;
    IocpItem q[IOCP_Q];
    unsigned head, tail, n;
} Iocp;

static Iocp g_iocp[IOCP_MAX];
static pthread_mutex_t g_iocp_lock = PTHREAD_MUTEX_INITIALIZER;

/* File handle -> port binding lives on the GHandle.  A side table keyed by the
 * guest handle value goes stale when the handle is closed and its slot is
 * reused: the completion then carries the previous file's key and the stream
 * worker dereferences a released object.  Keeping the association on the
 * handle also removes the 32-association ceiling.  With the old table full,
 * CreateIoCompletionPort still reported success, no completion was posted, and
 * the worker's Sleep(INFINITE) on the port never returned.
 *
 * A port outlives its guest handle while a worker is still inside
 * GetQueuedCompletionStatus.  The table lock protects lookup and the reference
 * count; the per-port mutex protects the queue.  CloseHandle marks the port
 * closing and broadcasts, but the slot is not reused until the last waiter has
 * dropped its reference. */
static Iocp *iocp_get(uint32_t handle)
{
    Iocp *found = NULL;
    Iocp *closing = NULL;
    pthread_mutex_lock(&g_iocp_lock);
    for (int i = 0; i < IOCP_MAX; i++) {
        Iocp *p = &g_iocp[i];
        if (!p->used || p->handle != handle) continue;
        if (!p->closing) { found = p; break; }
        if (!closing) closing = p;
    }
    if (!found) found = closing;
    if (found) found->refs++;
    pthread_mutex_unlock(&g_iocp_lock);
    return found;
}

static void iocp_put(Iocp *p)
{
    if (!p) return;
    pthread_mutex_lock(&g_iocp_lock);
    if (p->refs > 0) p->refs--;
    pthread_mutex_unlock(&g_iocp_lock);
}

/* Returns 1 when queued, 0 when the port is closing, -1 when the ring is full.
 * A completion is never silently dropped: the caller reports the full queue
 * and the stream worker's shutdown packet remains visible in the log. */
static int iocp_push(Iocp *p, uint32_t bytes, uint32_t key, uint32_t ov)
{
    int rc = 1;
    pthread_mutex_lock(&p->m);
    if (p->closing) {
        rc = 0;
    } else if (p->n >= IOCP_Q) {
        rc = -1;
    } else {
        p->q[p->tail].bytes = bytes;
        p->q[p->tail].key = key;
        p->q[p->tail].ov = ov;
        p->tail = (p->tail + 1) % IOCP_Q;
        p->n++;
        pthread_cond_signal(&p->cv);
    }
    pthread_mutex_unlock(&p->m);
#if WC3_DEBUG && WC3_DEBUG_HLE
    if (rc < 0) {
        static unsigned full_trace;
        if (full_trace++ < 16)
            fprintf(stderr, "[iocp] queue full port=%08X\n", p->handle);
    }
#endif
    return rc;
}

/* Called from ReadFile/WriteFile: complete the overlapped operation the way an
 * associated port expects, if this handle is bound to one. */
static void iocp_complete_file(uint32_t file, uint32_t bytes, uint32_t ov)
{
    GHandle *g = gh_get(file);
    if (!g || !g->iocp_port) {
#if WC3_DEBUG && WC3_DEBUG_HLE
        static unsigned no_port;
        if (g && g->save_trace && no_port++ < 32)
            fprintf(stderr, "[save] iocp complete file=%08X bytes=%u ov=%08X "
                            "-> no port\n", file, bytes, ov);
#endif
        return;
    }
    Iocp *p = iocp_get(g->iocp_port);
    if (!p) return;
#if WC3_DEBUG && WC3_DEBUG_HLE
    static unsigned push_trace;
    if (push_trace++ < 128)
        fprintf(stderr, "[iocp] push port=%08X key=%08X bytes=%u ov=%08X "
                        "save=%d\n", g->iocp_port, g->iocp_key, bytes, ov,
                g->save_trace);
#endif
    iocp_push(p, bytes, g->iocp_key, ov);
    iocp_put(p);
}

/* CloseHandle on a port wakes every blocked GetQueuedCompletionStatus so the
 * stream worker can leave its wait before the object that owns it is freed. */
static int iocp_close(uint32_t handle)
{
    Iocp *p = iocp_get(handle);
    if (!p) return 0;
    pthread_mutex_lock(&g_iocp_lock);
    pthread_mutex_lock(&p->m);
    p->closing = 1;
    pthread_cond_broadcast(&p->cv);
#if WC3_DEBUG && WC3_DEBUG_HLE
    unsigned pending = p->n;
#endif
    pthread_mutex_unlock(&p->m);
    pthread_mutex_unlock(&g_iocp_lock);
    iocp_put(p);
#if WC3_DEBUG && WC3_DEBUG_HLE
    static unsigned close_trace;
    if (close_trace++ < 16)
        fprintf(stderr, "[iocp] close port=%08X pending=%u\n", handle, pending);
#endif
    return 1;
}

static void h_create_iocp(CPU *c)
{
    uint32_t file = arg32(c, 0), port = arg32(c, 1), key = arg32(c, 2);
    if (file && port) {                     /* associate a file with a port */
        GHandle *g = gh_get(file);
        if (!g || g->kind != 1) {
            g_last_error = 6;               /* ERROR_INVALID_HANDLE */
            c->eax = 0;
            return;
        }
        g->iocp_port = port;
        g->iocp_key = key;
        g_last_error = 0;
        c->eax = port;
#if WC3_DEBUG && WC3_DEBUG_HLE
        static unsigned assoc_trace;
        if (assoc_trace++ < 128)
            fprintf(stderr, "[iocp] assoc file=%08X port=%08X key=%08X\n",
                    file, port, key);
#endif
        return;
    }

    pthread_mutex_lock(&g_iocp_lock);
    Iocp *p = NULL;
    for (int i = 0; i < IOCP_MAX; i++) {
        Iocp *q = &g_iocp[i];
        if (!q->used || (q->closing && q->refs == 0)) { p = q; break; }
    }
    if (!p) {
        pthread_mutex_unlock(&g_iocp_lock);
        g_last_error = 6;
        c->eax = 0;
        return;
    }
    if (!p->initialized) {
        pthread_mutex_init(&p->m, NULL);
        pthread_cond_init(&p->cv, NULL);
        p->initialized = 1;
    } else {
        /* The old handle was closed and no waiter holds a reference. */
        p->head = p->tail = p->n = 0;
    }
    p->used = 1;
    p->closing = 0;
    p->refs = 0;
    p->handle = gh_new_sync();
    pthread_mutex_unlock(&g_iocp_lock);
    g_last_error = 0;
    c->eax = p->handle;
#if WC3_DEBUG && WC3_DEBUG_HLE
    static unsigned create_trace;
    if (create_trace++ < 16)
        fprintf(stderr, "[iocp] create port=%08X slot=%d\n",
                p->handle, (int)(p - g_iocp));
#endif
}

/* The engine's stream worker calls GetQueuedCompletionStatus with a finite
 * timeout and then tests both the BOOL and the overlapped output.  Win32 leaves
 * the output slots unchanged on a timeout, but the guest stack slots holding
 * them are not initialised by the caller, so a stale overlapped pointer is
 * dereferenced when the call simply times out.  Zero the three outputs before
 * the wait: the success path overwrites them, and every FALSE return then
 * carries a safe value. */
static void iocp_clear_out(uint32_t pb, uint32_t pk, uint32_t pov)
{
    if (pb && guest_ok(pb, 4)) wr32g(pb, 0);
    if (pk && guest_ok(pk, 4)) wr32g(pk, 0);
    if (pov && guest_ok(pov, 4)) wr32g(pov, 0);
}

#if WC3_DEBUG && WC3_DEBUG_HLE
/* One bounded line per failed wait is enough to tell the three FALSE returns
 * apart: a bad/closed port, a zero timeout, and a timed wait that expired. */
static void iocp_trace_false(uint32_t h, uint32_t ms, const char *why)
{
    static unsigned n;
    if (n++ < 24)
        fprintf(stderr, "[iocp] get port=%08X ms=%u -> FALSE (%s)\n", h, ms, why);
}
#endif

static void h_get_queued(CPU *c)
{
    uint32_t h = arg32(c, 0), pb = arg32(c, 1), pk = arg32(c, 2),
             pov = arg32(c, 3), ms = arg32(c, 4);
    iocp_clear_out(pb, pk, pov);
    Iocp *p = iocp_get(h);
    if (!p) {
#if WC3_DEBUG && WC3_DEBUG_HLE
        iocp_trace_false(h, ms, "invalid");
#endif
        g_last_error = 6;
        c->eax = 0;
        return;
    }

    pthread_mutex_lock(&p->m);
    while (p->n == 0 && !p->closing) {
        if (ms == 0) {
            pthread_mutex_unlock(&p->m);
            iocp_put(p);
#if WC3_DEBUG && WC3_DEBUG_HLE
            iocp_trace_false(h, ms, "timeout");
#endif
            g_last_error = 258;             /* WAIT_TIMEOUT */
            c->eax = 0;
            return;
        }
        if (ms == 0xFFFFFFFFu) {            /* INFINITE: block, do not fake it */
            pthread_cond_wait(&p->cv, &p->m);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (time_t)(ms / 1000u);
            ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&p->cv, &p->m, &ts) == ETIMEDOUT && p->n == 0) {
                pthread_mutex_unlock(&p->m);
                iocp_put(p);
#if WC3_DEBUG && WC3_DEBUG_HLE
                iocp_trace_false(h, ms, "timeout");
#endif
                g_last_error = 258;
                c->eax = 0;
                return;
            }
        }
    }
    if (p->closing) {
        pthread_mutex_unlock(&p->m);
        iocp_put(p);
#if WC3_DEBUG && WC3_DEBUG_HLE
        iocp_trace_false(h, ms, "closing");
#endif
        g_last_error = 6;
        c->eax = 0;
        return;
    }
    IocpItem it = p->q[p->head];
    p->head = (p->head + 1) % IOCP_Q;
    p->n--;
    pthread_mutex_unlock(&p->m);
    iocp_put(p);
#if WC3_DEBUG && WC3_DEBUG_HLE
    {
        static unsigned gets;
        if (gets++ < 256)
            fprintf(stderr, "[iocp] get ok port=%08X key=%08X bytes=%u ov=%08X "
                            "ms=%u\n", h, it.key, it.bytes, it.ov, ms);
    }
#endif

    if (pb && guest_ok(pb, 4)) wr32g(pb, it.bytes);
    if (pk && guest_ok(pk, 4)) wr32g(pk, it.key);
    if (pov && guest_ok(pov, 4)) wr32g(pov, it.ov);
    g_last_error = 0;
    c->eax = 1;
}

static void h_post_queued(CPU *c)
{
    Iocp *p = iocp_get(arg32(c, 0));
    if (!p) { g_last_error = 6; c->eax = 0; return; }
    int rc = iocp_push(p, arg32(c, 1), arg32(c, 2), arg32(c, 3));
    iocp_put(p);
    if (rc == 1) {
        g_last_error = 0;
        c->eax = 1;
    } else {
        g_last_error = rc == 0 ? 6 : 8;     /* invalid handle / no memory */
        c->eax = 0;
    }
}

/* ------------------------------------------------------------------ */
/* MSVCR80                                                             */
/* ------------------------------------------------------------------ */

static void cr_fprintf(CPU *c)
{
    /* fprintf(stream, fmt, ...) - only ever used for diagnostics by the
     * launcher's CRT; format the first string argument and stop. */
#if WC3_DEBUG
    uint32_t fmt = arg32(c, 1);
    fprintf(stderr, "[crt] fprintf(\"%s\")\n", gstr(fmt));
#endif
    c->eax = 0;
}

/* A host FILE* is 64 bits wide on aarch64, so it cannot travel through a guest
 * register: the low half alone is not a valid pointer.  Every guest stream is
 * a 32-byte record in guest memory holding a magic value and a slot index, and
 * the slot table keeps the real stream - the same scheme the Windows host uses.
 * The record itself is opaque to the guest; it only ever hands it back. */
#define CR_FILE_MAGIC 0x47465354u      /* 'GFST' */
#define CR_FILE_SLOTS 64
static FILE *g_cr_file[CR_FILE_SLOTS];
static pthread_mutex_t g_cr_file_lock = PTHREAD_MUTEX_INITIALIZER;

static FILE *cr_file(uint32_t rec)
{
    if (!rec || !guest_ok(rec, 8) || rd32g(rec) != CR_FILE_MAGIC) return NULL;
    unsigned slot = rd32g(rec + 4);
    return slot < CR_FILE_SLOTS ? g_cr_file[slot] : NULL;
}

/* Takes ownership of `f` on success and on failure alike. */
static uint32_t cr_file_new(FILE *f)
{
    if (!f) return 0;
    pthread_mutex_lock(&g_cr_file_lock);
    for (unsigned i = 3; i < CR_FILE_SLOTS; i++) {
        if (g_cr_file[i]) continue;
        uint32_t rec = hle_alloc0(0x20);
        if (!rec) break;
        wr32g(rec, CR_FILE_MAGIC);
        wr32g(rec + 4, i);
        g_cr_file[i] = f;
        pthread_mutex_unlock(&g_cr_file_lock);
        return rec;
    }
    pthread_mutex_unlock(&g_cr_file_lock);
    fclose(f);
    return 0;
}

static void cr_file_release(uint32_t rec, unsigned slot)
{
    pthread_mutex_lock(&g_cr_file_lock);
    if (slot < CR_FILE_SLOTS && g_cr_file[slot]) g_cr_file[slot] = NULL;
    pthread_mutex_unlock(&g_cr_file_lock);
    wr32g(rec, 0);
}

static void cr_fopen(CPU *c)
{
    const char *name = gstr(arg32(c, 0)), *mode = gstr(arg32(c, 1));
    char host[512];
    win_path(name, host, sizeof host);
    FILE *f = fopen(host, mode);
    uint32_t rec = cr_file_new(f);
#if WC3_DEBUG
    fprintf(stderr, "[crt] fopen(\"%s\", \"%s\") -> %s rec=%08X\n", name, mode,
            f ? "ok" : "null", rec);
#endif
    c->eax = rec;
}

static void cr_fclose(CPU *c)
{
    uint32_t rec = arg32(c, 0);
    FILE *f = cr_file(rec);
    if (!f) { c->eax = 0xFFFFFFFFu; return; }
    unsigned slot = rd32g(rec + 4);
    int r = fclose(f);
    cr_file_release(rec, slot);
#if WC3_DEBUG
    fprintf(stderr, "[crt] fclose(%08X) -> %d\n", rec, r);
#endif
    c->eax = (uint32_t)r;
}

static void cr_fwrite(CPU *c)
{
    uint32_t buf = arg32(c, 0), size = arg32(c, 1), nmemb = arg32(c, 2);
    FILE *f = cr_file(arg32(c, 3));
    uint64_t total = (uint64_t)size * nmemb;
    size_t r = 0;
    if (f && size && nmemb && total <= 0x7FFFFFFFu && guest_ok(buf, (uint32_t)total))
        r = fwrite((const void *)(uintptr_t)buf, size, nmemb, f);
#if WC3_DEBUG
    fprintf(stderr, "[crt] fwrite(buf=%08X size=%u n=%u rec=%08X) -> %u\n",
            buf, size, nmemb, arg32(c, 3), (unsigned)r);
#endif
    c->eax = (uint32_t)r;
}

static void cr_fread(CPU *c)
{
    uint32_t buf = arg32(c, 0), size = arg32(c, 1), nmemb = arg32(c, 2);
    FILE *f = cr_file(arg32(c, 3));
    uint64_t total = (uint64_t)size * nmemb;
    size_t r = 0;
    if (f && size && nmemb && total <= 0x7FFFFFFFu && guest_ok(buf, (uint32_t)total))
        r = fread((void *)(uintptr_t)buf, size, nmemb, f);
    c->eax = (uint32_t)r;
}

static void cr_fseek(CPU *c)
{
    FILE *f = cr_file(arg32(c, 0));
    c->eax = f ? (uint32_t)fseek(f, (long)(int32_t)arg32(c, 1), (int)arg32(c, 2))
               : 0xFFFFFFFFu;
}

static void cr_ftell(CPU *c)
{
    FILE *f = cr_file(arg32(c, 0));
    c->eax = f ? (uint32_t)ftell(f) : 0xFFFFFFFFu;
}

static void cr_fflush(CPU *c)
{
    uint32_t rec = arg32(c, 0);
    if (!rec) { c->eax = (uint32_t)fflush(NULL); return; }
    FILE *f = cr_file(rec);
    c->eax = f ? (uint32_t)fflush(f) : 0xFFFFFFFFu;
}

static void cr_feof(CPU *c)
{
    FILE *f = cr_file(arg32(c, 0));
    c->eax = f ? (uint32_t)feof(f) : 0;
}

static void cr_ferror(CPU *c)
{
    FILE *f = cr_file(arg32(c, 0));
    c->eax = f ? (uint32_t)ferror(f) : 0;
}

/* fopen_s(FILE **pp, const char *name, const char *mode): same open, but the
 * result goes through an out-pointer and the return is an errno_t.  It also
 * has to be *bound*: an unknown import gets argc 0, so hle_call pops 4 bytes
 * instead of 16 and the caller's stack drifts on every call. */
static void cr_fopen_s(CPU *c)
{
    uint32_t pp = arg32(c, 0);
    const char *name = gstr(arg32(c, 1)), *mode = gstr(arg32(c, 2));
    char host[512];
    win_path(name, host, sizeof host);
    errno = 0;
    FILE *f = fopen(host, mode);
    int err = errno ? errno : 2;      /* 2 = ENOENT */
    uint32_t rec = cr_file_new(f);
#if WC3_DEBUG
    fprintf(stderr, "[crt] fopen_s(\"%s\", \"%s\") -> %s rec=%08X\n", name, mode,
            f ? "ok" : "null", rec);
#endif
    if (pp && guest_ok(pp, 4)) wr32g(pp, rec);
    c->eax = rec ? 0u : (uint32_t)err;
}

static void cr_setvbuf(CPU *c) { (void)c; c->eax = 0; }

/* _strupr(char *s): upper-cases in place and returns s.  Storm uses it on every
 * MPQ name it resolves, so an unbound import here (argc 0) leaks four bytes of
 * guest stack per call and the next epilogue returns into garbage. */
static void cr_strupr(CPU *c)
{
    uint32_t a = arg32(c, 0);
    uint32_t n = a ? guest_clamp(a, 0x100000u) : 0;
    for (uint32_t i = 0; i < n; i++) {
        char *p = (char *)(uintptr_t)(a + i);
        if (!*p) break;
        *p = (char)toupper((unsigned char)*p);
    }
    c->eax = a;
}

static void cr_strlwr(CPU *c)
{
    uint32_t a = arg32(c, 0);
    uint32_t n = a ? guest_clamp(a, 0x100000u) : 0;
    for (uint32_t i = 0; i < n; i++) {
        char *p = (char *)(uintptr_t)(a + i);
        if (!*p) break;
        *p = (char)tolower((unsigned char)*p);
    }
    c->eax = a;
}

/* strnlen(s, n): length of s, at most n.  The credits path measures a fixed
 * 0x104-byte map path with it; an unbound import returns 0, so the engine
 * treats every path as empty and the map never opens. */
static void cr_strnlen(CPU *c)
{
    uint32_t a = arg32(c, 0), max = arg32(c, 1), n = 0;
    uint32_t lim = a ? guest_clamp(a, max) : 0;
    for (; n < lim; n++) {
        const char *p = (const char *)(uintptr_t)(a + n);
        if (!*p) break;
    }
    c->eax = n;
}

/* The character classifiers return a non-zero flag, not the character, and the
 * guest compares against zero - so the exact value does not matter, but it
 * must not be 0 for a matching byte. */
static void cr_isdigit(CPU *c) { c->eax = isdigit((unsigned char)arg32(c, 0)) ? 1 : 0; }
static void cr_isalpha(CPU *c) { c->eax = isalpha((unsigned char)arg32(c, 0)) ? 1 : 0; }
static void cr_isspace(CPU *c) { c->eax = isspace((unsigned char)arg32(c, 0)) ? 1 : 0; }
/* iswspace takes a wide character (an int, not a byte).  The generic stub
 * answers 0, which reads as "this is not whitespace" for every input - a
 * plausible answer, and the wrong one exactly when the character is a space.
 * Reached from game.dll at 0x6F12C8C8 while it parses glue-screen text. */
static void cr_iswspace(CPU *c)
{
    uint32_t ch = arg32(c, 0);
    c->eax = (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' ||
              ch == '\f' || ch == '\r') ? 1u : 0u;
}
static void cr_toupper(CPU *c) { c->eax = (uint32_t)toupper((int)(unsigned char)arg32(c, 0)); }
static void cr_tolower(CPU *c) { c->eax = (uint32_t)tolower((int)(unsigned char)arg32(c, 0)); }

/* Numeric conversions must not silently answer 0: the game uses atol on values
 * it just read from a file, and a 0 there turns into a bad index far away. */
static void cr_atol(CPU *c) { c->eax = (uint32_t)(int32_t)strtol(gstr(arg32(c, 0)), NULL, 10); }
/* MSVCR80's rand(), not the host's.  game.dll uses srand/rand as a
 * deterministic key generator, not as a random source: L_6F3325C0 does
 * srand(0x150B) and fills a 20-byte buffer with rand()-derived characters, and
 * L_6F332630/L_6F332A90 decode the file header with exactly that buffer.  The
 * two halves only agree if both see the same sequence, so bionic's rand()
 * (a completely different generator) makes the decoder reject every font file.
 * MSVC's is the classic LCG below; srand(1); rand() == 41. */
static uint32_t g_rand_state = 1;
static void cr_srand(CPU *c)
{
    g_rand_state = arg32(c, 0);
    c->eax = 0;
}
static void cr_rand(CPU *c)
{
    g_rand_state = g_rand_state * 214013u + 2531011u;
    c->eax = (g_rand_state >> 16) & 0x7FFFu;
}
static void cr_strtol(CPU *c)
{
    const char *s = gstr(arg32(c, 0));
    uint32_t endp = arg32(c, 1);
    int base = (int)arg32(c, 2);
    char *end = NULL;
    long v = strtol(s, &end, base ? base : 10);
    if (endp && guest_ok(endp, 4)) wr32g(endp, (uint32_t)(uintptr_t)end);
    c->eax = (uint32_t)(int32_t)v;
}

/* UTF-16 guest string -> host UTF-8.  %S and %ls carry wide strings and the
 * game builds resource paths with them; treating one as narrow reads every
 * other byte as NUL and yields a one-character name. */
static size_t fmt_wide(char *out, size_t cap, uint32_t p)
{
    size_t o = 0;
    if (cap) out[0] = 0;
    if (!p) return 0;
    for (unsigned i = 0; i < 4096; i++) {
        if (!guest_ok(p + i * 2, 2)) break;
        uint32_t ch = rd16g(p + i * 2);
        if (!ch) break;
        char tmp[4];
        size_t n;
        if (ch < 0x80) { tmp[0] = (char)ch; n = 1; }
        else if (ch < 0x800) {
            tmp[0] = (char)(0xC0 | (ch >> 6));
            tmp[1] = (char)(0x80 | (ch & 0x3F));
            n = 2;
        } else {
            tmp[0] = (char)(0xE0 | (ch >> 12));
            tmp[1] = (char)(0x80 | ((ch >> 6) & 0x3F));
            tmp[2] = (char)(0x80 | (ch & 0x3F));
            n = 3;
        }
        if (o + n + 1 >= cap) break;
        memcpy(out + o, tmp, n);
        o += n;
    }
    if (cap) out[o] = 0;
    return o;
}

/* The one formatter behind sprintf / _vsnprintf / vsprintf / _snprintf.
 *
 * The varargs live on the guest stack (or behind a guest va_list) and %s
 * carries a *guest* pointer, so the host vsnprintf cannot be called with the
 * guest's list directly.  Guest memory is mapped 1:1, so only the *position*
 * of each argument has to be walked; the conversion itself is still the
 * host's.  `ap` is the guest address of the first vararg and is advanced as
 * arguments are consumed.
 *
 * Returns the length the whole string needs (snprintf semantics) and always
 * NUL-terminates when cap is non-zero.
 *
 * _vsnprintf used to copy the format string verbatim on the theory that "the
 * boot path only formats literals".  It does not: Storm's SStrPrintf is
 * MSVCR120!_vsnprintf with a va_list, so game.dll's team-colour table ended
 * up looked up under the literal name "%s%02u", the lookup returned NULL and
 * the caller dereferenced it (6F3537FD, `mov ecx, [eax]` with eax == 0). */
static size_t guest_format(char *out, size_t cap, const char *fmt, uint32_t ap)
{
    size_t o = 0;
    if (cap) out[0] = 0;

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { if (o + 1 < cap) out[o] = *p; o++; continue; }
        p++;
        if (*p == '%') { if (o + 1 < cap) out[o] = '%'; o++; continue; }

        char spec[28];
        size_t s = 0;
        int wide = 0, i64 = 0;
        spec[s++] = '%';
        while (*p && strchr("-+ #0", *p) && s < 10) spec[s++] = *p++;
        while (*p && isdigit((unsigned char)*p) && s < 14) spec[s++] = *p++;
        if (*p == '.') {
            spec[s++] = *p++;
            while (*p && isdigit((unsigned char)*p) && s < 18) spec[s++] = *p++;
        }
        /* Length modifiers.  `I64`/`L` take an 8-byte argument; `ll` is left
         * on the 4-byte path it has always used, because every caller that
         * relies on it is formatting a 32-bit value anyway. */
        for (;;) {
            if (*p == 'l') { wide = 1; p++; if (*p == 'l') p++; }
            else if (*p == 'h') { p++; if (*p == 'h') p++; }
            else if (*p == 'L') { i64 = 1; p++; }
            else if (*p == 'I' && p[1] == '6' && p[2] == '4') { i64 = 1; p += 3; }
            else if (*p == 'I' && p[1] == '3' && p[2] == '2') { p += 3; }
            else if (*p == 'z' || *p == 'I') { p++; }
            else break;
        }
        char conv = *p;
        if (!conv) break;
        if (conv == 'S') { conv = 's'; wide = 1; }

        char tmp[1024];
        tmp[0] = 0;
        switch (conv) {
        case 's':
            if (wide) {
                fmt_wide(tmp, sizeof tmp, rd32g(ap));
            } else {
                spec[s] = 's'; spec[s + 1] = 0;
                snprintf(tmp, sizeof tmp, spec, gstr(rd32g(ap)));
            }
            ap += 4;
            break;
        case 'c':
            spec[s] = 'c'; spec[s + 1] = 0;
            snprintf(tmp, sizeof tmp, spec, (int)rd32g(ap));
            ap += 4;
            break;
        case 'd': case 'i':
            spec[s] = 'l'; spec[s + 1] = 'l'; spec[s + 2] = 'd'; spec[s + 3] = 0;
            if (i64) {
                uint64_t v = (uint64_t)rd32g(ap) | ((uint64_t)rd32g(ap + 4) << 32);
                snprintf(tmp, sizeof tmp, spec, (long long)(int64_t)v);
                ap += 8;
            } else {
                snprintf(tmp, sizeof tmp, spec, (long long)(int32_t)rd32g(ap));
                ap += 4;
            }
            break;
        case 'u': case 'x': case 'X': case 'o':
            spec[s] = 'l'; spec[s + 1] = 'l'; spec[s + 2] = conv; spec[s + 3] = 0;
            if (i64) {
                uint64_t v = (uint64_t)rd32g(ap) | ((uint64_t)rd32g(ap + 4) << 32);
                snprintf(tmp, sizeof tmp, spec, (unsigned long long)v);
                ap += 8;
            } else {
                snprintf(tmp, sizeof tmp, spec, (unsigned long long)rd32g(ap));
                ap += 4;
            }
            break;
        case 'p':
            snprintf(tmp, sizeof tmp, "%08X", rd32g(ap));
            ap += 4;
            break;
        case 'f': case 'g': case 'e': case 'E': case 'G': {
            uint64_t bits = (uint64_t)rd32g(ap) | ((uint64_t)rd32g(ap + 4) << 32);
            double d;
            memcpy(&d, &bits, sizeof d);
            ap += 8;
            spec[s] = conv; spec[s + 1] = 0;
            snprintf(tmp, sizeof tmp, spec, d);
            break;
        }
        case 'n':
            ap += 4;
            continue;
        default:
            continue;
        }
        size_t tl = strlen(tmp);
        if (cap) {
            size_t room = (o + tl < cap) ? tl : (o < cap - 1 ? cap - 1 - o : 0);
            memcpy(out + o, tmp, room);
        }
        o += tl;
    }
    if (cap) out[o < cap ? o : cap - 1] = 0;
    return o;
}

/* sprintf(char *dst, const char *fmt, ...).  cdecl, so the varargs start at
 * esp+12: return address, dst, fmt.  Without a real formatter the launcher's
 * own error text is empty and a failure reads as MessageBoxA(""). */
static void cr_sprintf(CPU *c)
{
    uint32_t dst = arg32(c, 0);
    const char *fmt = gstr(arg32(c, 1));
    if (!dst || !fmt[0] || !guest_ok(dst, 1)) { c->eax = 0; return; }

    char out[4096];
    size_t n = guest_format(out, sizeof out, fmt, c->esp + 12);
    uint32_t room = guest_clamp(dst, (uint32_t)n + 1);
    if (room) {
        memcpy((void *)(uintptr_t)dst, out, room - 1);
        ((char *)(uintptr_t)dst)[room - 1] = 0;
    }
    c->eax = (uint32_t)n;
}

static void cr_strrchr(CPU *c)
{
    const char *s = gstr(arg32(c, 0));
    int ch = (int)arg32(c, 1);
    const char *p = strrchr(s, ch);
    c->eax = p ? (uint32_t)(uintptr_t)p : 0;
}

static void cr_strstr(CPU *c)
{
    const char *hay = gstr(arg32(c, 0)), *needle = gstr(arg32(c, 1));
    if (!hay[0]) { c->eax = 0; return; }
    const char *p = strstr(hay, needle);
    c->eax = p ? (uint32_t)(uintptr_t)p : 0;
}

/* strpbrk(s, accept).  Storm uses it to split a path on "\\/:" while it
 * formats a diagnostic (0x1501C28A).  An unimplemented import returned 0,
 * Storm stored that as the "next separator" and then read through it, which
 * is the `signal 11 at 0x21` crash - a NULL result here is not "no match" to
 * the caller, it is a pointer it will dereference. */
static void cr_strpbrk(CPU *c)
{
    const char *s = gstr(arg32(c, 0)), *accept = gstr(arg32(c, 1));
    if (!s[0] || !accept[0]) { c->eax = 0; return; }
    const char *p = strpbrk(s, accept);
    c->eax = p ? (uint32_t)(uintptr_t)p : 0;
}

static void cr_strspn(CPU *c)
{
    c->eax = (uint32_t)strspn(gstr(arg32(c, 0)), gstr(arg32(c, 1)));
}

static void cr_strcspn(CPU *c)
{
    c->eax = (uint32_t)strcspn(gstr(arg32(c, 0)), gstr(arg32(c, 1)));
}

/* _malloc_crt/_calloc_crt: the CRT's own heap.  They must really allocate;
 * a null here is a null dereference inside the CRT a few instructions later. */
static void cr_malloc_crt(CPU *c)
{
    uint32_t n = arg32(c, 0);
    c->eax = hle_guest_alloc(n ? n : 1);
}

static void cr_fullpath(CPU *c)
{
    uint32_t buf = arg32(c, 0), path = arg32(c, 1), maxlen = arg32(c, 2);
    char host[512], posix[1024], full[1024];
    win_path(gstr(path), host, sizeof host);
    if (host[0] == '/') snprintf(posix, sizeof posix, "%s", host);
    else {
        char cwd[512];
        if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
        snprintf(posix, sizeof posix, "%s/%s", cwd, host);
    }
    /* Windows semantics: the result is absolute and drive-qualified, and the
     * game cuts it at the last backslash to find its data directory. */
    posix_to_win(posix, full, sizeof full);
#if WC3_DEBUG
    fprintf(stderr, "[path] _fullpath(\"%s\") -> \"%s\" ra=%08X\n", gstr(path), full,
            rd32g(c->esp));
#endif
    size_t len = strlen(full);
    if (!buf || !maxlen || len + 1 > maxlen || !guest_ok(buf, maxlen)) {
        c->eax = 0;
        return;
    }
    memcpy((void *)(uintptr_t)buf, full, len + 1);
    c->eax = buf;
}

static void cr_strcpy_s(CPU *c)
{
    uint32_t dst = arg32(c, 0), cap = arg32(c, 1), src = arg32(c, 2);
    const char *s = gstr(src);
    size_t len = strlen(s);
    if (!dst || cap <= len || !guest_ok(dst, (uint32_t)len + 1)) { c->eax = 22; return; }
    memcpy((void *)(uintptr_t)dst, s, len + 1);
    c->eax = 0;
}

/* A refused range must not look like a completed copy: the guest checks the
 * return value, not the memory, so a silent skip is a data loss that surfaces
 * far away as "the mesh is empty".  Bounded, so a systematic refusal names its
 * caller in the first few lines instead of flooding the log. */
static void cr_refuse(CPU *c, const char *fn, uint32_t dst, uint32_t src,
                      uint32_t n)
{
    static int reps;
    if (reps >= 8) return;
    reps++;
    fprintf(stderr, "[hle] %s refused dst=%08X src=%08X n=%u ra=%08X\n",
            fn, dst, src, n, rd32g(c->esp));
}

static void cr_memmove(CPU *c)
{
    uint32_t dst = arg32(c, 0), src = arg32(c, 1), n = arg32(c, 2);
    if (dst && src && n && guest_ok(dst, n) && guest_ok(src, n))
        memmove((void *)(uintptr_t)dst, (void *)(uintptr_t)src, n);
    else if (n)
        cr_refuse(c, "memmove", dst, src, n);
    c->eax = dst;
}

static void cr_memcpy(CPU *c)
{
    uint32_t dst = arg32(c, 0), src = arg32(c, 1), n = arg32(c, 2);
    if (dst && src && n && guest_ok(dst, n) && guest_ok(src, n))
        memcpy((void *)(uintptr_t)dst, (void *)(uintptr_t)src, n);
    else if (n)
        cr_refuse(c, "memcpy", dst, src, n);
    c->eax = dst;
}

static void cr_memset(CPU *c)
{
    uint32_t dst = arg32(c, 0), v = arg32(c, 1), n = arg32(c, 2);
    if (dst && n && guest_ok(dst, n)) memset((void *)(uintptr_t)dst, (int)v, n);
    else if (n)
        cr_refuse(c, "memset", dst, 0, n);
    c->eax = dst;
}

static void cr_qsort(CPU *c)
{
    uint32_t base = arg32(c, 0), n = arg32(c, 1), width = arg32(c, 2), cmp = arg32(c, 3);
    if (!base || n < 2 || !width || width > 256 || !guest_ok(base, n * width)) { c->eax = 0; return; }
    /* The comparator is lifted guest code, so every pointer handed to it has to
     * be a *guest* address.  `tmp` used to be a host stack array and the low 32
     * bits of its host address are not mapped in the guest: the comparator read
     * `[arg1]` and died at 0xdd482870.  One 256-byte guest scratch per thread. */
    static THREAD_LOCAL uint32_t tmp_guest;
    if (!tmp_guest) tmp_guest = hle_alloc0(256);
    if (!tmp_guest) { c->eax = 0; return; }
    uint8_t *a = (uint8_t *)(uintptr_t)base;
    uint8_t *tmp = (uint8_t *)(uintptr_t)tmp_guest;
    for (uint32_t i = 1; i < n; i++) {
        memcpy(tmp, a + i * width, width);
        uint32_t j = i;
        while (j > 0) {
            uint32_t saved = c->esp;
            /* cdecl order: the last argument first, the return address last.
             * Pushing the filler first instead handed the comparator
             * (arg2, filler) and it dereferenced 0x7EFFFFF1 as a pointer. */
            push32(c, (uint32_t)(uintptr_t)(a + (j - 1) * width));
            push32(c, tmp_guest);
            push32(c, 0x7EFFFFF1u);      /* return slot; never jumped to */
            dispatch(c, cmp);
            c->esp = saved;
            int32_t r = (int32_t)c->eax;
            if (r <= 0) break;
            memcpy(a + j * width, a + (j - 1) * width, width);
            j--;
        }
        memcpy(a + j * width, tmp, width);
    }
    c->eax = 0;
}

static void cr_atof(CPU *c) { c->eax = 0; c->edx = 0; }
static void cr_strtoul(CPU *c)
{
    const char *s = gstr(arg32(c, 0));
    uint32_t endp = arg32(c, 1);
    char *end = NULL;
    unsigned long v = strtoul(s, &end, (int)arg32(c, 2));
    if (endp && guest_ok(endp, 4)) wr32g(endp, (uint32_t)(uintptr_t)end);
    c->eax = (uint32_t)v;
}
static void cr_atoi(CPU *c) { c->eax = (uint32_t)atoi(gstr(arg32(c, 0))); }

/* _itoa(value, buffer, radix).  The old missing binding returned 0 without
 * touching the caller's buffer; the caller then strcat-ed uninitialized stack
 * over the profile name and corrupted the object the next call walked. */
static void cr_itoa(CPU *c)
{
    int32_t v = (int32_t)arg32(c, 0);
    uint32_t buf = arg32(c, 1);
    int radix = (int)arg32(c, 2);
    char tmp[40];
    if (radix < 2 || radix > 36 || !buf) { c->eax = 0; return; }
    char *p = tmp + sizeof tmp;
    *--p = 0;
    uint32_t mag = v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    do {
        int d = (int)(mag % (uint32_t)radix);
        *--p = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        mag /= (uint32_t)radix;
    } while (mag);
    if (v < 0) *--p = '-';
    size_t n = strlen(p) + 1;
    if (!guest_ok(buf, (uint32_t)n)) { c->eax = 0; return; }
    memcpy((void *)(uintptr_t)buf, p, n);
    c->eax = buf;
}

/* _vsnprintf(dst, cap, fmt, va).  The fourth argument is the guest address of
 * the first vararg - which is exactly what a 32-bit va_list *is* - so it can
 * be handed to guest_format() unchanged.  Storm calls this on every formatted
 * string it builds (6856 calls in one boot), so it has to be a real
 * formatter: see guest_format(). */
static void cr_vsnprintf(CPU *c)
{
    uint32_t dst = arg32(c, 0), cap = arg32(c, 1), fmt = arg32(c, 2), va = arg32(c, 3);
    const char *f = gstr(fmt);
    if (!dst || !cap || !f[0]) { c->eax = 0; return; }

    char out[8192];
    size_t n = guest_format(out, sizeof out, f, va);
    uint32_t room = guest_clamp(dst, cap);
    if (room) {
        size_t w = (n < (size_t)room) ? n : (size_t)room - 1;
        memcpy((void *)(uintptr_t)dst, out, w);
        ((char *)(uintptr_t)dst)[w] = 0;
    }
    /* _vsnprintf returns the length the string *needs*, and Storm's caller
     * branches on `ret < size` to decide whether it was truncated. */
    c->eax = (uint32_t)n;
}

static void cr_getmainargs(CPU *c)
{
    uint32_t pargc = arg32(c, 0), pargv = arg32(c, 1), penvp = arg32(c, 2);
    /* The launcher reads its command line through __argv, not only through
     * GetCommandLineA.  Passing just argv[0] makes it ignore switches such as
     * -loadfile, so the map never starts. */
    const char *cl = g_cmdline_str ? gstr(g_cmdline_str) : "";
    const char *rest = cl;
    size_t exe_len = strlen(g_exe_path_win);
    if (*rest == '"') {
        /* Windows quotes an executable path that contains spaces.  Skip the
         * quoted argv[0]; otherwise the launcher's scanner stops at the space
         * in "Warcraft 3" before it reaches any switch. */
        const char *q = strchr(rest + 1, '"');
        rest = q ? q + 1 : rest + strlen(rest);
    } else if (exe_len && !strncmp(rest, g_exe_path_win, exe_len)) {
        rest += exe_len;
    } else {
        const char *sp = strchr(rest, ' ');
        rest = sp ? sp : rest + strlen(rest);
    }
    while (*rest == ' ' || *rest == '\t') rest++;

    char args[1024];
    snprintf(args, sizeof args, "%s", rest);
    char *tok[32];
    int ntok = 0;
    for (char *p = args; *p && ntok < (int)(sizeof tok / sizeof tok[0]); ) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        char *start;
        if (*p == '"') {
            start = ++p;
            while (*p && *p != '"') p++;
        } else {
            start = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        }
        if (*p) *p++ = 0;
        tok[ntok++] = start;
    }

    uint32_t argv = hle_alloc0((uint32_t)(ntok + 2) * 4);
    /* __argv[0] must be the Windows form too: the launcher derives its data
     * directory by cutting argv[0] at the last backslash, and a POSIX path
     * makes the whole string the "directory" ("/data/.../war3.exe/War3.mpq"). */
    wr32g(argv, hle_guest_strdup(g_exe_path_win));
    for (int i = 0; i < ntok; i++)
        wr32g(argv + (uint32_t)(i + 1) * 4, hle_guest_strdup(tok[i]));
    wr32g(argv + (uint32_t)(ntok + 1) * 4, 0);
    if (pargc && guest_ok(pargc, 4)) wr32g(pargc, (uint32_t)(ntok + 1));
    if (pargv && guest_ok(pargv, 4)) wr32g(pargv, argv);
    if (penvp && guest_ok(penvp, 4)) wr32g(penvp, 0);
#if defined(WC3_DEBUG) || defined(WC3_DEBUG_GRAPHICS)
    fprintf(stderr, "[cmdline] __getmainargs: %s (argc=%d)\n", cl, ntok + 1);
    for (int i = 0; i < ntok; i++)
        fprintf(stderr, "[cmdline] argv[%d]=%s\n", i + 1, tok[i]);
#endif
    c->eax = 0;
}

static void cr_time64(CPU *c)
{
    uint32_t p = arg32(c, 0);
    time_t t = time(NULL);
    if (p && guest_ok(p, 8)) {
        wr32g(p, (uint32_t)t);
        wr32g(p + 4, (uint32_t)((uint64_t)t >> 32));
    }
    c->eax = (uint32_t)t;
    c->edx = (uint32_t)((uint64_t)t >> 32);
}

static void cr_ctime64(CPU *c)
{
    uint32_t p = arg32(c, 0);
    time_t t = 0;
    if (p && guest_ok(p, 8)) t = (time_t)(((uint64_t)rd32g(p + 4) << 32) | rd32g(p));
    else t = time(NULL);
    char buf[32];
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, sizeof buf, "%a %b %d %H:%M:%S %Y\n", &tm);
    c->eax = hle_guest_strdup(buf);
}

/* ---- guest threads -------------------------------------------------- */
/*
 * game.dll starts worker threads during GameMain, and the launcher treats a
 * NULL handle as a fatal error, so this cannot stay a stub.  Each guest thread
 * gets its own CPU, stack and TEB from the fixed arenas in host.h and runs the
 * lifted code through the same dispatcher as the main thread.
 */

typedef struct GThread {
    int          used;
    uint32_t     guest_id;
    uint32_t     handle;          /* Win32 thread handle returned by CreateThread */
    uint32_t     teb, stack_base, stack_size;
    uint32_t     start, arg;
    pthread_t    host;
    volatile int running;
    uint32_t     exit_code;
    uint32_t     tls[64];
#if WC3_DEBUG && WC3_DEBUG_HLE
    CPU             *cpu;
    volatile uint32_t last_target, last_eip, last_esp, last_ebp;
    volatile uint32_t dispatch_count;
    volatile uint32_t wait_ra, wait_h0, wait_h1;
    volatile int wait_active;
#endif
} GThread;

static GThread g_threads[GUEST_THREAD_MAX];
static pthread_mutex_t g_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_next_thread_id = 0x1000;

/* Thread handles are looked up by the Win32 handle stored in the arena slot.
 * A slot stays reserved after the thread exits (used is never cleared), so the
 * pointer cannot be recycled underneath a waiter. */
static struct GThread *guest_thread_get(uint32_t h)
{
    struct GThread *found = NULL;
    if (!h) return NULL;
    pthread_mutex_lock(&g_thread_lock);
    for (int i = 0; i < GUEST_THREAD_MAX; i++) {
        GThread *t = &g_threads[i];
        if (t->used && __atomic_load_n(&t->handle, __ATOMIC_ACQUIRE) == h) {
            found = t;
            break;
        }
    }
    pthread_mutex_unlock(&g_thread_lock);
    return found;
}

static int guest_thread_is_done(struct GThread *t)
{
    if (!t) return 1;
    return !__atomic_load_n(&t->running, __ATOMIC_ACQUIRE);
}

/* WaitForSingleObject on a thread handle.  The handle is signalled when the
 * guest thread returns, so this must actually wait instead of reporting every
 * unknown handle as WAIT_OBJECT_0.  INFINITE is capped like the other waits. */
static int guest_thread_wait(struct GThread *t, int32_t ms)
{
    if (!t) return 1;
    if (ms < 0 || ms > WAIT_CAP_MS) ms = WAIT_CAP_MS;
    if (guest_thread_is_done(t)) return 1;
    if (ms == 0) return 0;
    uint32_t start = hle_now_ms();
    for (;;) {
        if (guest_thread_is_done(t)) return 1;
        if ((uint32_t)(hle_now_ms() - start) >= (uint32_t)ms) return 0;
        usleep(1000);
    }
}

static uint32_t hle_current_guest_id(void)
{
    return g_self_thread ? g_self_thread->guest_id : 0;
}

/* The main thread has no GThread slot; its TLS lives here. */
static __thread uint32_t  g_main_tls[64];

static uint32_t *tls_slot(void)
{
    return g_self_thread ? g_self_thread->tls : g_main_tls;
}

static void teb_init_local(CPU *c, uint32_t teb, uint32_t stack_base,
                           uint32_t stack_size, uint32_t tid)
{
    memset((void *)(uintptr_t)teb, 0, 0x1000);
    wr32g(teb + 0x00, 0xFFFFFFFFu);              /* SEH chain end */
    wr32g(teb + 0x04, stack_base + stack_size);  /* StackBase */
    wr32g(teb + 0x08, stack_base);               /* StackLimit */
    wr32g(teb + 0x18, teb);                      /* self */
    wr32g(teb + 0x20, 1234);                     /* process id */
    wr32g(teb + 0x24, tid);                      /* thread id */
    wr32g(teb + 0x34, 0);                        /* LastErrorValue */
    c->fs_base = teb;
    c->fs = 0x53;
    c->ds = c->es = c->ss = 0x2B;
    c->cs = 0x23;
}

#if WC3_DEBUG && WC3_DEBUG_HLE
/* Live CPU pointer, published for the watchdog only.  The pointed-to CPU is
 * owned by the thread running it; the watchdog may read its transient eip to
 * catch a guest loop that never reaches another dispatch. */
static CPU *g_main_cpu;

/* Called by the active boot dispatcher for every guest target.  Keep this
 * separate from wc3_dispatch_watch(), whose old-tree probes are far too heavy
 * to run on every dispatch. */
void wc3_watch_dispatch(CPU *c, uint32_t target)
{
    if (g_self_thread) {
        g_self_thread->cpu = c;
        /* A new dispatch means the thread left whatever HLE wait it was in;
         * the next wait will set this again. */
        g_self_thread->wait_active = 0;
        g_self_thread->last_target = target;
        g_self_thread->last_eip = c->eip;
        g_self_thread->last_esp = c->esp;
        g_self_thread->last_ebp = c->ebp;
        g_self_thread->dispatch_count++;
    } else {
        g_main_cpu = c;
        g_main_wait_active = 0;
        g_main_last_target = target;
        g_main_last_eip = c->eip;
        g_main_last_esp = c->esp;
        g_main_last_ebp = c->ebp;
        g_main_dispatch_count++;
    }
}

static void watch_note_wait(uint32_t ra, uint32_t h0, uint32_t h1, int active)
{
    if (g_self_thread) {
        g_self_thread->wait_ra = ra;
        g_self_thread->wait_h0 = h0;
        g_self_thread->wait_h1 = h1;
        g_self_thread->wait_active = active;
    } else {
        g_main_wait_ra = ra;
        g_main_wait_h0 = h0;
        g_main_wait_h1 = h1;
        g_main_wait_active = active;
    }
}

int wc3_watch_state_get(uint32_t gid, Wc3WatchState *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    out->gid = gid;

    for (int i = 0; i < GUEST_THREAD_MAX; i++) {
        GThread *t = &g_threads[i];
        if (!t->used || t->guest_id != gid) continue;
        out->valid = 1;
        out->running = t->running;
        out->exit_code = t->exit_code;
        out->last_target = t->last_target;
        out->last_eip = t->last_eip;
        out->last_esp = t->last_esp;
        out->last_ebp = t->last_ebp;
        out->dispatch_count = t->dispatch_count;
        out->wait_active = t->wait_active;
        out->wait_ra = t->wait_ra;
        out->wait_h0 = t->wait_h0;
        out->wait_h1 = t->wait_h1;
        return 1;
    }

    /* The main guest thread has no GThread slot; boot_probe.c assigns it TEB
     * id 5678 and the watchdog keeps its state in the g_main_* globals. */
    if (gid == 5678u) {
        out->valid = 1;
        out->main_thread = 1;
        out->running = 1;
        out->last_target = g_main_last_target;
        out->last_eip = g_main_last_eip;
        out->last_esp = g_main_last_esp;
        out->last_ebp = g_main_last_ebp;
        out->dispatch_count = g_main_dispatch_count;
        out->wait_active = g_main_wait_active;
        out->wait_ra = g_main_wait_ra;
        out->wait_h0 = g_main_wait_h0;
        out->wait_h1 = g_main_wait_h1;
        return 1;
    }
    return 0;
}

static int hang_watch_enabled(void)
{
    if (getenv("WC3_HANG_WATCH")) return 1;
    const char *dir = getenv("WC3_DATA_DIR");
    char trigger[1200];
    snprintf(trigger, sizeof trigger, "%s/hang_watch_on",
             (dir && *dir) ? dir : ".");
    return access(trigger, F_OK) == 0;
}

static uint32_t cpu_live_eip(CPU *c)
{
    return c ? __atomic_load_n(&c->eip, __ATOMIC_RELAXED) : 0;
}

/* The last 64 HLE entries are a global ring, so a stall dump shows which host
 * calls the guest made immediately before it stopped dispatching.  This is
 * cheaper than hle_android_report() and does not reset the call counters. */
static void hle_dump_ring(const char *why)
{
    fprintf(stderr, "[hle] ring %s len=%u\n", why, g_ring_len);
    for (unsigned i = 0; i < g_ring_len; i++) {
        unsigned p = (g_ring_pos + HLE_RING - g_ring_len + i) % HLE_RING;
        fprintf(stderr, "[hle]   %s ra=%08X esp=%08X\n",
                g_ring[p].name, g_ring[p].ra, g_ring[p].esp);
    }
}

static void *hang_watch_proc(void *arg)
{
    (void)arg;
    /* 30 minutes at 2 s.  The previous 3-minute window expired before the
     * player reached the save dialog, leaving the hang with no state at all. */
    uint32_t prev_main_n = 0;
    int stalled = 0;
    for (int i = 0; i < 900; i++) {
        usleep(2000000);
        CPU *mc = __atomic_load_n(&g_main_cpu, __ATOMIC_RELAXED);
        uint32_t mn = __atomic_load_n(&g_main_dispatch_count, __ATOMIC_RELAXED);
        if (mn == prev_main_n) stalled++; else { stalled = 0; prev_main_n = mn; }
        const char *mn_name = hle_describe(g_main_last_target);
        if (stalled == 5 && !g_main_wait_active) {
            fprintf(stderr, "[watch] STALL t=%u n=%u pc=%08X last=%08X(%s) "
                            "esp=%08X ebp=%08X\n",
                    hle_now_ms(), mn, cpu_live_eip(mc), g_main_last_target,
                    mn_name[0] ? mn_name : "-", g_main_last_esp, g_main_last_ebp);
            hle_dump_ring("main stall");
        }
        fprintf(stderr, "[watch] t=%u main last=%08X(%s) pc=%08X eip=%08X esp=%08X "
                        "ebp=%08X n=%u wait=%d ra=%08X h0=%08X h1=%08X stall=%d\n",
                hle_now_ms(), g_main_last_target, mn_name[0] ? mn_name : "-",
                cpu_live_eip(mc),
                g_main_last_eip, g_main_last_esp, g_main_last_ebp, mn,
                g_main_wait_active, g_main_wait_ra, g_main_wait_h0,
                g_main_wait_h1, stalled);
        for (int j = 0; j < GUEST_THREAD_MAX; j++) {
            GThread *t = &g_threads[j];
            if (!t->used) continue;
            CPU *tc = __atomic_load_n(&t->cpu, __ATOMIC_RELAXED);
            const char *tn = hle_describe(t->last_target);
            fprintf(stderr, "[watch] gid=%u run=%d last=%08X(%s) pc=%08X eip=%08X "
                            "esp=%08X ebp=%08X n=%u wait=%d ra=%08X "
                            "h0=%08X h1=%08X\n",
                    t->guest_id, t->running, t->last_target, tn[0] ? tn : "-",
                    cpu_live_eip(tc),
                    t->last_eip, t->last_esp, t->last_ebp, t->dispatch_count,
                    t->wait_active, t->wait_ra, t->wait_h0, t->wait_h1);
        }
    }
    return NULL;
}

static void hang_watch_start(void)
{
    static int started;
    if (started || !hang_watch_enabled()) return;
    started = 1;
    /* The generated dispatcher only calls wc3_dispatch_watch() when this is
     * set.  Without it the watchdog cannot see a guest EIP at all. */
    g_dispatch_diag = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, hang_watch_proc, NULL) == 0)
        pthread_detach(th);
}
#endif

static void *guest_thread_proc(void *p)
{
    GThread *t = (GThread *)p;
    g_self_thread = t;

    CPU c;
    memset(&c, 0, sizeof c);
    cpu_x87_init(&c);
    c.esp = t->stack_base + t->stack_size - 0x100;
    teb_init_local(&c, t->teb, t->stack_base, t->stack_size, t->guest_id);
#if WC3_DEBUG && WC3_DEBUG_HLE
    t->cpu = &c;
#endif

    /* `call start(arg)`: the argument goes first, then the return address. */
    push32(&c, t->arg);
    push32(&c, 0x7EFFFFF0u);
    dispatch(&c, t->start);
    t->exit_code = c.eax;
    __atomic_store_n(&t->running, 0, __ATOMIC_RELEASE);
#if WC3_DEBUG && WC3_DEBUG_HLE
    t->cpu = NULL;
#endif
    g_self_thread = NULL;
    return NULL;
}

/* Shared by CreateThread and _beginthreadex.  Returns a guest handle, or 0 on
 * failure, which is the Win32 failure value for both. */
static uint32_t guest_thread_spawn(uint32_t start, uint32_t arg, uint32_t flags,
                                   uint32_t thrdaddr, const char *who)
{
    (void)flags;   /* CREATE_SUSPENDED is not modelled; the thread starts now */

    GThread *t = NULL;
    pthread_mutex_lock(&g_thread_lock);
    for (int i = 0; i < GUEST_THREAD_MAX; i++) {
        if (!g_threads[i].used) {
            t = &g_threads[i];
            memset(t, 0, sizeof *t);
            t->used = 1;
            t->guest_id = g_next_thread_id;
            g_next_thread_id += 4;
            t->teb = GUEST_THREAD_TEB_BASE + (uint32_t)i * 0x1000u;
            t->stack_base = GUEST_THREAD_STACK_BASE +
                            (uint32_t)i * GUEST_THREAD_STACK_SIZE;
            t->stack_size = GUEST_THREAD_STACK_SIZE;
            t->start = start;
            t->arg = arg;
            t->exit_code = 0x103u;   /* STILL_ACTIVE */
            __atomic_store_n(&t->running, 1, __ATOMIC_RELEASE);
            break;
        }
    }
    pthread_mutex_unlock(&g_thread_lock);

    if (!t) {
        fprintf(stderr, "[hle] %s: thread arena exhausted\n", who);
        return 0;
    }
    if (!guest_map(t->teb, 0x1000) ||
        !guest_map(t->stack_base, t->stack_size)) {
        fprintf(stderr, "[hle] %s: cannot map the thread arena\n", who);
        t->used = 0;
        return 0;
    }
    if (pthread_create(&t->host, NULL, guest_thread_proc, t) != 0) {
        fprintf(stderr, "[hle] %s: pthread_create failed\n", who);
        t->used = 0;
        return 0;
    }
    if (thrdaddr && guest_ok(thrdaddr, 4)) wr32g(thrdaddr, t->guest_id);

    uint32_t h = gh_new_sync();
    __atomic_store_n(&t->handle, h, __ATOMIC_RELEASE);
#if WC3_DEBUG && WC3_DEBUG_HLE_SYNC
    sync_trace_mark(h, SYNC_TRACE_THREAD, t->guest_id);
    SYNC_TRACE("CreateThread(%s start=%08X arg=%08X) -> h=%08X gid=%u "
               "tid=%lu\n", who, start, arg, h, t->guest_id, SYNC_TID);
#endif
#if WC3_DEBUG
    fprintf(stderr, "[hle] %s(start=%08X arg=%08X) -> h=%08X gid=%u tid=%lu\n",
            who, start, arg, h, t->guest_id, (unsigned long)t->host);
#endif
    return h;
}

static void h_create_thread(CPU *c)
{
    c->eax = guest_thread_spawn(arg32(c, 2), arg32(c, 3), arg32(c, 4),
                                arg32(c, 5), "CreateThread");
}

/* ---- critical sections ---------------------------------------------- */
/*
 * With real worker threads a no-op lock lets them corrupt shared state, so the
 * guest's CRITICAL_SECTION address is mapped to a recursive pthread mutex.
 * The mapping is kept in a side table rather than in the guest's 24 bytes: the
 * guest owns that memory and writing a host pointer into it would break as
 * soon as the CRT initialises the structure itself.
 */
#define CS_MAX 512
static struct { uint32_t addr; pthread_mutex_t *m; } g_cs[CS_MAX];
static int g_cs_count;
static pthread_mutex_t g_cs_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *cs_get(uint32_t addr, int create)
{
    if (!addr) return NULL;
    pthread_mutex_lock(&g_cs_lock);
    for (int i = 0; i < g_cs_count; i++) {
        if (g_cs[i].addr == addr) {
            pthread_mutex_t *m = g_cs[i].m;
            pthread_mutex_unlock(&g_cs_lock);
            return m;
        }
    }
    pthread_mutex_t *m = NULL;
    if (create && g_cs_count < CS_MAX) {
        m = (pthread_mutex_t *)malloc(sizeof *m);
        if (m) {
            pthread_mutexattr_t a;
            pthread_mutexattr_init(&a);
            pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(m, &a);
            pthread_mutexattr_destroy(&a);
            g_cs[g_cs_count].addr = addr;
            g_cs[g_cs_count].m = m;
            g_cs_count++;
        }
    }
    pthread_mutex_unlock(&g_cs_lock);
    return m;
}

static void h_init_cs(CPU *c) { (void)cs_get(arg32(c, 0), 1); c->eax = 0; }
static void h_delete_cs(CPU *c)
{
    uint32_t addr = arg32(c, 0);
    pthread_mutex_lock(&g_cs_lock);
    for (int i = 0; i < g_cs_count; i++) {
        if (g_cs[i].addr != addr) continue;
        pthread_mutex_destroy(g_cs[i].m);
        free(g_cs[i].m);
        g_cs[i] = g_cs[--g_cs_count];
        break;
    }
    pthread_mutex_unlock(&g_cs_lock);
}
static void h_enter_cs(CPU *c)
{
    pthread_mutex_t *m = cs_get(arg32(c, 0), 1);
    if (m) pthread_mutex_lock(m);
}
static void h_leave_cs(CPU *c)
{
    pthread_mutex_t *m = cs_get(arg32(c, 0), 0);
    if (m) pthread_mutex_unlock(m);
}

static void cr_beginthreadex(CPU *c)
{
    c->eax = guest_thread_spawn(arg32(c, 2), arg32(c, 3), arg32(c, 4),
                                arg32(c, 5), "_beginthreadex");
}

static void cr_initterm(CPU *c)
{
    uint32_t first = arg32(c, 0), last = arg32(c, 1);
    extern void dispatch(CPU *c, uint32_t target);
#if WC3_DEBUG
    unsigned n = 0;
#endif
    for (uint32_t p = first; p < last; p += 4) {
        uint32_t fn = rd32g(p);
        if (!fn) continue;
        uint32_t saved = c->esp;
        push32(c, 0x7EFFFFF0u);
        dispatch(c, fn);
        c->esp = saved;
#if WC3_DEBUG
        n++;
#endif
    }
#if WC3_DEBUG
    fprintf(stderr, "[hle] _initterm(%08X..%08X): %u initialisers\n", first, last, n);
#endif
    c->eax = 0;
}

static void cr_exit(CPU *c)
{
    fprintf(stderr, "[hle] exit(%d)\n", (int)arg32(c, 0));
    hle_android_report();
    _exit(0);
}

static void cr_cexit(CPU *c) { (void)c; }

static void cr_xcpt_filter(CPU *c) { c->eax = 1; }
static void cr_amsg_exit(CPU *c)
{
    fprintf(stderr, "[hle] _amsg_exit(%u)\n", arg32(c, 0));
    hle_android_report();
    _exit(4);
}
static void cr_purecall(CPU *c)
{
    fprintf(stderr, "[hle] _purecall at ra=%08X\n", rd32g(c->esp));
    abort();
}
static void cr_invoke_watson(CPU *c)
{
    fprintf(stderr, "[hle] _invoke_watson at ra=%08X\n", rd32g(c->esp));
    hle_android_report();
    _exit(5);
}

static void cr_controlfp(CPU *c) { c->eax = 0x0008001Fu; }
static void cr_controlfp_s(CPU *c)
{
    uint32_t cur = arg32(c, 0);
    if (cur && guest_ok(cur, 4)) wr32g(cur, 0x0008001Fu);
    c->eax = 0;
}
static void cr_clearfp(CPU *c) { c->eax = 0; }

/* Pop an x87 value and view it as a host double.  SoftFloat's float64_t is a
 * struct, so the conversion is a memcpy of the payload. */
static double fpop_f64(CPU *c)
{
    float64_t f = extF80_to_f64(fpop(c));
    double d;
    memcpy(&d, &f.v, sizeof d);
    return d;
}

static void cr_cisqrt(CPU *c)
{
    double x = fpop_f64(c);
    fpush(c, x87_from_f64(sqrt(x)));
}
static void cr_cilog10(CPU *c)
{
    double x = fpop_f64(c);
    fpush(c, x87_from_f64(log10(x)));
}
static void cr_cipow(CPU *c)
{
    double y = fpop_f64(c);
    double x = fpop_f64(c);
    fpush(c, x87_from_f64(pow(x, y)));
}
/* _CIfmod is the compiler's internal fmod: both operands come off the x87
 * stack and the result goes back onto it, so a stub that only sets eax leaves
 * ST(0) holding whatever the previous calculation left there - the same
 * failure mode as ceil() at line 3071.  game.dll calls it from 0x6F1BEEBF. */
static void cr_cifmod(CPU *c)
{
    double y = fpop_f64(c);
    double x = fpop_f64(c);
    fpush(c, x87_from_f64(fmod(x, y)));
}
/* _CIatan2 is the compiler's x87 atan2, not a cdecl function: the caller
 * leaves y in ST(1) and x in ST(0), and fpatan replaces them with
 * atan2(ST(1), ST(0)).  Reading stack arguments here returns whatever the
 * caller happened to leave above the return address and leaves the two x87
 * inputs on the stack, which corrupts every later slerp that uses it. */
static void cr_ciatan2(CPU *c)
{
    double x = fpop_f64(c);
    double y = fpop_f64(c);
    fpush(c, x87_from_f64(atan2(y, x)));
}

/* MSVCR80's ordinary math functions take their arguments on the *stack*
 * (cdecl, 8 bytes each) and return the result on the x87 stack, not in eax.
 * The generic stub answers "0 in eax" and leaves ST(0) holding whatever the
 * previous calculation left there, and that is not harmless: game.dll calls
 * ceil() while formatting a movie path, and the garbage it got back made it
 * hand Storm the bare format "%sMovies\%s.mpg" as a file name. */
static double cr_arg_f64(CPU *c, unsigned n)
{
    uint64_t bits = (uint64_t)rd32g(c->esp + 4 + n * 8)
                  | ((uint64_t)rd32g(c->esp + 8 + n * 8) << 32);
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

#define CR_MATH1(fn, expr)                                              \
    static void cr_f64_##fn(CPU *c)                                     \
    {                                                                   \
        double x = cr_arg_f64(c, 0);                                    \
        fpush(c, x87_from_f64(expr));                                   \
    }
#define CR_MATH2(fn, expr)                                              \
    static void cr_f64_##fn(CPU *c)                                     \
    {                                                                   \
        double x = cr_arg_f64(c, 0), y = cr_arg_f64(c, 1);              \
        fpush(c, x87_from_f64(expr));                                   \
    }

CR_MATH1(ceil,  ceil(x))
CR_MATH1(floor, floor(x))
CR_MATH1(fabs,  fabs(x))
CR_MATH1(sqrt,  sqrt(x))
CR_MATH1(sin,   sin(x))
CR_MATH1(cos,   cos(x))
CR_MATH1(tan,   tan(x))
CR_MATH1(asin,  asin(x))
CR_MATH1(acos,  acos(x))
CR_MATH1(atan,  atan(x))
CR_MATH1(exp,   exp(x))
CR_MATH1(log,   log(x))
CR_MATH1(log10, log10(x))
CR_MATH2(pow,   pow(x, y))
CR_MATH2(fmod,  fmod(x, y))
CR_MATH2(atan2, atan2(x, y))

static void cr_ismbblead(CPU *c) { (void)c; c->eax = 0; }

/* --- CRT pieces reached on only a few paths, but reached hard --- */

/* malloc: 13 916 calls in one probe run, every one of them returning 0.  The
 * game's own allocator is the CRT statically linked into game.dll; only a few
 * internal paths import this one, and a null from it becomes a null
 * dereference or a "failed to allocate" dialog immediately. */
static void cr_malloc(CPU *c)
{
    uint32_t n = arg32(c, 0);
    c->eax = hle_guest_alloc(n ? n : 1);
}
static void cr_free(CPU *c) { hle_guest_free(arg32(c, 0)); c->eax = 0; }
static void cr_realloc(CPU *c)
{
    uint32_t old = arg32(c, 0), n = arg32(c, 1);
    if (!old) { c->eax = hle_guest_alloc(n ? n : 1); return; }
    if (!n) { hle_guest_free(old); c->eax = 0; return; }
    /* The heap header sits 8 bytes below the payload, so the old capacity is
     * readable - copying `n` bytes blind would walk off the block. */
    uint32_t oldsz = guest_ok(old - 8, 8) ? rd32g(old - 8) : 0;
    if (oldsz > HLE_HEAP_SIZE) oldsz = 0;
    uint32_t p = hle_guest_alloc(n);
    if (p) memcpy((void *)(uintptr_t)p, (void *)(uintptr_t)old,
                  oldsz < n ? oldsz : n);
    hle_guest_free(old);
    c->eax = p;
}
static void cr_strncpy(CPU *c)
{
    uint32_t d = arg32(c, 0), s = arg32(c, 1), n = arg32(c, 2);
    for (uint32_t i = 0; i < n; i++) {
        if (!guest_ok(d + i, 1)) break;
        uint8_t ch = guest_ok(s + i, 1) ? rd8g(s + i) : 0;
        wr8g(d + i, ch);
        if (!ch) break;
    }
    c->eax = d;
}
static void cr_calloc(CPU *c)
{
    uint32_t n = arg32(c, 0) * arg32(c, 1);
    c->eax = hle_alloc0(n ? n : 1);
}

/* strncmp/_strnicmp: a stub returns 0, i.e. "these strings are equal", which
 * is the worst possible default for a case-insensitive compare - 435 calls in
 * one run were silently reporting every pair of names as identical. */
static void cr_strnicmp(CPU *c)
{
    const char *a = gstr(arg32(c, 0)), *b = gstr(arg32(c, 1));
    int n = (int)arg32(c, 2), r = 0;
    while (n-- > 0) {
        int ca = toupper((unsigned char)*a++), cb = toupper((unsigned char)*b++);
        if (ca != cb) { r = ca - cb; break; }
        if (!ca) break;
    }
    c->eax = (uint32_t)r;
}
static void cr_strncmp(CPU *c)
{
    c->eax = (uint32_t)strncmp(gstr(arg32(c, 0)), gstr(arg32(c, 1)),
                               (size_t)arg32(c, 2));
}

/* _libm_sse2_sqrt_precise: double in xmm0, result in xmm0.  The x87 _CIsqrt
 * above is a different ABI (it pops the FPU stack) and cannot be reused. */
static void cr_sqrt_sse2(CPU *c)
{
    c->xmm[0].f64[0] = sqrt(c->xmm[0].f64[0]);
}

static void cr_tan_sse2(CPU *c)
{
    c->xmm[0].f64[0] = tan(c->xmm[0].f64[0]);
}

/* The rest of the SSE2 libm family.  Same ABI as _libm_sse2_sqrt_precise -
 * argument in xmm0 (xmm1 for the binary ones), result in xmm0 - and they were
 * registered as null stubs, which returns 0.0 where the guest expects a real
 * number.  game.dll asks for pow() while it builds a movie path; with 0.0 back
 * it passed Storm the bare format "%sMovies\%s.mpg" as a file name and then
 * died on the object it got. */
#define CR_SSE2_1(fn, expr)                                             \
    static void cr_##fn##_sse2(CPU *c)                                  \
    {                                                                   \
        double x = c->xmm[0].f64[0];                                    \
        c->xmm[0].f64[0] = (expr);                                      \
    }
#define CR_SSE2_2(fn, expr)                                             \
    static void cr_##fn##_sse2(CPU *c)                                  \
    {                                                                   \
        double x = c->xmm[0].f64[0], y = c->xmm[1].f64[0];              \
        c->xmm[0].f64[0] = (expr);                                      \
    }

CR_SSE2_1(log10, log10(x))
CR_SSE2_1(log,   log(x))
CR_SSE2_1(exp,   exp(x))
CR_SSE2_1(sin,   sin(x))
CR_SSE2_1(cos,   cos(x))
CR_SSE2_1(atan,  atan(x))
CR_SSE2_1(asin,  asin(x))
CR_SSE2_1(acos,  acos(x))
CR_SSE2_2(pow,   pow(x, y))
CR_SSE2_2(atan2, atan2(x, y))

/* vsprintf(dst, fmt, va) - cdecl and unbounded.  Same va_list-as-pointer
 * convention as _vsnprintf above; the old body copied the format string
 * verbatim, which is fine for a diagnostic but wrong for every caller that
 * formats a path. */
static void cr_vsprintf(CPU *c)
{
    uint32_t dst = arg32(c, 0), fmt = arg32(c, 1), va = arg32(c, 2);
    const char *f = gstr(fmt);
    if (!dst || !f[0]) { c->eax = 0; return; }

    char out[8192];
    size_t n = guest_format(out, sizeof out, f, va);
    uint32_t room = guest_clamp(dst, (uint32_t)n + 1);
    if (room) {
        memcpy((void *)(uintptr_t)dst, out, room - 1);
        ((char *)(uintptr_t)dst)[room - 1] = 0;
    }
    c->eax = (uint32_t)n;
}

/* _snprintf(dst, cap, fmt, ...) and _snprintf_s(dst, cap, count, fmt, ...):
 * the same body with the varargs one word further along.  Storm and game.dll
 * both import them and neither was bound, so a call became a null stub that
 * returned 0 and left the destination holding whatever was there - the exact
 * shape of a "the name is a stale format string" failure. */
static void snprintf_common(CPU *c, unsigned argbase, uint32_t dst, uint32_t cap,
                            const char *f)
{
    if (!dst || !cap || !f[0]) { c->eax = 0; return; }
    char out[8192];
    size_t n = guest_format(out, sizeof out, f, c->esp + 4 + argbase * 4);
    uint32_t room = guest_clamp(dst, cap);
    if (room) {
        size_t w = (n < (size_t)room) ? n : (size_t)room - 1;
        memcpy((void *)(uintptr_t)dst, out, w);
        ((char *)(uintptr_t)dst)[w] = 0;
    }
    c->eax = (uint32_t)n;
}

static void cr_snprintf(CPU *c)
{
    snprintf_common(c, 3, arg32(c, 0), arg32(c, 1), gstr(arg32(c, 2)));
}

static void cr_snprintf_s(CPU *c)
{
    snprintf_common(c, 4, arg32(c, 0), arg32(c, 1), gstr(arg32(c, 3)));
}

/* The CRT's logger.  stderr is `__iob_func() + 2 * sizeof(FILE)`, so a null
 * return makes every diagnostic - including _invalid_parameter's own report -
 * dereference 0 and fast-fail through __report_gsfailure.  Hand out a real
 * three-entry array of zeroed FILE structs (MSVCR80's FILE is 32 bytes);
 * vfprintf stays a stub, so nothing is written, but nothing dies either. */
static void cr_iob_func(CPU *c)
{
    static uint32_t iob;
    if (!iob) {
        iob = hle_alloc0(3 * 32);
        if (iob) {
            pthread_mutex_lock(&g_cr_file_lock);
            g_cr_file[0] = stdin;
            g_cr_file[1] = stdout;
            g_cr_file[2] = stderr;
            pthread_mutex_unlock(&g_cr_file_lock);
            for (uint32_t i = 0; i < 3; i++) {
                wr32g(iob + i * 32, CR_FILE_MAGIC);
                wr32g(iob + i * 32 + 4, i);
            }
        }
    }
    c->eax = iob;
}

static void cr_strerror(CPU *c)
{
    static uint32_t msg;
    if (!msg) {
        msg = hle_alloc0(32);
        if (msg) strcpy((char *)(uintptr_t)msg, "error");
    }
    c->eax = msg;
}

/* The game is trying to *tell us* something when it calls this.  The varargs
 * cannot be formatted without the guest's va_list, but the literal text of the
 * format string is usually the whole message. */
static void cr_vfprintf(CPU *c)
{
    const char *fmt = gstr(arg32(c, 1));
#if WC3_DEBUG
    fprintf(stderr, "[crt] vfprintf: %s", fmt);
#endif
    c->eax = (uint32_t)strlen(fmt);
}

static void h_get_std_handle(CPU *c)
{
    c->eax = 0x00000010u + arg32(c, 0) * 4u;   /* pseudo-handles, nonzero */
}
static void h_get_file_type(CPU *c) { (void)c; c->eax = 1; }   /* FILE_TYPE_DISK */

/* The main window's procedure is what turns a queued WM_LBUTTONDOWN into a
 * menu selection.  The class registration carries it, so remember it here;
 * CreateWindowExA below remembers the handle the game will pass back to
 * GetMessage/PeekMessage/DispatchMessage. */
static uint32_t g_wndproc;
static uint32_t g_main_hwnd;
static uint32_t g_main_userdata;

static void h_register_class_ex(CPU *c)
{
    uint32_t p = arg32(c, 0);
    if (p && guest_ok(p, 48)) {
        g_wndproc = rd32g(p + 8);
#if WC3_DEBUG
        fprintf(stderr, "[ui] RegisterClassExA(\"%s\") wndproc=%08X\n",
                gstr(rd32g(p + 40)), g_wndproc);
#endif
    } else {
        fprintf(stderr, "[ui] RegisterClassExA(%08X): bad pointer\n", p);
    }
    c->eax = 0x0000C001u;
}

/* _strdup: a stub returns 0, and the caller then walks a null string. */
static void cr_strdup(CPU *c)
{
    const char *s = gstr(arg32(c, 0));
    size_t n = strlen(s) + 1;
    uint32_t p = hle_guest_alloc((uint32_t)n);
    if (p) memcpy((void *)(uintptr_t)p, s, n);
    c->eax = p;
}

/* Toolhelp process enumeration (L_6F01B5F0): the game walks the process list
 * looking for another instance of itself.  Returning INVALID_HANDLE_VALUE ends
 * the walk at once - but the *arg count* is what matters here.  An unknown
 * import is registered with argc 0, so hle_call pops 4 bytes instead of 12 and
 * esp drifts 8 bytes lower on every call; one run ended with esp=2FFFFFFC,
 * i.e. below the whole guest stack.  Bind the arity, not just the answer. */
static void h_toolhelp_snapshot(CPU *c) { (void)c; c->eax = 0xFFFFFFFFu; }
static void h_process32_next(CPU *c) { (void)c; c->eax = 0; }

/* GetWindowInfo(HWND, PWINDOWINFO): WINDOWINFO is 60 bytes and the game reads
 * it straight back, so leaving it untouched hands it stack garbage. */
static void h_get_window_info(CPU *c)
{
    uint32_t p = arg32(c, 1);
    if (p && guest_ok(p, 60)) {
        memset((void *)(uintptr_t)p, 0, 60);
        wr32g(p + 0, 60);        /* cbSize */
        wr32g(p + 36, 0x04CF0000u); /* dwStyle: WS_OVERLAPPEDWINDOW-ish */
        wr32g(p + 40, 0x00000100u); /* dwExStyle: WS_EX_WINDOWEDGE */
    }
    c->eax = 1;
}

/* SHGetFolderPathA(hwnd, csidl, hToken, dwFlags, pszPath): five arguments, and
 * the arity is the point - see the toolhelp note above.  Failing cleanly lets
 * the game fall back to its own directory. */
static void h_sh_get_folder_path(CPU *c)
{
    uint32_t buf = arg32(c, 4);
    if (buf && guest_ok(buf, 260)) wr8g(buf, 0);
    c->eax = 0x80004005u;        /* E_FAIL */
}

/* strchr: a stub returns NULL, i.e. "this character is not in the string",
 * which silently changes the control flow of every parser that uses it. */
static void cr_strchr(CPU *c)
{
    const char *s = gstr(arg32(c, 0));
    const char *p = strchr(s, (int)(arg32(c, 1) & 0xFFu));
    c->eax = p ? (uint32_t)(uintptr_t)p : 0;
}

/* sscanf.  The game parses config and data text with it, so "return 0 and write
 * nothing" is not survivable - and an unbound import is also a stack imbalance,
 * because argc comes from the decorated name and an undecorated one gets 0.
 * Handles the subset that appears in practice: %d %i %u %x %s %c %f %lf, with
 * field widths, the `h` modifier, and `*` suppression. */
static void cr_sscanf(CPU *c)
{
    const char *s = gstr(arg32(c, 0));
    const char *f = gstr(arg32(c, 1));
    unsigned ai = 2, got = 0;
    char *end;

    while (*f) {
        if (isspace((unsigned char)*f)) {
            while (isspace((unsigned char)*s)) s++;
            f++;
            continue;
        }
        if (*f != '%') { if (*s != *f) break; s++; f++; continue; }
        f++;
        if (*f == '%') { if (*s != '%') break; s++; f++; continue; }
        int suppress = 0;
        if (*f == '*') { suppress = 1; f++; }
        unsigned width = 0;
        while (*f >= '0' && *f <= '9') width = width * 10u + (unsigned)(*f++ - '0');
        int lng = 0, shrt = 0;
        if (*f == 'l') { lng = 1; f++; }
        else if (*f == 'h') { shrt = 1; f++; }
        char conv = *f ? *f++ : 0;
        (void)lng;
        if (conv != 'c') while (isspace((unsigned char)*s)) s++;
        if (!*s) break;

        uint32_t p = suppress ? 0 : arg32(c, ai);
        if (conv == 's') {
            char tmp[256];
            unsigned n = 0;
            while (*s && !isspace((unsigned char)*s) && (!width || n < width)) {
                if (n + 1 < sizeof tmp) tmp[n] = *s;
                n++; s++;
            }
            unsigned k = n < sizeof tmp - 1 ? n : sizeof tmp - 1;
            tmp[k] = 0;
            if (!suppress && guest_ok(p, k + 1)) memcpy((void *)(uintptr_t)p, tmp, k + 1);
        } else if (conv == 'c') {
            if (!suppress && guest_ok(p, 1)) wr8g(p, (uint8_t)*s);
            s++;
        } else if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X') {
            long v = (conv == 'x' || conv == 'X') ? strtol(s, &end, 16)
                   : (conv == 'i') ? strtol(s, &end, 0)
                                   : strtol(s, &end, 10);
            if (end == s) break;
            s = end;
            if (!suppress) {
                if (shrt) { if (guest_ok(p, 2)) wr16g(p, (uint16_t)v); }
                else if (guest_ok(p, 4)) wr32g(p, (uint32_t)v);
            }
        } else if (conv == 'f' || conv == 'e' || conv == 'g') {
            double d = strtod(s, &end);
            if (end == s) break;
            s = end;
            if (!suppress) {
                if (lng) {
                    if (guest_ok(p, 8)) {
                        memcpy((void *)(uintptr_t)p, &d, 8);
                    }
                } else {
                    float fv = (float)d;
                    if (guest_ok(p, 4)) memcpy((void *)(uintptr_t)p, &fv, 4);
                }
            }
        } else {
            break;
        }
        if (!suppress) { ai++; got++; }
    }
    c->eax = got;
}

/* WS2_32 imports are ordinal-only, so they cannot be matched by name.  The one
 * that matters here is 115 = WSAStartup(0x0202, &WSADATA): the CRT calls it
 * from L_6F79F440 with a 0x190-byte stack buffer and checks the cookie at
 * [esp+0x190] on the way out.  A stub registered with argc == 0 leaves esp 8
 * bytes high, so the cookie read lands on the wrong slot and the epilogue
 * fast-fails through __report_gsfailure - which looks exactly like a stack
 * smash in the caller. */
/* The byte-order helpers are ordinary functions, not socket calls, so they can
 * be answered exactly.  9 = htons is reached from L_6F0A2590 in game.dll,
 * which fills a sockaddr_in: it writes 2 (AF_INET) to +0, calls htons(port) and
 * stores the 16-bit result at +2.  With argc 0 the four bytes of argument stay
 * on the guest stack and the epilogue returns into garbage. */
static void h_htons(CPU *c) { uint32_t v = arg32(c, 0) & 0xFFFFu;
                              c->eax = ((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu); }
static void h_ntohs(CPU *c) { h_htons(c); }
static void h_htonl(CPU *c) { uint32_t v = arg32(c, 0);
                              c->eax = (v << 24) | ((v & 0xFF00u) << 8) |
                                       ((v >> 8) & 0xFF00u) | (v >> 24); }
static void h_ntohl(CPU *c) { h_htonl(c); }

static void h_wsa_startup(CPU *c)
{
    uint32_t p = arg32(c, 1);
    if (p && guest_ok(p, 0x190)) {
        memset((void *)(uintptr_t)p, 0, 0x190);
        wr16g(p + 0, 0x0202);      /* wVersion     */
        wr16g(p + 2, 0x0202);      /* wHighVersion */
    }
    c->eax = 0;
}

/* ------------------------------------------------------------------ */
/* USER32 / GDI32 / ADVAPI32 / SHELL32 / others                        */
/* ------------------------------------------------------------------ */

static void u_message_box(CPU *c)
{
#if WC3_DEBUG
    uint32_t text = arg32(c, 1), caption = arg32(c, 2);
    fprintf(stderr, "[ui] MessageBoxA(hwnd=%08X, text=%08X \"%s\", caption=%08X \"%s\", type=%u) ra=%08X\n",
            arg32(c, 0), text, gstr(text), caption, gstr(caption), arg32(c, 3), rd32g(c->esp));
    /* A pointer that is valid but whose first byte is 0 usually means the
     * producer wrote nothing; dump the head so an empty message can be told
     * apart from a message built somewhere else. */
    if (text && guest_ok(text, 32)) {
        fprintf(stderr, "[ui]   text bytes:");
        for (int i = 0; i < 32; i++)
            fprintf(stderr, " %02X", rd8g(text + (uint32_t)i));
        fprintf(stderr, "\n");
    }
    /* The caller chain is the only way to tell which check failed: the text
     * buffer is empty, so the message itself names nothing. */
    {
        uint32_t ebp = c->ebp;
        fprintf(stderr, "[ui]   callers:");
        for (int i = 0; i < 12 && ebp && guest_ok(ebp, 8); i++) {
            uint32_t ret = rd32g(ebp + 4), next = rd32g(ebp);
            if (!ret || ret < 0x00400000u) break;
            fprintf(stderr, " %08X", ret);
            if (next <= ebp) break;
            ebp = next;
        }
        fprintf(stderr, "\n");
    }
    uint32_t sp = c->esp + 4;
    /* The two globals the fatal path decides on: 0x6FCD52F4 caches "which
     * archive index is usable" (-1 = none) and 0x6FD3C650 is the per-archive
     * flag array it is derived from. */
    fprintf(stderr, "[ui]   archive-pick: cache=%08X flags=%08X %08X\n",
            rd32g(0x6FCD52F4u), rd32g(0x6FD3C650u), rd32g(0x6FD3C654u));
    /* The body of the message is a global, not the caller's buffer: 6F08B3B0
     * copies [0x6FD3D3E0] into the caption slot, so an empty `text` above only
     * means the caller passed nothing.  Read the global too before concluding
     * the message is lost. */
    fprintf(stderr, "[ui]   text global [6FD3D3E0]=%08X \"%s\"\n",
            rd32g(0x6FD3D3E0u), gstr(rd32g(0x6FD3D3E0u)));
    /* The last 48 dispatches are the only record of *how* the check failed. */
    fprintf(stderr, "[ui]   last dispatches:");
    for (unsigned i = 0; i < 48 && i < g_disp_len; i++) {
        unsigned p = (g_disp_pos + 64 - g_disp_len + i) % 64;
        fprintf(stderr, " %08X", g_disp_ring[p]);
    }
    fprintf(stderr, "\n");
    if (guest_ok(sp, 4 * 24)) {
        fprintf(stderr, "[ui]   stack:");
        for (int i = 0; i < 24; i++) {
            uint32_t v = rd32g(sp + (uint32_t)i * 4);
            fprintf(stderr, " %08X", v);
        }
        fprintf(stderr, "\n");
    }
#endif
    c->eax = 1;   /* IDOK */
}

/* The launcher's error text is a STRINGTABLE entry, so without this every
 * failure reads as MessageBoxA("") and the actual complaint is invisible. */
static void u_load_string(CPU *c)
{
    uint32_t hmod = arg32(c, 0), id = arg32(c, 1);
    uint32_t buf = arg32(c, 2), n = arg32(c, 3);
    char tmp[1024];
    int len = 0;
    HleMod *m = mod_by_base(hmod);
    if (m && m->img) len = pe_string_resource((const pe_image *)m->img, id, tmp, sizeof tmp);

    if (buf && n > 0 && guest_ok(buf, n)) {
        size_t k = (size_t)len < (size_t)n - 1 ? (size_t)len : (size_t)n - 1;
        if (k) memcpy((void *)(uintptr_t)buf, tmp, k);
        ((char *)(uintptr_t)buf)[k] = 0;
    }
#if WC3_DEBUG
    fprintf(stderr, "[ui] LoadStringA(%08X, %u) -> %d \"%s\"\n",
            hmod, id, len, len ? tmp : "");
#endif
    c->eax = len;
}

/* wsprintfA is USER32's variadic sprintf: cdecl, no destination size, and the
 * same format language as the CRT for every conversion the game uses.  It
 * shares guest_format() so a width or zero-pad specifier is honoured - the old
 * body stripped "-+ #0" and reformatted with a bare "%d", which turned
 * "TeamColor%02u" into "TeamColor7" and made every generated resource name
 * miss its lookup. */
static void u_wsprintf(CPU *c)
{
    uint32_t dst = arg32(c, 0);
    const char *f = gstr(arg32(c, 1));
    if (!dst || !guest_ok(dst, 1) || !f[0]) { c->eax = 0; return; }

    char out[2048];
    size_t n = guest_format(out, sizeof out, f, c->esp + 12);
    uint32_t room = guest_clamp(dst, (uint32_t)n + 1);
    if (room) {
        memcpy((void *)(uintptr_t)dst, out, room - 1);
        ((char *)(uintptr_t)dst)[room - 1] = 0;
    }
    c->eax = (uint32_t)n;
}

static void u_register_class(CPU *c)
{
    uint32_t p = arg32(c, 0);
    uint32_t proc = (p && guest_ok(p, 40)) ? rd32g(p + 4) : 0;
    const char *name = (p && guest_ok(p, 40)) ? gstr(rd32g(p + 36)) : "";
#if WC3_DEBUG
    fprintf(stderr, "[ui] RegisterClassA(\"%s\") wndproc=%08X\n", name, proc);
#endif
    if (proc && (!g_wndproc || strstr(name, "Warcraft")))
        g_wndproc = proc;
    c->eax = 1;
}

/* ---- window messages ----------------------------------------------------
 * The engine takes clicks from its window procedure, not from polling
 * GetKeyState: a run with touch input shows GetCursorPos() being polled but
 * no GetKeyState() calls at all.  Touch therefore has to become real
 * WM_LBUTTONDOWN/WM_LBUTTONUP messages.  The queue is small and shared with
 * the input thread, so every operation takes g_uq_lock. */
#define UMSG_MAX 64
#define WM_KEYDOWN     0x0100u
#define WM_KEYUP       0x0101u
#define WM_CHAR        0x0102u
#define WM_SYSKEYDOWN  0x0104u
#define WM_SYSKEYUP    0x0105u
#define WM_MOUSEMOVE   0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP   0x0202u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP   0x0205u
#define WM_MBUTTONDOWN 0x0207u
#define WM_MBUTTONUP   0x0208u
#define WM_MOUSEWHEEL  0x020Au

/* The game calls ImmGetContext() from inside the window procedure when a
 * click focuses one of its edit fields.  Arm this for the duration of a left
 * button press: if ImmGetContext arrives, the click was a text field and the
 * soft keyboard stays up; otherwise the click was a button and it hides. */
static volatile int g_ime_click_pending;
static volatile int g_ime_shown;
/* Defined next to the virtual mouse below; the dispatcher needs them earlier. */
static volatile int32_t g_mouse_x, g_mouse_y;
static volatile int g_mouse_down, g_mouse_dispatched_down;
/* bit 0=left, bit 1=middle, bit 2=right.  g_mouse_down stays the left-button
 * alias used by the one-finger path and the wait hook. */
static volatile int g_mouse_buttons;
/* Updated only when WM_KEYDOWN/WM_KEYUP is dispatched, never at enqueue. */
static HleKeyState g_key_state;
/* The game can ask for the IME context just after DispatchMessage returns,
 * not inside the window procedure.  Remember when the last click happened so
 * ImmGetContext can still recognise it as a text-field focus. */
static volatile uint32_t g_ime_click_ms;
static volatile int g_ime_hide_pending;

typedef struct {
    uint32_t hwnd, msg, wp, lp, time;
    int32_t x, y;
} UMsg;

static UMsg g_uq[UMSG_MAX];
static int g_uq_head, g_uq_count;
static pthread_mutex_t g_uq_lock = PTHREAD_MUTEX_INITIALIZER;

static void u_msg_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    int32_t x = (int16_t)(lp & 0xFFFF);
    int32_t y = (int16_t)((lp >> 16) & 0xFFFF);
    pthread_mutex_lock(&g_uq_lock);
    /* Coalesce only a move that is still at the tail.  Replacing an earlier
     * move could jump it over a click and make the wndproc see the button
     * press at the wrong position. */
    if (msg == WM_MOUSEMOVE && g_uq_count > 0) {
        UMsg *tail = &g_uq[(g_uq_head + g_uq_count - 1) % UMSG_MAX];
        if (tail->msg == WM_MOUSEMOVE && tail->hwnd == hwnd) {
            tail->wp = wp;
            tail->lp = lp;
            tail->x = x;
            tail->y = y;
            tail->time = hle_now_ms();
            pthread_mutex_unlock(&g_uq_lock);
            return;
        }
    }
    if (g_uq_count == UMSG_MAX) {
        g_uq_head = (g_uq_head + 1) % UMSG_MAX;
        g_uq_count--;
    }
    UMsg *m = &g_uq[(g_uq_head + g_uq_count) % UMSG_MAX];
    m->hwnd = hwnd;
    m->msg = msg;
    m->wp = wp;
    m->lp = lp;
    m->time = hle_now_ms();
    m->x = x;
    m->y = y;
    g_uq_count++;
    pthread_mutex_unlock(&g_uq_lock);
}

static int u_msg_take(uint32_t out, uint32_t hwnd_filter, uint32_t lo,
                      uint32_t hi, int remove)
{
    if (!out) return 0;
    pthread_mutex_lock(&g_uq_lock);
    int found = -1;
    for (int i = 0; i < g_uq_count; i++) {
        UMsg *m = &g_uq[(g_uq_head + i) % UMSG_MAX];
        if (hwnd_filter && m->hwnd != hwnd_filter) continue;
        if ((lo || hi) && (m->msg < lo || m->msg > hi)) continue;
        found = i;
        break;
    }
    if (found < 0) {
        pthread_mutex_unlock(&g_uq_lock);
        return 0;
    }
    UMsg m = g_uq[(g_uq_head + found) % UMSG_MAX];
    if (remove) {
        for (int i = found; i < g_uq_count - 1; i++)
            g_uq[(g_uq_head + i) % UMSG_MAX] =
                g_uq[(g_uq_head + i + 1) % UMSG_MAX];
        g_uq_count--;
    }
    pthread_mutex_unlock(&g_uq_lock);

    wr32g(out + 0, m.hwnd);
    wr32g(out + 4, m.msg);
    wr32g(out + 8, m.wp);
    wr32g(out + 12, m.lp);
    wr32g(out + 16, m.time);
    wr32g(out + 20, (uint32_t)m.x);
    wr32g(out + 24, (uint32_t)m.y);
    return 1;
}

static void u_create_window(CPU *c)
{
    uint32_t hwnd = 0x00020000u + (g_fake_next += 4);
    uint32_t param = arg32(c, 11);
#if WC3_DEBUG
    fprintf(stderr, "[ui] CreateWindowExA(\"%s\", \"%s\", lp=%08X) -> %08X\n",
            gstr(arg32(c, 1)), gstr(arg32(c, 2)), param, hwnd);
#endif
    if (!g_main_hwnd) {
        g_main_hwnd = hwnd;
        /* CreateWindowExA would deliver WM_CREATE with a CREATESTRUCT whose
         * lpCreateParams is this argument, and the game's window procedure
         * stores that pointer as GWL_USERDATA.  It is the object the mouse
         * handler lives in, so install it here instead of sending WM_CREATE. */
        g_main_userdata = param;
    }
    c->eax = hwnd;
}

/* Call a guest function from an HLE import, preserving the caller's CPU
 * state.  The return slot is a sentinel: dispatch() returns control here, it
 * is never jumped to. */
static uint32_t u_call_guest(CPU *c, uint32_t fn, int argc, const uint32_t *args)
{
    uint32_t sbx = c->ebx, ssi = c->esi, sdi = c->edi, sbp = c->ebp;
    uint32_t sesp = c->esp;
    for (int i = argc - 1; i >= 0; i--) push32(c, args[i]);
    push32(c, 0x7EFFFFF6u);
    dispatch(c, fn);
    uint32_t r = c->eax;
    c->ebx = sbx;
    c->esi = ssi;
    c->edi = sdi;
    c->ebp = sbp;
    c->esp = sesp;
    return r;
}

/* The game writes its preference store only when the options dialog is
 * applied or when it exits through its own quit path.  Android can pause or
 * finish the activity without either path, so the host asks for one save at
 * the next message-pump boundary.  The guest is already inside this thread
 * when the request is serviced; u_call_guest keeps the nested call from
 * corrupting the outer import's stack. */
static volatile int g_settings_save_requested;

/* Defined in the registry HLE module.  The save writes all 79 preference
 * values, so bracket it and let the registry rewrite the file once. */
void wc3_hle_registry_begin_batch(void);
void wc3_hle_registry_end_batch(void);

void wc3_android_request_settings_save(void)
{
    g_settings_save_requested = 1;
}

static void service_settings_save(CPU *c)
{
    if (!g_settings_save_requested) return;

    /* The activity can be paused before the guest has loaded its preference
     * store.  Saving then would write a fresh default map over the player's
     * file.  The game loads the store before it registers its window class,
     * so g_wndproc is a safe "preferences are live" boundary. */
    if (!g_wndproc) return;

    /* game.dll 1.27, image base 0x6f000000.  CPreferencesWar3::Get() returns
     * the in-memory preference map; CPreferencesWar3::Save(0x50) walks every
     * known value and writes it through Storm/ADVAPI32.  Keep the addresses
     * here rather than in the host: this is the same pinned game image the
     * rest of the legacy path already targets. */
    const uint32_t saved_ecx = c->ecx;
    const uint32_t saved_edx = c->edx;
    const uint32_t prefs = u_call_guest(c, 0x6f040ed0u, 0, NULL);
    if (prefs) {
        uint32_t all = 0x50u;
        c->ecx = prefs;
        wc3_hle_registry_begin_batch();
        (void)u_call_guest(c, 0x6f042620u, 1, &all);
        wc3_hle_registry_end_batch();
        g_settings_save_requested = 0;
#if WC3_DEBUG
        fprintf(stderr, "[settings] saved in-memory preferences via game.dll\n");
#endif
    }
    c->ecx = saved_ecx;
    c->edx = saved_edx;
}

static void keyboard_service(void);

static void u_get_message(CPU *c)
{
    uint32_t out = arg32(c, 0);
    uint32_t hwnd = arg32(c, 1), lo = arg32(c, 2), hi = arg32(c, 3);
    if (!out) { c->eax = 0xFFFFFFFFu; return; }

    service_settings_save(c);
    keyboard_service();
    if (u_msg_take(out, hwnd, lo, hi, 1)) {
        c->eax = rd32g(out + 4) == 0x0012u ? 0 : 1;   /* WM_QUIT */
#if WC3_DEBUG
        {
            static unsigned n;
            if (n++ < 16)
                fprintf(stderr, "[input] GetMessage -> %04X hwnd=%08X at %d,%d\n",
                        rd32g(out + 4), hwnd,
                        (int)(int16_t)(rd32g(out + 12) & 0xFFFF),
                        (int)(int16_t)((rd32g(out + 12) >> 16) & 0xFFFF));
        }
#endif
        return;
    }
    c->eax = 0;
#if WC3_DEBUG
    {
        static unsigned n;
        if (n++ < 12)
            fprintf(stderr, "[input] GetMessage call hwnd=%08X range=%u..%u empty\n",
                    hwnd, lo, hi);
    }
#endif
}

static void u_peek_message(CPU *c)
{
    uint32_t out = arg32(c, 0);
    uint32_t hwnd = arg32(c, 1), lo = arg32(c, 2), hi = arg32(c, 3);
    int remove = (arg32(c, 4) & 1) != 0;             /* PM_REMOVE */
    service_settings_save(c);
    keyboard_service();
    c->eax = (uint32_t)u_msg_take(out, hwnd, lo, hi, remove);
#if WC3_DEBUG
    {
        static unsigned n;
        if (!c->eax && n++ < 12)
            fprintf(stderr, "[input] PeekMessage call hwnd=%08X range=%u..%u remove=%d empty\n",
                    hwnd, lo, hi, remove);
        else if (c->eax)
            fprintf(stderr, "[input] PeekMessage -> %04X hwnd=%08X at %d,%d\n",
                    rd32g(out + 4), hwnd,
                    (int)(int16_t)(rd32g(out + 12) & 0xFFFF),
                    (int)(int16_t)((rd32g(out + 12) >> 16) & 0xFFFF));
    }
#endif
}

static void u_dispatch_message(CPU *c)
{
    uint32_t p = arg32(c, 0);
    if (!p || !guest_ok(p, 28) || !g_wndproc) { c->eax = 0; return; }

    uint32_t hwnd = rd32g(p), msg = rd32g(p + 4);
    uint32_t wp = rd32g(p + 8), lp = rd32g(p + 12);

    /* Only the messages this HLE itself produces are dispatched.  Injecting
     * the game's own posted messages here changed startup behaviour, so the
     * queue is deliberately mouse, key and soft-keyboard chars only. */
    if (msg != WM_MOUSEMOVE && msg != WM_LBUTTONDOWN && msg != WM_LBUTTONUP &&
        msg != WM_RBUTTONDOWN && msg != WM_RBUTTONUP &&
        msg != WM_MBUTTONDOWN && msg != WM_MBUTTONUP && msg != WM_MOUSEWHEEL &&
        msg != WM_KEYDOWN && msg != WM_KEYUP &&
        msg != WM_SYSKEYDOWN && msg != WM_SYSKEYUP && msg != WM_CHAR) {
        c->eax = 0;
        return;
    }
    if (!hwnd) hwnd = g_main_hwnd;

    /* The button and key state belongs to the message being dispatched, not
     * to the moment the finger was queued.  A tap can enqueue DOWN and UP in
     * the same game tick; if UP clears the state at enqueue time, the window
     * procedure sees the button already released while handling DOWN and
     * GetKeyState(VK_LBUTTON) returns 0.  Switching here makes the state the
     * guest observes match the message it is handling. */
    if (msg == WM_LBUTTONDOWN) {
        g_mouse_buttons |= 0x1;
        g_mouse_down = 1;
        g_mouse_dispatched_down = 1;
        g_ime_click_pending = 1;
        g_ime_click_ms = hle_now_ms();
        g_ime_hide_pending = 0;
    } else if (msg == WM_LBUTTONUP) {
        g_mouse_buttons &= ~0x1;
        g_mouse_down = 0;
        g_mouse_dispatched_down = 0;
    } else if (msg == WM_MBUTTONDOWN) {
        g_mouse_buttons |= 0x2;
    } else if (msg == WM_MBUTTONUP) {
        g_mouse_buttons &= ~0x2;
    } else if (msg == WM_RBUTTONDOWN) {
        g_mouse_buttons |= 0x4;
    } else if (msg == WM_RBUTTONUP) {
        g_mouse_buttons &= ~0x4;
    }
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN ||
        msg == WM_KEYUP || msg == WM_SYSKEYUP) {
        int key_down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        hle_key_state_set_message(&g_key_state, (int)wp, lp, key_down);
        /* TranslateMessage produces WM_CHAR for printable key presses.  The
         * guest never sees the HLE's WM_KEYDOWN unless it goes through this
         * dispatcher, so generate the character here, after the key state is
         * current.  Without this the overlay's 1..8 buttons are hotkeys only
         * and cannot type into a focused edit field. */
        if (key_down) {
            uint16_t ch = hle_key_char_for_state(&g_key_state, (int)wp);
            if (ch)
                u_msg_post(hwnd, WM_CHAR, ch, 1u);
        }
    }
    if (msg == WM_MOUSEMOVE ||
        msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
        msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP ||
        msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) {
        g_mouse_x = (int32_t)(int16_t)(lp & 0xFFFF);
        g_mouse_y = (int32_t)(int16_t)((lp >> 16) & 0xFFFF);
    }

    RES_LOG("dispatch msg=%04X hwnd=%08X wndproc=%08X at=%d,%d buttons=%d",
            msg, hwnd, g_wndproc, g_mouse_x, g_mouse_y, g_mouse_buttons);

    /* Two lines per click, and they answer the only question a synthetic
     * click raises: did the guest's window procedure ever see it? */
#if WC3_DEBUG
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
        msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP ||
        msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP)
        fprintf(stderr, "[input] wndproc %08X msg=%04X wp=%08X at %d,%d\n",
                g_wndproc, msg, wp,
                (int)(int16_t)(lp & 0xFFFF),
                (int)(int16_t)((lp >> 16) & 0xFFFF));
    else if (msg == WM_KEYDOWN || msg == WM_KEYUP ||
             msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) {
        static unsigned n;
        if (n++ < 12)
            fprintf(stderr, "[input] wndproc %08X msg=%04X vk=%02X lp=%08X\n",
                    g_wndproc, msg, wp, lp);
    } else {
        static unsigned n;
        if (n++ < 12)
            fprintf(stderr, "[input] wndproc %08X move at %d,%d\n",
                    g_wndproc,
                    (int)(int16_t)(lp & 0xFFFF),
                    (int)(int16_t)((lp >> 16) & 0xFFFF));
    }
#endif

    uint32_t args[4] = { hwnd, msg, wp, lp };
    c->eax = u_call_guest(c, g_wndproc, 4, args);
#if WC3_DEBUG
    fprintf(stderr, "[input] wndproc %08X msg=%04X returned\n",
            g_wndproc, msg);
#endif
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)
        RES_LOG("dispatch msg=%04X returned eax=%08X buttons=%d",
                msg, c->eax, g_mouse_buttons);

    /* A left click that did not reach ImmGetContext inside the window
     * procedure can still do so just after DispatchMessage returns.  Do not
     * hide here: mark the click as possibly-a-button and let keyboard_service
     * hide only if no ImmGetContext arrives shortly afterwards. */
    if (msg == WM_LBUTTONDOWN && g_ime_click_pending) {
        g_ime_click_pending = 0;
        g_ime_hide_pending = 1;
    }
}

/* Hide the soft keyboard a little after a click that did not turn out to be
 * a text-field focus.  The game sometimes calls ImmGetContext immediately
 * after DispatchMessage returns, so hiding inside the dispatch would race
 * with the show. */
static void keyboard_service(void)
{
    if (!g_ime_hide_pending) return;
    if ((uint32_t)(hle_now_ms() - g_ime_click_ms) < 300u) return;
    g_ime_hide_pending = 0;
    if (g_ime_shown) {
        g_ime_shown = 0;
#if WC3_DEBUG
        fprintf(stderr, "[input] click was not a text field; keyboard hidden\n");
#endif
        if (g_keyboard_hide) g_keyboard_hide();
    }
}

/* Called by the Android front end when the IME window really appears or
 * disappears.  Without the hide side a keyboard dismissed with Back would
 * leave g_ime_shown set and the next text-field click would not show it. */
void hle_android_set_keyboard_visible(int visible)
{
    g_ime_shown = visible ? 1 : 0;
    if (!visible) {
        g_ime_click_pending = 0;
        g_ime_hide_pending = 0;
    }
}

static void u_translate_message(CPU *c) { (void)c; c->eax = 0; }
static void u_def_window_proc(CPU *c) { (void)c; c->eax = 0; }
static void u_post_message(CPU *c) { (void)c; c->eax = 1; }
static void u_send_message(CPU *c) { (void)c; c->eax = 0; }
static void u_get_dc(CPU *c) { c->eax = 0x00030000u + (g_fake_next += 4); }
static void u_release_dc(CPU *c) { (void)c; c->eax = 1; }
/* The window rect the renderer asked for.  SetWindowPos is the call that sets
 * it, and GetClientRect/GetWindowRect must agree with it or the game sizes its
 * back buffer from a rectangle it did not choose. */
static int32_t  g_win_x, g_win_y;
static uint32_t g_win_w = 800, g_win_h = 600;

/* ---- virtual mouse -------------------------------------------------------
 * The game imports no DirectInput and does not get a real Windows cursor on
 * Android, so touch has to become the polled Win32 state the engine reads:
 * GetCursorPos() for the position and GetKeyState(VK_LBUTTON) for the button.
 * The activity's input thread writes these from AInputQueue.  Coordinates are
 * normalised to the client area, which keeps them independent of the surface
 * size and of letterboxing by the compositor.
 *
 * A tap is usually shorter than one game tick.  The message queue carries
 * the click even if no poll happens in between, so the logical button state
 * follows the finger exactly: it is down while WM_LBUTTONDOWN is dispatched
 * and up when WM_LBUTTONUP is.  A deferred release would make the window
 * procedure see the button still held while handling the release. */
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04

/* bit 0=left, bit 1=middle, bit 2=right */
static int mouse_button_bit(int button)
{
    return button == 0 ? 0x1 : button == 1 ? 0x2 : button == 2 ? 0x4 : 0;
}

static uint32_t mouse_button_mk(int button)
{
    return button == 0 ? 0x0001u : button == 1 ? 0x0010u : button == 2 ? 0x0002u : 0u;
}

static uint32_t mouse_mk(void)
{
    uint32_t mk = 0;
    if (g_mouse_buttons & 0x1) mk |= 0x0001u;  /* MK_LBUTTON */
    if (g_mouse_buttons & 0x2) mk |= 0x0010u;  /* MK_MBUTTON */
    if (g_mouse_buttons & 0x4) mk |= 0x0002u;  /* MK_RBUTTON */
    return mk;
}

static volatile int32_t  g_mouse_x = 400, g_mouse_y = 300;
static volatile int      g_mouse_down;
/* Separate from g_mouse_down: set only when WM_LBUTTONDOWN/UP has actually
 * been handed to the guest window procedure.  The automation hook waits on
 * this, so a slow frame cannot turn a tap into a hover. */
static volatile int      g_mouse_dispatched_down;
/* The guest only has fake handles.  Track the ones it explicitly activates or
 * focuses so GetActiveWindow/GetFocus can answer with the same value instead
 * of a hard-coded handle that never matches g_main_hwnd. */
static uint32_t g_active_hwnd, g_focus_hwnd;

static void mouse_poll(void)
{
    /* The message queue is authoritative; nothing to expire here.  Kept as a
     * single place for GetCursorPos/GetKeyState to observe the state. */
}

/* nx,ny are surface-normalised touch coordinates.  The game image fills the
 * physical surface (DXVK scales the selected back-buffer mode up to it), so
 * the same fraction maps directly onto the guest client rectangle. */
void hle_android_mouse_move(float nx, float ny)
{
    if (nx < 0.0f) nx = 0.0f;
    if (nx > 1.0f) nx = 1.0f;
    if (ny < 0.0f) ny = 0.0f;
    if (ny > 1.0f) ny = 1.0f;
    uint32_t w = g_win_w > 1 ? g_win_w : 800;
    uint32_t h = g_win_h > 1 ? g_win_h : 600;
    g_mouse_x = (int32_t)(nx * (float)(w - 1) + 0.5f);
    g_mouse_y = (int32_t)(ny * (float)(h - 1) + 0.5f);
    uint32_t lp = ((uint32_t)g_mouse_y << 16) | (uint16_t)g_mouse_x;
    u_msg_post(g_main_hwnd, WM_MOUSEMOVE, mouse_mk(), lp);
#if WC3_DEBUG
    {
        static unsigned n;
        if (n++ < 16)
            fprintf(stderr, "[input] move %.3f,%.3f -> %d,%d win=%ux%u\n",
                    nx, ny, g_mouse_x, g_mouse_y, w, h);
    }
#endif
    RES_LOG("move norm=%.4f,%.4f win=%d,%d %ux%u -> %d,%d buttons=%d",
            nx, ny, g_win_x, g_win_y, w, h, g_mouse_x, g_mouse_y,
            g_mouse_buttons);
}

static uint32_t mouse_button_down_msg(int button)
{
    return button == 0 ? WM_LBUTTONDOWN :
           button == 1 ? WM_MBUTTONDOWN : WM_RBUTTONDOWN;
}

static uint32_t mouse_button_up_msg(int button)
{
    return button == 0 ? WM_LBUTTONUP :
           button == 1 ? WM_MBUTTONUP : WM_RBUTTONUP;
}

void hle_android_mouse_button_ex(int button, int down)
{
    int bit = mouse_button_bit(button);
    if (!bit) return;

    uint32_t lp = ((uint32_t)g_mouse_y << 16) | (uint16_t)g_mouse_x;
    if (down) {
        if (g_mouse_buttons & bit) return;
        /* Logical state is raised at press time so a poll between the press
         * and its dispatch still observes a held button.  The release is
         * different: it must not clear the state until the UP message is
         * actually dispatched, otherwise a short tap can make the window
         * procedure see "button already up" while handling DOWN. */
        g_mouse_buttons |= bit;
        if (button == 0) g_mouse_down = 1;
        u_msg_post(g_main_hwnd, mouse_button_down_msg(button), mouse_mk(), lp);
    } else {
        if (!(g_mouse_buttons & bit)) return;
        uint32_t wp = mouse_mk() & ~mouse_button_mk(button);
        u_msg_post(g_main_hwnd, mouse_button_up_msg(button), wp, lp);
    }
#if WC3_DEBUG
    {
        static unsigned n;
        if (n++ < 24)
            fprintf(stderr, "[input] button %d %s at %d,%d\n",
                    button, down ? "down" : "up", g_mouse_x, g_mouse_y);
    }
#endif
    RES_LOG("button %d %s at %d,%d win=%d,%d %ux%u",
            button, down ? "down" : "up", g_mouse_x, g_mouse_y,
            g_win_x, g_win_y, g_win_w, g_win_h);
}

void hle_android_mouse_button(int down)
{
    hle_android_mouse_button_ex(0, down);
}

void hle_android_mouse_move_delta(int dx, int dy)
{
    int32_t w = g_win_w > 1 ? (int32_t)g_win_w : 800;
    int32_t h = g_win_h > 1 ? (int32_t)g_win_h : 600;
    int32_t x = g_mouse_x + dx;
    int32_t y = g_mouse_y + dy;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= w) x = w - 1;
    if (y >= h) y = h - 1;
    g_mouse_x = x;
    g_mouse_y = y;
    uint32_t lp = ((uint32_t)g_mouse_y << 16) | (uint16_t)g_mouse_x;
    u_msg_post(g_main_hwnd, WM_MOUSEMOVE, mouse_mk(), lp);
#if WC3_DEBUG
    {
        static unsigned n;
        if (n++ < 16)
            fprintf(stderr, "[input] move delta %d,%d -> %d,%d\n",
                    dx, dy, g_mouse_x, g_mouse_y);
    }
#endif
    RES_LOG("move delta %d,%d -> %d,%d buttons=%d",
            dx, dy, g_mouse_x, g_mouse_y, g_mouse_buttons);
}

void hle_android_mouse_wheel(int delta)
{
    if (delta == 0) return;
    uint32_t wp = ((uint32_t)(uint16_t)delta << 16) | mouse_mk();
    uint32_t lp = ((uint32_t)g_mouse_y << 16) | (uint16_t)g_mouse_x;
    u_msg_post(g_main_hwnd, WM_MOUSEWHEEL, wp, lp);
#if WC3_DEBUG
    {
        static unsigned n;
        if (n++ < 16)
            fprintf(stderr, "[input] wheel %d at %d,%d\n",
                    delta, g_mouse_x, g_mouse_y);
    }
#endif
    RES_LOG("wheel %d at %d,%d", delta, g_mouse_x, g_mouse_y);
}

void hle_android_key_event(int vk, int down)
{
    if (!vk) return;
    int msg_vk = hle_key_message_vk(vk);
    uint32_t msg = msg_vk == HLE_VK_MENU
        ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP)
        : (down ? WM_KEYDOWN : WM_KEYUP);
    u_msg_post(g_main_hwnd, msg, (uint32_t)msg_vk, hle_key_lparam(vk, down));
#if WC3_DEBUG
    {
        static unsigned n;
        if (n++ < 32)
            fprintf(stderr, "[input] key vk=%02X %s\n",
                    vk, down ? "down" : "up");
    }
#endif
    RES_LOG("key vk=%02X %s", vk, down ? "down" : "up");
}

static int is_specific_modifier(int vk)
{
    return vk == HLE_VK_LCONTROL || vk == HLE_VK_RCONTROL ||
           vk == HLE_VK_LSHIFT || vk == HLE_VK_RSHIFT ||
           vk == HLE_VK_LMENU || vk == HLE_VK_RMENU;
}

void hle_android_release_all(void)
{
    for (int b = 0; b < 3; b++) {
        if (g_mouse_buttons & mouse_button_bit(b))
            hle_android_mouse_button_ex(b, 0);
    }
    g_mouse_buttons = 0;
    g_mouse_down = 0;
    g_mouse_dispatched_down = 0;

    static const int modifiers[] = {
        HLE_VK_LCONTROL, HLE_VK_RCONTROL,
        HLE_VK_LSHIFT, HLE_VK_RSHIFT,
        HLE_VK_LMENU, HLE_VK_RMENU,
    };
    for (size_t i = 0; i < sizeof(modifiers) / sizeof(modifiers[0]); i++) {
        if (g_key_state.down[modifiers[i]])
            hle_android_key_event(modifiers[i], 0);
    }
    for (int vk = 0; vk < 256; vk++) {
        if (is_specific_modifier(vk)) continue;
        if (vk == HLE_VK_CONTROL || vk == HLE_VK_SHIFT || vk == HLE_VK_MENU)
            continue;
        if (g_key_state.down[vk])
            hle_android_key_event(vk, 0);
    }
    hle_key_state_reset(&g_key_state);
}

/* 10 ms polling is coarse enough to stay off the game thread and fine enough
 * that the tap hook does not add a noticeable delay.  The timeout is a hard
 * cap, not the expected wait: at 60 fps a dispatch normally lands in <20 ms. */
int hle_android_wait_mouse(int down, int timeout_ms)
{
    if (timeout_ms <= 0) timeout_ms = 2000;
    uint32_t start = hle_now_ms();
    int want = down ? 1 : 0;
    while ((g_mouse_dispatched_down ? 1 : 0) != want) {
        if ((uint32_t)(hle_now_ms() - start) >= (uint32_t)timeout_ms)
            return 0;
        usleep(10000);
    }
    return 1;
}

/* ---- soft keyboard messages ---------------------------------------------
 * The Java EditText commits text, not key events.  Translate each edit into
 * the Win32 message the game's edit fields already understand.  WM_CHAR is
 * the one that inserts a character; WM_KEYDOWN/WM_KEYUP are added for the
 * editing keys so the field can also handle Backspace and Return. */
void hle_android_post_char(uint32_t ch)
{
    if (!g_main_hwnd) return;
    u_msg_post(g_main_hwnd, WM_CHAR, ch & 0xFFFFu, 1u);
#if WC3_DEBUG
    {
        static unsigned n;
        if (n++ < 32)
            fprintf(stderr, "[input] WM_CHAR %04X\n", ch & 0xFFFFu);
    }
#endif
}

void hle_android_post_backspace(void)
{
    if (!g_main_hwnd) return;
    u_msg_post(g_main_hwnd, WM_KEYDOWN, 0x08u, 1u);
    u_msg_post(g_main_hwnd, WM_CHAR, 0x08u, 1u);
    u_msg_post(g_main_hwnd, WM_KEYUP, 0x08u, 0xC0000001u);
#if WC3_DEBUG
    fprintf(stderr, "[input] WM_CHAR backspace\n");
#endif
}

void hle_android_post_enter(void)
{
    if (!g_main_hwnd) return;
    u_msg_post(g_main_hwnd, WM_KEYDOWN, 0x0Du, 1u);
    u_msg_post(g_main_hwnd, WM_CHAR, 0x0Du, 1u);
    u_msg_post(g_main_hwnd, WM_KEYUP, 0x0Du, 0xC0000001u);
#if WC3_DEBUG
    fprintf(stderr, "[input] WM_CHAR enter\n");
#endif
}

/* SetWindowPos(hwnd, after, x, y, cx, cy, flags) - seven arguments.  The arity
 * matters more than the answer here: registered with argc 0 the stub popped
 * four bytes instead of 28, the caller's frame drifted by 24, and the `ret`
 * after the call jumped to the window handle (`signal 11 at 0x21008`). */
static void u_set_window_pos(CPU *c)
{
    uint32_t x = arg32(c, 2), y = arg32(c, 3), cx = arg32(c, 4), cy = arg32(c, 5);
    RES_LOG("SetWindowPos(%d,%d %ux%u flags=%08x) old=%d,%d %ux%u",
            (int32_t)x, (int32_t)y, cx, cy, arg32(c, 6),
            g_win_x, g_win_y, g_win_w, g_win_h);
    if ((int32_t)cx > 0 && (int32_t)cy > 0) { g_win_w = cx; g_win_h = cy; }
    g_win_x = (int32_t)x;
    g_win_y = (int32_t)y;
    c->eax = 1;
}

static void u_get_client_rect(CPU *c)
{
    uint32_t p = arg32(c, 1);
    if (p && guest_ok(p, 16)) {
        wr32g(p, (uint32_t)g_win_x);
        wr32g(p + 4, (uint32_t)g_win_y);
        wr32g(p + 8, g_win_w);
        wr32g(p + 12, g_win_h);
    }
#if WC3_DEBUG && WC3_DEBUG_RESOLUTION
    {
        static int32_t lx = -0x7fffffff, ly = -0x7fffffff;
        static uint32_t lw, lh;
        if (g_win_x != lx || g_win_y != ly || g_win_w != lw || g_win_h != lh) {
            RES_LOG("GetClientRect -> %d,%d %ux%u", g_win_x, g_win_y,
                    g_win_w, g_win_h);
            lx = g_win_x; ly = g_win_y; lw = g_win_w; lh = g_win_h;
        }
    }
#endif
    c->eax = 1;
}
static void u_get_window_rect(CPU *c) { u_get_client_rect(c); }
static void u_get_sys_color(CPU *c) { c->eax = 0x00FFFFFFu; }
static void u_get_sys_color_brush(CPU *c) { c->eax = 0x00040000u + (g_fake_next += 4); }

/* ---- cursor probes -------------------------------------------------------
 * The UI changes the mouse cursor over a text field to the I-beam.  That is
 * a usable signal for showing the Android soft keyboard, because WC3 does not
 * use Win32 carets or a child edit control: it draws the field itself.  Keep
 * the mapping from the fake handles this HLE returns to the resource id the
 * guest asked for, so SetCursor can be decoded. */
#define U_CURSOR_MAX 32
static struct { uint32_t handle, id; } g_u_cursors[U_CURSOR_MAX];
static int g_u_cursor_count;
static uint32_t g_u_cursor_id;

static uint32_t u_cursor_lookup(uint32_t handle)
{
    for (int i = 0; i < g_u_cursor_count; i++)
        if (g_u_cursors[i].handle == handle)
            return g_u_cursors[i].id;
    return 0;
}

static void u_load_cursor(CPU *c)
{
    uint32_t hinst = arg32(c, 0);
    uint32_t name  = arg32(c, 1);
    uint32_t id    = name < 0x10000u ? name : 0;   /* MAKEINTRESOURCE */
    uint32_t handle = 0x00050000u + (g_fake_next += 4);
    if (g_u_cursor_count < U_CURSOR_MAX) {
        g_u_cursors[g_u_cursor_count].handle = handle;
        g_u_cursors[g_u_cursor_count].id = id;
        g_u_cursor_count++;
    }
#if WC3_DEBUG
    fprintf(stderr, "[input] LoadCursorA(%08X, %08X) id=%u -> %08X\n",
            hinst, name, id, handle);
#endif
    c->eax = handle;
}

static void u_set_cursor(CPU *c)
{
    uint32_t handle = arg32(c, 0);
    uint32_t id = handle < 0x10000u ? handle : u_cursor_lookup(handle);
    if (id != g_u_cursor_id) {
        g_u_cursor_id = id;
#if WC3_DEBUG
        fprintf(stderr, "[input] SetCursor(%08X) id=%u\n", handle, id);
#endif
    }
    c->eax = 0;   /* the previous cursor handle; unused by the menu */
}

/* ---- IME probes ----------------------------------------------------------
 * WC3 imports the IMM32 entry points even though this port answers them with
 * stubs.  The calls still tell us when the game enters or leaves one of its
 * own text fields, which is exactly the point where Android has to show or
 * hide the soft keyboard. */
static void u_imm_get_context(CPU *c)
{
    uint32_t hwnd = arg32(c, 0);
#if WC3_DEBUG
    fprintf(stderr, "[input] ImmGetContext(%08X) from %08X\n",
            hwnd, rd32g(c->esp));
#endif
    /* The game asks for an IME context when a click focuses one of its edit
     * fields.  It can do that inside the window procedure or immediately
     * after DispatchMessage returns, so accept either the in-dispatch flag or
     * a click from the last second.  That is the only reliable "show the soft
     * keyboard" signal: WC3 draws its own caret, stores focus internally and
     * never calls SetFocus or SetCursor for a text field. */
    uint32_t now = hle_now_ms();
    int recent_click = g_ime_click_ms != 0 &&
                       (uint32_t)(now - g_ime_click_ms) < 1000u;
    if (g_ime_click_pending || recent_click) {
        g_ime_click_pending = 0;
        g_ime_hide_pending = 0;
        if (!g_ime_shown) {
            g_ime_shown = 1;
#if WC3_DEBUG
            fprintf(stderr, "[input] text field focused; keyboard shown\n");
#endif
            if (g_keyboard_show) g_keyboard_show();
        }
    }
    c->eax = 0;
}

static void u_imm_associate_context(CPU *c)
{
    uint32_t hwnd = arg32(c, 0), himc = arg32(c, 1);
#if WC3_DEBUG
    fprintf(stderr, "[input] ImmAssociateContext(%08X, %08X)\n", hwnd, himc);
#endif
    /* This is a focus signal, not a blur signal: the game disassociates the
     * old context immediately before asking for a new one.  Hiding here would
     * close the keyboard that ImmGetContext is about to open. */
    c->eax = 0;
}

static void u_imm_release_context(CPU *c)
{
    uint32_t hwnd = arg32(c, 0), himc = arg32(c, 1);
#if WC3_DEBUG
    fprintf(stderr, "[input] ImmReleaseContext(%08X, %08X)\n", hwnd, himc);
#endif
    /* Called after the game has read a composition string, not when the
     * field loses focus.  The deferred click check hides the keyboard. */
    c->eax = 1;
}
static void u_load_image(CPU *c) { c->eax = 0; }
static void u_get_desktop_window(CPU *c) { c->eax = 0x00010000u; }
/* The first CreateWindowExA handle is g_main_hwnd; acting as if the foreground
 * window were a different constant makes the guest's own focus checks fail,
 * even though mouse messages are delivered to the right procedure. */
static void u_get_foreground_window(CPU *c)
{
    c->eax = g_main_hwnd ? g_main_hwnd : 0x00020000u;
    static uint32_t last;
    if (c->eax != last) {
        RES_LOG("GetForegroundWindow -> %08X (main=%08X)",
                c->eax, g_main_hwnd);
        last = c->eax;
    }
}
static void u_get_active_window(CPU *c)
{
    c->eax = g_active_hwnd ? g_active_hwnd :
             (g_main_hwnd ? g_main_hwnd : 0x00020000u);
    static uint32_t last;
    if (c->eax != last) {
        RES_LOG("GetActiveWindow -> %08X (main=%08X)",
                c->eax, g_main_hwnd);
        last = c->eax;
    }
}
static void u_get_focus(CPU *c)
{
    c->eax = g_focus_hwnd ? g_focus_hwnd :
             (g_main_hwnd ? g_main_hwnd : 0x00020000u);
    static uint32_t last;
    if (c->eax != last) {
        RES_LOG("GetFocus -> %08X (main=%08X)", c->eax, g_main_hwnd);
        last = c->eax;
    }
}
static void u_set_foreground_window(CPU *c)
{
    uint32_t hwnd = arg32(c, 0);
    if (hwnd) g_active_hwnd = hwnd;
    c->eax = 1;
}
static void u_set_active_window(CPU *c)
{
    uint32_t old = g_active_hwnd;
    uint32_t hwnd = arg32(c, 0);
    if (hwnd) g_active_hwnd = hwnd;
    c->eax = old ? old : (g_main_hwnd ? g_main_hwnd : 0x00020000u);
}
static void u_set_focus(CPU *c)
{
    uint32_t old = g_focus_hwnd;
    uint32_t hwnd = arg32(c, 0);
    if (hwnd) g_focus_hwnd = hwnd;
    c->eax = old ? old : (g_main_hwnd ? g_main_hwnd : 0x00020000u);
}
static void u_enable_window(CPU *c) { (void)c; c->eax = 1; }
static void u_destroy_window(CPU *c) { (void)c; c->eax = 1; }
static void u_show_window(CPU *c) { (void)c; c->eax = 1; }
static void u_update_window(CPU *c) { (void)c; c->eax = 1; }
static void u_begin_paint(CPU *c) { c->eax = 0x00030000u + (g_fake_next += 4); }
static void u_end_paint(CPU *c) { (void)c; c->eax = 1; }
static void u_fill_rect(CPU *c) { (void)c; c->eax = 1; }
static void u_draw_text(CPU *c) { (void)c; c->eax = 1; }
static void u_get_text_extent(CPU *c)
{
    uint32_t p = arg32(c, 2);
    if (p && guest_ok(p, 8)) { wr32g(p, 8); wr32g(p + 4, 16); }
    c->eax = 1;
}
static void u_get_key_state(CPU *c)
{
    uint32_t vk = arg32(c, 0) & 0xFF;
    mouse_poll();
    /* Win32 reports a pressed mouse button with the high bit set.  The engine
     * tests it as a signed short, so 0x8000 (not 1) is the value that means
     * "down".  Keyboard state is changed only by dispatched messages, so the
     * window procedure sees DOWN inside WM_KEYDOWN and UP inside WM_KEYUP. */
    uint16_t state;
    switch (vk) {
    case VK_LBUTTON:
        state = (g_mouse_buttons & 0x1) ? 0x8000u : 0u;
        break;
    case VK_RBUTTON:
        state = (g_mouse_buttons & 0x4) ? 0x8000u : 0u;
        break;
    case VK_MBUTTON:
        state = (g_mouse_buttons & 0x2) ? 0x8000u : 0u;
        break;
    default:
        state = hle_key_state_get(&g_key_state, (int)vk);
        break;
    }
    c->eax = state;
#if WC3_DEBUG
    if (vk == VK_LBUTTON) {
        static unsigned n;
        if (n++ < 12)
            fprintf(stderr, "[input] GetKeyState(VK_LBUTTON) -> %08X\n", c->eax);
    }
#endif
}
static void u_get_cursor_pos(CPU *c)
{
    uint32_t p = arg32(c, 0);
    mouse_poll();
    if (p && guest_ok(p, 8)) {
        wr32g(p, (uint32_t)g_mouse_x);
        wr32g(p + 4, (uint32_t)g_mouse_y);
    }
    c->eax = 1;
#if WC3_DEBUG
    {
        static unsigned n;
        if (n++ < 12)
            fprintf(stderr, "[input] GetCursorPos -> %d,%d buttons=%d\n",
                    g_mouse_x, g_mouse_y, g_mouse_buttons);
    }
#endif
#if WC3_DEBUG && WC3_DEBUG_RESOLUTION
    {
        static int32_t lx = -0x7fffffff, ly = -0x7fffffff;
        if (g_mouse_x != lx || g_mouse_y != ly) {
            RES_LOG("GetCursorPos -> %d,%d win=%d,%d %ux%u",
                    g_mouse_x, g_mouse_y, g_win_x, g_win_y, g_win_w, g_win_h);
            lx = g_mouse_x; ly = g_mouse_y;
        }
    }
#endif
}
static void u_set_cursor_pos(CPU *c)
{
    int32_t x = (int32_t)arg32(c, 0), y = (int32_t)arg32(c, 1);
    /* Apply the guest's own warps, but never while a finger is down: the
     * engine must not pull the cursor back from under a touch drag. */
    if (!g_mouse_buttons) { g_mouse_x = x; g_mouse_y = y; }
    c->eax = 1;
}
/* GetCursorPos already answers in client coordinates and Win32
 * ScreenToClient/ClientToScreen are no-ops here, so the pair stays
 * self-consistent.  Log the guest point to show whether that assumption still
 * holds when the window origin is not zero. */
static void u_screen_to_client(CPU *c)
{
    uint32_t p = arg32(c, 1);
    if (p && guest_ok(p, 8)) {
        int32_t x = (int32_t)rd32g(p), y = (int32_t)rd32g(p + 4);
        static int32_t lx = -0x7fffffff, ly = -0x7fffffff;
        if (x != lx || y != ly) {
            RES_LOG("ScreenToClient(%d,%d) -> %d,%d (win origin %d,%d)",
                    x, y, x, y, g_win_x, g_win_y);
            lx = x; ly = y;
        }
    }
    c->eax = 1;
}
static void u_client_to_screen(CPU *c)
{
    uint32_t p = arg32(c, 1);
    if (p && guest_ok(p, 8)) {
        int32_t x = (int32_t)rd32g(p), y = (int32_t)rd32g(p + 4);
        static int32_t lx = -0x7fffffff, ly = -0x7fffffff;
        if (x != lx || y != ly) {
            RES_LOG("ClientToScreen(%d,%d) -> %d,%d (win origin %d,%d)",
                    x, y, x, y, g_win_x, g_win_y);
            lx = x; ly = y;
        }
    }
    c->eax = 1;
}
static void u_get_window_long(CPU *c)
{
    uint32_t hwnd = arg32(c, 0);
    int32_t index = (int32_t)arg32(c, 1);
    /* GWL_USERDATA / GWLP_USERDATA are -21 (0xFFFFFFEB).  The main window's
     * procedure reads it on every message to find its UI object. */
    if (index == -21 && (!hwnd || hwnd == g_main_hwnd))
        c->eax = g_main_userdata;
    else
        c->eax = 0;
}
static void u_set_window_long(CPU *c)
{
    uint32_t hwnd = arg32(c, 0);
    int32_t index = (int32_t)arg32(c, 1);
    uint32_t value = arg32(c, 2);
    uint32_t old = 0;
    if (index == -21 && (!hwnd || hwnd == g_main_hwnd)) {
        old = g_main_userdata;
        g_main_userdata = value;
    }
    c->eax = old;
}
/* Class-long values belong to the real window class, which this host does not
 * have.  They must not share the window-long path: GWL_USERDATA is index -21,
 * so a SetClassLongA carrying that index would overwrite the object pointer the
 * window procedure reads on every message.  Real Windows rejects the index and
 * leaves the window data alone, and so does this. */
static void u_get_class_long(CPU *c) { (void)c; c->eax = 0; }
static void u_set_class_long(CPU *c) { (void)c; c->eax = 0; }
static void u_get_window_text(CPU *c)
{
    uint32_t buf = arg32(c, 1), n = arg32(c, 2);
    if (buf && n && guest_ok(buf, n)) ((char *)(uintptr_t)buf)[0] = 0;
    c->eax = 0;
}
static void u_get_window_text_length(CPU *c) { (void)c; c->eax = 0; }
static void u_set_window_text(CPU *c) { (void)c; c->eax = 1; }
static void u_message_beep(CPU *c) { (void)c; c->eax = 1; }
static void u_is_window_visible(CPU *c) { (void)c; c->eax = 1; }
static void u_is_window_enabled(CPU *c) { (void)c; c->eax = 1; }

/* --- display modes -------------------------------------------------------
 * The game treats the display as a desktop with a list of modes, while the
 * Android app has one surface whose size is the device screen.  Keep one
 * table, in this file, and let both the USER32 HLE and the D3D9 adapter read
 * it.  The table is deliberately device-relative rather than a list of PC
 * resolutions: heights start at 360 pixels and grow in 180-pixel steps to the
 * native surface height, with the width chosen so the aspect ratio always
 * matches the window.  The native height is always the final entry.
 *
 * Offsets below are those of DEVMODEA on 32-bit Windows: dmSize 36, dmFields
 * 40, dmBitsPerPel 104, dmPelsWidth 108, dmPelsHeight 112,
 * dmDisplayFrequency 120. */
#define DISPLAY_MODE_MIN_HEIGHT 360u
#define DISPLAY_MODE_HEIGHT_STEP 180u
#define DISP_CHANGE_BADMODE 0xFFFFFFFEu

static uint32_t g_screen_w = 800, g_screen_h = 600;
static int      g_screen_known;
static uint32_t g_display_generation = 1;
static uint32_t g_display_w = 800, g_display_h = 600;
static int      g_display_known;
static int      g_guest_started;

/* 1.27-only fixup, implemented by the widescreen wrapper object.  The weak
 * reference stays zero on the 1.26 image, so that build is untouched. */
#if defined(__GNUC__)
extern void hle_widescreen127_client_size_changed(void) __attribute__((weak));
#endif

#define DM_BITSPERPEL        0x00040000u
#define DM_PELSWIDTH         0x00080000u
#define DM_PELSHEIGHT        0x00100000u
#define DM_DISPLAYFREQUENCY  0x00400000u

static void ensure_screen_known(void)
{
    if (!g_screen_known) {
        /* The shell can reach enumeration before native_activity gets a
         * surface; use the same 800x600 the old fixed window used. */
        g_screen_w = 800;
        g_screen_h = 600;
        g_screen_known = 1;
    }
}

static uint32_t display_mode_count_for_screen(void)
{
    uint32_t native = g_screen_h ? g_screen_h : 600u;
    if (native <= DISPLAY_MODE_MIN_HEIGHT) return 1u;
    uint32_t count = 1u + (native - DISPLAY_MODE_MIN_HEIGHT +
                           DISPLAY_MODE_HEIGHT_STEP - 1u) /
                              DISPLAY_MODE_HEIGHT_STEP;
    if (count > HLE_ANDROID_DISPLAY_MODE_MAX)
        count = HLE_ANDROID_DISPLAY_MODE_MAX;
    return count;
}

static int display_mode_for_index(uint32_t index, uint32_t *w, uint32_t *h)
{
    ensure_screen_known();
    uint32_t count = display_mode_count_for_screen();
    if (index >= count) return 0;
    uint32_t hh = DISPLAY_MODE_MIN_HEIGHT + index * DISPLAY_MODE_HEIGHT_STEP;
    /* The final entry is the native height even when it is not on the step
     * grid (for example 600 on an 800x600 shell window). */
    if (hh > g_screen_h || index + 1u == count) hh = g_screen_h;
    uint64_t ww = ((uint64_t)hh * (uint64_t)g_screen_w) /
                  (g_screen_h ? (uint64_t)g_screen_h : 1u);
    if (!ww) ww = 1;
    if (w) *w = (uint32_t)ww;
    if (h) *h = hh;
    return 1;
}

void hle_android_set_screen_size(uint32_t width, uint32_t height)
{
    if (!width || !height) return;
    int changed = !g_screen_known || width != g_screen_w || height != g_screen_h;
    g_screen_w = width;
    g_screen_h = height;
    g_screen_known = 1;
    if (changed) g_display_generation++;
    if (changed || !g_display_known) {
        uint32_t dw, dh;
        hle_android_default_display_mode(&dw, &dh);
        hle_android_select_display_mode(dw, dh);
    }
    RES_LOG("screen %ux%u gen=%u current=%ux%u", g_screen_w, g_screen_h,
            g_display_generation, g_display_w, g_display_h);
}

uint32_t hle_android_display_generation(void) { return g_display_generation; }
uint32_t hle_android_display_mode_count(void)
{
    ensure_screen_known();
    return display_mode_count_for_screen();
}
uint32_t hle_android_display_refresh_rate(void) { return 60u; }

int hle_android_display_mode(uint32_t index, uint32_t *width, uint32_t *height)
{
    return display_mode_for_index(index, width, height);
}

int hle_android_default_display_mode(uint32_t *width, uint32_t *height)
{
    uint32_t count = hle_android_display_mode_count();
    return hle_android_display_mode(count - 1u, width, height);
}

int hle_android_current_display_mode(uint32_t *width, uint32_t *height)
{
    if (g_display_known) {
        if (width) *width = g_display_w;
        if (height) *height = g_display_h;
        return 1;
    }
    return hle_android_default_display_mode(width, height);
}

int hle_android_display_mode_supported(uint32_t width, uint32_t height)
{
    uint32_t count = hle_android_display_mode_count();
    for (uint32_t i = 0; i < count; i++) {
        uint32_t w, h;
        if (hle_android_display_mode(i, &w, &h) && w == width && h == height)
            return 1;
    }
    return 0;
}

int hle_android_select_display_mode(uint32_t width, uint32_t height)
{
    int index = -1;
    uint32_t count = hle_android_display_mode_count();
    for (uint32_t i = 0; i < count; i++) {
        uint32_t w, h;
        if (hle_android_display_mode(i, &w, &h) && w == width && h == height) {
            index = (int)i;
            break;
        }
    }
    int changed = !g_display_known || g_display_w != width ||
                  g_display_h != height || g_win_w != width || g_win_h != height;
    g_display_w = width;
    g_display_h = height;
    g_display_known = 1;
    /* The guest-visible client rectangle follows the selected mode even when
     * the game never calls SetWindowPos (the Android window has no real frame).
     * This is only the guest/back-buffer size; the Android surface and DXVK's
     * present extent stay at the native size so the image is scaled to fill
     * the display and touch remains in one fixed surface space. */
    g_win_w = width;
    g_win_h = height;
    if (changed && g_guest_started && g_game_module_mapped) {
#if defined(__GNUC__)
        if (hle_widescreen127_client_size_changed)
            hle_widescreen127_client_size_changed();
#endif
    }
    if (index >= 0) {
        RES_LOG("select %ux%u -> mode %d (height %u/%u)", width, height, index,
                height, g_screen_h);
        return 1;
    }
    RES_LOG("select %ux%u -> not in device mode table", width, height);
    return 0;
}

static void u_get_system_metrics(CPU *c)
{
    uint32_t index = arg32(c, 0);
    uint32_t w, h;
    if (!hle_android_current_display_mode(&w, &h)) { w = 800; h = 600; }
    switch (index) {
    case 0:  /* SM_CXSCREEN */
    case 61: /* SM_CXMAXIMIZED */
    case 78: /* SM_CXVIRTUALSCREEN */
        c->eax = w;
        break;
    case 1:  /* SM_CYSCREEN */
    case 62: /* SM_CYMAXIMIZED */
    case 79: /* SM_CYVIRTUALSCREEN */
        c->eax = h;
        break;
    case 80: /* SM_CMONITORS */
        c->eax = 1;
        break;
    default:
        /* The old HLE answered 800 to every metric.  Keep that for the
         * unrelated metrics so this change only affects screen geometry. */
        c->eax = 800;
        break;
    }
    RES_LOG("GetSystemMetrics(%u) -> %u (screen %ux%u)", index, c->eax, w, h);
}

static void u_enum_display_settings(CPU *c)
{
    uint32_t num = arg32(c, 1), dm = arg32(c, 2);
    uint32_t w = 0, h = 0, hz = hle_android_display_refresh_rate();
    if (!dm || !guest_ok(dm, 156)) { g_last_error = 0; c->eax = 0; return; }
    if (num == 0xFFFFFFFFu || num == 0xFFFFFFFEu) {
        /* ENUM_CURRENT_SETTINGS and ENUM_REGISTRY_SETTINGS.  The game reads
         * ENUM_CURRENT_SETTINGS before its hardcoded fallback table; returning
         * false there leaves uninitialised stack values in its mode globals. */
        if (!hle_android_current_display_mode(&w, &h)) {
            hle_android_default_display_mode(&w, &h);
        }
    } else if (!hle_android_display_mode(num, &w, &h)) {
        g_last_error = 0;
        c->eax = 0;
        RES_LOG("EnumDisplaySettingsA(%u) -> none", num);
        return;
    }
    memset((void *)(uintptr_t)dm, 0, 156);
    wr16g(dm + 36, 156);
    wr32g(dm + 40, DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY);
    wr32g(dm + 104, 32);
    wr32g(dm + 108, w);
    wr32g(dm + 112, h);
    wr32g(dm + 120, hz);
    c->eax = 1;
    RES_LOG("EnumDisplaySettingsA(%u) -> %ux%u@%u", num, w, h, hz);
}

static void u_enum_display_devices(CPU *c)
{
    uint32_t num = arg32(c, 1), dev = arg32(c, 2);
    if (num != 0 || !dev || !guest_ok(dev, 32)) { c->eax = 0; return; }
    memset((void *)(uintptr_t)dev, 0, 32);
    memcpy((void *)(uintptr_t)dev, "\\\\.\\DISPLAY1", 13);
    c->eax = 1;
}

static void u_change_display_settings(CPU *c)
{
    uint32_t dm = arg32(c, 0);
    uint32_t flags = arg32(c, 1);
    if (!dm) {
        /* ChangeDisplaySettings(NULL, 0) means "back to the desktop".  There
         * is no separate desktop on Android, so keep the selected mode. */
        RES_LOG("ChangeDisplaySettings(NULL, %u) -> keep %ux%u",
                flags, g_display_w, g_display_h);
        c->eax = 0;
        return;
    }
    if (!guest_ok(dm, 156)) { c->eax = DISP_CHANGE_BADMODE; return; }
    uint32_t w = rd32g(dm + 108), h = rd32g(dm + 112);
    if (!w || !h) { c->eax = 0; return; }
    int supported = hle_android_display_mode_supported(w, h);
    if (!supported) {
        /* The game's hardcoded fallback table is 800x600 and 640x480.  Those
         * must fail here, otherwise the fallback replaces the device-relative
         * mode that EnumDisplaySettings(ENUM_CURRENT_SETTINGS) just returned. */
        c->eax = DISP_CHANGE_BADMODE;
        RES_LOG("ChangeDisplaySettings(%ux%u, flags=%u) -> BADMODE", w, h, flags);
        return;
    }
    if (!(flags & 0x00000002u)) /* CDS_TEST */
        hle_android_select_display_mode(w, h);
    c->eax = 0;
    RES_LOG("ChangeDisplaySettings(%ux%u, flags=%u) -> ok%s", w, h, flags,
            (flags & 0x00000002u) ? " (test)" : "");
}

static void u_system_parameters_info(CPU *c) { (void)c; c->eax = 1; }
static void u_get_menu(CPU *c) { c->eax = 0; }
static void u_create_menu(CPU *c) { c->eax = 0x00060000u + (g_fake_next += 4); }
static void u_destroy_menu(CPU *c) { (void)c; c->eax = 1; }
static void u_get_menu_item_count(CPU *c) { (void)c; c->eax = 0; }
static void u_open_clipboard(CPU *c) { (void)c; c->eax = 1; }
static void u_close_clipboard(CPU *c) { (void)c; c->eax = 1; }
static void u_empty_clipboard(CPU *c) { (void)c; c->eax = 1; }
static void u_get_clipboard_data(CPU *c) { (void)c; c->eax = 0; }
static void u_set_clipboard_data(CPU *c) { (void)c; c->eax = 1; }
static void u_set_capture(CPU *c) { (void)c; c->eax = 0; }
static void u_release_capture(CPU *c) { (void)c; c->eax = 1; }
static void u_window_from_point(CPU *c) { (void)c; c->eax = 0; }

static void g_create_solid_brush(CPU *c) { c->eax = 0x00070000u + (g_fake_next += 4); }
static void g_get_stock_object(CPU *c) { c->eax = 0x00070000u + (g_fake_next += 4); }
static void g_select_object(CPU *c) { c->eax = 0x00070000u + (g_fake_next += 4); }
static void g_delete_object(CPU *c) { (void)c; c->eax = 1; }
static void g_set_bk_mode(CPU *c) { (void)c; c->eax = 1; }
static void g_set_text_color(CPU *c) { (void)c; c->eax = 1; }
static void g_get_text_extent(CPU *c) { u_get_text_extent(c); }

/* --- registry -----------------------------------------------------------
 * A flat in-memory store.  The launcher opens its settings key and reads the
 * values it wrote last time; when every open fails it silently falls back to
 * defaults, and at least one of those defaults leaves the archive-selection
 * table empty (0x6FD3C650), which is fatal.  Keys therefore have to open and
 * values have to round-trip. */
#define REG_MAX 64
typedef struct {
    char key[96];
    char name[64];
    uint32_t type;
    uint32_t len;
    unsigned char data[512];
    int used;
} RegVal;
static RegVal g_reg[REG_MAX];

static RegVal *reg_find(const char *key, const char *name)
{
    for (int i = 0; i < REG_MAX; i++)
        if (g_reg[i].used && !strcasecmp(g_reg[i].key, key) && !strcasecmp(g_reg[i].name, name))
            return &g_reg[i];
    return NULL;
}

static RegVal *reg_slot(const char *key, const char *name)
{
    RegVal *v = reg_find(key, name);
    if (v) return v;
    for (int i = 0; i < REG_MAX; i++) {
        if (!g_reg[i].used) {
            g_reg[i].used = 1;
            snprintf(g_reg[i].key, sizeof g_reg[i].key, "%s", key);
            snprintf(g_reg[i].name, sizeof g_reg[i].name, "%s", name);
            g_reg[i].type = 1;
            g_reg[i].len = 0;
            return &g_reg[i];
        }
    }
    return NULL;
}

/* The handle encodes the index into g_reg of the *key*, not of a value, so a
 * key handle stays valid while values are added. */
#define REG_HKEY_BASE 0x00030000u
/* The subkey most recently opened.  Queries and writes resolve against it, so
 * "Video\reswidth" and "Misc\reswidth" stay distinct. */
static char g_reg_key[96] = "Software\\Blizzard Entertainment\\Warcraft III";

/* Only the game's own subtree exists.  Answering "open" for every key would
 * also turn off the launcher's first-run detection for unrelated keys, which
 * decides whether it writes its defaults. */
static int reg_is_ours(const char *sub)
{
    if (!sub) return 0;
    while (*sub == '\\') sub++;
    return !strncasecmp(sub, "Software\\Blizzard Entertainment\\Warcraft III",
                        strlen("Software\\Blizzard Entertainment\\Warcraft III"));
}

static void a_reg_open_key(CPU *c)
{
    const char *sub = gstr(arg32(c, 1));
    uint32_t out = arg32(c, 4);
    if (!reg_is_ours(sub)) {
#if WC3_DEBUG
        fprintf(stderr, "[reg] RegOpenKeyExA(\"%s\") -> not ours\n", sub);
#endif
        g_last_error = 2;
        c->eax = 2;
        return;
    }
    snprintf(g_reg_key, sizeof g_reg_key, "%s", sub);
#if WC3_DEBUG
    fprintf(stderr, "[reg] RegOpenKeyExA(\"%s\")\n", g_reg_key);
#endif
    if (out && guest_ok(out, 4)) wr32g(out, REG_HKEY_BASE);
    g_last_error = 0;
    c->eax = 0;
}

static void a_reg_query_value(CPU *c)
{
    const char *name = gstr(arg32(c, 1));
    uint32_t ptype = arg32(c, 3), buf = arg32(c, 4), plen = arg32(c, 5);
    RegVal *v = reg_find(g_reg_key, name);
#if WC3_DEBUG
    fprintf(stderr, "[reg] RegQueryValueExA(\"%s\") -> %s\n", name, v ? "found" : "missing");
#endif
    if (!v) { g_last_error = 2; c->eax = 2; return; }
    if (ptype && guest_ok(ptype, 4)) wr32g(ptype, v->type);
    if (!buf || !plen || !guest_ok(plen, 4)) { c->eax = 0; return; }
    uint32_t cap = rd32g(plen);
    uint32_t n = v->len < cap ? v->len : cap;
    if (guest_ok(buf, n)) memcpy((void *)(uintptr_t)buf, v->data, n);
    wr32g(plen, v->len);
    g_last_error = 0;
    c->eax = 0;
}

static void a_reg_set_value(CPU *c)
{
    const char *name = gstr(arg32(c, 1));
    uint32_t type = arg32(c, 2), data = arg32(c, 4), len = arg32(c, 5);
    RegVal *v = reg_slot(g_reg_key, name);
    if (!v) { g_last_error = 8; c->eax = 8; return; }
    v->type = type;
    v->len = len > sizeof v->data ? (uint32_t)sizeof v->data : len;
    if (data && guest_ok(data, v->len)) memcpy(v->data, (void *)(uintptr_t)data, v->len);
#if WC3_DEBUG
    fprintf(stderr, "[reg] RegSetValueExA(\"%s\") len=%u\n", name, len);
#endif
    g_last_error = 0;
    c->eax = 0;
}

static void a_reg_create_key(CPU *c)
{
    const char *sub = gstr(arg32(c, 1));
    uint32_t disp = arg32(c, 3), out = arg32(c, 4);
    (void)disp;
    if (sub && *sub) snprintf(g_reg_key, sizeof g_reg_key, "%s", sub);
    if (out && guest_ok(out, 4)) wr32g(out, REG_HKEY_BASE);
#if WC3_DEBUG
    fprintf(stderr, "[reg] RegCreateKeyExA(\"%s\")\n", g_reg_key);
#endif
    g_last_error = 0;
    c->eax = 0;
}

static void a_reg_close_key(CPU *c) { (void)c; g_last_error = 0; c->eax = 0; }

/* A fresh Windows install has the game's Sound key populated by the
 * installer/first run.  The Android registry starts empty, so every setting
 * read would otherwise look "missing" and the game would use its silent
 * defaults.  Seed the same sane defaults a normal user has: music and effects
 * on, positional/environmental audio on, both volumes at 100%.  Do not seed
 * "nosoundwarn": the sound path is real now, so the game's own failure panel
 * must be allowed to appear if initialization ever regresses. */
static void reg_seed_sound_defaults(void)
{
    static const struct { const char *name; uint32_t value; } d[] = {
        { "music",         1 },
        { "musicvolume",   100 },
        { "sfx",           1 },
        { "sfxvolume",     100 },
        { "ambient",       1 },
        { "movement",      1 },
        { "unit",          1 },
        { "positional",    1 },
        { "environmental", 1 },
    };
    for (size_t i = 0; i < sizeof d / sizeof d[0]; i++) {
        RegVal *v = reg_slot("Software\\Blizzard Entertainment\\Warcraft III\\Sound",
                             d[i].name);
        if (!v) continue;
        v->type = 4;             /* REG_DWORD */
        v->len = 4;
        memset(v->data, 0, sizeof v->data);
        v->data[0] = d[i].value;
    }
#if defined(WC3_DEBUG) || defined(WC3_DEBUG_GRAPHICS)
    /* WC3_NOSOUND=1 is the diagnostic companion to the -nosound command-line
     * switch above.  It does not pretend sound succeeded; it only suppresses
     * the game's own failure panel so a map-load investigation is not parked
     * on a modal dialog when Miles cannot open a device. */
    const char *ns = getenv("WC3_NOSOUND");
    if (ns && *ns && strcmp(ns, "0")) {
        RegVal *v = reg_slot("Software\\Blizzard Entertainment\\Warcraft III\\Sound",
                             "nosoundwarn");
        if (v) {
            v->type = 4;         /* REG_DWORD */
            v->len = 4;
            memset(v->data, 0, sizeof v->data);
            v->data[0] = 1;
        }
    }
#endif
}

/* The Android registry is in memory and starts empty.  Without a Video key
 * the launcher falls back to 800x600 before it ever asks D3D9, so seed the
 * default (100% = native surface) plus the colour/refresh fields a normal
 * Windows install has.  The game's own resolution menu overwrites these at
 * run time; only the initial value matters here.
 *
 * lockfb is a DWORD on the Windows reference install and is 0 there.  The
 * Android backend has to present the same value before the launcher reads
 * the Video key; otherwise the missing value takes the launcher down its
 * default path. */
static void reg_seed_resolution_defaults(void)
{
    uint32_t w = 0, h = 0;
    if (!hle_android_default_display_mode(&w, &h)) return;
    static const struct { const char *name; uint32_t value; } d[] = {
        { "reswidth",     0 },
        { "resheight",    0 },
        { "colordepth",   32 },
        { "refreshrate",  60 },
        { "adapter",      0 },
        { "lockfb",       0 },
    };
    for (size_t i = 0; i < sizeof d / sizeof d[0]; i++) {
        uint32_t value = d[i].value;
        if (!strcmp(d[i].name, "reswidth")) value = w;
        if (!strcmp(d[i].name, "resheight")) value = h;
        RegVal *v = reg_slot("Software\\Blizzard Entertainment\\Warcraft III\\Video",
                             d[i].name);
        if (!v) continue;
        v->type = 4;             /* REG_DWORD */
        v->len = 4;
        memset(v->data, 0, sizeof v->data);
        memcpy(v->data, &value, 4);
    }
    RES_LOG("registry video default %ux%u colour=32 refresh=60", w, h);
}

static void a_get_user_name(CPU *c)
{
    uint32_t buf = arg32(c, 0), n = arg32(c, 1);
    const char *s = "player";
    if (buf && n && guest_ok(buf, n)) {
        size_t len = strlen(s);
        if (len >= n) len = n - 1;
        memcpy((void *)(uintptr_t)buf, s, len + 1);
        if (n >= 4) wr32g(n, (uint32_t)len + 1);
    }
    c->eax = 1;
}

static void s_shell_execute(CPU *c)
{
#if WC3_DEBUG
    fprintf(stderr, "[sh] ShellExecuteA(\"%s\")\n", gstr(arg32(c, 2)));
#endif
    c->eax = 0;
}

/* ------------------------------------------------------------------ */
/* Direct3D 9                                                          */
/* ------------------------------------------------------------------ */
/* game.dll does not import d3d9: it calls LoadLibraryA("d3d9.dll") at
 * 6F13FD6D and resolves Direct3DCreate9 with GetProcAddress, so the whole API
 * arrives through the HLE thunk window.  The object model is the host's
 * (runtime/host/hle_d3d9.cpp): one guest vtable per interface, every slot an
 * HLE thunk, `this` in argument 0, __stdcall throughout.
 *
 * What is implemented here is the *null* backend.  It answers the API and
 * reports one fixed adapter so the boot can be followed past DirectX init, and
 * returns D3D_OK for anything that would draw.  The Vulkan backend replaces the
 * bodies, not the plumbing - see docs/ANDROID.md plan step 3. */

#include "d3d9_tables.h"

#define D3D_OK              0x00000000u
#define D3DERR_INVALIDCALL  0x8876086Cu
#define D3DERR_NOTAVAILABLE 0x8876086Au

#define D3D9_KIND_D3D9   0
#define D3D9_KIND_DEVICE 1
#define D3D9_KIND_RES    2

/* Object header, same four dwords the host's make_obj() lays down. */
#define OBJ_VT     0
#define OBJ_KIND   4
#define OBJ_PARENT 8

enum {
    /* 0 means "this thunk is not a d3d9 method slot", so the default op must
     * not be 0 - d3d9_entry uses the table as its validity flag too. */
    OP_NOP = 1,          /* D3D_OK, no output                      */
    OP_QI,               /* hand back the object itself            */
    OP_ADDREF,
    OP_RELEASE,
    OP_ADAPTER_COUNT,
    OP_ADAPTER_IDENT,
    OP_DISPLAY_MODE,
    OP_ADAPTER_MODE_COUNT,
    OP_ENUM_MODES,
    OP_CAPS,
    OP_CREATE_DEVICE,
    OP_TEXTURE_MEM,
    OP_NEWOBJ,           /* write a fresh object of a given iface  */
    OP_SINGLETON,        /* write the d3d9 or the device object    */
    OP_LOCK_RECT,        /* fill D3DLOCKED_RECT with real memory   */
    OP_LOCK_BUFFER       /* fill void **ppbData for Lock()         */
};

static uint32_t g_d3d9_vt[D3D9_IFACE_COUNT];
static uint32_t g_d3d9_d3d_obj, g_d3d9_device_obj;
static int      g_d3d9_ready;
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
static int      g_d3d9_trace;
#endif
static uint8_t  g_d3d9_op[HLE_MAX];
static uint8_t  g_d3d9_iface[HLE_MAX];    /* which interface the slot belongs to */
static uint8_t  g_d3d9_out[HLE_MAX];      /* argument index of the out-pointer */
static uint8_t  g_d3d9_outif[HLE_MAX];    /* interface that argument receives   */

static HleEnt *reg_ent(const char *dll, const char *name, uint16_t ord,
                       HleFn fn, int argc, int cdecl_abi);      /* defined below */
const char *hle_describe(uint32_t thunk);                       /* defined below */

static uint32_t d3d9_obj(uint32_t iface, uint32_t kind, uint32_t parent)
{
    uint32_t o = hle_alloc0(0x10);
    if (!o) return 0;
    wr32g(o + OBJ_VT, g_d3d9_vt[iface]);
    wr32g(o + OBJ_KIND, kind);
    wr32g(o + OBJ_PARENT, parent);
    return o;
}

/* Which interface a Create/Get method hands back, and in which argument
 * (0-based, excluding `this`).  Everything else defaults to D3D_OK. */
static const struct { const char *m; uint8_t iface; uint8_t out; } g_d3d9_outs[] = {
    { "CreateTexture",               D3D9_IFACE_IDirect3DTexture9,        6 },
    { "CreateVolumeTexture",         D3D9_IFACE_IDirect3DVolumeTexture9,  7 },
    { "CreateCubeTexture",           D3D9_IFACE_IDirect3DCubeTexture9,    5 },
    { "CreateVertexBuffer",          D3D9_IFACE_IDirect3DVertexBuffer9,   4 },
    { "CreateIndexBuffer",           D3D9_IFACE_IDirect3DIndexBuffer9,    4 },
    { "CreateRenderTarget",          D3D9_IFACE_IDirect3DSurface9,        6 },
    { "CreateDepthStencilSurface",   D3D9_IFACE_IDirect3DSurface9,        6 },
    { "CreateOffscreenPlainSurface", D3D9_IFACE_IDirect3DSurface9,        4 },
    { "GetBackBuffer",               D3D9_IFACE_IDirect3DSurface9,        3 },
    { "GetRenderTarget",             D3D9_IFACE_IDirect3DSurface9,        1 },
    { "GetDepthStencilSurface",      D3D9_IFACE_IDirect3DSurface9,        0 },
    { "GetSurfaceLevel",             D3D9_IFACE_IDirect3DSurface9,        1 },
    { "GetCubeMapSurface",           D3D9_IFACE_IDirect3DSurface9,        2 },
    { "GetVolumeLevel",              D3D9_IFACE_IDirect3DVolume9,         1 },
    { "CreateStateBlock",            D3D9_IFACE_IDirect3DStateBlock9,     1 },
    { "CreateQuery",                 D3D9_IFACE_IDirect3DQuery9,          1 },
    { "CreateVertexDeclaration",     D3D9_IFACE_IDirect3DVertexDeclaration9, 1 },
    { "CreateVertexShader",          D3D9_IFACE_IDirect3DVertexShader9,   1 },
    { "CreatePixelShader",           D3D9_IFACE_IDirect3DPixelShader9,    1 },
    { "CreateAdditionalSwapChain",   D3D9_IFACE_IDirect3DSwapChain9,      1 },
    { "GetSwapChain",                D3D9_IFACE_IDirect3DSwapChain9,      1 },
    { "GetContainer",                D3D9_IFACE_IDirect3DResource9,       1 },
    { "GetDirect3D",                 D3D9_IFACE_IDirect3D9,               0 },
    { "GetDevice",                   D3D9_IFACE_IDirect3DDevice9,         0 },
};

/* D3DCAPS9 field offsets, so the game's capability checks see a coherent
 * device instead of a zeroed struct (a zero MaxTextureWidth makes it reject
 * the adapter outright). */
#define CAPS_MAXTEXW      88
#define CAPS_MAXTEXH      92
#define CAPS_MAXPRIM     180
#define CAPS_MAXVINDEX   184
#define CAPS_MAXSTREAMS  188
#define CAPS_VSHADER     196
#define CAPS_MAXVCONST   200
#define CAPS_PSHADER     204
#define CAPS_SIZE        212

static void d3d9_fill_caps(uint32_t p)
{
    if (!p || !guest_ok(p, CAPS_SIZE)) return;
    memset((void *)(uintptr_t)p, 0, CAPS_SIZE);
    wr32g(p + 8,  0x00020000u);        /* Caps: D3DCAPS_READ_SCANLINE-ish  */
    wr32g(p + 28, 0x00000020u);        /* DevCaps: D3DDEVCAPS_HWTRANSFORMANDLIGHT */
    wr32g(p + CAPS_MAXTEXW,    2048);
    wr32g(p + CAPS_MAXTEXH,    2048);
    wr32g(p + 96,              256);    /* MaxVolumeExtent */
    wr32g(p + 100,             2048);   /* MaxTextureRepeat */
    wr32g(p + 104,             2048);   /* MaxTextureAspectRatio */
    wr32g(p + 108,             4);      /* MaxAnisotropy */
    wr32g(p + 148,             8);      /* MaxTextureBlendStages */
    wr32g(p + 152,             8);      /* MaxSimultaneousTextures */
    wr32g(p + 156,             0x40u);  /* VertexProcessingCaps */
    wr32g(p + 160,             8);      /* MaxActiveLights */
    wr32g(p + CAPS_MAXPRIM,    0xFFFF);
    wr32g(p + CAPS_MAXVINDEX,  0xFFFF);
    wr32g(p + CAPS_MAXSTREAMS, 16);
    wr32g(p + 192,             256);    /* MaxStreamStride */
    wr32g(p + CAPS_VSHADER,    0xFFFE0101u);  /* vs_1_1, software */
    wr32g(p + CAPS_MAXVCONST,  96);
    wr32g(p + CAPS_PSHADER,    0xFFFFFFFFu);  /* no pixel shaders */
}

static void d3d9_fill_mode(uint32_t p, uint32_t w, uint32_t h)
{
    if (!p || !guest_ok(p, 16)) return;
    wr32g(p + 0, w);
    wr32g(p + 4, h);
    wr32g(p + 8, 60);      /* RefreshRate */
    wr32g(p + 12, 22);     /* D3DFMT_X8R8G8B8 */
}

/* The game enumerates adapter modes with
 * `for (i = 0; EnumAdapterModes(a, fmt, i, &m) == D3D_OK; i++)`, so always
 * returning D3D_OK is an infinite loop: the first run reached mode 0x530C and
 * 21 261 calls before the probe was killed.  A real list with a real end is the
 * whole fix. */
static const struct { uint16_t w, h; } g_d3d9_modes[] = {
    {  640,  480 }, {  800,  600 }, { 1024,  768 }, { 1152,  864 },
    { 1280,  720 }, { 1280,  960 }, { 1280, 1024 }, { 1440,  900 },
    { 1600, 1200 }, { 1920, 1080 },
};
#define D3D9_MODE_COUNT ((unsigned)(sizeof g_d3d9_modes / sizeof g_d3d9_modes[0]))

static void d3d9_fill_ident(uint32_t p)
{
    if (!p || !guest_ok(p, 1100)) return;
    memset((void *)(uintptr_t)p, 0, 1100);
    strcpy((char *)(uintptr_t)p, "wc3-recomp");
    strcpy((char *)(uintptr_t)(p + 512), "Android null adapter");
    strcpy((char *)(uintptr_t)(p + 1024), "\\\\.\\DISPLAY1");
    wr32g(p + 1064, 0x1414);   /* VendorId */
    wr32g(p + 1068, 0x0001);   /* DeviceId */
}

/* LockRect hands the game a writable pointer, so a null backend still needs
 * real memory behind it: the engine uploads texture data through
 * `D3DLOCKED_RECT.pBits`, and leaving the struct untouched makes it write
 * through whatever was on the caller's stack (round 4: address 1).  One
 * scratch surface is enough - the contents are discarded, and every lock is
 * followed by an unlock before the next one. */
#define D3D9_LOCK_W 1024
#define D3D9_LOCK_H 768
#define D3D9_LOCK_PITCH (D3D9_LOCK_W * 4)
static uint32_t g_d3d9_lock_bits;
/* Vertex/index buffers are locked through void **ppbData, not a D3DLOCKED_RECT.
 * The null backend still has to return writable guest memory: the engine fills
 * every mesh it creates, and leaving the out-pointer untouched writes through
 * whatever was on the stack.  16 MiB covers the meshes loaded during boot. */
#define D3D9_BUFFER_BYTES (16u * 1024u * 1024u)
static uint32_t g_d3d9_buffer_bits;

static void d3d9_entry(CPU *c)
{
    unsigned h = (unsigned)((c->eip - GUEST_HLE_BASE) / HLE_STRIDE);
    if (h >= HLE_MAX || !g_d3d9_op[h]) {
        c->eax = D3DERR_INVALIDCALL;
        return;
    }
    uint32_t self = arg32(c, 0);
    unsigned f = g_d3d9_iface[h];
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    if (g_d3d9_trace)
        fprintf(stderr, "[d3d9] %-46s this=%08X a=%08X %08X %08X\n",
                hle_describe(c->eip), self,
                arg32(c, 1), arg32(c, 2), arg32(c, 3));
#endif

    switch (g_d3d9_op[h]) {
    case OP_QI:      c->eax = self ? self : D3DERR_INVALIDCALL; return;
    case OP_ADDREF:  c->eax = 1; return;
    case OP_RELEASE: c->eax = 1; return;
    case OP_ADAPTER_COUNT:     c->eax = 1; return;
    case OP_ADAPTER_MODE_COUNT: c->eax = D3D9_MODE_COUNT; return;
    case OP_TEXTURE_MEM:       c->eax = 256u * 1024u * 1024u; return;
    /* arg32(c, n) counts `this` as n == 0, so parameter k is arg32(c, k + 1).
     * The first cut of this file used the parameter index directly and wrote
     * every out-pointer one slot low: GetDeviceCaps filled address 1,
     * GetAdapterIdentifier filled Flags, and CreateDevice wrote the device into
     * the D3DPRESENT_PARAMETERS struct, leaving *ppDevice null - the game then
     * dereferenced it at 6F142CD4 and died at 0x0. */
    case OP_ADAPTER_IDENT:
        d3d9_fill_ident(arg32(c, 3));            /* GetAdapterIdentifier(a, flags, p) */
        c->eax = D3D_OK; return;
    case OP_DISPLAY_MODE:
        d3d9_fill_mode(arg32(c, f == D3D9_IFACE_IDirect3D9 ? 2 : 1), 1024, 768);
        c->eax = D3D_OK; return;
    case OP_ENUM_MODES: {
        uint32_t i = arg32(c, 3);
        if (i >= D3D9_MODE_COUNT) { c->eax = D3DERR_INVALIDCALL; return; }
        d3d9_fill_mode(arg32(c, 4), g_d3d9_modes[i].w, g_d3d9_modes[i].h);
        c->eax = D3D_OK;
        return;
    }
    case OP_CAPS:
        d3d9_fill_caps(arg32(c, f == D3D9_IFACE_IDirect3D9 ? 3 : 2));
        c->eax = D3D_OK; return;
    case OP_CREATE_DEVICE: {
        uint32_t pp = arg32(c, 6);               /* ppReturnedDeviceInterface */
        if (!g_d3d9_device_obj)
            g_d3d9_device_obj = d3d9_obj(D3D9_IFACE_IDirect3DDevice9, D3D9_KIND_DEVICE, 0);
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
        fprintf(stderr, "[d3d9] CreateDevice(adapter=%u type=%u hwnd=%08X flags=%08X) -> %08X\n",
                arg32(c, 1), arg32(c, 2), arg32(c, 3), arg32(c, 4),
                g_d3d9_device_obj);
#endif
        if (pp && guest_ok(pp, 4)) wr32g(pp, g_d3d9_device_obj);
        c->eax = D3D_OK;
        return;
    }
    case OP_SINGLETON: {
        uint32_t pp = arg32(c, g_d3d9_out[h] + 1);
        uint32_t o = (g_d3d9_outif[h] == D3D9_IFACE_IDirect3D9)
                   ? g_d3d9_d3d_obj : g_d3d9_device_obj;
        if (pp && guest_ok(pp, 4)) wr32g(pp, o);
        c->eax = D3D_OK;
        return;
    }
    case OP_NEWOBJ: {
        uint32_t pp = arg32(c, g_d3d9_out[h] + 1);
        uint32_t o = d3d9_obj(g_d3d9_outif[h], D3D9_KIND_RES, g_d3d9_device_obj);
        if (pp && guest_ok(pp, 4)) wr32g(pp, o);
        c->eax = D3D_OK;
        return;
    }
    case OP_LOCK_RECT: {
        /* LockRect(D3DLOCKED_RECT *pLocked, const RECT *pRect, DWORD flags) */
        uint32_t pl = arg32(c, 1);
        if (!g_d3d9_lock_bits)
            g_d3d9_lock_bits = hle_alloc0(D3D9_LOCK_PITCH * D3D9_LOCK_H);
        if (pl && guest_ok(pl, 8)) {
            wr32g(pl, D3D9_LOCK_PITCH);
            wr32g(pl + 4, g_d3d9_lock_bits);
        }
        c->eax = D3D_OK;
        return;
    }
    case OP_LOCK_BUFFER: {
        /* Lock(offset, size, void **ppbData, flags) */
        uint32_t offset = arg32(c, 1);
        uint32_t pp = arg32(c, 3);
        if (!g_d3d9_buffer_bits)
            g_d3d9_buffer_bits = hle_alloc0(D3D9_BUFFER_BYTES);
        if (offset >= D3D9_BUFFER_BYTES)
            offset = 0;
        if (pp && guest_ok(pp, 4))
            wr32g(pp, g_d3d9_buffer_bits + offset);
        c->eax = D3D_OK;
        return;
    }
    default:
        c->eax = D3D_OK;
        return;
    }
}

static void d3d9_build(void)
{
    if (g_d3d9_ready) return;
    g_d3d9_ready = 1;

    for (unsigned f = 0; f < D3D9_IFACE_COUNT; f++) {
        const D3D9Iface *iface = &g_d3d9_ifaces[f];
        uint32_t vt = hle_alloc0(iface->count * 4u);
        g_d3d9_vt[f] = vt;
        for (unsigned i = 0; i < iface->count; i++) {
            const char *mn = iface->methods[i].name;
            char name[96];
            snprintf(name, sizeof name, "%s::%s", iface->name, mn);
            HleEnt *e = reg_ent("d3d9.dll", name, 0, d3d9_entry,
                                iface->methods[i].argc + 1, 0);
            unsigned h = (unsigned)(e - g_hle);

            /* Every object has QueryInterface/AddRef/Release at 0/1/2, and
             * every other slot defaults to "succeed, change nothing". */
            uint8_t op = OP_NOP;
            if (!strcmp(mn, "QueryInterface")) op = OP_QI;
            else if (!strcmp(mn, "AddRef"))    op = OP_ADDREF;
            else if (!strcmp(mn, "Release"))   op = OP_RELEASE;
            else if (!strcmp(mn, "GetAdapterCount")) op = OP_ADAPTER_COUNT;
            else if (!strcmp(mn, "GetAdapterModeCount")) op = OP_ADAPTER_MODE_COUNT;
            else if (!strcmp(mn, "GetAvailableTextureMem")) op = OP_TEXTURE_MEM;
            else if (!strcmp(mn, "GetAdapterIdentifier")) op = OP_ADAPTER_IDENT;
            else if (!strcmp(mn, "GetAdapterDisplayMode") ||
                     !strcmp(mn, "GetDisplayMode")) op = OP_DISPLAY_MODE;
            else if (!strcmp(mn, "EnumAdapterModes")) op = OP_ENUM_MODES;
            else if (!strcmp(mn, "GetDeviceCaps")) op = OP_CAPS;
            else if (!strcmp(mn, "CreateDevice")) op = OP_CREATE_DEVICE;
            else if (!strcmp(mn, "LockRect")) op = OP_LOCK_RECT;
            else if (!strcmp(mn, "Lock") &&
                     (f == D3D9_IFACE_IDirect3DVertexBuffer9 ||
                      f == D3D9_IFACE_IDirect3DIndexBuffer9)) op = OP_LOCK_BUFFER;
            else {
                for (unsigned k = 0; k < sizeof g_d3d9_outs / sizeof g_d3d9_outs[0]; k++) {
                    if (strcmp(mn, g_d3d9_outs[k].m)) continue;
                    g_d3d9_out[h]   = g_d3d9_outs[k].out;
                    g_d3d9_outif[h] = g_d3d9_outs[k].iface;
                    op = (g_d3d9_outs[k].iface == D3D9_IFACE_IDirect3D9 ||
                          g_d3d9_outs[k].iface == D3D9_IFACE_IDirect3DDevice9)
                       ? OP_SINGLETON : OP_NEWOBJ;
                    break;
                }
            }
            g_d3d9_iface[h] = (uint8_t)f;
            g_d3d9_op[h] = op;
            wr32g(vt + i * 4u, GUEST_HLE_BASE + h * HLE_STRIDE);
        }
    }
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    fprintf(stderr, "[d3d9] guest vtables built: %u interfaces\n", D3D9_IFACE_COUNT);
#endif
}

/* ------------------------------------------------------------------ */
/* backend selection: null vs DXVK                                     */
/* ------------------------------------------------------------------ */

/* The *real* guest Direct3D 9 COM surface is runtime/host/hle_d3d9.cpp - the
 * same file the Windows host links, 324 method slots over 16 interfaces - and
 * on Android it is backed by DXVK (hle_d3d9_dxvk.cpp), which picks 3.1.1 or
 * 1.10.3 when the device is created.  It is compiled
 * into the image only when build_boot_probe.ps1 is given -D3D9, and used only
 * when WC3_D3D9=dxvk is set: the null backend above stays the default, so a
 * DXVK regression can never take the boot milestone down with it.
 *
 * Weak on purpose.  Without -D3D9 the symbol does not exist at all, and the
 * check below is what keeps that a supported build instead of a link error. */
extern void hle_register_d3d9(void) __attribute__((weak));

static int g_d3d9_dxvk = -1;

static int d3d9_want_dxvk(void)
{
    if (g_d3d9_dxvk < 0) {
        const char *e = getenv("WC3_D3D9");
        int want = e && strcmp(e, "dxvk") == 0;
        if (want && !hle_register_d3d9) {
            fprintf(stderr, "[d3d9] WC3_D3D9=dxvk but this image has no DXVK "
                            "layer; rebuild with -D3D9.  Using the null backend.\n");
            want = 0;
        }
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
        if (want) fprintf(stderr, "[d3d9] backend: DXVK\n");
#endif
        g_d3d9_dxvk = want;
    }
    return g_d3d9_dxvk;
}

static void h_create_d3d9(CPU *c)
{
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    if (!g_d3d9_trace) {
        const char *tr = getenv("WC3_D3D9_TRACE");
        g_d3d9_trace = tr && *tr ? atoi(tr) : 0;
    }
#endif
    d3d9_build();
    if (!g_d3d9_d3d_obj)
        g_d3d9_d3d_obj = d3d9_obj(D3D9_IFACE_IDirect3D9, D3D9_KIND_D3D9, 0);
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    fprintf(stderr, "[d3d9] Direct3DCreate9(%u) -> guest=%08X (null backend)\n",
            arg32(c, 0), g_d3d9_d3d_obj);
#endif
    c->eax = g_d3d9_d3d_obj;
}

/* ------------------------------------------------------------------ */
/* ijl15.dll's static CRT                                              */
/* ------------------------------------------------------------------ */

/* ijl15.dll is the Intel JPEG Library.  It is not a thin Win32 wrapper: it
 * links a *static* MSVC 8 CRT, so its entry point is __crtDllMain, which
 * builds _environ, installs the stdio handle table and runs the C
 * initialisers before DllMain ever sees DLL_PROCESS_ATTACH.  Those steps call
 * KERNEL32 directly, and a stub that returns 0 there does not merely lose a
 * value - the CRT carries on with a NULL _environ / a bogus codepage and
 * faults several frames below the guest, where the crash handler can only see
 * ijlInit as "the last import entered".
 *
 * runtime/host/hle.c already answers these for the Windows host ("They are
 * pure OS services"); these are the POSIX versions.  Where Windows has no
 * POSIX equivalent (the locale NLS entry points) the answer is a clean
 * failure rather than a plausible-looking table: returning 0 makes MSVCR80
 * fall back to its built-in C-locale tables, which is the correct behaviour,
 * whereas returning success with a zeroed buffer would give it a character
 * classification table in which no character is a letter or a digit.
 *
 * Order matters as much as the answers: every entry below is bound before
 * ijl15's entry runs (boot_probe.c calls it immediately after mapping). */

static uint32_t g_env_a, g_env_w;

/* The CRT walks the block as NAME=VALUE\0 ... \0 and stops at the empty
 * string, so an empty block is a legal (if minimal) environment.  ijl15 reads
 * no variable; it only requires _environ to be a valid NULL-terminated
 * array.  The block is cached because Windows keeps it alive for the life of
 * the process and the CRT may hold the pointer. */
static void h_get_environment_a(CPU *c)
{
    if (!g_env_a) {
        g_env_a = hle_guest_alloc(2);
        if (g_env_a) { wr8g(g_env_a, 0); wr8g(g_env_a + 1, 0); }
    }
    c->eax = g_env_a;
}
static void h_get_environment_w(CPU *c)
{
    if (!g_env_w) {
        g_env_w = hle_guest_alloc(4);
        if (g_env_w) { wr16g(g_env_w, 0); wr16g(g_env_w + 2, 0); }
    }
    c->eax = g_env_w;
}
static void h_free_environment(CPU *c)
{
    /* Same as the Windows host: the block is a cached guest copy that lives
     * until the process exits, so releasing it here is a no-op. */
    (void)c;
    c->eax = 1;
}
static void h_get_environment_variable_a(CPU *c)
{
    /* Nothing is set, so every lookup is "not found".  ERROR_ENVVAR_NOT_FOUND
     * is what the CRT checks for; a 0 return with a stale last-error would
     * make it treat the variable as present-but-empty. */
    (void)c;
    g_last_error = 203;
    c->eax = 0;
}
/* win.ini profile queries: Miles asks for MSS/MSSDebug and uses the default
 * when the key is absent, which is what this host returns.  The output buffer
 * must be terminated even when the default is empty; leaving it untouched
 * makes Miles read stale guest bytes. */
static uint32_t copy_profile_default(CPU *c)
{
    const char *def = gstr(arg32(c, 2));
    uint32_t out = arg32(c, 3), n = arg32(c, 4);
    if (!out || !n || !guest_ok(out, n)) return 0;
    size_t len = def ? strlen(def) : 0;
    if (len >= n) len = n - 1;
    if (len) memcpy((void *)(uintptr_t)out, def, len);
    ((char *)(uintptr_t)out)[len] = 0;
    return (uint32_t)len;
}
static void h_get_profile_string_a(CPU *c) { c->eax = copy_profile_default(c); }
static void h_get_profile_int_a(CPU *c)
{
    const char *def = gstr(arg32(c, 2));
    c->eax = def ? (uint32_t)atoi(def) : 0;
}
static void h_set_error_mode(CPU *c) { (void)c; c->eax = 0; }
static void h_get_error_mode(CPU *c) { (void)c; c->eax = 0x8000; }
static void h_set_std_handle(CPU *c) { (void)c; c->eax = 1; }
static void h_set_handle_count(CPU *c) { c->eax = arg32(c, 0); }
static void h_lstrlen_a(CPU *c)
{
    const char *s = gstr(arg32(c, 0));
    c->eax = s ? (uint32_t)strlen(s) : 0;
}
static void h_lstrcpyn_a(CPU *c)
{
    uint32_t dst = arg32(c, 0), src = arg32(c, 1), n = arg32(c, 2);
    if (dst && n && guest_ok(dst, n)) {
        const char *s = gstr(src);
        uint32_t i = 0;
        while (i + 1 < n && s[i]) { wr8g(dst + i, (uint8_t)s[i]); i++; }
        wr8g(dst + i, 0);
    }
    c->eax = dst;
}

/* CPINFO is 20 bytes: MaxCharSize, DefaultChar[2], LeadByte[12].  The CRT
 * reads MaxCharSize to size its multibyte buffer and copies LeadByte into
 * _mbctype; a zeroed struct makes MaxCharSize 0 and _mbclen() returns 0 for
 * every byte, so the CRT stops consuming its input.  GetACP() is 1251 here
 * (h_get_acp), a single-byte codepage, so MaxCharSize 1 and no lead bytes is
 * the truthful answer for it. */
static void h_get_cp_info(CPU *c)
{
    uint32_t cp = arg32(c, 0), out = arg32(c, 1);
    if (!out || !guest_ok(out, 20)) { c->eax = 0; return; }
    memset((void *)(uintptr_t)out, 0, 20);
    uint32_t maxch = (cp == 65001u) ? 4u : (cp == 1200u || cp == 1201u) ? 2u : 1u;
    wr32g(out, maxch);
    wr8g(out + 4, '?');                 /* DefaultChar[0] */
    c->eax = 1;
}

static void h_get_string_type_a(CPU *c) { (void)c; c->eax = 0; }
static void h_get_string_type_w(CPU *c) { (void)c; c->eax = 0; }
static void h_lcmap_string_a(CPU *c) { (void)c; c->eax = 0; }
static void h_lcmap_string_w(CPU *c) { (void)c; c->eax = 0; }

static void h_rtl_unwind(CPU *c)
{
    /* Same reasoning as the Windows host: the guest runs on the host C stack,
     * so there are no guest SEH frames to unwind.  Reaching this means a guest
     * exception was raised; fail loudly instead of pretending it was handled. */
    fprintf(stderr, "[hle] RtlUnwind(target=%08X ip=%08X rec=%08X) reached\n",
            arg32(c, 0), arg32(c, 1), arg32(c, 2));
    hle_android_report();
    abort();
}

static void h_exit_process(CPU *c)
{
    fprintf(stderr, "[hle] ExitProcess(%u)\n", arg32(c, 0));
    hle_android_report();
    fflush(NULL);
    _exit((int)arg32(c, 0));
}

/* ------------------------------------------------------------------ */
/* registry table                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *dll;
    const char *name;
    HleFn       fn;
    uint16_t    argc;
    uint8_t     cdecl_abi;
    uint16_t    ord;        /* ordinal-only imports match on this, not the name */
} HleDef;

#define K(n, fn, argc)  { "KERNEL32.dll", n, fn, argc, 0 }
#define KC(n, fn, argc) { "KERNEL32.dll", n, fn, argc, 1 }
#define CR(n, fn, argc) { "MSVCR80.dll", n, fn, argc, 1 }
#define U(n, fn, argc)  { "USER32.dll", n, fn, argc, 0 }
#define UC(n, fn, argc) { "USER32.dll", n, fn, argc, 1 }
#define G(n, fn, argc)  { "GDI32.dll", n, fn, argc, 0 }
#define A(n, fn, argc)  { "ADVAPI32.dll", n, fn, argc, 0 }
#define S(n, fn, argc)  { "SHELL32.dll", n, fn, argc, 0 }
#define I(n, fn, argc)  { "IMM32.dll", n, fn, argc, 0 }
#define W(n, fn, argc)  { "WINMM.dll", n, fn, argc, 0 }

static const HleDef g_defs[] = {
    /* ---- d3d9: reached only through LoadLibraryA + GetProcAddress ---- */
    { "d3d9.dll", "Direct3DCreate9", h_create_d3d9, 1, 0 },
    /* ---- KERNEL32: process / module / strings ---- */
    K("GetVersion", h_get_version, 0),
    K("GetVersionExA", h_get_version_ex, 1),
    /* Everything a 2006-era CPU reports is available here; PF_FLOATING_POINT_
     * PRECISION_ERRATA (0) is the one a CRT must answer "no" to. */
    K("IsProcessorFeaturePresent", h_is_processor_feature, 1),
    K("DisableThreadLibraryCalls", h_one, 1),
    K("GetLastError", h_get_last_error, 0),
    K("SetLastError", h_set_last_error, 1),
    K("EncodePointer", h_identity, 1),
    K("DecodePointer", h_identity, 1),
    K("GetSystemInfo", h_get_system_info, 1),
    /* Same layout; a stub that returns without filling it leaves the caller
     * memcpy-ing a garbage length (one run died at 0x1F87C6C1). */
    K("GetNativeSystemInfo", h_get_system_info, 1),
    K("CreateToolhelp32Snapshot", h_toolhelp_snapshot, 2),
    K("GetProcessId", h_zero, 1),
    K("Process32First", h_process32_next, 2),
    K("Process32Next", h_process32_next, 2),
    K("GlobalMemoryStatus", h_global_memory_status, 1),
    K("GetStartupInfoA", h_get_startup_info, 1),
    K("GetCommandLineA", h_get_command_line, 0),
    K("GetProfileStringA", h_get_profile_string_a, 5),
    K("GetProfileIntA", h_get_profile_int_a, 3),
    K("SetErrorMode", h_set_error_mode, 1),
    K("GetErrorMode", h_get_error_mode, 0),
    K("GetModuleFileNameA", h_get_module_file_name, 3),
    K("GetWindowsDirectoryA", h_get_windows_directory, 2),
    K("GetSystemDirectoryA", h_get_system_directory, 2),
    K("GetCurrentDirectoryA", h_get_current_directory, 2),
    K("SetCurrentDirectoryA", h_set_current_directory, 1),
    K("GetFullPathNameA", h_get_full_path_name, 4),
    K("GetModuleHandleA", h_get_module_handle, 1),
    K("LoadLibraryA", h_load_library, 1),
    K("FreeLibrary", h_free_library, 1),
    K("GetProcAddress", h_get_proc_address, 2),
    K("GetACP", h_get_acp, 0),
    K("MultiByteToWideChar", h_multibyte_to_widechar, 6),
    /* Eight parameters, not six: CodePage, dwFlags, lpWideCharStr,
     * cchWideChar, lpMultiByteStr, cbMultiByte, lpDefaultChar,
     * lpUsedDefaultChar.  With argc 6, hle_call() popped 24 bytes instead of
     * 32 and left esp 8 bytes low, so every caller that read a local straight
     * after the call read the wrong slot.  Nothing did until ijl15's static CRT
     * converted its environment block and stored the result as _environ: it
     * read 1 instead of a pointer and then walked it (docs/ANDROID.md round 7).
     * runtime/host/hle.c has always registered 8. */
    K("WideCharToMultiByte", h_widechar_to_multibyte, 8),
    K("GetLocaleInfoA", h_get_locale_info, 4),
    K("GetUserDefaultLCID", h_get_user_default_lcid, 0),
    K("GetSystemDefaultLangID", h_get_system_default_langid, 0),
    K("GetUserDefaultLangID", h_get_user_default_langid, 0),
    K("GetSystemDefaultUILanguage", h_get_system_default_ui_language, 0),
    K("GetUserDefaultUILanguage", h_get_user_default_ui_language, 0),
    K("GetComputerNameA", h_get_computer_name, 2),
    K("FormatMessageA", h_format_message, 7),
    K("GetDateFormatA", h_get_date_format, 6),
    K("GetTimeFormatA", h_get_time_format, 6),

    /* ---- KERNEL32: files ---- */
    K("CreateFileA", h_create_file, 7),
    K("CloseHandle", h_close_handle, 1),
    K("ReadFile", h_read_file, 5),
    K("WriteFile", h_write_file, 5),
    K("FlushFileBuffers", h_flush_file_buffers, 1),
    K("SetFilePointer", h_set_file_pointer, 4),
    K("GetFileSize", h_get_file_size, 2),
    K("GetFileTime", h_get_file_time, 4),
    K("SetFileTime", h_set_file_time, 4),
    K("SetEndOfFile", h_set_end_of_file, 1),
    K("GetFileAttributesA", h_get_file_attributes, 1),
    K("SetFileAttributesA", h_set_file_attributes, 2),
    K("DeleteFileA", h_delete_file, 1),
    K("MoveFileA", h_move_file, 2),
    K("CopyFileA", h_copy_file, 3),
    K("CreateDirectoryA", h_create_directory, 2),
    K("RemoveDirectoryA", h_remove_directory, 1),
    K("FindFirstFileA", h_find_first_file, 2),
    K("FindNextFileA", h_find_next_file, 2),
    K("FindClose", h_find_close, 1),
    K("GetDiskFreeSpaceA", h_get_disk_free_space, 5),
    K("GetDiskFreeSpaceExA", h_get_disk_free_space_ex, 4),
    K("GetLogicalDriveStringsA", h_get_logical_drive_strings, 2),
    K("GetDriveTypeA", h_get_drive_type, 1),
    K("GetVolumeInformationA", h_get_volume_information, 8),
    K("CreateFileMappingA", h_create_file_mapping, 6),
    K("MapViewOfFile", h_map_view_of_file, 5),
    K("UnmapViewOfFile", h_unmap_view_of_file, 1),

    /* ---- KERNEL32: memory ---- */
    K("GlobalAlloc", h_global_alloc, 2),
    K("GlobalFree", h_global_free, 1),
    K("GlobalLock", h_global_lock, 1),
    K("GlobalUnlock", h_global_unlock, 1),
    K("GlobalSize", h_global_size, 1),
    K("LocalAlloc", h_global_alloc, 2),
    K("LocalFree", h_global_free, 1),
    K("VirtualAlloc", h_virtual_alloc, 4),
    K("VirtualFree", h_virtual_free, 3),
    K("VirtualProtect", h_virtual_protect, 4),
    K("VirtualQuery", h_virtual_query, 3),
    K("IsBadReadPtr", h_is_bad_ptr, 2),
    K("IsBadWritePtr", h_is_bad_ptr, 2),
    K("HeapCreate", h_heap_create, 3),
    K("HeapDestroy", h_heap_destroy, 1),
    K("HeapAlloc", h_heap_alloc, 3),
    K("HeapFree", h_heap_free, 3),
    K("HeapReAlloc", h_heap_realloc, 4),
    K("HeapSize", h_heap_size, 3),
    K("HeapLock", h_noop, 1),
    K("HeapUnlock", h_noop, 1),
    K("HeapWalk", h_zero, 2),
    K("GetProcessHeap", h_get_process_heap, 0),
    K("GetProcessHeaps", h_get_process_heaps, 2),

    /* ---- KERNEL32: sync / threads ---- */
    K("CreateEventA", h_create_event, 4),
    K("CreateEventW", h_create_event, 4),
    K("SetEvent", h_set_event, 1),
    K("ResetEvent", h_reset_event, 1),
    K("WaitForSingleObject", h_wait_one, 2),
    K("WaitForMultipleObjects", h_wait_many, 4),
    K("DuplicateHandle", h_dup_handle, 7),
    K("ReleaseSemaphore", h_release_sem, 3),
    K("CreateMutexA", h_create_mutex, 3),
    K("OpenMutexA", h_open_mutex, 3),
    K("ReleaseMutex", h_release_mutex, 1),
    K("CreateSemaphoreA", h_create_semaphore, 4),
    K("CreateThread", h_create_thread, 6),
    K("InitializeCriticalSection", h_init_cs, 1),
    K("DeleteCriticalSection", h_delete_cs, 1),
    K("EnterCriticalSection", h_enter_cs, 1),
    K("LeaveCriticalSection", h_leave_cs, 1),
    K("Sleep", h_sleep, 1),
    K("InterlockedExchange", h_interlocked_xchg, 2),
    K("InterlockedCompareExchange", h_interlocked_cmp, 3),
    K("InterlockedIncrement", h_interlocked_inc, 1),
    K("InterlockedDecrement", h_interlocked_dec, 1),
    K("TlsAlloc", h_tls_alloc, 0),
    K("TlsFree", h_tls_free, 1),
    K("TlsSetValue", h_tls_set, 2),
    K("TlsGetValue", h_tls_get, 1),
    K("GetCurrentProcess", h_get_current_process, 0),
    K("GetCurrentThread", h_get_current_thread, 0),
    K("GetCurrentProcessId", h_get_current_process_id, 0),
    K("GetCurrentThreadId", h_get_current_thread_id, 0),
    K("TerminateProcess", h_terminate_process, 2),
    K("TerminateThread", h_terminate_thread, 2),
    K("SuspendThread", h_suspend_thread, 1),
    K("ResumeThread", h_resume_thread, 1),
    K("SetThreadPriority", h_set_thread_priority, 2),
    K("GetThreadPriority", h_get_thread_priority, 1),
    K("CreateProcessA", h_create_process, 10),
    K("IsDebuggerPresent", h_is_debugger, 0),
    K("RaiseException", h_raise_exception, 4),
    K("OutputDebugStringA", h_output_debug_string, 1),
    K("UnhandledExceptionFilter", h_unhandled_filter, 1),
    K("SetUnhandledExceptionFilter", h_set_unhandled_filter, 1),
    K("CreateIoCompletionPort", h_create_iocp, 4),
    K("GetQueuedCompletionStatus", h_get_queued, 5),
    K("PostQueuedCompletionStatus", h_post_queued, 4),

    /* ---- KERNEL32: time ---- */
    K("GetTickCount", h_get_tick_count, 0),
    K("QueryPerformanceCounter", h_query_perf_counter, 1),
    K("QueryPerformanceFrequency", h_query_perf_freq, 1),
    K("GetSystemTimeAsFileTime", h_get_system_time_as_file_time, 1),
    K("GetSystemTime", h_get_system_time, 1),
    K("GetLocalTime", h_get_local_time, 1),
    K("GetTimeZoneInformation", h_get_time_zone_information, 1),
    K("FileTimeToSystemTime", h_filetime_to_system_time, 2),
    K("FileTimeToLocalFileTime", h_filetime_to_local, 2),
    K("SystemTimeToFileTime", h_systemtime_to_filetime, 2),
    K("CompareFileTime", h_compare_filetime, 2),


    /* ---- KERNEL32: ijl15.dll's static CRT ----
     * Bound arity, not just an answer: an unknown import is registered with
     * argc 0, so hle_call() pops 4 bytes instead of the real count and the
     * caller's frame drifts.  Inside __crtDllMain that shows up as a cookie
     * failure much later.  See the block above the registry table. */
    K("GetEnvironmentStrings", h_get_environment_a, 0),
    K("GetEnvironmentStringsW", h_get_environment_w, 0),
    K("FreeEnvironmentStringsA", h_free_environment, 1),
    K("FreeEnvironmentStringsW", h_free_environment, 1),
    K("GetEnvironmentVariableA", h_get_environment_variable_a, 3),
    K("SetStdHandle", h_set_std_handle, 2),
    K("SetHandleCount", h_set_handle_count, 1),
    K("lstrlenA", h_lstrlen_a, 1),
    K("lstrcpynA", h_lstrcpyn_a, 3),
    K("GetCPInfo", h_get_cp_info, 2),
    K("GetStringTypeA", h_get_string_type_a, 5),
    K("GetStringTypeW", h_get_string_type_w, 4),
    K("LCMapStringA", h_lcmap_string_a, 6),
    K("LCMapStringW", h_lcmap_string_w, 6),
    K("RtlUnwind", h_rtl_unwind, 4),
    K("ExitProcess", h_exit_process, 1),

    /* ---- MSVCR80 ---- */
    CR("fprintf", cr_fprintf, 2),
    CR("fopen", cr_fopen, 2),
    CR("fopen_s", cr_fopen_s, 3),
    S("SHGetFolderPathA", h_sh_get_folder_path, 5),
    CR("fclose", cr_fclose, 1),
    CR("fwrite", cr_fwrite, 4),
    CR("fread", cr_fread, 4),
    CR("fseek", cr_fseek, 3),
    CR("ftell", cr_ftell, 1),
    CR("fflush", cr_fflush, 1),
    CR("feof", cr_feof, 1),
    CR("ferror", cr_ferror, 1),
    CR("setvbuf", cr_setvbuf, 4),
    CR("_strupr", cr_strupr, 1),
    CR("_strlwr", cr_strlwr, 1),
    CR("strnlen", cr_strnlen, 2),
    CR("strrchr", cr_strrchr, 2),
    CR("strstr", cr_strstr, 2),
    CR("strpbrk", cr_strpbrk, 2),
    CR("strspn", cr_strspn, 2),
    CR("strcspn", cr_strcspn, 2),
    CR("_malloc_crt", cr_malloc_crt, 1),
    CR("_fullpath", cr_fullpath, 3),
    CR("strcpy_s", cr_strcpy_s, 3),
    CR("memmove", cr_memmove, 3),
    CR("memcpy", cr_memcpy, 3),
    CR("memset", cr_memset, 3),
    CR("qsort", cr_qsort, 4),
    CR("atof", cr_atof, 1),
    CR("strtoul", cr_strtoul, 3),
    CR("atoi", cr_atoi, 1),
    CR("_itoa", cr_itoa, 3),
    CR("_vsnprintf", cr_vsnprintf, 4),
    CR("__getmainargs", cr_getmainargs, 5),
    CR("_time64", cr_time64, 1),
    CR("_ctime64", cr_ctime64, 1),
    CR("_beginthreadex", cr_beginthreadex, 6),
    CR("_initterm", cr_initterm, 2),
    CR("_initterm_e", cr_initterm, 2),
    CR("exit", cr_exit, 1),
    CR("_exit", cr_exit, 1),
    CR("_cexit", cr_cexit, 0),
    CR("_XcptFilter", cr_xcpt_filter, 2),
    CR("_amsg_exit", cr_amsg_exit, 1),
    CR("_purecall", cr_purecall, 0),
    CR("_invoke_watson", cr_invoke_watson, 4),
    CR("_control87", cr_controlfp, 2),
    CR("_controlfp_s", cr_controlfp_s, 3),
    CR("_clearfp", cr_clearfp, 0),
    CR("_CIsqrt", cr_cisqrt, 0),
    CR("_CIlog10", cr_cilog10, 0),
    CR("_CIpow", cr_cipow, 0),
    CR("_CIfmod", cr_cifmod, 0),
    /* x87-returning math; see cr_arg_f64.  argc is the stack bytes for the
     * reader's benefit only - these are cdecl, so the caller pops them. */
    CR("ceil", cr_f64_ceil, 2),
    CR("floor", cr_f64_floor, 2),
    CR("fabs", cr_f64_fabs, 2),
    CR("sqrt", cr_f64_sqrt, 2),
    CR("sin", cr_f64_sin, 2),
    CR("cos", cr_f64_cos, 2),
    CR("tan", cr_f64_tan, 2),
    CR("asin", cr_f64_asin, 2),
    CR("acos", cr_f64_acos, 2),
    CR("atan", cr_f64_atan, 2),
    CR("exp", cr_f64_exp, 2),
    CR("log", cr_f64_log, 2),
    CR("log10", cr_f64_log10, 2),
    CR("pow", cr_f64_pow, 4),
    CR("fmod", cr_f64_fmod, 4),
    CR("atan2", cr_f64_atan2, 4),
    /* _CIatan2 is the same x87 atan2 under the name MSVCR80 uses
     * internally; game.dll's 3D math imports it directly.  Unlike atan2, its
     * arguments live on the x87 stack, so it must not use cr_f64_atan2. */
    CR("_CIatan2", cr_ciatan2, 0),
    CR("_ismbblead", cr_ismbblead, 1),
    CR("malloc", cr_malloc, 1),
    CR("free", cr_free, 1),
    CR("realloc", cr_realloc, 2),
    CR("calloc", cr_calloc, 2),
    CR("strncpy", cr_strncpy, 3),
    CR("_strdup", cr_strdup, 1),
    CR("strchr", cr_strchr, 2),
    CR("sscanf", cr_sscanf, 2),
    CR("strncmp", cr_strncmp, 3),
    CR("_strnicmp", cr_strnicmp, 3),
    CR("_libm_sse2_sqrt_precise", cr_sqrt_sse2, 0),
    CR("_libm_sse2_tan_precise", cr_tan_sse2, 0),
    CR("_libm_sse2_pow_precise", cr_pow_sse2, 0),
    CR("_libm_sse2_log10_precise", cr_log10_sse2, 0),
    CR("_libm_sse2_log_precise", cr_log_sse2, 0),
    CR("_libm_sse2_exp_precise", cr_exp_sse2, 0),
    CR("_libm_sse2_sin_precise", cr_sin_sse2, 0),
    CR("_libm_sse2_cos_precise", cr_cos_sse2, 0),
    CR("_libm_sse2_atan_precise", cr_atan_sse2, 0),
    CR("_libm_sse2_atan2_precise", cr_atan2_sse2, 0),
    CR("_libm_sse2_asin_precise", cr_asin_sse2, 0),
    CR("_libm_sse2_acos_precise", cr_acos_sse2, 0),
    CR("vsprintf", cr_vsprintf, 3),
    CR("__iob_func", cr_iob_func, 0),
    CR("strerror", cr_strerror, 1),
    CR("vfprintf", cr_vfprintf, 3),
    K("GetStdHandle", h_get_std_handle, 1),
    K("GetFileType", h_get_file_type, 1),
    U("RegisterClassExA", h_register_class_ex, 1),
    U("ClipCursor", h_one, 1),
    U("GetWindowInfo", h_get_window_info, 2),
    K("OpenEventA", h_zero, 3),
    CR("_ismbcspace", h_zero, 1),
    { "WS2_32.dll", "", h_wsa_startup, 2, 0, 115 },   /* WSAStartup */
    { "WS2_32.dll", "", h_htons, 1, 0, 9 },           /* htons */
    { "WS2_32.dll", "", h_ntohs, 1, 0, 15 },          /* ntohs */
    { "WS2_32.dll", "", h_htonl, 1, 0, 8 },           /* htonl */
    { "WS2_32.dll", "", h_ntohl, 1, 0, 14 },          /* ntohl */
    CR("_configthreadlocale", h_zero, 1),
    CR("__set_app_type", h_noop, 1),
    CR("__setusermatherr", h_noop, 1),
    CR("_crt_debugger_hook", h_noop, 0),
    CR("_lock", h_zero, 1),
    CR("_unlock", h_zero, 1),
    CR("_encode_pointer", h_identity, 1),
    CR("_decode_pointer", h_identity, 1),
    CR("_except_handler4_common", h_noop, 5),
    CR("__CxxFrameHandler3", h_noop, 4),
    CR("?terminate@@YAXXZ", h_noop, 0),
    CR("_onexit", h_zero, 1),
    CR("__dllonexit", h_zero, 3),
    CR("isdigit", cr_isdigit, 1),
    CR("isalpha", cr_isalpha, 1),
    CR("isspace", cr_isspace, 1),
    CR("iswspace", cr_iswspace, 1),
    CR("toupper", cr_toupper, 1),
    CR("tolower", cr_tolower, 1),
    CR("atol", cr_atol, 1),
    CR("srand", cr_srand, 1),
    CR("rand", cr_rand, 0),
    CR("atoi", cr_atoi, 1),
    CR("strtol", cr_strtol, 3),
    CR("strtoul", cr_strtoul, 3),
    CR("sprintf", cr_sprintf, 0),
    CR("_snprintf", cr_snprintf, 3),
    CR("_snprintf_s", cr_snprintf_s, 4),
    CR("?_type_info_dtor_internal_method@type_info@@QAEXXZ", h_noop, 1),
    /* The CRT's own entry points.  Each needs its real arity: an understated
     * argc leaves arguments on the guest stack and every later epilogue pops
     * one slot too low, which surfaces as a wild pointer far away. */
    CR("_calloc_crt", h_calloc_crt, 2),
    CR("__crtGetShowWindowMode", h_zero, 0),
    CR("__crtSetUnhandledExceptionFilter", h_noop, 1),
    CR("__crtTerminateProcess", h_noop, 1),
    CR("__crtUnhandledException", h_noop, 1),
    CR("_crt_debugger_hook", h_noop, 1),
    CR("_except1", h_noop, 3),

    /* ---- USER32 ---- */
    U("MessageBoxA", u_message_box, 4),
    U("LoadStringA", u_load_string, 4),
    UC("wsprintfA", u_wsprintf, 2),
    U("CreateWindowExA", u_create_window, 12),
    U("RegisterClassA", u_register_class, 1),
    U("GetMessageA", u_get_message, 4),
    U("PeekMessageA", u_peek_message, 5),
    U("DispatchMessageA", u_dispatch_message, 1),
    U("TranslateMessage", u_translate_message, 1),
    U("DefWindowProcA", u_def_window_proc, 4),
    U("CallWindowProcA", u_def_window_proc, 5),
    U("PostMessageA", u_post_message, 4),
    U("SendMessageA", u_send_message, 4),
    U("GetDC", u_get_dc, 1),
    U("GetDCEx", u_get_dc, 3),
    U("ReleaseDC", u_release_dc, 2),
    U("BeginPaint", u_begin_paint, 2),
    U("EndPaint", u_end_paint, 2),
    U("FillRect", u_fill_rect, 3),
    U("DrawTextA", u_draw_text, 5),
    U("GetClientRect", u_get_client_rect, 2),
    U("GetWindowRect", u_get_window_rect, 2),
    U("SetWindowPos", u_set_window_pos, 7),
    U("GetSysColor", u_get_sys_color, 1),
    U("GetSysColorBrush", u_get_sys_color_brush, 1),
    U("LoadCursorA", u_load_cursor, 2),
    U("LoadImageA", u_load_image, 6),
    U("GetDesktopWindow", u_get_desktop_window, 0),
    U("GetForegroundWindow", u_get_foreground_window, 0),
    U("SetForegroundWindow", u_set_foreground_window, 1),
    U("ShowWindow", u_show_window, 2),
    U("UpdateWindow", u_update_window, 1),
    U("InvalidateRect", u_fill_rect, 3),
    U("GetKeyState", u_get_key_state, 1),
    U("GetCursorPos", u_get_cursor_pos, 1),
    U("ScreenToClient", u_screen_to_client, 2),
    U("ClientToScreen", u_client_to_screen, 2),
    U("GetWindowLongA", u_get_window_long, 2),
    U("SetWindowLongA", u_set_window_long, 3),
    U("GetClassLongA", u_get_class_long, 2),
    U("SetClassLongA", u_set_class_long, 3),
    U("GetWindowTextA", u_get_window_text, 3),
    U("GetWindowTextLengthA", u_get_window_text_length, 1),
    U("SetWindowTextA", u_set_window_text, 2),
    U("MessageBeep", u_message_beep, 1),
    U("IsWindowVisible", u_is_window_visible, 1),
    U("IsWindowEnabled", u_is_window_enabled, 1),
    U("GetSystemMetrics", u_get_system_metrics, 1),
    U("EnumDisplaySettingsA", u_enum_display_settings, 3),
    U("EnumDisplaySettingsExA", u_enum_display_settings, 4),
    U("EnumDisplayDevicesA", u_enum_display_devices, 4),
    U("ChangeDisplaySettingsA", u_change_display_settings, 2),
    U("ChangeDisplaySettingsExA", u_change_display_settings, 5),
    U("SystemParametersInfoA", u_system_parameters_info, 4),
    U("GetMenu", u_get_menu, 1),
    U("CreateMenu", u_create_menu, 0),
    U("CreatePopupMenu", u_create_menu, 0),
    U("DestroyMenu", u_destroy_menu, 1),
    U("GetMenuItemCount", u_get_menu_item_count, 1),
    U("OpenClipboard", u_open_clipboard, 1),
    U("CloseClipboard", u_close_clipboard, 0),
    U("EmptyClipboard", u_empty_clipboard, 0),
    U("GetClipboardData", u_get_clipboard_data, 1),
    U("SetClipboardData", u_set_clipboard_data, 2),
    U("SetCapture", u_set_capture, 1),
    U("ReleaseCapture", u_release_capture, 0),
    U("WindowFromPoint", u_window_from_point, 2),
    U("GetParent", u_get_menu, 1),
    U("GetFocus", u_get_focus, 0),
    U("GetActiveWindow", u_get_active_window, 0),
    U("SetActiveWindow", u_set_active_window, 1),
    U("SetFocus", u_set_focus, 1),
    U("EnableWindow", u_enable_window, 2),
    U("DestroyWindow", u_destroy_window, 1),
    U("GetWindow", u_get_menu, 2),
    U("ShowCursor", h_one, 1),
    U("SetCursor", u_set_cursor, 1),
    U("SetCursorPos", u_set_cursor_pos, 2),

    /* ---- GDI32 ---- */
    G("CreateSolidBrush", g_create_solid_brush, 1),
    G("GetStockObject", g_get_stock_object, 1),
    G("SelectObject", g_select_object, 2),
    G("DeleteObject", g_delete_object, 1),
    G("SetBkMode", g_set_bk_mode, 2),
    G("SetTextColor", g_set_text_color, 2),
    G("GetTextExtentPoint32A", g_get_text_extent, 4),

    /* ---- IMM32: there is no IME, and the honest answers are the ones a window
     * with no input context gives: no previous context (0), and "associated"
     * for the Ex form so the caller does not think IME setup failed.  Both are
     * real arities - 2 and 3 - because a 4-byte pop would drift the caller's
     * frame and surface as a stack-cookie failure far away. ---- */
    I("ImmAssociateContext", u_imm_associate_context, 2),
    I("ImmAssociateContextEx", h_one, 3),
    I("ImmGetContext", u_imm_get_context, 1),
    I("ImmReleaseContext", u_imm_release_context, 2),

    /* ---- ADVAPI32 / SHELL32 ---- */
    A("RegOpenKeyExA", a_reg_open_key, 5),
    A("RegQueryValueExA", a_reg_query_value, 6),
    A("RegSetValueExA", a_reg_set_value, 6),
    A("RegCreateKeyExA", a_reg_create_key, 9),
    A("RegCloseKey", a_reg_close_key, 1),
    A("GetUserNameA", a_get_user_name, 2),
    S("ShellExecuteA", s_shell_execute, 6),
    /* Code-signing surface, answered exactly as the Windows host answers it:
     * game.dll statically imports WINTRUST/CRYPT32 for its "is this binary
     * tampered with" check.  There is no Authenticode store in a recompiled
     * host and the trust decision is policy, not translation, so the verdict is
     * "trusted" and the certificate queries report "nothing". */
    { "WINTRUST.dll", "WinVerifyTrust", h_zero, 3, 0 },
    { "CRYPT32.dll", "CryptQueryObject", h_zero, 11, 0 },
    { "CRYPT32.dll", "CryptMsgGetParam", h_zero, 5, 0 },
    { "CRYPT32.dll", "CryptMsgClose", h_one, 1, 0 },
    { "CRYPT32.dll", "CertFindCertificateInStore", h_zero, 6, 0 },
    { "CRYPT32.dll", "CertGetNameStringW", h_zero, 6, 0 },
    { "CRYPT32.dll", "CertFreeCertificateContext", h_one, 1, 0 },
    { "CRYPT32.dll", "CertCloseStore", h_one, 2, 0 },
    /* blizzard.ax is the DirectShow filter the launcher registers before it
     * loads game.dll.  There is no COM registry here, and the filter is only
     * used by the campaign movies, so registration succeeds with no side
     * effects - the same answer the Windows host gives. */
    { "blizzard.ax", "DllRegisterServer", h_zero, 0, 0 },
    { "blizzard.ax", "DllUnregisterServer", h_zero, 0, 0 },
};

/* ------------------------------------------------------------------ */
/* thunk allocation and call                                           */
/* ------------------------------------------------------------------ */

static HleEnt *find_ent(const char *dll, const char *name, uint16_t ord)
{
    for (unsigned i = 0; i < g_hle_count; i++) {
        HleEnt *e = &g_hle[i];
        if (strcasecmp(e->dll, dll) != 0) continue;
        if (name) {
            if (e->name[0] && strcasecmp(e->name, name) == 0) return e;
        } else if (e->ord == ord && !e->name[0]) {
            return e;
        }
    }
    return NULL;
}

static HleEnt *reg_ent(const char *dll, const char *name, uint16_t ord,
                       HleFn fn, int argc, int cdecl_abi)
{
    if (g_hle_count >= HLE_MAX) {
        fprintf(stderr, "[hle] import table full\n");
        abort();
    }
    HleEnt *e = &g_hle[g_hle_count++];
    memset(e, 0, sizeof *e);
    snprintf(e->dll, sizeof e->dll, "%s", dll);
    if (name) snprintf(e->name, sizeof e->name, "%s", name);
    e->ord = ord;
    e->fn = fn ? fn : hle_stub;
    e->argc = (uint16_t)argc;
    e->cdecl_abi = (uint8_t)cdecl_abi;
    e->used = 1;
    return e;
}

/* The three entry points hle_d3d9.cpp needs (hle_registry.h).  `reg` is the
 * name the Windows host uses for the same operation, so the D3D9 layer needs
 * no #ifdef of its own: it registers its 324 COM slots here and reads them
 * back by thunk address. */
HleEnt *reg(const char *dll, const char *name, uint16_t ord, HleFn fn,
            int argc, int cdecl_abi)
{
    return reg_ent(dll, name, ord, fn, argc, cdecl_abi);
}

HleEnt *hle_entry(uint32_t thunk)
{
    if (thunk < GUEST_HLE_BASE) return NULL;
    uint32_t idx = (thunk - GUEST_HLE_BASE) / HLE_STRIDE;
    if (idx >= g_hle_count) return NULL;
    return &g_hle[idx];
}

uint32_t g_alloc0(uint32_t size)
{
    return hle_alloc0(size);
}

/* The presentation window stand-in (see hle_internal.h).  game.dll tests
 * hFocusWindow for null before it creates its device, and DXVK's offscreen
 * presenter never dereferences it, so a stable non-null value is the whole
 * contract. */
void *hle_host_hwnd(void)
{
    return (void *)(uintptr_t)1;
}

/* ------------------------------------------------------------------ */
/* virtual-user harness (host.h)                                       */
/* ------------------------------------------------------------------ */

/* The Windows host drives its own presentation window with synthetic mouse and
 * keyboard messages - that is what WC3_CLICK_AT / WC3_CLICK_SEC / WC3_KEY_AT
 * do, and it is how the unattended menu tests work.  An `adb shell` process
 * has no window, no message queue and no input device, so these are no-ops:
 * the environment variables are inert here instead of pretending a click
 * happened.  Delivering real touches through the app process is a separate
 * step, and it will replace these bodies rather than add to them. */
void vu_host_pump(void) {}
void vu_host_move(int x, int y) { (void)x; (void)y; }
void vu_host_press(void) {}
void vu_host_release(void) {}
void vu_host_key_down(unsigned vk) { (void)vk; }
void vu_host_key_up(unsigned vk) { (void)vk; }
void vu_host_activate(void) {}
void vu_host_client_size(int *w, int *h) { if (w) *w = 0; if (h) *h = 0; }
void vu_host_input_stats(unsigned *moves, unsigned *downs, unsigned *ups,
                         unsigned *dropped)
{
    if (moves) *moves = 0;
    if (downs) *downs = 0;
    if (ups) *ups = 0;
    if (dropped) *dropped = 0;
}

/* Argument bytes of a decorated __stdcall name ("Foo@12" -> 3 dwords). */
static int decorated_argc(const char *name)
{
    if (!name) return 0;
    const char *at = strrchr(name, '@');
    if (!at || !at[1]) return 0;
    char *end = NULL;
    long n = strtol(at + 1, &end, 10);
    if (end && !*end && n > 0 && n <= 1024 && (n & 3) == 0) return (int)(n / 4);
    return 0;
}

void hle_android_start(const char *exe_path, const char *cmdline)
{
    if (exe_path && *exe_path) g_exe_path = exe_path;
    /* The launcher takes its data directory from its own image path, so that
     * path has to be absolute before it is handed back as "Z:\...".  The caller
     * may pass a bare "war3.exe" (that is what the probe's argv[1] is), and a
     * bare name makes the game look for "war3.exe\War3.mpq" - i.e. treat the
     * executable as a directory. */
    {
        static char g_exe_abs[1024];
        char abs[1024];
        if (realpath(g_exe_path, abs)) {
            snprintf(g_exe_abs, sizeof g_exe_abs, "%s", abs);
            g_exe_path = g_exe_abs;
        }
    }
    posix_to_win(g_exe_path, g_exe_path_win, sizeof g_exe_path_win);
#if WC3_DEBUG
    g_trace = getenv("WC3_A_TRACE");
    g_va_trace = getenv("WC3_VA_TRACE") != NULL;
#endif
#if WC3_DEBUG && WC3_DEBUG_HLE_SYNC
    g_sync_trace = getenv("WC3_SYNC_TRACE") != NULL;
#endif
#if WC3_DEBUG
    g_wait_no_block = getenv("WC3_WAIT_NOBLOCK") != NULL;
#endif
#if WC3_DEBUG
    /* The object watch rides on the same per-dispatch hook as the tree trace,
     * so asking for it has to arm the hook too. */
    g_obj_watch = getenv("WC3_OBJ_WATCH") != NULL;
    g_tex_trace = getenv("WC3_TEX") != NULL;
    {
        const char *fl = getenv("WC3_FN");
        if (fl && *fl) {
            const char *p = fl;
            while (*p && g_fn_probe_n < 16) {
                while (*p == ',' || *p == ' ') p++;
                if (!*p) break;
                char *end = NULL;
                unsigned long v = strtoul(p, &end, 16);
                if (end == p) break;
                g_fn_probe[g_fn_probe_n++] = (uint32_t)v;
                p = end;
            }
        }
    }
    g_dispatch_diag = getenv("WC3_TREE") != NULL || g_obj_watch ||
                      g_tex_trace || g_fn_probe_n != 0;
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
    /* The Battle.net probes need EAX at the next block entry, which is exactly
     * the dispatch hook.  The tag is debug-only; release never sets this. */
    g_dispatch_diag = 1;
#endif
#endif
    heap_init();
#if WC3_DEBUG && WC3_DEBUG_HLE
    hang_watch_start();
#endif
    if (!g_screen_known) {
        uint32_t sw = 800, sh = 600;
        const char *ew = getenv("WC3_DXVK_WIDTH");
        const char *eh = getenv("WC3_DXVK_HEIGHT");
        if (ew && atoi(ew) > 0) sw = (uint32_t)atoi(ew);
        if (eh && atoi(eh) > 0) sh = (uint32_t)atoi(eh);
        hle_android_set_screen_size(sw, sh);
    }
    reg_seed_sound_defaults();
    reg_seed_resolution_defaults();

    /* CRT data imports: the IAT slot holds the address of a pointer cell. */
    g_cmdline_cell = hle_alloc0(4);
    /* argv[0] is what the game uses to find its data directory, and it cuts at
     * the last backslash, so it has to be the Windows form as well. */
    const char *cl = (cmdline && *cmdline) ? cmdline : g_exe_path;
    if (!strcmp(cl, g_exe_path)) cl = g_exe_path_win;
    /* WC3_ARGS forwards real war3.exe switches from the Android front end.
     * It is how a test run starts straight in a map (-loadfile ...) instead of
     * navigating the menus, and it costs nothing for a normal launch.  Quote
     * the executable path because it contains a space: the guest's own
     * scanner stops at the first space otherwise. */
    char args_cmdline[1400];
    const char *extra_args = getenv("WC3_ARGS");
    if (extra_args && *extra_args) {
        if (cl == g_exe_path_win)
            snprintf(args_cmdline, sizeof args_cmdline, "\"%s\" %s",
                     g_exe_path_win, extra_args);
        else
            snprintf(args_cmdline, sizeof args_cmdline, "%s %s", cl, extra_args);
        cl = args_cmdline;
    }
#if defined(WC3_DEBUG) || defined(WC3_DEBUG_GRAPHICS)
    /* WC3_NOSOUND=1: the map-load path can park on the game's own sound
     * failure panel when Miles cannot initialize its device.  The game has a
     * -nosound switch, but the Android front end has no command line of its
     * own, so append it here.  This is a diagnostic switch, not a release
     * default. */
    char ns_cmdline[1500];
    const char *ns = getenv("WC3_NOSOUND");
    if (ns && *ns && strcmp(ns, "0")) {
        snprintf(ns_cmdline, sizeof ns_cmdline, "%s -nosound", cl);
        cl = ns_cmdline;
    }
#endif
    g_cmdline_str = hle_guest_strdup(cl);
    wr32g(g_cmdline_cell, g_cmdline_str);

    g_fmode_cell = hle_alloc0(4);
    wr32g(g_fmode_cell, 0x4000);
    g_commode_cell = hle_alloc0(4);
    wr32g(g_commode_cell, 0);
    g_adjust_fdiv_cell = hle_alloc0(4);
    wr32g(g_adjust_fdiv_cell, 0);

    for (unsigned i = 0; i < sizeof g_defs / sizeof g_defs[0]; i++) {
        /* With DXVK the whole d3d9.dll surface, Direct3DCreate9 included, is
         * registered by hle_register_d3d9() below.  Registering the null
         * backend's entry as well would win: find_ent() returns the first
         * match, and this table is walked before that call. */
        if (d3d9_want_dxvk() && strcmp(g_defs[i].dll, "d3d9.dll") == 0) continue;
        reg_ent(g_defs[i].dll, g_defs[i].name, g_defs[i].ord, g_defs[i].fn,
                g_defs[i].argc, g_defs[i].cdecl_abi);
    }

    if (d3d9_want_dxvk()) hle_register_d3d9();

    /* The CRT resolves the SSE2 libm entry points lazily, so they used to be
     * registered here as null stubs.  Every one of them now has a real handler
     * in g_defs (see cr_*_sse2): a stub returns 0.0, not "nothing", and the
     * guest believes it. */

    /* Guest memory is mapped and the image is ready; only now can the 1.27
     * client-rectangle cache be invalidated safely. */
    g_guest_started = 1;
}

uint32_t hle_thunk(const char *dll, const char *name, uint16_t ordinal)
{
    if (!dll) dll = "?";
    if (!strcasecmp(dll, "MSVCR120.dll")) dll = "MSVCR80.dll";

    if (name && !strcasecmp(dll, "MSVCR80.dll")) {
        if (!strcmp(name, "_acmdln")) return g_cmdline_cell;
        if (!strcmp(name, "_p__fmode") || !strcmp(name, "_fmode")) return g_fmode_cell;
        if (!strcmp(name, "_p__commode") || !strcmp(name, "_commode")) return g_commode_cell;
        if (!strcmp(name, "_adjust_fdiv")) return g_adjust_fdiv_cell;
        if (!strcmp(name, "_HUGE")) {
            static uint32_t huge;
            if (!huge) {
                huge = hle_alloc0(8);
                wr32g(huge + 4, 0x7FF00000u);
            }
            return huge;
        }
    }

    HleEnt *e = find_ent(dll, name, ordinal);
    if (!e) {
        e = reg_ent(dll, name, ordinal, NULL, decorated_argc(name), 0);
        if (e->argc == 0 && name) e->argc = decorated_argc(name);
    }
    return GUEST_HLE_BASE + (uint32_t)(e - g_hle) * HLE_STRIDE;
}

void hle_call(uint32_t thunk, CPU *c)
{
    unsigned idx = (unsigned)((thunk - GUEST_HLE_BASE) / HLE_STRIDE);
    if (idx >= g_hle_count) {
        fprintf(stderr, "[hle] bad thunk %08X\n", thunk);
        abort();
    }
    HleEnt *e = &g_hle[idx];
    e->calls++;
    g_last_cpu = c;

    uint32_t ra = rd32g(c->esp);
    g_ring[g_ring_pos].ra = ra;
    g_ring[g_ring_pos].name = e->name[0] ? e->name : e->dll;
    g_ring[g_ring_pos].esp = c->esp;
    g_ring_pos = (g_ring_pos + 1) % HLE_RING;
    if (g_ring_len < HLE_RING) g_ring_len++;

    if (e->fn == hle_stub) {
        note_missing(e);
        if (e->calls <= 3) {
            fprintf(stderr, "[hle] missing %s!%s ord=%u ra=%08X argc=%u"
                            " stk=%08X %08X %08X %08X %08X %08X -> 0\n",
                    e->dll, e->name[0] ? e->name : "?", (unsigned)e->ord, ra,
                    e->argc, rd32g(c->esp + 4), rd32g(c->esp + 8),
                    rd32g(c->esp + 12), rd32g(c->esp + 16),
                    rd32g(c->esp + 20), rd32g(c->esp + 24));
        }
    }
#if WC3_DEBUG
    else if (g_trace) {
        /* "game" restricts the trace to calls made from game.dll (6F......):
         * the boot path spends its first 20 000 imports inside Storm and the
         * CRT, which is exactly the noise that hides the interesting tail. */
        int want = !strcmp(g_trace, "all") ||
                   (!strcmp(g_trace, "game") && ra >= 0x6F000000u && ra < 0x70000000u) ||
                   strstr(g_trace, e->name) != NULL;
        if (want && g_trace_budget) {
            g_trace_budget--;
            fprintf(stderr, "[hle] %s!%s ra=%08X a=%08X %08X %08X %08X\n",
                    e->dll, e->name, ra, arg32(c, 0), arg32(c, 1),
                    arg32(c, 2), arg32(c, 3));
        }
    }
#endif

    c->eip = thunk;
    uint32_t sbx = c->ebx, ssi = c->esi, sdi = c->edi, sbp = c->ebp;
    uint32_t esp_in = c->esp;
    e->fn(c);
    c->ebx = sbx; c->esi = ssi; c->edi = sdi; c->ebp = sbp;

    /* The cleanup below is a *promise* about the handler, not about the guest:
     * a handler may push onto the guest stack to call back into lifted code,
     * and if it does not restore esp the whole caller chain runs one frame
     * low.  The symptom is never here - it is a `__security_check_cookie`
     * failure thousands of imports later that names nothing - so check the
     * promise at the only place that can still attribute it. */
    uint32_t want = e->cdecl_abi ? 4u : 4u + (uint32_t)e->argc * 4u;
    if (c->esp != esp_in) {
        static unsigned reported;
        if (reported < 24) {
            reported++;
            fprintf(stderr, "[hle] STACK %s!%s argc=%u cdecl=%u moved=%+d\n",
                    e->dll, e->name[0] ? e->name : "?", e->argc, e->cdecl_abi,
                    (int)(c->esp - esp_in));
        }
    }
    /* Forced rather than incremented: a handler that leaves esp moved has
     * already broken the guest's frame, and adding the cleanup on top of the
     * wrong base compounds it. */
    c->esp = esp_in + want;
}

const char *hle_describe(uint32_t thunk)
{
    unsigned idx = (unsigned)((thunk - GUEST_HLE_BASE) / HLE_STRIDE);
    if (idx >= g_hle_count) return "";
    HleEnt *e = &g_hle[idx];
    return e->name[0] ? e->name : e->dll;
}

int hle_missing_count(void) { return g_miss_count; }
const char *hle_missing_at(int i)
{
    return (i >= 0 && i < g_miss_count) ? g_miss[i] : "";
}
unsigned hle_missing_calls(int i)
{
    return (i >= 0 && i < g_miss_count) ? g_miss_calls[i] : 0;
}
void hle_stop_on_game_dll(int on) { g_stop_game_dll = on; }
int hle_reached_game_dll(void) { return g_reached_game_dll; }

/* ------------------------------------------------------------------ */
/* report + crash handling                                             */
/* ------------------------------------------------------------------ */

static char g_report[8192];

static void report_begin(void)
{
    if (g_in_report) return;
    g_in_report = 1;
}

static void report_end(void)
{
    ssize_t ignored = write(2, g_report, strlen(g_report));
    (void)ignored;
    g_in_report = 0;
}

void hle_android_report(void)
{
    report_begin();
    size_t o = 0;
#define OUT(...) do { if (o < sizeof g_report - 1) \
        o += (size_t)snprintf(g_report + o, sizeof g_report - o, __VA_ARGS__); } while (0)
    OUT("\n===== android hle report =====\n");
    OUT("imports bound: %u   guest heap live=%u peak=%u\n",
        g_hle_count, g_heap_used, g_heap_peak);
    OUT("last calls (oldest first):\n");
    for (unsigned i = 0; i < g_ring_len; i++) {
        unsigned p = (g_ring_pos + HLE_RING - g_ring_len + i) % HLE_RING;
        OUT("  %s ra=%08X esp=%08X\n", g_ring[p].name, g_ring[p].ra, g_ring[p].esp);
    }
#if WC3_DEBUG
    /* The ring holds the newest 64; printing from index 0 printed the *oldest*
     * 48 of them, i.e. everything except the tail that explains the crash. */
    {
        unsigned want = g_disp_len < 48 ? g_disp_len : 48;
        OUT("last %u dispatch targets (oldest first):\n", want);
        for (unsigned i = g_disp_len - want; i < g_disp_len; i++) {
            unsigned p = (g_disp_pos + 64 - g_disp_len + i) % 64;
            OUT("  %08X\n", g_disp_ring[p]);
        }
    }
#endif
    OUT("unimplemented imports: %d distinct\n", g_miss_count);
    for (int i = 0; i < g_miss_count && i < 32; i++)
        OUT("  %-40s calls=%u\n", g_miss[i], g_miss_calls[i]);
    OUT("busiest imports:\n");
    for (int k = 0; k < 40; k++) {
        HleEnt *best = NULL;
        for (unsigned i = 0; i < g_hle_count; i++) {
            HleEnt *e = &g_hle[i];
            if (!e->calls) continue;
            if (!best || e->calls > best->calls) best = e;
        }
        if (!best) break;
        OUT("  %-24s %-34s %u\n", best->dll, best->name, best->calls);
        best->calls = 0;
    }
    OUT("==============================\n");
#undef OUT
    report_end();
}

/* A guest object whose [0] is zero is this port's dominant failure: the guest
 * either stored an HLE return value where it expected a vtable, or a
 * constructor never ran.  Eight dwords are enough to tell those apart, and the
 * crash handler runs before anything can move the object. */
static void crash_dump(const char *tag, uint32_t a, int ndw)
{
    char buf[256];
    if (!a || !guest_ok(a, (uint32_t)ndw * 4)) return;
    int n = snprintf(buf, sizeof buf, "[crash] %s %08X:", tag, a);
    ssize_t ig = write(2, buf, (size_t)n);
    (void)ig;
    for (int i = 0; i < ndw; i++) {
        n = snprintf(buf, sizeof buf, " %08X", rd32g(a + (uint32_t)i * 4));
        ig = write(2, buf, (size_t)n);
    }
    ig = write(2, "\n", 1);
    (void)ig;
}

/* Name a host address.  Every lifted function is an exported symbol
 * (`L_<guest VA>`), so one line says both "where" and "which guest function".
 * `off` is from the symbol when there is one, else from the image base, so a
 * host-side `llvm-nm` binary search can always finish the job. */
static void crash_sym(const char *tag, uintptr_t a)
{
    char buf[256];
    Dl_info di;
    const char *sym = "?";
    uintptr_t off = 0, base = 0;
    if (a && dladdr((void *)a, &di)) {
        base = (uintptr_t)di.dli_fbase;
        if (di.dli_sname && di.dli_saddr) {
            sym = di.dli_sname;
            off = a - (uintptr_t)di.dli_saddr;
        } else {
            off = a - base;
        }
    }
    int n = snprintf(buf, sizeof buf, "[crash] %s %llX %s+%llX base=%llX\n", tag,
                     (unsigned long long)a, sym, (unsigned long long)off,
                     (unsigned long long)base);
    ssize_t ig = write(2, buf, (size_t)n);
    (void)ig;
}

typedef struct {
    void **frames;
    int   count;
    int   max;
} BtState;

static _Unwind_Reason_Code bt_cb(struct _Unwind_Context *ctx, void *arg)
{
    BtState *s = (BtState *)arg;
    uintptr_t ip = (uintptr_t)_Unwind_GetIP(ctx);
    if (ip && s->count < s->max)
        s->frames[s->count++] = (void *)ip;
    return _URC_NO_REASON;
}

static void crash_handler(int sig, siginfo_t *si, void *ucv)
{
    char buf[2048];
    int n = snprintf(buf, sizeof buf,
                     "\n[crash] signal %d at %p (code %d)\n",
                     sig, si ? si->si_addr : 0, si ? si->si_code : 0);
    ssize_t ignored = write(2, buf, (size_t)n);
    (void)ignored;

    /* SIGABRT comes from libc's FORTIFY check, not from a bad guest address.
     * The native stack is the only way to see which DXVK lock was touched
     * after it had been destroyed. */
    if (sig == SIGABRT || sig == SIGSEGV || sig == SIGBUS) {
        void *frames[48];
        BtState st = { frames, 0, 48 };
        _Unwind_Backtrace(bt_cb, &st);
        for (int i = 0; i < st.count; i++) {
            n = snprintf(buf, sizeof buf, "[crash] bt %d %p\n",
                         i, frames[i]);
            ignored = write(2, buf, (size_t)n);
            crash_sym("  bt", (uintptr_t)frames[i]);
        }
    }

    /* The arm64 `pc` in the ucontext is the only address that names the
     * faulting instruction.  c->eip below is *not* it: hle_call() leaves it on
     * the last import thunk, so for a crash inside a lifted function that
     * happened after an import returned, c->eip points into our own HLE code.
     * LR is the return address of the function that was executing, which for a
     * fault inside a `static inline` helper (wr32, push32, ...) names the
     * *caller* - exactly the lifted function the guest address belongs to. */
    if (ucv) {
        const ucontext_t *uc = (const ucontext_t *)ucv;
#if defined(__aarch64__)
        uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
        uintptr_t lr = (uintptr_t)uc->uc_mcontext.regs[30];
#elif defined(__arm__)
        uintptr_t pc = (uintptr_t)uc->uc_mcontext.arm_pc;
        uintptr_t lr = (uintptr_t)uc->uc_mcontext.arm_lr;
#elif defined(__x86_64__)
        uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
        uintptr_t lr = 0;
#else
        uintptr_t pc = 0, lr = 0;
#endif
        crash_sym("host pc", pc);
        if (lr) crash_sym("host lr", lr);
    }

    /* The fault address alone is not actionable: a guest read at base-0x1C can
     * come from a hundred places.  The last CPU that entered an import is the
     * one that was running, and its esp/ebp identify the frame. */
    CPU *c = g_last_cpu;
    if (c) {
        n = snprintf(buf, sizeof buf,
                     "[crash] eip=%08X esp=%08X ebp=%08X eax=%08X ebx=%08X "
                     "ecx=%08X edx=%08X esi=%08X edi=%08X\n",
                     c->eip, c->esp, c->ebp, c->eax, c->ebx,
                     c->ecx, c->edx, c->esi, c->edi);
        ignored = write(2, buf, (size_t)n);

        uint32_t sp = c->esp & ~3u;
        if (guest_ok(sp, 4 * 20)) {
            n = snprintf(buf, sizeof buf, "[crash] stack:");
            ignored = write(2, buf, (size_t)n);
            for (int i = 0; i < 20; i++) {
                n = snprintf(buf, sizeof buf, " %08X", rd32g(sp + i * 4));
                ignored = write(2, buf, (size_t)n);
            }
            ignored = write(2, "\n", 1);
        }
#if WC3_DEBUG
        n = snprintf(buf, sizeof buf, "[crash] last dispatch %08X\n",
                     g_disp_len ? g_disp_ring[(g_disp_pos + 63) % 64] : 0);
        ignored = write(2, buf, (size_t)n);
#endif

        /* The guest EBP chain is the only call stack the crash has: the host
         * stack is C frames of the interpreter, not the guest's.  Every lifted
         * function keeps ebp as the guest does, so [ebp] -> caller's ebp and
         * [ebp+4] -> the return address into the caller. */
        {
            uint32_t fp = c->ebp;
            for (int i = 0; i < 12 && fp && guest_ok(fp, 8); i++) {
                uint32_t next = rd32g(fp), ra = rd32g(fp + 4);
                n = snprintf(buf, sizeof buf, "[crash] frame %d ebp=%08X ra=%08X\n",
                             i, fp, ra);
                ignored = write(2, buf, (size_t)n);
                if (next <= fp) break;
                fp = next;
            }
        }

        /* A `__security_check_cookie` failure has two very different causes and
         * the frame alone cannot tell them apart: the per-frame copy at
         * [ebp-4] can be clobbered (a bad write or a wrong ebp), or the global
         * cookie itself can be (a wild write anywhere earlier).  WC3_COOKIE
         * names the global so the report can compare the two. */
        {
            const char *cv = getenv("WC3_COOKIE");
            if (cv && *cv) {
                uint32_t ga = (uint32_t)strtoul(cv, NULL, 16);
                uint32_t gv = guest_ok(ga, 4) ? rd32g(ga) : 0;
                uint32_t fv = (c->ebp && guest_ok(c->ebp - 4, 4)) ? rd32g(c->ebp - 4) : 0;
                n = snprintf(buf, sizeof buf,
                             "[crash] cookie global[%08X]=%08X frame[ebp-4]=%08X\n",
                             ga, gv, fv);
                ignored = write(2, buf, (size_t)n);
            }
        }

#if WC3_DEBUG
        /* The last dispatches, newest last.  A stack cookie failure is
         * diagnosed by the call path, and this ring is the only record of it
         * that survives: the guest EBP chain above is already walking a frame
         * that may be the corrupt one. */
        {
            unsigned cnt = g_disp_len < 64 ? g_disp_len : 64;
            n = snprintf(buf, sizeof buf, "[crash] dispatch:");
            ignored = write(2, buf, (size_t)n);
            for (unsigned i = cnt; i > 0; i--) {
                uint32_t t = g_disp_ring[(g_disp_pos - i) & 63];
                n = snprintf(buf, sizeof buf, " %08X", t);
                ignored = write(2, buf, (size_t)n);
            }
            ignored = write(2, "\n", 1);
        }
#endif

        /* hle_call() sets c->eip to the thunk before running the handler and
         * never restores it, so an eip inside the thunk window means only that
         * an import was the last thing *entered* - it is the faulting import
         * while a handler is still running, and a stale address for a crash in
         * a lifted function that happens after one returned.  Hence "last". */
        if (c->eip >= GUEST_HLE_BASE &&
            c->eip < GUEST_HLE_BASE + (uint32_t)HLE_MAX * HLE_STRIDE) {
            unsigned i = (unsigned)((c->eip - GUEST_HLE_BASE) / HLE_STRIDE);
            if (i < g_hle_count) {
                n = snprintf(buf, sizeof buf, "[crash] last import %s!%s argc=%u\n",
                             g_hle[i].dll, g_hle[i].name[0] ? g_hle[i].name : "?",
                             g_hle[i].argc);
                ignored = write(2, buf, (size_t)n);
            }
        }

        /* The object the guest was about to call through.  `eax == ecx == this`
         * is the shape of a __thiscall that just returned `this` (AddRef), so
         * dumping both is free and usually names the culprit outright. */
        crash_dump("eax obj", c->eax, 8);
        if (c->ecx != c->eax) crash_dump("ecx obj", c->ecx, 8);
    }

    /* A fault inside one of the VirtualAlloc arenas is either the guest
     * touching a block it already released, or us handing out a range we
     * never mapped.  The tables say which. */
    if (si && (uintptr_t)si->si_addr < 0x100000000ull) {
        uint32_t a = (uint32_t)(uintptr_t)si->si_addr;
        if (a >= GUEST_VA_BASE) {
            char d[64];
            va_describe(a, d, sizeof d);
            n = snprintf(buf, sizeof buf, "[crash] arena %08X: %s\n", a, d);
            ignored = write(2, buf, (size_t)n);
        }
    }

    {
        const char *od = getenv("WC3_OBJ_DUMP");
        if (od && *od) crash_dump("WC3_OBJ_DUMP", (uint32_t)strtoul(od, NULL, 16), 16);
    }

    hle_android_report();
    _exit(139);
}

void hle_install_crash_handler(void)
{
    static char altstack[65536];
    stack_t ss;
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    /* The generated dispatcher calls abort() on an unknown guest target; the
     * report has to survive that or the run says nothing about what led to
     * it.  SIGABRT is not a memory fault, so it needs its own registration. */
    sigaction(SIGABRT, &sa, NULL);
}

/* ------------------------------------------------------------------ */
/* dispatcher diagnostics required by the generated table              */
/* ------------------------------------------------------------------ */

#if WC3_DEBUG
/* Argument trace for the handful of guest functions that decide a question the
 * HLE cannot answer from its own side.  Called from the generated dispatcher
 * for *every* dispatch, so the gate has to be a few compares.
 *
 * The archive check (docs/ANDROID.md) is decided by Storm #289.  Its result is
 * in eax, which no host hook can see - but `dispatch_jmp` to L_1501F84B is how
 * the callee reports success, and that *is* a dispatch, so a window over the
 * call tree names the branch that was taken. */
static unsigned g_watch_n;
static int g_tree_left;
/* Set while a watched callee is running: `esp` is the value at its entry, so
 * any later dispatch with a *higher* esp means the call has returned and eax
 * still holds its result (the caller only runs test/branch before the next
 * call).  This is the one way a host hook can read a guest return value. */
static int g_rw_on;
static uint32_t g_rw_esp;
static unsigned g_lf_n;
static unsigned g_open_n;
static unsigned g_err_n;
/* Only the thread that entered the watched call is interesting.  Storm runs an
 * async-I/O thread that polls GetTickCount/WaitForSingleObject in a tight loop
 * (0x1501EB20 -> 0x1501F050 -> ...); left unfiltered it consumes the whole
 * window in a few microseconds and the trace says nothing about the caller.
 * Every guest thread gets its own stack out of GUEST_THREAD_STACK_BASE and the
 * main thread keeps GUEST_STACK_BASE, so the stack address identifies which
 * one is running. */
static uint32_t g_tree_stack;

/* WC3_OBJ_WATCH=1 dumps every object that the generic `IUnknown::AddRef`
 * (0x6F06C390) hands back.  The round-3 crash was a `this` with [0] == 0 - an
 * object that exists and is reference counted but has no vtable - and the only
 * thing that can say *who* produced it is the AddRef's return address.  A zero
 * vtable is printed every time (capped), a healthy one only a few times. */
static unsigned g_obj_watch_zero;
static unsigned g_obj_watch_ok;
#endif

#if WC3_DEBUG
static void watch_dispatch(uint32_t target, uint32_t esp, uint32_t ebp)
{
    if (g_tree_left > 0 && (esp & 0xFF000000u) == g_tree_stack) {
        const char *nm = target >= 0x7F000000u && target < 0x7F100000u
                       ? hle_describe(target) : NULL;
        /* toupper is called once per character per hash pass - 300+ lines of
         * noise that says nothing.  Everything else stays. */
        if (!nm || strcmp(nm, "toupper") != 0) {
            g_tree_left--;
            if (nm)
                fprintf(stderr, "[tree] %08X %-28s ra=%08X stk=%08X %08X %08X\n",
                        target, nm, rd32g(esp), rd32g(esp + 4), rd32g(esp + 8),
                        rd32g(esp + 12));
            else
                fprintf(stderr, "[tree] %08X ra=%08X stk=%08X %08X %08X\n", target,
                        rd32g(esp), rd32g(esp + 4), rd32g(esp + 8), rd32g(esp + 12));
        }
    }
    if (g_watch_n >= 80) return;
    switch (target) {
    case 0x15022170u:      /* Storm #289: SFileOpenArchive(name, priority, flags) */
        g_watch_n++;
        g_tree_left = 700;
        g_tree_stack = esp & 0xFF000000u;
        fprintf(stderr, "[disp] #289(hArc=%08X, \"%s\", scope=%u) ra=%08X\n",
                rd32g(esp + 4), gstr(rd32g(esp + 8)), rd32g(esp + 12),
                rd32g(esp));
        break;
    case 0x6F330AB0u:      /* font table loader: ecx/edx carry the vector and buffer */
        g_watch_n++;
        g_tree_left = 900;
        g_tree_stack = esp & 0xFF000000u;
        fprintf(stderr, "[disp] fontlist(out=%08X) ra=%08X\n",
                rd32g(esp + 4), rd32g(esp));
        break;
    /* Storm #279, the "read a whole file into a malloc'd buffer" helper that
     * 0x6F330C40 uses for font\font.gid.  It returns 0 for font.gid, which is
     * the whole reason the font list is empty and the game shows "unable to
     * initialize".  Arm the tree window here so the trace shows the branch it
     * takes inside. */
    case 0x150224E0u: {    /* Storm #279: read a whole file into a buffer */
        const char *nm = gstr(rd32g(esp + 4));
        if (g_lf_n++ < 24)
            fprintf(stderr, "[disp] LoadFile(\"%s\") &buf=%08X &size=%08X ra=%08X\n",
                    nm, rd32g(esp + 8), rd32g(esp + 12), rd32g(esp));
        if (strstr(nm, "font.")) {
            g_watch_n++;
            g_tree_left = 4000;
            g_tree_stack = esp & 0xFF000000u;
            fprintf(stderr, "[disp]   ^ arming tree window\n");
        }
        break;
    }
    /* The open-by-name step inside LoadFile.  Its name argument is the only
     * thing that decides whether the MPQ hash probe can hit at all. */
    case 0x15022BA0u:
        if (g_open_n++ < 12)
            fprintf(stderr, "[disp]   open(\"%s\", scope=%u) ra=%08X\n",
                    gstr(rd32g(esp + 8)), rd32g(esp + 12), rd32g(esp));
        break;
    /* 0x6F046CF0's loop calls #289 through this thunk, and its frame is intact
     * at that moment: [ebp-0x60] is the count 0x6F330AB0 produced, [ebp-0x64]
     * the entry array.  Reading it here is the only way to see the value - it
     * is a stack local of a function that has already returned by the time the
     * dialog is up. */
    case 0x6F07C792u:
        g_watch_n++;
        fprintf(stderr, "[disp] check-loop: count=%u array=%08X vec.count=%u\n",
                rd32g(ebp - 0x60), rd32g(ebp - 0x64), rd32g(ebp - 0x68));
        break;
    /* The two tails of the MPQ open path: 0x1501F84B is "the hash probe found
     * a block", 0x1501F817 is "the path resolved to a file on disk".  Printing
     * them unconditionally answers "did the lookup succeed" for every name,
     * even when the tree window has already been used up. */
    case 0x1501F84Bu:
        g_watch_n++;
        fprintf(stderr, "[disp]   hash HIT\n");
        break;
    case 0x1501F817u:
        g_watch_n++;
        fprintf(stderr, "[disp]   disk FOUND\n");
        break;
    default:
        break;
    }
}
#endif

#if WC3_DEBUG
void wc3_trace_dispatch(uint32_t target, uint32_t esp, uint32_t ebp){
    g_disp_ring[g_disp_pos++ & 63] = target;
    if (g_disp_len < 64) g_disp_len++;

#if WC3_DEBUG && WC3_DEBUG_HLE
    if (g_self_thread) {
        g_self_thread->last_target = target;
        g_self_thread->last_esp = esp;
        g_self_thread->last_ebp = ebp;
        g_self_thread->dispatch_count++;
    } else {
        g_main_last_target = target;
        g_main_last_esp = esp;
        g_main_last_ebp = ebp;
        g_main_dispatch_count++;
    }
#endif

#if WC3_DEBUG
    /* Temporary stream-start diagnostics.  The game's sound object methods are
     * 0x6F0AD5A0 (open) and 0x6F0AFEF0 (play).  Watching them together with the
     * Mss32 exports tells us which object owns which handle and whether the
     * MP3 open succeeded. */
    if (target == 0x6F0AD5A0u || target == 0x6F0AFEF0u ||
        target == 0x3510B340u || target == 0x3510AD60u ||
        target == 0x3510AE30u || target == 0x3510A9F0u ||
        target == 0x3511CDD0u)
        g_dispatch_diag = 1;
#endif

#if WC3_DEBUG
    /* Temporary sound-path trace: these are the Mss32 exports the game must
     * call to load and start a sample or stream.  Seeing which ones never
     * arrive separates "the game never asked for music" from "Miles failed to
     * decode/mix it".  Keep the count small; this runs on every dispatch. */
    {
        static unsigned mss_api_log;
        if (mss_api_log < 512) {
            uint32_t a0 = guest_ok(esp + 4, 4) ? rd32g(esp + 4) : 0;
            uint32_t a1 = guest_ok(esp + 8, 4) ? rd32g(esp + 8) : 0;
            uint32_t a2 = guest_ok(esp + 12, 4) ? rd32g(esp + 12) : 0;
            uint32_t a3 = guest_ok(esp + 16, 4) ? rd32g(esp + 16) : 0;
            uint32_t a4 = guest_ok(esp + 20, 4) ? rd32g(esp + 20) : 0;
            int hit = 1;
#define MSS_WATCH_RET(name_) do { \
                g_mss_ret_watch = guest_ok(esp, 4) ? rd32g(esp) : 0; \
                g_mss_ret_name = (name_); \
                g_dispatch_diag = 1; \
            } while (0)
            switch (target) {
            case 0x35102A80u:
                fprintf(stderr, "[mss-api] set_preference(%u, %d)\n",
                        a0, (int32_t)a1); break;
            case 0x351041D0u:
                fprintf(stderr, "[mss-api] allocate_sample_handle(dig=%08X)\n", a0); break;
            case 0x351043E0u:
                fprintf(stderr, "[mss-api] release_sample_handle(%08X)\n", a0); break;
            case 0x351044B0u:
                fprintf(stderr, "[mss-api] init_sample(%08X)\n", a0); break;
            case 0x35104580u:
                fprintf(stderr, "[mss-api] set_sample_file(%08X image=%08X block=%u)\n",
                        a0, a1, a2); MSS_WATCH_RET("set_sample_file"); break;
            case 0x35104690u:
                fprintf(stderr, "[mss-api] set_named_sample_file(%08X \"%s\" image=%08X size=%u block=%u)\n",
                        a0, gstr(a1), a2, a3, a4); MSS_WATCH_RET("set_named_sample_file"); break;
            case 0x35104E80u:
                fprintf(stderr, "[mss-api] start_sample(%08X)\n", a0);
                MSS_WATCH_RET("start_sample"); break;
            case 0x351050F0u:
                fprintf(stderr, "[mss-api] end_sample(%08X)\n", a0); break;
            case 0x3510A9F0u: {
                uint32_t cb0 = rd32g(0x3514E504u), cb1 = rd32g(0x3514E508u);
                uint32_t cb2 = rd32g(0x3514E50Cu), cb3 = rd32g(0x3514E510u);
                fprintf(stderr, "[mss-api] open_stream(dig=%08X \"%s\" mem=%u) "
                                "ra=%08X callbacks=%08X/%08X/%08X/%08X\n",
                        a0, gstr(a1), a2,
                        guest_ok(esp, 4) ? rd32g(esp) : 0, cb0, cb1, cb2, cb3);
                MSS_WATCH_RET("open_stream");
                break;
            }
            case 0x3511B670u:
                fprintf(stderr, "[mss-api] set_file_callbacks(%08X,%08X,%08X,%08X)\n",
                        a0, a1, a2, a3); break;
            case 0x3511B5A0u:
                fprintf(stderr, "[mss-api] set_file_async_callbacks(%08X,%08X,%08X,%08X,%08X)\n",
                        a0, a1, a2, a3, a4); break;
            case 0x3510AB00u:
                fprintf(stderr, "[mss-api] close_stream(%08X)\n", a0); break;
            case 0x3510AC90u:
                fprintf(stderr, "[mss-api] start_stream(%08X) ra=%08X\n", a0,
                        guest_ok(esp, 4) ? rd32g(esp) : 0);
                MSS_WATCH_RET("start_stream"); break;
            case 0x3510AD60u:
                fprintf(stderr, "[mss-api] pause_stream(%08X onoff=%d)\n",
                        a0, (int32_t)a1);
                g_dispatch_diag = 1; break;
            case 0x3510B340u:
                fprintf(stderr, "[mss-api] set_stream_loop_count(%08X loop=%d)\n",
                        a0, (int32_t)a1);
                g_dispatch_diag = 1; break;
            case 0x3510AE30u:
                fprintf(stderr, "[mss-api] set_stream_volume(%08X vol=%d) ra=%08X\n",
                        a0, (int32_t)a1,
                        guest_ok(esp, 4) ? rd32g(esp) : 0);
                g_dispatch_diag = 1; break;
            case 0x3510D020u:
                fprintf(stderr, "[mss-api] primary_digital_driver(flags=%08X)\n", a0); break;
            case 0x35102C30u:
                fprintf(stderr, "[mss-api] set_error(\"%s\")\n", gstr(a0)); break;
            case 0x3511CDD0u:
                fprintf(stderr, "[mss-api] stream_init\n"); break;
            case 0x351047B0u:
                fprintf(stderr, "[mss-api] set_sample_processor(sample=%08X index=%u provider=%08X)\n",
                        a0, a1, a2); break;
            case 0x35125C40u:
                fprintf(stderr, "[mss-api] RIB_register_interface(%08X)\n", a0); break;
            case 0x35126360u:
                fprintf(stderr, "[mss-api] RIB_unregister_interface(%08X)\n", a0); break;
            case 0x6F080E80u:
                fprintf(stderr, "[mss-file] open(\"%s\")\n", gstr(a0)); break;
            case 0x6F07E940u:
                fprintf(stderr, "[mss-file] close(handle=%08X)\n", a0); break;
            case 0x6F081580u:
                fprintf(stderr, "[mss-file] seek(handle=%08X off=%d whence=%u)\n",
                        a0, (int32_t)a1, a2); break;
            case 0x6F0810C0u:
                fprintf(stderr, "[mss-file] read(handle=%08X buf=%08X len=%u)\n",
                        a0, a1, a2); break;
            default:
                hit = 0;
                break;
            }
            if (hit) mss_api_log++;
        }
    }

    if (g_trace_mssfast_open &&
        target >= GUEST_IMAGE_MSSFAST &&
        target < GUEST_IMAGE_MSSFAST + 0x00020000u)
        fprintf(stderr, "[mss] mssfast dispatch %08X esp=%08X\n",
                target, esp);
#endif

#if WC3_DEBUG
    {
        static unsigned asi_log;
        if (target == 0x3510A9F0u) asi_log = 0;
        if (target >= GUEST_IMAGE_MP3DEC &&
            target < GUEST_IMAGE_MP3DEC + 0x0002A000u &&
            asi_log < 600) {
            uint32_t a0 = guest_ok(esp + 4, 4) ? rd32g(esp + 4) : 0;
            uint32_t a1 = guest_ok(esp + 8, 4) ? rd32g(esp + 8) : 0;
            uint32_t a2 = guest_ok(esp + 12, 4) ? rd32g(esp + 12) : 0;
            uint32_t a3 = guest_ok(esp + 16, 4) ? rd32g(esp + 16) : 0;
            fprintf(stderr, "[asi] %08X a0=%08X a1=%08X a2=%08X a3=%08X\n",
                    target, a0, a1, a2, a3);
            asi_log++;
        }
    }
#endif

#if WC3_DEBUG
    if (target >= GUEST_IMAGE_STORM) watch_dispatch(target, esp, ebp);
#endif
}
#endif

#if WC3_DEBUG
void wc3_host_stack_guard(uint32_t target, uint32_t esp)
{
    (void)target; (void)esp;
}
#endif

#if WC3_DEBUG
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
/* Battle.net login flow probes.  The generated dispatcher calls this hook
 * before every lifted block, so the guest EAX at a target is still the return
 * value of the preceding call.  That is the only host-visible way to read
 * SFileOpenArchive/SFileAuthenticateArchive results: Storm and Game.dll are
 * lifted guest code, not HLE.
 *
 * Keep the switch to exact addresses and exact block entries.  A match prints
 * one line; the caps stop a failed login from filling the log every frame. */
static unsigned g_bnet_probe_n;
static unsigned g_bnet_prepatch_n;
static uint32_t g_bnet_last_conn;
static uint32_t g_bnet_last_state = 0xFFFFFFFFu;

static uint32_t bnet_login_conn(CPU *c)
{
    if (!guest_ok(c->edi + 0x5F8u, 4)) return 0;
    return rd32g(c->edi + 0x5F8u);
}

static uint32_t bnet_login_state(uint32_t conn)
{
    if (!conn || !guest_ok(conn + 0x16Cu, 4)) return 0xFFFFFFFFu;
    return rd32g(conn + 0x16Cu);
}

static void battle_net_dispatch_probe(CPU *c, uint32_t target)
{
    if (g_bnet_probe_n >= 400u) return;

    switch (target) {
    case 0x6F0942D0u:
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr, "[bnet] patch-check entry path=\"%s\"\n",
                    gstr(c->ecx));
        break;
    case 0x6F094308u:
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr, "[bnet] patch-check body path=\"%s\"\n",
                    gstr(c->ebx));
        break;
    case 0x6F094364u:
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr, "[bnet] patch-check archive-open path=\"%s\"\n",
                    gstr(c->ebx));
        break;
    case 0x6F07C6A2u:
        if (rd32g(c->esp) != 0x6F094375u) break;
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr,
                    "[bnet] call SFileOpenArchive path=\"%s\" ra=%08X\n",
                    gstr(rd32g(c->esp + 4u)), rd32g(c->esp));
        break;
    case 0x6F8B5DAEu:
        if (rd32g(c->esp) != 0x6F09438Fu) break;
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr,
                    "[bnet] call SFileAuthenticateArchive archive=%08X "
                    "status_out=%08X ra=%08X\n",
                    rd32g(c->esp + 4u), rd32g(c->esp + 8u),
                    rd32g(c->esp));
        break;
    case 0x6F07C6BAu: {
        if (rd32g(c->esp) != 0x6F0943BBu) break;
        if (g_bnet_prepatch_n++ >= 12u) break;
        uint32_t status = guest_ok(c->ebp - 0x72Cu, 4)
                        ? rd32g(c->ebp - 0x72Cu) : 0xFFFFFFFFu;
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr,
                    "[bnet] call SFileOpenFileEx archive=%08X name=\"%s\" "
                    "auth_status=%08X ra=%08X\n",
                    rd32g(c->esp + 4u), gstr(rd32g(c->esp + 8u)),
                    status, rd32g(c->esp));
        break;
    }
    case 0x6F07C684u:
        if ((rd32g(c->esp) & 0xFFFFF000u) != 0x6F094000u) break;
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr, "[bnet] call SFileCloseArchive archive=%08X ra=%08X\n",
                    rd32g(c->esp + 4u), rd32g(c->esp));
        break;
    case 0x6F094739u:
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr,
                    "[bnet] patch-check AUTH-REJECT status=%08X path=\"%s\"\n",
                    c->eax, gstr(c->ebx));
        break;
    case 0x6F094744u:
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr,
                    "[bnet] patch-check ARCHIVE-OPEN-FAILED eax=%08X path=\"%s\"\n",
                    c->eax, gstr(c->ebx));
        break;
    case 0x6F094641u: {
        if (g_bnet_prepatch_n++ >= 12u) break;
        uint32_t archive = guest_ok(c->ebp - 0x710u, 4)
                         ? rd32g(c->ebp - 0x710u) : 0;
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr,
                    "[bnet] patch-check NO-PREPATCH archive=%08X path=\"%s\"\n",
                    archive, gstr(c->ebx));
        break;
    }
    case 0x6F09475Au:
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr, "[bnet] patch-check BNUPDATE-FAILED path=\"%s\"\n",
                    gstr(c->ebx));
        break;
    case 0x6F2EBAB0u: {
        uint32_t conn = c->ecx;
        uint32_t state = bnet_login_state(conn);
        if (conn != g_bnet_last_conn || state != g_bnet_last_state) {
            g_bnet_last_conn = conn;
            g_bnet_last_state = state;
            if (++g_bnet_probe_n <= 400u)
                fprintf(stderr, "[bnet] state tick conn=%08X state=%u\n",
                        conn, state);
        }
        break;
    }
    case 0x6F2EBB4Au: {
        uint32_t conn = bnet_login_conn(c);
        uint32_t cached = guest_ok(0x6FCD52F0u, 4)
                        ? rd32g(GVA(0x6FCD52F0u)) : 0xFFFFFFFFu;
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr,
                    "[bnet] state 2 patch-check conn=%08X state=%u cached=%08X\n",
                    conn, bnet_login_state(conn), cached);
        break;
    }
    case 0x6F8BE5A0u:
        if (rd32g(c->esp) != 0x6F2EBBC2u) break;
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr, "[bnet] patch-check query path=\"%s\" key=%08X\n",
                    gstr(c->edx), c->ecx);
        break;
    case 0x6F2EBB6Fu:
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr,
                    "[bnet] patch-check dispatch eax=%08X (cached result/flag)\n",
                    c->eax);
        break;
    case 0x6F2EBBF3u:
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr, "[bnet] patch-check mapped result 0x26 (success)\n");
        break;
    case 0x6F2EBBE7u:
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr,
                    "[bnet] patch-check mapped result 0x27 "
                    "(ERROR_ID_UPGRADEFAILED)\n");
        break;
    case 0x6F2EBBFDu:
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr,
                    "[bnet] patch-check mapped result 5 "
                    "(ERROR_ID_NOTINITIALIZED)\n");
        break;
    case 0x6F2EBC14u:
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr, "[bnet] state 1 cleanup\n");
        break;
    case 0x6F2EBC26u: {
        uint32_t conn = bnet_login_conn(c);
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr,
                    "[bnet] cleanup conn=%08X state=%u eax=%08X ebx=%08X\n",
                    conn, bnet_login_state(conn), c->eax, c->ebx);
        break;
    }
    case 0x6F2EBC97u:
        if (++g_bnet_probe_n <= 400u)
            fprintf(stderr, "[bnet] return success eax=%08X\n", c->eax);
        break;
    default:
        break;
    }
}
#endif

#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
void wc3_battle_net_dispatch(CPU *c, uint32_t target)
{
    battle_net_dispatch_probe(c, target);
}
#endif

void wc3_dispatch_watch(CPU *c, uint32_t target)
{
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
    battle_net_dispatch_probe(c, target);
#endif
#if WC3_DEBUG && WC3_DEBUG_HLE
    if (g_self_thread) {
        g_self_thread->last_target = target;
        g_self_thread->last_eip = c->eip;
        g_self_thread->last_esp = c->esp;
        g_self_thread->last_ebp = c->ebp;
        g_self_thread->dispatch_count++;
    } else {
        g_main_last_target = target;
        g_main_last_eip = c->eip;
        g_main_last_esp = c->esp;
        g_main_last_ebp = c->ebp;
        g_main_dispatch_count++;
    }
#endif
#if WC3_DEBUG
    if (g_mss_ret_watch && target == g_mss_ret_watch) {
        fprintf(stderr, "[mss-api] %s returned eax=%08X\n",
                g_mss_ret_name ? g_mss_ret_name : "?", c->eax);
        g_mss_ret_watch = 0;
        g_mss_ret_name = NULL;
    }

#if WC3_DEBUG
    if (target == 0x6F0AD5A0u || target == 0x6F0AFEF0u) {
        uint32_t obj = c->ecx;
        uint32_t pathp = guest_ok(obj + 0x34, 4) ? rd32g(obj + 0x34) : 0;
        uint32_t h = guest_ok(obj + 0x164, 4) ? rd32g(obj + 0x164) : 0;
        fprintf(stderr, "[mss-obj] %s obj=%08X handle=%08X path=\"%s\"\n",
                target == 0x6F0AD5A0u ? "open" : "play", obj, h,
                pathp ? gstr(pathp) : "?");
    }
    if (target == 0x3510A9F0u) {
        uint32_t obj = c->esi;
        uint32_t pathp = guest_ok(obj + 0x34, 4) ? rd32g(obj + 0x34) : 0;
        uint32_t h = guest_ok(obj + 0x164, 4) ? rd32g(obj + 0x164) : 0;
        fprintf(stderr, "[mss-obj] open_stream-entry obj=%08X oldhandle=%08X path=\"%s\"\n",
                obj, h, pathp ? gstr(pathp) : "?");
    }
    if (target == 0x3510B340u || target == 0x3510AD60u) {
        uint32_t obj = c->esi;
        uint32_t h = guest_ok(obj + 0x164, 4) ? rd32g(obj + 0x164) : 0;
        fprintf(stderr, "[mss-obj] %s-entry obj=%08X handle=%08X eax=%08X\n",
                target == 0x3510B340u ? "loop" : "pause", obj, h, c->eax);
        if (target == 0x3510B340u) {
            uint32_t list = guest_ok(0x3515483Cu, 4) ? rd32g(0x3515483Cu) : 0;
            unsigned n;
            for (n = 0; n < 8 && list; n++) {
                uint32_t next = guest_ok(list + 0x104u, 4) ? rd32g(list + 0x104u) : 0;
                uint32_t state = guest_ok(list + 0x108u, 4) ? rd32g(list + 0x108u) : 0;
                uint32_t vol = guest_ok(list + 0x64u, 4) ? rd32g(list + 0x64u) : 0;
                uint32_t inner = guest_ok(list + 0xCu, 4) ? rd32g(list + 0xCu) : 0;
                fprintf(stderr, "[mss-list] node=%08X inner=%08X state=%u vol=%d next=%08X\n",
                        list, inner, state, (int32_t)vol, next);
                list = next;
            }
        }
    }
    if (target == 0x6F0AC120u) {
        uint32_t obj = c->esi;
        int32_t in = (int32_t)c->ecx;
        uint32_t flag = guest_ok(0x6FD463FCu, 4) ? rd32g(GVA(0x6FD463FCu)) : 0xFFFFFFFFu;
        uint32_t idx = (uint32_t)in > 127u ? 127u : (uint32_t)in;
        uint32_t tbl = guest_ok(0x6FD461E8u + idx * 4u, 4)
            ? rd32g(GVA(0x6FD461E8u) + idx * 4u) : 0;
        int32_t f15c = guest_ok(obj + 0x15Cu, 4) ? (int32_t)rd32g(obj + 0x15Cu) : 0;
        int32_t f178 = guest_ok(obj + 0x178u, 4) ? (int32_t)rd32g(obj + 0x178u) : 0;
        int32_t f174 = guest_ok(obj + 0x174u, 4) ? (int32_t)rd32g(obj + 0x174u) : 0;
        uint32_t h164 = guest_ok(obj + 0x164u, 4) ? rd32g(obj + 0x164u) : 0;
        uint32_t h168 = guest_ok(obj + 0x168u, 4) ? rd32g(obj + 0x168u) : 0;
        uint32_t maxb = guest_ok(obj + 0x1BCu, 4) ? rd32g(obj + 0x1BCu) : 0;
        float maxd, spos[3], lpos[3];
        memcpy(&maxd, &maxb, 4);
        for (unsigned i = 0; i < 3; i++) {
            uint32_t s = guest_ok(obj + 0x1C4u + i * 4u, 4)
                ? rd32g(obj + 0x1C4u + i * 4u) : 0;
            uint32_t l = guest_ok(0x6FD3D310u + i * 4u, 4)
                ? rd32g(GVA(0x6FD3D310u) + i * 4u) : 0;
            memcpy(&spos[i], &s, 4);
            memcpy(&lpos[i], &l, 4);
        }
        fprintf(stderr, "[mss-3d] in=%d flag=%u idx=%u tbl=%d esi=%08X f15C=%d f178=%d f174=%d h164=%08X h168=%08X max=%g spos=(%g,%g,%g) lpos=(%g,%g,%g)\n",
                in, flag, idx, (int32_t)tbl, obj, f15c, f178, f174, h164, h168,
                maxd, spos[0], spos[1], spos[2], lpos[0], lpos[1], lpos[2]);
        static int table_dumped;
        if (!table_dumped) {
            static const unsigned picks[] = {0,1,2,4,8,16,24,32,48,64,80,96,112,120,126,127};
            table_dumped = 1;
            fprintf(stderr, "[mss-3d] table");
            for (unsigned k = 0; k < sizeof picks / sizeof picks[0]; k++) {
                uint32_t a = 0x6FD461E8u + picks[k] * 4u;
                fprintf(stderr, " %u=%d", picks[k],
                        guest_ok(a, 4) ? (int32_t)rd32g(GVA(a)) : 0);
            }
            fprintf(stderr, "\n");
        }
    }
    if (target == 0x3510AE30u) {
        uint32_t flag = guest_ok(0x6FD463FCu, 4) ? rd32g(GVA(0x6FD463FCu)) : 0xFFFFFFFFu;
        int32_t vol = (int32_t)(guest_ok(c->esp + 8, 4) ? rd32g(c->esp + 8) : 0);
        int32_t idx = vol < 0 ? 0 : (vol > 127 ? 127 : vol);
        uint32_t curve = guest_ok(0x6FD461E8u + (uint32_t)idx * 4, 4)
            ? rd32g(GVA(0x6FD461E8u) + (uint32_t)idx * 4) : 0;
        fprintf(stderr, "[mss-vol] raw=%d bypass=%u idx=%d curve=%d delta=%08X\n",
                vol, flag, idx, (int32_t)curve, g_image_delta);
    }
    if (target == 0x6F0AD5CDu) {
        uint32_t h = guest_ok(c->esi + 0x164u, 4) ? rd32g(c->esi + 0x164u) : 0;
        fprintf(stderr, "[mss-game] open_stream ret eax=%08X obj=%08X handle=%08X\n",
                c->eax, c->esi, h);
    }
    if (target == 0x6F0AFEF8u) {
        fprintf(stderr, "[mss-game] play open ret eax=%08X obj=%08X handle=%08X\n",
                c->eax, c->esi,
                guest_ok(c->esi + 0x164u, 4) ? rd32g(c->esi + 0x164u) : 0);
    }
    if (target == 0x6F0AFEFCu)
        fprintf(stderr, "[mss-game] play early-return 5 obj=%08X\n", c->esi);
    if (target == 0x6F0AFF03u)
        fprintf(stderr, "[mss-game] play continue obj=%08X\n", c->esi);
    if (target == 0x3511D154u) {
        uint32_t fsz = guest_ok(c->esi + 0xB4u, 4) ? rd32g(c->esi + 0xB4u) : 0;
        uint32_t pay = guest_ok(c->esi + 0x94u, 4) ? rd32g(c->esi + 0x94u) : 0;
        fprintf(stderr, "[mss-str] provider read path file_size=%u payload=%u\n", fsz, pay);
    }
    if (target == 0x3511D08Du)
        fprintf(stderr, "[mss-str] id3 fallback path\n");
    if (target == 0x3511D194u)
        fprintf(stderr, "[mss-str] set_sample_address ret eax=%08X\n", c->eax);
    if (target == 0x3511D1A0u) {
        uint32_t smp = guest_ok(c->esi + 0x0Cu, 4) ? rd32g(c->esi + 0x0Cu) : 0;
        uint32_t st = guest_ok(smp + 0x1A8u, 4) ? rd32g(smp + 0x1A8u) : 0;
        fprintf(stderr, "[mss-str] set_sample_processor ret eax=%08X sample=%08X state=%08X\n",
                c->eax, smp, st);
    }
    if (target == 0x3511D1BEu)
        fprintf(stderr, "[mss-str] query +0x40 ret eax=%08X\n", c->eax);
    if (target == 0x3511D2ADu)
        fprintf(stderr, "[mss-str] processor-empty fallback marker=%u\n",
                (unsigned)(guest_ok(c->ebp - 0x10u, 4) ? rd32g(c->ebp - 0x10u) : 0));
    if (target == 0x3511D2B8u)
        fprintf(stderr, "[mss-str] stream_init FAIL\n");
#endif

    /* 0x6F046CF0 re-enters the #289 thunk once per archive, and between two
     * entries it only runs `test eax,eax` and a compare - neither touches eax.
     * So at the second entry, eax still holds the previous #289 result.  This
     * is the only place the value is reachable: the lifted callee returns into
     * the caller's C frame, not through anything the dispatcher sees.
     *
     * WC3_TREE=1 arms this; without it g_dispatch_diag stays 0 and the
     * dispatcher never calls here. */
    if (target == 0x6F07C792u)
        fprintf(stderr, "[disp] check-loop entry: previous #289 eax=%08X\n", c->eax);

    /* WC3_FN entry probes.  Placed first so a probe fires even for a target
     * that the tree window or the object watch would otherwise swallow. */
    for (unsigned i = 0; i < g_fn_probe_n; i++) {
        if (target != g_fn_probe[i]) continue;
        if (g_fn_hits[i]++ < 48) {
            uint32_t sp = c->esp;
            int ok = guest_ok(sp, 16);
            fprintf(stderr, "[fn] %08X eax=%08X ecx=%08X edx=%08X esp=%08X "
                            "stk=%08X %08X %08X %08X\n",
                    target, c->eax, c->ecx, c->edx, sp,
                    ok ? rd32g(sp) : 0, ok ? rd32g(sp + 4) : 0,
                    ok ? rd32g(sp + 8) : 0, ok ? rd32g(sp + 12) : 0);
        }
        break;
    }

    if (g_tex_trace) {
        /* The decoder returns into L_6F8E9660 at 6F8E96A5, which then calls
         * 0x6F092F70 at 6F8E96A7 with nothing in between: at *that* dispatch
         * eax is still the decoder's result, and it is also what getMip will
         * hand back.  Waiting for the exact target matters - 6F8E8C10 is a
         * lifted function that dispatches internally, so "the next dispatch"
         * would be one of its own callees, not the return site. */
        if (g_tex_mip && target == 0x6F092F70u) {
            uint32_t buf = c->eax;
            unsigned mw = g_tex_mip_w >> (g_tex_mip_idx & 31u);
            unsigned mh = g_tex_mip_h >> (g_tex_mip_idx & 31u);
            if (!mw) mw = 1;
            if (!mh) mh = 1;
            uint32_t bytes = mw * mh * 4u;
            if (g_tex_alpha_reports < 64 && g_tex_mip_w >= 64 && g_tex_mip_h >= 64 &&
                buf && guest_ok(buf, bytes)) {
                unsigned a0 = 0, a255 = 0, nz = 0, amin = 255, amax = 0;
                unsigned long long sum = 0;
                unsigned n = mw * mh;
                for (unsigned i = 0; i < n; i++) {
                    unsigned a = rd8g(buf + i * 4u + 3u);
                    if (a == 0) a0++;
                    if (a == 255) a255++;
                    if (a) nz++;
                    if (a < amin) amin = a;
                    if (a > amax) amax = a;
                    sum += a;
                }
                fprintf(stderr,
                        "[texa] this=%08X mip=%u %ux%u comp=%u buf=%08X "
                        "a0=%u a255=%u nz=%u amin=%u amax=%u mean=%u px0=%08X\n",
                        g_tex_mip_this, g_tex_mip_idx, mw, mh, g_tex_mip_comp,
                        buf, a0, a255, nz, amin, amax,
                        (unsigned)(sum / (n ? n : 1)), rd32g(buf));
                g_tex_alpha_reports++;
            }
            fprintf(stderr, "[tex]       decode -> eax=%08X (getMip returns this)\n",
                    c->eax);
            g_tex_mip = 0;
        }
        /* The resolver's arguments are all strings or structs we can name. */
        if (target == 0x6F70DD90u) {
            fprintf(stderr, "[tex] resolve \"%s\"\n", gstr(c->ecx));
        } else if (target == 0x6F70D9B0u) {
            fprintf(stderr, "[tex]   try \"%s\" out=%08X/%08X\n", gstr(c->ecx),
                    rd32g(c->esp + 4), rd32g(c->esp + 8));
        } else if (target == 0x6F0B3CB0u) {
            fprintf(stderr, "[tex]   open \"%s\"\n", gstr(rd32g(c->esp + 4)));
        } else if (target == 0x6F0B3B80u) {
            fprintf(stderr, "[tex]     tga-open -> eax=%08X\n", c->eax);
        } else if (target == 0x6F8E9790u) {
            fprintf(stderr, "[tex]   blp-open \"%s\"\n", gstr(rd32g(c->esp + 4)));
        } else if (target == 0x6F8E9660u) {
            /* getMip(this=ecx, arg1=[esp+4], mip=[esp+8], &a, &b).  It fails
             * for exactly two reasons - `[this+0x20] == 0 && mip != 0`, or
             * `mip >= [this+0x4b8]` - and otherwise returns whatever the
             * decoder returned.  Printing the object fields separates the
             * three: a mip count of 0 means the header copy or the mip-count
             * helper is wrong, a mip count of 4 with a zero decoder result
             * means the pixel path is. */
            uint32_t th = c->ecx, ok = guest_ok(th, 0x4C0) ? 1u : 0u;
            fprintf(stderr, "[tex]     getMip this=%08X arg1=%u mip=%u "
                            "psub=%u mips=%u base=%08X w=%u h=%u comp=%u\n",
                    th, rd32g(c->esp + 4), rd32g(c->esp + 8),
                    ok ? rd32g(th + 0x20) : 0xDEADu, ok ? rd32g(th + 0x4B8) : 0xDEADu,
                    ok ? rd32g(th + 0x4A4) : 0xDEADu, ok ? rd32g(th + 0x14) : 0xDEADu,
                    ok ? rd32g(th + 0x18) : 0xDEADu, ok ? rd32g(th + 0xC) : 0xDEADu);
            g_tex_mip_this = th;
            g_tex_mip_w = ok ? rd32g(th + 0x14) : 0;
            g_tex_mip_h = ok ? rd32g(th + 0x18) : 0;
            g_tex_mip_comp = ok ? rd32g(th + 0xC) : 0;
            g_tex_mip_idx = rd32g(c->esp + 8);
            g_tex_mip = 1;
        } else if (target == 0x6F8E8C10u) {
            if (g_tex_mip) {
                /* Six arguments, pushed at 6F8E968C..6F8E969B:
                 *   a6 [ebp+0x14]          a5 [this+0x4A4] + [this+mip*4+0x24]
                 *   a4 [ecx+mip*4+0x64]    a3 [ebp+0x10]
                 *   a2 edx (the mip)       a1 2 (a format selector, cmp'd with 2)
                 * so with the return address at esp, a1 is esp+4 and the fields
                 * are one slot *later* than a naive reading suggests.  The old
                 * labels were shifted by one, which made an 8x8 mip print
                 * "size=813690660" - that value is a3, and the real size is a4.
                 * Verified against the file: TeamColor00.blp mip 0 is 47 bytes
                 * at offset 0x49C, and this prints size=47 with
                 * src = 11285DF8 + 0x49C.  a3's meaning is not established, so
                 * it is printed under its slot number rather than a guess. */
                fprintf(stderr, "[tex]       decode mip=%u size=%u src=%08X a3=%08X\n",
                        rd32g(c->esp + 8), rd32g(c->esp + 16),
                        rd32g(c->esp + 20), rd32g(c->esp + 12));
                g_tex_mip = 2;
            }
        } else if (target == 0x6F133E40u) {
            /* CArray::SetSize(this=ecx, n=[esp+4]): capacity, count, data.
             * A zero size here returns without allocating, which is exactly
             * how the out-struct ends up with a NULL pixel pointer. */
            uint32_t n = guest_ok(c->esp, 8) ? rd32g(c->esp + 4) : 0;
            uint32_t cap = guest_ok(c->ecx, 12) ? rd32g(c->ecx) : 0;
            uint32_t cnt = guest_ok(c->ecx, 12) ? rd32g(c->ecx + 4) : 0;
            uint32_t dat = guest_ok(c->ecx, 12) ? rd32g(c->ecx + 8) : 0;
            fprintf(stderr, "[tex]     set-size this=%08X n=%u cap=%u cnt=%u data=%08X\n",
                    c->ecx, n, cap, cnt, dat);
        }
    }
    if (g_obj_watch && target == 0x6F06C390u) {
        uint32_t obj = c->ecx;
        uint32_t v0 = (obj && guest_ok(obj, 4)) ? rd32g(obj) : 0;
        if (!v0) {
            if (g_obj_watch_zero++ < 24)
                fprintf(stderr, "[obj] AddRef this=%08X vtbl=00000000 ra=%08X "
                                "stk=%08X %08X %08X\n",
                        obj, rd32g(c->esp), rd32g(c->esp + 4),
                        rd32g(c->esp + 8), rd32g(c->esp + 12));
        } else if (g_obj_watch_ok++ < 6) {
            fprintf(stderr, "[obj] AddRef this=%08X vtbl=%08X [4]=%08X ra=%08X\n",
                    obj, v0, guest_ok(obj, 8) ? rd32g(obj + 4) : 0, rd32g(c->esp));
        }
    }
    if (g_rw_on && target != 0x15022BA0u && c->esp > g_rw_esp) {
        g_rw_on = 0;
        fprintf(stderr, "[disp]   open -> eax=%08X (next call %08X)\n",
                c->eax, target);
    }
    /* 0x15020790 is the MPQ hash probe: ecx = SFile, edx = the name it will
     * hash, and it returns the block index or -1.  Its inputs are the whole
     * question - a name that has been prefixed with the archive's own path, or
     * an SFile whose hash table was never loaded, both produce a silent miss. */
    if (target == 0x15022BA0u) {
        g_rw_on = 1;
        g_rw_esp = c->esp;
    }
    /* 0x1502B6D0 is SErrSetLastError(err, file, line): the only place Storm
     * says *why* an open failed.  arg1 is the Win32 error, arg2 the source
     * file it was raised from, arg3 the line.  Without this the failure is
     * invisible - SFileOpenFileEx just returns 0. */
    else if (target == 0x1502B6D0u) {
        if (g_err_n++ < 40)
            fprintf(stderr, "[disp] Storm error %u at %s:%u (ra=%08X)\n",
                    rd32g(c->esp + 4), gstr(rd32g(c->esp + 8)),
                    rd32g(c->esp + 12), rd32g(c->esp));
    }
    else if (target == 0x15020790u) {
        static unsigned probes;
        uint32_t ht = guest_ok(c->ecx, 0x148) ? rd32g(c->ecx + 0x138) : 0;
        if (probes++ < 40)
            fprintf(stderr, "[disp] hash-probe arc=%08X name=\"%s\" namepath=\"%s\" "
                            "hash=%08X block=%08X count=%u\n",
                    c->ecx, gstr(c->edx), gstr(c->ecx + 0x154), ht,
                    rd32g(c->ecx + 0x140),
                    (ht && guest_ok(ht, 0x1C)) ? rd32g(ht + 0x18) : 0);
    }
    /* 0x1503F040 is the memcpy wrapper the MPQ sector path goes through
     * (SFileReadFile -> 0x15023449 -> 0x1503F040 -> memcpy).  With the event
     * stubs in place it was called with n == 0 for every file, which is what
     * "the read returned nothing" looks like from the outside.  Now that it
     * carries bytes, the interesting question is *which* bytes: SFile::LoadFile
     * returns `*(uint32_t *)buf != 0`, so a zero first dword makes it report
     * failure even though the read succeeded.  Dump small payloads. */
    if (target == 0x1503F040u) {
        static unsigned mc;
        uint32_t n = rd32g(c->esp + 12), ra = rd32g(c->esp);
        if (ra == 0x15023449u && n && n <= 512 && mc++ < 8) {
            uint32_t dst = rd32g(c->esp + 4), src = rd32g(c->esp + 8);
            fprintf(stderr, "[disp] sector memcpy(dst=%08X src=%08X n=%u)\n", dst, src, n);
            if (guest_ok(src, n)) {
                fprintf(stderr, "[disp]   src =");
                for (uint32_t i = 0; i < n; i++) fprintf(stderr, " %02X", rd8g(src + i));
                fprintf(stderr, "\n");
            }
        }
    }
    /* 0x6F330A80 is the second gate in L_6F330C40: it is only reached when
     * Storm's LoadFile reported success, and it fails when strlen(name) does
     * not fit inside the loaded data.  Seeing it (or not) separates "the read
     * failed" from "the read worked but the content gate rejected it". */
    if (target == 0x6F330A80u) {
        fprintf(stderr, "[disp] font gate2(buf=%08X size=%08X name=\"%s\") ra=%08X\n",
                c->ecx, c->edx, gstr(rd32g(c->esp + 4)), rd32g(c->esp));
    }
    /* The fatal dialog's body is line #N of ui\startupstrings.txt:
     * L_6F0448A0 opens the file (0x6F07C74A), reads N lines with the "\n\r"
     * tokenizer (0x6F07C73E), and falls back to the built-in
     * "Warcraft III was unable to initialize." when the buffer is empty.
     * On the device the caption is right and the body is empty, so print both
     * ends of the handoff: what the file reader produced, and what
     * L_6F08EC60 (the ANSI/UTF-16 pair rebuilder) was handed. */
    if (target == 0x6F0448A0u)
        fprintf(stderr, "[disp] startup-strings line #%u\n", c->ecx);
    if (target == 0x6F07C74Au)
        fprintf(stderr, "[disp]   open \"%s\"\n", gstr(rd32g(c->esp + 4)));
    if (target == 0x6F07C73Eu) {
        static unsigned lc;
        if (lc++ < 10)
            fprintf(stderr, "[disp]   readline cur=%08X buf=%08X n=%u\n",
                    rd32g(c->esp + 4), rd32g(c->esp + 8), rd32g(c->esp + 12));
    }
    if (target == 0x6F08EC60u) {
        static unsigned sc;
        if (sc++ < 12) {
            fprintf(stderr, "[disp] setmsg src=%08X ansi=\"%s\"\n", c->ecx, gstr(c->ecx));
            if (guest_ok(c->ecx, 32)) {
                fprintf(stderr, "[disp]   raw =");
                for (unsigned i = 0; i < 32; i++) fprintf(stderr, " %02X", rd8g(c->ecx + i));
                fprintf(stderr, "\n");
            }
        }
    }
    if (target == 0x6F08B3B0u) {
        fprintf(stderr, "[disp] fatal dialog hwnd=%08X text=%08X caption=%08X\n",
                c->ecx, rd32g(c->esp + 4), rd32g(c->esp + 8));
        fprintf(stderr, "[disp]   text=\"%s\" caption=\"%s\"\n",
                gstr(rd32g(c->esp + 4)), gstr(rd32g(c->esp + 8)));
    }
#endif
}
#endif

#if WC3_DEBUG
void wc3_stack_balance(uint32_t target, uint32_t a, uint32_t b)
{
    (void)target; (void)a; (void)b;
}
#else
/* The generated dispatcher is built with its debug hooks even when the HLE
 * side is a graphics-only debug build.  These are the same no-op fallbacks the
 * Windows host supplies; without them dlopen cannot resolve the shared object
 * and the whole APK dies before the first frame. */
void wc3_trace_dispatch(uint32_t target, uint32_t esp, uint32_t ebp)
{
    (void)target; (void)esp; (void)ebp;
}
void wc3_stack_balance(uint32_t target, uint32_t a, uint32_t b)
{
    (void)target; (void)a; (void)b;
}
void wc3_host_stack_guard(uint32_t target, uint32_t esp)
{
    (void)target; (void)esp;
}
void wc3_dispatch_watch(CPU *c, uint32_t target)
{
    (void)c; (void)target;
}
#endif

void wc3_lift_todo(unsigned va, const char *text, unsigned eip, unsigned esp,
                   unsigned ebp, unsigned eax)
{
    fprintf(stderr, "[lift] UNIMPLEMENTED %08X %s eip=%08X esp=%08X ebp=%08X eax=%08X\n",
            va, text ? text : "?", eip, esp, ebp, eax);
    hle_android_report();
    _exit(6);
}
