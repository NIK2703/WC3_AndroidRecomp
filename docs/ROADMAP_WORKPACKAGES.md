# Дорожная карта: рабочие пакеты, протоколы и трассировка

Статус: **рабочий план, не норма** (2026-11-06). Владелец детального WBS и
протоколов; нормативный хаб плана — `docs/ROADMAP_PARALLEL.md`. При
расхождении приоритет у нормативных владельцев областей (`docs/README.md`),
затем у хаба, затем у этого документа. Новых `G-xx`, ADR и `R/PM/TA`-id не
вводит. Разделение сделано по образцу M2: R4/R22 ограничивает каждый
`docs/*.md` 400 строками (≥380 — warning), поэтому глубокая детализация
вынесена сюда, а хаб остаётся ≤380 строк и держит только стратегию.
Второй проход (DAG, кроссуоки, тест-матрицы, примеры артефактов, CI,
валидатор целостности) — `docs/ROADMAP_EXECUTION.md` EX-0…EX-14; он
ниже этого документа по приоритету и ссылается сюда, а не дублирует.
Третий проход (полнота норм, `in_flight`, CCR, откаты, каскад хэшей,
агрегация вердиктов) — `docs/ROADMAP_ASSURANCE.md` AS-0…AS-19.

---

## WP-0. Аудит детализации хаба v1 (что было слабо и где закрыто)

| # | Пробел / дефект v1 | Где закрыто |
|---|---|---|
| 1 | W0 = 7 пунктов прозы; нет списка файлов, изменений чекеров и ожидаемой дельты T0 | WP-2 |
| 2 | `contract_sha256` упомянут, но не определён: нет артефакта, схемы и политики дрейфа | WP-1 |
| 3 | Evidence-протокол описан одной строкой; нет схемы отчёта трека и machine-readable состояния дорожной карты | WP-1 |
| 4 | Крупнейшая работа (10 356 строк `hle_android.c`) сведена к «`kernel32/memory` + `file`»; нет реестра модулей, диапазонов строк, владельцев глобалов и условия удаления | WP-3 |
| 5 | **Дефект**: W2 снова планирует `kernel32/time`, хотя модуль уже перенесён (`time_hle.c`, `hle_time_test.c`) | исправлено в хабе W2 + WP-3 |
| 6 | **Противоречие**: W4 отдаёт треку B «разделение `cpu.h`», хотя §3.3 замораживает этот файл за интегратором | исправлено в хабе W4 + WP-3 примечание |
| 7 | T1 описан как один шаг «первый difftest»; нет лестницы DUT, формата потока команд, self-test'ов и машины вердиктов E11 | WP-4 |
| 8 | Нет контракта `wc3build.py`: CLI, коды возврата, no-op, изоляция target'ов, процедура проверки BP-4.4 | WP-5 |
| 9 | Нет таблицы переходов `declared`/`missing` → `enforced` с evidence по V8 | WP-6 |
| 10 | PM-1…PM-24, TA-1…TA-18, GB-5.x, E3/E4, V5.9 не привязаны к волнам/трекам | WP-7 |
| 11 | «Точка синхронизации S*» — слово без протокола: нет entry/exit, порядка слияния, разрешения конфликтов, rollback, политики отстающего трека, схемы worktree при R14 | WP-8 |
| 12 | Нет формата пакета задания для исполнителя и расписания для одиночной работы | WP-9 |
| 13 | Риски без триггеров/порогов и без лестницы де-скопинга на случай «нет устройства / нет захватов O/G» | WP-10 |
| 14 | Нет списка открытых решений с владельцем и дедлайном (dual DXVK, источник G, replay, 404 unresolved) | WP-11 |
| 15 | Нет контрольных метрик as-built с источником пересчёта и правил поддержки самого плана | WP-12 |
| 16 | **Снимок не привязан к `HEAD`/времени**: между 22:47 и 23:06 в дереве появились коммит `6055be0` и 5 новых файлов; W0.1 «закоммитить волну» мог опереться на устаревшие числа | WP-1, WP-2, WP-10, WP-12; хаб §1.3/C6 |
| 17 | Второй проход нашёл ещё 14 пробелов (DAG, кроссуоки Step/M, тест-кейсы, примеры JSON, CI, де-комиссионинг, валидатор плана) | `ROADMAP_EXECUTION.md` EX-0…EX-14 |

