# Дорожная карта реализации rev 2.2 / M2: playable-first и 3 параллельных трека

Статус: **рабочий план, не норма** (снимок 2026-11-06, 22:47 GMT+6).
Владельцы областей (`docs/README.md`) сохраняют приоритет: `VERIFICATION.md`
(V/G), `GUEST_ABI.md` (ABI), `EXACTNESS.md` (E), `FILE_LAYOUT.md` (R/M),
`HLE_MODULES.md` (H), `GRAPHICS_DECISION.md` (GB), `BUILD_PIPELINE.md` (BP),
`PERFORMANCE_CONTRACT.md` (PM), `THREAD_AUDIT.md` (TA), `ADR.md`.
Этот документ **не вводит** новых `G-xx`, ADR, R/PM/TA-id и не отменяет
`PLAN_REFACTOR_1.27B.md` (Step −1…9): он накладывает на него схему владения
файлами и порядок распараллеливания. ADR-029 (game-first) действует: красный
или `missing` гейт — отчёт, а не запрет работы, но **`unavailable ≠ pass`**
и «не измерено» ≠ «доказано» (V1.2, GB-7).

Состояние снято запуском команд, а не цитированием документов; команды и
вердикты — в §1. Все числа обязаны пересчитываться из `reports/generated/` (F2).

**Структура плана (хаб + три владельца).** Этот документ — стратегия, срез,
критический путь, 3 трека и волны W0–W4. Детализация разнесена по R4/R22
(каждый `docs/*.md` ≤400 строк) и читается по уровням:
`ROADMAP_WORKPACKAGES.md` (WP-0…WP-12 — WBS, пакеты W0.1…W0.10, реестр M5,
DUT-0…5, протокол S*, риски) → `ROADMAP_EXECUTION.md` (EX-0…EX-14 — DAG,
кроссуоки Step/M, тест-матрицы, примеры JSON, CI, валидатор) →
`ROADMAP_ASSURANCE.md` (AS-0…AS-19 — полнота норм, `in_flight`, CCR, откаты,
каскад хэшей, вердикты, анти-паттерны, границы). Приоритет: нормативные
владельцы → этот хаб → WP → EX → AS.

---

## 1. Измеренное состояние на 2026-11-06

### 1.1. Что уже есть (сильные стороны)

| Область | Факт | Источник |
|---|---|---|
| Норма | Хаб rev 2.2/M2 (379 стр.), 8 владельцев областей, ADR-001…029, R1–R22, M1–M8 | `docs/README.md`, `docs/ADR.md`, `config/migration.json` |
| Миграция | M1, M2, M8 = `done`; M5, M7 = `in_progress`; M3, M4, M6 = `pending`; R19/R22 исполняются | `config/migration.json` |
| Инструментарий M8 | 26 compiled C-бинарников, 50 пинящихся файлов, полный инвентарь `war3.exe`: 12 раундов, 9196 определений, 14 546 константных целей, **0 нарушений**, `verdict: pass` | `PROGRESS.md`, `reports/generated/closure_pipeline.json` |
| Идентичность корпуса | `corpus_sha256 = 37e4b45d…3038`, 7 входов, 4 перебазированных PE, `status: generated`, `lift_rev: 1`; `check_layout.py --require-corpus` → OK (20 регионов) | `config/layout.1.27b.json`, `reports/generated/gate_status.json` |
| Runtime (target) | 15 интерфейсов `Wc3Platform` в `platform.h`; `cpu/`, `guest/` (memory, pe, modules), `hle/abi`, `ipc/mailbox*`, `app/guest_runner`, `app/asset_check`, `boot/` | дерево `runtime/` |
| Шов M5 | `QUARANTINE.md` на 12 файлов (`expires_at 2027-06-30`), `runtime/hle/legacy_shim.c` — единственная дверь в карантин, `wc3_legacy_hle_override()`; первый модуль `runtime/hle/win32/kernel32/time/time_hle.c` и реестр модулей `runtime/guest/guest_modules.*` уже работают | `cmake/Wc3LegacyBoot.cmake`, `tests/hle_time_test.c` |
| Продукт | `libwc3gameactivity.so` линкуется как **переходный** граф: корпус `.o`, `libdxvk*_iso.a`, `dxvk_entry.cpp`, `libsoftfloat.a` из пинящегося старого дерева; скрипты старого дерева при сборке не исполняются | `CMakeLists.txt`, `cmake/Wc3LegacyBoot.cmake` |
| G-15 | Прототип `tools/lift/check_equivalence.py` (self-test pass, статус `provisional`) | `PROGRESS.md` |

