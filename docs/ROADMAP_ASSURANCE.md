# Дорожная карта: полнота, отказоустойчивость и границы (третий проход)

Статус: **рабочий план, не норма** (2026-11-06, 23:23 GMT+6,
`HEAD 6055be0`). Четвёртый владелец связки «хаб + владельцы»:
`ROADMAP_PARALLEL.md` (стратегия) → `ROADMAP_WORKPACKAGES.md` (WBS) →
`ROADMAP_EXECUTION.md` (DAG/тесты/CI) → **этот документ** (обратный индекс
норм, реконсиляция in-flight, изменение контрактов, откаты, каскад хэшей,
агрегация вердиктов, анти-паттерны, границы области). Разделение обязано
R4/R22 (≤400 строк на `docs/*.md`). Приоритет: нормативные владельцы →
хаб → WP → EX → AS. Новых `G-xx`, ADR, `R/PM/TA`-id не вводит.

---

## AS-0. Аудит третьего прохода

| # | Пробел после EX-0…EX-14 | Где закрыто |
|---|---|---|
| 1 | Нет **обратного индекса норм**: какое требование E/V/R/H/BP/GB/ABI/PM/TA/G каким пакетом закрывается, а какое забыто | AS-1 |
| 2 | Наблюдённая in-flight работа (`kernel32/memory`, `process`, `platform/headless`) не имеет статуса и процедуры поглощения | AS-2 |
| 3 | Заморозка контрактов есть, но нет процедуры **запроса на изменение** (кто, какие поля, какие тесты) | AS-3 |
| 4 | Rollback описан только для точек S*, нет механизма отката **отдельного пакета/модуля** | AS-4 |
| 5 | Хэши V5.7/BP-6 перечислены, но нет **каскада инвалидации**: что меняется при правке чего | AS-5 |
| 6 | TA-12 задаёт N расписаний, но нет правила **агрегации вердиктов** по ним и по контрольным точкам | AS-6 |
| 7 | Step −1/V5.9 baseline из O/G сопоставлен волне, но нет процедуры снятия захвата | AS-7 |
| 8 | Два machine-readable статуса (`migration.json`, `roadmap_status.json`) могут разойтись | AS-8 |
| 9 | Инциденты проекта (F2, B2/B6/B7, drift, R3, «уже собрано») разбросаны по документам | AS-9 |
| 10 | Нет явной границы области: что карта **не** решает и что отложено | AS-10 |
| 11 | Нет артефактно-измеряемых метрик самого процесса (без выдуманных порогов, PM-2) | AS-11 |
| 12 | Нет критерия полноты и регламента пересмотра самого плана | AS-12 |

Метод AS-1: прямой проход по стабильным id из документов-владельцев, затем
обратная проверка «есть ли пакет, гейт и evidence». Всё, что не нашлось,
**впервые назначается** в таблице ниже, а не объявляется «и так покрыто».

---

## AS-1. Обратный индекс норм (матрица полноты)

