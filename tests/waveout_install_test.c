/*
 * waveout_install_test.c - T0 contract for the WINMM install decision.
 *
 * O-91/O-125: the product default is the as-built WINMM path.  The product
 * fast-bind module must bind the existing rows and v2 must not chain or
 * override them while disabled.  The test links the real winmm_fast_bind.c and
 * waveout_hle.c and supplies only the quarantine/platform boundary; it does
 * not fake the modules under test.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "direct_call.h"
#include "legacy_shim.h"
#include "platform.h"
#include "waveout_hle.h"
#include "winmm_fast_bind.h"

#define MAX_CALLS 32u

static const char *g_fast_calls[MAX_CALLS];
static const char *g_chain_calls[MAX_CALLS];
static const char *g_override_calls[MAX_CALLS];
static unsigned g_fast_count;
static unsigned g_chain_count;
static unsigned g_override_count;

static int fail(const char *message)
{
    fprintf(stderr, "waveout install: %s\n", message);
    return 1;
}

const Wc3Platform *wc3_platform(void)
{
    static Wc3Platform platform;
    return &platform;
}

/* The install contract does not execute a guest callback.  Supply the two
 * dispatcher symbols used by waveout_hle.c so the real module can link
 * without pulling the boot dispatcher and its corpus into this T0 test. */
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

int wc3_legacy_hle_fast_bind(const char *dll, const char *name)
{
    if (!dll || strcmp(dll, "WINMM.dll") != 0 || !name) return -1;
    if (g_fast_count < MAX_CALLS)
        g_fast_calls[g_fast_count++] = name;
    return 0;
}

int wc3_legacy_hle_chain(const char *dll, const char *name,
                         void (*impl)(CPU *c), void (**previous)(CPU *c))
{
    (void)dll;
    (void)impl;
    if (previous) *previous = NULL;
    if (name && g_chain_count < MAX_CALLS)
        g_chain_calls[g_chain_count] = name;
    g_chain_count++;
    return 0;
}

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    (void)dll;
    (void)impl;
    if (name && g_override_count < MAX_CALLS)
        g_override_calls[g_override_count] = name;
    g_override_count++;
    return 0;
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

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    (void)addr;
    (void)size;
    return 0;
}

uint8_t wc3_legacy_rd8g(uint32_t addr) { (void)addr; return 0u; }
uint16_t wc3_legacy_rd16g(uint32_t addr) { (void)addr; return 0u; }
uint32_t wc3_legacy_rd32g(uint32_t addr) { (void)addr; return 0u; }
void wc3_legacy_wr8g(uint32_t addr, uint8_t value) { (void)addr; (void)value; }
void wc3_legacy_wr16g(uint32_t addr, uint16_t value) { (void)addr; (void)value; }
void wc3_legacy_wr32g(uint32_t addr, uint32_t value) { (void)addr; (void)value; }
uint32_t wc3_legacy_arg32(const CPU *cpu, unsigned index)
{
    (void)cpu;
    (void)index;
    return 0u;
}

int wc3_legacy_guest_callback_thread_start(Wc3LegacyGuestLoop body,
                                            void *opaque)
{
    (void)body;
    (void)opaque;
    return 0;
}

void wc3_legacy_guest_callback_wake(void) {}

int wc3_legacy_guest_callback_wait(void) { return 0; }

int main(int argc, char **argv)
{
    static const char *const expected[] = {
        "waveOutGetNumDevs",
        "waveOutGetDevCapsA",
        "waveOutOpen",
        "waveOutClose",
        "waveOutPrepareHeader",
        "waveOutUnprepareHeader",
        "waveOutWrite",
        "waveOutReset",
        "waveOutGetID",
        "waveOutGetVolume",
        "waveOutSetVolume",
    };
    static const char *const expected_overrides[] = {
        "waveOutOpen",
        "waveOutClose",
        "waveOutWrite",
        "waveOutReset",
        "waveOutPrepareHeader",
        "waveOutUnprepareHeader",
        "waveOutGetNumDevs",
        "waveOutGetDevCapsA",
        "waveOutGetID",
        "waveOutGetVolume",
        "waveOutSetVolume",
    };
    const unsigned expected_count =
        (unsigned)(sizeof expected / sizeof expected[0]);
    const unsigned expected_override_count =
        (unsigned)(sizeof expected_overrides / sizeof expected_overrides[0]);
    const int want_enabled = argc > 1 && strcmp(argv[1], "enabled") == 0;
    if (!want_enabled) {
        int bound = wc3_hle_winmm_fast_bind_rows();
        if (bound != (int)expected_count)
            return fail("product fast-bind did not bind every WINMM row");
    }
    const int rc = wc3_hle_winmm_waveout_install();

    if (!wc3_platform()) return fail("platform table missing");
    if (want_enabled) {
        if (rc != (int)expected_count + 4)
            return fail("enabled install returned the wrong row count");
        if (g_fast_count != 0u)
            return fail("enabled install fast-bound an as-built row");
        if (g_chain_count != 2u)
            return fail("enabled install did not chain both USER32 rows");
        if (strcmp(g_chain_calls[0], "GetMessageA") != 0 ||
            strcmp(g_chain_calls[1], "PeekMessageA") != 0)
            return fail("enabled install chained the wrong USER32 rows");
        if (g_override_count != expected_override_count + 2u)
            return fail("enabled install overrode the wrong row count");
        if (strcmp(g_override_calls[0], "SetTimer") != 0 ||
            strcmp(g_override_calls[1], "KillTimer") != 0)
            return fail("enabled install overrode the wrong timer rows");
        for (unsigned i = 0; i < expected_override_count; i++) {
            if (strcmp(g_override_calls[i + 2u], expected_overrides[i]) != 0)
                return fail("enabled override order or row name changed");
        }
    } else {
        if (rc != 0)
            return fail("disabled install did not return 0");
        if (g_fast_count != expected_count)
            return fail("wrong number of fast-bound WINMM rows");
        for (unsigned i = 0; i < expected_count; i++) {
            if (strcmp(g_fast_calls[i], expected[i]) != 0)
                return fail("fast-bind order or row name changed");
        }
        if (g_chain_count != 0u)
            return fail("disabled install chained a row");
        if (g_override_count != 0u)
            return fail("disabled install overrode a row");
    }
    if (wc3_hle_winmm_played_frames() != 0u)
        return fail("played-frame counter changed before an open");

    puts(want_enabled ? "waveout install enabled: ok"
                      : "waveout install disabled: ok");
    return 0;
}
