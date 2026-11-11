/*
 * dsound_hle_test.c - T0 contract for the active DirectSound path.
 *
 * Mss32 imports WINMM but reaches its DirectSound driver through
 * LoadLibraryA("DSOUND.DLL") + GetProcAddress.  This test links the real
 * dsound_hle.c behind a fake KERNEL32 registry, a fake guest heap and a fake
 * Wc3AudioApi.  It covers:
 *   - the synthetic DSOUND.DLL loader/get-proc chain;
 *   - DirectSoundEnumerateA calling back into guest code;
 *   - DirectSoundCreate opening the one AAudio stream without WINMM;
 *   - the Mss32 DSSCL_WRITEPRIMARY probe (primary Lock(0,0)/Unlock);
 *   - secondary buffer voice_add/Lock/Unlock/Play/GetCurrentPosition;
 *   - stream lifetime: one open, close only after the final device release.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "cpu.h"
#include "direct_call.h"
#include "hle_bridge.h"
#include "platform.h"
#include "dsound_hle.h"

#define GUEST_BASE       0x20000000u
#define GUEST_SIZE       0x00100000u
#define ARGS_BASE        0x200F0000u
#define HEAP_BASE        0x20010000u
#define HEAP_END         0x200E0000u
#define SCRATCH          0x20001000u
#define WFX              0x20001100u
#define OUT_DEVICE       0x20001200u
#define OUT_PRIMARY      0x20001210u
#define OUT_SECONDARY    0x20001220u
#define OUT_SECOND_DEV   0x20001230u
#define OUT_PTR1         0x20001300u
#define OUT_BYTES1       0x20001304u
#define OUT_PTR2         0x20001308u
#define OUT_BYTES2       0x2000130c
#define OUT_PLAY         0x20001310u
#define OUT_WRITE        0x20001314u
#define OUT_STATUS       0x20001318u
#define OUT_MODULE_BUF   0x20001400u

#define DS_OK                   0u
#define DSERR_INVALIDPARAM      0x80070057u
#define DSERR_NODRIVER          0x88780078u
#define TEST_CLASS_E_NOAGGREGATION 0x80040110u
#define DSBCAPS_PRIMARYBUFFER   0x00000001u
#define DSBPLAY_LOOPING         0x00000001u

#define TEST_STREAM          0x00003333u
#define TEST_PRIMARY_VOICE   0x00004444u
#define TEST_SECOND_VOICE    0x00005555u
#define ENUM_CALLBACK        0x00401000u
#define ENUM_CONTEXT         0x00C0FFEEu
#define DS_HMOD_EXPECTED     0x000A3000u

#define MAX_ROWS 128u

typedef struct {
    char dll[32];
    char name[64];
    void (*impl)(CPU *);
    uint32_t thunk;
} FakeRow;

static FakeRow g_rows[MAX_ROWS];
static unsigned g_row_count;
static const char *g_chain_names[8];
static void (*g_chain_impls[8])(CPU *);
static unsigned g_chain_count;
static uint32_t g_last_error;
static uint8_t *g_mem;
static uint32_t g_base;
static uint32_t g_heap = HEAP_BASE;

static uint32_t g_enum_calls;
static uint32_t g_enum_args[4];
static uint32_t g_dispatch_calls;

static uint32_t g_open_calls;
static uint32_t g_close_calls;
static uint32_t g_voice_add_calls;
static uint32_t g_voice_remove_calls;
static uint32_t g_publish_calls;
static uint32_t g_seek_calls;
static uint32_t g_control_calls;
static uint32_t g_published_calls;
static uint32_t g_open_stream;
static uint32_t g_voice_add_stream;
static uint32_t g_voice_add_rate;
static uint32_t g_voice_add_channels;
static uint32_t g_voice_add_bits;
static uint32_t g_voice_add_bytes;
static uint32_t g_publish_bytes;
static uint64_t g_publish_frame;
static uint64_t g_seek_frame;
static int g_last_playing = -1;
static int g_last_loop = -1;
static int g_last_volume;
static int g_last_pan;
static uint32_t g_last_frequency;

static Wc3Platform g_platform;

static int fail(const char *message)
{
    fprintf(stderr, "dsound runtime: %s\n", message);
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

uint32_t wc3_legacy_guest_alloc0(uint32_t size)
{
    uint32_t addr = (g_heap + 15u) & ~15u;
    if (size > HEAP_END - addr) return 0u;
    g_heap = addr + size;
    memset(guest_ptr(addr, size), 0, size);
    return addr;
}

void wc3_legacy_guest_free(uint32_t addr)
{
    (void)addr;
}

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    (void)argc;
    (void)cdecl_abi;
    if (!dll || !name || !impl) return -1;
    if (g_row_count >= MAX_ROWS) return -1;
    if (strlen(dll) >= sizeof g_rows[0].dll ||
        strlen(name) >= sizeof g_rows[0].name)
        return -1;
    FakeRow *r = &g_rows[g_row_count];
    strcpy(r->dll, dll);
    strcpy(r->name, name);
    r->impl = impl;
    r->thunk = 0x7F000000u + g_row_count * 16u;
    g_row_count++;
    return 0;
}

int wc3_legacy_hle_bind_ordinal(const char *dll, uint16_t ordinal, int argc,
                                int cdecl_abi, void (*impl)(CPU *c))
{
    (void)dll; (void)ordinal; (void)argc; (void)cdecl_abi; (void)impl;
    return 0;
}

uint32_t wc3_legacy_hle_thunk_of(const char *dll, const char *name)
{
    if (!dll || !name) return 0u;
    for (unsigned i = 0; i < g_row_count; i++)
        if (!strcmp(g_rows[i].dll, dll) && !strcmp(g_rows[i].name, name))
            return g_rows[i].thunk;
    return 0u;
}

int wc3_legacy_hle_chain(const char *dll, const char *name,
                         void (*impl)(CPU *c),
                         void (**previous)(CPU *c))
{
    (void)dll;
    if (previous) *previous = NULL;
    if (g_chain_count < 8u) {
        g_chain_names[g_chain_count] = name;
        g_chain_impls[g_chain_count] = impl;
    }
    g_chain_count++;
    return 0;
}

void wc3_legacy_set_last_error(CPU *c, uint32_t code)
{
    (void)c;
    g_last_error = code;
}

uint32_t wc3_legacy_get_last_error(CPU *c)
{
    (void)c;
    return g_last_error;
}

void dispatch(CPU *c, uint32_t target)
{
    g_dispatch_calls++;
    if (target == ENUM_CALLBACK) {
        g_enum_calls++;
        for (unsigned i = 0; i < 4u; i++)
            g_enum_args[i] = wc3_legacy_rd32g(c->esp + 4u + i * 4u);
        c->eax = 1u; /* continue enumeration */
    } else {
        c->eax = 0u;
    }
}

