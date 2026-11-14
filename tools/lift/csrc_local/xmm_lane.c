/* xmm_lane.c - D3c first increment: exact upper-lane zero reuse.
 *
 * xmm_local turns CPU.xmm references into a local XMM[8] and writes only
 * changed registers back to the CPU.  That leaves every value with a full
 * 128-bit store even when the function only ever works on the low f32 lane
 * and the high three lanes are already zero.
 *
 * This pass tracks which high lanes are provably zero.  When a full
 * assignment would only rewrite those zero lanes to zero, it is reduced to
 * the lane that actually changes:
 *
 *   sse_load_ss(&l[i], v)  ->  l[i].f32[0] = v
 *   l[d] = l[s]            ->  l[d].f32[0] = l[s].f32[0]
 *   pxor l[i], l[i]        ->  l[i].u32[0] = 0
 *
 * The state is monotone and conservative.  It starts unknown, is copied by
 * full movaps assignments, and is reset at any control-flow point that can
 * merge a different path.  A conditional block that only returns or tails is
 * special: the fall-through path keeps its state unchanged, so the zero
 * proof survives it.
 *
 * This is an exact transform, not a dead-upper heuristic: every replaced
 * store still leaves the same 128-bit value in the local register.  No
 * address allowlist, profile switch or guest-ABI change is involved.
 */

#include <errno.h>
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
    unsigned char zero[8]; /* bit 0: u32[1], bit 1: u32[2], bit 2: u32[3] */
} LaneState;

typedef struct {
    int in_comment;
} MaskState;

static void die(const char *msg)
{
    fprintf(stderr, "xmm_lane: %s\n", msg);
    exit(1);
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
    if (b->n + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + extra + 1) cap *= 2;
    b->p = xrealloc(b->p, cap);
    b->cap = cap;
}

static void buf_add(Buf *b, const void *p, size_t n)
{
    buf_need(b, n);
    if (n) memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void buf_printf(Buf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) die("vsnprintf failed");
    if ((size_t)n < sizeof(tmp)) {
        buf_add(b, tmp, (size_t)n);
    } else {
        char *big = xmalloc((size_t)n + 1);
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        buf_add(b, big, (size_t)n);
        free(big);
    }
}

static char *slurp(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    char *p = NULL;
    size_t n = 0, cap = 0;
    if (!f) {
        fprintf(stderr, "xmm_lane: cannot open %s: %s\n", path, strerror(errno));
        exit(1);
    }
    for (;;) {
        size_t got;
        if (n + 65536 + 1 > cap) {
            cap = cap ? cap * 2 : 1 << 20;
            p = xrealloc(p, cap);
        }
        got = fread(p + n, 1, 65536, f);
        n += got;
        if (got < 65536) {
            if (ferror(f)) {
                fprintf(stderr, "xmm_lane: read %s failed: %s\n", path, strerror(errno));
                exit(1);
            }
            break;
        }
    }
    fclose(f);
    if (!p) p = xmalloc(1);
    p[n] = 0;
    *out_n = n;
    return p;
}

static int write_all(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    if (n && fwrite(data, 1, n, f) != n) {
        fclose(f);
        return 0;
    }
    if (fclose(f) != 0) return 0;
    return 1;
}

static int word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int has_word(const char *s, size_t n, const char *w)
{
    size_t wl = strlen(w);
    size_t i;
    if (wl == 0 || wl > n) return 0;
    for (i = 0; i + wl <= n; i++) {
        if (memcmp(s + i, w, wl) != 0) continue;
        if (i > 0 && word_char(s[i - 1])) continue;
        if (i + wl < n && word_char(s[i + wl])) continue;
        return 1;
    }
    return 0;
}

/* Mask comments and string bodies while preserving byte offsets. */
static char *mask_line(const char *s, size_t n, MaskState *st)
{
    char *m = xmalloc(n + 1);
    size_t i = 0;
    memcpy(m, s, n);
    m[n] = 0;
    while (i < n) {
        if (st->in_comment) {
            if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') {
                m[i] = m[i + 1] = ' ';
                i += 2;
                st->in_comment = 0;
            } else {
                m[i] = (s[i] == '\n' || s[i] == '\r') ? s[i] : ' ';
                i++;
            }
            continue;
        }
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            m[i] = m[i + 1] = ' ';
            i += 2;
            st->in_comment = 1;
            continue;
        }
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') {
                m[i] = ' ';
                i++;
            }
            continue;
        }
        if (s[i] == '"' || s[i] == '\'') {
            char quote = s[i];
            m[i] = ' ';
            i++;
            while (i < n) {
                if (s[i] == '\\' && i + 1 < n) {
                    m[i] = m[i + 1] = ' ';
                    i += 2;
                    continue;
                }
                if (s[i] == quote) {
                    m[i] = ' ';
                    i++;
                    break;
                }
                m[i] = ' ';
                i++;
            }
            continue;
        }
        m[i] = s[i];
        i++;
    }
    return m;
}

static size_t skip_ws(const char *s, size_t n, size_t i)
{
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) i++;
    return i;
}

/* A single '=', not the first byte of '=='. */
static int plain_assign(const char *s, size_t n, size_t q)
{
    return q < n && s[q] == '=' && (q + 1 >= n || s[q + 1] != '=');
}

static int only_ws(const char *s, size_t n, size_t from)
{
    return skip_ws(s, n, from) == n;
}

static int parse_local(const char *s, size_t n, size_t i, int *idx, size_t *after)
{
    static const char head[] = "_wc3_xmm_local[";
    size_t p = i + sizeof(head) - 1;
    size_t q = p;
    int value = 0;
    if (i + sizeof(head) - 1 > n) return 0;
    if (memcmp(s + i, head, sizeof(head) - 1) != 0) return 0;
    if (q >= n || s[q] < '0' || s[q] > '9') return 0;
    while (q < n && s[q] >= '0' && s[q] <= '9') {
        value = value * 10 + (s[q] - '0');
        q++;
    }
    if (q >= n || s[q] != ']' || value < 0 || value > 7) return 0;
    *idx = value;
    *after = q + 1;
    return 1;
}

