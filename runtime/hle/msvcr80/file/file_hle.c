/*
 * file_hle.c - target-side MSVCR80 low-level file descriptors.
 *
 * The guest imports _open, _read, _write, _lseeki64 and _close from the CRT.
 * The as-built table did not define them, so the resolver bound generic
 * zero-returning stubs.  This slice implements the descriptor surface over
 * Wc3FileApi and keeps the as-built path translation:
 *
 *   Z:\data\war3.mpq  -> /data/war3.mpq
 *   C:\foo\bar.txt    -> foo/bar.txt
 *   a\b               -> a/b
 *   ""                -> .
 *
 * Relative paths continue to resolve against the current working directory,
 * which the boot path sets to the game data directory.  Text-mode translation
 * is not performed: the target is byte-oriented, matching the as-built Android
 * file path.  Wc3FileApi carries no permission bits, so the _open mode
 * argument is accepted and ignored.
 */
#include "file_hle.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "platform.h"

#define FILE_PATH_MAX 512u
#define FD_LIMIT 64u
#define FD_BASE 3u

/* _open flags from the Microsoft CRT.  Only the bits Wc3FileApi understands
 * are translated; text/binary, no-inherit and buffering hints have no host
 * effect here. */
enum {
    CRT_O_RDONLY = 0x0000,
    CRT_O_WRONLY = 0x0001,
    CRT_O_RDWR = 0x0002,
    CRT_O_APPEND = 0x0008,
    CRT_O_CREAT = 0x0100,
    CRT_O_TRUNC = 0x0200,
    CRT_O_EXCL = 0x0400
};

typedef struct FileSlot {
    uintptr_t handle;
    int used;
} FileSlot;

static FileSlot g_slots[FD_LIMIT];
static pthread_mutex_t g_fd_lock = PTHREAD_MUTEX_INITIALIZER;

static const Wc3FileApi *file_api(void)
{
    const Wc3Platform *platform = wc3_platform();
    if (!platform) return NULL;
    const Wc3FileApi *file = &platform->file;
    if (!file->open || !file->read || !file->write || !file->seek ||
        !file->close)
        return NULL;
    return file;
}

/* Read a bounded guest C string.  A missing terminator or an unmapped byte is
 * a hard path failure rather than a truncated name. */
static int read_guest_path(uint32_t address, char *buffer, size_t capacity)
{
    size_t i = 0;

    if (!address || !buffer || capacity < 2u) return 0;
    while (i + 1u < capacity) {
        if (address > UINT32_MAX - (uint32_t)i) return 0;
        const uint32_t byte_address = address + (uint32_t)i;
        if (!wc3_legacy_guest_ok(byte_address, 1u)) return 0;
        const char ch = (char)wc3_legacy_rd8g(byte_address);
        buffer[i++] = ch;
        if (!ch) return 1;
    }
    return 0;
}

/* Same translation as the as-built win_path(): the guest's fake Z: drive is
 * the POSIX root; C: paths (and paths with no drive) stay relative to the game
 * directory.  The as-built code writes a leading slash for rooted C: paths
 * too, contradicting its own comment; the target keeps C: relative so a save
 * path cannot escape to the host root. */
static void map_path(const char *input, char *output, size_t capacity)
{
    size_t out = 0;
    int keep_root = 1;

    if (!input || capacity == 0u) return;
    if (isalpha((unsigned char)input[0]) && input[1] == ':') {
        keep_root = (input[0] == 'Z' || input[0] == 'z') &&
                    (input[2] == '\\' || input[2] == '/');
        input += 2;
    }
    while (*input == '\\' || *input == '/') {
        if (keep_root && out == 0u && out + 1u < capacity) output[out++] = '/';
        input++;
    }
    for (; *input && out + 1u < capacity; input++)
        output[out++] = (*input == '\\') ? '/' : *input;
    output[out] = '\0';
    if (out == 0u && capacity >= 2u) {
        output[0] = '.';
        output[1] = '\0';
    }
}

