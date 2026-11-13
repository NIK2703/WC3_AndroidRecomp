/*
 * dyn_x86.c - executor for guest code generated at runtime.
 *
 * This is deliberately a small instruction interpreter, not a JIT and not a
 * general x86 emulator.  The only producer in the 1.27b corpus is the
 * CheckRevision equation compiler in ver-IX86-3.dll: it emits
 *
 *     mov eax, 0
 *     jmp generated_body
 *
 * followed by one unrolled block of integer register/memory operations and a
 * jump back into the DLL's hash continuation.  The continuation updates the
 * block counter and either jumps to the next generated block or returns.
 *
 * The interpreter executes that subset exactly, including the guest stack
 * effects of ret/ret imm16, so the lifted C caller chain unwinds naturally.
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
        case 0xC1: {
            /* Group 2, count imm8.  Warden's SHA-1 update uses
             * `C1 EF 03` (shr edi, 3) and `C1 E1 03` (shl ecx, 3). */
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            uint32_t cnt = rd8(p++) & 31u;
            int subop = (modrm >> 3) & 7;
            uint32_t v = dyn_get(c, &rm);
            switch (subop) {
            case 4:
            case 6:
                v = op_shl(c, v, cnt, 4);
                if (cnt == 1) c->of = ((v >> 31) & 1) ^ (c->cf & 1);
                break;
            case 5:
                if (cnt == 1) c->of = (v >> 31) & 1;
                v = op_shr(c, v, cnt, 4);
                break;
            case 7:
                v = op_sar(c, v, cnt, 4);
                if (cnt == 1) c->of = 0;
                break;
            default:
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
                fprintf(stderr, "[dyn] unsupported C1 /%d at %08X\n",
                        subop, pc);
#endif
                abort();
            }
            dyn_set(c, &rm, v);
            pc = p;
            break;
        }
        case 0xD1: {
            /* Group 2, count 1.  Warden's first reachable use is
             * `D1 6D FC` = shr dword ptr [ebp-4], 1. */
            uint8_t modrm = rd8(pc + 1);
            uint32_t p = pc + 1;
            DynRM rm = dyn_rm(c, &p);
            int subop = (modrm >> 3) & 7;
            uint32_t v = dyn_get(c, &rm);
            switch (subop) {
            case 4:
            case 6:
                v = op_shl(c, v, 1, 4);
                c->of = ((v >> 31) & 1) ^ (c->cf & 1);
                break;
            case 5:
                c->of = (v >> 31) & 1;
                v = op_shr(c, v, 1, 4);
                break;
            case 7:
                v = op_sar(c, v, 1, 4);
                c->of = 0;
                break;
            default:
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
                fprintf(stderr, "[dyn] unsupported D1 /%d at %08X\n",
                        subop, pc);
#endif
                abort();
            }
            dyn_set(c, &rm, v);
            pc = p;
            break;
        }
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
        case 0xAB: {
            /* STOSD.  Warden's SHA-1 context clears five dwords with DF=0. */
            wr32(c->edi, c->eax);
            c->edi += 4;
            pc++;
            break;
        }
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
        case 0x0F: {
            uint8_t op2 = rd8(pc + 1);
            if (op2 == 0x1F) {
                uint32_t p = pc + 2;
                (void)dyn_rm(c, &p);
                pc = p;
                break;
            }
            if (op2 == 0xAC || op2 == 0xAD) {
                /* SHRD r/m32, r32, imm8 (AC) / CL (AD).  Warden's SHA-1
                 * finalization uses `0F AC D0 08` = shrd eax, edx, 8. */
                uint8_t modrm = rd8(pc + 2);
                uint32_t p = pc + 2;
                DynRM rm = dyn_rm(c, &p);
                uint32_t d = dyn_get(c, &rm);
                uint32_t s = *dyn_reg(c, (modrm >> 3) & 7);
                uint32_t cnt = (op2 == 0xAC) ? rd8(p++) : (c->ecx & 31u);
                uint32_t r = op_shrd(c, d, s, cnt, 4);
                if (cnt == 1)
                    c->of = ((d >> 31) & 1) ^ ((r >> 31) & 1);
                dyn_set(c, &rm, r);
                pc = p;
                break;
            }
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
            fprintf(stderr, "[dyn] unsupported 0F %02X at %08X\n", op2, pc);
#endif
            abort();
        }
        default:
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
            fprintf(stderr,
                    "[dyn] unsupported opcode %02X at %08X (start %08X)\n",
                    op, pc, start);
#endif
            abort();
        }
    }
}
