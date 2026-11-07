/*
 * wc3_lock_trace.c - see wc3_lock_trace.h.
 *
 * Diagnostic only.  The probe answers one question that a snapshot of a
 * frozen process cannot: which guest thread acquired the registry lock and
 * never released it.  It records completed acquisitions and releases for one
 * lock address, keeps a per-thread ownership depth, and dumps that table when
 * an acquire call has been in flight for longer than any legitimate critical
 * section.
 */
#include "wc3_debug.h"

#if WC3_DEBUG && WC3_DEBUG_HLE

#include "wc3_lock_trace.h"
#include "wc3_watch_state.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

/* Public lock entry points in game.dll.  L_6F0964F0 acquires, L_6F0965B0
 * releases; both are called with ecx = lock, edx = 1 (exclusive) / 0 (shared).
 * They tail-call the implementation, so the dispatcher invocation that starts
 * at one of these addresses covers the complete acquire/release, including a
 * 60 s wait loop. */
#define LT_ACQUIRE_VA 0x6F0964F0u
#define LT_RELEASE_VA 0x6F0965B0u

enum {
    LT_OP_ACQ_EXCL = 1,
    LT_OP_ACQ_SHARED = 2,
    LT_OP_REL_EXCL = 3,
    LT_OP_REL_SHARED = 4
};

#define LT_MAX_THREADS 32
#define LT_RING_SIZE 1024
#define LT_DUMP_EVENTS 128
#define LT_INFLIGHT_LIMIT_MS 10000u
#define LT_HOLD_LIMIT_MS 20000u
#define LT_DUMP_INTERVAL_MS 30000u
#define LT_HB_INTERVAL_MS 60000u
#define LT_POLL_MS 2000u

typedef struct {
    uint32_t gid;
    uint32_t host_tid;
    int used;
    int depth;                 /* completed acquisitions not yet released */
    uint64_t hold_since_ms;

    uint32_t acq_op, acq_arg, acq_ra, acq_caller;
    uint64_t acq_ms;
    uint32_t rel_op, rel_arg, rel_ra, rel_caller;
    uint64_t rel_ms;
    uint32_t acq_n, rel_n;

    int inflight;
    uint32_t inf_op, inf_arg, inf_ra, inf_caller;
    uint64_t inf_ms;
} LtThread;

typedef struct {
    uint64_t seq;
    uint64_t ms;
    uint32_t gid;
    uint32_t host_tid;
    uint32_t op;
    uint32_t lock;
    uint32_t ra;
    uint32_t caller;
    uint32_t arg;
    uint32_t done;
} LtEvent;

static pthread_mutex_t g_lt_mu = PTHREAD_MUTEX_INITIALIZER;
static LtThread g_lt_threads[LT_MAX_THREADS];
static LtEvent g_lt_ring[LT_RING_SIZE];
static uint32_t g_lt_head;
static uint32_t g_lt_count;
static uint64_t g_lt_seq;
static uint32_t g_lt_target = WC3_LOCK_TRACE_DEFAULT_VA;
static volatile int g_lt_seen;
static volatile int g_lt_started;
static uint64_t g_lt_last_dump_ms;
static uint64_t g_lt_last_hb_ms;

static uint64_t lt_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t lt_host_tid(void)
{
#if defined(__linux__)
    return (uint32_t)syscall(__NR_gettid);
#else
    return 0;
#endif
}

static const char *lt_op_name(uint32_t op)
{
    switch (op) {
    case LT_OP_ACQ_EXCL:   return "acq-x";
    case LT_OP_ACQ_SHARED: return "acq-s";
    case LT_OP_REL_EXCL:   return "rel-x";
    case LT_OP_REL_SHARED: return "rel-s";
    default:               return "?";
    }
}

/* Print the hang-watchdog state of one traced thread.  This is what turns a
 * holder gid into an actionable answer: the last guest target and the HLE
 * object it is waiting on at the moment the lock stops moving. */
