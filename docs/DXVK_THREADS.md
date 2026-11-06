# DXVK internal threads and dual-backend isolation — WC3 1.27b

Статус: **source/build-graph confirmed, device counts pending** (rev 2,
2026-11-06). Связанная архитектура: `docs/ARCHITECTURE_1.27B.md` §7,
ADR-004/ADR-007/ADR-016. **Решение о выборе бэкенда** (ADR-004/007 переведены
в `provisional`), матрица A/B/C/D, контракт fallback и обязательные измерения —
норма `docs/GRAPHICS_DECISION.md` (GB-1…GB-10, ADR-023). Пункты §5 ниже
являются evidence для GB-5.4/GB-5.7.

Цель документа — зафиксировать границу владения: DXVK и его Win32 shim —
**vendor-owned код**. Их потоки и локи существуют, разрешены и не входят в
guest state. Правило «нет локов» (ARCHITECTURE §3.3) относится к guest-пути,
а не к DXVK.

## 1. As-built: как оба DXVK живут в одном .so

Это не предложение — конвейер уже работает:

| Шаг | Факт | Источник |
|---|---|---|
| Сборка | `libdxvk1.a` (DXVK 1.10.3) и `libdxvk3.a` (DXVK 3.1.1) | `tools/android/build_dxvk_probe.ps1` |
| Symbol isolation | изолятор распаковывает **каждый member архива** и запускает `llvm-objcopy --prefix-symbols` по одному; `NM`/`OBJCOPY` из NDK | `build_dxvk_probe.ps1:150-163` |
| Линковка | `--whole-archive libdxvk1.a libdxvk3.a --no-whole-archive`, `-Wl,--exclude-libs,ALL`, статический `libc++` | `tools/android/build_boot_probe.ps1:584-595` |
| Win32 shim | `dxvk_win32.cpp` (~57 КБ) архивируется **внутри `libdxvk1.a`** и не компилируется повторно | `build_boot_probe.ps1:424-431` |
| Выбор бэкенда | сегодня `WC3_DXVK_VERSION=1|3`; thin bridge владеет `wc3_dxvk_prepare` и version probe | `dxvk_entry.cpp`, `build_boot_probe.ps1:463` |
| Vulkan ICD | `WC3_VULKAN_DRIVER=turnip|system`, adrenotools (`libhook_impl.so`, `libmain_hook.so`) | `docs/ANDROID.md:1897-1947` |
| Устройство | Adreno 725, Turnip сообщает Vulkan 1.4; оба DXVK создают device | `docs/ANDROID.md:1937-1947` |
| Размер | `libwc3game-thin.so` 216 МБ, release APK 65,6 МБ | `build/android/boot_probe/` |

Следствия, которые rev 1 не проговаривал:

* префикс символов устраняет duplicate symbols, общие глобалы и RTTI-конфликты,
  но **статические конструкторы обоих DXVK выполняются при загрузке `.so`**.
  Требование «второй DXVK не инициализируется» означает «не создаёт Vulkan
  instance/device», а не «не имеет глобального состояния»;
* `dxvk_win32` — это не «только символы», а мини-Win32: потоки, sync, TLS,
  файлы. Он использует собственные host-локи, что разрешено allowlist'ом
  `--concurrency`.

## 2. Инвентарь потоков

Точное число потоков, lifetimes, владельцы очередей и shutdown не подтверждены
по исходникам/устройству — остаётся `unavailable`, не `pass`.

| DXVK | Поток | Роль | Guest/HLE access | Evidence | Статус |
|---|---|---|---|---|---|
| 1.10.3 | state cache compiler | компиляция pipeline state | нет, только staging | известная архитектура DXVK 1.10.x; точное число не снято | unavailable |
| 1.10.3 | state cache writer | запись state cache | нет, только staging | там же | unavailable |
| 3.1.1 | pipeline worker pool | компиляция pipeline | нет, только staging | известная архитектура DXVK 3.x; размер пула не снят | unavailable |
| обе | WSI / present / submit | отдача кадра в `ANativeWindow` | нет, через `SurfaceApi` | `dxvk_win32.cpp`, thin bridge | unavailable |
| обе | Vulkan loader / ICD | driver-side потоки | нет | `VulkanDriverApi`: system или Turnip | unavailable |
| `dxvk_win32` | Win32 thread/sync shim | эмуляция CreateThread/CS/TLS для DXVK | нет, изолирован от guest memory | `dxvk_win32.cpp` | unavailable |

