/*
 * app_boot.c - publish the Android window, then enter the lifted guest.
 *
 * The old NativeActivity front end and the shell probe share one guest entry
 * point (wc3_boot_probe_main).  This file is the GameActivity equivalent of
 * the NativeActivity front end: it does not map PEs or implement Win32, it
 * only gives DXVK the real surface and chooses the device-relative display
 * mode before the guest creates its device.
 */
#include "app_boot.h"

#include <android/log.h>
#include <android/native_window.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "init_array_probe.h"
#include "hle_bridge.h"
#include "present_target.h"
#include "wc3_crash.h"
#include "wc3_debug.h"
#include "wc3_trace.h"

#ifndef WC3_DEBUG
#define WC3_DEBUG 0
#endif

#if WC3_DEBUG
#define TAG "wc3boot"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#endif

/* Present-scaling trace.  Kept on the graphics tag: the scaling decision and
 * the graphics path are the same investigation, and the debug APK already
 * enables GRAPHICS. */
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
#define SCALE_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "wc3present", __VA_ARGS__)
#else
#define SCALE_LOG(...) ((void)0)
#endif

/* Implemented by runtime/hle/android/boot_probe.c. */
int wc3_boot_probe_main(int argc, char **argv);

/* Implemented by the DXVK Win32 shim (dxvk_win32.cpp). */
void wc3_android_set_window(ANativeWindow *window, uint32_t width,
                            uint32_t height);
int wc3_android_set_surface_size(uint32_t width, uint32_t height);

static void set_default(const char *name, const char *value)
{
    if (!getenv(name))
        setenv(name, value, 1);
}

/* The boot log lives in the app-private state directory while the game runs.
 * Both stdout and stderr go to the variant-tagged file so reports from
 * different builds stay separate; the host copies it to Download when the game
 * exits. */

static int open_log(const char *path)
{
    FILE *f = freopen(path, "w", stdout);
    if (!f)
        return -1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    f = freopen(path, "a", stderr);
    if (!f)
        return -1;
    setvbuf(stderr, NULL, _IOLBF, 0);
    wc3_crash_set_log_path(path);
    LOGI("boot log %s", path);
    return 0;
}

static void redirect_output(const char *state_dir)
{
    char path[1024];
    int n;

    n = snprintf(path, sizeof path, "%s/" WC3_CRASH_LOG_FILE, state_dir);
    if (n <= 0 || (size_t)n >= sizeof path)
        return;
    open_log(path);
}

#if WC3_DEBUG && WC3_DEBUG_HLE
/* On-device diagnostics: SIGUSR1 dumps the HLE last-call ring and the last
 * dispatch targets into the boot log without stopping the guest.  The crash
 * handler already calls the same reporter from a signal context. */
extern void hle_android_report(void);

static void report_signal(int sig)
{
    (void)sig;
    hle_android_report();
}

static void install_report_signal(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = report_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
}

/* Some Android runtimes keep SIGUSR1 blocked on the main thread, so a file
 * trigger is the reliable path.  Creating <state>/hle_report_on makes this
 * thread print a report every ten seconds; the file can be removed without
 * restarting the app. */
static char g_report_dir[1024];

static void *hle_report_thread(void *unused)
{
    (void)unused;
    char path[1200];
    int n = snprintf(path, sizeof path, "%s/hle_report_on", g_report_dir);
    if (n <= 0 || (size_t)n >= sizeof path)
        return NULL;
    for (;;) {
        sleep(10);
        if (access(path, F_OK) == 0) {
            hle_android_report();
            sleep(2);
        }
    }
    return NULL;
}

static void start_report_thread(const char *state_dir)
{
    snprintf(g_report_dir, sizeof g_report_dir, "%s", state_dir);
    pthread_t t;
    if (pthread_create(&t, NULL, hle_report_thread, NULL) == 0)
        pthread_detach(t);
}
#else
static void install_report_signal(void) {}
static void start_report_thread(const char *state_dir) { (void)state_dir; }
#endif

/* The Windows Reign of Chaos launcher starts war3.exe with -classic; the
 * Frozen Throne launcher starts it with no game switch.  Keep the switch in
 * the product boot layer, before the guest command line is assembled, so the
 * as-built HLE source does not need another edit. */
static void set_game_args(int roc)
{
    if (!roc)
        return;
    const char *existing = getenv("WC3_ARGS");
    if (!existing || !*existing) {
        setenv("WC3_ARGS", "-classic", 1);
        return;
    }
    if (strstr(existing, "-classic"))
        return;
    char merged[512];
    int n = snprintf(merged, sizeof merged, "-classic %s", existing);
    if (n > 0 && (size_t)n < sizeof merged)
        setenv("WC3_ARGS", merged, 1);
}

/* The APK has no shell to pass diagnostic environment variables.  A marker
 * file in the app-private state directory enables the corresponding oracle
 * before the guest starts; this is the same file-trigger idea as
 * hle_report_on and is compiled out of release.  The marker only turns a
 * verification on, never changes product behavior by default. */
