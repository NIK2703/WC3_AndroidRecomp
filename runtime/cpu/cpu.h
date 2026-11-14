/*
 * cpu.h - runtime for the CPU-struct x86-32 recompilation model
 *         (paired with tools/lift/lift32_cpu.py).
 *
 * Lifted x86 functions operate on an explicit CPU state struct, so several
 * machine states can be live at once - which is what a hybrid build needs when
 * real library code calls back into lifted code while an outer lifted call is
 * still on the stack. See runtime/hybrid/.
 *
 * The recomp runs as a 32-bit program so register values are real 32-bit host
 * pointers: the image's data sections live at their mapped VAs and heap comes
 * from malloc - both inside the 32-bit address space - so memory operands
 * dereference directly.
 *
 * Flags are computed eagerly by helpers after arithmetic/logic ops; Jcc reads
 * the stored bits. This is verbose but mechanical and easy to verify against
 * the hardware. (A lazy-flags optimization can come later.)
 */
#ifndef PCRECOMP_CPU_H
#define PCRECOMP_CPU_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* x87/MMX register slot.  The low 64 bits are the x87 double or the MMX
 * integer register; the upper 64 bits are padding so the slot keeps the
 * 16-byte stride the frozen CPU layout expects.  See x87.h. */
typedef struct {
    union {
        double   d;
        uint64_t mm;
    } v;
    uint64_t reserved;
} x87_ext;

_Static_assert(sizeof(x87_ext) == 16, "x87 slot must stay 16 bytes");

#if defined(_MSC_VER)
#include <intrin.h>   /* __readfsdword/__writefsdword: TIB-relative (fs:) access for SEH */
#if defined(__clang__) && defined(_M_IX86)
/* clang-cl has __readfsdword but not __writefsdword on x86: the call links as
 * an undefined ___writefsdword. MSVC has both. */
static __inline void pcrecomp_writefsdword(unsigned long off, unsigned long v)
{
    __asm__ volatile("movl %1, %%fs:(%0)" : : "r"(off), "r"(v) : "memory");
}
#define __writefsdword pcrecomp_writefsdword
#endif
#endif

/* ---- SSE register file ----
 *
 * One storage, several views - which is how the silicon works, and why this is
 * a union rather than eight float[4]. `movaps` moves a bit pattern, `pxor`
 * works on integers and `addss` works on floats, all through the same 128
 * bits; a float-only model corrupts the first two the moment a game stores a
 * mask in an XMM register, which every SSE-era game does.
 *
 * Scalar single precision is `float` here and NOT `double` like the x87 stack
 * below it. That difference is deliberate. An x87 register really is wider
 * than the values it holds, so widening it costs nothing - but SSE single
 * genuinely IS 32-bit, and computing `addss` in double rounds once where the
 * hardware rounds twice. The results differ in the low bits, and in a game
 * that feeds them to a coordinate transform the difference is visible. */
typedef union {
    float    f32[4];
    double   f64[2];
    uint32_t u32[4];
    uint64_t u64[2];
    int32_t  i32[4];
    /* The word lanes pextrw and pinsrw address. Expressing those two through
     * u32 and a shift works and reads like arithmetic; a lane array says what
     * the instruction says. */
    uint16_t u16[8];
    int16_t  i16[8];
    /* The byte lanes pslldq and psrldq shift through. Those two move the whole
     * register by a byte count, not each lane by a bit count, so they are the
     * one SSE shift that cannot be written in terms of any wider lane. */
    uint8_t  u8[16];
    int8_t   i8[16];
    int64_t  i64[2];
} XMM;

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t eip;
    /* flags as discrete bits (0/1) */
    uint32_t cf, zf, sf, of, pf, af;
    /* x87/MMX register stack.  Each slot is 16 bytes; x87 stores a native
     * double in the low 64 bits, MMX uses the same bits as an integer register.
     * st0 == st[fpu_top]; push pre-decrements.  See x87.h. */
    x87_ext  st[8];
    int      fpu_top;
    uint16_t fpu_cw;   /* control word: rounding, precision, exception masks */
    uint16_t fpu_tag;  /* tag word: 2 bits per physical register */
    uint32_t fpu_sw;   /* status word: exceptions, TOP, C0-C3 */
    /* Saved x87 environment pointers. WC3's Mss32 uses FNSTENV/FLDENV and
     * FNSAVE/FRSTOR; the pointers are recorded so a save/restore round trip
     * preserves the whole 28-byte protected-mode environment. The lifter does
     * not yet update these per instruction, so their values are zero unless a
     * guest environment image supplied them. Code that inspects the saved IP
     * for diagnostics is therefore outside the current exactness gate. */
    uint32_t fpu_ip, fpu_dp;
    uint16_t fpu_cs, fpu_ds;
    /* SSE: xmm0..xmm7. A 32-bit target has no xmm8-15. */
    XMM      xmm[8];
    /* Segment registers: stored, but NOT used in address computation. The
     * memory model below is flat - a register holds a real 32-bit address - so
     * `mov ds, ax` records the selector and does nothing else. That is right
     * for code that only saves and restores them, which is the common case even
     * in segmented 32-bit code, and wrong for code that switches DS to reach
     * another segment's data. The latter needs a selector -> base mapping the
     * runtime does not have; such code should be caught rather than lifted
     * silently. */
    uint16_t cs, ds, es, fs, gs, ss;
    /* Linear base of the TIB that `fs:` points at. Used only by the portable
     * fs: path (FS_* macros below): the host points this at a simulated TIB
     * per thread, and fs:[off] reads ordinary memory at fs_base+off. Left 0 and
     * unused on the MSVC default path, which reads the real segment. */
    uint32_t fs_base;
    /* Virtual time stamp counter. RDTSC is nondeterministic on hardware; the
     * recomp keeps a monotonic guest counter so timing code remains ordered
     * without pretending to reproduce a wall-clock frequency. */
    uint64_t tsc;
    /* ABI-4/ABI-5: the one transition state owned by each guest thread.
     * Appending fields is source-level ABI-safe (GUEST_ABI ABI-1/ABI-2);
     * direct_call.h and dispatch_boot.c must use these fields, not a third
     * copy of thread-local state.  pending_target holds either a guest VA
     * (kind 0) or a lifted function pointer (kind 1). */
    uint32_t disp_depth;
    uint32_t disp_pending_valid;
    uint32_t disp_pending_kind;
    uintptr_t disp_pending_target;
    uint32_t disp_draining;
    /* Reserved four-byte slot kept for layout stability.  Native x87 state is
     * the st/fpu_* block above; this field is appended after the frozen ABI
     * fields and is not read. */
    uint32_t x87_reserved;
    /* HLE-owned per-thread state.  A guest thread already owns exactly one CPU,
     * so Win32 last-error and Tls* values belong here instead of in host
     * emutls.  This removes the resolver from every kernel32 HLE call without
     * changing the guest-visible TEB or the frozen ABI above. */
    uint32_t hle_last_error;
    uint32_t hle_tmp_guest;      /* guest VA of cr_qsort's 256-byte scratch */
    uint32_t hle_tls[64];        /* Win32 TLS slots; index allocation is global */
} CPU;

/* Frozen source-level layout.  Generated C names fields, but these two offsets
 * are also used by the MMX alias and by hand-written paths; drift is a build
 * error, not a reason to regenerate the corpus. */
_Static_assert(offsetof(CPU, st) == 64, "CPU.st offset changed");
_Static_assert(offsetof(CPU, xmm) == 216, "CPU.xmm offset changed");

/* direct_call.h uses this to require the complete CPU definition. */
#define WC3_CPU_DEFINED 1

/* Values of CPU.disp_pending_kind (ABI-5.2). */
#define WC3_DISP_PENDING_VA 0u
#define WC3_DISP_PENDING_FN 1u

/* The generated translation unit calls these; the host supplies the table
 * lookup. Declared here so a lifted file is a complete C translation unit on
 * its own (and so a missing host implementation is a link error, not an
 * implicit-int call). */
void dispatch(CPU *c, uint32_t target);
void dispatch_jmp(CPU *c, uint32_t target);

/* Exact game-state transition hook, emitted by the lifter after the pinned
 * CGameUI/CEscMenu/CGameOverMenu stores.  The host implementation lives in
 * runtime/ipc/game_state.c; keeping the declaration here makes generated C a
 * complete translation unit without a second include. */
void wc3_game_state_hook(unsigned kind, uint32_t value);

/* ---- CPUID / RDTSC -------------------------------------------------------
 *
 * A fixed, documented CPU profile is part of the ABI: changing it can select
 * different code paths in the original binary. This is a Pentium 4 Willamette
 * class part: family 15, model 2, MMX/SSE/SSE2, no SSE3/AVX. The vendor and
 * brand strings are synthetic ("GenuineIntel" is the string the original
 * compiler expects; the brand is only informational).
 *
 * Keep this in sync with tools/lift/lift32_cpu.py and difftest. */
#define PCRECOMP_CPUID_VENDOR_EBX 0x756E6547u  /* "Genu" */
#define PCRECOMP_CPUID_VENDOR_EDX 0x49656E69u  /* "ineI" */
#define PCRECOMP_CPUID_VENDOR_ECX 0x6C65746Eu  /* "ntel" */
#define PCRECOMP_CPUID_FAMILY     15u
#define PCRECOMP_CPUID_MODEL      2u
#define PCRECOMP_CPUID_STEPPING   7u

static inline void do_cpuid(CPU *c) {
    uint32_t leaf = c->eax, sub = c->ecx;
    uint32_t a = 0, b = 0, cx = 0, d = 0;
    if (leaf == 0) {
        a = 1;
        b = PCRECOMP_CPUID_VENDOR_EBX;
        d = PCRECOMP_CPUID_VENDOR_EDX;
        cx = PCRECOMP_CPUID_VENDOR_ECX;
    } else if (leaf == 1) {
        /* family/model/stepping, then feature bits */
        a = (PCRECOMP_CPUID_FAMILY << 8) | (PCRECOMP_CPUID_MODEL << 4) |
            PCRECOMP_CPUID_STEPPING;
        /* FPU VME DE PSE TSC MSR PAE MCE CX8 SEP MTRR PGE CMOV PAT PSE36
         * CLFSH MMX FXSR SSE SSE2. No SSE3, no AVX, no hypervisor. */
        d = 0x078BB9FFu;
        cx = 0;
    } else if (leaf == 0x80000000u) {
        a = 0x80000004u;
    } else if (leaf == 0x80000001u) {
        /* 32-bit part: no long mode, no NX. */
        a = b = cx = d = 0;
    } else if (leaf >= 0x80000002u && leaf <= 0x80000004u) {
        static const char brand[49] =
            "Intel(R) Pentium(R) 4 CPU 2.00GHz\0\0\0\0\0\0\0";
        const unsigned char *p = (const unsigned char *)brand +
                                (leaf - 0x80000002u) * 16u;
        memcpy(&a, p + 0, 4);
        memcpy(&b, p + 4, 4);
        memcpy(&cx, p + 8, 4);
        memcpy(&d, p + 12, 4);
    } else if (leaf == 0x80000006u) {
        /* L2 cache: 256 KiB, 8-way, 64-byte lines (informational). */
        cx = 0x01008040u;
    } else if (leaf == 0x80000007u) {
        a = b = cx = d = 0;
    } else {
        a = b = cx = d = 0;
    }
    (void)sub;
    c->eax = a;
    c->ebx = b;
    c->ecx = cx;
    c->edx = d;
}

