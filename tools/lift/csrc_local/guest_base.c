/* guest_base.c - cache the write-once guest window base per lifted function.
 *
 * Why
 * ---
 * g_guest_host_base is set once by runtime/platform/android/guest_window_android.c
 * before guest code starts.  It is nevertheless an ordinary mutable global, so
 * the compiler reloads it after every guest store and every call to a function
 * it cannot see through.  A lifted function with a loop and a few memory
 * operations therefore carries a base load on nearly every iteration.
 *
 * This pass inserts one local snapshot at the top of every function that uses
 * a guest-memory helper:
 *
 *     const uintptr_t wc3_ghb = g_guest_host_base;
 *
 * and redirects the helper names in that translation unit to explicit-base
 * forms from guest_base_local.h.  The base is not changed at run time; that is
 * the same precondition as the existing 4 GiB guest window.  The pass is
 * default-off and must not be enabled without a device A/B.
 *
 * Scope
 * -----
 * The pass touches only canonical `void L_<8 hex>(CPU *restrict c)` bodies and
 * only helpers whose address arithmetic is exactly `base + guest_va`:
 *
 *   guest_host_ptr, rd/wr 8/16/32/64, push32/pop32,
 *   rdss/rdsd/rdxm, wrss/wrsd/wrxm, and the fs: wrappers built on rd/wr.
 *
 * It deliberately leaves the atomic helpers and x87 helpers alone.  They are
 * rare enough that changing their signatures would add more risk than code.
 * A function with no such helper is not touched.  A file with no eligible
 * function is copied byte-for-byte.
 *
 * The generated file is marked with WC3_LOCAL_BASE so a second run is a no-op.
 * A source pass is enough here: the guest ABI, guest pointers, and the
 * dispatcher contract are unchanged.
 *
 *   guest_base --inputs FILE --out-dir DIR [--report JSON] [--quiet]
 *   guest_base --self-test
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#define MARKER "WC3_LOCAL_BASE"
#define HEADER_INCLUDE "#include \"guest_base_local.h\""

typedef struct {
    char *p;
    size_t n;
    size_t cap;
} Buf;

typedef struct {
    char **p;
    size_t n;
    size_t cap;
} PathVec;

typedef struct {
    size_t hdr;
    size_t open;
    size_t close;
    int uses_mem;
} Func;

typedef struct {
    Func *p;
    size_t n;
    size_t cap;
} FuncVec;

typedef struct {
    size_t files;
    size_t files_rewritten;
    size_t files_skipped;
    size_t functions;
    size_t functions_rewritten;
    size_t memory_uses;
} Stats;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("guest_base: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1u);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1u);
    if (!q) die("out of memory");
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1u;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static void buf_reserve(Buf *b, size_t extra)
{
    size_t need = b->n + extra + 1u;
    size_t cap;
    if (need <= b->cap) return;
    cap = b->cap ? b->cap : 4096u;
    while (cap < need) {
        if (cap > (SIZE_MAX >> 1)) die("buffer overflow");
        cap <<= 1;
    }
    b->p = (char *)xrealloc(b->p, cap);
    b->cap = cap;
}

static void buf_add(Buf *b, const char *p, size_t n)
{
    if (!n) return;
    buf_reserve(b, n);
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void buf_str(Buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

static void func_push(FuncVec *v, size_t hdr, size_t open, size_t close,
                      int uses_mem)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2u : 1024u;
        v->p = (Func *)xrealloc(v->p, v->cap * sizeof *v->p);
    }
    v->p[v->n].hdr = hdr;
    v->p[v->n].open = open;
    v->p[v->n].close = close;
    v->p[v->n].uses_mem = uses_mem;
    v->n++;
}

static void path_push(PathVec *v, char *x)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2u : 256u;
        v->p = (char **)xrealloc(v->p, v->cap * sizeof *v->p);
    }
    v->p[v->n++] = x;
}

static char *read_file(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *p;
    size_t n;
    if (!f) die("cannot open %s", path);
    if (fseek(f, 0, SEEK_END) != 0) die("cannot seek %s", path);
    sz = ftell(f);
    if (sz < 0) die("cannot size %s", path);
    if (fseek(f, 0, SEEK_SET) != 0) die("cannot rewind %s", path);
    n = (size_t)sz;
    p = (char *)xmalloc(n + 1u);
    if (n && fread(p, 1, n, f) != n) die("cannot read %s", path);
    fclose(f);
    p[n] = 0;
    *out_n = n;
    return p;
}

static void write_file(const char *path, const char *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot create %s", path);
    if (n && fwrite(p, 1, n, f) != n) die("cannot write %s", path);
    if (fclose(f) != 0) die("cannot close %s", path);
}

static const char *path_base(const char *path)
{
    const char *a = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    if (!a) return b ? b + 1 : path;
    if (!b) return a + 1;
    return a > b ? a + 1 : b + 1;
}

static int word_char(int c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/* Comments and string/char literals are blanked, byte for byte.  Positions in
 * the mask therefore address the same byte in the original text.  Newlines are
 * preserved so find_func still sees the original line structure. */
