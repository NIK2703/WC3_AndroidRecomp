/* leaf_call.c - D4/D4b/D4c call-frame and tail ladder.
 *
 * Generated C replaces every `dispatch(c, VA)` with WC3_DIRECT_CALL and every
 * `dispatch_jmp(c, VA); return;` with WC3_DIRECT_TAIL.  The full call macro
 * saves five transition fields and drains a pending target after every call.
 * That is required only when the callee can publish a tail at its own level.
 * This pass classifies the complete corpus and chooses the smallest safe frame
 * for every direct call:
 *
 *   leaf      no guest call, tail, HLE/W3L helper or dispatcher access:
 *             WC3_DIRECT_CALL_LEAF, a depth-only frame.
 *   tail-free may enter other guests, but cannot publish pending at its own
 *             level: WC3_DIRECT_CALL_DEPTH, the same depth-only frame.
 *   tailful   may publish pending, so the caller drains after the call:
 *             WC3_DIRECT_CALL_CLEAN, which saves depth and draining only.
 *   unknown   target did not parse as a known function: leave the full
 *             WC3_DIRECT_CALL.
 *
 * A tail to a bare `ret` leaf needs no pending slot at all.  The leaf cannot
 * call, publish a tail, or read dispatcher state; its whole body is the guest
 * return-address pop.  In a one-tail body the generated
 * `WC3_DIRECT_TAIL; return;` becomes `WC3_DIRECT_TAIL_LEAF; return;`, a direct
 * sibling call.  Non-ret leaf targets and mixed-tail bodies stay on the drain
 * path: direct calls to a non-trivial leaf let the compiler inline its body,
 * and a mixed body changes tail/merge decisions even when every target is a
 * leaf.  The release A/B measured both effects as caller growth (D4c audit,
 * CODEGEN_FINDINGS_2026-11-16).
 *
 * The clean form and the leaf-tail form rely on a structural invariant: at
 * every reachable direct call site the pending slot is empty.  Generated tails
 * are followed by `return;`, and a nested direct call owns and drains its own
 * frame.  The pass checks the caller body and falls back to the full macro if it
 * sees a manual pending write or a tail without its return, so a future corpus
 * cannot silently break the invariant.  Leaf and depth calls do not touch
 * pending; a leaf-tail rewrite is also rejected unless the containing body
 * passes the same pending-safety check.
 *
 * With --gs-cookie, the exact MSVC /GS cookie guard body
 * (`flags_sub(ecx,cookie); if (!zf) tail(fail); esp += 4`) is replaced by a
 * specialized clean-frame helper.  A generated call site must have the form
 * `push32(ret); [xmm spill;] WC3_DIRECT_CALL[_CLEAN](guard, c)`.  The helper
 * is shared (external) below 20 guard sites in a TU and static above that
 * threshold, where constant propagation amortizes its body.  The standalone
 * tool switch stays off unless --gs-cookie=1 is passed; the product build
 * selects it with -DWC3_LEAF_GS_COOKIE=ON (adopted 2026-11-21 by user
 * request; no PM-19 device A/B has been run yet).
 *
 * This pass runs once over all final relifted chunks, after every other
 * corpus text pass.  Its sets are global because splitcorpus can place a
 * caller and its callee in different translation units.
 *
 *   leaf_call --inputs FILE --out-dir DIR [--report JSON] [--quiet]
 *             [--gs-cookie[=0|1]]
 *   leaf_call --self-test
 *
 * `--inputs` is a text file with one final chunk path per line.  Output files
 * keep the input basename.  A function that cannot be parsed is never used as
 * a proof, so an unfamiliar generated form can only miss an optimization, not
 * change dispatch semantics.
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

#define DIRECT_PREFIX "WC3_DIRECT_"
#define DIRECT_PREFIX_LEN (sizeof(DIRECT_PREFIX) - 1u)
#define CALL_MACRO "WC3_DIRECT_CALL"
#define CALL_MACRO_LEN (sizeof(CALL_MACRO) - 1u)
#define CALL_LEAF CALL_MACRO "_LEAF"
#define CALL_DEPTH CALL_MACRO "_DEPTH"
#define CALL_CLEAN CALL_MACRO "_CLEAN"
#define CALL_CLEAN_LEN (sizeof(CALL_CLEAN) - 1u)
#define TAIL_MACRO "WC3_DIRECT_TAIL"
#define TAIL_MACRO_LEN (sizeof(TAIL_MACRO) - 1u)
#define TAIL_LEAF TAIL_MACRO "_LEAF"
#define GS_STATIC_MIN_SITES 20u

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
    uint32_t target;
    uint32_t cookie;
    uint32_t fail;
} Guard;

typedef struct {
    Guard *p;
    size_t n;
    size_t cap;
} GuardVec;

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

static void guard_push(GuardVec *v, uint32_t target, uint32_t cookie,
                       uint32_t fail)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2u : 16u;
        v->p = (Guard *)xrealloc(v->p, v->cap * sizeof *v->p);
    }
    v->p[v->n].target = target;
    v->p[v->n].cookie = cookie;
    v->p[v->n].fail = fail;
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

/* The only leaf shape that D4c may enter directly.  A bare `ret` thunk has no
 * state to optimise: its host frame is just the guest return-address pop.  The
 * broader leaf set stays on WC3_DIRECT_TAIL; allowing a direct call to a
 * non-trivial leaf lets the compiler inline its body and changed the release
 * corpus by more than the frame saved (see CODEGEN_FINDINGS_2026-11-16). */
static int body_is_ret_leaf(const char *s)
{
    static const char pattern[] = "{c->esp+=4;return;}";
    size_t i = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isspace(c)) continue;
        if (i >= sizeof(pattern) - 1u || c != (unsigned char)pattern[i]) return 0;
        i++;
    }
    return i == sizeof(pattern) - 1u;
}

/* Exact MSVC /GS cookie guard body:
 *
 *   flags_sub(c, c->ecx, rd32((GVA(cookie))), 4);
 *   if (!c->zf) { WC3_DIRECT_TAIL(L_fail, c); return; }
 *   c->esp += 4; return;
 *
 * The call site form is `push32(ret); WC3_DIRECT_CALL[_CLEAN](L_guard, c);`.
 * Only this exact body is eligible for the shared guard helper: a different
 * register, extra instruction, or a non-tail failure path stays on the generic
 * direct-call frame. */
