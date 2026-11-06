#include "hle_abi.h"

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
    if (!std_entry || !cdecl_entry) return fail("register returned NULL");
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
    if (strcmp(hle_describe(&g_registry, thunk), "TestCdecl") != 0)
        return fail("describe");
#endif

    CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.esp = 0x31080000u;
    cpu.ebx = 1u;
    cpu.esi = 2u;
    cpu.edi = 3u;
    cpu.ebp = 4u;

    hle_call(&g_registry, GUEST_HLE_BASE, &cpu);
    if (g_hits != 1u) return fail("stdcall handler count");
    if (g_seen_esp != 0x31080000u) return fail("stdcall entry esp");
    if (cpu.esp != 0x31080000u + 16u) return fail("stdcall exit esp");
    if (cpu.eax != 0x5A5A5A5Au) return fail("stdcall eax");
    if (cpu.ebx != 1u || cpu.esi != 2u || cpu.edi != 3u || cpu.ebp != 4u)
        return fail("callee-saved registers");

    hle_call(&g_registry, thunk, &cpu);
    if (g_hits != 2u) return fail("cdecl handler count");
    if (cpu.esp != 0x31080000u + 16u + 4u) return fail("cdecl exit esp");
    if (cpu.eax != 0xC0DEC0DEu) return fail("cdecl eax");

#if WC3_DEBUG
    if (g_registry.debug[1].calls != 1u) return fail("debug call counter");
#endif

    puts("hle ABI OK");
    return 0;
}