#if WC3_DEBUG && WC3_DEBUG_BOOT
static void apply_debug_verify_markers(const char *state_dir)
{
    static const struct {
        const char *file;
        const char *env;
    } markers[] = {
        { "wc3_vertex_verify", "WC3_VERTEX_VERIFY" },
        { "wc3_triangle_verify", "WC3_TRIANGLE_VERIFY" },
        { "wc3_ray_verify", "WC3_RAY_VERIFY" },
    };
    for (size_t i = 0; i < sizeof markers / sizeof markers[0]; i++) {
        char path[1200];
        int n = snprintf(path, sizeof path, "%s/%s", state_dir,
                         markers[i].file);
        if (n > 0 && (size_t)n < sizeof path && access(path, F_OK) == 0)
            setenv(markers[i].env, "1", 1);
    }
}

static int debug_env_key_ok(const char *key)
{
    if (strncmp(key, "WC3_", 4) != 0 || key[4] == '\0')
        return 0;
    for (; *key; key++) {
        char c = *key;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

/* Generic debug-only environment input.  A marker per A/B switch would need
 * one file and one code path per switch; this file lets a run set any WC3_*
 * variable before the guest and the pool start.  Values are never logged, the
 * file is app-private, and release compiles the whole path out.  The verify
 * markers above run first, so this file can explicitly turn one off. */
static void apply_debug_env_file(const char *state_dir)
{
    char path[1200];
    int n = snprintf(path, sizeof path, "%s/wc3_debug_env", state_dir);
    if (n <= 0 || (size_t)n >= sizeof path)
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    unsigned applied = 0;
    while (fgets(line, sizeof line, f)) {
        char *key = line;
        while (*key == ' ' || *key == '\t')
            key++;
        if (*key == '#' || *key == '\n' || *key == '\r' || *key == '\0')
            continue;
        char *eq = strchr(key, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *value = eq + 1;
        while (*value == ' ' || *value == '\t')
            value++;
        char *end = key + strlen(key);
        while (end > key && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        end = value + strlen(value);
        while (end > value && (end[-1] == '\n' || end[-1] == '\r' ||
                               end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        if (!debug_env_key_ok(key))
            continue;
        if (setenv(key, value, 1) == 0)
            applied++;
    }
    fclose(f);
    LOGI("debug env: %u WC3_* variable(s) from %s", applied, path);
}
#endif

/* A separate small marker is reliable even though the boot log is opened
 * twice (stdout truncates, stderr appends) and the two streams can overwrite
 * each other at boot.  It is also the release-visible acceptance signal. */
static void write_game_mode_marker(const char *state_dir, int roc)
{
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/game_mode.txt", state_dir);
    if (n <= 0 || (size_t)n >= sizeof path)
        return;
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    const char *args = getenv("WC3_ARGS");
    fprintf(f, "mode=%s\nargs=%s\n", roc ? "roc" : "tft", args ? args : "");
    fclose(f);
}

int wc3_app_boot_start(const char *data_dir, const char *state_dir,
                       ANativeWindow *window, uint32_t width, uint32_t height,
                       const char *game_mode)
{
    if (!data_dir || !*data_dir || !state_dir || !*state_dir || !window ||
        !width || !height)
        return -EINVAL;
#if WC3_DEBUG && WC3_DEBUG_BOOT
    LOGI("init_array: %u entries (probe included)", wc3_init_array_count());
#endif
    const int roc = game_mode && !strcmp(game_mode, "roc");
    /* The shared game directory is also the guest's working directory.  Let
     * the game create saves, replays and logs without inheriting a restrictive
     * umask; Android's FUSE may still present 0660. */
    umask(0);
    if (chdir(data_dir) != 0) {
        LOGE("cannot enter data dir %s: %s", data_dir, strerror(errno));
        return -errno;
    }

    redirect_output(state_dir);
    install_report_signal();
    start_report_thread(state_dir);
    set_game_args(roc);
    write_game_mode_marker(state_dir, roc);
#if WC3_DEBUG && WC3_DEBUG_BOOT
    apply_debug_verify_markers(state_dir);
    apply_debug_env_file(state_dir);
#endif

    /* These are the product defaults, not diagnostics: the shipped APK has no
     * shell to pass arguments and must load the game and use DXVK.  Sound is
     * part of the product: never seed -nosound here.  A diagnostic run can
     * still opt out through WC3_ARGS or WC3_NOSOUND=1. */
    set_default("WC3_DATA_DIR", state_dir);
    set_default("WC3_LOAD_GAME", "1");
    set_default("WC3_D3D9", "dxvk");
    set_default("DXVK_STATE_CACHE", "0");
    set_default("WC3_HIDDEN", "1");
    set_default("WC3_VULKAN_DRIVER_DIR", data_dir);

    /* Half-resolution grid rendering: the game renders at half the surface and
     * the present pass upscales it 2x with nearest sampling, which is what the
     * grid mask is drawn against.  "off"/"system" (the default) leaves the
     * default mode at native and lets the compositor scale anything smaller. */
    const char *present_scale = getenv("WC3_PRESENT_SCALE");
    const int half_resolution =
        present_scale && (!strcmp(present_scale, "half") ||
                          !strcmp(present_scale, "1"));
    wc3_legacy_set_half_resolution(half_resolution);
    /* Publish the policy before any screen-size/select call can create the
     * target: the plan must see half mode from its first ensure, not only from
     * the explicit call below. */
    wc3_present_target_set_half_grid(half_resolution);

    wc3_legacy_set_screen_size(width, height);

    uint32_t guest_w = width;
    uint32_t guest_h = height;
    if (!wc3_legacy_default_display_mode(&guest_w, &guest_h)) {
        guest_w = width;
        guest_h = height;
    }
    /* The launch resolution is a normal system-mode choice.  In half mode the
     * guest extent is fixed by the exact half of the surface; applying a saved
     * native/lower launch resolution here would silently defeat the grid and
     * send a non-2x extent into the present plan. */
    if (!half_resolution) {
        const char *e = getenv("WC3_DXVK_WIDTH");
        if (e && atoi(e) > 0)
            guest_w = (uint32_t)atoi(e);
        e = getenv("WC3_DXVK_HEIGHT");
        if (e && atoi(e) > 0)
            guest_h = (uint32_t)atoi(e);
        if ((guest_w != width || guest_h != height) &&
            !display_mode_supported(guest_w, guest_h)) {
            /* The settings screen keeps a resolution from an older device or
             * display size.  Applying it as the guest mode is only meaningful
             * if the current surface mode table still contains that pair. */
            LOGE("launch resolution %ux%u is not in the device mode table; "
                 "using native %ux%u", guest_w, guest_h, width, height);
            guest_w = width;
            guest_h = height;
        }
    }

    /* The activity can publish the surface before this boot path, and at that
     * moment the half flag was not set yet, so the earlier screen-size update
     * may have selected the native default.  Apply the final launch mode
     * unconditionally, then read it back: select is idempotent and also
     * refreshes the guest client rectangle the shim reports to DXVK. */
    wc3_legacy_select_display_mode(guest_w, guest_h);
    uint32_t applied_w = guest_w, applied_h = guest_h;
    if (wc3_legacy_current_display_mode(&applied_w, &applied_h)) {
        guest_w = applied_w;
        guest_h = applied_h;
    }

    /* Publish the real window before ensure(): the direct grid-off path sets
     * its geometry on this window, and the half-grid path publishes its reader
     * against it.  ensure() then selects pass, direct, or native. */
    WC3_TRACE_BEGIN("wc3.window.publish");
    wc3_android_set_window(window, width, height);
    if (wc3_android_set_surface_size(width, height) != 0)
        LOGE("setBuffersGeometry(%ux%u) failed", width, height);
    WC3_TRACE_END();

    /* Half-grid needs the reader and exact 2x pass.  Grid off below native lets
     * DXVK present into the real window and the compositor scale.  Native
     * retires both.  Failure falls back to DXVK's ordinary stretched path. */
    if (wc3_present_target_ensure(guest_w, guest_h, width, height) != 0) {
        LOGE("present target unavailable for %ux%u; DXVK will scale into the "
             "window", guest_w, guest_h);
    }

    SCALE_LOG("boot: window=%ux%u present_scale=%s half=%d guest=%ux%u "
              "applied=%ux%u path=%s halfGrid=%d",
              width, height, present_scale ? present_scale : "(unset)",
              half_resolution, guest_w, guest_h, applied_w, applied_h,
              wc3_present_target_active() ? "pass"
                                          : (wc3_present_target_direct()
                                                 ? "direct"
                                                 : "native"),
              wc3_present_target_half_grid());
    LOGI("window %ux%u guest %ux%u present=%s", width, height, guest_w, guest_h,
         wc3_present_target_active() ? "target"
                                     : (wc3_present_target_direct() ? "direct"
                                                                    : "native"));

    char exe[1024];
    int n = snprintf(exe, sizeof exe, "%s/war3.exe", data_dir);
    if (n <= 0 || (size_t)n >= sizeof exe)
        return -ENAMETOOLONG;

    char *argv[4];
    argv[0] = (char *)"wc3game";
    argv[1] = exe;
    argv[2] = (char *)data_dir;
    argv[3] = (char *)"Storm.dll";

    WC3_TRACE_BEGIN("wc3.boot.guest");
    int rc = wc3_boot_probe_main(4, argv);
    WC3_TRACE_END();
    LOGI("guest returned %d", rc);

    /* This thread owns the guest entry stack.  If the launcher returns after
     * spawning GameMain on a guest thread, keeping this thread parked keeps
     * the process (and the last presented frame) alive until Android destroys
     * the activity. */
    for (;;)
        pause();
}
