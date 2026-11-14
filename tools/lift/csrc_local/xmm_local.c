/* xmm_local.c - function-local XMM promotion.
 *
 * The generated C keeps every XMM register in CPU.xmm, even when a function
 * body can keep them in a local XMM[8] across long straight-line stretches.
 * This pass promotes the eight guest XMM registers to a local array, loads
 * them only at dominating points, and writes back only the registers that
 * were actually modified.
 *
 * The cache is kept across ordinary if/else/brace merge points.  XMM state is
 * only observed at a guest call, a label (a join point), a goto (before control
 * leaves the current path), or a function exit.  Conditional exits are safe:
 * the exit flushes the current path, while the fall-through path keeps its own
 * local values.  A label is the canonical join: it flushes and invalidates the
 * cache, so a fall-through path arrives with c->xmm current; a path arriving by
 * goto has already written its state before the jump.  The same applies to a
 * guest call.  Tail calls keep the generated `return;` immediately after the macro;
 * the return is skipped by the flush logic because the tail already made
 * c->xmm current.
 *
 * Loads happen only at top-level statements that dominate the code using
 * them: the first top-level statement of a function and the first top-level
 * statement after a call or label.  All used registers are loaded together.
 * A load inside a braced branch would not execute when the branch is skipped,
 * while a static "loaded" bit would still be set at the merge; loading only at
 * dominating points avoids that whole class of stale-local bugs.  Calls and
 * labels are restricted to brace depth zero for the same reason.
 *
 * A goto flushes dirty locals but does not clear them: the fall-through path
 * may still need its own values, and a path that skips a later call must have
 * already written its state before jumping.  A top-level return or tail clears
 * the cache after writing it; a conditional exit keeps it for fall-through.
 *
 * Eligibility:
 *   - no switch/while/for/do/break/continue;
 *   - an unbraced if body is accepted only in the generated
 *     `if (cond) goto label;` form, where the transform can add braces around
 *     the writeback and keep the jump conditional; unbraced else bodies are
 *     accepted only as `else if` or a braced block;
 *   - no wc3_guest_* helper (it can observe/alter state outside the XMM
 *     model); WC3_GUEST_DRAIN is treated as a call;
 *   - guest calls are accepted only at brace depth zero, at a statement
 *     boundary and outside parentheses, where the cache can be flushed and
 *     invalidated on every path that reaches them;
 *   - labels are accepted only at brace depth zero;
 *   - every dispatch_jmp/WC3_DIRECT_TAIL is at a statement boundary and is
 *     followed by the generated `return;` (no code may run between);
 *   - only constant c->xmm[0..7] accesses (no whole-array/dynamic access);
 *   - at least three XMM accesses, so tiny functions are left alone.
 *
 * The JSON report carries a function-level change set (ABI-11.2): one row per
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

/* Parse one canonical c->xmm[N] access at i.  Returns the index and stores
 * the first byte after the closing bracket in *end, or -1 when the access is
 * dynamic, out of range or not a standalone c->xmm token. */
static int parse_xmm_access(const char *s, size_t n, size_t i, size_t *end)
{
    size_t q;
    size_t value = 0;
    int digits = 0;
    if (i + 7 > n || strncmp(s + i, "c->xmm[", 7) != 0) return -1;
    if (i > 0 && word_char(s[i - 1])) return -1;
    q = i + 7;
    while (q < n && isdigit((unsigned char)s[q])) {
        value = value * 10 + (size_t)(s[q] - '0');
        if (value > 7) return -1;
        digits = 1;
        q++;
    }
    if (!digits || q >= n || s[q] != ']') return -1;
    *end = q + 1;
    return (int)value;
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

/* A generated label is L_<hex>: at the beginning of a line. */
static int label_at(const char *s, size_t n, size_t i, size_t end)
{
    size_t q;
    if (i + 2 >= end || s[i] != 'L' || s[i + 1] != '_') return 0;
    q = i + 2;
    while (q < end && isxdigit((unsigned char)s[q])) q++;
    (void)n;
    return q < end && s[q] == ':';
}

/* True when the last non-space token before p is `else`.  An `if` on the
 * next line then belongs to `else if`; inserting loads between the two would
 * break the grammar. */
static int preceded_by_else(const char *s, size_t p)
{
    size_t q = p;
    while (q > 0 && (s[q - 1] == ' ' || s[q - 1] == '\t' ||
                     s[q - 1] == '\r' || s[q - 1] == '\n')) q--;
    if (q < 4) return 0;
    if (strncmp(s + q - 4, "else", 4) != 0) return 0;
    if (q - 4 > 0 && word_char(s[q - 5])) return 0;
    return 1;
}

static int is_call_at(const char *s, size_t n, size_t i)
{
    size_t p;
    if (starts_word(s, n, i, "WC3_DIRECT_CALL") ||
        starts_word(s, n, i, "WC3_DIRECT_CALL_LEAF") ||
        starts_word(s, n, i, "WC3_GUEST_DRAIN"))
        return 1;
    if (starts_word(s, n, i, "dispatch")) {
        p = i + 8;
        while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
        return p < n && s[p] == '(';
    }
    if (starts_word(s, n, i, "dispatch_common")) {
        p = i + 15;
        while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
        return p < n && s[p] == '(';
    }
    return 0;
}

/* A guest call inside an expression cannot be surrounded by flush
 * statements.  Reject those functions instead of leaving a stale cache. */
static int has_call_in_parens(const char *s, size_t n)
{
    size_t i;
    int depth = 0;
    for (i = 0; i < n; i++) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') {
            if (depth > 0) depth--;
        } else if (depth > 0 && is_call_at(s, n, i)) {
            return 1;
        }
    }
    return 0;
}

