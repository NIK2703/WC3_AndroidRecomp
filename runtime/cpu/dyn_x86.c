/*
 * dyn_x86.c - executor for guest code generated at runtime.
 *
 * This is deliberately a small instruction interpreter, not a JIT and not a
 * general x86 emulator.  It executes code that did not exist at lift time:
 *
 *   - ver-IX86-3.dll's CheckRevision equation compiler emits `mov eax, 0;
 *     jmp body` followed by unrolled integer register/memory operations and a
 *     jump back into the DLL's hash continuation;
 *   - Warden allocates a `0x6000`-byte image (text at `0x40041000+0x19F7`)
 *     and calls into it for its SHA-1/RC4/checksum work.
 *
 * The interpreter covers the integer forms present in the static Warden text,
 * including the guest stack effects of ret/ret imm16.  It assumes DF=0 (Warden
 * never executes CLD/STD) and aborts on a guest #DE instead of synthesizing an
 * exception; both limits are recorded in docs/WARDEN_DYN_OPCODE_CRASH.md.
 */
#include "dyn_x86.h"
#include "wc3_debug.h"

#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
#include <stdio.h>
#endif
#include <stdlib.h>

/* Executable guest ranges are registered by VirtualAlloc when the protection
 * has an execute bit.  Allocation and dispatch can happen on different guest
 * threads, so the table uses lock-free acquire/release atomics (the same
 * primitive the runtime allows for mailboxes).  A reserved base marks a slot
 * whose size is being published.  No host lock is taken, which preserves the
 * runtime lock invariant. */
#define WC3_DYN_RANGE_MAX 64
#define WC3_DYN_RESERVED 0xFFFFFFFFu
static uint32_t g_dyn_base[WC3_DYN_RANGE_MAX];
static uint32_t g_dyn_size[WC3_DYN_RANGE_MAX];

#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
/* Bounded trace of the calls the generated code makes back into the lifted
 * corpus.  Warden's entry is the first non-CheckRevision producer, so its
 * first few call targets are the useful evidence when an interpreter
 * instruction is still missing. */
static unsigned g_dyn_call_trace;
static void dyn_trace_call(uint32_t site, uint32_t target, uint32_t ret)
{
    if (g_dyn_call_trace++ < 128)
        fprintf(stderr, "[dyn] call %08X -> %08X ret %08X\n",
                site, target, ret);
}
#endif

void wc3_dyn_x86_register(uint32_t base, uint32_t size)
{
    if (!base || !size) return;
    for (unsigned i = 0; i < WC3_DYN_RANGE_MAX; i++) {
        if (__atomic_load_n(&g_dyn_base[i], __ATOMIC_ACQUIRE) == base) {
            __atomic_store_n(&g_dyn_size[i], size, __ATOMIC_RELEASE);
            return;
        }
    }
    for (unsigned i = 0; i < WC3_DYN_RANGE_MAX; i++) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&g_dyn_base[i], &expected,
                                        WC3_DYN_RESERVED, 0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            /* Publish the size before the base so a concurrent reader that
             * sees the real base also sees the matching size. */
            __atomic_store_n(&g_dyn_size[i], size, __ATOMIC_RELAXED);
            __atomic_store_n(&g_dyn_base[i], base, __ATOMIC_RELEASE);
            return;
        }
    }
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
    fprintf(stderr, "[dyn] executable range table full at %08X+%X\n",
            base, size);
#endif
}

void wc3_dyn_x86_unregister(uint32_t base)
{
    for (unsigned i = 0; i < WC3_DYN_RANGE_MAX; i++) {
        if (__atomic_load_n(&g_dyn_base[i], __ATOMIC_ACQUIRE) == base) {
            __atomic_store_n(&g_dyn_base[i], 0, __ATOMIC_RELEASE);
            __atomic_store_n(&g_dyn_size[i], 0, __ATOMIC_RELAXED);
        }
    }
}

int wc3_dyn_x86_contains(uint32_t va)
{
    for (unsigned i = 0; i < WC3_DYN_RANGE_MAX; i++) {
        uint32_t b = __atomic_load_n(&g_dyn_base[i], __ATOMIC_ACQUIRE);
        uint32_t s = __atomic_load_n(&g_dyn_size[i], __ATOMIC_RELAXED);
        if (b && b != WC3_DYN_RESERVED && va >= b &&
            (uint64_t)va - b < s)
            return 1;
    }
    return 0;
}

typedef struct {
    int is_reg;
    int reg;
    uint32_t addr;
} DynRM;

static uint32_t *dyn_reg(CPU *c, int r)
{
    switch (r) {
    case 0: return &c->eax;
    case 1: return &c->ecx;
    case 2: return &c->edx;
    case 3: return &c->ebx;
    case 4: return &c->esp;
    case 5: return &c->ebp;
    case 6: return &c->esi;
    case 7: return &c->edi;
    }
    return &c->eax;
}

