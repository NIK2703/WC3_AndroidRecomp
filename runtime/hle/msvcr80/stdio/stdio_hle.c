/*
 * stdio_hle.c - target-side MSVCR80 stdio streams.
 *
 * This slice replaces the stream surface the game actually imports:
 *
 *   fopen, fopen_s, fclose, fread, fwrite, fseek, ftell, fflush, feof, ferror
 *
 * and adds the missing rows on the same 32-byte guest records: putc, fputc,
 * fputs, fgets, _fileno, _wfopen, _setmode, _fdopen and printf.  A file stream
 * owns a Wc3FileApi descriptor from the low-level file slice; slots 0..2 are
 * the standard streams and stay on the host console.  Text mode is accepted
 * but not translated, matching the byte-oriented as-built Android path.
 *
 * fprintf and vfprintf remain the as-built no-op diagnostic rows: they do not
 * dereference the stream record, so replacing fopen does not make them unsafe.
 */
#include "stdio_hle.h"

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "file_hle.h"
#include "hle_bridge.h"
#include "platform.h"
#include "wc3_debug.h"

#define STREAM_MAGIC 0x47465354u /* 'GFST' */
#define STREAM_LIMIT 64u
#define STREAM_FIRST 3u
#define STREAM_PATH_MAX 512u
#define STREAM_MODE_MAX 32u
#define STREAM_WIDE_MAX 512u
#define STREAM_STRING_LIMIT 0x100000u

/* Microsoft _setmode modes.  The target is byte-oriented, so only the
 * reported/remembered mode changes; CRLF translation is intentionally not
 * performed, matching the as-built Android stream path. */
#define CRT_O_TEXT 0x4000
#define CRT_O_BINARY 0x8000
#define CRT_O_WTEXT 0x10000
#define CRT_O_U16TEXT 0x20000
#define CRT_O_U8TEXT 0x40000

typedef struct Stream {
    int used;       /* slot allocation bit, owned by g_stream_lock */
    int closed;     /* owned by Stream.lock; 1 = closed or never opened */
    int std_index;  /* 0..2 for stdin/stdout/stderr, -1 for a file */
    int fd;
    int eof;
    int error;
    int mode;
    _Atomic uint64_t generation; /* bumped on slot reuse */
    _Atomic int fd_hint;         /* advisory file-fd slot hint, -1 otherwise */
    pthread_mutex_t lock;
#if WC3_DEBUG && WC3_DEBUG_SAVE
    /* Host path kept only for the save path trace.  It is not part of the
     * guest record and never changes stream semantics. */
    char save_path[STREAM_PATH_MAX];
#endif
} Stream;

#if WC3_DEBUG && WC3_DEBUG_SAVE
/* The save trace is collected while Stream.lock is held, then printed by the
 * caller after the unlock.  The trace is per-thread because different guest
 * threads may be inside different stream handlers at the same time. */
#define STREAM_TRACE_MAX 400u
typedef struct StreamTraceEvent {
    uint32_t count;
    uint32_t result;
    int done;
} StreamTraceEvent;

typedef struct StreamTrace {
    int fd;
    char path[STREAM_PATH_MAX];
    unsigned events;
    StreamTraceEvent event[STREAM_TRACE_MAX];
} StreamTrace;

static _Thread_local StreamTrace *g_stream_trace;

static void stream_trace_begin(StreamTrace *trace, const Stream *stream)
{
    trace->fd = stream->fd;
    memcpy(trace->path, stream->save_path, sizeof trace->path);
    trace->events = 0u;
    g_stream_trace = trace;
}

static void stream_trace_event(uint32_t count, uint32_t result, int done)
{
    StreamTrace *trace = g_stream_trace;
    if (!trace || trace->events >= STREAM_TRACE_MAX) return;
    trace->event[trace->events].count = count;
    trace->event[trace->events].result = result;
    trace->event[trace->events].done = done;
    trace->events++;
}

static void stream_trace_flush(StreamTrace *trace, int write)
{
    g_stream_trace = NULL;
    for (unsigned i = 0; i < trace->events; i++) {
        const StreamTraceEvent *event = &trace->event[i];
        if (write)
            fprintf(stderr, "[savecrt] write begin fd=%d n=%u path=\"%s\"\n",
                    trace->fd, event->count, trace->path);
        else
            fprintf(stderr, "[savecrt] read begin fd=%d n=%u path=\"%s\"\n",
                    trace->fd, event->count, trace->path);
        if (!event->done) continue;
        if (write)
            fprintf(stderr, "[savecrt] write done n=%u -> %u\n",
                    event->count, event->result);
        else
            fprintf(stderr, "[savecrt] read done n=%u -> %u\n",
                    event->count, event->result);
    }
}
#endif

/* Stream table locking.
 *
 * g_stream_lock owns slot lifecycle: the `used` allocation bits, the _fdopen
 * in-use scan, the standard-fd duplicate fallback, and the short claim/retire
 * transitions.  An operation on a live stream never takes it, and it is never
 * held across a Wc3FileApi callback.
 *
 * A live stream is pinned by its own mutex.  stream_acquire() and
 * stream_by_fd_acquire() return with stream->lock held; the read/write/seek/
 * tell/flush/attribute handlers run under it and release it.  fclose() holds
 * the same mutex across the host close, so an in-flight operation finishes
 * before the descriptor is closed and the slot cannot be recycled underneath
 * it.
 *
 * stream_finish() clears the guest record magic before the slot is retired,
 * and stream_new() bumps `generation` under the slot mutex.  Acquire reads the
 * generation before validating the magic and rechecks it under the mutex, so a
 * stale record that raced with close-and-reuse is rejected instead of
 * operating on the slot's new occupant.
 *
 * `fd_hint` is an advisory, never-cleared file fd -> slot hint published by
 * stream_new() under the slot mutex; stream_by_fd_acquire() revalidates the
 * slot identity under that same mutex, so a stale hint costs one lock and can
 * never select the wrong stream.  The file-fd lookup therefore no longer needs
 * g_stream_lock.
 *
 * Lock order: g_stream_lock -> Stream.lock (stream_new only).  stream_finish()
 * drops Stream.lock before taking g_stream_lock, so the order never inverts.
 * Stream.lock may nest over a file_hle descriptor lock through the fd_* seam;
 * that module never calls back into this one.  Wc3FileApi is a host-only
 * platform seam (POSIX file I/O): it never dispatches guest code or re-enters
 * this module, so holding a Stream mutex across it does not run guest code
 * under a host lock.
 */