/* A colon is a statement boundary only when it terminates a line-start label.
 * A ternary or bitfield colon before a call/tail would otherwise let the
 * flush be inserted into an expression. */
static int label_colon_before(const char *s, size_t colon)
{
    size_t q = colon;
    while (q > 0 && isxdigit((unsigned char)s[q - 1])) q--;
    if (q < 2 || s[q - 1] != '_' || s[q - 2] != 'L') return 0;
    q -= 2;
    while (q > 0 && (s[q - 1] == ' ' || s[q - 1] == '\t' ||
                     s[q - 1] == '\r'))
        q--;
    return q == 0 || s[q - 1] == '\n';
}

/* True when only whitespace separates i from a statement boundary.  A call or
 * a tail must start a statement, otherwise the flush statement would be
 * inserted in the middle of an expression.  A line-start label colon is
 * accepted because a label already invalidates the cache. */
static int statement_boundary_before(const char *s, size_t i)
{
    size_t p = i;
    while (p > 0 && (s[p - 1] == ' ' || s[p - 1] == '\t' ||
                     s[p - 1] == '\r' || s[p - 1] == '\n'))
        p--;
    if (p == 0) return 1;
    if (s[p - 1] == ':') return label_colon_before(s, p - 1);
    return s[p - 1] == ';' || s[p - 1] == '{' || s[p - 1] == '}';
}

/* Guest calls are only safe at brace depth zero: a call inside a branch would
 * be reached on one side of a merge, and a path that skips it would keep stale
 * locals after the static cache was invalidated.  The statement-boundary check
 * also rejects `return dispatch(...)` and `x = dispatch(...)`, where inserting
 * a flush statement would not even be legal. */
static int calls_are_top_level(const char *s, size_t n)
{
    size_t i;
    int depth = 0;
    for (i = 1; i < n; i++) {
        if (s[i] == '{') {
            depth++;
        } else if (s[i] == '}') {
            if (depth > 0) depth--;
        } else if (is_call_at(s, n, i)) {
            if (depth != 0) return 0;
            if (!statement_boundary_before(s, i)) return 0;
        }
    }
    return 1;
}

static int label_is_line_start(const char *s, size_t i)
{
    while (i > 0 && s[i - 1] != '\n') {
        if (s[i - 1] != ' ' && s[i - 1] != '\t' && s[i - 1] != '\r')
            return 0;
        i--;
    }
    return 1;
}

/* A label inside a braced branch is skipped on the other side of the branch.
 * Flushing and invalidating there would clear the static state for a path that
 * never executed the label and still owns dirty locals, so such functions are
 * left unchanged.  Mid-line labels are rejected too: the transform only knows
 * how to flush before a line-start label. */
static int labels_are_top_level(const char *s, size_t n)
{
    size_t i;
    int depth = 0;
    for (i = 1; i < n; i++) {
        if (s[i] == '{') {
            depth++;
        } else if (s[i] == '}') {
            if (depth > 0) depth--;
        } else if (s[i] == 'L' && label_at(s, n, i, n)) {
            if (!label_is_line_start(s, i)) return 0;
            if (depth != 0) return 0;
        }
    }
    return 1;
}

/* Mark the registers whose local copy is written by this access.  A missed
 * write would lose guest state, so address-taking and all assignment forms
 * are treated as writes; a false positive only costs one store. */
