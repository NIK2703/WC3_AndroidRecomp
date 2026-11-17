/*
 * parallel_raycast.c - deterministic chunked L_6F749F90 dispatcher.
 *
 * The object-render loop in L_6F78D9B0 calls L_6F749F90 once per ray.  That
 * function owns two pieces of mutable state: its own guest stack frame and
 * the five output words passed by the caller.  Everything else in the
 * candidate loop - the container, the candidate objects, the candidate
 * arrays and the ray context - is only read.
 *
 * The wrapper therefore keeps the original lifted function as the single
 * source of truth and runs it on private worker CPUs over disjoint contiguous
 * ranges of top-level candidates.  Each worker gets its own container copy
 * (the object loop itself is untouched), its own guest stack and TEB, and its
 * own output words.  The main CPU is blocked in this wrapper for the whole
 * batch, so the shared reads cannot race with a game-state writer.
 *
 * Merge rules are the original ones: ranges are visited in increasing order,
 * a hit is taken when it is the first hit, and a later distance only replaces
 * it when it is strictly smaller.  That keeps the first candidate winning a
 * distance tie.
 *
 * Workers that ran the preceding job park on one shared futex, so a batch with
 * the same or fewer chunks needs one wakeup instead of one sem_post per
 * worker.  A growing batch sem_posts only the newly active workers.  Workers
 * outside the active set never read the stack RayJob.
 *
 * The dispatcher has an optional diagnostic text file:
 *
 *   <WC3_DATA_DIR>/raycast.cfg        (Android: app-private files directory)
 *   ./raycast.cfg                     (when WC3_DATA_DIR is unset)
 *
 * The file is opt-in: set WC3_RAY_TUNE=1 to load it.  Without that variable
 * the built-in automatic layout is used.  This matters because an old
 * diagnostic file must not silently pin `chunks` or `worker_cpu_mask` after
 * the code switches back to one worker per online CPU.
 *
 * Format: one or more `key=value` pairs separated by whitespace.  `#` starts
 * a comment.  Values can be changed by editing the file through `run-as` or
 * by pushing a new file; the next reload applies them without restarting the
 * process.  The file is checked every RAY_TUNE_CHECK_MASK+1 raycast calls.
 *
 * Keys:
 *   enabled=0|1             soft disable; keeps the pool alive
 *   threshold=auto|N        minimum top-level candidates for the pool;
 *                           `auto` is max(2*min_chunk, chunks*32)
 *   chunks=auto|N           active workers for a batch (1..pool size);
 *                           `auto` uses min(pool size, candidate count)
 *   min_chunk=N             minimum candidate count per split chunk
 *   stack_zero=N            bytes of worker stack cleared per chunk
 *                           (default 256; 4096 remains the diagnostic ceiling)
 *   container_copy=N        bytes copied from the caller's container
 *                           (default 336, the analyzed +0x14C maximum)
 *   verify=0|1              run the sequential reference and compare bits
 *   stats=0|1               print counters periodically
 *   timing=0|1              measure parallel and fallback wall time
 *   spin_us=N               workers busy-wait this long for the next job
 *   main_spin_us=N          caller polls this long for worker completion
 *                           before falling back to short sleeps
 *   worker_cpu_mask=0xNN    optional CPU affinity for pool workers; 0 = all
 *   stats_every=N           raycast calls between stats lines
 *
 * The environment variables keep their old meaning.  They seed the initial
 * values; a key present in the config file overrides that value.  The hard
 * kill switch WC3_PARALLEL_RAYCAST=0 is still read only at pool startup.
 *
 * This file is compiled into wc3_hle_android.  It deliberately contains no
 * symbol reference to L_6F749F90: the generated call site passes the lifted
 * function pointer in, so the runtime library never has to know which
 * translation unit the lift tool put the function in.
 */
#include "parallel_raycast.h"
#include "parallel_vertex.h"
#include "parallel_triangle.h"

#include "hle_android.h"
#include "wc3_debug.h"
#include "guest_host.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define RAY_CONTAINER_COPY       0x200u
#define RAY_CONTAINER_MIN_COPY   0x150u
#define RAY_CONTAINER_DEFAULT    RAY_CONTAINER_MIN_COPY
#define RAY_SCRATCH_SIZE         0x240u
#define RAY_STACK_SIZE           0x00040000u
/* The lifted chain only needs a small uninitialised-stack window zeroed:
 * nested calls write their frames before reading them, and the old tuned
 * configuration used 0x100 bytes.  Keep 4 KiB as the diagnostic ceiling for
 * verify/prewarm, not as the cost paid on every parallel chunk. */
#define RAY_STACK_ZERO           0x00000100u
#define RAY_STACK_ZERO_MAX       0x00001000u
#define RAY_STACK_ZERO_MIN       0x00000040u
#define RAY_STACK_TOP_RESERVE    0x00000100u
#define RAY_TEB_SIZE             0x00001000u
/* Each worker pays a fixed cost before it touches a candidate: a container
 * copy, a stack clear, a CPU/register setup and a wakeup.  A caller that only
 * has a few dozen candidates is therefore much slower through the pool than
 * on the calling thread.  Require at least this many candidates per worker
 * before splitting a batch.  The environment/config override still wins.
 * A 32-candidate chunk spends more time on its private stack/container setup
 * and wake than on the candidate loop in the old shadow test.  The loaded
 * TwistedMeadows/Justice render path is different: its candidate lists are
 * 64..127 entries (98 average, 103 maximum), so a 64-candidate floor leaves
 * every batch on the caller.  Use 24 entries per chunk: a typical batch then
 * splits into four chunks, while a 64-entry batch still needs two.  The auto
 * threshold follows the same measurements. */
#define RAY_MIN_CHUNK            8u
#define RAY_AUTO_THRESHOLD_CHUNK 2u
#define RAY_MAX_THRESHOLD        1000000u
#define RAY_SENTINEL             0x6F78D9F2u
#define RAY_TUNE_PATH_MAX        1024u
#define RAY_TUNE_CHECK_MASK      0x3FFu
#define RAY_STATS_DEFAULT        8192u
#define RAY_STATS_EVERY_MAX      1000000000u

/* Linux/Android futex opcodes.  Keeping the numbers local avoids a libc
 * header dependency in this HLE translation unit. */
#define WC3_FUTEX_WAIT_PRIVATE   128
#define WC3_FUTEX_WAKE_PRIVATE   129

/* Vertex chunks reuse the same worker contexts, guest stacks and TEBs.  A
 * batch uses one chunk per pool worker at most, and the pool itself has one
 * worker per online CPU.  The default split requires enough vertices per
 * worker to amortize a wakeup and a private frame copy; tiny batches stay on
 * the caller.  WC3_VERTEX_CHUNKS and WC3_VERTEX_MIN_CHUNK can tune that for
 * diagnostics. */
#define VERTEX_GLOBALS_SIZE      0x40u
#define VERTEX_FRAME_BELOW       0x100u
#define VERTEX_FRAME_ABOVE       0x40u
#define VERTEX_FRAME_COPY        (VERTEX_FRAME_BELOW + VERTEX_FRAME_ABOVE)
#define VERTEX_DEFAULT_THRESHOLD 256u
#define VERTEX_DEFAULT_MIN_CHUNK 128u
#define VERTEX_MAX_COUNT         1000000u

/* Triangle chunks reuse the same pool.  The loop is the stride-3 fast path of
 * the triangle intersection helper, so its work unit is one triangle and each
 * worker needs its own 6-byte-strided index cursor plus a private best/index
 * pair.  A worker pays the frame copy and wakeup cost, so require at least
 * TRIANGLE_MIN_CHUNK triangles per worker; the default threshold is the
 * smallest useful batch at that split.  A 16-triangle chunk is almost all
 * frame-copy and wakeup cost, so split to four chunks at 128 triangles and
 * reach one chunk per worker only at 256. */
#define TRIANGLE_FRAME_BELOW      0x100u
#define TRIANGLE_FRAME_ABOVE      0x40u
#define TRIANGLE_FRAME_COPY       (TRIANGLE_FRAME_BELOW + TRIANGLE_FRAME_ABOVE)
#define TRIANGLE_INDEX_STRIDE     6u
#define TRIANGLE_PRIVATE_BEST_OFF 0x28u
#define TRIANGLE_PRIVATE_IND_OFF  0x2Cu
#define TRIANGLE_DEFAULT_THRESHOLD 128u
#define TRIANGLE_MIN_CHUNK        32u
#define TRIANGLE_MAX_COUNT        1000000u

/* The triangle oracle replays the whole sequential loop before every pooled
 * batch.  Keep it available in debug builds but off by default: an always-on
 * oracle turns every "parallel" measurement into parallel + sequential work.
 * A verification build can set WC3_TRIANGLE_VERIFY_DEFAULT=1 at compile time. */
#if WC3_DEBUG && WC3_PARALLEL_TRIANGLE_VALIDATE
# ifndef WC3_TRIANGLE_VERIFY_DEFAULT
#  define WC3_TRIANGLE_VERIFY_DEFAULT 0
# endif
#endif

/* The candidate loop reads at most [container+0x14C].  The copy covers the
 * count/array words at +0x110/+0x114 and every field observed in the whole
 * candidate chain with comfortable headroom. */
typedef char ray_container_copy_is_large_enough[
    (RAY_CONTAINER_COPY >= 0x200u) ? 1 : -1];

typedef struct RayTuning {
    _Atomic int enabled;
    _Atomic unsigned threshold;
    _Atomic unsigned chunk_limit;   /* 0 = all workers */
    _Atomic unsigned min_chunk;
    _Atomic unsigned stack_zero;
    _Atomic unsigned container_copy;
    _Atomic int verify;
    _Atomic int stats;
    _Atomic int timing;
    _Atomic unsigned spin_us;
    _Atomic unsigned main_spin_us;
    _Atomic unsigned worker_cpu_mask;
    _Atomic unsigned stats_every;
} RayTuning;

typedef enum RayJobKind {
    RAY_JOB_RAYCAST = 0,
    RAY_JOB_VERTEX = 1,
    RAY_JOB_TRIANGLE = 2
} RayJobKind;

/* Each dispatch initializes only the common fields and the fields for its
 * kind.  Workers branch on kind before reading a payload, so the other fields
 * and any padding never need a full-structure clear. */
typedef struct RayJob {
    wc3_lifted_fn fn;
    const CPU *cpu;
    uint32_t container;
    uint32_t array;
    uint32_t in1;
    uint32_t in2;
    uint32_t user_ctx;
    /* Split total into contiguous chunks without a division in each worker.
     * Chunk i gets chunk_base + (i < chunk_rem) items and starts at
     * i * chunk_base + min(i, chunk_rem). */
    uint32_t chunk_base;
    uint32_t chunk_rem;
    unsigned stack_zero;
    unsigned container_copy;

    /* RAY_JOB_VERTEX uses fn, cpu, the chunk split and these fields instead
     * of the raycast arguments above.  vertex_globals is the snapshot of the
     * real 13-dword block taken before any worker is woken. */
    unsigned kind;
    unsigned vertex_kind;
    uint32_t vertex_globals[WC3_VERTEX_GLOBAL_WORDS];

    /* RAY_JOB_TRIANGLE uses fn, cpu, the chunk split and these fields.  The
     * triangle loop is entered in L_6F125810's frame; each worker copies that
     * frame and gets a private best/index pair. */
    uint32_t triangle_indices;
    uint32_t triangle_best_ptr;
    uint32_t triangle_index_ptr;
    uint32_t triangle_orig_best;
} RayJob;

/* One division per job instead of two per worker.  The split is contiguous and
 * balanced: the first chunk_rem chunks carry one extra item. */
static inline void job_set_chunk_split(RayJob *job, uint32_t total,
                                       unsigned chunks)
{
    job->chunk_base = total / chunks;
    job->chunk_rem = total % chunks;
}

typedef struct RayCtx {
    CPU cpu;

    uint32_t stack_base;
    uint32_t stack_top;
    uint32_t teb;
    uint32_t scratch;
    uint32_t container;
    uint32_t out_pos;
    uint32_t out_dist;

    uint32_t start;
    uint32_t count;
    uint32_t hit;
    uint32_t dist_bits;
    uint32_t pos[3];

    /* Vertex jobs use a private guest block for the rewritten globals and
     * keep the last chunk's observable XMM state for the caller merge. */
    uint32_t vertex_globals;
    XMM vertex_xmm0;
    XMM vertex_xmm1;
    int vertex_failed;

    /* Triangle jobs keep the worker's private best bits and local triangle
     * index.  The private words live in the copied frame at the offsets above
     * so the guest loop can write them through ordinary pointers. */
    uint32_t triangle_best;
    uint32_t triangle_index;
    uint32_t triangle_best_addr;
    uint32_t triangle_index_addr;
    int triangle_failed;

    int index;
    /* Set after this worker runs an active chunk.  The next wait then uses
     * the shared futex; workers outside the active set stay on their own
     * semaphore.  This is per-worker state so a caller publishing the next
     * active count cannot race the queue choice. */
    int wait_shared;
    int thread_started;
    unsigned applied_cpu_mask;
    uint64_t seen_seq;
    /* Published by the worker after it finishes the job sequence.  The caller
     * scans only the active workers; no shared locked counter is needed. */
    _Atomic uint64_t done_seq;
    pthread_t thread;
    sem_t wake;
    int wake_init;
} RayCtx;

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
/* Fast-path copy of the one-time init result.  The wrappers are hot; after the
 * first successful init they must not call pthread_once for every ray/vertex. */
static _Atomic int g_init_ok;
static int g_parallel_ok;
/* Set while init_once prewarms guest functions on the caller thread.  A vertex
 * loop reached from that prewarm must run sequentially instead of recursively
 * entering pthread_once on the same thread. */
static int g_pool_init_active;
static int g_raycast_enabled = 1;
static int g_vertex_enabled = 1;
static int g_triangle_enabled = 1;
#if WC3_DEBUG && WC3_PARALLEL_VERTEX_VALIDATE
/* Run the original full vertex loop after a pooled batch and compare its
 * observable bytes/CPU state with the parallel result.  Debug-only and
 * opt-in through WC3_VERTEX_VERIFY=1. */
static int g_vertex_verify;
#endif
#if WC3_DEBUG && WC3_PARALLEL_TRIANGLE_VALIDATE
/* Debug-only opt-in for the triangle sequential oracle.  Range checks stay
 * enabled; only the expensive whole-batch replay is gated. */
static int g_triangle_verify;
#endif
static unsigned g_vertex_threshold = VERTEX_DEFAULT_THRESHOLD;
static unsigned g_vertex_chunks;      /* 0 = one chunk per worker */
static unsigned g_vertex_min_chunk = VERTEX_DEFAULT_MIN_CHUNK;
static unsigned g_triangle_threshold = TRIANGLE_DEFAULT_THRESHOLD;
static unsigned g_triangle_chunks;    /* 0 = one chunk per worker */
static unsigned g_triangle_min_chunk = TRIANGLE_MIN_CHUNK;
static unsigned g_workers;
static RayCtx *g_worker;
#if WC3_DEBUG && (WC3_DEBUG_HLE || WC3_PARALLEL_VERTEX_VALIDATE || \
                  WC3_PARALLEL_TRIANGLE_VALIDATE)
static RayCtx g_verify_ctx;
#endif

static RayTuning g_tune;
#if WC3_DEBUG && WC3_DEBUG_HLE
static int g_threshold_auto;
static int g_tune_file_enabled;
static char g_tune_path[RAY_TUNE_PATH_MAX];
static int g_tune_path_ready;
static uint32_t g_tune_hash;
static int g_tune_missing_logged;
#endif

/* One job at a time.  run_batch fills a stack RayJob, publishes its address,
 * then publishes the active set tagged with the job sequence, and finally
 * publishes the sequence itself.  Workers acquire the sequence first and then
 * the tagged active set, so a worker that sees sequence N also sees the active
 * set and job address that belong to N.  The RayJob itself lives on
 * run_batch's stack until every active worker reports completion.  Inactive
 * workers are neither woken nor allowed to read the stack job, so the caller
 * only waits for the chunks that actually run. */
static _Atomic(uintptr_t) g_job_addr;
/* First published value is 1: done_seq starts at zero, so a bare increment
 * result would look like an already-finished first job.  Only the submit-lock
 * holder writes this counter. */
static _Atomic uint64_t g_job_seq;
/* High 32 bits: sequence tag; low 32 bits: number of active chunks.  Tagging
 * the active set prevents a delayed worker from combining a new active count
 * with an old stack job. */
