# План рефакторинга архитектуры WC3 1.27b (Android-only)

Статус: **rev 2**, Step 1 **DONE**; следующий — Step 2 (обновлён под
архитектуру rev 2). Цель: привести **уже работающий** Android-runtime к
целевой структуре без изменения семантики рекомпилированного x86-32 кода и без
потери исполняемого контроля на любом шаге. Исполнимость этого контроля
фиксируется `docs/VERIFICATION.md` (V1, V2, V6): Step 0 не закрыт, пока
acceptance-прогон V7 не зелёный.

Архитектура: `docs/ARCHITECTURE_1.27B.md` (**rev 2**, as-built → target).
При расхождении приоритет у архитектуры. Критика:
`reports/REFACTOR_PLAN_CRITIQUE_2.md`,
`reports/REFACTOR_PLAN_CRITIQUE_2_FOLLOWUP.md`,
`reports/CRITIQUE_FILTER_THREADING.md`.

## 0. Что изменилось в rev 2

Отменены пять решений rev 1, которые делали план неисполнимым:

1. «Принудительная однопоточность / один стек / никаких локов» → реальные
   guest threads + инвариант блокировок (§1.3, ADR-016/018).
2. «Windows-хост и difftest не проектируются» → Windows-эталон обязателен
   (ADR-002). **ADR-026:** это оригинальная игра и её захваты; рекомпилированный
   Windows-хост не собирается, difftest идёт против захватов (T1).
3. «`pe_fixture_status=unavailable`, PE нет» → PE присутствует
   (`E:\Games\Warcraft III`), фикстура обязана читать PE (§1.8).
4. «Только Android-дерево, старый проект исторический» → strangler-cutover из
   исполняемого старого дерева (§2, ADR-020).
5. «`vkEnumerateInstanceVersion` выбирает DXVK, Turnip вне политики» →
   physical-device селектор с fallback и явный `VulkanDriverApi` (ADR-007).

Добавлены: `ThreadApi`/`SyncApi`/`AssetApi`/`VulkanDriverApi`, 36 thread/sync
HLE-записей, layout как валидатор корпуса (`corpus_fixed`), T0/T1/T2
верификация, замороженный guest ABI.

## 1. Обязательные ограничения

1. **Продукт и единственный поставляемый target** — только Android arm64-v8a,
   API 26+ (ADR-026/027). Под Windows не собирается ни один рекомпилированный
   бинарь; `--target windows` запрещён. Разрешён верификационный `--target
   headless` (`wc3_headless`, CI-хост, не APK) как исполнимый DUT T1 (V4.7).
   Эталон — оригинальная игра на Windows и её зафиксированные захваты:
   difftest без adb, без Windows-сборки.
2. Графика — только thin bridge D3D9 → DXVK. Оба статических DXVK —
   **1.10.3** и **3.1.1** — в одном APK (symbol isolation через `objcopy`).
   Селектор по physical device (`apiVersion` + extensions) с **fallback** при
   ошибке создания device; только один бэкенд создаёт instance/device.
   OpenGL, GLES, `native_vk`, `hle_gl`, `hle_d3d9_vk`, `D9VkApi` и самописный
   Vulkan-транслятор удаляются. D3D8 — legacy 1.26.
3. `VulkanDriverApi` (`system` | `turnip`) — разрешённый путь выбора Vulkan ICD;
   adrenotools/Turnip не являются запрещённой подменой D3D-провайдера.
   Запрет — на внешние `d3d9.dll`/`d3d8.dll`, PATH и runtime-подмену frontend'а.
4. **Guest threads сохраняются**: `CreateThread`/`_beginthreadex` → реальный
   host thread; каждый имеет `CPU`, TEB, stack, TLS (`GUEST_THREAD_MAX = 24`).
   Кооперативное схлопывание запрещено. Собственные новые host threads runtime
   не добавляет; существующие Android/DXVK-потоки сохраняются.
5. **Инвариант блокировок**: ни один host lock не удерживается при `dispatch()`,
   обратном вызове в гость или ожидании guest-visible объекта. Локи запрещены в
   `runtime/cpu`, `runtime/guest`, `runtime/hle/abi`; разрешены per-object в
   HLE-объектах, lock-free atomics (acquire/release) в mailbox/ring и
   собственные локи DXVK/`dxvk_win32`. Spinlock запрещён.
6. Guest thread/sync imports аудируются, но **не удаляются до evidence**.
   36 записей реализуются с точной семантикой Win32; no-op waits и stubs
   (`TerminateThread`/`SuspendThread`/`ResumeThread`) заменяются или fatal.
7. Весь debug-код — под `#if WC3_DEBUG`. Release: `WC3_DEBUG=0`, `NDEBUG`; нет
   логов, probes, dumps, trace, debug-only полей, debug-строк и debug env.
   Release hygiene — на нашем коде; DXVK/third_party по allowlist.
