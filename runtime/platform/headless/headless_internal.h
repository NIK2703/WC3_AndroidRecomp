/*
 * headless_internal.h - state shared by the headless platform files.
 *
 * The platform API passes its context to every call.  Headless keeps one
 * HeadlessState per process and points every API context at it.  The input
 * consumer has no context argument, so the instance is also published through
 * g_headless for that one entry point.
 */
#ifndef WC3_HEADLESS_INTERNAL_H
#define WC3_HEADLESS_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>

#include "platform.h"
#include "platform_headless.h"

#define HEADLESS_TLS_MAX 64u
#define HEADLESS_WAIT_MAX 64u
#define HEADLESS_SYNC_MAGIC 0x57433353u /* "WC3S" */
#define HEADLESS_INPUT_RING 1024u
#define HEADLESS_INPUT_MASK (HEADLESS_INPUT_RING - 1u)
#define HEADLESS_MPQ_MAX 64u
#define HEADLESS_CONFIG_MAX 4096u
#define HEADLESS_SCRIPT_MAX 4096u
#define HEADLESS_DEFAULT_EPOCH_NS 946684800000000000ull /* 2000-01-01Z */

typedef struct HeadlessScript {
    Wc3HeadlessScriptEvent event;
    uint64_t publish_ns;
} HeadlessScript;

enum {
    HEADLESS_BLOCK_SYNC = 1,
    HEADLESS_BLOCK_SLEEP = 2,
    HEADLESS_BLOCK_JOIN = 3
};

typedef struct HeadlessSchedThread {
    struct HeadlessSchedThread *next;
    pthread_cond_t cond;
    pthread_t pthread;
    uint64_t seq;
    uint64_t wake_seq;
    uint64_t deadline_ns;
    void *owner;
    void *wait_target;
    int ready;
    int blocked;
    int block_reason;
    int timed_out;
    int exited;
} HeadlessSchedThread;

typedef struct HeadlessState {
    char asset_root[WC3_HEADLESS_PATH_MAX];
    char save_root[WC3_HEADLESS_PATH_MAX];
    uint32_t width;
    uint32_t height;
    char window_title[128];

    char *clipboard;
    pthread_mutex_t clipboard_lock;

    /* Deterministic T1 schedule.  virtual_clock == 0 keeps host clocks. */
    int virtual_clock;
    uint64_t epoch_unix_ns;
    int32_t timezone_minutes;
    uint32_t schedule_policy;
    uint32_t schedule_seed;
    char schedule_id[WC3_HEADLESS_SCHEDULE_ID_MAX];
    uint64_t schedule_hash;
    _Atomic(uint64_t) clock_ns;
    HeadlessScript *script;
    uint32_t script_count;
    uint32_t script_next;
    pthread_mutex_t script_lock;
    pthread_mutex_t input_push_lock;

    /* Cooperative T1 schedule.  Enabled only with a virtual schedule. */
    int sched_enabled;
    pthread_mutex_t sched_lock;
    pthread_cond_t sched_cond;
    HeadlessSchedThread *sched_threads;
    HeadlessSchedThread *sched_current;
    uint64_t sched_next_seq;
    uint64_t sched_wake_seq;

    /* Stable explicit configuration snapshot for ConfigApi.  Entries are
     * stored as "key=value"; get() returns pointers into these strings, so
     * they live until shutdown. */
    char **config;
    uint32_t config_count;

    /* MPQ names under asset_root, sorted for deterministic enumeration. */
    char mpq[HEADLESS_MPQ_MAX][WC3_HEADLESS_PATH_MAX];
    uint32_t mpq_count;

    _Alignas(64) Wc3InputEvent input[HEADLESS_INPUT_RING];
    _Alignas(64) _Atomic(uint32_t) input_head;
    _Alignas(64) _Atomic(uint32_t) input_tail;
    _Atomic(uint32_t) input_dropped;
    _Atomic(int) input_focus;

    void (*audio_callback)(void *opaque, void *frames, uint32_t bytes);
    void *audio_opaque;
} HeadlessState;

extern HeadlessState *g_headless;

