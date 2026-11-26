# Direct-touch debugging: state after rev 56

## Failed attempts (do not repeat)

1. rev 52 — read `CGameUI+0x41C` as a `CFrame`; result: `panel=63280A98`, rect unreadable, haptic unchanged.
2. rev 53 — scan global frame registry `[0x6FCE3BA4]/[0x6FCE3B9C]` for id `0x47`; result: SIGSEGV at `0x100FFFF0C` in `dt_frame_by_id` on map start.
3. rev 54 — read panel object's `+0xB4` and search direct children by `+0xC`; result: `frame=none`, `have_button=0`, `minimap-input` unchanged.
4. rev 55 — remove registry scan (crash fixed) but add `wc3_guest_range_ok()` to `hle_android_guest_committed()`; result: `dt_guest_readable(ui,…)` false, gate collapsed to NATIVE, world touches became raw selection.
5. rev 56 — revert that guard; gate is FIELD again. User still reports “not fixed”.

## False assumptions found

- “Quick-swipe selection is a routing bug.” Fresh rev 56 logs show `gate => FIELD`, `FIELD_CAMERA_BEGIN/MOVE/END`, `field-camera-begin/end` with mode-neutral, and no TAP for the swipe. The recognizer does the right thing.
- “`+0xC` of a frame child is a small frame ID.” Fresh `beacon-child` dump shows values `0x418E013C`, `0x41AB017C`, `0x4193013C`, `0x41FF016C` — pointer-like, and one of them is exactly `CMinimap+0xB4`. So `+0xC` is not the ID field.
- “Reading `panel+0xB4` gives the minimap panel frame.” `minimap-input` is still the old `0.0094,0.7817..0.1488,0.9931`; the panel rect never got published.
- “The global registry is an ID index.” `L_6F111410` is a release/destruction loop over a count, not a name→frame lookup.

## Evidence files

- `/data/user/0/com.termux/files/usr/tmp/wc3_rev56_live/touch_trace.txt`
- `/data/user/0/com.termux/files/usr/tmp/wc3_rev56_live/dt_trace.txt`
- `/data/user/0/com.termux/files/usr/tmp/wc3_rev56_live/wc3_log_debug.txt`

Key lines: `beacon-frame id=00000047 ... frame=none`, `beacon-child root=63350B4C ... id=4193013C rect=0`, `rects[idle] minimap-input=1(0.0094,0.7817..0.1488,0.9931)`, `gate => FIELD`, `FIELD_CAMERA_BEGIN/MOVE/END`.

## Static findings after rev 56 (server restart session)

- `0x6F111070` (native frame hit test) proves the `CFrame+0x40` child list is
  a linked list of **nodes**: `node+8` = next, `node+0xC` = child frame
  pointer, `node+0x10` = edge mask.  Rev 54's `dt_frame_find_id` treated the
  node as a frame and its `+0xC` as an ID; that is why the dump showed
  pointer-like values and `rect=0`.
- `0x6F35E01A` stores the result of the `"MinimapSignal"` name lookup in
  `CGameUI+0x484`, **not** `+0x440`.  `0x6F36195F` is the native event case
  that compares the message ID to `[ui+0x484]` and then calls the beacon
  action (`0x6F363BB0`).  The old `+0x440` slot is a different control; the
  previous analysis of `0x6F36F870` as the signal handler was wrong.
- The panel container at `CGameUI+0x41C` embeds its frame at `+0xB4`.  A
  layout container can have a clear visible flag while its rect is live, so
  `dt_minimap_panel_snapshot()` must use `dt_frame_rect_read()` (no visible
  gate) and let `wc3_dt_minimap_panel_plausible()` reject bad rects.

## Edits made (not built, not installed)

`runtime/hle/android/hle_direct_touch.c` only:
- `DT_UI_MINIMAP_SIGNAL` changed from `0x440` to `0x484`.
- Child-list traversal now follows `node+0xC` to the child frame and
  `node+0x8` to the next node; added `DT_FRAME_NODE_*` defines and
  `dt_control_has_id()` bounded ID match.
- Panel rect read without the visibility gate.
- Debug logs now show `id484`, `id440`, node/child frame, rect, and ID match.

`clang -fsyntax-only -DWC3_DEBUG=1 -DWC3_DEBUG_INPUT=1` passes with the
project include paths.  No build was run and no binary was installed.

## User-confirmed symptom (after this session)

"The upper part of the minimap only moves when the finger starts moving after a
long tap; the beacon button only responds to a long tap; dragging the beacon
onto the minimap does not place a signal — world gestures dominate the minimap
and its buttons."

This matches the published gates exactly:
- rev 56 `minimap-input` top is `0.7817` (command-card top), while the
  beacon/button row is around `0.77`.  A touch there routes `FIELD`, so a
  quick swipe pans the **world** camera; a long press becomes a native select
  click, which happens to activate the native signal button on release.
- `have_button=0` on every log line, so `dt_beacon_pointer()` never claims a
  button-origin drag; the gesture is treated as a minimap/native drag.
- `dt_native_defer_pointer()` also excludes only `on_minimap` from the field
  deferral, so the same strip above the published rect becomes a deferred
  field tap.

Fix path (already applied in the working tree, see above):
1. publish the full panel rect with `dt_frame_rect_read()`;
2. resolve `MinimapSignal` at `CGameUI+0x484`, follow `node+0xC` to the child
   frame, and match the ID (plus the constructor name pointer) in the control;
3. union the beacon rect into `minimap-input`;
4. route beacon rect to NATIVE before the minimap gate, so `dt_beacon_pointer`
   sees the DOWN and owns the drag.

## Build/install (rev57)

User explicitly ordered `собери и установи дебаг со всеми изменениями`.
Command:
`bash tools/android/build_apk.sh --variant debug --install --serial emulator-5554`

Result: BUILD SUCCESSFUL, `Performing Streamed Install / Success`.
- log: `build/android_app/debug-build-install-20261126-rev57.log`
- apk: `build/android_app/wc3-debug.apk` (178837017 bytes)
- so: `build/android_app/debug/cmake/libwc3gameactivity.so` (160433688 bytes)
- device: `versionCode=2`, `versionName=0.1.1-debug`,
  `lastUpdateTime=2026-11-26 12:18:52`
- distcc stayed enabled (`workers=pc local=8 remote=4 jobs=12`), no
  `WC3_DIST_AUTO=0`.

## Still required

1. Ask which symptom remains: beacon does not fire / minimap haptic /
   camera does not visually move / selection appears.  The rev 56 logs prove
   `FIELD_CAMERA_BEGIN/MOVE/END` are emitted, so if the camera does not move
   the next suspect is `dt_camera_pan_apply()`, not route policy.
2. On explicit user command, build debug and inspect: `beacon-frame
   id484=… frame=<nonzero>`, `beacon-child … frame=<nonzero> id=1`,
   `minimap-input` larger than `0.0094,0.7817..0.1488,0.9931`,
   `beacon-up … from_button=1`, `beacon-place-minimap`, and no haptic on a
   panel-edge tap.
3. If `id484` does not match any child, dump raw dwords of the matched
   control object around the matched offset to pin the ID field.

## Correction after rev57 log review

`CMinimap+0x484` is **not** the signal control: it is `"UnitCenter"` (stored
by `0x6F35E01A`, compared by `0x6F36195F`, acted on by `0x6F363BB0`).  The
work tree now uses `CGameUI+0x440` again.  Native initializer `0x6F367B43`
shows the whole ID table: `+0x42C QuickSave`, `+0x430 QuickLoad`,
`+0x434 QuickHelp`, `+0x438 QuickOptions`, `+0x43C QuickQuit`,
`+0x440 MinimapSignal`, `+0x444 MinimapTerrain`, `+0x448 MinimapColors`,
`+0x44C MinimapCreeps`, `+0x450 FormationToggle`; the strings live at
`0x6FAB9EA8..0x6FAB9F28`.  Runtime ID values (older log): `440=0x47`.

Frame-tree facts confirmed from the lifted code and rev56/rev57 logs:
- A control object embeds its CFrame at `object+0xB4`; the control's child
  list is `[frame+0x40]`, a node list with `node+0x8 = next`,
  `node+0xC = child frame`, `node+0x10 = edge mask`.  `node+0xC` is a pointer,
  never an ID.  The global-frame-registry scan stays removed (it crashed at
  map start).
- `CFrame` ctor `0x6F0EADB0` writes arg2 to `+0x24` and arg3 to `+0xA8`;
  `0x6F110180` initializes the hit-testable control at `+0xB4` with rect
  `+0x44/+0x48/+0x4C/+0x50`, visible `+0x54`, and `+0x40` as its child-list
  sentinel (`~(frame+0x3C)` when empty).
