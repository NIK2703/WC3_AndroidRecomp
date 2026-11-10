# wc3_android_recomp

Static recompilation of Warcraft III: The Frozen Throne (1.27.1.7085) into a
native Android arm64-v8a port. The pipeline lifts the original x86-32 code to
C and compiles it with the Android NDK/Clang for ARM64.

## Development status

The port can launch a melee map or a campaign mission. Audio playback and
in-game freezes are fixed. Saving, loading, and LAN play work.

<img width="400" height="180" alt="Screenshot_2026-11-07-20-34-58-138_wc3 recomp debug" src="https://github.com/user-attachments/assets/b7035652-e9e1-45c2-9f56-d20b77378dfd" />
<img width="400" height="180" alt="Screenshot_2026-11-07-20-39-40-093_wc3 recomp debug" src="https://github.com/user-attachments/assets/539d2d96-c366-47a8-8abb-1babd0b6d360" />
<img width="400" height="180" alt="Screenshot_2026-11-07-20-36-15-019_com miui securitycenter" src="https://github.com/user-attachments/assets/32a04fea-0b6e-4027-9b8a-c1b722f82c8e" />

## Launchers

The APK has two launcher entries that share the same game data. A normal tap
starts the selected game immediately. There is no intro screen.

| Launcher entry | Game host | Label | Host process | State directory | Guest command |
| --- | --- | --- | --- | --- | --- |
| `.Wc3RocLauncherActivity` | `.Wc3RocActivity` | Reign of Chaos | `:roc` | `files/roc` | `war3.exe -classic` |
| `.Wc3TftLauncherActivity` | `.Wc3GameActivity` | Frozen Throne | default | `files` | `war3.exe` |

The launcher activities validate the saved settings and start the native host
without showing a window. The settings screen is not a launcher entry. To open
it, long-press the game icon and choose **Launch settings** (or
**Настройки запуска**). The static shortcut sends
`wc3.recomp.action.OPEN_SETTINGS` to the launcher activity. The screen is
available in English and Russian.

Launch settings can override the game directory, guest resolution, sound,
and extra `war3.exe` arguments. The default game directory is
`/storage/emulated/0/Warcraft 3`. You can type the path, find it automatically,
or choose it with the system folder picker (SAF). The picker result must map to
a real filesystem path because the native host reads the licensed files with
POSIX calls. Resolution is chosen by height from 360 px in 180 px steps up to
the screen height, and the width comes from the display aspect ratio. The
native entry keeps the current surface size. The selected path reaches the
native host through `WC3_GAME_DATA_DIR`. Resolution maps to
`WC3_DXVK_WIDTH`/`WC3_DXVK_HEIGHT`, and sound-off maps to `-nosound` in
`WC3_ARGS`. The default process stores the values and passes them to both hosts
through the launch intent, so the `:roc` process never reads another process's
preferences.

The debug build adds `(debug)` to both labels, targets `wc3.recomp.debug` from
its shortcuts, and installs next to the release build. The APK does not ship
game assets. The game fills the display cutout and republishes its surface
after a resume. The game stores in-game options in the guest registry under
`Software\Blizzard Entertainment\Warcraft III` and writes them to
`wc3_registry.dat` in the active state directory.

## Target

* Warcraft III: The Frozen Throne 1.27.1.7085, PE32/x86.
* Android 8.0+ (API 26), arm64-v8a, Vulkan.
* The shipped product is `libwc3gameactivity.so`. The only shipped target is
  Android arm64-v8a. Recompiled code is not built, linked, or shipped for
  Windows.
* `--target headless` builds `wc3_headless`, a verification-only device under
  test (DUT) for T1. It is not part of the APK and is not a product.
* The oracle is the original licensed 1.27.1.7085 game on Windows and its
  recorded reference captures.
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

* D3D9 rendering goes through a bridge to DXVK.
* The dual-backend selector and fallback are provisional. They have not been
  measured.
