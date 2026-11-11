/* save_list_display.c - make the save/load list show the slot basename.
 *
 * The pass is a product invariant: the save/load list must show the slot
 * basename without ".w3z", not the Android save description.  It inserts a
 * display-only block before every 0x6F2A953D instruction and can append an
 * artifact marker that the product build checks.
 *
 *   save_list_display --lift IN --out OUT [--json REPORT] [--expect-sites N]
 *                     [--marker]
 *   save_list_display --self-test
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sha256.h"

#if defined(_WIN32)
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <unistd.h>
#define make_dir(p) mkdir((p), 0777)
#endif

#define PASS "save_list_display"
#define PASS_REV 5
#define TARGET_VA "0x6F2A953D"
#define MARKER "WC3_SAVE_LIST_STRIP_W3Z"
#define MARKER_SYMBOL "wc3_save_list_display_fix_marker"

static const char MARKER_BLOCK[] =
    "\n"
    "/* " MARKER " build marker: the pass was applied to this translation unit.\n"
    " * The product build checks this symbol/string, so a corpus that silently\n"
    " * bypassed the pass cannot become an APK. */\n"
    "#if defined(__GNUC__)\n"
    "__attribute__((used, visibility(\"hidden\")))\n"
    "#endif\n"
    "const char " MARKER_SYMBOL "[] = \"" MARKER "\";\n";

static const char BLOCK[] =
    "    /* " MARKER ": display-only basename, keeps [ebp-0x21c] intact.\n"
    "     * VA " TARGET_VA ", pass " PASS " rev 5. */\n"
    "    {\n"
    "        uint32_t _wc3_path = c->ebp + 0xFFFFFDE4u; /* [ebp - 0x21c] */\n"
    "        uint32_t _wc3_dst = c->ebp + 0xFFFFFEE8u;  /* [ebp - 0x118] */\n"
    "        uint32_t _wc3_len = 0;\n"
    "        uint32_t _wc3_base = 0;\n"
    "        uint32_t _wc3_i;\n"
    "        while (rd8(_wc3_path + _wc3_len) != 0) {\n"
    "            uint8_t _wc3_ch = rd8(_wc3_path + _wc3_len);\n"
    "            if (_wc3_ch == 92u || _wc3_ch == 47u) { /* backslash or slash */\n"
    "                _wc3_base = _wc3_len + 1;\n"
    "            }\n"
    "            _wc3_len++;\n"
    "        }\n"
    "        if (_wc3_len >= _wc3_base + 4 &&\n"
    "            rd8(_wc3_path + _wc3_len - 4) == '.' &&\n"
    "            (rd8(_wc3_path + _wc3_len - 3) | 0x20) == 'w' &&\n"
    "            rd8(_wc3_path + _wc3_len - 2) == '3' &&\n"
    "            (rd8(_wc3_path + _wc3_len - 1) | 0x20) == 'z') {\n"
    "            uint32_t _wc3_n = _wc3_len - _wc3_base - 4;\n"
    "            if (_wc3_n > 0x103u) {\n"
    "                _wc3_n = 0x103u;\n"
    "            }\n"
    "            for (_wc3_i = 0; _wc3_i < _wc3_n; _wc3_i++) {\n"
    "                wr8(_wc3_dst + _wc3_i, rd8(_wc3_path + _wc3_base + _wc3_i));\n"
    "            }\n"
    "            wr8(_wc3_dst + _wc3_n, 0);\n"
    "        }\n"
    "    }\n";

typedef struct {
    char *p;
    size_t n;
    size_t cap;
} Buf;

typedef struct {
    size_t start;
    size_t body;
    char name[16];
} Func;

typedef struct {
    Func *v;
    size_t n;
    size_t cap;
} FuncVec;

typedef struct {
    size_t pos;
    size_t func_start;
    size_t func_index;
    int marked;
} Site;

typedef struct {
    Site *v;
    size_t n;
    size_t cap;
} SiteVec;

typedef struct {
    size_t start;
    size_t end;
    const char *repl;
    size_t repl_n;
} Edit;

static void die(const char *msg)
{
    fprintf(stderr, PASS ": %s\n", msg);
    exit(2);
}

