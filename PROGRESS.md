# Прогресс переноса инструментария (2026-11-06, продолжение)

## Headless platform backend (track A, T0)

- `runtime/platform/headless/` — первая реализация всех 15 интерфейсов
  `Wc3Platform` для верификационного DUT: mem/file/time, thread/sync,
  window/input/audio/clipboard/surface/log/config/asset/locale/vulkan.
  Файлы разделены по подсистемам, каждый < 2000 строк; `platform.h` не менялся.
- Mem — фиксированные 32-битные VA: `VirtualAlloc` на Windows,
  `mmap(MAP_FIXED_NOREPLACE)`/`mprotect` на POSIX, с отказом от чужого
  mapping вместо тихой подмены. File — stdio с 64-битными `seek`/`size`
  (`_fseeki64`/`fseeko`), без текстового режима. Thread — реальный pthread на
  каждый guest thread, TLS-индексы и join/detach; sync — Win32-семантика
  event/mutex/semaphore/critical section, включая recursive acquire,
  `WAIT_ABANDONED_0` при выходе владельца и `wait_all`.
- `ConfigApi` больше не читает process environment: значения передаются
  явно через `Wc3HeadlessConfigEntry`, дубликаты и кривые ключи отвергаются.
  Это шаг к DUT-1 («запрет getenv»), но **не** полный DUT-1: время пока
  реальное host-время, виртуальные тики/TA-12 расписания не реализованы.
- Asset — явные asset/save roots, MPQ-имена сортируются для
  детерминированного `enumerate_mpq`, `present` делает case-insensitive
  поиск и отвергает `..`/разделители. Audio — валидный null-stream с
  подтверждённой записью; Vulkan честно возвращает `-1` (в headless нет
  instance/device). Input — тот же SPSC mailbox на 1024 события с
  dropped-счётчиком.
- `tests/platform_headless_test.c` — контрактный тест: таблица 15 API,
  mem reserve/commit/protect/decommit/release, файлы, часы, pthread/TLS,
  события/мьютексы/семафоры/CS/abandoned, input, окно, clipboard, locale
  (UTF-8/1252), config, asset enumeration, audio/surface, guest memory.
  `tools/run_tests.py --test platform_headless` — **OK** (debug+release).
- `check_file_layout.py --strict` — только две предсуществующие R4-ошибки
  (`runtime/legacy/hle_android.c`, `runtime/platform/android/platform_android.c`),
  новых ошибок нет. POSIX-ветка на этом хосте не собиралась (нет
  Linux/WSL); это зафиксировано как `unavailable`, а не pass.

## Virtual clock и cooperative TA-12 schedule (2026-11-07)

- `Wc3HeadlessScheduleConfig` — opt-in: без него платформа сохраняет host
  monotonic/realtime и host scheduling. С расписанием monotonic начинается с
  нуля, realtime считается от фиксированного epoch, `local_time()` использует
  фиксированный UTC-offset. Host-путь контрактного теста не изменился.
- Timed input script копируется при init, проверяется на порядок и валидность
  kind, публикуется в SPSC mailbox по virtual time с `mailbox_delay_ns`;
  `wc3_headless_clock_advance_ns()` и `wc3_headless_input_pop()` публикуют
  созревшие события. Identity расписания — детерминированный FNV-1a 64 по
  policy/seed/epoch/timezone/id/script; это не SHA-256 и не заменяет identity
  Python-difftest.
- `runtime/platform/headless/headless_sched.c` — cooperative token scheduler:
  каждый guest thread остаётся реальным pthread, токен передаётся на
  `yield`, `sleep_ns`, blocking wait/join. Реализованы ROUND_ROBIN, FIFO,
  LIFO и WAKE_REORDER; virtual timers обслуживаются только когда нет
  runnable-потока. Sync wait использует абсолютный deadline, поэтому
  spurious wake не продлевает timeout.
- Выход владельца освобождает mutex/CS до передачи токена
  (`headless_sync_thread_exit`), иначе sync-waiter мог повторно заблокироваться
  на объекте, который TLS-destructor освобождает уже после handoff. Join,
  abandoned mutex, spurious wake, timeout и wake-order проверены под всеми
  четырьмя policy.
