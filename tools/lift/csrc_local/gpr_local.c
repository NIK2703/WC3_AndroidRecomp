/* gpr_local.c - read-only guest GPR local caching.
 *
 * The generated C keeps every guest general-purpose register in CPU.  A hot
 * function often reloads the same register from that struct after every
 * operation, even when the register is only read for a long straight-line
 * stretch.  This pass promotes a *read-only* value to a local:
 *
 *     uint32_t _wc3_gpr_ebp = c->ebp;
 *     ... _wc3_gpr_ebp ...
 *
 * Only reads are replaced.  The pass never writes a cached local back to
 * CPU, so an interval ends at the next write to that register.  This is the
 * deliberately conservative half of GPR promotion: the guest-visible state
 * and every write stay exactly where the lifter put them.
 *
 * Intervals:
 *   - read-only segment 0, from function entry to the first write;
 *   - any later segment between two writes;
 *   - a live-in segment before a write at the end of the function.
 *
 * A local is initialized after the line containing its interval's opening
 * write, so reads on that line remain in memory.  A segment opens only at a
 * plain statement.  Nested blocks are allowed when the declaration stays in
 * scope until the interval closes.
 *
 * Safety rules:
 *   - a guest call, dispatch, HLE call, wc3_* helper, selection24_* helper,
 *     direct lifted call or unknown call is a clobber barrier; only the
 *     prefix before the first barrier is cached;
 *   - writes and calls inside a loop invalidate a live-in interval that is
 *     entered from outside the loop;
 *   - a goto from before the init into the cached interval is rejected;
 *   - C loops (while/for/do), switch, break/continue, preprocessor lines,
 *     multi-line comments, multi-line expressions and an unbraced if/else
 *     split are left alone;
 *   - an address-taken guest GPR disables the whole function;
 *   - at least one read must be replaced, and every function is transformed
 *     at most once (the second pass is a byte-for-byte no-op).
 *
 * The JSON report carries a function-level change set: one row per changed
 * function with the cached-register mask, replaced-read count and body sizes.
 *
 *   gpr_local --lift IN --out OUT [--json REPORT] [--quiet]
 *   gpr_local --self-test
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

typedef struct {
    char *func;
    unsigned mask;
    size_t reads;
    size_t body_before;
    size_t body_after;
} Change;

typedef struct {
    Change *v;
    size_t n;
    size_t cap;
} Changes;

typedef struct {
    size_t pos;
    int kind; /* 0 read, 1 write, 2 address-taken, 3 read-modify-write */
} Event;

typedef struct {
    Event *v;
    size_t n;
    size_t cap;
} Events;

typedef struct {
    char name[128];
    size_t pos;
} NamedPos;

typedef struct {
    NamedPos *v;
    size_t n;
    size_t cap;
} NamedVec;

typedef struct {
    size_t start;
    size_t end; /* exclusive, includes the newline when present */
} Line;

typedef struct {
    Line *v;
    size_t n;
    size_t cap;
} Lines;

static const char *const reg_names[8] = {
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"
};

/* All GPRs except esp.  esp is not cached: push32/pop32 and the generated
 * stack adjustments write it through helpers that are otherwise safe to call
 * between cached reads.  eax and ebx are included; the final object A/B on the
 * hot functions shows a further size reduction, and the read-only interval
 * rules are the same for every register. */
static const int reg_allowed[8] = { 1, 1, 1, 1, 0, 1, 1, 1 };

static void die(const char *msg)
{
    fprintf(stderr, "gpr_local: %s\n", msg);
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
                m[i] = s[i] == '\n' ? '\n' : ' ';
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
                    m[i] = s[i] == '\n' ? '\n' : ' ';
                    i++;
                    if (done) break;
                }
            }
        } else {
            i++;
        }
    }
}

static int has_multiline_comment(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
                if (s[i] == '\n') return 1;
                i++;
            }
            if (i + 1 >= n) return 1;
            i += 2;
        } else if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') i++;
        } else if (s[i] == '"' || s[i] == '\'') {
            char quote = s[i++];
            while (i < n) {
                if (s[i] == '\\' && i + 1 < n) {
                    i += 2;
                } else if (s[i] == quote) {
                    i++;
                    break;
                } else {
                    i++;
                }
            }
        } else {
            i++;
        }
    }
    return 0;
}

static int has_preprocessor_line(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t p = i;
        while (p < n && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')) p++;
        if (p < n && s[p] == '#') return 1;
        while (i < n && s[i] != '\n') i++;
        if (i < n) i++;
    }
    return 0;
}

static int parens_cross_lines(const char *s, size_t n)
{
    int depth = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        if (s[i] == '(') {
            depth++;
        } else if (s[i] == ')') {
            if (depth > 0) depth--;
        } else if (s[i] == '\n' && depth > 0) {
            return 1;
        }
    }
    return 0;
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

static size_t line_start(const char *s, size_t p)
{
    while (p > 0 && s[p - 1] != '\n') p--;
    return p;
}

static size_t line_end(const char *s, size_t n, size_t p)
{
    while (p < n && s[p] != '\n') p++;
    return p;
}

static size_t line_number(const char *s, size_t p)
{
    size_t i;
    size_t n = 0;
    for (i = 0; i < p; i++) {
        if (s[i] == '\n') n++;
    }
    return n;
}

static void events_push(Events *ev, size_t pos, int kind)
{
    if (ev->n == ev->cap) {
        ev->cap = ev->cap ? ev->cap * 2 : 16;
        ev->v = (Event *)xrealloc(ev->v, ev->cap * sizeof *ev->v);
    }
    ev->v[ev->n].pos = pos;
    ev->v[ev->n].kind = kind;
    ev->n++;
}

static void named_push(NamedVec *nv, const char *name, size_t len, size_t pos)
{
    if (nv->n == nv->cap) {
        nv->cap = nv->cap ? nv->cap * 2 : 16;
        nv->v = (NamedPos *)xrealloc(nv->v, nv->cap * sizeof *nv->v);
    }
    if (len >= sizeof nv->v[nv->n].name) len = sizeof nv->v[nv->n].name - 1;
    memcpy(nv->v[nv->n].name, name, len);
    nv->v[nv->n].name[len] = 0;
    nv->v[nv->n].pos = pos;
    nv->n++;
}

static size_t named_find(const NamedVec *nv, const char *name)
{
    size_t i;
    for (i = 0; i < nv->n; i++) {
        if (strcmp(nv->v[i].name, name) == 0) return nv->v[i].pos;
    }
    return SIZE_MAX;
}