Итог аудита: хаб был корректен стратегически, но **неисполним как задание** —
не хватало уровня «какой файл, какой чек, какой evidence, кто принимает».

---

## WP-1. Машиночитаемое состояние: контракты, evidence, статус

Три новых артефакта (создаются в W0, не являются отчётами):

| Артефакт | Схема | Владелец | Правило |
|---|---|---|---|
| `config/roadmap_contracts.json` | `{schema, contracts:[{path, sha256, owner, frozen_at, change_policy}], contracts_sha256}` | интегратор | Пинит все файлы §3.3 хаба. `contracts_sha256` = sha256 строк `"<path>:<sha256>"`, отсортированных по `path` и соединённых `\n`. Пересчитывается **только** в S*; расхождение = инцидент. Хэши в прозе не публикуются (F2, 5 drift `smoke.py`) |
| `config/roadmap_status.json` | `{schema, head_sha, snapshot_at, status_digest, single_writer, wave, tracks:{A,B,C}:{status,last_sync,evidence_sha256}, sync_points:[{id,at,head_sha,v7_verdict,contracts_sha256}], updated_at}` | интегратор | `status_digest` = sha256 отсортированных строк `git status --short -- .`; `head_sha` = `git rev-parse --short HEAD`. Оба поля проверяет `check_roadmap.py` (EX-12), расхождение = стоп треков. Состояние **плана**, не миграции (R19), не заменяет V2 |
| `reports/generated/roadmap/track_{A,B,C}.json` | V1.6 + `{track, wave, files_touched[], contract_sha256, commands[], artifact_sha256[], verdict, reviewer}` | трек | Gitignored, регенерируется; трек не пишет в общий `gate_status.json` |

Команды W0 (включаются в T0): `check_contracts.py --strict` (новый чек в
`tools/checks/` после M4; до M4 — в flat `tools/`), сверка с
`check_file_layout.py` по R18 и с `check_deps.py` по R16, а также сверка
`head_sha` из `roadmap_status.json` с `git rev-parse HEAD`: расхождение =
«дерево изменилось», все треки останавливаются до нового снимка. Перевод статуса
гейта — только по V8: `{gate_id, step, command, exit_code, artifact_sha256,
timestamp, reviewer}`; в `roadmap_status.json` фиксируется ссылка на это
evidence, а не сам факт «сделано».

---

## WP-2. W0: десять рабочих пакетов (последовательно, интегратор)

