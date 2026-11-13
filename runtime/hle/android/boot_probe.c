/*
 * boot_probe.c - run the lifted war3.exe entry on Android with a real HLE.
 *
 * This is the first Android target that executes the launcher's own code, not
 * a selected function: it maps the licensed war3.exe at its preferred base,
 * maps the guest stack and TEB, binds every import through the POSIX HLE, and
 * dispatches to the PE entry point.  The milestone is the launcher's own
 * LoadLibraryA("game.dll") call, which is where the next module (game.dll)
 * would start; hle_android reports and exits there.
 *
 * Storm.dll is mapped and its DllMain is called first, exactly as the Windows
 * loader would: war3.exe's static initialisers use Storm's allocator and
 * hashing, and a null Storm makes them take a fallback path that runs into
 * code the corpus does not cover.  Its exports are bound into war3's IAT
 * directly, so a Storm call is lifted code on both sides.
 *
 * ijl15.dll (the Intel JPEG Library) is mapped and entered the same way when
 * the game itself is being loaded: it is an implicit import of game.dll and
 * every BLP texture decode goes through it.  See load_ijl() for why an HLE
 * stub is not an option.
 *
 * Usage: wc3_boot_probe <war3.exe> [workdir] [storm.dll]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include "host.h"
#include "hle_android.h"
#include "wc3_trace.h"

void dispatch(CPU *c, uint32_t target);
void wc3_dispatch_init(void);
void wc3_dispatch_shutdown(void);
void hle_install_crash_handler(void);

typedef struct {
    pe_image *storm;
} bind_ctx;

/* Modules this probe mapped for real.  The pe_image has to outlive the call,
 * because GetProcAddress resolves against it later. */
/* war3 + Storm + ijl15 + Mss32 + game + ver-IX86-3 + the optional Miles
 * plug-ins.  Six was enough before the downloaded version DLL became a real
 * guest module; the table must be able to hold every mapped image at once. */
#define PROBE_MOD_MAX 12
#define PROBE_MOD_PATH_MAX 1024
static pe_image g_probe_mods[PROBE_MOD_MAX];
static char     g_probe_mod_paths[PROBE_MOD_MAX][PROBE_MOD_PATH_MAX];
static int      g_probe_mod_count;

/* pe_load() stores the caller's path pointer in pe_image.path.  The callers
 * below use short-lived stack buffers, and -O2 may reuse the same slot for
 * consecutive loads, so keep an owned copy for every module in the table. */
static void probe_own_path(pe_image *m, int index, const char *path)
{
    if (index < 0 || index >= PROBE_MOD_MAX)
        return;
    snprintf(g_probe_mod_paths[index], sizeof g_probe_mod_paths[index],
             "%s", path ? path : "");
    m->path = g_probe_mod_paths[index];
}

/* These diagnostics only observe the module table; they must not change a
 * decision.  They are part of the boot probe, so the boot tag controls them
 * even when the rest of the HLE is built release-like. */
#if WC3_DEBUG && WC3_DEBUG_BOOT
static int g_rdbg_mss_lookups;
static int g_rdbg_ijl_lookups;

static void rdbg_dump_mods(const char *name, int *calls)
{
    if (*calls >= 3)
        return;
    (*calls)++;
    fprintf(stderr, "[rdbg] lookup %s: %d module(s)\n", name, g_probe_mod_count);
    for (int i = 0; i < g_probe_mod_count; i++) {
        const char *p = g_probe_mods[i].path;
        fprintf(stderr, "[rdbg]   [%d] base=%08X path=%p '%s'\n",
                i, g_probe_mods[i].base, (const void *)p,
                p ? p : "(null)");
    }
}
#endif

