# wc3_android_recomp — Warcraft III 1.27.1.7085 → Android ARM64

Статическая рекомпиляция Warcraft III: The Frozen Throne в нативный порт
только под Android arm64-v8a. Никакого x86-эмулятора, JIT, Wine, Box64 или
FEX в готовой сборке: x86-32 код переводится в C на этапе сборки, C
компилируется NDK/Clang в ARM64.

> Актуальная архитектура: `docs/ARCHITECTURE_1.27B.md` (**rev 2**,
> as-built → target). План миграции: `PLAN_REFACTOR_1.27B.md`.
>
> **Статус:** rev 2, 2026-11-06. Это **не Phase 0**: в
> `E:\projects\warcraft3_recomp` уже существует рабочий Android-порт
> (boot → меню → 3D-сцена на устройстве, 191 650 адресов в dispatcher,
> 498/498 и 828/828 импортов, `libwc3game-thin.so` 216 МБ). Найденный ранее
> графический дефект исправлен в старом дереве; в целевом дереве он не
> расследуется, а сохранение исправления проверяется гейтами V5.9/V6.6.
> Новое дерево — канонический target; миграция идёт инкрементальным
> strangler-cutover с двумя executable gates на каждом шаге.
> Игровые данные и сгенерированный код в репозиторий не входят.

## Целевая конфигурация

* Warcraft III: The Frozen Throne **1.27.1.7085**, PE32/x86, MSVC 12.0.
* Android 8+ (API 26), arm64-v8a, Vulkan.
* Продукт — `libwc3gameactivity.so`. **Рекомпиляция только под Android**
  (ADR-026/027): под Windows не собирается ни один рекомпилированный бинарь.
  Исключение — верификационный `--target headless` (`wc3_headless`, CI-хост,
  не APK, не продукт), дающий T1 исполняемый DUT (`docs/VERIFICATION.md` V4.7).
  Эталон — оригинальная лицензионная игра 1.27.1.7085 на Windows и её
  зафиксированные захваты (difftest, быстрый цикл без adb).
* Guest: `war3.exe` `0x00400000`, `game.dll` `0x6F000000`, `Storm.dll`
  `0x35000000`, `Mss32.dll`/`Mssfast`/`Mp3dec` `0x351..0x35300000`,
  `ijl15.dll` `0x60000000` — фиксированный Android-layout, `corpus_fixed`.
  Identity mapping: guest VA → host pointer в младших 4 ГБ.
* Рендер: D3D9 → thin bridge → статически собранные DXVK **1.10.3** и
  **3.1.1** в одном APK (symbol isolation через `objcopy --prefix-symbols`).
  Селектор по возможностям physical device с fallback при ошибке создания
  device; только один бэкенд создаёт Vulkan instance/device.
* `VulkanDriverApi` (`system` | `turnip`): adrenotools/Turnip — разрешённый и
  документированный путь выбора Vulkan ICD, не подмена D3D-провайдера.
* Запрещены OpenGL/GLES, `native_vk`, `hle_gl`, `hle_d3d9_vk`, `D9VkApi`,
  legacy `hle_d3d9.cpp`/`hle_d3d9_dxvk.cpp`, самописный Vulkan-транслятор,
  `win32_d3d9`, внешние `d3d9.dll`/`d3d8.dll`/PATH.
* LAN/Battle.net — out of scope.
* 1.26/D3D8 — исторический legacy-путь, не активная цель.

## Runtime

* **Guest threads сохраняются**: `CreateThread`/`_beginthreadex` создают
  реальный host thread; каждый имеет `CPU`, guest stack, TEB, TLS
  (`GUEST_THREAD_MAX = 24`). В игре подтверждено 18 `CreateThread`.
* `ThreadApi`, `SyncApi`, `AssetApi`, `VulkanDriverApi` обязательны;
  `NetApi` отсутствует. 36 из 426 HLE-записей — thread/sync.