static int parse_gs_cookie_guard(const char *s, uint32_t *cookie,
                                 uint32_t *fail)
{
    size_t n = strlen(s);
    char *compact = (char *)xmalloc(n + 1u);
    size_t i;
    size_t j = 0;
    unsigned ck = 0, fl = 0;
    int used = 0;
    int r;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isspace(c)) compact[j++] = (char)c;
    }
    compact[j] = 0;
    r = sscanf(compact,
               "{flags_sub(c,c->ecx,rd32((GVA(0x%8x))),4);"
               "if(!c->zf){WC3_DIRECT_TAIL(L_%8x,c);return;}"
               "c->esp+=4;return;}%n",
               &ck, &fl, &used);
    free(compact);
    if (r != 2 || used != (int)j) return 0;
    *cookie = (uint32_t)ck;
    *fail = (uint32_t)fl;
    return 1;
}

/* Count tail macros in a function body.  D4c is limited to one-tail bodies:
 * in a mixed body the direct form changes the compiler's tail/merge decisions
 * and can grow the caller even when every individual leaf is harmless. */
static size_t body_tail_count(const char *s)
{
    size_t n = 0;
    const char *p = s;
    while ((p = strstr(p, "WC3_DIRECT_TAIL")) != NULL) {
        n++;
        p += TAIL_MACRO_LEN;
    }
    return n;
}

/* A tail-free callee may enter other guest functions, but cannot publish a
 * pending target at its own level.  A nested WC3_DIRECT_CALL owns and drains
 * its own frame; an indirect `dispatch` drains before returning.  Any textual
 * pending write is rejected: even a clear could hide a more complex state
 * change that the simple structural proof does not cover. */
static int body_is_tail_free(const char *s)
{
    if (strstr(s, "WC3_DIRECT_TAIL")) return 0;
    if (strstr(s, "dispatch_jmp")) return 0;
    if (strstr(s, "wc3_guest_set_pending_fn")) return 0;
    if (strstr(s, "disp_pending_")) return 0;
    return 1;
}

static int word_at(const char *s, size_t n, size_t i, const char *w)
{
    size_t len = strlen(w);
    if (i + len > n) return 0;
    if (i > 0 && word_char((unsigned char)s[i - 1u])) return 0;
    if (strncmp(s + i, w, len) != 0) return 0;
    if (i + len < n && word_char((unsigned char)s[i + len])) return 0;
    return 1;
}

/* True when the statement beginning at `i` (a tail macro or dispatch_jmp)
 * ends at `;` and the next token is `return`.  This is the generated form that
 * leaves the pending slot empty on every reachable fall-through path. */
static int tail_followed_by_return(const char *s, size_t n, size_t i)
{
    size_t p = i;
    size_t depth = 0;
    while (p < n) {
        int c = (unsigned char)s[p];
        if (c == '(') {
            depth++;
        } else if (c == ')') {
            if (depth) depth--;
        } else if (c == ';' && depth == 0) {
            p++;
            break;
        }
        p++;
    }
    while (p < n && (s[p] == ' ' || s[p] == '\t' ||
                     s[p] == '\r' || s[p] == '\n')) p++;
    return word_at(s, n, p, "return");
}

/* The clean frame does not save pending_valid/kind/target, so the slot must be
 * empty at every call in this body.  A tail is safe only in the generated
 * `tail; return;` form; a manual pending write makes the body ineligible. */
static int body_pending_safe(const char *s)
{
    size_t n = strlen(s);
    const char *p;
    if (strstr(s, "disp_pending_")) return 0;
    if (strstr(s, "wc3_guest_set_pending_fn")) return 0;
    for (p = s; (p = strstr(p, "WC3_DIRECT_TAIL")) != NULL; p++) {
        size_t i = (size_t)(p - s);
        if (word_at(s, n, i, "WC3_DIRECT_TAIL") &&
            !tail_followed_by_return(s, n, i)) return 0;
    }
    for (p = s; (p = strstr(p, "dispatch_jmp")) != NULL; p++) {
        size_t i = (size_t)(p - s);
        if (word_at(s, n, i, "dispatch_jmp") &&
            !tail_followed_by_return(s, n, i)) return 0;
    }
    return 1;
}

