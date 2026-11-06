/*
 * headless_init.c - assemble and release the verification platform table.
 */
#include "headless_internal.h"

#include <stdlib.h>
#include <string.h>

static void headless_state_free(HeadlessState *st)
{
    if (!st) return;
    headless_services_destroy(st);
    (void)pthread_mutex_destroy(&st->clipboard_lock);
    free(st);
}

int wc3_headless_platform_init(Wc3Platform *platform,
                               const Wc3HeadlessConfig *config)
{
    if (!platform) return -1;
    if (g_headless) return -1; /* one backend per process */

    HeadlessState *st = (HeadlessState *)calloc(1, sizeof *st);
    if (!st) return -1;
    if (pthread_mutex_init(&st->clipboard_lock, NULL) != 0) {
        free(st);
        return -1;
    }

    uint32_t width = config && config->width ? config->width : 1024u;
    uint32_t height = config && config->height ? config->height : 768u;
    st->width = width;
    st->height = height;
    (void)headless_copy_string(st->window_title, sizeof st->window_title,
                               "wc3-headless");

    const char *save_root =
        config && config->save_root && *config->save_root ? config->save_root
                                                          : ".";
    const char *asset_root =
        config && config->asset_root && *config->asset_root
            ? config->asset_root
            : NULL;
    if (headless_asset_roots(st, asset_root, save_root) != 0) {
        (void)pthread_mutex_destroy(&st->clipboard_lock);
        free(st);
        return -1;
    }
    if (headless_config_init(st, config ? config->entries : NULL,
                             config ? config->entry_count : 0u) != 0) {
        (void)pthread_mutex_destroy(&st->clipboard_lock);
        free(st);
        return -1;
    }
    headless_input_reset(st);

    memset(platform, 0, sizeof *platform);
    headless_mem_install(platform, st);
    headless_file_install(platform, st);
    headless_time_install(platform, st);
    headless_thread_install(platform, st);
    headless_sync_install(platform, st);
    headless_services_install(platform, st);
    g_headless = st;
    return 0;
}

void wc3_headless_platform_shutdown(Wc3Platform *platform)
{
    if (!platform) return;
    HeadlessState *st = (HeadlessState *)platform->mem.ctx;
    if (st != g_headless) return; /* not this backend's table */
    g_headless = NULL;
    memset(platform, 0, sizeof *platform);
    headless_state_free(st);
}
