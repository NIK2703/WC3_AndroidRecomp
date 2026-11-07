# Правила проекта

Цель — точная статическая рекомпиляция Warcraft III **1.27.1.7085**
(x86-32) в нативный C и сборка **только под Android arm64-v8a** как продукта.
Точность важнее скорости и удобства. 1.26/D3D8 — исторический reference.

Архитектура: `docs/ARCHITECTURE_1.27B.md` (**rev 2.2 / M2**, хаб ≤400 строк) —
нормативный хаб; as-built, ADR, перформанс, риски и backlog вынесены в
отдельные документы (`docs/README.md`). Владельцы областей: `docs/VERIFICATION.md` (гейты),
`docs/GUEST_ABI.md` (граница корпуса), `docs/EXACTNESS.md` (определение
точности), `docs/FILE_LAYOUT.md` (структура дерева). Правила ниже согласованы
с rev 2; при расхождении приоритет у владельца области, затем у архитектуры.
«Точность» не декларация: критерий равенства и эталоны — `EXACTNESS.md`.

## Продукт и эталон (oracle)

* Единственный поставляемый продукт — Android-приложение
  `libwc3gameactivity.so`.
* **Единственный поставляемый target — Android arm64-v8a** (ADR-026/027).
  Рекомпилированный код под Windows **не собирается, не линкуется и не
  поставляется**; `--target windows` запрещён. Разрешён единственный
  **верификационный** target `--target headless` (`wc3_headless`, CI-хост, не
  Windows, не APK, не продукт): он даёт T1 исполняемый DUT из того же корпуса
  и HLE (V4.7, ADR-027). Oracle — оригинальная лицензионная игра 1.27.1.7085
  на Windows и её зафиксированные эталонные захваты (frame hash, маркеры,
  save/replay), без какой-либо пересборки под Windows. `wc3_host27*.exe`
  старого дерева — исторический готовый артефакт, не цель сборки и не гейт
  нового дерева; `oracle/` хранит эталоны, а не исходники Windows-хоста.
* `E:\projects\warcraft3_recomp` — исполняемый источник рабочего кода до
  завершения strangler-cutover. Новое дерево — канонический target.

## Рендер

* 1.27b использует D3D9: guest видит `IDirect3D9`/`IDirect3DDevice9`.
  D3D8 относится к legacy 1.26 и в 1.27-сборку не входит.
* Графика — только thin bridge D3D9 → DXVK и статические DXVK
  **1.10.3** и **3.1.1** в одном APK с symbol isolation (`objcopy --prefix-symbols`
  по каждому member архива).
* Селектор — по возможностям **physical device** (`apiVersion` + extensions),
  а не по версии loader'а. При ошибке создания device обязателен однократный
  fallback на другой бэкенд. Только один DXVK создаёт instance/device.
* Dual-бэкенд и fallback — **ADR-004/007 со статусом `provisional`**: решение
  реализовано, но не измерено. Норма, матрица A/B/C/D, контракт
  `dxvk_shim_reset()`, обязательные измерения GB-5 и kill criterion —
  `docs/GRAPHICS_DECISION.md` (ADR-023). Ссылаться на «уже собрано» как на
  обоснование запрещено; до GB-5 fallback считается декларацией.
* `VulkanDriverApi` (`system` | `turnip`) — явный и разрешённый путь выбора
  Vulkan ICD; adrenotools/Turnip **не** являются запрещённой подменой
  провайдера. Запрет относится к внешним `d3d9.dll`/`d3d8.dll`, PATH и
  runtime-подмене D3D-frontend'а.
* Запрещены OpenGL/GLES, `native_vk`, `hle_gl`, `hle_d3d9_vk`, `D9VkApi`,
  legacy `hle_d3d9.cpp`/`hle_d3d9_dxvk.cpp`, самописный Vulkan-транслятор,
  legacy A/B и `win32_d3d9`.
* `runtime/platform/dxvk_win32` — vendor-owned Win32/thread/sync shim с
  поимённым allowlist; это не guest runtime и не Windows-host. Его собственные
  host-локи разрешены.

## Runtime

* **Guest threads сохраняются.** `CreateThread`/`_beginthreadex` создают
  реальный host thread; каждый имеет `CPU`, guest stack, TEB, TLS и dispatcher
  state (`GUEST_THREAD_MAX = 24`). Кооперативное схлопывание в один OS-поток
  запрещено.
* `ThreadApi`, `SyncApi`, `AssetApi`, `VulkanDriverApi` — обязательные интерфейсы
  `runtime/platform`. `NetApi` отсутствует (LAN out of scope).
