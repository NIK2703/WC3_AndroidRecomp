# Архитектура WC3 1.27b: Android arm64 (нормативный хаб)
Статус: **норма, rev 2.2 / M2** (2026-11-06). Цель — 1.27.1.7085
(PE32/x86), продукт — Android arm64-v8a `libwc3gameactivity.so`.
Хаб содержит принципы, модель исполнения, слои и инварианты; детальные
нормы — у владельцев областей (`docs/README.md`, §14), as-built/ADR/
перформанс/риски/backlog вынесены при M2. **Нумерация `§X` сохранена**:
ссылки в документах и плане ведут на короткие разделы-указатели.
При расхождении приоритет у владельца области, затем ADR, затем хаб.
## 0. Что исправлено в rev 2
История решений — `docs/AS_BUILT.md`. ADR-026: Windows-эталон — оригинальная
игра и её захваты, без рекомпиляции под Windows. `tools/strip_thread_lines.py`
к архитектурным документам не применяется.
## 1. Область и принципы
### 1.1. Продукт
Точная статическая рекомпиляция x86-32 PE в нативный C; C собирается
NDK/Clang в ARM64. Гость: `war3.exe`, `game.dll`, `Storm.dll`, `Mss32.dll`,
`Mssfast`, `Mp3dec`, `ijl15.dll`. В APK нет интерпретатора, JIT, Wine, Box64,
FEX и эмуляции x86.
### 1.2. Не является продуктом / вне области
**Рекомпиляция только под Android (ADR-026):** ни один рекомпилированный
Windows-бинарь не собирается; эталон — оригинальная игра и её захваты.
LAN/Battle.net и `NetApi` вне области (ADR-015). **Запрещены** D3D8 (legacy 1.26), OpenGL/GLES,
`native_vk`, `hle_gl`, `hle_d3d9_vk`, `D9VkApi`, legacy `hle_d3d9.cpp`/
`hle_d3d9_dxvk.cpp`, `win32_d3d9`, внешние `d3d9.dll`/`d3d8.dll`, PATH и
подмена D3D-frontend'а; выбор Vulkan ICD разрешён (§7.4). Ремейк, замена
Storm/Miles и публикация ассетов/PE/сгенерированного C запрещены (ADR-006).

### 1.3. Принципы
1. **Exactness > удобство** — норма `docs/EXACTNESS.md` (E1–E12), приоритет
   выше лестницы §3.7.
2. **Замороженный guest ABI** — `docs/GUEST_ABI.md` ABI-1/ABI-2 (source-level).
3. Один runtime core, один Android host, один oracle; всё OS-зависимое за
   интерфейсами `runtime/platform`.
4. Владение состоянием явно: immutable / thread-private / guest-shared /
   host-boundary (§3.1).
5. Каждое изменение проверяется исполняемым гейтом; статус гейта — часть
   архитектуры (ADR-022, `docs/VERIFICATION.md` V1).
6. Решения принимаются по данным, а не по факту «уже собрано»: статусы ADR
   `proposed/provisional/accepted/superseded` (`docs/ADR.md`, GB-1).
