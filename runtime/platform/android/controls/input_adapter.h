#ifndef WC3_INPUT_ADAPTER_H
#define WC3_INPUT_ADAPTER_H

/*
 * Input-thread adapter: drain a batch from the ring, feed the touch engine,
 * then service engine timers once.  It never allocates on the drain path.
 */

#include <stdint.h>

#include "controls.h"
#include "input_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WC3_INPUT_BATCH_MAX 64
#define WC3_INPUT_HISTORY 16

typedef struct {
    uint64_t time_ms;
    int32_t pointer_id;
    uint8_t phase;
    uint8_t _pad[3];
} Wc3InputRecord;

typedef struct {
    uint64_t drains;
    uint64_t events;
    uint64_t ticks;
    uint64_t faults;
    uint64_t recovered;
    uint64_t control_records;
    uint64_t early_events;
    uint64_t last_event_ms;
    uint64_t last_tick_ms;
    int last_batch;
    int max_batch;
    uint32_t action_mask;       /* bit (1 << Wc3TouchPhase) */
    Wc3InputRecord history[WC3_INPUT_HISTORY];
    int history_count;
    int history_head;
} Wc3InputAdapterStats;

typedef struct {
    Wc3InputRing *ring;
    uint64_t last_faults;
    uint64_t last_event_ms;
    Wc3InputAdapterStats stats;
} Wc3InputAdapter;

void input_adapter_init(Wc3InputAdapter *adapter, Wc3InputRing *ring);

/* Process one wake batch and tick once after it.  now_ms is CLOCK_MONOTONIC
 * milliseconds at the input thread.  Returns 0 or -1 if a touch/tick failed. */
int input_adapter_drain(Wc3InputAdapter *adapter, uint64_t now_ms);

uint64_t input_adapter_next_deadline_ms(const Wc3InputAdapter *adapter,
                                            uint64_t now_ms);

const Wc3InputAdapterStats *input_adapter_stats(
    const Wc3InputAdapter *adapter);

#ifdef __cplusplus
}
#endif

#endif /* WC3_INPUT_ADAPTER_H */
