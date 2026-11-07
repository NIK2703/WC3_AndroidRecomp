# Варианты сборки: release / debug и теги диагностики

Статус: **норма, rev 1** (2026-11-07), ADR-030. Нормативный владелец области
«варианты сборки, имена пакетов, отладочные теги». При расхождении в вопросах
имён пакетов, вариантов и тегов приоритет у этого документа, затем у
`docs/ADR.md`, затем у хаба `docs/ARCHITECTURE_1.27B.md` §9.3. Владельцы
смежных областей не дублируются: `docs/BUILD_PIPELINE.md` (BP, конвейер),
`docs/VERIFICATION.md` (V/G, гейты), `docs/FILE_LAYOUT.md` (R, дерево),
`docs/GUEST_ABI.md` (ABI, граница корпуса).

Связанные: ADR-014/021/025/026/027/029; BP-5/BP-7.4/BP-7.5/BP-12; G-01/G-12/G-13;
V4.7/V5.7/V9/V10; R4/R11/R15/R17/R18/R22; ABI-1/ABI-2/ABI-3; PM-1/PM-11; TA-3/TA-12.

**ADR-029 (game-first).** Гейты — диагностика, а не разрешение на работу. BV не
вводят новых `G-xx`: проверки ложатся на G-01/G-12/G-13. «Норма определена,
инструмента нет» фиксируется как `missing`, а не «пропущено» (V1.2/V1.3).

**Граница документа.** Здесь зафиксирована архитектура и точные целевые
значения. Конфиги Gradle, манифест, Java-пакет и сборочные скрипты на 2026-11-07
**ещё не приведены к этой норме** (BV-12); изменение исходников — отдельный шаг.

## BV-1. Контракт двух вариантов
**BV-1 (два варианта, одна норма).** Ровно два устанавливаемых варианта
продукта, различаемых `applicationId`. Третьего пакета нет: `WC3_DOGFOOD`
из §9.4 хаба не является вариантом и выводится из употребления (BV-8).
Версионные, профильные и «dogfood» сборки суть один из этих двух вариантов
с другим набором тегов, а не новый `applicationId`.

| Свойство | `release` | `debug` |
|---|---|---|
| `applicationId` | **`wc3.recomp`** | **`wc3.recomp.debug`** |
| `namespace` / Java-пакет | `wc3.recomp` | `wc3.recomp` (без суффикса) |
| Gradle buildType | `release` | `debug` |
| Задача | `assembleRelease` | `assembleDebug` |
| `applicationIdSuffix` | нет | `.debug` |
| `versionNameSuffix` | нет | `-debug` |
| APK | `wc3-release.apk` | `wc3-debug.apk` |
| `android:debuggable` | false (AGP) | true (AGP) |
| `WC3_DEBUG` | `0` | `1` |
| `NDEBUG` | определён | не определён |
| Теги `WC3_DEBUG_<ЦЕЛЬ>` | все `0` | только явно включённые `1` |
| `CMAKE_BUILD_TYPE` | `Release` | `Release` (BV-5) |
| Подпись | release-ключ | debug-ключ AGP |
| app-private dir | `/data/user/0/wc3.recomp` | `/data/user/0/wc3.recomp.debug` |
| external dir | `Android/data/wc3.recomp` | `Android/data/wc3.recomp.debug` |
| Метка в лаунчере | `WC3 recomp` | `WC3 recomp (debug)` |
**BV-1.1** Debug-вариант — **не** неоптимизированная сборка. Он сохраняет
`-O2`/Release-оптимизацию нативного кода и отличается только включённой
диагностикой; `-g` задаётся отдельным необязательным `WC3_DEBUG_SYMBOLS`.
Иначе дефект, проявляющийся в release, не воспроизводится в debug, а замеры
каденции guest-тиков (E3/TA-12) перестают быть сопоставимыми.
**BV-1.2** Только `release` является поставляемым артефактом (ADR-026).
Debug APK — средство разработки: не распространяется, не содержит
лицензионных ассетов и не участвует в GB-5/PM-бюджетах.

## BV-2. Имена пакетов, namespace и JNI
**BV-2 (суффикс меняет идентичность, не код).** Базовый `applicationId` и
`namespace` — `wc3.recomp`; debug получает суффикс через `buildTypes.debug`,
а не второй `applicationId` в `defaultConfig`:

