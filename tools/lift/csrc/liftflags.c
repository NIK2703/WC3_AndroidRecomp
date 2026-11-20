/* liftflags.c - see header.  Kept free of capstone and PE dependencies so the
 * liveness and the generated conditions can be exercised by a host self-test. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "liftflags.h"

typedef struct {
    char *p;
    size_t n;
    size_t cap;
    int ok;
} LfSb;

static void sb_init(LfSb *b, char *p, size_t cap)
{
    b->p = p;
    b->n = 0;
    b->cap = cap;
    b->ok = cap != 0;
    if (cap) p[0] = 0;
}

static void sb_puts(LfSb *b, const char *s)
{
    size_t n;
    if (!b->ok) return;
    n = strlen(s);
    if (b->n + n + 1 > b->cap) {
        b->ok = 0;
        return;
    }
    memcpy(b->p + b->n, s, n + 1);
    b->n += n;
}

static void sb_printf(LfSb *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    int n;
    if (!b->ok) return;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        b->ok = 0;
        return;
    }
    sb_puts(b, tmp);
}

uint8_t lf_cond_mask(const char *cc)
{
    static const struct {
        const char *cc;
        uint8_t mask;
    } tbl[] = {
        {"e", LF_FLAG_ZF},   {"z", LF_FLAG_ZF},
        {"ne", LF_FLAG_ZF},  {"nz", LF_FLAG_ZF},
        {"b", LF_FLAG_CF},   {"c", LF_FLAG_CF},   {"nae", LF_FLAG_CF},
        {"ae", LF_FLAG_CF},  {"nb", LF_FLAG_CF},  {"nc", LF_FLAG_CF},
        {"be", LF_FLAG_CF | LF_FLAG_ZF},
        {"na", LF_FLAG_CF | LF_FLAG_ZF},
        {"a", LF_FLAG_CF | LF_FLAG_ZF},
        {"nbe", LF_FLAG_CF | LF_FLAG_ZF},
        {"l", LF_FLAG_SF | LF_FLAG_OF},
        {"nge", LF_FLAG_SF | LF_FLAG_OF},
        {"ge", LF_FLAG_SF | LF_FLAG_OF},
        {"nl", LF_FLAG_SF | LF_FLAG_OF},
        {"le", LF_FLAG_ZF | LF_FLAG_SF | LF_FLAG_OF},
        {"ng", LF_FLAG_ZF | LF_FLAG_SF | LF_FLAG_OF},
        {"g", LF_FLAG_ZF | LF_FLAG_SF | LF_FLAG_OF},
        {"nle", LF_FLAG_ZF | LF_FLAG_SF | LF_FLAG_OF},
        {"s", LF_FLAG_SF},   {"ns", LF_FLAG_SF},
        {"o", LF_FLAG_OF},   {"no", LF_FLAG_OF},
        {"p", LF_FLAG_PF},   {"pe", LF_FLAG_PF},
        {"np", LF_FLAG_PF},  {"po", LF_FLAG_PF}
    };
    size_t i;
    if (!cc) return 0;
    for (i = 0; i < sizeof tbl / sizeof tbl[0]; i++)
        if (!strcmp(tbl[i].cc, cc)) return tbl[i].mask;
    return 0;
}

static int flag_bits(int sz)
{
    return sz * 8;
}

static const char *flag_sign_str(int sz)
{
    if (sz == 1) return "0x80u";
    if (sz == 2) return "0x8000u";
    return "0x80000000u";
}

static const char *flag_max_str(int sz)
{
    if (sz == 1) return "0x7Fu";
    if (sz == 2) return "0x7FFFu";
    return "0x7FFFFFFFu";
}

static const char *flag_min_str(int sz)
{
    if (sz == 1) return "0x80u";
    if (sz == 2) return "0x8000u";
    return "0x80000000u";
}

static void expr_cf(LfSb *b, LfFlagKind kind, int sz, const char *fa,
                    const char *fb, const char *fr, const char *ffull)
{
    switch (kind) {
    case LF_FLAG_CMP:
    case LF_FLAG_SUB:
        sb_printf(b, "(%s < %s)", fa, fb);
        break;
    case LF_FLAG_SBB:
        sb_printf(b, "((%s >> %d) & 1u)", ffull, flag_bits(sz));
        break;
    case LF_FLAG_ADD:
        sb_printf(b, "(%s < %s)", fr, fa);
        break;
    case LF_FLAG_ADC:
        sb_printf(b, "((%s >> %d) & 1u)", ffull, flag_bits(sz));
        break;
    case LF_FLAG_NEG:
        sb_printf(b, "(%s != 0u)", fb);
        break;
    case LF_FLAG_TEST:
    case LF_FLAG_AND:
    case LF_FLAG_OR:
    case LF_FLAG_XOR:
        sb_puts(b, "0u");
        break;
    default:
        b->ok = 0;
        break;
    }
}

static void expr_zf(LfSb *b, const char *fr)
{
    sb_printf(b, "(%s == 0u)", fr);
}

static void expr_sf(LfSb *b, int sz, const char *fr)
{
    sb_printf(b, "((%s & %s) != 0u)", fr, flag_sign_str(sz));
}

static void expr_of(LfSb *b, LfFlagKind kind, int sz, const char *fa,
                    const char *fb, const char *fr)
{
    switch (kind) {
    case LF_FLAG_ADD:
    case LF_FLAG_ADC:
        sb_printf(b, "((~(%s ^ %s) & (%s ^ %s) & %s) != 0u)",
                  fa, fb, fa, fr, flag_sign_str(sz));
        break;
    case LF_FLAG_SUB:
    case LF_FLAG_CMP:
    case LF_FLAG_SBB:
        sb_printf(b, "(((%s ^ %s) & (%s ^ %s) & %s) != 0u)",
                  fa, fb, fa, fr, flag_sign_str(sz));
        break;
    case LF_FLAG_NEG:
        sb_printf(b, "(%s == %s)", fb, flag_min_str(sz));
        break;
    case LF_FLAG_INC:
        sb_printf(b, "(%s == %s)", fa, flag_max_str(sz));
        break;
    case LF_FLAG_DEC:
        sb_printf(b, "(%s == %s)", fa, flag_min_str(sz));
        break;
    case LF_FLAG_TEST:
    case LF_FLAG_AND:
    case LF_FLAG_OR:
    case LF_FLAG_XOR:
        sb_puts(b, "0u");
        break;
    default:
        b->ok = 0;
        break;
    }
}

static void expr_pf(LfSb *b, const char *fr)
{
    /* WC3_PF_VALUE is variant-aware: with WC3_FLAGS_FAST_PF_AF the helper
     * leaves PF alone, so the fused condition must read the stored field
     * instead of computing parity. */
    sb_printf(b, "(WC3_PF_VALUE(c, %s) != 0u)", fr);
}

