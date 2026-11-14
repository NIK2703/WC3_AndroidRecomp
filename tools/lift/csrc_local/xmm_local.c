/* xmm_local.c - D3b first increment: function-local XMM promotion.
 *
 * The generated C keeps every XMM register in CPU.xmm, even when a function
 * has no guest calls, no labels, no loops and no switch.  For such a leaf
 * function the whole body executes on one host frame, so the XMM values can
 * live in a local XMM[8] and be written back only before an exit.
 *
 * Eligibility is deliberately conservative:
 *   - no labels, goto, switch, while, for or do;
 *   - no WC3_DIRECT_CALL, WC3_GUEST_DRAIN or generic dispatch();
 *   - no #define/#include/#undef inside the body;
 *   - only constant c->xmm[0..7] accesses (no whole-array or dynamic access);
 *   - every exit is dispatch_jmp, WC3_DIRECT_TAIL or return; the flush is
 *     inserted immediately before each exit token, after any XMM write on
 *     that line.  dispatch_jmp/WC3_DIRECT_TAIL also get a reload after the
 *     call because they can return after guest code changed c->xmm.
 *
 * Conditional branches without an exit stay inside the function and keep the
 * local values live; no canonicalization is needed until an exit.  The JSON
 * report carries a function-level change set (ABI-11.2): one row per
 * transformed function with access count, used-register mask and body sizes.
 * The pass is part of the normal relifted-corpus pipeline and runs
 * unconditionally after dead_flags; there is no opt-in switch.
 *
 *   xmm_local --lift IN --out OUT [--json REPORT] [--quiet]
 *   xmm_local --self-test
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *p;
    size_t n;
    size_t cap;
} Buf;

/* Function-level change set (ABI-11.2).  A chunk can contain thousands of
 * functions; only the transformed ones are recorded. */
typedef struct {
    char *func;
    size_t accesses;
    size_t bytes_before;
    size_t bytes_after;
    unsigned used_mask;
} Change;

typedef struct {
    Change *v;
    size_t n;
    size_t cap;
} Changes;

static void die(const char *msg)
{
    fprintf(stderr, "xmm_local: %s\n", msg);
    exit(2);
}

static void die_oom(void)
{
    die("out of memory");
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die_oom();
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die_oom();
    return q;
}

static void changes_free(Changes *cs)
{
    size_t i;
    for (i = 0; i < cs->n; i++) free(cs->v[i].func);
    free(cs->v);
    cs->v = NULL;
    cs->n = 0;
    cs->cap = 0;
}

static void changes_add(Changes *cs, const char *func, size_t accesses,
                        size_t before, size_t after, unsigned mask)
{
    Change *ch;
    size_t len;
    if (cs->n == cs->cap) {
        cs->cap = cs->cap ? cs->cap * 2 : 8;
        cs->v = (Change *)xrealloc(cs->v, cs->cap * sizeof *cs->v);
    }
    ch = &cs->v[cs->n++];
    len = strlen(func);
    ch->func = (char *)xmalloc(len + 1);
    memcpy(ch->func, func, len + 1);
    ch->accesses = accesses;
    ch->bytes_before = before;
    ch->bytes_after = after;
    ch->used_mask = mask;
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
    if (!b->p) die_oom();
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

static void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) die("formatting failed");
    buf_need(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    b->n += (size_t)n;
    b->p[b->n] = 0;
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
    p = (char *)xmalloc((size_t)sz + 1);
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

static int word_char(char c)
{
    unsigned char u = (unsigned char)c;
    return isalnum(u) || c == '_';
}

static int has_word(const char *s, size_t n, const char *w)
{
    size_t wl = strlen(w);
    size_t i;
    if (wl == 0 || wl > n) return 0;
    for (i = 0; i + wl <= n; i++) {
        if (strncmp(s + i, w, wl) != 0) continue;
        if (i > 0 && word_char(s[i - 1])) continue;
        if (i + wl < n && word_char(s[i + wl])) continue;
        return 1;
    }
    return 0;
}

static int starts_word(const char *s, size_t n, size_t i, const char *w)
{
    size_t wl = strlen(w);
    if (i + wl > n) return 0;
    if (strncmp(s + i, w, wl) != 0) return 0;
    if (i > 0 && word_char(s[i - 1])) return 0;
    if (i + wl < n && word_char(s[i + wl])) return 0;
    return 1;
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
                m[i] = ' ';
                i++;
            }
        } else if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') {
                m[i] = ' ';
                i++;
            }
        } else if (s[i] == '"' || s[i] == '\'') {
            char quote = s[i];
            m[i] = ' ';
            i++;
            while (i < n) {
                if (s[i] == '\\' && i + 1 < n) {
                    m[i] = m[i + 1] = ' ';
                    i += 2;
                } else {
                    int done = s[i] == quote;
                    m[i] = ' ';
                    i++;
                    if (done) break;
                }
            }
        } else {
            i++;
        }
    }
}

