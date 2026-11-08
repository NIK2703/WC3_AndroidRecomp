/* waveout_hle.h - WINMM waveOut slice on the platform Wc3AudioApi.
 *
 * A-4/A-12.4 (rev 6) from docs/AUDIO_ARCHITECTURE.md: the platform owns the
 * AAudio RT callback and a non-RT completion pump; winmm owns the WAVEHDR
 * pool; WOM_DONE is delivered from a dedicated guest callback thread;
 * WM_TIMER is an independent wall-clock actor.  Installs target-tree
 * handlers over the quarantined WINMM rows through wc3_legacy_hle_override
 * (R15/H6.3); runtime/legacy stays read-only.
 */
#ifndef WC3_HLE_WINMM_WAVEOUT_H
#define WC3_HLE_WINMM_WAVEOUT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of installed rows (0 when disabled), or -1 when the
 * as-built table has no WINMM row to replace.  Must run from
 * wc3_dispatch_init() before the first guest thread.  The rev-6 architecture
 * is OPT-IN until S5 device acceptance: <WC3_DATA_DIR>/wave2_v2_on enables
 * it, wave2_v2_off always wins; debug env WC3_AUDIO_V2 overrides. */
int wc3_hle_winmm_waveout_install(void);

/* Diagnostic only: frames actually handed to AAudio by the audio thread. */
unsigned long long wc3_hle_winmm_played_frames(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_WINMM_WAVEOUT_H */
