# Threading audit — WC3 1.27b

**ADR-026:** рекомпилированный Windows-хост не собирается; T1 ниже означает
difftest против эталонных захватов оригинальной игры, а не прогон хоста.

Статус: **partially confirmed, rev 2.4** (2026-11-06). Заменяет версию
«unavailable». Часть facts подтверждена исходниками as-built и логами
устройства; часть остаётся за T2-гейтом. Архитектурное решение —
`docs/ARCHITECTURE_1.27B.md` §3 и ADR-016/ADR-018. Rev 2.4 добавляет
§9 (TA-12…TA-18): исполнимость потоковой модели в T1, реентерабельность,
остановку при INFINITE-wait, конфликт DllMain с инвариантом блокировок,
суммаризацию корпуса для L2, владение staging и тепловой дрейф.

## 1. Определение статусов

* `pass` — контракт реализации подтверждён исходниками as-built и перенесён в
  target; **не** является утверждением о поведении на устройстве.
* `unavailable` — нужны PE/устройство: фактические call sites, lifetimes,
  таймауты или DllMain. Не `pass`.

`tools/check_thread_audit.py --corpus 1.27b --allow-unavailable` — гейт T0 для
текущего состояния. Полный `pass` по всем строкам требуется только для DoD.

## 2. Архитектурное решение (не «аудит ради удаления»)

Модель rev 1 «runtime однопоточный, один TEB, один стек, никаких локов»
**отменена** (`reports/CRITIQUE_FILTER_THREADING.md:3`). Она противоречила
фактам: guest threads реально создаются, а `CRITICAL_SECTION` в as-built
реализован рекурсивным `pthread_mutex`, потому что «с реальными worker-потоками
no-op lock портит общее состояние» (`hle_android.c:5602`).

Принятая модель:

* `CreateThread`/`_beginthreadex` → реальный host thread; у каждого свой `CPU`,
  guest stack, TEB, TLS и dispatcher state (`GUEST_THREAD_MAX = 24`);
* guest-visible sync — HLE-объекты с точной семантикой Win32; заглушки
  запрещены (ADR-008);
* **инвариант**: ни один host lock не удерживается при `dispatch()`, обратном
  вызове в гость или ожидании guest-visible объекта. Локи в guest-пути
  (`runtime/cpu`, `runtime/guest`, `runtime/hle/abi`) запрещены, в HLE-объектах
  и vendor-коде — разрешены и обязательны.

Кооперативное схлопывание потоков в один OS-поток запрещено: оно меняет
тайминги гостя. Удаление thread/sync кода до evidence запрещено.

## 3. As-built evidence (зафиксировано, не гипотеза)

| Факт | Источник |
|---|---|
| 18 guest `CreateThread`, 0 `CreateSemaphoreA`, 0 `ReleaseSemaphore`, все waits — events/mutex | `PROGRESS.md`, sync probe 2026-11-04 |
| `CreateThread -> gid=4096`, `_beginthreadex -> gid=4100`, у каждого CPU/stack/TEB/TLS | `docs/ANDROID.md`, Round 1 |
| `pthread_create` + `GThread g_threads[24]` + per-thread TEB/TLS | `hle_android.c:5482-5595` |
| `CRITICAL_SECTION` → рекурсивный `pthread_mutex` в side table | `hle_android.c:5600-5665` |
| Исторический баг: stateless event + немедленный `WAIT_OBJECT_0` сломал все MPQ-чтения (`SFileReadFile` → `ERROR_HANDLE_EOF`, пустой `font.gid`) | `hle_android.c:2096-2110` |
| Stubs, которые надо заменить: `TerminateThread`, `SuspendThread`, `ResumeThread` возвращают константу | `hle_android.c:4065-4073` |
| 36 thread/sync-записей из 426 в HLE-таблице | `config/hle_table.1.27b.tsv` |
| Эталонные захваты оригинальной игры как контроль (исторически — Windows host) | `PROGRESS.md`, 2026-11-04 |

Исторический баг с event'ами — прямое доказательство, что правило «точная
семантика sync» является требованием корректности, а не педантичностью.

## 4. Таблица аудита

Восемь колонок обязательны: удаление строки не делает аудит зелёным
(`check_thread_audit.py` проверяет наличие всех механизмов).

