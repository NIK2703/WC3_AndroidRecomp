/* liftcore.c - the compiled x86-32 -> C lifter (port of lift32_cpu.py).
 *
 * One C statement per guest instruction, emitted as text.  The Python lifter
 * remains the reference implementation; `lift.exe verify` diffs the two.
 */
#include <stdarg.h>
#include "liftcore.h"

#define NOPS(insn)  ((insn)->detail ? (insn)->detail->x86.op_count : 0)
#define OPS(insn)   ((insn)->detail->x86.operands)
#define OP(insn, i) (&(insn)->detail->x86.operands[i])

/* One decoded instruction plus its capstone detail.  The array stride is
 * sizeof(InsnCopy), not sizeof(cs_insn); flag planning indexes this array. */
typedef struct {
    cs_insn insn;
    cs_detail det;
} InsnCopy;

static int in_list(const char *s, const char *const *list, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) if (!strcmp(s, list[i])) return 1;
    return 0;
}

#define LST(s, ...) in_list((s), (const char *const[]){ __VA_ARGS__ }, \
                           sizeof((const char *const[]){ __VA_ARGS__ }) / sizeof(char *))

static char *strip_semi(const char *s)
{
    size_t n = strlen(s);
    while (n && s[n - 1] == ';') n--;
    return lf_afmt("%.*s", (int)n, s);
}