### 1.2. Чего нет (честно, без «пропущено»)

Отсутствуют: `tools/wc3build.py`, `tools/difftest.py`, `tools/check_manifest.py`,
`tools/lift_equivalence.py`, `tools/gen_d3d9_thin_bridge.py`,
`tools/perf_report.py`, `tools/check_deps.py`; `apps/wc3_headless/`,
`runtime/platform/fake/`, `runtime/gfx/`, `runtime/diag/`,
`runtime/guest/generated/`; `config/file_owners.json`,
`config/dep_matrix.json`, `config/import_policy.1.27b.json`,
`config/generated/`; `oracle/identity.json`, `oracle/normalization.json`
(каталог `oracle/` **пуст**). Это ровно G-08/G-09/G-12/G-13, R16/R18/R21, M3/M4.
**Уточнение снимка 4 (23:26, `15f6b5c`):** `runtime/platform/headless/` и
`config/schema/` появились и из этого списка исключены; актуальный список
всегда выводится `check_roadmap.py`/`check_file_layout.py`, а не из прозы (AS-17).

### 1.3. Вердикты, снятые в этой сессии

**Четыре замера, дерево менялось между ними.** 22:47 → 23:06
(`HEAD 6055be0`, M5 `kernel32/time` + shim) → 23:15 (тот же HEAD,
`kernel32/process`/`memory`, headless, R3 от `.o` в корне) → 23:34
(`HEAD 15f6b5c`, коммит «Add module registry, corpus binding, and headless
platform»). Это **прямое наблюдение риска B2/B7/R14**, а не гипотеза: план
обязан пинить `head_sha`/`status_digest` (WP-1), требовать `single_writer`
на W0, считать числа во время прогона (EX-0.1/EX-12) и перебазелайнивать
только строки-снимки (AS-17). Таблица ниже — замеры 1–3 как история;
снимок 4 добавлен отдельной строкой. Вердикты, кроме состава тестов и
временной ошибки R3, стабильны.

| Команда | Результат |
|---|---|
| `python tools/run_tests.py` | **`FAILED: file_layout, platform_contract`**; 125 строк ошибок `--concurrency`/`--release` из `runtime/legacy/*` («unowned host lock», `#ifdef WC3_DEBUG`); `thread audit: unavailable (6 rows, not pass)` |
| `python tools/check_file_layout.py --strict` | **FAIL**: 2 ошибки R4 (`runtime/legacy/hle_android.c` 10 356 строк, `runtime/platform/android/platform_android.c` 2040), 10 warnings (R14, R10 `config/schema/`, R10 `import_policy` — **404 unresolved**, R5, R12 ×6) |
| `python tools/hle_coverage.py --corpus 1.27b --fixture-kind pe --require-hle` | **FAIL**: 404 unresolved + 50 forbidden (было 832 в V2/M3 — идёт незавершённая классификация) |
| `python tools/check_arch.py --policy --renderer` | `not implemented yet: renderer/policy check` (G-02 = `declared`, честно падает) |
| `python tools/check_docs.py --strict` | **OK**, 0 warnings |
| `python tools/check_layout.py --corpus 1.27b --require-corpus` | **OK**, 20 регионов |
| `python tools/smoke.py verify --baseline config/baseline.1.27b.before.json` | **5 sha256 drift**: `config/imports.*`, `config/pe_imports.*`, `ARCHITECTURE`, `THREAD_AUDIT`, `runtime/platform/platform.h` |
| `python tools/run_tests.py` (снимок 2) | те же **`FAILED: file_layout, platform_contract`**, но набор тестов вырос (`corpus_binding`, `hle_process`); `thread audit: unavailable` |
| `git status --short -- .` (снимок 2) | **18 изменённых + 9 новых файлов**, `HEAD 6055be0`; снимок 1 давал 13 M + 5 ?? (M5/M3-волна: `kernel32/{time,process}`, `guest_modules.*`, `corpus_binding_test.c`, `module_registry_test.c`, `closure2.c`, правки `import_fixture.py`/`pe_imports.py`/CMake/`build_tools.ps1`) |
| `check_file_layout.py --strict` (снимок 3, 23:15+) | **третья ошибка R3**: 7 `.o` (`headless_*.o`) в корне от параллельной сборки; продукты обязаны лежать только в `build/` (R2/R3). Устранена писателем к 23:23 (AS-17) |
| снимок 4 (23:34, `HEAD 15f6b5c`) | `check_file_layout.py --strict`: **2 ошибки R4 + 9 warnings** (R10 `config/schema/` закрыт, R3 нет); coverage 404/50; `check_docs.py --strict` OK; `check_roadmap.py --strict` → **`unavailable`** (exit 2, 0 errors/warnings, 7 отсутствующих W0-артефактов). Последующие коммиты и текущий `HEAD` — только из `check_roadmap.json` (AS-17.1); подробности/реконсиляция — AS-2/AS-17 |