static void die_oom(void)
{
    die("out of memory");
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die_oom();
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)xrealloc(NULL, n);
    memcpy(p, s, n);
    return p;
}

static void buf_need(Buf *b, size_t extra)
{
    size_t need = b->n + extra;
    if (need < b->cap) return;
    if (b->cap == 0) b->cap = 4096;
    while (b->cap <= need) {
        if (b->cap > (size_t)-1 / 2) die_oom();
        b->cap *= 2;
    }
    b->p = (char *)xrealloc(b->p, b->cap);
    b->p[b->n] = 0;
}

static void buf_add(Buf *b, const void *p, size_t n)
{
    buf_need(b, n);
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void buf_str(Buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

static char *slurp(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *p;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    p = (char *)xrealloc(NULL, (size_t)sz + 1);
    if (fread(p, 1, (size_t)sz, f) != (size_t)sz) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    p[sz] = 0;
    *out_n = (size_t)sz;
    return p;
}

static int write_all(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    size_t put;
    if (!f) return -1;
    put = fwrite(data, 1, n, f);
    if (fclose(f) != 0 || put != n) return -1;
    return 0;
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static int mkdir_p(const char *path)
{
    char *tmp = xstrdup(path);
    char *p;
    for (p = tmp; *p; p++) {
        if ((*p == '/' || *p == '\\') && p != tmp && p[1]) {
            char c = *p;
            *p = 0;
            if (!is_dir(tmp)) make_dir(tmp);
            *p = c;
        }
    }
    if (!is_dir(tmp)) make_dir(tmp);
    if (!is_dir(tmp)) {
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

static void mkdir_parent(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *last = slash;
    if (bslash && (!last || bslash > last)) last = bslash;
    if (last && last != path) {
        char *parent = xstrdup(path);
        parent[(size_t)(last - path)] = 0;
        mkdir_p(parent);
        free(parent);
    }
}

static char *path_slash(const char *path)
{
#if defined(_WIN32)
    char *full = _fullpath(NULL, path, 0);
#else
    char *full = NULL;
    if (path[0] == '/') {
        full = xstrdup(path);
    } else {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))) {
            size_t dl = strlen(cwd);
            size_t pl = strlen(path);
            full = (char *)xrealloc(NULL, dl + pl + 2);
            memcpy(full, cwd, dl);
            full[dl] = '/';
            memcpy(full + dl + 1, path, pl + 1);
        }
    }
#endif
    char *p;
    if (!full) full = xstrdup(path);
    for (p = full; *p; p++)
        if (*p == '\\') *p = '/';
    return full;
}

static int starts_line(const char *s, size_t p)
{
    return p == 0 || s[p - 1] == '\n';
}

static int is_hex_upper(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
}

static int func_header_at(const char *s, size_t n, size_t p, Func *out)
{
    static const char prefix[] = "void L_";
    size_t i = p + 7;
    size_t name_start = p + 5;
    size_t name_len;
    if (!starts_line(s, p)) return 0;
    if (p + 7 > n || memcmp(s + p, prefix, 7) != 0) return 0;
    while (i < n && is_hex_upper((unsigned char)s[i])) i++;
    name_len = i - name_start;
    if (name_len == 0 || name_len > 15) return 0;
    if (i + 7 > n || memcmp(s + i, "(CPU *c)", 8) != 0) return 0;
    i += 8;
    if (i < n && s[i] == '\r') i++;
    if (i >= n || s[i] != '\n') return 0;
    i++;
    if (i >= n || s[i] != '{') return 0;
    i++;
    if (i < n && s[i] == '\r') i++;
    if (i >= n || s[i] != '\n') return 0;
    i++;
    out->start = p;
    out->body = i;
    memcpy(out->name, s + name_start, name_len);
    out->name[name_len] = 0;
    return 1;
}

static FuncVec find_funcs(const char *s, size_t n)
{
    FuncVec f = {0};
    size_t i;
    for (i = 0; i < n; i++) {
        Func fn;
        if (s[i] != 'v') continue;
        if (!func_header_at(s, n, i, &fn)) continue;
        if (f.n == f.cap) {
            f.cap = f.cap ? f.cap * 2 : 64;
            f.v = (Func *)xrealloc(f.v, f.cap * sizeof(*f.v));
        }
        f.v[f.n++] = fn;
    }
    return f;
}

static int find_site_line(const char *s, size_t start, size_t end, size_t *pos)
{
    static const char needle[] = "/* 6F2A953D:";
    size_t i;
    for (i = start; i + sizeof(needle) - 1 <= end; i++) {
        if (memcmp(s + i, needle, sizeof(needle) - 1) == 0) {
            *pos = start;
            return 1;
        }
    }
    return 0;
}

static SiteVec find_sites(const char *s, size_t n, const FuncVec *funcs)
{
    SiteVec sites = {0};
    size_t line = 0;
    if (!funcs->n) die("no lifted functions found");
    while (line < n) {
        size_t end = line;
        size_t pos;
        while (end < n && s[end] != '\n') end++;
        if (find_site_line(s, line, end, &pos)) {
            size_t k;
            size_t fi = (size_t)-1;
            Site st;
            for (k = 0; k < funcs->n; k++)
                if (funcs->v[k].start < pos) fi = k;
            if (fi == (size_t)-1) die("site " TARGET_VA " has no containing function");
            st.pos = pos;
            st.func_index = fi;
            st.func_start = funcs->v[fi].start;
            st.marked = 0;
            {
                const char *m = s + st.func_start;
                size_t left = pos - st.func_start;
                size_t ml = strlen(MARKER);
                size_t j;
                for (j = 0; j + ml <= left; j++) {
                    if (memcmp(m + j, MARKER, ml) == 0) {
                        st.marked = 1;
                        break;
                    }
                }
            }
            if (sites.n == sites.cap) {
                sites.cap = sites.cap ? sites.cap * 2 : 8;
                sites.v = (Site *)xrealloc(sites.v, sites.cap * sizeof(*sites.v));
            }
            sites.v[sites.n++] = st;
        }
        line = end + (end < n ? 1 : 0);
    }
    if (!sites.n) die(TARGET_VA " not found");
    return sites;
}

static size_t rfind_char(const char *s, size_t from, size_t to, char ch)
{
    size_t i = to;
    while (i > from) {
        i--;
        if (s[i] == ch) return i;
    }
    return (size_t)-1;
}

static int is_space_byte(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}

static int old_block_start(const char *s, size_t site_pos, size_t func_start,
                           size_t *out_start)
{
    size_t ml = strlen(MARKER);
    size_t marker_pos = (size_t)-1;
    size_t i;
    if (site_pos <= func_start) return 0;
    for (i = func_start; i + ml <= site_pos; i++) {
        if (memcmp(s + i, MARKER, ml) == 0) marker_pos = i;
    }
    if (marker_pos == (size_t)-1) return 0;
    {
        size_t nl = rfind_char(s, 0, marker_pos, '\n');
        size_t block_start = nl == (size_t)-1 ? 0 : nl + 1;
        size_t e = site_pos;
        while (e > block_start && is_space_byte((unsigned char)s[e - 1])) e--;
        if (e == block_start || s[e - 1] != '}') return 0;
        *out_start = block_start;
        return 1;
    }
}

static Buf transform(const char *s, size_t n, const FuncVec *funcs,
                     const SiteVec *sites, int *changed)
{
    Edit *edits = NULL;
    (void)funcs;
    size_t nedits = 0, cap = 0;
    size_t i, j;
    Buf out = {0};

    for (i = 0; i < sites->n; i++) {
        const Site *st = &sites->v[i];
        size_t start = 0, end = 0;
        int add = 0;
        if (st->marked) {
            size_t bs;
            if (old_block_start(s, st->pos, st->func_start, &bs) &&
                (st->pos - bs != strlen(BLOCK) ||
                 memcmp(s + bs, BLOCK, strlen(BLOCK)) != 0)) {
                start = bs;
                end = st->pos;
                add = 1;
            }
        } else {
            size_t nl = rfind_char(s, 0, st->pos, '\n');
            start = end = nl == (size_t)-1 ? 0 : nl + 1;
            add = 1;
        }
        if (!add) continue;
        if (nedits == cap) {
            cap = cap ? cap * 2 : 8;
            edits = (Edit *)xrealloc(edits, cap * sizeof(*edits));
        }
        edits[nedits].start = start;
        edits[nedits].end = end;
        edits[nedits].repl = BLOCK;
        edits[nedits].repl_n = strlen(BLOCK);
        nedits++;
    }
    if (!nedits) {
        out.p = (char *)xrealloc(NULL, n + 1);
        memcpy(out.p, s, n);
        out.p[n] = 0;
        out.n = n;
        *changed = 0;
        free(edits);
        return out;
    }
    /* Apply from the end so earlier offsets stay valid. */
    for (i = 0; i < nedits; i++)
        for (j = i + 1; j < nedits; j++)
            if (edits[j].start > edits[i].start) {
                Edit t = edits[i];
                edits[i] = edits[j];
                edits[j] = t;
            }
    {
        size_t cursor = 0;
        for (i = nedits; i > 0; i--) {
            Edit *e = &edits[i - 1];
            if (e->start < cursor) die("overlapping save-list edits");
            buf_add(&out, s + cursor, e->start - cursor);
            buf_add(&out, e->repl, e->repl_n);
            cursor = e->end;
        }
        buf_add(&out, s + cursor, n - cursor);
    }
    *changed = 1;
    free(edits);
    return out;
}

static Buf add_build_marker(const char *s, size_t n, int *added)
{
    Buf out = {0};
    size_t ml = strlen(MARKER_SYMBOL);
    size_t i;
    for (i = 0; i + ml <= n; i++) {
        if (memcmp(s + i, MARKER_SYMBOL, ml) == 0) {
            buf_add(&out, s, n);
            *added = 0;
            return out;
        }
    }
    buf_add(&out, s, n);
    buf_str(&out, MARKER_BLOCK);
    *added = 1;
    return out;
}

/* Normalize a function body exactly like the Python reference: strip closed
 * block comments, line comments, then collapse whitespace and trim. */
static Buf normalize(const char *s, size_t n)
{
    Buf tmp = {0}, out = {0};
    size_t i = 0;
    while (i < n) {
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
            if (j + 1 < n) {
                buf_add(&tmp, " ", 1);
                i = j + 2;
                continue;
            }
        }
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') {
            size_t j = i + 2;
            while (j < n && s[j] != '\n') j++;
            buf_add(&tmp, " ", 1);
            i = j;
            continue;
        }
        buf_add(&tmp, s + i, 1);
        i++;
    }
    {
        size_t j = 0;
        int pending_space = 0;
        while (j < tmp.n && is_space_byte((unsigned char)tmp.p[j])) j++;
        for (; j < tmp.n; j++) {
            if (is_space_byte((unsigned char)tmp.p[j])) {
                pending_space = 1;
            } else {
                if (pending_space && out.n) buf_add(&out, " ", 1);
                buf_add(&out, tmp.p + j, 1);
                pending_space = 0;
            }
        }
    }
    free(tmp.p);
    return out;
}