static _Atomic uint64_t g_active_seq;
static _Atomic int g_shutdown;
/* Workers that were active in the previous job wait on one shared futex.  A
 * same-size or shrinking batch then costs one futex wake instead of one
 * sem_post per worker.  Workers outside the previous active set stay on their
 * per-worker semaphore; a growing batch posts only those newly active
 * semaphores.  If futex is unavailable, every active worker uses its
 * semaphore, exactly like the previous implementation. */
static _Atomic unsigned g_wake_gen;
static unsigned g_prev_active;       /* caller-only, protected by g_submit_lock */
static int g_shared_wake;
/* Completion is polled by the caller.  A semaphore wake for every ray was a
 * large fixed cost: the worker finished but the main thread still had to be
 * rescheduled before it could merge.  Each active worker publishes its own
 * done sequence and the caller scans only those workers, avoiding a locked
 * counter on one cache line. */

/* Non-blocking submit lock: a nested or concurrent raycast falls back to the
 * sequential function instead of waiting for the pool.  This is what makes
 * the wrapper safe if a worker's candidate chain ever re-enters it. */
static pthread_mutex_t g_submit_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int t_ray_active;

#if WC3_DEBUG && (WC3_DEBUG_HLE || WC3_DEBUG_PERF)
static unsigned long long g_calls;
#endif
#if WC3_DEBUG && WC3_DEBUG_PERF
static unsigned long long g_parallel_calls;
static unsigned long long g_fallback_calls;
static unsigned long long g_parallel_ns;
static unsigned long long g_fallback_ns;
static unsigned long long g_stats_last_calls;
static unsigned long long g_stats_last_parallel;
static unsigned long long g_stats_last_fallback;
static unsigned long long g_stats_last_parallel_ns;
static unsigned long long g_stats_last_fallback_ns;
static unsigned g_last_chunks;
static unsigned g_last_total;

/* Candidate-count distribution.  Raycast batches on a loaded map can be far
 * smaller than the vertex batches, so a single auto threshold hides whether
 * the pool is missing work or the candidate lists are genuinely tiny.  Sample
 * the wrapper to keep the hot fallback path cheap even in a PERF build. */
enum { RAY_TOTAL_HIST_BUCKETS = 12 };
static unsigned long long g_ray_total_samples;
static unsigned long long g_ray_total_sum;
static unsigned g_ray_total_max;
static unsigned g_ray_total_hist[RAY_TOTAL_HIST_BUCKETS];

/* Vertex and triangle have their own counters so a perf run can separate
 * batch-shape effects from raycast tuning.  All writes happen on the caller
 * thread; workers never touch these values. */
enum {
    KIND_PERF_VERTEX = 0,
    KIND_PERF_TRIANGLE = 1,
    KIND_PERF_COUNT = 2
};
typedef struct KindPerf {
    unsigned long long calls;
    unsigned long long parallel;
    unsigned long long fallback;
    unsigned long long parallel_ns;
    unsigned long long total_sum;
    unsigned long long chunks_sum;
    unsigned hist_batch[6];
    unsigned hist_chunks[9];
    unsigned long long last_calls;
    unsigned long long last_parallel;
    unsigned long long last_fallback;
    unsigned long long last_parallel_ns;
} KindPerf;
static KindPerf g_kind_perf[KIND_PERF_COUNT];
#endif
#if WC3_DEBUG && WC3_DEBUG_HLE
static unsigned long long g_verify_calls;
static unsigned long long g_verify_failures;
#endif

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void cpu_relax(void)
{
#if defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    atomic_signal_fence(memory_order_seq_cst);
#endif
}

#if defined(__NR_futex)
static int futex_probe(void)
{
    unsigned word = 0;
    errno = 0;
    long rc = syscall(__NR_futex, (int *)(uintptr_t)&word,
                      WC3_FUTEX_WAIT_PRIVATE, 1, NULL, NULL, 0);
    return rc == -1 && errno == EAGAIN;
}

static void wake_shared_workers(void)
{
    atomic_fetch_add_explicit(&g_wake_gen, 1u, memory_order_release);
    (void)syscall(__NR_futex, (int *)(uintptr_t)&g_wake_gen,
                  WC3_FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
}
#else
static int futex_probe(void)
{
    return 0;
}

static void wake_shared_workers(void)
{
}
#endif

static int tune_int(const _Atomic int *p)
{
    return atomic_load_explicit(p, memory_order_relaxed);
}

static unsigned tune_uint(const _Atomic unsigned *p)
{
    return atomic_load_explicit(p, memory_order_relaxed);
}

static void tune_set_int(_Atomic int *p, int v)
{
    atomic_store_explicit(p, v, memory_order_relaxed);
}

static void tune_set_uint(_Atomic unsigned *p, unsigned v)
{
    atomic_store_explicit(p, v, memory_order_relaxed);
}

static unsigned env_u(const char *name, unsigned fallback, unsigned maximum)
{
    const char *s = getenv(name);
    if (!s || !*s) return fallback;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || v > maximum) return fallback;
    return (unsigned)v;
}

static int env_off(const char *name)
{
    const char *s = getenv(name);
    if (!s || !*s) return 0;
    if (!strcmp(s, "0") || !strcmp(s, "off") || !strcmp(s, "false") ||
        !strcmp(s, "OFF") || !strcmp(s, "FALSE"))
        return 1;
    return 0;
}

#if WC3_DEBUG && (WC3_DEBUG_HLE || WC3_DEBUG_PERF)
static int env_on(const char *name)
{
    const char *s = getenv(name);
    if (!s || !*s) return 0;
    return !env_off(name);
}
#endif

/* Read the CPUs this process may actually run on.  _SC_NPROCESSORS_ONLN
 * counts online CPUs for the whole device, not the cpuset attached to the app;
 * using it can create more workers than the scheduler will ever run. */
static int read_allowed_cpu_mask(unsigned long long *out)
{
#if defined(__NR_sched_getaffinity)
    unsigned char mask[128];
    memset(mask, 0, sizeof mask);
    if (syscall(__NR_sched_getaffinity, 0, sizeof mask, mask) == 0) {
        unsigned long long set = 0;
        for (unsigned i = 0; i < 64u; i++) {
            if (mask[i >> 3] & (1u << (i & 7u)))
                set |= 1ull << i;
        }
        if (set) {
            *out = set;
            return 1;
        }
    }
#else
    (void)out;
#endif
    return 0;
}

static unsigned allowed_cpu_count(void)
{
    unsigned long long set = 0;
    if (read_allowed_cpu_mask(&set)) {
        unsigned n = 0;
        for (unsigned i = 0; i < 64u; i++)
            n += (unsigned)((set >> i) & 1u);
        if (n) return n;
    }

    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) return 1;
    if ((unsigned long)n > UINT_MAX) return UINT_MAX;
    return (unsigned)n;
}

static unsigned auto_worker_count(void)
{
    /* One worker per CPU the process is allowed to use.  The caller blocks
     * inside run_batch while the batch runs, so the workers are the only
     * raycast consumers during it. */
    return allowed_cpu_count();
}

#if WC3_DEBUG && WC3_DEBUG_PERF
static unsigned perf_batch_bucket(uint32_t total)
{
    if (total < 64u) return 0;
    if (total < 128u) return 1;
    if (total < 256u) return 2;
    if (total < 512u) return 3;
    if (total < 1024u) return 4;
    return 5;
}

static unsigned ray_total_bucket(uint32_t total)
{
    if (total == 0u) return 0;
    if (total == 1u) return 1;
    if (total < 4u) return 2;
    if (total < 8u) return 3;
    if (total < 16u) return 4;
    if (total < 32u) return 5;
    if (total < 64u) return 6;
    if (total < 128u) return 7;
    if (total < 256u) return 8;
    if (total < 512u) return 9;
    if (total < 1024u) return 10;
    return 11;
}

static void ray_note_total(uint32_t total)
{
    /* One in four calls is enough for a stable shape and keeps the sampled
     * overhead below the normal stats cadence. */
    if ((g_calls & 3u) != 0u) return;
    g_ray_total_samples++;
    g_ray_total_sum += total;
    if (total > g_ray_total_max) g_ray_total_max = total;
    g_ray_total_hist[ray_total_bucket(total)]++;
}

static unsigned perf_chunk_bucket(unsigned chunks)
{
    return chunks < 9u ? chunks : 8u;
}

static void maybe_print_kind_stats(unsigned kind)
{
    KindPerf *p = &g_kind_perf[kind];
    if (!tune_int(&g_tune.stats)) return;

    unsigned every = tune_uint(&g_tune.stats_every);
    if (!every) every = RAY_STATS_DEFAULT;
    if (p->calls - p->last_calls < every) return;

    unsigned long long calls = p->calls - p->last_calls;
    unsigned long long parallel = p->parallel - p->last_parallel;
    unsigned long long fallback = p->fallback - p->last_fallback;
    unsigned long long parallel_ns = p->parallel_ns - p->last_parallel_ns;
    unsigned long long avg_us =
        parallel ? parallel_ns / parallel / 1000ull : 0;
    unsigned long long avg_total = calls ? p->total_sum / p->calls : 0;
    unsigned long long avg_chunks = calls ? p->chunks_sum / p->calls : 0;
    const char *name = kind == KIND_PERF_VERTEX ? "vertex" : "triangle";

    fprintf(stderr,
            "[perf] %s +%llu calls: parallel=%llu fallback=%llu "
            "avg=%lluus avg_total=%llu avg_chunks=%llu "
            "batch=[%u,%u,%u,%u,%u,%u] chunks=[%u,%u,%u,%u,%u,%u,%u,%u,%u]\n",
            name, calls, parallel, fallback, avg_us, avg_total, avg_chunks,
            p->hist_batch[0], p->hist_batch[1], p->hist_batch[2],
            p->hist_batch[3], p->hist_batch[4], p->hist_batch[5],
            p->hist_chunks[0], p->hist_chunks[1], p->hist_chunks[2],
            p->hist_chunks[3], p->hist_chunks[4], p->hist_chunks[5],
            p->hist_chunks[6], p->hist_chunks[7], p->hist_chunks[8]);

    p->last_calls = p->calls;
    p->last_parallel = p->parallel;
    p->last_fallback = p->fallback;
    p->last_parallel_ns = p->parallel_ns;
}

static void perf_note_kind(unsigned kind, uint32_t total, unsigned chunks,
                           int pooled, uint64_t elapsed_ns)
{
    KindPerf *p = &g_kind_perf[kind];
    p->calls++;
    if (pooled) {
        p->parallel++;
        p->parallel_ns += elapsed_ns;
    } else {
        p->fallback++;
    }
    p->total_sum += total;
    p->chunks_sum += chunks;
    p->hist_batch[perf_batch_bucket(total)]++;
    p->hist_chunks[perf_chunk_bucket(chunks)]++;
    maybe_print_kind_stats(kind);
}
#endif

#if WC3_DEBUG && WC3_DEBUG_HLE
static int parse_bool(const char *s, int *out)
{
    if (!s) return 0;
    if (!strcmp(s, "1") || !strcmp(s, "on") || !strcmp(s, "true") ||
        !strcmp(s, "ON") || !strcmp(s, "TRUE")) {
        *out = 1;
        return 1;
    }
    if (!strcmp(s, "0") || !strcmp(s, "off") || !strcmp(s, "false") ||
        !strcmp(s, "OFF") || !strcmp(s, "FALSE")) {
        *out = 0;
        return 1;
    }
    return 0;
}
#endif

static int parse_uint(const char *s, unsigned *out)
{
    if (!s || !*s) return 0;
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || *end != '\0' || errno == ERANGE || v > UINT_MAX)
        return 0;
    *out = (unsigned)v;
    return 1;
}

#if WC3_DEBUG && WC3_DEBUG_HLE
static unsigned clamp_uint(unsigned v, unsigned lo, unsigned hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void recompute_auto_threshold(void)
{
    if (!g_threshold_auto) return;
    unsigned chunks = tune_uint(&g_tune.chunk_limit);
    if (chunks == 0) chunks = g_workers ? g_workers : auto_worker_count();
    uint64_t threshold = (uint64_t)chunks * RAY_AUTO_THRESHOLD_CHUNK;
    uint64_t floor = (uint64_t)tune_uint(&g_tune.min_chunk) * 2u;
    if (threshold < floor) threshold = floor;
    if (threshold > RAY_MAX_THRESHOLD) threshold = RAY_MAX_THRESHOLD;
    tune_set_uint(&g_tune.threshold, (unsigned)threshold);
}

static void init_tune_path(void)
{
    const char *dir = getenv("WC3_DATA_DIR");
    if (dir && *dir)
        snprintf(g_tune_path, sizeof g_tune_path, "%s/raycast.cfg", dir);
    else
        snprintf(g_tune_path, sizeof g_tune_path, "raycast.cfg");
    g_tune_path_ready = 1;
}

static void log_tuning(const char *why)
{
    unsigned chunks = tune_uint(&g_tune.chunk_limit);
    fprintf(stderr,
            "[raycast] tune %s: enabled=%d threshold=%u%s chunks=%u%s "
            "min_chunk=%u stack_zero=%u copy=%u verify=%d stats=%d "
            "timing=%d spin_us=%u main_spin_us=%u cpu_mask=0x%X "
            "stats_every=%u workers=%u\n",
            why,
            tune_int(&g_tune.enabled),
            tune_uint(&g_tune.threshold),
            g_threshold_auto ? "(auto)" : "",
            chunks,
            chunks ? "" : "(auto)",
            tune_uint(&g_tune.min_chunk),
            tune_uint(&g_tune.stack_zero),
            tune_uint(&g_tune.container_copy),
            tune_int(&g_tune.verify),
            tune_int(&g_tune.stats),
            tune_int(&g_tune.timing),
            tune_uint(&g_tune.spin_us),
            tune_uint(&g_tune.main_spin_us),
            tune_uint(&g_tune.worker_cpu_mask),
            tune_uint(&g_tune.stats_every),
            g_workers);
}

static void apply_tuning_key(const char *key, const char *value)
{
    unsigned u;
    int b;

    if (!strcmp(key, "enabled")) {
        if (parse_bool(value, &b)) tune_set_int(&g_tune.enabled, b);
        return;
    }
    if (!strcmp(key, "threshold")) {
        if (!strcmp(value, "auto")) {
            g_threshold_auto = 1;
        } else if (parse_uint(value, &u) && u <= RAY_MAX_THRESHOLD) {
            g_threshold_auto = 0;
            tune_set_uint(&g_tune.threshold, u);
        }
        return;
    }
    if (!strcmp(key, "chunks")) {
        if (!strcmp(value, "auto")) {
            tune_set_uint(&g_tune.chunk_limit, 0);
        } else if (parse_uint(value, &u)) {
            if (u == 0) {
                tune_set_uint(&g_tune.chunk_limit, 0);
            } else {
                if (g_workers && u > g_workers) u = g_workers;
                tune_set_uint(&g_tune.chunk_limit, u);
            }
        }
        return;
    }
    if (!strcmp(key, "min_chunk")) {
        if (parse_uint(value, &u))
            tune_set_uint(&g_tune.min_chunk, clamp_uint(u, 1u, 4096u));
        return;
    }
    if (!strcmp(key, "stack_zero")) {
        if (parse_uint(value, &u))
            tune_set_uint(&g_tune.stack_zero,
                          clamp_uint(u, RAY_STACK_ZERO_MIN,
                                     RAY_STACK_ZERO_MAX));
        return;
    }
    if (!strcmp(key, "container_copy")) {
        if (parse_uint(value, &u))
            tune_set_uint(&g_tune.container_copy,
                          clamp_uint(u, RAY_CONTAINER_MIN_COPY,
                                     RAY_CONTAINER_COPY));
        return;
    }
    if (!strcmp(key, "verify")) {
        if (parse_bool(value, &b)) tune_set_int(&g_tune.verify, b);
        return;
    }
    if (!strcmp(key, "stats")) {
        if (parse_bool(value, &b)) tune_set_int(&g_tune.stats, b);
        return;
    }
    if (!strcmp(key, "timing")) {
        if (parse_bool(value, &b)) tune_set_int(&g_tune.timing, b);
        return;
    }
    if (!strcmp(key, "spin_us")) {
        if (parse_uint(value, &u))
            tune_set_uint(&g_tune.spin_us, clamp_uint(u, 0u, 10000u));
        return;
    }
    if (!strcmp(key, "main_spin_us")) {
        if (parse_uint(value, &u))
            tune_set_uint(&g_tune.main_spin_us,
                          clamp_uint(u, 0u, 1000000u));
        return;
    }
    if (!strcmp(key, "worker_cpu_mask")) {
        if (parse_uint(value, &u))
            tune_set_uint(&g_tune.worker_cpu_mask, u);
        return;
    }
    if (!strcmp(key, "stats_every")) {
        if (parse_uint(value, &u))
            tune_set_uint(&g_tune.stats_every,
                          clamp_uint(u, 1u, RAY_STATS_EVERY_MAX));
        return;
    }
    fprintf(stderr, "[raycast] tune: unknown key '%s'\n", key);
}

static void parse_tuning_text(const char *text)
{
    const char *p = text;
    while (p && *p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        if (*p == '#' || *p == ';') {
            while (*p && *p != '\n') p++;
            continue;
        }

        char key[64];
        size_t kn = 0;
        while (*p && *p != '=' && *p != ' ' && *p != '\t' &&
               *p != '\r' && *p != '\n') {
            if (kn + 1u < sizeof key) key[kn++] = *p;
            p++;
        }
        key[kn] = '\0';
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') {
            while (*p && *p != '\n') p++;
            continue;
        }
        p++;
        while (*p == ' ' || *p == '\t') p++;

        char value[64];
        size_t vn = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            if (vn + 1u < sizeof value) value[vn++] = *p;
            p++;
        }
        value[vn] = '\0';
        if (kn)
            apply_tuning_key(key, value);
    }
    recompute_auto_threshold();
}