static int label_has(const uint32_t *labels, int n, uint64_t v)
{
    int lo = 0, hi = n - 1;
    uint32_t a = (uint32_t)v;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (labels[mid] == a) return 1;
        if (labels[mid] < a) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* Global call/ret flag summaries are defined after flag_use. */

/* A.27: only the eight 32-bit GPRs are safe as an identity `lea` base.
 * 16-bit bases are windows into the low half of a 32-bit field and would
 * truncate the result, so they must not be elided. */
static int reg_is_r32(unsigned r)
{
    return r == X86_REG_EAX || r == X86_REG_EBX ||
           r == X86_REG_ECX || r == X86_REG_EDX ||
           r == X86_REG_ESI || r == X86_REG_EDI ||
           r == X86_REG_ESP || r == X86_REG_EBP;
}

static int mov_is_identity(const cs_insn *insn)
{
    const cs_x86_op *d = OP(insn, 0), *s = OP(insn, 1);
    return d->type == X86_OP_REG && s->type == X86_OP_REG &&
           d->reg == s->reg && d->size == s->size;
}

static int lea_is_identity(const cs_insn *insn)
{
    const cs_x86_op *d = OP(insn, 0), *m = OP(insn, 1);
    return d->type == X86_OP_REG && m->type == X86_OP_MEM &&
           reg_is_r32((unsigned)d->reg) &&
           m->mem.base == d->reg && m->mem.index == 0 &&
           m->mem.disp == 0;
}

/* A.28: pick the value-only helper when no flag is observable. */
static const char *shift_fn(const char *m, int keep)
{
    if (!strcmp(m, "shr")) return keep ? "op_shr" : "op_shr_nf";
    if (!strcmp(m, "sar")) return keep ? "op_sar" : "op_sar_nf";
    if (!strcmp(m, "ror")) return keep ? "op_ror" : "op_ror_nf";
    if (!strcmp(m, "rol")) return keep ? "op_rol" : "op_rol_nf";
    return keep ? "op_shl" : "op_shl_nf";   /* shl/sal */
}

static char *target_expr(Lifter *L, cs_insn *insn, cs_x86_op *op)
{
    if (op->type == X86_OP_IMM) return lf_afmt("0x%08Xu", (uint32_t)op->imm);
    if (op->type == X86_OP_REG) return lf_src(L, insn, op);
    if (op->type == X86_OP_MEM) return lf_rd(L, insn, op);
    return lf_afmt("0u");
}

/* Stack effect of the x87 instructions the fast path models.  A reset means
 * lf_fpu emitted a load after an observation point, so the next block starts
 * with virtual top 0.  fdecstp/fincstp move the top without moving values. */
static int x87_is_reset(const char *m)
{
    return LST(m, "fldenv", "fnstenv", "fsave", "fnsave", "frstor",
                  "fprem", "fprem1", "frndint", "fscale", "fxtract",
                  "fptan", "fpatan", "fyl2x", "fyl2xp1", "fsincos",
                  "fninit", "finit");
}
static int x87_is_push(const char *m)
{
    return LST(m, "fld", "fild", "fbld", "fldz", "fld1", "fldpi",
                  "fldl2e", "fldl2t", "fldlg2", "fldln2");
}
static int x87_pop_count(const char *m)
{
    if (LST(m, "fstp", "fistp", "fisttp", "fbstp",
               "faddp", "fsubp", "fsubrp", "fmulp", "fdivp", "fdivrp",
               "fcomp", "fucomp", "ficomp",
               "fcomip", "fucomip", "fcompi", "fucompi"))
        return 1;
    if (LST(m, "fcompp", "fucompp"))
        return 2;
    return 0;
}
static int x87_top_after_value(const char *m, int top)
{
    int n;
    if (!strcmp(m, "fdecstp")) return (top - 1) & 7;
    if (!strcmp(m, "fincstp")) return (top + 1) & 7;
    if (x87_is_reset(m)) return 0;
    if (x87_is_push(m)) top = (top - 1) & 7;
    n = x87_pop_count(m);
    while (n--) top = (top + 1) & 7;
    return top;
}
static void x87_top_after(Lifter *L, const char *m)
{
    L->x87_top = x87_top_after_value(m, L->x87_top);
}
static int x87_is_barrier(const char *m)
{
    return LST(m, "fnclex", "finit", "fninit",
               "fldenv", "fnstenv", "fstenv", "fnsave", "fsave",
               "frstor", "fxsave", "fxrstor", "fprem", "fprem1",
               "frndint", "fscale", "fxtract", "fldcw", "fnstcw",
               "fxam", "fptan", "fpatan", "fyl2x", "fyl2xp1",
               "fsincos", "int", "int3", "into", "syscall", "sysenter",
               "hlt", "iret", "iretd");
}
static int x87_branch_internal(const cs_insn *in, const uint32_t *labels,
                                int nlabels)
{
    const char *m = in->mnemonic;
    if (m[0] != 'j' && strncmp(m, "loop", 4)) return 0;
    if (NOPS(in) < 1 || OP(in, 0)->type != X86_OP_IMM) return 0;
    return label_has(labels, nlabels, (uint32_t)OP(in, 0)->imm);
}
static int x87_lazy_check(Lifter *L, InsnCopy *ins, int n,
                          const uint32_t *labels, int nlabels)
{
    int top = 0;
    int i;
    for (i = 0; i < n; i++) {
        const char *m = ins[i].insn.mnemonic;
        if (x87_is_barrier(m)) return 0;
        if (lf_is_mmx(L, &ins[i].insn)) return 0;
        if (m[0] == 'f') top = x87_top_after_value(m, top);
        if (label_has(labels, nlabels, ins[i].insn.address) && top != 0)
            return 0;
        if (x87_branch_internal(&ins[i].insn, labels, nlabels) && top != 0)
            return 0;
        if (!strcmp(m, "call") || lf_is_mmx(L, &ins[i].insn))
            top = 0;
    }
    return 1;
}

static void flag_tmp_name(uint32_t addr, const char *kind, char *buf, size_t cap)
{
    snprintf(buf, cap, "_f%s_%08X", kind, addr);
}

static const char *flag_mask_suffix(int sz)
{
    if (sz == 1) return " & 0xFFu";
    if (sz == 2) return " & 0xFFFFu";
    return "";
}

static char *flag_mat_call(int kind, int sz, uint32_t addr)
{
    char fa[32], fb[32];
    flag_tmp_name(addr, "a", fa, sizeof fa);
    flag_tmp_name(addr, "b", fb, sizeof fb);
    switch (kind) {
    case LF_FLAG_CMP:  return lf_afmt("flags_sub(c, %s, %s, %d);", fa, fb, sz);
    case LF_FLAG_TEST: return lf_afmt("flags_logicz(c, %s & %s, %d);", fa, fb, sz);
    case LF_FLAG_ADD:  return lf_afmt("flags_add(c, %s, %s, %d);", fa, fb, sz);
    case LF_FLAG_SUB:  return lf_afmt("flags_sub(c, %s, %s, %d);", fa, fb, sz);
    case LF_FLAG_AND:  return lf_afmt("flags_logicz(c, %s & %s, %d);", fa, fb, sz);
    case LF_FLAG_OR:   return lf_afmt("flags_logicz(c, %s | %s, %d);", fa, fb, sz);
    case LF_FLAG_XOR:  return lf_afmt("flags_logicz(c, %s ^ %s, %d);", fa, fb, sz);
    case LF_FLAG_INC:  return lf_afmt("flags_incs(c, %s, %d);", fa, sz);
    case LF_FLAG_DEC:  return lf_afmt("flags_decs(c, %s, %d);", fa, sz);
    case LF_FLAG_NEG:  return lf_afmt("flags_sub(c, 0, %s, %d);", fb, sz);
    case LF_FLAG_ADC:  return lf_afmt("flags_adc(c, %s, %s, %d);", fa, fb, sz);
    case LF_FLAG_SBB:  return lf_afmt("flags_sbb(c, %s, %s, %d);", fa, fb, sz);
    default: return "";
    }
}

/* Open the fused region at the producer.  The closing brace is emitted at the
 * consumer, so instructions between the two (moves, pushes, x87 bookkeeping)
 * stay inside the same C block.  The flag helper is deliberately not called
 * here: it is either dropped or emitted on one edge by lf_fused_end(). */
static Emit lf_fused_start(Lifter *L, cs_insn *p, const LfFlagFuse *f)
{
    Emit e = lf_emit();
    char fa[32], fb[32], fr[32], ffull[32], fcin[32];
    const char *a = "0u", *b = NULL, *fr_expr = NULL;
    const char *mask = flag_mask_suffix(f->size);
    char *decl, *dw = NULL;

    flag_tmp_name(f->producer_addr, "a", fa, sizeof fa);
    flag_tmp_name(f->producer_addr, "b", fb, sizeof fb);
    flag_tmp_name(f->producer_addr, "r", fr, sizeof fr);
    flag_tmp_name(f->producer_addr, "full", ffull, sizeof ffull);
    flag_tmp_name(f->producer_addr, "cin", fcin, sizeof fcin);

    /* A.8: evaluate both FP operands once at the compare, then select or
     * materialize the flags at the consumer. */
    if (f->kind == LF_FLAG_FP_CMP) {
        int wide = !strcmp(p->mnemonic, "ucomisd") || !strcmp(p->mnemonic, "comisd");
        const char *ctype = wide ? "double" : "float";
        char *a = wide ? lf_sd(L, p, OP(p, 0)) : lf_ss(L, p, OP(p, 0));
        char *b = wide ? lf_sd(L, p, OP(p, 1)) : lf_ss(L, p, OP(p, 1));
        lf_push(&e, lf_afmt("{ %s %s = %s; %s %s = %s;",
                            ctype, fa, a, ctype, fb, b));
        return e;
    }

    switch (f->kind) {
    case LF_FLAG_CMP:
    case LF_FLAG_TEST:
    case LF_FLAG_ADD:
    case LF_FLAG_SUB:
    case LF_FLAG_AND:
    case LF_FLAG_OR:
    case LF_FLAG_XOR:
    case LF_FLAG_ADC:
    case LF_FLAG_SBB:
        if (NOPS(p) < 2) return e;
        a = lf_read_dst(L, p, OP(p, 0));
        b = lf_src(L, p, OP(p, 1));
        break;
    case LF_FLAG_INC:
    case LF_FLAG_DEC:
        if (NOPS(p) < 1) return e;
        a = lf_read_dst(L, p, OP(p, 0));
        break;
    case LF_FLAG_NEG:
        if (NOPS(p) < 1) return e;
        b = lf_read_dst(L, p, OP(p, 0));
        break;
    default:
        return e;
    }

    switch (f->kind) {
    case LF_FLAG_CMP:
    case LF_FLAG_SUB: fr_expr = lf_afmt("%s - %s", fa, fb); break;
    case LF_FLAG_TEST:
    case LF_FLAG_AND: fr_expr = lf_afmt("%s & %s", fa, fb); break;
    case LF_FLAG_OR:  fr_expr = lf_afmt("%s | %s", fa, fb); break;
    case LF_FLAG_XOR: fr_expr = lf_afmt("%s ^ %s", fa, fb); break;
    case LF_FLAG_ADD: fr_expr = lf_afmt("%s + %s", fa, fb); break;
    case LF_FLAG_ADC: fr_expr = lf_afmt("%s + %s + %s", fa, fb, fcin); break;
    case LF_FLAG_SBB: fr_expr = lf_afmt("%s - %s - %s", fa, fb, fcin); break;
    case LF_FLAG_INC: fr_expr = lf_afmt("%s + 1u", fa); break;
    case LF_FLAG_DEC: fr_expr = lf_afmt("%s - 1u", fa); break;
    case LF_FLAG_NEG: fr_expr = lf_afmt("0u - %s", fb); break;
    default: return e;
    }

    /* Decide which temporaries the emitted block actually references.  A
     * cmp/test result is not architecturally visible, and its direct
     * condition may be constant (for example CF-only after test).  Emitting
     * unused temporaries would fail -Werror=unused-variable; dropping them
     * is safe only when the operand expression has no side effects. */
    {
        int need_a = 0, need_b = 0, need_r = 0, need_full = 0, need_cin = 0;
        int has_dest = (f->kind != LF_FLAG_CMP && f->kind != LF_FLAG_TEST);
        int known_consumer = 0;

        if (has_dest) {
            need_r = 1;
            switch (f->kind) {
            case LF_FLAG_CMP:
            case LF_FLAG_TEST:
            case LF_FLAG_ADD:
            case LF_FLAG_SUB:
            case LF_FLAG_AND:
            case LF_FLAG_OR:
            case LF_FLAG_XOR:
                need_a = need_b = 1;
                break;
            case LF_FLAG_ADC:
            case LF_FLAG_SBB:
                need_a = need_b = need_full = need_cin = 1;
                break;
            case LF_FLAG_INC:
            case LF_FLAG_DEC:
                need_a = 1;
                break;
            case LF_FLAG_NEG:
                need_b = 1;
                break;
            default:
                break;
            }
        }

        /* Memory operands must still be evaluated: a guest read can fault. */
        if (a && (strstr(a, "rd8(") || strstr(a, "rd16(") ||
                  strstr(a, "rd32(") || strstr(a, "rd64(")))
            need_a = 1;
        if (b && (strstr(b, "rd8(") || strstr(b, "rd16(") ||
                  strstr(b, "rd32(") || strstr(b, "rd64(")))
            need_b = 1;

        /* The direct condition text is produced again by lf_fused_end(); ask
         * the same builder now so operand use matches exactly. */
        {
            const InsnCopy *ic = (const InsnCopy *)L->flag_ins;
            cs_insn *cons = (cs_insn *)&ic[f->consumer].insn;
            const char *cm = cons->mnemonic;
            const char *cc = NULL;
            char cond[4096];
            if (cm[0] == 'j' && strcmp(cm, "jmp"))
                cc = cm + 1;
            else if (!strncmp(cm, "set", 3))
                cc = cm + 3;
            else if (!strncmp(cm, "cmov", 4))
                cc = cm + 4;
            if (cc && lf_flag_cond(cond, sizeof cond, cc,
                                   (LfFlagKind)f->kind, f->size,
                                   fa, fb, fr, ffull, fcin) == 0) {
                known_consumer = 1;
                if (strstr(cond, fa)) need_a = 1;
                if (strstr(cond, fb)) need_b = 1;
                if (strstr(cond, fr)) need_r = 1;
                if (strstr(cond, ffull)) need_full = 1;
                if (strstr(cond, fcin)) need_cin = 1;
            } else if (!cc) {
                /* LAHF or something without a direct condition. */
                known_consumer = 1;
            }
        }

        /* Materialisation on one edge runs the original helper. */
        if (f->mat_taken || f->mat_fall) {
            char *mat = NULL;
            if (f->kind != LF_FLAG_FP_CMP)
                mat = flag_mat_call((LfFlagKind)f->kind, f->size,
                                    f->producer_addr);
            if (mat) {
                if (strstr(mat, fa)) need_a = 1;
                if (strstr(mat, fb)) need_b = 1;
                if (strstr(mat, fr)) need_r = 1;
                if (strstr(mat, ffull)) need_full = 1;
                if (strstr(mat, fcin)) need_cin = 1;
                /* lf_afmt() hands out arena memory; lf_arena_reset() owns it. */
            } else {
                need_a = need_b = need_r = 1;
            }
        }

        if (!known_consumer) {
            need_a = need_b = need_r = 1;
            if (f->kind == LF_FLAG_ADC || f->kind == LF_FLAG_SBB)
                need_full = need_cin = 1;
        }

        /* The result expression itself may reference the operand temps. */
        if (need_r) {
            switch (f->kind) {
            case LF_FLAG_CMP:
            case LF_FLAG_TEST:
            case LF_FLAG_ADD:
            case LF_FLAG_SUB:
            case LF_FLAG_AND:
            case LF_FLAG_OR:
            case LF_FLAG_XOR:
                need_a = need_b = 1;
                break;
            case LF_FLAG_ADC:
            case LF_FLAG_SBB:
                need_a = need_b = need_full = need_cin = 1;
                break;
            case LF_FLAG_INC:
            case LF_FLAG_DEC:
                need_a = 1;
                break;
            case LF_FLAG_NEG:
                need_b = 1;
                break;
            default:
                break;
            }
        }

        decl = NULL;
        if (need_a)
            decl = lf_afmt("uint32_t %s = (uint32_t)(%s)%s;", fa, a, mask);
        if (need_b && f->kind != LF_FLAG_INC && f->kind != LF_FLAG_DEC)
            decl = lf_afmt("%s uint32_t %s = (uint32_t)(%s)%s;",
                           decl ? decl : "", fb, b, mask);
        if ((f->kind == LF_FLAG_ADC || f->kind == LF_FLAG_SBB) &&
            (need_full || need_cin)) {
            const char *op = f->kind == LF_FLAG_ADC ? "+" : "-";
            decl = lf_afmt("%s uint32_t %s = c->cf & 1u; "
                           "uint64_t %s = (uint64_t)%s %s %s %s %s;",
                           decl ? decl : "", fcin, ffull,
                           fa, op, fb, op, fcin);
        }
        if (need_r)
            decl = lf_afmt("%s uint32_t %s = (%s)%s;",
                           decl ? decl : "", fr, fr_expr, mask);
    }

    if (f->kind != LF_FLAG_CMP && f->kind != LF_FLAG_TEST)
        dw = lf_dst_write(L, p, OP(p, 0), fr);

    lf_push(&e, lf_afmt("{ %s %s", decl ? decl : "", dw ? dw : ""));
    return e;
}

/* A.8: direct condition for UCOMISS/COMISS/UCOMISD/COMISD.  The unordered
 * case maps to CF=ZF=PF=1 exactly as sse_compare() stores it. */
static int lf_fp_cond(char *buf, size_t cap, const char *cc,
                      const char *fa, const char *fb)
{
    const char *expr;
    if (!strcmp(cc, "a") || !strcmp(cc, "nbe")) expr = "(%s > %s)";
    else if (!strcmp(cc, "ae") || !strcmp(cc, "nb") || !strcmp(cc, "nc"))
        expr = "(%s >= %s)";
    else if (!strcmp(cc, "b") || !strcmp(cc, "nae") || !strcmp(cc, "c"))
        expr = "(!(%s >= %s))";
    else if (!strcmp(cc, "be") || !strcmp(cc, "na")) expr = "(!(%s > %s))";
    else if (!strcmp(cc, "e") || !strcmp(cc, "z"))
        expr = "(!(%s < %s) && !(%s > %s))";
    else if (!strcmp(cc, "ne") || !strcmp(cc, "nz"))
        expr = "((%s < %s) || (%s > %s))";
    else if (!strcmp(cc, "p") || !strcmp(cc, "pe"))
        expr = "((%s != %s) || (%s != %s))";
    else if (!strcmp(cc, "np") || !strcmp(cc, "po"))
        expr = "((%s == %s) && (%s == %s))";
    else if (!strcmp(cc, "s")) expr = "0";
    else if (!strcmp(cc, "ns")) expr = "1";
    else if (!strcmp(cc, "o")) expr = "0";
    else if (!strcmp(cc, "no")) expr = "1";
    else return -1;
    if (!strcmp(cc, "e") || !strcmp(cc, "z") ||
        !strcmp(cc, "ne") || !strcmp(cc, "nz") ||
        !strcmp(cc, "p") || !strcmp(cc, "pe") ||
        !strcmp(cc, "np") || !strcmp(cc, "po"))
        snprintf(buf, cap, expr, fa, fb, fa, fb);
    else if (!strcmp(cc, "s") || !strcmp(cc, "ns") ||
             !strcmp(cc, "o") || !strcmp(cc, "no"))
        snprintf(buf, cap, "%s", expr);
    else
        snprintf(buf, cap, expr, fa, fb);
    return 0;
}

/* Close the fused region at the consumer.  jcc may run the original flag
 * helper on exactly one edge: the other edge has no live flag read before a
 * later flag write, so its state is unobservable. */
static Emit lf_fused_end(Lifter *L, const LfFlagFuse *f,
                         const uint32_t *labels, int nlabels)
{
    Emit e = lf_emit();
    const InsnCopy *ic = (const InsnCopy *)L->flag_ins;
    cs_insn *c = (cs_insn *)&ic[f->consumer].insn;
    const char *m = c->mnemonic;
    const char *cc;
    char fa[32], fb[32], fr[32], ffull[32], fcin[32], cond[4096];
    char *mat = NULL;

    flag_tmp_name(f->producer_addr, "a", fa, sizeof fa);
    flag_tmp_name(f->producer_addr, "b", fb, sizeof fb);
    flag_tmp_name(f->producer_addr, "r", fr, sizeof fr);
    flag_tmp_name(f->producer_addr, "full", ffull, sizeof ffull);
    flag_tmp_name(f->producer_addr, "cin", fcin, sizeof fcin);

    if (!strcmp(m, "lahf")) {
        if (f->kind == LF_FLAG_FP_CMP) {
            /* AH = SF<<7 | ZF<<6 | AF<<4 | PF<<2 | 1<<1 | CF.  SSE compare
             * clears SF/AF; unordered sets ZF/PF/CF together. */
            lf_push(&e, lf_afmt("sse_compare_lahf(c, %s, %s);", fa, fb));
            lf_push(&e, "}");
            return e;
        }
        return e;
    }

    if (m[0] == 'j' && strcmp(m, "jmp"))
        cc = m + 1;
    else if (!strncmp(m, "set", 3))
        cc = m + 3;
    else if (!strncmp(m, "cmov", 4))
        cc = m + 4;
    else
        return e;

    if (f->kind == LF_FLAG_FP_CMP) {
        if (lf_fp_cond(cond, sizeof cond, cc, fa, fb) != 0) {
            lf_push(&e, lf_todo((uint32_t)c->address, "fused fp condition"));
            lf_push(&e, "}");
            return e;
        }
    } else if (lf_flag_cond(cond, sizeof cond, cc, (LfFlagKind)f->kind, f->size,
                            fa, fb, fr, ffull, fcin) != 0) {
        lf_push(&e, lf_todo((uint32_t)c->address, "fused condition"));
        lf_push(&e, "}");
        return e;
    }

    if (f->kind == LF_FLAG_FP_CMP) {
        if (f->mat_taken || f->mat_fall)
            mat = lf_afmt("sse_compare(c, %s, %s);", fa, fb);
    } else if (f->mat_taken || f->mat_fall) {
        mat = flag_mat_call(f->kind, f->size, f->producer_addr);
    }

    if (!strncmp(m, "cmov", 4)) {
        char *src = lf_src(L, c, OP(c, 1));
        char *dw = lf_dst_write(L, c, OP(c, 0), src);
        lf_push(&e, lf_afmt("if (%s) { %s }", cond, dw));
    } else if (!strncmp(m, "set", 3)) {
        char *dw = lf_dst_write(L, c, OP(c, 0),
                                lf_afmt("((%s) ? 1u : 0u)", cond));
        lf_push(&e, dw);
    } else {
        uint32_t target = (uint32_t)OP(c, 0)->imm;
        int internal = label_has(labels, nlabels, target);
        if (internal) {
            if (f->mat_taken)
                lf_push(&e, lf_afmt("if (%s) { %s goto L_%08X; }",
                                    cond, mat, target));
            else
                lf_push(&e, lf_afmt("if (%s) goto L_%08X;", cond, target));
        } else {
            if (f->mat_taken)
                lf_push(&e, lf_afmt("if (%s) { %s dispatch_jmp(c, 0x%08Xu); return; }",
                                    cond, mat, target));
            else
                lf_push(&e, lf_afmt("if (%s) { dispatch_jmp(c, 0x%08Xu); return; }",
                                    cond, target));
        }
        if (f->mat_fall)
            lf_push(&e, mat);
    }
    lf_push(&e, "}");
    return e;
}

/* ------------------------------------------------------------- translate */
static int flag_dead(Lifter *L, const cs_insn *in, int *kind, int *sz);

/* True when at least one flag in mask is observable after this instruction.
 * The conservative answer outside the flagless path keeps the original
 * helpers in place. */
static int flag_live_any(Lifter *L, uint8_t mask)
{
    if (!L->flagless || !L->flag_live_out || L->flag_current_index < 0)
        return 1;
    return (L->flag_live_out[L->flag_current_index] & mask) != 0;
}

Emit lf_translate(Lifter *L, cs_insn *insn, const uint32_t *labels, int nlabels)
{
    Emit e = lf_emit();
    cs_x86_op *ops = OPS(insn);
    int nops = NOPS(insn);
    uint64_t ea = insn->address;
    uint64_t nxt = insn->address + insn->size;
    char mbuf[64];
    const char *m;
    int i;
    int locked = 0;

    strncpy(mbuf, insn->mnemonic, sizeof mbuf - 1);
    mbuf[sizeof mbuf - 1] = 0;
    m = mbuf;

    /* A.2: the planning pass tagged this instruction as either the producer
     * that opens a fused block or the consumer that closes it. */
    if (L->flag_role) {
        int fi = L->flag_current_index;
        if (fi >= 0) {
            if (L->flag_role[fi] == 2)
                return lf_fused_end(L, &L->flag_fuse[fi], labels, nlabels);
            if (L->flag_role[fi] == 1)
                return lf_fused_start(L, insn, &L->flag_fuse[fi]);
        }
    }

    /* A memory operand with size 0 cannot be an integer access: data decoded
     * as code.  Keep it as a TODO rather than keying a size table with 0. */
    if (LST(m, "rcl", "rcr", "rol", "ror", "shl", "sal", "shr", "sar")) {
        for (i = 0; i < nops; i++)
            if (ops[i].type == X86_OP_MEM && ops[i].size == 0) {
                lf_push(&e, lf_afmt("RECOMP_TODO(0x%08X, \"%s %s\");",
                                    (uint32_t)ea, m, insn->op_str));
                return e;
            }
    }

    /* capstone keeps the `lock` prefix in the mnemonic. */
    if (!strncmp(m, "lock ", 5)) { locked = 1; m += 5; }

    /* ---- lock-prefixed read-modify-write ---- */
    /* x86 `lock inc/dec/add [mem]` is one atomic operation.  The generic
     * arithmetic path is a plain load and store, so emit the atomic form
     * before it; otherwise two guest threads can lose an update. */
    if (locked && nops > 0 && OP(insn, 0)->type == X86_OP_MEM &&
        OP(insn, 0)->size == 4) {
        cs_x86_op *d = OP(insn, 0);
        char *a = lf_addr_expr(L, insn, d);
        if (!strcmp(m, "inc")) {
            if (flag_live_any(L, LF_FLAG_ZF | LF_FLAG_SF | LF_FLAG_OF |
                                  LF_FLAG_PF | LF_FLAG_AF))
                lf_push(&e, lf_afmt("{ uint32_t _old = atomic_xadd32(%s, 1u); "
                                    "flags_incs(c, _old, 4); }", a));
            else
                lf_push(&e, lf_afmt("atomic_xadd32(%s, 1u);", a));
            return e;
        }
        if (!strcmp(m, "dec")) {
            if (flag_live_any(L, LF_FLAG_ZF | LF_FLAG_SF | LF_FLAG_OF |
                                  LF_FLAG_PF | LF_FLAG_AF))
                lf_push(&e, lf_afmt("{ uint32_t _old = atomic_xadd32(%s, 0xFFFFFFFFu); "
                                    "flags_decs(c, _old, 4); }", a));
            else
                lf_push(&e, lf_afmt("atomic_xadd32(%s, 0xFFFFFFFFu);", a));
            return e;
        }
        if (!strcmp(m, "add") && nops > 1) {
            char *v = lf_src(L, insn, OP(insn, 1));
            if (flag_live_any(L, LF_FLAG_ALL))
                lf_push(&e, lf_afmt("{ uint32_t _old = atomic_xadd32(%s, %s); "
                                    "flags_add(c, _old, %s, 4); }", a, v, v));
            else
                lf_push(&e, lf_afmt("atomic_xadd32(%s, %s);", a, v));
            return e;
        }
        if (!strcmp(m, "sub") && nops > 1) {
            char *v = lf_src(L, insn, OP(insn, 1));
            if (flag_live_any(L, LF_FLAG_ALL))
                lf_push(&e, lf_afmt("{ uint32_t _old = atomic_sub32(%s, %s); "
                                    "flags_sub(c, _old, %s, 4); }", a, v, v));
            else
                lf_push(&e, lf_afmt("atomic_sub32(%s, %s);", a, v));
            return e;
        }
        if ((!strcmp(m, "and") || !strcmp(m, "or") || !strcmp(m, "xor")) && nops > 1) {
            char *v = lf_src(L, insn, OP(insn, 1));
            const char *op = !strcmp(m, "and") ? "&" : !strcmp(m, "or") ? "|" : "^";
            if (flag_live_any(L, LF_FLAG_CF | LF_FLAG_ZF | LF_FLAG_SF |
                                  LF_FLAG_OF | LF_FLAG_PF))
                lf_push(&e, lf_afmt("{ uint32_t _old = atomic_%s32(%s, %s); "
                                    "flags_logicz(c, _old %s %s, 4); }", m, a, v, op, v));
            else
                lf_push(&e, lf_afmt("atomic_%s32(%s, %s);", m, a, v));
            return e;
        }
        if ((!strcmp(m, "adc") || !strcmp(m, "sbb")) && nops > 1) {
            char *v = lf_src(L, insn, OP(insn, 1));
            if (flag_live_any(L, LF_FLAG_ALL))
                lf_push(&e, lf_afmt("{ uint32_t _old = %s(%s, %s + c->cf); "
                                    "flags_%s(c, _old, %s, 4); }",
                                    !strcmp(m, "adc") ? "atomic_xadd32" : "atomic_sub32",
                                    a, v, m, v));
            else
                lf_push(&e, lf_afmt("%s(%s, %s + c->cf);",
                                    !strcmp(m, "adc") ? "atomic_xadd32" : "atomic_sub32",
                                    a, v));
            return e;
        }
        if (!strcmp(m, "neg")) {
            if (flag_live_any(L, LF_FLAG_ALL))
                lf_push(&e, lf_afmt("{ uint32_t _old = atomic_neg32(%s); "
                                    "flags_sub(c, 0, _old, 4); }", a));
            else
                lf_push(&e, lf_afmt("atomic_neg32(%s);", a));
            return e;
        }
        if (!strcmp(m, "not")) {
            lf_push(&e, lf_afmt("atomic_xor32(%s, 0xFFFFFFFFu);", a));
            return e;
        }
    }

    /* ---- bit test ---- */
    if (LST(m, "bt", "bts", "btr", "btc")) {
        int opnum = !strcmp(m, "bt") ? 0 : !strcmp(m, "bts") ? 1 : !strcmp(m, "btr") ? 2 : 3;
        int keep_cf = flag_live_any(L, LF_FLAG_CF);
        char *idx = lf_src(L, insn, OP(insn, 1));
        if (OP(insn, 0)->type == X86_OP_MEM) {
            if (locked && OP(insn, 0)->size == 4 && strcmp(m, "bt") != 0) {
                const char *fn = !strcmp(m, "bts") ? "atomic_or32" :
                                 !strcmp(m, "btr") ? "atomic_and32" : "atomic_xor32";
                const char *arg = !strcmp(m, "btr") ? "(~_m)" : "_m";
                if (keep_cf)
                    lf_push(&e, lf_afmt("{ uint32_t _a = (%s) + 4u * (uint32_t)((int32_t)(%s) >> 5); "
                                        "uint32_t _m = 1u << ((int32_t)(%s) & 31); "
                                        "uint32_t _old = %s(_a, %s); "
                                        "c->cf = (_old >> ((int32_t)(%s) & 31)) & 1; }",
                                        lf_addr_expr(L, insn, OP(insn, 0)), idx, idx, fn, arg, idx));
                else
                    lf_push(&e, lf_afmt("{ uint32_t _a = (%s) + 4u * (uint32_t)((int32_t)(%s) >> 5); "
                                        "uint32_t _m = 1u << ((int32_t)(%s) & 31); %s(_a, %s); }",
                                        lf_addr_expr(L, insn, OP(insn, 0)), idx, idx, fn, arg));
            } else if (keep_cf) {
                lf_push(&e, lf_afmt("bit_string_op(c, %s, (int32_t)(%s), %d);",
                                    lf_addr_expr(L, insn, OP(insn, 0)), idx, opnum));
            } else {
                lf_push(&e, lf_afmt("bit_string_op_nf(%s, (int32_t)(%s), %d);",
                                    lf_addr_expr(L, insn, OP(insn, 0)), idx, opnum));
            }
        } else if (keep_cf) {
            lf_push(&e, lf_dst_write(L, insn, OP(insn, 0),
                    lf_afmt("op_bittest(c, %s, %s, %d, %d)",
                            lf_read_dst(L, insn, OP(insn, 0)), idx,
                            (int)OP(insn, 0)->size, opnum)));
        } else if (opnum != 0) {
            lf_push(&e, lf_dst_write(L, insn, OP(insn, 0),
                    lf_afmt("op_bittest_nf(%s, %s, %d, %d)",
                            lf_read_dst(L, insn, OP(insn, 0)), idx,
                            (int)OP(insn, 0)->size, opnum)));
        }
        return e;
    }

    if (m[0] == 'f') return lf_fpu(L, insn);
    if (lf_is_mmx(L, insn)) return lf_mmx(L, insn);
    if (lf_is_sse(L, insn)) return lf_sse(L, insn);

    if (!strncmp(m, "prefetch", 8)) {
        lf_push(&e, "/* prefetch: hint only */");
        return e;
    }

    if (!strncmp(m, "cmov", 4)) {
        char jm[16];
        const char *cond;
        snprintf(jm, sizeof jm, "j%s", m + 4);
        cond = lf_cond(jm);
        if (cond) {
            lf_push(&e, lf_afmt("if (%s) { %s }", cond,
                                lf_dst_write(L, insn, OP(insn, 0),
                                             lf_src(L, insn, OP(insn, 1)))));
            return e;
        }
    }
    if (!strcmp(m, "sahf")) { lf_push(&e, "do_sahf(c, R8H(c->eax));"); return e; }
    if (!strcmp(m, "lahf")) {
        lf_push(&e, "SET8H(c->eax, (c->sf<<7)|(c->zf<<6)|(c->af<<4)|(c->pf<<2)|2|c->cf);");
        return e;
    }

    /* ---- arithmetic / logic ---- */
    if (!strcmp(m, "mov")) {
        if (mov_is_identity(insn)) {
            lf_push(&e, lf_afmt("/* %08X: %s %s: identity */",
                                (uint32_t)ea, insn->mnemonic, insn->op_str));
            return e;
        }
        lf_push(&e, lf_dst_write(L, insn, OP(insn, 0), lf_src(L, insn, OP(insn, 1))));
        return e;
    }
    if (!strcmp(m, "lea")) {
        if (lea_is_identity(insn)) {
            lf_push(&e, lf_afmt("/* %08X: %s %s: identity */",
                                (uint32_t)ea, insn->mnemonic, insn->op_str));
            return e;
        }
        lf_push(&e, lf_afmt("c->%s = (uint32_t)(%s);",
                            lf_reg_name(L, (unsigned)OP(insn, 0)->reg),
                            lf_addr_expr(L, insn, OP(insn, 1))));
        return e;
    }
    if (LST(m, "add", "sub", "and", "or", "xor", "adc", "sbb")) {
        cs_x86_op *d = OP(insn, 0), *s = OP(insn, 1);
        int sz = d->size;
        char *a = lf_read_dst(L, insn, d), *b = lf_src(L, insn, s);
        char *r;
        int kind, ksz = 0;
        if (flag_dead(L, insn, &kind, &ksz)) {
            if (!strcmp(m, "add")) r = lf_afmt("add32(%s, %s, %d)", a, b, sz);
            else if (!strcmp(m, "sub")) r = lf_afmt("sub32(%s, %s, %d)", a, b, sz);
            else if (!strcmp(m, "adc")) r = lf_afmt("adc32(c, %s, %s, %d)", a, b, sz);
            else if (!strcmp(m, "sbb")) r = lf_afmt("sbb32(c, %s, %s, %d)", a, b, sz);
            else if (!strcmp(m, "and")) r = lf_afmt("and32(%s, %s, %d)", a, b, sz);
            else if (!strcmp(m, "or"))  r = lf_afmt("or32(%s, %s, %d)", a, b, sz);
            else                        r = lf_afmt("xor32(%s, %s, %d)", a, b, sz);
            lf_push(&e, lf_dst_write(L, insn, d, r));
            return e;
        }
        if (!strcmp(m, "add")) r = lf_afmt("flags_add(c, %s, %s, %d)", a, b, sz);
        else if (!strcmp(m, "sub")) r = lf_afmt("flags_sub(c, %s, %s, %d)", a, b, sz);
        else if (!strcmp(m, "adc")) r = lf_afmt("flags_adc(c, %s, %s, %d)", a, b, sz);
        else if (!strcmp(m, "sbb")) r = lf_afmt("flags_sbb(c, %s, %s, %d)", a, b, sz);
        else if (!strcmp(m, "and")) r = lf_afmt("flags_logicz(c, %s & %s, %d)", a, b, sz);
        else if (!strcmp(m, "or"))  r = lf_afmt("flags_logicz(c, %s | %s, %d)", a, b, sz);
        else                        r = lf_afmt("flags_logicz(c, %s ^ %s, %d)", a, b, sz);
        lf_push(&e, lf_dst_write(L, insn, d, r));
        return e;
    }
    if (!strcmp(m, "cmp")) {
        int kind, ksz = 0;
        if (flag_dead(L, insn, &kind, &ksz)) return e;
        lf_push(&e, lf_afmt("flags_sub(c, %s, %s, %d);",
                            lf_read_dst(L, insn, OP(insn, 0)),
                            lf_src(L, insn, OP(insn, 1)), OP(insn, 0)->size));
        return e;
    }
    if (!strcmp(m, "test")) {
        int kind, ksz = 0;
        if (flag_dead(L, insn, &kind, &ksz)) return e;
        lf_push(&e, lf_afmt("flags_logicz(c, %s & %s, %d);",
                            lf_read_dst(L, insn, OP(insn, 0)),
                            lf_src(L, insn, OP(insn, 1)), OP(insn, 0)->size));
        return e;
    }
    if (!strcmp(m, "inc")) {
        cs_x86_op *d = OP(insn, 0);
        int kind, ksz = 0;
        if (flag_dead(L, insn, &kind, &ksz)) {
            lf_push(&e, lf_dst_write(L, insn, d,
                    lf_afmt("inc32(%s, %d)", lf_read_dst(L, insn, d), d->size)));
            return e;
        }
        lf_push(&e, lf_dst_write(L, insn, d,
                lf_afmt("flags_incs(c, %s, %d)", lf_read_dst(L, insn, d), d->size)));
        return e;
    }
    if (!strcmp(m, "dec")) {
        cs_x86_op *d = OP(insn, 0);
        int kind, ksz = 0;
        if (flag_dead(L, insn, &kind, &ksz)) {
            lf_push(&e, lf_dst_write(L, insn, d,
                    lf_afmt("dec32(%s, %d)", lf_read_dst(L, insn, d), d->size)));
            return e;
        }
        lf_push(&e, lf_dst_write(L, insn, d,
                lf_afmt("flags_decs(c, %s, %d)", lf_read_dst(L, insn, d), d->size)));
        return e;
    }
    if (!strcmp(m, "neg")) {
        cs_x86_op *d = OP(insn, 0);
        int kind, ksz = 0;
        if (flag_dead(L, insn, &kind, &ksz)) {
            lf_push(&e, lf_dst_write(L, insn, d,
                    lf_afmt("neg32(%s, %d)", lf_read_dst(L, insn, d), d->size)));
            return e;
        }
        lf_push(&e, lf_dst_write(L, insn, d,
                lf_afmt("flags_sub(c, 0, %s, %d)", lf_read_dst(L, insn, d), d->size)));
        return e;
    }
    if (!strcmp(m, "not")) {
        cs_x86_op *d = OP(insn, 0);
        lf_push(&e, lf_dst_write(L, insn, d, lf_afmt("(~(%s))", lf_read_dst(L, insn, d))));
        return e;
    }
    if (LST(m, "shl", "sal", "shr", "sar")) {
        cs_x86_op *d = OP(insn, 0);
        char *cnt = nops > 1 ? lf_src(L, insn, OP(insn, 1)) : (char *)"1";
        int keep = flag_live_any(L, LF_FLAG_CF | LF_FLAG_ZF |
                                    LF_FLAG_SF | LF_FLAG_PF);
        const char *fn = shift_fn(m, keep);
        char *dw = keep
            ? lf_afmt("%s(c, %s, %s, %d)", fn, lf_read_dst(L, insn, d), cnt, d->size)
            : lf_afmt("%s(%s, %s, %d)", fn, lf_read_dst(L, insn, d), cnt, d->size);
        lf_push(&e, lf_dst_write(L, insn, d, dw));
        return e;
    }
    if (LST(m, "rol", "ror")) {
        cs_x86_op *d = OP(insn, 0);
        char *cnt = nops > 1 ? lf_src(L, insn, OP(insn, 1)) : (char *)"1";
        int keep = flag_live_any(L, LF_FLAG_CF | LF_FLAG_OF);
        const char *fn = shift_fn(m, keep);
        char *dw = keep
            ? lf_afmt("%s(c, %s, %s, %d)", fn, lf_read_dst(L, insn, d), cnt, d->size)
            : lf_afmt("%s(%s, %s, %d)", fn, lf_read_dst(L, insn, d), cnt, d->size);
        lf_push(&e, lf_dst_write(L, insn, d, dw));
        return e;
    }
    if (!strcmp(m, "bswap")) {
        cs_x86_op *d = OP(insn, 0);
        if (d->size == 4) {
            lf_push(&e, lf_dst_write(L, insn, d, lf_afmt("op_bswap32(%s)", lf_read_dst(L, insn, d))));
            return e;
        }
    }
    if (LST(m, "bsf", "bsr")) {
        cs_x86_op *d = OP(insn, 0);
        int keep_zf = flag_live_any(L, LF_FLAG_ZF);
        const char *fn = !strcmp(m, "bsf") ? "op_bsf" : "op_bsr";
        lf_push(&e, lf_dst_write(L, insn, d,
                keep_zf ? lf_afmt("%s(c, %s, %s, %d)", fn, lf_read_dst(L, insn, d),
                                  lf_src(L, insn, OP(insn, 1)), d->size)
                        : lf_afmt("%s_nf(%s, %s, %d)", fn, lf_read_dst(L, insn, d),
                                  lf_src(L, insn, OP(insn, 1)), d->size)));
        return e;
    }
    if (LST(m, "rcl", "rcr")) {
        cs_x86_op *d = OP(insn, 0);
        char *cnt = nops > 1 ? lf_src(L, insn, OP(insn, 1)) : (char *)"1";
        int keep_flags = flag_live_any(L, LF_FLAG_CF | LF_FLAG_OF);
        const char *fn = !strcmp(m, "rcl") ? "op_rcl" : "op_rcr";
        lf_push(&e, lf_dst_write(L, insn, d,
                keep_flags ? lf_afmt("%s(c, %s, %s, %d)", fn, lf_read_dst(L, insn, d), cnt, d->size)
                           : lf_afmt("%s_nf(c, %s, %s, %d)", fn, lf_read_dst(L, insn, d), cnt, d->size)));
        return e;
    }
    if (LST(m, "shld", "shrd")) {
        cs_x86_op *d = OP(insn, 0);
        char *cnt = nops > 2 ? lf_src(L, insn, OP(insn, 2)) : (char *)"R8L(c->ecx)";
        int keep_flags = flag_live_any(L, LF_FLAG_CF | LF_FLAG_ZF | LF_FLAG_SF | LF_FLAG_PF);
        const char *fn = !strcmp(m, "shld") ? "op_shld" : "op_shrd";
        lf_push(&e, lf_dst_write(L, insn, d,
                keep_flags ? lf_afmt("%s(c, %s, %s, %s, %d)", fn, lf_read_dst(L, insn, d),
                                     lf_src(L, insn, OP(insn, 1)), cnt, d->size)
                           : lf_afmt("%s_nf(%s, %s, %s, %d)", fn, lf_read_dst(L, insn, d),
                                     lf_src(L, insn, OP(insn, 1)), cnt, d->size)));
        return e;
    }
    if (LST(m, "lds", "les", "lfs", "lgs", "lss")) {
        cs_x86_op *d = OP(insn, 0), *s = OP(insn, 1);
        int sz = d->size;
        char *dw = lf_dst_write(L, insn, d, lf_afmt("rd%d(_a)", sz * 8));
        lf_push(&e, lf_afmt("{ uint32_t _a = %s; %s c->%s = rd16(_a + %d); }",
                            lf_addr_expr(L, insn, s), dw, m + 1, sz));
        return e;
    }
    if (!strcmp(m, "retf")) {
        int n = (nops && OP(insn, 0)->type == X86_OP_IMM) ? (int)OP(insn, 0)->imm : 0;
        lf_push(&e, lf_afmt("/* retf %d: far frame unwound by the caller */ return;", n));
        return e;
    }
    if (!strcmp(m, "movzx")) {
        lf_push(&e, lf_dst_write(L, insn, OP(insn, 0),
                                 lf_afmt("(%s)", lf_src(L, insn, OP(insn, 1)))));
        return e;
    }
    if (!strcmp(m, "movsx")) {
        int ssz = OP(insn, 1)->size;
        const char *cast = ssz == 1 ? "int8_t" : "int16_t";
        lf_push(&e, lf_dst_write(L, insn, OP(insn, 0),
                lf_afmt("(uint32_t)(int32_t)(%s)(%s)", cast, lf_src(L, insn, OP(insn, 1)))));
        return e;
    }
    if (!strcmp(m, "cdq")) {
        lf_push(&e, "c->edx = (c->eax & 0x80000000u) ? 0xFFFFFFFFu : 0u;");
        return e;
    }
    if (!strcmp(m, "cwde")) {
        lf_push(&e, "c->eax = (uint32_t)(int32_t)(int16_t)R16(c->eax);");
        return e;
    }
    if (!strcmp(m, "xchg")) {
        cs_x86_op *op0 = OP(insn, 0), *op1 = OP(insn, 1);
        cs_x86_op *mem = NULL, *reg = NULL;
        if (op0->type == X86_OP_MEM && op0->size == 4) {
            mem = op0; reg = op1;
        } else if (op1->type == X86_OP_MEM && op1->size == 4) {
            mem = op1; reg = op0;
        }
        if (mem != NULL && reg != NULL && reg->type != X86_OP_MEM) {
            /* x86 xchg with a memory operand is implicitly locked, even
             * without a LOCK prefix.  A plain read/write pair is not: a
             * waiter can read "held", be preempted, the holder writes
             * "free", and the waiter's stale store writes "held" back.
             * The lock is then held with no owner and no release wakes it. */
            char *a = lf_addr_expr(L, insn, mem);
            lf_push(&e, lf_afmt("{ uint32_t _v = %s;", lf_src(L, insn, reg)));
            lf_push(&e, lf_afmt("  uint32_t _old = atomic_xchg32(%s, _v);", a));
            lf_push(&e, lf_afmt("  %s }", lf_dst_write(L, insn, reg, "_old")));
            return e;
        }
        char *t = lf_read_dst(L, insn, OP(insn, 0));
        char *w1 = strip_semi(lf_dst_write(L, insn, OP(insn, 0), lf_src(L, insn, OP(insn, 1))));
        char *w2 = strip_semi(lf_dst_write(L, insn, OP(insn, 1), "_t"));
        lf_push(&e, lf_afmt("{ uint32_t _t = %s; %s; %s; }", t, w1, w2));
        return e;
    }

    if (LST(m, "xadd", "cmpxchg") && OP(insn, 0)->size == 4) {
        cs_x86_op *d = OP(insn, 0), *s = OP(insn, 1);
        if (d->type == X86_OP_MEM) {
            char *a = lf_addr_expr(L, insn, d);
            if (!strcmp(m, "xadd")) {
                lf_push(&e, lf_afmt("{ uint32_t _v = %s;", lf_src(L, insn, s)));
                lf_push(&e, lf_afmt("  uint32_t _old = atomic_xadd32(%s, _v);", a));
                lf_push(&e, lf_afmt("  %s", lf_dst_write(L, insn, s, "_old")));
                if (flag_live_any(L, LF_FLAG_ALL))
                    lf_push(&e, "  flags_add(c, _old, _v, 4); }");
                else
                    lf_push(&e, "}");
                return e;
            }
            lf_push(&e, lf_afmt("{ uint32_t _old = atomic_cmpxchg32(%s, c->eax, %s);",
                                a, lf_src(L, insn, s)));
            if (flag_live_any(L, LF_FLAG_CF | LF_FLAG_SF | LF_FLAG_OF |
                                  LF_FLAG_AF | LF_FLAG_PF))
                lf_push(&e, "  flags_sub(c, c->eax, _old, 4);");
            else if (flag_live_any(L, LF_FLAG_ZF))
                lf_push(&e, "  c->zf = (_old == c->eax);");
            lf_push(&e, "  if (_old != c->eax) c->eax = _old; }");
            return e;
        }
    }
    if (!strcmp(m, "cmpxchg8b") && nops && OP(insn, 0)->type == X86_OP_MEM) {
        char *a = lf_addr_expr(L, insn, OP(insn, 0));
        lf_push(&e, "{ uint64_t _exp = ((uint64_t)c->edx << 32) | c->eax;");
        lf_push(&e, "  uint64_t _des = ((uint64_t)c->ecx << 32) | c->ebx;");
        lf_push(&e, lf_afmt("  uint64_t _old = atomic_cmpxchg64(%s, _exp, _des);", a));
        if (flag_live_any(L, LF_FLAG_ZF)) {
            lf_push(&e, "  if (_old == _exp) c->zf = 1;");
            lf_push(&e, "  else { c->zf = 0; c->eax = (uint32_t)_old;");
            lf_push(&e, "         c->edx = (uint32_t)(_old >> 32); } }");
        } else {
            lf_push(&e, "  if (_old != _exp) { c->eax = (uint32_t)_old;");
            lf_push(&e, "         c->edx = (uint32_t)(_old >> 32); } }");
        }
        return e;
    }
    if (!strcmp(m, "cpuid")) { lf_push(&e, "do_cpuid(c);"); return e; }
    if (!strcmp(m, "rdtsc")) { lf_push(&e, "do_rdtsc(c);"); return e; }

    if (!strncmp(m, "set", 3) && strcmp(m, "setssbsy")) {
        char jm[16];
        const char *cond;
        snprintf(jm, sizeof jm, "j%s", m + 3);
        cond = lf_cond(jm);
        if (cond) {
            lf_push(&e, lf_dst_write(L, insn, OP(insn, 0), lf_afmt("((%s) ? 1 : 0)", cond)));
            return e;
        }
    }
    if (!strcmp(m, "pushfd")) { lf_push(&e, "push32(c, eflags_pack(c));"); return e; }
    if (!strcmp(m, "popfd"))  { lf_push(&e, "eflags_unpack(c, pop32(c));"); return e; }
    if (!strcmp(m, "pushf"))  { lf_push(&e, "push32(c, eflags_pack(c) & 0xFFFFu);"); return e; }
    if (!strcmp(m, "popf"))   { lf_push(&e, "eflags_unpack(c, pop32(c) & 0xFFFFu);"); return e; }
    if (!strcmp(m, "cbw")) { lf_push(&e, "SET16(c->eax, (uint16_t)(int16_t)(int8_t)R8L(c->eax));"); return e; }
    if (!strcmp(m, "cwd")) { lf_push(&e, "SET16(c->edx, (R16(c->eax) & 0x8000u) ? 0xFFFFu : 0u);"); return e; }
    if (LST(m, "wait", "fwait", "nop", "int3", "cld", "fnop", "hint_nop")) {
        lf_push(&e, lf_afmt("/* %s */", m));
        return e;
    }
    if (!strcmp(m, "clc")) { lf_push(&e, "c->cf = 0;"); return e; }
    if (!strcmp(m, "stc")) { lf_push(&e, "c->cf = 1;"); return e; }
    if (!strcmp(m, "cmc")) { lf_push(&e, "c->cf ^= 1;"); return e; }
    if (LST(m, "cli", "sti")) { lf_push(&e, lf_afmt("/* %s: IRQs are host-managed */", m)); return e; }
    if (!strcmp(m, "std")) { lf_push(&e, "/* std (DF=1 unsupported; string ops assume forward) */"); return e; }
    if (!strcmp(m, "enter")) {
        long n = nops ? (long)OP(insn, 0)->imm : 0;
        lf_push(&e, lf_afmt("push32(c, c->ebp); c->ebp = c->esp; c->esp -= %ld;", n));
        return e;
    }
    if (LST(m, "loop", "loope", "loopz", "loopne", "loopnz")) {
        cs_x86_op *t = OP(insn, 0);
        const char *extra = "";
        if (!strcmp(m, "loope") || !strcmp(m, "loopz")) extra = " && c->zf";
        else if (!strcmp(m, "loopne") || !strcmp(m, "loopnz")) extra = " && !c->zf";
        if (t->type == X86_OP_IMM && label_has(labels, nlabels, (uint64_t)t->imm))
            lf_push(&e, lf_afmt("c->ecx--; if (c->ecx%s) goto L_%08X;", extra, (uint32_t)t->imm));
        else
            lf_push(&e, lf_afmt("c->ecx--; if (c->ecx%s) { dispatch_jmp(c, 0x%08Xu); return; }",
                                extra, (uint32_t)t->imm));
        return e;
    }

    /* ---- multiply / divide ---- */
    if (!strcmp(m, "mul")) {
        cs_x86_op *s = OP(insn, 0);
        int sz = s->size;
        char *v = lf_src(L, insn, s);
        int keep = flag_live_any(L, LF_FLAG_CF | LF_FLAG_OF);
        if (sz == 4)
            lf_push(&e, lf_afmt("{ uint64_t _p=(uint64_t)c->eax*(uint32_t)(%s); c->eax=(uint32_t)_p; c->edx=(uint32_t)(_p>>32);%s }", v,
                                keep ? " c->cf=c->of=(c->edx!=0);" : ""));
        else if (sz == 2)
            lf_push(&e, lf_afmt("{ uint32_t _p=(uint32_t)R16(c->eax)*(uint16_t)(%s); SET16(c->eax,_p); SET16(c->edx,_p>>16);%s }", v,
                                keep ? " c->cf=c->of=((_p>>16)!=0);" : ""));
        else
            lf_push(&e, lf_afmt("{ uint16_t _p=(uint16_t)R8L(c->eax)*(uint8_t)(%s); SET16(c->eax,_p);%s }", v,
                                keep ? " c->cf=c->of=((_p>>8)!=0);" : ""));
        return e;
    }
    if (!strcmp(m, "imul")) {
        if (nops == 1) {
            cs_x86_op *s = OP(insn, 0);
            int sz = s->size;
            char *v = lf_src(L, insn, s);
            int keep = flag_live_any(L, LF_FLAG_CF | LF_FLAG_OF);
            if (sz == 4)
                lf_push(&e, lf_afmt("{ int64_t _p=(int64_t)(int32_t)c->eax*(int32_t)(%s); c->eax=(uint32_t)_p; c->edx=(uint32_t)((uint64_t)_p>>32);%s }", v,
                                    keep ? " c->cf=c->of=((int32_t)_p!=_p);" : ""));
            else if (sz == 2)
                lf_push(&e, lf_afmt("{ int32_t _p=(int32_t)(int16_t)R16(c->eax)*(int16_t)(%s); SET16(c->eax,_p); SET16(c->edx,_p>>16);%s }", v,
                                    keep ? " c->cf=c->of=((int16_t)_p!=_p);" : ""));
            else
                lf_push(&e, lf_afmt("{ int16_t _p=(int16_t)(int8_t)R8L(c->eax)*(int8_t)(%s); SET16(c->eax,_p);%s }", v,
                                    keep ? " c->cf=c->of=((int8_t)_p!=_p);" : ""));
            return e;
        }
        {
            cs_x86_op *d = OP(insn, 0);
            int sz = d->size;
            const char *cast = sz == 1 ? "int8_t" : sz == 2 ? "int16_t" : "int32_t";
            char *a, *b, *dw;
            char fbuf[80];
            if (nops == 2) { a = lf_read_dst(L, insn, d); b = lf_src(L, insn, OP(insn, 1)); }
            else           { a = lf_src(L, insn, OP(insn, 1)); b = lf_src(L, insn, OP(insn, 2)); }
            dw = strip_semi(lf_dst_write(L, insn, d, "(uint32_t)_p"));
            snprintf(fbuf, sizeof fbuf, "c->cf=c->of=((%s)_p!=_p);", cast);
            lf_push(&e, lf_afmt("{ int64_t _p=(int64_t)(%s)(%s)*(%s)(%s); %s;%s }",
                                cast, a, cast, b, dw,
                                flag_live_any(L, LF_FLAG_CF | LF_FLAG_OF) ? fbuf : ""));
            return e;
        }
    }
    if (LST(m, "div", "idiv")) {
        cs_x86_op *s = OP(insn, 0);
        int sz = s->size;
        int sg = !strcmp(m, "idiv");
        char *v = lf_src(L, insn, s);
        if (sz == 4) {
            if (sg) lf_push(&e, lf_afmt("{ int64_t _n=(int64_t)(((uint64_t)c->edx<<32)|c->eax); int32_t _d=(int32_t)(%s); c->eax=(uint32_t)(_n/_d); c->edx=(uint32_t)(_n%%_d); }", v));
            else    lf_push(&e, lf_afmt("{ uint64_t _n=((uint64_t)c->edx<<32)|c->eax; uint32_t _d=(uint32_t)(%s); c->eax=(uint32_t)(_n/_d); c->edx=(uint32_t)(_n%%_d); }", v));
        } else if (sz == 2) {
            if (sg) lf_push(&e, lf_afmt("{ int32_t _n=(int32_t)(((uint32_t)R16(c->edx)<<16)|R16(c->eax)); int16_t _d=(int16_t)(%s); SET16(c->eax,(uint16_t)(int16_t)(_n/_d)); SET16(c->edx,(uint16_t)(int16_t)(_n%%_d)); }", v));
            else    lf_push(&e, lf_afmt("{ uint32_t _n=((uint32_t)R16(c->edx)<<16)|R16(c->eax); uint16_t _d=(uint16_t)(%s); SET16(c->eax,_n/_d); SET16(c->edx,_n%%_d); }", v));
        } else {
            if (sg) lf_push(&e, lf_afmt("{ int16_t _n=(int16_t)R16(c->eax); int8_t _d=(int8_t)(%s); SET8L(c->eax,(uint8_t)(int8_t)(_n/_d)); SET8H(c->eax,(uint8_t)(int8_t)(_n%%_d)); }", v));
            else    lf_push(&e, lf_afmt("{ uint16_t _n=R16(c->eax); uint8_t _d=(uint8_t)(%s); SET8L(c->eax,_n/_d); SET8H(c->eax,_n%%_d); }", v));
        }
        return e;
    }

    /* ---- string ops (DF=0 assumed, rep loops on ECX) ---- */
    {
        char sbuf[64];
        char *tok[4] = { 0 };
        int nt = 0;
        char *p;
        const char *rep = NULL, *base;
        int esz;
        const char *wfn, *rfn, *ar;

        strncpy(sbuf, m, sizeof sbuf - 1);
        sbuf[sizeof sbuf - 1] = 0;
        for (p = strtok(sbuf, " "); p && nt < 4; p = strtok(NULL, " ")) tok[nt++] = p;
        if (nt == 0) { tok[nt++] = sbuf; }

        if (LST(tok[0], "rep", "repe", "repz", "repne", "repnz") && nt > 1 &&
            !LST(tok[1], "movsb", "movsw", "movsd", "stosb", "stosw", "stosd",
               "scasb", "scasw", "scasd", "lodsb", "lodsw", "lodsd",
               "cmpsb", "cmpsw", "cmpsd", "insb", "insw", "insd",
               "outsb", "outsw", "outsd")) {
            m = tok[1];
            tok[0] = tok[1];
            nt = 1;
        }
        if (!strcmp(tok[0], "bnd") && nt > 1) {
            m = tok[1];
            tok[0] = tok[1];
            nt = 1;
        }
        if (LST(tok[0], "rep", "repe", "repz", "repne", "repnz", "movsb", "movsw", "movsd",
               "stosb", "stosw", "stosd", "scasb", "scasw", "scasd", "lodsb", "lodsw",
               "lodsd", "cmpsb", "cmpsw", "cmpsd", "insb", "insw", "insd", "outsb",
               "outsw", "outsd")) {
            rep = nt > 1 ? tok[0] : NULL;
            base = rep ? tok[1] : tok[0];
            esz = base[strlen(base) - 1] == 'b' ? 1 :
                  base[strlen(base) - 1] == 'w' ? 2 : 4;
            wfn = esz == 1 ? "wr8" : esz == 2 ? "wr16" : "wr32";
            rfn = esz == 1 ? "rd8" : esz == 2 ? "rd16" : "rd32";
            ar  = esz == 1 ? "R8L(c->eax)" : esz == 2 ? "R16(c->eax)" : "c->eax";
            if (!strncmp(base, "stos", 4)) {
                if (rep && esz == 1) {
                    /* memcpy/memset have a call overhead that dominates tiny
                     * fills and copies; the measured crossover is 16 bytes.
                     * Keep the scalar loop below it, and for overlapping
                     * movs where forward byte semantics are required. */
                    lf_push(&e, lf_afmt("if (c->ecx) { if (c->ecx >= 16) { "
                                        "memset((void *)(uintptr_t)c->edi, "
                                        "(int)(uint8_t)(%s), (size_t)c->ecx); "
                                        "c->edi += c->ecx; } "
                                        "else while (c->ecx) { wr8(c->edi, %s); "
                                        "c->edi++; c->ecx--; } c->ecx = 0; }",
                                        ar, ar));
                    return e;
                }
                if (rep && esz > 1) {
                    /* C.4.3: the wider REP STOS forms are a repeated scalar
                     * pattern, not a byte fill.  A NEON broadcast/store is the
                     * natural implementation; the scalar helper keeps the
                     * host build exact. */
                    lf_push(&e, lf_afmt("if (c->ecx) { rep_stos%s(c->edi, %s, c->ecx); "
                                        "c->edi += c->ecx * %du; c->ecx = 0; }",
                                        esz == 2 ? "w" : "d", ar, esz));
                    return e;
                }
                lf_push(&e, lf_afmt("%s(c->edi, %s); c->edi += %d;", wfn, ar, esz));
            } else if (!strncmp(base, "movs", 4)) {
                char *body = lf_afmt("%s(c->edi, %s(c->esi)); c->esi += %d; c->edi += %d;",
                                     wfn, rfn, esz, esz);
                if (rep) {
                    lf_push(&e, lf_afmt("if (c->ecx) { uint64_t _n = (uint64_t)c->ecx * %dull; "
                                        "if (_n >= 16 && (c->esi + _n <= c->edi || c->edi + _n <= c->esi)) { "
                                        "memcpy((void *)(uintptr_t)c->edi, (void *)(uintptr_t)c->esi, (size_t)_n); "
                                        "c->esi += (uint32_t)_n; c->edi += (uint32_t)_n; } "
                                        "else while (c->ecx) { %s c->ecx--; } "
                                        "c->ecx = 0; }",
                                        esz, body));
                    return e;
                }
                lf_push(&e, body);
            } else if (!strncmp(base, "lods", 4)) {
                if (esz == 1) lf_push(&e, lf_afmt("SET8L(c->eax,%s(c->esi)); c->esi += %d;", rfn, esz));
                else if (esz == 2) lf_push(&e, lf_afmt("SET16(c->eax,%s(c->esi)); c->esi += %d;", rfn, esz));
                else lf_push(&e, lf_afmt("c->eax = %s(c->esi); c->esi += %d;", rfn, esz));
            } else if (!strncmp(base, "scas", 4)) {
                char *cmp = lf_afmt("flags_sub(c, %s, %s(c->edi), %d); c->edi += %d;",
                                    ar, rfn, esz, esz);
                if (rep && (!strcmp(rep, "repe") || !strcmp(rep, "repz")))
                    lf_push(&e, lf_afmt("while (c->ecx) { c->ecx--; %s if (!c->zf) break; }", cmp));
                else if (rep && (!strcmp(rep, "repne") || !strcmp(rep, "repnz")))
                    lf_push(&e, lf_afmt("while (c->ecx) { c->ecx--; %s if (c->zf) break; }", cmp));
                else
                    lf_push(&e, cmp);
                return e;
            } else if (!strncmp(base, "cmps", 4)) {
                char *cmp = lf_afmt("flags_sub(c, %s(c->esi), %s(c->edi), %d); c->esi += %d; c->edi += %d;",
                                    rfn, rfn, esz, esz, esz);
                if (rep && (!strcmp(rep, "repe") || !strcmp(rep, "repz")))
                    lf_push(&e, lf_afmt("while (c->ecx) { c->ecx--; %s if (!c->zf) break; }", cmp));
                else if (rep && (!strcmp(rep, "repne") || !strcmp(rep, "repnz")))
                    lf_push(&e, lf_afmt("while (c->ecx) { c->ecx--; %s if (c->zf) break; }", cmp));
                else
                    lf_push(&e, cmp);
                return e;
            } else {
                lf_push(&e, lf_todo((uint32_t)ea, lf_afmt("string %s %s", m, insn->op_str)));
                return e;
            }
            if (rep) {
                /* the body was pushed; wrap it */
                const char *body = e.v[e.n - 1];
                e.n--;
                lf_push(&e, lf_afmt("while (c->ecx) { %s c->ecx--; }", body));
            }
            return e;
        }
    }

    /* ---- stack ---- */
    if (!strcmp(m, "push")) {
        lf_push(&e, lf_afmt("push32(c, %s);", lf_src(L, insn, OP(insn, 0))));
        return e;
    }
    if (!strcmp(m, "pop")) {
        cs_x86_op *d = OP(insn, 0);
        if (d->type == X86_OP_REG)
            lf_push(&e, lf_afmt("c->%s = (uint32_t)(pop32(c));", lf_reg_name(L, (unsigned)d->reg)));
        else
            lf_push(&e, lf_wr(L, insn, d, "pop32(c)"));
        return e;
    }
    if (LST(m, "pushal", "pushad")) {
        lf_push(&e, "{ uint32_t _sp = c->esp; push32(c, c->eax); push32(c, c->ecx); push32(c, c->edx); push32(c, c->ebx); push32(c, _sp); push32(c, c->ebp); push32(c, c->esi); push32(c, c->edi); }");
        return e;
    }
    if (LST(m, "popal", "popad")) {
        lf_push(&e, "{ c->edi = pop32(c); c->esi = pop32(c); c->ebp = pop32(c); (void)pop32(c); c->ebx = pop32(c); c->edx = pop32(c); c->ecx = pop32(c); c->eax = pop32(c); }");
        return e;
    }

    /* ---- control flow ---- */
    if ((!strcmp(m, "jmp") || !strcmp(m, "call")) && nops &&
        OP(insn, 0)->type == X86_OP_MEM && OP(insn, 0)->size == 6) {
        lf_push(&e, lf_todo((uint32_t)ea, lf_afmt("far %s m16:32", m)));
        return e;
    }
    if (!strcmp(m, "jmp")) {
        cs_x86_op *t = OP(insn, 0);
        if (t->type == X86_OP_IMM && label_has(labels, nlabels, (uint64_t)t->imm)) {
            lf_push(&e, lf_afmt("goto L_%08X;", (uint32_t)t->imm));
            return e;
        }
        {
            int k;
            for (k = 0; k < L->njt; k++) {
                int j, n = 0;
                uint32_t seen[LF_MAX_JT_ENT];
                if (L->jt[k].ea != (uint32_t)ea) continue;
                lf_push(&e, lf_afmt("{ uint32_t _jt = rd32(%s);", lf_addr_expr(L, insn, t)));
                /* sorted unique arms, like sorted(set(tgts)) */
                {
                    uint32_t tmp[LF_MAX_JT_ENT];
                    int a, b;
                    memcpy(tmp, L->jt[k].t, sizeof(uint32_t) * (size_t)L->jt[k].n);
                    for (a = 1; a < L->jt[k].n; a++) {
                        uint32_t v = tmp[a];
                        for (b = a - 1; b >= 0 && tmp[b] > v; b--) tmp[b + 1] = tmp[b];
                        tmp[b + 1] = v;
                    }
                    for (a = 0; a < L->jt[k].n; a++)
                        if (!n || seen[n - 1] != tmp[a]) seen[n++] = tmp[a];
                }
                lf_push(&e, "  switch (_jt) {");
                for (j = 0; j < n; j++) {
                    if (label_has(labels, nlabels, seen[j]))
                        lf_push(&e, lf_afmt("  case GVA(0x%08X): goto L_%08X;", seen[j], seen[j]));
                    else
                        lf_push(&e, lf_afmt("  case GVA(0x%08X): { dispatch_jmp(c, 0x%08Xu); return; }",
                                            seen[j], seen[j]));
                }
                lf_push(&e, "  default: dispatch_jmp(c, _jt); return; }");
                lf_push(&e, "}");
                return e;
            }
        }
        if (t->type == X86_OP_IMM) {
            lf_push(&e, lf_afmt("dispatch_jmp(c, 0x%08Xu); return;", (uint32_t)t->imm));
            return e;
        }
        lf_push(&e, lf_afmt("/* indirect jmp */ dispatch_jmp(c, %s); return;",
                            target_expr(L, insn, t)));
        return e;
    }
    if (!strcmp(m, "call")) {
        cs_x86_op *t = OP(insn, 0);
        char *tgt = target_expr(L, insn, t);
        if (t->type == X86_OP_IMM && (uint64_t)t->imm == nxt) {
            lf_push(&e, lf_afmt("push32(c, 0x%08Xu); /* get-PC, no callee */", (uint32_t)nxt));
            return e;
        }
        if (t->type == X86_OP_IMM) {
            lf_push(&e, lf_afmt("push32(c, 0x%08Xu); dispatch(c, %s);", (uint32_t)nxt, tgt));
            return e;
        }
        lf_push(&e, lf_afmt("{ uint32_t _ct = %s; push32(c, 0x%08Xu); dispatch(c, _ct); }",
                            tgt, (uint32_t)nxt));
        return e;
    }
    if (!strcmp(m, "ret")) {
        int n = (nops && OP(insn, 0)->type == X86_OP_IMM) ? (int)OP(insn, 0)->imm : 0;
        lf_push(&e, lf_afmt("c->esp += %d; return;", 4 + n));
        return e;
    }
    if (m[0] == 'j') {
        const char *cond = lf_cond(m);
        cs_x86_op *t = OP(insn, 0);
        if (!cond) { lf_push(&e, lf_todo((uint32_t)ea, m)); return e; }
        if (t->type == X86_OP_IMM && label_has(labels, nlabels, (uint64_t)t->imm)) {
            lf_push(&e, lf_afmt("if (%s) goto L_%08X;", cond, (uint32_t)t->imm));
            return e;
        }
        if (t->type == X86_OP_IMM) {
            lf_push(&e, lf_afmt("if (%s) { dispatch_jmp(c, 0x%08Xu); return; }",
                                cond, (uint32_t)t->imm));
            return e;
        }
        lf_push(&e, lf_todo((uint32_t)ea, lf_afmt("jcc %s %s", m, insn->op_str)));
        return e;
    }
    if (!strcmp(m, "nop")) { lf_push(&e, "/* nop */"); return e; }
    if (!strcmp(m, "leave")) { lf_push(&e, "c->esp = c->ebp; c->ebp = pop32(c);"); return e; }
    if (!strcmp(m, "stmxcsr")) { lf_push(&e, lf_wr(L, insn, OP(insn, 0), "0x00001F80u")); return e; }
    if (!strcmp(m, "ldmxcsr")) { lf_push(&e, "/* ldmxcsr ignored (no rounding modes or FP exceptions) */"); return e; }

    lf_push(&e, lf_todo((uint32_t)ea, lf_afmt("%s %s", m, insn->op_str)));
    return e;
}

/* ------------------------------------------------------------ PE reading */
size_t lf_read_va(Lifter *L, uint32_t va, uint8_t *buf, size_t n)
{
    Pe *pe = L->pe;
    int i;
    for (i = 0; i < pe->nsec; i++) {
        uint32_t start = pe->base + pe->sec[i].va;
        uint32_t vsize = pe->sec[i].vsize ? pe->sec[i].vsize : pe->sec[i].rawsize;
        uint32_t avail;
        if (va < start || va - start >= vsize) continue;
        avail = vsize - (va - start);
        if (pe->sec[i].rawsize) {
            uint32_t ravail = pe->sec[i].rawsize > (va - start)
                            ? pe->sec[i].rawsize - (va - start) : 0;
            if (ravail < avail) avail = ravail;
        } else {
            return 0;
        }
        if (avail > n) avail = (uint32_t)n;
        if (!avail) return 0;
        if (!pe_read_at(pe, (long long)pe->sec[i].rawptr + (va - start), buf, avail))
            return 0;
        return avail;
    }
    return 0;
}

/* ------------------------------------------------------------- one func */
static int last_word_is_term(const char *mnemonic)
{
    const char *p = strrchr(mnemonic, ' ');
    const char *w = p ? p + 1 : mnemonic;
    return !strcmp(w, "ret") || !strcmp(w, "retn") || !strcmp(w, "retf") ||
           !strcmp(w, "jmp") || !strcmp(w, "iret") || !strcmp(w, "iretd") ||
           !strcmp(w, "hlt");
}

static int decode_into(Lifter *L, const uint8_t *code, size_t len, uint32_t addr,
                       InsnCopy **arr, int *count, int *cap)
{
    const uint8_t *p = code;
    size_t left = len;
    uint64_t a = addr;
    int k;
    cs_insn *tmp = csg.malloc_insn(L->h);

    if (!tmp) return 0;
    while (left && csg.iter(L->h, &p, &left, &a, tmp)) {
        if (*count == *cap) {
            int nc = *cap ? *cap * 2 : 256;
            InsnCopy *na = (InsnCopy *)realloc(*arr, (size_t)nc * sizeof(InsnCopy));
            if (!na) { csg.free_insn(tmp, 1); return 0; }
            *arr = na;
            *cap = nc;
        }
        (*arr)[*count].insn = *tmp;
        if (tmp->detail) (*arr)[*count].det = *tmp->detail;
        (*count)++;
    }
    csg.free_insn(tmp, 1);
    /* The array is reallocated while it grows, so every stored detail pointer
     * has to be re-based on the final address; a pointer captured at insert
     * time dangles after the first move. */
    for (k = 0; k < *count; k++) (*arr)[k].insn.detail = &(*arr)[k].det;
    return 1;
}

/* Decode the exact extent lf_lift_function() emits for a seed.  The flag
 * summary must see the same instructions as the emitted fragment: funcs.txt
 * often cuts a function at a jmp to a shared epilogue, and a summary built
 * from that prefix would miss tail calls and the real ret paths in the grown
 * part. */
static int lf_decode_extent(Lifter *L, uint32_t start, uint32_t size,
                            InsnCopy **out_ins, int *out_n, uint32_t *out_end)
{
    uint8_t *code;
    size_t got;
    InsnCopy *ins = NULL;
    int n = 0, cap = 0;
    uint32_t end;
    int i;

    *out_ins = NULL;
    *out_n = 0;
    if (out_end) *out_end = start;
    code = (uint8_t *)malloc(size ? size : 1);
    if (!code) return 0;
    got = lf_read_va(L, start, code, size);
    if (!decode_into(L, code, got, start, &ins, &n, &cap)) {
        free(code);
        free(ins);
        return 0;
    }
    free(code);
    end = start + (uint32_t)got;

    /* an extent that ends inside an instruction: read the few more bytes */
    if (n > 0) {
        uint32_t tail = (uint32_t)(ins[n - 1].insn.address + ins[n - 1].insn.size);
        if (tail < end) {
            size_t want = (size_t)(end - tail) + 15;
            uint8_t *extra = (uint8_t *)malloc(want);
            size_t got2 = extra ? lf_read_va(L, tail, extra, want) : 0;
            if (got2) {
                int before = n;
                decode_into(L, extra, got2, tail, &ins, &n, &cap);
                for (i = before; i < n; i++)
                    if (ins[i].insn.address >= end) { n = i; break; }
                if (n > 0) {
                    uint32_t ne = (uint32_t)(ins[n - 1].insn.address + ins[n - 1].insn.size);
                    if (ne > end) end = ne;
                }
            }
            free(extra);
        }
    }

    if (L->block_mode && n > 0) {
        for (i = 0; i < n; i++)
            if (last_word_is_term(ins[i].insn.mnemonic)) { n = i + 1; break; }
    }

    {
        /* Two very different things can be seeded here, and they need
         * different extents.
         *
         * A *function entry* - one preceded by int3/nop/zero padding - has to
         * be lifted whole, up to its `ret`.  If it is cut short the prologue's
         * pushes land in one fragment and the epilogue's pops in another, and
         * since each fragment is entered as an independent "function" the pops
         * are balanced against the wrong frame.  That is not a cosmetic
         * problem: game.dll 6F1ACD50 lost its `pop edi` that way (its extent
         * ended on the `jmp` at 6F1ACD78) and the caller then read a field of
         * its object through a zeroed edi.
         *
         * A *mid-function* seed - a jump-table or branch target - must stay
         * short, because the prologue before it is not part of the fragment.
         * Growing those to the function's `ret` is what made a 0x4000 cap
         * explode ijl15 from 44 MB to 1.4 GB.
         *
         * So: whole function when the preceding byte says "entry", otherwise
         * the old 0x100 tail growth.  Note the guard is `whole ||` and not
         * just "not a terminator": an extent that ends exactly on a `jmp` is
         * still only half a function. */
        int whole = 0;
        {
            uint8_t prev = 0;
            if (start == L->image_lo ||
                (lf_read_va(L, start - 1, &prev, 1) == 1 &&
                 (prev == 0xCC || prev == 0x90 || prev == 0x00)))
                whole = 1;
        }
        if (n > 0 && (whole || !last_word_is_term(ins[n - 1].insn.mnemonic))) {
            int grown = 0;
            int cap_grow = whole ? 0x800 : 0x100;
            while (grown < cap_grow) {
                InsnCopy *last = &ins[n - 1];
                uint32_t nxt;
                uint8_t more[300];
                size_t got3;
                int before;
                if (last_word_is_term(last->insn.mnemonic)) {
                    uint8_t pad = 0;
                    size_t pg = lf_read_va(L, (uint32_t)(last->insn.address + last->insn.size),
                                           &pad, 1);
                    if (whole && strstr(last->insn.mnemonic, "ret")) break;
                    if (!pg || pad == 0xCC || pad == 0x90) break;
                    if (strstr(last->insn.mnemonic, "jmp") &&
                        strchr(last->insn.op_str, '[')) break;
                }
                nxt = (uint32_t)(last->insn.address + last->insn.size);
                {
                    size_t budget = (size_t)(cap_grow - grown);
                    if (budget > sizeof more - 15) budget = sizeof more - 15;
                    got3 = lf_read_va(L, nxt, more, budget + 15);
                }
                if (!got3) break;
                before = n;
                decode_into(L, more, got3, nxt, &ins, &n, &cap);
                if (n == before) break;
                if (whole) {
                    int k;
                    for (k = before; k < n; k++)
                        if (strstr(ins[k].insn.mnemonic, "ret")) { n = k + 1; break; }
                }
                if (L->block_mode) {
                    int k;
                    for (k = before; k < n; k++)
                        if (last_word_is_term(ins[k].insn.mnemonic)) { n = k + 1; break; }
                }
                {
                    uint32_t ne = (uint32_t)(ins[n - 1].insn.address + ins[n - 1].insn.size);
                    grown += (int)(ne - nxt);
                }
                if (L->block_mode && last_word_is_term(ins[n - 1].insn.mnemonic))
                    break;
            }
            if (n > 0) {
                uint32_t ne = (uint32_t)(ins[n - 1].insn.address + ins[n - 1].insn.size);
                if (ne > end) end = ne;
            }
        }
    }

    *out_ins = ins;
    *out_n = n;
    if (out_end) *out_end = end;
    return 1;
}

static void append_str(char **out, size_t *len, size_t *cap, const char *s)
{
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) {
        while (*len + n + 1 > *cap) *cap = *cap * 2 + 4096;
        *out = (char *)realloc(*out, *cap);
    }
    memcpy(*out + *len, s, n);
    *len += n;
}

