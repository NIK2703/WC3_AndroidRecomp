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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "legacy_shim.h"

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

/* Implemented by runtime/legacy/boot_probe.c. */
int wc3_boot_probe_main(int argc, char **argv);

/* Implemented by the DXVK Win32 shim (dxvk_win32.cpp). */
void wc3_android_set_window(ANativeWindow *window, uint32_t width,
                            uint32_t height);

static void set_default(const char *name, const char *value)
{
    if (!getenv(name))
        setenv(name, value, 1);
}

static void redirect_output(const char *state_dir)
{
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/run.txt", state_dir);
    if (n <= 0 || (size_t)n >= sizeof path)
        return;
    FILE *f = freopen(path, "w", stdout);
    if (f)
        setvbuf(stdout, NULL, _IOLBF, 0);
    f = freopen(path, "a", stderr);
    if (f)
        setvbuf(stderr, NULL, _IOLBF, 0);
}

/* The Windows Reign of Chaos launcher starts war3.exe with -classic; the
 * Frozen Throne launcher starts it with no game switch.  Keep the switch in
 * the product boot layer, before the guest command line is assembled, so the
 * quarantined legacy source does not need another edit. */
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

/* A separate small marker is reliable even though run.txt is opened twice
 * (stdout truncates, stderr appends) and the two streams can overwrite each
 * other at boot.  It is also the release-visible acceptance signal. */
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
    set_game_args(roc);
    write_game_mode_marker(state_dir, roc);

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

    wc3_legacy_set_screen_size(width, height);

    uint32_t guest_w = width;
    uint32_t guest_h = height;
    if (!wc3_legacy_default_display_mode(&guest_w, &guest_h)) {
        guest_w = width;
        guest_h = height;
    }
    const char *e = getenv("WC3_DXVK_WIDTH");
    if (e && atoi(e) > 0)
        guest_w = (uint32_t)atoi(e);
    e = getenv("WC3_DXVK_HEIGHT");
    if (e && atoi(e) > 0)
        guest_h = (uint32_t)atoi(e);
    if (guest_w != width || guest_h != height)
        wc3_legacy_select_display_mode(guest_w, guest_h);

    /* Keep the native buffer at the physical surface size; DXVK scales the
     * selected back buffer to it.  Publishing the window before the guest
     * creates its device is what makes DXVK's Android WSI create a surface. */
    if (ANativeWindow_setBuffersGeometry(window, (int32_t)width,
                                         (int32_t)height,
                                         WINDOW_FORMAT_RGBA_8888) != 0)
        LOGE("setBuffersGeometry(%ux%u) failed", width, height);
    wc3_android_set_window(window, width, height);
    LOGI("window %ux%u guest %ux%u", width, height, guest_w, guest_h);

    char exe[1024];
    int n = snprintf(exe, sizeof exe, "%s/war3.exe", data_dir);
    if (n <= 0 || (size_t)n >= sizeof exe)
        return -ENAMETOOLONG;

    char *argv[4];
    argv[0] = (char *)"wc3game";
    argv[1] = exe;
    argv[2] = (char *)data_dir;
    argv[3] = (char *)"Storm.dll";

    int rc = wc3_boot_probe_main(4, argv);
    LOGI("guest returned %d", rc);

    /* This thread owns the guest entry stack.  If the launcher returns after
     * spawning GameMain on a guest thread, keeping this thread parked keeps
     * the process (and the last presented frame) alive until Android destroys
     * the activity. */
    for (;;)
        pause();
}