| Семейство id | Закрыто в плане | **Впервые назначено** (было без владельца) | Evidence |
|---|---|---|---|
| `E1`–`E4`, `E8`, `E11` | WP-4 DUT-0…5, EX-3, WP-7 | — | identity V3.3, golden E3/E4, `WaitBlock` |
| `E5` (модель памяти A1), `E6` (G4) | частично EX-3/`memory` | **W2/B3**: статический аудит обычных load/store + TSO/ARM64 контракт; release-проверка арены, debug — полная; владелец интегратор (`runtime/guest` frozen) | `track_B.json`, stress E5.3 |
| `E7` (порядок bootstrap G5) | не было | **W0/boot, интегратор**: `layout → images → registry → dispatch_init → frozen=1 → CreateThread`; `g_lift_hash` не пишется после `frozen`; debug assertion | `check_roadmap.py` + unit boot-тест |
| `E9` (жизнь процесса G1/G8, `Recreated`) | хаб §8.1 только как норма | **W3/C**: таблица `state × action` E9.2, политика смерти процесса (перезапуск с последнего save), join guest threads до destroy device | `guest_runner_test`, T2 lifecycle |
| `E10`, `E12` | частично EX-10 | **AS-10/AS-12**: границы ответственности и трассировка id → пакет | этот индекс |
| `V1`–`V10`, `V4.7`, `V5.9`, `V6.6`, `V7` | WP-1/4/5/6, EX-2/5/7 | — | V8 evidence, `gate_status.json` |
| `R1`–`R4`, `R15`–`R22` | W0.1…W0.9, WP-2, EX-9 | `R5` (один генератор dispatcher, сейчас no-op) → **W0.3 `check_deps.py`**; `R11` (один нормативный источник) → **W0.9 `check_roadmap.py`** | отчёты чеков |
| `R6`, `R8`, `R9`, `R10`, `R13`, `R14` | C2/C3, EX-8, W0.5/W0.1 | `R6` → G-02 по link graph; `R14` → worktree-схема WP-8; `R10` → `config/schema/*` в W1/A | `check_file_layout --strict` |
| `H1`–`H12` | WP-3, EX-3, WP-9 | `H9` (`result_kind`/`arg_source`/`x87_stack_delta`) → **S1 правка `hle_abi.h` интегратором** (ABI-8, не перелифт) | `hle_abi_test`, golden E4.4 |
| `BP-1`–`BP-11` | WP-5, EX-6, EX-8 | `BP-7.5` (числовой boot/crash descriptor, D2) → **W3/C** | startup error codes, `release_hygiene_test` |
| `GB-1`–`GB-10` | WP-7, EX-1/C4, WP-11 | `GB-4.7` (понятный экран при отказе обоих DXVK) → **W1/C1** уже назначено; `GB-5.7` (5 строк `unavailable`) → **W3/C** | device matrix, ADR `supersedes` |
| `ABI-1`–`ABI-6`, `ABI-9`–`ABI-11` | §3.3 хаба, W4a, WP-6 | **`ABI-7`** (ёмкость thunk-окна 65 536 ≠ активный реестр 4 096) → **W0.9 static assertion** в `legacy_shim.c` + `check_contracts.py` | отчёт чека |
| `ABI-8` | частично EX-3 | **S1**: расширение `HleEntry` полями `result_kind`/`arg_source`/`x87_stack_delta` без смены `CPU` (ABI-2) | `hle_abi_test` |
| `PM-1`…`PM-24` | WP-7, EX-1/A5, EX-6 | `PM-23` (два уровня `-O` + UBSan) → **W4a**; `PM-24` (interim-профиль) → **W3** | `perf.<profile>.json` |
| `TA-1`…`TA-18` | WP-3/B3, WP-7, EX-3 | `TA-15` (`DllMain`/loader lock) → **W2/B4**, `TA-17` (staging ABA) → **W2/C2**; агрегация → AS-6 | `concurrency_audit.py`, `track_B.json` |
| `G-01`…`G-15` | WP-6 (все 15) | — | V8 evidence по каждому |
| `M1`…`M8`, `Step −1…9`, `ADR-001…029` | EX-2, EX-10 | `Step −1` процедура → AS-7; ADR-004/007 → WP-11/GB-6 | `migration.json`, `ADR.md` |
| `P0-*`…`P2-*` (`PERFORMANCE.md`) | W3/W4a через G-10 | `P1-17` (`packed_eflags`, класс C PM-10) → **явно исключён** до перелифта (WP-11); оптимизации не ранжируются статикой (PM-17) | `changeset.sha256` |
| BACKLOG `D1`, `G1`, `G7`, `G12` | WP-11, C1, AS-1/E9 | `G12` (онбординг ~1,5 ГБ MPQ, gamepad/`WM_CHAR`) → **W3/C** | `asset_api_test`, T2 |

**Итог индекса:** 12 семейств id, 8 групп требований впервые получили
владельца и волну (`E5/E6/E7/E9/E10/E12`, `ABI-7/8`, `R5/R11`, `BP-7.5`,
`TA-15/17`, `PM-23/24`, `G12`, `H9`). Ни один id не остался без ответа.

---

## AS-2. Реконсиляция in-flight работы (наблюдено 22:47→23:23)

Статус пакета расширяется: `pending → in_flight → in_progress →
{done|blocked}`; `in_flight` = код уже пишется другим процессом, план его
**не перезапускает**, а требует переиздания evidence под своим владельцем.

