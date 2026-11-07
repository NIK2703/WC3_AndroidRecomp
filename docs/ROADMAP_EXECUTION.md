# Дорожная карта: исполнимая спецификация (второй проход)

Статус: **рабочий план, не норма** (2026-11-06, 23:15 GMT+6, `HEAD 6055be0`).
Третий владелец в связке «хаб + владельцы»: `ROADMAP_PARALLEL.md`
(стратегия, срез, 3 трека, W0–W4) → `ROADMAP_WORKPACKAGES.md` (WBS,
протоколы, трассировка) → **этот документ** (DAG, кроссуоки, тест-матрицы,
примеры артефактов, CI, бинарные чек-листы, де-комиссионинг, валидатор
целостности). Разделение обязано R4/R22 (≤400 строк на `docs/*.md`).
Приоритет при расхождении: нормативные владельцы областей → хаб → WP → EX →
`ROADMAP_ASSURANCE.md` (AS-0…AS-19: полнота норм, откаты, каскад хэшей,
агрегация вердиктов). Новых `G-xx`, ADR и `R/PM/TA`-id не вводит.

---

## EX-0. Аудит второго прохода

| # | Оставшийся пробел после WP-0…WP-12 | Где закрыто |
|---|---|---|
| 1 | Нет DAG работ: кто кого блокирует, топологический порядок, критический путь | EX-1 |
| 2 | Нет кроссуока `PLAN` Step −1…9 и `config/migration.json` M1…M8 на волны/треки | EX-2 |
| 3 | WP-3 требует «`run_tests.py --test <module>`», но не задаёт тест-кейсы | EX-3 |
| 4 | WP-1 даёт схемы JSON без заполненного примера — не с чего скопировать | EX-4 |
| 5 | S* описаны прозой, нет бинарных условий входа/выхода с командами | EX-5 |
| 6 | Не отражена топология CI (BP-8: T0/T1/T2, private/public runner) | EX-6 |
| 7 | Нет дерева решений при красном гейте: идти, отчитаться или остановиться (ADR-029) | EX-7 |
| 8 | Нет политики данных `oracle/` и лицензионных входов (ADR-006) | EX-8 |
| 9 | Нет порядка вывода 12 файлов карантина (`QUARANTINE.md`) | EX-9 |
| 10 | V8 описан только для повышения статуса; понижение не регламентировано | EX-10 |
| 11 | Нет условия свёртки 3 → 2 → 1 трек | EX-11 |
| 12 | Нет валидатора целостности самого плана и проверки ссылок на id/файлы | EX-12 |
| 13 | Нет матрицы «кто пишет/кто сливает» отчёты | EX-13 |
| 14 | Числа файлов в прозе дрейфуют; нужен принцип «считать во время прогона» | EX-0.1 |

**EX-0.1. Наблюдение: план исполняется параллельно с его написанием.**
Четыре замера одного вечера: 22:47 → 23:06 (коммит `6055be0`, `kernel32/time`
+ shim) → 23:15 (появились `kernel32/process`, `kernel32/memory`,
`runtime/platform/headless/*`, `corpus_binding_test.c`, `memory_hle_test.c`,
`process_hle_test.c`), `HEAD` тот же, untracked 5 → 9 → 12 → 15; к 23:15+
параллельная сборка оставила в корне 7 `headless_*.o` (убраны к 23:23), и
`check_file_layout.py --strict` получил **третью ошибку R3** (продукты сборки
вне `build/`). T0 при этом стабилен: `FAILED: file_layout, platform_contract`;
coverage 404/50. Четвёртый замер 23:34: коммит `15f6b5c` закоммитил
headless-платформу, реестр модулей и тесты; R3 исчезла, `config/schema/`
появился. Убирать чужие активные артефакты без согласования нельзя —
W0.1 координирует писателя и только затем чистит/пинит R2-allowlist.
Следовательно, **любые счётчики в тексте — иллюстрация, а не состояние**:
источник истины = `{head_sha, snapshot_at, status_digest}` в
`roadmap_status.json` плюс `baseline_w0.json`, пересчитываемые `check_roadmap.py`
(EX-12). W0.1 обязан **поглотить** уже идущую работу (memory/process/headless),
а не начинать её заново и не конфликтовать с писателем (C6, WP-10).

