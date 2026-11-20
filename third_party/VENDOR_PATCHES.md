# VENDOR_PATCHES

Local deltas carried by vendored dependencies.  Each entry names the upstream
revision, the patch snapshot and the reason.

## DXVK 3.1.1

* Upstream: `23b07e9be441f87eb46204d78d9265e565cfbc0f` (`VENDOR_COMMIT`).
* Applied source state: `third_party/dxvk/3.1.1/`.
* Patches:
  * `third_party/dxvk/patches/dxvk-3.1.1-android.patch` — Android Vulkan
    loader/WSI, static archive shape and the local DXVK memory probes.
  * `third_party/dxvk/patches/dxvk-3.1.1-static.patch` — static-library Meson
    shape (included in the Android patch snapshot).
  * `third_party/dxvk/patches/dxvk-3.1.1-wc3-d3d9.patch` — makes
    `storageBuffer8BitAccess` and extension-provided required features
    non-fatal when the settings host forces 3.1.1 (`WC3_DXVK_VERSION=3`),
    and falls back to core entry points for maintenance5 index-buffer binds,
    maintenance6 descriptor-set/push-constant calls, depth-clip state and
    load-store-op-none attachment operations.  D3D9 never creates 8-bit
    storage buffers; Turnip on Adreno 7xx reports the feature false;
    Android's system Adreno 0.786 driver lacks maintenance6, so stock 3.1.1
    rejects the adapter or calls a null command.  The same driver advertises
    maintenance5, but the observed FF pipeline failure is consistent with the
    pNext shader-module path; the D3D9 archive therefore creates explicit
    `VkShaderModule` objects by default, matching the path DXVK 1.10.3 uses
    successfully.  `dxvk.useShaderModules = false` restores the pNext path.
    Core Vulkan 1.3 features, a graphics queue, the push-constant limit and
    `VK_KHR_swapchain` remain mandatory.  See `third_party/dxvk/README.md` and
    ADR-036.
  * `third_party/dxvk/patches/dxvk-3.1.1-android-debug.patch` — makes failed
    singleton construction retryable, guards the D3D9 interface against a null
    instance, and keeps DXVK stderr logging in the Android build.  The app
    redirects stderr to `run.txt`.
  * `third_party/dxvk/patches/dxvk-3.1.1-android-pacing.patch` — treats
    Android as POSIX inside `Sleep` even though the shim defines `_WIN32`, and
    sleeps on an absolute `CLOCK_MONOTONIC` deadline with
    `clock_nanosleep(TIMER_ABSTIME)` instead of the Windows
    `4 ms + duration/6` busy-wait tail.  Device profile 2026-11-11: presenter
    thread 32.52% of app CPU, 66.85% of it in `__kernel_clock_gettime`; at
    60 FPS the old path spun about 6.8 ms of every 16.7 ms frame.
  * `third_party/dxvk/patches/dxvk-3.1.1-android-present.patch` — treats
    `VK_SUBOPTIMAL_KHR` as a success result in `Presenter::acquireNextImage()`
    and `Presenter::presentImage()`.  Android's WSI returns it from
    `vkQueuePresentKHR` whenever the swap chain's `preTransform` differs from
    `NATIVE_WINDOW_TRANSFORM_HINT`; DXVK creates every swap chain with
    `IDENTITY`, so a landscape activity on a portrait-natural display got it
    on every frame.  DXVK then set `m_dirtySwapchain` and destroyed/recreated
    all 5 images, 10 semaphores and 5 fences per frame; the captured menu run
    logged 48,921 recreations.  The frame had already been presented, so only
    negative results recreate the swap chain now.  DXVK 1.10.3 already used
    this rule in `vulkan_presenter.cpp`; 3.1.1 lost it.
  * `third_party/dxvk/patches/dxvk-3.1.1-lowmem-placed.patch` — places
    host-visible memory below 4 GB with `VK_EXT_map_memory_placed` so the thin
    D3D9 bridge can hand the pointer to the 32-bit guest without a shadow
    copy; `dxvk.lowMemoryPlaced` (default true) is the kill switch.  The
    address window is project code in `third_party/dxvk/shim/wc3_lowmem.h`.
    See `docs/THIN_D3D9_COPY_PLAN.md`.
  * `third_party/dxvk/patches/dxvk-3.1.1-ff-cache.patch` — caches the
    fixed-function view-derived constants.  `transpose(inverse(View))` and
    the per-light `D3D9Light` transforms (including the two `cosf` calls)
    depend only on the view matrix and the enabled light values, but
    `UpdateFixedFunctionVS()` rebuilt them for every dirty world matrix.
    The cache compares the view matrix and the source `D3DLIGHT9` values
    bitwise, so a hit produces exactly the same bytes as a recompute.
    Profile `build/profiles/deep-render-20261112-2110`: the fixed-function
    update was 15.91% of main-thread inclusive CPU, `inverse` 7.10% and
    `D3D9Light` 4.27%.