| Артефакт (факт) | Статус | Действие W0.1 | Владелец / evidence |
|---|---|---|---|
| `kernel32/time` + shim, коммит `6055be0` | `done` | не переделывать; записать evidence в M5 | B / `track_B.json` |
| `kernel32/process` (`process_hle.c`, тест) | `in_flight` | принять как есть, прогнать EX-3 кейсы, закоммитить | B / `hle_process` |
| `kernel32/memory` (`memory_hle.c`, тест) | `in_flight` | то же; проверить `guest_ok`/арены (E6) | B / `hle_memory` |
| `runtime/platform/headless/*` (9 файлов) | **`done`, `15f6b5c`** | не переделывать; сверить 15 API/детерминизм и переиздать evidence A2 | A / `platform_headless_test.c` |
| `guest_modules.*`, `corpus_binding_test.c`, `platform_headless_test.c` | **`done`, `15f6b5c`** | принять в реестр модулей/корпуса, evidence по V1.6 | B+A |
| `config/schema/*`, `tools/check_config_schema.py` | **`done`, `6f10452`** (R10 закрыт по схемам) | принять в W0.2/M3, переиздать evidence M3 | интегратор |
| `msvcr80/`, `cmake/Wc3ReliftedCorpus.cmake`, `closure2.c`, `splitcorpus.c` | `in_flight` | проверить на M8-пин (`check_upstream.py`), не ломать compiled C-конвейер | A+B / `upstream_pin` |
| 7 `headless_*.o` в корне (R3) | **устранено писателем к 23:23** | закрытый инцидент; R2-allowlist на будущее | интегратор / `baseline_w0.json` |

`HEAD`: `6055be0` → `15f6b5c` (23:26) → `6f10452` (23:36); untracked
5 → 9 → 12 → 15, число исходников в T0-скане 137 → 160. Правило: **все счётчики выводятся во время
прогона** (EX-0.1/EX-12/AS-17), `in_flight` не становится `done` без
переизданного отчёта по V1.6, а `migration.json` M5 остаётся
`in_progress` с `evidence: null` до W0.1.

---

## AS-3. Запрос на изменение контракта (CCR)

Любая правка файла из §3.3 хаба/EX-1 требует CCR до коммита:

```
{ccr_id, path, contract_sha256_before, proposed_sha256, reason,
 requested_by, wave, abi_impact: none|cpu|dispatch|layout|registry,
 relift_required: bool, tests:[...], approver, decided_at, decision}
```

Порядок: (1) трек публикует CCR в `track_X.json`, не правя файл; (2) интегратор
классифицирует по ABI-1/2/9 — смена полей `CPU`, сигнатур `dispatch*`,
thunk-окна, формулы стека или баз = **перелифт и новый `corpus_sha256`**,
остановка всех треков; (3) правка делается **только в точке S*** одним
коммитом вместе с V2/V10 при изменении статуса гейта (V1.7); (4) пересчёт
`contracts_sha256`, уведомление треков, повтор `check_contracts.py`.
Добавление поля в конец `CPU` и правки сгенерированного C через LiftPass
CCR не требуют (ABI-1), но проходят G-15.

---

## AS-4. Матрица откатов (не только S*)

| Уровень | Механизм отката | Условие | Что **не** делается |
|---|---|---|---|
| Модуль M5 | не вызывать `wc3_*_install()` → остаётся as-built строка реестра; `wc3_legacy_hle_override` обратим до `frozen=1` | T0/T1 модуль красный | не удалять `runtime/legacy/*`, не править карантин |
| Пакет A (DUT) | откат к предыдущему `identity.json`/`normalization.json`, отключение расписания TA-12, `unavailable` вместо `pass` | self-test V5.4 не ловит порчу | не перезаписывать эталон G |
| Пакет C (графика) | переключение селектора на другой DXVK (GB-4.3 `dxvk_shim_reset()`), возврат предыдущего APK; при отказе обоих — числовой startup code (GB-4.7) | device matrix красная | не добавлять новый WSI-патч сверх kill criterion GB-6.3 |
| Трек | откат своей патч-серии до `rollback_to` из пакета задания (WP-9) | 2 провала чека подряд | не трогать чужие зоны R18 |
| Точка S* | полный откат до предыдущего `roadmap_status.json` (WP-8) | конфликт слияния/дрейф контрактов | не «ручной resolv» и не выборочный revert |
| Corpus/LiftPass | восстановление `changeset.json` + перегенерация тем же `lift_rev`; при смене ABI — перелифт (ABI-9) | G-15 `fail` | не применять pass без G-14→G-15 |

