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
 * source of truth and runs it on private CPU contexts over disjoint contiguous
 * ranges of top-level candidates.  Each range gets its own container copy
 * (the object loop itself is untouched), its own guest stack and TEB, and its
 * own output words.  The caller executes the last range in that range's private
 * context and the pool workers execute the preceding ranges.  The caller
 * remains inside this wrapper for the whole batch, so the shared reads cannot
 * race with a game-state writer.
 *
 * Merge rules are the original ones: ranges are visited in increasing order,
 * a hit is taken when it is the first hit, and a later distance only replaces
 * it when it is strictly smaller.  That keeps the first candidate winning a
 * distance tie.
 *
 * Workers that ran the preceding job park on one shared futex, so a batch with
 * the same or fewer chunks needs one wakeup instead of one sem_post per
 * worker.  A growing batch sem_posts only the newly active workers.  Workers
 * outside the active set never read the stack RayJob.  The context used by the
 * caller for its last chunk is always outside that active set.
 *
 * In a debug HLE/PERF build the layout and split can be measured without
 * changing the product: `WC3_MAIN_LAST=0` restores the all-worker layout,
 * `WC3_RAY_SPLIT_CEIL`, `WC3_VERTEX_SPLIT_CEIL` and
 * `WC3_TRIANGLE_SPLIT_CEIL` enable ceil split per path,
 * `WC3_WEIGHTED_SPLIT=1` uses measured per-core chunk timing for contiguous
 * weighted ranges, `WC3_REJECT_SLOW=1` drops a core measured over four times
 * slower than the best, and `WC3_MAIN_SPIN_ADAPTIVE=1` adjusts the caller's
 * completion-spin budget.  `WC3_WORKER_ORDER_REVERSE=1` reverses the product
 * fastest-to-slowest list to reproduce the old slowest-first active prefix.
 * A rejected trailing chunk leaves the active set, so
 * it is not woken or waited for; merge skips every zero-length chunk.
 * `WC3_PERF_NOTES=0` removes the per-call PERF counters from an A/B run.
 * Release always runs the main-last layout, equal floor split and fixed spin
 * budget; the last caller-only context does not create a worker thread.
 *
 * The dispatcher has an optional diagnostic text file:
 *
 *   <WC3_DATA_DIR>/raycast.cfg        (Android: app-private files directory)
 *   ./raycast.cfg                     (when WC3_DATA_DIR is unset)
 *
 * The file is opt-in: set WC3_RAY_TUNE=1 to load it.  Without that variable
 * the built-in automatic layout is used.  This matters because an old
 * diagnostic file must not silently pin `chunks` or `worker_cpu_mask` after
 * the code switches back to one worker per allowed CPU.  The product core
 * table is a separate input: WC3_PARALLEL_WORKER_CPUS sets both the pool size
 * and a one-core affinity per worker.
 *
 * Format: one or more `key=value` pairs separated by whitespace.  `#` starts
 * a comment.  Values can be changed by editing the file through `run-as` or
 * by pushing a new file; the next reload applies them without restarting the
 * process.  The file is checked every RAY_TUNE_CHECK_MASK+1 raycast calls.
 *
 * Keys:
 *   enabled=0|1             soft disable; keeps the pool alive
 *   threshold=auto|N        minimum top-level candidates for the pool;
 *                           `auto` is max(2*min_chunk, chunks*2)
 *   chunks=auto|N           chunk count for a batch (1..pool size); the
 *                           caller runs the last chunk, so the pool wakes
 *                           chunks-1 workers.  `auto` uses min(pool size,
 *                           candidate count)
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
#include "parallel_phase.h"

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

/* The measured layout (main-last, weighted split, per-chunk timing, spin
 * budget) is one debug facility.  PARALLEL is a superset of HLE/PERF for it:
 * the live tuner needs the same switches even in a build without those tags,
 * and release still compiles none of it. */
#if WC3_DEBUG && (WC3_DEBUG_HLE || WC3_DEBUG_PERF || WC3_DEBUG_PARALLEL)
# define WC3_PAR_LAYOUT_DEBUG 1
#else
# define WC3_PAR_LAYOUT_DEBUG 0
#endif

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
/* Each worker pays a fixed cost before it touches a candidate: a CPU copy,
 * a 336-byte container copy, a 256-byte stack clear and a wakeup.  The fixed
 * bytes are about 1.3 KiB; the candidate chain is much larger.  The loaded
 * path measured on 2026-11-20 had 2048 samples in the 32..63 bucket
 * (avg 45, max 48), so it runs three chunks of 11..16 candidates.  A
 * 16-candidate batch would be two chunks of eight; the 16..31 bucket was not
 * present in that sample, so its break-even is still unproven.  Keep the
 * eight-candidate floor and the auto threshold
 * max(2*min_chunk, 2*workers) = 16 for eight workers.  The older 24/64 notes
 * and the 64/256 numbers in docs/research/PARALLEL_CPU_LAYOUT_2026-11-17.md
 * came from a different shadow candidate cost and are not the as-built
 * default.  The 453 us batch average for 44 candidates/3 chunks is about
 * 30 us per candidate, so even an eight-candidate chunk has roughly 240 us
 * of candidate work; a 50 us wake would still leave it ahead of the two
 * sequential chunks.  That is a model, not an A/B: the 16..31 bucket was not
 * observed and must still be measured before changing the floor. */
#define RAY_MIN_CHUNK            24u
#define RAY_AUTO_THRESHOLD_CHUNK 8u
#define RAY_MAX_THRESHOLD        1000000u
#define RAY_SENTINEL             0x6F78D9F2u
#define RAY_TUNE_PATH_MAX        1024u
#define RAY_TUNE_CHECK_MASK      0x3FFu
#define RAY_STATS_DEFAULT        8192u
#define RAY_STATS_EVERY_MAX      1000000000u
/* The product CPU table and the kernel affinity syscall both use a 64-bit
 * mask.  Keep the worker list in the same range. */
#define WC3_PARALLEL_MAX_WORKERS 64u

/* Linux/Android futex opcodes.  Keeping the numbers local avoids a libc
 * header dependency in this HLE translation unit. */
#define WC3_FUTEX_WAIT_PRIVATE   128
#define WC3_FUTEX_WAKE_PRIVATE   129

/* Vertex chunks reuse the same worker contexts, guest stacks and TEBs.  A
 * batch uses one chunk per pool worker at most, and the pool itself has one
 * worker per allowed CPU or per selected product core.  The default split
 * requires enough vertices per worker to amortize a wakeup and a private
 * frame copy; tiny batches stay on the caller.  WC3_VERTEX_CHUNKS and
 * WC3_VERTEX_MIN_CHUNK can tune that for diagnostics. */
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
 * pair.  The observed game call site passes 96 indices (32 triangles).  That
 * batch is below the profitable split: a 16-triangle chunk is almost all
 * frame-copy and wakeup cost, and the outer raycast batch already parallelizes
 * the candidates that contain these triangles.  Keep the 128/32 defaults so
 * the small batch stays sequential.  Lowering the gate would not engage the
 * pool for this call: the outer raycast job holds g_submit_lock, so
 * run_triangle_batch fails the trylock and returns to the sequential loop.
 *
 * The caller executes the last chunk in that chunk's private context, so
 * a split wakes only chunks-1 workers.  A two-chunk split still pays one wake
 * and one copied frame: caller participation removes the second worker, not
 * the wake the remaining worker still pays.  The copies are about 1 KiB; they
 * cannot by themselves explain a 32-triangle loss, but the split still has a
 * wake that the sequential path does not.  A debug-only
 * one-shot probe (`[triangle] ab`) measures this exact batch sequentially and
 * over the pool with 16-triangle chunks, so the gate can be checked on device
 * before it is changed.  The probe does not alter the normal path.  The only
 * lever that removes the wake inside a split is keeping workers hot with
 * WC3_RAY_SPIN_US, which trades idle CPU and needs an on-device A/B.
 * WC3_TRIANGLE_THRESHOLD and WC3_TRIANGLE_MIN_CHUNK can still lower the gates
 * for a measured diagnostic experiment; do not treat 32/16 as a production
 * default. */
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
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
# ifndef WC3_TRIANGLE_VERIFY_DEFAULT
#  define WC3_TRIANGLE_VERIFY_DEFAULT 0
# endif
/* The A/B probe suppresses the shared oracle for its nested call.  The
 * parameter exists only in a validation build, so release has no debug-only
 * argument in the dispatcher signature. */
# define WC3_TRIANGLE_VERIFY_PARAM , int allow_verify
# define WC3_TRIANGLE_VERIFY_ARG(v) , (v)
#else
# define WC3_TRIANGLE_VERIFY_PARAM
# define WC3_TRIANGLE_VERIFY_ARG(v)
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
#if WC3_PAR_LAYOUT_DEBUG
    _Atomic int verify;
    _Atomic int stats;
    _Atomic int timing;
    _Atomic unsigned stats_every;
#endif
    _Atomic unsigned spin_us;
    _Atomic unsigned main_spin_us;
    _Atomic unsigned worker_cpu_mask;
} RayTuning;

#if WC3_DEBUG && WC3_DEBUG_PARALLEL
/* ------------------------------------------------------------------ */
/* Debug-only SPMD phase runtime.                                      */
/*                                                                    */
/* A phase is one fork-join over a captured batch of independent       */
/* L_6F749F90 requests.  The product path is not enabled by default:   */
/* the shadow collector below captures real requests, replays them    */
/* sequentially and through the pool, and compares the bits.  Release  */
/* compiles none of this.                                              */
/*                                                                    */
/* Each request owns a private guest container slot holding the bytes the */
/* function reads.  The collect half fills it once; both the parallel and */
/* sequential replays read it, so later game writes cannot change a     */
/* captured request.                                                    */
/* ------------------------------------------------------------------ */

typedef struct Wc3PhaseJob {
    Wc3PhaseRequest *req;
    const CPU *cpu;              /* common base when req_cpu is NULL */
    const CPU *req_cpu;          /* optional per-request base snapshot */
    uint32_t total;
    uint32_t chunks;
    uint32_t chunk_start[WC3_PARALLEL_MAX_WORKERS];
    uint32_t chunk_len[WC3_PARALLEL_MAX_WORKERS];
    unsigned active;
    int main_last;
    wc3_lifted_fn fn;
    unsigned stack_zero;
    int failed;
} Wc3PhaseJob;

typedef struct Wc3PhaseState {
    Wc3PhaseRequest req[WC3_PHASE_MAX_REQUESTS];
    Wc3PhaseResult seq_result[WC3_PHASE_MAX_REQUESTS];
    uint32_t count;
    uint32_t reserved;
    int enabled;
    int shadow_enabled;
    int shadow_busy;
    int main_last;
    int weighted;
    int ran;
    wc3_lifted_fn fn;
    unsigned max_requests;
    unsigned chunks;
    unsigned stack_zero;
    unsigned spin_us;
    unsigned threshold;
    unsigned long long captures;
    unsigned long long applies;
    unsigned long long flushes;
    unsigned long long mismatches;
    unsigned long long seq_ns_sum;
    unsigned long long par_ns_sum;
    uint64_t last_seq_ns;
    uint64_t last_par_ns;
    uint64_t last_wake_ns;
    uint64_t last_join_ns;
    uint64_t last_run_ns;
    uint64_t last_reset_ns;
    uint64_t last_max_slice_ns;
} Wc3PhaseState;

/* Caller continuation snapshot.  Level 0 is the innermost raycast and lives in
 * g_phase_cpu; levels 1..3 are the caller frames saved by the collect chain.
 * The frame is the live guest range [esp, ebp + 8) at the callee's post-call
 * point: saved registers, locals, saved ebp and the return address.  The
 * caller's own arguments live above ebp + 8 in the next outer frame, which is
 * restored by that frame's level before this postfix runs. */
typedef struct Wc3PhaseLevel {
    CPU cpu;
    uint32_t frame_va;
    uint32_t frame_len;
    uint8_t frame[WC3_PHASE_FRAME_MAX];
} Wc3PhaseLevel;

static Wc3PhaseState g_phase;
static CPU g_phase_cpu[WC3_PHASE_MAX_REQUESTS];
static Wc3PhaseLevel g_phase_level[WC3_PHASE_MAX_REQUESTS]
                                 [WC3_PHASE_MAX_LEVELS];
static uint8_t g_phase_level_count[WC3_PHASE_MAX_REQUESTS];
/* Minimal phase mailbox: one 64-bit word with a 32-bit sequence and a
 * 32-bit done mask.  The main publishes a new sequence with a release
 * store; each active worker sets its bit with a release RMW.  The job
 * parameters live in a static slot that the worker reads after the
 * acquire load, so the publish itself is one store. */
#define WC3_PHASE_MAILBOX_MAX_WORKERS 32u
static _Atomic uint64_t g_phase_mailbox;
static const Wc3PhaseJob *g_phase_job;
static uint32_t g_phase_seq;
static int g_phase_pool_ready;
static _Thread_local int t_phase_armed;
/* Set while wc3_phase_apply_all() is running on this thread.  A loop boundary
 * reached from an apply postfix must not run a nested apply: the outer drain
 * owns the window and the nested boundary would reset it under the outer
 * loop.  The nested boundary becomes a no-op and the new requests are drained
 * by the outer apply_all after the postfix returns. */
static _Thread_local int t_phase_apply_active;
#endif

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
     * The equal split uses chunk_base/chunk_rem; the measured weighted split
     * fills chunk_start/chunk_len directly.  Both representations keep the
     * ranges contiguous and ordered. */
    uint32_t chunk_base;
    uint32_t chunk_rem;
    uint32_t chunk_start[WC3_PARALLEL_MAX_WORKERS];
    uint32_t chunk_len[WC3_PARALLEL_MAX_WORKERS];
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
 * balanced: the first chunk_rem chunks carry one extra item.  The pool is
 * ordered fastest-to-slowest with the caller's core last, so the extra item
 * lands on a faster core and the slowest active core gets the base size. */
static inline void job_set_chunk_split(RayJob *job, uint32_t total,
                                       unsigned chunks)
{
    job->chunk_base = total / chunks;
    job->chunk_rem = total % chunks;
    for (unsigned i = 0; i < chunks; i++) {
        uint32_t extra = i < job->chunk_rem ? i : job->chunk_rem;
        job->chunk_start[i] = i * job->chunk_base + extra;
        job->chunk_len[i] = job->chunk_base + (i < job->chunk_rem ? 1u : 0u);
    }
}

/* Measured weighted split.  Weights are relative speed units: the fastest
 * selected core gets the largest share.  Largest-remainder allocation keeps
 * the sizes integral and contiguous.  This is diagnostic-only; release keeps
 * the equal split. */
#if WC3_PAR_LAYOUT_DEBUG
static void job_set_weighted_split(RayJob *job, uint32_t total,
                                   unsigned chunks, const uint64_t *weights)
{
    uint64_t sum = 0;
    uint64_t assigned = 0;
    uint64_t frac[WC3_PARALLEL_MAX_WORKERS];

    for (unsigned i = 0; i < chunks; i++) {
        sum += weights[i];
    }
    if (!sum) {
        /* All weights were rejected or unmeasured: fall back to equal. */
        for (unsigned i = 0; i < chunks; i++) {
            job->chunk_len[i] = (uint32_t)(total / chunks);
            assigned += job->chunk_len[i];
        }
        for (unsigned j = 0; j < total % chunks; j++) {
            job->chunk_len[j]++;
            assigned++;
        }
    } else {
        for (unsigned i = 0; i < chunks; i++) {
            uint64_t w = weights[i];
            if (!w) {
                job->chunk_len[i] = 0;
                frac[i] = 0;
                continue;
            }
            uint64_t raw = (uint64_t)total * w / sum;
            if (raw > total) raw = total;
            job->chunk_len[i] = (uint32_t)raw;
            assigned += raw;
            frac[i] = (uint64_t)total * w % sum;
        }
        /* Distribute the rounding remainder by largest fractional part among
         * chunks that are allowed to receive work. */
        while (assigned < total) {
            unsigned best = 0;
            int have = 0;
            for (unsigned i = 0; i < chunks; i++) {
                if (!weights[i]) continue;
                if (!have || frac[i] > frac[best]) {
                    best = i;
                    have = 1;
                }
            }
            if (!have || frac[best] == 0) {
                for (unsigned i = 0; i < chunks && assigned < total; i++) {
                    if (!weights[i]) continue;
                    job->chunk_len[i]++;
                    assigned++;
                }
                break;
            }
            job->chunk_len[best]++;
            assigned++;
            frac[best] = 0;
        }
    }

    /* The last chunk owns the vertex/triangle caller-frame epilogue.  It must
     * execute at least one item whenever the batch is non-empty, otherwise
     * its XMM state would be stale.  Move one item from an earlier chunk; a
     * rejected slow core is never allowed to starve the epilogue chunk. */
    if (total && job->chunk_len[chunks - 1u] == 0) {
        for (unsigned i = chunks - 1u; i-- > 0;) {
            if (job->chunk_len[i] > 1u) {
                job->chunk_len[i]--;
                job->chunk_len[chunks - 1u]++;
                break;
            }
        }
        if (job->chunk_len[chunks - 1u] == 0) {
            for (unsigned i = 0; i + 1u < chunks; i++) {
                if (job->chunk_len[i]) {
                    job->chunk_len[i]--;
                    job->chunk_len[chunks - 1u]++;
                    break;
                }
            }
        }
    }

    uint32_t start = 0;
    for (unsigned i = 0; i < chunks; i++) {
        job->chunk_start[i] = start;
        start += job->chunk_len[i];
    }
    /* Keep the equal-split fields consistent for callers that still use them
     * in diagnostics or merge fallbacks. */
    job->chunk_base = total / chunks;
    job->chunk_rem = total % chunks;
}
#endif

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

    /* Triangle jobs keep the chunk context's private best bits and local
     * triangle index.  The private words live in the copied frame at the
     * offsets above so the guest loop can write them through ordinary
     * pointers. */
    uint32_t triangle_best;
    uint32_t triangle_index;
    uint32_t triangle_best_addr;
    uint32_t triangle_index_addr;
    int triangle_failed;

#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    /* Phase slice state.  phase_failed is published with done_seq; the main
     * thread reads it only after the worker's completion store. */
    int phase_failed;
    uint64_t phase_slice_ns;
    uint32_t phase_count;
    /* Acknowledgement of the mailbox sequence.  The worker stores it only
     * after it has consumed the published job pointer (or decided it is
     * inactive), so the publisher can prove that no worker still needs the
     * job frame before it returns. */
    _Atomic uint32_t phase_ack;
#endif

    int index;
    /* Set after this worker runs an active chunk.  The next wait then uses
     * the shared futex; workers outside the active set stay on their own
     * semaphore.  This is per-worker state so a caller publishing the next
     * active count cannot race the queue choice. */
    int wait_shared;
    int thread_started;
    /* Product core selection: >=0 pins this worker to that CPU.  -1 keeps
     * the diagnostic worker_cpu_mask path. */
    int assigned_cpu;
    unsigned long long applied_cpu_mask;
#if WC3_PAR_LAYOUT_DEBUG
    /* Measured chunk timing.  The worker writes its own context after the
     * chunk; the caller reads it only after done_seq publishes the result.
     * These fields feed the debug weighted-split policy, never the product
     * equal split. */
    uint64_t chunk_ns;
    uint32_t chunk_count;
    uint64_t ns_per_item;
    /* Time spent inside the lifted call only.  The weighted split uses this
     * instead of the whole-chunk time so the per-chunk CPU copy, container
     * copy and stack clear do not look like candidate work. */
    uint64_t call_ns;
#endif
    uint64_t seen_seq;
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    /* Minimal phase mailbox: last sequence this worker consumed. */
    uint32_t phase_seen;
#endif
    /* Published by the worker after it finishes the job sequence.  The caller
     * scans only the active workers; no shared locked counter is needed. */
    _Atomic uint64_t done_seq;
    pthread_t thread;
    sem_t wake;
    int wake_init;
} RayCtx;

#if WC3_DEBUG && WC3_DEBUG_PARALLEL
/* Sequential fallback context for the phase window.  RayCtx is defined below
 * the early phase state block, so these two live here. */
static RayCtx g_phase_seq_ctx;
static int g_phase_seq_ctx_ready;
/* Dedicated phase worker pool.  The phase communicates only through
 * g_phase_mailbox; it does not use the generic g_job_seq/g_job_addr path. */
static RayCtx *g_phase_ctx;
static pthread_t *g_phase_thread;
static unsigned g_phase_thread_count;
static _Atomic int g_phase_shutdown;
/* One private guest container per request slot.  The collect half copies the
 * live container into its slot once; workers and the sequential oracle read
 * that slot directly.  This removes the per-run 0x150-byte copy into a
 * worker scratch while keeping the original container untouched. */
#define WC3_PHASE_SLOT_STRIDE  WC3_PHASE_CONTAINER_SNAP
static uint32_t g_phase_container_base;
static int g_phase_container_ready;

static inline uint32_t phase_container_slot(uint32_t index)
{
    return g_phase_container_base + index * WC3_PHASE_SLOT_STRIDE;
}
#endif

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
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_VERTEX_VALIDATE
/* Run the original full vertex loop after a pooled batch and compare its
 * observable bytes/CPU state with the parallel result.  Debug-only and
 * opt-in through WC3_VERTEX_VERIFY=1. */
static int g_vertex_verify;
#endif
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_VERTEX_VALIDATE
/* Per-kind oracle accounting.  The aggregate perf line cannot prove that
 * both entry points (copy and transform) were checked. */
static unsigned long long g_vertex_verify_calls[2];
static unsigned long long g_vertex_verify_failures[2];
#endif
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
/* Debug-only opt-in for the triangle sequential oracle.  Range checks stay
 * enabled; only the expensive whole-batch replay is gated. */
static int g_triangle_verify;
#endif
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
static unsigned long long g_triangle_verify_calls;
static unsigned long long g_triangle_verify_failures;

/* One-shot A/B probe for a small triangle batch.  It compares the generated
 * sequential loop with the same loop split over the pool, without touching the
 * caller's CPU, frame or output words.  The normal path stays sequential; this
 * only supplies the numbers needed to decide whether the 128/32 gate is still
 * right.  Three samples make a single short run comparable instead of relying
 * on one noisy first-batch measurement. */
#define TRIANGLE_AB_MIN_CHUNK 16u
#define TRIANGLE_AB_SAMPLES   3u
static RayCtx g_triangle_ab_ctx;
static int g_triangle_ab_ctx_ready;
static int g_triangle_ab_enabled = 1;
static _Atomic int g_triangle_ab_state;        /* main A/B: 0 idle, 1 running, 2 done */
static _Atomic int g_triangle_ab_nested_state; /* nested:  0 idle, 1 running, 2 done */
static _Atomic int g_triangle_ab_ctx_busy;
#endif
/* Live-tunable path gates.  The debug tuner rewrites them while the hot path
 * reads a relaxed value; release keeps the constants and never sees a store. */
static _Atomic unsigned g_vertex_threshold = VERTEX_DEFAULT_THRESHOLD;
static _Atomic unsigned g_vertex_chunks;      /* 0 = one chunk per worker */
static _Atomic unsigned g_vertex_min_chunk = VERTEX_DEFAULT_MIN_CHUNK;
static _Atomic unsigned g_triangle_threshold = TRIANGLE_DEFAULT_THRESHOLD;
static _Atomic unsigned g_triangle_chunks;    /* 0 = one chunk per worker */
static _Atomic unsigned g_triangle_min_chunk = TRIANGLE_MIN_CHUNK;
static inline unsigned par_gate_u(const _Atomic unsigned *p)
{
    return atomic_load_explicit(p, memory_order_relaxed);
}
static inline void par_gate_set_u(_Atomic unsigned *p, unsigned v)
{
    atomic_store_explicit(p, v, memory_order_relaxed);
}
static unsigned g_workers;
static RayCtx *g_worker;

#if WC3_DEBUG && WC3_DEBUG_PARALLEL
/* Live phase configuration.  The tuner thread only writes these atomics; the
 * guest thread copies them into the plain g_phase fields at a safe boundary
 * (empty window).  A split is therefore never changed while a phase job is in
 * flight, and the hot path has no cross-thread plain-data race. */
#define WC3_PHASE_AUTO_CHUNKS 4u

