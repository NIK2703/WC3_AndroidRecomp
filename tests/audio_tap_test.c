/* audio_tap_test.c - T0 for the N-16b preallocated post-mix tap ring. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_tap.h"

static int fail(const char *what)
{
    fprintf(stderr, "audio_tap: %s\n", what);
    return 1;
}

int main(void)
{
    uint8_t storage[64];
    Wc3AudioTapRing ring;
    int16_t input[32];
    int16_t output[32];
    float floats[8] = { -2.0f, -1.0f, -0.5f, 0.0f,
                        0.5f, 1.0f, 2.0f, NAN };
    static const int16_t expected[8] = {
        -32768, -32768, -16384, 0, 16384, 32767, 32767, 0
    };

    if (wc3_audio_tap_init(&ring, storage, 63u) == 0)
        return fail("non-power-of-two capacity accepted");
    if (wc3_audio_tap_init(&ring, storage, sizeof storage) != 0)
        return fail("valid ring init failed");

    for (unsigned i = 0; i < 32u; i++) input[i] = (int16_t)(i * 101);
    if (wc3_audio_tap_push_s16(&ring, input, 16u) != 0)
        return fail("s16 push failed");
    if (wc3_audio_tap_pop(&ring, output, sizeof output) != 32u)
        return fail("s16 pop size mismatch");
    if (memcmp(input, output, 32u) != 0)
        return fail("s16 roundtrip mismatch");

    if (wc3_audio_tap_push_s16(&ring, input, 32u) != 0)
        return fail("ring did not accept a full buffer");
    if (wc3_audio_tap_push_s16(&ring, input, 1u) == 0)
        return fail("full ring accepted an extra sample");
    if (wc3_audio_tap_dropped(&ring) != 2u)
        return fail("dropped sample was not counted");
    if (wc3_audio_tap_pop(&ring, output, 20u) != 20u)
        return fail("partial pop failed");

    /* Wrap the write pointer and check both spans. */
    if (wc3_audio_tap_push_s16(&ring, input, 10u) != 0)
        return fail("wrapped push failed");
    if (wc3_audio_tap_pop(&ring, output, sizeof output) != 64u)
        return fail("wrapped pop did not drain");
    for (unsigned i = 0; i < 22u; i++) {
        if (output[i] != input[10u + i]) return fail("old span mismatch");
    }
    for (unsigned i = 22u; i < 32u; i++) {
        if (output[i] != input[i - 22u]) return fail("wrapped span mismatch");
    }

    if (wc3_audio_tap_push_f32(&ring, floats, 8u) != 0)
        return fail("float push failed");
    if (wc3_audio_tap_pop(&ring, output, sizeof output) != 16u)
        return fail("float pop size mismatch");
    if (memcmp(expected, output, sizeof expected) != 0)
        return fail("float-to-s16 conversion mismatch");

    if (wc3_audio_tap_push_s16(&ring, input, 16u) != 0)
        return fail("post-float push failed");
    if (wc3_audio_tap_pop(&ring, output, 3u) != 2u)
        return fail("odd pop did not round down to a sample");
    if (wc3_audio_tap_pop(&ring, output, 30u) != 30u)
        return fail("post-odd pop drain failed");

    if (wc3_audio_tap_init(&ring, storage, sizeof storage) != 0)
        return fail("ring re-init failed");
    if (wc3_audio_tap_dropped(&ring) != 0u)
        return fail("re-init did not clear dropped counter");

    puts("audio_tap: ok");
    return 0;
}
