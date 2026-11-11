/* return_address_elide.c - P0-6 return-address-store elimination.
 *
 * P0-6: replace a guest return-address store before a direct call with a bare
 * stack-slot adjustment, but only when the callee summary proves that the
 * target never reads the entry [esp] slot.  The analysis is deliberately
 * textual and conservative; see the Python reference for the safety boundary.
 *
 *   return_address_elide --lift IN --out OUT [--json REPORT] [--quiet]
 *   return_address_elide --self-test
 */
#include <ctype.h>
#include <limits.h>
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

#define HLE_LO 0x7F000000u
#define HLE_HI 0x7F100000u

typedef struct {
    char *p;
    size_t n;
    size_t cap;
} Buf;

typedef struct {
    char **v;
    size_t n;
    size_t cap;
} StrVec;

typedef struct {
    size_t start;
    size_t end;
    char va[9];
} FuncSpan;

typedef struct {
    char va[9];
    char *body;
    size_t body_len;
    int reads_return_address;
    StrVec tail_targets;
    int tail_unknown;
} FuncInfo;

typedef struct {
    FuncInfo *v;
    size_t n;
    size_t cap;
} InfoVec;

typedef struct {
    size_t start;
    size_t end;
    int has_off;
    long off;
} ReadSpan;

typedef struct {
    long *v;
    size_t n;
    size_t cap;
    int unknown;
} StateSet;

typedef struct {
    char va[9];
    char before_sha[65];
    char after_sha[65];
} Change;

typedef struct {
    char *file;
    char *output;
    size_t functions;
    size_t call_sites;
    size_t elided;
    size_t skipped_reads_ra;
    size_t skipped_unknown;
    size_t skipped_hle;
    size_t changed_functions;
    Change *changes;
    size_t nchanges;
    size_t capchanges;
    StrVec errors;
} Report;

typedef struct {
    size_t start;
    size_t end;
    size_t push_start;
    size_t call_start;
    char ret[9];
    char target[9];
} CallSite;