## 2. Базовая линия и стратегия миграции
### 2.1. As-built (что уже работает)
`docs/AS_BUILT.md`: 191 650 записей dispatcher, 426 HLE-записей, boot → меню →
3D-сцена на устройстве. Графический дефект исправлен в старом дереве;
сохранение исправления проверяют V5.9/V6.6. Числа воспроизводят
`tools/perf_probe.py` и V2 (F2).
### 2.2. Замороженный guest ABI
Source-level контракт вшит в ~192 тыс. лифтнутых функций: имена/типы полей
`CPU`, `dispatch(CPU*, uint32_t)`/`dispatch_jmp`, HLE thunk-окно
`0x7F000000` (stride 16), формула `stack_cleanup = 4 + (cdecl ? 0 : argc*4)`,
фиксированные базы образов, identity mapping, thread-local SoftFloat,
настоящие host-атомики для `lock`-операций. Что именно ломает корпус,
идентичность `corpus.sha256` и `registry_sha256` — `docs/GUEST_ABI.md`
ABI-1…ABI-3. Ёмкость окна 65 536 ≠ активный реестр as-built 4 096 (ABI-7).
### 2.3. Инкрементальный cutover (strangler), а не big-bang
Новое дерево — канонический target, старое — источник до cutover. Одна
подсистема за шаг, адаптер на границе, T0 + T1 после каждого (Android boot →
меню = T2 на Step 9), старый файл удаляется после прохождения новым того же
гейта. Порядок: baseline/гейты → HLE ABI → platform/app → win32-модули →
UI/input/audio → графика → cleanup. Шов M5 требует `hle_legacy_shim` (H6.3).
Объём: ~20 000 строк C/C++ плюс 191 650 адресов корпуса.

### 2.4. Верификационный эталон
**ADR-026/027:** поставляемый target — Android arm64-v8a; рекомпилированный
код под Windows не собирается. Эталон **O/G** — оригинальная игра 1.27.1.7085
и её захваты; `wc3_host27*.exe` — исторический артефакт. T1 исполняет
headless-DUT **H** (`wc3_headless`) против G по identity V3 без adb и
устройства; контракт — V4.7/V5, роли — E1.1, **D** остаётся T2. Разные базы
Storm/Mss исключают побайтовый хэш памяти и кросс-хост frame hash как гейты
(V4); нормализация V4.2, критерий равенства E11.
## 3. Модель исполнения
### 3.1. Четыре класса состояния
| Класс | Примеры | Синхронизация |
|---|---|---|
| `immutable` | HLE registry, lift hash, layout, config | нет; прямая индексация после bootstrap |
| `thread-private` | `CPU`, guest stack, TEB, TLS[64], dispatcher state | TLS, разделения нет |
| `guest-shared` | handle slots, events, mutexes, semaphores, CS, staging | HLE-объект с точной семантикой Win32, per-object mutex/condvar |
| `host-boundary` | UI mailbox, input queue, audio ring, overlay canvas, DXVK shim | lock-free atomics acquire/release; у DXVK свои локи |

Guest memory явно относится к `guest-shared` (E5.4).
### 3.2. Guest threads
`CreateThread`/`_beginthreadex` создают **реальный host thread**; каждый имеет
`CPU`, guest stack, TEB, TLS и dispatcher state (`GUEST_THREAD_MAX = 24`).
`DllMain`, `Tls*`, `GetLastError`, приоритеты и wait на thread handle — с
точной семантикой, без no-op waits. Кооперативное схлопывание в один OS-поток
запрещено (ADR-018). Контракт host-потока (8 МиБ стека, join/detach, TLS
SoftFloat, `PTHREAD_PRIO_INHERIT`) — `docs/THREAD_AUDIT.md` §7.
### 3.3. Инвариант блокировок (вместо тотального запрета)
> **Ни один host lock не удерживается во время `dispatch()`, обратного вызова
> в гость или ожидания guest-visible объекта. В пути «инструкция → dispatcher
> → HLE registry» локов нет.**

