/* cstest.c - smoke test for csglue: decode a few bytes, print operands. */
#include "csglue.h"

int main(void)
{
    csh h;
    cs_insn *insn;
    const uint8_t code[] = { 0x8B, 0x44, 0x24, 0x08,      /* mov eax,[esp+8] */
                             0xD9, 0xC1,                  /* fld st(1)       */
                             0x0F, 0x28, 0xC1 };          /* movaps xmm0,xmm1*/
    const uint8_t *p = code;
    size_t left = sizeof code;
    uint64_t addr = 0x401000;

    if (csg_open_x86(&h) != 0) return 1;
    insn = csg.malloc_insn(h);
    if (!insn) { fprintf(stderr, "cs_malloc failed\n"); return 1; }

    while (left && csg.iter(h, &p, &left, &addr, insn)) {
        int i;
        printf("%08llX  %-8s %-24s nops=%d",
               (unsigned long long)insn->address, insn->mnemonic, insn->op_str,
               insn->detail ? insn->detail->x86.op_count : -1);
        if (insn->detail) {
            for (i = 0; i < insn->detail->x86.op_count; i++) {
                cs_x86_op *o = &insn->detail->x86.operands[i];
                printf(" [%d type=%d size=%d", i, (int)o->type, (int)o->size);
                if (o->type == X86_OP_REG)
                    printf(" reg=%s", csg.reg_name(h, o->reg));
                else if (o->type == X86_OP_MEM)
                    printf(" mem base=%s idx=%s scale=%d disp=%lld",
                           csg.reg_name(h, o->mem.base),
                           csg.reg_name(h, o->mem.index), (int)o->mem.scale,
                           (long long)o->mem.disp);
                else if (o->type == X86_OP_IMM)
                    printf(" imm=%lld", (long long)o->imm);
                printf("]");
            }
        }
        printf("\n");
    }
    csg.free_insn(insn, 1);
    csg.close(&h);
    return 0;
}