static DynRM dyn_rm(CPU *c, uint32_t *pc)
{
    DynRM r;
    uint8_t modrm = rd8((*pc)++);
    int mod = modrm >> 6, rm = modrm & 7;
    r.reg = rm;
    r.is_reg = (mod == 3);
    if (mod == 3) return r;
    uint32_t addr;
    if (rm == 4) {
        uint8_t sib = rd8((*pc)++);
        int scale = 1 << (sib >> 6);
        int index = (sib >> 3) & 7;
        int base = sib & 7;
        if (base == 5 && mod == 0) { addr = rd32(*pc); *pc += 4; }
        else addr = *dyn_reg(c, base);
        if (index != 4) addr += *dyn_reg(c, index) * (uint32_t)scale;
        if (mod == 1) { addr += (uint32_t)(int32_t)(int8_t)rd8(*pc); *pc += 1; }
        else if (mod == 2) { addr += rd32(*pc); *pc += 4; }
    } else if (rm == 5 && mod == 0) {
        addr = rd32(*pc); *pc += 4;
    } else {
        addr = *dyn_reg(c, rm);
        if (mod == 1) { addr += (uint32_t)(int32_t)(int8_t)rd8(*pc); *pc += 1; }
        else if (mod == 2) { addr += rd32(*pc); *pc += 4; }
    }
    r.addr = addr;
    return r;
}

static uint32_t dyn_get(CPU *c, const DynRM *r)
{
    return r->is_reg ? *dyn_reg(c, r->reg) : rd32(r->addr);
}

static void dyn_set(CPU *c, const DynRM *r, uint32_t v)
{
    if (r->is_reg) *dyn_reg(c, r->reg) = v;
    else wr32(r->addr, v);
}

/* Byte registers use the 8-bit encoding, not the 32-bit one: 4..7 are
 * AH/CH/DH/BH, i.e. the high byte of EAX/ECX/EDX/EBX. */
static uint32_t dyn_reg8_get(CPU *c, int r)
{
    uint32_t v = *dyn_reg(c, r & 3);
    return (r >= 4) ? ((v >> 8) & 0xFFu) : (v & 0xFFu);
}

static void dyn_reg8_set(CPU *c, int r, uint32_t v)
{
    uint32_t *d = dyn_reg(c, r & 3);
    v &= 0xFFu;
    if (r >= 4) *d = (*d & ~0xFF00u) | (v << 8);
    else *d = (*d & ~0xFFu) | v;
}

static uint32_t dyn_get8(CPU *c, const DynRM *r)
{
    return r->is_reg ? dyn_reg8_get(c, r->reg) : rd8(r->addr);
}

static void dyn_set8(CPU *c, const DynRM *r, uint32_t v)
{
    if (r->is_reg) dyn_reg8_set(c, r->reg, v);
    else wr8(r->addr, (uint8_t)v);
}

static int dyn_cc(CPU *c, int cc)
{
    switch (cc & 0xF) {
    case 0x0: return c->of;
    case 0x1: return !c->of;
    case 0x2: return c->cf;
    case 0x3: return !c->cf;
    case 0x4: return c->zf;
    case 0x5: return !c->zf;
    case 0x6: return c->cf || c->zf;
    case 0x7: return !(c->cf || c->zf);
    case 0x8: return c->sf;
    case 0x9: return !c->sf;
    case 0xA: return c->pf;
    case 0xB: return !c->pf;
    case 0xC: return c->sf != c->of;
    case 0xD: return c->sf == c->of;
    case 0xE: return c->zf || (c->sf != c->of);
    case 0xF: return !c->zf && (c->sf == c->of);
    }
    return 0;
}

/* 16-bit operands are sparse in the dynamic corpus: Warden uses one
 * `66 8b`/`66 89` pair to copy a word out of a stream.  Keep the accessors
 * width-correct instead of treating the prefix as a dword move. */
static uint32_t dyn_reg16_get(CPU *c, int r)
{
    return *dyn_reg(c, r) & 0xFFFFu;
}

static void dyn_reg16_set(CPU *c, int r, uint32_t v)
{
    uint32_t *d = dyn_reg(c, r);
    *d = (*d & 0xFFFF0000u) | (v & 0xFFFFu);
}

static uint32_t dyn_get16(CPU *c, const DynRM *r)
{
    return r->is_reg ? dyn_reg16_get(c, r->reg) : rd16(r->addr);
}

static void dyn_set16(CPU *c, const DynRM *r, uint32_t v)
{
    if (r->is_reg) dyn_reg16_set(c, r->reg, v);
    else wr16(r->addr, (uint16_t)v);
}

/* Shared ALU core.  `alu` is the Intel group number: ADD=0, OR=1, ADC=2,
 * SBB=3, AND=4, SUB=5, XOR=6, CMP=7.  CMP still computes the flags but the
 * caller must not write the result. */
