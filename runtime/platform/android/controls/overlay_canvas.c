#include "overlay_canvas.h"

#include <android/bitmap.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>

#include "wc3_debug.h"

#if WC3_DEBUG && WC3_DEBUG_INPUT
#define OVERLAY_CANVAS_LOGI(...) \
    __android_log_print(ANDROID_LOG_INFO, "wc3overlay", __VA_ARGS__)
#define OVERLAY_CANVAS_LOGE(...) \
    __android_log_print(ANDROID_LOG_ERROR, "wc3overlay", __VA_ARGS__)
#else
#define OVERLAY_CANVAS_LOGI(...) ((void)0)
#define OVERLAY_CANVAS_LOGE(...) ((void)0)
#endif

#if !defined(OVERLAY_CANVAS_CLASS)
/* ClassLoader.loadClass() takes a binary name with dots, not a JNI path. */
#define OVERLAY_CANVAS_CLASS "wc3.recomp.overlaycanvas.OverlayCanvasCache"
#endif

#define OVERLAY_CANVAS_METHOD "buildAtlas"
#define OVERLAY_CANVAS_SIGNATURE \
    "(Landroid/content/Context;IIIFI[I[F[Ljava/lang/String;[I[I)" \
    "Landroid/graphics/Bitmap;"

static JavaVM *g_vm;
static jobject g_activity;
static jclass g_helper;
static jmethodID g_build;

static void clear_exception(JNIEnv *env, const char *where) {
    if (!(*env)->ExceptionCheck(env)) return;
#if WC3_DEBUG && WC3_DEBUG_INPUT
    OVERLAY_CANVAS_LOGE("JNI exception in %s", where);
    (*env)->ExceptionDescribe(env);
#else
    (void)where;
#endif
    (*env)->ExceptionClear(env);
}

static void release_globals(JNIEnv *env) {
    if (g_helper) {
        (*env)->DeleteGlobalRef(env, g_helper);
        g_helper = NULL;
    }
    if (g_activity) {
        (*env)->DeleteGlobalRef(env, g_activity);
        g_activity = NULL;
    }
    g_build = NULL;
    g_vm = NULL;
}