static pe_image *probe_mod_by_name(const char *name)
{
#if WC3_DEBUG && WC3_DEBUG_BOOT
    int rdbg_track = name && (!strcasecmp(name, "Mss32.dll") ||
                              !strcasecmp(name, "ijl15.dll"));
    if (rdbg_track)
        rdbg_dump_mods(name, !strcasecmp(name, "Mss32.dll")
                       ? &g_rdbg_mss_lookups : &g_rdbg_ijl_lookups);
#endif
    for (int i = 0; i < g_probe_mod_count; i++) {
        const char *p = g_probe_mods[i].path;
        if (!p) continue;
        const char *leaf = strrchr(p, '/');
        leaf = leaf ? leaf + 1 : p;
        if (!strcasecmp(leaf, name)) {
#if WC3_DEBUG && WC3_DEBUG_BOOT
            if (rdbg_track)
                fprintf(stderr, "[rdbg] lookup %s -> idx=%d base=%08X "
                        "path=%p '%s'\n",
                        name, i, g_probe_mods[i].base,
                        (const void *)p, p);
#endif
            return &g_probe_mods[i];
        }
    }
    return NULL;
}

/* war3.exe imports Storm by ordinal (1.27 exports only the mangled SFile::
 * names), so the ordinal path is the one that matters here. */
static uint32_t resolve_import(const char *dll, const char *name,
                               uint16_t ordinal, void *user)
{
    bind_ctx *ctx = (bind_ctx *)user;

    /* A module the probe already mapped binds to its lifted code, not to a
     * thunk: game.dll's own imports of Storm must run the real Storm. */
    pe_image *m = dll ? probe_mod_by_name(dll) : NULL;
    if (m) {
        uint32_t va = name ? pe_export(m, name) : pe_export_ordinal(m, ordinal);
        if (va) {
#if WC3_DEBUG && WC3_DEBUG_BOOT
            if (name && !strcasecmp(dll, "Mss32.dll") &&
                strstr(name, "AIL_startup"))
                fprintf(stderr, "[rdbg] export Mss32!%s -> %08X via base=%08X "
                        "path=%p '%s'\n", name, va, m->base,
                        (const void *)m->path, m->path ? m->path : "(null)");
#endif
            /* The boot harness has to observe the first Miles call: the
             * renderer is not up when the imports are bound, and Mss32's
             * DllMain has to run before AIL_startup.  Return a small HLE
             * trampoline that performs the deferred DllMain and then enters
             * the real lifted export. */
            if (name && !strcasecmp(dll, "Mss32.dll") &&
                strstr(name, "AIL_startup")) {
#if WC3_DEBUG && WC3_DEBUG_BOOT
                fprintf(stderr, "[mss] hooking AIL_startup real=%08X\n", va);
#endif
                return hle_mss_hook_ail_startup(va);
            }
            if (name && !strcasecmp(dll, "Mss32.dll")) {
                uint32_t trace = hle_mss_hook_trace(name, va);
                if (trace) return trace;
            }
            return va;
        }
#if WC3_DEBUG && WC3_DEBUG_BOOT
        if (name && !strcasecmp(dll, "Mss32.dll") &&
            strstr(name, "AIL_startup"))
            fprintf(stderr, "[rdbg] export Mss32!%s -> MISS via base=%08X "
                    "path=%p '%s'\n", name, m->base,
                    (const void *)m->path, m->path ? m->path : "(null)");
#endif
        fprintf(stderr, "[host] %s export not found: %s#%u\n", dll,
                name ? name : "(ordinal)", (unsigned)ordinal);
    }
    if (ctx && ctx->storm && dll && !strcasecmp(dll, "Storm.dll")) {
        uint32_t va = name ? pe_export(ctx->storm, name)
                           : pe_export_ordinal(ctx->storm, ordinal);
        if (va) return va;
        fprintf(stderr, "[host] Storm export not found: %s#%u\n",
                name ? name : "(ordinal)", (unsigned)ordinal);
    }
    return hle_thunk(dll, name, ordinal);
}