8. Горячий путь: прямые compile-time таблицы, registry immutable после
   bootstrap, без лишних аллокаций, копирований, повторных lookup, `getenv` и
   debug-ветвлений.
9. `AssetApi`: лицензионные MPQ из app-specific/SAF-каталога, читаются лифтнутым
   `Storm.dll`; saves/replays/registry — app-private dir. Неполный набор данных
   — явная диагностика на старте.
10. LAN/Battle.net — out of scope для 1.27 Android; `NetApi` отсутствует.
11. Exactness важнее удобства. Неизвестный обязательный импорт — fatal;
    неизвестный dispatch target в release — fatal, не soft-unknown.
12. Presence проверяется **по PE** (PE есть), arity — по canonical spec +
    call-site анализу, callconv — по spec. `presence(table)` не доказательство;
    `unavailable` при существующем PE — ошибка фикстуры.
13. Точность определена нормой `docs/EXACTNESS.md` (E1–E12): эталоны,
    детерминизм входа, единая модель времени, FP/replay, модель памяти.
    Оптимизация §3.7 не имеет приоритета над E-контрактом; difftest не выдаёт
    `pass` до golden-наборов E3/E4 (V5.6).

## 2. Инварианты

1. Один `HleEntry`, один реестр, один `hle_call`.
2. `stack_cleanup = 4 + (cdecl ? 0 : argc*4)`; поле `user` для COM-слотов.
3. `argc` — из проверенной canonical spec + call-site анализа, не из PE import table.
4. Presence, arity и callconv — три разных источника.
5. Один layout/corpus manifest; target только `android`; базы `corpus_fixed`.
6. Образы PE32 маппятся по фиксированным адресам (identity mapping, младшие
   4 ГБ); layout — валидатор корпуса, не настраиваемая ручка.
7. Каждый guest thread имеет свой stack/TEB/`CPU`/TLS и dispatcher state.
8. Thin bridge D3D9 → DXVK — единственный graphics frontend; оба DXVK встроены.
9. Нет внешних `d3d9.dll`/`d3d8.dll`, PATH, runtime-подмены D3D-frontend'а.
10. Android host threads сохраняются; cross-thread события — lock-free mailbox.
11. `check_arch --concurrency` проверяет инвариант §1.5 по зонам владения, а не
    «0 mutex в `runtime/`»; guest threads и DXVK shim/third_party — allowlist.
12. Release не содержит нашего debug-кода (`check_arch --release` по
    preprocessed output и бинарнику).
13. Замороженный guest ABI (`CPU`, dispatcher, HLE thunk window, формула снятия
    стека, базы образов): изменение = перелифт и новый `corpus_sha256`.
14. **Каждый шаг cutover заканчивается зелёными T0 + T1** (static/unit и
    difftest против эталонных захватов оригинальной игры; Windows-бинарь не
    собирается — ADR-026), без adb и устройства.
    Android `.so` boot → меню — T2, обязателен на Step 9 и при доступном
    устройстве, но не может быть per-step gate в текущем контуре.
15. Верификация T0 (static/unit) + T1 (difftest против эталонных захватов,
    без Windows-сборки) + T2 (device).
    `unavailable ≠ pass` для T2; при зелёных T0/T1 проект не BLOCKED.
16. PE игры доступен; `pe_fixture_status=available`.
17. `AssetApi`/`VulkanDriverApi` — обязательные интерфейсы `runtime/platform`.
18. Гейт со статусом `declared`/`missing` не считается зелёным; acceptance-прогон
    не содержит ослабляющих флагов; `unavailable ≠ pass` на всех уровнях
    (ADR-022, `docs/VERIFICATION.md` V1/V2/V6).
19. **T1 имеет исполняемый DUT**: `wc3_headless` (`--target headless`, CI-хост,
    не Windows, не APK) исполняет тот же корпус и HLE, что и продукт, с
    `platform/headless` + `gfx/null`; статический хэш входов не заменяет
    difftest (ADR-027, `docs/VERIFICATION.md` V4.7/V5.7/V5.8).
20. **Гейт = отсутствие регрессии, а не корректность поведения**: baseline снимается
    с O/G на исправленной сборке, семантические инварианты V5.9 обязательны
    в T1, каждое переносимое исправление имеет владельца в V10 и
    регрессионный инвариант до cutover (`docs/VERIFICATION.md` V5.9/V6.6,
    `docs/EXACTNESS.md` E11).

## 3. Порядок шагов

### Step −1. Зафиксировать baseline исправленного рендера (до cutover)

Графический дефект as-built порта найден и **исправлен в старом дереве**
(причина — `guest_ok()`/`cr_memcpy`, `PROGRESS.md`). В целевом дереве он не
расследуется и не является отдельной работой плана: задача шага — не
потерять исправление при strangler-cutover.

Действие:
* снять эталонный сценарий с **O** и baseline с **исправленной** сборки
  старого дерева, записать identity (V3) и маркеры инвариантов V5.9;
