/*
 * msvcr80_time_test.c - T0 contract test for the MSVCR80 time slice.
 *
 * The test links crt_time_hle.c against a fake Wc3TimeApi and a fake shim.  It
 * checks the 64-bit return/write contract of _time64, the C-locale asctime
 * shape of _ctime64, the per-thread buffer identity, and the explicit failure
 * path when the platform table is incomplete.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "crt_time_hle.h"
#include "platform.h"

#define GUEST_BASE 0x10000u
#define GUEST_SIZE 0x10000u
#define MAX_OVERRIDES 8

static uint8_t g_mem[GUEST_SIZE];
static uint32_t g_next_alloc = GUEST_BASE + 0x8000u;
static int g_alloc_fail;
static uint64_t g_real_ns;
static uint64_t g_last_local_ns;
static int g_local_fail;
static int32_t g_year = 2024, g_month = 2, g_day = 29;
static int32_t g_hour = 15, g_minute = 34, g_second = 56;

typedef struct Override {
    char dll[32];
    char name[64];
    void (*impl)(CPU *c);
} Override;

static Override g_overrides[MAX_OVERRIDES];
static unsigned g_override_count;

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

uint32_t wc3_legacy_rd32g(uint32_t addr)
{
    uint32_t value = 0;
    void *p = guest_ptr(addr, 4);
    if (p) memcpy(&value, p, 4);
    return value;
}

void wc3_legacy_wr32g(uint32_t addr, uint32_t value)
{
    void *p = guest_ptr(addr, 4);
    if (p) memcpy(p, &value, 4);
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

uint16_t wc3_legacy_rd16g(uint32_t addr)
{
    uint16_t value = 0;
    void *p = guest_ptr(addr, 2);
    if (p) memcpy(&value, p, 2);
    return value;
}

uint32_t wc3_legacy_guest_alloc(uint32_t size)
{
    if (g_alloc_fail) return 0;
    size = (size + 7u) & ~7u;
    const uint32_t result = g_next_alloc;
    if ((uint64_t)result + size > (uint64_t)GUEST_BASE + GUEST_SIZE)
        return 0;
    g_next_alloc += size;
    memset(guest_ptr(result, size), 0, size);
    return result;
}

uint32_t wc3_legacy_arg32(const CPU *cpu, unsigned index)
{
    if (!cpu) return 0;
    return wc3_legacy_rd32g(cpu->esp + 4u + index * 4u);
}

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    if (!dll || !name || !impl || g_override_count >= MAX_OVERRIDES)
        return -1;
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
    if (!dll || !name || !impl || g_override_count >= MAX_OVERRIDES)
        return -1;
    snprintf(g_overrides[g_override_count].dll,
             sizeof g_overrides[g_override_count].dll, "%s", dll);
    snprintf(g_overrides[g_override_count].name,
             sizeof g_overrides[g_override_count].name, "%s", name);
    g_overrides[g_override_count].impl = impl;
    g_override_count++;
    return 0;
}

static void (*find_override(const char *name))(CPU *c)
{
    for (unsigned i = 0; i < g_override_count; i++)
        if (strcmp(g_overrides[i].name, name) == 0)
            return g_overrides[i].impl;
    return NULL;
}

static uint64_t fake_realtime_ns(void *ctx)
{
    (void)ctx;
    return g_real_ns;
}

static int fake_local_time(void *ctx, uint64_t unix_ns, int32_t *year,
                           int32_t *month, int32_t *day, int32_t *hour,
                           int32_t *minute, int32_t *second,
                           int32_t *millisecond)
{
    (void)ctx;
    if (g_local_fail) return -1;
    g_last_local_ns = unix_ns;
    *year = g_year;
    *month = g_month;
    *day = g_day;
    *hour = g_hour;
    *minute = g_minute;
    *second = g_second;
    *millisecond = 0;
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
    void (*impl)(CPU *c) = find_override(name);
    if (!impl) return fail(name);
    impl(c);
    return 0;
}

static uint64_t read_u64(uint32_t addr)
{
    return (uint64_t)wc3_legacy_rd32g(addr) |
           ((uint64_t)wc3_legacy_rd32g(addr + 4) << 32);
}

static void write_u64(uint32_t addr, uint64_t value)
{
    wc3_legacy_wr32g(addr, (uint32_t)value);
    wc3_legacy_wr32g(addr + 4, (uint32_t)(value >> 32));
}

static int32_t read_tm(uint32_t addr, unsigned index)
{
    return (int32_t)wc3_legacy_rd32g(addr + index * 4u);
}

static const char *guest_string(uint32_t addr)
{
    return (const char *)guest_ptr(addr, 64);
}

#define CHECK(condition, what) \
    do { if (!(condition)) return fail(what); } while (0)

int main(void)
{
    memset(&g_platform, 0, sizeof g_platform);

    /* An incomplete platform table is a startup error, not a fallback. */
    CHECK(wc3_hle_msvcr80_time_install() == -1, "install without time API");
    g_platform.time.realtime_ns = fake_realtime_ns;
    CHECK(wc3_hle_msvcr80_time_install() == -1, "install without local_time");
    g_platform.time.local_time = fake_local_time;
    CHECK(wc3_hle_msvcr80_time_install() == 6, "install count");
    CHECK(g_override_count == 6, "recorded row count");
    CHECK(strcmp(g_overrides[0].dll, "MSVCR80.dll") == 0, "DLL name");
    CHECK(find_override("_time64") != NULL, "_time64 row");
    CHECK(find_override("_ctime64") != NULL, "_ctime64 row");
    CHECK(find_override("_ftime64") != NULL, "_ftime64 row");
    CHECK(find_override("_gmtime64") != NULL, "_gmtime64 row");
    CHECK(find_override("_localtime64") != NULL, "_localtime64 row");
    CHECK(find_override("_localtime64_s") != NULL, "_localtime64_s row");

    /* 2024-02-29 12:34:56.789 UTC. */
    const uint64_t epoch_ns = 1709210096789000000ull;
    const uint64_t epoch_sec = 1709210096ull;
    g_real_ns = epoch_ns;

    CPU c;
    const uint32_t out = GUEST_BASE + 0x400u;
    const uint32_t timer = GUEST_BASE + 0x500u;
    uint32_t args[2];

    args[0] = out;
    put_args(&c, args, 1);
    if (call("_time64", &c)) return 1;
    CHECK(c.eax == (uint32_t)epoch_sec, "_time64 eax");
    CHECK(c.edx == (uint32_t)(epoch_sec >> 32), "_time64 edx");
    CHECK(read_u64(out) == epoch_sec, "_time64 output");

    /* A null output pointer must not disturb the return value. */
    g_real_ns = epoch_ns + 1000000000ull;
    args[0] = 0;
    put_args(&c, args, 1);
    if (call("_time64", &c)) return 1;
    CHECK(c.eax == (uint32_t)(epoch_sec + 1u), "_time64 null eax");
    CHECK(c.edx == 0, "_time64 null edx");

    /* __timeb64 is 16 bytes: 64-bit seconds, millitm, timezone minutes west
     * of UTC, dstflag, and two padding bytes. */
    const uint32_t ftime = GUEST_BASE + 0x700u;
    g_real_ns = epoch_ns;
    g_year = 2024;
    g_month = 2;
    g_day = 29;
    g_hour = 15;
    g_minute = 34;
    g_second = 56;
    args[0] = ftime;
    put_args(&c, args, 1);
    if (call("_ftime64", &c)) return 1;
    CHECK(c.eax == 0, "_ftime64 return");
    CHECK(read_u64(ftime) == epoch_sec, "_ftime64 seconds");
    CHECK(wc3_legacy_rd16g(ftime + 8u) == 789u, "_ftime64 millitm");
    CHECK((int16_t)wc3_legacy_rd16g(ftime + 10u) == -180,
          "_ftime64 timezone");
    CHECK(wc3_legacy_rd16g(ftime + 12u) == 0u, "_ftime64 dstflag");
    CHECK(wc3_legacy_rd16g(ftime + 14u) == 0u, "_ftime64 padding");

    args[0] = 0xFFFFFFFFu;
    put_args(&c, args, 1);
    if (call("_ftime64", &c)) return 1;
    CHECK(c.eax == 0, "_ftime64 bad pointer");

    write_u64(timer, epoch_sec);
    g_year = 2024;
    g_month = 2;
    g_day = 29;
    g_hour = 15;
    g_minute = 34;
    g_second = 56;
    args[0] = timer;
    put_args(&c, args, 1);
    if (call("_ctime64", &c)) return 1;
    CHECK(c.eax != 0, "_ctime64 return");
    CHECK(strcmp(guest_string(c.eax), "Thu Feb 29 15:34:56 2024\n") == 0,
          "_ctime64 text");
    CHECK(g_last_local_ns == epoch_sec * 1000000000ull,
          "_ctime64 local input");
    const uint32_t first_buffer = c.eax;

    /* The CRT buffer is per thread and reused, not leaked per call. */
    put_args(&c, args, 1);
    if (call("_ctime64", &c)) return 1;
    CHECK(c.eax == first_buffer, "_ctime64 buffer reuse");

    /* One-digit day is space padded exactly as asctime does. */
    g_year = 2024;
    g_month = 1;
    g_day = 9;
    g_hour = 1;
    g_minute = 2;
    g_second = 3;
    args[0] = 0;
    put_args(&c, args, 1);
    if (call("_ctime64", &c)) return 1;
    CHECK(strcmp(guest_string(c.eax), "Tue Jan  9 01:02:03 2024\n") == 0,
          "_ctime64 day padding");

    /* An invalid timer pointer means "use the current clock", as in the
     * as-built handler; the platform callback receives that current value. */
    g_year = 2024;
    g_month = 2;
    g_day = 29;
    g_hour = 12;
    g_minute = 34;
    g_second = 56;
    g_real_ns = epoch_sec * 1000000000ull;
    args[0] = 0xFFFFFFFFu;
    put_args(&c, args, 1);
    if (call("_ctime64", &c)) return 1;
    CHECK(g_last_local_ns == g_real_ns, "_ctime64 invalid timer fallback");

    g_local_fail = 1;
    put_args(&c, args, 1);
    if (call("_ctime64", &c)) return 1;
    CHECK(c.eax == 0, "_ctime64 local failure");
    g_local_fail = 0;

    /* _gmtime64 converts UTC without consulting the platform calendar. */
    write_u64(timer, epoch_sec);
    args[0] = timer;
    put_args(&c, args, 1);
    if (call("_gmtime64", &c)) return 1;
    CHECK(c.eax != 0, "_gmtime64 return");
    const uint32_t gm = c.eax;
    CHECK(read_tm(gm, 0) == 56, "_gmtime64 sec");
    CHECK(read_tm(gm, 1) == 34, "_gmtime64 min");
    CHECK(read_tm(gm, 2) == 12, "_gmtime64 hour");
    CHECK(read_tm(gm, 3) == 29, "_gmtime64 mday");
    CHECK(read_tm(gm, 4) == 1, "_gmtime64 mon");
    CHECK(read_tm(gm, 5) == 124, "_gmtime64 year");
    CHECK(read_tm(gm, 6) == 4, "_gmtime64 wday");
    CHECK(read_tm(gm, 7) == 59, "_gmtime64 yday");
    CHECK(read_tm(gm, 8) == 0, "_gmtime64 isdst");

    /* The CRT result is a per-thread object, reused on the next call. */
    put_args(&c, args, 1);
    if (call("_gmtime64", &c)) return 1;
    CHECK(c.eax == gm, "_gmtime64 buffer reuse");

    args[0] = 0;
    put_args(&c, args, 1);
    if (call("_gmtime64", &c)) return 1;
    CHECK(c.eax == 0, "_gmtime64 null");
    args[0] = 0xFFFFFFFFu;
    put_args(&c, args, 1);
    if (call("_gmtime64", &c)) return 1;
    CHECK(c.eax == 0, "_gmtime64 bad pointer");

    /* _localtime64 uses the platform's local calendar fields. */
    g_hour = 15;
    g_minute = 34;
    g_second = 56;
    args[0] = timer;
    put_args(&c, args, 1);
    if (call("_localtime64", &c)) return 1;
    CHECK(c.eax != 0, "_localtime64 return");
    const uint32_t local = c.eax;
    CHECK(read_tm(local, 0) == 56, "_localtime64 sec");
    CHECK(read_tm(local, 1) == 34, "_localtime64 min");
    CHECK(read_tm(local, 2) == 15, "_localtime64 hour");
    CHECK(read_tm(local, 3) == 29, "_localtime64 mday");
    CHECK(read_tm(local, 4) == 1, "_localtime64 mon");
    CHECK(read_tm(local, 5) == 124, "_localtime64 year");
    CHECK(read_tm(local, 6) == 4, "_localtime64 wday");
    CHECK(read_tm(local, 7) == 59, "_localtime64 yday");
    CHECK(read_tm(local, 8) == 0, "_localtime64 isdst");

    /* _localtime64_s writes the caller's buffer and returns EINVAL on a bad
     * destination or source pointer. */
    const uint32_t tm_out = GUEST_BASE + 0x600u;
    args[0] = tm_out;
    args[1] = timer;
    put_args(&c, args, 2);
    if (call("_localtime64_s", &c)) return 1;
    CHECK(c.eax == 0, "_localtime64_s return");
    CHECK(read_tm(tm_out, 2) == 15, "_localtime64_s hour");
    CHECK(read_tm(tm_out, 5) == 124, "_localtime64_s year");

    args[0] = 0;
    args[1] = timer;
    put_args(&c, args, 2);
    if (call("_localtime64_s", &c)) return 1;
    CHECK(c.eax == 22u, "_localtime64_s null destination");

    args[0] = tm_out;
    args[1] = 0;
    put_args(&c, args, 2);
    if (call("_localtime64_s", &c)) return 1;
    CHECK(c.eax == 22u, "_localtime64_s null source");

    printf("msvcr80 time OK (6 handlers)\n");
    return 0;
}
