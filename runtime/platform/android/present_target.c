#include "present_target.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <errno.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "present_target_policy.h"
#include "wc3_debug.h"

#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
#define PT_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "wc3present", __VA_ARGS__)
#else
#define PT_LOG(...) ((void)0)
#endif

/*
 * DXVK keeps `caps.minImageCount + 1` swap chain images (pickImageCount in
 * vulkan_presenter.cpp), and the producer side of this reader must be able to
 * dequeue that many at once or vkCreateSwapchainKHR deadlocks in dequeueBuffer.
 * The device reports minImageCount=4 for the real window, so 8 leaves room for
 * a driver that reports more, and costs 8 * 2.6 MB in half mode.
 */
#define PT_MAX_IMAGES 8

/* Bounded drain: a frame queues one image, so a handful covers a backlog after
 * a pause.  The loop exists to keep the call O(1) if a driver misbehaves. */
#define PT_DRAIN_LIMIT 6

/* Published to the DXVK Win32 shim (third_party/dxvk/shim/dxvk_win32.cpp). */
extern void wc3_android_set_present_target(ANativeWindow *window,
                                           uint32_t width, uint32_t height);

/* Readers that were unpublished while DXVK might still own a swap chain on
 * their window.  They are destroyed only by reap(), after that swap chain is
 * gone.  Four slots cover every realistic start/retire sequence in one
 * session; if the array is full the reader is leaked on purpose, because a
 * use-after-free here is worse than a few megabytes until process exit. */
#define PT_MAX_RETIRED 4

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static AImageReader *g_reader;
static ANativeWindow *g_window;
static AImageReader *g_retired_readers[PT_MAX_RETIRED];
static ANativeWindow *g_retired_windows[PT_MAX_RETIRED];
static _Atomic uint32_t g_width;
static _Atomic uint32_t g_height;
/* Surface extent last reported by the mode layer.  This is the same value the
 * half-mode choice was derived from and it does not move when the screen swap
 * chain changes the real window's buffer geometry. */
static _Atomic uint32_t g_surface_width;
static _Atomic uint32_t g_surface_height;
static _Atomic int g_active;
static _Atomic int g_half_grid;

/* Queue-buffer geometry.  Best effort on purpose: the reader was already
 * created at the guest extent and the swap chain extent comes from the surface
 * capabilities, so this call only helps a WSI that derives currentExtent from
 * the requested buffer dimensions.  Its return value is logged, not fatal. */
static void pt_apply_geometry(ANativeWindow *window, uint32_t width,
                              uint32_t height)
{
    int rc = ANativeWindow_setBuffersGeometry(window, (int32_t)width,
                                              (int32_t)height,
                                              WINDOW_FORMAT_RGBA_8888);
    if (rc != 0) {
        PT_LOG("geometry %ux%u rejected rc=%d", width, height, rc);
    }
}

static void pt_publish(uint32_t width, uint32_t height)
{
    atomic_store_explicit(&g_width, width, memory_order_relaxed);
    atomic_store_explicit(&g_height, height, memory_order_relaxed);
    wc3_android_set_present_target(g_window, width, height);
    PT_LOG("publish guest=%ux%u window=%p", width, height, (void *)g_window);
}

/* Bounded drain of one reader.  Caller holds g_lock. */
static void pt_drain_reader(AImageReader *reader)
{
    for (int i = 0; reader && i < PT_DRAIN_LIMIT; ++i) {
        AImage *image = NULL;
        if (AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK ||
            !image) {
            break;
        }
        AImage_delete(image);
    }
}

/* Caller holds g_lock. */
static void pt_destroy_reader(AImageReader *reader, ANativeWindow *window)
{
    pt_drain_reader(reader);
    if (reader) AImageReader_delete(reader);
    if (window) ANativeWindow_release(window);
}

/* Caller holds g_lock.  Park a reader whose producer may still feed a live
 * VkSwapchainKHR.  It is destroyed only by reap(). */
static void pt_park_locked(AImageReader *reader, ANativeWindow *window)
{
    if (!reader) return;

    int slot = -1;
    for (int i = 0; i < PT_MAX_RETIRED; ++i) {
        if (!g_retired_readers[i]) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        /* No safe place to park it.  Leaking is deliberate: deleting now can
         * free buffers a live VkSwapchain still references. */
        PT_LOG("retire: no free slot, leaking reader %p", (void *)reader);
        return;
    }

    g_retired_readers[slot] = reader;
    g_retired_windows[slot] = window;
}

/* Caller holds g_lock.  The reader and window move to the retired list; they
 * are not destroyed here. */