/* Find the closing brace of the block at text[open]; comments and strings are
 * skipped.  Returns SIZE_MAX when the block is not terminated. */
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

static int has_label(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t line_start = i;
        size_t p;
        while (i < n && s[i] != '\n') i++;
        p = line_start;
        while (p < i && (s[p] == ' ' || s[p] == '\t')) p++;
        if (p + 2 < i && s[p] == 'L' && s[p + 1] == '_') {
            size_t q = p + 2;
            while (q < i && isxdigit((unsigned char)s[q])) q++;
            if (q < i && s[q] == ':') return 1;
        }
        if (i < n) i++;
    }
    return 0;
}

static int has_dispatch_call(const char *s, size_t n)
{
    size_t i = 0;
    while (i + 8 <= n) {
        if (strncmp(s + i, "dispatch", 8) == 0) {
            size_t j = i + 8;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            if (j < n && s[j] == '(') return 1;
        }
        i++;
    }
    return 0;
}

/* The flush is a statement.  It is safe inside a braced branch, but an
 * unbraced `if (x) return;` would turn into `if (x) flush; return;`, i.e. the
 * return would become unconditional.  Reject every function whose if/else
 * bodies are not braced; that keeps the text pass local and predictable. */
static int has_unbraced_if(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (starts_word(s, n, i, "if")) {
            size_t p = i + 2;
            size_t depth = 0;
            while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
            if (p >= n || s[p] != '(') return 1;
            while (p < n) {
                if (s[p] == '(') depth++;
                else if (s[p] == ')') {
                    depth--;
                    if (depth == 0) {
                        p++;
                        break;
                    }
                }
                p++;
            }
            if (depth != 0) return 1;
            while (p < n && (s[p] == ' ' || s[p] == '\t' ||
                             s[p] == '\r' || s[p] == '\n')) p++;
            if (p >= n || s[p] != '{') return 1;
        } else if (starts_word(s, n, i, "else")) {
            size_t p = i + 4;
            while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
            if (p < n && starts_word(s, n, p, "if")) continue;
            if (p >= n || s[p] != '{') return 1;
        }
    }
    return 0;
}

/* Collect constant c->xmm[N] indices.  Returns -1 for a whole-array or
 * dynamic access, so the caller can skip the function. */
static int collect_indices(const char *s, size_t n, unsigned char used[16])
{
    size_t i = 0;
    while (i + 6 <= n) {
        const char *hit = strstr(s + i, "c->xmm");
        size_t pos;
        size_t q;
        size_t value = 0;
        int digits = 0;
        if (!hit) break;
        pos = (size_t)(hit - s);
        if (pos > 0 && word_char(s[pos - 1])) {
            i = pos + 6;
            continue;
        }
        if (pos + 6 >= n || s[pos + 6] != '[') return -1;
        q = pos + 7;
        while (q < n && isdigit((unsigned char)s[q])) {
            value = value * 10 + (size_t)(s[q] - '0');
            if (value > 1000) return -1;
            digits = 1;
            q++;
        }
        if (!digits || q >= n || s[q] != ']') return -1;
        if (value > 7) return -1;   /* x86-32 has xmm0..xmm7 only */
        used[value] = 1;
        i = q + 1;
    }
    return 0;
}

static size_t count_xmm_accesses(const char *s, size_t n)
{
    size_t i = 0;
    size_t count = 0;
    while (i + 7 <= n) {
        const char *hit = strstr(s + i, "c->xmm[");
        size_t pos;
        if (!hit) break;
        pos = (size_t)(hit - s);
        if (pos > 0 && word_char(s[pos - 1])) {
            i = pos + 7;
            continue;
        }
        count++;
        i = pos + 7;
    }
    return count;
}

static int is_dispatch_exit(const char *s, size_t n, size_t i)
{
    if (i > 0 && word_char(s[i - 1])) return 0;
    if (i + 12 <= n && strncmp(s + i, "dispatch_jmp", 12) == 0 &&
        (i + 12 == n || !word_char(s[i + 12]))) return 1;
    if (i + 15 <= n && strncmp(s + i, "WC3_DIRECT_TAIL", 15) == 0 &&
        (i + 15 == n || !word_char(s[i + 15]))) return 1;
    return 0;
}

