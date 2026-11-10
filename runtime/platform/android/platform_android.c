/*
 * platform_android.c - Android arm64 implementation of the OS boundary.
 *
 * This file is compiled only for the Android target.  It deliberately has no
 * JNI/GameActivity dependency: the activity-owned pieces (window, surface,
 * input, audio, clipboard, Vulkan ICD) are installed by the app layer, while
 * the core services below are usable by the guest runtime and by the
 * device-side platform test.
 *
 * Ownership and locking:
 *   - thread/TLS state is per host thread;
 *   - sync-object state is protected by one platform mutex; single-object
 *     waits sleep on a per-object condition, multi-object waits on the
 *     platform condition, and no such lock is held across a guest callback;
 *   - file and memory calls do not take a host lock over guest execution.
 */
#define _GNU_SOURCE  1

#ifndef WC3_DEBUG
#define WC3_DEBUG 0
#endif

#include "platform_android.h"
#include "asset_snapshot.h"
#include "audio_mixer.h"
#if WC3_DEBUG && WC3_DEBUG_AUDIO
#include "audio_tap.h"
#endif

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <sys/syscall.h>
#endif

#if defined(__ANDROID__) && defined(__ANDROID_API__) && __ANDROID_API__ < 28
/* bionic only declares this API 28 entry point for __ANDROID_API__ >= 28.
 * P0-9 keeps it optional: on older devices the weak reference resolves to
 * NULL and android_sync_init() falls back to a normal mutex. */
extern int pthread_mutexattr_setprotocol(pthread_mutexattr_t *, int)
    __attribute__((weak));
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define WC3_PLATFORM_TLS_MAX 64u
#define WC3_PLATFORM_WAIT_MAX 64u
#define WC3_PLATFORM_PATH_MAX 1024u
#define WC3_SYNC_MAGIC 0x57433353u  /* "WC3S" */

/* ------------------------------------------------------------------ memory */

static void *android_map_fixed(uint32_t base, uint32_t size, int prot)
{
    if (size == 0u || (uint64_t)base + size > 0x100000000ull) {
        errno = EINVAL;
        return MAP_FAILED;
    }

#ifdef MAP_FIXED_NOREPLACE
    void *p = mmap((void *)(uintptr_t)base, (size_t)size, prot,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
                       MAP_FIXED_NOREPLACE,
                   -1, 0);
    if (p != MAP_FAILED) return p;
    if (errno != EEXIST && errno != EINVAL) return MAP_FAILED;
#endif

    /* Older Android kernels do not know MAP_FIXED_NOREPLACE.  A hint plus a
     * returned-address check is safe because the guest layout is installed
     * before any competing mapping is allowed in that range. */
    void *hinted = mmap((void *)(uintptr_t)base, (size_t)size, prot,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (hinted == MAP_FAILED) return MAP_FAILED;
    if ((uintptr_t)hinted != (uintptr_t)base) {
        munmap(hinted, (size_t)size);
        errno = EEXIST;
        return MAP_FAILED;
    }
    return hinted;
}

static int android_mem_reserve(void *ctx, uint32_t base, uint32_t size)
{
    (void)ctx;
    void *p = android_map_fixed(base, size, PROT_NONE);
    return p == MAP_FAILED ? -1 : 0;
}

static int android_mem_commit(void *ctx, uint32_t base, uint32_t size)
{
    (void)ctx;
    if (size == 0u) return -1;
    return mprotect((void *)(uintptr_t)base, (size_t)size,
                    PROT_READ | PROT_WRITE) == 0
               ? 0
               : -1;
}

static int android_mem_decommit(void *ctx, uint32_t base, uint32_t size)
{
    (void)ctx;
    if (size == 0u) return -1;
#ifdef MADV_DONTNEED
    (void)madvise((void *)(uintptr_t)base, (size_t)size, MADV_DONTNEED);
#endif
    return mprotect((void *)(uintptr_t)base, (size_t)size, PROT_NONE) == 0
               ? 0
               : -1;
}

static int android_mem_release(void *ctx, uint32_t base, uint32_t size)
{
    (void)ctx;
    if (size == 0u) return -1;
    return munmap((void *)(uintptr_t)base, (size_t)size) == 0 ? 0 : -1;
}

static int android_mem_protect(void *ctx, uint32_t base, uint32_t size,
                               uint32_t prot)
{
    (void)ctx;
    int p = PROT_NONE;
    if (prot & WC3_MEM_READ) p |= PROT_READ;
    if (prot & WC3_MEM_WRITE) p |= PROT_WRITE;
    if (prot & WC3_MEM_EXEC) p |= PROT_EXEC;
    return mprotect((void *)(uintptr_t)base, (size_t)size, p) == 0 ? 0 : -1;
}

/* -------------------------------------------------------------------- file */

static uintptr_t android_fd_handle(int fd)
{
    return (uintptr_t)(uint32_t)(fd + 1);
}

static int android_fd_from_handle(uintptr_t h)
{
    uint32_t v = (uint32_t)h;
    return v == 0u ? -1 : (int)(v - 1u);
}

static int android_file_open(void *ctx, const char *path, int flags,
                             uintptr_t *out)
{
    (void)ctx;
    if (!path || !out) return -1;

    int oflags = 0;
    int access = flags & (WC3_FILE_READ | WC3_FILE_WRITE);
    if (access == (WC3_FILE_READ | WC3_FILE_WRITE)) {
        oflags = O_RDWR;
    } else if (access == WC3_FILE_WRITE) {
        oflags = O_WRONLY;
    } else if (access == WC3_FILE_READ) {
        oflags = O_RDONLY;
    } else {
        errno = EINVAL;
        return -1;
    }
    if (flags & WC3_FILE_CREATE) oflags |= O_CREAT;
    if (flags & WC3_FILE_TRUNCATE) oflags |= O_TRUNC;
    if (flags & WC3_FILE_APPEND) oflags |= O_APPEND;
    if (flags & WC3_FILE_EXCLUSIVE) oflags |= O_EXCL;
    oflags |= O_CLOEXEC;

    int fd = open(path, oflags, 0600);
    if (fd < 0) return -1;
    *out = android_fd_handle(fd);
    return 0;
}

static int android_file_read(void *ctx, uintptr_t file, void *buf,
                             uint32_t len, uint32_t *read_out)
{
    (void)ctx;
    int fd = android_fd_from_handle(file);
    if (fd < 0 || (!buf && len != 0u)) return -1;
    ssize_t n;
    do {
        n = read(fd, buf, (size_t)len);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return -1;
    if (read_out) *read_out = (uint32_t)n;
    return 0;
}

static int android_file_write(void *ctx, uintptr_t file, const void *buf,
                              uint32_t len, uint32_t *written_out)
{
    (void)ctx;
    int fd = android_fd_from_handle(file);
    if (fd < 0 || (!buf && len != 0u)) return -1;
    ssize_t n;
    do {
        n = write(fd, buf, (size_t)len);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return -1;
    if (written_out) *written_out = (uint32_t)n;
    return 0;
}

static int android_file_seek(void *ctx, uintptr_t file, int64_t offset,
                             int whence, uint64_t *pos_out)
{
    (void)ctx;
    int fd = android_fd_from_handle(file);
    if (fd < 0) return -1;
    int w = (whence == 0) ? SEEK_SET : (whence == 1) ? SEEK_CUR
              : (whence == 2) ? SEEK_END
                               : -1;
    if (w < 0) return -1;
    off_t pos = lseek(fd, (off_t)offset, w);
    if (pos < 0) return -1;
    if (pos_out) *pos_out = (uint64_t)pos;
    return 0;
}

static int android_file_size(void *ctx, uintptr_t file, uint64_t *size_out)
{
    (void)ctx;
    int fd = android_fd_from_handle(file);
    if (fd < 0 || !size_out) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    *size_out = (uint64_t)st.st_size;
    return 0;
}

static int android_file_stat(void *ctx, const char *path, Wc3FileStat *out)
{
    (void)ctx;
    if (!path || !out) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    out->size = (uint64_t)st.st_size;
    out->mtime_seconds = (int64_t)st.st_mtime;
    out->mode = S_ISDIR(st.st_mode) ? WC3_FILE_STAT_DIRECTORY
                                    : WC3_FILE_STAT_REGULAR;
    return 0;
}

static int android_file_close(void *ctx, uintptr_t file)
{
    (void)ctx;
    int fd = android_fd_from_handle(file);
    if (fd < 0) return -1;
    return close(fd) == 0 ? 0 : -1;
}

/* -------------------------------------------------------------------- time */

static uint64_t android_clock_ns(clockid_t id)
{
    struct timespec ts;
    if (clock_gettime(id, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t android_monotonic_ns(void *ctx)
{
    (void)ctx;
    return android_clock_ns(CLOCK_MONOTONIC);
}

static uint64_t android_realtime_ns(void *ctx)
{
    (void)ctx;
    return android_clock_ns(CLOCK_REALTIME);
}

static int android_local_time(void *ctx, uint64_t unix_ns, int32_t *year,
                              int32_t *month, int32_t *day, int32_t *hour,
                              int32_t *minute, int32_t *second,
                              int32_t *millisecond)
{
    (void)ctx;
    if (!year || !month || !day || !hour || !minute || !second ||
        !millisecond)
        return -1;
    time_t sec = (time_t)(unix_ns / 1000000000ull);
    struct tm tm;
    if (!localtime_r(&sec, &tm)) return -1;
    *year = tm.tm_year + 1900;
    *month = tm.tm_mon + 1;
    *day = tm.tm_mday;
    *hour = tm.tm_hour;
    *minute = tm.tm_min;
    *second = tm.tm_sec;
    *millisecond = (int32_t)((unix_ns % 1000000000ull) / 1000000ull);
    return 0;
}

/* ------------------------------------------------------------------ thread */

typedef struct AndroidThread {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int (*entry)(void *arg);
    void *arg;
    uint32_t refs;
    int detached;
    int running;
    int exit_code;
    uint64_t id;
    char name[16];
} AndroidThread;

static pthread_key_t g_thread_key;
static pthread_once_t g_thread_once = PTHREAD_ONCE_INIT;
static AndroidThread g_main_thread;

static uint64_t android_tid(void)
{
#ifdef __ANDROID__
    return (uint64_t)gettid();
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}

static void android_thread_key_init(void)
{
    (void)pthread_key_create(&g_thread_key, NULL);
}

static void android_thread_release(AndroidThread *t)
{
    pthread_cond_destroy(&t->cond);
    pthread_mutex_destroy(&t->lock);
    free(t);
}

static void *android_thread_main(void *opaque)
{
    AndroidThread *t = (AndroidThread *)opaque;
    (void)pthread_setspecific(g_thread_key, t);
#ifdef __ANDROID__
    if (t->name[0]) (void)pthread_setname_np(pthread_self(), t->name);
#endif
    t->id = android_tid();

    int rc = t->entry(t->arg);

    (void)pthread_mutex_lock(&t->lock);
    t->exit_code = rc;
    t->running = 0;
    (void)pthread_cond_broadcast(&t->cond);
    t->refs--;
    int free_it = (t->refs == 0u);
    (void)pthread_mutex_unlock(&t->lock);
    if (free_it) android_thread_release(t);
    return NULL;
}

static int android_thread_create(void *ctx, const Wc3ThreadDesc *desc,
                                 Wc3ThreadHandle *out)
{
    (void)ctx;
    if (!desc || !desc->entry || !out) return -1;
    (void)pthread_once(&g_thread_once, android_thread_key_init);

    AndroidThread *t = (AndroidThread *)calloc(1, sizeof *t);
    if (!t) return -1;
    if (pthread_mutex_init(&t->lock, NULL) != 0) {
        free(t);
        return -1;
    }
    if (pthread_cond_init(&t->cond, NULL) != 0) {
        pthread_mutex_destroy(&t->lock);
        free(t);
        return -1;
    }
    t->entry = desc->entry;
    t->arg = desc->arg;
    t->refs = 2u; /* caller handle + running thread */
    t->running = 1;
    if (desc->name) {
        size_t n = strlen(desc->name);
        if (n >= sizeof t->name) n = sizeof t->name - 1u;
        memcpy(t->name, desc->name, n);
        t->name[n] = '\0';
    }

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        pthread_cond_destroy(&t->cond);
        pthread_mutex_destroy(&t->lock);
        free(t);
        return -1;
    }
    uint32_t stack_size = desc->stack_size
                              ? desc->stack_size
                              : (uint32_t)WC3_HOST_THREAD_STACK_SIZE;
    if (stack_size >= (uint32_t)PTHREAD_STACK_MIN)
        (void)pthread_attr_setstacksize(&attr, (size_t)stack_size);
    int rc = pthread_create(&t->thread, &attr, android_thread_main, t);
    (void)pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_cond_destroy(&t->cond);
        pthread_mutex_destroy(&t->lock);
        free(t);
        return -1;
    }
    *out = (Wc3ThreadHandle)(uintptr_t)t;
    return 0;
}

static int android_thread_join(void *ctx, Wc3ThreadHandle handle,
                               uint32_t *exit_code)
{
    (void)ctx;
    AndroidThread *t = (AndroidThread *)(uintptr_t)handle;
    if (!t) return -1;

    (void)pthread_mutex_lock(&t->lock);
    int detached = t->detached;
    (void)pthread_mutex_unlock(&t->lock);
    if (detached) return -1;

    if (pthread_join(t->thread, NULL) != 0) return -1;

    (void)pthread_mutex_lock(&t->lock);
    if (exit_code) *exit_code = (uint32_t)t->exit_code;
    t->refs--;
    int free_it = (t->refs == 0u);
    (void)pthread_mutex_unlock(&t->lock);
    if (free_it) android_thread_release(t);
    return 0;
}

static int android_thread_detach(void *ctx, Wc3ThreadHandle handle)
{
    (void)ctx;
    AndroidThread *t = (AndroidThread *)(uintptr_t)handle;
    if (!t) return -1;
    if (pthread_detach(t->thread) != 0) return -1;

    (void)pthread_mutex_lock(&t->lock);
    if (t->detached) {
        (void)pthread_mutex_unlock(&t->lock);
        return -1;
    }
    t->detached = 1;
    t->refs--;
    int free_it = (t->refs == 0u && !t->running);
    (void)pthread_mutex_unlock(&t->lock);
    if (free_it) android_thread_release(t);
    return 0;
}

static Wc3ThreadHandle android_thread_self(void *ctx)
{
    (void)ctx;
    (void)pthread_once(&g_thread_once, android_thread_key_init);
    void *p = pthread_getspecific(g_thread_key);
    if (p) return (Wc3ThreadHandle)(uintptr_t)p;
    return (Wc3ThreadHandle)(uintptr_t)&g_main_thread;
}

static uint64_t android_thread_id(void *ctx, Wc3ThreadHandle handle)
{
    (void)ctx;
    AndroidThread *t = (AndroidThread *)(uintptr_t)handle;
    return t ? t->id : 0u;
}

static void android_thread_yield(void *ctx)
{
    (void)ctx;
    sched_yield();
}

static int android_thread_sleep_ns(void *ctx, uint64_t ns)
{
    (void)ctx;
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    while (nanosleep(&ts, &ts) != 0) {
        if (errno != EINTR) return -1;
    }
    return 0;
}

/* Short teardown poll shared by the audio paths.  It waits on the monotonic
 * clock so a wall-clock step cannot stretch the 1 ms pause. */
static void android_poll_pause(void)
{
    struct timespec ts = {0, 1000000L};
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR) {
    }
}

static pthread_key_t g_tls_keys[WC3_PLATFORM_TLS_MAX];
static uint8_t g_tls_used[WC3_PLATFORM_TLS_MAX];
static pthread_mutex_t g_tls_lock = PTHREAD_MUTEX_INITIALIZER;

static int android_tls_alloc(void *ctx, uint32_t *index)
{
    (void)ctx;
    if (!index) return -1;
    (void)pthread_mutex_lock(&g_tls_lock);
    for (uint32_t i = 0; i < WC3_PLATFORM_TLS_MAX; i++) {
        if (g_tls_used[i]) continue;
        if (pthread_key_create(&g_tls_keys[i], NULL) != 0) {
            (void)pthread_mutex_unlock(&g_tls_lock);
            return -1;
        }
        g_tls_used[i] = 1;
        *index = i;
        (void)pthread_mutex_unlock(&g_tls_lock);
        return 0;
    }
    (void)pthread_mutex_unlock(&g_tls_lock);
    return -1;
}

static int android_tls_free(void *ctx, uint32_t index)
{
    (void)ctx;
    if (index >= WC3_PLATFORM_TLS_MAX) return -1;
    (void)pthread_mutex_lock(&g_tls_lock);
    int ok = g_tls_used[index] != 0;
    if (ok) {
        (void)pthread_key_delete(g_tls_keys[index]);
        g_tls_used[index] = 0;
    }
    (void)pthread_mutex_unlock(&g_tls_lock);
    return ok ? 0 : -1;
}

static int android_tls_set(void *ctx, uint32_t index, uintptr_t value)
{
    (void)ctx;
    if (index >= WC3_PLATFORM_TLS_MAX) return -1;
    (void)pthread_mutex_lock(&g_tls_lock);
    int ok = g_tls_used[index] != 0;
    pthread_key_t key = g_tls_keys[index];
    (void)pthread_mutex_unlock(&g_tls_lock);
    if (!ok) return -1;
    return pthread_setspecific(key, (void *)value) == 0 ? 0 : -1;
}

static uintptr_t android_tls_get(void *ctx, uint32_t index)
{
    (void)ctx;
    if (index >= WC3_PLATFORM_TLS_MAX) return 0u;
    (void)pthread_mutex_lock(&g_tls_lock);
    int ok = g_tls_used[index] != 0;
    pthread_key_t key = g_tls_keys[index];
    (void)pthread_mutex_unlock(&g_tls_lock);
    if (!ok) return 0u;
    return (uintptr_t)pthread_getspecific(key);
}

/* -------------------------------------------------------------------- sync */

typedef enum AndroidSyncKind {
    ANDROID_SYNC_EVENT = 1,
    ANDROID_SYNC_MUTEX = 2,
    ANDROID_SYNC_SEMAPHORE = 3,
    ANDROID_SYNC_CRITICAL = 4
} AndroidSyncKind;

typedef struct AndroidSync {
    struct AndroidSync *next;
    struct AndroidSync *owner_next;
    uint32_t magic;
    uint32_t kind;
    int manual_reset;
    int signaled;
    int abandoned;
    int owner_dead;
    int32_t count;
    int32_t maximum;
    pthread_t owner;
    uint32_t recursion;
    uint32_t waiters;
    /* O-67: a single-object wait sleeps on this object's condition instead of
     * waking every waiter in the process.  Multi-object waits still use the
     * global condition because their state is one atomic observation across
     * several objects; `multi_waiters` lets a signal decide whether the global
     * condition must be broadcast too. */
    pthread_cond_t cond;
    int cond_ready;
    int cond_monotonic;
    uint32_t multi_waiters;
} AndroidSync;

static pthread_once_t g_sync_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_sync_lock;
static pthread_cond_t g_sync_cond;
static pthread_key_t g_owner_key;
static int g_owner_key_ready;
static int g_sync_monotonic;

/* P0-4, first step: the handle registry is direct-mapped instead of one
 * linear list.  The platform lock is still held for lifecycle changes, so
 * close/free races keep the existing contract; lookup no longer scans every
 * live object.  O-67 adds per-object condition variables for single-object
 * waits; removing the state lock itself remains an M5 task. */
#define ANDROID_SYNC_BUCKETS 1024u
_Static_assert((ANDROID_SYNC_BUCKETS & (ANDROID_SYNC_BUCKETS - 1u)) == 0u,
               "sync bucket count must be a power of two");
static AndroidSync *g_sync_buckets[ANDROID_SYNC_BUCKETS];

static uint32_t android_sync_bucket(Wc3SyncHandle handle)
{
    uintptr_t v = (uintptr_t)handle;
    return (uint32_t)((v >> 4) * 2654435761u) & (ANDROID_SYNC_BUCKETS - 1u);
}

static int android_sync_cond_init(AndroidSync *p)
{
    pthread_condattr_t attr;
    p->cond_ready = 0;
    p->cond_monotonic = 0;
    if (pthread_condattr_init(&attr) == 0) {
#if defined(__ANDROID__)
        if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0)
            p->cond_monotonic = 1;
#endif
        if (pthread_cond_init(&p->cond, &attr) == 0) {
            p->cond_ready = 1;
        } else if (pthread_cond_init(&p->cond, NULL) == 0) {
            p->cond_ready = 1;
            p->cond_monotonic = 0;
        }
        pthread_condattr_destroy(&attr);
    } else if (pthread_cond_init(&p->cond, NULL) == 0) {
        p->cond_ready = 1;
    }
    return p->cond_ready ? 0 : -1;
}

static void android_sync_wake(AndroidSync *p)
{
    if (!p) return;
    if (p->cond_ready) (void)pthread_cond_broadcast(&p->cond);
    if (p->multi_waiters != 0u)
        (void)pthread_cond_broadcast(&g_sync_cond);
}

static void android_owner_thread_exit(void *value);

static void android_owner_thread_exit(void *value)
{
    AndroidSync *list = (AndroidSync *)value;
    if (!list) return;

    (void)pthread_mutex_lock(&g_sync_lock);
    while (list) {
        AndroidSync *next = list->owner_next;
        list->owner_next = NULL;
        if (list->kind == ANDROID_SYNC_MUTEX) {
            /* Win32 mutexes are abandoned when the owning thread exits. */
            list->owner = 0;
            list->recursion = 0u;
            list->abandoned = 1;
            android_sync_wake(list);
        } else if (list->kind == ANDROID_SYNC_CRITICAL) {
            /* Critical sections are not kernel objects and are not abandoned;
             * a dead owner leaves them permanently locked. */
            list->owner = 0;
            list->recursion = 0u;
            list->owner_dead = 1;
            android_sync_wake(list);
        }
        list = next;
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
}

static void android_owner_add(AndroidSync *p)
{
    if (!g_owner_key_ready) return;
    AndroidSync *head = (AndroidSync *)pthread_getspecific(g_owner_key);
    p->owner_next = head;
    if (pthread_setspecific(g_owner_key, p) != 0) p->owner_next = NULL;
}

static void android_owner_remove(AndroidSync *p)
{
    if (!g_owner_key_ready) return;
    AndroidSync *head = (AndroidSync *)pthread_getspecific(g_owner_key);
    AndroidSync *prev = NULL;
    AndroidSync *it = head;
    while (it && it != p) {
        prev = it;
        it = it->owner_next;
    }
    if (!it) return;
    if (prev) {
        prev->owner_next = p->owner_next;
    } else {
        head = p->owner_next;
    }
    p->owner_next = NULL;
    (void)pthread_setspecific(g_owner_key, head);
}

static void android_sync_init(void)
{
    /* P0-9: priority inheritance on the one platform lock.  It is optional
     * (bionic may return ENOTSUP on some devices), so failure falls back to
     * a normal mutex rather than changing the sync contract. */
    pthread_mutexattr_t mattr;
    if (pthread_mutexattr_init(&mattr) == 0) {
#if defined(__ANDROID__)
#if defined(__ANDROID_API__) && __ANDROID_API__ < 28
        if (pthread_mutexattr_setprotocol)
#endif
            (void)pthread_mutexattr_setprotocol(&mattr, PTHREAD_PRIO_INHERIT);
#endif
        if (pthread_mutex_init(&g_sync_lock, &mattr) != 0)
            pthread_mutex_init(&g_sync_lock, NULL);
        pthread_mutexattr_destroy(&mattr);
    } else {
        pthread_mutex_init(&g_sync_lock, NULL);
    }
    if (pthread_key_create(&g_owner_key, android_owner_thread_exit) == 0)
        g_owner_key_ready = 1;
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) == 0) {
#if defined(__ANDROID__)
        if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0)
            g_sync_monotonic = 1;
#endif
        if (pthread_cond_init(&g_sync_cond, &attr) != 0) {
            pthread_cond_init(&g_sync_cond, NULL);
            g_sync_monotonic = 0;
        }
        pthread_condattr_destroy(&attr);
    } else {
        pthread_cond_init(&g_sync_cond, NULL);
    }
}

