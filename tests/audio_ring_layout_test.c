/*
 * audio_ring_layout_test.c - T0 guard for PASS11 O-107/O-108.
 *
 * The Android AAudio sink is not compiled on the host, but its SPSC layout is
 * pure C.  This test pins the properties that the RT path depends on:
 *   - the platform ring is the measured 64 KiB capacity;
 *   - read_pos and write_pos are one cache line apart;
 *   - the counters remain usable 32-bit atomics.
 */
#include "audio_ring_layout.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static int g_failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL %s\n", what);
        g_failures++;
    }
}

int main(void)
{
    check(WC3_AUDIO_RING_BYTES == 65536u, "ring is 64 KiB");
    check((WC3_AUDIO_RING_BYTES & (WC3_AUDIO_RING_BYTES - 1u)) == 0u,
          "ring capacity is a power of two");
    check(offsetof(Wc3AudioRingPositions, write_pos) -
              offsetof(Wc3AudioRingPositions, read_pos) ==
              WC3_AUDIO_CACHE_LINE_BYTES,
          "producer and consumer counters are one line apart");
    check(sizeof(Wc3AudioRingPositions) == WC3_AUDIO_CACHE_LINE_BYTES * 2u,
          "ring positions own two complete cache lines");

    Wc3AudioRingPositions positions;
    atomic_init(&positions.read_pos, 0u);
    atomic_init(&positions.write_pos, 0u);

    uintptr_t read_line =
        (uintptr_t)&positions.read_pos / WC3_AUDIO_CACHE_LINE_BYTES;
    uintptr_t write_line =
        (uintptr_t)&positions.write_pos / WC3_AUDIO_CACHE_LINE_BYTES;
    check(read_line != write_line, "live counters do not share a cache line");

    atomic_store_explicit(&positions.write_pos, 0x12345678u,
                          memory_order_release);
    atomic_store_explicit(&positions.read_pos, 0x0BADF00Du,
                          memory_order_release);
    check(atomic_load_explicit(&positions.write_pos, memory_order_acquire) ==
              0x12345678u,
          "write_pos round-trips");
    check(atomic_load_explicit(&positions.read_pos, memory_order_acquire) ==
              0x0BADF00Du,
          "read_pos round-trips");

    if (g_failures) {
        printf("audio_ring_layout: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("audio_ring_layout: 64 KiB ring; read/write counters separated\n");
    return 0;
}