static void pt_retire_locked(void)
{
    if (!g_reader) return;

    pt_park_locked(g_reader, g_window);
    g_reader = NULL;
    g_window = NULL;
    atomic_store_explicit(&g_active, 0, memory_order_release);
    wc3_android_set_present_target(NULL, 0, 0);
    atomic_store_explicit(&g_width, 0, memory_order_relaxed);
    atomic_store_explicit(&g_height, 0, memory_order_relaxed);
    PT_LOG("retire: producer unpublished, reader parked");
}

int wc3_present_target_start(uint32_t width, uint32_t height)
{
    if (!width || !height) return -EINVAL;

    PT_LOG("start request %ux%u active=%d", width, height,
           atomic_load_explicit(&g_active, memory_order_relaxed) ? 1 : 0);

    pthread_mutex_lock(&g_lock);

    if (g_reader &&
        atomic_load_explicit(&g_width, memory_order_relaxed) == width &&
        atomic_load_explicit(&g_height, memory_order_relaxed) == height) {
        pt_apply_geometry(g_window, width, height);
        pthread_mutex_unlock(&g_lock);
        PT_LOG("extent %ux%u (already started)", width, height);
        return 0;
    }

    AImageReader *reader = NULL;
    /* RGBA8, not PRIVATE: the producer window is handed to Vulkan's Android
     * WSI, which has to name a VkFormat for the buffers it dequeues.  An
     * implementation-defined private format leaves that naming to the vendor,
     * while RGBA8 maps to VK_FORMAT_R8G8B8A8_UNORM on every driver and is the
     * same 32-bit order the real SurfaceView reports.  AImageReader takes an
     * AHardwareBuffer format, and this is the RGBA8 value. */
    media_status_t status = AImageReader_newWithUsage(
        (int32_t)width, (int32_t)height, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
        PT_MAX_IMAGES, &reader);
    if (status != AMEDIA_OK || !reader) {
        pthread_mutex_unlock(&g_lock);
        PT_LOG("reader create %ux%u failed status=%d", width, height,
               (int)status);
        return status ? -(int)status : -ENOMEM;
    }

    ANativeWindow *window = NULL;
    status = AImageReader_getWindow(reader, &window);
    if (status != AMEDIA_OK || !window) {
        AImageReader_delete(reader);
        pthread_mutex_unlock(&g_lock);
        PT_LOG("reader window failed status=%d", (int)status);
        return status ? -(int)status : -ENODEV;
    }
    /* AImageReader_getWindow returns a new reference; this module owns it. */

    pt_apply_geometry(window, width, height);

    PT_LOG("reader %ux%u window geometry=%dx%d format=%d",
           width, height, ANativeWindow_getWidth(window),
           ANativeWindow_getHeight(window), ANativeWindow_getFormat(window));

    /* A reader's extent is fixed at creation; its producer surface reports
     * that extent to the Vulkan WSI.  Keeping the old reader and only changing
     * its buffer geometry would leave DXVK recreating a swap chain against the
     * old caps, so a new extent gets a new reader.  The old one is parked
     * until DXVK has destroyed the swap chain that references it. */
    pt_park_locked(g_reader, g_window);

    g_reader = reader;
    g_window = window;
    pt_publish(width, height);
    atomic_store_explicit(&g_active, 1, memory_order_release);

#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    int32_t max_images = 0;
    AImageReader_getMaxImages(reader, &max_images);
#endif
    pthread_mutex_unlock(&g_lock);

    PT_LOG("started %ux%u window=%p maxImages=%d", width, height,
           (void *)window, (int)max_images);
    return 0;
}

void wc3_present_target_drain(void)
{
    if (!atomic_load_explicit(&g_active, memory_order_acquire)) return;

    pthread_mutex_lock(&g_lock);
    pt_drain_reader(g_reader);
    pthread_mutex_unlock(&g_lock);
}

int wc3_present_target_set_extent(uint32_t width, uint32_t height)
{
    if (!width || !height) return -EINVAL;
    if (!wc3_present_target_active()) return 0;

    /* The reader cannot be resized in place: replace it and park the old one
     * until DXVK destroys the swap chain that references it. */
    return wc3_present_target_start(width, height);
}

