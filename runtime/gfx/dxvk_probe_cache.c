/*
 * dxvk_probe_cache.c - memoise the physical-device Vulkan probe.
 *
 * The as-built wc3_vulkan_major_minor() creates a throw-away VkInstance and
 * asks the physical device for its apiVersion.  That is the correct GB-3
 * selector, but the answer cannot change during one process lifetime.  Link
 * this file with --wrap=wc3_vulkan_major_minor so every caller sees the same
 * answer without paying for a second instance creation.
 *
 * This does not change which probe runs and does not use the loader version.
 * The first call still goes through the as-built physical-device path; only
 * repeated calls are served from this cache.
 */
#include <pthread.h>

#include "wc3_debug.h"

#if WC3_DEBUG && WC3_DEBUG_GPU_DRIVERS
#include <android/log.h>
#define PROBE_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "GPU_DRIVERS", __VA_ARGS__)
#else
#define PROBE_LOG(...) ((void)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Supplied by the linker's --wrap rewriting. */
extern int __real_wc3_vulkan_major_minor(int *major, int *minor);

int __wrap_wc3_vulkan_major_minor(int *major, int *minor);

static pthread_once_t g_probe_once = PTHREAD_ONCE_INIT;
static int g_probe_done;
static int g_probe_result;
static int g_probe_major;
static int g_probe_minor;

static void run_probe_once(void)
{
    int major = 0;
    int minor = 0;

    g_probe_result = __real_wc3_vulkan_major_minor(&major, &minor);
    g_probe_major = major;
    g_probe_minor = minor;
    g_probe_done = 1;
    PROBE_LOG("probe result=%d major=%d minor=%d", g_probe_result,
              g_probe_major, g_probe_minor);
}

int __wrap_wc3_vulkan_major_minor(int *major, int *minor)
{
    (void)pthread_once(&g_probe_once, run_probe_once);

    /* The as-built function always writes both outputs, including on failure.
     * Keep that contract even when the caller passes a null pointer here. */
    if (major) *major = g_probe_done ? g_probe_major : 0;
    if (minor) *minor = g_probe_done ? g_probe_minor : 0;
    return g_probe_done ? g_probe_result : 0;
}

#ifdef __cplusplus
}
#endif
