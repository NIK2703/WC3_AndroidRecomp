# DXVK vendor boundary

This directory is the source vendor for both DXVK majors linked into
`libwc3gameactivity.so`.  The product never loads a DXVK DLL: the sources are
compiled into static archives and the two copies are symbol-isolated before the
final link.

| Path | Contents |
|---|---|
| `1.10.3/` | DXVK 1.10.3 source at `e4fd5e9` (tag `v1.10.3`) with the old tree's Android/static patches already applied |
| `3.1.1/` | DXVK 3.1.1 source at `23b07e9` with the same Android/static patches, the WC3 D3D9 compatibility patch and the Android debug patch |
| `3.1.1/build-win64-ucrt/` | `compile_commands.json`, Meson-generated headers and `pnp-id-table.c`; the build recipe, not build output |
| `1.10.3/build-win64-ucrt/` | the same recipe for the legacy backend |
| `shim/` | the POSIX Win32 surface (`dxvk_shim/*.h` + `dxvk_win32.cpp`) used by both trees, the shared low-memory window, and the 1.10.3 Vulkan fallback header |
| `patches/` | provenance snapshots of the upstream diffs, the WC3 compatibility and Android debug patches, and both WC3 low-memory placed mapping patches |
| `VENDOR_COMMIT` | exact upstream and submodule revisions |

The source trees are snapshots of the working trees used by the old build, so
the patch files are **not applied again** at build time.  They exist so the
vendor delta can be reviewed against upstream.  `tools/android/build_dxvk.ps1`
checks the compatibility markers before compiling.

Build:

```powershell
powershell -File tools\android\build_dxvk.ps1 -Variant debug
```

The script compiles all translation units from `compile_commands.json` with the
NDK clang, links the Win32 shim into the 1.10.3 archive, and runs
`tools/lift/csrc_local/dxvk_isolate_archive_elf.c` to produce
`build/android/dxvk/<variant>/libdxvk{1,3}_iso.a`.  Every compiler invocation
goes through ccache.  `build_app.ps1` runs this script before CMake.

## WC3 D3D9 compatibility patch

`patches/dxvk-3.1.1-wc3-d3d9.patch` makes `storageBuffer8BitAccess` and
extension-provided required features non-fatal for this D3D9-only archive when
the settings host forces 3.1.1 (`WC3_DXVK_VERSION=3`).  D3D9 never creates
8-bit storage buffers; Turnip on Adreno 7xx reports every other required
feature but reports this one false, so stock DXVK 3.1.1 rejects the adapter
before D3D9 can start.  Android's system Adreno 0.786 driver also reports
Vulkan 1.3 without maintenance6, so the patch falls back from
`vkCmdBindDescriptorSets2KHR` and `vkCmdPushConstants2KHR` to the core Vulkan
entry points.  The core descriptor-set call needs an explicit
graphics/compute bind point; the fallback splits a stage mask that covers both.
The forced mode also falls back from maintenance5 index-buffer binding,
depth-clip state and load-store-op-none attachment operations.  Android's
Adreno 0.786 driver advertises maintenance5, but the observed FF pipeline
failure is consistent with the pNext shader-module path, so the D3D9 archive
creates explicit `VkShaderModule` objects by default.  That is the path DXVK
1.10.3 uses successfully on the same device; the low-memory placed mapping and
descriptor paths are unchanged.  Core Vulkan 1.3 features, a graphics queue, the
push-constant limit and `VK_KHR_swapchain` remain mandatory: without them
there is no working device or presentation surface.
`dxvk.ignoreMissingExtensions` can enable the feature relaxation for a
diagnostic run; the forced choice always wins.  Set
`dxvk.useShaderModules = false` to restore the maintenance5 pNext path.
DXVK 1.10.3 does not require either capability.

`patches/dxvk-3.1.1-android-debug.patch` records the Android runtime fixes:
failed `Singleton` construction stays retryable instead of leaving a live use
count with a null object, the D3D9 interface throws on a null instance instead
of dereferencing it, and the Android build writes DXVK logs to stderr even
though the reused Windows compile flags define `_WIN32`.  The app redirects
stderr to `run.txt`.

Do not weaken the features in a shared D3D11/D3D12 build.  If the archive is
ever reused outside the D3D9 frontend, revisit this patch rather than keeping
the requirements disabled by default.

## WC3 low-memory placed mapping

`patches/dxvk-3.1.1-lowmem-placed.patch` and
`patches/dxvk-1.10.3-lowmem-placed.patch` make both DXVK trees place
host-visible memory below 4 GB when the physical device supports
`VK_KHR_map_memory2` and `VK_EXT_map_memory_placed`.  The thin D3D9 bridge
then hands the native pointer straight to the 32-bit guest, so `Lock`/`Unlock`
no longer copy through a low shadow buffer.  The feature is controlled by
`dxvk.lowMemoryPlaced` (default `true`) and falls back to the ordinary
`vkMapMemory` on any refusal.

The address window and the `MAP_FIXED_NOREPLACE` probe live in project code,
not in the DXVK diff: `shim/wc3_lowmem.h` and
`shim/dxvk_win32.cpp::wc3_lowmem_acquire()`.  The thin D3D9 staging arena
keeps the same window in its `g_keep` table, so it cannot take the low holes
before a placed mapping.  3.1.1 caps mapped chunks at 64 MB to fit the holes
left by ART; 1.10.3 already picks 64 MB for host-visible memory on a 64-bit
host.  The shim object is linked once, unprefixed, into the 1.10.3 archive and
is shared by the 3.1.1 archive at final link.

DXVK 1.10.3 bundles Vulkan 1.3.217 headers, which predate both extensions.
`shim/wc3_vulkan_lowmem.h` defines only the structures and the
`vkMapMemory2KHR` typedef the 1.10.3 path needs; `vulkan_loader.h` includes it
after `<vulkan/vulkan.h>`.  The 3.1.1 tree has current headers and never
includes that file.  1.10.3 remains the fallback for old Turnip drivers, but
the placed path is present and uses the same kill switch.  The WC3-specific
rationale and the measurement plan are in `docs/THIN_D3D9_COPY_PLAN.md`.

## Licence

DXVK is zlib/libpng.  The upstream `LICENSE` is present in both source trees;
the APK must keep the corresponding third-party notice.  The shim is project
code and has no separate third-party licence.
