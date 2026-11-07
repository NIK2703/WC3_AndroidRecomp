# Анализ производительности Warcraft III 1.27.1.7085 → Android arm64

Дата: 2026-11-07  
Статус: аналитический отчёт; код проекта не изменялся.  
Область: `E:\projects\wc3_android_recomp`, продукт `libwc3gameactivity.so` (Android arm64-v8a).

## 1. Краткий вывод

Главные **новые** возможности, подтверждённые кодом и дизассемблером:

1. **TLS в текущей сборке — это emutls, а не нативный TLS.** Каждое обращение к `THREAD_LOCAL` превращается в вызов `__emutls_get_address`, который внутри вызывает `pthread_getspecific`. В `dispatch_common` таких вызовов **три на каждый dispatch**. В `x87_begin`/`x87_end` и внутри SoftFloat — ещё несколько на каждую x87-операцию. Это, вероятно, самый дешёвый по усилиям и самый широкий по охвату runtime-кандидат.
2. **Relift-корпус собирается с `-O0`.** Это не текущий legacy-артефакт, но это целевой путь после cutover: `cmake/Wc3ReliftedCorpus.cmake:99-106` добавляет `-O0` после `-O3`, и `build/android_relifted_clean/build.ninja` это подтверждает. При переходе на relift без изменения все ~181 тыс. входов диспетчера останутся неоптимизированными.
3. **HLE-строки проверяют guest-память побайтово.** `lstrlenA`, `lstrcpynA`, `strcmp` и родственные функции вызывают `guest_ok()` на каждый символ, а `guest_ok()` сканирует 20 статических арен и до 96 динамических диапазонов. Это усиливает уже известный P1-19 и требует не только bitmap, но и пакетной проверки диапазона.
4. **Таблица `g_lift[]` — 16 байт на запись и почти 206 тыс. RELATIVE-релокаций.** 181 003 записи дают ~2,9 МБ в `.data.rel.ro`; 206 040 из 206 943 релокаций приходятся на этот диапазон. Замена указателя на 32-битное смещение относительно одной функции уменьшает таблицу примерно вдвое и убирает ~181 тыс. релокаций без изменения guest ABI.
5. **Артефакт перегружен динамическими символами и unwind-таблицами.** В release `.so`: 220 948 записей `.dynsym`, из них **196 115** — `L_*`; `.dynstr` 4,8 МБ, `.gnu.hash` 1,6 МБ, `.eh_frame` 10,0 МБ. Это влияет на размер, загрузку и память, но не на гостевую семантику.

Уже известные P0-1…P0-10 остаются главными по потенциальному выигрышу; ниже отдельно указано, что из них подтверждается текущим кодом, а что является новым слоем.

---

## 2. Что именно анализировалось

- `docs/PERFORMANCE.md` и `docs/PERFORMANCE_CONTRACT.md` — нормативный каталог и правила измерений.
- `runtime/legacy/hle_android.c`, `runtime/platform/android/platform_android.c`, `runtime/boot/dispatch_boot.c`, `runtime/cpu/cpu.h`, `runtime/cpu/x87.h`.
- HLE-модули `runtime/hle/win32/...` и `runtime/hle/msvcr80/...`.
- Android-хост: `apps/android_gameactivity/src/main/cpp/host_shell.c` и `runtime/platform/android/controls/*`.
- Сборочные файлы `CMakeLists.txt`, `cmake/Wc3LegacyBoot.cmake`, `cmake/Wc3ReliftedCorpus.cmake`, `tools/android/build_app.ps1`.
- Артефакты:
  - текущий legacy release: `build/android_app/release/cmake/libwc3gameactivity.so` — 223 705 400 байт;
  - relift release: `build/android_relifted_clean/libwc3gameactivity.so` — 254 513 040 байт.

Текущий `android_app` собран из **legacy prebuilt objects**: в его `CMakeCache.txt` `WC3_RELIFTED_CORPUS_ROOT` пуст. Поэтому вывод про `-O0` **не относится к текущему .so**; он относится к relift-пути. Это важно не смешивать.