static int function_span(const char *s, size_t n, const char *name,
                         size_t *start, size_t *end)
{
    FuncVec funcs = find_funcs(s, n);
    size_t i;
    for (i = 0; i < funcs.n; i++) {
        if (strcmp(funcs.v[i].name, name) == 0) {
            size_t p = funcs.v[i].body;
            int depth = 1;
            while (p < n) {
                if (s[p] == '{') depth++;
                else if (s[p] == '}') {
                    depth--;
                    if (depth == 0) {
                        *start = funcs.v[i].body;
                        *end = p;
                        free(funcs.v);
                        return 1;
                    }
                }
                p++;
            }
        }
    }
    free(funcs.v);
    return 0;
}

static void function_hash(const char *s, size_t n, const char *name, char out[65])
{
    size_t start, end;
    Buf norm;
    unsigned char digest[32];
    sha256_ctx ctx;
    if (!function_span(s, n, name, &start, &end))
        die("function body not found");
    norm = normalize(s + start, end - start);
    sha256_init(&ctx);
    sha256_update(&ctx, norm.p ? norm.p : "", norm.n);
    sha256_final(&ctx, digest);
    sha256_hex(digest, out);
    free(norm.p);
}

static int count_substr(const char *s, size_t n, const char *needle)
{
    size_t nl = strlen(needle);
    size_t i;
    int count = 0;
    for (i = 0; i + nl <= n; i++)
        if (memcmp(s + i, needle, nl) == 0) count++;
    return count;
}