static uint32_t dyn_alu(CPU *c, int alu, uint32_t a, uint32_t b, int sz)
{
    switch (alu) {
    case 0: return flags_add(c, a, b, sz);
    case 1: return flags_logicz(c, a | b, sz);
    case 2: return flags_adc(c, a, b, sz);
    case 3: return flags_sbb(c, a, b, sz);
    case 4: return flags_logicz(c, a & b, sz);
    case 5: return flags_sub(c, a, b, sz);
    case 6: return flags_logicz(c, a ^ b, sz);
    case 7: (void)flags_sub(c, a, b, sz); return 0;
    default: return 0;
    }
}

/* Group 2: ROL/ROR/RCL/RCR/SHL/SHR/SAR.  `cnt` is already the architectural
 * count (1 for D0/D1, CL for D2/D3, imm8 for C0/C1); op_* masks it. */
static uint32_t dyn_shift(CPU *c, int subop, uint32_t v, uint32_t cnt, int sz)
{
    uint32_t old = v;
    switch (subop) {
    case 0: return op_rol(c, v, cnt, sz);
    case 1: return op_ror(c, v, cnt, sz);
    case 2: return op_rcl(c, v, cnt, sz);
    case 3: return op_rcr(c, v, cnt, sz);
    case 4:
    case 6: {
        uint32_t r = op_shl(c, v, cnt, sz);
        if (cnt == 1)
            c->of = (uint32_t)(((r >> (sz * 8 - 1)) & 1u) ^ (c->cf & 1u));
        return r;
    }
    case 5:
        if (cnt == 1)
            c->of = (old >> (sz * 8 - 1)) & 1u;
        return op_shr(c, v, cnt, sz);
    default: {
        uint32_t r = op_sar(c, v, cnt, sz);
        if (cnt == 1)
            c->of = 0;
        return r;
    }
    }
}

static void dyn_group2(CPU *c, uint8_t op, uint32_t *ppc)
{
    uint32_t pc = *ppc;
    int sz = (op == 0xC0 || op == 0xD0 || op == 0xD2) ? 1 : 4;
    uint8_t modrm = rd8(pc + 1);
    uint32_t p = pc + 1;
    DynRM rm = dyn_rm(c, &p);
    int subop = (modrm >> 3) & 7;
    uint32_t cnt;
    if (op == 0xC0 || op == 0xC1) cnt = rd8(p++);
    else if (op == 0xD0 || op == 0xD1) cnt = 1;
    else cnt = c->ecx;
    cnt &= 31u;
    uint32_t v = (sz == 1) ? dyn_get8(c, &rm) : dyn_get(c, &rm);
    uint32_t r = dyn_shift(c, subop, v, cnt, sz);
    if (sz == 1) dyn_set8(c, &rm, r); else dyn_set(c, &rm, r);
    *ppc = p;
}

static void dyn_divide_error(uint32_t pc)
{
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
    fprintf(stderr, "[dyn] divide error at %08X\n", pc);
#endif
    abort();
}

/* Group 3: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV.  The widening forms write the
 * implicit accumulator pair, never the ModRM operand. */