static int is_exit_at(const char *s, size_t n, size_t i)
{
    if (starts_word(s, n, i, "return")) return 1;
    return is_dispatch_exit(s, n, i);
}

static void append_flush(Buf *b, const unsigned char used[16])
{
    int i;
    for (i = 0; i < 16; i++) {
        if (used[i])
            buf_printf(b, " c->xmm[%d] = _wc3_xmm_local[%d];", i, i);
    }
    buf_str(b, " ");
}

/* A dispatch or tail call may return after executing guest code which has
 * itself changed c->xmm.  The local copy is stale at that point; reload it
 * after the call's semicolon so a following return (or more translated code)
 * writes the guest state, not the old local copy. */
static void append_reload(Buf *b, const unsigned char used[16])
{
    int i;
    for (i = 0; i < 16; i++) {
        if (used[i])
            buf_printf(b, " _wc3_xmm_local[%d] = c->xmm[%d];", i, i);
    }
    buf_str(b, " ");
}

static int eligible(const char *masked, size_t n, unsigned char used[16])
{
    int any = 0;
    int i;
    if (has_word(masked, n, "goto")) return 0;
    if (has_word(masked, n, "switch")) return 0;
    if (has_word(masked, n, "while")) return 0;
    if (has_word(masked, n, "for")) return 0;
    if (has_word(masked, n, "do")) return 0;
    if (has_word(masked, n, "break")) return 0;
    if (has_word(masked, n, "continue")) return 0;
    if (has_unbraced_if(masked, n)) return 0;
    if (strstr(masked, "WC3_DIRECT_CALL")) return 0;
    if (strstr(masked, "WC3_GUEST_DRAIN")) return 0;
    if (strstr(masked, "wc3_guest_")) return 0;
    if (strstr(masked, "#define") || strstr(masked, "#include") ||
        strstr(masked, "#undef")) return 0;
    if (has_dispatch_call(masked, n)) return 0;
    if (has_label(masked, n)) return 0;
    if (strstr(masked, "_wc3_xmm_local")) return 0;
    if (collect_indices(masked, n, used) != 0) return 0;
    for (i = 0; i < 16; i++) {
        if (used[i]) {
            any = 1;
            break;
        }
    }
    return any;
}

static void transform_body(const char *body, size_t n, const char *masked,
                           const unsigned char used[16], Buf *out)
{
    size_t i;
    int pending_reload = 0;
    buf_str(out, "{\n    XMM _wc3_xmm_local[8];\n");
    for (i = 0; i < 16; i++) {
        if (used[i])
            buf_printf(out, "    _wc3_xmm_local[%d] = c->xmm[%d];\n", (int)i, (int)i);
    }
    i = 1;
    if (i + 1 < n && body[i] == '\r' && body[i + 1] == '\n') i += 2;
    else if (i < n && body[i] == '\n') i++;
    while (i + 1 < n) {
        if (is_exit_at(masked, n, i)) {
            append_flush(out, used);
            if (is_dispatch_exit(masked, n, i)) pending_reload = 1;
        }
        if (masked[i] == 'c' && (i == 0 || !word_char(masked[i - 1])) &&
            i + 7 <= n && strncmp(masked + i, "c->xmm[", 7) == 0) {
            buf_str(out, "_wc3_xmm_local[");
            i += 7;
            continue;
        }
        buf_add(out, body + i, 1);
        if (pending_reload && masked[i] == ';') {
            append_reload(out, used);
            pending_reload = 0;
        }
        i++;
    }
    if (out->n > 0 && out->p[out->n - 1] != '\n') buf_str(out, "\n");
    append_flush(out, used);
    buf_str(out, "\n}");
}

/* Find a generated function header at or after start.  Only the canonical
 * `void L_<hex>(CPU *restrict c)\n{` form is accepted. */
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

