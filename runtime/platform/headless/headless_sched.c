/*
 * headless_sched.c - cooperative T1 schedule for guest threads.
 *
 * With a virtual schedule installed, exactly one registered guest thread owns
 * a token.  A thread releases it at yield, virtual sleep, a blocking sync
 * wait, or join.  This is a deterministic cooperative model, not a
 * preemptive CPU scheduler: guest code that never calls a platform blocking
 * point cannot be preempted.  It is sufficient for the T1 schedule set because
 * every guest thread in the runtime reaches the platform at its waits, and it
 * keeps the frozen Wc3ThreadApi unchanged.
 *
 * The schedule does not model guest priorities: Wc3ThreadDesc has no priority
 * field.  FIFO/LIFO/WAKE_REORDER therefore reorder thread creation and wake-up,
 * while ROUND_ROBIN visits ready threads in creation order.  A true priority
 * inversion schedule needs a platform contract change, which is tracked as a
 * separate ABI/design item rather than faked here.
 */
#include "headless_internal.h"

#include <stdlib.h>

static int sched_eligible(const HeadlessSchedThread *thread)
{
    return thread->ready && !thread->blocked && !thread->exited;
}

static HeadlessSchedThread *sched_select(HeadlessState *st,
                                         HeadlessSchedThread *skip)
{
    HeadlessSchedThread *best = NULL;

    if (st->schedule_policy == WC3_HEADLESS_SCHED_ROUND_ROBIN) {
        HeadlessSchedThread *start = skip ? skip->next : st->sched_threads;
        for (HeadlessSchedThread *it = start; it; it = it->next) {
            if (it != skip && sched_eligible(it)) return it;
        }
        for (HeadlessSchedThread *it = st->sched_threads; it != start && it;
             it = it->next) {
            if (it != skip && sched_eligible(it)) return it;
        }
        return skip && sched_eligible(skip) ? skip : NULL;
    }

    for (HeadlessSchedThread *it = st->sched_threads; it; it = it->next) {
        if (it == skip || !sched_eligible(it)) continue;
        if (!best) {
            best = it;
            continue;
        }
        if (st->schedule_policy == WC3_HEADLESS_SCHED_FIFO) {
            if (it->seq < best->seq) best = it;
        } else if (st->schedule_policy == WC3_HEADLESS_SCHED_LIFO) {
            if (it->seq > best->seq) best = it;
        } else { /* WAKE_REORDER: latest wake first, then latest creation */
            if (it->wake_seq > best->wake_seq ||
                (it->wake_seq == best->wake_seq && it->seq > best->seq))
                best = it;
        }
    }
    if (best) return best;
    return skip && sched_eligible(skip) ? skip : NULL;
}

static int sched_pump_locked(HeadlessState *st)
{
    uint64_t earliest = UINT64_MAX;
    for (HeadlessSchedThread *it = st->sched_threads; it; it = it->next) {
        if (!it->blocked || it->deadline_ns == UINT64_MAX) continue;
        if (it->deadline_ns < earliest) earliest = it->deadline_ns;
    }
    if (earliest == UINT64_MAX) return -1; /* no runnable thread and no timer */

    const uint64_t now = headless_now_ns(st);
    if (earliest < now) earliest = now;
    if (headless_clock_advance(st, earliest - now) != 0) return -1;

    for (HeadlessSchedThread *it = st->sched_threads; it; it = it->next) {
        if (!it->blocked || it->deadline_ns > earliest) continue;
        it->blocked = 0;
        it->ready = 1;
        if (it->block_reason != HEADLESS_BLOCK_SLEEP) it->timed_out = 1;
        it->wake_seq = ++st->sched_wake_seq;
    }
    return 0;
}

/* Switch away from `self`.  Returns 1 when another thread got the token,
 * 0 when self remains current, -1 when no thread can run. */
static int sched_switch_locked(HeadlessState *st, HeadlessSchedThread *self)
{
    HeadlessSchedThread *next = sched_select(st, self);
    if (!next) {
        if (sched_pump_locked(st) != 0) return -1;
        next = sched_select(st, self);
        if (!next) return -1;
    }
    if (next == self) return 0;
    st->sched_current = next;
    (void)pthread_cond_broadcast(&st->sched_cond);
    return 1;
}

static void sched_wait_token(HeadlessState *st, HeadlessSchedThread *self)
{
    while (st->sched_current != self)
        (void)pthread_cond_wait(&st->sched_cond, &st->sched_lock);
}

int headless_sched_init(HeadlessState *st)
{
    if (!st) return -1;
    if (!st->virtual_clock) {
        st->sched_enabled = 0;
        return 0;
    }

    HeadlessSchedThread *main_thread =
        (HeadlessSchedThread *)calloc(1, sizeof *main_thread);
    if (!main_thread) return -1;
    if (pthread_cond_init(&main_thread->cond, NULL) != 0) {
        free(main_thread);
        return -1;
    }
    main_thread->pthread = pthread_self();
    main_thread->seq = st->sched_next_seq++;
    main_thread->deadline_ns = UINT64_MAX;
    main_thread->ready = 1;
    st->sched_threads = main_thread;
    st->sched_current = main_thread;
    st->sched_enabled = 1;
    return 0;
}