| Импорт / механизм | Call sites | Lifetime | Waits | TLS | DllMain | Evidence | Статус |
|---|---|---|---|---|---|---|---|
| `CreateThread` / `_beginthreadex` | 18 + 1 подтверждено логом | до `ExitThread` или процесса | join через wait | per-thread | attach на старте | `hle_android.c:5482-5595`; `g_defs` 8582; `docs/ANDROID.md` Round 1 | pass |
| `ExitThread` / `GetExitCodeThread` | неизвестно | конец guest thread | joiners | — | detach | отсутствуют в `g_defs` 8560-8610; нужен PE/device | unavailable |
| `WaitForSingleObject` | Storm read-ahead + Miles | до сигнала/таймаута | INFINITE и timeout | — | — | `h_wait_one:2468`, `h_wait_mutex:2395`, `h_create_event:2321` | pass |
| `WaitForMultipleObjects` | неизвестно | до `bWaitAll`/первого | timeout | — | — | `h_wait_many:2642`, возврат `WAIT_OBJECT_0+i`:2712 | pass |
| `CreateEvent` / `SetEvent` / `ResetEvent` | Storm read-ahead | до close | manual/auto reset | — | — | `hle_android.c:2321-2325`; баг stateless event:2096 | pass |
| `CreateMutex` / `ReleaseMutex` | подтверждены waits | до close | recursive, abandoned | — | — | `h_create_mutex:2772`, `h_wait_mutex:2395`, `g_mutexes` | pass |
| `CreateSemaphore` / `ReleaseSemaphore` | 0 вызовов в замере | до close | count/max | — | — | `h_create_semaphore:2797`; `PROGRESS.md` sync probe; семантика счётчика не проверена | unavailable |
| Critical sections | используется guest CRT | до delete | try/leave, recursive | — | — | `cs_get:5610`, `h_enter_cs:5656`, `h_leave_cs:5661`, `CS_MAX 512` | pass |
| `TlsAlloc` / `TlsSetValue` / `TlsGetValue` | неизвестно | per-thread | — | да, `tls[64]` | — | `tls_slot:5490`, `h_tls_*:4093-4119` | pass |
| `DllMain` attach/detach | неизвестно | per-thread/module | — | да | да | PE-загрузчик в as-built; детальные attach/detach не записаны | unavailable |
| `TerminateThread` / `SuspendThread` / `ResumeThread` | неизвестно | сразу | — | — | — | stubs: `hle_android.c:4065-4073`; требуется точная семантика или fatal | unavailable |
| `Interlocked*` / `Sleep` | много | — | sleep | — | — | `h_interlocked_*:3823-3850`, `h_sleep:2808` | pass |
| `GetCurrentThread*` / приоритеты | неизвестно | — | — | да | — | `g_defs` 8596-8605; приоритеты не мапятся на host | unavailable |
| Host threads (UI/input/overlay/canvas/AAudio) | существуют | процесс | mailbox | — | — | `game_activity_host.c` pthread_create и mutex; переносятся по §3.5 архитектуры | pass |
| DXVK internal threads | см. `DXVK_THREADS.md` | vendor | — | — | — | vendor-owned исключение, guest state не трогают | unavailable |

## 5. Инварианты реализации (обязательные для cutover)

1. `immutable` (registry, lift hash, layout) — без синхронизации.
2. `thread-private` (`CPU`, stack, TEB, TLS, `g_disp_depth`, `g_tail_pending`)
   — TLS, без разделения.
3. `guest-shared` (handle slots, events, mutexes, semaphores, CS) — per-object
   mutex/condvar; короткая синхронизация только при создании/удалении слота.
4. `host-boundary` (mailbox, audio ring, overlay canvas) — lock-free atomics
   acquire/release; spinlock запрещён.
5. Debug assertion в `hle_call`/`dispatch`: «мы не под удержанным host lock».
   Static check: `dispatch()` не вызывается из критической секции.
6. Ни одного no-op wait, ни одного `WAIT_OBJECT_0` без состояния объекта.

## 6. Gate