static void teb_init(CPU *c, uint32_t teb, uint32_t stack_base,
                     uint32_t stack_size, uint32_t tid)
{
    memset((void *)(uintptr_t)teb, 0, 0x1000);
    *(uint32_t *)(uintptr_t)(teb + 0x00) = 0xFFFFFFFFu;      /* SEH chain end */
    *(uint32_t *)(uintptr_t)(teb + 0x04) = stack_base + stack_size;
    *(uint32_t *)(uintptr_t)(teb + 0x08) = stack_base;
    *(uint32_t *)(uintptr_t)(teb + 0x18) = teb;
    *(uint32_t *)(uintptr_t)(teb + 0x20) = 1234;
    *(uint32_t *)(uintptr_t)(teb + 0x24) = tid;
    *(uint32_t *)(uintptr_t)(teb + 0x34) = 0;
    c->fs_base = teb;
    c->fs = 0x53;
    c->ds = c->es = c->ss = 0x2B;
    c->cs = 0x23;
}

/* Run a DLL's entry point with DLL_PROCESS_ATTACH.  The implicit Storm load
 * passes a non-NULL lpReserved: Storm 1.27 seeds its MPQ crypt table
 * differently when it sees an explicit load (docs/WAR127.md).  Explicit
 * LoadLibrary loads, including game.dll and the Miles modules, pass NULL,
 * matching the Windows host's DllMain calls. */
static void call_dll_entry(CPU *c, const pe_image *img, uint32_t sentinel,
                           uint32_t reserved)
{
    uint32_t saved = c->esp;
    push32(c, reserved);             /* lpReserved */
    push32(c, 1);                    /* DLL_PROCESS_ATTACH */
    push32(c, img->base);            /* hInstance */
    push32(c, sentinel);             /* return sentinel */
    dispatch(c, img->entry);
    c->esp = saved;
}

/* Mss32 is mapped before the game, but its DllMain is deliberately deferred
 * until the D3D9 device exists.  Running it during boot made DXVK abort with
 * a destroyed pthread mutex inside CreateDevice; the game does not call Miles
 * until after the renderer is initialised. */
static pe_image *g_mss_image;
static int g_mss_entered;

static void wc3_mss_after_d3d9(void *cpu_ptr)
{
    CPU *cpu = (CPU *)cpu_ptr;
    if (!g_mss_image || g_mss_entered)
        return;
    g_mss_entered = 1;
#if WC3_DEBUG && WC3_DEBUG_BOOT
    printf("android boot probe: calling deferred Mss32 entry %08X\n",
           g_mss_image->entry);
    fflush(stdout);
#endif
    CPU saved = *cpu;
    WC3_TRACE_BEGIN("wc3.dll.mss32");
    call_dll_entry(cpu, g_mss_image, 0x7EFFFFFDu, 0);
    WC3_TRACE_END();
#if WC3_DEBUG && WC3_DEBUG_BOOT
    printf("android boot probe: deferred Mss32 entry returned eax=%08X\n",
           cpu->eax);
    fflush(stdout);
#endif
    /* The saved copy restores the caller's guest register file.  Win32
     * last-error/TLS and the qsort scratch are host-side per-thread state,
     * not guest registers: side effects made by DllMain must survive the
     * restore.  The x87 stack is ordinary CPU state and is restored with the
     * saved copy. */
    uint32_t hle_last_error = cpu->hle_last_error;
    uint32_t hle_tmp_guest = cpu->hle_tmp_guest;
    uint32_t hle_tls[64];
    memcpy(hle_tls, cpu->hle_tls, sizeof hle_tls);
    *cpu = saved;
    cpu->hle_last_error = hle_last_error;
    cpu->hle_tmp_guest = hle_tmp_guest;
    memcpy(cpu->hle_tls, hle_tls, sizeof hle_tls);
}

/* Windows paths are case-insensitive, Android's are not: the launcher asks for
 * "Game.dll" and the file pushed to the device is "game.dll".  Resolve the leaf
 * name against the work directory before giving up. */
static int resolve_case(const char *name, char *out, size_t cap)
{
    if (access(name, R_OK) == 0) { snprintf(out, cap, "%s", name); return 0; }

    const char *slash = strrchr(name, '/');
    const char *leaf = slash ? slash + 1 : name;
    DIR *d = opendir(".");
    if (!d) return -1;
    struct dirent *e;
    int rc = -1;
    while ((e = readdir(d)) != NULL) {
        if (strcasecmp(e->d_name, leaf)) continue;
        snprintf(out, cap, "%s", e->d_name);
        rc = 0;
        break;
    }
    closedir(d);
    return rc;
}