int overlay_canvas_attach(JNIEnv *env, jobject activity) {
    if (!env || !activity) return -1;
    if (g_activity && g_helper && g_build) return 0;
    if (g_activity || g_helper) release_globals(env);

    JavaVM *vm = NULL;
    if ((*env)->GetJavaVM(env, &vm) != JNI_OK || !vm) return -1;

    jobject activity_ref = (*env)->NewGlobalRef(env, activity);
    if (!activity_ref) {
        clear_exception(env, "NewGlobalRef(activity)");
        return -1;
    }

    int ok = 0;
    jclass loader_class = NULL;
    jclass activity_class = NULL;
    jobject loader = NULL;
    jstring helper_name = NULL;
    jclass helper = NULL;
    jclass helper_ref = NULL;
    jmethodID build = NULL;

    loader_class = (*env)->FindClass(env, "java/lang/ClassLoader");
    if (!loader_class) {
        clear_exception(env, "FindClass(ClassLoader)");
        goto done;
    }
    jmethodID load_class = (*env)->GetMethodID(
        env, loader_class, "loadClass",
        "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!load_class) {
        clear_exception(env, "GetMethodID(loadClass)");
        goto done;
    }

    activity_class = (*env)->GetObjectClass(env, activity);
    if (!activity_class) {
        clear_exception(env, "GetObjectClass(activity)");
        goto done;
    }
    jmethodID get_class_loader = (*env)->GetMethodID(
        env, activity_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
    if (!get_class_loader) {
        clear_exception(env, "GetMethodID(getClassLoader)");
        goto done;
    }
    loader = (*env)->CallObjectMethod(env, activity, get_class_loader);
    if ((*env)->ExceptionCheck(env) || !loader) {
        clear_exception(env, "getClassLoader");
        goto done;
    }

    helper_name = (*env)->NewStringUTF(env, OVERLAY_CANVAS_CLASS);
    if (!helper_name) {
        clear_exception(env, "NewStringUTF(helper)");
        goto done;
    }
    helper = (jclass)(*env)->CallObjectMethod(env, loader, load_class,
                                               helper_name);
    if ((*env)->ExceptionCheck(env) || !helper) {
        clear_exception(env, "loadClass(helper)");
        helper = NULL;
        goto done;
    }

    build = (*env)->GetStaticMethodID(env, helper, OVERLAY_CANVAS_METHOD,
                                      OVERLAY_CANVAS_SIGNATURE);
    if (!build) {
        clear_exception(env, "GetStaticMethodID(buildAtlas)");
        goto done;
    }
    helper_ref = (jclass)(*env)->NewGlobalRef(env, helper);
    if (!helper_ref) {
        clear_exception(env, "NewGlobalRef(helper)");
        goto done;
    }

    g_vm = vm;
    g_activity = activity_ref;
    g_helper = helper_ref;
    g_build = build;
    activity_ref = NULL;
    helper_ref = NULL;
    ok = 1;

done:
    if (helper) (*env)->DeleteLocalRef(env, helper);
    if (helper_name) (*env)->DeleteLocalRef(env, helper_name);
    if (loader) (*env)->DeleteLocalRef(env, loader);
    if (activity_class) (*env)->DeleteLocalRef(env, activity_class);
    if (loader_class) (*env)->DeleteLocalRef(env, loader_class);
    if (activity_ref) (*env)->DeleteGlobalRef(env, activity_ref);
    if (helper_ref) (*env)->DeleteGlobalRef(env, helper_ref);
    if (!ok) release_globals(env);
    return ok ? 0 : -1;
}

void overlay_canvas_detach(JNIEnv *env) {
    if (!env) return;
    release_globals(env);
}

static float effective_radius(const Wc3Overlay *overlay,
                              const Wc3OverlayElement *element) {
    float r = element->corner_radius;
    if (r < 0.0f) r = overlay->style.corner_radius;
    return r;
}

static int unpremultiply_atlas(JNIEnv *env, jobject bitmap,
                               uint8_t **out_pixels, int *out_width,
                               int *out_height) {
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) !=
        ANDROID_BITMAP_RESULT_SUCCESS) {
        OVERLAY_CANVAS_LOGE("AndroidBitmap_getInfo failed");
        return -1;
    }
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 ||
        info.width == 0 || info.height == 0) {
        OVERLAY_CANVAS_LOGE("unexpected atlas format=%d %ux%u",
                            (int)info.format, info.width, info.height);
        return -1;
    }
    if (info.stride < info.width * 4u) {
        OVERLAY_CANVAS_LOGE("atlas stride %u is smaller than width %u",
                            info.stride, info.width);
        return -1;
    }

    void *pixels = NULL;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) !=
            ANDROID_BITMAP_RESULT_SUCCESS ||
        !pixels) {
        OVERLAY_CANVAS_LOGE("AndroidBitmap_lockPixels failed");
        return -1;
    }

    size_t row_bytes = (size_t)info.width * 4u;
    size_t count = row_bytes * (size_t)info.height;
    uint8_t *rgba = (uint8_t *)malloc(count);
    if (!rgba) {
        AndroidBitmap_unlockPixels(env, bitmap);
        return -1;
    }

    const uint8_t *src_base = (const uint8_t *)pixels;
    for (uint32_t y = 0; y < info.height; y++) {
        const uint8_t *src = src_base + (size_t)y * (size_t)info.stride;
        uint8_t *dst = rgba + (size_t)y * row_bytes;
        for (uint32_t x = 0; x < info.width; x++) {
            uint8_t a = src[x * 4u + 3u];
            uint8_t *p = dst + (size_t)x * 4u;
            if (a == 0) {
                p[0] = p[1] = p[2] = p[3] = 0;
                continue;
            }
            for (int c = 0; c < 3; c++) {
                unsigned v = ((unsigned)src[x * 4u + (unsigned)c] * 255u +
                              (unsigned)a / 2u) /
                             (unsigned)a;
                p[c] = (uint8_t)(v > 255u ? 255u : v);
            }
            p[3] = a;
        }
    }

    AndroidBitmap_unlockPixels(env, bitmap);
    *out_pixels = rgba;
    *out_width = (int)info.width;
    *out_height = (int)info.height;
    return 0;
}