static int write_json_string(FILE *f, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (fputc('"', f) == EOF) return -1;
    while (*p) {
        switch (*p) {
        case '\\': if (fputs("\\\\", f) == EOF) return -1; break;
        case '"':  if (fputs("\\\"", f) == EOF) return -1; break;
        case '\n': if (fputs("\\n", f) == EOF) return -1; break;
        case '\r': if (fputs("\\r", f) == EOF) return -1; break;
        case '\t': if (fputs("\\t", f) == EOF) return -1; break;
        default:
            if (*p < 0x20) {
                if (fprintf(f, "\\u%04x", *p) < 0) return -1;
            } else if (fputc(*p, f) == EOF) return -1;
            break;
        }
        p++;
    }
    return fputc('"', f) == EOF ? -1 : 0;
}

static int write_report(const char *path, const char *src, int sites,
                        const char **names, const char *before,
                        const char *after, size_t nnames, int applied,
                        long expected_sites, int marker, int marker_count)
{
    FILE *f;
    size_t i;
    if (!path) return 0;
    mkdir_parent(path);
    f = fopen(path, "wb");
    if (!f) return -1;
    fputs("{\n  \"pass\": \"" PASS "\",\n", f);
    fprintf(f, "  \"pass_rev\": %d,\n", PASS_REV);
    fputs("  \"files\": [\n    {\n      \"path\": ", f);
    write_json_string(f, src);
    fprintf(f, ",\n      \"sites\": %d,\n      \"functions\": [\n", sites);
    for (i = 0; i < nnames; i++) {
        fputs("        {\n          \"va\": \"" TARGET_VA "\",\n"
              "          \"function\": ", f);
        write_json_string(f, names[i]);
        fputs(",\n          \"before_sha256\": ", f);
        write_json_string(f, before + i * 65);
        fputs(",\n          \"after_sha256\": ", f);
        write_json_string(f, after + i * 65);
        fputs(",\n          \"reason\": \"use the slot basename from "
              "[ebp-0x21c] as the save-list display text (PC list parity)\"\n"
              "        }", f);
        if (i + 1 != nnames) fputc(',', f);
        fputc('\n', f);
    }
    fputs("      ]\n    }\n  ],\n", f);
    fprintf(f, "  \"applied\": %s,\n", applied ? "true" : "false");
    if (expected_sites >= 0)
        fprintf(f, "  \"expected_sites\": %ld,\n", expected_sites);
    else
        fputs("  \"expected_sites\": null,\n", f);
    fputs("  \"marker_symbol\": ", f);
    if (marker) write_json_string(f, MARKER_SYMBOL);
    else fputs("null", f);
    fprintf(f, ",\n  \"marker_count\": %d,\n", marker_count);
    fputs("  \"g15\": \"unavailable\",\n"
          "  \"risk\": \"ADR-028/ADR-029: LiftPass change without a "
          "G-15 semantic-equivalence report\"\n}\n", f);
    if (fclose(f) != 0) return -1;
    return 0;
}