| Пакет | Файлы / изменение | Гейт и ожидаемая дельта T0 |
|---|---|---|
| W0.1 | зафиксировать `head_sha`/`snapshot_at` и правило одного писателя (C6); **согласовать** с активным писателем in-flight модули (`kernel32/memory`, `kernel32/process`, `platform/headless/*`) и убрать 7 `headless_*.o` из корня через R2-allowlist (не удаляя чужие активные артефакты без договорённости); закоммитить волну и обновить `config/migration.json` M5 evidence; `smoke.py record` **или** откат перегенерации фикстур отдельным коммитом с причиной (V3.4) | `smoke.py verify` → 0 drift; `git status -- .` чистый; `check_file_layout.py --strict` без ошибки R3; `head_sha` совпадает |
| W0.2 | `config/file_owners.json` + проверка в `check_file_layout.py`: новый файл в охраняемой зоне без владельца → error, дубли владельца → error (R18) | `check_file_layout.py --strict` без новых warnings R18 |
| W0.3 | `config/dep_matrix.json` + `tools/check_deps.py --strict`: рёбра §4.3, запрет `hle/abi` → OS-заголовок и `cpu` → HLE/platform; новый вызов в `run_tests.py` | T0-тест `deps` зелёный; отчёт по V1.6 |
| W0.4 | `--concurrency`/`--release` в `check_arch.py`: именной allowlist из `QUARANTINE.md` с `{file, owner, expires_at}`; просроченная запись = error. **Проверка не снимается** (V1.7) | `platform_contract` перестаёт выдавать 125 ошибок из карантина; намеренно испорченный allowlist даёт `fail` (V5.4 по аналогии) |
| W0.5 | разделить `runtime/platform/android/platform_android.c` (2040 строк) на `<2000`-файлы по R13, без смены поведения; `runtime/legacy/hle_android.c` остаётся в карантине с записью R15 | `check_file_layout.py --strict`: R4-ошибки = 0, warnings не выросли |
| W0.6 | `config/roadmap_contracts.json`, `config/roadmap_status.json` (с `head_sha`, `snapshot_at`, `single_writer`), `tools/check_contracts.py`, каталог `reports/generated/roadmap/` | `check_contracts.py --strict` зелёный; хэши и HEAD зафиксированы |
| W0.7 | `cmake/Track{A,B,C}.cmake` — пустые фрагменты, подключённые из `CMakeLists.txt` одним коммитом; `docs/README.md` индекс | CMake configure без изменений link graph; `check_docs.py --strict` OK |
| W0.8 | пересчитать контрольные метрики WP-12 на новом `head_sha`, опубликовать `reports/generated/roadmap/baseline_w0.json` | S0: `run_tests.py` без `FAILED`, V7 прогон `report-only` зафиксирован |
| W0.9 | `tools/check_roadmap.py --strict` **реализован как pilot четвёртого прохода** (владелец — интегратор, AS-14): диапазоны id, секции WP/EX/AS, `head_sha`/`status_digest`, длины четырёх документов, индекс, evidence у `done` | сейчас честно `unavailable` (exit 2, 7 отсутствующих W0-артефактов, 0 errors/warnings); цель W0 — `pass`. Отчёт `reports/generated/roadmap/check_roadmap.json` по V1.6 |
| W0.10 | `tools/check_manifest.py` — пин toolchain (`ndk/sdk/build-tools/gradle/cmake/python/VENDOR_COMMIT`), карта CI-job'ов EX-6 | `check_manifest.py` и T0-тест `toolchain` зелёные |

**S0 exit:** зелёные `check_docs`, `check_deps`, `check_contracts`,
`check_roadmap`, `run_tests`; красные/недоступные гейты честно перечислены
в `baseline_w0.json` (ADR-029).

---

## WP-3. M5: реестр модулей `hle_android.c` (главный объём работ)

Диапазоны строк — из `docs/HLE_MODULES.md` H5 (as-built, 10 356 строк).
Порядок — H6.2. Один модуль = один коммит (H6.1); удаление
`runtime/legacy/hle_android.c` — только после DoD M5 (H11: все 426 записей
отнесены к модулю, 36 thread/sync без stubs, `registry_sha256` зафиксирован,
каждый модуль прошёл T0 + T1).

