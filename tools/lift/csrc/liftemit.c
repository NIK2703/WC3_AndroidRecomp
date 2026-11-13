/* liftemit.c - x87 / MMX / SSE translation for the C lifter.
 *
 * Port of the fpu(), mmx() and sse() methods of lift32_cpu.py.  The text is
 * the contract: it is diffed against the Python lifter's output.
 */
#include <stdarg.h>
#include "liftcore.h"

#define NOPS(insn)  ((insn)->detail ? (insn)->detail->x86.op_count : 0)
#define OP(insn, i) (&(insn)->detail->x86.operands[i])

static int in_list(const char *s, const char *const *list, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) if (!strcmp(s, list[i])) return 1;
    return 0;
}
#define LST(s, ...) in_list((s), (const char *const[]){ __VA_ARGS__ }, \
                           sizeof((const char *const[]){ __VA_ARGS__ }) / sizeof(char *))

static int st_index(Lifter *L, cs_x86_op *op)
{
    const char *n = lf_reg_name(L, (unsigned)op->reg);
    const char *p = strchr(n, '(');
    return p ? atoi(p + 1) : 0;
}

static char *fmem(Lifter *L, cs_insn *insn, cs_x86_op *op, char kind)
{
    char *a = lf_addr_expr(L, insn, op);
    int sz = op->size;
    if (kind == 'f') {
        if (sz == 10) return lf_afmt("x87_ld_f80(%s)", a);
        return sz == 4 ? lf_afmt("x87_ld_f32(%s)", a) : lf_afmt("x87_ld_f64(%s)", a);
    }
    if (sz == 2) return lf_afmt("x87_ld_i16(%s)", a);
    if (sz == 4) return lf_afmt("x87_ld_i32(%s)", a);
    return lf_afmt("x87_ld_i64(%s)", a);
}