/* ------------------------------------------------ A.2 flag planning */

static const char *flag_strip_rep(const char *m)
{
    if (!strncmp(m, "rep ", 4)) return m + 4;
    if (!strncmp(m, "repe ", 5)) return m + 5;
    if (!strncmp(m, "repne ", 6)) return m + 6;
    if (!strncmp(m, "repz ", 5)) return m + 5;
    if (!strncmp(m, "repnz ", 6)) return m + 6;
    return m;
}

static int flag_is_control(const char *m)
{
    m = flag_strip_rep(m);
    if (m[0] == 'j') return 1;
    if (!strncmp(m, "loop", 4)) return 1;
    return !strcmp(m, "call") || !strcmp(m, "ret") || !strcmp(m, "retn") ||
           !strcmp(m, "retf") || !strcmp(m, "iret") || !strcmp(m, "iretd") ||
           !strcmp(m, "hlt") || !strcmp(m, "int") || !strcmp(m, "int3") ||
           !strcmp(m, "into") || !strcmp(m, "ud2") ||
           !strcmp(m, "syscall") || !strcmp(m, "sysenter");
}

static LfFlagConsumer flag_consumer_of(Lifter *L, const cs_insn *in,
                                       uint8_t *mask)
{
    const char *m = in->mnemonic;
    const char *cc;
    (void)L;
    *mask = 0;
    if (m[0] == 'j' && strcmp(m, "jmp") &&
        strcmp(m, "jecxz") && strcmp(m, "jcxz")) {
        cc = m + 1;
        *mask = lf_cond_mask(cc);
        if (*mask && NOPS(in) >= 1 && OP(in, 0)->type == X86_OP_IMM)
            return LF_CONS_JCC;
        return LF_CONS_NONE;
    }
    if (!strncmp(m, "set", 3) && strcmp(m, "setssbsy")) {
        cc = m + 3;
        *mask = lf_cond_mask(cc);
        if (*mask && NOPS(in) == 1) return LF_CONS_SETCC;
        return LF_CONS_NONE;
    }
    if (!strncmp(m, "cmov", 4)) {
        cc = m + 4;
        *mask = lf_cond_mask(cc);
        if (*mask && NOPS(in) == 2) return LF_CONS_CMOV;
        return LF_CONS_NONE;
    }
    if (!strcmp(m, "lahf")) {
        /* LAHF is a flag reader but not a branch: it copies SF/ZF/AF/PF/CF
         * into AH.  Folding an FP compare into it removes the flag stores and
         * the separate LAHF read without changing the AH value. */
        *mask = LF_FLAG_SF | LF_FLAG_ZF | LF_FLAG_AF | LF_FLAG_PF | LF_FLAG_CF;
        return LF_CONS_LAHF;
    }
    return LF_CONS_NONE;
}

