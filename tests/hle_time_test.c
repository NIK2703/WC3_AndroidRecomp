/*
 * hle_time_test.c - T0 contract test for the first M5 target module.
 *
 * The test links time_hle.c against a fake Wc3TimeApi and a fake shim.  It
 * checks the Windows-visible values, not just "a handler ran": SYSTEMTIME is
 * verified in Windows field order (the as-built code wrote it in struct tm
 * order), FILETIME is checked at the 100 ns boundary, and the local-time path
 * is checked through the platform callback.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "platform.h"
#include "time_hle.h"

#define GUEST_BASE 0x10000u
#define GUEST_SIZE 0x10000u

static uint8_t g_mem[GUEST_SIZE];
static uint64_t g_mono_ns;
static uint64_t g_real_ns;

static void *guest_ptr(uint32_t addr, uint32_t size)
{
    if (addr < GUEST_BASE) return NULL;
    if ((uint64_t)addr + size > (uint64_t)GUEST_BASE + GUEST_SIZE) return NULL;
    return g_mem + (addr - GUEST_BASE);
}

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    return guest_ptr(addr, size) != NULL;
}

uint8_t wc3_legacy_rd8g(uint32_t addr)
{
    uint8_t v = 0;
    void *p = guest_ptr(addr, 1);
    if (p) memcpy(&v, p, 1);
    return v;
}

uint16_t wc3_legacy_rd16g(uint32_t addr)
{
    uint16_t v = 0;
    void *p = guest_ptr(addr, 2);
    if (p) memcpy(&v, p, 2);
    return v;
}

uint32_t wc3_legacy_rd32g(uint32_t addr)
{
    uint32_t v = 0;
    void *p = guest_ptr(addr, 4);
    if (p) memcpy(&v, p, 4);
    return v;
}

void wc3_legacy_wr8g(uint32_t addr, uint8_t value)
{
    void *p = guest_ptr(addr, 1);
    if (p) memcpy(p, &value, 1);
}

void wc3_legacy_wr16g(uint32_t addr, uint16_t value)
{
    void *p = guest_ptr(addr, 2);
    if (p) memcpy(p, &value, 2);
}

void wc3_legacy_wr32g(uint32_t addr, uint32_t value)
{
    void *p = guest_ptr(addr, 4);
    if (p) memcpy(p, &value, 4);
}

uint32_t wc3_legacy_arg32(const CPU *cpu, unsigned index)
{
    if (!cpu) return 0;
    return wc3_legacy_rd32g(cpu->esp + 4u + index * 4u);
}

#define MAX_OVERRIDES 32
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

static void (*find_override(const char *name))(CPU *)
{
    for (unsigned i = 0; i < g_override_count; i++)
        if (strcmp(g_overrides[i].name, name) == 0)
            return g_overrides[i].impl;
    return NULL;
}

/* Fixed local time: 2024-02-29 15:34:56.789, i.e. UTC+3. */
static int fake_local_time(void *ctx, uint64_t unix_ns, int32_t *year,
                           int32_t *month, int32_t *day, int32_t *hour,
                           int32_t *minute, int32_t *second,
                           int32_t *millisecond)
{
    (void)ctx;
    (void)unix_ns;
    *year = 2024;
    *month = 2;
    *day = 29;
    *hour = 15;
    *minute = 34;
    *second = 56;
    *millisecond = 789;
    return 0;
}

static uint64_t fake_monotonic_ns(void *ctx)
{
    (void)ctx;
    return g_mono_ns;
}