Графический путь в целевом дереве отдельно не анализировался: `runtime/gfx/` содержит только `null/`, а D3D9/DXVK-объекты приходят из pinned legacy/vendor-сборки. Внутренности DXVK — вне границ этого отчёта; P2-10 остаётся отдельной задачей.

---

## 3. Где проходит горячий путь

1. **Guest-код → dispatcher.** Сгенерированные `L_*` вызывают `dispatch(c, VA)` / `dispatch_jmp(c, VA)`. `dispatch_common` ищет VA в `g_lift_hash` (2 МиБ, 2^19), затем берёт `g_lift[e-1].fn` и делает `blr`.
2. **Guest → HLE.** Для окна `0x7F000000` вызывается `wc3_legacy_hle_call` → `hle_call`; на каждом HLE-вызове сейчас безусловно обновляются `e->calls`, `g_last_cpu`, `g_ring[]`, `g_ring_pos/len` и читается `rd32g(c->esp)`.
3. **HLE → heap/sync/threads.** `hle_guest_alloc/free` идут через глобальный `g_heap_lock`; `cs_get` — глобальный мьютекс и линейный поиск; x87/SoftFloat — через TLS-состояние.
4. **Android host.** Отдельный overlay-поток строит Canvas-атлас, затем на каждый visual change делает software-blit в `ANativeWindow`; AAudio callback читает SPSC-кольцо; input идёт через `input_ring`.

Именно в этих четырёх местах и находятся подтверждённые кандидаты.

---

## 4. Что уже есть в нормативном каталоге

Это не новые находки, но их полезно отделить от отчёта:

| Пункт | Что подтверждается кодом | Статус |
|---|---|---|
| **P0-1** | 42 713 из 42 720 константных `dispatch`/`dispatch_jmp` в одном 71-МБ файле имеют цель в `g_lift[]`; наивная замена на прямой вызов ломает `g_disp_depth`/`g_tail_pending` | `specified`, не реализовано; нужен G-14/G-15 |
| **P0-2 / P0-10** | 98 964 flag-defining инструкций; 44 034 гарантированно мёртвых записей флагов; `parity8()` и `af` почти не читаются | измерено, не реализовано |
| **P0-3** | 28 591 `GVA()` читает изменяемый `g_image_delta`, который на Android всегда 0 | измерено, не реализовано |
| **P0-5 / P0-6** | 1 093 косвенных вызова через IAT; 24 261 пара `push32(ret) + dispatch` | измерено, не реализовано |
| **P0-7** | В `hle_call` (текущие строки `hle_android.c:9675-9683`) пять общих глобалов пишутся на каждом вызове, без `#if WC3_DEBUG` | подтверждается; это активный путь |
| **P0-9** | Legacy `guest_thread_proc` вызывает `cpu_x87_init()` (`hle_android.c:5937`), а target `android_thread_create` — нет (`platform_android.c:396-400`) | latent-разрыв при миграции на `ThreadApi` |
| **P1-18** | `x87_begin` пишет три TLS-глобала, `x87_end` читает/пишет `softfloat_exceptionFlags` (`x87.h:154-161`) | подтверждается; новый слой — emutls, см. N-01 |
| **P1-19** | `guest_ok()` — 20 статических арен + fallback `guest_range_ok()` | подтверждается; новый слой — побайтовые строки, см. N-03 |
| **P1-20** | `cs_get` — глобальный мьютекс + линейный скан до 512 записей; бенчмарк 182–190 нс против 9,4 нс у atomic fast path | подтверждается |
| **P1-8** | `.text` 160,9 млн байт; LTO/PGO/`--gc-sections`/hot-cold не включены | подтверждается; но `--gc-sections` ограничен `g_lift`, см. N-06 |

---

## 5. Новые находки

### N-01. Emulated TLS: `__emutls_get_address` на каждом dispatch и x87-шаге

**Приоритет: P0, текущий продукт.**  
**Сложность: S.**  
**Риск: средний (только загрузка .so, не guest-семантика).**

`dispatch_boot.c:135-137` объявляет:

```c
static THREAD_LOCAL int g_disp_depth;
static THREAD_LOCAL int g_tail_pending;
static THREAD_LOCAL uint32_t g_tail_target;
```