static int call_target_from(const char *masked, size_t n, size_t pos,
                            size_t macro_len, int reject_suffix, uint32_t *out)
{
    size_t p = pos + macro_len;
    uint32_t v = 0;
    size_t i;
    if (pos > 0 && word_char((unsigned char)masked[pos - 1u])) return 0;
    if (reject_suffix && p < n && masked[p] == '_') return 0;
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

static int call_target(const char *masked, size_t n, size_t pos, uint32_t *out)
{
    return call_target_from(masked, n, pos, CALL_MACRO_LEN, 1, out);
}

static int clean_call_target(const char *masked, size_t n, size_t pos,
                             uint32_t *out)
{
    return call_target_from(masked, n, pos, CALL_CLEAN_LEN, 0, out);
}

/* End of a macro invocation, including the closing ')'.  The arguments are
 * simple generated tokens but scan parentheses so a future argument expression
 * cannot silently truncate the replacement. */
static size_t call_invocation_end(const char *masked, size_t n, size_t pos,
                                  size_t macro_len)
{
    size_t p = pos + macro_len;
    size_t depth = 0;
    while (p < n && (masked[p] == ' ' || masked[p] == '\t')) p++;
    if (p >= n || masked[p] != '(') return SIZE_MAX;
    for (; p < n; p++) {
        if (masked[p] == '(') {
            depth++;
        } else if (masked[p] == ')') {
            depth--;
            if (!depth) return p + 1u;
        }
    }
    return SIZE_MAX;
}

static int skip_index(const char *s, size_t end, size_t *p,
                      const char *prefix, char close)
{
    size_t len = strlen(prefix);
    size_t digits;
    if (*p + len > end || strncmp(s + *p, prefix, len) != 0) return 0;
    *p += len;
    digits = *p;
    while (*p < end && isdigit((unsigned char)s[*p])) (*p)++;
    if (*p == digits || *p >= end || s[*p] != close) return 0;
    (*p)++;
    return 1;
}

/* The post-P0 xmm pass can insert register spills between push32 and the
 * guard call on the same generated line.  Those stores cannot change esp or
 * the guest return address, so they do not invalidate the guard frame.  Accept
 * only the two exact spill forms emitted by that pass; any other intervening
 * statement keeps the generic clean frame. */
static int statement_is_xmm_spill(const char *s, size_t start, size_t end)
{
    size_t p = start;
    size_t len;
    while (p < end && isspace((unsigned char)s[p])) p++;

    len = strlen("xmm_store_full(");
    if (p + len <= end && strncmp(s + p, "xmm_store_full(", len) == 0) {
        p += len;
        if (p >= end || s[p] != '&') return 0;
        p++;
        if (!skip_index(s, end, &p, "c->xmm[", ']')) return 0;
        while (p < end && isspace((unsigned char)s[p])) p++;
        if (p >= end || s[p] != ',') return 0;
        p++;
        while (p < end && isspace((unsigned char)s[p])) p++;
        if (p >= end || s[p] != '&') return 0;
        p++;
        if (!skip_index(s, end, &p, "_wc3_xmm_local[", ']')) return 0;
        while (p < end && isspace((unsigned char)s[p])) p++;
        if (p >= end || s[p] != ')') return 0;
        p++;
        while (p < end && isspace((unsigned char)s[p])) p++;
        return p == end;
    }

    if (!skip_index(s, end, &p, "c->xmm[", ']')) return 0;
    while (p < end && isspace((unsigned char)s[p])) p++;
    if (p >= end || s[p] != '=') return 0;
    p++;
    while (p < end && isspace((unsigned char)s[p])) p++;
    if (!skip_index(s, end, &p, "_wc3_xmm_local[", ']')) return 0;
    while (p < end && isspace((unsigned char)s[p])) p++;
    return p == end;
}

/* The guard helper owns the callee's return-address pop, so the call site must
 * be the generated `push32(ret); [xmm spill;] WC3_DIRECT_CALL...` sequence.
 * Check backwards over exact XMM spill statements instead of assuming that
 * push32 is the immediately preceding token. */
static int call_has_push32(const char *masked, size_t pos)
{
    size_t p = pos;
    for (;;) {
        size_t end;
        size_t start;
        while (p > 0 && isspace((unsigned char)masked[p - 1u])) p--;
        if (p == 0 || masked[p - 1u] != ';') return 0;
        end = p - 1u;
        while (end > 0 && isspace((unsigned char)masked[end - 1u])) end--;
        start = end;
        while (start > 0 && masked[start - 1u] != ';' &&
               masked[start - 1u] != '{' && masked[start - 1u] != '}') start--;
        while (start < end && isspace((unsigned char)masked[start])) start++;
        if (word_at(masked, end, start, "push32")) return 1;
        if (!statement_is_xmm_spill(masked, start, end)) return 0;
        p = start;
    }
}

static int tail_target(const char *masked, size_t n, size_t pos, uint32_t *out)
{
    size_t p = pos + TAIL_MACRO_LEN;
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

static int cmp_guard(const void *a, const void *b)
{
    uint32_t x = ((const Guard *)a)->target;
    uint32_t y = ((const Guard *)b)->target;
    return (x > y) - (x < y);
}

static const Guard *guard_find(const GuardVec *v, uint32_t target)
{
    Guard key;
    if (!v || !v->n) return NULL;
    key.target = target;
    key.cookie = 0;
    key.fail = 0;
    return (const Guard *)bsearch(&key, v->p, v->n, sizeof *v->p, cmp_guard);
}

static void sort_guards(GuardVec *v)
{
    if (v->n > 1u) qsort(v->p, v->n, sizeof *v->p, cmp_guard);
}

/* Structural count used only to choose the static or shared helper.  The
 * rewrite itself repeats the pending-safety proof per function; this count is
 * deliberately independent of that proof because both helper variants have
 * identical semantics. */
static size_t count_guard_sites(const char *masked, size_t n,
                                const GuardVec *guards)
{
    size_t scan = 0;
    size_t count = 0;
    while (scan + DIRECT_PREFIX_LEN <= n) {
        const char *hit = strstr(masked + scan, DIRECT_PREFIX);
        size_t pos;
        size_t end;
        uint32_t va;
        if (!hit) break;
        pos = (size_t)(hit - masked);
        if (pos + DIRECT_PREFIX_LEN > n) break;
        if (!strncmp(hit, CALL_CLEAN, CALL_CLEAN_LEN)) {
            if (clean_call_target(masked, n, pos, &va) &&
                guard_find(guards, va) != NULL && call_has_push32(masked, pos) &&
                (end = call_invocation_end(masked, n, pos, CALL_CLEAN_LEN)) !=
                    SIZE_MAX) {
                count++;
                scan = end;
                continue;
            }
            scan = pos + CALL_CLEAN_LEN;
            continue;
        }
        if (!strncmp(hit, CALL_MACRO, CALL_MACRO_LEN)) {
            if (pos + CALL_MACRO_LEN > n) break;
            if (masked[pos + CALL_MACRO_LEN] == '_') {
                scan = pos + CALL_MACRO_LEN;
                continue;
            }
            if (call_target(masked, n, pos, &va) &&
                guard_find(guards, va) != NULL && call_has_push32(masked, pos) &&
                (end = call_invocation_end(masked, n, pos, CALL_MACRO_LEN)) !=
                    SIZE_MAX) {
                count++;
                scan = end;
                continue;
            }
            scan = pos + CALL_MACRO_LEN;
            continue;
        }
        scan = pos + DIRECT_PREFIX_LEN;
    }
    return count;
}

static const char *gs_include_name(size_t sites)
{
    return sites >= GS_STATIC_MIN_SITES ? "gs_cookie_call_static.h"
                                        : "gs_cookie_call.h";
}

static int va_contains(const VaVec *v, uint32_t x)
{
    if (!v->n) return 0;
    return bsearch(&x, v->p, v->n, sizeof x, cmp_u32) != NULL;
}

static void sort_vas(VaVec *v)
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

static void collect_sets(const char *path, VaVec *leafs, VaVec *ret_leafs,
                         VaVec *depths, VaVec *known, VaVec *unsafe,
                         GuardVec *guards, size_t *funcs)
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
        uint32_t cookie, fail;
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
            vec_push(known, va);
            if (body_is_leaf(masked + open)) vec_push(leafs, va);
            if (body_is_ret_leaf(masked + open)) vec_push(ret_leafs, va);
            if (body_is_tail_free(masked + open)) vec_push(depths, va);
            if (!body_pending_safe(masked + open)) vec_push(unsafe, va);
            if (parse_gs_cookie_guard(masked + open, &cookie, &fail))
                guard_push(guards, va, cookie, fail);
            masked[close + 1u] = saved;
        }
        scan = close + 1u;
    }
    free(masked);
    free(text);
}

