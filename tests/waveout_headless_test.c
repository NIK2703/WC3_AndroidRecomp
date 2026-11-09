/*
 * waveout_headless_test.c - T0 integration between WINMM waveOut and the real
 * headless Wc3AudioApi.
 *
 * The existing waveout_runtime_test.c proves the handlers against a fake audio
 * sink.  This test keeps the same real waveout_hle.c but installs the actual
 * verification platform from runtime/platform/headless.  In particular it
 * drives headless_audio_advance(), which is the T1 DUT's virtual device clock,
 * and checks that the completion pump publishes WOM_DONE only after the
 * platform reports the bytes as consumed.
 *
 * Guest memory, the guest callback thread and the clock are still test-owned:
 * the headless platform has no guest address space and no guest dispatcher.
 * Everything audio-related below the waveOut handlers is the real headless
 * implementation, not a second fake.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "cpu.h"
#include "direct_call.h"
#include "legacy_shim.h"
#include "platform.h"
#include "platform_headless.h"
#include "waveout_hle.h"

#define GUEST_BASE       0x20000000u
#define GUEST_SIZE       0x00100000u
#define STACK_ARGS       0x200F0000u
#define STACK_TOP        0x200F8000u
#define PHWO             0x20001000u
#define WFX              0x20001100u
#define HDR_BASE         0x20002000u
#define HDR_STRIDE       0x00000100u
#define DATA_BASE        0x20010000u
#define DATA_STRIDE      0x00004000u
#define PCM_FRAMES       2208u
#define PCM_BYTES        (PCM_FRAMES * 4u)

#define WAVE_FORMAT_PCM      1u
#define CALLBACK_FUNCTION    0x00030000u
#define WOM_DONE             0x03BDu
#define TEST_WM_TIMER        0x0113u
#define WHDR_DONE            0x00000001u
#define WHDR_PREPARED        0x00000002u
#define WHDR_INQUEUE         0x00000010u
#define MMSYSERR_NOERROR     0u

#define MAX_ROWS 32u
#define MAX_ARGS 8u

static uint8_t *g_mem;
static uint32_t g_base;

static const char *g_fast_names[MAX_ROWS];
static unsigned g_fast_count;
static const char *g_chain_names[4];
static void (*g_chain_impls[4])(CPU *);
static unsigned g_chain_count;
static const char *g_override_names[MAX_ROWS];
static void (*g_override_impls[MAX_ROWS])(CPU *);
static unsigned g_override_count;

static Wc3LegacyGuestLoop g_callback_body;
static int g_callback_wait_calls;
static int g_callback_wake_calls;

static uint32_t g_dispatch_calls;
static uint32_t g_resolved_calls;
static uint32_t g_resolve_calls;
static uint32_t g_resolved_target;
static uint32_t g_last_target;
static uint32_t g_last_args[MAX_ARGS];

static uint64_t g_now_ns;
static uintptr_t g_stream;
static int (*g_real_open)(void *, uint32_t, uint32_t, uint32_t, uintptr_t *);

static Wc3Platform g_platform;

static int fail(const char *message)
{
    fprintf(stderr, "waveout headless: %s\n", message);
    return 1;
}

static void *guest_ptr(uint32_t addr, uint32_t size)
{
    if (addr < g_base) return NULL;
    uint64_t off = (uint64_t)addr - g_base;
    if (off + size > GUEST_SIZE) return NULL;
    return g_mem + off;
}

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    if (addr < g_base) return 0;
    uint64_t off = (uint64_t)addr - g_base;
    return off <= GUEST_SIZE && (uint64_t)size <= GUEST_SIZE - off;
}

uint8_t wc3_legacy_rd8g(uint32_t addr)
{
    uint8_t *p = guest_ptr(addr, 1u);
    return p ? *p : 0u;
}

uint16_t wc3_legacy_rd16g(uint32_t addr)
{
    uint8_t *p = guest_ptr(addr, 2u);
    uint16_t v = 0u;
    if (p) memcpy(&v, p, sizeof v);
    return v;
}

uint32_t wc3_legacy_rd32g(uint32_t addr)
{
    uint8_t *p = guest_ptr(addr, 4u);
    uint32_t v = 0u;
    if (p) memcpy(&v, p, sizeof v);
    return v;
}

void wc3_legacy_wr8g(uint32_t addr, uint8_t value)
{
    uint8_t *p = guest_ptr(addr, 1u);
    if (p) *p = value;
}

void wc3_legacy_wr16g(uint32_t addr, uint16_t value)
{
    uint8_t *p = guest_ptr(addr, 2u);
    if (p) memcpy(p, &value, sizeof value);
}

void wc3_legacy_wr32g(uint32_t addr, uint32_t value)
{
    uint8_t *p = guest_ptr(addr, 4u);
    if (p) memcpy(p, &value, sizeof value);
}

uint32_t wc3_legacy_arg32(const CPU *cpu, unsigned index)
{
    if (!cpu) return 0u;
    return wc3_legacy_rd32g(cpu->esp + 4u + index * 4u);
}

int wc3_legacy_hle_fast_bind(const char *dll, const char *name)
{
    if (!dll || strcmp(dll, "WINMM.dll") != 0 || !name) return -1;
    if (g_fast_count < MAX_ROWS) g_fast_names[g_fast_count] = name;
    g_fast_count++;
    return 0;
}

int wc3_legacy_hle_chain(const char *dll, const char *name,
                         void (*impl)(CPU *c), void (**previous)(CPU *c))
{
    (void)dll;
    if (previous) *previous = NULL;
    if (g_chain_count < 4u) {
        g_chain_names[g_chain_count] = name;
        g_chain_impls[g_chain_count] = impl;
    }
    g_chain_count++;
    return 0;
}

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    (void)dll;
    if (g_override_count < MAX_ROWS) {
        g_override_names[g_override_count] = name;
        g_override_impls[g_override_count] = impl;
    }
    g_override_count++;
    return 0;
}

static void (*find_override(const char *name))(CPU *)
{
    for (unsigned i = 0; i < g_override_count && i < MAX_ROWS; i++)
        if (strcmp(g_override_names[i], name) == 0)
            return g_override_impls[i];
    return NULL;
}

static void (*find_chain(const char *name))(CPU *)
{
    for (unsigned i = 0; i < g_chain_count && i < 4u; i++)
        if (strcmp(g_chain_names[i], name) == 0)
            return g_chain_impls[i];
    return NULL;
}

void dispatch(CPU *c, uint32_t target)
{
    g_dispatch_calls++;
    g_last_target = target;
    for (unsigned i = 0; i < MAX_ARGS; i++)
        g_last_args[i] = wc3_legacy_rd32g(c->esp + 4u + i * 4u);
}

void dispatch_jmp(CPU *c, uint32_t target)
{
    (void)c;
    (void)target;
}

static void fake_resolved_target(CPU *c)
{
    (void)c;
    g_resolved_calls++;
}

wc3_lifted_fn wc3_dispatch_resolve(uint32_t target)
{
    g_resolve_calls++;
    g_resolved_target = target;
    return target ? fake_resolved_target : NULL;
}

void wc3_dispatch_resolved(CPU *c, wc3_lifted_fn fn)
{
    if (!fn) return;
    g_dispatch_calls++;
    g_last_target = g_resolved_target;
    for (unsigned i = 0; i < MAX_ARGS; i++)
        g_last_args[i] = wc3_legacy_rd32g(c->esp + 4u + i * 4u);
    fn(c);
}

int wc3_legacy_guest_callback_thread_start(Wc3LegacyGuestLoop body,
                                            void *opaque)
{
    (void)opaque;
    g_callback_body = body;
    return 0;
}

void wc3_legacy_guest_callback_wake(void)
{
    g_callback_wake_calls++;
}

int wc3_legacy_guest_callback_wait(void)
{
    if (g_callback_wait_calls++ == 0) return 0;
    return -1;
}

static uint64_t fake_monotonic_ns(void *ctx)
{
    (void)ctx;
    return g_now_ns;
}

/* Keep the real headless open() but remember the opaque stream so the test can
 * call the real headless advance().  No audio behaviour is replaced. */