### 1.4. Расхождения норма ↔ факт (должны быть закрыты, но не блокируют)

1. `docs/VERIFICATION.md` V2, `ARCHITECTURE` §5.2 и `BUILD_PIPELINE` BP-6.3
   описывают `corpus_manifest.sha256 = null` и G-05 = `declared`, хотя хэш
   запечатан и `gate_status.json` содержит evidence с `exit_code: 0`.
   Статус в V2 не повышался (V8 требует evidence + reviewer + V1.7).
2. `PROGRESS.md` сообщает M8 `done` и зелёное замыкание; нормативные `docs/*`
   не обновлены (явно зафиксировано как «не правились по требованию владельца»).
3. `config/migration.json` M5 = `in_progress`, но `evidence: null`; H6.1/H12
   и часть V2/V7/V10/`PLAN` содержат блокирующие формулировки, отменённые
   ADR-029 (долг B5). **H6.3 как проектное требование не отменён**: shim есть,
   карты «один владелец на глобал» (TA-1) ещё нет.
4. Baseline `config/baseline.1.27b.before.json` рассинхронизирован с
   перегенерированными фикстурами и документами (`smoke.py verify` → 5 drift).
5. `AS_BUILT.md` §59–89 фиксирует bootstrap-снимок target-дерева без
   corpus/HLE/dispatcher, тогда как переходный link graph уже линкует корпус и
   as-built HLE. Снимок — история, но он не должен читаться как текущий статус.
6. `config/migration.json` M5 = `in_progress` с `evidence: null`, хотя
   `kernel32/time` уже закоммичен (`6055be0`), а `kernel32/process` находится
   в работе. Ledger R19 отстаёт от дерева; закрывается в W0.1/WP-2.

---

## 2. Критический путь и стратегия

**Стратегия: не переписывать с нуля.** `AS_BUILT.md` явно запрещает архитектуру,
предполагающую новый runtime с нуля: старое дерево содержит рабочий порт
(boot → меню → 3D-сцена, 191 650 dispatcher-адресов, 426 HLE-записей).
Переходный link graph из пинящихся `.o` — это **playable-база**, которую надо
сохранить, пока новая архитектура наращивается strangler'ом. ADR-029 ставит
исполняемую игру выше полноты гейтов; точность восстанавливается через E11.

Шесть узких мест в порядке влияния на результат:

| # | Узкое место | Почему критично | Владелец трека |
|---|---|---|---|
| C1 | **Нет исполняемого T1** (G-08/G-09/G-13): `wc3_headless`, `platform/headless`, `gfx/null`, `difftest.py`, `identity.json` | M5 переносит 10 356 строк без регрессионной сети; E11 не даёт `pass` без golden E3/E4 и инвариантов V5.9 против **G** | A |
| C2 | **Нет карты владельцев** (R18) и проверки зависимостей (R16) | B2/B6/B7: параллельные правки уже ломали T0. Без `file_owners.json` распараллеливание юридически и технически невозможно | интегратор (W0) |
| C3 | M3 `import_policy` и 404 unresolved / 50 forbidden | G-03 остаётся `declared`; неизвестный обязательный импорт = fatal at startup (ADR-008) | B |
| C4 | **BP-4.4 не выполнен**: продукт линкует `.o`/DXVK/softfloat из старого дерева | «Новое дерево — канонический target» остаётся декларацией; cutover не имеет измеримого конца | C + A |
| C5 | Дрейф статусов/baseline (§1.4) | V1.7, V3.4, F2: следующий инцидент «числа из текста» | интегратор |
| C6 | **Конкурентный писатель в дереве** (наблюдено 22:47→23:06: +1 коммит, +5 файлов) | Снимок плана устаревает за минуты; W0.1 «закоммитить волну» неисполним без пина `head_sha` и правила «один писатель» (R14/B7) | интегратор (W0) |

Игра-first не отменяет точности: **T1 на `gfx/null` ничего не говорит о DXVK**
(GB-7), а зелёный гейт против baseline, снятого до исправления, закрепляет
устаревшее поведение (V6.6). Поэтому A строит не только харнес, но и три
инварианта V5.9 на эталоне из O/G (владелец Step −1 в V10).

