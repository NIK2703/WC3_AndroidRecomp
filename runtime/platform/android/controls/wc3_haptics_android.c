/*
 * Direct-JNI vibrator delivery for the overlay touch actions.
 *
 * This is the WC3 adaptation of
 * winlator-enhanced-inpu-controls-2/app/src/main/cpp/winlator/dispatch/
 * sync_dispatch.c.  The effect mapping and API fallbacks are kept the same,
 * including the source C path's raw predefined IDs:
 *
 *   1 tick         API 33 composition, API 29 predefined 0, one-shot
 *   2 click        predefined 1 (source C path), one-shot 30
 *   3 heavy click  predefined 2 (source C path), one-shot 50
 *   4 double click predefined 3 (source C path), one-shot 30
 *   5 pulse        one-shot 10 ms
 *
 * The input thread is attached once, not on every touch.  perform() is cold
 * and only runs when wc3_controls emits ACT_HAPTIC.
 */

#include "wc3_haptics_android.h"

#include <stdbool.h>
#include <stddef.h>

#include "wc3_debug.h"

#if WC3_DEBUG && WC3_DEBUG_INPUT
#include <android/log.h>
#define TAG "wc3haptic"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGD(...) ((void)0)
#define LOGE(...) ((void)0)
#endif

static JavaVM *g_vm;
static jobject g_vibrator;
static jclass g_ve_cls;
static jclass g_comp_cls;
static jmethodID g_ve_create_one_shot;
static jmethodID g_ve_create_predefined;
static jmethodID g_ve_start_composition;
static jmethodID g_comp_add_primitive;
static jmethodID g_comp_compose;
static jmethodID g_vibrator_has_vibrator;
static jmethodID g_vibrator_vibrate_ve;
static jmethodID g_vibrator_all_primitives;
static int g_sdk_int = 26;
static jboolean g_has_vibrator = JNI_FALSE;

static _Thread_local JNIEnv *g_env;
static _Thread_local int g_thread_attached;

static void clear_exception(JNIEnv *env)
{
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
}

static void delete_global_refs(JNIEnv *env)
{
    if (g_vibrator) {
        (*env)->DeleteGlobalRef(env, g_vibrator);
        g_vibrator = NULL;
    }
    if (g_ve_cls) {
        (*env)->DeleteGlobalRef(env, g_ve_cls);
        g_ve_cls = NULL;
    }
    if (g_comp_cls) {
        (*env)->DeleteGlobalRef(env, g_comp_cls);
        g_comp_cls = NULL;
    }
}

static void ve_vibrate(JNIEnv *env, jobject effect)
{
    if (!effect)
        return;
    if (g_vibrator && g_vibrator_vibrate_ve) {
        (*env)->CallVoidMethod(env, g_vibrator, g_vibrator_vibrate_ve,
                               effect);
        clear_exception(env);
    }
    (*env)->DeleteLocalRef(env, effect);
}

static jobject ve_create_one_shot(JNIEnv *env, jlong ms, jint amplitude)
{
    if (!g_ve_cls || !g_ve_create_one_shot)
        return NULL;
    jobject effect = (*env)->CallStaticObjectMethod(
        env, g_ve_cls, g_ve_create_one_shot, ms, amplitude);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return NULL;
    }
    return effect;
}

static void play_predefined(JNIEnv *env, int effect_id, int fallback_ms)
{
    if (g_sdk_int >= 29 && g_ve_cls && g_ve_create_predefined) {
        jobject effect = (*env)->CallStaticObjectMethod(
            env, g_ve_cls, g_ve_create_predefined, (jint)effect_id);
        if (!(*env)->ExceptionCheck(env)) {
            if (effect) {
                ve_vibrate(env, effect);
                return;
            }
        } else {
            (*env)->ExceptionClear(env);
        }
    }
    ve_vibrate(env, ve_create_one_shot(env, (jlong)fallback_ms, 255));
}