static void rewrite_calls(const char *text, const char *masked,
                          size_t begin, size_t end, int pending_safe,
                          int single_tail,
                          const VaVec *leafs, const VaVec *ret_leafs,
                          const VaVec *depths,
                          const VaVec *known, const GuardVec *guards, Buf *out,
                          size_t *calls, size_t *leaf_rw,
                          size_t *depth_rw, size_t *clean_rw,
                          size_t *tails, size_t *tail_leaf_rw,
                          size_t *gs_rw)
{
    size_t scan = begin;
    size_t last = begin;
    while (scan + DIRECT_PREFIX_LEN <= end) {
        const char *hit = strstr(masked + scan, DIRECT_PREFIX);
        size_t pos;
        uint32_t va;
        const char *repl = NULL;
        const Guard *guard = NULL;
        size_t call_end;
        char guard_call[96];
        if (!hit) break;
        pos = (size_t)(hit - masked);
        if (pos + DIRECT_PREFIX_LEN > end) break;

        if (!strncmp(hit, TAIL_MACRO, TAIL_MACRO_LEN)) {
            if (pos + TAIL_MACRO_LEN > end) break;
            if (hit[TAIL_MACRO_LEN] == '_') {
                /* Already _LEAF: make a rerun idempotent. */
                scan = pos + TAIL_MACRO_LEN;
                continue;
            }
            if (tail_target(masked, end, pos, &va)) {
                (*tails)++;
                if (pending_safe && single_tail && va_contains(ret_leafs, va) &&
                    tail_followed_by_return(masked, end, pos)) {
                    buf_add(out, text + last, pos - last);
                    buf_str(out, TAIL_LEAF);
                    last = pos + TAIL_MACRO_LEN;
                    scan = last;
                    (*tail_leaf_rw)++;
                    continue;
                }
            }
            scan = pos + TAIL_MACRO_LEN;
            continue;
        }

        /* The leaf_call pass has already produced the clean form on a normal
         * rerun.  Rewrite a guard call from either the raw or the clean form;
         * keep the exact body/push32 proof for both. */
        if (!strncmp(hit, CALL_CLEAN, CALL_CLEAN_LEN)) {
            if (pending_safe && clean_call_target(masked, end, pos, &va) &&
                (guard = guard_find(guards, va)) != NULL &&
                call_has_push32(masked, pos) &&
                (call_end = call_invocation_end(masked, end, pos,
                                                CALL_CLEAN_LEN)) != SIZE_MAX) {
                snprintf(guard_call, sizeof guard_call,
                         "WC3_CALL_GS_COOKIE(c, 0x%08Xu, L_%08X)",
                         (unsigned)guard->cookie, (unsigned)guard->fail);
                buf_add(out, text + last, pos - last);
                buf_str(out, guard_call);
                last = call_end;
                scan = call_end;
                (*gs_rw)++;
                continue;
            }
            scan = pos + CALL_CLEAN_LEN;
            continue;
        }

        if (!strncmp(hit, CALL_MACRO, CALL_MACRO_LEN)) {
            if (pos + CALL_MACRO_LEN > end) break;
            if (hit[CALL_MACRO_LEN] == '_') {
                scan = pos + CALL_MACRO_LEN;
                continue;
            }
            if (call_target(masked, end, pos, &va)) {
                (*calls)++;
                if (pending_safe && (guard = guard_find(guards, va)) != NULL &&
                    call_has_push32(masked, pos) &&
                    (call_end = call_invocation_end(masked, end, pos,
                                                    CALL_MACRO_LEN)) !=
                        SIZE_MAX) {
                    snprintf(guard_call, sizeof guard_call,
                             "WC3_CALL_GS_COOKIE(c, 0x%08Xu, L_%08X)",
                             (unsigned)guard->cookie, (unsigned)guard->fail);
                    buf_add(out, text + last, pos - last);
                    buf_str(out, guard_call);
                    last = call_end;
                    scan = call_end;
                    (*gs_rw)++;
                    continue;
                }
                if (va_contains(leafs, va)) {
                    repl = CALL_LEAF;
                    (*leaf_rw)++;
                } else if (va_contains(depths, va)) {
                    repl = CALL_DEPTH;
                    (*depth_rw)++;
                } else if (pending_safe && va_contains(known, va)) {
                    repl = CALL_CLEAN;
                    (*clean_rw)++;
                }
                if (repl) {
                    buf_add(out, text + last, pos - last);
                    buf_str(out, repl);
                    last = pos + CALL_MACRO_LEN;
                    scan = last;
                    continue;
                }
            }
            scan = pos + CALL_MACRO_LEN;
            continue;
        }

        /* A future WC3_DIRECT_* macro: leave it alone. */
        scan = pos + DIRECT_PREFIX_LEN;
    }
    buf_add(out, text + last, end - last);
}

static void buf_insert_after_line(Buf *out, const char *needle, const char *text,
                                  size_t text_len)
{
    const char *hit;
    size_t at;
    if (!out->p || !text_len) return;
    hit = strstr(out->p, needle);
    if (!hit) return;
    at = (size_t)(hit - out->p) + strlen(needle);
    while (at < out->n && out->p[at] != '\n') at++;
    if (at < out->n) at++;
    buf_reserve(out, text_len);
    memmove(out->p + at + text_len, out->p + at, out->n - at + 1u);
    memcpy(out->p + at, text, text_len);
    out->n += text_len;
}

/* Add the guard declaration only to a translation unit that actually received
 * a guard rewrite.  The helper itself is one external function in wc3_guest,
 * but an unused include would still add a needless source dependency. */
static void buf_insert_gs_include(Buf *out, const char *header)
{
    const char *direct;
    char line[96];
    int written;
    if (!out->p) return;
    if (strstr(out->p, "#include \"gs_cookie_call.h\"") ||
        strstr(out->p, "#include \"gs_cookie_call_static.h\""))
        return;
    direct = strstr(out->p, "#include \"direct_call.h\"");
    if (!direct) return;
    written = snprintf(line, sizeof line, "#include \"%s\"\n", header);
    if (written <= 0 || (size_t)written >= sizeof line) return;
    buf_insert_after_line(out, "#include \"direct_call.h\"", line,
                          (size_t)written);
}

/* The helper passes the fail function directly, so a caller TU needs the fail
 * symbol declared.  Insert only the guards this TU actually calls; the other
 * two guard definitions may live in a different module.  Duplicate compatible
 * declarations are legal; this also keeps a rerun idempotent. */
static void buf_insert_guard_decls(Buf *out, const GuardVec *guards,
                                   const char *header)
{
    char decls[768];
    char needle[96];
    size_t len = 0;
    size_t i;
    if (!out->p || !guards->n) return;
    if (snprintf(needle, sizeof needle, "#include \"%s\"", header) <= 0) return;
    for (i = 0; i < guards->n; i++) {
        char call[96];
        char line[64];
        int call_written;
        int written;
        call_written = snprintf(
            call, sizeof call, "WC3_CALL_GS_COOKIE(c, 0x%08Xu, L_%08X)",
            (unsigned)guards->p[i].cookie, (unsigned)guards->p[i].fail);
        if (call_written <= 0 || (size_t)call_written >= sizeof call) return;
        if (!strstr(out->p, call)) continue;
        written = snprintf(line, sizeof line, "void L_%08X(CPU *c);\n",
                           (unsigned)guards->p[i].fail);
        if (written <= 0 || (size_t)written >= sizeof line) return;
        if (strstr(out->p, line)) continue;
        if (len + (size_t)written >= sizeof decls) return;
        memcpy(decls + len, line, (size_t)written);
        len += (size_t)written;
    }
    buf_insert_after_line(out, needle, decls, len);
}

