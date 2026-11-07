/*
 * process_hle_test.c - T0 contract test for kernel32/process.
 *
 * The platform thread API is the source of both IDs.  The test moves the
 * current thread ID after install and checks that GetCurrentThreadId follows
 * it while GetCurrentProcessId stays at the boot-thread value.  SleepEx is
 * checked against a recording sleep so a zero-time call yields instead of
 * sleeping and a finite call converts milliseconds to nanoseconds.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "platform.h"
#include "process_hle.h"

#define MAX_OVERRIDES 16

static struct {
    char dll[32];
    char name[64];
    void (*impl)(CPU *);
} g_overrides[MAX_OVERRIDES];
static unsigned g_override_count;

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    if (!dll || !name || !impl) return -1;
    if (g_override_count >= MAX_OVERRIDES) return -1;
    snprintf(g_overrides[g_override_count].dll,
             sizeof g_overrides[g_override_count].dll, "%s", dll);
    snprintf(g_overrides[g_override_count].name,
             sizeof g_overrides[g_override_count].name, "%s", name);
    g_overrides[g_override_count].impl = impl;
    g_override_count++;
    return 0;
}

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    (void)argc;
    (void)cdecl_abi;
    return wc3_legacy_hle_override(dll, name, impl);
}

static void (*find_override(const char *name))(CPU *)
{
    for (unsigned i = 0; i < g_override_count; i++)
        if (strcmp(g_overrides[i].name, name) == 0)
            return g_overrides[i].impl;
    return NULL;
}

static uint64_t g_current_thread_id;
static uint32_t g_arg0;
static uint32_t g_arg1;
static unsigned g_yield_count;
static uint64_t g_slept_ns;
static unsigned g_sleep_count;

uint32_t wc3_legacy_arg32(const CPU *c, unsigned index)
{
    (void)c;
    if (index == 0u) return g_arg0;
    if (index == 1u) return g_arg1;
    return 0u;
}

static Wc3ThreadHandle fake_self(void *ctx)
{
    (void)ctx;
    return (Wc3ThreadHandle)(uintptr_t)0x1234u;
}

static uint64_t fake_thread_id(void *ctx, Wc3ThreadHandle thread)
{
    (void)ctx;
    (void)thread;
    return g_current_thread_id;
}

static void fake_yield(void *ctx)
{
    (void)ctx;
    g_yield_count++;
}

static int fake_sleep_ns(void *ctx, uint64_t ns)
{
    (void)ctx;
    g_slept_ns += ns;
    g_sleep_count++;
    return 0;
}

static Wc3Platform g_platform;

const Wc3Platform *wc3_platform(void)
{
    return &g_platform;
}

static int fail(const char *what)
{
    fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

static int call(const char *name, CPU *c)
{
    void (*fn)(CPU *) = find_override(name);
    if (!fn) return fail(name);
    g_arg0 = 0;
    g_arg1 = 0;
    memset(c, 0, sizeof *c);
    fn(c);
    return 0;
}

static int call_arg(const char *name, uint32_t arg, CPU *c)
{
    void (*fn)(CPU *) = find_override(name);
    if (!fn) return fail(name);
    g_arg0 = arg;
    g_arg1 = 0;
    memset(c, 0, sizeof *c);
    fn(c);
    return 0;
}

static int call2(const char *name, uint32_t a0, uint32_t a1, CPU *c)
{
    void (*fn)(CPU *) = find_override(name);
    if (!fn) return fail(name);
    g_arg0 = a0;
    g_arg1 = a1;
    memset(c, 0, sizeof *c);
    fn(c);
    return 0;
}

#define CHECK(cond, what) do { if (!(cond)) return fail(what); } while (0)

int main(void)
{
    memset(&g_platform, 0, sizeof g_platform);
    g_platform.thread.self = fake_self;
    g_platform.thread.id = fake_thread_id;
    g_platform.thread.yield = fake_yield;
    g_platform.thread.sleep_ns = fake_sleep_ns;
    g_current_thread_id = 4242;

    /* Missing platform identity is a startup error, not a fake ID. */
    g_platform.thread.id = NULL;
    CHECK(wc3_hle_kernel32_process_install() == -1, "install without id");
    g_platform.thread.id = fake_thread_id;

    /* SleepEx has no honest fallback without a real sleep/yield seam. */
    g_platform.thread.sleep_ns = NULL;
    CHECK(wc3_hle_kernel32_process_install() == -1, "install without sleep");
    g_platform.thread.sleep_ns = fake_sleep_ns;
    g_platform.thread.yield = NULL;
    CHECK(wc3_hle_kernel32_process_install() == -1, "install without yield");
    g_platform.thread.yield = fake_yield;

    const int installed = wc3_hle_kernel32_process_install();
    CHECK(installed == 7, "install count");
    CHECK(find_override("SleepEx") != NULL, "SleepEx row");

    CPU c;
    if (call("GetCurrentProcess", &c)) return 1;
    CHECK(c.eax == 0xFFFFFFFFu, "GetCurrentProcess");
    if (call("GetCurrentThread", &c)) return 1;
    CHECK(c.eax == 0xFFFFFFFEu, "GetCurrentThread");
    if (call("GetCurrentProcessId", &c)) return 1;
    CHECK(c.eax == 4242u, "GetCurrentProcessId is the boot thread");
    if (call("GetCurrentThreadId", &c)) return 1;
    CHECK(c.eax == 4242u, "GetCurrentThreadId initial");
    if (call_arg("GetProcessId", 0xFFFFFFFFu, &c)) return 1;
    CHECK(c.eax == 4242u, "GetProcessId(current process)");
    if (call_arg("GetProcessId", 0x1234u, &c)) return 1;
    CHECK(c.eax == 0u, "GetProcessId(other handle)");
    if (call("IsDebuggerPresent", &c)) return 1;
    CHECK(c.eax == 0u, "IsDebuggerPresent");

    if (call2("SleepEx", 0, 0, &c)) return 1;
    CHECK(c.eax == 0u, "SleepEx zero return");
    CHECK(g_yield_count == 1u, "SleepEx zero yields");
    CHECK(g_sleep_count == 0u, "SleepEx zero does not sleep");

    if (call2("SleepEx", 25, 1, &c)) return 1;
    CHECK(c.eax == 0u, "SleepEx timeout return");
    CHECK(g_slept_ns == 25000000ull, "SleepEx milliseconds");
    CHECK(g_sleep_count == 1u, "SleepEx sleeps once");
    CHECK(g_yield_count == 1u, "SleepEx timeout does not yield");

    g_current_thread_id = 7777;
    if (call("GetCurrentThreadId", &c)) return 1;
    CHECK(c.eax == 7777u, "GetCurrentThreadId follows the thread");
    if (call("GetCurrentProcessId", &c)) return 1;
    CHECK(c.eax == 4242u, "GetCurrentProcessId is stable");

    printf("hle process OK (%d handlers)\n", installed);
    return 0;
}
