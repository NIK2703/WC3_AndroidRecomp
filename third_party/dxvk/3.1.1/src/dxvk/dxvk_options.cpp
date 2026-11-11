#include "dxvk_options.h"

namespace dxvk {

  DxvkOptions::DxvkOptions(const Config& config) {
    enableDebugUtils      = config.getOption<bool>    ("dxvk.enableDebugUtils",       false);
    enableMemoryDefrag    = config.getOption<Tristate>("dxvk.enableMemoryDefrag",     Tristate::Auto);
    numCompilerThreads    = config.getOption<int32_t> ("dxvk.numCompilerThreads",     0);
    enableGraphicsPipelineLibrary = config.getOption<Tristate>("dxvk.enableGraphicsPipelineLibrary", Tristate::Auto);
    enableDescriptorHeap  = config.getOption<Tristate>("dxvk.enableDescriptorHeap",   Tristate::Auto);
    enableDescriptorBuffer = config.getOption<Tristate>("dxvk.enableDescriptorBuffer", Tristate::Auto);
    enableDescriptorUpdateTemplates = config.getOption<bool>("dxvk.enableDescriptorUpdateTemplates", false);
    enableUnifiedImageLayout = config.getOption<bool> ("dxvk.enableUnifiedImageLayouts", true);
    enableImplicitResolves = config.getOption<bool>   ("dxvk.enableImplicitResolves", true);
    enableNvRawAccessChains = config.getOption<bool>  ("dxvk.enableNvRawAccessChains", true);
    enableNvCudaInterop   = config.getOption<bool>    ("dxvk.enableNvCudaInterop",    true);
    enablePresentTiming   = config.getOption<bool>    ("dxvk.enablePresentTiming",    true);
    trackPipelineLifetime = config.getOption<Tristate>("dxvk.trackPipelineLifetime",  Tristate::Auto);
    useRawSsbo            = config.getOption<Tristate>("dxvk.useRawSsbo",             Tristate::Auto);
    hud                   = config.getOption<std::string>("dxvk.hud", "");
    tearFree              = config.getOption<Tristate>("dxvk.tearFree",               Tristate::Auto);
    latencySleep          = config.getOption<Tristate>("dxvk.latencySleep",           Tristate::Auto);
    latencyTolerance      = config.getOption<int32_t> ("dxvk.latencyTolerance",       1000);
    disableNvLowLatency2  = config.getOption<Tristate>("dxvk.disableNvLowLatency2",   Tristate::Auto);
    hideIntegratedGraphics = config.getOption<bool>   ("dxvk.hideIntegratedGraphics", false);
    zeroMappedMemory      = config.getOption<bool>    ("dxvk.zeroMappedMemory",       false);
    lowMemoryPlaced       = config.getOption<bool>    ("dxvk.lowMemoryPlaced",        true);
    zeroShaderScratchMemory = config.getOption<bool>  ("dxvk.zeroShaderScratchMemory", false);

    /* WC3: an explicit 3.1.1 choice is a compatibility override.  The
     * settings host latches "3" into WC3_DXVK_VERSION, so the archive can
     * keep accepting the adapter when an optional extension is missing
     * instead of failing startup.  A config option is also available for
     * diagnostics; the forced choice always wins. */
    std::string forcedVersion = env::getEnvVar("WC3_DXVK_VERSION");
    ignoreMissingExtensions = config.getOption<bool>("dxvk.ignoreMissingExtensions", false)
      || (!forcedVersion.empty() && forcedVersion[0] == '3');

    /* WC3 D3D9 3.1.1: Adreno 0.786 reports VK_KHR_maintenance5 but the
     * observed FF pipeline failure is consistent with the pNext shader-
     * module path.  Use explicit VkShaderModule objects by default; the
     * low-memory placed mapping and the descriptor paths are unaffected.
     * Set dxvk.useShaderModules = false to restore the maintenance5 path. */
    useShaderModules = config.getOption<bool>("dxvk.useShaderModules", true);

    allowFse              = config.getOption<bool>    ("dxvk.allowFse",               false);
    deviceFilter          = config.getOption<std::string>("dxvk.deviceFilter",        "");
    lowerSinCos           = config.getOption<Tristate>("dxvk.lowerSinCos",            Tristate::Auto);
    tilerMode             = config.getOption<Tristate>("dxvk.tilerMode",              Tristate::Auto);

    auto budget = config.getOption<int32_t>("dxvk.maxMemoryBudget", 0);
    maxMemoryBudget = VkDeviceSize(std::max(budget, 0)) << 20u;
  }

}