```kotlin
android {
    namespace = "wc3.recomp"
    defaultConfig { applicationId = "wc3.recomp" /* ... */ }
    buildTypes {
        getByName("debug") {
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
        }
    }
}
```

* **BV-2.1** `applicationIdSuffix` не меняет `namespace`, R-класс, Java-пакет
  и имена JNI-символов. Поэтому JNI-привязки выводятся из `namespace`, а не из
  `applicationId`; жёстко зашитых строк вида `wc3.recomp.debug` в C-коде быть
  не должно. Текущий `host_shell.c` вызывает `GetStaticMethodID` по имени
  `setDiagnostics` через `GetObjectClass`, то есть не зависит от пакета —
  переименование Java-пакета безопасно. Будущие JNI-функции, связываемые по
  имени, обязаны использовать `Java_wc3_recomp_*` в обоих вариантах.
* **BV-2.2** Имя activity в манифесте — относительное (`.Wc3GameActivity`),
  чтобы слияние манифеста не зависело от суффикса. `android:debuggable` не
  прописывается вручную: его выставляет AGP для `debug`.
* **BV-2.3** Метка debug задаётся оверлеем `app/src/debug/` (ресурсы или
  манифест), а не условием в общем манифесте. `buildConfig = false`
  сохраняется: `BuildConfig.DEBUG` не источник истины, source sets — источник.
* **BV-2.4** Любой будущий `FileProvider`/`ContentProvider` обязан иметь
  authority с плейсхолдером `${applicationId}`, иначе два варианта не
  установятся рядом. Сейчас провайдеров нет.

## BV-3. Один атомарный вход вместо двух переключателей
**BV-3 (вариант не собирается из двух независимых флагов).** Единственный
вход `--variant release|debug` (в `wc3build.py` по BP-5 и в
`build_apk.ps1`) определяет **все** downstream-значения: задачу Gradle,
каталог native-сборки, `-DWC3_DEBUG`, набор тегов, каталог `jniLibs`, имя APK,
пакет для `adb install`/`am start` и пакет для device-тестов.

As-built (2026-11-07) рассогласован:

| Факт | Путь | Следствие |
|---|---|---|
| `applicationId = com.wc3.recomp` для обоих buildType | `apps/android_gameactivity/app/build.gradle.kts` | debug заменяет release при установке |
| `-Release` и `-NativeDebug` — два разных флага | `tools/android/build_apk.ps1` | вызов без флагов даёт `assembleDebug` с `WC3_DEBUG=OFF`: debug APK с release-нативным кодом |
| Один каталог `build/android_app/cmake` для всех вариантов | `tools/android/build_app.ps1` | stale `.so` предыдущего варианта попадает в APK |
| Один `-Pwc3JniLibs` на `main` | `tools/android/build_apk.ps1` | jniLibs не различаются по варианту |
| `-DCMAKE_BUILD_TYPE=Release` всегда | `tools/android/build_app.ps1` | native-профиль не связан с вариантом |
| `release.signingConfig = debug` | `apps/android_gameactivity/app/build.gradle.kts` | release подписан отладочным ключом |
| `com.wc3.recomp` в трёх инструментах | `tools/android/build_apk.ps1`, `tools/android/run_app_test.ps1`, `tools/device_test.py` | запуск/тест бьёт в старый пакет |
| `DEFAULT_ACTIVITY = ...Wc3Activity` | `tools/device_test.py` | не совпадает с `.Wc3GameActivity` манифеста |
| `/data/data/com.wc3.recomp/files` | `runtime/legacy/hle_android.c:3125` | фиксированный путь чужого пакета |

* **BV-3.1** Целевые каталоги: `build/android_app/<variant>/cmake`,
  `build/android_app/<variant>/jniLibs`, `build/android_app/<variant>/gradle`.
  Разделение обязательно и для кэша: макросы варианта входят в ключ ccache
  (BP-12), поэтому «тот же исходник, другой вариант» не имеет права попасть
  в кэш другого варианта.
* **BV-3.2** `-Pwc3JniLibsRelease` / `-Pwc3JniLibsDebug` задают `srcDir` у
  соответствующего variant source set, а не у `main`.
