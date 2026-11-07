#ifndef WC3_HAPTICS_ANDROID_H
#define WC3_HAPTICS_ANDROID_H

#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Direct-JNI vibrator path copied from Winlator's sync_dispatch.c.
 *
 * init() must run while the JNI env is attached (the GameActivity
 * RegisterNatives path).  The input thread calls attach_thread() once before
 * draining actions and detach_thread() before it exits.  perform() is cold:
 * it only runs when the touch engine emits ACT_HAPTIC.
 */
int haptics_android_init(JNIEnv *env, JavaVM *vm, jobject activity);

void haptics_android_attach_thread(void);
void haptics_android_detach_thread(void);

void haptics_android_perform(int effect);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HAPTICS_ANDROID_H */