Общее: старый файл удаляется только после прохождения новым того же гейта
(H6.1); любой откат фиксируется в отчёте трека с `artifact_sha256`.

---

## AS-5. Каскад инвалидации хэшей (V5.7/BP-6/EX-12)

| Изменение | Инвалидирует | Что пересчитать | Следствие для вердикта |
|---|---|---|---|
| Поле `CPU`/сигнатура `dispatch*`/база/thunk-окно/формула стека | `corpus_sha256`, `layout_sha256`, `identity_sha256` | перелифт, `lift_rev`, layout, identity (V3.4) | стоп треков; T1 = `unavailable` до нового baseline |
| Порядок/состав `HleEntry` | `registry_sha256` | реестр, thunk-адреса, отчёт V5.7 | повтор T1 по всем расписаниям TA-12 |
| Тело HLE-модуля без смены таблицы | ни один из пинов | только `track_B.json` | T1 повторяется, baseline остаётся |
| `wc3_headless`/`platform/headless`/`gfx/null` | `headless_sha256`, `platform_contract_sha256`, `null_backend_sha256` | отчёт BP-6.2, хэши DUT в difftest | смена хэша без пересчёта baseline = инцидент (V5.7) |
| Сценарий/seed/ввод/caps/набор MPQ | `identity_sha256`, `schedule_sha256`, `scenario_sha256` | identity отдельным коммитом **до** кода (V3.4) | все `pass` аннулируются |
| Набор расписаний TA-12 | `schedule_sha256` | identity + прогоны difftest | агрегация по AS-6 |
| LiftPass / цепочка проходов | `changeset.sha256`, `summary.sha256` | G-15, per-function summary (TA-16) | L2 остаётся `missing` без summary |
| Toolchain/NDK/vendor | `toolchain.sha256`, отчёт BP-6.2 | `check_manifest.py`, ABI-совместимость | замеры G-10 = `report-only` |
| Любой frozen-файл | `contracts_sha256` | CCR (AS-3), `check_contracts.py` | стоп треков до S* |
| `HEAD` без нового снимка | `status_digest` | `roadmap_status.json`, `baseline_w0.json` | стоп по C6 |

Приоритет при нескольких изменениях: сначала identity/baseline (V3.4),
затем corpus/registry, затем DUT-хэши, затем отчёты. Частичное обновление
пинов запрещено.

---

## AS-6. Агрегация вердиктов (TA-12 × V5.2 × E11)

Правило приоритета: **`fail` > `unavailable` > `pass`** на всех уровнях.

1. Контрольная точка V5.2 (boot/menu/map/60 c/save/load/exit): `pass`
   только если совпали маркеры, нормализованные хэши guest memory,
   save/replay байты, хэш потока D3D9 **и** три инварианта V5.9 против G.
   Нарушение V5.9 даёт `fail` даже при совпавших хэшах (E11).
2. Расписание TA-12: каждый прогон независим; итог набора = `fail`, если
   хотя бы одно `fail`; иначе `unavailable`, если хотя бы одно `unavailable`;
   `pass` — только при `pass` **всех** расписаний.
3. Несколько захватов G:identity одинакова (V3.2); расхождение identity =
   инцидент V3.4, а не «усреднение».
4. Итог G-09 = агрегат по всем точкам и расписаниям; `unavailable` даёт
   ненулевой exit в acceptance (V1.3), но не блокирует разработку (ADR-029).
5. T2/GB-5 **не** участвует в агрегате T1: зелёный `gfx/null` ничего не
   говорит о DXVK (GB-7), `unavailable` устройства не понижает вердикт T1.

Выходной отчёт: `{identity_sha256, schedule_sha256, checkpoints[],
schedules[], per_check_verdict, aggregate_verdict, first_divergence}` в
`reports/generated/` по схеме V1.6.

---

## AS-7. Процедура снятия Step −1 / V5.9 baseline из O/G

1. Зафиксировать `capture_tool_sha256`, версию игры 1.27.1.7085 и
   `pe_sha256[]` лицензионных модулей; **рекомпилированный Windows-хост не
   собирается** (ADR-026), эталон — оригинальная игра и её захваты (V3).