* **BV-3.3** Скрипт обязан печатать и проверять согласованность:
  `{variant, applicationId, WC3_DEBUG, tags, so_sha256}` попадает в отчёт
  сборки BP-6.2. Несовпадение — exit 1, а не предупреждение.
* **BV-3.4** Путь карантина read-only (R15): строка в `runtime/legacy`
  **не редактируется**. Замена — платформенный путь через `hle_legacy_shim`,
  куда его передаёт `android_app->activity->internalDataPath`, как уже делает
  `apps/android_gameactivity/src/main/cpp/host_shell.c`.

## BV-4. Сосуществование и изоляция данных
**BV-4 (два UID, общая лицензионная установка).** Разные `applicationId`
дают разные UID и app-private каталоги; оба варианта ставятся одновременно.
Лицензированные файлы игры намеренно читаются из одного каталога
`/storage/emulated/0/Warcraft 3`, поэтому `MANAGE_EXTERNAL_STORAGE` обязателен
на API 30+. Пока доступ не выдан, нативный host откатывается на app-private
каталог; `wc3.cfg`, `run.txt` и диагностика остаются app-private.

| Ресурс | Правило |
|---|---|
| MPQ/PE | общий лицензионный каталог `/storage/emulated/0/Warcraft 3`, выведенный из `externalDataPath`; app-private fallback, пока доступ не выдан |
| `wc3.cfg` | app-private state dir своего варианта |
| saves/replays | созданные игрой `Save/` и `Replay/` остаются в общем лицензионном каталоге, как в старом host; оба варианта видят одну установку |
| SAF URI-пермиссии | выдаются на UID: debug обязан запросить доступ заново, разрешение release не наследуется |
| trace/dump/crash-файлы | только app-private dir debug-варианта; `run.txt` и `WC3_DATA_DIR` указывают туда, а не в общий каталог |
| SharedPreferences/БД | app-private, автоматически разделены |
| logcat | общий системный буфер, поэтому тег обязан нести вариант и цель: `wc3/<variant>/<tag>` |
| `android:allowBackup` | `false` в обоих вариантах |
| authority/провайдеры | `${applicationId}` (BV-2.4) |
**BV-4.1** T2-проверка сосуществования: установить оба APK, убедиться, что
`pm list packages` содержит ровно `wc3.recomp` и `wc3.recomp.debug`, и что
запуск каждого стартует именно свою activity. Без этого сосуществование —
декларация.

## BV-5. Нативная конфигурация и единый источник макросов
**BV-5 (один INTERFACE-конфиг на все цели).** `CMakeLists.txt` задаёт
`WC3_VARIANT` со значениями `release|debug` (иначе `FATAL_ERROR`), из него
выводит `WC3_DEBUG=0|1`, `NDEBUG`, список тегов и `-DCMAKE_BUILD_TYPE`.
Один INTERFACE-таргет `wc3_build_config` с
`target_compile_definitions(... INTERFACE WC3_DEBUG=<0|1>)` линкуется во все
цели (`wc3_platform`, `wc3_guest`, `wc3_boot`, `wc3_hle_modules`,
`wc3_legacy_boot`, `wc3gameactivity`), чтобы макрос был **определён одинаково
в каждом TU**.

* **BV-5.1** Причина: сейчас `wc3_platform` публикует `WC3_DEBUG=0`, а
  `cmake/Wc3LegacyBoot.cmake` определяет `WC3_DEBUG=1` только для debug у
  части целей. В таком смешанном режиме `#ifdef WC3_DEBUG` истинен в release
  там, где макрос определён как `0`, — отладочный код физически попадает в
  продукт. Отсюда жёсткий запрет `#ifdef WC3_DEBUG` (уже ловится
  `check_arch.py --release`) и требование BV-5.
* **BV-5.2** Локальные `#ifndef WC3_DEBUG / #define WC3_DEBUG 0` в `.c`-файлах
  остаются как защита для автономной сборки юнит-тестов, но не являются
  источником варианта. Источник — только `WC3_VARIANT`.
* **BV-5.3** Теги активируются списком `WC3_DEBUG_TAGS` (например
  `GRAPHICS;HLE_SYNC`), который CMake превращает в `-DWC3_DEBUG_GRAPHICS=1`.
  В `release` список пуст принудительно, независимо от переданного значения.