* The app selects the Vulkan ICD through `VulkanDriverApi` (`system` or
  `turnip`).

## Runtime

* Guest threads are real host threads. `CreateThread` and `_beginthreadex`
  create a host thread with its own CPU state, guest stack, TEB, TLS state, and
  dispatcher state, within the fixed guest-thread limit.
* `ThreadApi`, `SyncApi`, `AssetApi`, `VulkanDriverApi`, and `NetApi` are
  required platform interfaces. `NetApi` is the LAN boundary (ADR-031).
  Battle.net and Internet services are the next phases after LAN.
* Guest-visible events, mutexes, semaphores, and critical sections are HLE
  objects. Their behavior must follow Win32 semantics. No-op waits and stub
  synchronization are not allowed.
* No host lock may be held across `dispatch()`, a callback into the guest, or a
  wait on a guest-visible object. Per-object HLE locks, lock-free
  mailbox/ring atomics, and vendor-owned DXVK/`dxvk_win32` locks are allowed.
* High-level emulation (HLE) replaces only external OS and hardware APIs. Game
  logic, rendering, audio, MPQ handling, and JASS remain recompiled guest code.
* HLE is split by guest DLL and then by subsystem.
* The guest ABI is frozen at source level. Changes to the `CPU` struct,
  dispatch signatures, HLE thunk window, stack-cleanup formula, or image bases
  require a relift. Appending fields to `CPU` is allowed.

## Data and verification

* `AssetApi` reads licensed MPQ archives from an app-specific directory or
  through SAF. MPQ parsing uses the lifted `Storm.dll`. The app keeps saves,
  replays, and registry data in app-private storage.
* T0 covers asset-free static and unit tests. T1 is a difftest of the
  `wc3_headless` DUT against recorded captures from the original Windows game.
  T2 is device end-to-end testing.
* T1 runs the same corpus and HLE as the product. The platform and graphics
  frontend are the verification-specific parts.
* Gate results are diagnostic. `unavailable` and `missing` are recorded as
  such. They are not reported as passes and do not block development.

## Debug and release builds

* All native debug code is compiled only under `#if WC3_DEBUG`. An undefined
  `WC3_DEBUG` evaluates to 0. Nested diagnostic subtags use
  `#if WC3_DEBUG && WC3_DEBUG_<TARGET>`.
* Every native diagnostic, logging, or tracing statement must use a tag from
  `runtime/diag/wc3_debug.h`, such as `WC3_DEBUG_HLE`, `WC3_DEBUG_GRAPHICS`,
  or `WC3_DEBUG_ASSETS`.
* Release builds use `-DWC3_DEBUG=0 -DNDEBUG` and zero subtags, so debug code
  is absent from the binary. Release build rules apply to project code. DXVK
  and `third_party` follow their own allowlists.
* Release uses application ID `wc3.recomp`. Debug uses `wc3.recomp.debug`.
  The namespace and JNI names remain `wc3.recomp`. Both variants can be
  installed side by side, with data isolated by UID. `--variant` selects the
  variant atomically for Gradle and the native build. Debug builds keep `-O2`.
* Numeric product failure codes remain in release builds.

## Building

The Android build uses `tools/android/build_app.ps1`. It requires ccache:
`WC3_USE_CCACHE=ON` and `WC3_REQUIRE_CCACHE=ON`, so a missing ccache is an
error rather than a silent fallback.

`tools/android/build_apk.ps1` packages the APKs:

```powershell
tools\android\build_apk.ps1                 # debug, wc3.recomp.debug
tools\android\build_apk.ps1 -Release        # release, wc3.recomp
```

Run host-side T0 checks with:

```powershell
python tools/run_tests.py --unit
```

## Legal

This repository contains source code and tools only. Game assets and binaries
are not included. Users must provide their own licensed copy of Warcraft III.
The generated C code is derived from the game binaries, so game assets, DLLs,
and generated C must not be redistributed.
