/* lift.c - compiled replacement for the hot Python steps of the lift loop.
 *
 * The recompiler build reads ~350 MB of generated C on every iteration: the
 * address-table merge, the dispatcher generation, the closure scan and the
 * chunk splitter.  All four are pure text processing, so Python pays for the
 * interpreter on every byte.  This tool does the same work in one pass with
 * no dependencies, and is what the build script calls when it is present.
 *
 *   lift known   <out.h> <dir>...        union of every lifted address found
 *                                        under the directories (X(ADDR) in
 *                                        *.h, "void L_ADDR(CPU" in *.c)
 *   lift merge   <out.h> <file>...       union of X(ADDR) in the given files
 *   lift dispatch <in.h> <out.c>         write the address -> function table
 *   lift targets <file|dir>...           unique constant dispatch targets
 *   lift split   <src.c> <dir> <prefix> [per] [max-mb]
 *                                        cut a lift into translation units
 *
 * Every command prints a one-line summary on stdout and exits non-zero on a
 * real failure, so it can be dropped into a pipeline unchanged.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- growable address vector ------------------------------------------- */

typedef struct {
    uint32_t *v;
    size_t    n, cap;
} Vec;

static void vec_push(Vec *a, uint32_t x) {
    if (a->n == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 1024;
        uint32_t *p = (uint32_t *)realloc(a->v, cap * sizeof *p);
        if (!p) { fprintf(stderr, "lift: out of memory\n"); exit(2); }
        a->v = p;
        a->cap = cap;
    }
    a->v[a->n++] = x;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* Sort in place and drop duplicates; returns the new length. */
static size_t vec_unique(Vec *a) {
    if (a->n == 0) return 0;
    qsort(a->v, a->n, sizeof *a->v, cmp_u32);
    size_t w = 1;
    for (size_t r = 1; r < a->n; r++)
        if (a->v[r] != a->v[w - 1]) a->v[w++] = a->v[r];
    a->n = w;
    return w;
}

/* ---- text helpers ------------------------------------------------------- */

static int is_hex(int c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

static uint32_t hex8(const char *p) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        int c = p[i];
        int d = (c <= '9') ? c - '0' : ((c | 0x20) - 'a' + 10);
        v = (v << 4) | (uint32_t)d;
    }
    return v;
}

static int all_hex8(const char *p) {
    for (int i = 0; i < 8; i++)
        if (!is_hex(p[i])) return 0;
    return 1;
}

/* Read a whole file.  Returns NULL on failure; *len receives the size. */
static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    *len = got;
    return buf;
}

/* ---- pattern scans ------------------------------------------------------ */

/* "void L_XXXXXXXX(CPU" - the definition line of a lifted function. */
static void scan_defs(const char *b, size_t n, Vec *out) {
    static const char pat[] = "void L_";
    const size_t pl = sizeof pat - 1;
    size_t i = 0;
    while (i + pl + 8 + 4 <= n) {
        const char *p = (const char *)memchr(b + i, 'v', n - i);
        if (!p) return;
        size_t off = (size_t)(p - b);
        if (off + pl + 8 + 4 <= n && memcmp(p, pat, pl) == 0 &&
            all_hex8(p + pl) && memcmp(p + pl + 8, "(CPU", 4) == 0) {
            vec_push(out, hex8(p + pl));
            i = off + pl + 8;
        } else {
            i = off + 1;
        }
    }
}

/* "X(XXXXXXXX)" - the X-macro listing used by the dispatch table. */
static void scan_list(const char *b, size_t n, Vec *out) {
    size_t i = 0;
    while (i + 2 + 8 + 1 <= n) {
        const char *p = (const char *)memchr(b + i, 'X', n - i);
        if (!p) return;
        size_t off = (size_t)(p - b);
        if (off + 2 + 8 + 1 <= n && p[1] == '(' &&
            all_hex8(p + 2) && p[10] == ')') {
            vec_push(out, hex8(p + 2));
            i = off + 11;
        } else {
            i = off + 1;
        }
    }
}