static LfFlagKind flag_producer_of(const cs_insn *in, int *sz)
{
    const char *m = in->mnemonic;
    int nops = NOPS(in);
    int s;

    *sz = 0;
    if (!strncmp(m, "lock ", 5)) return LF_FLAG_NONE;
    if (!strcmp(m, "ucomiss") || !strcmp(m, "comiss") ||
        !strcmp(m, "ucomisd") || !strcmp(m, "comisd")) {
        if (nops < 2) return LF_FLAG_NONE;
        /* Capstone reports the XMM register as 16 bytes; the compare width is
         * encoded in the mnemonic suffix, not in the operand size. */
        *sz = (m[strlen(m) - 1] == 'd') ? 8 : 4;
        return LF_FLAG_FP_CMP;
    }
    if (!strcmp(m, "inc") || !strcmp(m, "dec") || !strcmp(m, "neg")) {
        if (nops < 1 || OP(in, 0)->type == X86_OP_IMM) return LF_FLAG_NONE;
        s = OP(in, 0)->size;
        if (s != 1 && s != 2 && s != 4) return LF_FLAG_NONE;
        *sz = s;
        if (!strcmp(m, "inc")) return LF_FLAG_INC;
        if (!strcmp(m, "dec")) return LF_FLAG_DEC;
        return LF_FLAG_NEG;
    }
    if (!strcmp(m, "cmp") || !strcmp(m, "test") ||
        !strcmp(m, "add") || !strcmp(m, "sub") ||
        !strcmp(m, "and") || !strcmp(m, "or") || !strcmp(m, "xor") ||
        !strcmp(m, "adc") || !strcmp(m, "sbb")) {
        if (nops < 2 || OP(in, 0)->type == X86_OP_IMM) return LF_FLAG_NONE;
        s = OP(in, 0)->size;
        if (s != 1 && s != 2 && s != 4) return LF_FLAG_NONE;
        *sz = s;
        if (!strcmp(m, "cmp")) return LF_FLAG_CMP;
        if (!strcmp(m, "test")) return LF_FLAG_TEST;
        if (!strcmp(m, "add")) return LF_FLAG_ADD;
        if (!strcmp(m, "sub")) return LF_FLAG_SUB;
        if (!strcmp(m, "and")) return LF_FLAG_AND;
        if (!strcmp(m, "or")) return LF_FLAG_OR;
        if (!strcmp(m, "xor")) return LF_FLAG_XOR;
        if (!strcmp(m, "adc")) return LF_FLAG_ADC;
        return LF_FLAG_SBB;
    }
    return LF_FLAG_NONE;
}