2. Сценарий по E2: фиксированный seed, ввод в guest-тиках, разрешение,
   `caps_snapshot`, набор MPQ с sha256, config snapshot.
3. Снять маркерный поток и три инварианта V5.9
   (`locked_range_written`, `declared_range_consistency`,
   `degenerate_primitive_ratio`) на **исправленной** сборке (V6.6: baseline
   до исправления закрепляет устаревшее поведение).
4. Самопроверка V5.4: испорченный поток, сдвинутый байт save, изменённый
   маркер и пустой locked-диапазон обязаны дать `fail`.
5. Записать `oracle/identity.json` + `normalization.json` (V4.2); бинарные
   кадры/ассеты остаются на private runner (EX-8), в репозиторий идут хэши.
6. Назначить владельца Step −1 в V10 и регрессионный инвариант; только
   после этого A может выдать первый `pass` (E11).

---

## AS-8. Синхронизация двух статусных файлов

| Файл | Что хранит | Кто пишет | Правило |
|---|---|---|---|
| `config/migration.json` (R19) | M1…M8, evidence миграции | интегратор | `status: done` только с непустым evidence; M5 evidence собирается из `track_B.json` по модулям |
| `config/roadmap_status.json` (WP-1) | волны, треки, S*, `head_sha`, контракты | интегратор | состояние плана; не дублирует M-статусы, а ссылается на них |

Общий коммит в каждой S*: сначала evidence → затем `migration.json` →
затем `roadmap_status.json` → затем V2/V10 и `docs/README.md`.
`check_roadmap.py` (EX-12) сверяет пару и запрещает расхождение M5 ↔
`tracks.B.status`. Ручная правка любого из файлов треком = нарушение R18.

---

## AS-9. Реестр анти-паттернов проекта (консолидированно)

| Инцидент / норма | Как проявляется | Охрана в карте |
|---|---|---|
| F2: числа из текста (63 292 → 191 650) | снимок устарел за минуты | EX-0.1, EX-12, AS-2: считать во время прогона |
| B2/B6/B7: параллельные правки ломают T0 | `file_layout`/`platform_contract` красные | W0.2 R18, W0.1 `single_writer`, WP-8 worktree |
| 5 `sha256 drift` в `smoke.py verify` | baseline не совпадает с фикстурами/документами | W0.1, AS-5 каскад, V3.4 отдельный коммит |
| R3: 7 `headless_*.o` в корне | продукты сборки вне `build/` | EX-8, W0.1 R2-allowlist (устранено к 23:23) |
| «Уже собрано» как аргумент за dual DXVK (GB-7) | `provisional` выдают за `accepted` | WP-11, EX-10, GB-5/GB-6 |
| Объявленный гейт без проверки (V1.3) | `not implemented yet` при `declared` | WP-6, EX-7, `check_roadmap.py` |
| `unavailable = pass` (V1.2) | ложный зелёный T1/T2 | AS-6 приоритет вердиктов |
| Baseline до исправления (V6.6) | все шаги зелёные, дефект закреплён | AS-7, V5.9 против G |
| Наивная девиртуализация `dispatch_jmp` | `g_disp_depth` ломает host-вызовы | W4a: G-14 → G-15 → pass |
| no-op waits (`hle_android.c:2096–2110`) | MPQ-чтения сломаны | EX-3 `sync`, 36 записей, ADR-008 |
| Ослабление чека ради шага (V1.7) | allowlist без владельца/срока | W0.4, EX-7 п.4 |

---

## AS-10. Границы области: что карта НЕ решает

* Не отменяет и не заменяет нормативные документы-владельцы и ADR; не вводит
  новых `G-xx`/ADR/id; не является спецификацией точности (E1–E12).
* Не проектирует LAN/Battle.net (`NetApi` отсутствует, ADR-015) и 1.26/D3D8.
* Не допускает `--target windows`, OpenGL/GLES, `native_vk`, `hle_gl`,
  `D9VkApi`, внешние `d3d9.dll`/`d3d8.dll` и публикацию ассетов/PE/C
  (ADR-006/026, R6/R8).
* Не принимает shipping-решение по dual DXVK до GB-5 (ADR-004/007
  `provisional`); не объявляет бюджетов производительности до замеров (PM-2).