---

## 3. Модель параллелизма: ровно 3 трека

### 3.1. Почему 3, а не 4

Существуют ровно три непересекающихся **link graph / области владения**:
верификационный DUT (A), общее ядро HLE (B), Android-продукт (C). Четвёртый
исполнитель неизбежно потребовал бы правки одного из замороженных контрактов
(`platform.h`, `CPU`, `CMakeLists.txt`, `docs/*`, `config/layout.*`) или
разделил бы один link graph. При R14 (дерево живёт в родительском `.git`, нет
своего) параллельная работа требует **отдельных worktree/копий** и патч-серий;
конкурентная правка одного файла запрещена.

### 3.2. W0 — обязательные предусловия (только интегратор, последовательно)

Ни один трек не стартует до S0:

1. **Зафиксировать границу снимка**: `head_sha` (на 23:06 — `6055be0`) и
   `snapshot_at` в `roadmap_status.json`; договориться об **одном писателе**
   на время W0 (C6, R14). Закоммитить волну (18 M + 9 ?? на снимке 2) с
   T0-отчётом и обновить `config/migration.json` M5 evidence. Разобрать
   5 `sha256 drift`: либо перезаписать `smoke.py record` отдельным коммитом
   с обоснованием (V3.4), либо откатить перегенерацию фикстур. Не оставлять
   рассинхронизированный baseline как «и так сойдёт».
2. **`config/file_owners.json` (R18)** — ровно один владелец на нормативный
   документ, слой `runtime/*`, модуль `apps/*`, группу `tools/*` и на каждый
   существующий плоский `tests/*.c`; новый файл без записи → error.
3. **`config/dep_matrix.json` + `tools/check_deps.py --strict` (R16)** —
   граф `#include` по §4.3; ребро вверх, `hle/abi` → OS-заголовок,
   `cpu` → HLE/platform = error. Чек добавляется в T0.
4. **Карантин без ослабления гейтов (V1.7).** `--concurrency`/`--release`
   получают **именной allowlist с владельцем и `expires_at`** из
   `QUARANTINE.md` (R15), а не снятие проверки; `platform_android.c`
   (2040 строк) разделяется на `<2000`-файлы, а не «поднятием лимита».
   Ожидаемый результат W0: `run_tests.py` без `FAILED` по этим двум причинам.
5. **Заморозить контракты** (список в §3.3) в
   `config/roadmap_contracts.json` с `{path, sha256, owner, frozen_at,
   change_policy}`; хэши в прозе не публикуются (урок 5 drift `smoke.py`).
   Изменение — только коммитом интегратора в точке S*, с пересчётом
   `corpus_sha256` при изменении guest ABI (ABI-9). Схема и чек — WP-1/WP-2.
6. **Протокол evidence**: треки пишут только
   `reports/generated/roadmap/track_{A,B,C}.json` по схеме V1.6 плюс
   `{track, wave, files_touched[], contract_sha256}`; состояние плана —
   `config/roadmap_status.json` (не дублирует R19). Статусы V2/V10,
   `config/migration.json`, `docs/README.md` и `PROGRESS.md` правит
   **только интегратор** по V8 (одним коммитом с кодом, V1.7).
7. **Точки синхронизации S0…S4**: слияние патчей в порядке A → B → C,
   прогон V7 (`report-only`), проверка неизменности `corpus_sha256` и
   `registry_sha256`, публикация сводного `gate_status.json`. Полный
   протокол (entry/exit, rollback, отстающий трек, worktree при R14) — WP-8.
   Исполнимая разбивка этого списка — 10 пакетов W0.1…W0.10 (WP-2),
   включая `check_roadmap.py` (EX-12) и `check_manifest.py` (EX-6).

### 3.3. Замороженные контракты (никто из треков не трогает)

`runtime/cpu/cpu.h` и `x87.h`; `runtime/guest/guest_layout.h`,
`guest_memory.*`, `pe.*`; `runtime/platform/platform.h|platform.c` (15 API);
`runtime/hle/abi/hle_abi.h` (расширение `result_kind`/`arg_source` ABI-8 —
отдельным коммитом интегратора в S1); `runtime/hle/legacy_shim.h`;
`runtime/boot/**`, `runtime/app/**`; `CMakeLists.txt`,
`cmake/Wc3LegacyBoot.cmake`; `config/layout.1.27b.json`,
`config/corpus.1.27b.json`, `config/migration.json`, `config/baseline.*`;
`docs/**`, `AGENTS.md`, `README.md`, `PROGRESS.md`; `tools/lift/**` (M8 пин).
Каждый трек получает свой фрагмент `cmake/Track{A,B,C}.cmake`, который
интегратор подключает в S*; `CMakeLists.txt` остаётся у одного владельца.