Разрешены и требуется: per-object mutex/condvar в HLE-объектах, короткая
синхронизация или CAS-freelist при создании слота, atomics acquire/release в
mailbox/ring, собственные локи DXVK/shim'а (allowlist). Spinlock запрещён.
Исключение для guest-visible CS через callback — `docs/EXACTNESS.md` E8.4.
`--concurrency` проверяет зоны владения и «dispatch под локом», а не «0 слов
mutex в runtime/».
### 3.4. HLE sync
Guest-visible events (manual/auto reset), mutexes (recursive, abandoned),
semaphores (счётные), CRITICAL_SECTION (recursive), `WaitForSingleObject`
(таймаут/INFINITE), `WaitForMultipleObjects` (`bWaitAll`), `Interlocked*` —
полноценные HLE-объекты; заглушки «сразу `WAIT_OBJECT_0`» запрещены (ADR-008,
исторический баг сломал все MPQ-чтения). Алгоритм `WaitBlock` — E8.1;
36 записей и статусы — `docs/THREAD_AUDIT.md`.
### 3.5. Android host threads и mailbox
Сохраняются UI/GameActivity, input, overlay, canvas, AAudio и внутренние
потоки DXVK. Producers не вызывают HLE и не трогают guest memory: набор
per-producer SPSC очередей с одним consumer и фиксированным приоритетом
(`runtime/ipc/mailbox_set.h`, P0-8). Canvas/overlay публикуют кадр через
атомарный индекс двойного буфера. As-built путь ввода пишет очередь HLE под
глобальным локом и подлежит замене (H5/H6, V-заметки).
### 3.6. Dispatcher
Одна таблица `VA -> L_<va>`, hash-lookup (2^19, open addressing),
`dispatch_jmp` — tail-trampoline без роста host-стека. Состояние переходов
(depth, pending, draining) — **единое на все TU** и thread-private, запрещены
`static` TLS в заголовках (`docs/GUEST_ABI.md` ABI-4/ABI-5). `find_lifted`
допустим только в tooling/startup. Неизвестная цель: debug — probe-ledger,
release — fatal (запрещён soft-unknown). Полнота корпуса — статический
`refs`-аудит всех константных целей до неподвижной точки (§5.5).
### 3.7. Производительность и многопоточность (норма)
`docs/PERFORMANCE.md`: статические измерения, закрытые вопросы по хэшу,
инвентарь 10 гонок, микро-бенчмарк sync, каталог P0-1…P2-11 без
ранжирования (PM-17), коррекция G6. P0-1 = `specified` до G-14 (ABI-6);
LiftPass требует G-15 (ABI-11), инварианты проверяет генератор, не линт
2,4 ГБ C. Числа `perf_probe.py` сняты только с `war3.exe` (ABI-11.6).
`PERFORMANCE_CONTRACT.md` (PM-1…PM-24) и `THREAD_AUDIT.md` (TA-1…TA-18):
счётчики, калибровка, классификация PM-10, T2 sustained-load, профиль,
владение, L0/L1/L2, валидность замеров (PM-17…PM-24) и границы потоковой
модели (TA-12…TA-18). G-10 — диагностика (V6.7, ADR-029), L2 = `missing`,
E-контракт неизменен (E10.3).
## 4. Слои, дерево, зависимости
Слои сверху вниз, зависимости только вниз: `apps/android_gameactivity` →
`runtime/app` → `runtime/hle` (abi | win32 по guest DLL | d3d9 | dsound) →
`runtime/platform` (15 интерфейсов, NetApi нет) → `runtime/gfx` →
`runtime/cpu` + `runtime/guest` → Android NDK/AAudio/ANativeWindow/Vulkan.
`apps/wc3_headless` повторяет этот граф до `runtime/app`, подменяя только
`platform/headless` и `gfx/null`; это верификационный контур T1 (V4.7),
он **не входит** в APK link graph.
### 4.2. Дерево
Полное целевое дерево и правила R1–R22 (Rev 2: карантин R15, граф зависимостей R16, owners R18, ledger R19) — `docs/FILE_LAYOUT.md`.
Разбиение HLE по **guest DLL** (уровень 1) с подсистемами внутри DLL
(уровень 2), `oracle/` (эталонные захваты) вне `runtime/`, `dxvk_win32` в vendor-коде —
`docs/HLE_MODULES.md` H1/H2 (разрешает конфликт §4.2 ↔ R7, ADR-024).
Запрещены `gfx/native_vk`, `gfx/opengl`, `gfx/d9vk_api.h`, `hle_gl`;
`gfx/null` и `platform/headless` разрешены **только** как части `--target
headless` (ADR-027, BP-7.2).
### 4.3. Зависимости (только вниз)
`runtime/guest` включает только `cpu.h` и сгенерированный dispatcher;
`runtime/cpu` не знает про HLE и ОС; `runtime/hle/abi` зависит от `cpu` и
`platform`, не включает `windows.h`; `runtime/hle/win32` — от `hle/abi` и
`platform`, без `#ifdef __ANDROID__`; `runtime/hle/d3d9` — от `hle/abi`,
`platform` и thin bridge; `runtime/gfx/d3d9` не знает про Android (WSI через
`SurfaceApi`); `apps/android_gameactivity` не вызывает HLE; рекомпилированного
Windows-кода в продукте нет (ADR-026).
### 4.4. Статические библиотеки
`wc3_runtime` (CPU, dispatcher), `wc3_hle_abi`, `wc3_hle_win32` (по guest DLL),
`wc3_hle_d3d9`, `wc3_hle_dsound`, `wc3_platform_android`,
`wc3_gfx_dxvk_1_10`, `wc3_gfx_dxvk_3_1_1`, `wc3_audio_aaudio`,
`wc3_app_android`. Обе DXVK-библиотеки всегда в APK и линкуются в одно `.so`;
активный бэкенд — по §7.3 и `docs/GRAPHICS_DECISION.md` GB-3/GB-6.
**Только для `--target headless`:** `wc3_platform_headless`, `wc3_gfx_null`,
`wc3_headless`; они переиспользуют `wc3_runtime`/`wc3_hle_*`/`wc3_app_*` без
перекомпиляции корпуса и никогда не попадают в APK (V4.7, BP-7.2).
## 5. Память и layout
### 5.1. Ключевой инвариант: identity mapping
Гость 32-битный, хост 64-битный; все guest VA лежат в младших 4 ГБ и маппятся
по тем же адресам, что в PE, поэтому `uint32_t` → `void*` нулевым расширением.
Любая guest-аллокация обязана оставаться `< 0x100000000`; базы образов
фиксированы; ART резервирует `0x10980000–0x30980000` (измерено).
### 5.2. Манифест layout
`config/layout.1.27b.json` — **валидатор и вход генератора**, не «ручка»:
`images[]` с `corpus_fixed=true` и sha256, `arenas[]`, 7 `va_arenas[]`,
`thread_stacks[]` (24 × 2 МиБ), `thread_tebs[]`, `stack` (8 МиБ),
`heap`/`hle_heap`, `hle_thunks` (`0x7F000000`, 65 536 × 16), `guard_pages[]`,
`golden_sets[]`, `corpus{sha256, lift_rev, layout_rev}`. `corpus_manifest.sha256`
пока `null` — гейт G-05 = `declared` (ABI-3.2, V4.6).
### 5.3. As-built адреса (не менять без перелифта)
Android-layout: `war3.exe` `0x00400000`, `game.dll` `0x6F000000`,
`Storm.dll` `0x35000000`, Mss/Mssfast/Mp3dec `0x351..0x35300000`,
`ijl15.dll` `0x60000000`; stack `0x31000000`, thread stacks `0x31800000`,
TEB `0x3F000000`/`0x3F100000`, heap `0x36000000`, hle_heap `0x3A000000`,
VA-арены `0x61000000`, `0x40000000`, `0x80000000`, `0x70000000`,
`0xA1000000`, `0xD9800000`, `0xEBAD7000`; thunks `0x7F000000`.
Источник — `runtime/guest/guest_layout.h`; базы Storm/Mss на Windows другие (V4.1).
### 5.4. Манифест корпуса
`config/corpus.1.27b.json`: `modules[]` (PE, SHA-256, каталог, список функций),
`targets: ["android"]`, dispatcher, `layout_sha256`, `lift_rev`. Размеры
чанков — параметры сборки; входы лифта включают перебазированные PE (ABI-3.1).
### 5.5. Coverage gate
Статический `refs`-аудит всех константных целей `dispatch`/`dispatch_jmp`
плюс runtime probe-ledger: к релизу ни одна достигнутая цель не должна
остаться без лифта. Неизвестная цель — событие гейта и fatal в release.
## 6. HLE
### 6.1. Один ABI
Один `HleEntry` (`stack_cleanup`, `argc`, `flags`, `impl`, `user`), один
реестр, один `hle_call`. `user` уже присутствует в
`runtime/hle/abi/hle_abi.h`; не закрыты `result_kind`, `arg_source`,
`x87_stack_delta` (ABI-8, E4.5). Debug-only `HleDebugEntry` под `#if WC3_DEBUG`.
### 6.2. Снятие стека — одна формула
`stack_cleanup = 4 + (cdecl ? 0 : argc*4)`; значение вычисляется при
регистрации, `hle_call` не содержит ветки `if (callconv)`. Установка `esp`
принудительная, а не прибавление: обработчик, сдвинувший `esp`, уже сломал
кадр. 295 stdcall / 131 cdecl, fastcall нет.
#### 6.2.1. Источник аргументов и вид результата
`_CIpow`, `_CIsqrt`, `_CIfmod`, `_CIatan2`, `libm_sse2_*` имеют `argc=0` и
cdecl: операнды в x87/SSE, результат в ST0/XMM0, а не `eax`; часть CRT
возвращает `EDX:EAX` или структуру через скрытый указатель. As-built
`cr_sqrt_sse2`/`cr_pow_sse2` пишут `c->xmm[0]`, `_CIsqrt` меняет x87 TOP.
Полный контракт — ABI-8 и `docs/HLE_MODULES.md` H9.
### 6.3. Три источника истины
Presence — PE import directory (PE есть, `pe_fixture_status=available`,
md5 `game.dll`/`war3.exe` сверены), arity — canonical spec + call-site,
callconv — Win32 spec + `config/cdecl_allowlist.1.27b.json`.
`presence(table)` не доказательство; неизвестный обязательный импорт —
fatal at startup; `hle_stub` отсутствует (ADR-008/012). Классификация
2 009 импортов — `docs/HLE_MODULES.md` H3/H4.
### 6.4. Модули HLE
Норма — `docs/HLE_MODULES.md` H1/H2/H5: уровень 1 по guest DLL, уровень 2
по подсистеме (`kernel32/{memory,file,time,process,locale,thread,sync}`,
`user32`, `gdi32`, `advapi32`, `winmm`, `msvcr80`, `d3d9`, `dsound`, `miles`,
`display`). 36 из 426 записей — thread/sync. `NetApi` отсутствует.
### 6.5. Интерфейсы платформы
`MemApi`, `FileApi`, `TimeApi`, `LocaleApi`, `WindowApi`, `InputApi`,
`AudioApi`, `ClipboardApi`, `SurfaceApi`, `LogApi`, `ConfigApi` плюс
обязательные `ThreadApi`, `SyncApi`, `AssetApi`, `VulkanDriverApi`
(`runtime/platform/platform.h`). `platform/fake` — unit-тесты, включая
многопоточные сценарии; `dxvk_win32` — отдельная vendor-граница.
`ConfigApi` читает app-private `wc3.cfg` один раз до guest startup:
`key=value`, `#`-комментарий с начала строки, ASCII-trim, ключи
`[A-Za-z0-9_.-]` (case-sensitive, ≤63 байт), значения ≤255 байт, последний
дубликат побеждает, ≤64 записей; отсутствие файла — пустая конфигурация, а не
ошибка. `AudioApi` — AAudio callback sink с lock-free SPSC ring: `write`
принимает столько кадров, сколько помещается, `set_callback` добирает
недостающее; потеря устройства переоткрывается supervisor'ом с backoff, после
N неудач — явный silent sink (§8.3).
### 6.6. Handle-таблицы
Фиксированные массивы слотов на bootstrap; lookup по индексу thunk'а —
целочисленная арифметика без локов; создание/удаление — короткая
синхронизация или CAS-freelist; состояние объекта — per-object mutex/condvar.
Реестр **двухфазный**: `BOOTSTRAP` + `LATE_BIND` (Miles `AIL_*`, append-only,
до первого `CreateThread`), затем `frozen=1` (`docs/HLE_MODULES.md` H8).
Порядок регистрации пинится `registry_sha256` (ABI-3.3).
## 7. Графика: thin bridge D3D9 → DXVK
### 7.1. Единственный путь
Guest видит `IDirect3D9`/`IDirect3DDevice9`; frontend — только
`hle_d3d9_thin*`. В APK статически DXVK **1.10.3** и **3.1.1** с раздельными
symbol namespaces; внешних `d3d9.dll` и подмены провайдера нет, D3D8 не
линкуется. Решение о dual — **ADR-004/007 `provisional`**; норма
`docs/GRAPHICS_DECISION.md` (ADR-023, GB-1…GB-10).
### 7.2. Symbol isolation (as-built, не обещание)
`libdxvk1.a`/`libdxvk3.a`, `objcopy --prefix-symbols` по каждому member,
`--whole-archive ... --exclude-libs,ALL`. Статические конструкторы **обоих**
DXVK выполняются при загрузке `.so`; `dxvk_win32.cpp` архивируется
непрефиксованным внутри `libdxvk1.a`, то есть shim и его глобалы общие.
Контракт `dxvk_shim_reset()` и аудит конструкторов — GB-4/GB-5.
### 7.3. Селектор и fallback
`vkEnumerateInstanceVersion` даёт версию loader'а, не устройства. Порядок:
`VulkanDriverApi` → `vkEnumeratePhysicalDevices` (`apiVersion` + extensions)
→ ≥1.3 ⇒ DXVK 3.1.1, иначе 1.10.3 → при ошибке создания device или потере
на первом present **однократный fallback** с диагностикой в debug. Только
один бэкенд создаёт instance/device; release не читает `WC3_DXVK_VERSION`.
До GB-4.3/GB-5.6 fallback остаётся декларацией.
### 7.4. Vulkan ICD: system и Turnip
Запрет rev 1 на «подмену провайдера» относился к `d3d9.dll`. Рабочий билд
использует adrenotools (`WC3_VULKAN_DRIVER=turnip|system`); на Adreno 725
Turnip даёт Vulkan 1.4, системный драйвер — 1.3. `VulkanDriverApi` — явная
разрешённая точка выбора ICD; хук ставится до загрузки libvulkan.so (E7.2).
### 7.5. WSI, caps, staging
`ANativeWindow` через `SurfaceApi`; surface loss/resize обрабатывает
`GuestRunner`. D3D9 caps мапятся без «примерно», неподдержанная функция —
документированное поведение, а не тихий no-op. `LockRect`/`Lock` выдают
гостю указатель в staging (младшие 4 ГБ), `Unlock` копирует в DXVK;
владелец буфера один, при межпоточной передаче — атомарный индекс.
## 8. Android host: lifecycle, ввод, данные
### 8.1. GuestRunner
`Created → Running ⇄ Paused`, `SurfaceLost`/`AudioDeviceLost`, `Stopping →
Stopped`, плюс `Recreated` (E9.1). Callbacks публикуют события в mailbox и
не исполняют guest/HLE в своём контексте. `Paused` останавливает кадровой
цикл, а не удаляет TEB; таблица `state × action` — E9.2.
### 8.2. Input и controls
`InputEvent {type, key, x, y, pointer_id, time_ns}`; источники — GameActivity
UI, touch overlay, IME, gesture layer (vendor winlator-input, MIT). Overlay
генерирует события, `user32` превращает их в Win32-сообщения; прямой
`SendMessage` из UI запрещён. Политика gamepad/клавиатуры — E-долг G12.
### 8.3. Audio
`AudioSink {open, start, write, pause, close}` поверх AAudio; callback или
blocking-write фиксируется device-тестом; ring lock-free acquire/release.
Потеря устройства → close + reopen с backoff, после N неудач — silent sink.
### 8.4. Игровые данные и сохранения (обязательная подсистема)
Четыре лицензионных MPQ (`war3.mpq`, `War3x.mpq`, `War3xlocal.mpq`,
`War3Patch.mpq`) из app-specific/SAF-каталога; `AssetApi` проверяет полноту на
старте, `FileApi` мапит гостевые пути, MPQ читаются **лифтнутым Storm.dll**.
Saves/replays/registry — app-private dir, переживают pause и surface loss.
Byte-compare Windows↔Android с нормализацией V4.2; онбординг — E9.4.
### 8.5. Frame pacing
`TimeApi` от `CLOCK_MONOTONIC`; `Present` → DXVK → `ANativeWindow` —
единственный источник кадрового темпа, host не впрыскивает `sleep`.
Полная модель времени, коэффициенты и критерий независимости скорости от
устройства — `docs/EXACTNESS.md` E3; **до E3.2/E3.6 frame hash не является
гейтом точности**.
## 9. Сборка, CI, диагностика
### 9.1. Build pipeline
Норма — `docs/BUILD_PIPELINE.md` (BP-1…BP-11): самостоятельность target-дерева,
вендоринг с `VENDOR_COMMIT`, `wc3build.py`, идентичность сборки, link graph,
CI, измеримый конец cutover (BP-4.4). Проверенный NDK/PowerShell конвейер
переносится, не переписывается; toolchain пинуется хэшем. `wc3build.py` и
`check_manifest.py` отсутствуют (G-13 = `missing`). Bitwise-идентичное `.so`
не гейт; противоречие 216/~253 МБ закрывает GB-5.1.
### 9.2. CI
**T0** asset-free (docs/arch/layout/unit, `--policy`/`--concurrency`/`--release`);
**T1** difftest против эталонных захватов на private runner (лицензионный PE +
lift, без Windows-сборки); **T2** device E2E на self-hosted. ADR-006 запрещает
product build на публичном runner. Без устройства `unavailable` (V1.2).