static void dyn_group3(CPU *c, uint8_t op, uint32_t *ppc)
{
    uint32_t pc = *ppc;
    int sz = (op == 0xF6) ? 1 : 4;
    uint8_t modrm = rd8(pc + 1);
    uint32_t p = pc + 1;
    DynRM rm = dyn_rm(c, &p);
    int subop = (modrm >> 3) & 7;
    if (subop <= 1) {
        uint32_t imm = (sz == 1) ? rd8(p) : rd32(p);
        p += (sz == 1) ? 1 : 4;
        uint32_t v = (sz == 1) ? dyn_get8(c, &rm) : dyn_get(c, &rm);
        flags_logicz(c, v & imm, sz);
        *ppc = p;
        return;
    }
    uint32_t v = (sz == 1) ? dyn_get8(c, &rm) : dyn_get(c, &rm);
    switch (subop) {
    case 2:
        v = ~v;
        if (sz == 1) dyn_set8(c, &rm, v); else dyn_set(c, &rm, v);
        break;
    case 3:
        v = flags_sub(c, 0, v, sz);
        if (sz == 1) dyn_set8(c, &rm, v); else dyn_set(c, &rm, v);
        break;
    case 4:
        if (sz == 1) {
            uint32_t prod = (uint32_t)dyn_reg8_get(c, 0) * v;
            dyn_reg8_set(c, 0, prod & 0xFFu);
            dyn_reg8_set(c, 4, (prod >> 8) & 0xFFu);
            c->cf = c->of = (prod >> 8) != 0;
        } else {
            uint64_t prod = (uint64_t)c->eax * v;
            c->eax = (uint32_t)prod;
            c->edx = (uint32_t)(prod >> 32);
            c->cf = c->of = (prod >> 32) != 0;
        }
        break;
    case 5:
        if (sz == 1) {
            int16_t prod = (int16_t)((int8_t)dyn_reg8_get(c, 0) *
                                     (int8_t)v);
            dyn_reg8_set(c, 0, (uint8_t)prod);
            dyn_reg8_set(c, 4, (uint32_t)((uint16_t)prod >> 8));
            c->cf = c->of = (prod != (int16_t)(int8_t)(uint8_t)prod);
        } else {
            int64_t prod = (int64_t)(int32_t)c->eax * (int32_t)v;
            c->eax = (uint32_t)prod;
            c->edx = (uint32_t)((uint64_t)prod >> 32);
            c->cf = c->of =
                (prod != (int64_t)(int32_t)(uint32_t)prod);
        }
        break;
    case 6:
        if (sz == 1) {
            uint32_t dividend = (uint32_t)dyn_reg8_get(c, 0) |
                                ((uint32_t)dyn_reg8_get(c, 4) << 8);
            if (v == 0) dyn_divide_error(pc);
            uint32_t q = dividend / v;
            uint32_t r = dividend % v;
            if (q > 0xFFu) dyn_divide_error(pc);
            dyn_reg8_set(c, 0, q);
            dyn_reg8_set(c, 4, r);
        } else {
            uint64_t dividend = ((uint64_t)c->edx << 32) | c->eax;
            if (v == 0) dyn_divide_error(pc);
            uint64_t q = dividend / v;
            uint32_t r = (uint32_t)(dividend % v);
            if (q > 0xFFFFFFFFu) dyn_divide_error(pc);
            c->eax = (uint32_t)q;
            c->edx = r;
        }
        break;
    case 7:
        if (sz == 1) {
            int32_t dividend = (int16_t)((uint16_t)dyn_reg8_get(c, 0) |
                                         ((uint16_t)dyn_reg8_get(c, 4) << 8));
            int32_t divisor = (int8_t)v;
            if (divisor == 0) dyn_divide_error(pc);
            int32_t q = dividend / divisor;
            int32_t r = dividend % divisor;
            if (q < -128 || q > 127) dyn_divide_error(pc);
            dyn_reg8_set(c, 0, (uint32_t)q);
            dyn_reg8_set(c, 4, (uint32_t)r);
        } else {
            int64_t dividend = (int64_t)(((uint64_t)c->edx << 32) | c->eax);
            int64_t divisor = (int32_t)v;
            if (divisor == 0 ||
                (dividend == INT64_MIN && divisor == -1))
                dyn_divide_error(pc);
            int64_t q = dividend / divisor;
            int64_t r = dividend % divisor;
            if (q < INT32_MIN || q > INT32_MAX) dyn_divide_error(pc);
            c->eax = (uint32_t)q;
            c->edx = (uint32_t)r;
        }
        break;
    default:
        break;
    }
    *ppc = p;
}

/* String ops.  Warden never sets DF, so the direction is always forward.
 * The REP prefix is decoded separately and passes rep=1; rep_zf selects
 * REPZ (F3) versus REPNZ (F2) for CMPS/SCAS. */
static void dyn_string_op(CPU *c, uint8_t op, int rep, int rep_zf)
{
    int sz = (op & 1) ? 4 : 1;
    for (;;) {
        if (rep && c->ecx == 0) break;
        switch (op) {
        case 0xA4:
        case 0xA5:
            if (sz == 1) wr8(c->edi, rd8(c->esi));
            else wr32(c->edi, rd32(c->esi));
            c->esi += (uint32_t)sz;
            c->edi += (uint32_t)sz;
            break;
        case 0xA6:
        case 0xA7:
            if (sz == 1) (void)flags_sub(c, rd8(c->esi), rd8(c->edi), 1);
            else (void)flags_sub(c, rd32(c->esi), rd32(c->edi), 4);
            c->esi += (uint32_t)sz;
            c->edi += (uint32_t)sz;
            break;
        case 0xAA:
        case 0xAB:
            if (sz == 1) wr8(c->edi, c->eax & 0xFFu);
            else wr32(c->edi, c->eax);
            c->edi += (uint32_t)sz;
            break;
        case 0xAC:
        case 0xAD:
            if (sz == 1) dyn_reg8_set(c, 0, rd8(c->esi));
            else c->eax = rd32(c->esi);
            c->esi += (uint32_t)sz;
            break;
        case 0xAE:
        case 0xAF:
            if (sz == 1) (void)flags_sub(c, c->eax & 0xFFu, rd8(c->edi), 1);
            else (void)flags_sub(c, c->eax, rd32(c->edi), 4);
            c->edi += (uint32_t)sz;
            break;
        default:
            abort();
        }
        if (!rep) break;
        c->ecx--;
        if ((op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) &&
            (rep_zf ? !c->zf : c->zf))
            break;
    }
}

