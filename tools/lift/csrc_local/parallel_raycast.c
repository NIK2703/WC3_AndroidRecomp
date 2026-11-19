/* parallel_raycast.c - rewrite the single L_6F749F90 call site.
 *
 * This pass runs after leaf_call, so it sees the final call form:
 *
 *   push32(c, 0x6F78D9F2u);
 *   WC3_DIRECT_CALL_CLEAN(L_6F749F90, c);
 *
 * It replaces only the macro with
 *
 *   wc3_parallel_raycast(c, L_6F749F90);
 *
 * and adds `#include "parallel_raycast.h"` to the translation unit that owns
 * the call.  The artificial return address push and the five stack arguments
 * stay untouched: the runtime wrapper consumes them exactly like the lifted
 * function's `ret 0x14`.
 *
 * Comments and string/char literals are blanked before searching, so a call
 * mentioned in a generated comment is not rewritten.  The pass is fail-closed:
 * if the expected number of sites is not found, it exits non-zero instead of
 * silently producing a corpus with the optimization missing or doubled.
 *
 * This pass also rewrites the two vertex loops and the stride-3 triangle loop.
 * Each rewrite adds its own include and entry guard; the generated function
 * body remains the single source of truth.  The optional `--vertex-counter`
 * mode inserts a debug-only dispatch-decision call at the top of the vertex
 * batch setup; release compiles the call out.
 *
 *   parallel_raycast --inputs FILE --out-dir DIR [--report JSON]
 *                    [--expect-sites N] [--vertex]
 *                    [--expect-vertex-sites N] [--triangle]
 *                    [--expect-triangle-sites N] [--vertex-counter]
 *                    [--expect-vertex-counter-sites N] [--quiet]
 *   parallel_raycast --self-test
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CALL_NEEDLE "WC3_DIRECT_CALL_CLEAN(L_6F749F90, c)"
#define CALL_REPLACEMENT "wc3_parallel_raycast(c, L_6F749F90)"
#define CPU_INCLUDE "#include \"cpu.h\""
#define DIRECT_INCLUDE "#include \"direct_call.h\""
#define RAY_INCLUDE "#include \"parallel_raycast.h\""
#define VERTEX_INCLUDE "#include \"parallel_vertex.h\""
#define TRIANGLE_INCLUDE "#include \"parallel_triangle.h\""

#define VERTEX_COPY_SIG "void L_6F1419D4(CPU *restrict c)"
#define VERTEX_XFORM_SIG "void L_6F1417F0(CPU *restrict c)"
#define VERTEX_GUARD_COPY \
    "\n    if (wc3_parallel_vertex_try(c, L_6F1419D4, WC3_VERTEX_COPY)) return;\n"
#define VERTEX_GUARD_XFORM \
    "\n    if (wc3_parallel_vertex_try(c, L_6F1417F0, WC3_VERTEX_TRANSFORM)) return;\n"
#define VERTEX_GLOBAL_FIRST 0x6FD53938u
#define VERTEX_GLOBAL_LAST  0x6FD53968u

/* L_6F125924 is entered as a tail jump from L_6F125810, so the guard goes
 * after the two entry XMM snapshots and before any register is touched.  The
 * function has an internal loop label after this point, so the sequential
 * back-edge cannot re-enter the wrapper. */
#define TRIANGLE_SIG "void L_6F125924(CPU *restrict c)"
#define TRIANGLE_ANCHOR "_wc3_xmm_local[1] = c->xmm[1];"
#define TRIANGLE_GUARD \
    "\n    if (wc3_parallel_triangle_try(c, L_6F125924)) return;\n"

/* L_6F141680 is the vertex batch setup.  The counter is inserted after `this`
 * and `dest` are loaded and before the first format test, so it sees the
 * branch decision without changing the branch or the CPU.  The inserted call
 * is inside a debug-only preprocessor block; release contains no call. */
#define DISPATCH_SIG "void L_6F141680(CPU *restrict c)"
#define DISPATCH_ANCHOR "c->ebx = rd32((c->ebp + 0x00000008u));"
#define DISPATCH_NOTE \
    "\n#if WC3_DEBUG && WC3_DEBUG_PERF\n" \
    "    wc3_vertex_dispatch_note(c);\n" \
    "#endif\n"

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

static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "parallel_raycast: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory");
    return q;
}