## 3. Инварианты владения

1. Ни один DXVK-поток не входит в `CPU` гостя, HLE registry, handle table или
   guest memory без документированного staging-контракта (`LockRect` §7.5).
2. `dxvk_win32` не получает guest memory напрямую; `LockRect` выдаёт гостю
   указатель на staging, `Unlock` копирует в DXVK.
3. Второй (невыбранный) DXVK не создаёт Vulkan instance/device; его глобальное
   состояние от статических конструкторов допустимо и должно быть проверено на
   отсутствие побочных эффектов (создание потоков, файлов, сокетов).
4. Host-локи DXVK и shim'а не удерживаются при `dispatch()` и не разделяются с
   guest-shared объектами.
5. Android host threads (UI/input/overlay/canvas/AAudio) не вызывают DXVK из
   своего контекста; present инициируется owning guest thread.

## 4. Селектор, ICD и fallback (ADR-007 rev 2)

`vkEnumerateInstanceVersion` возвращает версию **loader'а**, поэтому не может
быть единственным критерием. Порядок:

1. `VulkanDriverApi` выбирает ICD (`system` | `turnip`) явно; запрет rev 1 на
   «runtime-подмену провайдера» относился к внешним `d3d9.dll`, а не к ICD.
2. `vkEnumeratePhysicalDevices` → `apiVersion` + требуемые extensions/features.
3. ≥ Vulkan 1.3 и достаточный набор → DXVK 3.1.1; иначе → DXVK 1.10.3.
4. При ошибке создания device или потере на первом present → **однократный
   fallback на другой бэкенд**, с `selector`/`reason`/`apiVersion` в debug.
5. `WC3_DXVK_VERSION` остаётся debug-override для A/B, в release не читается.

`--policy` запрещает внешние D3D-DLL/PATH и legacy frontends, но **не** bans
adrenotools/Turnip: на системном драйвере Adreno порт может не получить нужных
расширений.

## 5. Gate

1. По исходникам каждой версии: `std::thread`, `pthread_create`, worker pool,
   state cache, WSI, Vulkan loader. Записать count, owner, lifetime, queue,
   staging pointers, shutdown.
2. Проверить, что статические конструкторы невыбранного DXVK не создают
   потоки/устройства.
3. Проверить, что ни один DXVK-поток не читает `CPU`/HLE/handle table.
4. Device matrix (T2):
   * Vulkan < 1.3 → 1.10.3;
   * Vulkan ≥ 1.3 → 3.1.1;
   * 3.1.1 не смог создать device → fallback на 1.10.3;
   * только один бэкенд создаёт instance/device; нет duplicate symbol/init crash;
   * оба пути: boot → menu → map, frame hash.
5. `--concurrency` применяет allowlist к `third_party/dxvk*`,
   `runtime/platform/dxvk_win32`; `--release` не требует отсутствия debug-строк
   внутри vendor-кода.
6. **TA-7**: снять 5 строк `unavailable` по §2 (count, owner, lifetime,
   queue, staging pointers, shutdown), проверить статические конструкторы
   невыбранного бэкенда (GB-4.5) и отсутствие чтения `CPU`/HLE/handle table
   vendor-потоками (GB-4.6). T1 на headless-DUT не является evidence о
   vendor-потоках (GB-7).

Без пункта 4 статус — `unavailable`, не `pass`. По GB-5.1 размер `.so` также
обязан быть воспроизведён: сейчас ARCHITECTURE указывает 216 МБ, а ANDROID.md
старого дерева ~253 МБ; это расхождение блокирует сравнение вариантов A/B/C.
