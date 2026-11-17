/*
 * present_pass.cpp - the guest's resolution on screen, scaled by design.
 *
 * Layout of the problem, in one place:
 *
 *   guest D3D9 back buffer      guest mode      (the game renders here)
 *   DXVK swap chain image       producer size   (guest image at 0,0, 1:1)
 *   our screen swap chain       see below       (grid: surface, else guest)
 *   SurfaceFlinger                              (1:1, or the scaler)
 *
 * DXVK's own blitter is what places the guest image at the origin without
 * filtering: with GetClientRect equal to the guest extent, dstRect.extent ==
 * srcRect.extent, so DxvkSwapchainBlitter::draw takes sameSize -> m_fsCopy
 * (dxvk_swapchain_blitter.cpp:47,51), whose shader is a texelFetch with a
 * destination offset.  This pass only has to move the resulting rectangle.
 *
 * Two shapes, and only two:
 *
 *   half-resolution grid  2x nearest into a surface-sized image.  At a whole
 *                         factor vkCmdBlitImage with VK_FILTER_NEAREST is
 *                         specified as exact texel replication (destination
 *                         pixel i takes source texel floor(i / 2)), which is
 *                         what the grid mask needs and what a texelFetch
 *                         shader would compute - without a SPIR-V toolchain,
 *                         render pass, descriptor set, sampler or pipeline.
 *                         The project has no shader compiler in the app target;
 *                         DXVK builds its own shaders at archive build time.
 *
 *   everything else       a 1:1 copy into an image of the game's own size, and
 *                         the Android compositor scales the layer to the view.
 *                         No filter of ours runs here at all, not even when the
 *                         ratio happens to be whole.
 */
#include "present_pass.h"

#include "platform.h"
#include "present_scale.h"
#include "present_target.h"
#include "wc3_debug.h"

#include <android/log.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
#define PP_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "wc3present", __VA_ARGS__)
#else
#define PP_LOG(...) ((void)0)
#endif

namespace {

/* Frames the pass keeps in flight.  DXVK serialises the guest timeline with
 * its own per-frame semaphores, so two slots are enough and keep the
 * semaphore bookkeeping small. */
constexpr uint32_t kFrames = 2;

/* DXVK passes exactly one wait semaphore today; anything larger is forwarded
 * untouched to the guest present instead of being copied. */
constexpr uint32_t kMaxWaitSemaphores = 4;

struct SurfaceFns {
    PFN_vkCreateAndroidSurfaceKHR createSurface = nullptr;
    PFN_vkDestroySurfaceKHR destroySurface = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR getCaps = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR getFormats = nullptr;
};

struct DeviceFns {
    PFN_vkQueuePresentKHR queuePresent = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkQueueWaitIdle queueWaitIdle = nullptr;
    PFN_vkDeviceWaitIdle deviceWaitIdle = nullptr;
    PFN_vkCreateSwapchainKHR createSwapchain = nullptr;
    PFN_vkDestroySwapchainKHR destroySwapchain = nullptr;
    PFN_vkGetSwapchainImagesKHR getSwapchainImages = nullptr;
    PFN_vkAcquireNextImageKHR acquireNextImage = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer resetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdBlitImage cmdBlitImage = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage = nullptr;
    PFN_vkCreateSemaphore createSemaphore = nullptr;
    PFN_vkDestroySemaphore destroySemaphore = nullptr;
};

PFN_vkGetInstanceProcAddr g_realGipa;
PFN_vkGetDeviceProcAddr g_realGdpa;
VkInstance g_instance;
VkPhysicalDevice g_phys;
VkDevice g_device;
uint32_t g_queueFamily;

SurfaceFns g_surface;
DeviceFns g_dev;

std::atomic<int> g_ready;
/* A guest swap chain was destroyed, but its presenter may still own the
 * VkSurfaceKHR built on the reader's ANativeWindow (DXVK destroys the swap
 * chain before the surface).  Freeing the reader here would race that surface
 * teardown, so reap at the next swap chain creation, which happens after the
 * old presenter, including its surface, is gone. */
bool g_reapPending;

/* DXVK's swap chain.  One at a time: the guest has one device and one swap
 * chain, and every creation goes through our wrapper. */
VkSwapchainKHR g_guestSwapchain;
VkExtent2D g_guestExtent;
VkFormat g_guestFormat;
std::vector<VkImage> g_guestImages;

/* Our swap chain on the real window.  Its extent follows the plan: the surface
 * size for the grid path, the guest size for the compositor path. */
VkSurfaceKHR g_screenSurface;
uintptr_t g_screenWindow;
VkSwapchainKHR g_screenSwapchain;
VkExtent2D g_screenExtent;
VkFormat g_screenFormat;
std::vector<VkImage> g_screenImages;

VkCommandPool g_pool;
VkCommandBuffer g_cmd[kFrames];
VkSemaphore g_imageReady[kFrames];
VkSemaphore g_blitDoneGuest[kFrames];
VkSemaphore g_blitDoneScreen[kFrames];
uint32_t g_frame;

#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
uint32_t g_frameLog;
#endif

VkImageMemoryBarrier image_barrier(VkImage image, VkImageLayout from,
                                   VkImageLayout to, VkAccessFlags srcAccess,
                                   VkAccessFlags dstAccess);

/* Defined in the bootstrap section: finishes the pass on an already-created
 * device, either at device creation or on the first swap chain after a runtime
 * target start.  `checkProducer` is false in the latter case: DXVK has already
 * created its VkSurface on the new reader, and a second temporary surface on
 * the same ANativeWindow is not worth asking the WSI to support. */
bool activate_pass(bool checkProducer);

/* ------------------------------------------------------------ helpers ---- */

const Wc3Platform *platform_or_null()
{
    return wc3_platform();
}

int surface_acquire(const Wc3Platform *platform, uintptr_t *window)
{
    if (!platform || !platform->surface.acquire || !window) return -1;
    *window = 0;
    return platform->surface.acquire(platform->surface.ctx, window);
}

void surface_release(const Wc3Platform *platform, uintptr_t window)
{
    if (platform && platform->surface.release && window)
        platform->surface.release(platform->surface.ctx, window);
}

PFN_vkVoidFunction instance_fn(const char *name)
{
    if (!g_realGipa || !g_instance) return nullptr;
    return g_realGipa(g_instance, name);
}

bool load_surface_fns()
{
    g_surface.createSurface = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
        instance_fn("vkCreateAndroidSurfaceKHR"));
    g_surface.destroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
        instance_fn("vkDestroySurfaceKHR"));
    g_surface.getCaps =
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            instance_fn("vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    g_surface.getFormats =
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
            instance_fn("vkGetPhysicalDeviceSurfaceFormatsKHR"));

    return g_surface.createSurface && g_surface.destroySurface &&
           g_surface.getCaps && g_surface.getFormats;
}

