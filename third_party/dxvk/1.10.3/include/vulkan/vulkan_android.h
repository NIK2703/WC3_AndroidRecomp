#ifndef VULKAN_ANDROID_H_
#define VULKAN_ANDROID_H_ 1

/*
** Copyright 2015-2021 The Khronos Group Inc.
**
** SPDX-License-Identifier: Apache-2.0
*/

/*
** This header is generated from the Khronos Vulkan XML API Registry.
**
** The tree carries vulkan_win32.h for the Windows host but shipped without
** this file, so VK_USE_PLATFORM_ANDROID_KHR could not be used.  It is the
** unmodified Khronos header (spec version 6) so that the Android build uses
** the same `#ifdef VK_USE_PLATFORM_*` pattern as the Windows one.
*/

#ifdef __cplusplus
extern "C" {
#endif

#include <android/native_window.h>

#ifndef VK_KHR_android_surface
#define VK_KHR_android_surface 1
#define VK_KHR_ANDROID_SURFACE_SPEC_VERSION 6
#define VK_KHR_ANDROID_SURFACE_EXTENSION_NAME "VK_KHR_android_surface"
typedef VkFlags VkAndroidSurfaceCreateFlagsKHR;
typedef struct VkAndroidSurfaceCreateInfoKHR {
    VkStructureType                   sType;
    const void*                       pNext;
    VkAndroidSurfaceCreateFlagsKHR    flags;
    ANativeWindow*                    window;
} VkAndroidSurfaceCreateInfoKHR;

typedef VkResult (VKAPI_PTR *PFN_vkCreateAndroidSurfaceKHR)(VkInstance instance, const VkAndroidSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface);

#ifndef VK_NO_PROTOTYPES
VKAPI_ATTR VkResult VKAPI_CALL vkCreateAndroidSurfaceKHR(
    VkInstance                                  instance,
    const VkAndroidSurfaceCreateInfoKHR*        pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSurfaceKHR*                               pSurface);
#endif
#endif /* VK_KHR_android_surface */

#ifdef __cplusplus
}
#endif

#endif