static void transform_text(const char *text, size_t n, Buf *out,
                           size_t *out_funcs, size_t *out_changed,
                           Changes *changes)
{
    size_t last = 0;
    size_t scan = 0;
    *out_funcs = 0;
    *out_changed = 0;
    while (scan < n) {
        size_t hdr = find_func(text, n, scan);
        size_t nl_pos;
        const char *nl;
        size_t open;
        size_t close;
        size_t blen;
        char *masked;
        unsigned char used[16];
        int ok;
        if (hdr == SIZE_MAX) break;
        nl = (const char *)memchr(text + hdr, '\n', n - hdr);
        if (!nl) break;
        nl_pos = (size_t)(nl - text);
        open = nl_pos + 1;
        if (open + 1 < n && text[open] == '\r' && text[open + 1] == '{') open++;
        if (open >= n || text[open] != '{') {
            scan = open + 1;
            continue;
        }
        close = matching_brace(text, n, open);
        if (close == SIZE_MAX) break;
        (*out_funcs)++;
        buf_add(out, text + last, open - last);
        blen = close - open + 1;
        masked = (char *)xmalloc(blen + 1);
        mask_code(text + open, blen, masked);
        memset(used, 0, sizeof used);
        ok = eligible(masked, blen, used);
        if (ok) {
            size_t before = out->n;
            size_t accesses = count_xmm_accesses(masked, blen);
            unsigned mask = 0;
            char fname[64];
            const char *fstart = text + hdr + 5;
            const char *fend = (const char *)memchr(fstart, '(',
                                                    (size_t)(nl - fstart));
            size_t flen;
            int k;
            if (!fend) fend = nl;
            flen = (size_t)(fend - fstart);
            if (flen >= sizeof fname) flen = sizeof fname - 1;
            memcpy(fname, fstart, flen);
            fname[flen] = 0;
            for (k = 0; k < 16; k++)
                if (used[k]) mask |= 1u << k;
            transform_body(text + open, blen, masked, used, out);
            if (changes)
                changes_add(changes, fname, accesses, blen, out->n - before,
                            mask);
            (*out_changed)++;
        } else {
            buf_add(out, text + open, blen);
        }
        free(masked);
        last = close + 1;
        scan = close + 1;
    }
    buf_add(out, text + last, n - last);
}

static void write_report(const char *path, size_t funcs, size_t changed,
                         size_t before, size_t after, const Changes *changes)
{
    FILE *f = fopen(path, "wb");
    size_t i;
    if (!f) die("cannot write json report");
    fprintf(f,
            "{\"tool\":\"xmm_local\",\"functions\":%zu,\"transformed\":%zu,"
            "\"bytes_before\":%zu,\"bytes_after\":%zu,\"changes\":[",
            funcs, changed, before, after);
    for (i = 0; i < changes->n; i++) {
        const Change *ch = &changes->v[i];
        if (i) fputc(',', f);
        fprintf(f,
                "{\"function\":\"%s\",\"xmm_accesses\":%zu,"
                "\"used_mask\":%u,\"body_before\":%zu,\"body_after\":%zu}",
                ch->func, ch->accesses, ch->used_mask, ch->bytes_before,
                ch->bytes_after);
    }
    fputs("]}\n", f);
    if (fclose(f) != 0) die("cannot close json report");
}

static int check_contains(const char *name, const char *text, const char *needle)
{
    if (!strstr(text, needle)) {
        fprintf(stderr, "xmm_local self-test: %s: missing %s\n", name, needle);
        return 0;
    }
    return 1;
}