* Не применяет LiftPass и `packed_eflags` (класс C PM-10) до G-14/G-15 и
  перелифта соответственно.
* Не обещает T2 при отсутствии устройства: `unavailable ≠ pass`.
* Отложено в rev 3 / BACKLOG и остаётся за владельцами областей: SEH (A5),
  онбординг gamepad (G12 — назначен W3/C в AS-1), `DisplayApi` (D1 — WP-11),
  сериализация guest state (G1 — AS-1/E9), безопасность `.text` (E4).
* Product build на публичном CI-runner запрещён (ADR-006/013, EX-6).

---

## AS-11. Метрики процесса (только из артефактов)

Счётчики публикуются в `reports/generated/roadmap/process.json` по V1.6 и
служат **диагностикой, а не бюджетом**: пороги не объявляются до первого
замера (PM-2/PM-9), `report-only` по ADR-029.

`{head_sha, snapshot_at, packages_total, packages_in_flight,
packages_blocked, s_points_total, s_rollbacks, contract_drifts,
r3_violations, gate_promotions[], gate_demotions[], red_gates_open[],
red_gate_age_commits[], evidence_missing[], last_s_point}`.
Источник — `track_*.json`, `roadmap_status.json`, `gate_status.json`,
`check_roadmap.json`; числа в прозе плана не являются источником (EX-0.1).

---

## AS-12. Полнота и пересмотр самого плана

План считается полным, когда: (1) каждый id из AS-1 имеет владельца, волну и
evidence; (2) все 15 гейтов WP-6 имеют целевой статус и команду; (3) DAG
EX-1 не имеет висячих `depends_on`; (4) все четыре документа зарегистрированы
в `docs/README.md` и короче 380 строк; (5) `check_roadmap.py --strict`
зелёный; (6) `in_flight` из AS-2 либо принят, либо явно помечен `blocked`.

Владелец плана — интегратор. Пересмотр обязателен в каждой точке S* и при
любом триггере WP-12/EX-11/AS-17; правка четырёх документов одним коммитом
вместе с `roadmap_status.json`. Порядок чтения: хаб → WP → EX → AS →
нормативные владельцы (приоритет в обратную сторону).

---

## AS-13. Роль интегратора и право решения (RACI)

Слово «интегратор» использовалось в трёх документах, но роль не была
определена — это пробел четвёртого прохода.

| Решение | Отвечает (A/R) | Согласует (C) | Уведомляются (I) |
|---|---|---|---|
| Заморозка/изменение контракта | интегратор (CCR AS-3) | владелец области | все треки |
| Слияние в S*, rollback | интегратор | владельцы A/B/C | все |
| Перевод статуса гейта (V8) | интегратор | reviewer (не автор пакета) | V10 |
| Выбор DXVK (GB-6, ADR `supersedes`) | владелец проекта | Step 7/9, трек C | все |
| Перелифт (ABI-9) | интегратор + владелец ABI | — | стоп всех треков |
| Приёмка `in_flight` (AS-2) | интегратор + владелец трека | активный писатель | все |

`reviewer` в evidence V8 обязан быть не автором принимаемого пакета и
подтверждать `artifact_sha256`; при отсутствии независимого ревьюера статус
гейта не повышается. Если интегратор недоступен, **никто автоматически не
становится интегратором**: треки продолжают свои пакеты, но S* не открывается
(fail-safe). Заместитель назначается владельцем проекта явной записью в
`roadmap_status.json`.

## AS-14. Владение валидатором (исключение из W0.9)

`tools/check_roadmap.py` реализован как pilot четвёртого прохода **до**
появления `config/file_owners.json`. Это единственное исключение из правила
«новый файл в `tools/` без владельца не создаётся»: владелец зафиксирован
здесь (интегратор), после W0.2 файл получает запись в `file_owners.json`.
Чек **не добавляется в `run_tests.py` до W0.9**: иначе T0 станет краснее без
причины, что противоречит V1.7. Текущий вердикт честно `unavailable`
(exit 2, 0 errors/warnings, 7 отсутствующих W0-артефактов).

## AS-15. Протокол коммуникации между треками