* Build: `tools/android/build_dxvk.ps1 -Variant <release|debug>`.

## DXVK low-latency

* Upstream: `netborg-afps/dxvk-low-latency` commit
  `33ba69f4985cdc56b9fe7bdce763d6bc068f2792` (tag
  `low-latency-framepacing-3.1.1`), based on DXVK 3.1.1 and carrying the fork's
  framepacer.  Submodule pins are in `third_party/dxvk/VENDOR_COMMIT`.
* Applied source state: `third_party/dxvk/low-latency/`.
* Patches: the same `dxvk-3.1.1-*` snapshots listed above, replayed on the
  fork's tree.  The only fork-specific snapshot is
  `third_party/dxvk/patches/dxvk-low-latency-android.patch`, a delta on the
  already-patched low-latency tree:
  * routes the fork's new `sync_signal.h` and `sync_atomic_signal.h` to the
    POSIX/futex path on Android.  The DXVK shim defines `_WIN32` for the
    Android target, so the fork's `#ifdef _WIN32` branches tried to include
    `synchapi.h` and use `WaitOnAddress`, neither of which exists there.
  * extends the WC3 forced-compatibility check from `"3"` to `"ll"`, so the
    low-latency archive gets the same optional-feature relaxation as 3.1.1.
  * records every completed present in `Presenter::signalFrame()` even when
    `VK_KHR_present_wait` is unavailable, and lets the frame worker own
    completion signalling and fps limiting.  Without this, the 32-entry frame
    queue fills and the CS thread stalls after 32 presents.
* Build: `tools/android/build_dxvk.ps1` compiles 201 translation units into
  `libdxvkll.a` and isolates them as `libdxvkll_iso.a` with the `wc3_dxvkll_`
  prefix.  The app selects it with `WC3_DXVK_VERSION=ll`; a forced low-latency
  failure does not fall back to 3.1.1 or 1.10.3.

## DXVK 1.10.3