static int self_test(void)
{
    static const char *sample =
        "void L_00000001(CPU *restrict c)\n"
        "{\n"
        "    sse_load_ss(&c->xmm[0], 1.0f);\n"
        "    c->xmm[1] = c->xmm[0];\n"
        "    return;\n"
        "}\n";
    static const char *label =
        "void L_00000002(CPU *restrict c)\n"
        "{\n"
        "L_00000003:\n"
        "    c->xmm[0] = c->xmm[1];\n"
        "    return;\n"
        "}\n";
    static const char *call =
        "void L_00000004(CPU *restrict c)\n"
        "{\n"
        "    WC3_DIRECT_CALL(L_00000005, c);\n"
        "    c->xmm[0] = c->xmm[1];\n"
        "    return;\n"
        "}\n";
    static const char *drain =
        "void L_00000009(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[0] = c->xmm[1];\n"
        "    WC3_GUEST_DRAIN(c);\n"
        "    return;\n"
        "}\n";
    static const char *comment =
        "void L_00000006(CPU *restrict c)\n"
        "{\n"
        "    /* c->xmm[0] is mentioned only here */\n"
        "    return;\n"
        "}\n";
    static const char *fall =
        "void L_00000007(CPU *restrict c)\n"
        "{\n"
        "    if (c->xmm[0].f32[0] > 0.0f) { c->xmm[1] = c->xmm[0]; dispatch_jmp(c, 1u); return; }\n"
        "    c->xmm[2] = c->xmm[1];\n"
        "}\n";
    static const char *unbraced =
        "void L_00000008(CPU *restrict c)\n"
        "{\n"
        "    if (c->xmm[0].f32[0]) return;\n"
        "    c->xmm[1] = c->xmm[0];\n"
        "}\n";
    Buf out;
    Changes changes;
    size_t funcs, changed;
    int ok = 1;
    memset(&changes, 0, sizeof changes);
    memset(&out, 0, sizeof out);
    transform_text(sample, strlen(sample), &out, &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1;
    ok &= changes.n == 1 &&
          strcmp(changes.v[0].func, "L_00000001") == 0;
    ok &= changes.n == 1 && changes.v[0].accesses == 3;
    ok &= changes.n == 1 && changes.v[0].used_mask == 3u;
    ok &= changes.n == 1 && changes.v[0].bytes_after >
          changes.v[0].bytes_before;
    ok &= check_contains("sample", out.p, "XMM _wc3_xmm_local[8];");
    ok &= check_contains("sample", out.p, "_wc3_xmm_local[0] = c->xmm[0];");
    ok &= check_contains("sample", out.p, "sse_load_ss(&_wc3_xmm_local[0], 1.0f);");
    ok &= check_contains("sample", out.p, "_wc3_xmm_local[1] = _wc3_xmm_local[0];");
    ok &= check_contains("sample", out.p, "c->xmm[0] = _wc3_xmm_local[0];");
    ok &= check_contains("sample", out.p, "c->xmm[1] = _wc3_xmm_local[1];");
    free(out.p);
    memset(&out, 0, sizeof out);
    transform_text(label, strlen(label), &out, &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(label) && memcmp(out.p, label, out.n) == 0;
    ok &= changes.n == 1;
    free(out.p);
    memset(&out, 0, sizeof out);
    transform_text(call, strlen(call), &out, &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(call) && memcmp(out.p, call, out.n) == 0;
    ok &= changes.n == 1;
    free(out.p);
    memset(&out, 0, sizeof out);
    transform_text(drain, strlen(drain), &out, &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(drain) && memcmp(out.p, drain, out.n) == 0;
    ok &= changes.n == 1;
    free(out.p);
    memset(&out, 0, sizeof out);
    transform_text(comment, strlen(comment), &out, &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(comment) && memcmp(out.p, comment, out.n) == 0;
    ok &= changes.n == 1;
    free(out.p);
    memset(&out, 0, sizeof out);
    transform_text(fall, strlen(fall), &out, &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= check_contains("fall", out.p, "_wc3_xmm_local[0].f32[0]");
    ok &= check_contains("fall", out.p,
                         "dispatch_jmp(c, 1u); _wc3_xmm_local[0] = c->xmm[0];");
    ok &= check_contains("fall", out.p, "c->xmm[2] = _wc3_xmm_local[2];");
    ok &= changes.n == 2;
    if (changes.n == 2) {
        ok &= strcmp(changes.v[1].func, "L_00000007") == 0;
        ok &= changes.v[1].accesses == 5;
        ok &= changes.v[1].used_mask == 7u;
        ok &= changes.v[1].bytes_after > changes.v[1].bytes_before;
    }
    free(out.p);
    memset(&out, 0, sizeof out);
    transform_text(unbraced, strlen(unbraced), &out, &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(unbraced) && memcmp(out.p, unbraced, out.n) == 0;
    ok &= changes.n == 2;
    free(out.p);
    changes_free(&changes);
    if (!ok) {
        fputs("xmm_local self-test: FAILED\n", stderr);
        return 1;
    }
    puts("xmm_local self-test: OK");
    return 0;
}

static void usage(void)
{
    fputs("xmm_local: use --lift IN --out OUT [--json REPORT] [--quiet]\n"
          "           or --self-test\n", stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *lift = NULL;
    const char *out_path = NULL;
    const char *json_path = NULL;
    int quiet = 0;
    int i;
    char *text;
    size_t n;
    Buf out;
    Changes changes;
    size_t funcs, changed;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--self-test") == 0) return self_test();
        if (strcmp(argv[i], "--quiet") == 0) { quiet = 1; continue; }
        if (strcmp(argv[i], "--lift") == 0 && i + 1 < argc) {
            lift = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
            json_path = argv[++i];
            continue;
        }
        usage();
    }
    if (!lift || !out_path) usage();
    text = slurp(lift, &n);
    if (!text) die("cannot read input");
    memset(&out, 0, sizeof out);
    memset(&changes, 0, sizeof changes);
    transform_text(text, n, &out, &funcs, &changed, &changes);
    if (write_all(out_path, out.p ? out.p : "", out.n) != 0) die("cannot write output");
    if (json_path) write_report(json_path, funcs, changed, n, out.n, &changes);
    if (!quiet)
        printf("xmm_local: %zu functions, %zu transformed, %zu -> %zu bytes\n",
               funcs, changed, n, out.n);
    changes_free(&changes);
    free(out.p);
    free(text);
    return 0;
}