* включить `locked_range_written`, `declared_range_consistency` и
  `degenerate_primitive_ratio` в `gfx/null`/`difftest` **до** первого шага,
  переносящего graphics/HLE-код (V6.6);
* Acceptance: инварианты зелёные на исправленной сборке и дают `fail` при
  намеренно внесённой регрессии (самопроверка V5.4); baseline хранится
  в `oracle/`.

### Step 0. Базовая линия и гейты (prerequisite для всех шагов)

Действие:
* зафиксировать as-built: `libwc3gameactivity.so`/`libwc3game-thin.so`,
  boot → menu → map маркеры, frame hash, 191 650 адресов dispatcher,
  426 HLE-записей, 18 `CreateThread`; идентичность эталонных захватов — по
  `docs/VERIFICATION.md` V3 (pe_sha256 и набор захватов, без Windows-сборки —
  ADR-026);
* `config/baseline.1.27b.before.json` — `{path, sha256, status, exit_code,
  marker_tail, captured_at}`; `tools/smoke.py record|verify`;
* поднять T1 **с исполняемым DUT** (ADR-027, V4.7): собрать `wc3_headless`
  через `wc3build.py build --target headless` из того же корпуса/HLE/app, что
  и Android-продукт, с `platform/headless` (детерминированные 15 интерфейсов
  `Wc3Platform`, виртуальные guest-тики) и `gfx/null` (запись канонического
  потока команд D3D9 вместо пикселей). **Ни DUT, ни харнеса ещё нет** —
  реализовать контракт `docs/VERIFICATION.md` V4.7/V5 (фиксированный seed и
  сценарий ввода, контрольные точки, **нормализованный** хэш guest memory,
  снимок `CPU` при расхождении, byte-compare save/replay, хэш потока команд,
  пин хэшей DUT по V5.7, самопроверка V5.4). Границы сравнения двух частично
  совпадающих корпусов — V4; `--target windows` по-прежнему запрещён;
  статус G-09/G-13;
* `tools/run_tests.py` — единая точка T0/T1/T2, без ослабляющих флагов в
  acceptance-прогоне (V1.4): добавить `--require-hle`, убрать
  `--allow-unavailable` из acceptance, вызвать `--policy`/`--renderer`
  (G-02, G-03, G-04);
* зафиксировать **критерий точности** до любых оптимизаций:
  `docs/EXACTNESS.md` E2 (сценарий в guest-тиках), E3 (таблица коэффициентов
  времени и `CLOCK_MONOTONIC`), E4 (golden-набор бит-точных `sqrt/pow/sin`),
  E5 (стресс-тест ≥2 guest threads). До E3/E4 харнес G-09 не выдаёт `pass`
  (V5.6), а frame hash не является гейтом (E3.7);
* снять **perf-baseline до любых оптимизаций** (§3.7 архитектуры):
  счётчики `dispatch/frame`, `probes/frame`, `flags/frame`, `x87 ops/frame`,
  `staging bytes/frame`, `CS acquisitions/frame`, contention; frame time
  p50/p95/p99 на T1; 191 650 записей dispatcher и 2 МиБ hash-таблицы;
* привести существующие гейты к rev 2: `check_arch.py` (проверка по зонам
  владения, `--policy`/`--renderer` по link graph), `import_fixture.py`
  (чтение PE — уже сделано), `check_layout.py` (заполнить
  `corpus_manifest.sha256` по **фактическим**, в том числе перебазированным,
  входам лифта — V4.6, G-05);
* закрыть долги V10 с приоритетом G-08 → G-09 → G-03 → G-02 → G-05;
  **baseline исправленного рендера и инварианты V5.9** имеют владельца в V10
  и закрываются до переноса graphics/HLE-кода (V6.6);
  отсутствующие инструменты перечислены там же (`wc3build.py`, генератор
  thin bridge, release-hygiene test); `check_layout.py`, `check_docs.py` и
  `device_test.py` уже существуют;
* все as-built числа привязать к `{источник, hash, timestamp}`; ADR-022 и V1
  делают статус гейта частью архитектуры.

Acceptance — канонический прогон `docs/VERIFICATION.md` V7 (не дублируется
здесь). На 2026-11-06 он ожидаемо не зелёный (V7.1): V2 фиксирует
`enforced` 3 / `declared` 6 / `missing` 6 (включая G-14 — контракт вызова
и G-15 — эквивалентность LiftPass по ABI-11).

```powershell
python tools/check_file_layout.py --strict --json reports/generated/file_layout.json
python tools/smoke.py record --phase before --out config/baseline.1.27b.before.json
python tools/smoke.py verify --baseline config/baseline.1.27b.before.json
# далее — V7; T1: identity захватов (V3) + headless-DUT (V4.7) + difftest (V5)
```
Без закрытия G-08/G-09 не начинаются шаги, переносящие исполняемый код
(V6.2): иначе регрессию нечем поймать. Подготовительные шаги (включая Step 2)
допустимы при честно зафиксированных статусах V2 (V6.1).

