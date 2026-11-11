# DXVK patches

The vendored trees are already patched.  These files are provenance snapshots
against the upstream revisions in `../VENDOR_COMMIT`; they are not replayed by
the target build.

## Applied source state

| Tree | Android/static patch | WC3 compatibility | Android debug | WC3 low-memory |
|---|---|---|---|---|
| `3.1.1/` | `dxvk-3.1.1-android.patch` (includes `dxvk-3.1.1-static.patch`) | `dxvk-3.1.1-wc3-d3d9.patch` | `dxvk-3.1.1-android-debug.patch` | `dxvk-3.1.1-lowmem-placed.patch` |
| `1.10.3/` | `dxvk-1.10.3-android.patch` (includes `dxvk-1.10.3-static.patch`) | none | none | `dxvk-1.10.3-lowmem-placed.patch` |

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
extension.  The address window is `../shim/wc3_lowmem.h`.  Both patches use
`dxvk.lowMemoryPlaced` as the runtime kill switch.

## Reviewing the delta

Compare a tree against its upstream revision and the snapshots:

```powershell
git -C third_party/dxvk/3.1.1 diff
git -C third_party/dxvk/1.10.3 diff
```

`tools/android/build_dxvk.ps1` fails if the WC3 compatibility markers
(`storageBuffer8BitAccess, false`, optional maintenance6, and the command
fallbacks) or either low-memory patch marker is absent.  The build does not
apply patches automatically because the working trees are already in the
pinned post-patch state.
