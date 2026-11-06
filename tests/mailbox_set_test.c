/*
 * mailbox_set_test.c - four concurrent producers, one consumer, zero loss.
 *
 * This is the executable reason for P0-8: a single SPSC Wc3Mailbox is unsafe
 * with several producers (push has no CAS on tail), while a per-producer set
 * is safe by construction.
 *
 * Build:
 *   gcc -std=c11 -O2 -Wall -Wextra -Werror -pthread \
 *       -I runtime/ipc tests/mailbox_set_test.c runtime/ipc/mailbox.c \
 *       runtime/ipc/mailbox_set.c -o build/host/mailbox_set_test
 */
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mailbox_set.h"

#define PRODUCERS 4
#define PER_PRODUCER 20000

typedef struct {
    Wc3MailboxSet *set;
    uint32_t id;
} ProducerArg;

typedef struct {
    uint32_t seen[PRODUCERS];
    uint32_t last[PRODUCERS];
    uint32_t total;
    int order_error;
} SinkState;

static void *producer_main(void *p)
{
    ProducerArg *a = (ProducerArg *)p;
    for (uint32_t seq = 0; seq < PER_PRODUCER; ++seq) {
        Wc3MailboxMsg msg = { .kind = a->id, .a = seq, .b = 0xABCDu };
        /* A bounded queue can be full: retry, but never let the consumer
         * finish before all messages were accepted. */
        while (!wc3_mailbox_set_push(a->set, a->id, &msg)) sched_yield();
    }
    return NULL;
}

static void sink(uint32_t producer, const Wc3MailboxMsg *msg, void *ctx)
{
    SinkState *s = (SinkState *)ctx;
    if (producer >= PRODUCERS || msg->kind != producer || msg->b != 0xABCDu) {
        s->order_error = 1;
        return;
    }
    /* Per-producer FIFO must be preserved by the SPSC queue. */
    if (msg->a != s->last[producer]) s->order_error = 1;
    s->last[producer]++;
    s->seen[producer]++;
    s->total++;
}

int main(void)
{
    Wc3MailboxSet set;
    wc3_mailbox_set_init(&set);
    SinkState st;
    memset(&st, 0, sizeof st);

    pthread_t th[PRODUCERS];
    ProducerArg args[PRODUCERS];
    for (uint32_t i = 0; i < PRODUCERS; ++i) {
        args[i].set = &set;
        args[i].id = i;
        if (pthread_create(&th[i], NULL, producer_main, &args[i]) != 0) {
            fprintf(stderr, "FAIL: pthread_create %u\n", i);
            return 1;
        }
    }
    /* Drain CONCURRENTLY: each SPSC queue holds only WC3_MAILBOX_CAP
     * messages, so joining producers before draining would deadlock. */
    const uint32_t expected = PRODUCERS * PER_PRODUCER;
    uint64_t idle = 0;
    while (st.total < expected) {
        uint32_t n = wc3_mailbox_set_drain(&set, 256u, sink, &st);
        if (n == 0) {
            if (++idle > 20000000ULL) break;  /* fail fast instead of hanging */
            sched_yield();
        } else {
            idle = 0;
        }
    }
    for (uint32_t i = 0; i < PRODUCERS; ++i) pthread_join(th[i], NULL);
    while (wc3_mailbox_set_drain(&set, 256u, sink, &st) != 0) { /* tail */ }

    if (st.total != PRODUCERS * PER_PRODUCER) {
        fprintf(stderr, "FAIL: lost messages: got %u, want %d\n",
                st.total, PRODUCERS * PER_PRODUCER);
        return 1;
    }
    for (uint32_t i = 0; i < PRODUCERS; ++i) {
        if (st.seen[i] != PER_PRODUCER) {
            fprintf(stderr, "FAIL: producer %u delivered %u\n", i, st.seen[i]);
            return 1;
        }
    }
    if (st.order_error) {
        fprintf(stderr, "FAIL: per-producer FIFO violated\n");
        return 1;
    }
    printf("mailbox_set_test: OK (%u messages, %u producers, FIFO preserved)\n",
           st.total, PRODUCERS);
    return 0;
}