static int xmm_write_access(const char *s, size_t n, size_t i, size_t end)
{
    size_t p = i;
    while (p > 0 && (s[p - 1] == ' ' || s[p - 1] == '\t')) p--;
    if (p > 0 && s[p - 1] == '&') return 1;
    /* Prefix ++/-- also write the lvalue. */
    if (p >= 2 && ((s[p - 1] == '+' && s[p - 2] == '+') ||
                   (s[p - 1] == '-' && s[p - 2] == '-')))
        return 1;
    p = end;
    while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
    if (p >= n) return 0;
    if (s[p] == '=') {
        if (p + 1 < n && s[p + 1] == '=') return 0;
        return 1;
    }
    if ((s[p] == '+' || s[p] == '-' || s[p] == '*' || s[p] == '/' ||
         s[p] == '%' || s[p] == '&' || s[p] == '|' || s[p] == '^' ||
         s[p] == '<' || s[p] == '>') && p + 1 < n && s[p + 1] == '=') {
        if (s[p] == '<' || s[p] == '>') return 0;
        return 1;
    }
    if ((s[p] == '+' && p + 1 < n && s[p + 1] == '+') ||
        (s[p] == '-' && p + 1 < n && s[p + 1] == '-'))
        return 1;
    if (s[p] == ')') {
        /* A parenthesized access can still be the base of a member lvalue,
         * `(c->xmm[0]).f32[0] = ...`.  Treat that rare form as a write; a
         * false positive only costs one extra store. */
        size_t r = p + 1;
        while (r < n && (s[r] == ' ' || s[r] == '\t')) r++;
        if (r < n && (s[r] == '.' ||
                      (s[r] == '-' && r + 1 < n && s[r + 1] == '>')))
            return 1;
    }
    if (s[p] == '.' || (s[p] == '-' && p + 1 < n && s[p + 1] == '>')) {
        size_t q = p;
        int depth = 0;
        while (q < n) {
            char ch = s[q];
            if (ch == '(' || ch == '[') depth++;
            else if (ch == ')' || ch == ']') {
                if (depth == 0) break;
                depth--;
            } else if (depth == 0) {
                if (ch == ';' || ch == ',' || ch == '}' || ch == '?' ||
                    ch == ':')
                    break;
                if (ch == '=') {
                    char prev = q > 0 ? s[q - 1] : 0;
                    if (prev == '=' || prev == '!' || prev == '<' ||
                        prev == '>') {
                        q++;
                        continue;
                    }
                    if (q + 1 < n && s[q + 1] == '=') {
                        q += 2;
                        continue;
                    }
                    return 1;
                }
                if (((ch == '<' && q + 2 < n && s[q + 1] == '<' &&
                      s[q + 2] == '=') ||
                     (ch == '>' && q + 2 < n && s[q + 1] == '>' &&
                      s[q + 2] == '=')))
                    return 1;
                if ((ch == '+' || ch == '-' || ch == '*' || ch == '/' ||
                     ch == '%' || ch == '&' || ch == '|' || ch == '^') &&
                    q + 1 < n && s[q + 1] == '=')
                    return 1;
                if ((ch == '+' && q + 1 < n && s[q + 1] == '+') ||
                    (ch == '-' && q + 1 < n && s[q + 1] == '-'))
                    return 1;
            }
            q++;
        }
    }
    return 0;
}

/* Recognize the dominant generated form `if (cond) goto L;` when it starts a
 * line.  The transform wraps the goto body in braces so the flush stays
 * conditional and does not mark the following statements unreachable.  Other
 * unbraced bodies are rejected: the transform cannot wrap return/tail/call
 * forms without changing their semantics. */
static int if_body_is_goto(const char *s, size_t n, size_t i)
{
    size_t p = i + 2;
    size_t q;
    int depth = 0;
    if (!label_is_line_start(s, i)) return 0;
    while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
    if (p >= n || s[p] != '(') return 0;
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
    if (depth != 0) return 0;
    while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
    if (!starts_word(s, n, p, "goto")) return 0;
    q = p + 4;
    depth = 0;
    while (q < n) {
        if (s[q] == '\n') return 0;
        if (s[q] == '(') depth++;
        else if (s[q] == ')') {
            if (depth > 0) depth--;
        } else if (s[q] == ';' && depth == 0) {
            q++;
            while (q < n && (s[q] == ' ' || s[q] == '\t' || s[q] == '\r'))
                q++;
            return q < n && s[q] == '\n';
        }
        q++;
    }
    return 0;
}

static int has_unsupported_unbraced_if(const char *s, size_t n)
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
            if (p < n && s[p] == '{') continue;
            if (!if_body_is_goto(s, n, i)) return 1;
        } else if (starts_word(s, n, i, "else")) {
            size_t p = i + 4;
            while (p < n && (s[p] == ' ' || s[p] == '\t' ||
                             s[p] == '\r' || s[p] == '\n')) p++;
            if (p < n && starts_word(s, n, p, "if")) continue;
            if (p >= n || s[p] != '{') return 1;
        }
    }
    return 0;
}

/* Collect constant c->xmm[N] indices.  Returns -1 for a whole-array or
 * dynamic access, so the caller can skip the function. */
static int collect_indices(const char *s, size_t n, unsigned char used[8])
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

/* A generated tail is always followed by its own return.  If the macro were
 * followed by ordinary code the local cache would be stale after guest code
 * changed c->xmm, so those functions are left alone. */
static int tail_has_return(const char *s, size_t n, size_t i)
{
    size_t p = i;
    int depth = 0;
    while (p < n) {
        if (s[p] == '(') depth++;
        else if (s[p] == ')') {
            if (depth > 0) depth--;
        } else if (s[p] == ';' && depth == 0) {
            p++;
            break;
        }
        p++;
    }
    while (p < n && (s[p] == ' ' || s[p] == '\t' ||
                     s[p] == '\r' || s[p] == '\n')) p++;
    return starts_word(s, n, p, "return");
}

static int tails_have_return(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (is_dispatch_exit(s, n, i) && !tail_has_return(s, n, i))
            return 0;
    }
    return 1;
}

static int tails_at_statement_boundary(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (is_dispatch_exit(s, n, i) &&
            !statement_boundary_before(s, i))
            return 0;
    }
    return 1;
}