* Guest-visible events/mutex/semaphore/critical sections — HLE-объекты с
  точной семантикой Win32; no-op waits и заглушки запрещены.
* **Инвариант блокировок**: ни один host lock не удерживается при
  `dispatch()` или обратном вызове в гость. Локи запрещены в guest-пути,
  разрешены per-object в HLE, lock-free atomics в mailbox/ring и собственные
  локи DXVK/`dxvk_win32` (vendor allowlist).
* Android host threads сохраняются; cross-thread события идут через lock-free
  mailbox/queue с документированным владельцем.
* `HleEntry`: `stack_cleanup = 4 + (cdecl ? 0 : argc*4)`, поле `user` для
  COM-слотов. PE игры присутствует (`E:\Games\Warcraft III`), поэтому
  presence проверяется по PE, а arity — по spec + call-site анализу.

## Данные и верификация

* `AssetApi`: пользователь размещает лицензионные MPQ (`war3.mpq`,
  `War3x.mpq`, `War3xlocal.mpq`, `War3Patch.mpq`) в app-specific каталоге или
  импортирует через SAF; MPQ читаются собственным лифтнутым кодом `Storm.dll`.
  Saves/replays/registry HLE — в app-private dir.
* Гейты: **T0** asset-free static + unit (каждый коммит), **T1** difftest
  **headless-DUT** `wc3_headless` против эталонных захватов оригинальной игры
  на Windows (каждый шаг cutover, без устройства, без пересборки под Windows),
  **T2** device E2E (перед релизом). T1 исполняет тот же корпус и HLE, что и
  продукт, подменяя только `Wc3Platform` и графический frontend
  (`docs/VERIFICATION.md` V4.7, ADR-027); `--target windows` запрещён.
  `unavailable ≠ pass` для T2; при зелёных T0/T1 проект не BLOCKED.
* Каждый шаг переноса кода заканчивается зелёными **T0 + T1** (static/unit и
  сравнение с эталонными захватами), без устройства. Android boot →
  меню входит в T2 и обязателен на финальном E2E.

## Debug и оптимизация

* Весь отладочный код — только под `#if WC3_DEBUG`; `#ifndef WC3_DEBUG -> 0`,
  вложенные подтеги `#if WC3_DEBUG && WC3_DEBUG_X`.
* Release: `-DWC3_DEBUG=0 -DNDEBUG`, нулевые подтеги, debug-код физически
  отсутствует. Release hygiene — на нашем коде, DXVK/third_party по allowlist.
* В горячем пути нет `if (debug)`, лишних аллокаций, копирований, повторных
  lookup и debug env. HLE-таблицы compile-time; registry immutable.

## Документы

| Путь | Назначение |
|---|---|
| `docs/ARCHITECTURE_1.27B.md` | Архитектура rev 2: as-built → target, ADR, риски |
| `docs/THREAD_AUDIT.md` | Аудит thread/sync с as-built evidence и гейтами |
| `docs/DXVK_THREADS.md` | Dual-DXVK, symbol isolation, ICD, потоки vendor-кода |
| `PLAN_REFACTOR_1.27B.md` | План миграции и acceptance checks |
| `AGENTS.md` | Обязательные правила проекта |
| `reports/CRITIQUE_FILTER_THREADING.md` | Отфильтрованная критика (история решения по потокам) |
| `reports/REFACTOR_PLAN_CRITIQUE_2.md`, `..._FOLLOWUP.md` | critique-pass'ы |
| `E:\projects\warcraft3_recomp\PROGRESS.md` | As-built состояние порта (включая историю исправления) |

## Правовая рамка

Проект не содержит и не распространяет `game.dll`, `Storm.dll`, MPQ, текстуры,
звуки или сгенерированный из них C. Пользователь должен владеть лицензионной
копией игры. Сгенерированный код — производная от бинарника; его публикация
недопустима без правовой оценки.
