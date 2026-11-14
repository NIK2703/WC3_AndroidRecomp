/* leaf_call.c - D4 leaf-call fast path.
 *
 * A lifted leaf function has no guest call, tail dispatch, HLE call, W3L
 * helper or direct access to the dispatcher state.  A caller can therefore
 * enter it without the Wc3DispFrame/wc3_guest_enter/leave pair used by
 * WC3_DIRECT_CALL: nothing inside the callee can observe or create a pending
 * tail target, and the guest return address still lives at [esp] exactly as
 * before.  Only disp_depth is maintained for recursion diagnostics.
 *
 * This pass runs once over all final relifted chunks, after every other
 * corpus text pass.  It builds one global leaf set from the complete corpus,
 * then rewrites only calls whose target is in that set.  The set is global
 * because splitcorpus can place a caller and its leaf callee in different
 * translation units.
 *
 *   leaf_call --inputs FILE --out-dir DIR [--report JSON] [--quiet]
 *   leaf_call --self-test
 *
 * `--inputs` is a text file with one final chunk path per line.  Output files
 * keep the input basename.  A function that cannot be parsed is never treated
 * as a leaf, so an unfamiliar generated form can only miss an optimization,
 * not change dispatch semantics.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CALL_MACRO "WC3_DIRECT_CALL"
#define CALL_MACRO_LEN (sizeof(CALL_MACRO) - 1u)

typedef struct {
    char *p;
    size_t n;
    size_t cap;
} Buf;

typedef struct {
    uint32_t *p;
    size_t n;
    size_t cap;
} VaVec;

typedef struct {
    char **p;
    size_t n;
    size_t cap;
} PathVec;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("leaf_call: ", stderr);
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

static void vec_push(VaVec *v, uint32_t x)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2u : 1024u;
        v->p = (uint32_t *)xrealloc(v->p, v->cap * sizeof *v->p);
    }
    v->p[v->n++] = x;
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
        if (s[i] == '{') depth++;
        else if (s[i] == '}') {
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
        open = (size_t)(nl - s) + 1;
        if (open < n && s[open] == '{') return i;
        if (open + 1 < n && s[open] == '\r' && s[open + 1] == '{') return i;
    }
    return SIZE_MAX;
}

static int func_va(const char *s, size_t n, size_t hdr, uint32_t *out)
{
    uint32_t v = 0;
    size_t i;
    if (hdr + 15u > n) return 0;
    if (strncmp(s + hdr, "void L_", 7) != 0) return 0;
    for (i = 0; i < 8u; i++) {
        int c = (unsigned char)s[hdr + 7u + i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = (v << 4) | (uint32_t)d;
    }
    if (word_char((unsigned char)s[hdr + 15u])) return 0;
    *out = v;
    return 1;
}

/* A leaf may contain branch labels, loops and multiple returns: all of them
 * stay inside its host frame.  It must not contain anything that can enter
 * another guest function, publish a tail target, or touch dispatcher state. */
static int body_is_leaf(const char *s)
{
    if (strstr(s, CALL_MACRO)) return 0;
    if (strstr(s, "WC3_DIRECT_TAIL")) return 0;
    if (strstr(s, "WC3_GUEST_DRAIN")) return 0;
    if (strstr(s, "WC3_GUEST_")) return 0;
    if (strstr(s, "dispatch")) return 0;
    if (strstr(s, "hle_")) return 0;
    if (strstr(s, "w3l_")) return 0;
    if (strstr(s, "guest_")) return 0;
    if (strstr(s, "wc3_dispatch_")) return 0;
    if (strstr(s, "wc3_legacy_")) return 0;
    if (strstr(s, "disp_")) return 0;
    return 1;
}