int lf_flag_cond(char *buf, size_t cap, const char *cc, LfFlagKind kind,
                 int sz, const char *fa, const char *fb, const char *fr,
                 const char *ffull, const char *fcin)
{
    char cf[1024], zf[1024], sf[1024], of[1024], pf[1024];
    uint8_t need;
    LfSb out;
    (void)fcin;

    if (!buf || !cap) return -1;
    if (sz != 1 && sz != 2 && sz != 4) return -1;
    need = lf_cond_mask(cc);
    if (!need) return -1;
    if (kind == LF_FLAG_NONE) return -1;
    if ((kind == LF_FLAG_INC || kind == LF_FLAG_DEC) &&
        (need & LF_FLAG_CF))
        return -1;
    if ((kind == LF_FLAG_ADC || kind == LF_FLAG_SBB) && !ffull)
        return -1;

    cf[0] = zf[0] = sf[0] = of[0] = pf[0] = 0;
    if (need & LF_FLAG_CF) {
        LfSb b;
        sb_init(&b, cf, sizeof cf);
        expr_cf(&b, kind, sz, fa, fb, fr, ffull);
        if (!b.ok) return -1;
    }
    if (need & LF_FLAG_ZF) {
        LfSb b;
        sb_init(&b, zf, sizeof zf);
        expr_zf(&b, fr);
        if (!b.ok) return -1;
    }
    if (need & LF_FLAG_SF) {
        LfSb b;
        sb_init(&b, sf, sizeof sf);
        expr_sf(&b, sz, fr);
        if (!b.ok) return -1;
    }
    if (need & LF_FLAG_OF) {
        LfSb b;
        sb_init(&b, of, sizeof of);
        expr_of(&b, kind, sz, fa, fb, fr);
        if (!b.ok) return -1;
    }
    if (need & LF_FLAG_PF) {
        LfSb b;
        sb_init(&b, pf, sizeof pf);
        expr_pf(&b, fr);
        if (!b.ok) return -1;
    }

    sb_init(&out, buf, cap);
    if (!strcmp(cc, "e") || !strcmp(cc, "z")) {
        sb_puts(&out, zf);
    } else if (!strcmp(cc, "ne") || !strcmp(cc, "nz")) {
        sb_printf(&out, "!(%s)", zf);
    } else if (!strcmp(cc, "b") || !strcmp(cc, "c") ||
               !strcmp(cc, "nae")) {
        sb_puts(&out, cf);
    } else if (!strcmp(cc, "ae") || !strcmp(cc, "nb") ||
               !strcmp(cc, "nc")) {
        sb_printf(&out, "!(%s)", cf);
    } else if (!strcmp(cc, "be") || !strcmp(cc, "na")) {
        sb_printf(&out, "(%s || %s)", cf, zf);
    } else if (!strcmp(cc, "a") || !strcmp(cc, "nbe")) {
        sb_printf(&out, "(!(%s) && !(%s))", cf, zf);
    } else if (!strcmp(cc, "l") || !strcmp(cc, "nge")) {
        sb_printf(&out, "(%s != %s)", sf, of);
    } else if (!strcmp(cc, "ge") || !strcmp(cc, "nl")) {
        sb_printf(&out, "(%s == %s)", sf, of);
    } else if (!strcmp(cc, "le") || !strcmp(cc, "ng")) {
        sb_printf(&out, "(%s || (%s != %s))", zf, sf, of);
    } else if (!strcmp(cc, "g") || !strcmp(cc, "nle")) {
        sb_printf(&out, "(!(%s) && (%s == %s))", zf, sf, of);
    } else if (!strcmp(cc, "s")) {
        sb_puts(&out, sf);
    } else if (!strcmp(cc, "ns")) {
        sb_printf(&out, "!(%s)", sf);
    } else if (!strcmp(cc, "o")) {
        sb_puts(&out, of);
    } else if (!strcmp(cc, "no")) {
        sb_printf(&out, "!(%s)", of);
    } else if (!strcmp(cc, "p") || !strcmp(cc, "pe")) {
        sb_puts(&out, pf);
    } else if (!strcmp(cc, "np") || !strcmp(cc, "po")) {
        sb_printf(&out, "!(%s)", pf);
    } else {
        return -1;
    }
    return out.ok ? 0 : -1;
}

