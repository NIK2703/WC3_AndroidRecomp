/* liftutil.c - operand rendering and scratch strings for the C lifter.
 *
 * Every function here is a direct port of the same-named method in
 * tools/lift/lift32_cpu.py; the emitted text is compared byte for byte
 * against the Python lifter's output, so formatting is part of the contract.
 */
#include <stdarg.h>
#include "liftcore.h"

/* ------------------------------------------------------------------ arena */
typedef struct ArenaBlk {
    struct ArenaBlk *next;
    size_t used, cap;
    char data[1];
} ArenaBlk;

static ArenaBlk *g_first, *g_cur;

static ArenaBlk *blk_new(size_t cap)
{
    ArenaBlk *b = (ArenaBlk *)malloc(sizeof(ArenaBlk) + cap);
    if (!b) { fprintf(stderr, "[!] out of memory\n"); exit(2); }
    b->next = NULL;
    b->used = 0;
    b->cap = cap;
    return b;
}

void lf_arena_reset(void)
{
    g_cur = g_first;
    if (g_cur) g_cur->used = 0;
}

static void *ar_alloc(size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (!g_first) { g_first = g_cur = blk_new(n > (1u << 20) ? n : (1u << 20)); }
    for (;;) {
        if (g_cur->cap - g_cur->used >= n) {
            void *p = g_cur->data + g_cur->used;
            g_cur->used += n;
            return p;
        }
        if (g_cur->next) { g_cur = g_cur->next; g_cur->used = 0; continue; }
        g_cur->next = blk_new(n > (1u << 20) ? n : (1u << 20));
        g_cur = g_cur->next;
    }
}

char *lf_afmt(const char *fmt, ...)
{
    va_list ap;
    int need;
    char *out;

    va_start(ap, fmt);
    need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) return (char *)"";
    out = (char *)ar_alloc((size_t)need + 1);
    va_start(ap, fmt);
    vsnprintf(out, (size_t)need + 1, fmt, ap);
    va_end(ap);
    return out;
}

/* ------------------------------------------------------------- registers */
const char *lf_reg_name(Lifter *L, unsigned reg)
{
    const char *n = csg.reg_name(L->h, reg);
    return n ? n : "";
}

static const char *r16_of(const char *n)
{
    if (!strcmp(n, "ax")) return "eax";
    if (!strcmp(n, "cx")) return "ecx";
    if (!strcmp(n, "dx")) return "edx";
    if (!strcmp(n, "bx")) return "ebx";
    if (!strcmp(n, "sp")) return "esp";
    if (!strcmp(n, "bp")) return "ebp";
    if (!strcmp(n, "si")) return "esi";
    if (!strcmp(n, "di")) return "edi";
    return NULL;
}

static const char *r8l_of(const char *n)
{
    if (!strcmp(n, "al")) return "eax";
    if (!strcmp(n, "cl")) return "ecx";
    if (!strcmp(n, "dl")) return "edx";
    if (!strcmp(n, "bl")) return "ebx";
    return NULL;
}

static const char *r8h_of(const char *n)
{
    if (!strcmp(n, "ah")) return "eax";
    if (!strcmp(n, "ch")) return "ecx";
    if (!strcmp(n, "dh")) return "edx";
    if (!strcmp(n, "bh")) return "ebx";
    return NULL;
}

static int is_r32(const char *n)
{
    return !strcmp(n, "eax") || !strcmp(n, "ecx") || !strcmp(n, "edx") ||
           !strcmp(n, "ebx") || !strcmp(n, "esp") || !strcmp(n, "ebp") ||
           !strcmp(n, "esi") || !strcmp(n, "edi");
}

static int is_seg(const char *n)
{
    return !strcmp(n, "cs") || !strcmp(n, "ds") || !strcmp(n, "es") ||
           !strcmp(n, "fs") || !strcmp(n, "gs") || !strcmp(n, "ss");
}