static int map_open_flags(int flags, int *mapped)
{
    const int access = flags & 3;
    int out = 0;

    if (access == CRT_O_RDONLY)
        out = WC3_FILE_READ;
    else if (access == CRT_O_WRONLY)
        out = WC3_FILE_WRITE;
    else if (access == CRT_O_RDWR)
        out = WC3_FILE_READ | WC3_FILE_WRITE;
    else
        return 0;

    if (flags & CRT_O_APPEND) out |= WC3_FILE_APPEND;
    if (flags & CRT_O_CREAT) out |= WC3_FILE_CREATE;
    if (flags & CRT_O_TRUNC) out |= WC3_FILE_TRUNCATE;
    if ((flags & CRT_O_EXCL) && (flags & CRT_O_CREAT))
        out |= WC3_FILE_EXCLUSIVE;
    *mapped = out;
    return 1;
}

static void fail(CPU *c)
{
    c->eax = 0xFFFFFFFFu;
}

static void h_open(CPU *c)
{
    const uint32_t path_address = wc3_legacy_arg32(c, 0u);
    const int guest_flags = (int)wc3_legacy_arg32(c, 1u);
    char guest_path[FILE_PATH_MAX];
    char host_path[FILE_PATH_MAX];
    int flags = 0;

    fail(c);
    if (!read_guest_path(path_address, guest_path, sizeof guest_path)) return;
    if (!map_open_flags(guest_flags, &flags)) return;
    const Wc3FileApi *file = file_api();
    if (!file) return;

    map_path(guest_path, host_path, sizeof host_path);
    uintptr_t handle = 0;
    if (file->open(file->ctx, host_path, flags, &handle) != 0) return;

    pthread_mutex_lock(&g_fd_lock);
    for (unsigned i = 0; i < FD_LIMIT; i++) {
        if (g_slots[i].used) continue;
        g_slots[i].used = 1;
        g_slots[i].handle = handle;
        pthread_mutex_unlock(&g_fd_lock);
        c->eax = FD_BASE + i;
        return;
    }
    pthread_mutex_unlock(&g_fd_lock);
    (void)file->close(file->ctx, handle);
}

static void h_read(CPU *c)
{
    const int fd = (int)wc3_legacy_arg32(c, 0u);
    const uint32_t buffer = wc3_legacy_arg32(c, 1u);
    const uint32_t count = wc3_legacy_arg32(c, 2u);

    fail(c);
    if (count == 0u) {
        c->eax = 0u;
        return;
    }
    if (count > 0x7FFFFFFFu) return;
    if (!wc3_legacy_guest_ok(buffer, count)) return;
    const Wc3FileApi *file = file_api();
    if (!file) return;

    const unsigned slot = (unsigned)fd;
    if (fd < (int)FD_BASE || slot - FD_BASE >= FD_LIMIT) return;

    pthread_mutex_lock(&g_fd_lock);
    if (!g_slots[slot - FD_BASE].used) {
        pthread_mutex_unlock(&g_fd_lock);
        return;
    }
    uint32_t got = 0;
    const int rc = file->read(file->ctx, g_slots[slot - FD_BASE].handle,
                              (void *)(uintptr_t)buffer, count, &got);
    pthread_mutex_unlock(&g_fd_lock);
    if (rc != 0) return;
    c->eax = got;
}

static void h_write(CPU *c)
{
    const int fd = (int)wc3_legacy_arg32(c, 0u);
    const uint32_t buffer = wc3_legacy_arg32(c, 1u);
    const uint32_t count = wc3_legacy_arg32(c, 2u);

    fail(c);
    if (count == 0u) {
        c->eax = 0u;
        return;
    }
    if (count > 0x7FFFFFFFu) return;
    if (!wc3_legacy_guest_ok(buffer, count)) return;
    const Wc3FileApi *file = file_api();
    if (!file) return;

    const unsigned slot = (unsigned)fd;
    if (fd < (int)FD_BASE || slot - FD_BASE >= FD_LIMIT) return;

    pthread_mutex_lock(&g_fd_lock);
    if (!g_slots[slot - FD_BASE].used) {
        pthread_mutex_unlock(&g_fd_lock);
        return;
    }
    uint32_t written = 0;
    const int rc = file->write(file->ctx, g_slots[slot - FD_BASE].handle,
                               (const void *)(uintptr_t)buffer, count,
                               &written);
    pthread_mutex_unlock(&g_fd_lock);
    if (rc != 0) return;
    c->eax = written;
}