- The panel at `[CGameUI+0x41C]` has exactly four child frames
  (`0x418E013C`, `0x41AB017C`, `0x4193013C` = `CMinimap+0xB4`,
  `0x41FF016C`).  `CMinimap+0xB4` itself has **no** children.  Therefore the
  signal button is either one of those panel children or a CGameUI-level
  frame; the current DFS searches the panel, minimap and UI roots.
- `0x6F3A1C60` is `CTimeOfDayIndicator`, not the minimap image; the earlier
  note that it constructs the minimap image was wrong.  The FDF is loaded
  from `UI\FrameDef\FrameDef.toc`; a read-only MPQ reader was built in
  `/data/user/0/com.termux/files/usr/tmp/opencode/mpqx.py` for further data
  research (not part of the product).

Current work-tree changes (not yet built):
- revert to `DT_UI_MINIMAP_SIGNAL = 0x440`;
- correct node/child traversal in `dt_frame_find_id()` and child logging;
- match the control by constructor name (`MinimapSignal`,
  `MinimapSignalButton`, `MiniMapSignalButton`) at `+0xA8`/`+0x24`, plus the
  numeric ID anywhere in the bounded frame object;
- add the `CGameUI+0xB4` subtree as a third DFS root;
- keep panel rect publication without a visibility gate and union the beacon
  rect into `minimap-input`.

`clang -fsyntax-only -DWC3_DEBUG=1 -DWC3_DEBUG_INPUT=1` passes.

## Build/install (rev58)

User explicitly ordered `собери и установи дебаг со всеми изменениями`.
Command:
`bash tools/android/build_apk.sh --variant debug --install --serial emulator-5554`

Result: BUILD SUCCESSFUL, `Performing Streamed Install / Success`.
- so: `build/android_app/debug/cmake/libwc3gameactivity.so` (160432208 bytes)
  SHA-256 `bd66e73f63f0e10055cb812879f2c4efebba558346589dc4a3cf1c1af463020a`
- apk: `build/android_app/wc3-debug.apk` (178835537 bytes), SHA-256
  `686d4cfd11ee8a851050766b808324e55787d93616046624b55b1adff9d0ee36`
- the APK's `lib/arm64-v8a/libwc3gameactivity.so` hashes to the same
  `bd66e73f...` as the built SO, so the installed binary matches the tree.
- device: `versionCode=2`, `versionName=0.1.1-debug`,
  `lastUpdateTime=2026-11-26 14:00:11`
- distcc stayed enabled (`workers=pc local=8 remote=4 jobs=12`,
  `pc target=10.124.100.62:39559`); no `WC3_DIST_AUTO=0`.

Next: pull fresh rev58 logs after the user exercises the minimap/beacon, then
check `beacon-frame ... frame=<nonzero>`, `beacon-child ... match=1`,
`beacon-down ... on_button=1`, `beacon-up ... from_button=1`,
`beacon-place-minimap`, expanded `minimap-input`, no haptic on the panel edge,
and selection only on long tap.

## Rev59 static verification (no build/install)

Native layout proof:
- `0x6F36D890` creates five CFrames at `[CGameUI+0x408..0x418]`; `+0x408` is
  MinimapSignal and gets the `MiniMapSignalButton*` textures at `D9A1..D9EA`.
  They are children of the `+0x3D4` container under `+0x428` SimpleConsole.
- Anchor type 6 = BOTTOMLEFT; signal x=0.154375, y=0.14375, w=0.023125,
  h=0.020625.  Type 8 = BOTTOMRIGHT (command bar: matches the published
  `command=0.7727,0.7817..0.9908,0.9967` exactly after the 0.8/0.6 scale).
- `hle_keyboard_rect_from_ddc()` documents the authoritative transform:
  every UI frame rect is in the fixed 0.8x0.6 DDC space (`0x6F193532`),
  surface x = x/0.8, surface top = 1 - maxY/0.6.
- Consequence: the beacon button is `0.1930,0.7260..0.2219,0.7604`; the map
  image is `0.0118,0.7583..0.1860,0.9885`; the portrait is
  `0.2594,0.8063..0.3719,0.9490`; `minimap-input` becomes
  `0.0118,0.7260..0.2219,0.9885`, with no portrait/button overlap.
- The old unscaled map/portrait published rects were too small and too low;
  the button column fell under the portrait gate and the panel edge leaked to
  FIELD.  Fixed: `dt_minimap_snapshot()`, `dt_portrait_snapshot()` and
  `dt_minimap_panel_snapshot()` now use the console/DDC readers.
- DFS/id-scan removed; no references to `dt_frame_find_id`,
  `dt_control_has_id`, `dt_guest_string_is` or `beacon-child` remain.
- `clang -fsyntax-only -DWC3_DEBUG=1 -DWC3_DEBUG_INPUT=1` passes;
  `git diff --check` clean.  No build/install was run.
- Still needs a user-ordered rev59 debug build + fresh logs to confirm
  `beacon-frame button=<nonzero> rect=1`, `beacon-down ... on_button=1`,
  `beacon-place-minimap`, expanded `minimap-input`, no haptic at the edge,
  and selection only on long tap.

## Build/install (rev59)

User explicitly ordered `собери и установи дебаг со всеми изменениями`.
Command:
`bash tools/android/build_apk.sh --variant debug --install --serial emulator-5554`

Result: BUILD SUCCESSFUL (58 s), `Performing Streamed Install / Success`.
- so: `build/android_app/debug/cmake/libwc3gameactivity.so` (160420688 bytes)
  SHA-256 `2c54d6f1a225a1d2a9c06720ffcdae6acb673fbc6a592858e502f65ea7bd3e34`
- apk: `build/android_app/wc3-debug.apk` (178824017 bytes), SHA-256
  `1bcc86af143c1a7ec2b07aab1c9e4403e38cfc30d63f3c233e69e817f919b680`
- APK's `lib/arm64-v8a/libwc3gameactivity.so` hashes to the same
  `2c54d6f1...` as the built SO.
- device: `versionCode=2`, `versionName=0.1.1-debug`,
  `lastUpdateTime=2026-11-26 15:26:05`.
- distcc stayed enabled (`workers=pc local=8 remote=4 jobs=12`,
  `pc target=10.124.100.62:37381 slots=4`); zero `WC3_DIST_AUTO=0` in the log.
- `hle_direct_touch.c` object rebuilt by ccache; manifest audits all CONSISTENT;
  `no_softfloat_symbols: clean`.

Next: pull fresh rev59 logs after exercising the minimap/beacon and check the
signals listed in the previous section.

## Rev59 beacon vertical mirror fix (source only)

User-confirmed: everything works except the beacon signal appears mirrored
about the map's horizontal centre (top release lands at the bottom, and vice
versa).  Native `0x6F3B0A30` AddSignal reads CMinimap+0x644..0x650 as CRect
`{minY, minX, maxY, maxX}` in DDC space (Y grows upward) and clamps `sy` to
`[minY, maxY]`.  `wc3_dt_minimap_local_point()` expects a surface top-left
rect/Y-down fraction, so passing the release fraction straight through mapped
surface top -> minY (bottom) and surface bottom -> maxY (top).

Fix: added `wc3_dt_minimap_ddc_local_point()` in the intent header, which
mirrors the fraction inside the frame (`frame_top + frame_bottom - ny`) and
reuses the same clamp/fraction math; `dt_beacon_place_minimap()` now uses it.
The pointer-phase call site keeps `wc3_dt_minimap_local_point()` unchanged.
Added T0 regression assertions (top -> maxY, bottom -> minY, centre unchanged)
in `tests/direct_touch_test.c`.

`clang -fsyntax-only` passes for both files; `git diff --check` clean.

## Build/install (rev60, vertical-mirror fix)

User explicitly ordered `собери и установи дебаг со всеми изменениями`.
Command:
`bash tools/android/build_apk.sh --variant debug --install --serial emulator-5554`

Result: BUILD SUCCESSFUL (1m 7s), `Performing Streamed Install / Success`.
- so: `build/android_app/debug/cmake/libwc3gameactivity.so` (160422864 bytes)
  SHA-256 `0d2b9f86da7937f3fea8ca0c3468b7105623c374c65190224094d3261b1a0611`
- apk: `build/android_app/wc3-debug.apk` (178826193 bytes), SHA-256
  `77cdb54acb152ee86a3c157a236424b368ff9ddc61be013f3804e55701dc312d`
- APK's `lib/arm64-v8a/libwc3gameactivity.so` hashes to the same
  `0d2b9f86...` as the built SO.
- device: `versionCode=2`, `versionName=0.1.1-debug`,
  `lastUpdateTime=2026-11-26 15:38:48`.
- distcc stayed enabled (`workers=pc local=8 remote=4 jobs=12`,
  `pc target=10.124.100.62:39133 slots=4`); zero `WC3_DIST_AUTO=0`.