int overlay_canvas_build(Wc3Overlay *overlay) {
    if (!overlay) return -1;
    if (!g_vm || !g_helper || !g_build || !g_activity) {
        OVERLAY_CANVAS_LOGE("Canvas cache provider is not attached");
        return -1;
    }

    JNIEnv *env = NULL;
    jint status = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    int attached = 0;
    if (status == JNI_EDETACHED) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) {
            OVERLAY_CANVAS_LOGE("AttachCurrentThread failed");
            return -1;
        }
        attached = 1;
    } else if (status != JNI_OK || !env) {
        OVERLAY_CANVAS_LOGE("GetEnv failed: %d", (int)status);
        return -1;
    }

    int result = -1;
    int n = 0;
    int *meta = NULL;
    float *params = NULL;
    int *layers = NULL;
    int *icon_ids = NULL;
    uint8_t *atlas_pixels = NULL;
    jintArray meta_array = NULL;
    jfloatArray params_array = NULL;
    jobjectArray label_array = NULL;
    jintArray icon_array = NULL;
    jintArray layer_array = NULL;
    jstring *label_refs = NULL;
    jobject bitmap = NULL;

    n = overlay->element_count;
    if (n <= 0) goto done;

    int max_layers = n * WC3_OVERLAY_CACHE_SLOT_COUNT;
    meta = (int *)calloc((size_t)n * 6u, sizeof(int));
    params = (float *)calloc((size_t)n * 2u, sizeof(float));
    layers = (int *)calloc((size_t)max_layers * 9u + 1u, sizeof(int));
    icon_ids = (int *)calloc((size_t)n, sizeof(int));
    label_refs = (jstring *)calloc((size_t)n, sizeof(jstring));
    if (!meta || !params || !layers || !icon_ids || !label_refs) {
        OVERLAY_CANVAS_LOGE("out of memory preparing cache request");
        goto done;
    }

    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    if (!string_class) {
        clear_exception(env, "FindClass(String)");
        goto done;
    }
    meta_array = (*env)->NewIntArray(env, n * 6);
    params_array = (*env)->NewFloatArray(env, n * 2);
    label_array = (*env)->NewObjectArray(env, n, string_class, NULL);
    icon_array = (*env)->NewIntArray(env, n);
    layer_array = (*env)->NewIntArray(env, max_layers * 9 + 1);
    (*env)->DeleteLocalRef(env, string_class);
    if (!meta_array || !params_array || !label_array || !icon_array ||
        !layer_array) {
        clear_exception(env, "allocating JNI arrays");
        goto done;
    }

    for (int i = 0; i < n; i++) {
        const Wc3OverlayElement *e = &overlay->elements[i];
        int half_w = (int)e->hw;
        int half_h = (int)e->hh;
        if (half_w < 0) half_w = 0;
        if (half_h < 0) half_h = 0;
        meta[i * 6 + 0] = e->type == WC3_OVERLAY_ELEMENT_DPAD ? 1 : 0;
        meta[i * 6 + 1] = e->shape == WC3_OVERLAY_SHAPE_CIRCLE ? 1 : 0;
        meta[i * 6 + 2] = e->x - half_w;
        meta[i * 6 + 3] = e->y - half_h;
        meta[i * 6 + 4] = e->x + half_w;
        meta[i * 6 + 5] = e->y + half_h;
        params[i * 2 + 0] = e->scale;
        params[i * 2 + 1] = effective_radius(overlay, e);
        icon_ids[i] = e->icon_id;

        label_refs[i] = (*env)->NewStringUTF(
            env, e->type == WC3_OVERLAY_ELEMENT_BUTTON ? e->text : "");
        if (!label_refs[i]) {
            clear_exception(env, "NewStringUTF(label)");
            goto done;
        }
        (*env)->SetObjectArrayElement(env, label_array, i, label_refs[i]);
        (*env)->DeleteLocalRef(env, label_refs[i]);
        label_refs[i] = NULL;
    }

    (*env)->SetIntArrayRegion(env, meta_array, 0, n * 6, meta);
    (*env)->SetFloatArrayRegion(env, params_array, 0, n * 2, params);
    (*env)->SetIntArrayRegion(env, icon_array, 0, n, icon_ids);
    if ((*env)->ExceptionCheck(env)) {
        clear_exception(env, "setting cache arrays");
        goto done;
    }

    bitmap = (*env)->CallStaticObjectMethod(
        env, g_helper, g_build, g_activity,
        (jint)overlay->width, (jint)overlay->height,
        (jint)overlay->snapping_size,
        (jfloat)overlay->style.stroke_width_multiplier,
        (jint)n, meta_array, params_array, label_array, icon_array,
        layer_array);
    if ((*env)->ExceptionCheck(env)) {
        clear_exception(env, "buildAtlas");
        bitmap = NULL;
        goto done;
    }
    if (!bitmap) {
        OVERLAY_CANVAS_LOGE("Canvas buildAtlas returned null");
        goto done;
    }

    int atlas_w = 0;
    int atlas_h = 0;
    if (unpremultiply_atlas(env, bitmap, &atlas_pixels, &atlas_w,
                            &atlas_h) != 0)
        goto done;

    (*env)->GetIntArrayRegion(env, layer_array, 0, max_layers * 9 + 1,
                              layers);
    if ((*env)->ExceptionCheck(env)) {
        clear_exception(env, "reading layer metadata");
        goto done;
    }
    int layer_count = layers[0];
    if (layer_count < 0 || layer_count > max_layers) {
        OVERLAY_CANVAS_LOGE("invalid layer count %d (max %d)", layer_count,
                            max_layers);
        goto done;
    }

    if (overlay_cache_set_atlas(overlay, atlas_pixels, atlas_w, atlas_h,
                                atlas_w * 4, n) != 0) {
        OVERLAY_CANVAS_LOGE("cache atlas rejected: %dx%d", atlas_w, atlas_h);
        goto done;
    }
    atlas_pixels = NULL;

    for (int i = 0; i < layer_count; i++) {
        const int *l = layers + 1 + i * 9;
        if (overlay_cache_add_layer(overlay, l[0], l[1], l[2], l[3], l[4],
                                    l[5], l[6], l[7], l[8]) != 0) {
            OVERLAY_CANVAS_LOGE("cache layer %d rejected: el=%d slot=%d %dx%d",
                                i, l[0], l[1], l[4], l[5]);
            overlay_cache_clear(overlay);
            goto done;
        }
    }

    OVERLAY_CANVAS_LOGI("Canvas atlas %dx%d, %d layers, %d bytes", atlas_w,
                        atlas_h, layer_count, atlas_w * atlas_h * 4);
    result = 0;

