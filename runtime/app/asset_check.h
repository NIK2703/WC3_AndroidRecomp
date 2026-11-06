/*
 * asset_check.h - startup validation of the licensed MPQ set.
 *
 * The guest's Storm.dll still reads the MPQs; this module only proves that the
 * platform exposed the read-only asset root, the writable save root, and all
 * four TFT-era archives before guest startup.  It performs no I/O itself and
 * holds no lock.
 */
#ifndef WC3_ASSET_CHECK_H
#define WC3_ASSET_CHECK_H

#include <stdint.h>

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WC3_REQUIRED_MPQ_COUNT 4u

extern const char *const wc3_required_mpq[WC3_REQUIRED_MPQ_COUNT];

typedef struct Wc3AssetReport {
    uint32_t present_mask;
    uint32_t missing_mask;
    uint32_t missing_count;
    const char *missing[WC3_REQUIRED_MPQ_COUNT];
} Wc3AssetReport;

/* Returns 0 when all four MPQs are present, 1 when one or more are missing
 * (the report names them), and -1 for a broken AssetApi/root contract. */
int wc3_asset_validate(const Wc3AssetApi *asset, Wc3AssetReport *report);

#ifdef __cplusplus
}
#endif

#endif /* WC3_ASSET_CHECK_H */
