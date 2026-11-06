# Сборка: самостоятельность target-дерева и воспроизводимость

Статус: **норма, rev 1** (2026-11-06), ADR-025. Нормативный владелец
`docs/ARCHITECTURE_1.27B.md` §9.1/§9.2 в части build pipeline,
воспроизводимости, CI и release hygiene.

Связанные: `docs/VERIFICATION.md` (V1/V2/V6/V7, G-02/G-05/G-12/G-13),
`docs/GUEST_ABI.md` (ABI-3), `docs/EXACTNESS.md` (E7), `docs/GRAPHICS_DECISION.md`
(GB-5.1), `docs/HLE_MODULES.md` (H8), `docs/FILE_LAYOUT.md` (R2/R3/R9),
`docs/BACKLOG.md` (E2), `PLAN_REFACTOR_1.27B.md` Step 8/9.
Идентификаторы `BP-n` стабильны.

**ADR-026/027 (граница цели).** Единственный **поставляемый** target —
Android arm64-v8a: рекомпилированный код под Windows не собирается, не
линкуется и не поставляется. Разрешён единственный **верификационный**
target `--target headless` (V4.7) — исполнимый DUT T1, не входящий в APK.
Эталонные захваты снимаются с оригинальной лицензионной игры на Windows без
пересборки; `wc3_host27*.exe` старого дерева — исторический готовый
артефакт, не цель сборки. Поэтому BP-3.1, BP-4.4 и BP-11 требуют сборки
**только Android-продукта** как поставляемого артефакта.

**Причина появления.** ADR-020 объявляет новое дерево каноническим target, но
оно **не может собрать собственный продукт**: orchestrator отсутствует, а §9.1
прямо предлагает вызывать PowerShell-конвейер старого дерева. BACKLOG E2
называет это «скрытой междеревянной связностью» (P1), R14 фиксирует разные
git-контексты. При этом per-step гейт ADR-020 требует собирать оба дерева, а
ни один гейт сегодня не собирает ни одного (G-13 = `missing`). Проверка
2026-11-06 подтвердила: в `tools/` нового дерева 15 `.py` и 1 `.c`, нет каталога
apps, нет android-скриптов, нет `wc3build.py`, `check_manifest.py`, генератора
thin bridge и release-hygiene теста. Hub §9.1 при этом утверждает, что
`check_manifest.py` «проверяет версии» — это `declared`, а не `enforced`
(V1.3).

## BP-1. Проблема одним предложением

Канонический target не является самодостаточным: его сборка — это неявный
вызов скриптов из дерева, которое по плану должно быть когда-то удалено.
Ни один документ не определяет, **когда strangler-cutover считается
завершённым**; BP-4.4 даёт этот измеримый критерий.

## BP-2. As-built инвентарь (проверено 2026-11-06)

| Факт | Значение |
|---|---|
| Инструменты нового дерева | 15 `.py` + 1 `.c`; нет каталога apps, нет android-скриптов |
| Отсутствующие точки входа | `wc3build.py`, `check_manifest.py`, `gen_d3d9_thin_bridge.py`, `release_hygiene_test` (G-12/G-13) |
| Старое дерево | 208 файлов в `tools/`, 15 PowerShell-скриптов (`build_boot_probe.ps1`, `build_dxvk_probe.ps1`, `common.ps1`, `build_gameactivity_*.ps1`, …) |
| Скрытая связь | `link_apk.rsp` создаётся одним скриптом и читается другим; списки источников дублируются; сборка зависит от порядка запуска и stale-объектов |
| Диспетчеры | 6 вариантов в `build27/` плюс 4 в `build/` (R5 — только `gen_dispatch.py`) |
| Windows-эталон | оригинальная игра + зафиксированные захваты; `wc3_host27*.exe` — исторические артефакты старого дерева, **не пересобираются** (V3.5, ADR-026) |
| Размер `.so` | 216 МБ (ARCHITECTURE) против ~253 МБ (ANDROID.md старого дерева) — GB-5.1 |
| Git-контекст | новое дерево живёт внутри родительского `.git` (R14) |

## BP-3. Принципы

* **BP-3.1** Target-дерево обязано собирать **только Android-продукт**
  (`libwc3gameactivity.so`), **не исполняя скрипты старого дерева во время
  сборки**. Дополнительно разрешён `--target headless` — верификационный DUT
  T1, который не упаковывается в APK и не поставляется (ADR-027, V4.7.4).
  Никакой рекомпилированный Windows-бинарь не является целью сборки
  (ADR-026).
* **BP-3.2** Все внешние входы пинуются хэшем: toolchain (NDK/SDK/build-tools/
  Gradle/CMake/Python), vendor-коммиты, PE, входы лифта, порядок реестра.
  Неявных абсолютных путей в сборочных сценариях быть не должно.
* **BP-3.3** Старое дерево во время cutover — **поставщик входов только для
  чтения**, а не хост сборочного процесса. Сборка нового дерева не пишет в
  старое.
* **BP-3.4** Один генератор на артефакт; все продукты — только в `build/`
  (R2/R3); сгенерированный корпус не коммитится (ADR-006).