void wc3_dyn_x86_run(CPU *c, uint32_t start)
{
    uint32_t pc = start;
    uint32_t steps = 0;
    for (;;) {
        /* game.dll is ~14.8 MB; its 1024-byte blocks need about 22M
         * interpreted instructions.  The cap is a fail-closed guard against a
         * malformed equation, not a performance budget. */
        if (++steps > 268435456u) {
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
            fprintf(stderr, "[dyn] step limit at %08X (start %08X)\n",
                    pc, start);
#endif
            abort();
        }
        uint8_t op = rd8(pc);
        switch (op) {
        case 0x90:
            pc++;
            break;
        case 0x8B: {
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            *dyn_reg(c, (modrm >> 3) & 7) = dyn_get(c, &rm);
            pc = p;
            break;
        }
        case 0x89: {
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            dyn_set(c, &rm, *dyn_reg(c, (modrm >> 3) & 7));
            pc = p;
            break;
        }
        case 0x2B: {
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            uint32_t *d = dyn_reg(c, (modrm >> 3) & 7);
            *d = flags_sub(c, *d, dyn_get(c, &rm), 4);
            pc = p;
            break;
        }
        case 0x03: {
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            uint32_t *d = dyn_reg(c, (modrm >> 3) & 7);
            *d = flags_add(c, *d, dyn_get(c, &rm), 4);
            pc = p;
            break;
        }
        case 0x33: {
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            uint32_t *d = dyn_reg(c, (modrm >> 3) & 7);
            *d = flags_logicz(c, *d ^ dyn_get(c, &rm), 4);
            pc = p;
            break;
        }
        case 0x80: {
            /* Group 1, byte form.  Warden uses `80 7d 0b 03` (cmpb $3,
             * 0xb(%ebp)). */
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            int subop = (modrm >> 3) & 7;
            uint32_t imm = rd8(p++);
            uint32_t v = dyn_get8(c, &rm);
            uint32_t r = dyn_alu(c, subop, v, imm, 1);
            if (subop != 7) dyn_set8(c, &rm, r);
            pc = p;
            break;
        }
        case 0x81:
        case 0x83: {
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            int subop = (modrm >> 3) & 7;
            uint32_t imm = (op == 0x81) ? rd32(p)
                                        : (uint32_t)(int32_t)(int8_t)rd8(p);
            p += (op == 0x81) ? 4 : 1;
            uint32_t v = dyn_get(c, &rm);
            switch (subop) {
            case 0: v = flags_add(c, v, imm, 4); break;
            case 1: v = flags_logicz(c, v | imm, 4); break;
            case 2: v = flags_adc(c, v, imm, 4); break;
            case 3: v = flags_sbb(c, v, imm, 4); break;
            case 4: v = flags_logicz(c, v & imm, 4); break;
            case 5: v = flags_sub(c, v, imm, 4); break;
            case 6: v = flags_logicz(c, v ^ imm, 4); break;
            case 7: flags_sub(c, v, imm, 4); pc = p; continue;
            default:
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
                fprintf(stderr, "[dyn] unsupported %02X /%d at %08X\n",
                        op, subop, pc);
#endif
                abort();
            }
            dyn_set(c, &rm, v);
            pc = p;
            break;
        }
        case 0xFE: {
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            int subop = (modrm >> 3) & 7;
            uint32_t v = dyn_get8(c, &rm);
            if (subop == 0) {
                dyn_set8(c, &rm, flags_incs(c, v, 1));
                pc = p;
            } else if (subop == 1) {
                dyn_set8(c, &rm, flags_decs(c, v, 1));
                pc = p;
            } else {
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
                fprintf(stderr, "[dyn] unsupported FE /%d at %08X\n",
                        subop, pc);
#endif
                abort();
            }
            break;
        }
        case 0xF6:
        case 0xF7:
            dyn_group3(c, op, &pc);
            break;
        case 0xFF: {
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            int subop = (modrm >> 3) & 7;
            uint32_t v = dyn_get(c, &rm);
            if (subop == 0) { dyn_set(c, &rm, flags_incs(c, v, 4)); pc = p; }
            else if (subop == 1) { dyn_set(c, &rm, flags_decs(c, v, 4)); pc = p; }
            else if (subop == 2) {
                /* Near call through a register or memory operand.  The return
                 * address is the instruction after the modrm/operand bytes; the
                 * callee's ret/ret imm16 pops it from the guest stack.  The
                 * dispatcher is synchronous, so after it returns the dynamic
                 * code resumes at p. */
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
                dyn_trace_call(pc, v, p);
#endif
                push32(c, p);
                dispatch(c, v);
                pc = p;
            }
            else if (subop == 4) { pc = v; }
            else if (subop == 6) { push32(c, v); pc = p; }
            else {
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
                fprintf(stderr, "[dyn] unsupported FF /%d at %08X\n",
                        subop, pc);
#endif
                abort();
            }
            break;
        }
        case 0xC6: {
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            if (((modrm >> 3) & 7) != 0) {
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
                fprintf(stderr, "[dyn] unsupported C6 /%d at %08X\n",
                        (modrm >> 3) & 7, pc);
#endif
                abort();
            }
            dyn_set8(c, &rm, rd8(p++));
            pc = p;
            break;
        }
        case 0xC7: {
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            uint32_t imm = rd32(p);
            p += 4;
            if (((modrm >> 3) & 7) != 0) {
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
                fprintf(stderr, "[dyn] unsupported C7 /%d at %08X\n",
                        (modrm >> 3) & 7, pc);
#endif
                abort();
            }
            dyn_set(c, &rm, imm);
            pc = p;
            break;
        }
        case 0x8F: {
            uint8_t modrm = rd8(pc + 1);
            if (((modrm >> 3) & 7) != 0) {
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
                fprintf(stderr, "[dyn] unsupported 8F /%d at %08X\n",
                        (modrm >> 3) & 7, pc);
#endif
                abort();
            }
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            uint32_t v = pop32(c);
            if (!rm.is_reg) {
                /* POP r/m32 computes a memory destination's effective address
                 * after it increments ESP.  Re-decode with the new ESP rather
                 * than storing through the pre-increment address. */
                uint32_t q = pc + 1;
                rm = dyn_rm(c, &q);
                p = q;
            }
            dyn_set(c, &rm, v);
            pc = p;
            break;
        }
        case 0x8D: {
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            if (rm.is_reg) {
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
                fprintf(stderr, "[dyn] lea with register at %08X\n", pc);
#endif
                abort();
            }
            *dyn_reg(c, (modrm >> 3) & 7) = rm.addr;
            pc = p;
            break;
        }
        case 0xE8: {
            uint32_t ret = pc + 5;
            uint32_t target = ret + rd32(pc + 1);
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
            dyn_trace_call(pc, target, ret);
#endif
            push32(c, ret);
            dispatch(c, target);
            pc = ret;
            break;
        }
        case 0xE9:
            pc = pc + 5 + rd32(pc + 1);
            break;
        case 0xEB:
            pc = pc + 2 + (uint32_t)(int32_t)(int8_t)rd8(pc + 1);
            break;
        case 0x74: case 0x75: case 0x7C: case 0x7D:
        case 0x72: case 0x73: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7E: case 0x7F: case 0x70: case 0x71: {
            int cc = op & 0x0F;
            if (dyn_cc(c, cc))
                pc = pc + 2 + (uint32_t)(int32_t)(int8_t)rd8(pc + 1);
            else
                pc += 2;
            break;
        }
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
            *dyn_reg(c, op & 7) = flags_incs(c, *dyn_reg(c, op & 7), 4);
            pc++;
            break;
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            *dyn_reg(c, op & 7) = flags_decs(c, *dyn_reg(c, op & 7), 4);
            pc++;
            break;
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            push32(c, *dyn_reg(c, op & 7));
            pc++;
            break;
        case 0x68:
            push32(c, rd32(pc + 1));
            pc += 5;
            break;
        case 0x6A:
            push32(c, (uint32_t)(int32_t)(int8_t)rd8(pc + 1));
            pc += 2;
            break;
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            *dyn_reg(c, op & 7) = pop32(c);
            pc++;
            break;
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            dyn_reg8_set(c, op & 7, rd8(pc + 1));
            pc += 2;
            break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            *dyn_reg(c, op & 7) = rd32(pc + 1);
            pc += 5;
            break;
        case 0xC9:
            c->esp = c->ebp;
            c->ebp = pop32(c);
            pc++;
            break;
        case 0xC3: {
            uint32_t ret = rd32(c->esp);
            (void)pop32(c);
            (void)ret;
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
            fprintf(stderr, "[dyn] ret -> %08X (start %08X)\n", ret, start);
#endif
            return;
        }
        case 0xC2: {
            uint16_t n = rd16(pc + 1);
            uint32_t ret = rd32(c->esp);
            (void)pop32(c);
            (void)ret;
            c->esp += n;
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
            fprintf(stderr, "[dyn] ret %u -> %08X (start %08X)\n",
                    n, ret, start);
#endif
            return;
        }
        case 0xC0:
        case 0xC1:
        case 0xD0:
        case 0xD1:
        case 0xD2:
        case 0xD3:
            /* Group 2.  Warden uses C1 (imm8), D1 (count 1) and D3 (CL)
             * in SHA-1 and the LZ/stream helpers; the byte forms share the
             * same flag rules. */
            dyn_group2(c, op, &pc);
            break;
        case 0x88: {
            /* MOV r/m8, r8. */
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            dyn_set8(c, &rm, dyn_reg8_get(c, (modrm >> 3) & 7));
            pc = p;
            break;
        }
        case 0x8A: {
            /* MOV r8, r/m8. */
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            dyn_reg8_set(c, (modrm >> 3) & 7, dyn_get8(c, &rm));
            pc = p;
            break;
        }
        case 0xA0: {
            uint32_t a = rd32(pc + 1);
            c->eax = (c->eax & ~0xFFu) | rd8(a);
            pc += 5;
            break;
        }
        case 0xA1:
            c->eax = rd32(rd32(pc + 1));
            pc += 5;
            break;
        case 0xA2: {
            uint32_t a = rd32(pc + 1);
            wr8(a, (uint8_t)c->eax);
            pc += 5;
            break;
        }
        case 0xA3:
            wr32(rd32(pc + 1), c->eax);
            pc += 5;
            break;
        case 0xA8:
            flags_logicz(c, (c->eax & 0xFFu) & rd8(pc + 1), 1);
            pc += 2;
            break;
        case 0xA9:
            flags_logicz(c, c->eax & rd32(pc + 1), 4);
            pc += 5;
            break;
        case 0xA4:
        case 0xA5:
        case 0xA6:
        case 0xA7:
        case 0xAA:
        case 0xAB:
        case 0xAC:
        case 0xAD:
        case 0xAE:
        case 0xAF:
            /* Non-prefixed string op.  Warden's SHA-1 context clear uses
             * STOSD; the copy helpers use MOVSD.  DF is zero throughout. */
            dyn_string_op(c, op, 0, 0);
            pc++;
            break;
        case 0x84: {
            /* TEST r/m8, r8.  Warden executes `84 C0` (TEST AL, AL) after a
             * virtual call that returns a boolean in AL.  The memory form must
             * read one byte, not a dword: the operand can sit at the end of a
             * page. */
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            uint32_t a = dyn_get8(c, &rm);
            uint32_t b = dyn_reg8_get(c, (modrm >> 3) & 7);
            flags_logicz(c, a & b, 1);
            pc = p;
            break;
        }
        case 0x85: {
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            flags_logicz(c, dyn_get(c, &rm) & *dyn_reg(c, (modrm >> 3) & 7), 4);
            pc = p;
            break;
        }
        case 0x39: {
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            flags_sub(c, dyn_get(c, &rm), *dyn_reg(c, (modrm >> 3) & 7), 4);
            pc = p;
            break;
        }
        case 0x3B: {
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            flags_sub(c, *dyn_reg(c, (modrm >> 3) & 7), dyn_get(c, &rm), 4);
            pc = p;
            break;
        }
        case 0x01: {
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            dyn_set(c, &rm,
                    flags_add(c, dyn_get(c, &rm),
                              *dyn_reg(c, (modrm >> 3) & 7), 4));
            pc = p;
            break;
        }
        case 0x29: {
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            dyn_set(c, &rm,
                    flags_sub(c, dyn_get(c, &rm),
                              *dyn_reg(c, (modrm >> 3) & 7), 4));
            pc = p;
            break;
        }
        case 0x31: {
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            dyn_set(c, &rm,
                    flags_logicz(c, dyn_get(c, &rm) ^
                                      *dyn_reg(c, (modrm >> 3) & 7), 4));
            pc = p;
            break;
        }
        case 0x66: {
            /* Operand-size prefix.  Warden only uses the word move pair
             * `66 8b` / `66 89`; treating it as a dword move would corrupt
             * the upper half and the following `movl` writes. */
            uint8_t op2 = rd8(pc + 1);
            if (op2 == 0x8B || op2 == 0x89) {
                uint8_t modrm = rd8(pc + 2);
                uint32_t p = pc + 2;
                DynRM rm = dyn_rm(c, &p);
                if (op2 == 0x8B)
                    dyn_reg16_set(c, (modrm >> 3) & 7,
                                  dyn_get16(c, &rm));
                else
                    dyn_set16(c, &rm, dyn_reg16_get(c, (modrm >> 3) & 7));
                pc = p;
                break;
            }
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
            fprintf(stderr, "[dyn] unsupported 66 %02X at %08X\n", op2, pc);
#endif
            abort();
        }
        case 0xF3:
        case 0xF2: {
            uint8_t op2 = rd8(pc + 1);
            if ((op2 >= 0xA4 && op2 <= 0xA7) ||
                (op2 >= 0xAA && op2 <= 0xAF)) {
                dyn_string_op(c, op2, 1, op == 0xF3);
                pc += 2;
                break;
            }
            /* A repeat prefix in front of a non-string instruction is a
             * no-op for this corpus (F3 90 is PAUSE, F3 C3 is REP RET). */
            pc++;
            break;
        }
        case 0x0F: {
            uint8_t op2 = rd8(pc + 1);
            if (op2 == 0x1F) {
                uint32_t p = pc + 2;
                (void)dyn_rm(c, &p);
                pc = p;
                break;
            }
            if (op2 >= 0x80 && op2 <= 0x8F) {
                /* Near Jcc.  Warden's SHA-1 transform loops on `0F 85`
                 * (jne) and the LZ helpers use `0F 82/86/87/8C`. */
                uint32_t next = pc + 6;
                uint32_t disp = rd32(pc + 2);
                pc = dyn_cc(c, op2 & 0x0F) ? next + disp : next;
                break;
            }
            if (op2 == 0xA4 || op2 == 0xA5 ||
                op2 == 0xAC || op2 == 0xAD) {
                /* SHLD/SHRD r/m32, r32, imm8 (A4/AC) or CL (A5/AD).
                 * Warden's SHA-1 finalization uses `0F AC D0 08` =
                 * shrd eax, edx, 8. */
                uint8_t modrm = rd8(pc + 2);
                uint32_t p = pc + 2;
                DynRM rm = dyn_rm(c, &p);
                uint32_t d = dyn_get(c, &rm);
                uint32_t s = *dyn_reg(c, (modrm >> 3) & 7);
                uint32_t cnt = (op2 == 0xA4 || op2 == 0xAC)
                                   ? rd8(p++) : (c->ecx & 31u);
                uint32_t r = (op2 == 0xA4 || op2 == 0xA5)
                                   ? op_shld(c, d, s, cnt, 4)
                                   : op_shrd(c, d, s, cnt, 4);
                if (cnt == 1)
                    c->of = ((d >> 31) & 1) ^ ((r >> 31) & 1);
                dyn_set(c, &rm, r);
                pc = p;
                break;
            }
            if (op2 == 0xB6 || op2 == 0xB7 ||
                op2 == 0xBE || op2 == 0xBF) {
                /* MOVZX/MOVSX r32, r/m8/r/m16. */
                uint8_t modrm = rd8(pc + 2);
                uint32_t p = pc + 2;
                DynRM rm = dyn_rm(c, &p);
                uint32_t v = (op2 == 0xB6 || op2 == 0xBE)
                                 ? dyn_get8(c, &rm) : dyn_get16(c, &rm);
                if (op2 == 0xBE)
                    v = (uint32_t)(int32_t)(int8_t)v;
                else if (op2 == 0xBF)
                    v = (uint32_t)(int32_t)(int16_t)v;
                *dyn_reg(c, (modrm >> 3) & 7) = v;
                pc = p;
                break;
            }
            if (op2 == 0xAF) {
                /* IMUL r32, r/m32.  Not present in Warden today, but the
                 * decoder is complete for the same integer family. */
                uint8_t modrm = rd8(pc + 2);
                uint32_t p = pc + 2;
                DynRM rm = dyn_rm(c, &p);
                uint32_t *d = dyn_reg(c, (modrm >> 3) & 7);
                int64_t prod = (int64_t)(int32_t)*d *
                               (int64_t)(int32_t)dyn_get(c, &rm);
                *d = (uint32_t)prod;
                c->cf = c->of =
                    (prod != (int64_t)(int32_t)(uint32_t)prod);
                pc = p;
                break;
            }
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
            fprintf(stderr, "[dyn] unsupported 0F %02X at %08X\n", op2, pc);
#endif
            abort();
        }
        default:
            /* The eight classic ALU groups share opcodes 00..3D.  The
             * forms already special-cased above (01/03/29/2B/31/33/39/3B)
             * keep their code; this covers the remaining direction, byte,
             * and accumulator-immediate forms.  Low bits 6/7 are the
             * segment push/pop and DAA/AAS opcodes, so they stay out. */
            if (op < 0x40 && (op & 7) <= 5) {
                int alu = (op >> 3) & 7;
                int form = op & 7;
                if (form <= 3) {
                    uint8_t modrm = rd8(pc + 1);
                    uint32_t p = pc + 1;
                    DynRM rm = dyn_rm(c, &p);
                    int reg = (modrm >> 3) & 7;
                    int sz = (form == 0 || form == 2) ? 1 : 4;
                    uint32_t a, b, r;
                    if (form == 0 || form == 1) {
                        a = (sz == 1) ? dyn_get8(c, &rm) : dyn_get(c, &rm);
                        b = (sz == 1) ? dyn_reg8_get(c, reg)
                                      : *dyn_reg(c, reg);
                        r = dyn_alu(c, alu, a, b, sz);
                        if (alu != 7) {
                            if (sz == 1) dyn_set8(c, &rm, r);
                            else dyn_set(c, &rm, r);
                        }
                    } else {
                        a = (sz == 1) ? dyn_reg8_get(c, reg)
                                      : *dyn_reg(c, reg);
                        b = (sz == 1) ? dyn_get8(c, &rm) : dyn_get(c, &rm);
                        r = dyn_alu(c, alu, a, b, sz);
                        if (alu != 7) {
                            if (sz == 1) dyn_reg8_set(c, reg, r);
                            else *dyn_reg(c, reg) = r;
                        }
                    }
                    pc = p;
                } else if (form == 4) {
                    uint32_t r = dyn_alu(c, alu, dyn_reg8_get(c, 0),
                                         rd8(pc + 1), 1);
                    if (alu != 7) dyn_reg8_set(c, 0, r);
                    pc += 2;
                } else {
                    uint32_t r = dyn_alu(c, alu, c->eax, rd32(pc + 1), 4);
                    if (alu != 7) c->eax = r;
                    pc += 5;
                }
                break;
            }
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
            fprintf(stderr,
                    "[dyn] unsupported opcode %02X at %08X (start %08X)\n",
                    op, pc, start);
#endif
            abort();
        }
    }
}