* Guest-visible events/mutex/semaphore/critical sections — HLE-объекты с
  **точной семантикой Win32**. Заглушки и no-op waits запрещены
  (`hle_android.c:2096-2110` — исторический баг с MPQ-чтениями).
* **Инвариант блокировок**: ни один host lock не удерживается при `dispatch()`,
  обратном вызове в гость или ожидании guest-visible объекта. Локи запрещены в
  `runtime/cpu`, `runtime/guest`, `runtime/hle/abi`; разрешены per-object в
  HLE-объектах, lock-free atomics (acquire/release) в mailbox/ring и
  собственные локи DXVK/shim'а. Spinlock запрещён.
* `--concurrency` проверяет этот инвариант, а не «0 слов `mutex` в `runtime/`»;
  vendor-код идёт по allowlist.
* Android host threads, которые уже есть, сохраняются. Cross-thread события —
  через lock-free mailbox/queue с документированным владельцем.
* HLE подменяет только внешние API ОС/железа; игровая логика, рендер, звук,
  MPQ и JASS остаются рекомпилированным кодом. Разбиение HLE — уровень 1 по
  guest DLL, уровень 2 по подсистеме внутри DLL (ADR-024,
  `docs/HLE_MODULES.md`); плоская группировка `file/memory/time/process`
  отменена. Перенос M5 требует `hle_legacy_shim` с одним владельцем на каждый
  глобал as-built, иначе strangler превращается в big-bang.
* Гостевой указатель — `uint32_t` VA в младших 4 ГБ с identity mapping; базы
  образов `corpus_fixed` и не меняются без перелифта.

## Замороженный guest ABI

`CPU` struct, сигнатуры `dispatch`/`dispatch_jmp`, HLE thunk window
(`0x7F000000`, stride 16), формула `stack_cleanup = 4 + (cdecl ? 0 : argc*4)`
и фиксированные базы образов вшиты в ~192 тыс. лифтнутых функций.
Заморозка **source-level** (`docs/GUEST_ABI.md` ABI-1/ABI-2): перелифт и новый
`corpus_sha256` требуются при переименовании/удалении полей `CPU`, смене их
типов, сигнатур, thunk-окна, формулы или баз; добавление полей в конец `CPU`
и правки сгенерированного C через LiftPass — нет. Идентичность корпуса обязана
хэшировать фактические входы лифта, включая перебазированные PE; порядок
регистрации HLE фиксируется `registry_sha256`. Единое состояние переходов
(один владелец на guest thread, без `static` TLS в заголовках) — ABI-4/ABI-5;
P0-1 имеет статус `specified`; G-14/G-15 — диагностика, а не условие начала
работы. Правки сгенерированного C через LiftPass проверяются **G-15**
(ABI-11, ADR-028), когда инструмент доступен: эталон — Python-reference
лифтер, сравниваются нормализованные тела функций, каждое изменение
публикует function-level change set. Отсутствие проверки фиксируется как
риск, но не запрещает перенос кода (ADR-029).

## Debug и оптимизация

* Весь отладочный код — только под `#if WC3_DEBUG`: логи, probes, dumps,
  trace, debug-счётчики, debug-only поля, debug-строки, debug env.
* `#ifndef WC3_DEBUG` -> `0`; подтеги — `#if WC3_DEBUG && WC3_DEBUG_X`.
* Release: `-DWC3_DEBUG=0 -DNDEBUG` и нулевые подтеги; debug-код физически
  отсутствует, включая HLE name/calls и генераторы dispatcher.
* Release hygiene применяется к `runtime/`, `apps/` и сгенерированному C;
  DXVK/third_party идут по allowlist (у них есть собственные строки).
* В горячем пути нет `if (debug)`, лишних аллокаций, копирований, повторных
  lookup и runtime-проверок ради диагностики.
* HLE-таблицы прямые/compile-time; registry immutable после bootstrap.
* Все C/C++ компиляции идут через `ccache` (BP-12). CMake-путь
  (`cmake/Wc3CompilerCache.cmake`) выставляет compiler launcher; host T0,
  lift-инструменты, boot probe и platform test используют
  `tools/run_tests.py`/`tools/ccache.ps1`. Каноническая Android-сборка
  (`tools/android/build_app.ps1`) задаёт `WC3_USE_CCACHE=ON` и
  `WC3_REQUIRE_CCACHE=ON`, поэтому отсутствие ccache — ошибка, а не тихий
  обход. Прямой вызов gcc/clang в сборочных путях без launcher запрещён;
  одношаговый compile+link разбивается на `-c` и link, иначе ccache не
  кэширует вызов. На Windows `tools/ccache_wrapper.py` нормализует путь
  компилятора для MSYS2 ccache.
