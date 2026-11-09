/*
 * asset_snapshot.h - name snapshot for the Android AssetApi.
 *
 * The snapshot owns only a dynamically grown array of directory names.  The
 * platform keeps the asset root, the case-insensitive match, the live
 * stat/access check and the fallback scans; this module makes the storage and
 * the truncation limit testable without an Android device.
 */
#ifndef WC3_PLATFORM_ANDROID_ASSET_SNAPSHOT_H
#define WC3_PLATFORM_ANDROID_ASSET_SNAPSHOT_H

#include <stdint.h>

#define WC3_ASSET_SNAPSHOT_MAX 1024u
#define WC3_ASSET_NAME_MAX 256u

typedef struct Wc3AssetSnapshot {
    uint32_t count;
    uint32_t truncated;
    uint32_t open_failed;
    uint32_t capacity;
    char (*names)[WC3_ASSET_NAME_MAX];
} Wc3AssetSnapshot;

/* Rebuild the snapshot from `dir`.  The previous name array is released even
 * when the directory cannot be opened.  `open_failed` is left set so the
 * caller can retry after a mount or permission change. */
void wc3_asset_snapshot_build(Wc3AssetSnapshot *snapshot, const char *dir);

#endif /* WC3_PLATFORM_ANDROID_ASSET_SNAPSHOT_H */