| # | Модуль (H2) | As-build | Владелец | Волна | Ключевые глобалы / риск |
|---|---|---|---|---|---|
| 0 | `abi` + `hle_legacy_shim` | готово | интегратор/B | W0 | `wc3_legacy_hle_override`, `WC3_LEGACY_HLE_MAX=4096`, static assertions на stride |
| 1 | `kernel32/time` | 2955 timers | B | **DONE, коммит `6055be0`** | `time_hle.c` + `hle_time_test.c`; W0.1 лишь пишет evidence в `migration.json` M5 |
| 2 | `kernel32/memory` | 900–1010 handle/VA, 4511 file mapping | B | W1 | `g_handles`, `guest_ok`/`rd32g` через shim; E6 |
| 3 | `kernel32/file` | 1371 `h_create_file` | B | W1 | `g_cr_file`, маппинг путей, `AssetApi`/MPQ |
| 4 | `kernel32/process`, `locale` | 7681 registry | B | W0.1 / W2 | `process_hle.c` + `process_hle_test.c` **уже в работе, незакоммичены** — закрыть в W0.1; `g_reg`, `ConfigApi` app-private; `DllMain` по TA-15 |
| 5 | `kernel32/thread` | 4065–4119 TLS, 5482–5594 `GThread` | B | W2 | 18 `CreateThread`, `used` не освобождается, join/8 МиБ/TLS SoftFloat (TA-2) |
| 6 | `kernel32/sync` | 2096–2130 events, 2200 mutex, 2321–2820 waits, 5594–5680 `cs_get` | B | W2 | 36 записей, `WaitBlock` E8.1, P0-4/P0-7, 10 гонок |
| 7 | `user32` | 6391 `g_uq`, 6785–6954 key events | B/C | W2 | `g_uq_lock`, mailbox per-producer SPSC (P0-8/TA-5) |
| 8 | `gdi32`, `advapi32`, `winmm` | GL-форвардеры (legacy), 7681, 2912 `g_wave_dev` | B/C | W2 | gdi32 — только необходимое; winmm → `AudioApi` |
| 9 | `msvcr80` | 4740, 5893–5920 `cr_*_sse2` | B | W3 | ABI-8 `result_kind`/`arg_source`, E4.4 golden, x87 TOP |
| 10 | `miles` | 4180 `g_mods`; late-bind 166/327/3344 | B | W3 | H8 `LATE_BIND` до первого `CreateThread`, `frozen=1` |
| 11 | `d3d9` (324 COM-слота), `dsound` | 7947+ `g_d3d9_vt`/`g_d3d9_op` | C | W2–W3 | thin bridge, `user` для COM, GB-4/TA-17 staging |
| 12 | `display` | `hle_widescreen127.c`, display_mode | C | W3 | `DisplayApi` отсутствует (D1) — открытое решение WP-11 |

Приёмка каждого модуля: `run_tests.py --test <module>` + новые unit-тесты в
`tests/`, `check_arch.py --platform-contract --concurrency --release`,
`hle_coverage.py --require-hle` без роста unresolved, T1 (когда доступен),
отчёт `track_B.json` со списком затронутых `HleEntry` и новым
`registry_sha256`. **M6/разделение `cpu.h`**: не меняет поля `CPU` (иначе
перелифт по ABI-2) и выполняется интегратором в W4 со static assertion на
`sizeof(CPU)`/`offsetof` и неизменным `corpus_sha256`.

---

## WP-4. Трек A: лестница исполняемого T1 (DUT-0 → DUT-5)

| Ступень | Артефакт | Проверка готовности |
|---|---|---|
| DUT-0 | `oracle/identity.json` по V3.3 (`reference{pe_sha256[],capture_tool_sha256,captured_at}`, `scenario{seed,input_script_sha256,resolution,caps_snapshot}`, `baseline{frame_hashes[],markers[],memory_hashes_normalized[],save_hashes[],replay_hashes[]}`, `layout{sha256,lift_rev,rebased_modules[]}`) + `normalization.json` по V4.2 | G-08 → `enforced` по V8; хэши DUT в identity **не** входят (V3.3) |
| DUT-1 | `platform/headless` — все 15 `Wc3Platform` детерминированно: время = виртуальные guest-тики (E3.1), ввод = `input_script`, файлы = фиксированный каталог MPQ, `ThreadApi`/`SyncApi` = TA-12 расписания, `VulkanDriverApi` = `caps_snapshot`; запрет `getenv`/GPU/adb/сокетов | unit-тесты 15 таблиц; `schedule_sha256` в identity |
| DUT-2 | `gfx/null` — канонический поток `(method_index, canonical_args, state_keys)`, без Vulkan/DXVK/GL (V4.7.3), плюс три инварианта V5.9: `locked_range_written`, `declared_range_consistency`, `degenerate_primitive_ratio` против **G** | self-test V5.4: испорченный поток, сдвинутый байт save, изменённый маркер и пустой locked-диапазон → `fail` |
| DUT-3 | `wc3_headless` из того же corpus/HLE/app, `wc3build.py build --target headless`; `--policy` доказывает отсутствие headless/gfx-null в APK | отчёт BP-6.2 с `target: headless`; пинятся 6 хэшей V5.7 |
| DUT-4 | `tools/difftest.py`: контрольные точки V5.2, снимок `CPU` (x87 TOP/SSE), последние N dispatch-целей, byte-compare save/replay, нормализованный хэш guest memory V4.2 | прогон по всем расписаниям TA-12; E3.6/E4.6 golden → вердикт по E11 |
| DUT-5 | G-09 → `enforced`: golden E3 (таблица коэффициентов, тик/время) и E4 (`sqrt/pow/sin/fmod/atan2`, `_CI*`, битовые эталоны x86), V5.9 от O/G на **исправленной** сборке (V6.6) | `pass` разрешён только здесь; до этого `unavailable` с ненулевым exit в acceptance (V1.3) |