---

## EX-1. DAG работ и критический путь

Оценки времени и бюджеты намеренно **не объявляются**: PM-2/F2 запрещают
пороги без пары замеров, привязанной к артефакту. DAG фиксирует только
зависимости и допустимый порядок.

| id | Работа | `depends_on` | `blocks` | Трек | Волна |
|---|---|---|---|---|---|
| W0.1 | Пин HEAD, один писатель, коммит волны, baseline | — | всё | интегр. | W0 |
| W0.2 | `file_owners.json` (R18) | W0.1 | W0.3, все треки | интегр. | W0 |
| W0.3 | `dep_matrix.json` + `check_deps.py` (R16) | W0.1 | W1 | интегр. | W0 |
| W0.4 | Карантинный allowlist, без ослабления V1.7 | W0.1 | T0-зелень | интегр. | W0 |
| W0.5 | Разделение `platform_android.c` (<2000) | W0.1 | T0-зелень | C/интегр. | W0 |
| W0.6 | Контракты, evidence, статус плана | W0.2 | W1 | интегр. | W0 |
| W0.7 | `cmake/Track{A,B,C}.cmake` + индекс | W0.2 | W1 | интегр. | W0 |
| W0.8 | `baseline_w0.json`, S0 | W0.3…W0.7 | W1 | интегр. | W0 |
| W0.9 | `check_roadmap.py` (EX-12), W0.10 `check_manifest.py` + CI-карта | W0.6 | S0 | интегр. | W0 |
| A1 | `oracle/identity.json`, `normalization.json` | S0 | A2, DUT-4 | A | W1 |
| A2 | `platform/headless` (15 API) + расписания TA-12 | S0 | A3 | A | W1 |
| A3 | `gfx/null` + инварианты V5.9 + self-test V5.4 | A2 | A4 | A | W1–W2 |
| A4 | `wc3_headless`, `--target headless`, `difftest.py` | A1, A3, B2 | DUT-5, G-09 | A | W2 |
| A5 | G-02/G-13/G-10/G-12 инструменты | A4, C2 | S3 | A | W3 |
| B1 | M3 `import_policy`, карта глобалов TA-1 | S0 | B2, G-03 | B | W1 |
| B2 | `kernel32/{memory,file}`, реестр модулей | B1 | A4 (общий HLE) | B | W1 |
| B3 | `kernel32/{thread,sync}`, 36 записей, P0-4/P0-7 | B2 | T1-гонки, S2 | B | W2 |
| B4 | Остальные `win32/*`, `msvcr80`, `miles`, `registry_sha256` | B3 | удаление legacy | B | W3 |
| B5 | M6/M7, подготовка разделения `cpu.h` | B4 | S4 | B | W4 |
| C1 | R17, инвентарь DXVK, `wc3_debug.h`, error codes | S0 | C2 | C | W1 |
| C2 | Thin bridge, селектор, `dxvk_shim_reset()` | C1 | A5 (`--policy`), BP-4.4 | C | W2 |
| C3 | Вендоринг (BP-4.1), APK из target-дерева | C2, A5 | BP-4.4 | C | W3 |
| C4 | GB-5 на устройстве, T2, Step 9 E2E | C3, S3 | S4 | C | W3–W4 |
| W4a | G-14 → G-15 → LiftPass | B4, A5 | W4b | A/интегр. | W4 |
| W4b | M6/M7, DoD, GB-6 | W4a, C4 | — | интегр. | W4 |

