/*
 * headless_thread.c - Wc3ThreadApi with real host threads.
 *
 * Every guest thread is a real pthread, as required by AGENTS.md.  The
 * handle owns a small record with the entry, exit code and detach state so
 * join/detach never depend on the guest TEB or on a thread-local shortcut.
 */
#include "headless_internal.h"

#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN (64u * 1024u)
#endif

typedef struct HeadlessThread {
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
    HeadlessState *state;
    HeadlessSchedThread *sched;
} HeadlessThread;

static pthread_key_t g_thread_key;
static pthread_once_t g_thread_once = PTHREAD_ONCE_INIT;
static HeadlessThread g_main_thread;

static uint64_t headless_tid(void)
{
    return (uint64_t)(uintptr_t)pthread_self();
}

static void headless_thread_key_init(void)
{
    (void)pthread_key_create(&g_thread_key, NULL);
}

static void headless_thread_release(HeadlessThread *t)
{
    headless_sched_unregister(t->state, t->sched);
    pthread_cond_destroy(&t->cond);
    pthread_mutex_destroy(&t->lock);
    free(t);
}

static void *headless_thread_main(void *opaque)
{
    HeadlessThread *t = (HeadlessThread *)opaque;
    (void)pthread_setspecific(g_thread_key, t);
    t->id = headless_tid();
    headless_sched_start(t->state, t->sched);

    int rc = t->entry(t->arg);

    (void)pthread_mutex_lock(&t->lock);
    t->exit_code = rc;
    t->running = 0;
    (void)pthread_cond_broadcast(&t->cond);
    t->refs--;
    int free_it = (t->refs == 0u);
    (void)pthread_mutex_unlock(&t->lock);
    /* Release abandoned mutex/CS ownership before the scheduler lets a waiter
     * run; otherwise the waiter can re-block on an object that the TLS
     * destructor only clears after the token has already been handed over. */
    headless_sync_thread_exit();
    headless_sched_exit(t->state, t->sched);
    if (free_it) headless_thread_release(t);
    return NULL;
}

static int headless_thread_create(void *ctx, const Wc3ThreadDesc *desc,
                                  Wc3ThreadHandle *out)
{
    HeadlessState *st = (HeadlessState *)ctx;
    if (!desc || !desc->entry || !out) return -1;
    (void)pthread_once(&g_thread_once, headless_thread_key_init);

    HeadlessThread *t = (HeadlessThread *)calloc(1, sizeof *t);
    if (!t) return -1;
    t->state = st;
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

    t->sched = headless_sched_register(st, t);
    if (headless_sched_active(st) && !t->sched) {
        pthread_cond_destroy(&t->cond);
        pthread_mutex_destroy(&t->lock);
        free(t);
        return -1;
    }

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        headless_sched_unregister(st, t->sched);
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
    int rc = pthread_create(&t->thread, &attr, headless_thread_main, t);
    (void)pthread_attr_destroy(&attr);
    if (rc != 0) {
        headless_sched_unregister(st, t->sched);
        pthread_cond_destroy(&t->cond);
        pthread_mutex_destroy(&t->lock);
        free(t);
        return -1;
    }
    *out = (Wc3ThreadHandle)(uintptr_t)t;
    return 0;
}

static int headless_thread_join(void *ctx, Wc3ThreadHandle handle,
                                uint32_t *exit_code)
{
    (void)ctx;
    HeadlessThread *t = (HeadlessThread *)(uintptr_t)handle;
    if (!t) return -1;

    (void)pthread_mutex_lock(&t->lock);
    int detached = t->detached;
    (void)pthread_mutex_unlock(&t->lock);
    if (detached) return -1;

    if (headless_sched_active(t->state)) {
        for (;;) {
            (void)pthread_mutex_lock(&t->lock);
            int running = t->running;
            (void)pthread_mutex_unlock(&t->lock);
            if (!running) break;
            if (headless_sched_wait_join(t->state, t->sched) != 0)
                return -1;
        }
    }

    if (pthread_join(t->thread, NULL) != 0) return -1;

    (void)pthread_mutex_lock(&t->lock);
    if (exit_code) *exit_code = (uint32_t)t->exit_code;
    t->refs--;
    int free_it = (t->refs == 0u);
    (void)pthread_mutex_unlock(&t->lock);
    if (free_it) headless_thread_release(t);
    return 0;
}