static uint32_t hash_tuning_text(const char *text)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        h ^= (uint32_t)*p;
        h *= 16777619u;
    }
    return h;
}

static int load_tuning_file(int log)
{
    if (!g_tune_file_enabled) return 0;
    if (!g_tune_path_ready) init_tune_path();

    FILE *f = fopen(g_tune_path, "r");
    if (!f) {
        if (log && !g_tune_missing_logged) {
            fprintf(stderr, "[raycast] tune: no %s (create it to tune live)\n",
                    g_tune_path);
            g_tune_missing_logged = 1;
        }
        return 0;
    }

    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1u, f);
    fclose(f);
    buf[n] = '\0';

    uint32_t hash = hash_tuning_text(buf);
    if (hash == g_tune_hash) return 0;
    g_tune_hash = hash;
    g_tune_missing_logged = 0;
    parse_tuning_text(buf);
    if (log) log_tuning("reload");
    return 1;
}

static void maybe_reload_tuning(void)
{
    load_tuning_file(1);
}
#endif

static void set_tuning_defaults(void)
{
    unsigned workers = g_workers ? g_workers : auto_worker_count();
    uint64_t auto_threshold = (uint64_t)workers * RAY_AUTO_THRESHOLD_CHUNK;
    uint64_t auto_floor = (uint64_t)RAY_MIN_CHUNK * 2u;
    if (auto_threshold < auto_floor) auto_threshold = auto_floor;

    /* The live tune file is a diagnostic tool, not a product input.  Require
     * an explicit opt-in so a stale file cannot pin worker count or affinity. */
#if WC3_DEBUG && WC3_DEBUG_HLE
    g_tune_file_enabled = env_on("WC3_RAY_TUNE");
#endif
    if (auto_threshold > RAY_MAX_THRESHOLD) auto_threshold = RAY_MAX_THRESHOLD;

    unsigned threshold = (unsigned)auto_threshold;
#if WC3_DEBUG && WC3_DEBUG_HLE
    int threshold_auto = 1;
#endif
    const char *s = getenv("WC3_PARALLEL_RAYCAST_THRESHOLD");
    if (s && *s) {
        unsigned u;
        if (parse_uint(s, &u) && u <= RAY_MAX_THRESHOLD) {
            threshold = u;
#if WC3_DEBUG && WC3_DEBUG_HLE
            threshold_auto = 0;
#endif
        }
    }

    tune_set_int(&g_tune.enabled, 1);
    tune_set_uint(&g_tune.threshold, threshold);
    tune_set_uint(&g_tune.chunk_limit, 0);
    tune_set_uint(&g_tune.min_chunk, RAY_MIN_CHUNK);
    tune_set_uint(&g_tune.stack_zero, RAY_STACK_ZERO);
    tune_set_uint(&g_tune.container_copy, RAY_CONTAINER_DEFAULT);
#if WC3_DEBUG && WC3_DEBUG_HLE
    tune_set_int(&g_tune.verify, env_on("WC3_RAY_VERIFY"));
#endif
#if WC3_DEBUG && WC3_DEBUG_PERF
    {
        const char *stats = getenv("WC3_RAY_STATS");
        /* A PERF build is already a diagnostic build.  Default the counters
         * on; an explicit WC3_RAY_STATS=0 still turns them off. */
        tune_set_int(&g_tune.stats,
                     stats && *stats ? env_on("WC3_RAY_STATS") : 1);
    }
    /* A PERF build needs the fallback/parallel split in its periodic line;
     * release has no counters at all.  The two clock reads per call are noise
     * next to the guest candidate loop on the loaded map. */
    tune_set_int(&g_tune.timing, 1);
    tune_set_uint(&g_tune.stats_every, RAY_STATS_DEFAULT);
#endif
    tune_set_uint(&g_tune.spin_us,
                  env_u("WC3_RAY_SPIN_US", 0u, 10000u));
    tune_set_uint(&g_tune.main_spin_us,
                  env_u("WC3_RAY_MAIN_SPIN_US", 1000u, 1000000u));
    tune_set_uint(&g_tune.worker_cpu_mask,
                  env_u("WC3_RAY_WORKER_CPU_MASK", 0u, UINT_MAX));
#if WC3_DEBUG && WC3_DEBUG_PERF
    tune_set_uint(&g_tune.stats_every, RAY_STATS_DEFAULT);
#endif
#if WC3_DEBUG && WC3_DEBUG_HLE
    g_threshold_auto = threshold_auto;
#endif
}

static void teb_init_local(uint32_t teb, uint32_t stack_base,
                           uint32_t stack_size, uint32_t tid)
{
    memset(guest_host_ptr(teb), 0, RAY_TEB_SIZE);
    wr32(teb + 0x00u, 0xFFFFFFFFu);          /* empty SEH chain */
    wr32(teb + 0x04u, stack_base + stack_size); /* StackBase */
    wr32(teb + 0x08u, stack_base);           /* StackLimit */
    wr32(teb + 0x18u, teb);                  /* Self */
    wr32(teb + 0x20u, 0x00010000u);          /* process id, synthetic */
    wr32(teb + 0x24u, tid);                  /* thread id, synthetic */
}

static int alloc_ctx(RayCtx *ctx, unsigned index)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->index = (int)index;

    if (sem_init(&ctx->wake, 0, 0) != 0) return 0;
    ctx->wake_init = 1;

    ctx->stack_base = hle_guest_alloc(RAY_STACK_SIZE);
    ctx->teb = hle_guest_alloc(RAY_TEB_SIZE);
    ctx->scratch = hle_guest_alloc(RAY_SCRATCH_SIZE);
    ctx->vertex_globals = hle_guest_alloc(VERTEX_GLOBALS_SIZE);
    if (!ctx->stack_base || !ctx->teb || !ctx->scratch ||
        !ctx->vertex_globals)
        goto fail;
    if (!hle_android_guest_ok(ctx->stack_base, RAY_STACK_SIZE) ||
        !hle_android_guest_ok(ctx->teb, RAY_TEB_SIZE) ||
        !hle_android_guest_ok(ctx->scratch, RAY_SCRATCH_SIZE) ||
        !hle_android_guest_ok(ctx->vertex_globals, VERTEX_GLOBALS_SIZE))
        goto fail;

    memset(guest_host_ptr(ctx->stack_base), 0, RAY_STACK_SIZE);
    memset(guest_host_ptr(ctx->scratch), 0, RAY_SCRATCH_SIZE);
    memset(guest_host_ptr(ctx->vertex_globals), 0, VERTEX_GLOBALS_SIZE);
    teb_init_local(ctx->teb, ctx->stack_base, RAY_STACK_SIZE,
                   0x100u + (uint32_t)index);

    ctx->container = ctx->scratch;
    ctx->out_pos = ctx->scratch + 0x200u;
    ctx->out_dist = ctx->scratch + 0x210u;
    ctx->stack_top = (ctx->stack_base + RAY_STACK_SIZE -
                      RAY_STACK_TOP_RESERVE) & ~15u;
    if (ctx->stack_top <= ctx->stack_base + RAY_STACK_ZERO_MAX) goto fail;
    return 1;

fail:
    sem_destroy(&ctx->wake);
    ctx->wake_init = 0;
    return 0;
}

/* Copy the live caller CPU, then replace only the things that belong to the
 * worker: stack, TEB, dispatch state and the register arguments.  XMM/x87,
 * TLS slots, timestamps and flags are intentionally inherited. */
static void prepare_base(RayCtx *ctx, const CPU *src, int copy_src)
{
    CPU *w = &ctx->cpu;
    if (copy_src) {
        *w = *src;
    } else {
        memset(w, 0, sizeof *w);
        cpu_x87_init_thread(w);
    }

    w->eax = 0;
    w->ebx = 0;
    w->ecx = 0;
    w->edx = 0;
    w->esi = 0;
    w->edi = 0;
    w->ebp = 0;
    w->eip = 0;
    w->esp = ctx->stack_top;
    w->disp_depth = 0;
    w->disp_pending_valid = 0;
    w->disp_pending_kind = WC3_DISP_PENDING_VA;
    w->disp_pending_target = 0;
    w->disp_draining = 0;
    w->fs_base = ctx->teb;
    w->fs = 0x53u;
    w->es = 0x2Bu;
    w->ds = 0x2Bu;
    w->ss = 0x2Bu;
    w->cs = 0x23u;
    w->gs = 0;
    /* The candidate chain does not use the SEH chain, but a nested HLE call
     * may.  Start it empty on every entry; the TEB itself is persistent. */
    wr32(ctx->teb + 0x00u, 0xFFFFFFFFu);
}

static void push_arg(CPU *w, uint32_t v)
{
    push32(w, v);
}

/* Run one contiguous chunk.  The worker never touches the caller's stack or
 * the caller's output words. */
static void run_chunk(RayCtx *ctx, const RayJob *job)
{
    CPU *w = &ctx->cpu;
    uint32_t count = ctx->count;

    ctx->hit = 0;
    ctx->dist_bits = 0;
    ctx->pos[0] = ctx->pos[1] = ctx->pos[2] = 0;
    if (count == 0) return;

    /* A private copy is required: the hit path reads container+0x14c and the
     * count/array words are overridden per chunk.  Copy before the call and
     * never write through the original.  container_copy is a tunable, but
     * the parser never lets it drop below +0x14c. */
    memcpy(guest_host_ptr(ctx->container),
           guest_host_ptr(job->container), job->container_copy);
    wr32(ctx->container + 0x110u, count);
    wr32(ctx->container + 0x114u, job->array + ctx->start * 4u);

    memset(guest_host_ptr(ctx->out_pos), 0, 12);
    memset(guest_host_ptr(ctx->out_dist), 0, 4);

    /* A zeroed window makes a worker's stack independent of the previous job
     * as well as of the main thread.  The lifted chain only touches a small
     * part of this window, but the reserve keeps nested calls deterministic.
     * stack_zero is tunable for experiments; verify=1 is the safety net. */
    if (job->stack_zero)
        memset(guest_host_ptr(ctx->stack_top - job->stack_zero), 0,
               job->stack_zero);

    prepare_base(ctx, job->cpu, 1);
    w->ecx = ctx->container;
    push_arg(w, job->user_ctx);
    push_arg(w, ctx->out_dist);
    push_arg(w, ctx->out_pos);
    push_arg(w, job->in2);
    push_arg(w, job->in1);
    push_arg(w, RAY_SENTINEL);

    WC3_DIRECT_CALL_CLEAN(job->fn, w);

    ctx->hit = (w->eax != 0) ? 1u : 0u;
    if (ctx->hit) {
        ctx->dist_bits = rd32(ctx->out_dist);
        ctx->pos[0] = rd32(ctx->out_pos + 0u);
        ctx->pos[1] = rd32(ctx->out_pos + 4u);
        ctx->pos[2] = rd32(ctx->out_pos + 8u);
    }
}

/* ---- vertex chunks -------------------------------------------------------
 *
 * The two lifted loops are called as tail jumps from their setup blocks, so
 * their frame is the caller's frame: [ebp-0x30..ebp+0x10] holds locals, the
 * saved edi/esi/ebx and the destination delta.  A worker copies that window
 * verbatim to its private stack, runs the original loop with a short count and
 * a private 13-dword global block, then lets the original epilogue pop the
 * saved registers.  Main merges the private global block and repeats the
 * epilogue once.
 */

#if WC3_DEBUG && WC3_PARALLEL_VERTEX_VALIDATE
static int vertex_span_ok(uint32_t ptr, uint32_t stride, uint32_t count,
                          uint32_t element_size)
{
    if (!ptr || !count) return 0;
    uint64_t last = (uint64_t)ptr + (uint64_t)(count - 1u) * stride;
    if (last + element_size > 0x100000000ull) return 0;
    if (!hle_android_guest_ok(ptr, element_size)) return 0;
    return hle_android_guest_ok((uint32_t)last, element_size);
}

static int vertex_frame_ok(const CPU *c)
{
    if (c->ebp < VERTEX_FRAME_BELOW) return 0;
    if (c->ebp > 0xFFFFFFFFu - VERTEX_FRAME_ABOVE) return 0;
    uint32_t lo = c->ebp - VERTEX_FRAME_BELOW;
    if (!hle_android_guest_ok(lo, VERTEX_FRAME_COPY)) return 0;
    if (c->esp < lo || c->esp > c->ebp + VERTEX_FRAME_ABOVE) return 0;
    return 1;
}
#endif

static uint32_t vertex_relocate_frame_ptr(uint32_t ptr, uint32_t lo,
                                          uint32_t dst_lo, uint32_t hi)
{
    if (ptr >= lo && ptr < hi)
        return dst_lo + (ptr - lo);
    return ptr;
}

static int prepare_vertex_ctx(RayCtx *ctx, const RayJob *job)
{
    const CPU *src = job->cpu;
    CPU *w = &ctx->cpu;
    uint32_t lo = src->ebp - VERTEX_FRAME_BELOW;
    uint32_t dst_lo = ctx->stack_top - VERTEX_FRAME_COPY;
    uint32_t hi = lo + VERTEX_FRAME_COPY;

    if (dst_lo < ctx->stack_base) return 0;
#if WC3_DEBUG && WC3_PARALLEL_VERTEX_VALIDATE
    if (!hle_android_guest_ok(dst_lo, VERTEX_FRAME_COPY)) return 0;
#endif
    memcpy(guest_host_ptr(dst_lo), guest_host_ptr(lo),
           VERTEX_FRAME_COPY);

    *w = *src;
    w->ebp = dst_lo + VERTEX_FRAME_BELOW;
    w->esp = dst_lo + (src->esp - lo);
    /* Zero-stride optional outputs point at an in-frame dummy word.  The
     * copied pointer still names the caller's frame; retarget every in-frame
     * output pointer at the worker's private copy so workers never write the
     * shared dummy (one worker would otherwise corrupt the next worker's
     * EDX pointer before it enters the loop). */
    w->ebx = vertex_relocate_frame_ptr(w->ebx, lo, dst_lo, hi);
    w->esi = vertex_relocate_frame_ptr(w->esi, lo, dst_lo, hi);
    w->edi = vertex_relocate_frame_ptr(w->edi, lo, dst_lo, hi);
    w->edx = vertex_relocate_frame_ptr(w->edx, lo, dst_lo, hi);
    wr32(w->ebp - 8u,
         vertex_relocate_frame_ptr(rd32(w->ebp - 8u), lo, dst_lo, hi));
    w->eip = 0;
    w->disp_depth = 0;
    w->disp_pending_valid = 0;
    w->disp_pending_kind = WC3_DISP_PENDING_VA;
    w->disp_pending_target = 0;
    w->disp_draining = 0;
    w->fs_base = ctx->teb;
    w->fs = 0x53u;
    w->es = 0x2Bu;
    w->ds = 0x2Bu;
    w->ss = 0x2Bu;
    w->cs = 0x23u;
    w->gs = 0;
    w->parallel_vertex_delta = ctx->vertex_globals - WC3_VERTEX_GLOBAL_FIRST;
    wr32(ctx->teb + 0x00u, 0xFFFFFFFFu);
    return 1;
}

/* The private block has the same layout as the real one:
 *   0 count, 1 p0, 2 s0, 3 p1, 4 s1, 5 p2, 6 s2,
 *   7 pm, 8 sm, 9 p3, 10 p4, 11 s3, 12 s4. */
