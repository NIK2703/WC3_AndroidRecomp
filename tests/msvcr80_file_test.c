/*
 * msvcr80_file_test.c - T0 contract test for the MSVCR80 low-level file slice.
 *
 * The fake platform implements Wc3FileApi over one in-memory file.  The test
 * pins the as-built path translation, the Microsoft _open flag mapping, the
 * descriptor lifecycle, the 64-bit _lseeki64 return in EAX:EDX, and the
 * _stat64i32/_stat64/_fstat64 layouts used by Storm's directory check.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "file_hle.h"
#include "hle_bridge.h"
#include "platform.h"

#define MAX_ROWS 12u
#define PATH_ADDR 0x2000u
#define PATH_SIZE 128u
#define BUF_ADDR 0x2100u
#define BUF_SIZE 128u
#define STAT_ADDR 0x2200u
#define STAT_SIZE 64u
#define INVALID_ADDR 0x1000u

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
    int argc;
    int cdecl_abi;
} Row;

typedef struct FakeFile {
    int used;
    int flags;
    uint64_t position;
    uint64_t size;
    uint8_t data[256];
} FakeFile;

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;
static uint32_t g_args[4];
static uint8_t g_path[PATH_SIZE];
static uint8_t g_buffer[BUF_SIZE];
static uint8_t g_stat[STAT_SIZE];
static FakeFile g_file;
static char g_last_path[512];
static Wc3Platform g_platform;
static int g_failures;

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    if (!dll || !name || !impl || argc < 0) return -1;
    if (g_row_count >= MAX_ROWS) return -1;
    g_rows[g_row_count].dll = dll;
    g_rows[g_row_count].name = name;
    g_rows[g_row_count].impl = impl;
    g_rows[g_row_count].argc = argc;
    g_rows[g_row_count].cdecl_abi = cdecl_abi;
    g_row_count++;
    return 0;
}

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    if (addr >= PATH_ADDR && addr <= PATH_ADDR + PATH_SIZE)
        return size <= PATH_ADDR + PATH_SIZE - addr;
    if (addr >= BUF_ADDR && addr <= BUF_ADDR + BUF_SIZE)
        return size <= BUF_ADDR + BUF_SIZE - addr;
    if (addr >= STAT_ADDR && addr <= STAT_ADDR + STAT_SIZE)
        return size <= STAT_ADDR + STAT_SIZE - addr;
    return 0;
}

uint8_t wc3_legacy_rd8g(uint32_t addr)
{
    if (addr >= PATH_ADDR && addr < PATH_ADDR + PATH_SIZE)
        return g_path[addr - PATH_ADDR];
    if (addr >= BUF_ADDR && addr < BUF_ADDR + BUF_SIZE)
        return g_buffer[addr - BUF_ADDR];
    if (addr >= STAT_ADDR && addr < STAT_ADDR + STAT_SIZE)
        return g_stat[addr - STAT_ADDR];
    return 0u;
}

void wc3_legacy_wr8g(uint32_t addr, uint8_t value)
{
    if (addr >= PATH_ADDR && addr < PATH_ADDR + PATH_SIZE)
        g_path[addr - PATH_ADDR] = value;
    else if (addr >= BUF_ADDR && addr < BUF_ADDR + BUF_SIZE)
        g_buffer[addr - BUF_ADDR] = value;
    else if (addr >= STAT_ADDR && addr < STAT_ADDR + STAT_SIZE)
        g_stat[addr - STAT_ADDR] = value;
}

void wc3_legacy_wr16g(uint32_t addr, uint16_t value)
{
    wc3_legacy_wr8g(addr, (uint8_t)value);
    wc3_legacy_wr8g(addr + 1u, (uint8_t)(value >> 8));
}

void wc3_legacy_wr32g(uint32_t addr, uint32_t value)
{
    for (unsigned i = 0; i < 4u; i++)
        wc3_legacy_wr8g(addr + i, (uint8_t)(value >> (8u * i)));
}

uint32_t wc3_legacy_arg32(const CPU *c, unsigned index)
{
    (void)c;
    return index < sizeof g_args / sizeof g_args[0] ? g_args[index] : 0u;
}

const Wc3Platform *wc3_platform(void)
{
    return &g_platform;
}

static int fake_open(void *ctx, const char *path, int flags, uintptr_t *out)
{
    (void)ctx;
    if (!path || !out) return -1;
    snprintf(g_last_path, sizeof g_last_path, "%s", path);
    memset(&g_file, 0, sizeof g_file);
    g_file.used = 1;
    g_file.flags = flags;
    *out = (uintptr_t)(void *)&g_file;
    return 0;
}

static int fake_read(void *ctx, uintptr_t handle, void *buffer, uint32_t length,
                     uint32_t *read_out)
{
    (void)ctx;
    FakeFile *file = (FakeFile *)(uintptr_t)handle;
    if (file != &g_file || !file->used) return -1;
    const uint64_t available = file->size > file->position
                                   ? file->size - file->position
                                   : 0u;
    const uint32_t count =
        length < available ? length : (uint32_t)available;
    const uint32_t address = (uint32_t)(uintptr_t)buffer;
    for (uint32_t i = 0; i < count; i++)
        wc3_legacy_wr8g(address + i,
                        file->data[(uint32_t)file->position + i]);
    file->position += count;
    if (read_out) *read_out = count;
    return 0;
}

static int fake_write(void *ctx, uintptr_t handle, const void *buffer,
                      uint32_t length, uint32_t *written_out)
{
    (void)ctx;
    FakeFile *file = (FakeFile *)(uintptr_t)handle;
    if (file != &g_file || !file->used) return -1;
    const uint64_t room = file->position < sizeof file->data
                              ? (uint64_t)sizeof file->data - file->position
                              : 0u;
    const uint32_t count = length < room ? length : (uint32_t)room;
    const uint32_t address = (uint32_t)(uintptr_t)buffer;
    for (uint32_t i = 0; i < count; i++)
        file->data[(uint32_t)file->position + i] =
            wc3_legacy_rd8g(address + i);
    file->position += count;
    if (file->position > file->size) file->size = file->position;
    if (written_out) *written_out = count;
    return count == length ? 0 : -1;
}

static int fake_seek(void *ctx, uintptr_t handle, int64_t offset, int whence,
                     uint64_t *position_out)
{
    (void)ctx;
    FakeFile *file = (FakeFile *)(uintptr_t)handle;
    if (file != &g_file || !file->used) return -1;

    int64_t base;
    if (whence == 0)
        base = 0;
    else if (whence == 1)
        base = (int64_t)file->position;
    else if (whence == 2)
        base = (int64_t)file->size;
    else
        return -1;

    const int64_t next = base + offset;
    if (next < 0) return -1;
    file->position = (uint64_t)next;
    if (position_out) *position_out = file->position;
    return 0;
}

static int fake_close(void *ctx, uintptr_t handle)
{
    (void)ctx;
    FakeFile *file = (FakeFile *)(uintptr_t)handle;
    if (file != &g_file || !file->used) return -1;
    file->used = 0;
    return 0;
}

static int fake_size(void *ctx, uintptr_t handle, uint64_t *size_out)
{
    (void)ctx;
    FakeFile *file = (FakeFile *)(uintptr_t)handle;
    if (file != &g_file || !file->used || !size_out) return -1;
    *size_out = file->size;
    return 0;
}

static int fake_stat(void *ctx, const char *path, Wc3FileStat *out)
{
    (void)ctx;
    if (!path || !out) return -1;
    snprintf(g_last_path, sizeof g_last_path, "%s", path);
    if (strstr(path, "missing")) return -1;
    out->size = strstr(path, "big") ? 0x123456789ull : 1234u;
    out->mtime_seconds =
        strstr(path, "big") ? (int64_t)0x123456789ll : (int64_t)1600000000;
    out->mode = strstr(path, "dir") ? WC3_FILE_STAT_DIRECTORY
                                    : WC3_FILE_STAT_REGULAR;
    return 0;
}

static void check(int condition, const char *message)
{
    if (!condition) {
        printf("FAIL: %s\n", message);
        g_failures++;
    }
}

static void (*find_impl(const char *name))(CPU *c)
{
    for (unsigned i = 0; i < g_row_count; i++)
        if (strcmp(g_rows[i].name, name) == 0) return g_rows[i].impl;
    return NULL;
}

static uint32_t call1(void (*impl)(CPU *c), uint32_t a0)
{
    CPU c;
    memset(&c, 0, sizeof c);
    memset(g_args, 0, sizeof g_args);
    g_args[0] = a0;
    c.eax = 0xDEADBEEFu;
    c.edx = 0xDEADBEEFu;
    impl(&c);
    return c.eax;
}

static uint32_t call2(void (*impl)(CPU *c), uint32_t a0, uint32_t a1)
{
    CPU c;
    memset(&c, 0, sizeof c);
    memset(g_args, 0, sizeof g_args);
    g_args[0] = a0;
    g_args[1] = a1;
    c.eax = 0xDEADBEEFu;
    c.edx = 0xDEADBEEFu;
    impl(&c);
    return c.eax;
}

static uint32_t call3(void (*impl)(CPU *c), uint32_t a0, uint32_t a1,
                      uint32_t a2)
{
    CPU c;
    memset(&c, 0, sizeof c);
    memset(g_args, 0, sizeof g_args);
    g_args[0] = a0;
    g_args[1] = a1;
    g_args[2] = a2;
    c.eax = 0xDEADBEEFu;
    c.edx = 0xDEADBEEFu;
    impl(&c);
    return c.eax;
}

static void put_path(const char *text)
{
    memset(g_path, 0, sizeof g_path);
    const size_t length = strlen(text);
    const size_t copy = length < sizeof g_path ? length : sizeof g_path - 1u;
    memcpy(g_path, text, copy);
}

static void put_buffer(const char *text)
{
    memset(g_buffer, 0, sizeof g_buffer);
    const size_t length = strlen(text);
    const size_t copy = length < sizeof g_buffer ? length : sizeof g_buffer;
    memcpy(g_buffer, text, copy);
}

static int buffer_is(const char *text)
{
    return memcmp(g_buffer, text, strlen(text)) == 0;
}

static uint16_t stat_u16(unsigned offset)
{
    return (uint16_t)(g_stat[offset] | ((uint16_t)g_stat[offset + 1u] << 8));
}

static uint32_t stat_u32(unsigned offset)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 4u; i++)
        value |= (uint32_t)g_stat[offset + i] << (8u * i);
    return value;
}

static void reset_fake(void)
{
    memset(&g_file, 0, sizeof g_file);
    memset(g_last_path, 0, sizeof g_last_path);
    memset(&g_platform, 0, sizeof g_platform);
    g_platform.file.open = fake_open;
    g_platform.file.read = fake_read;
    g_platform.file.write = fake_write;
    g_platform.file.seek = fake_seek;
    g_platform.file.size = fake_size;
    g_platform.file.stat = fake_stat;
    g_platform.file.close = fake_close;
}

int main(void)
{
    reset_fake();
    const int installed = wc3_hle_msvcr80_file_install();
    check(installed == 9, "install count");
    check(g_row_count == 9u, "recorded row count");
    if (g_row_count != 9u) return 1;

    static const char *const names[] = {
        "_open", "_read", "_write", "_lseeki64", "_close",
        "_stat64i32", "_stat64", "_fstat64", "_fstat64i32"
    };
    static const int arities[] = { 3, 3, 3, 4, 1, 2, 2, 2, 2 };
    for (unsigned i = 0; i < 9u; i++) {
        check(strcmp(g_rows[i].dll, "MSVCR80.dll") == 0, "row dll");
        check(strcmp(g_rows[i].name, names[i]) == 0, "row name");
        check(g_rows[i].argc == arities[i], "row argc");
        check(g_rows[i].cdecl_abi == 1, "row cdecl");
    }

    void (*open_impl)(CPU *c) = find_impl("_open");
    void (*read_impl)(CPU *c) = find_impl("_read");
    void (*write_impl)(CPU *c) = find_impl("_write");
    void (*seek_impl)(CPU *c) = find_impl("_lseeki64");
    void (*close_impl)(CPU *c) = find_impl("_close");
    void (*stat32_impl)(CPU *c) = find_impl("_stat64i32");
    void (*stat64_impl)(CPU *c) = find_impl("_stat64");
    void (*fstat64_impl)(CPU *c) = find_impl("_fstat64");
    void (*fstat32_impl)(CPU *c) = find_impl("_fstat64i32");
    check(open_impl && read_impl && write_impl && seek_impl && close_impl &&
              stat32_impl && stat64_impl && fstat64_impl && fstat32_impl,
          "handlers found");
    if (!open_impl || !read_impl || !write_impl || !seek_impl || !close_impl ||
        !stat32_impl || !stat64_impl || !fstat64_impl || !fstat32_impl)
        return 1;

    /* A missing platform file API is an error, not a zero-returning stub. */
    g_platform.file.open = NULL;
    put_path("Z:\\data\\war3.mpq");
    check(call2(open_impl, PATH_ADDR, 0x8000u) == 0xFFFFFFFFu,
          "missing platform rejected");
    reset_fake();

    /* As-built path translation. */
    put_path("Z:\\data\\war3.mpq");
    check(call2(open_impl, PATH_ADDR, 0x8000u) == 3u,
          "Z: open returns fd 3");
    check(strcmp(g_last_path, "/data/war3.mpq") == 0, "Z: path mapping");
    check(g_file.flags == WC3_FILE_READ, "read-only flags");
    check(call1(close_impl, 3u) == 0u, "close fd 3");
    check(call1(close_impl, 3u) == 0xFFFFFFFFu, "double close fails");
    check(call3(read_impl, 3u, BUF_ADDR, 1u) == 0xFFFFFFFFu,
          "closed read fails");

    put_path("C:\\foo\\bar.txt");
    check(call2(open_impl, PATH_ADDR, 0x8000u) == 3u,
          "C: open returns fd 3");
    check(strcmp(g_last_path, "foo/bar.txt") == 0, "C: path mapping");
    check(call1(close_impl, 3u) == 0u, "close C: file");

    put_path("");
    check(call2(open_impl, PATH_ADDR, 0x8000u) == 3u,
          "empty path opens .");
    check(strcmp(g_last_path, ".") == 0, "empty path mapping");
    check(call1(close_impl, 3u) == 0u, "close empty path");

    put_path("a\\b");
    check(call2(open_impl, PATH_ADDR, 0x8000u) == 3u,
          "relative path opens");
    check(strcmp(g_last_path, "a/b") == 0, "relative path mapping");
    check(call1(close_impl, 3u) == 0u, "close relative path");

    check(call2(open_impl, INVALID_ADDR, 0x8000u) == 0xFFFFFFFFu,
          "invalid path rejected");
    check(call2(open_impl, PATH_ADDR, 0x8003u) == 0xFFFFFFFFu,
          "invalid access rejected");

    /* Write, seek and read the same descriptor. */
    put_path("C:\\save\\test.bin");
    check(call2(open_impl, PATH_ADDR, 0x8102u) == 3u,
          "rdwr create open returns fd 3");
    check(strcmp(g_last_path, "save/test.bin") == 0, "save path mapping");
    check(g_file.flags == (WC3_FILE_READ | WC3_FILE_WRITE | WC3_FILE_CREATE),
          "rdwr create flags");

    put_buffer("hello");
    check(call3(write_impl, 3u, BUF_ADDR, 5u) == 5u, "write count");
    check(memcmp(g_file.data, "hello", 5) == 0, "write bytes");

    CPU c;
    memset(&c, 0, sizeof c);
    memset(g_args, 0, sizeof g_args);
    g_args[0] = 3u;
    g_args[1] = 0u;
    g_args[2] = 0u;
    g_args[3] = 0u;
    seek_impl(&c);
    check(c.eax == 0u && c.edx == 0u, "seek to start");

    memset(g_buffer, 0, sizeof g_buffer);
    check(call3(read_impl, 3u, BUF_ADDR, 5u) == 5u, "read count");
    check(buffer_is("hello"), "read bytes");

    memset(&c, 0, sizeof c);
    memset(g_args, 0, sizeof g_args);
    g_args[0] = 3u;
    g_args[1] = 0x23456789u;
    g_args[2] = 1u;
    g_args[3] = 0u;
    seek_impl(&c);
    check(c.eax == 0x23456789u && c.edx == 1u,
          "64-bit seek return EAX:EDX");

    check(call3(read_impl, 3u, INVALID_ADDR, 1u) == 0xFFFFFFFFu,
          "invalid read buffer rejected");
    check(call3(write_impl, 3u, INVALID_ADDR, 1u) == 0xFFFFFFFFu,
          "invalid write buffer rejected");
    check(call3(read_impl, 3u, INVALID_ADDR, 0u) == 0u,
          "zero read count succeeds");
    check(call3(write_impl, 3u, INVALID_ADDR, 0u) == 0u,
          "zero write count succeeds");
    check(call1(close_impl, 3u) == 0u, "close rdwr file");

    /* Append is translated through Wc3FileApi. */
    put_path("a.log");
    check(call2(open_impl, PATH_ADDR, 0x810Au) == 3u,
          "append open returns fd 3");
    check((g_file.flags & WC3_FILE_APPEND) != 0, "append flag mapped");
    check(call1(close_impl, 3u) == 0u, "close append file");

    /* _stat64i32: directory mode at offset 6, size at 24. */
    put_path("Z:\\data\\dir");
    memset(g_stat, 0xFF, sizeof g_stat);
    check(call2(stat32_impl, PATH_ADDR, STAT_ADDR) == 0u, "stat64i32 ok");
    check(strcmp(g_last_path, "/data/dir") == 0, "stat path mapping");
    check((stat_u16(6) & 0x4000u) != 0, "stat directory mode");
    check(stat_u32(24) == 1234u && stat_u32(28) == 0u, "stat size");
    check(stat_u32(32) == 1600000000u, "stat time32");
    check(stat_u16(8) == 1u, "stat nlink");

    /* _stat64: 64-bit size and time fields. */
    put_path("C:\\big.bin");
    memset(g_stat, 0xFF, sizeof g_stat);
    check(call2(stat64_impl, PATH_ADDR, STAT_ADDR) == 0u, "stat64 ok");
    check((stat_u16(6) & 0x8000u) != 0, "stat regular mode");
    check(stat_u32(24) == 0x23456789u && stat_u32(28) == 1u, "stat64 size");
    check(stat_u32(32) == 0x23456789u && stat_u32(36) == 1u,
          "stat64 atime");
    check(stat_u32(40) == 0x23456789u && stat_u32(44) == 1u,
          "stat64 mtime");
    check(stat_u32(48) == 0x23456789u && stat_u32(52) == 1u,
          "stat64 ctime");

    put_path("missing");
    check(call2(stat32_impl, PATH_ADDR, STAT_ADDR) == 0xFFFFFFFFu,
          "stat missing fails");

    put_path("Z:\\data\\dir");
    check(call2(stat32_impl, PATH_ADDR, INVALID_ADDR) == 0xFFFFFFFFu,
          "stat invalid buffer rejected");

    g_platform.file.stat = NULL;
    check(call2(stat32_impl, PATH_ADDR, STAT_ADDR) == 0xFFFFFFFFu,
          "missing stat seam rejected");
    reset_fake();

    /* _fstat: size of an open descriptor, regular mode. */
    put_path("fstat.bin");
    check(call2(open_impl, PATH_ADDR, 0x8102u) == 3u, "fstat open");
    put_buffer("abc");
    check(call3(write_impl, 3u, BUF_ADDR, 3u) == 3u, "fstat write");
    memset(g_stat, 0xFF, sizeof g_stat);
    check(call2(fstat64_impl, 3u, STAT_ADDR) == 0u, "fstat64 ok");
    check((stat_u16(6) & 0x4000u) == 0u, "fstat regular mode");
    check(stat_u32(24) == 3u && stat_u32(28) == 0u, "fstat size");
    memset(g_stat, 0xFF, sizeof g_stat);
    check(call2(fstat32_impl, 3u, STAT_ADDR) == 0u, "fstat64i32 ok");
    check(stat_u32(32) == 0u, "fstat time zero");
    check(call1(close_impl, 3u) == 0u, "fstat close");
    check(call2(fstat64_impl, 3u, STAT_ADDR) == 0xFFFFFFFFu,
          "fstat closed fails");

    check(call1(close_impl, 0u) == 0xFFFFFFFFu, "stdin close rejected");

    if (g_failures) {
        printf("msvcr80 file: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("msvcr80 file OK (9 handlers)\n");
    return 0;
}
