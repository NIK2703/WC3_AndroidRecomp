/*
 * winmm_guard_timer_test.c - T0 contract for the two WINMM seams that survive
 * the DirectSound cutover.
 *
 * The product no longer has a waveOut transport: every frozen WINMM import is
 * bound to winmm_guard_hle.c and must return "no driver"/"invalid handle".
 * Mss32 still owns a USER32 timer, so mss_timer_hle.c must install and run that
 * timer without opening an audio stream or producing PCM.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "direct_call.h"
#include "hle_bridge.h"
#include "platform.h"
#include "mss_timer_hle.h"
#include "winmm_guard_hle.h"

#define MAX_ROWS 64u

typedef struct {
    char dll[32];
    char name[64];
    void (*impl)(CPU *);
} Row;

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;
static uint32_t g_args[8];
static uint32_t g_caps;
static int g_failures;

static int check(int condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "winmm guard/timer: %s\n", what);
        g_failures++;
    }
    return condition;
}

static void no_op(CPU *c)
{
    (void)c;
}

static uint64_t fake_monotonic_ns(void *ctx)
{
    (void)ctx;
    return 0u;
}

static Wc3Platform g_platform;

const Wc3Platform *wc3_platform(void)
{
    return &g_platform;
}

uint32_t wc3_legacy_arg32(const CPU *c, unsigned index)
{
    (void)c;
    return index < 8u ? g_args[index] : 0u;
}

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    (void)addr;
    (void)size;
    return 1;
}

uint8_t wc3_legacy_rd8g(uint32_t addr)
{
    (void)addr;
    return 0u;
}

uint32_t wc3_legacy_rd32g(uint32_t addr)
{
    (void)addr;
    return 0u;
}

void wc3_legacy_wr32g(uint32_t addr, uint32_t value)
{
    if (addr == 0x1000u) g_caps = value;
}

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    for (unsigned i = 0; i < g_row_count; i++) {
        if (!strcmp(g_rows[i].dll, dll) && !strcmp(g_rows[i].name, name)) {
            g_rows[i].impl = impl;
            return 0;
        }
    }
    return -1;
}

int wc3_legacy_hle_chain(const char *dll, const char *name,
                         void (*impl)(CPU *c), void (**previous)(CPU *c))
{
    for (unsigned i = 0; i < g_row_count; i++) {
        if (!strcmp(g_rows[i].dll, dll) && !strcmp(g_rows[i].name, name)) {
            if (previous) *previous = g_rows[i].impl;
            g_rows[i].impl = impl;
            return 0;
        }
    }
    return -1;
}

void wc3_legacy_set_last_error(uint32_t code)
{
    (void)code;
}

uint32_t wc3_legacy_get_last_error(void)
{
    return 0u;
}

wc3_lifted_fn wc3_dispatch_resolve(uint32_t target)
{
    (void)target;
    return NULL;
}

void wc3_dispatch_resolved(CPU *c, wc3_lifted_fn fn)
{
    (void)c;
    (void)fn;
}

void dispatch(CPU *c, uint32_t target)
{
    (void)c;
    (void)target;
}

void dispatch_jmp(CPU *c, uint32_t target)
{
    (void)c;
    (void)target;
}

static int add_row(const char *dll, const char *name)
{
    if (g_row_count >= MAX_ROWS) return -1;
    Row *r = &g_rows[g_row_count++];
    snprintf(r->dll, sizeof r->dll, "%s", dll);
    snprintf(r->name, sizeof r->name, "%s", name);
    r->impl = no_op;
    return 0;
}

static Row *find_row(const char *dll, const char *name)
{
    for (unsigned i = 0; i < g_row_count; i++)
        if (!strcmp(g_rows[i].dll, dll) && !strcmp(g_rows[i].name, name))
            return &g_rows[i];
    return NULL;
}

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    (void)argc;
    (void)cdecl_abi;
    Row *r = find_row(dll, name);
    if (r) {
        r->impl = impl;
        return 0;
    }
    if (add_row(dll, name) != 0)
        return -1;
    g_rows[g_row_count - 1].impl = impl;
    return 0;
}

static uint32_t call_row(Row *r)
{
    if (!r || !r->impl) return 0xFFFFFFFFu;
    CPU c;
    memset(&c, 0, sizeof c);
    c.esp = 0x1000u;
    r->impl(&c);
    return c.eax;
}

static void install_rows(void)
{
    static const char *const wave_rows[] = {
        "waveOutGetNumDevs", "waveOutGetDevCapsA", "waveOutOpen",
        "waveOutClose", "waveOutPrepareHeader", "waveOutUnprepareHeader",
        "waveOutWrite", "waveOutReset", "waveOutGetID", "waveOutGetVolume",
        "waveOutSetVolume",
    };
    for (unsigned i = 0; i < sizeof wave_rows / sizeof wave_rows[0]; i++)
        (void)add_row("WINMM.dll", wave_rows[i]);
    (void)add_row("USER32.dll", "SetTimer");
    (void)add_row("USER32.dll", "KillTimer");
    (void)add_row("USER32.dll", "GetMessageA");
    (void)add_row("USER32.dll", "PeekMessageA");
    (void)add_row("KERNEL32.dll", "WaitForSingleObject");
    (void)add_row("KERNEL32.dll", "WaitForMultipleObjects");
}

static void test_guard(void)
{
    if (!check(wc3_hle_winmm_guard_install() == 11,
               "guard install did not replace all 11 WINMM rows"))
        return;
    Row *num = find_row("WINMM.dll", "waveOutGetNumDevs");
    Row *open = find_row("WINMM.dll", "waveOutOpen");
    Row *write = find_row("WINMM.dll", "waveOutWrite");
    check(num && call_row(num) == 0u, "waveOutGetNumDevs must be zero");
    check(open && call_row(open) == 6u, "waveOutOpen must return NODRIVER");
    check(write && call_row(write) == 5u,
          "waveOutWrite must return INVALIDHANDLE");
}

static void test_timer(void)
{
    if (!check(wc3_hle_mss_timer_install() == 6,
               "timer install did not install all six rows"))
        return;
    check(wc3_hle_mss_timer_install() == 6, "timer install is not idempotent");
    Row *set = find_row("USER32.dll", "SetTimer");
    Row *kill = find_row("USER32.dll", "KillTimer");
    if (!set || !kill || set->impl == no_op || kill->impl == no_op) {
        check(0, "timer rows were not overridden");
        return;
    }

    memset(g_args, 0, sizeof g_args);
    g_args[2] = 50u; /* elapse; callback 0 keeps the guest out of the test */
    uint32_t id = call_row(set);
    check(id != 0u, "SetTimer returned no timer id");

    memset(g_args, 0, sizeof g_args);
    g_args[1] = id;
    check(call_row(kill) == 1u, "KillTimer did not find the timer");
}

int main(void)
{
    memset(&g_platform, 0, sizeof g_platform);
    g_platform.time.monotonic_ns = fake_monotonic_ns;
    install_rows();
    test_guard();
    test_timer();
    if (g_failures) {
        fprintf(stderr, "winmm guard/timer: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("winmm guard/timer: PASS\n");
    return 0;
}