**Машина вердиктов (E11):** нет identity/golden/маркеров G → `unavailable`;
нарушен V5.9 при совпавших хэшах → `fail`; совпали всё + E2–E4 → `pass`.
Baseline из O/G пересчитывается отдельным коммитом **до** правки кода
(V3.4); `frame hash` не гейт до E3.2/E3.6 (E3.7).

---

## WP-5. Сборка: контракт `wc3build.py` и проверка BP-4.4

| Команда | Артефакт | Разрешено | Запрещено |
|---|---|---|---|
| `corpus --target android [--clean]` | `build/corpus-inputs/`, `corpus.sha256` | 7 пинящихся входов, 4 перебазированных PE | неявные пути старого дерева, Windows-target |
| `build --target android` | `build/android/…/libwc3gameactivity.so`, APK | NDK/Gradle, оба DXVK, thin bridge | `platform/headless`, `gfx/null`, DXVK вне `third_party/` |
| `build --target headless` | `build/headless/wc3_headless` | `platform/headless`, `gfx/null`, тот же corpus/HLE | APK, Vulkan instance/device, adb, Windows |
| `check --target … [--json]` | отчёт BP-6.2 (`artifact, sha256, size, flags, toolchain, corpus_sha256, target`) | link graph по факту | grep по исходникам как доказательство |

Коды возврата BP-5.3: `0` pass / `1` fail / `2` unavailable; повторный `build`
без изменений — no-op; у `link_apk.rsp` ровно один генератор и один потребитель
(BP-5.1/5.2). `check_manifest.py` пинит toolchain
`{ndk, sdk, build_tools, gradle, cmake, python, VENDOR_COMMIT}`.
**BP-4.4 проверяется в S3**: чистая выгрузка (без `WC3_LEGACY_ROOT`),
`build --target android` без обращения к старому дереву, зелёные T0+T1,
отчёт воспроизводим для обоих target'ов; только после этого cutover считается
завершённым и `cmake/Wc3LegacyBoot.cmake` выводится из link graph.

---

## WP-6. Переходы гейтов (V8 evidence, без повышения «для галочки»)