### 3.4. Карта владения треками

| Трек | Владеет (пишет) | Не трогает | Главный выход |
|---|---|---|---|
| **A — Verification / Build spine** | `tools/**` кроме `tools/lift/**` и `tools/device_test.py`; `apps/wc3_headless/**`; `runtime/platform/headless/**`, `runtime/platform/fake/**`; `runtime/gfx/null/**`; `oracle/**`; `config/schema/**`, `config/generated/**`, `config/scenario.perf.json`; `tests/unit|golden|static/**`; `cmake/TrackA.cmake`; `reports/generated/roadmap/track_A.json` | `runtime/hle/**` (кроме `gfx/null`), `runtime/platform/android/**`, `apps/android_gameactivity/**`, docs, `CPU` | G-08/G-09/G-13/G-02/G-10, PM-1…PM-8, `wc3build.py` (`android`+`headless`) |
| **B — HLE / strangler core** | `runtime/hle/win32/**`, `runtime/hle/miles/**`, `runtime/hle/abi/*.c`, `runtime/hle/legacy_shim.c`; `runtime/legacy/**` (read-only, только `QUARANTINE.md` и удаление после гейта); `runtime/guest/guest_modules.*`; `runtime/ipc/**`; `config/import_policy.1.27b.json`, `config/hle_arity.*`, `config/cdecl_allowlist.*`, `config/hle_table.*`; `tests/hle_*`, `tests/module_registry_test.c`, `tests/mailbox_set_test.c`, `tests/fake_platform.*`; `cmake/TrackB.cmake`; `reports/generated/roadmap/track_B.json` | Frozen ABI, platform.h, Android/gfx, `tools/**`, docs | M3, M5 по H6.2, G-03, G-01 L2/TA-1…TA-18, `registry_sha256` |
| **C — Android product / graphics** | `apps/android_gameactivity/**`; `runtime/platform/android/**`; `runtime/gfx/**` кроме `null/`; `runtime/hle/d3d9/**`, `runtime/hle/dsound/**`, `runtime/hle/display/**`; `runtime/diag/**`; `third_party/**` (VENDOR_MANIFEST/LICENSE); `tools/device_test.py`; `tests/platform_android_test.c`, `tests/device/**`; `cmake/TrackC.cmake`; `reports/generated/roadmap/track_C.json` | `tools/**` (оркестратор), headless/gfx/null, `runtime/hle/win32/**`, docs, `CPU` | R17, thin bridge, G-12, GB-4/GB-5, BP-4.1 vendoring, T2 |
| **Интегратор (вне треков, последовательно)** | всё из §3.3, `config/file_owners.json`, `config/dep_matrix.json`, сводный `gate_status.json`, точки S0…S4 | — | V7, V8, V1.7, BP-4.4, V9 |

### 3.5. Матрица конфликтов и разрешение

| Конфликт | Разрешение |
|---|---|
| A строит `wc3_headless` из того же HLE, что меняет B | `platform.h` заморожен; B меняет только тела модулей и таблицы; общий `registry_sha256` пересчитывается интегратором в S* (V5.7) |
| A включает `--require-hle` (G-03), зависит от классификации B | A готовит флаг в W1, включает его в каноническом прогоне только после S2, когда B закроет 404 unresolved |
| A пишет `wc3build.py`, C вендорит android-скрипты | A вызывает пути/хэши из `config/build_upstream.json` (BP-4.2), C кладёт скрипты с `VENDOR_COMMIT`; стык проверяется в S3 |
| A нужен `--renderer` по link graph, C ещё не перенёс thin bridge | G-02 остаётся `declared` до S3; отчёт честный, `unavailable ≠ pass` |
| B и C оба пишут HLE-модули | Граница по H5: `win32/*` → B, `d3d9/dsound/display` → C; общий `hle_abi.h` frozen |
| Все хотят править V2/V10/migration | Запрещено: треки публикуют evidence, интегратор меняет статусы по V8 |
| Нужны счётчики `WC3_PERF` в горячем пути | G-10 **не параллелится в W1**: A готовит инструменты и спецификацию в `tools/`, C — `runtime/diag/wc3_perf.h`, интегратор добавляет точки включения в frozen-файлы в W3 (PM-1/PM-3) |
| Один родительский `.git` (R14) | Раздельные worktree/копии + патч-серии, слияние только в S*; запрет concurrent edit |