**Топологический порядок:** W0.1 → {W0.2…W0.7} → W0.9/W0.10 → S0 → {A1,A2,B1,C1} →
{A3,B2,C2} → {A4,B3} → S2 → {A5,B4,C3} → S3 (BP-4.4) → {W4a,C4} → W4b → S4.
**Критический путь:** W0 → A2 → A3 → A4 → S2 → A5/C3 → BP-4.4 → S3 → W4a → S4.
**Слабина:** B и C не блокируют A до S2 (общий `platform.h` заморожен);
C4 ждёт устройства и может дать `unavailable`, не задерживая S3 по ADR-029.

---

## EX-2. Кроссуок на существующие планы

| `PLAN_REFACTOR_1.27B.md` | Волна / трек | Гейт |
|---|---|---|
| Step −1 (baseline исправленного рендера из O/G, V6.6) | W1 / A (DUT-0, V5.9) | G-08 |
| Step 0 (baseline, T1-DUT, критерий точности, perf-baseline) | W0 + W1–W2 / A | G-05…G-10 |
| Step 1 (документы и политика) | **DONE** (M2/ADR-026…029) | G-07 |
| Step 2 (PE imports, arity, thread audit) | W1 / B (M3) + A | G-03, G-04 |
| Step 3 (HLE ABI, единый registry) | W0 / интегратор + B | G-06, ABI-8 |
| Step 4 (GuestRunner, platform, данные) | W0–W1 / A + C | G-06 |
| Step 5 (HLE-модули, debug, P0-4) | W1–W3 / B (WP-3/EX-3) | G-01, G-03 |
| Step 5b (LiftPass) | **W4a** (только после G-14→G-15) | G-14, G-15 |
| Step 5c (гонки hot path, TA-1…TA-18) | W2 / B3 | G-01, G-04 |
| Step 6 (UI/input/audio) | W2 / B+C (user32/winmm) | G-06 |
| Step 7 (thin bridge, dual DXVK, fallback) | W2–W3 / C2 | G-02, GB-5/GB-6 |
| Step 8 (build, CI, release hygiene) | W3 / A5+C3 | G-12, G-13 |
| Step 9 (cleanup, Android E2E) | W4b / C4 | G-11, V9 |

| `config/migration.json` | Пакет | Evidence для `done` |
|---|---|---|
| M1, M2 | закрыты | уже в ledger |
| M3 | W0.2 + B1 | `import_policy.1.27b.json`, `hle_coverage --require-hle` |
| M4 | W0.3/W0.7 + A5 | `check_deps.py`, `run_tests.py --unit`, `tools/checks/` |
| M5 | W0.1 + B2…B4 + WP-3 | `track_B.json` на каждый модуль, `registry_sha256`, T0+T1 |
| M6 | W4b | `check_file_layout --strict --warnings-as-errors` |
| M7 | W0.2…W0.7 + W4b | `check_deps.py`, R-id сплошность, owners map |
| M8 | закрыт (Rev 2.10) | `closure_pipeline.json`, `corpus.sha256` |

---

## EX-3. Тест-матрица M5 (к WP-3, один модуль = один коммит)

