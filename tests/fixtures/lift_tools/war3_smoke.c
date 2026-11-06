/* Fixture for the compiled lift toolchain smoke test.  Not a game asset. */
#include "cpu.h"
void L_00401000(CPU *c) { dispatch(c, 0x00401030u); return; }
void L_00401030(CPU *c) { c->eax = 2; return; }