static void run_vertex_chunk(RayCtx *ctx, const RayJob *job)
{
    const uint32_t *vg = job->vertex_globals;
    uint32_t start = ctx->start;
    uint32_t count = ctx->count;
    uint32_t base = ctx->vertex_globals;
    CPU *w = &ctx->cpu;

    ctx->vertex_failed = 0;
    if (!count) return;

    for (unsigned i = 0; i < WC3_VERTEX_GLOBAL_WORDS; i++)
        wr32(base + i * 4u, vg[i]);
    wr32(base + 0x00u, count);
    wr32(base + 0x04u, vg[1] + start * vg[2]);
    wr32(base + 0x0Cu, vg[3] + start * vg[4]);
    wr32(base + 0x14u, vg[5] + start * vg[6]);
    if (job->vertex_kind == WC3_VERTEX_TRANSFORM)
        wr32(base + 0x1Cu, vg[7] + start * vg[8]);
    wr32(base + 0x24u, vg[9] + start * vg[11]);
    wr32(base + 0x28u, vg[10] + start * vg[12]);

    if (!prepare_vertex_ctx(ctx, job)) {
        ctx->vertex_failed = 1;
        return;
    }

    if (start) {
        uint32_t adv_ebx = start * rd32(w->ebp - 0x1Cu);
        uint32_t adv_esi = start * rd32(w->ebp - 0x0Cu);
        uint32_t adv_edi = start * rd32(w->ebp - 0x14u);
        uint32_t adv_edx = start * rd32(w->ebp - 0x10u);
        w->ebx += adv_ebx;
        w->esi += adv_esi;
        w->edi += adv_edi;
        wr32(w->ebp - 8u, rd32(w->ebp - 8u) + adv_edx);
        /* L_6F1419D4 subtracts the current EBX from [ebp+8] on entry.  The
         * worker's EBX is already advanced, so put the same amount back. */
        if (job->vertex_kind == WC3_VERTEX_COPY)
            wr32(w->ebp + 8u, rd32(w->ebp + 8u) + adv_ebx);
    }

    /* L_6F1417F0 receives its matrix pointer in ECX from the setup block;
     * L_6F1419D4 loads the same field itself.  Both get the private copy. */
    w->ecx = rd32(base + 0x04u);

    WC3_DIRECT_CALL_CLEAN(job->fn, w);
    ctx->vertex_xmm0 = w->xmm[0];
    ctx->vertex_xmm1 = w->xmm[1];
}

/* ---- triangle chunks -----------------------------------------------------
 *
 * L_6F125924 is the stride-3 fast path of the ray/triangle test.  It runs in
 * L_6F125810's frame and reads the ray through [ebp-8]/[ebp-4], the vertex
 * base through [ebp+0xC] and the index cursor through [ebp+0x18].  [ebp+0x14]
 * starts as the index count and is converted to a triangle count in place.
 * The loop writes only the best distance through ESI and the hit index through
 * [ebp+0x20], always with a strict "best > new" comparison.
 *
 * A worker copies the caller's frame, points ESI and [ebp+0x20] at two private
 * words above the argument area, and sets [ebp+0x14] to count*3 and
 * [ebp+0x18] to indices + start*6.  The function converts the index count back
 * to `count` triangles and lets its own epilogue pop the copied frame.  The
 * caller merges the private best/index pairs in increasing chunk order, so an
 * equal distance keeps the lowest triangle index.
 */

#if WC3_DEBUG && WC3_PARALLEL_TRIANGLE_VALIDATE
static int triangle_frame_ok(const CPU *c)
{
    if (c->ebp < TRIANGLE_FRAME_BELOW) return 0;
    if (c->ebp > 0xFFFFFFFFu - TRIANGLE_FRAME_ABOVE) return 0;
    uint32_t lo = c->ebp - TRIANGLE_FRAME_BELOW;
    if (!hle_android_guest_ok(lo, TRIANGLE_FRAME_COPY)) return 0;
    if (c->esp < lo || c->esp > c->ebp + TRIANGLE_FRAME_ABOVE) return 0;
    return 1;
}

static int triangle_index_span_ok(uint32_t ptr, uint32_t tri_count)
{
    uint64_t bytes = (uint64_t)tri_count * TRIANGLE_INDEX_STRIDE;
    if (!ptr || !tri_count || bytes > 0xFFFFFFFFu) return 0;
    return hle_android_guest_ok(ptr, (uint32_t)bytes);
}

/* The loop reads exactly three vertices per triangle, but not necessarily
 * contiguous indices.  Check the full span between the smallest and largest
 * index so every vertex the chunk can touch lies in mapped guest memory. */
static int triangle_vertex_span_ok(uint32_t vertex, uint32_t indices,
                                   uint32_t tri_count)
{
    if (!vertex || !indices || !tri_count) return 0;
    uint32_t n = tri_count * 3u;
    uint32_t min_index = 0xFFFFFFFFu;
    uint32_t max_index = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = rd16(indices + i * 2u);
        if (idx < min_index) min_index = idx;
        if (idx > max_index) max_index = idx;
    }
    uint64_t lo = (uint64_t)vertex + (uint64_t)min_index * 12u;
    uint64_t hi = (uint64_t)vertex + (uint64_t)max_index * 12u + 12u;
    if (hi > 0x100000000ull || hi <= lo) return 0;
    return hle_android_guest_ok((uint32_t)lo, (uint32_t)(hi - lo));
}
#endif

static int prepare_triangle_ctx(RayCtx *ctx, const RayJob *job,
                                int private_output)
{
    const CPU *src = job->cpu;
    CPU *w = &ctx->cpu;
    uint32_t lo;
    uint32_t dst_lo;

    if (src->ebp < TRIANGLE_FRAME_BELOW) return 0;
    if (src->ebp > 0xFFFFFFFFu - TRIANGLE_FRAME_ABOVE) return 0;
    lo = src->ebp - TRIANGLE_FRAME_BELOW;
    if (src->esp < lo || src->esp > src->ebp + TRIANGLE_FRAME_ABOVE)
        return 0;
    dst_lo = ctx->stack_top - TRIANGLE_FRAME_COPY;
    if (dst_lo < ctx->stack_base) return 0;
#if WC3_DEBUG && WC3_PARALLEL_TRIANGLE_VALIDATE
    if (!hle_android_guest_ok(dst_lo, TRIANGLE_FRAME_COPY)) return 0;
#endif
    memcpy(guest_host_ptr(dst_lo), guest_host_ptr(lo),
           TRIANGLE_FRAME_COPY);

    *w = *src;
    w->ebp = dst_lo + TRIANGLE_FRAME_BELOW;
    w->esp = dst_lo + (src->esp - lo);
    w->eip = 0;
    w->disp_depth = 0;
    w->disp_pending_valid = 0;
    w->disp_pending_kind = WC3_DISP_PENDING_VA;
    w->disp_pending_target = 0;
    w->disp_draining = 0;
    w->fs_base = ctx->teb;
    w->fs = 0x53u;
    w->es = 0x2Bu;
    w->ds = 0x2Bu;
    w->ss = 0x2Bu;
    w->cs = 0x23u;
    w->gs = 0;
    w->parallel_vertex_delta = 0;
    w->parallel_triangle_bypass = 1;
    wr32(ctx->teb + 0x00u, 0xFFFFFFFFu);

    if (private_output) {
        ctx->triangle_best_addr = w->ebp + TRIANGLE_PRIVATE_BEST_OFF;
        ctx->triangle_index_addr = w->ebp + TRIANGLE_PRIVATE_IND_OFF;
        wr32(ctx->triangle_best_addr, job->triangle_orig_best);
        wr32(ctx->triangle_index_addr, 0);
        w->esi = ctx->triangle_best_addr;
        wr32(w->ebp + 0x20u, ctx->triangle_index_addr);
    } else {
        /* Sequential oracle: write the real output words so the caller can
         * compare its merged result against the definition of correct. */
        w->esi = job->triangle_best_ptr;
        wr32(w->ebp + 0x20u, job->triangle_index_ptr);
    }

    /* The function converts [ebp+0x14] from an index count to a triangle
     * count.  A chunk of `count` triangles therefore receives count*3. */
    wr32(w->ebp + 0x14u, ctx->count * 3u);
    wr32(w->ebp + 0x18u,
         job->triangle_indices + ctx->start * TRIANGLE_INDEX_STRIDE);
    return 1;
}

static void run_triangle_chunk(RayCtx *ctx, const RayJob *job)
{
    ctx->triangle_failed = 0;
    ctx->triangle_best = job->triangle_orig_best;
    ctx->triangle_index = 0;
    if (!ctx->count) return;
    if (!prepare_triangle_ctx(ctx, job, 1)) {
        ctx->triangle_failed = 1;
        return;
    }
    WC3_DIRECT_CALL_CLEAN(job->fn, &ctx->cpu);
    ctx->triangle_best = rd32(ctx->triangle_best_addr);
    ctx->triangle_index = rd32(ctx->triangle_index_addr);
}

static unsigned triangle_chunk_limit(void)
{
    unsigned limit = g_triangle_chunks;
    if (limit == 0 || limit > g_workers) limit = g_workers;
    return limit;
}

/* Vertex work has its own split.  It is not derived from the raycast
 * `chunks` value: an old raycast experiment must not cap the vertex path.
 * The default is all pool workers, one per online CPU. */
static unsigned vertex_chunk_limit(void)
{
    unsigned limit = g_vertex_chunks;
    if (limit == 0 || limit > g_workers) limit = g_workers;
    return limit;
}

/* Apply an optional affinity mask only when it changes.  The common case is a
 * single relaxed load per job; sched_setaffinity runs only when the user edits
 * worker_cpu_mask in the live config.  Mask 0 restores all online CPUs. */
static void apply_worker_affinity(RayCtx *ctx)
{
    unsigned mask = tune_uint(&g_tune.worker_cpu_mask);
    if (mask == ctx->applied_cpu_mask) return;
    ctx->applied_cpu_mask = mask;

    unsigned long long set = 0;
    if (mask) {
        for (unsigned cpu = 0; cpu < 32u; cpu++) {
            if (mask & (1u << cpu)) set |= 1ull << cpu;
        }
    } else if (!read_allowed_cpu_mask(&set)) {
        /* Fallback for kernels without sched_getaffinity: the old online-CPU
         * mask is still better than leaving the worker unbound. */
        long online = sysconf(_SC_NPROCESSORS_ONLN);
        if (online < 1) online = 1;
        if (online > 64) online = 64;
        for (long cpu = 0; cpu < online; cpu++) set |= 1ull << cpu;
    }

#if defined(__NR_sched_setaffinity)
    long rc = syscall(__NR_sched_setaffinity, 0, sizeof set, &set);
    if (rc != 0) {
        fprintf(stderr,
                "[raycast] worker %d: affinity mask 0x%X failed: %s\n",
                ctx->index, mask, strerror(errno));
    }
#else
    (void)set;
    fprintf(stderr,
            "[raycast] worker %d: affinity unavailable, mask 0x%X ignored\n",
            ctx->index, mask);
#endif
}

/* Inactive workers sleep on their own semaphore.  The drain loop removes a
 * token that was posted while a spin or an already-published job caught the
 * worker, so the next sleep cannot consume a wakeup that belongs to the past. */
static int worker_wait_sem(RayCtx *ctx, uint64_t seen)
{
    while (sem_trywait(&ctx->wake) == 0) {
    }
    if (atomic_load_explicit(&g_job_seq, memory_order_acquire) != seen)
        return 1;
    if (atomic_load_explicit(&g_shutdown, memory_order_acquire))
        return 0;

    while (sem_wait(&ctx->wake) != 0) {
        if (errno != EINTR) return 0;
    }
    if (atomic_load_explicit(&g_shutdown, memory_order_acquire))
        return 0;
    return 1;
}

/* Workers that ran the preceding job park on one shared futex.  A single
 * FUTEX_WAKE releases all of them; the caller no longer pays one syscall per
 * active chunk for the common same-size or shrinking batch. */
static int worker_wait_shared(RayCtx *ctx, uint64_t seen)
{
#if defined(__NR_futex)
    for (;;) {
        if (atomic_load_explicit(&g_shutdown, memory_order_acquire))
            return 0;
        if (atomic_load_explicit(&g_job_seq, memory_order_acquire) != seen)
            return 1;

        unsigned gen = atomic_load_explicit(&g_wake_gen,
                                            memory_order_acquire);
        /* The second sequence check closes the race with a dispatch that
         * increments the generation between the first check and the wait. */
        if (atomic_load_explicit(&g_job_seq, memory_order_acquire) != seen)
            return 1;

        long rc = syscall(__NR_futex, (int *)(uintptr_t)&g_wake_gen,
                          WC3_FUTEX_WAIT_PRIVATE, (int)gen, NULL, NULL, 0);
        if (rc == 0 || errno == EAGAIN || errno == EINTR)
            continue;

        /* A rejected futex wait must not strand a worker.  Polling the job
         * sequence with a short sleep is the safe fallback. */
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 50000L;
        (void)nanosleep(&ts, NULL);
    }
#else
    return worker_wait_sem(ctx, seen);
#endif
}

/* Wait for a new job.  `wait_shared` says whether this worker ran the
 * preceding job; only those workers may park on the shared futex.  Workers
 * outside that set stay asleep until a growing batch posts their semaphore. */
static int worker_wait_for_job(RayCtx *ctx)
{
    uint64_t seen = ctx->seen_seq;
    unsigned spin_us = tune_uint(&g_tune.spin_us);
    int spin_allowed = ctx->wait_shared;

    if (atomic_load_explicit(&g_shutdown, memory_order_acquire))
        return 0;
    if (atomic_load_explicit(&g_job_seq, memory_order_acquire) != seen)
        return 1;

    if (spin_us && spin_allowed) {
        uint64_t deadline = now_ns() + (uint64_t)spin_us * 1000ull;
        unsigned loops = 0;
        while (atomic_load_explicit(&g_job_seq, memory_order_acquire) == seen) {
            if ((++loops & 0x7Fu) == 0 && now_ns() >= deadline) break;
            cpu_relax();
        }
    }

    if (atomic_load_explicit(&g_shutdown, memory_order_acquire))
        return 0;
    if (atomic_load_explicit(&g_job_seq, memory_order_acquire) != seen)
        return 1;

    if (g_shared_wake && spin_allowed)
        return worker_wait_shared(ctx, seen);
    return worker_wait_sem(ctx, seen);
}

static void *worker_main(void *arg)
{
    RayCtx *ctx = (RayCtx *)arg;
    char name[16];
    snprintf(name, sizeof name, "wc3-ray-%d", ctx->index);
    (void)pthread_setname_np(pthread_self(), name);

    for (;;) {
        if (!worker_wait_for_job(ctx)) break;
        apply_worker_affinity(ctx);

        uint64_t seq = atomic_load_explicit(&g_job_seq, memory_order_acquire);
        if (seq == ctx->seen_seq) continue;

        uint64_t active_seq = atomic_load_explicit(&g_active_seq,
                                                   memory_order_acquire);
        if ((uint32_t)(active_seq >> 32) != (uint32_t)seq) {
            /* The tagged active set belongs to a different job, typically a
             * newer dispatch whose sequence store is not visible yet.  Never
             * combine it with the older sequence or read that job's stack
             * storage; retry after the publication completes. */
            cpu_relax();
            continue;
        }
        ctx->seen_seq = seq;

        unsigned idx = (unsigned)ctx->index;
        unsigned active = (unsigned)active_seq;
        if (idx >= active) {
            /* This worker is outside the active set.  It must not read the
             * stack RayJob: the caller only waits for the active chunks and
             * may return while this worker is still running.  It also parks
             * on its private semaphore for the next batch. */
            ctx->wait_shared = 0;
            continue;
        }
        /* This worker runs a chunk, so the next wait may use the shared
         * futex.  Set it before touching the job so a concurrent dispatch
         * cannot observe a half-updated queue choice. */
        ctx->wait_shared = 1;

        RayJob *job = (RayJob *)(uintptr_t)atomic_load_explicit(
            &g_job_addr, memory_order_acquire);
        if (!job) {
            /* A published sequence always has a job.  Report anyway so a
             * caller waiting for an active worker cannot hang on a transient
             * null pointer. */
            atomic_store_explicit(&ctx->done_seq, seq, memory_order_release);
            continue;
        }

        uint32_t base = job->chunk_base;
        uint32_t rem = job->chunk_rem;
        uint32_t extra = idx < rem ? idx : rem;
        ctx->start = idx * base + extra;
        ctx->count = base + (idx < rem);
        if (job->kind == RAY_JOB_VERTEX)
            run_vertex_chunk(ctx, job);
        else if (job->kind == RAY_JOB_TRIANGLE)
            run_triangle_chunk(ctx, job);
        else
            run_chunk(ctx, job);
        /* Only workers in the published active set report.  The caller waits
         * for exactly those chunks, so no inactive worker can read the stack
         * RayJob after the dispatch returns. */
        atomic_store_explicit(&ctx->done_seq, seq, memory_order_release);
    }
    return NULL;
}

