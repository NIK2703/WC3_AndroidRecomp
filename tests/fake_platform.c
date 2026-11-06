#include "fake_platform.h"

#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct FakeThread {
    pthread_t thread;
    int (*entry)(void *arg);
    void *arg;
    uint32_t exit_code;
} FakeThread;

static void *fake_thread_main(void *opaque)
{
    FakeThread *t = (FakeThread *)opaque;
    t->exit_code = (uint32_t)t->entry(t->arg);
    return NULL;
}

static int fake_create(void *ctx, const Wc3ThreadDesc *desc, Wc3ThreadHandle *out)
{
    (void)ctx;
    if (!desc || !desc->entry || !out) return -1;
    FakeThread *t = (FakeThread *)calloc(1, sizeof *t);
    if (!t) return -1;
    t->entry = desc->entry;
    t->arg = desc->arg;
    int rc = pthread_create(&t->thread, NULL, fake_thread_main, t);
    if (rc != 0) {
        free(t);
        return -1;
    }
    *out = (Wc3ThreadHandle)(uintptr_t)t;
    return 0;
}

static int fake_join(void *ctx, Wc3ThreadHandle handle, uint32_t *exit_code)
{
    (void)ctx;
    FakeThread *t = (FakeThread *)(uintptr_t)handle;
    if (!t) return -1;
    int rc = pthread_join(t->thread, NULL);
    if (rc != 0) return -1;
    if (exit_code) *exit_code = t->exit_code;
    free(t);
    return 0;
}

static int fake_detach(void *ctx, Wc3ThreadHandle handle)
{
    (void)ctx;
    FakeThread *t = (FakeThread *)(uintptr_t)handle;
    if (!t) return -1;
    /* The detached thread owns its FakeThread until it exits; a test that uses
     * detach must not expect join to recover the allocation. */
    return pthread_detach(t->thread) == 0 ? 0 : -1;
}

static Wc3ThreadHandle fake_self(void *ctx)
{
    (void)ctx;
    return (Wc3ThreadHandle)(uintptr_t)pthread_self();
}

static uint64_t fake_id(void *ctx, Wc3ThreadHandle handle)
{
    (void)ctx;
    return (uint64_t)handle;
}

static void fake_yield(void *ctx)
{
    (void)ctx;
    sched_yield();
}

static int fake_sleep_ns(void *ctx, uint64_t ns)
{
    (void)ctx;
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000u);
    ts.tv_nsec = (long)(ns % 1000000000u);
    return nanosleep(&ts, NULL) == 0 ? 0 : -1;
}

int wc3_fake_platform_init(Wc3Platform *platform)
{
    if (!platform) return -1;
    memset(platform, 0, sizeof *platform);
    platform->thread.ctx = NULL;
    platform->thread.create = fake_create;
    platform->thread.join = fake_join;
    platform->thread.detach = fake_detach;
    platform->thread.self = fake_self;
    platform->thread.id = fake_id;
    platform->thread.yield = fake_yield;
    platform->thread.sleep_ns = fake_sleep_ns;
    return 0;
}