static size_t matching_paren(const char *s, size_t n, size_t open)
{
    int depth = 0;
    size_t i;
    for (i = open; i < n; i++) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') {
            depth--;
            if (depth == 0) return i;
        }
    }
    return (size_t)-1;
}

static size_t find_bytes(const char *s, size_t n, size_t from, const char *needle)
{
    size_t nl = strlen(needle);
    size_t i;
    if (nl == 0 || nl > n) return (size_t)-1;
    for (i = from; i + nl <= n; i++) {
        if (memcmp(s + i, needle, nl) == 0) return i;
    }
    return (size_t)-1;
}

static int starts_control_label(const char *s, size_t n)
{
    size_t i = skip_ws(s, n, 0);
    size_t j;
    if (i >= n || !word_char(s[i])) return 0;
    j = i;
    while (j < n && word_char(s[j])) j++;
    j = skip_ws(s, n, j);
    return j < n && s[j] == ':';
}

static int is_control_line(const char *s, size_t n)
{
    if (has_word(s, n, "if") || has_word(s, n, "else") ||
        has_word(s, n, "for") || has_word(s, n, "while") ||
        has_word(s, n, "do") || has_word(s, n, "switch") ||
        has_word(s, n, "goto") || has_word(s, n, "return") ||
        has_word(s, n, "break") || has_word(s, n, "continue") ||
        has_word(s, n, "case") || has_word(s, n, "default"))
        return 1;
    if (find_bytes(s, n, 0, "WC3_DIRECT_TAIL") != (size_t)-1) return 1;
    if (find_bytes(s, n, 0, "dispatch_jmp") != (size_t)-1) return 1;
    if (find_bytes(s, n, 0, "WC3_DIRECT_CALL") != (size_t)-1) return 1;
    if (find_bytes(s, n, 0, "WC3_GUEST_DRAIN") != (size_t)-1) return 1;
    if (find_bytes(s, n, 0, "dispatch(") != (size_t)-1) return 1;
    if (starts_control_label(s, n)) return 1;
    return 0;
}

/* True when this line can only leave the current path, so the fall-through
 * state is untouched.  A flush of locals to c->xmm is a read, not a write. */
static int has_local_write(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t at = find_bytes(s, n, i, "_wc3_xmm_local");
        int idx;
        size_t after, q;
        if (at == (size_t)-1) break;
        if (!parse_local(s, n, at, &idx, &after)) {
            i = at + 1;
            continue;
        }
        q = skip_ws(s, n, after);
        if (plain_assign(s, n, q)) return 1;
        if (q < n && s[q] == '.') {
            q++;
            while (q < n && word_char(s[q])) q++;
            if (q < n && s[q] == '[') {
                q++;
                while (q < n && s[q] >= '0' && s[q] <= '9') q++;
                if (q < n && s[q] == ']') {
                    q = skip_ws(s, n, q + 1);
                    if (plain_assign(s, n, q)) return 1;
                }
            }
        }
        i = after;
    }
    return 0;
}

static int is_exit_only_if(const char *s, size_t n)
{
    if (!has_word(s, n, "if")) return 0;
    if (has_word(s, n, "else") || has_word(s, n, "goto")) return 0;
    if (!has_word(s, n, "return") &&
        find_bytes(s, n, 0, "WC3_DIRECT_TAIL") == (size_t)-1 &&
        find_bytes(s, n, 0, "dispatch_jmp") == (size_t)-1)
        return 0;
    if (has_local_write(s, n)) return 0;
    return 1;
}

static int rhs_is_zero(const char *s, size_t n, size_t i)
{
    i = skip_ws(s, n, i);
    if (i >= n) return 0;
    if (s[i] != '0') return 0;
    i++;
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        if (i < n && (s[i] == 'f' || s[i] == 'F')) i++;
    }
    i = skip_ws(s, n, i);
    if (i >= n) return 1;
    return s[i] == ';' || s[i] == ',' || s[i] == ')' || s[i] == '}';
}

/* Classify the compound lane writes emitted for one guest instruction. */
static int compound_dest(const char *s, size_t n, int *out_dest,
                         int *out_lanes, unsigned char *out_zero)
{
    int dest = -1;
    int writes = 0;
    int z2 = 0;
    int z3 = 0;
    int movq = 0;
    int full = 0;
    size_t i = 0;
    while (i < n) {
        size_t at = find_bytes(s, n, i, "_wc3_xmm_local");
        int d;
        size_t after, q, field_at, idx;
        char field[8];
        int flen = 0;
        if (at == (size_t)-1) break;
        if (!parse_local(s, n, at, &d, &after)) {
            i = at + 1;
            continue;
        }
        q = skip_ws(s, n, after);
        if (q >= n || s[q] != '.') {
            i = after;
            continue;
        }
        q++;
        field_at = q;
        while (q < n && word_char(s[q]) && flen < (int)sizeof(field) - 1) {
            field[flen++] = s[q++];
        }
        field[flen] = 0;
        if (q >= n || s[q] != '[') {
            i = after;
            continue;
        }
        q++;
        idx = 0;
        if (q >= n || s[q] < '0' || s[q] > '9') {
            i = after;
            continue;
        }
        while (q < n && s[q] >= '0' && s[q] <= '9') {
            idx = idx * 10 + (size_t)(s[q] - '0');
            q++;
        }
        if (q >= n || s[q] != ']') {
            i = after;
            continue;
        }
        q = skip_ws(s, n, q + 1);
        if (!plain_assign(s, n, q)) {
            i = after;
            continue;
        }
        (void)field_at;
        if (dest < 0) dest = d;
        else if (dest != d) return 0;
        writes++;
        q++;
        if (rhs_is_zero(s, n, q)) {
            if (strcmp(field, "i32") == 0 || strcmp(field, "u32") == 0) {
                if (idx == 2) z2 = 1;
                if (idx == 3) z3 = 1;
            }
            if ((strcmp(field, "u64") == 0) && idx == 1) movq = 1;
        }
        if ((strcmp(field, "f32") == 0 && idx == 3) ||
            (strcmp(field, "f64") == 0 && idx == 1) ||
            (strcmp(field, "u32") == 0 && idx == 3) ||
            (strcmp(field, "i32") == 0 && idx == 3) ||
            (strcmp(field, "u64") == 0 && idx == 1))
            full = 1;
        i = after;
    }
    if (writes < 2 || dest < 0) return 0;
    if (out_dest) *out_dest = dest;
    if (out_lanes) *out_lanes = writes;
    /* cvtpd2ps writes both low f32 lanes and both high i32 lanes as zero.
     * Requiring the exact four-write shape keeps a mixed union line from
     * being mistaken for it.  movq/movd write u64[1] = 0 and are exactly
     * two writes.  Everything else is a full-width write. */
    if (z2 && z3 && writes == 4) {
        *out_zero = 6;
        return 1;
    }
    if (movq && writes == 2) {
        *out_zero = 6;
        return 1;
    }
    if (full) {
        *out_zero = 0;
        return 1;
    }
    return 0;
}