Сообщение трека пишется в `track_X.json`, а не в `docs/**`:
`{from, to, wave, subject, contract_sha256, files_affected[],
needs_decision: bool, deadline_S}`.
Прямая правка чужой зоны R18 запрещена; запрос на изменение общего файла —
только CCR (AS-3). Эскалация: трек → интегратор → CCR → решение в S*.
Частота: один обмен на пакет и обязательный обмен перед каждой S*; трек,
не приславший `contract_sha256` перед слиянием, к S* не допускается.

## AS-16. Версионирование плана и правило консолидации

`plan_rev = 4` (хаб + WP + EX + AS). **Дальнейшее расширение идёт внутрь
существующих владельцев, а не через пятый документ**: ~1500 строк уже несут
риск F2/B2, который сам план считает главным инцидентом. Пятый документ —
только решением владельца проекта с `supersedes` и обновлением индекса,
`check_roadmap.py`, `roadmap_status.json` одним коммитом. Любая правка плана
сопровождается `check_roadmap.py --strict`; `plan_rev` растёт лишь при смене
структуры.

## AS-17. Ре-базелайнинг «план ↔ реальность»

Триггер: новый `HEAD`, изменившийся `status_digest`, новый вердикт T0.
Порядок: (1) `check_roadmap.py --strict` фиксирует `head_sha`/`status_digest`;
(2) правятся **только строки-снимки** (хаб §1.3, EX-0.1, AS-2, WP-12) —
стратегия, DAG и нормы не переписываются; (3) `in_flight → done` только с
переизданным evidence; (4) один коммит, `plan_rev` не меняется.

Наблюдение 23:26: коммит `15f6b5c` («Add module registry, corpus binding,
and headless platform») перевёл `platform/headless/*`, `guest_modules.*`,
`corpus_binding_test.c`, `platform_headless_test.c` в `done`; появился
`config/schema/` (warning R10 частично снят); новые `in_flight` —
`runtime/hle/msvcr80/`, `cmake/Wc3ReliftedCorpus.cmake`,
`closure2.c`/`splitcorpus.c`. Следующий коммит `6f10452` (23:36) закрыл
`config/schema/*` и `tools/check_config_schema.py` (M3/R10). Ошибка R3
устранена; `config/migration.json` M5 по-прежнему `in_progress` с
`evidence: null`, coverage 404/50, две ошибки R4 предсуществующие.

**Правило AS-17.1:** любое статусное утверждение в прозе четырех документов
имеет метку времени и является **историей замера**; текущее состояние
берётся только из `check_roadmap.json` (`head_sha`, `status_digest`,
`unavailable[]`) и `git status`. Переписывать прозу при каждом коммите
запрещено — это ровно анти-паттерн F2/AS-9: обновляются только строки,
перечисленные в п. (2), и только в точке S* или при `plan_rev`.

## AS-18. Операционная карточка и архивация плана

Ежедневный минимум перед правкой кода:
`git rev-parse --short HEAD` → `python tools/check_roadmap.py --strict` →
`python tools/run_tests.py` → `python tools/check_docs.py --strict` →
`python tools/check_file_layout.py --strict` → сверить `docs/README.md` →
при красном гейте действовать по EX-7 → перед слиянием проверить AS-15 →
в S* обновить `migration.json` и `roadmap_status.json` (AS-8).
Числа из прозы не переписывать вручную: их источник — отчёты.

После BP-4.4 и V9 план переносится в `docs/history/`, индекс обновляется,
`roadmap_status.json` получает `retired_at`; `check_roadmap.py` остаётся
T0-чеком целостности нормативных ссылок, а не гейтом продукта.

---

## AS-19. Трассировка четвёртого прохода

| Вопрос | Документ |
|---|---|
| Стратегия, срез, 3 трека, W0–W4 | `ROADMAP_PARALLEL.md` §1–§8 |
| WBS, пакеты W0.1…W0.10, протоколы, риски | `ROADMAP_WORKPACKAGES.md` WP-0…WP-12 |
| DAG, тест-матрицы, CI, валидатор | `ROADMAP_EXECUTION.md` EX-0…EX-14 |
| Полнота норм, CCR, откаты, хэши, RACI, ре-базелайнинг | этот документ AS-0…AS-18 |
| Исполнимый самоконтроль плана | `tools/check_roadmap.py` → `reports/generated/roadmap/check_roadmap.json` |
| Нормы (приоритет выше всех четырёх) | `docs/README.md` и владельцы областей |