`THREAD_LOCAL` — это `_Thread_local` (`cpu.h:34-35`, `x87.h:32-33`). В текущем .so это **emutls**. В дизассемблере `dispatch_common` видны три вызова `__emutls_get_address` подряд — для `g_disp_depth`, `g_tail_pending` и `g_tail_target`:

```text
ae83288: bl __emutls_get_address   ; g_disp_depth
ae832a4: bl __emutls_get_address   ; g_tail_pending
ae832c4: bl __emutls_get_address   ; g_tail_target
```

Сама `__emutls_get_address` — это отдельная функция размером ~0x1EC байт, которая вызывает `pthread_getspecific`, проверяет таблицу и при необходимости делает `malloc/realloc/memset`. То есть каждое TLS-обращение — не «загрузка из TLS-регистра», а вызов в libc.

То же касается x87/SoftFloat:

- `x87_begin(c)` делает три TLS-обращения (`softfloat_roundingMode`, `extF80_roundingPrecision`, `softfloat_exceptionFlags`);
- `x87_end(c)` читает и записывает `softfloat_exceptionFlags`;
- внутри `extF80_*`/`f32_*`/`f64_*` SoftFloat также обращается к TLS-глобалам.

В sample-функции `L_00407460` перед `extF80_lt`/`extF80_eq` видны три вызова `__emutls_get_address`. Итого на одну x87-операцию может приходиться 3–5 таких вызовов, а не «3 дешёвые TLS-записи», как можно прочитать в P1-18.

**Что делать:**

1. Собрать host C/C++ с:
   ```text
   -fno-emulated-tls -ftls-model=initial-exec
   ```
   Проверка NDK clang показала, что при этих флагах доступ превращается в:
   ```text
   adrp x8, :gottprel:x
   ldr  x8, [x8, :gottprel_lo12:x]
   mrs  x9, TPIDR_EL0
   ldr  w0, [x9, x8]
   ```
   без вызова `__emutls_get_address`.
2. Обязательно проверить загрузку .so на устройстве: `initial-exec` требует статического TLS-блока. Приложение одно большое, `minSdk = 26`; скорее всего, блок поместится, но это нужно подтвердить smoke-тестом. Если linker откажется — использовать `-fno-emulated-tls` с TLSDESC или перенести состояние.
3. Более надёжный, но более инвазивный вариант: перенести `g_disp_depth`/`g_tail_pending`/`g_tail_target` в конец `CPU` (добавление полей в конец разрешено ABI-2) и брать их из `c->...`. Для SoftFloat — per-thread структура, передаваемая через `CPU` или один указатель, полученный один раз на границе.
4. Не менять `CPU`/сигнатуры dispatcher без перелифта; добавление полей в конец — допустимый путь, но его всё равно нужно проверить G-14/G-15.

**Ожидаемый эффект:** исчезают три libc-вызова на каждый dispatch и несколько вызовов на каждую x87/SoftFloat-операцию. Это не заменяет P0-1, но снимает накладные расходы, которые останутся даже после девиртуализации — на истинно косвенных вызовах и на x87.

**Как измерять:** `simpleperf` на T2: доля `__emutls_get_address`/`pthread_getspecific` в профиле; счётчики `dispatch/frame`, `x87 ops/frame`; время загрузки .so; A/B по PM-19.

---

### N-02. Relift-корпус жёстко собирается с `-O0`

**Приоритет: P0 для будущего relift-продукта; на текущий legacy .so не влияет.**  
**Сложность: M/L.**  
**Риск: высокий для exactness — только через G-15/PM-23.**

`cmake/Wc3ReliftedCorpus.cmake:99-106`:

```cmake
target_compile_options(wc3_corpus_relifted PRIVATE
    -O0
    -fno-strict-aliasing
    -fwrapv
    -ffp-contract=off
    ...)
```

CMake подставляет `CMAKE_C_FLAGS_RELEASE` (`-O3`) раньше, но target-опция `-O0` идёт после и выигрывает. В `build/android_relifted_clean/build.ninja` это видно буквально:

```text
... -O3 -DNDEBUG ... -O0 -fno-strict-aliasing -fwrapv ...
```

