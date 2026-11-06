# Follow-up critique: revised PLAN_REFACTOR_1.27B

Date: 2026-11-06. Scope: current plan, architecture, `tools/check_docs.py`,
and the repository state after Step 1 and Step 2a. This is not a repeat of
`reports/REFACTOR_PLAN_CRITIQUE_2.md`; it checks whether the revised plan is
now executable and internally consistent.

Method: read the plan, architecture, checker, `AGENTS.md`, `README.md`, and
the relevant build/runtime files; ran `tools/android/audit_import_arity.py`,
`tools/check_docs.py` (default, `--strict`, `--strict-final`),
`tools/gen_d3d9_thin_bridge.py --check`, file/size checks, and ELF section
inspection. No repository file was edited except this report.

## Summary

| ID | Sev | Area | Finding |
|---|---|---|---|
| P0-1 | P0 | Steps 2b/2c-7 | There is no executable baseline: the host is 0 bytes, `adb` is absent, and there are no licensed PEs. Every "boot smoke" acceptance in Steps 2c-7 is unreachable until Step 8, which is after them. |
| P0-2 | P0 | Steps 2b/3c | Step 3c unconditionally requires an import fixture and an arity spec. Step 2b says the fixture may not exist without a licensed PE, and no step creates `config/hle_arity.1.27b.json`. |
| P1-1 | P1 | Step 2b | `smoke.py verify` can exit 0 after an `ok -> empty/unavailable` regression. The baseline gate can pass while the host is broken. |
| P1-2 | P1 | Step 2c | The layout manifest omits VA arenas/guard pages, and the corpus manifest has no reference address sets. The Android build still has a manual `_a35` branch in `build_boot_probe.ps1`. |
| P1-3 | P1 | Step 3a | Steps 3a/3b invoke `tools/run_tests.py --test hle_abi`, but `run_tests.py` is first created in Step 8 and `tests/hle_abi_test.c` in Step 3c. |
| P1-4 | P1 | Step 3a | The adapter compares `(dll,name,argc,abi)` for both registries. That comparison will fail on the 16 cdecl arity differences that the same acceptance allows. |
| P1-5 | P1 | Step 3b | `HLE_OPTIONAL` has no defined call-time behavior, and the 104 missing imports include plausible live paths (`winmm!waveOut*`, `msvcr80!getenv`). |
| P1-6 | P1 | Step 4 | `gr_create(const GuestConfig *, const Platform *)` depends on `runtime/platform/platform.h`, which Step 5 introduces. |
| P1-7 | P1 | Step 5 | `check_arch.py` is supposed to ban `getenv` in HLE, but 155 `getenv("WC3_*")` calls remain and the migration is scheduled for Step 9. |
| P1-8 | P1 | Step 5 | `hle_android.c < 2 000` lines cannot be met by extracting only kernel32/msvcr80; 124 non-kernel entries and all audio/input code remain until Step 6. |
| P1-9 | P1 | Step 6 | Device tests (home/resume, rotation, surface loss, audio device loss, save while paused) are prose with no command and no device/`adb` runner. |
| P1-10 | P1 | Step 6 | `WindowApi`, `InputApi`, `AudioApi`, `ClipboardApi` are used in Step 6 but are not defined in Step 5's platform header. |
| P1-11 | P1 | Step 7a | The A/B gate has no replay scene, frame-hash definition, performance threshold, or tool. `-LegacyD3D9` is `hle_d3d9.cpp` + `hle_d3d9_dxvk.cpp`, not `D9VkApi` (which does not exist). |
| P1-12 | P1 | Step 8 | CI is not specified: no root CI config, no self-hosted runner, and no step creates `check_manifest.py` or `run_tests.py` before their acceptance use. |
| P1-13 | P1 | Step 8 | The 120 MiB `.so` / 70 MiB APK budget is not supported by measured artifacts: the final release `.so` is 221.7 MB with 160 MB of `.text`, and the release APK is 129 MB. |
| P1-14 | P1 | Step 9 | The baseline is stored in `build/smoke_baseline.json`, but Step 9 deletes unused `build*` directories. There is also no step that refreshes the Step 2b empty/unavailable baseline after the host/device exist. |
| P1-15 | P1 | Policy | The checker verifies only the absence of a DXVK prohibition in `AGENTS.md`; it does not verify the positive embedded-only policy or `README.md`. The code still has `WC3_DXVK_VERSION` and a runtime external Vulkan ICD path. |

P2 findings are in the compact table in section 4.

## 1. P0 findings

### P0-1. No executable baseline; the plan's boot-smoke gates cannot run

**Where:** `PLAN_REFACTOR_1.27B.md:236`, `:281`, `:306`, `:328`, `:352`,
`:379`, `:425`, `:478`; `docs/ARCHITECTURE_1.27B.md:984-994`;
`PLAN_REFACTOR_1.27B.md:195-202`.

**Evidence:**

* `build27/wc3_host27.exe` is 0 bytes.
* `adb` is absent from PATH; there is no attached device in this environment.
* `refs/` contains only `PINS.md`; there is no game PE, MPQ, or asset set.
* `build/android/boot_probe/wc3_boot_probe` is 28,622,512 bytes, but it is an
  Android arm64 ELF, not a Windows-runnable host and not executable here.
* Step 8 (`PLAN_REFACTOR_1.27B.md:446-455`) is the first step that builds both
  targets from the manifest. Steps 2c-7 all require "host/Android boot smoke"
  before Step 8.
