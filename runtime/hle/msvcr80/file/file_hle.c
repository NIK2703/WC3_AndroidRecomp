/*
 * file_hle.c - target-side MSVCR80 low-level file descriptors.
 *
 * The guest imports _open, _read, _write, _lseeki64, _close and the four
 * _stat/_fstat variants from the CRT.  The as-built table did not define them,
 * so the resolver bound generic zero-returning stubs.  This slice implements
 * the descriptor and stat surface over Wc3FileApi and keeps the as-built path
 * translation:
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
#include "hle_bridge.h"
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
    pthread_mutex_t lock;
} FileSlot;

/* Descriptor table locking.
 *
 * The only nested acquisition order is g_fd_table_lock -> FileSlot.lock, used
 * by open.  Nothing acquires the table lock while holding a slot lock.  A
 * read/write/seek/size takes only the slot lock.  Close takes the table lock
 * briefly to publish/retire g_closing, then the slot lock, then the table lock
 * again to return the slot; those table sections never overlap the slot lock.
 *
 * g_fd_table_lock protects only g_free_mask/g_closing and is never held across
 * a Wc3FileApi callback.  A slot lock is the per-descriptor pin: read, write,
 * seek, size and close hold it across the host call, so a close cannot release
 * a handle that an in-flight operation is using, and the slot is not returned
 * to the free bitmap until its close has finished.  open owns a slot by
 * clearing its free bit under the table lock, then publishes handle/used under
 * the slot lock.
 *
 * A close publishes g_closing before it marks the slot unused and keeps that
 * count until the host close returns.  If an open finds the bitmap full while
 * a close is in flight, it waits for the close to return its slot instead of
 * reporting a false EMFILE.  A truly full table still returns -1.
 *
 * Wc3FileApi is a host-only platform seam (POSIX read/write/lseek or stdio).
 * Its callbacks must not dispatch guest code or re-enter this module, so
 * holding a slot lock across them does not run guest code under a host lock.
 */
static FileSlot g_slots[FD_LIMIT];
static pthread_mutex_t g_fd_table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fd_slot_freed = PTHREAD_COND_INITIALIZER;
static pthread_once_t g_fd_once = PTHREAD_ONCE_INIT;

_Static_assert(FD_LIMIT > 0u && FD_LIMIT <= 64u,
               "descriptor free bitmap is 64 bits wide");

/* Bit i set means slot i is free.  The bitmap makes allocation O(1) and keeps
 * the previous linear scan's lowest-free descriptor choice. */
static uint64_t g_free_mask = UINT64_MAX >> (64u - FD_LIMIT);
static unsigned g_closing;

static void init_fd_table(void)
{
    for (unsigned i = 0; i < FD_LIMIT; i++)
        pthread_mutex_init(&g_slots[i].lock, NULL);
}

static void ensure_fd_table(void)
{
    (void)pthread_once(&g_fd_once, init_fd_table);
}

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
int wc3_hle_msvcr80_read_guest_path(uint32_t address, char *buffer,
                                    size_t capacity)
{
    size_t i = 0;

    if (!address || !buffer || capacity < 2u) return 0;
    while (i + 1u < capacity) {
        if (address > UINT32_MAX - (uint32_t)i) return 0;
        const uint32_t byte_address = address + (uint32_t)i;
        const size_t remaining = capacity - 1u - i;
        uint32_t chunk = remaining > 64u ? 64u : (uint32_t)remaining;
        const uint64_t room = 0x100000000ull - byte_address;
        if ((uint64_t)chunk > room) chunk = (uint32_t)room;

        if (wc3_legacy_guest_ok(byte_address, chunk)) {
            for (uint32_t j = 0; j < chunk; j++) {
                const char ch = (char)wc3_legacy_rd8g(byte_address + j);
                buffer[i++] = ch;
                if (!ch) return 1;
            }
        } else {
            /* A chunk that ends in an unmapped byte must keep the old
             * all-or-nothing result: walk it byte by byte. */
            for (uint32_t j = 0; j < chunk; j++) {
                const uint32_t at = byte_address + j;
                if (!wc3_legacy_guest_ok(at, 1u)) return 0;
                const char ch = (char)wc3_legacy_rd8g(at);
                buffer[i++] = ch;
                if (!ch) return 1;
            }
        }
    }
    return 0;
}

/* Same translation as the as-built win_path(): the guest's fake Z: drive is
 * the POSIX root; C: paths (and paths with no drive) stay relative to the game
 * directory.  The as-built code writes a leading slash for rooted C: paths
 * too, contradicting its own comment; the target keeps C: relative so a save
 * path cannot escape to the host root. */
void wc3_hle_msvcr80_map_path(const char *input, char *output, size_t capacity)
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

