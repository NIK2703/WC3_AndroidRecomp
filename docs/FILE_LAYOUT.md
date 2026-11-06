# Файловая структура проекта и распределение кода

Статус: **rev 2** (2026-11-06). Нормативный документ по структуре дерева;
правила **R1–R22** и их статусы исполнимости фиксируются здесь, чеки живут в
`tools/check_file_layout.py` и `tools/check_docs.py`. Rev 2 добавляет §6 —
аудит оптимальности дерева (R15–R22) и `config/migration.json` как
машиночитаемое состояние M1–M7. Связанные документы:
`ARCHITECTURE_1.27B.md` §4 (слои/дерево), §10.2 (гейты), §13 F1–F3,
`BUILD_PIPELINE.md` BP-4/BP-5, `HLE_MODULES.md` H6.3.

## 1. As-built: что измерено

| Метрика | `warcraft3_recomp` | `wc3_android_recomp` |
|---|---|---|
| Файлов в `tools/` | 208 | 11 (+ `lift/passes/`) |
| Артефактов в build-каталогах | **25 712** | ~20 (build/host) |
| Место build-артефактов | **17 ГБ + 6 ГБ + 2,4 ГБ ≈ 25 ГБ** | <1 МБ |
| Крупнейший исходник | `hle_android.c` **398 КБ / 10 333 строки** | `cpu.h` 45 КБ / 955 строк |
| Вариантов dispatcher | **6** (`c_`, `py_`, `dispatch_test_lift`, `dispatch_test_py`, `wc3_dispatch_agent`, `wc3_dispatch_all`) | 1 (`gen_dispatch.py`) |
| Файлов `hle_d3d9*` | **9** (4 frontend'а + generated + lock + iids) | 0 (ещё не перенесены) |
| Крупнейший документ | `ARCHITECTURE_1.27B.md` 1253 строки | **>1200 строк, ~117 КБ** (точное число печатает `check_file_layout.py --strict`, правило R4) |
| Собственный `.git` | есть | **нет** — дерево внутри `/e/projects/.git` |
| `.gitignore` | есть (образцовый) | **не было до этой ревизии** |
| Скомпилированные библиотеки в дереве | — | `third_party/softfloat/build-host/softfloat.a` (346 КБ) |
| Сгенерированные отчёты | смешаны с исходниками | **лежали в `config/`** (исправлено) |
| PE-импортов в фикстуре | — | 2 009, из них **832 unresolved**, 50 forbidden, 76 excluded |

**Оценка: ~5/10.** Слои в §4 архитектуры спроектированы правильно, но само
дерево этой структуры ещё не выражает: сгенерированное перемешано с
рукописным, входные данные — с отчётами, нормативный документ стал монолитом,
а граница «oracle / продукт» не видна по именам каталогов. Плюс два дерева
живут в разных git-контекстах.

**Таблица выше — исторический снимок M1 (F2).** На 2026-11-06 (Rev 2)
измерено заново: 56 исходных файлов в T0-скане, 18 нормативных документов,
крупнейший исходник — `runtime/legacy/hle_android.c` **10 356 строк**,
крупнейшие документы — `ARCHITECTURE_1.27B.md` и `VERIFICATION.md` по
**399 строк** (до предела R4 — 1 строка), `apps/android_gameactivity/build/`
содержит Gradle-кэш вне корневого `build/`, а `config/` по-прежнему хранит
1,1 МБ `imports.1.27b.json` и 462 КБ `pe_imports.1.27b.json` рядом с
рукописными входами. Числа обязаны воспроизводиться `check_file_layout.py`
и `config/migration.json`, а не цитироваться из текста (F2, R19).

## 2. Целевое дерево (уточняет §4.2 архитектуры)

```
wc3_android_recomp/
  .gitignore  .gitattributes  AGENTS.md  README.md
  docs/
    README.md                 # индекс: нормативные / производные / история
    ARCHITECTURE.md           # НОРМА ~200 строк: принципы, слои, инварианты
    ADR.md                    # решения с {id,status,date,owner,supersedes}
    AS_BUILT.md               # снимок чисел + ссылки на reports/generated/*
    PERFORMANCE.md            # §3.7: измерения, ladder, закрытые вопросы
    PERFORMANCE_CONTRACT.md   # PM-1..PM-24: счётчики, бюджеты, DAG, ABI, валидность замеров
    VERIFICATION.md           # T0/T1/T2, гейты, Definition of Done
    RISKS.md  BACKLOG.md      # реестр рисков и приоритезированный backlog
    FILE_LAYOUT.md            # этот документ
    THREAD_AUDIT.md  DXVK_THREADS.md
    PLAN.md                   # перенесён из корня
    history/1.26/  history/rev1/
  config/                     # ТОЛЬКО ВХОДНЫЕ ДАННЫЕ (hand-written / canonical)
    schema/*.json              # JSON Schema для каждого типа конфигурации
    layout.1.27b.json  corpus.1.27b.json
    hle_arity.1.27b.json  cdecl_allowlist.1.27b.json
    arity_overrides.1.27b.json  hle_table.1.27b.tsv
    import_policy.1.27b.json   # классификация 2009 PE imports
    migration.json             # R19: machine-readable состояние M1-M8 и R15-R22
    build_upstream.json        # BP-4.2: sha256-пин C-инструментария и vendor-файлов
    dep_matrix.json            # R16: разрешённые рёбра слоёв §4.3
    file_owners.json           # R18: один владелец на файл/слой/модуль
    generated/                 # сгенерированные фикстуры, явно помечены
      pe_imports.1.27b.json  imports.1.27b.json
      baseline.1.27b.before.json
  reports/generated/           # ВЫХОДЫ (gitignored, регенерируются)
    perf_baseline.static.json  concurrency_audit.static.json
    devirt_plan.json  coverage.json
  runtime/
    cpu/       cpu.h (umbrella), cpu_flags.h, cpu_memory.h, cpu_atomics.h, x87/
    guest/     guest_layout.h, dispatcher.h, direct_call.h, generated/
    hle/
      abi/
      win32/   kernel32/ user32/ gdi32/ advapi32/ winmm/ msvcr80/ miles/
      d3d9/  dsound/
    platform/  android/ fake/ headless/        # headless = только T1-DUT
    gfx/       d3d9/ dxvk_1_10/ dxvk_3_1_1/ null/   # null = только T1-DUT
    ipc/  app/  diag/
    legacy/                   # R15: карантин миграции, read-only, QUARANTINE.md
  third_party/                 # vendor-owned, VENDOR_MANIFEST.md + LICENSE each
    capstone/     include/capstone/*.h + bin/capstone.dll (host tools only)
    dxvk/1.10.3/  dxvk/3.1.1/  dxvk/shim/
    softfloat/ (только source/)  winlator-input/ (MIT)
  oracle/                      # эталонные захваты оригинальной игры (НЕ Windows-хост)
    identity.json  normalization.json  host27/  tools/  baselines/
  apps/android_gameactivity/   # R17: один app/src/main/ для Java/res/cpp, Gradle в корне модуля
  apps/wc3_headless/          # T1-DUT, НЕ в APK: --target headless (V4.7, ADR-027)
  tools/
    checks/     check_arch, check_file_layout, check_thread_audit, check_docs, check_deps
    fixtures/   pe_imports, import_fixture, hle_coverage
    lift/       lift.py (Python-reference), passes/*, build_tools.ps1, check_*.py
    lift/csrc/  M8: 29 побайтно пинящихся C-файлов (22 инструмента + лифтер)
    perf/       perf_probe, concurrency_audit
    build/      wc3build, smoke
    run_tests.py  difftest.py
  tests/
    unit/  golden/  static/  device/  fixtures/
  build/                        # все продукты сборки (gitignored): APK + headless DUT
```

Ключевые отличия от §4.2: `oracle/` вынесен из `runtime/`; HLE разбит **по
guest DLL**, а не по «file/memory/time/process»; `dxvk_win32` переехал в
`third_party/dxvk/shim/` (это vendor-код, а не платформенная реализация);
`config/` отделён от `reports/generated/`; `tools/` и `tests/` сгруппированы
по роли; добавлены `schema/` и `import_policy`.

## 3. Базовые правила R1–R14 (статусы исполнимости — R20)

| # | Правило | Почему | Гейт |
|---|---|---|---|
| R1 | `config/` содержит только входные данные; сгенерированное — в `config/generated/`, отчёты — в `reports/generated/` | Сейчас 1,1 МБ `imports.*.json` и два baseline JSON лежали рядом с `cdecl_allowlist` — нельзя понять, что редактировать вручную | `partial`: regex ловит не те имена (R21) |
| R2 | Сгенерированный корпус — только `runtime/guest/generated/`, gitignored | 2,4 ГБ C в старом дереве смешаны с `reports/`; ADR-006 | `error`: `.gitignore` + скан |
| R3 | Ни одного `.o/.a/.so/.exe/.apk` в исходном дереве | `third_party/softfloat/build-host/softfloat.a` | `error`: `.gitignore` + скан |
| R4 | Исходник ≤ 2000 строк, нормативный документ ≤ 400 строк | `hle_android.c` 10 333 строки (старое дерево), ARCHITECTURE 1261 до M2; оба стали «файлами, где лежит всё». **Хаб закрыт M2: 355 строк, предупреждение R4 снято** | `error` для обоих классов (R22, Rev 2) |
| R5 | Ровно один генератор dispatcher; варианты — только в `build/` | 6 вариантов в `build27/` — неясно, какой авторитетный | **no-op**: условие в гейте невыполнимо (R20, M7) |
| R6 | Один graphics frontend **в продукте**; legacy — в `docs/history/` или удалён. `gfx/null` допустим только как верификационный frontend `--target headless` и запрещён в APK (ADR-027, V4.7.3) | 9 `hle_d3d9*` файлов / 4 пути рендера | `review`: `--policy` = declared (G-02) |
| R7 | Модули HLE повторяют границы guest DLL | «file/memory/time/process» — группировка по нашему удобству, а не по ABI гостя; 426 записей и 2 009 PE-импортов трудно трассировать. **Конфликт с §4.2 ARCHITECTURE разрешён ADR-024**: уровень 1 = DLL, уровень 2 = подсистема; норма `docs/HLE_MODULES.md` H1/H2 | `review` + H5/H11 |
| R8 | `oracle/` вне `runtime/`, вне link graph APK | Каталог эталонных захватов, а не исходников Windows-хоста; рекомпиляция только под Android (ADR-002/026) | `review`: `--policy` = declared |
| R9 | Vendor-код только в `third_party/` с `VENDOR_MANIFEST.md` и LICENSE | DXVK/SoftFloat/MIT winlator-input/adrenotools — разные лицензии | `warn`: только наличие манифеста |
| R10 | У каждого `config/*.json` есть `schema` и файл схемы | сейчас `schema: 1` без описания полей; 832 unresolved импорта не классифицированы | `warn`: ключ `schema`, не файл схемы |
| R11 | Один нормативный документ, остальные ссылаются по ADR-id/§-id; `check_docs.py` ищет противоречия | F1: инварианты продублированы в 6 файлах → уже был инцидент с 63 292 → 191 650 | `error`: `check_docs.py` + сплошность R/PM/TA-id |
| R12 | `tools/` и `tests/` сгруппированы по роли | 208 файлов в старом `tools/` без структуры; в новом 11 плоских + 1 вложенный | `warn`: 6 отсутствующих каталогов |
| R13 | Имена `<subsystem>_<role>.<ext>`; запрещены `_old/_v2/_test_py/_agent` в исходниках | `dispatch_test_py.c`, `wc3_dispatch_agent.c`, `hle_d3d9_thin_gen.cpp` | `error`, но только для `.c/.h/.cpp` |
| R14 | Один git-контекст на проект; `.gitignore` и `.gitattributes` обязательны | новое дерево внутри чужого `/e/projects/.git`, без ignore-файла | `warn`: собственного `.git` нет |

## 4. Что сделано в rev 1 (M1/M2)

Хаб разделён по схеме «хаб + владельцы»: `AS_BUILT.md`, `ADR.md`,
`PERFORMANCE.md`, `RISKS.md`, `BACKLOG.md` и восемь нормативных владельцев
(`VERIFICATION.md` V1–V10, `GUEST_ABI.md` ABI-1…11, `EXACTNESS.md` E1–E12,
`GRAPHICS_DECISION.md` GB-1…10, `HLE_MODULES.md` H1–H12,
`BUILD_PIPELINE.md` BP-1…11, `THREAD_AUDIT.md` TA, `PERFORMANCE_CONTRACT.md` PM).
Добавлены `.gitignore`, `.gitattributes`, `docs/README.md`,
`tools/check_file_layout.py` и `third_party/VENDOR_MANIFEST.md`.
`reports/generated/` отделён от `config/`. ADR-027 определил headless-DUT
(G-09/G-13 = `missing`, реализация — Step 0). После выноса `PM-*` в
`PERFORMANCE_CONTRACT.md` по R4 (419 строк) `PERFORMANCE.md` снова <400 строк.

M2: хаб 1261 → 355 строк, F1/F2/F3/G11 закрыты, нумерация `§X` сохранена.
Гейт на момент rev 1: 0 errors, warnings по R14/R10/R12. Текущее состояние —
`config/migration.json` (R19).

В rev 1 `config/imports.1.27b.json`, `pe_imports.1.27b.json` и
`baseline.1.27b.before.json` не перемещались, чтобы не сломать
`import_fixture.py` и `smoke.py`. Rev 2 снимает эту льготу: R19 даёт
машиночитаемый статус долга, R21 — манифест путей, обновляющий обоих
потребителей одним коммитом (образец BP-5.1 для `link_apk.rsp`).

## 5. Порядок миграции (без big-bang)

Эта таблица — **проекция** `config/migration.json` (R19): статусы, владельцы
и evidence шагов хранятся в машиночитаемом виде, `check_file_layout.py`
проверяет схему и запрещает `status: done` без `evidence`. Текст ниже не
является источником истины о прогрессе (F2).

| Шаг | Действие | Гейт |
|---|---|---|
| M1 | `.gitignore`, `reports/generated/`, индекс docs, гейт R1–R14 | `check_file_layout --strict` (с предупреждениями по R4) |
| M2 | **DONE (rev 2.2)** — вынесены `ADR.md`, `AS_BUILT.md`, `PERFORMANCE.md`, `RISKS.md`, `BACKLOG.md` плюс шесть владельцев областей; хаб 355 строк; `§X` сохранены, стабильные id введены, ADR получили метаданные | `check_docs.py --strict` + `check_file_layout.py --strict` без R4-warning |
| M3 | `config/schema/*` + `import_policy.1.27b.json`: классифицировать 832 unresolved / 50 forbidden / 76 excluded по состояниям `implemented`, `optional-unused`, `forbidden-LAN`, `fatal-missing` | `hle_coverage.py --fixture-kind pe` |
| M4 | Перенести `tools/` и `tests/` по ролям; обновить `run_tests.py` и все CLI-пути | `run_tests.py` полный набор зелёный |
| M5 | Перенести `hle_android.c` по DLL-модулям (R7), по одному модулю за коммит | T0 + T1 (difftest против эталонных захватов) на каждом шаге (§2.3) |
| M6 | Вынести `oracle/`, разделить `cpu.h` на umbrella + подзаголовки, убрать оставшиеся warnings (R14/R10/R12) | `check_file_layout --strict` без warnings |
| M7 | Закрыть R15–R22: карантин `runtime/legacy/`, граф зависимостей, один source root apps, owners map, маркеры generated, R4-эскалация | `check_docs.py --strict` (R-id), `check_deps.py`, `check_file_layout.py --strict` без новых warnings |
| M8 | **DONE (Rev 2.10)** — перенести compiled C-инструментарий и лифтер в target-дерево: 25 бинарников (22 пинящихся + `corpus`/`liftpass`/`closure_pipeline`), `build_tools.ps1`, sha256-пин upstream, прототип G-15. Замыкание `entry → seed → lift32 → closure → known → liftpass` до неподвижной точки — compiled C-драйвер, сгенерированный C остаётся в `build/` | `build_tools.ps1 -Force -Smoke`, `check_upstream.py` (T0 `upstream_pin`), `check_equivalence.py --self-test`, `closure_pipeline.exe --self-test` |

## 6. Аудит оптимальности дерева (Rev 2, R15–R22)

R1–R14 задают целевое дерево, но не описывают механику: владение файлами,
направление зависимостей, размещение исходников на время переноса и действия
при достижении лимита R4. Оценки ниже измерены по дереву на 2026-11-06.

| Аспект | Глубина | Почему |
|---|---|---|
| Целевое дерево и слои §4 | 8/10 | иерархия ясна, конфликт §4.2 ↔ R7 закрыт ADR-024 |
| Гигиена бинарников (R2/R3) | 7/10 | `.gitignore` и скан работают |
| Документооборот (R11/R4) | 7/10 | M2 сделал «хаб + владельцы», но потолок в 1 строку |
| Исполнимость R1/R5/R9/R10/R12 | 3/10 | R1 ловит не те имена, R5 — no-op, остальные warning-only |
| Направление зависимостей §4.3 | 1/10 | объявлено прозой, ни один `#include` не проверен |
| Изоляция `runtime/legacy/` | 1/10 | каталог появился при ADR-029 без правила; новый код уже включает его заголовки |
| Статус миграции | 2/10 | M1–M6 живут только в тексте §5 (ровно инцидент F2) |

Восемь разрывов перечислены в столбце «Почему» таблицы правил ниже. Три из
них видны в коде: `runtime/boot/dispatch_boot.c` включает `host.h` и
`hle_android.h` из `runtime/legacy/` (10 356 строк); `apps/android_gameactivity/`
имеет два source root; хаб и `VERIFICATION.md` стоят на 399/400 строках R4.

| # | Правило | Почему | Минимальный исполнимый чек |
|---|---|---|---|
| R15 | **Карантин миграции**: as-built исходники живут только в `runtime/legacy/`, read-only, с `QUARANTINE.md` (`file, source_path, source_revision, sha256, target_module, owner_step, expires_at`) | Перенос идёт, а границы нет: `runtime/boot/dispatch_boot.c` включает `host.h`/`hle_android.h` из карантина; R4 краснеет без внятного статуса | `check_file_layout.py`: файл в quarantine без строки манифеста или с истёкшим `expires_at` → error; `#include` из quarantine вне `hle_legacy_shim` → error |
| R16 | **Направление зависимостей исполняемо**: `config/dep_matrix.json` задаёт разрешённые рёбра слоёв §4.3; `tools/check_deps.py` строит граф `#include` по `runtime/`/`apps/` | Единственный центральный инвариант структуры не проверяется ни одним T0-гейтом; `--policy` смотрит link graph, а не включения | `check_deps.py --strict`: ребро вверх, `hle/abi` → OS-заголовок, `cpu` → HLE/platform = error; отчёт по V1.6 |
| R17 | **Один source root на Gradle-модуль**: Java/res/manifest и native-код в одном `app/src/main/`, все продукты сборки — только в корневом `build/` | Сейчас `src/main/cpp/host_shell.c` и `app/src/main/java/...` — два дерева; `.gitignore` содержит ad-hoc `/apps/android_gameactivity/build/` без правила | `check_file_layout.py`: второй `src/main/` в модуле → error; каждый путь в `.gitignore`, содержащий `/build/`, обязан быть в R2-allowlist |
| R18 | **Карта владельцев**: `config/file_owners.json` — ровно один владелец на нормативный документ, слой `runtime/*`, модуль `apps/*` и группу `tools/*` | Параллельные правки уже сломали T0 (B6); «один владелец на файл» из B2 не имеет артефакта | `check_file_layout.py`: новый файл в охраняемой области без записи в карте → error; дубли владельца → error |
| R19 | **Машиночитаемая миграция**: `config/migration.json` с `{schema, steps:[{id,status,owner,gate,evidence,updated_at}]}` для M1–M7; §5 — проекция, не источник | F2: «M2 DONE» и «0 errors» когда-то были верны, сегодня это текст, а не состояние | `check_file_layout.py`: невалидная схема, дубли `id` или `status: done` без `evidence` → error (реализовано в Rev 2) |
| R20 | **Статус исполнимости каждого Rn**: `{enforced_by, verdict_kind: error\|warn\|review}`, запрет слова «гейт» без команды; R5 no-op и R1 regex исправляются | Половина строк таблицы §3 обещает «гейт», но даёт warning или ревью; `declared` вместо `enforced` (ADR-022) | `check_docs.py`: столбец «Гейт» ссылается на существующий инструмент; `check_file_layout.py` печатает матрицу вердиктов по R1–R22 |
| R21 | **Единственный маршрут generated**: маркер `/* GENERATED by <tool> — DO NOT EDIT */` + `source_sha256`; только `config/generated/`, `reports/generated/`, `runtime/guest/generated/`; манифест путей обновляет всех потребителей одним коммитом | `config/` смешивает 1,1 МБ фикстур с рукописными входами, «оставлено намеренно» не имеет владельца и срока (BP-5.1 уже решил ту же задачу для `link_apk.rsp`) | `check_file_layout.py`: файл без маркера в generated-каталоге → error; битый путь в манифесте → error; `import_fixture.py`/`smoke.py` читают манифест |
| R22 | **R4 — ошибка и эскалация**: превышение 400 строк документом = error; ≥380 = `near-limit`-warning с обязательным владельцем разделения; новый владелец области регистрируется в индексе и `check_docs.py` тем же коммитом | Превышение для документов давало warning «split in step M2» при закрытом M2; хаб и VERIFICATION на 399, следующая правка прошла бы незамеченной | `check_file_layout.py`: error на >400, консолидированный warning на ≥380; шаблон M2 «хаб + владелец» без нового `G-xx`/ADR |

R15: исключение действует только на файлы из `QUARANTINE.md`, до `expires_at`
и с `target_module` по H5. Повышать лимит R4 или удалять проверку запрещено
(V1.7). Доступ к состоянию as-built идёт через `hle_legacy_shim` (H6.3) с
одним владельцем на глобал (TA-1).

R16: `config/dep_matrix.json` пинит §4.3 с `sha256` в отчёте (BP-6.2); при
расхождении приоритет у матрицы до обновления хаба одним коммитом (R11).
R19: `config/migration.json` — вход, не отчёт. Он не лежит в
`reports/generated/`, не перезаписывается гейтом и меняется вместе с §5.

**Статусы (R20).** `error`: R2/R3/R4/R11/R13. `warn`: R9/R10/R12/R14.
`review`: R6/R7/R8. `no-op`: R5. `partial`: R1. R15–R22 = `specified`,
реализованы R19 и R22 плюс контроль сплошности R-id в `check_docs.py`;
остальные чеки — M3/M4/M5/M7. Матрица V2 не меняется, новый `G-xx` и ADR не
вводятся, по ADR-029 чеки не блокируют разработку. `Rn`, `PM-n`, `TA-n`
проверяются на определённость и сплошность (R11).
