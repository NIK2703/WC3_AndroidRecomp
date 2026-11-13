/*
 * host_shell.c - GameActivity Android host shell for libwc3gameactivity.so.
 *
 * This is the app-side lifecycle and startup path, not guest code.  It owns the
 * platform table, maps the fixed guest address space, and loads the licensed PE
 * images when they are present in the app-private directory.  HLE modules and
 * the dispatcher are deliberately not linked yet: the shell reports the exact
 * missing stage instead of pretending to run the game.
 */
#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <errno.h>
#include <game-activity/GameActivity.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app_boot.h"
#include "controls.h"
#include "guest_layout.h"
#include "vulkan_driver_android.h"
#include "guest_memory.h"
#include "haptics_android.h"
#include "input_adapter.h"
#include "input_ring.h"
#include "input_wake.h"
#include "hle_bridge.h"
#include "overlay.h"
#include "overlay_android.h"
#include "overlay_canvas.h"
#include "pe.h"
#include "platform.h"
#include "platform_android.h"
#include "wc3_debug.h"
#include "wc3_trace.h"
#include "window_settle.h"

#if WC3_WITH_ASBUILT_HLE
#include "overlay_actions_legacy.h"
#endif

#ifndef WC3_DEBUG
#define WC3_DEBUG 0
#endif

#ifndef WC3_WITH_ASBUILT_HLE
#define WC3_WITH_ASBUILT_HLE 0
#endif

#if WC3_DEBUG
#define TAG "wc3host"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#endif

#if WC3_DEBUG && WC3_DEBUG_INPUT
#define IME_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "wc3ime", __VA_ARGS__)
#define IME_LOGE(...) \
    __android_log_print(ANDROID_LOG_ERROR, "wc3ime", __VA_ARGS__)
#else
#define IME_LOG(...) ((void)0)
#define IME_LOGE(...) ((void)0)
#endif

#if WC3_DEBUG && WC3_DEBUG_GPU_DRIVERS
#define GPU_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "GPU_DRIVERS", __VA_ARGS__)
#else
#define GPU_LOG(...) ((void)0)
#endif

static Wc3Platform g_platform;
static int g_platform_ready;
static int g_guest_ready;
static int g_config_ready;
static int g_asset_root_ready;
static int g_war3_mapped;
static int g_game_mapped;
static int g_asset_missing_count;
static char g_asset_missing[256];

#define WINDOW_SETTLE_MS 300u

static pthread_mutex_t g_window_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *g_window;
static uint32_t g_surface_w;
static uint32_t g_surface_h;
static uint64_t g_window_since_ms;
static int g_boot_started;
static pthread_t g_boot_thread;

/*
 * Touch-control overlay host state.
 *
 * The Android input callback only enqueues plain touch records.  A dedicated
 * input thread owns the Winlator state machine, and a separate render thread
 * owns the overlay ANativeWindow.  No engine lock or SurfaceView lock is held
 * while the guest runs, and the guest sees input only through the action
 * bridge.
 */
#define OVERLAY_SNAPSHOTS 3
#define OVERLAY_CACHE_SLOTS 3

typedef struct {
    Wc3OverlayCache cache;
    int refs;
} OverlayCacheSlot;

static struct android_app *g_app;
static Wc3InputRing g_touch_ring;
static Wc3InputWake *g_input_wake;
static Wc3InputAdapter g_input_adapter;
static pthread_t g_input_thread;
static pthread_t g_overlay_thread;
static int g_input_thread_started;
static int g_overlay_thread_started;
static _Atomic int g_input_stop;
static _Atomic int g_overlay_stop;
static _Atomic int g_input_ready;
static int g_input_width;
static int g_input_height;

static Wc3Overlay g_visual[OVERLAY_SNAPSHOTS];
static _Atomic int g_visual_active;
static _Atomic int g_visual_reading;
static _Atomic unsigned g_visual_gen;
static _Atomic int g_visual_ready;

#if WC3_DEBUG && WC3_DEBUG_INPUT
/* Phase 0 overlay-load counters.  They live only in the debug variant and are
 * touched by one thread each: publish counters by the input thread, present
 * counters by the overlay thread. */
static unsigned g_dbg_publish_calls;
static unsigned g_dbg_publish_changed;
static unsigned g_dbg_publish_equal;
static unsigned g_dbg_publish_errors;
static unsigned g_dbg_publish_skipped;
static unsigned g_dbg_presents;
static uint64_t g_dbg_present_ns_total;
static uint64_t g_dbg_present_ns_max;
#endif

static pthread_mutex_t g_overlay_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_overlay_cond;
static pthread_once_t g_overlay_cond_once = PTHREAD_ONCE_INIT;

/* The overlay renderer waits for a generation/stop signal.  It does not need
 * a periodic timeout: every producer of g_visual_gen, every surface change
 * and overlay_stop() broadcast this condition.  Waiting indefinitely removes
 * the old 10 Hz idle wakeup while keeping the same wake paths. */
static void overlay_cond_init(void)
{
    pthread_cond_init(&g_overlay_cond, NULL);
}

static void overlay_cond_ensure(void)
{
    (void)pthread_once(&g_overlay_cond_once, overlay_cond_init);
}

static ANativeWindow *g_overlay_window;
static int g_overlay_width;
static int g_overlay_height;

/*
 * The Canvas atlas is built once on a worker, not on the render thread.  A
 * published slot is reference-counted because the render thread copies a
 * visual snapshot before it presents; the slot must outlive that copy even if
 * a resize/teardown retires it.
 *
 * Lock order is g_overlay_lock -> g_cache_lock.  The cache worker never takes
 * g_overlay_lock while holding g_cache_lock.
 */
static OverlayCacheSlot g_cache_slots[OVERLAY_CACHE_SLOTS];
static int g_cache_published = -1;
static pthread_mutex_t g_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cache_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_cache_thread;
static int g_cache_thread_started;
static int g_cache_stop;
static int g_cache_requested;
static int g_cache_attached;

/*
 * Text-field/IME state.  The guest owns the focused field; this side only
 * mirrors the editor text and converts UTF-16 changes into WM_CHAR and
 * WM_KEYDOWN messages through the legacy bridge.
 */
static pthread_mutex_t g_ime_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_ime_text;
static int g_ime_text_len;
static int g_ime_text_cap;
static int g_ime_active;
/* The guest's CEditBox object is stable for the lifetime of a text field.
 * Keep the mirrored text per field so switching Login -> Password -> Login
 * restores the exact tail that the guest field still owns.  Without this the
 * diff would either delete the wrong characters or lose Backspace after a
 * field switch. */
#define IME_FIELD_SLOTS 8
typedef struct {
    uint32_t id;
    uint32_t used;
    char *text;
    int length;
} ImeFieldText;
static ImeFieldText g_ime_fields[IME_FIELD_SLOTS];
static uint32_t g_ime_field_id;
static uint32_t g_ime_field_clock;
/* The IME inset and focused-field rectangle decide whether the game surface
 * has to move.  g_ime_pan_px is the translation applied by Java; input adds it
 * back so a screen tap still lands on the same guest point. */
static int g_ime_rect_valid;
static Wc3LegacyKeyboardRect g_ime_rect;
static int g_ime_inset_bottom;
static float g_ime_pan_px;

static int g_haptics_ready;

static void overlay_input_stop(void);
static void overlay_stop(void);

#if WC3_DEBUG && WC3_DEBUG_INPUT
#define OVERLAY_LOGI(...) \
    __android_log_print(ANDROID_LOG_INFO, "wc3overlay", __VA_ARGS__)
#define OVERLAY_LOGE(...) \
    __android_log_print(ANDROID_LOG_ERROR, "wc3overlay", __VA_ARGS__)
#else
#define OVERLAY_LOGI(...) ((void)0)
#define OVERLAY_LOGE(...) ((void)0)
#endif

/* Implemented by the DXVK Win32 shim.  The set call publishes a replacement
 * ANativeWindow so DXVK can rebuild its Vulkan surface; the clear call drops
 * the exact window Android is about to destroy. */
void wc3_android_set_window(ANativeWindow *window, uint32_t width,
                            uint32_t height);
void wc3_android_clear_window(ANativeWindow *window);
int wc3_android_set_surface_size(uint32_t width, uint32_t height);

/*
 * User-visible diagnostic text.  This is not a debug log: the product is not
 * allowed to look like a running game while the corpus, HLE and dispatcher are
 * still absent.  Failures to reach the activity are non-fatal; the native
 * startup path continues so the developer still gets logcat diagnostics.
 */
static void set_activity_text(struct android_app *app, const char *text)
{
    if (!app || !app->activity || !app->activity->vm ||
        !app->activity->javaGameActivity || !text)
        return;

    JavaVM *vm = app->activity->vm;
    JNIEnv *env = NULL;
    int attached = 0;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return;
        attached = 1;
    }

    jclass cls = (*env)->GetObjectClass(env, app->activity->javaGameActivity);
    if (cls) {
        jmethodID method = (*env)->GetStaticMethodID(
            env, cls, "setDiagnostics", "(Ljava/lang/String;)V");
        if (method) {
            jstring value = (*env)->NewStringUTF(env, text);
            if (value) {
                (*env)->CallStaticVoidMethod(env, cls, method, value);
                (*env)->DeleteLocalRef(env, value);
            }
        }
        (*env)->DeleteLocalRef(env, cls);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (attached) (*vm)->DetachCurrentThread(vm);
}

static char g_data_dir[1024];
static char g_state_dir[1024];
/* Wc3RocActivity overrides the default before GameActivity starts android_main. */
static char g_game_mode[8] = "tft";
static char g_vulkan_driver_root[1024];
static char g_vulkan_driver_id[WC3_VULKAN_DRIVER_ID_MAX];
static char g_vulkan_driver_hook_dir[1024];
/* Settings default.  A direct diagnostic launch keeps the authored profile;
 * zero here means "do not override the .icp timer". */
static int g_double_tap_gestures = 1;
static int g_double_tap_timeout_ms;
static int g_long_press_timeout_ms;

#define SHARED_DATA_DIR_NAME "Warcraft 3"

/* Derive the storage root from the app's external files path:
 * /storage/emulated/0/Android/data/<package>/files ->
 * /storage/emulated/0/Warcraft 3.  Keeping the storage root out of the C
 * code makes the same APK work for a secondary user as well. */
static int derive_shared_data_dir(const char *external, char *out,
                                  size_t out_size)
{
    if (!external || !*external || !out || out_size == 0)
        return -1;
    const char *marker = strstr(external, "/Android/data/");
    if (!marker)
        return -1;
    size_t root_len = (size_t)(marker - external);
    if (root_len == 0)
        return -1;
    int n = snprintf(out, out_size, "%.*s/%s", (int)root_len, external,
                     SHARED_DATA_DIR_NAME);
    return (n > 0 && (size_t)n < out_size) ? 0 : -1;
}

