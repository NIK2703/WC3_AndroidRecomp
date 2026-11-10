/*
 * msvcr80_stdio_test.c - T0 contract test for the MSVCR80 stdio slice.
 *
 * The fake platform keeps one byte-addressed file behind Wc3FileApi.  The test
 * pins the guest record shape, the shared descriptor contract with the
 * low-level file slice, the fopen mode mapping, stream positioning/EOF state,
 * the six newly registered rows, and the wide _wfopen path conversion.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "file_hle.h"
#include "hle_bridge.h"
#include "platform.h"
#include "stdio_hle.h"

#define MAX_OVERRIDES 24u
#define MAX_ADDITIONS 24u
#define GUEST_SIZE 0x8000u
#define PATH_ADDR 0x2000u
#define MODE_ADDR 0x2200u
#define BUF_ADDR 0x2400u
#define STRING_ADDR 0x2600u
#define OUT_ADDR 0x2800u
#define STACK_ADDR 0x2C00u
#define WIDE_PATH_ADDR 0x3000u
#define WIDE_MODE_ADDR 0x3400u
#define HEAP_ADDR 0x4000u
#define INVALID_ADDR 0x0800u

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
    int argc;
    int cdecl_abi;
} Row;

typedef struct FakeFile {
    int exists;
    int used;
    int flags;
    uint64_t position;
    uint64_t size;
    uint8_t data[512];
} FakeFile;

static Row g_overrides[MAX_OVERRIDES];
static unsigned g_override_count;
static Row g_additions[MAX_ADDITIONS];
static unsigned g_addition_count;
static uint32_t g_args[4];
static uint8_t g_guest[GUEST_SIZE];
static uint32_t g_heap_at;
static FakeFile g_file;
static char g_last_path[512];
static Wc3Platform g_platform;
static int g_failures;

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    if (!dll || !name || !impl) return -1;
    if (g_override_count >= MAX_OVERRIDES) return -1;
    g_overrides[g_override_count].dll = dll;
    g_overrides[g_override_count].name = name;
    g_overrides[g_override_count].impl = impl;
    g_overrides[g_override_count].argc = 0;
    g_overrides[g_override_count].cdecl_abi = 1;
    g_override_count++;
    return 0;
}

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    if (!dll || !name || !impl || argc < 0) return -1;
    if (g_addition_count >= MAX_ADDITIONS) return -1;
    g_additions[g_addition_count].dll = dll;
    g_additions[g_addition_count].name = name;
    g_additions[g_addition_count].impl = impl;
    g_additions[g_addition_count].argc = argc;
    g_additions[g_addition_count].cdecl_abi = cdecl_abi;
    g_addition_count++;
    return 0;
}

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    if (addr < 0x1000u || addr > GUEST_SIZE) return 0;
    return size <= GUEST_SIZE - addr;
}

uint8_t wc3_legacy_rd8g(uint32_t addr)
{
    return addr < GUEST_SIZE ? g_guest[addr] : 0u;
}

uint16_t wc3_legacy_rd16g(uint32_t addr)
{
    if (addr + 1u >= GUEST_SIZE) return 0u;
    return (uint16_t)(g_guest[addr] | ((uint16_t)g_guest[addr + 1u] << 8));
}

uint32_t wc3_legacy_rd32g(uint32_t addr)
{
    if (addr + 3u >= GUEST_SIZE) return 0u;
    return (uint32_t)g_guest[addr] |
           ((uint32_t)g_guest[addr + 1u] << 8) |
           ((uint32_t)g_guest[addr + 2u] << 16) |
           ((uint32_t)g_guest[addr + 3u] << 24);
}

void wc3_legacy_wr8g(uint32_t addr, uint8_t value)
{
    if (addr < GUEST_SIZE) g_guest[addr] = value;
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

uint32_t wc3_legacy_guest_alloc0(uint32_t size)
{
    size = (size + 7u) & ~7u;
    if (!size || g_heap_at + size > GUEST_SIZE) return 0u;
    const uint32_t result = g_heap_at;
    memset(g_guest + result, 0, size);
    g_heap_at += size;
    return result;
}

void wc3_legacy_guest_free(uint32_t addr)
{
    (void)addr;
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

    const int read = (flags & WC3_FILE_READ) != 0;
    const int write = (flags & WC3_FILE_WRITE) != 0;
    const int create = (flags & WC3_FILE_CREATE) != 0;
    const int truncate = (flags & WC3_FILE_TRUNCATE) != 0;
    const int append = (flags & WC3_FILE_APPEND) != 0;
    if (!read && !write) return -1;
    if (!g_file.exists && !create) return -1;
    if (!g_file.exists && !write) return -1;
    if ((flags & WC3_FILE_EXCLUSIVE) && g_file.exists) return -1;

    g_file.exists = 1;
    g_file.used = 1;
    g_file.flags = flags;
    if (truncate) g_file.size = 0u;
    g_file.position = append ? g_file.size : 0u;
    *out = (uintptr_t)(void *)&g_file;
    return 0;
}

static int fake_read(void *ctx, uintptr_t handle, void *buffer, uint32_t length,
                     uint32_t *read_out)
{
    (void)ctx;
    FakeFile *file = (FakeFile *)(uintptr_t)handle;
    if (file != &g_file || !file->used || (!buffer && length)) return -1;
    const uint64_t available =
        file->size > file->position ? file->size - file->position : 0u;
    const uint32_t count =
        length < available ? length : (uint32_t)available;
    const uintptr_t address = (uintptr_t)buffer;
    if (address < GUEST_SIZE) {
        for (uint32_t i = 0; i < count; i++)
            wc3_legacy_wr8g((uint32_t)address + i,
                            file->data[file->position + i]);
    } else {
        memcpy(buffer, file->data + file->position, count);
    }
    file->position += count;
    if (read_out) *read_out = count;
    return 0;
}

static int fake_write(void *ctx, uintptr_t handle, const void *buffer,
                      uint32_t length, uint32_t *written_out)
{
    (void)ctx;
    FakeFile *file = (FakeFile *)(uintptr_t)handle;
    if (file != &g_file || !file->used || (!buffer && length)) return -1;
    if (file->position + length > sizeof file->data) return -1;
    const uintptr_t address = (uintptr_t)buffer;
    if (address < GUEST_SIZE) {
        for (uint32_t i = 0; i < length; i++)
            file->data[file->position + i] =
                wc3_legacy_rd8g((uint32_t)address + i);
    } else {
        memcpy(file->data + file->position, buffer, length);
    }
    file->position += length;
    if (file->position > file->size) file->size = file->position;
    if (written_out) *written_out = length;
    return 0;
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
    if (next < 0 || (uint64_t)next > sizeof file->data) return -1;
    file->position = (uint64_t)next;
    if (position_out) *position_out = file->position;
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
    if (!g_file.exists) return -1;
    out->size = g_file.size;
    out->mtime_seconds = 0;
    out->mode = WC3_FILE_STAT_REGULAR;
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

static int fake_from_wide(void *ctx, uint32_t codepage, const uint16_t *src,
                          int32_t src_len, char *dst, int32_t dst_len)
{
    (void)ctx;
    (void)codepage;
    if (!src || !dst || dst_len <= 0) return -1;
    int32_t len = src_len < 0 ? 0 : src_len;
    if (src_len < 0)
        while (src[len]) len++;
    if (len >= dst_len) return -1;
    for (int32_t i = 0; i < len; i++) {
        if (src[i] > 0x7Fu) return -1;
        dst[i] = (char)src[i];
    }
    dst[len] = '\0';
    return len;
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
    for (unsigned i = 0; i < g_override_count; i++)
        if (strcmp(g_overrides[i].name, name) == 0) return g_overrides[i].impl;
    for (unsigned i = 0; i < g_addition_count; i++)
        if (strcmp(g_additions[i].name, name) == 0) return g_additions[i].impl;
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

static uint32_t call4(void (*impl)(CPU *c), uint32_t a0, uint32_t a1,
                      uint32_t a2, uint32_t a3)
{
    CPU c;
    memset(&c, 0, sizeof c);
    memset(g_args, 0, sizeof g_args);
    g_args[0] = a0;
    g_args[1] = a1;
    g_args[2] = a2;
    g_args[3] = a3;
    c.eax = 0xDEADBEEFu;
    c.edx = 0xDEADBEEFu;
    impl(&c);
    return c.eax;
}

static uint32_t call_printf(void (*impl)(CPU *c), uint32_t format,
                            uint32_t argument)
{
    CPU c;
    memset(&c, 0, sizeof c);
    memset(g_args, 0, sizeof g_args);
    g_args[0] = format;
    c.esp = STACK_ADDR;
    c.eax = 0xDEADBEEFu;
    c.edx = 0xDEADBEEFu;
    wc3_legacy_wr32g(STACK_ADDR, 0xDEADBEEFu);
    wc3_legacy_wr32g(STACK_ADDR + 8u, argument);
    impl(&c);
    return c.eax;
}

static void put_at(uint32_t address, const char *text)
{
    const size_t length = strlen(text);
    memset(g_guest + address, 0, length + 1u);
    memcpy(g_guest + address, text, length);
}

static void put_wide(uint32_t address, const char *text)
{
    const size_t length = strlen(text);
    for (size_t i = 0; i < length; i++)
        wc3_legacy_wr16g(address + (uint32_t)(i * 2u),
                         (uint16_t)(unsigned char)text[i]);
    wc3_legacy_wr16g(address + (uint32_t)(length * 2u), 0u);
}

static void put_buffer(const void *data, size_t length)
{
    memset(g_guest + BUF_ADDR, 0, 256u);
    memcpy(g_guest + BUF_ADDR, data, length);
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
    g_platform.locale.from_wide = fake_from_wide;
    g_heap_at = HEAP_ADDR;
    memset(g_guest, 0, sizeof g_guest);
}

static uint32_t record_slot(uint32_t record)
{
    return wc3_legacy_rd32g(record + 4u);
}

int main(void)
{
    reset_fake();
    const int file_installed = wc3_hle_msvcr80_file_install();
    const int stdio_installed = wc3_hle_msvcr80_stdio_install();
    check(file_installed == 9, "file install count");
    check(stdio_installed == 19, "stdio install count");
    check(g_override_count == 10u, "override count");
    check(g_addition_count == 18u, "addition count");

    static const char *const override_names[] = {
        "fopen", "fopen_s", "fclose", "fread", "fwrite",
        "fseek", "ftell", "fflush", "feof", "ferror"
    };
    for (unsigned i = 0; i < 10u; i++) {
        check(strcmp(g_overrides[i].dll, "MSVCR80.dll") == 0,
              "override dll");
        check(strcmp(g_overrides[i].name, override_names[i]) == 0,
              "override name");
    }

    void (*fopen_impl)(CPU *c) = find_impl("fopen");
    void (*fopen_s_impl)(CPU *c) = find_impl("fopen_s");
    void (*fclose_impl)(CPU *c) = find_impl("fclose");
    void (*fread_impl)(CPU *c) = find_impl("fread");
    void (*fwrite_impl)(CPU *c) = find_impl("fwrite");
    void (*fseek_impl)(CPU *c) = find_impl("fseek");
    void (*ftell_impl)(CPU *c) = find_impl("ftell");
    void (*fflush_impl)(CPU *c) = find_impl("fflush");
    void (*feof_impl)(CPU *c) = find_impl("feof");
    void (*ferror_impl)(CPU *c) = find_impl("ferror");
    void (*putc_impl)(CPU *c) = find_impl("putc");
    void (*fputc_impl)(CPU *c) = find_impl("fputc");
    void (*fputs_impl)(CPU *c) = find_impl("fputs");
    void (*fgets_impl)(CPU *c) = find_impl("fgets");
    void (*fileno_impl)(CPU *c) = find_impl("_fileno");
    void (*wfopen_impl)(CPU *c) = find_impl("_wfopen");
    void (*setmode_impl)(CPU *c) = find_impl("_setmode");
    void (*fdopen_impl)(CPU *c) = find_impl("_fdopen");
    void (*printf_impl)(CPU *c) = find_impl("printf");
    void (*open_impl)(CPU *c) = find_impl("_open");
    void (*read_impl)(CPU *c) = find_impl("_read");
    void (*seek_impl)(CPU *c) = find_impl("_lseeki64");
    check(fopen_impl && fopen_s_impl && fclose_impl && fread_impl &&
              fwrite_impl && fseek_impl && ftell_impl && fflush_impl &&
              feof_impl && ferror_impl && putc_impl && fputc_impl &&
              fputs_impl && fgets_impl && fileno_impl && wfopen_impl &&
              setmode_impl && fdopen_impl && printf_impl && open_impl &&
              read_impl && seek_impl,
          "handlers found");
    if (!fopen_impl || !fopen_s_impl || !fclose_impl || !fread_impl ||
        !fwrite_impl || !fseek_impl || !ftell_impl || !fflush_impl ||
        !feof_impl || !ferror_impl || !putc_impl || !fputc_impl ||
        !fputs_impl || !fgets_impl || !fileno_impl || !wfopen_impl ||
        !setmode_impl || !fdopen_impl || !printf_impl || !open_impl ||
        !read_impl || !seek_impl)
        return 1;

    /* fopen maps the as-built Z:/C: rules and creates a guest record. */
    put_at(PATH_ADDR, "Z:\\save\\test.bin");
    put_at(MODE_ADDR, "w+b");
    const uint32_t record = call2(fopen_impl, PATH_ADDR, MODE_ADDR);
    check(record != 0u, "fopen returns record");
    check(strcmp(g_last_path, "/save/test.bin") == 0, "fopen Z: path");
    check((g_file.flags & (WC3_FILE_READ | WC3_FILE_WRITE |
                           WC3_FILE_CREATE | WC3_FILE_TRUNCATE)) ==
              (WC3_FILE_READ | WC3_FILE_WRITE | WC3_FILE_CREATE |
               WC3_FILE_TRUNCATE),
          "fopen w+b flags");
    check(record_slot(record) >= 3u, "fopen file slot");
    check(wc3_legacy_rd32g(record) == 0x47465354u, "fopen record magic");

    check(call1(fileno_impl, record) == 3u, "fileno file descriptor");
    check(call2(setmode_impl, 3u, 0x4000u) == 0x8000u,
          "setmode text returns binary");
    check(call2(setmode_impl, 3u, 0x8000u) == 0x4000u,
          "setmode binary returns text");
    check(call2(setmode_impl, 3u, 0x1234u) == 0xFFFFFFFFu,
          "setmode invalid mode");
    check(call2(setmode_impl, 99u, 0x8000u) == 0xFFFFFFFFu,
          "setmode invalid fd");

    put_buffer("hello", 5u);
    check(call4(fwrite_impl, BUF_ADDR, 1u, 5u, record) == 5u,
          "fwrite items");
    check(memcmp(g_file.data, "hello", 5u) == 0, "fwrite bytes");
    check(call1(ftell_impl, record) == 5u, "ftell after write");
    check(call3(fseek_impl, record, 0u, 0u) == 0u, "fseek start");
    memset(g_guest + BUF_ADDR, 0, 16u);
    check(call4(fread_impl, BUF_ADDR, 1u, 5u, record) == 5u,
          "fread items");
    check(memcmp(g_guest + BUF_ADDR, "hello", 5u) == 0, "fread bytes");
    check(call1(feof_impl, record) == 0u, "feof clear after exact read");
    check(call4(fread_impl, BUF_ADDR, 1u, 1u, record) == 0u,
          "fread at eof");
    check(call1(feof_impl, record) == 1u, "feof set");
    check(call1(ferror_impl, record) == 0u, "ferror clear");
    check(call3(fseek_impl, record, 0u, 0u) == 0u, "fseek clears eof");
    check(call1(feof_impl, record) == 0u, "feof cleared");
    check(call3(fseek_impl, record, 0u, 2u) == 0u, "seek to end");

    check(call2(putc_impl, 'A', record) == (uint32_t)'A', "putc");
    check(call2(fputc_impl, 'B', record) == (uint32_t)'B', "fputc");
    put_at(STRING_ADDR, "CD");
    check(call2(fputs_impl, STRING_ADDR, record) == 0u, "fputs");
    check(memcmp(g_file.data, "helloABCD", 9u) == 0, "putc/fputs bytes");

    check(call3(fseek_impl, record, 0u, 0u) == 0u, "fgets seek");
    memset(g_guest + STRING_ADDR, 0, 32u);
    check(call3(fgets_impl, STRING_ADDR, 32u, record) == STRING_ADDR,
          "fgets returns buffer");
    check(strcmp((const char *)g_guest + STRING_ADDR, "helloABCD") == 0,
          "fgets reads to eof");
    check(call1(feof_impl, record) == 1u, "fgets eof");
    check(call3(fgets_impl, STRING_ADDR, 32u, record) == 0u,
          "fgets at eof returns null");

    check(call1(fflush_impl, record) == 0u, "fflush stream");
    check(call1(fflush_impl, 0u) == 0u, "fflush all");

    /* The stdio descriptor is the same descriptor the low-level slice uses. */
    check(call4(seek_impl, 3u, 0u, 0u, 0u) == 0u, "shared seek");
    memset(g_guest + BUF_ADDR, 0, 16u);
    check(call3(read_impl, 3u, BUF_ADDR, 5u) == 5u, "shared read");
    check(memcmp(g_guest + BUF_ADDR, "hello", 5u) == 0, "shared bytes");

    check(call1(fclose_impl, record) == 0u, "fclose");
    check(call1(fclose_impl, record) == 0xFFFFFFFFu, "double fclose");
    check(wc3_legacy_rd32g(record) == 0u, "fclose clears magic");
    check(call1(fileno_impl, record) == 0xFFFFFFFFu, "fileno closed");

    /* _fdopen associates a stream with an existing low-level descriptor. */
    put_at(PATH_ADDR, "Z:\\save\\fdopen.bin");
    check(call2(open_impl, PATH_ADDR, 0x8000u) == 3u, "fdopen open");
    put_at(MODE_ADDR, "rb");
    const uint32_t fd_record = call2(fdopen_impl, 3u, MODE_ADDR);
    check(fd_record != 0u, "_fdopen record");
    check(call1(fileno_impl, fd_record) == 3u, "_fdopen fileno");
    memset(g_guest + BUF_ADDR, 0, 16u);
    check(call4(fread_impl, BUF_ADDR, 1u, 5u, fd_record) == 5u,
          "_fdopen fread");
    check(memcmp(g_guest + BUF_ADDR, "hello", 5u) == 0, "_fdopen bytes");
    check(call1(fclose_impl, fd_record) == 0u, "_fdopen close");
    check(call3(read_impl, 3u, BUF_ADDR, 1u) == 0xFFFFFFFFu,
          "_fdopen close closes fd");

    /* fopen_s uses the out pointer and reports ENOENT as 2. */
    put_at(PATH_ADDR, "C:\\save\\secure.txt");
    put_at(MODE_ADDR, "wb");
    wc3_legacy_wr32g(OUT_ADDR, 0xDEADBEEFu);
    check(call3(fopen_s_impl, OUT_ADDR, PATH_ADDR, MODE_ADDR) == 0u,
          "fopen_s success");
    const uint32_t secure = wc3_legacy_rd32g(OUT_ADDR);
    check(secure != 0u, "fopen_s out record");
    check(strcmp(g_last_path, "save/secure.txt") == 0, "fopen_s C: path");
    check(call1(fclose_impl, secure) == 0u, "fopen_s close");

    g_file.exists = 0;
    put_at(PATH_ADDR, "missing.bin");
    put_at(MODE_ADDR, "rb");
    wc3_legacy_wr32g(OUT_ADDR, 0xDEADBEEFu);
    check(call3(fopen_s_impl, OUT_ADDR, PATH_ADDR, MODE_ADDR) == 2u,
          "fopen_s missing");
    check(wc3_legacy_rd32g(OUT_ADDR) == 0u, "fopen_s missing out");

    /* Invalid mode/path/stream handling. */
    put_at(PATH_ADDR, "C:\\bad");
    put_at(MODE_ADDR, "q");
    check(call2(fopen_impl, PATH_ADDR, MODE_ADDR) == 0u, "invalid mode");
    check(call2(fopen_impl, INVALID_ADDR, MODE_ADDR) == 0u, "invalid path");
    check(call2(putc_impl, 'A', 0u) == 0xFFFFFFFFu, "putc invalid stream");
    check(call2(fputs_impl, STRING_ADDR, 0u) == 0xFFFFFFFFu,
          "fputs invalid stream");

    /* Line-oriented fgets and n == 1. */
    put_at(PATH_ADDR, "C:\\lines.txt");
    put_at(MODE_ADDR, "w+b");
    const uint32_t lines = call2(fopen_impl, PATH_ADDR, MODE_ADDR);
    check(lines != 0u, "lines fopen");
    put_buffer("one\ntwo\n", 8u);
    check(call4(fwrite_impl, BUF_ADDR, 1u, 8u, lines) == 8u,
          "lines write");
    check(call3(fseek_impl, lines, 0u, 0u) == 0u, "lines seek");
    memset(g_guest + STRING_ADDR, 0, 32u);
    check(call3(fgets_impl, STRING_ADDR, 5u, lines) == STRING_ADDR,
          "fgets line one");
    check(strcmp((const char *)g_guest + STRING_ADDR, "one\n") == 0,
          "fgets line one text");
    memset(g_guest + STRING_ADDR, 0, 32u);
    check(call3(fgets_impl, STRING_ADDR, 8u, lines) == STRING_ADDR,
          "fgets line two");
    check(strcmp((const char *)g_guest + STRING_ADDR, "two\n") == 0,
          "fgets line two text");
    check(call3(fgets_impl, STRING_ADDR, 8u, lines) == 0u,
          "fgets line eof");
    wc3_legacy_wr8g(STRING_ADDR, 0xFFu);
    check(call3(fgets_impl, STRING_ADDR, 1u, lines) == STRING_ADDR,
          "fgets n=1 returns buffer");
    check(wc3_legacy_rd8g(STRING_ADDR) == 0u, "fgets n=1 terminates");
    check(call1(fclose_impl, lines) == 0u, "lines close");

    /* _wfopen converts the UTF-16 path and mode through Wc3LocaleApi. */
    put_wide(WIDE_PATH_ADDR, "Z:\\save\\wide.txt");
    put_wide(WIDE_MODE_ADDR, "wb");
    const uint32_t wide = call2(wfopen_impl, WIDE_PATH_ADDR, WIDE_MODE_ADDR);
    check(wide != 0u, "_wfopen record");
    check(strcmp(g_last_path, "/save/wide.txt") == 0, "_wfopen path");
    check(call1(fclose_impl, wide) == 0u, "_wfopen close");

    /* Standard stream records from __iob_func map to fd 0..2. */
    wc3_legacy_wr32g(OUT_ADDR, 0x47465354u);
    wc3_legacy_wr32g(OUT_ADDR + 4u, 1u);
    check(call1(fileno_impl, OUT_ADDR) == 1u, "stdout fileno");
    check(call1(feof_impl, OUT_ADDR) == 0u, "stdout feof");
    check(call1(ferror_impl, OUT_ADDR) == 0u, "stdout ferror");

    /* printf decodes the guest cdecl vararg block and writes to stdout. */
    put_at(STRING_ADDR, "%d");
    check(call_printf(printf_impl, STRING_ADDR, 42u) == 2u, "printf %d");
    put_at(STRING_ADDR, "%s");
    put_at(PATH_ADDR, "ok");
    check(call_printf(printf_impl, STRING_ADDR, PATH_ADDR) == 2u,
          "printf %s");
    put_at(STRING_ADDR, "%08X");
    check(call_printf(printf_impl, STRING_ADDR, 0x2Au) == 8u,
          "printf %08X");
    check(call_printf(printf_impl, INVALID_ADDR, 0u) == 0xFFFFFFFFu,
          "printf invalid format");

    /* A missing platform file API is an error, not a zero-returning stub. */
    g_platform.file.open = NULL;
    put_at(PATH_ADDR, "Z:\\x");
    put_at(MODE_ADDR, "rb");
    check(call2(fopen_impl, PATH_ADDR, MODE_ADDR) == 0u,
          "missing platform open");

    if (g_failures) {
        printf("msvcr80 stdio: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("msvcr80 stdio OK (19 handlers)\n");
    return 0;
}