Emit lf_fpu(Lifter *L, cs_insn *insn)
{
    Emit e = lf_emit();
    const char *m = insn->mnemonic;
    int nops = NOPS(insn);
    cs_x86_op *memop = (nops && OP(insn, 0)->type == X86_OP_MEM) ? OP(insn, 0) : NULL;

    if (LST(m, "fbld", "fbstp")) {
        if (!memop) {
            lf_push(&e, lf_todo((uint32_t)insn->address,
                                lf_afmt("%s without memory: %s", m, insn->op_str)));
            return e;
        }
        if (!strcmp(m, "fbld"))
            lf_push(&e, lf_afmt("VS_PUSH(c, st, x87_tag, %d, x87_ld_bcd(c, %s));",
                                L->x87_top, lf_addr_expr(L, insn, memop)));
        else {
            lf_push(&e, lf_afmt("x87_st_bcd(c, %s, VS_FST(st, %d, 0));",
                                lf_addr_expr(L, insn, memop), L->x87_top));
            lf_push(&e, lf_afmt("VS_POP(c, x87_tag, %d);", L->x87_top));
        }
        return e;
    }
    if (LST(m, "fldenv", "fnstenv", "fsave", "fnsave", "frstor")) {
        if (!memop) {
            lf_push(&e, lf_todo((uint32_t)insn->address,
                                lf_afmt("%s without memory: %s", m, insn->op_str)));
            return e;
        }
        lf_push(&e, lf_afmt("VS_FLUSH(c, st, x87_tag, x87_base, %d);", L->x87_top));
        if (!strcmp(m, "fldenv") || !strcmp(m, "fnstenv"))
            lf_push(&e, lf_afmt("x87_%s(c, %s);", m, lf_addr_expr(L, insn, memop)));
        else if (!strcmp(m, "fsave") || !strcmp(m, "fnsave"))
            lf_push(&e, lf_afmt("x87_fnsave(c, %s);", lf_addr_expr(L, insn, memop)));
        else
            lf_push(&e, lf_afmt("x87_frstor(c, %s);", lf_addr_expr(L, insn, memop)));
        lf_push(&e, "VS_LOAD(c, st, x87_tag, x87_base);");
        return e;
    }

    if (!strcmp(m, "fld")) {
        if (memop) { lf_push(&e, lf_afmt("VS_PUSH(c, st, x87_tag, %d, %s);", L->x87_top, fmem(L, insn, memop, 'f'))); return e; }
        lf_push(&e, lf_afmt("VS_PUSH(c, st, x87_tag, %d, VS_FST(st, %d, %d));",
                            L->x87_top, L->x87_top,
                            nops ? st_index(L, OP(insn, nops - 1)) : 0));
        return e;
    }
    if (!strcmp(m, "fild")) {
        if (!memop) {
            lf_push(&e, lf_todo((uint32_t)insn->address,
                                lf_afmt("fild without memory: %s", insn->op_str)));
            return e;
        }
        lf_push(&e, lf_afmt("VS_PUSH(c, st, x87_tag, %d, %s);", L->x87_top, fmem(L, insn, memop, 'i')));
        return e;
    }
    if (!strcmp(m, "fldz")) { lf_push(&e, lf_afmt("VS_PUSH(c, st, x87_tag, %d, x87_zero());", L->x87_top)); return e; }
    if (!strcmp(m, "fld1")) { lf_push(&e, lf_afmt("VS_PUSH(c, st, x87_tag, %d, x87_one());", L->x87_top)); return e; }
    if (!strcmp(m, "fldpi"))  { lf_push(&e, lf_afmt("VS_PUSH(c, st, x87_tag, %d, x87_pi());", L->x87_top)); return e; }
    if (!strcmp(m, "fldl2e")) { lf_push(&e, lf_afmt("VS_PUSH(c, st, x87_tag, %d, x87_l2e());", L->x87_top)); return e; }
    if (!strcmp(m, "fldl2t")) { lf_push(&e, lf_afmt("VS_PUSH(c, st, x87_tag, %d, x87_l2t());", L->x87_top)); return e; }
    if (!strcmp(m, "fldlg2")) { lf_push(&e, lf_afmt("VS_PUSH(c, st, x87_tag, %d, x87_lg2());", L->x87_top)); return e; }
    if (!strcmp(m, "fldln2")) { lf_push(&e, lf_afmt("VS_PUSH(c, st, x87_tag, %d, x87_ln2());", L->x87_top)); return e; }

    if (!strcmp(m, "fst") || !strcmp(m, "fstp")) {
        const char *pop = !strcmp(m, "fstp")
                              ? lf_afmt(" VS_POP(c, x87_tag, %d);", L->x87_top) : "";
        if (memop) {
            int sz = memop->size;
            char *a = lf_addr_expr(L, insn, memop);
            if (sz == 4)  { lf_push(&e, lf_afmt("x87_st_f32(c, %s, VS_FST(st, %d, 0));%s", a, L->x87_top, pop)); return e; }
            if (sz == 8)  { lf_push(&e, lf_afmt("x87_st_f64(c, %s, VS_FST(st, %d, 0));%s", a, L->x87_top, pop)); return e; }
            if (sz == 10) { lf_push(&e, lf_afmt("x87_st_f80(%s, VS_FST(st, %d, 0));%s", a, L->x87_top, pop)); return e; }
            lf_push(&e, lf_todo((uint32_t)insn->address,
                                lf_afmt("fst m%d: %s", sz * 8, insn->op_str)));
            return e;
        }
        lf_push(&e, lf_afmt("VS_FST_REG(st, x87_tag, %d, %d);%s",
                            L->x87_top,
                            nops ? st_index(L, OP(insn, nops - 1)) : 0, pop));
        return e;
    }
    if (LST(m, "fist", "fistp", "fisttp")) {
        const char *pop = strcmp(m, "fist")
                              ? lf_afmt(" VS_POP(c, x87_tag, %d);", L->x87_top) : "";
        const char *trunc = !strcmp(m, "fisttp") ? "_rz" : "";
        int sz;
        char *a;
        if (!memop) {
            lf_push(&e, lf_todo((uint32_t)insn->address,
                                lf_afmt("%s without memory: %s", m, insn->op_str)));
            return e;
        }
        sz = memop->size;
        a = lf_addr_expr(L, insn, memop);
        if (sz == 2) { lf_push(&e, lf_afmt("x87_st_i16%s(c, %s, VS_FST(st, %d, 0));%s", trunc, a, L->x87_top, pop)); return e; }
        if (sz == 4) { lf_push(&e, lf_afmt("x87_st_i32%s(c, %s, VS_FST(st, %d, 0));%s", trunc, a, L->x87_top, pop)); return e; }
        if (sz == 8) { lf_push(&e, lf_afmt("x87_st_i64%s(c, %s, VS_FST(st, %d, 0));%s", trunc, a, L->x87_top, pop)); return e; }
        lf_push(&e, lf_todo((uint32_t)insn->address, lf_afmt("fist m%d: %s", sz * 8, insn->op_str)));
        return e;
    }

    if (LST(m, "fprem", "fprem1")) {
        lf_push(&e, lf_afmt("VS_FLUSH(c, st, x87_tag, x87_base, %d);", L->x87_top));
        lf_push(&e, lf_afmt("*fst(c, 0) = %s(c, *fst(c, 0), *fst(c, 1));",
                            !strcmp(m, "fprem1") ? "x87_fprem1" : "x87_fprem"));
        lf_push(&e, "VS_LOAD(c, st, x87_tag, x87_base);");
        return e;
    }
    if (!strcmp(m, "frndint")) {
        lf_push(&e, lf_afmt("VS_FLUSH(c, st, x87_tag, x87_base, %d);", L->x87_top));
        lf_push(&e, "*fst(c, 0) = x87_roundint(c, *fst(c, 0));");
        lf_push(&e, "VS_LOAD(c, st, x87_tag, x87_base);");
        return e;
    }
    if (!strcmp(m, "fscale"))  {
        lf_push(&e, lf_afmt("VS_FLUSH(c, st, x87_tag, x87_base, %d);", L->x87_top));
        lf_push(&e, "x87_fscale(c);");
        lf_push(&e, "VS_LOAD(c, st, x87_tag, x87_base);");
        return e;
    }
    if (!strcmp(m, "fxtract")) {
        lf_push(&e, lf_afmt("VS_FLUSH(c, st, x87_tag, x87_base, %d);", L->x87_top));
        lf_push(&e, "x87_fxtract(c);");
        lf_push(&e, "VS_LOAD(c, st, x87_tag, x87_base);");
        return e;
    }

    if (!strncmp(m, "fcmov", 5)) {
        const char *tail = m + 5, *cond = NULL;
        if (!strcmp(tail, "b")) cond = "c->cf";
        else if (!strcmp(tail, "e")) cond = "c->zf";
        else if (!strcmp(tail, "be")) cond = "(c->cf || c->zf)";
        else if (!strcmp(tail, "u")) cond = "c->pf";
        else if (!strcmp(tail, "nb")) cond = "!c->cf";
        else if (!strcmp(tail, "ne")) cond = "!c->zf";
        else if (!strcmp(tail, "nbe")) cond = "(!c->cf && !c->zf)";
        else if (!strcmp(tail, "nu")) cond = "!c->pf";
        if (!cond) {
            lf_push(&e, lf_todo((uint32_t)insn->address, lf_afmt("fcmov form: %s", insn->op_str)));
            return e;
        }
        lf_push(&e, lf_afmt("VS_FCMOV(c, st, x87_tag, %d, (%s), %d);",
                            L->x87_top, cond,
                            nops ? st_index(L, OP(insn, nops - 1)) : 1));
        return e;
    }
    if (!strcmp(m, "fchs")) { lf_push(&e, lf_afmt("VS_FCHS(st, %d);", L->x87_top)); return e; }
    if (!strcmp(m, "fabs")) { lf_push(&e, lf_afmt("VS_FABS(st, %d);", L->x87_top)); return e; }
    if (!strcmp(m, "fxch")) {
        lf_push(&e, lf_afmt("VS_FXCH(st, x87_tag, %d, %d);",
                            L->x87_top,
                            nops ? st_index(L, OP(insn, nops - 1)) : 1));
        return e;
    }
    if (!strcmp(m, "ffree")) {
        /* The plan lists ffree/fdecstp/fincstp as observation points.  The
         * local tag/top are authoritative, but flush first so a guest-visible
         * read after the instruction never sees stale c->st/c->fpu_tag. */
        lf_push(&e, lf_afmt("VS_FLUSH(c, st, x87_tag, x87_base, %d);", L->x87_top));
        lf_push(&e, lf_afmt("VS_FFREE(x87_tag, %d, %d);",
                            L->x87_top,
                            nops ? st_index(L, OP(insn, nops - 1)) : 0));
        return e;
    }
    if (!strcmp(m, "fdecstp")) {
        lf_push(&e, lf_afmt("VS_FLUSH(c, st, x87_tag, x87_base, %d);", L->x87_top));
        lf_push(&e, "/* top updated at lift time */");
        return e;
    }
    if (!strcmp(m, "fincstp")) {
        lf_push(&e, lf_afmt("VS_FLUSH(c, st, x87_tag, x87_base, %d);", L->x87_top));
        lf_push(&e, "/* top updated at lift time */");
        return e;
    }

    if (LST(m, "fadd", "fsub", "fsubr", "fmul", "fdiv", "fdivr",
           "faddp", "fsubp", "fsubrp", "fmulp", "fdivp", "fdivrp")) {
        int pops = m[strlen(m) - 1] == 'p';
        char base[8];
        int rev, a, b;
        const char *fn, *call;
        char *line;
        snprintf(base, sizeof base, "%.*s", (int)(pops ? strlen(m) - 1 : strlen(m)), m);
        rev = !strcmp(base, "fsubr") || !strcmp(base, "fdivr");
        if (!strcmp(base, "fadd") || !strcmp(base, "faddp")) fn = "x87_add";
        else if (!strcmp(base, "fsub") || !strcmp(base, "fsubp") ||
                 !strcmp(base, "fsubr") || !strcmp(base, "fsubrp")) fn = "x87_sub";
        else if (!strcmp(base, "fmul") || !strcmp(base, "fmulp")) fn = "x87_mul";
        else fn = "x87_div";

        if (memop) {
            char *src = fmem(L, insn, memop, 'f');
            if (rev) call = lf_afmt("%s(c, %s, VS_FST(st, %d, 0))", fn, src, L->x87_top);
            else     call = lf_afmt("%s(c, VS_FST(st, %d, 0), %s)", fn, L->x87_top, src);
            lf_push(&e, lf_afmt("VS_FST(st, %d, 0) = %s;", L->x87_top, call));
            return e;
        }
        if (nops == 2) { a = st_index(L, OP(insn, 0)); b = st_index(L, OP(insn, 1)); }
        else if (pops) { a = nops ? st_index(L, OP(insn, 0)) : 1; b = 0; }
        else { a = 0; b = nops ? st_index(L, OP(insn, 0)) : 1; }
        if (rev) call = lf_afmt("%s(c, VS_FST(st, %d, %d), VS_FST(st, %d, %d))",
                                fn, L->x87_top, b, L->x87_top, a);
        else     call = lf_afmt("%s(c, VS_FST(st, %d, %d), VS_FST(st, %d, %d))",
                                fn, L->x87_top, a, L->x87_top, b);
        line = lf_afmt("VS_FST(st, %d, %d) = %s;", L->x87_top, a, call);
        if (pops) line = lf_afmt("%s VS_POP(c, x87_tag, %d);", line, L->x87_top);
        lf_push(&e, line);
        return e;
    }

    if (LST(m, "fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr")) {
        char core[8];
        const char *fn, *call;
        char *src;
        int rev;
        if (!memop) {
            lf_push(&e, lf_todo((uint32_t)insn->address,
                                lf_afmt("%s without memory: %s", m, insn->op_str)));
            return e;
        }
        src = fmem(L, insn, memop, 'i');
        snprintf(core, sizeof core, "f%s", m + 2);
        rev = !strcmp(core, "fsubr") || !strcmp(core, "fdivr");
        if (!strcmp(core, "fadd")) fn = "x87_add";
        else if (!strcmp(core, "fsub") || !strcmp(core, "fsubr")) fn = "x87_sub";
        else if (!strcmp(core, "fmul")) fn = "x87_mul";
        else fn = "x87_div";
        if (rev) call = lf_afmt("%s(c, %s, VS_FST(st, %d, 0))", fn, src, L->x87_top);
        else     call = lf_afmt("%s(c, VS_FST(st, %d, 0), %s)", fn, L->x87_top, src);
        lf_push(&e, lf_afmt("VS_FST(st, %d, 0) = %s;", L->x87_top, call));
        return e;
    }

    if (LST(m, "fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp", "ficom", "ficomp")) {
        char *src;
        int signaling = !strncmp(m, "fcom", 4) || !strncmp(m, "ficom", 5);
        if (memop) src = fmem(L, insn, memop, (m[1] == 'c') ? 'f' : 'i');
        else if (nops) src = lf_afmt("VS_FST(st, %d, %d)", L->x87_top, st_index(L, OP(insn, nops - 1)));
        else src = lf_afmt("VS_FST(st, %d, 1)", L->x87_top);
        lf_push(&e, lf_afmt("x87_compare(c, VS_FST(st, %d, 0), %s, %d);",
                            L->x87_top, src, signaling));
        if (!strcmp(m, "fcomp") || !strcmp(m, "fucomp") || !strcmp(m, "ficomp"))
            lf_push(&e, lf_afmt("VS_POP(c, x87_tag, %d);", L->x87_top));
        if (!strcmp(m, "fcompp") || !strcmp(m, "fucompp")) {
            lf_push(&e, lf_afmt("VS_POP(c, x87_tag, %d);", L->x87_top));
            lf_push(&e, lf_afmt("VS_POP(c, x87_tag, %d);", (L->x87_top + 1) & 7));
        }
        return e;
    }
    if (LST(m, "fcomi", "fucomi", "fcomip", "fucomip", "fcompi", "fucompi")) {
        char *src = nops ? lf_afmt("VS_FST(st, %d, %d)", L->x87_top, st_index(L, OP(insn, nops - 1)))
                         : lf_afmt("VS_FST(st, %d, 1)", L->x87_top);
        int signaling = !strncmp(m, "fcom", 4);
        lf_push(&e, lf_afmt("x87_compare_eflags(c, VS_FST(st, %d, 0), %s, %d);",
                            L->x87_top, src, signaling));
        if (strcmp(m, "fcomi") && strcmp(m, "fucomi"))
            lf_push(&e, lf_afmt("VS_POP(c, x87_tag, %d);", L->x87_top));
        return e;
    }
    if (!strcmp(m, "ftst")) {
        lf_push(&e, lf_afmt("x87_compare(c, VS_FST(st, %d, 0), x87_zero(), 1);", L->x87_top));
        return e;
    }

    if (!strcmp(m, "fsqrt")) {
        lf_push(&e, lf_afmt("VS_FST(st, %d, 0) = x87_sqrt(c, VS_FST(st, %d, 0));",
                            L->x87_top, L->x87_top));
        return e;
    }
    if (!strcmp(m, "fsin"))  {
        lf_push(&e, lf_afmt("VS_FST(st, %d, 0) = x87_fsin(c, VS_FST(st, %d, 0));",
                            L->x87_top, L->x87_top));
        return e;
    }
    if (!strcmp(m, "fcos"))  {
        lf_push(&e, lf_afmt("VS_FST(st, %d, 0) = x87_fcos(c, VS_FST(st, %d, 0));",
                            L->x87_top, L->x87_top));
        return e;
    }
    if (!strcmp(m, "f2xm1")) {
        lf_push(&e, lf_afmt("VS_FST(st, %d, 0) = x87_f2xm1(c, VS_FST(st, %d, 0));",
                            L->x87_top, L->x87_top));
        return e;
    }
    if (LST(m, "fptan", "fpatan", "fyl2x", "fyl2xp1", "fsincos")) {
        lf_push(&e, lf_afmt("VS_FLUSH(c, st, x87_tag, x87_base, %d);", L->x87_top));
        if (!strcmp(m, "fptan")) lf_push(&e, "*fst(c, 0) = x87_fptan(c, *fst(c, 0)); fpush(c, x87_one());");
        else if (!strcmp(m, "fpatan")) lf_push(&e, "*fst(c, 1) = x87_fpatan(c, *fst(c, 1), *fst(c, 0)); fpop(c);");
        else if (!strcmp(m, "fyl2x")) lf_push(&e, "*fst(c, 1) = x87_fyl2x(c, *fst(c, 0), *fst(c, 1)); fpop(c);");
        else if (!strcmp(m, "fyl2xp1")) lf_push(&e, "*fst(c, 1) = x87_fyl2xp1(c, *fst(c, 0), *fst(c, 1)); fpop(c);");
        else lf_push(&e, "{ double _x = *fst(c, 0); *fst(c, 0) = x87_fsin(c, _x); fpush(c, x87_fcos(c, _x)); }");
        lf_push(&e, "VS_LOAD(c, st, x87_tag, x87_base);");
        return e;
    }

    if (!strcmp(m, "fnstsw")) {
        lf_push(&e, lf_afmt("VS_FLUSH(c, st, x87_tag, x87_base, %d);", L->x87_top));
        if (nops && OP(insn, 0)->type == X86_OP_REG) {
            lf_push(&e, "SET16(c->eax, (uint16_t)c->fpu_sw);");
            return e;
        }
        lf_push(&e, lf_wr(L, insn, OP(insn, 0), "(uint16_t)c->fpu_sw"));
        return e;
    }
    if (!strcmp(m, "fnstcw")) {
        lf_push(&e, lf_afmt("VS_FLUSH(c, st, x87_tag, x87_base, %d);", L->x87_top));
        lf_push(&e, lf_wr(L, insn, OP(insn, 0), "c->fpu_cw"));
        return e;
    }
    if (!strcmp(m, "fldcw")) {
        if (!memop) {
            lf_push(&e, lf_todo((uint32_t)insn->address,
                                lf_afmt("fldcw without memory: %s", insn->op_str)));
            return e;
        }
        lf_push(&e, lf_afmt("x87_fldcw(c, %s);", lf_addr_expr(L, insn, memop)));
        return e;
    }
    if (LST(m, "fnclex", "fclex")) {
        lf_push(&e, "x87_fnclex(c);");
        return e;
    }
    if (LST(m, "fninit", "finit")) {
        lf_push(&e, "cpu_x87_init(c);");
        lf_push(&e, "VS_LOAD(c, st, x87_tag, x87_base);");
        return e;
    }
    if (LST(m, "fwait", "wait", "fnop")) { lf_push(&e, "/* no-op */"); return e; }
    if (!strcmp(m, "fxam")) {
        lf_push(&e, lf_afmt("VS_FLUSH(c, st, x87_tag, x87_base, %d);", L->x87_top));
        lf_push(&e, "x87_fxam(c);");
        return e;
    }

    lf_push(&e, lf_todo((uint32_t)insn->address, lf_afmt("fpu %s %s", m, insn->op_str)));
    return e;
}

