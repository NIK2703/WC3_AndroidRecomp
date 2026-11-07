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
#include <time.h>
#include <unistd.h>

#include "app_boot.h"
#include "controls.h"
#include "guest_layout.h"
#include "guest_memory.h"
#include "haptics_android.h"
#include "input_adapter.h"
#include "input_ring.h"
#include "input_wake.h"
#include "legacy_shim.h"
#include "overlay.h"
#include "overlay_android.h"
#include "pe.h"
#include "platform.h"
#include "platform_android.h"
#include "wc3_debug.h"

#if WC3_WITH_LEGACY_BOOT
#include "overlay_actions_legacy.h"
#endif

#ifndef WC3_DEBUG
#define WC3_DEBUG 0
#endif

#ifndef WC3_WITH_LEGACY_BOOT
#define WC3_WITH_LEGACY_BOOT 0
#endif

#if WC3_DEBUG
#define TAG "wc3host"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
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

static pthread_mutex_t g_window_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *g_window;
static uint32_t g_surface_w;
static uint32_t g_surface_h;
static int g_boot_started;
static pthread_t g_boot_thread;

/* Implemented by the DXVK Win32 shim.  wc3_android_set_window is declared by
 * app_boot.h; the clear call belongs to the same shim and is used when Android
 * destroys the surface. */
void wc3_android_clear_window(ANativeWindow *window);

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

static const char *pick_data_dir(struct android_app *app)
{
    if (!app || !app->activity)
        return NULL;
    const char *candidates[2] = {
        app->activity->internalDataPath,
        app->activity->externalDataPath,
    };
    for (int i = 0; i < 2; i++) {
        if (!candidates[i] || !candidates[i][0])
            continue;
        char probe[1200];
        int n = snprintf(probe, sizeof probe, "%s/war3.exe", candidates[i]);
        if (n <= 0 || (size_t)n >= sizeof probe)
            continue;
        if (access(probe, R_OK) != 0)
            continue;
        n = snprintf(probe, sizeof probe, "%s/war3.mpq", candidates[i]);
        if (n <= 0 || (size_t)n >= sizeof probe)
            continue;
        if (access(probe, R_OK) != 0)
            continue;
        snprintf(g_data_dir, sizeof g_data_dir, "%s", candidates[i]);
        return g_data_dir;
    }
    return NULL;
}

static void *boot_thread(void *arg)
{
    (void)arg;
    ANativeWindow *window = NULL;
    uint32_t width = 0;
    uint32_t height = 0;

    /* GameActivity can replace the starting surface once the activity is laid
     * out.  Wait for a pointer that survives one settle interval rather than
     * publishing a surface that is about to be destroyed. */
    for (int attempt = 0; attempt < 8 && !window; attempt++) {
        usleep(300000);
        pthread_mutex_lock(&g_window_lock);
        window = g_window;
        width = g_surface_w;
        height = g_surface_h;
        pthread_mutex_unlock(&g_window_lock);
    }
    if (!window || !width || !height) {
        LOGE("no stable ANativeWindow for the guest");
        return NULL;
    }

    int rc = wc3_app_boot_start(g_data_dir, window, width, height);
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
            pthread_mutex_unlock(&g_window_lock);
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
#if WC3_WITH_LEGACY_BOOT
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
    case APP_CMD_TERM_WINDOW:
        pthread_mutex_lock(&g_window_lock);
        g_window = NULL;
        pthread_mutex_unlock(&g_window_lock);
        if (app->window)
            wc3_android_clear_window(app->window);
        wc3_android_surface_set_window(NULL);
        LOGI("window terminated");
        break;
    case APP_CMD_WINDOW_RESIZED:
        wc3_android_surface_set_window(app->window);
        if (app->window) {
            uint32_t width = (uint32_t)ANativeWindow_getWidth(app->window);
            uint32_t height = (uint32_t)ANativeWindow_getHeight(app->window);
            pthread_mutex_lock(&g_window_lock);
            g_window = app->window;
            g_surface_w = width;
            g_surface_h = height;
            pthread_mutex_unlock(&g_window_lock);
            const Wc3Platform *platform = wc3_platform();
            if (platform && platform->surface.resize) {
                platform->surface.resize(platform->surface.ctx, width, height);
            }
            LOGI("window resized %ux%u", width, height);
        }
        break;
    case APP_CMD_DESTROY:
        pthread_mutex_lock(&g_window_lock);
        g_window = NULL;
        pthread_mutex_unlock(&g_window_lock);
        if (app->window)
            wc3_android_clear_window(app->window);
        wc3_android_surface_set_window(NULL);
        LOGI("destroy requested");
        break;
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
    Wc3InputEvent event;
    memset(&event, 0, sizeof event);
    event.kind = kind;
    event.pointer_id = (uint32_t)pointer->id;
    event.x = pointer_coord(GameActivityPointerAxes_getX(pointer));
    event.y = pointer_coord(GameActivityPointerAxes_getY(pointer));
    event.modifiers = modifiers;
    event.time_ns = time_ns;
    push_input(&event);

#if WC3_WITH_LEGACY_BOOT
    /* The game only consumes one virtual cursor.  The touch fraction of the
     * physical surface is the same fraction of DXVK's scaled client image, so
     * no display-mode conversion is needed here. */
    if (pointer->id == 0) {
        float x = GameActivityPointerAxes_getX(pointer);
        float y = GameActivityPointerAxes_getY(pointer);
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

static void process_motion(const GameActivityMotionEvent *motion)
{
    if (!motion || motion->pointerCount == 0) return;

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

#if !WC3_WITH_LEGACY_BOOT
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
    LOGI("android_main pid=%d", (int)getpid());
    app->onAppCmd = on_app_cmd;

    if (wc3_android_platform_init(&g_platform) != 0) {
        LOGE("platform init failed");
    } else if (wc3_platform_install(&g_platform) != 0) {
        LOGE("platform install failed");
    } else {
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
    LOGI("data dir %s", data_dir);
    if (g_platform_ready) {
        char config_path[512];
        int n = snprintf(config_path, sizeof config_path, "%s/wc3.cfg",
                         data_dir);
        if (n > 0 && (size_t)n < sizeof config_path)
            g_config_ready = wc3_android_config_load(config_path) == 0;
        if (!g_config_ready) LOGE("config load failed: %s", config_path);
    }
    if (g_platform_ready && g_platform.asset.init) {
        if (g_platform.asset.init(g_platform.asset.ctx, data_dir, data_dir) == 0)
            g_asset_root_ready = 1;
        else
            LOGE("asset roots missing: %s", data_dir);
    }
    inspect_assets();

#if WC3_WITH_LEGACY_BOOT
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
#if WC3_WITH_LEGACY_BOOT
            "boot-owned";
#else
            g_guest_ready ? "ready" : "failed";
#endif
        const char *corpus_state =
#if WC3_WITH_LEGACY_BOOT
            "linked";
#else
            "missing";
#endif
        const char *game_state =
#if WC3_WITH_LEGACY_BOOT
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
    LOGI("android_main exit platform=%d guest=%d", g_platform_ready,
         g_guest_ready);
}