/* "dispatch(c, 0xXXXXXXXXu)" and "dispatch_jmp(c, 0xXXXXXXXXu)". */
static void scan_targets(const char *b, size_t n, Vec *out) {
    static const char call[] = "(c, 0x";
    static const char jump[] = "_jmp(c, 0x";
    size_t i = 0;
    while (i + 8 + 6 + 8 + 2 <= n) {
        const char *p = (const char *)memchr(b + i, 'd', n - i);
        if (!p) return;
        size_t off = (size_t)(p - b);
        if (off + 8 + 6 + 8 + 2 <= n && memcmp(p, "dispatch", 8) == 0) {
            const char *h = NULL;
            if (memcmp(p + 8, call, sizeof call - 1) == 0)
                h = p + 8 + (sizeof call - 1);
            else if (memcmp(p + 8, jump, sizeof jump - 1) == 0)
                h = p + 8 + (sizeof jump - 1);
            if (h && all_hex8(h) && h[8] == 'u' && h[9] == ')') {
                vec_push(out, hex8(h));
                i = off + 8;
                continue;
            }
        }
        i = off + 1;
    }
}

/* ---- directory walk ----------------------------------------------------- */

static int has_ext(const char *name, const char *ext) {
    size_t ln = strlen(name), le = strlen(ext);
    if (ln < le) return 0;
    for (size_t i = 0; i < le; i++) {
        int a = name[ln - le + i], c = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (a != c) return 0;
    }
    return 1;
}

static void walk(const char *dir, const char *ext, Vec *out, int mode, int *files);

/* mode: 0 = definitions in .c, 1 = X() entries in .h, 2 = dispatch targets */
static void scan_file(const char *path, Vec *out, int mode) {
    size_t n = 0;
    char *b = slurp(path, &n);
    if (!b) { fprintf(stderr, "lift: cannot read %s\n", path); return; }
    if (mode == 0)      scan_defs(b, n, out);
    else if (mode == 1) scan_list(b, n, out);
    else                scan_targets(b, n, out);
    free(b);
}

static void walk(const char *dir, const char *ext, Vec *out, int mode, int *files) {
    char spec[MAX_PATH];
    _snprintf(spec, sizeof spec, "%s\\*", dir);
    spec[sizeof spec - 1] = 0;

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(spec, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char path[MAX_PATH * 2];
        _snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        path[sizeof path - 1] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            walk(path, ext, out, mode, files);
        } else if (ext && has_ext(fd.cFileName, ext)) {
            scan_file(path, out, mode);
            if (files) (*files)++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* ---- writers ------------------------------------------------------------ */

static int write_list(const char *dst, Vec *a) {
    FILE *f = fopen(dst, "wb");
    if (!f) { fprintf(stderr, "lift: cannot write %s\n", dst); return 1; }
    fputs("#define LIFTED_FUNCS(X) \\\n", f);
    for (size_t i = 0; i < a->n; i++)
        fprintf(f, "    X(%08X)%s\n", a->v[i], (i + 1 < a->n) ? " \\" : "");
    fclose(f);
    return 0;
}

static int cmd_known(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: lift known <out.h> <dir>...\n"); return 2; }
    Vec a = {0};
    int files = 0;
    for (int i = 2; i < argc; i++) {
        DWORD attr = GetFileAttributesA(argv[i]);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            fprintf(stderr, "lift: missing %s\n", argv[i]);
            continue;
        }
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            walk(argv[i], ".c", &a, 0, &files);
            walk(argv[i], ".h", &a, 1, &files);
        } else {
            scan_file(argv[i], &a, has_ext(argv[i], ".h") ? 1 : 0);
            files++;
        }
    }
    size_t n = vec_unique(&a);
    if (write_list(argv[1], &a)) return 1;
    printf("%s: %zu addresses from %d file(s)\n", argv[1], n, files);
    return 0;
}

static int cmd_merge(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: lift merge <out.h> <file>...\n"); return 2; }
    Vec a = {0};
    int files = 0;
    for (int i = 2; i < argc; i++) {
        size_t n = 0;
        char *b = slurp(argv[i], &n);
        if (!b) { fprintf(stderr, "lift: missing %s\n", argv[i]); continue; }
        scan_list(b, n, &a);
        free(b);
        files++;
    }
    size_t n = vec_unique(&a);
    if (write_list(argv[1], &a)) return 1;
    printf("%s: %zu addresses from %d list(s)\n", argv[1], n, files);
    return 0;
}