static const char *pick_data_dir(struct android_app *app)
{
    if (!app || !app->activity)
        return NULL;

    char shared[1024];
    const char *candidates[4];
    int count = 0;

    /* The launch settings screen owns this override.  Check it before the
     * historical auto-detection so a user-selected licensed game directory
     * wins even when the default shared path also contains a partial copy. */
    const char *override = getenv("WC3_GAME_DATA_DIR");
    if (override && override[0])
        candidates[count++] = override;

    if (derive_shared_data_dir(app->activity->externalDataPath, shared,
                               sizeof shared) == 0)
        candidates[count++] = shared;
    if (app->activity->externalDataPath && app->activity->externalDataPath[0])
        candidates[count++] = app->activity->externalDataPath;
    if (app->activity->internalDataPath && app->activity->internalDataPath[0])
        candidates[count++] = app->activity->internalDataPath;

    for (int i = 0; i < count; i++) {
        const char *candidate = candidates[i];
        char probe[1200];

        if (access(candidate, R_OK | W_OK | X_OK) != 0) {
            LOGE("data candidate %s not accessible: %s", candidate,
                 strerror(errno));
            continue;
        }
        int n = snprintf(probe, sizeof probe, "%s/war3.exe", candidate);
        if (n <= 0 || (size_t)n >= sizeof probe) continue;
        if (access(probe, R_OK) != 0) {
            LOGE("no war3.exe in %s", candidate);
            continue;
        }
        n = snprintf(probe, sizeof probe, "%s/war3.mpq", candidate);
        if (n <= 0 || (size_t)n >= sizeof probe) continue;
        if (access(probe, R_OK) != 0) {
            LOGE("no war3.mpq in %s", candidate);
            continue;
        }
        snprintf(g_data_dir, sizeof g_data_dir, "%s", candidate);
        LOGI("game data %s", g_data_dir);
        return g_data_dir;
    }
    return NULL;
}

static uint64_t host_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

#if WC3_DEBUG && WC3_DEBUG_INPUT
static uint64_t host_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

static char *read_text_file(const char *path)
{
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0 || size > 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *text = (char *)malloc((size_t)size + 1u);
    if (!text) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(text, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(text);
        return NULL;
    }
    text[size] = '\0';
    if (size >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        memmove(text, text + 3, (size_t)size - 2u);
    }
    return text;
}