typedef struct Wc3PhaseConfig {
    _Atomic int enabled;
    _Atomic int shadow;
    _Atomic int main_last;
    _Atomic int weighted;
    _Atomic unsigned chunks;       /* 0 = auto: min(workers, 4) */
    _Atomic unsigned max_requests; /* shadow capture window */
    _Atomic unsigned threshold;    /* minimum requests for a fork-join */
    _Atomic unsigned stack_zero;
    _Atomic unsigned spin_us;
    _Atomic int dirty;
} Wc3PhaseConfig;

static Wc3PhaseConfig g_phase_cfg;

static unsigned phase_clamp_u(unsigned v, unsigned lo, unsigned hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void phase_live_touch(void)
{
    atomic_store_explicit(&g_phase_cfg.dirty, 1, memory_order_release);
}

static void phase_live_set_enabled(int on)
{
    atomic_store_explicit(&g_phase_cfg.enabled, on ? 1 : 0,
                          memory_order_relaxed);
    phase_live_touch();
}

static void phase_live_set_shadow(int on)
{
    atomic_store_explicit(&g_phase_cfg.shadow, on ? 1 : 0,
                          memory_order_relaxed);
    phase_live_touch();
}

static void phase_live_set_main_last(int on)
{
    atomic_store_explicit(&g_phase_cfg.main_last, on ? 1 : 0,
                          memory_order_relaxed);
    phase_live_touch();
}

static void phase_live_set_weighted(int on)
{
    atomic_store_explicit(&g_phase_cfg.weighted, on ? 1 : 0,
                          memory_order_relaxed);
    phase_live_touch();
}

static void phase_live_set_chunks(unsigned v)
{
    atomic_store_explicit(&g_phase_cfg.chunks, v, memory_order_relaxed);
    phase_live_touch();
}

static void phase_live_set_max(unsigned v)
{
    atomic_store_explicit(&g_phase_cfg.max_requests, v, memory_order_relaxed);
    phase_live_touch();
}

static void phase_live_set_threshold(unsigned v)
{
    atomic_store_explicit(&g_phase_cfg.threshold, v, memory_order_relaxed);
    phase_live_touch();
}

static void phase_live_set_stack_zero(unsigned v)
{
    atomic_store_explicit(&g_phase_cfg.stack_zero, v, memory_order_relaxed);
    phase_live_touch();
}

static void phase_live_set_spin_us(unsigned v)
{
    atomic_store_explicit(&g_phase_cfg.spin_us, v, memory_order_relaxed);
    phase_live_touch();
}

static void phase_log_config(const char *why)
{
    unsigned chunks = g_phase.chunks;
    fprintf(stderr,
            "[phase-tune] %s: enabled=%d shadow_cfg=%d shadow=%d "
            "main_last=%d weighted=%d chunks=%u%s max=%u threshold=%u "
            "stack_zero=%u spin_us=%u\n",
            why, g_phase.enabled,
            atomic_load_explicit(&g_phase_cfg.shadow, memory_order_relaxed),
            g_phase.shadow_enabled, g_phase.main_last, g_phase.weighted,
            chunks, chunks ? "" : "(auto)", g_phase.max_requests,
            g_phase.threshold, g_phase.stack_zero, g_phase.spin_us);
}

static void phase_sync_config(void)
{
    if (!atomic_load_explicit(&g_phase_cfg.dirty, memory_order_acquire))
        return;
    if (g_phase.count || g_phase.reserved || g_phase.shadow_busy)
        return;

    int en = atomic_load_explicit(&g_phase_cfg.enabled, memory_order_relaxed);
    int sh = atomic_load_explicit(&g_phase_cfg.shadow, memory_order_relaxed);
    if (!g_phase_seq_ctx_ready || !g_phase_container_ready) {
        en = 0;
        sh = 0;
    }

    unsigned chunks = atomic_load_explicit(&g_phase_cfg.chunks,
                                           memory_order_relaxed);
    if (chunks == 1u) chunks = 2u;
    if (g_workers && chunks > g_workers) chunks = g_workers;

    g_phase.enabled = en ? 1 : 0;
    g_phase.shadow_enabled = (sh && !en) ? 1 : 0;
    g_phase.main_last = atomic_load_explicit(&g_phase_cfg.main_last,
                                             memory_order_relaxed) ? 1 : 0;
    g_phase.weighted = atomic_load_explicit(&g_phase_cfg.weighted,
                                            memory_order_relaxed) ? 1 : 0;
    g_phase.chunks = chunks;
    g_phase.max_requests = phase_clamp_u(
        atomic_load_explicit(&g_phase_cfg.max_requests, memory_order_relaxed),
        2u, WC3_PHASE_MAX_REQUESTS);
    g_phase.threshold = phase_clamp_u(
        atomic_load_explicit(&g_phase_cfg.threshold, memory_order_relaxed),
        2u, WC3_PHASE_MAX_REQUESTS);
    g_phase.stack_zero = phase_clamp_u(
        atomic_load_explicit(&g_phase_cfg.stack_zero, memory_order_relaxed),
        RAY_STACK_ZERO_MIN, RAY_STACK_ZERO_MAX);
    g_phase.spin_us = phase_clamp_u(
        atomic_load_explicit(&g_phase_cfg.spin_us, memory_order_relaxed),
        0u, 1000000u);

    atomic_store_explicit(&g_phase_cfg.dirty, 0, memory_order_release);
    phase_log_config("reload");
}
#endif

static inline int parallel_main_last(void);
#if WC3_PAR_LAYOUT_DEBUG
static inline int parallel_weighted_split(void);
static inline int parallel_reject_slow(void);
#endif

#if WC3_PAR_LAYOUT_DEBUG
/* The caller executes the last chunk on the main core.  Its cost is measured
 * separately from the context it borrows: the same context may be a worker in
 * another split and then carries that core's timing. */
static uint64_t g_main_ns_per_item;

/* Build relative speed weights from the measured per-item cost of each
 * execution slot.  A missing sample gets a conservative mid weight instead of
 * being treated as fast as the best measured core; the first weighted job then
 * falls back to an equal split and later jobs use real samples.  A sample
 * slower than four times the best can be rejected by the debug policy.  The
 * weights are only a ratio, not a predicted absolute time. */
static uint64_t chunk_ns_per_item(unsigned i, unsigned chunks)
{
    if (parallel_main_last() && i + 1u == chunks)
        return g_main_ns_per_item;
    return g_worker[i].ns_per_item;
}

static void parallel_chunk_weights(uint64_t *weights, unsigned chunks)
{
    uint64_t best = 0;
    int have_best = 0;

    for (unsigned i = 0; i < chunks; i++) {
        uint64_t est = chunk_ns_per_item(i, chunks);
        if (est && (!have_best || est < best)) {
            best = est;
            have_best = 1;
        }
    }

    for (unsigned i = 0; i < chunks; i++) {
        uint64_t est = chunk_ns_per_item(i, chunks);
        if (!have_best || !est) {
            weights[i] = 50;
            continue;
        }
        if (parallel_reject_slow() && i + 1u != chunks &&
            est > best * 4u) {
            weights[i] = 0;
            continue;
        }
        uint64_t w = best * 100u / est;
        if (w > 100u) w = 100u;
        if (w < 10u) w = 10u;
        weights[i] = w;
    }
}

static void parallel_apply_split(RayJob *job, uint32_t total,
                                 unsigned chunks)
{
    if (!parallel_weighted_split()) {
        job_set_chunk_split(job, total, chunks);
        return;
    }
    uint64_t weights[WC3_PARALLEL_MAX_WORKERS];
    parallel_chunk_weights(weights, chunks);
    job_set_weighted_split(job, total, chunks, weights);

    static unsigned logged;
    if (logged < 8u) {
        fprintf(stderr, "[parallel] weighted split total=%u chunks=%u len=",
                total, chunks);
        for (unsigned i = 0; i < chunks; i++)
            fprintf(stderr, "%s%u", i ? "," : "", job->chunk_len[i]);
        fprintf(stderr, " weight=");
        for (unsigned i = 0; i < chunks; i++)
            fprintf(stderr, "%s%llu", i ? "," : "",
                    (unsigned long long)weights[i]);
        fprintf(stderr, "\n");
        logged++;
    }
}
#else
static void parallel_apply_split(RayJob *job, uint32_t total,
                                 unsigned chunks)
{
    job_set_chunk_split(job, total, chunks);
}
#endif

/* Chunk count from a minimum-chunk budget.  The debug ceil policy may use one
 * extra chunk when the floor would leave selected cores idle; release keeps
 * the floor.  The overflow-safe form avoids total + min_chunk wrapping. */
static unsigned chunk_count_by_min(uint32_t total, unsigned min_chunk,
                                   int use_ceil)
{
    if (!min_chunk) return total;
    unsigned by_count = total / min_chunk;
    if (use_ceil && (uint64_t)by_count * min_chunk < total)
        by_count++;
    if (by_count == 0u && total != 0u) by_count = 1u;
    return by_count;
}
#if WC3_DEBUG && WC3_DEBUG_PERF
/* The triangle wrapper is also entered from a raycast/vertex worker.  Pointer
 * identity is enough here; the pool owns these CPU objects for the process. */
static int triangle_ab_cpu_is_worker(const CPU *c)
{
    if (!g_worker) return 0;
    for (unsigned i = 0; i < g_workers; i++) {
        if (c == &g_worker[i].cpu) return 1;
    }
    return 0;
}
#endif
/* Product list from WC3_PARALLEL_WORKER_CPUS.  Empty means the automatic
 * one-worker-per-allowed-CPU layout; a non-empty list sets both the pool size
 * and a one-core affinity for each worker. */
static int g_worker_cpu[WC3_PARALLEL_MAX_WORKERS];
static unsigned g_worker_cpu_count;
#if WC3_PAR_LAYOUT_DEBUG
static RayCtx g_verify_ctx;
#endif

static RayTuning g_tune;
#if WC3_PAR_LAYOUT_DEBUG
/* A/B switches for the parallel layout.  They are diagnostic only: release
 * always runs the main-last layout, the equal floor split and the fixed
 * main-spin budget.  The four live-tunable ones are atomics: the debug tuner
 * thread rewrites them while the hot path reads a relaxed value. */
static int g_worker_order_reverse;
static int g_ray_split_ceil;
static int g_vertex_split_ceil;
static int g_triangle_split_ceil;
static _Atomic int g_main_last = 1;
static _Atomic int g_weighted_split;
static _Atomic int g_reject_slow;
static _Atomic int g_main_spin_adaptive;
#endif

static inline int parallel_main_last(void)
{
#if WC3_PAR_LAYOUT_DEBUG
    return atomic_load_explicit(&g_main_last, memory_order_relaxed);
#else
    return 1;
#endif
}

static inline int parallel_ray_split_ceil(void)
{
#if WC3_PAR_LAYOUT_DEBUG
    return g_ray_split_ceil;
#else
    return 0;
#endif
}

static inline int parallel_vertex_split_ceil(void)
{
#if WC3_PAR_LAYOUT_DEBUG
    return g_vertex_split_ceil;
#else
    return 0;
#endif
}

static inline int parallel_triangle_split_ceil(void)
{
#if WC3_PAR_LAYOUT_DEBUG
    return g_triangle_split_ceil;
#else
    return 0;
#endif
}

#if WC3_PAR_LAYOUT_DEBUG
static inline int parallel_weighted_split(void)
{
    return atomic_load_explicit(&g_weighted_split, memory_order_relaxed);
}

static inline int parallel_reject_slow(void)
{
    return atomic_load_explicit(&g_reject_slow, memory_order_relaxed);
}
#endif

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
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
/* Set while a debug phase owns the pool.  The existing batch dispatchers
 * check it before publishing so a shadow phase and a per-call job cannot
 * share g_job_seq.  Release never sees this flag. */
static _Atomic int g_phase_busy;
#endif
/* First published value is 1: done_seq starts at zero, so a bare increment
 * result would look like an already-finished first job.  Only the submit-lock
 * holder writes this counter. */
static _Atomic uint64_t g_job_seq;
/* High 32 bits: sequence tag; low 32 bits: number of active workers.  The
 * caller executes the last chunk itself and is not part of this set.  Tagging
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

#if WC3_PAR_LAYOUT_DEBUG
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
#if WC3_PAR_LAYOUT_DEBUG
static unsigned long long g_verify_calls;
static unsigned long long g_verify_failures;
#endif

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#if WC3_PAR_LAYOUT_DEBUG
/* Record the measured per-item cost of one chunk.  The caller supplies the
 * start timestamp; the context's count is the number of candidates that this
 * chunk actually processed.  `call_ns` excludes the fixed preparation cost,
 * so a small chunk does not look artificially slow.  A smoothed value keeps
 * one noisy chunk from dominating the weighted split. */
static void record_chunk_timing(RayCtx *ctx, uint64_t t0)
{
    uint64_t ns = now_ns() - t0;
    uint64_t work = ctx->call_ns ? ctx->call_ns : ns;
    ctx->chunk_ns = ns;
    ctx->chunk_count = ctx->count;
    if (ctx->count) {
        uint64_t per = work / ctx->count;
        if (!ctx->ns_per_item)
            ctx->ns_per_item = per;
        else
            ctx->ns_per_item = (ctx->ns_per_item * 3u + per) / 4u;
    }
}

/* The caller's chunk is measured on the main core, not on the borrowed
 * context's assigned core.  Keep it separate so a context that later returns
 * to the worker set still has its worker-core estimate. */
static void record_main_timing(RayCtx *ctx, uint64_t t0)
{
    uint64_t ns = now_ns() - t0;
    uint64_t work = ctx->call_ns ? ctx->call_ns : ns;
    ctx->chunk_ns = ns;
    ctx->chunk_count = ctx->count;
    if (ctx->count) {
        uint64_t per = work / ctx->count;
        if (!g_main_ns_per_item)
            g_main_ns_per_item = per;
        else
            g_main_ns_per_item = (g_main_ns_per_item * 3u + per) / 4u;
    }
}
#endif

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

#if WC3_PAR_LAYOUT_DEBUG
static unsigned env_u(const char *name, unsigned fallback, unsigned maximum)
{
    const char *s = getenv(name);
    if (!s || !*s) return fallback;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || v > maximum) return fallback;
    return (unsigned)v;
}
#endif

static int env_off(const char *name)
{
    const char *s = getenv(name);
    if (!s || !*s) return 0;
    if (!strcmp(s, "0") || !strcmp(s, "off") || !strcmp(s, "false") ||
        !strcmp(s, "OFF") || !strcmp(s, "FALSE"))
        return 1;
    return 0;
}

#if WC3_PAR_LAYOUT_DEBUG
static int env_on(const char *name)
{
    const char *s = getenv(name);
    if (!s || !*s) return 0;
    return !env_off(name);
}
#endif

#if WC3_DEBUG && WC3_DEBUG_PERF
/* Per-call diagnostic counters read guest memory and add a branch to every
 * vertex/triangle batch.  They are useful for coverage, but they also perturb
 * an A/B run.  WC3_PERF_NOTES=0 removes them without changing the parallel
 * path; the default stays on so an existing PERF run keeps its coverage
 * lines. */
static int g_perf_notes = 1;
static int g_perf_notes_env_ready;
static int perf_notes_enabled(void)
{
    if (!g_perf_notes_env_ready) {
        g_perf_notes_env_ready = 1;
        g_perf_notes = !env_off("WC3_PERF_NOTES");
    }
    return g_perf_notes;
}
#endif

/* Read the CPUs this process may actually run on.  _SC_NPROCESSORS_ONLN
 * counts online CPUs for the whole device, not the cpuset attached to the app;
 * using it can create more workers than the scheduler will ever run.
 *
 * Query the thread-group leader (getpid()), not the calling thread (pid 0):
 * the game main thread is pinned to one selected core, and filtering the
 * worker list through that one-core mask would collapse a multi-core
 * selection to a single worker.  The leader is not pinned by the game. */