- **Ограничения TA-12:** приоритеты guest-потоков не моделируются, потому что
  в замороженном `Wc3ThreadDesc` нет поля priority; FIFO/LIFO/WAKE_REORDER
  меняют порядок создания/wake-up, а не приоритет. Scheduler кооперативный:
  guest-код, не доходящий до platform blocking point, не вытесняется.
  Поток, созданный не через `Wc3ThreadApi`, не может блокироваться на
  guest-visible sync при активном расписании. Priority inversion/полный
  TA-12 остаются `missing`, а не заявляются закрытыми.
- Проверки: `run_tests.py --test platform_headless` — **OK** (debug+release,
  30 повторов release); `config_schema`, `module_registry`, `corpus_binding`,
  `imports`, `asset_api` — pass. `check_arch.py --concurrency --release` —
  только предсуществующие ошибки `runtime/legacy/`; `check_file_layout.py
  --strict` — те же две R4-ошибки (`hle_android.c`, `platform_android.c`).
  Android/T2 не проверялись.

## gfx/null: канонический поток и инварианты V5.9 (2026-11-07)

- `runtime/gfx/null/` — верификационный frontend: не рисует пиксели, не
  создаёт Vulkan/DXVK/GL и не входит в APK. `null_stream.c` пишет
  host-независимый канонический поток `(iface, method, argc, args[8])` с
  текущим digest состояния; host-указатели, host-время и порядок worker'ов в
  него не входят. Хэш — FNV-1a 64 с явным little-endian, это identity потока,
  а не SHA-256 и не замена identity oracle.
- `null_invariants.c` реализует три инварианта V5.9 на ограниченных таблицах
  ресурсов/интервалов: `locked_range_written` (ненулевая запись в покрытом
  Lock/Unlock диапазоне), `declared_range_consistency` (размеры буферов,
  `numVertices`/`startIndex`/`primitiveCount`, значения индексов и
  `BaseVertexIndex`) и `degenerate_primitive_ratio` (порог в ppm из G).
  Переполнение таблицы или отсутствие индексных данных даёт
  `UNAVAILABLE`, а не `PASS`; пустая/нулевая запись и рассогласование дают
  `FAIL`.
- Проверки: `run_tests.py --test null_gfx` — **OK** (debug+release, 50
  повторов release). Покрыты replay-хэш, изменённый аргумент, state digest,
  zero/read-only lock, overrun/negative base/короткие индексы, degenerate
  threshold, отсутствие данных и переполнение таблицы. `check_file_layout.py
  --strict` — только те же две предсуществующие R4-ошибки.
- **Не закрыто:** `gfx/null` пока не подключён к D3D9 HLE/thin bridge и не
  входит в `wc3_headless`; `null_backend_sha256` не публикуется, G-09/T1
  остаётся `missing`. Android/T2 не проверялись.

## Config schemas (track A, M3/R10)

- `config/schema/` — 10 JSON Schema (draft 2020-12): `common.schema.json`
  с общими `$defs` (va32, sha256, ordinal, import entry, PE file, export
  table) и отдельные схемы для `arity_overrides`, `baseline`,
  `build_upstream`, `cdecl_allowlist`, `corpus`, `hle_arity`, `imports`,
  `layout`, `migration`, `pe_imports`. `README.md` фиксирует mapping.
- `tools/check_config_schema.py` — валидатор без внешних зависимостей:
  type/required/properties/items/enum/const/pattern/min/max, локальные и
  относительные `$ref` на `common.schema.json`; печатает JSON-path ошибки.
  `--root` позволяет прогнать на другом checkout.
- `tools/run_tests.py --test config_schema` — **OK** (10 файлов).
  `check_file_layout.py --strict` больше не выдаёт R10 warning о
  `config/schema/`; остаётся только `import_policy.1.27b.json` (track B) и
  две предсуществующие R4-ошибки.
- Схемы структурные, не семантические: принадлежность импорта, HLE-статус и
  пересечения layout по-прежнему проверяют `hle_coverage.py`,
  `check_layout.py` и C-тесты. `import_policy` намеренно не описан, пока его
  не добавит владелец трека B.

