/* Fixture: an unsupported guest instruction is an explicit trap, not a
 * fall-through.  RECOMP_TODO expands to abort() (runtime/cpu/cpu.h), so
 * liftpass must accept this body as terminal.  Not a game asset. */
#include "cpu.h"
void L_00401040(CPU *c) { RECOMP_TODO(0x00401040, "hlt"); }
