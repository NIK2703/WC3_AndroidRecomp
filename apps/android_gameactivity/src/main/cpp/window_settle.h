/*
 * window_settle.h - pure timing rule for the first ANativeWindow.
 *
 * GameActivity can replace the initial surface after layout.  The boot thread
 * still requires a surface to survive one settle interval, but it must not
 * sleep unconditionally when the surface was published long before the thread
 * starts.  Keep the decision in a small header so host T0 can test it without
 * the Android NDK.
 */
#ifndef WC3_WINDOW_SETTLE_H
#define WC3_WINDOW_SETTLE_H

#include <stdint.h>

/* Milliseconds left before `since_ms` has survived `settle_ms`.
 * A zero result means the caller may use the window.  `since_ms == 0` means
 * "not published yet"; a backwards clock jump is treated as fresh so a bad
 * timestamp never skips the settle interval. */
static inline uint64_t wc3_window_settle_remaining_ms(uint64_t since_ms,
                                                      uint64_t now_ms,
                                                      uint64_t settle_ms)
{
    if (since_ms == 0u || now_ms < since_ms) return settle_ms;
    uint64_t elapsed = now_ms - since_ms;
    return elapsed >= settle_ms ? 0u : settle_ms - elapsed;
}

#endif /* WC3_WINDOW_SETTLE_H */
