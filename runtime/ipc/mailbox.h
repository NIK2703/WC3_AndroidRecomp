/*
 * mailbox.h - bounded SPSC handoff from Android callbacks to the owning guest
 * thread.
 *
 * One producer (the Android UI/audio callback thread that already exists) and
 * one consumer (the guest thread that owns the state).  No lock, no allocation
 * and no wake syscall in the queue itself.
 */
#ifndef WC3_MAILBOX_H
#define WC3_MAILBOX_H

#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WC3_MAILBOX_CAP
#define WC3_MAILBOX_CAP 64u
#endif

typedef struct Wc3MailboxMsg {
    uint32_t kind;
    uint32_t a;
    uint32_t b;
} Wc3MailboxMsg;

typedef struct Wc3Mailbox {
    Wc3MailboxMsg slot[WC3_MAILBOX_CAP];
    _Atomic uint32_t head; /* consumer-owned */
    _Atomic uint32_t tail; /* producer-owned */
} Wc3Mailbox;

void wc3_mailbox_init(Wc3Mailbox *mb);
/* Returns 1 on success, 0 when full.  Call only from the single producer. */
int wc3_mailbox_push(Wc3Mailbox *mb, const Wc3MailboxMsg *msg);
/* Returns 1 and fills msg, or 0 when empty.  Call only from the consumer. */
int wc3_mailbox_pop(Wc3Mailbox *mb, Wc3MailboxMsg *msg);
uint32_t wc3_mailbox_count(const Wc3Mailbox *mb);

#ifdef __cplusplus
}
#endif

#endif /* WC3_MAILBOX_H */