static void rewrite_file(const char *path, const char *out_dir,
                         const VaVec *leafs, const VaVec *ret_leafs,
                         const VaVec *depths,
                         const VaVec *known, const VaVec *unsafe,
                         const GuardVec *guards,
                         size_t *calls, size_t *leaf_rw,
                         size_t *depth_rw, size_t *clean_rw,
                         size_t *tails, size_t *tail_leaf_rw,
                         size_t *gs_rw)
{
    size_t n;
    char *text = read_file(path, &n);
    char *masked = (char *)xmalloc(n + 1u);
    Buf out = {0};
    size_t scan = 0;
    size_t last;
    size_t file_gs = 0;
    const char *gs_header;
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
    last = 0;
    while (scan < n) {
        size_t hdr = find_func(masked, n, scan);
        const char *nl;
        size_t open, close;
        uint32_t va;
        int allow_clean = 0;
        int single_tail = 0;
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
        if (func_va(masked, n, hdr, &va) && !va_contains(unsafe, va))
            allow_clean = 1;
        saved = masked[close + 1u];
        masked[close + 1u] = 0;
        single_tail = body_tail_count(masked + open) == 1u;
        masked[close + 1u] = saved;
        buf_add(&out, text + last, open - last);
        rewrite_calls(text, masked, open, close + 1u, allow_clean,
                      single_tail, leafs, ret_leafs, depths, known, guards, &out,
                      calls, leaf_rw, depth_rw, clean_rw, tails,
                      tail_leaf_rw, &file_gs);
        last = close + 1u;
        scan = close + 1u;
    }
    buf_add(&out, text + last, n - last);
    *gs_rw += file_gs;
    if (file_gs) {
        /* Same rule as the batch path: pay for a static helper only when this
         * file actually received enough rewritten sites. */
        gs_header = gs_include_name(file_gs);
        buf_insert_gs_include(&out, gs_header);
        buf_insert_guard_decls(&out, guards, gs_header);
    }
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

#ifndef LEAF_THREADS
#define LEAF_THREADS 8
#endif

static unsigned leaf_worker_count(size_t n)
{
    unsigned jobs = LEAF_THREADS;
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    if (jobs > si.dwNumberOfProcessors * 2u)
        jobs = si.dwNumberOfProcessors * 2u;
#endif
    if (jobs < 1u) jobs = 1u;
    if (jobs > 8u) jobs = 8u;
    if ((size_t)jobs > n) jobs = (unsigned)n;
    return jobs;
}

typedef struct {
    size_t next;
    const PathVec *paths;
    VaVec *leafs;
    VaVec *ret_leafs;
    VaVec *depths;
    VaVec *known;
    VaVec *unsafe;
    GuardVec *guards;
    size_t *funcs;
#if defined(_WIN32)
    CRITICAL_SECTION lock;
#endif
} CollectCtx;

typedef struct {
    CollectCtx *ctx;
    size_t id;
} CollectWorker;

static void collect_worker_body(CollectCtx *c, size_t id)
{
    for (;;) {
        size_t i;
#if defined(_WIN32)
        EnterCriticalSection(&c->lock);
#endif
        i = c->next;
        if (i < c->paths->n) c->next = i + 1u;
#if defined(_WIN32)
        LeaveCriticalSection(&c->lock);
#endif
        if (i >= c->paths->n) break;
        collect_sets(c->paths->p[i], &c->leafs[id], &c->ret_leafs[id],
                     &c->depths[id], &c->known[id], &c->unsafe[id],
                     &c->guards[id], &c->funcs[id]);
    }
}

#if defined(_WIN32)
static DWORD WINAPI collect_worker(LPVOID arg)
{
    CollectWorker *w = (CollectWorker *)arg;
    collect_worker_body(w->ctx, w->id);
    return 0;
}
#endif

static void collect_parallel(const PathVec *paths, unsigned jobs,
                             VaVec *leafs, VaVec *ret_leafs, VaVec *depths,
                             VaVec *known, VaVec *unsafe, GuardVec *guards,
                             size_t *funcs)
{
    CollectCtx c;
    VaVec *local_leafs;
    VaVec *local_ret_leafs;
    VaVec *local_depths;
    VaVec *local_known;
    VaVec *local_unsafe;
    GuardVec *local_guards;
    size_t *local_funcs;
    size_t i, w;
#if defined(_WIN32)
    HANDLE threads[8];
    CollectWorker workers[8];
    unsigned created = 0;
#endif

    memset(&c, 0, sizeof c);
    c.paths = paths;
    local_leafs = (VaVec *)xmalloc((size_t)jobs * sizeof(*local_leafs));
    local_ret_leafs = (VaVec *)xmalloc((size_t)jobs * sizeof(*local_ret_leafs));
    local_depths = (VaVec *)xmalloc((size_t)jobs * sizeof(*local_depths));
    local_known = (VaVec *)xmalloc((size_t)jobs * sizeof(*local_known));
    local_unsafe = (VaVec *)xmalloc((size_t)jobs * sizeof(*local_unsafe));
    local_guards = (GuardVec *)xmalloc((size_t)jobs * sizeof(*local_guards));
    local_funcs = (size_t *)xmalloc((size_t)jobs * sizeof(*local_funcs));
    memset(local_leafs, 0, (size_t)jobs * sizeof(*local_leafs));
    memset(local_ret_leafs, 0, (size_t)jobs * sizeof(*local_ret_leafs));
    memset(local_depths, 0, (size_t)jobs * sizeof(*local_depths));
    memset(local_known, 0, (size_t)jobs * sizeof(*local_known));
    memset(local_unsafe, 0, (size_t)jobs * sizeof(*local_unsafe));
    memset(local_guards, 0, (size_t)jobs * sizeof(*local_guards));
    memset(local_funcs, 0, (size_t)jobs * sizeof(*local_funcs));
    c.leafs = local_leafs;
    c.ret_leafs = local_ret_leafs;
    c.depths = local_depths;
    c.known = local_known;
    c.unsafe = local_unsafe;
    c.guards = local_guards;
    c.funcs = local_funcs;
#if defined(_WIN32)
    InitializeCriticalSection(&c.lock);
    for (i = 0; i < jobs; i++) {
        workers[i].ctx = &c;
        workers[i].id = i;
        threads[created] = CreateThread(NULL, 0, collect_worker,
                                        &workers[i], 0, NULL);
        if (threads[created]) created++;
        else break;
    }
    if (created < jobs)
        collect_worker_body(&c, created);
    if (created)
        WaitForMultipleObjects(created, threads, TRUE, INFINITE);
    for (i = 0; i < created; i++) CloseHandle(threads[i]);
    DeleteCriticalSection(&c.lock);
#else
    collect_worker_body(&c, 0);
#endif

    for (w = 0; w < jobs; w++) {
        for (i = 0; i < local_leafs[w].n; i++)
            vec_push(leafs, local_leafs[w].p[i]);
        for (i = 0; i < local_ret_leafs[w].n; i++)
            vec_push(ret_leafs, local_ret_leafs[w].p[i]);
        for (i = 0; i < local_depths[w].n; i++)
            vec_push(depths, local_depths[w].p[i]);
        for (i = 0; i < local_known[w].n; i++)
            vec_push(known, local_known[w].p[i]);
        for (i = 0; i < local_unsafe[w].n; i++)
            vec_push(unsafe, local_unsafe[w].p[i]);
        for (i = 0; i < local_guards[w].n; i++)
            guard_push(guards, local_guards[w].p[i].target,
                       local_guards[w].p[i].cookie,
                       local_guards[w].p[i].fail);
        *funcs += local_funcs[w];
        free(local_leafs[w].p);
        free(local_ret_leafs[w].p);
        free(local_depths[w].p);
        free(local_known[w].p);
        free(local_unsafe[w].p);
        free(local_guards[w].p);
    }
    free(local_leafs);
    free(local_ret_leafs);
    free(local_depths);
    free(local_known);
    free(local_unsafe);
    free(local_guards);
    free(local_funcs);
}

typedef struct {
    size_t calls;
    size_t leaf_rw;
    size_t depth_rw;
    size_t clean_rw;
    size_t gs_rw;
    size_t tails;
    size_t tail_leaf_rw;
} LeafStats;

typedef struct {
    size_t next;
    const PathVec *paths;
    const char *out_dir;
    const VaVec *leafs;
    const VaVec *ret_leafs;
    const VaVec *depths;
    const VaVec *known;
    const VaVec *unsafe;
    const GuardVec *guards;
    LeafStats *stats;
#if defined(_WIN32)
    CRITICAL_SECTION lock;
#endif
} RewriteCtx;

typedef struct {
    RewriteCtx *ctx;
    size_t id;
} RewriteWorker;

static void rewrite_worker_body(RewriteCtx *c, size_t id)
{
    LeafStats *s = &c->stats[id];
    for (;;) {
        size_t i;
#if defined(_WIN32)
        EnterCriticalSection(&c->lock);
#endif
        i = c->next;
        if (i < c->paths->n) c->next = i + 1u;
#if defined(_WIN32)
        LeaveCriticalSection(&c->lock);
#endif
        if (i >= c->paths->n) break;
        rewrite_file(c->paths->p[i], c->out_dir, c->leafs, c->ret_leafs,
                     c->depths, c->known, c->unsafe, c->guards, &s->calls,
                     &s->leaf_rw, &s->depth_rw, &s->clean_rw, &s->tails,
                     &s->tail_leaf_rw, &s->gs_rw);
    }
}

#if defined(_WIN32)
static DWORD WINAPI rewrite_worker(LPVOID arg)
{
    RewriteWorker *w = (RewriteWorker *)arg;
    rewrite_worker_body(w->ctx, w->id);
    return 0;
}
#endif

static void rewrite_parallel(const PathVec *paths, unsigned jobs,
                             const char *out_dir, const VaVec *leafs,
                             const VaVec *ret_leafs, const VaVec *depths,
                             const VaVec *known, const VaVec *unsafe,
                             const GuardVec *guards,
                             size_t *calls, size_t *leaf_rw,
                             size_t *depth_rw, size_t *clean_rw, size_t *tails,
                             size_t *tail_leaf_rw, size_t *gs_rw)
{
    RewriteCtx c;
    LeafStats *stats;
    size_t i;
#if defined(_WIN32)
    HANDLE threads[8];
    RewriteWorker workers[8];
    unsigned created = 0;
#endif

    memset(&c, 0, sizeof c);
    c.paths = paths;
    c.out_dir = out_dir;
    c.leafs = leafs;
    c.ret_leafs = ret_leafs;
    c.depths = depths;
    c.known = known;
    c.unsafe = unsafe;
    c.guards = guards;
    stats = (LeafStats *)xmalloc((size_t)jobs * sizeof(*stats));
    memset(stats, 0, (size_t)jobs * sizeof(*stats));
    c.stats = stats;
#if defined(_WIN32)
    InitializeCriticalSection(&c.lock);
    for (i = 0; i < jobs; i++) {
        workers[i].ctx = &c;
        workers[i].id = i;
        threads[created] = CreateThread(NULL, 0, rewrite_worker,
                                        &workers[i], 0, NULL);
        if (threads[created]) created++;
        else break;
    }
    if (created < jobs)
        rewrite_worker_body(&c, created);
    if (created)
        WaitForMultipleObjects(created, threads, TRUE, INFINITE);
    for (i = 0; i < created; i++) CloseHandle(threads[i]);
    DeleteCriticalSection(&c.lock);
#else
    rewrite_worker_body(&c, 0);
#endif

    for (i = 0; i < jobs; i++) {
        *calls += stats[i].calls;
        *leaf_rw += stats[i].leaf_rw;
        *depth_rw += stats[i].depth_rw;
        *clean_rw += stats[i].clean_rw;
        *gs_rw += stats[i].gs_rw;
        *tails += stats[i].tails;
        *tail_leaf_rw += stats[i].tail_leaf_rw;
    }
    free(stats);
}

static int self_test(void)
{
    static const char *src =
        "#include \"cpu.h\"\n"
        "#include \"direct_call.h\"\n"
        "/* void L_00000007(CPU *restrict c)\n"
        "{\n"
        "    WC3_DIRECT_CALL(L_00000001, c);\n"
        "}\n"
        "*/\n"
        "void L_00000001(CPU *restrict c)\n"
        "{\n"
        "    c->esp += 4; return; /* WC3_DIRECT_CALL(L_0000000D, c); */\n"
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
        "    WC3_DIRECT_CALL(L_0000000D, c);\n"
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
        "}\n"
        "void L_0000000D(CPU *restrict c)\n"
        "{\n"
        "    WC3_DIRECT_TAIL(L_00000001, c);\n"
        "    return;\n"
        "}\n"
        "void L_0000000E(CPU *restrict c)\n"
        "{\n"
        "    WC3_DIRECT_CALL(L_0000000D, c);\n"
        "}\n"
        "void L_0000000F(CPU *restrict c)\n"
        "{\n"
        "    WC3_DIRECT_CALL(L_00000001, c);\n"
        "}\n"
        "void L_00000010(CPU *restrict c)\n"
        "{\n"
        "    WC3_DIRECT_CALL(L_0000000F, c);\n"
        "}\n"
        "void L_00000011(CPU *restrict c)\n"
        "{\n"
        "    WC3_DIRECT_CALL(L_00000099, c);\n"
        "}\n"
        "void L_00000012(CPU *restrict c)\n"
        "{\n"
        "    WC3_DIRECT_TAIL(L_00000003, c);\n"
        "    return;\n"
        "}\n"
        "void L_00000013(CPU *restrict c)\n"
        "{\n"
        "    WC3_DIRECT_TAIL(L_00000001, c); return;\n"
        "    WC3_DIRECT_TAIL(L_00000001, c); return;\n"
        "}\n"
        "void L_00000014(CPU *restrict c)\n"
        "{\n"
        "    flags_sub(c, c->ecx, rd32((GVA(0x12345678))), 4);\n"
        "    if (!c->zf) { WC3_DIRECT_TAIL(L_00000001, c); return; }\n"
        "    c->esp += 4; return;\n"
        "}\n"
        "void L_00000015(CPU *restrict c)\n"
        "{\n"
        "    push32(c, 0u); xmm_store_full(&c->xmm[0], &_wc3_xmm_local[0]);\n"
        "    WC3_DIRECT_CALL(L_00000014, c);\n"
        "}\n";
    size_t n = strlen(src);
    char *text = (char *)xmalloc(n + 1u);
    char *masked = (char *)xmalloc(n + 1u);
    VaVec leafs = {0}, ret_leafs = {0}, depths = {0}, known = {0}, unsafe = {0};
    GuardVec guards = {0};
    size_t scan = 0, funcs = 0;
    Buf out = {0};
    size_t last = 0;
    size_t calls = 0, leaf_rw = 0, depth_rw = 0, clean_rw = 0;
    size_t gs_rw = 0;
    size_t gs_sites;
    const char *gs_header;
    size_t tails = 0, tail_leaf_rw = 0;
    memcpy(text, src, n + 1u);
    mask_code(text, n, masked);
    while (scan < n) {
        size_t hdr = find_func(masked, n, scan);
        const char *nl;
        size_t open, close;
        uint32_t va;
        uint32_t cookie, fail;
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
            vec_push(&known, va);
            if (body_is_leaf(masked + open)) vec_push(&leafs, va);
            if (body_is_ret_leaf(masked + open)) vec_push(&ret_leafs, va);
            if (body_is_tail_free(masked + open)) vec_push(&depths, va);
            if (!body_pending_safe(masked + open)) vec_push(&unsafe, va);
            if (parse_gs_cookie_guard(masked + open, &cookie, &fail))
                guard_push(&guards, va, cookie, fail);
            masked[close + 1u] = saved;
        }
        scan = close + 1u;
    }
    sort_vas(&leafs);
    sort_vas(&ret_leafs);
    sort_vas(&depths);
    sort_vas(&known);
    sort_vas(&unsafe);
    sort_guards(&guards);
    gs_sites = count_guard_sites(masked, n, &guards);
    scan = 0;
    last = 0;
    while (scan < n) {
        size_t hdr = find_func(masked, n, scan);
        const char *nl;
        size_t open, close;
        uint32_t va;
        int allow_clean = 0;
        int single_tail = 0;
        char saved;
        if (hdr == SIZE_MAX) break;
        nl = (const char *)memchr(masked + hdr, '\n', n - hdr);
        if (!nl) break;
        open = (size_t)(nl - masked) + 1u;
        if (open >= n || masked[open] != '{') break;
        close = matching_brace(text, n, open);
        if (close == SIZE_MAX) break;
        if (func_va(masked, n, hdr, &va) && !va_contains(&unsafe, va))
            allow_clean = 1;
        saved = masked[close + 1u];
        masked[close + 1u] = 0;
        single_tail = body_tail_count(masked + open) == 1u;
        masked[close + 1u] = saved;
        buf_add(&out, text + last, open - last);
        rewrite_calls(text, masked, open, close + 1u, allow_clean,
                      single_tail, &leafs, &ret_leafs, &depths, &known,
                      &guards, &out, &calls, &leaf_rw, &depth_rw, &clean_rw,
                      &tails, &tail_leaf_rw, &gs_rw);
        last = close + 1u;
        scan = close + 1u;
    }
    buf_add(&out, text + last, n - last);
    /* Choose the helper from the sites that actually passed pending_safe and
     * were rewritten.  The candidate count is useful for the report, but an
     * unsafe caller must not amortize a static helper it never uses. */
    gs_header = gs_include_name(gs_rw);
    if (gs_rw) {
        buf_insert_gs_include(&out, gs_header);
        buf_insert_guard_decls(&out, &guards, gs_header);
    }
    if (funcs != 21u || leafs.n != 1u || leafs.p[0] != 1u ||
        ret_leafs.n != 1u || ret_leafs.p[0] != 1u ||
        depths.n != 15u || known.n != 21u || unsafe.n != 2u ||
        guards.n != 1u || guards.p[0].target != 0x14u ||
        !va_contains(&unsafe, 7u) || !va_contains(&unsafe, 8u) ||
        calls != 12u || leaf_rw != 2u || depth_rw != 5u ||
        clean_rw != 2u || gs_rw != 1u || gs_sites != 1u || tails != 6u ||
        tail_leaf_rw != 2u ||
        strcmp(gs_include_name(1u), "gs_cookie_call.h") != 0 ||
        strcmp(gs_include_name(20u), "gs_cookie_call_static.h") != 0 ||
        !strstr(out.p, "#include \"gs_cookie_call.h\"") ||
        !strstr(out.p, "void L_00000001(CPU *c);") ||
        !strstr(out.p, "WC3_CALL_GS_COOKIE(c, 0x12345678u, L_00000001);") ||
        !strstr(out.p, "WC3_DIRECT_CALL_LEAF(L_00000001, c);") ||
        !strstr(out.p, "WC3_DIRECT_CALL_DEPTH(L_00000003, c);") ||
        !strstr(out.p, "WC3_DIRECT_CALL_DEPTH(L_00000009, c);") ||
        !strstr(out.p, "WC3_DIRECT_CALL_DEPTH(L_0000000A, c);") ||
        !strstr(out.p, "WC3_DIRECT_CALL_DEPTH(L_0000000B, c);") ||
        !strstr(out.p, "WC3_DIRECT_CALL_DEPTH(L_0000000F, c);") ||
        !strstr(out.p, "WC3_DIRECT_CALL_CLEAN(L_00000008, c);") ||
        !strstr(out.p, "WC3_DIRECT_CALL_CLEAN(L_0000000D, c);") ||
        !strstr(out.p, "c->disp_pending_valid = 1u;\n"
                       "    WC3_DIRECT_CALL(L_0000000D, c);") ||
        !strstr(out.p, "WC3_DIRECT_CALL(L_00000099, c);") ||
        !strstr(out.p, "/* WC3_DIRECT_CALL(L_0000000D, c); */") ||
        !strstr(out.p, "WC3_DIRECT_TAIL_LEAF(L_00000001, c);") ||
        !strstr(out.p, "WC3_DIRECT_TAIL(L_00000001, c);") ||
        !strstr(out.p, "WC3_DIRECT_TAIL(L_00000003, c);") ||
        strstr(out.p, "WC3_DIRECT_CALL_DEPTH(L_0000000D") ||
        strstr(out.p, "WC3_DIRECT_CALL_CLEAN(L_00000099") ||
        strstr(out.p, "WC3_DIRECT_CALL_DEPTH(L_00000007") ||
        strstr(out.p, "WC3_DIRECT_CALL_LEAF(L_00000003") ||
        strstr(out.p, "WC3_DIRECT_CALL_LEAF(L_00000008") ||
        strstr(out.p, "WC3_DIRECT_CALL_LEAF(L_00000009") ||
        strstr(out.p, "WC3_DIRECT_CALL_LEAF(L_0000000A") ||
        strstr(out.p, "WC3_DIRECT_CALL_LEAF(L_0000000B") ||
        strstr(out.p, "WC3_DIRECT_TAIL_LEAF(L_00000003")) {
        fprintf(stderr, "leaf_call self-test failed: funcs=%zu leafs=%zu "
                        "ret_leafs=%zu depths=%zu known=%zu unsafe=%zu guards=%zu "
                        "calls=%zu tails=%zu leaf_rw=%zu depth_rw=%zu clean_rw=%zu "
                        "gs_rw=%zu tail_leaf_rw=%zu\n",
                funcs, leafs.n, ret_leafs.n, depths.n, known.n, unsafe.n,
                guards.n, calls, tails, leaf_rw, depth_rw, clean_rw, gs_rw,
                tail_leaf_rw);
        return 1;
    }
    puts("leaf_call self-test: OK");
    free(out.p);
    free(masked);
    free(text);
    free(leafs.p);
    free(ret_leafs.p);
    free(depths.p);
    free(known.p);
    free(unsafe.p);
    free(guards.p);
    return 0;
}