void dispatch_jmp(CPU *c, uint32_t target)
{
    (void)c;
    (void)target;
}

const Wc3Platform *wc3_platform(void)
{
    return &g_platform;
}

static int fake_audio_open(void *ctx, uint32_t rate, uint32_t channels,
                           uint32_t bits, uintptr_t *out)
{
    (void)ctx;
    g_open_calls++;
    if (g_open_stream) return -1;
    if (rate != 44100u || channels != 2u || bits != 16u) return -1;
    g_open_stream = TEST_STREAM;
    if (!out) return -1;
    *out = TEST_STREAM;
    return 0;
}

static int fake_audio_close(void *ctx, uintptr_t stream)
{
    (void)ctx;
    if (stream != TEST_STREAM) return -1;
    g_close_calls++;
    g_open_stream = 0u;
    return 0;
}

static uintptr_t fake_audio_primary_voice(void *ctx, uintptr_t stream)
{
    (void)ctx;
    if (!g_open_stream) return 0u;
    if (stream && stream != TEST_STREAM) return 0u;
    return TEST_PRIMARY_VOICE;
}

static int fake_audio_voice_add(void *ctx, uintptr_t stream, uint32_t rate,
                                uint32_t channels, uint32_t bits,
                                uint32_t bytes, uintptr_t *out_voice)
{
    (void)ctx;
    g_voice_add_calls++;
    g_voice_add_stream = (uint32_t)stream;
    g_voice_add_rate = rate;
    g_voice_add_channels = channels;
    g_voice_add_bits = bits;
    g_voice_add_bytes = bytes;
    if (!out_voice) return -1;
    *out_voice = TEST_SECOND_VOICE;
    return 0;
}