static AndroidSync *android_sync_lookup(Wc3SyncHandle handle)
{
    AndroidSync *p = (AndroidSync *)(uintptr_t)handle;
    if (!p) return NULL;
    for (AndroidSync *it = g_sync_buckets[android_sync_bucket(handle)];
         it; it = it->next) {
        if (it == p) return it;
    }
    return NULL;
}

static AndroidSync *android_sync_new(uint32_t kind)
{
    AndroidSync *p = (AndroidSync *)calloc(1, sizeof *p);
    if (!p) return NULL;
    if (android_sync_cond_init(p) != 0) {
        free(p);
        return NULL;
    }
    p->magic = WC3_SYNC_MAGIC;
    p->kind = kind;
    uint32_t bucket = android_sync_bucket((Wc3SyncHandle)(uintptr_t)p);
    p->next = g_sync_buckets[bucket];
    g_sync_buckets[bucket] = p;
    return p;
}

static int android_event_create(void *ctx, int manual_reset,
                                int initial_state, Wc3SyncHandle *out)
{
    (void)ctx;
    if (!out) return -1;
    (void)pthread_once(&g_sync_once, android_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    AndroidSync *p = android_sync_new(ANDROID_SYNC_EVENT);
    if (p) {
        p->manual_reset = manual_reset != 0;
        p->signaled = initial_state != 0;
        *out = (Wc3SyncHandle)(uintptr_t)p;
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    return p ? 0 : -1;
}

static int android_event_set(void *ctx, Wc3SyncHandle handle)
{
    (void)ctx;
    (void)pthread_once(&g_sync_once, android_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    AndroidSync *p = android_sync_lookup(handle);
    int ok = p && p->kind == ANDROID_SYNC_EVENT;
    if (ok) {
        p->signaled = 1;
        android_sync_wake(p);
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    return ok ? 0 : -1;
}

static int android_event_reset(void *ctx, Wc3SyncHandle handle)
{
    (void)ctx;
    (void)pthread_once(&g_sync_once, android_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    AndroidSync *p = android_sync_lookup(handle);
    int ok = p && p->kind == ANDROID_SYNC_EVENT;
    if (ok) p->signaled = 0;
    (void)pthread_mutex_unlock(&g_sync_lock);
    return ok ? 0 : -1;
}

static int android_mutex_create(void *ctx, int initial_owner,
                                Wc3SyncHandle *out)
{
    (void)ctx;
    if (!out) return -1;
    (void)pthread_once(&g_sync_once, android_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    AndroidSync *p = android_sync_new(ANDROID_SYNC_MUTEX);
    if (p) {
        if (initial_owner) {
            p->owner = pthread_self();
            p->recursion = 1u;
            android_owner_add(p);
        }
        *out = (Wc3SyncHandle)(uintptr_t)p;
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    return p ? 0 : -1;
}

static int android_mutex_release(void *ctx, Wc3SyncHandle handle)
{
    (void)ctx;
    (void)pthread_once(&g_sync_once, android_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    AndroidSync *p = android_sync_lookup(handle);
    int ok = p && (p->kind == ANDROID_SYNC_MUTEX ||
                   p->kind == ANDROID_SYNC_CRITICAL) &&
             pthread_equal(p->owner, pthread_self()) && p->recursion > 0u;
    if (ok) {
        p->recursion--;
        if (p->recursion == 0u) {
            if (p->kind == ANDROID_SYNC_MUTEX) p->abandoned = 0;
            p->owner = 0;
            android_owner_remove(p);
        }
        android_sync_wake(p);
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    return ok ? 0 : -1;
}

static int android_semaphore_create(void *ctx, int32_t initial,
                                    int32_t maximum, Wc3SyncHandle *out)
{
    (void)ctx;
    if (!out || initial < 0 || maximum <= 0 || initial > maximum) return -1;
    (void)pthread_once(&g_sync_once, android_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    AndroidSync *p = android_sync_new(ANDROID_SYNC_SEMAPHORE);
    if (p) {
        p->count = initial;
        p->maximum = maximum;
        *out = (Wc3SyncHandle)(uintptr_t)p;
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    return p ? 0 : -1;
}

static int android_semaphore_release(void *ctx, Wc3SyncHandle handle,
                                     int32_t count, int32_t *previous)
{
    (void)ctx;
    if (count <= 0) return -1;
    (void)pthread_once(&g_sync_once, android_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    AndroidSync *p = android_sync_lookup(handle);
    int ok = p && p->kind == ANDROID_SYNC_SEMAPHORE &&
             count <= p->maximum - p->count;
    if (ok) {
        if (previous) *previous = p->count;
        p->count += count;
        android_sync_wake(p);
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    return ok ? 0 : -1;
}

static int android_sync_ready(const AndroidSync *p)
{
    switch (p->kind) {
    case ANDROID_SYNC_EVENT:
        return p->signaled != 0;
    case ANDROID_SYNC_MUTEX:
        return p->owner == 0 || pthread_equal(p->owner, pthread_self());
    case ANDROID_SYNC_CRITICAL:
        return !p->owner_dead &&
               (p->owner == 0 || pthread_equal(p->owner, pthread_self()));
    case ANDROID_SYNC_SEMAPHORE:
        return p->count > 0;
    default:
        return 0;
    }
}

static void android_sync_consume(AndroidSync *p)
{
    switch (p->kind) {
    case ANDROID_SYNC_EVENT:
        if (!p->manual_reset) p->signaled = 0;
        break;
    case ANDROID_SYNC_MUTEX:
    case ANDROID_SYNC_CRITICAL:
        if (pthread_equal(p->owner, pthread_self())) {
            p->recursion++;
        } else {
            if (p->kind == ANDROID_SYNC_MUTEX) p->abandoned = 0;
            p->owner = pthread_self();
            p->recursion = 1u;
            android_owner_add(p);
        }
        break;
    case ANDROID_SYNC_SEMAPHORE:
        p->count--;
        break;
    default:
        break;
    }
}

static int android_sync_abandoned(const AndroidSync *p)
{
    return p->kind == ANDROID_SYNC_MUTEX && p->abandoned != 0;
}

static int android_wait(void *ctx, const Wc3SyncHandle *objects, uint32_t count,
                        int wait_all, uint32_t timeout_ms, uint32_t *index)
{
    (void)ctx;
    if (!objects || count == 0u || count > WC3_PLATFORM_WAIT_MAX) {
        if (index) *index = 0u;
        return (int)WC3_WAIT_FAILED;
    }
    (void)pthread_once(&g_sync_once, android_sync_init);

    AndroidSync *p[WC3_PLATFORM_WAIT_MAX];
    (void)pthread_mutex_lock(&g_sync_lock);
    for (uint32_t i = 0; i < count; i++) {
        p[i] = android_sync_lookup(objects[i]);
        if (!p[i]) {
            (void)pthread_mutex_unlock(&g_sync_lock);
            if (index) *index = 0u;
            return (int)WC3_WAIT_FAILED;
        }
    }
    for (uint32_t i = 0; i < count; i++) p[i]->waiters++;
    const int single = count == 1u;
    if (!single) {
        for (uint32_t i = 0; i < count; i++) p[i]->multi_waiters++;
    }

    struct timespec deadline;
    if (timeout_ms != UINT32_MAX) {
        int monotonic = single ? p[0]->cond_monotonic : g_sync_monotonic;
        clockid_t cid = monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME;
        if (clock_gettime(cid, &deadline) != 0) {
            for (uint32_t i = 0; i < count; i++) {
                p[i]->waiters--;
                if (!single) p[i]->multi_waiters--;
            }
            (void)pthread_mutex_unlock(&g_sync_lock);
            if (index) *index = 0u;
            return (int)WC3_WAIT_FAILED;
        }
        uint64_t add = (uint64_t)timeout_ms * 1000000ull;
        deadline.tv_sec += (time_t)(add / 1000000000ull);
        deadline.tv_nsec += (long)(add % 1000000000ull);
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    for (;;) {
        int ready = wait_all ? 1 : 0;
        uint32_t first = 0u;
        for (uint32_t i = 0; i < count; i++) {
            int r = android_sync_ready(p[i]);
            if (wait_all) {
                if (!r) {
                    ready = 0;
                    break;
                }
            } else if (r) {
                ready = 1;
                first = i;
                break;
            }
        }

        if (ready) {
            int abandoned = 0;
            if (wait_all) {
                for (uint32_t i = 0; i < count; i++) {
                    if (android_sync_abandoned(p[i])) abandoned = 1;
                    android_sync_consume(p[i]);
                }
            } else {
                abandoned = android_sync_abandoned(p[first]);
                android_sync_consume(p[first]);
            }
            for (uint32_t i = 0; i < count; i++) {
                p[i]->waiters--;
                if (!single) p[i]->multi_waiters--;
            }
            if (!single) (void)pthread_cond_broadcast(&g_sync_cond);
            (void)pthread_mutex_unlock(&g_sync_lock);
            if (index) *index = wait_all ? 0u : first;
            if (abandoned)
                return (int)WC3_WAIT_ABANDONED_0 +
                       (int)(wait_all ? 0u : first);
            return (int)WC3_WAIT_OBJECT_0 + (int)(wait_all ? 0u : first);
        }

        if (timeout_ms == 0u) {
            for (uint32_t i = 0; i < count; i++) {
                p[i]->waiters--;
                if (!single) p[i]->multi_waiters--;
            }
            (void)pthread_mutex_unlock(&g_sync_lock);
            if (index) *index = 0u;
            return (int)WC3_WAIT_TIMEOUT;
        }

        int rc;
        if (single) {
            rc = timeout_ms == UINT32_MAX
                     ? pthread_cond_wait(&p[0]->cond, &g_sync_lock)
                     : pthread_cond_timedwait(&p[0]->cond, &g_sync_lock,
                                              &deadline);
        } else {
            rc = timeout_ms == UINT32_MAX
                     ? pthread_cond_wait(&g_sync_cond, &g_sync_lock)
                     : pthread_cond_timedwait(&g_sync_cond, &g_sync_lock,
                                              &deadline);
        }
        if (rc == ETIMEDOUT) {
            /* Re-check once: a signal may have landed at the deadline. */
            int ready_at_deadline = wait_all ? 1 : 0;
            uint32_t first_at_deadline = 0u;
            for (uint32_t i = 0; i < count; i++) {
                int r = android_sync_ready(p[i]);
                if (wait_all) {
                    if (!r) {
                        ready_at_deadline = 0;
                        break;
                    }
                } else if (r) {
                    ready_at_deadline = 1;
                    first_at_deadline = i;
                    break;
                }
            }
            if (!ready_at_deadline) {
                for (uint32_t i = 0; i < count; i++) {
                    p[i]->waiters--;
                    if (!single) p[i]->multi_waiters--;
                }
                (void)pthread_mutex_unlock(&g_sync_lock);
                if (index) *index = 0u;
                return (int)WC3_WAIT_TIMEOUT;
            }
            int abandoned_at_deadline = 0;
            if (wait_all) {
                for (uint32_t i = 0; i < count; i++) {
                    if (android_sync_abandoned(p[i]))
                        abandoned_at_deadline = 1;
                    android_sync_consume(p[i]);
                }
            } else {
                abandoned_at_deadline =
                    android_sync_abandoned(p[first_at_deadline]);
                android_sync_consume(p[first_at_deadline]);
            }
            for (uint32_t i = 0; i < count; i++) {
                p[i]->waiters--;
                if (!single) p[i]->multi_waiters--;
            }
            if (!single) (void)pthread_cond_broadcast(&g_sync_cond);
            (void)pthread_mutex_unlock(&g_sync_lock);
            if (index) *index = wait_all ? 0u : first_at_deadline;
            if (abandoned_at_deadline)
                return (int)WC3_WAIT_ABANDONED_0 +
                       (int)(wait_all ? 0u : first_at_deadline);
            return (int)WC3_WAIT_OBJECT_0 +
                   (int)(wait_all ? 0u : first_at_deadline);
        }
    }
}

static int android_sync_close(void *ctx, Wc3SyncHandle handle)
{
    (void)ctx;
    (void)pthread_once(&g_sync_once, android_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    AndroidSync **pp = &g_sync_buckets[android_sync_bucket(handle)];
    AndroidSync *p = NULL;
    while (*pp) {
        if ((Wc3SyncHandle)(uintptr_t)*pp == handle) {
            p = *pp;
            break;
        }
        pp = &(*pp)->next;
    }
    int ok = p && p->waiters == 0u && p->recursion == 0u;
    if (ok) {
        *pp = p->next;
        p->magic = 0;
        if (p->cond_ready) {
            (void)pthread_cond_destroy(&p->cond);
            p->cond_ready = 0;
        }
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    if (ok) free(p);
    return ok ? 0 : -1;
}

static int android_cs_create(void *ctx, Wc3SyncHandle *out)
{
    (void)ctx;
    if (!out) return -1;
    (void)pthread_once(&g_sync_once, android_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    AndroidSync *p = android_sync_new(ANDROID_SYNC_CRITICAL);
    if (p) *out = (Wc3SyncHandle)(uintptr_t)p;
    (void)pthread_mutex_unlock(&g_sync_lock);
    return p ? 0 : -1;
}

static int android_cs_enter(void *ctx, Wc3SyncHandle handle)
{
    (void)ctx;
    (void)pthread_once(&g_sync_once, android_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    AndroidSync *p = android_sync_lookup(handle);
    int ok = p && p->kind == ANDROID_SYNC_CRITICAL;
    if (ok) {
        if (pthread_equal(p->owner, pthread_self())) {
            p->recursion++;
        } else {
            p->waiters++;
            while (p->owner != 0 || p->owner_dead) {
                (void)pthread_cond_wait(&p->cond, &g_sync_lock);
            }
            p->waiters--;
            p->owner = pthread_self();
            p->recursion = 1u;
            android_owner_add(p);
        }
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    return ok ? 0 : -1;
}

static int android_cs_try_enter(void *ctx, Wc3SyncHandle handle)
{
    (void)ctx;
    (void)pthread_once(&g_sync_once, android_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    AndroidSync *p = android_sync_lookup(handle);
    int ok = p && p->kind == ANDROID_SYNC_CRITICAL;
    int acquired = 0;
    if (ok && !p->owner_dead &&
        (p->owner == 0 || pthread_equal(p->owner, pthread_self()))) {
        if (pthread_equal(p->owner, pthread_self())) {
            p->recursion++;
        } else {
            p->owner = pthread_self();
            p->recursion = 1u;
            android_owner_add(p);
        }
        acquired = 1;
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    return acquired;
}

static int android_cs_leave(void *ctx, Wc3SyncHandle handle)
{
    return android_mutex_release(ctx, handle);
}

/* ------------------------------------------------------------------- asset */

static char g_asset_root[WC3_PLATFORM_PATH_MAX];
static char g_save_root[WC3_PLATFORM_PATH_MAX];

/* The licensed MPQ directory does not change during a session.  Snapshot its
 * names once so present() and enumerate_mpq() do not open/scan the directory
 * on every call; enumerate by index used to cost O(N^2) readdir work.  The
 * snapshot stores names only, so case-insensitive matching, the root-escape
 * check and the live stat/access check all keep their old semantics.  A
 * truncated snapshot (very large directory) falls back to the old scan, and
 * asset.init() rebuilds it whenever the root is (re)installed. */
static pthread_mutex_t g_asset_lock = PTHREAD_MUTEX_INITIALIZER;
static Wc3AssetSnapshot g_asset_snapshot;
static int g_asset_snapshot_valid;

static void asset_snapshot_ensure_locked(void)
{
    /* A failed opendir must not freeze the empty snapshot for the rest of the
     * process.  The root can become available after a storage mount or a
     * permission change; retry on the next lookup instead of caching failure. */
    if (g_asset_snapshot_valid && !g_asset_snapshot.open_failed) return;
    wc3_asset_snapshot_build(&g_asset_snapshot, g_asset_root);
    g_asset_snapshot_valid = g_asset_snapshot.open_failed ? 0 : 1;
}

static int android_asset_init(void *ctx, const char *asset_root,
                              const char *save_root)
{
    (void)ctx;
    if (!asset_root || !*asset_root || !save_root || !*save_root) return -1;
    if (strlen(asset_root) >= sizeof g_asset_root ||
        strlen(save_root) >= sizeof g_save_root)
        return -1;
    (void)pthread_mutex_lock(&g_asset_lock);
    strcpy(g_asset_root, asset_root);
    strcpy(g_save_root, save_root);
    g_asset_snapshot_valid = 0;
    asset_snapshot_ensure_locked();
    (void)pthread_mutex_unlock(&g_asset_lock);
    return 0;
}

static const char *android_asset_root(void *ctx)
{
    (void)ctx;
    return g_asset_root[0] ? g_asset_root : NULL;
}

static const char *android_save_root(void *ctx)
{
    (void)ctx;
    return g_save_root[0] ? g_save_root : NULL;
}

static int ascii_equal_ci(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

static int is_mpq_name(const char *name)
{
    size_t n = strlen(name);
    return n > 4u && ascii_equal_ci(name + n - 4u, ".mpq");
}

static int asset_file_ok(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
           access(path, R_OK) == 0;
}

/* Fallback for a truncated snapshot: the old directory scan, which is also
 * what a missing/removed snapshot degrades to.  Caller holds g_asset_lock. */
static int asset_scan_present_locked(const char *logical_name)
{
    DIR *dir = opendir(g_asset_root);
    if (!dir) return 0;
    char path[WC3_PLATFORM_PATH_MAX];
    int present = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!ascii_equal_ci(ent->d_name, logical_name)) continue;
        int n = snprintf(path, sizeof path, "%s/%s", g_asset_root, ent->d_name);
        if (n > 0 && (size_t)n < sizeof path && asset_file_ok(path)) {
            present = 1;
            break;
        }
    }
    (void)closedir(dir);
    return present;
}

static int asset_scan_enumerate_locked(uint32_t index, char *name, size_t cap)
{
    DIR *dir = opendir(g_asset_root);
    if (!dir) return -1;

    uint32_t seen = 0u;
    struct dirent *ent;
    int result = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (!is_mpq_name(ent->d_name)) continue;
        if (seen++ != index) continue;
        size_t n = strlen(ent->d_name);
        if (n >= cap) n = cap - 1u;
        memcpy(name, ent->d_name, n);
        name[n] = '\0';
        result = 1;
        break;
    }
    closedir(dir);
    return result;
}

static int android_asset_present(void *ctx, const char *logical_name)
{
    (void)ctx;
    if (!g_asset_root[0] || !logical_name || !*logical_name) return -1;
    /* Asset names are MPQ basenames.  Do not let a logical name escape the
     * configured asset root. */
    if (strchr(logical_name, '/') || strchr(logical_name, '\\')) return 0;

    char path[WC3_PLATFORM_PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/%s", g_asset_root, logical_name);
    if (n < 0 || (size_t)n >= sizeof path) return -1;
    if (asset_file_ok(path)) return 1;

    /* Windows MPQ lookup is case-insensitive; Android's filesystem is not.
     * Keep the same logical result when a user copied War3Patch.mpq instead of
     * war3patch.mpq.  The snapshot resolves the name without syscalls, but
     * every candidate is still checked live. */
    (void)pthread_mutex_lock(&g_asset_lock);
    asset_snapshot_ensure_locked();
    int present = 0;
    for (uint32_t i = 0; i < g_asset_snapshot.count; i++) {
        if (!ascii_equal_ci(g_asset_snapshot.names[i], logical_name)) continue;
        n = snprintf(path, sizeof path, "%s/%s", g_asset_root,
                     g_asset_snapshot.names[i]);
        if (n > 0 && (size_t)n < sizeof path && asset_file_ok(path)) {
            present = 1;
            break;
        }
    }
    if (!present && (g_asset_snapshot.truncated || g_asset_snapshot.open_failed))
        present = asset_scan_present_locked(logical_name);
    (void)pthread_mutex_unlock(&g_asset_lock);
    return present;
}

static int android_asset_enumerate(void *ctx, uint32_t index, char *name,
                                   size_t cap)
{
    (void)ctx;
    if (!g_asset_root[0] || !name || cap == 0u) return -1;

    (void)pthread_mutex_lock(&g_asset_lock);
    asset_snapshot_ensure_locked();
    int result = 0;
    if (!g_asset_snapshot.truncated) {
        uint32_t seen = 0u;
        for (uint32_t i = 0; i < g_asset_snapshot.count; i++) {
            const char *candidate = g_asset_snapshot.names[i];
            if (!is_mpq_name(candidate)) continue;
            if (seen++ != index) continue;
            size_t n = strlen(candidate);
            if (n >= cap) n = cap - 1u;
            memcpy(name, candidate, n);
            name[n] = '\0';
            result = 1;
            break;
        }
    } else {
        /* Skipped long names make the snapshot's index order differ from a
         * fresh readdir order, so a truncated snapshot cannot answer an index
         * lookup correctly.  Keep the old O(N) scan for this rare case. */
        result = asset_scan_enumerate_locked(index, name, cap);
    }
    if (result == 0 && g_asset_snapshot.open_failed)
        result = -1;
    (void)pthread_mutex_unlock(&g_asset_lock);
    return result;
}

/* ------------------------------------------------------------------- input */

/*
 * One lock-free SPSC mailbox between the GameActivity callback thread and the
 * owning guest thread.  The producer writes the slot before the release-store
 * of head; the consumer acquire-loads head before reading the slot and then
 * release-stores tail.  No host lock is held on this path.
 *
 * A full mailbox rejects the event instead of overwriting an older one.  The
 * dropped counter is diagnostic only and is not read from the hot guest path.
 */
#define WC3_INPUT_RING 1024u
#define WC3_INPUT_MASK (WC3_INPUT_RING - 1u)

static _Alignas(64) Wc3InputEvent g_input_events[WC3_INPUT_RING];
static _Alignas(64) _Atomic(uint32_t) g_input_head;
static _Alignas(64) _Atomic(uint32_t) g_input_tail;
static _Atomic(uint32_t) g_input_dropped;
static _Atomic(int) g_input_text_focus;

static int input_kind_valid(uint32_t kind)
{
    return kind >= WC3_INPUT_POINTER_DOWN && kind <= WC3_INPUT_FOCUS;
}

static void android_input_reset(void)
{
    atomic_store_explicit(&g_input_head, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_input_tail, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_input_dropped, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_input_text_focus, 0, memory_order_relaxed);
}

static int android_input_push(void *ctx, const Wc3InputEvent *event)
{
    (void)ctx;
    if (!event || !input_kind_valid(event->kind)) return -1;

    uint32_t head = atomic_load_explicit(&g_input_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&g_input_tail, memory_order_acquire);
    if (head - tail >= WC3_INPUT_RING) {
        atomic_fetch_add_explicit(&g_input_dropped, 1u,
                                  memory_order_relaxed);
        return -1;
    }

    Wc3InputEvent copy = *event;
    if (copy.time_ns == 0u) copy.time_ns = android_monotonic_ns(NULL);
    g_input_events[head & WC3_INPUT_MASK] = copy;
    atomic_store_explicit(&g_input_head, head + 1u, memory_order_release);
    return 0;
}

int wc3_android_input_pop(Wc3InputEvent *event)
{
    if (!event) return -1;

    uint32_t tail = atomic_load_explicit(&g_input_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&g_input_head, memory_order_acquire);
    if (tail == head) return 0;

    *event = g_input_events[tail & WC3_INPUT_MASK];
    atomic_store_explicit(&g_input_tail, tail + 1u, memory_order_release);
    return 1;
}

uint32_t wc3_android_input_dropped(void)
{
    return atomic_load_explicit(&g_input_dropped, memory_order_relaxed);
}

static int android_input_set_text_focus(void *ctx, int focused)
{
    (void)ctx;
    atomic_store_explicit(&g_input_text_focus, focused != 0,
                          memory_order_release);
    return 0;
}

/* ------------------------------------------------------------------ surface */

/*
 * GameActivity owns exactly one ANativeWindow.  The platform keeps the current
 * pointer and a lock-free reference count; it never calls ANativeWindow_release
 * because the window lifetime belongs to the activity.  The activity calls
 * wc3_android_surface_set_window() on INIT/TERM/RESIZE, and consumers use
 * SurfaceApi only while they hold a reference.  A lost/replaced window is
 * never handed out again.
 */
static _Atomic(uintptr_t) g_surface_window;
static _Atomic(uint32_t) g_surface_refs;
static _Atomic(uint32_t) g_surface_lost = 1u;
static _Atomic(uint32_t) g_surface_width;
static _Atomic(uint32_t) g_surface_height;

void wc3_android_surface_set_window(struct ANativeWindow *window)
{
    if (window) {
        uint32_t width = (uint32_t)ANativeWindow_getWidth(window);
        uint32_t height = (uint32_t)ANativeWindow_getHeight(window);
        atomic_store_explicit(&g_surface_width, width, memory_order_relaxed);
        atomic_store_explicit(&g_surface_height, height, memory_order_relaxed);
        atomic_store_explicit(&g_surface_window, (uintptr_t)window,
                              memory_order_release);
        atomic_store_explicit(&g_surface_lost, 0u, memory_order_release);
    } else {
        atomic_store_explicit(&g_surface_lost, 1u, memory_order_release);
        atomic_store_explicit(&g_surface_window, (uintptr_t)0,
                              memory_order_release);
    }
}

static int android_surface_acquire(void *ctx, uintptr_t *native_window)
{
    (void)ctx;
    if (!native_window) return -1;

    uintptr_t window =
        atomic_load_explicit(&g_surface_window, memory_order_acquire);
    if (window == 0u ||
        atomic_load_explicit(&g_surface_lost, memory_order_acquire)) {
        return -1;
    }

    uint32_t refs =
        atomic_load_explicit(&g_surface_refs, memory_order_relaxed);
    for (;;) {
        if (refs == UINT32_MAX) return -1;
        if (atomic_compare_exchange_weak_explicit(
                &g_surface_refs, &refs, refs + 1u, memory_order_acq_rel,
                memory_order_relaxed)) {
            break;
        }
    }

    if (atomic_load_explicit(&g_surface_window, memory_order_acquire) !=
            window ||
        atomic_load_explicit(&g_surface_lost, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&g_surface_refs, 1u, memory_order_acq_rel);
        return -1;
    }

    *native_window = window;
    return 0;
}

static void android_surface_release(void *ctx, uintptr_t native_window)
{
    (void)ctx;
    (void)native_window;

    uint32_t refs =
        atomic_load_explicit(&g_surface_refs, memory_order_relaxed);
    while (refs != 0u) {
        if (atomic_compare_exchange_weak_explicit(
                &g_surface_refs, &refs, refs - 1u, memory_order_acq_rel,
                memory_order_relaxed)) {
            return;
        }
    }
}

static void android_surface_resize(void *ctx, uint32_t width, uint32_t height)
{
    (void)ctx;
    atomic_store_explicit(&g_surface_width, width, memory_order_relaxed);
    atomic_store_explicit(&g_surface_height, height, memory_order_relaxed);
}

static void android_surface_lost(void *ctx)
{
    (void)ctx;
    wc3_android_surface_set_window(NULL);
}

/* ----------------------------------------------------------------- config */

/*
 * ConfigApi is a single-read app-private file, wc3.cfg.  The format is
 * deliberately small and deterministic:
 *
 *   - UTF-8/ASCII text, one key=value pair per line;
 *   - a line whose first non-space character is '#' is a comment;
 *   - surrounding spaces/tabs/CR/LF are removed from key and value;
 *   - keys are [A-Za-z0-9_.-], at most 63 bytes, case-sensitive;
 *   - values are at most 255 bytes, may be empty and may contain '=';
 *   - the last occurrence of a duplicate key wins;
 *   - at most 64 entries are kept; excess entries are ignored.
 *
 * The file is read once before guest startup.  get() returns pointers into a
 * fixed immutable table, so they remain valid for the process lifetime.  A
 * missing file is an empty configuration, not a startup failure.
 */

#define WC3_CONFIG_MAX_ENTRIES 64u
#define WC3_CONFIG_KEY_MAX     64u
#define WC3_CONFIG_VALUE_MAX   256u

typedef struct AndroidConfigEntry {
    char key[WC3_CONFIG_KEY_MAX];
    char value[WC3_CONFIG_VALUE_MAX];
} AndroidConfigEntry;

static AndroidConfigEntry g_config_entries[WC3_CONFIG_MAX_ENTRIES];
static size_t g_config_count;
static int g_config_loaded;

#if WC3_DEBUG
#define CONFIG_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "wc3config", __VA_ARGS__)
#else
#define CONFIG_LOG(...) ((void)0)
#endif

static char *config_trim(char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;
    char *end = text + strlen(text);
    while (end > text &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
            end[-1] == '\n'))
        end--;
    *end = '\0';
    return text;
}

static int config_key_valid(const char *key)
{
    if (!key || !*key) return 0;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' || *p == '-')
            continue;
        return 0;
    }
    return 1;
}

static AndroidConfigEntry *config_find(const char *key)
{
    for (size_t i = 0; i < g_config_count; i++) {
        if (strcmp(g_config_entries[i].key, key) == 0)
            return &g_config_entries[i];
    }
    return NULL;
}

static const char *android_config_get(void *ctx, const char *key)
{
    (void)ctx;
    if (!key || !*key || !g_config_loaded) return NULL;
    AndroidConfigEntry *entry = config_find(key);
    return entry ? entry->value : NULL;
}

static int android_config_get_int(void *ctx, const char *key, int fallback)
{
    const char *value = android_config_get(ctx, key);
    if (!value || !*value) return fallback;
    errno = 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (errno == ERANGE || end == value || !end || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX)
        return fallback;
    return (int)parsed;
}

int wc3_android_config_load(const char *path)
{
    if (g_config_loaded) return -1;
    g_config_loaded = 1;
    if (!path || !*path) return 0;

    FILE *file = fopen(path, "r");
    if (!file) {
        if (errno == ENOENT) return 0;
        CONFIG_LOG("cannot open %s: %d", path, errno);
        return -1;
    }

    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        (void)length;
        char *text = config_trim(line);
        if (!*text || *text == '#') continue;

        char *separator = strchr(text, '=');
        if (!separator) {
            CONFIG_LOG("ignoring line without '='");
            continue;
        }
        *separator = '\0';
        char *key = config_trim(text);
        char *value = config_trim(separator + 1);
        if (!config_key_valid(key)) {
            CONFIG_LOG("ignoring invalid key");
            continue;
        }
        if (strlen(key) >= WC3_CONFIG_KEY_MAX ||
            strlen(value) >= WC3_CONFIG_VALUE_MAX) {
            CONFIG_LOG("ignoring oversized key or value");
            continue;
        }

        AndroidConfigEntry *entry = config_find(key);
        if (!entry) {
            if (g_config_count >= WC3_CONFIG_MAX_ENTRIES) {
                CONFIG_LOG("ignoring excess entry");
                continue;
            }
            entry = &g_config_entries[g_config_count++];
            memcpy(entry->key, key, strlen(key) + 1);
        }
        memcpy(entry->value, value, strlen(value) + 1);
    }

    int failed = ferror(file) != 0;
    free(line);
    (void)fclose(file);
    if (failed) {
        CONFIG_LOG("read error on %s", path);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ audio */

/*
 * AAudio output sink.  PCM is queued into one SPSC byte ring by the guest
 * audio thread and drained by the AAudio real-time callback.  The callback
 * never takes a host lock and never allocates; close joins the supervisor and
 * waits for an in-flight callback before freeing the ring.
 *
 * Device loss is reported by AAudio's error callback, which only sets an
 * atomic flag and posts a semaphore.  One supervisor thread per stream closes
 * the failed stream and reopens it with an exponential backoff.  After
 * WC3_AUDIO_REOPEN_MAX consecutive failures the stream becomes an explicit
 * silent sink: the mixer still runs and the guest still publishes, but the
 * callback emits zeroes.  The failure is recorded in debug builds.
 */

/* N-19 sizes the primary voice from the guest format: seven 50 ms headers plus
 * one frame of guard, rounded to 1 KiB with a 64 KiB floor.  At 44.1/48 kHz
 * that holds the five 8832-byte headers (44160 B) and is ~4.6 AAudio bursts on
 * the reference device (3536 frames).  audio_mixer.h owns the slot and floor
 * constants so the Android sink and the headless twin cannot drift. */
#define WC3_AUDIO_REOPEN_MAX   5
/* S-4 / P-3: measured A/B inputs from the nfsu2 architecture.  These are
 * NOT a declared fix: A-13.8 already measured 60 ms as -2.4 pp repeats only.
 * The target is an input; the granted value is the evidence. */
#define WC3_AUDIO_TARGET_MS    40
#define WC3_AUDIO_MAX_MS       100
/* AAudio's capacity request is in DEVICE frames, but the builder does not
 * know the device rate until after open (we deliberately do not force a
 * sample rate).  nfsu2 hard-codes 48 kHz for its 100 ms ceiling.  WC3 uses
 * 48 kHz as the reference for the common case, raises it for a higher-rate
 * guest, and never exceeds the 192 kHz HLE ceiling.  This is only a maximum:
 * the 40 ms target still sets the actual queue depth, and an xrun grows one
 * burst at a time up to 100 ms. */
#define WC3_AUDIO_REFERENCE_RATE 48000
#define WC3_AUDIO_MAX_RATE       192000

static int32_t audio_frames_for_ms(AAudioStream *stream, int32_t rate,
                                   int32_t ms);

#if WC3_DEBUG
#define AUDIO_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "wc3audio", __VA_ARGS__)
#else
#define AUDIO_LOG(...) ((void)0)
#endif

#if WC3_DEBUG && WC3_DEBUG_AUDIO
typedef struct AndroidAudioTap AndroidAudioTap;
#endif

typedef struct AndroidAudioStream {
    _Atomic int callback_users;
    /* C-8: control-path users that hold a stream/voice pointer.  close()
     * unregisters first, then waits for this to drain before freeing
     * PCM/mixer storage. */
    _Atomic int api_users;
    _Atomic int closing;
    _Atomic int silent;
    _Atomic int lost;
    _Atomic uint32_t lost_events;
    _Atomic int started;

    /* Measured AAudio buffer depth after applying the fixed target. */
    _Atomic int32_t buffer_frames;

    /* stream is written by the supervisor and read by close under lifecycle. */
    AAudioStream *stream;
    pthread_mutex_t lifecycle;
    sem_t wake;
    pthread_t supervisor;
    int supervisor_started;

    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits;

    /* nfsu2 mixer: one primary looping voice created at open(), plus the
     * voices DirectSound registers.  The AAudio callback renders only this
     * mixer; there is no linear or pull transport. */
    Wc3PcmMixer mixer;
    uint8_t *voice_pcm[WC3_MIXER_VOICES];
    uintptr_t primary_voice;
    uint32_t device_rate;
    uint32_t stream_slot;
    int float_output;

    /* S-4: non-RT starvation diagnostics and measured xrun growth. */
    _Atomic int32_t last_xruns;
    _Atomic uint32_t pump_ticks;

#if WC3_DEBUG && WC3_DEBUG_AUDIO
    /* N-16b: optional non-RT post-mix fixture.  NULL in a normal run. */
    AndroidAudioTap *tap;
#endif
} AndroidAudioStream;

#if WC3_DEBUG && WC3_DEBUG_AUDIO
/* N-16b: post-mix device fixture.  The RT callback only converts and pushes
 * into this preallocated ring.  fopen/fwrite/flush and logging happen on the
 * writer thread or at open/close, never in the callback. */
#define WC3_AUDIO_TAP_RING_BYTES (1u << 20)
#define WC3_AUDIO_TAP_LIMIT      (8u << 20)

struct AndroidAudioTap {
    Wc3AudioTapRing ring;
    uint8_t *ring_pcm;
    FILE *file;
    pthread_t writer;
    _Atomic int stop;
    _Atomic uint32_t file_bytes;
    _Atomic int truncated;
    _Atomic int format_invalid;
    int writer_started;
    uint32_t rate;
    uint32_t channels;
    int is_float;
};

static void *android_audio_tap_writer(void *arg)
{
    AndroidAudioTap *tap = (AndroidAudioTap *)arg;
    uint8_t buf[16384];
    unsigned flushes = 0u;

    for (;;) {
        uint32_t got;
        while ((got = wc3_audio_tap_pop(&tap->ring, buf, sizeof buf)) != 0u) {
            uint32_t written = atomic_load_explicit(&tap->file_bytes,
                                                    memory_order_relaxed);
            uint32_t remaining = written < WC3_AUDIO_TAP_LIMIT
                ? WC3_AUDIO_TAP_LIMIT - written : 0u;
            if (got > remaining) {
                got = remaining;
                atomic_store_explicit(&tap->truncated, 1,
                                      memory_order_release);
            }
            if (got) {
                if (fwrite(buf, 1, got, tap->file) != got) {
                    atomic_store_explicit(&tap->truncated, 1,
                                          memory_order_release);
                    break;
                }
                written += got;
                atomic_store_explicit(&tap->file_bytes, written,
                                      memory_order_relaxed);
                if ((++flushes % 64u) == 0u) fflush(tap->file);
            }
            if (written >= WC3_AUDIO_TAP_LIMIT) {
                atomic_store_explicit(&tap->truncated, 1,
                                      memory_order_release);
                break;
            }
        }
        if (atomic_load_explicit(&tap->stop, memory_order_acquire))
            break;
        struct timespec pause = { 0, 10u * 1000u * 1000u };
        (void)nanosleep(&pause, NULL);
    }
    return NULL;
}

static void android_audio_tap_start(AndroidAudioStream *st)
{
    const char *dir = getenv("WC3_DATA_DIR");
    char path[768];
    int n;
    if (!st || st->tap || !dir || !*dir) return;
    n = snprintf(path, sizeof path, "%s/audio_out_dump_on", dir);
    if (n < 0 || n >= (int)sizeof path) return;
    FILE *probe = fopen(path, "rb");
    if (!probe) return;
    fclose(probe);

    AndroidAudioTap *tap = (AndroidAudioTap *)calloc(1, sizeof *tap);
    if (!tap) return;
    tap->ring_pcm = (uint8_t *)malloc(WC3_AUDIO_TAP_RING_BYTES);
    if (!tap->ring_pcm ||
        wc3_audio_tap_init(&tap->ring, tap->ring_pcm,
                           WC3_AUDIO_TAP_RING_BYTES) != 0) {
        free(tap->ring_pcm);
        free(tap);
        return;
    }
    n = snprintf(path, sizeof path, "%s/audio_out_dump.pcm", dir);
    if (n < 0 || n >= (int)sizeof path) {
        free(tap->ring_pcm);
        free(tap);
        return;
    }
    tap->file = fopen(path, "wb");
    if (!tap->file) {
        free(tap->ring_pcm);
        free(tap);
        return;
    }
    tap->rate = st->device_rate ? st->device_rate : st->sample_rate;
    tap->channels = st->channels;
    tap->is_float = st->float_output;
    atomic_init(&tap->stop, 0);
    atomic_init(&tap->file_bytes, 0u);
    atomic_init(&tap->truncated, 0);
    atomic_init(&tap->format_invalid, 0);
    if (pthread_create(&tap->writer, NULL, android_audio_tap_writer, tap) != 0) {
        fclose(tap->file);
        free(tap->ring_pcm);
        free(tap);
        return;
    }
    /* Publish the tap only after the writer exists: a concurrent close must
     * never observe st->tap and skip join because writer_started was not set
     * yet. */
    tap->writer_started = 1;
    st->tap = tap;
    AUDIO_LOG("[audioout] start rate=%u ch=%u fmt=%s ring=%u limit=%u",
              tap->rate, tap->channels, tap->is_float ? "f32" : "s16",
              WC3_AUDIO_TAP_RING_BYTES, WC3_AUDIO_TAP_LIMIT);
}

static void android_audio_tap_stop(AndroidAudioStream *st)
{
    AndroidAudioTap *tap = st ? st->tap : NULL;
    if (!tap) return;
    atomic_store_explicit(&tap->stop, 1, memory_order_release);
    if (tap->writer_started)
        (void)pthread_join(tap->writer, NULL);
    fflush(tap->file);
    AUDIO_LOG("[audioout] stop bytes=%u rate=%u ch=%u fmt=%s dropped=%u "
              "truncated=%d format_invalid=%d",
              atomic_load_explicit(&tap->file_bytes, memory_order_relaxed),
              tap->rate, tap->channels, tap->is_float ? "f32" : "s16",
              wc3_audio_tap_dropped(&tap->ring),
              atomic_load_explicit(&tap->truncated, memory_order_relaxed),
              atomic_load_explicit(&tap->format_invalid, memory_order_relaxed));
    fclose(tap->file);
    free(tap->ring_pcm);
    free(tap);
    st->tap = NULL;
}

static void android_audio_tap_push(AndroidAudioStream *st, const void *pcm,
                                   uint32_t bytes)
{
    AndroidAudioTap *tap = st ? st->tap : NULL;
    if (!tap || !pcm || bytes == 0u) return;
    if (tap->rate != (st->device_rate ? st->device_rate : st->sample_rate) ||
        tap->channels != st->channels ||
        tap->is_float != st->float_output) {
        atomic_store_explicit(&tap->format_invalid, 1, memory_order_release);
        return;
    }
    if (tap->is_float) {
        if ((bytes % 4u) != 0u) return;
        (void)wc3_audio_tap_push_f32(&tap->ring, (const float *)pcm,
                                     bytes / 4u);
    } else {
        if ((bytes % 2u) != 0u) return;
        (void)wc3_audio_tap_push_s16(&tap->ring, (const int16_t *)pcm,
                                     bytes / 2u);
    }
}
#else
#define android_audio_tap_start(st) ((void)0)
#define android_audio_tap_stop(st) ((void)0)
#define android_audio_tap_push(st, pcm, bytes) ((void)0)
#endif

/* voice_remove() frees the shadow PCM as soon as the guest releases a
 * DirectSound buffer.  A render callback may already be inside that voice, so
 * the producer side waits for render_users to drain before freeing.  This is
 * a quiescence wait, not a lock: render() never waits for it. */
static void android_audio_wait_render_idle(Wc3PcmMixer *mixer)
{
    const Wc3Platform *p = wc3_platform();
    while (wc3_mixer_render_users(mixer) != 0u) {
        if (p && p->thread.yield)
            p->thread.yield(p->thread.ctx);
        else
            sched_yield();
    }
}

/* Start the device exactly once.  N-19 calls this from android_audio_open so
 * the callback plays silence before the first DirectSound publish; the same function
 * stays the idempotent retry for a reopened stream and for producers that run
 * before open() has returned.  Never called from the AAudio callback. */
static int android_audio_ensure_started(AndroidAudioStream *st)
{
    if (!st) return -1;
    if (atomic_load_explicit(&st->closing, memory_order_acquire)) return -1;
    if (atomic_load_explicit(&st->started, memory_order_acquire)) return 0;
    int started = 0;
    pthread_mutex_lock(&st->lifecycle);
    AAudioStream *stream = st->stream;
    if (stream && !atomic_load_explicit(&st->started, memory_order_acquire)) {
        aaudio_result_t rc = AAudioStream_requestStart(stream);
        if (rc == AAUDIO_OK) {
            atomic_store_explicit(&st->started, 1, memory_order_release);
            started = 1;
        } else {
            AUDIO_LOG("AAudio requestStart failed %d", (int)rc);
        }
    } else if (stream) {
        started = atomic_load_explicit(&st->started, memory_order_acquire);
    }
    pthread_mutex_unlock(&st->lifecycle);
    return started ? 0 : -1;
}

/* S-4: runs on the NON-RT supervisor pump, never in the AAudio callback.
 * Growth is a measured reaction to a real xrun, one burst at a time, bounded
 * by WC3_AUDIO_MAX_MS.  The report is diagnostics only: it introduces no new
 * gate threshold and never changes PCM content. */
static void android_audio_service(AndroidAudioStream *st)
{
    pthread_mutex_lock(&st->lifecycle);
    AAudioStream *stream = st->stream;
    if (!stream) {
        pthread_mutex_unlock(&st->lifecycle);
        return;
    }

    int32_t xruns = AAudioStream_getXRunCount(stream);
    int32_t last = atomic_load_explicit(&st->last_xruns,
                                        memory_order_relaxed);
    int32_t burst = AAudioStream_getFramesPerBurst(stream);
    int32_t size = AAudioStream_getBufferSizeInFrames(stream);
    /* nfsu2 measures the growth ceiling in the ACTUAL device rate. */
    int32_t device_rate = (int32_t)st->device_rate;
    if (device_rate <= 0) device_rate = (int32_t)st->sample_rate;
    int32_t max = audio_frames_for_ms(stream, device_rate,
                                      WC3_AUDIO_MAX_MS);
    if (xruns > last && burst > 0 && size > 0 && size < max) {
        int32_t grown = size + burst;
        if (grown > max) grown = max;
        (void)AAudioStream_setBufferSizeInFrames(stream, grown);
        size = AAudioStream_getBufferSizeInFrames(stream);
    }
    if (xruns >= 0)
        atomic_store_explicit(&st->last_xruns, xruns, memory_order_relaxed);
    atomic_store_explicit(&st->buffer_frames, size, memory_order_release);
    pthread_mutex_unlock(&st->lifecycle);

#if WC3_DEBUG && WC3_DEBUG_AUDIO
    uint32_t tick = atomic_fetch_add_explicit(&st->pump_ticks, 1u,
                                              memory_order_relaxed);
    if ((tick & 127u) == 0u) {
        uint32_t active = 0u, callback_peak = 0u;
        uint64_t clipped = 0u;
        float peak = 0.0f;
        wc3_mixer_stats(&st->mixer, &active, &clipped, &peak,
                        &callback_peak);
        AUDIO_LOG("[wc3audio] mix xruns=%d buffer=%d burst=%d "
                  "active=%u peak=%.4f clipped=%llu callbackPeak=%u us",
                  xruns, size, burst, active, (double)peak,
                  (unsigned long long)clipped, callback_peak);
    }
#endif
}static aaudio_data_callback_result_t audio_data_callback(
    AAudioStream *stream, void *user, void *audio_data, int32_t num_frames)
{
    AndroidAudioStream *st = (AndroidAudioStream *)user;
    (void)stream;
    if (!st || num_frames <= 0) return AAUDIO_CALLBACK_RESULT_CONTINUE;
    if (atomic_load_explicit(&st->closing, memory_order_acquire))
        return AAUDIO_CALLBACK_RESULT_STOP;

    atomic_fetch_add_explicit(&st->callback_users, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&st->closing, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&st->callback_users, 1, memory_order_acq_rel);
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    uint32_t out_bpf = st->float_output ? st->channels * 4u
                                        : st->channels * 2u;
    uint32_t need = (uint32_t)num_frames * out_bpf;
    int silent = atomic_load_explicit(&st->silent, memory_order_acquire);
    if (silent) {
        memset(audio_data, 0, (size_t)need);
        atomic_fetch_sub_explicit(&st->callback_users, 1,
                                  memory_order_acq_rel);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    /* The nfsu2 mixer is the only producer.  It pulls looping voices,
     * resamples to the DEVICE rate and fills the complete callback block. */
    uint64_t started = android_monotonic_ns(NULL);
    uint32_t got = wc3_mixer_render(&st->mixer, audio_data,
                                   (uint32_t)num_frames);
    if (got < need)
        memset((uint8_t *)audio_data + got, 0, need - got);
    uint64_t elapsed_ns = android_monotonic_ns(NULL) - started;
    uint32_t elapsed_us = (uint32_t)(elapsed_ns / 1000u);
    uint32_t peak = atomic_load_explicit(&st->mixer.callback_peak_us,
                                         memory_order_relaxed);
    while (elapsed_us > peak &&
           !atomic_compare_exchange_weak_explicit(
               &st->mixer.callback_peak_us, &peak, elapsed_us,
               memory_order_relaxed, memory_order_relaxed)) {}

    /* N-16b: post-mix fixture.  The tap is a preallocated lock-free push;
     * no file I/O happens in the RT callback. */
    android_audio_tap_push(st, audio_data, need);

    atomic_fetch_sub_explicit(&st->callback_users, 1, memory_order_acq_rel);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void audio_error_callback(AAudioStream *stream, void *user,
                                 aaudio_result_t error)
{
    AndroidAudioStream *st = (AndroidAudioStream *)user;
    (void)stream;
    (void)error;
    if (!st) return;
    atomic_fetch_add_explicit(&st->callback_users, 1, memory_order_acq_rel);
    atomic_store_explicit(&st->lost, 1, memory_order_release);
    atomic_fetch_add_explicit(&st->lost_events, 1, memory_order_acq_rel);
    (void)sem_post(&st->wake);
    AUDIO_LOG("AAudio error %d", (int)error);
    atomic_fetch_sub_explicit(&st->callback_users, 1, memory_order_acq_rel);
}

static int32_t audio_frames_for_ms(AAudioStream *stream, int32_t rate,
                                   int32_t ms)
{
    int32_t burst = AAudioStream_getFramesPerBurst(stream);
    int32_t want = (int32_t)((int64_t)rate * ms / 1000);
    int32_t cap = AAudioStream_getBufferCapacityInFrames(stream);
    if (burst > 0 && want > 0)
        want = ((want + burst - 1) / burst) * burst;
    if (want < burst) want = burst;
    if (cap > 0 && want > cap) want = cap;
    return want;
}

static AAudioStream *audio_create_stream(AndroidAudioStream *st,
                                         int allow_float)
{
    static const aaudio_performance_mode_t modes[] = {
        AAUDIO_PERFORMANCE_MODE_LOW_LATENCY,
        AAUDIO_PERFORMANCE_MODE_NONE,
    };
    /* nfsu2 opens PCM_FLOAT at the DEVICE rate and resamples in the mixer.
     * PCM_I16 is the compatibility fallback when a driver refuses float. */
    static const aaudio_format_t formats[] = {
        AAUDIO_FORMAT_PCM_FLOAT,
        AAUDIO_FORMAT_PCM_I16,
    };
    const int32_t rate = (int32_t)st->sample_rate;
    int32_t capacity_rate = rate;
    if (capacity_rate < WC3_AUDIO_REFERENCE_RATE)
        capacity_rate = WC3_AUDIO_REFERENCE_RATE;
    if (capacity_rate > WC3_AUDIO_MAX_RATE)
        capacity_rate = WC3_AUDIO_MAX_RATE;

    for (size_t f = allow_float ? 0u : 1u;
         f < sizeof formats / sizeof formats[0]; f++) {
      for (size_t m = 0; m < sizeof modes / sizeof modes[0]; m++) {
        AAudioStreamBuilder *builder = NULL;
        aaudio_result_t result = AAudio_createStreamBuilder(&builder);
        if (result != AAUDIO_OK || !builder) {
            AUDIO_LOG("AAudio builder failed %d", (int)result);
            return NULL;
        }

        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(builder, modes[m]);
        AAudioStreamBuilder_setFormat(builder, formats[f]);
        AAudioStreamBuilder_setChannelCount(builder, (int32_t)st->channels);
        /* Deliberately NO setSampleRate(): the device chooses its own rate and
         * the host mixer converts the guest rate.  Forcing the guest rate was
         * the reason AAudio could silently disagree with the producer. */
        AAudioStreamBuilder_setDataCallback(builder, audio_data_callback, st);
        AAudioStreamBuilder_setErrorCallback(builder, audio_error_callback, st);
        /* Reserve headroom for growth up to WC3_AUDIO_MAX_MS.  Use at least
         * the 48 kHz reference rate: the device may run at 48 kHz while the
         * guest opened at 22.05 kHz, and a capacity sized from the guest rate
         * would cap growth at ~46 ms instead of 100 ms. */
        AAudioStreamBuilder_setBufferCapacityInFrames(
            builder, (int32_t)((int64_t)capacity_rate *
                               WC3_AUDIO_MAX_MS / 1000));

        AAudioStream *stream = NULL;
        result = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);
        if (result != AAUDIO_OK || !stream) {
            AUDIO_LOG("AAudio open fmt=%d mode=%d failed %d",
                      (int)f, (int)m, (int)result);
            if (stream) AAudioStream_close(stream);
            continue;
        }

        int32_t actual = AAudioStream_getSampleRate(stream);
        if (actual <= 0) actual = rate;
        int32_t actual_ch = AAudioStream_getChannelCount(stream);
        if (actual_ch > 0 && actual_ch != (int32_t)st->channels) {
            AUDIO_LOG("AAudio changed channels %d -> %d",
                      (int)st->channels, (int)actual_ch);
            AAudioStream_close(stream);
            continue;
        }
        /* LOW_LATENCY streams on MediaTek/Unisoc cap capacity at two bursts,
         * which is too small for the measured 50 ms guest buffers.  Fall back
         * to a normal-latency stream instead of shipping a starving sink. */
        int32_t target = audio_frames_for_ms(stream, actual,
                                             WC3_AUDIO_TARGET_MS);
        if (m == 0 &&
            AAudioStream_getBufferCapacityInFrames(stream) < target) {
            AUDIO_LOG("AAudio low-latency capacity too small, using NONE");
            AAudioStream_close(stream);
            continue;
        }
        if (target > 0)
            (void)AAudioStream_setBufferSizeInFrames(stream, target);

        int32_t granted = AAudioStream_getBufferSizeInFrames(stream);
        atomic_store_explicit(&st->buffer_frames, granted,
                              memory_order_release);

        st->device_rate = (uint32_t)actual;
        st->float_output = formats[f] == AAUDIO_FORMAT_PCM_FLOAT;
        /* A device switch must not wipe the voices; only the output shape. */
        st->mixer.rate = (uint32_t)actual;
        st->mixer.float_output = st->float_output;
        st->mixer.channels = st->channels;

        /* A freshly opened stream is stopped.  android_audio_open() starts it
         * during open (N-19); a device switch explicitly calls
         * android_audio_ensure_started() after swapping the stream.  No silent
         * callback run happens before the first publish simply because the
         * producer has not written yet. */
        atomic_store_explicit(&st->started, 0, memory_order_release);
        /* nfsu2 startSoundStream() resets soundLastXruns after every open.
         * The reopened AAudio stream counts xruns from zero, so keeping the
         * old device's count here would suppress S-4 growth until the new
         * stream exceeded it. */
        atomic_store_explicit(&st->last_xruns, 0, memory_order_relaxed);

        AUDIO_LOG("AAudio opened fmt=%d mode=%d rate=%d guest=%d ch=%d "
                  "buffer=%d", (int)f, (int)m, (int)actual,
                  (int)st->sample_rate, (int)st->channels, (int)granted);
        return stream;
      }
    }
    return NULL;
}

static int audio_sleep_interruptible(AndroidAudioStream *st, uint32_t ms)
{
    while (ms) {
        if (atomic_load_explicit(&st->closing, memory_order_acquire)) return -1;
        uint32_t chunk = ms > 50u ? 50u : ms;
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = (long)chunk * 1000000L;
        (void)nanosleep(&ts, NULL);
        ms -= chunk;
    }
    return 0;
}

static void *audio_supervisor(void *arg)
{
    AndroidAudioStream *st = (AndroidAudioStream *)arg;
    unsigned failures = 0;

    for (;;) {
        /* Periodic service tick: xrun growth and the health report must run
         * even when the device never posts an error.  The old completion pump
         * is gone; this timeout is the only non-RT wake source. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        int wait_result;
        do {
            wait_result = sem_timedwait(&st->wake, &ts);
        } while (wait_result != 0 && errno == EINTR);
        if (wait_result != 0 && errno != ETIMEDOUT) break;
        if (atomic_load_explicit(&st->closing, memory_order_acquire)) break;

        android_audio_service(st);
        if (atomic_load_explicit(&st->silent, memory_order_acquire)) continue;
        if (!atomic_load_explicit(&st->lost, memory_order_acquire)) continue;

        uint32_t seen = atomic_load_explicit(&st->lost_events,
                                             memory_order_acquire);

        pthread_mutex_lock(&st->lifecycle);
        AAudioStream *old = st->stream;
        st->stream = NULL;
        pthread_mutex_unlock(&st->lifecycle);
        if (old) {
            (void)AAudioStream_requestStop(old);
            (void)AAudioStream_close(old);
        }
        /* AAudioStream_close() tears down the callback, but be explicit: the
         * mixer's rate/float_output/channels are consumed by the RT thread and
         * are only safe to change after the last callback has left. */
        while (atomic_load_explicit(&st->callback_users,
                                    memory_order_acquire) != 0)
            android_poll_pause();

        unsigned shift = failures < 4u ? failures : 4u;
        if (audio_sleep_interruptible(st, 50u << shift) != 0) break;

        AAudioStream *fresh = audio_create_stream(st, st->primary_voice != 0u);
        if (fresh) {
            pthread_mutex_lock(&st->lifecycle);
            if (atomic_load_explicit(&st->closing, memory_order_acquire)) {
                pthread_mutex_unlock(&st->lifecycle);
                (void)AAudioStream_requestStop(fresh);
                (void)AAudioStream_close(fresh);
                break;
            }
            st->stream = fresh;
            if (AAudioStream_requestStart(fresh) == AAUDIO_OK)
                atomic_store_explicit(&st->started, 1,
                                      memory_order_release);
            pthread_mutex_unlock(&st->lifecycle);

            atomic_store_explicit(&st->lost, 0, memory_order_release);
            failures = 0;
            if (atomic_load_explicit(&st->lost_events,
                                     memory_order_acquire) != seen) {
                atomic_store_explicit(&st->lost, 1, memory_order_release);
                (void)sem_post(&st->wake);
            }
            AUDIO_LOG("AAudio reopened");
        } else {
            failures++;
            if (failures >= WC3_AUDIO_REOPEN_MAX) {
                atomic_store_explicit(&st->silent, 1, memory_order_release);
                AUDIO_LOG("AAudio silent sink after %u failures", failures);
            } else {
                atomic_store_explicit(&st->lost, 1, memory_order_release);
                (void)sem_post(&st->wake);
            }
        }
    }

    pthread_mutex_lock(&st->lifecycle);
    AAudioStream *stream = st->stream;
    st->stream = NULL;
    pthread_mutex_unlock(&st->lifecycle);
    if (stream) {
        (void)AAudioStream_requestStop(stream);
        (void)AAudioStream_close(stream);
    }
    return NULL;
}

static int android_audio_voice_add(void *ctx, uintptr_t handle,
                                   uint32_t rate, uint32_t channels,
                                   uint32_t bits, uint32_t bytes,
                                   uintptr_t *out_voice);
static void android_audio_register_stream(AndroidAudioStream *st);
static void android_audio_unregister_stream(AndroidAudioStream *st);

/* N-19: the primary ring must hold seven 50 ms headers (five Miles headers
 * plus reset headroom) at up to 192 kHz.  Round to 1 KiB so the voice's slot
 * layout and the 1 KiB staging pool share one alignment. */
static uint32_t android_audio_primary_ring_bytes(uint32_t rate,
                                                 uint32_t out_align)
{
    if (!rate || !out_align) return 0u;
    uint64_t bytes = ((uint64_t)rate / 20u) * out_align + out_align;
    uint64_t header = ((bytes + WC3_VOICE_SLOT_BYTES - 1u) /
                       WC3_VOICE_SLOT_BYTES) * WC3_VOICE_SLOT_BYTES;
    uint64_t ring = 7u * header;
    if (ring < WC3_VOICE_MIN_RING_BYTES) ring = WC3_VOICE_MIN_RING_BYTES;
    ring = ((ring + WC3_VOICE_SLOT_BYTES - 1u) /
            WC3_VOICE_SLOT_BYTES) * WC3_VOICE_SLOT_BYTES;
    if (ring > (uint64_t)WC3_VOICE_MAX_SLOTS * WC3_VOICE_SLOT_BYTES)
        return 0u;
    return (uint32_t)ring;
}

/* N-18: the primary voice publishes whole-ring snapshot versions.  Its PCM
 * allocation is three full rings (3 * slots * 1 KiB); the generic voice_add()
 * stays one-ring/per-slot for DirectSound buffers. */
static int android_audio_primary_add(AndroidAudioStream *st, uint32_t rate,
                                     uint32_t channels, uint32_t bits,
                                     uint32_t bytes, uintptr_t *out_voice)
{
    if (!st || !out_voice || !st->stream_slot) return -1;
    *out_voice = 0;
    uint32_t bpf = channels * (bits / 8u);
    if (bpf == 0u || (WC3_VOICE_SLOT_BYTES % bpf) != 0u ||
        bytes < bpf || (bytes % bpf) != 0u) return -1;
    uint32_t slots = (bytes + WC3_VOICE_SLOT_BYTES - 1u) /
                     WC3_VOICE_SLOT_BYTES;
    if (slots == 0u || slots > WC3_VOICE_MAX_SLOTS) return -1;
    size_t total = (size_t)slots * WC3_VOICE_SLOT_BYTES *
                   WC3_VOICE_SNAPSHOTS;
    uint8_t *pcm = (uint8_t *)calloc(1, total);
    if (!pcm) return -1;
    uintptr_t local = 0;
    if (wc3_mixer_voice_add_snapshot(&st->mixer, pcm, bytes, rate, channels,
                                     bits, &local) != 0) {
        free(pcm);
        return -1;
    }
    st->voice_pcm[(size_t)local - 1u] = pcm;
    *out_voice = ((uintptr_t)st->stream_slot << 16) | local;
    return 0;
}

static int android_audio_open(void *ctx, uint32_t sample_rate,
                              uint32_t channels, uint32_t bits,
                              uintptr_t *out)
{
    (void)ctx;
    if (!out) return -1;
    *out = 0;
    if (bits != 8u && bits != 16u) return -1;
    if (sample_rate < 8000u || sample_rate > 192000u) return -1;
    if (channels < 1u || channels > 8u) return -1;

    AndroidAudioStream *st = (AndroidAudioStream *)calloc(1, sizeof *st);
    if (!st) return -1;
    st->sample_rate = sample_rate;
    st->channels = channels;
    st->bits = bits;

    if (pthread_mutex_init(&st->lifecycle, NULL) != 0) {
        free(st);
        return -1;
    }
    if (sem_init(&st->wake, 0, 0) != 0) {
        pthread_mutex_destroy(&st->lifecycle);
        free(st);
        return -1;
    }

    atomic_init(&st->callback_users, 0);
    atomic_init(&st->api_users, 0);
    atomic_init(&st->closing, 0);
    atomic_init(&st->silent, 0);
    atomic_init(&st->lost, 0);
    atomic_init(&st->lost_events, 0);
    atomic_init(&st->started, 0);
    atomic_init(&st->buffer_frames, 0);
    atomic_init(&st->last_xruns, 0);
    atomic_init(&st->pump_ticks, 0);

    AAudioStream *stream = audio_create_stream(st, 1);
    if (!stream) {
        sem_destroy(&st->wake);
        pthread_mutex_destroy(&st->lifecycle);
        free(st);
        return -1;
    }
    st->stream = stream;

    /* One mixer at the ACTUAL device rate owns one primary looping voice.
     * The guest rate/format is a voice property only; rate conversion belongs
     * to wc3_mixer_render(), never to the HLE. */
    android_audio_register_stream(st);
    uint32_t guest_bpf = channels * (bits / 8u);
    uint32_t primary_bytes = android_audio_primary_ring_bytes(
        st->sample_rate, guest_bpf);
    if (primary_bytes == 0u ||
        wc3_mixer_init(&st->mixer,
                       st->device_rate ? st->device_rate : st->sample_rate,
                       st->channels, st->float_output) != 0 ||
        android_audio_primary_add(st, st->sample_rate, st->channels, st->bits,
                                  primary_bytes,
                                  &st->primary_voice) != 0) {
        AUDIO_LOG("primary voice unavailable; audio open fails");
        st->primary_voice = 0;
        (void)AAudioStream_requestStop(stream);
        (void)AAudioStream_close(stream);
        st->stream = NULL;
        sem_destroy(&st->wake);
        pthread_mutex_destroy(&st->lifecycle);
        android_audio_unregister_stream(st);
        for (unsigned i = 0; i < WC3_MIXER_VOICES; i++)
            free(st->voice_pcm[i]);
        wc3_mixer_deinit(&st->mixer);
        free(st);
        return -1;
    }

    if (pthread_create(&st->supervisor, NULL, audio_supervisor, st) != 0) {
        (void)AAudioStream_requestStop(stream);
        (void)AAudioStream_close(stream);
        while (atomic_load_explicit(&st->callback_users,
                                    memory_order_acquire) != 0)
            android_poll_pause();
        sem_destroy(&st->wake);
        pthread_mutex_destroy(&st->lifecycle);
        android_audio_unregister_stream(st);
        for (unsigned i = 0; i < WC3_MIXER_VOICES; i++) free(st->voice_pcm[i]);
        wc3_mixer_deinit(&st->mixer);
        free(st);
        return -1;
    }
    st->supervisor_started = 1;
    /* N-16b: arm the optional post-mix tap before requestStart(), so the
     * first callback is captured.  Failure is diagnostic-only. */
    android_audio_tap_start(st);

    if (android_audio_ensure_started(st) != 0) {
        AUDIO_LOG("AAudio failed to start during open");
        atomic_store_explicit(&st->closing, 1, memory_order_release);
        (void)sem_post(&st->wake);
        (void)pthread_join(st->supervisor, NULL);
        st->supervisor_started = 0;
        pthread_mutex_lock(&st->lifecycle);
        AAudioStream *failed = st->stream;
        st->stream = NULL;
        pthread_mutex_unlock(&st->lifecycle);
        if (failed) {
            (void)AAudioStream_requestStop(failed);
            (void)AAudioStream_close(failed);
        }
        while (atomic_load_explicit(&st->callback_users,
                                    memory_order_acquire) != 0)
            android_poll_pause();
        android_audio_tap_stop(st);
        sem_destroy(&st->wake);
        pthread_mutex_destroy(&st->lifecycle);
        android_audio_unregister_stream(st);
        for (unsigned i = 0; i < WC3_MIXER_VOICES; i++)
            free(st->voice_pcm[i]);
        wc3_mixer_deinit(&st->mixer);
        free(st);
        return -1;
    }

    *out = (uintptr_t)st;
    return 0;
}

/* ---- nfsu2 host-owned looping voices --------------------------------- */
/* Voice handles encode the owning stream so the platform table can stay
 * ctx-free: high 16 bits = stream slot + 1, low 16 bits = mixer voice + 1. */
#define WC3_AUDIO_STREAM_SLOTS 8u
static AndroidAudioStream *g_audio_streams[WC3_AUDIO_STREAM_SLOTS];
static _Atomic uint32_t g_audio_stream_next;
/* Serializes stream table publication against lookup+ref.  Control path only;
 * the RT callback never touches this table or this lock. */
static pthread_mutex_t g_audio_stream_lock = PTHREAD_MUTEX_INITIALIZER;

static void android_audio_register_stream(AndroidAudioStream *st)
{
    uint32_t slot = atomic_fetch_add_explicit(&g_audio_stream_next, 1u,
                                              memory_order_relaxed) %
                    WC3_AUDIO_STREAM_SLOTS;
    st->stream_slot = slot + 1u;
    pthread_mutex_lock(&g_audio_stream_lock);
    g_audio_streams[slot] = st;
    pthread_mutex_unlock(&g_audio_stream_lock);
}

static void android_audio_unregister_stream(AndroidAudioStream *st)
{
    if (!st || !st->stream_slot || st->stream_slot > WC3_AUDIO_STREAM_SLOTS)
        return;
    pthread_mutex_lock(&g_audio_stream_lock);
    if (g_audio_streams[st->stream_slot - 1u] == st)
        g_audio_streams[st->stream_slot - 1u] = NULL;
    pthread_mutex_unlock(&g_audio_stream_lock);
}

static void android_audio_stream_release(AndroidAudioStream *st)
{
    if (st) atomic_fetch_sub_explicit(&st->api_users, 1, memory_order_acq_rel);
}

/* Find a stream and take an api_users ref while the table lock is held, so
 * close() cannot unregister and free it between lookup and ref. */
static AndroidAudioStream *android_audio_stream_acquire(AndroidAudioStream *hint)
{
    AndroidAudioStream *st = NULL;
    pthread_mutex_lock(&g_audio_stream_lock);
    if (hint) {
        for (unsigned i = 0; i < WC3_AUDIO_STREAM_SLOTS; i++) {
            if (g_audio_streams[i] == hint) { st = hint; break; }
        }
    } else {
        st = g_audio_streams[0];
    }
    if (st && !atomic_load_explicit(&st->closing, memory_order_acquire)) {
        atomic_fetch_add_explicit(&st->api_users, 1, memory_order_acq_rel);
    } else {
        st = NULL;
    }
    pthread_mutex_unlock(&g_audio_stream_lock);
    return st;
}

static AndroidAudioStream *android_audio_stream_acquire_slot(uint32_t slot)
{
    AndroidAudioStream *st = NULL;
    pthread_mutex_lock(&g_audio_stream_lock);
    if (slot && slot <= WC3_AUDIO_STREAM_SLOTS)
        st = g_audio_streams[slot - 1u];
    if (st && !atomic_load_explicit(&st->closing, memory_order_acquire)) {
        atomic_fetch_add_explicit(&st->api_users, 1, memory_order_acq_rel);
    } else {
        st = NULL;
    }
    pthread_mutex_unlock(&g_audio_stream_lock);
    return st;
}

static Wc3Voice *android_audio_voice(uintptr_t voice,
                                     AndroidAudioStream **out_stream,
                                     uintptr_t *out_local)
{
    uint32_t slot = (uint32_t)(voice >> 16);
    uintptr_t local = voice & 0xFFFFu;
    if (!slot || slot > WC3_AUDIO_STREAM_SLOTS || !local) return NULL;
    AndroidAudioStream *st = android_audio_stream_acquire_slot(slot);
    if (!st) return NULL;
    Wc3Voice *v = wc3_mixer_voice(&st->mixer, local);
    if (!v) {
        android_audio_stream_release(st);
        return NULL;
    }
    if (out_stream) *out_stream = st;
    if (out_local) *out_local = local;
    return v;
}

static int android_audio_voice_add(void *ctx, uintptr_t handle,
                                   uint32_t rate, uint32_t channels,
                                   uint32_t bits, uint32_t bytes,
                                   uintptr_t *out_voice)
{
    (void)ctx;
    /* DirectSoundCreate has no WINMM stream handle: fall back to the first
     * registered device, which already owns the primary voice. */
    AndroidAudioStream *st = android_audio_stream_acquire(
        (AndroidAudioStream *)handle);
    if (!st || !out_voice || !st->stream_slot) {
        android_audio_stream_release(st);
        return -1;
    }
    *out_voice = 0;
    uint32_t slots = (bytes + WC3_VOICE_SLOT_BYTES - 1u) /
                     WC3_VOICE_SLOT_BYTES;
    if (slots == 0u || slots > WC3_VOICE_MAX_SLOTS ||
        bytes > WC3_VOICE_MAX_SLOTS * WC3_VOICE_SLOT_BYTES) {
        android_audio_stream_release(st);
        return -1;
    }
    uint8_t *pcm = (uint8_t *)malloc((size_t)slots * WC3_VOICE_SLOT_BYTES);
    if (!pcm) {
        android_audio_stream_release(st);
        return -1;
    }
    uintptr_t local = 0;
    if (wc3_mixer_voice_add(&st->mixer, pcm, bytes, rate, channels, bits,
                            &local) != 0) {
        free(pcm);
        android_audio_stream_release(st);
        return -1;
    }
    st->voice_pcm[(size_t)local - 1u] = pcm;
    *out_voice = ((uintptr_t)st->stream_slot << 16) | local;
    android_audio_stream_release(st);
    return 0;
}

static int android_audio_voice_remove(void *ctx, uintptr_t voice)
{
    (void)ctx;
    AndroidAudioStream *st = NULL;
    uintptr_t local = 0;
    Wc3Voice *v = android_audio_voice(voice, &st, &local);
    if (!v) return -1;
    if (wc3_mixer_voice_remove(&st->mixer, local) != 0) {
        android_audio_stream_release(st);
        return -1;
    }
    android_audio_wait_render_idle(&st->mixer);
    while (atomic_load_explicit(&v->publishing, memory_order_acquire) != 0)
        android_poll_pause();
    while (atomic_load_explicit(&v->resetting, memory_order_acquire) != 0)
        android_poll_pause();
    wc3_mixer_voice_release(v);
    free(st->voice_pcm[(size_t)local - 1u]);
    st->voice_pcm[(size_t)local - 1u] = NULL;
    android_audio_stream_release(st);
    return 0;
}

static int android_audio_voice_publish(void *ctx, uintptr_t voice,
                                       uint64_t frame_base,
                                       const void *frames, uint32_t bytes,
                                       uint32_t *written)
{
    (void)ctx;
    if (written) *written = 0;
    AndroidAudioStream *st = NULL;
    Wc3Voice *v = android_audio_voice(voice, &st, NULL);
    if (!v || !frames || bytes == 0u) {
        android_audio_stream_release(st);
        return -1;
    }
    uint32_t got = wc3_voice_publish(v, frame_base, frames, bytes);
    if (!got) {
        android_audio_stream_release(st);
        return -1;
    }
    if (written) *written = got;
    android_audio_ensure_started(st);
    android_audio_stream_release(st);
    return 0;
}

static int android_audio_voice_publish2(void *ctx, uintptr_t voice,
                                        uint64_t frame_base1,
                                        const void *frames1, uint32_t bytes1,
                                        uint64_t frame_base2,
                                        const void *frames2, uint32_t bytes2,
                                        uint32_t *written)
{
    (void)ctx;
    if (written) *written = 0;
    AndroidAudioStream *st = NULL;
    Wc3Voice *v = android_audio_voice(voice, &st, NULL);
    if (!v || (bytes1 != 0u && !frames1) ||
        (bytes2 != 0u && !frames2) || (bytes1 == 0u && bytes2 == 0u)) {
        android_audio_stream_release(st);
        return -1;
    }
    uint32_t got = wc3_voice_publish2(v, frame_base1, frames1, bytes1,
                                      frame_base2, frames2, bytes2);
    if (!got) {
        android_audio_stream_release(st);
        return -1;
    }
    if (written) *written = got;
    android_audio_ensure_started(st);
    android_audio_stream_release(st);
    return 0;
}

static int android_audio_voice_published(void *ctx, uintptr_t voice)
{
    (void)ctx;
    AndroidAudioStream *st = NULL;
    Wc3Voice *v = android_audio_voice(voice, &st, NULL);
    if (!v) return -1;
    int rc = wc3_voice_published(v);
    if (rc != 0) {
        android_audio_stream_release(st);
        return rc;
    }
    /* A zero-byte Unlock carries no PCM, so it must not start the device on
     * its own; the first real publish does that. */
    android_audio_stream_release(st);
    return 0;
}
static int android_audio_voice_position(void *ctx, uintptr_t voice,
                                        uint32_t *play_bytes,
                                        uint32_t *write_bytes,
                                        uint64_t *play_frame)
{
    (void)ctx;
    AndroidAudioStream *st = NULL;
    Wc3Voice *v = android_audio_voice(voice, &st, NULL);
    if (!v) return -1;
    wc3_voice_position(v, st->mixer.rate, play_bytes, write_bytes, play_frame);
    android_audio_stream_release(st);
    return 0;
}

static int android_audio_voice_control(void *ctx, uintptr_t voice,
                                       int playing, int loop,
                                       uint32_t frequency, int volume,
                                       int pan)
{
    (void)ctx;
    AndroidAudioStream *st = NULL;
    Wc3Voice *v = android_audio_voice(voice, &st, NULL);
    if (!v) return -1;
    wc3_voice_control(v, playing, loop, frequency, volume, pan);
    android_audio_stream_release(st);
    return 0;
}

static int android_audio_voice_seek(void *ctx, uintptr_t voice,
                                    uint64_t abs_frame)
{
    (void)ctx;
    AndroidAudioStream *st = NULL;
    Wc3Voice *v = android_audio_voice(voice, &st, NULL);
    if (!v) return -1;
    int rc = wc3_voice_seek(v, abs_frame);
    android_audio_stream_release(st);
    return rc;
}
static int android_audio_voice_state(void *ctx, uintptr_t voice,
                                     uint32_t *playing, uint32_t *loop,
                                     uint64_t *position)
{
    (void)ctx;
    AndroidAudioStream *st = NULL;
    Wc3Voice *v = android_audio_voice(voice, &st, NULL);
    if (!v) return -1;
    int rc = wc3_voice_state(v, playing, loop, position);
    android_audio_stream_release(st);
    return rc;
}

static uint32_t android_audio_voice_device_rate(void *ctx, uintptr_t voice)
{
    (void)ctx;
    AndroidAudioStream *st = NULL;
    Wc3Voice *v = android_audio_voice(voice, &st, NULL);
    if (!v || !st) return 0u;
    uint32_t rate = st->mixer.rate;
    android_audio_stream_release(st);
    return rate;
}

static uint32_t android_audio_device_rate(void *ctx, uintptr_t handle)
{
    (void)ctx;
    AndroidAudioStream *st = (AndroidAudioStream *)handle;
    return st ? st->device_rate : 0u;
}

static uintptr_t android_audio_primary_voice(void *ctx, uintptr_t handle)
{
    (void)ctx;
    AndroidAudioStream *st = (AndroidAudioStream *)handle;
    /* DirectSoundCreate has no stream handle; use the same first registered
     * stream fallback as voice_add(). */
    if (!st) st = g_audio_streams[0];
    return st ? st->primary_voice : 0u;
}

static int android_audio_close(void *ctx, uintptr_t handle)
{
    (void)ctx;
    AndroidAudioStream *st = (AndroidAudioStream *)handle;
    if (!st) return -1;
    if (atomic_exchange_explicit(&st->closing, 1, memory_order_acq_rel))
        return -1;

    /* Stop new voice lookups first.  A publisher that already took api_users
     * is waited out below; one that starts now sees closing and fails. */
    android_audio_unregister_stream(st);

    (void)sem_post(&st->wake);
    if (st->supervisor_started) {
        (void)pthread_join(st->supervisor, NULL);
    } else {
        pthread_mutex_lock(&st->lifecycle);
        AAudioStream *stream = st->stream;
        st->stream = NULL;
        pthread_mutex_unlock(&st->lifecycle);
        if (stream) {
            (void)AAudioStream_requestStop(stream);
            (void)AAudioStream_close(stream);
        }
    }

    while (atomic_load_explicit(&st->callback_users, memory_order_acquire) != 0)
        android_poll_pause();
    /* N-16b: no callback can push after callback_users reaches zero.  Stop the
     * writer and drain the fixture before any mixer/PCM storage is freed. */
    android_audio_tap_stop(st);
    /* C-8: no publish/remove/reset may still hold this stream.  Registered
     * voice calls cannot enter after the unregister above. */
    while (atomic_load_explicit(&st->api_users, memory_order_acquire) != 0)
        android_poll_pause();

    sem_destroy(&st->wake);
    pthread_mutex_destroy(&st->lifecycle);
    for (unsigned i = 0; i < WC3_MIXER_VOICES; i++) free(st->voice_pcm[i]);
    wc3_mixer_deinit(&st->mixer);
    free(st);
    return 0;
}

/* -------------------------------------------------------------------- log */

static void android_log_write(void *ctx, int priority, const char *tag,
                              const char *text)
{
    (void)ctx;
#if WC3_DEBUG
    __android_log_write(priority, tag ? tag : "wc3", text ? text : "");
#else
    (void)priority;
    (void)tag;
    (void)text;
#endif
}

/* ------------------------------------------------------------------- init */

int wc3_android_platform_init(Wc3Platform *platform)
{
    if (!platform) return -1;
    memset(platform, 0, sizeof *platform);

    platform->mem.ctx = NULL;
    platform->mem.reserve = android_mem_reserve;
    platform->mem.commit = android_mem_commit;
    platform->mem.decommit = android_mem_decommit;
    platform->mem.release = android_mem_release;
    platform->mem.protect = android_mem_protect;

    platform->file.ctx = NULL;
    platform->file.open = android_file_open;
    platform->file.read = android_file_read;
    platform->file.write = android_file_write;
    platform->file.seek = android_file_seek;
    platform->file.size = android_file_size;
    platform->file.stat = android_file_stat;
    platform->file.close = android_file_close;

    platform->time.ctx = NULL;
    platform->time.monotonic_ns = android_monotonic_ns;
    platform->time.realtime_ns = android_realtime_ns;
    platform->time.local_time = android_local_time;

    platform->thread.ctx = NULL;
    platform->thread.create = android_thread_create;
    platform->thread.join = android_thread_join;
    platform->thread.detach = android_thread_detach;
    platform->thread.self = android_thread_self;
    platform->thread.id = android_thread_id;
    platform->thread.yield = android_thread_yield;
    platform->thread.sleep_ns = android_thread_sleep_ns;
    platform->thread.tls_alloc = android_tls_alloc;
    platform->thread.tls_free = android_tls_free;
    platform->thread.tls_set = android_tls_set;
    platform->thread.tls_get = android_tls_get;

    platform->sync.ctx = NULL;
    platform->sync.event_create = android_event_create;
    platform->sync.event_set = android_event_set;
    platform->sync.event_reset = android_event_reset;
    platform->sync.mutex_create = android_mutex_create;
    platform->sync.mutex_release = android_mutex_release;
    platform->sync.semaphore_create = android_semaphore_create;
    platform->sync.semaphore_release = android_semaphore_release;
    platform->sync.wait = android_wait;
    platform->sync.close = android_sync_close;
    platform->sync.critical_section_create = android_cs_create;
    platform->sync.critical_section_enter = android_cs_enter;
    platform->sync.critical_section_try_enter = android_cs_try_enter;
    platform->sync.critical_section_leave = android_cs_leave;

    android_input_reset();
    platform->input.ctx = NULL;
    platform->input.push = android_input_push;
    platform->input.set_text_focus = android_input_set_text_focus;

    platform->surface.ctx = NULL;
    platform->surface.acquire = android_surface_acquire;
    platform->surface.release = android_surface_release;
    platform->surface.resize = android_surface_resize;
    platform->surface.lost = android_surface_lost;

    platform->audio.ctx = NULL;
    platform->audio.open = android_audio_open;
    platform->audio.close = android_audio_close;
    platform->audio.voice_add = android_audio_voice_add;
    platform->audio.voice_remove = android_audio_voice_remove;
    platform->audio.voice_publish = android_audio_voice_publish;
    platform->audio.voice_position = android_audio_voice_position;
    platform->audio.voice_control = android_audio_voice_control;
    platform->audio.device_rate = android_audio_device_rate;
    platform->audio.primary_voice = android_audio_primary_voice;
    platform->audio.voice_seek = android_audio_voice_seek;
    platform->audio.voice_state = android_audio_voice_state;
    platform->audio.voice_device_rate = android_audio_voice_device_rate;
    platform->audio.voice_publish2 = android_audio_voice_publish2;
    platform->audio.voice_published = android_audio_voice_published;

    platform->log.ctx = NULL;
    platform->log.write = android_log_write;

    platform->config.ctx = NULL;
    platform->config.get = android_config_get;
    platform->config.get_int = android_config_get_int;

    platform->asset.ctx = NULL;
    platform->asset.init = android_asset_init;
    platform->asset.asset_root = android_asset_root;
    platform->asset.save_root = android_save_root;
    platform->asset.enumerate_mpq = android_asset_enumerate;
    platform->asset.present = android_asset_present;

    wc3_android_net_install(&platform->net);

    (void)pthread_once(&g_thread_once, android_thread_key_init);
    memset(&g_main_thread, 0, sizeof g_main_thread);
    g_main_thread.thread = pthread_self();
    g_main_thread.id = android_tid();
    strcpy(g_main_thread.name, "wc3-main");
    return 0;
}
