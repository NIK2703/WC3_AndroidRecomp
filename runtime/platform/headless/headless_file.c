/*
 * headless_file.c - Wc3FileApi over stdio.
 *
 * The headless target reads licensed PEs and saves from the host filesystem.
 * A FILE* is wrapped in a heap object because the API handle is uintptr_t and
 * the caller must be able to close it from any guest thread.  The API is
 * byte-oriented; no text translation is performed on Windows.
 */
#define _FILE_OFFSET_BITS 64

#include "headless_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#if defined(_WIN32)
typedef __int64 HeadlessOffset;
#define headless_fseek _fseeki64
#define headless_ftell _ftelli64
#else
typedef off_t HeadlessOffset;
#define headless_fseek fseeko
#define headless_ftell ftello
#endif

typedef struct HeadlessFile {
    FILE *fp;
} HeadlessFile;

static int path_exists(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    (void)fclose(fp);
    return 1;
}

static const char *select_mode(int flags, int exists)
{
    int read = (flags & WC3_FILE_READ) != 0;
    int write = (flags & WC3_FILE_WRITE) != 0;
    int create = (flags & WC3_FILE_CREATE) != 0;
    int truncate = (flags & WC3_FILE_TRUNCATE) != 0;
    int append = (flags & WC3_FILE_APPEND) != 0;

    if (!read && !write) return NULL;
    if (append) {
        if (read && write) return "a+b";
        if (write) return "ab";
        return NULL;
    }
    if (read && write) {
        if (exists && !truncate) return "r+b";
        if (create) return "w+b";
        return NULL;
    }
    if (write) {
        if (exists && !truncate) return "r+b";
        if (create) return "wb";
        return NULL;
    }
    return "rb";
}

static int headless_file_open(void *ctx, const char *path, int flags,
                              uintptr_t *out)
{
    (void)ctx;
    if (!path || !out) return -1;

    int exists = path_exists(path);
    if ((flags & WC3_FILE_EXCLUSIVE) != 0 && exists) return -1;
    if ((flags & WC3_FILE_TRUNCATE) != 0 &&
        (flags & WC3_FILE_CREATE) == 0 && !exists)
        return -1;

    const char *mode = select_mode(flags, exists);
    if (!mode) return -1;

    FILE *fp = fopen(path, mode);
    if (!fp) return -1;

    HeadlessFile *file = (HeadlessFile *)malloc(sizeof *file);
    if (!file) {
        (void)fclose(fp);
        return -1;
    }
    file->fp = fp;
    *out = (uintptr_t)file;
    return 0;
}

static int headless_file_read(void *ctx, uintptr_t handle, void *buf,
                              uint32_t len, uint32_t *read_out)
{
    (void)ctx;
    HeadlessFile *file = (HeadlessFile *)(uintptr_t)handle;
    if (!file || !file->fp || (!buf && len != 0u)) return -1;
    size_t got = fread(buf, 1, (size_t)len, file->fp);
    if (read_out) *read_out = (uint32_t)got;
    return ferror(file->fp) ? -1 : 0;
}

static int headless_file_write(void *ctx, uintptr_t handle, const void *buf,
                               uint32_t len, uint32_t *written_out)
{
    (void)ctx;
    HeadlessFile *file = (HeadlessFile *)(uintptr_t)handle;
    if (!file || !file->fp || (!buf && len != 0u)) return -1;
    size_t put = fwrite(buf, 1, (size_t)len, file->fp);
    if (written_out) *written_out = (uint32_t)put;
    return put == (size_t)len && !ferror(file->fp) ? 0 : -1;
}

static int headless_file_seek(void *ctx, uintptr_t handle, int64_t offset,
                              int whence, uint64_t *pos_out)
{
    (void)ctx;
    HeadlessFile *file = (HeadlessFile *)(uintptr_t)handle;
    if (!file || !file->fp) return -1;
    int native = whence == 0 ? SEEK_SET : (whence == 1 ? SEEK_CUR : SEEK_END);
    if (whence != 0 && whence != 1 && whence != 2) return -1;
    if (headless_fseek(file->fp, (HeadlessOffset)offset, native) != 0) return -1;
    HeadlessOffset at = headless_ftell(file->fp);
    if (at < 0) return -1;
    if (pos_out) *pos_out = (uint64_t)at;
    return 0;
}

static int headless_file_size(void *ctx, uintptr_t handle, uint64_t *size_out)
{
    (void)ctx;
    HeadlessFile *file = (HeadlessFile *)(uintptr_t)handle;
    if (!file || !file->fp || !size_out) return -1;
    HeadlessOffset at = headless_ftell(file->fp);
    if (at < 0 || headless_fseek(file->fp, 0, SEEK_END) != 0) return -1;
    HeadlessOffset end = headless_ftell(file->fp);
    (void)headless_fseek(file->fp, at, SEEK_SET);
    if (end < 0) return -1;
    *size_out = (uint64_t)end;
    return 0;
}

static int headless_file_close(void *ctx, uintptr_t handle)
{
    (void)ctx;
    HeadlessFile *file = (HeadlessFile *)(uintptr_t)handle;
    if (!file) return -1;
    int rc = 0;
    if (file->fp && fclose(file->fp) != 0) rc = -1;
    free(file);
    return rc;
}

void headless_file_install(Wc3Platform *platform, HeadlessState *st)
{
    platform->file.ctx = st;
    platform->file.open = headless_file_open;
    platform->file.read = headless_file_read;
    platform->file.write = headless_file_write;
    platform->file.seek = headless_file_seek;
    platform->file.size = headless_file_size;
    platform->file.close = headless_file_close;
}
