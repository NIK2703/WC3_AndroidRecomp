/*
 * dxvk_trace.cpp - debug-only Vulkan boundary tracer for DXVK.
 *
 * Why this exists: on the system Vulkan driver DXVK 3.1.1 can create a
 * device and a swapchain yet leave the screen black.  The failure is below
 * the D3D9 layer, so the useful evidence is what DXVK asks the selected ICD
 * to do: instance/device extensions and features, Android surface, swapchain
 * format/present mode, acquire/present results, and whether the frame that is
 * presented actually recorded a clear or a draw.
 *
 * The trace is installed by wrapping the vkGetInstanceProcAddr pointer in
 * wc3_dxvk_prepare().  DXVK resolves every Vulkan entry through that pointer,
 * including vkGetDeviceProcAddr, so both instance and device calls are
 * visible without editing the vendored DXVK trees.  Unknown entries are
 * forwarded unchanged.
 *
 * Guard: WC3_DEBUG && WC3_DEBUG_DXVK_DEBUG.  Release compiles only the
 * pass-through stubs at the bottom, so no trace string reaches the product.
 */
#define VK_USE_PLATFORM_ANDROID_KHR 1

#include "dxvk_trace.h"
#include "wc3_debug.h"

#if WC3_DEBUG && WC3_DEBUG_DXVK_DEBUG

#include <android/log.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

/* Maintenance 5/6 constants are not in every NDK Vulkan header revision.
 * runtime/platform/android/vulkan_driver_android.c uses the same numeric
 * values, so keep this in lockstep. */
#ifndef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR \
    ((VkStructureType)1000470000)
#endif
#ifndef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR \
    ((VkStructureType)1000545000)
#endif

namespace {

/* ---------------------------------------------------------------- log ---- */

void trace_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    __android_log_print(ANDROID_LOG_INFO, "DXVK_DEBUG", "%s", buf);
    fprintf(stderr, "[DXVK_DEBUG] %s\n", buf);
    fflush(stderr);
}

/* ------------------------------------------------------------- strings -- */

const char *result_name(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
        return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:
        return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
    case VK_ERROR_VALIDATION_FAILED_EXT:
        return "VK_ERROR_VALIDATION_FAILED_EXT";
    case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV";
    default: return "VK_RESULT_OTHER";
    }
}

const char *device_type_name(VkPhysicalDeviceType type)
{
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_OTHER:
        return "OTHER";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return "INTEGRATED_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return "DISCRETE_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return "VIRTUAL_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return "CPU";
    default:
        return "UNKNOWN";
    }
}

const char *format_name(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_UNDEFINED: return "UNDEFINED";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return "A8B8G8R8_UNORM_PACK32";
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return "A8B8G8R8_SRGB_PACK32";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return "A2B10G10R10_UNORM_PACK32";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
    case VK_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
    case VK_FORMAT_R5G6B5_UNORM_PACK16: return "R5G6B5_UNORM_PACK16";
    case VK_FORMAT_D16_UNORM: return "D16_UNORM";
    case VK_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case VK_FORMAT_D32_SFLOAT: return "D32_SFLOAT";
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32_SFLOAT_S8_UINT";
    default: return "FORMAT_OTHER";
    }
}

const char *present_mode_name(VkPresentModeKHR mode)
{
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    case VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR:
        return "SHARED_DEMAND_REFRESH";
    case VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR:
        return "SHARED_CONTINUOUS_REFRESH";
    default: return "PRESENT_MODE_OTHER";
    }
}

const char *color_space_name(VkColorSpaceKHR space)
{
    switch (space) {
    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR: return "SRGB_NONLINEAR";
    case VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT:
        return "DISPLAY_P3_NONLINEAR";
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
        return "EXTENDED_SRGB_LINEAR";
    case VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT: return "DISPLAY_P3_LINEAR";
    case VK_COLOR_SPACE_DCI_P3_NONLINEAR_EXT: return "DCI_P3_NONLINEAR";
    case VK_COLOR_SPACE_BT709_LINEAR_EXT: return "BT709_LINEAR";
    case VK_COLOR_SPACE_BT709_NONLINEAR_EXT: return "BT709_NONLINEAR";
    case VK_COLOR_SPACE_BT2020_LINEAR_EXT: return "BT2020_LINEAR";
    case VK_COLOR_SPACE_HDR10_ST2084_EXT: return "HDR10_ST2084";
    case VK_COLOR_SPACE_HDR10_HLG_EXT: return "HDR10_HLG";
    case VK_COLOR_SPACE_ADOBERGB_LINEAR_EXT: return "ADOBERGB_LINEAR";
    case VK_COLOR_SPACE_ADOBERGB_NONLINEAR_EXT: return "ADOBERGB_NONLINEAR";
    case VK_COLOR_SPACE_PASS_THROUGH_EXT: return "PASS_THROUGH";
    case VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT:
        return "EXTENDED_SRGB_NONLINEAR";
    default: return "COLOR_SPACE_OTHER";
    }
}

const char *layout_name(VkImageLayout layout)
{
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED: return "UNDEFINED";
    case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return "COLOR_ATTACHMENT_OPTIMAL";
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return "DEPTH_STENCIL_ATTACHMENT_OPTIMAL";
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        return "DEPTH_STENCIL_READ_ONLY_OPTIMAL";
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return "SHADER_READ_ONLY_OPTIMAL";
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "TRANSFER_SRC_OPTIMAL";
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "TRANSFER_DST_OPTIMAL";
    case VK_IMAGE_LAYOUT_PREINITIALIZED: return "PREINITIALIZED";
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return "PRESENT_SRC_KHR";
    case VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR: return "SHARED_PRESENT_KHR";
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
        return "DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL";
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
        return "DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL";
    case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL_KHR:
        return "ATTACHMENT_OPTIMAL";
    case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL_KHR: return "READ_ONLY_OPTIMAL";
    default: return "LAYOUT_OTHER";
    }
}

const char *load_op_name(VkAttachmentLoadOp op)
{
    switch (op) {
    case VK_ATTACHMENT_LOAD_OP_LOAD: return "LOAD";
    case VK_ATTACHMENT_LOAD_OP_CLEAR: return "CLEAR";
    case VK_ATTACHMENT_LOAD_OP_DONT_CARE: return "DONT_CARE";
    case VK_ATTACHMENT_LOAD_OP_NONE_EXT: return "NONE";
    default: return "LOAD_OP_OTHER";
    }
}

const char *store_op_name(VkAttachmentStoreOp op)
{
    switch (op) {
    case VK_ATTACHMENT_STORE_OP_STORE: return "STORE";
    case VK_ATTACHMENT_STORE_OP_DONT_CARE: return "DONT_CARE";
    case VK_ATTACHMENT_STORE_OP_NONE_EXT: return "NONE";
    default: return "STORE_OP_OTHER";
    }
}

const char *sharing_mode_name(VkSharingMode mode)
{
    switch (mode) {
    case VK_SHARING_MODE_EXCLUSIVE: return "EXCLUSIVE";
    case VK_SHARING_MODE_CONCURRENT: return "CONCURRENT";
    default: return "SHARING_OTHER";
    }
}

void append_flag(char *buf, size_t cap, const char *flag)
{
    size_t len = strlen(buf);

    if (len >= cap - 1)
        return;
    if (len) {
        buf[len++] = '|';
        buf[len] = 0;
    }
    snprintf(buf + len, cap - len, "%s", flag);
}

void usage_string(VkImageUsageFlags usage, char *buf, size_t cap)
{
    buf[0] = 0;
    if (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        append_flag(buf, cap, "TRANSFER_SRC");
    if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        append_flag(buf, cap, "TRANSFER_DST");
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT)
        append_flag(buf, cap, "SAMPLED");
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT)
        append_flag(buf, cap, "STORAGE");
    if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        append_flag(buf, cap, "COLOR_ATTACHMENT");
    if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
        append_flag(buf, cap, "DEPTH_STENCIL_ATTACHMENT");
    if (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)
        append_flag(buf, cap, "TRANSIENT_ATTACHMENT");
    if (usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)
        append_flag(buf, cap, "INPUT_ATTACHMENT");
    if (!buf[0])
        snprintf(buf, cap, "0");
}

void queue_flags_string(VkQueueFlags flags, char *buf, size_t cap)
{
    buf[0] = 0;
    if (flags & VK_QUEUE_GRAPHICS_BIT)
        append_flag(buf, cap, "GRAPHICS");
    if (flags & VK_QUEUE_COMPUTE_BIT)
        append_flag(buf, cap, "COMPUTE");
    if (flags & VK_QUEUE_TRANSFER_BIT)
        append_flag(buf, cap, "TRANSFER");
    if (flags & VK_QUEUE_SPARSE_BINDING_BIT)
        append_flag(buf, cap, "SPARSE");
    if (flags & VK_QUEUE_PROTECTED_BIT)
        append_flag(buf, cap, "PROTECTED");
    if (!buf[0])
        snprintf(buf, cap, "0");
}