* Step 2b explicitly accepts `host exe=empty, adb=unavailable` with
  `smoke.py verify` exit 0 (`:199`). That is a green step with no executable
  artifact, and it contradicts the architecture's Definition of Done
  (`docs/ARCHITECTURE_1.27B.md:990-992`: `ok` for every artifact that must be
  executable).

**Why it fails:** the plan cannot tell whether a layout/corpus/HLE change
preserved boot behavior, because no boot-capable host or device is available
until after the steps that depend on it. The existing `tools/build_host27.ps1`
has a Windows build path, but no step runs it before the boot-smoke gates.

**Correction:** add an explicit execution-baseline step after 2b (2d or a
pre-2c gate): run `tools/build_host27.ps1` (or `wc3build.py build --target
windows`) and require a non-empty `build27/wc3_host27.exe` plus a boot marker.
Provision `adb` + a device, or record device checks as machine-readable
`unavailable` and move the device-dependent acceptance to Step 8/9. Gate the
2c-7 "boot smoke unchanged" claims on that artifact. If the host cannot be
built, say so and move Step 8's build unification before 2c; do not call an
empty-host baseline green.

### P0-2. Step 3c requires a fixture and arity spec that do not exist and may never exist

**Where:** `PLAN_REFACTOR_1.27B.md:189-193`, `:298-300`;
`docs/ARCHITECTURE_1.27B.md:1069-1075`;
`tools/import_coverage.py:39-45`.

**Evidence:**

* `config/imports.1.27b.json` and `config/hle_arity.1.27b.json` do not exist.
* `refs/` has no PE; `config/imports.1.27b.json` is supposed to be generated
  from a licensed PE (`PLAN_REFACTOR_1.27B.md:190-193`).
* Step 2b explicitly says: if no licensed PE, "fixture не фабрикуется ...
  smoke.py фиксирует `unavailable`; CI-гейт с fixture включается после
  появления ассетов" (`:191-193`).
* Step 3c nevertheless requires `python tools/hle_coverage.py --corpus 1.27b
  --fixture` (`:298-300`) with no `unavailable` path.
* `tools/import_coverage.py` requires `--module <PE>` and reads only
  `runtime/host/hle.c`; it has no fixture mode and no Android mode
  (`:39-45`).
* No step says who creates `config/hle_arity.1.27b.json`; 3c only says
  `--update-spec` is manual (`:292`).

**Why it fails:** Step 3 cannot reach a presence/arity verdict. The only
existing arity oracle is `tools/android/audit_import_arity.py`, which compares
the two old tables, not the game PE. The plan removed the false "PE gives
argc" claim but replaced it with an unavailable fixture and an unspecified
spec.

**Correction:** create `config/hle_arity.1.27b.json` in Step 3a from the
Windows table plus manual review/MSDN entries, with a review log. Create an
interim presence fixture from the current Windows `reg(...)` table (label it
"table-derived, not PE-derived") so `hle_coverage.py --fixture` can run
offline. Make 3c's fixture gate `unavailable`-aware; add a later PE-derived
fixture job only when a licensed PE exists. Alternatively, make
`audit_import_arity.py` the explicit Step 3 gate and defer `hle_coverage.py`
to Step 8.

## 2. P1 findings

### P1-1. `smoke.py verify` can silently pass a broken baseline

**Where:** `PLAN_REFACTOR_1.27B.md:187-201`;
`docs/ARCHITECTURE_1.27B.md:966-969`, `:990-992`.

**Evidence:** the plan says `verify` fails on hash drift, prints
`SKIP(unavailable)`, and exits 0 when the host is `empty` and `adb` is
`unavailable` (`:199`). There is no rule for a status regression from `ok` to
`empty`/`missing`/`unavailable`. A host that used to work and now does not can
still have a stable hash (e.g., a deterministic 0-byte file), so `verify`
exits 0.

**Correction:** `record` stores the status; `verify` must fail when a
previously `ok` artifact becomes `empty`/`missing`/`unavailable`. Add
`--allow-unavailable` for the initial recording only, or an explicit
allowlist per artifact. Keep `--require-exec` for the release gate. Add a
self-test with a fake artifact that transitions `ok -> empty` and `ok ->
missing`.

### P1-2. Step 2c manifests are not a complete source of truth

**Where:** `PLAN_REFACTOR_1.27B.md:204-240`;
`docs/ARCHITECTURE_1.27B.md:361-419`;
`tools/android/build_boot_probe.ps1:218-220`;
`tools/build_host27.ps1:167`.

**Evidence:**

* The plan requires "динамические арены, стеки, guard pages" in
  `config/layout.1.27b.json` (`:208`), but the architecture's example JSON
  (`docs/ARCHITECTURE_1.27B.md:380-405`) contains only images and
  stack/heap/hle_heap/teb/thread_stack. It has no VA arenas, no guard pages,
  no HLE thunk range. Yet `check_layout.py` is supposed to prove "0 overlaps,
  0 omissions" (`PLAN_REFACTOR_1.27B.md:227`). It cannot prove anything about
  regions that are not in the schema.
* The as-built layout has Windows arenas at `0x10000000`/`0x40000000` and
  Android arenas at `0x61000000`, `0x40000000`, `0x80000000`, `0x70000000`,
  `0xA1000000`, `0xD9800000`, `0xEBAD7000` (`docs/ARCHITECTURE_1.27B.md:112-129`);
  none are represented in the manifest example.