static inline void do_rdtsc(CPU *c) {
    /* One tick per guest instruction boundary is too coarse for code that
     * spins on TSC; advance by a fixed step. Monotonic, never zero. */
    c->tsc += 1000u;
    c->eax = (uint32_t)c->tsc;
    c->edx = (uint32_t)(c->tsc >> 32);
}

/* ---- partial register access (preserve unaffected bits, like x86) ---- */
#define R8L(r)        ((uint8_t)(r))
#define R8H(r)        ((uint8_t)((r) >> 8))
#define R16(r)        ((uint16_t)(r))
#define SET8L(r, v)   ((r) = ((r) & 0xFFFFFF00u) | (uint8_t)(v))
#define SET8H(r, v)   ((r) = ((r) & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(v) << 8))
#define SET16(r, v)   ((r) = ((r) & 0xFFFF0000u) | (uint16_t)(v))

/* ---- memory access: register holds a real 32-bit address ---- */
/* WC3_WATCH_WRITES is a debugging aid for the war3 lift: it reports writes to
 * a fixed window so a field can be traced back to the lifted function that
 * stores it.  Off by default; define it and provide wc3_watch32(). */
#ifdef WC3_WATCH_WRITES
void wc3_watch32(uint32_t a, uint32_t v);
#define WC3_WR32_HOOK(a, v) do { \
    uint32_t _wa = (a), _wv = (v); \
    if ((_wa - 0x0DDB7BA0u < 0x40u) || (_wa - 0x0DDD7B40u < 0x40u) || \
        _wv == 0x0DDD7B50u) \
        wc3_watch32(_wa, _wv); \
} while (0)
#else
#define WC3_WR32_HOOK(a, v) ((void)0)
#endif
static inline uint8_t  rd8 (uint32_t a) { return *(uint8_t  *)(uintptr_t)a; }
static inline uint16_t rd16(uint32_t a) { return *(uint16_t *)(uintptr_t)a; }
static inline uint32_t rd32(uint32_t a) { return *(uint32_t *)(uintptr_t)a; }
static inline void wr8 (uint32_t a, uint8_t  v) { WC3_WR32_HOOK(a, v); *(uint8_t  *)(uintptr_t)a = v; }
static inline void wr16(uint32_t a, uint16_t v) { *(uint16_t *)(uintptr_t)a = v; }
static inline void wr32(uint32_t a, uint32_t v) { WC3_WR32_HOOK(a, v); *(uint32_t *)(uintptr_t)a = v; }

/* ---- fs:-relative access (TIB / SEH chain) ----
 *
 * The lifter emits these for every `fs:` memory operand (SEH prologues store
 * the exception-registration record through fs:[0]). Two implementations, one
 * chosen at compile time so a single lift builds for either host:
 *
 *   MSVC default        the real segment, via the __readfsN/__writefsN
 *                       intrinsics. Correct when the host is a 1:1 Win32
 *                       process whose own TIB the guest shares.
 *   portable / non-MSVC read ordinary memory at c->fs_base + off. The host
 *                       gives each guest thread a simulated TIB and points
 *                       c->fs_base at it. This is what a non-Windows (Linux)
 *                       host needs; it also works on Windows if the host sets
 *                       c->fs_base to NtCurrentTeb() and defines
 *                       RECOMP_FS_SIMULATED.
 */
#if defined(_MSC_VER) && !defined(RECOMP_FS_SIMULATED)
#define FS_RD8(c, off)      __readfsbyte(off)
#define FS_RD16(c, off)     __readfsword(off)
#define FS_RD32(c, off)     __readfsdword(off)
#define FS_WR8(c, off, v)   __writefsbyte((off), (unsigned char)(v))
#define FS_WR16(c, off, v)  __writefsword((off), (unsigned short)(v))
#define FS_WR32(c, off, v)  __writefsdword((off), (unsigned long)(v))
#else
#define FS_RD8(c, off)      rd8 ((c)->fs_base + (off))
#define FS_RD16(c, off)     rd16((c)->fs_base + (off))
#define FS_RD32(c, off)     rd32((c)->fs_base + (off))
#define FS_WR8(c, off, v)   wr8 ((c)->fs_base + (off), (uint8_t )(v))
#define FS_WR16(c, off, v)  wr16((c)->fs_base + (off), (uint16_t)(v))
#define FS_WR32(c, off, v)  wr32((c)->fs_base + (off), (uint32_t)(v))
#endif
/* movlps/movhps and friends move a lane as bits, not as a number: a double
 * round-trip would be exact for every value except the signalling NaNs a
 * mask-producing compare leaves behind. memcpy, because the address need
 * not be aligned. */
static inline uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, (void *)(uintptr_t)a, 8); return v; }
static inline void wr64(uint32_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, 8); }

/* ---- LSE atomics (C.6) ----
 *
 * ARMv8.1 LSE turns the LL/SC retry loops into one instruction.  The product
 * baseline is arm64-v8a, which does not guarantee LSE, so this is a runtime
 * choice: the test device has atomics, older devices keep the __sync path.
 * The `al` suffixes provide acquire-release; the trailing dmb ish makes the
 * operation a full barrier like the guest's x86 LOCK prefix.
 */
#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>

static inline int wc3_cpu_has_lse(void) {
    static int cached = -1;
    int v = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (v < 0) {
        v = (getauxval(AT_HWCAP) & (unsigned long)HWCAP_ATOMICS) != 0;
        __atomic_store_n(&cached, v, __ATOMIC_RELAXED);
    }
    return v;
}

static inline uint32_t wc3_lse_xadd32(uint32_t a, uint32_t v) {
    uint32_t old;
    __asm__ volatile(".arch armv8.1-a\n\tldaddal %w1, %w0, [%2]\n\t.arch armv8-a\n\t"
                     "dmb ish\n"
                     : "=&r"(old), "+r"(v) : "r"((uintptr_t)a) : "memory");
    return old;
}
static inline uint32_t wc3_lse_cmpxchg32(uint32_t a, uint32_t cmp, uint32_t nv) {
    uint32_t old = cmp;
    __asm__ volatile(".arch armv8.1-a\n\tcasal %w0, %w1, [%2]\n\t.arch armv8-a\n\t"
                     "dmb ish\n"
                     : "+r"(old) : "r"(nv), "r"((uintptr_t)a) : "memory");
    return old;
}
static inline uint32_t wc3_lse_xchg32(uint32_t a, uint32_t v) {
    uint32_t old;
    __asm__ volatile(".arch armv8.1-a\n\tswpal %w1, %w0, [%2]\n\t.arch armv8-a\n\t"
                     "dmb ish\n"
                     : "=&r"(old), "+r"(v) : "r"((uintptr_t)a) : "memory");
    return old;
}
static inline uint32_t wc3_lse_and32(uint32_t a, uint32_t v) {
    uint32_t old, inv = ~v;
    __asm__ volatile(".arch armv8.1-a\n\tldclral %w1, %w0, [%2]\n\t.arch armv8-a\n\t"
                     "dmb ish\n"
                     : "=&r"(old), "+r"(inv) : "r"((uintptr_t)a) : "memory");
    return old;
}
static inline uint32_t wc3_lse_or32(uint32_t a, uint32_t v) {
    uint32_t old;
    __asm__ volatile(".arch armv8.1-a\n\tldsetal %w1, %w0, [%2]\n\t.arch armv8-a\n\t"
                     "dmb ish\n"
                     : "=&r"(old), "+r"(v) : "r"((uintptr_t)a) : "memory");
    return old;
}
static inline uint32_t wc3_lse_xor32(uint32_t a, uint32_t v) {
    uint32_t old;
    __asm__ volatile(".arch armv8.1-a\n\tldeoral %w1, %w0, [%2]\n\t.arch armv8-a\n\t"
                     "dmb ish\n"
                     : "=&r"(old), "+r"(v) : "r"((uintptr_t)a) : "memory");
    return old;
}
static inline uint64_t wc3_lse_cmpxchg64(uint32_t a, uint64_t cmp, uint64_t nv) {
    uint64_t old = cmp;
    __asm__ volatile(".arch armv8.1-a\n\tcasal %x0, %x1, [%2]\n\t.arch armv8-a\n\t"
                     "dmb ish\n"
                     : "+r"(old) : "r"(nv), "r"((uintptr_t)a) : "memory");
    return old;
}
#endif

/* ---- EFLAGS pack/unpack (modelled bits only) ---- */
/* `lock`-prefixed read-modify-write.
 *
 * A recompiled game with a worker pool needs these to be real: the guest uses
 * `lock xadd` for every reference count and every queue index, and a
 * non-atomic one is a use-after-free an hour into a session rather than a
 * crash you can find. Nothing else in this header cares about other threads;
 * these do.  `xchg` with a memory operand is implicitly locked and the lifter
 * emits atomic_xchg32 for it.
 *
 * The x86 semantics are "return the old value" for xadd, and for cmpxchg
 * "return what was there, and the caller compares" - the ZF the instruction
 * sets follows from that. */