* Upstream: `e4fd5e9e8d335e8a2c0814829207cbd421f7e40e` (tag `v1.10.3`).
* Applied source state: `third_party/dxvk/1.10.3/`.
* Patches:
  * `third_party/dxvk/patches/dxvk-1.10.3-android.patch` — Android Vulkan
    loader/WSI and UCRT header fixes.
  * `third_party/dxvk/patches/dxvk-1.10.3-static.patch` — static-library shape
    and header fixes (included in the Android patch snapshot).
  * `third_party/dxvk/patches/dxvk-1.10.3-lowmem-placed.patch` — same
    low-memory placed mapping as 3.1.1, implemented in the older inline
    allocator.  Vulkan 1.3.217 headers lack both extensions, so
    `third_party/dxvk/shim/wc3_vulkan_lowmem.h` supplies the missing
    definitions.  `dxvk.lowMemoryPlaced` (default true) is the kill switch.
  * `third_party/dxvk/patches/dxvk-1.10.3-android-present.patch` — treats
    `VK_SUBOPTIMAL_KHR` as success in `D3D9SwapChainEx::SynchronizePresent`.
    The 1.10.3 presenter already kept its swap chain after a suboptimal
    present, but the D3D9 layer checked the previous present result and
    recreated the swap chain on the next frame.  On a forced-1.10.3 session
    with a landscape activity on a portrait-natural display, Android returned
    `VK_SUBOPTIMAL_KHR` from every present because `preTransform` was
    `IDENTITY` while `currentTransform` was `ROTATE_180`; the log showed a
    destroy/create pair between every two presents.  Accepting the result is
    correct per Vulkan: the frame was displayed, and only negative results
    require a new swap chain.
  * `third_party/dxvk/patches/dxvk-1.10.3-ff-cache.patch` — the same
    fixed-function view/light cache as 3.1.1, adapted to this tree's
    `D3D9Light` construction and `enabledLightIndices` list.  This is the
    active backend in the measured configuration.
  * `third_party/dxvk/patches/dxvk-1.10.3-wc3-d3d9.patch` — makes every
    feature requested by `D3D9DeviceEx::GetDeviceFeatures()` come from
    adapter support instead of forcing `VK_TRUE`, and gates the dependent
    D3D9 paths.  See below.
* WC3 optional-feature compatibility: the original Mali-G57 MC2 log (driver
  32.1.0, Vulkan 1.1.177) reached `DxvkAdapter::createDevice` and failed with
  `VK_ERROR_FEATURE_NOT_PRESENT` because 1.10.3 forced `fillModeNonSolid`,
  `multiViewport`, `textureCompressionBC`, `shaderClipDistance` and
  `shaderCullDistance`, all false on that device.  The patch does not replace
  1.10.3 and does not hide a fundamental requirement: it copies all requested
  features from the adapter, degrades the corresponding `GetDeviceCaps`
  values, clamps fill mode to `VK_POLYGON_MODE_FILL`, returns zero user clip
  planes and omits the `ClipDistance` capability from FF/dxso shaders,
  rejects BC formats in `CheckDeviceFormat` and texture creation, clamps
  anisotropy to 1, reuses the cube dummy view when `imageCubeArray` is
  absent, falls back to `vkCmdResetQueryPool` without host query reset, and
  refuses precise occlusion or pipeline-statistics queries the device cannot
  execute.
  3.1.1 remains the Vulkan 1.3 path; 1.10.3 is the fallback that this device
  actually selects.  The patch also carries runtime-gated `[DXVK_DEBUG]`
  stage markers for `CreateDeviceEx`, `InitialReset`, swapchain/presenter and
  `vkCreateDevice`; they appear only when the debug build sets
  `DXVK_LOG_LEVEL=debug`, and the Vulkan boundary tracer remains primary
  (GB-4.8).

## dxvk_win32 shim

* Project-owned code, moved to `third_party/dxvk/shim/` from the old tree's
  `runtime/android/dxvk_win32.cpp` and `runtime/android/dxvk_shim/`.
* It implements the Win32 surface DXVK compiles against.  It is not a guest
  runtime and is not in the frozen guest ABI.
* `wc3_lowmem_acquire()` (`wc3_lowmem.h`, `dxvk_win32.cpp`) owns the
  `[0x01000000, 0x31000000)` address window and the
  `MAP_FIXED_NOREPLACE` probe used by both placed mappings.  The shim
  object stays unprefixed and is linked once from the 1.10.3 archive.
* `wc3_vulkan_lowmem.h` supplies the `VK_KHR_map_memory2` /
  `VK_EXT_map_memory_placed` names, structures and the `vkMapMemory2KHR`
  typedef for DXVK 1.10.3's Vulkan 1.3.217 headers.  It is included only by
  that tree's `vulkan_loader.h`; 3.1.1 uses its own current headers.