static void buf_reserve(Buf *b, size_t extra)
{
    size_t need = b->n + extra + 1u;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256u;
    while (cap < need) cap *= 2u;
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

static char *read_file(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *p;
    size_t n;
    if (!f) die("cannot open %s", path);
    if (fseek(f, 0, SEEK_END) != 0) die("cannot seek %s", path);
    sz = ftell(f);
    if (sz < 0) die("cannot tell %s", path);
    if (fseek(f, 0, SEEK_SET) != 0) die("cannot seek %s", path);
    n = (size_t)sz;
    p = (char *)xmalloc(n + 1u);
    if (n && fread(p, 1, n, f) != n) die("cannot read %s", path);
    if (fclose(f) != 0) die("cannot close %s", path);
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
    const char *p = path;
    const char *last = path;
    for (; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

static char mask_blank(char c)
{
    return (c == '\n' || c == '\r') ? c : ' ';
}

/* Blank comments and literals byte for byte, preserving newlines. */
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

static size_t find_sub(const char *hay, size_t n, size_t start,
                       const char *needle)
{
    size_t len = strlen(needle);
    size_t i;
    if (!len || start > n || len > n - start) return (size_t)-1;
    for (i = start; i + len <= n; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, len) == 0)
            return i;
    }
    return (size_t)-1;
}

static size_t count_sub(const char *hay, size_t n, const char *needle)
{
    size_t count = 0, pos = 0;
    for (;;) {
        pos = find_sub(hay, n, pos, needle);
        if (pos == (size_t)-1) return count;
        count++;
        pos += strlen(needle);
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse the address in the generated spelling GVA(0x6FD53938).  `i` points at
 * the 'G'.  On success `*end` points just past the closing ')'. */
static int parse_gva(const char *s, size_t n, size_t i, uint32_t *out,
                     size_t *end)
{
    if (i + 4u > n || memcmp(s + i, "GVA(", 4u) != 0) return 0;
    size_t k = i + 4u;
    if (k + 2u > n || s[k] != '0' || (s[k + 1u] != 'x' &&
                                       s[k + 1u] != 'X'))
        return 0;
    k += 2u;
    uint32_t v = 0;
    unsigned digits = 0;
    for (; k < n; k++) {
        int d = hex_value(s[k]);
        if (d < 0) break;
        if (digits >= 8u) return 0;
        v = (v << 4) | (uint32_t)d;
        digits++;
    }
    if (!digits || k >= n || s[k] != ')') return 0;
    *out = v;
    *end = k + 1u;
    return 1;
}

static int find_function_body(const char *masked, size_t n, const char *sig,
                              size_t *open_out, size_t *close_out)
{
    size_t count = count_sub(masked, n, sig);
    if (count == 0) return 0;
    if (count != 1) return -1;
    size_t pos = find_sub(masked, n, 0, sig);
    if (pos == (size_t)-1) return -1;
    size_t open = pos + strlen(sig);
    while (open < n && (masked[open] == ' ' || masked[open] == '\t' ||
                        masked[open] == '\r' || masked[open] == '\n'))
        open++;
    if (open >= n || masked[open] != '{') return -1;
    int depth = 0;
    for (size_t i = open; i < n; i++) {
        if (masked[i] == '{') depth++;
        else if (masked[i] == '}') {
            depth--;
            if (depth == 0) {
                *open_out = open;
                *close_out = i;
                return 1;
            }
        }
    }
    return -1;
}

/* Replace only GVA(0x6FD53938..0x6FD53968) inside one function body.  The
 * generated text uses these addresses for every shared vertex global; no
 * local stack access has this form. */
static void replace_vertex_globals(const char *s, const char *mask,
                                   size_t begin, size_t end, Buf *out,
                                   unsigned *replaced)
{
    size_t last = begin;
    size_t i = begin;
    while (i < end) {
        uint32_t addr = 0;
        size_t after = 0;
        if (mask[i] == 'G' && parse_gva(mask, end, i, &addr, &after) &&
            addr >= VERTEX_GLOBAL_FIRST && addr <= VERTEX_GLOBAL_LAST) {
            buf_add(out, s + last, i - last);
            char tmp[64];
            snprintf(tmp, sizeof tmp, "wc3_vertex_global(c, 0x%08Xu)", addr);
            buf_str(out, tmp);
            (*replaced)++;
            i = after;
            last = after;
            continue;
        }
        i++;
    }
    buf_add(out, s + last, end - last);
}

static int insert_include(const char *text, size_t n, const char *include,
                          Buf *out, char *err, size_t errn)
{
    if (find_sub(text, n, 0, include) != (size_t)-1) {
        buf_add(out, text, n);
        return 1;
    }
    size_t anchor = find_sub(text, n, 0, RAY_INCLUDE);
    if (anchor == (size_t)-1)
        anchor = find_sub(text, n, 0, DIRECT_INCLUDE);
    if (anchor == (size_t)-1)
        anchor = find_sub(text, n, 0, CPU_INCLUDE);
    if (anchor == (size_t)-1) {
        snprintf(err, errn, "no %s, %s or %s include to anchor %s",
                 DIRECT_INCLUDE, RAY_INCLUDE, CPU_INCLUDE, include);
        return 0;
    }
    size_t at = anchor;
    while (at < n && text[at] != '\n') at++;
    if (at < n) at++;
    buf_add(out, text, at);
    if (at > 0 && text[at - 1u] != '\n') buf_str(out, "\n");
    buf_str(out, include);
    buf_str(out, "\n");
    buf_add(out, text + at, n - at);
    return 1;
}

static int insert_vertex_include(const char *text, size_t n, Buf *out,
                                 char *err, size_t errn)
{
    return insert_include(text, n, VERTEX_INCLUDE, out, err, errn);
}

/* Rewrite the two vertex loop bodies and add their entry guards.  Files with
 * neither function are copied unchanged.  A file with one of the two is
 * rejected: silently losing half of the optimization is worse than a build
 * failure. */
static int rewrite_vertex_text(const char *text, size_t n, Buf *out,
                               size_t *sites_out, char *err, size_t errn)
{
    char *masked = (char *)xmalloc(n + 1u);
    size_t copies, transforms;
    Buf with_include = {0};
    Buf final = {0};

    *sites_out = 0;
    mask_code(text, n, masked);
    copies = count_sub(masked, n, VERTEX_COPY_SIG);
    transforms = count_sub(masked, n, VERTEX_XFORM_SIG);
    if (copies == 0 && transforms == 0) {
        free(masked);
        buf_add(out, text, n);
        return 1;
    }
    if (copies != 1 || transforms != 1) {
        snprintf(err, errn,
                 "expected one copy and one transform loop, found %zu/%zu",
                 copies, transforms);
        free(masked);
        return 0;
    }
    if (count_sub(masked, n, "wc3_parallel_vertex_try") != 0) {
        snprintf(err, errn, "vertex guard already present");
        free(masked);
        return 0;
    }
    free(masked);

    if (!insert_vertex_include(text, n, &with_include, err, errn)) return 0;

    masked = (char *)xmalloc(with_include.n + 1u);
    mask_code(with_include.p ? with_include.p : "", with_include.n, masked);
    size_t open1 = 0, close1 = 0, open2 = 0, close2 = 0;
    if (find_function_body(masked, with_include.n, VERTEX_COPY_SIG,
                           &open1, &close1) != 1 ||
        find_function_body(masked, with_include.n, VERTEX_XFORM_SIG,
                           &open2, &close2) != 1) {
        snprintf(err, errn, "cannot delimit both vertex loop bodies");
        free(masked);
        free(with_include.p);
        return 0;
    }

    if (open1 >= open2) {
        snprintf(err, errn, "unexpected vertex loop order");
        free(masked);
        free(with_include.p);
        return 0;
    }

    size_t pos = 0;
    unsigned replaced = 0;
    for (unsigned body = 0; body < 2u; body++) {
        size_t open = body ? open2 : open1;
        size_t close = body ? close2 : close1;
        const char *guard = body ? VERTEX_GUARD_XFORM : VERTEX_GUARD_COPY;
        /* The transform function has an internal back-edge to
         * L_6F1417F0:, so its guard must be before that label.  Otherwise a
         * sequential call would re-enter the wrapper from inside its own
         * loop.  The copy function has no such label; keep its XMM declaration
         * in front of the guard when the later xmm_local pass provided one. */
        size_t guard_at = open + 1u;
        if (body) {
            size_t label = find_sub(masked, close, open + 1u,
                                    "L_6F1417F0:");
            if (label == (size_t)-1) {
                snprintf(err, errn, "transform loop label missing");
                free(final.p);
                free(masked);
                free(with_include.p);
                return 0;
            }
            guard_at = label;
        } else {
            size_t decl = find_sub(masked, close, open + 1u, "XMM ");
            if (decl != (size_t)-1) {
                size_t q = decl;
                while (q < close && with_include.p[q] != '\n') q++;
                if (q < close) guard_at = q + 1u;
            }
        }

        buf_add(&final, with_include.p + pos, guard_at - pos);
        buf_str(&final, guard);
        unsigned before = replaced;
        replace_vertex_globals(with_include.p, masked, guard_at, close,
                               &final, &replaced);
        if (replaced == before) {
            snprintf(err, errn, "vertex body %u has no shared-global access",
                     body);
            free(final.p);
            free(masked);
            free(with_include.p);
            return 0;
        }
        buf_add(&final, with_include.p + close, 1);
        pos = close + 1u;
    }
    buf_add(&final, with_include.p + pos, with_include.n - pos);
    free(masked);
    free(with_include.p);

    *out = final;
    *sites_out = 2;
    return 1;
}

/* Add the entry guard to the stride-3 triangle loop.  Files without the
 * function are copied unchanged; a file with more than one body is rejected
 * so a duplicated definition cannot silently skip the optimization. */
static int rewrite_triangle_text(const char *text, size_t n, Buf *out,
                                 size_t *sites_out, char *err, size_t errn)
{
    char *masked;
    size_t functions;
    Buf with_include = {0};

    *sites_out = 0;
    masked = (char *)xmalloc(n + 1u);
    mask_code(text, n, masked);
    functions = count_sub(masked, n, TRIANGLE_SIG);
    if (functions == 0) {
        free(masked);
        buf_add(out, text, n);
        return 1;
    }
    if (functions != 1) {
        snprintf(err, errn, "expected one triangle loop, found %zu",
                 functions);
        free(masked);
        return 0;
    }
    if (count_sub(masked, n, "wc3_parallel_triangle_try") != 0) {
        snprintf(err, errn, "triangle guard already present");
        free(masked);
        return 0;
    }
    free(masked);

    if (!insert_include(text, n, TRIANGLE_INCLUDE, &with_include, err, errn))
        return 0;

    masked = (char *)xmalloc(with_include.n + 1u);
    mask_code(with_include.p ? with_include.p : "", with_include.n, masked);
    size_t open = 0, close = 0;
    if (find_function_body(masked, with_include.n, TRIANGLE_SIG, &open,
                           &close) != 1) {
        snprintf(err, errn, "cannot delimit the triangle loop body");
        free(masked);
        free(with_include.p);
        return 0;
    }

    /* The first `_wc3_xmm_local[1]` assignment is the function-entry
     * snapshot.  The loop and epilogue load it again later, so the first
     * occurrence inside the body is unambiguous. */
    size_t anchor = find_sub(masked, close, open + 1u, TRIANGLE_ANCHOR);
    if (anchor == (size_t)-1) {
        snprintf(err, errn, "triangle entry XMM anchor missing");
        free(masked);
        free(with_include.p);
        return 0;
    }
    size_t guard_at = anchor + strlen(TRIANGLE_ANCHOR);
    while (guard_at < close && masked[guard_at] != '\n') guard_at++;
    if (guard_at < close) guard_at++;

    Buf final = {0};
    buf_add(&final, with_include.p, guard_at);
    buf_str(&final, TRIANGLE_GUARD);
    buf_add(&final, with_include.p + guard_at, with_include.n - guard_at);
    free(masked);
    free(with_include.p);

    *out = final;
    *sites_out = 1;
    return 1;
}

/* Insert the debug dispatch counter at the top of L_6F141680.  Files without
 * the setup are copied unchanged.  A file with more than one setup or with an
 * existing counter is rejected: a duplicated counter would distort the
 * profile and a missing one would silently defeat the measurement. */
static int rewrite_dispatch_text(const char *text, size_t n, Buf *out,
                                 size_t *sites_out, char *err, size_t errn)
{
    char *masked;
    size_t functions;
    Buf with_include = {0};

    *sites_out = 0;
    masked = (char *)xmalloc(n + 1u);
    mask_code(text, n, masked);
    functions = count_sub(masked, n, DISPATCH_SIG);
    if (functions == 0) {
        free(masked);
        buf_add(out, text, n);
        return 1;
    }
    if (functions != 1) {
        snprintf(err, errn, "expected one vertex dispatch setup, found %zu",
                 functions);
        free(masked);
        return 0;
    }
    if (count_sub(masked, n, "wc3_vertex_dispatch_note") != 0) {
        snprintf(err, errn, "vertex dispatch counter already present");
        free(masked);
        return 0;
    }
    free(masked);

    if (!insert_vertex_include(text, n, &with_include, err, errn))
        return 0;

    masked = (char *)xmalloc(with_include.n + 1u);
    mask_code(with_include.p ? with_include.p : "", with_include.n, masked);
    size_t open = 0, close = 0;
    if (find_function_body(masked, with_include.n, DISPATCH_SIG, &open,
                           &close) != 1) {
        snprintf(err, errn, "cannot delimit the vertex dispatch setup");
        free(masked);
        free(with_include.p);
        return 0;
    }
    size_t anchor = find_sub(masked, close, open + 1u, DISPATCH_ANCHOR);
    if (anchor == (size_t)-1) {
        snprintf(err, errn, "vertex dispatch anchor missing");
        free(masked);
        free(with_include.p);
        return 0;
    }

    /* Keep the generated line and its trailing instruction comment intact;
     * insert on the following line.  If the function is a one-liner, insert
     * before the closing brace. */
    size_t at = anchor + strlen(DISPATCH_ANCHOR);
    while (at < close && masked[at] != '\n') at++;
    if (at < close) at++;

    Buf final = {0};
    buf_add(&final, with_include.p, at);
    buf_str(&final, DISPATCH_NOTE);
    buf_add(&final, with_include.p + at, with_include.n - at);
    free(masked);
    free(with_include.p);

    *out = final;
    *sites_out = 1;
    return 1;
}

/* Build the output text.  Returns 0 and a human-readable reason on failure. */
static int rewrite_text(const char *text, size_t n, Buf *out,
                        size_t *sites_out, char *err, size_t errn)
{
    char *masked = (char *)xmalloc(n + 1u);
    size_t first, count;
    size_t insert_at = (size_t)-1;
    int has_ray_include;
    Buf with_include = {0};

    *sites_out = 0;
    mask_code(text, n, masked);

    count = count_sub(masked, n, CALL_NEEDLE);
    if (count > 1) {
        snprintf(err, errn, "more than one call site in one file (%zu)", count);
        free(masked);
        return 0;
    }
    if (count == 0) {
        buf_add(out, text, n);
        free(masked);
        return 1;
    }
    first = find_sub(masked, n, 0, CALL_NEEDLE);
    if (first == (size_t)-1) {
        snprintf(err, errn, "internal: call site disappeared");
        free(masked);
        return 0;
    }

    /* Includes contain a quoted literal, so look for the directive in the
     * original text.  Generated chunks never mention an include inside a
     * string, and the call needle above was already masked. */
    has_ray_include = find_sub(text, n, 0, RAY_INCLUDE) != (size_t)-1;
    if (!has_ray_include) {
        size_t inc = find_sub(text, n, 0, DIRECT_INCLUDE);
        if (inc == (size_t)-1) {
            snprintf(err, errn, "no %s include to anchor %s",
                     DIRECT_INCLUDE, RAY_INCLUDE);
            free(masked);
            return 0;
        }
        insert_at = inc;
        while (insert_at < n && text[insert_at] != '\n') insert_at++;
        if (insert_at < n) insert_at++;  /* keep the newline before the insert */
    }

    /* Insert the include first, then replace the call.  The mask is rebuilt
     * on the intermediate text so the later search sees the same bytes the
     * compiler will see. */
    if (insert_at == (size_t)-1) {
        buf_add(&with_include, text, n);
    } else {
        buf_add(&with_include, text, insert_at);
        if (insert_at > 0 && text[insert_at - 1u] != '\n')
            buf_str(&with_include, "\n");
        buf_str(&with_include, RAY_INCLUDE "\n");
        buf_add(&with_include, text + insert_at, n - insert_at);
    }
    free(masked);

    masked = (char *)xmalloc(with_include.n + 1u);
    mask_code(with_include.p ? with_include.p : "", with_include.n, masked);
    first = find_sub(masked, with_include.n, 0, CALL_NEEDLE);
    count = count_sub(masked, with_include.n, CALL_NEEDLE);
    free(masked);
    if (count != 1 || first == (size_t)-1) {
        snprintf(err, errn, "internal: call site count changed to %zu", count);
        free(with_include.p);
        return 0;
    }

    buf_add(out, with_include.p, first);
    buf_str(out, CALL_REPLACEMENT);
    buf_add(out, with_include.p + first + strlen(CALL_NEEDLE),
            with_include.n - first - strlen(CALL_NEEDLE));
    free(with_include.p);
    *sites_out = 1;
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
                     p[n - 1u] == ' ' || p[n - 1u] == '\t'))
            p[--n] = 0;
        if (!n || p[0] == '#') continue;
        if (paths->n == paths->cap) {
            paths->cap = paths->cap ? paths->cap * 2u : 64u;
            paths->p = (char **)xrealloc(paths->p, paths->cap * sizeof *paths->p);
        }
        paths->p[paths->n] = (char *)xmalloc(n + 1u);
        memcpy(paths->p[paths->n], p, n + 1u);
        paths->n++;
    }
    if (fclose(f) != 0) die("cannot close %s", list);
}

