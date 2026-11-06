#include "asset_check.h"

#include <stddef.h>
#include <string.h>

const char *const wc3_required_mpq[WC3_REQUIRED_MPQ_COUNT] = {
    "war3.mpq",
    "War3x.mpq",
    "War3xlocal.mpq",
    "War3Patch.mpq",
};

static int ascii_equal_ci(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

static int enumerate_contains(const Wc3AssetApi *asset, const char *wanted)
{
    char name[260];
    for (uint32_t i = 0; i < 256u; i++) {
        int rc = asset->enumerate_mpq(asset->ctx, i, name, sizeof name);
        if (rc < 0) return -1;
        if (rc == 0) return 0;
        if (ascii_equal_ci(name, wanted)) return 1;
    }
    return -1;
}

int wc3_asset_validate(const Wc3AssetApi *asset, Wc3AssetReport *report)
{
    if (!asset || !report) return -1;
    memset(report, 0, sizeof *report);

    const char *asset_root = asset->asset_root ? asset->asset_root(asset->ctx) : NULL;
    const char *save_root = asset->save_root ? asset->save_root(asset->ctx) : NULL;
    if (!asset_root || !*asset_root || !save_root || !*save_root) return -1;
    if (!asset->present && !asset->enumerate_mpq) return -1;

    for (uint32_t i = 0; i < WC3_REQUIRED_MPQ_COUNT; i++) {
        const char *wanted = wc3_required_mpq[i];
        int found;
        if (asset->present) {
            found = asset->present(asset->ctx, wanted);
            if (found < 0) return -1;
        } else {
            found = enumerate_contains(asset, wanted);
            if (found < 0) return -1;
        }
        if (found) {
            report->present_mask |= 1u << i;
        } else {
            report->missing_mask |= 1u << i;
            report->missing[report->missing_count++] = wanted;
        }
    }
    return report->missing_count ? 1 : 0;
}