### 3.6. Правила игры для треков (обязательные)

1. Один модуль = один коммит, на границе адаптер, после каждого — T0 и
   (когда доступен) T1 (H6.1/V6.1). Старый файл удаляется только после
   прохождения новым того же гейта.
2. Не ослаблять гейты и не поднимать пороги (V1.7): исправляется код,
   а не `--allow-unavailable`, R4-лимит или allowlist без владельца/срока.
3. Не менять guest ABI: переименование/типы полей `CPU`, сигнатуры
   `dispatch*`, thunk-окно, формулу `stack_cleanup`, базы образов → перелифт
   и новый `corpus_sha256` (ABI-1/ABI-2/ABI-9). `packed_eflags` (P1-17) —
   класс C по PM-10, не LiftPass.
4. Вердикт T1 по E11: `pass` только при E3/E4 golden и V5.9 против **G**;
   в остальных случаях `fail`/`unavailable`. Харнес обязан ловить
   намеренную порчу потока команд (V5.4).
5. Headless и APK — непересекающиеся link graph (V4.7.5, BP-7.2);
   `--target windows` запрещён; `gfx/null` не участник матрицы A/B/C/D (GB-8).
6. ADR-004/007 = `provisional` до GB-5; «уже собрано» не аргумент (GB-7);
   решение о dual — ADR с `supersedes` по GB-6, пересмотр на границе W3→W4.
7. Debug-код только под `#if WC3_DEBUG`; release hygiene на нашем коде,
   DXVK/third_party по allowlist (G-12/BP-7.4).
8. Инвариант блокировок: никакого host lock при `dispatch()`; 36 thread/sync
   записей с точной семантикой, no-op waits запрещены (ADR-008/018).
9. Каждый отчёт трека по V1.6; расхождение `corpus_sha256`/
   `registry_sha256` между S* = инцидент (V3.4, ABI-3.3).
10. Если шаг провалил чек дважды — остановиться и пересмотреть план
    (`PLAN_REFACTOR_1.27B.md` §4), а не обходить проверку.

---

## 4. Волны и точки синхронизации

```
        W0 (интегратор, последовательно) ── S0
                         │
        ┌────────────────┼────────────────┐
        A                B                C        ← до 3 параллельных исполнителей
        └────────────────┼────────────────┘
                         S1 → S2 → S3 (V7 report-only, V8 evidence)
                         │
        W4 (последовательно): G-14 → G-15 → LiftPass, M6/M7, Step 9, GB-6
```

| Волна | A | B | C | Выход (S*) |
|---|---|---|---|---|
| **W0** | — | — | — | S0: волна закоммичена; `file_owners.json`, `dep_matrix.json`, `check_deps.py` в T0; карантин allowlist с `expires_at`; `platform_android.c` <2000 строк; `run_tests.py` без `FAILED` по этим причинам; контракты заморожены с хэшами |
| **W1** | `oracle/identity.json` + `normalization.json` (V3), каркас `platform/headless` (15 API, виртуальные тики), `gfx/null` (канонический поток D3D9), `wc3build.py` каркас по BP-5, `check_manifest.py`, `config/schema/*` | M3 `import_policy` (404 unresolved / 50 forbidden → 4 состояния H4), карта владельцев глобалов TA-1/H6.3, `kernel32/memory` + `kernel32/file` по H6.2 | R17 (один `src/main`, Gradle-кэш в корневой `build/`), инвентарь thin bridge / DXVK с `VENDOR_COMMIT`, `wc3_debug.h`, числовые startup error codes (GB-4.7) | S1: T0 зелёный на трёх ветках по отдельности; G-05/`corpus_sha256` неизменны; 3 отчёта `track_*.json` |
| **W2** | Исполнимый `wc3_headless`, `tools/difftest.py` (V5.1–V5.8, самопроверка V5.4, инварианты V5.9, TA-12 расписания), `--target headless` в `wc3build.py` | `kernel32/{thread,sync}` — 36 записей, `WaitBlock` E8.1, P0-4/P0-7, `concurrency_audit.py --strict` → 0 findings; затем `kernel32/{process,locale}`, `user32/advapi32` (**`kernel32/time` уже перенесён — не повторять**, см. WP-3) | Thin bridge D3D9 → DXVK, селектор по physical device + однократный fallback, `dxvk_shim_reset()` (GB-4.3), AAudio/input, mailbox без глобального лока | S2: **первый T1 difftest** (ожидаемо `unavailable` до E3/E4 — честно); `--require-hle` включается после классификации B; 426 записей → модули |
| **W3** | G-02 `--policy`/`--renderer` по link graph, G-03 в V7, G-10/PM-1…PM-8 (`WC3_PERF`, baseline по всем модулям, отчёт V1.6), G-12 `release_hygiene_test` | Оставшиеся `win32/*`, `msvcr80`, `miles`, `registry_sha256`, удаление `hle_android.c` после T0+T1 | Вендоринг android/DXVK-скриптов (BP-4.1), `libwc3gameactivity.so` из target-дерева, GB-5.1…GB-5.7 на устройстве, T2 | S3: проверка **BP-4.4** (чистая выгрузка собирает продукт без старого дерева); при GB-6.1/6.2 — ADR с `supersedes`; G-02/G-09/G-13 → `enforced` по V8 |
| **W4** | G-14 (кросс-TU, смешанный путь, 2 guest threads, nested HLE→guest TA-13) → **G-15** (`lift_equivalence.py`, golden по всем модулям, change set) → только затем LiftPass по Step 5b | M6 (наполнение `oracle/`, подготовка разделения `cpu.h` без изменения полей `CPU` — **выполняет интегратор**, WP-3), M7 (R15–R22) | Step 9: E2E меню → карта → 60 с → save → load → exit, lifecycle, DXVK matrix | S4: V9/DoD, kill criterion GB-6.3, `git status` чистый в поддереве |