| Гейт | Сейчас | Цель | Волна | Владелец | Evidence |
|---|---|---|---|---|---|
| G-01 | `enforced` (L0/L1), L2 = `missing` | L2 через summary TA-16 | W4 | A+B | `summary.sha256`, граф до `dispatch()`, debug assertion |
| G-02 | `declared` | `enforced` | W3 | A+C | `--policy`/`--renderer` по link graph, вызов в V7 |
| G-03 | `declared` | `enforced` | W2→S2 | B+A | M3 `import_policy`, `--require-hle`, 0 unresolved |
| G-04 | `declared` | `enforced` | W3/T2 | B | 6 строк `unavailable` сняты, TA-2/TA-14, без `--allow-unavailable` |
| G-05 | `declared` в V2, evidence = `exit 0` | `enforced` по V8 | W0 | интегратор | `gate_status.json` + V2/§5.2/BP-6.3 правятся одним коммитом (V1.7) |
| G-06/G-07 | `enforced` | сохранить | все | все | без ослаблений |
| G-08 | `missing` | `enforced` | W1 | A | `oracle/identity.json` V3.3 |
| G-09 | `missing` | `enforced` | W2→DUT-5 | A | difftest + golden E3/E4 + V5.9 + TA-12 |
| G-10 | `declared` | `enforced` | W3 | A+C | PM-1/2/4/8/19/21, `WC3_PERF`, `scenario.perf.json`, ABBA |
| G-11 | `declared` | по устройству | W3/T2 | C | `device_test.py --require-device`, `unavailable ≠ pass` |
| G-12 | `missing` | `enforced` | W3 | A+C | `release_hygiene_test` по preprocessed output и бинарнику |
| G-13 | `missing` | `enforced` | W1→W3 | A+C | `wc3build.py` оба target'а, `check_manifest.py`, thin bridge генератор |
| G-14 | `missing` | `enforced` | W4 | A | кросс-TU ≥2, смешанный путь, 2 guest threads, nested HLE→guest (TA-13) |
| G-15 | `missing` (`provisional` прототип) | `enforced` | W4 | A | golden по каждому модулю, нормализованные тела, пустой change set при выключенных pass'ах |

---

## WP-7. Трассировка измерительных и потоковых контрактов

| Группа id | Волна / трек | Артефакт |
|---|---|---|
| PM-1…PM-8 (счётчики, бюджет, G-10 prerequisite, все 18 модулей, DAG, принятие, потоки, отчёт) | W3 / A + C + интегратор | `config/perf.baseline.json`, `reports/generated/perf.<profile>.json` |
| PM-9 (честные статусы), PM-10 (классы A–F), PM-11 (калибровка), PM-16 (идентичность LiftPass) | W3–W4 / A | отчёт V1.6, классификация перед каждым pass'ом |
| PM-12, PM-13, PM-15, PM-18, PM-20, PM-21 (T2 sustained-load, профиль, identity прогона, атрибуция, фазы, нагрузочный сценарий) | W3 / C + A | `config/scenario.perf.json`, phase markers |
| PM-14, PM-22, PM-23, PM-24 (профилировочная сборка, цена LTO, два уровня `-O` + UBSan, interim-профиль) | W4 / A | `changeset.sha256`, golden на `-O0`/`-O2` |
| PM-17, PM-19 (взвешивание профилем, interleaved `A B B A`) | W3 / C | медиана парных дельт, `tolerance` по PM-11 |
| TA-1…TA-5 (владение глобалами, host-поток, L0/L1/L2, стресс E5.3, mailbox SPSC) | W1–W2 / B | `concurrency_audit.py --strict` → 0 findings |
| TA-6…TA-11 (6 `unavailable`, vendor-потоки, P0-*, приёмка стресса, jitter T2, L2 механика) | W3 / B + C | DXVK allowlist, TA-3 L2 остаётся `missing` до W4 |
| TA-12…TA-18 (расписания T1, реентерабельность, shutdown при INFINITE, DllMain, summary L2, staging-владение, cpuset/thermal) | W2 и W4 / A + B + C | `schedule_sha256`, `summary.sha256`, `wc3_guest_enter/leave` |
| E2/E3/E4/E5/E8/E11 + V5.9 | W1–W2 / A + B | identity, golden, `WaitBlock`, три инварианта |
| GB-4.3/GB-5.1…5.7/GB-6 | W2–W3 / C | `dxvk_shim_reset()`, device matrix, ABBA, ADR с `supersedes` |

---

## WP-8. Протокол точки синхронизации S*

1. **Entry**: у каждого трека `track_X.json` с зелёными своими T0-чеками,
   `files_touched` внутри зоны R18, `contract_sha256` равен W0/S*; ни одного
   незакоммиченного файла вне зоны.
2. **Заморозка**: треки прекращают писать; интегратор снимает
   `contracts_sha256`, `corpus_sha256`, `registry_sha256`.
3. **Слияние** патч-серий строго A → B → C (A меняет общие инструменты,
   B — общий HLE, C — уже поверх них). Конфликт = стоп, а не разрешение
   «на глаз»: виновный трек перебазирует свою серию.
