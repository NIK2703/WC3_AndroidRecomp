/*
 * dsound_hle.h - DirectSound guest contract for the audio port.
 *
 * The 1.27b corpus imports winmm.dll and Mss32.dll, not dsound.dll.  Mss32
 * nevertheless selects its DirectSound driver through
 * LoadLibraryA("DSOUND.DLL") + GetProcAddress.  The install routine therefore
 * registers the COM rows and chains KERNEL32!LoadLibraryA/GetProcAddress to a
 * synthetic DSOUND.DLL handle, so the DirectSound path is reachable without a
 * corpus re-lift.  All rows are APPENDED at boot (ADR-034); the frozen
 * fast-bind slot indices of the shipped corpus do not move.
 */
#ifndef WC3_HLE_WIN32_DSOUND_HLE_H
#define WC3_HLE_WIN32_DSOUND_HLE_H

/* Idempotent.  Returns 0 on success, -1 when a registry row is unavailable.
 * Must run before the first guest thread. */
int wc3_hle_dsound_install(void);

#endif /* WC3_HLE_WIN32_DSOUND_HLE_H */