static void play_tick(JNIEnv *env)
{
    /* Keep the source's API-33 composition path.  Its JNI signature check is
     * intentionally unchanged; when the primitive query is unavailable the
     * predefined tick below is the same fallback the source takes. */
    if (g_sdk_int >= 33 && g_comp_cls && g_ve_start_composition &&
        g_comp_add_primitive && g_comp_compose && g_vibrator_all_primitives) {
        jboolean supported = (*env)->CallBooleanMethod(
            env, g_vibrator, g_vibrator_all_primitives, 1);
        if (!(*env)->ExceptionCheck(env) && supported) {
            jobject comp = (*env)->CallStaticObjectMethod(
                env, g_ve_cls, g_ve_start_composition);
            if (!(*env)->ExceptionCheck(env) && comp) {
                jobject comp2 = (*env)->CallObjectMethod(
                    env, comp, g_comp_add_primitive, 1, 1.0f, 0);
                if (!(*env)->ExceptionCheck(env)) {
                    if (comp2)
                        (*env)->DeleteLocalRef(env, comp2);
                    jobject composed = (*env)->CallObjectMethod(
                        env, comp, g_comp_compose);
                    if ((*env)->ExceptionCheck(env))
                        (*env)->ExceptionClear(env);
                    else
                        ve_vibrate(env, composed);
                    (*env)->DeleteLocalRef(env, comp);
                    return;
                }
                (*env)->ExceptionClear(env);
                (*env)->DeleteLocalRef(env, comp);
            } else {
                if ((*env)->ExceptionCheck(env))
                    (*env)->ExceptionClear(env);
                if (comp)
                    (*env)->DeleteLocalRef(env, comp);
            }
        } else {
            if ((*env)->ExceptionCheck(env))
                (*env)->ExceptionClear(env);
        }
    }

    if (g_sdk_int >= 29 && g_ve_cls && g_ve_create_predefined) {
        jobject effect = (*env)->CallStaticObjectMethod(
            env, g_ve_cls, g_ve_create_predefined, 0);
        if (!(*env)->ExceptionCheck(env)) {
            if (effect) {
                ve_vibrate(env, effect);
                return;
            }
        } else {
            (*env)->ExceptionClear(env);
        }
    }
    ve_vibrate(env, ve_create_one_shot(env, 30, 255));
}

static void perform_locked(JNIEnv *env, int effect)
{
    switch (effect) {
    case 1:
        play_tick(env);
        break;
    case 2:
        play_predefined(env, 1, 30);
        break;
    case 3:
        play_predefined(env, 2, 50);
        break;
    case 4:
        play_predefined(env, 3, 30);
        break;
    case 5:
        ve_vibrate(env, ve_create_one_shot(env, 10, -1));
        break;
    default:
        break;
    }
}

void wc3_haptics_android_attach_thread(void)
{
    if (g_env || !g_vm)
        return;

    JNIEnv *env = NULL;
    jint rc = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    if (rc == JNI_OK) {
        g_env = env;
        return;
    }
    if (rc != JNI_EDETACHED)
        return;
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK)
        return;
    g_env = env;
    g_thread_attached = 1;
}

void wc3_haptics_android_detach_thread(void)
{
    if (g_thread_attached && g_vm)
        (*g_vm)->DetachCurrentThread(g_vm);
    g_env = NULL;
    g_thread_attached = 0;
}

void wc3_haptics_android_perform(int effect)
{
    if (!g_vibrator || effect <= 0 || !g_has_vibrator)
        return;

    if (!g_env)
        wc3_haptics_android_attach_thread();
    if (!g_env)
        return;

    LOGD("effect=%d", effect);
    perform_locked(g_env, effect);
}

