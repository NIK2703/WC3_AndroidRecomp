/* cpu_helpers_test.c - host-runnable checks for the flag-affecting helpers.
 *
 * tests/cpu_selftest.c covers x87 and TIB state as well, but its memory
 * operands assume a 32-bit host pointer.  This file keeps the integer helpers
 * checked in a test that runs unchanged on a 64-bit host.
 */
#include <stdint.h>
#include <stdio.h>

#include "cpu.h"

static int fails;

static void eq(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        printf("FAIL %-28s got 0x%08X want 0x%08X\n", what, got, want);
        fails++;
    }
}

int main(void)
{
    CPU c = {0};
    cpu_x87_init(&c);

    /* bsf/bsr: the operand width masks the source before the scan. */
    eq("bsf 0x12345678", op_bsf(&c, 0u, 0x12345678u, 4), 3u);
    eq("bsr 0x12345678", op_bsr(&c, 0u, 0x12345678u, 4), 28u);
    eq("bsf top bit", op_bsf(&c, 0u, 1u << 31, 4), 31u);
    eq("bsr top bit", op_bsr(&c, 0u, 1u << 31, 4), 31u);
    eq("bsf 16-bit bit 8", op_bsf(&c, 0u, 0x0100u, 2), 8u);
    eq("bsr 16-bit bit 15", op_bsr(&c, 0u, 0x8000u, 2), 15u);
    eq("bsf sets ZF on zero", (op_bsf(&c, 0xDEADu, 0u, 4), c.zf), 1u);
    eq("bsf leaves dst on zero", op_bsf(&c, 0xDEADu, 0u, 4), 0xDEADu);
    eq("bsr sets ZF on zero", (op_bsr(&c, 0xDEADu, 0u, 4), c.zf), 1u);
    eq("bsr leaves dst on zero", op_bsr(&c, 0xDEADu, 0u, 4), 0xDEADu);
    eq("bsf 8-bit ignores bit 8",
       (op_bsf(&c, 0xDEADu, 0x0100u, 1), c.zf), 1u);

    /* rcl/rcr: 32-bit count masking plus the 8/16-bit width modulo. */
    c.cf = 0; eq("rcl 80000001,1", op_rcl(&c, 0x80000001u, 1, 4), 2u);
    eq("rcl 80000001,1 -> CF", c.cf, 1u);
    c.cf = 0; eq("rcl 80000001,33", op_rcl(&c, 0x80000001u, 33, 4), 2u);
    eq("rcl 80000001,33 -> CF", c.cf, 1u);
    c.cf = 1; eq("rcl by 32 is identity", op_rcl(&c, 0x12345678u, 32, 4),
                 0x12345678u);
    eq("rcl by 32 keeps CF", c.cf, 1u);
    c.cf = 1; (void)op_rcl(&c, 0x12345678u, 0, 4);
    eq("rcl by 0 keeps CF", c.cf, 1u);
    c.cf = 0; eq("rcl al 81,8", op_rcl(&c, 0x81u, 8, 1), 0x40u);
    eq("rcl al 81,8 -> CF", c.cf, 1u);
    c.cf = 0; eq("rcl ax 8001,16", op_rcl(&c, 0x8001u, 16, 2), 0x4000u);
    eq("rcl ax 8001,16 -> CF", c.cf, 1u);
    c.cf = 0; eq("rcl al 81,17", op_rcl(&c, 0x81u, 17, 1), 0x40u);

    c.cf = 1; eq("rcr 80000001,1", op_rcr(&c, 0x80000001u, 1, 4),
                 0xC0000000u);
    eq("rcr 80000001,1 -> CF", c.cf, 1u);
    c.cf = 1; eq("rcr 80000001,33", op_rcr(&c, 0x80000001u, 33, 4),
                 0xC0000000u);
    eq("rcr 80000001,33 -> CF", c.cf, 1u);
    c.cf = 1; eq("rcr by 32 is identity", op_rcr(&c, 0x12345678u, 32, 4),
                 0x12345678u);
    eq("rcr by 32 keeps CF", c.cf, 1u);
    c.cf = 1; (void)op_rcr(&c, 0x12345678u, 0, 4);
    eq("rcr by 0 keeps CF", c.cf, 1u);
    c.cf = 1; eq("rcr al 01,8", op_rcr(&c, 0x01u, 8, 1), 0x03u);
    eq("rcr al 01,8 -> CF", c.cf, 0u);
    c.cf = 0; eq("rcr al 80,8", op_rcr(&c, 0x80u, 8, 1), 0x00u);
    eq("rcr al 80,8 -> CF", c.cf, 1u);

    if (fails == 0) printf("cpu helpers OK\n");
    return fails != 0;
}