W1–W3 — три реально параллельных трека. W0 и W4 намеренно
последовательные: в W0 создаются контракты, в W4 меняется сгенерированный C
(ADR-028/ABI-11), где параллелизм создаёт непроверяемую дельту. В строке W4
ячейки A/B/C обозначают **области работ одного последовательного владельца**,
а не трёх исполнителей: сначала A закрывает G-14 → G-15, затем применяется
LiftPass, затем B/C доводят M6/M7 и Step 9. Исполнимые спецификации волн —
WP-2 (W0), WP-3 (M5), WP-4 (DUT), WP-5 (сборка), WP-6 (переходы гейтов).

---

## 5. Что намеренно НЕ распараллеливается

* **LiftPass / G-14 → G-15 → применение pass** (V6.5, ABI-11.4): меняет
  ~42 713 сайтов сгенерированного C; один владелец, один change set.
* **Перелифт / смена `CPU`, баз, thunk-окна** (ABI-9): отдельный коммит с
  новым `corpus_sha256`, до которого все треки останавливаются.
* **Нормативные документы, V2/V10, `migration.json`, baseline** (V1.7, V3.4,
  V8, R19): один интегратор, один коммит с evidence.
* **Снятие baseline из O/G и инварианты V5.9** (V6.6): один владелец Step −1.
* **GB-5 measurements на устройстве** (GB-5.1…GB-5.7): один прогон, один
  отчёт, перемешанная серия A/B (PM-19); параллельные замеры конфаундятся
  DVFS/page cache (PM-18/TA-18).
* **Канонический V7 и слияние патч-серий**: только в S*.

---

## 6. Риски параллелизма и kill criteria

| Риск | Проявление | Мера |
|---|---|---|
| Перекрёстная семантика: B меняет HLE, пока A снимает baseline | difftest ловит «регрессию», которой нет | Заморозка `registry_sha256` внутри волны; B публикует список затронутых записей в `track_B.json`; A пересчитывает baseline только в S* (V3.4) |
| Дрейф DUT от продукта | зелёный T1 при сломанном APK | Один corpus/`wc3_hle_*` для обоих target'ов (V4.7.1), `--policy` доказывает отсутствие headless/gfx-null в APK (BP-7.2), пинятся хэши DUT (V5.7) |
| Конкурентная правка в родительском `.git` (R14) | потеря коммита, `F2`-инцидент | Раздельные worktree, `file_owners.json`, слияние A→B→C только в S* |
| «Параллельно» = «одновременно правим docs» | V1.7/V8 нарушены | Запрет трекам писать `docs/**`; evidence-протокол §3.2.6 |
| Гонки за `reports/generated/*.json` | битый отчёт | Раздельные `track_{A,B,C}.json`; сводный файл — интегратор |
| Ложный `pass` T1 | E11: нет E3/E4 golden, нет маркеров G | Харнес не имеет права выдавать `pass`; `unavailable` с ненулевым exit в acceptance (V1.3/V5.6) |
| Dual DXVK объявлен рабочим без GB-5 | shipping на `provisional` ADR | GB-6: пересмотр в S3, ADR с `supersedes`, kill criterion GB-6.3 |
| Оптимизации «по статике» | PM-17/PM-19 | G-10 = `declared` до W3, результат `report-only` (V6.7) |

