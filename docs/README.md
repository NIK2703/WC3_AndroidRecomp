# Индекс документации

`ARCHITECTURE_1.27B.md` — **нормативный хаб** (rev 2.2 / M2, ≤400 строк R4):
принципы, модель исполнения, слои, инварианты и указатели. Нормативные
**владельцы своих областей**: `FILE_LAYOUT.md` (R1–R14), `VERIFICATION.md`
(V/G), `GUEST_ABI.md` (ABI), `EXACTNESS.md` (E), `GRAPHICS_DECISION.md` (GB),
`HLE_MODULES.md` (H), `BUILD_PIPELINE.md` (BP), `BUILD_VARIANTS.md` (BV:
release/debug-варианты, имена пакетов, теги диагностики),
`ADR.md` (записи решений), `PERFORMANCE.md` (P0/P1/P2),
`PERFORMANCE_CONTRACT.md` (PM), `THREAD_AUDIT.md` (TA).
**Граница цели (ADR-026/027):** единственный **поставляемый** target —
Android arm64-v8a; рекомпилированный код под Windows не собирается.
Допускается единственный **верификационный** target `--target headless`
(`wc3_headless`, CI-хост, не APK, не продукт) — исполнимый DUT для T1
(V4.7). Windows-эталон — оригинальная лицензионная игра и её зафиксированные
захваты; `oracle/` хранит эталоны, а не исходники хоста; T1 = difftest H
против G без Windows-сборки.

Приоритет внутри области — у владельца, затем у ADR, затем у хаба. Хаб
ссылается, а не дублирует; **нумерация `§X` сохранена**, поэтому все ссылки в
документах и плане ведут на короткие разделы-указатели.