void headless_sched_free(HeadlessState *st)
{
    if (!st || !st->sched_enabled) return;
    (void)pthread_mutex_lock(&st->sched_lock);
    HeadlessSchedThread *it = st->sched_threads;
    st->sched_threads = NULL;
    st->sched_current = NULL;
    st->sched_enabled = 0;
    (void)pthread_mutex_unlock(&st->sched_lock);
    while (it) {
        HeadlessSchedThread *next = it->next;
        (void)pthread_cond_destroy(&it->cond);
        free(it);
        it = next;
    }
}

int headless_sched_active(const HeadlessState *st)
{
    return st && st->sched_enabled;
}

HeadlessSchedThread *headless_sched_register(HeadlessState *st, void *owner)
{
    if (!headless_sched_active(st)) return NULL;
    HeadlessSchedThread *thread =
        (HeadlessSchedThread *)calloc(1, sizeof *thread);
    if (!thread) return NULL;
    if (pthread_cond_init(&thread->cond, NULL) != 0) {
        free(thread);
        return NULL;
    }
    thread->owner = owner;
    thread->deadline_ns = UINT64_MAX;
    thread->ready = 1;

    int wake = 0;
    (void)pthread_mutex_lock(&st->sched_lock);
    thread->seq = st->sched_next_seq++;
    HeadlessSchedThread **tail = &st->sched_threads;
    while (*tail) tail = &(*tail)->next;
    *tail = thread;
    if (!st->sched_current) {
        HeadlessSchedThread *next = sched_select(st, NULL);
        if (next) {
            st->sched_current = next;
            wake = 1;
        }
    }
    (void)pthread_mutex_unlock(&st->sched_lock);
    if (wake) (void)pthread_cond_broadcast(&st->sched_cond);
    return thread;
}

void headless_sched_unregister(HeadlessState *st, HeadlessSchedThread *thread)
{
    if (!st || !thread) return;
    (void)pthread_mutex_lock(&st->sched_lock);
    HeadlessSchedThread **pp = &st->sched_threads;
    while (*pp && *pp != thread) pp = &(*pp)->next;
    if (*pp) *pp = thread->next;
    if (st->sched_current == thread) st->sched_current = NULL;
    (void)pthread_mutex_unlock(&st->sched_lock);
    (void)pthread_cond_destroy(&thread->cond);
    free(thread);
}

void headless_sched_start(HeadlessState *st, HeadlessSchedThread *thread)
{
    if (!headless_sched_active(st) || !thread) return;
    (void)pthread_mutex_lock(&st->sched_lock);
    thread->pthread = pthread_self();
    while (st->sched_current != thread)
        (void)pthread_cond_wait(&st->sched_cond, &st->sched_lock);
    (void)pthread_mutex_unlock(&st->sched_lock);
}

void headless_sched_exit(HeadlessState *st, HeadlessSchedThread *thread)
{
    if (!headless_sched_active(st) || !thread) return;
    (void)pthread_mutex_lock(&st->sched_lock);
    thread->exited = 1;
    thread->ready = 0;
    thread->blocked = 0;

    for (HeadlessSchedThread *it = st->sched_threads; it; it = it->next) {
        if (!it->blocked) continue;
        if (it->block_reason == HEADLESS_BLOCK_JOIN &&
            it->wait_target == thread) {
            it->blocked = 0;
            it->ready = 1;
            it->wait_target = NULL;
            it->wake_seq = ++st->sched_wake_seq;
        } else if (it->block_reason == HEADLESS_BLOCK_SYNC) {
            /* The exiting thread may have abandoned a mutex.  Wake sync
             * waiters so they re-check their objects; this is a spurious
             * wake, never a fake success. */
            it->blocked = 0;
            it->ready = 1;
            it->timed_out = 0;
            it->wake_seq = ++st->sched_wake_seq;
        }
    }

    HeadlessSchedThread *next = sched_select(st, NULL);
    if (!next && sched_pump_locked(st) == 0)
        next = sched_select(st, NULL);
    st->sched_current = next;
    (void)pthread_cond_broadcast(&st->sched_cond);
    (void)pthread_mutex_unlock(&st->sched_lock);
}

void headless_sched_yield(HeadlessState *st)
{
    if (!headless_sched_active(st)) return;
    (void)pthread_mutex_lock(&st->sched_lock);
    HeadlessSchedThread *self = st->sched_current;
    if (!self || !pthread_equal(self->pthread, pthread_self())) {
        (void)pthread_mutex_unlock(&st->sched_lock);
        return;
    }
    const int rc = sched_switch_locked(st, self);
    if (rc > 0) sched_wait_token(st, self);
    (void)pthread_mutex_unlock(&st->sched_lock);
}