* **BV-5.4** `WC3_USE_CCACHE=ON` и `WC3_REQUIRE_CCACHE=ON` сохраняются в
  обоих вариантах (BP-12); раздельные каталоги сборки BV-3.1 не отменяют
  launcher.

## BV-6. Реестр и таксономия тегов
**BV-6 (каноническое имя тега).** Тег записывается как `WC3_DEBUG_<ЦЕЛЬ>`.
Пример из постановки задачи `DEBUG_GRAPHICS` реализуется как
`WC3_DEBUG_GRAPHICS`. Основания: (1) мастер-выключатель `WC3_DEBUG` из
`AGENTS.md` обязателен, тег вложен в него; (2) префикс `WC3_` исключает
коллизии с `DEBUG`/`_DEBUG` NDK и vendor-кодом; (3) в карантине уже есть
`WC3_DEBUG_GRAPHICS`, `WC3_DEBUG_HLE_SYNC`, `WC3_DEBUG_HLE_SLEEP`,
`WC3_DEBUG_RESOLUTION`; (4) один namespace нужен grep-гейтам. Короткие имена
без префикса, алиасы и `*_ALL` запрещены.

Единственный реестр — заголовок `runtime/diag/wc3_debug.h`, названный §9.3
хаба (файл на 2026-11-07 отсутствует: норма `declared`, реализация
`missing`). Для каждого тега там ровно одна пара
`#ifndef WC3_DEBUG_X` / `#define WC3_DEBUG_X 0` / `#endif` и комментарий
с владельцем подсистемы; макросы активируются только через `-D` сборщика
(BV-5.3). Неизвестный тег в исходнике = ошибка T0-проверки.

| Тег | Цель трассировки | Владелец/слой |
|---|---|---|
| `WC3_DEBUG_BOOT` | старт, конфиг, lifecycle, этапы загрузки | `runtime/boot`, `runtime/app` |
| `WC3_DEBUG_ASSETS` | MPQ, SAF, Storm.dll, `AssetApi`, список ассетов | `runtime/platform`, HLE Storm |
| `WC3_DEBUG_MEMORY` | guest-арены, mmap, PE-образы, базы | `runtime/guest` |
| `WC3_DEBUG_CPU` | dispatcher, флаги, x87/SSE, снимки `CPU` | `runtime/cpu` |
| `WC3_DEBUG_HLE` | реестр, импорты, thunk, `stack_cleanup` | `runtime/hle/abi` |
| `WC3_DEBUG_HLE_SYNC` | события/мьютексы/семфоры/ожидания | HLE sync (миграционный) |
| `WC3_DEBUG_HLE_SLEEP` | слипы и каденция ожиданий | HLE sync (миграционный) |
| `WC3_DEBUG_THREADS` | guest threads, mailbox, расписания | `runtime/platform`, `runtime/ipc` |
| `WC3_DEBUG_GRAPHICS` | D3D9-bridge, WSI, device, кадр, fallback | thin bridge / DXVK |
| `WC3_DEBUG_RESOLUTION` | режимы экрана | миграционный алиас карантина |
| `WC3_DEBUG_VULKAN` | ICD, physical device, capabilities | `VulkanDriverApi` |
| `WC3_DEBUG_AUDIO` | Mss32/Miles, AAudio, PCM-dump | HLE audio |
| `WC3_DEBUG_INPUT` | касания, клавиши, mailbox ввода | `runtime/platform/android` |
| `WC3_DEBUG_SAVE` | saves/replays/registry эмуляция | HLE file/registry |
| `WC3_DEBUG_JASS` | JASS/скриптовый слой | рекомпилированный код |
| `WC3_DEBUG_JNI` | граница Java ↔ native, `setDiagnostics` | `apps` |
| `WC3_DEBUG_CRASH` | crash dump, breadcrumb (заменяет `WC3_DOGFOOD`) | `runtime/app` |
| `WC3_DEBUG_PERF` | счётчики PM-1 как диагностика | см. BV-13 |