static int cmd_targets(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: lift targets <file|dir>...\n"); return 2; }
    Vec a = {0};
    for (int i = 1; i < argc; i++) {
        DWORD attr = GetFileAttributesA(argv[i]);
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        if (attr & FILE_ATTRIBUTE_DIRECTORY) walk(argv[i], ".c", &a, 2, NULL);
        else scan_file(argv[i], &a, 2);
    }
    size_t n = vec_unique(&a);
    for (size_t i = 0; i < n; i++) printf("%08X\n", a.v[i]);
    return 0;
}

/* Byte-for-byte the table gen_dispatch.py used to emit. */
static int cmd_dispatch(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: lift dispatch <in.h> <out.c>\n"); return 2; }
    size_t n = 0;
    char *b = slurp(argv[1], &n);
    if (!b) { fprintf(stderr, "lift: cannot read %s\n", argv[1]); return 1; }
    Vec a = {0};
    scan_list(b, n, &a);
    free(b);
    size_t cnt = vec_unique(&a);
    if (!cnt) { fprintf(stderr, "%s: no X(XXXXXXXX) entries\n", argv[1]); return 1; }

    const char *probe = getenv("WC3_PROBE_FILE");
    if (!probe || !*probe) probe = "build/probe_missing.txt";
    char probe_c[512];
    size_t w = 0;
    for (const char *p = probe; *p && w + 3 < sizeof probe_c; p++) {
        if (*p == '\\' || *p == '"') probe_c[w++] = '\\';
        probe_c[w++] = *p;
    }
    probe_c[w] = 0;

    const char *slash = strrchr(argv[1], '\\');
    if (!slash) slash = strrchr(argv[1], '/');
    const char *base = slash ? slash + 1 : argv[1];

    FILE *f = fopen(argv[2], "wb");
    if (!f) { fprintf(stderr, "lift: cannot write %s\n", argv[2]); return 1; }

    fputs("/* AUTO-GENERATED by tools/gen_dispatch.py - do not edit */\n", f);
    fputs("#include \"host.h\"\n#include <stdio.h>\n#include <stdlib.h>\n"
          "#ifdef _WIN32\n#include <windows.h>\n#endif\n\n", f);
    fputs("#define X(a) void L_##a(CPU *c);\n", f);
    fprintf(f, "#include \"%s\"\n", base);
    fputs("LIFTED_FUNCS(X)\n", f);
    fputs("#undef X\n\n", f);
    fputs("uint32_t g_image_delta = 0;\n\n", f);
    fputs("typedef struct { uint32_t va; void (*fn)(CPU *); } LiftEnt;\n", f);
    fputs("static const LiftEnt g_lift[] = {\n", f);
    for (size_t i = 0; i < cnt; i++)
        fprintf(f, "    { 0x%08Xu, L_%08X },\n", a.v[i], a.v[i]);
    fputs("};\n\n", f);
    /* The table no longer needs to be sorted: the hash is built from every
     * entry, and a sorted table is only useful for a binary search.  The old
     * code also qsort()ed a `static const` array, which is undefined
     * behaviour and happened to work only because the input was sorted. */
    fputs("#define LIFT_HASH_BITS 19\n"
          "#define LIFT_HASH_SIZE (1u << LIFT_HASH_BITS)\n"
          "static uint32_t g_lift_hash[LIFT_HASH_SIZE];\n\n", f);
    fputs("void wc3_dispatch_init(void) {\n"
          "    size_t count = sizeof g_lift / sizeof g_lift[0];\n"
          "    for (size_t i = 0; i < count; i++) {\n"
          "        uint32_t h = (g_lift[i].va * 2654435761u) >> (32 - LIFT_HASH_BITS);\n"
          "        while (g_lift_hash[h]) h = (h + 1) & (LIFT_HASH_SIZE - 1);\n"
          "        g_lift_hash[h] = (uint32_t)(i + 1);\n"
          "    }\n"
          "}\n", f);
    fputs("void wc3_dispatch_shutdown(void) { }\n\n", f);
    fputs("static inline void (*find_lifted(uint32_t va))(CPU *) {\n"
          "    uint32_t h = (va * 2654435761u) >> (32 - LIFT_HASH_BITS);\n"
          "    for (unsigned n = 0; n < LIFT_HASH_SIZE; n++) {\n"
          "        uint32_t e = g_lift_hash[h];\n"
          "        if (!e) return NULL;\n"
          "        if (g_lift[e - 1].va == va) return g_lift[e - 1].fn;\n"
          "        h = (h + 1) & (LIFT_HASH_SIZE - 1);\n"
          "    }\n"
          "    return NULL;\n"
          "}\n\n", f);
    /* WC3_REG_GUARD=1: a `call` must leave ebx/esi/edi/ebp exactly as it found
     * them.  When a lifted extent ends on a `jmp` into un-lifted code, or when
     * a fragment's epilogue lands in a different fragment than its prologue,
     * the callee "returns" through the probe stub without ever running its
     * pops - the caller then continues with a zeroed callee-saved register and
     * faults much later, in a frame that has nothing to do with the bug.
     * 6F1A0010 died that way: `mov edi,ecx` then `call 6F1ACD50`, and the
     * `[edi+0x20]` after the call read address 0x20 because edi had become 0.
     * A tail jump is exempt: it is a continuation, not a call. */
    fputs("/* The register guard, probe-file trace, and per-dispatch diagnostic hooks\n"
          " * are diagnostics.  Keep them on the host build, which does not define\n"
          " * WC3_DEBUG, and in Android debug builds, but drop them from an Android\n"
          " * release build. */\n"
          "#if !defined(__ANDROID__) || defined(WC3_DEBUG)\n"
          "static int g_reg_guard = -1;\n"
          "static uint32_t g_rg_target;\n"
          "#endif\n"
          /* Tail-jump trampoline.  A guest `jmp` that leaves its lifted extent
           * used to be a host call (dispatch_jmp -> dispatch_common -> fn), so
           * a loop that jumps between two extents added a host frame on every
           * iteration while guest ESP never moved: the credits loader walked
           * a byte string for 6.8 M iterations and consumed the whole 64 MB
           * main-thread stack (C00000FD with 18 KB of guest stack used).
           * Instead, dispatch_jmp records the target and returns to the
           * dispatch loop that is already on the stack; that loop runs the
           * next extent with the same frame.  A real `call` still nests, so
           * guest calls keep their host-stack call chain. */
          "static THREAD_LOCAL int g_disp_depth;\n"
          "static THREAD_LOCAL int g_tail_pending;\n"
          "static THREAD_LOCAL uint32_t g_tail_target;\n"
          "#if !defined(__ANDROID__) || defined(WC3_DEBUG)\n"
          "static void rg_report(CPU *c, uint32_t target, uint32_t ra,\n"
          "                      uint32_t bx, uint32_t si, uint32_t di, uint32_t bp) {\n"
          "    fprintf(stderr, \"[regguard] target=%08X ra=%08X ebx %08X->%08X esi %08X->%08X edi %08X->%08X ebp %08X->%08X\\n\",\n"
          "            target, ra, bx, c->ebx, si, c->esi, di, c->edi, bp, c->ebp);\n"
          "}\n"
          "#endif\n\n"
          "static void dispatch_common(CPU *c, uint32_t target, int tail) {\n"
          "#if !defined(__ANDROID__) || defined(WC3_DEBUG)\n"
          "    wc3_trace_dispatch(target, c->esp, c->ebp);\n"
          "    wc3_host_stack_guard(target, c->esp);\n"
          "    if (g_dispatch_diag) wc3_dispatch_watch(c, target);\n"
          "    uint32_t esp_in = c->esp;\n"
          "    uint32_t target0 = target;\n"
          /* The saved values MUST be locals: a nested dispatch inside the
           * callee would overwrite anything kept in a global, and then the
           * outer check compares against the inner call's registers and
           * reports every call in the program. */
          "    if (g_reg_guard < 0) {\n"
          "        const char *s = getenv(\"WC3_REG_GUARD\");\n"
          "        g_reg_guard = s ? 1 : 0;\n"
          "        s = getenv(\"WC3_REG_GUARD_TARGET\");\n"
          "        if (s && *s) g_rg_target = (uint32_t)strtoul(s, NULL, 16);\n"
          "    }\n"
          "    int rg = g_reg_guard && !tail &&\n"
          "             (!g_rg_target || target == g_rg_target);\n"
          "    uint32_t rg_ra = 0, rg_bx = 0, rg_si = 0, rg_di = 0, rg_bp = 0;\n"
          "    if (rg) { rg_ra = rd32(c->esp);\n"
          "              rg_bx = c->ebx; rg_si = c->esi; rg_di = c->edi; rg_bp = c->ebp; }\n"
          "#endif\n"
          "    int chained = 0;\n"
          "    g_disp_depth++;\n"
          "    for (;;) {\n"
          "        g_tail_pending = 0;\n"
          "        if (target >= GUEST_HLE_BASE && target < GUEST_HLE_BASE + 0x00100000u) {\n"
          "            hle_call(target, c);\n"
          "            break;\n"
          "        }\n"
          "        void (*fn)(CPU *) = find_lifted(target);\n"
          "        if (fn) {\n"
          "            fn(c);\n"
          "            if (!g_tail_pending) break;\n"
          "            chained = 1;\n"
          "            target = g_tail_target;\n"
          "#if !defined(__ANDROID__) || defined(WC3_DEBUG)\n"
          "            wc3_trace_dispatch(target, c->esp, c->ebp);\n"
          "            wc3_host_stack_guard(target, c->esp);\n"
          "            if (g_dispatch_diag) wc3_dispatch_watch(c, target);\n"
          "#endif\n"
          "            continue;\n"
          "        }\n"
          "#if !defined(__ANDROID__) || defined(WC3_DEBUG)\n"
          "        if (getenv(\"WC3_PROBE\")) {\n", f);
    fprintf(f, "            FILE *f = fopen(\"%s\", \"a\");\n", probe_c);
    fputs("            if (f) { fprintf(f, \"%08X\\n\", target); fclose(f); }\n"
          /* Keep the "unknown guest target" prefix: triage.c parses it.  The
           * [jmp] marker separates a tail jump into un-lifted code from a
           * plain call, which is the difference between a stack-balanced
           * no-op and a corrupted guest frame. */
          "            fprintf(stderr, \"[probe] unknown guest target %08X (returns 0)%s\\n\",\n"
          "                    target, (tail || chained) ? \" [jmp]\" : \"\");\n"
          "            c->eax = 0;\n"
          /* Both forms emulate "the missing code did nothing and returned", so
           * both consume the return address dispatch() pushed.  Leaving it for
           * a tail jump used to corrupt ESP: ijl15's 600351E0 ends its lifted
           * extent at 60035327 and jumps to 60035329, so the function returned
           * to its caller 0x40 too low and the next [esp+0x38] load picked up a
           * zero that became a near-NULL read at +0x37C0. */
          "            c->esp += 4;\n"
          "            break;\n"
          "        }\n"
          "#endif\n"
          "        {\n"
          "            uint32_t *sp = (uint32_t *)(uintptr_t)c->esp;\n"
          "            fprintf(stderr, \"[dispatch] %s unknown guest target %08X (eip=%08X esp=%08X ra=%08X %s)\\n\",\n"
          "                    (tail || chained) ? \"jmp\" : \"call\", target, c->eip, c->esp,\n"
          "                    (unsigned)(uintptr_t)sp ? sp[0] : 0, hle_describe(c->eip));\n"
          "            fprintf(stderr, \"[dispatch]   stk: %08X %08X %08X %08X %08X %08X\\n\",\n"
          "                    sp[0], sp[1], sp[2], sp[3], sp[4], sp[5]);\n"
          "#ifdef _WIN32\n"
          "            fprintf(stderr, \"[dispatch]   host_ra=%p rva=%llX\\n\",\n"
          "                    __builtin_return_address(0),\n"
          "                    (unsigned long long)((uintptr_t)__builtin_return_address(0) - (uintptr_t)GetModuleHandleA(NULL)));\n"
          "#else\n"
          "            fprintf(stderr, \"[dispatch]   host_ra=%p\\n\",\n"
          "                    __builtin_return_address(0));\n"
          "#endif\n"
          "        }\n"
          "        abort();\n"
          "    }\n"
          "    g_disp_depth--;\n"
          "#if !defined(__ANDROID__) || defined(WC3_DEBUG)\n"
          "    if (rg && (c->ebx != rg_bx || c->esi != rg_si ||\n"
          "               c->edi != rg_di || c->ebp != rg_bp))\n"
          "        rg_report(c, target0, rg_ra, rg_bx, rg_si, rg_di, rg_bp);\n"
          "#endif\n"
          /* A tail chain's final `ret` belongs to the last extent, so the
           * original target's ESP signature no longer applies. */
          "#if !defined(__ANDROID__) || defined(WC3_DEBUG)\n"
          "    if (g_stack_guard && !chained) wc3_stack_balance(target0, esp_in, c->esp);\n"
          "#endif\n"
          "}\n\n"
          "void dispatch(CPU *c, uint32_t target) { dispatch_common(c, target, 0); }\n"
          "void dispatch_jmp(CPU *c, uint32_t target) {\n"
          "    if (g_disp_depth > 0) { g_tail_pending = 1; g_tail_target = target; return; }\n"
          "    dispatch_common(c, target, 1);\n"
          "}\n", f);
    fclose(f);
    printf("%s: %zu functions -> %s\n", argv[1], cnt, argv[2]);
    return 0;
}