### Step 1. Документы и политика (DONE)

Действие: перенесены документы, зафиксированы Android-only, DXVK-only
1.10.3/3.1.1 с fallback, guest threads, инвариант блокировок, Windows-эталон,
`AssetApi`/`VulkanDriverApi`, compile-time debug exclusion.

Acceptance:
```powershell
rg -n "wc3_host|native_vk|OpenGL|hle_gl|hle_d3d9_vk|D9VkApi" `
  docs\ARCHITECTURE_1.27B.md PLAN_REFACTOR_1.27B.md AGENTS.md README.md
# только запрещающие формулировки; Windows упомянут как эталон (без рекомпиляции)
```

### Step 2. Asset-free import/ABI/thread baseline

Действие:
* сгенерировать `config/imports.1.27b.json` **из лицензионного PE**
  (`E:\Games\Warcraft III`): `va`, `name`, `module`, `ordinal`, `source`;
  `tools/import_fixture.py` получает `--pe`/`--game-dir` и перестаёт
  выдавать «PE not present»;
* `pe_fixture_status=available`; presence — по PE import directory;
* `config/hle_arity.1.27b.json` и `cdecl_allowlist.1.27b.json` сверить с
  анализом call-site (дельта `esp` перед `call [IAT]`);
* `docs/THREAD_AUDIT.md`: call sites, lifetime, waits, TLS, DllMain, evidence;
  `unavailable` только для device-зависимых строк;
* thread/sync код не удаляется и не заменяется заглушками до аудита.

Acceptance:
```powershell
python tools/import_fixture.py --corpus 1.27b --game-dir "E:\Games\Warcraft III" --out config/imports.1.27b.json
python tools/hle_coverage.py --corpus 1.27b --fixture-kind pe
# presence(pe) OK; pe_fixture_status=available
python tools/check_thread_audit.py --corpus 1.27b --allow-unavailable
python tools/run_tests.py --test hle_abi
```

### Step 3. HLE ABI и единый registry

Действие:
* один `HleEntry`/registry/call bridge; hot-поля `stack_cleanup`, `argc`,
  `flags`, `impl`, **`user`**; cold `name/dll/calls` — только `#if WC3_DEBUG`;
* `stack_cleanup = 4 + (cdecl ? 0 : argc*4)` в одном месте; принудительная
  установка `esp_in + stack_cleanup`;
* COM/DSound/GL forwarder'ы переносятся через `user` (as-built `e->fwd`);
* `runtime/host` — legacy Windows HLE, не входит в Android link graph; старые
  таблицы — oracle-адаптер до cutover;
* `HLE_OPTIONAL` требует точный `argc`; неизвестный обязательный импорт —
  fatal; `hle_stub` отсутствует; `hle_describe` — только debug.

Acceptance:
```powershell
python tools/run_tests.py --test hle_abi
# stack_cleanup 16 / 4, user round-trip, EBX/ESI/EDI/EBP сохранены
python tools/check_arch.py --hle-abi --concurrency
python tools/hle_coverage.py --corpus 1.27b --fixture-kind pe
```

### Step 4. GuestRunner, Android platform и данные

Действие:
* `runtime/platform/platform.h`: `MemApi`, `FileApi`, `TimeApi`, `LocaleApi`,
  `WindowApi`, `InputApi`, `AudioApi`, `ClipboardApi`, `SurfaceApi`, `LogApi`,
  `ConfigApi`, **`ThreadApi`**, **`SyncApi`**, **`AssetApi`**,
  **`VulkanDriverApi`**; `NetApi` не вводить;
* `runtime/app/guest_runner`: `Created → Running ⇄ Paused → SurfaceLost /
  AudioDeviceLost → Running → Stopping → Stopped`;
* guest threads сохраняются, каждый получает CPU/TEB/stack/TLS; `platform/fake`
  поддерживает многопоточные unit-сценарии;
* Android callbacks — в owning guest thread через lock-free mailbox;
* `AssetApi`: проверка всех четырёх MPQ, маппинг Windows-путей, save root;
* debug/trace в GuestRunner — только под `#if WC3_DEBUG`.

Acceptance:
```powershell
python tools/run_tests.py --test guest_runner
python tools/check_arch.py --lifecycle --platform-contract --concurrency --release
# ThreadApi/SyncApi/AssetApi/VulkanDriverApi есть; NetApi отсутствует
python tools/run_tests.py --test asset_api
```

### Step 5. HLE-модули, debug-gating и оптимизация