static int read_allowed_cpu_mask(unsigned long long *out)
{
#if defined(__NR_sched_getaffinity)
    unsigned char mask[128];
    memset(mask, 0, sizeof mask);
    if (syscall(__NR_sched_getaffinity, getpid(), sizeof mask, mask) == 0) {
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

/* Parse the product core list ("0,2,4").  Invalid entries and duplicates are
 * ignored; the caller filters the result against the live cpuset. */
static unsigned parse_worker_cpu_list(const char *text, int *out, unsigned max)
{
    unsigned count = 0;
    const char *p = text;

    while (p && *p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        if (*p < '0' || *p > '9') {
            while (*p && *p != ',') p++;
            continue;
        }

        unsigned value = 0;
        while (*p >= '0' && *p <= '9') {
            if (value <= WC3_PARALLEL_MAX_WORKERS)
                value = value * 10u + (unsigned)(*p - '0');
            p++;
        }
        if (value < WC3_PARALLEL_MAX_WORKERS) {
            int duplicate = 0;
            for (unsigned i = 0; i < count; i++) {
                if (out[i] == (int)value) {
                    duplicate = 1;
                    break;
                }
            }
            if (!duplicate && count < max) out[count++] = (int)value;
        }
        while (*p && *p != ',') p++;
    }
    return count;
}

/* The active set is a prefix of the pool, so the product list must be ordered
 * fastest-to-slowest: the fastest selected cores run the worker chunks and the
 * slowest active core, not the slowest selected core, owns the critical path.
 * The caller's core is last and outside the active set.  If an external list
 * did not put WC3_MAIN_CPU last, move it there without disturbing the relative
 * order of the remaining cores.  A main CPU outside the worker list needs no
 * change because no worker is pinned to it. */
static void reserve_main_cpu_last(void)
{
    if (g_worker_cpu_count < 2u) return;
    const char *value = getenv("WC3_MAIN_CPU");
    if (!value || !*value) return;

    char *end = NULL;
    long cpu = strtol(value, &end, 10);
    if (!end || end == value || *end != '\0' || cpu < 0 || cpu >= 64)
        return;

    unsigned pos = g_worker_cpu_count;
    for (unsigned i = 0; i < g_worker_cpu_count; i++) {
        if (g_worker_cpu[i] == (int)cpu) {
            pos = i;
            break;
        }
    }
    if (pos >= g_worker_cpu_count || pos == g_worker_cpu_count - 1u)
        return;

    int main_cpu = g_worker_cpu[pos];
    for (unsigned i = pos; i + 1u < g_worker_cpu_count; i++)
        g_worker_cpu[i] = g_worker_cpu[i + 1u];
    g_worker_cpu[g_worker_cpu_count - 1u] = main_cpu;
#if WC3_PAR_LAYOUT_DEBUG
    fprintf(stderr,
            "[parallel] main cpu %ld moved to the last pool slot; "
            "the last pool slot is never in the active set\n", cpu);
#endif
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
    if (!perf_notes_enabled()) return;
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
    if (!perf_notes_enabled()) return;
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

#if WC3_DEBUG && WC3_DEBUG_PERF
/* Guard-reason counter for the generic vertex path.
 *
 * L_6F141680 sends every flags==0 batch through the stream setup to
 * L_6F1419D4 (mode 0) or L_6F1417F0 (mode 1).  Both entries call
 * wc3_parallel_vertex_try(); with many candidate batches in a real match and
 * a 1024-call perf print cadence, a missing [perf] vertex line means no
 * meaningful number of them reached perf_note_kind, not that the loops were
 * never reached.
 * This counter names the rejecting check without changing CPU state, guest
 * memory or the validation policy.  It is compiled only into a PERF build;
 * release has no counter and no call. */
enum {
    VG_LOW = 0,
    VG_HIGH,
    VG_BYPASS,
    VG_DISABLED,
    VG_BAD_KIND,
    VG_INIT_ACTIVE,
    VG_READY,
    VG_SLOW_DISABLED,
    VG_FRAME,
    VG_OBJ,
    VG_MODE,
    VG_SLOT,
    VG_SRC0,
    VG_SRC1,
    VG_SRC2,
    VG_SRC3,
    VG_SRC4,
    VG_PM,
    VG_DUMMY_FRAME,
    VG_DUMMY_ALIAS,
    VG_OUT_SPAN,
    VG_OUT_FRAME,
    VG_OVERLAP,
    VG_CHUNKS,
    VG_CHUNKS_ONE,
    VG_RUN,
    VG_COUNT
};
static unsigned long long g_vertex_guard[VG_COUNT];
static unsigned long long g_vertex_guard_calls;
static unsigned long long g_vertex_guard_last_print;
static unsigned g_vertex_guard_every = 65536u;
static int g_vertex_guard_print = 1;
static int g_vertex_guard_env_ready;

static void vertex_guard_note(unsigned reason)
{
    if (!perf_notes_enabled()) return;
    if (!g_vertex_guard_env_ready) {
        g_vertex_guard_env_ready = 1;
        const char *s = getenv("WC3_VERTEX_STATS");
        g_vertex_guard_print = (s && *s) ? env_on("WC3_VERTEX_STATS") : 1;
        g_vertex_guard_every =
            env_u("WC3_VERTEX_STATS_EVERY", 65536u, UINT_MAX);
        if (!g_vertex_guard_every) g_vertex_guard_every = 65536u;
    }
    if (reason >= VG_COUNT) return;
    g_vertex_guard_calls++;
    g_vertex_guard[reason]++;
    if (!g_vertex_guard_print) return;
    if (g_vertex_guard_calls - g_vertex_guard_last_print <
        g_vertex_guard_every)
        return;
    g_vertex_guard_last_print = g_vertex_guard_calls;
    fprintf(stderr,
            "[vertex-guard] calls=%llu thr=%u minc=%u low=%llu high=%llu "
            "bypass=%llu disabled=%llu kind=%llu init=%llu ready=%llu "
            "slow_disabled=%llu frame=%llu obj=%llu mode=%llu slot=%llu "
            "src0=%llu src1=%llu src2=%llu src3=%llu src4=%llu pm=%llu "
            "dummy=%llu alias=%llu span=%llu out_frame=%llu overlap=%llu "
            "chunks=%llu chunks_one=%llu run=%llu\n",
            g_vertex_guard_calls,
            par_gate_u(&g_vertex_threshold), par_gate_u(&g_vertex_min_chunk),
            g_vertex_guard[VG_LOW], g_vertex_guard[VG_HIGH],
            g_vertex_guard[VG_BYPASS], g_vertex_guard[VG_DISABLED],
            g_vertex_guard[VG_BAD_KIND], g_vertex_guard[VG_INIT_ACTIVE],
            g_vertex_guard[VG_READY], g_vertex_guard[VG_SLOW_DISABLED],
            g_vertex_guard[VG_FRAME], g_vertex_guard[VG_OBJ],
            g_vertex_guard[VG_MODE], g_vertex_guard[VG_SLOT],
            g_vertex_guard[VG_SRC0], g_vertex_guard[VG_SRC1],
            g_vertex_guard[VG_SRC2], g_vertex_guard[VG_SRC3],
            g_vertex_guard[VG_SRC4], g_vertex_guard[VG_PM],
            g_vertex_guard[VG_DUMMY_FRAME], g_vertex_guard[VG_DUMMY_ALIAS],
            g_vertex_guard[VG_OUT_SPAN], g_vertex_guard[VG_OUT_FRAME],
            g_vertex_guard[VG_OVERLAP], g_vertex_guard[VG_CHUNKS],
            g_vertex_guard[VG_CHUNKS_ONE], g_vertex_guard[VG_RUN]);
}
static int vertex_guard_fail(unsigned reason)
{
    vertex_guard_note(reason);
    return 0;
}
# define WC3_VERTEX_GUARD_FAIL(reason) \
    do { return vertex_guard_fail(reason); } while (0)
# define WC3_VERTEX_GUARD_NOTE(reason) vertex_guard_note(reason)
#else
# define WC3_VERTEX_GUARD_FAIL(reason) do { return 0; } while (0)
# define WC3_VERTEX_GUARD_NOTE(reason) ((void)0)
#endif

#if WC3_DEBUG && WC3_DEBUG_PERF
/* Dispatch-decision counter for L_6F141680, the vertex batch setup.
 *
 * The post-lift pass inserts one call after `this` and `dest` are loaded and
 * before the first format test.  The counter only reads the incoming object
 * and the vertex block; it does not change the CPU or guest memory.  The
 * reason order matches the generated branch order exactly, so the reason
 * buckets sum to the number of setup calls.
 *
 * WC3_VERTEX_STATS=0 disables the periodic line; WC3_VERTEX_STATS_EVERY sets
 * the cadence.  A PERF build defaults the line on: the counter exists to
 * answer whether the format-2 fast loops run in an active match, and asking
 * for a profiled build and then forgetting the env is not useful.
 *
 * The per-branch count histograms answer the threshold question directly:
 * a CF0/BD0/AD0 branch that is selected only for tiny batches cannot pay for
 * a chunk split.  All state is caller-thread-only, like the raycast and kind
 * counters above. */
enum {
    VERTEX_DISPATCH_CF0 = 0,
    VERTEX_DISPATCH_BD0,
    VERTEX_DISPATCH_AD0,
    VERTEX_DISPATCH_GEN_FLAGS,
    VERTEX_DISPATCH_GEN_STREAM,
    VERTEX_DISPATCH_GEN_STRIDES,
    VERTEX_DISPATCH_GEN_ALIGN,
    VERTEX_DISPATCH_GEN_MODE,
    VERTEX_DISPATCH_GEN_MODE1,
    VERTEX_DISPATCH_REASON_COUNT
};

static unsigned long long g_dispatch_calls;
static unsigned long long g_dispatch_reason[VERTEX_DISPATCH_REASON_COUNT];
static unsigned long long g_dispatch_mode[3];
static unsigned long long g_dispatch_bit2;
static unsigned long long g_dispatch_stream2;
static unsigned long long g_dispatch_strides_ok;
static unsigned long long g_dispatch_align_ok;
static unsigned long long g_dispatch_index1;
/* Four work-kind rows: CF0, BD0, AD0, generic.  The eight columns are the
 * count buckets below. */
static unsigned long long g_dispatch_branch_count[4][8];
static unsigned long long g_dispatch_last_print;
static int g_dispatch_env_ready;
static int g_dispatch_print;
static unsigned g_dispatch_every = RAY_STATS_DEFAULT;

static unsigned dispatch_count_bucket(uint32_t n)
{
    if (n == 0u) return 0;
    if (n < 4u) return 1;
    if (n < 16u) return 2;
    if (n < 64u) return 3;
    if (n < 256u) return 4;
    if (n < 512u) return 5;
    if (n < 1024u) return 6;
    return 7;
}

static void dispatch_load_env(void)
{
    if (g_dispatch_env_ready) return;
    g_dispatch_env_ready = 1;
    const char *s = getenv("WC3_VERTEX_STATS");
    g_dispatch_print = (s && *s) ? env_on("WC3_VERTEX_STATS") : 1;
    unsigned every = env_u("WC3_VERTEX_STATS_EVERY", RAY_STATS_DEFAULT,
                           RAY_STATS_EVERY_MAX);
    if (!every) every = RAY_STATS_DEFAULT;
    g_dispatch_every = every;
}

void wc3_vertex_dispatch_note(const CPU *c)
{
    unsigned reason;
    uint32_t obj, flags, mode, stream, count, dest;
    uint32_t stride0, stride1, stride3, index_stride;
    unsigned bucket;

    if (!perf_notes_enabled()) return;
    if (!c) return;
    dispatch_load_env();

    if (!hle_android_guest_ok(c->ebp - 4u, 4u)) return;
    obj = rd32(c->ebp - 4u);
    if (!obj || !hle_android_guest_ok(obj, 0x22Cu)) return;

    flags = rd32(obj + 0x90u);
    mode = rd32(obj + 0x224u);
    stream = rd32(obj + 0x228u);
    dest = c->ebx;
    count = rd32(GVA(WC3_VERTEX_GLOBAL_FIRST));
    stride0 = rd32(GVA(0x6FD53940u));
    stride1 = rd32(GVA(0x6FD53948u));
    stride3 = rd32(GVA(0x6FD53964u));
    index_stride = rd32(GVA(0x6FD53958u));

    if (!(flags & 6u)) reason = VERTEX_DISPATCH_GEN_FLAGS;
    else if (stream != 2u) reason = VERTEX_DISPATCH_GEN_STREAM;
    else if (stride0 != 12u || stride1 != 12u || stride3 != 8u)
        reason = VERTEX_DISPATCH_GEN_STRIDES;
    else if (dest & 0xFu) reason = VERTEX_DISPATCH_GEN_ALIGN;
    else if (mode == 0u)
        reason = (flags & 4u) ? VERTEX_DISPATCH_BD0 : VERTEX_DISPATCH_AD0;
    else if (mode == 1u)
        reason = (flags & 4u) ? VERTEX_DISPATCH_CF0
                              : VERTEX_DISPATCH_GEN_MODE1;
    else
        reason = VERTEX_DISPATCH_GEN_MODE;

    bucket = dispatch_count_bucket(count);
    g_dispatch_calls++;
    g_dispatch_reason[reason]++;
    g_dispatch_mode[mode < 2u ? mode : 2u]++;
    g_dispatch_bit2 += (flags & 4u) != 0u;
    g_dispatch_stream2 += stream == 2u;
    g_dispatch_strides_ok += stride0 == 12u && stride1 == 12u &&
                             stride3 == 8u;
    g_dispatch_align_ok += (dest & 0xFu) == 0u;
    g_dispatch_index1 += index_stride == 1u;
    g_dispatch_branch_count[reason <= VERTEX_DISPATCH_AD0 ? reason : 3u]
                           [bucket]++;

    if (!g_dispatch_print) return;
    if (g_dispatch_calls - g_dispatch_last_print < g_dispatch_every) return;
    g_dispatch_last_print = g_dispatch_calls;
    fprintf(stderr,
            "[vertex-dispatch] total=%llu cf0=%llu bd0=%llu ad0=%llu "
            "gen_flags=%llu gen_stream=%llu gen_strides=%llu "
            "gen_align=%llu gen_mode=%llu gen_mode1=%llu "
            "mode0=%llu mode1=%llu modeX=%llu bit2=%llu stream2=%llu "
            "strides_ok=%llu align_ok=%llu index1=%llu "
            "cf0_n=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu] "
            "bd0_n=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu] "
            "ad0_n=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu] "
            "gen_n=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu]\n",
            g_dispatch_calls,
            g_dispatch_reason[VERTEX_DISPATCH_CF0],
            g_dispatch_reason[VERTEX_DISPATCH_BD0],
            g_dispatch_reason[VERTEX_DISPATCH_AD0],
            g_dispatch_reason[VERTEX_DISPATCH_GEN_FLAGS],
            g_dispatch_reason[VERTEX_DISPATCH_GEN_STREAM],
            g_dispatch_reason[VERTEX_DISPATCH_GEN_STRIDES],
            g_dispatch_reason[VERTEX_DISPATCH_GEN_ALIGN],
            g_dispatch_reason[VERTEX_DISPATCH_GEN_MODE],
            g_dispatch_reason[VERTEX_DISPATCH_GEN_MODE1],
            g_dispatch_mode[0], g_dispatch_mode[1], g_dispatch_mode[2],
            g_dispatch_bit2, g_dispatch_stream2,
            g_dispatch_strides_ok, g_dispatch_align_ok, g_dispatch_index1,
            g_dispatch_branch_count[VERTEX_DISPATCH_CF0][0],
            g_dispatch_branch_count[VERTEX_DISPATCH_CF0][1],
            g_dispatch_branch_count[VERTEX_DISPATCH_CF0][2],
            g_dispatch_branch_count[VERTEX_DISPATCH_CF0][3],
            g_dispatch_branch_count[VERTEX_DISPATCH_CF0][4],
            g_dispatch_branch_count[VERTEX_DISPATCH_CF0][5],
            g_dispatch_branch_count[VERTEX_DISPATCH_CF0][6],
            g_dispatch_branch_count[VERTEX_DISPATCH_CF0][7],
            g_dispatch_branch_count[VERTEX_DISPATCH_BD0][0],
            g_dispatch_branch_count[VERTEX_DISPATCH_BD0][1],
            g_dispatch_branch_count[VERTEX_DISPATCH_BD0][2],
            g_dispatch_branch_count[VERTEX_DISPATCH_BD0][3],
            g_dispatch_branch_count[VERTEX_DISPATCH_BD0][4],
            g_dispatch_branch_count[VERTEX_DISPATCH_BD0][5],
            g_dispatch_branch_count[VERTEX_DISPATCH_BD0][6],
            g_dispatch_branch_count[VERTEX_DISPATCH_BD0][7],
            g_dispatch_branch_count[VERTEX_DISPATCH_AD0][0],
            g_dispatch_branch_count[VERTEX_DISPATCH_AD0][1],
            g_dispatch_branch_count[VERTEX_DISPATCH_AD0][2],
            g_dispatch_branch_count[VERTEX_DISPATCH_AD0][3],
            g_dispatch_branch_count[VERTEX_DISPATCH_AD0][4],
            g_dispatch_branch_count[VERTEX_DISPATCH_AD0][5],
            g_dispatch_branch_count[VERTEX_DISPATCH_AD0][6],
            g_dispatch_branch_count[VERTEX_DISPATCH_AD0][7],
            g_dispatch_branch_count[3][0], g_dispatch_branch_count[3][1],
            g_dispatch_branch_count[3][2], g_dispatch_branch_count[3][3],
            g_dispatch_branch_count[3][4], g_dispatch_branch_count[3][5],
            g_dispatch_branch_count[3][6], g_dispatch_branch_count[3][7]);
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

#if WC3_PAR_LAYOUT_DEBUG
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
#endif

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

#if WC3_DEBUG && WC3_DEBUG_PARALLEL
/* ------------------------------------------------------------------ */
/* Live parallel tuner (debug-only).                                   */
/*                                                                     */
/* A detached thread polls <dir>/parallel.cfg, applies key=value pairs  */
/* to the atomic layout gates, and writes <dir>/parallel.metrics with   */
/* the efficiency snapshot.  It never takes g_submit_lock and never     */
/* touches a guest CPU, so the lock invariant is unchanged.  Release    */
/* compiles none of this.                                               */
/* ------------------------------------------------------------------ */

#define PAR_TUNE_PATH_MAX RAY_TUNE_PATH_MAX
#define PAR_REPORT_POLLS 10u   /* stderr [parallel-eff] cadence at 200 ms */

typedef struct ParEff {
    _Atomic unsigned long long main_wait_ns;
    _Atomic unsigned long long worker_busy_ns;
    _Atomic unsigned long long par_calls;
    _Atomic unsigned long long par_items;
    _Atomic unsigned long long par_ns;
    _Atomic unsigned long long seq_calls;
    _Atomic unsigned long long seq_items;
    _Atomic unsigned long long seq_ns;
    _Atomic unsigned long long ray_par_calls;
    _Atomic unsigned long long ray_par_items;
    _Atomic unsigned long long ray_par_ns;
    _Atomic unsigned long long chunks_sum;
    _Atomic unsigned long long phase_applies;
    _Atomic unsigned long long phase_items;
    _Atomic unsigned long long phase_par_runs;
    _Atomic unsigned long long phase_seq_runs;
    _Atomic unsigned long long phase_seq_ns;
    _Atomic unsigned long long phase_apply_ns;
    _Atomic unsigned long long phase_run_ns;
    _Atomic unsigned long long phase_wake_ns;
    _Atomic unsigned long long phase_join_ns;
    _Atomic unsigned long long phase_slice_ns;
    _Atomic unsigned long long phase_chunks_sum;
    _Atomic unsigned long long phase_shadow_flushes;
    _Atomic unsigned long long phase_shadow_mismatches;
    _Atomic unsigned long long phase_shadow_seq_ns;
    _Atomic unsigned long long phase_shadow_par_ns;
} ParEff;

static ParEff g_eff;

static _Atomic int g_par_report = 1;
static _Atomic int g_par_dump;
static _Atomic unsigned g_par_poll_ms = 200;
static _Atomic int g_par_ab;
static _Atomic unsigned g_par_ab_frames = 10;

static char g_par_cfg_path[PAR_TUNE_PATH_MAX];
static char g_par_alt_path[PAR_TUNE_PATH_MAX];
static char g_par_metrics_path[PAR_TUNE_PATH_MAX];
static int g_par_paths_ready;
static uint32_t g_par_cfg_hash;
static uint32_t g_par_alt_hash;
static int g_par_tuner_started;

/* Plain snapshot for the A/B baselines. */
typedef struct ParEffSnap {
    unsigned long long main_wait_ns;
    unsigned long long worker_busy_ns;
    unsigned long long par_calls;
    unsigned long long par_items;
    unsigned long long par_ns;
    unsigned long long seq_calls;
    unsigned long long seq_items;
    unsigned long long seq_ns;
    unsigned long long ray_par_calls;
    unsigned long long ray_par_items;
    unsigned long long ray_par_ns;
    unsigned long long chunks_sum;
    unsigned long long phase_applies;
    unsigned long long phase_items;
    unsigned long long phase_par_runs;
    unsigned long long phase_seq_runs;
    unsigned long long phase_seq_ns;
    unsigned long long phase_apply_ns;
    unsigned long long phase_run_ns;
    unsigned long long phase_wake_ns;
    unsigned long long phase_join_ns;
    unsigned long long phase_slice_ns;
    unsigned long long phase_chunks_sum;
    unsigned long long phase_shadow_flushes;
    unsigned long long phase_shadow_mismatches;
    unsigned long long phase_shadow_seq_ns;
    unsigned long long phase_shadow_par_ns;
} ParEffSnap;

static void par_eff_snapshot(ParEffSnap *o)
{
    o->main_wait_ns =
        atomic_load_explicit(&g_eff.main_wait_ns, memory_order_relaxed);
    o->worker_busy_ns =
        atomic_load_explicit(&g_eff.worker_busy_ns, memory_order_relaxed);
    o->par_calls =
        atomic_load_explicit(&g_eff.par_calls, memory_order_relaxed);
    o->par_items =
        atomic_load_explicit(&g_eff.par_items, memory_order_relaxed);
    o->par_ns = atomic_load_explicit(&g_eff.par_ns, memory_order_relaxed);
    o->seq_calls =
        atomic_load_explicit(&g_eff.seq_calls, memory_order_relaxed);
    o->seq_items =
        atomic_load_explicit(&g_eff.seq_items, memory_order_relaxed);
    o->seq_ns = atomic_load_explicit(&g_eff.seq_ns, memory_order_relaxed);
    o->ray_par_calls =
        atomic_load_explicit(&g_eff.ray_par_calls, memory_order_relaxed);
    o->ray_par_items =
        atomic_load_explicit(&g_eff.ray_par_items, memory_order_relaxed);
    o->ray_par_ns =
        atomic_load_explicit(&g_eff.ray_par_ns, memory_order_relaxed);
    o->chunks_sum =
        atomic_load_explicit(&g_eff.chunks_sum, memory_order_relaxed);
    o->phase_applies =
        atomic_load_explicit(&g_eff.phase_applies, memory_order_relaxed);
    o->phase_items =
        atomic_load_explicit(&g_eff.phase_items, memory_order_relaxed);
    o->phase_par_runs =
        atomic_load_explicit(&g_eff.phase_par_runs, memory_order_relaxed);
    o->phase_seq_runs =
        atomic_load_explicit(&g_eff.phase_seq_runs, memory_order_relaxed);
    o->phase_seq_ns =
        atomic_load_explicit(&g_eff.phase_seq_ns, memory_order_relaxed);
    o->phase_apply_ns =
        atomic_load_explicit(&g_eff.phase_apply_ns, memory_order_relaxed);
    o->phase_run_ns =
        atomic_load_explicit(&g_eff.phase_run_ns, memory_order_relaxed);
    o->phase_wake_ns =
        atomic_load_explicit(&g_eff.phase_wake_ns, memory_order_relaxed);
    o->phase_join_ns =
        atomic_load_explicit(&g_eff.phase_join_ns, memory_order_relaxed);
    o->phase_slice_ns =
        atomic_load_explicit(&g_eff.phase_slice_ns, memory_order_relaxed);
    o->phase_chunks_sum =
        atomic_load_explicit(&g_eff.phase_chunks_sum, memory_order_relaxed);
    o->phase_shadow_flushes =
        atomic_load_explicit(&g_eff.phase_shadow_flushes,
                             memory_order_relaxed);
    o->phase_shadow_mismatches =
        atomic_load_explicit(&g_eff.phase_shadow_mismatches,
                             memory_order_relaxed);
    o->phase_shadow_seq_ns =
        atomic_load_explicit(&g_eff.phase_shadow_seq_ns,
                             memory_order_relaxed);
    o->phase_shadow_par_ns =
        atomic_load_explicit(&g_eff.phase_shadow_par_ns,
                             memory_order_relaxed);
}

static void par_note_wait(unsigned long long ns)
{
    atomic_fetch_add_explicit(&g_eff.main_wait_ns, ns, memory_order_relaxed);
}

static void par_note_busy(unsigned long long ns)
{
    atomic_fetch_add_explicit(&g_eff.worker_busy_ns, ns, memory_order_relaxed);
}

static void par_note_par(unsigned long long items, unsigned long long ns,
                         unsigned chunks)
{
    atomic_fetch_add_explicit(&g_eff.par_items, items, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.par_ns, ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.par_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.chunks_sum, chunks, memory_order_relaxed);
}

/* Raycast-only parallel counters: the sequential baseline is measured only
 * on the raycast fallback, so the speedup must not mix in vertex/triangle. */
static void par_note_par_ray(unsigned long long items, unsigned long long ns,
                             unsigned chunks)
{
    par_note_par(items, ns, chunks);
    atomic_fetch_add_explicit(&g_eff.ray_par_items, items,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.ray_par_ns, ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.ray_par_calls, 1, memory_order_relaxed);
}

static void par_note_seq(unsigned long long items, unsigned long long ns)
{
    atomic_fetch_add_explicit(&g_eff.seq_items, items, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.seq_ns, ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.seq_calls, 1, memory_order_relaxed);
}

static void par_note_phase_apply(unsigned long long items,
                                 unsigned long long ns)
{
    atomic_fetch_add_explicit(&g_eff.phase_items, items,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.phase_apply_ns, ns,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.phase_applies, 1,
                              memory_order_relaxed);
}

static void par_note_phase_par(unsigned chunks, unsigned long long run_ns,
                               unsigned long long wake_ns,
                               unsigned long long join_ns,
                               unsigned long long slice_ns)
{
    atomic_fetch_add_explicit(&g_eff.phase_par_runs, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.phase_chunks_sum, chunks,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.phase_run_ns, run_ns,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.phase_wake_ns, wake_ns,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.phase_join_ns, join_ns,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.phase_slice_ns, slice_ns,
                              memory_order_relaxed);
}

static void par_note_phase_seq(unsigned long long ns)
{
    atomic_fetch_add_explicit(&g_eff.phase_seq_runs, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.phase_seq_ns, ns,
                              memory_order_relaxed);
}

static void par_note_phase_shadow(unsigned long long mismatches,
                                  unsigned long long seq_ns,
                                  unsigned long long par_ns)
{
    atomic_fetch_add_explicit(&g_eff.phase_shadow_flushes, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.phase_shadow_mismatches, mismatches,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.phase_shadow_seq_ns, seq_ns,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_eff.phase_shadow_par_ns, par_ns,
                              memory_order_relaxed);
}

static unsigned par_clamp_u(unsigned v, unsigned lo, unsigned hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int par_parse_bool(const char *s, int *out)
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

static uint32_t par_hash_text(const char *text)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        h ^= (uint32_t)*p;
        h *= 16777619u;
    }
    return h;
}

/* Apply one live key.  Unknown keys are reported once per reload. */
static void par_apply_key(const char *key, const char *value)
{
    unsigned u;
    int b;

    if (!strcmp(key, "ray_threshold")) {
        if (!strcmp(value, "auto")) {
#if WC3_DEBUG && WC3_DEBUG_HLE
            g_threshold_auto = 1;
            recompute_auto_threshold();
#endif
            return;
        }
        if (parse_uint(value, &u) && u <= RAY_MAX_THRESHOLD) {
            tune_set_uint(&g_tune.threshold, u);
#if WC3_DEBUG && WC3_DEBUG_HLE
            g_threshold_auto = 0;
#endif
        }
        return;
    }
    if (!strcmp(key, "ray_chunks")) {
        if (parse_uint(value, &u)) {
            if (u == 0) {
                tune_set_uint(&g_tune.chunk_limit, 0);
            } else if (g_workers && u > g_workers) {
                tune_set_uint(&g_tune.chunk_limit, g_workers);
            } else {
                tune_set_uint(&g_tune.chunk_limit, u);
            }
        }
        return;
    }
    if (!strcmp(key, "min_chunk")) {
        if (parse_uint(value, &u))
            tune_set_uint(&g_tune.min_chunk, par_clamp_u(u, 1u, 4096u));
        return;
    }
    if (!strcmp(key, "stack_zero")) {
        if (parse_uint(value, &u))
            tune_set_uint(&g_tune.stack_zero,
                          par_clamp_u(u, RAY_STACK_ZERO_MIN,
                                      RAY_STACK_ZERO_MAX));
        return;
    }
    if (!strcmp(key, "container_copy")) {
        if (parse_uint(value, &u))
            tune_set_uint(&g_tune.container_copy,
                          par_clamp_u(u, RAY_CONTAINER_MIN_COPY,
                                      RAY_CONTAINER_COPY));
        return;
    }
    if (!strcmp(key, "vertex_threshold")) {
        if (parse_uint(value, &u))
            par_gate_set_u(&g_vertex_threshold,
                           par_clamp_u(u, 1u, VERTEX_MAX_COUNT));
        return;
    }
    if (!strcmp(key, "vertex_chunks")) {
        if (parse_uint(value, &u))
            par_gate_set_u(&g_vertex_chunks,
                           g_workers ? par_clamp_u(u, 0u, g_workers) : u);
        return;
    }
    if (!strcmp(key, "vertex_min_chunk")) {
        if (parse_uint(value, &u))
            par_gate_set_u(&g_vertex_min_chunk,
                           par_clamp_u(u, 0u, VERTEX_MAX_COUNT));
        return;
    }
    if (!strcmp(key, "triangle_threshold")) {
        if (parse_uint(value, &u))
            par_gate_set_u(&g_triangle_threshold,
                           par_clamp_u(u, 1u, TRIANGLE_MAX_COUNT));
        return;
    }
    if (!strcmp(key, "triangle_chunks")) {
        if (parse_uint(value, &u))
            par_gate_set_u(&g_triangle_chunks,
                           g_workers ? par_clamp_u(u, 0u, g_workers) : u);
        return;
    }
    if (!strcmp(key, "triangle_min_chunk")) {
        if (parse_uint(value, &u))
            par_gate_set_u(&g_triangle_min_chunk,
                           par_clamp_u(u, 0u, TRIANGLE_MAX_COUNT));
        return;
    }
    if (!strcmp(key, "main_last")) {
        if (par_parse_bool(value, &b))
            atomic_store_explicit(&g_main_last, b, memory_order_relaxed);
        return;
    }
    if (!strcmp(key, "weighted_split")) {
        if (par_parse_bool(value, &b))
            atomic_store_explicit(&g_weighted_split, b, memory_order_relaxed);
        return;
    }
    if (!strcmp(key, "reject_slow")) {
        if (par_parse_bool(value, &b))
            atomic_store_explicit(&g_reject_slow, b, memory_order_relaxed);
        return;
    }
    if (!strcmp(key, "adaptive")) {
        if (par_parse_bool(value, &b))
            atomic_store_explicit(&g_main_spin_adaptive, b,
                                  memory_order_relaxed);
        return;
    }
    if (!strcmp(key, "main_spin_us")) {
        if (parse_uint(value, &u))
            tune_set_uint(&g_tune.main_spin_us, par_clamp_u(u, 0u, 1000000u));
        return;
    }
    if (!strcmp(key, "worker_spin_us")) {
        if (parse_uint(value, &u))
            tune_set_uint(&g_tune.spin_us, par_clamp_u(u, 0u, 10000u));
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
                          par_clamp_u(u, 1u, RAY_STATS_EVERY_MAX));
        return;
    }
    if (!strcmp(key, "phase")) {
        if (par_parse_bool(value, &b))
            phase_live_set_enabled(b);
        return;
    }
    if (!strcmp(key, "phase_shadow")) {
        if (par_parse_bool(value, &b))
            phase_live_set_shadow(b);
        return;
    }
    if (!strcmp(key, "phase_chunks")) {
        if (!strcmp(value, "auto")) {
            phase_live_set_chunks(0u);
        } else if (parse_uint(value, &u)) {
            phase_live_set_chunks(u);
        }
        return;
    }
    if (!strcmp(key, "phase_max")) {
        if (parse_uint(value, &u))
            phase_live_set_max(par_clamp_u(u, 2u, WC3_PHASE_MAX_REQUESTS));
        return;
    }
    if (!strcmp(key, "phase_threshold")) {
        if (parse_uint(value, &u))
            phase_live_set_threshold(
                par_clamp_u(u, 2u, WC3_PHASE_MAX_REQUESTS));
        return;
    }
    if (!strcmp(key, "phase_main_last")) {
        if (par_parse_bool(value, &b))
            phase_live_set_main_last(b);
        return;
    }
    if (!strcmp(key, "phase_weighted")) {
        if (par_parse_bool(value, &b))
            phase_live_set_weighted(b);
        return;
    }
    if (!strcmp(key, "phase_stack_zero")) {
        if (parse_uint(value, &u))
            phase_live_set_stack_zero(
                par_clamp_u(u, RAY_STACK_ZERO_MIN, RAY_STACK_ZERO_MAX));
        return;
    }
    if (!strcmp(key, "phase_spin_us")) {
        if (parse_uint(value, &u))
            phase_live_set_spin_us(par_clamp_u(u, 0u, 1000000u));
        return;
    }
    if (!strcmp(key, "report")) {
        if (par_parse_bool(value, &b))
            atomic_store_explicit(&g_par_report, b, memory_order_relaxed);
        return;
    }
    if (!strcmp(key, "dump")) {
        /* Any non-zero value requests one snapshot.  Incrementing the value
         * (dump=1, dump=2, ...) changes the file hash and requests another. */
        if (parse_uint(value, &u))
            atomic_store_explicit(&g_par_dump, u != 0u, memory_order_relaxed);
        return;
    }
    if (!strcmp(key, "tune_poll_ms")) {
        if (parse_uint(value, &u))
            atomic_store_explicit(&g_par_poll_ms,
                                  par_clamp_u(u, 1u, 5000u),
                                  memory_order_relaxed);
        return;
    }
    if (!strcmp(key, "ab")) {
        if (par_parse_bool(value, &b))
            atomic_store_explicit(&g_par_ab, b, memory_order_relaxed);
        return;
    }
    if (!strcmp(key, "ab_frames")) {
        if (parse_uint(value, &u))
            atomic_store_explicit(&g_par_ab_frames,
                                  par_clamp_u(u, 1u, 100000u),
                                  memory_order_relaxed);
        return;
    }
    fprintf(stderr, "[parallel-tune] unknown key '%s'\n", key);
}

static void par_parse_text(const char *text)
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
            par_apply_key(key, value);
    }
#if WC3_DEBUG && WC3_DEBUG_HLE
    /* The HLE tune file owns the `auto` threshold.  Keep it consistent if a
     * live min_chunk/chunks change arrived through the PARALLEL file. */
    recompute_auto_threshold();
#endif
}

