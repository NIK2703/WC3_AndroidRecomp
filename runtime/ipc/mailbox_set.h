/*
 * mailbox_set.h - bounded per-producer SPSC handoff (P0-8).
 *
 * The architecture needs several Android producers (UI, input, overlay, IME,
 * canvas), but runtime/ipc/mailbox.h is explicitly SPSC: wc3_mailbox_push()
 * reads `tail` relaxed and stores it with release, with no CAS.  Two producers
 * on one queue can write the same slot and lose an event.
 *
 * Instead of turning that queue into a contended MPSC ring, each producer owns
 * its own SPSC queue and the owning guest thread is the single consumer that
 * drains all queues in a fixed priority order.  No CAS contention, no shared
 * `tail`, and queues are cache-line aligned to avoid false sharing between
 * producers.  Intra-queue head/tail padding is still pending (P1-21).
 */
#ifndef WC3_IPC_MAILBOX_SET_H
#define WC3_IPC_MAILBOX_SET_H

#include <stdint.h>

#include "mailbox.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    WC3_PROD_INPUT = 0,   /* highest priority: control latency */
    WC3_PROD_UI,
    WC3_PROD_IME,
    WC3_PROD_OVERLAY,
    WC3_PROD_CANVAS,
    WC3_MAILBOX_PRODUCERS
};

typedef struct Wc3PaddedMailbox {
    _Alignas(64) Wc3Mailbox q;
} Wc3PaddedMailbox;

typedef struct Wc3MailboxSet {
    Wc3PaddedMailbox producer[WC3_MAILBOX_PRODUCERS];
} Wc3MailboxSet;

/* Callback invoked by the single consumer for every drained message. */
typedef void (*wc3_mailbox_sink)(uint32_t producer, const Wc3MailboxMsg *msg,
                                 void *ctx);

void wc3_mailbox_set_init(Wc3MailboxSet *set);

/* Producer side.  `producer` must be a stable id owned by exactly one thread.
 * Returns 1 on success, 0 when that producer's queue is full. */
int wc3_mailbox_set_push(Wc3MailboxSet *set, uint32_t producer,
                         const Wc3MailboxMsg *msg);

/* Consumer side.  Drains in priority order; returns number of messages
 * delivered.  A bounded batch prevents one busy producer from starving the
 * rest when the queues are continuously refilled. */
uint32_t wc3_mailbox_set_drain(Wc3MailboxSet *set, uint32_t max_messages,
                               wc3_mailbox_sink sink, void *ctx);

uint32_t wc3_mailbox_set_pending(const Wc3MailboxSet *set, uint32_t producer);

#ifdef __cplusplus
}
#endif

#endif /* WC3_IPC_MAILBOX_SET_H */
