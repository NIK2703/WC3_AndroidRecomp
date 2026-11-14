# Local patches to the vendored Winlator input engine

Base commit: `208371f89e2e2d218263308f26ee02bbf149a70e`

The detached host tests found boundary defects.  Four required vendored
patches; the duplicate-action defect is handled at the adapter sink, and the
HOVER digit-to-digit expectation was corrected rather than inventing a binding.
Keep this list in sync with the host gesture tests (`test_gestures.c` in the
reference tree, `tests/gesture_lifetime_test.c`, and
`tests/gesture_button_swipe_test.c` here); remove a patch only after the
corresponding test is changed deliberately.

## 1. `gesture/handler.c`: do not leak a first-finger tap after a second-finger tap

`cleanup_second_finger_up()` restored a held main-finger action, but when the
main finger was merely `GESTURE_STATE_TAP_WAITING` it left the state alone.  Its
later UP then scheduled a second tap, so a second-finger tap produced two left
clicks and left the finger state active.

The patch sets the main finger to `GESTURE_STATE_IDLE` and clears its tap state
when the second finger consumed the touch and the main finger has no pending
resume action.  Host test: `second finger presses left button once`,
`second finger releases left button once`, and
`second finger double tap presses right once`.

The adapter boundary filter in `wc3_controls.c` additionally enforces one live
press per key/button.  It is not a substitute for the engine fix: the engine
still has to clear the main finger so later gestures do not inherit stale
state.

## 2. `gesture/handler.c`: retire a lifted main finger when another pointer resolves its double-tap window

A main finger stays active after its UP while it owns the double-tap window.
When another pointer landed outside the tap radius, `check_confirm_dt_waiting()`
cancelled that window but left the lifted main finger active.  The stale
`gesture_main_ptr_id` then made every later pointer with a different id a
second finger: no `ACT_POINTER_MOVE`, no first-finger tap, until a two-finger
touch reused the stale id.

The patch cancels the window and retires the lifted main before role assignment
when a different pointer lands outside the radius.  For a confirming tap, the
main is retired after the confirming pointer goes up.  The
`double_tap_original_id_set` check keeps a physically held main finger intact.

Host test: `tests/gesture_lifetime_test.c`, T0 gate `gesture_lifetime`.  It
holds an overlay button with pointer 0, taps the field with pointer 1, releases
pointer 0, then taps pointer 0 before the double-tap timeout.  Before the patch
the second tap produced no `MOVE_ABS` and left the old main active; after the
patch it produces `MOVE_ABS` and normal single taps continue to work.  The same
test covers the near-confirm path: after the confirming pointer goes up, the
lifted owner must not stay active.

## 3. `gesture/branch.c` and `core/types/touch_config.h`: direct mode skips the scroll-only double-tap wait

The product gesture switch can disable double-tap actions by clearing their
packages to `count = 0`.  `tap_path_normal()` still entered a double-tap window
for a profile with scroll bindings, even when both double-tap packages were
empty, because the scroll-only path exists to let a second tap activate scroll
mode (`caps_has_scroll_bindings`).  The single-tap action already fired before
that wait, but the engine still held the finger in `DOUBLE_TAP_WAITING`, and a
second tap could confirm a zero-action double tap and enable scroll drag.

`TouchProcessorConfig.direct_gesture_mode` is set by the product adapter after
`profile_to_config()`; when true, `tap_path_normal()` skips only the
scroll-only wait.  Scroll bindings remain active, so first-finger drag and
two-finger rotation still work.  The wait is not disabled globally: authored
profiles without the flag keep the old tap-tap scroll behavior.

Host test: `tests/gesture_mode_test.c`, T0 gate `gesture_mode`.  It checks that
the authored profile enters the wait, direct mode leaves all four double-tap
slots at `count = 0`, clears `caps_has_double_tap`, and a single tap in direct
mode leaves `dt_waiting_count == 0` even though the profile has scroll bindings.

## 4. `gesture/handler.c` and `element/button.c`: count the whole swipe path for a button gesture

HOVER and TRACK buttons move through a slide-over state machine that forwards
`MOVE` to `element_button_move()` only while the pointer remains inside the
button.  On the first outside sample `handle_btn_move_transition()` released
the element and cleared its gesture flags.  A swipe that started near a button
edge and left the rect before `gestureThreshold` was reached was therefore
cancelled; the MOVE that crossed the threshold outside was never delivered, so
no gesture action or gesture haptic fired.  With the button kept engaged, the
later UP could turn the deferred primary press into a tap, producing the wrong
action and haptic.

`handle_engaged_element_move()` now keeps a button with
`TOUCH_BIT_CACHED_HAS_GESTURE` engaged after it is left and feeds outside MOVE
samples directly to `element_button_move()`.  The gesture distance is measured
from the original `down_x/down_y`, so the outside part of the path counts.
`element_button_up()` also evaluates the final UP point, covering coalesced
DOWN/UP events with no MOVE.  In HOVER, `button_up_handle_deferred()` cancels
the deferred tap when the pointer lifts outside before the threshold, instead
of clicking a button the finger already left.

Host test: `tests/gesture_button_swipe_test.c`, T0 gate
`gesture_button_swipe`.  It starts on the left edge of a HOVER button, leaves
the rect below the threshold, crosses the threshold outside, and verifies the
gesture press plus the button gesture haptic.  It then leaves below the
threshold and lifts outside, verifying that no tap press or gesture haptic is
emitted.  It also covers a coalesced fast swipe whose only outside sample is
the UP point.  Before the patch the outside threshold crossing produced no
gesture, and the outside UP could leak a deferred primary tap.
