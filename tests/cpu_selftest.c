/* cpu_selftest.c - check the CPU helpers against known-good values.
 *
 * These are bit operations where an off-by-one in a shift width produces
 * plausible-looking output rather than a crash, so they are worth pinning to
 * values worked out by hand. Rotates in particular are easy to get subtly
 * wrong: they do NOT touch ZF/SF/PF on x86, and a helper that sets them looks
 * fine until lifted code branches on a flag the previous instruction set.
 *
 *   cl /nologo cpu_selftest.c && cpu_selftest.exe
 *   cc -o cpu_selftest cpu_selftest.c && ./cpu_selftest
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "cpu.h"

static int fails;

static void eqf(const char *what, float got, float want)
{
    /* Compare the bit patterns, not the values: -0.0 == +0.0 is true in C and
     * the whole point of the min/max checks below is telling those apart. */
    uint32_t g, w;
    memcpy(&g, &got, 4); memcpy(&w, &want, 4);
    if (g != w) {
        printf("FAIL %-28s got %g (0x%08X) want %g (0x%08X)\n", what, got, g, want, w);
        fails++;
    }
}

static void eqd(const char *what, double got, double want)
{
    uint64_t g, w;
    memcpy(&g, &got, 8); memcpy(&w, &want, 8);
    if (g != w) {
        printf("FAIL %-28s got %.17g (0x%016llX) want %.17g (0x%016llX)\n", what,
               got, (unsigned long long)g, want, (unsigned long long)w);
        fails++;
    }
}

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

    /* rotates: 32-bit */
    eq("rol 0x12345678, 8", op_rol(&c, 0x12345678u, 8, 4), 0x34567812u);
    eq("ror 0x12345678, 8", op_ror(&c, 0x12345678u, 8, 4), 0x78123456u);
    eq("rol by 0 is identity", op_rol(&c, 0xDEADBEEFu, 0, 4), 0xDEADBEEFu);
    eq("rol by 32 is identity", op_rol(&c, 0xDEADBEEFu, 32, 4), 0xDEADBEEFu);

    /* rotates are width-sensitive: the same count differs per operand size */
    eq("rol 0x1234, 8 (16-bit)", op_rol(&c, 0x1234u, 8, 2), 0x3412u);
    eq("ror 0x12, 4 (8-bit)", op_ror(&c, 0x12u, 4, 1), 0x21u);

    /* a rotate must leave ZF alone - x86 does not set it here */
    c.zf = 1;
    (void)op_rol(&c, 0x00000001u, 1, 4);
    eq("rol preserves ZF", c.zf, 1);

    /* double-precision shifts */
    eq("shld F0F0F0F0 <- 0F0F0F0F, 4",
       op_shld(&c, 0xF0F0F0F0u, 0x0F0F0F0Fu, 4, 4), 0x0F0F0F00u);
    eq("shrd F0F0F0F0 <- 0F0F0F0F, 4",
       op_shrd(&c, 0xF0F0F0F0u, 0x0F0F0F0Fu, 4, 4), 0xFF0F0F0Fu);
    eq("shld by 0 is identity", op_shld(&c, 0xAAAAAAAAu, 0u, 0, 4), 0xAAAAAAAAu);

    /* bit scans. The flag is the whole interface - ZF says whether there was
     * a set bit at all - and with a zero source the destination is
     * architecturally undefined, so it must be left alone rather than given a
     * value that code could come to depend on. Values from hardware. */
    eq("bsf 0x12345678", op_bsf(&c, 0u, 0x12345678u, 4), 3u);
    eq("bsr 0x12345678", op_bsr(&c, 0u, 0x12345678u, 4), 28u);
    eq("bsr sets ZF on zero", (op_bsr(&c, 0xDEADu, 0u, 4), c.zf), 1u);
    eq("bsr leaves dst on zero", op_bsr(&c, 0xDEADu, 0u, 4), 0xDEADu);
    eq("bsf clears ZF otherwise", (op_bsf(&c, 0u, 1u, 4), c.zf), 0u);
    /* Operand width matters: the high half of a 16-bit value must not leak
     * into an 8-bit scan. */
    eq("bsf 16-bit bit 8", op_bsf(&c, 0u, 0x0100u, 2), 8u);
    eq("bsr 16-bit bit 15", op_bsr(&c, 0u, 0x8000u, 2), 15u);
    eq("bsf 8-bit ignores bit 8",
       (op_bsf(&c, 0xDEADu, 0x0100u, 1), c.zf), 1u);
    eq("bsf 8-bit leaves dst", op_bsf(&c, 0xDEADu, 0x0100u, 1), 0xDEADu);
    eq("bsf top bit", op_bsf(&c, 0u, 1u << 31, 4), 31u);
    eq("bsr top bit", op_bsr(&c, 0u, 1u << 31, 4), 31u);

    /* bit test: the selected bit into CF, and the count modulo the width -
     * which is what makes `bt eax, 33` test bit 1 and not read past. */
    eq("bt  0x20 bit 5 -> CF", (op_bittest(&c, 0x20u, 5, 4, 0), c.cf), 1u);
    eq("bt  does not write", op_bittest(&c, 0x20u, 5, 4, 0), 0x20u);
    eq("bts 0x00 bit 5", op_bittest(&c, 0x00u, 5, 4, 1), 0x20u);
    eq("btr 0x20 bit 5", op_bittest(&c, 0x20u, 5, 4, 2), 0x00u);
    eq("btc 0x20 bit 5", op_bittest(&c, 0x20u, 5, 4, 3), 0x00u);
    eq("bt  bit 33 is bit 1", (op_bittest(&c, 0x02u, 33, 4, 0), c.cf), 1u);

    /* rotate through carry: the rotated quantity is one bit wider than the
     * operand, so a 32-bit rcl by 4 is not the same as a rol by 4. */
    c.cf = 0; eq("rcl 80000001,1 CF=0", op_rcl(&c, 0x80000001u, 1, 4), 0x00000002u);
    eq("rcl 80000001,1 -> CF", c.cf, 1u);
    c.cf = 1; eq("rcl 80000001,1 CF=1", op_rcl(&c, 0x80000001u, 1, 4), 0x00000003u);
    c.cf = 1; eq("rcr 00000001,1 CF=1", op_rcr(&c, 0x00000001u, 1, 4), 0x80000000u);
    c.cf = 0; eq("rcl 12345678,4", op_rcl(&c, 0x12345678u, 4, 4), 0x23456780u);
    eq("rcl 12345678,4 -> CF", c.cf, 1u);
    c.cf = 1; eq("rcr 12345678,4", op_rcr(&c, 0x12345678u, 4, 4), 0x11234567u);
    c.cf = 1; eq("rcl al FF,1", op_rcl(&c, 0xFFu, 1, 1), 0xFFu);
    /* a zero count must change no flag at all, CF included */
    c.cf = 1; (void)op_rcl(&c, 0x12345678u, 0, 4);
    eq("rcl by 0 keeps CF", c.cf, 1u);
    /* x86 masks the count to five bits before the operand-width modulo. */
    c.cf = 0; eq("rcl 80000001,33", op_rcl(&c, 0x80000001u, 33, 4), 0x00000002u);
    eq("rcl 80000001,33 -> CF", c.cf, 1u);
    c.cf = 1; eq("rcl by 32 is identity", op_rcl(&c, 0x12345678u, 32, 4),
                 0x12345678u);
    eq("rcl by 32 keeps CF", c.cf, 1u);
    c.cf = 0; eq("rcl al 81,8", op_rcl(&c, 0x81u, 8, 1), 0x40u);
    eq("rcl al 81,8 -> CF", c.cf, 1u);
    c.cf = 0; eq("rcl ax 8001,16", op_rcl(&c, 0x8001u, 16, 2), 0x4000u);
    eq("rcl ax 8001,16 -> CF", c.cf, 1u);
    c.cf = 0; eq("rcl al 81,17", op_rcl(&c, 0x81u, 17, 1), 0x40u);
    c.cf = 1; eq("rcr 80000001,33", op_rcr(&c, 0x80000001u, 33, 4),
                 0xC0000000u);
    eq("rcr 80000001,33 -> CF", c.cf, 1u);
    c.cf = 1; eq("rcr by 32 is identity", op_rcr(&c, 0x12345678u, 32, 4),
                 0x12345678u);
    eq("rcr by 32 keeps CF", c.cf, 1u);
    c.cf = 1; eq("rcr al 01,8", op_rcr(&c, 0x01u, 8, 1), 0x03u);
    eq("rcr al 01,8 -> CF", c.cf, 0u);
    c.cf = 0; eq("rcr al 80,8", op_rcr(&c, 0x80u, 8, 1), 0x00u);
    eq("rcr al 80,8 -> CF", c.cf, 1u);

    /* segment registers exist and are 16-bit storage */
    c.ds = 0x1234; c.es = 0xFFFF; c.gs = 0x0007;
    eq("ds", c.ds, 0x1234u);
    eq("es", c.es, 0xFFFFu);
    eq("gs", c.gs, 0x0007u);

    /* ---- SSE ---- */

    /* The union really is one storage seen several ways - if it is not, every
     * movaps of a float mask silently corrupts something. */
    XMM x = {0};
    x.f32[0] = 1.0f;
    eq("xmm f32/u32 alias", x.u32[0], 0x3F800000u);

    /* A scalar load zeroes the rest of the register; a scalar reg-to-reg move
     * does not. Only the load has a helper, so only the load is checked here. */
    x.u64[0] = x.u64[1] = 0xAAAAAAAAAAAAAAAAull;
    sse_load_ss(&x, 2.0f);
    eq("movss load zeroes 32..63", x.u32[1], 0u);
    eq("movss load zeroes 64..127 lo", x.u32[2], 0u);
    eq("movss load zeroes 64..127 hi", x.u32[3], 0u);
    eqf("movss load keeps the value", x.f32[0], 2.0f);

    /* ucomiss sets the UNSIGNED flags - the compiler emits ja/jbe after a
     * float compare, never jg/jle - and an unordered compare sets all three,
     * which is what makes `ucomiss; jp` a NaN test. */
    sse_compare(&c, 2.0, 1.0);
    eq("ucomiss 2>1 zf", c.zf, 0u); eq("ucomiss 2>1 cf", c.cf, 0u);
    eq("ucomiss 2>1 pf", c.pf, 0u);
    sse_compare(&c, 1.0, 2.0);
    eq("ucomiss 1<2 cf", c.cf, 1u); eq("ucomiss 1<2 zf", c.zf, 0u);
    sse_compare(&c, 1.0, 1.0);
    eq("ucomiss 1==1 zf", c.zf, 1u); eq("ucomiss 1==1 cf", c.cf, 0u);
    c.sf = c.of = c.af = 1;
    sse_compare(&c, NAN, 1.0);
    eq("ucomiss NaN zf", c.zf, 1u);
    eq("ucomiss NaN pf", c.pf, 1u);
    eq("ucomiss NaN cf", c.cf, 1u);
    eq("ucomiss clears of", c.of, 0u);
    eq("ucomiss clears sf", c.sf, 0u);
    eq("ucomiss clears af", c.af, 0u);

    /* min/max are "if (dst OP src) dst else src", so the second operand wins
     * every tie. These four are the ones fmin/fmax get wrong.
     *
     * Through volatile, because the signed-zero pair has to be computed and
     * not folded: given two literals MSVC recognises the ternary as a min and
     * constant-folds it by a rule that does not keep the sign of a zero, so
     * the check would be measuring the compiler's front end rather than the
     * helper the lifted code actually calls. It reported +0 for both orders,
     * which no evaluation of `(d < s) ? d : s` can produce. */
    {
        volatile float pz = 0.0f, nz = -0.0f, one = 1.0f, two = 2.0f,
                       three = 3.0f, nan = (float)NAN;
        eqf("minss(1,2)", sse_minf(one, two), 1.0f);
        eqf("minss(2,1)", sse_minf(two, one), 1.0f);
        eqf("minss(+0,-0) is src", sse_minf(pz, nz), -0.0f);
        eqf("minss(-0,+0) is src", sse_minf(nz, pz), 0.0f);
        eqf("minss(NaN,3) is src", sse_minf(nan, three), 3.0f);
        eqf("minss(3,NaN) is src", sse_minf(three, nan), (float)NAN);
        eqf("maxss(1,2)", sse_maxf(one, two), 2.0f);
        eqf("maxss(NaN,3) is src", sse_maxf(nan, three), 3.0f);
    }

    /* Out of range, x86 yields the integer indefinite rather than whatever a
     * C cast would have done - which is undefined and may trap. */
    eq("cvttss2si 3.9 truncates", (uint32_t)sse_cvtt_i32(3.9), 3u);
    eq("cvttss2si -3.9 truncates", (uint32_t)sse_cvtt_i32(-3.9), (uint32_t)-3);
    eq("cvttss2si 1e18 indefinite", (uint32_t)sse_cvtt_i32(1e18), 0x80000000u);
    eq("cvttss2si NaN indefinite", (uint32_t)sse_cvtt_i32(NAN), 0x80000000u);

    /* x87 compares. Both forms of the same comparison, and the four outcomes
     * each - the unordered one above all, because it is the only case where
     * "not greater" and "less" differ, and a model that folds NaN into one of
     * the ordered answers makes a game take the wrong branch exactly when a
     * value has already gone wrong. The status-word bits are C3|C2|C0 of a
     * 0x4700 mask; the EFLAGS mapping is not the integer one. */
    extFloat80_t one80 = x87_from_i32(1);
    extFloat80_t two80 = x87_from_i32(2);
    extFloat80_t nan80 = x87_div(&c, x87_zero(), x87_zero());
    x87_compare(&c, two80, one80, 1);  eq("fcom greater  -> sw", c.fpu_sw & 0x4700u, 0x0000u);
    x87_compare(&c, one80, two80, 1);  eq("fcom less     -> C0", c.fpu_sw & 0x4700u, 0x0100u);
    x87_compare(&c, one80, one80, 1);  eq("fcom equal    -> C3", c.fpu_sw & 0x4700u, 0x4000u);
    x87_compare(&c, nan80, one80, 1);  eq("fcom unord -> C3|C2|C0", c.fpu_sw & 0x4700u, 0x4700u);

