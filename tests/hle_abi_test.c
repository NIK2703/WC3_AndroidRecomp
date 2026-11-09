#include "hle_abi.h"
#include "hle_fast.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static HleRegistry g_registry;
static unsigned g_hits;
static uint32_t g_seen_esp;

static void h_stdcall(CPU *c)
{
    g_hits++;
    g_seen_esp = c->esp;
    c->eax = 0x5A5A5A5Au;
    /* A handler is allowed to use the guest stack while it runs.  The bridge
     * must restore the caller frame before applying the import cleanup. */
    c->esp -= 16u;
}

static void h_cdecl(CPU *c)
{
    g_hits++;
    g_seen_esp = c->esp;
    c->eax = 0xC0DEC0DEu;
}

/* Test-only ABI violator: it proves the bridge does not mask a handler that
 * clobbers callee-saved registers.  Real handlers must preserve them. */
static void h_writes_callee_saved(CPU *c)
{
    g_hits++;
    c->ebx = 0xB0000001u;
    c->esi = 0xB0000002u;
    c->edi = 0xB0000003u;
    c->ebp = 0xB0000004u;
}

static void h_fast(CPU *c)
{
    c->eax = 0xF00Du;
}

static int fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    return 1;
}

int main(void)
{
    hle_registry_init(&g_registry);

    HleEntry *std_entry = hle_register(&g_registry, "KERNEL32.dll",
                                       "TestStdcall", 0, h_stdcall, 3, 0);
    HleEntry *cdecl_entry = hle_register_ex(&g_registry, "MSVCR80.dll",
                                            "TestCdecl", 0, h_cdecl, 2,
                                            HLE_CALL_CDECL | HLE_OPTIONAL,
                                            &g_hits);
    HleEntry *bad_saved_entry =
        hle_register(&g_registry, "KERNEL32.dll", "TestBadSaved", 0,
                     h_writes_callee_saved, 0, 0);
    if (!std_entry || !cdecl_entry || !bad_saved_entry)
        return fail("register returned NULL");
    if (cdecl_entry->user != &g_hits) return fail("per-thunk user payload");
    if (std_entry->stack_cleanup != 16u) return fail("stdcall cleanup");
    if (cdecl_entry->stack_cleanup != 4u) return fail("cdecl cleanup");
    if (cdecl_entry->argc != 2u) return fail("cdecl argc");
    if ((cdecl_entry->flags & HLE_OPTIONAL) == 0u) return fail("optional flag");

    uint32_t thunk = GUEST_HLE_BASE + 1u * HLE_STRIDE;
    if (hle_entry_for(&g_registry, thunk) != cdecl_entry)
        return fail("thunk lookup");
    if (hle_entry_for(&g_registry, thunk + 1u) != NULL)
        return fail("unaligned thunk accepted");
    if (hle_entry_for(&g_registry, GUEST_HLE_BASE + 99u * HLE_STRIDE) != NULL)
        return fail("out-of-range thunk accepted");

#if WC3_DEBUG
    if (hle_find(&g_registry, "kernel32.DLL", "testSTDCALL", 0) != std_entry)
        return fail("case-insensitive lookup");
    if (hle_find(&g_registry, "KERNEL32.dll", "missing", 0) != NULL)
        return fail("missing lookup");
    if (strcmp(hle_abi_describe(&g_registry, thunk), "TestCdecl") != 0)
        return fail("describe");
#endif

    CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.esp = 0x31080000u;
    cpu.ebx = 1u;
    cpu.esi = 2u;
    cpu.edi = 3u;
    cpu.ebp = 4u;

    hle_abi_call(&g_registry, GUEST_HLE_BASE, &cpu);
    if (g_hits != 1u) return fail("stdcall handler count");
    if (g_seen_esp != 0x31080000u) return fail("stdcall entry esp");
    if (cpu.esp != 0x31080000u + 16u) return fail("stdcall exit esp");
    if (cpu.eax != 0x5A5A5A5Au) return fail("stdcall eax");
    if (cpu.ebx != 1u || cpu.esi != 2u || cpu.edi != 3u || cpu.ebp != 4u)
        return fail("callee-saved registers");

    hle_abi_call(&g_registry, thunk, &cpu);
    if (g_hits != 2u) return fail("cdecl handler count");
    if (cpu.esp != 0x31080000u + 16u + 4u) return fail("cdecl exit esp");
    if (cpu.eax != 0xC0DEC0DEu) return fail("cdecl eax");

    /* O-09: the bridge is not a callee-saved safety net.  A handler that
     * writes these fields makes the violation visible instead of hiding it. */
    hle_abi_call(&g_registry, GUEST_HLE_BASE + 2u * HLE_STRIDE, &cpu);
    if (g_hits != 3u) return fail("callee-saved probe count");
    if (cpu.ebx != 0xB0000001u || cpu.esi != 0xB0000002u ||
        cpu.edi != 0xB0000003u || cpu.ebp != 0xB0000004u)
        return fail("bridge restored callee-saved registers");

    /* O-47: the cutover registry is bounded separately from the frozen ABI
     * window.  Bind the last ABI slot, prove that an unbound hole is still
     * rejected, and prove that the one-conversion entry point preserves the
     * guest stack cleanup contract. */
    uint32_t fast_thunk = GUEST_HLE_BASE + (HLE_MAX - 1u) * HLE_STRIDE;
    CPU fast_cpu;
    memset(&fast_cpu, 0, sizeof fast_cpu);
    fast_cpu.esp = 0x31080000u;
    wc3_hle_fast_bind(HLE_MAX - 1u, h_fast, 2u, 0u);
    if (wc3_hle_fast_call_slot(HLE_MAX, fast_thunk, &fast_cpu))
        return fail("fast slot above ABI window accepted");
    if (wc3_hle_fast_call_slot(HLE_MAX - 2u, fast_thunk, &fast_cpu))
        return fail("unbound fast slot accepted");
    if (!wc3_hle_fast_call_slot(HLE_MAX - 1u, fast_thunk, &fast_cpu))
        return fail("bound fast slot not called");
    if (fast_cpu.eax != 0xF00Du) return fail("fast slot result");
    if (fast_cpu.esp != 0x31080000u + 12u) return fail("fast slot cleanup");

    /* The one-conversion entry point must reject an unbound hole and non-ABI
     * thunks before it reaches the dense slot map. */
    CPU unbound_cpu;
    memset(&unbound_cpu, 0, sizeof unbound_cpu);
    unbound_cpu.esp = 0x31080000u;
    if (wc3_hle_fast_call(GUEST_HLE_BASE + 1u * HLE_STRIDE, &unbound_cpu))
        return fail("fast thunk bound through empty slot");
    if (wc3_hle_fast_call(GUEST_HLE_BASE + 1u * HLE_STRIDE + 1u,
                          &unbound_cpu))
        return fail("unaligned fast thunk accepted");
    if (wc3_hle_fast_call(GUEST_HLE_BASE + HLE_MAX * HLE_STRIDE,
                          &unbound_cpu))
        return fail("fast thunk outside ABI window accepted");

    /* Rebinding an existing slot updates the entry in place and must not
     * consume a second registry index. */
    CPU rebind_cpu;
    memset(&rebind_cpu, 0, sizeof rebind_cpu);
    rebind_cpu.esp = 0x31080000u;
    wc3_hle_fast_bind(HLE_MAX - 1u, h_stdcall, 1u, 0u);
    if (!wc3_hle_fast_call_slot(HLE_MAX - 1u, fast_thunk, &rebind_cpu))
        return fail("rebound fast slot not called");
    if (g_hits != 4u) return fail("rebind handler count");
    if (rebind_cpu.eax != 0x5A5A5A5Au) return fail("rebind result");
    if (rebind_cpu.esp != 0x31080000u + 8u)
        return fail("rebind cleanup");

    /* Fill the remaining cutover entries (slot 4095 is already bound) and
     * prove that the 4097th bind is rejected without displacing slot 0. */
    for (uint32_t slot = 0; slot < 4095u; slot++)
        wc3_hle_fast_bind(slot, h_fast, 0u, 0u);
    wc3_hle_fast_bind(4096u, h_fast, 0u, 0u);
    CPU overflow_cpu;
    memset(&overflow_cpu, 0, sizeof overflow_cpu);
    overflow_cpu.esp = 0x31080000u;
    if (wc3_hle_fast_call_slot(4096u, GUEST_HLE_BASE + 4096u * HLE_STRIDE,
                               &overflow_cpu))
        return fail("fast registry overflow accepted");
    if (!wc3_hle_fast_call_slot(0u, GUEST_HLE_BASE, &overflow_cpu))
        return fail("fast registry overflow displaced slot 0");
    if (overflow_cpu.eax != 0xF00Du) return fail("overflow survivor result");
    if (overflow_cpu.esp != 0x31080000u + 4u)
        return fail("overflow survivor cleanup");

#if WC3_DEBUG
    if (g_registry.debug[1].calls != 1u) return fail("debug call counter");
#endif

    puts("hle ABI OK");
    return 0;
}