| Уровень | Проверка |
|---|---|
| T0 | `python tools/check_thread_audit.py --corpus 1.27b --allow-unavailable`; unit-тесты `fake` platform с двумя guest threads; `--concurrency` по §3.3 архитектуры |
| T1 | difftest маркеров/таймингов против эталонных захватов оригинальной игры при любом изменении sync (без Windows-сборки) |
| T2 | `WC3_DEBUG_HLE_SYNC`/`WC3_SYNC_TRACE`, boot → menu → map → save/load: записать call sites, thread IDs, waits, TLS, DllMain; сверить с таблицей |

Без T2 статус строк `unavailable` сохраняется; это не `pass`, но и не BLOCKED
проекта, пока зелёны T0/T1.

## 7. Открытые долги (P0 для Step 3–5)

1. Заменить stubs `TerminateThread`/`SuspendThread`/`ResumeThread` точной
   семантикой или fatal (ADR-008).
2. Реализовать `ExitThread`/`GetExitCodeThread` и join на thread handle.
3. Проверить счётную семантику semaphore, даже если текущий замер показывает
   0 вызовов.
4. Снять DllMain attach/detach и TLS call sites на устройстве.
5. Перенести 36 записей в `runtime/hle/win32/thread` и `/sync`, сохранив
   as-built поведение; гейт — T0 unit + T1 по эталонным захватам, device boot = T2.
6. **Утечка слотов**: `guest_thread_proc()` (`hle_android.c:5530`) ставит
   `running=0`, но не сбрасывает `used`; `used=0` есть только на путях ошибок
   (5571, 5576). После 24 созданий `CreateThread` вернёт 0. Нет ни
   `pthread_join`, ни сигнала thread handle при завершении; диапазон
   semaphore/thread-handle объявлен stateless (1429).
7. **Гонки на `hle_call`**: `e->calls++`, `g_last_cpu`, `g_ring[]`,
   `g_ring_pos`, `g_ring_len` пишутся безусловно (9222-9230) на каждом вызове
   из всех guest threads; `g_sync_next++` (955) и `gh_new()` (973) — без
   синхронизации. Инвентарь: `tools/concurrency_audit.py`, 186 глобалов /
   140 разделяемых с записями / **10 подтверждённых находок**,
   `reports/generated/concurrency_audit.static.json`.
8. Гейт `concurrency_audit.py --strict` входит в T0; правило — ни одного
   разделяемого изменяемого глобала на пути `hle_call` (ARCHITECTURE §3.7).
9. **Host-стек и жизненный цикл потока**: `pthread_create(..., NULL, ...)`
   (`hle_android.c:5574`) создаёт guest thread со стеком по умолчанию (~1 МиБ)
   при 8 МиБ main guest stack; `triage.c:363` знает вердикт
   `HOST STACK OVERFLOW`. Нет `pthread_attr_setstacksize`, нет detach/join,
   нет инициализации TLS SoftFloat на новом потоке. Требуется контракт P0-9.
10. **Mailbox не соответствует числу producers**: `runtime/ipc/mailbox.h`
    объявляет SPSC, `wc3_mailbox_push()` пишет `slot[tail]` и делает
    `atomic_store(tail, next)` без CAS, тогда как producers несколько
    (UI, input, overlay, IME, canvas). Решение P0-8 — per-producer SPSC
    очереди с одним consumer'ом, а не «один MPSC».
11. **Инверсия приоритетов**: guest-visible mutex/CRITICAL_SECTION должны
    иметь `PTHREAD_PRIO_INHERIT`, иначе main guest thread, ждущий событие от
    low-priority worker'а, блокируется на неопределённое время.

## 8. Нормативный контракт потоков (TA-1…TA-11)

**Оценка глубины.** Модель исполнения проработана хорошо: четыре класса
состояния (§3.1), реальные guest threads, инвариант блокировок (§3.3),
точная семантика Win32 (§3.4), 36 записей и 8-колоночная таблица аудита,
per-producer SPSC с тестом 4×20 000 сообщений. Слабое место — **исполнение
и доказательства**: 6 строк `unavailable`, G-04 ослаблен флагом
`--allow-unavailable`, `--concurrency` проверяет зоны и грубую эвристику
«лок и dispatch в одном сегменте функции» (`check_arch.py`), но не граф
вызовов, не время жизни лока и не runtime-assertion §5.5; стресс-теста E5.3
с ≥2 guest threads в `tests/` нет. Итог: модель ~8/10, исполнимая проверка
~4/10. Идентификаторы `TA-n` стабильны; владелец раздела — этот документ.