bool load_device_fns()
{
    if (!g_realGdpa || !g_device) return false;

    /* vkQueuePresentKHR has to be loaded here, not only when DXVK asks for it.
     * Activation runs inside vkCreateDevice, before DXVK has queried any device
     * entry points, and the loader used to reject the whole activation because
     * this one pointer was still null.  The result was a permanently dormant
     * target: DXVK created its swap chain on the real window at 2400x1080 and
     * its own blitter stretched the 1200x540 back buffer. */
    g_dev.queuePresent = reinterpret_cast<PFN_vkQueuePresentKHR>(
        g_realGdpa(g_device, "vkQueuePresentKHR"));
    g_dev.queueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(
        g_realGdpa(g_device, "vkQueueSubmit"));
    g_dev.queueWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(
        g_realGdpa(g_device, "vkQueueWaitIdle"));
    g_dev.deviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(
        g_realGdpa(g_device, "vkDeviceWaitIdle"));
    g_dev.createSwapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
        g_realGdpa(g_device, "vkCreateSwapchainKHR"));
    g_dev.destroySwapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
        g_realGdpa(g_device, "vkDestroySwapchainKHR"));
    g_dev.getSwapchainImages = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
        g_realGdpa(g_device, "vkGetSwapchainImagesKHR"));
    g_dev.acquireNextImage = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
        g_realGdpa(g_device, "vkAcquireNextImageKHR"));
    g_dev.createCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(
        g_realGdpa(g_device, "vkCreateCommandPool"));
    g_dev.destroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(
        g_realGdpa(g_device, "vkDestroyCommandPool"));
    g_dev.allocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(
        g_realGdpa(g_device, "vkAllocateCommandBuffers"));
    g_dev.resetCommandBuffer = reinterpret_cast<PFN_vkResetCommandBuffer>(
        g_realGdpa(g_device, "vkResetCommandBuffer"));
    g_dev.beginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(
        g_realGdpa(g_device, "vkBeginCommandBuffer"));
    g_dev.endCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(
        g_realGdpa(g_device, "vkEndCommandBuffer"));
    g_dev.cmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(
        g_realGdpa(g_device, "vkCmdPipelineBarrier"));
    g_dev.cmdBlitImage = reinterpret_cast<PFN_vkCmdBlitImage>(
        g_realGdpa(g_device, "vkCmdBlitImage"));
    g_dev.cmdCopyImage = reinterpret_cast<PFN_vkCmdCopyImage>(
        g_realGdpa(g_device, "vkCmdCopyImage"));
    g_dev.createSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(
        g_realGdpa(g_device, "vkCreateSemaphore"));
    g_dev.destroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(
        g_realGdpa(g_device, "vkDestroySemaphore"));

    return g_dev.queuePresent && g_dev.queueSubmit && g_dev.queueWaitIdle &&
           g_dev.deviceWaitIdle && g_dev.createSwapchain &&
           g_dev.destroySwapchain && g_dev.getSwapchainImages &&
           g_dev.acquireNextImage && g_dev.createCommandPool &&
           g_dev.destroyCommandPool && g_dev.allocateCommandBuffers &&
           g_dev.beginCommandBuffer && g_dev.endCommandBuffer &&
           g_dev.cmdPipelineBarrier && g_dev.cmdBlitImage &&
           g_dev.cmdCopyImage && g_dev.createSemaphore &&
           g_dev.destroySemaphore;
}

/* ------------------------------------------------------------ probing ---- */

/*
 * Ask the driver whether a surface accepts the usage this pass needs on its
 * images.  `producer` selects the stricter test: the producer must be
 * readable (TRANSFER_SRC) and large enough to hold the guest image; the real
 * window only has to be writable (TRANSFER_DST) at the guest size.
 */
bool probe_surface(ANativeWindow *window, uint32_t guestW, uint32_t guestH,
                   bool producer)
{
    if (!window || !g_surface.createSurface || !g_surface.getCaps) return false;

    VkAndroidSurfaceCreateInfoKHR info;
    std::memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    info.window = window;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (g_surface.createSurface(g_instance, &info, nullptr, &surface) !=
        VK_SUCCESS) {
        PP_LOG("probe: surface create failed");
        return false;
    }

    VkSurfaceCapabilitiesKHR caps;
    VkResult capsResult = g_surface.getCaps(g_phys, surface, &caps);
    if (capsResult != VK_SUCCESS) {
        g_surface.destroySurface(g_instance, surface, nullptr);
        PP_LOG("probe: caps failed %d", (int)capsResult);
        return false;
    }

    const VkImageUsageFlags required = producer
                                           ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                           : VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    bool usable = (caps.supportedUsageFlags & required) != 0;

    uint32_t extentW = caps.currentExtent.width;
    uint32_t extentH = caps.currentExtent.height;
    if (usable && producer && extentW != UINT32_MAX &&
        (extentW < guestW || extentH < guestH)) {
        /* The producer reports a size it cannot grow from.  Patch the buffer
         * geometry once and ask again; a WSI that derives currentExtent from
         * the requested buffer size will follow. */
        PP_LOG("probe: producer extent %ux%u < guest %ux%u, regrow", extentW,
               extentH, guestW, guestH);
        ANativeWindow_setBuffersGeometry(window, (int32_t)guestW,
                                         (int32_t)guestH,
                                         WINDOW_FORMAT_RGBA_8888);
        capsResult = g_surface.getCaps(g_phys, surface, &caps);
        if (capsResult == VK_SUCCESS) {
            extentW = caps.currentExtent.width;
            extentH = caps.currentExtent.height;
            usable = extentW == UINT32_MAX ||
                     (extentW >= guestW && extentH >= guestH);
        } else {
            usable = false;
        }
    }

    /* Our own swap chain may be created at the guest extent even when the
     * surface reports a different currentExtent, so the guest extent has to be
     * inside the allowed range. */
    if (usable && !producer &&
        (guestW < caps.minImageExtent.width ||
         guestH < caps.minImageExtent.height ||
         guestW > caps.maxImageExtent.width ||
         guestH > caps.maxImageExtent.height)) {
        PP_LOG("probe: guest %ux%u outside window image range %ux%u..%ux%u",
               guestW, guestH, caps.minImageExtent.width,
               caps.minImageExtent.height, caps.maxImageExtent.width,
               caps.maxImageExtent.height);
        usable = false;
    }

    PP_LOG("probe %s %ux%u: current=%ux%u min=%ux%u max=%ux%u usage=0x%x %s",
           producer ? "producer" : "window", guestW, guestH, extentW, extentH,
           caps.minImageExtent.width, caps.minImageExtent.height,
           caps.maxImageExtent.width, caps.maxImageExtent.height,
           (unsigned)caps.supportedUsageFlags, usable ? "ok" : "rejected");

    g_surface.destroySurface(g_instance, surface, nullptr);
    return usable;
}

void destroy_screen();