static uint8_t live_at(const uint8_t *live, int n, int idx)
{
    if (idx == -1) return LF_FLAG_ALL;
    if (idx == -2) return 0;
    if (idx < 0 || idx >= n) return LF_FLAG_ALL;
    return live[idx];
}

/* Live flags on one edge of ins[idx].  The -1 sentinel is not "unknown" for
 * an exact control edge: flag_use() already put the external target's needs
 * into tail_reads.  A call, a non-control fallthrough, an inexact boundary and
 * the unknown succ2 continuation stay ALL.  Do not merge this with
 * live_out_of(): that function relies on reads[] to carry the boundary, while
 * the fusion planner needs the two outgoing edges separately. */
static uint8_t edge_live_at(const LfFlagInsn *ins, const uint8_t *live, int n,
                            int idx, int edge)
{
    int succ = edge ? ins[idx].succ2 : ins[idx].succ1;
    if (succ == -1) {
        if (!edge && ins[idx].is_control && !ins[idx].is_call &&
            ins[idx].tail_exact)
            return ins[idx].tail_reads;
        return LF_FLAG_ALL;
    }
    if (succ == -2) return 0;
    return live_at(live, n, succ);
}

static uint8_t live_out_of(const LfFlagInsn *in, const uint8_t *live, int n,
                           int idx)
{
    uint8_t out = 0;
    /* -1 means the edge leaves the lifted extent.  With the exact flag
     * boundary model enabled, reads of the boundary are modelled explicitly
     * by flag_use(): a ret carries its exit_flags_needed, a tail jump carries
     * the target's entry_reads and the caller's needs.  Only a call or a
     * fallthrough whose continuation is unknown stays ALL.  The legacy
     * disabled mode kept conservative ALL everywhere; D2 is now always on
     * and that mode is removed. */
    if (in[idx].succ1 == -1) {
        if (in[idx].is_call || !in[idx].is_control || !in[idx].tail_exact)
            out |= LF_FLAG_ALL;
    } else if (in[idx].succ1 >= 0) {
        out |= live_at(live, n, in[idx].succ1);
    }
    /* succ2 == -1 is either an unknown fallthrough (conditional at the end)
     * or a barrier with no real second edge (int/int3); ALL is conservative. */
    if (in[idx].succ2 == -1) out |= LF_FLAG_ALL;
    else if (in[idx].succ2 >= 0) out |= live_at(live, n, in[idx].succ2);
    return out;
}

