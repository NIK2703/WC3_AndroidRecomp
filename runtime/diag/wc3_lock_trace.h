/*
 * wc3_lock_trace.h - targeted guest lock ownership tracing (diagnostic only).
 *
 * The lifted engine implements its own reader/writer lock in game.dll.  A
 * freeze where several guest threads wait on that lock cannot be explained by
 * the HLE event code alone: the owner is a guest thread whose acquisition
 * already returned.  This probe records lock acquire/release completions for
 * one guest lock address and dumps the ownership table when an acquisition
 * stays in flight far longer than any legitimate critical section.
 *
 * Normative rules: every instruction here is guarded by
 * #if WC3_DEBUG && WC3_DEBUG_HLE, and the module is compiled only into the
 * debug variant.  It never calls dispatch() and never holds its mutex across
 * a guest callback or wait.
 */
#ifndef WC3_DIAG_WC3_LOCK_TRACE_H
#define WC3_DIAG_WC3_LOCK_TRACE_H

#include <stdint.h>

#include "cpu.h"

/* game.dll global lock used by the object/list registry (0x6FD77D00). */
#define WC3_LOCK_TRACE_DEFAULT_VA 0x6FD77D00u

/*
 * Called at the start of a dispatcher invocation.  When `target` is the
 * acquire or release wrapper for the traced lock, records the operation as
 * in flight and fills the out parameters.  The caller must then call
 * wc3_lock_trace_leave() with the same values once the whole invocation
 * (including its tail-call chain) has returned.  Returns 1 when tracked.
 *
 * The lock wrappers are called only from L_6F095730 (acquire) and
 * L_6F096340 (release); both push one argument before the call, so the guest
 * stack at the wrapper entry is [ret][arg][caller_ret].
 */
int wc3_lock_trace_enter(const CPU *c, uint32_t target,
                         uint32_t *op, uint32_t *lock, uint32_t *arg,
                         uint32_t *ra, uint32_t *caller, uint32_t *gid);

/* Records that a tracked operation completed (the lock was acquired or
 * released).  Matching is by guest id, so a thread that never reaches this
 * point stays visible as an in-flight operation. */
void wc3_lock_trace_leave(uint32_t op, uint32_t lock, uint32_t arg,
                          uint32_t ra, uint32_t caller, uint32_t gid);

/* Starts the diagnostic monitor thread.  It does nothing unless the hang
 * watchdog trigger is present (WC3_HANG_WATCH or <WC3_DATA_DIR>/hang_watch_on),
 * matching hang_watch_enabled() in the as-built HLE. */
void wc3_lock_trace_start(void);

#endif /* WC3_DIAG_WC3_LOCK_TRACE_H */
