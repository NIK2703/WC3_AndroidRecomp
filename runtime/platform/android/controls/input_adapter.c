#include "wc3_input_adapter.h"

#include <string.h>

static void record_event(Wc3InputAdapter *adapter, const Wc3TouchEvent *event,
                         uint64_t event_ms)
{
    Wc3InputAdapterStats *stats = &adapter->stats;
    stats->action_mask |= 1u << event->phase;
    if (event->phase != (uint8_t)WC3_TOUCH_MOVE)
        stats->control_records++;

    Wc3InputRecord *record = &stats->history[stats->history_head];
    record->time_ms = event_ms;
    record->pointer_id = event->pointer_id;
    record->phase = event->phase;
    record->_pad[0] = record->_pad[1] = record->_pad[2] = 0;
    stats->history_head = (stats->history_head + 1) % WC3_INPUT_HISTORY;
    if (stats->history_count < WC3_INPUT_HISTORY)
        stats->history_count++;
}

void input_adapter_init(Wc3InputAdapter *adapter, Wc3InputRing *ring)
{
    if (!adapter) return;
    memset(adapter, 0, sizeof(*adapter));
    adapter->ring = ring;
    if (ring) {
        Wc3InputRingStats stats;
        input_ring_get_stats(ring, &stats);
        adapter->last_faults = stats.faults;
    }
}

int input_adapter_drain(Wc3InputAdapter *adapter, uint64_t now_ms)
{
    if (!adapter || !adapter->ring) return -1;

    Wc3InputRingStats ring_stats;
    input_ring_get_stats(adapter->ring, &ring_stats);
    if (ring_stats.faults != adapter->last_faults) {
        /* A control was lost or the ring reported a fault.  Cancel every
         * engine-held action; the sink emits the synthetic UP. */
        controls_cancel_all();
        input_ring_clear(adapter->ring);
        adapter->last_faults = ring_stats.faults;
        adapter->stats.faults = ring_stats.faults;
        adapter->stats.recovered++;
    }

    Wc3TouchEvent batch[WC3_INPUT_BATCH_MAX];
    int n = input_ring_pop_batch(adapter->ring, batch, WC3_INPUT_BATCH_MAX);
    int rc = 0;

    for (int i = 0; i < n; i++) {
        uint64_t event_ms = batch[i].event_time_ms;
        if (event_ms > now_ms) {
            adapter->stats.early_events++;
            event_ms = now_ms;
        }
        if (event_ms == 0) event_ms = now_ms;
        if (event_ms < adapter->last_event_ms)
            event_ms = adapter->last_event_ms;
        adapter->last_event_ms = event_ms;

        if (controls_touch((Wc3TouchPhase)batch[i].phase,
                               batch[i].pointer_id, batch[i].x, batch[i].y,
                               event_ms) != 0)
            rc = -1;
        record_event(adapter, &batch[i], event_ms);
    }

    /* Timer deadlines are not delivery polling: one tick after the whole
     * batch, using the input thread's monotonic clock. */
    if (controls_tick(now_ms) != 0) rc = -1;

    Wc3InputAdapterStats *stats = &adapter->stats;
    stats->drains++;
    stats->events += (uint64_t)n;
    stats->ticks++;
    stats->last_batch = n;
    if (n > stats->max_batch) stats->max_batch = n;
    stats->last_tick_ms = now_ms;
    return rc;
}

uint64_t input_adapter_next_deadline_ms(const Wc3InputAdapter *adapter,
                                            uint64_t now_ms)
{
    if (!adapter || !adapter->ring) return 0;
    return controls_next_deadline_ms(now_ms);
}

const Wc3InputAdapterStats *input_adapter_stats(
    const Wc3InputAdapter *adapter)
{
    return adapter ? &adapter->stats : NULL;
}