4. **V7-подмножество**: `check_docs.py --strict`, `check_file_layout.py
   --strict`, `check_deps.py --strict`, `check_contracts.py --strict`,
   `check_arch.py --platform-contract --concurrency --lifecycle --release`,
   `check_layout.py --require-corpus`, `run_tests.py --unit`, полный V7 —
   `report-only` с перечислением красных гейтов.
5. **Статусы**: интегратор одним коммитом обновляет V2/V10,
   `config/migration.json` (R19), `docs/README.md`, `roadmap_status.json` —
   только по V8 с evidence. Повышение «просто потому, что сделано» запрещено.
6. **Exit**: `contracts_sha256` пересчитан, новые контракты согласованы,
   `baseline` не дрейфует (иначе отдельный коммит по V3.4), три трека
   получают новый пакет заданий WP-9.
7. **Rollback**: откат всей точки S* до предыдущего `roadmap_status.json`,
   а не выборочный revert файла трека; причина фиксируется в отчёте.
8. **Отстающий трек**: не задерживает S* дольше одной волны — его незакрытая
   работа переносится в следующую волну с честным статусом, но **не**
   объявляется `done`; WIP-лимит = 1 активный пакет на трек.
9. **R14**: отдельные worktree родительского репозитория (`wt-A`, `wt-B`,
   `wt-C`) или три копии дерева; запрет concurrent edit одного файла;
   слияние только в S*. Имена веток и `worktree`-корни пинятся в
   `roadmap_status.json`.

---

## WP-9. Пакет задания и одиночный режим

Шаблон выдачи задания треку (копируется в `track_X.json`):

```
{track, wave, package_id, inputs:[{path, sha256}], contract_sha256,
 own_paths:[…], forbidden_paths:[…], deliverables:[{path, gate}],
 evidence_commands:[…], acceptance:[{gate_id, verdict}], reviewer,
 rollback_to}
```

Review-чеклист интегратора: (a) все `deliverables` существуют; (b) ни один
`forbidden_path` не изменён (`check_contracts.py` + `git diff --name-only`);
(c) чек не ослаблен (V1.7); (d) отчёт по V1.6 с `artifact_sha256`;
(e) `unavailable` не выдан за `pass`; (f) ни одного `docs/**` в diff трека.

**Одиночный режим** (текущий, 1 исполнитель): W0 целиком → A/W1 → B/W1 →
C/W1 → S1 → A/W2 → B/W2 (сначала thread/sync — самый большой риск гонок) →
C/W2 → S2 → … После **каждого** пакета: `run_tests.py` + `check_docs.py
--strict`; при первом `FAILED` — остановка, а не накопление долгов. Приоритет
при нехватке времени: A/DUT → B/thread+sync → C/thin bridge → всё остальное.

---

## WP-10. Риски с триггерами, де-скопинг и kill criteria

| Триггер | Порог | Действие |
|---|---|---|
| **Конкурентный писатель** (наблюдено 22:47→23:06) | `git rev-parse HEAD` ≠ `head_sha` в `roadmap_status.json`, либо `git status` вырос между S* | стоп треков, новый снимок (§1.3/WP-12), пересчёт контрактов и baseline; W0 выполняется при `single_writer=true`, иначе параллельная работа не начинается |
| Нет эталонных захватов O/G | `oracle/identity.json` отсутствует к S2 | T1 = `unavailable`; B продолжает M5 с T0 + self-test V5.4, помечая каждый модуль «без T1»; G-08/G-09 не повышаются |
| Нет устройства | `device_test.py` → `unavailable` в S3 | T2 = `unavailable`, GB-5 остаётся `missing`, ADR-004/007 = `provisional`, ADR с `supersedes` не издаётся; по GB-6.1 вариант A по умолчанию не выбирается без измерений |
| `corpus_sha256` изменился без перелифта | любое расхождение в S* | стоп всех треков, инцидент по ABI-9, откат S* |
| `registry_sha256` разошёлся между B и A | любое расхождение | пересчёт интегратором, повтор T1 по V5.7 |
| Повторный провал чека | 2 раза подряд | стоп и пересмотр плана (`PLAN` §4), не обход флагом |
| Dual не даёт boot→map | к S4 (GB-6.3) | пересмотр подхода, а не очередной WSI-патч |
| Рост R4/новый монолит | любой `docs/*.md` ≥380 | разделение владельца до следующей ревизии (R22) |