static char *read_asset_text(const char *name)
{
    if (!g_app || !g_app->activity || !g_app->activity->assetManager || !name)
        return NULL;

    AAsset *asset = AAssetManager_open(g_app->activity->assetManager, name,
                                       AASSET_MODE_BUFFER);
    if (!asset) return NULL;

    off_t size = AAsset_getLength(asset);
    if (size <= 0 || size > 1024 * 1024) {
        AAsset_close(asset);
        return NULL;
    }
    char *text = (char *)malloc((size_t)size + 1u);
    if (!text) {
        AAsset_close(asset);
        return NULL;
    }
    size_t got = 0;
    while (got < (size_t)size) {
        int n = AAsset_read(asset, text + got, (size_t)size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    AAsset_close(asset);
    if (got != (size_t)size) {
        free(text);
        return NULL;
    }
    text[size] = '\0';
    if (size >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        memmove(text, text + 3, (size_t)size - 2u);
    }
    return text;
}

static char *load_controls_profile(void)
{
    static const char *const primary = "Warcraft 3.icp";
    static const char *const legacy = "overlay.icp";
    char path[1200];

    /* The shared game directory may still contain the old overlay.icp from
     * the 1.26 host.  Prefer the current profile, then the packaged copy, and
     * only then fall back to the legacy name. */
    if (g_data_dir[0]) {
        int n = snprintf(path, sizeof path, "%s/%s", g_data_dir, primary);
        if (n > 0 && (size_t)n < sizeof path) {
            char *text = read_text_file(path);
            if (text) {
                OVERLAY_LOGI("controls profile %s", path);
                return text;
            }
        }
    }

    char *text = read_asset_text(primary);
    if (text) {
        OVERLAY_LOGI("controls profile assets/%s", primary);
        return text;
    }

    if (g_data_dir[0]) {
        int n = snprintf(path, sizeof path, "%s/%s", g_data_dir, legacy);
        if (n > 0 && (size_t)n < sizeof path) {
            text = read_text_file(path);
            if (text) {
                OVERLAY_LOGI("controls profile %s", path);
                return text;
            }
        }
    }

    text = read_asset_text(legacy);
    if (text) {
        OVERLAY_LOGI("controls profile assets/%s", legacy);
        return text;
    }
    return NULL;
}

static void visual_signal(void)
{
    overlay_cond_ensure();
    pthread_mutex_lock(&g_overlay_lock);
    pthread_cond_broadcast(&g_overlay_cond);
    pthread_mutex_unlock(&g_overlay_lock);
}

static void cache_slot_clear(OverlayCacheSlot *slot)
{
    if (!slot) return;
    free(slot->cache.pixels);
    memset(&slot->cache, 0, sizeof slot->cache);
}

static const Wc3OverlayCache *cache_acquire(int *slot_index)
{
    const Wc3OverlayCache *view = NULL;
    if (slot_index) *slot_index = -1;

    pthread_mutex_lock(&g_cache_lock);
    int index = g_cache_published;
    if (index >= 0 && index < OVERLAY_CACHE_SLOTS &&
        g_cache_slots[index].cache.pixels) {
        g_cache_slots[index].refs++;
        view = &g_cache_slots[index].cache;
        if (slot_index) *slot_index = index;
    }
    pthread_mutex_unlock(&g_cache_lock);
    return view;
}

static void cache_release(int slot_index)
{
    if (slot_index < 0 || slot_index >= OVERLAY_CACHE_SLOTS) return;

    pthread_mutex_lock(&g_cache_lock);
    OverlayCacheSlot *slot = &g_cache_slots[slot_index];
    if (slot->refs > 0) slot->refs--;
    if (slot->refs == 0 && slot_index != g_cache_published)
        cache_slot_clear(slot);
    pthread_mutex_unlock(&g_cache_lock);
}

static void cache_wake_render(void)
{
    /* Wake the render thread without nesting the overlay lock inside the
     * cache lock.  Lock order remains overlay -> cache. */
    overlay_cond_ensure();
    pthread_mutex_lock(&g_overlay_lock);
    atomic_fetch_add_explicit(&g_visual_gen, 1u, memory_order_release);
    pthread_cond_broadcast(&g_overlay_cond);
    pthread_mutex_unlock(&g_overlay_lock);
}

static void cache_publish(Wc3OverlayCache *built)
{
    int store = -1;

    pthread_mutex_lock(&g_cache_lock);
    for (int i = 0; i < OVERLAY_CACHE_SLOTS; i++) {
        if (i != g_cache_published && g_cache_slots[i].refs == 0) {
            store = i;
            break;
        }
    }
    if (store < 0 && g_cache_published >= 0 &&
        g_cache_slots[g_cache_published].refs == 0)
        store = g_cache_published;

    if (store >= 0) {
        cache_slot_clear(&g_cache_slots[store]);
        g_cache_slots[store].cache = *built;
        memset(built, 0, sizeof *built);
        g_cache_published = store;
    }
    pthread_mutex_unlock(&g_cache_lock);

    if (store < 0) {
        OVERLAY_LOGE("Canvas atlas dropped: all cache slots are in use");
        return;
    }
    cache_wake_render();
}

static void *cache_worker_main(void *arg)
{
    (void)arg;
    WC3_TRACE_THREAD_NAME("wc3-cache");

    for (;;) {
        pthread_mutex_lock(&g_cache_lock);
        while (!g_cache_stop && !g_cache_requested)
            pthread_cond_wait(&g_cache_cond, &g_cache_lock);
        if (g_cache_stop) {
            pthread_mutex_unlock(&g_cache_lock);
            break;
        }
        g_cache_requested = 0;
        pthread_mutex_unlock(&g_cache_lock);

        Wc3Overlay source;
        int have_source = 0;
        memset(&source, 0, sizeof source);

        pthread_mutex_lock(&g_overlay_lock);
        if (atomic_load_explicit(&g_visual_ready, memory_order_acquire)) {
            int active = atomic_load_explicit(&g_visual_active,
                                              memory_order_acquire);
            if (active >= 0 && active < OVERLAY_SNAPSHOTS) {
                source = g_visual[active];
                memset(&source.cache, 0, sizeof source.cache);
                source.cache_view = NULL;
                have_source = 1;
            }
        }
        pthread_mutex_unlock(&g_overlay_lock);

        if (!have_source) continue;

        {
            WC3_TRACE_SCOPE("wc3.overlay.cache");
            if (overlay_canvas_build(&source) == 0) {
                Wc3OverlayCache built = source.cache;
                memset(&source.cache, 0, sizeof source.cache);
                cache_publish(&built);
                free(built.pixels);
            }
        }
        overlay_destroy(&source);
    }
    return NULL;
}

static void cache_start(void)
{
    if (!g_cache_attached) {
        OVERLAY_LOGE("Canvas atlas provider is not attached");
        return;
    }

    pthread_mutex_lock(&g_cache_lock);
    if (g_cache_thread_started) {
        g_cache_requested = 1;
        pthread_cond_broadcast(&g_cache_cond);
        pthread_mutex_unlock(&g_cache_lock);
        return;
    }
    g_cache_stop = 0;
    g_cache_requested = 1;
    g_cache_thread_started = 1;
    pthread_mutex_unlock(&g_cache_lock);

    if (pthread_create(&g_cache_thread, NULL, cache_worker_main, NULL) != 0) {
        pthread_mutex_lock(&g_cache_lock);
        g_cache_thread_started = 0;
        g_cache_requested = 0;
        pthread_mutex_unlock(&g_cache_lock);
        OVERLAY_LOGE("cannot start the Canvas atlas worker");
        return;
    }
    OVERLAY_LOGI("Canvas atlas worker started");
}

static void cache_stop(void)
{
    pthread_mutex_lock(&g_cache_lock);
    int started = g_cache_thread_started;
    if (started) {
        g_cache_stop = 1;
        g_cache_requested = 0;
        pthread_cond_broadcast(&g_cache_cond);
    }
    pthread_mutex_unlock(&g_cache_lock);

    if (started) pthread_join(g_cache_thread, NULL);

    pthread_mutex_lock(&g_cache_lock);
    g_cache_thread_started = 0;
    g_cache_stop = 0;
    g_cache_requested = 0;
    g_cache_published = -1;
    for (int i = 0; i < OVERLAY_CACHE_SLOTS; i++) {
        if (g_cache_slots[i].refs == 0) cache_slot_clear(&g_cache_slots[i]);
    }
    pthread_mutex_unlock(&g_cache_lock);
}

static void visual_destroy(void)
{
    /* Stop the producer before the snapshots it reads are destroyed.  The
     * render thread may still hold a cache reference; cache_stop retires the
     * published slot and the last release frees it. */
    cache_stop();

    pthread_mutex_lock(&g_overlay_lock);
    atomic_store_explicit(&g_visual_ready, 0, memory_order_release);
    for (int i = 0; i < OVERLAY_SNAPSHOTS; i++) {
        overlay_destroy(&g_visual[i]);
        memset(&g_visual[i], 0, sizeof g_visual[i]);
    }
    atomic_store_explicit(&g_visual_reading, -1, memory_order_release);
    atomic_fetch_add_explicit(&g_visual_gen, 1u, memory_order_release);
    pthread_mutex_unlock(&g_overlay_lock);
    visual_signal();
}

static int visual_initialize(int width, int height)
{
    int failed = 0;
    pthread_mutex_lock(&g_overlay_lock);
    atomic_store_explicit(&g_visual_ready, 0, memory_order_release);
    for (int i = 0; i < OVERLAY_SNAPSHOTS; i++) {
        memset(&g_visual[i], 0, sizeof g_visual[i]);
        overlay_init(&g_visual[i], width, height, NULL);
    }
    for (int i = 0; i < OVERLAY_SNAPSHOTS; i++) {
        if (controls_populate_overlay(&g_visual[i]) != 0) {
            failed = 1;
            break;
        }
        g_visual[i].alpha_mode = WC3_OVERLAY_ALPHA_PREMULTIPLIED;
    }
    if (!failed) {
        atomic_store_explicit(&g_visual_active, 0, memory_order_release);
        atomic_store_explicit(&g_visual_reading, -1, memory_order_release);
        atomic_fetch_add_explicit(&g_visual_gen, 1u, memory_order_release);
        atomic_store_explicit(&g_visual_ready, 1, memory_order_release);
    }
    pthread_mutex_unlock(&g_overlay_lock);

    if (failed) {
        OVERLAY_LOGE("overlay populate failed: %s", controls_last_error());
        visual_destroy();
        return -1;
    }
    visual_signal();
    cache_start();
    OVERLAY_LOGI("overlay visual ready %dx%d, %d elements", width, height,
                 g_visual[0].element_count);
    return 0;
}

static void publish_visual(void)
{
    WC3_TRACE_SCOPE("wc3.overlay.publish");
    pthread_mutex_lock(&g_overlay_lock);
    if (!atomic_load_explicit(&g_visual_ready, memory_order_acquire)) {
        pthread_mutex_unlock(&g_overlay_lock);
        return;
    }
    int active = atomic_load_explicit(&g_visual_active, memory_order_acquire);
    int next = -1;
    for (int i = 0; i < OVERLAY_SNAPSHOTS; i++) {
        if (i != active) {
            next = i;
            break;
        }
    }
    int changed = 0;
    int sync_failed = 0;
    if (next >= 0) {
        int rc = controls_sync_visuals(&g_visual[next]);
        if (rc < 0) {
            sync_failed = 1;
        } else if (active < 0 || active >= OVERLAY_SNAPSHOTS ||
                   !overlay_visual_state_equal(&g_visual[active],
                                               &g_visual[next])) {
            /* Compare with the displayed snapshot, not with the staging
             * buffer just filled.  A release that arrives before the next
             * publish must still become visible, while a steady-state tick
             * must not present a duplicate frame. */
            atomic_store_explicit(&g_visual_active, next, memory_order_release);
            atomic_fetch_add_explicit(&g_visual_gen, 1u, memory_order_release);
            changed = 1;
        }
    }
    pthread_mutex_unlock(&g_overlay_lock);

#if WC3_DEBUG && WC3_DEBUG_INPUT
    g_dbg_publish_calls++;
    if (changed)
        g_dbg_publish_changed++;
    else if (sync_failed)
        g_dbg_publish_errors++;
    else
        g_dbg_publish_equal++;
    if ((g_dbg_publish_calls % 300u) == 0u) {
        OVERLAY_LOGI("publish calls=%u changed=%u equal=%u errors=%u",
                     g_dbg_publish_calls, g_dbg_publish_changed,
                     g_dbg_publish_equal, g_dbg_publish_errors);
    }
#endif

    if (changed > 0) visual_signal();
}

static void present_blank(ANativeWindow *window, int width, int height)
{
    Wc3Overlay blank;
    memset(&blank, 0, sizeof blank);
    overlay_init(&blank, width, height, NULL);
    overlay_android_present(window, &blank);
    overlay_destroy(&blank);
}

static void *overlay_thread_main(void *arg)
{
    (void)arg;
    unsigned seen = 0;
    int first = 1;
    WC3_TRACE_THREAD_NAME("wc3-overlay");

    overlay_cond_ensure();
    for (;;) {
        pthread_mutex_lock(&g_overlay_lock);
        while (!atomic_load_explicit(&g_overlay_stop, memory_order_acquire)) {
            unsigned gen = atomic_load_explicit(&g_visual_gen,
                                                memory_order_acquire);
            if (first || gen != seen) break;
            pthread_cond_wait(&g_overlay_cond, &g_overlay_lock);
        }
        if (atomic_load_explicit(&g_overlay_stop, memory_order_acquire)) {
            pthread_mutex_unlock(&g_overlay_lock);
            break;
        }
        seen = atomic_load_explicit(&g_visual_gen, memory_order_acquire);
        first = 0;
        WC3_TRACE_SCOPE("wc3.overlay.tick");
        ANativeWindow *window = g_overlay_window;
        int width = g_overlay_width;
        int height = g_overlay_height;
        Wc3Overlay local;
        const Wc3OverlayCache *cache_view = NULL;
        int cache_slot = -1;
        int have_snapshot = 0;
        if (seen && atomic_load_explicit(&g_visual_ready,
                                         memory_order_acquire) &&
            width > 0 && height > 0) {
            int active = atomic_load_explicit(&g_visual_active,
                                              memory_order_acquire);
            if (active >= 0 && active < OVERLAY_SNAPSHOTS) {
                local = g_visual[active];
                memset(&local.cache, 0, sizeof local.cache);
                /* Acquire while the overlay lock is held: visual_destroy
                 * stops the producer under this same ordering. */
                cache_view = cache_acquire(&cache_slot);
                local.cache_view = cache_view;
                have_snapshot = 1;
            }
        }
        if (window) ANativeWindow_acquire(window);
        pthread_mutex_unlock(&g_overlay_lock);

        if (!window) {
            cache_release(cache_slot);
            continue;
        }
#if WC3_DEBUG && WC3_DEBUG_INPUT
        uint64_t present_start_ns = host_now_ns();
#endif
        {
            WC3_TRACE_SCOPE("wc3.overlay.present");
            if (have_snapshot) {
                overlay_android_present(window, &local);
            } else {
                present_blank(window, width > 0 ? width : 1,
                              height > 0 ? height : 1);
            }
        }
#if WC3_DEBUG && WC3_DEBUG_INPUT
        uint64_t present_end_ns = host_now_ns();
        if (present_start_ns && present_end_ns &&
            present_end_ns >= present_start_ns) {
            uint64_t elapsed_ns = present_end_ns - present_start_ns;
            g_dbg_present_ns_total += elapsed_ns;
            if (elapsed_ns > g_dbg_present_ns_max)
                g_dbg_present_ns_max = elapsed_ns;
        }
        g_dbg_presents++;
        if ((g_dbg_presents % 60u) == 0u) {
            unsigned long long avg_us =
                (unsigned long long)(g_dbg_present_ns_total /
                                     (uint64_t)g_dbg_presents / 1000ull);
            OVERLAY_LOGI("overlay presents=%u avg_us=%llu max_us=%llu",
                         g_dbg_presents, avg_us,
                         (unsigned long long)(g_dbg_present_ns_max / 1000ull));
        }
#endif
        cache_release(cache_slot);
        ANativeWindow_release(window);
    }
    return NULL;
}

static void overlay_stop(void)
{
    pthread_mutex_lock(&g_overlay_lock);
    ANativeWindow *window = g_overlay_window;
    g_overlay_window = NULL;
    int started = g_overlay_thread_started;
    if (started) {
        atomic_store_explicit(&g_overlay_stop, 1, memory_order_release);
        overlay_cond_ensure();
        pthread_cond_broadcast(&g_overlay_cond);
    }
    pthread_mutex_unlock(&g_overlay_lock);

    if (started) pthread_join(g_overlay_thread, NULL);
    if (window) ANativeWindow_release(window);

    pthread_mutex_lock(&g_overlay_lock);
    g_overlay_thread_started = 0;
    g_overlay_width = 0;
    g_overlay_height = 0;
    pthread_mutex_unlock(&g_overlay_lock);
}

static void overlay_start(ANativeWindow *window, int width, int height)
{
    if (!window || width <= 0 || height <= 0) {
        if (window) ANativeWindow_release(window);
        return;
    }
    overlay_stop();
    overlay_android_configure(window, width, height);

    pthread_mutex_lock(&g_overlay_lock);
    g_overlay_window = window;
    g_overlay_width = width;
    g_overlay_height = height;
    atomic_store_explicit(&g_overlay_stop, 0, memory_order_release);
    g_overlay_thread_started = 1;
    pthread_mutex_unlock(&g_overlay_lock);

    if (pthread_create(&g_overlay_thread, NULL, overlay_thread_main, NULL) != 0) {
        pthread_mutex_lock(&g_overlay_lock);
        g_overlay_window = NULL;
        g_overlay_thread_started = 0;
        pthread_mutex_unlock(&g_overlay_lock);
        ANativeWindow_release(window);
        OVERLAY_LOGE("cannot start the overlay render thread");
    } else {
        visual_signal();
        OVERLAY_LOGI("overlay surface %dx%d started", width, height);
    }
}

static void JNICALL native_set_overlay_surface(JNIEnv *env, jobject thiz,
                                               jobject surface, jint width,
                                               jint height)
{
    (void)thiz;
    if (!surface) return;
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        OVERLAY_LOGE("ANativeWindow_fromSurface failed");
        return;
    }
    int w = width > 0 ? (int)width : ANativeWindow_getWidth(window);
    int h = height > 0 ? (int)height : ANativeWindow_getHeight(window);
    overlay_start(window, w, h);
}

static void JNICALL native_clear_overlay_surface(JNIEnv *env, jobject thiz,
                                                 jobject surface)
{
    (void)env;
    (void)thiz;
    (void)surface;
    overlay_stop();
}

/* Dynamic-lookup fallbacks: the overlay SurfaceView can be created before
 * android_main registers any JNI method. */
JNIEXPORT void JNICALL
Java_wc3_recomp_Wc3GameActivity_nativeSetOverlaySurface(
    JNIEnv *env, jobject thiz, jobject surface, jint width, jint height)
{
    native_set_overlay_surface(env, thiz, surface, width, height);
}

JNIEXPORT void JNICALL
Java_wc3_recomp_Wc3GameActivity_nativeClearOverlaySurface(
    JNIEnv *env, jobject thiz, jobject surface)
{
    native_clear_overlay_surface(env, thiz, surface);
}

JNIEXPORT jfloat JNICALL
Java_wc3_recomp_Wc3GameActivity_nativeImePan(JNIEnv *env, jobject thiz)
{
    (void)env;
    (void)thiz;
    pthread_mutex_lock(&g_ime_lock);
    const float pan = g_ime_pan_px;
    pthread_mutex_unlock(&g_ime_lock);
    return (jfloat)pan;
}

JNIEXPORT void JNICALL
Java_wc3_recomp_Wc3GameActivity_nativeSetGameMode(
    JNIEnv *env, jclass clazz, jstring mode)
{
    (void)clazz;
    if (!mode) return;
    const char *value = (*env)->GetStringUTFChars(env, mode, NULL);
    if (!value) return;
    if (!strcmp(value, "roc") || !strcmp(value, "tft"))
        snprintf(g_game_mode, sizeof g_game_mode, "%s", value);
    else
        LOGE("unknown game mode: %s", value);
    (*env)->ReleaseStringUTFChars(env, mode, value);
}

/* Whole-token match for a space-separated guest command line. */
static int has_arg_token(const char *args, const char *token)
{
    if (!args || !args[0] || !token || !token[0]) return 0;
    const size_t token_len = strlen(token);
    const char *p = args;
    while ((p = strstr(p, token)) != NULL) {
        const int left = (p == args) || p[-1] == ' ' || p[-1] == '\t';
        const int right = p[token_len] == '\0' || p[token_len] == ' ' ||
                          p[token_len] == '\t';
        if (left && right) return 1;
        p += token_len;
    }
    return 0;
}

/*
 * Called before GameActivity.onCreate starts android_main.  Only the variables
 * owned by the settings screen are written; a host launch without the launch
 * bundle never reaches this function, so ADB/diagnostic launches keep their
 * environment untouched.
 */
JNIEXPORT void JNICALL
Java_wc3_recomp_Wc3GameActivity_nativeApplyLaunchSettings(
    JNIEnv *env, jclass clazz, jstring game_dir, jint width, jint height,
    jboolean sound, jstring extra_args, jstring driver_id, jstring driver_root,
    jstring hook_dir, jstring dxvk_version, jstring dxvk_config_path,
    jboolean double_tap_gestures, jint double_tap_timeout_ms,
    jint long_press_timeout_ms)
{
    /* JNI has no type checking between two jstring parameters.  Keep this
     * order identical to Wc3GameActivity.nativeApplyLaunchSettings: id before
     * root, DXVK version and config path, then the gesture flag and timers
     * last, or values are copied into the wrong slot. */
    (void)clazz;
    g_double_tap_gestures = double_tap_gestures ? 1 : 0;
    g_double_tap_timeout_ms = (int)double_tap_timeout_ms;
    g_long_press_timeout_ms = (int)long_press_timeout_ms;

    const char *dir = NULL;
    const char *user_args = NULL;
    const char *vulkan_root = NULL;
    const char *vulkan_id = NULL;
    const char *vulkan_hooks = NULL;
    const char *dxvk = NULL;
    const char *dxvk_config = NULL;
    int dxvk_forced = 0;

    if (game_dir)
        dir = (*env)->GetStringUTFChars(env, game_dir, NULL);
    if (extra_args)
        user_args = (*env)->GetStringUTFChars(env, extra_args, NULL);
    if (driver_root)
        vulkan_root = (*env)->GetStringUTFChars(env, driver_root, NULL);
    if (driver_id)
        vulkan_id = (*env)->GetStringUTFChars(env, driver_id, NULL);
    if (hook_dir)
        vulkan_hooks = (*env)->GetStringUTFChars(env, hook_dir, NULL);
    if (dxvk_version)
        dxvk = (*env)->GetStringUTFChars(env, dxvk_version, NULL);
    if (dxvk_config_path)
        dxvk_config = (*env)->GetStringUTFChars(env, dxvk_config_path, NULL);

    GPU_LOG("apply launch driver id='%s' root='%s' hooks='%s' dxvk='%s' "
            "config='%s'",
            vulkan_id ? vulkan_id : "(null)",
            vulkan_root ? vulkan_root : "(null)",
            vulkan_hooks ? vulkan_hooks : "(null)",
            dxvk ? dxvk : "(null)",
            dxvk_config ? dxvk_config : "(null)");

    /* An explicit version is a compatibility override, not an automatic
     * choice: the as-built thin bridge treats "1"/"3" as forced and skips its
     * version selector.  The DXVK entry wrappers also suppress the
     * 3.1.1 -> 1.10.3 fallback while "3" is forced. */
    dxvk_forced = dxvk && (strcmp(dxvk, "1") == 0 || strcmp(dxvk, "3") == 0);
    if (dxvk_forced)
        setenv("WC3_DXVK_VERSION", dxvk, 1);
    else
        unsetenv("WC3_DXVK_VERSION");
    GPU_LOG("DXVK selection '%s'", dxvk_forced ? dxvk : "auto");

    /* Both vendored DXVK versions read DXVK_CONFIG_FILE.  DXVK 1.10.3 has no
     * DXVK_CONFIG environment form, so the app writes one key-value file and
     * points both majors at it.  Unset the 3.1.1-only environment override so
     * an inherited shell variable cannot silently win after the file parse. */
    if (dxvk_config && dxvk_config[0]) {
        setenv("DXVK_CONFIG_FILE", dxvk_config, 1);
        GPU_LOG("DXVK config file '%s'", dxvk_config);
    } else {
        unsetenv("DXVK_CONFIG_FILE");
        GPU_LOG("DXVK config file disabled");
    }
    unsetenv("DXVK_CONFIG");

    /* The package choice is latched before android_main installs the platform
     * table.  A missing/empty root keeps the system loader. */
    snprintf(g_vulkan_driver_root, sizeof g_vulkan_driver_root, "%s",
             vulkan_root ? vulkan_root : "");
    snprintf(g_vulkan_driver_id, sizeof g_vulkan_driver_id, "%s",
             vulkan_id ? vulkan_id : "");
    snprintf(g_vulkan_driver_hook_dir, sizeof g_vulkan_driver_hook_dir, "%s",
             vulkan_hooks ? vulkan_hooks : "");

    if (dir && dir[0])
        setenv("WC3_GAME_DATA_DIR", dir, 1);
    else
        unsetenv("WC3_GAME_DATA_DIR");

    if (width > 0 && height > 0) {
        char value[16];
        snprintf(value, sizeof value, "%d", (int)width);
        setenv("WC3_DXVK_WIDTH", value, 1);
        snprintf(value, sizeof value, "%d", (int)height);
        setenv("WC3_DXVK_HEIGHT", value, 1);
    } else {
        unsetenv("WC3_DXVK_WIDTH");
        unsetenv("WC3_DXVK_HEIGHT");
    }

    char args[512];
    args[0] = '\0';
    if (user_args && user_args[0])
        snprintf(args, sizeof args, "%s", user_args);
    if (!sound && !has_arg_token(args, "-nosound")) {
        static const char token[] = "-nosound";
        size_t len = strlen(args);
        const size_t token_len = sizeof token - 1u;
        if (len > 0 && len + 1u + token_len < sizeof args) {
            args[len++] = ' ';
            args[len] = '\0';
        }
        if (len + token_len < sizeof args)
            snprintf(args + len, sizeof args - len, "%s", token);
    }
    if (args[0])
        setenv("WC3_ARGS", args, 1);
    else
        unsetenv("WC3_ARGS");

    if (wc3_android_vulkan_driver_configure(g_vulkan_driver_root,
                                            g_vulkan_driver_id,
                                            g_vulkan_driver_hook_dir) != 0)
        LOGE("Vulkan driver settings rejected (id=%s)",
             g_vulkan_driver_id);
    else
        GPU_LOG("driver settings configured");

    if (dir)
        (*env)->ReleaseStringUTFChars(env, game_dir, dir);
    if (user_args)
        (*env)->ReleaseStringUTFChars(env, extra_args, user_args);
    if (vulkan_root)
        (*env)->ReleaseStringUTFChars(env, driver_root, vulkan_root);
    if (vulkan_id)
        (*env)->ReleaseStringUTFChars(env, driver_id, vulkan_id);
    if (vulkan_hooks)
        (*env)->ReleaseStringUTFChars(env, hook_dir, vulkan_hooks);
    if (dxvk)
        (*env)->ReleaseStringUTFChars(env, dxvk_version, dxvk);
    if (dxvk_config)
        (*env)->ReleaseStringUTFChars(env, dxvk_config_path, dxvk_config);
}

static void canvas_attach_provider(struct android_app *app)
{
    if (g_cache_attached || !app || !app->activity ||
        !app->activity->vm || !app->activity->javaGameActivity)
        return;

    JavaVM *vm = app->activity->vm;
    JNIEnv *env = NULL;
    int attached = 0;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return;
        attached = 1;
    }
    if (overlay_canvas_attach(env, app->activity->javaGameActivity) == 0) {
        g_cache_attached = 1;
        OVERLAY_LOGI("Canvas atlas provider attached");
    }
    if (attached) (*vm)->DetachCurrentThread(vm);
}

