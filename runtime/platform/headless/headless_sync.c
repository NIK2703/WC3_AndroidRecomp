/*
 * headless_sync.c - Wc3SyncApi with Win32-visible semantics.
 *
 * The objects are host-side records guarded by one platform mutex and one
 * condition variable.  No lock is held across a guest callback: wait/acquire
 * returns to the caller before dispatch can run.  A mutex owned by a thread
 * that exits becomes abandoned; a critical section owned by a dead thread is
 * permanently locked, matching Windows.
 */
#include "headless_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

typedef enum HeadlessSyncKind {
    HEADLESS_SYNC_EVENT = 1,
    HEADLESS_SYNC_MUTEX = 2,
    HEADLESS_SYNC_SEMAPHORE = 3,
    HEADLESS_SYNC_CRITICAL = 4
} HeadlessSyncKind;

typedef struct HeadlessSync {
    struct HeadlessSync *next;
    struct HeadlessSync *owner_next;
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
} HeadlessSync;

static pthread_once_t g_sync_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_sync_lock;
static pthread_cond_t g_sync_cond;
static pthread_key_t g_owner_key;
static int g_owner_key_ready;
static HeadlessSync *g_sync_head;

static void headless_owner_thread_exit(void *value);

static void headless_owner_thread_exit(void *value)
{
    HeadlessSync *list = (HeadlessSync *)value;
    if (!list) return;

    (void)pthread_mutex_lock(&g_sync_lock);
    int wake = 0;
    while (list) {
        HeadlessSync *next = list->owner_next;
        list->owner_next = NULL;
        if (list->kind == HEADLESS_SYNC_MUTEX) {
            list->owner = 0;
            list->recursion = 0u;
            list->abandoned = 1;
            wake = 1;
        } else if (list->kind == HEADLESS_SYNC_CRITICAL) {
            list->owner = 0;
            list->recursion = 0u;
            list->owner_dead = 1;
        }
        list = next;
    }
    if (wake) (void)pthread_cond_broadcast(&g_sync_cond);
    (void)pthread_mutex_unlock(&g_sync_lock);
}

static void headless_owner_add(HeadlessSync *p)
{
    if (!g_owner_key_ready) return;
    HeadlessSync *head = (HeadlessSync *)pthread_getspecific(g_owner_key);
    p->owner_next = head;
    if (pthread_setspecific(g_owner_key, p) != 0) p->owner_next = NULL;
}