static int capture_open(void *ctx, uint32_t rate, uint32_t channels,
                        uint32_t bits, uintptr_t *out)
{
    if (!g_real_open) return -1;
    int rc = g_real_open(ctx, rate, channels, bits, out);
    if (rc == 0 && out) g_stream = *out;
    return rc;
}

static int guest_map(void)
{
#ifdef _WIN32
    void *p = VirtualAlloc((void *)(uintptr_t)GUEST_BASE, GUEST_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) return -1;
    g_mem = (uint8_t *)p;
    g_base = (uint32_t)(uintptr_t)p;
    return 0;
#else
    void *p = mmap((void *)(uintptr_t)GUEST_BASE, GUEST_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) return -1;
    g_mem = (uint8_t *)p;
    g_base = (uint32_t)(uintptr_t)p;
    return 0;
#endif
}

static void set_args(CPU *c, const uint32_t *args, unsigned n)
{
    memset(c, 0, sizeof *c);
    c->esp = STACK_ARGS;
    wc3_legacy_wr32g(STACK_ARGS, 0xDEADBEEFu);
    for (unsigned i = 0; i < n; i++)
        wc3_legacy_wr32g(STACK_ARGS + 4u + i * 4u, args[i]);
}

static uint32_t call_impl(void (*impl)(CPU *), const uint32_t *args,
                          unsigned n)
{
    CPU c;
    set_args(&c, args, n);
    impl(&c);
    return c.eax;
}

