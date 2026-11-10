/*
 * rand_hle.c - target-side MSVCR80 srand/rand pair.
 *
 * MSVC's generator is the classic 32-bit LCG:
 *
 *     state = state * 214013 + 2531011
 *     result = (state >> 16) & 0x7FFF
 *
 * srand(1) is the CRT default, so rand() must return 41 before the guest makes
 * any srand call.  The sequence is part of the font-header key derivation in
 * game.dll; a host rand() with different constants makes the decoder reject
 * every font file.
 */
#include "rand_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "hle_bridge.h"

static uint32_t g_rand_state = 1u;

static void h_srand(CPU *c)
{
    g_rand_state = wc3_legacy_arg32(c, 0);
    c->eax = 0;
}

static void h_rand(CPU *c)
{
    g_rand_state = g_rand_state * 214013u + 2531011u;
    c->eax = (g_rand_state >> 16) & 0x7FFFu;
}

typedef struct RandOverride {
    const char *name;
    void (*impl)(CPU *c);
} RandOverride;

static const RandOverride g_overrides[] = {
    { "srand", h_srand },
    { "rand", h_rand },
};

int wc3_hle_msvcr80_rand_install(void)
{
    g_rand_state = 1u;
    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("MSVCR80.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0]);
}