#if defined(_MSC_VER)
static inline uint32_t atomic_xadd32(uint32_t a, uint32_t v) {
    return (uint32_t)_InterlockedExchangeAdd((volatile long *)(uintptr_t)a, (long)v);
}
static inline uint32_t atomic_cmpxchg32(uint32_t a, uint32_t cmp, uint32_t nv) {
    return (uint32_t)_InterlockedCompareExchange((volatile long *)(uintptr_t)a,
                                                 (long)nv, (long)cmp);
}
static inline uint32_t atomic_xchg32(uint32_t a, uint32_t v) {
    return (uint32_t)_InterlockedExchange((volatile long *)(uintptr_t)a, (long)v);
}
static inline uint64_t atomic_cmpxchg64_aligned(uint32_t a, uint64_t cmp, uint64_t nv) {
    return (uint64_t)_InterlockedCompareExchange64(
        (volatile __int64 *)(uintptr_t)a, (__int64)nv, (__int64)cmp);
}
#elif defined(__aarch64__) && defined(__linux__)
static inline uint32_t atomic_xadd32(uint32_t a, uint32_t v) {
    if (wc3_cpu_has_lse()) return wc3_lse_xadd32(a, v);
    return __sync_fetch_and_add((volatile uint32_t *)(uintptr_t)a, v);
}
static inline uint32_t atomic_cmpxchg32(uint32_t a, uint32_t cmp, uint32_t nv) {
    if (wc3_cpu_has_lse()) return wc3_lse_cmpxchg32(a, cmp, nv);
    return __sync_val_compare_and_swap((volatile uint32_t *)(uintptr_t)a, cmp, nv);
}
static inline uint32_t atomic_xchg32(uint32_t a, uint32_t v) {
    if (wc3_cpu_has_lse()) return wc3_lse_xchg32(a, v);
    return __atomic_exchange_n((uint32_t *)(uintptr_t)a, v, __ATOMIC_SEQ_CST);
}
static inline uint64_t atomic_cmpxchg64_aligned(uint32_t a, uint64_t cmp, uint64_t nv) {
    if (wc3_cpu_has_lse()) return wc3_lse_cmpxchg64(a, cmp, nv);
    return __sync_val_compare_and_swap((volatile uint64_t *)(uintptr_t)a, cmp, nv);
}
#else
static inline uint32_t atomic_xadd32(uint32_t a, uint32_t v) {
    return __sync_fetch_and_add((volatile uint32_t *)(uintptr_t)a, v);
}
static inline uint32_t atomic_cmpxchg32(uint32_t a, uint32_t cmp, uint32_t nv) {
    return __sync_val_compare_and_swap((volatile uint32_t *)(uintptr_t)a, cmp, nv);
}
static inline uint32_t atomic_xchg32(uint32_t a, uint32_t v) {
    /* x86 xchg with a memory operand is locked and a full barrier; __sync's
     * test-and-set is only an acquire barrier on some targets. */
    return __atomic_exchange_n((uint32_t *)(uintptr_t)a, v, __ATOMIC_SEQ_CST);
}
static inline uint64_t atomic_cmpxchg64_aligned(uint32_t a, uint64_t cmp, uint64_t nv) {
    return __sync_val_compare_and_swap((volatile uint64_t *)(uintptr_t)a, cmp, nv);
}
#endif

/* The remaining locked RMW forms. x86 LOCK is valid on ADD/ADC/AND/BTC/BTR/
 * BTS/CMPXCHG/CMPXCHG8B/DEC/INC/NEG/NOT/OR/SBB/SUB/XADD/XCHG/XOR; the lifter
 * covers the dword memory forms. `lock sub` uses fetch-sub; `lock adc/sbb`
 * fold the carry into the value; `lock bts/btr/btc` use fetch-or/and/xor with
 * a bit mask; `lock not` is xor with all ones; `lock neg` is a CAS loop. */
#if defined(_MSC_VER)
static inline uint32_t atomic_sub32(uint32_t a, uint32_t v) {
    return (uint32_t)_InterlockedExchangeAdd((volatile long *)(uintptr_t)a, -(long)v);
}
static inline uint32_t atomic_and32(uint32_t a, uint32_t v) {
    return (uint32_t)_InterlockedAnd((volatile long *)(uintptr_t)a, (long)v);
}
static inline uint32_t atomic_or32(uint32_t a, uint32_t v) {
    return (uint32_t)_InterlockedOr((volatile long *)(uintptr_t)a, (long)v);
}
static inline uint32_t atomic_xor32(uint32_t a, uint32_t v) {
    return (uint32_t)_InterlockedXor((volatile long *)(uintptr_t)a, (long)v);
}
static inline uint32_t atomic_neg32(uint32_t a) {
    uint32_t old = rd32(a);
    while ((uint32_t)_InterlockedCompareExchange((volatile long *)(uintptr_t)a,
                                                 (long)(0u - old), (long)old) != old)
        old = rd32(a);
    return old;
}
#elif defined(__aarch64__) && defined(__linux__)
static inline uint32_t atomic_sub32(uint32_t a, uint32_t v) {
    if (wc3_cpu_has_lse()) return wc3_lse_xadd32(a, 0u - v);
    return __sync_fetch_and_sub((volatile uint32_t *)(uintptr_t)a, v);
}
static inline uint32_t atomic_and32(uint32_t a, uint32_t v) {
    if (wc3_cpu_has_lse()) return wc3_lse_and32(a, v);
    return __sync_fetch_and_and((volatile uint32_t *)(uintptr_t)a, v);
}
static inline uint32_t atomic_or32(uint32_t a, uint32_t v) {
    if (wc3_cpu_has_lse()) return wc3_lse_or32(a, v);
    return __sync_fetch_and_or((volatile uint32_t *)(uintptr_t)a, v);
}
static inline uint32_t atomic_xor32(uint32_t a, uint32_t v) {
    if (wc3_cpu_has_lse()) return wc3_lse_xor32(a, v);
    return __sync_fetch_and_xor((volatile uint32_t *)(uintptr_t)a, v);
}
static inline uint32_t atomic_neg32(uint32_t a) {
    uint32_t old = rd32(a);
    while (atomic_cmpxchg32(a, old, 0u - old) != old)
        old = rd32(a);
    return old;
}
#else
static inline uint32_t atomic_sub32(uint32_t a, uint32_t v) {
    return __sync_fetch_and_sub((volatile uint32_t *)(uintptr_t)a, v);
}
static inline uint32_t atomic_and32(uint32_t a, uint32_t v) {
    return __sync_fetch_and_and((volatile uint32_t *)(uintptr_t)a, v);
}
static inline uint32_t atomic_or32(uint32_t a, uint32_t v) {
    return __sync_fetch_and_or((volatile uint32_t *)(uintptr_t)a, v);
}
static inline uint32_t atomic_xor32(uint32_t a, uint32_t v) {
    return __sync_fetch_and_xor((volatile uint32_t *)(uintptr_t)a, v);
}
static inline uint32_t atomic_neg32(uint32_t a) {
    uint32_t old = rd32(a);
    while (__sync_val_compare_and_swap((volatile uint32_t *)(uintptr_t)a, old,
                                       0u - old) != old)
        old = rd32(a);
    return old;
}
#endif

/* cmpxchg8b allows an unaligned operand. The host's 64-bit CAS may fault on
 * one; use the compiler/runtime atomic primitive instead of a project-owned
 * spinlock.  On x86 this is a locked cmpxchg8b; on ARM64 the compiler may route
 * the misaligned case through libatomic.  The primitive covers only the 8-byte
 * memory update and never wraps dispatch(), an HLE callback or a guest wait. */