static Stream g_streams[STREAM_LIMIT];
static pthread_mutex_t g_stream_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_stream_once = PTHREAD_ONCE_INIT;

static void init_streams(void)
{
    for (unsigned i = 0; i < STREAM_LIMIT; i++) {
        pthread_mutex_init(&g_streams[i].lock, NULL);
        /* A slot that was never owned is not acquirable.  `closed` is the
         * per-object gate now that acquire does not take g_stream_lock;
         * stream_new() clears it last, after every other field is set. */
        g_streams[i].closed = 1;
        atomic_store_explicit(&g_streams[i].fd_hint, -1,
                              memory_order_relaxed);
    }
    for (unsigned i = 0; i < 3u; i++) {
        g_streams[i].used = 1;
        g_streams[i].closed = 0;
        g_streams[i].std_index = (int)i;
        g_streams[i].fd = -1;
        g_streams[i].mode = CRT_O_BINARY;
    }
}

static void ensure_streams(void)
{
    (void)pthread_once(&g_stream_once, init_streams);
}

static FILE *std_file(int index)
{
    if (index == 0) return stdin;
    if (index == 1) return stdout;
    if (index == 2) return stderr;
    return NULL;
}

/* The guest record is opaque; it carries the same magic and slot as the
 * as-built table so __iob_func()'s three records continue to work.
 *
 * Resolve a record to a pinned stream.  The generation is sampled before the
 * mutex; after locking, both the generation and the record magic are checked.
 * stream_finish() clears the magic under this mutex before the slot is
 * retired, and stream_new() bumps the generation under it, so a stale record
 * that raced with close-and-reuse is rejected instead of operating on the
 * slot's new occupant.  Only a live slot has closed == 0, which also replaces
 * the old `used` test for never-opened slots. */
static Stream *stream_acquire(uint32_t record)
{
    ensure_streams();
    if (!record || !wc3_legacy_guest_ok(record, 8u)) return NULL;

    const uint32_t slot = wc3_legacy_rd32g(record + 4u);
    if (slot >= STREAM_LIMIT) return NULL;

    Stream *stream = &g_streams[slot];
    /* Snapshot the generation before the guest magic.  If close-and-reuse
     * happens between these two reads, the old record's magic is already zero
     * and the magic read rejects it; if reuse happens after the magic read, the
     * generation check under the lock rejects it.  Neither read needs to happen
     * under the slot lock, so O-34 keeps guest memory outside host locks. */
    const uint64_t generation = atomic_load_explicit(
        &stream->generation, memory_order_relaxed);
    if (wc3_legacy_rd32g(record) != STREAM_MAGIC) return NULL;

    pthread_mutex_lock(&stream->lock);
    if (stream->closed ||
        atomic_load_explicit(&stream->generation, memory_order_relaxed) !=
            generation) {
        pthread_mutex_unlock(&stream->lock);
        return NULL;
    }
    return stream;
}

static Stream *stream_by_fd_acquire(int fd)
{
    ensure_streams();
    if (fd < 0) return NULL;

    /* The three standard slots have a fixed identity and are never recycled,
     * so printf's stdout lookup needs neither the table lock nor a scan. */
    if (fd < (int)STREAM_FIRST) {
        Stream *stream = &g_streams[fd];
        pthread_mutex_lock(&stream->lock);
        if (!stream->closed) return stream;
        pthread_mutex_unlock(&stream->lock);

        /* fclose(stdout) retires the fixed slot, but _fdopen() may have bound
         * the same standard fd to a later record.  The as-built scan skipped
         * a slot only after `used` was cleared and stopped at the first used
         * match, so keep it verbatim on this rare fallback; the hot printf
         * lookup above never reaches it. */
        pthread_mutex_lock(&g_stream_lock);
        Stream *candidate = NULL;
        if (!g_streams[fd].used) {
            for (unsigned i = STREAM_FIRST; i < STREAM_LIMIT; i++) {
                if (g_streams[i].used && g_streams[i].std_index == fd) {
                    candidate = &g_streams[i];
                    break;
                }
            }
        }
        pthread_mutex_unlock(&g_stream_lock);
        if (!candidate) return NULL;

        pthread_mutex_lock(&candidate->lock);
        if (candidate->closed || candidate->std_index != fd) {
            pthread_mutex_unlock(&candidate->lock);
            return NULL;
        }
        return candidate;
    }

    for (unsigned i = STREAM_FIRST; i < STREAM_LIMIT; i++) {
        Stream *stream = &g_streams[i];
        if (atomic_load_explicit(&stream->fd_hint, memory_order_acquire) != fd)
            continue;
        pthread_mutex_lock(&stream->lock);
        if (stream->closed) {
            pthread_mutex_unlock(&stream->lock);
            continue;
        }
        if (stream->std_index < 0 && stream->fd == fd) return stream;
        pthread_mutex_unlock(&stream->lock);
    }
    return NULL;
}

