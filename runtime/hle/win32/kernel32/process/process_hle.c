/*
 * process_hle.c - kernel32 process/thread identity imports.
 *
 * The as-built handlers answer GetCurrentProcessId with 1234 and
 * GetCurrentThreadId with 5678 for every guest thread.  That is not just a
 * diagnostic lie: the CRT and any guest code that keys per-thread state on the
 * thread ID see one thread where there are several.  Wc3ThreadApi already owns
 * the real host identity, so this module takes the IDs from there.
 *
 * GetCurrentProcessId is the boot thread's host ID captured at install time.
 * On Linux the main thread's TID is the process ID, and wc3_dispatch_init()
 * runs on that thread before any guest thread is created.  The two pseudo
 * handles keep their documented Win32 values; they are not real objects.
 *
 * SleepEx is an addition.  The old resolver left a guessed-arity stub that
 * returned zero without sleeping, which made every guest wait a spin.  The
 * target has no APC queue yet, so an alertable wait sleeps for the full
 * timeout and returns zero; an INFINITE wait really blocks.
 */
#include "process_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "platform.h"

#define WIN32_CURRENT_PROCESS ((uint32_t)-1)
#define WIN32_CURRENT_THREAD  ((uint32_t)-2)

static const Wc3ThreadApi *g_thread;
static uint32_t g_process_id;

static void h_get_current_process(CPU *c)
{
    c->eax = WIN32_CURRENT_PROCESS;
}

static void h_get_current_thread(CPU *c)
{
    c->eax = WIN32_CURRENT_THREAD;
}

static void h_get_current_process_id(CPU *c)
{
    c->eax = g_process_id;
}

static void h_get_current_thread_id(CPU *c)
{
    const Wc3ThreadHandle self = g_thread->self(g_thread->ctx);
    c->eax = (uint32_t)g_thread->id(g_thread->ctx, self);
}

static void h_get_process_id(CPU *c)
{
    /* Only the current-process pseudo handle is addressable: the HLE has no
     * OpenProcess handle table, so other handles fail closed.  The old handler
     * returned 0 even for GetCurrentProcess(), which made the CRT see PID 0. */
    c->eax = wc3_legacy_arg32(c, 0) == WIN32_CURRENT_PROCESS ? g_process_id : 0;
}

static void h_is_debugger_present(CPU *c)
{
    c->eax = 0;
}

static void h_sleep_ex(CPU *c)
{
    const uint32_t ms = wc3_legacy_arg32(c, 0);

    if (ms == 0u) {
        g_thread->yield(g_thread->ctx);
        c->eax = 0;
        return;
    }
    if (ms == 0xFFFFFFFFu) {
        /* No APC queue exists, so an INFINITE alertable wait can only end when
         * the process is torn down.  Sleep in bounded chunks so a virtual-time
         * platform does not have to represent a 584-year timestep. */
        for (;;)
            (void)g_thread->sleep_ns(g_thread->ctx, 3600ull * 1000000000ull);
    }
    (void)g_thread->sleep_ns(g_thread->ctx, (uint64_t)ms * 1000000ull);
    c->eax = 0;
}

typedef struct ProcessOverride {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
} ProcessOverride;

static const ProcessOverride g_overrides[] = {
    { "KERNEL32.dll", "GetCurrentProcess", h_get_current_process },
    { "KERNEL32.dll", "GetCurrentThread", h_get_current_thread },
    { "KERNEL32.dll", "GetCurrentProcessId", h_get_current_process_id },
    { "KERNEL32.dll", "GetCurrentThreadId", h_get_current_thread_id },
    { "KERNEL32.dll", "GetProcessId", h_get_process_id },
    { "KERNEL32.dll", "IsDebuggerPresent", h_is_debugger_present },
};

typedef struct ProcessAddition {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} ProcessAddition;

static const ProcessAddition g_additions[] = {
    { "SleepEx", 2, h_sleep_ex },
};

int wc3_hle_kernel32_process_install(void)
{
    const Wc3Platform *platform = wc3_platform();
    if (!platform || !platform->thread.self || !platform->thread.id ||
        !platform->thread.yield || !platform->thread.sleep_ns)
        return -1;
    g_thread = &platform->thread;
    g_process_id = (uint32_t)g_thread->id(g_thread->ctx,
                                          g_thread->self(g_thread->ctx));

    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override(g_overrides[i].dll, g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("KERNEL32.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 1,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0] +
                 sizeof g_additions / sizeof g_additions[0]);
}
