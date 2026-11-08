/* G-14 (a)/(b): direct call and tail targets spanning translation units. */
#include "direct_call_multi_tu_common.h"

void L_00401010(CPU *c);

void L_00401000(CPU *c)
{
    tc_note(c, 1);
    WC3_DIRECT_CALL(L_00401010, c);
    /* L_00401010 tail-jumps to L_00401020; this must run after that target. */
    tc_note(c, 4);
}

void L_00401020(CPU *c)
{
    tc_note(c, 3);
}

void L_00401040(CPU *c)
{
    tc_note(c, 6);
}

void L_00401050(CPU *c)
{
    TestCPU *s = tc(c);
    s->hops++;
    if (s->hops < 100000L)
        WC3_DIRECT_TAIL(L_00401050, c);
    return;
}
