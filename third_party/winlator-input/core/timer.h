#ifndef TP_TIMER_H
#define TP_TIMER_H

#include <stdbool.h>

// Tick interval in ms, set by tp_timer_start() from user's native_tick_rate_hz.
// Readable by executor.c for tick-quantized per-bind delay conversion.
extern int g_timer_interval_ms;

// Start the C-side tick+drain timer on a dedicated pthread.
// The timer thread calls touch_processor_tick() -> dispatch_and_flush -> cx11_drain_and_dispatch()
// in a loop at ~60Hz.
// interval_ms: period between ticks (16 for ~60Hz).
// Must be called after nativeRegisterDispatcher() and nativeInitSocketWriter().
__attribute__((cold)) bool tp_timer_start(int interval_ms);

// Stop the timer thread and wait for it to finish.
// Safe to call even if timer was not started.
__attribute__((cold)) void tp_timer_stop(void);

void tp_timer_wake(void);

#endif
