#ifndef WC3_OVERLAY_CANVAS_H
#define WC3_OVERLAY_CANVAS_H

#include <jni.h>

#include "overlay.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One-shot Canvas atlas builder.
 *
 * The hot path stays in C: this bridge asks the app's Kotlin Canvas helper to
 * rasterise the control layers once, copies the resulting RGBA bitmap into a
 * straight-alpha cache, and hands it back to the C compositor.  Per-frame
 * rendering, visual-state changes and touch handling never enter Java.
 */
int overlay_canvas_attach(JNIEnv *env, jobject activity);
int overlay_canvas_build(Wc3Overlay *overlay);
void overlay_canvas_detach(JNIEnv *env);

#ifdef __cplusplus
}
#endif

#endif /* WC3_OVERLAY_CANVAS_H */
