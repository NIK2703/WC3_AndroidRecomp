# As-built снимок WC3 1.27b

Статус: **снимок, не норма** (2026-11-06). Вынесено из
`docs/ARCHITECTURE_1.27B.md` §0 и §2.1 при M2. Все числа обязаны
воспроизводиться инструментами (`tools/perf_probe.py`,
`tools/concurrency_audit.py`, `tools/hle_coverage.py`) и храниться в
`reports/generated/`; расхождение фиксируется как инцидент F2
(`docs/BACKLOG.md`). Нормативные владельцы перечислены в `docs/README.md`.

**Уточнение ADR-026:** запись ниже о «Windows reference host = oracle» —
история rev 2. В целевом дереве рекомпилированный Windows-хост **не
собирается**: эталон — оригинальная игра и её захваты, а `wc3_host27*.exe`
остаётся историческим готовым артефактом старого дерева.

## История rev 2 (бывший §0)

| # | Было в rev 1 | Стало | Причина |
|---|---|---|---|
| 1 | Модель потоков отсутствует (677 строк, 0 упоминаний «поток»), `ThreadApi`/`SyncApi` запрещены | Явная модель guest threads, `ThreadApi`/`SyncApi` обязательны | В игре 18 `CreateThread`, 36 thread/sync-импортов из 426; `guest_layout.h` содержит 24 потоковых слота |
| 2 | «Никаких mutex/lock/critical section/atomic-lock» | Запрещены только локи в guest-пути и локи, удерживаемые через guest-код; локи HLE-объектов разрешены | Точная семантика `EnterCriticalSection` на реальных потоках требует блокировки; запрет делал архитектуру нереализуемой |
| 3 | `stack_cleanup = 4+argc*4` / `0` для cdecl, «снимает argc\*4» | Единая формула `4 + (cdecl ? 0 : argc*4)` | Расхождение на 4 байта = сдвиг кадра; так уже сделано в `hle_abi.c:63` |
| 4 | `HleEntry` без user-поля | Поле `user` обязательно | `e->fwd` в as-built восстанавливает (интерфейс, метод) для 324 COM-слотов |
| 5 | Layout = конфиг с `stack/heap/hle_heap` | Layout = валидатор корпуса; образы `corpus_fixed`, плюс thread stacks, 7 VA-арен, HLE-окно | Базы вшиты в 191 650 лифтнутые функции; ART резервирует `0x10980000–0x30980000` |
| 6 | Windows-хост и difftest вне области, exactness device-only | Windows reference host = oracle, не продукт; трёхуровневая верификация T0/T1/T2 | Хост сегодня является контролем при локализации бага рендера; устройства в контуре нет |
| 7 | Turnip/adrenotools не упомянуты, `--policy` их запрещает | Явный `VulkanDriverApi` (`system` \| `turnip`) | Через adrenotools сегодня работает DXVK на Adreno 725 |
| 8 | Селектор по `vkEnumerateInstanceVersion`, без fallback | Селектор по physical device + **fallback при ошибке создания device** | Версия loader'а ≠ возможности устройства |

`tools/strip_thread_lines.py` (удаляет строки со словом «поток») **не
применяется к этому документу** и подлежит удалению: разногласие в модели
исполнения решается проектированием, а не фильтрацией текста.

## As-built (бывший §2.1)

