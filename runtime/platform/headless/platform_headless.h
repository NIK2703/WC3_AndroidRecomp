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

/* Explicit key/value configuration.  The headless target must not read the
 * process environment (DUT-1): the caller supplies every value, and the
 * platform keeps a stable copy for ConfigApi. */
typedef struct Wc3HeadlessConfigEntry {
    const char *key;
    const char *value;
} Wc3HeadlessConfigEntry;

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

#ifdef __cplusplus
}
#endif

#endif /* WC3_PLATFORM_HEADLESS_H */
