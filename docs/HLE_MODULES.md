# HLE: разбиение по guest DLL и границы переноса M5

Статус: **норма, rev 1** (2026-11-06), ADR-024. Нормативный владелец
разбиения HLE и границ переноса M5. Разрешает конфликт
`docs/ARCHITECTURE_1.27B.md` §4.2/§6.4 ↔ `docs/FILE_LAYOUT.md` R7.

Связанные: `docs/VERIFICATION.md` (V1/V2/V6), `docs/GUEST_ABI.md`
(ABI-3/ABI-8), `docs/EXACTNESS.md` (E8), `docs/THREAD_AUDIT.md`,
`docs/GRAPHICS_DECISION.md` (GB-8), `config/hle_table.1.27b.tsv`,
`PLAN_REFACTOR_1.27B.md` Step 5 и M5. Идентификаторы `H<n>` стабильны.

**Причина появления.** §4.2 рисует каталог win32 как плоский список
`file, memory, time, process, strings/locale, user32, gdi32, advapi32, winmm,
thread, sync` — подсистемы перемешаны с именами DLL. R7 требует «модули HLE
повторяют границы guest DLL». Согласно `docs/README.md`, приоритет у владельца
области, то есть у FILE_LAYOUT, — но ARCHITECTURE остаётся хабом с устаревшим
деревом, и формально R7 недействителен. M5 (перенос 10 333 строк
`hle_android.c`) собирается идти по противоречивому правилу. Дополнительно:
426 записей таблицы уже сгруппированы по DLL, а 2 009 PE-импортов не
классифицированы (`import_policy` отсутствует, R10/M3).

## H1. Разрешение конфликта

* **H1.1** Два уровня: **уровень 1 — guest DLL** (имя из PE import
  directory), **уровень 2 — подсистема внутри DLL**. Поэтому `kernel32/thread`
  и `kernel32/sync` из §6.4 корректны, а плоский список §4.2 — нет. Конфликт
  устраняется не выбором победителя, а фиксацией иерархии.
* **H1.2** Приоритет: этот документ → `FILE_LAYOUT.md` R7 → §4.2/§6.4
  ARCHITECTURE. **R7 отныне действителен**; §4.2 считается устаревшей сводкой
  до M2, §6.4 — рабочей таблицей подмодулей `kernel32`.
* **H1.3** `ws2_32`/`INTERNET` не образуют модуля: LAN/Battle.net вне области
  (ADR-015), `NetApi` отсутствует. Соответствующие импорты классифицируются
  как `forbidden-LAN` (H4).

## H2. Целевое дерево

```
runtime/hle/
  abi/                  HleEntry, registry, hle_call (общий, не по DLL)
  win32/
    kernel32/           memory/ file/ time/ process/ locale/ thread/ sync/
    user32/             окна, сообщения, ввод, clipboard
    gdi32/              DC, bitmap, font, blit
    advapi32/           registry (guest-local, app-private)
    winmm/              waveOut, timeGetTime
    msvcr80/            CRT, _beginthreadex, математика _CI* / libm_sse2_*
    imm32/ shell32/ comctl32/ version/ ole32/   минимально необходимое
  d3d9/                 guest COM D3D9 + thin bridge
  dsound/               guest COM DirectSound
  miles/                AIL_*, динамические .m3d-экспорты (late-bind, H8)
  display/              display mode / widescreen (§13 D1)
```
Один `HleModule` на DLL; подмодули уровня 2 — отдельные `.c` внутри каталога
DLL. Имя файла по R13: `<subsystem>_<role>.c`.

## H3. Источники истины и схема модуля

| Проверка | Источник |
|---|---|
| Presence | `config/pe_imports.1.27b.json` — 2 009 импортов, `pe_fixture_status=available` |
| Arity | `config/hle_arity.1.27b.json` + анализ call-site |
| Callconv | canonical Win32 spec + `config/cdecl_allowlist.1.27b.json` |
| Текущая таблица | `config/hle_table.1.27b.tsv` — 426 записей, уже по DLL |
| Классификация | import_policy.1.27b.json — **отсутствует**, долг M3/R10 |

* **H3.1** Каждый модуль содержит декларативную таблицу
  `{dll, name|ordinal, argc, callconv, flags, result_kind, arg_source, impl,
  user}`; порядок строк фиксирует `registry_sha256` (ABI-3.3).
* **H3.2** `presence(table)` не доказательство (ADR-012); `unavailable` при
  существующем PE — ошибка фикстуры. 295 stdcall / 131 cdecl, fastcall нет
  (§6.2.1).

## H4. Классификация 2 009 импортов

`hle_coverage.py --fixture-kind pe`: **1 051** покрыто / **76** excluded /
**832** unresolved / **50** forbidden.