| Модуль | Обязательные тест-кейсы (T0, затем T1) |
|---|---|
| `abi` | `stack_cleanup` 16/4; `user` round-trip; сохранение EBX/ESI/EDI/EBP **и** x87/SSE (ABI-8); неизвестный импорт → fatal; `frozen=1` после первого `CreateThread` |
| `time` | единый `CLOCK_MONOTONIC` для QPC/GetTickCount/timeGetTime/filetime/`rdtsc`; таблица коэффициентов E3.2; E3.6 тик/время; локальное время/високосный год; `timeBeginPeriod` |
| `memory` | VirtualAlloc/Free/Protect, Heap*, guard pages, `guest_ok` по аренам, identity mapping `<4 ГБ`, запрет host-указателя в guest-структурах (E6/G4) |
| `file` | маппинг Windows-путей, 4 MPQ, save root, SAF, чтение лифтнутым `Storm.dll`, неполный набор → явная диагностика |
| `process`/`locale` | PID/TLS/`GetExitCodeThread`, app-private registry (`g_reg`), codepage, `to_wide`/`from_wide` с лимитами, `DllMain` без host lock (TA-15) |
| `thread` | create/join/detach, `pthread_attr_setstacksize(8 МиБ)`, TLS SoftFloat + `cpu_x87_init()` на каждом потоке, 24 слота без утечки `GThread.used`, приоритеты |
| `sync` | manual/auto-reset events; recursive + abandoned mutex; semaphore; `WaitForMultipleObjects(bWaitAll)`; timeout/INFINITE; `Interlocked*`; `WaitBlock` E8.1; wake-all на `Stopping` (TA-14); **0 no-op wait** |
| `user32` | очередь `g_uq`, input → Win32-сообщения, IME, clipboard, per-producer SPSC без глобального лока (P0-8/TA-5) |
| `gdi32`/`advapi32`/`winmm` | минимально необходимый GDI (GL-форвардеры не переносятся), registry, `waveOut`/`timeGetTime` → `AudioApi` |
| `msvcr80` | golden битовой точности `sqrt/pow/sin/fmod/atan2` (E4.4), `_CI*` и x87 TOP, `EDX:EAX`, скрытый указатель, `-ffast-math` запрещён |
| `miles` | late-bind sites 166/327/3344, проверка дублей/лимита, append-only до `frozen=1` (H8) |
| `d3d9`/`dsound` | 324 COM-слота через `user`, `LockRect` staging `FREE→GUEST_WRITING→GFX_READING→FREE` (TA-17), caps без «примерно», без host lock в guest-пути |
| `display` | режимы/widescreen, `--wrap` хуки, решение по `DisplayApi` (WP-11) |

Общее для всех: debug и release сборки теста (`-DWC3_DEBUG=1|0`),
`check_arch.py --concurrency --release`, отсутствие `#ifdef __ANDROID__` и
OS-заголовков в HLE core (H7/§4.3), отчёт `track_B.json` со списком
затронутых `HleEntry` и `registry_sha256`.

---

## EX-4. Примеры machine-readable артефактов (шаблоны W0)

```json
// config/roadmap_contracts.json (значения иллюстративны, хэши считает чек)
{"schema":1,"contracts_sha256":"<hash>",
 "contracts":[{"path":"runtime/platform/platform.h","sha256":"<hash>",
   "owner":"integrator","frozen_at":"2026-11-06T23:15:00+06:00",
   "change_policy":"S* only, ABI-9 relift if CPU/dispatch/layout changes"}]}
```
```json
// config/roadmap_status.json
{"schema":1,"head_sha":"6055be0","snapshot_at":"2026-11-06T23:15:00+06:00",
 "single_writer":true,"wave":"W0",
 "tracks":{"A":{"status":"pending","last_sync":null,"evidence_sha256":null},
           "B":{"status":"in_progress","last_sync":null,"evidence_sha256":null},
           "C":{"status":"pending","last_sync":null,"evidence_sha256":null}},
 "sync_points":[],"updated_at":"2026-11-06T23:15:00+06:00"}
```
```json
// reports/generated/roadmap/track_B.json (V1.6 + поля трека)
{"gate_id":"G-03","track":"B","wave":"W1","command":"python tools/hle_coverage.py --require-hle",
 "status":"fail","exit_code":1,"timestamp":"...","artifact_sha256":"...",
 "files_touched":["config/import_policy.1.27b.json"],"contract_sha256":"...",
 "commands":["python tools/run_tests.py --test hle_abi"],"reviewer":"integrator"}
```
```json
// evidence перевода статуса по V8 (пишет интегратор)
{"gate_id":"G-05","step":"W0","command":"python tools/check_layout.py --require-corpus",
 "exit_code":0,"artifact_sha256":"37e4b45d...3038","timestamp":"...","reviewer":"..."}
```

Статусы пакета: `pending → in_progress → {done|blocked}`; `done` требует
непустого `evidence_sha256` (то же правило, что R19 для M-шагов).
`blocked` обязан нести `blocked_by` и не превращается в `done` молчанием.

---

## EX-5. Бинарные чек-листы точек синхронизации