static char mask_blank(char c)
{
    return (c == '\n' || c == '\r') ? c : ' ';
}

static void mask_code(const char *s, size_t n, char *m)
{
    size_t i = 0;
    memcpy(m, s, n);
    m[n] = 0;
    while (i < n) {
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            m[i] = m[i + 1] = ' ';
            i += 2;
            while (i < n) {
                if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') {
                    m[i] = m[i + 1] = ' ';
                    i += 2;
                    break;
                }
                m[i] = mask_blank(s[i]);
                i++;
            }
        } else if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') {
                m[i] = mask_blank(s[i]);
                i++;
            }
        } else if (s[i] == '"' || s[i] == '\'') {
            char quote = s[i];
            m[i] = ' ';
            i++;
            while (i < n) {
                if (s[i] == '\\' && i + 1 < n) {
                    m[i] = ' ';
                    m[i + 1] = mask_blank(s[i + 1]);
                    i += 2;
                } else {
                    int done = s[i] == quote;
                    m[i] = mask_blank(s[i]);
                    i++;
                    if (done) break;
                }
            }
        } else {
            i++;
        }
    }
}

static size_t matching_brace(const char *s, size_t n, size_t open)
{
    size_t i = open;
    size_t depth = 0;
    while (i < n) {
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            if (i + 1 < n) i += 2;
            continue;
        }
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
            i += 2;
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (s[i] == '"' || s[i] == '\'') {
            char quote = s[i++];
            while (i < n) {
                if (s[i] == '\\' && i + 1 < n) {
                    i += 2;
                    continue;
                }
                if (s[i] == quote) {
                    i++;
                    break;
                }
                i++;
            }
            continue;
        }
        if (s[i] == '{') {
            depth++;
        } else if (s[i] == '}') {
            if (depth == 0) return SIZE_MAX;
            depth--;
            if (depth == 0) return i;
        }
        i++;
    }
    return SIZE_MAX;
}

/* Canonical generated form: `void L_<8 hex>(CPU *restrict c)\n{`. */
static size_t find_func(const char *s, size_t n, size_t start)
{
    size_t i;
    for (i = start; i + 7 <= n; i++) {
        const char *nl;
        size_t open;
        if (i != 0 && s[i - 1] != '\n') continue;
        if (strncmp(s + i, "void L_", 7) != 0) continue;
        nl = (const char *)memchr(s + i, '\n', n - i);
        if (!nl) return SIZE_MAX;
        open = (size_t)(nl - s) + 1u;
        if (open < n && s[open] == '\r') open++;
        if (open < n && s[open] == '{') return i;
    }
    return SIZE_MAX;
}

static int region_has_word(const char *s, size_t n, const char *w)
{
    size_t len = strlen(w);
    size_t i;
    if (!len || len > n) return 0;
    for (i = 0; i + len <= n; i++) {
        if (s[i] != w[0]) continue;
        if (i > 0 && word_char((unsigned char)s[i - 1u])) continue;
        if (strncmp(s + i, w, len) != 0) continue;
        if (i + len < n && word_char((unsigned char)s[i + len])) continue;
        return 1;
    }
    return 0;
}