## Cross-module import binding и аудит импорта (вечернее продолжение)

- `runtime/guest/guest_modules.{h,c}` — каталог загруженных guest-модулей и
  привязка IAT: загруженный lifted-модуль выигрывает у HLE (как в
  `runtime/legacy/boot_probe.c`), отсутствующий экспорт загруженного модуля
  считается отдельно (`export_missing`) и только затем падает в HLE,
  неразрешённый импорт не выдумывается. Каталог lock-free, без аллокаций,
  имена DLL сравниваются по basename без учёта регистра; экспорт —
  по имени (case-sensitive, как `GetProcAddress`) или ординалу.
- `tests/module_registry_test.c` — синтетические PE32-образы в low memory:
  именованные/ординальные экспорты, пути и регистр имён, дубликаты,
  lifted binding, HLE fallback, missing export, unresolved import,
  `wc3_module_bind_all`. Тест входит в T0 (`module_registry`, debug+release).
- `tests/corpus_binding_test.c` — real-corpus проверка того же пути:
  лицензионные rebased PE из `build/corpus-inputs` маппятся настоящим
  `wc3_pe_load` по фиксированным базам (war3.exe 0x400000, game.dll 0x6F000000,
  Storm 0x35000000, Mss32 0x35100000, Mssfast 0x35200000, Mp3dec 0x35300000,
  ijl15 0x60000000), регистрируются и биндятся `wc3_module_bind_all`.
  Ожидание из пинящей фикстуры: **1896** импортов семи файлов = **428**
  lifted + **1468** HLE, `missing=0`, `export_missing=0`. Дополнительно
  проверяются именованный IAT (`mss32.dll!_AIL_open_stream@12`), ординальный
  (`Storm.dll!#279`), попадание `KERNEL32.dll!Sleep` в HLE-окно, отсутствие
  диагностик и **полный обход import-таблиц**: все 428 lifted-слотов равны
  `wc3_pe_export*`, все 1468 остальных лежат в HLE-окне, ни один descriptor
  не остался без `OriginalFirstThunk`. Без корпуса тест возвращает
  `unavailable`, а не pass. `run_tests.py --test corpus_binding`
  (debug+release) — OK.
- **Аудит лицензионного корпуса:** все **428** импортов из lifted-модулей
  (`mss32.dll` 259, `Storm.dll` 166, `ijl15.dll` 3) разрешаются экспортами
  этих модулей; 0 case-mismatch, 0 forwarder, 0 отсутствующих ординалов.
  `game.dll` и `war3.exe` импортируют их статически, поэтому это не HLE-долг.
- `config/pe_imports.1.27b.json` теперь несёт `exports` (имена + ординалы
  каждого модуля), `config/imports.1.27b.json` — статус `lifted`:
  `hle=1051`, `lifted=428`, `unresolved=832→404`, `excluded=76`,
  `forbidden=50`. `tools/import_fixture.py` классифицирует lifted раньше HLE
  и учитывает только in-scope (loaded) модули; `tools/hle_coverage.py`
  требует, чтобы **каждый** импорт из loaded-модуля был lifted и чтобы
  каждый lifted-символ действительно был в export index; lifted не считается
  HLE-долгом. Обе негативные проверки (demoted-строка и поддельное имя)
  возвращают FAIL. Генерация детерминирована (повторный прогон даёт тот же
  sha256).
- `tools/check_file_layout.py`: предупреждение R10 о missing
  `import_policy.1.27b.json` теперь берёт остаток из
  `config/imports.1.27b.json` (404), а не хардкодит 832.
- Нормативные `docs/HLE_MODULES.md` H3/H4 всё ещё называют 832 unresolved и
  426 реализованных HLE; они не правились, потому что HLE-раздел ведёт
  параллельный агент. Расхождение зафиксировано здесь, а не замолчано.

Проверки: `run_tests.py --test imports` — pass; `module_registry` — pass
(debug/release); `corpus_binding` — **OK** на реальных 7 PE: 428 lifted /
1468 HLE / 0 missing / 0 export_missing (debug+release);
`check_layout.py --require-corpus` — pass;
`check_file_layout.py --strict` — те же 2 предсуществующие R4-ошибки
(`hle_android.c`, `platform_android.c`), warning теперь 404;
`check_arch.py --concurrency --release` — только предсуществующие ошибки
`runtime/legacy/`.