static char *reg_read(const char *n)
{
    const char *m;
    if (is_r32(n)) return lf_afmt("c->%s", n);
    if ((m = r16_of(n)) != NULL) return lf_afmt("R16(c->%s)", m);
    if ((m = r8l_of(n)) != NULL) return lf_afmt("R8L(c->%s)", m);
    if ((m = r8h_of(n)) != NULL) return lf_afmt("R8H(c->%s)", m);
    if (is_seg(n)) return lf_afmt("c->%s", n);
    fprintf(stderr, "[!] reg_read %s\n", n);
    return lf_afmt("c->%s", n);
}

static char *reg_write(const char *n, const char *val)
{
    const char *m;
    if (is_r32(n)) return lf_afmt("c->%s = (uint32_t)(%s);", n, val);
    if ((m = r16_of(n)) != NULL) return lf_afmt("SET16(c->%s, %s);", m, val);
    if ((m = r8l_of(n)) != NULL) return lf_afmt("SET8L(c->%s, %s);", m, val);
    if ((m = r8h_of(n)) != NULL) return lf_afmt("SET8H(c->%s, %s);", m, val);
    if (is_seg(n)) return lf_afmt("c->%s = (uint16_t)(%s);", n, val);
    fprintf(stderr, "[!] reg_write %s\n", n);
    return lf_afmt("c->%s = (uint32_t)(%s);", n, val);
}

/* A register used to form an address: the 16-bit ones are windows onto the
 * 32-bit struct fields, so they cannot be emitted raw. */
static char *areg(const char *n)
{
    const char *m;
    if (is_r32(n)) return lf_afmt("c->%s", n);
    if ((m = r16_of(n)) != NULL) return lf_afmt("R16(c->%s)", m);
    if ((m = r8l_of(n)) != NULL) return lf_afmt("R8L(c->%s)", m);
    if ((m = r8h_of(n)) != NULL) return lf_afmt("R8H(c->%s)", m);
    return lf_afmt("c->%s", n);
}

int lf_in_image(Lifter *L, uint32_t a)
{
    return a >= L->image_lo && a < L->image_hi;
}

static int reloc_has(Lifter *L, uint32_t va)
{
    size_t lo = 0, hi = L->nreloc;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (L->reloc[mid] == va) return 1;
        if (L->reloc[mid] < va) lo = mid + 1; else hi = mid;
    }
    return 0;
}

int lf_disp_is_addr(Lifter *L, cs_insn *insn, uint32_t d)
{
    unsigned doff = insn->detail ? insn->detail->x86.encoding.disp_offset : 0;
    if (L->nreloc && doff)
        return reloc_has(L, (uint32_t)(insn->address + doff));
    return lf_in_image(L, d);
}

const char *lf_seg_name(Lifter *L, cs_x86_op *op)
{
    unsigned seg = (unsigned)op->mem.segment;
    return seg ? lf_reg_name(L, seg) : NULL;
}

char *lf_addr_expr(Lifter *L, cs_insn *insn, cs_x86_op *op)
{
    char *terms[4];
    int nt = 0;
    uint32_t d = (uint32_t)op->mem.disp;
    char *out;
    int i;
    size_t len = 2;

    if (op->mem.base)  terms[nt++] = areg(lf_reg_name(L, (unsigned)op->mem.base));
    if (op->mem.index)
        terms[nt++] = lf_afmt("%s*%d", areg(lf_reg_name(L, (unsigned)op->mem.index)),
                              (int)op->mem.scale);
    if (d || nt == 0) {
        if (lf_disp_is_addr(L, insn, d))
            terms[nt++] = lf_afmt("GVA(0x%08X)", d);
        else
            terms[nt++] = lf_afmt("0x%08Xu", d);
    }
    for (i = 0; i < nt; i++) len += strlen(terms[i]) + 3;
    out = (char *)ar_alloc(len);
    out[0] = '(';
    out[1] = 0;
    for (i = 0; i < nt; i++) {
        if (i) strcat(out, " + ");
        strcat(out, terms[i]);
    }
    strcat(out, ")");
    return out;
}