static int body_uses_mem(const char *s, size_t n)
{
    static const char *const words[] = {
        "guest_host_ptr",
        "rd8", "rd16", "rd32", "rd64",
        "wr8", "wr16", "wr32", "wr64",
        "push32", "pop32",
        "rdss", "rdsd", "rdxm",
        "wrss", "wrsd", "wrxm",
        "FS_RD8", "FS_RD16", "FS_RD32",
        "FS_WR8", "FS_WR16", "FS_WR32"
    };
    size_t i;
    for (i = 0; i < sizeof words / sizeof words[0]; i++)
        if (region_has_word(s, n, words[i])) return 1;
    return 0;
}

static void append_macro_block(Buf *out)
{
    buf_str(out,
        "\n"
        "/* " MARKER ": the guest window base is write-once.  This snapshot is\n"
        " * taken after boot and is not a new address-space model. */\n"
        "#define guest_host_ptr(a) guest_host_ptr_at((a), wc3_ghb)\n"
        "#define rd8(a) rd8_at((a), wc3_ghb)\n"
        "#define rd16(a) rd16_at((a), wc3_ghb)\n"
        "#define rd32(a) rd32_at((a), wc3_ghb)\n"
        "#define rd64(a) rd64_at((a), wc3_ghb)\n"
        "#define wr8(a, v) wr8_at((a), (v), wc3_ghb)\n"
        "#define wr16(a, v) wr16_at((a), (v), wc3_ghb)\n"
        "#define wr32(a, v) wr32_at((a), (v), wc3_ghb)\n"
        "#define wr64(a, v) wr64_at((a), (v), wc3_ghb)\n"
        "#define push32(c, v) push32_at((c), (v), wc3_ghb)\n"
        "#define pop32(c) pop32_at((c), wc3_ghb)\n"
        "#define rdss(a) rdss_at((a), wc3_ghb)\n"
        "#define rdsd(a) rdsd_at((a), wc3_ghb)\n"
        "#define rdxm(a) rdxm_at((a), wc3_ghb)\n"
        "#define wrss(a, v) wrss_at((a), (v), wc3_ghb)\n"
        "#define wrsd(a, v) wrsd_at((a), (v), wc3_ghb)\n"
        "#define wrxm(a, v) wrxm_at((a), (v), wc3_ghb)\n");
}

static void append_undef_block(Buf *out)
{
    buf_str(out,
        "\n#undef guest_host_ptr\n"
        "#undef rd8\n#undef rd16\n#undef rd32\n#undef rd64\n"
        "#undef wr8\n#undef wr16\n#undef wr32\n#undef wr64\n"
        "#undef push32\n#undef pop32\n"
        "#undef rdss\n#undef rdsd\n#undef rdxm\n"
        "#undef wrss\n#undef wrsd\n#undef wrxm\n");
}


/* Transform one translation unit.  Returns 1 when at least one function was
 * rewritten.  The output buffer is always complete, including the no-op case. */
