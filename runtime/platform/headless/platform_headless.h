/*
 * platform_headless.h - host-side backend for the verification DUT.
 *
 * The headless target is not a product: it exists so the same corpus, HLE and
 * guest runtime can run on a CI host without a device.  It provides the whole
 * Wc3Platform surface with a null window/audio/surface layer and deterministic
 * lifecycle, while memory, files, threads and synchronization are real host
 * primitives.
 *
 * One backend instance per process.  The caller owns the Wc3Platform table and
 * may install it with wc3_platform_install() exactly once.  The context is
 * heap allocated by wc3_headless_platform_init() and released by shutdown();
 * the table itself is cleared on shutdown, so do not use it afterwards.
 */
#ifndef WC3_PLATFORM_HEADLESS_H
#define WC3_PLATFORM_HEADLESS_H

#include <stdint.h>

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WC3_HEADLESS_PATH_MAX 1024u
#define WC3_HEADLESS_SCHEDULE_ID_MAX 64u
#define WC3_HEADLESS_SCRIPT_MAX 4096u

/* Explicit key/value configuration.  The headless target must not read the
 * process environment (DUT-1): the caller supplies every value, and the
 * platform keeps a stable copy for ConfigApi. */
typedef struct Wc3HeadlessConfigEntry {
    const char *key;
    const char *value;
} Wc3HeadlessConfigEntry;

/* Deterministic schedule selection for T1.  The policy controls the order in
 * which a cooperatively serialized guest thread releases the token; it does
 * not model guest priority yet (Wc3ThreadDesc has no priority field). */
typedef enum Wc3HeadlessSchedulePolicy {
    WC3_HEADLESS_SCHED_ROUND_ROBIN = 1,
    WC3_HEADLESS_SCHED_FIFO = 2,
    WC3_HEADLESS_SCHED_LIFO = 3,
    WC3_HEADLESS_SCHED_WAKE_REORDER = 4
} Wc3HeadlessSchedulePolicy;

typedef struct Wc3HeadlessScriptEvent {
    /* Monotonic virtual time relative to platform init. */
    uint64_t time_ns;
    Wc3InputEvent event;
} Wc3HeadlessScriptEvent;

typedef struct Wc3HeadlessScheduleConfig {
    /* One of Wc3HeadlessSchedulePolicy.  Zero means "no schedule" and is
     * rejected when this structure is supplied. */
    uint32_t policy;
    uint32_t seed;
    /* Stable schedule identity, at least one character. */
    const char *id;
    /* Realtime value at virtual monotonic zero.  Zero selects
     * 2000-01-01T00:00:00Z so local time never depends on the host date. */
    uint64_t epoch_unix_ns;
    /* Fixed UTC offset used by local_time(); range -1440..1440 minutes. */
    int32_t timezone_minutes;
    /* Extra delay before a scripted input event becomes visible. */
    uint32_t mailbox_delay_ns;
    /* Optional timed input script, sorted by time_ns. */
    const Wc3HeadlessScriptEvent *input_script;
    uint32_t input_count;
} Wc3HeadlessScheduleConfig;

typedef struct Wc3HeadlessConfig {
    /* Licensed MPQ directory.  NULL means no assets; asset.init() may set the
     * roots later. */
    const char *asset_root;
    /* Writable saves/replays/registry root.  NULL means the current directory. */
    const char *save_root;
    /* Null window size.  Zero selects 1024x768. */
    uint32_t width;
    uint32_t height;
    /* Optional explicit ConfigApi snapshot.  Duplicate keys are rejected. */
    const Wc3HeadlessConfigEntry *entries;
    uint32_t entry_count;
    /* NULL keeps host clocks and host thread scheduling.  Non-NULL selects a
     * virtual monotonic clock starting at zero and a cooperative schedule.
     * The schedule preempts only at platform blocking points (yield, sleep,
     * sync wait, join); only threads created through Wc3ThreadApi may block
     * on guest-visible sync objects while it is active. */
    const Wc3HeadlessScheduleConfig *schedule;
} Wc3HeadlessConfig;

/* Fill `platform` with the headless implementation.  Returns 0 on success.
 * The configuration strings are copied. */
int wc3_headless_platform_init(Wc3Platform *platform,
                               const Wc3HeadlessConfig *config);

/* Release the backend context and clear `platform`.  Safe on a NULL or
 * already-cleared table. */
void wc3_headless_platform_shutdown(Wc3Platform *platform);

/* Input mailbox consumer used by the DUT and by replay schedules.  The queue
 * has one producer and one consumer; returns 1 when an event was copied, 0
 * when empty, -1 on an invalid argument or before init. */
int wc3_headless_input_pop(Wc3InputEvent *event);

/* Number of events rejected because the input mailbox was full. */
uint32_t wc3_headless_input_dropped(void);

/* Virtual clock controls.  In host-clock mode now_ns() follows
 * CLOCK_MONOTONIC and advance_ns() returns -1.  With a schedule, monotonic
 * zero is virtual and advance_ns() publishes due scripted input. */
uint64_t wc3_headless_clock_now_ns(void);
int wc3_headless_clock_advance_ns(uint64_t ns);

/* Schedule identity for reports and tests.  Host-clock mode returns 0 or
 * NULL.  The hash is a deterministic 64-bit digest, not a replacement for
 * the SHA-256 identity written by the Python difftest driver. */
int wc3_headless_schedule_policy(void);
uint64_t wc3_headless_schedule_seed(void);
const char *wc3_headless_schedule_id(void);
uint64_t wc3_headless_schedule_hash(void);

/* S-1 (docs/AUDIO_PULL_IMPLEMENTATION_SPEC.md): deterministic pull twin.
 * These are T0/T1 verification hooks, not product API.  They never change
 * guest-visible state; enable_pull selects which advance() path a test drives,
 * set_burst fixes the simulated AAudio callback cadence, and pull_stats
 * exposes the monotone device clock and the FNV-1a hash of rendered PCM. */
typedef struct Wc3HeadlessPullStats {
    uint64_t play_frame;
    uint64_t published_frame;
    uint64_t consumed;
    uint64_t pcm_hash;
    uint32_t stale_frames;
    uint32_t publish_regions;
    uint32_t render_calls;
    uint32_t burst_frames;
    int pull_mode;
} Wc3HeadlessPullStats;

void wc3_headless_audio_enable_pull(int enabled);
/* Full nfsu2 port: switch the T1 DUT to the voice mixer twin.  Default off
 * keeps the legacy linear / S-1 pull replays byte-identical.  Call BEFORE
 * waveOutOpen() so the new stream is created with the mixer active; calling it
 * later also switches the current stream, but WINMM has already captured its
 * primary voice during open. */
void wc3_headless_audio_enable_mixer(int enabled);
void wc3_headless_audio_set_burst(uint32_t frames);
int wc3_headless_audio_pull_stats(uintptr_t stream,
                                  Wc3HeadlessPullStats *out);

#ifdef __cplusplus
}
#endif

#endif /* WC3_PLATFORM_HEADLESS_H */