static void prewarm_one(uint32_t target, uint32_t sentinel)
{
    wc3_lifted_fn fn = wc3_dispatch_resolve(target);
    if (!fn) {
        fprintf(stderr, "[raycast] prewarm: no lifted function at %08X\n",
                target);
        return;
    }

    /* Prewarm before the worker threads start, using the first worker's
     * private context.  Release builds therefore do not need a separate
     * verification context. */
    RayCtx *ctx = &g_worker[0];
    prepare_base(ctx, NULL, 0);
    push_arg(&ctx->cpu, sentinel);
    WC3_DIRECT_CALL_CLEAN(fn, &ctx->cpu);
#if WC3_DEBUG && WC3_DEBUG_HLE
    if (ctx->cpu.esp != ctx->stack_top) {
        fprintf(stderr,
                "[raycast] prewarm %08X left esp=%08X, expected %08X\n",
                target, ctx->cpu.esp, ctx->stack_top);
    }
#endif
}

static void init_once(void)
{
    /* One product switch disables every parallel guest path.  The old
     * WC3_PARALLEL_RAYCAST kill switch stays independent so a diagnostic run
     * can compare the raycast path without also disabling the vertex loops. */
    if (env_off("WC3_PARALLEL_GAMEPLAY")) {
        g_raycast_enabled = 0;
        g_vertex_enabled = 0;
        g_triangle_enabled = 0;
#if WC3_DEBUG && WC3_DEBUG_HLE
        fprintf(stderr, "[parallel] disabled by WC3_PARALLEL_GAMEPLAY\n");
#endif
        atomic_store_explicit(&g_init_ok, 1, memory_order_release);
        return;
    }
    g_raycast_enabled = !env_off("WC3_PARALLEL_RAYCAST");
    g_vertex_enabled = !env_off("WC3_VERTEX");
    g_triangle_enabled = !env_off("WC3_TRIANGLE");
    if (!g_raycast_enabled && !g_vertex_enabled && !g_triangle_enabled) {
#if WC3_DEBUG && WC3_DEBUG_HLE
        fprintf(stderr, "[parallel] all guest paths disabled\n");
#endif
        atomic_store_explicit(&g_init_ok, 1, memory_order_release);
        return;
    }

    g_vertex_threshold = env_u("WC3_VERTEX_THRESHOLD",
                               VERTEX_DEFAULT_THRESHOLD, VERTEX_MAX_COUNT);
    g_vertex_chunks = env_u("WC3_VERTEX_CHUNKS", 0, UINT_MAX);
    g_vertex_min_chunk = env_u("WC3_VERTEX_MIN_CHUNK",
                               VERTEX_DEFAULT_MIN_CHUNK, VERTEX_MAX_COUNT);
    if (g_vertex_min_chunk && g_vertex_min_chunk < 4u) g_vertex_min_chunk = 4u;
    g_triangle_threshold = env_u("WC3_TRIANGLE_THRESHOLD",
                                 TRIANGLE_DEFAULT_THRESHOLD,
                                 TRIANGLE_MAX_COUNT);
    g_triangle_chunks = env_u("WC3_TRIANGLE_CHUNKS", 0, UINT_MAX);
    g_triangle_min_chunk = env_u("WC3_TRIANGLE_MIN_CHUNK", TRIANGLE_MIN_CHUNK,
                                 TRIANGLE_MAX_COUNT);

    g_workers = env_u("WC3_PARALLEL_RAYCAST_THREADS", 0, UINT_MAX);
    if (!g_workers) g_workers = auto_worker_count();
    if (g_workers < 1u) g_workers = 1u;
    if (g_vertex_chunks == 0 || g_vertex_chunks > g_workers)
        g_vertex_chunks = g_workers;
    if (g_triangle_chunks == 0 || g_triangle_chunks > g_workers)
        g_triangle_chunks = g_workers;

    set_tuning_defaults();
#if WC3_DEBUG && WC3_PARALLEL_VERTEX_VALIDATE
    const char *vertex_verify = getenv("WC3_VERTEX_VERIFY");
    g_vertex_verify = vertex_verify && *vertex_verify &&
                      !env_off("WC3_VERTEX_VERIFY");
#endif
#if WC3_DEBUG && WC3_PARALLEL_TRIANGLE_VALIDATE
    g_triangle_verify = WC3_TRIANGLE_VERIFY_DEFAULT;
    const char *triangle_verify = getenv("WC3_TRIANGLE_VERIFY");
    if (triangle_verify && *triangle_verify)
        g_triangle_verify = !env_off("WC3_TRIANGLE_VERIFY");
#endif
#if WC3_DEBUG && WC3_DEBUG_HLE
    if (!g_tune_path_ready) init_tune_path();
    load_tuning_file(0);
#endif

    g_shared_wake = futex_probe();
    g_prev_active = 0;
#if WC3_DEBUG && WC3_DEBUG_HLE
    fprintf(stderr, "[raycast] shared futex wake %s\n",
            g_shared_wake ? "enabled" : "unavailable; using semaphores");
#endif

    g_worker = calloc((size_t)g_workers, sizeof *g_worker);
    if (!g_worker) {
        fprintf(stderr, "[raycast] cannot allocate %u worker context(s)\n",
                g_workers);
        atomic_store_explicit(&g_init_ok, 1, memory_order_release);
        return;
    }

#if WC3_DEBUG && (WC3_DEBUG_HLE || WC3_PARALLEL_VERTEX_VALIDATE || \
                  WC3_PARALLEL_TRIANGLE_VALIDATE)
    if (!alloc_ctx(&g_verify_ctx, g_workers)) {
        fprintf(stderr, "[raycast] cannot allocate verification context\n");
        free(g_worker);
        g_worker = NULL;
        atomic_store_explicit(&g_init_ok, 1, memory_order_release);
        return;
    }
#endif

    for (unsigned i = 0; i < g_workers; i++) {
        if (!alloc_ctx(&g_worker[i], i)) {
            fprintf(stderr, "[raycast] cannot allocate worker %u context\n", i);
            goto fail;
        }
    }

    /* The two lazily allocated engine singletons are reached from the same
     * object-render call site.  L_6F78E790 has already run once before the
     * wrapper is entered, but prewarming both here keeps the rule explicit
     * and makes the worker path independent of which caller arrived first.
     * Worker 0 is not running yet, so its context can be used as a scratch
     * context without adding a release-only allocation. */
    g_pool_init_active = 1;
    prewarm_one(0x6F78E790u, 0x6F78D9EBu);
    prewarm_one(0x6F78E810u, 0x6F78D9F7u);
    g_pool_init_active = 0;

    for (unsigned i = 0; i < g_workers; i++) {
        g_worker[i].index = (int)i;
        if (pthread_create(&g_worker[i].thread, NULL, worker_main,
                           &g_worker[i]) != 0) {
            fprintf(stderr, "[raycast] cannot create worker %u\n", i);
            goto fail;
        }
        g_worker[i].thread_started = 1;
    }

    g_parallel_ok = 1;
    atomic_store_explicit(&g_init_ok, 1, memory_order_release);
#if WC3_DEBUG && WC3_PARALLEL_TRIANGLE_VALIDATE
    fprintf(stderr, "[triangle] sequential oracle %s\n",
            g_triangle_verify ? "on" : "off (set WC3_TRIANGLE_VERIFY=1)");
#endif
#if WC3_DEBUG && WC3_DEBUG_HLE
    fprintf(stderr,
            "[parallel] pool: %u worker(s) for %ld online CPU(s), "
            "ray threshold %u, vertex threshold %u, vertex chunks %u, "
            "vertex min_chunk %u, triangle threshold %u, triangle chunks %u, "
            "triangle min_chunk %u, verify %s\n",
            g_workers, sysconf(_SC_NPROCESSORS_ONLN),
            tune_uint(&g_tune.threshold), g_vertex_threshold,
            g_vertex_chunks, g_vertex_min_chunk, g_triangle_threshold,
            g_triangle_chunks, g_triangle_min_chunk,
            tune_int(&g_tune.verify) ? "on" : "off");
    fprintf(stderr, "[raycast] tune file: %s (%s)\n", g_tune_path,
            g_tune_file_enabled
                ? "enabled by WC3_RAY_TUNE"
                : "ignored; set WC3_RAY_TUNE=1 to load");
#if WC3_DEBUG && WC3_PARALLEL_VERTEX_VALIDATE
    fprintf(stderr, "[vertex] bit-exact verification %s\n",
            g_vertex_verify ? "on" : "off (set WC3_VERTEX_VERIFY=1)");
#endif
    log_tuning("init");
#endif
    return;

fail:
    atomic_store_explicit(&g_shutdown, 1, memory_order_release);
    for (unsigned i = 0; i < g_workers; i++) {
        if (g_worker[i].thread_started)
            sem_post(&g_worker[i].wake);
    }
    for (unsigned i = 0; i < g_workers; i++) {
        if (g_worker[i].thread_started)
            pthread_join(g_worker[i].thread, NULL);
    }
    for (unsigned i = 0; i < g_workers; i++) {
        if (g_worker[i].wake_init) {
            sem_destroy(&g_worker[i].wake);
            g_worker[i].wake_init = 0;
        }
    }
#if WC3_DEBUG && (WC3_DEBUG_HLE || WC3_PARALLEL_VERTEX_VALIDATE || \
                  WC3_PARALLEL_TRIANGLE_VALIDATE)
    if (g_verify_ctx.wake_init) {
        sem_destroy(&g_verify_ctx.wake);
        g_verify_ctx.wake_init = 0;
    }
#endif
    free(g_worker);
    g_worker = NULL;
    g_parallel_ok = 0;
    atomic_store_explicit(&g_init_ok, 1, memory_order_release);
}

static inline int ready(void)
{
    if (atomic_load_explicit(&g_init_ok, memory_order_acquire))
        return g_parallel_ok;
    pthread_once(&g_once, init_once);
    return g_parallel_ok;
}

/* Exact float comparison through the stored bits.  The generated code uses
 * ordinary float compares, so a host float load here is the same operation;
 * memcpy avoids the aliasing rule and keeps the bit pattern unchanged. */
static float bits_float(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static void merge_results(RayCtx *out, unsigned n)
{
    unsigned i;
    out->hit = 0;
    out->dist_bits = 0;
    out->pos[0] = out->pos[1] = out->pos[2] = 0;

    for (i = 0; i < n; i++) {
        RayCtx *r = &g_worker[i];
        if (!r->hit) continue;
        if (!out->hit) {
            out->hit = 1;
            out->dist_bits = r->dist_bits;
            out->pos[0] = r->pos[0];
            out->pos[1] = r->pos[1];
            out->pos[2] = r->pos[2];
            continue;
        }
        if (bits_float(r->dist_bits) < bits_float(out->dist_bits)) {
            out->dist_bits = r->dist_bits;
            out->pos[0] = r->pos[0];
            out->pos[1] = r->pos[1];
            out->pos[2] = r->pos[2];
        }
    }
}

/* Run the original function once on its own CPU and private stack.  The
 * caller's container and arrays are used directly, so this is exactly the
 * sequential call. */
#if WC3_DEBUG && WC3_DEBUG_HLE
static void run_verify(CPU *caller, wc3_lifted_fn fn, uint32_t container,
                       uint32_t in1, uint32_t in2, uint32_t out_pos,
                       uint32_t out_dist, uint32_t user_ctx, RayCtx *out)
{
    RayCtx *ctx = &g_verify_ctx;
    out->hit = 0;
    out->dist_bits = 0;
    out->pos[0] = out->pos[1] = out->pos[2] = 0;

    memset(guest_host_ptr(ctx->out_pos), 0, 12);
    memset(guest_host_ptr(ctx->out_dist), 0, 4);
    memset(guest_host_ptr(ctx->stack_top - RAY_STACK_ZERO_MAX), 0,
           RAY_STACK_ZERO_MAX);

    prepare_base(ctx, caller, 1);
    ctx->cpu.ecx = container;
    push_arg(&ctx->cpu, user_ctx);
    push_arg(&ctx->cpu, ctx->out_dist);
    push_arg(&ctx->cpu, ctx->out_pos);
    push_arg(&ctx->cpu, in2);
    push_arg(&ctx->cpu, in1);
    push_arg(&ctx->cpu, RAY_SENTINEL);

    WC3_DIRECT_CALL_CLEAN(fn, &ctx->cpu);

    out->hit = (ctx->cpu.eax != 0) ? 1u : 0u;
    if (out->hit) {
        out->dist_bits = rd32(ctx->out_dist);
        out->pos[0] = rd32(ctx->out_pos + 0u);
        out->pos[1] = rd32(ctx->out_pos + 4u);
        out->pos[2] = rd32(ctx->out_pos + 8u);
    }
}

static void log_mismatch(CPU *c, uint32_t container, uint32_t total,
                         const RayCtx *par, const RayCtx *seq)
{
    fprintf(stderr,
            "[raycast] VERIFY MISMATCH total=%u call=%llu hit par=%u seq=%u "
            "dist par=%08X seq=%08X pos par=%08X,%08X,%08X "
            "seq=%08X,%08X,%08X ret=%08X cont=%08X\n",
            total, g_calls, par->hit, seq->hit,
            par->dist_bits, seq->dist_bits,
            par->pos[0], par->pos[1], par->pos[2],
            seq->pos[0], seq->pos[1], seq->pos[2],
            c->esp, container);
}
#endif

#if WC3_DEBUG && WC3_DEBUG_PERF
static void maybe_print_stats(void)
{
    if (!tune_int(&g_tune.stats)) return;
    unsigned every = tune_uint(&g_tune.stats_every);
    if (!every) every = RAY_STATS_DEFAULT;
    if (g_calls - g_stats_last_calls < every) return;

    unsigned long long d_calls = g_calls - g_stats_last_calls;
    unsigned long long d_parallel = g_parallel_calls - g_stats_last_parallel;
    unsigned long long d_fallback = g_fallback_calls - g_stats_last_fallback;
    unsigned long long d_parallel_ns =
        g_parallel_ns - g_stats_last_parallel_ns;
    unsigned long long d_fallback_ns =
        g_fallback_ns - g_stats_last_fallback_ns;

    char par_avg[32];
    char fb_avg[32];
    if (tune_int(&g_tune.timing) && d_parallel)
        snprintf(par_avg, sizeof par_avg, "%.1fus",
                 (double)d_parallel_ns / (double)d_parallel / 1000.0);
    else
        snprintf(par_avg, sizeof par_avg, "n/a");
    if (tune_int(&g_tune.timing) && d_fallback)
        snprintf(fb_avg, sizeof fb_avg, "%.1fus",
                 (double)d_fallback_ns / (double)d_fallback / 1000.0);
    else
        snprintf(fb_avg, sizeof fb_avg, "n/a");

    unsigned long long total_avg =
        g_ray_total_samples ? g_ray_total_sum / g_ray_total_samples : 0;

    fprintf(stderr,
            "[raycast] stats +%llu calls: parallel=%llu (+%llu) "
            "fallback=%llu (+%llu) par_avg=%s fb_avg=%s "
            "last_total=%u last_chunks=%u threshold=%u chunks=%u "
            "stack_zero=%u copy=%u spin_us=%u main_spin_us=%u "
            "cpu_mask=0x%X total_n=%llu total_avg=%llu total_max=%u "
            "total_hist=[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]\n",
            d_calls,
            g_parallel_calls, d_parallel,
            g_fallback_calls, d_fallback,
            par_avg, fb_avg,
            g_last_total, g_last_chunks,
            tune_uint(&g_tune.threshold),
            tune_uint(&g_tune.chunk_limit),
            tune_uint(&g_tune.stack_zero),
            tune_uint(&g_tune.container_copy),
            tune_uint(&g_tune.spin_us),
            tune_uint(&g_tune.main_spin_us),
            tune_uint(&g_tune.worker_cpu_mask),
            g_ray_total_samples, total_avg, g_ray_total_max,
            g_ray_total_hist[0], g_ray_total_hist[1],
            g_ray_total_hist[2], g_ray_total_hist[3],
            g_ray_total_hist[4], g_ray_total_hist[5],
            g_ray_total_hist[6], g_ray_total_hist[7],
            g_ray_total_hist[8], g_ray_total_hist[9],
            g_ray_total_hist[10], g_ray_total_hist[11]);

    g_stats_last_calls = g_calls;
    g_stats_last_parallel = g_parallel_calls;
    g_stats_last_fallback = g_fallback_calls;
    g_stats_last_parallel_ns = g_parallel_ns;
    g_stats_last_fallback_ns = g_fallback_ns;
}
#endif

#define RAY_WAIT_SLEEP_NS 100000L

static int workers_done(unsigned workers, uint64_t seq)
{
    for (unsigned i = 0; i < workers; i++) {
        if (atomic_load_explicit(&g_worker[i].done_seq,
                                 memory_order_acquire) != seq)
            return 0;
    }
    return 1;
}

/* The old path blocked in sem_wait and paid a kernel wakeup on every ray.  A
 * short arrival poll covers the normal case; the sleep loop is only a safety
 * net for a worker that was descheduled or is waiting on an unrelated lock. */
static void wait_for_workers(unsigned workers, uint64_t seq,
                             unsigned main_spin_us)
{
    if (!workers) return;

    if (main_spin_us) {
        uint64_t deadline = now_ns() + (uint64_t)main_spin_us * 1000ull;
        unsigned loops = 0;
        while (!workers_done(workers, seq)) {
            if ((++loops & 0x7Fu) == 0 && now_ns() >= deadline) break;
            cpu_relax();
        }
    }

    while (!workers_done(workers, seq)) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = RAY_WAIT_SLEEP_NS;
        (void)nanosleep(&ts, NULL);
    }
}