int lf_flag_producer_af(int kind)
{
    return kind == LF_FLAG_CMP || kind == LF_FLAG_ADD ||
           kind == LF_FLAG_SUB || kind == LF_FLAG_NEG ||
           kind == LF_FLAG_ADC || kind == LF_FLAG_SBB ||
           kind == LF_FLAG_INC || kind == LF_FLAG_DEC;
}

int lf_flag_cond_needs_fr(LfFlagKind kind, uint8_t cond_mask)
{
    if (cond_mask & (LF_FLAG_ZF | LF_FLAG_SF | LF_FLAG_PF))
        return 1;
    if ((cond_mask & LF_FLAG_CF) && kind == LF_FLAG_ADD)
        return 1;
    if (cond_mask & LF_FLAG_OF) {
        switch (kind) {
        case LF_FLAG_ADD:
        case LF_FLAG_ADC:
        case LF_FLAG_SUB:
        case LF_FLAG_CMP:
        case LF_FLAG_SBB:
            return 1;
        default:
            break;
        }
    }
    return 0;
}

static int live_update(const LfFlagInsn *ins, uint8_t *live, int n, int i)
{
    uint8_t o = live_out_of(ins, live, n, i);
    uint8_t in = (uint8_t)(ins[i].reads |
                           (o & (uint8_t)~ins[i].writes));
    if (in == live[i]) return 0;
    live[i] = in;
    return 1;
}