static void die(const char *msg)
{
    fprintf(stderr, "return_address_elide: %s\n", msg);
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

static char *xstrndup(const char *s, size_t n)
{
    char *p = (char *)xrealloc(NULL, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
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

static void buf_ch(Buf *b, char c)
{
    buf_add(b, &c, 1);
}

static char *buf_take(Buf *b)
{
    if (!b->p) return xstrdup("");
    return b->p;
}

static void strvec_push(StrVec *v, const char *s)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->v = (char **)xrealloc(v->v, v->cap * sizeof(*v->v));
    }
    v->v[v->n++] = xstrdup(s);
}

static void strvec_free(StrVec *v)
{
    size_t i;
    for (i = 0; i < v->n; i++) free(v->v[i]);
    free(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

static int is_word(int c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static int is_hex(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int is_space(int c)
{
    return isspace((unsigned char)c) != 0;
}

static size_t skip_ws(const char *s, size_t n, size_t p)
{
    while (p < n && is_space((unsigned char)s[p])) p++;
    return p;
}

static int starts_line(const char *s, size_t p)
{
    return p == 0 || s[p - 1] == '\n';
}

/* Returns the end of the function header (after the opening brace), or 0. */
static size_t function_header_at(const char *s, size_t n, size_t p,
                                 char va[9])
{
    size_t i = p;
    if (!starts_line(s, p)) return 0;
    if (i + 4 > n || memcmp(s + i, "void", 4) != 0) return 0;
    i += 4;
    while (i < n && is_space((unsigned char)s[i])) i++;
    if (i + 2 > n || s[i] != 'L' || s[i + 1] != '_') return 0;
    i += 2;
    if (i + 8 > n) return 0;
    {
        int k;
        for (k = 0; k < 8; k++) {
            if (!is_hex((unsigned char)s[i + k])) return 0;
            va[k] = (char)toupper((unsigned char)s[i + k]);
        }
        va[8] = 0;
    }
    i += 8;
    while (i < n && is_space((unsigned char)s[i])) i++;
    if (i >= n || s[i] != '(') return 0;
    i++;
    while (i < n && is_space((unsigned char)s[i])) i++;
    if (i + 3 > n || memcmp(s + i, "CPU", 3) != 0) return 0;
    i += 3;
    while (i < n && is_space((unsigned char)s[i])) i++;
    if (i >= n || s[i] != '*') return 0;
    i++;
    while (i < n && is_space((unsigned char)s[i])) i++;
    if (i >= n || s[i] != 'c') return 0;
    i++;
    while (i < n && is_space((unsigned char)s[i])) i++;
    if (i >= n || s[i] != ')') return 0;
    i++;
    while (i < n && is_space((unsigned char)s[i])) i++;
    if (i >= n || s[i] != '{') return 0;
    return i + 1;
}

static FuncSpan *find_functions(const char *s, size_t n, size_t *out_n)
{
    FuncSpan *v = NULL;
    size_t count = 0, cap = 0, i;
    for (i = 0; i < n; i++) {
        char va[9];
        size_t end;
        if (s[i] != 'v') continue;
        end = function_header_at(s, n, i, va);
        if (!end) continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 64;
            v = (FuncSpan *)xrealloc(v, cap * sizeof(*v));
        }
        if (count > 0) v[count - 1].end = i;
        v[count].start = end;
        v[count].end = n;
        memcpy(v[count].va, va, 9);
        count++;
        i = end - 1;
    }
    *out_n = count;
    return v;
}

static char *sub_block_comments(const char *s, size_t n)
{
    Buf b = {0};
    size_t i = 0;
    while (i < n) {
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            size_t j = i + 2;
            size_t end;
            while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
            end = (j + 1 < n) ? j + 2 : n;
            buf_ch(&b, ' ');
            i = end;
        } else {
            buf_ch(&b, s[i]);
            i++;
        }
    }
    return buf_take(&b);
}

static char *sub_line_comments(const char *s, size_t n)
{
    Buf b = {0};
    size_t i = 0;
    while (i < n) {
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
            buf_ch(&b, ' ');
            break;
        }
        buf_ch(&b, s[i]);
        i++;
    }
    return buf_take(&b);
}

static char *sub_strings(const char *s, size_t n)
{
    Buf b = {0};
    size_t i = 0;
    while (i < n) {
        if (s[i] == '"') {
            size_t j = i + 1;
            while (j < n) {
                if (s[j] == '\\') {
                    j += 2;
                    continue;
                }
                if (s[j] == '"') {
                    j++;
                    break;
                }
                j++;
            }
            if (j <= n && j > i + 1) {
                buf_str(&b, "\"\"");
                i = j;
                continue;
            }
        }
        buf_ch(&b, s[i]);
        i++;
    }
    return buf_take(&b);
}

static char *clean_line(const char *s, size_t n)
{
    char *a = sub_block_comments(s, n);
    char *b = sub_line_comments(a, strlen(a));
    char *c = sub_strings(b, strlen(b));
    free(a);
    free(b);
    return c;
}

static char *collapse_ws(const char *s, size_t n)
{
    Buf b = {0};
    size_t i = 0;
    int have = 0;
    while (i < n) {
        if (is_space((unsigned char)s[i])) {
            i++;
            continue;
        }
        if (have) buf_ch(&b, ' ');
        while (i < n && !is_space((unsigned char)s[i])) {
            buf_ch(&b, s[i]);
            i++;
        }
        have = 1;
    }
    return buf_take(&b);
}

static char *normalize_body(const char *s, size_t n)
{
    char *a = sub_block_comments(s, n);
    char *b = sub_line_comments(a, strlen(a));
    char *c = sub_strings(b, strlen(b));
    char *out = collapse_ws(c, strlen(c));
    free(a);
    free(b);
    free(c);
    return out;
}

static void body_sha(const char *body, size_t n, char out[65])
{
    char *norm = normalize_body(body, n);
    unsigned char digest[32];
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, norm, strlen(norm));
    sha256_final(&ctx, digest);
    sha256_hex(digest, out);
    free(norm);
}

/* Parse a hexadecimal or decimal number with an optional u suffix. */
static int parse_number_at(const char *s, size_t n, size_t p, long *out,
                           size_t *end)
{
    size_t i = p;
    int base = 10;
    unsigned long v = 0;
    int any = 0;
    if (i + 1 < n && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        base = 16;
        i += 2;
        while (i < n && is_hex((unsigned char)s[i])) {
            int c = s[i];
            int d = (c >= '0' && c <= '9') ? c - '0'
                    : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                    : c - 'A' + 10;
            v = v * (unsigned long)base + (unsigned long)d;
            i++;
            any = 1;
        }
    } else {
        while (i < n && isdigit((unsigned char)s[i])) {
            v = v * 10u + (unsigned long)(s[i] - '0');
            i++;
            any = 1;
        }
    }
    if (!any) return 0;
    if (i < n && s[i] == 'u') i++;
    *out = (long)v;
    if (end) *end = i;
    return 1;
}

static int word_at(const char *s, size_t n, size_t p, const char *w,
                   size_t *end)
{
    size_t m = strlen(w);
    if (p + m > n) return 0;
    if (memcmp(s + p, w, m) != 0) return 0;
    if (p > 0 && is_word((unsigned char)s[p - 1])) return 0;
    if (p + m < n && is_word((unsigned char)s[p + m])) return 0;
    if (end) *end = p + m;
    return 1;
}

static int hex8_at(const char *s, size_t n, size_t p, char out[9])
{
    size_t i;
    if (p + 8 > n) return 0;
    for (i = 0; i < 8; i++) {
        if (!is_hex((unsigned char)s[p + i])) return 0;
        out[i] = (char)toupper((unsigned char)s[p + i]);
    }
    out[8] = 0;
    return 1;
}

static int hex_any_at(const char *s, size_t n, size_t p, char **out,
                      size_t *end)
{
    size_t i = p;
    while (i < n && is_hex((unsigned char)s[i])) i++;
    if (i == p) return 0;
    *out = xstrndup(s + p, i - p);
    if (end) *end = i;
    return 1;
}

/* ---- stack-read parser ------------------------------------------------ */

static int read_match_at(const char *s, size_t n, size_t p, const char *reg,
                         size_t *end, int *has_off, long *off)
{
    size_t i;
    if (p > 0 && is_word((unsigned char)s[p - 1])) return 0;
    if (p + 2 > n || s[p] != 'r' || s[p + 1] != 'd') return 0;
    i = p + 2;
    if (i + 2 <= n && s[i] == '1' && s[i + 1] == '6') i += 2;
    else if (i + 2 <= n && s[i] == '3' && s[i + 1] == '2') i += 2;
    else if (i + 2 <= n && s[i] == '6' && s[i + 1] == '4') i += 2;
    else if (i + 1 <= n && s[i] == '8') i += 1;
    else return 0;
    i = skip_ws(s, n, i);
    if (i >= n || s[i] != '(') return 0;
    i++;
    i = skip_ws(s, n, i);
    if (i < n && s[i] == '(') {
        i++;
        i = skip_ws(s, n, i);
    }
    if (i + 5 > n || memcmp(s + i, "c->", 3) != 0) return 0;
    if (i + 3 + strlen(reg) > n || memcmp(s + i + 3, reg, strlen(reg)) != 0)
        return 0;
    i += 3 + strlen(reg);
    *has_off = 0;
    *off = 0;
    {
        size_t q = skip_ws(s, n, i);
        if (q < n && s[q] == '+') {
            long v;
            size_t ne;
            q = skip_ws(s, n, q + 1);
            if (!parse_number_at(s, n, q, &v, &ne)) return 0;
            *has_off = 1;
            *off = v;
            i = ne;
        }
    }
    i = skip_ws(s, n, i);
    if (i < n && s[i] == ')') i++;
    i = skip_ws(s, n, i);
    if (i >= n || s[i] != ')') return 0;
    i++;
    *end = i;
    return 1;
}

static ReadSpan *collect_reads(const char *s, size_t n, const char *reg,
                               size_t *out_n)
{
    ReadSpan *v = NULL;
    size_t count = 0, cap = 0, p = 0;
    while (p < n) {
        size_t end;
        int has_off;
        long off;
        if (read_match_at(s, n, p, reg, &end, &has_off, &off)) {
            if (count == cap) {
                cap = cap ? cap * 2 : 8;
                v = (ReadSpan *)xrealloc(v, cap * sizeof(*v));
            }
            v[count].start = p;
            v[count].end = end;
            v[count].has_off = has_off;
            v[count].off = off;
            count++;
            p = end;
        } else {
            p++;
        }
    }
    *out_n = count;
    return v;
}

static char *sub_spans(const char *s, size_t n, const ReadSpan *v, size_t nv)
{
    Buf b = {0};
    size_t p = 0, i;
    for (i = 0; i < nv; i++) {
        if (v[i].start < p) continue;
        buf_add(&b, s + p, v[i].start - p);
        buf_ch(&b, ' ');
        p = v[i].end;
    }
    if (p < n) buf_add(&b, s + p, n - p);
    return buf_take(&b);
}

/* ---- small textual matchers ------------------------------------------ */

static int find_push32(const char *s, size_t n, size_t from, size_t *start,
                       size_t *end, size_t *arg_start, size_t *arg_end)
{
    size_t p = from;
    while (p + 6 <= n) {
        size_t i;
        if (memcmp(s + p, "push32", 6) != 0) {
            p++;
            continue;
        }
        i = skip_ws(s, n, p + 6);
        if (i >= n || s[i] != '(') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        if (i >= n || s[i] != 'c') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        if (i >= n || s[i] != ',') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        {
            size_t a = i;
            size_t q = i;
            while (q < n) {
                if (s[q] == ')') {
                    size_t z = skip_ws(s, n, q + 1);
                    if (z < n && s[z] == ';') {
                        *start = p;
                        *end = z + 1;
                        *arg_start = a;
                        *arg_end = q;
                        return 1;
                    }
                }
                q++;
            }
        }
        p++;
    }
    return 0;
}

static int find_pop32(const char *s, size_t n, size_t from, size_t *start,
                      size_t *end)
{
    size_t p = from;
    while (p < n) {
        size_t i, e;
        if (!word_at(s, n, p, "pop32", &e)) { p++; continue; }
        i = skip_ws(s, n, e);
        if (i >= n || s[i] != '(') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        if (i >= n || s[i] != 'c') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        if (i >= n || s[i] != ')') { p++; continue; }
        *start = p;
        *end = i + 1;
        return 1;
    }
    return 0;
}

/* Find `c->esp <op> <number>`; op is "+=" or "-=". */
static int find_esp_op(const char *s, size_t n, const char *op,
                       size_t *start, long *value, size_t *end)
{
    size_t p = 0;
    size_t oplen = strlen(op);
    while (p + 5 <= n) {
        size_t i;
        if (memcmp(s + p, "c->esp", 6) != 0 ||
            (p > 0 && is_word((unsigned char)s[p - 1]))) {
            p++;
            continue;
        }
        i = skip_ws(s, n, p + 6);
        if (i + oplen > n || memcmp(s + i, op, oplen) != 0) {
            p++;
            continue;
        }
        i = skip_ws(s, n, i + oplen);
        if (parse_number_at(s, n, i, value, &i)) {
            *start = p;
            *end = i;
            return 1;
        }
        p++;
    }
    return 0;
}

/* Remove every `c->esp <op> ...;` statement, matching the Python re.sub. */
static char *remove_esp_op(const char *s, size_t n, const char *op)
{
    Buf b = {0};
    size_t p = 0;
    size_t oplen = strlen(op);
    while (p < n) {
        if (memcmp(s + p, "c->esp", 6) == 0 &&
            (p == 0 || !is_word((unsigned char)s[p - 1]))) {
            size_t i = skip_ws(s, n, p + 6);
            if (i + oplen <= n && memcmp(s + i, op, oplen) == 0) {
                size_t e = i + oplen;
                while (e < n && s[e] != ';') e++;
                if (e < n && s[e] == ';') e++;
                buf_ch(&b, ' ');
                p = e;
                continue;
            }
        }
        buf_ch(&b, s[p]);
        p++;
    }
    return buf_take(&b);
}

static int has_esp_assign(const char *s, size_t n)
{
    size_t p = 0;
    while (p + 5 <= n) {
        if (memcmp(s + p, "c->esp", 6) == 0 &&
            (p == 0 || !is_word((unsigned char)s[p - 1]))) {
            size_t i = skip_ws(s, n, p + 6);
            if (i < n && s[i] == '=' &&
                (i + 1 >= n || s[i + 1] != '=')) return 1;
        }
        p++;
    }
    return 0;
}

/* Remove every `c->esp = ...;` statement. */
static char *remove_esp_assign(const char *s, size_t n)
{
    Buf b = {0};
    size_t p = 0;
    while (p < n) {
        if (memcmp(s + p, "c->esp", 6) == 0 &&
            (p == 0 || !is_word((unsigned char)s[p - 1]))) {
            size_t i = skip_ws(s, n, p + 6);
            if (i < n && s[i] == '=' &&
                (i + 1 >= n || s[i + 1] != '=')) {
                size_t q = i + 1;
                while (q < n && is_space((unsigned char)s[q])) q++;
                if (q < n && s[q] != ';') {
                    size_t e = q;
                    while (e < n && s[e] != ';') e++;
                    if (e < n && s[e] == ';') e++;
                    buf_ch(&b, ' ');
                    p = e;
                    continue;
                }
            }
        }
        buf_ch(&b, s[p]);
        p++;
    }
    return buf_take(&b);
}

static int has_ebp_assign(const char *s, size_t n)
{
    size_t p = 0;
    while (p + 5 <= n) {
        if (memcmp(s + p, "c->ebp", 6) == 0 &&
            (p == 0 || !is_word((unsigned char)s[p - 1]))) {
            size_t i = skip_ws(s, n, p + 6);
            if (i < n && s[i] == '=' &&
                (i + 1 >= n || s[i + 1] != '=')) return 1;
        }
        p++;
    }
    return 0;
}

/* `c->ebp = (uint32_t)(pop32(...))` frame restore. */
static int has_pop32_ebp_assign(const char *s, size_t n)
{
    size_t p = 0;
    while (p + 5 <= n) {
        size_t i;
        if (memcmp(s + p, "c->ebp", 6) != 0 ||
            (p > 0 && is_word((unsigned char)s[p - 1]))) {
            p++;
            continue;
        }
        i = skip_ws(s, n, p + 6);
        if (i >= n || s[i] != '=') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        if (i < n && s[i] == '(') { i = skip_ws(s, n, i + 1); }
        if (!word_at(s, n, i, "uint32_t", &i)) { p++; continue; }
        i = skip_ws(s, n, i);
        if (i < n && s[i] == ')') { i = skip_ws(s, n, i + 1); }
        if (i < n && s[i] == '(') { i = skip_ws(s, n, i + 1); }
        if (word_at(s, n, i, "pop32", &i)) {
            i = skip_ws(s, n, i);
            if (i < n && s[i] == '(') return 1;
        }
        p++;
    }
    return 0;
}

/* `c->ebp = (uint32_t)(c->esp)` or `c->ebp = c->esp`. */
static int has_prologue(const char *s, size_t n)
{
    size_t p = 0;
    while (p + 5 <= n) {
        size_t i;
        if (memcmp(s + p, "c->ebp", 6) != 0 ||
            (p > 0 && is_word((unsigned char)s[p - 1]))) {
            p++;
            continue;
        }
        i = skip_ws(s, n, p + 6);
        if (i >= n || s[i] != '=') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        if (i < n && s[i] == '(') { i = skip_ws(s, n, i + 1); }
        if (word_at(s, n, i, "uint32_t", &i)) {
            i = skip_ws(s, n, i);
            if (i < n && s[i] == ')') { i = skip_ws(s, n, i + 1); }
            if (i < n && s[i] == '(') { i = skip_ws(s, n, i + 1); }
        }
        if (i + 6 <= n && memcmp(s + i, "c->esp", 6) == 0) {
            size_t e = i + 6;
            if (e >= n || !is_word((unsigned char)s[e])) return 1;
        }
        p++;
    }
    return 0;
}

/* `c->esp = (uint32_t)(c->ebp)` */
static int has_esp_assign_ebp(const char *s, size_t n)
{
    size_t p = 0;
    while (p + 5 <= n) {
        size_t i;
        if (memcmp(s + p, "c->esp", 6) != 0 ||
            (p > 0 && is_word((unsigned char)s[p - 1]))) {
            p++;
            continue;
        }
        i = skip_ws(s, n, p + 6);
        if (i >= n || s[i] != '=') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        if (i < n && s[i] == '(') { i = skip_ws(s, n, i + 1); }
        if (!word_at(s, n, i, "uint32_t", &i)) { p++; continue; }
        i = skip_ws(s, n, i);
        if (i < n && s[i] == ')') { i = skip_ws(s, n, i + 1); }
        if (i < n && s[i] == '(') { i = skip_ws(s, n, i + 1); }
        if (i + 6 <= n && memcmp(s + i, "c->ebp", 6) == 0) {
            size_t e = i + 6;
            if (e >= n || !is_word((unsigned char)s[e])) return 1;
        }
        p++;
    }
    return 0;
}

/* `c->esp = ... flags_add(c, c->esp, N) ...;` */
static int find_flags_add(const char *s, size_t n, long *value)
{
    size_t p = 0;
    while (p < n) {
        size_t e;
        if (!word_at(s, n, p, "flags_add", &e)) { p++; continue; }
        {
            size_t i = skip_ws(s, n, e);
            size_t stmt = p;
            size_t q;
            if (i >= n || s[i] != '(') { p++; continue; }
            i = skip_ws(s, n, i + 1);
            if (i >= n || s[i] != 'c') { p++; continue; }
            i = skip_ws(s, n, i + 1);
            if (i >= n || s[i] != ',') { p++; continue; }
            i = skip_ws(s, n, i + 1);
            if (i + 6 > n || memcmp(s + i, "c->esp", 6) != 0) {
                p++;
                continue;
            }
            i = skip_ws(s, n, i + 6);
            if (i >= n || s[i] != ',') { p++; continue; }
            i = skip_ws(s, n, i + 1);
            if (!parse_number_at(s, n, i, value, &i)) { p++; continue; }
            i = skip_ws(s, n, i);
            if (i >= n || s[i] != ')') { p++; continue; }
            while (stmt > 0 && s[stmt - 1] != ';') stmt--;
            q = stmt;
            while (q < p) {
                if (memcmp(s + q, "c->esp", 6) == 0 &&
                    (q == 0 || !is_word((unsigned char)s[q - 1]))) {
                    size_t z = skip_ws(s, n, q + 6);
                    if (z < n && s[z] == '=') return 1;
                }
                q++;
            }
        }
        p++;
    }
    return 0;
}

static char *replace_literal(const char *s, size_t n, const char *lit)
{
    Buf b = {0};
    size_t p = 0;
    size_t m = strlen(lit);
    while (p < n) {
        if (p + m <= n && memcmp(s + p, lit, m) == 0) {
            buf_ch(&b, ' ');
            p += m;
        } else {
            buf_ch(&b, s[p]);
            p++;
        }
    }
    return buf_take(&b);
}

static int has_call(const char *s, size_t n)
{
    size_t p = 0;
    static const char *const names[] = {
        "dispatch", "dispatch_jmp", "WC3_DIRECT_CALL", "WC3_DIRECT_TAIL",
        "wc3_legacy_hle_call", "hle_call", NULL
    };
    while (p < n) {
        size_t start, end, i;
        int hit = 0, k;
        if (!(isalpha((unsigned char)s[p]) || s[p] == '_')) { p++; continue; }
        start = p;
        p++;
        while (p < n && is_word((unsigned char)s[p])) p++;
        end = p;
        for (k = 0; names[k]; k++) {
            if (strlen(names[k]) == end - start &&
                memcmp(s + start, names[k], end - start) == 0) {
                hit = 1;
                break;
            }
        }
        if (!hit && end - start >= 4 &&
            memcmp(s + start, "hle_", 4) == 0) hit = 1;
        if (!hit) continue;
        i = skip_ws(s, n, end);
        if (i < n && s[i] == '(') return 1;
    }
    return 0;
}

static int has_dispatch_jmp(const char *s, size_t n)
{
    size_t p = 0;
    while (p < n) {
        size_t e;
        if (!word_at(s, n, p, "dispatch_jmp", &e)) { p++; continue; }
        e = skip_ws(s, n, e);
        if (e < n && s[e] == '(') return 1;
        p++;
    }
    return 0;
}

static int collect_direct_tails(const char *s, size_t n, StrVec *out)
{
    size_t p = 0;
    size_t before = out->n;
    while (p < n) {
        size_t e, i;
        char va[9];
        if (!word_at(s, n, p, "WC3_DIRECT_TAIL", &e)) { p++; continue; }
        i = skip_ws(s, n, e);
        if (i >= n || s[i] != '(') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        if (i + 2 > n || s[i] != 'L' || s[i + 1] != '_') { p++; continue; }
        i += 2;
        if (!hex8_at(s, n, i, va)) { p++; continue; }
        i += 8;
        i = skip_ws(s, n, i);
        if (i >= n || s[i] != ',') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        if (i >= n || s[i] != 'c') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        if (i >= n || s[i] != ')') { p++; continue; }
        strvec_push(out, va);
        p = i + 1;
    }
    return (int)(out->n - before);
}

static int collect_dispatch_tails(const char *s, size_t n, StrVec *out)
{
    size_t p = 0;
    size_t before = out->n;
    while (p < n) {
        size_t e, i;
        char va[9];
        if (!word_at(s, n, p, "dispatch_jmp", &e)) { p++; continue; }
        i = skip_ws(s, n, e);
        if (i >= n || s[i] != '(') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        if (i >= n || s[i] != 'c') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        if (i >= n || s[i] != ',') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        if (i + 2 > n || s[i] != '0' || s[i + 1] != 'x') { p++; continue; }
        i += 2;
        if (!hex8_at(s, n, i, va)) { p++; continue; }
        i += 8;
        if (i < n && s[i] == 'u') i++;
        i = skip_ws(s, n, i);
        if (i >= n || s[i] != ')') { p++; continue; }
        strvec_push(out, va);
        p = i + 1;
    }
    return (int)(out->n - before);
}

static int find_goto(const char *s, size_t n, char **out)
{
    size_t p = 0;
    while (p < n) {
        size_t e, i;
        char *va = NULL;
        if (!word_at(s, n, p, "goto", &e)) { p++; continue; }
        i = skip_ws(s, n, e);
        if (i + 2 > n || s[i] != 'L' || s[i + 1] != '_') { p++; continue; }
        i += 2;
        if (!hex_any_at(s, n, i, &va, &i)) { p++; continue; }
        {
            char *full = (char *)xrealloc(NULL, strlen(va) + 3);
            full[0] = 'L';
            full[1] = '_';
            strcpy(full + 2, va);
            free(va);
            va = full;
        }
        i = skip_ws(s, n, i);
        if (i < n && s[i] == ';') {
            *out = va;
            return 1;
        }
        free(va);
        p++;
    }
    return 0;
}

static int has_return(const char *s, size_t n)
{
    size_t p = 0;
    while (p < n) {
        size_t e;
        if (!word_at(s, n, p, "return", &e)) { p++; continue; }
        e = skip_ws(s, n, e);
        if (e < n && s[e] == ';') return 1;
        p++;
    }
    return 0;
}

static int starts_with_if(const char *s)
{
    size_t p = 0, n = strlen(s), e;
    p = skip_ws(s, n, p);
    return word_at(s, n, p, "if", &e);
}

static int starts_with_loop(const char *s)
{
    size_t p = 0, n = strlen(s), e;
    p = skip_ws(s, n, p);
    return word_at(s, n, p, "while", &e) ||
           word_at(s, n, p, "for", &e) ||
           word_at(s, n, p, "switch", &e) ||
           word_at(s, n, p, "do", &e);
}

static char *trim_dup(const char *s)
{
    size_t n = strlen(s), a = 0, b = n;
    while (a < b && is_space((unsigned char)s[a])) a++;
    while (b > a && is_space((unsigned char)s[b - 1])) b--;
    return xstrndup(s + a, b - a);
}

/* ---- function analysis ------------------------------------------------ */

static int state_add(StateSet *s, long v)
{
    size_t i;
    if (v == LONG_MIN) {
        if (s->unknown) return 0;
        s->unknown = 1;
        return 1;
    }
    for (i = 0; i < s->n; i++)
        if (s->v[i] == v) return 0;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = (long *)xrealloc(s->v, s->cap * sizeof(*s->v));
    }
    s->v[s->n++] = v;
    return 1;
}