int headless_sched_sleep(HeadlessState *st, uint64_t ns)
{
    if (!headless_sched_active(st)) return headless_clock_advance(st, ns);
    if (ns == 0u) {
        headless_sched_yield(st);
        return 0;
    }

    (void)pthread_mutex_lock(&st->sched_lock);
    HeadlessSchedThread *self = st->sched_current;
    if (!self || !pthread_equal(self->pthread, pthread_self())) {
        (void)pthread_mutex_unlock(&st->sched_lock);
        return headless_clock_advance(st, ns);
    }
    const uint64_t now = headless_now_ns(st);
    if (UINT64_MAX - now < ns) {
        (void)pthread_mutex_unlock(&st->sched_lock);
        return -1;
    }
    self->blocked = 1;
    self->ready = 0;
    self->block_reason = HEADLESS_BLOCK_SLEEP;
    self->deadline_ns = now + ns;
    self->timed_out = 0;
    self->wake_seq = 0u;

    int rc = sched_switch_locked(st, self);
    if (rc < 0) {
        self->blocked = 0;
        self->ready = 1;
        self->deadline_ns = UINT64_MAX;
        self->block_reason = 0;
        (void)pthread_mutex_unlock(&st->sched_lock);
        return -1;
    }
    if (rc > 0) sched_wait_token(st, self);
    self->blocked = 0;
    self->ready = 1;
    self->deadline_ns = UINT64_MAX;
    self->block_reason = 0;
    (void)pthread_mutex_unlock(&st->sched_lock);
    return 0;
}

int headless_sched_block(HeadlessState *st, uint32_t timeout_ms, int reason,
                         int *timed_out)
{
    uint64_t deadline = UINT64_MAX;
    if (timeout_ms != UINT32_MAX) {
        const uint64_t add = (uint64_t)timeout_ms * 1000000ull;
        const uint64_t now = headless_now_ns(st);
        if (UINT64_MAX - now >= add) deadline = now + add;
    }
    return headless_sched_block_until(st, deadline, reason, timed_out);
}

int headless_sched_block_until(HeadlessState *st, uint64_t deadline_ns,
                               int reason, int *timed_out)
{
    if (!headless_sched_active(st)) return -1;
    (void)pthread_mutex_lock(&st->sched_lock);
    HeadlessSchedThread *self = st->sched_current;
    if (!self || !pthread_equal(self->pthread, pthread_self())) {
        (void)pthread_mutex_unlock(&st->sched_lock);
        return -1;
    }

    self->blocked = 1;
    self->ready = 0;
    self->block_reason = reason;
    self->deadline_ns = deadline_ns;
    self->timed_out = 0;
    self->wait_target = NULL;
    self->wake_seq = 0u;

    int rc = sched_switch_locked(st, self);
    if (rc < 0) {
        self->blocked = 0;
        self->ready = 1;
        self->deadline_ns = UINT64_MAX;
        self->block_reason = 0;
        (void)pthread_mutex_unlock(&st->sched_lock);
        return -1;
    }
    if (rc > 0) sched_wait_token(st, self);
    if (timed_out) *timed_out = self->timed_out;
    self->blocked = 0;
    self->ready = 1;
    self->deadline_ns = UINT64_MAX;
    self->block_reason = 0;
    self->timed_out = 0;
    self->wait_target = NULL;
    (void)pthread_mutex_unlock(&st->sched_lock);
    return 0;
}

int headless_sched_wait_join(HeadlessState *st, HeadlessSchedThread *target)
{
    if (!headless_sched_active(st) || !target) return -1;
    (void)pthread_mutex_lock(&st->sched_lock);
    HeadlessSchedThread *self = st->sched_current;
    if (!self || !pthread_equal(self->pthread, pthread_self())) {
        (void)pthread_mutex_unlock(&st->sched_lock);
        return -1;
    }
    self->blocked = 1;
    self->ready = 0;
    self->block_reason = HEADLESS_BLOCK_JOIN;
    self->deadline_ns = UINT64_MAX;
    self->timed_out = 0;
    self->wait_target = target;
    self->wake_seq = 0u;

    int rc = sched_switch_locked(st, self);
    if (rc < 0) {
        self->blocked = 0;
        self->ready = 1;
        self->block_reason = 0;
        self->wait_target = NULL;
        (void)pthread_mutex_unlock(&st->sched_lock);
        return -1;
    }
    if (rc > 0) sched_wait_token(st, self);
    self->blocked = 0;
    self->ready = 1;
    self->block_reason = 0;
    self->wait_target = NULL;
    (void)pthread_mutex_unlock(&st->sched_lock);
    return 0;
}

void headless_sched_wake_sync(HeadlessState *st)
{
    if (!headless_sched_active(st)) return;
    (void)pthread_mutex_lock(&st->sched_lock);
    for (HeadlessSchedThread *it = st->sched_threads; it; it = it->next) {
        if (!it->blocked || it->block_reason != HEADLESS_BLOCK_SYNC) continue;
        it->blocked = 0;
        it->ready = 1;
        it->timed_out = 0;
        it->wake_seq = ++st->sched_wake_seq;
    }
    if (!st->sched_current) {
        HeadlessSchedThread *next = sched_select(st, NULL);
        if (next) {
            st->sched_current = next;
            (void)pthread_cond_broadcast(&st->sched_cond);
        }
    }
    (void)pthread_mutex_unlock(&st->sched_lock);
}