void composite_alpha_string(VkCompositeAlphaFlagsKHR alpha, char *buf,
                            size_t cap)
{
    buf[0] = 0;
    if (alpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
        append_flag(buf, cap, "OPAQUE");
    if (alpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
        append_flag(buf, cap, "PRE_MULTIPLIED");
    if (alpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
        append_flag(buf, cap, "POST_MULTIPLIED");
    if (alpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
        append_flag(buf, cap, "INHERIT");
    if (!buf[0])
        snprintf(buf, cap, "0");
}

void transform_string(VkSurfaceTransformFlagsKHR transform, char *buf,
                      size_t cap)
{
    buf[0] = 0;
    if (transform & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
        append_flag(buf, cap, "IDENTITY");
    if (transform & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR)
        append_flag(buf, cap, "ROTATE_90");
    if (transform & VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR)
        append_flag(buf, cap, "ROTATE_180");
    if (transform & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)
        append_flag(buf, cap, "ROTATE_270");
    if (transform & VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_BIT_KHR)
        append_flag(buf, cap, "H_MIRROR");
    if (transform & VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR)
        append_flag(buf, cap, "H_MIRROR_90");
    if (transform & VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_180_BIT_KHR)
        append_flag(buf, cap, "H_MIRROR_180");
    if (transform & VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR)
        append_flag(buf, cap, "H_MIRROR_270");
    if (!buf[0])
        snprintf(buf, cap, "0");
}

/* -------------------------------------------------------------- state ---- */

std::atomic<uint64_t> g_present_count{0};
std::atomic<uint64_t> g_acquire_count{0};
std::atomic<uint64_t> g_submit_count{0};
std::atomic<uint64_t> g_draw_count{0};
std::atomic<uint64_t> g_clear_count{0};
std::atomic<uint64_t> g_pass_count{0};
std::atomic<uint32_t> g_current_image{0xFFFFFFFFu};
std::atomic<uint32_t> g_suboptimal_log_count{0};

std::atomic<uint32_t> g_draw_log_budget{8};
std::atomic<uint32_t> g_pass_log_budget{16};
std::atomic<uint32_t> g_clear_log_budget{8};
std::atomic<uint32_t> g_barrier_log_budget{8};

std::atomic<bool> g_instance_extensions_logged{false};
std::atomic<bool> g_instance_layers_logged{false};

std::mutex g_state_mutex;
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical_device = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkSurfaceKHR g_surface = VK_NULL_HANDLE;
VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
VkFormat g_swapchain_format = VK_FORMAT_UNDEFINED;
VkColorSpaceKHR g_swapchain_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
VkExtent2D g_swapchain_extent = {0, 0};
VkPresentModeKHR g_swapchain_present_mode = VK_PRESENT_MODE_FIFO_KHR;
uint32_t g_swapchain_min_images = 0;
uint32_t g_swapchain_image_total = 0;
VkImage g_swapchain_images[8] = {};

struct ViewRecord {
    VkImageView view;
    VkImage image;
    VkFormat format;
};
ViewRecord g_views[16] = {};

struct FramebufferRecord {
    VkFramebuffer framebuffer;
    bool has_swapchain;
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t attachments;
};
FramebufferRecord g_framebuffers[16] = {};

VkPhysicalDevice g_props_logged[4] = {};
VkPhysicalDevice g_features_logged[4] = {};
VkPhysicalDevice g_queue_logged[4] = {};
VkPhysicalDevice g_memory_logged[4] = {};
VkPhysicalDevice g_ext_logged[4] = {};

PFN_vkGetInstanceProcAddr g_real_gipa = nullptr;
PFN_vkGetDeviceProcAddr g_real_gdpa = nullptr;

char g_version[16] = "?";

bool mark_device(VkPhysicalDevice device, VkPhysicalDevice *slots,
                 size_t slot_count)
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    size_t i;

    for (i = 0; i < slot_count; ++i) {
        if (slots[i] == device)
            return false;
    }
    for (i = 0; i < slot_count; ++i) {
        if (slots[i] == VK_NULL_HANDLE) {
            slots[i] = device;
            return true;
        }
    }
    return false;
}

bool is_swapchain_image_locked(VkImage image)
{
    uint32_t i;

    if (!image)
        return false;
    for (i = 0; i < g_swapchain_image_total && i < 8; ++i) {
        if (g_swapchain_images[i] == image)
            return true;
    }
    return false;
}

bool view_is_swapchain_locked(VkImageView view)
{
    uint32_t i;

    if (!view)
        return false;
    for (i = 0; i < 16; ++i) {
        if (g_views[i].view == view)
            return true;
    }
    return false;
}

bool is_swapchain_image(VkImage image)
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    return is_swapchain_image_locked(image);
}

void remember_swapchain_locked(VkSwapchainKHR swapchain,
                               const VkSwapchainCreateInfoKHR *info)
{
    g_swapchain = swapchain;
    g_swapchain_format = info->imageFormat;
    g_swapchain_color_space = info->imageColorSpace;
    g_swapchain_extent = info->imageExtent;
    g_swapchain_present_mode = info->presentMode;
    g_swapchain_min_images = info->minImageCount;
    g_swapchain_image_total = 0;
    memset(g_swapchain_images, 0, sizeof g_swapchain_images);
    memset(g_views, 0, sizeof g_views);
    memset(g_framebuffers, 0, sizeof g_framebuffers);
}

bool take_budget(std::atomic<uint32_t> &budget)
{
    uint32_t old = budget.load(std::memory_order_relaxed);

    while (old) {
        if (budget.compare_exchange_weak(old, old - 1,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed))
            return true;
    }
    return false;
}

bool trace_detail_frame()
{
    uint64_t present = g_present_count.load(std::memory_order_relaxed);
    return present < 5 || (present % 300) == 0;
}

/* ---------------------------------------------------------- features ----- */

void log_core_features(const char *prefix, const VkPhysicalDeviceFeatures &f)
{
    trace_log("%s core: geometry=%u tess=%u dualSrc=%u depthClamp=%u "
              "fillNonSolid=%u fragmentStoresAtomics=%u fullDrawIndex=%u "
              "imageCubeArray=%u independentBlend=%u multiDrawIndirect=%u "
              "multiViewport=%u occlusionQueryPrecise=%u robustBuffer=%u "
              "sampleRateShading=%u samplerAnisotropy=%u shaderClip=%u "
              "shaderCull=%u shaderImageGatherExtended=%u shaderInt16=%u "
              "shaderInt64=%u sampledImageArrayDynamic=%u textureCompressionBC=%u",
              prefix, f.geometryShader, f.tessellationShader, f.dualSrcBlend,
              f.depthClamp, f.fillModeNonSolid, f.fragmentStoresAndAtomics,
              f.fullDrawIndexUint32, f.imageCubeArray, f.independentBlend,
              f.multiDrawIndirect, f.multiViewport, f.occlusionQueryPrecise,
              f.robustBufferAccess, f.sampleRateShading, f.samplerAnisotropy,
              f.shaderClipDistance, f.shaderCullDistance,
              f.shaderImageGatherExtended, f.shaderInt16, f.shaderInt64,
              f.shaderSampledImageArrayDynamicIndexing,
              f.textureCompressionBC);
}

void log_vulkan11_features(const VkPhysicalDeviceVulkan11Features &f)
{
    trace_log("Vk11: storageBuffer16=%u uniformStorageBuffer16=%u "
              "storagePush16=%u storageInputOutput16=%u multiview=%u "
              "multiviewGeometry=%u multiviewTessellation=%u "
              "variablePointersStorage=%u variablePointers=%u "
              "shaderDrawParameters=%u",
              f.storageBuffer16BitAccess,
              f.uniformAndStorageBuffer16BitAccess,
              f.storagePushConstant16, f.storageInputOutput16, f.multiview,
              f.multiviewGeometryShader, f.multiviewTessellationShader,
              f.variablePointersStorageBuffer, f.variablePointers,
              f.shaderDrawParameters);
}

void log_vulkan12_features(const VkPhysicalDeviceVulkan12Features &f)
{
    trace_log("Vk12a: samplerMirrorClamp=%u drawIndirectCount=%u "
              "storageBuffer8=%u uniformStorageBuffer8=%u storagePush8=%u "
              "shaderBufferInt64Atomics=%u shaderSharedInt64Atomics=%u "
              "shaderFloat16=%u shaderInt8=%u descriptorIndexing=%u "
              "runtimeDescriptorArray=%u samplerFilterMinmax=%u "
              "scalarBlockLayout=%u imagelessFramebuffer=%u "
              "uniformBufferStandardLayout=%u separateDepthStencilLayouts=%u "
              "hostQueryReset=%u timelineSemaphore=%u bufferDeviceAddress=%u",
              f.samplerMirrorClampToEdge, f.drawIndirectCount,
              f.storageBuffer8BitAccess,
              f.uniformAndStorageBuffer8BitAccess, f.storagePushConstant8,
              f.shaderBufferInt64Atomics, f.shaderSharedInt64Atomics,
              f.shaderFloat16, f.shaderInt8, f.descriptorIndexing,
              f.runtimeDescriptorArray, f.samplerFilterMinmax,
              f.scalarBlockLayout, f.imagelessFramebuffer,
              f.uniformBufferStandardLayout, f.separateDepthStencilLayouts,
              f.hostQueryReset, f.timelineSemaphore, f.bufferDeviceAddress);
    trace_log("Vk12b: descriptorBindingUniformBufferUpdateAfterBind=%u "
              "descriptorBindingSampledImageUpdateAfterBind=%u "
              "descriptorBindingStorageImageUpdateAfterBind=%u "
              "descriptorBindingStorageBufferUpdateAfterBind=%u "
              "descriptorBindingUpdateUnusedWhilePending=%u "
              "descriptorBindingPartiallyBound=%u "
              "descriptorBindingVariableDescriptorCount=%u "
              "vulkanMemoryModel=%u vulkanMemoryModelDeviceScope=%u "
              "shaderOutputViewportIndex=%u shaderOutputLayer=%u",
              f.descriptorBindingUniformBufferUpdateAfterBind,
              f.descriptorBindingSampledImageUpdateAfterBind,
              f.descriptorBindingStorageImageUpdateAfterBind,
              f.descriptorBindingStorageBufferUpdateAfterBind,
              f.descriptorBindingUpdateUnusedWhilePending,
              f.descriptorBindingPartiallyBound,
              f.descriptorBindingVariableDescriptorCount,
              f.vulkanMemoryModel, f.vulkanMemoryModelDeviceScope,
              f.shaderOutputViewportIndex, f.shaderOutputLayer);
}

void log_vulkan13_features(const VkPhysicalDeviceVulkan13Features &f)
{
    trace_log("Vk13: robustImageAccess=%u inlineUniformBlock=%u "
              "descriptorBindingInlineUniformBlockUpdateAfterBind=%u "
              "pipelineCreationCacheControl=%u privateData=%u "
              "shaderDemoteToHelper=%u shaderTerminateInvocation=%u "
              "subgroupSizeControl=%u computeFullSubgroups=%u "
              "synchronization2=%u textureCompressionASTC_HDR=%u "
              "shaderZeroInitializeWorkgroupMemory=%u dynamicRendering=%u "
              "shaderIntegerDotProduct=%u maintenance4=%u",
              f.robustImageAccess, f.inlineUniformBlock,
              f.descriptorBindingInlineUniformBlockUpdateAfterBind,
              f.pipelineCreationCacheControl, f.privateData,
              f.shaderDemoteToHelperInvocation, f.shaderTerminateInvocation,
              f.subgroupSizeControl, f.computeFullSubgroups,
              f.synchronization2, f.textureCompressionASTC_HDR,
              f.shaderZeroInitializeWorkgroupMemory, f.dynamicRendering,
              f.shaderIntegerDotProduct, f.maintenance4);
}

void log_descriptor_indexing_features(
    const VkPhysicalDeviceDescriptorIndexingFeatures &f)
{
    trace_log("descriptorIndexing: shaderInputAttachmentArrayDynamic=%u "
              "shaderUniformTexelBufferArrayDynamic=%u "
              "shaderStorageTexelBufferArrayDynamic=%u "
              "shaderUniformBufferArrayNonUniform=%u "
              "shaderSampledImageArrayNonUniform=%u "
              "shaderStorageBufferArrayNonUniform=%u "
              "shaderStorageImageArrayNonUniform=%u "
              "shaderInputAttachmentArrayNonUniform=%u "
              "shaderUniformTexelBufferArrayNonUniform=%u "
              "shaderStorageTexelBufferArrayNonUniform=%u "
              "bindingUniformBufferUpdateAfterBind=%u "
              "bindingSampledImageUpdateAfterBind=%u "
              "bindingStorageImageUpdateAfterBind=%u "
              "bindingStorageBufferUpdateAfterBind=%u "
              "bindingUniformTexelBufferUpdateAfterBind=%u "
              "bindingStorageTexelBufferUpdateAfterBind=%u "
              "bindingUpdateUnusedWhilePending=%u "
              "bindingPartiallyBound=%u bindingVariableDescriptorCount=%u "
              "runtimeDescriptorArray=%u",
              f.shaderInputAttachmentArrayDynamicIndexing,
              f.shaderUniformTexelBufferArrayDynamicIndexing,
              f.shaderStorageTexelBufferArrayDynamicIndexing,
              f.shaderUniformBufferArrayNonUniformIndexing,
              f.shaderSampledImageArrayNonUniformIndexing,
              f.shaderStorageBufferArrayNonUniformIndexing,
              f.shaderStorageImageArrayNonUniformIndexing,
              f.shaderInputAttachmentArrayNonUniformIndexing,
              f.shaderUniformTexelBufferArrayNonUniformIndexing,
              f.shaderStorageTexelBufferArrayNonUniformIndexing,
              f.descriptorBindingUniformBufferUpdateAfterBind,
              f.descriptorBindingSampledImageUpdateAfterBind,
              f.descriptorBindingStorageImageUpdateAfterBind,
              f.descriptorBindingStorageBufferUpdateAfterBind,
              f.descriptorBindingUniformTexelBufferUpdateAfterBind,
              f.descriptorBindingStorageTexelBufferUpdateAfterBind,
              f.descriptorBindingUpdateUnusedWhilePending,
              f.descriptorBindingPartiallyBound,
              f.descriptorBindingVariableDescriptorCount,
              f.runtimeDescriptorArray);
}

void log_feature_chain(const char *prefix, const void *pNext)
{
    const VkBaseInStructure *base =
        reinterpret_cast<const VkBaseInStructure *>(pNext);

    while (base) {
        switch (base->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
            log_vulkan11_features(*reinterpret_cast<
                const VkPhysicalDeviceVulkan11Features *>(base));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
            log_vulkan12_features(*reinterpret_cast<
                const VkPhysicalDeviceVulkan12Features *>(base));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
            log_vulkan13_features(*reinterpret_cast<
                const VkPhysicalDeviceVulkan13Features *>(base));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
            log_descriptor_indexing_features(*reinterpret_cast<
                const VkPhysicalDeviceDescriptorIndexingFeatures *>(base));
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES: {
            const auto *f = reinterpret_cast<
                const VkPhysicalDeviceTimelineSemaphoreFeatures *>(base);
            trace_log("%s timelineSemaphore=%u", prefix, f->timelineSemaphore);
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES: {
            const auto *f = reinterpret_cast<
                const VkPhysicalDeviceBufferDeviceAddressFeatures *>(base);
            trace_log("%s bufferDeviceAddress=%u captureReplay=%u "
                      "multiDevice=%u", prefix, f->bufferDeviceAddress,
                      f->bufferDeviceAddressCaptureReplay,
                      f->bufferDeviceAddressMultiDevice);
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES: {
            const auto *f = reinterpret_cast<
                const VkPhysicalDeviceDynamicRenderingFeatures *>(base);
            trace_log("%s dynamicRendering=%u", prefix, f->dynamicRendering);
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES: {
            const auto *f = reinterpret_cast<
                const VkPhysicalDeviceSynchronization2Features *>(base);
            trace_log("%s synchronization2=%u", prefix, f->synchronization2);
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR: {
            const auto *f = reinterpret_cast<
                const VkPhysicalDeviceMaintenance5FeaturesKHR *>(base);
            trace_log("%s maintenance5=%u", prefix, f->maintenance5);
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR: {
            const auto *f = reinterpret_cast<
                const VkPhysicalDeviceMaintenance6FeaturesKHR *>(base);
            trace_log("%s maintenance6=%u", prefix, f->maintenance6);
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: {
            const auto *f = reinterpret_cast<
                const VkPhysicalDeviceRobustness2FeaturesEXT *>(base);
            trace_log("%s robustBufferAccess2=%u robustImageAccess2=%u "
                      "nullDescriptor=%u", prefix, f->robustBufferAccess2,
                      f->robustImageAccess2, f->nullDescriptor);
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT: {
            const auto *f = reinterpret_cast<
                const VkPhysicalDeviceDepthClipEnableFeaturesEXT *>(base);
            trace_log("%s depthClipEnable=%u", prefix, f->depthClipEnable);
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES: {
            const auto *f = reinterpret_cast<
                const VkPhysicalDevice8BitStorageFeatures *>(base);
            trace_log("%s storageBuffer8BitAccess=%u "
                      "uniformAndStorageBuffer8BitAccess=%u "
                      "storagePushConstant8=%u", prefix,
                      f->storageBuffer8BitAccess,
                      f->uniformAndStorageBuffer8BitAccess,
                      f->storagePushConstant8);
            break;
        }
        default:
            break;
        }
        base = base->pNext;
    }
}

/* ---------------------------------------------------------- extension ---- */

void log_extension_list(const char *kind, uint32_t count,
                        const VkExtensionProperties *props)
{
    uint32_t i;
    uint32_t limit = count > 256 ? 256 : count;

    trace_log("%s extension count=%u%s", kind, count,
              count > limit ? " (showing first 256)" : "");
    for (i = 0; i < limit; ++i) {
        trace_log("  %s extension[%u] %s spec=%u", kind, i,
                  props[i].extensionName, props[i].specVersion);
    }
}

bool has_extension(uint32_t count, const VkExtensionProperties *props,
                   const char *name)
{
    uint32_t i;

    for (i = 0; i < count; ++i) {
        if (strcmp(props[i].extensionName, name) == 0)
            return true;
    }
    return false;
}

void log_missing_instance_extensions(const VkInstanceCreateInfo *info)
{
    PFN_vkEnumerateInstanceExtensionProperties enumerate;
    std::vector<VkExtensionProperties> props;
    uint32_t count = 0;
    uint32_t i;
    VkResult r;

    if (!info || !info->enabledExtensionCount || !g_real_gipa)
        return;

    enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        g_real_gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
    if (!enumerate)
        return;

    r = enumerate(nullptr, &count, nullptr);
    if (r != VK_SUCCESS || !count)
        return;
    props.resize(count);
    r = enumerate(nullptr, &count, props.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE)
        return;
    props.resize(count);

    for (i = 0; i < info->enabledExtensionCount; ++i) {
        if (!has_extension(count, props.data(), info->ppEnabledExtensionNames[i]))
            trace_log("requested instance extension not advertised: %s",
                      info->ppEnabledExtensionNames[i]);
    }
}

void log_missing_device_extensions(VkPhysicalDevice device,
                                   const VkDeviceCreateInfo *info)
{
    PFN_vkEnumerateDeviceExtensionProperties enumerate;
    std::vector<VkExtensionProperties> props;
    uint32_t count = 0;
    uint32_t i;
    VkResult r;

    if (!info || !info->enabledExtensionCount || !g_real_gipa)
        return;

    enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        g_real_gipa(g_instance, "vkEnumerateDeviceExtensionProperties"));
    if (!enumerate)
        return;

    r = enumerate(device, nullptr, &count, nullptr);
    if (r != VK_SUCCESS || !count)
        return;
    props.resize(count);
    r = enumerate(device, nullptr, &count, props.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE)
        return;
    props.resize(count);

    for (i = 0; i < info->enabledExtensionCount; ++i) {
        if (!has_extension(count, props.data(), info->ppEnabledExtensionNames[i]))
            trace_log("requested device extension not advertised: %s",
                      info->ppEnabledExtensionNames[i]);
    }
}

/* ------------------------------------------------------- surface caps ---- */

void log_surface_caps(const VkSurfaceCapabilitiesKHR &caps)
{
    char usage[256];
    char composite[128];
    char transforms[256];
    char current[64];

    usage_string(caps.supportedUsageFlags, usage, sizeof usage);
    composite_alpha_string(caps.supportedCompositeAlpha, composite,
                           sizeof composite);
    transform_string(caps.supportedTransforms, transforms, sizeof transforms);
    transform_string(1u << caps.currentTransform, current, sizeof current);

    trace_log("surface caps: minImages=%u maxImages=%u currentExtent=%ux%u "
              "minExtent=%ux%u maxExtent=%ux%u maxLayers=%u "
              "currentTransform=%s transforms=%s compositeAlpha=%s usage=%s",
              caps.minImageCount, caps.maxImageCount,
              caps.currentExtent.width, caps.currentExtent.height,
              caps.minImageExtent.width, caps.minImageExtent.height,
              caps.maxImageExtent.width, caps.maxImageExtent.height,
              caps.maxImageArrayLayers, current, transforms, composite,
              usage);
}

void log_surface_formats(uint32_t count, const VkSurfaceFormatKHR *formats)
{
    uint32_t i;

    trace_log("surface formats count=%u", count);
    for (i = 0; i < count; ++i) {
        trace_log("  surface format[%u] format=%d (%s) colorSpace=%d (%s)",
                  i, formats[i].format, format_name(formats[i].format),
                  formats[i].colorSpace,
                  color_space_name(formats[i].colorSpace));
    }
}

void log_present_modes(uint32_t count, const VkPresentModeKHR *modes)
{
    uint32_t i;

    trace_log("surface present modes count=%u", count);
    for (i = 0; i < count; ++i) {
        trace_log("  present mode[%u] %d (%s)", i, modes[i],
                  present_mode_name(modes[i]));
    }
}

void log_queue_families(uint32_t count, const VkQueueFamilyProperties *props)
{
    uint32_t i;

    trace_log("queue families count=%u", count);
    for (i = 0; i < count; ++i) {
        char flags[128];

        queue_flags_string(props[i].queueFlags, flags, sizeof flags);
        trace_log("  queue family[%u] count=%u flags=0x%x (%s) "
                  "timestampValidBits=%u granularity=%ux%ux%u", i,
                  props[i].queueCount, props[i].queueFlags, flags,
                  props[i].timestampValidBits,
                  props[i].minImageTransferGranularity.width,
                  props[i].minImageTransferGranularity.height,
                  props[i].minImageTransferGranularity.depth);
    }
}

void log_memory_properties(const VkPhysicalDeviceMemoryProperties &mem)
{
    uint32_t i;

    trace_log("memory heaps=%u types=%u", mem.memoryHeapCount,
              mem.memoryTypeCount);
    for (i = 0; i < mem.memoryHeapCount; ++i) {
        trace_log("  heap[%u] size=%llu flags=0x%x%s", i,
                  (unsigned long long)mem.memoryHeaps[i].size,
                  mem.memoryHeaps[i].flags,
                  (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                      ? " DEVICE_LOCAL" : "");
    }
    for (i = 0; i < mem.memoryTypeCount; ++i) {
        trace_log("  memoryType[%u] heap=%u flags=0x%x", i,
                  mem.memoryTypes[i].heapIndex,
                  mem.memoryTypes[i].propertyFlags);
    }
}

/* -------------------------------------------------------- property log --- */

void log_device_properties(VkPhysicalDevice device,
                           const VkPhysicalDeviceProperties &p)
{
    char name[256];
    size_t n = strnlen(p.deviceName, sizeof p.deviceName);

    if (n >= sizeof name)
        n = sizeof name - 1;
    memcpy(name, p.deviceName, n);
    name[n] = 0;

    trace_log("physical device %p: name='%s' type=%s api=%u.%u.%u "
              "driver=0x%08x vendor=0x%04x device=0x%04x", (void *)device,
              name, device_type_name(p.deviceType),
              VK_API_VERSION_MAJOR(p.apiVersion),
              VK_API_VERSION_MINOR(p.apiVersion),
              VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion,
              p.vendorID, p.deviceID);
    trace_log("limits: maxImageDimension2D=%u maxImageDimension3D=%u "
              "maxColorAttachments=%u maxFramebufferWidth=%u "
              "maxFramebufferHeight=%u maxViewportDimensions=%u,%u "
              "maxDescriptorSetSampledImages=%u maxMemoryAllocationCount=%u",
              p.limits.maxImageDimension2D, p.limits.maxImageDimension3D,
              p.limits.maxColorAttachments, p.limits.maxFramebufferWidth,
              p.limits.maxFramebufferHeight,
              p.limits.maxViewportDimensions[0],
              p.limits.maxViewportDimensions[1],
              p.limits.maxDescriptorSetSampledImages,
              p.limits.maxMemoryAllocationCount);
}

// ==== WRAPPERS ====

PFN_vkEnumerateInstanceExtensionProperties
    g_real_vkEnumerateInstanceExtensionProperties = nullptr;
PFN_vkEnumerateInstanceLayerProperties
    g_real_vkEnumerateInstanceLayerProperties = nullptr;
PFN_vkCreateInstance g_real_vkCreateInstance = nullptr;
PFN_vkDestroyInstance g_real_vkDestroyInstance = nullptr;
PFN_vkEnumeratePhysicalDevices g_real_vkEnumeratePhysicalDevices = nullptr;
PFN_vkGetPhysicalDeviceProperties g_real_vkGetPhysicalDeviceProperties = nullptr;
PFN_vkGetPhysicalDeviceProperties2 g_real_vkGetPhysicalDeviceProperties2 = nullptr;
PFN_vkGetPhysicalDeviceFeatures g_real_vkGetPhysicalDeviceFeatures = nullptr;
PFN_vkGetPhysicalDeviceFeatures2 g_real_vkGetPhysicalDeviceFeatures2 = nullptr;
PFN_vkGetPhysicalDeviceQueueFamilyProperties
    g_real_vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties
    g_real_vkGetPhysicalDeviceMemoryProperties = nullptr;
PFN_vkEnumerateDeviceExtensionProperties
    g_real_vkEnumerateDeviceExtensionProperties = nullptr;
PFN_vkGetPhysicalDeviceSurfaceSupportKHR
    g_real_vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR
    g_real_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceFormatsKHR
    g_real_vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfacePresentModesKHR
    g_real_vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;
#ifdef VK_KHR_get_surface_capabilities2
PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR
    g_real_vkGetPhysicalDeviceSurfaceCapabilities2KHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceFormats2KHR
    g_real_vkGetPhysicalDeviceSurfaceFormats2KHR = nullptr;
#endif
PFN_vkCreateAndroidSurfaceKHR g_real_vkCreateAndroidSurfaceKHR = nullptr;
PFN_vkDestroySurfaceKHR g_real_vkDestroySurfaceKHR = nullptr;

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkEnumerateInstanceExtensionProperties(
    const char *pLayerName, uint32_t *pPropertyCount,
    VkExtensionProperties *pProperties)
{
    VkResult r = g_real_vkEnumerateInstanceExtensionProperties(
        pLayerName, pPropertyCount, pProperties);

    if (pProperties && pPropertyCount && *pPropertyCount &&
        !g_instance_extensions_logged.exchange(true))
        log_extension_list("instance", *pPropertyCount, pProperties);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE)
        trace_log("vkEnumerateInstanceExtensionProperties result=%s",
                  result_name(r));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkEnumerateInstanceLayerProperties(
    uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
    VkResult r = g_real_vkEnumerateInstanceLayerProperties(
        pPropertyCount, pProperties);

    if (pProperties && pPropertyCount && *pPropertyCount &&
        !g_instance_layers_logged.exchange(true)) {
        uint32_t i;
        trace_log("instance layers count=%u", *pPropertyCount);
        for (i = 0; i < *pPropertyCount; ++i) {
            trace_log("  layer[%u] %s spec=%u impl=%u", i,
                      pProperties[i].layerName,
                      pProperties[i].specVersion,
                      pProperties[i].implementationVersion);
        }
    }
    if (r != VK_SUCCESS && r != VK_INCOMPLETE)
        trace_log("vkEnumerateInstanceLayerProperties result=%s",
                  result_name(r));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkCreateInstance(
    const VkInstanceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
    uint32_t i;

    if (pCreateInfo) {
        const VkApplicationInfo *app = pCreateInfo->pApplicationInfo;
        trace_log("vkCreateInstance: app='%s' engine='%s' api=%u.%u.%u "
                  "extensions=%u layers=%u", app && app->pApplicationName
                      ? app->pApplicationName : "",
                  app && app->pEngineName ? app->pEngineName : "",
                  app ? VK_API_VERSION_MAJOR(app->apiVersion) : 0,
                  app ? VK_API_VERSION_MINOR(app->apiVersion) : 0,
                  app ? VK_API_VERSION_PATCH(app->apiVersion) : 0,
                  pCreateInfo->enabledExtensionCount,
                  pCreateInfo->enabledLayerCount);
        for (i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
            trace_log("  requested instance extension: %s",
                      pCreateInfo->ppEnabledExtensionNames[i]);
        for (i = 0; i < pCreateInfo->enabledLayerCount; ++i)
            trace_log("  requested instance layer: %s",
                      pCreateInfo->ppEnabledLayerNames[i]);
        log_missing_instance_extensions(pCreateInfo);
    } else {
        trace_log("vkCreateInstance: pCreateInfo=null");
    }

    VkResult r = g_real_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
    trace_log("vkCreateInstance result=%s instance=%p", result_name(r),
              pInstance && r == VK_SUCCESS ? (void *)*pInstance : nullptr);
    if (r == VK_SUCCESS && pInstance) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_instance = *pInstance;
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL trace_vkDestroyInstance(
    VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
    trace_log("vkDestroyInstance instance=%p", (void *)instance);
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (g_instance == instance)
            g_instance = VK_NULL_HANDLE;
    }
    g_real_vkDestroyInstance(instance, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkEnumeratePhysicalDevices(
    VkInstance instance, uint32_t *pPhysicalDeviceCount,
    VkPhysicalDevice *pPhysicalDevices)
{
    VkResult r = g_real_vkEnumeratePhysicalDevices(
        instance, pPhysicalDeviceCount, pPhysicalDevices);

    if (pPhysicalDevices && pPhysicalDeviceCount)
        trace_log("physical devices count=%u result=%s",
                  *pPhysicalDeviceCount, result_name(r));
    else if (r != VK_SUCCESS)
        trace_log("physical device count query result=%s", result_name(r));
    return r;
}

static VKAPI_ATTR void VKAPI_CALL trace_vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties *pProperties)
{
    g_real_vkGetPhysicalDeviceProperties(physicalDevice, pProperties);
    if (pProperties &&
        mark_device(physicalDevice, g_props_logged, 4))
        log_device_properties(physicalDevice, *pProperties);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *pProperties)
{
    g_real_vkGetPhysicalDeviceProperties2(physicalDevice, pProperties);
    if (!pProperties)
        return;
    if (mark_device(physicalDevice, g_props_logged, 4))
        log_device_properties(physicalDevice, pProperties->properties);
#ifdef VK_KHR_driver_properties
    {
        const VkBaseInStructure *base =
            reinterpret_cast<const VkBaseInStructure *>(pProperties->pNext);
        while (base) {
            if (base->sType ==
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) {
                const auto *driver = reinterpret_cast<
                    const VkPhysicalDeviceDriverProperties *>(base);
                trace_log("driver properties: driverID=%d name='%s' "
                          "info='%s' conformance=%u.%u.%u.%u",
                          driver->driverID, driver->driverName,
                          driver->driverInfo,
                          driver->conformanceVersion.major,
                          driver->conformanceVersion.minor,
                          driver->conformanceVersion.subminor,
                          driver->conformanceVersion.patch);
            }
            base = base->pNext;
        }
    }
#endif
}

static VKAPI_ATTR void VKAPI_CALL trace_vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures *pFeatures)
{
    g_real_vkGetPhysicalDeviceFeatures(physicalDevice, pFeatures);
    if (pFeatures && mark_device(physicalDevice, g_features_logged, 4))
        log_core_features("supported", *pFeatures);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *pFeatures)
{
    g_real_vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
    if (!pFeatures)
        return;
    if (mark_device(physicalDevice, g_features_logged, 4)) {
        log_core_features("supported", pFeatures->features);
        log_feature_chain("supported", pFeatures->pNext);
    }
}

static VKAPI_ATTR void VKAPI_CALL trace_vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physicalDevice, uint32_t *pQueueFamilyPropertyCount,
    VkQueueFamilyProperties *pQueueFamilyProperties)
{
    g_real_vkGetPhysicalDeviceQueueFamilyProperties(
        physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
    if (pQueueFamilyProperties && pQueueFamilyPropertyCount &&
        mark_device(physicalDevice, g_queue_logged, 4))
        log_queue_families(*pQueueFamilyPropertyCount, pQueueFamilyProperties);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
    g_real_vkGetPhysicalDeviceMemoryProperties(physicalDevice, pMemoryProperties);
    if (pMemoryProperties && mark_device(physicalDevice, g_memory_logged, 4))
        log_memory_properties(*pMemoryProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice, const char *pLayerName,
    uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
    VkResult r = g_real_vkEnumerateDeviceExtensionProperties(
        physicalDevice, pLayerName, pPropertyCount, pProperties);

    if (!pLayerName && pProperties && pPropertyCount && *pPropertyCount &&
        mark_device(physicalDevice, g_ext_logged, 4))
        log_extension_list("device", *pPropertyCount, pProperties);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE && pLayerName == nullptr)
        trace_log("vkEnumerateDeviceExtensionProperties result=%s",
                  result_name(r));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
    VkSurfaceKHR surface, VkBool32 *pSupported)
{
    VkResult r = g_real_vkGetPhysicalDeviceSurfaceSupportKHR(
        physicalDevice, queueFamilyIndex, surface, pSupported);
    trace_log("surface support device=%p queueFamily=%u surface=%p "
              "supported=%u result=%s", (void *)physicalDevice,
              queueFamilyIndex, (void *)surface,
              pSupported ? *pSupported : 0, result_name(r));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR *pSurfaceCapabilities)
{
    VkResult r = g_real_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physicalDevice, surface, pSurfaceCapabilities);
    if (r == VK_SUCCESS && pSurfaceCapabilities)
        log_surface_caps(*pSurfaceCapabilities);
    else
        trace_log("surface capabilities device=%p surface=%p result=%s",
                  (void *)physicalDevice, (void *)surface, result_name(r));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    uint32_t *pSurfaceFormatCount, VkSurfaceFormatKHR *pSurfaceFormats)
{
    VkResult r = g_real_vkGetPhysicalDeviceSurfaceFormatsKHR(
        physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);
    if (pSurfaceFormats && pSurfaceFormatCount && *pSurfaceFormatCount)
        log_surface_formats(*pSurfaceFormatCount, pSurfaceFormats);
    else if (r != VK_SUCCESS)
        trace_log("surface formats count query result=%s", result_name(r));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    uint32_t *pPresentModeCount, VkPresentModeKHR *pPresentModes)
{
    VkResult r = g_real_vkGetPhysicalDeviceSurfacePresentModesKHR(
        physicalDevice, surface, pPresentModeCount, pPresentModes);
    if (pPresentModes && pPresentModeCount && *pPresentModeCount)
        log_present_modes(*pPresentModeCount, pPresentModes);
    else if (r != VK_SUCCESS)
        trace_log("surface present-modes count query result=%s",
                  result_name(r));
    return r;
}

#ifdef VK_KHR_get_surface_capabilities2
static VKAPI_ATTR VkResult VKAPI_CALL trace_vkGetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
    VkSurfaceCapabilities2KHR *pSurfaceCapabilities)
{
    VkResult r = g_real_vkGetPhysicalDeviceSurfaceCapabilities2KHR(
        physicalDevice, pSurfaceInfo, pSurfaceCapabilities);
    if (r == VK_SUCCESS && pSurfaceCapabilities)
        log_surface_caps(pSurfaceCapabilities->surfaceCapabilities);
    else
        trace_log("surface capabilities2 device=%p surface=%p result=%s",
                  (void *)physicalDevice,
                  pSurfaceInfo ? (void *)pSurfaceInfo->surface : nullptr,
                  result_name(r));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkGetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
    uint32_t *pSurfaceFormatCount,
    VkSurfaceFormat2KHR *pSurfaceFormats)
{
    VkResult r = g_real_vkGetPhysicalDeviceSurfaceFormats2KHR(
        physicalDevice, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);
    if (pSurfaceFormats && pSurfaceFormatCount && *pSurfaceFormatCount) {
        uint32_t i;
        trace_log("surface formats2 count=%u", *pSurfaceFormatCount);
        for (i = 0; i < *pSurfaceFormatCount; ++i) {
            trace_log("  surface format2[%u] format=%d (%s) colorSpace=%d (%s)",
                      i, pSurfaceFormats[i].surfaceFormat.format,
                      format_name(pSurfaceFormats[i].surfaceFormat.format),
                      pSurfaceFormats[i].surfaceFormat.colorSpace,
                      color_space_name(
                          pSurfaceFormats[i].surfaceFormat.colorSpace));
        }
    } else if (r != VK_SUCCESS) {
        trace_log("surface formats2 count query result=%s", result_name(r));
    }
    return r;
}
#endif /* VK_KHR_get_surface_capabilities2 */

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkCreateAndroidSurfaceKHR(
    VkInstance instance, const VkAndroidSurfaceCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface)
{
    VkResult r = g_real_vkCreateAndroidSurfaceKHR(
        instance, pCreateInfo, pAllocator, pSurface);
    trace_log("vkCreateAndroidSurfaceKHR window=%p flags=0x%x result=%s "
              "surface=%p", pCreateInfo ? pCreateInfo->window : nullptr,
              pCreateInfo ? pCreateInfo->flags : 0, result_name(r),
              pSurface && r == VK_SUCCESS ? (void *)*pSurface : nullptr);
    if (r == VK_SUCCESS && pSurface) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_surface = *pSurface;
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL trace_vkDestroySurfaceKHR(
    VkInstance instance, VkSurfaceKHR surface,
    const VkAllocationCallbacks *pAllocator)
{
    trace_log("vkDestroySurfaceKHR surface=%p", (void *)surface);
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (g_surface == surface)
            g_surface = VK_NULL_HANDLE;
    }
    g_real_vkDestroySurfaceKHR(instance, surface, pAllocator);
}

// ==== DEVICE WRAPPERS ====

PFN_vkCreateDevice g_real_vkCreateDevice = nullptr;
PFN_vkDestroyDevice g_real_vkDestroyDevice = nullptr;
PFN_vkGetDeviceQueue g_real_vkGetDeviceQueue = nullptr;
PFN_vkCreateSwapchainKHR g_real_vkCreateSwapchainKHR = nullptr;
PFN_vkDestroySwapchainKHR g_real_vkDestroySwapchainKHR = nullptr;
PFN_vkGetSwapchainImagesKHR g_real_vkGetSwapchainImagesKHR = nullptr;
PFN_vkAcquireNextImageKHR g_real_vkAcquireNextImageKHR = nullptr;
PFN_vkAcquireNextImage2KHR g_real_vkAcquireNextImage2KHR = nullptr;
PFN_vkQueuePresentKHR g_real_vkQueuePresentKHR = nullptr;
PFN_vkQueueSubmit g_real_vkQueueSubmit = nullptr;
PFN_vkQueueSubmit2 g_real_vkQueueSubmit2 = nullptr;
#ifdef VK_KHR_synchronization2
PFN_vkQueueSubmit2KHR g_real_vkQueueSubmit2KHR = nullptr;
#endif
PFN_vkQueueWaitIdle g_real_vkQueueWaitIdle = nullptr;
PFN_vkDeviceWaitIdle g_real_vkDeviceWaitIdle = nullptr;
PFN_vkCreateRenderPass g_real_vkCreateRenderPass = nullptr;
PFN_vkCreateRenderPass2 g_real_vkCreateRenderPass2 = nullptr;
PFN_vkCreateFramebuffer g_real_vkCreateFramebuffer = nullptr;
PFN_vkDestroyFramebuffer g_real_vkDestroyFramebuffer = nullptr;
PFN_vkCreateImageView g_real_vkCreateImageView = nullptr;
PFN_vkDestroyImageView g_real_vkDestroyImageView = nullptr;
PFN_vkCreateImage g_real_vkCreateImage = nullptr;
PFN_vkCreateGraphicsPipelines g_real_vkCreateGraphicsPipelines = nullptr;
PFN_vkCreateComputePipelines g_real_vkCreateComputePipelines = nullptr;
PFN_vkCreateShaderModule g_real_vkCreateShaderModule = nullptr;
PFN_vkWaitForFences g_real_vkWaitForFences = nullptr;
PFN_vkAllocateMemory g_real_vkAllocateMemory = nullptr;
PFN_vkBindImageMemory g_real_vkBindImageMemory = nullptr;
PFN_vkBindBufferMemory g_real_vkBindBufferMemory = nullptr;

static uint32_t g_color_image_log_count;
static uint32_t g_view_log_count;
static uint32_t g_framebuffer_log_count;
static uint32_t g_render_pass_log_count;
static uint32_t g_shader_log_count;

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkCreateDevice(
    VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
    uint32_t i;

    trace_log("vkCreateDevice physicalDevice=%p queues=%u extensions=%u "
              "layers=%u", (void *)physicalDevice,
              pCreateInfo ? pCreateInfo->queueCreateInfoCount : 0,
              pCreateInfo ? pCreateInfo->enabledExtensionCount : 0,
              pCreateInfo ? pCreateInfo->enabledLayerCount : 0);
    if (pCreateInfo) {
        for (i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {
            const VkDeviceQueueCreateInfo *q =
                &pCreateInfo->pQueueCreateInfos[i];
            trace_log("  queueCreateInfo[%u] family=%u count=%u flags=0x%x",
                      i, q->queueFamilyIndex, q->queueCount, q->flags);
        }
        for (i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
            trace_log("  requested device extension: %s",
                      pCreateInfo->ppEnabledExtensionNames[i]);
        for (i = 0; i < pCreateInfo->enabledLayerCount; ++i)
            trace_log("  requested device layer: %s",
                      pCreateInfo->ppEnabledLayerNames[i]);
        if (pCreateInfo->pEnabledFeatures)
            log_core_features("device requested",
                              *pCreateInfo->pEnabledFeatures);
        log_feature_chain("device requested", pCreateInfo->pNext);
        log_missing_device_extensions(physicalDevice, pCreateInfo);
    }

    VkResult r = g_real_vkCreateDevice(physicalDevice, pCreateInfo,
                                       pAllocator, pDevice);
    trace_log("vkCreateDevice result=%s device=%p", result_name(r),
              pDevice && r == VK_SUCCESS ? (void *)*pDevice : nullptr);
    if (r == VK_SUCCESS && pDevice) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_physical_device = physicalDevice;
        g_device = *pDevice;
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL trace_vkDestroyDevice(
    VkDevice device, const VkAllocationCallbacks *pAllocator)
{
    trace_log("vkDestroyDevice device=%p", (void *)device);
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (g_device == device)
            g_device = VK_NULL_HANDLE;
    }
    g_real_vkDestroyDevice(device, pAllocator);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkGetDeviceQueue(
    VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex,
    VkQueue *pQueue)
{
    g_real_vkGetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
    trace_log("vkGetDeviceQueue family=%u index=%u queue=%p",
              queueFamilyIndex, queueIndex,
              pQueue ? (void *)*pQueue : nullptr);
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
    char usage[256];
    uint32_t i;

    if (pCreateInfo) {
        usage_string(pCreateInfo->imageUsage, usage, sizeof usage);
        trace_log("vkCreateSwapchainKHR surface=%p minImages=%u format=%d "
                  "(%s) colorSpace=%d (%s) extent=%ux%u layers=%u usage=0x%x "
                  "(%s) sharing=%s queueFamilies=%u preTransform=0x%x "
                  "compositeAlpha=0x%x presentMode=%d (%s) clipped=%u "
                  "oldSwapchain=%p flags=0x%x",
                  (void *)pCreateInfo->surface, pCreateInfo->minImageCount,
                  pCreateInfo->imageFormat,
                  format_name(pCreateInfo->imageFormat),
                  pCreateInfo->imageColorSpace,
                  color_space_name(pCreateInfo->imageColorSpace),
                  pCreateInfo->imageExtent.width,
                  pCreateInfo->imageExtent.height,
                  pCreateInfo->imageArrayLayers, pCreateInfo->imageUsage,
                  usage, sharing_mode_name(pCreateInfo->imageSharingMode),
                  pCreateInfo->queueFamilyIndexCount,
                  pCreateInfo->preTransform, pCreateInfo->compositeAlpha,
                  pCreateInfo->presentMode,
                  present_mode_name(pCreateInfo->presentMode),
                  pCreateInfo->clipped, (void *)pCreateInfo->oldSwapchain,
                  pCreateInfo->flags);

        {
            const VkBaseInStructure *base =
                reinterpret_cast<const VkBaseInStructure *>(pCreateInfo->pNext);
            while (base) {
#ifdef VK_KHR_image_format_list
                if (base->sType ==
                    VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
                    const auto *list = reinterpret_cast<
                        const VkImageFormatListCreateInfo *>(base);
                    trace_log("  swapchain imageFormatList count=%u",
                              list->viewFormatCount);
                    for (i = 0; i < list->viewFormatCount; ++i)
                        trace_log("    viewFormat[%u]=%d (%s)", i,
                                  list->pViewFormats[i],
                                  format_name(list->pViewFormats[i]));
                }
#endif
#ifdef VK_KHR_swapchain_maintenance1
                if (base->sType ==
                    VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_KHR) {
                    const auto *modes = reinterpret_cast<
                        const VkSwapchainPresentModesCreateInfoKHR *>(base);
                    trace_log("  swapchain presentModes count=%u",
                              modes->presentModeCount);
                    for (i = 0; i < modes->presentModeCount; ++i)
                        trace_log("    presentMode[%u]=%d (%s)", i,
                                  modes->pPresentModes[i],
                                  present_mode_name(modes->pPresentModes[i]));
                }
#endif
                base = base->pNext;
            }
        }
    } else {
        trace_log("vkCreateSwapchainKHR pCreateInfo=null");
    }

    VkResult r = g_real_vkCreateSwapchainKHR(device, pCreateInfo,
                                             pAllocator, pSwapchain);
    trace_log("vkCreateSwapchainKHR result=%s swapchain=%p", result_name(r),
              pSwapchain && r == VK_SUCCESS ? (void *)*pSwapchain : nullptr);
    if (r == VK_SUCCESS && pCreateInfo && pSwapchain) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        remember_swapchain_locked(*pSwapchain, pCreateInfo);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL trace_vkDestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks *pAllocator)
{
    trace_log("vkDestroySwapchainKHR swapchain=%p", (void *)swapchain);
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (g_swapchain == swapchain) {
            g_swapchain = VK_NULL_HANDLE;
            g_swapchain_image_total = 0;
            memset(g_swapchain_images, 0, sizeof g_swapchain_images);
        }
    }
    g_real_vkDestroySwapchainKHR(device, swapchain, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkGetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t *pSwapchainImageCount,
    VkImage *pSwapchainImages)
{
    VkResult r = g_real_vkGetSwapchainImagesKHR(
        device, swapchain, pSwapchainImageCount, pSwapchainImages);

    if (pSwapchainImageCount && !pSwapchainImages) {
        trace_log("vkGetSwapchainImagesKHR count=%u result=%s",
                  *pSwapchainImageCount, result_name(r));
    } else if (pSwapchainImageCount && pSwapchainImages) {
        uint32_t i;
        uint32_t stored = *pSwapchainImageCount > 8 ? 8 : *pSwapchainImageCount;

        trace_log("vkGetSwapchainImagesKHR count=%u result=%s",
                  *pSwapchainImageCount, result_name(r));
        for (i = 0; i < *pSwapchainImageCount; ++i) {
            if (i < 16)
                trace_log("  swapchain image[%u]=%p", i,
                          (void *)pSwapchainImages[i]);
        }
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_swapchain_image_total = *pSwapchainImageCount;
            memset(g_swapchain_images, 0, sizeof g_swapchain_images);
            memcpy(g_swapchain_images, pSwapchainImages,
                   stored * sizeof(VkImage));
        }
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
    VkResult r = g_real_vkAcquireNextImageKHR(
        device, swapchain, timeout, semaphore, fence, pImageIndex);
    uint64_t n = g_acquire_count.fetch_add(1) + 1;

    if (pImageIndex)
        g_current_image.store(*pImageIndex);
    if (n <= 10 || (n % 300) == 0 || r != VK_SUCCESS) {
        trace_log("vkAcquireNextImageKHR #%llu swapchain=%p result=%s "
                  "image=%u semaphore=%p fence=%p timeout=%llu",
                  (unsigned long long)n, (void *)swapchain, result_name(r),
                  pImageIndex ? *pImageIndex : 0xFFFFFFFFu,
                  (void *)semaphore, (void *)fence,
                  (unsigned long long)timeout);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkAcquireNextImage2KHR(
    VkDevice device, const VkAcquireNextImageInfoKHR *pAcquireInfo,
    uint32_t *pImageIndex)
{
    VkResult r = g_real_vkAcquireNextImage2KHR(
        device, pAcquireInfo, pImageIndex);
    uint64_t n = g_acquire_count.fetch_add(1) + 1;

    if (pImageIndex)
        g_current_image.store(*pImageIndex);
    if (n <= 10 || (n % 300) == 0 || r != VK_SUCCESS) {
        trace_log("vkAcquireNextImage2KHR #%llu swapchain=%p result=%s "
                  "image=%u semaphore=%p fence=%p timeout=%llu deviceMask=0x%x",
                  (unsigned long long)n,
                  pAcquireInfo ? (void *)pAcquireInfo->swapchain : nullptr,
                  result_name(r),
                  pImageIndex ? *pImageIndex : 0xFFFFFFFFu,
                  pAcquireInfo ? (void *)pAcquireInfo->semaphore : nullptr,
                  pAcquireInfo ? (void *)pAcquireInfo->fence : nullptr,
                  pAcquireInfo
                      ? (unsigned long long)pAcquireInfo->timeout : 0,
                  pAcquireInfo ? pAcquireInfo->deviceMask : 0);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkQueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    uint64_t frame = g_present_count.fetch_add(1);
    uint64_t acquires = g_acquire_count.exchange(0);
    uint64_t submits = g_submit_count.exchange(0);
    uint64_t draws = g_draw_count.exchange(0);
    uint64_t clears = g_clear_count.exchange(0);
    uint64_t passes = g_pass_count.exchange(0);
    uint32_t image = g_current_image.load();
    VkFormat format;
    VkColorSpaceKHR colorSpace;
    VkExtent2D extent;
    VkPresentModeKHR mode;
    uint32_t imageTotal;
    VkSwapchainKHR swapchain;
    VkResult r;
    uint32_t i;

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        format = g_swapchain_format;
        colorSpace = g_swapchain_color_space;
        extent = g_swapchain_extent;
        mode = g_swapchain_present_mode;
        imageTotal = g_swapchain_image_total;
        swapchain = g_swapchain;
    }

    if (pPresentInfo) {
        trace_log("vkQueuePresentKHR #%llu queue=%p waitSemaphores=%u "
                  "swapchains=%u", (unsigned long long)frame, (void *)queue,
                  pPresentInfo->waitSemaphoreCount,
                  pPresentInfo->swapchainCount);
        for (i = 0; i < pPresentInfo->swapchainCount; ++i) {
            trace_log("  present swapchain[%u]=%p imageIndex=%u", i,
                      (void *)pPresentInfo->pSwapchains[i],
                      pPresentInfo->pImageIndices[i]);
        }
        {
            const VkBaseInStructure *base =
                reinterpret_cast<const VkBaseInStructure *>(pPresentInfo->pNext);
            while (base) {
#ifdef VK_KHR_swapchain_maintenance1
                if (base->sType ==
                    VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR) {
                    const auto *modes = reinterpret_cast<
                        const VkSwapchainPresentModeInfoKHR *>(base);
                    uint32_t j;
                    trace_log("  present modeInfo count=%u",
                              modes->swapchainCount);
                    for (j = 0; j < modes->swapchainCount; ++j)
                        trace_log("    presentMode[%u]=%d (%s)", j,
                                  modes->pPresentModes[j],
                                  present_mode_name(modes->pPresentModes[j]));
                }
                if (base->sType ==
                    VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR) {
                    const auto *fences = reinterpret_cast<
                        const VkSwapchainPresentFenceInfoKHR *>(base);
                    uint32_t j;
                    trace_log("  present fenceInfo count=%u",
                              fences->swapchainCount);
                    for (j = 0; j < fences->swapchainCount; ++j)
                        trace_log("    presentFence[%u]=%p", j,
                                  (void *)fences->pFences[j]);
                }
#endif
#ifdef VK_KHR_present_id
                if (base->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR) {
                    const auto *ids = reinterpret_cast<
                        const VkPresentIdKHR *>(base);
                    uint32_t j;
                    trace_log("  present ids count=%u", ids->swapchainCount);
                    for (j = 0; j < ids->swapchainCount; ++j)
                        trace_log("    presentId[%u]=%llu", j,
                                  (unsigned long long)ids->pPresentIds[j]);
                }
#endif
                base = base->pNext;
            }
        }
    }

    r = g_real_vkQueuePresentKHR(queue, pPresentInfo);

    if (pPresentInfo && pPresentInfo->pResults) {
        for (i = 0; i < pPresentInfo->swapchainCount; ++i) {
            if (pPresentInfo->pResults[i] != VK_SUCCESS)
                trace_log("  present result[%u]=%s", i,
                          result_name(pPresentInfo->pResults[i]));
        }
    }

    const bool present_failed = r < 0;

    if (frame < 5 || (frame % 300) == 0 || present_failed ||
        (draws == 0 && clears == 0 && (frame % 60) == 0)) {
        trace_log("present #%llu result=%s image=%u/%u swapchain=%p "
                  "format=%d (%s) colorSpace=%d (%s) extent=%ux%u mode=%d (%s) "
                  "acquires=%llu submits=%llu draws=%llu clears=%llu passes=%llu",
                  (unsigned long long)frame, result_name(r), image, imageTotal,
                  (void *)swapchain, format, format_name(format), colorSpace,
                  color_space_name(colorSpace), extent.width, extent.height,
                  mode, present_mode_name(mode),
                  (unsigned long long)acquires,
                  (unsigned long long)submits,
                  (unsigned long long)draws,
                  (unsigned long long)clears,
                  (unsigned long long)passes);
    }
    if (present_failed)
        trace_log("present failed: result=%s (surface/device lost or "
                  "out-of-date)", result_name(r));
    else if (r == VK_SUBOPTIMAL_KHR &&
             g_suboptimal_log_count.fetch_add(1) == 0)
        trace_log("present suboptimal: result=%s (surface is not pre-rotated); "
                  "frame was presented", result_name(r));

    g_acquire_count.store(0);
    g_submit_count.store(0);
    g_draw_count.store(0);
    g_clear_count.store(0);
    g_pass_count.store(0);
    g_draw_log_budget.store(8);
    g_pass_log_budget.store(16);
    g_clear_log_budget.store(8);
    g_barrier_log_budget.store(8);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkQueueSubmit(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits,
    VkFence fence)
{
    VkResult r = g_real_vkQueueSubmit(queue, submitCount, pSubmits, fence);
    uint64_t n = g_submit_count.fetch_add(1) + 1;

    if (n <= 5 || r != VK_SUCCESS) {
        uint32_t i;
        uint32_t waits = 0;
        uint32_t signals = 0;
        uint32_t cmds = 0;

        for (i = 0; i < submitCount; ++i) {
            waits += pSubmits[i].waitSemaphoreCount;
            signals += pSubmits[i].signalSemaphoreCount;
            cmds += pSubmits[i].commandBufferCount;
        }
        trace_log("vkQueueSubmit #%llu submits=%u waits=%u signals=%u "
                  "commandBuffers=%u fence=%p result=%s",
                  (unsigned long long)n, submitCount, waits, signals, cmds,
                  (void *)fence, result_name(r));
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkQueueSubmit2(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits,
    VkFence fence)
{
    VkResult r = g_real_vkQueueSubmit2(queue, submitCount, pSubmits, fence);
    uint64_t n = g_submit_count.fetch_add(1) + 1;

    if (n <= 5 || r != VK_SUCCESS) {
        uint32_t i;
        uint32_t waits = 0;
        uint32_t signals = 0;
        uint32_t cmds = 0;

        for (i = 0; i < submitCount; ++i) {
            waits += pSubmits[i].waitSemaphoreInfoCount;
            signals += pSubmits[i].signalSemaphoreInfoCount;
            cmds += pSubmits[i].commandBufferInfoCount;
        }
        trace_log("vkQueueSubmit2 #%llu submits=%u waits=%u signals=%u "
                  "commandBuffers=%u fence=%p result=%s",
                  (unsigned long long)n, submitCount, waits, signals, cmds,
                  (void *)fence, result_name(r));
    }
    return r;
}

#ifdef VK_KHR_synchronization2
static VKAPI_ATTR VkResult VKAPI_CALL trace_vkQueueSubmit2KHR(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits,
    VkFence fence)
{
    VkResult r = g_real_vkQueueSubmit2KHR(queue, submitCount, pSubmits, fence);
    uint64_t n = g_submit_count.fetch_add(1) + 1;

    if (n <= 5 || r != VK_SUCCESS)
        trace_log("vkQueueSubmit2KHR #%llu submits=%u result=%s",
                  (unsigned long long)n, submitCount, result_name(r));
    return r;
}
#endif

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkQueueWaitIdle(VkQueue queue)
{
    VkResult r = g_real_vkQueueWaitIdle(queue);

    if (r != VK_SUCCESS)
        trace_log("vkQueueWaitIdle result=%s", result_name(r));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkDeviceWaitIdle(VkDevice device)
{
    VkResult r = g_real_vkDeviceWaitIdle(device);

    if (r != VK_SUCCESS)
        trace_log("vkDeviceWaitIdle result=%s", result_name(r));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkCreateImage(
    VkDevice device, const VkImageCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
    VkResult r = g_real_vkCreateImage(device, pCreateInfo, pAllocator, pImage);

    if (r == VK_SUCCESS && pCreateInfo &&
        (pCreateInfo->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
        g_color_image_log_count++ < 16) {
        char usage[256];
        usage_string(pCreateInfo->usage, usage, sizeof usage);
        trace_log("vkCreateImage colorAttachment image=%p format=%d (%s) "
                  "extent=%ux%ux%u mips=%u layers=%u samples=%u tiling=%u "
                  "usage=%s initialLayout=%s",
                  pImage ? (void *)*pImage : nullptr, pCreateInfo->format,
                  format_name(pCreateInfo->format), pCreateInfo->extent.width,
                  pCreateInfo->extent.height, pCreateInfo->extent.depth,
                  pCreateInfo->mipLevels, pCreateInfo->arrayLayers,
                  pCreateInfo->samples, pCreateInfo->tiling, usage,
                  layout_name(pCreateInfo->initialLayout));
    } else if (r != VK_SUCCESS) {
        trace_log("vkCreateImage failed result=%s format=%d extent=%ux%u "
                  "usage=0x%x", result_name(r),
                  pCreateInfo ? pCreateInfo->format : 0,
                  pCreateInfo ? pCreateInfo->extent.width : 0,
                  pCreateInfo ? pCreateInfo->extent.height : 0,
                  pCreateInfo ? pCreateInfo->usage : 0);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkCreateImageView(
    VkDevice device, const VkImageViewCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
    VkResult r = g_real_vkCreateImageView(device, pCreateInfo, pAllocator,
                                          pView);

    if (r == VK_SUCCESS && pCreateInfo && pView && is_swapchain_image(pCreateInfo->image)) {
        bool stored = false;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            uint32_t i;
            for (i = 0; i < 16; ++i) {
                if (!g_views[i].view) {
                    g_views[i].view = *pView;
                    g_views[i].image = pCreateInfo->image;
                    g_views[i].format = pCreateInfo->format;
                    stored = true;
                    break;
                }
            }
        }
        if (stored && g_view_log_count++ < 16) {
            trace_log("swapchain view=%p image=%p format=%d (%s) "
                      "viewType=%d components=%u,%u,%u,%u subresource=%u..%u "
                      "layers=%u..%u",
                      (void *)*pView, (void *)pCreateInfo->image,
                      pCreateInfo->format, format_name(pCreateInfo->format),
                      pCreateInfo->viewType,
                      pCreateInfo->components.r, pCreateInfo->components.g,
                      pCreateInfo->components.b, pCreateInfo->components.a,
                      pCreateInfo->subresourceRange.baseMipLevel,
                      pCreateInfo->subresourceRange.levelCount,
                      pCreateInfo->subresourceRange.baseArrayLayer,
                      pCreateInfo->subresourceRange.layerCount);
        }
    } else if (r != VK_SUCCESS) {
        trace_log("vkCreateImageView failed result=%s image=%p format=%d",
                  result_name(r), pCreateInfo ? (void *)pCreateInfo->image : nullptr,
                  pCreateInfo ? pCreateInfo->format : 0);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL trace_vkDestroyImageView(
    VkDevice device, VkImageView imageView,
    const VkAllocationCallbacks *pAllocator)
{
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        uint32_t i;
        for (i = 0; i < 16; ++i) {
            if (g_views[i].view == imageView)
                memset(&g_views[i], 0, sizeof g_views[i]);
        }
    }
    g_real_vkDestroyImageView(device, imageView, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkCreateFramebuffer(
    VkDevice device, const VkFramebufferCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkFramebuffer *pFramebuffer)
{
    VkResult r = g_real_vkCreateFramebuffer(device, pCreateInfo, pAllocator,
                                            pFramebuffer);

    if (r == VK_SUCCESS && pCreateInfo && pFramebuffer) {
        bool hasSwapchain = false;
        uint32_t i;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            for (i = 0; i < pCreateInfo->attachmentCount; ++i) {
                if (view_is_swapchain_locked(pCreateInfo->pAttachments[i])) {
                    hasSwapchain = true;
                    break;
                }
            }
            for (i = 0; i < 16; ++i) {
                if (!g_framebuffers[i].framebuffer) {
                    g_framebuffers[i].framebuffer = *pFramebuffer;
                    g_framebuffers[i].has_swapchain = hasSwapchain;
                    g_framebuffers[i].width = pCreateInfo->width;
                    g_framebuffers[i].height = pCreateInfo->height;
                    g_framebuffers[i].layers = pCreateInfo->layers;
                    g_framebuffers[i].attachments =
                        pCreateInfo->attachmentCount;
                    break;
                }
            }
        }
        if (hasSwapchain || g_framebuffer_log_count++ < 8) {
            trace_log("vkCreateFramebuffer framebuffer=%p renderPass=%p "
                      "attachments=%u size=%ux%u layers=%u swapchain=%u",
                      (void *)*pFramebuffer, (void *)pCreateInfo->renderPass,
                      pCreateInfo->attachmentCount, pCreateInfo->width,
                      pCreateInfo->height, pCreateInfo->layers, hasSwapchain);
        }
    } else if (r != VK_SUCCESS) {
        trace_log("vkCreateFramebuffer failed result=%s renderPass=%p "
                  "attachments=%u size=%ux%u", result_name(r),
                  pCreateInfo ? (void *)pCreateInfo->renderPass : nullptr,
                  pCreateInfo ? pCreateInfo->attachmentCount : 0,
                  pCreateInfo ? pCreateInfo->width : 0,
                  pCreateInfo ? pCreateInfo->height : 0);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL trace_vkDestroyFramebuffer(
    VkDevice device, VkFramebuffer framebuffer,
    const VkAllocationCallbacks *pAllocator)
{
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        uint32_t i;
        for (i = 0; i < 16; ++i) {
            if (g_framebuffers[i].framebuffer == framebuffer)
                memset(&g_framebuffers[i], 0, sizeof g_framebuffers[i]);
        }
    }
    g_real_vkDestroyFramebuffer(device, framebuffer, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkCreateRenderPass(
    VkDevice device, const VkRenderPassCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass)
{
    VkResult r = g_real_vkCreateRenderPass(device, pCreateInfo, pAllocator,
                                           pRenderPass);

    if (r == VK_SUCCESS && pCreateInfo && g_render_pass_log_count++ < 8) {
        uint32_t i;
        trace_log("vkCreateRenderPass renderPass=%p attachments=%u "
                  "subpasses=%u dependencies=%u",
                  pRenderPass ? (void *)*pRenderPass : nullptr,
                  pCreateInfo->attachmentCount, pCreateInfo->subpassCount,
                  pCreateInfo->dependencyCount);
        for (i = 0; i < pCreateInfo->attachmentCount; ++i) {
            const VkAttachmentDescription *a = &pCreateInfo->pAttachments[i];
            trace_log("  attachment[%u] format=%d (%s) samples=%u load=%s "
                      "store=%s stencilLoad=%s stencilStore=%s initial=%s "
                      "final=%s", i, a->format, format_name(a->format),
                      a->samples, load_op_name(a->loadOp),
                      store_op_name(a->storeOp),
                      load_op_name(a->stencilLoadOp),
                      store_op_name(a->stencilStoreOp),
                      layout_name(a->initialLayout),
                      layout_name(a->finalLayout));
        }
    } else if (r != VK_SUCCESS) {
        trace_log("vkCreateRenderPass failed result=%s attachments=%u",
                  result_name(r),
                  pCreateInfo ? pCreateInfo->attachmentCount : 0);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkCreateRenderPass2(
    VkDevice device, const VkRenderPassCreateInfo2 *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass)
{
    VkResult r = g_real_vkCreateRenderPass2(device, pCreateInfo, pAllocator,
                                            pRenderPass);

    if (r == VK_SUCCESS && pCreateInfo && g_render_pass_log_count++ < 8) {
        uint32_t i;
        trace_log("vkCreateRenderPass2 renderPass=%p attachments=%u "
                  "subpasses=%u dependencies=%u",
                  pRenderPass ? (void *)*pRenderPass : nullptr,
                  pCreateInfo->attachmentCount, pCreateInfo->subpassCount,
                  pCreateInfo->dependencyCount);
        for (i = 0; i < pCreateInfo->attachmentCount; ++i) {
            const VkAttachmentDescription2 *a = &pCreateInfo->pAttachments[i];
            trace_log("  attachment2[%u] format=%d (%s) samples=%u load=%s "
                      "store=%s stencilLoad=%s stencilStore=%s initial=%s "
                      "final=%s", i, a->format, format_name(a->format),
                      a->samples, load_op_name(a->loadOp),
                      store_op_name(a->storeOp),
                      load_op_name(a->stencilLoadOp),
                      store_op_name(a->stencilStoreOp),
                      layout_name(a->initialLayout),
                      layout_name(a->finalLayout));
        }
    } else if (r != VK_SUCCESS) {
        trace_log("vkCreateRenderPass2 failed result=%s attachments=%u",
                  result_name(r),
                  pCreateInfo ? pCreateInfo->attachmentCount : 0);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkCreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo *pCreateInfos,
    const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
    VkResult r = g_real_vkCreateGraphicsPipelines(
        device, pipelineCache, createInfoCount, pCreateInfos, pAllocator,
        pPipelines);

    if (r != VK_SUCCESS)
        trace_log("vkCreateGraphicsPipelines failed result=%s count=%u",
                  result_name(r), createInfoCount);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkCreateComputePipelines(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkComputePipelineCreateInfo *pCreateInfos,
    const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
    VkResult r = g_real_vkCreateComputePipelines(
        device, pipelineCache, createInfoCount, pCreateInfos, pAllocator,
        pPipelines);

    if (r != VK_SUCCESS)
        trace_log("vkCreateComputePipelines failed result=%s count=%u",
                  result_name(r), createInfoCount);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkCreateShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule)
{
    VkResult r = g_real_vkCreateShaderModule(device, pCreateInfo, pAllocator,
                                             pShaderModule);

    if (r == VK_SUCCESS && pCreateInfo && pCreateInfo->pCode &&
        g_shader_log_count++ < 12) {
        uint32_t hash = 2166136261u;
        size_t i;
        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(
            pCreateInfo->pCode);
        for (i = 0; i < pCreateInfo->codeSize; ++i)
            hash = (hash ^ bytes[i]) * 16777619u;
        trace_log("vkCreateShaderModule shader=%p codeSize=%zu hash=%08x "
                  "result=%s", pShaderModule ? (void *)*pShaderModule : nullptr,
                  pCreateInfo->codeSize, hash, result_name(r));
    } else if (r != VK_SUCCESS) {
        trace_log("vkCreateShaderModule failed result=%s codeSize=%zu",
                  result_name(r), pCreateInfo ? pCreateInfo->codeSize : 0);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkWaitForFences(
    VkDevice device, uint32_t fenceCount, const VkFence *pFences,
    VkBool32 waitAll, uint64_t timeout)
{
    VkResult r = g_real_vkWaitForFences(device, fenceCount, pFences, waitAll,
                                        timeout);

    if (r != VK_SUCCESS)
        trace_log("vkWaitForFences result=%s fences=%u waitAll=%u "
                  "timeout=%llu", result_name(r), fenceCount, waitAll,
                  (unsigned long long)timeout);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
    const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory)
{
    VkResult r = g_real_vkAllocateMemory(device, pAllocateInfo, pAllocator,
                                         pMemory);

    if (r != VK_SUCCESS) {
        trace_log("vkAllocateMemory failed result=%s size=%llu type=%u",
                  result_name(r),
                  pAllocateInfo
                      ? (unsigned long long)pAllocateInfo->allocationSize : 0,
                  pAllocateInfo ? pAllocateInfo->memoryTypeIndex : 0);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkBindImageMemory(
    VkDevice device, VkImage image, VkDeviceMemory memory,
    VkDeviceSize memoryOffset)
{
    VkResult r = g_real_vkBindImageMemory(device, image, memory,
                                          memoryOffset);

    if (r != VK_SUCCESS)
        trace_log("vkBindImageMemory failed result=%s image=%p offset=%llu",
                  result_name(r), (void *)image,
                  (unsigned long long)memoryOffset);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_vkBindBufferMemory(
    VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
    VkDeviceSize memoryOffset)
{
    VkResult r = g_real_vkBindBufferMemory(device, buffer, memory,
                                           memoryOffset);

    if (r != VK_SUCCESS)
        trace_log("vkBindBufferMemory failed result=%s buffer=%p offset=%llu",
                  result_name(r), (void *)buffer,
                  (unsigned long long)memoryOffset);
    return r;
}

// ==== COMMAND WRAPPERS ====

PFN_vkCmdBeginRenderPass g_real_vkCmdBeginRenderPass = nullptr;
PFN_vkCmdEndRenderPass g_real_vkCmdEndRenderPass = nullptr;
PFN_vkCmdBeginRendering g_real_vkCmdBeginRendering = nullptr;
PFN_vkCmdEndRendering g_real_vkCmdEndRendering = nullptr;
PFN_vkCmdClearAttachments g_real_vkCmdClearAttachments = nullptr;
PFN_vkCmdClearColorImage g_real_vkCmdClearColorImage = nullptr;
PFN_vkCmdClearDepthStencilImage g_real_vkCmdClearDepthStencilImage = nullptr;
PFN_vkCmdDraw g_real_vkCmdDraw = nullptr;
PFN_vkCmdDrawIndexed g_real_vkCmdDrawIndexed = nullptr;
PFN_vkCmdDrawIndirect g_real_vkCmdDrawIndirect = nullptr;
PFN_vkCmdDrawIndexedIndirect g_real_vkCmdDrawIndexedIndirect = nullptr;
PFN_vkCmdDrawIndirectCount g_real_vkCmdDrawIndirectCount = nullptr;
PFN_vkCmdDrawIndexedIndirectCount
    g_real_vkCmdDrawIndexedIndirectCount = nullptr;
PFN_vkCmdBindPipeline g_real_vkCmdBindPipeline = nullptr;
PFN_vkCmdBindDescriptorSets g_real_vkCmdBindDescriptorSets = nullptr;
PFN_vkCmdPushConstants g_real_vkCmdPushConstants = nullptr;
PFN_vkCmdPipelineBarrier g_real_vkCmdPipelineBarrier = nullptr;
PFN_vkCmdPipelineBarrier2 g_real_vkCmdPipelineBarrier2 = nullptr;
#ifdef VK_KHR_synchronization2
PFN_vkCmdPipelineBarrier2KHR g_real_vkCmdPipelineBarrier2KHR = nullptr;
#endif

static bool view_is_swapchain(VkImageView view, VkFormat *format)
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    uint32_t i;

    for (i = 0; i < 16; ++i) {
        if (g_views[i].view == view) {
            if (format)
                *format = g_views[i].format;
            return true;
        }
    }
    return false;
}

static bool framebuffer_is_swapchain(VkFramebuffer framebuffer,
                                     uint32_t *width, uint32_t *height,
                                     uint32_t *layers)
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    uint32_t i;

    for (i = 0; i < 16; ++i) {
        if (g_framebuffers[i].framebuffer == framebuffer) {
            if (width)
                *width = g_framebuffers[i].width;
            if (height)
                *height = g_framebuffers[i].height;
            if (layers)
                *layers = g_framebuffers[i].layers;
            return g_framebuffers[i].has_swapchain;
        }
    }
    return false;
}

static void log_clear_value(const char *where, const VkClearValue &value)
{
    trace_log("%s color=(%.3f,%.3f,%.3f,%.3f) depth=%.3f stencil=%u",
              where, value.color.float32[0], value.color.float32[1],
              value.color.float32[2], value.color.float32[3],
              value.depthStencil.depth, value.depthStencil.stencil);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdBeginRenderPass(
    VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo *pRenderPassBegin,
    VkSubpassContents contents)
{
    g_pass_count.fetch_add(1);
    if (pRenderPassBegin && trace_detail_frame() &&
        take_budget(g_pass_log_budget)) {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t layers = 0;
        bool swapchain = framebuffer_is_swapchain(
            pRenderPassBegin->framebuffer, &width, &height, &layers);
        uint32_t i;
        uint32_t n = pRenderPassBegin->clearValueCount > 4
            ? 4 : pRenderPassBegin->clearValueCount;

        trace_log("vkCmdBeginRenderPass renderPass=%p framebuffer=%p "
                  "swapchain=%u area=%u,%u %ux%u layers=%u clearValues=%u",
                  (void *)pRenderPassBegin->renderPass,
                  (void *)pRenderPassBegin->framebuffer, swapchain,
                  pRenderPassBegin->renderArea.offset.x,
                  pRenderPassBegin->renderArea.offset.y,
                  pRenderPassBegin->renderArea.extent.width,
                  pRenderPassBegin->renderArea.extent.height, layers,
                  pRenderPassBegin->clearValueCount);
        for (i = 0; i < n; ++i) {
            if (pRenderPassBegin->pClearValues)
                log_clear_value("  renderPass clear", pRenderPassBegin->pClearValues[i]);
        }
    }
    g_real_vkCmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdEndRenderPass(
    VkCommandBuffer commandBuffer)
{
    g_real_vkCmdEndRenderPass(commandBuffer);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdBeginRendering(
    VkCommandBuffer commandBuffer, const VkRenderingInfo *pRenderingInfo)
{
    g_pass_count.fetch_add(1);
    if (pRenderingInfo && trace_detail_frame() &&
        take_budget(g_pass_log_budget)) {
        uint32_t i;
        uint32_t swapchainCount = 0;

        trace_log("vkCmdBeginRendering area=%u,%u %ux%u layers=%u viewMask=0x%x "
                  "colorAttachments=%u depth=%u stencil=%u",
                  pRenderingInfo->renderArea.offset.x,
                  pRenderingInfo->renderArea.offset.y,
                  pRenderingInfo->renderArea.extent.width,
                  pRenderingInfo->renderArea.extent.height,
                  pRenderingInfo->layerCount, pRenderingInfo->viewMask,
                  pRenderingInfo->colorAttachmentCount,
                  pRenderingInfo->pDepthAttachment != nullptr,
                  pRenderingInfo->pStencilAttachment != nullptr);
        for (i = 0; i < pRenderingInfo->colorAttachmentCount; ++i) {
            const VkRenderingAttachmentInfo *a =
                &pRenderingInfo->pColorAttachments[i];
            VkFormat format = VK_FORMAT_UNDEFINED;
            bool swapchain = view_is_swapchain(a->imageView, &format);

            if (swapchain)
                swapchainCount++;
            trace_log("  colorAttachment[%u] view=%p swapchain=%u format=%d "
                      "(%s) layout=%s resolveView=%p resolveLayout=%s "
                      "load=%s store=%s",
                      i, (void *)a->imageView, swapchain, format,
                      format_name(format), layout_name(a->imageLayout),
                      (void *)a->resolveImageView,
                      layout_name(a->resolveImageLayout),
                      load_op_name(a->loadOp), store_op_name(a->storeOp));
            log_clear_value("    clear", a->clearValue);
        }
        if (pRenderingInfo->pDepthAttachment) {
            const VkRenderingAttachmentInfo *a =
                pRenderingInfo->pDepthAttachment;
            trace_log("  depthAttachment view=%p layout=%s load=%s store=%s",
                      (void *)a->imageView, layout_name(a->imageLayout),
                      load_op_name(a->loadOp), store_op_name(a->storeOp));
            log_clear_value("    clear", a->clearValue);
        }
        if (pRenderingInfo->pStencilAttachment) {
            const VkRenderingAttachmentInfo *a =
                pRenderingInfo->pStencilAttachment;
            trace_log("  stencilAttachment view=%p layout=%s load=%s store=%s",
                      (void *)a->imageView, layout_name(a->imageLayout),
                      load_op_name(a->loadOp), store_op_name(a->storeOp));
            log_clear_value("    clear", a->clearValue);
        }
        if (swapchainCount)
            trace_log("  SWAPCHAIN RENDERING: %u color attachment(s)",
                      swapchainCount);
    }
    g_real_vkCmdBeginRendering(commandBuffer, pRenderingInfo);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdEndRendering(
    VkCommandBuffer commandBuffer)
{
    g_real_vkCmdEndRendering(commandBuffer);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdClearAttachments(
    VkCommandBuffer commandBuffer, uint32_t attachmentCount,
    const VkClearAttachment *pAttachments, uint32_t rectCount,
    const VkClearRect *pRects)
{
    g_clear_count.fetch_add(1);
    if (trace_detail_frame() && take_budget(g_clear_log_budget)) {
        uint32_t i;
        trace_log("vkCmdClearAttachments attachments=%u rects=%u", attachmentCount,
                  rectCount);
        for (i = 0; i < attachmentCount && i < 8; ++i) {
            trace_log("  clearAttachment[%u] aspect=0x%x colorAttachment=%u",
                      i, pAttachments[i].aspectMask,
                      pAttachments[i].colorAttachment);
            log_clear_value("    clear", pAttachments[i].clearValue);
        }
    }
    g_real_vkCmdClearAttachments(commandBuffer, attachmentCount, pAttachments,
                                 rectCount, pRects);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdClearColorImage(
    VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout,
    const VkClearColorValue *pColor, uint32_t rangeCount,
    const VkImageSubresourceRange *pRanges)
{
    g_clear_count.fetch_add(1);
    if (trace_detail_frame() && take_budget(g_clear_log_budget)) {
        trace_log("vkCmdClearColorImage image=%p swapchain=%u layout=%s "
                  "color=(%.3f,%.3f,%.3f,%.3f) ranges=%u",
                  (void *)image, is_swapchain_image(image),
                  layout_name(imageLayout),
                  pColor ? pColor->float32[0] : 0.0f,
                  pColor ? pColor->float32[1] : 0.0f,
                  pColor ? pColor->float32[2] : 0.0f,
                  pColor ? pColor->float32[3] : 0.0f, rangeCount);
    }
    g_real_vkCmdClearColorImage(commandBuffer, image, imageLayout, pColor,
                                rangeCount, pRanges);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdClearDepthStencilImage(
    VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout,
    const VkClearDepthStencilValue *pDepthStencil, uint32_t rangeCount,
    const VkImageSubresourceRange *pRanges)
{
    g_clear_count.fetch_add(1);
    if (trace_detail_frame() && take_budget(g_clear_log_budget)) {
        trace_log("vkCmdClearDepthStencilImage image=%p layout=%s "
                  "depth=%.3f stencil=%u ranges=%u", (void *)image,
                  layout_name(imageLayout),
                  pDepthStencil ? pDepthStencil->depth : 0.0f,
                  pDepthStencil ? pDepthStencil->stencil : 0,
                  rangeCount);
    }
    g_real_vkCmdClearDepthStencilImage(commandBuffer, image, imageLayout,
                                       pDepthStencil, rangeCount, pRanges);
}

static void log_draw(const char *what, uint64_t count, const char *detail)
{
    g_draw_count.fetch_add(1);
    if (trace_detail_frame() && take_budget(g_draw_log_budget))
        trace_log("%s #%llu %s", what, (unsigned long long)count, detail);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdDraw(
    VkCommandBuffer commandBuffer, uint32_t vertexCount,
    uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
    char detail[160];
    uint64_t n = g_draw_count.load() + 1;

    snprintf(detail, sizeof detail, "vertices=%u instances=%u firstVertex=%u "
             "firstInstance=%u", vertexCount, instanceCount, firstVertex,
             firstInstance);
    log_draw("vkCmdDraw", n, detail);
    g_real_vkCmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex,
                     firstInstance);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdDrawIndexed(
    VkCommandBuffer commandBuffer, uint32_t indexCount,
    uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset,
    uint32_t firstInstance)
{
    char detail[192];
    uint64_t n = g_draw_count.load() + 1;

    snprintf(detail, sizeof detail, "indices=%u instances=%u firstIndex=%u "
             "vertexOffset=%d firstInstance=%u", indexCount, instanceCount,
             firstIndex, vertexOffset, firstInstance);
    log_draw("vkCmdDrawIndexed", n, detail);
    g_real_vkCmdDrawIndexed(commandBuffer, indexCount, instanceCount,
                            firstIndex, vertexOffset, firstInstance);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdDrawIndirect(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
    uint32_t drawCount, uint32_t stride)
{
    char detail[160];
    uint64_t n = g_draw_count.load() + 1;

    snprintf(detail, sizeof detail, "buffer=%p offset=%llu draws=%u stride=%u",
             (void *)buffer, (unsigned long long)offset, drawCount, stride);
    log_draw("vkCmdDrawIndirect", n, detail);
    g_real_vkCmdDrawIndirect(commandBuffer, buffer, offset, drawCount, stride);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdDrawIndexedIndirect(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
    uint32_t drawCount, uint32_t stride)
{
    char detail[160];
    uint64_t n = g_draw_count.load() + 1;

    snprintf(detail, sizeof detail, "buffer=%p offset=%llu draws=%u stride=%u",
             (void *)buffer, (unsigned long long)offset, drawCount, stride);
    log_draw("vkCmdDrawIndexedIndirect", n, detail);
    g_real_vkCmdDrawIndexedIndirect(commandBuffer, buffer, offset, drawCount,
                                    stride);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdDrawIndirectCount(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
    VkBuffer countBuffer, VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount, uint32_t stride)
{
    char detail[200];
    uint64_t n = g_draw_count.load() + 1;

    snprintf(detail, sizeof detail, "buffer=%p offset=%llu countBuffer=%p "
             "countOffset=%llu maxDraws=%u stride=%u", (void *)buffer,
             (unsigned long long)offset, (void *)countBuffer,
             (unsigned long long)countBufferOffset, maxDrawCount, stride);
    log_draw("vkCmdDrawIndirectCount", n, detail);
    g_real_vkCmdDrawIndirectCount(commandBuffer, buffer, offset, countBuffer,
                                  countBufferOffset, maxDrawCount, stride);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdDrawIndexedIndirectCount(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
    VkBuffer countBuffer, VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount, uint32_t stride)
{
    char detail[200];
    uint64_t n = g_draw_count.load() + 1;

    snprintf(detail, sizeof detail, "buffer=%p offset=%llu countBuffer=%p "
             "countOffset=%llu maxDraws=%u stride=%u", (void *)buffer,
             (unsigned long long)offset, (void *)countBuffer,
             (unsigned long long)countBufferOffset, maxDrawCount, stride);
    log_draw("vkCmdDrawIndexedIndirectCount", n, detail);
    g_real_vkCmdDrawIndexedIndirectCount(commandBuffer, buffer, offset,
                                         countBuffer, countBufferOffset,
                                         maxDrawCount, stride);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdBindPipeline(
    VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
    VkPipeline pipeline)
{
    g_real_vkCmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdBindDescriptorSets(
    VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount,
    const VkDescriptorSet *pDescriptorSets, uint32_t dynamicOffsetCount,
    const uint32_t *pDynamicOffsets)
{
    g_real_vkCmdBindDescriptorSets(commandBuffer, pipelineBindPoint, layout,
                                   firstSet, descriptorSetCount,
                                   pDescriptorSets, dynamicOffsetCount,
                                   pDynamicOffsets);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdPushConstants(
    VkCommandBuffer commandBuffer, VkPipelineLayout layout,
    VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size,
    const void *pValues)
{
    g_real_vkCmdPushConstants(commandBuffer, layout, stageFlags, offset, size,
                              pValues);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdPipelineBarrier(
    VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags,
    uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier *pBufferMemoryBarriers,
    uint32_t imageMemoryBarrierCount,
    const VkImageMemoryBarrier *pImageMemoryBarriers)
{
    if (trace_detail_frame() && take_budget(g_barrier_log_budget)) {
        uint32_t i;
        for (i = 0; i < imageMemoryBarrierCount; ++i) {
            const VkImageMemoryBarrier *b = &pImageMemoryBarriers[i];
            if (b->newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
                b->oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
                is_swapchain_image(b->image)) {
                trace_log("vkCmdPipelineBarrier image=%p swapchain=%u "
                          "old=%s new=%s srcStage=0x%x dstStage=0x%x "
                          "srcAccess=0x%x dstAccess=0x%x", (void *)b->image,
                          is_swapchain_image(b->image),
                          layout_name(b->oldLayout),
                          layout_name(b->newLayout), srcStageMask,
                          dstStageMask, b->srcAccessMask,
                          b->dstAccessMask);
            }
        }
    }
    g_real_vkCmdPipelineBarrier(commandBuffer, srcStageMask, dstStageMask,
                                dependencyFlags, memoryBarrierCount,
                                pMemoryBarriers, bufferMemoryBarrierCount,
                                pBufferMemoryBarriers, imageMemoryBarrierCount,
                                pImageMemoryBarriers);
}

static VKAPI_ATTR void VKAPI_CALL trace_vkCmdPipelineBarrier2(
    VkCommandBuffer commandBuffer, const VkDependencyInfo *pDependencyInfo)
{
    if (pDependencyInfo && trace_detail_frame() &&
        take_budget(g_barrier_log_budget)) {
        uint32_t i;
        for (i = 0; i < pDependencyInfo->imageMemoryBarrierCount; ++i) {
            const VkImageMemoryBarrier2 *b =
                &pDependencyInfo->pImageMemoryBarriers[i];
            if (b->newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
                b->oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
                is_swapchain_image(b->image)) {
                trace_log("vkCmdPipelineBarrier2 image=%p swapchain=%u "
                          "old=%s new=%s srcStage=0x%llx dstStage=0x%llx "
                          "srcAccess=0x%llx dstAccess=0x%llx",
                          (void *)b->image, is_swapchain_image(b->image),
                          layout_name(b->oldLayout), layout_name(b->newLayout),
                          (unsigned long long)b->srcStageMask,
                          (unsigned long long)b->dstStageMask,
                          (unsigned long long)b->srcAccessMask,
                          (unsigned long long)b->dstAccessMask);
            }
        }
    }
    g_real_vkCmdPipelineBarrier2(commandBuffer, pDependencyInfo);
}

#ifdef VK_KHR_synchronization2
static VKAPI_ATTR void VKAPI_CALL trace_vkCmdPipelineBarrier2KHR(
    VkCommandBuffer commandBuffer, const VkDependencyInfo *pDependencyInfo)
{
    if (pDependencyInfo && trace_detail_frame() &&
        take_budget(g_barrier_log_budget)) {
        uint32_t i;
        for (i = 0; i < pDependencyInfo->imageMemoryBarrierCount; ++i) {
            const VkImageMemoryBarrier2 *b =
                &pDependencyInfo->pImageMemoryBarriers[i];
            if (b->newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
                b->oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
                is_swapchain_image(b->image)) {
                trace_log("vkCmdPipelineBarrier2KHR image=%p swapchain=%u "
                          "old=%s new=%s srcStage=0x%llx dstStage=0x%llx "
                          "srcAccess=0x%llx dstAccess=0x%llx",
                          (void *)b->image, is_swapchain_image(b->image),
                          layout_name(b->oldLayout), layout_name(b->newLayout),
                          (unsigned long long)b->srcStageMask,
                          (unsigned long long)b->dstStageMask,
                          (unsigned long long)b->srcAccessMask,
                          (unsigned long long)b->dstAccessMask);
            }
        }
    }
    g_real_vkCmdPipelineBarrier2KHR(commandBuffer, pDependencyInfo);
}
#endif

// ==== PROC ADDRESS ====

static VkInstance current_instance()
{
    std::lock_guard<std::mutex> lock(g_state_mutex);

    return g_instance;
}

static PFN_vkVoidFunction resolve_device_proc(VkInstance instance,
                                              VkDevice device,
                                              const char *name)
{
    PFN_vkVoidFunction real = nullptr;

    if (device && g_real_gdpa)
        real = g_real_gdpa(device, name);
    if (!real && g_real_gipa)
        real = g_real_gipa(instance, name);
    return real;
}

#define TRACE_WRAP_INSTANCE_PROC(fn)                                         \
    if (strcmp(pName, #fn) == 0) {                                           \
        if (!g_real_##fn)                                                    \
            g_real_##fn = reinterpret_cast<PFN_##fn>(                        \
                g_real_gipa ? g_real_gipa(instance, pName) : nullptr);       \
        if (!g_real_##fn)                                                    \
            return nullptr;                                                  \
        return reinterpret_cast<PFN_vkVoidFunction>(trace_##fn);             \
    }

#define TRACE_WRAP_GLOBAL_PROC(fn)                                           \
    if (strcmp(pName, #fn) == 0) {                                           \
        if (!g_real_##fn)                                                    \
            g_real_##fn = reinterpret_cast<PFN_##fn>(                        \
                g_real_gipa ? g_real_gipa(VK_NULL_HANDLE, pName)             \
                            : nullptr);                                      \
        if (!g_real_##fn)                                                    \
            return nullptr;                                                  \
        return reinterpret_cast<PFN_vkVoidFunction>(trace_##fn);             \
    }

#define TRACE_WRAP_DEVICE_PROC(fn)                                           \
    if (strcmp(pName, #fn) == 0) {                                           \
        if (!g_real_##fn)                                                    \
            g_real_##fn = reinterpret_cast<PFN_##fn>(                        \
                resolve_device_proc(instance, device, pName));               \
        if (!g_real_##fn)                                                    \
            return nullptr;                                                  \
        return reinterpret_cast<PFN_vkVoidFunction>(trace_##fn);             \
    }

static PFN_vkVoidFunction wrap_device_proc(VkInstance instance, VkDevice device,
                                           const char *pName)
{
    TRACE_WRAP_DEVICE_PROC(vkCreateSwapchainKHR)
    TRACE_WRAP_DEVICE_PROC(vkDestroySwapchainKHR)
    TRACE_WRAP_DEVICE_PROC(vkGetSwapchainImagesKHR)
    TRACE_WRAP_DEVICE_PROC(vkAcquireNextImageKHR)
    TRACE_WRAP_DEVICE_PROC(vkAcquireNextImage2KHR)
    TRACE_WRAP_DEVICE_PROC(vkQueuePresentKHR)
    TRACE_WRAP_DEVICE_PROC(vkQueueSubmit)
    TRACE_WRAP_DEVICE_PROC(vkQueueSubmit2)
#ifdef VK_KHR_synchronization2
    TRACE_WRAP_DEVICE_PROC(vkQueueSubmit2KHR)
#endif
    TRACE_WRAP_DEVICE_PROC(vkQueueWaitIdle)
    TRACE_WRAP_DEVICE_PROC(vkDeviceWaitIdle)
    TRACE_WRAP_DEVICE_PROC(vkGetDeviceQueue)
    TRACE_WRAP_DEVICE_PROC(vkDestroyDevice)
    TRACE_WRAP_DEVICE_PROC(vkCreateImage)
    TRACE_WRAP_DEVICE_PROC(vkCreateImageView)
    TRACE_WRAP_DEVICE_PROC(vkDestroyImageView)
    TRACE_WRAP_DEVICE_PROC(vkCreateFramebuffer)
    TRACE_WRAP_DEVICE_PROC(vkDestroyFramebuffer)
    TRACE_WRAP_DEVICE_PROC(vkCreateRenderPass)
    TRACE_WRAP_DEVICE_PROC(vkCreateRenderPass2)
    TRACE_WRAP_DEVICE_PROC(vkCreateGraphicsPipelines)
    TRACE_WRAP_DEVICE_PROC(vkCreateComputePipelines)
    TRACE_WRAP_DEVICE_PROC(vkCreateShaderModule)
    TRACE_WRAP_DEVICE_PROC(vkWaitForFences)
    TRACE_WRAP_DEVICE_PROC(vkAllocateMemory)
    TRACE_WRAP_DEVICE_PROC(vkBindImageMemory)
    TRACE_WRAP_DEVICE_PROC(vkBindBufferMemory)
    TRACE_WRAP_DEVICE_PROC(vkCmdBeginRenderPass)
    TRACE_WRAP_DEVICE_PROC(vkCmdEndRenderPass)
    TRACE_WRAP_DEVICE_PROC(vkCmdBeginRendering)
    TRACE_WRAP_DEVICE_PROC(vkCmdEndRendering)
    TRACE_WRAP_DEVICE_PROC(vkCmdClearAttachments)
    TRACE_WRAP_DEVICE_PROC(vkCmdClearColorImage)
    TRACE_WRAP_DEVICE_PROC(vkCmdClearDepthStencilImage)
    TRACE_WRAP_DEVICE_PROC(vkCmdDraw)
    TRACE_WRAP_DEVICE_PROC(vkCmdDrawIndexed)
    TRACE_WRAP_DEVICE_PROC(vkCmdDrawIndirect)
    TRACE_WRAP_DEVICE_PROC(vkCmdDrawIndexedIndirect)
    TRACE_WRAP_DEVICE_PROC(vkCmdDrawIndirectCount)
    TRACE_WRAP_DEVICE_PROC(vkCmdDrawIndexedIndirectCount)
    TRACE_WRAP_DEVICE_PROC(vkCmdBindPipeline)
    TRACE_WRAP_DEVICE_PROC(vkCmdBindDescriptorSets)
    TRACE_WRAP_DEVICE_PROC(vkCmdPushConstants)
    TRACE_WRAP_DEVICE_PROC(vkCmdPipelineBarrier)
    TRACE_WRAP_DEVICE_PROC(vkCmdPipelineBarrier2)
#ifdef VK_KHR_synchronization2
    TRACE_WRAP_DEVICE_PROC(vkCmdPipelineBarrier2KHR)
#endif
    return nullptr;
}

#undef TRACE_WRAP_DEVICE_PROC

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
trace_vkGetDeviceProcAddr(VkDevice device, const char *pName);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
trace_vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    PFN_vkVoidFunction proc;

    if (!pName)
        return nullptr;
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(trace_vkGetInstanceProcAddr);
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        if (!g_real_gdpa && g_real_gipa)
            g_real_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                g_real_gipa(instance, pName));
        return g_real_gdpa
            ? reinterpret_cast<PFN_vkVoidFunction>(trace_vkGetDeviceProcAddr)
            : nullptr;
    }

    TRACE_WRAP_GLOBAL_PROC(vkEnumerateInstanceExtensionProperties)
    TRACE_WRAP_GLOBAL_PROC(vkEnumerateInstanceLayerProperties)
    TRACE_WRAP_GLOBAL_PROC(vkCreateInstance)

    TRACE_WRAP_INSTANCE_PROC(vkDestroyInstance)
    TRACE_WRAP_INSTANCE_PROC(vkEnumeratePhysicalDevices)
    TRACE_WRAP_INSTANCE_PROC(vkGetPhysicalDeviceProperties)
    TRACE_WRAP_INSTANCE_PROC(vkGetPhysicalDeviceProperties2)
    TRACE_WRAP_INSTANCE_PROC(vkGetPhysicalDeviceFeatures)
    TRACE_WRAP_INSTANCE_PROC(vkGetPhysicalDeviceFeatures2)
    TRACE_WRAP_INSTANCE_PROC(vkGetPhysicalDeviceQueueFamilyProperties)
    TRACE_WRAP_INSTANCE_PROC(vkGetPhysicalDeviceMemoryProperties)
    TRACE_WRAP_INSTANCE_PROC(vkEnumerateDeviceExtensionProperties)
    TRACE_WRAP_INSTANCE_PROC(vkGetPhysicalDeviceSurfaceSupportKHR)
    TRACE_WRAP_INSTANCE_PROC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
    TRACE_WRAP_INSTANCE_PROC(vkGetPhysicalDeviceSurfaceFormatsKHR)
    TRACE_WRAP_INSTANCE_PROC(vkGetPhysicalDeviceSurfacePresentModesKHR)
#ifdef VK_KHR_get_surface_capabilities2
    TRACE_WRAP_INSTANCE_PROC(vkGetPhysicalDeviceSurfaceCapabilities2KHR)
    TRACE_WRAP_INSTANCE_PROC(vkGetPhysicalDeviceSurfaceFormats2KHR)
#endif
    TRACE_WRAP_INSTANCE_PROC(vkCreateAndroidSurfaceKHR)
    TRACE_WRAP_INSTANCE_PROC(vkDestroySurfaceKHR)
    TRACE_WRAP_INSTANCE_PROC(vkCreateDevice)

    proc = wrap_device_proc(instance, VK_NULL_HANDLE, pName);
    if (proc)
        return proc;

    return g_real_gipa ? g_real_gipa(instance, pName) : nullptr;
}

#undef TRACE_WRAP_INSTANCE_PROC
#undef TRACE_WRAP_GLOBAL_PROC

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
trace_vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
    PFN_vkVoidFunction proc;

    if (!pName)
        return nullptr;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(trace_vkGetDeviceProcAddr);

    proc = wrap_device_proc(current_instance(), device, pName);
    if (proc)
        return proc;

    return g_real_gdpa ? g_real_gdpa(device, pName) : nullptr;
}

} /* namespace */

extern "C" PFN_vkGetInstanceProcAddr wc3_dxvk_trace_wrap_gipa(
    PFN_vkGetInstanceProcAddr real_gipa)
{
    if (!real_gipa)
        return nullptr;
    if (real_gipa == trace_vkGetInstanceProcAddr)
        return g_real_gipa ? g_real_gipa : real_gipa;
    g_real_gipa = real_gipa;
    return trace_vkGetInstanceProcAddr;
}

extern "C" void wc3_dxvk_trace_set_version(const char *version)
{
    if (!version || !version[0])
        version = "?";
    snprintf(g_version, sizeof g_version, "%s", version);
    trace_log("==== DXVK Vulkan boundary trace: archive %s ====", g_version);
}

extern "C" void wc3_dxvk_trace_mark(const char *what)
{
    trace_log("mark %s", what && what[0] ? what : "");
}

#else /* WC3_DEBUG && WC3_DEBUG_DXVK_DEBUG */

extern "C" PFN_vkGetInstanceProcAddr wc3_dxvk_trace_wrap_gipa(
    PFN_vkGetInstanceProcAddr real_gipa)
{
    return real_gipa;
}

extern "C" void wc3_dxvk_trace_set_version(const char *) {}

extern "C" void wc3_dxvk_trace_mark(const char *) {}

#endif /* WC3_DEBUG && WC3_DEBUG_DXVK_DEBUG */
