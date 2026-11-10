/* mss_timer_hle.h - USER32 timer/message pump required by Mss32.
 *
 * This is not an audio transport.  Mss32 creates its 50 ms mixer timer with
 * USER32!SetTimer and pumps it from GetMessage/PeekMessage while its owner
 * thread waits.  The old WINMM waveOut path owned that timer table; with the
 * nfsu2 DirectSound architecture the timer survives on its own.
 */
#ifndef WC3_HLE_WINMM_MSS_TIMER_H
#define WC3_HLE_WINMM_MSS_TIMER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the USER32 timer overrides and the pump/wait slices.  Returns the
 * number of installed rows, or -1 when an as-built row is missing.  Must run
 * from wc3_dispatch_init() before the first guest thread. */
int wc3_hle_mss_timer_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_WINMM_MSS_TIMER_H */