* **BP-3.5** `unavailable ≠ pass` (V1.2): отсутствующий инструмент — это
  падающий гейт, а не «пропущено»; `declared` не считается зелёным (ADR-022).

## BP-4. Граница вендоринга и конец cutover (E2)

* **BP-4.1** Android-сборочные скрипты старого дерева переносятся в новое
  дерево как vendor-код с `VENDOR_COMMIT`, `VENDOR_MANIFEST.md` и LICENSE
  (R9). Переписывать проверенный NDK/PowerShell конвейер **запрещено** —
  переносится он, а не заменяется (Step 8).
* **BP-4.2** До переноса обязателен файл `build_upstream.json` в каталоге config с полями
  `{path, revision, sha256, allow_unpinned}`. `allow_unpinned: true` и
  неразрешимая ревизия = ошибка гейта, а не предупреждение.
* **BP-4.3** Кэш корпуса ключуется `corpus_sha256`; попадание в кэш
  **проверяется** по хэшу входов, а не принимается на веру (BP-6).
* **BP-4.4** **Критерий завершения ADR-020:** чистая выгрузка нового дерева
  собирает `libwc3gameactivity.so` без обращения к старому дереву на этапе
  сборки, с зелёными T0+T1 (Windows-эталон не собирается, ADR-026). Пока это не выполнено, cutover не
  завершён и старое дерево не удаляется (§2.3). Это первое измеримое
  определение конца миграции.

## BP-5. Контракт `wc3build.py`

```
wc3build.py corpus --target android [--clean]
wc3build.py build  --target android [--clean] [--profile release|debug]
wc3build.py build  --target headless [--clean] [--profile release|debug]
wc3build.py check  --target android [--json <report>]
wc3build.py check  --target headless [--json <report>]
```

* **BP-5.1** Оркестратор генерирует списки источников и манифесты (`corpus`,
  `layout`, `link_apk.rsp`) и вызывает проверенный конвейер; он не хранит
  вторую копию списка источников (устранение долга BP-2).
* **BP-5.2** `link_apk.rsp` имеет ровно одного генератора и ровно одного
  потребителя; повторный `build` без изменений — no-op (проверяется
  acceptance Step 8). Порядок запуска скриптов не влияет на результат.
* **BP-5.3** Коды возврата: `0` pass, `1` fail, `2` unavailable; `--json`
  пишет отчёт по схеме V1.6. Молчаливый `exit 0` при непроверенном
  инварианте запрещён.
* **BP-5.4** Release-сборка не читает `getenv`; debug-переключатели — только
  под `#if WC3_DEBUG` (§9.3).
* **BP-5.5** `--target headless` собирает **только верификационный DUT**
  `wc3_headless` (V4.7, ADR-027): тот же корпус/HLE/app, `platform/headless`
  + `gfx/null`, без APK, без DXVK, без Windows. Он не входит в поставляемый
  link graph; `--target windows` запрещён для любого артефакта (ADR-026). Отчёт по BP-6.2
  обязан нести `target: headless` и хэши DUT для V5.7. **Статус: `missing`**
  (G-09/G-13) — контракт определён, оркестратора ещё нет.

## BP-6. Идентичность и воспроизводимость

| Хэш | Что покрывает | Владелец |
|---|---|---|
| `corpus.sha256` | фактические входы лифта, включая перебазированные PE | ABI-3.1, G-05 |
| `registry_sha256` | упорядоченный список HLE-записей | ABI-3.3, H3.1 |
| `layout_sha256`, `lift_rev` | раскладка и версия генератора | §5.2/§5.4 |
| `changeset.sha256` | function-level change set LiftPass (`va`, pass, хэши до/после, причина) | ABI-11.2, G-15 (отсутствует) |
| `toolchain.sha256` | версии NDK/SDK/build-tools/Gradle/CMake/Python | `check_manifest.py` (отсутствует) |

* **BP-6.1** Одинаковые входы → одинаковые object/manifest hashes и
  отсортированный symbol set. **Bitwise-идентичное `.so` не является гейтом**
  (§9.1): timestamp и порядок архива допустимы.
* **BP-6.2** Отчёт сборки — `reports/generated/build.<profile>.json`
  (gitignored) с `{artifact, sha256, size, flags, toolchain, corpus_sha256}`.
  Именно он закрывает расхождение 216/~253 МБ (GB-5.1), а не ссылка на текст
  документа.
* **BP-6.3** `corpus_manifest.sha256` сейчас `null` (G-05 = `declared`), поэтому
  воспроизводимость корпуса **не подтверждена**; до заполнения хэша T1 не
  может считаться эталоном (V6.2).
* **BP-6.4** Инварианты сгенерированного C и change set создаются
  **генератором** во время лифта и пинятся `lift_rev`/`corpus.sha256`
  (ABI-11.2/11.3, G-15). T0 не сканирует 2,4 ГБ сгенерированного C регексом:
  corpus в asset-free контуре отсутствует, а на машине разработчика такой
  проход неподъёмен. G-01 проверяет рукописный `runtime/`, G-15 — продукт
  генератора по нормализованным телам.

## BP-7. Граф сборки, линковка и release hygiene