/* ------------------------------------------------------------------- MMX */
static const struct { const char *m, *tpl; } MMX_BIN[] = {
    {"pand","({a} & {b})"}, {"pandn","(~{a} & {b})"}, {"por","({a} | {b})"},
    {"pxor","({a} ^ {b})"},
    {"paddw","mmx_paddw({a}, {b})"}, {"paddd","mmx_paddd({a}, {b})"},
    {"paddsw","mmx_paddsw({a}, {b})"},
    {"psubw","mmx_psubw({a}, {b})"}, {"psubd","mmx_psubd({a}, {b})"},
    {"psubsw","mmx_psubsw({a}, {b})"},
    {"pmulhw","mmx_pmulhw({a}, {b})"}, {"pmullw","mmx_pmullw({a}, {b})"},
    {"pmaddwd","mmx_pmaddwd({a}, {b})"},
    {"punpcklwd","mmx_punpcklwd({a}, {b})"}, {"punpckhwd","mmx_punpckhwd({a}, {b})"},
    {"punpcklbw","mmx_punpcklbw({a}, {b})"}, {"punpckhbw","mmx_punpckhbw({a}, {b})"},
    {"punpckldq","mmx_punpckldq({a}, {b})"}, {"punpckhdq","mmx_punpckhdq({a}, {b})"},
    {"packssdw","mmx_packssdw({a}, {b})"}, {"packuswb","mmx_packuswb({a}, {b})"},
    {"pcmpeqw","mmx_pcmpeqw({a}, {b})"}, {"pcmpgtw","mmx_pcmpgtw({a}, {b})"},
    {"pcmpeqd","mmx_pcmpeqd({a}, {b})"}, {"pcmpgtd","mmx_pcmpgtd({a}, {b})"},
    {"paddb","mmx_paddb({a}, {b})"}, {"psubb","mmx_psubb({a}, {b})"},
    {"paddsb","mmx_paddsb({a}, {b})"}, {"psubsb","mmx_psubsb({a}, {b})"},
    {"paddusb","mmx_paddusb({a}, {b})"}, {"psubusb","mmx_psubusb({a}, {b})"},
    {"paddusw","mmx_paddusw({a}, {b})"}, {"psubusw","mmx_psubusw({a}, {b})"},
    {"pcmpeqb","mmx_pcmpeqb({a}, {b})"}, {"pcmpgtb","mmx_pcmpgtb({a}, {b})"},
    {"packsswb","mmx_packsswb({a}, {b})"},
};
static const struct { const char *m, *fn; } MMX_SHIFT[] = {
    {"psllq","mmx_psllq"}, {"psrlq","mmx_psrlq"},
    {"psllw","mmx_psllw"}, {"psrlw","mmx_psrlw"}, {"psraw","mmx_psraw"},
    {"pslld","mmx_pslld"}, {"psrld","mmx_psrld"}, {"psrad","mmx_psrad"},
};

