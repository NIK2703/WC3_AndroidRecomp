/*
 * softfloat_state.h - the four SoftFloat state bytes owned by a CPU.
 *
 * Upstream SoftFloat 3e keeps these values in thread-local globals.  A
 * dlopen'ed Android product cannot use initial-exec TLS for them, while
 * global-dynamic TLS resolves on every access.  WC3_SOFTFLOAT_CONTEXT moves
 * the state into CPU and passes it explicitly through the x87 hot path.
 *
 * The generated context tree includes this header after softfloat.h.  When
 * the context build is off, only the type is used (CPU.sf_state is still
 * present, so both variants have the same host ABI); the original globals
 * keep their upstream behaviour.
 */
#ifndef WC3_SOFTFLOAT_STATE_H
#define WC3_SOFTFLOAT_STATE_H

#include "softfloat.h"

#ifndef WC3_SOFTFLOAT_CONTEXT
#define WC3_SOFTFLOAT_CONTEXT 0
#endif

typedef struct softfloat_state {
    uint_fast8_t roundingMode;
    uint_fast8_t detectTininess;
    uint_fast8_t exceptionFlags;
    uint_fast8_t extF80RoundingPrecision;
} softfloat_state;

#if WC3_SOFTFLOAT_CONTEXT
/* Cold fallback state for old-name wrappers and SoftFloat functions outside
 * the transformed x87 closure.  The hot x87 path never reads it. */
extern THREAD_LOCAL softfloat_state softfloat_tls_state;
#define softfloat_detectTininess (softfloat_tls_state.detectTininess)
#define softfloat_roundingMode (softfloat_tls_state.roundingMode)
#define softfloat_exceptionFlags (softfloat_tls_state.exceptionFlags)
#define extF80_roundingPrecision (softfloat_tls_state.extF80RoundingPrecision)
#endif

#endif