/* Create the command pool, per-frame semaphores and command buffers. */
bool create_device_objects()
{
    VkCommandPoolCreateInfo poolInfo;
    std::memset(&poolInfo, 0, sizeof poolInfo);
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = g_queueFamily;
    if (!g_dev.createCommandPool ||
        g_dev.createCommandPool(g_device, &poolInfo, nullptr, &g_pool) !=
            VK_SUCCESS) {
        PP_LOG("device: command pool failed");
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo;
    std::memset(&allocInfo, 0, sizeof allocInfo);
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = g_pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kFrames;
    if (g_dev.allocateCommandBuffers(g_device, &allocInfo, g_cmd) !=
        VK_SUCCESS) {
        PP_LOG("device: command buffers failed");
        return false;
    }

    VkSemaphoreCreateInfo semInfo;
    std::memset(&semInfo, 0, sizeof semInfo);
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (uint32_t i = 0; i < kFrames; ++i) {
        if (g_dev.createSemaphore(g_device, &semInfo, nullptr,
                                  &g_imageReady[i]) != VK_SUCCESS ||
            g_dev.createSemaphore(g_device, &semInfo, nullptr,
                                  &g_blitDoneGuest[i]) != VK_SUCCESS ||
            g_dev.createSemaphore(g_device, &semInfo, nullptr,
                                  &g_blitDoneScreen[i]) != VK_SUCCESS) {
            PP_LOG("device: semaphores failed at frame %u", i);
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------ screen surface ---- */

void destroy_screen()
{
    const Wc3Platform *platform = platform_or_null();

    g_screenImages.clear();

    if (g_screenSwapchain && g_dev.destroySwapchain)
        g_dev.destroySwapchain(g_device, g_screenSwapchain, nullptr);
    g_screenSwapchain = VK_NULL_HANDLE;

    if (g_screenSurface && g_surface.destroySurface)
        g_surface.destroySurface(g_instance, g_screenSurface, nullptr);
    g_screenSurface = VK_NULL_HANDLE;

    if (g_screenWindow) {
        surface_release(platform, g_screenWindow);
        g_screenWindow = 0;
    }
    g_screenExtent = {0, 0};
    g_screenFormat = VK_FORMAT_UNDEFINED;
}

VkCompositeAlphaFlagBitsKHR pick_composite_alpha(VkCompositeAlphaFlagsKHR flags)
{
    if (flags & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (flags & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
        return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (flags & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
        return VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    return VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
}

bool create_screen(VkQueue queue, uintptr_t window, uint32_t wantW, uint32_t wantH)
{
    /* The old swap chain may still be owned by the presentation engine and a
     * previous blit may still be in flight.  This runs on a mode or window
     * change, not per frame, so waiting here is the correct trade. */
    if (g_dev.queueWaitIdle) g_dev.queueWaitIdle(queue);
    destroy_screen();

    if (!g_surface.createSurface || !g_surface.getCaps || !g_surface.getFormats)
        return false;

    ANativeWindow *native = reinterpret_cast<ANativeWindow *>(window);
    /* The real window's buffer geometry is the layer the compositor scans
     * out.  On the grid path it must be the full surface, not the previous
     * screen swap chain's guest-sized buffer; otherwise our exact 2x image is
     * written into a smaller layer and the compositor scales it again with
     * its own (smooth) filter.  Set it explicitly before asking for caps so
     * currentExtent/maxImageExtent describe the buffer we are about to use. */
    const int geoRc = ANativeWindow_setBuffersGeometry(
        native, (int32_t)wantW, (int32_t)wantH, WINDOW_FORMAT_RGBA_8888);
    PP_LOG("screen: geometry %ux%u rc=%d live=%dx%d", wantW, wantH, geoRc,
           ANativeWindow_getWidth(native), ANativeWindow_getHeight(native));

    VkAndroidSurfaceCreateInfoKHR surfaceInfo;
    std::memset(&surfaceInfo, 0, sizeof surfaceInfo);
    surfaceInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.window = native;
    if (g_surface.createSurface(g_instance, &surfaceInfo, nullptr,
                                &g_screenSurface) != VK_SUCCESS) {
        PP_LOG("screen: surface create failed window=%p", (void *)window);
        return false;
    }

    VkSurfaceCapabilitiesKHR caps;
    if (g_surface.getCaps(g_phys, g_screenSurface, &caps) != VK_SUCCESS) {
        PP_LOG("screen: caps failed");
        destroy_screen();
        return false;
    }

    PP_LOG("screen: caps current=%ux%u min=%ux%u max=%ux%u transform=0x%x "
           "usage=0x%x",
           caps.currentExtent.width, caps.currentExtent.height,
           caps.minImageExtent.width, caps.minImageExtent.height,
           caps.maxImageExtent.width, caps.maxImageExtent.height,
           (unsigned)caps.currentTransform, (unsigned)caps.supportedUsageFlags);

    /* imageExtent is ours to choose (between min and max); it does not have to
     * equal currentExtent.  A smaller image is scaled by the presentation
     * engine / compositor, which is exactly what the general-case path wants. */
    VkExtent2D extent;
    extent.width = wantW;
    extent.height = wantH;
    if (extent.width < caps.minImageExtent.width)
        extent.width = caps.minImageExtent.width;
    if (extent.height < caps.minImageExtent.height)
        extent.height = caps.minImageExtent.height;
    if (extent.width > caps.maxImageExtent.width)
        extent.width = caps.maxImageExtent.width;
    if (extent.height > caps.maxImageExtent.height)
        extent.height = caps.maxImageExtent.height;
    if (!extent.width || !extent.height) {
        PP_LOG("screen: empty extent");
        destroy_screen();
        return false;
    }
    if (extent.width != wantW || extent.height != wantH) {
        /* A clamped screen image would be handed to the compositor for the
         * rest of the scale, so the grid would lose its exact 2x output.  Do
         * not silently build a screen the plan did not ask for; the caller
         * retires the target and the next Present goes to the real window. */
        PP_LOG("screen: extent clamped %ux%u -> %ux%u, refusing", wantW, wantH,
               extent.width, extent.height);
        destroy_screen();
        return false;
    }

    uint32_t formatCount = 0;
    g_surface.getFormats(g_phys, g_screenSurface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    if (formatCount) {
        g_surface.getFormats(g_phys, g_screenSurface, &formatCount,
                             formats.data());
        formats.resize(formatCount);
    }
    if (formats.empty()) {
        PP_LOG("screen: no surface formats");
        destroy_screen();
        return false;
    }

#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    for (size_t i = 0; i < formats.size(); ++i) {
        PP_LOG("screen: format[%u]=%d colorSpace=%d (guest=%d)", (unsigned)i,
               (int)formats[i].format, (int)formats[i].colorSpace,
               (int)g_guestFormat);
    }
#endif

    VkSurfaceFormatKHR chosen = formats[0];
    bool exact = false;
    for (size_t i = 0; i < formats.size(); ++i) {
        if (formats[i].format == g_guestFormat) {
            chosen = formats[i];
            exact = true;
            if (formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                break;
        }
    }
    if (!exact) {
        /* A copy or a blit between compatible 32-bit formats reinterprets the
         * texels; it does not swap red and blue.  This path has no shader to
         * fix a channel order, so a window that cannot take the guest format
         * gets the ordinary path instead of a colour-swapped image. */
        PP_LOG("screen: window does not offer guest format %d (first %d)",
               (int)g_guestFormat, (int)formats[0].format);
        destroy_screen();
        return false;
    }

    uint32_t imageCount = caps.minImageCount ? caps.minImageCount : 2;
    if (caps.maxImageCount && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR swapInfo;
    std::memset(&swapInfo, 0, sizeof swapInfo);
    swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapInfo.surface = g_screenSurface;
    swapInfo.minImageCount = imageCount;
    swapInfo.imageFormat = chosen.format;
    swapInfo.imageColorSpace = chosen.colorSpace;
    swapInfo.imageExtent = extent;
    swapInfo.imageArrayLayers = 1;
    /* TRANSFER_DST alone is not enough on Android: without COLOR_ATTACHMENT
     * the WSI does not add AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT and the
     * compositor scans out a black buffer even though the blit succeeded.
     * The as-built host asks for COLOR_ATTACHMENT | TRANSFER_SRC. */
    swapInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    /* The guest image is blitted without rotating it, exactly like DXVK's own
     * direct present.  The SurfaceView's rotation belongs to the compositor,
     * so the screen swap chain must not claim the content is already rotated.
     * Using caps.currentTransform here (ROTATE_270 on this device) told the
     * WSI that the landscape image was pre-rotated; the compositor then
     * rotated the whole layer out of view and the integer scaler looked
     * black even though the magenta clear was visible. */
    swapInfo.preTransform =
        (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
            : caps.currentTransform;
    swapInfo.compositeAlpha = pick_composite_alpha(caps.supportedCompositeAlpha);
    swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapInfo.clipped = VK_TRUE;
    swapInfo.oldSwapchain = VK_NULL_HANDLE;

    if (g_dev.createSwapchain(g_device, &swapInfo, nullptr, &g_screenSwapchain) !=
        VK_SUCCESS) {
        PP_LOG("screen: swapchain create failed %ux%u format=%d", extent.width,
               extent.height, (int)chosen.format);
        destroy_screen();
        return false;
    }

    uint32_t count = 0;
    g_dev.getSwapchainImages(g_device, g_screenSwapchain, &count, nullptr);
    g_screenImages.resize(count);
    if (count) {
        g_dev.getSwapchainImages(g_device, g_screenSwapchain, &count,
                                 g_screenImages.data());
        g_screenImages.resize(count);
    }
    if (g_screenImages.empty()) {
        PP_LOG("screen: no images");
        destroy_screen();
        return false;
    }

    g_screenWindow = window;
    g_screenExtent = extent;
    g_screenFormat = chosen.format;

    PP_LOG("screen: created %ux%u format=%d images=%u (wanted %ux%u)", extent.width,
           extent.height, (int)chosen.format, (unsigned)g_screenImages.size(),
           wantW, wantH);
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    PP_LOG("screen: create params usage=0x%x preTransform=0x%x composite=0x%x "
           "minImages=%u extent=%ux%u fmt=%d mode=%d capsTransform=0x%x "
           "supportedTransforms=0x%x alphaSupport=0x%x",
           (unsigned)swapInfo.imageUsage, (unsigned)swapInfo.preTransform,
           (unsigned)swapInfo.compositeAlpha, (unsigned)imageCount,
           extent.width, extent.height, (int)chosen.format,
           (int)swapInfo.presentMode, (unsigned)caps.currentTransform,
           (unsigned)caps.supportedTransforms,
           (unsigned)caps.supportedCompositeAlpha);
#endif
    return true;
}

/* ---------------------------------------------------------- recording ---- */

VkImageMemoryBarrier image_barrier(VkImage image, VkImageLayout from,
                                   VkImageLayout to, VkAccessFlags srcAccess,
                                   VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier barrier;
    std::memset(&barrier, 0, sizeof barrier);
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    return barrier;
}

/* ------------------------------------------------------------- present ---- */

PFN_vkQueuePresentKHR real_present()
{
    if (!g_dev.queuePresent && g_realGdpa && g_device) {
        g_dev.queuePresent = reinterpret_cast<PFN_vkQueuePresentKHR>(
            g_realGdpa(g_device, "vkQueuePresentKHR"));
    }
    return g_dev.queuePresent;
}

VKAPI_ATTR VkResult VKAPI_CALL pp_vkQueuePresentKHR(VkQueue queue,
                                                   const VkPresentInfoKHR *info)
{
    PFN_vkQueuePresentKHR real = real_present();
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;

    /* A retired target must not be presented to: its producer is unpublished
     * and the next guest present goes back to the real window.  The reader is
     * still alive here (retire defers the delete to reap). */
    const bool readyNow = g_ready.load(std::memory_order_acquire) != 0;
    const bool targetNow = wc3_present_target_active() != 0;
    const bool sameSwapchain =
        info && info->swapchainCount == 1 && info->pSwapchains &&
        info->pImageIndices && info->pSwapchains[0] == g_guestSwapchain;
    const bool usable =
        readyNow && targetNow && info && info->swapchainCount == 1 &&
        info->pSwapchains && info->pImageIndices &&
        info->pSwapchains[0] == g_guestSwapchain &&
        info->waitSemaphoreCount <= kMaxWaitSemaphores;
    if (!usable) {
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
        /* A direct present means the pass did not touch the frame: DXVK is
         * presenting into the real window and its own blitter is scaling.
         * Print the first occurrence of each reason, not every frame. */
        int reason = 0;
        if (!readyNow) reason |= 1;
        if (!targetNow) reason |= 2;
        if (!info) reason |= 4;
        if (info && (info->swapchainCount != 1 || !info->pSwapchains ||
                     !info->pImageIndices || !sameSwapchain))
            reason |= 8;
        if (info && info->waitSemaphoreCount > kMaxWaitSemaphores) reason |= 16;
        static int lastReason = -1;
        if (reason != lastReason) {
            lastReason = reason;
            PP_LOG("direct present: reason=0x%x ready=%d target=%d info=%p "
                   "scCount=%u scMatch=%d waits=%u",
                   reason, readyNow, targetNow, (const void *)info,
                   info ? info->swapchainCount : 0u, sameSwapchain,
                   info ? info->waitSemaphoreCount : 0u);
        }
#endif
        return real(queue, info);
    }

    wc3_present_target_drain();

    uint32_t guestW = 0;
    uint32_t guestH = 0;
    if (!wc3_present_target_extent(&guestW, &guestH)) return real(queue, info);

    const Wc3Platform *platform = platform_or_null();
    uintptr_t window = 0;
    if (surface_acquire(platform, &window) != 0 || !window) {
        PP_LOG("direct present: no real window");
        return real(queue, info);
    }

    ANativeWindow *native = reinterpret_cast<ANativeWindow *>(window);
    /* The surface extent comes from the mode layer, not from the live window.
     * Once our screen swap chain exists, ANativeWindow_getWidth/Height can
     * report the swap chain's buffer extent (guest size on the compositor
     * path).  Reading that here would make exact half undetectable and send
     * the grid frame down the copy path. */
    uint32_t surfaceW = 0;
    uint32_t surfaceH = 0;
    const int surfaceFromMode =
        wc3_present_target_surface_extent(&surfaceW, &surfaceH);
    if (!surfaceFromMode || !surfaceW || !surfaceH) {
        surfaceW = (uint32_t)ANativeWindow_getWidth(native);
        surfaceH = (uint32_t)ANativeWindow_getHeight(native);
    }
    if (!surfaceW || !surfaceH) {
        /* No usable window geometry: fall back to the guest extent, which
         * yields the pure copy path. */
        surfaceW = guestW;
        surfaceH = guestH;
    }

    const int halfGrid = wc3_present_target_half_grid();
    const PresentScalePlan plan =
        present_scale_plan(guestW, guestH, surfaceW, surfaceH, halfGrid);

#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    /* State-change trace for the scaling decision.  Printed once when any
     * input changes, then every 240 frames so a long run still proves which
     * path is active.  The individual fields are the full answer to "why is
     * this frame not 2x nearest?". */
    {
        static uint32_t lastGuestW, lastGuestH, lastSurfaceW, lastSurfaceH;
        static uint32_t lastTargetW, lastTargetH, lastSrcW, lastSrcH;
        static uint32_t lastImageW, lastImageH;
        static int lastHalf = -1, lastFilter = -1, lastSource = -1;
        static uint32_t traceFrames;
        const int source = surfaceFromMode ? 1 : 2;
        const bool changed =
            lastHalf < 0 || guestW != lastGuestW || guestH != lastGuestH ||
            surfaceW != lastSurfaceW || surfaceH != lastSurfaceH ||
            plan.target_w != lastTargetW || plan.target_h != lastTargetH ||
            plan.src_w != lastSrcW || plan.src_h != lastSrcH ||
            g_guestExtent.width != lastImageW ||
            g_guestExtent.height != lastImageH ||
            plan.filter != lastFilter || source != lastSource;
        if (changed || (traceFrames++ % 240u) == 0u) {
            PP_LOG("plan: guest=%ux%u image=%ux%u surface=%ux%u(%s) "
                   "half=%d even=%d exactHalf=%d -> target=%ux%u filter=%s "
                   "system=%d",
                   guestW, guestH, g_guestExtent.width, g_guestExtent.height,
                   surfaceW, surfaceH,
                   surfaceFromMode ? "mode" : "window", halfGrid,
                   (surfaceW & 1u) == 0u && (surfaceH & 1u) == 0u,
                   guestW == surfaceW / 2u && guestH == surfaceH / 2u,
                   plan.target_w, plan.target_h,
                   plan.filter == PRESENT_SCALE_FILTER_NEAREST ? "nearest"
                                                              : "copy",
                   plan.system_scale);
            lastGuestW = guestW;
            lastGuestH = guestH;
            lastSurfaceW = surfaceW;
            lastSurfaceH = surfaceH;
            lastTargetW = plan.target_w;
            lastTargetH = plan.target_h;
            lastSrcW = plan.src_w;
            lastSrcH = plan.src_h;
            lastImageW = g_guestExtent.width;
            lastImageH = g_guestExtent.height;
            lastHalf = halfGrid;
            lastFilter = (int)plan.filter;
            lastSource = source;
        }
    }
#endif

    /* The plan's source rectangle comes from the published target extent; the
     * image comes from DXVK's swap chain.  A mode change should have rebuilt
     * that swap chain before this present, but a stale one must never make us
     * copy outside the image. */
    if (g_guestExtent.width < plan.src_w || g_guestExtent.height < plan.src_h) {
        PP_LOG("present: guest image %ux%u smaller than %ux%u, direct present",
               g_guestExtent.width, g_guestExtent.height, plan.src_w, plan.src_h);
        surface_release(platform, window);
        return real(queue, info);
    }

    const bool haveScreen =
        g_screenSwapchain != VK_NULL_HANDLE && g_screenWindow == window &&
        g_screenExtent.width == plan.target_w &&
        g_screenExtent.height == plan.target_h &&
        g_screenFormat == g_guestFormat;
    if (!haveScreen) {
        if (!create_screen(queue, window, plan.target_w, plan.target_h)) {
            /* Without a screen swap chain the guest image would go nowhere, so
             * this is the one case that must not silently continue: unpublish
             * the producer and let DXVK rebuild its presenter on the real
             * window.  The generation change is what makes DXVK notice.  The
             * reader stays alive until DXVK has destroyed the swap chain that
             * still references it; deleting it here would be a use-after-free
             * on the real present below. */
            PP_LOG("present: screen unavailable, falling back to the real window");
            surface_release(platform, window);
            g_ready.store(0, std::memory_order_release);
            wc3_present_target_retire();
            return real(queue, info);
        }
    }

    const uint32_t frame = g_frame;
    g_frame = (frame + 1) % kFrames;

    uint32_t screenIndex = 0;
    VkResult acquire = g_dev.acquireNextImage(
        g_device, g_screenSwapchain, UINT64_MAX, g_imageReady[frame],
        VK_NULL_HANDLE, &screenIndex);
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        PP_LOG("present: acquire failed %d, recreating screen", (int)acquire);
        destroy_screen();
        surface_release(platform, window);
        return real(queue, info);
    }
    if (screenIndex >= g_screenImages.size() || g_guestImages.empty() ||
        info->pImageIndices[0] >= g_guestImages.size()) {
        surface_release(platform, window);
        return real(queue, info);
    }

    VkImage guestImage = g_guestImages[info->pImageIndices[0]];
    VkImage screenImage = g_screenImages[screenIndex];

    VkCommandBuffer cmd = g_cmd[frame];
    g_dev.resetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin;
    std::memset(&begin, 0, sizeof begin);
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_dev.beginCommandBuffer(cmd, &begin);

    /* DXVK leaves its swap chain image in the layout of its own blit render
     * pass; PRESENT_SRC_KHR is the layout an acquired WSI image is defined to
     * have, and on Android both map to the same memory, so this transition is
     * exact in the normal case and a no-op in the worst one.  See PS-13. */
    /* The guest image was written by DXVK's swap-chain blit render pass at
     * COLOR_ATTACHMENT_OUTPUT and left in PRESENT_SRC_KHR there.  The wait on
     * the guest present semaphore must cover that stage as well as TRANSFER:
     * putting the layout transition on TOP_OF_PIPE let it run before the guest
     * render was complete, which loses the UBWC/tile contents on Adreno and
     * makes the blit read black. */
    const VkPipelineStageFlags guestWriteStages =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkImageMemoryBarrier toSrc =
        image_barrier(guestImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT);
    g_dev.cmdPipelineBarrier(cmd, guestWriteStages,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &toSrc);

    /* The whole screen image is overwritten every frame, so its previous
     * contents do not need to be preserved and UNDEFINED is both the cheapest
     * and the only first-use-safe source layout.  The acquired image is still
     * owned by the presentation engine until g_imageReady[frame] is waited on,
     * so this transition must run in the TRANSFER stage, not TOP_OF_PIPE. */
    VkImageMemoryBarrier toDst =
        image_barrier(screenImage, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                      VK_ACCESS_TRANSFER_WRITE_BIT);
    g_dev.cmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &toDst);

    if (plan.filter == PRESENT_SCALE_FILTER_NEAREST) {
        VkImageBlit region;
        std::memset(&region, 0, sizeof region);
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.srcOffsets[1].x = (int32_t)plan.src_w;
        region.srcOffsets[1].y = (int32_t)plan.src_h;
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.dstOffsets[1].x = (int32_t)plan.target_w;
        region.dstOffsets[1].y = (int32_t)plan.target_h;
        g_dev.cmdBlitImage(cmd, guestImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           screenImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region, VK_FILTER_NEAREST);
    } else {
        VkImageCopy region;
        std::memset(&region, 0, sizeof region);
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.extent.width = plan.src_w;
        region.extent.height = plan.src_h;
        region.extent.depth = 1;
        g_dev.cmdCopyImage(cmd, guestImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           screenImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);
    }

    VkImageMemoryBarrier backGuest =
        image_barrier(guestImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT,
                      0);
    g_dev.cmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &backGuest);

    VkImageMemoryBarrier backScreen =
        image_barrier(screenImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    g_dev.cmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &backScreen);

    g_dev.endCommandBuffer(cmd);

    /* No fence here.  The screen image is reused only after this submission's
     * transfer completes, and g_blitDoneScreen / g_blitDoneGuest carry that
     * ordering to the two presents below.  A per-image fence that is never
     * waited on orders nothing; the guest swap chain's WSI acquire and release
     * already supply the guest-side serialisation. */

    /* The screen image was just acquired with g_imageReady[frame], so that
     * image is not readable by the presentation engine yet.  Waiting for the
     * acquire semaphore here is mandatory: without it the blit may overwrite
     * an image the compositor is still scanning out, which shows up as
     * tearing or stale blocks rather than as a wrong filter.  The guest's own
     * wait semaphores stay first so their stage order is preserved. */
    VkSemaphore waitSemaphores[kMaxWaitSemaphores + 1];
    VkPipelineStageFlags waitStages[kMaxWaitSemaphores + 1];
    uint32_t waitCount = 0;
    for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i) {
        waitSemaphores[waitCount] = info->pWaitSemaphores[i];
        /* The guest semaphore covers DXVK's color-attachment writes.  Waiting
         * only at TRANSFER leaves the layout transition below able to execute
         * before the render is done. */
        waitStages[waitCount] = guestWriteStages;
        ++waitCount;
    }
    waitSemaphores[waitCount] = g_imageReady[frame];
    waitStages[waitCount] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    ++waitCount;

    VkSemaphore signals[2] = {g_blitDoneGuest[frame], g_blitDoneScreen[frame]};

    VkSubmitInfo submit;
    std::memset(&submit, 0, sizeof submit);
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = waitCount;
    submit.pWaitSemaphores = waitSemaphores;
    submit.pWaitDstStageMask = waitStages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 2;
    submit.pSignalSemaphores = signals;

    const VkResult submitResult =
        g_dev.queueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    if (submitResult != VK_SUCCESS) {
        /* The guest's wait semaphores were not consumed, so the ordinary
         * present still owns them; the acquired screen image is dropped with
         * the swap chain and rebuilt on the next frame. */
        PP_LOG("present: submit failed %d", (int)submitResult);
        g_dev.queueWaitIdle(queue);
        destroy_screen();
        surface_release(platform, window);
        return real(queue, info);
    }

    VkPresentInfoKHR guestInfo = *info;
    guestInfo.waitSemaphoreCount = 1;
    guestInfo.pWaitSemaphores = &g_blitDoneGuest[frame];
    VkResult guestResult = real(queue, &guestInfo);
    if (info->pResults) info->pResults[0] = guestResult;

    VkPresentInfoKHR screenInfo;
    std::memset(&screenInfo, 0, sizeof screenInfo);
    screenInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    screenInfo.waitSemaphoreCount = 1;
    screenInfo.pWaitSemaphores = &g_blitDoneScreen[frame];
    screenInfo.swapchainCount = 1;
    screenInfo.pSwapchains = &g_screenSwapchain;
    screenInfo.pImageIndices = &screenIndex;
    const VkResult screenResult = real(queue, &screenInfo);
    if (screenResult != VK_SUCCESS && screenResult != VK_SUBOPTIMAL_KHR) {
        PP_LOG("present: screen present failed %d", (int)screenResult);
        g_dev.queueWaitIdle(queue);
        destroy_screen();
    }
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    {
        static VkResult lastGuestResult = VK_SUCCESS;
        static VkResult lastScreenResult = VK_SUCCESS;
        if (guestResult != lastGuestResult || screenResult != lastScreenResult) {
            lastGuestResult = guestResult;
            lastScreenResult = screenResult;
            PP_LOG("present results: guest=%d screen=%d", (int)guestResult,
                   (int)screenResult);
        }
    }
#endif

#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    if ((g_frameLog++ % 240u) == 0u) {
        PP_LOG("frame %u: guest=%ux%u image=%ux%u fmt=%d surface=%ux%u "
               "screen=%ux%u images=%u fmt=%d target=%ux%u src=%ux%u "
               "filter=%s system=%d guestIdx=%u screenIdx=%u acq=%d waits=%u",
               (unsigned)g_frameLog, guestW, guestH, g_guestExtent.width,
               g_guestExtent.height, (int)g_guestFormat, surfaceW, surfaceH,
               g_screenExtent.width, g_screenExtent.height,
               (unsigned)g_screenImages.size(), (int)g_screenFormat,
               plan.target_w, plan.target_h, plan.src_w, plan.src_h,
               plan.filter == PRESENT_SCALE_FILTER_NEAREST ? "nearest" : "copy",
               plan.system_scale, info->pImageIndices[0], screenIndex,
               (int)acquire, (unsigned)waitCount);
    }
#endif

    surface_release(platform, window);
    return guestResult;
}

/* --------------------------------------------------------- swap chains ---- */

PFN_vkCreateSwapchainKHR real_create_swapchain()
{
    if (!g_dev.createSwapchain && g_realGdpa && g_device) {
        g_dev.createSwapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
            g_realGdpa(g_device, "vkCreateSwapchainKHR"));
    }
    return g_dev.createSwapchain;
}

VKAPI_ATTR VkResult VKAPI_CALL
pp_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                        const VkAllocationCallbacks *alloc,
                        VkSwapchainKHR *swapchain)
{
    PFN_vkCreateSwapchainKHR real = real_create_swapchain();
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;

    if (g_reapPending) {
        g_reapPending = false;
        wc3_present_target_reap();
    }

    if (!info || !swapchain) return real(device, info, alloc, swapchain);

    if (!wc3_present_target_active()) {
        /* DXVK is going back to the real window.  Our screen swap chain is a
         * second consumer of that same window, so destroy it before DXVK's
         * swap chain starts using the window's buffer queue.  DXVK is idle
         * here (presenter rebuild waits for the device). */
        if (g_screenSwapchain) {
            PP_LOG("target inactive: destroy screen swap chain");
            destroy_screen();
        }
        return real(device, info, alloc, swapchain);
    }

    if (!g_ready.load(std::memory_order_acquire) && !activate_pass(false)) {
        /* The target cannot be displayed on this device.  Unpublish it but do
         * not delete the reader yet: DXVK is about to create a swap chain on
         * it and the generation change makes the next Present rebuild on the
         * real window.  The reader is reaped after that presenter's swap chain
         * and surface are gone. */
        PP_LOG("late activation failed: retiring target");
        g_guestSwapchain = VK_NULL_HANDLE;
        g_guestImages.clear();
        wc3_present_target_retire();
        return real(device, info, alloc, swapchain);
    }

    /* The pass reads the guest image, so DXVK's swap chain needs TRANSFER_SRC
     * on top of whatever it asked for.  At boot the producer probe checked that
     * the surface advertises it; a target started at runtime skipped that probe
     * because DXVK's VkSurface already exists, so a rejected creation is
     * handled here by retrying exactly what DXVK asked for and returning the
     * next Present to the real window. */
    const bool addedTransferSrc =
        (info->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0;
    VkSwapchainCreateInfoKHR create = *info;
    create.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    VkResult result = real(device, &create, alloc, swapchain);
    if (result != VK_SUCCESS && addedTransferSrc) {
        PP_LOG("guest swapchain rejected TRANSFER_SRC (%d), retrying direct",
               (int)result);
        result = real(device, info, alloc, swapchain);
        if (result == VK_SUCCESS) {
            g_guestSwapchain = VK_NULL_HANDLE;
            g_guestImages.clear();
            g_ready.store(0, std::memory_order_release);
            wc3_present_target_retire();
        }
        return result;
    }
    if (result != VK_SUCCESS) return result;

    g_guestSwapchain = *swapchain;
    g_guestExtent = create.imageExtent;
    g_guestFormat = create.imageFormat;

    uint32_t count = 0;
    g_guestImages.clear();
    if (g_dev.getSwapchainImages) {
        g_dev.getSwapchainImages(device, g_guestSwapchain, &count, nullptr);
        g_guestImages.resize(count);
        if (count) {
            g_dev.getSwapchainImages(device, g_guestSwapchain, &count,
                                     g_guestImages.data());
            g_guestImages.resize(count);
        }
    }

    PP_LOG("guest swapchain %ux%u format=%d images=%u usage=0x%x",
           create.imageExtent.width, create.imageExtent.height,
           (int)create.imageFormat, (unsigned)g_guestImages.size(),
           (unsigned)create.imageUsage);
    return result;
}

VKAPI_ATTR void VKAPI_CALL
pp_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                         const VkAllocationCallbacks *alloc)
{
    /* The wrapper is installed even while the target is dormant, so resolve
     * the real entry point lazily instead of assuming load_device_fns ran. */
    PFN_vkDestroySwapchainKHR real = g_dev.destroySwapchain;
    if (!real && g_realGdpa && device) {
        real = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
            g_realGdpa(device, "vkDestroySwapchainKHR"));
        g_dev.destroySwapchain = real;
    }

    if (swapchain && swapchain == g_guestSwapchain) {
        g_guestSwapchain = VK_NULL_HANDLE;
        g_guestImages.clear();
    }
    if (real) real(device, swapchain, alloc);

    /* Do not reap here: this runs before the presenter destroys its surface,
     * which still holds the reader's ANativeWindow.  The next
     * vkCreateSwapchainKHR (or vkDestroyDevice) does it after teardown. */
    g_reapPending = true;
}

VKAPI_ATTR void VKAPI_CALL
pp_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
    /* Resolve the real entry point before any state is cleared: afterwards
     * there is no device left to ask. */
    PFN_vkDestroyDevice destroy = nullptr;
    if (g_realGdpa && device) {
        destroy = reinterpret_cast<PFN_vkDestroyDevice>(
            g_realGdpa(device, "vkDestroyDevice"));
    }

    if (device && device == g_device) {
        g_ready.store(0, std::memory_order_release);
        /* The device is going away, so wait for the last frame instead of
         * leaking in-flight work. */
        if (g_dev.deviceWaitIdle) g_dev.deviceWaitIdle(device);

        destroy_screen();

        if (g_pool && g_dev.destroyCommandPool)
            g_dev.destroyCommandPool(device, g_pool, nullptr);
        g_pool = VK_NULL_HANDLE;

        for (uint32_t i = 0; i < kFrames; ++i) {
            if (g_dev.destroySemaphore) {
                if (g_imageReady[i]) {
                    g_dev.destroySemaphore(device, g_imageReady[i], nullptr);
                    g_imageReady[i] = VK_NULL_HANDLE;
                }
                if (g_blitDoneGuest[i]) {
                    g_dev.destroySemaphore(device, g_blitDoneGuest[i], nullptr);
                    g_blitDoneGuest[i] = VK_NULL_HANDLE;
                }
                if (g_blitDoneScreen[i]) {
                    g_dev.destroySemaphore(device, g_blitDoneScreen[i], nullptr);
                    g_blitDoneScreen[i] = VK_NULL_HANDLE;
                }
            }
            g_cmd[i] = VK_NULL_HANDLE;
        }

        g_guestSwapchain = VK_NULL_HANDLE;
        g_guestImages.clear();
        /* The device is idle and going away: readers retired for a swap chain
         * that is already destroyed (or is about to be) are safe to free. */
        wc3_present_target_reap();
        g_device = VK_NULL_HANDLE;
        g_phys = VK_NULL_HANDLE;
    }

    if (destroy) destroy(device, alloc);
}

/* ------------------------------------------------------------ bootstrap ---- */

bool probe(VkPhysicalDevice physical, uint32_t guestW, uint32_t guestH,
           bool checkProducer)
{
    (void)physical;

    const Wc3Platform *platform = platform_or_null();

    if (checkProducer) {
        ANativeWindow *producer = wc3_present_target_window();
        if (!producer) {
            PP_LOG("probe: no producer window");
            return false;
        }
        if (!probe_surface(producer, guestW, guestH, true)) return false;
    }

    uintptr_t window = 0;
    if (surface_acquire(platform, &window) != 0 || !window) {
        PP_LOG("probe: no screen window");
        return false;
    }
    ANativeWindow *native = reinterpret_cast<ANativeWindow *>(window);
    /* Probe the extent the pass will actually ask the screen swap chain for.
     * In grid mode that is the full surface; in the general path it is the
     * guest extent.  Setting the geometry first matters because Android WSI
     * commonly reports currentExtent == minImageExtent == the window buffer
     * size; probing a 1200x540 target while the window is still 2400x1080
     * fails the minImageExtent check even though create_screen would resize
     * the window before creating the surface. */
    uint32_t targetW = guestW;
    uint32_t targetH = guestH;
    uint32_t surfaceW = 0;
    uint32_t surfaceH = 0;
    if (wc3_present_target_surface_extent(&surfaceW, &surfaceH) && surfaceW &&
        surfaceH) {
        const PresentScalePlan plan =
            present_scale_plan(guestW, guestH, surfaceW, surfaceH,
                               wc3_present_target_half_grid());
        targetW = plan.target_w;
        targetH = plan.target_h;
    }
    const int rc = ANativeWindow_setBuffersGeometry(
        native, (int32_t)targetW, (int32_t)targetH, WINDOW_FORMAT_RGBA_8888);
    PP_LOG("probe: window geometry target=%ux%u guest=%ux%u surface=%ux%u "
           "rc=%d live=%dx%d",
           targetW, targetH, guestW, guestH, surfaceW, surfaceH, rc,
           ANativeWindow_getWidth(native), ANativeWindow_getHeight(native));
    const bool screenOk = probe_surface(native, targetW, targetH, false);
    surface_release(platform, window);
    return screenOk;
}

/* Finish the pass on an existing device.  Called either at device creation or,
 * for a target that was dormant at boot, from the first swap chain creation
 * after a runtime mode change.  Runs on the guest render thread in both cases,
 * so it never races a present. */
bool activate_pass(bool checkProducer)
{
    if (g_ready.load(std::memory_order_acquire)) return true;
    if (!g_device || !g_realGdpa) {
        PP_LOG("activate: no device/gdpa");
        return false;
    }

    uint32_t guestW = 0;
    uint32_t guestH = 0;
    if (!wc3_present_target_extent(&guestW, &guestH)) {
        PP_LOG("activate: no target extent");
        return false;
    }

    if (!load_surface_fns() || !load_device_fns()) {
        PP_LOG("activate: entry points missing");
        return false;
    }

    if (!probe(g_phys, guestW, guestH, checkProducer)) {
        PP_LOG("activate: probe rejected");
        return false;
    }

    if (!create_device_objects()) {
        PP_LOG("activate: device objects failed");
        return false;
    }

    g_ready.store(1, std::memory_order_release);
    PP_LOG("device: ready guest=%ux%u queueFamily=%u halfGrid=%d", guestW, guestH,
           g_queueFamily, wc3_present_target_half_grid());
    return true;
}

void on_device_created(VkPhysicalDevice physical, VkDevice device,
                       const VkDeviceCreateInfo *info)
{
    g_phys = physical;
    g_device = device;

    /* DXVK asks for vkGetDeviceProcAddr only after vkCreateDevice returns, but
     * activation runs here, inside the create call.  Resolve the real device
     * entry point from the instance now; otherwise load_device_fns() sees a
     * null g_realGdpa and the whole pass is rejected before the first frame. */
    if (!g_realGdpa && g_realGipa && g_instance) {
        g_realGdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            g_realGipa(g_instance, "vkGetDeviceProcAddr"));
        PP_LOG("device: resolved gdpa at creation (%p)",
               reinterpret_cast<void *>(g_realGdpa));
    }

    g_queueFamily = 0;
    if (info && info->queueCreateInfoCount && info->pQueueCreateInfos)
        g_queueFamily = info->pQueueCreateInfos[0].queueFamilyIndex;

    if (!wc3_present_target_active()) {
        /* Native resolution: the target is deliberately unpublished and DXVK
         * presents into the real window.  Keep the device so a later mode
         * change can activate the pass without another device creation. */
        PP_LOG("device: pass dormant (direct window)");
        return;
    }

    if (!activate_pass(true)) {
        /* Fail safe: hand DXVK the real window again.  No guest swap chain
         * exists yet, so deleting the producer here is safe. */
        PP_LOG("device: activation rejected, falling back to the real window");
        wc3_present_target_stop();
    }
}