static const char *probe_leaf(const char *name)
{
    const char *leaf = name;
    for (const char *p = name; p && *p; p++)
        if (*p == '\\' || *p == '/') leaf = p + 1;
    return leaf;
}

/* Convert a Windows path to the work directory's form.  Absolute paths keep
 * their root; Z: is the fake drive the Android HLE uses for "/". */
static void probe_posix_path(const char *name, char *out, size_t cap)
{
    const char *p = name ? name : "";
    size_t o = 0;
    if (p[0] && p[1] == ':') p += 2;
    int absolute = (*p == '\\' || *p == '/');
    if (absolute && o + 1 < cap) out[o++] = '/';
    while (*p == '\\' || *p == '/') p++;
    for (; *p && o + 1 < cap; p++)
        out[o++] = (*p == '\\') ? '/' : *p;
    out[o] = 0;
}

/* Case-insensitive lookup of a nested path.  The game asks for "Game.dll" and
 * Miles may ask for "redist\miles\Mssfast.m3d"; Android is case-sensitive and
 * treats backslashes as ordinary characters, so the exact spelling cannot be
 * assumed. */
static int resolve_path_case(const char *name, char *out, size_t cap)
{
    char norm[512];
    probe_posix_path(name, norm, sizeof norm);
    if (access(norm, R_OK) == 0) { snprintf(out, cap, "%s", norm); return 0; }

    const char *slash = strrchr(norm, '/');
    if (slash) {
        char dir[512];
        size_t dlen = (size_t)(slash - norm);
        if (dlen == 0) dlen = 1;
        if (dlen >= sizeof dir) return -1;
        memcpy(dir, norm, dlen);
        dir[dlen] = 0;
        DIR *d = opendir(dir);
        if (d) {
            const char *leaf = slash + 1;
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (strcasecmp(e->d_name, leaf)) continue;
                snprintf(out, cap, "%s/%s", dir, e->d_name);
                closedir(d);
                return 0;
            }
            closedir(d);
        }
    }
    return resolve_case(name, out, cap);
}

static int probe_resolve_dll(const char *name, char *out, size_t cap)
{
    const char *leaf = probe_leaf(name);
    if (strcasecmp(leaf, "Mssfast.m3d") && strcasecmp(leaf, "Mp3dec.asi"))
        return resolve_path_case(name, out, cap);

    /* Miles calls AIL_set_redist_directory("redist\\miles") and then loads
     * Mssfast.m3d and Mp3dec.asi by bare name.  The Windows host prepends that
     * directory; Android has no global DLL search path, so do the same here. */
    if (resolve_path_case(name, out, cap) == 0) return 0;
    static const char *dirs[] = { "redist/miles", "redist", "." };
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        char cand[512];
        snprintf(cand, sizeof cand, "%s/%s", dirs[i], leaf);
        if (resolve_path_case(cand, out, cap) == 0) return 0;
    }
    return -1;
}

/* The HLE calls this from inside a guest dispatch and passes the CPU of the
 * thread that called LoadLibraryA.  Win32 runs DllMain on that thread, with
 * that thread's stack; the hook must do the same.  (A process-wide "main" CPU
 * here used to let a Battle.net worker thread run DllMain on the main thread's
 * guest CPU and stack while the renderer was still using them.) */