int lf_is_mmx(Lifter *L, cs_insn *insn)
{
    int i, n = NOPS(insn);
    if (!strcmp(insn->mnemonic, "emms")) return 1;
    for (i = 0; i < n; i++) if (lf_is_mmx_op(L, OP(insn, i))) return 1;
    return 0;
}

static char *mmx_tpl(const char *tpl, const char *a, const char *b)
{
    char buf[512];
    const char *p = tpl;
    size_t o = 0;
    while (*p && o + 1 < sizeof buf) {
        if (p[0] == '{' && p[1] == 'a' && p[2] == '}') { o += (size_t)snprintf(buf + o, sizeof buf - o, "%s", a); p += 3; }
        else if (p[0] == '{' && p[1] == 'b' && p[2] == '}') { o += (size_t)snprintf(buf + o, sizeof buf - o, "%s", b); p += 3; }
        else buf[o++] = *p++;
    }
    buf[o] = 0;
    return lf_afmt("%s", buf);
}

Emit lf_mmx(Lifter *L, cs_insn *insn)
{
    Emit e = lf_emit();
    const char *m = insn->mnemonic;
    int nops = NOPS(insn);
    size_t i;

    for (i = 0; i < sizeof MMX_BIN / sizeof MMX_BIN[0]; i++) {
        if (!strcmp(m, MMX_BIN[i].m)) {
            char *expr = mmx_tpl(MMX_BIN[i].tpl, lf_mm_read(L, insn, OP(insn, 0)),
                                 lf_mm_read(L, insn, OP(insn, 1)));
            lf_push(&e, lf_mm_write(L, insn, OP(insn, 0), expr));
            return e;
        }
    }
    for (i = 0; i < sizeof MMX_SHIFT / sizeof MMX_SHIFT[0]; i++) {
        if (!strcmp(m, MMX_SHIFT[i].m)) {
            char *expr = lf_afmt("%s(%s, %s)", MMX_SHIFT[i].fn,
                                 lf_mm_read(L, insn, OP(insn, 0)),
                                 lf_mm_count(L, insn, OP(insn, 1)));
            lf_push(&e, lf_mm_write(L, insn, OP(insn, 0), expr));
            return e;
        }
    }
    if (!strcmp(m, "movq") || !strcmp(m, "movntq")) {
        lf_push(&e, lf_mm_write(L, insn, OP(insn, 0), lf_mm_read(L, insn, OP(insn, 1))));
        return e;
    }
    if (!strcmp(m, "movd")) {
        if (lf_is_mmx_op(L, OP(insn, 0))) {
            int k = lf_mm_index(L, OP(insn, 0));
            lf_push(&e, lf_afmt("*mm_get(c, %d) = (uint64_t)(uint32_t)(%s);", k,
                                lf_src(L, insn, OP(insn, 1))));
            lf_push(&e, lf_afmt("mmx_mark(c, %d);", k));
            return e;
        }
        lf_push(&e, lf_dst_write(L, insn, OP(insn, 0),
                                 lf_afmt("(uint32_t)(%s)", lf_mm_read(L, insn, OP(insn, 1)))));
        return e;
    }
    if (!strcmp(m, "pshufw")) {
        int imm = nops > 2 ? (int)(OP(insn, 2)->imm & 0xFF) : 0;
        char *expr = lf_afmt("mmx_pshufw(%s, %du)",
                             lf_mm_read(L, insn, OP(insn, 1)), imm);
        lf_push(&e, lf_mm_write(L, insn, OP(insn, 0), expr));
        return e;
    }
    if (!strcmp(m, "pmovmskb")) {
        int k = lf_mm_index(L, OP(insn, 1));
        lf_push(&e, lf_afmt("{ uint32_t _m = 0; for (int _i = 0; _i < 8; _i++) "
                            "_m |= (uint32_t)(((*mm_get(c, %d)) >> (_i * 8 + 7)) & 1u) << _i;", k));
        lf_push(&e, lf_afmt("%s }", lf_dst_write(L, insn, OP(insn, 0), "_m")));
        return e;
    }
    if (!strcmp(m, "emms")) { lf_push(&e, "mmx_emms(c);"); return e; }

    lf_push(&e, lf_todo((uint32_t)insn->address, lf_afmt("mmx %s %s", m, insn->op_str)));
    return e;
}

/* ------------------------------------------------------------------- SSE */
static const char *SSE_MOV128[] = {
    "movaps","movups","movapd","movupd","movdqa","movdqu",
    "movntps","movntpd","movntdq"
};
static const char *SSE_PINT_NAMES[] = {
    "paddb","paddw","paddd","paddq","psubb","psubw","psubd","psubq"
};
static const char *SSE_PSAT_NAMES[] = {
    "paddsb","paddsw","paddusb","paddusw","psubsb","psubsw","psubusb","psubusw"
};
static const char *SSE_PCMP_NAMES[] = {
    "pcmpeqb","pcmpeqw","pcmpeqd","pcmpgtb","pcmpgtw","pcmpgtd"
};
static const char *SSE_PACK_NAMES[] = { "packsswb","packuswb","packssdw" };
static const char *SSE_PUNPCK_NAMES[] = {
    "punpcklbw","punpcklwd","punpckldq","punpcklqdq",
    "punpckhbw","punpckhwd","punpckhdq","punpckhqdq"
};
static const char *SSE_BITWISE_NAMES[] = {
    "andps","andpd","pand","orps","orpd","por","xorps","xorpd","pxor",
    "andnps","andnpd","pandn"
};
static const char *SSE_SHIFT_NAMES[] = {
    "psllw","pslld","psllq","psrlw","psrld","psrlq","psraw","psrad"
};
static const char *SSE_MISC[] = {
    /* movsd/movq/movd are in SSE_AMBIG as well: they are only SSE when an xmm
     * operand is present, which is what the ambiguous routing below checks. */
    "movss","movsd","movq","movd","sqrtss","sqrtsd","sqrtps","sqrtpd",
    "shufps","shufpd","pshufd","pshuflw","pshufhw",
    "unpcklps","unpckhps","unpcklpd","unpckhpd",
    "minps","maxps","minpd","maxpd","minss","maxss","minsd","maxsd",
    "ucomiss","comiss","ucomisd","comisd",
    "cvtsi2ss","cvtsi2sd","cvttss2si","cvttsd2si",
    "cvtss2si","cvtsd2si","cvtss2sd","cvtsd2ss",
    "cvtdq2ps","cvtps2dq","cvttps2dq","cvtps2pd","cvtpd2ps",
    "pextrw","pinsrw","pslldq","psrldq",
    "pmullw","pmulhw","pmaddwd","pmovmskb","movmskps","movmskpd","cvtdq2pd",
    "movlps","movlpd","movhps","movhpd","movlhps","movhlps",
    "addps","subps","mulps","divps","addpd","subpd","mulpd","divpd",
    "addss","subss","mulss","divss","addsd","subsd","mulsd","divsd"
};
static const char *SSE_AMBIG[] = {
    "movsd","movq","movd","pxor","pand","por","pandn",
    "pmullw","pmulhw","pmaddwd","pmovmskb",
    "packsswb","packuswb","packssdw",
    "paddb","paddw","paddd","paddq","psubb","psubw","psubd","psubq",
    "paddsb","paddsw","paddusb","paddusw","psubsb","psubsw","psubusb","psubusw",
    "pcmpeqb","pcmpeqw","pcmpeqd","pcmpgtb","pcmpgtw","pcmpgtd",
    "punpcklbw","punpcklwd","punpckldq","punpckhbw","punpckhwd","punpckhdq"
};