#define FLAGS3(cc) (((cc).zf << 2) | ((cc).pf << 1) | (cc).cf)
    x87_compare_eflags(&c, two80, one80, 1); eq("fcomi greater  -> 000", FLAGS3(c), 0u);
    x87_compare_eflags(&c, one80, two80, 1); eq("fcomi less     -> CF",  FLAGS3(c), 1u);
    x87_compare_eflags(&c, one80, one80, 1); eq("fcomi equal    -> ZF",  FLAGS3(c), 4u);
    x87_compare_eflags(&c, nan80, one80, 1); eq("fcomi unord -> ZF|PF|CF", FLAGS3(c), 7u);
    /* and it must clear OF/SF/AF, which an integer compare would have set */
    c.of = c.sf = c.af = 1;
    x87_compare_eflags(&c, one80, two80, 1);
    eq("fcomi clears OF|SF|AF", (uint32_t)(c.of | c.sf | c.af), 0u);
#undef FLAGS3

    /* ---- x87 extended, the ten-byte format ----
     *
     * The patterns are what a real x87 stores: taken from a compiler whose
     * long double IS this format, which MSVC's is not - it makes long double
     * a double, so comparing against it here would compare nothing. Pinning
     * the bytes instead means the check works in the build that matters.
     *
     * Reading eight of the ten bytes as a double would give a number that
     * looks plausible and is not, which is the whole reason these exist. */
    {
        static const struct { uint64_t m; uint16_t se; } x87[] = {
            { 0x8000000000000000ULL, 0x3FFF },
            { 0x8000000000000000ULL, 0x3FFE },
            { 0xC90FDAA221688800ULL, 0xC000 },
            { 0xBF21E44003ACE000ULL, 0x43E3 },
            { 0xAB70FE17C79AC800ULL, 0xBC1A },
            { 0x0000000000000000ULL, 0x0000 },
            { 0x0000000000000000ULL, 0x8000 },
            { 0x8000000000000000ULL, 0x401E },
            { 0xAAAAAAAAAAAAA800ULL, 0x3FFD },
        };
        unsigned k;
#ifdef _WIN32
        unsigned char *buf = (unsigned char *)VirtualAlloc(
            (void *)(uintptr_t)0x20000000u, 4096,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
        unsigned char *buf = malloc(16);
#endif
        if (!buf) { printf("FAIL low test buffer\n"); fails++; return 1; }
        for (k = 0; k < sizeof(x87) / sizeof(x87[0]); k++) {
            uint64_t m; uint16_t se;

            memcpy(buf,     &x87[k].m,  8);
            memcpy(buf + 8, &x87[k].se, 2);
            extFloat80_t v = x87_ld_f80((uint32_t)(uintptr_t)buf);
            eq("fld tbyte mantissa hi", (uint32_t)(v.signif >> 32),
               (uint32_t)(x87[k].m >> 32));
            eq("fld tbyte mantissa lo", (uint32_t)v.signif, (uint32_t)x87[k].m);
            eq("fld tbyte sign+exp", v.signExp, x87[k].se);

            memset(buf, 0xCC, 16);
            x87_st_f80((uint32_t)(uintptr_t)buf, v);
            memcpy(&m,  buf,     8);
            memcpy(&se, buf + 8, 2);
            eq("fstp tbyte mantissa hi", (uint32_t)(m >> 32), (uint32_t)(x87[k].m >> 32));
            eq("fstp tbyte mantissa lo", (uint32_t)m,         (uint32_t)x87[k].m);
            eq("fstp tbyte sign+exp",    se,                  x87[k].se);
        }
    }

    /* fs: via the portable simulated-TIB path (FS_* macros). The MSVC default
     * reads the real segment, not c->fs_base, so only check it when the
     * simulated path is the one compiled in (gcc/clang, or RECOMP_FS_SIMULATED).
     * This is what a non-Windows host relies on for SEH prologues. */
#if !defined(_MSC_VER) || defined(RECOMP_FS_SIMULATED)
    {
        static uint8_t tib[256];
        CPU fc; memset(&fc, 0, sizeof fc);
        fc.fs_base = (uint32_t)(uintptr_t)tib;
        *(uint32_t *)(tib + 0) = 0xDEADBEEFu;            /* prior SEH list head */
        eq("fs:[0] read",      FS_RD32(&fc, 0x00000000u), 0xDEADBEEFu);
        FS_WR32(&fc, 0x00000000u, 0x00123450u);          /* push a record */
        eq("fs:[0] write",     FS_RD32(&fc, 0x00000000u), 0x00123450u);
        FS_WR16(&fc, 0x00000004u, 0xBEEF);
        eq("fs:[4] word",      FS_RD16(&fc, 0x00000004u), 0xBEEFu);
        FS_WR8 (&fc, 0x00000006u, 0x5A);
        eq("fs:[6] byte",      FS_RD8 (&fc, 0x00000006u), 0x5Au);
        FS_WR32(&fc, 0x00000018u, fc.fs_base);           /* fs:[0x18] = self */
        eq("fs:[0x18] self",   FS_RD32(&fc, 0x00000018u), fc.fs_base);
    }
#endif

    if (fails == 0)
        printf("cpu_selftest: all checks passed\n");
    return fails != 0;
}