static void write_wfx(void)
{
    wc3_legacy_wr16g(WFX + 0u, WAVE_FORMAT_PCM);
    wc3_legacy_wr16g(WFX + 2u, 2u);
    wc3_legacy_wr32g(WFX + 4u, 44100u);
    wc3_legacy_wr32g(WFX + 8u, 44100u * 4u);
    wc3_legacy_wr16g(WFX + 12u, 4u);
    wc3_legacy_wr16g(WFX + 14u, 16u);
}

static void init_header(uint32_t hdr, uint32_t data, uint32_t bytes)
{
    wc3_legacy_wr32g(hdr + 0u, data);
    wc3_legacy_wr32g(hdr + 4u, bytes);
    wc3_legacy_wr32g(hdr + 8u, 0u);
    wc3_legacy_wr32g(hdr + 12u, 0u);
    wc3_legacy_wr32g(hdr + 16u, 0u);
    wc3_legacy_wr32g(hdr + 20u, 0u);
    wc3_legacy_wr32g(hdr + 24u, 0u);
    wc3_legacy_wr32g(hdr + 28u, 0u);
}

static void run_callback_thread(void)
{
    CPU c;
    memset(&c, 0, sizeof c);
    c.esp = STACK_TOP;
    g_callback_wait_calls = 0;
    g_callback_body(&c, NULL);
}

static int test_open(void)
{
    uint32_t args[6] = { PHWO, 0u, WFX, 0x00401000u, 0x00005000u,
                         CALLBACK_FUNCTION };
    uint32_t rc = call_impl(find_override("waveOutOpen"), args, 6u);
    if (rc != MMSYSERR_NOERROR) return fail("waveOutOpen failed");
    if (!g_stream) return fail("headless audio open returned no stream");
    if (!g_callback_body) return fail("guest callback thread not started");
    if (wc3_legacy_rd32g(PHWO) == 0u) return fail("waveOutOpen wrote no handle");
    if (g_resolve_calls != 1u)
        return fail("waveOutOpen did not resolve the callback once");
    if (g_platform.audio.consumed_bytes(g_platform.audio.ctx, g_stream) != 0u)
        return fail("headless audio starts with consumed bytes");
    return 0;
}

static int test_completion(uint32_t hwo)
{
    uint32_t hdr = HDR_BASE;
    uint32_t data = DATA_BASE;
    uint32_t args[2] = { hwo, hdr };
    init_header(hdr, data, PCM_BYTES);
    memset(guest_ptr(data, PCM_BYTES), 0x5A, PCM_BYTES);

    if (call_impl(find_override("waveOutPrepareHeader"), args, 2u) !=
        MMSYSERR_NOERROR)
        return fail("waveOutPrepareHeader failed");
    if ((wc3_legacy_rd32g(hdr + 16u) & WHDR_PREPARED) == 0u)
        return fail("prepare did not set WHDR_PREPARED");

    if (call_impl(find_override("waveOutWrite"), args, 2u) != MMSYSERR_NOERROR)
        return fail("waveOutWrite failed");
    if ((wc3_legacy_rd32g(hdr + 16u) & WHDR_INQUEUE) == 0u)
        return fail("write did not set WHDR_INQUEUE");
    if (wc3_hle_winmm_played_frames() != 0u)
        return fail("played frames changed before consumption");

    /* This is the same virtual device clock the T1 headless DUT uses. */
    if (g_platform.audio.advance(g_platform.audio.ctx, g_stream, PCM_BYTES) !=
        0)
        return fail("headless audio advance failed");
    if ((wc3_legacy_rd32g(hdr + 16u) & WHDR_DONE) == 0u)
        return fail("advance did not set WHDR_DONE");
    if ((wc3_legacy_rd32g(hdr + 16u) & WHDR_INQUEUE) != 0u)
        return fail("advance left WHDR_INQUEUE set");
    if (g_callback_wake_calls == 0u)
        return fail("advance did not wake the guest callback thread");
    if (wc3_hle_winmm_played_frames() != PCM_FRAMES)
        return fail("played-frame count after advance mismatch");

    g_dispatch_calls = 0u;
    g_resolved_calls = 0u;
    run_callback_thread();
    if (g_dispatch_calls != 1u) return fail("no WOM_DONE dispatch");
    if (g_resolved_calls != 1u)
        return fail("WOM_DONE did not use the resolved target");
    if (g_last_target != 0x00401000u) return fail("WOM_DONE target mismatch");
    if (g_last_args[0] != hwo || g_last_args[1] != WOM_DONE ||
        g_last_args[2] != 0x00005000u || g_last_args[3] != hdr ||
        g_last_args[4] != 0u)
        return fail("WOM_DONE arguments mismatch");
    return 0;
}