static int stream_fd_in_use(int fd)
{
    ensure_streams();
    int found = 0;
    pthread_mutex_lock(&g_stream_lock);
    for (unsigned i = 0; i < STREAM_LIMIT; i++) {
        if (g_streams[i].used && g_streams[i].std_index < 0 &&
            g_streams[i].fd == fd) {
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_stream_lock);
    return found;
}

static uint32_t stream_new(int fd, int std_index, int mode)
{
    ensure_streams();
    uint32_t record = wc3_legacy_guest_alloc0(0x20u);
    if (!record) return 0;

    unsigned slot = STREAM_LIMIT;
    pthread_mutex_lock(&g_stream_lock);
    for (unsigned i = STREAM_FIRST; i < STREAM_LIMIT; i++) {
        Stream *stream = &g_streams[i];
        if (stream->used) continue;
        stream->used = 1;
        /* g_stream_lock -> Stream.lock is the only nesting order.  The slot
         * has no user yet, so this wait is never a host I/O call. */
        pthread_mutex_lock(&stream->lock);
        atomic_fetch_add_explicit(&stream->generation, 1u,
                                  memory_order_relaxed);
        stream->std_index = std_index;
        stream->fd = fd;
        stream->eof = 0;
        stream->error = 0;
        stream->mode = mode;
        atomic_store_explicit(&stream->fd_hint,
                              std_index < 0 ? fd : -1,
                              memory_order_release);
        stream->closed = 0;
        pthread_mutex_unlock(&stream->lock);
        slot = i;
        break;
    }
    pthread_mutex_unlock(&g_stream_lock);

    if (slot == STREAM_LIMIT) {
        wc3_legacy_guest_free(record);
        return 0;
    }

    wc3_legacy_wr32g(record, STREAM_MAGIC);
    wc3_legacy_wr32g(record + 4u, slot);
    return record;
}

/* The caller holds stream->lock.  Closing is terminal even when the host
 * close reports an error, matching fclose's stream-closing contract.  The
 * record magic is cleared before the slot is retired, so stream_acquire()
 * cannot pair the old record with a later occupant of this slot. */
static void stream_finish(Stream *stream, uint32_t record)
{
    stream->closed = 1;
    if (record && wc3_legacy_guest_ok(record, 4u))
        wc3_legacy_wr32g(record, 0u);
    pthread_mutex_unlock(&stream->lock);

    pthread_mutex_lock(&g_stream_lock);
    stream->used = 0;
    pthread_mutex_unlock(&g_stream_lock);
}

#if WC3_DEBUG && WC3_DEBUG_SAVE
static int save_trace_path(const char *path);
#endif

static int stream_read(Stream *stream, void *buffer, uint32_t count,
                       uint32_t *read_out)
{
    if (count == 0u) {
        if (read_out) *read_out = 0u;
        return 0;
    }
    if (stream->std_index >= 0) {
        FILE *file = std_file(stream->std_index);
        if (!file) return -1;
        const size_t got = fread(buffer, 1u, count, file);
        if (read_out) *read_out = (uint32_t)got;
        if (got < count) {
            if (ferror(file))
                stream->error = 1;
            else if (feof(file))
                stream->eof = 1;
        }
        return ferror(file) ? -1 : 0;
    }

#if WC3_DEBUG && WC3_DEBUG_SAVE
    const int trace_save = save_trace_path(stream->save_path);
    static unsigned save_trace_reads;
    const int trace_event = trace_save && save_trace_reads++ < 120;
#endif
    uint32_t got = 0;
    if (wc3_hle_msvcr80_fd_read(stream->fd, buffer, count, &got) != 0) {
        stream->error = 1;
#if WC3_DEBUG && WC3_DEBUG_SAVE
        if (trace_event) stream_trace_event(count, got, 0);
#endif
        return -1;
    }
#if WC3_DEBUG && WC3_DEBUG_SAVE
    if (trace_event) stream_trace_event(count, got, 1);
#endif
    if (read_out) *read_out = got;
    if (got < count) stream->eof = 1;
    return 0;
}

static int stream_write(Stream *stream, const void *buffer, uint32_t count,
                        uint32_t *written_out)
{
    if (count == 0u) {
        if (written_out) *written_out = 0u;
        return 0;
    }
    if (stream->std_index >= 0) {
        FILE *file = std_file(stream->std_index);
        if (!file) return -1;
        const size_t put = fwrite(buffer, 1u, count, file);
        if (written_out) *written_out = (uint32_t)put;
        if (put != count || ferror(file)) {
            stream->error = 1;
            return -1;
        }
        return 0;
    }

#if WC3_DEBUG && WC3_DEBUG_SAVE
    const int trace_save = save_trace_path(stream->save_path);
    static unsigned save_trace_writes;
    const int trace_event = trace_save && save_trace_writes++ < 400;
#endif
    uint32_t put = 0;
    if (wc3_hle_msvcr80_fd_write(stream->fd, buffer, count, &put) != 0 ||
        put != count) {
        stream->error = 1;
#if WC3_DEBUG && WC3_DEBUG_SAVE
        if (trace_event) stream_trace_event(count, put, 0);
#endif
        return -1;
    }
#if WC3_DEBUG && WC3_DEBUG_SAVE
    if (trace_event) stream_trace_event(count, put, 1);
#endif
    if (written_out) *written_out = put;
    return 0;
}

static int stream_seek(Stream *stream, int64_t offset, int whence)
{
    if (stream->std_index >= 0) {
        FILE *file = std_file(stream->std_index);
        if (!file) return -1;
        if (fseek(file, (long)offset, whence) != 0) return -1;
        stream->eof = 0;
        return 0;
    }

    uint64_t position = 0;
    if (wc3_hle_msvcr80_fd_seek(stream->fd, offset, whence, &position) != 0)
        return -1;
    stream->eof = 0;
    return 0;
}

static int stream_tell(Stream *stream, uint64_t *position_out)
{
    if (stream->std_index >= 0) {
        FILE *file = std_file(stream->std_index);
        if (!file) return -1;
        const long position = ftell(file);
        if (position < 0) return -1;
        *position_out = (uint64_t)(unsigned long)position;
        return 0;
    }
    return wc3_hle_msvcr80_fd_seek(stream->fd, 0, 1, position_out);
}

static int stream_flush(Stream *stream)
{
    if (stream->std_index < 0) return 0; /* Wc3FileApi writes are immediate */
    FILE *file = std_file(stream->std_index);
    if (!file) return -1;
    if (fflush(file) != 0) {
        stream->error = 1;
        return -1;
    }
    return 0;
}

/* fflush(NULL) drains the two writable standard streams.  Slots 0..2 have a
 * fixed identity, so the slot is indexed directly; `closed` is revalidated
 * under its mutex so a concurrent fclose(stdout) cannot leave us flushing a
 * closed FILE*. */
static int flush_standard_stream(unsigned slot)
{
    ensure_streams();
    if (slot >= STREAM_LIMIT) return 0;

    Stream *stream = &g_streams[slot];
    pthread_mutex_lock(&stream->lock);
    int result = 0;
    if (!stream->closed) result = stream_flush(stream);
    pthread_mutex_unlock(&stream->lock);
    return result;
}

static int stream_close(Stream *stream)
{
    if (stream->std_index < 0)
        return wc3_hle_msvcr80_fd_close(stream->fd);

    FILE *file = std_file(stream->std_index);
    if (!file) return -1;
    (void)fflush(file);
    return fclose(file) == 0 ? 0 : -1;
}

/* Parse the Microsoft fopen mode string into Wc3FileApi flags.  The target is
 * byte-oriented, so 't' is recorded for completeness but performs no CRLF
 * translation. */
static int parse_mode(const char *mode, int *flags_out, int *text_out)
{
    if (!mode || !mode[0] || !flags_out || !text_out) return 0;

    int flags = 0;
    switch (mode[0]) {
    case 'r':
        flags = WC3_FILE_READ;
        break;
    case 'w':
        flags = WC3_FILE_WRITE | WC3_FILE_CREATE | WC3_FILE_TRUNCATE;
        break;
    case 'a':
        flags = WC3_FILE_WRITE | WC3_FILE_CREATE | WC3_FILE_APPEND;
        break;
    default:
        return 0;
    }

    int text = 0;
    int plus = 0;
    for (const char *p = mode + 1; *p; p++) {
        switch (*p) {
        case '+':
            if (plus) return 0;
            plus = 1;
            flags |= WC3_FILE_READ | WC3_FILE_WRITE;
            break;
        case 'b':
            text = 0;
            break;
        case 't':
            text = 1;
            break;
        case 'c':
        case 'n':
        case 'S':
        case 'R':
        case 'T':
        case 'D':
            break;
        default:
            return 0;
        }
    }
    *flags_out = flags;
    *text_out = text;
    return 1;
}

#if WC3_DEBUG && WC3_DEBUG_SAVE
static int save_trace_path(const char *path)
{
    return path && (strstr(path, "Save") || strstr(path, ".w3z") ||
                    strstr(path, ".w3v") || strstr(path, ".w3p"));
}

static void save_trace_note(uint32_t record, const char *path)
{
    if (!record || !path) return;
    const uint32_t slot = wc3_legacy_rd32g(record + 4u);
    if (slot >= STREAM_LIMIT) return;
    pthread_mutex_lock(&g_stream_lock);
    snprintf(g_streams[slot].save_path, STREAM_PATH_MAX, "%s", path);
    pthread_mutex_unlock(&g_stream_lock);
}
#endif

static uint32_t open_stream(const char *guest_path, const char *mode)
{
    int flags = 0;
    int text = 0;
    if (!parse_mode(mode, &flags, &text)) return 0;

    char host_path[STREAM_PATH_MAX];
    wc3_hle_msvcr80_map_path(guest_path, host_path, sizeof host_path);

#if WC3_DEBUG && WC3_DEBUG_SAVE
    const int trace_save = save_trace_path(host_path);
    if (trace_save)
        fprintf(stderr, "[savecrt] open guest=\"%s\" host=\"%s\" mode=%s\n",
                guest_path ? guest_path : "", host_path, mode ? mode : "");
#endif
    int fd = -1;
    if (wc3_hle_msvcr80_fd_open(host_path, flags, &fd) != 0) {
#if WC3_DEBUG && WC3_DEBUG_SAVE
        if (trace_save) fprintf(stderr, "[savecrt] open failed host=\"%s\"\n", host_path);
#endif
        return 0;
    }

    const uint32_t record = stream_new(fd, -1, text ? CRT_O_TEXT : CRT_O_BINARY);
    if (!record) (void)wc3_hle_msvcr80_fd_close(fd);
#if WC3_DEBUG && WC3_DEBUG_SAVE
    else save_trace_note(record, host_path);
    if (trace_save) fprintf(stderr, "[savecrt] open fd=%d record=%08X\n", fd, record);
#endif
    return record;
}

static int read_wide_string(uint32_t address, uint16_t *buffer, size_t capacity)
{
    if (!address || !buffer || capacity < 2u) return 0;
    for (size_t i = 0; i + 1u < capacity; ) {
        if (address > UINT32_MAX - (uint32_t)(i * 2u)) return 0;
        const uint32_t at = address + (uint32_t)(i * 2u);
        const size_t remaining = capacity - 1u - i;
        uint32_t units = remaining > 32u ? 32u : (uint32_t)remaining;
        const uint64_t room = (0x100000000ull - at) / 2u;
        if (units > room) units = (uint32_t)room;
        if (units == 0u) return 0;

        if (wc3_legacy_guest_ok(at, units * 2u)) {
            for (uint32_t j = 0; j < units; j++) {
                const uint16_t ch = wc3_legacy_rd16g(at + j * 2u);
                buffer[i++] = ch;
                if (!ch) return 1;
            }
        } else {
            /* A chunk that crosses a page or arena boundary keeps the old
             * all-or-nothing result: walk its wide units one at a time. */
            for (uint32_t j = 0; j < units; j++) {
                const uint32_t unit = at + j * 2u;
                if (!wc3_legacy_guest_ok(unit, 2u)) return 0;
                const uint16_t ch = wc3_legacy_rd16g(unit);
                buffer[i++] = ch;
                if (!ch) return 1;
            }
        }
    }
    return 0;
}

static int wide_to_utf8(const uint16_t *source, char *destination,
                        size_t capacity)
{
    const Wc3Platform *platform = wc3_platform();
    if (!platform || !platform->locale.from_wide || capacity == 0u) return 0;
    const int written = platform->locale.from_wide(
        platform->locale.ctx, 65001u, source, -1, destination,
        (int32_t)capacity);
    if (written < 0 || (size_t)written >= capacity) return 0;
    destination[written] = '\0';
    return 1;
}

static int read_guest_cstring(uint32_t address, char *buffer, size_t capacity)
{
    return wc3_hle_msvcr80_read_guest_path(address, buffer, capacity);
}

static int format_wide_string(uint32_t address, char *buffer, size_t capacity)
{
    uint16_t wide[512];
    if (!read_wide_string(address, wide, sizeof wide / sizeof wide[0]))
        return 0;
    if (wide_to_utf8(wide, buffer, capacity)) return 1;

    /* A missing locale seam still leaves ASCII readable. */
    size_t i = 0;
    while (wide[i] && i + 1u < capacity) {
        buffer[i] = wide[i] <= 0x7Fu ? (char)wide[i] : '?';
        i++;
    }
    buffer[i] = '\0';
    return 1;
}

/* Format the guest's x86 cdecl vararg block.  The layout is the one the
 * as-built guest_format() established: ap points at the first vararg, and
 * every argument occupies four bytes except doubles and explicitly 64-bit
 * integers.  The supported conversions cover the formats the lifted game
 * actually uses (all game.dll printf sites are "... = %d") plus the common
 * CRT forms needed by diagnostics. */
static size_t format_guest(char *out, size_t cap, const char *fmt,
                           uint32_t ap)
{
    size_t o = 0;
    if (cap) out[0] = '\0';

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            if (cap && o + 1u < cap) out[o] = *p;
            o++;
            continue;
        }
        p++;
        if (*p == '%') {
            if (cap && o + 1u < cap) out[o] = '%';
            o++;
            continue;
        }

        char spec[32];
        size_t s = 0;
        int wide = 0;
        int i64 = 0;
        spec[s++] = '%';
        while (*p && strchr("-+ #0", *p) && s < 10u) spec[s++] = *p++;
        while (*p && isdigit((unsigned char)*p) && s < 14u) spec[s++] = *p++;
        if (*p == '.') {
            spec[s++] = *p++;
            while (*p && isdigit((unsigned char)*p) && s < 18u)
                spec[s++] = *p++;
        }
        for (;;) {
            if (*p == 'l') {
                wide = 1;
                p++;
                if (*p == 'l') {
                    i64 = 1;
                    p++;
                }
            } else if (*p == 'h') {
                p++;
                if (*p == 'h') p++;
            } else if (*p == 'L') {
                i64 = 1;
                p++;
            } else if (*p == 'I' && p[1] == '6' && p[2] == '4') {
                i64 = 1;
                p += 3;
            } else if (*p == 'I' && p[1] == '3' && p[2] == '2') {
                p += 3;
            } else if (*p == 'z' || *p == 'I') {
                p++;
            } else {
                break;
            }
        }

        char conv = *p;
        if (!conv) break;
        if (conv == 'S') {
            conv = 's';
            wide = 1;
        }

        char tmp[1024];
        tmp[0] = '\0';
        switch (conv) {
        case 's': {
            char text[1024];
            text[0] = '\0';
            if (wide)
                (void)format_wide_string(wc3_legacy_rd32g(ap), text,
                                         sizeof text);
            else
                (void)read_guest_cstring(wc3_legacy_rd32g(ap), text,
                                         sizeof text);
            spec[s] = 's';
            spec[s + 1u] = '\0';
            snprintf(tmp, sizeof tmp, spec, text);
            ap += 4u;
            break;
        }
        case 'c':
            spec[s] = 'c';
            spec[s + 1u] = '\0';
            snprintf(tmp, sizeof tmp, spec, (int)wc3_legacy_rd32g(ap));
            ap += 4u;
            break;
        case 'd':
        case 'i':
            spec[s] = 'l';
            spec[s + 1u] = 'l';
            spec[s + 2u] = 'd';
            spec[s + 3u] = '\0';
            if (i64) {
                const uint64_t v = (uint64_t)wc3_legacy_rd32g(ap) |
                                   ((uint64_t)wc3_legacy_rd32g(ap + 4u) << 32);
                snprintf(tmp, sizeof tmp, spec, (long long)(int64_t)v);
                ap += 8u;
            } else {
                snprintf(tmp, sizeof tmp, spec,
                         (long long)(int32_t)wc3_legacy_rd32g(ap));
                ap += 4u;
            }
            break;
        case 'u':
        case 'x':
        case 'X':
        case 'o':
            spec[s] = 'l';
            spec[s + 1u] = 'l';
            spec[s + 2u] = conv;
            spec[s + 3u] = '\0';
            if (i64) {
                const uint64_t v = (uint64_t)wc3_legacy_rd32g(ap) |
                                   ((uint64_t)wc3_legacy_rd32g(ap + 4u) << 32);
                snprintf(tmp, sizeof tmp, spec, (unsigned long long)v);
                ap += 8u;
            } else {
                snprintf(tmp, sizeof tmp, spec,
                         (unsigned long long)wc3_legacy_rd32g(ap));
                ap += 4u;
            }
            break;
        case 'p':
            snprintf(tmp, sizeof tmp, "%08X", wc3_legacy_rd32g(ap));
            ap += 4u;
            break;
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'e':
        case 'E':
        case 'a':
        case 'A': {
            const uint64_t bits = (uint64_t)wc3_legacy_rd32g(ap) |
                                  ((uint64_t)wc3_legacy_rd32g(ap + 4u) << 32);
            double value;
            memcpy(&value, &bits, sizeof value);
            spec[s] = conv;
            spec[s + 1u] = '\0';
            snprintf(tmp, sizeof tmp, spec, value);
            ap += 8u;
            break;
        }
        case 'n': {
            const uint32_t pointer = wc3_legacy_rd32g(ap);
            if (pointer && wc3_legacy_guest_ok(pointer, 4u))
                wc3_legacy_wr32g(pointer, (uint32_t)o);
            ap += 4u;
            continue;
        }
        default:
            continue;
        }

        const size_t length = strlen(tmp);
        if (cap) {
            const size_t room = (o + length < cap)
                                    ? length
                                    : (o < cap - 1u ? cap - 1u - o : 0u);
            memcpy(out + o, tmp, room);
        }
        o += length;
    }
    if (cap) out[o < cap ? o : cap - 1u] = '\0';
    return o;
}