done:
    free(atlas_pixels);
    free(meta);
    free(params);
    free(layers);
    free(icon_ids);
    if (icon_array) (*env)->DeleteLocalRef(env, icon_array);
    if (label_refs) {
        for (int i = 0; i < n; i++) {
            if (label_refs[i]) (*env)->DeleteLocalRef(env, label_refs[i]);
        }
        free(label_refs);
    }
    if (bitmap) {
        jclass bitmap_class = (*env)->GetObjectClass(env, bitmap);
        if (bitmap_class) {
            jmethodID recycle =
                (*env)->GetMethodID(env, bitmap_class, "recycle", "()V");
            if (recycle) (*env)->CallVoidMethod(env, bitmap, recycle);
            clear_exception(env, "recycle(bitmap)");
            (*env)->DeleteLocalRef(env, bitmap_class);
        }
        (*env)->DeleteLocalRef(env, bitmap);
    }
    if (meta_array) (*env)->DeleteLocalRef(env, meta_array);
    if (params_array) (*env)->DeleteLocalRef(env, params_array);
    if (label_array) (*env)->DeleteLocalRef(env, label_array);
    if (layer_array) (*env)->DeleteLocalRef(env, layer_array);
    if (attached) (*g_vm)->DetachCurrentThread(g_vm);
    return result;
}