static StateSet state_clone(const StateSet *s)
{
    StateSet c;
    memset(&c, 0, sizeof(c));
    if (s->n) {
        c.v = (long *)xrealloc(NULL, s->n * sizeof(*c.v));
        memcpy(c.v, s->v, s->n * sizeof(*c.v));
        c.n = c.cap = s->n;
    }
    c.unknown = s->unknown;
    return c;
}

static void state_free(StateSet *s)
{
    free(s->v);
    memset(s, 0, sizeof(*s));
}

static char **split_lines(const char *s, size_t n, size_t *out_n)
{
    char **v = NULL;
    size_t count = 0, cap = 0, i = 0;
    while (i < n) {
        size_t start = i;
        while (i < n && s[i] != '\n' && s[i] != '\r' &&
               s[i] != '\v' && s[i] != '\f') i++;
        if (count == cap) {
            cap = cap ? cap * 2 : 64;
            v = (char **)xrealloc(v, cap * sizeof(*v));
        }
        v[count++] = xstrndup(s + start, i - start);
        if (i < n) {
            if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') i += 2;
            else i++;
        }
    }
    *out_n = count;
    return v;
}

typedef struct {
    char *name;
    long line;
} Label;

static long find_label(Label *v, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (strcmp(v[i].name, name) == 0) return v[i].line;
    return -1;
}

