/* winmm_guard_hle.h - dead-end WINMM waveOut rows.
 *
 * WC3's Mss32 imports the eleven waveOut entry points but the product audio
 * path is DirectSound.  These rows stay registered because the guest corpus
 * has frozen WINMM slots, but they deliberately expose no devices and open no
 * stream.  This makes the removed as-built waveOut sink unreachable.
 */
#ifndef WC3_HLE_WINMM_GUARD_H
#define WC3_HLE_WINMM_GUARD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of replaced WINMM rows, or -1 when a row is missing. */
int wc3_hle_winmm_guard_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_WINMM_GUARD_H */