static int has_helper_address(const char *s, size_t n, int *dest, int *kind)
{
    size_t i = 0;
    while (i < n) {
        size_t at = find_bytes(s, n, i, "sse_");
        size_t open, q;
        int d;
        size_t after;
        if (at == (size_t)-1) return 0;
        q = at + 4;
        while (q < n && word_char(s[q])) q++;
        open = skip_ws(s, n, q);
        if (open < n && s[open] == '(') {
            size_t p = skip_ws(s, n, open + 1);
            if (p < n && s[p] == '&' && parse_local(s, n, p + 1, &d, &after)) {
                *dest = d;
                if (strncmp(s + at, "sse_load_ss", 11) == 0) *kind = 1;
                else if (strncmp(s + at, "sse_load_sd", 11) == 0) *kind = 2;
                else if (strncmp(s + at, "sse_pxor", 8) == 0) *kind = 3;
                else *kind = 4;
                return 1;
            }
        }
        i = at + 4;
    }
    return 0;
}

static int self_pxor(const char *s, size_t n, int *dest, size_t *start, size_t *end)
{
    size_t i = 0;
    while (i < n) {
        size_t at = find_bytes(s, n, i, "{");
        size_t p;
        int d1, d2;
        size_t after;
        if (at == (size_t)-1) return 0;
        p = skip_ws(s, n, at + 1);
        if (p + 3 <= n && memcmp(s + p, "XMM", 3) == 0 &&
            (p + 3 == n || !word_char(s[p + 3]))) {
            p = skip_ws(s, n, p + 3);
            if (p + 2 <= n && memcmp(s + p, "_s", 2) == 0) {
                p = skip_ws(s, n, p + 2);
                if (plain_assign(s, n, p)) {
                    p = skip_ws(s, n, p + 1);
                    if (parse_local(s, n, p, &d1, &after)) {
                        p = skip_ws(s, n, after);
                        if (p < n && s[p] == ';') {
                            p = skip_ws(s, n, p + 1);
                            if (p + 8 <= n && memcmp(s + p, "sse_pxor", 8) == 0) {
                                p += 8;
                                p = skip_ws(s, n, p);
                                if (p < n && s[p] == '(') {
                                    p = skip_ws(s, n, p + 1);
                                    if (p < n && s[p] == '&' &&
                                        parse_local(s, n, p + 1, &d2, &after)) {
                                        p = skip_ws(s, n, after);
                                        if (p < n && s[p] == ',') {
                                            p = skip_ws(s, n, p + 1);
                                            if (p < n && s[p] == '&' &&
                                                p + 2 <= n && memcmp(s + p + 1, "_s", 2) == 0) {
                                                p = skip_ws(s, n, p + 3);
                                                if (p < n && s[p] == ')') {
                                                    p = skip_ws(s, n, p + 1);
                                                    if (p < n && s[p] == ';') {
                                                        p = skip_ws(s, n, p + 1);
                                                        if (p < n && s[p] == '}' && d1 == d2) {
                                                            *dest = d1;
                                                            *start = at;
                                                            *end = p;
                                                            return 1;
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        i = at + 1;
    }
    return 0;
}

static char *replace_range(const char *s, size_t n, size_t start, size_t end,
                           const char *repl, size_t *out_n)
{
    size_t rn = strlen(repl);
    char *p = xmalloc(n - (end - start) + rn + 1);
    memcpy(p, s, start);
    memcpy(p + start, repl, rn);
    memcpy(p + start + rn, s + end, n - end);
    p[n - (end - start) + rn] = 0;
    *out_n = n - (end - start) + rn;
    return p;
}

static int rewrite_load(const char *search, const char *raw, size_t n,
                        const LaneState *st, int sd, char **out, size_t *out_n,
                        int *dest)
{
    const char *name = sd ? "sse_load_sd" : "sse_load_ss";
    size_t nl = strlen(name);
    size_t i = 0;
    while (i + nl <= n) {
        size_t at = find_bytes(search, n, i, name);
        size_t open, close, arg, expr_start, expr_end;
        int d;
        size_t after;
        char *expr;
        if (at == (size_t)-1) return 0;
        if (at != skip_ws(search, n, 0)) {
            i = at + nl;
            continue;
        }
        open = skip_ws(search, n, at + nl);
        if (open >= n || search[open] != '(') {
            i = at + nl;
            continue;
        }
        close = matching_paren(search, n, open);
        if (close == (size_t)-1) return 0;
        if (close + 1 >= n || search[close + 1] != ';' ||
            !only_ws(search, n, close + 2)) {
            i = close + 1;
            continue;
        }
        arg = skip_ws(search, n, open + 1);
        if (arg >= n || search[arg] != '&' ||
            !parse_local(search, n, arg + 1, &d, &after)) {
            i = close + 1;
            continue;
        }
        {
            size_t comma = skip_ws(search, n, after);
            if (comma >= n || search[comma] != ',') {
                i = close + 1;
                continue;
            }
            expr_start = skip_ws(search, n, comma + 1);
            expr_end = close;
            while (expr_end > expr_start &&
                   (raw[expr_end - 1] == ' ' || raw[expr_end - 1] == '\t' ||
                    raw[expr_end - 1] == '\r'))
                expr_end--;
            if (expr_end <= expr_start) {
                i = close + 1;
                continue;
            }
            expr = xmalloc(expr_end - expr_start + 1);
            memcpy(expr, raw + expr_start, expr_end - expr_start);
            expr[expr_end - expr_start] = 0;
        }
        if (find_bytes(expr, strlen(expr), 0, "_wc3_xmm_local") != (size_t)-1) {
            free(expr);
            i = close + 1;
            continue;
        }
        if (sd) {
            if ((st->zero[d] & 6) != 6) {
                free(expr);
                i = close + 1;
                continue;
            }
        } else {
            if (st->zero[d] != 7) {
                free(expr);
                i = close + 1;
                continue;
            }
        }
        {
            Buf repl = {0};
            char *r;
            size_t rn;
            buf_printf(&repl, "_wc3_xmm_local[%d].%s[0] = %s",
                       d, sd ? "f64" : "f32", expr);
            r = replace_range(raw, n, at, close + 1, repl.p, &rn);
            free(repl.p);
            *out = r;
            *out_n = rn;
        }
        free(expr);
        *dest = d;
        return 1;
    }
    return 0;
}

static int rewrite_copy(const char *search, const char *raw, size_t n,
                        const LaneState *st, char **out, size_t *out_n,
                        int *dd, int *ss)
{
    size_t i = 0;
    while (i < n) {
        size_t at = find_bytes(search, n, i, "_wc3_xmm_local[");
        int d, src;
        size_t after, q, start2, after2;
        char repl[128];
        if (at == (size_t)-1) return 0;
        if (at != skip_ws(search, n, 0)) {
            i = at + 1;
            continue;
        }
        if (!parse_local(search, n, at, &d, &after)) {
            i = at + 1;
            continue;
        }
        q = skip_ws(search, n, after);
        if (!plain_assign(search, n, q)) {
            i = after;
            continue;
        }
        q = skip_ws(search, n, q + 1);
        if (q >= n || !parse_local(search, n, q, &src, &after2)) {
            i = after;
            continue;
        }
        start2 = skip_ws(search, n, after2);
        if (start2 >= n || search[start2] != ';' ||
            !only_ws(search, n, start2 + 1)) {
            i = after;
            continue;
        }
        if (st->zero[d] == 7 && st->zero[src] == 7) {
            snprintf(repl, sizeof(repl),
                     "_wc3_xmm_local[%d].f32[0] = _wc3_xmm_local[%d].f32[0];",
                     d, src);
            *out = replace_range(raw, n, at, start2 + 1, repl, out_n);
            *dd = d;
            *ss = src;
            return 1;
        }
        i = start2 + 1;
    }
    return 0;
}

static int rewrite_pxor(const char *search, const char *raw, size_t n,
                        const LaneState *st, char **out, size_t *out_n,
                        int *dest)
{
    int d;
    size_t start, end;
    char repl[96];
    if (!self_pxor(search, n, &d, &start, &end)) return 0;
    if (start != skip_ws(search, n, 0) || !only_ws(search, n, end + 1))
        return 0;
    if (st->zero[d] != 7) return 0;
    snprintf(repl, sizeof(repl), "_wc3_xmm_local[%d].u32[0] = 0;", d);
    *out = replace_range(raw, n, start, end + 1, repl, out_n);
    *dest = d;
    return 1;
}

/* Count local actions on a line.  A flush (c->xmm = l) is a read and does
 * not count.  Multiple actions are left untouched: the ordered state update
 * below is deliberately only used for one action per line. */
static int count_actions(const char *s, size_t n, int *compound)
{
    int actions = 0;
    int helpers = 0;
    int assigns = 0;
    int lanes = 0;
    size_t i = 0;
    int expected_lanes = 0;
    unsigned char dummy;
    *compound = 0;
    if (compound_dest(s, n, NULL, &expected_lanes, &dummy)) {
        *compound = 1;
    }
    while (i < n) {
        size_t at = find_bytes(s, n, i, "_wc3_xmm_local");
        int d;
        size_t after, q;
        if (at == (size_t)-1) break;
        if (!parse_local(s, n, at, &d, &after)) {
            i = at + 1;
            continue;
        }
        q = skip_ws(s, n, after);
        if (plain_assign(s, n, q)) {
            assigns++;
        } else if (q < n && s[q] == '.') {
            q++;
            while (q < n && word_char(s[q])) q++;
            if (q < n && s[q] == '[') {
                q++;
                while (q < n && s[q] >= '0' && s[q] <= '9') q++;
                if (q < n && s[q] == ']') {
                    q = skip_ws(s, n, q + 1);
                    if (plain_assign(s, n, q)) lanes++;
                }
            }
        }
        i = after;
    }
    {
        size_t p = 0;
        while (p < n) {
            size_t at = find_bytes(s, n, p, "sse_");
            size_t q, open;
            if (at == (size_t)-1) break;
            q = at + 4;
            while (q < n && word_char(s[q])) q++;
            open = skip_ws(s, n, q);
            if (open < n && s[open] == '(') {
                size_t arg = skip_ws(s, n, open + 1);
                if (arg < n && s[arg] == '&') {
                    int d;
                    size_t after;
                    if (parse_local(s, n, arg + 1, &d, &after)) helpers++;
                }
            }
            p = at + 4;
        }
    }
    actions = helpers + assigns;
    if (!*compound) {
        actions += lanes;
    } else if (lanes == expected_lanes && lanes > 0) {
        /* One compound guest instruction with exactly its expected lane
         * writes is a single action.  Any extra lane write is a second
         * action and falls back to the conservative merge reset. */
        actions += 1;
    } else {
        actions += lanes;
    }
    return actions;
}

static void update_state(const char *s, size_t n, LaneState *st)
{
    int compound = 0;
    int actions;
    int cdest = -1;
    int clanes = 0;
    unsigned char cz = 0;
    if (find_bytes(s, n, 0, "XMM _wc3_xmm_local[8]") != (size_t)-1) {
        memset(st->zero, 0, sizeof(st->zero));
        return;
    }
    actions = count_actions(s, n, &compound);
    if (actions == 0) return;
    if (actions > 1) {
        memset(st->zero, 0, sizeof(st->zero));
        return;
    }
    if (compound && compound_dest(s, n, &cdest, &clanes, &cz)) {
        st->zero[cdest] = cz;
        return;
    }
    /* One sse_* helper taking &local[d]. */
    {
        int d = -1, kind = 0;
        if (has_helper_address(s, n, &d, &kind)) {
            if (kind == 1) st->zero[d] = 7;
            else if (kind == 2) st->zero[d] = 6;
            else if (kind == 3) {
                int pd;
                size_t ps, pe;
                if (self_pxor(s, n, &pd, &ps, &pe) && pd == d) st->zero[d] = 7;
                else st->zero[d] = 0;
            } else {
                st->zero[d] = 0;
            }
            return;
        }
    }
    /* One plain local assignment.  It need not be the first local
     * reference on the line: a flush or a store can precede it. */
    {
        size_t p = 0;
        while (p < n) {
            size_t q = find_bytes(s, n, p, "_wc3_xmm_local[");
            int d;
            size_t after, eq;
            if (q == (size_t)-1) break;
            if (!parse_local(s, n, q, &d, &after)) {
                p = q + 1;
                continue;
            }
            eq = skip_ws(s, n, after);
            if (plain_assign(s, n, eq)) {
                size_t rhs = skip_ws(s, n, eq + 1);
                int src;
                size_t after2, semi;
                if (parse_local(s, n, rhs, &src, &after2)) {
                    semi = skip_ws(s, n, after2);
                    if (semi < n && s[semi] == ';') {
                        st->zero[d] = st->zero[src];
                    } else {
                        st->zero[d] = 0;
                    }
                } else {
                    st->zero[d] = 0;
                }
                return;
            }
            p = after;
        }
    }
    /* One direct lane write. */
    {
        size_t p = 0;
        while (p < n) {
            size_t q = find_bytes(s, n, p, "_wc3_xmm_local[");
            int d;
            size_t after, f, idx;
            char field[8];
            int flen = 0;
            if (q == (size_t)-1) break;
            if (!parse_local(s, n, q, &d, &after)) {
                p = q + 1;
                continue;
            }
            f = skip_ws(s, n, after);
            if (f >= n || s[f] != '.') {
                p = after;
                continue;
            }
            f++;
            while (f < n && word_char(s[f]) && flen < (int)sizeof(field) - 1)
                field[flen++] = s[f++];
            field[flen] = 0;
            if (f >= n || s[f] != '[') {
                p = after;
                continue;
            }
            f++;
            idx = 0;
            while (f < n && s[f] >= '0' && s[f] <= '9') {
                idx = idx * 10 + (size_t)(s[f] - '0');
                f++;
            }
            if (f >= n || s[f] != ']') {
                p = after;
                continue;
            }
            f = skip_ws(s, n, f + 1);
            if (!plain_assign(s, n, f)) {
                p = after;
                continue;
            }
            f++;
            if (rhs_is_zero(s, n, f)) {
                if ((strcmp(field, "u32") == 0 || strcmp(field, "i32") == 0 ||
                     strcmp(field, "f32") == 0) && idx >= 1 && idx <= 3)
                    st->zero[d] |= (unsigned char)(1u << (idx - 1));
                else if ((strcmp(field, "u64") == 0 || strcmp(field, "f64") == 0) && idx == 1)
                    st->zero[d] |= 6;
                else if ((strcmp(field, "u64") == 0 || strcmp(field, "f64") == 0) && idx == 0)
                    st->zero[d] |= 1;
            } else {
                if ((strcmp(field, "u32") == 0 || strcmp(field, "i32") == 0 ||
                     strcmp(field, "f32") == 0) && idx >= 1 && idx <= 3)
                    st->zero[d] &= (unsigned char)~(1u << (idx - 1));
                else if ((strcmp(field, "u64") == 0 || strcmp(field, "f64") == 0) && idx == 0)
                    st->zero[d] &= (unsigned char)~1u;
                else if ((strcmp(field, "u64") == 0 || strcmp(field, "f64") == 0) && idx == 1)
                    st->zero[d] &= (unsigned char)~6u;
            }
            p = after;
        }
    }
}

#if WC3_DEBUG && WC3_DEBUG_LIFT
/* WC3_DEBUG_LIFT: D3c lane-decision trace.  The block is compiled out in
 * release, so the pass output and the host tool stay byte-identical.  Env:
 *   WC3_DEBUG_LIFT=1        enable counters/trace support
 *   WC3_DEBUG_LIFT_TRACE=1  print per-site decisions
 *   WC3_DEBUG_LIFT_ADDR=HEX only trace this function (0 = all)
 */
static int g_lane_dbg_on;
static int g_lane_dbg_trace;
static uint32_t g_lane_dbg_addr;

static void lane_dbg_init(void)
{
    const char *s;
    g_lane_dbg_on = 0;
    g_lane_dbg_trace = 0;
    g_lane_dbg_addr = 0;
    s = getenv("WC3_DEBUG_LIFT");
    if (s && *s == '1') g_lane_dbg_on = 1;
    s = getenv("WC3_DEBUG_LIFT_TRACE");
    if (s && *s == '1') g_lane_dbg_trace = 1;
    s = getenv("WC3_DEBUG_LIFT_ADDR");
    if (s && *s) g_lane_dbg_addr = (uint32_t)strtoul(s, NULL, 16);
}

static int lane_dbg_sel(uint32_t addr)
{
    return g_lane_dbg_addr == 0 || g_lane_dbg_addr == addr;
}

static uint32_t lane_dbg_addr_of(const char *func)
{
    if (!func || func[0] != 'L' || func[1] != '_') return 0;
    return (uint32_t)strtoul(func + 2, NULL, 16);
}

static void lane_dbg_func(const char *func, uint32_t addr, size_t line)
{
    if (!g_lane_dbg_on || !g_lane_dbg_trace || !lane_dbg_sel(addr)) return;
    fprintf(stderr, "[lanedbg] %s:%zu function start\n", func, line);
}

static void lane_dbg_reset(const char *func, size_t line, const char *why,
                           const char *text)
{
    if (!g_lane_dbg_on || !g_lane_dbg_trace ||
        !lane_dbg_sel(lane_dbg_addr_of(func))) return;
    fprintf(stderr, "[lanedbg] %s:%zu reset (%s) %.180s\n",
            func, line, why, text ? text : "");
}

static void lane_dbg_site(const char *func, size_t line, const char *kind,
                          int d, int s, unsigned char before,
                          unsigned char after, const char *raw,
                          const char *repl)
{
    if (!g_lane_dbg_on || !g_lane_dbg_trace ||
        !lane_dbg_sel(lane_dbg_addr_of(func))) return;
    fprintf(stderr, "[lanedbg] %s:%zu site=%s d=%d", func, line, kind, d);
    if (s >= 0) fprintf(stderr, " s=%d", s);
    fprintf(stderr, " zero=%u->%u\n  in : %.200s\n  out: %.200s\n",
            (unsigned)before, (unsigned)after, raw, repl);
}

static void lane_dbg_skip(const char *func, size_t line, const char *why,
                          int d, unsigned char zero, const char *text)
{
    if (!g_lane_dbg_on || !g_lane_dbg_trace ||
        !lane_dbg_sel(lane_dbg_addr_of(func))) return;
    fprintf(stderr, "[lanedbg] %s:%zu skip (%s) d=%d zero=%u %.200s\n",
            func, line, why, d, (unsigned)zero, text ? text : "");
}
#else
#define lane_dbg_init() ((void)0)
#define lane_dbg_func(func, addr, line) ((void)0)
#define lane_dbg_reset(func, line, why, text) ((void)0)
#define lane_dbg_site(func, line, kind, d, s, before, after, raw, repl) \
    ((void)0)
#define lane_dbg_skip(func, line, why, d, zero, text) ((void)0)
#endif

#if WC3_DEBUG && WC3_DEBUG_LIFT
static int parse_func_header(const char *s, size_t n, char *out, size_t cap,
                             uint32_t *addr)
{
    size_t i = skip_ws(s, n, 0);
    size_t start;
    uint32_t v = 0;
    if (i + 4 > n || memcmp(s + i, "void", 4) != 0) return 0;
    i = skip_ws(s, n, i + 4);
    if (i + 2 > n || s[i] != 'L' || s[i + 1] != '_') return 0;
    i += 2;
    start = i;
    while (i < n) {
        int c = (unsigned char)s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | (uint32_t)d;
        i++;
    }
    if (i == start || i >= n || s[i] != '(') return 0;
    if (cap) {
        size_t len = i - start + 2;
        if (len >= cap) len = cap - 1;
        out[0] = 'L';
        out[1] = '_';
        if (len > 2) memcpy(out + 2, s + start, len - 2);
        out[len] = 0;
    }
    if (addr) *addr = v;
    return 1;
}

static int has_lane_candidate(const char *s, size_t n)
{
    int d;
    size_t after, q;
    if (find_bytes(s, n, 0, "sse_load_ss") != (size_t)-1 ||
        find_bytes(s, n, 0, "sse_load_sd") != (size_t)-1 ||
        find_bytes(s, n, 0, "sse_pxor") != (size_t)-1)
        return 1;
    q = skip_ws(s, n, 0);
    if (!parse_local(s, n, q, &d, &after)) return 0;
    q = skip_ws(s, n, after);
    if (!plain_assign(s, n, q)) return 0;
    q = skip_ws(s, n, q + 1);
    if (!parse_local(s, n, q, &d, &after)) return 0;
    q = skip_ws(s, n, after);
    return q < n && s[q] == ';' && only_ws(s, n, q + 1);
}
#endif

static int check_contains(const char *name, const char *text, const char *needle)
{
    if (!strstr(text, needle)) {
        fprintf(stderr, "xmm_lane self-test: %s missing: %s\n", name, needle);
        return 0;
    }
    return 1;
}

static int check_not_contains(const char *name, const char *text, const char *needle)
{
    if (strstr(text, needle)) {
        fprintf(stderr, "xmm_lane self-test: %s unexpected: %s\n", name, needle);
        return 0;
    }
    return 1;
}

static char *run_pass(const char *text, size_t n, size_t *out_n,
                      size_t *funcs, size_t *changed, size_t *sites)
{
    Buf out = {0};
    MaskState ms = {0};
    LaneState st;
    size_t pos = 0;
#if WC3_DEBUG && WC3_DEBUG_LIFT
    size_t line_no = 1;
    char cur_func[32] = {0};
    uint32_t cur_addr = 0;
#endif
    int func_changed = 0;
    memset(&st, 0, sizeof(st));
    *funcs = 0;
    *changed = 0;
    *sites = 0;
    while (pos < n) {
        size_t end = pos;
        char *line, *masked;
        size_t ln;
        int control, exit_only;
        while (end < n && text[end] != '\n') end++;
        ln = end - pos;
        line = xmalloc(ln + 1);
        memcpy(line, text + pos, ln);
        line[ln] = 0;
        masked = mask_line(line, ln, &ms);
#if WC3_DEBUG && WC3_DEBUG_LIFT
        {
            char fname[32];
            uint32_t faddr = 0;
            if (parse_func_header(line, ln, fname, sizeof(fname), &faddr)) {
                memcpy(cur_func, fname, sizeof(cur_func));
                cur_addr = faddr;
                lane_dbg_func(cur_func, cur_addr, line_no);
            }
        }
#endif
        if (find_bytes(masked, ln, 0, "XMM _wc3_xmm_local[8]") != (size_t)-1) {
            if (func_changed) (*changed)++;
            func_changed = 0;
            (*funcs)++;
            memset(st.zero, 0, sizeof(st.zero));
#if WC3_DEBUG && WC3_DEBUG_LIFT
            lane_dbg_func(cur_func, cur_addr, line_no);
#endif
        }
        control = is_control_line(masked, ln);
        exit_only = control && is_exit_only_if(masked, ln);
        if (control && !exit_only) {
            memset(st.zero, 0, sizeof(st.zero));
#if WC3_DEBUG && WC3_DEBUG_LIFT
            lane_dbg_reset(cur_func, line_no, "control", line);
#endif
            buf_add(&out, line, ln);
        } else if (!control) {
            int compound = 0;
            int actions = count_actions(masked, ln, &compound);
            char *repl = NULL;
            size_t rn = 0;
            int d = -1;
            int ssrc = -1;
            int did = 0;
            const char *kind = NULL;
            unsigned char before = 0, after = 0;
            if (actions <= 1) {
                if (!did) {
                    did = rewrite_load(masked, line, ln, &st, 0, &repl, &rn, &d);
                    if (did) kind = "load_ss";
                }
                if (!did && !compound) {
                    did = rewrite_load(masked, line, ln, &st, 1, &repl, &rn, &d);
                    if (did) kind = "load_sd";
                }
                if (!did && !compound) {
                    did = rewrite_copy(masked, line, ln, &st, &repl, &rn, &d,
                                       &ssrc);
                    if (did) kind = "copy";
                }
                if (!did && !compound) {
                    did = rewrite_pxor(masked, line, ln, &st, &repl, &rn, &d);
                    if (did) kind = "pxor";
                }
            }
            if (d >= 0) before = st.zero[d];
            if (did) {
                (*sites)++;
                func_changed = 1;
                buf_add(&out, repl, rn);
            } else {
                buf_add(&out, line, ln);
            }
            update_state(masked, ln, &st);
#if WC3_DEBUG && WC3_DEBUG_LIFT
            if (d >= 0) after = st.zero[d];
            if (did) {
                lane_dbg_site(cur_func, line_no, kind, d, ssrc, before,
                              after, line, repl);
            } else if (has_lane_candidate(masked, ln)) {
                const char *why = actions > 1 ? "multi_action" :
                                  compound ? "compound" : "state";
                lane_dbg_skip(cur_func, line_no, why, d, before, line);
            }
#else
            (void)kind;
            (void)before;
            (void)after;
#endif
            if (did) free(repl);
        } else {
            buf_add(&out, line, ln);
        }
        if (end < n) buf_add(&out, "\n", 1);
        free(masked);
        free(line);
        pos = end + (end < n ? 1 : 0);
#if WC3_DEBUG && WC3_DEBUG_LIFT
        line_no++;
#endif
    }
    if (func_changed) (*changed)++;
    buf_need(&out, 0);
    *out_n = out.n;
    return out.p;
}

static int self_test(void)
{
    const char *with_xmm =
        "void L_1(CPU *restrict c)\n"
        "{\n"
        "    XMM _wc3_xmm_local[8];\n"
        "    _wc3_xmm_local[0] = c->xmm[0];\n"
        "    sse_load_ss(&_wc3_xmm_local[0], rdss((c->eax)));\n"
        "    _wc3_xmm_local[0].f32[0] = _wc3_xmm_local[0].f32[0] + 1.0f;\n"
        "    sse_load_ss(&_wc3_xmm_local[0], rdss((c->ebx)));\n"
        "    _wc3_xmm_local[1] = c->xmm[1];\n"
        "    sse_load_ss(&_wc3_xmm_local[1], rdss((c->edx)));\n"
        "    _wc3_xmm_local[1] = _wc3_xmm_local[0];\n"
        "    { XMM _s = _wc3_xmm_local[1]; sse_pxor(&_wc3_xmm_local[1], &_s); }\n"
        "    sse_load_ss(&_wc3_xmm_local[1], rdss((c->ecx)));\n"
        "    c->xmm[0] = _wc3_xmm_local[0];\n"
        "    c->xmm[1] = _wc3_xmm_local[1];\n"
        "}\n";
    const char *branch =
        "void L_2(CPU *restrict c)\n"
        "{\n"
        "    XMM _wc3_xmm_local[8];\n"
        "    sse_load_ss(&_wc3_xmm_local[0], rdss((c->eax)));\n"
        "    if (c->cf) { c->xmm[0] = _wc3_xmm_local[0]; return; }\n"
        "    sse_load_ss(&_wc3_xmm_local[0], rdss((c->ebx)));\n"
        "    if (c->zf) { _wc3_xmm_local[0].f32[1] = 1.0f; }\n"
        "    sse_load_ss(&_wc3_xmm_local[0], rdss((c->ecx)));\n"
        "}\n";
    const char *cvt =
        "void L_3(CPU *restrict c)\n"
        "{\n"
        "    XMM _wc3_xmm_local[8];\n"
        "    sse_load_sd(&_wc3_xmm_local[0], rdsd((c->eax)));\n"
        "    sse_load_sd(&_wc3_xmm_local[0], rdsd((c->ebx)));\n"
        "    { XMM _s = _wc3_xmm_local[0]; _wc3_xmm_local[0].f32[0] = (float)_s.f64[0]; _wc3_xmm_local[0].f32[1] = (float)_s.f64[1]; _wc3_xmm_local[0].i32[2] = 0; _wc3_xmm_local[0].i32[3] = 0; }\n"
        "    sse_load_sd(&_wc3_xmm_local[0], rdsd((c->edx)));\n"
        "    sse_load_ss(&_wc3_xmm_local[0], rdss((c->ecx)));\n"
        "}\n";
    const char *plain = "void L_4(CPU *restrict c) { c->eax = 1; }\n";
    const char *read_before_copy =
        "void L_8(CPU *restrict c)\n"
        "{\n"
        "    XMM _wc3_xmm_local[8];\n"
        "    sse_load_ss(&_wc3_xmm_local[1], rdss((c->eax)));\n"
        "    c->eax = (int)_wc3_xmm_local[0].f32[0]; _wc3_xmm_local[0] = _wc3_xmm_local[1];\n"
        "    sse_load_ss(&_wc3_xmm_local[0], rdss((c->edx)));\n"
        "}\n";
    const char *comment =
        "void L_6(CPU *restrict c)\n"
        "{\n"
        "    XMM _wc3_xmm_local[8];\n"
        "    sse_load_ss(&_wc3_xmm_local[0], rdss((c->eax))); /* sse_load_ss(&_wc3_xmm_local[0], rdss((c->ebx))); */\n"
        "    sse_load_ss(&_wc3_xmm_local[0], rdss((c->ecx)));\n"
        "}\n";
    char longdoc[1024];
    {
        char plus[400];
        size_t k;
        plus[0] = 0;
        for (k = 0; k < 24; k++) strcat(plus, " + c->eax");
        snprintf(longdoc, sizeof(longdoc),
                 "void L_7(CPU *restrict c)\n"
                 "{\n"
                 "    XMM _wc3_xmm_local[8];\n"
                 "    sse_load_ss(&_wc3_xmm_local[0], rdss((c->eax)));\n"
                 "    sse_load_ss(&_wc3_xmm_local[0], rdss((c->eax%s)));\n"
                 "}\n", plus);
    }
    size_t n, funcs, changed, sites;
    char *out;
    int ok = 1;

    out = run_pass(with_xmm, strlen(with_xmm), &n, &funcs, &changed, &sites);
    ok &= funcs == 1;
    ok &= check_contains("load reuse", out,
                         "_wc3_xmm_local[0].f32[0] = rdss((c->ebx));");
    ok &= check_contains("copy reuse", out,
                         "_wc3_xmm_local[1].f32[0] = _wc3_xmm_local[0].f32[0];");
    ok &= check_contains("pxor reuse", out, "_wc3_xmm_local[1].u32[0] = 0;");
    ok &= check_not_contains("load reuse", out,
                             "sse_load_ss(&_wc3_xmm_local[0], rdss((c->ebx)))");
    free(out);

    out = run_pass(branch, strlen(branch), &n, &funcs, &changed, &sites);
    ok &= check_contains("exit-only branch", out,
                         "_wc3_xmm_local[0].f32[0] = rdss((c->ebx));");
    ok &= check_not_contains("exit-only branch", out,
                             "sse_load_ss(&_wc3_xmm_local[0], rdss((c->ebx)))");
    ok &= check_contains("control line kept", out, "if (c->cf)");
    ok &= check_contains("control line kept", out, "if (c->zf)");
    ok &= check_contains("write resets lane", out,
                         "sse_load_ss(&_wc3_xmm_local[0], rdss((c->ecx)))");
    free(out);

    out = run_pass(cvt, strlen(cvt), &n, &funcs, &changed, &sites);
    ok &= check_contains("sd reuse", out,
                         "_wc3_xmm_local[0].f64[0] = rdsd((c->ebx));");
    ok &= check_contains("cvtpd2ps reuse", out,
                         "_wc3_xmm_local[0].f64[0] = rdsd((c->edx));");
    ok &= check_contains("sd upper", out,
                         "sse_load_ss(&_wc3_xmm_local[0], rdss((c->ecx)))");
    free(out);

    out = run_pass(read_before_copy, strlen(read_before_copy), &n, &funcs,
                   &changed, &sites);
    ok &= check_contains("read before copy", out,
                         "_wc3_xmm_local[0].f32[0] = rdss((c->edx));");
    ok &= check_contains("read before copy kept", out,
                         "c->eax = (int)_wc3_xmm_local[0].f32[0]; _wc3_xmm_local[0] = _wc3_xmm_local[1];");
    free(out);

    out = run_pass(comment, strlen(comment), &n, &funcs, &changed, &sites);
    ok &= check_contains("comment kept", out,
                         "/* sse_load_ss(&_wc3_xmm_local[0], rdss((c->ebx))); */");
    ok &= check_contains("comment load reuse", out,
                         "_wc3_xmm_local[0].f32[0] = rdss((c->ecx));");
    free(out);

    out = run_pass(longdoc, strlen(longdoc), &n, &funcs, &changed, &sites);
    ok &= sites == 1;
    ok &= check_contains("long expression", out,
                         "_wc3_xmm_local[0].f32[0] = rdss((c->eax + c->eax");
    free(out);

    /* Idempotence: a second pass must find no lane site. */
    out = run_pass(with_xmm, strlen(with_xmm), &n, &funcs, &changed, &sites);
    {
        size_t n2, f2, c2, s2;
        char *again = run_pass(out, n, &n2, &f2, &c2, &s2);
        ok &= s2 == 0;
        ok &= n2 == n && memcmp(again, out, n) == 0;
        free(again);
    }
    free(out);

    out = run_pass(plain, strlen(plain), &n, &funcs, &changed, &sites);
    ok &= changed == 0;
    ok &= strcmp(out, plain) == 0;
    free(out);

    if (!ok) return 0;
    printf("xmm_lane self-test: OK\n");
    return 1;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: xmm_lane --lift <in.c> --out <out.c> [--json <report>] "
            "[--quiet]\n"
            "       xmm_lane --self-test\n");
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out_path = NULL, *json = NULL;
    int quiet = 0;
    int i;
    char *text, *out;
    size_t n, out_n, funcs, changed, sites;
    lane_dbg_init();
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--self-test") == 0) return self_test() ? 0 : 1;
        else if (strcmp(argv[i], "--lift") == 0 && i + 1 < argc) in = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_path = argv[++i];
        else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) json = argv[++i];
        else if (strcmp(argv[i], "--quiet") == 0) quiet = 1;
        else {
            usage();
            return 1;
        }
    }
    if (!in || !out_path) {
        usage();
        return 1;
    }
    text = slurp(in, &n);
    out = run_pass(text, n, &out_n, &funcs, &changed, &sites);
    if (!write_all(out_path, out, out_n)) {
        fprintf(stderr, "xmm_lane: cannot write %s: %s\n", out_path, strerror(errno));
        return 1;
    }
    if (json) {
        Buf j = {0};
        buf_printf(&j,
                   "{\"tool\":\"xmm_lane\",\"functions\":%zu,\"transformed\":%zu,"
                   "\"sites\":%zu,\"bytes_before\":%zu,\"bytes_after\":%zu}\n",
                   funcs, changed, sites, n, out_n);
        if (!write_all(json, j.p ? j.p : "", j.n)) {
            fprintf(stderr, "xmm_lane: cannot write %s: %s\n", json, strerror(errno));
            return 1;
        }
        free(j.p);
    }
    if (!quiet) {
        printf("xmm_lane: %zu functions, %zu transformed, %zu lane sites, "
               "%zu -> %zu bytes\n", funcs, changed, sites, n, out_n);
    }
    free(text);
    free(out);
    return 0;
}