static int macro_write_before(const char *s, size_t n, size_t pos)
{
    size_t q = pos;
    size_t t;
    size_t e;
    size_t len;
    (void)n;
    while (q > 0 && (s[q - 1] == ' ' || s[q - 1] == '\t' ||
                     s[q - 1] == '\r' || s[q - 1] == '\n'))
        q--;
    if (q == 0 || s[q - 1] != '(') return 0;
    t = q - 1;
    while (t > 0 && (s[t - 1] == '(' || s[t - 1] == ' ' ||
                     s[t - 1] == '\t' || s[t - 1] == '\r' ||
                     s[t - 1] == '\n'))
        t--;
    e = t;
    while (e > 0 && word_char(s[e - 1])) e--;
    len = t - e;
    if (len == 5 && strncmp(s + e, "SET8L", 5) == 0) return 1;
    if (len == 5 && strncmp(s + e, "SET8H", 5) == 0) return 1;
    if (len == 5 && strncmp(s + e, "SET16", 5) == 0) return 1;
    return 0;
}

static int range_has_any(const char *s, size_t a, size_t b, const char *needle)
{
    size_t nl = strlen(needle);
    size_t i;
    if (nl == 0 || b < a || nl > b - a) return 0;
    for (i = a; i + nl <= b; i++) {
        if (strncmp(s + i, needle, nl) == 0) return 1;
    }
    return 0;
}

/* Distinguish unary address-of from binary bitwise AND.  The generated code
 * has both `&c->eax` (rare, disables the pass for the function) and
 * `c->eax & c->eax` (common in flags helpers).  A false "binary" answer would
 * be unsafe, so ambiguous parenthesized values are treated as unary and merely
 * skip the function. */
static int amp_is_unary(const char *s, size_t n, size_t amp)
{
    static const char *const unary_words[] = {
        "return", "sizeof", "alignof", "typeof", "case", "default", "else",
        "do", "if", "while", "for", "switch", "goto"
    };
    size_t p = amp;
    size_t i;
    char c;
    (void)n;
    while (p > 0 && (s[p - 1] == ' ' || s[p - 1] == '\t' ||
                     s[p - 1] == '\r' || s[p - 1] == '\n'))
        p--;
    if (p == 0) return 1;
    c = s[p - 1];
    if (word_char(c)) {
        size_t b = p;
        while (b > 0 && word_char(s[b - 1])) b--;
        for (i = 0; i < sizeof unary_words / sizeof unary_words[0]; i++) {
            if (strlen(unary_words[i]) == p - b &&
                strncmp(s + b, unary_words[i], p - b) == 0)
                return 1;
        }
        return 0;
    }
    if (c == ')') {
        size_t q = p;
        int depth = 0;
        while (q > 0) {
            q--;
            if (s[q] == ')') {
                depth++;
            } else if (s[q] == '(') {
                depth--;
                if (depth == 0) break;
            }
        }
        if (q == 0 && s[q] != '(') return 1;
        if (range_has_any(s, q + 1, p - 1, "c->") ||
            range_has_any(s, q + 1, p - 1, "++") ||
            range_has_any(s, q + 1, p - 1, "--") ||
            range_has_any(s, q + 1, p - 1, "+") ||
            range_has_any(s, q + 1, p - 1, "-") ||
            range_has_any(s, q + 1, p - 1, "/") ||
            range_has_any(s, q + 1, p - 1, "%") ||
            range_has_any(s, q + 1, p - 1, "<") ||
            range_has_any(s, q + 1, p - 1, ">") ||
            range_has_any(s, q + 1, p - 1, "=") ||
            range_has_any(s, q + 1, p - 1, "!") ||
            range_has_any(s, q + 1, p - 1, "&") ||
            range_has_any(s, q + 1, p - 1, "|") ||
            range_has_any(s, q + 1, p - 1, "^") ||
            range_has_any(s, q + 1, p - 1, "?") ||
            range_has_any(s, q + 1, p - 1, ",") ||
            range_has_any(s, q + 1, p - 1, ":"))
            return 0;
        return 1;
    }
    if (c == ']' || c == '.' || isdigit((unsigned char)c) ||
        c == '\'' || c == '"')
        return 0;
    return 1;
}

static int classify_access(const char *s, size_t n, size_t pos, int reg,
                           size_t *after_out)
{
    size_t after = pos + 3 + strlen(reg_names[reg]);
    size_t q;
    if (after > n) return 0;
    if (after < n && word_char(s[after])) return -1;
    q = after;
    while (q < n && (s[q] == ' ' || s[q] == '\t' ||
                     s[q] == '\r' || s[q] == '\n'))
        q++;
    if (q < n && s[q] == '=') {
        if (q + 1 < n && s[q + 1] == '=') return 0;
        *after_out = after;
        return 1;
    }
    if (q + 1 < n && ((s[q] == '+' && s[q + 1] == '+') ||
                      (s[q] == '-' && s[q + 1] == '-'))) {
        *after_out = after;
        return 3;
    }
    if (q + 1 < n && ((s[q] == '+' && s[q + 1] == '=') ||
                      (s[q] == '-' && s[q + 1] == '=') ||
                      (s[q] == '*' && s[q + 1] == '=') ||
                      (s[q] == '/' && s[q + 1] == '=') ||
                      (s[q] == '%' && s[q + 1] == '=') ||
                      (s[q] == '&' && s[q + 1] == '=') ||
                      (s[q] == '|' && s[q + 1] == '=') ||
                      (s[q] == '^' && s[q + 1] == '='))) {
        *after_out = after;
        return 3;
    }
    if (q + 2 < n && ((s[q] == '<' && s[q + 1] == '<' && s[q + 2] == '=') ||
                      (s[q] == '>' && s[q + 1] == '>' && s[q + 2] == '='))) {
        *after_out = after;
        return 3;
    }
    q = pos;
    while (q > 0 && (s[q - 1] == ' ' || s[q - 1] == '\t' ||
                     s[q - 1] == '\r' || s[q - 1] == '\n'))
        q--;
    if (q > 0) {
        if (s[q - 1] == '&' && !(q > 1 && s[q - 2] == '&') &&
            amp_is_unary(s, n, q - 1)) {
            *after_out = after;
            return 2;
        }
        /* `&(c->eax)` and `& (c->eax)`: the address-of sits before one or
         * more opening parentheses, so the immediate-token test above misses
         * it.  Walk back over parentheses and whitespace, then reuse the same
         * unary/binary discriminator. */
        if (s[q - 1] == '(') {
            size_t b = q;
            while (b > 0 && (s[b - 1] == '(' || s[b - 1] == ' ' ||
                             s[b - 1] == '\t' || s[b - 1] == '\r' ||
                             s[b - 1] == '\n'))
                b--;
            if (b > 0 && s[b - 1] == '&' &&
                !(b > 1 && s[b - 2] == '&') &&
                amp_is_unary(s, n, b - 1)) {
                *after_out = after;
                return 2;
            }
        }
        if (q > 1 && ((s[q - 2] == '+' && s[q - 1] == '+') ||
                      (s[q - 2] == '-' && s[q - 1] == '-'))) {
            *after_out = after;
            return 3;
        }
    }
    if (macro_write_before(s, n, pos)) {
        *after_out = after;
        return 1;
    }
    *after_out = after;
    return 0;
}