int lf_flag_plan_ex(const LfFlagInsn *ins, int n, LfFlagFuse *out, int max_out,
                    uint8_t *live_out)
{
    uint8_t *live;
    int *pred_head = NULL, *pred_next = NULL, *pred_src = NULL;
    int *queue = NULL;
    uint8_t *queued = NULL;
    int i, nf = 0, edges = 0, qhead = 0, qtail = 0, qcount = 0;
    int have_producer = 0, have_consumer = 0, have_flag_writer = 0;

    if (n <= 0 || !ins) return 0;
    if (!out || max_out <= 0) return 0;
    for (i = 0; i < n; i++) {
        if (ins[i].producer != LF_FLAG_NONE && ins[i].writes) have_producer = 1;
        if (ins[i].consumer != LF_CONS_NONE) have_consumer = 1;
        if (ins[i].writes) have_flag_writer = 1;
    }
    /* liveness is also read by flag_live_any() for atomics that are not
     * fusable producers (cmpxchg8b, xadd, cmpxchg).  Returning here left
     * live_out zeroed and made those atomics drop their flag writes.  The
     * same applies to the global boundary model: a function made only of
     * calls and rets still has to propagate its callers' exit_flags_needed
     * to those calls, so live_out is needed even with no local producer. */
    if (!have_producer && !have_flag_writer && !live_out) return 0;
    if (!have_consumer && !live_out) return 0;
    live = (uint8_t *)calloc((size_t)n + 1, 1);
    pred_head = (int *)malloc(sizeof(int) * (size_t)n);
    pred_next = (int *)malloc(sizeof(int) * (size_t)n * 2);
    pred_src = (int *)malloc(sizeof(int) * (size_t)n * 2);
    queue = (int *)malloc(sizeof(int) * (size_t)n);
    queued = (uint8_t *)calloc((size_t)n, 1);
    if (!live || !pred_head || !pred_next || !pred_src || !queue || !queued) {
        free(live);
        free(pred_head);
        free(pred_next);
        free(pred_src);
        free(queue);
        free(queued);
        return -1;
    }
    live[n] = LF_FLAG_ALL;
    for (i = 0; i < n; i++) pred_head[i] = -1;

    /* Predecessor lists make liveness converge in a bounded number of
     * updates.  A fixed pass count would silently disable fusion on large
     * functions with long loops. */
    for (i = 0; i < n; i++) {
        int s;
        s = ins[i].succ1;
        if (s >= 0 && s < n) {
            pred_src[edges] = i;
            pred_next[edges] = pred_head[s];
            pred_head[s] = edges++;
        }
        s = ins[i].succ2;
        if (s >= 0 && s < n) {
            pred_src[edges] = i;
            pred_next[edges] = pred_head[s];
            pred_head[s] = edges++;
        }
    }

    /* The reverse sweep resolves every forward edge in one pass; the worklist
     * then only revisits nodes whose live-in set actually grew. */
    for (i = n - 1; i >= 0; i--) live_update(ins, live, n, i);
    for (i = 0; i < n; i++) {
        queue[qtail] = i;
        qtail = (qtail + 1) % n;
        queued[i] = 1;
        qcount++;
    }
    while (qcount > 0) {
        int x = queue[qhead];
        int e;
        qhead = (qhead + 1) % n;
        queued[x] = 0;
        qcount--;
        for (e = pred_head[x]; e != -1; e = pred_next[e]) {
            int p = pred_src[e];
            if (live_update(ins, live, n, p) && !queued[p]) {
                queue[qtail] = p;
                qtail = (qtail + 1) % n;
                queued[p] = 1;
                qcount++;
            }
        }
    }
    free(pred_head);
    free(pred_next);
    free(pred_src);
    free(queue);
    free(queued);

    if (live_out)
        for (i = 0; i < n; i++) live_out[i] = live_out_of(ins, live, n, i);

    for (i = 0; i < n; i++) {
        int j;
        if (ins[i].producer == LF_FLAG_NONE || ins[i].writes == 0)
            continue;
        for (j = i + 1; j < n && j - i <= LF_FLAG_MAX_SPAN; j++) {
            int kind = ins[i].producer;
            int sz = ins[i].size;
            uint8_t need;
            uint8_t mw;
            int mat_taken = 0, mat_fall = 0;
            uint8_t mat_taken_mask = LF_FLAG_ALL;
            uint8_t mat_fall_mask = LF_FLAG_ALL;

            if (ins[j].is_label) break;
            if (ins[j].consumer != LF_CONS_NONE) {
                need = ins[j].cond_mask ? ins[j].cond_mask : ins[j].reads;
                if (need & (uint8_t)~ins[i].writes) break;
                mw = (uint8_t)(ins[i].writes |
                               (lf_flag_producer_af(kind) ? LF_FLAG_AF : 0));
                if (ins[j].consumer == LF_CONS_LAHF) {
                    /* Only the FP compare has a direct AH expression; an
                     * integer producer would need the original helper plus a
                     * LAHF, which is what the unfused path already emits. */
                    if (kind != LF_FLAG_FP_CMP) break;
                } else if (ins[j].consumer == LF_CONS_SETCC ||
                    ins[j].consumer == LF_CONS_CMOV) {
                    uint8_t after = live_out_of(ins, live, n, j);
                    if (after & mw) break;
                } else {
                    uint8_t lt, lf;
                    int nt, nff;
                    if (ins[j].succ1 == -2 || ins[j].succ2 == -2) break;
                    lt = edge_live_at(ins, live, n, j, 0);
                    lf = edge_live_at(ins, live, n, j, 1);
                    nt = (lt & mw) != 0;
                    nff = (lf & mw) != 0;
                    mat_taken_mask = lt;
                    mat_fall_mask = lf;
                    if (nt && nff) break;
                    /* New external-tail case: the target was previously
                     * always materialised.  When the exact tail proof says
                     * the target needs nothing but the fall-through does, the
                     * fused form adds a block without removing a helper.
                     * Keep the old, smaller shape only for that new case;
                     * internal targets keep their existing edge-aware form. */
                    if (!nt && nff && ins[j].succ1 == -1 &&
                        ins[j].tail_exact) break;
                    mat_taken = nt;
                    mat_fall = nff;
                }
                if (kind != LF_FLAG_FP_CMP &&
                    sz != 1 && sz != 2 && sz != 4) break;
                out[nf].producer = i;
                out[nf].consumer = j;
                out[nf].kind = kind;
                out[nf].size = sz;
                out[nf].mat_taken = mat_taken;
                out[nf].mat_fall = mat_fall;
                out[nf].mat_taken_mask = mat_taken_mask;
                out[nf].mat_fall_mask = mat_fall_mask;
                out[nf].cond_mask = need;
                nf++;
                if (nf >= max_out) {
                    free(live);
                    return nf;
                }
                break;
            }
            /* A consumer is never transparent.  Instructions between the
             * producer and the consumer may be moved into the fused block
             * only when they neither touch flags nor transfer control. */
            if (ins[j].reads || ins[j].writes || ins[j].is_control)
                break;
        }
    }
    free(live);
    return nf;
}

