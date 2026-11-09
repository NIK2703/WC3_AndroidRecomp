/*
 * dsound_hle.h - DirectSound guest contract for the full nfsu2 audio port.
 *
 * The current 1.27b corpus imports winmm.dll (35 rows), not dsound.dll.
 * The rows are APPENDED to the registry at boot (ADR-034), so the frozen
 * fast-bind slot indices of the shipped corpus do not move.  Binding them to
 * a corpus that actually imports dsound.dll requires a re-lift (G-14/G-15,
 * GUEST_ABI.md); until then the active WINMM path uses the same host mixer.
 */
#ifndef WC3_HLE_WIN32_DSOUND_HLE_H
#define WC3_HLE_WIN32_DSOUND_HLE_H

/* Idempotent.  Returns 0 on success, -1 when a registry row is unavailable.
 * Must run before the first guest thread. */
int wc3_hle_dsound_install(void);

#endif /* WC3_HLE_WIN32_DSOUND_HLE_H */