/* Conservative flag-use summary.  Under-reporting a write would wrongly let a
 * producer's flags be treated as dead, so unknown integer instructions are
 * marked as readers of every flag and writers of none.  That keeps liveness
 * live and blocks fusion rather than changing behaviour. */
static LfFlagSummary *lf_get_summary(Lifter *L, uint32_t addr);

/* Flags live at an external tail jump.  entry_reads is what the target reads
 * before overwriting; the current function's exit needs must survive the jump
 * unless the target definitely overwrites them on every exit path. */
static uint8_t lf_tail_reads(Lifter *L, const cs_insn *in,
                             const uint32_t *labels, int nlabels)
{
    LfFlagSummary *s;
    if (!L->flag_boundary) return 0;
    if (NOPS(in) < 1 || OP(in, 0)->type != X86_OP_IMM)
        return LF_FLAG_ALL; /* unknown target */
    if (labels && label_has(labels, nlabels, (uint32_t)OP(in, 0)->imm))
        return 0;
    s = lf_get_summary(L, (uint32_t)OP(in, 0)->imm);
    if (!s) return LF_FLAG_ALL;
    return (uint8_t)((s->entry_reads |
                      (L->summary.exit_flags_needed &
                       (uint8_t)(LF_FLAG_ALL & ~s->exit_must_writes))) &
                     LF_FLAG_ALL);
}

static void flag_use(Lifter *L, const cs_insn *in, const uint32_t *labels,
                     int nlabels, uint8_t *reads, uint8_t *writes)
{
    const char *m = in->mnemonic;
    uint8_t r = 0, w = 0, cmask = 0;
    LfFlagConsumer cons;
    int is_cond = 0;

    if (!strncmp(m, "lock ", 5)) m += 5;

    if (!strncmp(m, "rep ", 4) || !strncmp(m, "repe ", 5) ||
        !strncmp(m, "repne ", 6) || !strncmp(m, "repz ", 5) ||
        !strncmp(m, "repnz ", 6)) {
        const char *op = strchr(m, ' ');
        op = op ? op + 1 : m;
        if (!strncmp(op, "cmps", 4) || !strncmp(op, "scas", 4)) {
            r |= LF_FLAG_ALL;
            w |= LF_FLAG_CF | LF_FLAG_ZF | LF_FLAG_SF | LF_FLAG_OF |
                 LF_FLAG_AF | LF_FLAG_PF;
        } else if (strncmp(op, "movs", 4) && strncmp(op, "stos", 4) &&
                   strncmp(op, "lods", 4)) {
            /* A rep prefix on a non-string instruction is outside the fast
             * path (notably the `rep ret` idiom).  Treat it as a full flag
             * barrier instead of silently calling it transparent. */
            r |= LF_FLAG_ALL;
        }
        *reads = (uint8_t)(r & LF_FLAG_ALL);
        *writes = (uint8_t)(w & LF_FLAG_ALL);
        return;
    }

    cons = flag_consumer_of(L, in, &cmask);
    if (cons != LF_CONS_NONE) {
        r |= cmask;
        is_cond = 1;
    } else if (m[0] == 'j' && strcmp(m, "jmp") &&
               strcmp(m, "jecxz") && strcmp(m, "jcxz")) {
        r |= LF_FLAG_ALL;
        is_cond = 1;
    } else if (!strncmp(m, "set", 3) && strcmp(m, "setssbsy")) {
        r |= LF_FLAG_ALL;
        is_cond = 1;
    } else if (!strncmp(m, "cmov", 4)) {
        r |= LF_FLAG_ALL;
        is_cond = 1;
    }
    if (is_cond) {
        if (cons == LF_CONS_JCC)
            r |= lf_tail_reads(L, in, labels, nlabels);
        *reads = (uint8_t)(r & LF_FLAG_ALL);
        *writes = (uint8_t)(w & LF_FLAG_ALL);
        return;
    }

    if (!strcmp(m, "loop") || !strcmp(m, "loope") || !strcmp(m, "loopz") ||
        !strcmp(m, "loopne") || !strcmp(m, "loopnz")) {
        if (strcmp(m, "loop")) r |= LF_FLAG_ZF;
        /* loop is also a control transfer; its external target needs the same
         * boundary reads as jmp/jcc.  The earlier else-if used to skip the
         * control branch below and silently lost the target's needs. */
        r |= lf_tail_reads(L, in, labels, nlabels);
    } else if (!strcmp(m, "pushf") || !strcmp(m, "pushfd") ||
               !strcmp(m, "pushfq")) {
        r |= LF_FLAG_ALL;
    } else if (!strcmp(m, "popf") || !strcmp(m, "popfd") ||
               !strcmp(m, "popfq")) {
        w |= LF_FLAG_ALL;
    } else if (!strcmp(m, "lahf")) {
        r |= LF_FLAG_CF | LF_FLAG_PF | LF_FLAG_AF | LF_FLAG_ZF | LF_FLAG_SF;
    } else if (!strcmp(m, "sahf")) {
        w |= LF_FLAG_CF | LF_FLAG_PF | LF_FLAG_AF | LF_FLAG_ZF | LF_FLAG_SF;
    } else if (!strcmp(m, "into")) {
        r |= LF_FLAG_ALL;
    } else if (!strcmp(m, "clc") || !strcmp(m, "stc")) {
        w |= LF_FLAG_CF;
    } else if (!strcmp(m, "cmc")) {
        r |= LF_FLAG_CF;
        w |= LF_FLAG_CF;
    } else if (!strcmp(m, "adc") || !strcmp(m, "sbb")) {
        r |= LF_FLAG_CF;
        w |= LF_FLAG_CF | LF_FLAG_ZF | LF_FLAG_SF | LF_FLAG_OF |
             LF_FLAG_AF | LF_FLAG_PF;
    } else if (!strcmp(m, "rcl") || !strcmp(m, "rcr")) {
        r |= LF_FLAG_CF;
        w |= LF_FLAG_CF | LF_FLAG_OF;
    } else if (!strcmp(m, "rol") || !strcmp(m, "ror")) {
        w |= LF_FLAG_CF | LF_FLAG_OF;
    } else if (!strcmp(m, "shl") || !strcmp(m, "sal") ||
               !strcmp(m, "shr") || !strcmp(m, "sar") ||
               !strcmp(m, "shld") || !strcmp(m, "shrd")) {
        w |= LF_FLAG_CF | LF_FLAG_ZF | LF_FLAG_SF | LF_FLAG_PF;
    } else if (!strcmp(m, "cmp") || !strcmp(m, "add") ||
               !strcmp(m, "sub") || !strcmp(m, "neg")) {
        /* flags_sub/flags_add/flags_logicz: the first three write AF too;
         * flags_* logic (and/or/xor/test) leaves AF undefined, so it must
         * stay conservative and not kill an incoming AF. */
        w |= LF_FLAG_CF | LF_FLAG_ZF | LF_FLAG_SF | LF_FLAG_OF |
             LF_FLAG_AF | LF_FLAG_PF;
    } else if (!strcmp(m, "test") || !strcmp(m, "and") ||
               !strcmp(m, "or") || !strcmp(m, "xor")) {
        w |= LF_FLAG_CF | LF_FLAG_ZF | LF_FLAG_SF | LF_FLAG_OF | LF_FLAG_PF;
    } else if (!strcmp(m, "inc") || !strcmp(m, "dec")) {
        w |= LF_FLAG_ZF | LF_FLAG_SF | LF_FLAG_OF | LF_FLAG_AF | LF_FLAG_PF;
    } else if (!strcmp(m, "bt") || !strcmp(m, "bts") ||
               !strcmp(m, "btr") || !strcmp(m, "btc")) {
        w |= LF_FLAG_CF;
    } else if (!strcmp(m, "bsf") || !strcmp(m, "bsr")) {
        w |= LF_FLAG_ZF;
    } else if (!strcmp(m, "xadd") || !strcmp(m, "cmpxchg")) {
        w |= LF_FLAG_CF | LF_FLAG_ZF | LF_FLAG_SF | LF_FLAG_OF |
             LF_FLAG_AF | LF_FLAG_PF;
    } else if (!strcmp(m, "cmpxchg8b")) {
        w |= LF_FLAG_ZF;
    } else if (!strcmp(m, "daa") || !strcmp(m, "das") ||
               !strcmp(m, "aaa") || !strcmp(m, "aas") ||
               !strcmp(m, "aam") || !strcmp(m, "aad")) {
        r |= LF_FLAG_ALL;
        w |= LF_FLAG_ALL;
    } else if (!strcmp(m, "mul") || !strcmp(m, "imul")) {
        /* MUL/IMUL write CF/OF and read no flags.  A.24 drops the write when
         * both are dead instead of treating the instruction as a full barrier. */
        w |= LF_FLAG_CF | LF_FLAG_OF;
    } else if (!strcmp(m, "div") || !strcmp(m, "idiv")) {
        /* DIV leaves flags undefined.  Keep the existing helper behaviour
         * (they are untouched), but kill the liveness state so an earlier
         * producer is not kept alive across the division. */
        w |= LF_FLAG_ALL;
    } else if (m[0] == 'f') {
        if (!strncmp(m, "fcmov", 5))
            r |= LF_FLAG_CF | LF_FLAG_ZF | LF_FLAG_PF;
        else if (!strncmp(m, "fcomi", 5) || !strncmp(m, "fucomi", 6) ||
                 !strncmp(m, "fcompi", 6) || !strncmp(m, "fucompi", 7))
            w |= LF_FLAG_ALL;
    } else if (lf_is_mmx(L, (cs_insn *)in) || lf_is_sse(L, (cs_insn *)in)) {
        if (strstr(m, "comis") || strstr(m, "ucomis"))
            w |= LF_FLAG_ALL;
        else if (strstr(m, "ptest") || strstr(m, "vptest"))
            r |= LF_FLAG_ALL;
    } else if (!strncmp(m, "movs", 4) || !strncmp(m, "stos", 4) ||
               !strncmp(m, "lods", 4)) {
        /* string move: no flags */
    } else if (!strncmp(m, "cmps", 4) || !strncmp(m, "scas", 4)) {
        w |= LF_FLAG_CF | LF_FLAG_ZF | LF_FLAG_SF | LF_FLAG_OF |
             LF_FLAG_AF | LF_FLAG_PF;
    } else if (!strcmp(m, "mov") || !strcmp(m, "movzx") ||
               !strcmp(m, "movsx") || !strcmp(m, "movsxd") ||
               !strcmp(m, "lea") || !strcmp(m, "not") ||
               !strcmp(m, "push") || !strcmp(m, "pop") ||
               !strcmp(m, "xchg") ||
               !strcmp(m, "bswap") || !strcmp(m, "nop") ||
               !strcmp(m, "hint_nop") || !strncmp(m, "prefetch", 8) ||
               !strcmp(m, "cbw") || !strcmp(m, "cwde") ||
               !strcmp(m, "cwd") || !strcmp(m, "cdq") ||
               !strcmp(m, "pusha") || !strcmp(m, "popa") ||
               !strcmp(m, "pushad") || !strcmp(m, "popad") ||
               !strcmp(m, "enter") || !strcmp(m, "leave") ||
               !strcmp(m, "xlat") || !strcmp(m, "stmxcsr") ||
               !strcmp(m, "ldmxcsr") || !strcmp(m, "emms") ||
               !strcmp(m, "lfence") || !strcmp(m, "mfence") ||
               !strcmp(m, "sfence") || !strcmp(m, "pause") ||
               !strcmp(m, "wait") || !strcmp(m, "cpuid") ||
               !strcmp(m, "rdtsc") || !strcmp(m, "rdtscp")) {
        /* no EFLAGS effect */
    } else if (flag_is_control(m)) {
        /* call/ret/int and unsupported transfers: a caller, callee or trap
         * handler may observe every flag.  With the global boundary model,
         * use per-function summaries instead of the conservative ALL. */
        if (!strcmp(m, "call")) {
            if (L->flag_boundary && NOPS(in) >= 1 &&
                OP(in, 0)->type == X86_OP_IMM) {
                LfFlagSummary *s = lf_get_summary(L, (uint32_t)OP(in, 0)->imm);
                if (s) {
                    r |= (uint8_t)(s->entry_reads |
                                  (uint8_t)(LF_FLAG_ALL &
                                            ~s->exit_must_writes));
                    w |= s->exit_must_writes;
                } else {
                    /* No summary (table or allocation failure).  The callee
                     * may leave a flag untouched, so killing it here would
                     * expose a stale pre-call value.  Keep every incoming
                     * flag. */
                    r |= LF_FLAG_ALL;
                }
            } else {
                r |= LF_FLAG_ALL;
            }
        } else if (!strcmp(m, "jmp") || m[0] == 'j' ||
                   !strncmp(m, "loop", 4)) {
            r |= lf_tail_reads(L, in, labels, nlabels);
        } else if (L->flag_boundary &&
                   (!strcmp(m, "ret") || !strcmp(m, "retn") ||
                    !strcmp(m, "retf"))) {
            /* The ret must keep every flag the caller needs alive, not only
             * the flags the summary believes this function may write.  The
             * may-write set can still be incomplete for conditional OF forms
             * and for helpers whose exact EFLAGS effect is not modelled;
             * filtering by it would drop the function's own writer and
             * expose a stale incoming value.  An unknown/no-summary function
             * still takes the conservative ALL path below. */
            if (L->summary.addr != 0xffffffffu)
                r |= (uint8_t)(L->summary.exit_flags_needed & LF_FLAG_ALL);
            else
                r |= LF_FLAG_ALL;
        } else {
            r |= LF_FLAG_ALL;
        }
    } else {
        r |= LF_FLAG_ALL;
    }

    *reads = (uint8_t)(r & LF_FLAG_ALL);
    *writes = (uint8_t)(w & LF_FLAG_ALL);
}

/* Can this producer drop its flag writes?  Only when every flag it writes is
 * dead on every outgoing edge.  flag_use() already adds AF for the producer
 * kinds whose helper writes it; the extra AF term below is a defensive guard
 * for a future kind/helper mismatch, not the primary source of that bit. */
static int flag_dead(Lifter *L, const cs_insn *in, int *kind, int *sz)
{
    uint8_t reads, writes;
    int k;
    if (!L->flagless || !L->flag_live_out || L->flag_current_index < 0) return 0;
    /* cmp/test with a memory operand is also a guest load.  Dropping it would
     * hide a possible page fault, so only the register/immediate forms may be
     * removed outright; memory forms fall back to the flags helper. */
    if (!strcmp(in->mnemonic, "cmp") || !strcmp(in->mnemonic, "test")) {
        for (k = 0; k < NOPS(in); k++)
            if (OP(in, k)->type == X86_OP_MEM) return 0;
    }
    *kind = flag_producer_of(in, sz);
    if (*kind == LF_FLAG_NONE) return 0;
    flag_use(L, in, NULL, 0, &reads, &writes);
    if (L->flag_live_out[L->flag_current_index] &
        (writes | (lf_flag_producer_af(*kind) ? LF_FLAG_AF : 0)))
        return 0;
    return 1;
}

static int flag_insn_index(const InsnCopy *ins, int n, uint64_t addr)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint64_t a = ins[mid].insn.address;
        if (a == addr) return mid;
        if (a < addr) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

/* ---- global call/ret flag summaries ---- */

static int is_ret_mnemonic(const char *m)
{
    return !strcmp(m, "ret") || !strcmp(m, "retn") || !strcmp(m, "retf");
}

static void flag_use_arch(Lifter *L, const cs_insn *in, uint8_t *reads,
                          uint8_t *writes)
{
    int save = L->flag_boundary;
    L->flag_boundary = 0;
    flag_use(L, in, NULL, 0, reads, writes);
    L->flag_boundary = save;
    if (!strcmp(in->mnemonic, "call") || !strcmp(in->mnemonic, "ret") ||
        !strcmp(in->mnemonic, "retn") || !strcmp(in->mnemonic, "retf") ||
        !strcmp(in->mnemonic, "iret") || !strcmp(in->mnemonic, "iretd") ||
        !strcmp(in->mnemonic, "hlt") || !strcmp(in->mnemonic, "int") ||
        !strcmp(in->mnemonic, "int3") || !strcmp(in->mnemonic, "into") ||
        !strcmp(in->mnemonic, "syscall") ||
        !strcmp(in->mnemonic, "sysenter")) {
        *reads = 0;
        *writes = 0;
    }
    if (*reads == LF_FLAG_ALL && *writes == 0)
        *writes = LF_FLAG_ALL; /* unknown instruction may write flags */
}