/* Copy a string, returning -1 when it does not fit.  NULL src is an empty
 * string. */
int headless_copy_string(char *dst, size_t cap, const char *src);

/* Configure the optional deterministic clock and input script.  NULL keeps
 * host clocks.  The script is copied; call headless_schedule_free() before
 * releasing the state. */
int headless_schedule_init(HeadlessState *st,
                           const Wc3HeadlessScheduleConfig *config);
void headless_schedule_free(HeadlessState *st);

/* Monotonic time: virtual clock when configured, host CLOCK_MONOTONIC
 * otherwise. */
uint64_t headless_now_ns(HeadlessState *st);

/* Advance the virtual clock.  Returns -1 in host-clock mode or on overflow.
 * Due scripted input is published as a side effect. */
int headless_clock_advance(HeadlessState *st, uint64_t ns);

/* Publish scripted input whose release time is <= headless_now_ns(). */
void headless_input_publish_due(HeadlessState *st);

/* Producer-side ring insertion.  Serializes the external producer and the
 * script publisher; returns -1 when the ring is full. */
int headless_input_ring_push(HeadlessState *st, const Wc3InputEvent *event);

int headless_input_kind_valid(uint32_t kind);

/* Release mutex/critical-section ownership held by the calling thread before
 * it leaves the scheduler.  The pthread TLS destructor repeats this for host
 * threads, but scheduler waiters must see the abandonment before the exiting
 * thread hands the token away. */
void headless_sync_thread_exit(void);

/* Cooperative T1 scheduler.  Registration happens before the host thread is
 * created; start() parks a new thread until it owns the token.  All entry
 * points are no-ops unless a virtual schedule is active. */
int headless_sched_init(HeadlessState *st);
void headless_sched_free(HeadlessState *st);
int headless_sched_active(const HeadlessState *st);
HeadlessSchedThread *headless_sched_register(HeadlessState *st, void *owner);
void headless_sched_unregister(HeadlessState *st, HeadlessSchedThread *thread);
void headless_sched_start(HeadlessState *st, HeadlessSchedThread *thread);
void headless_sched_exit(HeadlessState *st, HeadlessSchedThread *thread);
void headless_sched_yield(HeadlessState *st);
int headless_sched_sleep(HeadlessState *st, uint64_t ns);
int headless_sched_block(HeadlessState *st, uint32_t timeout_ms,
                         int reason, int *timed_out);
int headless_sched_block_until(HeadlessState *st, uint64_t deadline_ns,
                               int reason, int *timed_out);
int headless_sched_wait_join(HeadlessState *st, HeadlessSchedThread *target);
void headless_sched_wake_sync(HeadlessState *st);

/* Set the asset/save roots and rescan the MPQ list.  Returns 0 on success. */
int headless_asset_roots(HeadlessState *st, const char *asset_root,
                         const char *save_root);

/* True when path is a readable regular file. */
int headless_regular_file(const char *path);

/* Copy the explicit configuration table.  Returns 0 on success; duplicate or
 * malformed keys fail. */
int headless_config_init(HeadlessState *st,
                         const Wc3HeadlessConfigEntry *entries,
                         uint32_t count);
void headless_config_free(HeadlessState *st);

/* MemApi, FileApi, TimeApi. */
void headless_mem_install(Wc3Platform *platform, HeadlessState *st);
void headless_file_install(Wc3Platform *platform, HeadlessState *st);
void headless_time_install(Wc3Platform *platform, HeadlessState *st);

/* ThreadApi, SyncApi. */
void headless_thread_install(Wc3Platform *platform, HeadlessState *st);
void headless_sync_install(Wc3Platform *platform, HeadlessState *st);

/* Window/Input/Audio/Clipboard/Surface/Log/Config/Asset/Locale/Vulkan. */
void headless_services_install(Wc3Platform *platform, HeadlessState *st);

/* Reset the input ring. */
void headless_input_reset(HeadlessState *st);

/* Free service-owned memory (clipboard, env, MPQ list, audio callback). */
void headless_services_destroy(HeadlessState *st);

#endif /* WC3_HEADLESS_INTERNAL_H */