* **TA-1 (матрица владения).** Четыре класса §3.1 получают конкретные объекты
  и ровно одного владельца на каждый file-scope глобал as-built (H6.3):
  | Класс | Объекты | Разрешённая синхронизация | Владелец |
  |---|---|---|---|
  | `immutable` | `HleRegistry`, `g_lift[]`, `corpus.sha256`, layout, config | нет; прямая индексация после `frozen=1` | bootstrap |
  | `thread-private` | `CPU`, guest stack, TEB, TLS[64], `g_disp_depth`/`pending`/`draining` | TLS, без разделения | owning guest thread |
  | `guest-shared` | `g_handles[]`, events/mutexes/semaphores/CS, `g_hle[]`, `g_heap_lock`, `g_sync_next`, `GThread[24]`, guest memory, staging | per-object mutex/condvar или CAS-freelist; короткая синхронизация только при create/delete слота | `hle_legacy_shim` → `kernel32/{thread,sync,memory}` |
  | `host-boundary` | `g_uq`, `g_key_state`, UI/input/overlay/canvas mailbox, audio ring, overlay canvas | lock-free atomics acquire/release, `_Alignas(64)`, spinlock запрещён | owning guest thread как единственный consumer |
  `arg32`/`rd32g` — чистые чтения guest-стека, состояния не хранят; владелец
  фиксируется как `hle/abi`. До M5 разделение обязательно (H6.3).

* **TA-2 (контракт host-потока, P0-9).** `ThreadApi.create` обязан:
  `pthread_attr_setstacksize(8 МиБ)` для main guest стека, отдельный guest
  стек 2 МиБ из `thread_stacks[]`, инициализация TEB/TLS/`CPU`,
  `cpu_x87_init()` и TLS SoftFloat **на каждом** новом потоке,
  `PTHREAD_PRIO_INHERIT` для guest-visible mutex/CS, detach либо join по
  thread handle, сброс `GThread.used` и сигнал waiters при выходе,
  `ExitThread`/`GetExitCodeThread`, запрет миграции по P1-9. Обязателен
  `tests/thread_sync_test.c` с ≥2 guest threads: create/join, recursive CS,
  event/semaphore, отсутствие дубликатов handle и утечки 24 слотов.
  Статус: `missing` (G-04/G-14).

* **TA-3 (уровни исполнения инварианта §3.3).**
  | Уровень | Что проверяет | Статус |
  |---|---|---|
  | L0 | классификация зон (`GUEST_ZONES`, `LOCK_FREE_ZONES`, `HLE_PREFIX`) и запрет host lock/spinlock по регекспам | `implemented` (`check_arch.py --concurrency`) |
  | L1 | эвристика «лок и `dispatch()`/`hle_call()` в одном сегменте функции» (`_function_segments`) | `implemented` **как эвристика**, не доказательство: нет времени жизни лока, меж-TU вызовов и анализа через указатели |
  | L2 | граф вызовов с пометкой владения + debug-assertion в `hle_call`/`dispatch` «мы не под удержанным host lock» (§5.5) + именной allowlist vendor-кода | `missing` |
  До L2 инвариант не переводится в `verified`; G-01 остаётся `enforced`
  только для L0/L1, и `--concurrency` не ослабляется флагами.

* **TA-4 (стресс-тест и модель памяти).** E5.2/E5.3 требуют один
  воспроизводимый сценарий ≥2 guest threads на общих структурах
  (handle table, events, MPQ read-ahead, `hle_call`) с контролем расхождения
  и, где доступно, под TSAN. P1-7 (ослабление до `acquire/release`) и P1-9
  (affinity) принимаются только после зелёного TA-4; до него E5.1 остаётся
  `seq_cst`. Метрики contention — по PM-1/PM-7. Владелец G-14, `missing`.

* **TA-5 (mailbox).** `runtime/ipc/mailbox_set.*` (5 `_Alignas(64)`
  per-producer SPSC, один consumer, фиксированный приоритет, ограниченный
  батч) = `implemented`, тест 4×20 000 сообщений зелёный. Не закрыто:
  политика переполнения (drop/overwrite/backpressure с явным владельцем),
  `mailbox_backlog_max` (PM-1), приоритет producer'ов при starvation,
  padding `head`/`tail` одиночного `Wc3Mailbox` (P1-21), запрет аллокаций
  в producer-контексте. Статус политики: `missing`.