Статус: M8 доведён до рабочего **C-only** конвейера. `build_tools.ps1` собирает
**26 бинарников** и гоняет расширенный `-Smoke`. Замыкание
`entry → seed → lift32 → closure → known → liftpass` **зелёное на полном
инвентаре лицензионного `war3.exe`** (0 нарушений), G-05 закрыт по
идентичности входов корпуса, все 32 C-исходника проходят POSIX-ветку.
Сгенерированный C остаётся в `build/`.

**Правило владельца:** весь рабочий инструментарий — на C. Python-драйвер
конвейера удалён, `closure_pipeline.exe` и `corpusinput.exe` написаны в
`tools/lift/csrc_local/`. Python-модули `tools/lift/` — только эталон G-15,
PowerShell (`build_tools.ps1`) — только оркестрация. `docs/*` в этом
продолжении не правились по требованию владельца.

## Сделано ранее

- `tools/lift/csrc/` — 29 файлов, побайтная копия upstream (22 инструмента),
  `third_party/capstone/`, `config/build_upstream.json` (50 файлов, rev
  `791bdbaf3135`), `check_upstream.py` и `check_equivalence.py` (прототип G-15).
- `tools/lift/csrc_local/{sha256,dirwalk,corpus,liftpass}.{c,h}` — собственный
  C-код дерева; `CMakeLists.txt` читает upstream из `config/build_upstream.json`.

## Сделано в этом продолжении

1. **Разбор 6 остаточных нарушений и зелёный полный прогон.**
   - 4 `unknown_target` (`0xE8C9EC10`, `0xE993EC1C`, `0xEBFAEC24`,
     `0x433AF7D2`) — не дыры замыкания, а `data-as-code`: `seed.exe`
     восстанавливает extent до терминатора, за которым следует `0xCC`/`0x90`;
     у `L_0042EA00` (609 B) и `L_0042F840` (224 B) после настоящего `ret`
     лежит встроенная таблица данных, линейный лифт декодирует её как код.
   - 2 `no_terminal` (`L_0041E1D1` `hlt`, `L_00449BF0` `far jmp m16:32`) —
     тела из `RECOMP_TODO`, который разворачивается в `abort()`
     (`runtime/cpu/cpu.h:364`), то есть **явная ловушка, а не fall-through**.
   - `closure_pipeline.exe`: seeds по умолчанию в **block-mode** (остановка на
     первом терминаторе, остальное добирает `closure.exe`), `--full-seeds`
     оставляет линейный режим диагностикой. `liftpass.exe` считает
     `RECOMP_TODO(` терминалом. Детекторы V5.4 не ослаблены: испорченная
     фикстура по-прежнему даёт 5/5 нарушений.
   - Фикстура `tests/fixtures/lift_tools/todo_smoke.c` доказывает, что
     `RECOMP_TODO`-тело принимается; `lifted_smoke.h` и `corpus_inputs.txt`
     расширены до 4 VA / 4 файлов.
2. **G-05: compiled C-перебазирование и идентичность корпуса.**
   - `tools/lift/csrc_local/corpusinput.c` → `corpusinput.exe` с подкомандами
     `rebase`, `prepare`, `seal`, `verify`. Порт `rebase_pe.py` на C: правка
     `ImageBase` и всех `IMAGE_REL_BASED_HIGHLOW` с проверкой инволюции
     (обратный rebase побайтно совпадает с исходником).
   - `config/corpus.1.27b.json` — декларация 7 входов лифта; `prepare`
     генерирует `build/corpus-inputs/` из лицензионных файлов, 4 модуля
     перебазируются на `0x35000000/0x35100000/0x35200000/0x35300000`.
     `Mss32_a35.dll` и `Mssfast_a35.m3d` **побайтно совпали** с эталоном
     старого дерева (Python).
   - `corpus.exe` получил `--layout-file` (хэш `runtime/guest/guest_layout.h`,
     без циклической зависимости от записываемого манифеста).
     `corpus_sha256 = 37e4b45d71736bfed356a98b81fe60a4d46d7b11e72f4a1e8e2c6bc3250f3038`
     запечатан в `config/layout.1.27b.json` (`status: generated`,
     `lift_rev: 1`, `inputs: 7`).
   - `check_layout.py --require-corpus` и `corpusinput verify` проходят;
     V8-evidence — `reports/generated/gate_status.json`. В `run_tests.py`
     добавлен тест `corpus_identity` (при отсутствии build-артефактов честно
     возвращает `unavailable`), а `layout` теперь вызывается с `--require-corpus`.