VKAPI_ATTR VkResult VKAPI_CALL
pp_vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                  const VkAllocationCallbacks *alloc, VkDevice *device)
{
    auto real = reinterpret_cast<PFN_vkCreateDevice>(instance_fn("vkCreateDevice"));
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;

    const VkResult result = real(physical, info, alloc, device);
    if (result == VK_SUCCESS && device && *device)
        on_device_created(physical, *device, info);
    return result;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL pp_gdpa(VkDevice device,
                                                const char *name)
{
    if (!g_realGdpa) return nullptr;
    const PFN_vkVoidFunction fn = g_realGdpa(device, name);
    if (!name || !fn) return fn;

    if (std::strcmp(name, "vkQueuePresentKHR") == 0) {
        g_dev.queuePresent = reinterpret_cast<PFN_vkQueuePresentKHR>(fn);
        return reinterpret_cast<PFN_vkVoidFunction>(pp_vkQueuePresentKHR);
    }
    if (std::strcmp(name, "vkCreateSwapchainKHR") == 0) {
        g_dev.createSwapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(fn);
        return reinterpret_cast<PFN_vkVoidFunction>(pp_vkCreateSwapchainKHR);
    }
    if (std::strcmp(name, "vkDestroySwapchainKHR") == 0) {
        g_dev.destroySwapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(fn);
        return reinterpret_cast<PFN_vkVoidFunction>(pp_vkDestroySwapchainKHR);
    }
    if (std::strcmp(name, "vkDestroyDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pp_vkDestroyDevice);
    return fn;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL pp_gipa(VkInstance instance,
                                                const char *name)
{
    if (!g_realGipa) return nullptr;
    if (instance) g_instance = instance;
    if (!name) return g_realGipa(instance, name);

    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        if (!g_realGdpa && instance) {
            g_realGdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                g_realGipa(instance, "vkGetDeviceProcAddr"));
        }
        return g_realGdpa ? reinterpret_cast<PFN_vkVoidFunction>(pp_gdpa)
                          : nullptr;
    }
    if (std::strcmp(name, "vkCreateDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pp_vkCreateDevice);

    return g_realGipa(instance, name);
}

}  /* namespace */

extern "C" PFN_vkGetInstanceProcAddr
wc3_present_pass_wrap_gipa(PFN_vkGetInstanceProcAddr real_gipa)
{
    if (!real_gipa) return real_gipa;

    /* Installed even when the target is dormant at boot: a resolution change
     * may start it later, and DXVK caches the device entry points.  While no
     * target is published every hook forwards to the real entry point, so the
     * direct path is the ordinary DXVK path plus one branch. */
    g_realGipa = real_gipa;

    uint32_t width = 0;
    uint32_t height = 0;
    if (wc3_present_target_extent(&width, &height)) {
        PP_LOG("installed for guest %ux%u", width, height);
    } else {
        PP_LOG("installed dormant (direct window)");
    }
    return pp_gipa;
}

extern "C" int wc3_present_pass_active(void)
{
    return g_ready.load(std::memory_order_acquire);
}