static int fake_audio_voice_remove(void *ctx, uintptr_t voice)
{
    (void)ctx;
    if (voice != TEST_SECOND_VOICE) return -1;
    g_voice_remove_calls++;
    return 0;
}

static int fake_audio_voice_publish2(void *ctx, uintptr_t voice,
                                     uint64_t frame_base1, const void *frames1,
                                     uint32_t bytes1,
                                     uint64_t frame_base2, const void *frames2,
                                     uint32_t bytes2, uint32_t *written)
{
    (void)ctx; (void)voice; (void)frames1; (void)frames2;
    if (!frames1 && bytes1) return -1;
    if (frames2 && bytes2 && frame_base2 == 0u) return -1;
    g_publish_calls++;
    g_publish_frame = frame_base1;
    g_publish_bytes = bytes1 + bytes2;
    if (written) *written = bytes1 + bytes2;
    return 0;
}

static int fake_audio_voice_position(void *ctx, uintptr_t voice,
                                     uint32_t *play_bytes,
                                     uint32_t *write_bytes,
                                     uint64_t *play_frame)
{
    (void)ctx; (void)voice;
    if (play_bytes) *play_bytes = 123u;
    if (write_bytes) *write_bytes = 456u;
    if (play_frame) *play_frame = 1000u;
    return 0;
}

static int fake_audio_voice_control(void *ctx, uintptr_t voice, int playing,
                                    int loop, uint32_t frequency, int volume,
                                    int pan)
{
    (void)ctx; (void)voice;
    g_control_calls++;
    g_last_playing = playing;
    g_last_loop = loop;
    g_last_frequency = frequency;
    g_last_volume = volume;
    g_last_pan = pan;
    return 0;
}

static int fake_audio_voice_seek(void *ctx, uintptr_t voice, uint64_t frame)
{
    (void)ctx; (void)voice;
    g_seek_calls++;
    g_seek_frame = frame;
    return 0;
}

static int fake_audio_voice_published(void *ctx, uintptr_t voice)
{
    (void)ctx; (void)voice;
    g_published_calls++;
    return 0;
}

static int fake_audio_voice_state(void *ctx, uintptr_t voice,
                                  uint32_t *playing, uint32_t *loop,
                                  uint64_t *position)
{
    (void)ctx; (void)voice;
    if (playing) *playing = 1u;
    if (loop) *loop = 1u;
    if (position) *position = 1000u;
    return 0;
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
    c->esp = ARGS_BASE;
    wc3_legacy_wr32g(ARGS_BASE, 0xDEADBEEFu);
    for (unsigned i = 0; i < n; i++)
        wc3_legacy_wr32g(ARGS_BASE + 4u + i * 4u, args[i]);
}

static uint32_t call_impl(void (*impl)(CPU *), const uint32_t *args,
                          unsigned n)
{
    CPU c;
    set_args(&c, args, n);
    impl(&c);
    return c.eax;
}

static FakeRow *find_row(const char *name)
{
    for (unsigned i = 0; i < g_row_count; i++)
        if (!strcmp(g_rows[i].name, name)) return &g_rows[i];
    return NULL;
}

static void (*find_chain(const char *name))(CPU *)
{
    for (unsigned i = 0; i < g_chain_count && i < 8u; i++)
        if (!strcmp(g_chain_names[i], name)) return g_chain_impls[i];
    return NULL;
}

static void write_wfx(uint32_t va)
{
    wc3_legacy_wr16g(va + 0u, 1u);
    wc3_legacy_wr16g(va + 2u, 2u);
    wc3_legacy_wr32g(va + 4u, 44100u);
    wc3_legacy_wr32g(va + 8u, 44100u * 4u);
    wc3_legacy_wr16g(va + 12u, 4u);
    wc3_legacy_wr16g(va + 14u, 16u);
    wc3_legacy_wr16g(va + 16u, 0u);
}