* **TA-6 (шесть `unavailable`).** Каждая строка §4 обязана получить
  `{evidence_artifact, device_or_PE_requirement, owner_step, gate}` и
  сниматься `WC3_DEBUG_HLE_SYNC`/`WC3_SYNC_TRACE` на T2 (§6).
  `unavailable ≠ pass` (V1.2); `--allow-unavailable` допустим только в
  отчётном прогоне, G-04 остаётся `declared` до снятия всех шести.

* **TA-7 (vendor-потоки).** `DXVK_THREADS.md` §2 (5 `unavailable` строк)
  закрывается GB-5.7: снять count/owner/lifetime/queue/staging/shutdown по
  исходникам обеих версий DXVK, проверить, что статические конструкторы
  невыбранного бэкенда не создают потоки/файлы/сокеты/устройства, и что ни
  один DXVK-поток не читает `CPU`/HLE/handle table (GB-4.5/GB-4.6).
  Именной allowlist `--concurrency` обязателен; T1 на headless-DUT ничего
  не говорит о vendor-потоках (GB-7).

* **TA-8 (трассировка и статусы).** TA-1 → H6.3/M5; TA-2/TA-4 → G-04/G-14;
  TA-3/TA-11 → G-01; TA-5 → G-06/P0-8; TA-6 → G-04/T2; TA-7 → GB-5.7/T2;
  TA-9 → G-14; TA-10 → G-11/T2; TA-12…TA-18 → G-09/G-14/G-04/G-01/G-15/G-10/T2
  по таблице §9. Все новые id имеют статус `specified` или `missing`; ни один
  новый `G-xx` или ADR не вводится, матрица V2 (`enforced` 3 / `declared` 6 /
  `missing` 6) не меняется.

* **TA-9 (протокол стресс-теста TA-4).** Сценарий детерминирован:
  фиксированный seed и вход в guest-тиках (E2), ≥2 guest threads на общих
  структурах (handle table, events, MPQ read-ahead, `hle_call`), а
  длительность и число итераций фиксируются в отчёте и обосновываются
  наблюдаемым покрытием, а не удобством. Проверяются: расхождение
  снимков `CPU`/guest memory (V5.3), отсутствие гонок (TSAN, где доступно),
  отсутствие дубликатов handle и утечки `GThread.used`, инвариант §3.3
  уровня L2 (TA-3). Выход — `{iterations, divergence_count, race_count,
  max_wait_ns, contention, artifact_sha256, verdict}` по V1.6. `pass`
  только при нулевой дивергенции и нулевых гонках; отсутствие
  инструмента = `unavailable`, не `pass`. Владелец G-14, статус `missing`.

* **TA-10 (слепое пятно детерминизма).** V4.7.2 виртуализирует guest-тики,
  а TA-4/TA-9 фиксируют порядок планирования: оба контура по конструкции
  **не видят** регрессий, проявляющихся только при реальном wall-clock и
  произвольном планировании (гонки по времени, frame pacing, инверсия
  приоритетов на big.LITTLE). Поэтому T2 обязан содержать **второй** прогон
  того же сценария с jitter/adversarial scheduling и снятием
  `wait_ns/frame`, `cs_contention/frame`, p50/p95/p99 кадра; E3.6
  проверяется только на устройстве. Без устройства статус `unavailable`,
  не `pass` (V1.2); зелёный TA-9 не заменяет TA-10. Владелец G-11/T2,
  статус `missing`.

* **TA-11 (контракт уровня L2 из TA-3).** Помимо «граф вызовов +
  assertion» фиксируется механика: (a) аннотация владения на каждой
  функции HLE (`none | per-object | global`) и поимённый allowlist
  DXVK/`dxvk_win32`; (b) статический граф вызовов по всем TU `runtime/`
  с пометкой рёбер `dispatch`/`dispatch_jmp`/`hle_call`/callback-in-guest
  и проверкой, что ни один путь от захвата host lock'а до освобождения
  не содержит такого ребра (исключение E8.4 — явно); (c) runtime-assertion
  под `#if WC3_DEBUG` в `hle_call`/`dispatch` с thread-local счётчиком
  удерживаемых host lock'ов; (d) отчёт T0 по схеме V1.6 с
  `{zone, edge, verdict, artifact_sha256}`. Статус `missing`; G-01
  остаётся `enforced` только для L0/L1 до появления TA-11.

