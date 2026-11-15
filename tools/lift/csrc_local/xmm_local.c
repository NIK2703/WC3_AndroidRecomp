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
 * transformed function with access count, used-register mask, lazy mask, block
 * mask and body sizes.  The default whole-function pass is part of the normal
 * relifted-corpus pipeline and runs unconditionally after dead_flags.
 * --planner and --lazy remain calibration switches.  The canonical build
 * additionally enables the block fallback and its entry-load elision with
 * --block --block-lazy; the switches stay explicit so a manual A/B can omit
 * either one.
 *
 *   xmm_local --lift IN --out OUT [--json REPORT] [--planner] [--lazy]
 *             [--block] [--block-lazy] [--quiet]
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
    unsigned plan_mask;
    unsigned lazy_mask;
    unsigned block_mask;
    int plan_score;
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

/* The planner is opt-in until its selection model has a full-corpus object
 * A/B.  Without the flag the pass keeps the live-in-only policy for
 * packed-double functions and the full cache elsewhere; the report still
 * carries the planner proposal for calibration. */
static int planner_enabled;
static int lazy_enabled;
static int block_enabled;
static int block_lazy_enabled;

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
                        size_t before, size_t after, unsigned mask,
                        unsigned plan_mask, unsigned lazy_mask,
                        unsigned block_mask, int plan_score)
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
    ch->plan_mask = plan_mask;
    ch->lazy_mask = lazy_mask;
    ch->block_mask = block_mask;
    ch->plan_score = plan_score;
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

/* A multi-register flush is where a plain union assignment gets scalarised
 * into one 32-bit store plus two zero stores per register.  At three or more
 * live XMM registers that is more expensive than one explicit 128-bit
 * load/store pair, so use the named helper for that case.  Fewer registers
 * stay as assignments: there the scalar form is smaller and the helper only
 * adds a vector build.  The helper copies the complete register, so lane
 * semantics and upper lanes are unchanged. */
#define XMM_WRITEBACK_VECTOR_MIN 3

static int has_any_sse_op(const char *s, size_t n,
                          const char *const *ops, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++)
        if (has_word(s, n, ops[i])) return 1;
    return 0;
}

/* True when the function's SSE work is packed double and nothing else.  Such
 * functions keep the pass, but only for registers that are live-in: a full
 * cache made every written-first register live across the function and paid
 * more for its writeback than it saved.  Scalar and packed-single operations
 * keep the full cache: that is where the plain assignment scalarises into
 * per-lane stores and the helper pays off.  `shufps` is deliberately not
 * treated as packed-single work: it is a lane shuffle that also appears in
 * double/integer code. */
static int packed_double_only(const char *s, size_t n)
{
    static const char *const scalar_ops[] = {
        "movss", "movsd", "addss", "addsd", "subss", "subsd",
        "mulss", "mulsd", "divss", "divsd", "comiss", "comisd",
        "ucomiss", "ucomisd", "cvtsi2ss", "cvtsi2sd", "cvtss2sd",
        "cvtsd2ss", "cvttss2si", "cvttsd2si", "sqrtss", "sqrtsd",
        "minss", "maxss", "minsd", "maxsd", "rsqrtss", "rcpss"
    };
    static const char *const single_ops[] = {
        "movaps", "movups", "movhps", "movlps", "addps", "subps",
        "mulps", "divps", "andps", "andnps", "orps", "xorps",
        "cmpps", "unpcklps", "unpckhps", "cvtdq2ps",
        "cvtps2dq", "cvttps2dq", "cvtps2pd", "cvtpd2ps",
        "sqrtps", "maxps", "minps", "rcpps", "rsqrtps", "movntps",
        "movhlps", "movlhps"
    };
    static const char *const double_ops[] = {
        "movapd", "movupd", "movhpd", "movlpd", "addpd", "subpd",
        "mulpd", "divpd", "andpd", "andnpd", "orpd", "xorpd",
        "cmppd", "shufpd", "unpcklpd", "unpckhpd", "cvtdq2pd",
        "cvtpd2dq", "cvttpd2dq", "sqrtpd", "maxpd", "minpd",
        "movntpd"
    };
    return has_any_sse_op(s, n, double_ops,
                          sizeof double_ops / sizeof double_ops[0]) &&
           !has_any_sse_op(s, n, scalar_ops,
                           sizeof scalar_ops / sizeof scalar_ops[0]) &&
           !has_any_sse_op(s, n, single_ops,
                           sizeof single_ops / sizeof single_ops[0]);
}

/* For packed-double-only functions, keep only registers that are read before
 * their first write.  Those are the only ones that can avoid a real guest
 * load; a written-first register already has a store, and keeping it live in
 * the local cache only adds a writeback at every exit.  A register must also
 * be accessed at least twice, otherwise the entry load cannot pay for itself.
 * The scan is lexical, so it is conservative around branches: a register is
 * selected only when its first textual access is a read, and selecting it is
 * always safe because the existing preload path loads it at function entry. */
static int select_live_in(const char *s, size_t n,
                          const unsigned char used[8],
                          unsigned char cache[8])
{
    unsigned char seen[8];
    size_t count[8];
    size_t i = 0;
    int any = 0;

    memset(seen, 0, sizeof seen);
    memset(count, 0, sizeof count);
    memset(cache, 0, 8);
    while (i + 7 <= n) {
        const char *hit = strstr(s + i, "c->xmm[");
        size_t pos;
        size_t after;
        int idx;
        if (!hit) break;
        pos = (size_t)(hit - s);
        if (pos > 0 && word_char(s[pos - 1])) {
            i = pos + 7;
            continue;
        }
        idx = parse_xmm_access(s, n, pos, &after);
        if (idx < 0) {
            i = pos + 7;
            continue;
        }
        if (!used[idx]) {
            i = after;
            continue;
        }
        count[idx]++;
        if (!seen[idx]) {
            seen[idx] = 1;
            if (!xmm_write_access(s, n, pos, after)) cache[idx] = 1;
        }
        i = after;
    }
    for (i = 0; i < 8; i++) {
        if (cache[i] && count[i] < 2) cache[i] = 0;
        if (cache[i]) any = 1;
    }
    return any;
}

/* Per-register facts for the planner.  These are deliberately independent of
 * SSE mnemonic classes: the planner sees reads, writes, lane accesses and
 * control-flow join/exit sites, not `movapd` versus `movss`. */
typedef struct {
    size_t reads;
    size_t writes;
    size_t lane_reads;
    size_t lane_writes;
    size_t full_writes;
    size_t first_pos;
    int first_write;
    int first_lane;
    int lazy_safe;
    int seen;
} XmmRegFacts;

typedef struct {
    XmmRegFacts reg[8];
    size_t calls;
    size_t labels;
    size_t gotos;
    size_t exits;
} XmmFacts;

/* During P1 the model is intentionally small and provisional:
 *   - a read before the first write can avoid the entry load;
 *   - a lane read benefits more from scalar replacement than a full-register
 *     read, which is already one vector load/store;
 *   - a written-first register is cheap only when its lane reads dominate its
 *     full-register writes; otherwise the local copy just adds pressure;
 *   - one fixed entry/pressure allowance keeps tiny ranges out.
 * The values are object-code weights, not guest semantics.  They are kept in
 * one place so the full-corpus A/B can replace them with measured costs. */
#define PLAN_ENTRY_COST 1
#define PLAN_FIRST_WRITE_COST 2
#define PLAN_FULL_WRITE_WEIGHT 2
#define PLAN_LANE_READ_WEIGHT 4
#define PLAN_REMOVE_GAIN (-1)

/* A lane access is a member/index access after the register itself, e.g.
 * `c->xmm[3].f32[0]`.  A whole-register access or an address-taken register
 * (`&c->xmm[3]`) is a full access. */
static int xmm_lane_access(const char *s, size_t n, size_t after)
{
    size_t p = after;
    while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
    if (p < n && s[p] == '.') return 1;
    if (p + 1 < n && s[p] == '-' && s[p + 1] == '>') return 1;
    return 0;
}

/* A lazy local may be created by its first write instead of by an entry load.
 * That is safe only when the first access is a full-register store at the
 * function's top level and no branch, join or early exit precedes it.  A call
 * is allowed: it flushes the cache, and the store that follows still runs on
 * every path that reaches later code. */