static void lt_dump_state_locked(const LtThread *t)
{
    Wc3WatchState ws;
    if (!wc3_watch_state_get(t->gid, &ws) || !ws.valid)
        return;
    fprintf(stderr,
            "[lock] state gid=%u main=%d run=%d last=%08X eip=%08X "
            "esp=%08X ebp=%08X n=%u wait=%d wra=%08X h0=%08X h1=%08X\n",
            ws.gid, ws.main_thread, ws.running, ws.last_target, ws.last_eip,
            ws.last_esp, ws.last_ebp, ws.dispatch_count, ws.wait_active,
            ws.wait_ra, ws.wait_h0, ws.wait_h1);
}

static LtThread *lt_thread_locked(uint32_t gid, int create)
{
    for (int i = 0; i < LT_MAX_THREADS; i++) {
        if (g_lt_threads[i].used && g_lt_threads[i].gid == gid)
            return &g_lt_threads[i];
    }
    if (!create)
        return NULL;
    for (int i = 0; i < LT_MAX_THREADS; i++) {
        if (!g_lt_threads[i].used) {
            memset(&g_lt_threads[i], 0, sizeof g_lt_threads[i]);
            g_lt_threads[i].used = 1;
            g_lt_threads[i].gid = gid;
            g_lt_threads[i].host_tid = lt_host_tid();
            return &g_lt_threads[i];
        }
    }
    return NULL;
}

static void lt_ring_add_locked(uint32_t gid, uint32_t host_tid, uint32_t op,
                               uint32_t lock, uint32_t ra, uint32_t caller,
                               uint32_t arg, uint32_t done)
{
    LtEvent *e = &g_lt_ring[g_lt_head];
    e->seq = ++g_lt_seq;
    e->ms = lt_now_ms();
    e->gid = gid;
    e->host_tid = host_tid;
    e->op = op;
    e->lock = lock;
    e->ra = ra;
    e->caller = caller;
    e->arg = arg;
    e->done = done;
    g_lt_head = (g_lt_head + 1u) % LT_RING_SIZE;
    if (g_lt_count < LT_RING_SIZE)
        g_lt_count++;
}

int wc3_lock_trace_enter(const CPU *c, uint32_t target,
                         uint32_t *op, uint32_t *lock, uint32_t *arg,
                         uint32_t *ra, uint32_t *caller, uint32_t *gid)
{
    uint32_t kind;

    if (target == LT_ACQUIRE_VA)
        kind = c->edx ? LT_OP_ACQ_EXCL : LT_OP_ACQ_SHARED;
    else if (target == LT_RELEASE_VA)
        kind = c->edx ? LT_OP_REL_EXCL : LT_OP_REL_SHARED;
    else
        return 0;

    if (c->ecx != g_lt_target)
        return 0;

    uint32_t guest_id = 0;
    if (c->fs_base)
        guest_id = *(volatile uint32_t *)(uintptr_t)(c->fs_base + 0x24u);
    uint32_t host_id = lt_host_tid();

    uint32_t ret = 0, call_ret = 0;
    if (c->esp) {
        const volatile uint32_t *sp =
            (const volatile uint32_t *)(uintptr_t)c->esp;
        ret = sp[0];
        call_ret = sp[2];   /* [ret][arg][caller_ret] */
    }

    pthread_mutex_lock(&g_lt_mu);
    LtThread *t = lt_thread_locked(guest_id, 1);
    if (t) {
        t->inflight = 1;
        t->inf_op = kind;
        t->inf_arg = c->edx;
        t->inf_ra = ret;
        t->inf_caller = call_ret;
        t->inf_ms = lt_now_ms();
        t->host_tid = host_id;
    }
    lt_ring_add_locked(guest_id, host_id, kind, c->ecx, ret, call_ret,
                       c->edx, 0);
    g_lt_seen = 1;
    pthread_mutex_unlock(&g_lt_mu);

    *op = kind;
    *lock = c->ecx;
    *arg = c->edx;
    *ra = ret;
    *caller = call_ret;
    *gid = guest_id;
    return 1;
}