static inline uint64_t atomic_cmpxchg64_slow(uint32_t a, uint64_t cmp, uint64_t nv) {
#if defined(_MSC_VER)
    return (uint64_t)_InterlockedCompareExchange64(
        (volatile __int64 *)(uintptr_t)a, (__int64)nv, (__int64)cmp);
#else
    uint64_t expected = cmp;
    __atomic_compare_exchange_n((uint64_t *)(uintptr_t)a, &expected, nv, 0,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
#endif
}
static inline uint64_t atomic_cmpxchg64(uint32_t a, uint64_t cmp, uint64_t nv) {
    if ((a & 7u) == 0) return atomic_cmpxchg64_aligned(a, cmp, nv);
    return atomic_cmpxchg64_slow(a, cmp, nv);
}

static inline uint32_t eflags_pack(CPU *c) {
    return 0x202u | (c->cf) | (c->pf << 2) | (c->af << 4) |
           (c->zf << 6) | (c->sf << 7) | (c->of << 11);
}
static inline void eflags_unpack(CPU *c, uint32_t v) {
    c->cf = v & 1; c->pf = (v >> 2) & 1; c->af = (v >> 4) & 1;
    c->zf = (v >> 6) & 1; c->sf = (v >> 7) & 1; c->of = (v >> 11) & 1;
}

/* ---- stack ----
 *
 * ESP is an address in a flat build, so the base is zero and these compile to
 * exactly what they did before. A segmented build defines STACK_BASE to its
 * SS base instead, because there ESP is an OFFSET: 16-bit code reaching its
 * arguments does `mov bp, sp` and then `[bp+6]`, and truncating a host address
 * to 16 bits produces a pointer to nothing. */
#ifndef STACK_BASE
#define STACK_BASE(c) 0u
#endif
static inline void push32(CPU *c, uint32_t v) { c->esp -= 4; wr32(STACK_BASE(c) + c->esp, v); }
static inline uint32_t pop32(CPU *c) { uint32_t v = rd32(STACK_BASE(c) + c->esp); c->esp += 4; return v; }

/* An instruction the lifter could not express. Plain abort() by default, so a
 * project that includes only cpu.h is unchanged; a runtime that can say
 * something useful - which guest address, which mnemonic, how it got there -
 * defines RECOMP_TODO before including this. A bare abort() in two million
 * lines becomes __fastfail in a release build, which no handler sees. */
#ifndef RECOMP_TODO
#define RECOMP_TODO(va, text) abort()
#endif

/* ---- absolute image references: abs VA at preferred base -> live address ----
 *
 * P0-3: both shipped targets use a fixed identity mapping for the corpus
 * (docs/GUEST_ABI.md ABI-1/ABI-2), so the delta is a compile-time zero.  The
 * mutable global remains defined for the legacy HLE diagnostics and for a
 * future relocatable build, but the hot path no longer loads it.  Define
 * GVA_DELTA=1 to opt back into the run-time delta; that is a relocation
 * decision and requires a new corpus identity (GUEST_ABI ABI-9).
 */
#ifndef GVA_DELTA
#define GVA_DELTA 0
#endif
#if GVA_DELTA
extern uint32_t g_image_delta;   /* live_base - PE ImageBase */
#define GVA(abs) ((uint32_t)((abs) + g_image_delta))
#else
#define GVA(abs) ((uint32_t)(abs))
#endif

/* ---- flag helpers ---- */
/* P0-10 field-level DFE.  PF and AF are read by almost no lifted instruction;
 * when a corpus audit proves there is no read, the arithmetic helpers can
 * leave both fields alone and skip parity8().  The switch is OFF by default:
 * without that audit the change is a semantic change, not an optimization. */
#ifndef WC3_FLAGS_FAST_PF_AF
#define WC3_FLAGS_FAST_PF_AF 0
#endif
#if WC3_FLAGS_FAST_PF_AF
#define WC3_SET_PF(c, v) ((void)0)
#define WC3_SET_AF(c, v) ((void)0)
#else
#define WC3_SET_PF(c, v) ((c)->pf = (v))
#define WC3_SET_AF(c, v) ((c)->af = (v))
#endif

/* PF as observed after a flag-producing operation.  With fast PF enabled the
 * helpers deliberately leave the stored PF alone, so a fused jp/jnp condition
 * must read the stored field rather than compute parity from the result. */
#if WC3_FLAGS_FAST_PF_AF
#define WC3_PF_VALUE(c, r) ((c)->pf)
#else
#define WC3_PF_VALUE(c, r) (parity8((uint8_t)(r)))
#endif

static inline uint32_t parity8(uint8_t v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return (~v) & 1;
}

/* logical ops (AND/OR/XOR/TEST): CF=OF=0 */
static inline void flags_logic32(CPU *c, uint32_t r) {
    c->cf = 0; c->of = 0; c->sf = r >> 31; c->zf = (r == 0); WC3_SET_PF(c, parity8((uint8_t)r));
}
static inline void flags_logic8(CPU *c, uint8_t r) {
    c->cf = 0; c->of = 0; c->sf = (r >> 7) & 1; c->zf = (r == 0); WC3_SET_PF(c, parity8(r));
}
/* cmp/sub (a - b) */
static inline uint32_t flags_sub32(CPU *c, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    c->cf = (a < b);
    c->zf = (r == 0);
    c->sf = r >> 31;
    c->of = (((a ^ b) & (a ^ r)) >> 31) & 1;
    WC3_SET_AF(c, ((a ^ b ^ r) >> 4) & 1);
    WC3_SET_PF(c, parity8((uint8_t)r));
    return r;
}
/* add (a + b) */
static inline uint32_t flags_add32(CPU *c, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    c->cf = (r < a);
    c->zf = (r == 0);
    c->sf = r >> 31;
    c->of = ((~(a ^ b) & (a ^ r)) >> 31) & 1;
    WC3_SET_AF(c, ((a ^ b ^ r) >> 4) & 1);
    WC3_SET_PF(c, parity8((uint8_t)r));
    return r;
}
/* inc/dec: like add/sub by 1 but CF preserved */
static inline uint32_t flags_inc32(CPU *c, uint32_t a) {
    uint32_t keepcf = c->cf, r = flags_add32(c, a, 1); c->cf = keepcf; return r;
}
static inline uint32_t flags_dec32(CPU *c, uint32_t a) {
    uint32_t keepcf = c->cf, r = flags_sub32(c, a, 1); c->cf = keepcf; return r;
}

/* ---- width-generic flag helpers (sz in {1,2,4}); used by generated code ---- */
static inline uint32_t mask_sz(int sz) { return sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
static inline uint32_t sign_sz(int sz) { return sz == 1 ? 0x80u : sz == 2 ? 0x8000u : 0x80000000u; }

/* Flagless arithmetic: used when the lifter proved every written flag dead.
 * ADC/SBB still read CF because it is an input, not a flag they write. */
static inline uint32_t add32(uint32_t a, uint32_t b, int sz) { return (a + b) & mask_sz(sz); }
static inline uint32_t sub32(uint32_t a, uint32_t b, int sz) { return (a - b) & mask_sz(sz); }
static inline uint32_t and32(uint32_t a, uint32_t b, int sz) { return (a & b) & mask_sz(sz); }
static inline uint32_t or32(uint32_t a, uint32_t b, int sz) { return (a | b) & mask_sz(sz); }
static inline uint32_t xor32(uint32_t a, uint32_t b, int sz) { return (a ^ b) & mask_sz(sz); }
static inline uint32_t inc32(uint32_t a, int sz) { return (a + 1u) & mask_sz(sz); }
static inline uint32_t dec32(uint32_t a, int sz) { return (a - 1u) & mask_sz(sz); }
static inline uint32_t neg32(uint32_t a, int sz) { return (0u - a) & mask_sz(sz); }
static inline uint32_t adc32(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint64_t full = (uint64_t)a + b + (c->cf & 1u);
    return (uint32_t)full & mask_sz(sz);
}
static inline uint32_t sbb32(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint64_t full = (uint64_t)a - b - (c->cf & 1u);
    return (uint32_t)full & mask_sz(sz);
}

static inline uint32_t flags_sub(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz);
    a &= m; b &= m; uint32_t r = (a - b) & m;
    c->cf = (a < b); c->zf = (r == 0); c->sf = (r & s) != 0;
    c->of = (((a ^ b) & (a ^ r)) & s) != 0; WC3_SET_AF(c, ((a ^ b ^ r) & 0x10) != 0);
    WC3_SET_PF(c, parity8((uint8_t)r)); return r;
}
static inline uint32_t flags_add(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz);
    a &= m; b &= m; uint32_t r = (a + b) & m;
    c->cf = (r < a); c->zf = (r == 0); c->sf = (r & s) != 0;
    c->of = ((~(a ^ b) & (a ^ r)) & s) != 0; WC3_SET_AF(c, ((a ^ b ^ r) & 0x10) != 0);
    WC3_SET_PF(c, parity8((uint8_t)r)); return r;
}
static inline uint32_t flags_logicz(CPU *c, uint32_t r, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz); r &= m;
    c->cf = 0; c->of = 0; c->sf = (r & s) != 0; c->zf = (r == 0);
    WC3_SET_PF(c, parity8((uint8_t)r)); return r;
}
static inline uint32_t flags_incs(CPU *c, uint32_t a, int sz) {
    uint32_t keep = c->cf, r = flags_add(c, a, 1, sz); c->cf = keep; return r;
}
static inline uint32_t flags_decs(CPU *c, uint32_t a, int sz) {
    uint32_t keep = c->cf, r = flags_sub(c, a, 1, sz); c->cf = keep; return r;
}
static inline uint32_t flags_adc(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz), cin = c->cf & 1;
    a &= m; b &= m; uint64_t full = (uint64_t)a + b + cin; uint32_t r = (uint32_t)full & m;
    c->cf = (full >> (sz * 8)) & 1; c->zf = (r == 0); c->sf = (r & s) != 0;
    c->of = ((~(a ^ b) & (a ^ r)) & s) != 0; WC3_SET_AF(c, ((a ^ b ^ r) & 0x10) != 0);
    WC3_SET_PF(c, parity8((uint8_t)r)); return r;
}
static inline uint32_t flags_sbb(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz), bin = c->cf & 1;
    a &= m; b &= m; uint64_t full = (uint64_t)a - b - bin; uint32_t r = (uint32_t)full & m;
    c->cf = (full >> (sz * 8)) & 1; c->zf = (r == 0); c->sf = (r & s) != 0;
    c->of = (((a ^ b) & (a ^ r)) & s) != 0; WC3_SET_AF(c, ((a ^ b ^ r) & 0x10) != 0);
    WC3_SET_PF(c, parity8((uint8_t)r)); return r;
}

static inline uint32_t op_shl(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); cnt &= 31; if (!cnt) return v & m;
    uint32_t w = (uint32_t)sz * 8u;
    if (cnt >= w) {                 /* count >= width: result 0, CF undefined */
        c->cf = 0; uint32_t r = 0;
        c->zf = 1; c->sf = 0; WC3_SET_PF(c, parity8((uint8_t)r)); return r;
    }
    c->cf = (v >> (w - cnt)) & 1; uint32_t r = (v << cnt) & m;
    c->zf = (r == 0); c->sf = (r & sign_sz(sz)) != 0; WC3_SET_PF(c, parity8((uint8_t)r)); return r;
}
static inline uint32_t op_shr(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); v &= m; cnt &= 31; if (!cnt) return v;
    uint32_t w = (uint32_t)sz * 8u;
    if (cnt >= w) {                 /* count >= width: result 0, CF undefined */
        c->cf = 0; uint32_t r = 0;
        c->zf = 1; c->sf = 0; WC3_SET_PF(c, parity8((uint8_t)r)); return r;
    }
    c->cf = (v >> (cnt - 1)) & 1; uint32_t r = v >> cnt;
    c->zf = (r == 0); c->sf = (r & sign_sz(sz)) != 0; WC3_SET_PF(c, parity8((uint8_t)r)); return r;
}
static inline uint32_t op_sar(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz); v &= m; cnt &= 31; if (!cnt) return v;
    uint32_t w = (uint32_t)sz * 8u, r;
    if (cnt >= w) {                 /* count >= width: sign-replicated, CF undefined */
        c->cf = 0;
        r = (v & s) ? m : 0;
    } else {
        uint32_t ext = (v & s) ? (m << (w - cnt)) & m : 0;
        c->cf = (v >> (cnt - 1)) & 1;
        r = ((v >> cnt) | ext) & m;
    }
    c->zf = (r == 0); c->sf = (r & s) != 0; WC3_SET_PF(c, parity8((uint8_t)r)); return r;
}

/* ---- rotates. Unlike the shifts these leave ZF/SF/PF alone, which is not an
   omission: x86 does not touch them here, and code that rotates then branches
   on the zero flag is reading the flag the PREVIOUS instruction set. ---- */
static inline uint32_t op_rol(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8; v &= m;
    cnt &= 31; cnt %= (uint32_t)w; if (!cnt) return v;
    uint32_t r = ((v << cnt) | (v >> (w - cnt))) & m;
    c->cf = r & 1;
    c->of = (((r >> (w - 1)) & 1) ^ (c->cf & 1));
    return r;
}
static inline uint32_t op_ror(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8; v &= m;
    cnt &= 31; cnt %= (uint32_t)w; if (!cnt) return v;
    uint32_t r = ((v >> cnt) | (v << (w - cnt))) & m;
    c->cf = (r >> (w - 1)) & 1;
    c->of = (((r >> (w - 1)) & 1) ^ ((r >> (w - 2)) & 1));
    return r;
}

/* A.28: value-only forms.  They must match the helpers above exactly,
 * including the count >= width cases and rotate modulo; only the flag
 * stores are omitted. */