* Step 2c removes the `35xxxxxx` filter only from `tools/build_host27.ps1:167`.
  `tools/android/build_boot_probe.ps1:218-220` still manually selects
  `storm_27_extra2_a35.c` / `storm_27_extra_a35.c`. The plan's acceptance
  checks only `build_host27.ps1` (`:236-237`), so the Android manual branch
  survives.
* `check_corpus.py` is specified as "покрытие адресов" (`:219`) with no
  reference address set. The plan itself says the Mss a35 set has 968 extra
  addresses (`:75-79`), so a regenerated Android set could shrink without the
  checker noticing.

**Correction:** extend the layout schema with `arenas[]`, `guard_pages[]`,
`hle_thunks`, and `thread_stacks`, each with base/size/owner/target; require
explicit entries or `inherit_from` and reject unknown/missing keys. Print the
effective merged layout in `check_layout.py`. Make
`build_boot_probe.ps1`/`build_gameactivity_game.ps1` source selection
manifest-driven and add a check that no `_a35` filename appears in build
scripts. Add golden address sets for Storm/Mss/Mssfast/Mp3dec and require
generated Android ⊇ the current a35 set; define coverage as "every `L_<va>`
in the manifest has a dispatcher row", not "no gaps in the VA space".

### P1-3. `run_tests.py` and `hle_abi_test.c` are used before they exist

**Where:** `PLAN_REFACTOR_1.27B.md:258`, `:278`, `:294`, `:301`, `:323`,
`:375-376`, `:452`, `:478`; `PLAN_REFACTOR_1.27B.md:292`.

**Evidence:** `tools/run_tests.py` does not exist. Step 3a's acceptance runs
`python tools/run_tests.py --test hle_abi` (`:258`), but the plan creates
`tests/hle_abi_test.c` only in Step 3c (`:292-294`). Step 8 mentions
`run_tests.py` as part of the toolchain (`:439`), but that is after Steps 3-7.

**Correction:** create `tools/run_tests.py` in Step 2b/2c with a `--list`
command, and create `tests/hle_abi_test.c` in Step 3a, not 3c. Make Step 3a's
acceptance run the test it creates. Alternatively, make one canonical runner
(`wc3build.py test`) and use it everywhere; see P2-14.

### P1-4. The Step 3a adapter comparison contradicts the 16 allowed cdecl differences

**Where:** `PLAN_REFACTOR_1.27B.md:251`, `:262-263`;
`tools/android/audit_import_arity.py:14-15`, `:26-29`.

**Evidence:** Step 3a says "bind оба реестра и сравнивать `(dll, name, argc,
abi)`" (`:251`). The acceptance allows "16 cdecl arity differences" (`:263`).
A literal comparison of `argc` for every entry will report those 16 as
mismatches and fail before the allowlist is applied. The audit script itself
separates `stdcall` (frame drift, must fix) from `cdecl` (inert for `esp`, but
the handler can read the wrong number of args).

**Correction:** compare `argc` strictly for `HLE_ABI_STDCALL`; for cdecl
compare `(dll,name,abi)` and apply a checked-in
`config/cdecl_allowlist.1.27b.json` with one reason per entry. Log the cdecl
arity differences at startup. Add two adapter tests: a cdecl difference is
allowed, a stdcall difference is fatal.

### P1-5. `HLE_OPTIONAL` has no defined call-time behavior

**Where:** `PLAN_REFACTOR_1.27B.md:273-274`;
`docs/ARCHITECTURE_1.27B.md:531-532`, `:1046-1049`;
`tools/android/audit_import_arity.py` MISSING list.

**Evidence:** the plan says the 104 imports missing on Android must be
implemented or marked `HLE_OPTIONAL` with exact `argc` and reason. The list
includes `msvcr80!getenv`, `msvcr80!_malloc`, `winmm!waveOutOpen/waveOutWrite`,
`imm32!*`, `crypt32!*`, `kernel32!ExitThread`, and `storm.dll!#463`. These are
not obviously unreachable. ADR-008 forbids "stubs with argc 0"; an optional
entry that returns 0 is the same silent stub with a better label. The plan
never says what happens when the guest calls an optional entry.

**Correction:** define `HLE_OPTIONAL` semantics explicitly: either bind a
thunk that logs a marker and aborts the guest, or return a documented value
per entry, and require a test for each. Do not mark an entry optional without
evidence it is unreachable (a fixture or a guest trace). Add a
`hle_optional_test`.

### P1-6. `GuestRunner` depends on `Platform`, which Step 5 introduces

**Where:** `PLAN_REFACTOR_1.27B.md:310-319` vs `:333-343`;
`docs/ARCHITECTURE_1.27B.md:757`.

**Evidence:** `gr_create(const GuestConfig *cfg, const Platform *plat)` takes
a `Platform`. Step 5 creates `runtime/platform/platform.h` and the
`MemApi`/`FileApi`/... interfaces. `runtime/platform` and `runtime/app` do not
exist yet. Step 4 is therefore not compilable as specified unless it invents
a temporary `Platform`.

**Correction:** define the platform interface header in Step 4 (or a new
Step 3d) and implement it in Step 5; or narrow `gr_create` to a
`GuestRunnerHost` with only the event queue, surface, clock, and stop flag,
and add the full `Platform` later. Add a compile-only test for the header.

