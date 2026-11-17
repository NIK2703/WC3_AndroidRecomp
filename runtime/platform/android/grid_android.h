#ifndef WC3_GRID_ANDROID_H
#define WC3_GRID_ANDROID_H

#include <android/native_window.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Native presenter for the half-resolution grid display mode.
 *
 * This is a static presentation layer: the pattern is rasterised once into the
 * grid SurfaceView buffer and re-posted only when the OLED cycle advances the
 * phase.  SurfaceFlinger composites the layer; there is no per-frame Java,
 * JNI or CPU work.
 *
 * Threading: configure/set_active/set_surface/clear_surface are UI-thread
 * functions.  The only other thread is the private cycle worker, which never
 * calls back into this API.  The worker is always joined before its state is
 * replaced, so a surface, visibility or settings change cannot race an
 * in-flight post.
 */

/*
 * Latched launch settings.  Called from nativeApplyLaunchSettings before the
 * grid surface exists, so the first posted buffer already carries the final
 * darken value.  `darken_percent` is clamped to 0..100 and `cycle_seconds` to
 * 0..3600; a non-positive cycle keeps one static phase.
 */
void grid_android_configure(int enabled, int darken_percent, int cycle_seconds);

/*
 * Attach the grid SurfaceView buffer.  Takes ownership of `window` on success
 * and on failure (matching overlay_start), so the JNI wrapper must not release
 * it separately.  The first frame is posted synchronously; a cycle worker is
 * started only when the effect is enabled and the interval is positive.
 * Returns 0 on success, -1 on invalid arguments.
 */
int grid_android_set_surface(ANativeWindow *window, int width, int height);

/*
 * Start/stop presentation for the activity's visible lifetime.  Inactive stops
 * the cycle worker and suppresses posting: a background window can stop
 * consuming its BufferQueue, and a producer that keeps locking it would block
 * the UI thread in the join.  Active reposts the current phase once and
 * restarts the interval.  The default after configure is active.
 */
void grid_android_set_active(int active);

/*
 * Detach the current surface and stop the cycle worker.  Called from the
 * matching surfaceDestroyed callback and again from onDestroy; idempotent.
 */
void grid_android_clear_surface(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_GRID_ANDROID_H */