static int is_lifted_call(const char *name, size_t len)
{
    size_t i;
    if (len < 3 || name[0] != 'L' || name[1] != '_') return 0;
    for (i = 2; i < len; i++) {
        if (!isxdigit((unsigned char)name[i])) return 0;
    }
    return 1;
}

static int name_is(const char *name, size_t len, const char *want)
{
    return strlen(want) == len && strncmp(name, want, len) == 0;
}

static int name_has_prefix(const char *name, size_t len, const char *prefix)
{
    size_t pl = strlen(prefix);
    return len >= pl && strncmp(name, prefix, pl) == 0;
}

static int safe_call_name(const char *name, size_t len)
{
    static const char *const exact[] = {
        "rd32", "wr32", "rd16", "wr16", "rd8", "wr8", "rd64", "wr64",
        "rdss", "wrss", "wrsd", "rdsd", "rdxm", "wrxm", "rdx", "wrx",
        "push32", "pop32", "add32", "sub32", "xor32", "and32", "or32",
        "not32", "neg32", "inc32", "dec32", "shl32", "shr32", "sar32",
        "rol32", "ror32", "mul32", "imul32", "div32", "idiv32", "adc32",
        "sbb32", "bswap32", "parity8", "do_sahf", "fst", "fpop",
        "rep_stosd", "rep_movsd", "abs", "llabs", "fabs", "sqrt", "sin",
        "cos", "tan", "floor", "ceil", "trunc", "round", "lrint", "lround",
        "isnan", "isinf", "signbit", "memcmp", "mm_get", "mmx_mark",
        "x87_stack_fault", "RECOMP_TODO", "WC3_PF_VALUE", "WC3_DIRECT_TAIL",
        "abort"
    };
    static const char *const prefixes[] = {
        "op_", "flags_", "eflags_", "sse_", "xmm_", "mmx_", "mm_",
        "x87_", "fpu_", "VS_", "FS_", "R8", "R16", "SET8L", "SET8H",
        "SET16", "GVA", "STACK_BASE", "__builtin_", "__c11_", "__atomic_",
        "__sync_"
    };
    size_t i;
    for (i = 0; i < sizeof exact / sizeof exact[0]; i++) {
        if (name_is(name, len, exact[i])) return 1;
    }
    for (i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        if (name_has_prefix(name, len, prefixes[i])) return 1;
    }
    return 0;
}

static int barrier_call_name(const char *name, size_t len)
{
    if (name_has_prefix(name, len, "WC3_DIRECT_CALL")) return 1;
    if (name_has_prefix(name, len, "dispatch")) return 1;
    if (name_has_prefix(name, len, "hle_")) return 1;
    if (name_has_prefix(name, len, "wc3_")) return 1;
    if (name_has_prefix(name, len, "selection24_")) return 1;
    if (name_has_prefix(name, len, "call_")) return 1;
    if (name_has_prefix(name, len, "invoke")) return 1;
    if (name_is(name, len, "memcpy") || name_is(name, len, "memset") ||
        name_is(name, len, "memmove") ||
        name_has_prefix(name, len, "__builtin_mem"))
        return 1;
    if (is_lifted_call(name, len)) return 1;
    return 0;
}

static void collect_calls(const char *s, size_t n, NamedVec *barriers)
{
    size_t i = 0;
    while (i < n) {
        size_t start;
        size_t end;
        size_t q;
        if (!(isalpha((unsigned char)s[i]) || s[i] == '_')) {
            i++;
            continue;
        }
        start = i;
        while (i < n && word_char(s[i])) i++;
        end = i;
        q = end;
        while (q < n && (s[q] == ' ' || s[q] == '\t')) q++;
        if (q >= n || s[q] != '(') continue;
        if (name_is(s + start, end - start, "if") ||
            name_is(s + start, end - start, "for") ||
            name_is(s + start, end - start, "while") ||
            name_is(s + start, end - start, "switch") ||
            name_is(s + start, end - start, "sizeof") ||
            name_is(s + start, end - start, "return") ||
            name_is(s + start, end - start, "do") ||
            name_is(s + start, end - start, "else") ||
            name_is(s + start, end - start, "case") ||
            name_is(s + start, end - start, "default") ||
            name_is(s + start, end - start, "goto") ||
            name_is(s + start, end - start, "typeof") ||
            name_is(s + start, end - start, "_Static_assert") ||
            name_is(s + start, end - start, "_Alignof") ||
            name_is(s + start, end - start, "alignof") ||
            name_is(s + start, end - start, "__attribute__") ||
            name_is(s + start, end - start, "__declspec"))
            continue;
        if (barrier_call_name(s + start, end - start)) {
            named_push(barriers, s + start, end - start, start);
        } else if (!safe_call_name(s + start, end - start)) {
            named_push(barriers, s + start, end - start, start);
        }
    }
}

static int has_any_addr(const Events ev[8])
{
    int r;
    size_t i;
    for (r = 0; r < 8; r++) {
        for (i = 0; i < ev[r].n; i++) {
            if (ev[r].v[i].kind == 2) return 1;
        }
    }
    return 0;
}

static int line_next_is_else(const char *s, size_t n, size_t le)
{
    size_t p = le;
    while (p < n && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' ||
                     s[p] == '\n'))
        p++;
    return p + 4 <= n && strncmp(s + p, "else", 4) == 0 &&
           (p + 4 == n || !word_char(s[p + 4]));
}

/* The opening write must be a complete statement on its own line.  A direct
 * assignment starts at the first non-space character.  A SET8L/SET8H/SET16
 * macro may precede the first argument.  Two statements on one line, a label,
 * a trailing brace or any control keyword make the insertion point unsafe. */