| Документ | Статус | Содержание |
|---|---|---|
| `ARCHITECTURE_1.27B.md` | **норма, rev 2.2 / M2** | Хаб: принципы, слои, модель исполнения, layout, HLE, DXVK, верификация, указатели |
| `ADR.md` | **норма (решения), rev 1** | ADR-001…ADR-029 с `{id,status,date,owner,supersedes,evidence}` (бывший §11); ADR-027 — headless-DUT T1, ADR-028 — гейт эквивалентности LiftPass, ADR-029 — game-first |
| `AS_BUILT.md` | снимок | As-built числа и история rev 2 (бывшие §0, §2.1); воспроизводимость F2 |
| `PERFORMANCE.md` | **норма (перформанс), rev 2.5** | Измерения, закрытые вопросы, инвентарь гонок, каталог P0-1…P2-11 без ранжирования (PM-17), разрывы измеримости Rev 2.5; владелец PM-* вынесен в отдельный документ по R4 |
| `PERFORMANCE_CONTRACT.md` | **норма (измерения), rev 3** | **PM-1…PM-24**: счётчики, бюджеты и калибровка, DAG проходов, классификация A–F по границе ABI, T2 sustained-load, профиль, валидность замеров, взвешивание, A/B, фазы, сценарий, цена LTO |
| `RISKS.md` | производный реестр | Риски в порядке приоритета (бывший §12) |
| `BACKLOG.md` | история + backlog | A1–A5 … G1–G13 плюс E5/E6, B5/B6, F4/F5 как работы по реализации (бывшие §13/§13.1) |
| `ROADMAP_PARALLEL.md` | рабочий план, не норма (хаб) | Дорожная карта реализации rev 2.2/M2: измеренный срез 2026-11-06, критический путь, W0–W4 и владение файлами для параллельной работы до 3 исполнителей (A — верификация/сборка, B — HLE/strangler, C — Android/графика); статусы гейтов и ADR не вводит |
| `ROADMAP_WORKPACKAGES.md` | рабочий план, не норма (владелец WBS) | Детализация дорожной карты по схеме «хаб + владелец» (R4/R22): аудит пробелов WP-0, реестр контрактов/evidence WP-1, 10 пакетов W0.1…W0.10 (WP-2), реестр 13 модулей M5 (WP-3), лестница DUT-0…DUT-5 (WP-4), контракт `wc3build.py` (WP-5), переходы G-01…G-15 (WP-6), трассировка PM/TA/GB/E (WP-7), протокол S* (WP-8), пакет задания/одиночный режим (WP-9), риски с триггерами (WP-10), открытые решения (WP-11), контрольные метрики (WP-12) |
| `ROADMAP_EXECUTION.md` | рабочий план, не норма (владелец исполнимой спецификации, второй проход) | EX-0…EX-14: аудит второго прохода, DAG/критический путь без выдуманных оценок (PM-2), кроссуоки Step −1…9 и M1…M8, тест-матрица 13 модулей M5, примеры `roadmap_contracts.json`/`roadmap_status.json`/`track_B.json`/evidence V8, бинарные чек-листы S0…S4, топология CI T0/T1/T2, дерево решений при красном гейте (ADR-029), политика данных oracle (ADR-006), де-комиссионинг 12 файлов карантина, понижение статуса гейта, свёртка 3→2→1 треков, спецификация `check_roadmap.py --strict`, матрица владения отчётами |
| `ROADMAP_ASSURANCE.md` | рабочий план, не норма (владелец полноты/отказоустойчивости, третий проход) | AS-0…AS-19: обратный индекс 12 семейств норм с впервые назначенными владельцами (E5/E6/E7/E9, ABI-7/8, R5/R11, BP-7.5, TA-15/17, PM-23/24, H9, G12), реконсиляция `in_flight` работы, процедура CCR на изменение контрактов, матрица откатов по пакетам, каскад инвалидации 10 хэшей, агрегация вердиктов `fail > unavailable > pass` по TA-12/V5.2/E11, снятие Step −1/V5.9 baseline из O/G, синхронизация `migration.json` ↔ `roadmap_status.json`, реестр анти-паттернов, границы области, метрики процесса и критерий полноты плана; AS-13…AS-19 (четвёртый проход) — RACI интегратора, владение валидатором, протокол коммуникации треков, `plan_rev = 4` с правилом консолидации (пятый документ только по решению владельца), ре-базелайнинг план↔реальность, операционная карточка и архивация плана; валидатор целостности реализован в `tools/check_roadmap.py` |
| `FILE_LAYOUT.md` | **норма (структура), rev 2** | **R1–R22**: целевое дерево, статусы исполнимости правил, карантин миграции, граф зависимостей, owners map, миграция M1–M7 в `config/migration.json` |
| `VERIFICATION.md` | **норма (верификация), rev 2 / ADR-029** | V1–V10: статусы гейтов как диагностика, **V4.7 — headless-DUT T1**, роли O/G/H/D, V5.7/V5.8, **G-15** и V6.5, V5.9, V6.6/V6.7, PM-1…PM-24 и TA-1…TA-18 в V9/V10, матрица `G-01…G-15`, oracle identity, difftest, acceptance-прогон |
| `GUEST_ABI.md` | **норма (граница корпуса), rev 1** | ABI-1…ABI-11: что заморожено, ADR-019, идентичность корпуса и реестра, единое состояние переходов, статус P0-1, G-14, **ABI-11/G-15 — эквивалентность сгенерированного C** |
| `EXACTNESS.md` | **норма (точность), rev 1** | E1–E12: эталоны, время, FP/replay, память, bootstrap, sync, lifecycle, критерий равенства difftest |
| `GRAPHICS_DECISION.md` | **норма (выбор DXVK), rev 1, ADR-023** | GB-1…GB-10: статусы ADR, матрица A/B/C/D, fallback, измерения, kill criterion |
| `HLE_MODULES.md` | **норма (разбиение HLE), rev 1, ADR-024** | H1–H12: дерево по guest DLL, §4.2 ↔ R7, 2 009 импортов, карта M5, реестр |
| `BUILD_PIPELINE.md` | **норма (сборка), rev 2, ADR-025** | BP-1…BP-12: самостоятельность target-дерева, вендоринг, `wc3build.py`, воспроизводимость, CI, конец cutover, ccache |
| `BUILD_VARIANTS.md` | **норма (варианты и диагностика), rev 1, ADR-030** | BV-1…BV-13: `applicationId` `wc3.recomp` / `wc3.recomp.debug`, один атомарный `--variant`, изоляция данных двух UID, `wc3_build_config`, реестр тегов `WC3_DEBUG_<ЦЕЛЬ>`, обязательная обёртка отладочных инструкций, guest-ABI-инвариант наблюдения, release hygiene и план миграции (G-12/G-13) |
| `THREAD_AUDIT.md` | производный + гейт, rev 2.4 | 36 thread/sync импортов, evidence, инвентарь гонок, T0/T1/T2, **TA-1…TA-18 — контракт потоков, матрица владения, L0/L1/L2, стресс-тест, расписания T1, реентерабельность, shutdown, summary для L2** |
| `DXVK_THREADS.md` | производный + гейт | Dual-DXVK, symbol isolation, ICD, vendor-потоки, TA-7 |
| `reports/REFACTOR_PLAN_CRITIQUE_2.md` | история | Первый critique-pass rev 1 |
| `reports/REFACTOR_PLAN_CRITIQUE_2_FOLLOWUP.md` | история | Второй critique-pass rev 1 |
| `reports/CRITIQUE_FILTER_THREADING.md` | история | Отмена принудительной однопоточности |
| `reports/generated/*.json` | снимки (gitignored) | `perf_baseline.static.json`, `concurrency_audit.static.json`, `devirt_plan.json` |

## В корне проекта

| Файл | Роль |
|---|---|
| `AGENTS.md` | Обязательные правила (производная сводка ADR) |
| `README.md` | Цель, статус, быстрый старт |
| `PLAN_REFACTOR_1.27B.md` | Шаги миграции и acceptance-гейты (после M2 → `docs/PLAN.md`) |
| `PROGRESS.md` | Фактический прогресс переноса инструментария, проверки и планы |

