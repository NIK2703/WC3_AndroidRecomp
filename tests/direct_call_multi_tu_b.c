/* G-14 (b): a direct callee defers a tail target through dispatch_jmp. */
#include "direct_call_multi_tu_common.h"

void L_00401040(CPU *c);

void L_00401010(CPU *c)
{
    tc_note(c, 2);
    dispatch_jmp(c, 0x00401020u);
    return;
}

void L_00401030(CPU *c)
{
    tc_note(c, 5);
    WC3_DIRECT_TAIL(L_00401040, c);
    return;
}

/* D4c leaf target in a different translation unit from L_00401070. */
void L_00401060(CPU *c)
{
    c->esp += 4u; /* model the leaf's guest ret */
    tc_note(c, 7);
}