static int simple_write_line(const char *s, size_t n, size_t write_pos, int reg)
{
    size_t ls = line_start(s, write_pos);
    size_t le = line_end(s, n, write_pos);
    size_t p = ls;
    size_t last = le;
    size_t semis = 0;
    size_t i;
    while (p < write_pos && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r'))
        p++;
    if (p < write_pos) {
        size_t q = p;
        while (q < write_pos && word_char(s[q])) q++;
        if (!name_is(s + p, q - p, "SET8L") &&
            !name_is(s + p, q - p, "SET8H") &&
            !name_is(s + p, q - p, "SET16"))
            return 0;
        while (q < write_pos && (s[q] == ' ' || s[q] == '\t' ||
                                 s[q] == '\r' || s[q] == '('))
            q++;
        if (q != write_pos) return 0;
    }
    while (last > ls && (s[last - 1] == ' ' || s[last - 1] == '\t' ||
                         s[last - 1] == '\r'))
        last--;
    if (last == ls || s[last - 1] != ';') return 0;
    for (i = ls; i < le; i++) {
        if (s[i] == ';') semis++;
        if (s[i] == '{' || s[i] == '}') return 0;
    }
    if (semis != 1) return 0;
    if (has_word(s + ls, le - ls, "if") ||
        has_word(s + ls, le - ls, "else") ||
        has_word(s + ls, le - ls, "for") ||
        has_word(s + ls, le - ls, "while") ||
        has_word(s + ls, le - ls, "switch") ||
        has_word(s + ls, le - ls, "goto") ||
        has_word(s + ls, le - ls, "return") ||
        has_word(s + ls, le - ls, "break") ||
        has_word(s + ls, le - ls, "continue") ||
        has_word(s + ls, le - ls, "case") ||
        has_word(s + ls, le - ls, "default"))
        return 0;
    (void)reg;
    return 1;
}

static void lines_push(Lines *ls, size_t start, size_t end)
{
    if (ls->n == ls->cap) {
        ls->cap = ls->cap ? ls->cap * 2 : 128;
        ls->v = (Line *)xrealloc(ls->v, ls->cap * sizeof *ls->v);
    }
    ls->v[ls->n].start = start;
    ls->v[ls->n].end = end;
    ls->n++;
}

static void build_lines(const char *s, size_t n, Lines *out)
{
    size_t p = 0;
    while (p < n) {
        size_t e = line_end(s, n, p);
        if (e < n) e++;
        lines_push(out, p, e);
        p = e;
    }
}

static void changes_add(Changes *cs, const char *func, unsigned mask,
                        size_t reads, size_t before, size_t after)
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
    ch->mask = mask;
    ch->reads = reads;
    ch->body_before = before;
    ch->body_after = after;
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

typedef struct {
    size_t at;
    size_t count;
    int valid;
} Choice;

typedef struct {
    size_t pos;
    int reg;
} Selected;

typedef struct {
    Selected *v;
    size_t n;
    size_t cap;
} SelVec;

static void sel_push(SelVec *sv, size_t pos, int reg)
{
    if (sv->n == sv->cap) {
        sv->cap = sv->cap ? sv->cap * 2 : 32;
        sv->v = (Selected *)xrealloc(sv->v, sv->cap * sizeof *sv->v);
    }
    sv->v[sv->n].pos = pos;
    sv->v[sv->n].reg = reg;
    sv->n++;
}

static int sel_cmp(const void *a, const void *b)
{
    const Selected *x = (const Selected *)a;
    const Selected *y = (const Selected *)b;
    return x->pos < y->pos ? -1 : x->pos > y->pos ? 1 : 0;
}

static int pos_in_loop(const NamedVec *labels, const NamedVec *gotos,
                       size_t pos)
{
    size_t i;
    for (i = 0; i < gotos->n; i++) {
        size_t lp = named_find(labels, gotos->v[i].name);
        size_t gp = gotos->v[i].pos;
        if (lp != SIZE_MAX && lp < gp && lp <= pos && pos <= gp) return 1;
    }
    return 0;
}

static int transform_body(const char *body, size_t n, int *out_changed,
                          unsigned *out_mask, size_t *out_reads,
                          Buf *out)
{
    char *masked;
    Events ev[8];
    NamedVec barriers = {0};
    NamedVec labels = {0};
    NamedVec gotos = {0};
    Choice chosen[8] = {{0}};
    int r;
    size_t i;
    size_t reads_total = 0;
    unsigned mask = 0;
    Lines lines = {0};
    SelVec selected = {0};
    Buf body_out = {0};

    *out_changed = 0;
    *out_mask = 0;
    *out_reads = 0;
    if (strstr(body, "_wc3_gpr_") != NULL) {
        buf_add(out, body, n);
        return 0;
    }
    if (has_word(body, n, "switch") || has_word(body, n, "while") ||
        has_word(body, n, "for") || has_word(body, n, "do") ||
        has_word(body, n, "break") || has_word(body, n, "continue")) {
        buf_add(out, body, n);
        return 0;
    }
    if (has_multiline_comment(body, n) || has_preprocessor_line(body, n) ||
        parens_cross_lines(body, n)) {
        buf_add(out, body, n);
        return 0;
    }

    masked = (char *)xmalloc(n + 1);
    mask_code(body, n, masked);
    build_lines(body, n, &lines);
    memset(ev, 0, sizeof ev);

    for (r = 0; r < 8; r++) {
        size_t p = 0;
        const char *name = reg_names[r];
        size_t nl = strlen(name);
        while (p < n) {
            const char *hit = strstr(masked + p, "c->");
            if (!hit) break;
            p = (size_t)(hit - masked);
            if (p + 3 + nl > n) break;
            if (p > 0 && word_char(masked[p - 1])) {
                p += 3;
                continue;
            }
            if (strncmp(masked + p + 3, name, nl) != 0 ||
                (p + 3 + nl < n && word_char(masked[p + 3 + nl]))) {
                p += 3;
                continue;
            }
            {
                size_t after = 0;
                int kind = classify_access(masked, n, p, r, &after);
                if (kind >= 0) events_push(&ev[r], p, kind);
                p = after ? after : p + 3 + nl;
            }
        }
    }

    /* A pointer to one guest GPR can be used to rewrite neighbouring CPU
     * fields.  Rather than model that alias, leave the whole function alone. */
    if (has_any_addr(ev)) {
        buf_add(out, body, n);
        free(masked);
        free(lines.v);
        return 0;
    }

    collect_calls(masked, n, &barriers);
    /* Generated labels are not always at the start of a line: leaf_call and
     * the XMM write-back can leave `... L_6F9A4B70:` on one line.  Scan the
     * whole masked body so a back edge to such a label is still seen. */
    for (i = 0; i + 2 < n; i++) {
        size_t q;
        if (i > 0 && word_char(masked[i - 1])) continue;
        if (masked[i] != 'L' || masked[i + 1] != '_') continue;
        q = i + 2;
        while (q < n && word_char(masked[q])) q++;
        if (q < n && masked[q] == ':')
            named_push(&labels, masked + i, q - i, i);
    }
    for (i = 0; i + 5 < n; i++) {
        if (starts_word(masked, n, i, "goto")) {
            size_t p = i + 4;
            size_t q;
            while (p < n && (masked[p] == ' ' || masked[p] == '\t')) p++;
            q = p;
            while (q < n && word_char(masked[q])) q++;
            if (q > p) named_push(&gotos, masked + p, q - p, i);
        }
    }

    for (r = 0; r < 8; r++) {
        size_t *reads;
        size_t *writes;
        size_t nr = 0;
        size_t nw = 0;
        size_t j;
        int bad = 0;
        if (!reg_allowed[r] || ev[r].n == 0) continue;
        reads = (size_t *)xmalloc(ev[r].n * sizeof *reads);
        writes = (size_t *)xmalloc(ev[r].n * sizeof *writes);
        for (j = 0; j < ev[r].n; j++) {
            if (ev[r].v[j].kind == 3) bad = 1;
            if (ev[r].v[j].kind == 0 || ev[r].v[j].kind == 3)
                reads[nr++] = ev[r].v[j].pos;
            if (ev[r].v[j].kind == 1 || ev[r].v[j].kind == 3)
                writes[nw++] = ev[r].v[j].pos;
        }
        if (bad || nr == 0) {
            free(reads);
            free(writes);
            continue;
        }

        {
            size_t best_score = 0;
            size_t best_at = 0;
            size_t *best_reads = NULL;
            size_t best_nreads = 0;
            size_t seg;
            for (seg = 0; seg <= nw; seg++) {
                size_t start;
                size_t init_line_end;
                size_t stop;
                size_t close;
                size_t at;
                size_t depth;
                size_t cur;
                size_t k;
                size_t first_barrier;
                size_t nrep = 0;
                size_t last_read = 0;
                int rejected = 0;
                if (seg == 0) {
                    start = 0;
                    init_line_end = line_end(masked, n, 0);
                    at = 1;
                    close = n;
                } else {
                    start = writes[seg - 1];
                    if (!simple_write_line(masked, n, start, r)) continue;
                    depth = 0;
                    for (k = 0; k < start; k++) {
                        if (masked[k] == '{') depth++;
                        else if (masked[k] == '}') depth--;
                    }
                    if (depth < 1) continue;
                    init_line_end = line_end(masked, n, start);
                    if (line_next_is_else(masked, n, init_line_end)) continue;
                    close = n;
                    cur = depth;
                    for (k = start; k < n; k++) {
                        if (masked[k] == '{') cur++;
                        else if (masked[k] == '}') {
                            cur--;
                            if (cur < depth) {
                                close = k;
                                break;
                            }
                        }
                    }
                    if (close <= init_line_end) continue;
                    at = line_number(masked, start) + 1;
                }
                if (at >= lines.n) continue;
                stop = seg < nw ? writes[seg] : n;
                for (k = 0; k < nr; k++) {
                    size_t rp = reads[k];
                    if (seg == 0) {
                        if (rp > init_line_end && rp < stop) nrep++;
                    } else if (rp > init_line_end && rp < stop && rp < close) {
                        nrep++;
                    }
                }
                if (nrep == 0) continue;
                {
                    size_t *rep = (size_t *)xmalloc(nrep * sizeof *rep);
                    size_t ri = 0;
                    for (k = 0; k < nr; k++) {
                        size_t rp = reads[k];
                        if (seg == 0) {
                            if (rp > init_line_end && rp < stop) rep[ri++] = rp;
                        } else if (rp > init_line_end && rp < stop && rp < close) {
                            rep[ri++] = rp;
                        }
                    }
                    first_barrier = SIZE_MAX;
                    for (k = 0; k < barriers.n; k++) {
                        size_t bp = barriers.v[k].pos;
                        if (bp > init_line_end && bp <= rep[ri - 1]) {
                            first_barrier = bp;
                            break;
                        }
                    }
                    if (first_barrier != SIZE_MAX) {
                        size_t keep = 0;
                        for (k = 0; k < ri; k++) {
                            if (rep[k] < first_barrier) rep[keep++] = rep[k];
                        }
                        ri = keep;
                    }
                    if (ri == 0) {
                        free(rep);
                        continue;
                    }
                    last_read = rep[ri - 1];
                    /* A forward goto from before the init may skip the
                     * declaration.  A target inside the cached interval is
                     * always invalid.  A target after the interval is safe
                     * unless it is inside a loop, where a back edge can bring
                     * control back to a replaced read. */
                    for (k = 0; k < gotos.n; k++) {
                        size_t gpos = gotos.v[k].pos;
                        size_t lp;
                        if (gpos >= start) continue;
                        lp = named_find(&labels, gotos.v[k].name);
                        if (lp == SIZE_MAX ||
                            (lp > start && lp <= last_read) ||
                            (lp > last_read &&
                             pos_in_loop(&labels, &gotos, lp))) {
                            rejected = 1;
                            break;
                        }
                    }
                    if (rejected) {
                        free(rep);
                        continue;
                    }
                    /* An interval initialized outside a loop cannot survive a
                     * write or call inside that loop.  An interval whose init
                     * is inside the loop is re-initialized by every back edge,
                     * and a forward jump into the loop past the init was
                     * rejected above. */
                    for (k = 0; k < gotos.n && !rejected; k++) {
                        size_t gp = gotos.v[k].pos;
                        size_t lp = named_find(&labels, gotos.v[k].name);
                        size_t bi;
                        int any_read_in_loop = 0;
                        int any_clobber = 0;
                        if (lp == SIZE_MAX || lp >= gp) continue;
                        if (seg != 0 && start >= lp && start <= gp) continue;
                        if (start > gp) continue;
                        for (bi = 0; bi < ri; bi++) {
                            if (rep[bi] >= lp && rep[bi] <= gp) {
                                any_read_in_loop = 1;
                                break;
                            }
                        }
                        if (!any_read_in_loop) continue;
                        for (bi = 0; bi < nw; bi++) {
                            if (writes[bi] >= lp && writes[bi] <= gp) {
                                any_clobber = 1;
                                break;
                            }
                        }
                        if (!any_clobber) {
                            for (bi = 0; bi < barriers.n; bi++) {
                                if (barriers.v[bi].pos >= lp &&
                                    barriers.v[bi].pos <= gp) {
                                    any_clobber = 1;
                                    break;
                                }
                            }
                        }
                        if (any_clobber) rejected = 1;
                    }
                    if (rejected) {
                        free(rep);
                        continue;
                    }
                    if (ri > best_score) {
                        free(best_reads);
                        best_reads = rep;
                        best_nreads = ri;
                        best_score = ri;
                        best_at = at;
                    } else {
                        free(rep);
                    }
                }
            }
            if (best_reads) {
                chosen[r].valid = 1;
                chosen[r].at = best_at;
                chosen[r].count = best_nreads;
                for (j = 0; j < best_nreads; j++)
                    sel_push(&selected, best_reads[j], r);
                reads_total += best_nreads;
                mask |= 1u << r;
                free(best_reads);
            }
        }
        free(reads);
        free(writes);
    }

    if (selected.n == 0) {
        buf_add(out, body, n);
        free(masked);
        free(barriers.v);
        free(labels.v);
        free(gotos.v);
        free(lines.v);
        free(selected.v);
        return 0;
    }
    qsort(selected.v, selected.n, sizeof selected.v[0], sel_cmp);
    {
        size_t li;
        size_t si = 0;
        for (li = 0; li < lines.n; li++) {
            size_t p = lines.v[li].start;
            size_t e = lines.v[li].end;
            while (si < selected.n && selected.v[si].pos < p) si++;
            while (p < e) {
                if (si < selected.n && selected.v[si].pos == p) {
                    buf_str(&body_out, "_wc3_gpr_");
                    buf_str(&body_out, reg_names[selected.v[si].reg]);
                    p += 3 + strlen(reg_names[selected.v[si].reg]);
                    si++;
                } else {
                    buf_add(&body_out, &body[p], 1);
                    p++;
                }
            }
            /* Insert declarations after the selected line. */
            for (r = 0; r < 8; r++) {
                if (chosen[r].valid && chosen[r].at == li + 1) {
                    size_t indent = lines.v[li].start;
                    while (indent < lines.v[li].end &&
                           (body[indent] == ' ' || body[indent] == '\t')) {
                        buf_add(&body_out, &body[indent], 1);
                        indent++;
                    }
                    if (indent == lines.v[li].start && li == 0)
                        buf_str(&body_out, "    ");
                    buf_printf(&body_out, "uint32_t _wc3_gpr_%s = c->%s;\n",
                               reg_names[r], reg_names[r]);
                }
            }
        }
    }
    if (body_out.n == n && memcmp(body_out.p, body, n) == 0) {
        buf_add(out, body, n);
        *out_changed = 0;
        free(body_out.p);
    } else {
        buf_add(out, body_out.p, body_out.n);
        *out_changed = 1;
        *out_mask = mask;
        *out_reads = reads_total;
        free(body_out.p);
    }
    free(masked);
    free(barriers.v);
    free(labels.v);
    free(gotos.v);
    free(lines.v);
    free(selected.v);
    return *out_changed;
}

static size_t find_function(const char *s, size_t n, size_t from,
                            size_t *name_start, size_t *name_end,
                            size_t *header_end)
{
    size_t i;
    for (i = from; i + 5 < n; i++) {
        size_t p;
        size_t ns;
        size_t ne;
        if (i != 0 && s[i - 1] != '\n') continue;
        if (strncmp(s + i, "void ", 5) != 0) continue;
        ns = i + 5;
        ne = ns;
        while (ne < n && word_char(s[ne])) ne++;
        if (ne == ns) continue;
        p = ne;
        while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
        if (p >= n || s[p] != '(') continue;
        p++;
        while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
        if (p + 3 > n || strncmp(s + p, "CPU", 3) != 0) continue;
        p += 3;
        while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
        if (p >= n || s[p] != '*') continue;
        p++;
        while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
        if (p + 8 > n || strncmp(s + p, "restrict", 8) != 0) continue;
        p += 8;
        if (p >= n || !(s[p] == ' ' || s[p] == '\t')) continue;
        while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
        if (p >= n || s[p] != 'c') continue;
        p++;
        while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
        if (p >= n || s[p] != ')') continue;
        p++;
        while (p < n && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')) p++;
        if (p >= n || s[p] != '\n') continue;
        *name_start = ns;
        *name_end = ne;
        *header_end = p + 1;
        return i;
    }
    return SIZE_MAX;
}

static size_t find_open_brace(const char *s, size_t n, size_t from)
{
    size_t i = from;
    while (i < n) {
        if (s[i] == '{') return i;
        if (s[i] == ';' || s[i] == '}') return SIZE_MAX;
        i++;
    }
    return SIZE_MAX;
}

static void transform_text(const char *text, size_t n, Buf *out,
                           size_t *funcs, size_t *changed, Changes *cs)
{
    size_t last = 0;
    size_t scan = 0;
    *funcs = 0;
    *changed = 0;
    while (scan < n) {
        size_t name_start;
        size_t name_end;
        size_t header_end;
        size_t hdr = find_function(text, n, scan, &name_start, &name_end,
                                   &header_end);
        size_t open;
        size_t close;
        size_t blen;
        char fname[128];
        size_t flen;
        size_t before = out->n;
        int ch = 0;
        unsigned mask = 0;
        size_t reads = 0;
        if (hdr == SIZE_MAX) break;
        open = find_open_brace(text, n, header_end);
        if (open == SIZE_MAX) {
            scan = header_end;
            continue;
        }
        close = matching_brace(text, n, open);
        if (close == SIZE_MAX) break;
        (*funcs)++;
        buf_add(out, text + last, open - last);
        blen = close - open + 1;
        flen = name_end - name_start;
        if (flen >= sizeof fname) flen = sizeof fname - 1;
        memcpy(fname, text + name_start, flen);
        fname[flen] = 0;
        transform_body(text + open, blen, &ch, &mask, &reads, out);
        if (ch) {
            changes_add(cs, fname, mask, reads, blen, out->n - before);
            (*changed)++;
        }
        last = close + 1;
        scan = close + 1;
    }
    buf_add(out, text + last, n - last);
}

static void write_report(const char *path, size_t funcs, size_t changed,
                         size_t before, size_t after, const Changes *cs)
{
    FILE *f = fopen(path, "wb");
    size_t i;
    if (!f) die("cannot write json report");
    fprintf(f,
            "{\"tool\":\"gpr_local\",\"functions\":%zu,\"transformed\":%zu,"
            "\"bytes_before\":%zu,\"bytes_after\":%zu,\"changes\":[",
            funcs, changed, before, after);
    for (i = 0; i < cs->n; i++) {
        const Change *ch = &cs->v[i];
        if (i) fputc(',', f);
        fprintf(f,
                "{\"function\":\"%s\",\"gpr_mask\":%u,"
                "\"reads_replaced\":%zu,\"body_before\":%zu,"
                "\"body_after\":%zu}",
                ch->func, ch->mask, ch->reads, ch->body_before,
                ch->body_after);
    }
    fputs("]}\n", f);
    if (fclose(f) != 0) die("cannot close json report");
}

static int check_contains(const char *name, const char *text,
                          const char *needle)
{
    if (!strstr(text, needle)) {
        fprintf(stderr, "gpr_local self-test: %s: missing %s\n", name, needle);
        return 0;
    }
    return 1;
}

static int check_not_contains(const char *name, const char *text,
                              const char *needle)
{
    if (strstr(text, needle)) {
        fprintf(stderr, "gpr_local self-test: %s: unexpected %s\n", name,
                needle);
        return 0;
    }
    return 1;
}

static size_t count_substr(const char *text, const char *needle)
{
    size_t c = 0;
    const char *p = text;
    while ((p = strstr(p, needle)) != NULL) {
        c++;
        p++;
    }
    return c;
}

static int self_test(void)
{
    static const char *read_only =
        "void L_00000001(CPU *restrict c)\n"
        "{\n"
        "    c->eax = rd32(c->ecx);\n"
        "    c->edx = c->ecx + 1;\n"
        "    c->ebx = c->ecx;\n"
        "}\n";
    static const char *brace_code =
        "void L_00000013(CPU *restrict c)\n"
        "{ c->eax = rd32(c->ecx);\n"
        "    c->edx = c->ecx;\n"
        "}\n";
    static const char *prologue =
        "void L_00000002(CPU *restrict c)\n"
        "{\n"
        "    push32(c, c->ebp);\n"
        "    c->ebp = c->esp;\n"
        "    c->eax = rd32(c->ebp + 8);\n"
        "    c->edx = rd32(c->ebp + 12);\n"
        "    c->ebp = pop32(c);\n"
        "}\n";
    static const char *call_prefix =
        "void L_00000003(CPU *restrict c)\n"
        "{\n"
        "    c->eax = rd32(c->ecx);\n"
        "    c->edx = c->ecx;\n"
        "    WC3_DIRECT_CALL_CLEAN(L_00000004, c);\n"
        "    c->ebx = c->ecx;\n"
        "}\n";
    static const char *loop_write =
        "void L_00000005(CPU *restrict c)\n"
        "{\n"
        "    c->eax = 1;\n"
        "L_00000006:\n"
        "    c->edx = c->ecx;\n"
        "    c->ecx = c->ecx + 1;\n"
        "    goto L_00000006;\n"
        "}\n";
    static const char *inline_label_loop =
        "void L_00000010(CPU *restrict c)\n"
        "{\n"
        "    c->eax = 1; L_00000011:\n"
        "    c->edx = c->ecx;\n"
        "    c->ecx = c->ecx + 1;\n"
        "    goto L_00000011;\n"
        "}\n";
    static const char *goto_skip =
        "void L_00000007(CPU *restrict c)\n"
        "{\n"
        "    goto L_00000008;\n"
        "    c->ecx = 2;\n"
        "L_00000008:\n"
        "    c->edx = c->ecx;\n"
        "}\n";
    static const char *else_split =
        "void L_00000009(CPU *restrict c)\n"
        "{\n"
        "    c->eax = 1;\n"
        "    if (c->edx)\n"
        "        c->ecx = 2;\n"
        "    else\n"
        "        c->eax = 3;\n"
        "    c->edx = c->ecx;\n"
        "}\n";
    static const char *set8l =
        "void L_0000000A(CPU *restrict c)\n"
        "{\n"
        "    SET8L(c->ecx, 2);\n"
        "    c->edx = c->ecx;\n"
        "    c->ebx = c->ecx;\n"
        "}\n";
    static const char *unknown_call =
        "void L_0000000B(CPU *restrict c)\n"
        "{\n"
        "    c->eax = rd32(c->ecx);\n"
        "    foo(c);\n"
        "    c->edx = c->ecx;\n"
        "}\n";
    static const char *addr_taken =
        "void L_0000000C(CPU *restrict c)\n"
        "{\n"
        "    c->eax = rd32(c->ecx);\n"
        "    memcpy(&c->ecx, &x, 4);\n"
        "    c->edx = c->ecx;\n"
        "}\n";
    static const char *cast_addr_taken =
        "void L_0000000D(CPU *restrict c)\n"
        "{\n"
        "    c->eax = rd32(c->ecx);\n"
        "    foo((uint32_t)&c->eax);\n"
        "    c->edx = c->ecx;\n"
        "}\n";
    static const char *preprocessor =
        "void L_00000016(CPU *restrict c)\n"
        "{\n"
        "    c->eax = rd32(c->ecx);\n"
        "    /* selection24 evt2 trace */\n"
        "#if WC3_DEBUG && WC3_DEBUG_SELECT24\n"
        "    wc3_sel24_trace_evt2(c, c->ecx, c->ebp);\n"
        "#endif\n"
        "    c->edx = c->ecx;\n"
        "}\n";
    static const char *esp_only =
        "void L_00000014(CPU *restrict c)\n"
        "{\n"
        "    c->eax = c->esp;\n"
        "    c->edx = c->esp;\n"
        "}\n";
    static const char *all_regs =
        "void L_00000015(CPU *restrict c)\n"
        "{\n"
        "    c->eax = rd32(c->ecx);\n"
        "    c->ebx = c->eax;\n"
        "    c->edx = c->ebx;\n"
        "    c->esi = c->edx;\n"
        "    c->edi = c->esi;\n"
        "    c->ebp = c->edi;\n"
        "    c->eax = c->ebp;\n"
        "}\n";
    static const char *bitwise_and =
        "void L_0000000E(CPU *restrict c)\n"
        "{\n"
        "    c->eax = rd32(c->ecx);\n"
        "    flags_logicz(c, c->eax & c->eax, 4);\n"
        "    c->edx = c->ecx;\n"
        "}\n";
    static const char *paren_bitwise_and =
        "void L_0000000F(CPU *restrict c)\n"
        "{\n"
        "    c->eax = rd32(c->ecx);\n"
        "    flags_logicz(c, (c->eax) & c->eax, 4);\n"
        "    c->edx = c->ecx;\n"
        "}\n";
    static const char *paren_addr_taken =
        "void L_00000012(CPU *restrict c)\n"
        "{\n"
        "    c->eax = rd32(c->ecx);\n"
        "    foo(&(c->eax));\n"
        "    c->edx = c->ecx;\n"
        "}\n";
    Buf out = {0};
    Changes cs = {0};
    size_t funcs;
    size_t changed;
    int ok = 1;

    transform_text(read_only, strlen(read_only), &out, &funcs, &changed, &cs);
    ok &= funcs == 1 && changed == 1;
    ok &= cs.n == 1 && cs.v[0].mask == 2u && cs.v[0].reads == 3;
    ok &= check_contains("read_only", out.p,
                         "uint32_t _wc3_gpr_ecx = c->ecx;");
    ok &= check_contains("read_only", out.p, "c->eax = rd32(_wc3_gpr_ecx);");
    ok &= check_contains("read_only", out.p,
                         "c->edx = _wc3_gpr_ecx + 1;");
    ok &= check_not_contains("read_only", out.p, "_wc3_gpr_eax");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    transform_text(brace_code, strlen(brace_code), &out, &funcs, &changed,
                   &cs);
    ok &= funcs == 1 && changed == 1;
    ok &= check_contains("brace_code", out.p, "c->eax = rd32(c->ecx);");
    ok &= check_contains("brace_code", out.p, "c->edx = _wc3_gpr_ecx;");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    transform_text(prologue, strlen(prologue), &out, &funcs, &changed, &cs);
    ok &= funcs == 1 && changed == 1;
    ok &= check_contains("prologue", out.p,
                         "uint32_t _wc3_gpr_ebp = c->ebp;");
    ok &= check_contains("prologue", out.p,
                         "c->eax = rd32(_wc3_gpr_ebp + 8);");
    ok &= check_contains("prologue", out.p, "c->ebp = pop32(c);");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    transform_text(call_prefix, strlen(call_prefix), &out, &funcs, &changed,
                   &cs);
    ok &= funcs == 1 && changed == 1;
    ok &= count_substr(out.p, "_wc3_gpr_ecx") == 3;
    ok &= check_contains("call_prefix", out.p, "c->ebx = c->ecx;");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    transform_text(loop_write, strlen(loop_write), &out, &funcs, &changed,
                   &cs);
    ok &= funcs == 1 && changed == 0;
    ok &= check_not_contains("loop_write", out.p, "_wc3_gpr_");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    transform_text(inline_label_loop, strlen(inline_label_loop), &out, &funcs,
                   &changed, &cs);
    ok &= funcs == 1 && changed == 0;
    ok &= check_not_contains("inline_label_loop", out.p, "_wc3_gpr_");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    transform_text(goto_skip, strlen(goto_skip), &out, &funcs, &changed, &cs);
    ok &= funcs == 1 && changed == 0;
    ok &= check_not_contains("goto_skip", out.p, "_wc3_gpr_ecx");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    transform_text(else_split, strlen(else_split), &out, &funcs, &changed,
                   &cs);
    ok &= funcs == 1;
    ok &= check_not_contains("else_split", out.p, "_wc3_gpr_ecx");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    transform_text(set8l, strlen(set8l), &out, &funcs, &changed, &cs);
    ok &= funcs == 1 && changed == 1;
    ok &= check_contains("set8l", out.p,
                         "uint32_t _wc3_gpr_ecx = c->ecx;");
    ok &= check_contains("set8l", out.p, "c->edx = _wc3_gpr_ecx;");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    transform_text(unknown_call, strlen(unknown_call), &out, &funcs, &changed,
                   &cs);
    ok &= funcs == 1 && changed == 1;
    ok &= count_substr(out.p, "_wc3_gpr_ecx") == 2;
    ok &= check_contains("unknown_call", out.p, "c->edx = c->ecx;");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    transform_text(addr_taken, strlen(addr_taken), &out, &funcs, &changed,
                   &cs);
    ok &= funcs == 1 && changed == 0;
    ok &= check_not_contains("addr_taken", out.p, "_wc3_gpr_");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    transform_text(cast_addr_taken, strlen(cast_addr_taken), &out, &funcs,
                   &changed, &cs);
    ok &= funcs == 1 && changed == 0;
    ok &= check_not_contains("cast_addr_taken", out.p, "_wc3_gpr_");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    transform_text(preprocessor, strlen(preprocessor), &out, &funcs, &changed,
                   &cs);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(preprocessor) &&
          memcmp(out.p, preprocessor, out.n) == 0;
    ok &= check_not_contains("preprocessor", out.p, "_wc3_gpr_");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    transform_text(esp_only, strlen(esp_only), &out, &funcs, &changed, &cs);
    ok &= funcs == 1 && changed == 0;
    ok &= check_not_contains("esp_only", out.p, "_wc3_gpr_");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    transform_text(all_regs, strlen(all_regs), &out, &funcs, &changed, &cs);
    ok &= funcs == 1 && changed == 1;
    ok &= cs.n == 1 && cs.v[0].mask == 0xEFu && cs.v[0].reads == 7;
    ok &= check_contains("all_regs", out.p, "_wc3_gpr_eax");
    ok &= check_contains("all_regs", out.p, "_wc3_gpr_ebx");
    ok &= check_contains("all_regs", out.p, "_wc3_gpr_ecx");
    ok &= check_not_contains("all_regs", out.p, "_wc3_gpr_esp");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    transform_text(bitwise_and, strlen(bitwise_and), &out, &funcs, &changed,
                   &cs);
    ok &= funcs == 1 && changed == 1;
    ok &= check_contains("bitwise_and", out.p,
                         "uint32_t _wc3_gpr_ecx = c->ecx;");
    ok &= check_contains("bitwise_and", out.p,
                         "uint32_t _wc3_gpr_eax = c->eax;");
    ok &= check_contains("bitwise_and", out.p,
                         "flags_logicz(c, _wc3_gpr_eax & _wc3_gpr_eax, 4);");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    transform_text(paren_bitwise_and, strlen(paren_bitwise_and), &out, &funcs,
                   &changed, &cs);
    ok &= funcs == 1 && changed == 1;
    ok &= check_contains("paren_bitwise_and", out.p,
                         "uint32_t _wc3_gpr_ecx = c->ecx;");
    ok &= check_contains("paren_bitwise_and", out.p,
                         "flags_logicz(c, (_wc3_gpr_eax) & _wc3_gpr_eax, 4);");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    transform_text(paren_addr_taken, strlen(paren_addr_taken), &out, &funcs,
                   &changed, &cs);
    ok &= funcs == 1 && changed == 0;
    ok &= check_not_contains("paren_addr_taken", out.p, "_wc3_gpr_");
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    /* Second pass: the marker line makes the function ineligible, and the
     * output must be byte-identical. */
    transform_text(read_only, strlen(read_only), &out, &funcs, &changed, &cs);
    {
        Buf again = {0};
        size_t funcs2 = 0;
        size_t changed2 = 0;
        transform_text(out.p, out.n, &again, &funcs2, &changed2, &cs);
        ok &= funcs2 == 1 && changed2 == 0;
        ok &= again.n == out.n && memcmp(again.p, out.p, out.n) == 0;
        free(again.p);
    }
    free(out.p); out.p = NULL; out.n = out.cap = 0;
    changes_free(&cs);

    if (!ok) {
        fputs("gpr_local self-test: FAILED\n", stderr);
        return 1;
    }
    puts("gpr_local self-test: OK");
    return 0;
}

static void usage(void)
{
    fputs("gpr_local: use --lift IN --out OUT [--json REPORT] [--quiet]\n"
          "           or --self-test\n", stderr);
}

int main(int argc, char **argv)
{
    const char *in_path = NULL;
    const char *out_path = NULL;
    const char *json_path = NULL;
    int quiet = 0;
    int i;
    char *text;
    size_t n;
    Buf out = {0};
    Changes cs = {0};
    size_t funcs;
    size_t changed;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--self-test") == 0) return self_test();
        if (strcmp(argv[i], "--lift") == 0 && i + 1 < argc) {
            in_path = argv[++i];
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
        if (strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
            continue;
        }
        usage();
        return 2;
    }
    if (!in_path || !out_path) {
        usage();
        return 2;
    }
    text = slurp(in_path, &n);
    if (!text) die("cannot read input");
    transform_text(text, n, &out, &funcs, &changed, &cs);
    if (write_all(out_path, out.p ? out.p : "", out.n) != 0)
        die("cannot write output");
    if (json_path)
        write_report(json_path, funcs, changed, n, out.n, &cs);
    if (!quiet) {
        printf("gpr_local: functions=%zu transformed=%zu bytes=%zu->%zu\n",
               funcs, changed, n, out.n);
    }
    free(text);
    free(out.p);
    changes_free(&cs);
    return 0;
}