static int process_file(const char *src, const char *dst, const char *json,
                        long expect_sites, int marker)
{
    char *text;
    size_t text_n;
    FuncVec funcs;
    SiteVec sites;
    int changed, marker_added = 0;
    Buf out, marked;
    const char **names = NULL;
    size_t nnames = 0;
    char *before = NULL, *after = NULL;
    size_t i;
    int marker_count;

    text = slurp(src, &text_n);
    if (!text) {
        fprintf(stderr, PASS ": cannot read %s\n", src);
        return 2;
    }
    funcs = find_funcs(text, text_n);
    sites = find_sites(text, text_n, &funcs);
    if (expect_sites >= 0 && (size_t)expect_sites != sites.n) {
        fprintf(stderr, PASS ": expected %ld site(s) for " TARGET_VA
                ", found %zu\n", expect_sites, sites.n);
        free(text);
        free(funcs.v);
        free(sites.v);
        return 2;
    }
    names = (const char **)xrealloc(NULL, sites.n * sizeof(*names));
    for (i = 0; i < sites.n; i++) {
        const char *name = funcs.v[sites.v[i].func_index].name;
        size_t k;
        int seen = 0;
        for (k = 0; k < nnames; k++)
            if (strcmp(names[k], name) == 0) { seen = 1; break; }
        if (!seen) names[nnames++] = name;
    }
    before = (char *)xrealloc(NULL, nnames * 65);
    after = (char *)xrealloc(NULL, nnames * 65);
    for (i = 0; i < nnames; i++)
        function_hash(text, text_n, names[i], before + i * 65);

    out = transform(text, text_n, &funcs, &sites, &changed);
    if (marker) {
        marked = add_build_marker(out.p, out.n, &marker_added);
        free(out.p);
        out = marked;
        changed = changed || marker_added;
    }
    for (i = 0; i < nnames; i++)
        function_hash(out.p, out.n, names[i], after + i * 65);

    if (changed || !is_dir(dst)) {
        /* A regular file is not a directory; use the file existence check
         * that the Python pass expresses as dst.exists(). */
        FILE *probe = fopen(dst, "rb");
        if (probe) fclose(probe);
        if (changed || !probe) {
            mkdir_parent(dst);
            if (write_all(dst, out.p ? out.p : "", out.n) != 0) {
                fprintf(stderr, PASS ": cannot write %s\n", dst);
                free(text);
                free(funcs.v);
                free(sites.v);
                free(names);
                free(before);
                free(after);
                free(out.p);
                return 2;
            }
        }
    }

    marker_count = count_substr(out.p, out.n, MARKER);
    if (marker_count < (int)sites.n) {
        fprintf(stderr, PASS ": only %d marker(s) for %zu site(s)\n",
                marker_count, sites.n);
        free(text);
        free(funcs.v);
        free(sites.v);
        free(names);
        free(before);
        free(after);
        free(out.p);
        return 2;
    }
    if (write_report(json, src, (int)sites.n, names, before, after, nnames,
                     changed, expect_sites, marker, marker_count) != 0) {
        fprintf(stderr, PASS ": cannot write report %s\n", json);
        free(text);
        free(funcs.v);
        free(sites.v);
        free(names);
        free(before);
        free(after);
        free(out.p);
        return 2;
    }
    printf(PASS ": %s -> %s: %s (%zu site(s), %zu function(s))\n",
           src, dst, changed ? "applied" : "already applied (idempotent)",
           sites.n, nnames);
    free(text);
    free(funcs.v);
    free(sites.v);
    free(names);
    free(before);
    free(after);
    free(out.p);
    return 0;
}