static inline uint32_t op_shl_nf(uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); cnt &= 31; if (!cnt) return v & m;
    uint32_t w = (uint32_t)sz * 8u;
    if (cnt >= w) return 0;
    return (v << cnt) & m;
}
static inline uint32_t op_shr_nf(uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); v &= m; cnt &= 31; if (!cnt) return v;
    uint32_t w = (uint32_t)sz * 8u;
    if (cnt >= w) return 0;
    return v >> cnt;
}
static inline uint32_t op_sar_nf(uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz); v &= m; cnt &= 31;
    if (!cnt) return v;
    uint32_t w = (uint32_t)sz * 8u, r;
    if (cnt >= w) {
        r = (v & s) ? m : 0;
    } else {
        uint32_t ext = (v & s) ? (m << (w - cnt)) & m : 0;
        r = ((v >> cnt) | ext) & m;
    }
    return r;
}
static inline uint32_t op_rol_nf(uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8; v &= m;
    cnt &= 31; cnt %= (uint32_t)w; if (!cnt) return v;
    return ((v << cnt) | (v >> (w - cnt))) & m;
}
static inline uint32_t op_ror_nf(uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8; v &= m;
    cnt &= 31; cnt %= (uint32_t)w; if (!cnt) return v;
    return ((v >> cnt) | (v << (w - cnt))) & m;
}

/* ---- byte order ----
 *
 * bswap reverses the four bytes of a 32-bit register and touches no flags.
 * It is how a compiler writes htonl/ntohl inline, so it turns up in any code
 * that opens a socket, next to a `ror bx, 8` doing the 16-bit half.
 *
 * The 16-bit form is architecturally undefined - Intel documents it as such
 * and real parts leave the register zeroed - so a compiler never emits it and
 * this deliberately does not pretend to implement it. */
static inline uint32_t op_bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0x0000FF00u)
         | ((v << 8) & 0x00FF0000u) | (v << 24);
}

/* ---- double-precision shifts: shift `d`, feeding in bits from `s` ---- */
static inline uint32_t op_shld(CPU *c, uint32_t d, uint32_t s, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8;
    d &= m; s &= m; cnt &= 31;
    if (!cnt || cnt > (uint32_t)w) return d;      /* count > width is undefined */
    uint32_t r = ((d << cnt) | (s >> (w - cnt))) & m;
    c->cf = (d >> (w - cnt)) & 1;
    c->zf = (r == 0); c->sf = (r & sign_sz(sz)) != 0; WC3_SET_PF(c, parity8((uint8_t)r));
    return r;
}
/* BSF/BSR: the index of the lowest (or highest) set bit. The flag is the
 * whole interface - ZF says whether there WAS one - and when the source is
 * zero the destination is architecturally undefined, so it is left alone
 * rather than given a value that code might come to depend on. */
static inline uint32_t op_bsf(CPU *c, uint32_t d, uint32_t s, int sz) {
    s &= mask_sz(sz);
    c->zf = (s == 0);
    if (s == 0) return d;
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_ctz(s);
#else
    uint32_t i, n = (uint32_t)sz * 8;
    for (i = 0; i < n; i++) if (s & (1u << i)) return i;
    return d;
#endif
}

static inline uint32_t op_bsr(CPU *c, uint32_t d, uint32_t s, int sz) {
    s &= mask_sz(sz);
    c->zf = (s == 0);
    if (s == 0) return d;
#if defined(__GNUC__) || defined(__clang__)
    return 31u - (uint32_t)__builtin_clz(s);
#else
    uint32_t i, n = (uint32_t)sz * 8;
    for (i = n; i-- > 0; ) if (s & (1u << i)) return i;
    return d;
#endif
}

/* BT/BTS/BTR/BTC: the selected bit into CF, then the register form's own
 * change to it. The count is taken modulo the operand width - which is what
 * makes `bt eax, 33` test bit 1 and not read past the register. Only the
 * register form is expressed here; the memory form of these addresses a bit
 * string that can run past the operand, and no lifted site uses it. */
static inline uint32_t op_bittest(CPU *c, uint32_t d, uint32_t bit, int sz, int op) {
    uint32_t mask = 1u << (bit & ((uint32_t)sz * 8 - 1));
    c->cf = (d & mask) ? 1 : 0;
    switch (op) {
        case 1: return d | mask;      /* bts */
        case 2: return d & ~mask;     /* btr */
        case 3: return d ^ mask;      /* btc */
        default: return d;            /* bt  */
    }
}

/* RCL/RCR rotate through CF, so the rotated quantity is one bit wider than
 * the operand.  x86 masks the count to five bits first; for 8- and 16-bit
 * operands the result is then taken modulo the nine- or seventeen-bit width.
 * The operand and CF are packed into one uint64_t and rotated in one step. */
static inline uint32_t op_rcl(CPU *c, uint32_t d, uint32_t cnt, int sz) {
    uint32_t n = (uint32_t)sz * 8u;
    uint32_t mask = (n == 32u) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    uint64_t limit = ((uint64_t)1u << (n + 1u)) - 1u;
    cnt &= 31u;
    if (cnt == 0u) return d;       /* a zero count changes no flag at all */
    if (n < 32u) {
        cnt %= n + 1u;
        if (cnt == 0u) return d;
    }
    uint64_t value = ((uint64_t)d & mask) | ((uint64_t)(c->cf & 1u) << n);
    uint64_t rotated = ((value << cnt) | (value >> ((n + 1u) - cnt))) & limit;
    c->cf = (uint32_t)((rotated >> n) & 1u);
    uint32_t r = (uint32_t)rotated & mask;
    /* OF is defined only for a count of one; hardware leaves something there
     * for the rest and nothing may read it. */
    if (cnt == 1u) c->of = (uint32_t)(((r >> (n - 1u)) & 1u) ^ c->cf);
    return r;
}

static inline uint32_t op_rcr(CPU *c, uint32_t d, uint32_t cnt, int sz) {
    uint32_t n = (uint32_t)sz * 8u;
    uint32_t mask = (n == 32u) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    uint64_t limit = ((uint64_t)1u << (n + 1u)) - 1u;
    cnt &= 31u;
    if (cnt == 0u) return d;
    if (n < 32u) {
        cnt %= n + 1u;
        if (cnt == 0u) return d;
    }
    uint64_t value = ((uint64_t)d & mask) | ((uint64_t)(c->cf & 1u) << n);
    uint64_t rotated = ((value >> cnt) | (value << ((n + 1u) - cnt))) & limit;
    c->cf = (uint32_t)((rotated >> n) & 1u);
    uint32_t r = (uint32_t)rotated & mask;
    /* RCR's OF is the XOR of the two top bits of the result; only the
     * count-of-one case is architecturally defined. */
    if (cnt == 1u)
        c->of = (uint32_t)(((r >> (n - 1u)) & 1u) ^ ((r >> (n - 2u)) & 1u));
    return r;
}

static inline uint32_t op_shrd(CPU *c, uint32_t d, uint32_t s, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8;
    d &= m; s &= m; cnt &= 31;
    if (!cnt || cnt > (uint32_t)w) return d;
    uint32_t r = ((d >> cnt) | (s << (w - cnt))) & m;
    c->cf = (d >> (cnt - 1)) & 1;
    c->zf = (r == 0); c->sf = (r & sign_sz(sz)) != 0; WC3_SET_PF(c, parity8((uint8_t)r));
    return r;
}

/* Flagless forms used by A.24 when liveness proves the written flags dead.
 * RCL/RCR still read CF: it is an input to the rotate, not a result. */
static inline uint32_t op_shld_nf(uint32_t d, uint32_t s, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8;
    d &= m; s &= m; cnt &= 31;
    if (!cnt || cnt > (uint32_t)w) return d;
    return ((d << cnt) | (s >> (w - cnt))) & m;
}
static inline uint32_t op_shrd_nf(uint32_t d, uint32_t s, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8;
    d &= m; s &= m; cnt &= 31;
    if (!cnt || cnt > (uint32_t)w) return d;
    return ((d >> cnt) | (s << (w - cnt))) & m;
}
static inline uint32_t op_bsf_nf(uint32_t d, uint32_t s, int sz) {
    s &= mask_sz(sz);
    if (s == 0) return d;
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_ctz(s);
#else
    uint32_t i, n = (uint32_t)sz * 8;
    for (i = 0; i < n; i++) if (s & (1u << i)) return i;
    return d;
#endif
}
static inline uint32_t op_bsr_nf(uint32_t d, uint32_t s, int sz) {
    s &= mask_sz(sz);
    if (s == 0) return d;
#if defined(__GNUC__) || defined(__clang__)
    return 31u - (uint32_t)__builtin_clz(s);
#else
    uint32_t i, n = (uint32_t)sz * 8;
    for (i = n; i-- > 0; ) if (s & (1u << i)) return i;
    return d;
#endif
}
static inline uint32_t op_bittest_nf(uint32_t d, uint32_t bit, int sz, int op) {
    uint32_t mask = 1u << (bit & ((uint32_t)sz * 8 - 1));
    switch (op) {
        case 1: return d | mask;
        case 2: return d & ~mask;
        case 3: return d ^ mask;
        default: return d;
    }
}
static inline uint32_t op_rcl_nf(CPU *c, uint32_t d, uint32_t cnt, int sz) {
    uint32_t n = (uint32_t)sz * 8u;
    uint32_t mask = (n == 32u) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    uint64_t limit = ((uint64_t)1u << (n + 1u)) - 1u;
    cnt &= 31u;
    if (cnt == 0u) return d;
    if (n < 32u) { cnt %= n + 1u; if (cnt == 0u) return d; }
    uint64_t value = ((uint64_t)d & mask) | ((uint64_t)(c->cf & 1u) << n);
    uint64_t rotated = ((value << cnt) | (value >> ((n + 1u) - cnt))) & limit;
    return (uint32_t)rotated & mask;
}
static inline uint32_t op_rcr_nf(CPU *c, uint32_t d, uint32_t cnt, int sz) {
    uint32_t n = (uint32_t)sz * 8u;
    uint32_t mask = (n == 32u) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    uint64_t limit = ((uint64_t)1u << (n + 1u)) - 1u;
    cnt &= 31u;
    if (cnt == 0u) return d;
    if (n < 32u) { cnt %= n + 1u; if (cnt == 0u) return d; }
    uint64_t value = ((uint64_t)d & mask) | ((uint64_t)(c->cf & 1u) << n);
    uint64_t rotated = ((value >> cnt) | (value << ((n + 1u) - cnt))) & limit;
    return (uint32_t)rotated & mask;
}

/* ---- shifts (set flags like x86; count masked to 5 bits) ---- */
static inline uint32_t shr32(CPU *c, uint32_t v, uint32_t cnt) {
    cnt &= 31; if (!cnt) return v;
    c->cf = (v >> (cnt - 1)) & 1;
    uint32_t r = v >> cnt;
    c->zf = (r == 0); c->sf = r >> 31; WC3_SET_PF(c, parity8((uint8_t)r));
    return r;
}
static inline uint32_t shl32(CPU *c, uint32_t v, uint32_t cnt) {
    cnt &= 31; if (!cnt) return v;
    c->cf = (v >> (32 - cnt)) & 1;
    uint32_t r = v << cnt;
    c->zf = (r == 0); c->sf = r >> 31; WC3_SET_PF(c, parity8((uint8_t)r));
    return r;
}
static inline uint8_t shr8(CPU *c, uint8_t v, uint32_t cnt) {
    cnt &= 31; if (!cnt) return v;
    if (cnt <= 8) c->cf = (v >> (cnt - 1)) & 1;
    uint8_t r = (cnt < 8) ? (uint8_t)(v >> cnt) : 0;
    c->zf = (r == 0); c->sf = (r >> 7) & 1; WC3_SET_PF(c, parity8(r));
    return r;
}