static void write_desc(uint32_t va, uint32_t flags, uint32_t bytes,
                       uint32_t fmt)
{
    wc3_legacy_wr32g(va + 0u, 20u);
    wc3_legacy_wr32g(va + 4u, flags);
    wc3_legacy_wr32g(va + 8u, bytes);
    wc3_legacy_wr32g(va + 12u, 0u);
    wc3_legacy_wr32g(va + 16u, fmt);
}

static uint32_t call_row(const char *name, const uint32_t *args, unsigned n)
{
    FakeRow *r = find_row(name);
    if (!r) return 0xFFFFFFFFu;
    return call_impl(r->impl, args, n);
}

static int test_loader(void)
{
    uint32_t name = SCRATCH;
    const char *text = "C:\\Windows\\System32\\DSOUND.DLL";
    for (uint32_t i = 0; i <= strlen(text); i++)
        wc3_legacy_wr8g(name + i, (uint8_t)text[i]);

    void (*load)(CPU *) = find_chain("LoadLibraryA");
    void (*getproc)(CPU *) = find_chain("GetProcAddress");
    void (*getmod)(CPU *) = find_chain("GetModuleHandleA");
    void (*getfile)(CPU *) = find_chain("GetModuleFileNameA");
    if (!load || !getproc || !getmod || !getfile)
        return fail("KERNEL32 loader chain not installed");

    uint32_t args[3] = { name, 0x00008000u, 0u };
    uint32_t module = call_impl(load, args, 2u);
    if (module != DS_HMOD_EXPECTED)
        return fail("LoadLibraryA(DSOUND.DLL) did not return the synthetic handle");

    if (call_impl(getmod, args, 1u) != module)
        return fail("GetModuleHandleA(DSOUND.DLL) mismatch");

    uint32_t file_args[3] = { module, OUT_MODULE_BUF, 64u };
    uint32_t n = call_impl(getfile, file_args, 3u);
    if (n != 10u || strcmp((const char *)guest_ptr(OUT_MODULE_BUF, 64u),
                           "dsound.dll") != 0)
        return fail("GetModuleFileNameA(DSOUND.DLL) mismatch");

    uint32_t proc_name = SCRATCH + 0x100u;
    text = "DirectSoundCreate";
    for (uint32_t i = 0; i <= strlen(text); i++)
        wc3_legacy_wr8g(proc_name + i, (uint8_t)text[i]);
    uint32_t proc_args[2] = { module, proc_name };
    uint32_t create_thunk = call_impl(getproc, proc_args, 2u);
    if (!create_thunk ||
        create_thunk != wc3_legacy_hle_thunk_of("dsound.dll",
                                                 "DirectSoundCreate"))
        return fail("GetProcAddress(DirectSoundCreate) mismatch");

    uint32_t ordinal_args[2] = { module, 1u };
    if (call_impl(getproc, ordinal_args, 2u) != create_thunk)
        return fail("GetProcAddress(#1) mismatch");

    text = "DirectSoundEnumerateA";
    for (uint32_t i = 0; i <= strlen(text); i++)
        wc3_legacy_wr8g(proc_name + i, (uint8_t)text[i]);
    if (!call_impl(getproc, proc_args, 2u))
        return fail("GetProcAddress(DirectSoundEnumerateA) missing");

    text = "NoSuchExport";
    for (uint32_t i = 0; i <= strlen(text); i++)
        wc3_legacy_wr8g(proc_name + i, (uint8_t)text[i]);
    g_last_error = 0u;
    if (call_impl(getproc, proc_args, 2u) != 0u || g_last_error != 127u)
        return fail("unknown export did not return error 127");
    return 0;
}