static int probe_load_dll(CPU *c, const char *name, uint32_t *out_base,
                          const void **out_img)
{
    /* A module may be requested by a full Windows path after it was already
     * loaded (Mss32 is mapped before the game starts).  Re-mapping it at the
     * same preferred address would overwrite the live image and duplicate the
     * module table entry, so return the existing one. */
    pe_image *loaded = probe_mod_by_name(probe_leaf(name));
    if (loaded) {
        *out_base = loaded->base;
        *out_img = loaded;
        return 0;
    }

    if (g_probe_mod_count >= PROBE_MOD_MAX) {
        fprintf(stderr, "[host] module table full, cannot load %s\n", name);
        return -1;
    }
    char path[512];
    if (probe_resolve_dll(name, path, sizeof path) != 0) {
        fprintf(stderr, "[host] %s: no such file in the work directory\n", name);
        return -1;
    }
    const char *leaf = probe_leaf(name);
    uint32_t preferred = GUEST_IMAGE_GAME;
    int relocate = 0;
    if (!strcasecmp(leaf, "Mss32.dll")) {
        preferred = GUEST_IMAGE_MSS;
        relocate = 1;
    } else if (!strcasecmp(leaf, "Mssfast.m3d")) {
        preferred = GUEST_IMAGE_MSSFAST;
        relocate = 1;
    } else if (!strcasecmp(leaf, "Mp3dec.asi")) {
        preferred = GUEST_IMAGE_MP3DEC;
        relocate = 1;
    } else if (!strcasecmp(leaf, "ver-IX86-3.dll")) {
        /* The file is downloaded at runtime, so its on-disk ImageBase is the
         * PE default 0x10000000, while the corpus was lifted from a copy
         * rebased to 0x35400000.  Map and relocate it to that base, otherwise
         * every lifted CheckRevision address would miss the image. */
        preferred = GUEST_IMAGE_VER;
        relocate = 1;
    }
    pe_image *m = &g_probe_mods[g_probe_mod_count];
    memset(m, 0, sizeof *m);
    if (pe_load(path, preferred, m, relocate) != 0) {
        fprintf(stderr, "[host] cannot map %s\n", path);
        return -1;
    }
    probe_own_path(m, g_probe_mod_count, path);
#if WC3_DEBUG && WC3_DEBUG_BOOT
    fprintf(stderr, "[rdbg] dynamic %s loaded: path_buffer=%p '%s' "
            "image=%p base=%08X path_ptr=%p '%s'\n",
            name, (const void *)path, path, (const void *)m, m->base,
            (const void *)m->path, m->path ? m->path : "(null)");
#endif
    int missing = pe_bind_imports(m, resolve_import, NULL);
    /* pe_load/pe_bind_imports already log the module; only the entry call and
     * the unresolved count are worth a second line. */
#if WC3_DEBUG && WC3_DEBUG_BOOT
    printf("[pe] %s: %d unresolved\n", path, missing);
    fflush(stdout);
#else
    (void)missing;
#endif

    /* Publish before the entry call: game.dll's initialisers take the module
     * handle of themselves and resolve their own exports. */
    g_probe_mod_count++;
    hle_register_module(probe_leaf(name), m->base, m);
    *out_base = m->base;
    *out_img = m;

    if (c) {
#if WC3_DEBUG && WC3_DEBUG_BOOT
        printf("android boot probe: calling %s DllMain %08X\n", name, m->entry);
        fflush(stdout);
#endif
        call_dll_entry(c, m, 0x7EFFFFFDu, 0);
#if WC3_DEBUG && WC3_DEBUG_BOOT
        printf("android boot probe: %s DllMain returned eax=%08X\n", name, c->eax);
        fflush(stdout);
#endif
    }
    return 0;
}

static uint32_t probe_export(const void *img, const char *name, uint16_t ordinal)
{
    const pe_image *m = (const pe_image *)img;
    if (!m) return 0;
    if (name && name[0]) return pe_export(m, name);
    return pe_export_ordinal(m, ordinal);
}

/* The probe's entry point.  It is not main() so that the same code can be
 * called from the APK's activity thread (runtime/android/apk/native_activity.c)
 * with a constructed argv; the shell build still gets main() below.  One
 * implementation, two front ends - the alternative was a second copy of the
 * boot sequence that would drift. */