| Точка | Вход (все true) | Выход (все true) |
|---|---|---|
| S0 | W0.1…W0.10 отмечены `done`; `single_writer=true`; `head_sha` совпадает | `check_docs`, `check_deps`, `check_contracts`, `run_tests` зелёные; `baseline_w0.json` существует; V7 зафиксирован `report-only` |
| S1 | 3 отчёта `track_*.json`; diff треков внутри `file_owners.json`; контракты не дрейфуют | T0 зелёный на объединённом дереве; `corpus_sha256`/`registry_sha256` неизменны; A1/A2/B1/C1 приняты |
| S2 | A4 собран, B3 закрыл 36 thread/sync, C2 дал thin bridge | первый difftest исполнен (вердикт `pass`/`fail`/`unavailable` честно); `--require-hle` включён; TA-12 прогоны есть |
| S3 | A5/C3 закрыли G-02/G-12/G-13; отчёт BP-6.2 для обоих target'ов | **BP-4.4**: чистая выгрузка без `WC3_LEGACY_ROOT` собирает APK; V7 без ослабляющих флагов; GB-6 пересмотр зафиксирован |
| S4 | W4a: G-14 и G-15 `enforced`, change set пуст при выключенных pass'ах | V9/DoD, T2 (или честный `unavailable`), `git status` чистый в поддереве |

Команды S* (общие, без ослаблений): `check_docs.py --strict`,
`check_file_layout.py --strict`, `check_deps.py --strict`,
`check_contracts.py --strict`, `check_roadmap.py --strict`,
`check_arch.py --platform-contract --concurrency --lifecycle --release
--policy --renderer`, `check_layout.py --require-corpus`,
`hle_coverage.py --fixture-kind pe --require-hle`, `run_tests.py --unit`,
затем полный V7. При любом `false` — WP-8 rollback, точка не считается пройденной.

---

## EX-6. Топология CI (BP-8, ADR-006/013)

| Job | Runner | Что исполняет | Артефакт | Запрещено |
|---|---|---|---|---|
| T0 asset-free | любой публичный runner | docs/layout/deps/contracts/roadmap + unit-тесты, `--policy --concurrency --release` | `baseline_w0.json`, `track_*.json` | product build, лицензионный PE, корпус |
| T1 reference | **private** runner с лицензионным PE и lift | `wc3build.py build --target headless`, `difftest.py --require-oracle`, TA-12 расписания | `build.headless.json`, difftest-отчёт V5.7 | APK, Windows-бинарь, adb, публикация ассетов |
| T2 device | self-hosted Android | APK, GB-5 matrix, lifecycle, sustained-load PM-12/PM-19 | `device_test.py` отчёт, `perf.<profile>.json` | выдавать `unavailable` за `pass` |

Смешивать job'ы запрещено (BP-8.3): product build на публичном runner
нарушает ADR-006/013, difftest без DUT — V4.7. T0 идёт на каждый коммит,
T1 — на каждую точку S*, T2 — в W3/W4 при доступном устройстве.

---

## EX-7. Дерево решений при красном гейте (ADR-029)

1. Гейт `missing`/`declared`/красный → зафиксировать `{gate_id, verdict,
   artifact_sha256}` в отчёте трека; **не** выдавать `pass`.
2. Это блокирует ли шаг? По ADR-029 — **нет**, кроме трёх случаев:
   изменение guest ABI без перелифта (ABI-9), расхождение `contracts_sha256`
   (C6/WP-8), повторный провал одного чека (PLAN §4).
3. Работа продолжается, но результат помечается `report-only`, если не
   закрыты G-10 (PM-3), G-15 (V6.5), E3/E4 golden (V5.6/E11) или T2.
4. Ослаблять гейт, поднимать порог R4 или добавлять allowlist без владельца
   и `expires_at` запрещено (V1.7): исправляется код.
5. Повышение статуса — только по V8 с evidence и ревьюером, одним коммитом
   с изменением инварианта.

---