3. **POSIX-порт пинящегося инструментария.** `tools/lift/posix_shim/` —
   `windows.h` и `direct.h`, эмулирующие ровно то подмножество Win32, которое
   используют 26 файлов `csrc/*.c`: типы/константы (`DWORD`, `HANDLE`,
   `WIN32_FIND_DATAA`, `MAX_PATH`, `INVALID_FILE_ATTRIBUTES`…),
   `FindFirstFileA`/`FindNextFileA`/`FindClose` через `readdir` (все вызовы
   используют шаблон `<dir>\*`), `GetFileAttributesA`, `CreateDirectoryA`,
   `MoveFileExA` → `rename`, `LoadLibraryA`/`GetProcAddress` → `dlopen`/`dlsym`
   с фолбэком на `libcapstone.so`, `_stricmp`/`_snprintf`/`_mkdir`,
   `GetModuleFileNameA`. Пинящиеся `csrc/*.c` и
   `config/build_upstream.json` **не изменялись** (`check_upstream.py` pass).
   `WC3_FORCE_POSIX` выбирает POSIX-ветку и на Windows-тулчейне: там нет
   `dlfcn.h`, поэтому `dlopen`/`dlsym` объявлены без вызова, а проверка
   ограничена `gcc -fsyntax-only`. В `csrc_local/*.c` платформенные условия
   приведены к `#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)`, чтобы та же
   проверка покрывала и их POSIX-ветки. `-I posix_shim` добавляется **только** в
   режиме `-PosixCheck`, обычная Windows-сборка shim не видит.

## Проверено

| Проверка | Результат |
|---|---|
| `build_tools.ps1 -Force -Smoke -PosixCheck` | 26/26 бинарников; capstone, address merge (4 VA), corpus hash/list + негативный verify, liftpass 5/5 детекторов + TODO-фикстура, self-test драйвера — OK |
| `-PosixCheck` / T0 `posix_shim` | **32/32 C-файла** (26 пинящихся + 6 локальных) проходят `gcc -fsyntax-only` в POSIX-ветке, 0 ошибок, 1 предсуществующий `-Wsign-compare` в `bmpinfo.c` (идентичен на обеих ветках) |
| Полный инвентарь `war3.exe` (block-mode, 5391 seed → 5360 extents) | **verdict pass**: 12 раундов, +3836, 9196 определений, 14 546 константных целей, **0 нарушений** → `reports/generated/closure_pipeline.json` |
| Ограниченный прогон (64 seeds) | **pass**: 15 раундов, +119, 183 определения, 302 цели, 0 нарушений → `reports/generated/closure_pipeline_c_smoke.json` |
| `--full-seeds` диагностика | `closure pass`, но `verdict fail`: ровно 4 `unknown_target` (data-as-code), 0 `no_terminal` → `reports/generated/closure_pipeline_full_seeds.json` |
| `gcc -fsyntax-only` на `seed.c` + `closed.c` полного прогона | OK (оба TU) |
| `corpusinput rebase ... --verify` | round trip OK; sha256 `Mss32_a35.dll` / `Mssfast_a35.m3d` совпали с эталоном старого дерева |
| Детерминизм: повторный `prepare` + `corpus.exe verify` | `pass` — тот же `corpus_sha256` (7 inputs) |
| `corpus.exe verify` с `--lift-rev 2` | `FAIL` (детектор изменений работает) |
| `corpusinput verify --evidence` | `pass`, evidence записан |
| `check_layout.py --corpus 1.27b --require-corpus` | OK, 20 регионов |
| `check_docs.py --strict` | OK, 0 warnings |
| `check_upstream.py` | pass (50 pinned files) |
| `check_equivalence.py ... --self-test` | pass (1 function, 0 mismatches) |
| `run_tests.py` | 15 тестов: 12 зелёных (включая новые `corpus_identity`, `posix_shim`) + `thread_audit` = `unavailable`; `FAILED: file_layout, platform_contract` — **до изменений** |
| `check_file_layout.py --strict` | 2 ошибки R4, 10 warnings — все до изменений (`hle_android.c` 10 356 строк, `platform_android.c` 2040) |
| `check_arch.py --platform-contract --concurrency --lifecycle --release` | те же ошибки карантина `runtime/legacy/` |