int wc3_haptics_android_init(JNIEnv *env, JavaVM *vm, jobject activity)
{
    if (!env || !vm || !activity)
        return -1;

    g_vm = vm;
    delete_global_refs(env);
    g_has_vibrator = JNI_FALSE;

    jclass build_version = (*env)->FindClass(env, "android/os/Build$VERSION");
    if (build_version) {
        jfieldID sdk_field = (*env)->GetStaticFieldID(
            env, build_version, "SDK_INT", "I");
        if (sdk_field)
            g_sdk_int = (*env)->GetStaticIntField(env, build_version,
                                                  sdk_field);
        clear_exception(env);
        (*env)->DeleteLocalRef(env, build_version);
    } else {
        clear_exception(env);
        g_sdk_int = 26;
    }

    jclass activity_cls = (*env)->GetObjectClass(env, activity);
    jmethodID get_system_service = activity_cls
        ? (*env)->GetMethodID(env, activity_cls, "getSystemService",
                              "(Ljava/lang/String;)Ljava/lang/Object;")
        : NULL;
    clear_exception(env);

    jstring service = (*env)->NewStringUTF(env, "vibrator");
    jobject vibrator_local = NULL;
    if (get_system_service && service) {
        vibrator_local = (*env)->CallObjectMethod(
            env, activity, get_system_service, service);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            vibrator_local = NULL;
        }
    }
    if (service)
        (*env)->DeleteLocalRef(env, service);
    if (activity_cls)
        (*env)->DeleteLocalRef(env, activity_cls);

    if (vibrator_local) {
        g_vibrator = (*env)->NewGlobalRef(env, vibrator_local);
        (*env)->DeleteLocalRef(env, vibrator_local);
    }
    if (!g_vibrator) {
        LOGE("Vibrator service unavailable");
        return -1;
    }

    jclass ve_local = (*env)->FindClass(env, "android/os/VibrationEffect");
    if (!ve_local) {
        clear_exception(env);
        LOGE("VibrationEffect class unavailable");
        return -1;
    }
    g_ve_cls = (jclass)(*env)->NewGlobalRef(env, ve_local);
    (*env)->DeleteLocalRef(env, ve_local);
    if (!g_ve_cls)
        return -1;

    g_ve_create_one_shot = (*env)->GetStaticMethodID(
        env, g_ve_cls, "createOneShot",
        "(JI)Landroid/os/VibrationEffect;");
    clear_exception(env);

    if (g_sdk_int >= 29) {
        g_ve_create_predefined = (*env)->GetStaticMethodID(
            env, g_ve_cls, "createPredefined",
            "(I)Landroid/os/VibrationEffect;");
        clear_exception(env);
    }

    if (g_sdk_int >= 33) {
        g_ve_start_composition = (*env)->GetStaticMethodID(
            env, g_ve_cls, "startComposition",
            "()Landroid/os/VibrationEffect$Composition;");
        clear_exception(env);

        jclass comp_local = (*env)->FindClass(
            env, "android/os/VibrationEffect$Composition");
        if (comp_local) {
            g_comp_cls = (jclass)(*env)->NewGlobalRef(env, comp_local);
            (*env)->DeleteLocalRef(env, comp_local);
            if (g_comp_cls) {
                g_comp_add_primitive = (*env)->GetMethodID(
                    env, g_comp_cls, "addPrimitive",
                    "(IFI)Landroid/os/VibrationEffect$Composition;");
                clear_exception(env);
                g_comp_compose = (*env)->GetMethodID(
                    env, g_comp_cls, "compose",
                    "()Landroid/os/VibrationEffect;");
                clear_exception(env);
            }
        } else {
            clear_exception(env);
        }
    }

    jclass vibrator_cls = (*env)->GetObjectClass(env, g_vibrator);
    if (!vibrator_cls) {
        clear_exception(env);
        return -1;
    }

    g_vibrator_has_vibrator = (*env)->GetMethodID(
        env, vibrator_cls, "hasVibrator", "()Z");
    clear_exception(env);
    if (g_vibrator_has_vibrator) {
        g_has_vibrator = (*env)->CallBooleanMethod(
            env, g_vibrator, g_vibrator_has_vibrator);
        clear_exception(env);
    }

    g_vibrator_vibrate_ve = (*env)->GetMethodID(
        env, vibrator_cls, "vibrate", "(Landroid/os/VibrationEffect;)V");
    clear_exception(env);

    if (g_sdk_int >= 33) {
        /* The source uses this lookup; keep the same signature so its
         * predefined-tick fallback remains the observable behavior. */
        g_vibrator_all_primitives = (*env)->GetMethodID(
            env, vibrator_cls, "areAllPrimitivesSupported", "(I)Z");
        clear_exception(env);
    }

    (*env)->DeleteLocalRef(env, vibrator_cls);
    LOGI("initialized sdk=%d hasVibrator=%d", g_sdk_int,
         g_has_vibrator ? 1 : 0);
    return 0;
}