static int test_timer(void)
{
    void (*pump_get)(CPU *) = find_chain("GetMessageA");
    if (!pump_get) return fail("GetMessageA wrapper not chained");

    uint32_t args[4] = { 0x00001000u, 0u, 50u, 0x00402000u };
    uint32_t id = call_impl(find_override("SetTimer"), args, 4u);
    if (id == 0u) return fail("SetTimer returned no id");

    g_now_ns = 50u * 1000000u;
    g_dispatch_calls = 0u;
    g_resolved_calls = 0u;
    CPU c;
    set_args(&c, NULL, 0u);
    pump_get(&c);
    if (g_dispatch_calls != 1u) return fail("WM_TIMER did not fire");
    if (g_resolved_calls != 1u)
        return fail("WM_TIMER did not use the resolved target");
    if (g_resolve_calls != 2u)
        return fail("SetTimer did not resolve its callback once");
    if (g_last_target != 0x00402000u) return fail("WM_TIMER target mismatch");
    if (g_last_args[0] != 0x00001000u || g_last_args[1] != TEST_WM_TIMER ||
        g_last_args[2] != id || g_last_args[3] != 50u)
        return fail("WM_TIMER arguments mismatch");

    pump_get(&c);
    if (g_dispatch_calls != 1u) return fail("WM_TIMER fired twice in one period");

    uint32_t kill_args[2] = { 0x00001000u, id };
    if (call_impl(find_override("KillTimer"), kill_args, 2u) != 1u)
        return fail("KillTimer did not find the timer");
    g_now_ns += 100u * 1000000u;
    pump_get(&c);
    if (g_dispatch_calls != 1u) return fail("killed WM_TIMER still fired");
    return 0;
}

static int test_close(uint32_t hwo)
{
    uint32_t args[1] = { hwo };
    if (call_impl(find_override("waveOutClose"), args, 1u) != MMSYSERR_NOERROR)
        return fail("waveOutClose failed");
    if (wc3_hle_winmm_played_frames() != 0u)
        return fail("closed device still reports played frames");
    return 0;
}

int main(void)
{
    if (guest_map() != 0) {
        fprintf(stderr, "waveout headless: unavailable (no low guest mapping)\n");
        return 2;
    }

    Wc3HeadlessConfig config;
    memset(&config, 0, sizeof config);
    config.width = 800u;
    config.height = 600u;
    if (wc3_headless_platform_init(&g_platform, &config) != 0)
        return fail("headless platform init failed");

    g_real_open = g_platform.audio.open;
    if (!g_real_open || !g_platform.audio.advance)
        return fail("headless audio API incomplete");
    g_platform.audio.open = capture_open;

    /* waveOut only needs the monotonic clock.  Keep it test-controlled so the
     * WM_TIMER case is deterministic without enabling a full T1 schedule. */
    g_platform.time.monotonic_ns = fake_monotonic_ns;
    g_platform.time.ctx = NULL;

    if (wc3_platform_install(&g_platform) != 0)
        return fail("platform install failed");
    if (wc3_hle_winmm_waveout_install() != 15)
        return fail("enabled install did not return 15");
    if (!find_override("waveOutOpen") || !find_override("waveOutWrite") ||
        !find_override("waveOutReset") || !find_override("waveOutClose") ||
        !find_override("SetTimer") || !find_override("KillTimer"))
        return fail("required override missing");

    write_wfx();
    if (test_open() != 0) return 1;
    uint32_t hwo = wc3_legacy_rd32g(PHWO);
    if (test_completion(hwo) != 0) return 1;
    if (test_timer() != 0) return 1;
    if (test_close(hwo) != 0) return 1;

    wc3_headless_platform_shutdown(&g_platform);
    puts("waveout headless: ok");
    return 0;
}
