#include "mailbox.h"

void wc3_mailbox_init(Wc3Mailbox *mb)
{
    if (!mb) return;
    atomic_store_explicit(&mb->head, 0u, memory_order_relaxed);
    atomic_store_explicit(&mb->tail, 0u, memory_order_relaxed);
}

int wc3_mailbox_push(Wc3Mailbox *mb, const Wc3MailboxMsg *msg)
{
    if (!mb || !msg) return 0;
    uint32_t tail = atomic_load_explicit(&mb->tail, memory_order_relaxed);
    uint32_t next = (tail + 1u) % WC3_MAILBOX_CAP;
    uint32_t head = atomic_load_explicit(&mb->head, memory_order_acquire);
    if (next == head) return 0;
    mb->slot[tail] = *msg;
    atomic_store_explicit(&mb->tail, next, memory_order_release);
    return 1;
}

int wc3_mailbox_pop(Wc3Mailbox *mb, Wc3MailboxMsg *msg)
{
    if (!mb || !msg) return 0;
    uint32_t head = atomic_load_explicit(&mb->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&mb->tail, memory_order_acquire);
    if (head == tail) return 0;
    *msg = mb->slot[head];
    atomic_store_explicit(&mb->head, (head + 1u) % WC3_MAILBOX_CAP,
                          memory_order_release);
    return 1;
}

uint32_t wc3_mailbox_count(const Wc3Mailbox *mb)
{
    if (!mb) return 0;
    uint32_t head = atomic_load_explicit(&mb->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&mb->tail, memory_order_acquire);
    return (tail + WC3_MAILBOX_CAP - head) % WC3_MAILBOX_CAP;
}