* **BV-6.1** Тег выбирается по **цели наблюдения**, а не по каталогу файла:
  лог внутри HLE-модуля, исследующий поведение устройства, несёт
  `WC3_DEBUG_GRAPHICS`, а не `WC3_DEBUG_HLE`. Именно это и есть «тег цели».
* **BV-6.2** Один тег = одна подсистема. Трассировка двух подсистем — два
  отдельных guard'а; новый «объединяющий» тег не вводится.
* **BV-6.3** Три миграционных тега карантина сохраняются до завершения M5,
  чтобы не править read-only исходники (R15); новый код их не использует.

## BV-7. Правило обёртки отладочных инструкций
**BV-7 (обязательная обёртка).** Любая вводимая в код отладочная,
логгирующая или трассирующая инструкция обязана быть обёрнута в директиву
препроцессора **с тегом цели**: `#if WC3_DEBUG && WC3_DEBUG_<ЦЕЛЬ>`.
К диагностике относятся: `LOGI`/`LOGE`/`__android_log_print`/`fprintf`,
probes, dump-файлы, trace-записи, debug-счётчики, debug-only поля структур,
debug-only строки, debug env/`getenv`, trigger-файлы, генераторы dispatcher,
HLE name/calls и debug-only assert.

Каноническая форма:

```c
#include "wc3_debug.h"          /* мастер + нулевые значения всех тегов */

#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    WC3_TRACE_GFX("device=%p backend=%d", dev, backend);
#endif
```

* **BV-7.1** Инструкция считается обёрнутой в двух случаях: (a) она
  лексически лежит внутри `#if WC3_DEBUG && WC3_DEBUG_X`; (b) это вызов
  макроса, чьё **определение целиком** лежит внутри такого же `#if`, а в
  противном случае макрос раскрывается в `((void)0)`. Форма (b) допустима
  только для однострочных логов; блоки из нескольких инструкций и
  debug-only поля — только форма (a).
* **BV-7.2** Запрещены: `#ifdef WC3_DEBUG`, `#if DEBUG`, guard только по
  `NDEBUG`, нетегированный `#if WC3_DEBUG`, `if (debug)` и runtime-флаги
  диагностики, алиасы тегов, а также «один безобидный printf» без обёртки.
  В горячем пути запрещены аллокации, копирования, повторные lookup и
  любые проверки ради диагностики (`AGENTS.md`, BP-5.4).
* **BV-7.3** Debug-only assert оформляется отдельным макросом на тег
  (`WC3_DASSERT_GFX(expr)`), чьё определение лежит под тем же двойным
  guard'ом: имя тега нельзя передать в `#if` как аргумент макроса.
  `assert` как проверка контракта исполнения диагностикой не считается и
  живёт по `NDEBUG`.
* **BV-7.4** Java/Kotlin препроцессора не имеет: отладочный код целиком
  выносится в `app/src/debug/` (классы, ресурсы, манифест-оверлей), а в
  `main` не остаётся ни одного диагностического класса или строки.
  Debug source set — вариант того же модуля, а не второй source root (R17).
* **BV-7.5** Debug-only поля структур допустимы только в host-owned типах.
  `HleDebugEntry`/`HleRegistry` из `runtime/hle/abi/hle_abi.h` — пример
  разрешённого: это host-таблица, не guest-память. Поля, видимые гостю,
  не меняются ни при каком варианте (BV-9).

## BV-8. Что диагностикой не является
**BV-8 (продуктовый отказ — не debug-лог).** Исключения из BV-7 ровно три,
и все они обязаны присутствовать в release:

1. Числовые startup/crash-коды и минимальный числовой descriptor (BP-7.5,
   §9.4 хаба): продукт обязан понятно отказывать, `abort` без диагностики
   непоставляем.
2. Тексты ошибок в Java-ресурсах, сопоставленные числовым кодам (GB-4.7).
3. Явная диагностика неполного набора лицензионных MPQ на старте
   (`AGENTS.md` «Данные и верификация»).
