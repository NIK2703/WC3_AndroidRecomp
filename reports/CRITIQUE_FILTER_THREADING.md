# Фильтр критики с учётом сохранения guest threads

Контекст: принудительная однопоточность отменена. Guest threads из старого
проекта сохраняются; удаляются только добавленные host mutex/lock/critical
section/atomic-lock. Поэтому пункты критики, которые требуют удалить потоки
или свести всё к одному OS-потоку, отклонены. Пункты про синхронизацию,
debug-gate, DXVK и release hygiene оставлены.

## Принято

| Пункт | Что делаем |
|---|---|
| A1 (частично) | Однопоточность не считается доказанной. Добавлен `docs/THREAD_AUDIT.md`; без PE/устройства — `unavailable`, не pass. |
| A3 | Thread/sync HLE не заглушки: `CreateThread` сохраняется, waits/events/mutex/semaphore/CS получают точную семантику или fatal. `ThreadApi`/`SyncApi` возвращены в platform contract. |
| A4 | Debug-gate: `#if WC3_DEBUG`, вложенные подтеги, `#ifndef -> 0`, release `-DWC3_DEBUG=0 -DNDEBUG`, генераторы без `g_dispatch_diag`/`WC3_REG_GUARD`; `HleEntry.name/calls` debug-only. |
| A5 | Старый план с Windows/размерами не является target. Актуальный `PLAN_REFACTOR_1.27B.md` — Android-only; старый файл нужно пометить/удалить. |
| A6 | `dxvk_win32` — не «только символы», а DXVK-required thread/sync shim; допускается только поимённым allowlist. DXVK internal threads — vendor exception. |
| A9 | Один HLE ABI/registry; `runtime/host` — legacy и не входит в Android link graph. |
| A10 | `--concurrency` проверяет mutex/lock/CS/atomic-lock и новые host threads; guest threads разрешены; DXVK shim/third_party — allowlist. |
| A11 | Release crash: fatal-only abort без logcat/файла/dump; debug — logcat+файл, dump под `#if WC3_DEBUG`. |
| C.3–C.4 | Release-флаги, preprocessed/binary gate, hot/cold HleEntry, без per-call ring/counters. |
| C.6 | Thin bridge — единственный frontend; оба DXVK в одном APK; symbol isolation и выбор по Vulkan 1.3+ обязательны. |
| C.7 | Fatal imports; `hle_stub` и sync-заглушки запрещены. |
| D | Source scan + preprocessor + binary + link graph + device thread audit. |

## Отклонено или переписано

| Пункт | Почему |
|---|---|
| A1 (вывод «нужен один OS-поток/один стек») | Это старая однопоточная модель. Новое правило: guest threads сохраняются, каждый имеет CPU/TEB/stack/TLS; один стек запрещён. |
| A2 «выбрать main thread или dedicated native thread» | Навязывание одного runtime-потока отменено. Существующие Android threads сохраняются; cross-thread события — lock-free mailbox. |
| A7 «удалить input/overlay/canvas pthreads» | Потоки не удаляются. Удаляются mutex/cond/lock; HLE-actions не вызываются из UI. |
| A8 «один стек несовместим» | Согласны только с per-thread stacks/TEBs. Одностековая модель удалена из архитектуры. |
| C.5 «удалить runtime/android threads» | Отклонено: сохраняем существующие потоки, убираем locks. `dxvk_win32` — allowlist, не guest runtime. |
| F outline «один OS runtime thread» | Устарел; заменён на guest threads + lock-free handoff. |

## Остаётся P0

1. `docs/THREAD_AUDIT.md` — заполнить по PE/устройству; сейчас `unavailable`.
2. Sync-заглушки (`WAIT_OBJECT_0` для thread/semaphore, no-op ReleaseSemaphore)
   заменить точной семантикой или fatal.
3. Debug-gate: заменить все `#ifdef WC3_DEBUG` на `#if`, вложить подтеги,
   исправить `gen_dispatch`/`lift` и release-флаги.
4. DXVK dual-version: symbol isolation, один selector по Vulkan 1.3+, не
   инициализировать второй DXVK; проверить link graph.
5. Release crash: fatal-only; убрать log/dump из release.
6. `runtime/host` и старый Windows-план — вне Android link graph; старый
   `PLAN_REFACTOR_1.27B.md` в `warcraft3_recomp` не должен читаться как target.