/* ---- x87 FPU ----
 * The native double model lives in x87.h.  It must be included after CPU is
 * complete.  cpu_x87_init() is the architectural FNINIT/FNSAVE reset;
 * cpu_x87_init_thread() is the Windows/MSVC default used when the runtime
 * creates a guest thread. */
static inline void cpu_x87_init(CPU *c);
#include "x87.h"

static inline void cpu_x87_init(CPU *c) {
    x87_init(c, 0x037F);   /* round nearest, 64-bit precision, all masked */
}
static inline void cpu_x87_init_thread(CPU *c) {
    x87_init(c, 0x027F);   /* Windows/MSVC default control word */
}

/* ---- virtual x87 stack (L2) ------------------------------------------- */
/* The C lifter keeps the x87 stack in local st[8]/x87_top/x87_tag within a
 * basic block and flushes to CPU at labels, branches, calls and observation
 * points. These are the local-stack primitives; the generated C declares the
 * locals and calls vs_load/vs_flush around the block. */
#define VS_TAG(tag, i)   (((tag) >> (((i) & 7) * 2)) & 3u)
#define VS_SET_TAG(tag, i, t) \
    ((tag) = (uint16_t)(((tag) & ~(3u << (((i) & 7) * 2))) | \
                        (((t) & 3u) << (((i) & 7) * 2))))

static inline void vs_flush(CPU *c, double *st, int top, uint16_t tag) {
    c->fpu_top = top;
    x87_set_top(c);
    c->fpu_tag = tag;
    for (int i = 0; i < 8; i++)
        c->st[i].v.d = st[i];
}
static inline void vs_load(CPU *c, double *st, int *top, uint16_t *tag) {
    *top = c->fpu_top;
    *tag = c->fpu_tag;
    for (int i = 0; i < 8; i++)
        st[i] = c->st[i].v.d;
}
static inline void vs_push(CPU *c, double *st, int *top, uint16_t *tag, double v) {
    const int p = (*top - 1) & 7;
    if (VS_TAG(*tag, p) != 3) x87_stack_fault(c);
    *top = p;
    st[p] = v;
    VS_SET_TAG(*tag, p, x87_tag_for(v));
}
static inline double vs_pop(CPU *c, double *st, int *top, uint16_t *tag) {
    const int p = *top;
    const double v = st[p];
    if (VS_TAG(*tag, p) == 3) x87_stack_fault(c);
    VS_SET_TAG(*tag, p, 3);
    *top = (*top + 1) & 7;
    return v;
}
static inline double *vs_fst(double *st, int top, int i) {
    return &st[(top + i) & 7];
}
static inline void vs_fst_reg(double *st, int top, uint16_t *tag, int i) {
    const int d = (top + i) & 7, s = top;
    st[d] = st[s];
    VS_SET_TAG(*tag, d, VS_TAG(*tag, s));
}
static inline void vs_fxch(double *st, int top, uint16_t *tag, int i) {
    const int a = top, b = (top + i) & 7;
    const double t = st[a];
    const uint16_t tt = VS_TAG(*tag, a);
    st[a] = st[b]; VS_SET_TAG(*tag, a, VS_TAG(*tag, b));
    st[b] = t;     VS_SET_TAG(*tag, b, tt);
}
static inline void vs_fcmov(CPU *c, double *st, int top, uint16_t *tag,
                            int cond, int i) {
    if (!cond) return;
    const int s = (top + i) & 7;
    if (VS_TAG(*tag, s) == 3) { x87_stack_fault(c); return; }
    st[top] = st[s];
    VS_SET_TAG(*tag, top, VS_TAG(*tag, s));
}
static inline void vs_ffree(uint16_t *tag, int top, int i) {
    VS_SET_TAG(*tag, (top + i) & 7, 3);
}
static inline void vs_fchs(double *st, int top) {
    const double d = st[top];
    st[top] = x87_from_bits(x87_bits(d) ^ UINT64_C(0x8000000000000000));
}
static inline void vs_fabs(double *st, int top) {
    const double d = st[top];
    st[top] = x87_from_bits(x87_bits(d) & UINT64_C(0x7FFFFFFFFFFFFFFF));
}

/* Register-friendly virtual x87 stack (A.6 v2).
 *
 * The generated body normalizes the guest stack at a block entry: st[0] is
 * ST(0) at that point and `base` is the physical TOP.  The tag word is
 * rotated into the same virtual order, so every hot access uses a constant
 * index and no pointer to st escapes.  The compiler can keep the eight
 * doubles in registers; only block boundaries copy to/from CPU.  The old
 * vs_* helpers above stay for the corpus lifted before this form. */
static inline uint16_t vs_tag_ror(uint16_t tag, int top) {
    uint32_t t = tag;
    return (uint16_t)((t >> (2 * top)) | (t << (16 - 2 * top)));
}
static inline uint16_t vs_tag_rol(uint16_t tag, int top) {
    uint32_t t = tag;
    return (uint16_t)((t << (2 * top)) | (t >> (16 - 2 * top)));
}
#define VS_LOAD(_c, _st, _tag, _base) do { \
    (_base) = (_c)->fpu_top; \
    (_st)[0] = (_c)->st[((_base) + 0) & 7].v.d; \
    (_st)[1] = (_c)->st[((_base) + 1) & 7].v.d; \
    (_st)[2] = (_c)->st[((_base) + 2) & 7].v.d; \
    (_st)[3] = (_c)->st[((_base) + 3) & 7].v.d; \
    (_st)[4] = (_c)->st[((_base) + 4) & 7].v.d; \
    (_st)[5] = (_c)->st[((_base) + 5) & 7].v.d; \
    (_st)[6] = (_c)->st[((_base) + 6) & 7].v.d; \
    (_st)[7] = (_c)->st[((_base) + 7) & 7].v.d; \
    (_tag) = vs_tag_ror((_c)->fpu_tag, (_base)); \
} while (0)
#define VS_FLUSH(_c, _st, _tag, _base, _top) do { \
    (_c)->st[((_base) + 0) & 7].v.d = (_st)[0]; \
    (_c)->st[((_base) + 1) & 7].v.d = (_st)[1]; \
    (_c)->st[((_base) + 2) & 7].v.d = (_st)[2]; \
    (_c)->st[((_base) + 3) & 7].v.d = (_st)[3]; \
    (_c)->st[((_base) + 4) & 7].v.d = (_st)[4]; \
    (_c)->st[((_base) + 5) & 7].v.d = (_st)[5]; \
    (_c)->st[((_base) + 6) & 7].v.d = (_st)[6]; \
    (_c)->st[((_base) + 7) & 7].v.d = (_st)[7]; \
    (_c)->fpu_top = ((_base) + (_top)) & 7; \
    x87_set_top(_c); \
    (_c)->fpu_tag = vs_tag_rol((_tag), (_base)); \
} while (0)
#define VS_FST(_st, _top, _i) ((_st)[(((_top) + (_i)) & 7)])
#define VS_PUSH(_c, _st, _tag, _top, _val) do { \
    const int _p = (((_top) - 1) & 7); \
    const double _pushv = (_val); \
    if (VS_TAG((_tag), _p) != 3) x87_stack_fault(_c); \
    (_st)[_p] = _pushv; \
    VS_SET_TAG((_tag), _p, x87_tag_for(_pushv)); \
} while (0)
#define VS_POP(_c, _tag, _top) do { \
    const int _p = (_top); \
    if (VS_TAG((_tag), _p) == 3) x87_stack_fault(_c); \
    VS_SET_TAG((_tag), _p, 3); \
} while (0)
#define VS_FST_REG(_st, _tag, _top, _i) do { \
    const int _d = (((_top) + (_i)) & 7), _s = (_top); \
    (_st)[_d] = (_st)[_s]; \
    VS_SET_TAG((_tag), _d, VS_TAG((_tag), _s)); \
} while (0)
#define VS_FXCH(_st, _tag, _top, _i) do { \
    const int _a = (_top), _b = (((_top) + (_i)) & 7); \
    const double _x = (_st)[_a]; \
    const uint16_t _xt = VS_TAG((_tag), _a); \
    (_st)[_a] = (_st)[_b]; \
    VS_SET_TAG((_tag), _a, VS_TAG((_tag), _b)); \
    (_st)[_b] = _x; \
    VS_SET_TAG((_tag), _b, _xt); \
} while (0)
#define VS_FCMOV(_c, _st, _tag, _top, _cond, _i) do { \
    if (_cond) { \
        const int _d = (_top), _s = (((_top) + (_i)) & 7); \
        if (VS_TAG((_tag), _s) == 3) { x87_stack_fault(_c); } \
        else { \
            (_st)[_d] = (_st)[_s]; \
            VS_SET_TAG((_tag), _d, VS_TAG((_tag), _s)); \
        } \
    } \
} while (0)
#define VS_FFREE(_tag, _top, _i) do { \
    VS_SET_TAG((_tag), (((_top) + (_i)) & 7), 3); \
} while (0)
#define VS_FCHS(_st, _top) do { \
    const int _d = (_top); \
    (_st)[_d] = x87_from_bits(x87_bits((_st)[_d]) ^ UINT64_C(0x8000000000000000)); \
} while (0)
#define VS_FABS(_st, _top) do { \
    const int _d = (_top); \
    (_st)[_d] = x87_from_bits(x87_bits((_st)[_d]) & UINT64_C(0x7FFFFFFFFFFFFFFF)); \
} while (0)

/* Double-precision helpers for SSE/scalar code. These are NOT x87 helpers:
 * x87 memory operands use x87_ld_f32/f64 and x87_st_f32/f64 in x87.h. */