Действие:
* разбиение HLE — по норме `docs/HLE_MODULES.md` (ADR-024, H1/H2/H5):
  уровень 1 = guest DLL, уровень 2 = подсистема; `kernel32` делится на
  memory/file/time/process/locale/**thread**/**sync**, `msvcr80`, `miles` и
  `display` — отдельные модули. 36 записей перенести с точной семантикой,
  заменить stubs `TerminateThread`/`SuspendThread`/`ResumeThread`;
* до первого модуля M5 обязателен `hle_legacy_shim` с одним владельцем на
  каждый file-scope глобал as-built (`g_handles`, `g_uq`, `g_key_state`,
  `g_hle[]`, `arg32`, `rd32g`) — H6.3: без разделения состояния шов
  strangler отсутствует; карта строк `hle_android.c` → модули — H5;
* HLE core не включает OS-заголовки и не содержит `#ifdef __ANDROID__`;
* все `WC3_*` env аудировать; debug-переменные не читаются в release;
  не-debug bootstrap — один раз, не в горячем пути;
* весь HLE debug-код и генераторы — под `#if WC3_DEBUG`;
* убрать лишние аллокации, копирования, повторные registry lookup; таблицы
  HLE — прямые/compile-time;
* **P0-4 по §3.7**: устранить глобальные локи горячего пути — `cs_get()`
  (глобальный мьютекс + линейный скан 512 на каждый
  `Enter/LeaveCriticalSection`) заменить direct-mapped таблицей по guest-адресу
  с per-object recursive mutex; `g_heap_lock` шардировать; `gh_new()` → atomic
  freelist; `g_sync_next++` → атомик (сейчас гонка); `GThread[24]` выровнять
  по 64 Б; `rd32g/wr32g` перевести с `volatile` на `may_alias`;
* `check_arch.py`: зависимости, запрет OS-заголовков/env, concurrency по зонам,
  release-debug, ацикличность, а также gate «нет глобального лока на горячем
  пути».

Acceptance:
```powershell
python tools/check_arch.py --strict --concurrency --release
python tools/run_tests.py --test platform_contract
python tools/hle_coverage.py --corpus 1.27b --fixture-kind pe
# плюс T0 unit и T1 difftest против эталонных захватов (без устройства/Windows-сборки)
```

### Step 5b. LiftPass: оптимизации лифта без смены guest ABI (§3.7)

Baseline (уже снят, воспроизводим):
```powershell
python tools/perf_probe.py --json reports/generated/perf_baseline.static.json
# 191 650 записей dispatcher; 42 720 константных вызовов, 99,98% в lift set;
# 82,93% dispatch-трафика снимаемо; 44,5% мёртвых записей флагов;
# 24 261 push адреса возврата; 28 591 GVA(); 1 093 IAT-подобных вызова;
# hash: 2^19 = hit 1,230 / 2^18 = 2,098 / 2^20 = 1,091 → P1-5 ЗАКРЫТ
```

**Prerequisite — НЕ ВЫПОЛНЕН (исправлено по `docs/GUEST_ABI.md` ABI-6,
гейт G-14):** `runtime/guest/direct_call.h` лишь специфицирует контракт
`WC3_DIRECT_CALL`/`WC3_DIRECT_TAIL`/`WC3_GUEST_DRAIN`. Он объявляет `static`
TLS в заголовке (копия состояния на каждый чанк корпуса), не связан с
`g_disp_depth`/`g_tail_pending` из `tools/gen_dispatch.py` и не включается ни
одним сгенерированным TU; `tests/direct_call_test.c` — один TU и не ловит
ни кросс-TU tail, ни смешанный путь. Единое состояние, смешанный путь и
обязательные тесты — ABI-4/ABI-5/ABI-6. **До закрытия G-14 и G-15 конвейер
к корпусу не применяется (V6.5, ADR-028/ABI-11).** G-15 доказывает, что
каждый pass сохранил семантику нормализованных тел функций относительно
Python-reference лифтера; `tools/lift_equivalence.py` отсутствует, статус
`missing`.

Наивная замена `dispatch(c, 0xVA)` → `L_<va>(c)` **неверна**: `dispatch_jmp`
полагается на `g_disp_depth > 0`, иначе guest-стек верен, а host-вызовы
ломаются. `tools/lift/passes/devirtualize.py` (prototype, dry-run) насчитал
**22 197 (52%)** константных вызовов, callee которых имеют tail/indirect
переходы, и 20 148 с `dispatch_jmp` в callee. `return fn(c);` в void-функции
запрещён C11 6.8.6.4 — используется публикация указателя.

Действие — упорядоченный конвейер `tools/lift/passes/`, каждый pass с
 golden-тестом эквивалентности нормализованных тел функций (**G-15**,
 ABI-11), function-level change set и флагом отключения. Все pass меняют
сгенерированный C, **не** `CPU` и не сигнатуры
dispatcher (добавление полей в конец `CPU` допустимо — контракт
source-level):