**BV-8.1** Техническая панель `setDiagnostics` в
`apps/android_gameactivity/src/main/cpp/host_shell.c` — переходная: она
показывает внутренние состояния (`corpus`, `HLE`, `dispatcher`) и потому
в native-части уходит под `WC3_DEBUG_BOOT`, а Java-часть — в debug source
set; в release панель заменяют числовой код и ресурсный текст (BP-7.5).
Считать её «продуктовой диагностикой» нельзя.
**BV-8.2** `WC3_DOGFOOD` из §9.4 хаба упраздняется как отдельный вариант:
breadcrumb становится `WC3_DEBUG_CRASH` в debug, а в release остаётся только
числовой descriptor по BV-8.1. Третий `applicationId` не появляется.

## BV-9. Граница guest ABI и инвариант наблюдения
**BV-9 (диагностика наблюдает, а не меняет).** Вариант сборки не имеет права
менять замороженный guest ABI (ABI-1/ABI-2): поля `CPU`, сигнатуры
`dispatch`/`dispatch_jmp`, thunk-окно `0x7F000000` stride 16, формулу
`stack_cleanup`, базы образов, порядок HLE-реестра (`registry_sha256`,
ABI-3) и `corpus.sha256`. Иначе debug и release — разные корпуса, и difftest
теряет смысл.

* **BV-9.1** `registry_sha256` вычисляется по guest-visible части записей и
  обязан совпадать у обоих вариантов; debug-поля в хэш не входят.
* **BV-9.2** Инструментация наблюдательна: ни одного host lock вокруг
  `dispatch()`, обратного вызова в гость или ожидания guest-visible объекта
  (инвариант `AGENTS.md`, G-01, TA-3); никакого синхронного ввода-вывода в
  dispatcher; запись trace идёт через lock-free mailbox в debug-only поток.
* **BV-9.3** Интрузивные пробы (принудительные задержки, точки останова,
  подмена времени) разрешены только под своим тегом, явно маркируются в
  отчёте и **запрещены** в difftest и в детерминированных расписаниях TA-12.
  Иначе «баг не воспроизвёлся» окажется следствием трассировки.
* **BV-9.4** Канонический headless-DUT T1 собирается с `--profile release`
  (V5.7 пинит `headless_sha256`). Debug-DUT — отдельный артефакт с отдельным
  хэшем и не может служить baseline G/H. Debug APK используется только как
  T2-диагностика и не даёт данных для G-10/PM-11.

## BV-10. Release hygiene и исполнимая проверка
**BV-10 (release не содержит нашего debug-кода).** Release:
`-DWC3_DEBUG=0 -DNDEBUG`, все теги `0`, без debug-символов и debug-строк,
без HLE name/calls и генераторов dispatcher (BP-7.4, G-12).

Проверка делится на три уровня, все — расширение существующих G-01/G-12/G-13:

| Уровень | Что проверяет | Инструмент |
|---|---|---|
| Исходник | нет `#ifdef WC3_DEBUG`; каждый диагностический сайт имеет тег из реестра; неизвестных тегов нет | `check_arch.py --release` + T0-чекер тегов |
| Препроцессор | `-E` release-вывод не содержит наших лог-строк и `__android_log_print` | сборка release с `-E`, `--release` по preprocessed output |
| Бинарник/APK | нет debug-символов и debug-строк в `.so`; `aapt2 dump badging` даёт ровно `wc3.recomp`; debug — `wc3.recomp.debug`; `.so` в APK совпадает с отчётом BP-6.2 | `release_hygiene_test` (G-12) |
**BV-10.1** Чекер тегов и бинарный `release_hygiene_test` отсутствуют
(G-12 = `missing`); норма определена, реализация — работа Step 8.
DXVK/third_party идут по allowlist (ADR-014), к ним BV-7 не применяется.
**BV-10.2** Честный as-built статус на 2026-11-07: `check_arch.py --release`
возвращает 1 с четырьмя ошибками `#ifdef WC3_DEBUG` в карантинных
`runtime/legacy/*.c|h`; `check_file_layout.py --strict` уже красный по R3/R4.
Согласно ADR-029 это отчёт, а не запрет работы; V2 не переписывается задним
числом.

## BV-11. Подпись и распространение
**BV-11 (разные ключи, один продукт).** `release` подписывается выделенным
release-ключом; `debug` — debug-ключом AGP. Разные `applicationId` и разные
подписи исключают конфликт обновления. Текущая строка
`release.signingConfig = signingConfigs.getByName("debug")` в
`app/build.gradle.kts` — долг, а не решение: она годится только для локального
release-hygiene артефакта. Ключи не коммитятся; debug APK не публикуется и не
содержит MPQ/PE/сгенерированного C (ADR-006).

