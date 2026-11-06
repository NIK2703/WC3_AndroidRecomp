#include "hle_abi.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

void wc3_dispatch_init(HleRegistry *registry);
void wc3_dispatch_shutdown(void);
void dispatch(CPU *c, uint32_t target);
void dispatch_jmp(CPU *c, uint32_t target);
void L_00401000(CPU *c);
void L_00401010(CPU *c);

static HleRegistry g_registry;

void L_00401000(CPU *c)
{
    c->eax = 0x11111111u;
    dispatch_jmp(c, 0x00401010u);
}

void L_00401010(CPU *c)
{
    c->eax = 0x22222222u;
}

static void h_import(CPU *c)
{
    c->eax = 0x33333333u;
}

static int fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    return 1;
}

int main(void)
{
    hle_registry_init(&g_registry);
    hle_register(&g_registry, "KERNEL32.dll", "TestImport", 0, h_import, 0, 0);
    wc3_dispatch_init(&g_registry);

    CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    dispatch(&cpu, 0x00401000u);
    if (cpu.eax != 0x22222222u) return fail("tail jump chain");

    cpu.eax = 0;
    dispatch_jmp(&cpu, 0x00401010u);
    if (cpu.eax != 0x22222222u) return fail("top-level jump");

    cpu.eax = 0;
    dispatch(&cpu, GUEST_HLE_BASE);
    if (cpu.eax != 0x33333333u) return fail("HLE dispatch");
    if (cpu.esp != 4u) return fail("HLE stack cleanup");

    wc3_dispatch_shutdown();
    puts("dispatch OK");
    return 0;
}
