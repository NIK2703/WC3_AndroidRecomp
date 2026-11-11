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
* Build: `tools/android/build_dxvk.ps1 -Variant <release|debug>`.

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
* No WC3 feature-requirement change; 1.10.3 remains the fallback path, now
  also carrying the low-memory placed patch.

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