static int lf_func_index(Lifter *L, uint32_t addr)
{
    size_t lo = 0, hi = L->func_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (L->func_map[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < L->func_n && L->func_map[lo].addr == addr)
        return (int)lo;
    return -1;
}

static uint32_t lf_func_size(Lifter *L, uint32_t addr)
{
    int idx = lf_func_index(L, addr);
    return idx >= 0 ? L->func_map[idx].size : 0;
}

/* The function whose claimed [addr, addr+size) range contains the value.
 * Used only by the address-taken scan: an interior pointer (a thunk or a
 * computed entry) still makes that function's ret indirectly reachable, so
 * the containing function has to stay conservative. */
static int lf_func_containing(Lifter *L, uint32_t addr)
{
    size_t lo = 0, hi = L->func_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (L->func_map[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < L->func_n && L->func_map[lo].addr == addr)
        return (int)lo;
    if (lo > 0) {
        size_t p = lo - 1;
        /* funcs.txt can end a function at a jmp to a shared epilogue, while
         * lf_decode_extent() keeps growing it: the 0x800 cap is checked per
         * batch, so the grown extent can overshoot it by one 300-byte batch.
         * Accept 0xC00 past the claimed size too: an interior pointer there
         * still makes the function's ret indirectly reachable.  Over-marking
         * only costs optimization; a missed mark would hide the ret. */
        uint64_t end = (uint64_t)L->func_map[p].addr + L->func_map[p].size + 0xC00u;
        if ((uint64_t)addr < end) return (int)p;
    }
    return -1;
}

static uint32_t lf_summary_hash(uint32_t addr)
{
    return (addr * 2654435761u) >> 8;
}

static void lf_summary_table_init(Lifter *L, size_t cap)
{
    size_t i;
    if (L->summary_table) return;
    while (cap & (cap - 1)) cap++;
    if (cap < 1024) cap = 1024;
    L->summary_table_cap = cap;
    L->summary_table = (LfFlagSummary *)calloc(cap, sizeof(LfFlagSummary));
    if (!L->summary_table) return;
    for (i = 0; i < cap; i++) L->summary_table[i].addr = 0xffffffffu;
}

static LfFlagSummary *lf_summary_lookup(Lifter *L, uint32_t addr)
{
    size_t mask, idx;
    if (!L->summary_table) return NULL;
    mask = L->summary_table_cap - 1;
    idx = lf_summary_hash(addr) & mask;
    while (L->summary_table[idx].addr != 0xffffffffu) {
        if (L->summary_table[idx].addr == addr)
            return &L->summary_table[idx];
        idx = (idx + 1) & mask;
    }
    return NULL;
}

static LfFlagSummary *lf_summary_insert_raw(Lifter *L, uint32_t addr)
{
    size_t mask = L->summary_table_cap - 1;
    size_t idx = lf_summary_hash(addr) & mask;
    while (L->summary_table[idx].addr != 0xffffffffu) {
        if (L->summary_table[idx].addr == addr)
            return &L->summary_table[idx];
        idx = (idx + 1) & mask;
    }
    L->summary_table[idx].addr = addr;
    L->summary_table[idx].entry_reads = LF_FLAG_ALL;
    L->summary_table[idx].exit_may_writes = LF_FLAG_ALL;
    L->summary_table[idx].exit_must_writes = 0;
    L->summary_table[idx].exit_flags_needed = 0;
    L->summary_table[idx].computed = 0;
    L->summary_table[idx].conservative = 1;
    L->summary_table[idx].queued = 0;
    L->summary_table_n++;
    return &L->summary_table[idx];
}

static void lf_summary_grow(Lifter *L)
{
    size_t old_cap = L->summary_table_cap;
    LfFlagSummary *old_table = L->summary_table;
    size_t new_cap = old_cap * 2;
    LfFlagSummary *new_table;
    size_t i;
    if (!old_table) { lf_summary_table_init(L, 1024); return; }
    new_table = (LfFlagSummary *)calloc(new_cap, sizeof(LfFlagSummary));
    if (!new_table) return;
    for (i = 0; i < new_cap; i++) new_table[i].addr = 0xffffffffu;
    L->summary_table = new_table;
    L->summary_table_cap = new_cap;
    L->summary_table_n = 0;
    for (i = 0; i < old_cap; i++) {
        if (old_table[i].addr != 0xffffffffu) {
            LfFlagSummary *s = lf_summary_insert_raw(L, old_table[i].addr);
            if (s) *s = old_table[i];
        }
    }
    free(old_table);
}

static LfFlagSummary *lf_summary_insert(Lifter *L, uint32_t addr)
{
    LfFlagSummary *s;
    if (!L->summary_table) lf_summary_table_init(L, 1024);
    if (!L->summary_table) {
        L->summary_incomplete = 1;
        return NULL;
    }
    /* Updating an existing entry must not depend on the load factor: a full
     * table used to return NULL for an address that was already present. */
    s = lf_summary_lookup(L, addr);
    if (s) return s;
    if (L->summary_table_n * 10 >= L->summary_table_cap * 7)
        lf_summary_grow(L);
    if (!L->summary_table ||
        L->summary_table_n * 10 >= L->summary_table_cap * 7) {
        /* A full table must not silently drop summaries: callers would use
         * stale N=0 and the ret would not store their flags. */
        L->summary_incomplete = 1;
        return NULL;
    }
    s = lf_summary_insert_raw(L, addr);
    if (!s) L->summary_incomplete = 1;
    return s;
}

static int is_flag_barrier(const char *m)
{
    return !strcmp(m, "hlt") || !strcmp(m, "int") || !strcmp(m, "int3") ||
           !strcmp(m, "into") || !strcmp(m, "iret") ||
           !strcmp(m, "iretd") || !strcmp(m, "syscall") ||
           !strcmp(m, "sysenter") || !strcmp(m, "rsm") ||
           !strcmp(m, "ud2");
}

/* True only when the instruction defines the flags it writes on every
 * execution.  Call/return/trap barriers are handled separately; shifts and
 * rotates with a zero or variable count, and DIV/IDIV, are deliberately not
 * in this set because their flag effects are conditional or undefined. */
static int is_must_flag_writer(const cs_insn *in)
{
    const char *m = in->mnemonic;
    int sz;
    if (flag_producer_of(in, &sz) != LF_FLAG_NONE) return 1;
    return !strcmp(m, "popf") || !strcmp(m, "popfd") ||
           !strcmp(m, "popfq") || !strcmp(m, "sahf") ||
           !strncmp(m, "cmps", 4) || !strncmp(m, "scas", 4) ||
           !strncmp(m, "fcomi", 5) || !strncmp(m, "fucomi", 6) ||
           !strncmp(m, "fcompi", 6) || !strncmp(m, "fucompi", 7) ||
           !strcmp(m, "mul") || !strcmp(m, "imul") ||
           !strcmp(m, "bt") || !strcmp(m, "bts") ||
           !strcmp(m, "btr") || !strcmp(m, "btc") ||
           !strcmp(m, "bsf") || !strcmp(m, "bsr") ||
           !strcmp(m, "stc") || !strcmp(m, "clc") ||
           !strcmp(m, "cmc") || !strcmp(m, "xadd") ||
           !strcmp(m, "cmpxchg") || !strcmp(m, "cmpxchg8b");
}

static int lf_summary_cfg(Lifter *L, const InsnCopy *ins, int n,
                          LfFlagInsn *d)
{
    int i;
    int has_indirect = 0;
    for (i = 0; i < n; i++) {
        const cs_insn *in = &ins[i].insn;
        const char *m = in->mnemonic;
        d[i].reads = d[i].writes = d[i].must_writes = 0;
        d[i].is_ret = (uint8_t)is_ret_mnemonic(m);
        flag_use_arch(L, in, &d[i].reads, &d[i].writes);
        if (!strcmp(m, "call") || is_flag_barrier(m)) {
            /* A callee or trap handler may read any incoming flag and leave
             * any outgoing flag; nothing is a guaranteed write. */
            d[i].reads = LF_FLAG_ALL;
            d[i].writes = LF_FLAG_ALL;
            d[i].must_writes = 0;
        } else if (d[i].is_ret) {
            d[i].reads = 0;
            d[i].writes = 0;
            d[i].must_writes = 0;
        } else if (d[i].reads == LF_FLAG_ALL &&
                   d[i].writes == LF_FLAG_ALL) {
            d[i].must_writes = 0;
        } else if (is_must_flag_writer(in)) {
            d[i].must_writes = d[i].writes;
        } else {
            d[i].must_writes = 0;
        }
        d[i].succ1 = d[i].succ2 = -2;
        if (!strcmp(m, "jmp")) {
            if (NOPS(in) >= 1 && OP(in, 0)->type == X86_OP_IMM) {
                int t = flag_insn_index(ins, n, (uint64_t)OP(in, 0)->imm);
                if (t < 0) {
                    d[i].reads = LF_FLAG_ALL;
                    d[i].writes = LF_FLAG_ALL;
                    d[i].must_writes = 0;
                }
                d[i].succ1 = t >= 0 ? t : -1;
            } else {
                /* The target is unknown: it may read any incoming flag and
                 * leave any outgoing one, exactly like an indirect call. */
                d[i].reads = LF_FLAG_ALL;
                d[i].writes = LF_FLAG_ALL;
                d[i].must_writes = 0;
                d[i].succ1 = -1;
                has_indirect = 1;
            }
        } else if (m[0] == 'j' || !strncmp(m, "loop", 4)) {
            if (NOPS(in) >= 1 && OP(in, 0)->type == X86_OP_IMM) {
                int t = flag_insn_index(ins, n, (uint64_t)OP(in, 0)->imm);
                if (t < 0) {
                    d[i].reads = LF_FLAG_ALL;
                    d[i].writes = LF_FLAG_ALL;
                    d[i].must_writes = 0;
                }
                d[i].succ1 = t >= 0 ? t : -1;
            } else {
                d[i].reads = LF_FLAG_ALL;
                d[i].writes = LF_FLAG_ALL;
                d[i].must_writes = 0;
                d[i].succ1 = -1;
                has_indirect = 1;
            }
            d[i].succ2 = (i + 1 < n) ? i + 1 : -1;
        } else if (!strcmp(m, "call")) {
            if (NOPS(in) >= 1 && OP(in, 0)->type != X86_OP_IMM)
                has_indirect = 1;
            d[i].succ1 = (i + 1 < n) ? i + 1 : -1;
        } else if (!strcmp(m, "ret") || !strcmp(m, "retn") ||
                   !strcmp(m, "retf") || !strcmp(m, "hlt") ||
                   !strcmp(m, "iret") || !strcmp(m, "iretd")) {
            /* no successor */
        } else if (!strcmp(m, "int3") || !strcmp(m, "int") ||
                   !strcmp(m, "into")) {
            d[i].succ1 = (i + 1 < n) ? i + 1 : -1;
            d[i].succ2 = -1;
        } else if (flag_is_control(m)) {
            d[i].succ1 = -1;
            d[i].succ2 = -1;
        } else {
            d[i].succ1 = (i + 1 < n) ? i + 1 : -1;
        }
    }
    return has_indirect;
}

static uint8_t lf_entry_reads(const LfFlagInsn *d, int n)
{
    uint8_t *in, *queued, entry_reads = 0;
    int *work;
    size_t head = 0, tail = 0, work_cap;
    if (n <= 0) return LF_FLAG_ALL;
    work_cap = (size_t)n * 8 + 8;
    in = (uint8_t *)calloc((size_t)n, 1);
    queued = (uint8_t *)calloc((size_t)n, 1);
    work = (int *)malloc(work_cap * sizeof(int));
    if (!in || !queued || !work) {
        free(in); free(queued); free(work);
        return LF_FLAG_ALL;
    }
    in[0] = LF_FLAG_ALL;
    work[tail++] = 0;
    queued[0] = 1;
    while (head < tail) {
        int i = work[head++];
        uint8_t out = (uint8_t)(in[i] & ~d[i].must_writes);
        entry_reads |= (uint8_t)(d[i].reads & in[i]);
        int s;
        for (s = 0; s < 2; s++) {
            int succ = s == 0 ? d[i].succ1 : d[i].succ2;
            if (succ >= 0 && succ < n) {
                uint8_t ni = (uint8_t)(in[succ] | out);
                if (ni != in[succ]) {
                    in[succ] = ni;
                    if (!queued[succ]) {
                        if (tail >= work_cap) {
                            free(in); free(queued); free(work);
                            return LF_FLAG_ALL;
                        }
                        queued[succ] = 1;
                        work[tail++] = succ;
                    }
                }
            }
        }
        queued[i] = 0;
    }
    free(in); free(queued); free(work);
    return entry_reads;
}

static uint8_t lf_exit_may_writes(const LfFlagInsn *d, int n)
{
    uint8_t *in, *queued, exit_may = 0;
    int *work, any_exit = 0;
    size_t head = 0, tail = 0, work_cap;
    if (n <= 0) return LF_FLAG_ALL;
    work_cap = (size_t)n * 8 + 8;
    in = (uint8_t *)calloc((size_t)n, 1);
    queued = (uint8_t *)calloc((size_t)n, 1);
    work = (int *)malloc(work_cap * sizeof(int));
    if (!in || !queued || !work) {
        free(in); free(queued); free(work);
        return LF_FLAG_ALL;
    }
    in[0] = 0;
    work[tail++] = 0;
    queued[0] = 1;
    while (head < tail) {
        int i = work[head++];
        uint8_t out = (uint8_t)(in[i] | d[i].writes);
        int s;
        /* Every path that leaves the function counts, not only a direct ret.
         * An external tail jump, hlt/iret or an unresolved branch is modelled
         * as a barrier with writes=ALL and must=0 by lf_summary_cfg(); if it
         * were ignored whenever another path has a ret, the caller would
         * wrongly assume that this exit preserves or rewrites a flag. */
        if (d[i].succ1 == -1 || d[i].succ2 == -1 ||
            (d[i].succ1 < 0 && d[i].succ2 < 0)) {
            exit_may |= out;
            any_exit = 1;
        }
        for (s = 0; s < 2; s++) {
            int succ = s == 0 ? d[i].succ1 : d[i].succ2;
            if (succ >= 0 && succ < n) {
                uint8_t ni = (uint8_t)(in[succ] | out);
                if (ni != in[succ]) {
                    in[succ] = ni;
                    if (!queued[succ]) {
                        if (tail >= work_cap) {
                            free(in); free(queued); free(work);
                            return LF_FLAG_ALL;
                        }
                        queued[succ] = 1;
                        work[tail++] = succ;
                    }
                }
            }
        }
        queued[i] = 0;
    }
    free(in); free(queued); free(work);
    return any_exit ? exit_may : LF_FLAG_ALL;
}

static uint8_t lf_exit_must_writes(const LfFlagInsn *d, int n)
{
    uint8_t *in, *queued, *valid, exit_must = LF_FLAG_ALL;
    int *work, any_exit = 0;
    size_t head = 0, tail = 0, work_cap;
    if (n <= 0) return 0;
    work_cap = (size_t)n * 8 + 8;
    in = (uint8_t *)calloc((size_t)n, 1);
    queued = (uint8_t *)calloc((size_t)n, 1);
    valid = (uint8_t *)calloc((size_t)n, 1);
    work = (int *)malloc((size_t)work_cap * sizeof(int));
    if (!in || !queued || !valid || !work) {
        free(in); free(queued); free(valid); free(work);
        return 0;
    }
    /* Intersection analysis: a successor starts at TOP and is tightened by
     * every predecessor.  The valid[] bit distinguishes "not visited yet"
     * from a real ALL value, so a first predecessor that writes all flags
     * still schedules the successor. */
    in[0] = 0;
    valid[0] = 1;
    work[tail++] = 0;
    queued[0] = 1;
    while (head < tail) {
        int i = work[head++];
        uint8_t out = (uint8_t)(in[i] | d[i].must_writes);
        int s;
        /* Intersect over every exit path, including external tail jumps and
         * traps.  Such a barrier has must_writes=0, so a flag written only on
         * the ret path is not claimed as a must-write for the whole function;
         * otherwise the caller could drop the pre-call value and observe a
         * stale flag after the barrier's target eventually returns. */
        if (d[i].succ1 == -1 || d[i].succ2 == -1 ||
            (d[i].succ1 < 0 && d[i].succ2 < 0)) {
            exit_must &= out;
            any_exit = 1;
        }
        for (s = 0; s < 2; s++) {
            int succ = s == 0 ? d[i].succ1 : d[i].succ2;
            if (succ >= 0 && succ < n) {
                int first = !valid[succ];
                uint8_t ni = first ? out : (uint8_t)(in[succ] & out);
                if (first || ni != in[succ]) {
                    valid[succ] = 1;
                    in[succ] = ni;
                    if (!queued[succ]) {
                        if (tail >= work_cap) {
                            free(in); free(queued); free(valid); free(work);
                            return 0;
                        }
                        queued[succ] = 1;
                        work[tail++] = succ;
                    }
                }
            }
        }
        queued[i] = 0;
    }
    free(in); free(queued); free(valid); free(work);
    /* No direct ret: the function may leave through a tail jump or never
     * return.  Claiming that it must write flags would let its caller drop
     * pre-call flag values; return "none" instead. */
    return any_exit ? exit_must : 0;
}

static void lf_compute_summary(Lifter *L, const InsnCopy *ins, int n,
                               uint32_t addr, LfFlagSummary *out)
{
    LfFlagInsn *d;
    out->addr = addr;
    out->computed = 0;
    out->queued = 0;
    out->conservative = 1;
    if (n <= 0) {
        out->entry_reads = LF_FLAG_ALL;
        out->exit_may_writes = LF_FLAG_ALL;
        out->exit_must_writes = 0;
        out->exit_flags_needed = LF_FLAG_ALL;
        return;
    }
    d = (LfFlagInsn *)calloc((size_t)n, sizeof *d);
    if (!d) {
        out->entry_reads = LF_FLAG_ALL;
        out->exit_may_writes = LF_FLAG_ALL;
        out->exit_must_writes = 0;
        out->exit_flags_needed = LF_FLAG_ALL;
        return;
    }
    if (lf_summary_cfg(L, ins, n, d)) {
        out->entry_reads = LF_FLAG_ALL;
        out->exit_may_writes = LF_FLAG_ALL;
        out->exit_must_writes = 0;
        out->exit_flags_needed = LF_FLAG_ALL;
        free(d);
        return;
    }
    /* The size in funcs.txt can end before the real terminator (the lifted
     * extent is grown past a jmp to a shared epilogue).  A trailing jmp/jcc
     * is modelled as an external barrier and its needs are propagated to the
     * target, but a non-terminator means the decoded extent is cut in the
     * middle of the function; its continuation is unknown. */
    if (!last_word_is_term(ins[n - 1].insn.mnemonic)) {
        out->entry_reads = LF_FLAG_ALL;
        out->exit_may_writes = LF_FLAG_ALL;
        out->exit_must_writes = 0;
        out->exit_flags_needed = LF_FLAG_ALL;
        out->conservative = 1;
        free(d);
        return;
    }
    out->entry_reads = lf_entry_reads(d, n);
    out->exit_may_writes = lf_exit_may_writes(d, n);
    out->exit_must_writes = lf_exit_must_writes(d, n);
    /* Callers replace this with the propagated N or the global fallback; the
     * default only matters when no summary could be stored, where ALL is the
     * only safe choice (may_writes does not cover AF and some OF forms). */
    out->exit_flags_needed = LF_FLAG_ALL;
    out->conservative = 0;
    free(d);
}

static LfFlagSummary *lf_get_summary(Lifter *L, uint32_t addr)
{
    LfFlagSummary *s;
    LfFlagSummary tmp;
    uint32_t size;
    InsnCopy *ins = NULL;
    int n = 0;
    uint8_t need, queued;
    uint8_t precomputed = (uint8_t)(L->summary_precomputed != 0);

    s = lf_summary_lookup(L, addr);
    if (s && s->computed) return s;
    size = lf_func_size(L, addr);
    if (!size) {
        s = lf_summary_insert(L, addr);
        if (s) {
            queued = s->queued;
            s->entry_reads = LF_FLAG_ALL;
            s->exit_may_writes = LF_FLAG_ALL;
            s->exit_must_writes = 0;
            s->exit_flags_needed = LF_FLAG_ALL;
            s->computed = 1;
            s->conservative = 1;
            s->queued = queued;
        }
        return s;
    }
    if (!lf_decode_extent(L, addr, size, &ins, &n, NULL) || n <= 0) {
        free(ins);
        s = lf_summary_insert(L, addr);
        if (s) {
            queued = s->queued;
            s->entry_reads = LF_FLAG_ALL;
            s->exit_may_writes = LF_FLAG_ALL;
            s->exit_must_writes = 0;
            s->exit_flags_needed = LF_FLAG_ALL;
            s->computed = 1;
            s->conservative = 1;
            s->queued = queued;
        }
        return s;
    }
    lf_compute_summary(L, ins, n, addr, &tmp);
    s = lf_summary_insert(L, addr);
    if (s) {
        need = s->exit_flags_needed;
        queued = s->queued;
        /* During precompute a fresh summary starts with N=0; Phase 2 fixes
         * that after callers have propagated their live_out.  After
         * precompute that would be a hole (the function was skipped because
         * decode or insertion failed), so keep every flag. */
        if (!tmp.conservative)
            tmp.exit_flags_needed = precomputed ? LF_FLAG_ALL : need;
        tmp.computed = 1;
        tmp.queued = queued;
        *s = tmp;
    }
    free(ins);
    return s;
}

void lf_summary_init(Lifter *L, size_t cap)
{
    lf_summary_table_init(L, cap);
}

static int build_flag_plan(Lifter *L, const InsnCopy *ins, int n,
                           const uint32_t *labels, int nlabels,
                           uint8_t **role_out, LfFlagFuse **fuse_out,
                           uint8_t **live_out_out)
{
    LfFlagInsn *d;
    LfFlagFuse *pairs, *fuse;
    uint8_t *role, *live_out;
    int i, nf;

    *role_out = NULL;
    *fuse_out = NULL;
    *live_out_out = NULL;
    if (n <= 0) return 0;
    d = (LfFlagInsn *)calloc((size_t)n, sizeof *d);
    pairs = (LfFlagFuse *)calloc((size_t)n, sizeof *pairs);
    live_out = (uint8_t *)calloc((size_t)n, 1);
    if (!d || !pairs || !live_out) {
        free(d);
        free(pairs);
        free(live_out);
        return -1;
    }
    for (i = 0; i < n; i++) {
        const cs_insn *in = &ins[i].insn;
        const char *m = in->mnemonic;
        int sz = 0;
        uint8_t mask = 0;
        d[i].is_label = (uint8_t)(label_has(labels, nlabels, in->address) != 0);
        d[i].is_control = (uint8_t)flag_is_control(m);
        d[i].is_ret = (uint8_t)is_ret_mnemonic(m);
        d[i].is_call = (uint8_t)!strcmp(m, "call");
        d[i].tail_exact = (uint8_t)L->flag_boundary;
        d[i].consumer = (uint8_t)flag_consumer_of(L, in, &mask);
        d[i].cond_mask = mask;
        d[i].producer = (uint8_t)flag_producer_of(in, &sz);
        d[i].size = (uint8_t)sz;
        flag_use(L, in, labels, nlabels, &d[i].reads, &d[i].writes);

        d[i].succ1 = d[i].succ2 = -2;
        if (!strcmp(m, "jmp")) {
            if (NOPS(in) >= 1 && OP(in, 0)->type == X86_OP_IMM) {
                int t = flag_insn_index(ins, n, (uint64_t)OP(in, 0)->imm);
                d[i].succ1 = t >= 0 ? t : -1;
            } else {
                d[i].succ1 = -1;
            }
        } else if (m[0] == 'j' || !strncmp(m, "loop", 4)) {
            if (NOPS(in) >= 1 && OP(in, 0)->type == X86_OP_IMM) {
                int t = flag_insn_index(ins, n, (uint64_t)OP(in, 0)->imm);
                d[i].succ1 = t >= 0 ? t : -1;
            } else {
                d[i].succ1 = -1;
            }
            d[i].succ2 = (i + 1 < n) ? i + 1 : -1;
        } else if (!strcmp(m, "call")) {
            d[i].succ1 = (i + 1 < n) ? i + 1 : -1;
        } else if (!strcmp(m, "ret") || !strcmp(m, "retn") ||
                   !strcmp(m, "retf") || !strcmp(m, "hlt") ||
                   !strcmp(m, "iret") || !strcmp(m, "iretd")) {
            /* no successor */
        } else if (!strcmp(m, "int3") || !strcmp(m, "int") ||
                   !strcmp(m, "into")) {
            d[i].succ1 = (i + 1 < n) ? i + 1 : -1;
            d[i].succ2 = -1;
        } else if (flag_is_control(m)) {
            d[i].succ1 = -1;
            d[i].succ2 = -1;
        } else {
            d[i].succ1 = (i + 1 < n) ? i + 1 : -1;
        }
    }
    nf = lf_flag_plan_ex(d, n, pairs, n, live_out);
    free(d);
    if (nf < 0) {
        free(pairs);
        free(live_out);
        return -1;
    }
    if (nf == 0) {
        free(pairs);
        *live_out_out = live_out;
        return 0;
    }
    role = (uint8_t *)calloc((size_t)n, 1);
    fuse = (LfFlagFuse *)calloc((size_t)n, sizeof *fuse);
    if (!role || !fuse) {
        free(role);
        free(fuse);
        free(pairs);
        free(live_out);
        return -1;
    }
    for (i = 0; i < nf; i++) {
        int p = pairs[i].producer;
        int c = pairs[i].consumer;
        pairs[i].producer_addr = (uint32_t)ins[p].insn.address;
        role[p] = 1;
        role[c] = 2;
        fuse[p] = pairs[i];
        fuse[c] = pairs[i];
    }
    free(pairs);
    *role_out = role;
    *fuse_out = fuse;
    *live_out_out = live_out;
    return nf;
}

static int u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void lf_summary_enqueue(Lifter *L, uint32_t addr)
{
    LfFlagSummary *s = lf_summary_lookup(L, addr);
    if (!s) s = lf_summary_insert(L, addr);
    if (!s) {
        L->summary_incomplete = 1;
        return;
    }
    if (s->queued) return;
    if (L->summary_qtail == L->summary_qcap) {
        size_t ncap = L->summary_qcap ? L->summary_qcap * 2 : 1024;
        uint32_t *nq = (uint32_t *)realloc(L->summary_queue,
                                           ncap * sizeof(uint32_t));
        if (!nq) {
            L->summary_incomplete = 1;
            return;
        }
        L->summary_queue = nq;
        L->summary_qcap = ncap;
    }
    s->queued = 1;
    L->summary_queue[L->summary_qtail++] = addr;
}

static int lf_summary_add_needed(Lifter *L, uint32_t addr, uint8_t mask)
{
    LfFlagSummary *s = lf_summary_lookup(L, addr);
    if (!s) s = lf_summary_insert(L, addr);
    if (!s) {
        L->summary_incomplete = 1;
        return 0;
    }
    mask = (uint8_t)(mask & LF_FLAG_ALL);
    if ((mask & (uint8_t)~s->exit_flags_needed) == 0) return 0;
    s->exit_flags_needed |= mask;
    /* Targets outside func_map have no lifted ret to update. */
    if (s->computed && lf_func_size(L, addr))
        lf_summary_enqueue(L, addr);
    return 1;
}

/* Re-run one function's liveness with its current exit_flags_needed and push
 * the resulting call/tail needs to the targets.  The initial pass below does
 * the same work on the already-decoded extent; the worklist uses this helper
 * when a function gains a needed flag after it was first visited. */
static void lf_summary_pass(Lifter *L, uint32_t addr)
{
    uint32_t size = lf_func_size(L, addr);
    InsnCopy *ins = NULL;
    int n = 0;
    uint32_t *labels = NULL;
    int nlabels = 0;
    uint8_t *role = NULL;
    LfFlagFuse *fuse = NULL;
    uint8_t *live_out = NULL;
    LfFlagSummary *s;
    LfFlagSummary cur;
    int i;

    if (!size) return;
    if (!lf_decode_extent(L, addr, size, &ins, &n, NULL) || n <= 0) {
        L->summary_incomplete = 1;
        free(ins);
        return;
    }
    labels = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    if (!labels) {
        L->summary_incomplete = 1;
        free(ins);
        return;
    }
    for (i = 0; i < n; i++) labels[i] = (uint32_t)ins[i].insn.address;
    qsort(labels, (size_t)n, sizeof(uint32_t), u32_cmp);
    nlabels = 1;
    for (i = 1; i < n; i++)
        if (labels[i] != labels[nlabels - 1]) labels[nlabels++] = labels[i];

    s = lf_summary_lookup(L, addr);
    if (!s || !s->computed) {
        L->summary_incomplete = 1;
        free(labels); free(ins);
        return;
    }
    cur = *s;
    /* Tail jumps first: N(F) travels through the jump to the target's ret,
     * because the caller of F still needs those flags after the target
     * eventually returns.  flag_use() separately models what the jump itself
     * must keep live (target entry_reads and N(F) the target does not
     * definitely overwrite). */
    if (cur.exit_flags_needed) {
        for (i = 0; i < n; i++) {
            const char *m = ins[i].insn.mnemonic;
            if ((!strcmp(m, "jmp") || m[0] == 'j' ||
                 !strncmp(m, "loop", 4)) &&
                NOPS(&ins[i].insn) >= 1 &&
                OP(&ins[i].insn, 0)->type == X86_OP_IMM) {
                uint32_t target = (uint32_t)OP(&ins[i].insn, 0)->imm;
                if (target != addr && !label_has(labels, nlabels, target) &&
                    lf_func_size(L, target))
                    lf_summary_add_needed(L, target, cur.exit_flags_needed);
            }
        }
    }
    if (n > 0) {
        int save = L->flag_boundary;
        L->flag_boundary = 1;
        L->summary = cur;
        if (build_flag_plan(L, ins, n, labels, nlabels,
                            &role, &fuse, &live_out) < 0) {
            role = NULL; fuse = NULL; live_out = NULL;
            /* Without live_out the calls below keep the callee's old N. */
            L->summary_incomplete = 1;
        }
        L->flag_boundary = save;
    }
    if (live_out) {
        for (i = 0; i < n; i++) {
            const char *m = ins[i].insn.mnemonic;
            if (!strcmp(m, "call") && NOPS(&ins[i].insn) >= 1 &&
                OP(&ins[i].insn, 0)->type == X86_OP_IMM) {
                uint32_t target = (uint32_t)OP(&ins[i].insn, 0)->imm;
                lf_summary_add_needed(L, target, live_out[i]);
            }
        }
    }
    free(role); free(fuse); free(live_out);
    free(labels); free(ins);
}

/* A function whose address appears in the image may be reached by an
 * indirect call/jmp.  Such a caller cannot name the target, so it cannot
 * contribute to exit_flags_needed; the target must keep every flag it may
 * write visible at its ret.  Marking is global (relocation table plus code
 * immediates), not an address denylist. */
static void lf_mark_taken(Lifter *L, uint8_t *taken, uint32_t v)
{
    LfFlagSummary *s;
    int idx;
    if (!taken || v < L->image_lo || v >= L->image_hi) return;
    idx = lf_func_containing(L, v);
    if (idx < 0 || taken[idx]) return;
    taken[idx] = 1;
    s = lf_summary_lookup(L, L->func_map[idx].addr);
    if (s && s->computed)
        lf_summary_add_needed(L, L->func_map[idx].addr, LF_FLAG_ALL);
}

static void lf_scan_addr_taken(Lifter *L, uint8_t *taken,
                               const InsnCopy *ins, int n)
{
    int i, k;
    if (!taken) return;
    for (i = 0; i < n; i++) {
        const cs_insn *in = &ins[i].insn;
        const char *m = in->mnemonic;
        int direct = !strcmp(m, "call") || !strcmp(m, "jmp") ||
                     m[0] == 'j' || !strncmp(m, "loop", 4);
        for (k = 0; k < NOPS(in); k++) {
            const cs_x86_op *op = OP(in, k);
            uint32_t v;
            if (op->type == X86_OP_IMM) {
                if (direct && k == 0) continue; /* direct branch target */
                v = (uint32_t)op->imm;
            } else if (op->type == X86_OP_MEM && !strcmp(m, "lea") &&
                       op->mem.base == 0 && op->mem.index == 0) {
                v = (uint32_t)op->mem.disp;
            } else {
                continue;
            }
            lf_mark_taken(L, taken, v);
        }
    }
}

/* Sound only for the exact func_map handed in: every direct call and tail
 * edge in the corpus has to be represented, otherwise a callee's N omits a
 * later caller's live flags and flag_dead can delete a writer that caller
 * still observes.  Seed/closure rounds pass a partial map for target
 * discovery; closure2's final pass repeats the lift with seed + all closure
 * targets before publishing seed.c/closed.c. */
void lf_precompute_summaries(Lifter *L)
{
    size_t fi;
    uint8_t *addr_taken = NULL;
    uint8_t *phase1_done = NULL;
    if (!L->flag_boundary) {
        L->summary_precomputed = 1;
        return;
    }
    L->summary_qhead = L->summary_qtail = 0;

    addr_taken = (uint8_t *)calloc(L->func_n ? L->func_n : 1, 1);
    if (addr_taken) {
        size_t ri;
        if (!L->nreloc) {
            /* Without relocations a function address can be hidden in any
             * initialized data word; keep every ret conservative. */
            memset(addr_taken, 1, L->func_n);
        } else {
            for (ri = 0; ri < L->nreloc; ri++) {
                uint8_t raw[4];
                if (lf_read_va(L, L->reloc[ri], raw, 4) != 4) continue;
                lf_mark_taken(L, addr_taken, pe_rd32(raw));
            }
        }
    }
    /* Per-function proof that the Phase 1 body reached its end.  A summary
     * can be computed on demand while an earlier caller is processed; that
     * sets computed=1 without propagating this function's call needs or
     * scanning its code immediates. */
    phase1_done = (uint8_t *)calloc(L->func_n ? L->func_n : 1, 1);
    if (!phase1_done) L->summary_incomplete = 1;

    /* Phase 1: entry_reads / exit_may_writes / exit_must_writes.  Calls are
     * modelled as barriers, so these masks do not depend on callee summaries
     * and this pass is order-independent. */
    for (fi = 0; fi < L->func_n; fi++) {
        uint32_t addr = L->func_map[fi].addr;
        uint32_t size = L->func_map[fi].size;
        InsnCopy *ins = NULL;
        int n = 0;
        uint32_t *labels = NULL;
        int nlabels = 0;
        uint8_t *role = NULL;
        LfFlagFuse *fuse = NULL;
        uint8_t *live_out = NULL;
        int i;
        int indirect = 0;
        int have_s = 0;
        uint8_t cur_needed = 0;
        LfFlagSummary tmp;
        LfFlagSummary *s;

        if (!lf_decode_extent(L, addr, size, &ins, &n, NULL) || n <= 0) {
            free(ins);
            continue;
        }
        labels = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
        if (!labels) { free(ins); continue; }
        for (i = 0; i < n; i++) labels[i] = (uint32_t)ins[i].insn.address;
        qsort(labels, (size_t)n, sizeof(uint32_t), u32_cmp);
        nlabels = 1;
        for (i = 1; i < n; i++)
            if (labels[i] != labels[nlabels - 1]) labels[nlabels++] = labels[i];

        for (i = 0; i < n; i++) {
            const char *m = ins[i].insn.mnemonic;
            if ((!strcmp(m, "jmp") || m[0] == 'j' ||
                 !strncmp(m, "loop", 4) || !strcmp(m, "call")) &&
                (NOPS(&ins[i].insn) < 1 ||
                 OP(&ins[i].insn, 0)->type != X86_OP_IMM))
                indirect = 1;
        }
        lf_scan_addr_taken(L, addr_taken, ins, n);

        lf_compute_summary(L, ins, n, addr, &tmp);
        tmp.computed = 1;
        tmp.queued = 0;
        if (indirect || tmp.conservative) {
            tmp.entry_reads = LF_FLAG_ALL;
            tmp.exit_may_writes = LF_FLAG_ALL;
            tmp.exit_must_writes = 0;
            tmp.exit_flags_needed = LF_FLAG_ALL;
            tmp.conservative = 1;
        } else {
            LfFlagSummary *old = lf_summary_lookup(L, addr);
            tmp.exit_flags_needed = old ? old->exit_flags_needed : 0;
            /* An indirect caller cannot add its needs to this function;
             * keep every may-written flag visible at its ret instead. */
            if (!addr_taken || addr_taken[fi])
                tmp.exit_flags_needed = LF_FLAG_ALL;
            tmp.conservative = 0;
        }
        s = lf_summary_insert(L, addr);
        have_s = s != NULL;
        if (s) {
            uint8_t q = s->queued;
            *s = tmp;
            s->queued = q;
            tmp = *s;
            cur_needed = tmp.exit_flags_needed;
        }

        /* Tail jumps first: N(F) travels through the jump to the target's
         * ret, because the caller of F still needs those flags after the
         * target eventually returns.  This may rehash the summary table; tmp
         * and have_s are the only values used below. */
        if (cur_needed) {
            for (i = 0; i < n; i++) {
                const char *m = ins[i].insn.mnemonic;
                if ((!strcmp(m, "jmp") || m[0] == 'j' ||
                     !strncmp(m, "loop", 4)) &&
                    NOPS(&ins[i].insn) >= 1 &&
                    OP(&ins[i].insn, 0)->type == X86_OP_IMM) {
                    uint32_t target = (uint32_t)OP(&ins[i].insn, 0)->imm;
                    if (target != addr && !label_has(labels, nlabels, target) &&
                        lf_func_size(L, target))
                        lf_summary_add_needed(L, target, cur_needed);
                }
            }
        }

        if (n > 0 && have_s) {
            int save = L->flag_boundary;
            L->flag_boundary = 1;
            L->summary = tmp;
            if (build_flag_plan(L, ins, n, labels, nlabels,
                                &role, &fuse, &live_out) < 0) {
                role = NULL; fuse = NULL; live_out = NULL;
                /* No live_out means no call-needs propagation below. */
                L->summary_incomplete = 1;
            }
            L->flag_boundary = save;
        }
        if (live_out) {
            for (i = 0; i < n; i++) {
                const char *m = ins[i].insn.mnemonic;
                if (!strcmp(m, "call") && NOPS(&ins[i].insn) >= 1 &&
                    OP(&ins[i].insn, 0)->type == X86_OP_IMM) {
                    uint32_t target = (uint32_t)OP(&ins[i].insn, 0)->imm;
                    lf_summary_add_needed(L, target, live_out[i]);
                }
            }
        }
        free(role); free(fuse); free(live_out);
        free(labels);
        free(ins);
        if (phase1_done) phase1_done[fi] = 1;
    }

    /* Phase 1 can compute a summary on demand while an earlier caller is
     * processed.  computed=1 then only proves that the masks exist; it does
     * not prove that this function's own pass ran, so its call needs were
     * never propagated and its code immediates were never scanned.  A missed
     * pass can still lift later (lf_lift_function decodes independently), so
     * force the global ALL fallback before the first lift; there is no safe
     * repair after some functions were already emitted.  A persistent decode
     * failure costs precision but not correctness. */
    if (phase1_done) {
        for (fi = 0; fi < L->func_n; fi++) {
            if (!L->func_map[fi].size) continue;
            if (phase1_done[fi]) continue;
            L->summary_incomplete = 1;
            break;
        }
    }

    /* Phase 2: propagate exit_flags_needed backwards.  A function that gains
     * a needed flag must be re-run because its ret may now keep an additional
     * flag alive at calls inside it. */
    while (L->summary_qhead < L->summary_qtail) {
        uint32_t addr = L->summary_queue[L->summary_qhead++];
        LfFlagSummary *s = lf_summary_lookup(L, addr);
        if (!s) {
            L->summary_incomplete = 1;
            continue;
        }
        s->queued = 0;
        if (s->computed) lf_summary_pass(L, addr);
        else L->summary_incomplete = 1;
    }

    L->summary_precomputed = 1;
    free(addr_taken);
    free(phase1_done);
    free(L->summary_queue);
    L->summary_queue = NULL;
    L->summary_qhead = L->summary_qtail = L->summary_qcap = 0;
}
/* ---- optional lifter-level GPR cache (A.4 prototype) ----
 *
 * The generated C reads and writes guest registers through c->eax and friends.
 * Within a straight-line run (no label, branch, call or implicit-clobber
 * instruction) those accesses can be redirected to local variables, with a
 * load at the run start and a flush at the run end.  The cache does not cross
 * barriers, so calls, returns, string ops and other implicit register users
 * observe the canonical c-> fields.  This is the cheap lifter-level half of
 * A.4; the CFG/LIR version is CGP-6.
 */
static const char *const gpr_cache_regs[7] = {
    "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp"
};

static char *gpr_cache_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char **gpr_cache_split(const char *s, size_t *n)
{
    size_t cap = 16, cnt = 0;
    char **v = (char **)malloc(cap * sizeof(char *));
    const char *p = s;
    if (!v) return NULL;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char *line = (char *)malloc(len + 1);
        if (!line) break;
        memcpy(line, p, len);
        line[len] = 0;
        if (cnt == cap) {
            cap *= 2;
            v = (char **)realloc(v, cap * sizeof(char *));
            if (!v) break;
        }
        v[cnt++] = line;
        if (!e) break;
        p = e + 1;
    }
    *n = cnt;
    return v;
}

static char *gpr_cache_join(char **v, size_t n, size_t *len)
{
    size_t total = 1, i, k = 0;
    char *s;
    for (i = 0; i < n; i++) total += strlen(v[i]) + 1;
    s = (char *)malloc(total);
    if (!s) return NULL;
    for (i = 0; i < n; i++) {
        size_t l = strlen(v[i]);
        memcpy(s + k, v[i], l);
        k += l;
        if (i + 1 < n) s[k++] = '\n';
    }
    s[k] = 0;
    *len = k;
    return s;
}

static const char *gpr_cache_find_before(const char *s, const char *pat,
                                         const char *end)
{
    size_t plen = strlen(pat);
    const char *p = s;
    while (p < end && *p) {
        if (!strncmp(p, pat, plen)) return p;
        p++;
    }
    return NULL;
}

static char *gpr_cache_replace_line(const char *line, const int *used)
{
    const char *cm = strstr(line, "/*");
    size_t code_len = cm ? (size_t)(cm - line) : strlen(line);
    size_t cap = strlen(line) + 64, o = 0, i = 0;
    char *out = (char *)malloc(cap);
    int r;
    if (!out) return NULL;
    while (i < code_len) {
        int replaced = 0;
        for (r = 0; r < 7; r++) {
            char pat[16], local[16];
            size_t plen, llen;
            if (!used[r]) continue;
            snprintf(pat, sizeof pat, "c->%s", gpr_cache_regs[r]);
            plen = strlen(pat);
            if (i + plen <= code_len && !strncmp(line + i, pat, plen)) {
                snprintf(local, sizeof local, "gpr_%s", gpr_cache_regs[r]);
                llen = strlen(local);
                if (o + llen + 1 > cap) {
                    cap = cap * 2 + llen + 1;
                    out = (char *)realloc(out, cap);
                    if (!out) return NULL;
                }
                memcpy(out + o, local, llen);
                o += llen;
                i += plen;
                replaced = 1;
                break;
            }
        }
        if (!replaced) {
            if (o + 2 > cap) {
                cap = cap * 2 + 2;
                out = (char *)realloc(out, cap);
                if (!out) return NULL;
            }
            out[o++] = line[i++];
        }
    }
    {
        size_t rest = strlen(line) - code_len;
        if (o + rest + 1 > cap) {
            cap = o + rest + 1;
            out = (char *)realloc(out, cap);
            if (!out) return NULL;
        }
        memcpy(out + o, line + code_len, rest);
        o += rest;
    }
    out[o] = 0;
    return out;
}

static int gpr_cache_clobber_mnemonic(const char *mn)
{
    static const char *const clobber[] = {
        "call", "ret", "retf", "hlt", "iret", "iretw", "iretd", "iretq",
        "mul", "imul", "div", "idiv", "pusha", "popa", "pushad", "popad",
        "cpuid", "rdtsc", "rdtscp", "xlat", "xlatb", "cmpxchg", "cmpxchg8b",
        "cmpxchg16b", "xadd", "movs", "movsb", "movsw", "movsd", "movsq",
        "stos", "stosb", "stosw", "stosd", "stosq", "lods", "lodsb",
        "lodsw", "lodsd", "lodsq", "scas", "scasb", "scasw", "scasd",
        "scasq", "cmps", "cmpsb", "cmpsw", "cmpsd", "cmpsq", "rep",
        "repe", "repz", "repne", "repnz", "ins", "insb", "insw", "insd",
        "outs", "outsb", "outsw", "outsd", "in", "inb", "inw", "ind",
        "out", "outb", "outw", "outd", "int", "int1", "int3", "into",
        "syscall", "sysenter", "sysexit", "sysret", "rdmsr", "wrmsr",
        "rdpmc", "xgetbv", "xsetbv", "xbegin", "xabort", "xend",
        "invlpg", "invpcid", "invd", "wbinvd", "clts", "lmsw", "smsw",
        "ltr", "str", "lldt", "sldt", "verr", "verw", "lar", "lsl",
        "arpl", "clgi", "stgi", "skinit", "vmload", "vmmrun", "vmmsave",
        "vmrun", "invlpga", "rdpkru", "wrpkru", "pconfig", "wrmsrns",
        "urmsr", "umsr", "uiret", "testui", "clui", "stui", "senduipi",
        "hreset", "seamops", "seamret", "seamcall", "tdcall", "tdret",
        "encls", "enclu", "getsec", "vmread", "vmwrite", "vmlaunch",
        "vmresume", "vmxoff", "vmcall", "vmfunc", "ud0", "ud1", "ud2"
    };
    size_t i;
    for (i = 0; i < sizeof(clobber) / sizeof(clobber[0]); i++)
        if (!strcmp(mn, clobber[i])) return 1;
    if (mn[0] == 'j' && mn[1]) return 1;   /* jmp and every jcc */
    if (!strncmp(mn, "loop", 4)) return 1;
    return 0;
}

static int gpr_cache_barrier_line(const char *line)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (p[0] == 'L' && p[1] == '_' && strchr(p, ':')) return 1;
    if (strstr(p, "goto ") || strstr(p, "return") ||
        strstr(p, "dispatch") || strstr(p, "RECOMP_TODO") ||
        strstr(p, "HLE_") || strstr(p, "switch") ||
        strstr(p, "case ") || strstr(p, "default:") ||
        strstr(p, "break;") || strstr(p, "continue;") ||
        strstr(p, "do {") || strstr(p, "while (") ||
        strstr(p, "for (") || strstr(p, "else") ||
        p[0] == '{' || p[0] == '}' || p[0] == '#')
        return 1;
    {
        const char *cm = strstr(p, "/* ");
        if (cm) {
            const char *q = cm + 3;
            while (*q && *q != ':') q++;
            if (*q == ':') q++;
            while (*q == ' ') q++;
            {
                char mn[32];
                int n = 0;
                while (*q && *q != ' ' && *q != '*' && n < 31) mn[n++] = *q++;
                mn[n] = 0;
                if (gpr_cache_clobber_mnemonic(mn)) return 1;
            }
            if (strstr(q, "rep ") || strstr(q, "rep;")) return 1;
        }
    }
    return 0;
}