static int write_stdout(const char *buffer, size_t length)
{
    if (length == 0u) return 0;
    if (length > 0x7FFFFFFFu) return -1;
    Stream *stream = stream_by_fd_acquire(1);
    if (!stream) return -1;
#if WC3_DEBUG && WC3_DEBUG_SAVE
    StreamTrace trace;
    stream_trace_begin(&trace, stream);
#endif
    uint32_t put = 0;
    const int result =
        stream_write(stream, buffer, (uint32_t)length, &put);
    pthread_mutex_unlock(&stream->lock);
#if WC3_DEBUG && WC3_DEBUG_SAVE
    stream_trace_flush(&trace, 1);
#endif
    return result == 0 && put == length ? 0 : -1;
}

static void h_printf(CPU *c)
{
    char format[1024];
    c->eax = 0xFFFFFFFFu;
    if (!read_guest_cstring(wc3_legacy_arg32(c, 0u), format,
                            sizeof format))
        return;

    char output[4096];
    const size_t needed = format_guest(output, sizeof output, format,
                                       c->esp + 8u);
    const size_t written =
        needed < sizeof output ? needed : sizeof output - 1u;
    if (write_stdout(output, written) != 0) return;
    c->eax = (uint32_t)needed;
}

static void h_fopen(CPU *c)
{
    char path[STREAM_PATH_MAX];
    char mode[STREAM_MODE_MAX];

    c->eax = 0u;
    if (!wc3_hle_msvcr80_read_guest_path(wc3_legacy_arg32(c, 0u), path,
                                         sizeof path))
        return;
    if (!wc3_hle_msvcr80_read_guest_path(wc3_legacy_arg32(c, 1u), mode,
                                         sizeof mode))
        return;
    c->eax = open_stream(path, mode);
}