static uint64_t fake_realtime_ns(void *ctx)
{
    (void)ctx;
    return g_real_ns;
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

static void put_args(CPU *c, const uint32_t *args, unsigned count)
{
    const uint32_t stack = GUEST_BASE + 0x100u;
    memset(c, 0, sizeof *c);
    c->esp = stack;
    wc3_legacy_wr32g(stack, 0);
    for (unsigned i = 0; i < count; i++)
        wc3_legacy_wr32g(stack + 4u + i * 4u, args[i]);
}

static int call(const char *name, CPU *c)
{
    void (*fn)(CPU *) = find_override(name);
    if (!fn) return fail(name);
    fn(c);
    return 0;
}

static uint64_t read_u64(uint32_t addr)
{
    return (uint64_t)wc3_legacy_rd32g(addr) |
           ((uint64_t)wc3_legacy_rd32g(addr + 4) << 32);
}

#define CHECK(cond, what) do { if (!(cond)) return fail(what); } while (0)

int main(void)
{
    memset(&g_platform, 0, sizeof g_platform);
    g_platform.time.monotonic_ns = fake_monotonic_ns;
    g_platform.time.realtime_ns = fake_realtime_ns;
    g_platform.time.local_time = fake_local_time;

    const int installed = wc3_hle_kernel32_time_install();
    if (installed <= 0) return fail("install");

    CPU c;
    const uint32_t out = GUEST_BASE + 0x400u;
    uint32_t args[2];

    /* GetTickCount / timeGetTime: one monotonic source, milliseconds. */
    g_mono_ns = 1234567890123ull;
    put_args(&c, NULL, 0);
    if (call("GetTickCount", &c)) return 1;
    CHECK(c.eax == 1234567u, "GetTickCount value");
    put_args(&c, NULL, 0);
    if (call("timeGetTime", &c)) return 1;
    CHECK(c.eax == 1234567u, "timeGetTime value");

    /* QPC/QPF: nanoseconds and a 1 GHz frequency, as Miles expects. */
    args[0] = out;
    put_args(&c, args, 1);
    if (call("QueryPerformanceCounter", &c)) return 1;
    CHECK(c.eax == 1u, "QPC return");
    CHECK(read_u64(out) == g_mono_ns, "QPC value");
    put_args(&c, args, 1);
    if (call("QueryPerformanceFrequency", &c)) return 1;
    CHECK(c.eax == 1u, "QPF return");
    CHECK(read_u64(out) == 1000000000ull, "QPF value");

    /* 2024-02-29 12:34:56.789 UTC. */
    const uint64_t epoch_sec = 1709210096ull;
    g_real_ns = epoch_sec * 1000000000ull + 789000000ull;

    put_args(&c, args, 1);
    if (call("GetSystemTimeAsFileTime", &c)) return 1;
    CHECK(read_u64(out) == 116444736000000000ull + g_real_ns / 100ull,
          "GetSystemTimeAsFileTime value");

    put_args(&c, args, 1);
    if (call("GetSystemTime", &c)) return 1;
    CHECK(wc3_legacy_rd16g(out + 0) == 2024u, "SYSTEMTIME year");
    CHECK(wc3_legacy_rd16g(out + 2) == 2u, "SYSTEMTIME month");
    CHECK(wc3_legacy_rd16g(out + 4) == 4u, "SYSTEMTIME dayOfWeek");
    CHECK(wc3_legacy_rd16g(out + 6) == 29u, "SYSTEMTIME day");
    CHECK(wc3_legacy_rd16g(out + 8) == 12u, "SYSTEMTIME hour");
    CHECK(wc3_legacy_rd16g(out + 10) == 34u, "SYSTEMTIME minute");
    CHECK(wc3_legacy_rd16g(out + 12) == 56u, "SYSTEMTIME second");
    CHECK(wc3_legacy_rd16g(out + 14) == 789u, "SYSTEMTIME milliseconds");

    put_args(&c, args, 1);
    if (call("GetLocalTime", &c)) return 1;
    CHECK(wc3_legacy_rd16g(out + 8) == 15u, "local hour");
    CHECK(wc3_legacy_rd16g(out + 14) == 789u, "local milliseconds");

    /* SYSTEMTIME -> FILETIME -> SYSTEMTIME round trip. */
    const uint32_t st = GUEST_BASE + 0x800u;
    const uint32_t ft = GUEST_BASE + 0x900u;
    memset(guest_ptr(st, 16), 0, 16);
    wc3_legacy_wr16g(st + 0, 2024);
    wc3_legacy_wr16g(st + 2, 2);
    wc3_legacy_wr16g(st + 4, 4);
    wc3_legacy_wr16g(st + 6, 29);
    wc3_legacy_wr16g(st + 8, 12);
    wc3_legacy_wr16g(st + 10, 34);
    wc3_legacy_wr16g(st + 12, 56);
    wc3_legacy_wr16g(st + 14, 789);
    args[0] = st;
    args[1] = ft;
    put_args(&c, args, 2);
    if (call("SystemTimeToFileTime", &c)) return 1;
    CHECK(c.eax == 1u, "SystemTimeToFileTime return");
    const uint64_t expected_ft = 116444736000000000ull +
                                 epoch_sec * 10000000ull + 7890000ull;
    CHECK(read_u64(ft) == expected_ft, "SystemTimeToFileTime value");

    memset(guest_ptr(st, 16), 0, 16);
    args[0] = ft;
    args[1] = st;
    put_args(&c, args, 2);
    if (call("FileTimeToSystemTime", &c)) return 1;
    CHECK(c.eax == 1u, "FileTimeToSystemTime return");
    CHECK(wc3_legacy_rd16g(st + 6) == 29u, "FileTimeToSystemTime day");
    CHECK(wc3_legacy_rd16g(st + 8) == 12u, "FileTimeToSystemTime hour");
    CHECK(wc3_legacy_rd16g(st + 14) == 789u, "FileTimeToSystemTime ms");

    /* UTC+3 local conversion shifts by exactly three hours. */
    args[0] = ft;
    args[1] = out;
    put_args(&c, args, 2);
    if (call("FileTimeToLocalFileTime", &c)) return 1;
    CHECK(c.eax == 1u, "FileTimeToLocalFileTime return");
    CHECK(read_u64(out) == expected_ft + 3ull * 3600ull * 10000000ull,
          "FileTimeToLocalFileTime value");

    /* Current bias for UTC+3 is -180 minutes at offset 0. */
    args[0] = out;
    put_args(&c, args, 1);
    if (call("GetTimeZoneInformation", &c)) return 1;
    CHECK((int32_t)wc3_legacy_rd32g(out) == -180, "time zone bias");

    /* CompareFileTime keeps the Win32 sign convention. */
    const uint32_t ft2 = GUEST_BASE + 0xA00u;
    wc3_legacy_wr32g(ft, 10);
    wc3_legacy_wr32g(ft + 4, 0);
    wc3_legacy_wr32g(ft2, 20);
    wc3_legacy_wr32g(ft2 + 4, 0);
    args[0] = ft;
    args[1] = ft2;
    put_args(&c, args, 2);
    if (call("CompareFileTime", &c)) return 1;
    CHECK((int32_t)c.eax == -1, "CompareFileTime less");
    args[0] = ft2;
    args[1] = ft;
    put_args(&c, args, 2);
    if (call("CompareFileTime", &c)) return 1;
    CHECK((int32_t)c.eax == 1, "CompareFileTime greater");
    args[0] = ft;
    args[1] = ft;
    put_args(&c, args, 2);
    if (call("CompareFileTime", &c)) return 1;
    CHECK(c.eax == 0u, "CompareFileTime equal");

    printf("hle time OK (%d handlers)\n", installed);
    return 0;
}
