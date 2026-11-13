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

    /* A.24: the flagless forms must produce the same destination value, and
     * must leave the flag fields untouched. */
    {
        static const uint32_t vals[] = {0u, 1u, 0x80000000u, 0x12345678u, 0xFFFFFFFFu};
        static const uint32_t cnts[] = {0u, 1u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 32u};
        for (int sz = 1; sz <= 4; sz++) {
            for (unsigned vi = 0; vi < sizeof vals / sizeof vals[0]; vi++) {
                for (unsigned ci = 0; ci < sizeof cnts / sizeof cnts[0]; ci++) {
                    uint32_t d = vals[vi], s = vals[(vi + 2) % 5], cnt = cnts[ci];
                    CPU a = c, b = c;
                    a.cf = b.cf = 1; a.zf = b.zf = 1; a.sf = b.sf = 1; a.pf = b.pf = 1;
                    eq("shld_nf result", op_shld_nf(d, s, cnt, sz),
                       op_shld(&a, d, s, cnt, sz));
                    eq("shld_nf leaves flags", (b.zf << 2) | (b.sf << 1) | b.pf, 0x7u);
                    eq("shrd_nf result", op_shrd_nf(d, s, cnt, sz),
                       op_shrd(&a, d, s, cnt, sz));
                    a = c; b = c; a.cf = b.cf = 1;
                    eq("rcl_nf result", op_rcl_nf(&b, d, cnt, sz),
                       op_rcl(&a, d, cnt, sz));
                    eq("rcl_nf keeps CF", b.cf, 1u);
                    a = c; b = c; a.cf = b.cf = 1;
                    eq("rcr_nf result", op_rcr_nf(&b, d, cnt, sz),
                       op_rcr(&a, d, cnt, sz));
                    eq("rcr_nf keeps CF", b.cf, 1u);
                }
                CPU a = c;
                eq("bsf_nf result", op_bsf_nf(vals[vi], vals[vi] | 1u, 4),
                   op_bsf(&a, vals[vi], vals[vi] | 1u, 4));
                eq("bsr_nf result", op_bsr_nf(vals[vi], vals[vi] | 1u, 4),
                   op_bsr(&a, vals[vi], vals[vi] | 1u, 4));
                eq("bittest_nf result", op_bittest_nf(vals[vi], 5, 4, 1),
                   op_bittest(&a, vals[vi], 5, 4, 1));
            }
        }
    }

    /* C.4.2: the packed integer helpers are the same code path the generated
     * corpus now calls.  On this host they exercise the scalar fallback; the
     * NEON implementations are covered by the Android self-test/device run. */
    {
        XMM a, b;

        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        a.u8[0] = 0xFFu; a.u8[1] = 0x01u;
        b.u8[0] = 0x02u; b.u8[1] = 0xFFu;
        sse_paddb(&a, &b);
        eq("paddb wraps", (uint32_t)a.u8[0] | ((uint32_t)a.u8[1] << 8), 0x0001u);

        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        a.i8[0] = 0x7F; a.i8[1] = (int8_t)0x80;
        b.i8[0] = 1; b.i8[1] = -1;
        sse_paddsb(&a, &b);
        eq("paddsb saturates", (uint32_t)(uint8_t)a.i8[0] |
                            ((uint32_t)(uint8_t)a.i8[1] << 8), 0x807Fu);

        for (int i = 0; i < 8; i++) { a.i16[i] = (int16_t)(i + 1); b.i16[i] = (int16_t)(i + 2); }
        sse_pmaddwd(&a, &b);
        eq("pmaddwd lane 0", (uint32_t)a.i32[0], 8u);
        eq("pmaddwd lane 3", (uint32_t)a.i32[3], 128u);

        for (int i = 0; i < 8; i++) { a.i16[i] = (int16_t)(0x7FFF - i * 0x1111); b.i16[i] = (int16_t)(-0x8000 + i); }
        sse_packsswb(&a, &b);
        eq("packsswb low", (uint32_t)a.u8[0] | ((uint32_t)a.u8[1] << 8), 0x7F7Fu);
        eq("packsswb high", (uint32_t)a.u8[8] | ((uint32_t)a.u8[9] << 8), 0x8080u);

        for (int i = 0; i < 16; i++) { a.u8[i] = (uint8_t)i; b.u8[i] = (uint8_t)(16 + i); }
        sse_punpcklbw(&a, &b);
        eq("punpcklbw interleave", (uint32_t)a.u8[0] | ((uint32_t)a.u8[1] << 8), 0x1000u);

        a.u32[0] = 1; a.u32[1] = 2; a.u32[2] = 3; a.u32[3] = 4;
        sse_pshufd(&a, &a, 0x1B);
        eq("pshufd reverse", (uint32_t)a.u32[0] | ((uint32_t)a.u32[3] << 8), 0x0104u);

        for (int i = 0; i < 16; i++) a.u8[i] = (uint8_t)i;
        sse_pslldq(&a, 2);
        eq("pslldq low bytes", (uint32_t)a.u8[0] | ((uint32_t)a.u8[2] << 8), 0x0000u);
        eq("pslldq shifted", (uint32_t)a.u8[2] | ((uint32_t)a.u8[15] << 8), 0x0D00u);
        for (int i = 0; i < 16; i++) a.u8[i] = (uint8_t)i;
        sse_psrldq(&a, 2);
        eq("psrldq shifted", (uint32_t)a.u8[0] | ((uint32_t)a.u8[13] << 8), 0x0F02u);
        eq("psrldq high bytes", (uint32_t)a.u8[14] | ((uint32_t)a.u8[15] << 8), 0x0000u);

        for (int i = 0; i < 16; i++) a.u8[i] = (uint8_t)(i & 1 ? 0x80 : 0);
        eq("pmovmskb", sse_pmovmskb(&a), 0xAAAAu);

        a.u16[0] = 1; a.u16[1] = 2; a.u16[2] = 3; a.u16[3] = 4;
        sse_pshuflw(&a, &a, 0x1B);
        eq("pshuflw", (uint32_t)a.u16[0], 4u);
    }

    if (fails == 0) printf("cpu helpers OK\n");
    return fails != 0;
}