static int headless_thread_detach(void *ctx, Wc3ThreadHandle handle)
{
    (void)ctx;
    HeadlessThread *t = (HeadlessThread *)(uintptr_t)handle;
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
    if (free_it) headless_thread_release(t);
    return 0;
}

static Wc3ThreadHandle headless_thread_self(void *ctx)
{
    (void)ctx;
    (void)pthread_once(&g_thread_once, headless_thread_key_init);
    void *p = pthread_getspecific(g_thread_key);
    if (p) return (Wc3ThreadHandle)(uintptr_t)p;
    return (Wc3ThreadHandle)(uintptr_t)&g_main_thread;
}

static uint64_t headless_thread_id(void *ctx, Wc3ThreadHandle handle)
{
    (void)ctx;
    HeadlessThread *t = (HeadlessThread *)(uintptr_t)handle;
    return t ? t->id : 0u;
}

static void headless_thread_yield(void *ctx)
{
    HeadlessState *st = (HeadlessState *)ctx;
    if (headless_sched_active(st)) {
        headless_sched_yield(st);
        return;
    }
    sched_yield();
}

static int headless_thread_sleep_ns(void *ctx, uint64_t ns)
{
    HeadlessState *st = (HeadlessState *)ctx;
    if (headless_sched_active(st)) return headless_sched_sleep(st, ns);
    if (st && st->virtual_clock) return headless_clock_advance(st, ns);

    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    while (nanosleep(&ts, &ts) != 0) {
        if (errno != EINTR) return -1;
    }
    return 0;
}

static pthread_key_t g_tls_keys[HEADLESS_TLS_MAX];
static uint8_t g_tls_used[HEADLESS_TLS_MAX];
static pthread_mutex_t g_tls_lock = PTHREAD_MUTEX_INITIALIZER;

static int headless_tls_alloc(void *ctx, uint32_t *index)
{
    (void)ctx;
    if (!index) return -1;
    (void)pthread_mutex_lock(&g_tls_lock);
    for (uint32_t i = 0; i < HEADLESS_TLS_MAX; i++) {
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

static int headless_tls_free(void *ctx, uint32_t index)
{
    (void)ctx;
    if (index >= HEADLESS_TLS_MAX) return -1;
    (void)pthread_mutex_lock(&g_tls_lock);
    int ok = g_tls_used[index] != 0;
    if (ok) {
        (void)pthread_key_delete(g_tls_keys[index]);
        g_tls_used[index] = 0;
    }
    (void)pthread_mutex_unlock(&g_tls_lock);
    return ok ? 0 : -1;
}

static int headless_tls_set(void *ctx, uint32_t index, uintptr_t value)
{
    (void)ctx;
    if (index >= HEADLESS_TLS_MAX) return -1;
    (void)pthread_mutex_lock(&g_tls_lock);
    int ok = g_tls_used[index] != 0;
    pthread_key_t key = g_tls_keys[index];
    (void)pthread_mutex_unlock(&g_tls_lock);
    if (!ok) return -1;
    return pthread_setspecific(key, (void *)value) == 0 ? 0 : -1;
}

static uintptr_t headless_tls_get(void *ctx, uint32_t index)
{
    (void)ctx;
    if (index >= HEADLESS_TLS_MAX) return 0u;
    (void)pthread_mutex_lock(&g_tls_lock);
    int ok = g_tls_used[index] != 0;
    pthread_key_t key = g_tls_keys[index];
    (void)pthread_mutex_unlock(&g_tls_lock);
    if (!ok) return 0u;
    return (uintptr_t)pthread_getspecific(key);
}

void headless_thread_install(Wc3Platform *platform, HeadlessState *st)
{
    (void)pthread_once(&g_thread_once, headless_thread_key_init);
    memset(&g_main_thread, 0, sizeof g_main_thread);
    g_main_thread.thread = pthread_self();
    g_main_thread.id = headless_tid();
    strcpy(g_main_thread.name, "wc3-main");

    platform->thread.ctx = st;
    platform->thread.create = headless_thread_create;
    platform->thread.join = headless_thread_join;
    platform->thread.detach = headless_thread_detach;
    platform->thread.self = headless_thread_self;
    platform->thread.id = headless_thread_id;
    platform->thread.yield = headless_thread_yield;
    platform->thread.sleep_ns = headless_thread_sleep_ns;
    platform->thread.tls_alloc = headless_tls_alloc;
    platform->thread.tls_free = headless_tls_free;
    platform->thread.tls_set = headless_tls_set;
    platform->thread.tls_get = headless_tls_get;
}
