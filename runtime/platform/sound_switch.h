/*
 * sound_switch.h - the product switch for the guest's audio path.
 *
 * The launch settings can turn sound off completely.  The decision is latched
 * in the environment before android_main starts (WC3_SOUND=0), so every layer
 * that could bring the audio subsystem up reads the same value:
 *
 *   - the platform refuses to open an AAudio stream, so no mixer, no
 *     supervisor thread and no real-time callback exist at all;
 *   - the DirectSound HLE answers DSERR_NODRIVER, so the guest never gets an
 *     audio device and stays on its own silent path.
 *
 * A missing variable keeps sound on: the product default is sound on, and a
 * host launch without the launch bundle must not change it.
 */
#ifndef WC3_PLATFORM_SOUND_SWITCH_H
#define WC3_PLATFORM_SOUND_SWITCH_H

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "0" means the guest has no audio device and the platform opens no stream. */
static inline int wc3_sound_enabled(void)
{
    const char *value = getenv("WC3_SOUND");
    return !(value && value[0] == '0' && value[1] == '\0');
}

#ifdef __cplusplus
}
#endif

#endif /* WC3_PLATFORM_SOUND_SWITCH_H */