/* Write dirty locals to c->xmm without clearing the dirty mask.  A conditional
 * exit can be taken on one path while the other path still needs its own
 * dirty set; leaving the mask intact lets the fall-through path write later. */
static void write_dirty(Buf *b, const unsigned char loaded[8],
                        const unsigned char dirty[8])
{
    int i;
    int wrote = 0;
    for (i = 0; i < 8; i++) {
        if (loaded[i] && dirty[i]) {
            buf_printf(b, " c->xmm[%d] = _wc3_xmm_local[%d];", i, i);
            wrote = 1;
        }
    }
    if (wrote) buf_str(b, " ");
}

/* A call or a label is reached on every path that continues past it.  Those
 * are the only points where the cache can be flushed and then discarded
 * without making the other side of a branch stale. */
static void flush_and_invalidate(Buf *b, unsigned char loaded[8],
                                 unsigned char dirty[8])
{
    write_dirty(b, loaded, dirty);
    memset(loaded, 0, 8);
    memset(dirty, 0, 8);
}

static int used_loaded(const unsigned char loaded[8],
                       const unsigned char used[8])
{
    int i;
    for (i = 0; i < 8; i++) {
        if (used[i] && !loaded[i]) return 0;
    }
    return 1;
}

static void preload_needed(Buf *b, unsigned char loaded[8],
                           unsigned char dirty[8], const unsigned char need[8])
{
    int i;
    for (i = 0; i < 8; i++) {
        if (!need[i] || loaded[i]) continue;
        if (b->n > 0 && b->p[b->n - 1] != '\n') buf_str(b, "\n");
        buf_printf(b, "    _wc3_xmm_local[%d] = c->xmm[%d];\n", i, i);
        loaded[i] = 1;
        dirty[i] = 0;
    }
}

/* A line that starts inside a block comment cannot receive preload statements:
 * they would be inserted before the comment closes and stay inside it.  The
 * generated corpus only uses one-line block comments, so reject the rare
 * multi-line form instead of tracking comment state in the transform. */
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

/* A line break inside parentheses makes the rest of the expression a
 * continuation line.  The transform cannot insert a flush there without
 * changing the grammar; reject those functions rather than leave an
 * untracked guest write under a loaded cache. */
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

static int eligible(const char *masked, size_t n, unsigned char used[8])
{
    int any = 0;
    int i;
    if (has_word(masked, n, "switch")) return 0;
    if (has_word(masked, n, "while")) return 0;
    if (has_word(masked, n, "for")) return 0;
    if (has_word(masked, n, "do")) return 0;
    if (has_word(masked, n, "break")) return 0;
    if (has_word(masked, n, "continue")) return 0;
    if (has_preprocessor_line(masked, n)) return 0;
    if (parens_cross_lines(masked, n)) return 0;
    if (has_unsupported_unbraced_if(masked, n)) return 0;
    if (strstr(masked, "wc3_guest_")) return 0;
    if (strstr(masked, "_wc3_xmm_local")) return 0;
    if (collect_indices(masked, n, used) != 0) return 0;
    if (count_xmm_accesses(masked, n) < 3) return 0;
    if (has_call_in_parens(masked, n)) return 0;
    if (!calls_are_top_level(masked, n)) return 0;
    if (!labels_are_top_level(masked, n)) return 0;
    if (!tails_have_return(masked, n)) return 0;
    if (!tails_at_statement_boundary(masked, n)) return 0;
    for (i = 0; i < 8; i++) {
        if (used[i]) {
            any = 1;
            break;
        }
    }
    return any;
}

static void drop_cache(unsigned char loaded[8], unsigned char dirty[8])
{
    memset(loaded, 0, 8);
    memset(dirty, 0, 8);
}

