#ifndef WC3_INPUT_WAKE_H
#define WC3_INPUT_WAKE_H

/*
 * Blocking wake primitive for the C input thread.
 *
 * Android/Linux uses eventfd; the Windows host tests use an auto-reset event.
 * A successful wait consumes one wake.  The ring remains the source of truth,
 * so a coalesced wake is safe.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Wc3InputWake Wc3InputWake;

Wc3InputWake *input_wake_create(void);
void input_wake_destroy(Wc3InputWake *wake);

/* Safe to call from the touch callback. */
void input_wake_signal(Wc3InputWake *wake);

/* 1 = signaled, 0 = timeout, -1 = error.  timeout_ms < 0 waits forever. */
int input_wake_wait(Wc3InputWake *wake, int timeout_ms);

/* Consume a wake that is already pending, if any. */
void input_wake_drain(Wc3InputWake *wake);

#ifdef __cplusplus
}
#endif

#endif /* WC3_INPUT_WAKE_H */
