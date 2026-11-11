/*
 * dyn_x86.h - executor for guest code generated at runtime.
 *
 * ver-IX86-3 compiles the server's CheckRevision equation into a small x86
 * program, allocates it with PAGE_EXECUTE_READWRITE and jumps to it.  Static
 * recompilation cannot lift code that does not exist at lift time, so the
 * runtime keeps a tiny interpreter for that generated subset.  It is not a
 * general x86 emulator and it is only entered for ranges explicitly registered
 * as executable guest allocations.
 */
#ifndef WC3_DYN_X86_H
#define WC3_DYN_X86_H

#include "cpu.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Record an executable guest allocation.  The dispatcher consults this table
 * only after the lifted-function lookup fails, so registered ranges are the
 * sole entry point into the interpreter. */
void wc3_dyn_x86_register(uint32_t base, uint32_t size);
void wc3_dyn_x86_unregister(uint32_t base);
int wc3_dyn_x86_contains(uint32_t va);

/* Execute guest x86 at start until the dynamic code executes ret.  The guest
 * stack is updated exactly as the ret would update it; the C caller chain then
 * returns to the lifted caller that dispatched into the generated code. */
void wc3_dyn_x86_run(CPU *c, uint32_t start);

#ifdef __cplusplus
}
#endif

#endif /* WC3_DYN_X86_H */
