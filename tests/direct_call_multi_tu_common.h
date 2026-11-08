/*
 * direct_call_multi_tu_common.h - shared fixture for the G-14 multi-TU test.
 *
 * TestCPU puts the real CPU first so a CPU * passed through the dispatcher can
 * be recovered without changing the generated ABI.  The test only uses the
 * transition fields in CPU; the log/hops fields are test-local.
 */
#ifndef WC3_TESTS_DIRECT_CALL_MULTI_TU_COMMON_H
#define WC3_TESTS_DIRECT_CALL_MULTI_TU_COMMON_H

#include <stdint.h>

#include "cpu.h"
#include "direct_call.h"

typedef struct TestCPU {
    CPU cpu;
    int log[32];
    int n;
    long hops;
} TestCPU;

static inline TestCPU *tc(CPU *c)
{
    return (TestCPU *)c;
}

static inline const TestCPU *tcc(const CPU *c)
{
    return (const TestCPU *)c;
}

static inline void tc_note(CPU *c, int value)
{
    TestCPU *s = tc(c);
    if (s->n < (int)(sizeof s->log / sizeof s->log[0]))
        s->log[s->n++] = value;
}

#endif /* WC3_TESTS_DIRECT_CALL_MULTI_TU_COMMON_H */