- manifest audits CONSISTENT; `no_softfloat_symbols: clean`.

Next: user verifies on device that a top release lands at the top of the map
and a bottom release at the bottom.

## Pause-menu native click: gateway at release (source only)

User-confirmed symptom: pause-menu (Esc) buttons need a second tap; if another
button was hovered first, holding the next one presses the previous button.
This is the same stale hot-control class that `810af4e` fixed for the main
menu, but the pause menu did not use that path.

Root cause in code: a pause-menu press is inside the field rectangle, select
mode is inactive, no target mode is live, and the click gate is closed.  The
raw route is NATIVE; `dt_native_defer_pointer()` therefore takes the
`late_defer` branch.  On release `dt_field_release_tap()` sees route LEGACY or
NATIVE with no select, `should_defer` false, UI present, and returns
`WC3_DT_FIELD_RELEASE_NATIVE`.  That branch called `dt_native_click()` ->
`hle_android_mouse_press_replay()` (immediate DOWN+UP), bypassing the Native
Input Gateway's idle hover pass.  The DOWN was hit-tested against the previous
hot control; the next tap saw the refreshed control and activated.

Fix (no menu special case):
- `hle_direct_touch_intent.h`: `wc3_dt_native_release_replay(route,
  target_live)` selects the immediate replay only for a live target/placement
  mode outside LEGACY.  Every other native release is an ordinary UI click.
- `hle_direct_touch.c`: the NATIVE outcome uses `dt_native_press_deferred()`
  when replay is not selected.  It arms the DOWN at the release point through
  the same `dt_press_native()`/gateway used by every normal press.  The
  service publishes the DOWN after the idle `GetCursorPos` pass and posts the
  matching UP one pump later; a one-pump fallback publishes the DOWN for modal
  loops that never poll the cursor.  The guest still sees DOWN and UP in
  separate pumps.  A new pointer event is held while the deferred UP is in
  flight, and a context change/cancel drops the whole sequence.
- `dt_paused(c)` also blocks the replay: a paused game cannot confirm a target,
  so a stale target-mode word under the pause menu cannot select replay.
- `hle_android.c` / `native_input_gateway.h`: added
  `hle_android_mouse_press_pending()` so the release owner can distinguish an
  armed-but-unpublished press from a published/cancelled one.
- The moved late-defer drag path now publishes an armed DOWN before its UP
  (`hle_android_mouse_flush_pending_down()`), so a fast drag cannot queue the
  UP ahead of its DOWN.  The cancel path cancels an armed press, or posts the
  matching UP only when the DOWN was already published; it checks the pending
  kind first so a second finger's camera sequence is not cancelled.
- Deduplicated the native press bookkeeping: `dt_native_begin()` starts every
  press, and `dt_native_end()`, `dt_native_abort()`,
  `dt_native_replay_done()`, `dt_native_cancel_done()` own the release
  variants.  Normal DOWN/UP, field drag, deferred release, replay and cancel
  paths no longer repeat the raw flag assignments.

T0:
- `tests/direct_touch_test.c`: `test_native_release_replay()` asserts replay
  only for `NATIVE + target_live`, never for LEGACY or a non-target release.
- `python3 tools/run_tests.py --test direct_touch --test native_press_barrier`
  passes; `git diff --check` clean.
- NDK `clang -fsyntax-only` passes for `hle_direct_touch.c` and
  `hle_android.c` in both debug and release define sets.

