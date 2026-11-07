#ifndef WC3_OVERLAY_ANDROID_H
#define WC3_OVERLAY_ANDROID_H

#include <android/native_window.h>

#include "wc3_overlay.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Detached Android buffer adapter.  It is intentionally not referenced by the
 * recompiler build yet.  The eventual GameActivity overlay SurfaceView calls
 * these two functions from its native render thread.
 */

/* Configure the window for RGBA8.  Returns 0 on success. */
int overlay_android_configure(ANativeWindow *window, int width, int height);

/*
 * Lock the window, resize the overlay to the locked buffer if needed, render,
 * and post.  Returns 0 on success, -1 on a lock failure or a missing buffer.
 */
int overlay_android_present(ANativeWindow *window, Wc3Overlay *overlay);

#ifdef __cplusplus
}
#endif

#endif /* WC3_OVERLAY_ANDROID_H */