## EX-8. Политика данных и инструментов (ADR-006)

* Лицензионные PE, MPQ, DLL, сгенерированный C **не коммитятся**; входы
  пинуются хэшем в `config/corpus.1.27b.json`/`build_upstream.json`,
  продукты лежат в gitignored `build/`.
* `oracle/` хранит только эталоны: `identity.json`, `normalization.json`,
  маркеры/хэши/сценарии ввода. Захваты, содержащие данные игры, остаются
  на private runner; в репозиторий попадают хэши и нормализованные
  метаданные (V3.2/V4.2), не бинарные кадры.
* `oracle/identity.json` не содержит хэшей DUT (V3.3): пересборка
  `wc3_headless` не инвалидирует эталон; смена любого поля identity —
  отдельный коммит **до** правки кода (V3.4).
* Toolchain пинится `check_manifest.py` (`ndk/sdk/build-tools/gradle/cmake/
  python/VENDOR_COMMIT`); вendor-код — только `third_party/` с LICENSE и
  `VENDOR_COMMIT` (R9/BP-4.1).
* Все продукты сборки (`.o/.a/.so/.apk/.exe`) — только в gitignored `build/`
  (R2/R3). Наблюдённые 7 `headless_*.o` в корне — нарушение, закрываемое
  W0.1; `check_file_layout.py` обязан давать на него error.
* `--target windows` запрещён для любого артефакта (ADR-026); headless-DUT
  не входит в APK (V4.7.4/BP-7.2).
* Секреты и пути лицензионной копии не печатаются в отчётах; отчёты V1.6
  содержат только хэши и вердикты.

---

## EX-9. Де-комиссионинг карантина (`QUARANTINE.md`, 12 файлов)

Удаление файла = отдельный коммит после того, как целевой модуль прошёл
T0+T1 и строка манифеста помечена закрытой; `expires_at 2027-06-30`
обязан проверяться `check_file_layout.py` (просрочка = error).

| Файлы | `target_module` / `owner_step` | Условие вывода |
|---|---|---|
| `boot_probe.c` | boot / M3 | `runtime/boot/*` закрыл bootstrap, corpus из target-дерева |
| `host.h`, `pe_format.h`, `guest_layout.h`, `pe32.c`, `guest_mem.c` | guest / M3 | `runtime/guest/*` + layout-валидатор, `corpus_sha256` неизменен |
| `hle_android.c`, `hle_android.h`, `hle_input_state.h`, `hle_registry.h`, `hle_widescreen127.c` | hle / M5 | DoD M5 (H11): 426 записей, 36 sync, `registry_sha256`, все модули T0+T1 |
| `d3d9_tables.h` | hle-d3d9 / M5 | thin bridge + 324 COM-слота через `user`, `--policy` зелёный |

Локальный null-D3D9 фикс в `hle_android.c` (scratch-буфер `OP_LOCK_BUFFER`)
переносится в `hle/d3d9` с регрессионным инвариантом V5.9 до удаления файла
(V6.6); «исправление потеряно» = инцидент, а не допустимая дельта.
До вывода всех 12 файлов `cmake/Wc3LegacyBoot.cmake` остаётся переходным
link graph и не может считаться BP-4.4-завершённым.

---

## EX-10. Понижение статуса гейта и ADR-поток

V8: `enforced → declared` требует **ADR с явной причиной** и коммита,
одновременно правящего V2, V10 и `roadmap_status.json`; «временно ослабили
для прохождения шага» — нарушение V1.7. Повышение
`provisional → accepted` для ADR-004/007 возможно только после GB-5 и
требует ADR с `supersedes` (GB-1.3/GB-6.2). Любое решение фиксируется
`{id, status, date, owner, supersedes, evidence}` в `docs/ADR.md`; WP-11
ведёт открытые вопросы до решения, но не подменяет ADR.

---

## EX-11. Свёртка параллелизма