static int call_target(const char *masked, size_t n, size_t pos, uint32_t *out)
{
    size_t p = pos + CALL_MACRO_LEN;
    uint32_t v = 0;
    size_t i;
    if (pos > 0 && word_char((unsigned char)masked[pos - 1u])) return 0;
    if (p < n && masked[p] == '_') return 0;   /* already _LEAF */
    while (p < n && (masked[p] == ' ' || masked[p] == '\t')) p++;
    if (p >= n || masked[p] != '(') return 0;
    p++;
    while (p < n && (masked[p] == ' ' || masked[p] == '\t')) p++;
    if (p + 10u > n || masked[p] != 'L' || masked[p + 1] != '_') return 0;
    p += 2;
    for (i = 0; i < 8u; i++) {
        int c = (unsigned char)masked[p + i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = (v << 4) | (uint32_t)d;
    }
    if (p + 8u < n && word_char((unsigned char)masked[p + 8u])) return 0;
    *out = v;
    return 1;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static int leaf_contains(const VaVec *v, uint32_t x)
{
    if (!v->n) return 0;
    return bsearch(&x, v->p, v->n, sizeof x, cmp_u32) != NULL;
}

static void sort_leafs(VaVec *v)
{
    if (v->n > 1u) qsort(v->p, v->n, sizeof *v->p, cmp_u32);
}

static const char *path_base(const char *path)
{
    const char *p = path;
    const char *last = NULL;
    for (; *p; p++)
        if (*p == '/' || *p == '\\') last = p;
    return last ? last + 1 : path;
}

static void collect_leafs(const char *path, VaVec *leafs, size_t *funcs)
{
    size_t n;
    char *text = read_file(path, &n);
    char *masked = (char *)xmalloc(n + 1u);
    size_t scan = 0;
    mask_code(text, n, masked);
    while (scan < n) {
        size_t hdr = find_func(masked, n, scan);
        const char *nl;
        size_t open, close;
        uint32_t va;
        char saved;
        if (hdr == SIZE_MAX) break;
        nl = (const char *)memchr(masked + hdr, '\n', n - hdr);
        if (!nl) break;
        open = (size_t)(nl - masked) + 1u;
        if (open + 1u < n && masked[open] == '\r' && masked[open + 1u] == '{') open++;
        if (open >= n || masked[open] != '{') {
            scan = open + 1u;
            continue;
        }
        close = matching_brace(text, n, open);
        if (close == SIZE_MAX) break;
        (*funcs)++;
        if (func_va(masked, n, hdr, &va)) {
            saved = masked[close + 1u];
            masked[close + 1u] = 0;
            if (body_is_leaf(masked + open)) vec_push(leafs, va);
            masked[close + 1u] = saved;
        }
        scan = close + 1u;
    }
    free(masked);
    free(text);
}

static void rewrite_file(const char *path, const char *out_dir,
                         const VaVec *leafs, size_t *calls, size_t *rewrites)
{
    size_t n;
    char *text = read_file(path, &n);
    char *masked = (char *)xmalloc(n + 1u);
    Buf out = {0};
    size_t scan = 0;
    size_t last = 0;
    const char *base = path_base(path);
    char *out_path;
    size_t out_len = strlen(out_dir);
    size_t base_len = strlen(base);
    if (out_len == 0) die("empty --out-dir");
    out_path = (char *)xmalloc(out_len + base_len + 2u);
    memcpy(out_path, out_dir, out_len);
    if (out_path[out_len - 1u] != '/' && out_path[out_len - 1u] != '\\')
        out_path[out_len++] = '/';
    memcpy(out_path + out_len, base, base_len + 1u);

    mask_code(text, n, masked);
    while (scan + CALL_MACRO_LEN <= n) {
        const char *hit = strstr(masked + scan, CALL_MACRO);
        size_t pos;
        uint32_t va;
        if (!hit) break;
        pos = (size_t)(hit - masked);
        if (call_target(masked, n, pos, &va)) {
            (*calls)++;
            if (leaf_contains(leafs, va)) {
                buf_add(&out, text + last, pos - last);
                buf_str(&out, CALL_MACRO "_LEAF");
                last = pos + CALL_MACRO_LEN;
                scan = last;
                (*rewrites)++;
                continue;
            }
        }
        scan = pos + CALL_MACRO_LEN;
    }
    buf_add(&out, text + last, n - last);
    write_file(out_path, out.p ? out.p : "", out.n);
    free(out.p);
    free(out_path);
    free(masked);
    free(text);
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

static int self_test(void)
{
    static const char *src =
        "/* void L_00000007(CPU *restrict c)\n"
        "{\n"
        "    WC3_DIRECT_CALL(L_00000001, c);\n"
        "}\n"
        "*/\n"
        "void L_00000001(CPU *restrict c)\n"
        "{\n"
        "    c->eax = 1; /* WC3_DIRECT_CALL(L_0000000D, c); */\n"
        "}\n"
        "void L_00000002(CPU *restrict c)\n"
        "{\n"
        "    push32(c, 0u); WC3_DIRECT_CALL(L_00000001, c);\n"
        "}\n"
        "void L_00000003(CPU *restrict c)\n"
        "{\n"
        "    dispatch(c, 0x1u);\n"
        "}\n"
        "void L_00000004(CPU *restrict c)\n"
        "{\n"
        "    wc3_w3l_handle_0x83(c);\n"
        "}\n"
        "void L_00000005(CPU *restrict c)\n"
        "{\n"
        "    WC3_DIRECT_CALL_LEAF(L_00000001, c);\n"
        "}\n"
        "void L_00000006(CPU *restrict c)\n"
        "{\n"
        "    WC3_DIRECT_CALL(L_00000003, c);\n"
        "}\n"
        "void L_00000007(CPU *restrict c)\n"
        "{\n"
        "    WC3_DIRECT_TAIL(L_00000001, c);\n"
        "}\n"
        "void L_00000008(CPU *restrict c)\n"
        "{\n"
        "    c->disp_pending_valid = 1u;\n"
        "}\n"
        "void L_00000009(CPU *restrict c)\n"
        "{\n"
        "    hle_probe(c);\n"
        "}\n"
        "void L_0000000A(CPU *restrict c)\n"
        "{\n"
        "    guest_probe(c);\n"
        "}\n"
        "void L_0000000B(CPU *restrict c)\n"
        "{\n"
        "    WC3_GUEST_DRAIN(c);\n"
        "}\n"
        "void L_0000000C(CPU *restrict c)\n"
        "{\n"
        "    WC3_DIRECT_CALL(L_00000008, c);\n"
        "    WC3_DIRECT_CALL(L_00000009, c);\n"
        "    WC3_DIRECT_CALL(L_0000000A, c);\n"
        "    WC3_DIRECT_CALL(L_0000000B, c);\n"
        "}\n";
    size_t n = strlen(src);
    char *text = (char *)xmalloc(n + 1u);
    char *masked = (char *)xmalloc(n + 1u);
    VaVec leafs = {0};
    size_t scan = 0, funcs = 0;
    Buf out = {0};
    size_t last = 0, calls = 0, rewrites = 0;
    memcpy(text, src, n + 1u);
    mask_code(text, n, masked);
    while (scan < n) {
        size_t hdr = find_func(masked, n, scan);
        const char *nl;
        size_t open, close;
        uint32_t va;
        char saved;
        if (hdr == SIZE_MAX) break;
        nl = (const char *)memchr(masked + hdr, '\n', n - hdr);
        if (!nl) break;
        open = (size_t)(nl - masked) + 1u;
        if (open >= n || masked[open] != '{') break;
        close = matching_brace(text, n, open);
        if (close == SIZE_MAX) break;
        funcs++;
        if (func_va(masked, n, hdr, &va)) {
            saved = masked[close + 1u];
            masked[close + 1u] = 0;
            if (body_is_leaf(masked + open)) vec_push(&leafs, va);
            masked[close + 1u] = saved;
        }
        scan = close + 1u;
    }
    sort_leafs(&leafs);
    scan = 0;
    while (scan + CALL_MACRO_LEN <= n) {
        const char *hit = strstr(masked + scan, CALL_MACRO);
        size_t pos;
        uint32_t va;
        if (!hit) break;
        pos = (size_t)(hit - masked);
        if (call_target(masked, n, pos, &va)) {
            calls++;
            if (leaf_contains(&leafs, va)) {
                buf_add(&out, text + last, pos - last);
                buf_str(&out, CALL_MACRO "_LEAF");
                last = pos + CALL_MACRO_LEN;
                scan = last;
                rewrites++;
                continue;
            }
        }
        scan = pos + CALL_MACRO_LEN;
    }
    buf_add(&out, text + last, n - last);
    if (funcs != 12u || leafs.n != 1u || leafs.p[0] != 1u ||
        calls != 6u || rewrites != 1u ||
        !strstr(out.p, "WC3_DIRECT_CALL_LEAF(L_00000001, c)") ||
        !strstr(out.p, "WC3_DIRECT_CALL(L_00000003, c)") ||
        !strstr(out.p, "WC3_DIRECT_CALL(L_00000008, c)") ||
        !strstr(out.p, "WC3_DIRECT_CALL(L_00000009, c)") ||
        !strstr(out.p, "WC3_DIRECT_CALL(L_0000000A, c)") ||
        !strstr(out.p, "WC3_DIRECT_CALL(L_0000000B, c)") ||
        !strstr(out.p, "/* WC3_DIRECT_CALL(L_0000000D, c); */") ||
        strstr(out.p, "WC3_DIRECT_CALL_LEAF(L_0000000D") ||
        strstr(out.p, "WC3_DIRECT_CALL_LEAF(L_00000003") ||
        strstr(out.p, "WC3_DIRECT_CALL_LEAF(L_00000008") ||
        strstr(out.p, "WC3_DIRECT_CALL_LEAF(L_00000009") ||
        strstr(out.p, "WC3_DIRECT_CALL_LEAF(L_0000000A") ||
        strstr(out.p, "WC3_DIRECT_CALL_LEAF(L_0000000B")) {
        fprintf(stderr, "leaf_call self-test failed: funcs=%zu leafs=%zu "
                        "calls=%zu rewrites=%zu\n",
                funcs, leafs.n, calls, rewrites);
        return 1;
    }
    puts("leaf_call self-test: OK");
    free(out.p);
    free(masked);
    free(text);
    free(leafs.p);
    return 0;
}

int main(int argc, char **argv)
{
    const char *inputs = NULL;
    const char *out_dir = NULL;
    const char *report = NULL;
    int quiet = 0;
    PathVec paths = {0};
    VaVec leafs = {0};
    size_t funcs = 0, calls = 0, rewrites = 0;
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
        } else {
            die("unknown argument: %s", argv[i]);
        }
    }
    if (!inputs || !out_dir) die("--inputs and --out-dir are required");

    read_paths(inputs, &paths);
    for (i = 0; i < paths.n; i++)
        collect_leafs(paths.p[i], &leafs, &funcs);
    if (!funcs) die("no canonical functions parsed from %zu inputs", paths.n);
    sort_leafs(&leafs);
    for (i = 0; i < paths.n; i++)
        rewrite_file(paths.p[i], out_dir, &leafs, &calls, &rewrites);

    if (report) {
        FILE *f = fopen(report, "wb");
        if (!f) die("cannot create report %s", report);
        fprintf(f,
                "{\n"
                "  \"schema\": 1,\n"
                "  \"inputs\": %zu,\n"
                "  \"functions\": %zu,\n"
                "  \"leaf_functions\": %zu,\n"
                "  \"calls_seen\": %zu,\n"
                "  \"calls_rewritten\": %zu,\n"
                "  \"verdict\": \"pass\"\n"
                "}\n",
                paths.n, funcs, leafs.n, calls, rewrites);
        if (fclose(f) != 0) die("cannot close report %s", report);
    }
    if (!quiet)
        printf("leaf_call: %zu inputs, %zu functions, %zu leafs, "
               "%zu calls, %zu rewritten\n",
               paths.n, funcs, leafs.n, calls, rewrites);

    for (i = 0; i < paths.n; i++) free(paths.p[i]);
    free(paths.p);
    free(leafs.p);
    return 0;
}