static int par_read_file(const char *path, char *buf, size_t cap,
                         uint32_t *hash)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap - 1u, f);
    fclose(f);
    buf[n] = '\0';
    uint32_t h = par_hash_text(buf);
    if (hash && *hash == h) return 0;
    if (hash) *hash = h;
    return 1;
}

static void par_tuner_init_paths(void)
{
    const char *dir = getenv("WC3_DATA_DIR");
    if (dir && *dir) {
        snprintf(g_par_cfg_path, sizeof g_par_cfg_path, "%s/parallel.cfg", dir);
        snprintf(g_par_alt_path, sizeof g_par_alt_path,
                 "%s/parallel_alt.cfg", dir);
        snprintf(g_par_metrics_path, sizeof g_par_metrics_path,
                 "%s/parallel.metrics", dir);
    } else {
        snprintf(g_par_cfg_path, sizeof g_par_cfg_path, "parallel.cfg");
        snprintf(g_par_alt_path, sizeof g_par_alt_path, "parallel_alt.cfg");
        snprintf(g_par_metrics_path, sizeof g_par_metrics_path,
                 "parallel.metrics");
    }
    g_par_paths_ready = 1;
}

static void par_format_metrics(char *buf, size_t cap,
                               const ParEffSnap *cur,
                               const ParEffSnap *base,
                               unsigned long long interval_ns, int side)
{
    unsigned long long d_wait = cur->main_wait_ns - base->main_wait_ns;
    unsigned long long d_busy = cur->worker_busy_ns - base->worker_busy_ns;
    unsigned long long d_par_calls = cur->par_calls - base->par_calls;
    unsigned long long d_par_items = cur->par_items - base->par_items;
    unsigned long long d_par_ns = cur->par_ns - base->par_ns;
    unsigned long long d_seq_calls = cur->seq_calls - base->seq_calls;
    unsigned long long d_seq_items = cur->seq_items - base->seq_items;
    unsigned long long d_seq_ns = cur->seq_ns - base->seq_ns;
    unsigned long long d_ray_calls = cur->ray_par_calls - base->ray_par_calls;
    unsigned long long d_ray_items = cur->ray_par_items - base->ray_par_items;
    unsigned long long d_ray_ns = cur->ray_par_ns - base->ray_par_ns;
    unsigned long long d_chunks = cur->chunks_sum - base->chunks_sum;

    double wait_pct = interval_ns
        ? (double)d_wait * 100.0 / (double)interval_ns : 0.0;
    unsigned workers = g_workers ? g_workers : 1u;
    double util_pct = interval_ns
        ? (double)d_busy * 100.0 / ((double)workers * (double)interval_ns)
        : 0.0;
    unsigned long long par_per_item =
        d_par_items ? d_par_ns / d_par_items : 0ull;
    unsigned long long ray_par_per_item =
        d_ray_items ? d_ray_ns / d_ray_items : 0ull;
    unsigned long long seq_per_item =
        d_seq_items ? d_seq_ns / d_seq_items : 0ull;
    unsigned long long speedup_x100 =
        (ray_par_per_item && seq_per_item)
        ? seq_per_item * 100ull / ray_par_per_item : 0ull;
    double par_avg_us = d_par_calls ? (double)d_par_ns / (double)d_par_calls /
                                      1000.0 : 0.0;
    double ray_avg_us = d_ray_calls ? (double)d_ray_ns / (double)d_ray_calls /
                                      1000.0 : 0.0;
    double seq_avg_us = d_seq_calls ? (double)d_seq_ns / (double)d_seq_calls /
                                      1000.0 : 0.0;
    double chunks_avg = d_par_calls ? (double)d_chunks / (double)d_par_calls
        : 0.0;

    snprintf(buf, cap,
             "side=%c interval_ms=%llu "
             "main_wait_pct=%.1f worker_util_pct=%.1f "
             "par_calls=%llu par_items=%llu par_avg_us=%.1f "
             "par_ns_per_item=%llu chunks_avg=%.2f "
             "ray_par_calls=%llu ray_par_items=%llu ray_par_avg_us=%.1f "
             "ray_par_ns_per_item=%llu "
             "ray_seq_calls=%llu ray_seq_items=%llu ray_seq_avg_us=%.1f "
             "ray_seq_ns_per_item=%llu ray_speedup_x100=%llu",
             side ? 'B' : 'A', interval_ns / 1000000ull,
             wait_pct, util_pct,
             d_par_calls, d_par_items, par_avg_us, par_per_item, chunks_avg,
             d_ray_calls, d_ray_items, ray_avg_us, ray_par_per_item,
             d_seq_calls, d_seq_items, seq_avg_us, seq_per_item,
             speedup_x100);
}

static void par_format_phase_metrics(char *buf, size_t cap,
                                     const ParEffSnap *cur,
                                     const ParEffSnap *base,
                                     unsigned long long interval_ns, int side)
{
    unsigned long long d_applies =
        cur->phase_applies - base->phase_applies;
    unsigned long long d_items = cur->phase_items - base->phase_items;
    unsigned long long d_par_runs =
        cur->phase_par_runs - base->phase_par_runs;
    unsigned long long d_seq_runs =
        cur->phase_seq_runs - base->phase_seq_runs;
    unsigned long long d_apply_ns =
        cur->phase_apply_ns - base->phase_apply_ns;
    unsigned long long d_run_ns = cur->phase_run_ns - base->phase_run_ns;
    unsigned long long d_wake_ns = cur->phase_wake_ns - base->phase_wake_ns;
    unsigned long long d_join_ns = cur->phase_join_ns - base->phase_join_ns;
    unsigned long long d_slice_ns =
        cur->phase_slice_ns - base->phase_slice_ns;
    unsigned long long d_chunks =
        cur->phase_chunks_sum - base->phase_chunks_sum;
    unsigned long long d_seq_ns = cur->phase_seq_ns - base->phase_seq_ns;
    unsigned long long d_shadow_flushes =
        cur->phase_shadow_flushes - base->phase_shadow_flushes;
    unsigned long long d_shadow_mismatches =
        cur->phase_shadow_mismatches - base->phase_shadow_mismatches;
    unsigned long long d_shadow_seq_ns =
        cur->phase_shadow_seq_ns - base->phase_shadow_seq_ns;
    unsigned long long d_shadow_par_ns =
        cur->phase_shadow_par_ns - base->phase_shadow_par_ns;

    double avg_count = d_applies
        ? (double)d_items / (double)d_applies : 0.0;
    double chunks_avg = d_par_runs
        ? (double)d_chunks / (double)d_par_runs : 0.0;
    double apply_avg_us = d_applies
        ? (double)d_apply_ns / (double)d_applies / 1000.0 : 0.0;
    double run_avg_us = d_par_runs
        ? (double)d_run_ns / (double)d_par_runs / 1000.0 : 0.0;
    double wake_avg_us = d_par_runs
        ? (double)d_wake_ns / (double)d_par_runs / 1000.0 : 0.0;
    double join_avg_us = d_par_runs
        ? (double)d_join_ns / (double)d_par_runs / 1000.0 : 0.0;
    double slice_avg_us = d_par_runs
        ? (double)d_slice_ns / (double)d_par_runs / 1000.0 : 0.0;
    double seq_avg_us = d_seq_runs
        ? (double)d_seq_ns / (double)d_seq_runs / 1000.0 : 0.0;
    double shadow_seq_avg_us = d_shadow_flushes
        ? (double)d_shadow_seq_ns / (double)d_shadow_flushes / 1000.0 : 0.0;
    double shadow_par_avg_us = d_shadow_flushes
        ? (double)d_shadow_par_ns / (double)d_shadow_flushes / 1000.0 : 0.0;
    unsigned long long shadow_speedup_x100 =
        d_shadow_par_ns ? d_shadow_seq_ns * 100ull / d_shadow_par_ns : 0ull;

    snprintf(buf, cap,
             "side=%c interval_ms=%llu phase_applies=%llu phase_items=%llu "
             "phase_avg_count=%.1f phase_par_runs=%llu phase_seq_runs=%llu "
             "phase_chunks_avg=%.2f phase_apply_avg_us=%.1f "
             "phase_run_avg_us=%.1f phase_wake_avg_us=%.1f "
             "phase_join_avg_us=%.1f phase_slice_avg_us=%.1f "
             "phase_seq_avg_us=%.1f phase_shadow_flushes=%llu "
             "phase_shadow_mismatches=%llu "
             "phase_shadow_seq_avg_us=%.1f phase_shadow_par_avg_us=%.1f "
             "phase_shadow_speedup_x100=%llu",
             side ? 'B' : 'A', interval_ns / 1000000ull,
             d_applies, d_items, avg_count, d_par_runs, d_seq_runs,
             chunks_avg, apply_avg_us, run_avg_us, wake_avg_us,
             join_avg_us, slice_avg_us, seq_avg_us,
             d_shadow_flushes, d_shadow_mismatches,
             shadow_seq_avg_us, shadow_par_avg_us, shadow_speedup_x100);
}

static void par_emit_metrics(const ParEffSnap *cur, const ParEffSnap *base,
                             unsigned long long interval_ns, int side)
{
    char line[512];
    char phase_line[768];
    par_format_metrics(line, sizeof line, cur, base, interval_ns, side);
    par_format_phase_metrics(phase_line, sizeof phase_line, cur, base,
                             interval_ns, side);

    FILE *f = fopen(g_par_metrics_path, "w");
    if (f) {
        fprintf(f, "%s\n%s\n", line, phase_line);
        fclose(f);
    }
    if (atomic_load_explicit(&g_par_report, memory_order_relaxed)) {
        fprintf(stderr, "[parallel-eff] %s\n", line);
        fprintf(stderr, "[phase-eff] %s\n", phase_line);
    }
}

static void *par_tuner_main(void *arg)
{
    (void)arg;
    char buf[4096];
    unsigned polls = 0;
    int side = 0;
    ParEffSnap report_base, cur;
    unsigned long long report_t0;

    par_tuner_init_paths();
    par_eff_snapshot(&report_base);
    report_t0 = now_ns();

    for (;;) {
        unsigned ms = atomic_load_explicit(&g_par_poll_ms,
                                           memory_order_relaxed);
        if (ms < 1u) ms = 1u;
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = (long)ms * 1000000L;
        (void)nanosleep(&ts, NULL);
        polls++;

        int ab = atomic_load_explicit(&g_par_ab, memory_order_relaxed);
        unsigned long long now = now_ns();
        int old_side = side;
        int side_changed = 0;
        if (ab) {
            unsigned frames = atomic_load_explicit(&g_par_ab_frames,
                                                   memory_order_relaxed);
            if (frames < 1u) frames = 1u;
            if ((polls % frames) == 0u) {
                side ^= 1;
                side_changed = 1;
            }
        } else if (side != 0) {
            side = 0;
            side_changed = 1;
        }
        if (side_changed) {
            /* Close the window of the side that just ended, then start the
             * new side from the current counter values. */
            par_eff_snapshot(&cur);
            par_emit_metrics(&cur, &report_base, now - report_t0, old_side);
            par_eff_snapshot(&report_base);
            report_t0 = now;
        }

        int active_side = ab ? side : 0;
        const char *path = active_side ? g_par_alt_path : g_par_cfg_path;
        uint32_t *hash = active_side ? &g_par_alt_hash : &g_par_cfg_hash;
        if (par_read_file(path, buf, sizeof buf, hash)) {
            par_parse_text(buf);
            fprintf(stderr, "[parallel-tune] applied %s\n", path);
        }

        int dump = atomic_load_explicit(&g_par_dump, memory_order_relaxed);
        int periodic = (polls % PAR_REPORT_POLLS) == 0u &&
                       now - report_t0 >= 1000000000ull;
        if (dump || periodic) {
            if (dump)
                atomic_store_explicit(&g_par_dump, 0, memory_order_relaxed);
            par_eff_snapshot(&cur);
            par_emit_metrics(&cur, &report_base, now - report_t0,
                             active_side);
            report_base = cur;
            report_t0 = now;
        }
    }
    return NULL;
}