static void canvas_detach_provider(struct android_app *app)
{
    if (!g_cache_attached || !app || !app->activity || !app->activity->vm)
        return;

    JavaVM *vm = app->activity->vm;
    JNIEnv *env = NULL;
    int attached = 0;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return;
        attached = 1;
    }
    overlay_canvas_detach(env);
    g_cache_attached = 0;
    if (attached) (*vm)->DetachCurrentThread(vm);
}

static GameActivity *host_activity(void)
{
    return g_app && g_app->activity ? g_app->activity : NULL;
}

static void ime_clear_locked(void)
{
    g_ime_text_len = 0;
    if (g_ime_text) g_ime_text[0] = '\0';
}

/* Selection offsets in GameTextInputState are UTF-16 code units, while the
 * text is modified UTF-8.  Count decoded units, not bytes. */
static int ime_text_units(const char *text, int length)
{
    int units = 0;
    int pos = 0;
    while (text && pos < length) {
        int step = wc3_legacy_mutf8_next(text, length, pos, NULL);
        if (step <= 0) break;
        pos += step;
        units++;
    }
    return units;
}

static int ime_reserve_locked(int length)
{
    if (length < 0) return 0;
    if (length + 1 <= g_ime_text_cap) return 1;

    int cap = g_ime_text_cap > 0 ? g_ime_text_cap : 64;
    while (cap < length + 1) {
        if (cap > 1 << 20) {
            cap = length + 1;
            break;
        }
        cap *= 2;
    }
    char *next = (char *)realloc(g_ime_text, (size_t)cap);
    if (!next) return 0;
    g_ime_text = next;
    g_ime_text_cap = cap;
    return 1;
}

static ImeFieldText *ime_field_lookup_locked(uint32_t id)
{
    if (!id) return NULL;
    for (int i = 0; i < IME_FIELD_SLOTS; i++) {
        if (g_ime_fields[i].id == id) return &g_ime_fields[i];
    }
    return NULL;
}