static int is_cmp_sse(const char *m, char *pred, size_t predcap, char *suffix, size_t sufcap)
{
    static const char *preds[] = {"eq","lt","le","unord","neq","nlt","nle","ord"};
    size_t i;
    if (strncmp(m, "cmp", 3)) return 0;
    for (i = 0; i < sizeof preds / sizeof preds[0]; i++) {
        size_t pl = strlen(preds[i]);
        if (!strncmp(m + 3, preds[i], pl)) {
            const char *rest = m + 3 + pl;
            if (!strcmp(rest, "ss") || !strcmp(rest, "sd") ||
                !strcmp(rest, "ps") || !strcmp(rest, "pd")) {
                snprintf(pred, predcap, "%s", preds[i]);
                snprintf(suffix, sufcap, "%s", rest);
                return 1;
            }
        }
    }
    return 0;
}

static const char *cmp_pred_expr(const char *pred)
{
    if (!strcmp(pred, "eq")) return "(_a == _b)";
    if (!strcmp(pred, "lt")) return "(_a < _b)";
    if (!strcmp(pred, "le")) return "(_a <= _b)";
    if (!strcmp(pred, "unord")) return "(_a != _a || _b != _b)";
    if (!strcmp(pred, "neq")) return "!(_a == _b)";
    if (!strcmp(pred, "nlt")) return "!(_a < _b)";
    if (!strcmp(pred, "nle")) return "!(_a <= _b)";
    return "!(_a != _a || _b != _b)";
}

int lf_is_sse(Lifter *L, cs_insn *insn)
{
    const char *m = insn->mnemonic;
    int i, n = NOPS(insn);
    char pred[16], suf[8];

    for (i = 0; i < n; i++) if (lf_is_mmx_op(L, OP(insn, i))) return 0;
    if (!in_list(m, SSE_MISC, sizeof SSE_MISC / sizeof SSE_MISC[0]) &&
        !in_list(m, SSE_MOV128, sizeof SSE_MOV128 / sizeof SSE_MOV128[0]) &&
        !in_list(m, SSE_BITWISE_NAMES, sizeof SSE_BITWISE_NAMES / sizeof SSE_BITWISE_NAMES[0]) &&
        !in_list(m, SSE_PINT_NAMES, sizeof SSE_PINT_NAMES / sizeof SSE_PINT_NAMES[0]) &&
        !in_list(m, SSE_PSAT_NAMES, sizeof SSE_PSAT_NAMES / sizeof SSE_PSAT_NAMES[0]) &&
        !in_list(m, SSE_PCMP_NAMES, sizeof SSE_PCMP_NAMES / sizeof SSE_PCMP_NAMES[0]) &&
        !in_list(m, SSE_PACK_NAMES, sizeof SSE_PACK_NAMES / sizeof SSE_PACK_NAMES[0]) &&
        !in_list(m, SSE_PUNPCK_NAMES, sizeof SSE_PUNPCK_NAMES / sizeof SSE_PUNPCK_NAMES[0]) &&
        !in_list(m, SSE_SHIFT_NAMES, sizeof SSE_SHIFT_NAMES / sizeof SSE_SHIFT_NAMES[0]) &&
        !is_cmp_sse(m, pred, sizeof pred, suf, sizeof suf))
        return 0;
    if (in_list(m, SSE_AMBIG, sizeof SSE_AMBIG / sizeof SSE_AMBIG[0])) {
        for (i = 0; i < n; i++) if (lf_is_xmm(L, OP(insn, i))) return 1;
        return 0;
    }
    return 1;
}

/* SSE_PINT: (op, bits, lanes) */
static int pint_info(const char *m, const char **op, int *bits, int *lanes)
{
    static const struct { const char *m, *op; int bits, lanes; } t[] = {
        {"paddb","+",8,16},{"paddw","+",16,8},{"paddd","+",32,4},{"paddq","+",64,2},
        {"psubb","-",8,16},{"psubw","-",16,8},{"psubd","-",32,4},{"psubq","-",64,2},
    };
    size_t i;
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(m, t[i].m)) { *op = t[i].op; *bits = t[i].bits; *lanes = t[i].lanes; return 1; }
    return 0;
}
static int psat_info(const char *m, const char **op, int *bits, const char **helper)
{
    static const struct { const char *m, *op; int bits; const char *h; } t[] = {
        {"paddsb","+",8,"sse_sat_i8"},{"paddsw","+",16,"sse_sat_i16"},
        {"paddusb","+",8,"sse_sat_u8"},{"paddusw","+",16,"sse_sat_u16"},
        {"psubsb","-",8,"sse_sat_i8"},{"psubsw","-",16,"sse_sat_i16"},
        {"psubusb","-",8,"sse_sat_u8"},{"psubusw","-",16,"sse_sat_u16"},
    };
    size_t i;
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(m, t[i].m)) { *op = t[i].op; *bits = t[i].bits; *helper = t[i].h; return 1; }
    return 0;
}
static int pcmp_info(const char *m, const char **op, int *bits, const char **sgn)
{
    static const struct { const char *m, *op; int bits; const char *s; } t[] = {
        {"pcmpeqb","==",8,"u"},{"pcmpeqw","==",16,"u"},{"pcmpeqd","==",32,"u"},
        {"pcmpgtb",">",8,"i"},{"pcmpgtw",">",16,"i"},{"pcmpgtd",">",32,"i"},
    };
    size_t i;
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(m, t[i].m)) { *op = t[i].op; *bits = t[i].bits; *sgn = t[i].s; return 1; }
    return 0;
}