static void par_tuner_start(void)
{
    if (g_par_tuner_started) return;
    if (!g_par_paths_ready) par_tuner_init_paths();
    pthread_t th;
    if (pthread_create(&th, NULL, par_tuner_main, NULL) != 0) {
        fprintf(stderr, "[parallel-tune] cannot start tuner thread\n");
        return;
    }
    pthread_detach(th);
    g_par_tuner_started = 1;
    fprintf(stderr,
            "[parallel-tune] live tuner started: cfg=%s alt=%s metrics=%s "
            "poll_ms=%u\n",
            g_par_cfg_path, g_par_alt_path, g_par_metrics_path,
            atomic_load_explicit(&g_par_poll_ms, memory_order_relaxed));
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
#if WC3_PAR_LAYOUT_DEBUG
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
#endif

    tune_set_int(&g_tune.enabled, 1);
    tune_set_uint(&g_tune.threshold, threshold);
    tune_set_uint(&g_tune.chunk_limit, 0);
    tune_set_uint(&g_tune.min_chunk, RAY_MIN_CHUNK);
    tune_set_uint(&g_tune.stack_zero, RAY_STACK_ZERO);
    tune_set_uint(&g_tune.container_copy, RAY_CONTAINER_DEFAULT);
#if WC3_PAR_LAYOUT_DEBUG
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
    tune_set_uint(&g_tune.spin_us, 0u);
    tune_set_uint(&g_tune.main_spin_us, 1000u);
    tune_set_uint(&g_tune.worker_cpu_mask, 0u);
#if WC3_PAR_LAYOUT_DEBUG
    tune_set_uint(&g_tune.spin_us,
                  env_u("WC3_RAY_SPIN_US", 0u, 10000u));
    tune_set_uint(&g_tune.main_spin_us,
                  env_u("WC3_RAY_MAIN_SPIN_US", 1000u, 1000000u));
    tune_set_uint(&g_tune.worker_cpu_mask,
                  env_u("WC3_RAY_WORKER_CPU_MASK", 0u, UINT_MAX));
    g_main_last = !env_off("WC3_MAIN_LAST");
    g_ray_split_ceil = env_on("WC3_RAY_SPLIT_CEIL");
    g_vertex_split_ceil = env_on("WC3_VERTEX_SPLIT_CEIL");
    g_triangle_split_ceil = env_on("WC3_TRIANGLE_SPLIT_CEIL");
    g_weighted_split = env_on("WC3_WEIGHTED_SPLIT");
    g_reject_slow = env_on("WC3_REJECT_SLOW");
    g_main_spin_adaptive = env_on("WC3_MAIN_SPIN_ADAPTIVE");
    fprintf(stderr,
            "[parallel] main_last=%d ray_ceil=%d vertex_ceil=%d "
            "triangle_ceil=%d weighted=%d reject_slow=%d spin_adaptive=%d\n",
            g_main_last, g_ray_split_ceil, g_vertex_split_ceil,
            g_triangle_split_ceil, g_weighted_split, g_reject_slow,
            g_main_spin_adaptive);
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
    ctx->assigned_cpu = -1;

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
#if WC3_PAR_LAYOUT_DEBUG
    ctx->call_ns = 0;
#endif
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

#if WC3_PAR_LAYOUT_DEBUG
    uint64_t call_t0 = parallel_weighted_split() ? now_ns() : 0;
#endif
    WC3_DIRECT_CALL_CLEAN(job->fn, w);
#if WC3_PAR_LAYOUT_DEBUG
    if (call_t0) ctx->call_ns = now_ns() - call_t0;
#endif

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
 * saved edi/esi/ebx and the destination delta.  A chunk copies that window
 * verbatim to its private stack, runs the original loop with a short count
 * and a private 13-dword global block, then lets the original epilogue pop the
 * saved registers.  The caller executes the last chunk and merges the private
 * state into the real globals once all chunks are complete.
 */

#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_VERTEX_VALIDATE
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
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_VERTEX_VALIDATE
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
#if WC3_PAR_LAYOUT_DEBUG
    ctx->call_ns = 0;
#endif
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

#if WC3_PAR_LAYOUT_DEBUG
    uint64_t call_t0 = parallel_weighted_split() ? now_ns() : 0;
#endif
    WC3_DIRECT_CALL_CLEAN(job->fn, w);
#if WC3_PAR_LAYOUT_DEBUG
    if (call_t0) ctx->call_ns = now_ns() - call_t0;
#endif
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
 * A chunk copies the caller's frame, points ESI and [ebp+0x20] at two private
 * words above the argument area, and sets [ebp+0x14] to count*3 and
 * [ebp+0x18] to indices + start*6.  The function converts the index count back
 * to `count` triangles and lets its own epilogue pop the copied frame.  The
 * caller merges the private best/index pairs in increasing chunk order, so an
 * equal distance keeps the lowest triangle index.
 */

#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
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

/* Validation shared by the normal slow path and the debug A/B probe.  The
 * probe must not read a frame or index span that the real path would reject. */
static int triangle_args_ok(const CPU *c, uint32_t tri_count, uint32_t indices,
                            uint32_t vertex, uint32_t best_ptr,
                            uint32_t index_ptr)
{
    if (!best_ptr || !index_ptr) return 0;
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
    return 1;
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
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
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
#if WC3_PAR_LAYOUT_DEBUG
    ctx->call_ns = 0;
#endif
    if (!ctx->count) return;
    if (!prepare_triangle_ctx(ctx, job, 1)) {
        ctx->triangle_failed = 1;
        return;
    }
#if WC3_PAR_LAYOUT_DEBUG
    uint64_t call_t0 = parallel_weighted_split() ? now_ns() : 0;
#endif
    WC3_DIRECT_CALL_CLEAN(job->fn, &ctx->cpu);
#if WC3_PAR_LAYOUT_DEBUG
    if (call_t0) ctx->call_ns = now_ns() - call_t0;
#endif
    ctx->triangle_best = rd32(ctx->triangle_best_addr);
    ctx->triangle_index = rd32(ctx->triangle_index_addr);
}

#if WC3_DEBUG && WC3_DEBUG_PARALLEL
static void run_phase_slice(RayCtx *ctx, const Wc3PhaseJob *pj, unsigned idx,
                            int main_chunk);
static void *phase_worker_main(void *arg);
static void wake_phase_workers(void);
static int phase_run_job(Wc3PhaseJob *pj);
#endif

/* The caller executes the last chunk in that chunk's private context.  That
 * context is kept outside the published active set, so it never runs a chunk
 * concurrently with the caller; the context's CPU, stack, TEB, private frame
 * and output words are therefore safe to borrow.  Keeping the caller on the
 * last chunk also leaves the final chunk state in the context the vertex and
 * triangle epilogues already use. */
static inline void job_chunk_range(RayCtx *ctx, const RayJob *job,
                                   unsigned idx)
{
    ctx->start = job->chunk_start[idx];
    ctx->count = job->chunk_len[idx];
}

/* The measured weighted split can zero a rejected chunk.  Worker index still
 * equals chunk index, so a zero chunk in the middle runs and returns at once;
 * a trailing run of zero chunks is outside the active prefix and must not be
 * woken or waited for.  Merge loops skip every zero-length chunk so a skipped
 * context cannot contribute a stale result. */
static unsigned job_active_chunks(const RayJob *job, unsigned chunks,
                                  int main_last)
{
    unsigned active = main_last ? chunks - 1u : chunks;
    while (active > 0u && job->chunk_len[active - 1u] == 0u)
        active--;
    return active;
}

static inline void run_job_chunk(RayCtx *ctx, const RayJob *job)
{
    if (job->kind == RAY_JOB_VERTEX)
        run_vertex_chunk(ctx, job);
    else if (job->kind == RAY_JOB_TRIANGLE)
        run_triangle_chunk(ctx, job);
    else
        run_chunk(ctx, job);
}

#if WC3_PAR_LAYOUT_DEBUG
/* Per-chunk clocks exist only for the measured weighted split and for the
 * PARALLEL worker-utilization counters.  A debug HLE/PERF build with the
 * product equal split reads no clock per chunk; PARALLEL reads two per worker
 * chunk.  Release does not compile this function at all. */
static inline void run_job_chunk_timed(RayCtx *ctx, const RayJob *job,
                                       int main_chunk)
{
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    uint64_t busy_t0 = now_ns();
#endif
    if (parallel_weighted_split()) {
        uint64_t t0 = now_ns();
        run_job_chunk(ctx, job);
        if (main_chunk)
            record_main_timing(ctx, t0);
        else
            record_chunk_timing(ctx, t0);
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
        if (!main_chunk) par_note_busy(now_ns() - busy_t0);
#endif
        return;
    }
    run_job_chunk(ctx, job);
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    if (!main_chunk) par_note_busy(now_ns() - busy_t0);
#endif
}
#define RUN_JOB_CHUNK(ctx, job, main_chunk) \
    run_job_chunk_timed((ctx), (job), (main_chunk))
#else
#define RUN_JOB_CHUNK(ctx, job, main_chunk) run_job_chunk((ctx), (job))
#endif

static unsigned triangle_chunk_limit(void)
{
    unsigned limit = par_gate_u(&g_triangle_chunks);
    if (limit == 0 || limit > g_workers) limit = g_workers;
    return limit;
}

/* Vertex work has its own split.  It is not derived from the raycast
 * `chunks` value: an old raycast experiment must not cap the vertex path.
 * The default is all pool workers, one per allowed CPU; a product core list
 * creates exactly one worker per selected core. */
static unsigned vertex_chunk_limit(void)
{
    unsigned limit = par_gate_u(&g_vertex_chunks);
    if (limit == 0 || limit > g_workers) limit = g_workers;
    return limit;
}

/* Apply the worker affinity only when it changes.  A product core list pins
 * worker i to the i-th selected CPU (one core per worker).  Without a list,
 * the diagnostic worker_cpu_mask still applies to every worker; mask 0 keeps
 * all allowed CPUs.  The common case is one relaxed load per job. */
static void apply_worker_affinity(RayCtx *ctx)
{
    unsigned long long set = 0;

    if (ctx->assigned_cpu >= 0) {
        set = 1ull << (unsigned)ctx->assigned_cpu;
    } else {
        unsigned mask = tune_uint(&g_tune.worker_cpu_mask);
        if (mask) {
            for (unsigned cpu = 0; cpu < 32u; cpu++) {
                if (mask & (1u << cpu)) set |= 1ull << cpu;
            }
        } else if (!read_allowed_cpu_mask(&set)) {
            /* Fallback for kernels without sched_getaffinity: the old
             * online-CPU mask is still better than leaving the worker
             * unbound. */
            long online = sysconf(_SC_NPROCESSORS_ONLN);
            if (online < 1) online = 1;
            if (online > 64) online = 64;
            for (long cpu = 0; cpu < online; cpu++) set |= 1ull << cpu;
        }
    }

    if (set == ctx->applied_cpu_mask) return;
    ctx->applied_cpu_mask = set;

#if defined(__NR_sched_setaffinity)
    /* The product list only covers CPU ids 0..63, but the kernel cpumask can
     * be larger.  Pass CPU_SETSIZE bytes; a short buffer is EINVAL on kernels
     * whose NR_CPUS is above 64. */
    unsigned char kernel_mask[128];
    memset(kernel_mask, 0, sizeof kernel_mask);
    for (unsigned cpu = 0; cpu < 64u; cpu++) {
        if (set & (1ull << cpu))
            kernel_mask[cpu >> 3] |= (unsigned char)(1u << (cpu & 7u));
    }
    long rc = syscall(__NR_sched_setaffinity, 0, sizeof kernel_mask,
                      kernel_mask);
#if WC3_PAR_LAYOUT_DEBUG
    if (rc != 0) {
        fprintf(stderr,
                "[raycast] worker %d: affinity mask 0x%llX failed: %s\n",
                ctx->index, (unsigned long long)set, strerror(errno));
    }
#else
    (void)rc;
#endif
#else
    (void)set;
#if WC3_PAR_LAYOUT_DEBUG
    fprintf(stderr,
            "[raycast] worker %d: affinity unavailable, mask 0x%llX ignored\n",
            ctx->index, (unsigned long long)set);
#endif
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
    /* Pin before the first wait so an idle worker is already on its selected
     * core; the call after the wait still picks up a diagnostic mask edit. */
    apply_worker_affinity(ctx);

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

        job_chunk_range(ctx, job, idx);
        RUN_JOB_CHUNK(ctx, job, 0);
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
#if WC3_PAR_LAYOUT_DEBUG
        fprintf(stderr, "[raycast] prewarm: no lifted function at %08X\n",
                target);
#endif
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
#if WC3_PAR_LAYOUT_DEBUG
        fprintf(stderr, "[parallel] disabled by WC3_PARALLEL_GAMEPLAY\n");
#endif
        atomic_store_explicit(&g_init_ok, 1, memory_order_release);
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    /* Debug phase run: the old chunked paths are opt-in.  The worker pool
     * below is still created for the SPMD phase. */
    g_raycast_enabled = env_on("WC3_PARALLEL_RAYCAST");
    g_vertex_enabled = env_on("WC3_VERTEX");
    g_triangle_enabled = env_on("WC3_TRIANGLE");
#else
    g_raycast_enabled = !env_off("WC3_PARALLEL_RAYCAST");
    g_vertex_enabled = !env_off("WC3_VERTEX");
    g_triangle_enabled = !env_off("WC3_TRIANGLE");
#endif
    if (!g_raycast_enabled && !g_vertex_enabled && !g_triangle_enabled) {
#if WC3_PAR_LAYOUT_DEBUG
        fprintf(stderr, "[parallel] all guest paths disabled\n");
#endif
        /* The debug phase build still needs the worker pool for the SPMD
         * phase; only a build without the phase can stop here. */
#if !(WC3_DEBUG && WC3_DEBUG_PARALLEL)
        atomic_store_explicit(&g_init_ok, 1, memory_order_release);
        return;
#endif
    }

#if WC3_PAR_LAYOUT_DEBUG
    par_gate_set_u(&g_vertex_threshold,
                   env_u("WC3_VERTEX_THRESHOLD",
                         VERTEX_DEFAULT_THRESHOLD, VERTEX_MAX_COUNT));
    par_gate_set_u(&g_vertex_chunks, env_u("WC3_VERTEX_CHUNKS", 0, UINT_MAX));
    par_gate_set_u(&g_vertex_min_chunk,
                   env_u("WC3_VERTEX_MIN_CHUNK",
                         VERTEX_DEFAULT_MIN_CHUNK, VERTEX_MAX_COUNT));
    par_gate_set_u(&g_triangle_threshold,
                   env_u("WC3_TRIANGLE_THRESHOLD",
                         TRIANGLE_DEFAULT_THRESHOLD, TRIANGLE_MAX_COUNT));
    par_gate_set_u(&g_triangle_chunks,
                   env_u("WC3_TRIANGLE_CHUNKS", 0, UINT_MAX));
    par_gate_set_u(&g_triangle_min_chunk,
                   env_u("WC3_TRIANGLE_MIN_CHUNK", TRIANGLE_MIN_CHUNK,
                         TRIANGLE_MAX_COUNT));
#endif
    if (par_gate_u(&g_vertex_min_chunk) && par_gate_u(&g_vertex_min_chunk) < 4u)
        par_gate_set_u(&g_vertex_min_chunk, 4u);
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
    g_triangle_ab_enabled = !env_off("WC3_TRIANGLE_AB");
#endif

    /* The product core table is authoritative: a non-empty list sets both
     * the pool size and a one-core affinity per worker.  An empty list keeps
     * the diagnostic thread-count/auto behaviour. */
    const char *worker_cpu_env = getenv("WC3_PARALLEL_WORKER_CPUS");
    if (worker_cpu_env && *worker_cpu_env) {
        unsigned parsed = parse_worker_cpu_list(
            worker_cpu_env, g_worker_cpu, WC3_PARALLEL_MAX_WORKERS);
        g_worker_cpu_count = parsed;
        if (g_worker_cpu_count) {
            g_workers = g_worker_cpu_count;
#if WC3_PAR_LAYOUT_DEBUG
            /* A/B only: the product list is fastest-to-slowest.  Reversing it
             * restores the old slowest-first active prefix without changing
             * the selected set or the merge order. */
            if (env_on("WC3_WORKER_ORDER_REVERSE")) {
                g_worker_order_reverse = 1;
                for (unsigned i = 0; i < g_worker_cpu_count / 2u; i++) {
                    int tmp = g_worker_cpu[i];
                    g_worker_cpu[i] =
                        g_worker_cpu[g_worker_cpu_count - 1u - i];
                    g_worker_cpu[g_worker_cpu_count - 1u - i] = tmp;
                }
            }
#endif
            reserve_main_cpu_last();
        }
#if WC3_PAR_LAYOUT_DEBUG
        if (parsed == 0) {
            fprintf(stderr,
                    "[parallel] worker core list '%s' is empty; using auto\n",
                    worker_cpu_env);
        }
        if (g_worker_cpu_count) {
            fprintf(stderr, "[parallel] worker cores:");
            for (unsigned i = 0; i < g_worker_cpu_count; i++)
                fprintf(stderr, " %d", g_worker_cpu[i]);
            fprintf(stderr, " (%u worker(s), order_reverse=%d)\n", g_workers,
                    g_worker_order_reverse);
        } else {
            fprintf(stderr,
                    "[parallel] worker core list is empty; using auto\n");
        }
#endif
    }
#if WC3_PAR_LAYOUT_DEBUG
    if (!g_workers)
        g_workers = env_u("WC3_PARALLEL_RAYCAST_THREADS", 0, UINT_MAX);
#endif
    if (!g_workers) g_workers = auto_worker_count();
    if (g_workers < 1u) g_workers = 1u;
    /* RayJob stores one start/length pair per chunk in a fixed array.  The
     * product list parser and auto count cannot exceed this bound, but the
     * debug WC3_PARALLEL_RAYCAST_THREADS override can; cap it here before any
     * allocation or chunk split. */
    if (g_workers > WC3_PARALLEL_MAX_WORKERS) {
        g_workers = WC3_PARALLEL_MAX_WORKERS;
#if WC3_PAR_LAYOUT_DEBUG
        fprintf(stderr, "[parallel] worker count capped at %u\n",
                WC3_PARALLEL_MAX_WORKERS);
#endif
    }
    /* A pool with one slot cannot split a batch: every caller requires at
     * least two chunks.  Treat it exactly like the product single-core path
     * and return before allocating contexts, probing futexes, reading tuning
     * files or creating the idle worker thread.  This is also the safety net
     * for a stale environment that still says WC3_PARALLEL_GAMEPLAY=1 while
     * the selected core table has one entry. */
    if (g_workers < 2u) {
        g_raycast_enabled = 0;
        g_vertex_enabled = 0;
        g_triangle_enabled = 0;
        g_parallel_ok = 0;
#if WC3_PAR_LAYOUT_DEBUG
        fprintf(stderr,
                "[parallel] one pool slot; all guest paths stay sequential\n");
#endif
        atomic_store_explicit(&g_init_ok, 1, memory_order_release);
        return;
    }
    if (par_gate_u(&g_vertex_chunks) == 0 ||
        par_gate_u(&g_vertex_chunks) > g_workers)
        par_gate_set_u(&g_vertex_chunks, g_workers);
    if (par_gate_u(&g_triangle_chunks) == 0 ||
        par_gate_u(&g_triangle_chunks) > g_workers)
        par_gate_set_u(&g_triangle_chunks, g_workers);

    set_tuning_defaults();
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_VERTEX_VALIDATE
    const char *vertex_verify = getenv("WC3_VERTEX_VERIFY");
    g_vertex_verify = vertex_verify && *vertex_verify &&
                      !env_off("WC3_VERTEX_VERIFY");
#endif
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
    g_triangle_verify = WC3_TRIANGLE_VERIFY_DEFAULT;
    const char *triangle_verify = getenv("WC3_TRIANGLE_VERIFY");
    if (triangle_verify && *triangle_verify)
        g_triangle_verify = !env_off("WC3_TRIANGLE_VERIFY");
#endif
#if WC3_DEBUG && WC3_DEBUG_HLE
    if (!g_tune_path_ready) init_tune_path();
    load_tuning_file(0);
#endif

#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    /* Debug default: the SPMD phase is the parallel guest path; WC3_PHASE=0
     * opts out.  The old chunked paths above are opt-in in this build.
     * `parallel.cfg` can change every field below at runtime; the tuner writes
     * the atomics and phase_sync_config() copies them at the next empty
     * window. */
    int phase_en = !env_off("WC3_PHASE");
    int phase_sh = env_on("WC3_PHASE_SHADOW");
    int phase_main_last = !env_off("WC3_PHASE_MAIN_LAST");
    int phase_weighted = !env_off("WC3_PHASE_WEIGHTED");
    unsigned phase_chunks = env_u("WC3_PHASE_CHUNKS", 4u,
                                  WC3_PARALLEL_MAX_WORKERS);
    if (phase_chunks == 1u) phase_chunks = 2u;
    unsigned phase_max = env_u("WC3_PHASE_MAX", 64u,
                               WC3_PHASE_MAX_REQUESTS);
    if (phase_max < 2u) phase_max = 2u;
    unsigned phase_threshold = env_u("WC3_PHASE_THRESHOLD", 2u,
                                     WC3_PHASE_MAX_REQUESTS);
    if (phase_threshold < 2u) phase_threshold = 2u;
    unsigned phase_stack = env_u("WC3_PHASE_STACK_ZERO", RAY_STACK_ZERO,
                                 RAY_STACK_ZERO_MAX);
    if (phase_stack < RAY_STACK_ZERO_MIN) phase_stack = RAY_STACK_ZERO_MIN;
    unsigned phase_spin = env_u("WC3_PHASE_MAIN_SPIN_US", 5000u, 1000000u);

    atomic_store_explicit(&g_phase_cfg.enabled, phase_en,
                          memory_order_relaxed);
    atomic_store_explicit(&g_phase_cfg.shadow, phase_sh,
                          memory_order_relaxed);
    atomic_store_explicit(&g_phase_cfg.main_last, phase_main_last,
                          memory_order_relaxed);
    atomic_store_explicit(&g_phase_cfg.weighted, phase_weighted,
                          memory_order_relaxed);
    atomic_store_explicit(&g_phase_cfg.chunks, phase_chunks,
                          memory_order_relaxed);
    atomic_store_explicit(&g_phase_cfg.max_requests, phase_max,
                          memory_order_relaxed);
    atomic_store_explicit(&g_phase_cfg.threshold, phase_threshold,
                          memory_order_relaxed);
    atomic_store_explicit(&g_phase_cfg.stack_zero, phase_stack,
                          memory_order_relaxed);
    atomic_store_explicit(&g_phase_cfg.spin_us, phase_spin,
                          memory_order_relaxed);
    atomic_store_explicit(&g_phase_cfg.dirty, 0, memory_order_release);

    g_phase.enabled = phase_en;
    g_phase.shadow_enabled = phase_sh && !phase_en;
    g_phase.main_last = phase_main_last;
    g_phase.weighted = phase_weighted;
    g_phase.max_requests = phase_max;
    g_phase.threshold = phase_threshold;
    g_phase.chunks = phase_chunks;
    g_phase.stack_zero = phase_stack;
    g_phase.spin_us = phase_spin;
    g_phase.ran = 0;
    fprintf(stderr,
            "[phase] enabled=%d shadow_cfg=%d shadow=%d main_last=%d "
            "weighted=%d chunks=%u%s shadow_max=%u threshold=%u "
            "stack_zero=%u spin_us=%u\n",
            g_phase.enabled, phase_sh, g_phase.shadow_enabled,
            g_phase.main_last, g_phase.weighted, g_phase.chunks,
            g_phase.chunks ? "" : "(auto)", g_phase.max_requests,
            g_phase.threshold, g_phase.stack_zero, g_phase.spin_us);
#endif

    g_shared_wake = futex_probe();
    g_prev_active = 0;
#if WC3_DEBUG && WC3_DEBUG_HLE
    fprintf(stderr, "[raycast] shared futex wake %s\n",
            g_shared_wake ? "enabled" : "unavailable; using semaphores");
#endif

    g_worker = calloc((size_t)g_workers, sizeof *g_worker);
    if (!g_worker) {
#if WC3_PAR_LAYOUT_DEBUG
        fprintf(stderr, "[raycast] cannot allocate %u worker context(s)\n",
                g_workers);
#endif
        atomic_store_explicit(&g_init_ok, 1, memory_order_release);
        return;
    }

#if WC3_PAR_LAYOUT_DEBUG
    if (!alloc_ctx(&g_verify_ctx, g_workers)) {
        fprintf(stderr, "[raycast] cannot allocate verification context\n");
        free(g_worker);
        g_worker = NULL;
        atomic_store_explicit(&g_init_ok, 1, memory_order_release);
        return;
    }
#endif
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    /* The sequential oracle is also the shadow replay context and the
     * fallback for a live-enabled phase.  Allocate it unconditionally so a
     * `phase=1` or `phase_shadow=1` write to parallel.cfg can take effect
     * without a relaunch. */
    if (alloc_ctx(&g_phase_seq_ctx, g_workers)) {
        g_phase_seq_ctx_ready = 1;
        uint32_t bytes =
            WC3_PHASE_MAX_REQUESTS * WC3_PHASE_SLOT_STRIDE;
        g_phase_container_base = hle_guest_alloc(bytes);
        if (g_phase_container_base &&
            hle_android_guest_ok(g_phase_container_base, bytes)) {
            memset(guest_host_ptr(g_phase_container_base), 0, bytes);
            g_phase_container_ready = 1;
        } else {
            fprintf(stderr,
                    "[phase] container snapshot allocation failed\n");
            g_phase.enabled = 0;
            g_phase.shadow_enabled = 0;
            atomic_store_explicit(&g_phase_cfg.enabled, 0,
                                  memory_order_relaxed);
            atomic_store_explicit(&g_phase_cfg.shadow, 0,
                                  memory_order_relaxed);
            atomic_store_explicit(&g_phase_cfg.dirty, 0,
                                  memory_order_release);
        }
    } else {
        fprintf(stderr, "[phase] sequential context allocation failed\n");
        g_phase.enabled = 0;
        g_phase.shadow_enabled = 0;
        atomic_store_explicit(&g_phase_cfg.enabled, 0,
                              memory_order_relaxed);
        atomic_store_explicit(&g_phase_cfg.shadow, 0,
                              memory_order_relaxed);
        atomic_store_explicit(&g_phase_cfg.dirty, 0, memory_order_release);
    }
#endif
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
    if (g_triangle_ab_enabled && g_triangle_enabled) {
        if (alloc_ctx(&g_triangle_ab_ctx, g_workers)) {
            g_triangle_ab_ctx_ready = 1;
        } else {
            fprintf(stderr, "[triangle] ab context allocation failed\n");
        }
    }
#endif

    for (unsigned i = 0; i < g_workers; i++) {
        if (!alloc_ctx(&g_worker[i], i)) {
#if WC3_PAR_LAYOUT_DEBUG
            fprintf(stderr, "[raycast] cannot allocate worker %u context\n", i);
#endif
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

    /* The main-last layout never publishes the last context in the active
     * set: the caller uses it for the last chunk.  Its worker thread is
     * therefore pure overhead.  Keep the context (the caller needs the CPU,
     * stack and TEB) but skip the pthread.  The debug all-worker A/B layout
     * still creates every thread because chunks may equal g_workers there. */
    unsigned thread_count = g_workers;
    if (parallel_main_last() && g_workers >= 2u)
        thread_count = g_workers - 1u;
#if WC3_PAR_LAYOUT_DEBUG
    if (thread_count != g_workers) {
        fprintf(stderr,
                "[parallel] last pool slot is caller-only; created %u of %u "
                "worker threads\n", thread_count, g_workers);
    }
#endif

    for (unsigned i = 0; i < g_workers; i++) {
        g_worker[i].index = (int)i;
        if (i < g_worker_cpu_count)
            g_worker[i].assigned_cpu = g_worker_cpu[i];
    }
    for (unsigned i = 0; i < thread_count; i++) {
        if (pthread_create(&g_worker[i].thread, NULL, worker_main,
                           &g_worker[i]) != 0) {
#if WC3_PAR_LAYOUT_DEBUG
            fprintf(stderr, "[raycast] cannot create worker %u\n", i);
#endif
            goto fail;
        }
        g_worker[i].thread_started = 1;
    }

#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    /* Dedicated phase pool: the phase communicates only through its own
     * mailbox and does not use the generic job path.  It is allocated
     * unconditionally so a live phase=0 -> phase=1 tune change does not
     * need a relaunch. */
    g_phase_ctx = calloc((size_t)g_workers, sizeof *g_phase_ctx);
    g_phase_thread = calloc((size_t)g_workers, sizeof *g_phase_thread);
    if (!g_phase_ctx || !g_phase_thread) {
        fprintf(stderr, "[phase] worker pool allocation failed\n");
        goto fail;
    }
    for (unsigned i = 0; i < g_workers; i++) {
        if (!alloc_ctx(&g_phase_ctx[i], i)) {
            fprintf(stderr,
                    "[phase] cannot allocate worker %u context\n", i);
            goto fail;
        }
        g_phase_ctx[i].assigned_cpu = g_worker[i].assigned_cpu;
    }
    for (unsigned i = 0; i < g_workers; i++) {
        if (pthread_create(&g_phase_thread[i], NULL, phase_worker_main,
                           &g_phase_ctx[i]) != 0) {
            fprintf(stderr, "[phase] cannot create worker %u\n", i);
            goto fail;
        }
    }
    g_phase_thread_count = g_workers;
    g_phase_pool_ready = 1;
    fprintf(stderr, "[phase] mailbox pool: %u worker(s)\n",
            g_phase_thread_count);
#endif

    g_parallel_ok = 1;
    atomic_store_explicit(&g_init_ok, 1, memory_order_release);
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    par_tuner_start();
#endif
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
    fprintf(stderr, "[triangle] sequential oracle %s\n",
            g_triangle_verify ? "on" : "off (set WC3_TRIANGLE_VERIFY=1)");
#endif
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
    if (g_triangle_ab_ctx_ready && g_workers >= 2u) {
        fprintf(stderr,
                "[triangle] ab probe armed: main A/B %u samples, nested "
                "sequential sample, %u-triangle chunks\n",
                TRIANGLE_AB_SAMPLES, TRIANGLE_AB_MIN_CHUNK);
    } else if (g_triangle_ab_ctx_ready) {
        fprintf(stderr,
                "[triangle] ab probe unavailable (need at least 2 workers)\n");
    } else if (!g_triangle_ab_enabled) {
        fprintf(stderr, "[triangle] ab probe off (WC3_TRIANGLE_AB=0)\n");
    } else if (!g_triangle_enabled) {
        fprintf(stderr,
                "[triangle] ab probe off (triangle path disabled)\n");
    } else {
        fprintf(stderr, "[triangle] ab probe unavailable\n");
    }
#endif
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_VERTEX_VALIDATE && \
    WC3_PARALLEL_TRIANGLE_VALIDATE
    fprintf(stderr, "[parallel] verify vertex=%u triangle=%u raycast=%u\n",
            g_vertex_verify ? 1u : 0u, g_triangle_verify ? 1u : 0u,
            tune_int(&g_tune.verify) ? 1u : 0u);
#endif
#if WC3_DEBUG && WC3_DEBUG_HLE
    fprintf(stderr,
            "[parallel] pool: %u context(s), %u thread(s), "
            "%u selected core(s), %ld online CPU(s), "
            "ray threshold %u, ray min_chunk %u, vertex threshold %u, "
            "vertex chunks %u, vertex min_chunk %u, triangle threshold %u, "
            "triangle chunks %u, triangle min_chunk %u, verify %s\n",
            g_workers, thread_count, g_worker_cpu_count,
            sysconf(_SC_NPROCESSORS_ONLN),
            tune_uint(&g_tune.threshold), tune_uint(&g_tune.min_chunk),
            par_gate_u(&g_vertex_threshold),
            par_gate_u(&g_vertex_chunks), par_gate_u(&g_vertex_min_chunk),
            par_gate_u(&g_triangle_threshold),
            par_gate_u(&g_triangle_chunks), par_gate_u(&g_triangle_min_chunk),
            tune_int(&g_tune.verify) ? "on" : "off");
    fprintf(stderr, "[raycast] tune file: %s (%s)\n", g_tune_path,
            g_tune_file_enabled
                ? "enabled by WC3_RAY_TUNE"
                : "ignored; set WC3_RAY_TUNE=1 to load");
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_VERTEX_VALIDATE
    fprintf(stderr, "[vertex] bit-exact verification %s\n",
            g_vertex_verify ? "on" : "off (set WC3_VERTEX_VERIFY=1)");
#endif
    log_tuning("init");
#endif
    return;

fail:
    atomic_store_explicit(&g_shutdown, 1, memory_order_release);
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    atomic_store_explicit(&g_phase_shutdown, 1, memory_order_release);
    if (g_phase_thread_count) {
        wake_phase_workers();
        for (unsigned i = 0; i < g_phase_thread_count; i++)
            pthread_join(g_phase_thread[i], NULL);
    }
    if (g_phase_ctx) {
        for (unsigned i = 0; i < g_phase_thread_count; i++) {
            if (g_phase_ctx[i].wake_init) {
                sem_destroy(&g_phase_ctx[i].wake);
                g_phase_ctx[i].wake_init = 0;
            }
        }
    }
    free(g_phase_ctx);
    free(g_phase_thread);
    g_phase_ctx = NULL;
    g_phase_thread = NULL;
    g_phase_thread_count = 0;
    g_phase_pool_ready = 0;
#endif
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
#if WC3_PAR_LAYOUT_DEBUG
    if (g_verify_ctx.wake_init) {
        sem_destroy(&g_verify_ctx.wake);
        g_verify_ctx.wake_init = 0;
    }
#endif
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    if (g_phase_seq_ctx.wake_init) {
        sem_destroy(&g_phase_seq_ctx.wake);
        g_phase_seq_ctx.wake_init = 0;
    }
    g_phase_seq_ctx_ready = 0;
    g_phase_container_ready = 0;
#endif
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
    if (g_triangle_ab_ctx.wake_init) {
        sem_destroy(&g_triangle_ab_ctx.wake);
        g_triangle_ab_ctx.wake_init = 0;
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

static void merge_results(RayCtx *out, const RayJob *job, unsigned n)
{
    unsigned i;
    out->hit = 0;
    out->dist_bits = 0;
    out->pos[0] = out->pos[1] = out->pos[2] = 0;

    for (i = 0; i < n; i++) {
        if (!job->chunk_len[i]) continue;
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
#if WC3_PAR_LAYOUT_DEBUG
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
            "last_total=%u last_chunks=%u threshold=%u min_chunk=%u "
            "chunks=%u stack_zero=%u copy=%u spin_us=%u main_spin_us=%u "
            "cpu_mask=0x%X total_n=%llu total_avg=%llu total_max=%u "
            "total_hist=[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]\n",
            d_calls,
            g_parallel_calls, d_parallel,
            g_fallback_calls, d_fallback,
            par_avg, fb_avg,
            g_last_total, g_last_chunks,
            tune_uint(&g_tune.threshold),
            tune_uint(&g_tune.min_chunk),
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
static int wait_for_workers(unsigned workers, uint64_t seq,
                            unsigned main_spin_us)
{
    if (!workers) return 0;

#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    uint64_t t0 = now_ns();
#endif

    if (main_spin_us) {
        uint64_t deadline = now_ns() + (uint64_t)main_spin_us * 1000ull;
        unsigned loops = 0;
        while (!workers_done(workers, seq)) {
            if ((++loops & 0x7Fu) == 0 && now_ns() >= deadline) break;
            cpu_relax();
        }
        if (workers_done(workers, seq)) {
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
            par_note_wait(now_ns() - t0);
#endif
            return 0;
        }
    }

    while (!workers_done(workers, seq)) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = RAY_WAIT_SLEEP_NS;
        (void)nanosleep(&ts, NULL);
    }
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    par_note_wait(now_ns() - t0);
#endif
    return 1;
}

/* Debug-only adaptive main-spin budget.  A wait that finished inside the
 * spin did not need the whole budget; a wait that fell back to sleeping paid
 * a wakeup and should try a little longer next time.  This is a measured
 * control loop, not a per-scene prediction. */
#if WC3_PAR_LAYOUT_DEBUG
static void note_main_wait(unsigned workers, int slept)
{
    if (!workers || !atomic_load_explicit(&g_main_spin_adaptive,
                                          memory_order_relaxed)) return;
    unsigned cur = tune_uint(&g_tune.main_spin_us);
    if (slept) {
        if (cur < 2000u) cur *= 2u;
    } else {
        if (cur > 50u) cur = cur * 3u / 4u;
    }
    tune_set_uint(&g_tune.main_spin_us, cur);
}
#else
static void note_main_wait(unsigned workers, int slept)
{
    (void)workers;
    (void)slept;
}
#endif

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

#if WC3_DEBUG && WC3_DEBUG_PARALLEL
/* ------------------------------------------------------------------ */
/* Phase execution.                                                    */
/*                                                                    */
/* The phase is one fork-join: publish the request array and split,    */
/* wake the active workers, run the last slice on the caller, wait for */
/* done_seq, then let the caller apply the scratch results.  Workers   */
/* never write guest memory; they only fill r->hit/dist/pos.           */
/* ------------------------------------------------------------------ */

static void phase_set_split(Wc3PhaseJob *pj, unsigned chunks)
{
    uint32_t total = pj->total;
    if (chunks < 1u) chunks = 1u;
    if (chunks > total) chunks = total;
    if (chunks < 1u) chunks = 1u;
    pj->chunks = chunks;

    uint32_t base = total / chunks;
    uint32_t rem = total % chunks;
    uint32_t start = 0;
    for (unsigned i = 0; i < chunks; i++) {
        uint32_t len = base + (i < rem ? 1u : 0u);
        pj->chunk_start[i] = start;
        pj->chunk_len[i] = len;
        start += len;
    }
}

#if WC3_PAR_LAYOUT_DEBUG
/* Measured weighted split for T>2.  The existing RayJob split is reused so
 * the phase and the per-call pool share one allocation policy.  The caller
 * must have already clamped chunks to the worker/thread limit. */
static void phase_set_weighted_split(Wc3PhaseJob *pj, unsigned chunks)
{
    uint64_t weights[WC3_PARALLEL_MAX_WORKERS];
    RayJob j;

    parallel_chunk_weights(weights, chunks);
    memset(&j, 0, sizeof j);
    job_set_weighted_split(&j, pj->total, chunks, weights);
    memcpy(pj->chunk_start, j.chunk_start, chunks * sizeof j.chunk_start[0]);
    memcpy(pj->chunk_len, j.chunk_len, chunks * sizeof j.chunk_len[0]);
    pj->chunks = chunks;
}
#else
static void phase_set_weighted_split(Wc3PhaseJob *pj, unsigned chunks)
{
    phase_set_split(pj, chunks);
}
#endif

static unsigned phase_active_chunks(const Wc3PhaseJob *pj)
{
    unsigned chunks = pj->chunks;
    unsigned active = pj->main_last ? chunks - 1u : chunks;
    while (active > 0u && pj->chunk_len[active - 1u] == 0u)
        active--;
    return active;
}

/* The main-last pool layout creates one thread fewer than g_workers.  A phase
 * that opts out of main participation must not publish a chunk for that
 * caller-only context. */
static unsigned phase_worker_threads(void)
{
    unsigned n = g_workers;
    if (parallel_main_last() && n >= 2u) n--;
    return n;
}

/* Prepare one request on a private context.  The request already owns a
 * private guest container snapshot, so execution reads it directly and never
 * touches the live container.  The hit path writes every output word before
 * returning; a miss leaves the outputs unread, so no pre-clear is needed.
 * The full CPU copy is deliberate: the shadow must reproduce the sequential
 * call exactly, and a product phase can later replace it with a reset of only
 * the fields the lifted chain is audited to dirty. */
static int phase_prepare_request(RayCtx *ctx, const Wc3PhaseJob *pj,
                                 uint32_t i)
{
    const Wc3PhaseRequest *r = &pj->req[i];
    const CPU *src = pj->req_cpu ? &pj->req_cpu[i] : pj->cpu;

    if (!src) return 0;

    if (pj->stack_zero) {
        if (ctx->stack_top <= ctx->stack_base + pj->stack_zero) return 0;
        memset(guest_host_ptr(ctx->stack_top - pj->stack_zero), 0,
               pj->stack_zero);
    }

    prepare_base(ctx, src, 1);
    ctx->cpu.ecx = r->container;
    push_arg(&ctx->cpu, r->user_ctx);
    push_arg(&ctx->cpu, ctx->out_dist);
    push_arg(&ctx->cpu, ctx->out_pos);
    push_arg(&ctx->cpu, r->in2);
    push_arg(&ctx->cpu, r->in1);
    push_arg(&ctx->cpu, RAY_SENTINEL);
    return 1;
}

static int phase_run_request(RayCtx *ctx, const Wc3PhaseJob *pj, uint32_t i)
{
    if (!phase_prepare_request(ctx, pj, i)) return 0;

    WC3_DIRECT_CALL_CLEAN(pj->fn, &ctx->cpu);

    Wc3PhaseRequest *r = &pj->req[i];
    r->hit = (ctx->cpu.eax != 0) ? 1u : 0u;
    if (r->hit) {
        r->dist_bits = rd32(ctx->out_dist);
        r->pos[0] = rd32(ctx->out_pos + 0u);
        r->pos[1] = rd32(ctx->out_pos + 4u);
        r->pos[2] = rd32(ctx->out_pos + 8u);
    }
    return 1;
}

static void run_phase_slice(RayCtx *ctx, const Wc3PhaseJob *pj, unsigned idx,
                            int main_chunk)
{
    uint32_t start = pj->chunk_start[idx];
    uint32_t end = start + pj->chunk_len[idx];
    int saved_active = t_ray_active;

    (void)main_chunk;
    ctx->phase_failed = 0;
    ctx->phase_slice_ns = 0;
    ctx->phase_count = end - start;
    t_ray_active = 1;

    uint64_t t0 = now_ns();
    for (uint32_t i = start; i < end; i++) {
        if (!phase_run_request(ctx, pj, i)) {
            ctx->phase_failed = 1;
            break;
        }
    }
    ctx->phase_slice_ns = now_ns() - t0;
    t_ray_active = saved_active;
}

/* Wait for a new mailbox sequence.  The futex word is the low 32 bits of
 * the mailbox (the sequence), so every publish changes the word the kernel
 * compares.  A done bit in the high half does not need to wake this thread;
 * a spurious wake is still safe because the loop re-checks the sequence. */
static void phase_worker_wait(RayCtx *ctx, uint64_t word)
{
    (void)ctx;
    uint64_t deadline = now_ns() + 100000ull;
    while (atomic_load_explicit(&g_phase_mailbox,
                                memory_order_acquire) == word) {
        if (atomic_load_explicit(&g_phase_shutdown, memory_order_acquire))
            return;
        if (now_ns() >= deadline) break;
        cpu_relax();
    }
    if (atomic_load_explicit(&g_phase_mailbox, memory_order_acquire) != word)
        return;
#if defined(__NR_futex)
    if (g_shared_wake) {
        (void)syscall(__NR_futex, (int *)(uintptr_t)&g_phase_mailbox,
                      WC3_FUTEX_WAIT_PRIVATE, (int)(uint32_t)word,
                      NULL, NULL, 0);
        return;
    }
#endif
    while (atomic_load_explicit(&g_phase_mailbox,
                                memory_order_acquire) == word) {
        if (atomic_load_explicit(&g_phase_shutdown, memory_order_acquire))
            return;
        cpu_relax();
    }
}

static void wake_phase_workers(void)
{
#if defined(__NR_futex)
    if (g_shared_wake)
        (void)syscall(__NR_futex, (int *)(uintptr_t)&g_phase_mailbox,
                      WC3_FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
#else
    (void)g_shared_wake;
#endif
}

static void *phase_worker_main(void *arg)
{
    RayCtx *ctx = (RayCtx *)arg;
    char name[16];
    snprintf(name, sizeof name, "wc3-phase-%d", ctx->index);
    (void)pthread_setname_np(pthread_self(), name);
    apply_worker_affinity(ctx);

    for (;;) {
        if (atomic_load_explicit(&g_phase_shutdown, memory_order_acquire))
            break;
        uint64_t word = atomic_load_explicit(&g_phase_mailbox,
                                             memory_order_acquire);
        uint32_t seq = (uint32_t)word;
        if (seq == ctx->phase_seen) {
            phase_worker_wait(ctx, word);
            continue;
        }
        ctx->phase_seen = seq;
        const Wc3PhaseJob *pj = g_phase_job;
        uint32_t active = pj->active;
        if ((unsigned)ctx->index >= active) {
            atomic_store_explicit(&ctx->phase_ack, seq, memory_order_release);
            continue;
        }
        run_phase_slice(ctx, pj, (unsigned)ctx->index, 0);
        uint32_t bit = 1u << ctx->index;
        if (active == 1u) {
            atomic_store_explicit(&g_phase_mailbox,
                                  (uint64_t)seq | ((uint64_t)bit << 32),
                                  memory_order_release);
        } else {
            atomic_fetch_or_explicit(&g_phase_mailbox,
                                     (uint64_t)bit << 32,
                                     memory_order_release);
        }
        atomic_store_explicit(&ctx->phase_ack, seq, memory_order_release);
    }
    return NULL;
}

static int phase_run_job(Wc3PhaseJob *pj)
{
    if (!ready()) return 0;
    if (!g_phase.enabled && !g_phase.shadow_busy) return 0;
    if (!g_phase_pool_ready) return 0;
    if (pj->total < 2u) return 0;

    uint64_t run_t0 = now_ns();
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &g_phase_busy, &expected, 1,
            memory_order_acq_rel, memory_order_acquire))
        return 0;

    unsigned chunks = g_phase.chunks;
    if (!chunks) {
        chunks = pj->total < g_workers ? pj->total : g_workers;
        if (chunks > WC3_PHASE_AUTO_CHUNKS) chunks = WC3_PHASE_AUTO_CHUNKS;
    }
    if (chunks < 2u) chunks = 2u;
    unsigned max_chunks = pj->main_last ? g_workers : phase_worker_threads();
    if (max_chunks < 2u) {
        atomic_store_explicit(&g_phase_busy, 0, memory_order_release);
        return 0;
    }
    if (chunks > max_chunks) chunks = max_chunks;
    if (g_phase.weighted)
        phase_set_weighted_split(pj, chunks);
    else
        phase_set_split(pj, chunks);

    unsigned active = phase_active_chunks(pj);
    if (!active || active > WC3_PHASE_MAILBOX_MAX_WORKERS) {
        atomic_store_explicit(&g_phase_busy, 0, memory_order_release);
        return 0;
    }

    pj->active = active;
    pj->failed = 0;

    int saved_active = t_ray_active;
    t_ray_active = 1;

    /* One publish: publish the job pointer, then release-store the mailbox
     * sequence.  The job lives on the caller's frame until this function
     * returns, and the caller waits for every active worker before returning,
     * so the pointer is valid for the whole slice.  The sequence is the
     * low 32 bits and the futex word; the done mask is the high 32 bits. */
    g_phase_job = pj;
    uint32_t seq = ++g_phase_seq;
    atomic_store_explicit(&g_phase_mailbox, (uint64_t)seq,
                          memory_order_release);

    uint64_t t1 = now_ns();
    wake_phase_workers();
    uint64_t t2 = now_ns();

    if (pj->main_last) {
        RayCtx *main_ctx = &g_worker[chunks - 1u];
        run_phase_slice(main_ctx, pj, chunks - 1u, 1);
    }

    uint32_t mask = active >= 32u ? 0xFFFFFFFFu : ((1u << active) - 1u);
    uint64_t deadline = now_ns() + (uint64_t)g_phase.spin_us * 1000ull;
    unsigned loops = 0;
    for (;;) {
        uint64_t w = atomic_load_explicit(&g_phase_mailbox,
                                          memory_order_acquire);
        if ((uint32_t)w == seq && (uint32_t)(w >> 32) == mask)
            break;
        if (g_phase.spin_us &&
            ((++loops & 0x7Fu) != 0u || now_ns() < deadline)) {
            cpu_relax();
            continue;
        }
        break;
    }
    while (1) {
        uint64_t w = atomic_load_explicit(&g_phase_mailbox,
                                          memory_order_acquire);
        if ((uint32_t)w == seq && (uint32_t)(w >> 32) == mask)
            break;
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = RAY_WAIT_SLEEP_NS;
        (void)nanosleep(&ts, NULL);
    }

    /* The done mask covers only active workers.  An inactive worker still has
     * to read the published job pointer to learn that it is inactive, and the
     * publisher does not wait for it in the mask loop above.  Wait for every
     * worker to acknowledge the sequence before the caller's job frame can go
     * away; otherwise a descheduled inactive worker can dereference a stale
     * pointer after the frame has been reused. */
    for (;;) {
        int all = 1;
        for (unsigned i = 0; i < g_phase_thread_count; i++) {
            if (atomic_load_explicit(&g_phase_ctx[i].phase_ack,
                                     memory_order_acquire) != seq) {
                all = 0;
                break;
            }
        }
        if (all)
            break;
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = RAY_WAIT_SLEEP_NS;
        (void)nanosleep(&ts, NULL);
    }
    g_phase_job = NULL;
    uint64_t t3 = now_ns();

    int failed = 0;
    for (unsigned i = 0; i < active; i++) {
        if (!pj->chunk_len[i]) continue;
        if (g_phase_ctx[i].phase_failed) {
            failed = 1;
            break;
        }
    }
    if (!failed && pj->main_last && chunks) {
        if (g_worker[chunks - 1u].phase_failed)
            failed = 1;
    }
    pj->failed = failed;

    g_phase.last_wake_ns = t2 - t1;
    g_phase.last_join_ns = t3 - t2;
    g_phase.last_run_ns = t3 - run_t0;
    g_phase.last_max_slice_ns = 0;
    for (unsigned i = 0; i < active; i++) {
        if (g_phase_ctx[i].phase_slice_ns > g_phase.last_max_slice_ns)
            g_phase.last_max_slice_ns = g_phase_ctx[i].phase_slice_ns;
    }
    if (pj->main_last && chunks) {
        uint64_t main_ns = g_worker[chunks - 1u].phase_slice_ns;
        if (main_ns > g_phase.last_max_slice_ns)
            g_phase.last_max_slice_ns = main_ns;
    }

    atomic_store_explicit(&g_phase_busy, 0, memory_order_release);
    t_ray_active = saved_active;
    return !failed;
}

/* Product-facing debug API.  A future LiftPass collect/apply pair calls
 * wc3_phase_reserve() before the stateful prefix and wc3_phase_commit()
 * after it.  Reserving first keeps the failure path safe: when no slot is
 * available the collect half runs the original lifted function instead of
 * running the prefix and only then discovering that the phase cannot take
 * the request.  The shadow below uses the same storage but does not go
 * through this API. */
int wc3_phase_reserve(void)
{
    phase_sync_config();
    if (!g_phase.enabled) return 0;
    if (!atomic_load_explicit(&g_phase_cfg.enabled, memory_order_relaxed))
        return 0;
    if (t_ray_active) return 0;
    if (atomic_load_explicit(&g_phase_busy, memory_order_acquire)) return 0;
    if (atomic_load_explicit(&g_job_addr, memory_order_acquire)) return 0;
    if (g_phase.count + g_phase.reserved >= WC3_PHASE_MAX_REQUESTS)
        return 0;
    g_phase.reserved++;
    return 1;
}

static void phase_commit_fail(const char *why)
{
    fprintf(stderr, "[phase] commit failed: %s\n", why);
    abort();
}

void wc3_phase_commit(CPU *c, wc3_lifted_fn fn, uint32_t container,
                      uint32_t array, uint32_t count, uint32_t in1,
                      uint32_t in2, uint32_t out_pos, uint32_t out_dist,
                      uint32_t user_ctx, uint32_t frame_va, uint32_t frame_len)
{
    Wc3PhaseRequest *r;
    uint32_t index;
    uint64_t bytes;

    if (!g_phase.reserved) phase_commit_fail("no reserved slot");
    if (!g_phase_container_ready)
        phase_commit_fail("container snapshot unavailable");
    index = g_phase.count + g_phase.reserved - 1u;
    if (index >= WC3_PHASE_MAX_REQUESTS) phase_commit_fail("slot out of range");
    if (!c || !fn || !container || count == 0u || count > 0x100000u)
        phase_commit_fail("bad request");
    if (!hle_android_guest_ok(container, WC3_PHASE_CONTAINER_SNAP))
        phase_commit_fail("bad container range");

    bytes = (uint64_t)count * 4u;
    if (!array || bytes > 0xFFFFFFFFu ||
        !hle_android_guest_ok(array, (uint32_t)bytes))
        phase_commit_fail("bad array range");
    if (!out_pos || !out_dist) phase_commit_fail("missing outputs");
    if (!hle_android_guest_ok(out_pos, 12u) ||
        !hle_android_guest_ok(out_dist, 4u))
        phase_commit_fail("bad output range");
    if (frame_len > WC3_PHASE_FRAME_MAX) phase_commit_fail("frame too large");
    if (frame_len && (!frame_va ||
                      !hle_android_guest_ok(frame_va, frame_len)))
        phase_commit_fail("bad frame range");

    r = &g_phase.req[index];
    r->index = index;
    r->array = array;
    r->count = count;
    r->out_pos = out_pos;
    r->out_dist = out_dist;
    r->in1 = in1;
    r->in2 = in2;
    r->user_ctx = user_ctx;
    r->frame_va = frame_va;
    r->frame_len = frame_len;
    if (frame_len)
        memcpy(r->frame, guest_host_ptr(frame_va), frame_len);

    /* One copy at collect time replaces the per-run copy into a worker
     * scratch.  The slot is private to this request and is read-only while
     * the phase runs. */
    uint32_t slot = phase_container_slot(index);
    memcpy(guest_host_ptr(slot), guest_host_ptr(container),
           WC3_PHASE_CONTAINER_SNAP);
    wr32(slot + 0x110u, count);
    wr32(slot + 0x114u, array);
    r->container = slot;

    g_phase_cpu[index] = *c;
    g_phase_level_count[index] = 0;
    g_phase.fn = fn;
    g_phase.reserved--;
    g_phase.count++;
    g_phase.captures++;
    g_phase.ran = 0;
}

int wc3_phase_run(void)
{
    Wc3PhaseJob pj;

    if (!g_phase.enabled || g_phase.count < g_phase.threshold) return 0;
    if (g_phase.reserved) return 0;
    if (atomic_load_explicit(&g_phase_busy, memory_order_acquire)) return 0;

    memset(&pj, 0, sizeof pj);
    pj.req = g_phase.req;
    pj.cpu = NULL;
    pj.req_cpu = g_phase_cpu;
    pj.total = g_phase.count;
    pj.fn = g_phase.fn;
    pj.stack_zero = g_phase.stack_zero;
    pj.main_last = g_phase.main_last;

    int ok = phase_run_job(&pj);
    g_phase.ran = ok;
    if (ok) {
        par_note_phase_par(pj.chunks, g_phase.last_run_ns,
                           g_phase.last_wake_ns, g_phase.last_join_ns,
                           g_phase.last_max_slice_ns);
    }
    return ok;
}

const Wc3PhaseRequest *wc3_phase_result(unsigned index)
{
    if (!g_phase.ran || index >= g_phase.count) return NULL;
    return &g_phase.req[index];
}

const CPU *wc3_phase_request_cpu(unsigned index)
{
    if (!g_phase.ran || index >= g_phase.count) return NULL;
    return &g_phase_cpu[index];
}

void wc3_phase_restore_frame(unsigned index)
{
    const Wc3PhaseRequest *r;

    if (!g_phase.ran || index >= g_phase.count) return;
    r = &g_phase.req[index];
    if (r->frame_len && r->frame_len <= WC3_PHASE_FRAME_MAX &&
        r->frame_va && hle_android_guest_ok(r->frame_va, r->frame_len))
        memcpy(guest_host_ptr(r->frame_va), r->frame, r->frame_len);
}

void wc3_phase_commit_level(CPU *c, unsigned level, uint32_t frame_va,
                            uint32_t frame_len)
{
    Wc3PhaseLevel *lv;
    uint32_t index;

    if (!g_phase.enabled) phase_commit_fail("level commit with phase off");
    if (!g_phase.count) phase_commit_fail("level commit without request");
    if (level == 0u || level >= WC3_PHASE_MAX_LEVELS)
        phase_commit_fail("bad level");
    index = g_phase.count - 1u;
    if (index >= WC3_PHASE_MAX_REQUESTS)
        phase_commit_fail("level slot out of range");
    if (!c) phase_commit_fail("null level cpu");
    if (frame_len > WC3_PHASE_FRAME_MAX)
        phase_commit_fail("level frame too large");
    if (frame_len && (!frame_va || !hle_android_guest_ok(frame_va, frame_len)))
        phase_commit_fail("bad level frame range");

    lv = &g_phase_level[index][level];
    lv->cpu = *c;
    lv->frame_va = frame_va;
    lv->frame_len = frame_len;
    if (frame_len)
        memcpy(lv->frame, guest_host_ptr(frame_va), frame_len);
    if (g_phase_level_count[index] <= level)
        g_phase_level_count[index] = (uint8_t)(level + 1u);
}

const CPU *wc3_phase_request_cpu_level(unsigned index, unsigned level)
{
    if (!g_phase.ran || index >= g_phase.count) return NULL;
    if (level == 0u) return &g_phase_cpu[index];
    if (level >= WC3_PHASE_MAX_LEVELS) return NULL;
    if (level >= g_phase_level_count[index]) return NULL;
    return &g_phase_level[index][level].cpu;
}

void wc3_phase_restore_frame_level(unsigned index, unsigned level)
{
    const Wc3PhaseLevel *lv;

    if (!g_phase.ran || index >= g_phase.count) return;
    if (level == 0u) {
        wc3_phase_restore_frame(index);
        return;
    }
    if (level >= WC3_PHASE_MAX_LEVELS) return;
    if (level >= g_phase_level_count[index]) return;
    lv = &g_phase_level[index][level];
    if (lv->frame_len && lv->frame_len <= WC3_PHASE_FRAME_MAX &&
        lv->frame_va && hle_android_guest_ok(lv->frame_va, lv->frame_len))
        memcpy(guest_host_ptr(lv->frame_va), lv->frame, lv->frame_len);
}

unsigned wc3_phase_count(void)
{
    return g_phase.enabled ? g_phase.count : 0u;
}

int wc3_phase_window_full(void)
{
    if (!g_phase.enabled) return 0;
    return g_phase.count + g_phase.reserved >= WC3_PHASE_MAX_REQUESTS;
}

int wc3_phase_arm(int on)
{
    int prev = t_phase_armed;
    t_phase_armed = on ? 1 : 0;
    return prev;
}

int wc3_phase_armed(void)
{
    return t_phase_armed;
}

int wc3_phase_run_or_seq(void)
{
    Wc3PhaseJob pj;

    if (!g_phase.enabled || !g_phase.count) return 0;
    if (wc3_phase_run()) return 1;
    if (!g_phase_seq_ctx_ready || !g_phase_container_ready) return 0;

    memset(&pj, 0, sizeof pj);
    pj.req = g_phase.req;
    pj.cpu = NULL;
    pj.req_cpu = g_phase_cpu;
    pj.total = g_phase.count;
    pj.fn = g_phase.fn;
    pj.stack_zero = g_phase.stack_zero;
    pj.main_last = g_phase.main_last;

    uint64_t seq_t0 = now_ns();
    for (uint32_t i = 0; i < pj.total; i++) {
        if (!phase_run_request(&g_phase_seq_ctx, &pj, i))
            return 0;
    }
    par_note_phase_seq(now_ns() - seq_t0);
    g_phase.ran = 1;
    return 1;
}

int wc3_phase_apply_all(CPU *c, Wc3PhaseApplyFn apply)
{
    CPU loop;
    uint32_t n;
    uint32_t applied;
    uint32_t i;

    if (!g_phase.enabled || !g_phase.count) return 0;
    if (!c || !apply) phase_commit_fail("bad apply all");
    if (t_phase_apply_active) return 0;

    uint64_t apply_t0 = now_ns();
    t_phase_apply_active = 1;
    n = g_phase.count;
    loop = *c;
    applied = 0;
    for (;;) {
        if (n <= applied) break;
        if (!wc3_phase_run_or_seq())
            phase_commit_fail("phase run failed");
        for (i = applied; i < n; i++) {
            uint32_t before = g_phase.count;
            apply(c, i);
            c->disp_depth = loop.disp_depth;
            c->disp_draining = loop.disp_draining;
            WC3_CLEAN_DRAIN(c);
            c->disp_depth = loop.disp_depth;
            c->disp_draining = loop.disp_draining;
            if (g_phase.count != before) {
                /* The apply postfix re-entered the object loop and appended
                 * requests.  Extend the slice and rerun before applying the
                 * rest, so g_phase.ran covers every request. */
                n = g_phase.count;
                if (!wc3_phase_run_or_seq())
                    phase_commit_fail("phase run failed");
            }
        }
        applied = n;
        if (g_phase.count > n) n = g_phase.count;
    }
    par_note_phase_apply((unsigned long long)applied,
                         now_ns() - apply_t0);
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    g_phase.applies++;
    if (g_phase.applies == 1u || (g_phase.applies & 0xFFu) == 0u) {
        unsigned long long par_runs =
            atomic_load_explicit(&g_eff.phase_par_runs, memory_order_relaxed);
        unsigned long long seq_runs =
            atomic_load_explicit(&g_eff.phase_seq_runs, memory_order_relaxed);
        fprintf(stderr,
                "[phase] applies=%llu captures=%llu count=%u ran=%d "
                "par=%llu seq=%llu wake_us=%llu join_us=%llu "
                "max_slice_us=%llu\n",
                g_phase.applies, g_phase.captures, g_phase.count,
                g_phase.ran, par_runs, seq_runs,
                (unsigned long long)(g_phase.last_wake_ns / 1000u),
                (unsigned long long)(g_phase.last_join_ns / 1000u),
                (unsigned long long)(g_phase.last_max_slice_ns / 1000u));
    }
#endif
    *c = loop;
    wc3_phase_reset();
    t_phase_apply_active = 0;
    return 1;
}

void wc3_phase_reset(void)
{
    g_phase.count = 0;
    g_phase.reserved = 0;
    g_phase.ran = 0;
    phase_sync_config();
}

/* Replay the captured batch sequentially and through the phase pool, then
 * compare every hit/dist/pos bit.  This is the mandatory first step before
 * any product integration: it changes no guest state and no caller register.
 * The sequential result is the definition of correct. */
static void phase_shadow_flush(void)
{
    uint32_t n = g_phase.count;
    if (!n) return;

    g_phase.shadow_busy = 1;
    if (!g_phase_seq_ctx_ready || !g_phase_container_ready) {
        g_phase.count = 0;
        g_phase.reserved = 0;
        g_phase.shadow_busy = 0;
        phase_sync_config();
        return;
    }

    Wc3PhaseJob pj;
    memset(&pj, 0, sizeof pj);
    pj.req = g_phase.req;
    pj.cpu = NULL;
    pj.req_cpu = g_phase_cpu;
    pj.total = n;
    pj.fn = g_phase.fn;
    pj.stack_zero = g_phase.stack_zero;
    pj.main_last = g_phase.main_last;

    uint64_t seq_t0 = now_ns();
    int seq_ok = 1;
    for (uint32_t i = 0; i < n; i++) {
        if (!phase_run_request(&g_phase_seq_ctx, &pj, i)) {
            seq_ok = 0;
            break;
        }
        g_phase.seq_result[i].hit = g_phase.req[i].hit;
        g_phase.seq_result[i].dist_bits = g_phase.req[i].dist_bits;
        g_phase.seq_result[i].pos[0] = g_phase.req[i].pos[0];
        g_phase.seq_result[i].pos[1] = g_phase.req[i].pos[1];
        g_phase.seq_result[i].pos[2] = g_phase.req[i].pos[2];
    }
    uint64_t seq_ns = now_ns() - seq_t0;

    uint64_t par_t0 = now_ns();
    int par_ok = phase_run_job(&pj);
    uint64_t par_ns = now_ns() - par_t0;

    unsigned long long mismatch = 0;
    int equal = par_ok && seq_ok;
    if (par_ok && seq_ok) {
        for (uint32_t i = 0; i < n; i++) {
            const Wc3PhaseRequest *p = &g_phase.req[i];
            const Wc3PhaseResult *s = &g_phase.seq_result[i];
            /* A miss has no defined dist/pos.  Compare those words only on
             * a hit; otherwise a stale value from an earlier request in the
             * same chunk would look like a mismatch even though apply never
             * reads it. */
            if (p->hit != s->hit ||
                (p->hit &&
                 (p->dist_bits != s->dist_bits ||
                  p->pos[0] != s->pos[0] ||
                  p->pos[1] != s->pos[1] ||
                  p->pos[2] != s->pos[2]))) {
                equal = 0;
                mismatch++;
            }
        }
    }

    g_phase.flushes++;
    g_phase.mismatches += mismatch;
    g_phase.seq_ns_sum += seq_ns;
    g_phase.par_ns_sum += par_ns;
    g_phase.last_seq_ns = seq_ns;
    g_phase.last_par_ns = par_ns;
    par_note_phase_shadow(mismatch, seq_ns, par_ns);

    unsigned long long speedup =
        par_ns ? (seq_ns * 100ull) / par_ns : 0ull;
    fprintf(stderr,
            "[phase-shadow] n=%u chunks=%u seq_ok=%d par_ok=%d equal=%d "
            "mismatch=%llu seq_ns=%llu par_ns=%llu speedup_x100=%llu "
            "wake_ns=%llu join_ns=%llu max_slice_ns=%llu "
            "captures=%llu flushes=%llu\n",
            n, pj.chunks, seq_ok, par_ok, equal, mismatch,
            (unsigned long long)seq_ns, (unsigned long long)par_ns, speedup,
            (unsigned long long)g_phase.last_wake_ns,
            (unsigned long long)g_phase.last_join_ns,
            (unsigned long long)g_phase.last_max_slice_ns,
            g_phase.captures, g_phase.flushes);

    g_phase.count = 0;
    g_phase.reserved = 0;
    g_phase.shadow_busy = 0;
    phase_sync_config();
}

static void phase_shadow_capture(CPU *c, wc3_lifted_fn fn, uint32_t container,
                                 uint32_t total)
{
    phase_sync_config();
    if (!g_phase_seq_ctx_ready || !g_phase_container_ready) return;
    if (g_phase.enabled) return;
    if (g_phase.shadow_busy) return;

    int cfg_enabled =
        atomic_load_explicit(&g_phase_cfg.enabled, memory_order_relaxed);
    int cfg_shadow =
        atomic_load_explicit(&g_phase_cfg.shadow, memory_order_relaxed);
    if (!cfg_shadow || cfg_enabled) {
        /* The tuner changed the mode while a shadow window was pending.
         * Finish that window first; phase_sync_config() applies the new mode
         * after the window is empty. */
        if (g_phase.count)
            phase_shadow_flush();
        return;
    }
    if (!g_phase.shadow_enabled) return;

    if (t_ray_active) return;
    if (atomic_load_explicit(&g_phase_busy, memory_order_acquire)) return;
    if (atomic_load_explicit(&g_job_addr, memory_order_acquire)) return;
    if (!container || total == 0u || total > 0x100000u) return;
    if (!hle_android_guest_ok(container, WC3_PHASE_CONTAINER_SNAP)) return;

    uint32_t sp = c->esp;
    if (!hle_android_guest_ok(sp, 24u)) return;

    uint32_t array = rd32(container + 0x114u);
    uint64_t bytes = (uint64_t)total * 4u;
    if (!array || bytes > 0xFFFFFFFFu ||
        !hle_android_guest_ok(array, (uint32_t)bytes))
        return;

    if (g_phase.count >= g_phase.max_requests)
        phase_shadow_flush();
    if (g_phase.count >= WC3_PHASE_MAX_REQUESTS) return;

    uint32_t index = g_phase.count;
    uint32_t slot = phase_container_slot(index);
    Wc3PhaseRequest *r = &g_phase.req[index];
    r->index = index;
    r->array = array;
    r->count = total;
    r->out_pos = rd32(sp + 12u);
    r->out_dist = rd32(sp + 16u);
    r->in1 = rd32(sp + 4u);
    r->in2 = rd32(sp + 8u);
    r->user_ctx = rd32(sp + 20u);
    r->frame_va = 0;
    r->frame_len = 0;
    memcpy(guest_host_ptr(slot), guest_host_ptr(container),
           WC3_PHASE_CONTAINER_SNAP);
    wr32(slot + 0x110u, total);
    wr32(slot + 0x114u, array);
    r->container = slot;
    g_phase_cpu[index] = *c;
    g_phase.fn = fn;
    g_phase.count++;
    g_phase.captures++;

    if (g_phase.count >= g_phase.max_requests)
        phase_shadow_flush();
}
#endif

/* Dispatch a vertex batch.  All source and output ranges were validated by
 * the caller.  Returns 0 only when the pool could not be used; on 1 the
 * caller's frame and the real globals already match the sequential epilogue. */
static int run_vertex_batch(CPU *c, wc3_lifted_fn fn, unsigned kind,
                            uint32_t total, unsigned chunks,
                            const uint32_t *vg)
{
    if (chunks < 2u || chunks > g_workers) return 0;
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    if (atomic_load_explicit(&g_phase_busy, memory_order_acquire)) return 0;
#endif
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
    parallel_apply_split(&job, total, chunks);
    memcpy(job.vertex_globals, vg, sizeof job.vertex_globals);

    unsigned active = job_active_chunks(&job, chunks,
                                        parallel_main_last());
    uint64_t seq = publish_job((uintptr_t)&job, active);
    /* In the product layout the caller runs the last chunk in
     * g_worker[chunks-1] and that worker stays outside the active set.  The
     * debug A/B mode can restore the old all-worker layout for measurement. */
    wake_workers(active);

    if (parallel_main_last()) {
        RayCtx *main_ctx = &g_worker[chunks - 1u];
        job_chunk_range(main_ctx, &job, chunks - 1u);
        RUN_JOB_CHUNK(main_ctx, &job, 1);
    }

    note_main_wait(active, wait_for_workers(active, seq,
                                            tune_uint(&g_tune.main_spin_us)));
    atomic_store_explicit(&g_job_addr, 0, memory_order_release);

    int failed = 0;
    for (unsigned i = 0; i < chunks; i++) {
        if (!job.chunk_len[i]) continue;
        if (g_worker[i].vertex_failed) {
            failed = 1;
            break;
        }
    }

    if (failed) {
        /* A chunk context could not copy its private frame after all.  The
         * real globals and the caller's frame are still untouched; run the
         * original function once on the caller.  The entry guard is told to
         * stay out of this recovery call. */
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

    /* The last chunk ran in this context: the caller in the product layout,
     * or worker chunks-1 in the debug all-worker A/B layout. */
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

#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_VERTEX_VALIDATE
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

#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_VERTEX_VALIDATE
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
static int vertex_verify_after(CPU *c, wc3_lifted_fn fn, unsigned kind,
                               uint32_t count, const uint32_t *vg,
                               const CPU *before, const uint8_t *frame_before,
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

#if WC3_DEBUG && WC3_DEBUG_PERF
    unsigned vk = kind & 1u;
    g_vertex_verify_calls[vk]++;
    if (g_vertex_verify_calls[vk] == 1u ||
        (g_vertex_verify_calls[vk] % 8192u) == 0u) {
        fprintf(stderr,
                "[vertex] verify kind=%s calls=%llu failures=%llu\n",
                kind == WC3_VERTEX_TRANSFORM ? "transform" : "copy",
                g_vertex_verify_calls[vk], g_vertex_verify_failures[vk]);
    }
#endif

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
#if WC3_DEBUG && WC3_DEBUG_PERF
        g_vertex_verify_failures[vk]++;
#endif
        if (reports < 8u) {
            reports++;
            fprintf(stderr,
                    "[vertex] VERIFY MISMATCH kind=%s count=%u out=%d frame=%d "
                    "cpu=%d globals=%d\n",
                    kind == WC3_VERTEX_TRANSFORM ? "transform" : "copy",
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
    if (c->parallel_vertex_bypass) WC3_VERTEX_GUARD_FAIL(VG_BYPASS);
    if (!g_vertex_enabled) WC3_VERTEX_GUARD_FAIL(VG_DISABLED);
    if (kind > WC3_VERTEX_TRANSFORM) WC3_VERTEX_GUARD_FAIL(VG_BAD_KIND);
    if (g_pool_init_active) WC3_VERTEX_GUARD_FAIL(VG_INIT_ACTIVE);

    uint32_t count = rd32(GVA(WC3_VERTEX_GLOBAL_FIRST));
    if (count < par_gate_u(&g_vertex_threshold))
        WC3_VERTEX_GUARD_FAIL(VG_LOW);
    if (count > VERTEX_MAX_COUNT) WC3_VERTEX_GUARD_FAIL(VG_HIGH);
    return vertex_try_slow(c, fn, kind, count);
}

static __attribute__((noinline)) int vertex_try_slow(CPU *c,
                                                     wc3_lifted_fn fn,
                                                     unsigned kind,
                                                     uint32_t count)
{
#if WC3_DEBUG && WC3_DEBUG_PERF
    {
        static unsigned seen;
        unsigned bit = 1u << (kind & 1u);
        if (!(seen & bit)) {
            seen |= bit;
            fprintf(stderr, "[vertex] first slow kind=%s count=%u\n",
                    kind == WC3_VERTEX_TRANSFORM ? "transform" : "copy",
                    count);
        }
    }
#endif
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_VERTEX_VALIDATE
    VertexOracle oracle;
    CPU oracle_before;
    uint8_t oracle_frame[VERTEX_FRAME_COPY];
    int oracle_ready = 0;
#endif

    /* ready() applies the environment kill switches and sets the final
     * thresholds.  Recheck the values it may have changed before validating. */
    if (!ready()) WC3_VERTEX_GUARD_FAIL(VG_READY);
    if (!g_vertex_enabled) WC3_VERTEX_GUARD_FAIL(VG_SLOW_DISABLED);
    if (count < par_gate_u(&g_vertex_threshold))
        WC3_VERTEX_GUARD_FAIL(VG_LOW);
    if (count > VERTEX_MAX_COUNT) WC3_VERTEX_GUARD_FAIL(VG_HIGH);

#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_VERTEX_VALIDATE
    if (!vertex_frame_ok(c)) WC3_VERTEX_GUARD_FAIL(VG_FRAME);

    /* The setup block leaves the object pointer in [ebp-4] and the loop kind
     * in [ctx+0x224].  Check it so a future call-site change fails closed. */
    uint32_t obj = rd32(c->ebp - 4u);
    if (!obj || !hle_android_guest_ok(obj, 0x228u))
        WC3_VERTEX_GUARD_FAIL(VG_OBJ);
    uint32_t mode = rd32(obj + 0x224u);
    if ((kind == WC3_VERTEX_COPY && mode != 0u) ||
        (kind == WC3_VERTEX_TRANSFORM && mode != 1u))
        WC3_VERTEX_GUARD_FAIL(VG_MODE);
    if (kind == WC3_VERTEX_TRANSFORM && rd32(c->ebp - 0x18u) != 0u)
        WC3_VERTEX_GUARD_FAIL(VG_SLOT);
#endif

#if WC3_DEBUG && WC3_DEBUG_HLE
    {
        static unsigned trace;
        if (trace < 64u) {
            fprintf(stderr,
                    "[vertex] try kind=%u count=%u threshold=%u ebp=%08X esp=%08X\n",
                    kind, count, par_gate_u(&g_vertex_threshold),
                    c->ebp, c->esp);
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

#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_VERTEX_VALIDATE
    /* Source streams: p0 and p1 are 12-byte vectors, p2 is 4 bytes, p3/p4
     * are 8 bytes.  Transform also reads one byte from the matrix-index array
     * pm per vertex. */
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
    static const uint32_t src_size[4] = { 12u, 4u, 8u, 8u };
    for (unsigned i = 0; i < 4; i++) {
        uint32_t size = src_size[i];
        if (!vertex_span_ok(src_ptr[i], src_stride[i], count, size))
            WC3_VERTEX_GUARD_FAIL((unsigned)VG_SRC1 + i);
        src[i + 1].lo = src_ptr[i];
        src[i + 1].hi = (uint64_t)src_ptr[i] +
                        (uint64_t)(count - 1u) * src_stride[i] + size;
        src[i + 1].stride = src_stride[i];
        src[i + 1].size = size;
        src[i + 1].used = 1;
    }
    if (!vertex_span_ok(vg[1], vg[2], count, 12u))
        WC3_VERTEX_GUARD_FAIL(VG_SRC0);
    if (kind == WC3_VERTEX_TRANSFORM) {
        if (!vertex_span_ok(vg[7], vg[8], count, 1u))
            WC3_VERTEX_GUARD_FAIL(VG_PM);
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
    /* Position (EBX) writes p0 (12 bytes) then p1 (12 bytes) at +0xC.  ESI
     * and EDI write 8 bytes each (p3/p4), and the optional EDX stream writes
     * p2 (4 bytes).  Keeping p1 inside out[0] makes the oracle and the alias
     * check see every byte the loop actually writes. */
    uint32_t out_size[4] = { 24u, 8u, 8u, 4u };
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
                WC3_VERTEX_GUARD_FAIL(VG_DUMMY_FRAME);
            if (skip_count >= 4u) WC3_VERTEX_GUARD_FAIL(VG_DUMMY_ALIAS);
            /* The setup block can point the optional EDX output at [ebp-8]
             * itself.  That is a self-pointer dummy: prepare_vertex_ctx
             * retargets it at the worker's private frame, and
             * run_vertex_batch takes the caller's EDX exit value from the
             * pre-dispatch [ebp-8].  The dummy word is skipped in the frame
             * comparison.  Allow exactly that word; every other overlap with
             * merge state still rejects the batch. */
            int self_dummy = dummy_lo == (uint64_t)c->ebp - 8u &&
                             out_size[i] == 4u;
            if (dummy_lo < (uint64_t)c->ebp - 0x18u + 4u &&
                (uint64_t)c->ebp - 0x18u < dummy_hi)
                WC3_VERTEX_GUARD_FAIL(VG_DUMMY_ALIAS);
            if (!self_dummy &&
                dummy_lo < (uint64_t)c->ebp - 8u + 4u &&
                (uint64_t)c->ebp - 8u < dummy_hi)
                WC3_VERTEX_GUARD_FAIL(VG_DUMMY_ALIAS);
            if (dummy_lo < (uint64_t)c->ebp + 8u + 4u &&
                (uint64_t)c->ebp + 8u < dummy_hi)
                WC3_VERTEX_GUARD_FAIL(VG_DUMMY_ALIAS);
            for (unsigned j = 0; j < 5u; j++) {
                if (src[j].used && dummy_lo < src[j].hi &&
                    src[j].lo < dummy_hi)
                    WC3_VERTEX_GUARD_FAIL(VG_DUMMY_ALIAS);
            }
            skip[skip_count].off = (uint32_t)(dummy_lo - frame_lo);
            skip[skip_count].size = out_size[i];
            skip_count++;
            continue;
        }
        if (!vertex_span_ok(out_ptr[i], out_stride[i], count, out_size[i]))
            WC3_VERTEX_GUARD_FAIL(VG_OUT_SPAN);
        out[i].lo = out_ptr[i];
        out[i].hi = (uint64_t)out_ptr[i] +
                    (uint64_t)(count - 1u) * out_stride[i] + out_size[i];
        out[i].stride = out_stride[i];
        out[i].size = out_size[i];
        out[i].used = 1;
        if ((uint64_t)frame_lo < out[i].hi &&
            out[i].lo < (uint64_t)frame_hi)
            WC3_VERTEX_GUARD_FAIL(VG_OUT_FRAME);
    }
    for (unsigned i = 0; i < 4; i++) {
        if (!out[i].used) continue;
        for (unsigned j = i + 1u; j < 4; j++) {
            if (ranges_overlap(&out[i], &out[j]))
                WC3_VERTEX_GUARD_FAIL(VG_OVERLAP);
        }
        for (unsigned j = 0; j < 5; j++) {
            if (ranges_overlap(&out[i], &src[j]))
                WC3_VERTEX_GUARD_FAIL(VG_OVERLAP);
        }
    }

    vertex_oracle_init(&oracle, out, count, skip, skip_count);
    oracle_ready = 1;
#endif

    unsigned chunks = vertex_chunk_limit();
    if (!chunks) WC3_VERTEX_GUARD_FAIL(VG_CHUNKS);
    if (chunks > count) chunks = count;
    if (par_gate_u(&g_vertex_min_chunk)) {
        unsigned by_count = chunk_count_by_min(count,
                                              par_gate_u(&g_vertex_min_chunk),
                                               parallel_vertex_split_ceil());
        if (chunks > by_count) chunks = by_count;
    }
    if (chunks < 2u) WC3_VERTEX_GUARD_FAIL(VG_CHUNKS_ONE);
    WC3_VERTEX_GUARD_NOTE(VG_RUN);

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

#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_VERTEX_VALIDATE
    int oracle_run = oracle_ready && g_vertex_verify;
    if (oracle_run) {
        oracle_before = *c;
        memcpy(oracle_frame,
               guest_host_ptr(c->ebp - VERTEX_FRAME_BELOW),
               sizeof oracle_frame);
    }
#endif
#if WC3_DEBUG && WC3_DEBUG_PERF
    uint64_t perf_t0 = tune_int(&g_tune.timing) ? now_ns() : 0;
#endif
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    uint64_t par_t0 = now_ns();
#endif
    int pooled = run_vertex_batch(c, fn, kind, count, chunks, vg);
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    if (pooled) par_note_par(count, now_ns() - par_t0, chunks);
#endif
#if WC3_DEBUG && WC3_DEBUG_PERF
    perf_note_kind(KIND_PERF_VERTEX, count, chunks, pooled,
                   perf_t0 ? now_ns() - perf_t0 : 0);
    if (pooled) {
        static unsigned seen;
        unsigned bit = 1u << (kind & 1u);
        if (!(seen & bit)) {
            seen |= bit;
            fprintf(stderr, "[vertex] first pooled kind=%s count=%u "
                    "chunks=%u\n",
                    kind == WC3_VERTEX_TRANSFORM ? "transform" : "copy",
                    count, chunks);
        }
    }
#endif
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_VERTEX_VALIDATE
    if (pooled && oracle_run && pthread_mutex_trylock(&g_submit_lock) == 0) {
        /* Keep the pool idle while the oracle replays the sequential loop on
         * the caller.  A concurrent wrapper that loses this trylock remains
         * sequential, and t_ray_active blocks nested pool use here. */
        int saved_active = t_ray_active;
        t_ray_active = 1;
        (void)vertex_verify_after(c, fn, kind, count, vg, &oracle_before,
                                  oracle_frame, &oracle);
        t_ray_active = saved_active;
        pthread_mutex_unlock(&g_submit_lock);
    }
#endif
    return pooled;
}

#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
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
 * succeeds; a chunk failure or a validation mismatch simply returns 0 so the
 * generated function runs the original loop on the caller.  On 1 the frame has
 * already been advanced exactly like the sequential epilogue.  `allow_verify`
 * lets the debug A/B probe skip the shared g_verify_ctx oracle without
 * changing the process-wide WC3_TRIANGLE_VERIFY setting. */
static int run_triangle_batch(CPU *c, wc3_lifted_fn fn, uint32_t total,
                              unsigned chunks, uint32_t indices,
                              uint32_t best_ptr, uint32_t index_ptr,
                              uint32_t orig_best WC3_TRIANGLE_VERIFY_PARAM)
{
    if (chunks < 2u || chunks > g_workers) return 0;
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    if (atomic_load_explicit(&g_phase_busy, memory_order_acquire)) return 0;
#endif
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
    parallel_apply_split(&job, total, chunks);
    job.triangle_indices = indices;
    job.triangle_best_ptr = best_ptr;
    job.triangle_index_ptr = index_ptr;
    job.triangle_orig_best = orig_best;

#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
    /* The oracle writes the real best/index words, so snapshot them first.
     * The caller's frame is not touched: prepare_triangle_ctx copies it.
     * Range checks in triangle_try_slow are always on in a validation build;
     * only this full sequential replay is opt-in. */
    TriangleState expect;
    int verify = allow_verify && g_triangle_verify;
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

    unsigned active = job_active_chunks(&job, chunks,
                                        parallel_main_last());
    uint64_t seq = publish_job((uintptr_t)&job, active);
    wake_workers(active);

    if (parallel_main_last()) {
        RayCtx *main_ctx = &g_worker[chunks - 1u];
        job_chunk_range(main_ctx, &job, chunks - 1u);
        RUN_JOB_CHUNK(main_ctx, &job, 1);
    }

    note_main_wait(active, wait_for_workers(active, seq,
                                            tune_uint(&g_tune.main_spin_us)));
    atomic_store_explicit(&g_job_addr, 0, memory_order_release);

    for (unsigned i = 0; i < chunks; i++) {
        if (!job.chunk_len[i]) continue;
        if (g_worker[i].triangle_failed) {
            /* Every chunk context wrote only its private frame words, and the
             * oracle restored the real outputs.  The caller can run the
             * original loop from the guard with no repair. */
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
        if (!job.chunk_len[i]) continue;
        uint32_t chunk_best = g_worker[i].triangle_best;
        if (bits_float(chunk_best) < bits_float(merged_best)) {
            merged_best = chunk_best;
            merged_index = job.chunk_start[i] + g_worker[i].triangle_index;
        }
    }

    /* The last chunk ran in this context: the caller in the product layout,
     * or worker chunks-1 in the debug all-worker A/B layout. */
    RayCtx *last = &g_worker[chunks - 1u];

#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
    /* Compare everything the chunks produce before the epilogue pops the
     * caller's frame.  XMM0 in a chunk context is its own chunk's best, so
     * only XMM1..7 and ECX/EDX are comparable here. */
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
#if WC3_DEBUG && WC3_DEBUG_PERF
            g_triangle_verify_failures++;
#endif
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
     * final iteration there, so copy it from the caller's last-chunk context. */
    for (unsigned i = 1; i < 8u; i++) c->xmm[i] = last->cpu.xmm[i];
    c->ecx = last->cpu.ecx;
    c->edx = last->cpu.edx;

#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
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
#if WC3_DEBUG && WC3_DEBUG_PERF
            g_triangle_verify_failures++;
#endif
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

#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
    if (verify) {
        g_triangle_verify_calls++;
        if (g_triangle_verify_calls == 1u ||
            (g_triangle_verify_calls % 4096u) == 0u) {
            fprintf(stderr,
                    "[triangle] verify calls=%llu failures=%llu "
                    "total=%u chunks=%u\n",
                    g_triangle_verify_calls, g_triangle_verify_failures,
                    total, chunks);
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

#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
/* A 32-triangle call from a raycast worker cannot use the pool: the outer job
 * already owns it, and the pool is not re-entrant.  Still sample the sequential
 * cost once, on a private frame, and say why the parallel half is unavailable.
 * This is the case the loaded map actually hits, so silence here would make the
 * main-thread A/B look armed but never produce a line. */
static __attribute__((noinline)) void triangle_ab_nested_probe(
    CPU *c, wc3_lifted_fn fn, uint32_t raw_count, uint32_t tri_count)
{
    if (!g_triangle_ab_enabled || !g_triangle_ab_ctx_ready) return;
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &g_triangle_ab_nested_state, &expected, 1,
            memory_order_acq_rel, memory_order_relaxed))
        return;
    int busy = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &g_triangle_ab_ctx_busy, &busy, 1,
            memory_order_acq_rel, memory_order_relaxed)) {
        atomic_store_explicit(&g_triangle_ab_nested_state, 0,
                              memory_order_release);
        return;
    }

    const char *reason = triangle_ab_cpu_is_worker(c) ? "worker" :
                         t_ray_active ? "nested" :
                         atomic_load_explicit(&g_job_addr,
                                              memory_order_acquire) ?
                             "pool-active" : "unknown";
    unsigned pool_busy = atomic_load_explicit(&g_job_addr,
                                              memory_order_acquire) != 0;

    uint32_t indices = rd32(c->ebp + 0x18u);
    uint32_t vertex = rd32(c->ebp + 0x0Cu);
    uint32_t best_ptr = c->esi;
    uint32_t index_ptr = rd32(c->ebp + 0x20u);
    if (!triangle_args_ok(c, tri_count, indices, vertex, best_ptr, index_ptr))
        goto abort;

    unsigned chunks = tri_count / TRIANGLE_AB_MIN_CHUNK;
    if (chunks > g_workers) chunks = g_workers;
    if (par_gate_u(&g_triangle_chunks) &&
        chunks > par_gate_u(&g_triangle_chunks))
        chunks = par_gate_u(&g_triangle_chunks);
    if (chunks < 2u) goto abort;

    RayJob job;
    memset(&job, 0, sizeof job);
    job.kind = RAY_JOB_TRIANGLE;
    job.fn = fn;
    job.cpu = c;
    job_set_chunk_split(&job, tri_count, chunks);
    job.triangle_indices = indices;
    job.triangle_best_ptr = best_ptr;
    job.triangle_index_ptr = index_ptr;
    job.triangle_orig_best = rd32(best_ptr);

    RayCtx *v = &g_triangle_ab_ctx;
    v->start = 0;
    v->count = tri_count;
    if (!prepare_triangle_ctx(v, &job, 1)) goto abort;

    int saved_active = t_ray_active;
    t_ray_active = 1;
    uint64_t t0 = now_ns();
    WC3_DIRECT_CALL_CLEAN(fn, &v->cpu);
    uint64_t t1 = now_ns();
    t_ray_active = saved_active;
    uint64_t seq_ns = t1 >= t0 ? t1 - t0 : 0;
    uint32_t seq_best = rd32(v->triangle_best_addr);
    uint32_t seq_index = rd32(v->triangle_index_addr);

    fprintf(stderr,
            "[triangle] ab nested raw=%u tris=%u chunks=%u minc=%u "
            "samples=1 seq_ns=%llu par=unavailable reason=%s pool_busy=%u "
            "best=%08X idx=%u\n",
            raw_count, tri_count, chunks, TRIANGLE_AB_MIN_CHUNK,
            (unsigned long long)seq_ns, reason, pool_busy,
            seq_best, seq_index);
    atomic_store_explicit(&g_triangle_ab_nested_state, 2,
                          memory_order_release);
    atomic_store_explicit(&g_triangle_ab_ctx_busy, 0, memory_order_release);
    return;

abort:
    atomic_store_explicit(&g_triangle_ab_nested_state, 0,
                          memory_order_release);
    atomic_store_explicit(&g_triangle_ab_ctx_busy, 0, memory_order_release);
}

/* Measure the small batch before the normal threshold returns.  Both trials
 * use g_triangle_ab_ctx: the sequential one runs the generated function on a
 * private frame, the parallel one hands the same private frame to
 * run_triangle_batch.  Private best/index words at +0x28/+0x2C keep the real
 * caller's output untouched, so the normal sequential path can still run after
 * this probe.  The first valid sample stays one-shot for the process. */
static __attribute__((noinline)) void triangle_ab_probe(CPU *c,
                                                        wc3_lifted_fn fn,
                                                        uint32_t raw_count,
                                                        uint32_t tri_count)
{
    if (!g_triangle_ab_enabled || !g_triangle_ab_ctx_ready) return;
    if (atomic_load_explicit(&g_triangle_ab_state, memory_order_relaxed) == 2 &&
        atomic_load_explicit(&g_triangle_ab_nested_state,
                             memory_order_relaxed) == 2)
        return;
    /* A raycast/vertex worker also reaches the triangle wrapper.  Its call is
     * inside an active outer job, so the pool cannot be re-entered; log the
     * nested sequential cost instead of waiting for a main-thread call that
     * may never happen.  The pool-active check comes first because it is the
     * common worker case and avoids scanning the worker CPU table on every
     * small batch after the one-shot nested sample is done. */
    int nested = t_ray_active ||
                 atomic_load_explicit(&g_job_addr, memory_order_acquire) != 0;
    if (!nested) nested = triangle_ab_cpu_is_worker(c);
    if (nested) {
        if (atomic_load_explicit(&g_triangle_ab_nested_state,
                                 memory_order_relaxed) == 2)
            return;
        triangle_ab_nested_probe(c, fn, raw_count, tri_count);
        return;
    }
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &g_triangle_ab_state, &expected, 1,
            memory_order_acq_rel, memory_order_relaxed))
        return;
    int busy = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &g_triangle_ab_ctx_busy, &busy, 1,
            memory_order_acq_rel, memory_order_relaxed)) {
        atomic_store_explicit(&g_triangle_ab_state, 0, memory_order_release);
        return;
    }
    uint32_t indices = rd32(c->ebp + 0x18u);
    uint32_t vertex = rd32(c->ebp + 0x0Cu);
    uint32_t best_ptr = c->esi;
    uint32_t index_ptr = rd32(c->ebp + 0x20u);
    if (!triangle_args_ok(c, tri_count, indices, vertex, best_ptr, index_ptr))
        goto abort;

    unsigned chunks = tri_count / TRIANGLE_AB_MIN_CHUNK;
    if (chunks > g_workers) chunks = g_workers;
    if (par_gate_u(&g_triangle_chunks) &&
        chunks > par_gate_u(&g_triangle_chunks))
        chunks = par_gate_u(&g_triangle_chunks);
    if (chunks < 2u) goto abort;

    uint32_t orig_best = rd32(best_ptr);
    RayJob job;
    memset(&job, 0, sizeof job);
    job.kind = RAY_JOB_TRIANGLE;
    job.fn = fn;
    job.cpu = c;
    job_set_chunk_split(&job, tri_count, chunks);
    job.triangle_indices = indices;
    job.triangle_best_ptr = best_ptr;
    job.triangle_index_ptr = index_ptr;
    job.triangle_orig_best = orig_best;

    RayCtx *v = &g_triangle_ab_ctx;
    uint64_t seq_min = ~(uint64_t)0;
    uint64_t par_min = ~(uint64_t)0;
    uint64_t seq_sum = 0;
    uint64_t par_sum = 0;
    uint32_t seq_best = 0, seq_index = 0;
    uint32_t par_best = 0, par_index = 0;
    int equal = 1;

    for (unsigned sample = 0; sample < TRIANGLE_AB_SAMPLES; sample++) {
        v->start = 0;
        v->count = tri_count;
        if (!prepare_triangle_ctx(v, &job, 1)) goto abort;

        int saved_active = t_ray_active;
        t_ray_active = 1;
        uint64_t seq_t0 = now_ns();
        WC3_DIRECT_CALL_CLEAN(fn, &v->cpu);
        uint64_t seq_t1 = now_ns();
        t_ray_active = saved_active;
        uint64_t seq_dt = seq_t1 >= seq_t0 ? seq_t1 - seq_t0 : 0;
        seq_best = rd32(v->triangle_best_addr);
        seq_index = rd32(v->triangle_index_addr);

        v->start = 0;
        v->count = tri_count;
        if (!prepare_triangle_ctx(v, &job, 1)) goto abort;
        uint32_t best_addr = v->triangle_best_addr;
        uint32_t index_addr = v->triangle_index_addr;

        uint64_t par_t0 = now_ns();
        int pooled = run_triangle_batch(&v->cpu, fn, tri_count, chunks,
                                        indices, best_addr, index_addr,
                                        orig_best WC3_TRIANGLE_VERIFY_ARG(0));
        uint64_t par_t1 = now_ns();
        if (!pooled) goto abort;
        uint64_t par_dt = par_t1 >= par_t0 ? par_t1 - par_t0 : 0;

        par_best = rd32(best_addr);
        par_index = rd32(index_addr);
        if (seq_best != par_best || seq_index != par_index) equal = 0;
        if (seq_dt < seq_min) seq_min = seq_dt;
        if (par_dt < par_min) par_min = par_dt;
        seq_sum += seq_dt;
        par_sum += par_dt;
    }

    {
        unsigned long long seq_avg = seq_sum / TRIANGLE_AB_SAMPLES;
        unsigned long long par_avg = par_sum / TRIANGLE_AB_SAMPLES;
        unsigned long long speedup =
            par_avg ? (seq_avg * 100ull) / par_avg : 0;
        long long gain = (long long)seq_avg - (long long)par_avg;
        fprintf(stderr,
                "[triangle] ab raw=%u tris=%u chunks=%u minc=%u samples=%u "
                "seq_min_ns=%llu seq_avg_ns=%llu par_min_ns=%llu "
                "par_avg_ns=%llu gain_avg_ns=%lld speedup_x100=%llu "
                "equal=%d seq_best=%08X seq_idx=%u par_best=%08X par_idx=%u\n",
                raw_count, tri_count, chunks, TRIANGLE_AB_MIN_CHUNK,
                TRIANGLE_AB_SAMPLES,
                (unsigned long long)seq_min, seq_avg,
                (unsigned long long)par_min, par_avg, gain, speedup,
                equal, seq_best, seq_index, par_best, par_index);
    }
    atomic_store_explicit(&g_triangle_ab_state, 2, memory_order_release);
    atomic_store_explicit(&g_triangle_ab_ctx_busy, 0, memory_order_release);
    return;

abort:
    atomic_store_explicit(&g_triangle_ab_state, 0, memory_order_release);
    atomic_store_explicit(&g_triangle_ab_ctx_busy, 0, memory_order_release);
}
#endif

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
#if WC3_DEBUG && WC3_DEBUG_PERF
    {
        static unsigned trace;
        if (trace < 4u) {
            uint32_t trace_tri =
                raw_count ? (raw_count - 1u) / 3u + 1u : 0u;
            const char *ctx =
                triangle_ab_cpu_is_worker(c) ? "worker" : "main";
            /* `pool` is the process-wide job address; `active` is the local
             * submission guard.  A worker inside an outer job therefore logs
             * ctx=worker pool=1 active=0. */
            fprintf(stderr,
                    "[triangle] dispatch raw=%u tris=%u threshold=%u minc=%u "
                    "ctx=%s pool=%u active=%u\n",
                    raw_count, trace_tri, par_gate_u(&g_triangle_threshold),
                    par_gate_u(&g_triangle_min_chunk), ctx,
                    (unsigned)(atomic_load_explicit(&g_job_addr,
                                                    memory_order_acquire) != 0),
                    (unsigned)(t_ray_active != 0));
            trace++;
        }
    }
#endif
    if (!raw_count || raw_count > TRIANGLE_MAX_COUNT * 3u) return 0;

    /* L_6F125924 converts an index count to a triangle count with
     * ceil((count-1)/3).  Use the same formula so a chunk boundary always
     * lands on a triangle the original loop would have visited. */
    uint32_t tri_count = (raw_count - 1u) / 3u + 1u;
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
    /* The production gate keeps this batch sequential.  Sample it once in a
     * debug build so the log can say whether a 16-triangle split would win;
     * the probe itself never changes the path or the caller's outputs. */
    if (tri_count >= 32u && tri_count < par_gate_u(&g_triangle_threshold))
        triangle_ab_probe(c, fn, raw_count, tri_count);
#endif
    if (tri_count < par_gate_u(&g_triangle_threshold) ||
        tri_count > TRIANGLE_MAX_COUNT)
        return 0;
    return triangle_try_slow(c, fn, raw_count, tri_count);
}

static __attribute__((noinline)) int triangle_try_slow(CPU *c,
                                                       wc3_lifted_fn fn,
                                                       uint32_t raw_count,
                                                       uint32_t tri_count)
{
    uint32_t indices = rd32(c->ebp + 0x18u);
#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
    uint32_t vertex = rd32(c->ebp + 0x0Cu);
#endif
    uint32_t best_ptr = c->esi;
    uint32_t index_ptr = rd32(c->ebp + 0x20u);
    if (!best_ptr || !index_ptr) return 0;

#if WC3_DEBUG && WC3_DEBUG_PERF && WC3_PARALLEL_TRIANGLE_VALIDATE
    if (!triangle_args_ok(c, tri_count, indices, vertex, best_ptr, index_ptr))
        return 0;
#endif

    unsigned chunks = triangle_chunk_limit();
    if (!chunks) return 0;
    if (chunks > tri_count) chunks = tri_count;
    if (par_gate_u(&g_triangle_min_chunk)) {
        unsigned by_count = chunk_count_by_min(tri_count,
                                              par_gate_u(&g_triangle_min_chunk),
                                              parallel_triangle_split_ceil());
        if (chunks > by_count) chunks = by_count;
    }
    if (chunks < 2u) return 0;

#if WC3_DEBUG && WC3_DEBUG_PERF
    {
        static unsigned trace;
        if (trace < 4u) {
            fprintf(stderr,
                    "[triangle] run raw=%u tris=%u chunks=%u threshold=%u "
                    "minc=%u\n",
                    raw_count, tri_count, chunks,
                    par_gate_u(&g_triangle_threshold),
                    par_gate_u(&g_triangle_min_chunk));
            trace++;
        }
    }
#endif

#if WC3_DEBUG && WC3_DEBUG_HLE
    {
        static unsigned trace;
        if (trace < 16u) {
            fprintf(stderr,
                    "[triangle] batch indices=%u tris=%u chunks=%u "
                    "threshold=%u best=%08X\n",
                    raw_count, tri_count, chunks,
                    par_gate_u(&g_triangle_threshold),
                    rd32(best_ptr));
            trace++;
        }
    }
#endif

#if WC3_DEBUG && WC3_DEBUG_PERF
    uint64_t perf_t0 = tune_int(&g_tune.timing) ? now_ns() : 0;
#endif
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    uint64_t par_t0 = now_ns();
#endif
    int pooled = run_triangle_batch(c, fn, tri_count, chunks, indices,
                                    best_ptr, index_ptr,
                                    rd32(best_ptr) WC3_TRIANGLE_VERIFY_ARG(1));
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    if (pooled) par_note_par(tri_count, now_ns() - par_t0, chunks);
#endif
#if WC3_DEBUG && WC3_DEBUG_PERF
    uint64_t perf_dt = perf_t0 ? now_ns() - perf_t0 : 0;
    perf_note_kind(KIND_PERF_TRIANGLE, tri_count, chunks, pooled, perf_dt);
    if (pooled && perf_t0) {
        static unsigned seen;
        if (!seen) {
            seen = 1;
            fprintf(stderr,
                    "[triangle] first pooled tris=%u chunks=%u wall_us=%llu\n",
                    tri_count, chunks,
                    (unsigned long long)(perf_dt / 1000u));
        }
    }
#endif
    return pooled;
}

/* Dispatch one job and merge the results.  Returns 0 only when the pool could
 * not be used.  On success the merged output is in `merged`. */
/* Small raycast batches lose more in wakeups and frame copies than they gain
 * from splitting.  The as-built floor is eight candidates per chunk
 * (RAY_MIN_CHUNK), with the auto threshold at
 * max(2*min_chunk, 2*workers) = 16 for eight workers; the split is also
 * capped by the batch size. */
static unsigned raycast_chunk_count(uint32_t total)
{
    unsigned limit = tune_uint(&g_tune.chunk_limit);
    if (limit == 0 || limit > g_workers) limit = g_workers;
    if (limit > total) limit = total;
    unsigned min_chunk = tune_uint(&g_tune.min_chunk);
    if (min_chunk) {
        unsigned by_count = chunk_count_by_min(total, min_chunk,
                                               parallel_ray_split_ceil());
        if (limit > by_count) limit = by_count;
    }
    return limit;
}

static int run_batch(CPU *c, wc3_lifted_fn fn, uint32_t container,
                     uint32_t total, unsigned limit, uint32_t array,
                     uint32_t in1, uint32_t in2, uint32_t out_pos,
                     uint32_t out_dist, uint32_t user_ctx, RayCtx *merged)
{
    if (limit < 2u || limit > g_workers) return 0;
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    if (atomic_load_explicit(&g_phase_busy, memory_order_acquire)) return 0;
#endif
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
            fprintf(stderr,
                    "[raycast] batch total=%u chunks=%u main=%d ceil=%d\n",
                    total, limit, parallel_main_last(),
                    parallel_ray_split_ceil());
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
    parallel_apply_split(&job, total, limit);
    job.stack_zero = tune_uint(&g_tune.stack_zero);
    job.container_copy = tune_uint(&g_tune.container_copy);

    unsigned active = job_active_chunks(&job, limit,
                                        parallel_main_last());
    uint64_t seq = publish_job((uintptr_t)&job, active);
    /* Product layout: the caller runs the last chunk in g_worker[limit-1],
     * which stays outside the active set.  Debug A/B can use all workers. */
    wake_workers(active);

    if (parallel_main_last()) {
        RayCtx *main_ctx = &g_worker[limit - 1u];
        job_chunk_range(main_ctx, &job, limit - 1u);
        RUN_JOB_CHUNK(main_ctx, &job, 1);
    }

    note_main_wait(active, wait_for_workers(active, seq,
                                            tune_uint(&g_tune.main_spin_us)));
    atomic_store_explicit(&g_job_addr, 0, memory_order_release);

#if WC3_DEBUG && WC3_DEBUG_PERF
    g_last_chunks = limit;
    g_last_total = total;
#endif
    merge_results(merged, &job, limit);

#if WC3_PAR_LAYOUT_DEBUG
    if (tune_int(&g_tune.verify)) {
        RayCtx seq;
        run_verify(c, fn, container, in1, in2, out_pos, out_dist, user_ctx,
                   &seq);
        g_verify_calls++;
        if (g_verify_calls == 1u || (g_verify_calls % 65536u) == 0u) {
            fprintf(stderr,
                    "[raycast] verify calls=%llu failures=%llu total=%u "
                    "chunks=%u\n",
                    g_verify_calls, g_verify_failures, total, limit);
        }
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
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    uint64_t par_t0 = now_ns();
#endif
    unsigned ray_chunks = raycast_chunk_count(total);
    if (ray_chunks < 2u) {
        call_direct(c, fn);
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
        par_note_seq(total, now_ns() - par_t0);
#endif
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
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
        par_note_seq(total, now_ns() - par_t0);
#endif
        return;
    }

    RayCtx merged;
    if (!run_batch(c, fn, container, total, ray_chunks, array, in1, in2,
                   out_pos, out_dist, user_ctx, &merged)) {
        call_direct(c, fn);
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
        par_note_seq(total, now_ns() - par_t0);
#endif
        return;
    }

#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    par_note_par_ray(total, now_ns() - par_t0, ray_chunks);
#endif
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

#if WC3_PAR_LAYOUT_DEBUG
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

    /* Read the container only after the sequential early-outs.  The disabled
     * product path must not touch the guest CPU beyond the call itself. */
    uint32_t container = c->ecx;
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
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    /* Shadow capture runs before the real call so the snapshot sees the
     * caller state and container as they are at the call site.  It is
     * debug-only and does not change the normal path.  Sync first so a live
     * `phase_shadow=1` write can take effect without a relaunch. */
    phase_sync_config();
    if (g_phase.shadow_enabled && !g_phase.shadow_busy)
        phase_shadow_capture(c, fn, container, total);
#endif
    unsigned threshold = tune_uint(&g_tune.threshold);
    if (total < threshold) {
        call_direct(c, fn);
        return;
    }

    raycast_try_slow(c, fn, container, total);
}