/* Release exactly the workers that need to see the published job.  Common
 * repeated batches have the same active count: one shared futex wake covers
 * every one of them.  Growth additionally posts the semaphores of the newly
 * active workers, which were parked outside the previous active set. */
static void wake_workers(unsigned limit)
{
    if (!g_shared_wake) {
        for (unsigned i = 0; i < limit; i++)
            sem_post(&g_worker[i].wake);
        g_prev_active = limit;
        return;
    }

    unsigned prev = g_prev_active;
    if (prev)
        wake_shared_workers();
    if (limit > prev) {
        for (unsigned i = prev; i < limit; i++)
            sem_post(&g_worker[i].wake);
    }
    g_prev_active = limit;
}

/* Publish a fully initialized stack job.  The address and tagged active set
 * are visible before the sequence release store, so any worker that acquires
 * sequence N can trust the matching active set and can then read job N.  Only
 * the submit-lock holder writes g_job_seq, so load+store is a valid increment
 * while allowing the sequence store to remain the last publication. */
static uint64_t publish_job(uintptr_t addr, unsigned active)
{
    uint64_t seq = atomic_load_explicit(&g_job_seq, memory_order_relaxed) + 1u;
    atomic_store_explicit(&g_job_addr, addr, memory_order_release);
    atomic_store_explicit(&g_active_seq,
                          ((uint64_t)(uint32_t)seq << 32) | active,
                          memory_order_release);
    atomic_store_explicit(&g_job_seq, seq, memory_order_release);
    return seq;
}

/* Dispatch a vertex batch.  All source and output ranges were validated by
 * the caller.  Returns 0 only when the pool could not be used; on 1 the
 * caller's frame and the real globals already match the sequential epilogue. */
static int run_vertex_batch(CPU *c, wc3_lifted_fn fn, unsigned kind,
                            uint32_t total, unsigned chunks,
                            const uint32_t *vg)
{
    if (pthread_mutex_trylock(&g_submit_lock) != 0) return 0;
    if (t_ray_active || c->parallel_vertex_bypass) {
        pthread_mutex_unlock(&g_submit_lock);
        return 0;
    }
    t_ray_active = 1;

    RayJob job;
    job.kind = RAY_JOB_VERTEX;
    job.vertex_kind = kind;
    job.fn = fn;
    job.cpu = c;
    job_set_chunk_split(&job, total, chunks);
    memcpy(job.vertex_globals, vg, sizeof job.vertex_globals);

    uint64_t seq = publish_job((uintptr_t)&job, chunks);
    /* Wake only the workers that execute a chunk.  Inactive workers sleep on
     * their own semaphore and never read the stack RayJob. */
    wake_workers(chunks);

    wait_for_workers(chunks, seq, tune_uint(&g_tune.main_spin_us));
    atomic_store_explicit(&g_job_addr, 0, memory_order_release);

    int failed = 0;
    for (unsigned i = 0; i < chunks; i++) {
        if (g_worker[i].vertex_failed) {
            failed = 1;
            break;
        }
    }

    if (failed) {
        /* A worker could not copy its private frame after all.  The real
         * globals and the caller's frame are still untouched; run the original
         * function once on the caller.  The entry guard is told to stay out of
         * this recovery call. */
        uint32_t saved_bypass = c->parallel_vertex_bypass;
        c->parallel_vertex_bypass = 1;
        WC3_DIRECT_CALL_CLEAN(fn, c);
        c->parallel_vertex_bypass = saved_bypass;
        t_ray_active = 0;
        pthread_mutex_unlock(&g_submit_lock);
        return 1;
    }

    /* The original loops advance p0..p4 (and pm on the transform path) once
     * per vertex.  Apply the same 32-bit additions to the real block. */
    uint32_t p0 = vg[1] + total * vg[2];
    wr32(GVA(0x6FD5393Cu), p0);
    wr32(GVA(0x6FD53944u), vg[3] + total * vg[4]);
    wr32(GVA(0x6FD5394Cu), vg[5] + total * vg[6]);
    if (kind == WC3_VERTEX_TRANSFORM)
        wr32(GVA(0x6FD53954u), vg[7] + total * vg[8]);
    wr32(GVA(0x6FD5395Cu), vg[9] + total * vg[11]);
    wr32(GVA(0x6FD53960u), vg[10] + total * vg[12]);

    RayCtx *last = &g_worker[chunks - 1u];
    c->xmm[0] = last->vertex_xmm0;
    c->xmm[1] = last->vertex_xmm1;

    /* Observable exit state differs slightly between the two bodies:
     *
     *   copy       eax = advanced p0, ecx = count, [ebp-8] unchanged,
     *              [ebp+8] = original delta - original EBX;
     *   transform  eax = count, ecx = advanced p0, [ebp-8] = EDX.
     *
     * Both leave [ebp-0x18] = count, EDX = advanced optional pointer and
     * flags from the final count comparison. */
    uint32_t edx_final = rd32(c->ebp - 8u) +
                         total * rd32(c->ebp - 0x10u);
    wr32(c->ebp - 0x18u, total);
    if (kind == WC3_VERTEX_TRANSFORM) {
        wr32(c->ebp - 8u, edx_final);
        c->eax = total;
        c->ecx = p0;
    } else {
        /* The copy body performs this subtraction itself after the guard.
         * Its loop starts from edx=[ebp-8], but never stores the advanced EDX
         * back into the local. */
        wr32(c->ebp + 8u, sub32(rd32(c->ebp + 8u), c->ebx, 4));
        c->eax = p0;
        c->ecx = total;
    }
    c->edx = edx_final;

    c->edi = (uint32_t)pop32(c);
    c->esi = (uint32_t)pop32(c);
    c->ebx = (uint32_t)pop32(c);
    c->esp = c->ebp;
    c->ebp = (uint32_t)pop32(c);
    c->esp += 8;

    flags_sub(c, total, total, 4);

    t_ray_active = 0;
    pthread_mutex_unlock(&g_submit_lock);
    return 1;
}

#if WC3_DEBUG && WC3_PARALLEL_VERTEX_VALIDATE
typedef struct VertexRange {
    uint64_t lo;
    uint64_t hi;
    uint32_t stride;
    uint32_t size;
    int used;
} VertexRange;

/* Two interleaved streams can share a bounding box while writing disjoint
 * fields of the same vertex record.  Compare the repeated intervals when the
 * strides match; keep the conservative bounding check otherwise. */
static int circle_overlap(uint32_t a_off, uint32_t a_size,
                          uint32_t b_off, uint32_t b_size,
                          uint32_t stride)
{
    for (int k = -1; k <= 1; k++) {
        int64_t bs = (int64_t)b_off + (int64_t)k * stride;
        int64_t be = bs + b_size;
        if ((int64_t)a_off < be && bs < (int64_t)a_off + a_size)
            return 1;
    }
    return 0;
}

static int ranges_overlap(const VertexRange *a, const VertexRange *b)
{
    if (!a->used || !b->used) return 0;
    if (a->stride && a->stride == b->stride &&
        a->size <= a->stride && b->size <= b->stride) {
        uint32_t a_off = (uint32_t)(a->lo % a->stride);
        uint32_t b_off = (uint32_t)(b->lo % b->stride);
        return circle_overlap(a_off, a->size, b_off, b->size, a->stride);
    }
    return a->lo < b->hi && b->lo < a->hi;
}
#endif

#if WC3_DEBUG && WC3_PARALLEL_VERTEX_VALIDATE
typedef struct VertexFrameSkip {
    uint32_t off;
    uint32_t size;
} VertexFrameSkip;

typedef struct VertexOracle {
    VertexRange out[4];
    uint32_t count;
    uint8_t *snapshot[4];
    /* Zero-stride optional outputs use an in-frame dummy.  Workers write a
     * private copy, so the caller's dummy differs from the sequential run and
     * is not part of the observable frame. */
    VertexFrameSkip frame_skip[4];
    unsigned frame_skip_count;
} VertexOracle;

static void vertex_oracle_init(VertexOracle *o, const VertexRange *out,
                               uint32_t count, const VertexFrameSkip *skip,
                               unsigned skip_count)
{
    memcpy(o->out, out, sizeof o->out);
    o->count = count;
    memset(o->snapshot, 0, sizeof o->snapshot);
    o->frame_skip_count = skip_count < 4u ? skip_count : 4u;
    memcpy(o->frame_skip, skip, o->frame_skip_count * sizeof *skip);
}

static void vertex_oracle_free(VertexOracle *o)
{
    for (unsigned i = 0; i < 4u; i++) {
        free(o->snapshot[i]);
        o->snapshot[i] = NULL;
    }
}

/* Copy the bytes the loop is allowed to write, in increasing vertex order.
 * The gaps between strided records are not part of the contract.  An exact
 * snapshot avoids relying on a hash collision not to hide a divergence. */
static int vertex_oracle_snapshot(VertexOracle *o)
{
    for (unsigned i = 0; i < 4u; i++) {
        if (!o->out[i].used) continue;
        size_t bytes = (size_t)o->count * o->out[i].size;
        if (!bytes) continue;
        uint8_t *buf = malloc(bytes);
        if (!buf) {
            vertex_oracle_free(o);
            return 0;
        }
        o->snapshot[i] = buf;
        uint32_t p = (uint32_t)o->out[i].lo;
        for (uint32_t j = 0; j < o->count; j++) {
            memcpy(buf + (size_t)j * o->out[i].size,
                   guest_host_ptr(p), o->out[i].size);
            p += o->out[i].stride;
        }
    }
    return 1;
}

static int vertex_oracle_compare(VertexOracle *o)
{
    int ok = 1;
    for (unsigned i = 0; i < 4u; i++) {
        if (!o->out[i].used || !o->snapshot[i]) continue;
        uint32_t p = (uint32_t)o->out[i].lo;
        for (uint32_t j = 0; j < o->count; j++) {
            if (memcmp(o->snapshot[i] + (size_t)j * o->out[i].size,
                       guest_host_ptr(p), o->out[i].size) != 0) {
                ok = 0;
                break;
            }
            p += o->out[i].stride;
        }
    }
    vertex_oracle_free(o);
    return ok;
}

static int vertex_compare_cpu(const CPU *want, const CPU *got)
{
    if (want->eax != got->eax || want->ecx != got->ecx ||
        want->edx != got->edx || want->ebx != got->ebx ||
        want->esp != got->esp || want->ebp != got->ebp ||
        want->esi != got->esi || want->edi != got->edi ||
        want->eip != got->eip ||
        want->cf != got->cf || want->zf != got->zf ||
        want->sf != got->sf || want->of != got->of ||
        want->pf != got->pf || want->af != got->af)
        return 0;
    return memcmp(&want->xmm[0], &got->xmm[0], sizeof(XMM)) == 0 &&
           memcmp(&want->xmm[1], &got->xmm[1], sizeof(XMM)) == 0;
}

static int vertex_frame_equal(const uint8_t *par, const uint8_t *seq,
                              const VertexOracle *o)
{
    for (uint32_t i = 0; i < VERTEX_FRAME_COPY; i++) {
        int skipped = 0;
        for (unsigned j = 0; j < o->frame_skip_count; j++) {
            if (i >= o->frame_skip[j].off &&
                i < o->frame_skip[j].off + o->frame_skip[j].size) {
                skipped = 1;
                break;
            }
        }
        if (!skipped && par[i] != seq[i]) return 0;
    }
    return 1;
}

/* Re-run the full original loop on the caller after a pooled batch and compare
 * every observable result.  The sequential run becomes the final state; a
 * mismatch disables the vertex path instead of leaving a divergent frame. */
static int vertex_verify_after(CPU *c, wc3_lifted_fn fn, uint32_t count,
                               const uint32_t *vg, const CPU *before,
                               const uint8_t *frame_before,
                               VertexOracle *oracle)
{
    uint32_t par_globals[WC3_VERTEX_GLOBAL_WORDS];
    CPU par_cpu = *c;
    for (unsigned i = 0; i < WC3_VERTEX_GLOBAL_WORDS; i++)
        par_globals[i] = rd32(GVA(WC3_VERTEX_GLOBAL_FIRST + i * 4u));

    uint8_t par_frame[VERTEX_FRAME_COPY];
    memcpy(par_frame,
           guest_host_ptr(before->ebp - VERTEX_FRAME_BELOW),
           sizeof par_frame);
    if (!vertex_oracle_snapshot(oracle)) {
        static unsigned alloc_reports;
        if (alloc_reports < 4u) {
            alloc_reports++;
            fprintf(stderr,
                    "[vertex] VERIFY skipped: cannot snapshot %u output(s)\n",
                    count);
        }
        return 1;
    }

    /* The successful parallel merge has already rewritten the caller frame
     * to its post-loop state.  Restore the pre-batch frame so the sequential
     * oracle starts from the same inputs as the workers. */
    memcpy(guest_host_ptr(before->ebp - VERTEX_FRAME_BELOW),
           frame_before, VERTEX_FRAME_COPY);
    *c = *before;
    for (unsigned i = 0; i < WC3_VERTEX_GLOBAL_WORDS; i++)
        wr32(GVA(WC3_VERTEX_GLOBAL_FIRST + i * 4u), vg[i]);
    c->parallel_vertex_bypass = 1;
    WC3_DIRECT_CALL_CLEAN(fn, c);
    c->parallel_vertex_bypass = before->parallel_vertex_bypass;

    int out_ok = vertex_oracle_compare(oracle);
    int frame_ok = vertex_frame_equal(
        par_frame,
        guest_host_ptr(before->ebp - VERTEX_FRAME_BELOW),
        oracle);
    int cpu_ok = vertex_compare_cpu(&par_cpu, c);
    int globals_ok = 1;
    for (unsigned i = 0; i < WC3_VERTEX_GLOBAL_WORDS; i++) {
        if (par_globals[i] !=
            rd32(GVA(WC3_VERTEX_GLOBAL_FIRST + i * 4u))) {
            globals_ok = 0;
            break;
        }
    }
    int ok = out_ok && frame_ok && cpu_ok && globals_ok;

    if (!ok) {
        static unsigned reports;
        if (reports < 8u) {
            reports++;
            fprintf(stderr,
                    "[vertex] VERIFY MISMATCH count=%u out=%d frame=%d "
                    "cpu=%d globals=%d\n",
                    count, out_ok, frame_ok, cpu_ok, globals_ok);
        }
        g_vertex_enabled = 0;
    }
    return ok;
}
#endif

static __attribute__((noinline)) int vertex_try_slow(CPU *c,
                                                     wc3_lifted_fn fn,
                                                     unsigned kind,
                                                     uint32_t count);

int wc3_parallel_vertex_try_impl(CPU *c, wc3_lifted_fn fn, unsigned kind)
{
    /* Hot early-out.  The inline wrapper has already rejected NULL and a
     * worker's private frame.  Keep this function small: the validation and
     * dispatch body lives in vertex_try_slow so a small batch does not pay a
     * large stack frame, register save or TLS lookup. */
    if (c->parallel_vertex_bypass) return 0;
    if (!g_vertex_enabled) return 0;
    if (kind > WC3_VERTEX_TRANSFORM) return 0;
    if (g_pool_init_active) return 0;

    uint32_t count = rd32(GVA(WC3_VERTEX_GLOBAL_FIRST));
    if (count < g_vertex_threshold || count > VERTEX_MAX_COUNT) return 0;
    return vertex_try_slow(c, fn, kind, count);
}