static void h_fopen_s(CPU *c)
{
    const uint32_t out_pointer = wc3_legacy_arg32(c, 0u);
    char path[STREAM_PATH_MAX];
    char mode[STREAM_MODE_MAX];
    uint32_t record = 0;

    if (wc3_hle_msvcr80_read_guest_path(wc3_legacy_arg32(c, 1u), path,
                                        sizeof path) &&
        wc3_hle_msvcr80_read_guest_path(wc3_legacy_arg32(c, 2u), mode,
                                        sizeof mode))
        record = open_stream(path, mode);

    if (out_pointer && wc3_legacy_guest_ok(out_pointer, 4u))
        wc3_legacy_wr32g(out_pointer, record);
    c->eax = record ? 0u : 2u; /* ENOENT is the as-built fallback */
}

static void h_fclose(CPU *c)
{
    const uint32_t record = wc3_legacy_arg32(c, 0u);
    Stream *stream = stream_acquire(record);
    if (!stream) {
        c->eax = 0xFFFFFFFFu;
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_SAVE
    const int trace_close =
        stream->std_index < 0 && save_trace_path(stream->save_path);
    const int trace_fd = stream->fd;
    char trace_path[STREAM_PATH_MAX];
    if (trace_close) memcpy(trace_path, stream->save_path,
                             sizeof trace_path);
#endif
    const int result = stream_close(stream);
    stream_finish(stream, record);
#if WC3_DEBUG && WC3_DEBUG_SAVE
    if (trace_close)
        fprintf(stderr, "[savecrt] close fd=%d path=\"%s\"\n",
                trace_fd, trace_path);
#endif
    c->eax = result == 0 ? 0u : 0xFFFFFFFFu;
}

static void h_fread(CPU *c)
{
    const uint32_t buffer = wc3_legacy_arg32(c, 0u);
    const uint32_t size = wc3_legacy_arg32(c, 1u);
    const uint32_t count = wc3_legacy_arg32(c, 2u);
    const uint32_t record = wc3_legacy_arg32(c, 3u);

    c->eax = 0u;
    if (!size || !count) return;
    const uint64_t total = (uint64_t)size * count;
    if (total > 0x7FFFFFFFu) return;
    if (!wc3_legacy_guest_ok(buffer, (uint32_t)total)) return;

    Stream *stream = stream_acquire(record);
    if (!stream) return;
#if WC3_DEBUG && WC3_DEBUG_SAVE
    StreamTrace trace;
    stream_trace_begin(&trace, stream);
#endif
    uint32_t got = 0;
    if (stream_read(stream, (void *)(uintptr_t)buffer, (uint32_t)total,
                    &got) == 0)
        c->eax = got / size;
    pthread_mutex_unlock(&stream->lock);
#if WC3_DEBUG && WC3_DEBUG_SAVE
    stream_trace_flush(&trace, 0);
#endif
}

static void h_fwrite(CPU *c)
{
    const uint32_t buffer = wc3_legacy_arg32(c, 0u);
    const uint32_t size = wc3_legacy_arg32(c, 1u);
    const uint32_t count = wc3_legacy_arg32(c, 2u);
    const uint32_t record = wc3_legacy_arg32(c, 3u);

    c->eax = 0u;
    if (!size || !count) return;
    const uint64_t total = (uint64_t)size * count;
    if (total > 0x7FFFFFFFu) return;
    if (!wc3_legacy_guest_ok(buffer, (uint32_t)total)) return;

    Stream *stream = stream_acquire(record);
    if (!stream) return;
#if WC3_DEBUG && WC3_DEBUG_SAVE
    StreamTrace trace;
    stream_trace_begin(&trace, stream);
#endif
    uint32_t put = 0;
    if (stream_write(stream, (const void *)(uintptr_t)buffer,
                     (uint32_t)total, &put) == 0)
        c->eax = put / size;
    pthread_mutex_unlock(&stream->lock);
#if WC3_DEBUG && WC3_DEBUG_SAVE
    stream_trace_flush(&trace, 1);
#endif
}

static void h_fseek(CPU *c)
{
    const uint32_t record = wc3_legacy_arg32(c, 0u);
    const int32_t offset = (int32_t)wc3_legacy_arg32(c, 1u);
    const int whence = (int)wc3_legacy_arg32(c, 2u);

    c->eax = 0xFFFFFFFFu;
    Stream *stream = stream_acquire(record);
    if (!stream) return;
    if (stream_seek(stream, offset, whence) == 0) c->eax = 0u;
    pthread_mutex_unlock(&stream->lock);
}

static void h_ftell(CPU *c)
{
    c->eax = 0xFFFFFFFFu;
    Stream *stream = stream_acquire(wc3_legacy_arg32(c, 0u));
    if (!stream) return;
    uint64_t position = 0;
    if (stream_tell(stream, &position) == 0) c->eax = (uint32_t)position;
    pthread_mutex_unlock(&stream->lock);
}

static void h_fflush(CPU *c)
{
    const uint32_t record = wc3_legacy_arg32(c, 0u);
    if (!record) {
        /* File streams are unbuffered; only the console streams have host
         * buffers to drain. */
        int result = 0;
        if (flush_standard_stream(1u) != 0) result = -1;
        if (flush_standard_stream(2u) != 0) result = -1;
        c->eax = result == 0 ? 0u : 0xFFFFFFFFu;
        return;
    }

    c->eax = 0xFFFFFFFFu;
    Stream *stream = stream_acquire(record);
    if (!stream) return;
    if (stream_flush(stream) == 0) c->eax = 0u;
    pthread_mutex_unlock(&stream->lock);
}

static void h_feof(CPU *c)
{
    c->eax = 0u;
    Stream *stream = stream_acquire(wc3_legacy_arg32(c, 0u));
    if (!stream) return;
    c->eax = stream->eof ? 1u : 0u;
    pthread_mutex_unlock(&stream->lock);
}

static void h_ferror(CPU *c)
{
    c->eax = 0u;
    Stream *stream = stream_acquire(wc3_legacy_arg32(c, 0u));
    if (!stream) return;
    c->eax = stream->error ? 1u : 0u;
    pthread_mutex_unlock(&stream->lock);
}

static void h_putc(CPU *c)
{
    const int ch = (int)wc3_legacy_arg32(c, 0u);
    const uint32_t record = wc3_legacy_arg32(c, 1u);

    c->eax = 0xFFFFFFFFu;
    Stream *stream = stream_acquire(record);
    if (!stream) return;
#if WC3_DEBUG && WC3_DEBUG_SAVE
    StreamTrace trace;
    stream_trace_begin(&trace, stream);
#endif
    const unsigned char byte = (unsigned char)ch;
    uint32_t put = 0;
    if (stream_write(stream, &byte, 1u, &put) == 0 && put == 1u)
        c->eax = (uint32_t)byte;
    pthread_mutex_unlock(&stream->lock);
#if WC3_DEBUG && WC3_DEBUG_SAVE
    stream_trace_flush(&trace, 1);
#endif
}

static void h_fputc(CPU *c)
{
    h_putc(c);
}

static void h_fputs(CPU *c)
{
    const uint32_t string = wc3_legacy_arg32(c, 0u);
    const uint32_t record = wc3_legacy_arg32(c, 1u);
    char chunk[256];
    size_t used = 0;
    uint32_t at = 0;

    c->eax = 0xFFFFFFFFu;
    if (!string) return;
    Stream *stream = stream_acquire(record);
    if (!stream) return;
#if WC3_DEBUG && WC3_DEBUG_SAVE
    StreamTrace trace;
    stream_trace_begin(&trace, stream);
#endif

    for (;;) {
        if (at >= STREAM_STRING_LIMIT || at > UINT32_MAX - string)
            goto done;
        const uint32_t address = string + at;
        if (!wc3_legacy_guest_ok(address, 1u)) goto done;
        const char ch = (char)wc3_legacy_rd8g(address);
        if (!ch) break;
        chunk[used++] = ch;
        at++;
        if (used == sizeof chunk) {
            uint32_t put = 0;
            if (stream_write(stream, chunk, (uint32_t)used, &put) != 0 ||
                put != used)
                goto done;
            used = 0;
        }
    }
    if (used) {
        uint32_t put = 0;
        if (stream_write(stream, chunk, (uint32_t)used, &put) != 0 ||
            put != used)
            goto done;
    }
    c->eax = 0u;
done:
    pthread_mutex_unlock(&stream->lock);
#if WC3_DEBUG && WC3_DEBUG_SAVE
    stream_trace_flush(&trace, 1);
#endif
}

static void h_fgets(CPU *c)
{
    const uint32_t string = wc3_legacy_arg32(c, 0u);
    const int count = (int)wc3_legacy_arg32(c, 1u);
    const uint32_t record = wc3_legacy_arg32(c, 2u);

    c->eax = 0u;
    if (!string || count <= 0 ||
        !wc3_legacy_guest_ok(string, (uint32_t)count))
        return;
    if (count == 1) {
        wc3_legacy_wr8g(string, 0u);
        c->eax = string;
        return;
    }

    Stream *stream = stream_acquire(record);
    if (!stream) return;
#if WC3_DEBUG && WC3_DEBUG_SAVE
    StreamTrace trace;
    stream_trace_begin(&trace, stream);
#endif

    int at = 0;
    int failed = 0;
    for (; at < count - 1; at++) {
        unsigned char ch = 0;
        uint32_t got = 0;
        if (stream_read(stream, &ch, 1u, &got) != 0) {
            failed = 1;
            break;
        }
        if (got == 0) {
            if (at == 0) goto done;
            break;
        }
        wc3_legacy_wr8g(string + (uint32_t)at, ch);
        if (ch == '\n') {
            at++;
            break;
        }
    }
    wc3_legacy_wr8g(string + (uint32_t)at, 0u);
    if (!failed) c->eax = string;
done:
    pthread_mutex_unlock(&stream->lock);
#if WC3_DEBUG && WC3_DEBUG_SAVE
    stream_trace_flush(&trace, 0);
#endif
}

static void h_fileno(CPU *c)
{
    c->eax = 0xFFFFFFFFu;
    Stream *stream = stream_acquire(wc3_legacy_arg32(c, 0u));
    if (!stream) return;
    c->eax = stream->std_index >= 0 ? (uint32_t)stream->std_index
                                    : (uint32_t)stream->fd;
    pthread_mutex_unlock(&stream->lock);
}

static void h_setmode(CPU *c)
{
    const int fd = (int)wc3_legacy_arg32(c, 0u);
    const int mode = (int)wc3_legacy_arg32(c, 1u);
    if (mode != CRT_O_TEXT && mode != CRT_O_BINARY && mode != CRT_O_WTEXT &&
        mode != CRT_O_U16TEXT && mode != CRT_O_U8TEXT) {
        c->eax = 0xFFFFFFFFu;
        return;
    }

    c->eax = 0xFFFFFFFFu;
    Stream *stream = stream_by_fd_acquire(fd);
    if (!stream) return;
    c->eax = (uint32_t)stream->mode;
    stream->mode = mode;
    pthread_mutex_unlock(&stream->lock);
}

static void h_fdopen(CPU *c)
{
    const int fd = (int)wc3_legacy_arg32(c, 0u);
    char mode[STREAM_MODE_MAX];
    int flags = 0;
    int text = 0;

    c->eax = 0u;
    if (fd < 0) return;
    if (!wc3_hle_msvcr80_read_guest_path(wc3_legacy_arg32(c, 1u), mode,
                                         sizeof mode))
        return;
    if (!parse_mode(mode, &flags, &text)) return;
    (void)flags;

    if (fd >= 3) {
        uint64_t position = 0;
        if (wc3_hle_msvcr80_fd_seek(fd, 0, 1, &position) != 0) return;
        if (stream_fd_in_use(fd)) return;
    }
    c->eax = stream_new(fd, fd < 3 ? fd : -1,
                        text ? CRT_O_TEXT : CRT_O_BINARY);
}

static void h_wfopen(CPU *c)
{
    uint16_t wide_path[STREAM_WIDE_MAX];
    uint16_t wide_mode[STREAM_MODE_MAX];
    char path[STREAM_PATH_MAX];
    char mode[STREAM_MODE_MAX];

    c->eax = 0u;
    if (!read_wide_string(wc3_legacy_arg32(c, 0u), wide_path,
                          STREAM_WIDE_MAX))
        return;
    if (!read_wide_string(wc3_legacy_arg32(c, 1u), wide_mode,
                          STREAM_MODE_MAX))
        return;
    if (!wide_to_utf8(wide_path, path, sizeof path)) return;
    if (!wide_to_utf8(wide_mode, mode, sizeof mode)) return;
    c->eax = open_stream(path, mode);
}

static const struct {
    const char *name;
    void (*impl)(CPU *c);
} g_overrides[] = {
    { "fopen", h_fopen },
    { "fopen_s", h_fopen_s },
    { "fclose", h_fclose },
    { "fread", h_fread },
    { "fwrite", h_fwrite },
    { "fseek", h_fseek },
    { "ftell", h_ftell },
    { "fflush", h_fflush },
    { "feof", h_feof },
    { "ferror", h_ferror },
};

static const struct {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} g_additions[] = {
    { "putc", 2u, h_putc },
    { "fputc", 2u, h_fputc },
    { "fputs", 2u, h_fputs },
    { "fgets", 3u, h_fgets },
    { "_fileno", 1u, h_fileno },
    { "_wfopen", 2u, h_wfopen },
    { "_setmode", 2u, h_setmode },
    { "_fdopen", 2u, h_fdopen },
    { "printf", 1u, h_printf },
};

int wc3_hle_msvcr80_stdio_install(void)
{
    ensure_streams();
    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("MSVCR80.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("MSVCR80.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 1,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0] +
                 sizeof g_additions / sizeof g_additions[0]);
}