**Kill criteria:** (1) ни A, ни C не дают boot → menu → map на целевом
устройстве к S4 — подход пересматривается, а не дополняется WSI-патчами
(GB-6.3); (2) два последовательных провала чека на шаге — стоп и
пересмотр плана (`PLAN` §4); (3) изменение `corpus_sha256` без нового
перелифта/ADR — инцидент, треки останавливаются (ABI-9). Триггеры,
пороги, де-скопинг и лестница приоритетов — WP-10; вопросы, требующие
решения владельца (источник G, dual DXVK, replay, 404 unresolved) — WP-11.

---

## 7. Definition of Done и ближайшие действия

DoD = BP-4.4 + V9 + §10.3 хаба: чистая выгрузка собирает APK и headless-DUT,
`wc3build.py build/check` для обоих target'ов зелёные и повторный вызов no-op,
T0 без PE, T1 difftest на **H** против `identity.json` без Windows-сборки,
T2 E2E при устройстве, 36 thread/sync без stubs, release без нашего debug-кода,
G-02/G-05/G-09/G-12/G-13 → `enforced` по V8, ADR-004/007 решены по GB-6,
`git status` чистый в поддереве проекта.

**Немедленно (W0, даже если работа идёт в одиночку):**

```powershell
# 0. пиним HEAD/время и обеспечиваем одного писателя (C6, WP-1/WP-2)
git rev-parse HEAD
git status --short -- .
# 1. зафиксировать волну и разобрать drift (не «забить»)
python tools/run_tests.py
python tools/smoke.py verify --baseline config/baseline.1.27b.before.json
# 2. карта владельцев и граф зависимостей (R18/R16)
python tools/check_deps.py --strict                 # после реализации чека
# 3. карантинный allowlist с expires_at + разделение platform_android.c
python tools/check_arch.py --platform-contract --concurrency --lifecycle --release
python tools/check_file_layout.py --strict
# 4. заморозка контрактов и evidence-протокол (§3.2–3.3)
# 5. только после S0 — старт A/B/C
```

Порядок при одиночной работе: W0 целиком → A (W1) → B (W1) → C (W1) → S1 →
дальше по волнам; детальный планировщик, WIP-лимиты и шаблон пакета задания —
WP-9. При трёх исполнителях A/B/C идут одновременно, а S* и интегратор
остаются последовательными. Контрольные метрики и правила инвалидации
самого плана — WP-12.

---

## 8. Трассировка

| Норма | Где |
|---|---|
| Порядок шагов и acceptance | `PLAN_REFACTOR_1.27B.md` Step −1…9, `VERIFICATION.md` V7 |
| Статусы и перевод гейтов | `VERIFICATION.md` V1/V2/V8/V10, `ADR.md` ADR-029 |
| Граница корпуса, LiftPass | `GUEST_ABI.md` ABI-1/2/9/11 |
| Критерий точности T1 | `EXACTNESS.md` E3/E4/E11, `VERIFICATION.md` V5.9/V6.6 |
| Модули HLE и шов M5 | `HLE_MODULES.md` H4/H5/H6/H8/H11/H12 |
| Сборка и конец cutover | `BUILD_PIPELINE.md` BP-3.1/BP-4.4/BP-5.5/BP-7.2 |
| Графика, provisional, kill | `GRAPHICS_DECISION.md` GB-1/5/6/7 |
| Структура, владение, карантин | `FILE_LAYOUT.md` R15–R22, `config/migration.json` |
| Измерения и потоки | `PERFORMANCE_CONTRACT.md` PM-1…PM-24, `THREAD_AUDIT.md` TA-1…TA-18 |
| Измеренные числа | `reports/generated/*.json`, `PROGRESS.md` |
| Исполнимые рабочие пакеты | `docs/ROADMAP_WORKPACKAGES.md` WP-0…WP-12 |
| DAG, тест-матрицы, CI, валидатор плана | `docs/ROADMAP_EXECUTION.md` EX-0…EX-14 |
| Полнота норм, откаты, каскад хэшей, границы | `docs/ROADMAP_ASSURANCE.md` AS-0…AS-19 |
