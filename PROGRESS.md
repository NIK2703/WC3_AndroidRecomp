# Прогресс переноса инструментария (2026-11-06, продолжение)

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
- M5: `QUARANTINE.md`, `hle_legacy_shim`, разделение `hle_android.c` по DLL.

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