| Состояние | Правило |
|---|---|
| `implemented` | есть запись с точным `argc` и обработчиком |
| `optional-unused` | документированное поведение, `HLE_OPTIONAL` с точным `argc` |
| `forbidden-LAN` | LAN/Battle.net, не реализуется (ADR-015) |
| `fatal-missing` | обязательный импорт без обработчика → fatal at startup (ADR-008) |

* **H4.1** Все 832 `unresolved` обязаны получить **модуль-владельца** по имени
  DLL **до** реализации: «не классифицировано» не может быть состоянием
  границы модуля. Владелец определяется H5, а не удобством переноса.
* **H4.2** 36 записей thread/sync (ровно 36 по таблице) относятся к
  `kernel32/thread` и `kernel32/sync`; это не «аудит потом», а обязательная
  часть M5 (`docs/THREAD_AUDIT.md`).
* **H4.3** Гейт G-03 обязан вызываться с `--require-hle`; текущий
  `run_tests.py` без флага даёт зелёный результат при 832+50 (V2).

## H5. Матрица модулей

| Модуль (уровень 1) | Подмодули | As-built источник (`hle_android.c`) | Platform API |
|---|---|---|---|
| `kernel32` | memory, file, time, process, locale | 900–1010 handle/VA; 1371 `h_create_file`; 4511 file mapping; 2955 timers; 7681 registry | `MemApi`, `FileApi`, `TimeApi`, `ConfigApi`, `LocaleApi`, `AssetApi` |
| `kernel32` | **thread** | 4065–4119 stubs/TLS; 5482–5594 `GThread`/`guest_thread_proc`/`h_create_thread` | `ThreadApi` |
| `kernel32` | **sync** | 2096–2130 events; 2200 mutexes; 2321–2820 waits/semaphores; 5594–5680 `cs_get` | `SyncApi` |
| `user32` | messages, input, clipboard | 6391 `g_uq` + `u_msg_post`/`u_msg_take`; 6785–6954 `hle_android_key_event`/`release_all` | `WindowApi`, `InputApi`, `ClipboardApi` |
| `gdi32` | DC, font, blit | as-built GL-форвардеры Windows host (legacy, не входят в target) | `WindowApi` |
| `advapi32` | registry | 7681 `g_reg` | `ConfigApi`, app-private dir |
| `winmm` | waveOut, time | 2912 `g_wave_dev` | `AudioApi`, `TimeApi` |
| `msvcr80` | CRT, math, threads | 4740 `g_cr_file`; 5895 `cr_sqrt_sse2`; 5893–5920 `cr_pow_sse2` | `MemApi`, `FileApi`, `ThreadApi` |
| `d3d9` | 324 COM-слота | 7947+ `g_d3d9_vt`/`g_d3d9_op` | thin bridge (GB-8) |
| `dsound` | COM | `hle_dsound.cpp` as-built | `AudioApi` |
| `miles` | `AIL_*`, `.m3d` | 4180 `g_mods`; late-bind sites 166, 327, 3344 | `AudioApi`, `FileApi` |
| `display` | mode/widescreen | `hle_android_display_mode*`, `hle_widescreen127.c` | `DisplayApi` (§13 D1, отсутствует) |

`ijl15.dll` — лифтнутый код, не HLE. Внешние D3D-DLL и legacy-рендеры
запрещены политикой G-02. Модули `d3d9`/`dsound`/`win32` в T1 работают с
`platform/headless` и `gfx/null` (V4.7): это тот же код HLE, но без DXVK;
`--policy` обязан доказать, что этот frontend не попал в APK (BP-7.2).

## H6. Границы переноса M5

* **H6.1** Один модуль = один коммит; на границе — адаптер; после каждого шага
  зелёные **T0 + T1** (ADR-020, V6.1). Старый файл удаляется только после
  прохождения новым того же гейта (§2.3). V6.2 запрещает начинать M5 до
  закрытия G-08/G-09.
* **H6.2** Порядок: `abi` → `kernel32/{memory,file,time,process,locale}` →
  `kernel32/{thread,sync}` → `user32/gdi32/advapi32/winmm` → `msvcr80` →
  `miles` → `d3d9/dsound` → `display` → cleanup.
* **H6.3** **Реальное препятствие strangler:** as-built `hle_android.c` связан
  через file-scope глобалы (`g_handles`, `g_uq`, `g_key_state`, `g_hle[]`,
  `arg32`, `rd32g`), а не через интерфейсы. Перенос «по одному модулю»
  невозможен без предварительного разделения состояния: обязателен переходный
  `hle_legacy_shim`, публикующий те же глобалы, и **один владелец на каждый
  глобал** по классам §3.1. Иначе «шов», на который опирается ADR-020, не
  существует и cutover превращается в big-bang.
* **H6.4** 10 подтверждённых гонок на пути `hle_call` и глобальные локи
  (`cs_get`, `g_heap_lock`, `g_uq_lock`) устраняются **в момент разделения**,
  а не после: P0-4/P0-7 из §3.7 — часть M5.