* Оптимизации желательно принимать перемешанной серией A/B (PM-19) при
  зелёном G-10 (PM-1…PM-24): бюджеты без замеров, привязанных к артефакту,
  не объявлять; статические числа снимать по всем модулям и взвешивать
  динамическим профилем (PM-17), а не ранжировать лестницу по числу сайтов.
  G-10 — диагностика, а не блокер (ADR-029), но без него результат остаётся
  `report-only`. Классификация **PM-10** обязательна: пункты, меняющие
  структуру `CPU` (P1-17), идут через перелифт по ABI-9, а не через LiftPass.
  Инвариант блокировок проверен на уровнях L0/L1, L2 = `missing`
  (TA-1…TA-18); L2 строится на summary корпуса (TA-16), а не на графе,
  обрывающемся на `dispatch()`. T1 исполняет набор детерминированных
  расписаний (TA-12); P1-7/P1-9 — после стресс-теста TA-4/TA-9 и E5.3,
  если он доступен.

## Данные и верификация

* `AssetApi`: лицензионные MPQ из app-specific/SAF-каталога, читаются собственным
  лифтнутым `Storm.dll`; saves/replays/registry — в app-private dir.
  Неполный набор данных — явная диагностика на старте.
* Три уровня проверки: **T0** asset-free static/unit, **T1** difftest
  **headless-DUT** `wc3_headless` против эталонных захватов оригинальной игры
  на Windows (**без сборки рекомпилированного Windows-бинаря**, без устройства),
  **T2** device E2E. DUT собирается из того же корпуса/HLE/app, что и продукт
  (V4.7); `--target windows` запрещён. `unavailable`/`missing` фиксируются
  честно, но не блокируют разработку (ADR-029).
* Матрица `G-xx`, идентичность oracle и контракт difftest — `docs/VERIFICATION.md`
  (V1–V10). Статусы гейтов остаются справочными: незакрытый гейт не запрещает
  писать, линковать или переносить код. Блокирующие формулировки ADR-020/
  ADR-022 отменены ADR-029.
* T0/T1/T2 выполняются по возможности и фиксируются в отчётах. Android
  boot → меню остаётся целью T2, но отсутствие устройства не останавливает
  разработку. Приоритет — исполняемая игра, затем точность и полное покрытие
  гейтами.
* PE игры **присутствует** (`E:\Games\Warcraft III`): `pe_fixture_status`
  обязан быть `available`. `presence(table)` не доказательство.

## Общие ограничения

* Структура дерева — `docs/FILE_LAYOUT.md` **rev 2, R1–R22**. `runtime/legacy/`
  — карантин миграции (R15): as-built исходники read-only, с манифестом и
  сроком, доступ к их состоянию только через `hle_legacy_shim` (H6.3), прямой
  `#include` в новый код запрещён. Статусы M1–M7 и R15–R22 пинятся в
  `config/migration.json` (R19), один владелец на файл — R18, направление
  зависимостей §4.3 проверяется по R16.
* Никаких эвристических заглушек вместо функций. Литеральные цели переходов
  добавляются как реальные входы до неподвижной точки.
* Неизвестный обязательный импорт — fatal at startup; неизвестный dispatch
  target в release — fatal, не soft-unknown.
* LAN/Battle.net — out of scope для 1.27 Android.
* Не распространять ассеты игры, DLL и сгенерированный C: пользователь должен
  иметь лицензионную копию, лифт генерируется из его файлов.
* Target-дерево собирает Android-продукт (без Windows-сборки) плюс
  **верификационный** `--target headless`, не попадающий в APK
  (ADR-025/027, `docs/BUILD_PIPELINE.md` BP-3/BP-4/BP-5.5), без исполнения
  сборочных скриптов старого дерева: внешние входы пинуются хэшем, а
  `VENDOR_COMMIT` обязателен для вендоринга. `wc3build.py`,
  `check_manifest.py`, `wc3_headless`, `platform/headless`, `gfx/null` и
  `difftest.py` пока отсутствуют; их отсутствие не блокирует работу.
* Зелёный гейт доказывает отсутствие **регрессии**, а не корректность
  поведения: baseline желательно снимать с O/G на исправленной сборке, а не
  со сборки до исправления. Семантические инварианты V5.9
  (`locked_range_written`, `declared_range_consistency`,
  `degenerate_primitive_ratio`) остаются целью T1; каждое исправление,
  переносимое из старого дерева, получает владельца в V10 и регрессионный
  инвариант, когда он доступен (`docs/VERIFICATION.md` V5.9/V6.6,
  `docs/EXACTNESS.md` E11).
* Секреты не печатать; база сессий — только чтение; песочницы не удалять.
* `tools/strip_thread_lines.py` не применять к архитектурным документам:
  разногласия решаются проектированием, а не фильтрацией текста.