int wc3_hle_msvcr80_fd_open(const char *host_path, int flags, int *fd_out)
{
    if (!host_path || !fd_out) return -1;
    ensure_fd_table();
    const Wc3FileApi *file = file_api();
    if (!file) return -1;

    uintptr_t handle = 0;
    if (file->open(file->ctx, host_path, flags, &handle) != 0) return -1;

    /* The host open is done before the table lock, so a slow filesystem call
     * never serializes unrelated descriptor allocation. */
    pthread_mutex_lock(&g_fd_table_lock);
    /* A close keeps its slot unavailable until the host close returns.  If the
     * bitmap is full only because such a close is in flight, wait for it rather
     * than reporting a false EMFILE; a genuinely full table still fails. */
    while (g_free_mask == 0u && g_closing != 0u)
        (void)pthread_cond_wait(&g_fd_slot_freed, &g_fd_table_lock);
    if (g_free_mask == 0u) {
        /* Table full: discard the newly opened host handle exactly as the
         * linear-scan version did. */
        pthread_mutex_unlock(&g_fd_table_lock);
        (void)file->close(file->ctx, handle);
        return -1;
    }
    const unsigned index = (unsigned)__builtin_ctzll(g_free_mask);
    g_free_mask &= g_free_mask - 1u;

    FileSlot *slot = &g_slots[index];
    pthread_mutex_lock(&slot->lock);
    slot->handle = handle;
    slot->used = 1;
    pthread_mutex_unlock(&slot->lock);
    pthread_mutex_unlock(&g_fd_table_lock);

    *fd_out = (int)(FD_BASE + index);
    return 0;
}

/* Guest fd -> slot by direct index; no table lock is required for the lookup
 * itself because the mapping is fixed.  Slot state is read under slot->lock. */
static FileSlot *fd_slot(int fd)
{
    const unsigned slot = (unsigned)fd;
    if (fd < (int)FD_BASE || slot - FD_BASE >= FD_LIMIT) return NULL;
    return &g_slots[slot - FD_BASE];
}

int wc3_hle_msvcr80_fd_read(int fd, void *buffer, uint32_t count,
                            uint32_t *read_out)
{
    if (count == 0u) {
        if (read_out) *read_out = 0u;
        return 0;
    }
    if (!buffer || count > 0x7FFFFFFFu) return -1;
    const Wc3FileApi *file = file_api();
    if (!file) return -1;
    FileSlot *slot = fd_slot(fd);
    if (!slot) return -1;

    ensure_fd_table();
    pthread_mutex_lock(&slot->lock);
    if (!slot->used) {
        pthread_mutex_unlock(&slot->lock);
        return -1;
    }
    uint32_t got = 0;
    const int rc = file->read(file->ctx, slot->handle, buffer, count, &got);
    pthread_mutex_unlock(&slot->lock);
    if (rc != 0) return -1;
    if (read_out) *read_out = got;
    return 0;
}

int wc3_hle_msvcr80_fd_write(int fd, const void *buffer, uint32_t count,
                             uint32_t *written_out)
{
    if (count == 0u) {
        if (written_out) *written_out = 0u;
        return 0;
    }
    if (!buffer || count > 0x7FFFFFFFu) return -1;
    const Wc3FileApi *file = file_api();
    if (!file) return -1;
    FileSlot *slot = fd_slot(fd);
    if (!slot) return -1;

    ensure_fd_table();
    pthread_mutex_lock(&slot->lock);
    if (!slot->used) {
        pthread_mutex_unlock(&slot->lock);
        return -1;
    }
    uint32_t written = 0;
    const int rc = file->write(file->ctx, slot->handle, buffer, count,
                               &written);
    pthread_mutex_unlock(&slot->lock);
    if (rc != 0) return -1;
    if (written_out) *written_out = written;
    return 0;
}

int wc3_hle_msvcr80_fd_seek(int fd, int64_t offset, int whence,
                            uint64_t *position_out)
{
    if (whence < 0 || whence > 2) return -1;
    const Wc3FileApi *file = file_api();
    if (!file) return -1;
    FileSlot *slot = fd_slot(fd);
    if (!slot) return -1;

    ensure_fd_table();
    pthread_mutex_lock(&slot->lock);
    if (!slot->used) {
        pthread_mutex_unlock(&slot->lock);
        return -1;
    }
    uint64_t position = 0;
    const int rc = file->seek(file->ctx, slot->handle, offset, whence,
                              &position);
    pthread_mutex_unlock(&slot->lock);
    if (rc != 0) return -1;
    if (position_out) *position_out = position;
    return 0;
}

int wc3_hle_msvcr80_fd_size(int fd, uint64_t *size_out)
{
    if (!size_out) return -1;
    const Wc3FileApi *file = file_api();
    if (!file || !file->size) return -1;
    FileSlot *slot = fd_slot(fd);
    if (!slot) return -1;

    ensure_fd_table();
    pthread_mutex_lock(&slot->lock);
    if (!slot->used) {
        pthread_mutex_unlock(&slot->lock);
        return -1;
    }
    const int rc = file->size(file->ctx, slot->handle, size_out);
    pthread_mutex_unlock(&slot->lock);
    return rc == 0 ? 0 : -1;
}