static __attribute__((noinline)) int vertex_try_slow(CPU *c,
                                                     wc3_lifted_fn fn,
                                                     unsigned kind,
                                                     uint32_t count)
{
#if WC3_DEBUG && WC3_PARALLEL_VERTEX_VALIDATE
    VertexOracle oracle;
    CPU oracle_before;
    uint8_t oracle_frame[VERTEX_FRAME_COPY];
    int oracle_ready = 0;
#endif

    /* ready() applies the environment kill switches and sets the final
     * thresholds.  Recheck the values it may have changed before validating. */
    if (!ready()) return 0;
    if (!g_vertex_enabled) return 0;
    if (count < g_vertex_threshold || count > VERTEX_MAX_COUNT) return 0;

#if WC3_DEBUG && WC3_PARALLEL_VERTEX_VALIDATE
    if (!vertex_frame_ok(c)) return 0;

    /* The setup block leaves the object pointer in [ebp-4] and the loop kind
     * in [ctx+0x224].  Check it so a future call-site change fails closed. */
    uint32_t obj = rd32(c->ebp - 4u);
    if (!obj || !hle_android_guest_ok(obj, 0x228u)) return 0;
    uint32_t mode = rd32(obj + 0x224u);
    if ((kind == WC3_VERTEX_COPY && mode != 0u) ||
        (kind == WC3_VERTEX_TRANSFORM && mode != 1u))
        return 0;
    if (kind == WC3_VERTEX_TRANSFORM && rd32(c->ebp - 0x18u) != 0u)
        return 0;
#endif

#if WC3_DEBUG && WC3_DEBUG_HLE
    {
        static unsigned trace;
        if (trace < 64u) {
            fprintf(stderr,
                    "[vertex] try kind=%u count=%u threshold=%u ebp=%08X esp=%08X\n",
                    kind, count, g_vertex_threshold, c->ebp, c->esp);
            trace++;
        }
    }
#endif

    uint32_t vg[WC3_VERTEX_GLOBAL_WORDS];
    for (unsigned i = 0; i < WC3_VERTEX_GLOBAL_WORDS; i++)
        vg[i] = rd32(GVA(WC3_VERTEX_GLOBAL_FIRST + i * 4u));

#if WC3_DEBUG && WC3_DEBUG_HLE
    {
        static unsigned trace;
        if (trace < 64u) {
            fprintf(stderr,
                    "[vertex] regs kind=%u count=%u ebx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X\n",
                    kind, count, c->ebx, c->esi, c->edi, c->ebp, c->esp);
            fprintf(stderr,
                    "[vertex] vg p0=%08X s0=%u p1=%08X s1=%u p2=%08X s2=%u p3=%08X s3=%u p4=%08X s4=%u pm=%08X sm=%u\n",
                    vg[1], vg[2], vg[3], vg[4], vg[5], vg[6],
                    vg[9], vg[11], vg[10], vg[12], vg[7], vg[8]);
            fprintf(stderr,
                    "[vertex] out ptr=[%08X,%08X,%08X,%08X] stride=[%u,%u,%u,%u]\n",
                    c->ebx, c->esi, c->edi, rd32(c->ebp - 8u),
                    rd32(c->ebp - 0x1Cu), rd32(c->ebp - 0x0Cu),
                    rd32(c->ebp - 0x14u), rd32(c->ebp - 0x10u));
            trace++;
        }
    }
#endif

#if WC3_DEBUG && WC3_PARALLEL_VERTEX_VALIDATE
    /* Source streams: p0 is a 12-byte position, p1..p4 are 8-byte
     * attribute vectors, and the transform path also reads one byte from the
     * matrix-index array pm per vertex. */
    VertexRange src[5];
    memset(src, 0, sizeof src);
    src[0].lo = vg[1];
    src[0].hi = (uint64_t)vg[1] +
                (uint64_t)(count - 1u) * vg[2] + 12u;
    src[0].stride = vg[2];
    src[0].size = 12u;
    src[0].used = 1;
    uint32_t src_ptr[4] = { vg[3], vg[5], vg[9], vg[10] };
    uint32_t src_stride[4] = { vg[4], vg[6], vg[11], vg[12] };
    for (unsigned i = 0; i < 4; i++) {
        if (!vertex_span_ok(src_ptr[i], src_stride[i], count, 8u))
            return 0;
        src[i + 1].lo = src_ptr[i];
        src[i + 1].hi = (uint64_t)src_ptr[i] +
                        (uint64_t)(count - 1u) * src_stride[i] + 8u;
        src[i + 1].stride = src_stride[i];
        src[i + 1].size = 8u;
        src[i + 1].used = 1;
    }
    if (!vertex_span_ok(vg[1], vg[2], count, 12u)) return 0;
    if (kind == WC3_VERTEX_TRANSFORM) {
        if (!vertex_span_ok(vg[7], vg[8], count, 1u)) return 0;
        src[4].lo = vg[7];
        src[4].hi = (uint64_t)vg[7] +
                    (uint64_t)(count - 1u) * vg[8] + 1u;
        src[4].stride = vg[8];
        src[4].size = 1u;
        src[4].used = 1;
    }

    uint32_t out_ptr[4] = {
        c->ebx, c->esi, c->edi, rd32(c->ebp - 8u)
    };
    uint32_t out_stride[4] = {
        rd32(c->ebp - 0x1Cu), rd32(c->ebp - 0x0Cu),
        rd32(c->ebp - 0x14u), rd32(c->ebp - 0x10u)
    };
    /* Position (EBX) writes 12 bytes, ESI/EDI write 8 each, and the optional
     * EDX stream writes 4. */
    uint32_t out_size[4] = { 12u, 8u, 8u, 4u };
    uint32_t frame_lo = c->ebp - VERTEX_FRAME_BELOW;
    uint32_t frame_hi = frame_lo + VERTEX_FRAME_COPY;
    VertexRange out[4];
    VertexFrameSkip skip[4];
    unsigned skip_count = 0;
    memset(out, 0, sizeof out);
    for (unsigned i = 0; i < 4; i++) {
        if (out_stride[i] == 0u) {
            /* The setup block uses an in-frame dummy word for an optional
             * attribute.  Every worker has its own copied dummy, so a
             * zero-stride stream is safe when it stays inside the frame. */
            uint64_t dummy_lo = out_ptr[i];
            uint64_t dummy_hi = dummy_lo + out_size[i];
            if (dummy_lo < frame_lo || dummy_hi > (uint64_t)frame_hi)
                return 0;
            if (skip_count >= 4u) return 0;
            /* The sequential run writes the caller's dummy; the parallel run
             * writes only a private copy.  Skip the dummy in the frame
             * comparison, but never let it alias observable merge state or a
             * source range. */
            if (dummy_lo < (uint64_t)c->ebp - 0x18u + 4u &&
                (uint64_t)c->ebp - 0x18u < dummy_hi)
                return 0;
            if (dummy_lo < (uint64_t)c->ebp - 8u + 4u &&
                (uint64_t)c->ebp - 8u < dummy_hi)
                return 0;
            if (dummy_lo < (uint64_t)c->ebp + 8u + 4u &&
                (uint64_t)c->ebp + 8u < dummy_hi)
                return 0;
            for (unsigned j = 0; j < 5u; j++) {
                if (src[j].used && dummy_lo < src[j].hi &&
                    src[j].lo < dummy_hi)
                    return 0;
            }
            skip[skip_count].off = (uint32_t)(dummy_lo - frame_lo);
            skip[skip_count].size = out_size[i];
            skip_count++;
            continue;
        }
        if (!vertex_span_ok(out_ptr[i], out_stride[i], count, out_size[i]))
            return 0;
        out[i].lo = out_ptr[i];
        out[i].hi = (uint64_t)out_ptr[i] +
                    (uint64_t)(count - 1u) * out_stride[i] + out_size[i];
        out[i].stride = out_stride[i];
        out[i].size = out_size[i];
        out[i].used = 1;
        if ((uint64_t)frame_lo < out[i].hi &&
            out[i].lo < (uint64_t)frame_hi)
            return 0;
    }
    for (unsigned i = 0; i < 4; i++) {
        if (!out[i].used) continue;
        for (unsigned j = i + 1u; j < 4; j++) {
            if (ranges_overlap(&out[i], &out[j])) return 0;
        }
        for (unsigned j = 0; j < 5; j++) {
            if (ranges_overlap(&out[i], &src[j])) return 0;
        }
    }

    vertex_oracle_init(&oracle, out, count, skip, skip_count);
    oracle_ready = 1;
#endif

    unsigned chunks = vertex_chunk_limit();
    if (!chunks) return 0;
    if (chunks > count) chunks = count;
    if (g_vertex_min_chunk) {
        unsigned by_count = count / g_vertex_min_chunk;
        if (chunks > by_count) chunks = by_count;
    }
    if (chunks < 2u) return 0;

#if WC3_DEBUG && WC3_DEBUG_HLE
    {
        static unsigned trace;
        if (trace < 16u) {
            fprintf(stderr, "[vertex] batch kind=%u count=%u chunks=%u\n",
                    kind, count, chunks);
            trace++;
        }
    }
#endif

#if WC3_DEBUG && WC3_PARALLEL_VERTEX_VALIDATE
    int oracle_run = oracle_ready && g_vertex_verify;
    if (oracle_run) {
        oracle_before = *c;
        memcpy(oracle_frame,
               guest_host_ptr(c->ebp - VERTEX_FRAME_BELOW),
               sizeof oracle_frame);
    }
#endif
#if WC3_DEBUG && WC3_DEBUG_PERF
    uint64_t perf_t0 = now_ns();
#endif
    int pooled = run_vertex_batch(c, fn, kind, count, chunks, vg);
#if WC3_DEBUG && WC3_DEBUG_PERF
    perf_note_kind(KIND_PERF_VERTEX, count, chunks, pooled,
                   now_ns() - perf_t0);
#endif
#if WC3_DEBUG && WC3_PARALLEL_VERTEX_VALIDATE
    if (pooled && oracle_run && pthread_mutex_trylock(&g_submit_lock) == 0) {
        /* Keep the pool idle while the oracle replays the sequential loop on
         * the caller.  A concurrent wrapper that loses this trylock remains
         * sequential, and t_ray_active blocks nested pool use here. */
        int saved_active = t_ray_active;
        t_ray_active = 1;
        (void)vertex_verify_after(c, fn, count, vg, &oracle_before,
                                  oracle_frame, &oracle);
        t_ray_active = saved_active;
        pthread_mutex_unlock(&g_submit_lock);
    }
#endif
    return pooled;
}

#if WC3_DEBUG && WC3_PARALLEL_TRIANGLE_VALIDATE
typedef struct TriangleState {
    uint32_t best;
    uint32_t index;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    XMM xmm[8];
    uint32_t cf, zf, sf, of, pf, af;
} TriangleState;

static void triangle_capture_cpu(const CPU *c, TriangleState *s)
{
    s->eax = c->eax;
    s->ecx = c->ecx;
    s->edx = c->edx;
    memcpy(s->xmm, c->xmm, sizeof s->xmm);
    s->cf = c->cf;
    s->zf = c->zf;
    s->sf = c->sf;
    s->of = c->of;
    s->pf = c->pf;
    s->af = c->af;
}

static void log_triangle_mismatch(const char *where, uint32_t total,
                                  unsigned chunks, const TriangleState *want,
                                  const TriangleState *got)
{
    static unsigned reports;
    if (reports >= 8u) return;
    reports++;

    fprintf(stderr,
            "[triangle] VERIFY MISMATCH %s total=%u chunks=%u "
            "best want=%08X got=%08X index want=%u got=%u "
            "eax want=%08X got=%08X ecx want=%08X got=%08X "
            "edx want=%08X got=%08X flags want=%u%u%u%u%u%u "
            "got=%u%u%u%u%u%u\n",
            where, total, chunks, want->best, got->best,
            want->index, got->index, want->eax, got->eax,
            want->ecx, got->ecx, want->edx, got->edx,
            want->cf, want->zf, want->sf, want->of, want->pf, want->af,
            got->cf, got->zf, got->sf, got->of, got->pf, got->af);

    for (unsigned i = 0; i < 8u; i++) {
        if (want->xmm[i].u64[0] == got->xmm[i].u64[0] &&
            want->xmm[i].u64[1] == got->xmm[i].u64[1])
            continue;
        fprintf(stderr,
                "[triangle]   xmm%u want=%016llX%016llX got=%016llX%016llX\n",
                i,
                (unsigned long long)want->xmm[i].u64[1],
                (unsigned long long)want->xmm[i].u64[0],
                (unsigned long long)got->xmm[i].u64[1],
                (unsigned long long)got->xmm[i].u64[0]);
    }
}
#endif

/* Dispatch a triangle batch.  All ranges were validated by the caller.  The
 * caller's frame, best word and index word are untouched until the merge
 * succeeds; a worker failure or a validation mismatch simply returns 0 so the
 * generated function runs the original loop on the caller.  On 1 the frame has
 * already been advanced exactly like the sequential epilogue. */
static int run_triangle_batch(CPU *c, wc3_lifted_fn fn, uint32_t total,
                              unsigned chunks, uint32_t indices,
                              uint32_t best_ptr, uint32_t index_ptr,
                              uint32_t orig_best)
{
    if (pthread_mutex_trylock(&g_submit_lock) != 0) return 0;
    if (t_ray_active || c->parallel_vertex_bypass) {
        pthread_mutex_unlock(&g_submit_lock);
        return 0;
    }
    t_ray_active = 1;

    RayJob job;
    job.kind = RAY_JOB_TRIANGLE;
    job.fn = fn;
    job.cpu = c;
    job_set_chunk_split(&job, total, chunks);
    job.triangle_indices = indices;
    job.triangle_best_ptr = best_ptr;
    job.triangle_index_ptr = index_ptr;
    job.triangle_orig_best = orig_best;

#if WC3_DEBUG && WC3_PARALLEL_TRIANGLE_VALIDATE
    /* The oracle writes the real best/index words, so snapshot them first.
     * The caller's frame is not touched: prepare_triangle_ctx copies it.
     * Range checks in triangle_try_slow are always on in a validation build;
     * only this full sequential replay is opt-in. */
    TriangleState expect;
    int verify = g_triangle_verify;
    memset(&expect, 0, sizeof expect);
    if (verify) {
        RayCtx *v = &g_verify_ctx;
        v->start = 0;
        v->count = total;
        if (!prepare_triangle_ctx(v, &job, 0)) {
            t_ray_active = 0;
            pthread_mutex_unlock(&g_submit_lock);
            return 0;
        }
        WC3_DIRECT_CALL_CLEAN(fn, &v->cpu);
        expect.best = rd32(best_ptr);
        expect.index = rd32(index_ptr);
        triangle_capture_cpu(&v->cpu, &expect);
        wr32(best_ptr, orig_best);
        wr32(index_ptr, 0);
    }
#endif

    uint64_t seq = publish_job((uintptr_t)&job, chunks);
    wake_workers(chunks);

    wait_for_workers(chunks, seq, tune_uint(&g_tune.main_spin_us));
    atomic_store_explicit(&g_job_addr, 0, memory_order_release);

    for (unsigned i = 0; i < chunks; i++) {
        if (g_worker[i].triangle_failed) {
            /* Every worker wrote only its private frame words, and the oracle
             * restored the real outputs.  The caller can run the original
             * loop from the guard with no repair. */
            t_ray_active = 0;
            pthread_mutex_unlock(&g_submit_lock);
            return 0;
        }
    }

    /* Chunks are contiguous and merged in increasing order.  The guest update
     * is `if (best > distance)`, so a strict float compare keeps the lowest
     * triangle index on a tie and leaves NaN untouched exactly like the
     * generated `comiss`/`jbe` sequence. */
    uint32_t merged_best = orig_best;
    uint32_t merged_index = 0;
    for (unsigned i = 0; i < chunks; i++) {
        uint32_t chunk_best = g_worker[i].triangle_best;
        if (bits_float(chunk_best) < bits_float(merged_best)) {
            uint32_t extra = i < job.chunk_rem ? i : job.chunk_rem;
            uint32_t begin = i * job.chunk_base + extra;
            merged_best = chunk_best;
            merged_index = begin + g_worker[i].triangle_index;
        }
    }

    RayCtx *last = &g_worker[chunks - 1u];

#if WC3_DEBUG && WC3_PARALLEL_TRIANGLE_VALIDATE
    /* Compare everything the chunks produce before the epilogue pops the
     * caller's frame.  XMM0 on a worker is its own chunk's best, so only
     * XMM1..7 and ECX/EDX are comparable here. */
    if (verify) {
        int ok = merged_best == expect.best && merged_index == expect.index &&
                 last->cpu.ecx == expect.ecx &&
                 last->cpu.edx == expect.edx;
        for (unsigned i = 1; ok && i < 8u; i++) {
            if (memcmp(&last->cpu.xmm[i], &expect.xmm[i], sizeof(XMM)) != 0)
                ok = 0;
        }
        if (!ok) {
            TriangleState got;
            memset(&got, 0, sizeof got);
            got.best = merged_best;
            got.index = merged_index;
            got.eax = 0;
            got.ecx = last->cpu.ecx;
            got.edx = last->cpu.edx;
            memcpy(got.xmm, last->cpu.xmm, sizeof got.xmm);
            log_triangle_mismatch("chunks", total, chunks, &expect, &got);
            g_triangle_enabled = 0;
            t_ray_active = 0;
            pthread_mutex_unlock(&g_submit_lock);
            return 0;
        }
    }
#endif

    /* Publish the merged words, then reproduce the original epilogue on the
     * caller.  [ebp+0x14] = 0 makes the loop skip and enter L_6F125999
     * directly: load the best distance, compare it with +inf, pop the saved
     * registers and return through `ret 0x1c`. */
    wr32(best_ptr, merged_best);
    wr32(index_ptr, merged_index);
    wr32(c->ebp + 0x14u, 0);
    uint32_t saved_bypass = c->parallel_triangle_bypass;
    c->parallel_triangle_bypass = 1;
    WC3_DIRECT_CALL_CLEAN(fn, c);
    c->parallel_triangle_bypass = saved_bypass;

    /* The epilogue reads XMM0 from the merged best but leaves XMM1..7, ECX and
     * EDX from before the call.  The sequential loop leaves the last chunk's
     * final iteration there, so copy it from the last worker. */
    for (unsigned i = 1; i < 8u; i++) c->xmm[i] = last->cpu.xmm[i];
    c->ecx = last->cpu.ecx;
    c->edx = last->cpu.edx;

#if WC3_DEBUG && WC3_PARALLEL_TRIANGLE_VALIDATE
    if (verify) {
        TriangleState got;
        memset(&got, 0, sizeof got);
        got.best = rd32(best_ptr);
        got.index = rd32(index_ptr);
        triangle_capture_cpu(c, &got);
        if (got.best != expect.best || got.index != expect.index ||
            got.eax != expect.eax || got.ecx != expect.ecx ||
            got.edx != expect.edx ||
            memcmp(got.xmm, expect.xmm, sizeof got.xmm) != 0 ||
            got.cf != expect.cf || got.zf != expect.zf ||
            got.sf != expect.sf || got.of != expect.of ||
            got.pf != expect.pf || got.af != expect.af) {
            log_triangle_mismatch("epilogue", total, chunks, &expect, &got);
            /* The frame is gone, so repair the words the epilogue owns.  The
             * merged best/index and the copied chunk state are already the
             * sequential values; only the flag test could differ. */
            c->eax = expect.eax;
            c->xmm[0] = expect.xmm[0];
            c->cf = expect.cf;
            c->zf = expect.zf;
            c->sf = expect.sf;
            c->of = expect.of;
            c->pf = expect.pf;
            c->af = expect.af;
            g_triangle_enabled = 0;
        }
    }
#endif

    t_ray_active = 0;
    pthread_mutex_unlock(&g_submit_lock);
    return 1;
}