static ImeFieldText *ime_field_claim_locked(uint32_t id)
{
    ImeFieldText *free_slot = NULL;
    ImeFieldText *oldest = NULL;
    for (int i = 0; i < IME_FIELD_SLOTS; i++) {
        ImeFieldText *entry = &g_ime_fields[i];
        if (entry->id == id) return entry;
        if (!entry->id) {
            if (!free_slot) free_slot = entry;
        } else if (!oldest || entry->used < oldest->used) {
            oldest = entry;
        }
    }

    ImeFieldText *entry = free_slot ? free_slot : oldest;
    if (!entry) return NULL;
    free(entry->text);
    entry->text = NULL;
    entry->length = 0;
    entry->id = id;
    entry->used = 0;
    return entry;
}

/* Park the current field's mirror before another field replaces it. */
static void ime_field_save_locked(void)
{
    if (!g_ime_field_id) return;

    char *copy = NULL;
    if (g_ime_text_len > 0) {
        copy = (char *)malloc((size_t)g_ime_text_len + 1u);
        if (!copy) return;
        memcpy(copy, g_ime_text, (size_t)g_ime_text_len);
        copy[g_ime_text_len] = '\0';
    }

    ImeFieldText *entry = ime_field_claim_locked(g_ime_field_id);
    if (!entry) {
        free(copy);
        return;
    }
    free(entry->text);
    entry->text = copy;
    entry->length = g_ime_text_len;
    entry->used = ++g_ime_field_clock;
}

static void ime_field_load_locked(uint32_t id)
{
    g_ime_field_id = id;
    ImeFieldText *entry = ime_field_lookup_locked(id);
    if (!entry) {
        ime_clear_locked();
        return;
    }

    if (ime_reserve_locked(entry->length)) {
        if (entry->length > 0)
            memcpy(g_ime_text, entry->text, (size_t)entry->length);
        g_ime_text[entry->length] = '\0';
        g_ime_text_len = entry->length;
    } else {
        ime_clear_locked();
    }
    entry->used = ++g_ime_field_clock;
}

static void ime_fields_clear_locked(void)
{
    for (int i = 0; i < IME_FIELD_SLOTS; i++) {
        free(g_ime_fields[i].text);
        memset(&g_ime_fields[i], 0, sizeof g_ime_fields[i]);
    }
    g_ime_field_id = 0;
    g_ime_field_clock = 0;
}

static float ime_input_pan(void)
{
    pthread_mutex_lock(&g_ime_lock);
    const float pan = g_ime_pan_px;
    pthread_mutex_unlock(&g_ime_lock);
    return pan;
}

/* Apply the current pan on the UI thread.  The runnable asks the native side
 * for the value when it runs, so a show from the guest thread and an inset
 * update from the UI thread cannot overwrite a newer translation with an
 * older one. */
static void ime_request_pan_refresh(GameActivity *activity)
{
    if (!activity || !activity->vm || !activity->javaGameActivity)
        return;

    JavaVM *vm = activity->vm;
    JNIEnv *env = NULL;
    int attached = 0;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) {
            IME_LOGE("IME pan: cannot attach thread");
            return;
        }
        attached = 1;
    }

    jclass cls = (*env)->GetObjectClass(env, activity->javaGameActivity);
    jmethodID method = cls
        ? (*env)->GetMethodID(env, cls, "requestImePan", "()V")
        : NULL;
    if (method) {
        (*env)->CallVoidMethod(env, activity->javaGameActivity, method);
    } else {
        IME_LOGE("IME pan: requestImePan missing");
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
    if (cls) (*env)->DeleteLocalRef(env, cls);
    if (attached) (*vm)->DetachCurrentThread(vm);
}

static void ime_pan_update(GameActivity *activity)
{
    pthread_mutex_lock(&g_window_lock);
    const uint32_t surface_h = g_surface_h;
    pthread_mutex_unlock(&g_window_lock);

    float previous;
    float desired;
    int inset;
    int rect_valid;
    Wc3LegacyKeyboardRect rect;

    pthread_mutex_lock(&g_ime_lock);
    previous = g_ime_pan_px;
    inset = g_ime_inset_bottom;
    rect_valid = g_ime_rect_valid;
    rect = g_ime_rect;
    if (g_ime_active && rect_valid)
        desired = wc3_legacy_keyboard_pan_px(&rect, inset, surface_h);
    else
        desired = 0.0f;
    g_ime_pan_px = desired;
    pthread_mutex_unlock(&g_ime_lock);

    if (desired == previous)
        return;

    if (rect_valid) {
        IME_LOG("pan %.1f -> %.1f inset=%d surface=%u field=%.3f,%.3f..%.3f,%.3f",
                previous, desired, inset, (unsigned)surface_h,
                rect.left, rect.top, rect.right, rect.bottom);
    } else {
        IME_LOG("pan %.1f -> %.1f inset=%d surface=%u field=none",
                previous, desired, inset, (unsigned)surface_h);
    }
    ime_request_pan_refresh(activity);
}

/* The IME inset is the only reliable keyboard height: the native window stays
 * full-screen and the swap chain keeps its extent while the IME is open. */
static void ime_read_insets(GameActivity *activity)
{
    if (!activity) return;

    ARect insets;
    memset(&insets, 0, sizeof insets);
    GameActivity_getWindowInsets(activity, GAMECOMMON_INSETS_TYPE_IME,
                                 &insets);
    const int bottom = insets.bottom > 0 ? (int)insets.bottom : 0;

    pthread_mutex_lock(&g_ime_lock);
    g_ime_inset_bottom = bottom;
    pthread_mutex_unlock(&g_ime_lock);

    IME_LOG("inset bottom=%d", bottom);
    ime_pan_update(activity);
}

static void ime_window_insets(GameActivity *activity)
{
    ime_read_insets(activity);
}

/* Called from the guest thread when the game focuses a text field.  The field
 * identity comes from the guest's CEditBox object, so a hide/show of the same
 * field keeps its mirror while switching to another field restores that
 * field's own mirror.  The editor buffer deliberately survives a hide/show
 * cycle: it is the only record of the text this bridge has already delivered
 * to the guest.  Clearing it while the game keeps its field contents makes
 * Backspace unable to remove anything that was typed before the keyboard was
 * dismissed. */
static void ime_show(uint32_t field_id, const Wc3LegacyKeyboardRect *rect)
{
    GameActivity *activity = host_activity();
    if (!activity) return;

    pthread_mutex_lock(&g_ime_lock);
    g_ime_active = 1;
    g_ime_rect_valid = rect != NULL;
    if (rect) g_ime_rect = *rect;
    if (field_id && field_id != g_ime_field_id) {
        ime_field_save_locked();
        ime_field_load_locked(field_id);
    }
    const int length = g_ime_text_len;
    char *text = NULL;
    if (length > 0) {
        text = (char *)malloc((size_t)length + 1u);
        if (text) {
            memcpy(text, g_ime_text, (size_t)length);
            text[length] = '\0';
        }
    }
    pthread_mutex_unlock(&g_ime_lock);

    /* Show first: setSoftKeyboardActive() creates the input connection and
     * gives the surface focus.  Restoring the state after that keeps the
     * editor mirror identical to the guest field and puts the cursor at the
     * end.  Doing it in the opposite order restarts an inactive connection,
     * which some IMEs do not recover from on the next show. */
    GameActivity_showSoftInput(activity,
                               GAMEACTIVITY_SHOW_SOFT_INPUT_IMPLICIT);

    if (length > 0 && text) {
        GameTextInputState state;
        memset(&state, 0, sizeof state);
        const int cursor = ime_text_units(text, length);
        state.text_UTF8 = text;
        state.text_length = length;
        state.selection.start = cursor;
        state.selection.end = cursor;
        state.composingRegion.start = SPAN_UNDEFINED;
        state.composingRegion.end = SPAN_UNDEFINED;
        GameActivity_setTextInputState(activity, &state);
    } else if (length == 0) {
        GameTextInputState empty;
        memset(&empty, 0, sizeof empty);
        empty.text_UTF8 = "";
        empty.text_length = 0;
        empty.selection.start = 0;
        empty.selection.end = 0;
        empty.composingRegion.start = SPAN_UNDEFINED;
        empty.composingRegion.end = SPAN_UNDEFINED;
        GameActivity_setTextInputState(activity, &empty);
    } else {
        IME_LOGE("IME show: cannot mirror %d text bytes", length);
    }
    free(text);

    if (rect) {
        IME_LOG("show field=%08X text=%d rect=%.3f,%.3f..%.3f,%.3f",
                field_id, length, rect->left, rect->top, rect->right,
                rect->bottom);
    } else {
        IME_LOG("show field=%08X text=%d rect=none", field_id, length);
    }
    LOGI("IME show field=%08X text=%d", field_id, length);
    ime_pan_update(activity);
}

static void ime_hide(void)
{
    GameActivity *activity = host_activity();

    pthread_mutex_lock(&g_ime_lock);
    g_ime_active = 0;
    g_ime_rect_valid = 0;
    const int length = g_ime_text_len;
    pthread_mutex_unlock(&g_ime_lock);

    if (activity) {
        GameActivity_hideSoftInput(activity,
                                   GAMEACTIVITY_HIDE_SOFT_INPUT_NOT_ALWAYS);
    }
    IME_LOG("hide text=%d kept", length);
    LOGI("IME hide requested (text=%d kept)", length);
    ime_pan_update(activity);
}

static void ime_text_changed(GameActivity *activity,
                             const GameTextInputState *state)
{
    (void)activity;
    if (!state || !state->text_UTF8) return;

    const char *text = state->text_UTF8;
    int length = state->text_length;
    if (length < 0) length = (int)strlen(text);

    LOGI("IME text len=%d sel=%d..%d comp=%d..%d", length,
         state->selection.start, state->selection.end,
         state->composingRegion.start, state->composingRegion.end);

    pthread_mutex_lock(&g_ime_lock);
    if (!g_ime_active) {
        /* Ignore a late event from a hidden editor, but keep the baseline:
         * the next show restores the same state into the editor and any edit
         * must still diff against what the guest has actually received. */
        pthread_mutex_unlock(&g_ime_lock);
        IME_LOG("text ignored inactive len=%d", length);
        return;
    }

    Wc3LegacyTextDiff diff;
    wc3_legacy_text_diff(g_ime_text ? g_ime_text : "", g_ime_text_len,
                         text, length, &diff);
    for (int i = 0; i < diff.removed; i++)
        wc3_legacy_post_backspace();

    int pos = diff.new_start;
    while (pos < length) {
        uint16_t unit = 0;
        int step = wc3_legacy_mutf8_next(text, length, pos, &unit);
        if (step <= 0) break;
        pos += step;
        if (unit) wc3_legacy_post_char((uint32_t)unit);
    }

    if (ime_reserve_locked(length)) {
        memcpy(g_ime_text, text, (size_t)length);
        g_ime_text[length] = '\0';
        g_ime_text_len = length;
    } else {
        ime_clear_locked();
    }
    pthread_mutex_unlock(&g_ime_lock);

    LOGI("IME apply removed=%d added=%d", diff.removed, diff.added);
}