static inline double rdf32(uint32_t a) { float  f; memcpy(&f, (void *)(uintptr_t)a, 4); return (double)f; }
static inline double rdf64(uint32_t a) { double d; memcpy(&d, (void *)(uintptr_t)a, 8); return d; }
static inline double rdi16(uint32_t a) { int16_t i; memcpy(&i, (void *)(uintptr_t)a, 2); return (double)i; }
static inline double rdi32(uint32_t a) { int32_t i; memcpy(&i, (void *)(uintptr_t)a, 4); return (double)i; }
static inline double rdi64(uint32_t a) { int64_t i; memcpy(&i, (void *)(uintptr_t)a, 8); return (double)i; }
static inline void   wrf32(uint32_t a, double v) { float  f = (float)v; memcpy((void *)(uintptr_t)a, &f, 4); }
static inline void   wrf64(uint32_t a, double v) { memcpy((void *)(uintptr_t)a, &v, 8); }
static inline void   wri32(uint32_t a, double v) { int32_t i = (int32_t)nearbyint(v); memcpy((void *)(uintptr_t)a, &i, 4); }
static inline void   wri16(uint32_t a, double v) { int16_t i = (int16_t)nearbyint(v); memcpy((void *)(uintptr_t)a, &i, 2); }
static inline void   wri64(uint32_t a, double v) { int64_t i = (int64_t)nearbyint(v); memcpy((void *)(uintptr_t)a, &i, 8); }

/* ---- MMX ---------------------------------------------------------------
 *
 * MMX registers are the low 64 bits of the eight physical x87 registers, not a
 * separate array. `mm_get` exposes that alias without repeating the 16-byte
 * stride of x87_ext. MMX writes share the low 64 bits with the native x87
 * double and mark the physical register valid; EMMS empties all tags. This is
 * the architectural aliasing, so interleaved MMX/x87 code sees one register
 * file.
 */
#define MM_W(v, i)  ((int16_t)((uint64_t)(v) >> ((i) * 16)))
#define MM_D(v, i)  ((int32_t)((uint64_t)(v) >> ((i) * 32)))
#define MM_PUT_W(i, x) ((uint64_t)(uint16_t)(x) << ((i) * 16))
#define MM_PUT_D(i, x) ((uint64_t)(uint32_t)(x) << ((i) * 32))
#define MM_B(v, i)  ((int8_t)((uint64_t)(v) >> ((i) * 8)))
#define MM_UB(v, i) ((uint8_t)((uint64_t)(v) >> ((i) * 8)))
#define MM_UW(v, i) ((uint16_t)((uint64_t)(v) >> ((i) * 16)))
#define MM_PUT_B(i, x) ((uint64_t)(uint8_t)(x) << ((i) * 8))
#define MMX_LANES(n, put, expr) do { for (int i = 0; i < (n); i++) r |= put(i, (expr)); } while (0)

/* The arithmetic/compare helpers are lane loops in portable C.  AArch64 has
 * direct SIMD instructions for all of them, so use those instead of hoping the
 * compiler scalarises and re-vectorises each loop.  The bitwise MMX operations
 * stay as C expressions: the compiler already maps them to one instruction. */
#if defined(__aarch64__)
#include "cpu_mmx_neon.h"
#endif

static inline uint64_t *mm_get(CPU *c, int i) {
    return &c->st[i & 7].v.mm;
}
static inline void mmx_mark(CPU *c, int i) {
    /* MMX writes make the physical register valid, independently of TOP. */
    c->fpu_tag &= (uint16_t)~(3u << ((i & 7) * 2));
}
static inline void mmx_emms(CPU *c) {
    c->fpu_tag = 0xFFFFu;
}
static inline int16_t mmx_sat16(int32_t v) {
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}
#if !defined(__aarch64__)
static inline uint64_t mmx_paddw(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++) r |= MM_PUT_W(i, MM_W(a, i) + MM_W(b, i));
    return r;
}
static inline uint64_t mmx_psubw(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++) r |= MM_PUT_W(i, MM_W(a, i) - MM_W(b, i));
    return r;
}
static inline uint64_t mmx_paddd(uint64_t a, uint64_t b) {
    return MM_PUT_D(0, MM_D(a, 0) + MM_D(b, 0)) | MM_PUT_D(1, MM_D(a, 1) + MM_D(b, 1));
}
static inline uint64_t mmx_psubd(uint64_t a, uint64_t b) {
    return MM_PUT_D(0, MM_D(a, 0) - MM_D(b, 0)) | MM_PUT_D(1, MM_D(a, 1) - MM_D(b, 1));
}
static inline uint64_t mmx_paddsw(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++) r |= MM_PUT_W(i, mmx_sat16(MM_W(a, i) + MM_W(b, i)));
    return r;
}
static inline uint64_t mmx_psubsw(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++) r |= MM_PUT_W(i, mmx_sat16(MM_W(a, i) - MM_W(b, i)));
    return r;
}
static inline uint64_t mmx_pmulhw(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++)
        r |= MM_PUT_W(i, (int16_t)(((int32_t)MM_W(a, i) * MM_W(b, i)) >> 16));
    return r;
}
static inline uint64_t mmx_pmullw(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++) r |= MM_PUT_W(i, (int16_t)((int32_t)MM_W(a, i) * MM_W(b, i)));
    return r;
}
static inline uint64_t mmx_pmaddwd(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 2; i++)
        r |= MM_PUT_D(i, (int32_t)MM_W(a, i*2) * MM_W(b, i*2)
                       + (int32_t)MM_W(a, i*2+1) * MM_W(b, i*2+1));
    return r;
}
static inline uint64_t mmx_punpcklwd(uint64_t a, uint64_t b) {
    return MM_PUT_W(0, MM_W(a,0)) | MM_PUT_W(1, MM_W(b,0))
         | MM_PUT_W(2, MM_W(a,1)) | MM_PUT_W(3, MM_W(b,1));
}
static inline uint64_t mmx_punpckhwd(uint64_t a, uint64_t b) {
    return MM_PUT_W(0, MM_W(a,2)) | MM_PUT_W(1, MM_W(b,2))
         | MM_PUT_W(2, MM_W(a,3)) | MM_PUT_W(3, MM_W(b,3));
}
static inline uint64_t mmx_punpcklbw(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++) {
        r |= (uint64_t)(uint8_t)(a >> (i*8)) << (i*16);
        r |= (uint64_t)(uint8_t)(b >> (i*8)) << (i*16 + 8);
    }
    return r;
}
static inline uint64_t mmx_punpckhbw(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++) {
        r |= (uint64_t)(uint8_t)(a >> (32 + i*8)) << (i*16);
        r |= (uint64_t)(uint8_t)(b >> (32 + i*8)) << (i*16 + 8);
    }
    return r;
}
static inline uint64_t mmx_punpckldq(uint64_t a, uint64_t b) {
    return (uint64_t)(uint32_t)a | ((uint64_t)(uint32_t)b << 32);
}
static inline uint64_t mmx_punpckhdq(uint64_t a, uint64_t b) {
    return (uint64_t)(uint32_t)(a >> 32) | ((uint64_t)(uint32_t)(b >> 32) << 32);
}
static inline uint64_t mmx_psllq(uint64_t a, uint32_t c) { return c > 63 ? 0 : a << c; }
static inline uint64_t mmx_psrlq(uint64_t a, uint32_t c) { return c > 63 ? 0 : a >> c; }
static inline uint64_t mmx_psllw(uint64_t a, uint32_t c) {
    uint64_t r = 0;
    if (c > 15) return 0;
    for (int i = 0; i < 4; i++) r |= MM_PUT_W(i, (uint16_t)MM_W(a, i) << c);
    return r;
}
static inline uint64_t mmx_psrlw(uint64_t a, uint32_t c) {
    uint64_t r = 0;
    if (c > 15) return 0;
    for (int i = 0; i < 4; i++) r |= MM_PUT_W(i, (uint16_t)MM_W(a, i) >> c);
    return r;
}
static inline uint64_t mmx_psraw(uint64_t a, uint32_t c) {
    uint64_t r = 0;
    if (c > 15) c = 15;
    for (int i = 0; i < 4; i++) r |= MM_PUT_W(i, (int16_t)(MM_W(a, i) >> c));
    return r;
}
static inline uint64_t mmx_pslld(uint64_t a, uint32_t c) {
    if (c > 31) return 0;
    return MM_PUT_D(0, (uint32_t)MM_D(a,0) << c) | MM_PUT_D(1, (uint32_t)MM_D(a,1) << c);
}
static inline uint64_t mmx_psrld(uint64_t a, uint32_t c) {
    if (c > 31) return 0;
    return MM_PUT_D(0, (uint32_t)MM_D(a,0) >> c) | MM_PUT_D(1, (uint32_t)MM_D(a,1) >> c);
}
static inline uint64_t mmx_psrad(uint64_t a, uint32_t c) {
    if (c > 31) c = 31;
    return MM_PUT_D(0, MM_D(a,0) >> c) | MM_PUT_D(1, MM_D(a,1) >> c);
}
static inline uint64_t mmx_pshufw(uint64_t a, uint32_t imm) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++)
        r |= MM_PUT_W(i, MM_W(a, (imm >> (2 * i)) & 3));
    return r;
}
static inline uint64_t mmx_packssdw(uint64_t a, uint64_t b) {
    return MM_PUT_W(0, mmx_sat16(MM_D(a,0))) | MM_PUT_W(1, mmx_sat16(MM_D(a,1)))
         | MM_PUT_W(2, mmx_sat16(MM_D(b,0))) | MM_PUT_W(3, mmx_sat16(MM_D(b,1)));
}
static inline uint64_t mmx_packuswb(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++) {
        int16_t v = MM_W(a, i); uint8_t p = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        r |= (uint64_t)p << (i * 8);
        v = MM_W(b, i);         p = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        r |= (uint64_t)p << (32 + i * 8);
    }
    return r;
}
static inline uint64_t mmx_pcmpeqw(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++) r |= MM_PUT_W(i, MM_W(a,i) == MM_W(b,i) ? 0xFFFF : 0);
    return r;
}
static inline uint64_t mmx_pcmpgtw(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++) r |= MM_PUT_W(i, MM_W(a,i) > MM_W(b,i) ? 0xFFFF : 0);
    return r;
}
static inline uint64_t mmx_pcmpeqd(uint64_t a, uint64_t b) {
    return MM_PUT_D(0, MM_D(a,0) == MM_D(b,0) ? -1 : 0) | MM_PUT_D(1, MM_D(a,1) == MM_D(b,1) ? -1 : 0);
}
static inline uint64_t mmx_pcmpgtd(uint64_t a, uint64_t b) {
    return MM_PUT_D(0, MM_D(a,0) > MM_D(b,0) ? -1 : 0) | MM_PUT_D(1, MM_D(a,1) > MM_D(b,1) ? -1 : 0);
}
static inline int mmx_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint64_t mmx_paddb(uint64_t a, uint64_t b)   { uint64_t r = 0; MMX_LANES(8, MM_PUT_B, MM_UB(a,i) + MM_UB(b,i)); return r; }
static inline uint64_t mmx_psubb(uint64_t a, uint64_t b)   { uint64_t r = 0; MMX_LANES(8, MM_PUT_B, MM_UB(a,i) - MM_UB(b,i)); return r; }
static inline uint64_t mmx_paddsb(uint64_t a, uint64_t b)  { uint64_t r = 0; MMX_LANES(8, MM_PUT_B, mmx_clamp(MM_B(a,i) + MM_B(b,i), -128, 127)); return r; }
static inline uint64_t mmx_psubsb(uint64_t a, uint64_t b)  { uint64_t r = 0; MMX_LANES(8, MM_PUT_B, mmx_clamp(MM_B(a,i) - MM_B(b,i), -128, 127)); return r; }
static inline uint64_t mmx_paddusb(uint64_t a, uint64_t b) { uint64_t r = 0; MMX_LANES(8, MM_PUT_B, mmx_clamp(MM_UB(a,i) + MM_UB(b,i), 0, 255)); return r; }
static inline uint64_t mmx_psubusb(uint64_t a, uint64_t b) { uint64_t r = 0; MMX_LANES(8, MM_PUT_B, mmx_clamp(MM_UB(a,i) - MM_UB(b,i), 0, 255)); return r; }
static inline uint64_t mmx_paddusw(uint64_t a, uint64_t b) { uint64_t r = 0; MMX_LANES(4, MM_PUT_W, mmx_clamp(MM_UW(a,i) + MM_UW(b,i), 0, 65535)); return r; }
static inline uint64_t mmx_psubusw(uint64_t a, uint64_t b) { uint64_t r = 0; MMX_LANES(4, MM_PUT_W, mmx_clamp(MM_UW(a,i) - MM_UW(b,i), 0, 65535)); return r; }
static inline uint64_t mmx_pcmpeqb(uint64_t a, uint64_t b) { uint64_t r = 0; MMX_LANES(8, MM_PUT_B, MM_UB(a,i) == MM_UB(b,i) ? 0xFF : 0); return r; }
static inline uint64_t mmx_pcmpgtb(uint64_t a, uint64_t b) { uint64_t r = 0; MMX_LANES(8, MM_PUT_B, MM_B(a,i) > MM_B(b,i) ? 0xFF : 0); return r; }
static inline uint64_t mmx_packsswb(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    MMX_LANES(4, MM_PUT_B, mmx_clamp(MM_W(a,i), -128, 127));
    for (int i = 0; i < 4; i++) r |= MM_PUT_B(4 + i, mmx_clamp(MM_W(b,i), -128, 127));
    return r;
}
#endif /* !__aarch64__ */