static int prefix_has_control(const char *s, size_t end)
{
    static const char *const words[] = {
        "if", "else", "goto", "return", "switch", "while", "for", "do",
        "break", "continue"
    };
    size_t i;
    size_t j;
    for (i = 0; i < sizeof words / sizeof words[0]; i++) {
        if (has_word(s, end, words[i])) return 1;
    }
    for (j = 0; j < end; ) {
        size_t le = j;
        size_t p;
        while (le < end && s[le] != '\n') le++;
        p = j;
        while (p < le && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')) p++;
        if (p < le && label_at(s, end, p, le)) return 1;
        j = le + (le < end ? 1 : 0);
    }
    return 0;
}

/* Count accesses to one register on [ls, le) and report whether the first one
 * is a plain full-register assignment.  A lazy first write is accepted only
 * when it is the sole access on the line, so an RHS read cannot observe an
 * uninitialized local. */
static int line_xmm_access(const char *s, size_t n, size_t ls, size_t le,
                           int idx, int *first_full_write)
{
    size_t i = ls;
    int count = 0;
    int first_full = 0;
    while (i < le) {
        const char *hit = strstr(s + i, "c->xmm[");
        size_t pos;
        size_t after;
        int got;
        if (!hit) break;
        pos = (size_t)(hit - s);
        if (pos >= le) break;
        if (pos > 0 && word_char(s[pos - 1])) {
            i = pos + 7;
            continue;
        }
        got = parse_xmm_access(s, n, pos, &after);
        if (got < 0 || after > le) {
            i = pos + 7;
            continue;
        }
        if (got == idx) {
            size_t p;
            int full;
            if (count == 0) {
                full = !xmm_lane_access(s, n, after) &&
                       xmm_write_access(s, n, pos, after);
                if (full) {
                    p = after;
                    while (p < le && (s[p] == ' ' || s[p] == '\t')) p++;
                    if (p >= le || s[p] != '=' ||
                        (p + 1 < le && s[p + 1] == '='))
                        full = 0;
                }
                first_full = full;
            }
            count++;
        }
        i = after;
    }
    *first_full_write = first_full;
    return count;
}

static int lazy_first_write_safe(const char *s, size_t n, size_t pos, int idx)
{
    size_t p = pos;
    size_t after;
    size_t ls;
    size_t le;
    size_t j;
    int depth = 0;
    int count;
    int first_full;
    while (p > 0 && s[p - 1] != '\n') p--;
    ls = p;
    while (p < n && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')) p++;
    if (p != pos) return 0;
    le = p;
    while (le < n && s[le] != '\n') le++;
    if (parse_xmm_access(s, n, pos, &after) != idx) return 0;
    if (line_xmm_access(s, n, ls, le, idx, &first_full) != 1 ||
        !first_full)
        return 0;
    for (j = 0; j < pos; j++) {
        if (s[j] == '{') depth++;
        else if (s[j] == '}') depth--;
    }
    if (depth != 1) return 0;
    return !prefix_has_control(s, pos);
}

static void collect_xmm_facts(const char *s, size_t n, XmmFacts *f)
{
    size_t i = 0;
    size_t ls = 0;

    memset(f, 0, sizeof *f);
    while (i + 7 <= n) {
        const char *hit = strstr(s + i, "c->xmm[");
        size_t pos;
        size_t after;
        int idx;
        int write;
        if (!hit) break;
        pos = (size_t)(hit - s);
        if (pos > 0 && word_char(s[pos - 1])) {
            i = pos + 7;
            continue;
        }
        idx = parse_xmm_access(s, n, pos, &after);
        if (idx < 0) {
            i = pos + 7;
            continue;
        }
        write = xmm_write_access(s, n, pos, after);
        if (!f->reg[idx].seen) {
            f->reg[idx].seen = 1;
            f->reg[idx].first_write = write;
            f->reg[idx].first_pos = pos;
            f->reg[idx].first_lane = xmm_lane_access(s, n, after);
        }
        if (write) {
            f->reg[idx].writes++;
            if (xmm_lane_access(s, n, after))
                f->reg[idx].lane_writes++;
            else
                f->reg[idx].full_writes++;
        } else {
            f->reg[idx].reads++;
            if (xmm_lane_access(s, n, after)) f->reg[idx].lane_reads++;
        }
        i = after;
    }
    for (i = 0; i < 8; i++) {
        if (f->reg[i].seen && f->reg[i].first_write &&
            !f->reg[i].first_lane)
            f->reg[i].lazy_safe = lazy_first_write_safe(
                s, n, f->reg[i].first_pos, (int)i);
    }

    /* Count the points where the existing transform flushes or invalidates
     * the cache.  A line may contain a tail and its generated return; that is
     * one exit, not two. */
    while (ls < n) {
        size_t le = ls;
        size_t p = ls;
        int line_exit = 0;
        int line_goto = 0;
        size_t q;
        while (le < n && s[le] != '\n') le++;
        while (p < le && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')) p++;
        if (p < le) {
            if (label_at(s, n, p, le)) f->labels++;
            if (is_call_at(s, n, p)) f->calls++;
        }
        for (q = p; q < le; q++) {
            if (!line_goto && starts_word(s, n, q, "goto")) {
                line_goto = 1;
                f->gotos++;
            }
            if (starts_word(s, n, q, "return") ||
                is_dispatch_exit(s, n, q)) {
                line_exit = 1;
                break;
            }
        }
        if (line_exit) f->exits++;
        ls = le + (le < n ? 1 : 0);
    }
}

/* Start from the current policy's mask and remove only a register whose facts
 * make the local copy strongly unprofitable.  Keeping the current mask as the
 * baseline makes this first planner step monotone: a model error can leave a
 * store in place, but cannot silently drop a register that the existing
 * live-in/full policy already selected.  Adding profitable registers and a
 * full 2^8 search need pressure/lazy-load facts and are later stages. */
static int select_planned(const char *s, size_t n,
                          const unsigned char used[8],
                          const unsigned char current[8],
                          unsigned char cache[8], unsigned char lazy_out[8],
                          unsigned *mask_out)
{
    XmmFacts f;
    unsigned mask = 0;
    unsigned current_mask = 0;
    long score = 0;
    long current_score = 0;
    int k;

    collect_xmm_facts(s, n, &f);
    for (k = 0; k < 8; k++) {
        long g;
        if (!used[k] || !current[k] || !f.reg[k].seen) continue;
        current_mask |= 1u << k;
        g = (long)f.reg[k].reads +
            (long)PLAN_LANE_READ_WEIGHT * (long)f.reg[k].lane_reads;
        if (f.reg[k].first_write) {
            g -= PLAN_FIRST_WRITE_COST;
            g -= (long)PLAN_FULL_WRITE_WEIGHT *
                 (long)f.reg[k].full_writes;
        }
        g -= PLAN_ENTRY_COST;
        current_score += g;
        if (g < PLAN_REMOVE_GAIN) continue;
        mask |= 1u << k;
        score += g;
    }

    /* A full mask means the established policy deliberately promoted every
     * used register.  Partial selection changes LLVM's alias/register
     * allocation much more than the text size suggests, and the object A/B
     * rejected it.  Keep the proven full mask until the model can price that
     * pressure.  Likewise an empty proposal keeps the current mask instead of
     * replacing it with one arbitrary "least bad" register. */
    if (current_mask == 0xFFu || mask == 0) {
        mask = current_mask;
        score = current_score;
    }

    memset(cache, 0, 8);
    memset(lazy_out, 0, 8);
    for (k = 0; k < 8; k++) {
        if (!(mask & (1u << k))) continue;
        cache[k] = 1;
        if (f.reg[k].lazy_safe) lazy_out[k] = 1;
    }
    *mask_out = mask;
    return (int)score;
}

/* Lazy promotion is independent of the mask decision.  The current cache can
 * skip an entry load when its first access is a dominating full-register
 * store, exactly like a planner-selected register. */
static void select_lazy_current(const char *s, size_t n,
                                const unsigned char current[8],
                                unsigned char lazy[8])
{
    XmmFacts f;
    int k;
    collect_xmm_facts(s, n, &f);
    memset(lazy, 0, 8);
    for (k = 0; k < 8; k++) {
        if (current[k] && f.reg[k].seen && f.reg[k].lazy_safe)
            lazy[k] = 1;
    }
}

/* Write dirty locals to c->xmm without clearing the dirty mask.  A conditional
 * exit can be taken on one path while the other path still needs its own
 * dirty set; leaving the mask intact lets the fall-through path write later. */
static void write_dirty(Buf *b, const unsigned char loaded[8],
                        const unsigned char dirty[8])
{
    int i;
    int wrote = 0;
    int ndirty = 0;
    for (i = 0; i < 8; i++)
        if (loaded[i] && dirty[i]) ndirty++;
    if (ndirty >= XMM_WRITEBACK_VECTOR_MIN) {
        for (i = 0; i < 8; i++) {
            if (loaded[i] && dirty[i]) {
                buf_printf(b,
                           " xmm_store_full(&c->xmm[%d], &_wc3_xmm_local[%d]);",
                           i, i);
                wrote = 1;
            }
        }
    } else {
        for (i = 0; i < 8; i++) {
            if (loaded[i] && dirty[i]) {
                buf_printf(b, " c->xmm[%d] = _wc3_xmm_local[%d];", i, i);
                wrote = 1;
            }
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

/* Preload one top-level line.  Ordinary locals are loaded eagerly at the
 * first line, preserving the established transform.  A lazy local is loaded
 * only when this line actually reads it; a sole plain full-register store can
 * create the local in place and skip the entry load entirely. */
static void preload_line(Buf *b, unsigned char loaded[8],
                         unsigned char dirty[8], const unsigned char used[8],
                         const unsigned char lazy[8], const char *masked,
                         size_t n, size_t ls, size_t le)
{
    int i;
    for (i = 0; i < 8; i++) {
        int count;
        int first_full;
        if (!used[i] || loaded[i]) continue;
        if (lazy[i]) {
            count = line_xmm_access(masked, n, ls, le, i, &first_full);
            if (count == 0) continue;
            if (count == 1 && first_full) {
                loaded[i] = 1;
                dirty[i] = 0;
                continue;
            }
        }
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
                          const unsigned char used[8],
                          const unsigned char lazy[8], Buf *out)
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

        if (preload && line_live)
            preload_line(out, loaded, dirty, used, lazy, masked, n, ls, le);

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

/* The whole-function transform above needs dominating load points.  Functions
 * with labels, loops, switches or nested guest calls are left alone.  The
 * fallback below instead treats every straight-line run between control-flow
 * statements as an independent region: it loads at the region entry and
 * writes back at the region exit.  A region never spans a branch, a label, a
 * guest call or a brace, so no value is live across a path join.  This is a
 * deliberately conservative subset of block-local promotion; it is opt-in
 * until its object A/B is accepted. */

static int starts_macro_prefix(const char *s, size_t n, size_t i, const char *w)
{
    size_t wl = strlen(w);
    if (i + wl > n) return 0;
    if (i > 0 && word_char(s[i - 1])) return 0;
    return strncmp(s + i, w, wl) == 0;
}

/* The generated macros have several suffixes (WC3_DIRECT_CALL_CLEAN,
 * WC3_DIRECT_CALL_DEPTH, WC3_DIRECT_CALL_LEAF).  The full-function transform
 * predates those suffixes; the block fallback must recognize the prefix. */
static int line_has_guest_call(const char *s, size_t n, size_t ls, size_t le)
{
    size_t i;
    for (i = ls; i < le; i++) {
        size_t p;
        if (starts_macro_prefix(s, n, i, "WC3_DIRECT_CALL") ||
            starts_macro_prefix(s, n, i, "WC3_DIRECT_TAIL") ||
            starts_macro_prefix(s, n, i, "WC3_GUEST_DRAIN") ||
            starts_macro_prefix(s, n, i, "dispatch_jmp") ||
            starts_macro_prefix(s, n, i, "wc3_guest_"))
            return 1;
        if (starts_word(s, n, i, "dispatch")) {
            p = i + 8;
            while (p < le && (s[p] == ' ' || s[p] == '\t')) p++;
            if (p < le && s[p] == '(') return 1;
        }
        if (starts_word(s, n, i, "dispatch_common")) {
            p = i + 15;
            while (p < le && (s[p] == ' ' || s[p] == '\t')) p++;
            if (p < le && s[p] == '(') return 1;
        }
    }
    return 0;
}

static int line_indent(const char *s, size_t ls, size_t le)
{
    int n = 0;
    size_t i = ls;
    while (i < le) {
        if (s[i] == ' ') n++;
        else if (s[i] == '\t') n += 4;
        else break;
        i++;
    }
    return n;
}

/* Control keywords may follow an earlier statement on the same generated
 * line, as in `c->eax = 1; if (cond) goto L;`.  A region must end before any
 * such line: otherwise a goto or return could skip the region's writeback.
 * Scan only top-level tokens, so control words inside a call argument or an
 * initializer do not matter. */
static int line_has_top_level_control(const char *s, size_t n, size_t ls,
                                      size_t le)
{
    size_t p;
    int paren = 0;
    int brace = 0;
    p = ls;
    while (p < le && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')) p++;
    if (p < le && s[p] == '#') return 0;
    for (p = ls; p < le; p++) {
        char c = s[p];
        if (c == '(') {
            paren++;
            continue;
        }
        if (c == ')') {
            if (paren) paren--;
            continue;
        }
        if (c == '{') {
            brace++;
            continue;
        }
        if (c == '}') {
            if (brace) brace--;
            continue;
        }
        if (paren || brace) continue;
        if (label_at(s, n, p, le)) return 1;
        if (starts_word(s, n, p, "if") ||
            starts_word(s, n, p, "else") ||
            starts_word(s, n, p, "switch") ||
            starts_word(s, n, p, "while") ||
            starts_word(s, n, p, "for") ||
            starts_word(s, n, p, "do") ||
            starts_word(s, n, p, "goto") ||
            starts_word(s, n, p, "return") ||
            starts_word(s, n, p, "break") ||
            starts_word(s, n, p, "continue") ||
            starts_word(s, n, p, "case") ||
            starts_word(s, n, p, "default"))
            return 1;
    }
    return 0;
}

/* A region may not contain a control-flow statement, a label, a brace or a
 * guest-visible call.  Braces are rejected anywhere on the line, not only at
 * the first non-space character: an inline block would otherwise let a region
 * cross a scope or a conditional boundary. */
static int line_is_barrier(const char *masked, size_t n, size_t ls, size_t le)
{
    size_t p = ls;
    while (p < le && (masked[p] == ' ' || masked[p] == '\t' ||
                      masked[p] == '\r'))
        p++;
    if (p >= le) return 0;
    if (masked[p] == '#') return 1;
    if (label_at(masked, n, p, le)) return 1;
    if (line_has_top_level_control(masked, n, ls, le)) return 1;
    if (memchr(masked + ls, '{', le - ls) ||
        memchr(masked + ls, '}', le - ls))
        return 1;
    return line_has_guest_call(masked, n, ls, le);
}

/* A control line without a `{` or a complete `;` statement owns the following
 * indented lines.  Those lines are copied without a local cache: inserting a
 * load or a writeback before an unbraced body would make it unconditional. */
static int line_is_unbraced_control(const char *s, size_t n, size_t ls,
                                    size_t le)
{
    size_t p;
    size_t q;
    int paren = 0;
    int brace = 0;
    int control = 0;
    p = ls;
    while (p < le && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')) p++;
    if (p < le && s[p] == '#') return 0;
    for (p = ls; p < le; p++) {
        char c = s[p];
        if (c == '(') {
            paren++;
            continue;
        }
        if (c == ')') {
            if (paren) paren--;
            continue;
        }
        if (c == '{') {
            brace++;
            continue;
        }
        if (c == '}') {
            if (brace) brace--;
            continue;
        }
        if (paren || brace) continue;
        if (starts_word(s, n, p, "if") ||
            starts_word(s, n, p, "else") ||
            starts_word(s, n, p, "while") ||
            starts_word(s, n, p, "for") ||
            starts_word(s, n, p, "do") ||
            starts_word(s, n, p, "switch")) {
            control = 1;
            p += 1;
        }
    }
    if (!control) return 0;
    q = le;
    while (q > ls && (s[q - 1] == ' ' || s[q - 1] == '\t' ||
                      s[q - 1] == '\r' || s[q - 1] == '\n'))
        q--;
    if (q == ls) return 0;
    /* A complete statement ends in `;` or `}`.  A braced control opens with
     * `{`.  The only remaining shape is a control line whose body follows. */
    return s[q - 1] != '{' && s[q - 1] != ';' && s[q - 1] != '}';
}

static int low_lane_suffix(const char *s, size_t re, size_t p, const char *lane)
{
    size_t n = strlen(lane);
    if (p + n > re) return 0;
    if (strncmp(s + p, lane, n) != 0) return 0;
    if (p + n < re) {
        char c = s[p + n];
        if (word_char(c) || c == '.' || c == '[') return 0;
    }
    return 1;
}

/* Does the range [p, end) contain another access to the same XMM register?
 * Used to reject a lazy entry load when the first write also reads the old
 * value, for example `c->xmm[0] = c->xmm[0]`. */
static int xmm_same_access(const char *s, size_t n, size_t p, size_t end,
                           int idx)
{
    while (p < end) {
        const char *hit = strstr(s + p, "c->xmm[");
        size_t pos;
        size_t after;
        int other;
        if (!hit) return 0;
        pos = (size_t)(hit - s);
        if (pos >= end) return 0;
        if (pos > 0 && word_char(s[pos - 1])) {
            p = pos + 7;
            continue;
        }
        other = parse_xmm_access(s, n, pos, &after);
        if (other < 0) {
            p = pos + 7;
            continue;
        }
        if (other == idx) return 1;
        p = after;
    }
    return 0;
}

/* The generated form `sse_load_ss(&c->xmm[i], value)` writes the whole
 * register, upper lanes zero included.  Recognize the helper immediately
 * before the address-of operator. */
static int xmm_full_write_helper(const char *s, size_t pos)
{
    size_t p = pos;
    size_t e;
    while (p > 0 && (s[p - 1] == ' ' || s[p - 1] == '\t')) p--;
    if (p == 0 || s[p - 1] != '&') return 0;
    p--;
    while (p > 0 && (s[p - 1] == ' ' || s[p - 1] == '\t')) p--;
    if (p > 0 && s[p - 1] == '(') p--;
    e = p;
    while (p > 0 && word_char(s[p - 1])) p--;
    if (e - p == 11 && strncmp(s + p, "sse_load_ss", 11) == 0) return 1;
    if (e - p == 11 && strncmp(s + p, "sse_load_sd", 11) == 0) return 1;
    return 0;
}

/* Decide whether the first access to one register makes an entry load dead.
 * full is set for a complete 16-byte overwrite; lane is 1/2 for a simple
 * low-f32/low-f64 store.  The final region kind decides which of those covers
 * every later read, so the caller applies that check after the scan. */
static void xmm_first_write(const char *s, size_t n, size_t pos, size_t after,
                            size_t re, int idx, int *full, int *lane)
{
    size_t le = after;
    size_t p;
    size_t q;
    *full = 0;
    *lane = 0;
    while (le < re && s[le] != '\n') le++;
    if (!xmm_write_access(s, n, pos, after)) return;
    if (xmm_full_write_helper(s, pos)) {
        if (!xmm_same_access(s, n, after, le, idx)) *full = 1;
        return;
    }
    p = after;
    while (p < le && (s[p] == ' ' || s[p] == '\t')) p++;
    if (p < le && s[p] == '=' && !(p + 1 < le && s[p + 1] == '=')) {
        if (!xmm_same_access(s, n, p + 1, le, idx)) *full = 1;
        return;
    }
    if (low_lane_suffix(s, le, p, ".f32[0]")) {
        q = p + 7;
        while (q < le && (s[q] == ' ' || s[q] == '\t')) q++;
        if (q < le && s[q] == '=' && !(q + 1 < le && s[q + 1] == '=') &&
            !xmm_same_access(s, n, q + 1, le, idx))
            *lane = 1;
    } else if (low_lane_suffix(s, le, p, ".f64[0]")) {
        q = p + 7;
        while (q < le && (s[q] == ' ' || s[q] == '\t')) q++;
        if (q < le && s[q] == '=' && !(q + 1 < le && s[q + 1] == '=') &&
            !xmm_same_access(s, n, q + 1, le, idx))
            *lane = 2;
    }
}

/* Classify the accesses of one region.  kind is 1 for a low f32 lane, 2 for a
 * low f64 lane and 3 for a whole register or any other lane.  A low-lane
 * access can use a 4/8-byte load and store; a whole register uses a full
 * 16-byte copy.  Dynamic accesses make the whole region untransformable.
 * no_load marks registers whose first access overwrites the whole value that
 * the region could read, so the entry load can be elided. */
static int block_region_scan(const char *s, size_t n, size_t rs, size_t re,
                             unsigned char sel[8], int kind[8],
                             unsigned char no_load[8])
{
    int count[8];
    int k[8];
    unsigned char first[8];
    unsigned char first_full[8];
    unsigned char first_lane[8];
    size_t i = rs;
    int any = 0;
    memset(count, 0, sizeof count);
    memset(k, 0, sizeof k);
    memset(sel, 0, 8);
    memset(no_load, 0, 8);
    memset(first, 0, sizeof first);
    memset(first_full, 0, sizeof first_full);
    memset(first_lane, 0, sizeof first_lane);
    while (i < re) {
        const char *hit = strstr(s + i, "c->xmm[");
        size_t pos;
        size_t after;
        size_t p;
        int idx;
        if (!hit) break;
        pos = (size_t)(hit - s);
        if (pos >= re) break;
        if (pos > 0 && word_char(s[pos - 1])) {
            i = pos + 7;
            continue;
        }
        idx = parse_xmm_access(s, n, pos, &after);
        if (idx < 0 || after > re) return 0;
        if (!first[idx]) {
            int full = 0;
            int lane = 0;
            first[idx] = 1;
            xmm_first_write(s, n, pos, after, re, idx, &full, &lane);
            first_full[idx] = (unsigned char)full;
            first_lane[idx] = (unsigned char)lane;
        }
        count[idx]++;
        p = pos;
        while (p > 0 && (s[p - 1] == ' ' || s[p - 1] == '\t')) p--;
        if (p > 0 && s[p - 1] == '&') {
            k[idx] = 3;
        } else {
            int ak = 3;
            p = after;
            while (p < re && (s[p] == ' ' || s[p] == '\t')) p++;
            if (low_lane_suffix(s, re, p, ".f32[0]")) ak = 1;
            else if (low_lane_suffix(s, re, p, ".f64[0]")) ak = 2;
            if (k[idx] == 0) k[idx] = ak;
            else if (k[idx] != ak) k[idx] = 3;
        }
        i = after;
    }
    for (i = 0; i < 8; i++) {
        kind[i] = k[i];
        if (count[i] >= ((k[i] == 1 || k[i] == 2) ? 2 : 3)) {
            sel[i] = 1;
            any = 1;
            if ((k[i] == 3 && first_full[i]) ||
                (k[i] == 1 && first_lane[i] == 1) ||
                (k[i] == 2 && first_lane[i] == 2))
                no_load[i] = 1;
        }
    }
    return any;
}

static void block_write_dirty(Buf *b, const unsigned char dirty[8],
                              const int kind[8])
{
    int i;
    int full = 0;
    for (i = 0; i < 8; i++)
        if (dirty[i] && kind[i] == 3) full++;
    for (i = 0; i < 8; i++) {
        if (!dirty[i]) continue;
        if (kind[i] == 1)
            buf_printf(b,
                       "    c->xmm[%d].f32[0] = _wc3_xmm_local[%d].f32[0];\n",
                       i, i);
        else if (kind[i] == 2)
            buf_printf(b,
                       "    c->xmm[%d].f64[0] = _wc3_xmm_local[%d].f64[0];\n",
                       i, i);
        else if (full >= XMM_WRITEBACK_VECTOR_MIN)
            buf_printf(b,
                       "    xmm_store_full(&c->xmm[%d], &_wc3_xmm_local[%d]);\n",
                       i, i);
        else
            buf_printf(b, "    c->xmm[%d] = _wc3_xmm_local[%d];\n", i, i);
    }
}

/* Emit one region with a local cache.  On any unexpected shape the output is
 * rolled back and the caller copies the original text. */
static int block_emit_region(const char *body, size_t n, const char *masked,
                             size_t rs, size_t re, Buf *out,
                             unsigned *out_mask)
{
    unsigned char sel[8];
    unsigned char no_load[8];
    unsigned char dirty[8];
    int kind[8];
    size_t start = out->n;
    size_t i;
    size_t last;
    int any;

    if (rs >= re) return 0;
    any = block_region_scan(masked, n, rs, re, sel, kind, no_load);
    if (!any) return 0;
    memset(dirty, 0, sizeof dirty);
    if (out->n > 0 && out->p[out->n - 1] != '\n') buf_str(out, "\n");
    for (i = 0; i < 8; i++) {
        if (!sel[i]) continue;
        if (block_lazy_enabled && no_load[i]) {
            if (out_mask) *out_mask |= 1u << i;
            continue;
        }
        if (kind[i] == 1)
            buf_printf(out,
                       "    _wc3_xmm_local[%zu].f32[0] = c->xmm[%zu].f32[0];\n",
                       i, i);
        else if (kind[i] == 2)
            buf_printf(out,
                       "    _wc3_xmm_local[%zu].f64[0] = c->xmm[%zu].f64[0];\n",
                       i, i);
        else
            buf_printf(out, "    _wc3_xmm_local[%zu] = c->xmm[%zu];\n", i, i);
        if (out_mask) *out_mask |= 1u << i;
    }
    last = rs;
    i = rs;
    while (i < re) {
        const char *hit = strstr(masked + i, "c->xmm[");
        size_t pos;
        size_t after;
        int idx;
        if (!hit) break;
        pos = (size_t)(hit - masked);
        if (pos >= re) break;
        if (pos > 0 && word_char(masked[pos - 1])) {
            i = pos + 7;
            continue;
        }
        idx = parse_xmm_access(masked, n, pos, &after);
        if (idx < 0 || after > re) {
            out->n = start;
            return 0;
        }
        if (!sel[idx]) {
            i = after;
            continue;
        }
        buf_add(out, body + last, pos - last);
        buf_printf(out, "_wc3_xmm_local[%d]", idx);
        if (xmm_write_access(masked, n, pos, after)) dirty[idx] = 1;
        last = after;
        i = after;
    }
    buf_add(out, body + last, re - last);
    if (out->n > 0 && out->p[out->n - 1] != '\n') buf_str(out, "\n");
    block_write_dirty(out, dirty, kind);
    return 1;
}

/* The outer closing brace is handled separately so a writeback can never be
 * emitted after it.  If the closing brace shares a line with code, that whole
 * line is copied unchanged; this loses one line of promotion but keeps the
 * grammar and scope rules simple. */
static int transform_blocks(const char *body, size_t n, const char *masked,
                            Buf *out, unsigned *out_mask)
{
    size_t body_start = out->n;
    size_t final_close = n;
    size_t final_ls;
    size_t i;
    size_t region_start = SIZE_MAX;
    int skip_indent = -1;
    int replaced = 0;
    unsigned mask = 0;

    while (final_close > 0 && (body[final_close - 1] == ' ' ||
                               body[final_close - 1] == '\t' ||
                               body[final_close - 1] == '\r' ||
                               body[final_close - 1] == '\n'))
        final_close--;
    if (final_close > 0) final_close--;
    final_ls = final_close;
    while (final_ls > 0 && body[final_ls - 1] != '\n') final_ls--;

    buf_str(out, "{\n    XMM _wc3_xmm_local[8];\n");
    i = 1;
    if (i + 1 < n && body[i] == '\r' && body[i + 1] == '\n') i += 2;
    else if (i < n && body[i] == '\n') i++;
    while (i < final_ls) {
        size_t ls = i;
        size_t le = i;
        size_t next;
        size_t p;
        int blank;
        int indent;
        while (le < final_ls && body[le] != '\n') le++;
        next = (le < final_ls) ? le + 1 : final_ls;
        p = ls;
        while (p < le && (masked[p] == ' ' || masked[p] == '\t' ||
                          masked[p] == '\r'))
            p++;
        blank = p >= le;
        indent = line_indent(body, ls, le);

        if (skip_indent >= 0) {
            if (blank || indent > skip_indent) {
                buf_add(out, body + ls, next - ls);
                i = next;
                continue;
            }
            skip_indent = -1;
        }
        if (blank) {
            if (region_start != SIZE_MAX) {
                i = next;
                continue;
            }
            buf_add(out, body + ls, next - ls);
            i = next;
            continue;
        }
        if (line_is_unbraced_control(masked, n, ls, le)) {
            if (region_start != SIZE_MAX) {
                unsigned rmask = 0;
                if (block_emit_region(body, n, masked, region_start, ls, out,
                                      &rmask)) {
                    mask |= rmask;
                    replaced = 1;
                } else {
                    buf_add(out, body + region_start, ls - region_start);
                }
                region_start = SIZE_MAX;
            }
            buf_add(out, body + ls, next - ls);
            skip_indent = indent;
            i = next;
            continue;
        }
        if (line_is_barrier(masked, n, ls, le)) {
            if (region_start != SIZE_MAX) {
                unsigned rmask = 0;
                if (block_emit_region(body, n, masked, region_start, ls, out,
                                      &rmask)) {
                    mask |= rmask;
                    replaced = 1;
                } else {
                    buf_add(out, body + region_start, ls - region_start);
                }
                region_start = SIZE_MAX;
            }
            buf_add(out, body + ls, next - ls);
            i = next;
            continue;
        }
        if (region_start == SIZE_MAX) region_start = ls;
        i = next;
    }
    if (region_start != SIZE_MAX) {
        unsigned rmask = 0;
        if (block_emit_region(body, n, masked, region_start, final_ls, out,
                              &rmask)) {
            mask |= rmask;
            replaced = 1;
        } else {
            buf_add(out, body + region_start, final_ls - region_start);
        }
    }
    buf_add(out, body + final_ls, n - final_ls);
    if (!replaced) {
        out->n = body_start;
        buf_add(out, body, n);
        if (out_mask) *out_mask = 0;
        return 0;
    }
    if (out->n > 0 && out->p[out->n - 1] != '\n') buf_str(out, "\n");
    if (out_mask) *out_mask = mask;
    return 1;
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
        unsigned char cache[8];
        unsigned char lazy[8];
        unsigned char plan_cache[8];
        unsigned char plan_lazy[8];
        unsigned plan_mask = 0;
        int plan_score = 0;
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
        memset(cache, 0, sizeof cache);
        memset(lazy, 0, sizeof lazy);
        memset(plan_cache, 0, sizeof plan_cache);
        memset(plan_lazy, 0, sizeof plan_lazy);
        ok = !has_multiline_comment(text + open, blen) &&
             eligible(masked, blen, used);
        if (ok) {
            if (packed_double_only(text + open, blen)) {
                if (!select_live_in(masked, blen, used, cache)) ok = 0;
            } else {
                memcpy(cache, used, sizeof cache);
            }
        }
        if (ok) {
            /* The planner decision is always collected for the report.  Full
             * masks and empty proposals collapse to the current mask; the
             * decision only drives code generation under --planner.  The
             * default policy remains live-in/full until object A/B accepts
             * the model. */
            plan_score = select_planned(masked, blen, used, cache, plan_cache,
                                        plan_lazy, &plan_mask);
            if (planner_enabled) {
                memcpy(cache, plan_cache, sizeof cache);
                memcpy(lazy, plan_lazy, sizeof lazy);
            } else if (lazy_enabled) {
                select_lazy_current(masked, blen, cache, lazy);
            }
        }
        if (ok) {
            size_t before = out->n;
            size_t accesses = count_xmm_accesses(masked, blen);
            unsigned mask = 0;
            char fname[64];
            const char *fstart = text + hdr + 5;
            const char *fend = (const char *)memchr(fstart, '(',
                                                    (size_t)(nl - fstart));
            size_t flen;
            unsigned lazy_mask = 0;
            int k;
            int body_changed;
            if (!fend) fend = nl;
            flen = (size_t)(fend - fstart);
            if (flen >= sizeof fname) flen = sizeof fname - 1;
            memcpy(fname, fstart, flen);
            fname[flen] = 0;
            for (k = 0; k < 8; k++) {
                if (cache[k]) mask |= 1u << k;
                if (lazy[k]) lazy_mask |= 1u << k;
            }
            body_changed = transform_body(text + open, blen, masked, cache,
                                          lazy, out);
            if (body_changed) {
                if (changes)
                    changes_add(changes, fname, accesses, blen, out->n - before,
                                mask, plan_mask, lazy_mask, 0u, plan_score);
                (*out_changed)++;
            }
        } else if (block_enabled &&
                   !has_multiline_comment(text + open, blen) &&
                   !has_preprocessor_line(masked, blen) &&
                   !parens_cross_lines(masked, blen) &&
                   strstr(masked, "_wc3_xmm_local") == NULL &&
                   collect_indices(masked, blen, used) == 0) {
            size_t before = out->n;
            size_t accesses = count_xmm_accesses(masked, blen);
            unsigned bmask = 0;
            char fname[64];
            const char *fstart = text + hdr + 5;
            const char *fend = (const char *)memchr(fstart, '(',
                                                    (size_t)(nl - fstart));
            size_t flen;
            int body_changed;
            if (!fend) fend = nl;
            flen = (size_t)(fend - fstart);
            if (flen >= sizeof fname) flen = sizeof fname - 1;
            memcpy(fname, fstart, flen);
            fname[flen] = 0;
            body_changed = transform_blocks(text + open, blen, masked, out,
                                            &bmask);
            if (body_changed) {
                if (changes)
                    changes_add(changes, fname, accesses, blen, out->n - before,
                                bmask, 0u, 0u, bmask, 0);
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
                "\"used_mask\":%u,\"plan_mask\":%u,\"lazy_mask\":%u,"
                "\"block_mask\":%u,\"plan_score\":%d,"
                "\"body_before\":%zu,\"body_after\":%zu}",
                ch->func, ch->accesses, ch->used_mask, ch->plan_mask,
                ch->lazy_mask, ch->block_mask, ch->plan_score,
                ch->bytes_before, ch->bytes_after);
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
    static const char *wide_return =
        "void L_0000001E(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[0] = c->xmm[3]; /* 00000000: movaps xmm0, xmm3 */\n"
        "    c->xmm[1] = c->xmm[3];\n"
        "    c->xmm[2] = c->xmm[3];\n"
        "    return;\n"
        "}\n";
    static const char *packed_double_return =
        "void L_0000001F(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[0] = c->xmm[3]; /* 00000000: movapd xmm0, xmm3 */\n"
        "    c->xmm[1] = c->xmm[3];\n"
        "    c->xmm[2] = c->xmm[3];\n"
        "    return;\n"
        "}\n";
    static const char *packed_double_no_live =
        "void L_00000020(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[0].u64[0] = 1u; /* 00000000: movapd xmm0, xmm1 */\n"
        "    c->xmm[1] = c->xmm[0];\n"
        "    c->xmm[2] = c->xmm[1];\n"
        "    return;\n"
        "}\n";
    static const char *planner_live =
        "void L_00000021(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[1].f32[0] = c->xmm[0].f32[0];\n"
        "    c->xmm[1].f32[1] = c->xmm[0].f32[1];\n"
        "    c->xmm[2].f32[0] = c->xmm[0].f32[0];\n"
        "    c->xmm[2].f32[1] = c->xmm[0].f32[1];\n"
        "    c->xmm[1].f32[0] = c->xmm[0].f32[2];\n"
        "    c->xmm[2].f32[0] = c->xmm[0].f32[3];\n"
        "    return;\n"
        "}\n";
    static const char *planner_lazy =
        "void L_00000022(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[2] = c->xmm[1];\n"
        "    c->xmm[0].f32[0] = c->xmm[2].f32[0];\n"
        "    c->xmm[0].f32[1] = c->xmm[2].f32[1];\n"
        "    c->xmm[0].f32[2] = c->xmm[2].f32[2];\n"
        "    c->xmm[0].f32[3] = c->xmm[2].f32[3];\n"
        "    c->xmm[3].f32[0] = c->xmm[2].f32[0];\n"
        "    return;\n"
        "}\n";
    static const char *planner_lazy_branch =
        "void L_00000023(CPU *restrict c)\n"
        "{\n"
        "    if (c->eax != 0) {\n"
        "        c->xmm[3] = c->xmm[1];\n"
        "    }\n"
        "    c->xmm[2] = c->xmm[1];\n"
        "    c->xmm[0].f32[0] = c->xmm[2].f32[0];\n"
        "    c->xmm[0].f32[1] = c->xmm[2].f32[1];\n"
        "    c->xmm[0].f32[2] = c->xmm[2].f32[2];\n"
        "    c->xmm[0].f32[3] = c->xmm[2].f32[3];\n"
        "    c->xmm[3].f32[0] = c->xmm[2].f32[0];\n"
        "    return;\n"
        "}\n";
    static const char *planner_lazy_lane =
        "void L_00000024(CPU *restrict c)\n"
        "{\n"
        "    c->xmm[2].f32[0] = 1.0f;\n"
        "    c->xmm[2].f32[1] = c->xmm[1].f32[0];\n"
        "    c->xmm[0].f32[0] = c->xmm[2].f32[0];\n"
        "    c->xmm[0].f32[1] = c->xmm[2].f32[1];\n"
        "    return;\n"
        "}\n";
    static const char *block_scalar =
        "void L_00000030(CPU *restrict c)\n"
        "{\n"
        "    switch (c->eax) {\n"
        "    case 0:\n"
        "        c->xmm[0].f32[0] = c->xmm[1].f32[0];\n"
        "        c->xmm[2].f32[0] = c->xmm[1].f32[0];\n"
        "        c->xmm[0].f32[0] = c->xmm[0].f32[0] + c->xmm[2].f32[0];\n"
        "        break;\n"
        "    default:\n"
        "        c->xmm[3] = c->xmm[4];\n"
        "        c->xmm[5] = c->xmm[3];\n"
        "        c->xmm[4] = c->xmm[5];\n"
        "        c->xmm[3] = c->xmm[4];\n"
        "        break;\n"
        "    }\n"
        "}\n";
    static const char *block_call =
        "void L_00000031(CPU *restrict c)\n"
        "{\n"
        "    switch (c->eax) {\n"
        "    case 0:\n"
        "        c->xmm[0].f32[0] = c->xmm[1].f32[0];\n"
        "        c->xmm[2].f32[0] = c->xmm[0].f32[0];\n"
        "        WC3_DIRECT_CALL_CLEAN(L_00000032, c);\n"
        "        c->xmm[0].f32[0] = c->xmm[2].f32[0];\n"
        "        c->xmm[1].f32[0] = c->xmm[0].f32[0];\n"
        "        break;\n"
        "    }\n"
        "}\n";
    static const char *block_unbraced =
        "void L_00000032(CPU *restrict c)\n"
        "{\n"
        "    if (c->eax)\n"
        "        c->xmm[0] = c->xmm[1];\n"
        "    c->xmm[2] = c->xmm[3];\n"
        "    c->xmm[2] = c->xmm[3];\n"
        "    c->xmm[2] = c->xmm[3];\n"
        "    return;\n"
        "}\n";
    static const char *block_else_if =
        "void L_00000033(CPU *restrict c)\n"
        "{\n"
        "    switch (c->eax) {\n"
        "    case 0:\n"
        "        if (c->ebx) {\n"
        "            c->xmm[0].f32[0] = c->xmm[1].f32[0];\n"
        "        } else if (c->ecx)\n"
        "            c->xmm[2].f32[0] = c->xmm[2].f32[0] + c->xmm[3].f32[0];\n"
        "        else\n"
        "            c->xmm[4].f32[0] = c->xmm[4].f32[0] + c->xmm[5].f32[0];\n"
        "        c->xmm[6].f32[0] = c->xmm[7].f32[0];\n"
        "        c->xmm[6].f32[0] = c->xmm[7].f32[0];\n"
        "        break;\n"
        "    }\n"
        "}\n";
    static const char *block_midline_goto =
        "void L_00000034(CPU *restrict c)\n"
        "{\n"
        "    switch (c->eax) {\n"
        "    case 0:\n"
        "        c->xmm[0].f32[0] = c->xmm[1].f32[0];\n"
        "        c->xmm[0].f32[0] = c->xmm[1].f32[0];\n"
        "        c->eax = 0; if (c->ecx) goto L_00000034_end;\n"
        "        c->xmm[2].f32[0] = c->xmm[3].f32[0];\n"
        "        c->xmm[2].f32[0] = c->xmm[3].f32[0];\n"
        "L_00000034_end:\n"
        "        break;\n"
        "    }\n"
        "}\n";
    static const char *block_lazy_full =
        "void L_00000035(CPU *restrict c)\n"
        "{\n"
        "    switch (c->eax) {\n"
        "    case 0:\n"
        "        sse_load_ss(&c->xmm[0], 1.0f);\n"
        "        c->xmm[0].f32[0] = c->xmm[0].f32[0] * 2.0f;\n"
        "        c->xmm[1].f32[0] = c->xmm[0].f32[0];\n"
        "        c->xmm[1].f32[0] = c->xmm[0].f32[0];\n"
        "        break;\n"
        "    }\n"
        "}\n";
    static const char *block_lazy_lane_read =
        "void L_00000036(CPU *restrict c)\n"
        "{\n"
        "    switch (c->eax) {\n"
        "    case 0:\n"
        "        c->xmm[0].f32[0] = c->xmm[0].f32[0] + 1.0f;\n"
        "        c->xmm[0].f32[0] = c->xmm[0].f32[0] + 2.0f;\n"
        "        break;\n"
        "    }\n"
        "}\n";
    static const char *block_lazy_unsafe =
        "void L_00000037(CPU *restrict c)\n"
        "{\n"
        "    switch (c->eax) {\n"
        "    case 0:\n"
        "        c->xmm[0].f32[0] = 1.0f;\n"
        "        c->xmm[0] = c->xmm[1];\n"
        "        c->xmm[0].f32[0] = c->xmm[2].f32[0];\n"
        "        break;\n"
        "    }\n"
        "}\n";
    static const char *block_lazy_self =
        "void L_00000038(CPU *restrict c)\n"
        "{\n"
        "    switch (c->eax) {\n"
        "    case 0:\n"
        "        c->xmm[0] = c->xmm[0];\n"
        "        c->xmm[0].f32[0] = 2.0f;\n"
        "        c->xmm[1].f32[0] = c->xmm[0].f32[0];\n"
        "        break;\n"
        "    }\n"
        "}\n";
    static const char *block_lazy_direct_full =
        "void L_00000039(CPU *restrict c)\n"
        "{\n"
        "    switch (c->eax) {\n"
        "    case 0:\n"
        "        c->xmm[0] = c->xmm[1];\n"
        "        c->xmm[0].f32[0] = c->xmm[0].f32[0] + 1.0f;\n"
        "        c->xmm[2].f32[0] = c->xmm[0].f32[0];\n"
        "        break;\n"
        "    }\n"
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

    transform_text(wide_return, strlen(wide_return), &out, &funcs, &changed,
                   &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].used_mask == 0xFu;
    ok &= check_contains("wide_return", out.p,
                         "xmm_store_full(&c->xmm[0], &_wc3_xmm_local[0]);");
    ok &= check_contains("wide_return", out.p,
                         "xmm_store_full(&c->xmm[1], &_wc3_xmm_local[1]);");
    ok &= check_contains("wide_return", out.p,
                         "xmm_store_full(&c->xmm[2], &_wc3_xmm_local[2]);");
    ok &= check_not_contains("wide_return", out.p,
                             "c->xmm[0] = _wc3_xmm_local[0];");
    ok &= count_substr(out.p, "xmm_store_full(") == 3;
    RESET_CASE();

    transform_text(packed_double_return, strlen(packed_double_return), &out,
                   &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].used_mask == 0x8u;
    ok &= check_contains("packed_double_return", out.p,
                         "_wc3_xmm_local[3] = c->xmm[3];");
    ok &= check_contains("packed_double_return", out.p,
                         "c->xmm[0] = _wc3_xmm_local[3];");
    ok &= check_not_contains("packed_double_return", out.p,
                             "c->xmm[0] = _wc3_xmm_local[0];");
    ok &= check_not_contains("packed_double_return", out.p,
                             "xmm_store_full(");
    RESET_CASE();

    transform_text(packed_double_no_live, strlen(packed_double_no_live), &out,
                   &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= changes.n == 0;
    ok &= strcmp(out.p, packed_double_no_live) == 0;
    ok &= check_not_contains("packed_double_no_live", out.p,
                             "_wc3_xmm_local");
    RESET_CASE();

    /* Report-only mode keeps the existing full cache for non-packed code but
     * records the planner proposal: only xmm0 is read-first and pays for the
     * entry load; xmm1/xmm2 are written first. */
    transform_text(planner_live, strlen(planner_live), &out, &funcs, &changed,
                   &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].used_mask == 7u;
    ok &= changes.n == 1 && changes.v[0].plan_mask == 1u;
    ok &= changes.n == 1 && changes.v[0].plan_score > 0;
    ok &= check_contains("planner_report", out.p,
                         "_wc3_xmm_local[1] = c->xmm[1];");
    RESET_CASE();

    /* --planner applies the same decision: written-first xmm1/xmm2 stay in
     * c->xmm, while xmm0 keeps the local copy. */
    planner_enabled = 1;
    transform_text(planner_live, strlen(planner_live), &out, &funcs, &changed,
                   &changes);
    planner_enabled = 0;
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].used_mask == 1u;
    ok &= changes.n == 1 && changes.v[0].plan_mask == 1u;
    ok &= check_contains("planner_apply", out.p,
                         "_wc3_xmm_local[0] = c->xmm[0];");
    ok &= check_not_contains("planner_apply", out.p,
                             "_wc3_xmm_local[1] = c->xmm[1];");
    ok &= check_contains("planner_apply", out.p,
                         "c->xmm[1].f32[0] = _wc3_xmm_local[0].f32[0];");
    RESET_CASE();

    /* A top-level full write may create the local in place.  xmm2 is written
     * before it is read, so the entry load is skipped while the later lane
     * reads still use the local. */
    planner_enabled = 1;
    transform_text(planner_lazy, strlen(planner_lazy), &out, &funcs, &changed,
                   &changes);
    planner_enabled = 0;
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].used_mask == 6u;
    ok &= changes.n == 1 && changes.v[0].plan_mask == 6u;
    ok &= changes.n == 1 && changes.v[0].lazy_mask == 4u;
    ok &= check_not_contains("planner_lazy", out.p,
                             "_wc3_xmm_local[2] = c->xmm[2];");
    ok &= check_contains("planner_lazy", out.p,
                         "_wc3_xmm_local[2] = _wc3_xmm_local[1];");
    ok &= check_contains("planner_lazy", out.p,
                         "c->xmm[0].f32[0] = _wc3_xmm_local[2].f32[0];");
    RESET_CASE();

    /* --lazy applies the same entry-load elision to the current full mask
     * without changing which registers are promoted. */
    lazy_enabled = 1;
    transform_text(planner_lazy, strlen(planner_lazy), &out, &funcs, &changed,
                   &changes);
    lazy_enabled = 0;
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].used_mask == 15u;
    ok &= changes.n == 1 && changes.v[0].lazy_mask == 4u;
    ok &= check_not_contains("lazy_flag", out.p,
                             "_wc3_xmm_local[2] = c->xmm[2];");
    ok &= check_contains("lazy_flag", out.p,
                         "_wc3_xmm_local[2] = _wc3_xmm_local[1];");
    RESET_CASE();

    /* The same written-first register is not lazy when a branch precedes its
     * first write: the entry load must remain, because a skipped branch would
     * otherwise leave the local uninitialized. */
    planner_enabled = 1;
    transform_text(planner_lazy_branch, strlen(planner_lazy_branch), &out,
                   &funcs, &changed, &changes);
    planner_enabled = 0;
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].used_mask == 6u;
    ok &= changes.n == 1 && changes.v[0].plan_mask == 6u;
    ok &= changes.n == 1 && changes.v[0].lazy_mask == 0u;
    ok &= check_contains("planner_lazy_branch", out.p,
                         "_wc3_xmm_local[2] = c->xmm[2];");
    RESET_CASE();

    /* A lane write preserves the upper lanes, so it cannot create the local
     * without loading the previous guest value. */
    planner_enabled = 1;
    transform_text(planner_lazy_lane, strlen(planner_lazy_lane), &out, &funcs,
                   &changed, &changes);
    planner_enabled = 0;
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].used_mask == 6u;
    ok &= changes.n == 1 && changes.v[0].lazy_mask == 0u;
    ok &= check_contains("planner_lazy_lane", out.p,
                         "_wc3_xmm_local[2] = c->xmm[2];");
    RESET_CASE();

    /* The block fallback is opt-in.  Without --block the switch function is
     * left byte-identical. */
    block_enabled = 0;
    transform_text(block_scalar, strlen(block_scalar), &out, &funcs, &changed,
                   &changes);
    ok &= funcs == 1 && changed == 0;
    ok &= out.n == strlen(block_scalar) &&
          memcmp(out.p, block_scalar, out.n) == 0;
    RESET_CASE();

    /* Low-lane accesses in a switch case use a scalar local and only write
     * the low lane back.  Whole-register accesses in the other case use the
     * XMM local and its normal writeback. */
    block_enabled = 1;
    transform_text(block_scalar, strlen(block_scalar), &out, &funcs, &changed,
                   &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].block_mask == 0x1Fu;
    ok &= check_contains("block_scalar", out.p,
                         "_wc3_xmm_local[0].f32[0] = c->xmm[0].f32[0];");
    ok &= check_contains("block_scalar", out.p,
                         "c->xmm[2].f32[0] = _wc3_xmm_local[2].f32[0];");
    ok &= check_contains("block_scalar", out.p,
                         "_wc3_xmm_local[3] = c->xmm[3];");
    ok &= check_not_contains("block_scalar", out.p, "xmm_store_full(");
    RESET_CASE();

    /* A guest call with the newer _CLEAN suffix is a real boundary.  The
     * block fallback must store before it and reload afterwards; a local may
     * not stay live across the call. */
    transform_text(block_call, strlen(block_call), &out, &funcs, &changed,
                   &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].block_mask == 0x1u;
    ok &= count_substr(out.p,
                       "_wc3_xmm_local[0].f32[0] = c->xmm[0].f32[0];") == 2;
    ok &= count_substr(out.p,
                       "c->xmm[0].f32[0] = _wc3_xmm_local[0].f32[0];") == 2;
    ok &= check_contains("block_call", out.p,
                         "WC3_DIRECT_CALL_CLEAN(L_00000032, c);");
    RESET_CASE();

    /* The body of an unbraced if is copied untouched.  The straight-line
     * statements after it are still promoted. */
    transform_text(block_unbraced, strlen(block_unbraced), &out, &funcs,
                   &changed, &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].block_mask == 0x0Cu;
    ok &= check_contains("block_unbraced", out.p,
                         "        c->xmm[0] = c->xmm[1];");
    ok &= check_not_contains("block_unbraced", out.p,
                             "_wc3_xmm_local[0]");
    ok &= check_not_contains("block_unbraced", out.p,
                             "_wc3_xmm_local[1]");
    ok &= check_contains("block_unbraced", out.p,
                         "_wc3_xmm_local[2] = c->xmm[2];");
    RESET_CASE();

    /* A `} else if (...)` line owns an unbraced body even though the line
     * starts with a closing brace.  The body must not receive an entry load or
     * an exit writeback, or it would execute unconditionally. */
    transform_text(block_else_if, strlen(block_else_if), &out, &funcs,
                   &changed, &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].block_mask == 0xC0u;
    ok &= check_contains("block_else_if", out.p,
                         "        } else if (c->ecx)");
    ok &= check_contains("block_else_if", out.p,
                         "            c->xmm[2].f32[0] = c->xmm[2].f32[0] + c->xmm[3].f32[0];");
    ok &= check_not_contains("block_else_if", out.p,
                             "_wc3_xmm_local[2]");
    ok &= check_not_contains("block_else_if", out.p,
                             "_wc3_xmm_local[4]");
    ok &= check_contains("block_else_if", out.p,
                         "_wc3_xmm_local[6].f32[0] = c->xmm[6].f32[0];");
    RESET_CASE();

    /* A goto after another statement on the same line is still a barrier.
     * The preceding region must flush before it, not after the jump target. */
    transform_text(block_midline_goto, strlen(block_midline_goto), &out,
                   &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].block_mask == 0x0Fu;
    {
        const char *wb = strstr(out.p,
            "c->xmm[0].f32[0] = _wc3_xmm_local[0].f32[0];");
        const char *go = strstr(out.p,
            "        c->eax = 0; if (c->ecx) goto L_00000034_end;");
        ok &= wb != NULL && go != NULL && wb < go;
    }
    ok &= check_contains("block_midline_goto", out.p,
                         "        c->eax = 0; if (c->ecx) goto L_00000034_end;");
    RESET_CASE();

    /* --block-lazy elides block entry loads when the first access covers every
     * value the region can read.  A full helper store creates the local, and
     * a low-lane store does the same for a lane-only region. */
    block_lazy_enabled = 1;
    transform_text(block_lazy_full, strlen(block_lazy_full), &out, &funcs,
                   &changed, &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].block_mask == 0x3u;
    ok &= check_not_contains("block_lazy_full", out.p,
                             "_wc3_xmm_local[0] = c->xmm[0];");
    ok &= check_not_contains("block_lazy_full", out.p,
                             "_wc3_xmm_local[1].f32[0] = c->xmm[1].f32[0];");
    ok &= check_contains("block_lazy_full", out.p,
                         "sse_load_ss(&_wc3_xmm_local[0], 1.0f);");
    ok &= check_contains("block_lazy_full", out.p,
                         "c->xmm[1].f32[0] = _wc3_xmm_local[1].f32[0];");
    RESET_CASE();

    /* The first lane store reads the old lane on its right-hand side.  That
     * read must keep the entry load. */
    transform_text(block_lazy_lane_read, strlen(block_lazy_lane_read), &out,
                   &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].block_mask == 0x1u;
    ok &= count_substr(out.p,
                       "_wc3_xmm_local[0].f32[0] = c->xmm[0].f32[0];") == 1;
    RESET_CASE();

    /* A lane write followed by a full-register access makes the region
     * full-width.  The lane write alone does not initialize the upper lanes,
     * so the entry load must remain. */
    transform_text(block_lazy_unsafe, strlen(block_lazy_unsafe), &out, &funcs,
                   &changed, &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].block_mask == 0x1u;
    ok &= count_substr(out.p, "_wc3_xmm_local[0] = c->xmm[0];") == 1;
    RESET_CASE();

    /* A full write that reads the same register cannot create the local: the
     * old value is still needed. */
    transform_text(block_lazy_self, strlen(block_lazy_self), &out, &funcs,
                   &changed, &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].block_mask == 0x1u;
    ok &= count_substr(out.p, "_wc3_xmm_local[0] = c->xmm[0];") == 1;
    RESET_CASE();

    /* A direct full assignment from another register needs no entry load. */
    transform_text(block_lazy_direct_full, strlen(block_lazy_direct_full), &out,
                   &funcs, &changed, &changes);
    ok &= funcs == 1 && changed == 1;
    ok &= changes.n == 1 && changes.v[0].block_mask == 0x1u;
    ok &= check_not_contains("block_lazy_direct_full", out.p,
                             "_wc3_xmm_local[0] = c->xmm[0];");
    ok &= check_contains("block_lazy_direct_full", out.p,
                         "_wc3_xmm_local[0] = c->xmm[1];");
    RESET_CASE();
    block_lazy_enabled = 0;

    /* The fallback is idempotent: the generated local array makes the
     * function ineligible, and the fallback skips an already transformed
     * body. */
    block_enabled = 1;
    transform_text(block_scalar, strlen(block_scalar), &out, &funcs, &changed,
                   &changes);
    {
        char *first = (char *)xmalloc(out.n + 1);
        size_t first_n = out.n;
        memcpy(first, out.p, out.n);
        first[out.n] = 0;
        RESET_CASE();
        transform_text(first, first_n, &out, &funcs, &changed, &changes);
        ok &= funcs == 1 && changed == 0;
        ok &= out.n == first_n && memcmp(out.p, first, first_n) == 0;
        free(first);
        RESET_CASE();
    }
    block_enabled = 0;

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
    fputs("xmm_local: use --lift IN --out OUT [--json REPORT] [--planner] "
          "[--lazy] [--block] [--block-lazy] [--quiet]\n"
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
        if (strcmp(argv[i], "--planner") == 0) {
            planner_enabled = 1;
            continue;
        }
        if (strcmp(argv[i], "--lazy") == 0) {
            lazy_enabled = 1;
            continue;
        }
        if (strcmp(argv[i], "--block") == 0) {
            block_enabled = 1;
            continue;
        }
        if (strcmp(argv[i], "--block-lazy") == 0) {
            block_lazy_enabled = 1;
            continue;
        }
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