1. `import_devirtualize` (**P0-5**) — IAT-слот → `hle_call` при bind'е импортов;
2. `devirtualize` (**P0-1**) — `dispatch(c, 0xVA)` →
   `WC3_DIRECT_CALL(L_<va>, c)`, `dispatch_jmp` →
   `WC3_DIRECT_TAIL(L_<va>, c)`; 42 713 применимых из 42 720;
3. `return_address_elide` (**P0-6**) — убрать запись адреса возврата, если
   до соответствующего `ret` его никто не читает; сохранить `call/pop` и SEH;
4. `dead_flags` (**P0-2**) — ленивые флаги / DFE, нижняя граница 44 034 записи;
5. `gva_fold` (**P0-3**) — `g_image_delta` → compile-time `0`;
6. `superblock` — склейка всегда следующих друг за другом тел в пределах чанка;
7. ThinLTO/inlining для прямых вызовов; hot/cold placement по
   `config/hot_functions.1.27b.txt`, снятому на **T2-устройстве или из
   эталонных захватов** (рекомпилированный Windows-хост для профиля не
   собирается, ADR-026; формат профиля gcc↔clang не нужен).

Acceptance:
```powershell
# контур измерений G-10 (PM-1..PM-24, V6.7, ADR-029): счётчики,
# калиброванный baseline по всем модулям, классификация PM-10, атрибуция
# PM-18, серия A/B PM-19, фазы PM-20 и отчёт V1.6 существуют ДО первого
# замера «после»; без них результат = report-only (класс C — через ABI-9)
# G-14 (docs/GUEST_ABI.md ABI-6): кросс-TU >= 2 TU, прямой -> dispatch_jmp,
# legacy -> прямой tail, 2 guest threads, golden-эквивалентность, release
python tools/run_tests.py --test direct_call
# tail chain, nesting, 100000 hops: OK (недостаточно без G-14)
python tools/lift/passes/devirtualize.py --json build/lift/devirt_plan.json
# converted == 42713; макросы обязательны для всех 22197 сайтов с depth
python tools/perf_probe.py --json config/perf_baseline.after.json
# devirtualizable_pct -> ~0, dispatch_traffic_removed_pct >= 80,
# return_address_push_pairs -> ~0, dead_flag_pct -> ~0
# G-15 (ABI-11): golden-набор обязан покрывать ВСЕ модули, не только war3_27;
# change set пуст при выключенных pass'ах; инструмент отсутствует (missing)
python tools/lift_equivalence.py --golden all --check-normalized-bodies --changeset build/lift/changeset.json
python tools/perf_report.py --before reports/generated/perf_baseline.static.json --after config/perf_baseline.after.json
# T1: markers/frame hash идентичны; dispatch/frame и probes/frame упали;
# frame time p50/p95/p99 не хуже baseline
```
Оптимизация принимается только с парой замеров; без P0-1 не решать вопрос о
`dispatch ABI v2` (G6). Тюнинг хэша и cuckoo-таблицы **не делать** — измерено
как тупик.

### Step 5c. Устранение гонок на HLE hot path (§3.7, P0-7)

Baseline: `python tools/concurrency_audit.py --json
reports/generated/concurrency_audit.static.json` — 186 глобалов файловой области,
140 разделяемых с записями, **10 подтверждённых находок**.

Действие:
* `hle_call`: убрать безусловные `e->calls++`, `g_last_cpu`, `g_ring[]`,
  `g_ring_pos`, `g_ring_len` из release-пути; диагностику перевести в
  per-thread буферы под `#if WC3_DEBUG`, `g_last_cpu` — в `CPU`/TLS;
* `gh_new_sync()`/`gh_new()` → atomic freelist вместо `g_sync_next++` и
  линейного скана без лока; `g_hle_count` — только в `LATE_BIND` до `frozen`;
* `GThread`: сбрасывать `used` и сигнализировать thread handle при выходе,
  реализовать `pthread_join`/`GetExitCodeThread`, убрать утечку 24 слотов;
* **P0-8 mailbox**: заменить «один MPSC» на набор per-producer SPSC очередей
  (`ui`/`input`/`overlay`/`ime`/`canvas`) с одним consumer'ом и фиксированным
  приоритетом обхода; текущий `wc3_mailbox_push()` без CAS корректен только
  для одного producer'а;
* **P0-9 контракт host-потока**: `pthread_attr_setstacksize(8 МиБ)`,
  detach/join, `cpu_x87_init()` + TLS SoftFloat на каждом guest thread,
  `PTHREAD_PRIO_INHERIT` для CS/mutex; `sched_setaffinity` по P1-9
  (норма — TA-2);
* **TA-1…TA-18**: матрица владения file-scope глобалами as-built, уровни
  L0/L1/L2 проверки инварианта блокировок, расписания T1 (TA-12),
  реентерабельность (TA-13), shutdown (TA-14), summary корпуса для L2
  (TA-16) и стресс-тест E5.3 с ≥2 guest threads (`docs/THREAD_AUDIT.md`
  §8–§9); P1-7/P1-9 — после TA-4, если стресс-тест доступен;