/* fcom/fcomi are in x87.h: the comparison uses the shared double slot and the
 * guest C0-C3 status bits. */
/* sahf: load AH into CF,PF,AF,ZF,SF */
static inline void do_sahf(CPU *c, uint8_t ah) {
    c->cf = ah & 1; c->pf = (ah >> 2) & 1; c->af = (ah >> 4) & 1;
    c->zf = (ah >> 6) & 1; c->sf = (ah >> 7) & 1;
}

/* ---- SSE ----
 *
 * Enough of SSE/SSE2 for a Pentium 4-era game: scalar float and double, the
 * packed moves and bitwise ops, and the compares. Packed arithmetic beyond
 * that is not here yet - it lifts to a TODO rather than silently wrong code.
 */

/* Memory operands. Separate from the x87 rdf32/rdf64 above because those
 * widen to double for the 80-bit stack, and these must not. memcpy because an
 * SSE memory operand is frequently unaligned (movups exists for a reason). */
static inline float  rdss(uint32_t a) { float  f; memcpy(&f, (void *)(uintptr_t)a, 4); return f; }
static inline double rdsd(uint32_t a) { double d; memcpy(&d, (void *)(uintptr_t)a, 8); return d; }
static inline XMM    rdxm(uint32_t a) { XMM x;    memcpy(&x, (void *)(uintptr_t)a, 16); return x; }
static inline void   wrss(uint32_t a, float  v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static inline void   wrsd(uint32_t a, double v) { memcpy((void *)(uintptr_t)a, &v, 8); }
static inline void   wrxm(uint32_t a, XMM    v) { memcpy((void *)(uintptr_t)a, &v, 16); }

/* MOVSS/MOVSD loading from memory zero the rest of the register; the
 * register-to-register forms leave the high lanes alone. Getting that
 * backwards is invisible until something reads the high lanes, which is why
 * it is a named helper and not an open-coded assignment. */
static inline void sse_load_ss(XMM *d, float  v) { d->u64[0] = 0; d->u64[1] = 0; d->f32[0] = v; }
static inline void sse_load_sd(XMM *d, double v) { d->u64[0] = 0; d->u64[1] = 0; d->f64[0] = v; }

/* UCOMISS/COMISS/UCOMISD/COMISD: the comparison lands in ZF/PF/CF, and clears
 * OF/SF/AF. An unordered compare - either operand NaN - sets all three, which
 * is exactly what makes `ucomiss; jp` a NaN test and `ucomiss; jbe` mean
 * "below or equal, or unordered". Note the flags are the UNSIGNED set: the
 * compiler emits `ja`/`jbe` after a float compare, never `jg`/`jle`. */
static inline void sse_compare(CPU *c, double a, double b) {
    c->of = 0; c->sf = 0; c->af = 0;
    if (a != a || b != b) { c->zf = 1; c->pf = 1; c->cf = 1; }   /* unordered */
    else                  { c->zf = (a == b); c->pf = 0; c->cf = (a < b); }
}

/* A.8: LAHF after an SSE compare.  AH = SF<<7 | ZF<<6 | AF<<4 | PF<<2 |
 * 1<<1 | CF; the compare clears SF/AF and sets the unordered triple. */
static inline void sse_compare_lahf(CPU *c, double a, double b) {
    uint32_t ah = 2u;
    if (a != a || b != b) ah |= 0x45u;              /* PF|ZF|CF */
    else if (a < b)       ah |= 1u;                /* CF */
    else if (a == b)      ah |= 0x40u;             /* ZF */
    SET8H(c->eax, ah);
}

/* MINSS/MAXSS are not fmin/fmax. The manual defines them as
 *     if (dst OP src) then dst else src
 * and that "else src" is load-bearing in two cases fmin gets the other way
 * round: when the operands are equal - which includes +0.0 against -0.0, so
 * min(+0,-0) is -0 but min(-0,+0) is +0 - and when either is NaN, where the
 * comparison is false and the SECOND operand wins. Written in that exact
 * shape, C's own "any compare with NaN is false" rule delivers both for free.
 * Writing it the other way round, `(src < dst) ? src : dst`, looks identical
 * and is wrong on both. */
static inline float  sse_minf(float  d, float  s) { return (d < s) ? d : s; }
static inline float  sse_maxf(float  d, float  s) { return (d > s) ? d : s; }
static inline double sse_mind(double d, double s) { return (d < s) ? d : s; }
static inline double sse_maxd(double d, double s) { return (d > s) ? d : s; }

/* CVTTSS2SI and friends yield the "integer indefinite" value when the source
 * does not fit in 32 bits, where the equivalent C cast is undefined behaviour
 * and may trap. One comparison buys a defined answer that matches hardware. */
static inline int32_t sse_cvtt_i32(double v) {
    return (v >= -2147483648.0 && v < 2147483648.0) ? (int32_t)v : (int32_t)0x80000000;
}

/* ---- BT / BTS / BTR / BTC ----
 *
 * With a memory destination and a register bit index, the index is NOT masked
 * to the operand size: it selects a dword away from the base address, which is
 * how a bitmap of arbitrary size gets addressed. Masking it the way the
 * register form does would quietly read and write the wrong word - the kind of
 * wrong that shows up as unrelated corruption much later.
 *
 * op: 0 test, 1 set, 2 reset, 3 complement. CF receives the bit as it was. */
static inline uint32_t bit_string_op(CPU *c, uint32_t addr, int32_t idx, int op)
{
    uint32_t a = addr + 4u * (uint32_t)(idx >> 5);
    unsigned b = (unsigned)(idx & 31);
    uint32_t w = rd32(a);
    c->cf = (w >> b) & 1u;
    if      (op == 1) w |=  (1u << b);
    else if (op == 2) w &= ~(1u << b);
    else if (op == 3) w ^=  (1u << b);
    if (op) wr32(a, w);
    return c->cf;
}

/* A.24: same memory access and write, but CF is dead and not materialized. */
static inline void bit_string_op_nf(uint32_t addr, int32_t idx, int op)
{
    uint32_t a = addr + 4u * (uint32_t)(idx >> 5);
    unsigned b = (unsigned)(idx & 31);
    uint32_t w = rd32(a);
    if      (op == 1) w |=  (1u << b);
    else if (op == 2) w &= ~(1u << b);
    else if (op == 3) w ^=  (1u << b);
    if (op) wr32(a, w);
}

/* The register form, where the index really is taken modulo the width. */
static inline uint32_t bit_reg_op(CPU *c, uint32_t *r, uint32_t idx, int op)
{
    unsigned b = idx & 31u;
    c->cf = (*r >> b) & 1u;
    if      (op == 1) *r |=  (1u << b);
    else if (op == 2) *r &= ~(1u << b);
    else if (op == 3) *r ^=  (1u << b);
    return c->cf;
}

/* x87 memory conversion, stack, comparison and fprem are all in x87.h. */
/* The packed integer ops that saturate rather than wrap. PADDSW and friends
 * clamp to the lane's own range instead of carrying into the next lane, and
 * the arithmetic has to be done a width up for the clamp to see the overflow
 * at all - which is the whole reason these are functions and not expressions
 * in the generated code. */
static inline int8_t   sse_sat_i8 (int32_t v) { return v >  127 ?  127 : v < -128 ? -128 : (int8_t)v; }
static inline int16_t  sse_sat_i16(int32_t v) { return v >  32767 ?  32767 : v < -32768 ? -32768 : (int16_t)v; }
static inline uint8_t  sse_sat_u8 (int32_t v) { return v >  255 ?  255 : v < 0 ? 0 : (uint8_t)v; }
static inline uint16_t sse_sat_u16(int32_t v) { return v > 65535 ? 65535 : v < 0 ? 0 : (uint16_t)v; }

/* Packed integer SSE: one ASIMD instruction on AArch64, scalar here. */
#include "cpu_sse_int.h"
/* Packed FP SSE: ASIMD arithmetic with a scalar NaN fallback, scalar here. */
#include "cpu_sse_fp.h"
#include "cpu_bulk.h"

#endif /* PCRECOMP_CPU_H */