int wc3_boot_probe_main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <war3.exe> [workdir] [storm.dll]\n", argv[0]);
        return 2;
    }
    const char *exe = argv[1];
    const char *storm_path = argc >= 4 ? argv[3] : "Storm.dll";
    WC3_TRACE_SCOPE("wc3.boot.probe");
    if (argc >= 3 && chdir(argv[2]) != 0) {
        fprintf(stderr, "cannot enter workdir %s\n", argv[2]);
        return 2;
    }

    const char *load_game = getenv("WC3_LOAD_GAME");
    const int want_game = load_game && !strcmp(load_game, "1");

    hle_install_crash_handler();

    if (!guest_map(GUEST_STACK_BASE, GUEST_STACK_SIZE)) {
        fprintf(stderr, "cannot map the guest stack\n");
        return 1;
    }
    if (!guest_map(GUEST_TEB_BASE, 0x1000)) {
        fprintf(stderr, "cannot map the TEB\n");
        return 1;
    }

    pe_image img;
    memset(&img, 0, sizeof img);
    if (pe_load(exe, GUEST_IMAGE_WAR3, &img, 0) != 0) {
        fprintf(stderr, "pe_load failed: %s\n", exe);
        return 1;
    }

    CPU c;
    memset(&c, 0, sizeof c);
    /* `CPU *restrict` in the lifted functions: the guest lives below 4 GiB,
     * so the host CPU object must not be reachable through a guest VA. */
    if ((uintptr_t)&c < 0x100000000ull) abort();
    cpu_x87_init_thread(&c);
    c.esp = GUEST_STACK_BASE + GUEST_STACK_SIZE - 0x200;
    teb_init(&c, GUEST_TEB_BASE, GUEST_STACK_BASE, GUEST_STACK_SIZE, 5678);

    hle_android_start(exe, exe);
    wc3_dispatch_init();

    hle_set_dll_hooks(probe_load_dll, probe_export);
    hle_register_module("war3.exe", img.base, &img);

    /* Storm first: the launcher's CRT initialisers call into it.
     *
     * The Android layout puts Storm at a base other than its PE ImageBase
     * (docs/ANDROID_WSI.md §10), so the loader has to apply base relocations.
     * The corpus linked here was lifted from a copy rebased to the same
     * address, which is what makes the guest VA equal the host one again. */
    pe_image *storm = &g_probe_mods[g_probe_mod_count];
    memset(storm, 0, sizeof *storm);
    int have_storm = (pe_load(storm_path, GUEST_IMAGE_STORM, storm, 1) == 0);
    if (have_storm) {
        probe_own_path(storm, g_probe_mod_count, storm_path);
        g_probe_mod_count++;
        pe_bind_imports(storm, resolve_import, NULL);
        hle_register_module("Storm.dll", storm->base, storm);
#if WC3_DEBUG && WC3_DEBUG_BOOT
        printf("android boot probe: calling Storm entry %08X\n", storm->entry);
        fflush(stdout);
#endif
        WC3_TRACE_BEGIN("wc3.dll.storm");
        call_dll_entry(&c, storm, 0x7EFFFFFEu, 1);
        WC3_TRACE_END();
#if WC3_DEBUG && WC3_DEBUG_BOOT
        printf("android boot probe: Storm entry returned eax=%08X\n", c.eax);
        fflush(stdout);
#endif
    } else {
        fprintf(stderr, "[host] Storm.dll not mapped: the launcher will take "
                        "its fallback path\n");
    }

    bind_ctx ctx = { have_storm ? storm : NULL };

    /* ijl15.dll is an implicitly linked import of game.dll, so the real loader
     * maps it at its preferred base (0x60000000) before any game code runs,
     * and game.dll's IAT entries are resolved from that image.  This is the
     * Windows host's arrangement (runtime/host/main.c) and it is not optional:
     * game.dll imports ijlInit/ijlRead/ijlFree by ordinal, the whole BLP
     * decode path is `ijlInit -> ijlRead -> ijlFree`, and an HLE stub that
     * returns 0 for ijlInit makes every getMip return 0 - i.e. no texture in
     * the game ever decodes, which is what round 6 was chasing.
     *
     * Only mapped when the game is being loaded: without game.dll nothing
     * dispatches into it, and its corpus is 40 MB of objects.
     *
     * The entry point is NOT called here - it needs the boot CPU, and it must
     * run after Storm's DllMain, exactly as the Windows loader orders it. */
    pe_image *ijl = NULL;
    if (want_game) {
        char ijl_path[512];
        if (resolve_case("ijl15.dll", ijl_path, sizeof ijl_path) != 0) {
            fprintf(stderr, "[host] ijl15.dll: no such file in the work "
                            "directory - BLP textures will not decode\n");
        } else if (g_probe_mod_count < PROBE_MOD_MAX) {
            pe_image *m = &g_probe_mods[g_probe_mod_count];
            memset(m, 0, sizeof *m);
            if (pe_load(ijl_path, GUEST_IMAGE_IJL, m, 0) != 0) {
                fprintf(stderr, "[host] cannot map %s\n", ijl_path);
            } else {
                probe_own_path(m, g_probe_mod_count, ijl_path);
                g_probe_mod_count++;
                int imiss = pe_bind_imports(m, resolve_import, NULL);
                hle_register_module("ijl15.dll", m->base, m);
#if WC3_DEBUG && WC3_DEBUG_BOOT
                fprintf(stderr, "[rdbg] ijl15 loaded: path_buffer=%p '%s' "
                        "image=%p base=%08X path_ptr=%p '%s'\n",
                        (const void *)ijl_path, ijl_path, (const void *)m,
                        m->base, (const void *)m->path,
                        m->path ? m->path : "(null)");
#endif
#if WC3_DEBUG && WC3_DEBUG_BOOT
                printf("android boot probe: ijl15 base=%08X entry=%08X "
                       "unresolved=%d\n", m->base, m->entry, imiss);
                fflush(stdout);
#else
                (void)imiss;
#endif
                ijl = m;
            }
        }
    }

    /* Mss32.dll is an implicit import of both war3.exe and game.dll.  It has
     * to be in the module table before either image's IAT is bound, or those
     * imports would bind to HLE stubs instead of the real lifted Miles code.
     * Android puts it at 0x35100000 (ART owns the PE ImageBase range), so the
     * loaded copy is relocated and the linked corpus was lifted at that base. */
    pe_image *mss = NULL;