char *lf_seg_off(Lifter *L, cs_insn *insn, cs_x86_op *op)
{
    char *terms[4];
    int nt = 0, i;
    uint32_t d = (uint32_t)op->mem.disp;
    char *out;
    size_t len = 2;

    (void)insn;
    if (op->mem.base)  terms[nt++] = areg(lf_reg_name(L, (unsigned)op->mem.base));
    if (op->mem.index)
        terms[nt++] = lf_afmt("%s*%d", areg(lf_reg_name(L, (unsigned)op->mem.index)),
                              (int)op->mem.scale);
    if (d || nt == 0) terms[nt++] = lf_afmt("0x%08Xu", d);
    for (i = 0; i < nt; i++) len += strlen(terms[i]) + 3;
    out = (char *)ar_alloc(len);
    out[0] = '(';
    out[1] = 0;
    for (i = 0; i < nt; i++) {
        if (i) strcat(out, " + ");
        strcat(out, terms[i]);
    }
    strcat(out, ")");
    return out;
}

char *lf_rd(Lifter *L, cs_insn *insn, cs_x86_op *op)
{
    const char *sg = lf_seg_name(L, op);
    int sz = op->size;
    char *a;

    if (sg && !strcmp(sg, "fs")) {
        char *off = lf_seg_off(L, insn, op);
        if (sz == 1) return lf_afmt("FS_RD8(c, %s)", off);
        if (sz == 2) return lf_afmt("FS_RD16(c, %s)", off);
        if (sz == 4) return lf_afmt("FS_RD32(c, %s)", off);
        /* Must stay an *expression*: this is substituted into e.g.
         * `mmx_psllq(a, rd(b))`, so a trailing `;` here produced
         * `(uint32_t)(RECOMP_TODO(...);)` and gcc rejected the whole
         * translation unit. */
        return lf_afmt("(RECOMP_TODO(0x%08X, \"rd fs: bad size\"), 0u)",
                       (uint32_t)insn->address);
    }
    a = lf_addr_expr(L, insn, op);
    if (sz == 1) return lf_afmt("rd8(%s)", a);
    if (sz == 2) return lf_afmt("rd16(%s)", a);
    if (sz == 4) return lf_afmt("rd32(%s)", a);
    /* MMX/SSE operands are 8 bytes wide; cpu.h has had rd64 all along and the
     * lifter simply never used it.  `psllq mm1, qword ptr [edi]` in
     * 6F762F53.. was the one that broke the build. */
    if (sz == 8) return lf_afmt("rd64(%s)", a);
    return lf_afmt("(RECOMP_TODO(0x%08X, \"rd bad size\"), 0u)",
                   (uint32_t)insn->address);
}

char *lf_wr(Lifter *L, cs_insn *insn, cs_x86_op *op, const char *val)
{
    const char *sg = lf_seg_name(L, op);
    int sz = op->size;
    char *a;

    if (sg && !strcmp(sg, "fs")) {
        char *off = lf_seg_off(L, insn, op);
        if (sz == 1) return lf_afmt("FS_WR8(c, %s, %s);", off, val);
        if (sz == 2) return lf_afmt("FS_WR16(c, %s, %s);", off, val);
        if (sz == 4) return lf_afmt("FS_WR32(c, %s, %s);", off, val);
        return lf_afmt("RECOMP_TODO(0x%08X, \"wr fs: bad size\");", (uint32_t)insn->address);
    }
    a = lf_addr_expr(L, insn, op);
    if (sz == 1) return lf_afmt("wr8(%s, %s);", a, val);
    if (sz == 2) return lf_afmt("wr16(%s, %s);", a, val);
    if (sz == 4) return lf_afmt("wr32(%s, %s);", a, val);
    if (sz == 8) return lf_afmt("wr64(%s, %s);", a, val);
    return lf_afmt("RECOMP_TODO(0x%08X, \"wr bad size\");", (uint32_t)insn->address);
}