static __attribute__((noinline)) int triangle_try_slow(CPU *c,
                                                       wc3_lifted_fn fn,
                                                       uint32_t raw_count,
                                                       uint32_t tri_count);

/* Cold half of the triangle wrapper: run pthread_once and retry.  Keeping it
 * out of line leaves the initialized hot path free of a stack frame. */
static __attribute__((noinline)) int triangle_init_cold(CPU *c,
                                                        wc3_lifted_fn fn)
{
    if (!ready()) return 0;
    return wc3_parallel_triangle_try_impl(c, fn);
}

int wc3_parallel_triangle_try_impl(CPU *c, wc3_lifted_fn fn)
{
    /* Hot early-out.  Keep the validation and dispatch body in a separate
     * noinline function so a small triangle batch pays only this check. */
    if (c->parallel_vertex_bypass) return 0;
    if (g_pool_init_active) return 0;
    if (!atomic_load_explicit(&g_init_ok, memory_order_acquire))
        return triangle_init_cold(c, fn);
    if (!g_parallel_ok || !g_triangle_enabled) return 0;

    uint32_t raw_count = rd32(c->ebp + 0x14u);
    if (!raw_count || raw_count > TRIANGLE_MAX_COUNT * 3u) return 0;

    /* L_6F125924 converts an index count to a triangle count with
     * ceil((count-1)/3).  Use the same formula so a chunk boundary always
     * lands on a triangle the original loop would have visited. */
    uint32_t tri_count = (raw_count - 1u) / 3u + 1u;
    if (tri_count < g_triangle_threshold || tri_count > TRIANGLE_MAX_COUNT)
        return 0;
    return triangle_try_slow(c, fn, raw_count, tri_count);
}

static __attribute__((noinline)) int triangle_try_slow(CPU *c,
                                                       wc3_lifted_fn fn,
                                                       uint32_t raw_count,
                                                       uint32_t tri_count)
{
    uint32_t indices = rd32(c->ebp + 0x18u);
    uint32_t vertex = rd32(c->ebp + 0x0Cu);
    uint32_t best_ptr = c->esi;
    uint32_t index_ptr = rd32(c->ebp + 0x20u);
    if (!best_ptr || !index_ptr) return 0;

#if WC3_DEBUG && WC3_PARALLEL_TRIANGLE_VALIDATE
    if (!hle_android_guest_ok(best_ptr, 4u) ||
        !hle_android_guest_ok(index_ptr, 4u))
        return 0;
    if (!triangle_frame_ok(c)) return 0;
    if (!triangle_index_span_ok(indices, tri_count)) return 0;
    if (!triangle_vertex_span_ok(vertex, indices, tri_count)) return 0;

    /* The workers write private words in their copied frames and the main
     * thread writes these two words only after the batch, so an output inside
     * the copied frame would be overwritten or alias caller state. */
    uint32_t frame_lo = c->ebp - TRIANGLE_FRAME_BELOW;
    uint32_t frame_hi = frame_lo + TRIANGLE_FRAME_COPY;
    if (best_ptr >= frame_lo && best_ptr + 4u <= frame_hi) return 0;
    if (index_ptr >= frame_lo && index_ptr + 4u <= frame_hi) return 0;
#endif

    unsigned chunks = triangle_chunk_limit();
    if (!chunks) return 0;
    if (chunks > tri_count) chunks = tri_count;
    if (g_triangle_min_chunk) {
        unsigned by_count = tri_count / g_triangle_min_chunk;
        if (chunks > by_count) chunks = by_count;
    }
    if (chunks < 2u) return 0;

#if WC3_DEBUG && WC3_DEBUG_HLE
    {
        static unsigned trace;
        if (trace < 16u) {
            fprintf(stderr,
                    "[triangle] batch indices=%u tris=%u chunks=%u "
                    "threshold=%u best=%08X\n",
                    raw_count, tri_count, chunks, g_triangle_threshold,
                    rd32(best_ptr));
            trace++;
        }
    }
#endif

#if WC3_DEBUG && WC3_DEBUG_PERF
    uint64_t perf_t0 = now_ns();
#endif
    int pooled = run_triangle_batch(c, fn, tri_count, chunks, indices,
                                    best_ptr, index_ptr, rd32(best_ptr));
#if WC3_DEBUG && WC3_DEBUG_PERF
    perf_note_kind(KIND_PERF_TRIANGLE, tri_count, chunks, pooled,
                   now_ns() - perf_t0);
#endif
    return pooled;
}

/* Dispatch one job and merge the results.  Returns 0 only when the pool could
 * not be used.  On success the merged output is in `merged`. */
/* Small raycast batches lose more in wakeups and frame copies than they gain
 * from splitting.  The tuning floor is 64 candidates per chunk; the split is
 * also capped by the batch size. */
static unsigned raycast_chunk_count(uint32_t total)
{
    unsigned limit = tune_uint(&g_tune.chunk_limit);
    if (limit == 0 || limit > g_workers) limit = g_workers;
    if (limit > total) limit = total;
    unsigned min_chunk = tune_uint(&g_tune.min_chunk);
    if (min_chunk) {
        unsigned by_count = total / min_chunk;
        if (limit > by_count) limit = by_count;
    }
    return limit;
}

static int run_batch(CPU *c, wc3_lifted_fn fn, uint32_t container,
                     uint32_t total, unsigned limit, uint32_t array,
                     uint32_t in1, uint32_t in2, uint32_t out_pos,
                     uint32_t out_dist, uint32_t user_ctx, RayCtx *merged)
{
    if (limit < 2u) return 0;
    if (pthread_mutex_trylock(&g_submit_lock) != 0) return 0;
    if (t_ray_active) {
        pthread_mutex_unlock(&g_submit_lock);
        return 0;
    }
    t_ray_active = 1;

#if WC3_DEBUG && WC3_DEBUG_HLE
    {
        static unsigned trace;
        if (trace < 16u) {
            fprintf(stderr, "[raycast] batch total=%u chunks=%u\n",
                    total, limit);
            trace++;
        }
    }
#endif

    RayJob job;
    job.kind = RAY_JOB_RAYCAST;
    job.fn = fn;
    job.cpu = c;
    job.container = container;
    job.array = array;
    job.in1 = in1;
    job.in2 = in2;
    job.user_ctx = user_ctx;
    job_set_chunk_split(&job, total, limit);
    job.stack_zero = tune_uint(&g_tune.stack_zero);
    job.container_copy = tune_uint(&g_tune.container_copy);

    uint64_t seq = publish_job((uintptr_t)&job, limit);
    /* Wake only the active chunks.  Inactive workers stay asleep and never
     * touch the stack RayJob. */
    wake_workers(limit);

    wait_for_workers(limit, seq, tune_uint(&g_tune.main_spin_us));
    atomic_store_explicit(&g_job_addr, 0, memory_order_release);

#if WC3_DEBUG && WC3_DEBUG_PERF
    g_last_chunks = limit;
    g_last_total = total;
#endif
    merge_results(merged, limit);

#if WC3_DEBUG && WC3_DEBUG_HLE
    if (tune_int(&g_tune.verify)) {
        RayCtx seq;
        run_verify(c, fn, container, in1, in2, out_pos, out_dist, user_ctx,
                   &seq);
        g_verify_calls++;
        if (seq.hit != merged->hit || seq.dist_bits != merged->dist_bits ||
            (seq.hit &&
             (seq.pos[0] != merged->pos[0] ||
              seq.pos[1] != merged->pos[1] ||
              seq.pos[2] != merged->pos[2]))) {
            g_verify_failures++;
            log_mismatch(c, container, total, merged, &seq);
            /* The sequential result is the definition of correct.  Stop using
             * the pool for the rest of the process rather than risk another
             * divergence. */
            g_parallel_ok = 0;
            *merged = seq;
        }
    }
#endif

    t_ray_active = 0;
    pthread_mutex_unlock(&g_submit_lock);
    return 1;
}

/* Keep the fallback out of line.  In a release build this function is tiny
 * enough that the compiler otherwise inlines the whole register-save/call
 * sequence into wc3_parallel_raycast, giving the hot wrapper a stack frame and
 * three callee-saved spills on every small raycast.  A tail call keeps the
 * wrapper frameless; the direct call pays its own prologue only when it runs. */
static __attribute__((noinline)) void call_direct(CPU *c, wc3_lifted_fn fn)
{
#if WC3_DEBUG && WC3_DEBUG_PERF
    if (tune_int(&g_tune.timing)) {
        uint64_t t0 = now_ns();
        WC3_DIRECT_CALL_CLEAN(fn, c);
        g_fallback_ns += now_ns() - t0;
    } else
#endif
    {
        WC3_DIRECT_CALL_CLEAN(fn, c);
    }
#if WC3_DEBUG && WC3_DEBUG_PERF
    g_fallback_calls++;
    maybe_print_stats();
#endif
}

static __attribute__((noinline)) void raycast_try_slow(CPU *c,
                                                       wc3_lifted_fn fn,
                                                       uint32_t container,
                                                       uint32_t total)
{
    unsigned ray_chunks = raycast_chunk_count(total);
    if (ray_chunks < 2u) {
        call_direct(c, fn);
        return;
    }

#if WC3_DEBUG && WC3_DEBUG_PERF
    uint64_t t0 = tune_int(&g_tune.timing) ? now_ns() : 0;
#endif

    /* The call site pushes the five arguments before L_6F78E790 and leaves
     * them in place for this call; [esp] is the artificial return address. */
    uint32_t sp = c->esp;
    uint32_t in1 = rd32(sp + 4u);
    uint32_t in2 = rd32(sp + 8u);
    uint32_t out_pos = rd32(sp + 12u);
    uint32_t out_dist = rd32(sp + 16u);
    uint32_t user_ctx = rd32(sp + 20u);
    uint32_t array = rd32(container + 0x114u);
    uint64_t array_bytes = (uint64_t)total * 4u;
#if WC3_DEBUG && WC3_DEBUG_HLE
    {
        static unsigned trace;
        if (trace < 64u) {
            fprintf(stderr,
                    "[raycast] try total=%u chunks=%u array=%08X container=%08X\n",
                    total, ray_chunks, array, container);
            trace++;
        }
    }
#endif
    if (array == 0 || array_bytes > 0xFFFFFFFFu ||
        !hle_android_guest_ok(container, RAY_CONTAINER_COPY) ||
        !hle_android_guest_ok(array, (uint32_t)array_bytes)) {
        call_direct(c, fn);
        return;
    }

    RayCtx merged;
    if (!run_batch(c, fn, container, total, ray_chunks, array, in1, in2,
                   out_pos, out_dist, user_ctx, &merged)) {
        call_direct(c, fn);
        return;
    }

#if WC3_DEBUG && WC3_DEBUG_PERF
    if (t0) g_parallel_ns += now_ns() - t0;
    g_parallel_calls++;
#endif

    /* A no-hit call must not write either output buffer.  The sequential
     * function leaves the caller's 0x7F800000 / zero initialization intact;
     * only a hit writes a distance and a position. */
    if (merged.hit) {
        if (out_dist) wr32(out_dist, merged.dist_bits);
        if (out_pos) {
            wr32(out_pos + 0u, merged.pos[0]);
            wr32(out_pos + 4u, merged.pos[1]);
            wr32(out_pos + 8u, merged.pos[2]);
        }
    }

    c->eax = merged.hit ? 1u : 0u;
    /* Emulate the callee's `ret 0x14`: pop the artificial return address and
     * its five arguments.  The caller's registers are otherwise untouched. */
    c->esp = sp + 24u;

#if WC3_DEBUG && WC3_DEBUG_PERF
    maybe_print_stats();
#endif
}

/* Cold half of the raycast wrapper: run pthread_once and retry.  A separate
 * noinline function keeps the initialized hot path frameless. */
static __attribute__((noinline)) void raycast_init_cold(CPU *c,
                                                        wc3_lifted_fn fn)
{
    if (g_pool_init_active) {
        call_direct(c, fn);
        return;
    }
    if (ready())
        wc3_parallel_raycast(c, fn);
    else
        call_direct(c, fn);
}

#if WC3_DEBUG && WC3_DEBUG_HLE
/* Tuning reload is rare (once per 1024 calls) and may touch the filesystem.
 * Run it out of line, then retry with the hot path. */
static __attribute__((noinline)) void raycast_reload_cold(CPU *c,
                                                          wc3_lifted_fn fn)
{
    maybe_reload_tuning();
    wc3_parallel_raycast(c, fn);
}
#endif

void wc3_parallel_raycast(CPU *c, wc3_lifted_fn fn)
{
    if (!c || !fn) return;

    uint32_t container = c->ecx;

#if WC3_DEBUG && (WC3_DEBUG_HLE || WC3_DEBUG_PERF)
    g_calls++;
#endif

    if (!atomic_load_explicit(&g_init_ok, memory_order_acquire)) {
        raycast_init_cold(c, fn);
        return;
    }
    if (!g_parallel_ok) {
        call_direct(c, fn);
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_HLE
    if ((g_calls & RAY_TUNE_CHECK_MASK) == 0) {
        raycast_reload_cold(c, fn);
        return;
    }
#endif

    if (!g_raycast_enabled || !tune_int(&g_tune.enabled) || container == 0) {
        call_direct(c, fn);
        return;
    }

    /* The count threshold is the hot early-out.  Do not touch the five stack
     * arguments, validate ranges or build a job until this call is actually
     * going to use the pool. */
    uint32_t total = rd32(container + 0x110u);
#if WC3_DEBUG && WC3_DEBUG_PERF
    ray_note_total(total);
#endif
    unsigned threshold = tune_uint(&g_tune.threshold);
    if (total < threshold) {
        call_direct(c, fn);
        return;
    }

    raycast_try_slow(c, fn, container, total);
}