Emit lf_sse(Lifter *L, cs_insn *insn)
{
    Emit e = lf_emit();
    const char *m = insn->mnemonic;
    int nops = NOPS(insn);
    cs_x86_op *d = OP(insn, 0);
    cs_x86_op *s = nops > 1 ? OP(insn, 1) : NULL;
    char pred[16], suf[8];
    int i;

    /* ---- scalar moves ---- */
    if (!strcmp(m, "movss") || !strcmp(m, "movsd")) {
        int wide = !strcmp(m, "movsd");
        if (lf_is_xmm(L, d) && s && lf_is_xmm(L, s)) {
            lf_push(&e, lf_afmt("%s = %s;", wide ? lf_sd(L, insn, d) : lf_ss(L, insn, d),
                                wide ? lf_sd(L, insn, s) : lf_ss(L, insn, s)));
            return e;
        }
        if (lf_is_xmm(L, d)) {
            lf_push(&e, lf_afmt("sse_load_s%s(&c->xmm[%d], %s);", wide ? "d" : "s",
                                lf_xmm_index(L, d),
                                wide ? lf_sd(L, insn, s) : lf_ss(L, insn, s)));
            return e;
        }
        lf_push(&e, lf_afmt("%s(%s, %s);", wide ? "wrsd" : "wrss",
                            lf_addr_expr(L, insn, d),
                            wide ? lf_sd(L, insn, s) : lf_ss(L, insn, s)));
        return e;
    }

    if (!strcmp(m, "pextrw") || !strcmp(m, "pinsrw")) {
        int idx = nops > 2 ? (int)(OP(insn, 2)->imm & 7) : 0;
        if (!strcmp(m, "pextrw")) {
            lf_push(&e, lf_dst_write(L, insn, d, lf_afmt("c->xmm[%d].u16[%d]",
                                                         lf_xmm_index(L, s), idx)));
            return e;
        }
        lf_push(&e, lf_afmt("c->xmm[%d].u16[%d] = (uint16_t)(%s);",
                            lf_xmm_index(L, d), idx, lf_src(L, insn, s)));
        return e;
    }

    if (!strcmp(m, "unpcklpd") || !strcmp(m, "unpckhpd") ||
        !strcmp(m, "unpcklps") || !strcmp(m, "unpckhps")) {
        int xd = lf_xmm_index(L, d);
        lf_push(&e, lf_afmt("{ XMM _s = %s; sse_%s(&c->xmm[%d], &_s); }",
                            lf_xm(L, insn, s), m, xd));
        return e;
    }

    if (!strcmp(m, "movd")) {
        if (lf_is_xmm(L, d)) {
            int xd = lf_xmm_index(L, d);
            lf_push(&e, lf_afmt("c->xmm[%d].u64[0] = %s; c->xmm[%d].u64[1] = 0;",
                                xd, lf_src(L, insn, s), xd));
            return e;
        }
        lf_push(&e, lf_dst_write(L, insn, d,
                                 lf_afmt("c->xmm[%d].u32[0]", lf_xmm_index(L, s))));
        return e;
    }
    if (!strcmp(m, "movq")) {
        if (lf_is_xmm(L, d) && s && lf_is_xmm(L, s)) {
            int xd = lf_xmm_index(L, d), xs = lf_xmm_index(L, s);
            lf_push(&e, lf_afmt("c->xmm[%d].u64[0] = c->xmm[%d].u64[0]; c->xmm[%d].u64[1] = 0;",
                                xd, xs, xd));
            return e;
        }
        if (lf_is_xmm(L, d)) {
            int xd = lf_xmm_index(L, d);
            lf_push(&e, lf_afmt("c->xmm[%d].u64[0] = rdxm(%s).u64[0]; c->xmm[%d].u64[1] = 0;",
                                xd, lf_addr_expr(L, insn, s), xd));
            return e;
        }
        lf_push(&e, lf_afmt("wrsd(%s, c->xmm[%d].f64[0]);",
                            lf_addr_expr(L, insn, d), lf_xmm_index(L, s)));
        return e;
    }

    if (in_list(m, SSE_MOV128, sizeof SSE_MOV128 / sizeof SSE_MOV128[0])) {
        if (lf_is_xmm(L, d)) {
            lf_push(&e, lf_afmt("c->xmm[%d] = %s;", lf_xmm_index(L, d), lf_xm(L, insn, s)));
            return e;
        }
        lf_push(&e, lf_afmt("wrxm(%s, c->xmm[%d]);",
                            lf_addr_expr(L, insn, d), lf_xmm_index(L, s)));
        return e;
    }

    if (in_list(m, SSE_BITWISE_NAMES, sizeof SSE_BITWISE_NAMES / sizeof SSE_BITWISE_NAMES[0])) {
        const char *fn;
        int n = lf_xmm_index(L, d);
        if (!strcmp(m, "andnps") || !strcmp(m, "andnpd") || !strcmp(m, "pandn"))
            fn = "sse_pandn";
        else if (!strcmp(m, "andps") || !strcmp(m, "andpd") || !strcmp(m, "pand"))
            fn = "sse_pand";
        else if (!strcmp(m, "orps") || !strcmp(m, "orpd") || !strcmp(m, "por"))
            fn = "sse_por";
        else
            fn = "sse_pxor";
        lf_push(&e, lf_afmt("{ XMM _s = %s; %s(&c->xmm[%d], &_s); }",
                            lf_xm(L, insn, s), fn, n));
        return e;
    }

    /* ---- packed integer ---- */
    {
        const char *op;
        int bits, lanes;
        if (pint_info(m, &op, &bits, &lanes)) {
            int xd = lf_xmm_index(L, d);
            lf_push(&e, lf_afmt("{ XMM _s = %s; sse_%s(&c->xmm[%d], &_s); }",
                                lf_xm(L, insn, s), m, xd));
            return e;
        }
    }
    {
        const char *op, *helper;
        int bits;
        if (psat_info(m, &op, &bits, &helper)) {
            int xd = lf_xmm_index(L, d);
            lf_push(&e, lf_afmt("{ XMM _s = %s; sse_%s(&c->xmm[%d], &_s); }",
                                lf_xm(L, insn, s), m, xd));
            return e;
        }
    }
    {
        const char *op, *sgn;
        int bits;
        if (pcmp_info(m, &op, &bits, &sgn)) {
            int xd = lf_xmm_index(L, d);
            lf_push(&e, lf_afmt("{ XMM _s = %s; sse_%s(&c->xmm[%d], &_s); }",
                                lf_xm(L, insn, s), m, xd));
            return e;
        }
    }
    if (!strcmp(m, "pmullw") || !strcmp(m, "pmulhw") || !strcmp(m, "pmaddwd")) {
        int xd = lf_xmm_index(L, d);
        lf_push(&e, lf_afmt("{ XMM _s = %s; sse_%s(&c->xmm[%d], &_s); }",
                            lf_xm(L, insn, s), m, xd));
        return e;
    }
    if (!strcmp(m, "packsswb") || !strcmp(m, "packuswb") || !strcmp(m, "packssdw")) {
        int xd = lf_xmm_index(L, d);
        lf_push(&e, lf_afmt("{ XMM _s = %s; sse_%s(&c->xmm[%d], &_s); }",
                            lf_xm(L, insn, s), m, xd));
        return e;
    }
    if (!strncmp(m, "punpck", 6)) {
        int bits = 0;
        if (!strcmp(m, "punpcklbw")) bits = 1;
        else if (!strcmp(m, "punpcklwd")) bits = 1;
        else if (!strcmp(m, "punpckldq")) bits = 1;
        else if (!strcmp(m, "punpcklqdq")) bits = 1;
        else if (!strcmp(m, "punpckhbw")) bits = 1;
        else if (!strcmp(m, "punpckhwd")) bits = 1;
        else if (!strcmp(m, "punpckhdq")) bits = 1;
        else if (!strcmp(m, "punpckhqdq")) bits = 1;
        if (bits) {
            int xd = lf_xmm_index(L, d);
            lf_push(&e, lf_afmt("{ XMM _s = %s; sse_%s(&c->xmm[%d], &_s); }",
                                lf_xm(L, insn, s), m, xd));
            return e;
        }
    }

    if (!strcmp(m, "pmovmskb") || !strcmp(m, "movmskps") || !strcmp(m, "movmskpd")) {
        const char *fn = !strcmp(m, "pmovmskb") ? "sse_pmovmskb" :
                         !strcmp(m, "movmskps") ? "sse_movmskps" : "sse_movmskpd";
        lf_push(&e, lf_afmt("{ XMM _s = %s; %s }", lf_xm(L, insn, s),
                            lf_dst_write(L, insn, d, lf_afmt("%s(&_s)", fn))));
        return e;
    }

    if (!strcmp(m, "pshuflw") || !strcmp(m, "pshufhw")) {
        int imm = nops > 2 ? (int)OP(insn, 2)->imm : 0;
        int xd = lf_xmm_index(L, d);
        lf_push(&e, lf_afmt("{ XMM _s = %s; sse_%s(&c->xmm[%d], &_s, %du); }",
                            lf_xm(L, insn, s), m, xd, (unsigned)imm));
        return e;
    }

    if (!strcmp(m, "cvtdq2pd")) {
        int xd = lf_xmm_index(L, d);
        lf_push(&e, lf_afmt("{ XMM _s = %s; c->xmm[%d].f64[0] = (double)_s.i32[0];"
                            " c->xmm[%d].f64[1] = (double)_s.i32[1]; }",
                            lf_xm(L, insn, s), xd, xd));
        return e;
    }

    if (!strcmp(m, "movlhps") || !strcmp(m, "movhlps")) {
        int xd = lf_xmm_index(L, d);
        lf_push(&e, lf_afmt("{ XMM _s = %s; sse_%s(&c->xmm[%d], &_s); }",
                            lf_xm(L, insn, s), m, xd));
        return e;
    }

    if (!strcmp(m, "movlps") || !strcmp(m, "movlpd") ||
        !strcmp(m, "movhps") || !strcmp(m, "movhpd")) {
        int lane = m[3] == 'h' ? 1 : 0;
        if (lf_is_xmm(L, d)) {
            lf_push(&e, lf_afmt("c->xmm[%d].u64[%d] = rd64(%s);",
                                lf_xmm_index(L, d), lane, lf_addr_expr(L, insn, s)));
            return e;
        }
        lf_push(&e, lf_afmt("wr64(%s, c->xmm[%d].u64[%d]);",
                            lf_addr_expr(L, insn, d), lf_xmm_index(L, s), lane));
        return e;
    }

    /* ---- packed shifts ---- */
    if (in_list(m, SSE_SHIFT_NAMES, sizeof SSE_SHIFT_NAMES / sizeof SSE_SHIFT_NAMES[0])) {
        int xd = lf_xmm_index(L, d);
        char *cnt;
        if (s->type == X86_OP_IMM)
            cnt = lf_afmt("unsigned _n = %uu;", (unsigned)(s->imm & 0xFF));
        else
            cnt = lf_afmt("XMM _s = %s; unsigned _n = _s.u64[0] > 255u ? 255u : (unsigned)_s.u64[0];",
                          lf_xm(L, insn, s));
        lf_push(&e, lf_afmt("{ %s sse_%s(&c->xmm[%d], _n); }", cnt, m, xd));
        return e;
    }

    if (!strcmp(m, "pslldq") || !strcmp(m, "psrldq")) {
        int xd = lf_xmm_index(L, d);
        int k = (int)(s->imm & 0xFF);
        if (k > 16) k = 16;
        lf_push(&e, lf_afmt("sse_%s(&c->xmm[%d], %du);", m, xd, (unsigned)k));
        return e;
    }

    if (!strcmp(m, "pshufd")) {
        int imm = nops > 2 ? (int)OP(insn, 2)->imm : 0;
        int xd = lf_xmm_index(L, d);
        lf_push(&e, lf_afmt("{ XMM _s = %s; sse_pshufd(&c->xmm[%d], &_s, %du); }",
                            lf_xm(L, insn, s), xd, (unsigned)imm));
        return e;
    }

    if (!strcmp(m, "shufps") || !strcmp(m, "shufpd")) {
        int imm = nops > 2 ? (int)OP(insn, 2)->imm : 0;
        int xd = lf_xmm_index(L, d);
        char *buf = (char *)malloc(4 * 120 + 32);
        size_t o = 0;
        if (!strcmp(m, "shufpd")) {
            o += (size_t)snprintf(buf + o, 4 * 120 + 32 - o,
                "c->xmm[%d].f64[0] = _d.f64[%d]; c->xmm[%d].f64[1] = _s.f64[%d];",
                xd, imm & 1, xd, (imm >> 1) & 1);
        } else {
            for (i = 0; i < 4; i++)
                o += (size_t)snprintf(buf + o, 4 * 120 + 32 - o,
                    "c->xmm[%d].u32[%d] = %s.u32[%d];", xd, i, i < 2 ? "_d" : "_s",
                    (imm >> (2 * i)) & 3);
        }
        lf_push(&e, lf_afmt("{ XMM _s = %s; XMM _d = c->xmm[%d]; %s }", lf_xm(L, insn, s), xd, buf));
        free(buf);
        return e;
    }

    /* ---- packed arithmetic ---- */
    if (m[0] == 'a' || m[0] == 's' || m[0] == 'm' || m[0] == 'd') {
        size_t ml = strlen(m);
        if (ml > 2 && (!strcmp(m + ml - 2, "ps") || !strcmp(m + ml - 2, "pd"))) {
            char opc[8];
            snprintf(opc, sizeof opc, "%.*s", (int)(ml - 2), m);
            if (!strcmp(opc, "add") || !strcmp(opc, "sub") ||
                !strcmp(opc, "mul") || !strcmp(opc, "div")) {
                int wide = !strcmp(m + ml - 2, "pd");
                const char *op = !strcmp(opc, "add") ? "+" : !strcmp(opc, "sub") ? "-" :
                                 !strcmp(opc, "mul") ? "*" : "/";
                const char *lanef = wide ? "f64" : "f32";
                int n = wide ? 2 : 4, xd = lf_xmm_index(L, d);
                char *buf = (char *)malloc((size_t)n * 120 + 32);
                size_t o = 0;
                for (i = 0; i < n; i++)
                    o += (size_t)snprintf(buf + o, (size_t)n * 120 + 32 - o,
                        "c->xmm[%d].%s[%d] = _d.%s[%d] %s _s.%s[%d];",
                        xd, lanef, i, lanef, i, op, lanef, i);
                lf_push(&e, lf_afmt("{ XMM _s = %s; XMM _d = c->xmm[%d]; %s }",
                                    lf_xm(L, insn, s), xd, buf));
                free(buf);
                return e;
            }
        }
        if (!strcmp(m, "sqrtps") || !strcmp(m, "sqrtpd")) {
            int wide = !strcmp(m, "sqrtpd");
            const char *lanef = wide ? "f64" : "f32";
            const char *fn = wide ? "sqrt" : "sqrtf";
            int n = wide ? 2 : 4, xd = lf_xmm_index(L, d);
            char *buf = (char *)malloc((size_t)n * 100 + 32);
            size_t o = 0;
            for (i = 0; i < n; i++)
                o += (size_t)snprintf(buf + o, (size_t)n * 100 + 32 - o,
                    "c->xmm[%d].%s[%d] = %s(_s.%s[%d]);", xd, lanef, i, fn, lanef, i);
            lf_push(&e, lf_afmt("{ XMM _s = %s; %s }", lf_xm(L, insn, s), buf));
            free(buf);
            return e;
        }
        if (!strcmp(m, "minps") || !strcmp(m, "maxps") ||
            !strcmp(m, "minpd") || !strcmp(m, "maxpd")) {
            int wide = m[strlen(m) - 1] == 'd';
            const char *lanef = wide ? "f64" : "f32";
            char fn[16];
            int n = wide ? 2 : 4, xd = lf_xmm_index(L, d);
            char *buf;
            size_t o = 0;
            snprintf(fn, sizeof fn, "sse_%s%s", m[0] == 'm' && m[1] == 'i' ? "min" : "max",
                     wide ? "d" : "f");
            buf = (char *)malloc((size_t)n * 140 + 32);
            for (i = 0; i < n; i++)
                o += (size_t)snprintf(buf + o, (size_t)n * 140 + 32 - o,
                    "c->xmm[%d].%s[%d] = %s(_d.%s[%d], _s.%s[%d]);",
                    xd, lanef, i, fn, lanef, i, lanef, i);
            lf_push(&e, lf_afmt("{ XMM _s = %s; XMM _d = c->xmm[%d]; %s }",
                                lf_xm(L, insn, s), xd, buf));
            free(buf);
            return e;
        }
    }

    /* ---- scalar arithmetic ---- */
    {
        size_t ml = strlen(m);
        if (ml > 2 && (!strcmp(m + ml - 2, "ss") || !strcmp(m + ml - 2, "sd"))) {
            char opc[8];
            snprintf(opc, sizeof opc, "%.*s", (int)(ml - 2), m);
            if (!strcmp(opc, "add") || !strcmp(opc, "sub") ||
                !strcmp(opc, "mul") || !strcmp(opc, "div")) {
                int wide = !strcmp(m + ml - 2, "sd");
                const char *op = !strcmp(opc, "add") ? "+" : !strcmp(opc, "sub") ? "-" :
                                 !strcmp(opc, "mul") ? "*" : "/";
                lf_push(&e, lf_afmt("%s = %s %s %s;",
                                    wide ? lf_sd(L, insn, d) : lf_ss(L, insn, d),
                                    wide ? lf_sd(L, insn, d) : lf_ss(L, insn, d), op,
                                    wide ? lf_sd(L, insn, s) : lf_ss(L, insn, s)));
                return e;
            }
        }
    }
    if (!strcmp(m, "sqrtss") || !strcmp(m, "sqrtsd")) {
        int wide = !strcmp(m, "sqrtsd");
        lf_push(&e, lf_afmt("%s = %s(%s);",
                            wide ? lf_sd(L, insn, d) : lf_ss(L, insn, d),
                            wide ? "sqrt" : "sqrtf",
                            wide ? lf_sd(L, insn, s) : lf_ss(L, insn, s)));
        return e;
    }
    if (!strcmp(m, "minss") || !strcmp(m, "maxss") ||
        !strcmp(m, "minsd") || !strcmp(m, "maxsd")) {
        int wide = m[strlen(m) - 1] == 'd';
        char fn[16];
        snprintf(fn, sizeof fn, "sse_%s%s", m[1] == 'i' ? "min" : "max", wide ? "d" : "f");
        lf_push(&e, lf_afmt("%s = %s(%s, %s);",
                            wide ? lf_sd(L, insn, d) : lf_ss(L, insn, d), fn,
                            wide ? lf_sd(L, insn, d) : lf_ss(L, insn, d),
                            wide ? lf_sd(L, insn, s) : lf_ss(L, insn, s)));
        return e;
    }

    if (!strcmp(m, "ucomiss") || !strcmp(m, "comiss") ||
        !strcmp(m, "ucomisd") || !strcmp(m, "comisd")) {
        int wide = m[strlen(m) - 1] == 'd';
        lf_push(&e, lf_afmt("sse_compare(c, %s, %s);",
                            wide ? lf_sd(L, insn, d) : lf_ss(L, insn, d),
                            wide ? lf_sd(L, insn, s) : lf_ss(L, insn, s)));
        return e;
    }

    if (is_cmp_sse(m, pred, sizeof pred, suf, sizeof suf)) {
        const char *expr = cmp_pred_expr(pred);
        if (!strcmp(suf, "ps") || !strcmp(suf, "pd")) {
            int wide = !strcmp(suf, "pd");
            const char *lanef = wide ? "f64" : "f32";
            const char *ints = wide ? "u64" : "u32";
            const char *ones = wide ? "~(uint64_t)0" : "0xFFFFFFFFu";
            int n = wide ? 2 : 4, xd = lf_xmm_index(L, d);
            char *buf = (char *)malloc((size_t)n * 220 + 32);
            size_t o = 0;
            for (i = 0; i < n; i++)
                o += (size_t)snprintf(buf + o, (size_t)n * 220 + 32 - o,
                    "{ %s _a = _d.%s[%d], _b = _s.%s[%d]; c->xmm[%d].%s[%d] = %s ? %s : 0; }",
                    wide ? "double" : "float", lanef, i, lanef, i, xd, ints, i, expr, ones);
            lf_push(&e, lf_afmt("{ XMM _s = %s; XMM _d = c->xmm[%d]; %s }",
                                lf_xm(L, insn, s), xd, buf));
            free(buf);
            return e;
        }
        {
            int wide = !strcmp(suf, "sd");
            const char *ctype = wide ? "double" : "float";
            const char *lane = wide ? lf_afmt("c->xmm[%d].u64[0]", lf_xmm_index(L, d))
                                    : lf_afmt("c->xmm[%d].u32[0]", lf_xmm_index(L, d));
            const char *ones = wide ? "~(uint64_t)0" : "0xFFFFFFFFu";
            const char *zero = wide ? "(uint64_t)0" : "0u";
            lf_push(&e, lf_afmt("{ %s _a = %s, _b = %s; %s = %s ? %s : %s; }",
                                ctype,
                                wide ? lf_sd(L, insn, d) : lf_ss(L, insn, d),
                                wide ? lf_sd(L, insn, s) : lf_ss(L, insn, s),
                                lane, expr, ones, zero));
            return e;
        }
    }

    if (!strcmp(m, "cvtsi2ss") || !strcmp(m, "cvtsi2sd")) {
        int wide = m[strlen(m) - 1] == 'd';
        lf_push(&e, lf_afmt("%s = (int32_t)(%s);",
                            wide ? lf_sd(L, insn, d) : lf_ss(L, insn, d),
                            lf_src(L, insn, s)));
        return e;
    }
    if (!strcmp(m, "cvttss2si") || !strcmp(m, "cvttsd2si")) {
        int wide = !strncmp(m, "cvttsd", 6);
        lf_push(&e, lf_dst_write(L, insn, d,
                lf_afmt("sse_cvtt_i32(%s)", wide ? lf_sd(L, insn, s) : lf_ss(L, insn, s))));
        return e;
    }
    if (!strcmp(m, "cvtss2si") || !strcmp(m, "cvtsd2si")) {
        int wide = !strncmp(m, "cvtsd", 5);
        lf_push(&e, lf_dst_write(L, insn, d,
                lf_afmt("sse_cvtt_i32(nearbyint(%s))",
                        wide ? lf_sd(L, insn, s) : lf_ss(L, insn, s))));
        return e;
    }
    if (!strcmp(m, "cvtss2sd")) {
        lf_push(&e, lf_afmt("c->xmm[%d].f64[0] = %s;", lf_xmm_index(L, d), lf_ss(L, insn, s)));
        return e;
    }
    if (!strcmp(m, "cvtsd2ss")) {
        lf_push(&e, lf_afmt("c->xmm[%d].f32[0] = (float)(%s);", lf_xmm_index(L, d), lf_sd(L, insn, s)));
        return e;
    }

    if (!strcmp(m, "cvtdq2ps") || !strcmp(m, "cvtps2dq") || !strcmp(m, "cvttps2dq")) {
        int n = lf_xmm_index(L, d);
        char *buf = (char *)malloc(4 * 120 + 32);
        size_t o = 0;
        for (i = 0; i < 4; i++) {
            if (!strcmp(m, "cvtdq2ps"))
                o += (size_t)snprintf(buf + o, 4 * 120 + 32 - o,
                    "c->xmm[%d].f32[%d] = (float)_s.i32[%d];", n, i, i);
            else if (!strcmp(m, "cvttps2dq"))
                o += (size_t)snprintf(buf + o, 4 * 120 + 32 - o,
                    "c->xmm[%d].i32[%d] = sse_cvtt_i32(_s.f32[%d]);", n, i, i);
            else
                o += (size_t)snprintf(buf + o, 4 * 120 + 32 - o,
                    "c->xmm[%d].i32[%d] = sse_cvtt_i32(nearbyintf(_s.f32[%d]));", n, i, i);
        }
        lf_push(&e, lf_afmt("{ XMM _s = %s; %s }", lf_xm(L, insn, s), buf));
        free(buf);
        return e;
    }

    if (!strcmp(m, "cvtps2pd")) {
        int n = lf_xmm_index(L, d);
        if (lf_is_xmm(L, s)) {
            int j = lf_xmm_index(L, s);
            lf_push(&e, lf_afmt("{ float _a = c->xmm[%d].f32[0], _b = c->xmm[%d].f32[1];"
                                " c->xmm[%d].f64[0] = _a; c->xmm[%d].f64[1] = _b; }",
                                j, j, n, n));
            return e;
        }
        lf_push(&e, lf_afmt("{ uint32_t _p = %s; float _a = rdf32(_p), _b = rdf32(_p + 4u);"
                            " c->xmm[%d].f64[0] = _a; c->xmm[%d].f64[1] = _b; }",
                            lf_addr_expr(L, insn, s), n, n));
        return e;
    }

    if (!strcmp(m, "cvtpd2ps")) {
        int n = lf_xmm_index(L, d);
        lf_push(&e, lf_afmt("{ XMM _s = %s; c->xmm[%d].f32[0] = (float)_s.f64[0];"
                            " c->xmm[%d].f32[1] = (float)_s.f64[1];"
                            " c->xmm[%d].i32[2] = 0; c->xmm[%d].i32[3] = 0; }",
                            lf_xm(L, insn, s), n, n, n, n));
        return e;
    }

    lf_push(&e, lf_todo((uint32_t)insn->address, lf_afmt("sse %s %s", m, insn->op_str)));
    return e;
}