static long match_end(size_t i, size_t n, const int *depth_before,
                      const int *depth_after, char **lines)
{
    int base = depth_before[i];
    size_t j;
    if (depth_after[i] == base) return (long)i;
    for (j = i + 1; j < n; j++) {
        if (depth_after[j] == base && strchr(lines[j], '}') != NULL)
            return (long)j;
    }
    return (long)n - 1;
}

static void analyze_body(const char *body, size_t body_len,
                         const char *va, int *out_unsafe,
                         StrVec *out_tails, int *out_tail_unknown)
{
    size_t n = 0, i;
    char **raw = split_lines(body, body_len, &n);
    char **lines = (char **)xrealloc(NULL, (n ? n : 1) * sizeof(*lines));
    Label *labels = NULL;
    size_t nlabels = 0, caplabels = 0;
    int *depth_before = NULL, *depth_after = NULL;
    long *ebp_writes = NULL;
    size_t nebp = 0, capebp = 0;
    long prologue = -1;
    long frame_rel = LONG_MIN;
    StateSet *states = NULL;
    long *work = NULL;
    size_t nwork = 0;
    int unsafe = 0, tail_unknown = 0;
    StrVec tails = {0};
    int depth = 0;

    for (i = 0; i < n; i++) {
        lines[i] = clean_line(raw[i], strlen(raw[i]));
        free(raw[i]);
    }
    free(raw);
    if (n == 0) {
        free(lines);
        *out_unsafe = 0;
        *out_tails = tails;
        *out_tail_unknown = 0;
        return;
    }

    for (i = 0; i < n; i++) {
        size_t p = skip_ws(lines[i], strlen(lines[i]), 0);
        size_t m = strlen(lines[i]);
        if (p + 2 <= m && lines[i][p] == 'L' && lines[i][p + 1] == '_') {
            size_t q = p + 2;
            while (q < m && is_hex((unsigned char)lines[i][q])) q++;
            if (q > p + 2) {
                size_t z = skip_ws(lines[i], m, q);
                if (z < m && lines[i][z] == ':') {
                    if (nlabels == caplabels) {
                        caplabels = caplabels ? caplabels * 2 : 16;
                        labels = (Label *)xrealloc(
                            labels, caplabels * sizeof(*labels));
                    }
                    labels[nlabels].name = xstrndup(lines[i] + p, q - p);
                    labels[nlabels].line = (long)i;
                    nlabels++;
                }
            }
        }
    }

    depth_before = (int *)xrealloc(NULL, n * sizeof(*depth_before));
    depth_after = (int *)xrealloc(NULL, n * sizeof(*depth_after));
    for (i = 0; i < n; i++) {
        size_t k;
        depth_before[i] = depth;
        for (k = 0; k < strlen(lines[i]); k++) {
            if (lines[i][k] == '{') depth++;
            else if (lines[i][k] == '}') depth--;
        }
        depth_after[i] = depth;
    }

    for (i = 0; i < n; i++) {
        if (has_ebp_assign(lines[i], strlen(lines[i]))) {
            if (nebp == capebp) {
                capebp = capebp ? capebp * 2 : 16;
                ebp_writes = (long *)xrealloc(
                    ebp_writes, capebp * sizeof(*ebp_writes));
            }
            ebp_writes[nebp++] = (long)i;
        }
    }
    for (i = 0; i < n; i++) {
        if (has_prologue(lines[i], strlen(lines[i]))) {
            prologue = (long)i;
            break;
        }
    }

    if (prologue >= 0) {
        int others = 0;
        size_t k;
        for (k = 0; k < nebp; k++) {
            long w = ebp_writes[k];
            if (w != prologue &&
                !has_pop32_ebp_assign(lines[w], strlen(lines[w]))) {
                others = 1;
                break;
            }
        }
        if (!others) {
            long rel = 0;
            for (k = 0; k < (size_t)prologue; k++)
                if (strstr(lines[k], "push32") != NULL) rel -= 4;
            frame_rel = rel;
        }
    }

    states = (StateSet *)xrealloc(NULL, n * sizeof(*states));
    memset(states, 0, n * sizeof(*states));
    state_add(&states[0], 0);
    work = (long *)xrealloc(NULL, sizeof(*work));
    work[nwork++] = 0;

    {
        size_t analysis_steps = 0;
        while (nwork) {
        long li = work[--nwork];
        StateSet snap;
        if (++analysis_steps > 10000000) {
            fprintf(stderr, "return_address_elide: analysis did not converge "
                            "for L_%s (lines=%zu work=%zu)\n",
                    va, n, nwork);
            exit(3);
        }
        size_t si;
        if (li < 0 || (size_t)li >= n) continue;
        snap = state_clone(&states[li]);
        for (si = 0; si <= snap.n; si++) {
            long delta, incoming;
            const char *line;
            char *rest, *tmp;
            ReadSpan *reads;
            size_t nreads, r;
            size_t push_start, push_end, arg_start, arg_end;
            size_t op_start, op_end;
            long add;
            char *stripped;

            if (si == snap.n) {
                if (!snap.unknown) break;
                incoming = LONG_MIN;
            } else {
                incoming = snap.v[si];
            }
            delta = incoming;
            line = lines[li];

            reads = collect_reads(line, strlen(line), "ebp", &nreads);
            for (r = 0; r < nreads; r++) {
                long off = reads[r].has_off ? reads[r].off : 0;
                if (!reads[r].has_off && reads[r].off != 0) { /* unreachable */ }
                if (frame_rel == LONG_MIN || frame_rel + off == 0)
                    unsafe = 1;
            }
            free(reads);

            reads = collect_reads(line, strlen(line), "esp", &nreads);
            for (r = 0; r < nreads; r++) {
                long off = reads[r].has_off ? reads[r].off : 0;
                if (delta == LONG_MIN || delta + off == 0)
                    unsafe = 1;
            }
            rest = sub_spans(line, strlen(line), reads, nreads);
            free(reads);
            {
                ReadSpan *ebps = collect_reads(rest, strlen(rest), "ebp",
                                               &nreads);
                tmp = sub_spans(rest, strlen(rest), ebps, nreads);
                free(ebps);
                free(rest);
                rest = tmp;
            }

            if (find_push32(rest, strlen(rest), 0, &push_start, &push_end,
                            &arg_start, &arg_end)) {
                char *arg = xstrndup(rest + arg_start, arg_end - arg_start);
                if (strstr(arg, "c->esp") != NULL) unsafe = 1;
                if (strstr(arg, "c->ebp") != NULL && has_call(line, strlen(line)))
                    unsafe = 1;
                free(arg);
                if (delta != LONG_MIN) delta -= 4;
                {
                    Buf b = {0};
                    buf_add(&b, rest, push_start);
                    buf_ch(&b, ' ');
                    buf_add(&b, rest + push_end, strlen(rest) - push_end);
                    free(rest);
                    rest = buf_take(&b);
                }
            }

            if (find_pop32(rest, strlen(rest), 0, &push_start, &push_end)) {
                if (delta == LONG_MIN || delta == 0) unsafe = 1;
                else delta += 4;
            }

            if (find_esp_op(rest, strlen(rest), "+=", &op_start, &add,
                            &op_end)) {
                delta = (delta == LONG_MIN) ? LONG_MIN : delta + add;
                tmp = remove_esp_op(rest, strlen(rest), "+=");
                free(rest);
                rest = tmp;
            }
            if (find_esp_op(rest, strlen(rest), "-=", &op_start, &add,
                            &op_end)) {
                delta = (delta == LONG_MIN) ? LONG_MIN : delta - add;
                tmp = remove_esp_op(rest, strlen(rest), "-=");
                free(rest);
                rest = tmp;
            }

            if (find_flags_add(rest, strlen(rest), &add)) {
                delta = (delta == LONG_MIN) ? LONG_MIN : delta + add;
                tmp = remove_esp_assign(rest, strlen(rest));
                free(rest);
                rest = tmp;
            } else if (has_esp_assign_ebp(rest, strlen(rest))) {
                delta = frame_rel;
                tmp = remove_esp_assign(rest, strlen(rest));
                free(rest);
                rest = tmp;
            } else if (has_esp_assign(rest, strlen(rest))) {
                delta = LONG_MIN;
                tmp = remove_esp_assign(rest, strlen(rest));
                free(rest);
                rest = tmp;
            }

            if (li == prologue) {
                tmp = replace_literal(rest, strlen(rest), "c->esp");
                free(rest);
                rest = tmp;
            }
            if (strstr(rest, "c->esp") != NULL) unsafe = 1;

            if (has_call(line, strlen(line))) delta = LONG_MIN;

            if (has_ebp_assign(rest, strlen(rest))) {
                if (li == prologue) {
                    tmp = replace_literal(rest, strlen(rest), "c->ebp");
                    free(rest);
                    rest = tmp;
                } else if (has_pop32_ebp_assign(rest, strlen(rest))) {
                    frame_rel = LONG_MIN;
                    tmp = replace_literal(rest, strlen(rest), "c->ebp");
                    free(rest);
                    rest = tmp;
                } else {
                    unsafe = 1;
                }
            }
            if (strstr(rest, "c->ebp") != NULL) unsafe = 1;

            collect_direct_tails(line, strlen(line), &tails);
            {
                size_t before = tails.n;
                collect_dispatch_tails(line, strlen(line), &tails);
                for (r = before; r < tails.n; r++) {
                    unsigned long t = strtoul(tails.v[r], NULL, 16);
                    if (t >= HLE_LO && t < HLE_HI) {
                        tail_unknown = 1;
                        free(tails.v[r]);
                        memmove(&tails.v[r], &tails.v[r + 1],
                                (tails.n - r - 1) * sizeof(*tails.v));
                        tails.n--;
                        r--;
                    }
                }
            }
            if (has_dispatch_jmp(line, strlen(line))) {
                size_t nd = 0, ndir = 0;
                StrVec probe = {0};
                nd = (size_t)collect_dispatch_tails(line, strlen(line), &probe);
                ndir = (size_t)collect_direct_tails(line, strlen(line), &probe);
                strvec_free(&probe);
                if (nd == 0 && ndir == 0) tail_unknown = 1;
            }

            stripped = trim_dup(line);
            if (has_return(line, strlen(line)) && !starts_with_if(stripped)) {
                free(stripped);
                free(rest);
                continue;
            }

            {
                char *target = NULL;
                if (find_goto(line, strlen(line), &target)) {
                    long tl = find_label(labels, nlabels, target);
                    if (tl >= 0 && state_add(&states[tl], delta)) {
                        work = (long *)xrealloc(
                            work, (nwork + 1) * sizeof(*work));
                        work[nwork++] = tl;
                    }
                    free(target);
                    if (!starts_with_if(stripped)) {
                        free(stripped);
                        free(rest);
                        continue;
                    }
                    if ((size_t)li + 1 < n &&
                        state_add(&states[li + 1], incoming)) {
                        work = (long *)xrealloc(
                            work, (nwork + 1) * sizeof(*work));
                        work[nwork++] = li + 1;
                    }
                    free(stripped);
                    free(rest);
                    continue;
                }
            }

            if (starts_with_if(stripped)) {
                if (strchr(stripped, '{') != NULL) {
                    long end = match_end((size_t)li, n, depth_before,
                                         depth_after, lines);
                    if (end + 1 < (long)n &&
                        state_add(&states[end + 1], incoming)) {
                        work = (long *)xrealloc(
                            work, (nwork + 1) * sizeof(*work));
                        work[nwork++] = end + 1;
                    }
                } else {
                    unsafe = 1;
                }
            }

            if (starts_with_loop(stripped)) {
                if (strstr(stripped, "push32") != NULL ||
                    strstr(stripped, "pop32") != NULL ||
                    strstr(stripped, "c->esp") != NULL) unsafe = 1;
                if ((size_t)li + 1 < n &&
                    state_add(&states[li + 1], incoming)) {
                    work = (long *)xrealloc(work,
                                            (nwork + 1) * sizeof(*work));
                    work[nwork++] = li + 1;
                }
            }

            if ((size_t)li + 1 < n &&
                state_add(&states[li + 1], delta)) {
                work = (long *)xrealloc(work, (nwork + 1) * sizeof(*work));
                work[nwork++] = li + 1;
            }
            free(stripped);
            free(rest);
        }
        state_free(&snap);
        }
    }

    for (i = 0; i < n; i++) free(lines[i]);
    free(lines);
    for (i = 0; i < nlabels; i++) free(labels[i].name);
    free(labels);
    free(depth_before);
    free(depth_after);
    free(ebp_writes);
    for (i = 0; i < n; i++) state_free(&states[i]);
    free(states);
    free(work);

    *out_unsafe = unsafe;
    *out_tails = tails;
    *out_tail_unknown = tail_unknown;
}

