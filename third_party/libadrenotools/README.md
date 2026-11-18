# libadrenotools / liblinkernsbypass

BSD-2-Clause Android product inputs for ADR-036.  Two provenances are
accepted by `cmake/Wc3LegacyBoot.cmake`:

1. the historical pinned legacy build (opaque archives), and
2. a source build from the commits in `VENDOR_COMMIT` via
   `tools/android/build_adrenotools.sh`.

The source build writes `build/android-shared/adrenotools/source-build.json`
with the two commits and the archive hashes.  `build_app.sh` passes
`-DWC3_ADRENOTOOLS_TRUST_MANIFEST=ON` when that file exists; CMake then
rechecks the commits against `VENDOR_COMMIT` and accepts the recorded hashes,
so a source build works on a different host without editing the fixed legacy
hash list.  Both hook DSOs are still the pinned transitional files and are
copied from a built APK or a pinned directory by
`tools/android/build_adrenotools.sh --from-apk <apk>`; they are not committed
because `*.so` is gitignored.

| file | sha256 |
|---|---|
| `libadrenotools.a` (legacy pinned build) | `22d71eb19343af4a0a524c860b7caf258a7b92fdb1cd71c2eb44d49a91a69f1d` |
| `liblinkernsbypass.a` (legacy pinned build) | `91a5893282ce4839e887e35e8726c2e5c6436c9e8192031039339abd968222fc` |
| `libadrenotools.a` (source build, commit `8fae8ce`) | `45e4cc2d2cf6ef29f1e96b9565983ca47c4afcec1a16c0ad782bd63cf91be73e` |
| `liblinkernsbypass.a` (source build, commit `aa39758`) | `36c16f6a23c97d7aa33010ece92d6b5eda1a8b6f43782c0c7206a9153882622e` |
| `libmain_hook.so` | `be1cbb5774d76d1a06be0e2f67d4c11d681f9af9cb8959d5e2a6c0fa285bc103` |
| `libhook_impl.so` | `f95475bd98254c9172c9efa29ced69dc3404631fff96fbb589b4c919e6e9d855` |

Source: `github.com/bylaws/libadrenotools` and
`github.com/bylaws/liblinkernsbypass`, BSD-2-Clause, Copyright (c) 2021 Billy
Laws.  `LICENSE` is the upstream text.  The source pin is
`third_party/libadrenotools/VENDOR_COMMIT`; the rebuild uses the NDK prebuilt
selected from the host (`linux-aarch64`, `linux-x86_64`, `darwin-*` or
`windows-x86_64`) with a Release CMake build (`-O3`, `-fPIC`) for API 26.
The loader
ABI used by `runtime/platform/android/vulkan_driver_android.c` is only
`adrenotools_open_libvulkan()` plus `vkGetInstanceProcAddr` from its handle.