static void headless_owner_remove(HeadlessSync *p)
{
    if (!g_owner_key_ready) return;
    HeadlessSync *head = (HeadlessSync *)pthread_getspecific(g_owner_key);
    HeadlessSync *prev = NULL;
    HeadlessSync *it = head;
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

static void headless_sync_init(void)
{
    pthread_mutex_init(&g_sync_lock, NULL);
    if (pthread_key_create(&g_owner_key, headless_owner_thread_exit) == 0)
        g_owner_key_ready = 1;
    /* The default condition clock is CLOCK_REALTIME on both glibc and
     * winpthreads; deadlines below use the same clock. */
    (void)pthread_cond_init(&g_sync_cond, NULL);
}

static HeadlessSync *headless_sync_lookup(Wc3SyncHandle handle)
{
    HeadlessSync *p = (HeadlessSync *)(uintptr_t)handle;
    for (HeadlessSync *it = g_sync_head; it; it = it->next) {
        if (it == p) return it;
    }
    return NULL;
}

static HeadlessSync *headless_sync_new(uint32_t kind)
{
    HeadlessSync *p = (HeadlessSync *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->magic = HEADLESS_SYNC_MAGIC;
    p->kind = kind;
    p->next = g_sync_head;
    g_sync_head = p;
    return p;
}

static int headless_event_create(void *ctx, int manual_reset, int initial_state,
                                 Wc3SyncHandle *out)
{
    (void)ctx;
    if (!out) return -1;
    (void)pthread_once(&g_sync_once, headless_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    HeadlessSync *p = headless_sync_new(HEADLESS_SYNC_EVENT);
    if (p) {
        p->manual_reset = manual_reset != 0;
        p->signaled = initial_state != 0;
        *out = (Wc3SyncHandle)(uintptr_t)p;
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    return p ? 0 : -1;
}

static int headless_event_set(void *ctx, Wc3SyncHandle handle)
{
    (void)ctx;
    (void)pthread_once(&g_sync_once, headless_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    HeadlessSync *p = headless_sync_lookup(handle);
    int ok = p && p->kind == HEADLESS_SYNC_EVENT;
    if (ok) {
        p->signaled = 1;
        (void)pthread_cond_broadcast(&g_sync_cond);
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    return ok ? 0 : -1;
}

static int headless_event_reset(void *ctx, Wc3SyncHandle handle)
{
    (void)ctx;
    (void)pthread_once(&g_sync_once, headless_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    HeadlessSync *p = headless_sync_lookup(handle);
    int ok = p && p->kind == HEADLESS_SYNC_EVENT;
    if (ok) p->signaled = 0;
    (void)pthread_mutex_unlock(&g_sync_lock);
    return ok ? 0 : -1;
}

static int headless_mutex_create(void *ctx, int initial_owner,
                                 Wc3SyncHandle *out)
{
    (void)ctx;
    if (!out) return -1;
    (void)pthread_once(&g_sync_once, headless_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    HeadlessSync *p = headless_sync_new(HEADLESS_SYNC_MUTEX);
    if (p) {
        if (initial_owner) {
            p->owner = pthread_self();
            p->recursion = 1u;
            headless_owner_add(p);
        }
        *out = (Wc3SyncHandle)(uintptr_t)p;
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    return p ? 0 : -1;
}

static int headless_mutex_release(void *ctx, Wc3SyncHandle handle)
{
    (void)ctx;
    (void)pthread_once(&g_sync_once, headless_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    HeadlessSync *p = headless_sync_lookup(handle);
    int ok = p && (p->kind == HEADLESS_SYNC_MUTEX ||
                   p->kind == HEADLESS_SYNC_CRITICAL) &&
             pthread_equal(p->owner, pthread_self()) && p->recursion > 0u;
    if (ok) {
        p->recursion--;
        if (p->recursion == 0u) {
            if (p->kind == HEADLESS_SYNC_MUTEX) p->abandoned = 0;
            p->owner = 0;
            headless_owner_remove(p);
        }
        (void)pthread_cond_broadcast(&g_sync_cond);
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    return ok ? 0 : -1;
}

static int headless_semaphore_create(void *ctx, int32_t initial,
                                     int32_t maximum, Wc3SyncHandle *out)
{
    (void)ctx;
    if (!out || initial < 0 || maximum <= 0 || initial > maximum) return -1;
    (void)pthread_once(&g_sync_once, headless_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    HeadlessSync *p = headless_sync_new(HEADLESS_SYNC_SEMAPHORE);
    if (p) {
        p->count = initial;
        p->maximum = maximum;
        *out = (Wc3SyncHandle)(uintptr_t)p;
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    return p ? 0 : -1;
}

static int headless_semaphore_release(void *ctx, Wc3SyncHandle handle,
                                      int32_t count, int32_t *previous)
{
    (void)ctx;
    if (count <= 0) return -1;
    (void)pthread_once(&g_sync_once, headless_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    HeadlessSync *p = headless_sync_lookup(handle);
    int ok = p && p->kind == HEADLESS_SYNC_SEMAPHORE &&
             count <= p->maximum - p->count;
    if (ok) {
        if (previous) *previous = p->count;
        p->count += count;
        (void)pthread_cond_broadcast(&g_sync_cond);
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    return ok ? 0 : -1;
}

static int headless_sync_ready(const HeadlessSync *p)
{
    switch (p->kind) {
    case HEADLESS_SYNC_EVENT:
        return p->signaled != 0;
    case HEADLESS_SYNC_MUTEX:
        return p->owner == 0 || pthread_equal(p->owner, pthread_self());
    case HEADLESS_SYNC_CRITICAL:
        return !p->owner_dead &&
               (p->owner == 0 || pthread_equal(p->owner, pthread_self()));
    case HEADLESS_SYNC_SEMAPHORE:
        return p->count > 0;
    default:
        return 0;
    }
}

static void headless_sync_consume(HeadlessSync *p)
{
    switch (p->kind) {
    case HEADLESS_SYNC_EVENT:
        if (!p->manual_reset) p->signaled = 0;
        break;
    case HEADLESS_SYNC_MUTEX:
    case HEADLESS_SYNC_CRITICAL:
        if (pthread_equal(p->owner, pthread_self())) {
            p->recursion++;
        } else {
            if (p->kind == HEADLESS_SYNC_MUTEX) p->abandoned = 0;
            p->owner = pthread_self();
            p->recursion = 1u;
            headless_owner_add(p);
        }
        break;
    case HEADLESS_SYNC_SEMAPHORE:
        p->count--;
        break;
    default:
        break;
    }
}

static int headless_sync_abandoned(const HeadlessSync *p)
{
    return p->kind == HEADLESS_SYNC_MUTEX && p->abandoned != 0;
}

static int headless_wait(void *ctx, const Wc3SyncHandle *objects, uint32_t count,
                         int wait_all, uint32_t timeout_ms, uint32_t *index)
{
    (void)ctx;
    if (!objects || count == 0u || count > HEADLESS_WAIT_MAX) {
        if (index) *index = 0u;
        return (int)WC3_WAIT_FAILED;
    }
    (void)pthread_once(&g_sync_once, headless_sync_init);

    struct timespec deadline;
    if (timeout_ms != UINT32_MAX) {
        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
            return (int)WC3_WAIT_FAILED;
        uint64_t add = (uint64_t)timeout_ms * 1000000ull;
        deadline.tv_sec += (time_t)(add / 1000000000ull);
        deadline.tv_nsec += (long)(add % 1000000000ull);
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    HeadlessSync *p[HEADLESS_WAIT_MAX];
    (void)pthread_mutex_lock(&g_sync_lock);
    for (uint32_t i = 0; i < count; i++) {
        p[i] = headless_sync_lookup(objects[i]);
        if (!p[i]) {
            (void)pthread_mutex_unlock(&g_sync_lock);
            if (index) *index = 0u;
            return (int)WC3_WAIT_FAILED;
        }
    }
    for (uint32_t i = 0; i < count; i++) p[i]->waiters++;

    for (;;) {
        int ready = wait_all ? 1 : 0;
        uint32_t first = 0u;
        for (uint32_t i = 0; i < count; i++) {
            int r = headless_sync_ready(p[i]);
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
                    if (headless_sync_abandoned(p[i])) abandoned = 1;
                    headless_sync_consume(p[i]);
                }
            } else {
                abandoned = headless_sync_abandoned(p[first]);
                headless_sync_consume(p[first]);
            }
            for (uint32_t i = 0; i < count; i++) p[i]->waiters--;
            (void)pthread_cond_broadcast(&g_sync_cond);
            (void)pthread_mutex_unlock(&g_sync_lock);
            if (index) *index = wait_all ? 0u : first;
            if (abandoned)
                return (int)WC3_WAIT_ABANDONED_0 +
                       (int)(wait_all ? 0u : first);
            return (int)WC3_WAIT_OBJECT_0 + (int)(wait_all ? 0u : first);
        }

        if (timeout_ms == 0u) {
            for (uint32_t i = 0; i < count; i++) p[i]->waiters--;
            (void)pthread_mutex_unlock(&g_sync_lock);
            if (index) *index = 0u;
            return (int)WC3_WAIT_TIMEOUT;
        }

        int rc;
        if (timeout_ms == UINT32_MAX) {
            rc = pthread_cond_wait(&g_sync_cond, &g_sync_lock);
        } else {
            rc = pthread_cond_timedwait(&g_sync_cond, &g_sync_lock, &deadline);
        }
        if (rc == ETIMEDOUT) {
            int ready_at_deadline = wait_all ? 1 : 0;
            uint32_t first_at_deadline = 0u;
            for (uint32_t i = 0; i < count; i++) {
                int r = headless_sync_ready(p[i]);
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
                for (uint32_t i = 0; i < count; i++) p[i]->waiters--;
                (void)pthread_mutex_unlock(&g_sync_lock);
                if (index) *index = 0u;
                return (int)WC3_WAIT_TIMEOUT;
            }
            int abandoned_at_deadline = 0;
            if (wait_all) {
                for (uint32_t i = 0; i < count; i++) {
                    if (headless_sync_abandoned(p[i]))
                        abandoned_at_deadline = 1;
                    headless_sync_consume(p[i]);
                }
            } else {
                abandoned_at_deadline =
                    headless_sync_abandoned(p[first_at_deadline]);
                headless_sync_consume(p[first_at_deadline]);
            }
            for (uint32_t i = 0; i < count; i++) p[i]->waiters--;
            (void)pthread_cond_broadcast(&g_sync_cond);
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

static int headless_sync_close(void *ctx, Wc3SyncHandle handle)
{
    (void)ctx;
    (void)pthread_once(&g_sync_once, headless_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    HeadlessSync **pp = &g_sync_head;
    HeadlessSync *p = NULL;
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
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    if (ok) free(p);
    return ok ? 0 : -1;
}

static int headless_cs_create(void *ctx, Wc3SyncHandle *out)
{
    (void)ctx;
    if (!out) return -1;
    (void)pthread_once(&g_sync_once, headless_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    HeadlessSync *p = headless_sync_new(HEADLESS_SYNC_CRITICAL);
    if (p) *out = (Wc3SyncHandle)(uintptr_t)p;
    (void)pthread_mutex_unlock(&g_sync_lock);
    return p ? 0 : -1;
}

static int headless_cs_enter(void *ctx, Wc3SyncHandle handle)
{
    (void)ctx;
    (void)pthread_once(&g_sync_once, headless_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    HeadlessSync *p = headless_sync_lookup(handle);
    int ok = p && p->kind == HEADLESS_SYNC_CRITICAL;
    if (ok) {
        if (pthread_equal(p->owner, pthread_self())) {
            p->recursion++;
        } else {
            p->waiters++;
            while (p->owner != 0 || p->owner_dead) {
                (void)pthread_cond_wait(&g_sync_cond, &g_sync_lock);
            }
            p->waiters--;
            p->owner = pthread_self();
            p->recursion = 1u;
            headless_owner_add(p);
        }
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    return ok ? 0 : -1;
}

static int headless_cs_try_enter(void *ctx, Wc3SyncHandle handle)
{
    (void)ctx;
    (void)pthread_once(&g_sync_once, headless_sync_init);
    (void)pthread_mutex_lock(&g_sync_lock);
    HeadlessSync *p = headless_sync_lookup(handle);
    int ok = p && p->kind == HEADLESS_SYNC_CRITICAL;
    int acquired = 0;
    if (ok && !p->owner_dead &&
        (p->owner == 0 || pthread_equal(p->owner, pthread_self()))) {
        if (pthread_equal(p->owner, pthread_self())) {
            p->recursion++;
        } else {
            p->owner = pthread_self();
            p->recursion = 1u;
            headless_owner_add(p);
        }
        acquired = 1;
    }
    (void)pthread_mutex_unlock(&g_sync_lock);
    return acquired;
}

static int headless_cs_leave(void *ctx, Wc3SyncHandle handle)
{
    return headless_mutex_release(ctx, handle);
}

void headless_sync_install(Wc3Platform *platform, HeadlessState *st)
{
    platform->sync.ctx = st;
    platform->sync.event_create = headless_event_create;
    platform->sync.event_set = headless_event_set;
    platform->sync.event_reset = headless_event_reset;
    platform->sync.mutex_create = headless_mutex_create;
    platform->sync.mutex_release = headless_mutex_release;
    platform->sync.semaphore_create = headless_semaphore_create;
    platform->sync.semaphore_release = headless_semaphore_release;
    platform->sync.wait = headless_wait;
    platform->sync.close = headless_sync_close;
    platform->sync.critical_section_create = headless_cs_create;
    platform->sync.critical_section_enter = headless_cs_enter;
    platform->sync.critical_section_try_enter = headless_cs_try_enter;
    platform->sync.critical_section_leave = headless_cs_leave;
}