* **BP-7.1** Оба DXVK собираются всегда; symbol isolation по каждому member
  архива, `--whole-archive`, `--exclude-libs,ALL`, статический libc++.
  Активный бэкенд — по GB-3/GB-6, а не по флагу сборки по умолчанию.
* **BP-7.2** В link graph **APK** только thin bridge; **запрещённые** legacy
  D3D-фронтенды и внешние D3D-DLL отсутствуют. `--policy` проверяет
  **фактический link graph**, а не grep по исходникам (G-02 сейчас
  `declared`), и обязан доказать, что `platform/headless` и `gfx/null`
  присутствуют только в link graph `--target headless` и **отсутствуют** в APK
  (V4.7.5). Один и тот же frontend не может обслуживать оба target'а.
* **BP-7.3** Статические библиотеки — по §4.4; `dxvk_win32` — vendor-код с
  именным allowlist `--concurrency` (§9.3).
* **BP-7.4** Release: `-DWC3_DEBUG=0 -DNDEBUG`, нулевые подтеги, без
  debug-символов. `--release` применяется к `runtime/`, каталогу apps и
  сгенерированному C по preprocessed output **и бинарнику**; DXVK/third_party —
  по allowlist. `release_hygiene_test` обязателен (G-12 = `missing`).
* **BP-7.5** Продукт обязан иметь понятный отказ: числовые startup error codes
  и тексты в Java-ресурсах (G7, GB-4.7), минимальный числовой boot/crash
  descriptor без строк (D2). Release = `abort` без диагностики не является
  поставляемым поведением.

## BP-8. CI

| Job | Требует | Артефакт | Вердикт |
|---|---|---|---|
| T0 asset-free static | ничего, любой runner | docs/arch/layout/unit, `--policy`/`--concurrency`/`--release` | ADR-006/013: product build на публичном runner запрещён |
| T1 reference | лицензионный PE + lift, private runner | `wc3_headless` (headless-DUT, V4.7) + difftest против эталонных захватов G, identity V3; **ни APK, ни Windows-бинарь не собираются** | per-step гейт ADR-020 |
| T2 device E2E | устройство, self-hosted | APK + WSI/lifecycle/input/audio/DXVK matrix | `unavailable ≠ pass`, проект не BLOCKED |

* **BP-8.1** Acceptance-прогон V7 не может быть зелёным, пока G-13
  (`wc3build.py`) = `missing`: ни T1, ни T2 нечем собрать (V6.1, V7.3).
* **BP-8.2** Каждый job публикует отчёт BP-6.2; `unavailable` даёт ненулевой
  exit при обязательном гейте (V1.3).
* **BP-8.3** T1-сборка идёт **только** через `--target headless` (BP-5.5) и не
  требует Android SDK/NDK, устройства или adb. APK собирается отдельным
  `--target android`; смешивать два job'а запрещено: product build на public
  runner нарушает ADR-006/013, а difftest без исполняемого DUT — V4.7.

## BP-9. Статусы и трассировка

Документ не вводит новых `G-xx`: все требования трассируются на существующие
гейты V2, чтобы не раздувать матрицу без исполнителя.

| Норма | Гейт / шаг | Статус 2026-11-06 |
|---|---|---|
| BP-4 (вендоринг, upstream pin) | G-13, Step 8 | `missing` |
| BP-5 (`wc3build.py`) | G-13, Step 4/8 | `missing` |
| BP-6 (`corpus.sha256`, toolchain) | G-05, G-13 | `declared` / `missing` |
| BP-7.2 (link graph) | G-02 | `declared` |
| BP-7.4 (release hygiene) | G-12 | `missing` |
| BP-8 (CI) | V7 | `declared` |
| BP-4.4 (конец cutover) | Step 9 | не определён до этого документа |

## BP-10. Владение и порядок

| Шаг | Действие |
|---|---|
| Step 0 | `build_upstream.json`, toolchain manifest, `check_manifest.py`; снятие противоречия размера `.so` |
| Step 4 | `wc3build.py check --target android`, единый генератор `link_apk.rsp` |
| Step 7 | вендоринг android/DXVK-скриптов с `VENDOR_COMMIT` |
| Step 8 | полная сборка, CI, `release_hygiene_test`, `--policy` по link graph |
| Step 9 | проверка BP-4.4 и удаление зависимости от старого дерева |

## BP-11. Definition of done

Чистая выгрузка нового дерева: `wc3build.py build --target android`
производит APK, `build --target headless` — исполняемый DUT T1 (V4.7);
повторный вызов каждого — no-op, `check --target android` и
`check --target headless` зелёные; в сборочных сценариях нет путей старого
дерева; T0 проходит без PE и lift; T1 — с лицензионным PE, зафиксированным
identity эталонных захватов и difftest'ом на H (без Windows-сборки, без
устройства); T2 — при доступном устройстве, с APK; отчёт BP-6.2
воспроизводим для обоих target'ов; R2/R3 чистые; G-02/G-05/G-09/G-12/G-13
переведены в `enforced`.

До BP-4.4 утверждение «новое дерево — канонический target» остаётся
декларацией, а ADR-020 не имеет измеримого конца.