static void h_lseeki64(CPU *c)
{
    const int fd = (int)wc3_legacy_arg32(c, 0u);
    const uint32_t low = wc3_legacy_arg32(c, 1u);
    const uint32_t high = wc3_legacy_arg32(c, 2u);
    const int origin = (int)wc3_legacy_arg32(c, 3u);
    const int64_t offset = (int64_t)(((uint64_t)high << 32) | low);

    fail(c);
    c->edx = 0xFFFFFFFFu;
    if (origin < 0 || origin > 2) return;
    const Wc3FileApi *file = file_api();
    if (!file) return;

    const unsigned slot = (unsigned)fd;
    if (fd < (int)FD_BASE || slot - FD_BASE >= FD_LIMIT) return;

    pthread_mutex_lock(&g_fd_lock);
    if (!g_slots[slot - FD_BASE].used) {
        pthread_mutex_unlock(&g_fd_lock);
        return;
    }
    uint64_t position = 0;
    const int rc = file->seek(file->ctx, g_slots[slot - FD_BASE].handle,
                              offset, origin, &position);
    pthread_mutex_unlock(&g_fd_lock);
    if (rc != 0) return;
    c->eax = (uint32_t)position;
    c->edx = (uint32_t)(position >> 32);
}

static void h_close(CPU *c)
{
    const int fd = (int)wc3_legacy_arg32(c, 0u);
    const Wc3FileApi *file = file_api();

    fail(c);
    if (!file) return;
    const unsigned slot = (unsigned)fd;
    if (fd < (int)FD_BASE || slot - FD_BASE >= FD_LIMIT) return;

    pthread_mutex_lock(&g_fd_lock);
    if (!g_slots[slot - FD_BASE].used) {
        pthread_mutex_unlock(&g_fd_lock);
        return;
    }
    const uintptr_t handle = g_slots[slot - FD_BASE].handle;
    g_slots[slot - FD_BASE].used = 0;
    g_slots[slot - FD_BASE].handle = 0;
    const int rc = file->close(file->ctx, handle);
    pthread_mutex_unlock(&g_fd_lock);
    if (rc == 0) c->eax = 0u;
}

/* MSVCRT's _stat64i32 is 48 bytes and _stat64 is 56 bytes.  Only the fields
 * observable to the guest file code are filled: mode at 6, nlink at 8, size at
 * 24, and the three time_t values after it.  The guest clears the buffer first,
 * so the padding and the dev/ino/rdev fields stay zero. */
static void write_stat(CPU *c, uint32_t address, const Wc3FileStat *st,
                       int wide_time)
{
    const uint32_t total = wide_time ? 56u : 48u;
    if (!wc3_legacy_guest_ok(address, total)) {
        fail(c);
        return;
    }

    for (uint32_t i = 0; i < total; i++) wc3_legacy_wr8g(address + i, 0u);

    uint16_t mode = (st->mode & WC3_FILE_STAT_DIRECTORY) ? 0x4000u : 0x8000u;
    mode = (uint16_t)(mode | 0x0100u | 0x0080u); /* _S_IREAD | _S_IWRITE */
    wc3_legacy_wr16g(address + 6u, mode);
    wc3_legacy_wr16g(address + 8u, 1u); /* st_nlink */
    wc3_legacy_wr32g(address + 24u, (uint32_t)st->size);
    wc3_legacy_wr32g(address + 28u, (uint32_t)(st->size >> 32));

    const uint32_t low = (uint32_t)(uint64_t)st->mtime_seconds;
    const uint32_t high =
        (uint32_t)((uint64_t)st->mtime_seconds >> 32);
    if (wide_time) {
        wc3_legacy_wr32g(address + 32u, low);
        wc3_legacy_wr32g(address + 36u, high);
        wc3_legacy_wr32g(address + 40u, low);
        wc3_legacy_wr32g(address + 44u, high);
        wc3_legacy_wr32g(address + 48u, low);
        wc3_legacy_wr32g(address + 52u, high);
    } else {
        wc3_legacy_wr32g(address + 32u, low);
        wc3_legacy_wr32g(address + 36u, low);
        wc3_legacy_wr32g(address + 40u, low);
    }
    c->eax = 0u;
}