Текущий `build/android_app/release` использует legacy prebuilt objects, поэтому его `.text` — 160,9 млн байт, и к нему это замечание не относится. Но `build/android_relifted_clean` — это relift-путь, и его .so уже 254,5 МБ. Если переключить продукт на relift, не изменив этот флаг, весь корпус будет исполняться без оптимизации.

**Что делать:** не «просто убрать `-O0`». Добавить явную опцию, например `WC3_CORPUS_OPT`, со значениями `-O0` для debug/непроверенного корпуса и `-O2` для release; прогнать golden-набор G-15 на обоих уровнях (`PM-23`), сравнить нормализованные тела функций и поведение. Только после этого включать `-O2` в релиз.

**Ожидаемый эффект:** потенциально самый большой выигрыш для будущего продукта; без проверки exactness риск неприемлем.

---

### N-03. HLE-строки: `guest_ok()` на каждый символ

**Приоритет: P1, текущий продукт.**  
**Сложность: M.**  
**Риск: низкий/средний (границы валидности guest-памяти).**

`runtime/hle/win32/kernel32/string/string_hle.c:28-37`:

```c
while (p && wc3_legacy_guest_ok(p, 1) && wc3_legacy_rd8g(p)) {
    n++;
    p++;
}
```

`h_lstrcpyn_a` (`:45-57`) проверяет `src + i` на каждый символ; `h_strcmp`/`h_stricmp` в `runtime/hle/msvcr80/string/string_hle.c:446+` делают то же через `byte_ok()`.

`wc3_legacy_guest_ok` → `hle_android_guest_ok` → `guest_ok` (`hle_android.c:438-457`) — это скан **20 статических арен** и fallback `guest_range_ok()` ещё на **до 96 диапазонов**. Для строки в 100 символов это ~2 000 проверок; для пути в 260 символов — больше 5 000; для `strcmp` на двух строках — умножение на 2. Для UI, загрузки карт и разбора путей это может быть заметно.

**Что делать:**

1. Сначала P1-19 (page bitmap) — убирает O(20) на одну проверку.
2. Затем добавить `wc3_legacy_guest_span(addr, &len)` — вернуть непрерывный валидный участок до конца арены/страницы. Внутри него использовать `memchr`/`strnlen`/`memcpy`, а на границе валидного участка останавливаться и проверять следующий. Это сохраняет текущую семантику «остановиться на первом невалидном байте», но убирает вызов на каждый символ.
3. Для `lstrcpynA` проверять `dst` один раз (уже сделано), а `src` читать пакетно; для `strcmp` — пакетно по обоим указателям.
4. Добавить unit-тесты на строку, заканчивающуюся ровно на границе арены, на невалидный указатель и на строку без NUL.

**Ожидаемый эффект:** средний, сильно зависит от доли HLE-строк в профиле; на map load/UI может быть существенным. Это не заменяет P1-19, а снимает его «побайтовый усилитель».

---

### N-04. `g_lift[]`: 16 байт на запись и ~181 тыс. RELATIVE-релокаций

**Приоритет: P1, текущий продукт.**  
**Сложность: M.**  
**Риск: низкий (внутренний dispatcher, не guest ABI).**

`runtime/boot/dispatch_boot.c:56-65`:

```c
typedef struct {
    uint32_t va;
    void (*fn)(CPU *);
} LiftEnt;
```

На arm64 это 16 байт: 4 байта `va`, 4 выравнивания, 8 байт указателя. В таблице 181 003 записи из `boot_dispatch_list.h`, то есть ~2,9 МБ. В release `.data.rel.ro` — 3 208 312 байт, и **206 040 из 206 943** `R_AARCH64_RELATIVE` указывают в этот диапазон. Это не только размер: загрузчик применяет ~206 тыс. релокаций, а `find_lifted` делает зависимую загрузку `g_lift[e-1].fn`.

**Что делать:** генерировать `LiftEnt` как `{ uint32_t va; uint32_t off; }`, где `off` — смещение функции от одной якорной `L_*` (например, `L_00401018`). В `find_lifted` восстанавливать указатель:

```c
uintptr_t addr = (uintptr_t)&L_00401018 + ent->off;
void (*fn)(CPU *);
memcpy(&fn, &addr, sizeof fn);
return fn;
```

