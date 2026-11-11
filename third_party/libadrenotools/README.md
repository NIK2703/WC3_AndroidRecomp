# libadrenotools / liblinkernsbypass

Transitional Android product input for ADR-036.  The target tree does not yet
vendor the upstream sources; it links the pinned archives produced by the
legacy build and packages the two hook DSOs.  `tools/android/build_apk.ps1`
copies them from `WC3_ADRENOTOOLS_DIR` (default: the pinned
`config/build_upstream.json` path plus `build/android-shared/adrenotools`) into
the per-build `jniLibs` directory and verifies the SHA-256 before packaging;
they are not committed because `*.so` is gitignored:

| file | sha256 |
|---|---|
| `libadrenotools.a` | `22d71eb19343af4a0a524c860b7caf258a7b92fdb1cd71c2eb44d49a91a69f1d` |
| `liblinkernsbypass.a` | `91a5893282ce4839e887e35e8726c2e5c6436c9e8192031039339abd968222fc` |
| `libmain_hook.so` | `be1cbb5774d76d1a06be0e2f67d4c11d681f9af9cb8959d5e2a6c0fa285bc103` |
| `libhook_impl.so` | `f95475bd98254c9172c9efa29ced69dc3404631fff96fbb589b4c919e6e9d855` |

Source: `github.com/bylaws/libadrenotools` and
`github.com/bylaws/liblinkernsbypass`, BSD-2-Clause, Copyright (c) 2021 Billy
Laws.  `LICENSE` is the upstream text.  The exact upstream commit is still an
open vendor item; replacing the archives with sources must pin it and update
`third_party/VENDOR_MANIFEST.md`.  The loader ABI used by
`runtime/platform/android/vulkan_driver_android.c` is only
`adrenotools_open_libvulkan()` plus `vkGetInstanceProcAddr` from its handle.