int wc3_present_target_ensure(uint32_t guest_w, uint32_t guest_h,
                              uint32_t surface_w, uint32_t surface_h)
{
    if (!guest_w || !guest_h || !surface_w || !surface_h) return -EINVAL;

    /* Remember the surface extent the mode layer is working against.  The
     * pass must not re-read the live ANativeWindow geometry: after the screen
     * swap chain is created that value can be the swap chain extent, not the
     * SurfaceView.  Using it for the half test would then never see exact
     * half and would silently take the compositor path. */
    atomic_store_explicit(&g_surface_width, surface_w, memory_order_relaxed);
    atomic_store_explicit(&g_surface_height, surface_h, memory_order_relaxed);

    int needed =
        wc3_present_pass_needed(guest_w, guest_h, surface_w, surface_h);

    PT_LOG("ensure guest=%ux%u surface=%ux%u needed=%d active=%d half=%d",
           guest_w, guest_h, surface_w, surface_h, needed,
           wc3_present_target_active(), wc3_present_target_half_grid());
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    /* A/B and regression lever: force the pass even when it is a 1:1 copy, so
     * the old always-on behaviour can be reproduced on a device. */
    const char *force = getenv("WC3_PRESENT_FORCE");
    if (force && *force && strcmp(force, "0") != 0) {
        needed = 1;
        PT_LOG("force pass for %ux%u on %ux%u", guest_w, guest_h, surface_w,
               surface_h);
    }
#endif

    if (needed) return wc3_present_target_start(guest_w, guest_h);

    if (wc3_present_target_active()) {
        /* The mode is back at the surface extent: the pass would only copy 1:1.
         * Unpublish it now, but leave the reader to reap() so a swap chain that
         * is still in flight cannot outlive its buffers. */
        pthread_mutex_lock(&g_lock);
        pt_retire_locked();
        pthread_mutex_unlock(&g_lock);
        PT_LOG("retire for native %ux%u", surface_w, surface_h);
    } else {
        PT_LOG("bypass: guest %ux%u == surface %ux%u", guest_w, guest_h,
               surface_w, surface_h);
    }
    return 0;
}

void wc3_present_target_retire(void)
{
    pthread_mutex_lock(&g_lock);
    pt_retire_locked();
    pthread_mutex_unlock(&g_lock);
    PT_LOG("retired (reader kept for reap)");
}

void wc3_present_target_reap(void)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < PT_MAX_RETIRED; ++i) {
        if (!g_retired_readers[i]) continue;
        AImageReader *reader = g_retired_readers[i];
        ANativeWindow *window = g_retired_windows[i];
        g_retired_readers[i] = NULL;
        g_retired_windows[i] = NULL;
        pt_destroy_reader(reader, window);
        PT_LOG("reaped retired reader %p", (void *)reader);
    }
    pthread_mutex_unlock(&g_lock);
}

void wc3_present_target_stop(void)
{
    pthread_mutex_lock(&g_lock);
    AImageReader *reader = g_reader;
    ANativeWindow *window = g_window;
    g_reader = NULL;
    g_window = NULL;
    atomic_store_explicit(&g_active, 0, memory_order_release);
    wc3_android_set_present_target(NULL, 0, 0);
    atomic_store_explicit(&g_width, 0, memory_order_relaxed);
    atomic_store_explicit(&g_height, 0, memory_order_relaxed);
    if (reader) pt_destroy_reader(reader, window);

    /* Shutdown/fail-safe: DXVK is idle or gone, so retired readers are safe to
     * free here as well. */
    for (int i = 0; i < PT_MAX_RETIRED; ++i) {
        if (!g_retired_readers[i]) continue;
        AImageReader *retired = g_retired_readers[i];
        ANativeWindow *retired_window = g_retired_windows[i];
        g_retired_readers[i] = NULL;
        g_retired_windows[i] = NULL;
        pt_destroy_reader(retired, retired_window);
    }
    pthread_mutex_unlock(&g_lock);

    PT_LOG("stopped");
}

int wc3_present_target_active(void)
{
    return atomic_load_explicit(&g_active, memory_order_acquire) ? 1 : 0;
}

int wc3_present_target_extent(uint32_t *width, uint32_t *height)
{
    if (!atomic_load_explicit(&g_active, memory_order_acquire)) return 0;

    uint32_t w = atomic_load_explicit(&g_width, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&g_height, memory_order_relaxed);
    if (!w || !h) return 0;

    if (width) *width = w;
    if (height) *height = h;
    return 1;
}

int wc3_present_target_surface_extent(uint32_t *width, uint32_t *height)
{
    uint32_t w = atomic_load_explicit(&g_surface_width, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&g_surface_height, memory_order_relaxed);
    if (!w || !h) return 0;

    if (width) *width = w;
    if (height) *height = h;
    return 1;
}

ANativeWindow *wc3_present_target_window(void)
{
    pthread_mutex_lock(&g_lock);
    ANativeWindow *window = g_window;
    pthread_mutex_unlock(&g_lock);
    return window;
}

void wc3_present_target_set_half_grid(int enabled)
{
    atomic_store_explicit(&g_half_grid, enabled ? 1 : 0, memory_order_release);
}

int wc3_present_target_half_grid(void)
{
    return atomic_load_explicit(&g_half_grid, memory_order_acquire) ? 1 : 0;
}