static int read_stat_path(CPU *c, uint32_t path_address, char *host_path,
                          const Wc3FileApi **file_out)
{
    char guest_path[FILE_PATH_MAX];
    if (!read_guest_path(path_address, guest_path, sizeof guest_path)) return 0;
    const Wc3FileApi *file = file_api();
    if (!file || !file->stat) return 0;
    map_path(guest_path, host_path, FILE_PATH_MAX);
    *file_out = file;
    return 1;
}

static void h_stat64i32(CPU *c)
{
    char host_path[FILE_PATH_MAX];
    const Wc3FileApi *file = NULL;
    Wc3FileStat st;

    fail(c);
    if (!read_stat_path(c, wc3_legacy_arg32(c, 0u), host_path, &file)) return;
    if (file->stat(file->ctx, host_path, &st) != 0) return;
    write_stat(c, wc3_legacy_arg32(c, 1u), &st, 0);
}

static void h_stat64(CPU *c)
{
    char host_path[FILE_PATH_MAX];
    const Wc3FileApi *file = NULL;
    Wc3FileStat st;

    fail(c);
    if (!read_stat_path(c, wc3_legacy_arg32(c, 0u), host_path, &file)) return;
    if (file->stat(file->ctx, host_path, &st) != 0) return;
    write_stat(c, wc3_legacy_arg32(c, 1u), &st, 1);
}

static int fstat_size(CPU *c, int fd, uint64_t *size_out)
{
    const Wc3FileApi *file = file_api();
    if (!file) return 0;
    const unsigned slot = (unsigned)fd;
    if (fd < (int)FD_BASE || slot - FD_BASE >= FD_LIMIT) return 0;

    pthread_mutex_lock(&g_fd_lock);
    if (!g_slots[slot - FD_BASE].used) {
        pthread_mutex_unlock(&g_fd_lock);
        return 0;
    }
    const int rc = file->size(file->ctx, g_slots[slot - FD_BASE].handle,
                              size_out);
    pthread_mutex_unlock(&g_fd_lock);
    return rc == 0;
}

static void h_fstat64(CPU *c)
{
    uint64_t size = 0;

    fail(c);
    if (!fstat_size(c, (int)wc3_legacy_arg32(c, 0u), &size)) return;
    Wc3FileStat st = { size, 0, WC3_FILE_STAT_REGULAR };
    write_stat(c, wc3_legacy_arg32(c, 1u), &st, 1);
}

static void h_fstat64i32(CPU *c)
{
    uint64_t size = 0;

    fail(c);
    if (!fstat_size(c, (int)wc3_legacy_arg32(c, 0u), &size)) return;
    Wc3FileStat st = { size, 0, WC3_FILE_STAT_REGULAR };
    write_stat(c, wc3_legacy_arg32(c, 1u), &st, 0);
}

static const struct {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} g_additions[] = {
    { "_open", 3u, h_open },
    { "_read", 3u, h_read },
    { "_write", 3u, h_write },
    { "_lseeki64", 4u, h_lseeki64 },
    { "_close", 1u, h_close },
    { "_stat64i32", 2u, h_stat64i32 },
    { "_stat64", 2u, h_stat64 },
    { "_fstat64", 2u, h_fstat64 },
    { "_fstat64i32", 2u, h_fstat64i32 },
};

int wc3_hle_msvcr80_file_install(void)
{
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("MSVCR80.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 1,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_additions / sizeof g_additions[0]);
}