static int test_enumerate(void)
{
    uint32_t args[2] = { ENUM_CALLBACK, ENUM_CONTEXT };
    g_enum_calls = 0u;
    uint32_t rc = call_row("DirectSoundEnumerateA", args, 2u);
    if (rc != DS_OK) return fail("DirectSoundEnumerateA failed");
    if (g_enum_calls != 1u) return fail("enumeration callback not invoked");
    if (g_enum_args[0] != 0u || g_enum_args[3] != ENUM_CONTEXT)
        return fail("enumeration GUID/context mismatch");
    if (strcmp((const char *)guest_ptr(g_enum_args[1], 32u),
               "Primary Sound Driver") != 0)
        return fail("enumeration description mismatch");
    if (strcmp((const char *)guest_ptr(g_enum_args[2], 16u),
               "dsound.dll") != 0)
        return fail("enumeration module mismatch");
    return 0;
}

static int test_create_without_winmm(void)
{
    uint32_t args[3] = { 0u, OUT_DEVICE, 0u };
    uint32_t rc = call_row("DirectSoundCreate", args, 3u);
    if (rc != DS_OK) return fail("DirectSoundCreate failed without WINMM");
    if (g_open_calls != 1u || g_open_stream != TEST_STREAM)
        return fail("DirectSoundCreate did not open the AAudio stream");
    uint32_t device = wc3_legacy_rd32g(OUT_DEVICE);
    if (!device) return fail("DirectSoundCreate wrote no device");

    uint32_t winner = 0x1234u;
    uint32_t coop[3] = { device, winner, 4u };
    if (call_row("DSound_1_6", coop, 3u) != DS_OK)
        return fail("SetCooperativeLevel(DSSCL_WRITEPRIMARY) rejected");

    /* Mss32 asks for the primary buffer with flags=1. */
    write_desc(SCRATCH + 0x200u, DSBCAPS_PRIMARYBUFFER, 0u, 0u);
    uint32_t create[4] = { device, SCRATCH + 0x200u, OUT_PRIMARY, 0u };
    if (call_row("DSound_1_3", create, 4u) != DS_OK)
        return fail("primary CreateSoundBuffer failed");
    uint32_t primary = wc3_legacy_rd32g(OUT_PRIMARY);
    if (!primary) return fail("primary buffer pointer is null");

    /* Mss32's DSSCL_WRITEPRIMARY probe asks the device for DSCAPS(96), then
     * the primary buffer for DSBCAPS(20) and Lock(0, dwBufferBytes). */
    uint32_t dcaps = SCRATCH + 0x400u;
    wc3_legacy_wr32g(dcaps, 96u);
    uint32_t dcaps_args[2] = { device, dcaps };
    if (call_row("DSound_1_4", dcaps_args, 2u) != DS_OK ||
        wc3_legacy_rd32g(dcaps + 4u) != 0xF1Fu ||
        wc3_legacy_rd32g(dcaps + 16u) != 1u)
        return fail("device GetCaps(DSCAPS) mismatch");

    uint32_t bcaps = SCRATCH + 0x480u;
    wc3_legacy_wr32g(bcaps, 20u);
    uint32_t bcaps_args[2] = { primary, bcaps };
    if (call_row("DSound_2_3", bcaps_args, 2u) != DS_OK ||
        wc3_legacy_rd32g(bcaps) != 20u ||
        (wc3_legacy_rd32g(bcaps + 4u) & DSBCAPS_PRIMARYBUFFER) == 0u ||
        wc3_legacy_rd32g(bcaps + 8u) != 0u)
        return fail("primary GetCaps(DSBCAPS) mismatch");

    write_wfx(WFX);
    uint32_t setfmt[2] = { primary, WFX };
    if (call_row("DSound_2_14", setfmt, 2u) != DS_OK)
        return fail("primary SetFormat failed");

    uint32_t lock[8] = { primary, 0u, 0u, OUT_PTR1, OUT_BYTES1,
                         OUT_PTR2, OUT_BYTES2, 0u };
    if (call_row("DSound_2_11", lock, 8u) != DS_OK)
        return fail("primary zero-length Lock rejected");
    if (wc3_legacy_rd32g(OUT_PTR1) != 0u ||
        wc3_legacy_rd32g(OUT_BYTES1) != 0u ||
        wc3_legacy_rd32g(OUT_PTR2) != 0u ||
        wc3_legacy_rd32g(OUT_BYTES2) != 0u)
        return fail("primary zero-length Lock returned a span");

    uint32_t unlock[5] = { primary, 0u, 0u, 0u, 0u };
    if (call_row("DSound_2_19", unlock, 5u) != DS_OK)
        return fail("primary zero-length Unlock rejected");

    uint32_t play[4] = { primary, 0u, 0u, DSBPLAY_LOOPING };
    if (call_row("DSound_2_12", play, 4u) != DS_OK)
        return fail("primary Play failed");

    /* A second DirectSoundCreate must reuse the open stream. */
    uint32_t second_args[3] = { 0u, OUT_SECOND_DEV, 0u };
    if (call_row("DirectSoundCreate", second_args, 3u) != DS_OK)
        return fail("second DirectSoundCreate failed");
    if (g_open_calls != 1u)
        return fail("second DirectSoundCreate opened another stream");
    uint32_t second_dev = wc3_legacy_rd32g(OUT_SECOND_DEV);
    uint32_t rel[1] = { second_dev };
    if (call_row("DSound_1_2", rel, 1u) != 0u)
        return fail("second device Release failed");
    if (g_close_calls != 0u)
        return fail("shared device release closed the stream");

    /* Secondary streaming buffer, same descriptor Mss32 builds. */
    write_desc(SCRATCH + 0x300u, 0x000180E0u, 4096u, WFX);
    uint32_t create_sec[4] = { device, SCRATCH + 0x300u, OUT_SECONDARY, 0u };
    if (call_row("DSound_1_3", create_sec, 4u) != DS_OK)
        return fail("secondary CreateSoundBuffer failed");
    uint32_t sec = wc3_legacy_rd32g(OUT_SECONDARY);
    if (!sec) return fail("secondary buffer pointer is null");
    if (g_voice_add_calls != 1u || g_voice_add_stream != TEST_STREAM ||
        g_voice_add_rate != 44100u || g_voice_add_channels != 2u ||
        g_voice_add_bits != 16u || g_voice_add_bytes != 4096u)
        return fail("secondary voice_add contract mismatch");

    uint32_t lock2[8] = { sec, 0u, 4096u, OUT_PTR1, OUT_BYTES1,
                          OUT_PTR2, OUT_BYTES2, 0u };
    if (call_row("DSound_2_11", lock2, 8u) != DS_OK)
        return fail("secondary Lock failed");
    uint32_t p1 = wc3_legacy_rd32g(OUT_PTR1);
    if (!p1 || wc3_legacy_rd32g(OUT_BYTES1) != 4096u ||
        wc3_legacy_rd32g(OUT_BYTES2) != 0u)
        return fail("secondary Lock spans mismatch");

    uint32_t unlock2[5] = { sec, p1, 4096u, 0u, 0u };
    if (call_row("DSound_2_19", unlock2, 5u) != DS_OK)
        return fail("secondary Unlock failed");
    if (g_publish_calls != 1u || g_publish_frame != 0u ||
        g_publish_bytes != 4096u)
        return fail("secondary publication mismatch");

    uint32_t play2[4] = { sec, 0u, 0u, DSBPLAY_LOOPING };
    if (call_row("DSound_2_12", play2, 4u) != DS_OK || g_last_playing != 1 ||
        g_last_loop != 1)
        return fail("secondary Play did not start the voice");

    uint32_t pos[3] = { sec, OUT_PLAY, OUT_WRITE };
    if (call_row("DSound_2_4", pos, 3u) != DS_OK ||
        wc3_legacy_rd32g(OUT_PLAY) != 123u ||
        wc3_legacy_rd32g(OUT_WRITE) != 456u)
        return fail("GetCurrentPosition mismatch");

    uint32_t seek[2] = { sec, 1024u };
    if (call_row("DSound_2_13", seek, 2u) != DS_OK || g_seek_calls != 1u)
        return fail("SetCurrentPosition did not seek the voice");
    /* play_frame was 1000; seconds per lap = 4096/4 = 1024 frames, pos 1024
     * is one lap ahead, so the absolute frame is 1000/1024*1024 + 256 = 256. */
    if (g_seek_frame != 256u)
        return fail("SetCurrentPosition absolute frame mismatch");

    uint32_t status[2] = { sec, OUT_STATUS };
    if (call_row("DSound_2_9", status, 2u) != DS_OK ||
        (wc3_legacy_rd32g(OUT_STATUS) & 1u) == 0u)
        return fail("GetStatus did not report playing");

    uint32_t vol[2] = { sec, (uint32_t)-1000 };
    uint32_t pan[2] = { sec, 500u };
    uint32_t freq[2] = { sec, 22050u };
    if (call_row("DSound_2_15", vol, 2u) != DS_OK ||
        call_row("DSound_2_16", pan, 2u) != DS_OK ||
        call_row("DSound_2_17", freq, 2u) != DS_OK)
        return fail("volume/pan/frequency setters failed");
    if (g_last_volume != -1000 || g_last_pan != 500 ||
        g_last_frequency != 22050u)
        return fail("voice controls were not forwarded");

    uint32_t stop[1] = { sec };
    if (call_row("DSound_2_18", stop, 1u) != DS_OK || g_last_playing != 0)
        return fail("Stop did not stop the voice");

    uint32_t rel_sec[1] = { sec };
    if (call_row("DSound_2_2", rel_sec, 1u) != 0u ||
        g_voice_remove_calls != 1u)
        return fail("secondary Release did not remove the voice");
    if (g_close_calls != 0u)
        return fail("secondary release closed the stream early");

    uint32_t rel_primary[1] = { primary };
    if (call_row("DSound_2_2", rel_primary, 1u) != 0u)
        return fail("primary Release failed");
    if (g_close_calls != 0u)
        return fail("device closed while buffers were still alive");

    uint32_t rel_dev[1] = { device };
    if (call_row("DSound_1_2", rel_dev, 1u) != 0u)
        return fail("device Release failed");
    if (g_close_calls != 1u || g_open_stream != 0u)
        return fail("final device Release did not close the stream");
    return 0;
}