* `concurrency_audit.py --strict` и `--concurrency` проверяют классификацию
  `none | per-object | global`; глобальный лок на горячем пути — ошибка гейта.

Acceptance:
```powershell
python tools/concurrency_audit.py --strict
# verified_findings == 0
python tools/run_tests.py --test thread_sync
# два guest threads: create/join, CS recursive, event/semaphore, no duplicate handles
# T1: markers/эталонные хэши идентичны (без Windows-сборки)
```

### Step 6. Android UI, input и audio

Действие:
* вынести `user32`, `imm32`, `shell32`, `comctl32`, `winmm`, `dsound` в
  HLE-модули над `WindowApi`, `InputApi`, `AudioApi`, `ClipboardApi`;
* перенести vendored `winlator-input` (MIT) как единую подсистему с LICENSE;
* input/overlay/IME/canvas маршрутизируются в owning guest thread; UI не
  вызывает HLE; буферы canvas/overlay с атомарным индексом, без общего mutex;
* audio: AAudio + silent sink; ring lock-free; существующий audio/callback
  thread сохраняется;
* `errorCallback`: close + reopen с backoff или silent sink;
* debug overlay/traces/underrun counters — только под `#if WC3_DEBUG`.

Acceptance:
```powershell
python tools/check_arch.py --strict --concurrency --release --policy
python tools/run_tests.py --test input_event
python tools/run_tests.py --test audio_ring
```

### Step 7. Thin bridge D3D9 → DXVK и статический DXVK

