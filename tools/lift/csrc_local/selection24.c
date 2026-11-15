/* selection24.c - 24-unit selection / adaptive 6x2 <-> 8x3 panel LiftPass.
 *
 * The pass edits the generated corpus through address-comment anchors.  It is
 * idempotent, fail-closed on a template mismatch, and publishes a per-kind
 * counts object plus a line-level change set.  Per-chunk runs use a subset of
 * the sites; the configure step sums every chunk JSON through
 * --verify-coverage and compares the union with the fixed totals below.
 *
 *   selection24 --lift IN --out OUT [--json REPORT] [--profile seed|closed]
 *               [--quiet]
 *   selection24 --verify-coverage @manifest
 *   selection24 --self-test
 *
 * Product integration is described in docs/research/SELECTION_24_ANALYSIS.md.
 * No source file under build/corpus-full is modified in place.
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"

#define PASS "selection24"
#define PASS_REV 4

typedef enum {
    K_GROUP_ALLOC = 0,
    K_INIT_GROUP,
    K_SELECT,
    K_CAP_OUTER,
    K_CAP_INNER,
    K_FILL_BLOCK,
    K_FILL_GRID,
    K_NOTE_SELECTION,
    K_HIDE_ALL,
    K_ADDUNIT_CAP,
    K_CAP_LOCAL_LIST,
    K_CAP_COPY_LIST,
    K_CAP_SYNC_ADD,
    K_CAP_NET_RECV,
    K_CAP_NET_APPLY,
    K_CAP_NET_SEND,
    K_CAP_SELECT_ALL,
    K_ORDER_FRAME,
    K_ORDER_SORT,
    K_ORDER_LOOP,
    K_COUNT
} Kind;

static const char *const kind_name[K_COUNT] = {
    "group_alloc",
    "init_group",
    "select",
    "cap_outer",
    "cap_inner",
    "fill_block",
    "fill_grid",
    "note_selection",
    "hide_all",
    "addunit_cap",
    "cap_local_list",
    "cap_copy_list",
    "cap_sync_add",
    "cap_net_recv",
    "cap_net_apply",
    "cap_net_send",
    "cap_select_all",
    "order_frame",
    "order_sort",
    "order_loop"
};

/* Fixed union totals for the whole relifted game corpus. */
static const unsigned expected_total[K_COUNT] = {
    1, /* group_alloc   */
    2, /* init_group    */
    2, /* select        */
    2, /* cap_outer     */
    2, /* cap_inner     */
    2, /* fill_block    */
    1, /* fill_grid     */
    1, /* note_selection*/
    1, /* hide_all      */
    1, /* addunit_cap   */
    5, /* cap_local_list*/
    3, /* cap_copy_list */
    1, /* cap_sync_add  */
    1, /* cap_net_recv  */
    5, /* cap_net_apply */
    1, /* cap_net_send  */
    1, /* cap_select_all*/
    1, /* order_frame   */
    4, /* order_sort    */
    4  /* order_loop    */
};

/* Whole-file expectations.  seed.c carries the update prologue only. */
static const unsigned expected_seed[K_COUNT] = {
    0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0
};

/* closed.c carries the split fragments and the UI allocation/fill code. */
static const unsigned expected_closed[K_COUNT] = {
    1, 2, 1, 1, 1, 2, 1, 0, 0, 1, 5, 3, 1, 0, 5, 1, 1, 1, 4, 4
};

typedef struct {
    unsigned n[K_COUNT];
} Counts;

typedef struct {
    Kind kind;
    uint32_t va;
    const char *action;
    int line;
    char before[65];
    char after[65];
} Site;

typedef struct {
    Site *v;
    size_t n;
    size_t cap;
} Sites;

typedef struct {
    char *p;
    size_t n;
    size_t cap;
} Buf;

static int counts_match(const Counts *c, const unsigned *exp)
{
    int i;

    for (i = 0; i < K_COUNT; ++i)
        if (c->n[i] != exp[i])
            return 0;
    return 1;
}

static unsigned counts_sum(const Counts *c)
{
    unsigned total = 0;
    int i;

    for (i = 0; i < K_COUNT; ++i)
        total += c->n[i];
    return total;
}

/* ------------------------------------------------------------------ */
/* Small buffers and line helpers                                      */
/* ------------------------------------------------------------------ */

static int buf_append(Buf *b, const char *p, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        char *np;
        while (cap < b->n + n + 1)
            cap *= 2;
        np = (char *)realloc(b->p, cap);
        if (!np)
            return -1;
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = '\0';
    return 0;
}

static int buf_puts(Buf *b, const char *s)
{
    return buf_append(b, s, strlen(s));
}