int wc3_hle_msvcr80_fd_close(int fd)
{
    const Wc3FileApi *file = file_api();
    if (!file) return -1;
    FileSlot *slot = fd_slot(fd);
    if (!slot) return -1;

    ensure_fd_table();
    const unsigned index = (unsigned)(slot - g_slots);

    /* Publish the in-flight close before the slot becomes unused.  An open that
     * sees a full bitmap waits for this close to return its slot. */
    pthread_mutex_lock(&g_fd_table_lock);
    g_closing++;
    pthread_mutex_unlock(&g_fd_table_lock);

    pthread_mutex_lock(&slot->lock);
    if (!slot->used) {
        pthread_mutex_unlock(&slot->lock);
        pthread_mutex_lock(&g_fd_table_lock);
        g_closing--;
        pthread_cond_signal(&g_fd_slot_freed);
        pthread_mutex_unlock(&g_fd_table_lock);
        return -1;
    }
    const uintptr_t handle = slot->handle;
    slot->used = 0;
    slot->handle = 0;
    /* The host close runs under the slot lock, so it waits for every in-flight
     * read/write/seek/size on this descriptor and blocks new ones.  The slot
     * stays out of the free bitmap until the close returns, which keeps open
     * from reusing it while the old handle is being destroyed. */
    const int rc = file->close(file->ctx, handle);
    pthread_mutex_unlock(&slot->lock);

    /* Drop the slot lock before taking the table lock: table -> slot is the
     * only nesting order in this module. */
    pthread_mutex_lock(&g_fd_table_lock);
    g_free_mask |= UINT64_C(1) << index;
    g_closing--;
    pthread_cond_signal(&g_fd_slot_freed);
    pthread_mutex_unlock(&g_fd_table_lock);
    return rc == 0 ? 0 : -1;
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
    if (!wc3_hle_msvcr80_read_guest_path(path_address, guest_path,
                                         sizeof guest_path))
        return;
    if (!map_open_flags(guest_flags, &flags)) return;

    wc3_hle_msvcr80_map_path(guest_path, host_path, sizeof host_path);
    int fd = -1;
    if (wc3_hle_msvcr80_fd_open(host_path, flags, &fd) != 0) return;
    c->eax = (uint32_t)fd;
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

    uint32_t got = 0;
    if (wc3_hle_msvcr80_fd_read(fd, (void *)(uintptr_t)buffer, count,
                                &got) != 0)
        return;
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

    uint32_t written = 0;
    if (wc3_hle_msvcr80_fd_write(fd, (const void *)(uintptr_t)buffer, count,
                                 &written) != 0)
        return;
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
    uint64_t position = 0;
    if (wc3_hle_msvcr80_fd_seek(fd, offset, origin, &position) != 0) return;
    c->eax = (uint32_t)position;
    c->edx = (uint32_t)(position >> 32);
}

static void h_close(CPU *c)
{
    fail(c);
    if (wc3_hle_msvcr80_fd_close((int)wc3_legacy_arg32(c, 0u)) == 0)
        c->eax = 0u;
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

static int read_stat_path(uint32_t path_address, char *host_path,
                          const Wc3FileApi **file_out)
{
    char guest_path[FILE_PATH_MAX];
    if (!wc3_hle_msvcr80_read_guest_path(path_address, guest_path,
                                         sizeof guest_path))
        return 0;
    const Wc3FileApi *file = file_api();
    if (!file || !file->stat) return 0;
    wc3_hle_msvcr80_map_path(guest_path, host_path, FILE_PATH_MAX);
    *file_out = file;
    return 1;
}

static void h_stat64i32(CPU *c)
{
    char host_path[FILE_PATH_MAX];
    const Wc3FileApi *file = NULL;
    Wc3FileStat st;

    fail(c);
    if (!read_stat_path(wc3_legacy_arg32(c, 0u), host_path, &file)) return;
    if (file->stat(file->ctx, host_path, &st) != 0) return;
    write_stat(c, wc3_legacy_arg32(c, 1u), &st, 0);
}

static void h_stat64(CPU *c)
{
    char host_path[FILE_PATH_MAX];
    const Wc3FileApi *file = NULL;
    Wc3FileStat st;

    fail(c);
    if (!read_stat_path(wc3_legacy_arg32(c, 0u), host_path, &file)) return;
    if (file->stat(file->ctx, host_path, &st) != 0) return;
    write_stat(c, wc3_legacy_arg32(c, 1u), &st, 1);
}

static int fstat_size(int fd, uint64_t *size_out)
{
    return wc3_hle_msvcr80_fd_size(fd, size_out) == 0;
}

static void h_fstat64(CPU *c)
{
    uint64_t size = 0;

    fail(c);
    if (!fstat_size((int)wc3_legacy_arg32(c, 0u), &size)) return;
    Wc3FileStat st = { size, 0, WC3_FILE_STAT_REGULAR };
    write_stat(c, wc3_legacy_arg32(c, 1u), &st, 1);
}

static void h_fstat64i32(CPU *c)
{
    uint64_t size = 0;

    fail(c);
    if (!fstat_size((int)wc3_legacy_arg32(c, 0u), &size)) return;
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
