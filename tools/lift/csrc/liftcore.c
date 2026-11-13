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

static char *target_expr(Lifter *L, cs_insn *insn, cs_x86_op *op)
{
    if (op->type == X86_OP_IMM) return lf_afmt("0x%08Xu", (uint32_t)op->imm);
    if (op->type == X86_OP_REG) return lf_src(L, insn, op);
    if (op->type == X86_OP_MEM) return lf_rd(L, insn, op);
    return lf_afmt("0u");
}

/* ------------------------------------------------------------- translate */
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
            lf_push(&e, lf_afmt("{ uint32_t _old = atomic_xadd32(%s, 1u); "
                                "flags_incs(c, _old, 4); }", a));
            return e;
        }
        if (!strcmp(m, "dec")) {
            lf_push(&e, lf_afmt("{ uint32_t _old = atomic_xadd32(%s, 0xFFFFFFFFu); "
                                "flags_decs(c, _old, 4); }", a));
            return e;
        }
        if (!strcmp(m, "add") && nops > 1) {
            char *v = lf_src(L, insn, OP(insn, 1));
            lf_push(&e, lf_afmt("{ uint32_t _old = atomic_xadd32(%s, %s); "
                                "flags_add(c, _old, %s, 4); }", a, v, v));
            return e;
        }
        if (!strcmp(m, "sub") && nops > 1) {
            char *v = lf_src(L, insn, OP(insn, 1));
            lf_push(&e, lf_afmt("{ uint32_t _old = atomic_sub32(%s, %s); "
                                "flags_sub(c, _old, %s, 4); }", a, v, v));
            return e;
        }
        if ((!strcmp(m, "and") || !strcmp(m, "or") || !strcmp(m, "xor")) && nops > 1) {
            char *v = lf_src(L, insn, OP(insn, 1));
            const char *op = !strcmp(m, "and") ? "&" : !strcmp(m, "or") ? "|" : "^";
            lf_push(&e, lf_afmt("{ uint32_t _old = atomic_%s32(%s, %s); "
                                "flags_logicz(c, _old %s %s, 4); }", m, a, v, op, v));
            return e;
        }
        if ((!strcmp(m, "adc") || !strcmp(m, "sbb")) && nops > 1) {
            char *v = lf_src(L, insn, OP(insn, 1));
            lf_push(&e, lf_afmt("{ uint32_t _old = %s(%s, %s + c->cf); "
                                "flags_%s(c, _old, %s, 4); }",
                                !strcmp(m, "adc") ? "atomic_xadd32" : "atomic_sub32",
                                a, v, m, v));
            return e;
        }
        if (!strcmp(m, "neg")) {
            lf_push(&e, lf_afmt("{ uint32_t _old = atomic_neg32(%s); "
                                "flags_sub(c, 0, _old, 4); }", a));
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
        char *idx = lf_src(L, insn, OP(insn, 1));
        if (OP(insn, 0)->type == X86_OP_MEM) {
            if (locked && OP(insn, 0)->size == 4 && strcmp(m, "bt") != 0) {
                const char *fn = !strcmp(m, "bts") ? "atomic_or32" :
                                 !strcmp(m, "btr") ? "atomic_and32" : "atomic_xor32";
                const char *arg = !strcmp(m, "btr") ? "(~_m)" : "_m";
                lf_push(&e, lf_afmt("{ uint32_t _a = (%s) + 4u * (uint32_t)((int32_t)(%s) >> 5); "
                                    "uint32_t _m = 1u << ((int32_t)(%s) & 31); "
                                    "uint32_t _old = %s(_a, %s); "
                                    "c->cf = (_old >> ((int32_t)(%s) & 31)) & 1; }",
                                    lf_addr_expr(L, insn, OP(insn, 0)), idx, idx, fn, arg, idx));
            } else {
                lf_push(&e, lf_afmt("bit_string_op(c, %s, (int32_t)(%s), %d);",
                                    lf_addr_expr(L, insn, OP(insn, 0)), idx, opnum));
            }
        } else {
            lf_push(&e, lf_dst_write(L, insn, OP(insn, 0),
                    lf_afmt("op_bittest(c, %s, %s, %d, %d)",
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
        lf_push(&e, lf_dst_write(L, insn, OP(insn, 0), lf_src(L, insn, OP(insn, 1))));
        return e;
    }
    if (!strcmp(m, "lea")) {
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
        lf_push(&e, lf_afmt("flags_sub(c, %s, %s, %d);",
                            lf_read_dst(L, insn, OP(insn, 0)),
                            lf_src(L, insn, OP(insn, 1)), OP(insn, 0)->size));
        return e;
    }
    if (!strcmp(m, "test")) {
        lf_push(&e, lf_afmt("flags_logicz(c, %s & %s, %d);",
                            lf_read_dst(L, insn, OP(insn, 0)),
                            lf_src(L, insn, OP(insn, 1)), OP(insn, 0)->size));
        return e;
    }
    if (!strcmp(m, "inc")) {
        cs_x86_op *d = OP(insn, 0);
        lf_push(&e, lf_dst_write(L, insn, d,
                lf_afmt("flags_incs(c, %s, %d)", lf_read_dst(L, insn, d), d->size)));
        return e;
    }
    if (!strcmp(m, "dec")) {
        cs_x86_op *d = OP(insn, 0);
        lf_push(&e, lf_dst_write(L, insn, d,
                lf_afmt("flags_decs(c, %s, %d)", lf_read_dst(L, insn, d), d->size)));
        return e;
    }
    if (!strcmp(m, "neg")) {
        cs_x86_op *d = OP(insn, 0);
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
        const char *fn = (!strcmp(m, "shr")) ? "op_shr" : (!strcmp(m, "sar")) ? "op_sar" : "op_shl";
        lf_push(&e, lf_dst_write(L, insn, d,
                lf_afmt("%s(c, %s, %s, %d)", fn, lf_read_dst(L, insn, d), cnt, d->size)));
        return e;
    }
    if (LST(m, "rol", "ror")) {
        cs_x86_op *d = OP(insn, 0);
        char *cnt = nops > 1 ? lf_src(L, insn, OP(insn, 1)) : (char *)"1";
        const char *fn = !strcmp(m, "rol") ? "op_rol" : "op_ror";
        lf_push(&e, lf_dst_write(L, insn, d,
                lf_afmt("%s(c, %s, %s, %d)", fn, lf_read_dst(L, insn, d), cnt, d->size)));
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
        const char *fn = !strcmp(m, "bsf") ? "op_bsf" : "op_bsr";
        lf_push(&e, lf_dst_write(L, insn, d,
                lf_afmt("%s(c, %s, %s, %d)", fn, lf_read_dst(L, insn, d),
                        lf_src(L, insn, OP(insn, 1)), d->size)));
        return e;
    }
    if (LST(m, "rcl", "rcr")) {
        cs_x86_op *d = OP(insn, 0);
        char *cnt = nops > 1 ? lf_src(L, insn, OP(insn, 1)) : (char *)"1";
        const char *fn = !strcmp(m, "rcl") ? "op_rcl" : "op_rcr";
        lf_push(&e, lf_dst_write(L, insn, d,
                lf_afmt("%s(c, %s, %s, %d)", fn, lf_read_dst(L, insn, d), cnt, d->size)));
        return e;
    }
    if (LST(m, "shld", "shrd")) {
        cs_x86_op *d = OP(insn, 0);
        char *cnt = nops > 2 ? lf_src(L, insn, OP(insn, 2)) : (char *)"R8L(c->ecx)";
        const char *fn = !strcmp(m, "shld") ? "op_shld" : "op_shrd";
        lf_push(&e, lf_dst_write(L, insn, d,
                lf_afmt("%s(c, %s, %s, %s, %d)", fn, lf_read_dst(L, insn, d),
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
                lf_push(&e, "  flags_add(c, _old, _v, 4); }");
                return e;
            }
            lf_push(&e, lf_afmt("{ uint32_t _old = atomic_cmpxchg32(%s, c->eax, %s);",
                                a, lf_src(L, insn, s)));
            lf_push(&e, "  flags_sub(c, c->eax, _old, 4);");
            lf_push(&e, "  if (_old != c->eax) c->eax = _old; }");
            return e;
        }
    }
    if (!strcmp(m, "cmpxchg8b") && nops && OP(insn, 0)->type == X86_OP_MEM) {
        char *a = lf_addr_expr(L, insn, OP(insn, 0));
        lf_push(&e, "{ uint64_t _exp = ((uint64_t)c->edx << 32) | c->eax;");
        lf_push(&e, "  uint64_t _des = ((uint64_t)c->ecx << 32) | c->ebx;");
        lf_push(&e, lf_afmt("  uint64_t _old = atomic_cmpxchg64(%s, _exp, _des);", a));
        lf_push(&e, "  if (_old == _exp) c->zf = 1;");
        lf_push(&e, "  else { c->zf = 0; c->eax = (uint32_t)_old;");
        lf_push(&e, "         c->edx = (uint32_t)(_old >> 32); } }");
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
        if (sz == 4)
            lf_push(&e, lf_afmt("{ uint64_t _p=(uint64_t)c->eax*(uint32_t)(%s); c->eax=(uint32_t)_p; c->edx=(uint32_t)(_p>>32); c->cf=c->of=(c->edx!=0); }", v));
        else if (sz == 2)
            lf_push(&e, lf_afmt("{ uint32_t _p=(uint32_t)R16(c->eax)*(uint16_t)(%s); SET16(c->eax,_p); SET16(c->edx,_p>>16); c->cf=c->of=((_p>>16)!=0); }", v));
        else
            lf_push(&e, lf_afmt("{ uint16_t _p=(uint16_t)R8L(c->eax)*(uint8_t)(%s); SET16(c->eax,_p); c->cf=c->of=((_p>>8)!=0); }", v));
        return e;
    }
    if (!strcmp(m, "imul")) {
        if (nops == 1) {
            cs_x86_op *s = OP(insn, 0);
            int sz = s->size;
            char *v = lf_src(L, insn, s);
            if (sz == 4)
                lf_push(&e, lf_afmt("{ int64_t _p=(int64_t)(int32_t)c->eax*(int32_t)(%s); c->eax=(uint32_t)_p; c->edx=(uint32_t)((uint64_t)_p>>32); c->cf=c->of=((int32_t)_p!=_p); }", v));
            else if (sz == 2)
                lf_push(&e, lf_afmt("{ int32_t _p=(int32_t)(int16_t)R16(c->eax)*(int16_t)(%s); SET16(c->eax,_p); SET16(c->edx,_p>>16); c->cf=c->of=((int16_t)_p!=_p); }", v));
            else
                lf_push(&e, lf_afmt("{ int16_t _p=(int16_t)(int8_t)R8L(c->eax)*(int8_t)(%s); SET16(c->eax,_p); c->cf=c->of=((int8_t)_p!=_p); }", v));
            return e;
        }
        {
            cs_x86_op *d = OP(insn, 0);
            int sz = d->size;
            const char *cast = sz == 1 ? "int8_t" : sz == 2 ? "int16_t" : "int32_t";
            char *a, *b, *dw;
            if (nops == 2) { a = lf_read_dst(L, insn, d); b = lf_src(L, insn, OP(insn, 1)); }
            else           { a = lf_src(L, insn, OP(insn, 1)); b = lf_src(L, insn, OP(insn, 2)); }
            dw = strip_semi(lf_dst_write(L, insn, d, "(uint32_t)_p"));
            lf_push(&e, lf_afmt("{ int64_t _p=(int64_t)(%s)(%s)*(%s)(%s); %s; c->cf=c->of=((%s)_p!=_p); }",
                                cast, a, cast, b, dw, cast));
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
                    lf_push(&e, lf_afmt("if (c->ecx) { memset((void *)(uintptr_t)c->edi, "
                                        "(int)(uint8_t)(%s), (size_t)c->ecx); "
                                        "c->edi += c->ecx; c->ecx = 0; }", ar));
                    return e;
                }
                lf_push(&e, lf_afmt("%s(c->edi, %s); c->edi += %d;", wfn, ar, esz));
            } else if (!strncmp(base, "movs", 4)) {
                char *body = lf_afmt("%s(c->edi, %s(c->esi)); c->esi += %d; c->edi += %d;",
                                     wfn, rfn, esz, esz);
                if (rep) {
                    lf_push(&e, lf_afmt("if (c->ecx) { uint64_t _n = (uint64_t)c->ecx * %dull; "
                                        "if (c->esi + _n <= c->edi || c->edi + _n <= c->esi) "
                                        "memcpy((void *)(uintptr_t)c->edi, (void *)(uintptr_t)c->esi, (size_t)_n); "
                                        "else while (c->ecx) { %s c->ecx--; } "
                                        "c->esi += (uint32_t)_n; c->edi += (uint32_t)_n; c->ecx = 0; }",
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
typedef struct {
    cs_insn insn;
    cs_detail det;
} InsnCopy;

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

char *lf_lift_function(Lifter *L, uint32_t start, uint32_t size)
{
    uint8_t *code;
    size_t got;
    InsnCopy *ins = NULL;
    int n = 0, cap = 0;
    uint32_t end;
    uint32_t *labels = NULL;
    size_t lcap = 0;
    int nlabels = 0;
    char *out;
    size_t out_len = 0, out_cap = 0;
    int i;
    int has_x87 = 0;

    code = (uint8_t *)malloc(size ? size : 1);
    if (!code) return NULL;
    got = lf_read_va(L, start, code, size);
    if (!decode_into(L, code, got, start, &ins, &n, &cap)) { free(code); return NULL; }
    free(code);
    end = start + (uint32_t)got;

    /* an extent that ends inside an instruction: read the few more bytes */
    if (n > 0) {
        uint32_t tail = (uint32_t)(ins[n - 1].insn.address + ins[n - 1].insn.size);
        if (tail < end) {
            /* the extent can be kilobytes longer than what capstone decoded
             * (a clamp against a phantom start leaves the tail in data), so the
             * read buffer is sized from the gap, not a fixed array. */
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

    for (i = 0; i < n; i++)
        if (ins[i].insn.mnemonic[0] == 'f') { has_x87 = 1; break; }

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
                size_t pg = lf_read_va(L, (uint32_t)(last->insn.address + last->insn.size), &pad, 1);
                /* In whole-function mode a ret ends it even without padding:
                 * the bytes after it belong to the next function. */
                if (whole && strstr(last->insn.mnemonic, "ret")) break;
                if (!pg || pad == 0xCC || pad == 0x90) break;
                if (strstr(last->insn.mnemonic, "jmp") && strchr(last->insn.op_str, '[')) break;
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
            /* decode_into decodes the whole read buffer linearly, so a batch
             * can run past the function's `ret` into the next one.  In
             * whole-function mode the first `ret` is the end, so cut there. */
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
            if (L->block_mode && last_word_is_term(ins[n - 1].insn.mnemonic)) break;
        }
        if (n > 0) {
            uint32_t ne = (uint32_t)(ins[n - 1].insn.address + ins[n - 1].insn.size);
            if (ne > end) end = ne;
        }
        }
    }

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

    /* ---- emit ---- */
    out_cap = 4096 + (size_t)n * 200;
    out = (char *)malloc(out_cap);
    out[0] = 0;
    {
        char hdr[64];
        int hn = snprintf(hdr, sizeof hdr, "void L_%08X(CPU *c)\n{\n", start);
        memcpy(out, hdr, (size_t)hn);
        out_len = (size_t)hn;
    }
    if (has_x87) {
        append_str(&out, &out_len, &out_cap,
                   "    double st[8]; int x87_top; uint16_t x87_tag;\n"
                   "    vs_load(c, st, &x87_top, &x87_tag);\n");
    }
    for (i = 0; i < n; i++) {
        cs_insn *in = &ins[i].insn;
        Emit em;
        int k;
        int is_branch = (in->mnemonic[0] == 'j') ||
                        !strcmp(in->mnemonic, "call") ||
                        strstr(in->mnemonic, "ret") ||
                        strstr(in->mnemonic, "iret") ||
                        LST(in->mnemonic, "loop", "loope", "loopz", "loopne", "loopnz");
        if (has_x87 && (is_branch || label_has(labels, nlabels, in->address)))
            append_str(&out, &out_len, &out_cap, "    vs_flush(c, st, x87_top, x87_tag);\n");
        if (label_has(labels, nlabels, in->address)) {
            char lb[32];
            int ln = snprintf(lb, sizeof lb, "L_%08X:\n", (uint32_t)in->address);
            if (out_len + (size_t)ln + 1 > out_cap) { out_cap = out_cap * 2 + 4096; out = realloc(out, out_cap); }
            memcpy(out + out_len, lb, (size_t)ln);
            out_len += (size_t)ln;
        }
        lf_arena_reset();
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
        if (has_x87 && !strcmp(in->mnemonic, "call"))
            append_str(&out, &out_len, &out_cap, "    vs_load(c, st, &x87_top, &x87_tag);\n");
    }
    /* An extent that decoded to nothing at all is still a body with no
     * transfer: Python emits the fall-through with the extent end, and a
     * dispatcher target that is not lifted is a runtime abort, so the empty
     * case has to keep emitting it. */
    if (n == 0 || !last_word_is_term(ins[n - 1].insn.mnemonic)) {
        uint32_t nxt = n ? (uint32_t)(ins[n - 1].insn.address + ins[n - 1].insn.size) : end;
        char tail[160];
        int tn;
        if (has_x87)
            append_str(&out, &out_len, &out_cap, "    vs_flush(c, st, x87_top, x87_tag);\n");
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

    free(labels);
    free(ins);
    return out;
}