static int cmd_split(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: lift split <src.c> <out-dir> <prefix> [per] [max-mb]\n");
        return 2;
    }
    const char *src = argv[1], *outdir = argv[2], *prefix = argv[3];
    int per = (argc > 4) ? atoi(argv[4]) : 500;
    if (per < 1) per = 500;
    /* Function count is not a size bound: game_27_extra2.c has functions
     * averaging 6 000 generated lines, and 500 of them became a 273 MB unit
     * that gcc cannot optimize at -O2.  Also start a new file when the next
     * whole function would cross max_mb (0 disables the byte cap). */
    size_t max_bytes = (argc > 5) ? (size_t)atoi(argv[5]) * 1024u * 1024u : 0;

    FILE *in = fopen(src, "rb");
    if (!in) { fprintf(stderr, "lift: cannot read %s\n", src); return 1; }
    CreateDirectoryA(outdir, NULL);

    const char *slash = strrchr(src, '\\');
    if (!slash) slash = strrchr(src, '/');
    const char *base = slash ? slash + 1 : src;

    char line[1 << 16];
    FILE *out = NULL;
    int chunk = 0, count = 0, total = 0;
    size_t written = 0;

    while (fgets(line, sizeof line, in)) {
        int is_func = !strncmp(line, "void L_", 7);
        if (is_func && (!out || count >= per ||
                        (max_bytes && count > 0 &&
                         written + strlen(line) > max_bytes))) {
            if (out) { fclose(out); chunk++; }
            char path[MAX_PATH * 2];
            _snprintf(path, sizeof path, "%s\\%s_%04d.c", outdir, prefix, chunk);
            path[sizeof path - 1] = 0;
            out = fopen(path, "wb");
            if (!out) { fprintf(stderr, "lift: cannot write %s\n", path); fclose(in); return 1; }
            int n = fprintf(out, "/* AUTO-GENERATED split of %s - do not edit */\n", base);
            fputs("#include \"cpu.h\"\n\n", out);
            written = (size_t)n + strlen("#include \"cpu.h\"\n\n");
            count = 0;
        }
        if (is_func) {
            count++;
            total++;
        }
        if (out) {
            fputs(line, out);
            written += strlen(line);
        }
    }
    if (out) { fclose(out); chunk++; }
    fclose(in);
    printf("%s: %d functions -> %d file(s) of <=%d, <=%d MB in %s\n",
           src, total, chunk, per, (int)(max_bytes / (1024u * 1024u)), outdir);
    return 0;
}

static void usage(void) {
    fputs("usage: lift <command> ...\n"
          "  known   <out.h> <dir>...             union of lifted addresses in a tree\n"
          "  merge   <out.h> <file>...            union of X(ADDR) list entries\n"
          "  dispatch <in.h> <out.c>              write the address -> function table\n"
          "  targets <file|dir>...                unique constant dispatch targets\n"
          "  split   <src.c> <dir> <prefix> [per] [max-mb] split a lift into chunks\n", stderr);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    const char *c = argv[1];
    /* Shift so every command sees argv[0] == its own name and argv[1] == its
     * first real argument; the bodies then read like standalone tools. */
    int ac = argc - 1;
    char **av = argv + 1;
    if (!strcmp(c, "known"))    return cmd_known(ac, av);
    if (!strcmp(c, "merge"))    return cmd_merge(ac, av);
    if (!strcmp(c, "dispatch")) return cmd_dispatch(ac, av);
    if (!strcmp(c, "targets"))  return cmd_targets(ac, av);
    if (!strcmp(c, "split"))    return cmd_split(ac, av);
    usage();
    return 2;
}