static bool ime_editor_action(GameActivity *activity, int action)
{
    (void)activity;
    if (action == IME_ACTION_DONE || action == IME_ACTION_GO ||
        action == IME_ACTION_SEND) {
        LOGI("IME editor action %d -> Enter", action);
        wc3_legacy_post_enter();
        return true;
    }
    return false;
}

/* Insets-based visibility is advisory only.  GameTextInput propagates text
 * changes even when this callback says the IME is invisible, because the
 * platform's keyboard-visibility state is unreliable.  The bridge therefore
 * tracks its own show/hide lifetime and never clears the text baseline here:
 * doing so dropped keystrokes while the IME was actually up and made the next
 * state diff replay text the guest had already received. */
static void ime_visibility(GameActivity *activity, bool visible)
{
    wc3_legacy_set_keyboard_visible(visible ? 1 : 0);
    LOGI("IME visibility %d", visible ? 1 : 0);
    ime_read_insets(activity);
}

static void ime_install(struct android_app *app)
{
    if (!app || !app->activity || !app->activity->callbacks) {
        LOGE("GameActivity callbacks are NULL; IME bridge unavailable");
        return;
    }

    GameActivityCallbacks *callbacks = app->activity->callbacks;
    callbacks->onTextInputEvent = ime_text_changed;
    callbacks->onEditorAction = ime_editor_action;
    callbacks->onSoftwareKeyboardVisibilityChanged = ime_visibility;
    callbacks->onWindowInsetsChanged = ime_window_insets;
    wc3_legacy_set_keyboard_bridge(ime_show, ime_hide);
    LOGI("IME callbacks installed");
}

static void ime_shutdown(struct android_app *app)
{
    wc3_legacy_set_keyboard_bridge(NULL, NULL);
    wc3_legacy_set_keyboard_visible(0);

    if (app && app->activity && app->activity->callbacks) {
        GameActivityCallbacks *callbacks = app->activity->callbacks;
        callbacks->onTextInputEvent = NULL;
        callbacks->onEditorAction = NULL;
        callbacks->onSoftwareKeyboardVisibilityChanged = NULL;
        callbacks->onWindowInsetsChanged = NULL;
    }

    pthread_mutex_lock(&g_ime_lock);
    ime_clear_locked();
    ime_fields_clear_locked();
    free(g_ime_text);
    g_ime_text = NULL;
    g_ime_text_len = 0;
    g_ime_text_cap = 0;
    g_ime_active = 0;
    g_ime_rect_valid = 0;
    g_ime_inset_bottom = 0;
    g_ime_pan_px = 0.0f;
    pthread_mutex_unlock(&g_ime_lock);

    if (app && app->activity)
        ime_request_pan_refresh(app->activity);
}

static void haptics_init(struct android_app *app)
{
    if (g_haptics_ready || !app || !app->activity ||
        !app->activity->vm || !app->activity->javaGameActivity)
        return;

    JavaVM *vm = app->activity->vm;
    JNIEnv *env = NULL;
    int attached = 0;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return;
        attached = 1;
    }
    if (haptics_android_init(env, vm, app->activity->javaGameActivity) == 0)
        g_haptics_ready = 1;
    if (attached) (*vm)->DetachCurrentThread(vm);
}

static void host_haptic_sink(int effect, void *user)
{
    (void)user;
    if (g_haptics_ready) haptics_android_perform(effect);
}

static void *input_thread_main(void *arg)
{
    (void)arg;
    WC3_TRACE_THREAD_NAME("wc3-input");
    haptics_android_attach_thread();
    while (!atomic_load_explicit(&g_input_stop, memory_order_acquire)) {
        uint64_t now = host_now_ms();
        {
            WC3_TRACE_SCOPE("wc3.input.drain");
            input_adapter_drain(&g_input_adapter, now);
        }
        const Wc3InputAdapterStats *input_stats =
            input_adapter_stats(&g_input_adapter);
        /* A move-only batch is published only when the engine marked the
         * overlay dirty.  Down/up/cancel keep the input-event fallback so a
         * missing mark can never leave a control stuck on screen. */
        int visual_dirty = controls_visual_dirty() ||
                           (input_stats && input_stats->last_control_events > 0);
        int pending_visual = 0;
        if (visual_dirty) {
            WC3_TRACE_SCOPE("wc3.input.publish");
            publish_visual();
            /* A failed sync or an unavailable snapshot must be retried even
             * when the engine has no timer left to wake the input thread. */
            pending_visual = controls_visual_dirty();
        }
#if WC3_DEBUG && WC3_DEBUG_INPUT
        else {
            g_dbg_publish_skipped++;
            if ((g_dbg_publish_skipped % 600u) == 0u) {
                OVERLAY_LOGI("overlay idle publish skips=%u",
                             g_dbg_publish_skipped);
            }
        }
#endif

        uint64_t deadline = input_adapter_next_deadline_ms(&g_input_adapter,
                                                           now);
        int timeout = -1;
        if (deadline != 0) {
            uint64_t after = host_now_ms();
            timeout = deadline > after ? (int)(deadline - after) : 0;
            if (timeout > 1000) timeout = 1000;
        }
        if (pending_visual && (timeout < 0 || timeout > 16))
            timeout = 16;
        /* timeout == -1 is the idle state: block until a touch or stop. */
        input_wake_wait(g_input_wake, timeout);
    }
    haptics_android_detach_thread();
    return NULL;
}

static int overlay_input_start(int width, int height)
{
    if (width <= 0 || height <= 0) return -1;
    if (atomic_load_explicit(&g_input_ready, memory_order_acquire)) {
        if (width == g_input_width && height == g_input_height) return 0;
        overlay_input_stop();
    }

    char *profile = load_controls_profile();
    if (!profile) {
        OVERLAY_LOGE("controls profile missing; using the one-finger fallback");
        return -1;
    }

#if WC3_WITH_ASBUILT_HLE
    overlay_actions_init(width, height);
    overlay_actions_set_haptic_sink(host_haptic_sink, NULL);
    controls_set_action_sink(overlay_actions_handle, NULL);
#else
    controls_set_action_sink(NULL, NULL);
#endif

    if (controls_init_ex(profile, width, height,
                         g_double_tap_gestures != 0,
                         g_double_tap_timeout_ms,
                         g_long_press_timeout_ms) != 0) {
        OVERLAY_LOGE("controls init failed: %s", controls_last_error());
        free(profile);
        controls_set_action_sink(NULL, NULL);
#if WC3_WITH_ASBUILT_HLE
        overlay_actions_set_haptic_sink(NULL, NULL);
#endif
        return -1;
    }
    free(profile);

    if (visual_initialize(width, height) != 0)
        OVERLAY_LOGE("overlay visual initialization failed; input continues");

    input_ring_init(&g_touch_ring);
    g_input_wake = input_wake_create();
    if (!g_input_wake) {
        OVERLAY_LOGE("cannot create the controls wake primitive");
        controls_cancel_all();
#if WC3_WITH_ASBUILT_HLE
        overlay_actions_release_all();
#endif
        controls_set_action_sink(NULL, NULL);
#if WC3_WITH_ASBUILT_HLE
        overlay_actions_set_haptic_sink(NULL, NULL);
#endif
        input_ring_destroy(&g_touch_ring);
        visual_destroy();
        return -1;
    }

    input_adapter_init(&g_input_adapter, &g_touch_ring);
    g_input_width = width;
    g_input_height = height;
    atomic_store_explicit(&g_input_stop, 0, memory_order_release);

    if (pthread_create(&g_input_thread, NULL, input_thread_main, NULL) != 0) {
        OVERLAY_LOGE("cannot start the controls input thread");
        controls_cancel_all();
#if WC3_WITH_ASBUILT_HLE
        overlay_actions_release_all();
#endif
        controls_set_action_sink(NULL, NULL);
#if WC3_WITH_ASBUILT_HLE
        overlay_actions_set_haptic_sink(NULL, NULL);
#endif
        input_wake_destroy(g_input_wake);
        g_input_wake = NULL;
        input_ring_destroy(&g_touch_ring);
        visual_destroy();
        return -1;
    }
    g_input_thread_started = 1;
    atomic_store_explicit(&g_input_ready, 1, memory_order_release);
    OVERLAY_LOGI("controls ready %dx%d (ring -> adapter -> engine)", width,
                 height);
    return 0;
}

static void overlay_input_stop(void)
{
    if (!atomic_exchange_explicit(&g_input_ready, 0, memory_order_acq_rel)) {
        visual_destroy();
        return;
    }
    atomic_store_explicit(&g_input_stop, 1, memory_order_release);
    if (g_input_wake) input_wake_signal(g_input_wake);
    if (g_input_thread_started) {
        pthread_join(g_input_thread, NULL);
        g_input_thread_started = 0;
    }

    controls_cancel_all();
#if WC3_WITH_ASBUILT_HLE
    overlay_actions_release_all();
#endif
    controls_set_action_sink(NULL, NULL);
#if WC3_WITH_ASBUILT_HLE
    overlay_actions_set_haptic_sink(NULL, NULL);
#endif
    if (g_input_wake) {
        input_wake_destroy(g_input_wake);
        g_input_wake = NULL;
    }
    input_ring_destroy(&g_touch_ring);
    visual_destroy();
    g_input_width = 0;
    g_input_height = 0;
    OVERLAY_LOGI("controls stopped");
}

static int queue_touch(Wc3TouchPhase phase, int32_t pointer_id, float x,
                       float y, uint64_t event_time_ms)
{
    WC3_TRACE_SCOPE("wc3.input.queue");
    if (!atomic_load_explicit(&g_input_ready, memory_order_acquire) ||
        !g_input_wake)
        return -1;
    Wc3TouchEvent event;
    memset(&event, 0, sizeof event);
    event.event_time_ms = event_time_ms ? event_time_ms : host_now_ms();
    event.pointer_id = pointer_id;
    event.x = x;
    event.y = y;
    event.phase = (uint8_t)phase;
    int rc = input_ring_push(&g_touch_ring, &event);
    input_wake_signal(g_input_wake);
    return rc;
}