static const char SELF_TEST_FIXTURE[] =
    "/* header */\n"
    "void L_6F2A950C(CPU *c)\n"
    "{\n"
    "    push32(c, 0x6F2A953Du); dispatch(c, 0x6F07C6DEu);\n"
    "    c->ebx = (uint32_t)(rd32((c->ebx + 0x000001ECu)));           /* 6F2A953D: mov ebx, dword ptr [ebx + 0x1ec] */\n"
    "    flags_logicz(c, c->ebx & c->ebx, 4);                         /* 6F2A9543: test ebx, ebx */\n"
    "    dispatch_jmp(c, 0x6F2A9550u); return;\n"
    "}\n"
    "\n"
    "void L_6F2A953D(CPU *c)\n"
    "{\n"
    "    c->ebx = (uint32_t)(rd32((c->ebx + 0x000001ECu)));           /* 6F2A953D: mov ebx, dword ptr [ebx + 0x1ec] */\n"
    "    flags_logicz(c, c->ebx & c->ebx, 4);                         /* 6F2A9543: test ebx, ebx */\n"
    "    dispatch_jmp(c, 0x6F2A9550u); return;\n"
    "}\n";

static int self_test(void)
{
    FuncVec funcs = find_funcs(SELF_TEST_FIXTURE, sizeof(SELF_TEST_FIXTURE) - 1);
    SiteVec sites = find_sites(SELF_TEST_FIXTURE, sizeof(SELF_TEST_FIXTURE) - 1,
                               &funcs);
    int changed;
    Buf out = transform(SELF_TEST_FIXTURE, sizeof(SELF_TEST_FIXTURE) - 1,
                        &funcs, &sites, &changed);
    if (!changed || count_substr(out.p, out.n, MARKER) != 2) {
        fputs(PASS " self-test: block not inserted at both sites\n", stderr);
        return 1;
    }
    {
        FuncVec f2 = find_funcs(out.p, out.n);
        SiteVec s2 = find_sites(out.p, out.n, &f2);
        int changed2;
        Buf again = transform(out.p, out.n, &f2, &s2, &changed2);
        int ok = !changed2 && again.n == out.n &&
                 memcmp(again.p, out.p, out.n) == 0;
        free(f2.v);
        free(s2.v);
        free(again.p);
        if (!ok) {
            fputs(PASS " self-test: not idempotent\n", stderr);
            return 1;
        }
    }
    if (!strstr(out.p, "0xFFFFFDE4u") || !strstr(out.p, "0xFFFFFEE8u") ||
        !strstr(out.p, "rd8(") || !strstr(out.p, "wr8(")) {
        fputs(PASS " self-test: malformed block\n", stderr);
        return 1;
    }
    {
        Buf n1 = normalize(SELF_TEST_FIXTURE, sizeof(SELF_TEST_FIXTURE) - 1);
        Buf n2 = normalize(out.p, out.n);
        int ok = n1.n != n2.n;
        free(n1.p);
        free(n2.p);
        if (!ok) {
            fputs(PASS " self-test: normalizer did not see a change\n", stderr);
            return 1;
        }
    }
    {
        char *old = xstrdup(out.p);
        char *rev = strstr(old, "rev 5");
        Buf updated;
        FuncVec f2;
        SiteVec s2;
        int changed2;
        if (!rev) {
            free(old);
            fputs(PASS " self-test: replacement fixture is unchanged\n", stderr);
            return 1;
        }
        memcpy(rev, "rev 2", 5);
        f2 = find_funcs(old, out.n);
        s2 = find_sites(old, out.n, &f2);
        updated = transform(old, out.n, &f2, &s2, &changed2);
        if (!changed2 || updated.n != out.n ||
            memcmp(updated.p, out.p, out.n) != 0) {
            free(updated.p);
            free(f2.v);
            free(s2.v);
            free(old);
            fputs(PASS " self-test: old block was not replaced\n", stderr);
            return 1;
        }
        free(updated.p);
        free(f2.v);
        free(s2.v);
        free(old);
    }
    {
        int added, added2;
        Buf marked = add_build_marker(out.p, out.n, &added);
        Buf marked2 = add_build_marker(marked.p, marked.n, &added2);
        int ok = added && strstr(marked.p, MARKER_SYMBOL) &&
                 !added2 && marked2.n == marked.n &&
                 memcmp(marked2.p, marked.p, marked.n) == 0;
        free(marked.p);
        free(marked2.p);
        if (!ok) {
            fputs(PASS " self-test: marker not idempotent\n", stderr);
            return 1;
        }
    }
    free(out.p);
    free(funcs.v);
    free(sites.v);
    puts(PASS " self-test: ok");
    return 0;
}

static void usage(void)
{
    fputs("usage: " PASS " --lift IN --out OUT [--json REPORT] "
          "[--expect-sites N] [--marker]\n"
          "       " PASS " --self-test\n", stderr);
}

int main(int argc, char **argv)
{
    const char *lift = NULL, *out = NULL, *json = NULL;
    long expect_sites = -1;
    int marker = 0, i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--self-test")) return self_test();
        else if (!strcmp(argv[i], "--lift") && i + 1 < argc) lift = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) json = argv[++i];
        else if (!strcmp(argv[i], "--expect-sites") && i + 1 < argc)
            expect_sites = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--marker")) marker = 1;
        else { usage(); return 2; }
    }
    if (!lift || !out) {
        usage();
        return 2;
    }
    {
        char *a = path_slash(lift);
        char *b = path_slash(out);
        int same = strcmp(a, b) == 0;
        free(a);
        free(b);
        if (same) {
            fputs(PASS ": --out must differ from --lift; replace the corpus "
                  "explicitly\n", stderr);
            return 2;
        }
    }
    return process_file(lift, out, json, expect_sites, marker);
}