static int test_argument_precedence(void)
{
    uint32_t bad_out[3] = { 0u, GUEST_BASE - 4u, 0u };
    if (call_row("DirectSoundCreate", bad_out, 3u) != 0x80004003u)
        return fail("DirectSoundCreate bad out pointer precedence");
    uint32_t out = OUT_DEVICE;
    uint32_t outer[3] = { 0u, out, 0x1111u };
    if (call_row("DirectSoundCreate", outer, 3u) != TEST_CLASS_E_NOAGGREGATION)
        return fail("DirectSoundCreate aggregation precedence");
    uint32_t guid[3] = { 0x20001500u, out, 0u };
    if (call_row("DirectSoundCreate", guid, 3u) != DSERR_NODRIVER)
        return fail("DirectSoundCreate explicit GUID should fail");
    return 0;
}

int main(void)
{
    if (guest_map() != 0) return fail("guest memory map failed");
    memset(g_mem, 0, GUEST_SIZE);
    memset(&g_platform, 0, sizeof g_platform);
    g_platform.audio.open = fake_audio_open;
    g_platform.audio.close = fake_audio_close;
    g_platform.audio.primary_voice = fake_audio_primary_voice;
    g_platform.audio.voice_add = fake_audio_voice_add;
    g_platform.audio.voice_remove = fake_audio_voice_remove;
    g_platform.audio.voice_publish2 = fake_audio_voice_publish2;
    g_platform.audio.voice_position = fake_audio_voice_position;
    g_platform.audio.voice_control = fake_audio_voice_control;
    g_platform.audio.voice_seek = fake_audio_voice_seek;
    g_platform.audio.voice_published = fake_audio_voice_published;
    g_platform.audio.voice_state = fake_audio_voice_state;
    write_wfx(WFX);

    if (wc3_hle_dsound_install() != 0)
        return fail("DirectSound install failed");
    if (find_row("DirectSoundCreate") == NULL ||
        find_row("DirectSoundEnumerateA") == NULL)
        return fail("DirectSound exports were not registered");

    if (test_loader() != 0) return 1;
    if (test_enumerate() != 0) return 1;
    if (test_argument_precedence() != 0) return 1;
    if (test_create_without_winmm() != 0) return 1;

    printf("dsound runtime: PASS\n");
    return 0;
}