| Факт | Значение | Источник |
|---|---|---|
| Android-порт | boot → меню → 3D-сцена на устройстве | `PROGRESS.md`, 2026-11-04 |
| Устройство | `23049PCD8G`, Adreno 725, Vulkan 1.4 (Turnip) | `docs/ANDROID.md:1937` |
| Импорты | `war3.exe` 498/498, `game.dll` 828/828, 0 unresolved | `docs/ANDROID.md`, Round 1 |
| Диспетчер | **191 650 уникальных** `g_lift[]` записей, hash 2^19 (2 МиБ, load 0,366), tail-jump | `build27/wc3_dispatch_all.c` (измерено 2026-11-06) |
| HLE Android | `hle_android.c`, 10 333 строки, 426 записей | `runtime/android/` |
| HLE Windows | `hle.c`, 5 874 строки (oracle) | `runtime/host/` |
| Guest threads | 18 `CreateThread`, `gid=4096`, `_beginthreadex gid=4100` | `PROGRESS.md`, `docs/ANDROID.md` |
| Сборка | `libwc3game-thin.so` 216 МБ, release APK 65,6 МБ | `build/android/boot_probe/` |
| DXVK | 1.10.3 и 3.1.1 статически в одном `.so`, symbol isolation через `objcopy` | `build_boot_probe.ps1:584`, `build_dxvk_probe.ps1:150` |
| Исходный PE | `E:\Games\Warcraft III\game.dll`, 14 837 224 байта, 2016 | локальная лицензионная копия |
| Графический дефект | найден и исправлен в старом дереве (`guest_ok()`/`cr_memcpy`); в target сохраняется регрессионным контролем V5.9 | `PROGRESS.md` |

**Источник устаревшей цифры.** `docs/ANDROID.md` (Round 1) сообщает 63 292
адреса; это ранний билд. Текущий `wc3_dispatch_all.c` содержит 191 650
уникальных записей (проверено перебором массива). Rev 2/2.1 цитировали
63 292 в пяти местах — исправлено; это ровно инцидент F2, который документ
сам же описывает: as-built числа обязаны проверяться по артефакту.

Следствие: проект **не находится в Phase 0**. Это рабочий порт; найденный
графический дефект исправлен в старом дереве и в целевом дереве не
расследуется. Любая архитектура, предполагающая написание HLE/runtime с нуля,
ошибочна.

## Целевое дерево: bootstrap-снимок (2026-11-06)

Это отдельный, качественный снимок `wc3_android_recomp`; он не продолжает
таблицу старого дерева выше и не содержит её чисел. Источник фактов —
`build/android_app/`, `tools/android/run_platform_test.ps1` и
`tools/android/run_app_test.ps1` на устройстве `23049PCD8G`.

| Факт | Состояние |
|---|---|
| Продукт | `libwc3gameactivity.so` собирается только для Android arm64-v8a; Windows-сборки нет |
| App shell | GameActivity + Java diagnostic overlay; surface lifecycle, PE loading, JNI diagnostics |
| Platform core | memory, file, time, thread/TLS, sync, surface, audio, config, asset, log |
| Sync | Win32 events/mutex/semaphore/CS; abandoned mutex detected through per-thread ownership; owner death leaves CS permanently locked |
| Audio | AAudio callback sink, SPSC acquire/release ring, supervisor reopen with backoff, explicit silent sink after repeated failure; open/write/callback/close device-tested, loss path not fault-injected |
| PE32 | real `war3.exe` at `00400000` and `game.dll` at `6F000000` mapped; headers/sections/imports/exports/resources inspected; imports not bound |
| Assets | four MPQ names checked case-insensitively at startup; incomplete set shown in the overlay, not silently ignored |
| Config | optional app-private `wc3.cfg`, one read before guest startup; missing file is empty config |
| Corpus / HLE / dispatcher | absent; overlay reports `corpus: missing`, `HLE: missing`, `dispatcher: missing`, `game: not started` |
| Release smoke | release APK launches and shows the same diagnostic; manual scan found no `wc3host`/`wc3pe`/`wc3platform`/`wc3audio` strings |

`wc3.cfg` format (implementation contract, not a game asset): UTF-8/ASCII
`key=value` lines; `#` starts a comment when it is the first non-space
character; ASCII space/tab/CR/LF around key and value is trimmed; keys are
`[A-Za-z0-9_.-]`, case-sensitive, at most 63 bytes; values at most 255 bytes;
last duplicate wins; at most 64 entries. Unknown keys are visible through
`ConfigApi` but do not change startup. A malformed line is skipped, not treated
as a game-data fallback.

**Не закрыто этим снимком:** G-05, G-08, G-09, G-12, G-13, G-15; нет
`wc3_headless`, `tools/difftest.py`, `wc3build.py`, `check_manifest.py`,
`platform/headless`, `gfx/null`; HLE не переносится до закрытия G-08/G-09.