static void *boot_thread(void *arg)
{
    (void)arg;
    ANativeWindow *window = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    WC3_TRACE_THREAD_NAME("wc3-boot");
    WC3_TRACE_SCOPE("wc3.boot.thread");

    /* GameActivity can replace the starting surface once the activity is laid
     * out.  The pointer must still survive one settle interval, but a surface
     * published before this thread starts has already done so: sleep only for
     * the remaining part of that interval instead of always waiting 300 ms. */
    int stable = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        pthread_mutex_lock(&g_window_lock);
        window = g_window;
        width = g_surface_w;
        height = g_surface_h;
        uint64_t since = g_window_since_ms;
        pthread_mutex_unlock(&g_window_lock);

        if (!window || !width || !height) {
            usleep(WINDOW_SETTLE_MS * 1000u);
            continue;
        }

        uint64_t remaining = wc3_window_settle_remaining_ms(
            since, host_now_ms(), WINDOW_SETTLE_MS);
        if (remaining == 0u) {
            stable = 1;
            break;
        }
        usleep((useconds_t)(remaining * 1000u));
    }
    if (!stable || !window || !width || !height) {
        LOGE("no stable ANativeWindow for the guest");
        return NULL;
    }

    int rc = wc3_app_boot_start(g_data_dir, g_state_dir, window, width, height,
                                g_game_mode);
    if (rc != 0)
        LOGE("guest boot failed: %d", rc);
    return NULL;
}

static void on_app_cmd(struct android_app *app, int32_t cmd)
{
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        wc3_android_surface_set_window(app->window);
        if (app->window) {
            uint32_t width = (uint32_t)ANativeWindow_getWidth(app->window);
            uint32_t height = (uint32_t)ANativeWindow_getHeight(app->window);
            pthread_mutex_lock(&g_window_lock);
            g_window = app->window;
            g_surface_w = width;
            g_surface_h = height;
            g_window_since_ms = host_now_ms();
            pthread_mutex_unlock(&g_window_lock);
            /* Republish every replacement surface to the DXVK shim.  Without
             * this, Present keeps skipping frames after the activity comes
             * back because its window slot was cleared on TERM_WINDOW. */
            wc3_legacy_set_screen_size(width, height);
            wc3_android_set_window(app->window, width, height);
            if (wc3_android_set_surface_size(width, height) != 0)
                LOGE("setBuffersGeometry(%ux%u) failed", width, height);
#if WC3_DEBUG
            const Wc3Platform *platform = wc3_platform();
            uintptr_t surface = 0;
            if (platform && platform->surface.acquire &&
                platform->surface.acquire(platform->surface.ctx, &surface) ==
                    0) {
                LOGI("surface acquire %p", (void *)surface);
                platform->surface.release(platform->surface.ctx, surface);
            } else {
                LOGI("surface acquire failed");
            }
#endif
            LOGI("window %ux%u", width, height);
            overlay_input_start((int)width, (int)height);
            ime_pan_update(app->activity);
#if WC3_WITH_ASBUILT_HLE
            if (!g_boot_started && g_data_dir[0]) {
                g_boot_started = 1;
                if (pthread_create(&g_boot_thread, NULL, boot_thread, NULL) != 0) {
                    g_boot_started = 0;
                    LOGE("cannot start guest thread");
                }
            }
#endif
        } else {
            LOGE("APP_CMD_INIT_WINDOW without a window");
        }
        break;
    case APP_CMD_PAUSE:
    case APP_CMD_STOP:
    case APP_CMD_SAVE_STATE:
        /* The activity may be finished or killed without the game's own quit
         * path.  Ask the guest thread to persist its in-memory preferences at
         * the next message-pump boundary; this also covers settings the player
         * changed but did not explicitly apply. */
        wc3_android_request_settings_save();
        break;
    case APP_CMD_TERM_WINDOW: {
        ANativeWindow *window;
        wc3_android_request_settings_save();
        ime_hide();
        overlay_input_stop();
        pthread_mutex_lock(&g_window_lock);
        window = g_window;
        g_window = NULL;
        g_surface_w = 0;
        g_surface_h = 0;
        g_window_since_ms = 0;
        pthread_mutex_unlock(&g_window_lock);
        if (!window)
            window = app->window;
        if (window)
            wc3_android_clear_window(window);
        wc3_android_surface_set_window(NULL);
        LOGI("window terminated");
        break;
    }
    case APP_CMD_WINDOW_RESIZED:
        wc3_android_surface_set_window(app->window);
        if (app->window) {
            uint32_t width = (uint32_t)ANativeWindow_getWidth(app->window);
            uint32_t height = (uint32_t)ANativeWindow_getHeight(app->window);
            pthread_mutex_lock(&g_window_lock);
            g_window = app->window;
            g_surface_w = width;
            g_surface_h = height;
            g_window_since_ms = host_now_ms();
            pthread_mutex_unlock(&g_window_lock);
            wc3_legacy_set_screen_size(width, height);
            wc3_android_set_window(app->window, width, height);
            if (wc3_android_set_surface_size(width, height) != 0)
                LOGE("setBuffersGeometry(%ux%u) failed", width, height);
            const Wc3Platform *platform = wc3_platform();
            if (platform && platform->surface.resize) {
                platform->surface.resize(platform->surface.ctx, width, height);
            }
            overlay_input_start((int)width, (int)height);
            LOGI("window resized %ux%u", width, height);
            ime_pan_update(app->activity);
        }
        break;
    case APP_CMD_DESTROY: {
        ANativeWindow *window;
        wc3_android_request_settings_save();
        ime_hide();
        overlay_input_stop();
        overlay_stop();
        pthread_mutex_lock(&g_window_lock);
        window = g_window;
        g_window = NULL;
        g_surface_w = 0;
        g_surface_h = 0;
        g_window_since_ms = 0;
        pthread_mutex_unlock(&g_window_lock);
        if (!window)
            window = app->window;
        if (window)
            wc3_android_clear_window(window);
        wc3_android_surface_set_window(NULL);
        LOGI("destroy requested");
        break;
    }
    default:
        break;
    }
}

/*
 * Input path.  GameActivity's native glue buffers motion/key events on the
 * activity thread; this thread drains that buffer and pushes one event at a
 * time into the platform's lock-free SPSC mailbox.  No HLE or dispatcher call
 * happens here.
 */
static int32_t pointer_coord(float value)
{
    if (value != value) return 0;
    if (value >= 2147483520.0f) return INT32_MAX;
    if (value <= -2147483648.0f) return INT32_MIN;
    return (int32_t)lrintf(value);
}

static uint64_t motion_time_ns(int64_t java_milliseconds)
{
    if (java_milliseconds <= 0) return 0;
    return (uint64_t)java_milliseconds * 1000000ull;
}

static uint64_t key_time_ns(int64_t nanoseconds)
{
    return nanoseconds > 0 ? (uint64_t)nanoseconds : 0;
}

static void push_input(const Wc3InputEvent *event)
{
    if (!g_platform_ready || !g_platform.input.push || !event) return;
    if (g_platform.input.push(g_platform.input.ctx, event) != 0) return;
#if WC3_DEBUG
    if (event->kind == WC3_INPUT_POINTER_DOWN) {
        LOGI("input pointer down id=%u x=%d y=%d",
             (unsigned)event->pointer_id, event->x, event->y);
    } else if (event->kind == WC3_INPUT_KEY_DOWN) {
        LOGI("input key down code=%u", (unsigned)event->key);
    }
#endif
}

static void push_pointer(uint32_t kind, const GameActivityPointerAxes *pointer,
                         uint32_t modifiers, uint64_t time_ns)
{
    if (!pointer) return;
    const float x = GameActivityPointerAxes_getX(pointer);
    /* The surface itself is translated up while the IME covers a field.
     * GameActivity hands us window coordinates, so undo that translation
     * before the guest hit-tests the point. */
    const float y = GameActivityPointerAxes_getY(pointer) + ime_input_pan();
    Wc3InputEvent event;
    memset(&event, 0, sizeof event);
    event.kind = kind;
    event.pointer_id = (uint32_t)pointer->id;
    event.x = pointer_coord(x);
    event.y = pointer_coord(y);
    event.modifiers = modifiers;
    event.time_ns = time_ns;
    if (kind == WC3_INPUT_POINTER_DOWN)
        IME_LOG("pointer down x=%d y=%d", event.x, event.y);
    push_input(&event);

#if WC3_WITH_ASBUILT_HLE
    /* The game only consumes one virtual cursor.  The touch fraction of the
     * physical surface is the same fraction of DXVK's scaled client image, so
     * no display-mode conversion is needed here. */
    if (pointer->id == 0) {
        float nx = g_surface_w ? x / (float)g_surface_w : 0.5f;
        float ny = g_surface_h ? y / (float)g_surface_h : 0.5f;
        wc3_legacy_mouse_move(nx, ny);
        if (kind == WC3_INPUT_POINTER_DOWN)
            wc3_legacy_mouse_button(1);
        else if (kind == WC3_INPUT_POINTER_UP)
            wc3_legacy_mouse_button(0);
    }
#endif
}

static float pointer_x(const GameActivityPointerAxes *pointer)
{
    return pointer->axisValues[AMOTION_EVENT_AXIS_X];
}

static float pointer_y(const GameActivityPointerAxes *pointer)
{
    return pointer->axisValues[AMOTION_EVENT_AXIS_Y];
}

static void queue_pointer(Wc3TouchPhase phase,
                          const GameActivityPointerAxes *pointer,
                          uint64_t event_time_ms)
{
    if (!pointer) return;
    queue_touch(phase, pointer->id, pointer_x(pointer),
                pointer_y(pointer) + ime_input_pan(), event_time_ms);
}

static void queue_overlay_motion(const GameActivityMotionEvent *motion)
{
    if (!motion || motion->pointerCount == 0) return;
    uint32_t count = motion->pointerCount;
    if (count > GAMEACTIVITY_MAX_NUM_POINTERS_IN_MOTION_EVENT)
        count = GAMEACTIVITY_MAX_NUM_POINTERS_IN_MOTION_EVENT;

    const int32_t action = motion->action & AMOTION_EVENT_ACTION_MASK;
    uint32_t index =
        (uint32_t)(motion->action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
        AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    const uint64_t event_ms = motion->eventTime > 0
                                  ? (uint64_t)motion->eventTime
                                  : host_now_ms();

    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:
        queue_pointer(WC3_TOUCH_DOWN, &motion->pointers[0], event_ms);
        break;
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        if (index < count)
            queue_pointer(WC3_TOUCH_DOWN, &motion->pointers[index], event_ms);
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        for (uint32_t i = 0; i < count; i++)
            queue_pointer(WC3_TOUCH_MOVE, &motion->pointers[i], event_ms);
        break;
    case AMOTION_EVENT_ACTION_UP:
        queue_pointer(WC3_TOUCH_UP, &motion->pointers[0], event_ms);
        break;
    case AMOTION_EVENT_ACTION_POINTER_UP:
        if (index < count)
            queue_pointer(WC3_TOUCH_UP, &motion->pointers[index], event_ms);
        break;
    case AMOTION_EVENT_ACTION_CANCEL:
        queue_touch(WC3_TOUCH_CANCEL, 0, 0.0f, 0.0f, event_ms);
        break;
    default:
        break;
    }
}