## BV-12. As-built и миграция
**BV-12 (порядок приведения дерева).** Норма не подменяется реализацией:
каждый шаг фиксируется отдельно, без big-bang (ADR-020).

| Шаг | Действие | Владелец |
|---|---|---|
| 1 | `apps/android_gameactivity/app/build.gradle.kts`: `applicationId`/`namespace` → `wc3.recomp`, `applicationIdSuffix`/`versionNameSuffix`, per-variant jniLibs, release-подпись | Step 8 |
| 2 | Перенос Java-пакета `com/wc3/recomp` → `wc3/recomp`, относительное имя activity в манифесте, оверлей метки в `app/src/debug/` | Step 8 |
| 3 | `tools/android/build_apk.ps1` и `tools/android/build_app.ps1`: один `-Variant`, раздельные каталоги BV-3.1, `-DWC3_DEBUG`, `WC3_DEBUG_TAGS`, отчёт BP-6.2 | Step 8 |
| 4 | `tools/android/run_app_test.ps1`, `tools/device_test.py`, `build_apk.ps1`: имя пакета/activity выводится из варианта | Step 8 |
| 5 | `runtime/diag/wc3_debug.h`: реестр тегов; `wc3_build_config` в `CMakeLists.txt` и `cmake/Wc3LegacyBoot.cmake` | Step 8 / M5 |
| 6 | Перенос отладочных сайтов карантина под BV-7 через `hle_legacy_shim`, замена фиксированного `/data/data/...` платформенным путём | M5 (H6.3) |
| 7 | Чекер тегов, `release_hygiene_test`, T2-тест сосуществования BV-4.1 | Step 8 (G-12) |
**BV-12.1** До шага 3 утверждение «собираем release и debug» — декларация:
сегодня Gradle-вариант и native-вариант независимы (BV-3). Шаги не
блокируются красными гейтами (ADR-029), но отчёт обязан это фиксировать.

## BV-13. Трассировка и открытые решения
**BV-13 (трассировка и открытые решения).** Новых `G-xx` нет; каждая норма
сведена к существующему владельцу гейта, а незакрытые оси измерений названы
явно. При росте документа сверх R4 владелец разделения — BV-6/BV-7
(отдельный документ реестра тегов), а не новый `G-xx`.

| Норма | Гейт / владелец | Статус 2026-11-07 |
|---|---|---|
| BV-1…BV-4 (пакеты, изоляция, атомарность) | G-13, Step 8 | `missing` |
| BV-5 (INTERFACE-конфиг, ccache по варианту) | G-13, BP-12 | `missing` (ccache = `enforced`) |
| BV-6/BV-7 (реестр и обёртка тегов) | G-01 `--release`, Step 8 | `declared` (`#ifdef` ловится, теги — нет) |
| BV-10 (release hygiene, бинарник) | G-12, Step 8 | `missing` |
| BV-11 (подпись) | Step 8 | `missing` (release подписан debug-ключом) |
| BV-2/§9.3 хаба | R22 | новый владелец зарегистрирован в `docs/README.md` и `tools/check_docs.py` |
**BV-13.1** Открытое решение: счётчики PM-1 — это измерение, а не лог, но по
BV-7 они тоже попадают под `WC3_DEBUG_PERF`. Такой замер идёт на debug-профиле
и **не может закрыть G-10** (PM-11 требует калибровки на оптимизированном
артефакте). Разделение оси «диагностика» и оси «измерение» требует отдельного
ADR, когда `WC3_PERF` появится в дереве; до него результат = `report-only`
(V6.7). Новый вариант сборки для этого не вводится.
**BV-13.2** Открытое решение: `CMAKE_BUILD_TYPE=Debug` с `-O0` допустим только
как локальный отладочный прогон с явным `WC3_DEBUG_SYMBOLS`, но не как
вариант BV-1 и не как источник эталонных хэшей V5.7.
**BV-13.3** Новые `G-xx`, ADR и статусы гейтов этот документ не вводит помимо
ADR-030; приоритет владельцев областей и нумерация §X хаба сохранены (R11).