#if WC3_DEBUG && WC3_DEBUG_BOOT
    if (want_game && !getenv("WC3_SKIP_MSS")) {
#else
    if (want_game) {
#endif
        char mss_path[512];
        if (resolve_case("Mss32.dll", mss_path, sizeof mss_path) != 0) {
            fprintf(stderr, "[host] Mss32.dll: no such file in the work "
                            "directory - sound will not initialise\n");
        } else if (g_probe_mod_count < PROBE_MOD_MAX) {
            pe_image *m = &g_probe_mods[g_probe_mod_count];
            memset(m, 0, sizeof *m);
            if (pe_load(mss_path, GUEST_IMAGE_MSS, m, 1) != 0) {
                fprintf(stderr, "[host] cannot map %s\n", mss_path);
            } else {
                probe_own_path(m, g_probe_mod_count, mss_path);
                g_probe_mod_count++;
                int mmiss = pe_bind_imports(m, resolve_import, NULL);
                hle_register_module("Mss32.dll", m->base, m);
#if WC3_DEBUG && WC3_DEBUG_BOOT
                fprintf(stderr, "[rdbg] Mss32 loaded: path_buffer=%p '%s' "
                        "image=%p base=%08X path_ptr=%p '%s'\n",
                        (const void *)mss_path, mss_path, (const void *)m,
                        m->base, (const void *)m->path,
                        m->path ? m->path : "(null)");
#endif
#if WC3_DEBUG && WC3_DEBUG_BOOT
                printf("android boot probe: Mss32 base=%08X entry=%08X "
                       "unresolved=%d\n", m->base, m->entry, mmiss);
                fflush(stdout);
#else
                (void)mmiss;
#endif
                mss = m;
                g_mss_image = m;
                hle_android_set_after_d3d9(wc3_mss_after_d3d9);
            }
        }
    }

    int missing = pe_bind_imports(&img, resolve_import, &ctx);
#if WC3_DEBUG && WC3_DEBUG_BOOT
    printf("android boot probe: base=%08X entry=%08X stack=%08X fs=%08X "
           "unresolved=%d\n", img.base, img.entry, c.esp, c.fs_base, missing);
    fflush(stdout);
#else
    (void)missing;
#endif

    /* ijl15.dll links a static MSVC CRT, so its entry point is not DllMain but
     * __crtDllMain: it builds _environ, installs the stdio handle table and
     * runs the C initialisers.  The real loader calls it before any export is
     * used, and ijlInit dereferences that state - skipping it leaves the
     * decoder faulting inside the CRT rather than returning an error. */
    if (ijl) {
#if WC3_DEBUG && WC3_DEBUG_BOOT
        printf("android boot probe: calling ijl15 entry %08X\n", ijl->entry);
        fflush(stdout);
#endif
        WC3_TRACE_BEGIN("wc3.dll.ijl15");
        call_dll_entry(&c, ijl, 0x7EFFFFFDu, 1);
        WC3_TRACE_END();
#if WC3_DEBUG && WC3_DEBUG_BOOT
        printf("android boot probe: ijl15 entry returned eax=%08X\n", c.eax);
        fflush(stdout);
#endif
    }

    /* Mss32 is an explicit module load in the Windows host, so its DllMain
     * receives lpReserved = NULL.  It is initialised from the D3D9 callback
     * (or, if Miles starts earlier, from the AIL_startup hook) so the
     * renderer exists before Miles touches the audio stack. */
    if (mss) {
#if WC3_DEBUG && WC3_DEBUG_BOOT
        printf("android boot probe: Mss32 entry %08X deferred until first "
               "audio/renderer use\n", mss->entry);
        fflush(stdout);
#endif
    }

    /* The launcher's CRT entry point is cdecl: [esp] is its return address. */
    if (want_game) {
        hle_stop_on_game_dll(0);
#if WC3_DEBUG && WC3_DEBUG_BOOT
        printf("android boot probe: WC3_LOAD_GAME=1, game.dll will be mapped\n");
#endif
    }
    push32(&c, 0x7EFFFFFFu);
#if WC3_DEBUG && WC3_DEBUG_BOOT
    printf("android boot probe: entering %08X\n", img.entry);
    fflush(stdout);
#endif
    WC3_TRACE_BEGIN("wc3.launcher.entry");
    dispatch(&c, img.entry);
    WC3_TRACE_END();
#if WC3_DEBUG && WC3_DEBUG_BOOT
    printf("android boot probe: entry returned eax=%08X esp=%08X eip=%08X\n",
           c.eax, c.esp, c.eip);
    fflush(stdout);
#endif

    /* The launcher's entry returns as soon as it has started GameMain on a
     * worker thread, so the process would exit while the renderer is still
     * reading its video settings - and a run that stops there says nothing
     * about Direct3D at all.  WC3_WAIT_SEC keeps the harness alive after the
     * entry returns so those threads can reach the device; it is a wait, not a
     * hang: the harness still kills the run on its own timeout. */
#if WC3_DEBUG && WC3_DEBUG_BOOT
    const char *wait_s = getenv("WC3_WAIT_SEC");
    if (wait_s && *wait_s) {
        int secs = atoi(wait_s);
        if (secs > 0) {
            printf("android boot probe: waiting %d s for the worker threads\n", secs);
            fflush(stdout);
            sleep((unsigned)secs);
        }
    }
#endif

    hle_android_report();
    wc3_dispatch_shutdown();

    const int ok = hle_reached_game_dll();
#if WC3_DEBUG && WC3_DEBUG_BOOT
    printf(ok ? "android boot probe: PASS\n" : "android boot probe: entry returned early\n");
#endif
    return ok ? 0 : 1;
}

#ifndef WC3_NO_MAIN
int main(int argc, char **argv)
{
    return wc3_boot_probe_main(argc, argv);
}
#endif
