# wc3_android_recomp

Статическая рекомпиляция Warcraft III: The Frozen Throne (1.27.1.7085) в
нативный порт для Android arm64-v8a. x86-32 код переводится в C на этапе
сборки, затем NDK/Clang компилирует его в ARM64.

> Архитектура описана в `docs/ARCHITECTURE_1.27B.md` (rev 2.2 / M2): текущее
> состояние и целевая модель. План миграции лежит в `PLAN_REFACTOR_1.27B.md`.
>
> Статус: rev 2.2 / M2, 2026-11-07. В `E:\projects\warcraft3_recomp` уже
> работает Android-порт: загрузка, меню и 3D-сцена на устройстве, 191 650
> адресов в dispatcher, 498/498 и 828/828 импортов, `libwc3game-thin.so`
> размером 216 МБ. Ранее найденный графический дефект исправлен в старом
> дереве. Сохранение исправления проверяют гейты V5.9 и V6.6. Новое дерево
> объявлено каноническим target. Миграция идёт постепенным strangler-cutover,
> и каждый шаг проходит два исполняемых гейта.

## Целевая конфигурация

* Warcraft III: The Frozen Throne 1.27.1.7085, PE32/x86, MSVC 12.0.
* Android 8+ (API 26), arm64-v8a, Vulkan.
* Продуктом является `libwc3gameactivity.so`. Рекомпилированный код
  собирается только под Android (ADR-026/027). Верификационный
  `--target headless` (`wc3_headless`, CI-хост) даёт исполняемый DUT для T1
  (`docs/VERIFICATION.md`, V4.7). Эталон: оригинальная лицензионная игра
  1.27.1.7085 на Windows и её зафиксированные захваты. Difftest использует их
  для быстрого цикла без adb.
* Guest: `war3.exe` `0x00400000`, `game.dll` `0x6F000000`, `Storm.dll`
  `0x35000000`, `Mss32.dll`/`Mssfast`/`Mp3dec` `0x351..0x35300000`,
  `ijl15.dll` `0x60000000`. Это фиксированный Android-layout, `corpus_fixed`.
  Guest VA отображается на host pointer в младших 4 ГБ (identity mapping).
* Рендер идёт через D3D9 и thin bridge в статически собранные DXVK 1.10.3 и
  3.1.1. Оба лежат в одном APK, символы изолированы через
  `objcopy --prefix-symbols`. Селектор выбирает бэкенд по возможностям
  physical device и один раз откатывается на другой при ошибке создания
  device. Только один бэкенд создаёт Vulkan instance и device.
* `VulkanDriverApi` (`system` | `turnip`) задаёт Vulkan ICD. adrenotools и
  Turnip разрешены и документированы как способ выбора ICD.

## Runtime

* Guest threads сохраняются. `CreateThread` и `_beginthreadex` создают реальный
  host thread. Каждый поток получает `CPU`, guest stack, TEB и TLS
  (`GUEST_THREAD_MAX = 24`). В игре подтверждено 18 вызовов `CreateThread`.
* `ThreadApi`, `SyncApi`, `AssetApi` и `VulkanDriverApi` обязательны. 36 из 426
  HLE-записей относятся к thread/sync.
* Guest-visible events, mutex, semaphore и critical sections реализованы как
  HLE-объекты с точной семантикой Win32.
* Host lock освобождается до `dispatch()` и до обратного вызова в гость.
  Per-object локи допустимы в HLE, lock-free atomics в mailbox/ring и
  собственные локи DXVK/`dxvk_win32` из vendor allowlist.
* Android host threads сохраняются. Cross-thread события идут через lock-free
  mailbox/queue с документированным владельцем.
* `HleEntry` вычисляет `stack_cleanup = 4 + (cdecl ? 0 : argc*4)` и хранит
  поле `user` для COM-слотов. PE игры присутствует (`E:\Games\Warcraft III`),
  поэтому presence проверяется по PE, а arity проверяется по spec и анализу
  call-site.

## Данные и верификация

* `AssetApi` читает лицензионные MPQ (`war3.mpq`, `War3x.mpq`,
  `War3xlocal.mpq`, `War3Patch.mpq`) из app-specific каталога или через SAF.
  MPQ читает собственный лифтнутый код `Storm.dll`. Saves, replays и registry
  HLE хранятся в app-private dir.
* Проверка идёт на трёх уровнях. T0: asset-free static и unit, на каждом
  коммите. T1: difftest headless-DUT `wc3_headless` против эталонных захватов
  оригинальной игры на Windows, на каждом шаге cutover, на CI-хосте. T2:
  device E2E, перед релизом. T1 исполняет тот же корпус и HLE, что и продукт,
  подменяя только `Wc3Platform` и графический frontend
  (`docs/VERIFICATION.md`, V4.7; ADR-027). T2 считается пройденным только со
  статусом `pass`. Зелёные T0 и T1 достаточны для продолжения работы.
* Каждый шаг переноса кода завершают зелёные T0 и T1: static/unit и сравнение
  с эталонными захватами, на CI-хосте. Загрузка Android до меню входит в T2 и
  обязательна на финальном E2E.

## Debug и оптимизация

* Весь отладочный код находится под `#if WC3_DEBUG`. Если `WC3_DEBUG` не
  определён, он равен 0. Вложенные подтеги используют
  `#if WC3_DEBUG && WC3_DEBUG_X`.
* Каждая отладочная, логгирующая или трассирующая инструкция оборачивается в
  `#if WC3_DEBUG && WC3_DEBUG_<ЦЕЛЬ>`. Тег берётся из реестра
  `runtime/diag/wc3_debug.h`: `WC3_DEBUG_HLE`, `WC3_DEBUG_GRAPHICS`,
  `WC3_DEBUG_ASSETS` и другие.
* Release собирается с `-DWC3_DEBUG=0 -DNDEBUG` и нулевыми подтегами. При
  нулевых подтегах компилятор удаляет отладочные ветки. Release hygiene
  применяется к нашему коду; DXVK и third_party идут по allowlist.
* Два варианта описаны в ADR-030 и `docs/BUILD_VARIANTS.md`. Release имеет
  `applicationId` `wc3.recomp`, debug имеет `wc3.recomp.debug`. Оба варианта
  ставятся рядом, данные изолированы по UID. `--variant` задаёт вариант
  атомарно для Gradle и native-сборки. Debug сохраняет `-O2`.
* Java-диагностика находится только в `app/src/debug/`. Продуктовые числовые
  коды отказа остаются в release.
* Горячий путь использует compile-time HLE-таблицы и неизменяемый registry.

## Правовая рамка

Репозиторий содержит исходный код и инструменты. Игровые данные и бинарники
пользователь берёт из своей лицензионной копии игры. Сгенерированный C
является производной от бинарника.