## Не доделано

- Нормативные `docs/*` (VERIFICATION G-05, ARCHITECTURE §5.2/§5.4,
  BUILD_PIPELINE BP-6.3, GUEST_ABI ABI-3.2) всё ещё описывают
  `corpus_manifest.sha256 = null` и `G-05 = declared`; не правились по
  требованию владельца. `check_docs.py --strict` при этом зелёный.
- POSIX-порт проверен только на уровне `gcc -fsyntax-only` (BP-8.3 требует
  именно этого). Реальная сборка и запуск 26 инструментов под Linux с
  `libcapstone.so` не выполнялись: на этой машине Windows-тулчейн, а
  `WC3_FORCE_POSIX` объявляет `dlopen`/`dlsym` без вызова. Это следующий шаг
  для Linux-CI. Один `-Wsign-compare` в `bmpinfo.c` остаётся в пинящемся
  файле — не regressions.
- T0 красный по `file_layout` / `platform_contract` из-за
  `runtime/legacy/hle_android.c` и `platform_android.c` — к инструментарию
  отношения не имеет.
- `check_equivalence.py` = `provisional`, G-15 = `missing` (нужен golden-набор
  по 18 модулям). G-05 закрыт только по **статической идентичности входов**;
  поведенческая эквивалентность остаётся за G-08/G-09.
- M5: cross-module каталог и привязка импортов готовы
  (`runtime/guest/guest_modules.c`), но boot-path продукта всё ещё идёт через
  `runtime/legacy/boot_probe.c`; остаются `QUARANTINE.md`, `hle_legacy_shim`,
  разделение `hle_android.c` по DLL и cutover загрузки модулей на новый
  каталог.

## Планы

1. Собрать и запустить 26 инструментов под Linux-CI с `libcapstone.so`
   (полноценная проверка shim'а, а не только `-fsyntax-only`).
2. Расширить `check_equivalence.py` до golden-набора по 18 модулям → G-15;
   затем поднять полный многомодульный лифт (не только `war3.exe`).
3. M5: карантин, `hle_legacy_shim`, разбиение HLE по guest DLL.
4. Привести `docs/*` к фактическим статусам G-05 и зелёному полному прогону.

## Воспроизведение

```
tools/lift/build_tools.ps1 -Force -Smoke -PosixCheck
build/lift-tools/closure_pipeline.exe --max-functions 64 \
  --out-dir build/lift-c-smoke --work-dir build/lift-c-work \
  --report reports/generated/closure_pipeline_c_smoke.json
build/lift-tools/closure_pipeline.exe \
  --out-dir build/lift-c-block-full --work-dir build/lift-c-block-work \
  --report reports/generated/closure_pipeline.json
build/lift-tools/corpusinput.exe prepare --manifest config/corpus.1.27b.json \
  --root . --out-list build/corpus-inputs/inputs.txt
build/lift-tools/corpus.exe hash --inputs build/corpus-inputs/inputs.txt \
  --root . --generator wc3lift --lift-rev 1 \
  --layout-file runtime/guest/guest_layout.h \
  --out reports/generated/corpus.sha256 --json reports/generated/corpus_inputs.json
build/lift-tools/corpusinput.exe verify --layout config/layout.1.27b.json \
  --hash reports/generated/corpus.sha256 \
  --evidence reports/generated/gate_status.json
python tools/check_layout.py --corpus 1.27b --require-corpus
```

Лицензионные PE и сгенерированный C в репозиторий не попадают: все артефакты
лежат в gitignored `build/` и `reports/generated/`.