static int transform_body(const char *body, size_t n, const char *masked,
                          const unsigned char used[8], Buf *out)
{
    size_t i;
    size_t body_start = out->n;
    size_t final_close = n;
    unsigned char loaded[8];
    unsigned char dirty[8];
    int paren = 0;
    int brace_depth = 0;
    int tail_pending_return = 0;
    int unreachable = 0;
    int replaced = 0;

    memset(loaded, 0, sizeof loaded);
    memset(dirty, 0, sizeof dirty);
    while (final_close > 0 && (body[final_close - 1] == ' ' ||
                               body[final_close - 1] == '\t' ||
                               body[final_close - 1] == '\r' ||
                               body[final_close - 1] == '\n'))
        final_close--;
    if (final_close > 0) final_close--;

    buf_str(out, "{\n    XMM _wc3_xmm_local[8];\n");
    i = 1;
    if (i + 1 < n && body[i] == '\r' && body[i + 1] == '\n') i += 2;
    else if (i < n && body[i] == '\n') i++;

    while (i < n) {
        size_t ls = i;
        size_t le = i;
        size_t p;
        int line_live = 1;
        int preload = (brace_depth == 0);
        int blank;
        int preproc;
        int label;
        int start_else;
        int start_if;
        int start_close;
        int start_open;
        int start_exit;
        int start_goto;
        int start_call;
        int line_if_goto;
        int goto_brace_pending = 0;

        while (le < n && body[le] != '\n') le++;
        p = ls;
        while (p < le && (masked[p] == ' ' || masked[p] == '\t' ||
                          masked[p] == '\r'))
            p++;

        blank = p >= le;
        preproc = !blank && masked[p] == '#';
        label = !blank && label_at(masked, n, p, le);
        start_else = !blank && starts_word(masked, n, p, "else");
        start_if = !blank && starts_word(masked, n, p, "if");
        start_close = !blank && masked[p] == '}';
        start_open = !blank && masked[p] == '{';
        start_exit = !blank && (starts_word(masked, n, p, "return") ||
                                is_dispatch_exit(masked, n, p));
        start_goto = !blank && starts_word(masked, n, p, "goto");
        start_call = !blank && is_call_at(masked, n, p);
        line_if_goto = start_if && if_body_is_goto(masked, n, p);

        if (blank || preproc || paren > 0) {
            line_live = 0;
            preload = 0;
        } else if (label) {
            /* A label is a join.  Flush the values from the fall-through
             * path; a path arriving by goto has already written its dirty
             * locals before the jump. */
            flush_and_invalidate(out, loaded, dirty);
            unreachable = 0;
            line_live = 0;
            preload = 0;
            tail_pending_return = 0;
        } else if (unreachable) {
            line_live = 0;
            preload = 0;
        } else if (start_else || start_close || start_open) {
            preload = 0;
        } else if (start_if) {
            /* `else` followed by a newline and `if` is `else if`; no
             * statement may be inserted between them.  Never preload inside
             * braces: a skipped branch would leave the static loaded bits
             * set without having executed the loads. */
            preload = brace_depth == 0 && !preceded_by_else(masked, ls);
        } else if (start_exit || start_goto || start_call) {
            preload = 0;
        }

        if (preload && line_live && !used_loaded(loaded, used))
            preload_needed(out, loaded, dirty, used);

        while (i < le) {
            if (i == final_close && !unreachable)
                write_dirty(out, loaded, dirty);

            if (!unreachable && paren == 0) {
                if (starts_word(masked, n, i, "return")) {
                    if (tail_pending_return) {
                        tail_pending_return = 0;
                        if (brace_depth == 0) {
                            drop_cache(loaded, dirty);
                            unreachable = 1;
                            line_live = 0;
                        }
                    } else {
                        write_dirty(out, loaded, dirty);
                        if (brace_depth == 0) {
                            drop_cache(loaded, dirty);
                            unreachable = 1;
                            line_live = 0;
                        }
                    }
                } else if (is_dispatch_exit(masked, n, i)) {
                    write_dirty(out, loaded, dirty);
                    tail_pending_return = 1;
                } else if (starts_word(masked, n, i, "goto")) {
                    /* A path that jumps over a later call/label must have
                     * written its locals before the jump.  Do not clear the
                     * mask: the fall-through path may still own dirty values.
                     * An unbraced `if (cond) goto L;` needs braces around the
                     * flush, otherwise the goto itself would become
                     * unconditional. */
                    if (line_if_goto) {
                        buf_str(out, "{");
                        goto_brace_pending = 1;
                    }
                    write_dirty(out, loaded, dirty);
                    tail_pending_return = 0;
                    if (brace_depth == 0 && !line_if_goto) {
                        unreachable = 1;
                        line_live = 0;
                    }
                } else if (is_call_at(masked, n, i)) {
                    flush_and_invalidate(out, loaded, dirty);
                    tail_pending_return = 0;
                    line_live = 0;
                }
            }

            if (line_live && masked[i] == 'c' &&
                (i == 0 || !word_char(masked[i - 1])) &&
                i + 7 <= n && strncmp(masked + i, "c->xmm[", 7) == 0) {
                size_t after;
                int idx = parse_xmm_access(masked, n, i, &after);
                if (idx >= 0 && loaded[idx]) {
                    buf_printf(out, "_wc3_xmm_local[%d]", idx);
                    if (xmm_write_access(masked, n, i, after))
                        dirty[idx] = 1;
                    replaced = 1;
                    i = after;
                    continue;
                }
            }

            buf_add(out, body + i, 1);
            if (goto_brace_pending && masked[i] == ';') {
                buf_str(out, " }");
                goto_brace_pending = 0;
            }
            if (masked[i] == '(') paren++;
            else if (masked[i] == ')' && paren > 0) paren--;
            if (masked[i] == '{') brace_depth++;
            else if (masked[i] == '}') {
                if (brace_depth > 0) brace_depth--;
                tail_pending_return = 0;
            }
            i++;
        }
        if (i < n) {
            buf_add(out, body + i, 1);
            i++;
        }
    }

    if (!replaced) {
        /* The function was eligible, but every access sat on a line that the
         * transform leaves untouched (for example a label line).  Restore the
         * original body so an unused local is not declared. */
        out->n = body_start;
        buf_add(out, body, n);
        return 0;
    }
    if (out->n > 0 && out->p[out->n - 1] != '\n') buf_str(out, "\n");
    return 1;
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
        unsigned char used[8];
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
        ok = !has_multiline_comment(text + open, blen) &&
             eligible(masked, blen, used);
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
            int body_changed;
            if (!fend) fend = nl;
            flen = (size_t)(fend - fstart);
            if (flen >= sizeof fname) flen = sizeof fname - 1;
            memcpy(fname, fstart, flen);
            fname[flen] = 0;
            for (k = 0; k < 8; k++)
                if (used[k]) mask |= 1u << k;
            body_changed = transform_body(text + open, blen, masked, used,
                                          out);
            if (body_changed) {
                if (changes)
                    changes_add(changes, fname, accesses, blen, out->n - before,
                                mask);
                (*out_changed)++;
            }
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

static int check_not_contains(const char *name, const char *text,
                              const char *needle)
{
    if (strstr(text, needle)) {
        fprintf(stderr, "xmm_local self-test: %s: unexpected %s\n", name,
                needle);
        return 0;
    }
    return 1;
}

static size_t count_substr(const char *text, const char *needle)
{
    size_t count = 0;
    const char *p = text;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p++;
    }
    return count;
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
        "    c->xmm[2] = c->xmm[0];\n"
        "    return;\n"
        "}\n";
    static const char *goto_loop =
        "void L_0000000F(CPU *restrict c)\n"
        "{\n"
        "L_00000003:\n"
        "    c->xmm[0] = c->xmm[1];\n"
        "    if (c->xmm[2].f32[0] > 0.0f) goto L_00000003;\n"
        "    return;\n"
        "}\n";
    static const char *call =
        "void L_00000004(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[0] = c->xmm[1];\n"
        "    WC3_DIRECT_CALL(L_00000005, c);\n"
        "    c->xmm[2] = c->xmm[0];\n"
        "    return;\n"
        "}\n";
    static const char *label_call =
        "void L_00000016(CPU *restrict c)\n"
        "{\n"
        "L_00000017:\n"
        "    WC3_DIRECT_CALL(L_00000018, c);\n"
        "    c->xmm[0] = c->xmm[1];\n"
        "    c->xmm[2] = c->xmm[0];\n"
        "    return;\n"
        "}\n";
    static const char *guest_helper =
        "void L_00000009(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[0] = c->xmm[1];\n"
        "    wc3_guest_ok(c, 1u);\n"
        "    c->xmm[2] = c->xmm[0];\n"
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
    static const char *call_in_if =
        "void L_0000000A(CPU *restrict c)\n"
        "{\n"
        "    if (c->xmm[0].f32[0]) { WC3_DIRECT_CALL(L_00000005, c); c->xmm[1] = c->xmm[0]; }\n"
        "    c->xmm[2] = c->xmm[1];\n"
        "    return;\n"
        "}\n";
    static const char *call_in_paren =
        "void L_0000000B(CPU *restrict c)\n"
        "{\n"
        "    if (dispatch(c, 1u)) { c->xmm[0] = c->xmm[1]; }\n"
        "    c->xmm[2] = c->xmm[0];\n"
        "    return;\n"
        "}\n";
    static const char *tail_without_return =
        "void L_0000000C(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[0] = c->xmm[1];\n"
        "    dispatch_jmp(c, 1u);\n"
        "    c->xmm[2] = c->xmm[0];\n"
        "    return;\n"
        "}\n";
    static const char *tiny =
        "void L_0000000D(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[0] = c->xmm[1];\n"
        "    return;\n"
        "}\n";
    static const char *branch =
        "void L_0000000E(CPU *restrict c)\n"
        "{\n"
        "    if (c->xmm[0].f32[0] > 0.0f) { c->xmm[1].f32[0] = 1.0f; }\n"
        "    c->xmm[2] = c->xmm[1];\n"
        "    return;\n"
        "}\n";
    static const char *inner_label =
        "void L_00000013(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[0] = c->xmm[1];\n"
        "    if (c->xmm[2].f32[0]) {\n"
        "L_00000014:\n"
        "        c->xmm[0] = c->xmm[2];\n"
        "    }\n"
        "    c->xmm[2] = c->xmm[0];\n"
        "    return;\n"
        "}\n";
    static const char *goto_forward =
        "void L_00000011(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[0].f32[0] = 1.0f;\n"
        "    goto L_00000012;\n"
        "    c->xmm[1].f32[0] = 2.0f;\n"
        "L_00000012:\n"
        "    c->xmm[2].f32[0] = 3.0f;\n"
        "    return;\n"
        "}\n";
    static const char *call_after_assign =
        "void L_00000015(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[0] = c->xmm[1];\n"
        "    c->xmm[2] = c->xmm[0];\n"
        "    int r = dispatch(c, 1u);\n"
        "    (void)r;\n"
        "    return;\n"
        "}\n";
    static const char *return_dead =
        "void L_00000016(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[0] = c->xmm[1];\n"
        "    return;\n"
        "    c->xmm[2] = c->xmm[0];\n"
        "}\n";
    static const char *preproc =
        "void L_00000017(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[0] = c->xmm[1];\n"
        "    c->xmm[2] = c->xmm[0];\n"
        "#if defined(FOO)\n"
        "    c->xmm[1] = c->xmm[2];\n"
        "#endif\n"
        "    return;\n"
        "}\n";
    static const char *paren_break =
        "void L_00000018(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[0] = c->xmm[1];\n"
        "    x = (c->xmm[2] = c->xmm[0],\n"
        "         1);\n"
        "    return;\n"
        "}\n";
    static const char *multiline_comment =
        "void L_00000019(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[0] = c->xmm[1];\n"
        "    /* a comment\n"
        "     * over two lines */\n"
        "    c->xmm[2] = c->xmm[0];\n"
        "    return;\n"
        "}\n";
    static const char *label_line =
        "void L_0000001A(CPU *restrict c)\n"
        "{\n"
        "L_0000001B: c->xmm[0] = c->xmm[1]; c->xmm[2] = c->xmm[0]; return;\n"
        "}\n";
    static const char *if_goto_cond_write =
        "void L_0000001C(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[0] = c->xmm[1];\n"
        "    if ((c->xmm[2] = c->xmm[0]).f32[0] > 0.0f) goto L_0000001D;\n"
        "    c->xmm[1] = c->xmm[2];\n"
        "L_0000001D:\n"
        "    return;\n"
        "}\n";
    Buf out;
    Changes changes;
    size_t funcs, changed;
    int ok = 1;

#define RESET_CASE() do { free(out.p); memset(&out, 0, sizeof out); \
    changes_free(&changes); memset(&changes, 0, sizeof changes); } while (0)

    memset(&changes, 0, sizeof changes);
    memset(&out, 0, sizeof out);

    transform_text(sample, strlen(sample), &out, &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 1;
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
    /* A top-level return writes the dirty set once and drops the cache. */
    ok &= count_substr(out.p, "c->xmm[1] = _wc3_xmm_local[1];") == 1;
    RESET_CASE();

    transform_text(label, strlen(label), &out, &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].accesses == 4;
    ok &= changes.n == 1 && changes.v[0].used_mask == 7u;
    ok &= check_contains("label", out.p, "L_00000003:");
    ok &= check_contains("label", out.p,
                         "_wc3_xmm_local[0] = _wc3_xmm_local[1];");
    ok &= check_contains("label", out.p,
                         "_wc3_xmm_local[2] = _wc3_xmm_local[0];");
    RESET_CASE();

    transform_text(goto_loop, strlen(goto_loop), &out, &funcs, &changed,
                   &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].accesses == 3;
    ok &= check_contains("goto", out.p, "goto L_00000003;");
    ok &= check_not_contains("goto", out.p, "goto _wc3");
    /* The loop-back jump must write the dirty value before leaving, and the
     * fall-through return must remain reachable. */
    ok &= check_contains("goto", out.p,
                         "if (_wc3_xmm_local[2].f32[0] > 0.0f) { "
                         "c->xmm[0] = _wc3_xmm_local[0]; goto L_00000003; }");
    ok &= check_contains("goto", out.p,
                         "c->xmm[0] = _wc3_xmm_local[0]; return;");
    RESET_CASE();

    transform_text(call, strlen(call), &out, &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].accesses == 4;
    ok &= changes.n == 1 && changes.v[0].used_mask == 7u;
    ok &= check_contains("call", out.p, "WC3_DIRECT_CALL(L_00000005, c);");
    ok &= check_contains("call", out.p, "c->xmm[0] = _wc3_xmm_local[0];");
    ok &= check_contains("call", out.p,
                         "c);\n    _wc3_xmm_local[0] = c->xmm[0];");
    ok &= check_contains("call", out.p,
                         "_wc3_xmm_local[2] = _wc3_xmm_local[0];");
    RESET_CASE();

    /* A call immediately after a line-start label must be recognized as a
     * top-level statement; the label itself already invalidated the cache. */
    transform_text(label_call, strlen(label_call), &out, &funcs, &changed,
                   &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].accesses == 4;
    ok &= changes.n == 1 && changes.v[0].used_mask == 7u;
    ok &= check_contains("label_call", out.p,
                         "L_00000017:\n    WC3_DIRECT_CALL(L_00000018, c);");
    ok &= check_contains("label_call", out.p,
                         "c);\n    _wc3_xmm_local[0] = c->xmm[0];");
    ok &= check_contains("label_call", out.p,
                         "_wc3_xmm_local[2] = _wc3_xmm_local[0];");
    ok &= check_contains("label_call", out.p,
                         "c->xmm[0] = _wc3_xmm_local[0];");
    ok &= check_contains("label_call", out.p,
                         "c->xmm[2] = _wc3_xmm_local[2];");
    RESET_CASE();

    transform_text(guest_helper, strlen(guest_helper), &out, &funcs, &changed,
                   &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(guest_helper) &&
          memcmp(out.p, guest_helper, out.n) == 0;
    RESET_CASE();

    transform_text(comment, strlen(comment), &out, &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(comment) && memcmp(out.p, comment, out.n) == 0;
    RESET_CASE();

    transform_text(fall, strlen(fall), &out, &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].accesses == 5;
    ok &= changes.n == 1 && changes.v[0].used_mask == 7u;
    ok &= check_contains("fall", out.p, "_wc3_xmm_local[0].f32[0]");
    ok &= check_contains("fall", out.p, "dispatch_jmp(c, 1u); return;");
    ok &= check_not_contains("fall", out.p,
                             "dispatch_jmp(c, 1u); _wc3_xmm_local");
    ok &= check_contains("fall", out.p,
                         "_wc3_xmm_local[2] = _wc3_xmm_local[1];");
    ok &= check_contains("fall", out.p, "c->xmm[1] = _wc3_xmm_local[1];");
    ok &= check_contains("fall", out.p, "c->xmm[2] = _wc3_xmm_local[2];");
    RESET_CASE();

    transform_text(unbraced, strlen(unbraced), &out, &funcs, &changed,
                   &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(unbraced) && memcmp(out.p, unbraced, out.n) == 0;
    RESET_CASE();

    transform_text(call_in_if, strlen(call_in_if), &out, &funcs, &changed,
                   &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(call_in_if) &&
          memcmp(out.p, call_in_if, out.n) == 0;
    RESET_CASE();

    transform_text(call_in_paren, strlen(call_in_paren), &out, &funcs,
                   &changed, &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(call_in_paren) &&
          memcmp(out.p, call_in_paren, out.n) == 0;
    RESET_CASE();

    transform_text(tail_without_return, strlen(tail_without_return), &out,
                   &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(tail_without_return) &&
          memcmp(out.p, tail_without_return, out.n) == 0;
    RESET_CASE();

    transform_text(tiny, strlen(tiny), &out, &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(tiny) && memcmp(out.p, tiny, out.n) == 0;
    RESET_CASE();

    transform_text(branch, strlen(branch), &out, &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].accesses == 4;
    ok &= changes.n == 1 && changes.v[0].used_mask == 7u;
    /* All used registers must be loaded before the branch; a skipped branch
     * must not leave a "loaded" bit without an executed load. */
    ok &= count_substr(out.p, "_wc3_xmm_local[1] = c->xmm[1];") == 1;
    ok &= check_contains("branch", out.p,
                         "if (_wc3_xmm_local[0].f32[0] > 0.0f) { "
                         "_wc3_xmm_local[1].f32[0] = 1.0f; }");
    ok &= check_contains("branch", out.p,
                         "_wc3_xmm_local[2] = _wc3_xmm_local[1];");
    ok &= check_contains("branch", out.p,
                         "c->xmm[1] = _wc3_xmm_local[1];");
    RESET_CASE();

    transform_text(inner_label, strlen(inner_label), &out, &funcs, &changed,
                   &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(inner_label) &&
          memcmp(out.p, inner_label, out.n) == 0;
    RESET_CASE();

    transform_text(goto_forward, strlen(goto_forward), &out, &funcs, &changed,
                   &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].accesses == 3;
    ok &= check_contains("goto_flush", out.p,
                         "c->xmm[0] = _wc3_xmm_local[0]; goto L_00000012;");
    ok &= check_not_contains("goto_flush", out.p,
                             "_wc3_xmm_local[1].f32[0] = 2.0f;");
    ok &= check_contains("goto_flush", out.p,
                         "_wc3_xmm_local[2].f32[0] = 3.0f;");
    RESET_CASE();

    transform_text(call_after_assign, strlen(call_after_assign), &out, &funcs,
                   &changed, &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(call_after_assign) &&
          memcmp(out.p, call_after_assign, out.n) == 0;
    RESET_CASE();

    transform_text(return_dead, strlen(return_dead), &out, &funcs, &changed,
                   &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].accesses == 4;
    ok &= check_contains("return_dead", out.p,
                         "c->xmm[0] = _wc3_xmm_local[0]; return;");
    ok &= check_not_contains("return_dead", out.p,
                             "_wc3_xmm_local[2] = _wc3_xmm_local[0];");
    RESET_CASE();

    transform_text(preproc, strlen(preproc), &out, &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(preproc) && memcmp(out.p, preproc, out.n) == 0;
    RESET_CASE();

    transform_text(paren_break, strlen(paren_break), &out, &funcs, &changed,
                   &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(paren_break) &&
          memcmp(out.p, paren_break, out.n) == 0;
    RESET_CASE();

    transform_text(multiline_comment, strlen(multiline_comment), &out, &funcs,
                   &changed, &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(multiline_comment) &&
          memcmp(out.p, multiline_comment, out.n) == 0;
    RESET_CASE();

    transform_text(label_line, strlen(label_line), &out, &funcs, &changed,
                   &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(label_line) &&
          memcmp(out.p, label_line, out.n) == 0;
    RESET_CASE();

    transform_text(if_goto_cond_write, strlen(if_goto_cond_write), &out,
                   &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].accesses == 6;
    ok &= check_contains("if_goto_cond_write", out.p,
                         "if ((_wc3_xmm_local[2] = _wc3_xmm_local[0]).f32[0] "
                         "> 0.0f) { c->xmm[0] = _wc3_xmm_local[0]; "
                         "c->xmm[2] = _wc3_xmm_local[2]; goto L_0000001D; }");
    ok &= check_contains("if_goto_cond_write", out.p,
                         "_wc3_xmm_local[1] = _wc3_xmm_local[2];");
    RESET_CASE();

#undef RESET_CASE
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
