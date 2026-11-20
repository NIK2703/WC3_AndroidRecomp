# DXVK patches

The vendored trees are already patched.  These files are provenance snapshots
against the upstream revisions in `../VENDOR_COMMIT`; they are not replayed by
the target build.

## Applied source state

| Tree | Android/static patch | WC3 compatibility | Android debug | WC3 low-memory | Android pacing | Android present | Fixed-function cache |
|---|---|---|---|---|---|---|---|
| `3.1.1/` | `dxvk-3.1.1-android.patch` (includes `dxvk-3.1.1-static.patch`) | `dxvk-3.1.1-wc3-d3d9.patch` | `dxvk-3.1.1-android-debug.patch` | `dxvk-3.1.1-lowmem-placed.patch` | `dxvk-3.1.1-android-pacing.patch` | `dxvk-3.1.1-android-present.patch` | `dxvk-3.1.1-ff-cache.patch` |
| `low-latency/` | same as 3.1.1, plus `dxvk-low-latency-android.patch` | same | same | same | same | same | same |
| `1.10.3/` | `dxvk-1.10.3-android.patch` (includes `dxvk-1.10.3-static.patch`) | `dxvk-1.10.3-wc3-d3d9.patch` | none | `dxvk-1.10.3-lowmem-placed.patch` | `dxvk-1.10.3-android-pacing.patch` | `dxvk-1.10.3-android-present.patch` | `dxvk-1.10.3-ff-cache.patch` |

The pacing snapshots are deltas on top of the already-patched vendored trees,
not on the upstream revisions; they are not replayed by the build.  They make
Android use an absolute `clock_nanosleep` deadline instead of DXVK's Windows
busy-wait timer branch.  The rationale is in `../../VENDOR_PATCHES.md`.

The present snapshots are deltas on the already-patched trees.  They keep the
swap chain when Android reports `VK_SUBOPTIMAL_KHR` from
`vkQueuePresentKHR`; that result means the frame was presented but the surface
is not pre-rotated.  In 3.1.1 the unpatched presenter treats any
non-`VK_SUCCESS` result as a reason to destroy and recreate every swap chain
image.  1.10.3's presenter already accepted the result, but its D3D9 layer
still recreated the swap chain from `SynchronizePresent`.  Both paths now
recreate only for a real error.  The rationale is in
`../../VENDOR_PATCHES.md`.

The 1.10.3 WC3 compatibility snapshot is a delta on the already-patched tree.
`D3D9DeviceEx::GetDeviceFeatures()` no longer forces a `VkPhysicalDeviceFeatures`
flag to `VK_TRUE`; it copies every flag from adapter support.  A missing
feature therefore cannot make `vkCreateDevice` return
`VK_ERROR_FEATURE_NOT_PRESENT`.  The dependent D3D9 paths degrade instead of
creating an invalid pipeline or sampler: point/wireframe fill clamps to solid,
user clip planes report zero and the fixed-function/dxso shaders omit
`ClipDistance`, BC formats are rejected by `CheckDeviceFormat` and texture
creation, anisotropy is clamped to 1, the unbound cube-array dummy view reuses
the cube view, and query reset falls back to `vkCmdResetQueryPool` when
`VK_EXT_host_query_reset` is absent.  Occlusion queries drop the precise flag
and `D3DQUERYTYPE_VERTEXSTATS` is refused when the corresponding feature is
missing.  The rationale is in `../../VENDOR_PATCHES.md`; the Mali-G57 MC2
device that exposed the original failure reports `fillModeNonSolid`,
`multiViewport`, `textureCompressionBC`, `shaderClipDistance` and
`shaderCullDistance` false on Vulkan 1.1.177.

The same snapshot carries runtime-gated `[DXVK_DEBUG]` stage markers for
`CreateDeviceEx`, `InitialReset`, swapchain/presenter and `vkCreateDevice`.
They use `Logger::debug` and appear only when the debug build sets
`DXVK_LOG_LEVEL=debug`; the Vulkan boundary tracer remains the primary
diagnostic channel (GB-4.8).

The fixed-function cache snapshots are deltas on the already-patched trees.
`UpdateFixedFunctionVS()` used to rebuild `transpose(inverse(View))` and every
enabled `D3D9Light` for each dirty world matrix; both depend only on the view
matrix and the light values.  The cache compares those inputs bitwise, so a
hit writes the same constants as a recompute.  The rationale and the measured
profile are in `../../VENDOR_PATCHES.md`.

`dxvk-low-latency-android.patch` is specific to the fork.  It is a delta on the
already-patched low-latency tree, not on the fork commit, like the pacing
snapshots.  The fork's new `sync_signal.h`/`sync_atomic_signal.h` use
`WaitOnAddress` under `_WIN32`, but
the DXVK shim defines `_WIN32` for the Android target; the patch selects the
fork's existing POSIX/futex path instead.  It also extends the WC3
forced-compatibility check from `"3"` to `"ll"`, so the fork gets the same
optional-feature relaxation as 3.1.1.  It also makes the low-latency presenter
record every submitted frame even when `VK_KHR_present_wait` is unavailable,
so the frame worker drains its 32-entry queue instead of stalling the CS
thread.  The fork commit itself supplies the framepacer; the 3.1.1 snapshots
above are replayed on top of it.

The Android patches add the Vulkan loader/WSI changes and the static-library
Meson shape; the static patches are included in them.  The WC3 patch makes
`storageBuffer8BitAccess` and `VK_KHR_maintenance6` optional in the D3D9
archive, and falls back from `vkCmdBindDescriptorSets2KHR` /
`vkCmdPushConstants2KHR` to the core Vulkan entry points.  See `../README.md`
for why this is safe for D3D9.  The Android debug patch makes failed singleton
construction retryable, guards the D3D9 interface against a null instance, and
lets the Android build log to stderr (the app redirects it to `run.txt`).

The low-memory patches place host-visible mappings below 4 GB through
`VK_EXT_map_memory_placed`; 1.10.3 also relies on the project-owned
`../shim/wc3_vulkan_lowmem.h` because its vendored Vulkan headers predate the
extension.  The address window is `../shim/wc3_lowmem.h`.  All three trees use
`dxvk.lowMemoryPlaced` as the runtime kill switch.

## Reviewing the delta

Compare a tree against its upstream revision and the snapshots:

```powershell
git -C third_party/dxvk/3.1.1 diff
git -C third_party/dxvk/low-latency diff
git -C third_party/dxvk/1.10.3 diff
```

`tools/android/build_dxvk.ps1` fails if the WC3 compatibility markers
(`storageBuffer8BitAccess, false`, optional maintenance6, and the command
fallbacks) for 3.1.1, the 1.10.3 optional-feature fallbacks, either low-memory
patch marker, either Android pacing marker, or the low-latency sync/`"ll"`
markers are absent.  The build does not apply
patches automatically because the working trees are already in the pinned
post-patch state.