Лестница де-скопинга (сохраняет playable): 1) продукт на переходном link
graph → 2) target-tree APK через `wc3build.py` → 3) T1 `unavailable`→`pass`
→ 4) оптимизации/LiftPass → 5) device E2E. Шаг 4 никогда не ставится выше
шага 2 (ADR-029: игра важнее полноты гейтов).

---

## WP-11. Открытые решения (владелец, дедлайн, умолчание)

| Вопрос | Владелец | Дедлайн | Умолчание при молчании |
|---|---|---|---|
| Источник захватов G без рекомпилированного Windows-хоста (V3, ADR-026) | владелец проекта | S2 | T1 = `unavailable`, работа продолжается |
| Оставлять dual DXVK (вариант C) или уйти в A по GB-6.1 | владелец проекта | S3 | сохранить `provisional`, shipping-решение не принимать |
| 404 unresolved: какие импорты `fatal-missing`, какие `optional-unused` | B + H4 | S1 | всё неклассифицированное = `fatal-missing` (ADR-008) |
| Replay-совместимость при расхождении 1 ULP (E4.4) | владелец проекта | DUT-5 | `unavailable`, не «просто bionic»; нужен явный ADR |
| `DisplayApi` для `display`/widescreen (D1) | C | W3 | минимальная реализация без расширения `Wc3Platform` (контракт заморожен) |
| Число исполнителей: 1 / 2 / 3 | владелец проекта | S0 | 1 (WP-9), схема треков остаётся валидной |
| `packed_eflags` (P1-17, класс C PM-10) | интегратор | W4 | не делать: требует перелифта |

---

## WP-12. Контрольные метрики и поддержка плана

| Метрика | Значение на 2026-11-06 | Пересчёт |
|---|---|---|
| Снимок плана | 4 замера 22:47 / 23:06 / 23:15 / 23:34 GMT+6; `HEAD 6055be0` → `15f6b5c`; `check_roadmap.py` пинит `head_sha` и `status_digest` | `python tools/check_roadmap.py --strict`, `git rev-parse HEAD` |
| Записей dispatcher | 191 650 | `tools/perf_probe.py` → `perf_baseline.static.json` |
| HLE-записей / thread-sync | 426 / 36 | `config/hle_table.1.27b.tsv`, `docs/THREAD_AUDIT.md` |
| PE-импортов / unresolved / forbidden | 2 009 / **404** / 50 | `hle_coverage.py --fixture-kind pe --require-hle` |
| Интерфейсов `Wc3Platform` | 15 (`NetApi` нет) | `runtime/platform/platform.h` |
| Входов лифта / `corpus_sha256` | 7 / `37e4b45d…3038` | `config/corpus.1.27b.json`, `corpusinput.exe verify` |
| Строк в карантине | `hle_android.c` 10 356; `platform_android.c` 2 040 | `check_file_layout.py --strict` |
| Замыкание корпуса | `verdict: pass`, 0 нарушений | `reports/generated/closure_pipeline.json` |

**Инвалидация плана**: смена любого ADR с `supersedes`, новый `corpus_sha256`,
изменение матрицы V2/V10, закрытие BP-4.4, выход R4 за 380 строк, смена
числа треков, **новый `HEAD` с незафиксированным снимком** (C6). Каждая инвалидация = правка хаба и этого документа **одним
коммитом** с обновлением `roadmap_status.json` (V1.7 по аналогии).
Самопроверка: `python tools/check_docs.py --strict`,
`python tools/check_file_layout.py --strict`, все четыре длины <380 строк.