char *lf_src(Lifter *L, cs_insn *insn, cs_x86_op *op)
{
    if (op->type == X86_OP_REG) return reg_read(lf_reg_name(L, (unsigned)op->reg));
    if (op->type == X86_OP_IMM) {
        int sz = op->size ? op->size : 1;
        uint64_t mask = sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu;
        uint32_t v = (uint32_t)((uint64_t)op->imm & mask);
        if (sz == 4 && L->nreloc) {
            unsigned io = insn->detail ? insn->detail->x86.encoding.imm_offset : 0;
            if (io && reloc_has(L, (uint32_t)(insn->address + io)))
                return lf_afmt("GVA(0x%08X)", v);
        }
        return lf_afmt("0x%Xu", v);
    }
    if (op->type == X86_OP_MEM) return lf_rd(L, insn, op);
    return lf_afmt("0u");
}

char *lf_dst_write(Lifter *L, cs_insn *insn, cs_x86_op *op, const char *val)
{
    if (op->type == X86_OP_REG) return reg_write(lf_reg_name(L, (unsigned)op->reg), val);
    if (op->type == X86_OP_MEM) return lf_wr(L, insn, op, val);
    return lf_afmt("RECOMP_TODO(0x%08X, \"dst type\");", (uint32_t)insn->address);
}

char *lf_read_dst(Lifter *L, cs_insn *insn, cs_x86_op *op)
{
    if (op->type == X86_OP_REG) return reg_read(lf_reg_name(L, (unsigned)op->reg));
    if (op->type == X86_OP_MEM) return lf_rd(L, insn, op);
    return lf_afmt("0u");
}

char *lf_todo(uint32_t va, const char *what)
{
    char buf[512];
    size_t i, j = 0;
    /* '"' -> '\'', then strip - the Python does the same so the generated C
     * stays a single valid string literal. */
    while (*what == ' ' || *what == '\t' || *what == '\n') what++;
    for (i = 0; what[i] && j + 1 < sizeof buf; i++)
        buf[j++] = what[i] == '"' ? '\'' : what[i];
    buf[j] = 0;
    while (j > 0 && (buf[j - 1] == ' ' || buf[j - 1] == '\t' || buf[j - 1] == '\n'))
        buf[--j] = 0;
    return lf_afmt("RECOMP_TODO(0x%08X, \"%s\");", va, buf);
}

const char *lf_cond(const char *m)
{
    static const struct { const char *m, *c; } tbl[] = {
        {"je","c->zf"},{"jz","c->zf"},{"jne","!c->zf"},{"jnz","!c->zf"},
        {"jbe","(c->cf || c->zf)"},{"jna","(c->cf || c->zf)"},
        {"ja","(!c->cf && !c->zf)"},{"jnbe","(!c->cf && !c->zf)"},
        {"jb","c->cf"},{"jc","c->cf"},{"jnae","c->cf"},
        {"jae","!c->cf"},{"jnb","!c->cf"},{"jnc","!c->cf"},
        {"jl","(c->sf != c->of)"},{"jnge","(c->sf != c->of)"},
        {"jge","(c->sf == c->of)"},{"jnl","(c->sf == c->of)"},
        {"jle","(c->zf || (c->sf != c->of))"},{"jng","(c->zf || (c->sf != c->of))"},
        {"jg","(!c->zf && (c->sf == c->of))"},{"jnle","(!c->zf && (c->sf == c->of))"},
        {"js","c->sf"},{"jns","!c->sf"},{"jo","c->of"},{"jno","!c->of"},
        {"jp","c->pf"},{"jpe","c->pf"},{"jnp","!c->pf"},{"jpo","!c->pf"},
        {"jecxz","(c->ecx == 0)"},{"jcxz","(R16(c->ecx) == 0)"},
    };
    size_t i;
    for (i = 0; i < sizeof tbl / sizeof tbl[0]; i++)
        if (!strcmp(tbl[i].m, m)) return tbl[i].c;
    return NULL;
}