static int transform_text(const char *text, size_t n, Buf *out, Stats *st)
{
    char *masked;
    FuncVec funcs = {0};
    size_t start = 0;
    size_t last = 0;
    size_t ins = SIZE_MAX;
    size_t i;
    int any = 0;

    if (strstr(text, HEADER_INCLUDE) || strstr(text, MARKER)) {
        buf_add(out, text, n);
        st->files_skipped++;
        return 0;
    }

    masked = (char *)xmalloc(n + 1u);
    mask_code(text, n, masked);

    while (start < n) {
        size_t hdr = find_func(masked, n, start);
        size_t nl;
        size_t open;
        size_t close;
        int mem;
        if (hdr == SIZE_MAX) break;
        nl = (size_t)((const char *)memchr(masked + hdr, '\n', n - hdr) - masked);
        open = nl + 1u;
        if (open < n && masked[open] == '\r') open++;
        if (open >= n || masked[open] != '{') {
            start = hdr + 1u;
            continue;
        }
        close = matching_brace(masked, n, open);
        if (close == SIZE_MAX) {
            free(masked);
            free(funcs.p);
            die("unbalanced function at byte %zu", hdr);
        }
        mem = body_uses_mem(masked + open + 1u, close - open - 1u);
        func_push(&funcs, hdr, open, close, mem);
        if (mem && ins == SIZE_MAX) ins = hdr;
        if (mem) any = 1;
        start = close + 1u;
    }
    free(masked);

    st->functions += funcs.n;
    if (!any) {
        buf_add(out, text, n);
        free(funcs.p);
        return 0;
    }

    for (i = 0; i < funcs.n; i++) {
        Func *f = &funcs.p[i];
        if (!f->uses_mem) continue;
        if (last <= ins) {
            buf_add(out, text + last, ins - last);
            buf_str(out, HEADER_INCLUDE "\n");
            append_macro_block(out);
            last = ins;
        }
        buf_add(out, text + last, f->open + 1u - last);
        buf_str(out, "\n    const uintptr_t wc3_ghb = g_guest_host_base;\n");
        last = f->open + 1u;
        st->functions_rewritten++;
    }
    buf_add(out, text + last, n - last);
    append_undef_block(out);

    st->files_rewritten++;
    free(funcs.p);
    return 1;
}

static void read_paths(const char *list, PathVec *paths)
{
    FILE *f = fopen(list, "rb");
    char line[8192];
    if (!f) die("cannot open input list %s", list);
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        size_t n;
        while (*p == ' ' || *p == '\t') p++;
        n = strlen(p);
        while (n && (p[n - 1u] == '\n' || p[n - 1u] == '\r' ||
                     p[n - 1u] == ' ' || p[n - 1u] == '\t')) p[--n] = 0;
        if (!n || p[0] == '#') continue;
        path_push(paths, xstrdup(p));
    }
    fclose(f);
    if (!paths->n) die("input list is empty: %s", list);
}

static void write_report(const char *path, const Stats *st)
{
    FILE *f;
    if (!path) return;
    f = fopen(path, "wb");
    if (!f) die("cannot create report %s", path);
    fprintf(f,
            "{\n"
            "  \"verdict\": \"pass\",\n"
            "  \"inputs\": %zu,\n"
            "  \"files_rewritten\": %zu,\n"
            "  \"files_skipped_existing\": %zu,\n"
            "  \"functions\": %zu,\n"
            "  \"functions_rewritten\": %zu\n"
            "}\n",
            st->files, st->files_rewritten, st->files_skipped,
            st->functions, st->functions_rewritten);
    fclose(f);
}