static void buf_free(Buf *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

static const char *line_end(const char *p, const char *end)
{
    const char *q = (const char *)memchr(p, '\n', (size_t)(end - p));

    return q ? q + 1 : end;
}

static size_t term_len(const char *p, const char *le)
{
    if (le > p && le[-1] == '\n') {
        if (le - p >= 2 && le[-2] == '\r')
            return 2;
        return 1;
    }
    return 0;
}

static const char *find_sub(const char *p, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    size_t i;

    if (m == 0)
        return p;
    if (n < m)
        return NULL;
    for (i = 0; i + m <= n; ++i)
        if (memcmp(p + i, needle, m) == 0)
            return p + i;
    return NULL;
}

static int contains_sub(const char *p, size_t n, const char *needle)
{
    return find_sub(p, n, needle) != NULL;
}

static int is_space_ch(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/*
 * Compare the executable part of one generated line with a template.  The
 * trailing address comment and indentation are not part of the template, so
 * later passes that reflow the comment do not invalidate the anchor.
 */
static int code_equals(const char *p, const char *e, const char *want)
{
    const char *code = p;
    const char *ce = e;
    const char *comment;
    size_t wlen = strlen(want);
    size_t clen;

    while (code < ce && (*code == ' ' || *code == '\t'))
        ++code;
    comment = (const char *)find_sub(code, (size_t)(ce - code), "/*");
    if (comment)
        ce = comment;
    while (ce > code && is_space_ch((unsigned char)ce[-1]))
        --ce;
    clen = (size_t)(ce - code);
    return clen == wlen && memcmp(code, want, wlen) == 0;
}

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

/* Parse the last 8-hex-digit VA in the trailing comment. */
static int parse_va(const char *p, const char *e, uint32_t *out)
{
    const char *comment = NULL;
    const char *q;
    const char *digits;
    uint32_t v = 0;
    int i;

    for (q = p; q + 2 <= e; ++q)
        if (q[0] == '/' && q[1] == '*')
            comment = q;
    if (!comment)
        return 0;
    digits = comment + 2;
    while (digits < e && (*digits == ' ' || *digits == '\t'))
        ++digits;
    if ((size_t)(e - digits) < 9 || digits[8] != ':')
        return 0;
    for (i = 0; i < 8; ++i) {
        int h = hex_digit((unsigned char)digits[i]);
        if (h < 0)
            return 0;
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return 1;
}

static void hash_hex(const char *p, size_t n, char out[65])
{
    unsigned char digest[32];
    sha256_ctx s;

    sha256_init(&s);
    sha256_update(&s, p, n);
    sha256_final(&s, digest);
    sha256_hex(digest, out);
}

/*
 * Replace exactly one occurrence of old_s with new_s.  Returns 1 on success,
 * 0 when the substring is absent and -1 when it occurs more than once.
 */
static int replace_once(char *hay, size_t n, const char *old_s,
                        const char *new_s, char **outp, size_t *outn)
{
    const char *hit = find_sub(hay, n, old_s);
    size_t olen = strlen(old_s);
    size_t nlen = strlen(new_s);
    char *res;

    if (!hit)
        return 0;
    if (find_sub(hit + olen, n - (size_t)(hit + olen - hay), old_s))
        return -1;
    res = (char *)malloc(n + nlen + 1);
    if (!res)
        return -1;
    memcpy(res, hay, (size_t)(hit - hay));
    memcpy(res + (size_t)(hit - hay), new_s, nlen);
    memcpy(res + (size_t)(hit - hay) + nlen, hit + olen,
           n - (size_t)(hit + olen - hay));
    *outp = res;
    *outn = n - olen + nlen;
    return 1;
}

static void sites_add(Sites *s, Kind k, uint32_t va, const char *action,
                      int line, const char *before, const char *after)
{
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 16;
        Site *nv = (Site *)realloc(s->v, cap * sizeof(*nv));
        if (!nv)
            return;
        s->v = nv;
        s->cap = cap;
    }
    s->v[s->n].kind = k;
    s->v[s->n].va = va;
    s->v[s->n].action = action;
    s->v[s->n].line = line;
    snprintf(s->v[s->n].before, sizeof(s->v[s->n].before), "%s", before);
    snprintf(s->v[s->n].after, sizeof(s->v[s->n].after), "%s", after);
    ++s->n;
}

/* ------------------------------------------------------------------ */
/* Site templates                                                      */
/* ------------------------------------------------------------------ */

#define VA_GROUP_ALLOC      0x6F39E7DEu
#define VA_INIT_GROUP       0x6F39EDA4u
#define VA_SELECT           0x6F3C7DBBu
#define VA_CAP_OUTER        0x6F3C7D71u
#define VA_CAP_INNER        0x6F3C7DD0u
#define VA_FILL_BLOCK       0x6F3C7E78u
#define VA_FILL_GRID        0x6F3DA47Fu
#define VA_NOTE_SELECTION   0x6F3C7D35u
#define VA_HIDE_ALL         0x6F3ACCA3u
#define VA_ADDUNIT_CAP      0x6F28DC30u
#define VA_CAP_LOCAL_LIST   0x6F28EB97u
#define VA_CAP_COPY_LIST    0x6F391AD2u
#define VA_CAP_SYNC_ADD     0x6F28D667u
#define VA_CAP_NET_RECV     0x6F6BC82Au
#define VA_CAP_NET_APPLY    0x6F6BCAC9u
#define VA_CAP_NET_SEND     0x6F6BCF1Du
#define VA_CAP_SELECT_ALL   0x6F390293u

/* Command batch (CSelectionWar3::IssueOrder path).  The original frame has a
 * 12-entry 0x24-byte local array ending exactly at the security cookie.  The
 * 24-unit path sorts that array with the live selection count, so the array is
 * moved below the frame and the frame is extended by one extra 12-entry slot. */
#define VA_ORDER_FRAME      0x6F6B7F01u
#define VA_ORDER_SORT1      0x6F6B9E05u
#define VA_ORDER_SORT2      0x6F6BA444u
#define VA_ORDER_LOOP1      0x6F6B9E34u
#define VA_ORDER_LOOP2      0x6F6BA473u
#define ORDER_OLD_FRAME     0x5D4u
#define ORDER_NEW_FRAME     0x940u
#define ORDER_OLD_BASE      0xFFFFFE40u /* -0x1c0 */
#define ORDER_NEW_BASE      0xFFFFF6C0u /* -0x940 */
#define ORDER_NEW_LOOP1     0xFFFFF6D8u /* -0x928 */
#define ORDER_NEW_LOOP2     0xFFFFF6DCu /* -0x924 */

static const char *const INIT_GROUP_DISPATCH =
    "push32(c, 0x6F39EDA9u); dispatch(c, 0x6F3B91B0u);";
static const char *const INIT_GROUP_DIRECT =
    "push32(c, 0x6F39EDA9u); WC3_DIRECT_CALL(L_6F3B91B0, c);";

static const char *const INSERT_INIT_LINES[] = {
    "    wc3_sel24_init_group(c, c->edi); "
    "/* 6F39EDA4: selection24 grid24 init */"
};

static const char *const INSERT_SELECT_LINES[] = {
    "    wc3_sel24_select(c, rd32((c->ebp + 0xFFFFFFE0u)), "
    "rd32((c->ebp + 0xFFFFFFDCu))); "
    "/* 6F3C7DBB: selection24 mode */"
};

static const char *const INSERT_NOTE_LINES[] = {
    "    wc3_sel24_note_selection(c, rd32((c->ebp + 0xFFFFFFE0u)), c->esi); "
    "/* 6F3C7D35: selection24 total */"
};

static const char *const INSERT_HIDE[] = {
    /* The insertion is emitted after `mov ebx, ecx`, so ebx is the group
     * `this` pointer.  Keep that argument explicit: the helper signature is
     * wc3_sel24_hide_buttons(c, group), not a raw caller register. */
    "    wc3_sel24_hide_buttons(c, c->ebx); "
    "/* 6F3ACCA3: selection24 active grid */",
    "    c->edi = (uint32_t)(pop32(c));                               "
    "/* 6F3ACCF2: pop edi */",
    "    c->esi = (uint32_t)(pop32(c));                               "
    "/* 6F3ACCF3: pop esi */",
    "    c->ebx = (uint32_t)(pop32(c));                               "
    "/* 6F3ACCF4: pop ebx */",
    "    c->esp += 4; return;                                         "
    "/* 6F3ACCF5: ret  */"
};

static const char INSERT_FILL[] =
    "    wc3_sel24_fill(c, rd32((c->ebp + 0xFFFFFFE0u)), c->ebx, c->esi, "
    "rd32((c->ebp + 0xFFFFFFE4u))); /* 6F3C7E78: selection24 fill */";

static const char *const FILL_CALL_DIRECT =
    "push32(c, 0x6F3C7E99u); WC3_DIRECT_CALL(L_6F3DA470, c);";

static const char *const FILL_CODE[13] = {
    "push32(c, rd32((c->ebp + 0xFFFFFFE4u)));",
    "c->eax = 0xAAAAAAABu;",
    "{ uint64_t _p=(uint64_t)c->eax*(uint32_t)(c->ebx); "
    "c->eax=(uint32_t)_p; c->edx=(uint32_t)(_p>>32); }",
    "push32(c, c->esi);",
    "c->edx = op_shr_nf(c->edx, 0x2u, 4);",
    "c->eax = c->ebx;",
    "push32(c, c->edx);",
    "c->ecx = (uint32_t)((c->edx + c->edx*2));",
    "c->ecx = add32(c->ecx, c->ecx, 4);",
    "c->eax = flags_sub(c, c->eax, c->ecx, 4);",
    "c->ecx = rd32((c->ebp + 0xFFFFFFE0u));",
    "push32(c, c->eax);",
    "push32(c, 0x6F3C7E99u); dispatch(c, 0x6F3DA470u);"
};

static const uint32_t FILL_VA[13] = {
    0x6F3C7E78u, 0x6F3C7E7Bu, 0x6F3C7E80u, 0x6F3C7E82u,
    0x6F3C7E83u, 0x6F3C7E86u, 0x6F3C7E88u, 0x6F3C7E89u,
    0x6F3C7E8Cu, 0x6F3C7E8Eu, 0x6F3C7E90u, 0x6F3C7E93u,
    0x6F3C7E94u
};

static int is_fill_va(uint32_t va)
{
    int i;

    for (i = 0; i < 13; ++i)
        if (FILL_VA[i] == va)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Site handlers                                                       */
/* ------------------------------------------------------------------ */

static int handle_subst(Buf *out, const char *p, const char *e,
                        const char *old_code, const char *new_code,
                        const char *old_comment, const char *new_comment,
                        Kind k, uint32_t va, int line, Counts *counts,
                        Sites *sites, int *handled, char *err, size_t errsz)
{
    int has_old = contains_sub(p, (size_t)(e - p), old_code);
    int has_new = contains_sub(p, (size_t)(e - p), new_code);
    char before[65];
    char after[65];
    Buf tmp = {0};
    const char *action;

    hash_hex(p, (size_t)(e - p), before);

    if (has_old && has_new) {
        snprintf(err, errsz,
                 "site 0x%08X line %d contains both '%s' and '%s'",
                 va, line, old_code, new_code);
        return -1;
    }
    if (!has_old && !has_new) {
        snprintf(err, errsz,
                 "site 0x%08X line %d does not match '%s'", va, line, old_code);
        return -1;
    }

    if (buf_append(&tmp, p, (size_t)(e - p)) != 0) {
        buf_free(&tmp);
        return -1;
    }

    if (has_old) {
        char *cur = NULL;
        size_t curlen = 0;
        char *next = NULL;
        size_t nextlen = 0;
        int r;

        r = replace_once((char *)p, (size_t)(e - p), old_code, new_code,
                         &cur, &curlen);
        if (r != 1) {
            snprintf(err, errsz,
                     "site 0x%08X line %d: ambiguous substitution", va, line);
            free(cur);
            buf_free(&tmp);
            return -1;
        }
        free(tmp.p);
        tmp.p = cur;
        tmp.n = curlen;
        tmp.cap = curlen + 1;

        if (old_comment && new_comment &&
            contains_sub(tmp.p, tmp.n, old_comment)) {
            r = replace_once(tmp.p, tmp.n, old_comment, new_comment,
                             &next, &nextlen);
            if (r != 1) {
                snprintf(err, errsz,
                         "site 0x%08X line %d: ambiguous comment edit",
                         va, line);
                free(next);
                buf_free(&tmp);
                return -1;
            }
            free(tmp.p);
            tmp.p = next;
            tmp.n = nextlen;
            tmp.cap = nextlen + 1;
        }
        action = "replace";
    } else {
        /* Already patched.  A stale comment is repaired, otherwise the line
         * is copied byte-for-byte. */
        action = "already";
        if (old_comment && new_comment &&
            contains_sub(tmp.p, tmp.n, old_comment)) {
            char *next = NULL;
            size_t nextlen = 0;
            int r = replace_once(tmp.p, tmp.n, old_comment, new_comment,
                                 &next, &nextlen);
            if (r != 1) {
                snprintf(err, errsz,
                         "site 0x%08X line %d: ambiguous comment edit",
                         va, line);
                free(next);
                buf_free(&tmp);
                return -1;
            }
            free(tmp.p);
            tmp.p = next;
            tmp.n = nextlen;
            tmp.cap = nextlen + 1;
        }
    }

    hash_hex(tmp.p, tmp.n, after);
    if (buf_append(out, tmp.p, tmp.n) != 0) {
        buf_free(&tmp);
        return -1;
    }
    buf_free(&tmp);
    counts->n[k]++;
    sites_add(sites, k, va, action, line, before, after);
    *handled = 1;
    return 0;
}

static int handle_insert(Buf *out, const char **pp, const char *end,
                         const char *anchor, const char *anchor_alt,
                         const char *marker,
                         const char *const *ins, int ins_count,
                         Kind k, uint32_t va, int line, Counts *counts,
                         Sites *sites, int *handled, char *err, size_t errsz)
{
    const char *p = *pp;
    const char *le = line_end(p, end);
    const char *ns;
    const char *ne;
    size_t tlen;
    char before[65];
    char after[65];
    char insbuf[1024];
    size_t inslen = 0;
    int i;
    int already;
    const char *term;

    if (!code_equals(p, le, anchor) &&
        (!anchor_alt || !code_equals(p, le, anchor_alt))) {
        snprintf(err, errsz,
                 "site 0x%08X line %d does not match anchor '%s'",
                 va, line, anchor);
        return -1;
    }
    hash_hex(p, (size_t)(le - p), before);

    ns = le;
    ne = line_end(ns, end);
    already = contains_sub(ns, (size_t)(ne - ns), marker);

    if (buf_append(out, p, (size_t)(le - p)) != 0)
        return -1;

    tlen = term_len(p, le);
    term = (tlen == 2) ? "\r\n" : "\n";
    if (tlen == 0)
        term = "\n";

    if (already) {
        if (buf_append(out, ns, (size_t)(ne - ns)) != 0)
            return -1;
        *pp = ne;
        hash_hex(ns, (size_t)(ne - ns), after);
        sites_add(sites, k, va, "already", line, before, after);
    } else {
        for (i = 0; i < ins_count; ++i) {
            size_t l = strlen(ins[i]);

            if (inslen + l + 3 > sizeof(insbuf)) {
                snprintf(err, errsz, "selection24 insertion text too large");
                return -1;
            }
            memcpy(insbuf + inslen, ins[i], l);
            inslen += l;
            memcpy(insbuf + inslen, term, strlen(term));
            inslen += strlen(term);
        }
        hash_hex(insbuf, inslen, after);
        if (buf_append(out, insbuf, inslen) != 0)
            return -1;
        *pp = le;
        sites_add(sites, k, va, "insert", line, before, after);
    }

    counts->n[k]++;
    *handled = 1;
    return 0;
}

static int handle_fill_block(Buf *out, const char **pp, const char *end,
                             int line, Counts *counts, Sites *sites,
                             int *handled, char *err, size_t errsz)
{
    const char *p = *pp;
    const char *le = line_end(p, end);
    const char *q;
    const char *term;
    size_t tlen;
    char before[65];
    char after[65];
    int i;

    if (contains_sub(p, (size_t)(le - p), "wc3_sel24_fill")) {
        const char *ns = le;
        const char *ne = line_end(ns, end);
        uint32_t nv;

        if (parse_va(ns, ne, &nv) && is_fill_va(nv) &&
            code_equals(ns, ne, FILL_CODE[0])) {
            snprintf(err, errsz,
                     "site 0x6F3C7E78 line %d: helper line followed by the "
                     "original fill block", line);
            return -1;
        }
        hash_hex(p, (size_t)(le - p), before);
        if (buf_append(out, p, (size_t)(le - p)) != 0)
            return -1;
        sites_add(sites, K_FILL_BLOCK, VA_FILL_BLOCK, "already", line,
                  before, before);
        counts->n[K_FILL_BLOCK]++;
        *pp = le;
        *handled = 1;
        return 0;
    }

    if (!code_equals(p, le, FILL_CODE[0])) {
        snprintf(err, errsz,
                 "site 0x6F3C7E78 line %d does not match the fill block",
                 line);
        return -1;
    }

    hash_hex(p, (size_t)(le - p), before);
    q = p;
    for (i = 0; i < 13; ++i) {
        const char *qe = line_end(q, end);
        uint32_t v = 0;
        int match = code_equals(q, qe, FILL_CODE[i]);

        if (!match && i == 12)
            match = code_equals(q, qe, FILL_CALL_DIRECT);
        if (!parse_va(q, qe, &v) || v != FILL_VA[i] || !match) {
            snprintf(err, errsz,
                     "fill block line %d (0x%08X) does not match template",
                     i + 1, FILL_VA[i]);
            return -1;
        }
        q = qe;
    }

    tlen = term_len(p, le);
    term = (tlen == 2) ? "\r\n" : "\n";
    if (tlen == 0)
        term = "\n";
    if (buf_append(out, INSERT_FILL, strlen(INSERT_FILL)) != 0)
        return -1;
    if (buf_append(out, term, strlen(term)) != 0)
        return -1;
    hash_hex(INSERT_FILL, strlen(INSERT_FILL), after);
    sites_add(sites, K_FILL_BLOCK, VA_FILL_BLOCK, "block", line,
              before, after);
    counts->n[K_FILL_BLOCK]++;
    *pp = q;
    *handled = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Buffer processing                                                   */
/* ------------------------------------------------------------------ */

static int process_buffer(const char *data, size_t n, const char *profile,
                          Buf *out, Counts *counts, Sites *sites,
                          char *err, size_t errsz)
{
    const char *p = data;
    const char *end = data + n;
    int line = 1;

    if (err && errsz)
        err[0] = '\0';
    memset(counts, 0, sizeof(*counts));

    while (p < end) {
        const char *le = line_end(p, end);
        const char *next = le;
        uint32_t va = 0;
        int has_va = parse_va(p, le, &va);
        int handled = 0;

        if (has_va) {
            switch (va) {
            case VA_GROUP_ALLOC:
                if (handle_subst(out, p, le,
                                 "0x134u", "0x144u",
                                 "push 0x134 */", "push 0x144 */",
                                 K_GROUP_ALLOC, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_INIT_GROUP:
                next = p;
                if (handle_insert(out, &next, end,
                                  INIT_GROUP_DISPATCH, INIT_GROUP_DIRECT,
                                  "wc3_sel24_init_group", INSERT_INIT_LINES, 1,
                                  K_INIT_GROUP, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_SELECT:
                next = p;
                if (handle_insert(out, &next, end,
                                  "wr32((c->ebp + 0xFFFFFFF0u), c->edi);",
                                  NULL,
                                  "wc3_sel24_select", INSERT_SELECT_LINES, 1,
                                  K_SELECT, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_OUTER:
                if (handle_subst(out, p, le,
                                 "(uint32_t)(0xCu)", "(uint32_t)(0x18u)",
                                 "cmp ebx, 0xc */", "cmp ebx, 0x18 */",
                                 K_CAP_OUTER, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_INNER:
                if (handle_subst(out, p, le,
                                 "0xCu", "0x18u",
                                 "cmp ebx, 0xc */", "cmp ebx, 0x18 */",
                                 K_CAP_INNER, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_FILL_BLOCK:
                next = p;
                if (handle_fill_block(out, &next, end, line, counts, sites,
                                      &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_FILL_GRID:
                if (handle_subst(out, p, le,
                                 "0x00000130u", "0x00000138u",
                                 "[ecx + 0x130] */", "[ecx + 0x138] */",
                                 K_FILL_GRID, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_NOTE_SELECTION:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->esi = rd32((c->eax + 0x00000034u));",
                                  NULL,
                                  "wc3_sel24_note_selection", INSERT_NOTE_LINES, 1,
                                  K_NOTE_SELECTION, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_HIDE_ALL:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ebx = c->ecx;",
                                  NULL,
                                  "wc3_sel24_hide_buttons",
                                  INSERT_HIDE, 5,
                                  K_HIDE_ALL, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ADDUNIT_CAP: {
                /* The trace is inserted after the patched comparison and is
                 * compiled out unless WC3_DEBUG_SELECT24 is enabled.  It is
                 * part of the same site: the cap substitution remains the one
                 * counted change, and the insertion is idempotent because the
                 * marker line is already present on a second pass. */
                static const char *const TRACE_LINES[] = {
                    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
                    "    wc3_sel24_trace_addunit(c, c->ebx, c->edi, c->eax); "
                    "/* selection24 addunit trace */",
                    "#endif"
                };
                const char *term;
                size_t tlen;
                size_t ti;
                const char *scan;
                int trace_present = 0;
                int scan_i;

                if (handle_subst(out, p, le,
                                 "0xCu", "0x18u",
                                 "cmp eax, 0xc */", "cmp eax, 0x18 */",
                                 K_ADDUNIT_CAP, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                /* The trace block starts with #if and carries its marker on
                 * the helper line, so inspect the whole short block rather
                 * than only the immediately following line. */
                scan = le;
                for (scan_i = 0; scan_i < 4 && scan < end; ++scan_i) {
                    const char *se = line_end(scan, end);

                    if (contains_sub(scan, (size_t)(se - scan),
                                     "wc3_sel24_trace_addunit")) {
                        trace_present = 1;
                        break;
                    }
                    scan = se;
                }
                if (!trace_present) {
                    tlen = term_len(p, le);
                    term = (tlen == 2) ? "\r\n" : "\n";
                    if (tlen == 0)
                        term = "\n";
                    for (ti = 0;
                         ti < sizeof(TRACE_LINES) / sizeof(TRACE_LINES[0]);
                         ++ti) {
                        if (buf_append(out, TRACE_LINES[ti],
                                       strlen(TRACE_LINES[ti])) != 0 ||
                            buf_append(out, term, strlen(term)) != 0)
                            return -1;
                    }
                }
                break;
            }
            case VA_CAP_LOCAL_LIST:
                if (handle_subst(out, p, le,
                                 "push32(c, 0xCu);", "push32(c, 0x18u);",
                                 "push 0xc */", "push 0x18 */",
                                 K_CAP_LOCAL_LIST, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_COPY_LIST:
                if (handle_subst(out, p, le,
                                 "c->edx = 0xCu;", "c->edx = 0x18u;",
                                 "mov edx, 0xc */", "mov edx, 0x18 */",
                                 K_CAP_COPY_LIST, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_SYNC_ADD:
                if (handle_subst(out, p, le,
                                 "flags_sub(c, rd32((c->ebx + 0x00000010u)), "
                                 "0xCu, 4);",
                                 "flags_sub(c, rd32((c->ebx + 0x00000010u)), "
                                 "0x18u, 4);",
                                 "cmp dword ptr [ebx + 0x10], 0xc */",
                                 "cmp dword ptr [ebx + 0x10], 0x18 */",
                                 K_CAP_SYNC_ADD, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_NET_RECV:
                if (handle_subst(out, p, le,
                                 "flags_sub(c, c->eax, 0xCu, 4);",
                                 "flags_sub(c, c->eax, 0x18u, 4);",
                                 "cmp eax, 0xc */", "cmp eax, 0x18 */",
                                 K_CAP_NET_RECV, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_NET_APPLY:
                if (handle_subst(out, p, le,
                                 "c->edx = 0xCu;", "c->edx = 0x18u;",
                                 "mov edx, 0xc */", "mov edx, 0x18 */",
                                 K_CAP_NET_APPLY, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_NET_SEND:
                if (handle_subst(out, p, le,
                                 "c->ebx = 0xCu;", "c->ebx = 0x18u;",
                                 "mov ebx, 0xc */", "mov ebx, 0x18 */",
                                 K_CAP_NET_SEND, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_SELECT_ALL:
                if (handle_subst(out, p, le,
                                 "push32(c, 0xCu);", "push32(c, 0x18u);",
                                 "push 0xc */", "push 0x18 */",
                                 K_CAP_SELECT_ALL, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_FRAME:
                if (handle_subst(out, p, le,
                                 "0x5D4u", "0x940u",
                                 "sub esp, 0x5d4 */", "sub esp, 0x940 */",
                                 K_ORDER_FRAME, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_SORT1:
            case VA_ORDER_SORT2:
                if (handle_subst(out, p, le,
                                 "0xFFFFFE40u", "0xFFFFF6C0u",
                                 "[ebp - 0x1c0] */", "[ebp - 0x940] */",
                                 K_ORDER_SORT, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_LOOP1:
                if (handle_subst(out, p, le,
                                 "0xFFFFFE58u", "0xFFFFF6D8u",
                                 "[ebp - 0x1a8] */", "[ebp - 0x928] */",
                                 K_ORDER_LOOP, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_LOOP2:
                if (handle_subst(out, p, le,
                                 "0xFFFFFE5Cu", "0xFFFFF6DCu",
                                 "[ebp - 0x1a4] */", "[ebp - 0x924] */",
                                 K_ORDER_LOOP, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            default:
                break;
            }
        }

        if (!handled && buf_append(out, p, (size_t)(le - p)) != 0)
            return -1;
        {
            const char *q;
            for (q = p; q < next; ++q)
                if (*q == '\n')
                    ++line;
        }
        p = next;
    }

    if (profile) {
        const unsigned *exp = NULL;

        if (strcmp(profile, "seed") == 0)
            exp = expected_seed;
        else if (strcmp(profile, "closed") == 0)
            exp = expected_closed;
        else {
            snprintf(err, errsz, "unknown profile '%s'", profile);
            return -1;
        }
        if (!counts_match(counts, exp)) {
            int i;

            for (i = 0; i < K_COUNT; ++i)
                if (counts->n[i] != exp[i])
                    snprintf(err + strlen(err),
                             errsz - strlen(err),
                             "%s%s=%u/%u", strlen(err) ? ", " : "",
                             kind_name[i], counts->n[i], exp[i]);
            snprintf(err + strlen(err), errsz - strlen(err),
                     ": profile %s counts do not match", profile);
            return -1;
        }
    }

    return 0;
}

static int write_json(const char *path, const char *input, const char *output,
                      const char *profile, const Counts *c, const Sites *s,
                      char *err, size_t errsz);

/* ------------------------------------------------------------------ */
/* File IO                                                             */
/* ------------------------------------------------------------------ */

static char *read_file(const char *path, size_t *out_n, char *err,
                       size_t errsz)
{
    FILE *f;
    char *buf;
    size_t cap = 1 << 20;
    size_t n = 0;

    f = fopen(path, "rb");
    if (!f) {
        if (err)
            snprintf(err, errsz, "cannot open '%s': %s",
                     path, strerror(errno));
        return NULL;
    }
    buf = (char *)malloc(cap + 1);
    if (!buf) {
        fclose(f);
        if (err)
            snprintf(err, errsz, "out of memory reading '%s'", path);
        return NULL;
    }
    for (;;) {
        size_t got;
        if (n == cap) {
            char *nb;
            cap *= 2;
            nb = (char *)realloc(buf, cap + 1);
            if (!nb) {
                free(buf);
                fclose(f);
                if (err)
                    snprintf(err, errsz, "out of memory reading '%s'", path);
                return NULL;
            }
            buf = nb;
        }
        got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) {
            if (ferror(f)) {
                free(buf);
                fclose(f);
                if (err)
                    snprintf(err, errsz, "read error on '%s': %s",
                             path, strerror(errno));
                return NULL;
            }
            break;
        }
    }
    fclose(f);
    buf[n] = '\0';
    *out_n = n;
    return buf;
}

static int files_equal(const char *path, const char *data, size_t n)
{
    size_t m = 0;
    char *old = read_file(path, &m, NULL, 0);
    int eq = 0;

    if (old) {
        eq = (m == n) && memcmp(old, data, n) == 0;
        free(old);
    }
    return eq;
}

/* A small portability shim so the tool never pulls in windows.h. */
static unsigned long temp_serial(void)
{
    static unsigned long serial;
    return (++serial * 2654435761u) +
           (unsigned long)(uintptr_t)&serial;
}

static int write_file_atomic(const char *path, const char *data, size_t n,
                             char *err, size_t errsz)
{
    char tmp[4096];
    FILE *f;
    int wrote = 0;

    if (files_equal(path, data, n))
        return 0;

    snprintf(tmp, sizeof(tmp), "%s.tmp.%lu", path,
             (unsigned long)temp_serial());
    f = fopen(tmp, "wb");
    if (!f) {
        snprintf(err, errsz, "cannot create '%s': %s", tmp, strerror(errno));
        return -1;
    }
    wrote = fwrite(data, 1, n, f) == n;
    if (fclose(f) != 0)
        wrote = 0;
    if (!wrote) {
        remove(tmp);
        snprintf(err, errsz, "short write on '%s'", tmp);
        return -1;
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        snprintf(err, errsz, "cannot replace '%s': %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int run_pass(const char *in_path, const char *out_path,
                    const char *json_path, const char *profile, int quiet,
                    char *err, size_t errsz)
{
    size_t n = 0;
    char *data;
    Buf out = {0};
    Counts counts;
    Sites sites = {0};
    int rc;

    data = read_file(in_path, &n, err, errsz);
    if (!data)
        return -1;
    rc = process_buffer(data, n, profile, &out, &counts, &sites,
                        err, errsz);
    free(data);
    if (rc != 0) {
        buf_free(&out);
        free(sites.v);
        return -1;
    }
    if (out_path && write_file_atomic(out_path, out.p, out.n, err, errsz) != 0) {
        buf_free(&out);
        free(sites.v);
        return -1;
    }
    if (json_path &&
        write_json(json_path, in_path, out_path, profile, &counts, &sites,
                   err, errsz) != 0) {
        buf_free(&out);
        free(sites.v);
        return -1;
    }
    if (!quiet) {
        printf("%s: %s -> %s (%u site%s", PASS, in_path,
               out_path ? out_path : "-", counts_sum(&counts),
               counts_sum(&counts) == 1 ? "" : "s");
        if (profile)
            printf(", profile %s", profile);
        printf(")\n");
    }
    buf_free(&out);
    free(sites.v);
    return 0;
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */

static void write_json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (c < 0x20)
                fprintf(f, "\\u%04x", c);
            else
                fputc(c, f);
            break;
        }
    }
    fputc('"', f);
}

static int write_json(const char *path, const char *input, const char *output,
                      const char *profile, const Counts *c, const Sites *s,
                      char *err, size_t errsz)
{
    FILE *f;
    int i;
    size_t j;

    f = fopen(path, "wb");
    if (!f) {
        snprintf(err, errsz, "cannot create '%s': %s", path, strerror(errno));
        return -1;
    }
    fputs("{\n", f);
    fputs("  \"pass\": \"" PASS "\",\n", f);
    fprintf(f, "  \"pass_rev\": %d,\n", PASS_REV);
    fputs("  \"input\": ", f);
    write_json_string(f, input);
    fputs(",\n  \"output\": ", f);
    write_json_string(f, output ? output : "");
    fputs(",\n  \"profile\": ", f);
    if (profile)
        write_json_string(f, profile);
    else
        fputs("null", f);
    fputs(",\n  \"counts\": {\n", f);
    for (i = 0; i < K_COUNT; ++i) {
        fputs("    \"", f);
        fputs(kind_name[i], f);
        fprintf(f, "\": %u", c->n[i]);
        fputs(i + 1 == K_COUNT ? "\n" : ",\n", f);
    }
    fputs("  },\n", f);
    fprintf(f, "  \"total\": %u,\n", counts_sum(c));
    fputs("  \"applied\": ", f);
    {
        int applied = 0;
        for (j = 0; j < s->n; ++j)
            if (strcmp(s->v[j].action, "already") != 0)
                applied = 1;
        fputs(applied ? "true" : "false", f);
    }
    fputs(",\n  \"sites\": [", f);
    if (s->n) {
        fputc('\n', f);
        for (j = 0; j < s->n; ++j) {
            const Site *st = &s->v[j];
            fputs("    {\n      \"kind\": ", f);
            write_json_string(f, kind_name[st->kind]);
            fprintf(f, ",\n      \"va\": \"0x%08X\"", st->va);
            fputs(",\n      \"action\": ", f);
            write_json_string(f, st->action);
            fprintf(f, ",\n      \"line\": %d", st->line);
            fputs(",\n      \"before_sha256\": ", f);
            write_json_string(f, st->before);
            fputs(",\n      \"after_sha256\": ", f);
            write_json_string(f, st->after);
            fputs("\n    }", f);
            fputs(j + 1 == s->n ? "\n" : ",\n", f);
        }
        fputs("  ]\n", f);
    } else {
        fputs("]\n", f);
    }
    fputs("}\n", f);
    if (fclose(f) != 0) {
        snprintf(err, errsz, "write error on '%s': %s",
                 path, strerror(errno));
        return -1;
    }
    return 0;
}

static int json_counts_parse(const char *text, Counts *c, char *err,
                             size_t errsz)
{
    const char *p = strstr(text, "\"counts\"");
    const char *q;
    unsigned seen[K_COUNT];
    int i;

    memset(c, 0, sizeof(*c));
    memset(seen, 0, sizeof(seen));
    if (!p) {
        snprintf(err, errsz, "no counts object");
        return -1;
    }
    p = strchr(p + 8, '{');
    if (!p) {
        snprintf(err, errsz, "malformed counts object");
        return -1;
    }
    ++p;
    for (;;) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' ||
               *p == ',')
            ++p;
        if (*p == '}' || *p == '\0')
            break;
        if (*p != '"') {
            snprintf(err, errsz, "malformed counts key");
            return -1;
        }
        ++p;
        q = strchr(p, '"');
        if (!q) {
            snprintf(err, errsz, "unterminated counts key");
            return -1;
        }
        {
            char name[64];
            size_t len = (size_t)(q - p);
            int kind = -1;
            unsigned long ul;
            char *endp = NULL;

            if (len >= sizeof(name)) {
                snprintf(err, errsz, "counts key too long");
                return -1;
            }
            memcpy(name, p, len);
            name[len] = '\0';
            for (i = 0; i < K_COUNT; ++i)
                if (strcmp(name, kind_name[i]) == 0)
                    kind = i;
            if (kind < 0) {
                snprintf(err, errsz, "unknown counts key '%s'", name);
                return -1;
            }
            p = q + 1;
            while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
                ++p;
            if (*p != ':') {
                snprintf(err, errsz, "missing ':' after counts key '%s'",
                         name);
                return -1;
            }
            ++p;
            while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
                ++p;
            errno = 0;
            ul = strtoul(p, &endp, 10);
            if (errno != 0 || endp == p || ul > UINT_MAX) {
                snprintf(err, errsz, "bad value for counts key '%s'", name);
                return -1;
            }
            if (seen[kind]) {
                snprintf(err, errsz, "duplicate counts key '%s'", name);
                return -1;
            }
            seen[kind] = 1;
            c->n[kind] = (unsigned)ul;
            p = endp;
        }
    }
    return 0;
}

static int json_pass_ok(const char *text)
{
    return strstr(text, "\"" PASS "\"") != NULL;
}

static int verify_coverage_one(const char *path, Counts *sum, int *reports,
                               char *err, size_t errsz)
{
    char *text;
    size_t n = 0;
    Counts c;

    text = read_file(path, &n, err, errsz);
    if (!text)
        return -1;
    if (!json_pass_ok(text)) {
        free(text);
        snprintf(err, errsz, "'%s' is not a " PASS " report", path);
        return -1;
    }
    if (json_counts_parse(text, &c, err, errsz) != 0) {
        free(text);
        return -1;
    }
    free(text);
    {
        int i;
        for (i = 0; i < K_COUNT; ++i)
            sum->n[i] += c.n[i];
    }
    ++*reports;
    return 0;
}

static int verify_coverage(const char *arg, int quiet, char *err,
                           size_t errsz)
{
    const char *manifest;
    char *text;
    size_t n = 0;
    Counts sum;
    int reports = 0;
    int failed = 0;

    memset(&sum, 0, sizeof(sum));
    manifest = (arg[0] == '@') ? arg + 1 : NULL;
    if (manifest) {
        text = read_file(manifest, &n, err, errsz);
        if (!text)
            return -1;
        {
            char *p = text;
            char *end = text + n;
            while (p < end) {
                char *le = (char *)find_sub(p, (size_t)(end - p), "\n");
                size_t len = le ? (size_t)(le - p) : (size_t)(end - p);
                char path[4096];

                while (len && (p[len - 1] == '\r' || p[len - 1] == ' ' ||
                               p[len - 1] == '\t'))
                    --len;
                if (len) {
                    if (len >= sizeof(path))
                        len = sizeof(path) - 1;
                    memcpy(path, p, len);
                    path[len] = '\0';
                    if (verify_coverage_one(path, &sum, &reports,
                                            err, errsz) != 0) {
                        failed = 1;
                        break;
                    }
                }
                if (!le)
                    break;
                p = le + 1;
            }
        }
        free(text);
    } else {
        if (verify_coverage_one(arg, &sum, &reports, err, errsz) != 0)
            failed = 1;
    }

    if (!failed && reports == 0) {
        snprintf(err, errsz, "coverage manifest is empty");
        return -1;
    }
    if (!failed && !counts_match(&sum, expected_total)) {
        int i;

        snprintf(err, errsz, "coverage mismatch:");
        for (i = 0; i < K_COUNT; ++i)
            if (sum.n[i] != expected_total[i])
                snprintf(err + strlen(err), errsz - strlen(err),
                         " %s=%u/%u", kind_name[i], sum.n[i],
                         expected_total[i]);
        failed = 1;
    }

    if (!quiet) {
        int i;
        printf("%s coverage:", PASS);
        for (i = 0; i < K_COUNT; ++i)
            printf(" %s=%u", kind_name[i], sum.n[i]);
        printf(" reports=%d %s\n", reports,
               failed ? "MISMATCH" : "OK");
    }
    return failed ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

static const char SELF_FIXTURE[] =
"/* selection24 self-test fixture */\n"
"void L_fixture(CPU *restrict c)\n"
"{\n"
"    push32(c, 0x134u);                                           /* 6F39E7DE: push 0x134 */\n"
"    push32(c, 0x6F39EDA9u); dispatch(c, 0x6F3B91B0u);            /* 6F39EDA4: call 0x6f3b91b0 */\n"
"    push32(c, 0x6F39EDA9u); WC3_DIRECT_CALL(L_6F3B91B0, c);            /* 6F39EDA4: call 0x6f3b91b0 */\n"
"    c->esi = rd32((c->eax + 0x00000034u));                       /* 6F3C7D35: mov esi, dword ptr [eax + 0x34] */\n"
"    wr32((c->ebp + 0xFFFFFFF0u), c->edi);                        /* 6F3C7DBB: mov dword ptr [ebp - 0x10], edi */\n"
"    wr32((c->ebp + 0xFFFFFFF0u), c->edi);                        /* 6F3C7DBB: mov dword ptr [ebp - 0x10], edi */\n"
"    { uint32_t _fa_6F3C7D71 = (uint32_t)(c->ebx); uint32_t _fb_6F3C7D71 = (uint32_t)(0xCu);  /* 6F3C7D71: cmp ebx, 0xc */\n"
"    { uint32_t _fa_6F3C7D71 = (uint32_t)(c->ebx); uint32_t _fb_6F3C7D71 = (uint32_t)(0xCu);  /* 6F3C7D71: cmp ebx, 0xc */\n"
"    flags_sub(c, c->ebx, 0xCu, 4);                               /* 6F3C7DD0: cmp ebx, 0xc */\n"
"    flags_sub(c, c->ebx, 0xCu, 4);                               /* 6F3C7DD0: cmp ebx, 0xc */\n"
"    push32(c, rd32((c->ebp + 0xFFFFFFE4u)));                     /* 6F3C7E78: push dword ptr [ebp - 0x1c] */\n"
"    c->eax = 0xAAAAAAABu;                                        /* 6F3C7E7B: mov eax, 0xaaaaaaab */\n"
"    { uint64_t _p=(uint64_t)c->eax*(uint32_t)(c->ebx); c->eax=(uint32_t)_p; c->edx=(uint32_t)(_p>>32); } /* 6F3C7E80: mul ebx */\n"
"    push32(c, c->esi);                                           /* 6F3C7E82: push esi */\n"
"    c->edx = op_shr_nf(c->edx, 0x2u, 4);                         /* 6F3C7E83: shr edx, 2 */\n"
"    c->eax = c->ebx;                                             /* 6F3C7E86: mov eax, ebx */\n"
"    push32(c, c->edx);                                           /* 6F3C7E88: push edx */\n"
"    c->ecx = (uint32_t)((c->edx + c->edx*2));                    /* 6F3C7E89: lea ecx, [edx + edx*2] */\n"
"    c->ecx = add32(c->ecx, c->ecx, 4);                           /* 6F3C7E8C: add ecx, ecx */\n"
"    c->eax = flags_sub(c, c->eax, c->ecx, 4);                    /* 6F3C7E8E: sub eax, ecx */\n"
"    c->ecx = rd32((c->ebp + 0xFFFFFFE0u));                       /* 6F3C7E90: mov ecx, dword ptr [ebp - 0x20] */\n"
"    push32(c, c->eax);                                           /* 6F3C7E93: push eax */\n"
"    push32(c, 0x6F3C7E99u); dispatch(c, 0x6F3DA470u);            /* 6F3C7E94: call 0x6f3da470 */\n"
"    push32(c, rd32((c->ebp + 0xFFFFFFE4u)));                     /* 6F3C7E78: push dword ptr [ebp - 0x1c] */\n"
"    c->eax = 0xAAAAAAABu;                                        /* 6F3C7E7B: mov eax, 0xaaaaaaab */\n"
"    { uint64_t _p=(uint64_t)c->eax*(uint32_t)(c->ebx); c->eax=(uint32_t)_p; c->edx=(uint32_t)(_p>>32); } /* 6F3C7E80: mul ebx */\n"
"    push32(c, c->esi);                                           /* 6F3C7E82: push esi */\n"
"    c->edx = op_shr_nf(c->edx, 0x2u, 4);                         /* 6F3C7E83: shr edx, 2 */\n"
"    c->eax = c->ebx;                                             /* 6F3C7E86: mov eax, ebx */\n"
"    push32(c, c->edx);                                           /* 6F3C7E88: push edx */\n"
"    c->ecx = (uint32_t)((c->edx + c->edx*2));                    /* 6F3C7E89: lea ecx, [edx + edx*2] */\n"
"    c->ecx = add32(c->ecx, c->ecx, 4);                           /* 6F3C7E8C: add ecx, ecx */\n"
"    c->eax = flags_sub(c, c->eax, c->ecx, 4);                    /* 6F3C7E8E: sub eax, ecx */\n"
"    c->ecx = rd32((c->ebp + 0xFFFFFFE0u));                       /* 6F3C7E90: mov ecx, dword ptr [ebp - 0x20] */\n"
"    push32(c, c->eax);                                           /* 6F3C7E93: push eax */\n"
"    push32(c, 0x6F3C7E99u); WC3_DIRECT_CALL(L_6F3DA470, c);            /* 6F3C7E94: call 0x6f3da470 */\n"
"    c->eax = rd32((c->ecx + 0x00000130u));                       /* 6F3DA47F: mov eax, dword ptr [ecx + 0x130] */\n"
"    flags_sub(c, c->eax, 0xCu, 4);                               /* 6F28DC30: cmp eax, 0xc */\n"
"    push32(c, 0xCu);                                             /* 6F28EB97: push 0xc */\n"
"    push32(c, 0xCu);                                             /* 6F28EB97: push 0xc */\n"
"    push32(c, 0xCu);                                             /* 6F28EB97: push 0xc */\n"
"    push32(c, 0xCu);                                             /* 6F28EB97: push 0xc */\n"
"    push32(c, 0xCu);                                             /* 6F28EB97: push 0xc */\n"
"    push32(c, 0xCu);                                             /* 6F390293: push 0xc */\n"
"    c->edx = 0xCu;                                               /* 6F391AD2: mov edx, 0xc */\n"
"    c->edx = 0xCu;                                               /* 6F391AD2: mov edx, 0xc */\n"
"    c->edx = 0xCu;                                               /* 6F391AD2: mov edx, 0xc */\n"
"    flags_sub(c, rd32((c->ebx + 0x00000010u)), 0xCu, 4);         /* 6F28D667: cmp dword ptr [ebx + 0x10], 0xc */\n"
"    flags_sub(c, c->eax, 0xCu, 4);                               /* 6F6BC82A: cmp eax, 0xc */\n"
"    c->edx = 0xCu;                                               /* 6F6BCAC9: mov edx, 0xc */\n"
"    c->edx = 0xCu;                                               /* 6F6BCAC9: mov edx, 0xc */\n"
"    c->edx = 0xCu;                                               /* 6F6BCAC9: mov edx, 0xc */\n"
"    c->edx = 0xCu;                                               /* 6F6BCAC9: mov edx, 0xc */\n"
"    c->edx = 0xCu;                                               /* 6F6BCAC9: mov edx, 0xc */\n"
"    c->ebx = 0xCu;                                               /* 6F6BCF1D: mov ebx, 0xc */\n"
"    c->ebx = c->ecx;                                             /* 6F3ACCA3: mov ebx, ecx */\n"
"    c->esp = flags_sub(c, c->esp, 0x5D4u, 4);                    /* 6F6B7F01: sub esp, 0x5d4 */\n"
"    c->ecx = (uint32_t)((c->ebp + 0xFFFFFE40u));                 /* 6F6B9E05: lea ecx, [ebp - 0x1c0] */\n"
"    c->ecx = (uint32_t)((c->ebp + 0xFFFFFE40u));                 /* 6F6B9E05: lea ecx, [ebp - 0x1c0] */\n"
"    c->ecx = (uint32_t)((c->ebp + 0xFFFFFE40u));                 /* 6F6BA444: lea ecx, [ebp - 0x1c0] */\n"
"    c->ecx = (uint32_t)((c->ebp + 0xFFFFFE40u));                 /* 6F6BA444: lea ecx, [ebp - 0x1c0] */\n"
"    c->esi = (uint32_t)((c->ebp + 0xFFFFFE58u));                 /* 6F6B9E34: lea esi, [ebp - 0x1a8] */\n"
"    c->esi = (uint32_t)((c->ebp + 0xFFFFFE58u));                 /* 6F6B9E34: lea esi, [ebp - 0x1a8] */\n"
"    c->esi = (uint32_t)((c->ebp + 0xFFFFFE5Cu));                 /* 6F6BA473: lea esi, [ebp - 0x1a4] */\n"
"    c->esi = (uint32_t)((c->ebp + 0xFFFFFE5Cu));                 /* 6F6BA473: lea esi, [ebp - 0x1a4] */\n"
"}\n";

static size_t count_occurrences(const char *p, size_t n, const char *needle)
{
    size_t count = 0;
    const char *q = p;

    while ((q = find_sub(q, n - (size_t)(q - p), needle)) != NULL) {
        ++count;
        q += strlen(needle);
    }
    return count;
}

static int self_test(char *err, size_t errsz)
{
    Buf out1 = {0};
    Buf out2 = {0};
    Counts c1;
    Counts c2;
    Sites s1 = {0};
    Sites s2 = {0};
    int ok = 1;
    char err2[512];

    if (process_buffer(SELF_FIXTURE, strlen(SELF_FIXTURE), NULL,
                       &out1, &c1, &s1, err2, sizeof(err2)) != 0) {
        snprintf(err, errsz, "self-test first pass failed: %s", err2);
        goto fail;
    }
    if (!counts_match(&c1, expected_total)) {
        snprintf(err, errsz, "self-test counts mismatch after first pass");
        goto fail;
    }
    if (count_occurrences(out1.p, out1.n, "wc3_sel24_init_group") != 2 ||
        count_occurrences(out1.p, out1.n, "wc3_sel24_note_selection") != 1 ||
        count_occurrences(out1.p, out1.n, "wc3_sel24_select") != 2 ||
        count_occurrences(out1.p, out1.n,
                          "wc3_sel24_hide_buttons(c, c->ebx)") != 1 ||
        count_occurrences(out1.p, out1.n, "wc3_sel24_fill") != 2 ||
        count_occurrences(out1.p, out1.n, "0x144u") != 1 ||
        count_occurrences(out1.p, out1.n, "0x00000138u") != 1 ||
        count_occurrences(out1.p, out1.n, "0x18u") != 22 ||
        count_occurrences(out1.p, out1.n, "0x940u") != 1 ||
        count_occurrences(out1.p, out1.n, "0xFFFFF6C0u") != 4 ||
        count_occurrences(out1.p, out1.n, "0xFFFFF6D8u") != 2 ||
        count_occurrences(out1.p, out1.n, "0xFFFFF6DCu") != 2 ||
        count_occurrences(out1.p, out1.n,
                          "wc3_sel24_trace_addunit") != 1) {
        snprintf(err, errsz, "self-test first pass content mismatch");
        goto fail;
    }

    if (process_buffer(out1.p, out1.n, NULL, &out2, &c2, &s2,
                       err2, sizeof(err2)) != 0) {
        snprintf(err, errsz, "self-test second pass failed: %s", err2);
        goto fail;
    }
    if (!counts_match(&c2, expected_total)) {
        snprintf(err, errsz, "self-test counts mismatch after second pass");
        goto fail;
    }
    if (out1.n != out2.n || memcmp(out1.p, out2.p, out1.n) != 0) {
        snprintf(err, errsz, "self-test is not idempotent");
        goto fail;
    }

    {
        size_t flen = strlen(SELF_FIXTURE);
        char *tampered = (char *)malloc(flen + 1);
        Buf out3 = {0};
        Counts c3;
        Sites s3 = {0};

        if (!tampered) {
            snprintf(err, errsz, "self-test out of memory");
            goto fail;
        }
        memcpy(tampered, SELF_FIXTURE, flen + 1);
        {
            char *hit = (char *)find_sub(tampered, flen, "0x134u");
            if (hit)
                memcpy(hit, "0x999u", 6);
        }
        if (process_buffer(tampered, flen, NULL, &out3, &c3, &s3,
                           err2, sizeof(err2)) == 0) {
            snprintf(err, errsz, "self-test did not reject a tampered template");
            free(tampered);
            buf_free(&out3);
            free(s3.v);
            goto fail;
        }
        free(tampered);
        buf_free(&out3);
        free(s3.v);
    }

    {
        const char json[] =
            "{\"pass\":\"" PASS "\",\"pass_rev\":4,\"counts\":{"
            "\"group_alloc\":1,\"init_group\":2,\"select\":2,"
            "\"cap_outer\":2,\"cap_inner\":2,\"fill_block\":2,"
            "\"fill_grid\":1,\"note_selection\":1,\"hide_all\":1,"
            "\"addunit_cap\":1,\"cap_local_list\":5,\"cap_copy_list\":3,"
            "\"cap_sync_add\":1,\"cap_net_recv\":1,\"cap_net_apply\":5,"
            "\"cap_net_send\":1,\"cap_select_all\":1,"
            "\"order_frame\":1,\"order_sort\":4,\"order_loop\":4},"
            "\"total\":41}";
        Counts c3;

        if (json_counts_parse(json, &c3, err2, sizeof(err2)) != 0 ||
            !counts_match(&c3, expected_total)) {
            snprintf(err, errsz, "self-test coverage parser mismatch");
            goto fail;
        }
    }

    printf("self-test: OK (%u sites, idempotent, fail-closed)\n",
           counts_sum(&c1));
    ok = 1;
    goto done;

fail:
    ok = 0;
done:
    buf_free(&out1);
    buf_free(&out2);
    free(s1.v);
    free(s2.v);
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
            "usage: " PASS " --lift IN --out OUT [--json REPORT] "
            "[--profile seed|closed] [--quiet]\n"
            "       " PASS " --verify-coverage @manifest [--quiet]\n"
            "       " PASS " --self-test\n");
}

int main(int argc, char **argv)
{
    const char *lift = NULL;
    const char *out = NULL;
    const char *json = NULL;
    const char *profile = NULL;
    const char *coverage = NULL;
    int quiet = 0;
    int self = 0;
    int i;
    char err[1024];

    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--lift") == 0 && i + 1 < argc) {
            lift = argv[++i];
        } else if (strcmp(a, "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(a, "--json") == 0 && i + 1 < argc) {
            json = argv[++i];
        } else if (strcmp(a, "--profile") == 0 && i + 1 < argc) {
            profile = argv[++i];
        } else if (strcmp(a, "--verify-coverage") == 0 && i + 1 < argc) {
            coverage = argv[++i];
        } else if (strcmp(a, "--quiet") == 0) {
            quiet = 1;
        } else if (strcmp(a, "--self-test") == 0) {
            self = 1;
        } else {
            usage();
            return 2;
        }
    }

    if (self) {
        if (self_test(err, sizeof(err)) != 0) {
            fprintf(stderr, "%s: %s\n", PASS, err);
            return 1;
        }
        return 0;
    }

    if (coverage) {
        if (verify_coverage(coverage, quiet, err, sizeof(err)) != 0) {
            fprintf(stderr, "%s: %s\n", PASS, err);
            return 1;
        }
        return 0;
    }

    if (!lift || !out) {
        usage();
        return 2;
    }
    if (run_pass(lift, out, json, profile, quiet, err, sizeof(err)) != 0) {
        fprintf(stderr, "%s: %s\n", PASS, err);
        return 1;
    }
    return 0;
}