Смещение в пределах `.text` (160,9 млн байт) гарантированно влезает в 32 бита; добавить static assert и тест, что все 181 003 записи находятся и указывают на ожидаемые функции. Это уменьшает таблицу примерно до 1,4 МиБ и убирает ~181 тыс. релокаций. Это **не** то же самое, что скрытие символов: RELATIVE-релокации останутся, даже если `L_*` перестанут быть экспортированными.

**Ожидаемый эффект:** меньше памяти, меньше работы загрузчика, лучше локальность `find_lifted`; полезно как fallback после P0-1.

---

### N-05. 196 тыс. экспортированных `L_*` и перегруженный `.dynsym`

**Приоритет: P1, текущий продукт.**  
**Сложность: S/M.**  
**Риск: средний (можно случайно скрыть JNI/GameActivity).**

Измерения release `.so`:

- `.dynsym` — 5 302 776 байт, **220 948** записей;
- из них **196 115** — `L_*`;
- `.dynstr` — 4 813 364 байта;
- `.gnu.hash` — 1 627 524 байта;
- `FLAGS: SYMBOLIC BIND_NOW`.

Все `L_*` экспортированы как `GLOBAL DEFAULT`, хотя нужны только внутри .so. Это раздувает таблицу символов, хэш и строки, а загрузчику добавляет работы.

**Что делать:** version script, который оставляет только реально нужные экспорты:

```text
{
  global:
    Java_*;
    JNI_OnLoad;
    GameActivity*;
    android_main;
  local: *;
};
```

Не использовать слепо `--exclude-libs,ALL`: он может скрыть функции GameActivity, которые вызывает Java. После линковки проверить `System.loadLibrary`, все `native`-методы, `JNI_OnLoad` и запуск `android_main`.

**Ожидаемый эффект:** до ~10 МБ метаданных и заметно меньше динамических символов; на скорость гостя не влияет, но уменьшает размер и время загрузки. RELATIVE-релокации это **не** убирает — для них N-04.

---

### N-06. `.eh_frame` 10 МБ и `.eh_frame_hdr` 2 МБ для сгенерированного C

**Приоритет: P1/P2, текущий продукт и relift.**  
**Сложность: S.**  
**Риск: низкий, если не трогать HLE с backtrace.**

В release:

- `.eh_frame` — 10 026 144 байт;
- `.eh_frame_hdr` — 2 000 572 байта;
- `.gcc_except_table` — 176 440 байт (это уже C++ DXVK).

NDK по умолчанию добавляет `-funwind-tables`; для сгенерированного C исключений нет, но unwind-таблицы создаются для каждой функции. Для корпуса можно собрать с `-fno-unwind-tables` (или `-fno-asynchronous-unwind-tables`) и получить минус ~12 МБ mapped-памяти и работы загрузчика.

**Ограничение:** не применять это к `runtime/legacy/hle_android.c`: там есть `_Unwind_Backtrace` (`hle_android.c:9877-9901`) для диагностики. Также не трогать C++ DXVK. Флаг — только на generated corpus/relift target.

**Ожидаемый эффект:** размер/загрузка, не frame time. Низкий риск, если сохранить backtrace в HLE.

---

### N-07. `--pack-dyn-relocs=android` для 206 тыс. RELATIVE-релокаций

**Приоритет: P2, текущий продукт.**  
**Сложность: S.**  
**Риск: низкий, но требует проверки загрузчика.**

`.rela.dyn` — 4 966 872 байта, `RELACOUNT` — 206 943. `minSdk = 26`, поэтому Android packed relocations (`DT_ANDROID_REL`) поддерживаются. Флаг `-Wl,--pack-dyn-relocs=android` сжимает таблицу релокаций; после N-04 часть из них исчезнет, но оставшиеся всё равно сожмутся.

Проверить: загрузка на API 26/27 и на современном устройстве, отсутствие ошибок linker'а, `readelf -d` показывает `ANDROID_REL`/`ANDROID_RELA`. Это не ускоряет сам гость, но сокращает `.so` и время загрузки.

---

### N-08. `HeapReAlloc` всегда alloc/copy/free; `HeapFree` дважды валидирует блок

**Приоритет: P2, текущий продукт.**  
**Сложность: M.**  
**Риск: средний (точная семантика Windows heap).**

