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
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
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

static double ext_bits(uint64_t bits)
{
    return x87_from_f64(x87_from_bits(bits));
}

/* x87 memory helpers take a 32-bit guest VA.  Map a page below 4 GiB so the
 * test is runnable on a 64-bit host as well as under a 32-bit compiler. */
static unsigned char *map_low(size_t size)
{
#ifdef _WIN32
    return (unsigned char *)VirtualAlloc((void *)(uintptr_t)0x20000000u,
                                         size, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_READWRITE);
#else
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#else
    flags |= MAP_FIXED;
#endif
    void *p = mmap((void *)(uintptr_t)0x20000000u, size,
                   PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p == MAP_FAILED) {
#ifdef MAP_32BIT
        p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#else
        p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    }
    if (p == MAP_FAILED)
        return NULL;
    if ((uintptr_t)p + size > UINT64_C(0x100000000)) {
        (void)munmap(p, size);
        return NULL;
    }
    return (unsigned char *)p;
#endif
}

static void unmap_low(unsigned char *p, size_t size)
{
    if (!p)
        return;
#ifdef _WIN32
    (void)size;
    (void)VirtualFree(p, 0, MEM_RELEASE);
#else
    (void)munmap(p, size);
#endif
}

int main(void)
{
    CPU c = {0};
    setvbuf(stdout, NULL, _IONBF, 0);
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

    /* shifts with count >= width: result 0 (shl/shr) or sign-replicated (sar),
     * CF undefined (we pin it to 0). 8/16-bit operands used to index a
     * negative C shift here; the guard is what keeps this defined. */
    eq("shl 8-bit by 8 -> 0", op_shl(&c, 0x12u, 8, 1), 0x00u);
    eq("shl 8-bit by 9 -> 0", op_shl(&c, 0x12u, 9, 1), 0x00u);
    eq("shl 16-bit by 16 -> 0", op_shl(&c, 0x1234u, 16, 2), 0x0000u);
    eq("shl 16-bit by 17 -> 0", op_shl(&c, 0x1234u, 17, 2), 0x0000u);
    eq("shr 8-bit by 8 -> 0", op_shr(&c, 0x80u, 8, 1), 0x00u);
    eq("shr 16-bit by 16 -> 0", op_shr(&c, 0x8000u, 16, 2), 0x0000u);
    eq("sar 8-bit by 8 negative -> FF", op_sar(&c, 0x80u, 8, 1), 0xFFu);
    eq("sar 8-bit by 8 positive -> 0", op_sar(&c, 0x7Fu, 8, 1), 0x00u);
    eq("sar 16-bit by 20 negative -> FFFF", op_sar(&c, 0x8000u, 20, 2), 0xFFFFu);
    eq("shl by 8 sets ZF/SF/PF", (op_shl(&c, 0x12u, 8, 1), c.zf), 1u);
    eq("shl by 8 clears SF", c.sf, 0u);
    eq("shl by 8 sets PF", c.pf, 1u);
    eq("sar by 8 negative sets SF", (op_sar(&c, 0x80u, 8, 1), c.sf), 1u);
    eq("shl 8-bit by 7 -> 0 (CF 0)", op_shl(&c, 0x80u, 7, 1), 0x00u);
    eq("shr 8-bit by 7 -> 1", op_shr(&c, 0x80u, 7, 1), 0x01u);
    eq("sar 8-bit by 7 -> FF", op_sar(&c, 0x80u, 7, 1), 0xFFu);

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
    double one80 = x87_from_i32(1);
    double two80 = x87_from_i32(2);
    double nan80 = x87_div(&c, x87_zero(), x87_zero());
    x87_compare(&c, two80, one80, 1);  eq("fcom greater  -> sw", c.fpu_sw & 0x4700u, 0x0000u);
    x87_compare(&c, one80, two80, 1);  eq("fcom less     -> C0", c.fpu_sw & 0x4700u, 0x0100u);
    x87_compare(&c, one80, one80, 1);  eq("fcom equal    -> C3", c.fpu_sw & 0x4700u, 0x4000u);
    x87_compare(&c, nan80, one80, 1);  eq("fcom unord -> C3|C2|C0", c.fpu_sw & 0x4700u, 0x4500u);

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

    /* ---- native x87 semantics ---- */
    {
        double qnan = ext_bits(UINT64_C(0x7FF8000000000000));
        double snan = ext_bits(UINT64_C(0x7FF0000000000001));
        double inf  = ext_bits(UINT64_C(0x7FF0000000000000));
        double ninf = ext_bits(UINT64_C(0xFFF0000000000000));
        double r;

        c.fpu_sw = 0;
        (void)x87_add(&c, qnan, one80);
        eq("qnan + 1 does not raise IE", c.fpu_sw & X87_IE, 0u);
        c.fpu_sw = 0;
        (void)x87_add(&c, snan, one80);
        eq("snan + 1 raises IE", c.fpu_sw & X87_IE, X87_IE);
        c.fpu_sw = 0;
        r = x87_add(&c, inf, ninf);
        eq("inf + -inf raises IE", c.fpu_sw & X87_IE, X87_IE);
        eq("inf + -inf is NaN", x87_is_nan_bits(x87_bits(r)), 1u);
        c.fpu_sw = 0;
        r = x87_mul(&c, x87_zero(), inf);
        eq("0 * inf raises IE", c.fpu_sw & X87_IE, X87_IE);
        eq("0 * inf is NaN", x87_is_nan_bits(x87_bits(r)), 1u);
        c.fpu_sw = 0;
        r = x87_div(&c, x87_zero(), x87_zero());
        eq("0 / 0 raises IE", c.fpu_sw & X87_IE, X87_IE);
        eq("0 / 0 is NaN", x87_is_nan_bits(x87_bits(r)), 1u);
        c.fpu_sw = 0;
        r = x87_div(&c, one80, x87_zero());
        eq("1 / 0 keeps IE clear", c.fpu_sw & X87_IE, 0u);
        eq("1 / 0 is Inf", x87_is_inf_bits(x87_bits(r)), 1u);
        c.fpu_sw = 0;
        r = x87_sqrt(&c, x87_from_i32(-1));
        eq("sqrt(-1) raises IE", c.fpu_sw & X87_IE, X87_IE);
        eq("sqrt(-1) is NaN", x87_is_nan_bits(x87_bits(r)), 1u);
        c.fpu_sw = 0;
        r = x87_fprem1(&c, x87_from_i32(5), x87_from_i32(2));
        eqd("fprem1 5,2", r, 1.0);
        (void)x87_fprem1(&c, x87_from_i32(5), x87_zero());
        eq("fprem1 x,0 raises IE", c.fpu_sw & X87_IE, X87_IE);

        /* FSCALE returns a NaN scale operand, and only an SNaN raises IE. */
        cpu_x87_init(&c);
        fpush(&c, ext_bits(UINT64_C(0x7FF8000000000123)));
        fpush(&c, x87_from_i32(2));
        x87_fscale(&c);
        eq("fscale qnan keeps IE clear", c.fpu_sw & X87_IE, 0u);
        eq("fscale qnan result is NaN",
           x87_is_nan_bits(x87_bits(*fst(&c, 0))), 1u);
        cpu_x87_init(&c);
        fpush(&c, ext_bits(UINT64_C(0x7FF0000000000123)));
        fpush(&c, x87_from_i32(2));
        x87_fscale(&c);
        eq("fscale snan raises IE", c.fpu_sw & X87_IE, X87_IE);
        eq("fscale snan result is quiet",
           x87_is_snan_bits(x87_bits(*fst(&c, 0))), 0u);

        cpu_x87_init(&c);
        fpush(&c, x87_from_i32(8));
        x87_fxtract(&c);
        eq("fxtract 8 keeps IE clear", c.fpu_sw & X87_IE, 0u);
        eqd("fxtract 8 significand", *fst(&c, 0), 1.0);
        eqd("fxtract 8 exponent", *fst(&c, 1), 3.0);
        cpu_x87_init(&c);
        fpush(&c, x87_zero());
        x87_fxtract(&c);
        eq("fxtract 0 keeps IE clear", c.fpu_sw & X87_IE, 0u);
        eqd("fxtract 0 significand", *fst(&c, 0), 0.0);
        eqd("fxtract 0 exponent", *fst(&c, 1), -INFINITY);

        cpu_x87_init(&c);
        fpush(&c, one80);
        *fst(&c, 0) = x87_zero();       /* helper-style write leaves tag valid */
        x87_refresh_tags(&c);
        eq("tag refresh sees zero", x87_tag(&c, c.fpu_top), 1u);
        cpu_x87_init(&c);
        fpush(&c, one80);
        x87_fst_reg(&c, 1);
        eq("fst st(1) copies tag", x87_tag(&c, (c.fpu_top + 1) & 7), 0u);

        eq("x87 slot is 16 bytes", (uint32_t)sizeof(x87_ext), 16u);
        eq("CPU x87 stack is 128 bytes", (uint32_t)sizeof c.st, 128u);
        c.st[0].v.d = one80;
        eq("mm aliases x87 low word", (uint32_t)(*mm_get(&c, 0) ==
                                                 x87_bits(one80)), 1u);
        mmx_emms(&c);
        c.fpu_cw = 0;
        cpu_x87_init(&c);
        eq("FNINIT cw", c.fpu_cw, 0x037Fu);
        cpu_x87_init_thread(&c);
        eq("thread-init cw", c.fpu_cw, 0x027Fu);
        cpu_x87_init(&c);
    }

    /* ---- virtual x87 stack (L2) ---- */
    {
        CPU vc = {0};
        double st[8];
        int top;
        uint16_t tag;

        cpu_x87_init(&vc);
        vs_load(&vc, st, &top, &tag);
        vs_push(&vc, st, &top, &tag, 1.0);
        vs_push(&vc, st, &top, &tag, 2.0);
        eqd("vs top is last push", *vs_fst(st, top, 0), 2.0);
        eqd("vs next is first push", *vs_fst(st, top, 1), 1.0);
        eqd("vs pop returns top", vs_pop(&vc, st, &top, &tag), 2.0);
        eqd("vs pop returns next", vs_pop(&vc, st, &top, &tag), 1.0);
        (void)vs_pop(&vc, st, &top, &tag);
        eq("vs pop empty raises IE", vc.fpu_sw & X87_IE, X87_IE);

        cpu_x87_init(&vc);
        vs_load(&vc, st, &top, &tag);
        vs_push(&vc, st, &top, &tag, 3.0);
        vs_flush(&vc, st, top, tag);
        eqd("vs flush writes st", vc.st[vc.fpu_top].v.d, 3.0);
        eq("vs flush writes top", (uint32_t)vc.fpu_top, (uint32_t)top);

        cpu_x87_init(&vc);
        vs_load(&vc, st, &top, &tag);
        vs_push(&vc, st, &top, &tag, 1.0);
        vs_push(&vc, st, &top, &tag, 2.0);
        vs_fxch(st, top, &tag, 1);
        eqd("vs fxch top", *vs_fst(st, top, 0), 1.0);
        eqd("vs fxch next", *vs_fst(st, top, 1), 2.0);
        vs_fst_reg(st, top, &tag, 1);
        eqd("vs fst_reg copies", *vs_fst(st, top, 1), 1.0);
        vc.cf = 1;
        vs_fcmov(&vc, st, top, &tag, 1, 1);
        eqd("vs fcmov copies", *vs_fst(st, top, 0), 1.0);
        vs_ffree(&tag, top, 0);
        eq("vs ffree marks empty", VS_TAG(tag, top), 3u);
        vs_push(&vc, st, &top, &tag, -1.0);
        vs_fchs(st, top);
        eqd("vs fchs flips sign", *vs_fst(st, top, 0), 1.0);
        vs_fabs(st, top);
        eqd("vs fabs clears sign", *vs_fst(st, top, 0), 1.0);
        cpu_x87_init(&vc);
    }

    /* ---- x87 f80 memory codec ----
     *
     * The native register stores a double, so a 64-bit x87 significand cannot
     * round-trip bit-for-bit.  These checks pin the accepted conversion:
     * normal values decode to the nearest double, low eleven significand bits
     * are lost, and unsupported encodings become signed zero. */
    {
        static const struct { uint64_t sig; uint16_t se; double want; } x87[] = {
            { UINT64_C(0x8000000000000000), 0x3FFF, 1.0 },
            { UINT64_C(0x8000000000000000), 0x3FFE, 0.5 },
            { UINT64_C(0xC90FDAA22168C235), 0x4000, 3.14159265358979323846 },
            { UINT64_C(0x0000000000000000), 0x0000, 0.0 },
            { UINT64_C(0x0000000000000000), 0x8000, -0.0 },
            { UINT64_C(0x8000000000000000), 0x7FFF, INFINITY },
            { UINT64_C(0x8000000000000000), 0xFFFF, -INFINITY },
            { UINT64_C(0x8000000000000000), 0x0001, 0.0 },   /* denormal */
            { UINT64_C(0x7FFFFFFFFFFFFFFF), 0x3FFF, 0.0 },   /* unnormal */
            { UINT64_C(0x7FFFFFFFFFFFFFFF), 0x7FFF, 0.0 },   /* pseudo-NaN */
        };
        unsigned k;
        unsigned char *buf = map_low(4096);
        if (!buf) { printf("FAIL low test buffer\n"); fails++; return 1; }
        for (k = 0; k < sizeof(x87) / sizeof(x87[0]); k++) {
            memcpy(buf,     &x87[k].sig, 8);
            memcpy(buf + 8, &x87[k].se,  2);
            double v = x87_ld_f80((uint32_t)(uintptr_t)buf);
            eqd("fld tbyte -> double", v, x87[k].want);

            memset(buf, 0xCC, 16);
            x87_st_f80((uint32_t)(uintptr_t)buf, v);
            double back = x87_ld_f80((uint32_t)(uintptr_t)buf);
            eqd("f80 round-trip double", back, v);
        }

        /* low 11 bits of the source significand are not preserved */
        {
            static const uint64_t sig = UINT64_C(0xC90FDAA22168C235);
            static const uint16_t se = 0x4000;
            uint64_t stored;
            memcpy(buf, &sig, 8);
            memcpy(buf + 8, &se, 2);
            double v = x87_ld_f80((uint32_t)(uintptr_t)buf);
            memset(buf, 0xCC, 16);
            x87_st_f80((uint32_t)(uintptr_t)buf, v);
            memcpy(&stored, buf, 8);
            eq("f80 low 11 bits cleared", (uint32_t)(stored & 0x7FFu), 0u);
        }

        /* rounding control is applied when the guest writes FCW */
        {
            uint16_t *cw = (uint16_t *)(void *)(buf + 256);
            int32_t *out = (int32_t *)(void *)(buf + 260);
            volatile double two_nine = 2.9;   /* keep nearbyint runtime */
            *cw = 0x0F7F;           /* round toward zero, 64-bit precision */
            x87_fldcw(&c, (uint32_t)(uintptr_t)cw);
            x87_st_i32(&c, (uint32_t)(uintptr_t)out,
                       x87_from_f64(two_nine));
            eq("fist with RC=truncate", (uint32_t)*out, 2u);
            *cw = 0x037F;
            x87_fldcw(&c, (uint32_t)(uintptr_t)cw);
            x87_st_i32(&c, (uint32_t)(uintptr_t)out,
                       x87_from_f64(two_nine));
            eq("fist with RC=nearest", (uint32_t)*out, 3u);
        }

        /* packed BCD uses its own integer codec */
        {
            unsigned char *bcd = buf;
            double v;
            memset(bcd, 0, 10);
            bcd[0] = 0x23; bcd[1] = 0x01;   /* 123 */
            v = x87_ld_bcd(&c, (uint32_t)(uintptr_t)bcd);
            eqd("fbld 123", v, 123.0);
            memset(bcd, 0, 10);
            x87_st_bcd(&c, (uint32_t)(uintptr_t)bcd, v);
            eq("fbstp low digit pair", bcd[0], 0x23u);
            eq("fbstp high digit pair", bcd[1], 0x01u);
            eq("fbstp zero pair", bcd[2], 0x00u);
            eq("fbstp sign", bcd[9], 0x00u);

            /* Invalid BCD digits have an undefined value but do not raise IE;
             * FBSTP out-of-range does raise IE and stores the BCD indefinite. */
            memset(bcd, 0, 10);
            bcd[0] = 0xAB;
            c.fpu_sw = 0;
            v = x87_ld_bcd(&c, (uint32_t)(uintptr_t)bcd);
            eq("fbld invalid digits keep IE clear", c.fpu_sw & X87_IE, 0u);
            eqd("fbld masks invalid digits", v, 111.0);
            c.fpu_sw = 0;
            memset(bcd, 0, 10);
            x87_st_bcd(&c, (uint32_t)(uintptr_t)bcd, x87_from_f64(INFINITY));
            eq("fbstp inf raises IE", c.fpu_sw & X87_IE, X87_IE);
            eq("fbstp indefinite byte 7", bcd[7], 0xC0u);
            eq("fbstp indefinite byte 8", bcd[8], 0xFFu);
            eq("fbstp indefinite byte 9", bcd[9], 0xFFu);
        }

        /* 32-bit protected-mode environment is seven dword slots, not
         * packed 16-bit fields.  Mss32 reads FIP/FDP with these offsets. */
        {
            uint32_t e[7];
            CPU d;
            c.fpu_cw = 0x037F;
            c.fpu_sw = 0;
            c.fpu_tag = 0xFFFFu;
            c.fpu_ip = 0x11111111u; c.fpu_cs = 0x2222u;
            c.fpu_dp = 0x33333333u; c.fpu_ds = 0x4444u;
            memset(buf, 0xCC, 32);
            x87_fnstenv(&c, (uint32_t)(uintptr_t)buf);
            memcpy(e, buf, sizeof e);
            eq("fnstenv cw slot", e[0], 0x037Fu);
            eq("fnstenv sw slot", e[1], 0u);
            eq("fnstenv tag slot", e[2], 0xFFFFu);
            eq("fnstenv ip slot", e[3], 0x11111111u);
            eq("fnstenv cs slot", e[4], 0x2222u);
            eq("fnstenv dp slot", e[5], 0x33333333u);
            eq("fnstenv ds slot", e[6], 0x4444u);
            memset(&d, 0, sizeof d);
            cpu_x87_init(&d);
            x87_fldenv(&d, (uint32_t)(uintptr_t)buf);
            eq("fldenv cw", d.fpu_cw, 0x037Fu);
            eq("fldenv ip", d.fpu_ip, 0x11111111u);
            eq("fldenv cs", d.fpu_cs, 0x2222u);
            eq("fldenv dp", d.fpu_dp, 0x33333333u);
            eq("fldenv ds", d.fpu_ds, 0x4444u);
        }

        /* FNSAVE/FRSTOR use the 28-byte environment plus eight 10-byte
         * registers, and FNSAVE leaves the x87 in the FNINIT state. */
        {
            cpu_x87_init(&c);
            fpush(&c, x87_from_f64(1.25));
            memset(buf, 0, 128);
            x87_fnsave(&c, (uint32_t)(uintptr_t)buf);
            eq("fnsave resets cw", c.fpu_cw, 0x037Fu);
            eq("fnsave clears tags", c.fpu_tag, 0xFFFFu);
            x87_frstor(&c, (uint32_t)(uintptr_t)buf);
            eq("frstor restores cw", c.fpu_cw, 0x037Fu);
            eqd("frstor restores st0", *fst(&c, 0), 1.25);
        }

        unmap_low(buf, 4096);
    }

    /* fs: via the portable simulated-TIB path (FS_* macros). The MSVC default
     * reads the real segment, not c->fs_base, so only check it when the
     * simulated path is the one compiled in (gcc/clang, or RECOMP_FS_SIMULATED).
     * This is what a non-Windows host relies on for SEH prologues. */
#if !defined(_MSC_VER) || defined(RECOMP_FS_SIMULATED)
    {
        uint8_t *tib = map_low(4096);
        CPU fc;
        if (!tib) {
            printf("FAIL low TIB buffer\n");
            fails++;
        } else {
            memset(&fc, 0, sizeof fc);
            fc.fs_base = (uint32_t)(uintptr_t)tib;
            *(uint32_t *)(tib + 0) = 0xDEADBEEFu;        /* prior SEH list head */
            eq("fs:[0] read",      FS_RD32(&fc, 0x00000000u), 0xDEADBEEFu);
            FS_WR32(&fc, 0x00000000u, 0x00123450u);      /* push a record */
            eq("fs:[0] write",     FS_RD32(&fc, 0x00000000u), 0x00123450u);
            FS_WR16(&fc, 0x00000004u, 0xBEEF);
            eq("fs:[4] word",      FS_RD16(&fc, 0x00000004u), 0xBEEFu);
            FS_WR8 (&fc, 0x00000006u, 0x5A);
            eq("fs:[6] byte",      FS_RD8 (&fc, 0x00000006u), 0x5Au);
            FS_WR32(&fc, 0x00000018u, fc.fs_base);       /* fs:[0x18] = self */
            eq("fs:[0x18] self",   FS_RD32(&fc, 0x00000018u), fc.fs_base);
            unmap_low(tib, 4096);
        }
    }
#endif

    if (fails == 0)
        printf("cpu_selftest: all checks passed\n");
    return fails != 0;
}