## История ревизий

Все ревизии сохраняют нумерацию `§X` и стабильные id (`V/G`, `ABI`, `E`, `GB`,
`H`, `R`, `BP`, `P`, `PM`, `TA`). Матрица V2 с Rev 2.4 не менялась:
`enforced` 3 / `declared` 6 / `missing` 6. ADR-029 (game-first) делает гейты
диагностикой: новые контракты запрещают выдавать замер за доказательство, но
не останавливают разработку.

| Rev | Добавлено | Норма / владелец |
|---|---|---|
| M2 / F3 | as-built, ADR, перформанс, риски и backlog вынесены из хаба (1261 → 355 строк); F1/F2/G11 закрыты. `PLAN_REFACTOR_1.27B.md` → `docs/PLAN.md` остаётся за M4 | `AS_BUILT.md`, `ADR.md`, `docs/README.md` |
| 2.3 / ADR-027 | `--target headless`, `wc3_headless`, роли O/G/H/D; E3.6 без рекомпилированного Windows-хоста; V4.5 = входное условие T1; G-09/G-13 = `missing` | V4.7/V5.7/V5.8, E1.1/E3.6/E11, BP-5.5/BP-7.2/BP-8.3/BP-11 |
| 2.4 / ADR-028 | G-15 и ABI-11: эталон Python-reference лифтер, нормализованные тела, function-level change set, инварианты в генераторе, порядок G-14 → G-15 → pass. Число 82,93% относится только к `war3.exe` | `GUEST_ABI.md` ABI-11, V6.5 |
| 2.5 | V5.9 (три семантических инварианта против G), V6.6 (baseline из O/G на исправленной сборке), владелец Step −1. Графический дефект исправлен в старом дереве | V5.9/V6.6, E11, BACKLOG C4 |
| 2.6 | PM-1…PM-9 (счётчики, бюджеты, G-10 prerequisite, 18 модулей, DAG, отчёт V1.6) и TA-1…TA-8 (матрица владения, host-поток, L0/L1/L2, E5.3, mailbox, vendor-потоки) | `PERFORMANCE_CONTRACT.md`, `THREAD_AUDIT.md` |
| 2.7 | PM-10…PM-13 (классы A–F по границе ABI, калибровка PM-11, T2 sustained-load PM-12, профиль PM-13), TA-9; исправлены PM-5 и P1-13. `PM-*` вынесен из `PERFORMANCE.md` (419 строк) по R4 | `PERFORMANCE_CONTRACT.md`, `check_docs.py` |
| 2.8 | PM-17…PM-24 и TA-12…TA-18; `check_docs.py` проверяет сплошность `PM-n`/`TA-n`; V6.7/V10/G-10 и `AGENTS.md` приведены к ADR-029 | `PERFORMANCE_CONTRACT.md` rev 3, `THREAD_AUDIT.md` rev 2.4 |
| 2.9 | R15–R22, `config/migration.json` (M1–M7), валидатор в `check_file_layout.py`, R4-overflow = error, near-limit ≥380, R-id в `check_docs.py` | `FILE_LAYOUT.md` rev 2 §6, `BACKLOG.md` F4/F5/B7 |
| 2.10 | M8: 25 compiled C-инструментов (22 пинящихся upstream + `corpus`/`liftpass`/`closure_pipeline` в `tools/lift/csrc_local/`), sha256-пин upstream, прототип G-15. Замыкание корпуса — только compiled C-конвейер `entry → seed → lift32 → closure → known → liftpass` (`closure_pipeline.exe`), без Python-драйвера; `CMakeLists.txt` читает upstream из `config/build_upstream.json` | `PROGRESS.md`, `config/migration.json`, `reports/generated/closure_pipeline.json` |
| 2.11 | BV-1…BV-13 + ADR-030: нормативная архитектура release/debug (`wc3.recomp` / `wc3.recomp.debug`), единый `--variant`, теги `WC3_DEBUG_<ЦЕЛЬ>` для любой диагностики; новый владелец зарегистрирован в индексе и `check_docs.py` по R22 | `BUILD_VARIANTS.md` rev 1, `ADR.md`, §9.3/§9.4 хаба, `AGENTS.md`, BP-7.4 |

Канонический acceptance-прогон — `VERIFICATION.md` V7. Состояние миграции
ведётся в `config/migration.json` (R19), а не в этом тексте.

## Воспроизводимость чисел и быстрые проверки

```powershell
python tools/perf_probe.py --json reports/generated/perf_baseline.static.json
python tools/concurrency_audit.py --json reports/generated/concurrency_audit.static.json
python tools/check_file_layout.py --strict
python tools/run_tests.py
python tools/check_arch.py --strict --concurrency --release
```

Блок выше не acceptance-прогон. Канонический список команд без ослабляющих
флагов — `VERIFICATION.md` V7; на текущем шаге он ожидаемо не зелёный
(V7.1, V10).