`runtime/hle/msvcr80/heap/heap_hle.c:31-55`:

- `h_realloc` всегда делает `guest_size(old)`, `guest_alloc(size)`, `guest_copy`, `guest_free(old)`.
- Уменьшение размера тоже копирует весь блок.
- `HEAP_ZERO_MEMORY` в `h_heap_realloc` (`memory_hle.c:113-120`) сначала обнуляет весь новый блок, затем копирует старые байты.

`h_heap_free` (`memory_hle.c:76-89`) сначала вызывает `guest_size(p)`, затем `guest_free(p)`, а `hle_guest_free` снова проверяет заголовок и диапазон. То есть на каждый `HeapFree` — две проверки `guest_ok` и один линейный поиск free-list.

**Что делать:**

1. Для `realloc`/`HeapReAlloc`: если `size <= old_size` — уменьшить блок на месте (обновить заголовок и free-list), без копирования; если `size > old_size` — попробовать расширить за счёт следующего свободного блока, иначе fallback на alloc/copy/free.
2. Добавить `wc3_legacy_guest_free_checked()` с одним захватом heap-lock, который возвращает `bool`; `HeapFree` использует его вместо `guest_size + guest_free`.
3. `HEAP_ZERO_MEMORY` обнулять только новый хвост, а не весь блок.

**Ожидаемый эффект:** меньше копирований и пиковых аллокаций; особенно на загрузке карт и работе с большими буферами. Частоту нужно измерить: если `HeapReAlloc` редок, выигрыш будет небольшим.

---

### N-09. Overlay: линейный `cache_find_layer` и полная очистка кадра

**Приоритет: P2, overlay-поток (не guest).**  
**Сложность: S/M.**  
**Риск: низкий для индекса; средний для dirty-rect.**

`runtime/platform/android/controls/overlay.c:102-110`:

```c
for (int i = 0; i < cache->layer_count; i++) {
    if (l->element == element && l->slot == slot)
        return l;
}
```

`draw_button_cached` вызывает это 4 раза на элемент, `draw_dpad_cached` — 3. Максимум: 64 элемента × 4 × 384 слоя ≈ **98 тыс. сравнений на present**. Это не guest-поток, но при touch-driven перерисовке может быть заметно.

**Что делать:** при публикации атласа построить прямой индекс `int16_t layer_index[64][6]`; `cache_find_layer` становится одним индексированием. Это локально, без изменения пикселей.

Второй кандидат — `overlay_render` (`:477-499`) полностью очищает RGBA-буфер:

```c
for (int y = 0; y < height; y++)
    memset(rgba + y * stride, 0, width * 4);
```

При 2560×1440 это ~14,7 МБ на кадр; при 60 Гц — ~0,88 ГБ/с только на очистку. Но `ANativeWindow` не гарантирует сохранение старого буфера, поэтому dirty-rect требует отдельного доказательства/эксперимента. Начинать с безопасного индекса слоёв.

**Важно:** атлас **не** перестраивается на каждый hover/press. `cache_start()` вызывается из `visual_initialize` (`host_shell.c:643`), а `publish_visual` (`:649-674`) только меняет visual snapshot; `cache_worker_main` (`:495-537`) строит атлас один раз (и при resize). Поэтому `unpremultiply_atlas` с делением на альфу (`overlay_canvas.c:168-231`) — не горячий путь; оптимизировать его в первую очередь не нужно.

---

### N-10. Мелкие кандидаты: input ring, audio memset, window queue

**Приоритет: P2/P3; только после профиля.**

- `input_ring_push/pop_batch` (`input_ring.c:23-66`) используют мьютекс, хотя producer/consumer фактически SPSC. `input_adapter_drain` дополнительно берёт `input_ring_get_stats` отдельным локом. При 120 Гц и малом количестве событий выигрыш небольшой; lock-free SPSC имеет смысл только если профиль покажет конкуренцию.
- `audio_data_callback` (`platform_android.c:1630-1643`) сначала делает `memset(audio_data, 0, need)`, затем читает ring. Если ring полон, memset полностью перезаписывается. Можно обнулять только хвост после `audio_ring_read`; это real-time callback, но экономия — доли микросекунды.
- `u_msg_post/u_msg_take` (`hle_android.c:6841-6901`) — глобальный `g_uq_lock`, линейный поиск и `memmove` до 64 сообщений на каждый `PeekMessage`. `WM_MOUSEMOVE` коалесцируется, поэтому очередь обычно короткая; низкий приоритет, но это ещё один общий лок на input-пути.

