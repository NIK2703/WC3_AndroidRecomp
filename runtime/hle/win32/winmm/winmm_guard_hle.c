/*
 * winmm_guard_hle.c - the only WINMM waveOut implementation in the product.
 *
 * The nfsu2 DirectSound architecture does not use waveOut.  Mss32 still imports
 * the old entry points, so the frozen corpus slots must resolve.  Every row
 * returns "no driver"/"invalid handle" and waveOutGetNumDevs returns zero.
 * No AAudio stream, WAVEHDR completion or old linear ring can be reached.
 */
#include <stdint.h>

#include "cpu.h"
#include "hle_bridge.h"
#include "winmm_guard_hle.h"

#define GUARD_MMSYSERR_INVALHANDLE 5u
#define GUARD_MMSYSERR_NODRIVER    6u

static void guard_get_num_devs(CPU *c)
{
    c->eax = 0u;
}

static void guard_get_dev_caps(CPU *c)
{
    uint32_t caps = wc3_legacy_arg32(c, 1);
    if (caps && wc3_legacy_guest_ok(caps, 4u))
        wc3_legacy_wr32g(caps, 0u);
    c->eax = GUARD_MMSYSERR_NODRIVER;
}

static void guard_open(CPU *c)
{
    c->eax = GUARD_MMSYSERR_NODRIVER;
}

static void guard_invalid_handle(CPU *c)
{
    c->eax = GUARD_MMSYSERR_INVALHANDLE;
}

static int guard_bind(const char *name, void (*impl)(CPU *c), int argc)
{
    /* The target tree no longer registers waveOut.  Keep the override branch
     * for a transitional build that still carries an as-built row; otherwise
     * add the guard row before the first guest thread. */
    if (wc3_legacy_hle_override("WINMM.dll", name, impl) == 0)
        return 0;
    return wc3_legacy_hle_add("WINMM.dll", name, argc, 0, impl);
}

int wc3_hle_winmm_guard_install(void)
{
    static const struct {
        const char *name;
        void (*impl)(CPU *c);
        int argc;
    } rows[] = {
        { "waveOutGetNumDevs",      guard_get_num_devs,      0 },
        { "waveOutGetDevCapsA",     guard_get_dev_caps,      3 },
        { "waveOutOpen",            guard_open,              6 },
        { "waveOutClose",           guard_invalid_handle,    1 },
        { "waveOutPrepareHeader",   guard_invalid_handle,    3 },
        { "waveOutUnprepareHeader", guard_invalid_handle,    3 },
        { "waveOutWrite",           guard_invalid_handle,    3 },
        { "waveOutReset",           guard_invalid_handle,    1 },
        { "waveOutGetID",           guard_invalid_handle,    2 },
        { "waveOutGetVolume",       guard_invalid_handle,    2 },
        { "waveOutSetVolume",       guard_invalid_handle,    2 },
    };

    int n = 0;
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        if (guard_bind(rows[i].name, rows[i].impl, rows[i].argc) != 0)
            return -1;
        n++;
    }
    return n;
}
