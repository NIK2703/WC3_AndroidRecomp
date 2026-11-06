# VENDOR_MANIFEST — wc3_android_recomp

Статус: **rev 1** (2026-11-06). Инвентарь составлен по **фактически
прочитанным** license-файлам; ничего не угадано. Правило R9 из
`docs/FILE_LAYOUT.md`: vendor-код живёт только здесь, каждый компонент имеет
запись в этом файле и свой LICENSE/COPYING рядом с исходниками.

## Компоненты

| Компонент | В новом дереве | Источник | Лицензия | Файл лицензии | Примечание |
|---|---|---|---|---|---|
| Berkeley SoftFloat **Release 3e** | **да**, `third_party/softfloat/` | John R. Hauser, 2018-01-20; «Copyright 2011–2018 The Regents of the University of California» | UC Berkeley / BSD-подобная, применяется ко всему релизу и к каждому файлу | `third_party/softfloat/COPYING.txt` | state должен быть thread-local: `-DTHREAD_LOCAL=_Thread_local`; сборочные продукты (`build-host/`) gitignored |
| DXVK **3.1.1** | **нет** (submodule `third_party/dxvk-src` старого дерева) | `github.com/doitsujin/dxvk` | **zlib/libpng** («Copyright (c) 2017 Philip Rebohle, 2019 Joshua Ashton, 2019 Robin Kertels, 2023 Jeffrey Ellison») | `LICENSE` в `dxvk-src` | при вендоринге скопировать LICENSE; требуется Android-WSI патч и symbol isolation |
| DXVK **1.10.3** | **нет** (submodule `third_party/dxvk-1.10.3` старого дерева) | тот же репозиторий, тег 1.10.3 | **zlib/libpng** | `LICENSE` в `dxvk-1.10.3` | второй статический бэкенд одного APK; свой namespace |
| `winlator-input` (touch engine) | **нет** (`runtime/android/controls/vendor/winlator-input/` старого дерева) | `E:\projects\winlator-enhanced-inpu-controls-2`, commit `208371f89e2e2d218263308f26ee02bbf149a70e` | **MIT**, Copyright (c) 2023 BrunoSX | `LICENSE` в vendor-каталоге | профиль `Warcraft 3.icp` в upstream untracked; копировать по SHA-256 `349B6113…E04`, а не «похожий» профиль |
| `dxvk_win32` shim | **нет** (`runtime/android/dxvk_win32.cpp` старого дерева) | собственный код проекта | проектное право | — | при переносе → `third_party/dxvk/shim/`; это vendor-boundary, не guest runtime |
| adrenotools / Turnip ICD | **нет** (hook `.so` в `build/android-shared/adrenotools/` старого дерева) | Winlator / libadrenotools | **НЕ ПОДТВЕРЖДЕНО** | — | перед включением в APK обязательно установить лицензию и добавить attribution; до этого компонент не вендорится |
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

## Что осталось открытым

* Перенос DXVK 1.10.3/3.1.1, `winlator-input` и `dxvk_win32` в это дерево —
  шаг M6 (`docs/FILE_LAYOUT.md`), по одному компоненту за коммит.
* Лицензия adrenotools не установлена: `VulkanDriverApi` использует его в
  as-built билде старого дерева, но до проверки лицензии компонент не
  попадает в новый APK.
* `VENDOR_PATCHES.md` для DXVK ещё не создан.
