# Local patches to the vendored Winlator input engine

Base commit: `208371f89e2e2d218263308f26ee02bbf149a70e`

The detached host tests found three boundary defects.  One required a vendored
patch; the duplicate-action defect is handled at the adapter sink, and the
HOVER digit-to-digit expectation was corrected rather than inventing a binding.
Keep this list in sync with `test_gestures.c`; remove a patch only after the
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
