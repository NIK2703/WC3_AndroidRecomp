/*
 * vulkan_driver_android.h - app-layer ICD selection for the Android product.
 *
 * The platform table owns the policy (system by default, one installed ADPkg
 * package as an explicit choice).  The loader and the DXVK entry point only
 * see this small interface; nothing here reaches the guest ABI.
 */
#ifndef WC3_VULKAN_DRIVER_ANDROID_H
#define WC3_VULKAN_DRIVER_ANDROID_H

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install the five VulkanDriverApi entries.  Called by android_main after
 * wc3_android_platform_init() and before wc3_platform_install(). */
void wc3_android_vulkan_driver_install(Wc3VulkanDriverApi *api);

/* Latch the app-private registry root, the selected package id (empty means
 * system) and the directory that contains libmain_hook.so/libhook_impl.so.
 * Called from nativeApplyLaunchSettings before the game thread starts.
 * A second call after an ICD was opened is refused. */
int wc3_android_vulkan_driver_configure(const char *root, const char *id,
                                        const char *hook_dir);

#ifdef __cplusplus
}
#endif

#endif /* WC3_VULKAN_DRIVER_ANDROID_H */
