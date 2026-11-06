#include "mailbox_set.h"

/* Traversal order is explicit: input first (latency), then UI, IME, overlay,
 * canvas.  It is NOT the numeric order of the producer ids. */
static const uint32_t kDrainOrder[WC3_MAILBOX_PRODUCERS] = {
    WC3_PROD_INPUT, WC3_PROD_UI, WC3_PROD_IME,
    WC3_PROD_OVERLAY, WC3_PROD_CANVAS
};

void wc3_mailbox_set_init(Wc3MailboxSet *set)
{
    if (!set) return;
    for (uint32_t i = 0; i < WC3_MAILBOX_PRODUCERS; ++i)
        wc3_mailbox_init(&set->producer[i].q);
}

int wc3_mailbox_set_push(Wc3MailboxSet *set, uint32_t producer,
                         const Wc3MailboxMsg *msg)
{
    if (!set || !msg || producer >= WC3_MAILBOX_PRODUCERS) return 0;
    return wc3_mailbox_push(&set->producer[producer].q, msg);
}

uint32_t wc3_mailbox_set_drain(Wc3MailboxSet *set, uint32_t max_messages,
                               wc3_mailbox_sink sink, void *ctx)
{
    if (!set || !sink || max_messages == 0) return 0;
    uint32_t delivered = 0;
    for (uint32_t k = 0; k < WC3_MAILBOX_PRODUCERS && delivered < max_messages; ++k) {
        uint32_t producer = kDrainOrder[k];
        Wc3Mailbox *q = &set->producer[producer].q;
        /* Fairness: at most one queue's worth per pass, but never exceed the
         * caller's batch limit.  A producer that refills continuously cannot
         * indefinitely postpone the lower-priority queues. */
        uint32_t budget = WC3_MAILBOX_CAP;
        if (budget > max_messages - delivered) budget = max_messages - delivered;
        while (budget) {
            Wc3MailboxMsg msg;
            if (!wc3_mailbox_pop(q, &msg)) break;
            sink(producer, &msg, ctx);
            ++delivered;
            --budget;
        }
    }
    return delivered;
}

uint32_t wc3_mailbox_set_pending(const Wc3MailboxSet *set, uint32_t producer)
{
    if (!set || producer >= WC3_MAILBOX_PRODUCERS) return 0;
    return wc3_mailbox_count(&set->producer[producer].q);
}