### 9.3. Debug и release
Норма — `docs/BUILD_VARIANTS.md` (BV, ADR-030): release `wc3.recomp`,
debug `wc3.recomp.debug`; один `--variant`; debug сохраняет `-O2`.
Центральный `runtime/diag/wc3_debug.h`: `#ifndef WC3_DEBUG -> 0`,
теги `#if WC3_DEBUG && WC3_DEBUG_<ЦЕЛЬ>` по реестру подсистем;
логи/probes/dumps/trace/debug-поля/env/генераторы — только под тегом.
Запрещены `#ifdef`, `if (debug)` и нетегированные guard'ы.
Release: `-DWC3_DEBUG=0 -DNDEBUG`; `--release` к `runtime/`,
`apps/` и сгенерированному C; DXVK/third_party — по allowlist.
### 9.4. Crash
Release — fatal abort с кодом, без logcat/dump; debug — под `WC3_DEBUG_CRASH`.
`WC3_DOGFOOD` как вариант упразднён; отказ обоих DXVK — GB-4.7.
## 10. Верификация
### 10.1. Три уровня
**T0** asset-free (каждый коммит), **T1** difftest headless-DUT **H** против
эталонных захватов **G** оригинальной игры (каждый cutover-шаг, без
рекомпиляции под Windows, без устройства — ADR-026/027, V4.7), **T2** device
E2E (перед релизом и после графических правок).
`unavailable ≠ pass` действует на всех уровнях (ADR-022, V1.2); T1 —
обязательный быстрый контур, device не единственный источник истины
(ADR-017).
### 10.2. Гейты
Нормативный владелец — `docs/VERIFICATION.md` (V1–V10, V4.7, ABI-11):
инвариант исполнимости гейта, исполнимый DUT T1, гейт эквивалентности
сгенерированного C G-15, матрица `G-01…G-15` со статусами
`enforced`/`declared`/`missing`, oracle identity, difftest, acceptance-прогон
V7 и трассировка DoD.
Канонический раннер не содержит ослабляющих флагов (V1.4); состояние на
2026-11-06 — `enforced` 3 / `declared` 6 / `missing` 6, V7 ожидаемо не зелёный.
### 10.3. Definition of done
Android boot → меню → карта → 60 с → save → load → exit; HLE ABI и layout
проверены; 36 thread/sync с точной семантикой; инвариант блокировок соблюдён;
DXVK matrix зелёный; release без нашего debug-кода; ни одного неизвестного
dispatch target; семантические инварианты V5.9 зелёные (V6.6).
Трассировка каждого пункта на гейт — V9.
## 11. ADR (rev 2)
**Вынесено в `docs/ADR.md`** (M2): полная таблица ADR-001…ADR-028 с
метаданными `{id, status, date, owner, supersedes, evidence}`. Статусы —
GB-1; ADR-004/007 = `provisional`, ADR-022/023/024 — новые.
## 12. Риски (в порядке приоритета)
**Вынесено в `docs/RISKS.md`** (M2). Ключевые: гонки guest-shared таблиц,
регрессия при переносе, объявленный гейт, drift `argc`, MPQ, отказ DXVK 3.1.1,
неизвестная цель dispatch, debug-код в release, legacy-пути, размер `.so`.
## 13. Известные ограничения rev 2 и кандидаты rev 3
**Вынесено в `docs/BACKLOG.md`** (M2) вместе с §13.1. Таблицы A1–A5 … G1–G13
описывают **работы**, а не отсутствующую спецификацию: требования уже
нормативно зафиксированы в `docs/EXACTNESS.md`, `docs/VERIFICATION.md`,
`docs/GUEST_ABI.md`, `docs/GRAPHICS_DECISION.md` и `docs/HLE_MODULES.md`.
F1 (дублирование инвариантов) и F2 (числа без привязки к артефакту) закрыты
M2; F3 (монолит) закрыт сокращением хаба до ≤400 строк R4.
### 13.1. Второй проход: аспекты, не охваченные таблицей выше
G1…G13 (смерть процесса, FP-детерминизм, модель времени, bootstrap,
терминальный отказ, действия состояний, инкрементальный перелифт, граница
C/C++, управление ADR, ввод/онбординг, kill criteria) — `docs/BACKLOG.md`.
## 14. Ссылки
- Индекс и приоритет владельцев: `docs/README.md`.
- Снимки и решения: `docs/AS_BUILT.md`, `docs/ADR.md`, `docs/RISKS.md`, `docs/BACKLOG.md`.
- Нормы: `docs/VERIFICATION.md` (V), `docs/GUEST_ABI.md` (ABI),
  `docs/EXACTNESS.md` (E), `docs/GRAPHICS_DECISION.md` (GB),
  `docs/HLE_MODULES.md` (H), `docs/PERFORMANCE.md` (P),
  `docs/PERFORMANCE_CONTRACT.md` (PM),
  `docs/BUILD_PIPELINE.md` (BP), `docs/FILE_LAYOUT.md` (R).
  Нормативный §8 по потокам — `docs/THREAD_AUDIT.md` (TA).
- Evidence: `docs/THREAD_AUDIT.md`, `docs/DXVK_THREADS.md`.
- Корень: `AGENTS.md`, `README.md`, `PLAN_REFACTOR_1.27B.md`.
