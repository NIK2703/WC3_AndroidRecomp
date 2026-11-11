/*
 * wc3_trace.h - debug-only ATrace/Perfetto slices for the Android product.
 *
 * Normative owner: docs/BUILD_VARIANTS.md (BV-6/BV-7) and
 * docs/PERFORMANCE_CONTRACT.md (PM-13/PM-18).  This is not a profiler:
 * simpleperf collects CPU samples and the system perfetto service collects
 * scheduling, ftrace and heapprofd data.  The only in-process piece is ATrace,
 * which gives those captures phase and wait context on the thread that ran.
 *
 * Every instruction here is guarded by WC3_DEBUG && WC3_DEBUG_PERF.  The
 * release build defines WC3_DEBUG=0, so the string literals and libandroid
 * calls are compiled out; enabling the tag in a release build is rejected by
 * wc3_debug.h.  ATrace section names are static strings, never guest data;
 * thread names may carry a host-assigned id, but never a guest string.
 */
#ifndef WC3_DIAG_WC3_TRACE_H
#define WC3_DIAG_WC3_TRACE_H

#ifndef WC3_DEBUG
#define WC3_DEBUG 0
#endif

#include "wc3_debug.h"

#if WC3_DEBUG && WC3_DEBUG_PERF

#include <android/trace.h>
#include <pthread.h>

typedef struct {
    int active;
} Wc3TraceScope;

static inline void wc3_trace_scope_cleanup(Wc3TraceScope *scope)
{
    if (scope->active) {
        ATrace_endSection();
        scope->active = 0;
    }
}

static inline void wc3_trace_scope_begin(Wc3TraceScope *scope,
                                          const char *name)
{
    if (ATrace_isEnabled()) {
        ATrace_beginSection(name);
        scope->active = 1;
    }
}

#define WC3_TRACE_CONCAT_(a, b) a##b
#define WC3_TRACE_CONCAT(a, b) WC3_TRACE_CONCAT_(a, b)

/*
 * Closes the slice automatically on every return path.  Use it at function
 * entry (or in the smallest block that owns the work) when a scope would
 * otherwise need an end call before each return statement.
 */
#define WC3_TRACE_SCOPE(name)                                                \
    Wc3TraceScope WC3_TRACE_CONCAT(wc3_trace_scope_, __LINE__)               \
        __attribute__((cleanup(wc3_trace_scope_cleanup))) = {0};             \
    wc3_trace_scope_begin(                                                   \
        &WC3_TRACE_CONCAT(wc3_trace_scope_, __LINE__), (name))

/* Explicit begin/end pair for a region whose end is not a C scope boundary. */
#define WC3_TRACE_BEGIN(name) ATrace_beginSection(name)
#define WC3_TRACE_END() ATrace_endSection()

/* Give app-owned threads a profile-visible name.  Linux truncates the name to
 * 15 bytes; keep the prefix stable so simpleperf/Perfetto and crash reports
 * agree.  This is debug-only, like every other instruction in this header. */
#define WC3_TRACE_THREAD_NAME(name)                                         \
    ((void)pthread_setname_np(pthread_self(), (name)))

#else

#define WC3_TRACE_SCOPE(name) ((void)0)
#define WC3_TRACE_BEGIN(name) ((void)0)
#define WC3_TRACE_END() ((void)0)
#define WC3_TRACE_THREAD_NAME(name) ((void)0)

#endif

#endif /* WC3_DIAG_WC3_TRACE_H */