static void write_report(const char *path, size_t inputs, size_t rewritten,
                         size_t sites, size_t expected, size_t vertex_sites,
                         size_t triangle_sites, size_t dispatch_sites)
{
    FILE *f;
    if (!path) return;
    f = fopen(path, "wb");
    if (!f) die("cannot create report %s", path);
    fprintf(f,
            "{\n"
            "  \"tool\": \"parallel_raycast\",\n"
            "  \"verdict\": \"ok\",\n"
            "  \"inputs\": %zu,\n"
            "  \"files_rewritten\": %zu,\n"
            "  \"sites_rewritten\": %zu,\n"
            "  \"expected_sites\": %zu,\n"
            "  \"vertex_functions\": %zu,\n"
            "  \"triangle_functions\": %zu,\n"
            "  \"vertex_dispatch_notes\": %zu\n"
            "}\n",
            inputs, rewritten, sites, expected, vertex_sites, triangle_sites,
            dispatch_sites);
    if (fclose(f) != 0) die("cannot close report %s", path);
}

static int self_test(void)
{
    const char *good =
        "#include \"cpu.h\"\n"
        "#include \"direct_call.h\"\n"
        "void f(CPU *c) {\n"
        "    /* WC3_DIRECT_CALL_CLEAN(L_6F749F90, c) in a comment */\n"
        "    const char *s = \"WC3_DIRECT_CALL_CLEAN(L_6F749F90, c)\";\n"
        "    push32(c, 0x6F78D9F2u);\n"
        "    WC3_DIRECT_CALL_CLEAN(L_6F749F90, c);\n"
        "}\n";
    Buf out = {0};
    char err[256];
    size_t sites = 0;

    if (!rewrite_text(good, strlen(good), &out, &sites, err, sizeof err)) {
        fprintf(stderr, "self-test good input failed: %s\n", err);
        return 1;
    }
    if (sites != 1) {
        fprintf(stderr, "self-test: sites=%zu\n", sites);
        return 1;
    }
    if (!strstr(out.p, RAY_INCLUDE) || !strstr(out.p, DIRECT_INCLUDE)) {
        fprintf(stderr, "self-test: include missing\n%s\n", out.p);
        return 1;
    }
    if (!strstr(out.p, CALL_REPLACEMENT ";")) {
        fprintf(stderr, "self-test: replacement missing\n%s\n", out.p);
        return 1;
    }
    {
        char *m = (char *)xmalloc(out.n + 1u);
        mask_code(out.p, out.n, m);
        if (count_sub(m, out.n, CALL_NEEDLE) != 0) {
            fprintf(stderr, "self-test: comment/string was rewritten\n");
            return 1;
        }
        if (count_sub(m, out.n, CALL_REPLACEMENT) != 1) {
            fprintf(stderr, "self-test: replacement count is not one\n");
            return 1;
        }
        free(m);
    }
    free(out.p);
    out.p = NULL;
    out.n = 0;
    out.cap = 0;

    {
        const char *twice =
            "#include \"direct_call.h\"\n"
            "void f(CPU *c) { WC3_DIRECT_CALL_CLEAN(L_6F749F90, c); }\n"
            "void g(CPU *c) { WC3_DIRECT_CALL_CLEAN(L_6F749F90, c); }\n";
        Buf bad = {0};
        if (rewrite_text(twice, strlen(twice), &bad, &sites, err, sizeof err)) {
            fprintf(stderr, "self-test: duplicate was accepted\n");
            return 1;
        }
        free(bad.p);
    }

    {
        const char *none = "#include \"direct_call.h\"\nvoid f(CPU *c) {}\n";
        Buf copy = {0};
        sites = 99;
        if (!rewrite_text(none, strlen(none), &copy, &sites, err, sizeof err)) {
            fprintf(stderr, "self-test: no-site file rejected: %s\n", err);
            return 1;
        }
        if (sites != 0 || copy.n != strlen(none) ||
            memcmp(copy.p, none, copy.n) != 0) {
            fprintf(stderr, "self-test: no-site file was changed\n");
            return 1;
        }
        free(copy.p);
    }

    {
        const char *vertex =
            "#include \"cpu.h\"\n"
            "#include \"direct_call.h\"\n"
            "void L_6F1419D4(CPU *restrict c)\n"
            "{\n"
            "    XMM _wc3_xmm_local[8];\n"
            "    c->eax = rd32((GVA(0x6FD53938)));\n"
            "    wr32((GVA(0x6FD53944)), c->eax);\n"
            "    /* GVA(0x6FD5394C) stays a comment */\n"
            "}\n"
            "void L_6F1417F0(CPU *restrict c)\n"
            "{\n"
            "    XMM _wc3_xmm_local[8];\n"
            "L_6F1417F0:\n"
            "    c->eax = rd32((GVA(0x6FD53968)));\n"
            "}\n";
        Buf vout = {0};
        size_t vsites = 0;
        if (!rewrite_vertex_text(vertex, strlen(vertex), &vout, &vsites,
                                 err, sizeof err)) {
            fprintf(stderr, "self-test vertex input failed: %s\n", err);
            return 1;
        }
        if (vsites != 2 || !strstr(vout.p, VERTEX_INCLUDE) ||
            !strstr(vout.p, VERTEX_GUARD_COPY) ||
            !strstr(vout.p, VERTEX_GUARD_XFORM) ||
            !strstr(vout.p, "wc3_vertex_global(c, 0x6FD53938u)") ||
            !strstr(vout.p, "wc3_vertex_global(c, 0x6FD53944u)") ||
            !strstr(vout.p, "wc3_vertex_global(c, 0x6FD53968u)") ||
            strstr(vout.p, "wc3_vertex_global(c, 0x6FD5394Cu)")) {
            fprintf(stderr, "self-test vertex replacement missing\n%s\n",
                    vout.p);
            return 1;
        }
        free(vout.p);
    }

    {
        const char *none = "#include \"cpu.h\"\nvoid f(CPU *c) {}\n";
        Buf vcopy = {0};
        size_t vsites = 99;
        if (!rewrite_vertex_text(none, strlen(none), &vcopy, &vsites,
                                 err, sizeof err)) {
            fprintf(stderr, "self-test vertex no-site rejected: %s\n", err);
            return 1;
        }
        if (vsites != 0 || vcopy.n != strlen(none) ||
            memcmp(vcopy.p, none, vcopy.n) != 0) {
            fprintf(stderr, "self-test vertex no-site was changed\n");
            return 1;
        }
        free(vcopy.p);
    }

    {
        const char *triangle =
            "#include \"cpu.h\"\n"
            "#include \"direct_call.h\"\n"
            "void L_6F125924(CPU *restrict c)\n"
            "{\n"
            "    XMM _wc3_xmm_local[8];\n"
            "    _wc3_xmm_local[0] = c->xmm[0];\n"
            "    _wc3_xmm_local[1] = c->xmm[1];\n"
            "    c->ecx = rd32((c->ebp + 0x14u));\n"
            "}\n";
        Buf tout = {0};
        size_t tsites = 0;
        if (!rewrite_triangle_text(triangle, strlen(triangle), &tout, &tsites,
                                   err, sizeof err)) {
            fprintf(stderr, "self-test triangle input failed: %s\n", err);
            return 1;
        }
        if (tsites != 1 || !strstr(tout.p, TRIANGLE_INCLUDE) ||
            !strstr(tout.p, TRIANGLE_GUARD)) {
            fprintf(stderr, "self-test triangle guard missing\n%s\n", tout.p);
            return 1;
        }
        if (!strstr(tout.p, "_wc3_xmm_local[1] = c->xmm[1];\n"
                             "\n    if (wc3_parallel_triangle_try")) {
            fprintf(stderr, "self-test triangle guard placement wrong\n%s\n",
                    tout.p);
            return 1;
        }
        free(tout.p);
    }

    {
        const char *none = "#include \"cpu.h\"\nvoid f(CPU *c) {}\n";
        Buf tcopy = {0};
        size_t tsites = 99;
        if (!rewrite_triangle_text(none, strlen(none), &tcopy, &tsites,
                                   err, sizeof err)) {
            fprintf(stderr, "self-test triangle no-site rejected: %s\n", err);
            return 1;
        }
        if (tsites != 0 || tcopy.n != strlen(none) ||
            memcmp(tcopy.p, none, tcopy.n) != 0) {
            fprintf(stderr, "self-test triangle no-site was changed\n");
            return 1;
        }
        free(tcopy.p);
    }

    {
        const char *twice =
            "#include \"cpu.h\"\n"
            "void L_6F125924(CPU *restrict c) {}\n"
            "void L_6F125924(CPU *restrict c) {}\n";
        Buf bad = {0};
        size_t tsites = 0;
        if (rewrite_triangle_text(twice, strlen(twice), &bad, &tsites,
                                  err, sizeof err)) {
            fprintf(stderr, "self-test triangle duplicate was accepted\n");
            return 1;
        }
        free(bad.p);
    }

    {
        const char *dispatch =
            "#include \"cpu.h\"\n"
            "void L_6F141680(CPU *restrict c)\n"
            "{\n"
            "    c->edx = rd32((c->ecx + 0x00000090u));\n"
            "    wr32((c->ebp + 0xFFFFFFFCu), c->ecx);\n"
            "    push32(c, c->ebx);\n"
            "    c->ebx = rd32((c->ebp + 0x00000008u)); /* 6F141690 */\n"
            "    if (c->zf) return;\n"
            "}\n";
        Buf dout = {0};
        size_t dsites = 0;
        if (!rewrite_dispatch_text(dispatch, strlen(dispatch), &dout,
                                   &dsites, err, sizeof err)) {
            fprintf(stderr, "self-test dispatch input failed: %s\n", err);
            return 1;
        }
        if (dsites != 1 || !strstr(dout.p, VERTEX_INCLUDE) ||
            !strstr(dout.p, "wc3_vertex_dispatch_note(c);")) {
            fprintf(stderr, "self-test dispatch note missing\n%s\n", dout.p);
            return 1;
        }
        {
            const char *note =
                strstr(dout.p, "wc3_vertex_dispatch_note(c);");
            const char *branch = strstr(dout.p, "if (c->zf)");
            if (!note || !branch || note > branch) {
                fprintf(stderr,
                        "self-test dispatch note is outside the entry\n%s\n",
                        dout.p);
                return 1;
            }
        }
        {
            Buf dup = {0};
            size_t dsites2 = 0;
            if (rewrite_dispatch_text(dout.p, dout.n, &dup, &dsites2,
                                      err, sizeof err)) {
                fprintf(stderr,
                        "self-test dispatch duplicate was accepted\n");
                return 1;
            }
            free(dup.p);
        }
        free(dout.p);
    }

    {
        const char *none = "#include \"cpu.h\"\nvoid f(CPU *c) {}\n";
        Buf copy = {0};
        size_t dsites = 99;
        if (!rewrite_dispatch_text(none, strlen(none), &copy, &dsites,
                                   err, sizeof err)) {
            fprintf(stderr, "self-test dispatch no-site rejected: %s\n",
                    err);
            return 1;
        }
        if (dsites != 0 || copy.n != strlen(none) ||
            memcmp(copy.p, none, copy.n) != 0) {
            fprintf(stderr, "self-test dispatch no-site was changed\n");
            return 1;
        }
        free(copy.p);
    }

    printf("parallel_raycast self-test: ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *inputs = NULL;
    const char *out_dir = NULL;
    const char *report = NULL;
    size_t expected = 1;
    size_t expected_vertex = 2;
    size_t expected_triangle = 1;
    size_t expected_dispatch = 1;
    int quiet = 0;
    int vertex = 0;
    int triangle = 0;
    int vertex_counter = 0;
    PathVec paths = {0};
    size_t files_rewritten = 0;
    size_t sites = 0;
    size_t vertex_sites = 0;
    size_t triangle_sites = 0;
    size_t dispatch_sites = 0;
    size_t i;

    if (argc == 2 && !strcmp(argv[1], "--self-test")) return self_test();

    for (i = 1; i < (size_t)argc; i++) {
        if (!strcmp(argv[i], "--inputs") && i + 1 < (size_t)argc) {
            inputs = argv[++i];
        } else if (!strcmp(argv[i], "--out-dir") && i + 1 < (size_t)argc) {
            out_dir = argv[++i];
        } else if (!strcmp(argv[i], "--report") && i + 1 < (size_t)argc) {
            report = argv[++i];
        } else if (!strcmp(argv[i], "--expect-sites") && i + 1 < (size_t)argc) {
            char *end = NULL;
            unsigned long v = strtoul(argv[++i], &end, 0);
            if (!end || *end) die("bad --expect-sites value: %s", argv[i]);
            expected = (size_t)v;
        } else if (!strcmp(argv[i], "--expect-vertex-sites") &&
                   i + 1 < (size_t)argc) {
            char *end = NULL;
            unsigned long v = strtoul(argv[++i], &end, 0);
            if (!end || *end) die("bad --expect-vertex-sites value: %s",
                                  argv[i]);
            expected_vertex = (size_t)v;
        } else if (!strcmp(argv[i], "--expect-triangle-sites") &&
                   i + 1 < (size_t)argc) {
            char *end = NULL;
            unsigned long v = strtoul(argv[++i], &end, 0);
            if (!end || *end) die("bad --expect-triangle-sites value: %s",
                                  argv[i]);
            expected_triangle = (size_t)v;
        } else if (!strcmp(argv[i], "--expect-vertex-counter-sites") &&
                   i + 1 < (size_t)argc) {
            char *end = NULL;
            unsigned long v = strtoul(argv[++i], &end, 0);
            if (!end || *end)
                die("bad --expect-vertex-counter-sites value: %s", argv[i]);
            expected_dispatch = (size_t)v;
        } else if (!strcmp(argv[i], "--vertex")) {
            vertex = 1;
        } else if (!strcmp(argv[i], "--triangle")) {
            triangle = 1;
        } else if (!strcmp(argv[i], "--vertex-counter")) {
            vertex_counter = 1;
        } else if (!strcmp(argv[i], "--quiet")) {
            quiet = 1;
        } else {
            die("unknown argument: %s", argv[i]);
        }
    }
    if (!inputs || !out_dir) die("--inputs and --out-dir are required");

    read_paths(inputs, &paths);
    if (!paths.n) die("no input files in %s", inputs);

    for (i = 0; i < paths.n; i++) {
        const char *path = paths.p[i];
        size_t n, file_sites = 0, file_vertex_sites = 0;
        size_t file_triangle_sites = 0;
        size_t file_dispatch_sites = 0;
        char *text = read_file(path, &n);
        Buf ray = {0};
        Buf out = {0};
        char err[256];
        char out_path[8192];

        if (!rewrite_text(text, n, &ray, &file_sites, err, sizeof err))
            die("%s: %s", path, err);

        if (vertex) {
            if (!rewrite_vertex_text(ray.p ? ray.p : "", ray.n, &out,
                                     &file_vertex_sites, err, sizeof err))
                die("%s: %s", path, err);
            free(ray.p);
        } else {
            out = ray;
        }

        if (triangle) {
            Buf tout = {0};
            if (!rewrite_triangle_text(out.p ? out.p : "", out.n, &tout,
                                       &file_triangle_sites, err, sizeof err))
                die("%s: %s", path, err);
            free(out.p);
            out = tout;
        }

        if (vertex_counter) {
            Buf dout = {0};
            if (!rewrite_dispatch_text(out.p ? out.p : "", out.n, &dout,
                                       &file_dispatch_sites, err, sizeof err))
                die("%s: %s", path, err);
            free(out.p);
            out = dout;
        }

        snprintf(out_path, sizeof out_path, "%s/%s", out_dir, path_base(path));
        write_file(out_path, out.p ? out.p : "", out.n);
        if (file_sites || file_vertex_sites || file_triangle_sites ||
            file_dispatch_sites)
            files_rewritten++;
        sites += file_sites;
        vertex_sites += file_vertex_sites;
        triangle_sites += file_triangle_sites;
        dispatch_sites += file_dispatch_sites;

        free(text);
        free(out.p);
    }

    if (sites != expected) {
        die("rewrote %zu sites, expected %zu", sites, expected);
    }
    if (vertex && vertex_sites != expected_vertex) {
        die("rewrote %zu vertex functions, expected %zu",
            vertex_sites, expected_vertex);
    }
    if (triangle && triangle_sites != expected_triangle) {
        die("rewrote %zu triangle functions, expected %zu",
            triangle_sites, expected_triangle);
    }
    if (vertex_counter && dispatch_sites != expected_dispatch) {
        die("inserted %zu dispatch counters, expected %zu",
            dispatch_sites, expected_dispatch);
    }

    write_report(report, paths.n, files_rewritten, sites, expected,
                 vertex_sites, triangle_sites, dispatch_sites);
    if (!quiet) {
        printf("parallel_raycast: %zu inputs, %zu files, %zu/%zu sites, "
               "%zu/%zu vertex functions, %zu/%zu triangle functions, "
               "%zu/%zu dispatch counters\n",
               paths.n, files_rewritten, sites, expected,
               vertex_sites, vertex ? expected_vertex : 0,
               triangle_sites, triangle ? expected_triangle : 0,
               dispatch_sites, vertex_counter ? expected_dispatch : 0);
    }

    for (i = 0; i < paths.n; i++) free(paths.p[i]);
    free(paths.p);
    return 0;
}