int lf_flag_plan(const LfFlagInsn *ins, int n, LfFlagFuse *out, int max_out)
{
    return lf_flag_plan_ex(ins, n, out, max_out, NULL);
}

static void plan_selftest_reset(LfFlagInsn *d, int n)
{
    int i;
    memset(d, 0, (size_t)n * sizeof *d);
    for (i = 0; i < n; i++) {
        d[i].succ1 = d[i].succ2 = -2;
        d[i].tail_reads = LF_FLAG_ALL;
    }
}

/* cmp eax, ebx followed by jae <external>; the fall-through is a flagless
 * move followed by a ret.  Every case changes only the edge model. */
static void plan_selftest_base(LfFlagInsn *d)
{
    plan_selftest_reset(d, 4);
    d[0].producer = LF_FLAG_CMP;
    d[0].writes = LF_FLAG_ALL;
    d[0].size = 4;
    d[0].succ1 = 1;

    d[1].consumer = LF_CONS_JCC;
    d[1].cond_mask = LF_FLAG_CF;
    d[1].reads = LF_FLAG_CF;
    d[1].is_control = 1;
    d[1].tail_exact = 1;
    d[1].tail_reads = 0;
    d[1].succ1 = -1;
    d[1].succ2 = 2;

    d[2].succ1 = 3;
}