static int self_test(void)
{
    static const char input[] =
        "#include \"cpu.h\"\n"
        "#include \"direct_call.h\"\n"
        "#include \"fake_static_helper.h\"\n"
        "\n"
        "void L_00000001(CPU *restrict c)\n"
        "{\n"
        "    c->eax = rd32(c->ecx + 4u);\n"
        "    c->esp += 4; return;\n"
        "}\n"
        "\n"
        "void L_00000002(CPU *restrict c)\n"
        "{\n"
        "    c->eax = 1u;\n"
        "    c->esp += 4; return;\n"
        "}\n";
    static const char comment_only[] =
        "#include \"cpu.h\"\n"
        "#include \"direct_call.h\"\n"
        "void L_00000003(CPU *restrict c)\n"
        "{\n"
        "    /* rd32(c->ecx) in a comment */\n"
        "    c->esp += 4; return;\n"
        "}\n";
    Buf out = {0};
    Stats st = {0};
    const char *p;
    const char *hdr;
    const char *fake;

    if (!transform_text(input, sizeof input - 1u, &out, &st)) {
        fprintf(stderr, "guest_base self-test: expected a rewrite\n");
        free(out.p);
        return 1;
    }
    if (!strstr(out.p, HEADER_INCLUDE) ||
        !strstr(out.p, "const uintptr_t wc3_ghb = g_guest_host_base;") ||
        !strstr(out.p, "#define rd32(a) rd32_at") ||
        !strstr(out.p, "c->eax = rd32(c->ecx + 4u);") ||
        strstr(out.p, "const uintptr_t wc3_ghb = g_guest_host_base;\n    c->eax = 1u")) {
        fprintf(stderr, "guest_base self-test: bad rewritten text\n");
        free(out.p);
        return 1;
    }
    if (st.functions != 2 || st.functions_rewritten != 1) {
        fprintf(stderr, "guest_base self-test: function accounting %zu/%zu\n",
                st.functions, st.functions_rewritten);
        free(out.p);
        return 1;
    }
    hdr = strstr(out.p, HEADER_INCLUDE);
    fake = strstr(out.p, "#include \"fake_static_helper.h\"");
    if (!hdr || !fake || fake > hdr) {
        fprintf(stderr, "guest_base self-test: helper include before static header\n");
        free(out.p);
        return 1;
    }
    {
        Buf again = {0};
        Stats st2 = {0};
        char *first = out.p;
        size_t first_n = out.n;
        int changed = transform_text(first, first_n, &again, &st2);
        if (changed || st2.files_skipped != 1u ||
            again.n != first_n || memcmp(again.p, first, first_n) != 0) {
            fprintf(stderr, "guest_base self-test: second pass is not a no-op\n");
            free(again.p);
            free(first);
            return 1;
        }
        free(again.p);
        out.p = first;
        out.n = first_n;
    }
    free(out.p);
    out.p = NULL;
    out.n = out.cap = 0;
    st = (Stats){0};
    if (transform_text(comment_only, sizeof comment_only - 1u, &out, &st)) {
        fprintf(stderr, "guest_base self-test: comment counted as code\n");
        free(out.p);
        return 1;
    }
    if (st.files_rewritten != 0) {
        fprintf(stderr, "guest_base self-test: comment-only file rewritten\n");
        free(out.p);
        return 1;
    }
    p = out.p;
    if (!p || strcmp(p, comment_only) != 0) {
        fprintf(stderr, "guest_base self-test: no-op output changed\n");
        free(out.p);
        return 1;
    }
    free(out.p);
    puts("guest_base self-test: OK");
    return 0;
}

int main(int argc, char **argv)
{
    const char *inputs = NULL;
    const char *out_dir = NULL;
    const char *report = NULL;
    int quiet = 0;
    PathVec paths = {0};
    Stats st = {0};
    size_t i;

    if (argc == 2 && !strcmp(argv[1], "--self-test")) return self_test();
    for (i = 1; i < (size_t)argc; i++) {
        if (!strcmp(argv[i], "--inputs") && i + 1 < (size_t)argc) {
            inputs = argv[++i];
        } else if (!strcmp(argv[i], "--out-dir") && i + 1 < (size_t)argc) {
            out_dir = argv[++i];
        } else if (!strcmp(argv[i], "--report") && i + 1 < (size_t)argc) {
            report = argv[++i];
        } else if (!strcmp(argv[i], "--quiet")) {
            quiet = 1;
        } else if (!strcmp(argv[i], "--help")) {
            puts("guest_base --inputs FILE --out-dir DIR [--report JSON] [--quiet]\n"
                 "guest_base --self-test");
            return 0;
        } else {
            die("unknown argument: %s", argv[i]);
        }
    }
    if (!inputs || !out_dir) die("--inputs and --out-dir are required");
    read_paths(inputs, &paths);
    st.files = paths.n;

    for (i = 0; i < paths.n; i++) {
        size_t n;
        char *text = read_file(paths.p[i], &n);
        Buf out = {0};
        const char *base = path_base(paths.p[i]);
        size_t need = strlen(out_dir) + strlen(base) + 2u;
        char *out_path = (char *)xmalloc(need);
        snprintf(out_path, need, "%s/%s", out_dir, base);
        transform_text(text, n, &out, &st);
        write_file(out_path, out.p ? out.p : "", out.n);
        free(out_path);
        free(out.p);
        free(text);
    }
    if (!quiet)
        fprintf(stderr,
                "guest_base: %zu files, %zu rewritten, %zu functions, "
                "%zu function bodies\n",
                st.files, st.files_rewritten, st.functions,
                st.functions_rewritten);
    write_report(report, &st);
    return 0;
}