## H7. Инварианты модуля

* HLE core не включает OS-заголовки и не содержит `#ifdef __ANDROID__`
  (§4.3); OS-зависимость только через `runtime/platform` (H5).
* Ни один host lock не удерживается при `dispatch()`; per-object локи
  разрешены (§3.3). Исключение для guest-visible CS через callback — E8.4.
* Весь debug-код под `#if WC3_DEBUG`; release hygiene по §9.3/G-12.
* Один `HleEntry`, одна формула `stack_cleanup`, поле `user` уже присутствует
  (ABI-8). `hle_stub` запрещён; неизвестный обязательный импорт — fatal.
* Таблицы прямые/compile-time; реестр immutable после bootstrap (H8).

## H8. Двухфазный реестр и late-bind

* **H8.1** `BOOTSTRAP` — статическая таблица импортов из PE плюс заранее
  известные динамические экспорты Miles, до первого guest thread.
* **H8.2** `LATE_BIND` — append-only, однопоточно, до `CreateThread`; каждое
  добавление проверяется на дубликат и лимит слотов. As-built регистрирует
  после старта три sites (`hle_android.c:166, 327, 3344`: `AIL_startup`,
  обёртки `AIL_*`, `__wave_ret`), потому что `.m3d` открываются динамически.
* **H8.3** После первого `CreateThread` — `frozen=1`; поздняя регистрация =
  debug assertion / fatal. Порядок строк таблицы пинится `registry_sha256`
  (ABI-3.3): thunk-адреса зависят от `count++`.

## H9. Полнота ABI обработчиков

`result_kind` (`EAX | EDX_EAX | ST0 | XMM0 | HIDDEN_PTR`), `arg_source`
(`STACK | X87_REGS | SSE_REGS`) и `x87_stack_delta` обязательны (ABI-8, E4.5).
As-built `cr_sqrt_sse2`/`cr_pow_sse2` пишут результат в `c->xmm[0]`, а
`_CIsqrt` **снимает значение с FPU-стека**, меняя x87 TOP; исторический баг с
null-заглушкой привёл к неверному пути movie-файла. `hle_call` обязан
симметрично сохранять x87/SSE, а не только EBX/ESI/EDI/EBP; обязателен
golden-тест битовой точности `sqrt/pow/sin` (E4.4).

## H10. Состояние и гонки

Четыре класса §3.1; handle-слоты — per-object mutex/condvar или CAS-freelist;
mailbox — набор per-producer SPSC (P0-8). Инвентарь: 186 глобалов файловой
области, 140 разделяемых с записями, **10 подтверждённых находок**
(`concurrency_audit.static.json`). Обязательны: освобождение `GThread.used`,
`pthread_join`/сигнал thread handle, `pthread_attr_setstacksize(8 МиБ)`,
TLS SoftFloat на новом потоке, `PTHREAD_PRIO_INHERIT` (THREAD_AUDIT §7).
Контракты **TA-1…TA-18** (`THREAD_AUDIT.md` §8–§9) фиксируют матрицу владения для
этих глобалов, host-поток (TA-2), уровни L0/L1/L2 проверки блокировок
(TA-3) и стресс-тест E5.3 с ≥2 guest threads (TA-4).
Глобального лока на горячем пути быть не должно (§3.7, инвариант 1).

## H11. Гейты и Definition of Done M5

| Норма | Гейт / шаг |
|---|---|
| H3, H4 | G-03 (`--require-hle`), M3 import_policy |
| H6 | V6.1/V6.2, T0 + T1 на каждом модуле |
| H7 | G-01, G-02, G-12 |
| H8 | G-05, ABI-3 |
| H9 | G-14, E4 |
| H10 | `concurrency_audit.py --strict`, G-04 |

**DoD M5:** все 426 записей отнесены к модулю; 36 thread/sync реализованы с
точной семантикой; ни одного обязательного `unresolved`; каждый модуль прошёл
T0+T1; `hle_android.c` удалён только после этого; `registry_sha256`
зафиксирован.

## H12. Открытые долги и владельцы

| Долг | Владелец |
|---|---|
| import_policy.1.27b.json и классификация 832 unresolved / 50 forbidden | M3 / G-03 |
| `hle_legacy_shim` и владельцы глобалов (H6.3) | M5, до первого модуля |
| 6 строк `unavailable` и 11 P0-долгов `THREAD_AUDIT.md` | Step 5 / T2 |
| модуль miles и `DisplayApi` (§13 D1) | Step 5 |
| `registry_sha256`, golden-тест x87/SSE | Step 3 / G-05 |

До закрытия H6.3 M5 не начинается даже при зелёных T0/T1: адаптер без
владельцев состояния создаёт видимость strangler при фактическом big-bang.