/* ---- function index and closure -------------------------------------- */

static void info_free(FuncInfo *f)
{
    free(f->body);
    strvec_free(&f->tail_targets);
    memset(f, 0, sizeof(*f));
}

static void infovec_free(InfoVec *v)
{
    size_t i;
    for (i = 0; i < v->n; i++) info_free(&v->v[i]);
    free(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

static int cmp_info(const void *a, const void *b)
{
    return strcmp(((const FuncInfo *)a)->va, ((const FuncInfo *)b)->va);
}

static FuncInfo *info_lookup(InfoVec *v, const char *va)
{
    size_t lo = 0, hi = v->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(va, v->v[mid].va);
        if (c == 0) return &v->v[mid];
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

static InfoVec index_functions(const char *text, size_t n)
{
    InfoVec v = {0};
    size_t nspans = 0, i;
    FuncSpan *spans = find_functions(text, n, &nspans);
    v.v = (FuncInfo *)xrealloc(NULL, (nspans ? nspans : 1) *
                               sizeof(*v.v));
    v.cap = nspans ? nspans : 1;
    for (i = 0; i < nspans; i++) {
        FuncInfo *f = &v.v[v.n];
        size_t len = spans[i].end - spans[i].start;
        memset(f, 0, sizeof(*f));
        memcpy(f->va, spans[i].va, 9);
        f->body = xstrndup(text + spans[i].start, len);
        f->body_len = len;
        analyze_body(f->body, len, f->va, &f->reads_return_address,
                     &f->tail_targets, &f->tail_unknown);
        v.n++;
    }
    free(spans);
    qsort(v.v, v.n, sizeof(*v.v), cmp_info);
    /* Python's dict keeps the last body for a duplicate VA.  The corpus has
     * unique VAs; keep the first sorted entry if that ever changes. */
    {
        size_t w = 0;
        for (i = 0; i < v.n; i++) {
            if (w > 0 && strcmp(v.v[w - 1].va, v.v[i].va) == 0) {
                info_free(&v.v[i]);
                continue;
            }
            if (w != i) v.v[w] = v.v[i];
            w++;
        }
        v.n = w;
    }
    return v;
}

static void compute_tail_closures(InfoVec *infos)
{
    int changed = 1;
    while (changed) {
        size_t i;
        changed = 0;
        for (i = 0; i < infos->n; i++) {
            FuncInfo *f = &infos->v[i];
            size_t k;
            if (f->reads_return_address) continue;
            if (f->tail_unknown) {
                f->reads_return_address = 1;
                changed = 1;
                continue;
            }
            for (k = 0; k < f->tail_targets.n; k++) {
                FuncInfo *t = info_lookup(infos, f->tail_targets.v[k]);
                if (!t || t->reads_return_address) {
                    f->reads_return_address = 1;
                    changed = 1;
                    break;
                }
            }
        }
    }
}

/* ---- code mask -------------------------------------------------------- */

static size_t find_sub(const char *s, size_t n, size_t from,
                       const char *needle)
{
    size_t m = strlen(needle), p;
    if (m == 0) return from;
    for (p = from; p + m <= n; p++)
        if (memcmp(s + p, needle, m) == 0) return p;
    return (size_t)-1;
}

static unsigned char *code_mask(const char *text, size_t n)
{
    unsigned char *mask = (unsigned char *)calloc(n ? n : 1, 1);
    size_t nspans = 0, i;
    FuncSpan *spans;
    if (!mask) die_oom();
    spans = find_functions(text, n, &nspans);
    for (i = 0; i < nspans; i++) {
        if (spans[i].end > spans[i].start)
            memset(mask + spans[i].start, 1, spans[i].end - spans[i].start);
    }
    free(spans);
    i = 0;
    while (i < n) {
        if (!mask[i]) { i++; continue; }
        if (text[i] == '/' && i + 1 < n && text[i + 1] == '*') {
            size_t e = find_sub(text, n, i + 2, "*/");
            e = (e == (size_t)-1) ? n : e + 2;
            memset(mask + i, 0, e - i);
            i = e;
        } else if (text[i] == '/' && i + 1 < n && text[i + 1] == '/') {
            size_t e = find_sub(text, n, i + 2, "\n");
            e = (e == (size_t)-1) ? n : e;
            memset(mask + i, 0, e - i);
            i = e;
        } else if (text[i] == '"' || text[i] == '\'') {
            char quote = text[i];
            size_t j = i + 1;
            while (j < n) {
                if (text[j] == '\\') { j += 2; continue; }
                if (text[j] == quote) { j++; break; }
                j++;
            }
            if (j > n) j = n;
            memset(mask + i, 0, j - i);
            i = j;
        } else {
            i++;
        }
    }
    return mask;
}

static int is_code_match(size_t start, size_t end, const char *text,
                         const unsigned char *mask)
{
    size_t i;
    for (i = start; i < end; i++)
        if (!mask[i] && !is_space((unsigned char)text[i])) return 0;
    return 1;
}

/* ---- call-site transform --------------------------------------------- */

static int call_site_at(const char *s, size_t n, size_t from, CallSite *out)
{
    size_t p = from;
    while (p + 6 <= n) {
        size_t i;
        char ret[9];
        if (memcmp(s + p, "push32", 6) != 0) { p++; continue; }
        i = skip_ws(s, n, p + 6);
        if (i >= n || s[i] != '(') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        if (i >= n || s[i] != 'c') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        if (i >= n || s[i] != ',') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        if (i + 2 > n || s[i] != '0' || s[i + 1] != 'x') { p++; continue; }
        i += 2;
        if (!hex8_at(s, n, i, ret)) { p++; continue; }
        i += 8;
        if (i < n && s[i] == 'u') i++;
        i = skip_ws(s, n, i);
        if (i >= n || s[i] != ')') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        if (i >= n || s[i] != ';') { p++; continue; }
        i = skip_ws(s, n, i + 1);
        out->start = p;
        out->push_start = p;
        out->call_start = i;
        memcpy(out->ret, ret, 9);
        if (word_at(s, n, i, "WC3_DIRECT_CALL", &i)) {
            char target[9];
            i = skip_ws(s, n, i);
            if (i >= n || s[i] != '(') { p++; continue; }
            i = skip_ws(s, n, i + 1);
            if (i + 2 > n || s[i] != 'L' || s[i + 1] != '_') { p++; continue; }
            i += 2;
            if (!hex8_at(s, n, i, target)) { p++; continue; }
            i += 8;
            i = skip_ws(s, n, i);
            if (i >= n || s[i] != ',') { p++; continue; }
            i = skip_ws(s, n, i + 1);
            if (i >= n || s[i] != 'c') { p++; continue; }
            i = skip_ws(s, n, i + 1);
            if (i >= n || s[i] != ')') { p++; continue; }
            out->end = i + 1;
            memcpy(out->target, target, 9);
            return 1;
        }
        if (word_at(s, n, i, "dispatch", &i)) {
            char target[9];
            i = skip_ws(s, n, i);
            if (i >= n || s[i] != '(') { p++; continue; }
            i = skip_ws(s, n, i + 1);
            if (i >= n || s[i] != 'c') { p++; continue; }
            i = skip_ws(s, n, i + 1);
            if (i >= n || s[i] != ',') { p++; continue; }
            i = skip_ws(s, n, i + 1);
            if (i + 2 > n || s[i] != '0' || s[i + 1] != 'x') {
                p++;
                continue;
            }
            i += 2;
            if (!hex8_at(s, n, i, target)) { p++; continue; }
            i += 8;
            if (i < n && s[i] == 'u') i++;
            i = skip_ws(s, n, i);
            if (i >= n || s[i] != ')') { p++; continue; }
            out->end = i + 1;
            memcpy(out->target, target, 9);
            return 1;
        }
        p++;
    }
    return 0;
}

static char *rewrite_push_prefix(const char *prefix, size_t n)
{
    size_t start, end, a, b;
    Buf out = {0};
    if (!find_push32(prefix, n, 0, &start, &end, &a, &b))
        return NULL;
    buf_add(&out, prefix, start);
    buf_str(&out, "c->esp -= 4;");
    buf_add(&out, prefix + end, n - end);
    return buf_take(&out);
}

typedef struct {
    char va[9];
    const char *body;
    size_t len;
} BodyEntry;

static int cmp_body_entry(const void *a, const void *b)
{
    return strcmp(((const BodyEntry *)a)->va, ((const BodyEntry *)b)->va);
}

static BodyEntry *build_bodies(const char *text, size_t n, size_t *out_n)
{
    size_t nspans = 0, i, w = 0;
    FuncSpan *spans = find_functions(text, n, &nspans);
    BodyEntry *v = (BodyEntry *)xrealloc(NULL, (nspans ? nspans : 1) *
                                        sizeof(*v));
    for (i = 0; i < nspans; i++) {
        memcpy(v[w].va, spans[i].va, 9);
        v[w].body = text + spans[i].start;
        v[w].len = spans[i].end - spans[i].start;
        w++;
    }
    free(spans);
    qsort(v, w, sizeof(*v), cmp_body_entry);
    /* Keep the last duplicate (Python dict assignment order). */
    if (w > 1) {
        size_t r = 0;
        for (i = 0; i < w; i++) {
            if (r > 0 && strcmp(v[r - 1].va, v[i].va) == 0)
                v[r - 1] = v[i];
            else
                v[r++] = v[i];
        }
        w = r;
    }
    *out_n = w;
    return v;
}

static void change_push(Report *r, const char *va, const char *before,
                        const char *after)
{
    if (r->nchanges == r->capchanges) {
        r->capchanges = r->capchanges ? r->capchanges * 2 : 8;
        r->changes = (Change *)xrealloc(
            r->changes, r->capchanges * sizeof(*r->changes));
    }
    memcpy(r->changes[r->nchanges].va, va, 9);
    memcpy(r->changes[r->nchanges].before_sha, before, 65);
    memcpy(r->changes[r->nchanges].after_sha, after, 65);
    r->nchanges++;
}

static char *transform_text(const char *text, size_t n, InfoVec *infos,
                            Report *report)
{
    unsigned char *mask = code_mask(text, n);
    Buf out = {0};
    size_t last = 0, p = 0;
    CallSite site;
    while (call_site_at(text, n, p, &site)) {
        report->call_sites++;
        if (!is_code_match(site.start, site.end, text, mask)) {
            p = site.end;
            continue;
        }
        {
            unsigned long tva = strtoul(site.target, NULL, 16);
            FuncInfo *info;
            if (tva >= HLE_LO && tva < HLE_HI) {
                report->skipped_hle++;
                p = site.end;
                continue;
            }
            info = info_lookup(infos, site.target);
            if (!info) {
                report->skipped_unknown++;
                p = site.end;
                continue;
            }
            if (info->reads_return_address) {
                report->skipped_reads_ra++;
                p = site.end;
                continue;
            }
            if (strtoul(site.ret, NULL, 16) == 0) {
                report->skipped_unknown++;
                p = site.end;
                continue;
            }
            {
                size_t prefix_len = site.call_start - site.push_start;
                char *prefix = xstrndup(text + site.push_start, prefix_len);
                char *rewritten = rewrite_push_prefix(prefix, prefix_len);
                free(prefix);
                if (!rewritten) {
                    char msg[128];
                    snprintf(msg, sizeof(msg),
                             "could not rewrite push at %zu", site.start);
                    strvec_push(&report->errors, msg);
                    p = site.end;
                    continue;
                }
                buf_add(&out, text + last, site.push_start - last);
                buf_str(&out, rewritten);
                free(rewritten);
                last = site.call_start;
                report->elided++;
            }
        }
        p = site.end;
    }
    buf_add(&out, text + last, n - last);
    free(mask);

    {
        size_t nb = 0, na = 0, ib = 0, ia = 0;
        BodyEntry *before = build_bodies(text, n, &nb);
        BodyEntry *after = build_bodies(out.p, out.n, &na);
        while (ib < nb || ia < na) {
            int cmp;
            if (ib >= nb) cmp = 1;
            else if (ia >= na) cmp = -1;
            else cmp = strcmp(before[ib].va, after[ia].va);
            if (cmp < 0) {
                char msg[64];
                snprintf(msg, sizeof(msg), "function set changed for L_%s",
                         before[ib].va);
                strvec_push(&report->errors, msg);
                ib++;
            } else if (cmp > 0) {
                char msg[64];
                snprintf(msg, sizeof(msg), "function set changed for L_%s",
                         after[ia].va);
                strvec_push(&report->errors, msg);
                ia++;
            } else {
                char *nb_norm = normalize_body(before[ib].body,
                                               before[ib].len);
                char *na_norm = normalize_body(after[ia].body,
                                               after[ia].len);
                if (strcmp(nb_norm, na_norm) != 0) {
                    char *sb = NULL, *sa = NULL;
                    char bsha[65], asha[65];
                    /* structural_body: canonicalize the two allowed forms. */
                    {
                        /* inline structural canonicalization */
                        Buf bb = {0};
                        size_t q = 0;
                        while (q < strlen(nb_norm)) {
                            size_t ps, pe, pa, pb;
                            if (find_push32(nb_norm, strlen(nb_norm), q,
                                            &ps, &pe, &pa, &pb) &&
                                ps == q) {
                                buf_str(&bb, "SITE;");
                                q = pe;
                                continue;
                            }
                            if (memcmp(nb_norm + q, "c->esp", 6) == 0) {
                                size_t z = skip_ws(nb_norm, strlen(nb_norm),
                                                   q + 6);
                                if (z + 1 < strlen(nb_norm) &&
                                    nb_norm[z] == '-' && nb_norm[z + 1] == '=') {
                                    z = skip_ws(nb_norm, strlen(nb_norm), z + 2);
                                    if (z < strlen(nb_norm) && nb_norm[z] == '4') {
                                        size_t e = skip_ws(nb_norm,
                                                           strlen(nb_norm),
                                                           z + 1);
                                        if (e < strlen(nb_norm) &&
                                            nb_norm[e] == ';') {
                                            buf_str(&bb, "SITE;");
                                            q = e + 1;
                                            continue;
                                        }
                                    }
                                }
                            }
                            buf_ch(&bb, nb_norm[q]);
                            q++;
                        }
                        sb = buf_take(&bb);
                    }
                    {
                        Buf ab = {0};
                        size_t q = 0;
                        while (q < strlen(na_norm)) {
                            size_t ps, pe, pa, pb;
                            if (find_push32(na_norm, strlen(na_norm), q,
                                            &ps, &pe, &pa, &pb) &&
                                ps == q) {
                                buf_str(&ab, "SITE;");
                                q = pe;
                                continue;
                            }
                            if (memcmp(na_norm + q, "c->esp", 6) == 0) {
                                size_t z = skip_ws(na_norm, strlen(na_norm),
                                                   q + 6);
                                if (z + 1 < strlen(na_norm) &&
                                    na_norm[z] == '-' && na_norm[z + 1] == '=') {
                                    z = skip_ws(na_norm, strlen(na_norm), z + 2);
                                    if (z < strlen(na_norm) && na_norm[z] == '4') {
                                        size_t e = skip_ws(na_norm,
                                                           strlen(na_norm),
                                                           z + 1);
                                        if (e < strlen(na_norm) &&
                                            na_norm[e] == ';') {
                                            buf_str(&ab, "SITE;");
                                            q = e + 1;
                                            continue;
                                        }
                                    }
                                }
                            }
                            buf_ch(&ab, na_norm[q]);
                            q++;
                        }
                        sa = buf_take(&ab);
                    }
                    if (strcmp(sb, sa) != 0) {
                        char msg[64];
                        snprintf(msg, sizeof(msg),
                                 "unexpected body change in L_%s",
                                 before[ib].va);
                        strvec_push(&report->errors, msg);
                    } else {
                        body_sha(before[ib].body, before[ib].len, bsha);
                        body_sha(after[ia].body, after[ia].len, asha);
                        report->changed_functions++;
                        change_push(report, before[ib].va, bsha, asha);
                    }
                    free(sb);
                    free(sa);
                }
                free(nb_norm);
                free(na_norm);
                ib++;
                ia++;
            }
        }
        free(before);
        free(after);
    }
    return buf_take(&out);
}

/* ---- verification ----------------------------------------------------- */

static int verify_text(const char *text, size_t n, Report *report)
{
    int errors_before = (int)report->errors.n;
    if (report->elided && report->nchanges == 0)
        strvec_push(&report->errors,
                    "elided sites but no function-level change set");
    {
        size_t p = 0;
        while (p + 5 <= n) {
            size_t i;
            unsigned long target = 0;
            if (memcmp(text + p, "c->esp", 6) != 0 ||
                (p > 0 && is_word((unsigned char)text[p - 1]))) {
                p++;
                continue;
            }
            i = skip_ws(text, n, p + 6);
            if (i + 1 >= n || text[i] != '-' || text[i + 1] != '=') {
                p++;
                continue;
            }
            i = skip_ws(text, n, i + 2);
            if (i >= n || text[i] != '4') { p++; continue; }
            i = skip_ws(text, n, i + 1);
            if (i >= n || text[i] != ';') { p++; continue; }
            i = skip_ws(text, n, i + 1);
            if (word_at(text, n, i, "dispatch", &i)) {
                i = skip_ws(text, n, i);
                if (i >= n || text[i] != '(') { p++; continue; }
                i = skip_ws(text, n, i + 1);
                if (i >= n || text[i] != 'c') { p++; continue; }
                i = skip_ws(text, n, i + 1);
                if (i >= n || text[i] != ',') { p++; continue; }
                i = skip_ws(text, n, i + 1);
                if (i + 2 <= n && text[i] == '0' && text[i + 1] == 'x') {
                    char va[9];
                    i += 2;
                    if (!hex8_at(text, n, i, va)) { p++; continue; }
                    target = strtoul(va, NULL, 16);
                } else if (i + 2 <= n && text[i] == 'L' && text[i + 1] == '_') {
                    char va[9];
                    i += 2;
                    if (!hex8_at(text, n, i, va)) { p++; continue; }
                    target = strtoul(va, NULL, 16);
                } else {
                    p++;
                    continue;
                }
            } else if (word_at(text, n, i, "WC3_DIRECT_CALL", &i)) {
                char va[9];
                i = skip_ws(text, n, i);
                if (i >= n || text[i] != '(') { p++; continue; }
                i = skip_ws(text, n, i + 1);
                if (i + 2 > n || text[i] != 'L' || text[i + 1] != '_') {
                    p++;
                    continue;
                }
                i += 2;
                if (!hex8_at(text, n, i, va)) { p++; continue; }
                target = strtoul(va, NULL, 16);
            } else {
                p++;
                continue;
            }
            if (target >= HLE_LO && target < HLE_HI) {
                strvec_push(&report->errors, "HLE call was rewritten");
                break;
            }
            p++;
        }
    }
    return (int)report->errors.n == errors_before;
}

/* ---- report ----------------------------------------------------------- */

static int write_json_string(FILE *f, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    fputc('"', f);
    while (*p) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\b': fputs("\\b", f); break;
        case '\f': fputs("\\f", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (*p < 0x20) fprintf(f, "\\u%04x", *p);
            else fputc(*p, f);
        }
        p++;
    }
    fputc('"', f);
    return 0;
}

static void write_json_path(FILE *f, const char *s)
{
    size_t i;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    for (i = 0; i < n; i++) p[i] = s[i] == '\\' ? '/' : s[i];
    p[n] = '\0';
    write_json_string(f, p);
    free(p);
}

static int write_report(const char *path, Report *r)
{
    FILE *f;
    size_t i;
    if (!path) return 0;
    /* Text mode gives the CRLF newlines that Python's Path.write_text uses
     * on Windows; the JSON escapes themselves contain no raw newlines. */
    f = fopen(path, "w");
    if (!f) return -1;
    fputs("{\n  \"schema\": 1,\n  \"pass_id\": \"return_address_elide\",\n"
          "  \"files\": 1,\n  \"functions\": ", f);
    fprintf(f, "%zu,\n  \"call_sites\": %zu,\n  \"elided\": %zu,\n"
               "  \"skipped_reads_ra\": %zu,\n  \"skipped_unknown\": %zu,\n"
               "  \"skipped_hle\": %zu,\n  \"changed_functions\": %zu,\n"
               "  \"change_set\": [",
            r->functions, r->call_sites, r->elided, r->skipped_reads_ra,
            r->skipped_unknown, r->skipped_hle, r->changed_functions);
    for (i = 0; i < r->nchanges; i++) {
        fputs("\n    {\n      \"file\": ", f);
        write_json_path(f, r->file);
        fputs(",\n      \"va\": ", f);
        write_json_string(f, r->changes[i].va);
        fputs(",\n      \"pass_id\": \"return_address_elide\",\n"
              "      \"before_sha256\": ", f);
        write_json_string(f, r->changes[i].before_sha);
        fputs(",\n      \"after_sha256\": ", f);
        write_json_string(f, r->changes[i].after_sha);
        fputs(",\n      \"reason\": \"return-address store elided before a "
              "direct call whose target does not read [esp]\"\n    }", f);
        if (i + 1 != r->nchanges) fputc(',', f);
    }
    fputs(r->nchanges ? "\n  ],\n  \"errors\": [" : "],\n  \"errors\": [",
          f);
    if (r->errors.n) {
        for (i = 0; i < r->errors.n; i++) {
            fputs("\n    ", f);
            write_json_string(f, r->errors.v[i]);
            if (i + 1 != r->errors.n) fputc(',', f);
        }
        fputs("\n  ", f);
    }
    fprintf(f, "],\n  \"verdict\": \"%s\"\n}\n",
            r->errors.n ? "fail" : "pass");
    fclose(f);
    return 0;
}

static void report_free(Report *r)
{
    size_t i;
    free(r->file);
    free(r->output);
    free(r->changes);
    for (i = 0; i < r->errors.n; i++) free(r->errors.v[i]);
    free(r->errors.v);
    memset(r, 0, sizeof(*r));
}

static int process_file(const char *src, const char *dst, Report *report)
{
    char *text;
    size_t n;
    char *out;
    InfoVec infos;
    size_t i;
    FILE *f = fopen(src, "rb");
    long sz;
    if (!f) {
        fprintf(stderr, "return_address_elide: cannot read %s\n", src);
        return 2;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        fprintf(stderr, "return_address_elide: cannot read %s\n", src);
        return 2;
    }
    text = (char *)xrealloc(NULL, (size_t)sz + 1);
    if (fread(text, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(text);
        fprintf(stderr, "return_address_elide: cannot read %s\n", src);
        return 2;
    }
    fclose(f);
    text[sz] = 0;
    n = (size_t)sz;
    report->file = xstrdup(src);

    infos = index_functions(text, n);
    compute_tail_closures(&infos);
    {
        size_t nb = 0;
        BodyEntry *b = build_bodies(text, n, &nb);
        report->functions = nb;
        free(b);
    }
    out = transform_text(text, n, &infos, report);
    verify_text(out, strlen(out), report);
    if (report->errors.n == 0 && dst) {
        const char *slash = strrchr(dst, '/');
        const char *bslash = strrchr(dst, '\\');
        const char *last = slash;
        if (bslash && (!last || bslash > last)) last = bslash;
        if (last && last != dst) {
            char *parent = xstrdup(dst);
            parent[(size_t)(last - dst)] = 0;
            {
                char *p;
                for (p = parent; *p; p++) {
                    if ((*p == '/' || *p == '\\') && p != parent && p[1]) {
                        char c = *p;
                        *p = 0;
                        make_dir(parent);
                        *p = c;
                    }
                }
                make_dir(parent);
            }
            free(parent);
        }
        f = fopen(dst, "wb");
        if (!f) {
            fprintf(stderr, "return_address_elide: cannot write %s\n", dst);
            free(out);
            free(text);
            infovec_free(&infos);
            return 2;
        }
        if (fwrite(out, 1, strlen(out), f) != strlen(out) || fclose(f) != 0) {
            fprintf(stderr, "return_address_elide: cannot write %s\n", dst);
            free(out);
            free(text);
            infovec_free(&infos);
            return 2;
        }
        report->output = xstrdup(dst);
    }
    free(out);
    free(text);
    infovec_free(&infos);
    for (i = 0; i < report->errors.n; i++)
        fprintf(stderr, "%s: %s\n", src, report->errors.v[i]);
    return report->errors.n ? 1 : 0;
}

/* ---- self-test -------------------------------------------------------- */

static const char SELF_TEST[] =
    "#include \"cpu.h\"\n"
    "void L_00401000(CPU *c)\n"
    "{\n"
    "    /* safe: no stack read */\n"
    "    c->eax = 1u;\n"
    "    c->esp += 4; return;\n"
    "}\n"
    "void L_00401010(CPU *c)\n"
    "{\n"
    "    /* unsafe: reads the entry return-address slot */\n"
    "    c->eax = (uint32_t)(rd32((c->esp)));\n"
    "    c->esp += 4; return;\n"
    "}\n"
    "void L_00401020(CPU *c)\n"
    "{\n"
    "    /* safe: reads a value it pushed itself */\n"
    "    push32(c, 7u);\n"
    "    c->eax = (uint32_t)(pop32(c));\n"
    "    c->esp += 4; return;\n"
    "}\n"
    "void L_00401030(CPU *c)\n"
    "{\n"
    "    /* unsafe: pops the entry return-address slot */\n"
    "    c->eax = (uint32_t)(pop32(c));\n"
    "    c->esp += 4; return;\n"
    "}\n"
    "void L_00401040(CPU *c)\n"
    "{\n"
    "    /* unsafe: tails to a reader */\n"
    "    WC3_DIRECT_TAIL(L_00401010, c);\n"
    "    return;\n"
    "}\n"
    "void L_00401050(CPU *c)\n"
    "{\n"
    "    /* safe standard frame: [ebp+8] is an argument, not the return address */\n"
    "    push32(c, c->ebp);\n"
    "    c->ebp = (uint32_t)(c->esp);\n"
    "    c->eax = (uint32_t)(rd32((c->ebp + 8u)));\n"
    "    c->ebp = (uint32_t)(pop32(c));\n"
    "    c->esp += 4; return;\n"
    "}\n"
    "void L_00401060(CPU *c)\n"
    "{\n"
    "    /* unsafe standard frame: [ebp+4] is the return address */\n"
    "    push32(c, c->ebp);\n"
    "    c->ebp = (uint32_t)(c->esp);\n"
    "    c->eax = (uint32_t)(rd32((c->ebp + 4u)));\n"
    "    c->ebp = (uint32_t)(pop32(c));\n"
    "    c->esp += 4; return;\n"
    "}\n"
    "void L_00402000(CPU *c)\n"
    "{\n"
    "    push32(c, 0x00402010u); dispatch(c, 0x00401000u);\n"
    "    push32(c, 0x00402020u); dispatch(c, 0x00401010u);\n"
    "    push32(c, 0x00402030u); dispatch(c, 0x00401020u);\n"
    "    push32(c, 0x00402040u); dispatch(c, 0x00401030u);\n"
    "    push32(c, 0x00402050u); dispatch(c, 0x00401040u);\n"
    "    push32(c, 0x00402060u); dispatch(c, 0x00401050u);\n"
    "    push32(c, 0x00402070u); dispatch(c, 0x00401060u);\n"
    "    push32(c, 0x00402080u); dispatch(c, 0x7F000010u);\n"
    "    /* push32(c, 0x00402090u); dispatch(c, 0x00401000u); */\n"
    "    const char *s = \"push32(c, 0x004020A0u); dispatch(c, 0x00401000u);\";\n"
    "    (void)s;\n"
    "    c->esp += 4; return;\n"
    "}\n";

static int self_test(void)
{
    Report report;
    InfoVec infos;
    char *out;
    size_t n = sizeof(SELF_TEST) - 1;
    static const char *const checks[] = {
        "c->esp -= 4; dispatch(c, 0x00401000u);",
        "push32(c, 0x00402020u); dispatch(c, 0x00401010u);",
        "c->esp -= 4; dispatch(c, 0x00401020u);",
        "push32(c, 0x00402040u); dispatch(c, 0x00401030u);",
        "push32(c, 0x00402050u); dispatch(c, 0x00401040u);",
        "c->esp -= 4; dispatch(c, 0x00401050u);",
        "push32(c, 0x00402070u); dispatch(c, 0x00401060u);",
        "push32(c, 0x00402080u); dispatch(c, 0x7F000010u);",
        "/* push32(c, 0x00402090u); dispatch(c, 0x00401000u); */",
        "\"push32(c, 0x004020A0u); dispatch(c, 0x00401000u);\"",
        NULL
    };
    size_t i;
    memset(&report, 0, sizeof(report));
    report.file = xstrdup("self-test");
    infos = index_functions(SELF_TEST, n);
    compute_tail_closures(&infos);
    out = transform_text(SELF_TEST, n, &infos, &report);
    verify_text(out, strlen(out), &report);
    if (report.errors.n) {
        fprintf(stderr, "return_address_elide self-test: FAIL errors\n");
        for (i = 0; i < report.errors.n; i++)
            fprintf(stderr, "  %s\n", report.errors.v[i]);
        free(out);
        infovec_free(&infos);
        report_free(&report);
        return 1;
    }
    for (i = 0; checks[i]; i++) {
        if (!strstr(out, checks[i])) {
            fprintf(stderr, "return_address_elide self-test: FAIL missing %s\n",
                    checks[i]);
            free(out);
            infovec_free(&infos);
            report_free(&report);
            return 1;
        }
    }
    if (report.elided != 3) {
        fprintf(stderr, "return_address_elide self-test: FAIL elided=%zu\n",
                report.elided);
        free(out);
        infovec_free(&infos);
        report_free(&report);
        return 1;
    }
    if (report.nchanges == 0 || report.changed_functions != 1) {
        fprintf(stderr, "return_address_elide self-test: FAIL change set\n");
        free(out);
        infovec_free(&infos);
        report_free(&report);
        return 1;
    }
    for (i = 0; i < report.nchanges; i++) {
        if (strcmp(report.changes[i].before_sha,
                   report.changes[i].after_sha) == 0) {
            fprintf(stderr, "return_address_elide self-test: FAIL unchanged "
                            "body in change set\n");
            free(out);
            infovec_free(&infos);
            report_free(&report);
            return 1;
        }
    }
    {
        static const char forbidden[] =
            "void L_00403000(CPU *c)\n{\n"
            "    c->esp -= 4; WC3_DIRECT_CALL(L_7F000010, c);\n"
            "    c->esp += 4; return;\n}\n";
        Report probe;
        memset(&probe, 0, sizeof(probe));
        probe.elided = 1;
        probe.nchanges = 1;
        if (verify_text(forbidden, sizeof(forbidden) - 1, &probe)) {
            fprintf(stderr, "return_address_elide self-test: FAIL HLE "
                            "direct-call guard\n");
            report_free(&probe);
            free(out);
            infovec_free(&infos);
            report_free(&report);
            return 1;
        }
        report_free(&probe);
    }
    free(out);
    infovec_free(&infos);
    report_free(&report);
    puts("return_address_elide self-test: OK");
    return 0;
}

static void usage(void)
{
    fputs("usage: return_address_elide --lift IN --out OUT "
          "[--json REPORT] [--quiet]\n"
          "       return_address_elide --self-test\n", stderr);
}

int main(int argc, char **argv)
{
    const char *lift = NULL, *out = NULL, *json = NULL;
    int quiet = 0, i, rc;
    Report report;
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return self_test();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--lift") && i + 1 < argc) lift = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) json = argv[++i];
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else { usage(); return 2; }
    }
    if (!lift || !out) { usage(); return 2; }
    memset(&report, 0, sizeof(report));
    rc = process_file(lift, out, &report);
    if (write_report(json, &report) != 0) {
        fprintf(stderr, "return_address_elide: cannot write report %s\n", json);
        rc = 2;
    }
    if (!quiet) {
        printf("return_address_elide: %zu site(s) elided, %zu kept "
               "(target reads [esp]), %zu kept (unknown target)\n",
               report.elided, report.skipped_reads_ra,
               report.skipped_unknown);
    }
    report_free(&report);
    return rc;
}