Действие:
* оставить только thin bridge; оба DXVK 1.10.3 и 3.1.1 статически в одном
  APK с symbol isolation (`objcopy --prefix-symbols` по member'ам архива);
* dual сохраняется **как `provisional`** (ADR-004/007, ADR-023): норма —
  `docs/GRAPHICS_DECISION.md`. До измерений GB-5 (размер, старт, device
  matrix, `dxvk_shim_reset()`, аудит статических конструкторов) решение не
  получает статус `accepted` и не может обосновывать shipping; на границе
  Step 7 → Step 9 обязателен пересмотр по правилу GB-6;
* селектор: `VulkanDriverApi` → `vkEnumeratePhysicalDevices` → `apiVersion` +
  extensions → 3.1.1 при ≥1.3, иначе 1.10.3; **fallback** при ошибке создания
  device; только выбранный бэкенд создаёт instance/device;
* проверить, что статические конструкторы невыбранного DXVK не создают
  потоки/устройства;
* удалить OpenGL/GLES, `native_vk`, `hle_gl`, `hle_d3d9_vk`, `D9VkApi`,
  legacy `hle_d3d9.cpp`/`hle_d3d9_dxvk.cpp`, legacy A/B, `win32_d3d9`;
* `dxvk_win32` — DXVK-required thread/sync shim с поимённым allowlist;
* WSI: `ANativeWindow`/`SurfaceApi`; surface loss/resize в GuestRunner;
* frame dumps/counters/traces — только `#if WC3_DEBUG`;
* caps/fixed-function и LockRect staging без host lock в guest-пути.

Acceptance:
```powershell
python tools/check_arch.py --renderer --policy --concurrency --release
python tools/gen_d3d9_thin_bridge.py --check
python tools/wc3build.py check --target android
# --policy банит внешние D3D-DLL, но НЕ adrenotools/Turnip
# T2 (GB-5): размер/старт/device matrix/dxvk_shim_reset/static-ctor audit;
# без них ADR-004/007 остаются provisional, а DXVK matrix = unavailable
```
Device boot, frame hash и DXVK matrix — в Step 8/9 (T2).

### Step 8. Android build, CI и release hygiene

Действие:
* норма сборки — `docs/BUILD_PIPELINE.md` (ADR-025, BP-1…BP-11): вендоринг
  android-скриптов с `VENDOR_COMMIT`, `build_upstream.json` до переноса,
  запрет неявных путей старого дерева, измеримый конец cutover (BP-4.4);
* `tools/wc3build.py` — **тонкий оркестратор** над проверенным NDK/PowerShell
  конвейером (`build_boot_probe.ps1`, `build_dxvk_probe.ps1`, Gradle), не его
  переписывание: `corpus`, `build`, `check` с `--target android`;
* генерирует списки источников, `layout`/`corpus` манифесты, `link_apk.rsp`;
  оба DXVK собираются всегда;
* фиксировать NDK/SDK/build-tools/Gradle/CMake/Python;
* CI: T0 asset-free static на любом runner; T1 reference build на private
  runner с лицензионным PE; T2 device smoke на self-hosted Android runner;
* release: `-DWC3_DEBUG=0 -DNDEBUG`, все подтеги `0`, без debug-символов;
* `release_hygiene_test` — на `runtime/`, `apps/` и сгенерированном C;
  DXVK/third_party по allowlist.

Acceptance:
```powershell
python tools/wc3build.py build --target android --clean
python tools/wc3build.py build --target android   # no-op
python tools/check_manifest.py
python tools/run_tests.py --unit
python tools/run_tests.py --test release_hygiene
python tools/check_arch.py --release --concurrency --policy
python tools/device_test.py --require-device
# exit 0 только с устройством; иначе explicit unavailable (T2), не BLOCKED
```

### Step 9. Legacy cleanup и Android E2E

Действие:
* перенести 1.26-документы в `docs/HISTORY/1.26/`; удалить OpenGL, native_vk,
  D3D8, `D9VkApi`, мёртвые build-каталоги и env allowlist;
* **ADR-026:** рекомпилированный Windows-хост не переносится и не собирается;
  `runtime/host_ref` не создаётся. В `oracle/` хранятся только эталонные
  захваты оригинальной игры и инструменты сравнения;
* финальный E2E на Android: меню → карта → 60 секунд → save → load → exit;
  lifecycle: home/resume, screen off/on, rotation, surface loss, audio device
  change;
* сравнить markers/hashes с baseline T1/T2;
* подтвердить: guest threads сохранены, инвариант блокировок соблюдён, release
  без нашего debug-кода, только DXVK 1.10.3/3.1.1 + fallback, Turnip/system
  документированы, 36 thread/sync записей без stubs.

Acceptance:
```powershell
python tools/check_docs.py --strict-final
python tools/check_arch.py --strict --concurrency --release --policy
python tools/run_tests.py --e2e
python tools/smoke.py verify --baseline config/baseline.1.27b.json --require-device
python tools/check_thread_audit.py --corpus 1.27b --require-pass
```
Плюс `git status` чистый (кроме ранее существовавших submodule changes) и
отчёт before/after по поведению.

## 4. Риски порядка

* Step 3 нельзя начинать до Step 2 и Step 0: без canonical `argc` нельзя
  безопасно снять стек, без T1 нечем ловить регрессию.
* Step 4 нельзя начинать до Step 3: GuestRunner не должен обходить HLE ABI.
* Step 5–6 — перенос ~20 000 строк рабочего C/C++; самый большой риск
  проекта. Каждый модуль — отдельный коммит, **T0 + T1** после каждого;
  device boot (T2) — при доступном устройстве и на Step 9.
* Step 7 нельзя начинать до Step 3; device A/B разрешён только между DXVK
  1.10.3 и 3.1.1; без устройства — T2 `unavailable`, не BLOCKED.
* Step 8 — после Step 4–7; Step 9 — после Step 8 baseline.
* Если шаг провалил check дважды — остановиться и пересмотреть план, а не
  обходить проверку.
* Ни на одном шаге не меняется замороженный guest ABI (§2.13).

## 5. Что изменили critique-pass'ы и rev 2

1. Step 3 стал staged: adapter/oracle и отказ от big-bang merge.
2. Presence — по PE fixture; arity — по canonical spec + call-site.
3. Baseline фиксирует `empty`/`unavailable`, а не pass.
4. GuestRunner/lifecycle — до UI/audio; добавлены surface/audio device loss.
5. Step 7 — thin bridge, статические DXVK 1.10.3/3.1.1, fallback-селектор.
6. `AGENTS.md`/`README.md` обновляются в Step 1, а не в конце.
7. Добавлены asset-free import fixture, pinned toolchain и CI job.
8. `check_docs.py` получил `--strict`/`--strict-final`; 1.26 — в Step 9.
9. **ADR-026 уточняет ADR-002:** Windows-эталон сохранён как оригинальная игра
   и её захваты, но рекомпилированный Windows-хост не собирается; Windows
   platform продукта отсутствует.
10. **Guest threads сохранены**; `ThreadApi`/`SyncApi` обязательны; удаляются
    только stubs и лишние host-локи, не потоки.
11. Инвариант блокировок заменил тотальный запрет: локи запрещены в guest-пути
    и под guest-кодом, разрешены в HLE-объектах и vendor-коде.
12. Весь debug-код — под `#if WC3_DEBUG`; release gate с allowlist для DXVK.
13. Горячий путь очищается от лишних аллокаций, копирований, повторных lookup
    и debug-ветвлений.
14. Layout — валидатор корпуса (`corpus_fixed`), добавлены thread stacks,
    7 VA-арен, HLE thunk window, identity mapping.
15. Добавлены `AssetApi` (MPQ/saves), frame pacing и `VulkanDriverApi`
    (system/Turnip); `--policy` больше не банит adrenotools.
16. Верификация T0/T1/T2: device нужен для финального E2E, но не является
    единственным источником истины.
17. Добавлены ADR-018 (модель потоков), ADR-019 (замороженный guest ABI),
    ADR-020 (strangler-cutover), ADR-021 (поток данных).