void wc3_lock_trace_leave(uint32_t op, uint32_t lock, uint32_t arg,
                          uint32_t ra, uint32_t caller, uint32_t gid)
{
    if (lock != g_lt_target)
        return;

    pthread_mutex_lock(&g_lt_mu);
    LtThread *t = lt_thread_locked(gid, 1);
    if (t) {
        t->inflight = 0;
        t->host_tid = lt_host_tid();
        if (op == LT_OP_ACQ_EXCL || op == LT_OP_ACQ_SHARED) {
            if (t->depth == 0)
                t->hold_since_ms = lt_now_ms();
            t->depth++;
            t->acq_op = op;
            t->acq_arg = arg;
            t->acq_ra = ra;
            t->acq_caller = caller;
            t->acq_ms = lt_now_ms();
            t->acq_n++;
        } else {
            if (t->depth > 0)
                t->depth--;
            if (t->depth == 0)
                t->hold_since_ms = 0;
            t->rel_op = op;
            t->rel_arg = arg;
            t->rel_ra = ra;
            t->rel_caller = caller;
            t->rel_ms = lt_now_ms();
            t->rel_n++;
        }
    }
    lt_ring_add_locked(gid, t ? t->host_tid : lt_host_tid(), op, lock, ra,
                       caller, arg, 1);
    pthread_mutex_unlock(&g_lt_mu);
}

static int lt_enabled(void)
{
    if (getenv("WC3_HANG_WATCH"))
        return 1;
    const char *dir = getenv("WC3_DATA_DIR");
    char trigger[1200];
    snprintf(trigger, sizeof trigger, "%s/hang_watch_on",
             (dir && *dir) ? dir : ".");
    return access(trigger, F_OK) == 0;
}

static void lt_dump_locked(uint64_t now, const char *why, uint32_t gid,
                           uint64_t age)
{
    volatile uint32_t *lw = (volatile uint32_t *)(uintptr_t)g_lt_target;

    fprintf(stderr, "[lock] STUCK why=%s gid=%u age=%llums t=%llums\n",
            why, gid, (unsigned long long)age,
            (unsigned long long)now);
    fprintf(stderr, "[lock] lock=%08X words=%08X %08X %08X %08X\n",
            g_lt_target, lw[0], lw[1], lw[2], lw[3]);

    for (int i = 0; i < LT_MAX_THREADS; i++) {
        LtThread *t = &g_lt_threads[i];
        if (!t->used)
            continue;
        if (t->inflight) {
            fprintf(stderr,
                    "[lock] inflight gid=%u tid=%u op=%s arg=%u age=%llums "
                    "ra=%08X caller=%08X\n",
                    t->gid, t->host_tid, lt_op_name(t->inf_op), t->inf_arg,
                    (unsigned long long)(now - t->inf_ms), t->inf_ra,
                    t->inf_caller);
        }
        if (t->depth > 0) {
            fprintf(stderr,
                    "[lock] holder gid=%u tid=%u depth=%d op=%s arg=%u "
                    "held=%llums acq_ra=%08X acq_caller=%08X "
                    "acq_n=%u rel_n=%u\n",
                    t->gid, t->host_tid, t->depth, lt_op_name(t->acq_op),
                    t->acq_arg,
                    (unsigned long long)(t->hold_since_ms ?
                                         now - t->hold_since_ms : 0),
                    t->acq_ra, t->acq_caller, t->acq_n, t->rel_n);
        }
        if (t->inflight || t->depth > 0)
            lt_dump_state_locked(t);
        if (t->rel_n) {
            fprintf(stderr,
                    "[lock] lastrel gid=%u tid=%u op=%s arg=%u ago=%llums "
                    "ra=%08X caller=%08X\n",
                    t->gid, t->host_tid, lt_op_name(t->rel_op), t->rel_arg,
                    (unsigned long long)(now - t->rel_ms), t->rel_ra,
                    t->rel_caller);
        }
    }

    uint32_t n = g_lt_count < LT_DUMP_EVENTS ? g_lt_count : LT_DUMP_EVENTS;
    fprintf(stderr, "[lock] ring last=%u of %u\n", n, g_lt_count);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (g_lt_head + LT_RING_SIZE - n + i) % LT_RING_SIZE;
        LtEvent *e = &g_lt_ring[idx];
        fprintf(stderr,
                "[lock] ev seq=%llu t=%llums gid=%u tid=%u op=%s %s "
                "lock=%08X ra=%08X caller=%08X arg=%u\n",
                (unsigned long long)e->seq, (unsigned long long)e->ms,
                e->gid, e->host_tid, lt_op_name(e->op),
                e->done ? "done" : "enter", e->lock, e->ra, e->caller,
                e->arg);
    }
}