int lf_flag_plan_selftest(void)
{
    LfFlagInsn d[4];
    LfFlagFuse f[4];
    uint8_t live[4];
    int nf;

    /* Exact external target needs no flags: the taken edge must not
     * materialise the producer, even though live_at(-1) would say ALL. */
    plan_selftest_base(d);
    nf = lf_flag_plan_ex(d, 4, f, 4, live);
    if (nf != 1) return 1;
    if (f[0].mat_taken != 0 || f[0].mat_fall != 0) return 2;
    if (live[1] != 0) return 3;

    /* The target reads ZF: materialise only on the taken edge. */
    plan_selftest_base(d);
    d[1].tail_reads = LF_FLAG_ZF;
    d[1].reads = LF_FLAG_CF | LF_FLAG_ZF;
    nf = lf_flag_plan_ex(d, 4, f, 4, live);
    if (nf != 1) return 4;
    if (f[0].mat_taken != 1 || f[0].mat_fall != 0) return 5;
    if (f[0].cond_mask != LF_FLAG_CF) return 6;
    if (f[0].mat_taken_mask != LF_FLAG_ZF) return 32;
    if (live[0] != (LF_FLAG_CF | LF_FLAG_ZF)) return 7;

    /* Only the fall-through reads ZF.  The unfused form is smaller than a
     * fused block with a fall-through helper, so the planner keeps it. */
    plan_selftest_base(d);
    d[2].reads = LF_FLAG_ZF;
    nf = lf_flag_plan_ex(d, 4, f, 4, live);
    if (nf != 0) return 8;

    /* An internal target keeps the existing edge-aware fall-through form:
     * only the newly exact external-tail case is kept unfused for size. */
    plan_selftest_base(d);
    d[1].succ1 = 2;
    d[1].succ2 = 3;
    d[2].succ1 = -2;
    d[3].reads = LF_FLAG_ZF;
    nf = lf_flag_plan_ex(d, 4, f, 4, live);
    if (nf != 1) return 30;
    if (f[0].mat_taken != 0 || f[0].mat_fall != 1) return 31;
    if (f[0].mat_fall_mask != LF_FLAG_ZF) return 33;

    /* Unknown fall-through: the missing second successor also stays
     * unfused, because only the fall-through would need materialisation. */
    plan_selftest_base(d);
    d[1].succ2 = -1;
    nf = lf_flag_plan_ex(d, 4, f, 4, live);
    if (nf != 0) return 10;

    /* Unknown external target: tail_reads is ALL, so materialise. */
    plan_selftest_base(d);
    d[1].tail_reads = LF_FLAG_ALL;
    nf = lf_flag_plan_ex(d, 4, f, 4, live);
    if (nf != 1) return 12;
    if (f[0].mat_taken != 1) return 13;

    /* A target-only flag must not be lost when it overlaps the condition.
     * cond_mask stays CF|ZF; tail_reads is not derived by subtraction. */
    plan_selftest_base(d);
    d[1].cond_mask = LF_FLAG_CF | LF_FLAG_ZF;
    d[1].reads = LF_FLAG_CF | LF_FLAG_ZF;
    d[1].tail_reads = LF_FLAG_ZF;
    nf = lf_flag_plan_ex(d, 4, f, 4, live);
    if (nf != 1) return 14;
    if (f[0].mat_taken != 1) return 15;
    if (f[0].cond_mask != (LF_FLAG_CF | LF_FLAG_ZF)) return 16;

    /* A logic producer does not write AF.  A target that reads AF must see
     * the earlier value, so the producer is not materialised for it. */
    plan_selftest_base(d);
    d[0].producer = LF_FLAG_AND;
    d[0].writes = LF_FLAG_CF | LF_FLAG_ZF | LF_FLAG_SF | LF_FLAG_OF |
                  LF_FLAG_PF;
    d[1].tail_reads = LF_FLAG_AF;
    d[1].reads = LF_FLAG_CF | LF_FLAG_AF;
    nf = lf_flag_plan_ex(d, 4, f, 4, live);
    if (nf != 1) return 17;
    if (f[0].mat_taken != 0) return 18;

    /* An internal target still uses ordinary liveness at that index. */
    plan_selftest_base(d);
    d[1].succ1 = 2;
    d[1].succ2 = 3;
    d[2].reads = LF_FLAG_ZF;
    d[2].succ1 = 3;
    nf = lf_flag_plan_ex(d, 4, f, 4, live);
    if (nf != 1) return 19;
    if (f[0].mat_taken != 1 || f[0].mat_fall != 0) return 20;

    /* If the exact boundary model is disabled, the old conservative ALL
     * behaviour is preserved. */
    plan_selftest_base(d);
    d[1].tail_exact = 0;
    nf = lf_flag_plan_ex(d, 4, f, 4, live);
    if (nf != 1) return 21;
    if (f[0].mat_taken != 1) return 22;

    return 0;
}