## 9. Второй аудит глубины: исполнимость и границы модели (TA-12…TA-18)

§2–§8 дают модель (4 класса состояния, 36 импортов, L0/L1/L2, TA-9), но
6 строк `unavailable`, G-04 ослаблен `--allow-unavailable`, L2 = `missing`,
стресс-теста нет. V4.7.2 фиксирует планирование, поэтому при недоступном
устройстве покрытие гонок по времени в T1 равно нулю. Модель ~8/10,
исполнимость ~4/10. Rev 2.4 добавляет TA-12…TA-18 без нового `G-xx`, ADR и
повышения статусов V2.

* **TA-12 (детерминированные враждебные расписания в T1).** V4.7.2 требует
  фиксированного порядка планирования, а TA-10 относит всю проверку jitter
  на T2. `platform/headless` обязан предоставлять конечный набор
  детерминированных расписаний с фиксированным seed: несколько квантов
  round-robin, инверсия приоритетов, переупорядочение wake-up и задержка
  публикации mailbox. difftest (V5) исполняет один и тот же сценарий на
  каждом расписании и сравнивает с G по E11; набор и `schedule_sha256`
  входят в identity V3.3. Это **уточняет, а не отменяет** V4.7.2: каждое
  расписание по-прежнему детерминировано, виртуальное время, запрет
  wall-clock и `getenv` сохраняются, поэтому новый ADR не требуется.
  TA-10 не заменяется: произвольное планирование и thermal jitter остаются
  за T2. Владелец — Step 0 / G-09 / G-14; статус `missing`.
* **TA-13 (реентерабельность HLE → guest на одном потоке).** ABI-4 фиксирует
  одного владельца состояния переходов на guest thread, но умалчивает про
  вложенные вызовы: обработчик HLE (window proc, `DllMain`, COM callback)
  может снова войти в лифтнутый код на **том же** host thread. Обязателен
  стек вложенных вызовов гостя: `wc3_guest_enter/leave` сохраняют и
  восстанавливают `{depth, pending_valid, pending_kind, pending_target,
  draining}` на каждой такой границе; отложенная tail-цель внутреннего
  вызова дренируется **внутри** него и не может утечь во внешний drain-цикл.
  `HleEntry` получает флаг `may_reenter_guest` (это runtime, не корпус —
  ABI-2 не нарушается); обработчик без флага, вошедший в гостя, = debug
  assertion / fatal. Перечень тестов G-14 (ABI-6) дополняется случаем
  (h) nested HLE→guest. Владелец — Step 5b / G-14; статус `specified`.
* **TA-14 (остановка при INFINITE-wait).** E9.2 предписывает «join всех guest
  threads», но поток в `WaitForSingleObject(INFINITE)` или
  `WaitForMultipleObjects(bWaitAll)` так не останавливается: `Stopping`
  зависает до destroy device. Обязателен протокол: `Stopping` →
  `cancelling=1` → wake-all по всем HLE-объектам с документированным
  Win32-совместимым статусом (не `WAIT_OBJECT_0`, E8.2) → bounded join с
  дедлайном по PM-11 → debug dump незавершённых потоков с `guest_eip` и
  владельцами локов → release fatal. Порядок: до destroy device, close sink
  и drain mailbox; ни один host lock не удерживается при wake-all (§3.3).
  Владелец — Step 5 / G-04 / G-14; статус `missing`.
* **TA-15 (DllMain и loader lock).** Строка `DllMain` в §4 = `unavailable`,
  но само явление конфликтует с §3.3: Win32 выполняет
  `DLL_THREAD_ATTACH`/`DETACH` под loader lock, то есть наивная эмуляция
  удерживает host lock **через guest callback**. Целевая стратегия —
  маршалинг attach/detach в owning guest thread через набор SPSC (TA-5), без
  host lock в guest-пути; порядок событий фиксируется расписанием TA-12.
  Альтернатива (явное исключение с allowlist по образцу E8.4) требует
  отдельного ADR с областью и критерием отката (E10.2) и до принятия
  запрещена. Строка §4 остаётся `unavailable`, пока не снята записью T2,
  но конфликт зафиксирован. Владелец — Step 5 / G-04.