static void *lt_monitor(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(LT_POLL_MS * 1000u);
        if (!g_lt_seen)
            continue;

        uint64_t now = lt_now_ms();
        uint32_t why_gid = 0;
        uint64_t why_age = 0;
        const char *why = NULL;

        pthread_mutex_lock(&g_lt_mu);
        if (now - g_lt_last_hb_ms >= LT_HB_INTERVAL_MS) {
            g_lt_last_hb_ms = now;
            uint32_t acq = 0, rel = 0, holders = 0, inflight = 0;
            for (int i = 0; i < LT_MAX_THREADS; i++) {
                LtThread *t = &g_lt_threads[i];
                if (!t->used) continue;
                acq += t->acq_n;
                rel += t->rel_n;
                if (t->depth > 0) holders++;
                if (t->inflight) inflight++;
            }
            volatile uint32_t *lw =
                (volatile uint32_t *)(uintptr_t)g_lt_target;
            fprintf(stderr,
                    "[lock] hb t=%llums acq=%u rel=%u holders=%u "
                    "inflight=%u words=%08X %08X %08X %08X\n",
                    (unsigned long long)now, acq, rel, holders, inflight,
                    lw[0], lw[1], lw[2], lw[3]);
        }
        for (int i = 0; i < LT_MAX_THREADS; i++) {
            LtThread *t = &g_lt_threads[i];
            if (!t->used)
                continue;
            if (t->inflight && now - t->inf_ms >= LT_INFLIGHT_LIMIT_MS) {
                why = "inflight";
                why_gid = t->gid;
                why_age = now - t->inf_ms;
                break;
            }
            if (t->depth > 0 && t->hold_since_ms &&
                now - t->hold_since_ms >= LT_HOLD_LIMIT_MS) {
                why = "held";
                why_gid = t->gid;
                why_age = now - t->hold_since_ms;
                break;
            }
        }

        if (why && now - g_lt_last_dump_ms >= LT_DUMP_INTERVAL_MS) {
            g_lt_last_dump_ms = now;
            lt_dump_locked(now, why, why_gid, why_age);
        }
        pthread_mutex_unlock(&g_lt_mu);
    }
    return NULL;
}

void wc3_lock_trace_start(void)
{
    if (g_lt_started)
        return;
    g_lt_started = 1;

    const char *va = getenv("WC3_LOCK_TRACE_VA");
    if (va && *va)
        g_lt_target = (uint32_t)strtoul(va, NULL, 16);

    if (!lt_enabled())
        return;

    fprintf(stderr, "[lock] trace armed target=%08X\n", g_lt_target);

    pthread_t th;
    if (pthread_create(&th, NULL, lt_monitor, NULL) == 0) {
#if defined(__ANDROID__)
        pthread_setname_np(th, "wc3-locktrace");
#endif
        pthread_detach(th);
    }
}

#endif /* WC3_DEBUG && WC3_DEBUG_HLE */
