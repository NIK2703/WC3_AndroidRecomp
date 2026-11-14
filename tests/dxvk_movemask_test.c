/*
 * T0: the DXVK x86 shim's movemask must preserve x86 byte order.
 *
 * The multiply/gather shortcut is easy to get backwards: the first byte of the
 * vector must become bit 0 of the result.  Exercise every 16-bit sign pattern
 * with random low bits, then a spread of full byte values.
 */
#include "../third_party/dxvk/shim/x86intrin.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t rng_state = 0x9E3779B9u;

static uint32_t rng_next(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static int scalar_movemask(const unsigned char bytes[16])
{
    int mask = 0;
    for (int i = 0; i < 16; i++)
        mask |= (bytes[i] >> 7) << i;
    return mask;
}

static int check_vector(const unsigned char bytes[16], unsigned case_id)
{
    __m128i v;
    int want = scalar_movemask(bytes);
    int got;
    memcpy(&v, bytes, sizeof v);
    got = _mm_movemask_epi8(v);
    if (got != want) {
        fprintf(stderr,
                "FAIL case %u: want 0x%04X, got 0x%04X; bytes:", case_id,
                (unsigned)want, (unsigned)got);
        for (int i = 0; i < 16; i++)
            fprintf(stderr, " %02X", bytes[i]);
        fputc('\n', stderr);
        return 1;
    }
    return 0;
}

int main(void)
{
    unsigned char bytes[16];
    unsigned failures = 0;
    unsigned cases = 0;

    _Static_assert(sizeof(__m128i) == 16, "movemask expects 16-byte vectors");

    /* Every possible sign-bit pattern; the low seven bits stay random. */
    for (unsigned sign = 0; sign < 0x10000u; sign++) {
        for (int i = 0; i < 16; i++)
            bytes[i] = (unsigned char)(rng_next() & 0x7Fu);
        for (int i = 0; i < 16; i++) {
            if (sign & (1u << i))
                bytes[i] |= 0x80u;
        }
        failures += (unsigned)check_vector(bytes, cases++);
    }

    /* Every byte value in every lane, with the remaining lanes random. */
    for (unsigned value = 0; value < 256u; value++) {
        for (int lane = 0; lane < 16; lane++) {
            for (int i = 0; i < 16; i++)
                bytes[i] = (unsigned char)rng_next();
            bytes[lane] = (unsigned char)value;
            failures += (unsigned)check_vector(bytes, cases++);
        }
    }

    if (failures != 0) {
        fprintf(stderr, "dxvk movemask: %u failure(s) in %u cases\n", failures,
                cases);
        return 1;
    }
    printf("dxvk movemask: ok (%u cases)\n", cases);
    return 0;
}