int main(int argc, char **argv)
{
    const char *inputs = NULL;
    const char *out_dir = NULL;
    const char *report = NULL;
    int quiet = 0;
    int gs_cookie = 0;
    PathVec paths = {0};
    VaVec leafs = {0}, ret_leafs = {0}, depths = {0}, known = {0}, unsafe = {0};
    GuardVec guards = {0};
    size_t funcs = 0, rewrites = 0;
    size_t calls = 0, leaf_rw = 0, depth_rw = 0, clean_rw = 0;
    size_t gs_rw = 0;
    size_t tails = 0, tail_leaf_rw = 0;
    size_t calls_rw = 0;
    unsigned jobs;
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
        } else if (!strcmp(argv[i], "--gs-cookie")) {
            gs_cookie = 1;
        } else if (!strncmp(argv[i], "--gs-cookie=", 12)) {
            const char *value = argv[i] + 12;
            if (!strcmp(value, "0")) {
                gs_cookie = 0;
            } else if (!strcmp(value, "1")) {
                gs_cookie = 1;
            } else {
                die("invalid --gs-cookie value: %s", value);
            }
        } else {
            die("unknown argument: %s", argv[i]);
        }
    }
    if (!inputs || !out_dir) die("--inputs and --out-dir are required");

    read_paths(inputs, &paths);
    jobs = leaf_worker_count(paths.n);
    collect_parallel(&paths, jobs, &leafs, &ret_leafs, &depths, &known,
                     &unsafe, &guards, &funcs);
    if (!funcs) die("no canonical functions parsed from %zu inputs", paths.n);
    sort_vas(&leafs);
    sort_vas(&ret_leafs);
    sort_vas(&depths);
    sort_vas(&known);
    sort_vas(&unsafe);
    sort_guards(&guards);
    rewrite_parallel(&paths, jobs, out_dir, &leafs, &ret_leafs, &depths,
                     &known, &unsafe, gs_cookie ? &guards : NULL, &calls,
                     &leaf_rw, &depth_rw, &clean_rw, &tails, &tail_leaf_rw,
                     &gs_rw);
    rewrites = leaf_rw + depth_rw + clean_rw + tail_leaf_rw + gs_rw;
    calls_rw = leaf_rw + depth_rw + clean_rw + gs_rw;

    if (report) {
        FILE *f = fopen(report, "wb");
        if (!f) die("cannot create report %s", report);
        fprintf(f,
                "{\n"
                "  \"schema\": 1,\n"
                "  \"inputs\": %zu,\n"
                "  \"functions\": %zu,\n"
                "  \"leaf_functions\": %zu,\n"
                "  \"ret_leaf_functions\": %zu,\n"
                "  \"tail_free_functions\": %zu,\n"
                "  \"known_functions\": %zu,\n"
                "  \"unsafe_callers\": %zu,\n"
                "  \"gs_cookie_guards\": %zu,\n"
                "  \"gs_cookie_enabled\": %d,\n"
                "  \"calls_seen\": %zu,\n"
                "  \"calls_rewritten\": %zu,\n"
                "  \"calls_leaf_rewritten\": %zu,\n"
                "  \"calls_depth_rewritten\": %zu,\n"
                "  \"calls_clean_rewritten\": %zu,\n"
                "  \"calls_gs_cookie_rewritten\": %zu,\n"
                "  \"tails_seen\": %zu,\n"
                "  \"tails_leaf_rewritten\": %zu,\n"
                "  \"rewrites_total\": %zu,\n"
                "  \"verdict\": \"pass\"\n"
                "}\n",
                paths.n, funcs, leafs.n, ret_leafs.n, depths.n, known.n,
                unsafe.n, guards.n, gs_cookie ? 1 : 0, calls, calls_rw,
                leaf_rw, depth_rw, clean_rw, gs_rw, tails, tail_leaf_rw,
                rewrites);
        if (fclose(f) != 0) die("cannot close report %s", report);
    }
    if (!quiet)
        printf("leaf_call: %zu inputs, %zu functions, %zu leafs, "
               "%zu tail-free, %zu guards, %zu calls, %zu tails, %zu rewritten "
               "(leaf %zu, depth %zu, clean %zu, gs-cookie %zu, tail-leaf %zu)\n",
               paths.n, funcs, leafs.n, depths.n, guards.n, calls, tails,
               rewrites, leaf_rw, depth_rw, clean_rw, gs_rw, tail_leaf_rw);

    for (i = 0; i < paths.n; i++) free(paths.p[i]);
    free(paths.p);
    free(leafs.p);
    free(ret_leafs.p);
    free(depths.p);
    free(known.p);
    free(unsafe.p);
    free(guards.p);
    return 0;
}
