#pragma once

#include "../util/config/config.h"
#include "../util/util_env.h"

#include "../vulkan/vulkan_loader.h"

namespace dxvk {

  struct DxvkOptions {
    DxvkOptions() { }
    DxvkOptions(const Config& config);

    /// Enable debug utils
    bool enableDebugUtils = false;

    /// Enable memory defragmentation
    Tristate enableMemoryDefrag = Tristate::Auto;

    /// Number of compiler threads
    /// when using the state cache
    int32_t numCompilerThreads = 0;

    /// Enable graphics pipeline library
    Tristate enableGraphicsPipelineLibrary = Tristate::Auto;

    /// Enable descriptor heap
    Tristate enableDescriptorHeap = Tristate::Auto;

    /// Enable descriptor buffer
    Tristate enableDescriptorBuffer = Tristate::Auto;

    /// Enable unified image layout path
    bool enableUnifiedImageLayout = true;

    /// Enables pipeline lifetime tracking
    Tristate trackPipelineLifetime = Tristate::Auto;

    /// Shader-related options
    Tristate useRawSsbo = Tristate::Auto;

    /// HUD elements
    std::string hud;

    /// Forces swap chain into MAILBOX (if true)
    /// or FIFO_RELAXED (if false) present mode
    Tristate tearFree = Tristate::Auto;

    /// Enables latency sleep
    Tristate latencySleep = Tristate::Auto;

    /// Latency tolerance, in microseconds
    int32_t latencyTolerance = 0u;

    /// Disable VK_NV_low_latency2. This extension
    /// appears to be all sorts of broken on 32-bit.
    Tristate disableNvLowLatency2 = Tristate::Auto;

    // Hides integrated GPUs if dedicated GPUs are
    // present. May be necessary for some games that
    // incorrectly assume monitor layouts.
    bool hideIntegratedGraphics = false;

    /// Clears all mapped memory to zero.
    bool zeroMappedMemory = false;

    /// Maps host-visible memory below 4 GB when the driver and the
    /// platform window allow it. Falls back to a normal mapping otherwise.
    bool lowMemoryPlaced = true;

    /// WC3 forced 3.1.1: accept adapters that lack extension-provided
    /// required features. Core Vulkan features and VK_KHR_swapchain remain
    /// mandatory; each ignored feature is logged and stays disabled.
    bool ignoreMissingExtensions = false;

    /// WC3 D3D9 3.1.1: create explicit VkShaderModule objects for
    /// pipeline creation instead of chaining VkShaderModuleCreateInfo
    /// through VkPipelineShaderStageCreateInfo. Some Android drivers
    /// advertise VK_KHR_maintenance5 but fail to compile pipelines that
    /// use the pNext shader-module path. Enabled by default in the WC3
    /// D3D9 archive; set dxvk.useShaderModules = false to opt out.
    bool useShaderModules = true;

    /// Allows full-screen exclusive mode on Windows
    bool allowFse = false;

    /// Whether to enable tiler optimizations
    Tristate tilerMode = Tristate::Auto;

    /// Overrides memory budget for DXVK
    VkDeviceSize maxMemoryBudget = 0u;

    /// Whether to use custom sin/cos approximation
    Tristate lowerSinCos = Tristate::Auto;

    /// Enables implicit resolves that are used to
    /// deal with MSAA-related undefined behaviour.
    bool enableImplicitResolves = true;

    /// Enables NV_raw_access_chains extension on Nvidia
    bool enableNvRawAccessChains = true;

    /// Enables CUDA interop extensions if available
    bool enableNvCudaInterop = true;

    /// Enable present timing features
    bool enablePresentTiming = true;

    /// Enable descriptor update templates
    bool enableDescriptorUpdateTemplates = env::is32BitHostPlatform();

    /// Whether scratch memory in shaders needs
    /// zero-initialization to work around game bugs.
    bool zeroShaderScratchMemory = false;

    /// Device name
    std::string deviceFilter;
  };

}