static int gpr_cache_is_set_write(const char *line, const char *p)
{
    static const char *const sets[] = { "SET8L(", "SET8H(", "SET16(" };
    const char *q = p;
    size_t i;
    while (q > line && (q[-1] == ' ' || q[-1] == '\t')) q--;
    for (i = 0; i < sizeof(sets) / sizeof(sets[0]); i++) {
        size_t len = strlen(sets[i]);
        size_t off = (size_t)(q - line);
        if (off >= len && !strncmp(q - len, sets[i], len)) return 1;
    }
    return 0;
}

static void gpr_cache_scan_reg(const char *line, const char *reg,
                               int *read, int *write, int *count)
{
    char pat[16];
    const char *cm = strstr(line, "/*");
    const char *end = cm ? cm : line + strlen(line);
    const char *p = line;
    size_t plen;
    snprintf(pat, sizeof pat, "c->%s", reg);
    plen = strlen(pat);
    while (p < end && *p) {
        if (!strncmp(p, pat, plen)) {
            const char *q = p + plen;
            int is_write = 0;
            while (*q == ' ' || *q == '\t') q++;
            if (q < end) {
                if (q[0] == '=' && q[1] != '=') is_write = 1;
                else if (q[0] == '+' && (q[1] == '+' || q[1] == '=')) is_write = 1;
                else if (q[0] == '-' && (q[1] == '-' || q[1] == '=')) is_write = 1;
                else if (q[0] == '*' && q[1] == '=') is_write = 1;
                else if (q[0] == '/' && q[1] == '=') is_write = 1;
                else if (q[0] == '%' && q[1] == '=') is_write = 1;
                else if (q[0] == '&' && q[1] == '=') is_write = 1;
                else if (q[0] == '|' && q[1] == '=') is_write = 1;
                else if (q[0] == '^' && q[1] == '=') is_write = 1;
                else if (q[0] == '<' && q[1] == '<' && q[2] == '=') is_write = 1;
                else if (q[0] == '>' && q[1] == '>' && q[2] == '=') is_write = 1;
            }
            if (!is_write && gpr_cache_is_set_write(line, p)) is_write = 1;
            if (is_write) *write = 1;
            else *read = 1;
            *count += 1;
        }
        p++;
    }
}

