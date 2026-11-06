/*
 * sync_bench.c - uncontended cost of the guest-visible critical section paths.
 *
 * Reproduces docs/ARCHITECTURE_1.27B.md section 3.7 (P0-4 / P0-7):
 *   A: pthread recursive mutex                     (per-object mutex target)
 *   B: global mutex + linear scan of 512 entries   (as-built cs_get)
 *   C: C11 atomic fast path, recursion counted     (Win32-like CRITICAL_SECTION)
 *
 * Host-side only; no Android APIs.  Build:
 *   gcc -std=c11 -O2 -pthread tools/sync_bench.c -o build/host/sync_bench
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define N 1000000
#define TABLE 512

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint32_t addr; int used; } g_cs[TABLE];
static int g_cs_count;
static volatile uint32_t sink;      /* prevents dead-code elimination */
static pthread_mutex_t g_obj[TABLE];

static void bench_recursive(void) {
    pthread_mutex_t m;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m, &a);
    double t0 = now_s();
    for (int i = 0; i < N; ++i) { pthread_mutex_lock(&m); pthread_mutex_unlock(&m); }
    double dt = now_s() - t0;
    printf("A recursive mutex          : %8.2f ns/op\n", dt * 1e9 / N);
    pthread_mutex_destroy(&m);
    pthread_mutexattr_destroy(&a);
}

static void bench_global_scan(void) {
    for (int i = 0; i < TABLE; ++i) { g_cs[i].addr = 0x1000u + (uint32_t)i; g_cs[i].used = 1; }
    g_cs_count = TABLE;
    double t0 = now_s();
    for (int i = 0; i < N; ++i) {
        uint32_t target = 0x1000u + (uint32_t)(i % TABLE);
        uint32_t found = 0;
        pthread_mutex_lock(&g_lock);
        for (int j = 0; j < g_cs_count; ++j) {
            if (g_cs[j].addr == target) { found = g_cs[j].addr; break; }
        }
        pthread_mutex_unlock(&g_lock);
        sink = found;
    }
    double dt = now_s() - t0;
    printf("B global lock + scan(512)  : %8.2f ns/op\n", dt * 1e9 / N);
}

static void bench_direct_mapped(void) {
    for (int i = 0; i < TABLE; ++i) pthread_mutex_init(&g_obj[i], NULL);
    double t0 = now_s();
    for (int i = 0; i < N; ++i) {
        uint32_t addr = 0x1000u + (uint32_t)(i % TABLE);
        pthread_mutex_t *m = &g_obj[(addr - 0x1000u) & (TABLE - 1)];
        pthread_mutex_lock(m);
        sink = addr;
        pthread_mutex_unlock(m);
    }
    double dt = now_s() - t0;
    printf("D direct-mapped + obj mutex: %8.2f ns/op\n", dt * 1e9 / N);
    for (int i = 0; i < TABLE; ++i) pthread_mutex_destroy(&g_obj[i]);
}

typedef struct { _Atomic uint32_t owner; uint32_t depth; } FastCS;
static FastCS fcs;
static _Thread_local uint32_t tid = 1;

static inline void fast_enter(FastCS *s) {
    uint32_t expected = 0;
    if (atomic_compare_exchange_strong_explicit(&s->owner, &expected, tid,
                                                memory_order_acquire,
                                                memory_order_relaxed)) {
        s->depth = 1;
    } else if (expected == tid) {
        ++s->depth;
    }
    /* Contended slow path (futex/condvar) is deliberately omitted: this
     * measures the uncontended fast path only. */
}
static inline void fast_leave(FastCS *s) {
    if (s->depth > 1) --s->depth;
    else { s->depth = 0; atomic_store_explicit(&s->owner, 0, memory_order_release); }
}

static void bench_atomic(void) {
    atomic_store(&fcs.owner, 0);
    fcs.depth = 0;
    double t0 = now_s();
    for (int i = 0; i < N; ++i) { fast_enter(&fcs); fast_leave(&fcs); }
    double dt = now_s() - t0;
    printf("C atomic fast path         : %8.2f ns/op\n", dt * 1e9 / N);
}

int main(void) {
    printf("sync_bench: %d uncontended enter/leave pairs each\n", N);
    bench_recursive();
    bench_global_scan();
    bench_direct_mapped();
    bench_atomic();
    return 0;
}