* **TA-16 (L2 через суммаризацию корпуса).** TA-11 требует статический граф
  вызовов по всем TU `runtime/`, однако ребро `dispatch()`/`dispatch_jmp()`
  уходит в 2,4 ГБ сгенерированного C, который T0 по ABI-11.3 и BP-6.4
  **не сканирует**. Граф, обрывающийся на `dispatch()`, не может доказать
  «ни одного пути от захвата лока до `hle_call`» — ровно тот путь, который
  и интересен. Обязательна генерация лифтером **per-function summary**
  `{va, may_hle_call, may_callback_in_guest, max_tail_depth}` во время лифта,
  пинящаяся `lift_rev` и `corpus.sha256`; L2 компонует граф `runtime/` с этими
  свёртками, а не с телами функций. До появления summary уровень L2 остаётся
  `missing`, G-01 не переводится в `verified` (TA-3). Инварианты проверяются
  в генераторе, не регекс-линтом корпуса в T0. Владелец — Step 5b / G-01 / G-15.
* **TA-17 (владение staging-буфером).** §7.5 и `DXVK_THREADS.md` §3 передают
  `LockRect`/`Lock` буфер между гостем и DXVK через «атомарный индекс
  двойного буфера». Один индекс не задаёт владельца и уязвим к ABA между
  кадрами N и N+2: гость может начать писать в буфер, который ещё читает
  DXVK. Обязательна машина состояний с поколением
  `FREE → GUEST_WRITING → GFX_READING → FREE`, переход — release/acquire
  (E5.1), поколение проверяется при `Unlock`/submit, повторная публикация
  одного поколения = fatal. Гость никогда не получает указатель на буфер в
  состоянии `GFX_READING`; vendor-потоки не читают `CPU`/HLE (GB-4.6).
  Стресс-кейс добавляется в TA-4/TA-9. Владелец — Step 7 / G-04 / T2.
* **TA-18 (cpuset, тепловой дрейф и публикация immutable).** P1-9/TA-2
  фиксируют affinity, но Android может перекрыть её cpuset'ом и термальным
  троттлингом; значит ни один инвариант корректности (§3.3, E5) не имеет
  права зависеть от привязки к ядру. Обязательны повторная заявка affinity
  на смене foreground/thermal с записью `{cpuset, thermal_status,
  apiVersion, driver_version}` в PM-8/PM-15 и запрет считать ускорение
  evidence, если между A и B изменился любой из этих параметров (PM-19).
  Отдельно: `frozen=1` (E7.3) публикуется release-store до создания любого
  потока вне `ThreadApi` (AAudio callback, vendor), читающего immutable
  таблицы; `pthread_create` даёт happens-before только своим детям.
  Владелец — Step 0 / G-10 / G-01.

| Id | Минимальный исполнимый гейт | Владелец |
|---|---|---|
| TA-12 | `schedule_sha256` в identity; прогон difftest по всем расписаниям | Step 0 / G-09 |
| TA-13 | тест (h) nested HLE→guest в G-14, покрытие ≥2 вложенных уровней | Step 5b / G-14 |
| TA-14 | bounded-join тест с потоком в INFINITE-wait; зависание → `fail` | Step 5 / G-04 |
| TA-15 | `--concurrency` помечает loader-lock путь как запрещённый | Step 5 / G-01 |
| TA-16 | `summary.sha256` в отчёте сборки; отсутствие → L2 = `missing` | Step 5b / G-01 |
| TA-17 | поколение staging в трассе T2; повтор поколения → `fail` | Step 7 / G-04 |
| TA-18 | identity прогона содержит cpuset/thermal; смена → `unavailable` | Step 0 / G-10 |

Как и PM-17…PM-24, каждый новый id имеет минимальный исполнимый чек:
он фиксирует статус честно, но по ADR-029 **не блокирует** разработку,
а запрещает выдавать непроверенное поведение за доказанное. TA-12…TA-18 =
`specified` / `missing`; перевод в `enforced` — по V8 с evidence.
Матрица V2 не меняется.
