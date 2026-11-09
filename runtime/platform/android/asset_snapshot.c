/*
 * asset_snapshot.c - dynamic name snapshot for the Android AssetApi.
 *
 * O-45: the old snapshot reserved 1024 * 256 bytes in .bss even when the
 * licensed asset directory contained a handful of MPQ names.  The table now
 * grows to the observed count and is shrunk once the scan is complete.  The
 * 1024-entry cap is intentional: a directory that large is not a normal Warcraft
 * III asset root, and the platform falls back to the live scan when the
 * snapshot is truncated.
 */
#include "asset_snapshot.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

/* Grow the name table to at least `want` entries.  The final table is shrunk
 * to the actual count after the directory scan; this helper only avoids a
 * reallocation per directory entry while the count is still unknown. */
static int snapshot_reserve(Wc3AssetSnapshot *snapshot, uint32_t want)
{
    if (want <= snapshot->capacity) return 0;
    if (want > WC3_ASSET_SNAPSHOT_MAX) return -1;

    uint32_t cap = snapshot->capacity ? snapshot->capacity : 64u;
    while (cap < want) {
        if (cap > WC3_ASSET_SNAPSHOT_MAX / 2u) {
            cap = WC3_ASSET_SNAPSHOT_MAX;
            break;
        }
        cap *= 2u;
    }
    if (cap < want) return -1;

    char (*names)[WC3_ASSET_NAME_MAX] =
        realloc(snapshot->names, (size_t)cap * WC3_ASSET_NAME_MAX);
    if (!names) return -1;
    snapshot->names = names;
    snapshot->capacity = cap;
    return 0;
}

void wc3_asset_snapshot_build(Wc3AssetSnapshot *snapshot, const char *dir)
{
    if (!snapshot) return;

    free(snapshot->names);
    snapshot->names = NULL;
    snapshot->count = 0u;
    snapshot->truncated = 0u;
    snapshot->open_failed = 0u;
    snapshot->capacity = 0u;

    if (!dir || !*dir) {
        snapshot->open_failed = 1u;
        return;
    }

    DIR *d = opendir(dir);
    if (!d) {
        snapshot->open_failed = 1u;
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len == 0u) continue;
        if (len >= WC3_ASSET_NAME_MAX) {
            snapshot->truncated = 1u;
            continue;
        }
        if (snapshot->count >= WC3_ASSET_SNAPSHOT_MAX) {
            snapshot->truncated = 1u;
            break;
        }
        if (snapshot_reserve(snapshot, snapshot->count + 1u) != 0) {
            snapshot->truncated = 1u;
            break;
        }
        memcpy(snapshot->names[snapshot->count], name, len + 1u);
        snapshot->count++;
    }
    (void)closedir(d);

    /* The temporary growth policy keeps at most one spare capacity step; the
     * steady-state snapshot owns exactly count entries. */
    if (snapshot->count == 0u) {
        free(snapshot->names);
        snapshot->names = NULL;
        snapshot->capacity = 0u;
    } else if (snapshot->count < snapshot->capacity) {
        char (*names)[WC3_ASSET_NAME_MAX] =
            realloc(snapshot->names,
                    (size_t)snapshot->count * WC3_ASSET_NAME_MAX);
        if (names) {
            snapshot->names = names;
            snapshot->capacity = snapshot->count;
        }
    }
}
