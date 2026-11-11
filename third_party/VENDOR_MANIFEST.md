# VENDOR_MANIFEST — wc3_android_recomp

Статус: **rev 2** (2026-11-10). Инвентарь составлен по **фактически
прочитанным** license-файлам; ничего не угадано. Правило R9 из
`docs/FILE_LAYOUT.md`: vendor-код живёт только здесь, каждый компонент имеет
запись в этом файле и свой LICENSE/COPYING рядом с исходниками.

## Компоненты

| Компонент | В новом дереве | Источник | Лицензия | Файл лицензии | Примечание |
|---|---|---|---|---|---|
| Berkeley SoftFloat **Release 3e** | **да**, `third_party/softfloat/` | John R. Hauser, 2018-01-20; «Copyright 2011–2018 The Regents of the University of California» | UC Berkeley / BSD-подобная, применяется ко всему релизу и к каждому файлу | `third_party/softfloat/COPYING.txt` | state должен быть thread-local: `-DTHREAD_LOCAL=_Thread_local`; сборочные продукты (`build-host/`) gitignored |
| DXVK **3.1.1** | **да**, `third_party/dxvk/3.1.1/` | `github.com/doitsujin/dxvk`, commit `23b07e9be441f87eb46204d78d9265e565cfbc0f`; submodules pinned в `third_party/dxvk/VENDOR_COMMIT` | **zlib/libpng** («Copyright (c) 2017 Philip Rebohle, 2019 Joshua Ashton, 2019 Robin Kertels, 2023 Jeffrey Ellison») | `third_party/dxvk/3.1.1/LICENSE` | Android/static patches уже применены; WC3 D3D9 patch делает `storageBuffer8BitAccess` и `VK_KHR_maintenance6` необязательными и включает fallback на core-команды; `dxvk-3.1.1-android-debug.patch` фиксирует retry singleton, null-guard и stderr-логи; `dxvk-3.1.1-lowmem-placed.patch` включает placed mapping host-visible памяти ниже 4 ГБ. Сборка — `tools/android/build_dxvk.ps1`, symbol isolation — `tools/lift/csrc_local/dxvk_isolate_archive_elf.c` |
| DXVK **1.10.3** | **да**, `third_party/dxvk/1.10.3/` | тот же репозиторий, commit `e4fd5e9e8d335e8a2c0814829207cbd421f7e40e` (tag `v1.10.3`) | **zlib/libpng** | `third_party/dxvk/1.10.3/LICENSE` | второй статический бэкенд одного APK; свой namespace; сборка из того же vendor-каталога; `dxvk-1.10.3-lowmem-placed.patch` включает тот же placed mapping (Vulkan 1.3.217 headers → `shim/wc3_vulkan_lowmem.h`), kill switch `dxvk.lowMemoryPlaced` |
| `winlator-input` (touch engine) | **нет** (`runtime/android/controls/vendor/winlator-input/` старого дерева) | `E:\projects\winlator-enhanced-inpu-controls-2`, commit `208371f89e2e2d218263308f26ee02bbf149a70e` | **MIT**, Copyright (c) 2023 BrunoSX | `LICENSE` в vendor-каталоге | профиль `Warcraft 3.icp` в upstream untracked; копировать по SHA-256 `349B6113…E04`, а не «похожий» профиль |
| `dxvk_win32` shim | **да**, `third_party/dxvk/shim/` | собственный код проекта; перенесён из `runtime/android/dxvk_win32.cpp` + `runtime/android/dxvk_shim/` | проектное право | — | vendor-boundary, не guest runtime; компилируется в 1.10.3 archive и остаётся unprefixed при symbol isolation |
| `libadrenotools` | **частично**: `third_party/libadrenotools/` (LICENSE/README), архивы `22d71e…`/`91a589…` линкуются из pinned legacy build, hooks `libmain_hook.so`/`libhook_impl.so` упакованы в `jniLibs` | `github.com/bylaws/libadrenotools`, README: Android 9+ / arm64 | **BSD-2-Clause**, Copyright (c) 2021 Billy Laws | `third_party/libadrenotools/LICENSE` | загрузка пользовательского Vulkan HAL без root; `hookLibDir` = extracted hooks, `customDriverDir` только app-private. `VENDOR_COMMIT` всё ещё открыт: до него архивы — переходный pinned build input, не source vendor |
| `liblinkernsbypass` | **частично**: `liblinkernsbypass.a` (`91a589…`) линкуется из pinned legacy build | `github.com/bylaws/liblinkernsbypass`, submodule libadrenotools | **BSD-2-Clause** (тот же текст, что у libadrenotools) | `third_party/libadrenotools/LICENSE` | private linker namespace для Android 9+; вендорится одним пин-коммитом с libadrenotools. Хэши и список файлов — `third_party/libadrenotools/README.md` |
| Capstone **5.x** (headers + `capstone.dll`) | **да**, `third_party/capstone/` | `E:\projects\warcraft3_recomp\tools\wc3skill\src\capstone\` и `bin\capstone.dll`, upstream rev `791bdbaf3135` (пин в `config/build_upstream.json`) | **НЕ ПОДТВЕРЖДЕНО локально** (upstream заявляет BSD-3-Clause) | — | только host-side инструменты, в APK не входит. `*.h` и DLL копируются побайтно, sha256 пинится; перед публикацией дерева обязательно положить LICENSE из upstream. Исключение R3 для `bin/capstone.dll` зафиксировано в `.gitignore` |

## Обязательства

1. **Ни одного vendor-файла без записи здесь и licence-файла рядом.**
   `tools/check_file_layout.py` (R9) проверяет наличие этого манифеста.
2. Статическая линковка DXVK 1.10.3 и 3.1.1 в один
   `libwc3gameactivity.so` требует сохранить уведомления zlib/libpng в
   составе APK (NOTICE/Third-party licenses в UI или в ресурсах).
3. Сборочные продукты vendor-кода (`*.a`, `*.so`, `build-host/`,
   `build/`) не коммитятся — см. `.gitignore` (R3).
4. Изменения vendor-кода фиксируются отдельным коммитом с указанием
   upstream commit/tag и перечнем локальных патчей (`VENDOR_PATCHES.md`).
   Для DXVK это критично: Android-WSI и symbol isolation — локальные патчи,
   их надо поддерживать для **обоих** форков.
5. Профиль `Warcraft 3.icp` не воспроизводится из git commit upstream:
   копировать конкретный файл с SHA-256 из манифеста старого дерева.
6. `tools/lift/csrc/` — не vendor, а побайтно пинящаяся копия собственного C
   инструментария проекта из старого дерева (BP-4.1: переносится проверенный
   конвейер, а не переписывается). Хэши и upstream-ревизия —
   `config/build_upstream.json`, проверка — `tools/lift/check_upstream.py`
   (входит в T0 как `upstream_pin`). Правка файла на месте без обновления
   пина считается нарушением R19.
7. `third_party/capstone/bin/capstone.dll` — единственный бинарник vendor-кода
   в исходном дереве: он грузится инструментами во время выполнения, а
   исходники Capstone в дерево не вендорятся. Продукты сборки инструментов
   лежат только в `build/lift-tools/` (R2/R3).
8. `libadrenotools.a`, `liblinkernsbypass.a` и hooks `libmain_hook.so` /
   `libhook_impl.so` — **переходные** Android product inputs (ADR-036). Их
   sha256 проверяет `cmake/Wc3LegacyBoot.cmake` (архивы) и фиксирует
   `third_party/libadrenotools/README.md` (все четыре файла). Это не source
   vendor: замена на исходники обязана добавить `VENDOR_COMMIT` и LICENSE
   рядом с исходниками. Hooks лежат только в APK `jniLibs`, не в
   сгенерированном C и не в guest ABI.

## Что осталось открытым

* Перенос `winlator-input` в это дерево — шаг M6
  (`docs/FILE_LAYOUT.md`), по одному компоненту за коммит.
* Лицензия adrenotools **установлена** (2026-11-10): `libadrenotools` и
  `liblinkernsbypass` — BSD-2-Clause, Copyright (c) 2021 Billy Laws; текст
  прочитан в upstream `LICENSE` (git blob `a8f664a7…`). Компонент ещё не
  вендорен: нужны `VENDOR_COMMIT`, LICENSE рядом с исходниками и attribution
  в NOTICE APK. Проект использования — `docs/DRIVER_PACKAGES.md` (ADR-036
  `proposed`).
* DXVK 1.10.3/3.1.1 и `dxvk_win32` перенесены 2026-11-10; патчи и ревизии —
  `third_party/dxvk/VENDOR_COMMIT` и `third_party/dxvk/patches/`. Обе ветки
  содержат low-memory placed patch: 3.1.1 — в `dxvk_memory`,
  `dxvk_device_info`, `dxvk_options`, `vulkan_loader.h`; 1.10.3 — в
  `dxvk_memory`, `dxvk_adapter`, `dxvk_device_info`, `dxvk_extensions`,
  `dxvk_options`, `vulkan_loader.h`, плюс `shim/wc3_vulkan_lowmem.h` для
  старых Vulkan-заголовков. Общий shim — `shim/wc3_lowmem.h`. Патч меняет
  source snapshot, поэтому архив 1.10.3 больше не совпадает со старым
  побайтно. Сборка обеих веток с low-memory patch ещё не выполнялась;
  синтаксис проверен NDK clang.
