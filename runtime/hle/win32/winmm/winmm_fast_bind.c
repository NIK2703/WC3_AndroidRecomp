/* winmm_fast_bind.c - product fast binding for as-built WINMM rows.
 *
 * This list used to be executed by the disabled branch of waveout_hle.c.
 * O-91 excludes that v2 sink from release, so the binding moved here: the
 * product keeps the same fast registry in every variant, while v2 remains
 * available to debug/headless evidence.
 */
#include <stdio.h>

#include "legacy_shim.h"
#include "wc3_debug.h"
#include "winmm_fast_bind.h"

static const char *const g_rows[] = {
    "waveOutGetNumDevs",
    "waveOutGetDevCapsA",
    "waveOutOpen",
    "waveOutClose",
    "waveOutPrepareHeader",
    "waveOutUnprepareHeader",
    "waveOutWrite",
    "waveOutReset",
    "waveOutGetID",
    "waveOutGetVolume",
    "waveOutSetVolume",
};

int wc3_hle_winmm_fast_bind_rows(void)
{
    int bound = 0;
    for (unsigned i = 0; i < sizeof g_rows / sizeof g_rows[0]; i++) {
        if (wc3_legacy_hle_fast_bind("WINMM.dll", g_rows[i]) == 0) {
            bound++;
            continue;
        }
#if WC3_DEBUG && WC3_DEBUG_HLE
        fprintf(stderr, "[winmm] fast-bind missing WINMM row %s\n",
                g_rows[i]);
#endif
    }
    return bound;
}