### P1-7. Step 5's `getenv` ban cannot pass with the current code

**Where:** `PLAN_REFACTOR_1.27B.md:341-343`, `:466-467`;
`docs/ARCHITECTURE_1.27B.md:286-287`.

**Evidence:** the architecture forbids `getenv` in `runtime/hle`,
`runtime/gfx`, and `runtime/app` except `runtime/diag`. The repository has
155 unique `getenv("WC3_*")` calls (verified; the plan's count is correct),
many in `runtime/host/hle_d3d9_dxvk.cpp`, `runtime/host/hle_d3d9_vk.cpp`, and
`runtime/android/hle_android.c`. Step 5 adds the ban to `check_arch.py`; Step
9 is where old env variables are removed.

**Correction:** move the `DiagConfig`/`runtime/diag/env_map.c` migration into
Step 5 (or Step 2c) and make `check_arch.py --env` compare against a baseline
count. If that is too large, allowlist the existing env call sites until Step
9 and make Step 9 remove the allowlist. Do not put the ban in Step 5 without
the migration.

### P1-8. Step 5's `< 2 000` line target is unachievable before Step 6

**Where:** `PLAN_REFACTOR_1.27B.md:352`, `:338`, `:360-361`, `:381`.

**Evidence:** `runtime/android/hle_android.c` is 10,212 lines. Its macro
counts are: K 171 + CR 133 = 304 kernel32/msvcr80 entries; U 82 + G 7 + A 6 +
S 2 + I 4 + W 12 + OUT 11 = 124 non-kernel entries, plus AAudio/input/JNI
code. Step 5 moves only kernel32/msvcr80. Step 6 moves user32/imm32/shell32/
comctl32/winmm/dsound and only then requires `< 500` lines. Extracting 304
entries cannot reduce a 10k-line file below 2k while 124 UI/audio entries and
their Android glue remain.

**Correction:** set Step 5's target to "kernel32/msvcr80 removed; the
remaining file contains only UI/audio/platform glue and no kernel32/msvcr80
symbols". Set the line threshold in Step 6 after UI/audio extraction, or
delete `hle_android.c` only when all modules exist. Measure total lines with a
defined command.

### P1-9. Step 6 device tests are not an executable acceptance

**Where:** `PLAN_REFACTOR_1.27B.md:372-381`, `:441`.

**Evidence:** the only commands in Step 6 acceptance are
`check_arch.py --strict`, `run_tests.py --test input_event`, and
`run_tests.py --test audio_ring`. The real device tests (home/resume, screen
off/on, rotation, surface loss, audio device change, save while paused) are
prose. `adb` is absent. Step 8 mentions a self-hosted device runner, but that
is after Step 6 and no runner is configured.

**Correction:** add `tools/device_test.py` with explicit artifact statuses and
a `--require-device` gate, or move the device tests to Step 8/9. Define the
runner image, `adb` install, and device provisioning. A manual checklist can
be a fallback, but not an acceptance claim.

### P1-10. Step 6 uses platform interfaces that Step 5 does not define

**Where:** `PLAN_REFACTOR_1.27B.md:339`, `:361`;
`docs/ARCHITECTURE_1.27B.md:607-612`.

**Evidence:** Step 5 lists `MemApi`, `FileApi`, `ThreadApi`, `SyncApi`,
`TimeApi`, `LocaleApi`, `LogApi`, `ConfigApi`. Architecture §5.4 also defines
`WindowApi`, `InputApi`, and `AudioApi`, and `ClipboardApi` appears only in
Step 6. Step 6's HLE modules need all of them.

**Correction:** add `WindowApi`, `InputApi`, `AudioApi`, and `ClipboardApi`
to the Step 4/5 platform header with fake implementations; add `ClipboardApi`
to architecture §5.4 or drop it. Add a compile test that every interface has
a fake and a real implementation.

### P1-11. Step 7a's A/B gate has no scenario, threshold, or tool, and mislabels the legacy path

**Where:** `PLAN_REFACTOR_1.27B.md:388-401`;
`tools/android/build_boot_probe.ps1:42`, `:461-464`, `:493-494`.

**Evidence:** the plan says "собрать Android с текущим thin bridge и с
`-LegacyD3D9` (`hle_d3d9.cpp` + `D9VkApi`); прогнать одинаковую сцену,
сравнить frame hash, стабильность, performance и caps; записать ADR." It
does not define the scene, frame count, hash algorithm, warm-up, performance
metric, or pass/fail threshold. `-LegacyD3D9` actually selects
`hle_d3d9.cpp` + `hle_d3d9_dxvk.cpp` (`build_boot_probe.ps1:493-494`), not
`D9VkApi`; `D9VkApi` and `d9vk_api.h` do not exist in the tree.

**Correction:** add a concrete `tools/d3d9_ab.py` (or
`run_tests.py --test d3d9_ab`) with a fixed replay scene, warm-up frames,
frame hash, p50/p95 frame time, caps dump, and explicit thresholds. Word 7a
as "thin vs current legacy DXVK"; introduce `D9VkApi` only in 7b if legacy
wins. Make the ADR a checked artifact with numbers, not prose.

### P1-12. Step 8's CI and test/build tools are unspecified

**Where:** `PLAN_REFACTOR_1.27B.md:438-441`, `:452-457`;
`PLAN_REFACTOR_1.27B.md:439`.

**Evidence:** there is no `.github` directory at the repository root (only
inside `third_party/*`), no root CI workflow, no self-hosted runner config,
no `tools/check_manifest.py`, and no `tools/run_tests.py`. `adb` is absent.
The acceptance says "asset-free CI job зелёный" but gives no local command or
workflow path. `check_manifest.py` is mentioned only in prose and never
invoked by an acceptance command.

**Correction:** commit a concrete CI workflow (or `tools/ci_asset_free.ps1`),
add `check_manifest.py` to the Step 8 action and acceptance, and make
`wc3build.py ci --asset-free` the local command. Define the self-hosted
runner image and device provisioning. If CI is out of scope, remove the
green-CI acceptance.

### P1-13. The Step 8 size budget uses the wrong artifact and is not supported by measurements

**Where:** `PLAN_REFACTOR_1.27B.md:67`, `:442-444`;
`docs/ARCHITECTURE_1.27B.md:873-875`, `:1086-1089`.

**Evidence:**

* The plan's "release thin `.so`" is `build/android/boot_probe/libwc3game-thin.so`,
  216,160,728 bytes. The final GameActivity release product is
  `build/android-shared/gameactivity/game-release-ciatan2/libwc3gameactivity.so`,
  221,713,808 bytes (and `build/android-gl-test-release/game/libwc3gameactivity.so`,
  221,761,328 bytes). The budget should apply to the final product, not the
  boot probe.
* ELF sections of the thin `.so`: `.text` 160,098,476 bytes, `.symtab`
  12,088,536, `.strtab` 9,371,113, `.debug_*` about 3.8 MB, total section
  bytes 222,339,841. Strip + `--gc-sections` + `--icf=all` can recover roughly
  25 MB of symbols/debug, not 96 MB.
* The non-DXVK `build/android/boot_probe/libwc3game.so` is 55,340,608 bytes
  with `.text` 30,987,988. DXVK's static archives add roughly 129 MB of
  `.text`. The 120 MiB target therefore requires dropping or shrinking DXVK,
  not just linker flags.
* Current APKs: release 129,135,041 bytes, debug 170,532,670 bytes. The
  70 MiB target is about 54% of the current release APK. No APK baseline or
  reduction plan is in the plan.

**Correction:** record the final `libwc3gameactivity.so` as the baseline; add
`wc3build.py size --breakdown` (text/rodata/symtab/debug/DXVK archives); set
per-frontend budgets (thin+DXVK vs native_vk/null); include a concrete
DXVK-archive/corpus reduction plan or raise the budget; track the APK's
current size. Do not make 120/70 a release gate until a prototype build proves
it.

### P1-14. Step 9's E2E baseline is stored in a directory Step 9 deletes, and there is no executable baseline to compare

**Where:** `docs/ARCHITECTURE_1.27B.md:965`;
`PLAN_REFACTOR_1.27B.md:466-467`, `:478-480`, `:195-202`.

**Evidence:** architecture §10.3 stores the baseline at
`build/smoke_baseline.json` "вне git". Step 9 deletes "неиспользуемые
build-каталоги". Step 2b records the host as `empty` and `adb` as
`unavailable`; no step re-records a baseline after the host/device exist.
Step 8 builds the host but does not refresh the baseline. Step 9 then asks to
compare markers/hashes with a baseline that is either empty or deleted.

**Correction:** store baseline metadata in `config/baseline.1.27b.json` or
`reports/baseline/` (hashes/markers only, no assets); keep the Step 2b
"before" file separately; add a `smoke.py record --require-exec` checkpoint
after Step 8 and before Step 9; exclude the baseline from build cleanup.

### P1-15. Policy enforcement is incomplete in the checker and contradicted by the code

**Where:** `tools/check_docs.py:122-138`;
`AGENTS.md:11-15`;
`PLAN_REFACTOR_1.27B.md:144-150`;
`runtime/host/hle_d3d9_dxvk.cpp:14`, `:1605`;
`runtime/android/dxvk_entry.cpp:20-24`, `:124-156`;
`docs/ARCHITECTURE_1.27B.md:703`.

**Evidence:**

* `check_policy()` checks that `AGENTS.md`/`README.md` contain
  `1.27.1.7085`, and that `AGENTS.md` does not match
  `DXVK...запрещ|не используется`. It does not check that `README.md` states
  the DXVK policy, does not require the positive "embedded compile-time
  provider" statement, and does not reject "external `d3d9.dll`/PATH".
* `hle_d3d9_dxvk.cpp:14` says `WC3_DXVK_VERSION=1|3 forces a version`;
  `:1605` reads it. Architecture §6.2 line 703 says the release has no
  `WC3_D3D9`/`WC3_DXVK_VERSION` switches.
* `dxvk_entry.cpp:20-24` documents `WC3_VULKAN_DRIVER` /
  `WC3_VULKAN_DRIVER_DIR` / `WC3_VULKAN_DRIVER_NAME`; `:124-156` loads a
  custom external Vulkan ICD via `adrenotools_open_libvulkan`. That is a
  runtime external `.so` selected by env, not a compile-time embedded
  provider.
* Step 7b/9 do not explicitly remove either path.

**Correction:** make `check_policy()` require, in both `AGENTS.md` and
`README.md`, a positive "embedded/compile-time" statement and a negative
"external `d3d9.dll`/`d3d8.dll`/PATH" statement. Add a release code gate
(`check_arch.py --policy` or `check_release.py`) that fails on
`WC3_DXVK_VERSION`, `WC3_D3D9`, `WC3_VULKAN_DRIVER*`, `adrenotools_*`, and
external D3D9/D3D8 loads. Either remove/gate the custom Vulkan ICD path or
write an ADR that explicitly allows it and update AGENTS/architecture. Make
Step 7's `--renderer` include this gate.

## 3. P2 findings

| ID | Where | Evidence | Correction |
|---|---|---|---|
| P2-1 | `tools/check_docs.py:240-245` | Default, `--strict`, and `--strict-final` differ only in the `WARN`/`STRICT` label; default and `--strict` both exit 0 with the same three known-stale docs. `--strict` is a no-op gate. | Make default warn-only and `--strict` fail on any known stale live doc, or drop `--strict` and use default + `--strict-final`. |
| P2-2 | `tools/check_docs.py:202-206` | An archived live doc is accepted only if the first 20 lines contain `archived`/`history/1.26` and `architecture_1.27b.md`. Russian "АРХИВ" does not match `archived`; a live doc with the right banner is accepted without being moved. | Accept a documented Russian/English banner, or require the live file to be absent and present under `docs/HISTORY/1.26/`. Add a test. |
| P2-3 | `PLAN_REFACTOR_1.27B.md:56-57`; `docs/ARCHITECTURE_1.27B.md:237` | `hle_android.c` is 10,212 lines (401,757 bytes exact); the plan says 9,564. `hle.c` is 5,875 lines; the plan says 5,727. Architecture says ~15,300 total; actual is 16,087. The byte sizes are exact, so the line metric is undefined. | Record total lines with `len(splitlines())` and use the same metric in acceptance; update the numbers. |
| P2-4 | `config/game127.json:38,53,67`; `docs/ARCHITECTURE_1.27B.md:429-448`; `PLAN_REFACTOR_1.27B.md:211` | `game127.json` and the architecture corpus example use `md5`, while the plan and architecture prose say SHA-256. | Add SHA-256 fields (keep MD5 for PE identity if needed) and state which hash is authoritative. |
| P2-5 | `PLAN_REFACTOR_1.27B.md:444`; `docs/ARCHITECTURE_1.27B.md:874` | `--icf=all` can merge identical lifted functions and change guest-observable function addresses (callbacks, vtables, `GetProcAddress`). No pointer-identity test exists. | Use `--icf=safe` or exclude the lifted corpus; add a test that distinct `L_<va>` functions stay distinct where the guest can observe them; keep the link map. |
| P2-6 | `tools/build_host27.ps1:358-360`, `:394` | The Windows host build compiles/links `hle_d3d8.o` and `hle_d3d8_vk.o`. Step 7 says D3D8 is not linked in 1.27, but does not say to edit this script; Step 9 deletes dead backends later. | Remove D3D8 from the 1.27 build graph in Step 7 and check the link map/symbol list, not just file presence. |
| P2-7 | `README.md:10,22,28,51`; `docs/ARCHITECTURE_1.27B.md:3,9-12` | README still says "Phase 0", lists `docs/DESIGN.md` as architecture, and lists `config/game.json` instead of `config/game127.json`. Architecture status says "proposed" and "AGENTS/README правятся в Step 2a" after Step 2a is DONE. | Update the README docs table/quick start or mark the 1.26 sections historical; change the architecture status to active and the Step 2a sentence to past tense. |
| P2-8 | `docs/ARCHITECTURE_1.27B.md:477-481`; `runtime/host/host.h:189`; `runtime/host/lift_smoke.c:41`; `runtime/android/hle_android.c:9946` | Architecture says `g_image_delta` is checked and the runtime fails if it is non-zero. The symbol is only declared in `host.h`, defined in `lift_smoke.c`, and printed in an MSS debug line; `main.c` has no check. | Implement the check in `main.c`/`guest_mem.c` and add a test, or remove the claim; add it to Step 2c acceptance. |
| P2-9 | `tools/import_coverage.py:39-45`; `PLAN_REFACTOR_1.27B.md:290` | `import_coverage.py` requires a PE (`--module`) and reads only `runtime/host/hle.c`; it has no fixture or Android mode. Step 3c says "presence по fixture". | Extend it with `--fixture` and `--registry android|windows`, or replace it with `hle_coverage.py`. |
| P2-10 | `tools/check_docs.py:92-94`, `:136-137` | `read_text()` decodes UTF-8 with `errors="replace"`, but the policy regexes are Cyrillic. The comment says the patterns are ASCII, which is false for `DXVK...запрещ`. Current docs are UTF-8, but a cp1251 AGENTS.md would silently skip the check. | Decode UTF-8 then cp1251 fallback, or use English policy tokens; add a cp1251 fixture test. |
| P2-11 | `tools/check_layout.py` (planned); `runtime/host/hle_d3d9_dxvk.cpp:120-121`; `runtime/host/hle_d3d9_vk.cpp:81-82` | The plan asks for "0 hardcoded bases", but the DXVK/VK files legitimately contain `0x15000000`/`0x21100000`/`0x21200000` ranges. A naive hex grep will flag them. | Define "base" as an image/dynamic base from the manifest; compare against generated constants; whitelist/parse range constants; add `--list`. |
| P2-12 | `tools/check_docs.py:67-88`; `PLAN_REFACTOR_1.27B.md:474` | `STALE_DOCS` covers only README, AGENTS, DESIGN, MODULES, MEMORY_MAP. `docs/ANDROID.md`, `docs/PLAN.md`, `docs/EXACTNESS.md`, `docs/ANDROID_WSI.md`, `docs/WAR127.md` are not checked. Step 9 claims "0 live stale docs". | Enumerate all live docs and scan for version/D3D8/DXVK patterns with an archive allowlist, or narrow the Step 9 claim to the five tracked docs. |
| P2-13 | `docs/ARCHITECTURE_1.27B.md:307`, `:689`; `PLAN_REFACTOR_1.27B.md:409-411` | "76 `d9vk_*` + lifecycle" is a call-site count, not a clean ABI: it includes diagnostics (`d9vk_note_caller`, `d9vk_stats`, `d9vk_dump_steps`) and lifecycle (`d9vk_ready`, `d9vk_device_destroy`, `d9vk_reset`, `d9vk_present`). `D9VkApi`/`d9vk_api.h` do not exist. If selection is compile-time, a 76-pointer runtime struct is unnecessary indirection. | Enumerate the exact ABI in `runtime/gfx/d9vk_api.h`, separate diagnostics from the API, state whether lifecycle is included, and use a header contract for compile-time linking or a struct only for the A/B build. |
| P2-14 | `docs/ARCHITECTURE_1.27B.md:856`; `PLAN_REFACTOR_1.27B.md:258` | Two test entry points: `wc3build.py test --unit|--smoke|--e2e` and `tools/run_tests.py`. Neither is defined as canonical. | Pick one; make `wc3build.py test` delegate to `run_tests.py`, or remove one. |
| P2-15 | `docs/ARCHITECTURE_1.27B.md:642-645` | "CreateThread ... запускает GuestRunner." `GuestRunner` is the app lifecycle state machine, not a per-thread trampoline. | Change to `GuestThread` trampoline; keep `GuestRunner` as the app-level owner. |
| P2-16 | `PLAN_REFACTOR_1.27B.md:361-362`; `docs/ARCHITECTURE_1.27B.md:607-612`, `:817-833` | Step 6 says waveOut/DirectSound sit over `AudioApi`; architecture §7.5 defines `AudioSink` for the same layer. Two interfaces with no mapping. | Define `AudioSink` as the backend and `AudioApi` as the HLE-facing wrapper (or rename), and document the layering. |
| P2-17 | `PLAN_REFACTOR_1.27B.md:369-370`; `docs/ARCHITECTURE_1.27B.md:569` | Step 6 keeps a LAN fork (NetApi + ws2_32 + INTERNET + test) while the target is offline single-player. The architecture lists `wsock32/ws2_32` as a module. | Decide now: declare LAN/Battle.net out of scope for 1.27 Android and remove `ws2_32` from the target module list, or commit to the full NetApi + permission + device test. |
| P2-18 | `runtime/host/guest_layout.h:31-122`; `PLAN_REFACTOR_1.27B.md:212-213` | A hand-written `guest_layout.h` already exists with both platform layouts and is included by `hle_d3d9_dxvk.cpp:37`; only `hle.c`/`main.c`/`hle_d3d9_vk.cpp` duplicate literals. The plan says "generate `guest_layout.h`" as if it were new. | State that Step 2c replaces the existing hand-written header with a generated one and converts all four files to include it; keep the existing values as the golden layout. |

## 4. Critique 2 fixes: what is still wrong or incomplete

| Critique 2 item | Revised plan | Remaining problem |
|---|---|---|
| Staged Step 3 | 3a adapter, 3b per-DLL, 3c cutover (`:244-306`) | 3a's comparison includes cdecl arity (P1-4); the test runner/test file are created after their use (P1-3); `HLE_OPTIONAL` behavior is undefined (P1-5); the arity spec creator is unnamed (P0-2). |
| Presence/arity split | Fixture vs canonical spec (`:190-193`, `:290-292`) | The fixture is unavailable and 3c is unconditional (P0-2); no step creates `hle_arity.1.27b.json`; `import_coverage.py` has no fixture mode (P2-9). |
| Baseline statuses | `{ok, missing, empty, unavailable}`, `--require-exec` (`:183-201`) | `verify` can still exit 0 on a status regression (P1-1); the baseline is stored in `build/` and deleted in Step 9 (P1-14); no refresh after Step 8. |
| GuestRunner before UI/audio | Step 4 (`:310-331`) | It depends on `Platform` from Step 5 (P1-6); pause/resume has no cooperative safepoint or blocking-call policy; the fake-platform test cannot prove real thread behavior. |
| Step 7 decision gate | A/B thin vs legacy (`:387-428`) | No scene/threshold/tool (P1-11); `-LegacyD3D9` is not `D9VkApi`; `D9VkApi` is undefined (P2-13); no device. |
| AGENTS.md in Step 2a | `AGENTS.md:11-15` updated | `check_policy` checks only AGENTS and only the absence of a prohibition (P1-15); README is not checked; the code still runtime-switches DXVK and loads an external ICD (P1-15). |
| Asset-free CI/fixture/pinned toolchain/size | Step 8 (`:430-459`) | No CI config/runner (P1-12); no `check_manifest.py`/`run_tests.py`; the size budget uses the boot probe, not the final `.so`, and is not supported by measurements (P1-13). |
| `check_docs.py` archive/strict/final | `tools/check_docs.py:185-246` | `--strict` is identical to default (P2-1); the archive banner is brittle (P2-2); `--strict-final` covers only five docs (P2-12); no Evidence enforcement (P2-6 in the plan, section 5 below). |
| Updated numbers | 365/403, 104, 16, 0 (`:58-62`) | Correct. The line counts (P2-3) and the release `.so` baseline (P1-13) are not. |

## 5. Ordering and dependency errors

1. **Step 2c -> boot smoke:** no host/device until Step 8 (P0-1). The plan's
   own risk section says "один boot smoke до изменений" (`:491-492`), but that
   smoke cannot run.
2. **Step 3a -> `run_tests.py`/`hle_abi_test.c`:** the test file is created in
   3c, the runner in 8 (P1-3).
3. **Step 4 -> Step 5:** GuestRunner needs `Platform` before it exists
   (P1-6).
4. **Step 5 -> Step 9:** the `getenv` ban is enforced before the migration
   (P1-7).
5. **Step 6 -> Step 8:** device tests are promised before the device runner
   exists (P1-9).
6. **Step 7a -> Step 7b:** the A/B gate is declared before `D9VkApi` is
   defined; 7a is actually thin vs legacy DXVK (P1-11).
7. **Step 9 -> Step 2b/8:** E2E compares to a baseline that is empty or
   deleted (P1-14).

## 6. Merge / drop / simplify

* **Drop `--strict`** and keep default + `--strict-final`; the current flag
  is only a label (P2-1).
* **Merge `wc3build.py test` and `run_tests.py`** into one test entry point
  (P2-14). This also fixes the Step 3a ordering if the runner is created in
  Step 2b.
* **Drop the 76-pointer `D9VkApi` struct** if the frontend/provider is chosen
  at compile time; keep a `d9vk_api.h` declaration contract and link one
  backend. Keep a struct only for the Step 7a A/B binary (P2-13).
* **Drop `--icf=all`** or restrict it to non-lifted code; exactness is more
  important than the size saving (P2-5).
* **Drop LAN from 1.27 Android** or commit to the full NetApi + INTERNET +
  device test; the current "either/or" leaves `ws2_32` in the target
  architecture with no owner (P2-17).
* **Merge `check_manifest.py` into `wc3build.py check`** rather than adding a
  fourth CLI; the plan already has `wc3build.py`, `run_tests.py`, `smoke.py`,
  `check_layout.py`, `check_corpus.py`, `check_arch.py`, `hle_coverage.py`,
  and `import_coverage.py` (P1-12, P2-9, P2-14).
* **Merge the two line thresholds** (`hle_android.c < 2000` in Step 5,
  `< 500` in Step 6) into one threshold after Step 6 (P1-8).

## 7. What is already correct (do not re-litigate)

* The baseline counts 365 Windows / 403 Android entries, 0 stdcall drift,
  16 cdecl differences, 104 missing on Android match
  `tools/android/audit_import_arity.py`.
* Step 2a correctly updated `AGENTS.md`/`README.md` to 1.27.1.7085 and the
  embedded-only DXVK policy; `check_docs.py` is green and `--strict-final`
  reports exactly the three historical docs (four findings).
* Step 3 is staged; the old tables remain the oracle; the plan no longer
  claims PE imports supply `argc`.
* Step 7 no longer proposes `GfxProvider -> IDirect3D9`, `win32_d3d9`, or a
  new `gen_d3d9_spec.py`; `tools/gen_d3d9_thin_bridge.py --check` passes
  ("16 interfaces, 308 methods, 216 generated, 92 handwritten").
* `tools/rebase_pe.py` exists; the a35 corpus path is feasible in principle.
* `runtime/host/guest_layout.h` already contains the correct Windows and
  Android bases; Step 2c is a source-of-truth swap, not a new layout design.

## Verdict

Verdict: needs revision

The plan is directionally correct and fixes the biggest conceptual errors
from critique 2 (staged HLE migration, presence/arity split, A/B renderer
gate, policy truth). It is not yet executable: the execution baseline is
absent, the Step 3 verification chain is missing, the Step 2c manifests are
incomplete, and the Step 8 size gate is not supported by the measured
artifacts. The three most important changes:

1. **Add an execution-baseline gate after Step 2b:** build a non-empty
   `build27/wc3_host27.exe`, provision `adb`/device or record device checks
   as `unavailable`, make `smoke.py verify` fail on `ok -> empty/missing`
   regressions, and refresh the baseline after Step 8. Without this, every
   "boot smoke unchanged" acceptance in Steps 2c-7 and the Step 9 E2E are
   unverifiable.
2. **Repair the Step 3 verification chain:** create `tools/run_tests.py` and
   `tests/hle_abi_test.c` before Step 3a; define
   `config/hle_arity.1.27b.json` and the 16-entry cdecl allowlist in 3a;
   generate an interim presence fixture from the old Windows table (labeled
   table-derived) or make 3c `unavailable`-aware. Compare stdcall arity
   strictly and cdecl separately.
3. **Rebase Step 8 on measured artifacts and a real CI:** use the final
   `libwc3gameactivity.so` (221.7 MB) and the current APK (129 MB) as
   baselines, add a `size --breakdown` and a per-frontend budget (thin+DXVK
   vs native_vk/null), commit a concrete CI workflow and
   `check_manifest.py`, and add a release policy gate for
   `WC3_DXVK_VERSION`/`WC3_VULKAN_DRIVER` so the embedded-only DXVK policy is
   enforced in code, not just in AGENTS.md.