No build/install was run.  Next: explicit user build command, then device
confirmation that the first pause-menu tap activates the intended button and
that holding a different button does not press the previously hovered one.
- Related T0 `controls_mode`, `gesture_lifetime` pass.  `gesture_mode` is red
  on clean HEAD `739069a` with the same two failures ("direct mode lost the
  camera scroll bindings", "two-finger drag did not enter camera scroll
  mode"); verified in a detached worktree, so it is pre-existing and
  unrelated to the pause-menu fix.

## Building placement: tap-tap target confirmation (source only)

User-confirmed symptom: tap a building command on the command card, then tap
the terrain.  The projection follows the tap point but the building is not
placed.  Dragging from the command icon to the terrain and releasing does
place it.

Root cause: the command's target/placement mode closes `CGameUI+0x1AC`
(the click gate).  `dt_context()` requires that gate, so on the world tap
`dt_field_target_tap_ready()` returned false and `dt_ready_route()` returned
LEGACY.  The release then fell into `dt_native_press_deferred()`: the gateway
posted the move (the projection visibly followed) and a deferred DOWN/UP, but
the modal placement loop may never poll `GetCursorPos`, so the confirmation
did not reach it.  The command-card drag worked because
`dt_command_context(..., require_gate = 0)` deliberately resolves a target
release without the gate.

Fix:
- `dt_context_impl(ctx, require_gate)` keeps the strict semantic context and
  adds `dt_target_context()` for target/placement, which does not require the
  gate.
- `dt_target_mode_live(ui)` reads the live active/select/target/camera words
  and uses the pure `wc3_dt_target_mode_live()` rule; camera is never a target.
- `dt_ready_route()` takes the closed-gate target branch only when the game is
  in-game and not paused, so the pause menu keeps the LEGACY/gateway path.
- `dt_field_target_tap_ready()` uses the same rule for a fresh world tap; the
  pause predicate keeps a stale target word under the pause menu out.  The
  field-drag forwarding path passes `NULL` and keeps the strict context.
- The CPU is threaded through `dt_native_pointer()` and
  `dt_native_minimap_pointer()` so the fresh tap can make that exact check.
- `wc3_dt_target_mode_live()` is in `hle_direct_touch_intent.h` and covered by
  `test_target_mode_live()`.

T0: `direct_touch` and `native_press_barrier` pass; NDK `clang -fsyntax-only`
passes debug/release; `git diff --check` clean.  No build/install was run.
Device confirmation pending: tap-tap building placement confirms on the first
terrain tap, drag-from-icon still confirms, pause-menu buttons still work.

## 2026-11-26: attack-move long-tap design

- New design doc: `docs/research/ATTACK_MOVE_LONG_TAP_PLAN_2026-11-26.md`
  (rev 1, `design`, not normative).  It records the current recognizer/HLE
  state, the `FIELD_SELECT_DRAG_BEGIN=18` event, the deferred-LMB condition,
  the `A`+left-click attack path, the release re-check, and the T0/T2 plan.
- No attack-move code was written at that point; no build/install was run.
- Settings finding: the long-press duration is the app setting
  `long_press_timeout_ms` (slider 50..200 ms, step 10, default 120), passed
  through `host_shell.c` -> `controls_init_ex()` ->
  `wc3_profile_apply_gesture_timings(full, ...)` -> `direct_touch_configure()`
  -> `wc3_direct_touch_init()` -> `dt->long_press_ms`.  The timing was applied
  only to the `full` profile; `simple`/`simple_esc` kept the authored value.
  `controls_set_overlay_mode(SIMPLE/SIMPLE_ESC)` and `controls_resize()` could
  therefore reconfigure direct touch with the authored value.

## 2026-11-26: attack-move long-tap implementation (source only)

User command: `реализуй`, then `продолжи`.

Implemented the full plan from the design doc:

- `runtime/ipc/direct_touch_event.h`: `FIELD_SELECT_DRAG_BEGIN = 18`.
- `runtime/platform/android/controls/direct_touch.h/.c`: `hold_x`, `hold_y`,
  `drag_started`; `arm_hold()` records the hold point; `move()` and `up()`
  emit `DRAG_BEGIN` once when the configured drag threshold is crossed.
  `dt_kind_name()` knows the event.  The threshold is `dt->drag_threshold_px`,
  not a constant.
- `runtime/hle/android/hle_direct_touch_intent.h`: `wc3_dt_point_in_rect()`,
  `wc3_dt_attack_candidate()`; `FIELD_SELECT_BEGIN` and `DRAG_BEGIN` cancel a
  pending field click.
- `runtime/hle/android/hle_direct_touch.c`: `select_deferred` /
  `select_deferred_pointer`; `dt_field_hud_contains()` using the published
  minimap-input/beacon/portrait/command rects; `dt_field_attack_ready()` with
  the native left-button pick; `dt_field_select_drag_begin()`; rewritten
  `dt_field_select_begin/end`; dispatch/reset/service handling; attack path
  `hle_android_key_event(0x41, 1/0)` + `dt_native_left_click()`.
  Release policy: reset (`time_ms == 0`) drop, busy drop, HUD drop, paused ->
  `dt_native_press_deferred()`, target -> `dt_native_click()`, otherwise
  attack if the re-check passes, else native left click.
- `runtime/platform/android/controls/controls.c`: timings are now applied to
  `full`, `simple` and `simple_esc`, so pause/cinematic keep the user setting.
  Added debug log `dt configure size=... drag=... long_press_ms=... haptic=...`.
- EN/RU `launch_gesture_hint_direct` updated with attack-move and
  no-selection lines.
- T0: `tests/direct_touch_test.c` event counts updated; added
  `test_configured_long_press_and_threshold` (50/200 ms, 11 px),
  `test_attack_candidate()`, `test_point_in_rect()`.
- `docs/research/ATTACK_MOVE_LONG_TAP_PLAN_2026-11-26.md` updated to the
  implemented behavior (status, §4.4/4.5/4.7/4.8, §7, §8, §9, §10, §11).

Verification done without building the product:

- `python3 tools/run_tests.py --test direct_touch` -> `ok`.
- `python3 tools/run_tests.py --test controls_mode` -> `okay`.
- `python3 tools/run_tests.py --test gesture_mode` -> `FAILED` with exactly
  the two pre-existing failures already verified on clean HEAD `739069a` in a
  detached worktree: "direct mode lost the camera scroll bindings" and
  "two-finger drag did not enter camera scroll mode".  They are unrelated to
  the timing/attack changes.
- `python3 tools/run_tests.py --test release_hygiene` -> `FAILED` only on the
  nine pre-existing ungated `fprintf(stderr, ...)` lines in
  `runtime/platform/android/guest_window_android.c`; none of the attack-move
  files are in the audit list.
- NDK `clang -fsyntax-only` for `hle_direct_touch.c` and `controls.c` in both
  debug and release define sets -> clean.
- `git diff --check` -> clean.

No APK/SO build and no install were run in that implementation step.  A
later explicit user command built and installed rev61 (see below); the T2
input path is confirmed in the rev61 logs, and distcc stayed enabled.

## Build/install (rev61, 2026-11-26 17:21)

User explicitly ordered: "собери и установи дебаг со всеми изменениями".

Command:
`bash tools/android/build_apk.sh --variant debug --install --serial emulator-5554`

distcc stayed enabled: `workers=pc local=8 remote=4 jobs=12`; `WC3_DIST_AUTO`
was unset and defaulted to 1.  Build exit 0; Gradle install `Success`.

Artifacts:
- APK: `build/android_app/wc3-debug.apk`
  - size 178852377
  - SHA-256 `f6b4e1e8b7a545124e255ac3a8fd55060a7e7b504df3700fb82734ccf18f6384`
- packaged SO: `lib/arm64-v8a/libwc3gameactivity.so`
  - size 160448776
  - SHA-256 `6d720d3b315044fb779228ee4fa720dc6f1dad1ee6308b126715bcbd9d721d74`
- installed base.apk hash matches the local APK hash.

Installed package:
- `wc3.recomp.debug`, versionCode 2, versionName `0.1.1-debug`
- lastUpdateTime `2026-11-26 17:21:46`
- device `23049PCD8G`, Android 17, physical 1080x2400

Pre-build T0: `direct_touch` and `native_press_barrier` pass.  The test-file
top/bottom half-open assertion fix is included.  NDK `clang -fsyntax-only`
debug/release for `hle_direct_touch.c` and `hle_android.c` passed before the
build.  No commit was made.

Device confirmation pending: pause-menu first tap and no previous-button
press; tap-tap building placement confirms on the first terrain tap;
drag-from-icon still confirms; beacon top/bottom is no longer mirrored.

## T2 attack-move confirmation (rev61, 2026-11-26 17:25)

Pulled the live debug files from the installed `wc3.recomp.debug` at 17:29
(last write 17:25): `dt_trace.txt`, `touch_trace.txt`,
`wc3_log_debug.txt`.  The installed base.apk hash matches the local rev61
APK, and the packaged SO contains `field-attack-move` and
`FIELD_SELECT_DRAG_BEGIN`.

Settings and timing:

- `shared_prefs/wc3_launch.xml` has `long_press_timeout_ms=100`.
- Every `touch_trace` long press shows `down event_t=N` ->
  `hold -> SELECT_BEGIN event_t=N+100` (or +101 on a tick boundary).  The
  recognizer is using the app setting, not the authored 120 ms default.
- The 50..200 ms clamp and the `full`/`simple`/`simple_esc` timing fix are
  in the rev61 SO; T0 `controls_mode` passed.

Attack path:

- 43 `field-select-deferred ptr=...` events, all with `pick=2` (terrain).
- 35 `field-attack-move ptr=...` events, all with `pick=2`.
- 8 `field-select-drag-begin` events.
- 43 = 35 attacks + 8 drags: in this capture every deferred gesture
  resolved as either an attack or a native selection drag; no fallback,
  HUD, paused, target, reset or busy drop occurred.
- Example attack at t=267771200: `pick result r=2`, then the native queue
  contains `msg=0100 wp=00000041` (WM_KEYDOWN A), `msg=0101
  wp=00000041` (WM_KEYUP A), `msg=0200` (move), `msg=0201`
  (WM_LBUTTONDOWN), `msg=0202` (WM_LBUTTONUP); `field-attack-move ptr=1
  pick=2`.  This is the same A+click sequence proven by rev58
  (`cmd=000D000F`).  The current capture does not log the game-side
  command descriptor, so the visual order confirmation remains user-side.

Drag path:

- All 8 drags ended with `field-select-end ... active=1 deferred=0`.
- No `field-attack-move` occurred between a `field-select-drag-begin` and
  its `field-select-end`.
- Example at t=267761516: `field-select-drag-begin`, native move to the
  hold point 1852,472, `msg=0201` down, moves, then `msg=0202` up at
  t=267761808.  The native selection rectangle starts at the hold point,
  not at the release point.
- Non-deferred long presses (no controlled selection / non-terrain) still
  take `active=1` at `FIELD_SELECT_BEGIN` and end with native UP; the
  attack disambiguation adds no latency there.

Minimap/HUD edge:

- No touch inside `minimap-input` in this capture; the edge case was not
  exercised.  The published rect is the expanded panel
  `0.0117,0.7583..0.2219,0.9885`; the route code is unchanged from the
  rev59/60 minimap fix.  Do not count this as a T2 pass.

No code changes were made by this verification and no rebuild was run.
The installed rev61 SO is the 17:17 artifact.  `hle_direct_touch.c` has a
later mtime (17:35) than the build; its attack-path functions and log
strings are present in the SO, but if that late edit is not comment-only it
is not in the verified binary.

## Building placement first-tap fix (source, 2026-11-26)

`rev63` (installed 18:19:43, APK SHA-256
`1e964d77fc80134589520209546d148b26a26962367e30cd1a146aeb75c5badc`)
reproduced the report: selecting a building command and tapping free
terrain moved only the preview; a second tap at almost the same point
confirmed the building.

Trace (`/data/user/0/com.termux/files/usr/tmp/opencode/rev63_logs/`):

- First tap at `1715,660`, DOWN/UP ~36 ms apart, left the active mode
  `42220088` unchanged; the second tap at `1723,689` exited it.
- Guest log shows `PeekMessage -> 0200`, `wndproc 0200 returned`, then
  `PeekMessage -> 0201` / `wndproc 0201`.  The release hover is
  dispatched before DOWN, but the placement preview updates only on the
  next guest frame.
- The first click therefore resolved against the stale preview; the
  second resolved against the first tap's position.

Source fix:

- `runtime/platform/android/controls/direct_touch.c`: emit
  `FIELD_HOVER` on a target/placement DOWN as well as on release/hold,
  so the guest gets the contact point earlier.
- `runtime/hle/android/hle_direct_touch.c`: the live target/placement
  native click now reuses the deferred field-click slot and is held for
  one 30 Hz frame (`DT_FIELD_TARGET_CLICK_DELAY_MS = 33`).  The tick
  revalidates `dt_field_target_tap_ready()` and `!dt_pause_menu_active()`
  before replaying, so a new gesture, a mode exit or an open pause menu
  cancels it.  Map-load replay and the pause-menu gateway are unchanged.
- The early hover is gated on a live target/placement mode; select and
  camera contacts do not move the shared cursor.

T0: `direct_touch`, `native_press_barrier`, `controls_mode`,
`gesture_lifetime` pass.  NDK `clang -fsyntax-only` passes for
`hle_direct_touch.c` and `hle_android.c` in debug and release define
sets.  `git diff --check` clean.

No build/install was run.  Next: explicit build command, install
`rev64`, then device-confirm pause-menu first tap, tap-tap building
placement, drag-from-icon placement, and beacon vertical mirror.

## rev64 build/install (explicit user command, 2026-11-26 19:23)

Command: `bash tools/android/build_apk.sh --variant debug --install --serial emulator-5554`
(log: `/data/user/0/com.termux/files/usr/tmp/opencode/build_debug_rev64_20261126.log`).

- Build exit `0`, Gradle `BUILD SUCCESSFUL in 1m 13s`, `Performing Streamed Install` / `Success`.
- Native SO: `libwc3gameactivity.so` 160453424 bytes, `no_softfloat_symbols: clean`,
  manifest audits `CONSISTENT`, `wc3_hle_android/hle_direct_touch.c.o` rebuilt.
- APK: `build/android_app/wc3-debug.apk`, 178857025 bytes, SHA-256
  `a91c3931cca9ff95b59eef4b241a157b3348c8ebd514dfc92b589e2a9173e374`.
- Installed `wc3.recomp.debug`, versionCode `2`, versionName `0.1.1-debug`,
  `lastUpdateTime=2026-11-26 19:23:51`.  Pulled installed `base.apk` — hash and
  size match the local APK byte-for-byte.
- Installed SO contains the new trace strings `field-target-hover`,
  `field-tap-release-native-queue` and `field-target-click-deferred`.

Device T2 confirmation still pending: pause-menu first tap, tap-tap building
placement, drag-from-icon placement, beacon vertical mirror.

## Pause menu = main menu route (source, 2026-11-26)

The pause menu now has its own physical route `MENU` and exactly the
main-menu gesture set.  Source only at that step; built and installed as
`rev65` below.

- Exact identity: `[CGameUI+0x228] -> CEscMenu`,
  `[CEscMenu+0x18] -> panel`, `[panel+0x170] != 0`; no paused-flag or
  geometric fallback.
- `WC3_DIRECT_TOUCH_ROUTE_MENU = 5`.  `dt_route_snapshot()` publishes it;
  `direct_touch_route_for()` resolves it to `NATIVE` (`menu-native`)
  before beacon/portrait/minimap/command/field geometry.
- Pause entry resets field/camera/selection/target/grid/inventory/command/
  command-target/beacon and defers the portrait release through
  `g_portrait_release_pending`.  The native press that opened the menu is
  not cancelled.
- `wc3_dt_menu_event_allowed()` accepts key signals, `POINTER_CANCEL` and
  raw native phases except `MINIMAP`; group signals and semantic
  field/portrait events are dropped.
- Controls transition cancel marks FIELD/PORTRAIT/MINIMAP owners
  `IGNORED` (their later phases are swallowed until UP), sends the
  minimap one `POINTER_CANCEL` with route `MINIMAP`, resets the
  recognizer, and leaves an active NATIVE press to finish.
- Minimap raw phases now carry `MINIMAP` via
  `wc3_direct_touch_emit_pointer_route()`; any other route is clamped to
  `NATIVE`.
- `dt_dispatch()` in pause bypasses beacon, command, inventory, grid and
  minimap and calls `dt_native_pointer()` directly.

T0: `direct_touch`, `controls_mode`, `native_press_barrier`,
`controls_idle`, `gesture_lifetime` pass.  Host `-fsyntax-only` with
`-Wall -Wextra -Werror` is clean for `hle_direct_touch.c` in release and
debug define sets.  `git diff --check` clean.

T2 still pending.  An explicit user command built and installed this change
as `rev65` below.

## rev65 build/install (explicit user command, 2026-11-26 20:08)

Command: `bash tools/android/build_apk.sh --variant debug --install --serial emulator-5554`
(log: `/data/user/0/com.termux/files/usr/tmp/opencode/build_debug_pause_menu_20261126_200847.log`).
distcc was left at its default auto-worker mode.

- Build exit `0`, Gradle `BUILD SUCCESSFUL in 1m 31s`, streamed install
  `Success`.
- Native SO: `libwc3gameactivity.so` 160461368 bytes, built 20:14:20;
  contains the new `pause-menu` and `menu-native` strings.
- APK: `build/android_app/wc3-debug.apk`, 178864969 bytes, SHA-256
  `e6816728c471e6d88dde25a2ec0d04f520a5ae57d34d05afdc092268f807b793`.
- Installed `wc3.recomp.debug`, versionCode `2`, versionName `0.1.1-debug`,
  `lastUpdateTime=2026-11-26 20:15:55`.  Pulled installed `base.apk` —
  SHA-256 and size match the local APK byte-for-byte.
- T0 before build: `direct_touch`, `controls_mode`, `native_press_barrier`,
  `gesture_lifetime`, `controls_idle` pass; `git diff --check` clean.

T2 found a pause-menu click regression in this build: see the next section.
rev65 must not be treated as the pause-menu result.

## Pause-menu click regression, root cause and source fix (2026-11-26)

rev65 reproduced the report: no pause-menu button responds to touch.
Logs pulled from the installed debug package
(`/data/user/0/com.termux/files/usr/tmp/opencode/pause_fail_logs_202430/`)
show the exact chain:

- `dt_trace.txt`: `snapshot route=MENU reason=pause-menu` is published, so
  the pause identity itself is correct.
- Each touch arrives as `gate => NATIVE (menu-native)` and
  `emit kind=POINTER_DOWN ... route=NATIVE`; the service logs
  `dispatch kind=POINTER_DOWN ... route=NATIVE` and
  `consumed=native-menu ptr=0`.
- `dt_native_pointer()` then logs
  `native-ptr kind=POINTER_DOWN ... down=0 up_pending=1 hw_down=0` and
  `[direct] field-late-defer`: the press is held by the field tap
  resolver even though the pause menu is open.
- On release the pause bypass calls `dt_native_pointer()` directly.  The
  deferred state has no posted DOWN and no movement, so the UP is dropped:
  the trace shows only `probe native-q[0] msg=0200` (a `WM_MOUSEMOVE`) and
  no `WM_LBUTTONDOWN`/`WM_LBUTTONUP`.

Root cause: rev26 kept a pause-specific exception inside
`dt_native_defer_pointer()`:

```c
const int inside_field = !on_minimap &&
                         (dt_field_contains(ev->x, ev->y) ||
                          dt_pause_menu_active());
```

The battlefield rectangle is still live under `CEscMenu`.  The new pause
bypass in `dt_dispatch()` skips the field resolver that was supposed to
turn the deferred UP into a click, so the two pause-specific paths
cancel each other.  The route and subgesture layers were unified, but the
native gateway was not: the main menu never enters this deferral, while
the pause menu did for every point.

Fix:

- `hle_direct_touch_intent.h`: new pure
  `wc3_dt_native_deferral_allowed(pause_menu)`, false while `CEscMenu` is
  open.
- `hle_direct_touch.c`: `dt_native_defer_pointer()` returns early when
  that policy rejects the menu, before the phase switch; `inside_field`
  is geometry-only again.
- Pause DOWN/MOVE/UP now take the same `dt_native_pointer()` gateway as
  the main menu; no field tap, target or late-defer resolver is involved.
- Pause entry also releases a field drag that already published a native
  DOWN (`dt_native_abort()` before `dt_field_click_reset()`), so the
  guest cannot keep the left button held while the menu is open.

Verification: T0 `direct_touch` (policy table +
`wc3_dt_native_deferral_allowed`), `controls_mode`,
`native_press_barrier`, `controls_idle`, `gesture_lifetime` pass.
NDK `clang -fsyntax-only` with `-Wall -Wextra -Werror` passes for
`hle_direct_touch.c` in release and debug define sets.
`git diff --check` clean.

Build/install evidence for rev66 is below.  T2 is still pending: pause
buttons must emit `WM_LBUTTONDOWN/UP` and activate on the first tap.

## rev66 build/install (explicit user command, 2026-11-26 20:40)

Command: `bash tools/android/build_apk.sh --variant debug --install --serial emulator-5554`
(log: `/data/user/0/com.termux/files/usr/tmp/opencode/build_debug_pause_fix_20261126_204027.log`).
distcc was left at its default auto-worker mode.

- Build exit `0`, Gradle `BUILD SUCCESSFUL in 1m 1s`, streamed install
  `Success`.
- Native SO: `libwc3gameactivity.so` 160461920 bytes (552 bytes larger than
  the rev65 SO, so the HLE change was recompiled).
- APK: `build/android_app/wc3-debug.apk`, 178865521 bytes, SHA-256
  `71a2c307827aeb1d3522625bdda4e7e0389bb2cbb218f3eb8745d0fbe7824f17`.
- Installed `wc3.recomp.debug`, versionCode `2`, versionName `0.1.1-debug`,
  `lastUpdateTime=2026-11-26 20:46:22`.  Installed `base.apk` SHA-256 and
  size match the local APK byte-for-byte.
- T0 before build: `direct_touch`, `controls_mode`, `native_press_barrier`,
  `gesture_lifetime`, `controls_idle` pass; NDK `clang -fsyntax-only`
  release/debug `hle_direct_touch.c` = 0/0; `git diff --check` clean.

Device T2 found a second failure: the cursor still only hovers, no pause
button clicks.  See the next section.  rev66 must not be treated as the
pause-menu result.

## rev66 T2 failure: pause press canceled by batch context switch

Logs pulled from the installed rev66 debug package
(`/data/user/0/com.termux/files/usr/tmp/opencode/pause_fail_rev66_205544/`)
show the press is now armed, then immediately canceled:

```
event kind=POINTER_DOWN(3) ... event_route=NATIVE service_route=NATIVE
native-ptr kind=POINTER_DOWN ... down=0 up_pending=0 hw_down=0
probe barrier-defer msg=0201 at=1414,359 lp=01670586
[direct] native-down ...
consumed=native-menu ptr=0
[direct] batch-context-change a=00000000 b=00000000
probe native-down-cancel
```

The armed `WM_LBUTTONDOWN` (`0201`) is canceled before the guest can
dispatch it.  The same sequence repeats for every pause tap; the release
posts only `probe native-q[0] msg=0200` (`WM_MOUSEMOVE`), which is exactly
the reported cursor hover with no click.

Root cause: in pause `dt_ready_route()` returns `NATIVE` with a **zeroed
semantic context**.  The batch loop still probes the live UI/world under
the panel:

```c
(void)dt_context(&probe);
const int context_switch =
    probe.ui != ctx.ui || probe.world != ctx.world;
```

`probe` has the live `ui=411C0088`, `world=411F0088`, while `ctx` is zero,
so `context_switch` is true on **every** pause event.  The
`batch-context-change` block calls `dt_native_release()`, which cancels the
just-armed native DOWN.  The XOR printed in the trace is `0/0` because
`next_ctx` is also zeroed by `dt_ready_route()`, which made the log look
like a no-op change.

Fix:

- `hle_direct_touch_intent.h`: new pure
  `wc3_dt_context_switch_relevant(pause_menu)`, false while `CEscMenu` is
  open.
- `hle_direct_touch.c`: `context_switch` is suppressed in pause; the live
  world under the panel is not a new context.  When the menu closes, the
  next normal route/context comparison runs again.
- `tests/direct_touch_test.c`: T0 asserts for the new policy.

Verification: T0 `direct_touch`, `controls_mode`, `native_press_barrier`,
`gesture_lifetime`, `controls_idle` pass; NDK `clang -fsyntax-only`
release/debug `hle_direct_touch.c` = 0/0; `git diff --check` clean.

Build/install evidence for rev67 is below.

## rev67 build/install (explicit user command, 2026-11-26 21:18)

Command: `bash tools/android/build_apk.sh --variant debug --install --serial emulator-5554`
(log: `/data/user/0/com.termux/files/usr/tmp/opencode/build_debug_pause_fix2_20261126_211831.log`).
distcc was left at its default auto-worker mode.

- Build exit `0`, Gradle `BUILD SUCCESSFUL in 1m 3s`, streamed install
  `Success`.
- Build log line 599 recompiled
  `CMakeFiles/wc3_hle_android.dir/runtime/hle/android/hle_direct_touch.c.o`;
  the linked SO is `libwc3gameactivity.so` 160466920 bytes.
- APK: `build/android_app/wc3-debug.apk`, 178870621 bytes, SHA-256
  `6eb27d479bb90cc47ecf8f272b8a199c516a42a7fddcb2a3d2feb2b7d91f4f2f`.
- Installed `wc3.recomp.debug`, versionCode `2`, versionName `0.1.1-debug`,
  `lastUpdateTime=2026-11-26 21:24:49`.  Installed `base.apk` SHA-256 and
  size match the local APK byte-for-byte.
- T0 before build: `direct_touch`, `controls_mode`, `native_press_barrier`,
  `gesture_lifetime`, `controls_idle` pass; NDK `clang -fsyntax-only`
  release/debug `hle_direct_touch.c` = 0/0; `git diff --check` clean.

Device T2 pending: pause taps must show `probe barrier-defer msg=0201`,
then `0202` on release, with **no** `batch-context-change`/`native-down-cancel`
in the pause path.  Also check first-tap activation, entering pause with a
live field drag, and the earlier first-tap/placement regressions.

## rev68 build/install (explicit user command, 2026-11-26 21:52)

Command: `bash tools/android/build_apk.sh --variant debug --install --serial emulator-5554`
(log: `/data/user/0/com.termux/files/usr/tmp/opencode/build_debug_all_20261126_214520.log`).
distcc stayed at its default auto-worker mode.

- Build exit `0`, Gradle `BUILD SUCCESSFUL in 2m 9s`, streamed install
  `Success`.
- Build log lines 420-428/599-603 recompiled `direct_touch_event.c`,
  `direct_touch.c`, `controls.c`, `hle_direct_touch.c`; linked SO
  `libwc3gameactivity.so` 160467992 bytes.
- APK: `build/android_app/wc3-debug.apk`, 178871925 bytes, SHA-256
  `7c107a3722bec8195eb4446089f98467c3442dd9171483fb9cfcde15cfb7b93d`.
- Installed `wc3.recomp.debug`, versionCode `2`, versionName `0.1.1-debug`,
  `lastUpdateTime=2026-11-26 21:52:51`; installed `base.apk` matches the
  local APK byte-for-byte.
- T0 before build: `direct_touch`, `controls_mode`, `native_press_barrier`,
  `gesture_lifetime`, `controls_idle` pass; NDK `clang -fsyntax-only`
  release/debug `hle_direct_touch.c` = 0/0; `git diff --check` clean.
- T2 traces at 21:53 are in
  `/data/user/0/com.termux/files/usr/tmp/opencode/first_tap_world_rev68/`.
  They expose the two first-tap gaps recorded below; rev68 predates both
  source fixes.

## 2026-11-26: smart-order long-tap plan (documentation only)

- New design doc: `docs/research/SMART_ORDER_LONG_TAP_PLAN_2026-11-26.md`
  (rev 1, `design`, not normative). It records the corrected tap matrix
  (target controllability via `0x6F66EC60`, not selection ownership), the
  long-tap smart order without drag, the native selection rectangle on drag,
  the single `dt_smart_order_at()` path, and the T0/T2 plan.
- `ATTACK_MOVE_LONG_TAP_PLAN_2026-11-26.md` is marked superseded for the
  long-tap action; its `FIELD_SELECT_DRAG_BEGIN` machinery and long-press
  timing remain.
- `docs/README.md` indexes the new plan.
- No code was changed, no T0/T2 was run, and no build/install was performed.

## 2026-11-26: smart-order long-tap implementation (source only)

- `hle_direct_touch_intent.h`: `wc3_dt_tap_unit_intent()` decides a unit tap
  by target controllability (`0x6F66EC60`), not by the live selection;
  `wc3_dt_field_tap_intent()` is the single decision point;
  `wc3_dt_tap_intent()` is narrowed to the non-unit/terrain leg;
  `wc3_dt_smart_order_candidate()` replaces `wc3_dt_attack_candidate()`.
- `hle_direct_touch.c`: `dt_tap()` always left-picks, validates a `CUnit`,
  tests the target with `0x6F66EC60`, and routes `ORDER_UNIT`/`ORDER_GROUND`
  through the shared `dt_smart_order_at()`. A controlled target is selected
  by the native left click; a hostile/neutral/decoration target and terrain
  are smart-ordered. The native right-click fallback is guarded by
  `dt_world_click_allowed()`.
- Long tap: `dt_field_attack_ready()` is replaced by
  `dt_smart_order_ready()`; `dt_field_select_begin()` no longer picks at the
  deadline. A controlled selection in the field defers the native LMB;
  `FIELD_SELECT_DRAG_BEGIN` starts the native selection rectangle, and a
  stationary release calls the same `dt_smart_order_at(attack_move=1)`:
  a unit/decoration gets a smart order, terrain gets the restored
  attack-move (`A`+left click). `S=0`, HUD, target/placement and paused keep
  press-at-BEGIN.
- Comments in `runtime/ipc/direct_touch_event.h`,
  `runtime/platform/android/controls/direct_touch.h/.c` and the EN/RU
  direct-touch hints now describe smart order and the selection box.
- T0 `direct_touch`, `controls_mode`, `native_press_barrier`,
  `gesture_lifetime`, `controls_idle` pass; NDK `clang -fsyntax-only`
  release/debug `hle_direct_touch.c` = 0/0; `git diff --check` clean.
- No build/install was run for this source step. The device runs rev68
  (installed 21:52); this smart-order change is in that build, but the T2
  matrix in `docs/research/SMART_ORDER_LONG_TAP_PLAN_2026-11-26.md` §6.4 is
  still pending.

## 2026-11-26: long-tap attack-move restored (source only)

- The rev 2 smart-order change made long tap on terrain a right-click smart
  move and removed the legacy attack-move. The user reported that
  attack-move on terrain with controlled units was broken.
- `dt_smart_order_at()` now takes an `attack_move` flag. Tap passes 0:
  terrain is still a right-click smart move. Long tap passes 1: a unit or
  decoration is a smart order, terrain sends `A` down/up plus the native
  left click (the previous attack-move). The `dt_smart_order_ready()` gate
  and the native right-click fallback for smart orders are unchanged.
- EN/RU hints now distinguish long press on a unit/decoration (smart order)
  from long press on the ground (attack-move).
- T0 `direct_touch`, release/debug `-fsyntax-only`, and `git diff --check`
  pass. No build/install was run; T2 still needs an explicit command.

## 2026-11-26: stable first-tap attack-move confirmation (source only)

- Last-run trace root cause: the long tap on terrain emitted
  `field-attack-move` at t=283857490, then `A` down/up and the native left
  click in the same guest pump. The guest processed `msg=0100/0101` and
  `msg=0201/0202`, but the attack target mode is built on the next update,
  so that click was not a target confirmation. The next contact at
  t=283858446 was handled by the live target path
  (`field-target-hover` -> `field-target-click-deferred`), which is why the
  second tap confirmed the order.
- `dt_smart_order_at(attack_move=1)` now sends `A` down/up and queues the
  confirming left click through the existing field-click slot
  (`dt_field_attack_move_click_queue`). `dt_field_click_tick` waits for
  `DT_FIELD_TARGET_CLICK_DELAY_MS=33`, revalidates
  `dt_field_target_tap_ready()` on every pump, retries until
  `DT_FIELD_CLICK_TIMEOUT_MS`, and cancels on a new gesture or pause menu.
  No new IPC events, timers, ABI fields or constants.
- T0 `direct_touch`, `controls_mode`, `native_press_barrier`,
  `gesture_lifetime`, `controls_idle` pass; NDK release/debug
  `-fsyntax-only -Wall -Wextra -Werror` for `hle_direct_touch.c` = 0/0;
  `git diff --check` clean. No build/install was run; T2 needs the explicit
  command and should check `field-attack-move` followed by
  `field-attack-move-click` on the first long tap.

## 2026-11-26: first world tap — unit-selection gateway fix (source only)

- The same last-run trace exposes a second first-tap gap before the
  attack-move contact.  The first field contact at t=283852395 picked unit
  `6387DF14` (`intent=1`, `tap-select-unit-native`), but the map audit
  sampled `primary=00000000 has=0`; the second contact at t=283853403
  selected it (`primary=6387DF14 has=1`).  The first native click was
  dispatched in the same pump as `0200/0201/0202` through
  `hle_android_mouse_press_replay()`.
- Root cause: `dt_tap()`'s select-unit path used the synchronous modal
  replay helper.  The guest refreshes its hot control only in the idle
  `GetCursorPos` pass after the move, so the replay's `DOWN` was hit-tested
  against the previous hot control and swallowed.  The HLE pick was
  correct; the native selection click was stale.  This is the documented
  first-world-tap class, not a map-load or pick failure.
- Fix: the select-unit intent and the deferred field-select fallback now
  call `dt_native_press_deferred()`; `dt_native_left_click()` is reserved
  for a target/placement confirmation in a modal loop, where the guest may
  never call `GetCursorPos`.  The T0 `native_press_barrier` gate checks the
  select-unit case.
- T0 `direct_touch`, `controls_mode`, `native_press_barrier`,
  `gesture_lifetime`, `controls_idle` pass; NDK release/debug
  `-fsyntax-only -Wall -Wextra -Werror` for `hle_direct_touch.c` = 0/0;
  `git diff --check` clean.  The attack-move queue fix in the previous
  section was also source-only at that point; rev68 contains neither fix.
  Both fixes are now in the rev69 build/install below.

## rev69 build/install (explicit user command, 2026-11-26 22:27)

Command: `bash tools/android/build_apk.sh --variant debug --install --serial emulator-5554`
(log: `/data/user/0/com.termux/files/usr/tmp/opencode/build_debug_all_20261126_2218.log`).
distcc stayed at its default auto-worker mode.

- Build exit `0`, Gradle `BUILD SUCCESSFUL in 1m 13s`, streamed install
  `Success`.
- Build log line 592 recompiled
  `CMakeFiles/wc3_hle_android.dir/runtime/hle/android/hle_direct_touch.c.o`;
  the linked SO is `libwc3gameactivity.so` 160468640 bytes.
- APK: `build/android_app/wc3-debug.apk`, 178872573 bytes, SHA-256
  `f8c3cf3e820f3921e7575175f1fc4d14f32a53f04e568fda0f3578857bd91287`.
  It contains the `field-attack-move-click` and
  `field-attack-move-click-timeout` trace strings, so the attack-move
  queue fix is compiled in; the unit-selection path uses
  `dt_native_press_deferred()`.
- Installed `wc3.recomp.debug`, versionCode `2`, versionName `0.1.1-debug`,
  `lastUpdateTime=2026-11-26 22:27:37`; installed `base.apk` size
  `178872573` and SHA-256 match the local APK byte-for-byte.
- T0 before build: `direct_touch`, `controls_mode`, `native_press_barrier`,
  `gesture_lifetime`, `controls_idle` pass; NDK release/debug
  `clang -fsyntax-only -Wall -Wextra -Werror` for `hle_direct_touch.c` =
  0/0; `git diff --check` clean.

Device T2 pending.  Expected on the first field tap: `tap-select-unit-native`
followed by `probe barrier-defer`/`barrier-flush` and `map-audit ... has=1`
without a second tap.  Expected on the first terrain long tap:
`field-attack-move` followed by `field-attack-move-click` after the target
mode is live.  Pause taps must still show `barrier-defer msg=0201` then
`0202`, with no `batch-context-change`/`native-down-cancel`.

## 2026-11-26: rev69 T2 — first world tap still idle; fallback bypassed idle pass

- rev69 device log (`/data/user/0/com.termux/files/usr/tmp/opencode/first_tap_world_rev69/`):
  the first field tap at `t=286622251` picked unit `63F9E224`
  (`tap-select-unit-native`), but `map-audit#1` at `t=286624582/286624628`
  still reported `primary=00000000 has=0`.  The second tap at
  `t=286624628` selected the same unit; `map-audit#2` at `t=286625125`
  reported `primary=63F9E224 has=1 q=3/3`.
- The first tap's trace armed `0201` (`barrier-defer`), dispatched `0200`
  (`native-down-move`), then forced `0201` from
  `native-up-fallback a=2` and posted `0202` from `native-up-deferred` at
  the same `t=286622251`.  The idle
  `GetCursorPos` pass that refreshes hot control arrived later, at
  `t=286622260`; the fallback had already hit-tested the stale hot control.
  The second tap worked because the first tap's move/idle pass had left the
  hot control on the unit.
- Root cause: the select-unit path entered `dt_native_press_deferred()`, but
  that state was designed for a late-resolved native UI release and its
  message-pump fallback (`g_native_up_wait++ > 0`) forced the DOWN on the
  second service call in the same pump, bypassing `move_ready`.
- Fix: `dt_native_press_deferred(ev, modal_fallback)`.  Field selection and
  the field deferred click pass `0` and wait for the normal idle
  `GetCursorPos` flush; only the two native-UI release paths (paused field
  release and late native UI release) pass `1` and keep the modal fallback.
  Pause-menu entry now clears an armed field press before it can be forced
  into the menu or block the next tap; the service detects the pause edge
  before the deferred-press branch, otherwise that branch would return first
  and the clear would never run.
- T0 `native_press_barrier` now checks the select-unit call uses
  `dt_native_press_deferred(ev, 0)`, the late native release keeps `ev, 1`,
  and the fallback is gated by `g_native_up_modal`.  `direct_touch`,
  `controls_mode`, `gesture_lifetime`, `controls_idle` pass; NDK
  release/debug `clang -fsyntax-only -Wall -Wextra -Werror` for
  `hle_direct_touch.c` = 0/0; `git diff --check` clean.  No build/install was
  run; rev70 must show `barrier-flush` after the idle pass and no
  `native-up-fallback` for the first field tap, with `map-audit ... has=1`
  on that first contact.

## rev70 debug build/install (explicit user command, 2026-11-26 23:44)

- `tools/android/build_apk.sh --variant debug --install --serial emulator-5554`
  used the live adb device and distcc (`ideapad` + `pc`, 24 jobs).
  Native build and Gradle finished with `BUILD SUCCESSFUL in 1m 7s`;
  streamed install returned `Success`.  This is a rebuild of the same
  working tree; the first rev70 attempt at 23:32 was superseded by this
  artifact.
- APK: `build/android_app/wc3-debug.apk`, `178875509` bytes,
  SHA-256 `ce53d7276d1b0c520d478b3478f4edbbc88c43d26f126d4c5fb7b91cf7398d4e`.
  Installed `base.apk` reports the same size and SHA-256, so the device
  package matches the built artifact.
- Package `wc3.recomp.debug`: versionCode `2`, versionName `0.1.1-debug`,
  `lastUpdateTime=2026-11-26 23:44:37`.  This build contains the deferred
  field-press modal split and the pause-entry ordering fix.
- Device T2 remains manual.  First field tap must show `barrier-defer` then
  `barrier-flush` after the idle pass, no `native-up-fallback`, and
  `map-audit ... has=1` on that first contact.  Pause-menu first tap and the
  smart-order long-tap matrix still need manual confirmation.

## 2026-11-26: attack-move подтверждается по живому target mode без 33 мс

- T2 предыдущей debug-сборки (fix rev 4, APK `lastUpdateTime=2026-11-26
  22:27:37`) подтвердил механизм: 52 `field-attack-move`, 52
  `field-attack-move-click`, 0 `field-attack-move-click-timeout`,
  release→click 32–45 мс (среднее 36.1 мс). Все 52 клика пришлись на уже
  живой target mode, но каждый ждал `DT_FIELD_TARGET_CLICK_DELAY_MS=33`.
- `dt_field_attack_move_click_queue()` теперь ставит
  `g_field_click_ready_ms=0`. `dt_field_click_tick()` по-прежнему
  проверяет `dt_field_target_tap_ready()` (живой не-select target mode,
  точка в поле, нет pause), поэтому клик уходит на первом service pass
  после guest update, создавшего режим, без слепой задержки. Release
  pump уже содержит `A` down/up и hover, так что режим создаётся с
  курсором в точке.
- Target/placement сохраняет 33 мс: его режим был живым до жеста и
  должен перестроить preview из hover. Таймаут 1000 мс, отмена новым
  жестом/pause и повторная проверка режима не менялись. Новых состояний,
  IPC, таймеров, ABI и констант нет; verify/retry-петля не добавлялась.
- T0 `direct_touch`, `controls_mode`, `native_press_barrier`,
  `gesture_lifetime`, `controls_idle` — зелёные; NDK
  `clang -fsyntax-only -Wall -Wextra -Werror` для
  `hle_direct_touch.c` release/debug = 0/0; `git diff --check` чистый.
  APK не собирался и не устанавливался.
- T2 после явной команды: ожидается `field-attack-move` →
  `field-attack-move-click` без timeout, задержка release→click около
  одного guest update (цель — меньше 36.1 мс), второй tap не нужен;
  `field-target-click-deferred` для target/placement остаётся с 33 мс.

## 2026-11-26: проверка оптимальности и неизбыточности

- Убран повторный `dt_pause_menu_sync()` из `dt_sync_idle_context()`.
  Верхняя проверка `wc3_hle_direct_touch_service()` уже синхронизирует
  pause-entry до deferred-ветки; idle-путь теперь только публикует route.
  На каждом idle service pass стало на одно чтение `CGameUI/CEscMenu`
  меньше; новых guest-вызовов, dispatch, состояний и таймеров нет.
- Удалён устаревший комментарий в service, оставшийся после переноса
  pause-sync наверх. Обновлён контракт raw pointer phases: они несут
  `NATIVE` или `MINIMAP`, а не всегда `NATIVE`; T0-комментарий уточняет,
  что idle-gate относится к обычному press, а modal fallback — отдельная
  политика.
- Два abort-блока в `dt_pause_menu_enter_state()` не дублируются:
  первый снимает armed deferred press (`g_native_up_pending`), второй
  отпускает уже опубликованный field drag (`g_field_defer_down_posted`).
  Это взаимоисключающие состояния; объединение скрыло бы разную очистку.
  `g_native_up_modal` оставлен отдельным битом: инвариант `modal => pending`
  проверяется сбросом только вместе с pending, а смешивание с `wait`
  связало бы таймаут и политику fallback.
- T0 `native_press_barrier`, `direct_touch`, `controls_mode`,
  `gesture_lifetime`, `controls_idle` — зелёные; `git diff --check`
  чистый.  Установленный rev70 APK (`ce53d727...`) собран до этой
  уборки и отличается только удалённым повторным чтением; для переноса
  уборки в устройство нужна отдельная явная команда сборки.

## 2026-11-27: событийная отмена отложенного field-клика

- Удалены `DT_FIELD_CLICK_TIMEOUT_MS=1000`,
  `DT_FIELD_NATIVE_CLICK_TIMEOUT_MS=200` и `g_field_click_ms`. Число
  миллисекунд не выводилось из состояния игры и оставляло клик
  заряженным на чужой target mode; оба предела убраны из слота
  `g_field_click_*`.
- Введён монотонный `g_input_epoch` (`hle_android_input_epoch()` /
  `hle_android_input_epoch_bump()`). Слот хранит
  `g_field_click_input_epoch` и отменяется на любом новом
  пользовательском вводе. Bump и push нового ввода
  (`u_msg_post_input`) и проверка epoch с постановкой DOWN/UP
  (`hle_android_mouse_click_if_epoch`) идут под одним `g_uq_lock`,
  поэтому клик либо уходит раньше нового ввода, либо не уходит вовсе.
- Bump-точки: контакт (`POINTER_DOWN`, `TAP`, `FIELD_HOVER`,
  `FIELD_SELECT_BEGIN`, `FIELD_SELECT_DRAG_BEGIN`, `LONG_PRESS`,
  `FOLLOW_START`, `GROUP_SIGNAL`), key-down не-модификатора только при
  переходе up→down (битовая карта против auto-repeat) и реальный
  button-down. Auto-repeat, key-up, move, wheel, camera begin и
  synthetic replay epoch не меняют. `KEY_SIGNAL` не bump-ит: у него
  нет producer'а и один kind покрывает press/release; при возврате
  пути направление нужно передать явно.
- `A` в `dt_smart_order_at()` переведён на
  `hle_android_key_event_synthetic()`; epoch снимается в начале
  функции, до pick и `A`, поэтому собственный `A` не отменяет свой
  клик, а новый ввод после снимка — отменяет. Подтверждающий клик
  ставится через `hle_android_mouse_click_if_epoch()`.
- Native tick ждёт пустую direct-touch очередь
  (`field-target-click-wait-queue`), чтобы уже стоящий в ней контакт
  разрешился раньше клика. Map-load tap эту проверку не использует: он
  replay-ится раньше нового жеста.
- Map-load/fade-in tap отменяется по `dt_pause_menu_active()` и
  `dt_field_load_context_alive()` (живые UI/world/select-mode), а не по
  несуществующей ветке `MENU` в `dt_ready_route()`; медленная загрузка
  больше не теряет первый tap и не replay-ит его после выхода в меню.
- `DT_FIELD_TARGET_CLICK_DELAY_MS=33` остаётся только у
  target/placement как один кадр на перестройку preview; это не
  retry-окно и не предел жизни. Отмена по epoch работает и в эти 33 мс.
- T0 `direct_touch`, `native_press_barrier`, `controls_mode`,
  `gesture_lifetime`, `controls_idle`, `hle_abi`, `input_adapter` —
  зелёные; NDK `-fsyntax-only` для `hle_direct_touch.c`
  (release/debug, `-Wall -Wextra -Werror`) и `hle_android.c`
  (release/debug, флаги сборки) = 0/0; `git diff --check` чистый.
  APK не собирался и не устанавливался; T2 ждёт явной команды.
  Известные несвязанные падения: `release_hygiene` на
  `guest_window_android.c`, `platform_contract` на `dxvk_trace.cpp`,
  `thread_audit` — `unavailable`.

## 2026-11-27: rev71 debug build/install (явная команда)

- `tools/android/build_apk.sh --variant debug --install --serial emulator-5554`.
  `ideapad` не разрешился по MAC и был пропущен; использованы `pc` и
  локальные 8 воркеров (12 jobs).  distcc не отключался.
- Native `libwc3gameactivity.so` — `160491456` байт; Gradle
  `BUILD SUCCESSFUL in 1m 20s`; streamed install — `Success`.
- APK: `build/android_app/wc3-debug.apk`, `178895389` байт,
  SHA-256 `a9b96264db120d2660164139cadff74491d0f0fd9d779402f876f7319efa62d2`.
  Установленный `base.apk` совпадает по размеру и SHA-256.
- Пакет `wc3.recomp.debug`: versionCode `2`, versionName `0.1.1-debug`,
  `lastUpdateTime=2026-11-27 00:30:42`.
- Сборка включает текущие изменения `hle_android.c`, `hle_direct_touch.c`,
  `native_input_gateway.h`, `hle_input_state.h`, controls и T0, а также
  удалённый повторный pause-sync из idle-пути.  T2 остаётся ручным;
  коммит не делался.