---

### N-11. Latent-проблемы target `platform_android`: TLS и sync

**Приоритет: latent; не текущий горячий путь, но обязательно до миграции HLE на `ThreadApi`/`SyncApi`.**

`runtime/platform/android/platform_android.c:489-548`: `android_tls_get`/`android_tls_set` берут глобальный `g_tls_lock` на **каждое** обращение, затем вызывают `pthread_getspecific/setspecific`. Сейчас target TLS API не является основным путём legacy-HLE, но после миграции это будет тот же emutls-подобный по стоимости слой.

`android_sync_lookup` (`:664-681`) линейно ищет объект в глобальном списке под `g_sync_lock`; `android_event_set` делает `pthread_cond_broadcast` на все объекты (`:701-713`); `WaitForMultipleObjects` ищет все handles и держит глобальный лок (`:877-939`). Это ровно тот класс, который P0-4 запрещает на горячем пути. Пока HLE использует legacy-реализацию, это не проявляется; при переносе нужно сразу делать direct-mapped таблицу и per-object condition variable.

`android_thread_create` (`:367-400`) задаёт stack size только при `desc->stack_size >= PTHREAD_STACK_MIN` и не вызывает `cpu_x87_init`; legacy `guest_thread_proc` (`hle_android.c:5930-5938`) вызывает. Это подтверждает P0-9 и является latent-разрывом, а не текущей оптимизацией.

---

## 6. Проверено и отклонено

Чтобы не тратить время на ложные цели:

| Гипотеза | Проверка | Вывод |
|---|---|---|
| «Атлас overlay перестраивается на каждый hover/press» | `cache_start` вызывается из `visual_initialize` (`host_shell.c:643`), `publish_visual` только меняет snapshot, `cache_worker_main` строит атлас один раз | **Неверно**; это не горячий путь |
| «`unpremultiply_atlas` делит на альфу каждый кадр» | Функция вызывается только из `overlay_canvas_build`, а тот — из cache worker при init/resize | **Не каждый кадр**; низкий приоритет |
| «Нужно включить `-ffunction-sections`» | NDK уже передаёт `-ffunction-sections -fdata-sections` (видно в `build.ninja`) | Уже включено; документация права |
| «`--gc-sections` уберёт почти весь корпус» | `g_lift[]` содержит 181 003 записи и является корнем; всего `L_*` в .so 196 115 | Потолок ~15 тыс. функций, а не 181 тыс. |
| «Переставить поля `CPU`» | 95,6% обращений к 9 полям в первых 60 байтах; это одна cache line | Закрытый вопрос, выигрыша нет |
| «Тюнинг hash dispatcher» | P1-5 закрыт измерениями: Knuth 2^19 лучше альтернатив | Не тратить время |
| «Звук уже lock-free, значит всё хорошо» | Callback действительно без host-lock и аллокаций; остаётся только `memset` | Только микрооптимизация |
| «DXVK можно оптимизировать в этом дереве» | В `runtime/gfx/` только `null/`; DXVK — pinned vendor-объекты | Вне границ этого анализа |

---

## 7. Предлагаемый порядок работ

Порядок — по соотношению «выигрыш / риск / стоимость», а не вместо ранжирования PM-17. Все изменения — только после A/B по PM-19.

### Этап 0. Базовая линия и профиль

- Собрать `--profile perf` (PM-14) и снять на T2: frame p50/p95/p99, map load, `dispatch/frame`, `probes/frame`, `flags/frame`, `x87 ops/frame`, HLE-вызовы, CS wait, audio underrun.
- `simpleperf` на текущем legacy release: проверить долю `__emutls_get_address`, `pthread_getspecific`, `hle_call`, `guest_ok`, `memcpy`.
- Без устройства честно зафиксировать `unavailable`, а не выдумывать проценты.

### Этап 1. Дешёвые и широкие

