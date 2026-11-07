# wc3_android_recomp

Static recompilation of Warcraft III: The Frozen Throne (1.27.1.7085) into a
native Android arm64-v8a port. The target pipeline lifts the original x86-32
code to C and compiles it with the Android NDK/Clang for ARM64.

This repository is the canonical target tree. This README describes the target
design, project boundaries, and current development status; it does not claim
that the port is finished. Detailed migration state is tracked in
`config/migration.json`.

## Development status

At the time of writing, the graphics subsystem is fully debugged. The port can
launch a melee map or a campaign mission.

Known issues:

* Audio is unstable: periodic clicks are audible, and music occasionally drops
  out.
* The game freezes intermittently.

## Target

* Warcraft III: The Frozen Throne 1.27.1.7085, PE32/x86.
* Android 8.0+ (API 26), arm64-v8a, Vulkan.
* The shipped product is `libwc3gameactivity.so`. The only shipped target is
  Android arm64-v8a; recompiled code is not built, linked, or shipped for
  Windows.
* `--target headless` builds `wc3_headless`, a verification-only device under
  test (DUT) for T1. It is not part of the APK and is not a product.
* The oracle is the original licensed 1.27.1.7085 game on Windows and its
  recorded reference captures. The recompiled code is not built for Windows.
* Guest images use fixed, corpus-defined base addresses in the low 4 GB, with
  identity mapping to host pointers. Changing those bases requires a relift.

## Controls

| Action | Touch gesture |
| --- | --- |
| Move the camera | Quick swipe |
| Select a group of units | Double tap with a swipe, or long tap with a swipe |
| Add units to a group | Swipe on button 1-8 |
| Rebind a group | Long press on button 1-8 |
| Left mouse click | Single tap |
| Right mouse click | Double tap |
| Attack command (A + left click) | Long tap |
| Open the game menu | Long press on Esc |

## Graphics

* D3D9 rendering goes through a thin bridge to DXVK.
* The dual-backend selector and fallback are provisional until they are
  measured.
* The Vulkan ICD is selected explicitly through `VulkanDriverApi` (`system` or
  `turnip`).

## Runtime

* Guest threads are real host threads. `CreateThread` and `_beginthreadex`
  create a host thread with its own CPU state, guest stack, TEB, TLS state, and
  dispatcher state, within the fixed guest-thread limit.
* `ThreadApi`, `SyncApi`, `AssetApi`, and `VulkanDriverApi` are required
  platform interfaces. `NetApi` is not included; LAN and Battle.net are out of
  scope.
* Guest-visible events, mutexes, semaphores, and critical sections are HLE
  objects; their behavior must follow Win32 semantics. No-op waits and stub
  synchronization are not allowed.
* No host lock may be held across `dispatch()`, a callback into the guest, or a
  wait on a guest-visible object. Per-object HLE locks, lock-free
  mailbox/ring atomics, and vendor-owned DXVK/`dxvk_win32` locks are allowed.
* High-level emulation (HLE) replaces only external OS and hardware APIs. Game
  logic, rendering, audio, MPQ handling, and JASS remain recompiled guest code.
* HLE is split by guest DLL and then by subsystem.
* The guest ABI is frozen at source level. Changes to the `CPU` struct,
  dispatch signatures, HLE thunk window, stack-cleanup formula, or image bases
  require a relift; appending fields to `CPU` is allowed.

## Data and verification

* `AssetApi` reads licensed MPQ archives from an app-specific directory or
  through SAF. MPQ parsing uses the lifted `Storm.dll`. Saves, replays, and
  registry data are kept in app-private storage.
* T0 covers asset-free static and unit tests. T1 is a difftest of the
  `wc3_headless` DUT against recorded captures from the original Windows game.
  T2 is device end-to-end testing.
* T1 runs the same corpus and HLE as the product; the platform and graphics
  frontend are the verification-specific parts.
* Gate results are diagnostic. `unavailable` and `missing` are recorded as
  such; they are not reported as passes and do not stop development by
  themselves.

## Debug and release builds

* All native debug code is compiled only under `#if WC3_DEBUG`. An undefined
  `WC3_DEBUG` evaluates to 0. Nested diagnostic subtags use
  `#if WC3_DEBUG && WC3_DEBUG_<TARGET>`.
* Every native diagnostic, logging, or tracing statement must use a tag from
  `runtime/diag/wc3_debug.h`, such as `WC3_DEBUG_HLE`, `WC3_DEBUG_GRAPHICS`,
  or `WC3_DEBUG_ASSETS`.
* Release builds use `-DWC3_DEBUG=0 -DNDEBUG` and zero subtags, so debug code
  is absent from the binary. Release hygiene applies to project code; DXVK and
  `third_party` follow their own allowlists.
* Release uses application ID `wc3.recomp`; debug uses `wc3.recomp.debug`.
  The namespace and JNI names remain `wc3.recomp`. Both variants can be
  installed side by side, with data isolated by UID. `--variant` selects the
  variant atomically for Gradle and the native build. Debug builds keep `-O2`.
* Numeric product failure codes remain in release builds.

## Building

The canonical Android build is driven by `tools/android/build_app.ps1`. It
requires ccache: `WC3_USE_CCACHE=ON` and `WC3_REQUIRE_CCACHE=ON`, so a missing
ccache is an error rather than a silent fallback.

Host-side T0 checks are run with:

```powershell
python tools/run_tests.py --unit
```

## Legal

This repository contains source code and tools only. Game assets and binaries
are not included. Users must provide their own licensed copy of Warcraft III.
The generated C code is derived from the game binaries, so game assets, DLLs,
and generated C must not be redistributed.
