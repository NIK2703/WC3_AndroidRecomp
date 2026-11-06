#include "asset_check.h"
#include "platform.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t g_mask;
static const char *g_asset_root = "assets";
static const char *g_save_root = "saves";

static const char *fake_asset_root(void *ctx)
{
    (void)ctx;
    return g_asset_root;
}

static const char *fake_save_root(void *ctx)
{
    (void)ctx;
    return g_save_root;
}

static int fake_present(void *ctx, const char *name)
{
    (void)ctx;
    for (uint32_t i = 0; i < WC3_REQUIRED_MPQ_COUNT; i++) {
        if (strcmp(name, wc3_required_mpq[i]) == 0) {
            return (g_mask & (1u << i)) != 0;
        }
    }
    return 0;
}

static int fake_enumerate(void *ctx, uint32_t index, char *name, size_t cap)
{
    (void)ctx;
    static const char *const upper[WC3_REQUIRED_MPQ_COUNT] = {
        "WAR3.MPQ", "WAR3X.MPQ", "WAR3XLOCAL.MPQ", "WAR3PATCH.MPQ",
    };
    if (index >= WC3_REQUIRED_MPQ_COUNT) return 0;
    if (!name || cap < strlen(upper[index]) + 1u) return -1;
    memcpy(name, upper[index], strlen(upper[index]) + 1u);
    return 1;
}

static int fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    return 1;
}

int main(void)
{
    Wc3AssetApi asset;
    memset(&asset, 0, sizeof asset);
    asset.asset_root = fake_asset_root;
    asset.save_root = fake_save_root;
    asset.present = fake_present;

    Wc3AssetReport report;
    g_mask = 0;
    if (wc3_asset_validate(&asset, &report) != 1) return fail("empty mask accepted");
    if (report.missing_count != WC3_REQUIRED_MPQ_COUNT)
        return fail("missing count");
    if (report.missing_mask != 0xFu) return fail("missing mask");
    if (strcmp(report.missing[0], "war3.mpq") != 0) return fail("missing name");

    g_mask = 0x7u;
    if (wc3_asset_validate(&asset, &report) != 1) return fail("partial mask accepted");
    if (report.missing_count != 1 || report.missing_mask != 0x8u)
        return fail("partial mask report");
    if (strcmp(report.missing[0], "War3Patch.mpq") != 0)
        return fail("patch missing name");

    g_mask = 0xFu;
    if (wc3_asset_validate(&asset, &report) != 0) return fail("complete mask rejected");
    if (report.missing_count != 0 || report.missing_mask != 0)
        return fail("complete mask report");
    if (report.present_mask != 0xFu) return fail("present mask");

    g_asset_root = "";
    if (wc3_asset_validate(&asset, &report) != -1) return fail("empty asset root accepted");
    g_asset_root = "assets";
    g_save_root = NULL;
    if (wc3_asset_validate(&asset, &report) != -1) return fail("missing save root accepted");
    g_save_root = "saves";
    if (wc3_asset_validate(&asset, NULL) != -1) return fail("null report accepted");

    /* No `present` callback is a supported AssetApi shape; the fallback must
     * enumerate and compare case-insensitively. */
    asset.present = NULL;
    asset.enumerate_mpq = fake_enumerate;
    if (wc3_asset_validate(&asset, &report) != 0) return fail("enumerate fallback");
    if (report.present_mask != 0xFu) return fail("enumerate present mask");

    puts("asset API OK");
    return 0;
}
