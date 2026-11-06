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

typedef struct HeadlessState {
    char asset_root[WC3_HEADLESS_PATH_MAX];
    char save_root[WC3_HEADLESS_PATH_MAX];
    uint32_t width;
    uint32_t height;
    char window_title[128];

    char *clipboard;
    pthread_mutex_t clipboard_lock;

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