static void gpr_cache_flush_run(char ***bv, size_t *bn, size_t *cap,
                                char **run, size_t run_n)
{
    int used[7] = {0}, load[7] = {0}, flush[7] = {0}, count[7] = {0};
    size_t i;
    int r;
    if (run_n == 0) return;
    for (i = 0; i < run_n; i++) {
        for (r = 0; r < 7; r++) {
            int rd = 0, wr = 0, cnt = 0;
            gpr_cache_scan_reg(run[i], gpr_cache_regs[r], &rd, &wr, &cnt);
            count[r] += cnt;
            if (rd) load[r] = 1;
            if (wr) flush[r] = 1;
        }
    }
    for (r = 0; r < 7; r++) {
        if (count[r] < 2) {
            used[r] = 0;
            load[r] = 0;
            flush[r] = 0;
        } else {
            used[r] = 1;
        }
    }
    {
        int any = 0;
        for (r = 0; r < 7; r++) if (used[r]) any = 1;
        if (!any) {
            for (i = 0; i < run_n; i++) {
                if (*bn >= *cap) {
                    *cap = *cap * 2 + 16;
                    *bv = (char **)realloc(*bv, *cap * sizeof(char *));
                }
                (*bv)[(*bn)++] = gpr_cache_strdup(run[i]);
            }
            return;
        }
    }
    for (r = 0; r < 7; r++) if (load[r]) {
        char buf[64];
        if (*bn >= *cap) {
            *cap = *cap * 2 + 16;
            *bv = (char **)realloc(*bv, *cap * sizeof(char *));
        }
        snprintf(buf, sizeof buf, "    gpr_%s = c->%s;",
                 gpr_cache_regs[r], gpr_cache_regs[r]);
        (*bv)[(*bn)++] = gpr_cache_strdup(buf);
    }
    for (i = 0; i < run_n; i++) {
        char *nl;
        if (*bn >= *cap) {
            *cap = *cap * 2 + 16;
            *bv = (char **)realloc(*bv, *cap * sizeof(char *));
        }
        nl = gpr_cache_replace_line(run[i], used);
        (*bv)[(*bn)++] = nl;
    }
    for (r = 0; r < 7; r++) if (flush[r]) {
        char buf[64];
        if (*bn >= *cap) {
            *cap = *cap * 2 + 16;
            *bv = (char **)realloc(*bv, *cap * sizeof(char *));
        }
        snprintf(buf, sizeof buf, "    c->%s = gpr_%s;",
                 gpr_cache_regs[r], gpr_cache_regs[r]);
        (*bv)[(*bn)++] = gpr_cache_strdup(buf);
    }
}

static void gpr_cache_apply(char **out, size_t *out_len)
{
    size_t nlines, i, body_start = 0, body_end, bn = 0, bcap, run_n = 0;
    char **lines, **bv, **run, **nv, *ns;
    size_t nn = 0, nlen;
    int used[7] = {0}, r;
    if (!out || !*out) return;
    lines = gpr_cache_split(*out, &nlines);
    if (!lines) return;
    body_end = nlines;
    for (i = 0; i < nlines; i++) {
        if (strchr(lines[i], '{')) { body_start = i + 1; break; }
    }
    for (i = nlines; i > body_start; i--) {
        const char *t = lines[i - 1];
        while (*t == ' ' || *t == '\t') t++;
        if (*t == '}') { body_end = i - 1; break; }
    }
    bcap = (body_end - body_start) + 1;
    if (bcap < 16) bcap = 16;
    bv = (char **)malloc(bcap * sizeof(char *));
    run = (char **)malloc(((body_end - body_start) + 1) * sizeof(char *));
    if (!bv || !run) { free(bv); free(run); return; }
    for (i = body_start; i < body_end; i++) {
        if (gpr_cache_barrier_line(lines[i])) {
            gpr_cache_flush_run(&bv, &bn, &bcap, run, run_n);
            run_n = 0;
            if (bn >= bcap) {
                bcap = bcap * 2 + 16;
                bv = (char **)realloc(bv, bcap * sizeof(char *));
            }
            bv[bn++] = gpr_cache_strdup(lines[i]);
        } else {
            run[run_n++] = lines[i];
        }
    }
    gpr_cache_flush_run(&bv, &bn, &bcap, run, run_n);
    free(run);
    for (i = 0; i < bn; i++) {
        for (r = 0; r < 7; r++) {
            char pat[16];
            snprintf(pat, sizeof pat, "gpr_%s", gpr_cache_regs[r]);
            if (strstr(bv[i], pat)) used[r] = 1;
        }
    }
    nv = (char **)malloc((body_start + bn + 8) * sizeof(char *));
    if (!nv) { free(bv); return; }
    for (i = 0; i < body_start; i++) nv[nn++] = gpr_cache_strdup(lines[i]);
    for (r = 0; r < 7; r++) if (used[r]) {
        char buf[64];
        snprintf(buf, sizeof buf, "    uint32_t gpr_%s;", gpr_cache_regs[r]);
        nv[nn++] = gpr_cache_strdup(buf);
    }
    for (i = 0; i < bn; i++) nv[nn++] = bv[i];
    if (body_end < nlines) nv[nn++] = gpr_cache_strdup(lines[body_end]);
    ns = gpr_cache_join(nv, nn, &nlen);
    for (i = 0; i < nn; i++) free(nv[i]);
    free(nv);
    for (i = 0; i < nlines; i++) free(lines[i]);
    free(lines);
    if (ns) {
        free(*out);
        *out = ns;
        *out_len = nlen;
    }
    free(bv);
}

char *lf_lift_function(Lifter *L, uint32_t start, uint32_t size)
{
    InsnCopy *ins = NULL;
    int n = 0;
    uint32_t end;
    uint32_t *labels = NULL;
    size_t lcap = 0;
    int nlabels = 0;
    char *out;
    size_t out_len = 0, out_cap = 0;
    int i;
    int has_x87 = 0;
    uint8_t *flag_role = NULL;
    LfFlagFuse *flag_fuse = NULL;
    uint8_t *flag_live_out = NULL;

    if (!lf_decode_extent(L, start, size, &ins, &n, &end)) return NULL;

    L->func_start = start;
    L->func_end = end;
    L->njt = 0;

    /* branch targets that are instruction boundaries of this function */
    /* every instruction can contribute a branch label, and one jump table can
     * contribute up to LF_MAX_JT_ENT arms, so size for both. */
    lcap = (size_t)(n ? n : 1) + (size_t)LF_MAX_JT * LF_MAX_JT_ENT + 64;
    labels = (uint32_t *)malloc(sizeof(uint32_t) * lcap);
    for (i = 0; i < n; i++) {
        cs_insn *in = &ins[i].insn;
        int j;
        if (in->mnemonic[0] != 'j' &&
            !LST(in->mnemonic, "loop", "loopne", "loope", "loopz", "loopnz"))
            continue;
        for (j = 0; j < NOPS(in); j++) {
            cs_x86_op *op = OP(in, j);
            if (op->type == X86_OP_IMM) {
                int k, hit = 0;
                for (k = 0; k < n; k++)
                    if ((uint64_t)ins[k].insn.address == (uint64_t)op->imm) { hit = 1; break; }
                if (hit) labels[nlabels++] = (uint32_t)op->imm;
            } else if (op->type == X86_OP_MEM && !strcmp(in->mnemonic, "jmp")) {
                uint32_t d = (uint32_t)op->mem.disp;
                if (op->mem.index && op->mem.scale == 4 && lf_in_image(L, d)) {
                    uint32_t tg[LF_MAX_JT_ENT];
                    int nt = 0, k;
                    for (k = 0; k < LF_MAX_JT_ENT; k++) {
                        uint8_t raw[4];
                        uint32_t v;
                        if (lf_read_va(L, d + 4u * (uint32_t)k, raw, 4) != 4) break;
                        v = pe_rd32(raw);
                        if (!lf_in_image(L, v)) break;
                        tg[nt++] = v;
                    }
                    if (nt) {
                        int a, b, hit;
                        LfJumpTable *jt = &L->jt[L->njt < LF_MAX_JT ? L->njt++ : LF_MAX_JT - 1];
                        jt->ea = (uint32_t)in->address;
                        jt->n = nt;
                        memcpy(jt->t, tg, sizeof(uint32_t) * (size_t)nt);
                        for (a = 0; a < nt; a++) {
                            hit = 0;
                            for (b = 0; b < n; b++)
                                if ((uint64_t)ins[b].insn.address == (uint64_t)tg[a]) { hit = 1; break; }
                            if (hit) labels[nlabels++] = tg[a];
                        }
                    }
                }
            }
        }
    }
    /* sort + unique labels */
    for (i = 1; i < nlabels; i++) {
        uint32_t v = labels[i];
        int j = i - 1;
        while (j >= 0 && labels[j] > v) { labels[j + 1] = labels[j]; j--; }
        labels[j + 1] = v;
    }
    {
        int w = 0;
        for (i = 0; i < nlabels; i++)
            if (!w || labels[w - 1] != labels[i]) labels[w++] = labels[i];
        nlabels = w;
    }

    /* The x87 prologue decision must see every instruction, including the
     * ones decoded while growing the extent: a short seed extent can end
     * before the first fpu instruction, and deciding early left functions
     * using `st`/`x87_top` without declaring them. */
    for (i = 0; i < n; i++)
        if (ins[i].insn.mnemonic[0] == 'f') { has_x87 = 1; break; }

    L->x87_lazy = 0;
    {
        const char *s = getenv("WC3_LIFT_X87_LAZY");
        if (s && strcmp(s, "0")) L->x87_lazy = 1;
    }
    L->flag_boundary = 1;
    {
        const char *s = getenv("WC3_LIFT_FLAG_BOUNDARY");
        if (s && !strcmp(s, "0")) L->flag_boundary = 0;
    }
    L->x87_lazy_safe = 0;
    if (has_x87 && L->x87_lazy)
        L->x87_lazy_safe = x87_lazy_check(L, ins, n, labels, nlabels);
    L->x87_dirty = 0;

    L->summary.addr = 0xffffffffu;
    lf_compute_summary(L, ins, n, start, &L->summary);
    {
        LfFlagSummary *sp = lf_summary_lookup(L, start);
        if (sp && L->summary_precomputed && !L->summary_incomplete)
            L->summary.exit_flags_needed = sp->exit_flags_needed;
        else
            /* No stored summary or a failed/skipped summary step: some
             * callee N may be 0 even though this function's callers need its
             * result.  Keep every flag visible at its ret. */
            L->summary.exit_flags_needed = LF_FLAG_ALL;
        sp = lf_summary_insert(L, start);
        if (sp) {
            uint8_t q = sp->queued;
            *sp = L->summary;
            sp->queued = q;
            sp->computed = 1;
        }
    }
    if (L->flag_fusion && n > 1) {
        if (build_flag_plan(L, ins, n, labels, nlabels,
                            &flag_role, &flag_fuse, &flag_live_out) < 0) {
            flag_role = NULL;
            flag_fuse = NULL;
            flag_live_out = NULL;
        }
    }
    L->flag_ins = ins;
    L->flag_role = flag_role;
    L->flag_fuse = flag_fuse;
    L->flag_live_out = flag_live_out;

    /* ---- emit ---- */
    out_cap = 4096 + (size_t)n * 200;
    out = (char *)malloc(out_cap);
    out[0] = 0;
    {
        char hdr[64];
        int hn = snprintf(hdr, sizeof hdr, "void L_%08X(CPU *restrict c)\n{\n", start);
        memcpy(out, hdr, (size_t)hn);
        out_len = (size_t)hn;
    }
    if (has_x87) {
        append_str(&out, &out_len, &out_cap,
                   "    double st[8]; int x87_base; uint16_t x87_tag;\n"
                   "    VS_LOAD(c, st, x87_tag, x87_base);\n");
        L->x87_top = 0;
        L->x87_dirty = 0;
    }
    for (i = 0; i < n; i++) {
        cs_insn *in = &ins[i].insn;
        Emit em;
        int k;
        int is_branch = (in->mnemonic[0] == 'j') ||
                        !strcmp(in->mnemonic, "call") ||
                        !strcmp(in->mnemonic, "hlt") ||
                        strstr(in->mnemonic, "ret") ||
                        strstr(in->mnemonic, "iret") ||
                        LST(in->mnemonic, "loop", "loope", "loopz", "loopne", "loopnz");
        int is_label = label_has(labels, nlabels, in->address);
        int x87_internal_branch = has_x87 &&
                                  x87_branch_internal(in, labels, nlabels);
        int x87_external_transfer = 0;
        /* MMX aliases the low 64 bits of the same physical st[8].  A mixed
         * x87/MMX function must spill the virtual stack before the MMX
         * instruction and reload it afterwards, or MMX reads stale values and
         * later x87 ops overwrite the MMX result. */
        int is_mmx = has_x87 && lf_is_mmx(L, in);
        if (has_x87) {
            if (L->x87_lazy_safe) {
                if (!strcmp(in->mnemonic, "call") ||
                    !strcmp(in->mnemonic, "ret") ||
                    !strcmp(in->mnemonic, "retn") ||
                    !strcmp(in->mnemonic, "retf") ||
                    !strcmp(in->mnemonic, "hlt") ||
                    !strcmp(in->mnemonic, "iret") ||
                    !strcmp(in->mnemonic, "iretd") ||
                    (is_branch && !x87_internal_branch)) {
                    x87_external_transfer = 1;
                }
                if (x87_external_transfer && L->x87_dirty) {
                    char x87buf[128];
                    snprintf(x87buf, sizeof x87buf,
                             "    VS_FLUSH(c, st, x87_tag, x87_base, %d);\n",
                             L->x87_top);
                    append_str(&out, &out_len, &out_cap, x87buf);
                    L->x87_dirty = 0;
                }
            } else if (is_branch || is_label) {
                char x87buf[128];
                snprintf(x87buf, sizeof x87buf,
                         "    VS_FLUSH(c, st, x87_tag, x87_base, %d);\n", L->x87_top);
                append_str(&out, &out_len, &out_cap, x87buf);
            }
        }
        if (is_label) {
            char lb[32];
            int ln = snprintf(lb, sizeof lb, "L_%08X:\n", (uint32_t)in->address);
            if (out_len + (size_t)ln + 1 > out_cap) { out_cap = out_cap * 2 + 4096; out = realloc(out, out_cap); }
            memcpy(out + out_len, lb, (size_t)ln);
            out_len += (size_t)ln;
            if (has_x87) {
                if (!L->x87_lazy_safe) {
                    append_str(&out, &out_len, &out_cap,
                               "    VS_LOAD(c, st, x87_tag, x87_base);\n");
                    L->x87_top = 0;
                } else {
                    /* A label may be entered from a dirty path.  Treat the
                     * state as dirty so a later real transfer flushes. */
                    L->x87_dirty = 1;
                    L->x87_top = 0;
                }
            }
        }
        if (is_mmx) {
            if (has_x87) {
                if (L->x87_lazy_safe) {
                    if (L->x87_dirty) {
                        char x87buf[128];
                        snprintf(x87buf, sizeof x87buf,
                                 "    VS_FLUSH(c, st, x87_tag, x87_base, %d);\n",
                                 L->x87_top);
                        append_str(&out, &out_len, &out_cap, x87buf);
                        L->x87_dirty = 0;
                    }
                } else {
                    char x87buf[128];
                    snprintf(x87buf, sizeof x87buf,
                             "    VS_FLUSH(c, st, x87_tag, x87_base, %d);\n", L->x87_top);
                    append_str(&out, &out_len, &out_cap, x87buf);
                }
            }
        }
        lf_arena_reset();
        L->flag_current_index = i;
        em = lf_translate(L, in, labels, nlabels);
        for (k = 0; k < em.n; k++) {
            char line[8192];
            int ln = snprintf(line, sizeof line, "    %-60s /* %08X: %s %s */\n",
                              em.v[k], (uint32_t)in->address, in->mnemonic, in->op_str);
            if (ln < 0) continue;
            if (out_len + (size_t)ln + 1 > out_cap) {
                while (out_len + (size_t)ln + 1 > out_cap) out_cap = out_cap * 2 + 4096;
                out = realloc(out, out_cap);
            }
            memcpy(out + out_len, line, (size_t)ln);
            out_len += (size_t)ln;
        }
        if (has_x87) {
            if (!strcmp(in->mnemonic, "call") || is_mmx) {
                append_str(&out, &out_len, &out_cap,
                           "    VS_LOAD(c, st, x87_tag, x87_base);\n");
                L->x87_top = 0;
                L->x87_dirty = 0;
            } else {
                x87_top_after(L, in->mnemonic);
                if (L->x87_lazy_safe && in->mnemonic[0] == 'f' &&
                    !LST(in->mnemonic, "fwait", "fnop")) {
                    L->x87_dirty = 1;
                }
            }
        }
    }
    /* An extent that decoded to nothing at all is still a body with no
     * transfer: Python emits the fall-through with the extent end, and a
     * dispatcher target that is not lifted is a runtime abort, so the empty
     * case has to keep emitting it. */
    if (n == 0 || !last_word_is_term(ins[n - 1].insn.mnemonic)) {
        uint32_t nxt = n ? (uint32_t)(ins[n - 1].insn.address + ins[n - 1].insn.size) : end;
        char tail[160];
        int tn;
        if (has_x87 && (!L->x87_lazy_safe || L->x87_dirty)) {
            char x87buf[128];
            snprintf(x87buf, sizeof x87buf,
                     "    VS_FLUSH(c, st, x87_tag, x87_base, %d);\n", L->x87_top);
            append_str(&out, &out_len, &out_cap, x87buf);
            L->x87_dirty = 0;
        }
        tn = snprintf(tail, sizeof tail,
                      "    /* extent ends mid-function: fall through */\n"
                      "    dispatch_jmp(c, 0x%08Xu); return;\n", nxt);
        if (out_len + (size_t)tn + 1 > out_cap) { out_cap += (size_t)tn + 4096; out = realloc(out, out_cap); }
        memcpy(out + out_len, tail, (size_t)tn);
        out_len += (size_t)tn;
    }
    if (out_len + 3 > out_cap) { out_cap += 16; out = realloc(out, out_cap); }
    out[out_len++] = '}';
    out[out_len] = 0;

    if (L->gpr_cache) gpr_cache_apply(&out, &out_len);

    L->flag_ins = NULL;
    L->flag_current_index = 0;
    L->flag_role = NULL;
    L->flag_fuse = NULL;
    L->flag_live_out = NULL;
    free(flag_role);
    free(flag_fuse);
    free(flag_live_out);
    free(labels);
    free(ins);
    return out;
}