1. **N-01, native TLS.** Один build-флаг, максимальный охват: dispatch, x87, SoftFloat, HLE `g_last_error`/`g_self_thread`. Обязательный smoke-тест загрузки .so.
2. **P0-7, убрать глобалы с `hle_call`.** `e->calls` оставить только для `hle_stub`; `g_last_cpu`/`g_ring` — под `#if WC3_DEBUG`. Это подтверждённый активный путь и почти нулевой риск для guest-семантики.
3. **N-05, version script** + **N-07, packed relocs**. Быстро, проверяется `readelf`/JNI smoke.
4. **N-06, `-fno-unwind-tables` только для corpus.**
5. **N-04, 32-битные смещения `g_lift`.** Средняя сложность, но локально и без ABI.

### Этап 2. Известные P0 по каталогу

- **P0-1** — девиртуализация константных целей; самый большой потенциальный выигрыш, но требует G-14/G-15 и аккуратного `g_disp_depth`/`g_tail_pending` (после N-01 эти поля уже не будут стоить libc-вызовов).
- **P0-2/P0-10** — dead flags/field-level DFE.
- **P0-3** — свернуть `GVA()`.
- **P0-5/P0-6** — импортные thunk'и и return-address elide.
- **P0-4/P1-20** — direct-mapped sync + atomic CS fast path; heap — шардирование.

### Этап 3. HLE и overlay

- **N-03** — пакетная валидация строк после P1-19.
- **N-08** — `HeapReAlloc` in-place + `HeapFree` checked.
- **N-09** — индекс слоёв overlay; dirty-rect только после доказательства поведения `ANativeWindow`.
- **N-10** — input ring/audio/u_msg, если профиль покажет.

### Этап 4. Relift

- **N-02** — `-O2` для relift-корпуса, но только через G-15/PM-23 и golden-набор на `-O0`/`-O2`. Это отдельный, более рискованный трек, чем текущий legacy-продукт.

---

## 8. Как проверять результат

Минимальный набор для каждой оптимизации:

| Что | Инструмент/метрика | Критерий |
|---|---|---|
| Emutls | `simpleperf`, дизассемблер; исчезновение `__emutls_get_address` из профиля | Нет вызовов на горячем пути; .so загружается |
| Dispatch | `dispatch/frame`, `probes/frame`, hit/miss | Снижение без роста miss |
| X87/SoftFloat | `x87 ops/frame`, доля SoftFloat | Снижение TLS-времени |
| HLE-строки | profile HLE, time в `lstrlenA`/`strcmp` | Снижение без изменения T1/T2 |
| `g_lift` | `readelf -r`, размер `.data.rel.ro`, время загрузки | ~181 тыс. RELATIVE меньше |
| Symbols/eh_frame/relocs | `readelf`, размер .so, load time | Размер/символы меньше; JNI работает |
| Heap | allocator profile, peak RSS, map load | Меньше копирований/пиков |
| Overlay | время `overlay_android_present`, p95 | Без визуального diff |
| Relift `-O2` | G-15 golden, frame time, map load | Exactness не хуже `-O0` |
| Exactness | E-контракт, V5/G-15 | Нет изменения guest-семантики |

Для generated C обязательны **G-15/PM-23**, для dispatcher — **G-14**, для HLE/overlay — T0/T1; отсутствие устройства фиксируется как `unavailable`, а не как «доказано».

---

## 9. Ограничения отчёта

- Это статический анализ, не бенчмарк. Проценты ускорения не приводятся: без T2 любая цифра была бы выдуманной.
- Текущий release — legacy prebuilt corpus с `-O3`; вывод про `-O0` относится к relift-пути и не объясняет текущий .so.
- DXVK и его pipeline compilation/staging не анализировались: в целевом дереве нет их исходников, только pinned объекты. P2-10 остаётся отдельной задачей.
- Код проекта не изменялся. Удалена только ошибочная внешняя папка `WarsmashAndroidPort2022`, не относящаяся к этому проекту.

Если выбирать **три первых действия**, то это: **N-01 (native TLS), P0-7 (убрать глобалы с `hle_call`) и N-04 (смещения в `g_lift`)**, с параллельной подготовкой N-02 для relift-пути.