| Условие | Действие |
|---|---|
| Два раза подряд конфликт контрактов или rollback S* | 3 → 2 трека: A и B, C переносится в следующую волну |
| `head_sha` дрейфует при `single_writer=true` (EX-0.1) | стоп всех треков до W0.1 заново |
| Нет второго писателя / работа в одиночку | 1 трек по WP-9, DAG EX-1 сохраняет порядок |
| Любой трек `blocked` дольше одной волны | WIP-лимит не повышается, работа переносится с честным статусом |

Свёртка не меняет владение файлами и не отменяет S*: меняется только число
одновременно активных исполнителей.

---

## EX-12. Валидатор целостности плана (`check_roadmap.py --strict`)

**Реализовано** в `tools/check_roadmap.py` (pilot четвёртого прохода,
владелец — интегратор по AS-14; W0.9 остаётся точкой включения в T0).
Текущий вердикт на `HEAD 15f6b5c`: `unavailable` (exit 2, 0 errors,
0 warnings, 7 отсутствующих W0-артефактов) — честно, по V1.3/ADR-029.
Правила проверки, которым следует реализация:

1. Все четыре документа зарегистрированы в `docs/README.md`, каждая длина <380
   строк (R4/R22); `check_docs.py --strict` вызывается отдельно в V7.
2. Каждый ссылочный id семейств `WP/EX/AS/G/PM/TA/GB/BP/ABI/ADR/M/R/E/V/H/DUT`
   попадает в объявленный диапазон владельца, `W0.n` — в W0.1…W0.10,
   `Step` — в −1…9; `WP/EX/AS`-ссылки обязаны иметь секцию в документе-владельце
   (отсутствие = warning, выход за диапазон = error). Так ловятся опечатки
   вида «семейство-номер вне диапазона» без ложных срабатываний на
   сокращениях `PM-1…PM-24` и под-id `V4.7`/`GB-5.4`/`BP-5.5`.
3. `roadmap_status.json`: валидная схема, `head_sha == git rev-parse HEAD`,
   `status_digest` совпадает с текущим `git status --short -- .`,
   `single_writer` явно задан.
4. `roadmap_contracts.json`: все пути существуют, `sha256` совпадают,
   `contracts_sha256` пересчитан; дрейф = error.
5. `done` у пакета/трека без `evidence_sha256` = error (аналог R19).
6. Контрольные метрики WP-12 сверяются с `reports/generated/`; числа в прозе
   не являются источником истины (EX-0.1).
7. Отчёт по V1.6 в `reports/generated/roadmap/check_roadmap.json`;
   коды возврата `0/1/2` (BP-5.3).

---

## EX-13. Матрица владения отчётами

| Файл | Пишет | Сливает | Частота |
|---|---|---|---|
| `reports/generated/roadmap/track_{A,B,C}.json` | трек | — | каждый пакет |
| `reports/generated/roadmap/check_roadmap.json` | `check_roadmap.py` | — | каждый коммит T0 |
| `reports/generated/roadmap/baseline_w0.json` | интегратор | — | S0 и при V3.4 |
| `reports/generated/gate_status.json` | интегратор | из evidence треков | каждая S* |
| `config/roadmap_status.json`, `config/migration.json`, V2/V10 | интегратор | по V8 | каждая S* |

Конфликт слияния отчётов = стоп S*, а не «последний записал»: отчёты
регенерируются из evidence, не правятся вручную (V1.6/F2).

---

## EX-14. Трассировка второго прохода

| Вопрос | Документ |
|---|---|
| Стратегия, срез, 3 трека, W0–W4 | `ROADMAP_PARALLEL.md` §1–§8 |
| WBS, протоколы, риски, метрики | `ROADMAP_WORKPACKAGES.md` WP-0…WP-12 |
| DAG, тесты, примеры, CI, де-комиссионинг, валидатор | этот документ EX-0…EX-14 |
| Полнота норм, откаты, хэши, вердикты, границы | `ROADMAP_ASSURANCE.md` AS-0…AS-19 |
| Нормы (приоритет выше всех четырёх) | `docs/README.md` и владельцы областей |