static void process_motion(const GameActivityMotionEvent *motion)
{
    if (!motion || motion->pointerCount == 0) return;

    if (atomic_load_explicit(&g_input_ready, memory_order_acquire)) {
        queue_overlay_motion(motion);
        return;
    }

    uint32_t count = motion->pointerCount;
    if (count > GAMEACTIVITY_MAX_NUM_POINTERS_IN_MOTION_EVENT)
        count = GAMEACTIVITY_MAX_NUM_POINTERS_IN_MOTION_EVENT;

    const int32_t action = motion->action & AMOTION_EVENT_ACTION_MASK;
    uint32_t index =
        (uint32_t)(motion->action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
        AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    const uint32_t modifiers = (uint32_t)motion->metaState;
    const uint64_t time_ns = motion_time_ns(motion->eventTime);

    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        if (index < count)
            push_pointer(WC3_INPUT_POINTER_DOWN, &motion->pointers[index],
                         modifiers, time_ns);
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
        if (index < count)
            push_pointer(WC3_INPUT_POINTER_UP, &motion->pointers[index],
                         modifiers, time_ns);
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        for (uint32_t i = 0; i < count; i++)
            push_pointer(WC3_INPUT_POINTER_MOVE, &motion->pointers[i],
                         modifiers, time_ns);
        break;
    case AMOTION_EVENT_ACTION_CANCEL:
        for (uint32_t i = 0; i < count; i++)
            push_pointer(WC3_INPUT_POINTER_UP, &motion->pointers[i],
                         modifiers, time_ns);
        break;
    default:
        break;
    }
}

static void process_key(const GameActivityKeyEvent *key, int down)
{
    if (!key) return;
    Wc3InputEvent event;
    memset(&event, 0, sizeof event);
    event.kind = down ? WC3_INPUT_KEY_DOWN : WC3_INPUT_KEY_UP;
    event.key = (uint32_t)key->keyCode;
    if (key->unicodeChar > 0)
        event.codepoint = (uint32_t)key->unicodeChar;
    event.modifiers = (uint32_t)(key->metaState | key->modifiers);
    event.time_ns = key_time_ns(key->eventTime);
    push_input(&event);
}

static void drain_input(struct android_app *app)
{
    if (!app) return;
    struct android_input_buffer *buffer = android_app_swap_input_buffers(app);
    if (!buffer) return;

    for (uint64_t i = 0; i < buffer->motionEventsCount; i++)
        process_motion(&buffer->motionEvents[i]);
    for (uint64_t i = 0; i < buffer->keyEventsCount; i++) {
        const GameActivityKeyEvent *key = &buffer->keyEvents[i];
        if (key->action == AKEY_EVENT_ACTION_DOWN)
            process_key(key, 1);
        else if (key->action == AKEY_EVENT_ACTION_UP)
            process_key(key, 0);
    }

    if (buffer->motionEventsCount)
        android_app_clear_motion_events(buffer);
    if (buffer->keyEventsCount)
        android_app_clear_key_events(buffer);
}

static void inspect_assets(void)
{
    static const char *const required[] = {
        "war3.mpq", "War3x.mpq", "War3xlocal.mpq", "War3Patch.mpq",
    };

    g_asset_missing_count = 0;
    g_asset_missing[0] = '\0';
    if (!g_asset_root_ready || !g_platform.asset.present) return;

    size_t used = 0;
    for (size_t i = 0; i < sizeof required / sizeof required[0]; i++) {
        if (g_platform.asset.present(g_platform.asset.ctx, required[i]) == 1)
            continue;
        g_asset_missing_count++;
        int n = snprintf(g_asset_missing + used,
                         sizeof g_asset_missing - used, "%s%s",
                         used ? ", " : "", required[i]);
        if (n < 0) break;
        if ((size_t)n >= sizeof g_asset_missing - used) {
            used = sizeof g_asset_missing - 1u;
            break;
        }
        used += (size_t)n;
    }
}

#if !WC3_WITH_ASBUILT_HLE
static int load_image(const char *dir, const char *name, uint32_t base)
{
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/%s", dir, name);
    if (n <= 0 || (size_t)n >= sizeof path) {
        LOGE("path too long: %s/%s", dir, name);
        return -1;
    }

    Wc3PeImage image;
    memset(&image, 0, sizeof image);
    if (wc3_pe_load(&g_platform, path, 0, &image, 0) != 0) {
        LOGE("PE missing or unreadable: %s", path);
        return -1;
    }
    if (image.base != base) {
        LOGE("PE %s mapped at %08X, expected %08X", name, image.base, base);
        return -1;
    }
    LOGI("PE %s base=%08X span=%X entry=%08X imports=%08X exports=%08X",
         name, image.base, image.span, image.entry, image.import_rva,
         image.export_rva);
    return 0;
}
#endif

void android_main(struct android_app *app)
{
    WC3_TRACE_THREAD_NAME("wc3-ui");
    WC3_TRACE_SCOPE("wc3.android_main");
    LOGI("android_main pid=%d", (int)getpid());
    g_app = app;
    app->onAppCmd = on_app_cmd;
    ime_install(app);

    if (wc3_android_platform_init(&g_platform) != 0) {
        LOGE("platform init failed");
    } else {
        GPU_LOG("platform init ok; installing Vulkan driver api");
        wc3_android_vulkan_driver_install(&g_platform.vulkan_driver);
        GPU_LOG("Vulkan driver api installed");
        if (wc3_platform_install(&g_platform) != 0)
            LOGE("platform install failed");
        else
            g_platform_ready = 1;
    }

    const char *data_dir = pick_data_dir(app);
    if (!data_dir && app->activity && app->activity->internalDataPath) {
        snprintf(g_data_dir, sizeof g_data_dir, "%s",
                 app->activity->internalDataPath);
        data_dir = g_data_dir;
    }
    if (!data_dir) {
        snprintf(g_data_dir, sizeof g_data_dir, "%s", "/data/local/tmp");
        data_dir = g_data_dir;
    }
    const char *state_dir =
        app->activity && app->activity->internalDataPath &&
        app->activity->internalDataPath[0]
            ? app->activity->internalDataPath
            : data_dir;
    snprintf(g_state_dir, sizeof g_state_dir, "%s", state_dir);
    if (!strcmp(g_game_mode, "roc")) {
        char roc_state[1200];
        int n = snprintf(roc_state, sizeof roc_state, "%s/roc", g_state_dir);
        if (n > 0 && (size_t)n < sizeof roc_state) {
            if (mkdir(roc_state, 0770) == 0 || errno == EEXIST)
                snprintf(g_state_dir, sizeof g_state_dir, "%s", roc_state);
            else
                LOGE("cannot create RoC state dir %s: %s", roc_state,
                     strerror(errno));
        }
    }
    LOGI("game mode %s", g_game_mode);
    LOGI("data dir %s", data_dir);
    LOGI("state dir %s", g_state_dir);
    canvas_attach_provider(app);
    if (g_platform_ready) haptics_init(app);
    if (g_platform_ready) {
        char config_path[512];
        int n = snprintf(config_path, sizeof config_path, "%s/wc3.cfg",
                         g_state_dir);
        if (n > 0 && (size_t)n < sizeof config_path)
            g_config_ready = wc3_android_config_load(config_path) == 0;
        if (!g_config_ready) LOGE("config load failed: %s", config_path);
    }
    if (g_platform_ready && g_platform.asset.init) {
        if (g_platform.asset.init(g_platform.asset.ctx, data_dir,
                                  g_state_dir) == 0)
            g_asset_root_ready = 1;
        else
            LOGE("asset roots missing: %s", data_dir);
    }
    inspect_assets();
    /* The static-arena page map is mapping-independent.  Build it before the
     * guest threads exist so the shim fast path needs no lazy init race. */
    wc3_guest_static_init();

#if WC3_WITH_ASBUILT_HLE
    /* The boot path owns the fixed guest mappings and maps the PE images at
     * the exact bases the lifted corpus was generated for.  Do not initialize
     * the new guest-memory manager here: two owners of [0, 4 GiB) would make
     * the second map fail. */
    {
        char probe[1200];
        int n = snprintf(probe, sizeof probe, "%s/war3.exe", data_dir);
        g_war3_mapped = n > 0 && (size_t)n < sizeof probe &&
                        access(probe, R_OK) == 0;
        n = snprintf(probe, sizeof probe, "%s/game.dll", data_dir);
        g_game_mapped = n > 0 && (size_t)n < sizeof probe &&
                        access(probe, R_OK) == 0;
    }
#else
    if (g_platform_ready && wc3_guest_memory_init(&g_platform) == 0) {
        g_guest_ready = 1;
        g_war3_mapped =
            load_image(data_dir, "war3.exe", GUEST_IMAGE_WAR3) == 0;
        g_game_mapped =
            load_image(data_dir, "game.dll", GUEST_IMAGE_GAME) == 0;
    } else {
        LOGE("guest memory init failed");
    }
#endif

    {
        char assets[384];
        if (!g_platform_ready) {
            (void)snprintf(assets, sizeof assets, "unavailable");
        } else if (!g_asset_root_ready) {
            (void)snprintf(assets, sizeof assets, "root missing");
        } else if (g_asset_missing_count) {
            (void)snprintf(assets, sizeof assets, "missing %d: %s",
                           g_asset_missing_count, g_asset_missing);
        } else {
            (void)snprintf(assets, sizeof assets, "ready");
        }

        const char *memory_state =
#if WC3_WITH_ASBUILT_HLE
            "boot-owned";
#else
            g_guest_ready ? "ready" : "failed";
#endif
        const char *corpus_state =
#if WC3_WITH_ASBUILT_HLE
            "linked";
#else
            "missing";
#endif
        const char *game_state =
#if WC3_WITH_ASBUILT_HLE
            (g_war3_mapped && g_game_mapped) ? "starting on window"
                                             : "waiting for assets";
#else
            "not started";
#endif
        char diagnostics[1024];
        (void)snprintf(
            diagnostics, sizeof diagnostics,
            "WC3 recomp diagnostic\n"
            "platform: %s\n"
            "guest memory: %s\n"
            "config: %s\n"
            "input: %s\n"
            "assets: %s\n"
            "war3.exe: %s\n"
            "game.dll: %s\n"
            "imports: guest boot\n"
            "corpus: %s\n"
            "HLE: %s\n"
            "dispatcher: %s\n"
            "game: %s",
            g_platform_ready ? "ready" : "failed", memory_state,
            g_config_ready ? "ready" : "failed",
            (g_platform_ready && g_platform.input.push) ? "ready" : "missing",
            assets, g_war3_mapped ? "found" : "missing",
            g_game_mapped ? "found" : "missing", corpus_state, corpus_state,
            corpus_state, game_state);
        set_activity_text(app, diagnostics);
    }

    while (!app->destroyRequested) {
        int events = 0;
        struct android_poll_source *source = NULL;
        int result = ALooper_pollOnce(-1, NULL, &events, (void **)&source);
        if (result >= 0 && source) source->process(app, source);
        drain_input(app);
    }
    overlay_input_stop();
    overlay_stop();
    cache_stop();
    canvas_detach_provider(app);
    ime_shutdown(app);
    LOGI("android_main exit platform=%d guest=%d", g_platform_ready,
         g_guest_ready);
}
