#ifndef WC3_WATCH_STATE_H
#define WC3_WATCH_STATE_H

#include <stdint.h>

/*
 * Read-only snapshot of the hang-watchdog state for one guest thread.
 *
 * The lock trace uses this to print where a lock holder was last executing
 * and which guest object it is waiting on.  The snapshot is deliberately
 * lock-free and only compiled when WC3_DEBUG_HLE is on.
 */
typedef struct Wc3WatchState {
    int valid;
    int main_thread;
    int running;
    uint32_t gid;
    uint32_t last_target, last_eip, last_esp, last_ebp;
    uint32_t dispatch_count;
    int wait_active;
    uint32_t wait_ra, wait_h0, wait_h1;
    uint32_t exit_code;
} Wc3WatchState;

/* Returns 1 and fills *out when gid is a known guest thread (or the main
 * guest thread).  Returns 0 when the gid is not known. */
int wc3_watch_state_get(uint32_t gid, Wc3WatchState *out);

#endif /* WC3_WATCH_STATE_H */