/* ------------------------------------------------------------ MMX / SSE */
int lf_is_mmx_op(Lifter *L, cs_x86_op *op)
{
    if (op->type != X86_OP_REG) return 0;
    return !strncmp(lf_reg_name(L, (unsigned)op->reg), "mm", 2);
}

int lf_is_xmm(Lifter *L, cs_x86_op *op)
{
    if (op->type != X86_OP_REG) return 0;
    return !strncmp(lf_reg_name(L, (unsigned)op->reg), "xmm", 3);
}

static int mm_index(Lifter *L, cs_x86_op *op)
{
    const char *n = lf_reg_name(L, (unsigned)op->reg);
    return n[2] ? atoi(n + 2) : 0;
}

static int xmm_index(Lifter *L, cs_x86_op *op)
{
    const char *n = lf_reg_name(L, (unsigned)op->reg);
    return n[3] ? atoi(n + 3) : 0;
}

char *lf_mm_read(Lifter *L, cs_insn *insn, cs_x86_op *op)
{
    if (lf_is_mmx_op(L, op)) return lf_afmt("(*mm_get(c, %d))", mm_index(L, op));
    if (op->type == X86_OP_MEM) return lf_afmt("rd64(%s)", lf_addr_expr(L, insn, op));
    return lf_afmt("(uint64_t)(%s)", lf_src(L, insn, op));
}

char *lf_mm_write(Lifter *L, cs_insn *insn, cs_x86_op *op, const char *val)
{
    if (lf_is_mmx_op(L, op)) {
        int i = mm_index(L, op);
        return lf_afmt("*mm_get(c, %d) = %s; mmx_mark(c, %d);", i, val, i);
    }
    if (op->type == X86_OP_MEM) return lf_afmt("wr64(%s, %s);", lf_addr_expr(L, insn, op), val);
    return lf_dst_write(L, insn, op, lf_afmt("(uint32_t)(%s)", val));
}

char *lf_mm_count(Lifter *L, cs_insn *insn, cs_x86_op *op)
{
    if (op->type == X86_OP_IMM) return lf_afmt("%d", (int)(op->imm & 0xFF));
    if (lf_is_mmx_op(L, op)) {
        int i = mm_index(L, op);
        return lf_afmt("((uint32_t)(*mm_get(c, %d)) > 63u ? 64u : (uint32_t)(*mm_get(c, %d)))", i, i);
    }
    return lf_afmt("(uint32_t)(%s)", lf_src(L, insn, op));
}

char *lf_xm(Lifter *L, cs_insn *insn, cs_x86_op *op)
{
    if (lf_is_xmm(L, op)) return lf_afmt("c->xmm[%d]", xmm_index(L, op));
    return lf_afmt("rdxm(%s)", lf_addr_expr(L, insn, op));
}

char *lf_ss(Lifter *L, cs_insn *insn, cs_x86_op *op)
{
    if (lf_is_xmm(L, op)) return lf_afmt("c->xmm[%d].f32[0]", xmm_index(L, op));
    return lf_afmt("rdss(%s)", lf_addr_expr(L, insn, op));
}

char *lf_sd(Lifter *L, cs_insn *insn, cs_x86_op *op)
{
    if (lf_is_xmm(L, op)) return lf_afmt("c->xmm[%d].f64[0]", xmm_index(L, op));
    return lf_afmt("rdsd(%s)", lf_addr_expr(L, insn, op));
}

int lf_xmm_index(Lifter *L, cs_x86_op *op) { return xmm_index(L, op); }
int lf_mm_index(Lifter *L, cs_x86_op *op) { return mm_index(L, op); }
