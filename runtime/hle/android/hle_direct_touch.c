/*
 * hle_direct_touch.c - resolve direct-touch gestures on the guest thread.
 *
 * The overlay layer has already decided that a touch belongs to the
 * battlefield.  This file turns the pointer-free event into the same native
 * calls the game would make for a left click (select) or a right click
 * (smart order).  It runs at the PeekMessageA/GetMessageA boundary, so the
 * nested guest calls are equivalent to the ones the window procedure makes
 * for a real mouse message.
 *
 * The semantic surface is deliberately small:
 *   - tap: select a unit, or smart-order when the selection is controlled;
 *   - long press: native left-button press-and-hold from the deadline to the
 *     finger release; with a controlled selection a stationary release is a
 *     smart order on a unit/decoration or attack-move on terrain, and a drag
 *     selects a box; without it a stationary release clicks one unit;
 *   - portrait: sticky-finger camera follow;
 *   - minimap: native pointer phases clamped to its frame;
 *   - selected-unit grid: tap selects a unit, drag switches the subgroup
 *     under the finger without collapsing the squad;
 *   - inventory: native click pairs for use/swap/move/drop;
 *   - command card: select on finger-down, targeted drag and autocast.
 * Native UI paths post the same Win32 mouse messages a real mouse
 * produces; the overlay touch engine is not involved.
 *
 * The guest pointers below are addresses from the fixed 1.27.1.7085 image
 * base.  They are pinned in docs/research/DIRECT_TOUCH_CONTROL_ANALYSIS.md;
 * do not replace them with guessed field offsets.
 */
#include "hle_direct_touch.h"
#include "hle_direct_touch_intent.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "guest_host.h"
#include "guest_memory.h"
#include "hle_android.h"
#include "wc3_debug.h"

#define DT_UI_GLOBAL       0x6FD6A8B8u
#define DT_WORLD_GLOBAL    0x6FD47818u
/* game.dll stores WM_ACTIVATEAPP's wParam here.  The main loop gates its
 * foreground work on this word, so it is the exact "is the app active" bit a
 * synthetic first click must establish before its button messages. */
#define DT_APP_ACTIVE_GLOBAL 0x6FCD5E74u
#define DT_PLAYER_MANAGER  0x6FD687A8u

#define DT_UI_WORLD_FRAME  0x000003BCu
#define DT_UI_CLICK_GATE   0x000001ACu
#define DT_UI_ACTIVE_MODE  0x000001B4u
#define DT_UI_MODE_COUNT   0x000001BCu
#define DT_UI_MODE_ARRAY   0x000001C0u
#define DT_UI_TARGET_MODE  0x00000210u
#define DT_UI_SELECT_MODE  0x00000214u
#define DT_UI_DRAG_MODE    0x00000218u
#define DT_UI_ALT_MODE     0x00000220u
#define DT_UI_CMD_MODE_228 0x00000228u
#define DT_UI_CMD_MODE_238 0x00000238u
#define DT_UI_CMD_MODE_23C 0x0000023Cu
#define DT_UI_CMD_MODE_240 0x00000240u
#define DT_UI_CAMERA_MODE  0x0000021Cu
#define DT_UI_CAMERA       0x00000254u
#define DT_UI_MANAGER      0x0000024Cu
/* Pause menu: CGameUI+0x228 -> CEscMenu; main panel at CEscMenu+0x18;
 * pause flag is dword [main panel+0x170].  CEscMenu::Show is the only writer
 * of that flag.  This is distinct from DT_FN_PAUSED: a paused game cannot
 * confirm a target, and an open pause menu must always own native clicks. */
#define DT_UI_ESC_MENU     0x00000228u
#define DT_ESC_MENU_PANEL  0x00000018u
#define DT_ESC_MENU_PAUSED 0x00000170u
#define DT_UI_INFO_BAR     0x000003C4u
#define DT_UI_PORTRAIT     0x000003F4u
#define DT_UI_MINIMAP      0x000003C0u
#define DT_UI_COMMAND_BAR  0x000003C8u
/* CGameUI+0x41C is the full minimap panel frame: it has the map image at
 * +0xB4 and the Minimap* buttons as children.  Its rect is the authoritative
 * input area, including the button row above the map image. */
#define DT_UI_MINIMAP_CONTAINER 0x0000041Cu
/* CFrame's child list is a linked list of nodes at [frame+0x40].  A node's
 * +0x8 is the next node, +0xC is the child CFrame pointer, and +0x10 is an
 * edge mask.  The +0xC field is NOT a frame ID: rev 54 dumped values such as
 * 0x4193013C, which is exactly CMinimap+0xB4.  Native code confirms this in
 * 0x6F111070: it walks [frame+0x40], follows [node+0xC], and tests
 * [node+0x10]. */
#define DT_FRAME_CHILD_LIST  0x00000040u
#define DT_FRAME_NODE_NEXT   0x00000008u
#define DT_FRAME_NODE_FRAME  0x0000000Cu
#define DT_FRAME_NODE_MASK   0x00000010u
#define DT_PORTRAIT_UNIT   0x00000238u
#define DT_PORTRAIT_FRAME  0x000000B4u
#define DT_PORTRAIT_OFF    0x00000260u
#define DT_MINIMAP_FRAME   0x000000B4u
/* CGameUI resolves every HUD control name once, in the initializer at
 * 0x6F367B43, and stores the results in a fixed table:
 *   +0x42C QuickSave   +0x430 QuickLoad   +0x434 QuickHelp
 *   +0x438 QuickOptions +0x43C QuickQuit  +0x440 MinimapSignal
 *   +0x444 MinimapTerrain +0x448 MinimapColors +0x44C MinimapCreeps
 *   +0x450 FormationToggle
 * 0x6F367C08 is the store of the "MinimapSignal" lookup into +0x440.
 * CMinimap+0x484 is a different ID ("UnitCenter", stored by 0x6F35E01A),
 * not the signal button. */
#define DT_UI_MINIMAP_SIGNAL 0x00000440u
/* Native 0x6F36D890 constructs the five minimap buttons as CFrames and
 * stores them directly in CGameUI: +0x408 MinimapSignal, +0x40C Terrain,
 * +0x410 Colors, +0x414 Creeps, +0x418 FormationToggle.  The signal button
 * gets MiniMapSignalButtonEnabled/Pushed/Disabled at 0x6F36D9A1..D9EA through
 * [CGameUI+0x408].  The buttons hang off the +0x3D4 container under +0x428,
 * not off the minimap image or the +0x41C panel frame, which is why a tree
 * search keyed on the control ID never found them. */
#define DT_UI_SIGNAL_BUTTON 0x00000408u
#define DT_MINIMAP_MATRIX  0x00000750u
#define DT_MINIMAP_MATRIX_SIZE 36u
#define DT_FRAME_RECT      0x00000044u
#define DT_FRAME_VISIBLE   0x00000054u

/* Selected-unit panel: CInfoBar+0x13C -> CInfoPanelGroup; the active grid is
 * group+0x138 (6x2, 8x3 or 10x4 depending on the selection size). */
#define DT_INFO_GROUP        0x0000013Cu
#define DT_GROUP_ACTIVE_GRID 0x00000138u
#define DT_GROUP_VISIBLE     0x00000090u
#define DT_GRID_ROWS         0x00000124u
#define DT_GRID_COLS         0x00000128u
#define DT_GRID_ROW_ARRAY    0x00000154u
#define DT_GRID_VISIBLE      0x00000090u
#define DT_GRID_ROW_STRIDE   0x00000010u
#define DT_GRID_ROW_BUTTONS  0x00000008u
#define DT_GRID_BUTTON_UNIT  0x00000124u
#define DT_GRID_BUTTON_VISIBLE 0x00000090u
/* CGridPanel stores rows at +0x124 and columns at +0x128: 2x6 for the
 * original panel, 3x8 for grid24 and 4x10 for grid40.  Do not swap these
 * limits: a cols limit of 4 would reject every grid, including 6x2. */
#define DT_GRID_MAX_ROWS     4u
#define DT_GRID_MAX_COLS     10u

/* Inventory bar: CInfoBar+0x148 -> CInventoryBar.  Each slot entry is eight
 * bytes: the item holder at +0 and the CCommandButton* at +4.  The button's
 * command data lives at +0x190; the native 0x30065 handler rejects a button
 * without it.  The data keeps its item holder at +0x5C0. */
#define DT_INFO_INVENTORY    0x00000148u
#define DT_INV_SIZE          0x00000128u
#define DT_INV_CAP           0x0000012Cu
#define DT_INV_ARRAY         0x00000130u
#define DT_INV_ENTRY_ITEM    0x00000000u
#define DT_INV_ENTRY_BUTTON  0x00000004u
#define DT_INV_BUTTON_DATA   0x00000190u
#define DT_INV_DATA_ITEM     0x000005C0u
#define DT_INV_MAX_SLOTS     6u
/* Mirror of controls.c direct_touch_field_contains().  A release inside this
 * rectangle is a battlefield point; everything outside it is native HUD and
 * must not turn an inventory drag into a drop.  Keep the two in sync.
 * The bottom is the console's top edge: the live minimap frame starts at
 * 0.8550 and the command card at 0.8690, so the world extends well below
 * 0.72.  The minimap, portrait and beacon have their own rect gates that run
 * before this rectangle, so the overlap is safe. */
#define DT_INV_FIELD_LEFT    0.03f
#define DT_INV_FIELD_RIGHT   0.97f
#define DT_INV_FIELD_TOP     0.10f
#define DT_INV_FIELD_BOTTOM  0.85f
/* A tap that drifts out of its slot still uses the item; a real drag to the
 * field drops it.  The slop matches the command/field tap slop. */
#define DT_INV_TAP_SLOP      0.02f

/* Command card: CGameUI+0x3C8 -> CCommandBar.  The bar is a CGridPanel with
 * 3 rows x 4 columns at +0x124/+0x128; +0x154 is the row array, each row is
 * 0x10 bytes and row+8 is the four CCommandButton*.  A command button
 * registers 0x30064 with CGameUI.  The native down handler only sets the
 * pressed state; the up handler sends the event with the old button mask, so
 * a tap must be a full native down/up pair.  CCommandButton+0x140 is refreshed
 * by 0x6F3AA387 from the entries: 0x10 when a left entry exists, 0x40 when a
 * right entry exists.  The right entry is the autocast toggle. */
#define DT_CMD_ROWS          0x00000124u
#define DT_CMD_COLS          0x00000128u
#define DT_CMD_ROW_ARRAY     0x00000154u
#define DT_CMD_ROW_STRIDE    0x00000010u
#define DT_CMD_ROW_BUTTONS   0x00000008u
#define DT_CMD_BUTTON_DATA   0x00000190u
#define DT_CMD_BUTTON_VISIBLE 0x00000054u
#define DT_CMD_BUTTON_SHOWN  0x00000094u
#define DT_CMD_DATA_TYPE     0x00000008u
#define DT_CMD_DATA_FLAGS    0x00000010u
#define DT_CMD_ENTRY_LEFT    1u
#define DT_CMD_ENTRY_RIGHT   4u
/* Target/area command flags.  A left entry with any of these bits needs a
 * world target.  Diagnostic only: the live target mode decides a release. */
#define DT_CMD_TARGET_FLAGS  0x00000281u
#define DT_CMD_MAX_ROWS      4u
#define DT_CMD_MAX_COLS      4u
#define DT_CMD_LONG_PRESS_MS 450u
#define DT_CMD_TAP_SLOP      0.02f
/* A drag release queues the confirming field click.  The game switches modes
 * on its own update, so the click is posted from a later pump tick.  Drop it
 * if the mode never becomes live. */
#define DT_CMD_TARGET_TIMEOUT_MS 400u
/* A live target/placement click is held for one guest frame.  The release
 * hover only moves the native cursor; the mode builds its placement preview
 * on the next frame, so a click posted in the same pump resolves against the
 * previous preview.  One 30 Hz frame is the smallest bound that covers both
 * the common 60 Hz and 30 Hz update rates. */
#define DT_FIELD_TARGET_CLICK_DELAY_MS 33u
#define DT_FIELD_TAP_SLOP 0.02f
/* A finger aiming at the rightmost icon often lands on the card's right
 * border.  The margin is in surface units and covers the gap between the
 * last button and the card edge (0.9838..0.9908 on a 2400x1080 surface);
 * the click still targets the centre of the button, so the command
 * semantics do not change.  Do not add a top margin: the old 0.08 local
 * margin extended the top row above the card and captured world points. */
#define DT_CMD_HIT_MARGIN_RIGHT 0.017f

#define DT_SELECTION_PRIMARY 0x000001E4u

#define DT_WORLD_SELECT_GATE 0x000001A4u
#define DT_WORLD_ALT_GATE    0x000001A8u

#define DT_FN_IN_GAME          0x6F36E1F0u
#define DT_FN_MODE_ACTIVE      0x6F36E330u
#define DT_FN_EVENT_GATE       0x6F072190u
#define DT_FN_PAUSED           0x6F32C110u
#define DT_FN_INPUT_COORDS     0x6F8B38A0u
#define DT_FN_PICK             0x6F3840E0u
#define DT_FN_PLAYER           0x6F1E0EA0u
#define DT_FN_VALIDATE_OBJECT  0x6F21B730u
/* 0x6F1D73B0 is the CUnit counterpart of 0x6F21B730.  It validates a CUnit
 * (class 0x2B773375), takes its own reference and writes it to the guest
 * dword at ecx.  The native unit-at-point helper uses it on pick+0x0C. */
#define DT_FN_VALIDATE_UNIT    0x6F1D73B0u
/* Debug-only counterparts of the validation chain inside 0x6F21B730:
 * 0x6F6DA7D0 returns the expected CGhostImage class id, the vtable slot at
 * +0x1C returns the object's class id, and 0x6F04CDD0 compares the two
 * (walking the class registry for subclasses).  0x6F054530 resolves a
 * handle/type pair. */
#define DT_FN_CLASS_GHOST_ID   0x6F6DA7D0u
#define DT_FN_CLASS_IS         0x6F04CDD0u
#define DT_FN_HANDLE_LOOKUP    0x6F054530u
#define DT_FN_VALIDATE_WORLD_OBJECT 0x6F1F2A60u
#define DT_CLASS_GHOST_ID      0x2B776768u
#define DT_FN_SELECTION_COUNT  0x6F290270u
#define DT_FN_SELECTION_PRIMARY_UNIT 0x6F290240u
/* 0x6F28FFE0 returns CSelectionWar3+0x1E0, the refcounted primary selected
 * CUnit.  Neither native order path reads it; it is only a diagnostic
 * counterpart to the active-subgroup primary below. */
#define DT_FN_SELECTION_PRIMARY_SELECTED 0x6F28FFE0u
/* 0x6F290570 is the active subgroup's vector accessor:
 * (group, index) -> CUnit* or 0.  Used by the debug selection dump. */
#define DT_FN_GROUP_AT         0x6F290570u
/* 0x6F291240 is CSelectionWar3::Realize (mirror update), not a clear.
 * dt_clear_selection() predates this analysis; do not copy the name into new
 * paths.  The grid path uses the explicit DT_FN_SELECTION_REALIZE alias. */
#define DT_FN_CLEAR_SELECTION  0x6F291240u
/* 0x6F66EC60 -> 0x6F66EC70(CUnit*, local_player_id).  It reads CUnit+0x58,
 * CUnit+0x5C and the CUnit vtable, not a CSelectionWar3*.  The native ground
 * order path calls it with the primary selected CUnit, never with the
 * selection object. */
#define DT_FN_UNIT_CONTROLLABLE 0x6F66EC60u
#define DT_FN_GHOST_INIT       0x6F37C550u
#define DT_FN_CLICK_DISPATCH   0x6F392FA0u
#define DT_FN_GROUND_ORDER     0x6F394680u
#define DT_FN_HANDLE_TO_UNIT   0x6F1EEF90u
#define DT_FN_UNIT_POSITION    0x6F058900u
#define DT_FN_CAMERA_PAN       0x6F361B90u
#define DT_FN_CAMERA_FOLLOW    0x6F3635E0u
/* Cursor-free camera drag.  CSimpleMessageFrame at CGameUI+0x250 owns the
 * native drag state; CDragScrollManager at +0x24C only provides the +0xA0
 * gate.  0x6F391170 captures projection/target, 0x6F390DE0 clears +0x84, and
 * 0x6F399CB0 applies one native update using global mouse input. */
#define DT_FN_CAMERA_DRAG_BEGIN  0x6F391170u
#define DT_FN_CAMERA_DRAG_CANCEL 0x6F390DE0u
#define DT_FN_CAMERA_DRAG_UPDATE 0x6F399CB0u
#define DT_FN_CAMERA_CAN_DRAG    0x6F360990u
#define DT_UI_DRAG_MANAGER       0x0000024Cu
#define DT_UI_DRAG_FRAME         0x00000250u
#define DT_DRAG_MANAGER_GATE     0x000000A0u
#define DT_DRAG_TARGET           0x0000006Cu /* 12 bytes: x,y,z */
#define DT_DRAG_WORLD            0x00000078u /* 12 bytes: x,y,z */
#define DT_DRAG_ACTIVE           0x00000084u
#define DT_DRAG_MARK             0x00000088u
#define DT_MOUSE_INPUT_XY        0x6FD4789Cu
#define DT_MOUSE_INPUT_Z         0x6FD478A4u
#define DT_FN_GRID_ACTIVATE    0x6F293480u
#define DT_FN_SUBGROUP_HAS_UNIT 0x6F290820u
#define DT_FN_GRID_SELECT_UNIT 0x6F396CF0u
#define DT_FN_SELECTION_REALIZE 0x6F291240u
#define DT_FN_COMMAND_BAR_POP  0x6F3C4E40u
#define DT_FN_COMMAND_BAR_HIDE 0x6F3ACA80u
#define DT_FN_UI_DIRTY         0x6F377190u
#define DT_FN_TARGET_UNIT      0x6F3BF010u
#define DT_FN_HOLDER_ALIVE     0x6F0549F0u
/* CMinimap::AddSignal.  Called with ecx = [CGameUI+0x3C0] and six dwords:
 * {world_x, world_y, 0, world_z, 0, 0}.  The callee overwrites the third
 * dword with 1.0f, applies the world->screen matrix at +0x750 and stores the
 * result in the 16-entry signal ring.  ret 0x18. */
#define DT_FN_ADD_SIGNAL       0x6F3B0A30u
#define DT_BEACON_TAP_SLOP     0.02f
/* CGameUI control-group action handlers.  These are the exact calls the
 * native digit hotkey handler makes in build/corpus-full/game/closed.c:
 *   plain digit: 0x6F3972D0([ui+0x24C], slot).  It rebuilds the group set
 *                from +0xDC+slot*0x14 and applies it to the selection via
 *                0x6F39738E -> 0x6F28FFA0 -> 0x6F3968B0 (0x6F36B880 is a
 *                different seven-slot manager and never recalls a group);
 *   Ctrl digit:  0x6F293B30(selection, &set) exports the current selection,
 *                0x6F293050(selection, slot, &set) assigns it to the slot,
 *                and 0x6F6BCD30(slot, &set) posts the 0x17 command;
 *   Shift digit: 0x6F28FF80(selection, slot, &set) exports the group, its
 *                units are merged into the exported selection, and the
 *                union is assigned through the same two calls as Ctrl.
 * No keyboard message, key state or modifier state is involved. */
#define DT_FN_GROUP_RECALL   0x6F3972D0u
#define DT_UI_GROUP_MANAGER  0x0000024Cu
#define DT_FN_EXPORT_SELECT  0x6F293B30u
#define DT_FN_EXPORT_GROUP   0x6F28FF80u
#define DT_FN_ASSIGN_GROUP   0x6F293050u
#define DT_FN_STORE_GROUP    0x6F6BCD30u
#define DT_FN_SET_ADD        0x6F268150u
#define DT_FN_SET_RETAIN     0x6F1DBB50u
/* DT_PLAYER_MANAGER (+0x28 local player id) -> 0x6F1E0EA0 -> player +0x34
 * is the live selection manager, the same chain the native handler uses. */
#define DT_FN_PLAYER_BY_ID   0x6F1E0EA0u
/* Non-group overlay bindings keep the original key-signal path. */
#define DT_FN_KEY_SIGNAL     0x6F358940u
#define DT_FN_SET_KEY_STATE  0x6F35A9F0u
/* set layout used by 0x6F2939F0/0x6F293050: +4 count, +8 unit array.
 * The set owns a dynamically grown vector (0x6F268150 grows it), so the
 * bound below is the selection profile's structural cap, not the original
 * 12-unit gameplay cap.  A group slot can hold 24 or 40 units when the
 * launch setting raises WC3_SELECTION_CAP; rejecting counts above 12 made a
 * Shift+digit merge silently fall back to a plain store. */
#define DT_GROUP_SET_COUNT   0x00000004u
#define DT_GROUP_SET_ARRAY   0x00000008u
#define DT_GROUP_SET_MAX     40u

#define DT_SELECTION_ACTIVE_GROUP 0x0000020Cu
/* The native 0x50065 handler derives the selection holder from the unit:
 * holder = [[unit+0x6C]+0x124].  The grid button stores a unit holder at
 * +0x124; dt_handle_unit() resolves that to the class instance first. */
#define DT_UNIT_HOLDER_SUB     0x0000006Cu
#define DT_UNIT_HOLDER_REF     0x00000124u
#define DT_UNIT_POSITION_SLOT  0x000000B8u
#define DT_CAMERA_READ_SIZE    0x00000060u

#define DT_EVENT_ID_TERRAIN_GATE 0x1A0068u
#define DT_EVENT_ID_GHOST        0x1A0065u
#define DT_EVENT_ID_GRID_TARGET  0x00050065u

#define DT_CLASS_UNIT 0x2B61676Cu

#define DT_SCRATCH_SIZE 0x100u
#define DT_PICK_OFFSET  0x00u
#define DT_POINT_OFFSET 0x30u
#define DT_UNIT_OFFSET  0x40u
/* Second 4-byte validator slot for debug probes; 0x44..0x4F is otherwise
 * unused before the selection event at 0x50. */
#define DT_PROBE_OFFSET 0x44u
#define DT_EVENT_OFFSET 0x50u
#define DT_EVENT_SIZE   0x40u

#define DT_MAX_EVENTS_PER_PUMP 16u
/* Backstop for a tap that was queued before the game left the map while the
 * guest thread was not pumping.  A normal event is serviced within one pump;
 * anything older than this belongs to a previous map/menu and must not be
 * replayed against the current one. */
#define DT_EVENT_MAX_AGE_MS 2000u

typedef struct {
    uint32_t ui;
    uint32_t world;
    uint32_t manager;
    uint32_t mode;      /* [ui+0x214], CSelectMode */
} DtContext;

static pthread_t g_main_thread;
static int g_main_thread_valid;
/* Re-entrancy guard, written only by the main guest thread but read by worker
 * GetMessageA callers through wc3_hle_direct_touch_can_service(). */
static int g_busy;
/* Routing snapshot.  The guest thread writes it at the message-pump boundary;
 * the Android input thread reads it before claiming a pointer.  Atomic so no
 * host lock is shared across those threads. */
static int g_route;
/* Last value written through dt_publish_route().  The input thread observes
 * g_route only; keeping a private mirror here avoids a needless release store
 * every time the pump re-publishes an unchanged route. */
static int g_route_published = -1;
/* Set only while the service has posted a native WM_LBUTTONDOWN and has not
 * yet posted the matching release.  Main guest thread only. */
static int g_native_down;
/* Set after a direct-owned release was posted but before the guest dispatched
 * it.  A second tap can arrive in the same service batch; the HLE still
 * reports the button down, so the next DOWN must bypass that guard while the
 * queued UP is ahead of it in the message queue.  Both physical buttons use
 * the same rule, so they are one small state object.  Main guest thread only. */
typedef struct {
    uint8_t left;
    uint8_t right;
} DtPendingRelease;

static DtPendingRelease g_native_release;
/* A late-resolved native UI click enters the universal gateway at release
 * time, not on the original DOWN: the press was held so the live route could
 * classify it as a world tap.  The gateway arms the DOWN at the release point
 * and publishes it after the idle hover pass; this state posts the matching
 * UP one pump later, exactly like a normal gesture.  The first service call
 * after arming gives the guest its idle pass, the second is the modal-loop
 * fallback.  Main guest thread only. */
static int g_native_up_pending;
static int g_native_up_wait;
/* Only a late-resolved native UI click may be forced out from the message-pump
 * boundary when its guest loop stops polling GetCursorPos.  A field selection
 * must always wait for the idle hover pass; forcing it is the first-world-tap
 * stale-hot-control regression.  Main guest thread only. */
static int g_native_up_modal;
/* Native minimap backstop.  The controls layer marks a minimap-owned DOWN
 * with the MINIMAP route, but a stale/empty snapshot can still deliver it as
 * NATIVE.  Track a DOWN that lands in the published minimap rectangle, keep
 * every later phase inside it, and force the release: the native frame
 * manager only delivers the button-up to the minimap while the cursor is
 * over the frame, and SetCapture is a no-op in this HLE.  Main guest thread
 * only. */
typedef struct {
    uint8_t active;
    uint8_t pointer;
    uint8_t pressed;
    float x;
    float y;
} DtMinimapGesture;

static DtMinimapGesture g_minimap;
/* Beacon button (MinimapSignal) gesture and armed minimap placement.  A tap
 * on the signal button arms the next minimap tap; a drag from the button
 * places the beacon at the release point.  The native signal mode is bypassed
 * because its two-step click path is not reliable through the HLE message
 * pump.  No native button is posted for this gesture, so there is no latched
 * button to release.  Main guest thread only. */
typedef struct {
    uint8_t armed;
    uint8_t active;
    uint8_t pointer;
    uint8_t moved;
    uint8_t from_button;
    float down_x;
    float down_y;
} DtBeaconGesture;

static DtBeaconGesture g_beacon;
/* Field selection and target gestures.  The camera drag has its own state
 * (Wc3CameraPan) and owns no mouse button.  Main guest thread only. */
typedef struct {
    uint8_t select_active;
    uint8_t select_pointer;
    /* A controlled selection in the field: the recognizer emitted BEGIN but
     * the native LMB press is withheld until the gesture proves to be a drag
     * (DRAG_BEGIN -> selection rectangle) or is released (END -> select for
     * a controllable exception target, smart order for every other unit,
     * attack-move for terrain, or native fallback click). */
    uint8_t select_deferred;
    uint8_t select_deferred_pointer;
    /* Hold point of the deferred long tap.  The target is resolved at the
     * point where the long press was recognised, not at the release point:
     * a finger that drifts a few pixels inside the drag threshold must not
     * turn an entangled gold mine into a ground order. */
    float select_deferred_x;
    float select_deferred_y;
    uint8_t target_active;
    uint8_t target_pointer;
} DtFieldGesture;

static DtFieldGesture g_field_drag;

/*
 * Cursor-free camera pan.  It owns no virtual mouse button: the native drag
 * manager is driven directly, with global mouse input swapped only around the
 * guest calls.  Several fingers may own the same camera pan at once; each
 * keeps its own world anchor and coalesced pending point, while the native
 * frame target is shared.  The recognizer's END and the service's reset paths
 * clear a finger, never a mouse message.  Main guest thread only.
 */
#define DT_CAMERA_PAN_MAX 4u

typedef struct {
    uint8_t  active;
    uint8_t  pending;          /* one coalesced MOVE awaits apply */
    uint8_t  pointer;
    uint32_t gesture_id;
    float    z;
    float    last_world[3];    /* world anchor under last_x/last_y */
    float    last_x;           /* pick-event space of last processed point */
    float    last_y;
    float    pending_x;        /* surface-normalised latest MOVE */
    float    pending_y;
} Wc3CameraPanPointer;

typedef struct {
    uint8_t  active;           /* at least one camera finger owns the pan */
    uint8_t  rebase_pending;
    uint32_t frame;            /* CSimpleMessageFrame at ui+0x250 */
    uint32_t camera;
    float    target[3];        /* frame+0x6C, diagnostics */
    Wc3CameraPanPointer pointers[DT_CAMERA_PAN_MAX];
} Wc3CameraPan;

static Wc3CameraPan g_camera_pan;

/* A battlefield drag that starts while target/placement/transient owns the
 * world.  The recognizer still sees the physical FIELD gesture, but the
 * gesture must confirm/settle the target instead of rotating the camera or
 * starting a selection box.  No native button is pressed until release. */
/* Last context the service dispatched against.  A change means the queue may
 * still hold semantic events from the previous map/menu even if the
 * transition itself was never observed (the guest thread was not pumping). */
static uint32_t g_last_ui;
static uint32_t g_last_world;
/* Portrait and minimap frame snapshots, surface-normalised.  The guest thread
 * is the only writer; the Android input thread reads them through
 * wc3_hle_direct_touch_portrait_rect()/wc3_hle_direct_touch_minimap_rect().
 * A seqlock keeps the four floats consistent without sharing a host lock
 * across threads. */
typedef struct {
    uint32_t seq;
    uint32_t rect[4];
} DtRectSnapshot;

static DtRectSnapshot g_portrait_snapshot;
static DtRectSnapshot g_minimap_snapshot;
/* Full minimap panel (CGameUI+0x41C), including the Minimap* button row.
 * Published on the guest thread so the controls layer can claim the whole
 * panel before the field recognizer turns a button press into a long press. */
static DtRectSnapshot g_minimap_panel_snapshot;
/* The beacon button (MinimapSignal) is a separate frame.  Publishing its
 * rectangle lets the controls layer force a press on the button to the raw
 * NATIVE route even if the button overlaps the field rectangle; the guest
 * service then claims the gesture before posting any native mouse button. */
static DtRectSnapshot g_beacon_snapshot;
/* The command card's top row overlaps the field rectangle (the card starts
 * at 0.7817 and the field ends at 0.85), so the controls layer needs the
 * card rect to claim the press for NATIVE before the field recognizer sees
 * it.  Published in surface coordinates, like the beacon snapshot. */
static DtRectSnapshot g_command_snapshot;
/* The selected-unit grid's top row overlaps the field rectangle, so the
 * controls layer needs the union of the visible cell rectangles to claim a
 * unit tap for NATIVE before the field recognizer turns it into a world
 * selection.  Published in surface coordinates. */
static DtRectSnapshot g_grid_snapshot;
/* The union of the live inventory slot-button rectangles.  The bar sits left
 * of the command card and its top row overlaps the field rectangle (the
 * inventory top row centre is around y 0.8355, inside the field bottom 0.85),
 * so the controls layer must claim a press inside this union for the raw
 * NATIVE route before the field recognizer sees it.  The same union is the
 * HUD exclusion for an item-drag release: a point over the bar is never a
 * battlefield drop. */
static DtRectSnapshot g_inventory_snapshot;
/* Set by wc3_hle_direct_touch_reset() (any thread) when the game leaves the
 * in-game UI.  The service, on the main guest thread, clears the camera
 * follow before it drops the queue. */
static int g_portrait_release_pending;
/* True while the native camera follow started by a portrait finger is live.
 * A direct camera pan yields to it; otherwise both controllers fight over the
 * same camera target.  Main guest thread only. */
static int g_portrait_follow_active;
/* True while the exact CEscMenu pause panel is open.  This is the service's
 * cached copy of dt_pause_menu_active(); the input thread sees the same state
 * as the MENU route in the published snapshot.  Main guest thread only. */
static int g_pause_menu_active;
/* Selected-unit grid gesture.  A press on a cell makes that unit's subgroup
 * active immediately; a release without crossing into another cell then
 * selects the unit individually.  Crossing to another cell keeps switching
 * the active subgroup and suppresses the individual selection, so a drag is
 * a subgroup switch while a tap is a unit selection.  Main guest thread
 * only. */
typedef struct {
    uint8_t active;
    uint8_t pointer;
    uint8_t target;
    uint8_t moved;
    uint32_t ui;
    uint32_t selection;
    uint32_t unit;      /* cell currently under the finger */
    uint32_t down_unit; /* cell where the press started */
} DtGridGesture;

static DtGridGesture g_grid;
/* Set when the grid touch selected a target instead of activating a group.
 * The rest of the gesture is consumed so a drag cannot retarget or reach the
 * native world handler. */

/* Inventory item drag.  A touch that starts on an occupied slot is consumed
 * so the release can be replayed as the native click pair: left click on the
 * source (use), or right click on the source followed by a left click on the
 * destination slot or world point (swap/move/drop).  Main guest thread only. */
typedef struct {
    uint8_t active;
    uint8_t pointer;
    uint8_t slot;
    uint32_t bar;
    float down_x;
    float down_y;
} DtInventoryGesture;

static DtInventoryGesture g_inv;
/* A raw field press that is a tap candidate.  The native DOWN is held only
 * until the release so the gesture can be classified as a tap; the tap is
 * then dispatched immediately with the live route.  A drag falls back to the
 * normal native path.  Main guest thread only. */
static int g_field_defer_active;
static uint32_t g_field_defer_pointer;
static float g_field_defer_x;
static float g_field_defer_y;
static int g_field_defer_moved;
static int g_field_defer_down_posted;
/* Why a raw field press is being deferred.  The four cases are mutually
 * exclusive, so they are one owner tag instead of four booleans that could
 * disagree.  Priority at DOWN: LOAD > LIVE > TARGET > LATE. */
typedef enum {
    WC3_DT_FIELD_DEFER_NONE = 0,
    WC3_DT_FIELD_DEFER_LIVE,    /* live FIELD route, tap resolved on UP */
    WC3_DT_FIELD_DEFER_TARGET,  /* in-game target/transient mode */
    WC3_DT_FIELD_DEFER_LATE,    /* native UI live, semantic route not FIELD */
    WC3_DT_FIELD_DEFER_LOAD     /* map load / input gate closed */
} Wc3DtFieldDefer;

static Wc3DtFieldDefer g_field_defer_kind;

#if WC3_DEBUG && WC3_DEBUG_INPUT
/* Trace labels are indexed by the owner tag so a new case cannot silently
 * inherit another case's label through a ternary chain. */
static const char *const g_field_defer_defer_trace[] = {
    "field-none-defer", "field-tap-defer", "field-target-defer",
    "field-late-defer", "field-click-defer"
};
static const char *const g_field_defer_drag_trace[] = {
    "field-none-drag", "field-tap-drag", "field-target-drag",
    "field-late-drag", "field-click-drag"
};
static const char *const g_field_defer_up_miss_trace[] = {
    "field-none-up-miss", "field-tap-up-miss", "field-target-up-miss",
    "field-late-up-miss", "field-load-up-miss"
};
static const char *const g_field_defer_release_trace[] = {
    "field-none-release", "field-tap-release-up", "field-target-release",
    "field-late-release", "field-load-release"
};
#endif
/* Deferred field replay.  Two cases share the slot: a field press held until
 * the map-load/fade-in gate opens, and a live target/placement native click
 * held for one guest frame so the placement preview follows the release
 * hover.  A live select tap is dispatched immediately and never uses it. */
static int g_field_click_pending;
static Wc3DirectTouchEvent g_field_click_event;
/* Input epoch captured when the deferred click was armed.  The click is
 * posted only while the epoch is unchanged, so any newer key/button/touch
 * cancels it deterministically instead of a timeout expiring. */
static uint32_t g_field_click_input_epoch;
/* The same slot carries two kinds of replay: the map-load semantic tap and
 * the live target/placement native click.  The latter is held for one frame
 * so the preview has moved before the mode resolves it. */
static int g_field_click_native;
/* The queued native click confirms the attack-move target mode opened by the
 * A key.  The tick waits for the game's live-mode signal instead of the fixed
 * target/placement delay: the mode is created on the guest update after the
 * A key, with the release hover already in place. */
static int g_field_click_attack_move;
static uint64_t g_field_click_ready_ms;
/* Exact service route for the current pump.  The input-thread snapshot can
 * already see a live select mode while the guest still reports not-in-game;
 * the field deferral must use this exact route, not the snapshot. */
static int g_service_field_ready;

/* Command-card gesture.  A touch that starts on a command button is consumed
 * and immediately replayed as a native left click, so the command is selected
 * on finger-down.  A drag follows the finger with the native cursor; a release
 * over the field from a targeted/area command confirms the target there.  A
 * long press posts the native right click, which is the autocast toggle.  Main
 * guest thread only. */
static int g_cmd_active;
static uint32_t g_cmd_pointer;
static uint32_t g_cmd_bar;
static uint32_t g_cmd_button;
static uint32_t g_cmd_data;
static uint32_t g_cmd_type;
static float g_cmd_down_x;
static float g_cmd_down_y;
static uint64_t g_cmd_down_ms;
static int g_cmd_moved;
static int g_cmd_long_pending;
static int g_cmd_long_fired;
/* Confirming field click queued by a drag release.  It is posted by the pump
 * tick, after the game has switched to target mode. */
typedef struct {
    uint8_t pending;
    uint32_t bar;
    uint32_t button;
    uint32_t data;
    uint32_t type;
    uint32_t mode;
    float x;
    float y;
    uint64_t ms;
} DtCommandTarget;

static DtCommandTarget g_cmd_target;
/* The live input mode the command gesture left CSelectMode for.  A field
 * select drag uses a different temporary mode, so a bare "active != select"
 * check could mistake the drag for a command target.  Recording the first
 * non-select mode seen after the card click keeps the confirmation tied to the
 * game's target mode. */

#if WC3_DEBUG && WC3_DEBUG_INPUT
#include <stdarg.h>
#include <stdlib.h>

/*
 * Debug-only direct-touch log.  logcat is a ring buffer: a long guest run
 * overwrites the trace before it can be pulled, and the game log is a
 * different stream.  Write every line only to dt_trace.txt in WC3_DATA_DIR,
 * the same way selection.c writes sel_trace.txt.  The file is truncated per
 * process, line buffered and opened once; there is no logcat sink.
 */
static FILE *g_dt_log;
static pthread_once_t g_dt_log_once = PTHREAD_ONCE_INIT;
/* The probe is posted from the input thread and dumped from the guest
 * thread.  Serialize complete lines; the lock covers file I/O only and is
 * never held across a guest call or a wait. */
static pthread_mutex_t g_dt_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void dt_open_log(void)
{
    const char *dir = getenv("WC3_DATA_DIR");
    char path[1024];

    if (dir && *dir)
        snprintf(path, sizeof(path), "%s/dt_trace.txt", dir);
    else
        snprintf(path, sizeof(path), "dt_trace.txt");
    g_dt_log = fopen(path, "w");
    if (!g_dt_log)
        return;
    setvbuf(g_dt_log, NULL, _IOLBF, 0);
    fprintf(g_dt_log, "[dt] trace start\n");
}

/* Defined below; the trace prefix uses the same monotonic clock as the input
 * events so a line can be matched against touch_trace.txt and sel_trace.txt. */
static uint64_t dt_now_ms(void);

static void dt_log_vwrite(const char *fmt, va_list ap)
{
    pthread_once(&g_dt_log_once, dt_open_log);
    if (!g_dt_log)
        return;

    pthread_mutex_lock(&g_dt_log_lock);
    fprintf(g_dt_log, "[dt] t=%llu ", (unsigned long long)dt_now_ms());
    vfprintf(g_dt_log, fmt, ap);
    fputc('\n', g_dt_log);
    pthread_mutex_unlock(&g_dt_log_lock);
}

static void dt_log_write(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    dt_log_vwrite(fmt, ap);
    va_end(ap);
}

#define dt_log(...) dt_log_write(__VA_ARGS__)

static const char *dt_route_name(int route)
{
    switch (route) {
    case WC3_DIRECT_TOUCH_ROUTE_LEGACY: return "LEGACY";
    case WC3_DIRECT_TOUCH_ROUTE_FIELD: return "FIELD";
    case WC3_DIRECT_TOUCH_ROUTE_NATIVE: return "NATIVE";
    case WC3_DIRECT_TOUCH_ROUTE_PORTRAIT: return "PORTRAIT";
    case WC3_DIRECT_TOUCH_ROUTE_MINIMAP: return "MINIMAP";
    case WC3_DIRECT_TOUCH_ROUTE_MENU: return "MENU";
    default: return "?";
    }
}

static const char *dt_kind_name(uint32_t kind)
{
    switch (kind) {
    case WC3_DIRECT_TOUCH_NONE: return "NONE";
    case WC3_DIRECT_TOUCH_TAP: return "TAP";
    case WC3_DIRECT_TOUCH_LONG_PRESS: return "LONG_PRESS";
    case WC3_DIRECT_TOUCH_POINTER_DOWN: return "POINTER_DOWN";
    case WC3_DIRECT_TOUCH_POINTER_MOVE: return "POINTER_MOVE";
    case WC3_DIRECT_TOUCH_POINTER_UP: return "POINTER_UP";
    case WC3_DIRECT_TOUCH_POINTER_CANCEL: return "POINTER_CANCEL";
    case WC3_DIRECT_TOUCH_HOLD_RELEASE: return "HOLD_RELEASE";
    case WC3_DIRECT_TOUCH_FOLLOW_START: return "FOLLOW_START";
    case WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN: return "FIELD_CAMERA_BEGIN";
    case WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE: return "FIELD_CAMERA_MOVE";
    case WC3_DIRECT_TOUCH_FIELD_CAMERA_END: return "FIELD_CAMERA_END";
    case WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN: return "FIELD_SELECT_BEGIN";
    case WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE: return "FIELD_SELECT_MOVE";
    case WC3_DIRECT_TOUCH_FIELD_SELECT_END: return "FIELD_SELECT_END";
    case WC3_DIRECT_TOUCH_FIELD_SELECT_DRAG_BEGIN:
        return "FIELD_SELECT_DRAG_BEGIN";
    case WC3_DIRECT_TOUCH_FIELD_HOVER: return "FIELD_HOVER";
    case WC3_DIRECT_TOUCH_KEY_SIGNAL: return "KEY_SIGNAL";
    case WC3_DIRECT_TOUCH_GROUP_SIGNAL: return "GROUP_SIGNAL";
    default: return "?";
    }
}

static void dt_trace(const char *what, uint32_t a, uint32_t b)
{
    dt_log("[direct] %s a=%08X b=%08X", what, a, b);
}
#else
#define dt_log(...) ((void)0)
#define dt_trace(what, a, b) ((void)0)
#endif

static uint32_t dt_bits(float value)
{
    union {
        float f;
        uint32_t u;
    } u;
    u.f = value;
    return u.u;
}

static float dt_float(uint32_t bits)
{
    union {
        uint32_t u;
        float f;
    } v;
    v.u = bits;
    return v.f;
}

static void dt_beacon_reset(void)
{
    g_beacon.active = 0;
    g_beacon.pointer = 0;
    g_beacon.moved = 0;
    g_beacon.from_button = 0;
}

static void dt_beacon_cancel(void)
{
    g_beacon.armed = 0;
    dt_beacon_reset();
}

static void dt_field_defer_reset(void)
{
    g_field_defer_active = 0;
    g_field_defer_pointer = 0;
    g_field_defer_moved = 0;
    g_field_defer_down_posted = 0;
    g_field_defer_kind = WC3_DT_FIELD_DEFER_NONE;
}

static void dt_field_click_reset(void)
{
    dt_field_defer_reset();
    g_field_click_pending = 0;
    memset(&g_field_click_event, 0, sizeof g_field_click_event);
    g_field_click_input_epoch = 0;
    g_field_click_native = 0;
    g_field_click_attack_move = 0;
    g_field_click_ready_ms = 0;
}

/* Defined later; the beacon release path uses the published minimap
 * rectangle and the battlefield rectangle before those helpers appear. */
static int dt_minimap_native_rect(float *left, float *top,
                                  float *right, float *bottom);
static int dt_field_contains(float nx, float ny);
static int dt_field_hud_contains(float x, float y);
static int dt_field_target_active(CPU *c, const DtContext *ctx);
static void dt_native_left_click(float x, float y);
static void dt_native_right_click(float x, float y);
static void dt_native_click(const Wc3DirectTouchEvent *ev);
static void dt_native_press_deferred(const Wc3DirectTouchEvent *ev,
                                     int modal_fallback);
static void dt_press_native(float x, float y, int button, int release);

/* Start a native left press at a surface point.  The gateway posts the move
 * and arms the DOWN; the idle GetCursorPos pass publishes it.  Keeping the
 * two state updates together stops a call site from arming the HLE press
 * without the direct-touch bookkeeping (or the reverse). */
static void dt_native_begin(float x, float y)
{
    dt_press_native(x, y, 0, 0);
    g_native_down = 1;
    g_native_release.left = 0;
}

/* The replay boundary already posted both DOWN and UP; only the direct-touch
 * release bookkeeping remains. */
static void dt_native_replay_done(void)
{
    g_native_down = 0;
    g_native_release.left = 1;
}

/* The armed press never reached the guest; clear both direct-touch flags
 * without posting a lone UP. */
static void dt_native_cancel_done(void)
{
    g_native_down = 0;
    g_native_release.left = 0;
}

/* Post the matching left-button UP and record that a release is in the
 * queue.  Replay paths already posted their own UP and call
 * dt_native_replay_done() instead. */
static void dt_native_end(void)
{
    hle_android_mouse_button_up_force(0);
    dt_native_replay_done();
}

/* Drop a native left press that may still be armed.  An armed press is
 * cancelled before it reaches the guest; a press that already reached the
 * queue gets its matching UP.  A camera sequence in the same single pending
 * slot belongs to another gesture and is left alone. */
static void dt_native_abort(void)
{
    if (hle_android_mouse_press_pending()) {
        (void)hle_android_mouse_cancel_pending_press();
        dt_native_cancel_done();
        return;
    }
    if (g_native_down) dt_native_end();
}

static float dt_clamp01(float value)
{
    if (!(value > 0.0f)) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

/* Event timestamps come from the input thread's monotonic clock.  The service
 * runs on the same device clock, so the age check below stays in one domain. */
static uint64_t dt_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Hold a semantic tap for the map-load/fade-in deferral.  This is not used
 * for panel taps: those are dispatched immediately on the same gesture. */
static void dt_field_click_queue(const Wc3DirectTouchEvent *ev)
{
    /* Snapshot before committing the click: every input already queued is
     * part of this gesture's past, and any input that arrives after this
     * point must cancel the click rather than be absorbed into the snapshot. */
    g_field_click_input_epoch = hle_android_input_epoch();
    g_field_click_event = *ev;
    g_field_click_event.kind = WC3_DIRECT_TOUCH_TAP;
    g_field_click_event.route = WC3_DIRECT_TOUCH_ROUTE_FIELD;
    g_field_click_pending = 1;
    g_field_click_native = 0;
    g_field_click_attack_move = 0;
    g_field_click_ready_ms = 0;
}

/* A live target/placement confirmation is a native click, but the mode must
 * see the hover first.  The recognizer already publishes the contact point on
 * DOWN and again on UP; hold the click for one frame so the guest can rebuild
 * the placement preview from those moves.  The existing field-click slot is
 * reused, so a new gesture or a context change cancels both kinds.
 *
 * `input_epoch` is captured before any synthetic key or hover move that
 * belongs to this confirmation.  A user input that arrives after that point
 * advances the epoch and cancels the click; the synthetic key itself does
 * not advance it. */
static void dt_field_target_click_queue_at(const Wc3DirectTouchEvent *ev,
                                           uint32_t input_epoch)
{
    g_field_click_input_epoch = input_epoch;
    g_field_click_event = *ev;
    g_field_click_pending = 1;
    g_field_click_native = 1;
    g_field_click_attack_move = 0;
    g_field_click_ready_ms = dt_now_ms() +
                             DT_FIELD_TARGET_CLICK_DELAY_MS;
    hle_android_mouse_move(ev->x, ev->y);
}

static void dt_field_target_click_queue(const Wc3DirectTouchEvent *ev)
{
    dt_field_target_click_queue_at(ev, hle_android_input_epoch());
}

/* The A key opens attack-move target mode on the guest's next update, not in
 * the release pump.  Queue the confirming left click through the same slot as
 * a live target/placement click, but do not wait the fixed 33 ms: the tick
 * fires as soon as the game reports the mode live.  The mode is created with
 * the release hover already in place, so the target/placement preview delay
 * is not needed here. */
static void dt_field_attack_move_click_queue_at(const Wc3DirectTouchEvent *ev,
                                                uint32_t input_epoch)
{
    dt_field_target_click_queue_at(ev, input_epoch);
    g_field_click_attack_move = 1;
    g_field_click_ready_ms = 0;
}

static int dt_guest_readable(uint32_t va, uint32_t size)
{
    /* A selection/handle list link left over from a released VirtualAlloc
     * block still passes hle_android_guest_ok() because the whole arena is a
     * legal layout range.  The debug trace walks those links, so require the
     * live-block check before dereferencing one. */
    return va && hle_android_guest_committed(va, size);
}

#if WC3_DEBUG && WC3_DEBUG_INPUT
/*
 * Bounded first-contact probe.
 *
 * The guest keeps its own input ring in CGame.dll's data: sixteen entries of
 * five dwords at 0x6FD3D410, with the next-write index at 0x6FD3D550 and the
 * next-read index at 0x6FD3D554.  The window procedure fills it from the
 * Win32 mouse messages before the per-frame pump converts the entries into
 * CGameUI events.  Code 9 is a button down, 10 an absolute move, 11 a wheel,
 * 12 a relative move and 13 a button up; a full ring drops its oldest entry
 * (0x6F091327/0x6F091344), so the probe also has to show whether the hover was
 * evicted before the click.
 *
 * The probe starts at the panel gesture and ends after the first field
 * release, on timeout, or when a later panel gesture supersedes it.  It reads
 * only guest memory already owned by the pump and never calls guest code, so
 * it can run inside the native message wrappers.
 */
#define DT_PROBE_LIFETIME_MS  60000u
#define DT_GAME_QUEUE_BASE    0x6FD3D410u
#define DT_GAME_QUEUE_HEAD    0x6FD3D550u
#define DT_GAME_QUEUE_TAIL    0x6FD3D554u
#define DT_GAME_QUEUE_SLOTS   16u
#define DT_GAME_QUEUE_STRIDE  20u
#define DT_GAME_CAPTURE       0x6FD3D3E8u
#define DT_GAME_PRESSED       0x6FD3D408u
#define DT_GAME_REL_GUARD     0x6FD3D40Cu
#define DT_GAME_UI_BUTTONS    0x6FD3CE54u
#define DT_GAME_UI_PRESSED    0x6FD3CE58u
#define DT_GAME_UI_CONTEXT    0x6FD3CE60u
#define DT_GAME_LAST_X        0x6FD3D558u
#define DT_GAME_LAST_Y        0x6FD3D55Cu
#define DT_UI_MODE_STACK      0x000001B8u

/* Written by the guest thread, read by the input thread when it posts a
 * message.  A diagnostic race is harmless, but volatile keeps the state from
 * being cached across the two threads at -O3. */
static volatile int g_dt_probe_active;
static volatile uint64_t g_dt_probe_until;
static uint32_t g_dt_probe_seq;
static int g_dt_probe_field_seen;

/* Map-start contact trace.  The panel probe only starts when a panel gesture
 * begins, which is too late for the first tap after a map load.  Arm this
 * tracker when a new world appears, then start a fresh probe sequence at the
 * first contact and keep the first few contacts bounded.  The audit samples
 * the live selection after a select tap so a late apply is visible even when
 * the guest consumed the native click long after the touch. */
#define DT_MAP_PROBE_CONTACTS 6u
#define DT_MAP_AUDIT_CONTACTS 4u
#define DT_MAP_AUDIT_MS       5000u
#define DT_MAP_AUDIT_EVERY_MS 20u
#define DT_MAP_AUDIT_SAMPLES  300
#define DT_MAP_AUDIT_HOLD     3

static int g_map_probe_armed;
static uint32_t g_map_probe_world;
static uint32_t g_map_contacts;
static int g_map_contact_open;
static int g_map_audit_remaining;
static int g_map_audit_samples;
static int g_map_audit_seen;
static uint32_t g_map_audit_contact;
static uint32_t g_map_audit_unit;
static uint64_t g_map_audit_until;
static uint64_t g_map_audit_last_ms;

static uint32_t dt_probe_read32(uint32_t va, int *ok)
{
    if (!dt_guest_readable(va, 4u)) {
        if (ok) *ok = 0;
        return 0;
    }
    if (ok) *ok = 1;
    return wc3_rd32g(va);
}

static void dt_probe_dump_queue(const char *why)
{
    if (!dt_guest_readable(DT_GAME_QUEUE_BASE, 0x148u)) {
        dt_log("probe q %s unavailable", why);
        return;
    }

    const uint32_t head = wc3_rd32g(DT_GAME_QUEUE_HEAD);
    const uint32_t tail = wc3_rd32g(DT_GAME_QUEUE_TAIL);
    if (head >= DT_GAME_QUEUE_SLOTS || tail >= DT_GAME_QUEUE_SLOTS) {
        dt_log("probe q %s bad head=%u tail=%u", why, head, tail);
        return;
    }

    const uint32_t count =
        (head - tail) & (DT_GAME_QUEUE_SLOTS - 1u);
    dt_log("probe q %s head=%u tail=%u n=%u", why, head, tail, count);

    /* A full ring drops its oldest entry.  The indices alone cannot show
     * which entry was overwritten; log every live entry in read order. */
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t index = (tail + i) & (DT_GAME_QUEUE_SLOTS - 1u);
        const uint32_t entry =
            DT_GAME_QUEUE_BASE + index * DT_GAME_QUEUE_STRIDE;
        dt_log("probe q[%u] code=%u d1=%08X d2=%08X d3=%08X d4=%08X",
               index, wc3_rd32g(entry), wc3_rd32g(entry + 4u),
               wc3_rd32g(entry + 8u), wc3_rd32g(entry + 0x0Cu),
               wc3_rd32g(entry + 0x10u));
    }
}

static void dt_probe_dump(const char *what, uint32_t a, uint32_t b)
{
    int ok = 0;
    const uint32_t ui = dt_probe_read32(DT_UI_GLOBAL, &ok);
    uint32_t world = 0;
    uint32_t gate = 0;
    uint32_t active = 0;
    uint32_t select = 0;
    uint32_t target = 0;
    uint32_t stack_count = 0;
    uint32_t stack_ptr = 0;
    if (ok && ui && dt_guest_readable(ui, DT_UI_SELECT_MODE + 4u)) {
        world = wc3_rd32g(ui + DT_UI_WORLD_FRAME);
        gate = wc3_rd32g(ui + DT_UI_CLICK_GATE);
        active = wc3_rd32g(ui + DT_UI_ACTIVE_MODE);
        select = wc3_rd32g(ui + DT_UI_SELECT_MODE);
        target = wc3_rd32g(ui + DT_UI_TARGET_MODE);
        stack_ptr = wc3_rd32g(ui + DT_UI_MODE_STACK);
        stack_count = wc3_rd32g(ui + DT_UI_MODE_COUNT);
    }

    dt_log("probe %s a=%08X b=%08X ui=%08X world=%08X gate=%08X "
           "active=%08X select=%08X target=%08X stack=%08X/%u",
           what ? what : "?", a, b, ui, world, gate, active, select,
           target, stack_ptr, stack_count);
    dt_log("probe %s cap=%08X pressed=%08X guard=%08X ui_btns=%08X "
           "ui_press=%08X ctx=%08X last=%08X,%08X",
           what ? what : "?",
           dt_probe_read32(DT_GAME_CAPTURE, NULL),
           dt_probe_read32(DT_GAME_PRESSED, NULL),
           dt_probe_read32(DT_GAME_REL_GUARD, NULL),
           dt_probe_read32(DT_GAME_UI_BUTTONS, NULL),
           dt_probe_read32(DT_GAME_UI_PRESSED, NULL),
           dt_probe_read32(DT_GAME_UI_CONTEXT, NULL),
           dt_probe_read32(DT_GAME_LAST_X, NULL),
           dt_probe_read32(DT_GAME_LAST_Y, NULL));

    if (stack_ptr && stack_count && stack_count <= 16u &&
        dt_guest_readable(stack_ptr, stack_count * 4u)) {
        char line[160];
        int used = snprintf(line, sizeof line, "probe %s stack",
                            what ? what : "?");
        for (uint32_t i = 0; i < stack_count && used > 0 &&
                            (size_t)used + 10u < sizeof line; i++) {
            const int n = snprintf(line + used,
                                   sizeof line - (size_t)used,
                                   " %08X", wc3_rd32g(stack_ptr + i * 4u));
            if (n <= 0) break;
            used += n;
        }
        if (used > 0) dt_log("%s", line);
    }

    dt_probe_dump_queue(what ? what : "?");
}

void wc3_hle_direct_touch_tracef(const char *fmt, ...)
{
    va_list ap;

    /* Same prefix, same lock, same file as dt_log_write: the public trace
     * entry point must not be a second writer with its own state. */
    va_start(ap, fmt);
    dt_log_vwrite(fmt, ap);
    va_end(ap);
}

void wc3_hle_direct_touch_probe_begin(const char *reason,
                                      uint32_t a, uint32_t b)
{
    const uint64_t now = dt_now_ms();
    const int was_active =
        g_dt_probe_active &&
        !(now && g_dt_probe_until && now >= g_dt_probe_until);

    /* A field release closes the sequence.  Keep the probe running so the
     * native posts and the guest dispatch after it are still captured, but a
     * later panel click starts a new sequence instead of extending the old. */
    const int new_sequence = !was_active || g_dt_probe_field_seen;
    if (new_sequence) {
        g_dt_probe_seq++;
        g_dt_probe_field_seen = 0;
    }
    g_dt_probe_active = 1;
    g_dt_probe_until = now ? now + DT_PROBE_LIFETIME_MS : 0;
    dt_log("probe begin#%u %s reason=%s a=%08X b=%08X",
           g_dt_probe_seq, new_sequence ? "start" : "extend",
           reason ? reason : "?", a, b);
    dt_probe_dump(new_sequence ? "begin" : "extend", a, b);
}

int wc3_hle_direct_touch_probe_active(void)
{
    if (!g_dt_probe_active) return 0;
    const uint64_t now = dt_now_ms();
    if (now && g_dt_probe_until && now >= g_dt_probe_until) {
        dt_log("probe end#%u reason=timeout", g_dt_probe_seq);
        g_dt_probe_active = 0;
        return 0;
    }
    return 1;
}

void wc3_hle_direct_touch_probe_mark(const char *what, uint32_t a,
                                     uint32_t b)
{
    if (!wc3_hle_direct_touch_probe_active()) return;
    if (what && !g_dt_probe_field_seen &&
        strncmp(what, "field-release", 13u) == 0) {
        g_dt_probe_field_seen = 1;
        /* The release marks below and the native posts they trigger are the
         * evidence.  Keep a short grace period for the next guest pump, then
         * stop instead of logging the rest of the match. */
        const uint64_t now = dt_now_ms();
        if (now) g_dt_probe_until = now + 2000u;
    }
    dt_probe_dump(what ? what : "?", a, b);
}

void wc3_hle_direct_touch_probe_end(const char *reason)
{
    if (!g_dt_probe_active) return;
    dt_log("probe end#%u reason=%s", g_dt_probe_seq,
           reason ? reason : "?");
    dt_probe_dump("end", 0, 0);
    g_dt_probe_active = 0;
}

static uint32_t dt_app_active_flag(void)
{
    return dt_guest_readable(DT_APP_ACTIVE_GLOBAL, 4u)
        ? wc3_rd32g(DT_APP_ACTIVE_GLOBAL) : 0u;
}

static void dt_map_probe_clear(void)
{
    if (!g_map_probe_world && !g_map_probe_armed) return;
    dt_log("map-probe clear world=%08X contacts=%u",
           g_map_probe_world, g_map_contacts);
    g_map_probe_world = 0;
    g_map_probe_armed = 0;
    g_map_contacts = 0;
    g_map_contact_open = 0;
    g_map_audit_remaining = 0;
    g_map_audit_until = 0;
    g_map_audit_last_ms = 0;
}

static void dt_map_probe_arm(uint32_t ui, uint32_t world, const char *why,
                             int force)
{
    if (!world) return;
    if (!force && g_map_probe_world == world) return;
    g_map_probe_world = world;
    g_map_probe_armed = 1;
    g_map_contacts = 0;
    g_map_contact_open = 0;
    g_map_audit_remaining = 0;
    g_map_audit_until = 0;
    g_map_audit_last_ms = 0;
    dt_log("map-probe arm world=%08X ui=%08X reason=%s force=%d",
           world, ui, why ? why : "?", force);
}

static void dt_map_note_contact(const char *reason, uint32_t kind,
                                uint32_t pointer, float x, float y,
                                uint32_t event_route, uint32_t service_route)
{
    if (g_map_contacts >= DT_MAP_PROBE_CONTACTS) {
        /* Keep the contact open so the rest of this gesture is not counted
         * again; the bounded probe has already captured enough evidence. */
        g_map_contact_open = 1;
        return;
    }
    const int first = g_map_probe_armed;
    g_map_probe_armed = 0;
    g_map_contacts++;
    g_map_contact_open = 1;
    wc3_hle_direct_touch_probe_begin(first ? "map-first-contact"
                                           : "map-contact",
                                     kind, pointer);
    dt_log("map-contact#%u %s kind=%s(%u) ptr=%u n=%.4f,%.4f "
           "event_route=%s service_route=%s field=%d first=%d app_active=%u",
           g_map_contacts, reason ? reason : "?", dt_kind_name(kind), kind,
           pointer, x, y, dt_route_name((int)event_route),
           dt_route_name((int)service_route), dt_field_contains(x, y), first,
           dt_app_active_flag());
}
#endif

/*
 * CGameUI+0x3BC is the live CWorldFrameWar3* for the current map.  The
 * separate 0x6FD47818 global is only populated while a world-frame
 * selection path is active (0x6F0F04B0 writes it from the world frame);
 * during normal play it is zero.  Prefer the UI field, which exists as
 * soon as the map is loaded, and keep the global only as a transition
 * fallback.
 */
static uint32_t dt_world_from_ui(uint32_t ui)
{
    if (ui && dt_guest_readable(ui, DT_UI_WORLD_FRAME + 4u)) {
        const uint32_t world = wc3_rd32g(ui + DT_UI_WORLD_FRAME);
        if (world) return world;
    }
    if (dt_guest_readable(DT_WORLD_GLOBAL, 4u)) {
        const uint32_t world = wc3_rd32g(DT_WORLD_GLOBAL);
        if (world) return world;
    }
    return 0;
}

/* The UI singleton is the live native recipient even when no semantic field
 * context exists (menu, loading, target/transient mode). */
static int dt_ui_present(uint32_t *ui_out)
{
    uint32_t ui = 0;
    if (dt_guest_readable(DT_UI_GLOBAL, 4u)) ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui) return 0;
    if (ui_out) *ui_out = ui;
    return 1;
}

static void dt_publish_route(Wc3DirectTouchRoute route)
{
    const int value = (int)route;
    if (__atomic_load_n(&g_route_published, __ATOMIC_RELAXED) == value) return;
    __atomic_store_n(&g_route_published, value, __ATOMIC_RELAXED);
    __atomic_store_n(&g_route, value, __ATOMIC_RELEASE);
}

static void dt_rect_publish(DtRectSnapshot *snap, float left, float top,
                            float right, float bottom)
{
    const uint32_t seq = __atomic_load_n(&snap->seq, __ATOMIC_RELAXED);
    __atomic_store_n(&snap->seq, seq + 1u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&snap->rect[0], dt_bits(left), __ATOMIC_RELAXED);
    __atomic_store_n(&snap->rect[1], dt_bits(top), __ATOMIC_RELAXED);
    __atomic_store_n(&snap->rect[2], dt_bits(right), __ATOMIC_RELAXED);
    __atomic_store_n(&snap->rect[3], dt_bits(bottom), __ATOMIC_RELAXED);
    __atomic_store_n(&snap->seq, seq + 2u, __ATOMIC_RELEASE);
}

static int dt_rect_read(const DtRectSnapshot *snap, float *left, float *top,
                        float *right, float *bottom)
{
    if (!left || !top || !right || !bottom) return 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        const uint32_t seq1 =
            __atomic_load_n(&snap->seq, __ATOMIC_ACQUIRE);
        if (seq1 & 1u) continue;
        const uint32_t l =
            __atomic_load_n(&snap->rect[0], __ATOMIC_RELAXED);
        const uint32_t t =
            __atomic_load_n(&snap->rect[1], __ATOMIC_RELAXED);
        const uint32_t r =
            __atomic_load_n(&snap->rect[2], __ATOMIC_RELAXED);
        const uint32_t b =
            __atomic_load_n(&snap->rect[3], __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const uint32_t seq2 =
            __atomic_load_n(&snap->seq, __ATOMIC_RELAXED);
        if (seq1 != seq2 || (seq2 & 1u)) continue;

        const float fl = dt_float(l);
        const float ft = dt_float(t);
        const float fr = dt_float(r);
        const float fb = dt_float(b);
        if (!(fr > fl) || !(fb > ft)) return 0;
        *left = fl;
        *top = ft;
        *right = fr;
        *bottom = fb;
        return 1;
    }
    return 0;
}

static void dt_portrait_publish(float left, float top, float right,
                                float bottom)
{
    dt_rect_publish(&g_portrait_snapshot, left, top, right, bottom);
}

static void dt_minimap_publish(float left, float top, float right,
                               float bottom)
{
    dt_rect_publish(&g_minimap_snapshot, left, top, right, bottom);
}

static void dt_minimap_panel_publish(float left, float top, float right,
                                     float bottom)
{
    dt_rect_publish(&g_minimap_panel_snapshot, left, top, right, bottom);
}

static void dt_beacon_publish(float left, float top, float right,
                              float bottom)
{
    dt_rect_publish(&g_beacon_snapshot, left, top, right, bottom);
}

static void dt_command_publish(float left, float top, float right,
                               float bottom)
{
    dt_rect_publish(&g_command_snapshot, left, top, right, bottom);
}

static void dt_grid_publish(float left, float top, float right,
                            float bottom)
{
    dt_rect_publish(&g_grid_snapshot, left, top, right, bottom);
}

static void dt_inventory_publish(float left, float top, float right,
                                 float bottom)
{
    dt_rect_publish(&g_inventory_snapshot, left, top, right, bottom);
}

int wc3_hle_direct_touch_portrait_rect(float *left, float *top,
                                       float *right, float *bottom)
{
    return dt_rect_read(&g_portrait_snapshot, left, top, right, bottom);
}

int wc3_hle_direct_touch_minimap_rect(float *left, float *top,
                                      float *right, float *bottom)
{
    return dt_rect_read(&g_minimap_snapshot, left, top, right, bottom);
}

int wc3_hle_direct_touch_minimap_input_rect(float *left, float *top,
                                            float *right, float *bottom)
{
    float ml = 0.0f;
    float mt = 0.0f;
    float mr = 0.0f;
    float mb = 0.0f;
    if (!dt_rect_read(&g_minimap_snapshot, &ml, &mt, &mr, &mb)) return 0;

    /* The published panel frame is authoritative: it includes the button row
     * above the map image, which the console-derived rect missed. */
    float pl = 0.0f;
    float pt = 0.0f;
    float pr = 0.0f;
    float pb = 0.0f;
    if (dt_rect_read(&g_minimap_panel_snapshot, &pl, &pt, &pr, &pb) &&
        wc3_dt_minimap_panel_plausible(pl, pt, pr, pb, ml, mt, mr, mb)) {
        *left = pl;
        *top = pt;
        *right = pr;
        *bottom = pb;
        return 1;
    }

    float cl = 0.0f;
    float ct = 0.0f;
    float cr = 0.0f;
    float cb = 0.0f;
    const int console_valid =
        dt_rect_read(&g_command_snapshot, &cl, &ct, &cr, &cb);
    if (!wc3_dt_minimap_input_rect(ml, mt, mr, mb, console_valid, ct,
                                   left, top, right, bottom))
        return 0;

    /* Extend the gate to the MinimapSignal button.  Its frame is resolved by
     * native ID, so this is exact geometry rather than a margin guess; the
     * adjacency check rejects a stray frame with the same ID. */
    float bl = 0.0f;
    float bt = 0.0f;
    float br = 0.0f;
    float bb = 0.0f;
    if (dt_rect_read(&g_beacon_snapshot, &bl, &bt, &br, &bb) &&
        wc3_dt_minimap_beacon_plausible(bl, bt, br, bb, ml, mt, mr, mb)) {
        wc3_dt_minimap_input_union(left, top, right, bottom,
                                   bl, bt, br, bb);
    }
    return 1;
}

int wc3_hle_direct_touch_beacon_rect(float *left, float *top,
                                     float *right, float *bottom)
{
    return dt_rect_read(&g_beacon_snapshot, left, top, right, bottom);
}

int wc3_hle_direct_touch_command_rect(float *left, float *top,
                                      float *right, float *bottom)
{
    return dt_rect_read(&g_command_snapshot, left, top, right, bottom);
}

int wc3_hle_direct_touch_grid_rect(float *left, float *top,
                                   float *right, float *bottom)
{
    return dt_rect_read(&g_grid_snapshot, left, top, right, bottom);
}

int wc3_hle_direct_touch_inventory_rect(float *left, float *top,
                                        float *right, float *bottom)
{
    return dt_rect_read(&g_inventory_snapshot, left, top, right, bottom);
}

/*
 * Read a frame subobject rect in surface-normalised form.  The native layout
 * builds the rect from anchor fractions; depending on the UI path it is
 * either already 0..1 or in guest client pixels.  Detect the pixel form by
 * its magnitude, normalise it against the guest client size, and clamp both
 * forms to the surface.
 *
 * The CFrame rect at +0x44 is stored as {top, left, bottom, right}, not as
 * the Win32 {left, top, right, bottom}.  The native hit test 0x6F1111C0
 * compares x against +0x48/+0x50 and y against +0x44/+0x4C; 0x6F074010
 * builds the pick event with x at +0x24 and y at +0x28 (scaled by 0.8/0.6
 * for the 800x600 game space), and 0x6F385B80 passes +0x24 as the first
 * Pick argument.  Reading the fields in Win32 order transposes every HUD
 * rectangle and makes the minimap, portrait, grid and command gates miss.
 */
static int dt_frame_rect_read(uint32_t frame, uint32_t width,
                              uint32_t height, float *out_left,
                              float *out_top, float *out_right,
                              float *out_bottom)
{
    if (!dt_guest_readable(frame, DT_FRAME_RECT + 20u)) return 0;

    float t = dt_float(wc3_rd32g(frame + DT_FRAME_RECT + 0u));
    float l = dt_float(wc3_rd32g(frame + DT_FRAME_RECT + 4u));
    float b = dt_float(wc3_rd32g(frame + DT_FRAME_RECT + 8u));
    float r = dt_float(wc3_rd32g(frame + DT_FRAME_RECT + 12u));
    if (!isfinite(l) || !isfinite(t) || !isfinite(r) || !isfinite(b) ||
        !(r > l) || !(b > t)) {
        return 0;
    }
    if (r > 1.5f || b > 1.5f) {
        if (width <= 1u || height <= 1u) return 0;
        l /= (float)width;
        r /= (float)width;
        t /= (float)height;
        b /= (float)height;
    }
    l = dt_clamp01(l);
    t = dt_clamp01(t);
    r = dt_clamp01(r);
    b = dt_clamp01(b);
    if (!(r > l) || !(b > t)) return 0;
    /* The guest frame rect is stored in the native bottom-left space (the
     * native event producer's y origin is bottom-left), while the overlay
     * touch coordinates are top-left.  Report the rect in the overlay space
     * so every HUD hit-test and the published controls gates compare the
     * same y axis. */
    *out_left = l;
    *out_top = 1.0f - b;
    *out_right = r;
    *out_bottom = 1.0f - t;
    return 1;
}

/* Same rect, but only when the frame is visible.  The portrait, minimap and
 * selected-unit grid use this; inventory slot buttons are laid out and
 * clickable even when an individual slot has no item, so their lookup reads
 * the rect without the visibility gate. */
static int dt_frame_rect_snapshot(uint32_t frame, uint32_t width,
                                  uint32_t height, float *out_left,
                                  float *out_top, float *out_right,
                                  float *out_bottom)
{
    if (!dt_guest_readable(frame, DT_FRAME_RECT + 20u) ||
        wc3_rd32g(frame + DT_FRAME_VISIBLE) == 0) {
        return 0;
    }
    return dt_frame_rect_read(frame, width, height, out_left, out_top,
                              out_right, out_bottom);
}

/* The command card, inventory and selected-unit grid are console-space
 * frames; the minimap, portrait and beacon are full-surface frames.  These
 * wrappers apply the console scale to the former so hit-tests and synthetic
 * clicks use surface fractions, the same axis as the touch events. */
static int dt_console_rect_read(uint32_t frame, uint32_t width,
                                uint32_t height, float *out_left,
                                float *out_top, float *out_right,
                                float *out_bottom)
{
    if (!dt_frame_rect_read(frame, width, height, out_left, out_top,
                            out_right, out_bottom)) {
        return 0;
    }
    wc3_dt_console_rect_scale(out_left, out_top, out_right, out_bottom);
    return 1;
}

static int dt_console_rect_snapshot(uint32_t frame, uint32_t width,
                                    uint32_t height, float *out_left,
                                    float *out_top, float *out_right,
                                    float *out_bottom)
{
    if (!dt_frame_rect_snapshot(frame, width, height, out_left, out_top,
                                out_right, out_bottom)) {
        return 0;
    }
    wc3_dt_console_rect_scale(out_left, out_top, out_right, out_bottom);
    return 1;
}

/*
 * Publish the visible portrait frame.  The portrait handler ignores the
 * frame while [portrait+0x260] is non-zero, so an empty rect keeps direct
 * touch from claiming a portrait the game itself would not follow.
 */
static void dt_portrait_snapshot(uint32_t ui, uint32_t width, uint32_t height)
{
    if (!ui || !dt_guest_readable(ui + DT_UI_PORTRAIT, 4u)) {
        dt_portrait_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const uint32_t portrait = wc3_rd32g(ui + DT_UI_PORTRAIT);
    if (!portrait ||
        !dt_guest_readable(portrait, DT_PORTRAIT_OFF + 4u) ||
        wc3_rd32g(portrait + DT_PORTRAIT_OFF) != 0) {
        /* The native handler treats a non-zero flag as "portrait off"
         * (0x6F3C203F); do not claim a frame the game itself ignores. */
        dt_portrait_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float l = 0.0f;
    float t = 0.0f;
    float r = 0.0f;
    float b = 0.0f;
    /* CFrame+0x44 is the game's fixed 0.8x0.6 DDC space for every HUD frame
     * (0x6F193532), not a surface fraction.  Apply the same scale as the
     * command card and the signal button; publishing the raw rect would put
     * the portrait on top of the Minimap* button column. */
    if (!dt_console_rect_snapshot(portrait + DT_PORTRAIT_FRAME, width, height,
                                  &l, &t, &r, &b)) {
        dt_portrait_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    dt_portrait_publish(l, t, r, b);
}

/*
 * Publish the visible minimap frame.  [CGameUI+0x3C0] is the CMinimap
 * object; the visible frame is its +0xB4 subobject, anchored to the minimap
 * container at CGameUI+0x41C.  The primary object's +0x44 rect is not the
 * sized minimap frame, so read the subobject, matching the portrait layout.
 */
static void dt_minimap_snapshot(uint32_t ui, uint32_t width, uint32_t height)
{
    if (!ui || !dt_guest_readable(ui + DT_UI_MINIMAP, 4u)) {
        dt_minimap_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const uint32_t minimap = wc3_rd32g(ui + DT_UI_MINIMAP);
    if (!minimap) {
        dt_minimap_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float l = 0.0f;
    float t = 0.0f;
    float r = 0.0f;
    float b = 0.0f;
    /* The map image is a DDC-space HUD frame like the signal button.  Its
     * raw +0x44 rect must be scaled to surface fractions; without the scale
     * the input gate is narrower and lower than the visible map, so panel
     * edges and the button column leak into the FIELD route. */
    if (!dt_console_rect_snapshot(minimap + DT_MINIMAP_FRAME, width, height,
                                  &l, &t, &r, &b)) {
        dt_minimap_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    dt_minimap_publish(l, t, r, b);
}

/*
 * The native camera handler maps CGameUI+0x41C (the full panel), not the
 * +0xB4 map image.  The panel also hosts the MinimapSignal/Terrain/Colors/
 * Creeps buttons above the image; those presses must be claimed before the
 * field recognizer can turn them into a long press.  Read the panel rect from
 * the frame itself instead of deriving it from the command bar top.
 */
static void dt_minimap_panel_snapshot(uint32_t ui, uint32_t width,
                                      uint32_t height)
{
    if (!ui || !dt_guest_readable(ui + DT_UI_MINIMAP_CONTAINER, 4u)) {
        dt_minimap_panel_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const uint32_t panel = wc3_rd32g(ui + DT_UI_MINIMAP_CONTAINER);
    float l = 0.0f;
    float t = 0.0f;
    float r = 0.0f;
    float b = 0.0f;
    /* CGameUI+0x41C points at the panel object, not at a CFrame: like the
     * CMinimap object it embeds its frame at +0xB4.  The panel is a layout
     * container, so its own visible flag can be clear even though its rect
     * is live; wc3_dt_minimap_panel_plausible() gates the result. */
    if (panel &&
        dt_console_rect_read(panel + DT_MINIMAP_FRAME, width, height,
                             &l, &t, &r, &b) &&
        r > l && b > t) {
        dt_minimap_panel_publish(l, t, r, b);
        return;
    }
    if (panel &&
        dt_console_rect_snapshot(panel, width, height, &l, &t, &r, &b) &&
        r > l && b > t) {
        dt_minimap_panel_publish(l, t, r, b);
        return;
    }
    dt_minimap_panel_publish(0.0f, 0.0f, 0.0f, 0.0f);
}

/* CGameUI+0x3C0 is the live CMinimap.  The world->screen matrix used by
 * AddSignal is at +0x750.  Require the whole matrix window so a half-built
 * object is not mistaken for a usable minimap. */
static uint32_t dt_minimap_object(void)
{
    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui || !dt_guest_readable(ui + DT_UI_MINIMAP, 4u)) return 0;
    const uint32_t minimap = wc3_rd32g(ui + DT_UI_MINIMAP);
    if (!minimap ||
        !dt_guest_readable(minimap,
                           DT_MINIMAP_MATRIX + DT_MINIMAP_MATRIX_SIZE)) {
        return 0;
    }
    return minimap;
}

static int dt_beacon_signal_rect(uint32_t width, uint32_t height,
                                 float *left, float *top, float *right,
                                 float *bottom)
{
    if (!dt_minimap_object()) return 0;

    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui || !dt_guest_readable(ui + DT_UI_SIGNAL_BUTTON, 4u)) return 0;

    /* The native constructor 0x6F36D890 stores the MinimapSignal button as a
     * CFrame directly in CGameUI+0x408; the signal textures are applied to
     * that frame at 0x6F36D9A1..D9EA.  It hangs off the +0x3D4 button
     * container under the +0x428 console frame, not off the minimap image
     * (+0xB4) or the +0x41C panel.  Walking those trees with a control-ID
     * scan only matched a false 0x47 deep in the world tree, and doing it on
     * every pointer event stalled the guest thread. */
    const uint32_t button = wc3_rd32g(ui + DT_UI_SIGNAL_BUTTON);
    if (!button) return 0;

    /* Keep a stale button pointer from claiming a menu/loading touch after
     * the HUD has gone away. */
    float ml = 0.0f;
    float mt = 0.0f;
    float mr = 0.0f;
    float mb = 0.0f;
    if (!dt_minimap_native_rect(&ml, &mt, &mr, &mb)) return 0;

    /* CFrame+0x44 stores the rect in the game's 0.8x0.6 DDC space, with Y
     * growing up from the bottom of the root frame.  dt_console_rect_snapshot
     * flips it and applies the native 0.8/0.6 scale, giving the same surface
     * fractions the touch events use. */
    const int ok = dt_console_rect_snapshot(button, width, height,
                                            left, top, right, bottom);
#if WC3_DEBUG && WC3_DEBUG_INPUT
    {
        static uint32_t last_button = 0;
        if (button != last_button || !ok) {
            last_button = button;
            dt_log("beacon-frame button=%08X ui=%08X rect=%d "
                   "(%.4f,%.4f..%.4f,%.4f)",
                   button, ui, ok, *left, *top, *right, *bottom);
        }
    }
#endif
    return ok;
}

/*
 * Publish the visible signal button.  The controls layer forces a touch that
 * starts on it to the raw NATIVE route so a beacon gesture never enters the
 * field recognizer, even when the button overlaps the field rectangle.
 */
static void dt_beacon_snapshot(uint32_t width, uint32_t height)
{
    float l = 0.0f;
    float t = 0.0f;
    float r = 0.0f;
    float b = 0.0f;
    if (!dt_beacon_signal_rect(width, height, &l, &t, &r, &b)) {
        dt_beacon_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    dt_beacon_publish(l, t, r, b);
}

/*
 * Publish the visible command bar in surface coordinates.  The card's top
 * row overlaps the field rectangle (the card starts at 0.7817 and the field
 * ends at 0.85), so the controls layer must claim a press inside the card
 * for the raw NATIVE route before the field recognizer can turn it into a
 * world tap.  CFrame+0x44 is console-local; dt_console_rect_read() applies
 * the 0.8x0.6 transform and returns surface fractions.
 */
static void dt_command_snapshot(uint32_t ui, uint32_t width, uint32_t height)
{
    if (!ui || !dt_guest_readable(ui + DT_UI_COMMAND_BAR, 4u)) {
        dt_command_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const uint32_t bar = wc3_rd32g(ui + DT_UI_COMMAND_BAR);
    float l = 0.0f;
    float t = 0.0f;
    float r = 0.0f;
    float b = 0.0f;
    if (!bar || !dt_console_rect_read(bar, width, height, &l, &t, &r, &b)) {
        dt_command_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    dt_command_publish(l, t, r, b);
}

/*
 * Resolve the active grid's rows, columns and row array.  The controls gate
 * snapshot and the pointer hit test share this check so they cannot disagree
 * about whether the panel is live and visible.
 */
static int dt_grid_layout(uint32_t ui, uint32_t *rows_out,
                          uint32_t *cols_out, uint32_t *row_array_out)
{
    if (!ui || !dt_guest_readable(ui + DT_UI_INFO_BAR, 4u)) return 0;
    const uint32_t infobar = wc3_rd32g(ui + DT_UI_INFO_BAR);
    if (!infobar || !dt_guest_readable(infobar + DT_INFO_GROUP, 4u)) return 0;
    const uint32_t group = wc3_rd32g(infobar + DT_INFO_GROUP);
    if (!group || !dt_guest_readable(group, DT_GROUP_ACTIVE_GRID + 4u) ||
        !wc3_rd32g(group + DT_GROUP_VISIBLE)) {
        return 0;
    }
    const uint32_t grid = wc3_rd32g(group + DT_GROUP_ACTIVE_GRID);
    if (!grid || !dt_guest_readable(grid, DT_GRID_ROW_ARRAY + 4u) ||
        !wc3_rd32g(grid + DT_GRID_VISIBLE)) {
        return 0;
    }
    const uint32_t rows = wc3_rd32g(grid + DT_GRID_ROWS);
    const uint32_t cols = wc3_rd32g(grid + DT_GRID_COLS);
    if (!rows || !cols || rows > DT_GRID_MAX_ROWS ||
        cols > DT_GRID_MAX_COLS) {
        return 0;
    }
    const uint32_t row_array = wc3_rd32g(grid + DT_GRID_ROW_ARRAY);
    if (!row_array ||
        !dt_guest_readable(row_array, rows * DT_GRID_ROW_STRIDE)) {
        return 0;
    }
    *rows_out = rows;
    *cols_out = cols;
    *row_array_out = row_array;
    return 1;
}

/*
 * Publish the union of the visible selected-unit cell rectangles.  The grid
 * frame's own +0x44 rect is not used here: the cells are the clickable
 * geometry, and their union is exactly the area the controls layer must keep
 * out of the field recognizer.  A hidden group/grid publishes an empty rect.
 */
static void dt_grid_snapshot(uint32_t ui, uint32_t width, uint32_t height)
{
    uint32_t rows = 0;
    uint32_t cols = 0;
    uint32_t row_array = 0;
    if (!dt_grid_layout(ui, &rows, &cols, &row_array)) {
        dt_grid_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    int have = 0;
    for (uint32_t row_index = 0; row_index < rows; row_index++) {
        const uint32_t row = row_array + row_index * DT_GRID_ROW_STRIDE;
        const uint32_t buttons = wc3_rd32g(row + DT_GRID_ROW_BUTTONS);
        if (!buttons || !dt_guest_readable(buttons, cols * 4u)) continue;
        for (uint32_t col = 0; col < cols; col++) {
            const uint32_t button = wc3_rd32g(buttons + col * 4u);
            /* The gate claims every visible cell, including one whose unit
             * field is currently stale: the hit test will reject that cell,
             * but the press must still not fall through to the field. */
            if (!button ||
                !dt_guest_readable(button, DT_GRID_BUTTON_VISIBLE + 4u) ||
                !wc3_rd32g(button + DT_GRID_BUTTON_VISIBLE)) {
                continue;
            }
            float bl = 0.0f;
            float bt = 0.0f;
            float br = 0.0f;
            float bb = 0.0f;
            if (!dt_console_rect_read(button, width, height,
                                      &bl, &bt, &br, &bb)) {
                continue;
            }
            if (!have) {
                left = bl;
                top = bt;
                right = br;
                bottom = bb;
                have = 1;
            } else {
                if (bl < left) left = bl;
                if (bt < top) top = bt;
                if (br > right) right = br;
                if (bb > bottom) bottom = bb;
            }
        }
    }
    if (!have) {
        dt_grid_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    dt_grid_publish(left, top, right, bottom);
}

/*
 * Publish the union of the inventory slot-button rectangles.  Empty slots
 * keep their buttons and rects, so the gate also claims a press on an empty
 * slot instead of letting the field recognizer turn it into a world click.
 * The bar's own frame rect is not used: the buttons are the clickable
 * geometry and their union is what dt_inventory_slot_at() hit-tests.
 */
static void dt_inventory_snapshot(uint32_t ui, uint32_t width,
                                  uint32_t height)
{
    if (!ui || !dt_guest_readable(ui + DT_UI_INFO_BAR, 4u)) {
        dt_inventory_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const uint32_t infobar = wc3_rd32g(ui + DT_UI_INFO_BAR);
    if (!infobar || !dt_guest_readable(infobar + DT_INFO_INVENTORY, 4u)) {
        dt_inventory_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const uint32_t bar = wc3_rd32g(infobar + DT_INFO_INVENTORY);
    if (!bar || !dt_guest_readable(bar, DT_INV_ARRAY + 4u)) {
        dt_inventory_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const uint32_t cap = wc3_rd32g(bar + DT_INV_CAP);
    const uint32_t array = wc3_rd32g(bar + DT_INV_ARRAY);
    if (!cap || cap > DT_INV_MAX_SLOTS || !array ||
        !dt_guest_readable(array, cap * 8u)) {
        dt_inventory_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    int have = 0;
    for (uint32_t i = 0; i < cap; i++) {
        const uint32_t button =
            wc3_rd32g(array + i * 8u + DT_INV_ENTRY_BUTTON);
        if (!button ||
            !dt_guest_readable(button, DT_INV_BUTTON_DATA + 4u)) {
            continue;
        }
        float bl = 0.0f;
        float bt = 0.0f;
        float br = 0.0f;
        float bb = 0.0f;
        if (!dt_console_rect_read(button, width, height,
                                  &bl, &bt, &br, &bb)) {
            continue;
        }
        if (!have) {
            left = bl;
            top = bt;
            right = br;
            bottom = bb;
            have = 1;
        } else {
            if (bl < left) left = bl;
            if (bt < top) top = bt;
            if (br > right) right = br;
            if (bb > bottom) bottom = bb;
        }
    }
    if (!have) {
        dt_inventory_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    dt_inventory_publish(left, top, right, bottom);
}

#if WC3_DEBUG && WC3_DEBUG_INPUT
static void dt_trace_one_rect(const char *where, const char *name,
                              const DtRectSnapshot *snap,
                              float last[4], int *last_present)
{
    float l = 0.0f;
    float t = 0.0f;
    float r = 0.0f;
    float b = 0.0f;
    const int present = dt_rect_read(snap, &l, &t, &r, &b);
    int changed = present != *last_present;
    if (!changed && present) {
        changed = l != last[0] || t != last[1] ||
                  r != last[2] || b != last[3];
    }
    if (!changed) return;
    *last_present = present;
    if (present) {
        last[0] = l;
        last[1] = t;
        last[2] = r;
        last[3] = b;
    }
    dt_log("rects[%s] %s=%d(%.4f,%.4f..%.4f,%.4f)",
           where, name, present, l, t, r, b);
}

/* The beacon and command rectangles are the prime suspects for a field
 * touch being forced to NATIVE.  Log every change of the four input-thread
 * snapshots so the log shows whether either grew over the battlefield. */
static void dt_trace_snapshots(const char *where)
{
    static float portrait[4];
    static float minimap[4];
    static float minimap_input[4];
    static float beacon[4];
    static float command[4];
    static float grid[4];
    static float inventory[4];
    static int portrait_present = -1;
    static int minimap_present = -1;
    static int minimap_input_present = -1;
    static int beacon_present = -1;
    static int command_present = -1;
    static int grid_present = -1;
    static int inventory_present = -1;

    dt_trace_one_rect(where, "portrait", &g_portrait_snapshot,
                      portrait, &portrait_present);
    dt_trace_one_rect(where, "minimap", &g_minimap_snapshot,
                      minimap, &minimap_present);
    {
        float l = 0.0f;
        float t = 0.0f;
        float r = 0.0f;
        float b = 0.0f;
        const int present =
            wc3_hle_direct_touch_minimap_input_rect(&l, &t, &r, &b);
        int changed = present != minimap_input_present;
        if (!changed && present) {
            changed = l != minimap_input[0] || t != minimap_input[1] ||
                      r != minimap_input[2] || b != minimap_input[3];
        }
        if (changed) {
            minimap_input_present = present;
            if (present) {
                minimap_input[0] = l;
                minimap_input[1] = t;
                minimap_input[2] = r;
                minimap_input[3] = b;
            }
            dt_log("rects[%s] minimap-input=%d(%.4f,%.4f..%.4f,%.4f)",
                   where, present, l, t, r, b);
        }
    }
    dt_trace_one_rect(where, "beacon", &g_beacon_snapshot,
                      beacon, &beacon_present);
    dt_trace_one_rect(where, "command", &g_command_snapshot,
                      command, &command_present);
    dt_trace_one_rect(where, "grid", &g_grid_snapshot,
                      grid, &grid_present);
    dt_trace_one_rect(where, "inventory", &g_inventory_snapshot,
                      inventory, &inventory_present);
}
#endif

/* Defined after the routing snapshot; the pause menu is an exact native UI
 * identity, not a paused-game or geometric guess. */
static int dt_pause_menu_active(void);

/*
 * Pointer-only routing snapshot.  Once the UI singleton is readable the
 * native window exists, so menus, loading screens, HUD and targeting all
 * route to native pointer phases.  Only a missing UI keeps a touch on the
 * legacy overlay path; that is a startup boundary, not a per-feature
 * fallback.  No guest calls.
 */
static uint32_t dt_ui_mode_read(uint32_t ui, uint32_t offset)
{
    if (!ui || !dt_guest_readable(ui + offset, 4u)) return 0;
    return wc3_rd32g(ui + offset);
}

static Wc3DirectTouchRoute dt_route_snapshot(void)
{
    Wc3DirectTouchRoute route = WC3_DIRECT_TOUCH_ROUTE_LEGACY;
    const char *reason = "no-ui-global";
    uint32_t ui = 0;
    uint32_t select_mode = 0;
#if WC3_DEBUG && WC3_DEBUG_INPUT
    /* Debug-only fields: the snapshot decision itself uses select_mode and
     * the click gate, never the active/target modes. */
    uint32_t active_mode = 0;
    uint32_t target_mode = 0;
#endif
    uint32_t world = 0;
    uint32_t manager = 0;
    int world_ready = 0;
    int click_gate = 0;

    /* CEscMenu is a native menu.  It must override the live battlefield
     * snapshot: a point inside the field, minimap or command card is still a
     * menu click while the pause panel is open. */
    if (dt_pause_menu_active()) {
        route = WC3_DIRECT_TOUCH_ROUTE_MENU;
        reason = "pause-menu";
        goto done;
    }

    if (!dt_guest_readable(DT_UI_GLOBAL, 4)) {
        goto done;
    }
    ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui || !dt_guest_readable(ui, DT_UI_WORLD_FRAME + 4u)) {
        reason = "no-ui";
        goto done;
    }

    select_mode = wc3_rd32g(ui + DT_UI_SELECT_MODE);
#if WC3_DEBUG && WC3_DEBUG_INPUT
    active_mode = wc3_rd32g(ui + DT_UI_ACTIVE_MODE);
    target_mode = wc3_rd32g(ui + DT_UI_TARGET_MODE);
#endif
    world = dt_world_from_ui(ui);
    click_gate = wc3_rd32g(ui + DT_UI_CLICK_GATE) != 0;

    /* The physical owner needs a battlefield, not a particular input mode.
     * Target/placement is handled on release.  Without a world or with the
     * click gate closed, native pointer phases are the safe recipient. */
    if (!select_mode || !world || !click_gate) {
        route = WC3_DIRECT_TOUCH_ROUTE_NATIVE;
        reason = !select_mode ? "no-select-mode"
               : !world ? "no-world-frame"
               : "click-gate-closed";
        goto done;
    }

    /* The semantic executor needs the live world and player manager.  If
     * either is in transition, native pointer phases are still safe; do not
     * claim a field gesture that the executor would have to drop. */
    if (!dt_guest_readable(world, DT_WORLD_ALT_GATE + 4u) ||
        !dt_guest_readable(DT_PLAYER_MANAGER, 4) ||
        !(manager = wc3_rd32g(DT_PLAYER_MANAGER))) {
        route = WC3_DIRECT_TOUCH_ROUTE_NATIVE;
        reason = "no-world-or-manager";
        goto done;
    }
    world_ready = 1;

    route = wc3_dt_physical_route(1, world_ready, click_gate);
    reason = route == WC3_DIRECT_TOUCH_ROUTE_FIELD ? "field-ready"
                                                   : "native-recipient";

done:
#if !(WC3_DEBUG && WC3_DEBUG_INPUT)
    (void)reason;
#endif
#if WC3_DEBUG && WC3_DEBUG_INPUT
    {
        static int last_route = -1;
        if ((int)route != last_route) {
            last_route = (int)route;
            dt_log("snapshot route=%s reason=%s ui=%08X select=%08X "
                   "active=%08X target=%08X world_present=%d click_gate=%d "
                   "world=%08X manager=%08X select_active=%d target_active=%d "
                   "active_eq=%d target_eq=%d",
                   dt_route_name(route), reason, ui, select_mode, active_mode,
                   target_mode, world != 0, click_gate, world, manager,
                   select_mode && active_mode == select_mode,
                   target_mode && active_mode == target_mode,
                   active_mode == select_mode,
                   target_mode && active_mode == target_mode);
        }
    }
#endif
    return route;
}

/* hle_guest_alloc() is 8-byte aligned and the guest mapping may live in a
 * 4 GiB host window, so host access always goes through guest_host_ptr().
 * One guest allocation is kept for the whole lifetime of the direct-touch
 * service and handed out as bump slots; the old code allocated, zeroed and
 * freed a 0x100 block for every pick/order/position.  Slots are released in
 * LIFO order, and the offset is rewound at every pump boundary as a backstop
 * for an error path that forgot to free. */
#define DT_SCRATCH_SLOTS 8u
#define DT_SCRATCH_AREA  (DT_SCRATCH_SIZE * DT_SCRATCH_SLOTS)

static uint32_t g_scratch_area;
static uint32_t g_scratch_used;

static void dt_scratch_reset(void)
{
    g_scratch_used = 0;
}

/* A map load can recycle the guest heap the arena came from.  Drop it on a
 * context change so the next allocation is fresh, as it was when every
 * event called hle_guest_alloc() directly. */
static void dt_scratch_invalidate(void)
{
    g_scratch_area = 0;
    g_scratch_used = 0;
}

static uint32_t dt_scratch_alloc(void)
{
    if (!g_scratch_area ||
        !hle_android_guest_ok(g_scratch_area, DT_SCRATCH_AREA)) {
        const uint32_t area = hle_guest_alloc(DT_SCRATCH_AREA);
        if (!area || !hle_android_guest_ok(area, DT_SCRATCH_AREA)) return 0;
        g_scratch_area = area;
        g_scratch_used = 0;
    }
    if (g_scratch_used + DT_SCRATCH_SIZE > DT_SCRATCH_AREA)
        g_scratch_used = 0;

    const uint32_t p = g_scratch_area + g_scratch_used;
    g_scratch_used += DT_SCRATCH_SIZE;
    memset(guest_host_ptr(p), 0, DT_SCRATCH_SIZE);
    return p;
}

static void dt_scratch_free(uint32_t p)
{
    if (!p || p < g_scratch_area) return;
    const uint32_t offset = p - g_scratch_area;
    if (offset + DT_SCRATCH_SIZE == g_scratch_used)
        g_scratch_used = offset;
}

static uint32_t dt_call_thiscall(CPU *c, uint32_t this_va, uint32_t fn,
                                 int argc, const uint32_t *args)
{
    c->ecx = this_va;
    c->edx = 0;
    return wc3_hle_call_guest(c, fn, argc, args);
}

/* Fire the action the player bound to a key.
 *
 * Control-group digits arrive as GROUP_SIGNAL and resolve through the game's
 * own action handler.  No other overlay key has a verified direct guest
 * action: arrows, Enter, Escape and the standalone Shift button stay on the
 * keyboard path in controls.c and are deliberately ignored here.  The old
 * fallback called 0x6F358940 (the named sound-event dispatcher) and the
 * arrow candidate 0x6F35A9F0 is a global game-flag setter, not key state.
 */
static void dt_key_signal(CPU *c, uint32_t key_code, uint32_t down)
{
    (void)c;
    /* Modifier bindings are not guest actions: the game matches a chord by
     * calling GetKeyState(VK_CONTROL/VK_SHIFT/VK_MENU) inside its hotkey
     * matcher (0x6F08B2E0 -> 0x6F08B2FC/F5/EE).  Feed the HLE key state that
     * matcher reads, without posting any keyboard message. */
    if (key_code == 0x10 || key_code == 0x11 || key_code == 0x12 ||
        (key_code >= 0xA0 && key_code <= 0xA5)) {
        wc3_hle_set_modifier_state((int)key_code, down ? 1 : 0);
        dt_trace("modifier", key_code, down);
        return;
    }

    uint32_t arrow_bit = 0;
    switch (key_code) {
    case 0x25: arrow_bit = 0x08u; break; /* VK_LEFT  -> bit 3 */
    case 0x26: arrow_bit = 0x10u; break; /* VK_UP    -> bit 4 */
    case 0x27: arrow_bit = 0x20u; break; /* VK_RIGHT -> bit 5 */
    case 0x28: arrow_bit = 0x40u; break; /* VK_DOWN  -> bit 6 */
    default: break;
    }
    if (arrow_bit) {
        const uint32_t args[1] = { 0u };
        c->ecx = arrow_bit;
        c->edx = down;
        (void)wc3_hle_call_guest(c, DT_FN_SET_KEY_STATE, 1, args);
        dt_trace("key-state", arrow_bit, down);
        return;
    }

    if (!down) return;

    const uint32_t key_byte = dt_scratch_alloc();
    const uint32_t payload = dt_scratch_alloc();
    if (!key_byte || !payload) return;
    memset(guest_host_ptr(key_byte), 0, 4u);
    memset(guest_host_ptr(payload), 0, 12u);
    ((uint8_t *)(uintptr_t)guest_host_ptr(key_byte))[0] = (uint8_t)key_code;

    const uint32_t args[2] = { 0u, 0u };
    c->ecx = key_byte;
    c->edx = payload;
    (void)wc3_hle_call_guest(c, DT_FN_KEY_SIGNAL, 2, args);
    dt_scratch_free(payload);
    dt_scratch_free(key_byte);
    dt_trace("key-signal", key_code, 0);
}

/* Resolve the live CSelectionManager the same way the native control-group
 * handler does: player manager -> local player -> selection. */
static uint32_t dt_group_selection(CPU *c)
{
    if (!dt_guest_readable(DT_PLAYER_MANAGER, 4u)) return 0;
    const uint32_t manager = wc3_rd32g(DT_PLAYER_MANAGER);
    if (!manager || !dt_guest_readable(manager, 0x30u)) return 0;
    const uint32_t args[1] = { wc3_rd16g(manager + 0x28u) };
    c->ecx = manager;
    const uint32_t player = wc3_hle_call_guest(c, DT_FN_PLAYER_BY_ID, 1, args);
    if (!player || !dt_guest_readable(player, 0x38u)) return 0;
    return wc3_rd32g(player + 0x34u);
}

/* The native group hotkey handler assigns the set to the slot in the live
 * selection and then posts the group command.  Both halves are required:
 * posting the command alone leaves the local selection group untouched. */
static void dt_group_assign(CPU *c, uint32_t sel, uint32_t slot,
                            uint32_t set)
{
    const uint32_t args[2] = { slot, set };
    c->ecx = sel;
    (void)wc3_hle_call_guest(c, DT_FN_ASSIGN_GROUP, 2, args);
    c->ecx = slot;
    c->edx = set;
    (void)wc3_hle_call_guest(c, DT_FN_STORE_GROUP, 0, NULL);
}

/* Shift+digit in the native handler builds the union group-first: it copies
 * the stored group into the union set, then appends the current selection,
 * and stores that union back into the same slot.  The order matters when the
 * runtime cap truncates the union: a selection-first merge would store the
 * selection and drop the group, which looks like a plain store.  0x6F268150
 * appends a unit and returns the new element; 0x6F1DBB50 takes that element
 * in ecx and the unit as its stack argument. */
static void dt_group_merge_set(CPU *c, uint32_t dst, uint32_t src)
{
    if (!dt_guest_readable(src, DT_GROUP_SET_ARRAY + 4u) ||
        !dt_guest_readable(dst, DT_GROUP_SET_ARRAY + 4u)) return;
    const uint32_t src_count = wc3_rd32g(src + DT_GROUP_SET_COUNT);
    const uint32_t src_array = wc3_rd32g(src + DT_GROUP_SET_ARRAY);
    const uint32_t dst_count = wc3_rd32g(dst + DT_GROUP_SET_COUNT);
    const uint32_t dst_array = wc3_rd32g(dst + DT_GROUP_SET_ARRAY);
    if (src_count > DT_GROUP_SET_MAX || dst_count > DT_GROUP_SET_MAX ||
        (src_count && (!src_array ||
                       !dt_guest_readable(src_array, src_count * 4u))) ||
        (dst_count && (!dst_array ||
                       !dt_guest_readable(dst_array, dst_count * 4u)))) {
        dt_trace("group-add-bad-count", src_count, dst_count);
        return;
    }

    for (uint32_t i = 0; i < src_count; i++) {
        const uint32_t unit = wc3_rd32g(src_array + i * 4u);
        if (!unit || !dt_guest_readable(unit, 4u)) continue;
        const uint32_t live_count = wc3_rd32g(dst + DT_GROUP_SET_COUNT);
        /* 0x6F268150 reallocates the vector when it grows past its
         * capacity, so the array pointer must be re-read after every add. */
        const uint32_t live_array = wc3_rd32g(dst + DT_GROUP_SET_ARRAY);
        if (live_count >= DT_GROUP_SET_MAX ||
            (live_count && (!live_array ||
                            !dt_guest_readable(live_array, live_count * 4u))))
            break;
        int present = 0;
        for (uint32_t j = 0; j < live_count; j++) {
            if (wc3_rd32g(live_array + j * 4u) == unit) {
                present = 1;
                break;
            }
        }
        if (present) continue;
        const uint32_t add_arg[1] = { unit };
        c->ecx = dst;
        const uint32_t element =
            wc3_hle_call_guest(c, DT_FN_SET_ADD, 1, add_arg);
        if (!element || !dt_guest_readable(element, 4u)) continue;
        const uint32_t retain_arg[1] = { unit };
        c->ecx = element;
        (void)wc3_hle_call_guest(c, DT_FN_SET_RETAIN, 1, retain_arg);
    }
}

/* Resolve one control-group chord.  group is 1..8, action is 0 recall /
 * 1 store / 2 add-to-selection.  The overlay publishes exactly one event for
 * the chord, so nothing here needs the HLE modifier state or a keyboard
 * message. */
static void dt_group_signal(CPU *c, uint32_t group, uint32_t action)
{
    dt_trace("group-signal-enter", group, action);
    if (!group || group > 8u) return;
    const uint32_t slot = group - 1u;

    if (action == 0u) { /* plain digit: recall the group */
        uint32_t ui = 0;
        if (!dt_ui_present(&ui)) {
            dt_trace("group-recall-no-ui", slot, 0);
            return;
        }
        if (!dt_guest_readable(ui + DT_UI_GROUP_MANAGER, 4u)) {
            dt_trace("group-recall-bad-manager-slot", slot, ui);
            return;
        }
        const uint32_t manager = wc3_rd32g(ui + DT_UI_GROUP_MANAGER);
        if (!manager || !dt_guest_readable(manager, 4u)) {
            dt_trace("group-recall-bad-manager", slot, manager);
            return;
        }
        const uint32_t args[1] = { slot };
        c->ecx = manager;
        (void)wc3_hle_call_guest(c, DT_FN_GROUP_RECALL, 1, args);
        dt_trace("group-recall", slot, manager);
        return;
    }

    const uint32_t sel = dt_group_selection(c);
    if (!sel) {
        dt_trace("group-no-selection", slot, action);
        return;
    }
    const uint32_t set = dt_scratch_alloc();
    if (!set) {
        dt_trace("group-no-scratch", slot, action);
        return;
    }

    if (action == 1u) { /* Ctrl: assign the current selection to the group */
        const uint32_t args[1] = { set };
        c->ecx = sel;
        (void)wc3_hle_call_guest(c, DT_FN_EXPORT_SELECT, 1, args);
        dt_trace("group-store", slot, wc3_rd32g(set + DT_GROUP_SET_COUNT));
        dt_group_assign(c, sel, slot, set);
    } else if (action == 2u) { /* Shift: merge the selection into the group */
        const uint32_t selection_set = dt_scratch_alloc();
        if (!selection_set) {
            dt_scratch_free(set);
            return;
        }
        /* Native 0x6F393D50 exports the group into the union set first and
         * then appends the selection.  Keep that order so a capped store
         * truncates the selection tail, not the group. */
        const uint32_t group_args[2] = { slot, set };
        const uint32_t selection_args[1] = { selection_set };
        c->ecx = sel;
        (void)wc3_hle_call_guest(c, DT_FN_EXPORT_GROUP, 2, group_args);
        c->ecx = sel;
        (void)wc3_hle_call_guest(c, DT_FN_EXPORT_SELECT, 1, selection_args);
        /* Trace order matches the union: group count, then selection. */
        dt_trace("group-add-merge", wc3_rd32g(set + DT_GROUP_SET_COUNT),
                 wc3_rd32g(selection_set + DT_GROUP_SET_COUNT));
        dt_group_merge_set(c, set, selection_set);
        dt_scratch_free(selection_set);
        dt_group_assign(c, sel, slot, set);
        dt_trace("group-add", slot, wc3_rd32g(set + DT_GROUP_SET_COUNT));
    }
    dt_scratch_free(set);
}

/*
 * Add a beacon through the native CMinimap::AddSignal.  AddSignal itself
 * applies the world->screen matrix and writes the signal ring, so the caller
 * passes a world point (the same value PingMinimap accepts).  The six dwords
 * mirror the native PingMinimap stack layout: {x, y, 0, z, 0, 0}; AddSignal
 * overwrites the third dword with 1.0f.
 */
static int dt_add_beacon(CPU *c, uint32_t minimap, float world_x,
                         float world_y, float world_z)
{
    if (!c || !minimap ||
        !dt_guest_readable(minimap,
                           DT_MINIMAP_MATRIX + DT_MINIMAP_MATRIX_SIZE)) {
        return 0;
    }
    const uint32_t args[6] = {
        dt_bits(world_x), dt_bits(world_y), 0u, dt_bits(world_z), 0u, 0u
    };
    (void)dt_call_thiscall(c, minimap, DT_FN_ADD_SIGNAL, 6, args);
    return 1;
}

/*
 * Place a beacon at a surface-normalised point on the minimap.  The published
 * minimap rectangle and the local clamp rectangle at +0x644 are both derived
 * from the same CFrame rect, so the release point is mapped fractionally into
 * the local space.  That avoids guessing whether the frame rect is in client
 * pixels or the fixed 800x600 game space.  The minimap's world->screen affine
 * matrix is then inverted; AddSignal re-applies it, so the stored signal
 * lands on the release point (clamped by the game to the minimap rect).
 */
static int dt_beacon_place_minimap(CPU *c, float nx, float ny,
                                   uint32_t width, uint32_t height)
{
    (void)width;
    (void)height;
    const uint32_t minimap = dt_minimap_object();
    if (!minimap) return 0;

    float frame_left = 0.0f;
    float frame_top = 0.0f;
    float frame_right = 0.0f;
    float frame_bottom = 0.0f;
    if (!dt_minimap_native_rect(&frame_left, &frame_top, &frame_right,
                                &frame_bottom)) {
        dt_trace("beacon-no-frame", 0, 0);
        return 0;
    }

    /* CRect at +0x644 is {minY, minX, maxY, maxX} in the DDC space whose Y
     * grows upward; AddSignal clamps sy to [minY, maxY].  Map the surface
     * release through the DDC helper so top stays top. */
    const float local_min_y = dt_float(wc3_rd32g(minimap + 0x644u));
    const float local_min_x = dt_float(wc3_rd32g(minimap + 0x648u));
    const float local_max_y = dt_float(wc3_rd32g(minimap + 0x64Cu));
    const float local_max_x = dt_float(wc3_rd32g(minimap + 0x650u));

    float sx = 0.0f;
    float sy = 0.0f;
    if (!wc3_dt_minimap_ddc_local_point(nx, ny, frame_top, frame_left,
                                        frame_bottom, frame_right,
                                        local_min_y, local_min_x,
                                        local_max_y, local_max_x,
                                        &sx, &sy)) {
        dt_trace("beacon-local-rect", 0, 0);
        return 0;
    }

    float m[9];
    for (uint32_t i = 0; i < 9u; i++) {
        m[i] = dt_float(wc3_rd32g(minimap + DT_MINIMAP_MATRIX + i * 4u));
    }

    float wx = 0.0f;
    float wy = 0.0f;
    if (!wc3_dt_minimap_screen_to_world(m, sx, sy, &wx, &wy)) {
        dt_trace("beacon-matrix", dt_bits(sx), dt_bits(sy));
        return 0;
    }
    if (!dt_add_beacon(c, minimap, wx, wy, 0.0f)) return 0;
    dt_trace("beacon-minimap", dt_bits(wx), dt_bits(wy));
    return 1;
}

static int dt_call_ui_bool(CPU *c, uint32_t ui, uint32_t fn)
{
    return dt_call_thiscall(c, ui, fn, 0, NULL) != 0;
}

static int dt_mode_active(CPU *c, uint32_t ui, uint32_t mode)
{
    if (!mode) return 0;
    return dt_call_thiscall(c, ui, DT_FN_MODE_ACTIVE, 1, &mode) != 0;
}

static int dt_event_gate(CPU *c, uint32_t world, uint32_t event_id)
{
    return dt_call_thiscall(c, world, DT_FN_EVENT_GATE, 1, &event_id) != 0;
}

static int dt_paused(CPU *c)
{
    return wc3_hle_call_guest(c, DT_FN_PAUSED, 0, NULL) != 0;
}

/* Precise CEscMenu visibility flag.  A paused-game predicate is not a
 * substitute for this: the menu can own native clicks independently of a
 * stale target-mode word, and its hot-control state must not leak into a
 * target replay. */
static int dt_pause_menu_active(void)
{
    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui || !dt_guest_readable(ui, DT_UI_ESC_MENU + 4u)) return 0;
    const uint32_t esc = wc3_rd32g(ui + DT_UI_ESC_MENU);
    if (!esc || !dt_guest_readable(esc, DT_ESC_MENU_PANEL + 4u)) return 0;
    const uint32_t panel = wc3_rd32g(esc + DT_ESC_MENU_PANEL);
    if (!panel || !dt_guest_readable(panel, DT_ESC_MENU_PAUSED + 4u)) return 0;
    return wc3_rd32g(panel + DT_ESC_MENU_PAUSED) != 0;
}

/* Reset helpers defined after the route snapshot; entering the pause menu
 * must tear down every game-owned gesture before any menu event is drained. */
static void dt_camera_pan_reset(void);
static void dt_field_reset(void);
static void dt_grid_reset(void);
static void dt_inventory_reset(void);
static void dt_command_reset(void);
static void dt_command_target_reset(void);

/*
 * A pause-menu entry ends every game gesture.  The menu itself uses the same
 * raw native pointer path as the main menu, so field/camera/selection,
 * pending target replays, command/inventory/grid state and the beacon
 * one-shot must not survive into it.  The portrait camera follow is released
 * through the existing deferred slot on the next service call.  No guest
 * calls, so the service entry can run this before draining any event.
 */
static void dt_pause_menu_enter_state(void)
{
    dt_trace("pause-menu-enter", 0, 0);
    dt_grid_reset();
    dt_inventory_reset();
    dt_command_reset();
    dt_command_target_reset();
    dt_field_reset();
    dt_camera_pan_reset();
    /* A field release may have armed its DOWN but not yet reached the idle
     * hover pass.  The pause entry supersedes that gesture; cancel it before
     * it can be forced into the menu or block the next tap. */
    if (g_native_up_pending) {
        dt_native_abort();
        g_native_up_pending = 0;
        g_native_up_wait = 0;
        g_native_up_modal = 0;
    }
    /* A field drag may already have published its native DOWN.  The pause
     * entry ends that world gesture; release the press before the defer
     * state is cleared, otherwise the guest keeps the left button down and
     * the first menu tap is read as a second press. */
    if (g_field_defer_active && g_field_defer_down_posted) dt_native_abort();
    dt_field_click_reset();
    dt_beacon_cancel();
    if (__atomic_load_n(&g_portrait_follow_active, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g_portrait_release_pending, 1, __ATOMIC_RELEASE);
    }
}

/* Exact pause-menu state with a one-time teardown on the false->true edge.
 * The input thread observes the same state as WC3_DIRECT_TOUCH_ROUTE_MENU. */
static int dt_pause_menu_sync(void)
{
    const int active = dt_pause_menu_active();
    if (active && !g_pause_menu_active) dt_pause_menu_enter_state();
    g_pause_menu_active = active;
    return active;
}

/*
 * Reproduce the native input-event conversion.  The overlay reports
 * surface-normalised coordinates with origin top-left; 0x6F074010 copies
 * them into the event and 0x6F8B38A0 scales them by 0.8/0.6 into the
 * pick-event space that 0x6F3840E0 and its viewport gates consume.  The
 * native producer's y origin is bottom-left, so y is inverted here;
 * without the inversion the upper half picks the wrong side of the map.
 */
static int dt_event_point(CPU *c, float nx, float ny, uint32_t width,
                          uint32_t height, uint32_t out)
{
    (void)width;
    (void)height;

    const uint32_t args[2] = { dt_bits(nx), dt_bits(1.0f - ny) };
    c->ecx = out;
    c->edx = out + 4u;
    (void)wc3_hle_call_guest(c, DT_FN_INPUT_COORDS, 2, args);
    return dt_guest_readable(out, 8);
}

static int dt_context_impl(DtContext *ctx, int require_gate)
{
    if (!dt_guest_readable(DT_UI_GLOBAL, 4)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!dt_guest_readable(ui, DT_UI_WORLD_FRAME + 4)) return 0;
    const uint32_t world = dt_world_from_ui(ui);
    if (!world) return 0;
    if (require_gate && !wc3_rd32g(ui + DT_UI_CLICK_GATE)) return 0;

    const uint32_t mode = wc3_rd32g(ui + DT_UI_SELECT_MODE);
    if (!mode) return 0;

    if (!dt_guest_readable(world, DT_WORLD_ALT_GATE + 4)) return 0;

    if (!dt_guest_readable(DT_PLAYER_MANAGER, 4)) return 0;
    const uint32_t manager = wc3_rd32g(DT_PLAYER_MANAGER);
    if (!dt_guest_readable(manager, 0x60)) return 0;

    ctx->ui = ui;
    ctx->world = world;
    ctx->manager = manager;
    ctx->mode = mode;
    return 1;
}

/* The semantic field executor needs the click gate.  Target/placement mode
 * may close that gate while it still owns the next world click, so it gets a
 * gate-free context; the target predicate below decides whether it applies. */
static int dt_context(DtContext *ctx)
{
    return dt_context_impl(ctx, 1);
}

static int dt_target_context(DtContext *ctx)
{
    return dt_context_impl(ctx, 0);
}

/* A live target/placement mode is the one native context whose click gate can
 * be closed.  The gate is not evidence either way: target mode may close it
 * while the active mode still owns the next world click.  Exclude the camera
 * mode; it is also non-select but never confirms a target. */
static int dt_target_mode_live(uint32_t ui)
{
    if (!ui || !dt_guest_readable(ui, DT_UI_CAMERA_MODE + 4u)) return 0;
    const uint32_t select = wc3_rd32g(ui + DT_UI_SELECT_MODE);
    const uint32_t target = wc3_rd32g(ui + DT_UI_TARGET_MODE);
    const uint32_t active = wc3_rd32g(ui + DT_UI_ACTIVE_MODE);
    const uint32_t camera = wc3_rd32g(ui + DT_UI_CAMERA_MODE);
    return wc3_dt_target_mode_live(active, select, target, camera);
}

#if WC3_DEBUG && WC3_DEBUG_INPUT
/* Explain exactly which dt_context() precondition failed.  This runs only
 * when the readiness check is already failing, so it cannot add cost to the
 * normal field path. */
static void dt_trace_context_failure(void)
{
    const int ui_global_ok = dt_guest_readable(DT_UI_GLOBAL, 4u);
    const uint32_t ui = ui_global_ok ? wc3_rd32g(DT_UI_GLOBAL) : 0;
    const int ui_ok = ui && dt_guest_readable(ui, DT_UI_WORLD_FRAME + 4u);
    const uint32_t world_frame =
        ui_ok ? wc3_rd32g(ui + DT_UI_WORLD_FRAME) : 0;
    const uint32_t click_gate =
        ui_ok ? wc3_rd32g(ui + DT_UI_CLICK_GATE) : 0;
    const uint32_t select_mode =
        ui_ok ? wc3_rd32g(ui + DT_UI_SELECT_MODE) : 0;
    const int world_global_ok = dt_guest_readable(DT_WORLD_GLOBAL, 4u);
    const uint32_t world_global =
        world_global_ok ? wc3_rd32g(DT_WORLD_GLOBAL) : 0;
    const uint32_t world = dt_world_from_ui(ui);
    const int world_ok =
        world && dt_guest_readable(world, DT_WORLD_ALT_GATE + 4u);
    const int manager_global_ok = dt_guest_readable(DT_PLAYER_MANAGER, 4u);
    const uint32_t manager =
        manager_global_ok ? wc3_rd32g(DT_PLAYER_MANAGER) : 0;
    const int manager_ok = manager && dt_guest_readable(manager, 0x60u);

    dt_log("ctx-fail ui_global=%d ui=%08X ui_ok=%d world_frame=%08X "
           "click_gate=%08X select_mode=%08X world_global=%d "
           "world_global_val=%08X world=%08X world_ok=%d "
           "manager_global=%d manager=%08X manager_ok=%d",
           ui_global_ok, ui, ui_ok, world_frame, click_gate, select_mode,
           world_global_ok, world_global, world, world_ok, manager_global_ok,
           manager, manager_ok);
}
#endif

/*
 * Track the live UI/world pointers while the queue is empty.  Calling this
 * only when an event is queued would leave g_last_* at the previous map:
 * the first tap after a transition would then look like a stale event and
 * be discarded.  The queue is empty here, so adopting a new pointer cannot
 * rescue a stale event; it only keeps the next comparison honest.
 *
 * Pointer-only: no guest calls, no dispatch.  Runs on the main guest thread
 * through wc3_hle_direct_touch_can_service().
 */
static void dt_sync_idle_context(uint32_t width, uint32_t height)
{
    uint32_t ui = 0;
    uint32_t world = 0;
    if (dt_guest_readable(DT_UI_GLOBAL, 4)) {
        ui = wc3_rd32g(DT_UI_GLOBAL);
        world = dt_world_from_ui(ui);
    }
    /* Keep the input thread's portrait, minimap, beacon and command-card
     * rectangles fresh even while no direct event is queued: the next touch
     * must not hit a stale frame. */
    dt_portrait_snapshot(ui, width, height);
    dt_minimap_snapshot(ui, width, height);
    dt_minimap_panel_snapshot(ui, width, height);
    dt_beacon_snapshot(width, height);
    dt_command_snapshot(ui, width, height);
    dt_grid_snapshot(ui, width, height);
    dt_inventory_snapshot(ui, width, height);
#if WC3_DEBUG && WC3_DEBUG_INPUT
    dt_trace_snapshots("idle");
#endif
    /* The snapshot decides between FIELD, MENU and NATIVE, including menus
     * and loading screens.  Only a missing UI is LEGACY.  The service entry
     * has already synchronized the pause-menu edge before this idle path. */
    dt_publish_route(dt_route_snapshot());
    /* A partial context is a transition in progress.  Keep the previous
     * marker: an event produced now (for example a tap on the loading
     * screen) must still compare against the old map and be dropped. */
#if WC3_DEBUG && WC3_DEBUG_INPUT
    if (!ui || !world) {
        /* Menu/loading: the next world is a new map.  This also re-arms the
         * first-contact probe when a map reload reuses the old pointers. */
        dt_map_probe_clear();
    } else {
        dt_map_probe_arm(ui, world, "idle", 0);
    }
#endif
    if (!ui || !world) return;
    g_last_ui = ui;
    g_last_world = world;
}

/*
 * Exact readiness for the semantic executor.  Native pointer events do not
 * need this context: they only post Win32 mouse messages.  LEGACY here means
 * "no semantic field action", not "send the touch to the overlay engine".
 */
static Wc3DirectTouchRoute dt_ready_route(CPU *c, DtContext *ctx)
{
    Wc3DirectTouchRoute route = WC3_DIRECT_TOUCH_ROUTE_NATIVE;
    const char *reason = "unknown";
    int context_ok = 0;
    int in_game = 0;
    int target_active = 0;
    int select_active = 0;
    int active_eq = 0;
    uint32_t target_mode = 0;
    uint32_t active_mode = 0;
#if WC3_DEBUG && WC3_DEBUG_INPUT
    /* Command-mode words are read only for the debug readiness line. */
    uint32_t cmd_228 = 0;
    uint32_t cmd_238 = 0;
    uint32_t cmd_23c = 0;
    uint32_t cmd_240 = 0;
    int cmd_target = 0;
#endif

    /* The pause menu is a native menu.  It has no semantic executor path:
     * its contacts are raw pointer phases, exactly like the main menu.  The
     * native dispatch path does not need this context, so leave it zero. */
    if (dt_pause_menu_active()) {
        route = WC3_DIRECT_TOUCH_ROUTE_NATIVE;
        reason = "pause-menu";
        goto done;
    }

    if (!dt_context(ctx)) {
        /* Target/placement mode closes the click gate but still owns the
         * next world click.  Keep the route native so the release resolves
         * as a target confirmation instead of an ordinary UI click.  An open
         * pause menu is excluded: the menu owns that click.  Do NOT gate
         * native target replay on dt_paused(): dt_tap() drops semantic world
         * orders while paused, but a native target confirmation is not a
         * semantic order; drag-from-icon already confirms without checking
         * dt_paused().  The original 810af4e native replay had no dt_paused
         * gate either. */
        DtContext target_ctx;
        memset(&target_ctx, 0, sizeof target_ctx);
        if (dt_target_context(&target_ctx) &&
            dt_target_mode_live(target_ctx.ui) &&
            !dt_pause_menu_active() &&
            dt_call_ui_bool(c, target_ctx.ui, DT_FN_IN_GAME)) {
            *ctx = target_ctx;
            route = WC3_DIRECT_TOUCH_ROUTE_NATIVE;
            reason = "target-mode-closed-gate";
            goto done;
        }
        route = WC3_DIRECT_TOUCH_ROUTE_LEGACY;
        reason = "context";
#if WC3_DEBUG && WC3_DEBUG_INPUT
        dt_trace_context_failure();
#endif
        goto done;
    }
    context_ok = 1;
    in_game = dt_call_ui_bool(c, ctx->ui, DT_FN_IN_GAME);
    if (!in_game) {
        route = WC3_DIRECT_TOUCH_ROUTE_LEGACY;
        reason = "not-in-game";
        goto done;
    }

    target_mode = wc3_rd32g(ctx->ui + DT_UI_TARGET_MODE);
    target_active = target_mode && dt_mode_active(c, ctx->ui, target_mode);
    if (target_active) {
        route = WC3_DIRECT_TOUCH_ROUTE_NATIVE;
        reason = "target-mode";
        goto done;
    }

    select_active = dt_mode_active(c, ctx->ui, ctx->mode);
    active_mode = wc3_rd32g(ctx->ui + DT_UI_ACTIVE_MODE);
    active_eq = active_mode == ctx->mode;
#if WC3_DEBUG && WC3_DEBUG_INPUT
    cmd_228 = dt_ui_mode_read(ctx->ui, DT_UI_CMD_MODE_228);
    cmd_238 = dt_ui_mode_read(ctx->ui, DT_UI_CMD_MODE_238);
    cmd_23c = dt_ui_mode_read(ctx->ui, DT_UI_CMD_MODE_23C);
    cmd_240 = dt_ui_mode_read(ctx->ui, DT_UI_CMD_MODE_240);
    cmd_target = wc3_dt_command_target_mode(active_mode, target_mode,
                                            cmd_228, cmd_238, cmd_23c,
                                            cmd_240);
#endif
    if (select_active && active_eq) {
        route = WC3_DIRECT_TOUCH_ROUTE_FIELD;
        reason = "select-mode";
    } else {
        route = WC3_DIRECT_TOUCH_ROUTE_NATIVE;
        reason = select_active ? "active-mode-mismatch"
                               : "select-mode-inactive";
    }

done:
#if !(WC3_DEBUG && WC3_DEBUG_INPUT)
    (void)reason;
    (void)context_ok;
#endif
#if WC3_DEBUG && WC3_DEBUG_INPUT
    {
        const uint32_t ui = ctx ? ctx->ui : 0;
        dt_log("ready route=%s reason=%s ui=%08X world=%08X manager=%08X "
               "mode=%08X target=%08X active=%08X context=%d in_game=%d "
               "target_active=%d select_active=%d active_eq=%d "
               "cmd=%08X/%08X/%08X/%08X cmd_target=%d",
               dt_route_name(route), reason, ui,
               ctx ? ctx->world : 0, ctx ? ctx->manager : 0,
               ctx ? ctx->mode : 0, target_mode, active_mode, context_ok,
               in_game, target_active, select_active, active_eq,
               cmd_228, cmd_238, cmd_23c, cmd_240, cmd_target);
    }
#endif
    return route;
}

static void dt_publish_service_route(Wc3DirectTouchRoute route,
                                     Wc3DirectTouchRoute physical)
{
    /* `route` is the semantic readiness for this pump.  The input thread
     * needs the physical owner instead: target/placement and transient panel
     * modes still live over the battlefield and must be recognized as a field
     * gesture, then resolved on release.  Publishing `route` here recreated
     * the split-route defect whenever a command changed the mode between
     * DOWN and UP. */
    (void)route;
    dt_publish_route(physical);
#if WC3_DEBUG && WC3_DEBUG_INPUT
    {
        static int last_published = -1;
        if ((int)physical != last_published) {
            last_published = (int)physical;
            dt_log("publish-service ready=%s physical=%s",
                   dt_route_name(route), dt_route_name(physical));
        }
    }
#endif
}

/*
 * Mirror CWorldFrameWar3::OnSelect's pick-context initialisation.  A tap
 * selects with a left click when the live selection cannot receive an order,
 * and uses the right-click context when it can.  A long press is the
 * left-click analogue and always uses the left-button context.  The button
 * changes the pick filters and the 0x20/0x04 flags, and therefore the object
 * class returned at pick+0x0C.
 */
static int dt_init_pick(CPU *c, const DtContext *ctx, uint32_t pick,
                        uint32_t button)
{
    const uint32_t ui = ctx->ui;
    const uint32_t world = ctx->world;
    const uint32_t select_gate = wc3_rd32g(world + DT_WORLD_SELECT_GATE);
    const uint32_t alt_gate = wc3_rd32g(world + DT_WORLD_ALT_GATE);

    wc3_wr32g(pick + 0x00u, 0);
    wc3_wr32g(pick + 0x04u, 2);

    if (select_gate & button) {
        uint32_t flags = 2u | 0x210u;
        const uint32_t select_mode = ctx->mode;
        const uint32_t drag_mode = wc3_rd32g(ui + DT_UI_DRAG_MODE);
        const uint32_t alt_mode = wc3_rd32g(ui + DT_UI_ALT_MODE);
        const int drag_active = dt_mode_active(c, ui, drag_mode);
        const int alt_active = dt_mode_active(c, ui, alt_mode);
        const int select_active = dt_mode_active(c, ui, select_mode);

        wc3_wr32g(pick + 0x00u, 1);
        if (button != 4u && !drag_active)
            flags |= 0x20u;
        if (alt_active)
            flags |= 0x84u;
        if (button != 4u && select_active)
            flags |= 0x04u;
        wc3_wr32g(pick + 0x04u, flags);
#if WC3_DEBUG && WC3_DEBUG_INPUT
        dt_log("pick-init select button=%u gate=%08X drag=%08X/%d "
               "alt=%08X/%d select=%08X/%d flags=%08X",
               button, select_gate, drag_mode, drag_active, alt_mode,
               alt_active, select_mode, select_active, flags);
#endif
    } else {
#if WC3_DEBUG && WC3_DEBUG_INPUT
        dt_log("pick-init select-skip button=%u gate=%08X", button,
               select_gate);
#endif
    }

    const int terrain_gate =
        dt_event_gate(c, world, DT_EVENT_ID_TERRAIN_GATE);
    if (terrain_gate && (alt_gate & button)) {
        wc3_wr32g(pick + 0x00u, wc3_rd32g(pick + 0x00u) | 2u);
    }
#if WC3_DEBUG && WC3_DEBUG_INPUT
    dt_log("pick-init terrain button=%u gate=%d alt=%08X pick=%08X "
           "flags=%08X", button, terrain_gate, alt_gate,
           wc3_rd32g(pick + 0x00u), wc3_rd32g(pick + 0x04u));
#endif

    return wc3_rd32g(pick + 0x00u) != 0;
}

/*
 * Pick a world target.  x_bits/y_bits are already in the native pick-event
 * coordinate space (see dt_event_point), not surface-normalised and not
 * client pixels.  Returns 0 when the native path would not pick at all (or
 * when phase 1 cannot represent the active targeting mode), 1 for a
 * unit/sprite and 2 for terrain.  On success the world point is read from the
 * pick context, and object is the raw pick object.  It is borrowed from the
 * pick context: 0x6F395410 reads event+0xC without releasing it, so the
 * executor must not release it either.  0x6F21B730 (ghost) and 0x6F1D73B0
 * (CUnit) create the reference that the executor releases after the action.
 */
static int dt_pick(CPU *c, const DtContext *ctx, uint32_t x_bits,
                   uint32_t y_bits, uint32_t pick, uint32_t button,
                   uint32_t *object, uint32_t *wx_bits, uint32_t *wy_bits,
                   uint32_t *wz_bits)
{
    *object = 0;
    *wx_bits = 0;
    *wy_bits = 0;
    *wz_bits = 0;

    const uint32_t target_mode = wc3_rd32g(ctx->ui + DT_UI_TARGET_MODE);
    if (target_mode && dt_mode_active(c, ctx->ui, target_mode)) {
        dt_trace("targeting-mode", target_mode, 0);
        dt_log("pick blocked target=%08X", target_mode);
        return 0;
    }

    const int init = dt_init_pick(c, ctx, pick, button);
    dt_log("pick init=%d button=%u", init, button);
    if (!init) return 0;

#if WC3_DEBUG && WC3_DEBUG_INPUT
    /* The wrapper rejects before the raycast on its own gates: world+0x108,
     * the viewport rectangle and the pick upper bound at 0x6FABB988.  Dump
     * them so a zero result can be attributed without a second build. */
    if (dt_guest_readable(ctx->world, 0x13Cu) &&
        dt_guest_readable(0x6FABB988u, 4u)) {
        dt_log("pick gates world=%08X +108=%08X +12C=%08X +130=%08X "
               "+134=%08X +138=%08X ny=%.3f max=%.3f",
               ctx->world,
               wc3_rd32g(ctx->world + 0x108u),
               wc3_rd32g(ctx->world + 0x12Cu),
               wc3_rd32g(ctx->world + 0x130u),
               wc3_rd32g(ctx->world + 0x134u),
               wc3_rd32g(ctx->world + 0x138u),
               dt_float(y_bits), dt_float(wc3_rd32g(0x6FABB988u)));
    } else {
        dt_log("pick gates unreadable world=%08X", ctx->world);
    }
#endif

    const uint32_t args[5] = { x_bits, y_bits, pick, 1u, 1u };
    const int r = (int)dt_call_thiscall(c, ctx->world, DT_FN_PICK, 5, args);
    if (r != 1 && r != 2) {
        dt_log("pick result r=%d (no hit)", r);
        return 0;
    }

    *object = wc3_rd32g(pick + 0x0Cu);
    *wx_bits = wc3_rd32g(pick + 0x10u);
    *wy_bits = wc3_rd32g(pick + 0x14u);
    *wz_bits = wc3_rd32g(pick + 0x18u);
    dt_log("pick result r=%d object=%08X world=%.3f,%.3f,%.3f", r, *object,
           dt_float(*wx_bits), dt_float(*wy_bits), dt_float(*wz_bits));
    return r;
}

/*
 * Pick a world point for the beacon gesture.  Unlike dt_pick(), this does not
 * bail out when a targeting mode is active: the beacon button is a UI action,
 * and the native pick context still resolves the point under the release.
 * The object result is ignored; only the world coordinates matter.
 */
static int dt_pick_world(CPU *c, const DtContext *ctx, uint32_t x_bits,
                         uint32_t y_bits, uint32_t pick, uint32_t *wx_bits,
                         uint32_t *wy_bits, uint32_t *wz_bits)
{
    *wx_bits = 0;
    *wy_bits = 0;
    *wz_bits = 0;
    if (!dt_init_pick(c, ctx, pick, 1u)) return 0;

    const uint32_t args[5] = { x_bits, y_bits, pick, 1u, 1u };
    const int r = (int)dt_call_thiscall(c, ctx->world, DT_FN_PICK, 5, args);
    if (r != 1 && r != 2) return 0;

    *wx_bits = wc3_rd32g(pick + 0x10u);
    *wy_bits = wc3_rd32g(pick + 0x14u);
    *wz_bits = wc3_rd32g(pick + 0x18u);
    return r;
}

/*
 * Place a beacon at a surface-normalised battlefield point.  The native pick
 * converts the release point through the same event-coordinate path a mouse
 * click uses, so the beacon lands on the terrain or unit under the finger.
 */
static int dt_beacon_place_field(CPU *c, const DtContext *ctx, float nx,
                                 float ny, uint32_t width, uint32_t height)
{
    const uint32_t minimap = dt_minimap_object();
    if (!minimap || !ctx || !ctx->ui || !ctx->world) return 0;
    if (dt_paused(c)) {
        dt_trace("beacon-field-paused", 0, 0);
        return 0;
    }

    const uint32_t scratch = dt_scratch_alloc();
    if (!scratch) return 0;
    const uint32_t pick = scratch + DT_PICK_OFFSET;
    const uint32_t point = scratch + DT_POINT_OFFSET;
    if (!dt_event_point(c, nx, ny, width, height, point)) {
        dt_scratch_free(scratch);
        return 0;
    }

    uint32_t wx = 0;
    uint32_t wy = 0;
    uint32_t wz = 0;
    const int r = dt_pick_world(c, ctx, wc3_rd32g(point),
                                wc3_rd32g(point + 4u), pick, &wx, &wy, &wz);
    dt_scratch_free(scratch);
    if (!r) {
        dt_trace("beacon-field-pick", 0, 0);
        return 0;
    }

    const float fx = dt_float(wx);
    const float fy = dt_float(wy);
    const float fz = dt_float(wz);
    if (!isfinite(fx) || !isfinite(fy) || !isfinite(fz)) return 0;
    if (!dt_add_beacon(c, minimap, fx, fy, fz)) return 0;
    dt_trace("beacon-field", wx, wy);
    return 1;
}

/*
 * 0x6F21B730 writes a validated, reference-counted object to the guest dword
 * at ecx.  The caller must release it after the action, exactly like
 * CWorldFrameWar3::OnSelect does.
 */
static uint32_t dt_validate_object(CPU *c, uint32_t object, uint32_t out)
{
    if (!object) return 0;
    wc3_wr32g(out, 0);
    (void)dt_call_thiscall(c, out, DT_FN_VALIDATE_OBJECT, 1, &object);
#if WC3_DEBUG && WC3_DEBUG_INPUT
    dt_log("validate ghost object=%08X valid=%08X out=%08X", object,
           wc3_rd32g(out), out);
#endif
    return wc3_rd32g(out);
}

/*
 * 0x6F1D73B0 is the CUnit validator.  It writes a reference-counted CUnit to
 * the guest dword at ecx; the caller releases it after the action.  A pick
 * made with the left-button context returns a CUnit at pick+0x0C, so the
 * selection path must use this validator instead of the CGhostImage one.
 */
static uint32_t dt_validate_unit(CPU *c, uint32_t object, uint32_t out)
{
    if (!object) return 0;
    wc3_wr32g(out, 0);
    (void)dt_call_thiscall(c, out, DT_FN_VALIDATE_UNIT, 1, &object);
#if WC3_DEBUG && WC3_DEBUG_INPUT
    dt_log("validate unit object=%08X valid=%08X out=%08X", object,
           wc3_rd32g(out), out);
#endif
    return wc3_rd32g(out);
}

static void dt_release_object(CPU *c, uint32_t object)
{
    if (!object || !dt_guest_readable(object, 8)) return;

    uint32_t refs = wc3_rd32g(object + 4u);
    if (!refs) return;
    refs--;
    wc3_wr32g(object + 4u, refs);
    if (refs) return;

    const uint32_t vtable = wc3_rd32g(object);
    if (!vtable || !dt_guest_readable(vtable, 4)) return;
    const uint32_t destroy = wc3_rd32g(vtable);
    if (!destroy) return;
    (void)dt_call_thiscall(c, object, destroy, 0, NULL);
}

#if WC3_DEBUG && WC3_DEBUG_INPUT
/*
 * Debug-only object probe for the direct-touch pick path.  0x6F21B730
 * rejects an object when its vtable class id is not a CGhostImage
 * (0x2B776768); a CUnit uses 0x2B773375 and is rejected by design.  The
 * trace already prints the raw pointer; this dump adds the class-id call,
 * the class comparison, the handle lookup and the world-object validator so
 * a rejection can be attributed without another build.  out is a
 * caller-owned 4-byte guest slot for 0x6F1F2A60; pass 0 to skip it.
 */
static void dt_probe_object(CPU *c, const char *label, uint32_t object,
                            uint32_t out)
{
    if (!object) {
        dt_log("probe %s object=00000000", label);
        return;
    }
    if (!dt_guest_readable(object, 0x30u)) {
        dt_log("probe %s object=%08X unreadable", label, object);
        return;
    }

    const uint32_t vtable = wc3_rd32g(object);
    const uint32_t refs = wc3_rd32g(object + 4u);
    const uint32_t f08 = wc3_rd32g(object + 8u);
    const uint32_t f0c = wc3_rd32g(object + 0x0Cu);
    const uint32_t f10 = wc3_rd32g(object + 0x10u);
    const uint32_t f20 = wc3_rd32g(object + 0x20u);
    const uint32_t f28 = wc3_rd32g(object + 0x28u);
    const uint32_t f2c = wc3_rd32g(object + 0x2Cu);
    uint32_t class_fn = 0;
    uint32_t expected_class = DT_CLASS_GHOST_ID;
    uint32_t class_id = 0;
    uint32_t class_ok = 0;
    int class_call = 0;
    uint32_t holder_a = 0;
    uint32_t holder_a_class = 0;
    uint32_t holder_a_flags = 0;
    uint32_t holder_b = 0;
    uint32_t holder_b_class = 0;
    uint32_t holder_b_flags = 0;
    uint32_t world_object = 0;
    int vtable_ok = 0;

    if (vtable >= 0x6F000000u && vtable < 0x70000000u &&
        dt_guest_readable(vtable, 0x20u)) {
        vtable_ok = 1;
        class_fn = wc3_rd32g(vtable + 0x1Cu);
    }

    /* Write the raw fields before any virtual call: if a corrupt candidate
     * faults inside the class probe, the line still identifies it. */
    dt_log("probe %s raw object=%08X vt=%08X refs=%u f08=%08X f0C=%08X "
           "f10=%08X f20=%08X f28=%08X f2C=%08X class_fn=%08X vtable_ok=%d",
           label, object, vtable, refs, f08, f0c, f10, f20, f28, f2c,
           class_fn, vtable_ok);

    if (vtable_ok &&
        class_fn >= 0x6F000000u && class_fn < 0x70000000u &&
        dt_guest_readable(class_fn, 1u)) {
        class_call = 1;
        expected_class = wc3_hle_call_guest(c, DT_FN_CLASS_GHOST_ID, 0, NULL);
        class_id = dt_call_thiscall(c, object, class_fn, 0, NULL);
        c->ecx = class_id;
        c->edx = expected_class;
        class_ok = wc3_hle_call_guest(c, DT_FN_CLASS_IS, 0, NULL) != 0;
    }

    /* 0x6F054530(handle, type) is the fastcall resolver used by the native
     * unit paths.  Try the two plausible pairs; a CUnit does not carry a
     * handle at +0xC, so this also shows whether the pick object is a
     * handle-holder instead. */
    if (f0c || f10) {
        c->ecx = f0c;
        c->edx = f10;
        holder_a = wc3_hle_call_guest(c, DT_FN_HANDLE_LOOKUP, 0, NULL);
        if (holder_a && dt_guest_readable(holder_a, 0x24u)) {
            holder_a_class = wc3_rd32g(holder_a + 0x0Cu);
            holder_a_flags = wc3_rd32g(holder_a + 0x20u);
        }
    }
    if (f28 || f2c) {
        c->ecx = f28;
        c->edx = f2c;
        holder_b = wc3_hle_call_guest(c, DT_FN_HANDLE_LOOKUP, 0, NULL);
        if (holder_b && dt_guest_readable(holder_b, 0x24u)) {
            holder_b_class = wc3_rd32g(holder_b + 0x0Cu);
            holder_b_flags = wc3_rd32g(holder_b + 0x20u);
        }
    }

    if (out && class_call) {
        wc3_wr32g(out, 0);
        const uint32_t world_arg = object;
        (void)dt_call_thiscall(c, out, DT_FN_VALIDATE_WORLD_OBJECT, 1,
                               &world_arg);
        world_object = wc3_rd32g(out);
        /* The validator took its own reference; the probe is not an action,
         * so balance it instead of leaking the object. */
        if (world_object) dt_release_object(c, world_object);
    }

    dt_log("probe %s object=%08X class_call=%d expect=%08X class=%08X "
           "class_ok=%d holderA=%08X hAclass=%08X hAflags=%08X "
           "holderB=%08X hBclass=%08X hBflags=%08X world=%08X",
           label, object, class_call, expected_class, class_id, class_ok,
           holder_a, holder_a_class, holder_a_flags, holder_b,
           holder_b_class, holder_b_flags, world_object);
}
#endif

static uint32_t dt_selection(CPU *c, const DtContext *ctx)
{
    const uint32_t local_id = wc3_rd16g(ctx->manager + 0x28u);
    const uint32_t player = dt_call_thiscall(c, ctx->manager, DT_FN_PLAYER, 1,
                                             &local_id);
    if (!player || !dt_guest_readable(player + 0x34u, 4)) return 0;

    const uint32_t selection = wc3_rd32g(player + 0x34u);
    /* The live selection exposes its count at +0x1F8 through
     * 0x6F290270; 0x6F291240 clears through +0x1AC.  A shorter readable
     * window would let a corrupt pointer pass the guard and fault inside a
     * guest call. */
    if (!selection || !dt_guest_readable(selection, 0x210u)) return 0;
    return selection;
}

/*
 * The portrait stores a handle-holder, not a CUnit*: [holder+0xC] is the JASS
 * handle and [holder+0x10] is its type.  0x6F1EEF90 resolves and validates it
 * exactly like the native GetUnitX/Y path.  Accept a direct CUnit* too, since
 * a fallback selection slot may already be resolved.
 */
static uint32_t dt_handle_unit(CPU *c, uint32_t holder)
{
    if (!holder || !dt_guest_readable(holder, 0x14u)) return 0;
    if (wc3_rd32g(holder + 0x0Cu) == DT_CLASS_UNIT) {
        if (wc3_rd32g(holder + 0x20u) != 0) return 0;
        return holder;
    }

    const uint32_t handle = wc3_rd32g(holder + 0x0Cu);
    if (!handle) return 0;
    const uint32_t unit = dt_call_thiscall(c, handle,
                                           DT_FN_HANDLE_TO_UNIT, 0, NULL);
    if (!unit || !dt_guest_readable(unit, 0x24u)) return 0;
    if (wc3_rd32g(unit + 0x0Cu) != DT_CLASS_UNIT) return 0;
    if (wc3_rd32g(unit + 0x20u) != 0) return 0;
    return unit;
}

/*
 * The unit shown by the portrait, or the primary selection when the portrait
 * has no unit.  A displayed unit that no longer validates is not silently
 * replaced by a different selection: the portrait is the user's target.
 */
static uint32_t dt_portrait_unit(CPU *c, const DtContext *ctx)
{
    uint32_t holder = 0;
    if (dt_guest_readable(ctx->ui + DT_UI_PORTRAIT, 4u)) {
        const uint32_t portrait = wc3_rd32g(ctx->ui + DT_UI_PORTRAIT);
        if (portrait) {
            if (!dt_guest_readable(portrait, DT_PORTRAIT_OFF + 4u) ||
                wc3_rd32g(portrait + DT_PORTRAIT_OFF) != 0) {
                /* The native handler ignores the frame while this flag is
                 * set; do not fall back to the selection and pan anyway. */
                return 0;
            }
            if (dt_guest_readable(portrait, DT_PORTRAIT_UNIT + 4u)) {
                holder = wc3_rd32g(portrait + DT_PORTRAIT_UNIT);
            }
            if (holder) return dt_handle_unit(c, holder);
        }
    }

    const uint32_t selection = dt_selection(c, ctx);
    if (!selection ||
        !dt_guest_readable(selection + DT_SELECTION_PRIMARY, 4u)) {
        return 0;
    }
    holder = wc3_rd32g(selection + DT_SELECTION_PRIMARY);
    return dt_handle_unit(c, holder);
}

static uint32_t dt_camera(uint32_t ui)
{
    if (!ui || !dt_guest_readable(ui + DT_UI_CAMERA, 4u)) return 0;
    const uint32_t camera = wc3_rd32g(ui + DT_UI_CAMERA);
    if (!camera || !dt_guest_readable(camera, DT_CAMERA_READ_SIZE)) return 0;
    return camera;
}

/*
 * CUnit vtable +0xB8 returns the unit's handle-holder subobject (its own
 * vtable is 0x6FAC2F10; handle at +8, type at +0xC).  The native GetUnitX/Y
 * path then calls 0x6F058900(holder, &vec), which resolves the holder and
 * writes the world position as three floats.  Call the virtual instead of
 * reading a fixed offset so a derived unit class keeps its own subobject.
 */
static int dt_unit_position(CPU *c, uint32_t unit, float *x, float *y)
{
    if (!unit || !dt_guest_readable(unit, 0x10u)) return 0;
    const uint32_t vtable = wc3_rd32g(unit);
    if (!vtable ||
        !dt_guest_readable(vtable + DT_UNIT_POSITION_SLOT + 4u, 4u)) {
        return 0;
    }
    const uint32_t fn = wc3_rd32g(vtable + DT_UNIT_POSITION_SLOT);
    if (!fn) return 0;
    const uint32_t holder = dt_call_thiscall(c, unit, fn, 0, NULL);
    if (!holder || !dt_guest_readable(holder, 0x14u)) return 0;

    const uint32_t out = dt_scratch_alloc();
    if (!out) return 0;
    const uint32_t args[1] = { out };
    (void)dt_call_thiscall(c, holder, DT_FN_UNIT_POSITION, 1, args);

    const float px = dt_float(wc3_rd32g(out + 0u));
    const float py = dt_float(wc3_rd32g(out + 4u));
    dt_scratch_free(out);
    if (!isfinite(px) || !isfinite(py)) return 0;
    *x = px;
    *y = py;
    return 1;
}

/*
 * Place a beacon on the unit shown by the portrait.  A button-origin drag
 * that ends over the portrait targets the displayed/primary-selected unit,
 * not the terrain under the portrait frame.  The unit position and AddSignal
 * are the same native paths the portrait pan and minimap beacon use.
 */
static int dt_beacon_place_portrait(CPU *c, const DtContext *ctx)
{
    const uint32_t minimap = dt_minimap_object();
    if (!minimap || !ctx || !ctx->ui) return 0;
    if (dt_paused(c)) {
        dt_trace("beacon-portrait-paused", 0, 0);
        return 0;
    }
    const uint32_t unit = dt_portrait_unit(c, ctx);
    if (!unit) {
        dt_trace("beacon-portrait-no-unit", 0, 0);
        return 0;
    }
    float x = 0.0f;
    float y = 0.0f;
    if (!dt_unit_position(c, unit, &x, &y)) {
        dt_trace("beacon-portrait-no-position", unit, 0);
        return 0;
    }
    if (!dt_add_beacon(c, minimap, x, y, 0.0f)) return 0;
    dt_trace("beacon-portrait", unit, 0);
    return 1;
}

/* Compatibility: a TAP queued by an older build.  The current recognizer
 * starts the follow on DOWN and emits no portrait tap.  The native
 * PanCameraTo path clears an active follow first. */
static void dt_portrait_tap(CPU *c, const DtContext *ctx)
{
    if (dt_paused(c)) {
        dt_trace("portrait-tap-paused", 0, 0);
        return;
    }
    const uint32_t camera = dt_camera(ctx->ui);
    if (!camera) {
        dt_trace("portrait-no-camera", 0, 0);
        return;
    }
    const uint32_t unit = dt_portrait_unit(c, ctx);
    if (!unit) {
        dt_trace("portrait-no-unit", 0, 0);
        return;
    }
    float x = 0.0f;
    float y = 0.0f;
    if (!dt_unit_position(c, unit, &x, &y)) {
        dt_trace("portrait-no-position", unit, 0);
        return;
    }
    const uint32_t args[4] = { dt_bits(x), dt_bits(y), 0u, 0u };
    (void)dt_call_thiscall(c, camera, DT_FN_CAMERA_PAN, 4, args);
    dt_trace("portrait-pan", unit, 0);
}

/* Follow: native camera follow with a zero offset; it takes its own unit ref.
 * Started immediately on portrait DOWN and held until the owning finger is
 * released, not by a long-press deadline. */
static void dt_portrait_follow_start(CPU *c, const DtContext *ctx)
{
    if (dt_paused(c)) {
        dt_trace("portrait-follow-paused", 0, 0);
        return;
    }
    const uint32_t camera = dt_camera(ctx->ui);
    if (!camera) {
        dt_trace("portrait-follow-no-camera", 0, 0);
        return;
    }
    const uint32_t unit = dt_portrait_unit(c, ctx);
    if (!unit) {
        dt_trace("portrait-follow-no-unit", 0, 0);
        return;
    }
    const uint32_t args[5] = { unit, 0u, 0u, 0u, 0u };
    (void)dt_call_thiscall(c, camera, DT_FN_CAMERA_FOLLOW, 5, args);
    g_portrait_follow_active = 1;
    dt_trace("portrait-follow", unit, 0);
}

/* Release: a null unit makes the native follow path drop its reference. */
static void dt_portrait_release(CPU *c)
{
    g_portrait_follow_active = 0;
    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    const uint32_t camera = dt_camera(ui);
    if (!camera) return;
    const uint32_t args[5] = { 0u, 0u, 0u, 0u, 0u };
    (void)dt_call_thiscall(c, camera, DT_FN_CAMERA_FOLLOW, 5, args);
    g_portrait_follow_active = 0;
    dt_trace("portrait-release", 0, 0);
}

static int dt_selection_nonempty(CPU *c, uint32_t selection)
{
    if (!selection) return 0;
    const int count = (int)dt_call_thiscall(c, selection,
                                             DT_FN_SELECTION_COUNT, 0, NULL);
    return count > 0 && count <= 40;
}

#define DT_SELECTION_HEAD       0x000001F4u
#define DT_SELECTION_TOTAL      0x000001F8u
#define DT_SELECTION_LAST_GROUP 0x00000204u
/* CSelectionWar3 leaves +0x1F4 and +0x204 undefined while the selection is
 * empty.  0x6F2928C0, the native path that consumes the selected-unit list,
 * tests +0x1F8 before it reads +0x1F4.  That count is the actual contract;
 * an empty selection has no live head to validate.  Do not turn this into a
 * pointer-range filter: require the native count, then walk the list only
 * when the selection is non-empty. */
static int dt_selection_contains_unit(CPU *c, uint32_t selection,
                                      uint32_t unit)
{
    if (!c || !selection || !unit ||
        !dt_selection_nonempty(c, selection)) {
        return 0;
    }
    uint32_t node = wc3_rd32g(selection + DT_SELECTION_HEAD);
    for (uint32_t i = 0; i < 40u && node; i++) {
        if (!dt_guest_readable(node, 0x0Cu)) return 0;
        if (wc3_rd32g(node + 0x08u) == unit) return 1;
        node = wc3_rd32g(node + 0x04u);
    }
    return 0;
}

#if WC3_DEBUG && WC3_DEBUG_INPUT
/*
 * Post-tap audit.  A field tap posts a native left click; the guest applies
 * the selection later, on its own input pump.  Sampling the live selection at
 * bounded pump boundaries shows whether the first click was consumed and how
 * long its selection took, without adding a timing threshold to the gesture
 * path.  Only the first few map contacts are audited.
 */
static void dt_map_audit_arm(uint32_t unit)
{
    if (g_map_contacts == 0 || g_map_contacts > DT_MAP_AUDIT_CONTACTS)
        return;
    const uint64_t now = dt_now_ms();
    g_map_audit_remaining = DT_MAP_AUDIT_SAMPLES;
    g_map_audit_samples = 0;
    g_map_audit_seen = 0;
    g_map_audit_contact = g_map_contacts;
    g_map_audit_unit = unit;
    g_map_audit_until = now ? now + DT_MAP_AUDIT_MS : 0;
    g_map_audit_last_ms = 0;
    dt_log("map-audit arm contact=%u unit=%08X until=%llu",
           g_map_audit_contact, g_map_audit_unit,
           (unsigned long long)g_map_audit_until);
}

/* sample=0 is the idle path: it only expires an audit whose deadline passed.
 * The sampling below calls guest code, so it runs only from an active pump
 * that already dispatches guest messages. */
static void dt_map_audit_tick(CPU *c, int sample)
{
    if (!g_map_audit_remaining) return;

    const uint64_t now = dt_now_ms();
    if (g_map_audit_until && now && now >= g_map_audit_until) {
        dt_log("map-audit end contact=%u sample=%d reason=timeout",
               g_map_audit_contact, g_map_audit_samples);
        g_map_audit_remaining = 0;
        return;
    }
    if (!sample) return;
    if (now && g_map_audit_last_ms &&
        now - g_map_audit_last_ms < DT_MAP_AUDIT_EVERY_MS) {
        return;
    }
    g_map_audit_last_ms = now;
    g_map_audit_samples++;
    g_map_audit_remaining--;

    DtContext ctx;
    memset(&ctx, 0, sizeof ctx);
    const int ctx_ok = dt_context(&ctx);
    uint32_t selection = 0;
    uint32_t primary = 0;
    int has = 0;
    if (ctx_ok) {
        selection = dt_selection(c, &ctx);
        if (selection) {
            primary = dt_call_thiscall(c, selection,
                                       DT_FN_SELECTION_PRIMARY_UNIT, 0,
                                       NULL);
            has = dt_selection_contains_unit(c, selection,
                                             g_map_audit_unit);
        }
    }

    uint32_t qhead = 0;
    uint32_t qtail = 0;
    const int qok = dt_guest_readable(DT_GAME_QUEUE_BASE, 0x148u);
    if (qok) {
        qhead = wc3_rd32g(DT_GAME_QUEUE_HEAD);
        qtail = wc3_rd32g(DT_GAME_QUEUE_TAIL);
    }

    dt_log("map-audit#%u sample=%d left=%d ctx=%d sel=%08X primary=%08X "
           "expected=%08X has=%d q=%u/%u qok=%d native=%d app_active=%u",
           g_map_audit_contact, g_map_audit_samples, g_map_audit_remaining,
           ctx_ok, selection, primary, g_map_audit_unit, has,
           qhead, qtail, qok, hle_android_native_button_pending(),
           dt_app_active_flag());

    if (has) {
        if (++g_map_audit_seen >= DT_MAP_AUDIT_HOLD) {
            dt_log("map-audit end contact=%u sample=%d reason=selected",
                   g_map_audit_contact, g_map_audit_samples);
            g_map_audit_remaining = 0;
        }
    } else {
        g_map_audit_seen = 0;
    }
}
#endif

/* 0x6F293550 sets CSelectionWar3+0x20C from the last subgroup-list node
 * ([+0x204]+8).  A group selection can leave +0x20C zero, empty, or pointing
 * at a subgroup that is no longer part of the selection; the native order
 * paths (0x6F290240) then resolve a wrong or missing primary and silently
 * issue nothing.  Replicate the native write in those cases.  A populated
 * active subgroup whose first unit is still selected is kept: +0x20C is also
 * the user's active grid subgroup, not just the last one. */
static void dt_selection_ensure_active_group(CPU *c, uint32_t selection)
{
    if (!selection ||
        !dt_guest_readable(selection + DT_SELECTION_ACTIVE_GROUP, 4u))
        return;
    const uint32_t active =
        wc3_rd32g(selection + DT_SELECTION_ACTIVE_GROUP);
    if (active && dt_guest_readable(active, 0x14u)) {
        const uint32_t count = wc3_rd32g(active + 0x10u);
        if (count > 0) {
            const uint32_t index = 0;
            const uint32_t first = dt_call_thiscall(c, active, DT_FN_GROUP_AT,
                                                    1, &index);
            if (first &&
                dt_selection_contains_unit(c, selection, first)) {
                return;
            }
            dt_log("selection active-group stale selection=%08X active=%08X "
                   "first=%08X", selection, active, first);
        } else {
            dt_log("selection active-group empty selection=%08X active=%08X",
                   selection, active);
        }
    }
    if (!dt_guest_readable(selection + DT_SELECTION_LAST_GROUP, 4u)) {
        dt_log("selection active-group no-list selection=%08X active=%08X",
               selection, active);
        return;
    }
    const uint32_t last = wc3_rd32g(selection + DT_SELECTION_LAST_GROUP);
    if ((int32_t)last <= 0 || !dt_guest_readable(last, 0x14u)) {
        dt_log("selection active-group bad-tail selection=%08X active=%08X "
               "last=%08X", selection, active, last);
        return;
    }
    const uint32_t group = last + 8u;
    if (!dt_guest_readable(group, 0x14u)) {
        dt_log("selection active-group bad-group selection=%08X active=%08X "
               "last=%08X group=%08X", selection, active, last, group);
        return;
    }
    wc3_wr32g(selection + DT_SELECTION_ACTIVE_GROUP, group);
    dt_log("selection active-group repaired selection=%08X old=%08X "
           "group=%08X count=%u", selection, active, group,
           wc3_rd32g(group + 0x10u));
}

#if WC3_DEBUG && WC3_DEBUG_INPUT
static uint32_t dt_trace_word(uint32_t va, uint32_t off)
{
    if (!dt_guest_readable(va + off, 4u)) return 0xFFFFFFFFu;
    return wc3_rd32g(va + off);
}

/* Dump the live selection before and after the gate.  The fields are the
 * ones the native order paths use: +0x1F8 is the selected-unit count,
 * +0x1F4 is the selected-unit list head, +0x204 is the last subgroup-list
 * node, and +0x20C is the active subgroup.  +0x1F4 is only defined after
 * the count becomes non-zero; 0x6F2928C0 reads it under that condition.
 * The walk uses the same list shape as 0x6F293FB0/0x6F293D30: node+8 is
 * the CUnit, node+4 is next. */
static void dt_trace_selection_impl(CPU *c, const DtContext *ctx,
                                    const char *where)
{
    const uint32_t selection = dt_selection(c, ctx);
    if (!selection) {
        dt_log("sel[%s] none", where);
        return;
    }
    dt_log("sel[%s] obj=%08X +1E0=%08X +1E4=%08X +1F0=%08X +1F4=%08X "
           "+1F8=%08X +200=%08X +204=%08X +208=%08X +20C=%08X",
           where, selection,
           dt_trace_word(selection, 0x1E0u),
           dt_trace_word(selection, 0x1E4u),
           dt_trace_word(selection, 0x1F0u),
           dt_trace_word(selection, 0x1F4u),
           dt_trace_word(selection, 0x1F8u),
           dt_trace_word(selection, 0x200u),
           dt_trace_word(selection, 0x204u),
           dt_trace_word(selection, 0x208u),
           dt_trace_word(selection, 0x20Cu));

    const uint32_t total = dt_trace_word(selection, DT_SELECTION_TOTAL);
    if (!total) {
        /* +0x20C, +0x1F4 and +0x204 are not list state yet.  Reading them
         * here was the first-tap fault: +0x1F4 held reused float bytes while
         * the native count was zero. */
        dt_log("sel[%s] empty count=0", where);
        return;
    }

    const uint32_t active = dt_trace_word(selection, DT_SELECTION_ACTIVE_GROUP);
    const uint32_t primary = dt_call_thiscall(c, selection,
                                              DT_FN_SELECTION_PRIMARY_UNIT, 0,
                                              NULL);
    const uint32_t selected = dt_call_thiscall(c, selection,
                                               DT_FN_SELECTION_PRIMARY_SELECTED,
                                               0, NULL);
    dt_log("sel[%s] primary_unit=%08X primary_selected=%08X active=%08X",
           where, primary, selected, active);

    if (active && dt_guest_readable(active, 0x14u)) {
        const uint32_t index = 0;
        const uint32_t first = dt_call_thiscall(c, active, DT_FN_GROUP_AT, 1,
                                                &index);
        dt_log("sel[%s] active=%08X count=%u first=%08X", where, active,
               wc3_rd32g(active + 0x10u), first);
    }

    uint32_t node = dt_trace_word(selection, DT_SELECTION_HEAD);
    for (uint32_t i = 0; i < 40u && node; i++) {
        if (!dt_guest_readable(node, 0x0Cu)) {
            dt_log("sel[%s] node[%u]=%08X invalid", where, i, node);
            break;
        }
        const uint32_t unit = wc3_rd32g(node + 0x08u);
        if (!unit || !dt_guest_readable(unit, 0x60u)) {
            dt_log("sel[%s] node[%u]=%08X unit=%08X unreadable", where, i,
                   node, unit);
        } else {
            const uint32_t cls = wc3_rd32g(unit + 0x0Cu);
            const uint32_t owner = wc3_rd16g(unit + 0x58u);
            const uint32_t flags = wc3_rd32g(unit + 0x5Cu);
            const int can = dt_call_thiscall(c, unit, DT_FN_UNIT_CONTROLLABLE,
                                             0, NULL) != 0;
            dt_log("sel[%s] unit[%u]=%08X node=%08X cls=%08X owner=%u "
                   "flags=%08X can=%d", where, i, unit, node, cls, owner,
                   flags, can);
        }
        node = wc3_rd32g(node + 0x04u);
    }
}
#define dt_trace_selection(c, ctx, where) \
    dt_trace_selection_impl((c), (ctx), (where))
#else
#define dt_trace_selection(c, ctx, where) ((void)0)
#endif

/*
 * True when the live selection can receive an order from the local player.
 * Both native order paths resolve the primary through 0x6F290240
 * (CSelectionWar3+0x20C, the active subgroup's first unit), so the gate
 * evaluates that same unit.  +0x1E0 (0x6F28FFE0) is the refcounted primary
 * selected CUnit, but neither 0x6F394680 nor 0x6F393018 reads it; accepting a
 * selection on +0x1E0 would let the tap issue an order that the native path
 * then rejects.  0x6F66EC60 is a CUnit predicate (owner at +0x58, flags at
 * +0x5C, CUnit vtable +0xEC), not a CSelectionWar3 predicate.
 *
 * 0x6F66EC60 is necessary but not sufficient.  Device log 2026-11-27 showed
 * a selected neutral Goblin Ammo Dump (ngad, owner 15) returning can_order=1;
 * tapping egol then issued an order even though the selection was not under
 * local control.  The owner gate keeps neutral slots select-only.
 */
static int dt_selection_controlled(CPU *c, const DtContext *ctx)
{
    const uint32_t selection = dt_selection(c, ctx);
    if (!selection || !dt_selection_nonempty(c, selection)) {
        dt_log("controlled selection=%08X nonempty=0", selection);
        return 0;
    }
    dt_selection_ensure_active_group(c, selection);

    const uint32_t primary = dt_call_thiscall(c, selection,
                                              DT_FN_SELECTION_PRIMARY_UNIT, 0,
                                              NULL);
    /* The selection list stores CUnit objects; 0x2B61676C is the class id
     * of the handle-holder resolved by 0x6F054530, not of the CUnit at
     * +0xC (that field is the unit type).  The native controllable
     * predicate is necessary, but owner 12..15 (neutral) is select-only. */
    if (!primary || !dt_guest_readable(primary, 0x60u)) {
        dt_log("controlled selection=%08X primary=%08X readable=0",
               selection, primary);
        return 0;
    }
    const uint32_t owner = wc3_rd16g(primary + 0x58u);
    const int native_controllable =
        dt_call_thiscall(c, primary, DT_FN_UNIT_CONTROLLABLE, 0, NULL) != 0;
    const int can_order =
        wc3_dt_selection_can_order(owner, native_controllable);
    dt_log("controlled selection=%08X primary=%08X owner=%u "
           "native_controllable=%d can_order=%d",
           selection, primary, owner, native_controllable, can_order);
    return can_order;
}

/*
 * True when a single CUnit can receive an order from the local player.
 * 0x6F66EC60 is the same authority dt_selection_controlled() uses for the
 * primary unit; it accounts for shared control.  A left pick is the only
 * pick context that returns a CUnit, so this check is possible only after a
 * left pick.  A null or unreadable unit is not controllable.
 */
static int dt_unit_controlled(CPU *c, uint32_t unit)
{
    if (!unit || !dt_guest_readable(unit, 0x60u)) return 0;
    return dt_call_thiscall(c, unit, DT_FN_UNIT_CONTROLLABLE, 0, NULL) != 0;
}

/*
 * Native GetUnitTypeId 0x6F2040C0 resolves the JASS handle through
 * 0x6F1EEF90 and returns dword [CUnit+0x30].  dt_validate_unit already
 * produced the validated CUnit, so this reads the same rawcode directly.
 * The value is a unit-type rawcode (for example 'ugol'), not a pointer.
 */
static uint32_t dt_unit_type_id(uint32_t unit)
{
    if (!unit || !dt_guest_readable(unit, 0x34u)) return 0;
    return wc3_rd32g(unit + 0x30u);
}

static void dt_clear_selection(CPU *c, const DtContext *ctx)
{
    const uint32_t selection = dt_selection(c, ctx);
    if (!selection) return;
    /* 0x6F291240 takes the value 0, not a pointer to it.  The native
     * callers push an immediate zero; passing &mode would pass a non-zero
     * guest address into the mode branch. */
    const uint32_t args[1] = { 0u };
    (void)dt_call_thiscall(c, selection, DT_FN_CLEAR_SELECTION, 1, args);
}

/*
 * Selected-unit grid helpers.  The native panel only reacts to a completed
 * button click (command 0x50065) and that click arrives on release.  Direct
 * touch activates the subgroup as soon as the finger lands and follows the
 * finger across the grid; in targeting mode the same down is delivered to the
 * target-mode 0x50065 handler, which validates the unit and applies the
 * target.  No synthetic button event is posted.
 */
static void dt_grid_reset(void)
{
    g_grid.active = 0;
    g_grid.pointer = 0;
    g_grid.target = 0;
    g_grid.moved = 0;
    g_grid.ui = 0;
    g_grid.selection = 0;
    g_grid.unit = 0;
    g_grid.down_unit = 0;
}

/*
 * Resolve the live UI for the grid without a guest call.  A modal menu clears
 * [ui+0x1AC]; in that state the grid is not a valid recipient even if its
 * frames are still visible underneath.  Hit-testing a cell only needs this
 * half of the context, so it must not wait on player/selection resolution.
 */
static int dt_grid_ui(uint32_t *ui_out)
{
    if (!ui_out) return 0;
    *ui_out = 0;

    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui || !dt_guest_readable(ui, DT_UI_INFO_BAR + 4u)) return 0;
    if (!wc3_rd32g(ui + DT_UI_WORLD_FRAME)) return 0;
    if (!wc3_rd32g(ui + DT_UI_CLICK_GATE)) return 0;
    if (!wc3_rd32g(ui + DT_UI_SELECT_MODE)) return 0;

    *ui_out = ui;
    return 1;
}

/* Selection is only needed when a subgroup is actually switched.  Keeping it
 * out of the hit test removes a guest call from every DOWN/MOVE/UP and stops
 * a transient player lookup from swallowing an otherwise valid cell press. */
static int dt_grid_context(CPU *c, uint32_t ui, uint32_t *selection_out)
{
    if (!c || !ui || !selection_out) return 0;
    *selection_out = 0;

    if (!dt_guest_readable(DT_PLAYER_MANAGER, 4u)) return 0;
    const uint32_t manager = wc3_rd32g(DT_PLAYER_MANAGER);
    if (!manager || !dt_guest_readable(manager, 0x60u)) return 0;

    DtContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ui = ui;
    ctx.manager = manager;
    const uint32_t selection = dt_selection(c, &ctx);
    if (!selection) return 0;

    *selection_out = selection;
    return 1;
}

static uint32_t dt_grid_target_mode(uint32_t ui)
{
    if (!ui || !dt_guest_readable(ui, DT_UI_TARGET_MODE + 4u)) return 0;
    const uint32_t target = wc3_rd32g(ui + DT_UI_TARGET_MODE);
    if (!target) return 0;
    return wc3_rd32g(ui + DT_UI_ACTIVE_MODE) == target ? target : 0;
}

static int dt_grid_targeting(uint32_t ui)
{
    return dt_grid_target_mode(ui) != 0;
}

/* Return the CUnit under the normalised surface point, or 0 when the point is
 * outside the active grid or the cell has no live unit. */
static uint32_t dt_grid_button_at(CPU *c, uint32_t ui, float nx, float ny,
                                  uint32_t width, uint32_t height)
{
    uint32_t rows = 0;
    uint32_t cols = 0;
    uint32_t row_array = 0;
    if (!c || !dt_grid_layout(ui, &rows, &cols, &row_array)) return 0;

    for (uint32_t row_index = 0; row_index < rows; row_index++) {
        const uint32_t row =
            row_array + row_index * DT_GRID_ROW_STRIDE;
        const uint32_t buttons = wc3_rd32g(row + DT_GRID_ROW_BUTTONS);
        if (!buttons || !dt_guest_readable(buttons, cols * 4u)) continue;

        for (uint32_t col = 0; col < cols; col++) {
            const uint32_t button = wc3_rd32g(buttons + col * 4u);
            if (!button ||
                !dt_guest_readable(button, DT_GRID_BUTTON_UNIT + 4u)) {
                continue;
            }
            if (!wc3_rd32g(button + DT_GRID_BUTTON_VISIBLE)) continue;

            float left = 0.0f;
            float top = 0.0f;
            float right = 0.0f;
            float bottom = 0.0f;
            /* CInfoPanelGroupButton uses +0x90 for visibility, not the
             * CFrame +0x54 flag checked by dt_console_rect_snapshot().  The
             * +0x90 check above is authoritative; read the rect without the
             * unrelated +0x54 gate. */
            if (!dt_console_rect_read(button, width, height, &left, &top,
                                      &right, &bottom)) {
                continue;
            }
            if (!(nx >= left && nx < right && ny >= top && ny < bottom)) {
                continue;
            }

            const uint32_t ref = wc3_rd32g(button + DT_GRID_BUTTON_UNIT);
            /* The native fill path and the target-mode 0x50065 handler both
             * skip a holder with [holder+0x5C] & 0x100.  Keep the same rule
             * here so a stale visible button cannot select a unit the game
             * already hides. */
            if (!ref || !dt_guest_readable(ref, 0x60u)) continue;
            if (wc3_rd32g(ref + 0x5Cu) & 0x100u) continue;
            const uint32_t unit = dt_handle_unit(c, ref);
            if (unit) return unit;
        }
    }
    return 0;
}

#if WC3_DEBUG && WC3_DEBUG_INPUT
/* Explain a failed cell hit test.  The controls gate already claimed the
 * point from the published rect union, so the only remaining reasons are a
 * layout mismatch, a rect mismatch, or a cell whose unit field cannot be
 * resolved.  Keep this out of the success path. */
static void dt_grid_dump_miss(CPU *c, uint32_t ui, float nx, float ny,
                              uint32_t width, uint32_t height)
{
    uint32_t rows = 0;
    uint32_t cols = 0;
    uint32_t row_array = 0;
    if (!dt_grid_layout(ui, &rows, &cols, &row_array)) {
        dt_log("grid-miss-ui ui=%08X layout=0", ui);
        return;
    }
    dt_log("grid-miss-ui ui=%08X rows=%u cols=%u row_array=%08X n=%.4f,%.4f",
           ui, rows, cols, row_array, nx, ny);
    for (uint32_t r = 0; r < rows; r++) {
        const uint32_t row = row_array + r * DT_GRID_ROW_STRIDE;
        const uint32_t buttons = wc3_rd32g(row + DT_GRID_ROW_BUTTONS);
        if (!buttons || !dt_guest_readable(buttons, cols * 4u)) {
            dt_log("grid-miss-row r=%u buttons=%08X", r, buttons);
            continue;
        }
        for (uint32_t col = 0; col < cols; col++) {
            const uint32_t button = wc3_rd32g(buttons + col * 4u);
            if (!button ||
                !dt_guest_readable(button, DT_GRID_BUTTON_UNIT + 4u)) {
                dt_log("grid-miss-cell r=%u c=%u btn=%08X unreadable", r, col,
                       button);
                continue;
            }
            float l = 0.0f;
            float t = 0.0f;
            float rr = 0.0f;
            float b = 0.0f;
            const int rect_ok = dt_console_rect_read(button, width, height,
                                                     &l, &t, &rr, &b);
            const int inside = rect_ok && nx >= l && nx < rr &&
                               ny >= t && ny < b;
            const uint32_t ref = wc3_rd32g(button + DT_GRID_BUTTON_UNIT);
            const uint32_t flags = ref && dt_guest_readable(ref, 0x60u)
                ? wc3_rd32g(ref + 0x5Cu) : 0u;
            const uint32_t unit = inside && ref ? dt_handle_unit(c, ref) : 0u;
            dt_log("grid-miss-cell r=%u c=%u btn=%08X vis=%u "
                   "rect=%d(%.4f,%.4f..%.4f,%.4f) inside=%d "
                   "ref=%08X flags=%08X unit=%08X",
                   r, col, button,
                   wc3_rd32g(button + DT_GRID_BUTTON_VISIBLE),
                   rect_ok, l, t, rr, b, inside, ref, flags, unit);
        }
    }
}
#endif

/* The native 0x50065 handler resolves the selection holder from the touched
 * unit as [[unit+0x6C]+0x124] and validates it with 0x6F0549F0.  The grid
 * button stores a unit holder, not the class instance, so dt_grid_button_at()
 * resolves it first and this function derives the same holder the native
 * handler uses. */
static uint32_t dt_grid_holder(CPU *c, uint32_t unit)
{
    if (!c || !unit ||
        !dt_guest_readable(unit + DT_UNIT_HOLDER_SUB + 4u, 4u)) {
        return 0;
    }
    const uint32_t sub = wc3_rd32g(unit + DT_UNIT_HOLDER_SUB);
    if (!sub ||
        !dt_guest_readable(sub + DT_UNIT_HOLDER_REF + 4u, 4u)) {
        return 0;
    }
    const uint32_t holder = wc3_rd32g(sub + DT_UNIT_HOLDER_REF);
    if (!holder || !dt_guest_readable(holder, 0x14u)) return 0;
    if (!dt_call_thiscall(c, holder, DT_FN_HOLDER_ALIVE, 0, NULL)) return 0;
    return holder;
}

/* Native single-unit selection: 0x6F396CF0(manager, holder, 0, 0, 0).  The
 * manager is [CGameUI+0x24C], exactly what the native 0x50065 branch uses. */
static void dt_grid_select_unit(CPU *c, uint32_t ui, uint32_t holder)
{
    if (!c || !ui || !holder) return;
    if (!dt_guest_readable(ui + DT_UI_MANAGER, 4u)) return;
    const uint32_t manager = wc3_rd32g(ui + DT_UI_MANAGER);
    if (!manager || !dt_guest_readable(manager, 0x60u)) return;

    const uint32_t args[4] = { holder, 0u, 0u, 0u };
    (void)dt_call_thiscall(c, manager, DT_FN_GRID_SELECT_UNIT, 4, args);
    dt_trace("grid-select-unit", holder, manager);
}

/* The native 0x50065 subgroup branch closes command-bar pages and hides the
 * old command buttons before 0x6F293480, then marks the UI dirty after
 * 0x6F291240.  Without that the command card can keep the previous page's
 * buttons after a direct-touch subgroup switch.  The native assert helper
 * 0x6F359350 is debug-only, and the target-mode cancel before the hide cannot
 * fire here because dt_grid_targeting() already routed that case.  The native
 * pop loop is unbounded; cap it so a corrupt mode count cannot hang the
 * guest. */
static void dt_grid_prepare_command_bar(CPU *c, uint32_t ui)
{
    if (!c || !ui || !dt_guest_readable(ui + DT_UI_COMMAND_BAR, 4u)) return;
    const uint32_t bar = wc3_rd32g(ui + DT_UI_COMMAND_BAR);
    if (!bar || !dt_guest_readable(bar, 0x1C0u)) return;

    for (int i = 0; i < 8; i++) {
        if (!dt_call_thiscall(c, bar, DT_FN_COMMAND_BAR_POP, 0, NULL)) break;
    }
    (void)dt_call_thiscall(c, bar, DT_FN_COMMAND_BAR_HIDE, 0, NULL);
}

/* Make the unit's subgroup current without changing the selection.  The
 * native 0x50065 handler uses 0x6F293480 for this and follows it with
 * 0x6F291240(selection, 0); the realize alone is only a mirror update.  The
 * command bar is reset first because the old page's buttons belong to the
 * previous subgroup.  The native selection-option overrides are not
 * reproduced: this is the standard panel path. */
static void dt_grid_switch(CPU *c, uint32_t ui, uint32_t selection,
                           uint32_t unit)
{
    if (!c || !selection || !unit) return;

    const uint32_t holder = dt_grid_holder(c, unit);
    if (!holder) return;
    if (!dt_guest_readable(selection + DT_SELECTION_ACTIVE_GROUP, 4u)) return;

    const uint32_t active =
        wc3_rd32g(selection + DT_SELECTION_ACTIVE_GROUP);
    if (active && dt_guest_readable(active, 0x10u)) {
        const uint32_t arg = holder;
        if (dt_call_thiscall(c, active, DT_FN_SUBGROUP_HAS_UNIT, 1, &arg)) {
            /* Already the active subgroup: the touch only selects the unit
             * on release, so there is no command-bar rebuild here. */
            return;
        }
    }

    const uint32_t arg = holder;
    dt_grid_prepare_command_bar(c, ui);
    (void)dt_call_thiscall(c, selection, DT_FN_GRID_ACTIVATE, 1, &arg);
    const uint32_t mode = 0;
    (void)dt_call_thiscall(c, selection, DT_FN_SELECTION_REALIZE, 1, &mode);
    (void)dt_call_thiscall(c, 0u, DT_FN_UI_DIRTY, 0, NULL);
    dt_trace("grid-switch", holder,
             wc3_rd32g(selection + DT_SELECTION_ACTIVE_GROUP));
}

/* Target-mode 0x50065: event+0xC is the CUnit, event+0x10 is an optional
 * world-position pointer (null lets the handler use the unit position). */
static void dt_grid_target(CPU *c, uint32_t target_mode, uint32_t unit)
{
    if (!c || !target_mode || !unit) return;

    const uint32_t scratch = dt_scratch_alloc();
    if (!scratch) return;
    const uint32_t event = scratch + DT_EVENT_OFFSET;
    memset(guest_host_ptr(event), 0, 0x14u);
    wc3_wr32g(event + 0x08u, DT_EVENT_ID_GRID_TARGET);
    wc3_wr32g(event + 0x0Cu, unit);
    wc3_wr32g(event + 0x10u, 0u);
    const uint32_t args[1] = { event };
    (void)dt_call_thiscall(c, target_mode, DT_FN_TARGET_UNIT, 1, args);
    dt_scratch_free(scratch);
    dt_trace("grid-target", unit, target_mode);
}

/*
 * Consume a native-route pointer gesture that started on the selected-unit
 * grid.  Returns 1 when the event belongs to the grid and must not be posted
 * as a native mouse message.  Only the pointer that landed on the grid keeps
 * ownership; the rest of the gesture is consumed even after it leaves the
 * buttons, so the native UI never sees a MOVE/UP without its DOWN.
 */
static int dt_grid_pointer(CPU *c, const Wc3DirectTouchEvent *ev,
                           uint32_t width, uint32_t height)
{
    if (!c || !ev || ev->route != WC3_DIRECT_TOUCH_ROUTE_NATIVE) return 0;

    switch (ev->kind) {
    case WC3_DIRECT_TOUCH_POINTER_DOWN: {
        if (g_grid.active) return 1;

        /* Hit-test first.  Selection resolution is a guest call and is not
         * needed to decide whether this press belongs to the grid; doing it
         * first made a transient player lookup swallow valid cell presses. */
        uint32_t ui = 0;
        if (!dt_grid_ui(&ui)) {
#if WC3_DEBUG && WC3_DEBUG_INPUT
            dt_log("grid-ui-fail n=%.4f,%.4f", ev->x, ev->y);
#endif
            return 0;
        }
        const uint32_t unit =
            dt_grid_button_at(c, ui, ev->x, ev->y, width, height);
        if (!unit) {
#if WC3_DEBUG && WC3_DEBUG_INPUT
            dt_grid_dump_miss(c, ui, ev->x, ev->y, width, height);
#endif
            return 0;
        }

        const uint32_t target = dt_grid_target_mode(ui);
        uint32_t selection = 0;
        if (!target && !dt_grid_context(c, ui, &selection)) {
            /* The cell is still owned by the grid: the release can select
             * the unit through the manager without a selection pointer. */
            dt_trace("grid-no-selection", unit, ui);
        }

        g_grid.active = 1;
        g_grid.pointer = ev->pointer_id;
        g_grid.ui = ui;
        g_grid.selection = selection;
        g_grid.unit = unit;
        g_grid.down_unit = unit;
        g_grid.moved = 0;
        g_grid.target = 0;
#if WC3_DEBUG && WC3_DEBUG_INPUT
        wc3_hle_direct_touch_probe_begin("grid-select", unit, selection);
#endif

        if (target) {
            dt_grid_target(c, target, unit);
            g_grid.target = 1;
        } else {
            /* The subgroup under the finger becomes current immediately.
             * If the finger then crosses into another cell, MOVE switches
             * again; the cell under the finger is always the highlighted
             * subgroup. */
            dt_grid_switch(c, ui, selection, unit);
        }
        dt_trace("grid-down", unit, g_grid.target);
        return 1;
    }
    case WC3_DIRECT_TOUCH_POINTER_MOVE: {
        if (!g_grid.active || ev->pointer_id != g_grid.pointer) return 0;
        if (g_grid.target) return 1;

        const uint32_t unit =
            dt_grid_button_at(c, g_grid.ui, ev->x, ev->y, width, height);
        if (!unit) {
            /* Leaving the cells makes this a drag, not a tap, even if the
             * finger returns to the original cell before release. */
            g_grid.moved = 1;
            return 1;
        }
        if (unit != g_grid.unit) {
            g_grid.moved = 1;
            g_grid.unit = unit;
            if (!g_grid.selection) {
                /* A failed player lookup at DOWN must not permanently
                 * disable subgroup switching for the rest of the drag. */
                (void)dt_grid_context(c, g_grid.ui, &g_grid.selection);
            }
            dt_grid_switch(c, g_grid.ui, g_grid.selection, unit);
            dt_trace("grid-move", unit, 0);
        }
        return 1;
    }
    case WC3_DIRECT_TOUCH_POINTER_UP:
    case WC3_DIRECT_TOUCH_POINTER_CANCEL:
        if (!g_grid.active || ev->pointer_id != g_grid.pointer) return 0;
        if (ev->kind == WC3_DIRECT_TOUCH_POINTER_UP) {
            /* Re-hit-test the release point only while the gesture can still
             * become a tap.  A target press or a crossed cell has already
             * decided the gesture, so a hit test there would be wasted guest
             * calls.  Android may deliver a UP at a different coordinate
             * without an intervening MOVE, so a release outside the starting
             * cell must still be classified as a drag. */
            const int tap_possible = !g_grid.target && !g_grid.moved;
            const uint32_t up_unit = tap_possible
                ? dt_grid_button_at(c, g_grid.ui, ev->x, ev->y,
                                    width, height)
                : 0;
            if (wc3_dt_grid_release_select(g_grid.target, g_grid.moved,
                                           up_unit == g_grid.down_unit)) {
                const uint32_t holder = dt_grid_holder(c, g_grid.unit);
                if (holder) dt_grid_select_unit(c, g_grid.ui, holder);
                dt_trace("grid-select", g_grid.unit, 0);
            }
        }
        dt_trace("grid-up", g_grid.unit, g_grid.target);
        dt_grid_reset();
        return 1;
    default:
        return 0;
    }
}

/* Returns the native 0x6F394680 result: 1 when an order was issued, 0 when
 * the native path rejected the selection or the point. */
static uint32_t dt_order_ground(CPU *c, const DtContext *ctx,
                                uint32_t wx_bits, uint32_t wy_bits)
{
    const uint32_t args[2] = { wx_bits, wy_bits };
    return dt_call_thiscall(c, ctx->mode, DT_FN_GROUND_ORDER, 2, args);
}

/*
 * Build the same CGhostSpriteClickEvent OnSelect builds for a unit, then let
 * the native dispatcher validate the target and issue the smart order.  The
 * event lives only on our guest scratch and is never posted to a queue.
 *
 * event+0x38 must be the raw pick object, not our validated reference: the
 * handler validates it again with 0x6F21B730 and takes its own reference,
 * exactly as the native OnSelect path does.  Our reference, when the caller
 * holds one for target inspection, only keeps the raw object alive.
 *
 * Returns the 0x6F392FA0 result: non-zero when the handler accepted the
 * selection and issued the order, 0 when it rejected the selection (paused,
 * no active-subgroup primary, or the primary is not controllable).  The tap
 * uses 0 only to decide whether a native right-click replay is worth trying.
 */
static uint32_t dt_order_unit(CPU *c, const DtContext *ctx, uint32_t scratch,
                              uint32_t object, uint32_t candidate,
                              uint32_t wx_bits, uint32_t wy_bits,
                              uint32_t wz_bits)
{
    const uint32_t event = scratch + DT_EVENT_OFFSET;
    memset(guest_host_ptr(event), 0, DT_EVENT_SIZE);

    const uint32_t event_id = DT_EVENT_ID_GHOST;
    (void)dt_call_thiscall(c, event, DT_FN_GHOST_INIT, 1, &event_id);

    wc3_wr32g(event + 0x1Cu, wx_bits);
    wc3_wr32g(event + 0x20u, wy_bits);
    wc3_wr32g(event + 0x24u, wz_bits);
    wc3_wr32g(event + 0x28u, 4u);              /* right button: smart order */
    wc3_wr32g(event + 0x34u, candidate);
    wc3_wr32g(event + 0x38u, object);

    return dt_call_thiscall(c, ctx->mode, DT_FN_CLICK_DISPATCH, 1, &event);
}

/*
 * A native right-click fallback is safe only while the contact still belongs
 * to the battlefield.  A pick or an order can cross a mode change, so the
 * fallback re-checks the live context instead of leaking a click into a menu,
 * a target/placement mode or a HUD panel.
 */
static int dt_world_click_allowed(CPU *c, const DtContext *ctx,
                                  const Wc3DirectTouchEvent *ev)
{
    if (!c || !ctx || !ctx->ui || !ctx->world) return 0;
    if (dt_paused(c)) return 0;
    if (dt_field_target_active(c, ctx)) return 0;
    if (!dt_field_contains(ev->x, ev->y)) return 0;
    if (dt_field_hud_contains(ev->x, ev->y)) return 0;
    return 1;
}

/*
 * Result of dt_smart_order_at().  The caller uses it only for tracing; the
 * native handlers have already accepted or rejected the order.
 */
typedef enum {
    DT_ORDER_NONE = 0,
    DT_ORDER_SELECT,
    DT_ORDER_UNIT,
    DT_ORDER_GROUND,
    DT_ORDER_ATTACK_MOVE,
    DT_ORDER_FALLBACK
} DtOrderResult;

/*
 * Issue one order at the contact point.  Tap and the order leg of a long tap
 * share this executor; a long tap on a controllable exception target is
 * selected by dt_long_tap_at() before this function runs:
 *   - a right-button pick yields a CGhostImage for a hostile/neutral unit
 *     or decoration, a CUnit for a controllable unit, or a world point for
 *     terrain;
 *   - a validated ghost is a smart order through 0x6F392FA0;
 *   - a CUnit (the ghost validation fails) and any other failed pick use
 *     the native right click, which is the game's own fallback;
 *   - terrain is a right-click smart move for tap and the legacy attack-move
 *     (A + left click) for long tap.
 * The native right-click fallback is used only while the live context still
 * owns a world click.
 */
static DtOrderResult dt_smart_order_at(CPU *c, const DtContext *ctx,
                                       const Wc3DirectTouchEvent *ev,
                                       uint32_t width, uint32_t height,
                                       int attack_move)
{
    if (!c || !ctx || !ctx->ui || !ctx->world) return DT_ORDER_NONE;
    if (dt_paused(c)) {
        dt_trace("order-paused", 0, 0);
        return DT_ORDER_NONE;
    }

    /* Capture the user-input epoch before the pick, the synthetic A key and
     * the deferred click.  All three are part of the command the release
     * already started; only a real input after this point may cancel the
     * confirmation.  The synthetic A does not advance the epoch itself. */
    const uint32_t input_epoch = hle_android_input_epoch();

    const uint32_t scratch = dt_scratch_alloc();
    if (!scratch) {
        dt_log("order no-scratch");
        return DT_ORDER_NONE;
    }

    const uint32_t pick = scratch + DT_PICK_OFFSET;
    const uint32_t unit_out = scratch + DT_UNIT_OFFSET;
    const uint32_t point = scratch + DT_POINT_OFFSET;
    if (!dt_event_point(c, ev->x, ev->y, width, height, point)) {
        dt_log("order no-point");
        dt_scratch_free(scratch);
        return DT_ORDER_NONE;
    }

    uint32_t object = 0, wx = 0, wy = 0, wz = 0;
    const int r = dt_pick(c, ctx, wc3_rd32g(point), wc3_rd32g(point + 4u),
                          pick, 4u, &object, &wx, &wy, &wz);
    const uint32_t candidate = wc3_rd32g(pick + 8u);
    dt_log("order pick r=%d object=%08X world=%08X,%08X,%08X attack=%d",
           r, object, wx, wy, wz, attack_move);

    /* 0x6F21B730 accepts only a CGhostImage and takes its own reference for
     * the native dispatcher.  A controllable target is a CUnit instead, so
     * the validation fails; wc3_dt_order_route() keeps that pick on the unit
     * leg and selects the native right click rather than the terrain
     * attack-move. */
    uint32_t ghost = 0;
    if (r == WC3_DT_PICK_OBJECT) {
        ghost = dt_validate_object(c, object, unit_out);
    }
    const Wc3DtOrderRoute route =
        wc3_dt_order_route(r, ghost != 0, attack_move);

    DtOrderResult result = DT_ORDER_NONE;
    switch (route) {
    case WC3_DT_ORDER_ROUTE_UNIT:
        result = dt_order_unit(c, ctx, scratch, object, candidate,
                               wx, wy, wz)
                     ? DT_ORDER_UNIT
                     : DT_ORDER_FALLBACK;
        dt_release_object(c, ghost);
        break;
    case WC3_DT_ORDER_ROUTE_GROUND:
        result = dt_order_ground(c, ctx, wx, wy) ? DT_ORDER_GROUND
                                                 : DT_ORDER_FALLBACK;
        break;
    case WC3_DT_ORDER_ROUTE_ATTACK_MOVE:
        /* Long tap on terrain keeps the legacy attack-move: the native A
         * key enters target mode, then a deferred native left click
         * confirms the ground point once the guest has built that mode.
         * Tap keeps the right-click smart move.  The A key is synthetic:
         * it belongs to this command and must not advance the input epoch
         * past the click's snapshot. */
        hle_android_key_event_synthetic(0x41, 1);
        hle_android_key_event_synthetic(0x41, 0);
        dt_field_attack_move_click_queue_at(ev, input_epoch);
        result = DT_ORDER_ATTACK_MOVE;
        break;
    case WC3_DT_ORDER_ROUTE_NATIVE:
        result = DT_ORDER_FALLBACK;
        break;
    case WC3_DT_ORDER_ROUTE_NONE:
    default:
        break;
    }
    dt_log("order r=%d object=%08X ghost=%08X route=%d result=%d", r, object,
           ghost, route, result);

    if (result == DT_ORDER_NONE || result == DT_ORDER_FALLBACK) {
        if (dt_world_click_allowed(c, ctx, ev)) {
            dt_log("order fallback native pending=%d down=%d",
                   g_native_release.right, hle_android_mouse_is_down(2));
            dt_native_right_click(ev->x, ev->y);
            result = DT_ORDER_FALLBACK;
        } else {
            dt_trace("order-fallback-dropped", dt_bits(ev->x),
                     dt_bits(ev->y));
        }
    }

    dt_scratch_free(scratch);
    return result;
}

/*
 * Long tap without drag.  The target is resolved at the hold point with a
 * left pick so its CUnit controllability can be tested before the order
 * executor runs:
 *   - any controllable target (gold mine, transport, zeppelin, air barge or
 *     an ordinary own unit/building) is selected with the native left
 *     click; this is the selection gesture the user expects even when the
 *     target's tap is the exception smart order;
 *   - an uncontrollable unit/decoration and terrain keep the rev 10
 *     long-tap action through dt_smart_order_at().
 * The caller has already checked the field/HUD/target/paused/native-busy
 * gates.  This function re-checks the live selection because the release can
 * follow a selection change; without a controlled selection a native left
 * click is the correct select/clear action.
 */
static DtOrderResult dt_long_tap_at(CPU *c, const DtContext *ctx,
                                    const Wc3DirectTouchEvent *ev,
                                    uint32_t width, uint32_t height)
{
    if (!c || !ctx || !ctx->ui || !ctx->world) return DT_ORDER_NONE;
    if (dt_paused(c)) return DT_ORDER_NONE;

    const int controlled = dt_selection_controlled(c, ctx);
    if (!controlled) {
        dt_native_press_deferred(ev, 0);
        return DT_ORDER_SELECT;
    }

    const uint32_t scratch = dt_scratch_alloc();
    if (!scratch) {
        dt_log("long-tap no-scratch");
        return DT_ORDER_NONE;
    }

    const uint32_t pick = scratch + DT_PICK_OFFSET;
    const uint32_t unit_out = scratch + DT_UNIT_OFFSET;
    const uint32_t point = scratch + DT_POINT_OFFSET;
    if (!dt_event_point(c, ev->x, ev->y, width, height, point)) {
        dt_log("long-tap no-point");
        dt_scratch_free(scratch);
        return DT_ORDER_NONE;
    }

    uint32_t object = 0, wx = 0, wy = 0, wz = 0;
    const int r = dt_pick(c, ctx, wc3_rd32g(point), wc3_rd32g(point + 4u),
                          pick, 1u, &object, &wx, &wy, &wz);
    dt_log("long-tap pick r=%d object=%08X world=%08X,%08X,%08X",
           r, object, wx, wy, wz);

    if (r == WC3_DT_PICK_OBJECT) {
        const uint32_t unit = dt_validate_unit(c, object, unit_out);
        const int is_unit = unit != 0;
        const int target_controllable =
            is_unit ? dt_unit_controlled(c, unit) : 0;
#if WC3_DEBUG && WC3_DEBUG_INPUT
        const uint32_t target_type =
            is_unit ? dt_unit_type_id(unit) : 0u;
#endif
        const int intent = wc3_dt_long_tap_unit_intent(
            1, target_controllable);
        dt_log("long-tap unit object=%08X unit=%08X is_unit=%d "
               "target_type=%08X target_controlled=%d intent=%d",
               object, unit, is_unit, target_type, target_controllable,
               intent);
        if (intent == WC3_DT_INTENT_SELECT_UNIT) {
            dt_native_press_deferred(ev, 0);
            dt_release_object(c, unit);
            dt_scratch_free(scratch);
            return DT_ORDER_SELECT;
        }
        dt_release_object(c, unit);
        dt_scratch_free(scratch);
        return dt_smart_order_at(c, ctx, ev, width, height, 1);
    }

    dt_scratch_free(scratch);
    return dt_smart_order_at(c, ctx, ev, width, height, 1);
}

static void dt_tap(CPU *c, const DtContext *ctx, const Wc3DirectTouchEvent *ev,
                   uint32_t width, uint32_t height)
{
#if WC3_DEBUG && WC3_DEBUG_INPUT
    if (g_map_contacts > 0 && g_map_contacts <= DT_MAP_AUDIT_CONTACTS) {
        dt_log("map-tap#%u enter ptr=%u n=%.4f,%.4f",
               g_map_contacts, ev->pointer_id, ev->x, ev->y);
    }
#endif
    dt_log("tap enter ptr=%u n=%.4f,%.4f", ev->pointer_id, ev->x, ev->y);
    if (dt_paused(c)) {
        dt_trace("tap-paused", 0, 0);
        dt_log("tap paused");
        return;
    }

    const uint32_t scratch = dt_scratch_alloc();
    if (!scratch) {
        dt_log("tap no-scratch");
        return;
    }

    const uint32_t pick = scratch + DT_PICK_OFFSET;
    const uint32_t unit_out = scratch + DT_UNIT_OFFSET;
    const uint32_t point = scratch + DT_POINT_OFFSET;
    if (!dt_event_point(c, ev->x, ev->y, width, height, point)) {
        dt_log("tap no-point");
        dt_scratch_free(scratch);
        return;
    }
    dt_log("tap point=%08X,%08X", wc3_rd32g(point),
           wc3_rd32g(point + 4u));

    dt_trace_selection(c, ctx, "tap-pre");
    const int controlled = dt_selection_controlled(c, ctx);
    dt_trace_selection(c, ctx, "tap-post");

    /* Always left pick.  Only a left-button pick returns a CUnit, and only a
     * CUnit can be tested with 0x6F66EC60; a right pick returns a
     * CGhostImage and cannot answer whether the target is controllable. */
    uint32_t object = 0, wx = 0, wy = 0, wz = 0;
    const int r = dt_pick(c, ctx, wc3_rd32g(point), wc3_rd32g(point + 4u),
                          pick, 1u, &object, &wx, &wy, &wz);
    dt_log("tap pickctx +00=%08X +04=%08X +08=%08X +0C=%08X +10=%08X "
           "+14=%08X +18=%08X +1C=%08X +20=%08X +24=%08X +28=%08X "
           "+2C=%08X",
           wc3_rd32g(pick), wc3_rd32g(pick + 4u),
           wc3_rd32g(pick + 8u), wc3_rd32g(pick + 0x0Cu),
           wc3_rd32g(pick + 0x10u), wc3_rd32g(pick + 0x14u),
           wc3_rd32g(pick + 0x18u), wc3_rd32g(pick + 0x1Cu),
           wc3_rd32g(pick + 0x20u), wc3_rd32g(pick + 0x24u),
           wc3_rd32g(pick + 0x28u), wc3_rd32g(pick + 0x2Cu));
#if WC3_DEBUG && WC3_DEBUG_INPUT
    /* Do not probe pick+0x08: it is opaque to the handler and must never be
     * dereferenced through its vtable.  The 20:31 crash was a candidate
     * probe calling 0x6F1A2640 on an arbitrary pointer. */
    dt_probe_object(c, "tap-picked", object, scratch + DT_PROBE_OFFSET);
#endif
    dt_log("tap pick r=%d object=%08X world=%08X,%08X,%08X controlled=%d",
           r, object, wx, wy, wz, controlled);

    if (r == 1) {
        const uint32_t unit = dt_validate_unit(c, object, unit_out);
        const int is_unit = unit != 0;
        const int target_controllable = dt_unit_controlled(c, unit);
        const uint32_t target_type =
            is_unit ? dt_unit_type_id(unit) : 0u;
        const int target_tap_order =
            wc3_dt_unit_type_tap_order(target_type);
        const int intent = wc3_dt_field_tap_intent(
            WC3_DT_PICK_OBJECT, is_unit, is_unit, controlled,
            target_controllable, target_tap_order);
        dt_log("tap unit object=%08X unit=%08X is_unit=%d "
               "target_type=%08X target_controlled=%d "
               "target_tap_order=%d intent=%d",
               object, unit, is_unit, target_type, target_controllable,
               target_tap_order, intent);
#if WC3_DEBUG && WC3_DEBUG_INPUT
        if (g_map_contacts > 0 && g_map_contacts <= DT_MAP_AUDIT_CONTACTS) {
            dt_log("map-tap#%u pick object=%08X unit=%08X type=%08X "
                   "intent=%d controlled=%d target_controlled=%d "
                   "target_tap_order=%d", g_map_contacts, object, unit,
                   target_type, intent, controlled, target_controllable,
                   target_tap_order);
        }
#endif
        switch (intent) {
        case WC3_DT_INTENT_SELECT_UNIT:
            /* The game's own left click is the exact select route.  It also
             * selects a controllable target while another controlled
             * selection is live, which is the behavior this path exists
             * for.  A field tap is touch-originated, not a modal placement
             * replay: arm it through the idle gateway so the guest refreshes
             * its hot control after the move before DOWN is hit-tested.  The
             * immediate replay here swallowed the first world tap after a
             * panel/menu contact. */
            dt_native_press_deferred(ev, 0);
            dt_trace("tap-select-unit-native", unit, 0);
#if WC3_DEBUG && WC3_DEBUG_INPUT
            dt_map_audit_arm(unit);
#endif
            break;
        case WC3_DT_INTENT_ORDER_UNIT: {
            const DtOrderResult order =
                dt_smart_order_at(c, ctx, ev, width, height, 0);
            dt_log("tap order-unit result=%d", order);
            dt_trace(order == DT_ORDER_UNIT ? "tap-order-unit"
                                            : "tap-order-native",
                     unit, object);
            (void)order;
            break;
        }
        case WC3_DT_INTENT_CLEAR_SELECTION:
            dt_clear_selection(c, ctx);
            dt_trace("tap-clear-unit", unit, 0);
            break;
        case WC3_DT_INTENT_NONE:
        default:
            dt_trace("tap-non-unit", object, 0);
            break;
        }
        dt_release_object(c, unit);
        dt_scratch_free(scratch);
        return;
    }

    if (r == 2) {
        const int intent = wc3_dt_field_tap_intent(WC3_DT_PICK_TERRAIN, 0, 0,
                                                   controlled, 0, 0);
        dt_log("tap terrain r=2 controlled=%d intent=%d world=%08X,%08X",
               controlled, intent, wx, wy);
        if (intent == WC3_DT_INTENT_ORDER_GROUND) {
            const DtOrderResult order =
                dt_smart_order_at(c, ctx, ev, width, height, 0);
            dt_log("tap order-ground result=%d", order);
            dt_trace(order == DT_ORDER_GROUND ? "tap-order-ground"
                                              : "tap-order-ground-native",
                     wx, wy);
            (void)order;
        } else if (intent == WC3_DT_INTENT_CLEAR_SELECTION) {
            dt_clear_selection(c, ctx);
            dt_trace("tap-clear", 0, 0);
        }
        dt_scratch_free(scratch);
        return;
    }

    dt_log("tap no-hit r=%d controlled=%d", r, controlled);
    if (controlled) {
        /* 0x6F3840E0 can reject before its raycast (world/viewport/pick
         * bound gates), leaving no semantic target.  Replay the same
         * gesture as a native right click, but only while the live context
         * still owns a world click. */
        if (dt_world_click_allowed(c, ctx, ev)) {
            dt_log("tap order-native fallback pending=%d down=%d",
                   g_native_release.right, hle_android_mouse_is_down(2));
            dt_native_right_click(ev->x, ev->y);
            dt_trace("tap-order-native", 0, 0);
        } else {
            dt_trace("tap-order-native-dropped", 0, 0);
        }
    }

    dt_log("tap done");
    dt_scratch_free(scratch);
}

static int dt_is_native_pointer(uint32_t kind)
{
    return kind == WC3_DIRECT_TOUCH_POINTER_DOWN ||
           kind == WC3_DIRECT_TOUCH_POINTER_MOVE ||
           kind == WC3_DIRECT_TOUCH_POINTER_UP ||
           kind == WC3_DIRECT_TOUCH_POINTER_CANCEL;
}

static int dt_minimap_native_rect(float *left, float *top,
                                  float *right, float *bottom)
{
    return dt_rect_read(&g_minimap_snapshot, left, top, right, bottom);
}

/*
 * Convert a point claimed by the full minimap input area into the smaller
 * native map frame.  This is used only for beacon signal placement, whose
 * AddSignal matrix is defined in the child frame's local space.  It must NOT
 * be used for pointer phases: the native camera handler maps the whole
 * container at CGameUI+0x41C to the map, so compressing a DOWN/MOVE into the
 * child frame makes the camera jump to its top edge (~65% of the panel)
 * before the real point.  Native phases use dt_minimap_input_clamp.
 */
static void dt_minimap_input_point(float x, float y,
                                   float *out_x, float *out_y)
{
    *out_x = x;
    *out_y = y;
    float il = 0.0f;
    float it = 0.0f;
    float ir = 0.0f;
    float ib = 0.0f;
    float ml = 0.0f;
    float mt = 0.0f;
    float mr = 0.0f;
    float mb = 0.0f;
    if (!wc3_hle_direct_touch_minimap_input_rect(&il, &it, &ir, &ib) ||
        !dt_minimap_native_rect(&ml, &mt, &mr, &mb)) return;
    /* A release on the map image is already in the frame's surface space.
     * The input gate also covers the button row, so compressing that whole
     * gate into the smaller image would move a valid map release. */
    if (wc3_dt_minimap_input_contains(x, y, ml, mt, mr, mb)) return;
    (void)wc3_dt_minimap_local_point(x, y,
                                     it, il, ib, ir,
                                     mt, ml, mb, mr,
                                     out_x, out_y);
}

/*
 * Clamp a native minimap pointer to a point strictly inside the published
 * frame.  The snapshot and the controls layer use inclusive normalised
 * bounds, while the native frame manager tests its rectangle with Win32
 * RECT semantics (exclusive right/bottom).  One client pixel of inset keeps
 * the release inside after hle_android_mouse_move maps the fraction back to
 * client pixels.  A vanished frame (UI transition) reuses the last in-rect
 * point so the native handler still receives a coherent release.
 */
static void dt_minimap_native_clamp(float *x, float *y,
                                    uint32_t width, uint32_t height)
{
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    if (!dt_minimap_native_rect(&left, &top, &right, &bottom)) {
        *x = g_minimap.x;
        *y = g_minimap.y;
        return;
    }
    const float inset_x = width > 1u ? 1.0f / (float)width : 0.001f;
    const float inset_y = height > 1u ? 1.0f / (float)height : 0.001f;
    float inner_left = left + inset_x;
    float inner_top = top + inset_y;
    float inner_right = right - inset_x;
    float inner_bottom = bottom - inset_y;
    if (inner_left > inner_right) {
        inner_left = inner_right = (left + right) * 0.5f;
    }
    if (inner_top > inner_bottom) {
        inner_top = inner_bottom = (top + bottom) * 0.5f;
    }
    if (*x < inner_left) *x = inner_left;
    if (*x > inner_right) *x = inner_right;
    if (*y < inner_top) *y = inner_top;
    if (*y > inner_bottom) *y = inner_bottom;
    g_minimap.x = *x;
    g_minimap.y = *y;
}

/*
 * Clamp a minimap drag point to the full visible panel (the container frame),
 * not to the smaller +0xB4 map image.  The native camera handler maps the
 * container to the whole map; once the DOWN has started a drag, the guest
 * keeps receiving moves even when the point leaves the child frame.  Keeping
 * the raw finger coordinate in container space therefore lets an upward drag
 * reach the north edge instead of stopping at the child frame's top edge.
 */
static void dt_minimap_input_clamp(float *x, float *y,
                                   uint32_t width, uint32_t height)
{
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    if (!wc3_hle_direct_touch_minimap_input_rect(&left, &top,
                                                 &right, &bottom)) {
        /* No live container: fall back to the published native frame. */
        dt_minimap_native_clamp(x, y, width, height);
        return;
    }
    const float inset_x = width > 1u ? 1.0f / (float)width : 0.001f;
    const float inset_y = height > 1u ? 1.0f / (float)height : 0.001f;
    float inner_left = left + inset_x;
    float inner_top = top + inset_y;
    float inner_right = right - inset_x;
    float inner_bottom = bottom - inset_y;
    if (inner_left > inner_right) {
        inner_left = inner_right = (left + right) * 0.5f;
    }
    if (inner_top > inner_bottom) {
        inner_top = inner_bottom = (top + bottom) * 0.5f;
    }
    wc3_dt_minimap_pointer_point(*x, *y, inner_left, inner_top,
                                 inner_right, inner_bottom, x, y);
    g_minimap.x = *x;
    g_minimap.y = *y;
}

static void dt_minimap_native_reset(void)
{
    g_minimap.active = 0;
    g_minimap.pointer = 0;
    g_minimap.pressed = 0;
}

static void dt_native_release(void)
{
    dt_beacon_cancel();
    /* A context change or surface cancel supersedes a late-resolved click:
     * the armed DOWN is cancelled below, and the matching UP must not be
     * posted into the new UI. */
    g_native_up_pending = 0;
    g_native_up_wait = 0;
    g_native_up_modal = 0;
    const int minimap_pressed = g_minimap.pressed;
    const int pressed = g_native_down || minimap_pressed;
    if (pressed && g_minimap.active) {
        /* Context change/stale release: put the cursor back at the last
         * clamped minimap point before the UP, so the native frame manager
         * still routes the release to the minimap. */
        hle_android_mouse_move(g_minimap.x, g_minimap.y);
    }
    dt_minimap_native_reset();
    const int pending_cancelled = hle_android_mouse_cancel_pending_down();
    if (!pressed) return;
    if (pending_cancelled && !minimap_pressed) {
        /* The press never reached the guest, so a lone UP could click the
         * control under the cancelled gesture.  Drop the direct-touch press
         * state without posting it. */
        dt_native_cancel_done();
        return;
    }
    /* The logical bit may already be clear (overlay release_all, surface
     * stop, duplicate cancel).  The guest may still own a native drag, so
     * post the matching UP unconditionally. */
    dt_native_end();
}

/* A deferred field click is valid only while the UI, world and select-mode
 * words it was queued against still exist.  Returning to the main menu or
 * tearing the map down destroys them; waiting through LEGACY/NATIVE is only
 * for a load/fade-in that keeps the same context.  This is the deterministic
 * replacement for the old 1000 ms timeout. */
static int dt_field_load_context_alive(void)
{
    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui || !dt_guest_readable(ui, DT_UI_WORLD_FRAME + 4u)) return 0;
    if (!wc3_rd32g(ui + DT_UI_WORLD_FRAME)) return 0;
    if (!wc3_rd32g(ui + DT_UI_SELECT_MODE)) return 0;
    return 1;
}

/*
 * A field press during the map load/fade-in would be posted as a native click
 * before the game is ready; the guest drops it and the user's first tap is
 * lost.  Hold the press and replay a tap when the exact service route becomes
 * FIELD.  The world and select mode must be present and select must be the
 * active mode, so a menu, a field drag or a command target mode is never
 * deferred.  A drag is not a tap: once the finger leaves the slop, the native
 * press starts and follows it.
 */
static int dt_field_click_should_defer(float x, float y)
{
    const int inside_field = dt_field_contains(x, y);
    int ui_present = 0;
    int world_present = 0;
    int select_present = 0;
    int select_active = 0;
    uint32_t click_gate = 0;
    if (dt_guest_readable(DT_UI_GLOBAL, 4u)) {
        const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
        if (ui && dt_guest_readable(ui, DT_UI_WORLD_FRAME + 4u)) {
            ui_present = 1;
            world_present = wc3_rd32g(ui + DT_UI_WORLD_FRAME) != 0;
            const uint32_t select = wc3_rd32g(ui + DT_UI_SELECT_MODE);
            select_present = select != 0;
            select_active =
                select && wc3_rd32g(ui + DT_UI_ACTIVE_MODE) == select;
            click_gate = wc3_rd32g(ui + DT_UI_CLICK_GATE);
        }
    }
    return wc3_dt_field_click_defer(inside_field, ui_present, world_present,
                                    select_present, select_active,
                                    (int)click_gate, g_service_field_ready);
}

/* Any live non-select input mode over the battlefield owns the click as a
 * target/transient resolution.  `CGameUI+0x220` is one observed example; it
 * is not CGameUI+0x210, so the old grid-only predicate missed it.  The caller
 * suppresses the native DOWN, follows the cursor with moves, and posts the
 * native click pair on release.  A fresh tap reads the live active/select
 * words without requiring the click gate; target mode may close it.  The
 * pause predicate keeps a stale target mode under the pause menu out. */
static int dt_field_target_tap_ready(CPU *c, float x, float y)
{
    DtContext ctx;
    memset(&ctx, 0, sizeof ctx);
    /* A fresh world tap has a CPU and may use the gate-free target context.
     * The field-drag forwarding path passes NULL; it already owns the native
     * button, so only the strict semantic context applies there. */
    if (!(c ? dt_target_context(&ctx) : dt_context(&ctx))) return 0;
    if (!dt_field_contains(x, y)) return 0;
    if (!dt_target_mode_live(ctx.ui)) return 0;
    /* An open pause menu leaves a stale target mode.  That click belongs to
     * the menu and must take the gateway.  A paused game is not a reason to
     * drop a native target confirmation: dt_tap() already drops semantic
     * orders while paused, and drag-from-icon does not check dt_paused(). */
    return !dt_pause_menu_active();
}

/* Returns 1 when the event belongs to the deferred field tap. */
static int dt_native_defer_pointer(CPU *c, const Wc3DirectTouchEvent *ev)
{
    /* The pause menu is a native menu, exactly like the main menu.  The
     * battlefield rectangle is still live underneath it, so without this
     * guard every pause press would be held as a field/target tap.  The
     * pause path in dt_dispatch() resolves pointer phases directly, so a
     * held DOWN would never reach the field resolver and the release would
     * be dropped: no button click at all.  Never enter the field tap
     * deferral while CEscMenu is open; dt_native_pointer() posts the same
     * DOWN/MOVE/UP phases it posts for the main menu. */
    if (!wc3_dt_native_deferral_allowed(g_pause_menu_active)) return 0;
    switch (ev->kind) {
    case WC3_DIRECT_TOUCH_POINTER_DOWN:
        /* The previous tap's UP may still be queued while the logical button
         * is already released.  It is not a held press and must not mask the
         * first field tap after a command-card or menu click: those clicks
         * set g_native_release.left, and the old check turned the next field tap
         * into a native click that the closed gate dropped. */
        if (g_native_release.left && !hle_android_mouse_is_down(0)) {
            g_native_release.left = 0;
        }
        if (g_field_defer_active) {
            /* A second press while the first is held: drop the held gesture
             * and release any native press it started. */
            if (g_field_defer_down_posted) dt_native_abort();
            dt_field_defer_reset();
        }
        if (g_field_click_pending) {
            /* A new gesture supersedes the load-time replay or a deferred
             * attack-move confirmation. */
            dt_trace("field-click-cancel", dt_bits(ev->x), dt_bits(ev->y));
            g_field_click_pending = 0;
            g_field_click_attack_move = 0;
        }
        /* A raw field press on the live FIELD route is a tap candidate.  It
         * is recognized on release, so the native DOWN is not posted until a
         * drag proves it is not a tap.  This is the same immediate tap path
         * used for a semantic FIELD event; it does not depend on a preceding
         * panel gesture. */
        float ml = 0.0f;
        float mt = 0.0f;
        float mr = 0.0f;
        float mb = 0.0f;
        /* The minimap owns the whole visible panel, including the strip that
         * overlaps the bottom of the field rectangle.  Using the +0xB4 map
         * frame here let that strip become a deferred field tap as well as a
         * minimap drag.  The published input rect is the authoritative gate:
         * if it claims the point, no field defer may start. */
        const int on_minimap =
            wc3_hle_direct_touch_minimap_input_rect(&ml, &mt, &mr, &mb) &&
            wc3_dt_minimap_input_contains(ev->x, ev->y, ml, mt, mr, mb);
        const int inside_field =
            !on_minimap && dt_field_contains(ev->x, ev->y);
        const int load_defer =
            inside_field && !g_native_release.left &&
            !hle_android_mouse_is_down(0) &&
            dt_field_click_should_defer(ev->x, ev->y);
        const int live_defer =
            !load_defer && g_service_field_ready && inside_field;
        const int target_defer =
            !load_defer && !live_defer && inside_field &&
            dt_field_target_tap_ready(c, ev->x, ev->y);
        const int late_defer =
            !load_defer && !live_defer && !target_defer && inside_field &&
            dt_ui_present(NULL);
        if (!g_native_down &&
            (live_defer || target_defer || late_defer || load_defer)) {
            g_field_defer_active = 1;
            g_field_defer_pointer = ev->pointer_id;
            g_field_defer_x = ev->x;
            g_field_defer_y = ev->y;
            g_field_defer_moved = 0;
            g_field_defer_down_posted = 0;
            g_field_defer_kind =
                load_defer ? WC3_DT_FIELD_DEFER_LOAD :
                live_defer ? WC3_DT_FIELD_DEFER_LIVE :
                target_defer ? WC3_DT_FIELD_DEFER_TARGET :
                late_defer ? WC3_DT_FIELD_DEFER_LATE
                           : WC3_DT_FIELD_DEFER_NONE;
            if (g_field_defer_kind == WC3_DT_FIELD_DEFER_TARGET) {
                /* Move the target cursor without pressing the button. */
                hle_android_mouse_move(ev->x, ev->y);
            }
            dt_trace(g_field_defer_defer_trace[g_field_defer_kind],
                     dt_bits(ev->x), dt_bits(ev->y));
            return 1;
        }
        return 0;
    case WC3_DIRECT_TOUCH_POINTER_MOVE:
        if (!g_field_defer_active || ev->pointer_id != g_field_defer_pointer)
            return 0;
        if (g_field_defer_kind == WC3_DT_FIELD_DEFER_TARGET) {
            /* Follow the target cursor without pressing the button. */
            hle_android_mouse_move(ev->x, ev->y);
        }
        if (!g_field_defer_moved) {
            const float dx = ev->x - g_field_defer_x;
            const float dy = ev->y - g_field_defer_y;
            if (dx * dx + dy * dy >
                DT_FIELD_TAP_SLOP * DT_FIELD_TAP_SLOP) {
                g_field_defer_moved = 1;
                if (g_field_defer_kind == WC3_DT_FIELD_DEFER_TARGET) {
                    /* Target/placement owns the release, not the press.
                     * Follow the cursor and confirm once on UP; a native
                     * DOWN here would turn the gesture into a drag that the
                     * target mode may cancel instead of confirm. */
                    dt_trace("field-target-drag", dt_bits(ev->x),
                             dt_bits(ev->y));
                } else {
                    /* The press is a drag, not a tap: start the native press
                     * at the original point and follow the finger. */
                    dt_native_begin(g_field_defer_x, g_field_defer_y);
                    g_field_defer_down_posted = 1;
                    dt_trace(g_field_defer_drag_trace[g_field_defer_kind],
                             dt_bits(ev->x), dt_bits(ev->y));
                }
            }
        }
        if (g_field_defer_moved &&
            g_field_defer_kind != WC3_DT_FIELD_DEFER_TARGET) {
            hle_android_mouse_move(ev->x, ev->y);
        }
        return 1;
    case WC3_DIRECT_TOUCH_POINTER_UP:
        if (!g_field_defer_active || ev->pointer_id != g_field_defer_pointer)
            return 0;
        if (!g_field_defer_moved) {
            /* The matching UP is resolved by dt_dispatch() before this
             * native path, so a no-move candidate reaching here belongs to
             * another pointer or a superseded gesture.  Drop it; there is no
             * native press to release and it must not be queued for a later
             * world tap. */
            dt_trace(g_field_defer_up_miss_trace[g_field_defer_kind],
                     ev->pointer_id, 0);
        } else if (g_field_defer_down_posted) {
            /* The DOWN may still be armed.  Publish it before the UP,
             * exactly like the normal native release path; otherwise the
             * queue would deliver the UP first and leave the armed DOWN to
             * press a button afterwards.  Flush only our press: a second
             * finger may have replaced it with a camera sequence. */
            hle_android_mouse_move(ev->x, ev->y);
            if (hle_android_mouse_press_pending()) {
                hle_android_mouse_flush_pending_down();
            }
            dt_native_end();
            dt_trace("field-click-drag-up", dt_bits(ev->x), dt_bits(ev->y));
        }
        dt_field_defer_reset();
        return 1;
    case WC3_DIRECT_TOUCH_POINTER_CANCEL:
        if (!g_field_defer_active || ev->pointer_id != g_field_defer_pointer)
            return 0;
        if (g_field_defer_down_posted) dt_native_abort();
        dt_trace("field-click-cancel", ev->pointer_id, 0);
        dt_field_defer_reset();
        return 1;
    default:
        return 0;
    }
}

/*
 * Native UI input.  The event coordinates are surface-normalised, exactly
 * what hle_android_mouse_move expects.  Posting WM_MOUSEMOVE/WM_LBUTTONDOWN/
 * WM_LBUTTONUP is the same path a real mouse uses; it does not touch the
 * overlay touch engine.  The guest window procedure then runs the minimap,
 * command card, portrait, resource bar and targeting/placement handlers with
 * their native semantics.
 */
/* Every native pointer phase goes through the same gateway.  There is no
 * menu/HUD-only defer flag: the Native Input Gateway publishes all presses at
 * the idle boundary, so a new gesture cannot introduce a synchronous press
 * path by choosing a different wrapper. */
static void dt_native_pointer(CPU *c, const Wc3DirectTouchEvent *ev)
{
    /* A late-resolved click owns the button until its deferred UP is posted.
     * The original contact is already consumed, so any further pointer event
     * is a new gesture; letting it through would either post a second DOWN or
     * release the deferred press before its DOWN.  A cancel still aborts the
     * whole sequence. */
    if (g_native_up_pending) {
        if (ev->kind == WC3_DIRECT_TOUCH_POINTER_CANCEL) {
            dt_native_release();
            dt_trace("native-cancel", ev->pointer_id, 0);
        } else {
            dt_trace("native-up-pending-busy", ev->kind, ev->pointer_id);
        }
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_INPUT
    {
        static uint32_t move_count;
        if (ev->kind != WC3_DIRECT_TOUCH_POINTER_MOVE ||
            (move_count++ & 7u) == 0u) {
            dt_log("native-ptr kind=%s ptr=%u n=%.4f,%.4f down=%d "
                   "up_pending=%d hw_down=%d",
                   dt_kind_name(ev->kind), ev->pointer_id, ev->x, ev->y,
                   g_native_down, g_native_release.left,
                   hle_android_mouse_is_down(0));
        }
    }
#endif
    if (dt_native_defer_pointer(c, ev)) return;
    switch (ev->kind) {
    case WC3_DIRECT_TOUCH_POINTER_DOWN:
        if (g_native_release.left && !hle_android_mouse_is_down(0)) {
            /* The queued release has been dispatched; a normal DOWN will do. */
            g_native_release.left = 0;
        }
        if (g_native_down ||
            (!g_native_release.left && hle_android_mouse_is_down(0))) {
            /* The overlay engine already owns the left button, or a previous
             * native press has not been released yet.  Do not post a second
             * DOWN: the matching UP would then cancel the other owner. */
            dt_trace("native-down-busy", ev->pointer_id, 0);
            break;
        }
        /* One universal press path: the HLE posts the move and, when the
         * barrier is armed, holds this DOWN behind the idle hover pass.
         * No extra release is posted at the new position: the menu would
         * consume it before this DOWN. */
        dt_native_begin(ev->x, ev->y);
        dt_trace("native-down", dt_bits(ev->x), dt_bits(ev->y));
        break;
    case WC3_DIRECT_TOUCH_POINTER_MOVE:
        hle_android_mouse_move(ev->x, ev->y);
        break;
    case WC3_DIRECT_TOUCH_POINTER_UP:
        hle_android_mouse_move(ev->x, ev->y);
        /* The idle pass normally releases the press.  If the release arrived
         * first, post it now for both the menu and the universal press slot;
         * otherwise this UP would sit ahead of its DOWN in the queue. */
        hle_android_mouse_flush_pending_down();
        if (g_native_down) {
            /* Force the UP: another release path may have cleared the
             * logical bit while the guest still owns the native press. */
            dt_native_end();
        }
        dt_trace("native-up", dt_bits(ev->x), dt_bits(ev->y));
        break;
    case WC3_DIRECT_TOUCH_POINTER_CANCEL:
        dt_native_release();
        dt_trace("native-cancel", ev->pointer_id, 0);
        break;
    default:
        break;
    }
}

/*
 * Beacon button gesture.  The signal button and the armed minimap press are
 * claimed here before any native mouse button is posted, so pressing the
 * button or placing the beacon never starts a camera drag or a native click.
 *
 *   DOWN on the signal button          -> start a button-origin drag;
 *   DOWN on the minimap while armed    -> start an armed placement;
 *   MOVE                               -> consume, remember whether it moved;
 *   UP on the minimap                  -> place at the release point;
 *   UP on the portrait                 -> place on the displayed unit;
 *   UP in the field                    -> pick and place at the release point;
 *   UP on the button without movement  -> arm the next minimap press;
 *   UP anywhere else after movement    -> cancel.
 *
 * A button-origin drag is resolved from its DOWN and UP points, not only from
 * the MOVE phases that happened to reach the service: a hold dragged onto the
 * minimap or into the world always drops the beacon where the finger lifted.
 *
 * A press anywhere else disarms the one-shot mode and is handled normally.
 */
static int dt_beacon_pointer(CPU *c, const DtContext *ctx,
                             const Wc3DirectTouchEvent *ev,
                             uint32_t width, uint32_t height)
{
    if (!ev) return 0;

    if (ev->kind == WC3_DIRECT_TOUCH_POINTER_DOWN) {
        if (g_beacon.active) {
            /* Native HUD gestures are single-pointer; a second press must not
             * start a second beacon or leak a native click. */
            return 1;
        }

        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        const int have_button =
            dt_beacon_signal_rect(width, height, &left, &top, &right,
                                  &bottom);
        const int on_button =
            have_button &&
            ev->x >= left && ev->x <= right &&
            ev->y >= top && ev->y <= bottom;
        dt_log("beacon-down ptr=%u n=%.4f,%.4f active=%d armed=%d "
               "have_button=%d button=%.4f,%.4f..%.4f,%.4f on_button=%d",
               ev->pointer_id, ev->x, ev->y, g_beacon.active, g_beacon.armed,
               have_button, left, top, right, bottom, on_button);
        if (on_button) {
            g_beacon.armed = 0;
            g_beacon.active = 1;
            g_beacon.pointer = ev->pointer_id;
            g_beacon.down_x = ev->x;
            g_beacon.down_y = ev->y;
            g_beacon.moved = 0;
            g_beacon.from_button = 1;
            dt_trace("beacon-button-down", dt_bits(ev->x), dt_bits(ev->y));
            return 1;
        }

        if (g_beacon.armed) {
            float ml = 0.0f;
            float mt = 0.0f;
            float mr = 0.0f;
            float mb = 0.0f;
            const int have_minimap =
                wc3_hle_direct_touch_minimap_input_rect(&ml, &mt, &mr, &mb);
            const int on_minimap =
                have_minimap &&
                ev->x >= ml && ev->x <= mr &&
                ev->y >= mt && ev->y <= mb;
            dt_log("beacon-armed-down ptr=%u n=%.4f,%.4f have_minimap=%d "
                   "input=%.4f,%.4f..%.4f,%.4f on_minimap=%d",
                   ev->pointer_id, ev->x, ev->y, have_minimap,
                   ml, mt, mr, mb, on_minimap);
            if (on_minimap) {
                g_beacon.active = 1;
                g_beacon.pointer = ev->pointer_id;
                g_beacon.down_x = ev->x;
                g_beacon.down_y = ev->y;
                g_beacon.moved = 0;
                g_beacon.from_button = 0;
                dt_trace("beacon-armed-down", dt_bits(ev->x),
                         dt_bits(ev->y));
                return 1;
            }
            /* One-shot: a press outside the minimap cancels the armed mode
             * and is handled by the normal route. */
            g_beacon.armed = 0;
            dt_trace("beacon-disarm", dt_bits(ev->x), dt_bits(ev->y));
        }
        return 0;
    }

    if (ev->kind == WC3_DIRECT_TOUCH_POINTER_CANCEL) {
        if (!g_beacon.active && !g_beacon.armed) return 0;
        const int owned = g_beacon.active;
        dt_beacon_cancel();
        dt_trace("beacon-cancel", ev->pointer_id, 0);
        return owned ? 1 : 0;
    }

    if (!g_beacon.active || ev->pointer_id != g_beacon.pointer) return 0;

    if (ev->kind == WC3_DIRECT_TOUCH_POINTER_MOVE) {
        if (!g_beacon.moved) {
            const float dx = ev->x - g_beacon.down_x;
            const float dy = ev->y - g_beacon.down_y;
            if (dx * dx + dy * dy >
                DT_BEACON_TAP_SLOP * DT_BEACON_TAP_SLOP) {
                g_beacon.moved = 1;
            }
        }
        return 1;
    }

    if (ev->kind != WC3_DIRECT_TOUCH_POINTER_UP) return 0;

    float ml = 0.0f;
    float mt = 0.0f;
    float mr = 0.0f;
    float mb = 0.0f;
    const int inside_minimap =
        wc3_hle_direct_touch_minimap_input_rect(&ml, &mt, &mr, &mb) &&
        ev->x >= ml && ev->x <= mr &&
        ev->y >= mt && ev->y <= mb;
    float bl = 0.0f;
    float bt = 0.0f;
    float br = 0.0f;
    float bb = 0.0f;
    const int inside_button =
        dt_beacon_signal_rect(width, height, &bl, &bt, &br, &bb) &&
        ev->x >= bl && ev->x <= br &&
        ev->y >= bt && ev->y <= bb;
    float pl = 0.0f;
    float pt = 0.0f;
    float pr = 0.0f;
    float pb = 0.0f;
    const int have_portrait =
        wc3_hle_direct_touch_portrait_rect(&pl, &pt, &pr, &pb);
    const int inside_portrait =
        have_portrait &&
        ev->x >= pl && ev->x <= pr && ev->y >= pt && ev->y <= pb;
    const int inside_field = dt_field_contains(ev->x, ev->y);
    /* Resolve from the press/release endpoints.  The service may defer or
     * coalesce MOVE phases, so `g_beacon.moved` alone cannot prove that a
     * hold on the button was dragged onto the map. */
    const Wc3DtBeaconRelease release = wc3_dt_beacon_release_from_points(
        inside_button, inside_minimap, inside_portrait, inside_field,
        g_beacon.down_x, g_beacon.down_y, ev->x, ev->y, DT_BEACON_TAP_SLOP);
    dt_log("beacon-up ptr=%u n=%.4f,%.4f moved=%d from_button=%d button=%d"
           "(%.4f,%.4f..%.4f,%.4f) minimap=%d(%.4f,%.4f..%.4f,%.4f) "
           "portrait=%d(%.4f,%.4f..%.4f,%.4f) field=%d release=%d",
           ev->pointer_id, ev->x, ev->y, g_beacon.moved, g_beacon.from_button,
           inside_button,
           bl, bt, br, bb, inside_minimap, ml, mt, mr, mb,
           inside_portrait, pl, pt, pr, pb, inside_field, (int)release);

    g_beacon.active = 0;
    g_beacon.pointer = 0;
    g_beacon.moved = 0;
    g_beacon.from_button = 0;

    switch (release) {
    case WC3_DT_BEACON_MINIMAP: {
        g_beacon.armed = 0;
        float mx = ev->x;
        float my = ev->y;
        dt_minimap_input_point(mx, my, &mx, &my);
        if (dt_beacon_place_minimap(c, mx, my, width, height)) {
            dt_trace("beacon-place-minimap", dt_bits(mx), dt_bits(my));
        }
        return 1;
    }
    case WC3_DT_BEACON_PORTRAIT:
        g_beacon.armed = 0;
        if (dt_beacon_place_portrait(c, ctx)) {
            dt_trace("beacon-place-portrait", dt_bits(ev->x), dt_bits(ev->y));
        }
        return 1;
    case WC3_DT_BEACON_FIELD:
        g_beacon.armed = 0;
        if (dt_beacon_place_field(c, ctx, ev->x, ev->y, width, height)) {
            dt_trace("beacon-place-field", dt_bits(ev->x),
                     dt_bits(ev->y));
        }
        return 1;
    case WC3_DT_BEACON_ARM:
        g_beacon.armed = 1;
#if WC3_DEBUG && WC3_DEBUG_INPUT
        wc3_hle_direct_touch_probe_begin("beacon-arm", dt_bits(ev->x),
                                         dt_bits(ev->y));
#endif
        dt_trace("beacon-arm", dt_bits(ev->x), dt_bits(ev->y));
        return 1;
    default:
        g_beacon.armed = 0;
        dt_trace("beacon-none", dt_bits(ev->x), dt_bits(ev->y));
        return 1;
    }
}

/*
 * Native minimap backstop.  The controls layer clamps a pointer it owns as
 * MINIMAP and marks its raw phases with the MINIMAP route; a stale/empty
 * snapshot at DOWN can still deliver them as NATIVE.  A DOWN inside the
 * published rectangle starts a minimap-owned native drag here too, so the
 * release stays inside the panel even if the controls gate missed and the
 * finger came up far outside.  The UP is forced because the logical left bit
 * may already be clear by the time the raw phase arrives.
 */
static void dt_native_minimap_pointer(CPU *c,
                                      const Wc3DirectTouchEvent *ev,
                                      uint32_t width, uint32_t height)
{
    if (!ev) return;

    if (ev->kind == WC3_DIRECT_TOUCH_POINTER_DOWN) {
        float il = 0.0f;
        float it = 0.0f;
        float ir = 0.0f;
        float ib = 0.0f;
        float ml = 0.0f;
        float mt = 0.0f;
        float mr = 0.0f;
        float mb = 0.0f;
        const int input_present =
            wc3_hle_direct_touch_minimap_input_rect(&il, &it, &ir, &ib);
        const int native_present =
            dt_minimap_native_rect(&ml, &mt, &mr, &mb);
        const int inside =
            !g_native_down && !g_field_drag.select_active &&
            (!hle_android_mouse_is_down(0) || g_native_release.left) &&
            input_present && native_present &&
            wc3_dt_minimap_input_contains(ev->x, ev->y, il, it, ir, ib);
        dt_log("minimap-native-down ptr=%u n=%.4f,%.4f inside=%d "
               "native_down=%d field_select=%d "
               "up_pending=%d hw_down=%d rect=%.4f,%.4f..%.4f,%.4f "
               "input=%.4f,%.4f..%.4f,%.4f",
               ev->pointer_id, ev->x, ev->y, inside, g_native_down,
               g_field_drag.select_active,
               g_native_release.left, hle_android_mouse_is_down(0),
               ml, mt, mr, mb, il, it, ir, ib);
        if (!inside) {
            dt_native_pointer(c, ev);
            return;
        }
        Wc3DirectTouchEvent clamped = *ev;
        g_minimap.active = 1;
        g_minimap.pointer = ev->pointer_id;
        /* Post the DOWN at the actual finger point in panel space.  Mapping
         * it into the +0xB4 child frame first made the camera jump to the
         * ~60% barrier on the tap, then to the tap point on the first MOVE.
         * The whole visible panel is the native minimap's input area. */
        dt_minimap_input_clamp(&clamped.x, &clamped.y, width, height);
        dt_native_pointer(c, &clamped);
        g_minimap.pressed = g_native_down;
        dt_trace("minimap-native-down", dt_bits(clamped.x),
                 dt_bits(clamped.y));
        return;
    }

    if (!g_minimap.active ||
        ev->pointer_id != g_minimap.pointer ||
        (ev->kind != WC3_DIRECT_TOUCH_POINTER_MOVE &&
         ev->kind != WC3_DIRECT_TOUCH_POINTER_UP &&
         ev->kind != WC3_DIRECT_TOUCH_POINTER_CANCEL)) {
        dt_native_pointer(c, ev);
        return;
    }

    Wc3DirectTouchEvent clamped = *ev;
    /* Keep every phase of the drag in panel space so the native camera
     * handler reaches the full map without an intermediate frame-top jump. */
    dt_minimap_input_clamp(&clamped.x, &clamped.y, width, height);

    if (ev->kind == WC3_DIRECT_TOUCH_POINTER_MOVE) {
        dt_native_pointer(c, &clamped);
        dt_trace("minimap-native-move", dt_bits(clamped.x),
                 dt_bits(clamped.y));
        return;
    }

    /* UP or CANCEL.  Post the final panel-space move first, then the release.
     * dt_native_pointer() would skip the release when g_native_down is
     * already clear, which is exactly the latched-drag case this backstop
     * exists for. */
    hle_android_mouse_move(clamped.x, clamped.y);
    dt_log("minimap-native-release ptr=%u n=%.4f,%.4f kind=%s "
           "native_down=%d pressed=%d",
           ev->pointer_id, clamped.x, clamped.y, dt_kind_name(ev->kind),
           g_native_down, g_minimap.pressed);
    if (g_native_down || g_minimap.pressed) {
        dt_native_end();
    }
    dt_minimap_native_reset();
    dt_trace(ev->kind == WC3_DIRECT_TOUCH_POINTER_CANCEL
                 ? "minimap-native-cancel" : "minimap-native-up",
             dt_bits(clamped.x), dt_bits(clamped.y));
}

/*
 * True while the live world is not in CSelectMode and a battlefield gesture
 * therefore belongs to target/placement or another transient input mode.
 * Active != select is the authoritative release signal; it includes the
 * observed CGameUI+0x220 object that the +0x210-only grid check missed.
 */
static int dt_field_target_active(CPU *c, const DtContext *ctx)
{
    if (!c || !ctx || !ctx->ui || !ctx->world) return 0;
    if (!dt_call_ui_bool(c, ctx->ui, DT_FN_IN_GAME)) return 0;
    const uint32_t select = wc3_rd32g(ctx->ui + DT_UI_SELECT_MODE);
    const uint32_t target = wc3_rd32g(ctx->ui + DT_UI_TARGET_MODE);
    const uint32_t active = wc3_rd32g(ctx->ui + DT_UI_ACTIVE_MODE);
    if (target && dt_mode_active(c, ctx->ui, target)) return 1;
    return wc3_dt_command_target_live(active, select);
}

static void dt_field_target_gesture_begin(const Wc3DirectTouchEvent *ev)
{
    if (g_field_drag.target_active) {
        dt_trace("field-target-gesture-restart",
                 g_field_drag.target_pointer, ev->pointer_id);
    }
    g_field_drag.target_active = 1;
    g_field_drag.target_pointer = ev->pointer_id;
    hle_android_mouse_move(ev->x, ev->y);
    dt_trace("field-target-gesture-begin", dt_bits(ev->x),
             dt_bits(ev->y));
}

static void dt_field_target_gesture_move(const Wc3DirectTouchEvent *ev)
{
    if (!g_field_drag.target_active ||
        ev->pointer_id != g_field_drag.target_pointer) {
        dt_log("field-target-move ignored ptr=%u active=%d owner=%u",
               ev->pointer_id, g_field_drag.target_active,
               g_field_drag.target_pointer);
        return;
    }
    hle_android_mouse_move(ev->x, ev->y);
}

static void dt_field_target_gesture_end(const Wc3DirectTouchEvent *ev)
{
    if (!g_field_drag.target_active ||
        ev->pointer_id != g_field_drag.target_pointer) {
        return;
    }
    hle_android_mouse_move(ev->x, ev->y);
    dt_native_click(ev);
    g_field_drag.target_active = 0;
    g_field_drag.target_pointer = 0;
    dt_trace("field-target-gesture-end", dt_bits(ev->x), dt_bits(ev->y));
}

/*
 * Cursor-free camera pan.  The guest's native drag manager is driven directly
 * through CSimpleMessageFrame at CGameUI+0x250.  No mouse message is posted and
 * no virtual button is latched, so the second finger keeps the full cursor.
 *
 * Global mouse input (0x6FD4789C/A4) is swapped only around guest calls and
 * restored on every path.  frame+0x84 and frame+0x88 are zero between camera
 * events; otherwise the game's frame update would pan from the action finger.
 */
#define DT_FN_CAMERA_UNPROJECT   0x6F391F80u
#define DT_FN_CAMERA_READ_TARGET 0x6F360900u

static void dt_camera_vec_read(uint32_t va, float out[3])
{
    for (int i = 0; i < 3; i++) out[i] = dt_float(wc3_rd32g(va + (uint32_t)i * 4u));
}

static void dt_camera_vec_write(uint32_t va, const float v[3])
{
    for (int i = 0; i < 3; i++) wc3_wr32g(va + (uint32_t)i * 4u, dt_bits(v[i]));
}

static int dt_camera_vec_finite(const float v[3])
{
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

static void dt_camera_scope_save(uint32_t saved[3])
{
    if (!dt_guest_readable(DT_MOUSE_INPUT_XY, 12u)) {
        saved[0] = saved[1] = saved[2] = 0u;
        return;
    }
    saved[0] = wc3_rd32g(DT_MOUSE_INPUT_XY);
    saved[1] = wc3_rd32g(DT_MOUSE_INPUT_XY + 4u);
    saved[2] = wc3_rd32g(DT_MOUSE_INPUT_Z);
}

static void dt_camera_scope_set(float x, float y, float z)
{
    if (!dt_guest_readable(DT_MOUSE_INPUT_XY, 12u)) return;
    wc3_wr32g(DT_MOUSE_INPUT_XY, dt_bits(x));
    wc3_wr32g(DT_MOUSE_INPUT_XY + 4u, dt_bits(y));
    wc3_wr32g(DT_MOUSE_INPUT_Z, dt_bits(z));
}

static void dt_camera_scope_restore(const uint32_t saved[3])
{
    if (!dt_guest_readable(DT_MOUSE_INPUT_XY, 12u)) return;
    wc3_wr32g(DT_MOUSE_INPUT_XY, saved[0]);
    wc3_wr32g(DT_MOUSE_INPUT_XY + 4u, saved[1]);
    wc3_wr32g(DT_MOUSE_INPUT_Z, saved[2]);
}

static int dt_camera_conflict_active(void)
{
    return g_portrait_follow_active || g_minimap.pressed;
}

/* Native 0x6F394490 refuses middle-drag while the drag-scroll manager gate is
 * set.  That gate is also left dirty by a UI contact, and the first finger may
 * already be an ACTION cursor holding the game's virtual mouse.  The direct
 * camera drag owns no mouse button and neither posts nor waits for messages,
 * so clear the gate only for the guest call and put it back afterwards. */
static uint32_t dt_camera_manager_gate_clear(uint32_t manager)
{
    if (!manager || !dt_guest_readable(manager + DT_DRAG_MANAGER_GATE, 4u))
        return 0u;
    const uint32_t saved = wc3_rd32g(manager + DT_DRAG_MANAGER_GATE);
    if (saved != 0u)
        wc3_wr32g(manager + DT_DRAG_MANAGER_GATE, 0u);
    return saved;
}

static void dt_camera_manager_gate_restore(uint32_t manager, uint32_t saved)
{
    if (saved == 0u) return;
    if (!manager || !dt_guest_readable(manager + DT_DRAG_MANAGER_GATE, 4u))
        return;
    wc3_wr32g(manager + DT_DRAG_MANAGER_GATE, saved);
}

static int dt_camera_pan_count(void)
{
    int count = 0;
    for (uint32_t i = 0; i < DT_CAMERA_PAN_MAX; i++) {
        if (g_camera_pan.pointers[i].active) count++;
    }
    return count;
}

static Wc3CameraPanPointer *dt_camera_pan_find(uint32_t pointer_id,
                                               uint32_t gesture_id)
{
    for (uint32_t i = 0; i < DT_CAMERA_PAN_MAX; i++) {
        Wc3CameraPanPointer *p = &g_camera_pan.pointers[i];
        if (p->active && (uint32_t)p->pointer == pointer_id &&
            p->gesture_id == gesture_id) {
            return p;
        }
    }
    return NULL;
}

/* Unproject one finger point under the current camera.  The caller's global
 * mouse scope is restored on every path. */
static int dt_camera_unproject_at(CPU *c, uint32_t frame, float x, float y,
                                  float z, float out[3])
{
    if (!c || !frame || !dt_guest_readable(frame, DT_DRAG_MARK + 4u))
        return 0;
    uint32_t saved[3];
    dt_camera_scope_save(saved);
    dt_camera_scope_set(x, y, z);
    const uint32_t scratch = dt_scratch_alloc();
    if (!scratch) {
        dt_camera_scope_restore(saved);
        return 0;
    }
    const uint32_t args[2] = { scratch, dt_bits(z) };
    (void)dt_call_thiscall(c, frame, DT_FN_CAMERA_UNPROJECT, 2, args);
    dt_camera_vec_read(scratch, out);
    dt_scratch_free(scratch);
    dt_camera_scope_restore(saved);
    return dt_camera_vec_finite(out);
}

/* Safety net for map changes and stale queue entries.  No mouse state is
 * touched: the recognizer's END is the normal path for a live gesture. */
static void dt_camera_pan_reset(void)
{
    if (g_camera_pan.active && g_camera_pan.frame &&
        dt_guest_readable(g_camera_pan.frame, DT_DRAG_MARK + 4u)) {
        wc3_wr32g(g_camera_pan.frame + DT_DRAG_ACTIVE, 0u);
        wc3_wr32g(g_camera_pan.frame + DT_DRAG_MARK, 0u);
    }
    memset(&g_camera_pan, 0, sizeof(g_camera_pan));
}

/* Add a second (or later) camera finger to the live pan.  The native drag
 * manager already has a target; only this finger's own anchor is needed. */
static int dt_camera_pan_add(CPU *c, const DtContext *ctx,
                             const Wc3DirectTouchEvent *ev,
                             uint32_t width, uint32_t height,
                             uint32_t frame)
{
    if (!c || !ctx || !ctx->world || !frame) return 0;

    int slot_index = -1;
    for (uint32_t i = 0; i < DT_CAMERA_PAN_MAX; i++) {
        const Wc3CameraPanPointer *p = &g_camera_pan.pointers[i];
        if (p->active && (uint32_t)p->pointer == ev->pointer_id) {
            slot_index = (int)i;
            break;
        }
    }
    if (slot_index < 0) {
        for (uint32_t i = 0; i < DT_CAMERA_PAN_MAX; i++) {
            if (!g_camera_pan.pointers[i].active) {
                slot_index = (int)i;
                break;
            }
        }
    }
    if (slot_index < 0) return 0;

    const uint32_t scratch = dt_scratch_alloc();
    if (!scratch) return 0;
    const uint32_t point = scratch + DT_POINT_OFFSET;
    const uint32_t pick = scratch + DT_PICK_OFFSET;
    if (!dt_event_point(c, ev->x, ev->y, width, height, point)) {
        dt_scratch_free(scratch);
        return 0;
    }
    uint32_t wx = 0u;
    uint32_t wy = 0u;
    uint32_t wz = 0u;
    if (!dt_pick_world(c, ctx, wc3_rd32g(point), wc3_rd32g(point + 4u),
                       pick, &wx, &wy, &wz) ||
        !isfinite(dt_float(wz))) {
        dt_trace("field-camera-add-pick", dt_bits(ev->x), dt_bits(ev->y));
        dt_scratch_free(scratch);
        return 0;
    }
    const float z = dt_float(wz);
    const float event_x = dt_float(wc3_rd32g(point));
    const float event_y = dt_float(wc3_rd32g(point + 4u));
    wc3_wr32g(point + 8u, dt_bits(z));
    float world[3] = { 0.0f, 0.0f, 0.0f };
    const int projected =
        dt_camera_unproject_at(c, frame, event_x, event_y, z, world);
    dt_scratch_free(scratch);
    if (!projected) {
        dt_trace("field-camera-add-projection",
                 dt_bits(event_x), dt_bits(event_y));
        return 0;
    }

    Wc3CameraPanPointer *p = &g_camera_pan.pointers[slot_index];
    memset(p, 0, sizeof(*p));
    p->active = 1;
    p->pointer = (uint8_t)ev->pointer_id;
    p->gesture_id = ev->gesture_id;
    p->z = z;
    p->last_x = event_x;
    p->last_y = event_y;
    memcpy(p->last_world, world, sizeof(world));
#if WC3_DEBUG && WC3_DEBUG_INPUT
    dt_log("cam-add ptr=%u gid=%u px=%08X py=%08X z=%08X "
           "world=%08X/%08X/%08X",
           ev->pointer_id, ev->gesture_id, dt_bits(event_x), dt_bits(event_y),
           dt_bits(z), dt_bits(world[0]), dt_bits(world[1]),
           dt_bits(world[2]));
#endif
    dt_trace("field-camera-add", ev->pointer_id, ev->gesture_id);
    return 1;
}

static void dt_camera_pan_cancel_field_click(const Wc3DirectTouchEvent *ev)
{
    (void)ev;
    if (g_field_click_pending &&
        wc3_dt_event_cancels_field_click(
            WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN, 1)) {
        dt_trace("field-click-cancel", dt_bits(ev->x), dt_bits(ev->y));
        g_field_click_pending = 0;
        g_field_click_attack_move = 0;
    }
}

static int dt_camera_pan_begin(CPU *c, const DtContext *ctx,
                               const Wc3DirectTouchEvent *ev,
                               uint32_t width, uint32_t height)
{
    if (!c || !ctx || !ctx->ui || !ctx->world) return 0;
    if (dt_paused(c)) return 0;
    if (!dt_call_ui_bool(c, ctx->ui, DT_FN_IN_GAME)) return 0;
    if (!dt_guest_readable(ctx->ui + DT_UI_DRAG_MANAGER, 4u) ||
        !dt_guest_readable(ctx->ui + DT_UI_DRAG_FRAME, 4u)) return 0;

    const uint32_t frame = wc3_rd32g(ctx->ui + DT_UI_DRAG_FRAME);
    if (!frame || !dt_guest_readable(frame, DT_DRAG_MARK + 4u)) return 0;
    const uint32_t camera = dt_camera(ctx->ui);
    if (!camera) return 0;

    if (g_camera_pan.active) {
        if (g_camera_pan.frame == frame && g_camera_pan.camera == camera) {
            const int added =
                dt_camera_pan_add(c, ctx, ev, width, height, frame);
            if (added) dt_camera_pan_cancel_field_click(ev);
            return added;
        }
        dt_camera_pan_reset();
    }

    const uint32_t manager = wc3_rd32g(ctx->ui + DT_UI_DRAG_MANAGER);
    const uint32_t manager_gate = dt_camera_manager_gate_clear(manager);
    if (manager_gate != 0u)
        dt_trace("field-camera-gate-bypass", manager_gate, manager);
    if (dt_call_thiscall(c, camera, DT_FN_CAMERA_CAN_DRAG, 0, NULL) != 0u) {
        dt_trace("field-camera-can-drag", camera, 0);
        dt_camera_manager_gate_restore(manager, manager_gate);
        return 0;
    }

    const uint32_t scratch = dt_scratch_alloc();
    if (!scratch) {
        dt_camera_manager_gate_restore(manager, manager_gate);
        return 0;
    }
    const uint32_t point = scratch + DT_POINT_OFFSET;
    const uint32_t pick = scratch + DT_PICK_OFFSET;
    if (!dt_event_point(c, ev->x, ev->y, width, height, point)) {
        dt_scratch_free(scratch);
        dt_camera_manager_gate_restore(manager, manager_gate);
        return 0;
    }
    uint32_t wx = 0u;
    uint32_t wy = 0u;
    uint32_t wz = 0u;
    if (!dt_pick_world(c, ctx, wc3_rd32g(point), wc3_rd32g(point + 4u),
                       pick, &wx, &wy, &wz) ||
        !isfinite(dt_float(wz))) {
        dt_trace("field-camera-pick", dt_bits(ev->x), dt_bits(ev->y));
        dt_scratch_free(scratch);
        dt_camera_manager_gate_restore(manager, manager_gate);
        return 0;
    }
    const float z = dt_float(wz);
    wc3_wr32g(point + 8u, dt_bits(z));

    /* Same direct drag-manager sequence as the native middle-button path, but
     * without SetInputMode: globals at the finger first, then
     * 0x6F391170(frame, &point).  No mouse message is posted and no button is
     * latched, so the action finger keeps its cursor and left button.  Native
     * 0x6F0EF2C5 copies event+0x24/+0x28 into the global mouse, i.e. the
     * pick-event space produced by 0x6F8B38A0 (nx*0.8, (1-ny)*0.6), not raw
     * client pixels.  Client pixels skew the ray by 1/0.8 in X and 1/0.6 in Y
     * and couple the screen axes. */
    const float event_x = dt_float(wc3_rd32g(point));
    const float event_y = dt_float(wc3_rd32g(point + 4u));
    uint32_t saved[3];
    dt_camera_scope_save(saved);
    dt_camera_scope_set(event_x, event_y, z);
    dt_trace("field-camera-mode-neutral",
             wc3_rd32g(ctx->ui + DT_UI_ACTIVE_MODE),
             dt_ui_mode_read(ctx->ui, DT_UI_SELECT_MODE));
    const uint32_t arg = point;
    const uint32_t started =
        dt_call_thiscall(c, frame, DT_FN_CAMERA_DRAG_BEGIN, 1, &arg);
    if (started == 0u) {
        wc3_wr32g(frame + DT_DRAG_ACTIVE, 0u);
        wc3_wr32g(frame + DT_DRAG_MARK, 0u);
        dt_camera_scope_restore(saved);
        dt_camera_manager_gate_restore(manager, manager_gate);
        dt_scratch_free(scratch);
        dt_trace("field-camera-begin-rejected", frame, 0);
        return 0;
    }
    float target[3] = { 0.0f, 0.0f, 0.0f };
    float world[3] = { 0.0f, 0.0f, 0.0f };
    dt_camera_vec_read(frame + DT_DRAG_TARGET, target);
    dt_camera_vec_read(frame + DT_DRAG_WORLD, world);
#if WC3_DEBUG && WC3_DEBUG_INPUT
    dt_log("cam-begin px=%08X py=%08X z=%08X "
           "target=%08X/%08X/%08X world=%08X/%08X/%08X",
           dt_bits(event_x), dt_bits(event_y), dt_bits(z),
           dt_bits(target[0]), dt_bits(target[1]), dt_bits(target[2]),
           dt_bits(world[0]), dt_bits(world[1]), dt_bits(world[2]));
#endif
    (void)dt_call_thiscall(c, frame, DT_FN_CAMERA_DRAG_CANCEL, 0, NULL);
    wc3_wr32g(frame + DT_DRAG_MARK, 0u);
    dt_camera_scope_restore(saved);
    dt_camera_manager_gate_restore(manager, manager_gate);
    dt_scratch_free(scratch);

    if (!dt_camera_vec_finite(target) || !dt_camera_vec_finite(world)) {
        dt_trace("field-camera-projection", dt_bits(target[0]),
                 dt_bits(world[0]));
        return 0;
    }
    memset(&g_camera_pan, 0, sizeof(g_camera_pan));
    g_camera_pan.active = 1;
    g_camera_pan.frame = frame;
    g_camera_pan.camera = camera;
    memcpy(g_camera_pan.target, target, sizeof(target));
    Wc3CameraPanPointer *pan = &g_camera_pan.pointers[0];
    pan->active = 1;
    pan->pointer = (uint8_t)ev->pointer_id;
    pan->gesture_id = ev->gesture_id;
    pan->z = z;
    pan->last_x = event_x;
    pan->last_y = event_y;
    memcpy(pan->last_world, world, sizeof(world));
    dt_camera_pan_cancel_field_click(ev);
    dt_trace("field-camera-begin", dt_bits(ev->x), dt_bits(ev->y));
    return 1;
}

/* MOVE only records the latest point for that finger.  One native update per
 * pending finger is applied after the service drains its batch, so a 120 Hz
 * touch stream does not cause 120 guest camera dispatches per pump. */
static void dt_camera_pan_move(const Wc3DirectTouchEvent *ev)
{
    Wc3CameraPanPointer *p = dt_camera_pan_find(ev->pointer_id,
                                                ev->gesture_id);
    if (!p) {
        dt_log("field-camera-move ignored ptr=%u gid=%u", ev->pointer_id,
               ev->gesture_id);
        return;
    }
    p->pending_x = ev->x;
    p->pending_y = ev->y;
    p->pending = 1;
}

static void dt_camera_pan_end(CPU *c, const Wc3DirectTouchEvent *ev)
{
    Wc3CameraPanPointer *p = dt_camera_pan_find(ev->pointer_id,
                                                ev->gesture_id);
    if (!p) return;

    const int remaining = dt_camera_pan_count() - 1;
    memset(p, 0, sizeof(*p));
    if (remaining > 0) {
        dt_trace("field-camera-end-one", ev->pointer_id,
                 (uint32_t)remaining);
        return;
    }
    if (c && g_camera_pan.frame &&
        dt_guest_readable(g_camera_pan.frame, DT_DRAG_MARK + 4u)) {
        (void)dt_call_thiscall(c, g_camera_pan.frame,
                               DT_FN_CAMERA_DRAG_CANCEL, 0, NULL);
        wc3_wr32g(g_camera_pan.frame + DT_DRAG_ACTIVE, 0u);
        wc3_wr32g(g_camera_pan.frame + DT_DRAG_MARK, 0u);
    }
    memset(&g_camera_pan, 0, sizeof(g_camera_pan));
    dt_trace("field-camera-end", dt_bits(ev->x), dt_bits(ev->y));
}

/* Same conversion as 0x6F074010 + 0x6F8B38A0: the global mouse that
 * 0x6F391F80 consumes is in pick-event space, not client pixels. */
static int dt_camera_event_point(CPU *c, float nx, float ny,
                                 uint32_t width, uint32_t height,
                                 float *out_x, float *out_y)
{
    *out_x = 0.0f;
    *out_y = 0.0f;
    const uint32_t out = dt_scratch_alloc();
    if (!out) return 0;
    const int ok = dt_event_point(c, nx, ny, width, height, out);
    if (ok) {
        *out_x = dt_float(wc3_rd32g(out));
        *out_y = dt_float(wc3_rd32g(out + 4u));
    }
    dt_scratch_free(out);
    return ok;
}

/* Rebase one idle finger after another finger moved the camera.  Its own
 * anchor must follow the camera so its next MOVE does not undo that pan. */
static void dt_camera_pan_rebase_pointer(CPU *c, Wc3CameraPanPointer *p)
{
    float cur[3] = { 0.0f, 0.0f, 0.0f };
    if (dt_camera_unproject_at(c, g_camera_pan.frame, p->last_x, p->last_y,
                               p->z, cur)) {
        memcpy(p->last_world, cur, sizeof(cur));
    }
}

static void dt_camera_pan_rebase_others(CPU *c,
                                        const Wc3CameraPanPointer *skip)
{
    for (uint32_t i = 0; i < DT_CAMERA_PAN_MAX; i++) {
        Wc3CameraPanPointer *p = &g_camera_pan.pointers[i];
        if (p->active && p != skip)
            dt_camera_pan_rebase_pointer(c, p);
    }
}

/* A portrait follow or minimap drag owns the camera for now.  Drop the queued
 * movement of every camera finger and re-anchor all of them, so the pan does
 * not jump when the conflict clears. */
static void dt_camera_pan_rebase_conflict(CPU *c, uint32_t width,
                                          uint32_t height)
{
    for (uint32_t i = 0; i < DT_CAMERA_PAN_MAX; i++) {
        Wc3CameraPanPointer *p = &g_camera_pan.pointers[i];
        if (!p->active) continue;
        float px = p->last_x;
        float py = p->last_y;
        if (p->pending) {
            float nx = 0.0f;
            float ny = 0.0f;
            if (dt_camera_event_point(c, p->pending_x, p->pending_y,
                                      width, height, &nx, &ny)) {
                px = nx;
                py = ny;
                p->last_x = px;
                p->last_y = py;
            }
            p->pending = 0;
        }
        float cur[3] = { 0.0f, 0.0f, 0.0f };
        if (dt_camera_unproject_at(c, g_camera_pan.frame, px, py, p->z,
                                   cur)) {
            memcpy(p->last_world, cur, sizeof(cur));
        }
    }
    g_camera_pan.rebase_pending = 1;
}

static void dt_camera_pan_update(CPU *c, Wc3CameraPanPointer *p,
                                 float px, float py)
{
#if WC3_DEBUG && WC3_DEBUG_INPUT
    dt_log("cam-apply ptr=%u gid=%u px=%08X py=%08X z=%08X "
           "anchor=%08X/%08X/%08X",
           p->pointer, p->gesture_id, dt_bits(px), dt_bits(py), dt_bits(p->z),
           dt_bits(p->last_world[0]), dt_bits(p->last_world[1]),
           dt_bits(p->last_world[2]));
#endif
    dt_camera_scope_set(px, py, p->z);
    dt_camera_vec_write(g_camera_pan.frame + DT_DRAG_WORLD, p->last_world);
    wc3_wr32g(g_camera_pan.frame + DT_DRAG_ACTIVE, 1u);
    wc3_wr32g(g_camera_pan.frame + DT_DRAG_MARK, 1u);
    (void)dt_call_thiscall(c, g_camera_pan.frame,
                           DT_FN_CAMERA_DRAG_UPDATE, 0, NULL);
    wc3_wr32g(g_camera_pan.frame + DT_DRAG_ACTIVE, 0u);
    wc3_wr32g(g_camera_pan.frame + DT_DRAG_MARK, 0u);

    float target[3] = { 0.0f, 0.0f, 0.0f };
    float world[3] = { 0.0f, 0.0f, 0.0f };
    dt_camera_vec_read(g_camera_pan.frame + DT_DRAG_TARGET, target);
    dt_camera_vec_read(g_camera_pan.frame + DT_DRAG_WORLD, world);
#if WC3_DEBUG && WC3_DEBUG_INPUT
    dt_log("cam-apply-out ptr=%u gid=%u "
           "target=%08X/%08X/%08X world=%08X/%08X/%08X",
           p->pointer, p->gesture_id,
           dt_bits(target[0]), dt_bits(target[1]), dt_bits(target[2]),
           dt_bits(world[0]), dt_bits(world[1]), dt_bits(world[2]));
#endif
    if (dt_camera_vec_finite(target) && dt_camera_vec_finite(world)) {
        memcpy(g_camera_pan.target, target, sizeof(target));
        memcpy(p->last_world, world, sizeof(world));
    }
    p->pending = 0;
    p->last_x = px;
    p->last_y = py;
}

static void dt_camera_pan_apply(CPU *c, const DtContext *ctx,
                                uint32_t width, uint32_t height)
{
    (void)ctx;
    if (!c || !g_camera_pan.active) return;
    int pending = 0;
    for (uint32_t i = 0; i < DT_CAMERA_PAN_MAX; i++) {
        if (g_camera_pan.pointers[i].active &&
            g_camera_pan.pointers[i].pending) {
            pending = 1;
            break;
        }
    }
    if (!pending) return;
    /* A queued native click must be dispatched by the Win32 pump first; the
     * camera update is kept pending for the next service call. */
    if (hle_android_native_button_pending()) return;
    if (!g_camera_pan.frame ||
        !dt_guest_readable(g_camera_pan.frame, DT_DRAG_MARK + 4u)) {
        dt_camera_pan_reset();
        return;
    }

    if (dt_camera_conflict_active()) {
        dt_camera_pan_rebase_conflict(c, width, height);
        return;
    }

    uint32_t saved[3];
    dt_camera_scope_save(saved);
    for (uint32_t i = 0; i < DT_CAMERA_PAN_MAX; i++) {
        Wc3CameraPanPointer *p = &g_camera_pan.pointers[i];
        if (!p->active || !p->pending) continue;
        float px = 0.0f;
        float py = 0.0f;
        if (!dt_camera_event_point(c, p->pending_x, p->pending_y,
                                   width, height, &px, &py)) {
            p->pending = 0;
            continue;
        }
        dt_camera_pan_update(c, p, px, py);
        dt_camera_pan_rebase_others(c, p);
    }
    dt_camera_scope_restore(saved);

    if (g_camera_pan.rebase_pending && g_camera_pan.camera) {
        const uint32_t out = dt_scratch_alloc();
        if (out) {
            const uint32_t args[1] = { out };
            (void)dt_call_thiscall(c, g_camera_pan.camera,
                                   DT_FN_CAMERA_READ_TARGET, 1, args);
            float live[3] = { 0.0f, 0.0f, 0.0f };
            dt_camera_vec_read(out, live);
            if (dt_camera_vec_finite(live)) {
                memcpy(g_camera_pan.target, live, sizeof(live));
                dt_camera_vec_write(g_camera_pan.frame + DT_DRAG_TARGET,
                                    g_camera_pan.target);
            }
            dt_scratch_free(out);
        }
        g_camera_pan.rebase_pending = 0;
    }
}

/*
 * Field selection and long-tap order.  Legacy longPressDragAction is
 * MOUSE_LEFT_BUTTON, so the native game draws and commits its own selection
 * rectangle.  The recognizer emits BEGIN at the configured long-press
 * deadline and END on release.  For a controlled selection in the field the
 * executor withholds the native LMB at BEGIN: a post-deadline move beyond the
 * configured drag threshold emits DRAG_BEGIN, which starts the native
 * selection rectangle at the hold point; a stationary release becomes a
 * smart order over the live selection (unit/decoration) or attack-move
 * (terrain), not a selection click.  Every other case keeps the native
 * press-at-BEGIN behavior.  MOVE/END reuse the native pointer state machine
 * so a rapid gesture gets the same queued-release handling as a HUD click.
 */

/* True when the point is inside a published HUD rectangle.  These are the
 * same snapshots the controls layer uses for its route priority; no
 * frame-registry scan or DFS is involved. */
static int dt_field_hud_contains(float x, float y)
{
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    if (wc3_hle_direct_touch_minimap_input_rect(&left, &top, &right, &bottom) &&
        wc3_dt_point_in_rect(x, y, left, top, right, bottom)) {
        return 1;
    }
    if (wc3_hle_direct_touch_beacon_rect(&left, &top, &right, &bottom) &&
        wc3_dt_point_in_rect(x, y, left, top, right, bottom)) {
        return 1;
    }
    if (wc3_hle_direct_touch_portrait_rect(&left, &top, &right, &bottom) &&
        wc3_dt_point_in_rect(x, y, left, top, right, bottom)) {
        return 1;
    }
    if (wc3_hle_direct_touch_command_rect(&left, &top, &right, &bottom) &&
        wc3_dt_point_in_rect(x, y, left, top, right, bottom)) {
        return 1;
    }
    if (wc3_hle_direct_touch_inventory_rect(&left, &top, &right, &bottom) &&
        wc3_dt_point_in_rect(x, y, left, top, right, bottom)) {
        return 1;
    }
    if (wc3_hle_direct_touch_grid_rect(&left, &top, &right, &bottom) &&
        wc3_dt_point_in_rect(x, y, left, top, right, bottom)) {
        return 1;
    }
    return 0;
}

/*
 * Smart-order gate for a long tap without drag.  No pick happens here: the
 * target is resolved on release, so the gate depends only on the live
 * selection and the contact's ownership of the field.  A target/placement
 * mode, a paused game, a HUD edge or another native button owner keeps the
 * ordinary native press/click path.
 */
static int dt_smart_order_ready(CPU *c, const DtContext *ctx,
                                const Wc3DirectTouchEvent *ev)
{
    if (!c || !ctx || !ctx->ui || !ctx->world) return 0;
    return wc3_dt_smart_order_candidate(
        dt_selection_controlled(c, ctx),
        dt_field_contains(ev->x, ev->y),
        dt_field_hud_contains(ev->x, ev->y),
        dt_field_target_active(c, ctx),
        dt_paused(c),
        g_native_down || g_native_up_pending ||
            (!g_native_release.left && hle_android_mouse_is_down(0)));
}

static void dt_field_select_begin(CPU *c, const DtContext *ctx,
                                  const Wc3DirectTouchEvent *ev)
{
    dt_log("field-select-begin ptr=%u n=%.4f,%.4f active=%d deferred=%d "
           "native_down=%d up_pending=%d hw_down=%d",
           ev->pointer_id, ev->x, ev->y, g_field_drag.select_active,
           g_field_drag.select_deferred, g_native_down,
           g_native_release.left, hle_android_mouse_is_down(0));
    if (g_field_drag.select_active) {
        Wc3DirectTouchEvent release = *ev;
        release.kind = WC3_DIRECT_TOUCH_POINTER_UP;
        dt_native_pointer(NULL, &release);
        g_field_drag.select_active = 0;
        g_field_drag.select_pointer = 0;
    }
    g_field_drag.select_deferred = 0;
    g_field_drag.select_deferred_pointer = 0;
    g_field_drag.select_deferred_x = 0.0f;
    g_field_drag.select_deferred_y = 0.0f;
    if (g_native_down || g_native_up_pending ||
        (!g_native_release.left && hle_android_mouse_is_down(0))) {
        /* A HUD click or another native press still owns the left button.
         * Do not steal it: the selection drag is dropped instead of posting
         * a DOWN whose matching UP would cancel the other owner. */
        hle_android_mouse_move(ev->x, ev->y);
        dt_trace("field-select-busy", ev->pointer_id, 0);
        return;
    }
    if (dt_smart_order_ready(c, ctx, ev)) {
        g_field_drag.select_deferred = 1;
        g_field_drag.select_deferred_pointer = (int)ev->pointer_id;
        g_field_drag.select_deferred_x = ev->x;
        g_field_drag.select_deferred_y = ev->y;
        dt_trace("field-select-deferred", dt_bits(ev->x), dt_bits(ev->y));
        dt_log("field-select-deferred ptr=%u", ev->pointer_id);
        return;
    }
    dt_native_begin(ev->x, ev->y);
    g_field_drag.select_active = 1;
    g_field_drag.select_pointer = (int)ev->pointer_id;
    dt_trace("field-select-begin", dt_bits(ev->x), dt_bits(ev->y));
}

/* The deferred long tap crossed the drag threshold.  Start the native
 * selection rectangle at the hold point, not at the current finger point; the
 * following FIELD_SELECT_MOVE carries the current point after the input
 * barrier publishes the DOWN. */
static void dt_field_select_drag_begin(CPU *c, const DtContext *ctx,
                                       const Wc3DirectTouchEvent *ev)
{
    if (g_field_drag.select_active) return;
    if (!g_field_drag.select_deferred ||
        (int)ev->pointer_id != g_field_drag.select_deferred_pointer) {
        return;
    }

    const int pointer = g_field_drag.select_deferred_pointer;
    g_field_drag.select_deferred = 0;
    g_field_drag.select_deferred_pointer = 0;

    if (dt_field_target_active(c, ctx)) {
        dt_field_target_gesture_begin(ev);
        dt_trace("field-select-drag-target", ev->pointer_id, 0);
        return;
    }
    if (dt_paused(c)) {
        dt_trace("field-select-drag-paused", ev->pointer_id, 0);
        return;
    }
    if (!dt_field_contains(ev->x, ev->y) ||
        dt_field_hud_contains(ev->x, ev->y)) {
        dt_trace("field-select-drag-hud", dt_bits(ev->x), dt_bits(ev->y));
        return;
    }
    if (g_native_down || g_native_up_pending ||
        (!g_native_release.left && hle_android_mouse_is_down(0))) {
        dt_trace("field-select-drag-busy", ev->pointer_id, 0);
        return;
    }

    dt_native_begin(ev->x, ev->y);
    g_field_drag.select_active = 1;
    g_field_drag.select_pointer = pointer;
    dt_trace("field-select-drag-begin", dt_bits(ev->x), dt_bits(ev->y));
}

static void dt_field_select_move(const Wc3DirectTouchEvent *ev)
{
    if (!g_field_drag.select_active ||
        (int)ev->pointer_id != g_field_drag.select_pointer) {
        dt_log("field-select-move ignored ptr=%u active=%d deferred=%d "
               "owner=%d",
               ev->pointer_id, g_field_drag.select_active,
               g_field_drag.select_deferred, g_field_drag.select_pointer);
        return;
    }
    Wc3DirectTouchEvent move = *ev;
    move.kind = WC3_DIRECT_TOUCH_POINTER_MOVE;
    dt_native_pointer(NULL, &move);
}

static void dt_field_select_end(CPU *c, const DtContext *ctx,
                                const Wc3DirectTouchEvent *ev,
                                uint32_t width, uint32_t height)
{
    dt_log("field-select-end ptr=%u n=%.4f,%.4f active=%d deferred=%d "
           "owner=%d hold=%.4f,%.4f",
           ev->pointer_id, ev->x, ev->y, g_field_drag.select_active,
           g_field_drag.select_deferred, g_field_drag.select_pointer,
           g_field_drag.select_deferred_x,
           g_field_drag.select_deferred_y);

    if (g_field_drag.select_deferred &&
        (int)ev->pointer_id == g_field_drag.select_deferred_pointer) {
        const float hold_x = g_field_drag.select_deferred_x;
        const float hold_y = g_field_drag.select_deferred_y;
        Wc3DirectTouchEvent hold = *ev;
        hold.x = hold_x;
        hold.y = hold_y;
        g_field_drag.select_deferred = 0;
        g_field_drag.select_deferred_pointer = 0;
        g_field_drag.select_deferred_x = 0.0f;
        g_field_drag.select_deferred_y = 0.0f;

        /* A reset/cancel path emits END with time_ms 0.  It must not turn a
         * held gesture into an order. */
        if (ev->time_ms == 0) {
            dt_trace("field-select-deferred-reset", ev->pointer_id, 0);
            return;
        }
        if (g_native_down || g_native_up_pending ||
            (!g_native_release.left && hle_android_mouse_is_down(0))) {
            dt_trace("field-select-deferred-busy", ev->pointer_id, 0);
            return;
        }
        if (!ctx || !ctx->ui || !ctx->world) {
            dt_trace("field-select-deferred-drop", ev->pointer_id, 0);
            return;
        }
        /* A HUD edge is not a world order and must not receive the fallback
         * click either: dropping it keeps the minimap/command panel edge
         * free of gesture leakage.  Check both endpoints because the target
         * is resolved at the hold point while the release can drift. */
        if (dt_field_hud_contains(ev->x, ev->y) ||
            dt_field_hud_contains(hold.x, hold.y)) {
            dt_trace("field-select-deferred-hud", dt_bits(hold.x),
                     dt_bits(hold.y));
            return;
        }
        if (dt_paused(c)) {
            /* The pause menu is native UI: its modal loop may stop polling
             * GetCursorPos, so this late click keeps the pump fallback. */
            dt_native_press_deferred(ev, 1);
            dt_trace("field-select-deferred-paused", ev->pointer_id, 0);
            return;
        }
        if (dt_field_target_active(c, ctx)) {
            dt_native_click(ev);
            dt_trace("field-select-deferred-target", ev->pointer_id, 0);
            return;
        }
        if (dt_smart_order_ready(c, ctx, &hold)) {
            const DtOrderResult order =
                dt_long_tap_at(c, ctx, &hold, width, height);
            dt_log("field-order ptr=%u result=%d", ev->pointer_id, order);
            dt_trace(order == DT_ORDER_SELECT
                         ? "field-long-tap-select"
                         : order == DT_ORDER_ATTACK_MOVE
                               ? "field-attack-move"
                               : "field-smart-order",
                     dt_bits(hold.x), dt_bits(hold.y));
            (void)order;
            return;
        }
        /* A live field click is not a modal native loop: wait for the idle
         * hover pass instead of forcing a stale hot control. */
        dt_native_press_deferred(ev, 0);
        dt_trace("field-select-deferred-click", dt_bits(ev->x),
                 dt_bits(ev->y));
        return;
    }

    if (!g_field_drag.select_active ||
        (int)ev->pointer_id != g_field_drag.select_pointer) {
        return;
    }
    Wc3DirectTouchEvent release = *ev;
    release.kind = WC3_DIRECT_TOUCH_POINTER_UP;
    dt_native_pointer(NULL, &release);
    g_field_drag.select_active = 0;
    g_field_drag.select_pointer = 0;
    dt_trace("field-select-end", dt_bits(ev->x), dt_bits(ev->y));
}

/* Release a field drag without an END event (map change, reset, stale queue
 * entry).  The recognizer's END event is preferred; this is the safety net
 * that keeps a native button from staying latched.  A deferred press owns no
 * native button, so it is cleared without a synthetic UP. */
static void dt_field_reset(void)
{
    g_field_drag.target_active = 0;
    g_field_drag.target_pointer = 0;
    g_field_drag.select_deferred = 0;
    g_field_drag.select_deferred_pointer = 0;
    g_field_drag.select_deferred_x = 0.0f;
    g_field_drag.select_deferred_y = 0.0f;
    if (g_field_drag.select_active) {
        dt_log("field-reset select ptr=%d native_down=%d",
               g_field_drag.select_pointer, g_native_down);
        if (g_native_down) {
            dt_native_end();
        }
        g_field_drag.select_active = 0;
        g_field_drag.select_pointer = 0;
    }
}

static int dt_field_drag_active(void)
{
    return g_field_drag.select_active || g_field_drag.target_active;
}

static int dt_is_field_drag_kind(uint32_t kind)
{
    return kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN ||
           kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE ||
           kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END ||
           kind == WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN ||
           kind == WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE ||
           kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END ||
           kind == WC3_DIRECT_TOUCH_FIELD_SELECT_DRAG_BEGIN;
}

static int dt_is_field_drag_end(uint32_t kind)
{
    return kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END ||
           kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END;
}

/* Pure motion never changes the game's input mode, so re-running the exact
 * readiness check (three guest dispatches) after every MOVE is wasted work.
 * Everything else - taps, button presses/releases, cancellations and the
 * portrait follow - can post a native click or change the mode and keeps the
 * old, unconditional re-check. */
static int dt_event_can_change_mode(uint32_t kind)
{
    switch (kind) {
    case WC3_DIRECT_TOUCH_POINTER_MOVE:
    case WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE:
    case WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE:
    case WC3_DIRECT_TOUCH_FIELD_HOVER:
    case WC3_DIRECT_TOUCH_FOLLOW_START:
        return 0;
    default:
        return 1;
    }
}

/* Post one native press (optionally with its release) through the universal
 * hot-control barrier.  The barrier orders the press behind the idle hover
 * pass; it does not add a LEFT UP release, because a release at the new
 * cursor position would consume the menu click before DOWN is seen.  The HLE
 * posts the move in both paths, so callers must not post a separate move for
 * the press itself. */
static void dt_press_native(float x, float y, int button, int release)
{
    /* A new press supersedes an armed camera start, as the native DOWN path
     * always did.  The hot-control barrier itself is owned by the HLE: no
     * caller here can decide to bypass it. */
    (void)hle_android_mouse_cancel_pending_camera();
    hle_android_mouse_press(x, y, button, release);
}

/*
 * A semantic gesture can be recognized just as the game enters targeting or
 * placement.  The controls layer decided FIELD on DOWN, so the event arrives
 * as TAP/LONG_PRESS, but the native UI now owns the click.  Replay it as a
 * native click instead of dropping the user's target selection.
 */
static void dt_native_click(const Wc3DirectTouchEvent *ev)
{
    /* The caller already resolved the route and decided that the game owns
     * this click.  Do not let the raw field deferral re-queue it as a tap:
     * that would replay a target confirmation through the select path. */
    if (g_native_down) {
        /* Another owner still holds the left button.  Posting only this
         * click's UP would release that owner's press; dropping the click is
         * the safe outcome because the target mode is already changing. */
        dt_trace("native-click-busy", dt_bits(ev->x), dt_bits(ev->y));
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_INPUT
    wc3_hle_direct_touch_probe_mark("native-click-down", dt_bits(ev->x),
                                    dt_bits(ev->y));
#endif
    /* Already validated by the resolver: restore HEAD's immediate replay so
     * the placement loop sees the confirmation in this pump. */
    hle_android_mouse_press_replay(ev->x, ev->y, 0);
    dt_native_replay_done();
#if WC3_DEBUG && WC3_DEBUG_INPUT
    wc3_hle_direct_touch_probe_mark("native-click-up", g_native_down, 0);
#endif
}

/*
 * A touch-originated native click (field unit selection or a late-resolved
 * native UI release) enters the universal gateway.  The DOWN is armed at the
 * release point; the gateway publishes it after the idle hover pass, and the
 * service posts this UP one pump later.  The old immediate replay hit-tested
 * the stale hot control, so the first tap only highlighted the previously
 * hovered button or selected nothing in the world.
 *
 * `modal_fallback` is 1 only for a late-resolved native UI click whose guest
 * loop may stop polling GetCursorPos (the pause menu).  A field selection
 * must keep it 0 and wait for the real idle pass: forcing its DOWN from the
 * message-pump boundary is the first-world-tap regression.  Main guest thread
 * only.
 */
static void dt_native_press_deferred(const Wc3DirectTouchEvent *ev,
                                     int modal_fallback)
{
    if (g_native_down || g_native_up_pending) {
        dt_trace("native-press-deferred-busy", dt_bits(ev->x),
                 dt_bits(ev->y));
        return;
    }
    dt_native_begin(ev->x, ev->y);
    g_native_up_pending = 1;
    g_native_up_wait = 0;
    g_native_up_modal = modal_fallback;
    dt_trace("native-press-deferred", dt_bits(ev->x), dt_bits(ev->y));
}

/* A field contact was held only until this release so it could be classified
 * as a tap.  Resolve it now with the exact route and the live world:
 *
 *   FIELD      semantic select/order (the reason direct touch exists)
 *   target     validated target/placement confirmation, immediate replay
 *   native UI  ordinary menu/HUD click through the idle gateway
 *   loading    the separate map-load replay, not a panel tap
 *   no UI      explicit drop
 *
 * There is no "keep waiting for the next tap" outcome: the user's contact is
 * always consumed exactly once on its own release. */
static void dt_field_release_tap(CPU *c, const Wc3DirectTouchEvent *ev,
                                 uint32_t width, uint32_t height)
{
#if WC3_DEBUG && WC3_DEBUG_INPUT
    wc3_hle_direct_touch_probe_mark("field-release-enter", dt_bits(ev->x),
                                    dt_bits(ev->y));
#endif
    DtContext ctx;
    memset(&ctx, 0, sizeof ctx);
    const Wc3DirectTouchRoute route = dt_ready_route(c, &ctx);
    const int ui_present = dt_ui_present(NULL);
    /* A closed click gate during a target/transient mode is not a map-load
     * deferral.  The game is in control (route NATIVE); the release must be a
     * native click, not a replay queued until select mode returns. */
    const int should_defer =
        route != WC3_DIRECT_TOUCH_ROUTE_NATIVE &&
        dt_field_click_should_defer(ev->x, ev->y);
    const uint32_t outcome =
        (uint32_t)wc3_dt_field_release(route == WC3_DIRECT_TOUCH_ROUTE_FIELD,
                                       should_defer, ui_present);
    /* Only a live target/placement mode may bypass the idle gateway.  A
     * menu, loading screen or stale target word on a LEGACY route is an
     * ordinary native UI click and must wait for the hover pass. */
    const int native_replay =
        wc3_dt_native_release_replay(route,
                                     dt_field_target_tap_ready(c, ev->x,
                                                               ev->y));
#if WC3_DEBUG && WC3_DEBUG_INPUT
    wc3_hle_direct_touch_probe_mark("field-release-route", (uint32_t)route,
                                    (outcome << 8) |
                                    ((uint32_t)ui_present << 16) |
                                    (uint32_t)should_defer << 24);
    wc3_hle_direct_touch_probe_mark("field-release-native-mode",
                                    (uint32_t)native_replay, 0);
#endif
    switch (outcome) {
    case WC3_DT_FIELD_RELEASE_SEMANTIC: {
        Wc3DirectTouchEvent tap = *ev;
        tap.kind = WC3_DIRECT_TOUCH_TAP;
        tap.route = WC3_DIRECT_TOUCH_ROUTE_FIELD;
        dt_tap(c, &ctx, &tap, width, height);
        dt_trace("field-tap-release", dt_bits(ev->x), dt_bits(ev->y));
#if WC3_DEBUG && WC3_DEBUG_INPUT
        wc3_hle_direct_touch_probe_mark("field-release-semantic",
                                        dt_bits(ev->x), dt_bits(ev->y));
#endif
        break;
    }
    case WC3_DT_FIELD_RELEASE_DEFER:
        dt_field_click_queue(ev);
        dt_trace("field-tap-load-queue", dt_bits(ev->x), dt_bits(ev->y));
#if WC3_DEBUG && WC3_DEBUG_INPUT
        wc3_hle_direct_touch_probe_mark("field-release-defer",
                                        dt_bits(ev->x), dt_bits(ev->y));
#endif
        break;
    case WC3_DT_FIELD_RELEASE_NATIVE:
#if WC3_DEBUG && WC3_DEBUG_INPUT
        wc3_hle_direct_touch_probe_mark("field-release-native-pre",
                                        dt_bits(ev->x), dt_bits(ev->y));
#endif
        /* Native target confirmation is not dt_tap(): it must not be blocked
         * by dt_paused().  An open pause menu still owns the click and uses
         * the gateway; `native_replay` already excludes it.  A live target
         * click is queued for one frame so the placement preview catches up
         * with the release hover before the mode resolves the click. */
        if (native_replay && !dt_pause_menu_active()) {
            dt_field_target_click_queue(ev);
            dt_trace("field-tap-release-native-queue", dt_bits(ev->x),
                     dt_bits(ev->y));
        } else {
            if (native_replay) dt_trace("field-release-paused", 0, 0);
            /* A native UI release may sit in a modal menu loop that does not
             * poll GetCursorPos; keep the pump fallback for it. */
            dt_native_press_deferred(ev, 1);
            dt_trace("field-tap-release-deferred", dt_bits(ev->x),
                     dt_bits(ev->y));
        }
#if WC3_DEBUG && WC3_DEBUG_INPUT
        wc3_hle_direct_touch_probe_mark("field-release-native-post",
                                        dt_bits(ev->x), dt_bits(ev->y));
#endif
        break;
    case WC3_DT_FIELD_RELEASE_DROP:
    default:
        dt_trace("field-tap-release-drop", dt_bits(ev->x), dt_bits(ev->y));
#if WC3_DEBUG && WC3_DEBUG_INPUT
        wc3_hle_direct_touch_probe_mark("field-release-drop",
                                        dt_bits(ev->x), dt_bits(ev->y));
#endif
        break;
    }
}

/*
 * Inventory item gestures.  The native inventory button registers 0x30065:
 * a left click uses the item, a right click enters move mode, and the next
 * left click on a slot or in the world confirms the move.  Direct touch
 * consumes a gesture that starts on an occupied slot and replays that click
 * pair on release.  Item use, swap and drop then stay on the exact native
 * path instead of re-deriving the target-mode fields.
 *
 * Only the pointer that landed on the item owns the gesture.  A second DOWN
 * is swallowed; a MOVE/UP from another pointer is forwarded so a native
 * button cannot stay held.
 */
static void dt_inventory_reset(void)
{
    g_inv.active = 0;
    g_inv.pointer = 0;
    g_inv.bar = 0;
    g_inv.slot = 0;
    g_inv.down_x = 0.0f;
    g_inv.down_y = 0.0f;
}

/* Resolve CInfoBar+0x148 -> CInventoryBar from the live UI singleton. */
static uint32_t dt_inventory_live_bar(void)
{
    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui || !dt_guest_readable(ui, DT_UI_INFO_BAR + 4u)) return 0;
    const uint32_t infobar = wc3_rd32g(ui + DT_UI_INFO_BAR);
    if (!infobar || !dt_guest_readable(infobar, DT_INFO_INVENTORY + 4u)) {
        return 0;
    }
    return wc3_rd32g(infobar + DT_INFO_INVENTORY);
}

/* Resolve the live inventory bar only in the normal in-game UI.  Target mode
 * owns the click while it is active, so a gesture there is forwarded to the
 * native handler unchanged. */
static int dt_inventory_context(uint32_t *bar_out)
{
    if (!bar_out) return 0;
    *bar_out = 0;

    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui || !dt_guest_readable(ui, DT_UI_INFO_BAR + 4u)) return 0;
    if (!wc3_rd32g(ui + DT_UI_WORLD_FRAME)) return 0;
    if (!wc3_rd32g(ui + DT_UI_CLICK_GATE)) return 0;
    if (!wc3_rd32g(ui + DT_UI_SELECT_MODE)) return 0;
    if (dt_grid_targeting(ui)) return 0;

    const uint32_t bar = dt_inventory_live_bar();
    if (!bar || !dt_guest_readable(bar, DT_INV_ARRAY + 4u)) return 0;
    const uint32_t cap = wc3_rd32g(bar + DT_INV_CAP);
    if (!cap || cap > DT_INV_MAX_SLOTS) return 0;

    *bar_out = bar;
    return 1;
}

static int dt_inventory_has_item(uint32_t entry, uint32_t button)
{
    if (wc3_rd32g(entry + DT_INV_ENTRY_ITEM)) return 1;
    const uint32_t data = wc3_rd32g(button + DT_INV_BUTTON_DATA);
    if (!data || !dt_guest_readable(data, DT_INV_DATA_ITEM + 4u)) return 0;
    return wc3_rd32g(data + DT_INV_DATA_ITEM) != 0;
}

/*
 * Return the slot under the point, or -1.  need_item restricts the search to
 * occupied slots for the gesture source; the destination search accepts an
 * empty slot so a drag can move the item into it.  The rect is read without
 * the frame visibility test because the six inventory buttons stay laid out
 * even when an individual slot has no item icon.
 */
static int dt_inventory_slot_at(uint32_t bar, float nx, float ny,
                                uint32_t width, uint32_t height,
                                int need_item, uint32_t *button_out)
{
    if (button_out) *button_out = 0;
    if (!bar || !dt_guest_readable(bar, DT_INV_ARRAY + 4u)) return -1;
    const uint32_t cap = wc3_rd32g(bar + DT_INV_CAP);
    if (!cap || cap > DT_INV_MAX_SLOTS) return -1;
    const uint32_t array = wc3_rd32g(bar + DT_INV_ARRAY);
    if (!array || !dt_guest_readable(array, cap * 8u)) return -1;

    for (uint32_t i = 0; i < cap; i++) {
        const uint32_t entry = array + i * 8u;
        const uint32_t button = wc3_rd32g(entry + DT_INV_ENTRY_BUTTON);
        if (!button ||
            !dt_guest_readable(button, DT_INV_BUTTON_DATA + 4u)) {
            continue;
        }
        if (need_item && !dt_inventory_has_item(entry, button)) continue;

        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        if (!dt_console_rect_read(button, width, height, &left, &top,
                                  &right, &bottom)) {
            continue;
        }
        if (!(nx >= left && nx < right && ny >= top && ny < bottom)) {
            continue;
        }
        if (button_out) *button_out = button;
        return (int)i;
    }
    return -1;
}

static int dt_field_contains(float nx, float ny)
{
    return nx >= DT_INV_FIELD_LEFT && nx <= DT_INV_FIELD_RIGHT &&
           ny >= DT_INV_FIELD_TOP && ny <= DT_INV_FIELD_BOTTOM;
}

/* A release over a live HUD panel is never a battlefield point.  The command
 * card, inventory bar and unit grid all overlap the field rectangle at the
 * bottom edge; without this exclusion a release on their top row would turn
 * an item drag into a world drop.
 *
 * Use the same gated rectangles as the controls layer and the field
 * recognizer.  In particular the minimap panel snapshot can hold a whole-HUD
 * or whole-screen frame while the game builds the HUD; the published
 * minimap-input rectangle applies wc3_dt_minimap_panel_plausible() and falls
 * back to the map frame, but a raw dt_rect_read() of g_minimap_panel_snapshot
 * would claim every release in the field as HUD. */
static int dt_inventory_hud_contains(float nx, float ny)
{
    return dt_field_hud_contains(nx, ny);
}

/* Replay a native left click from a guest-thread modal path.  Use this only
 * for an already-resolved target/placement confirmation: the modal loop may
 * never call GetCursorPos, so the idle gateway slot cannot publish it.  A
 * touch-originated field selection must use dt_native_press_deferred(ev, 0)
 * so its DOWN is hit-tested only after the idle hover pass refreshed hot
 * control. */
static void dt_native_left_click(float x, float y)
{
    hle_android_mouse_press_replay(x, y, 0);
    dt_native_replay_done();
}

/* Replay a deferred field tap.  A map-load tap waits for the exact service
 * route FIELD; the semantic path gives a selected unit a smart order rather
 * than the left-click selection a native replay would do.  A live target
 * click and the attack-move confirmation both wait for the guest to build
 * their mode, then replay natively; revalidation cancels a stale click when
 * a new gesture or an open pause menu takes over. */
static void dt_field_click_tick(CPU *c, uint32_t width, uint32_t height)
{
    if (!g_field_click_pending) return;
    /* The replay decides select vs. target from the live mode.  Do not read
     * that mode while an earlier native click is still waiting in the Win32
     * queue; the mode belongs to the previous state until it is applied. */
    if (hle_android_native_button_pending()) return;

    /* A newer command-starting input supersedes the deferred click.  This is
     * the cheap early check; the authoritative check happens inside
     * hle_android_mouse_click_if_epoch() under the same g_uq_lock that the
     * input path uses to advance the epoch. */
    if (hle_android_input_epoch() != g_field_click_input_epoch) {
        dt_trace(g_field_click_attack_move ? "field-attack-move-superseded"
                                           : "field-click-superseded",
                 dt_bits(g_field_click_event.x),
                 dt_bits(g_field_click_event.y));
        g_field_click_pending = 0;
        g_field_click_attack_move = 0;
        return;
    }

    const uint64_t now = dt_now_ms();
    /* A live target/placement click and the A-key attack-move click share the
     * field-click slot but need a guest update before the click.  The mode
     * builds its target/placement preview from the hover on that update;
     * posting the click in the release pump made the first contact resolve
     * against the stale mode, so the user had to tap twice.  Revalidate the
     * mode here: a new gesture, a pause menu, a context loss or a mode exit
     * cancels the queued click. */
    if (g_field_click_native) {
        if (g_field_click_ready_ms && now && now < g_field_click_ready_ms)
            return;
        /* Let an already queued contact resolve first.  A new gesture cancels
         * the click in the event loop; a continuation (move/end) is harmless
         * and the click posts on the next pump.  This closes the window where
         * a contact arrived after the release but before this tick, with the
         * event loop not having reached it yet. */
        if (wc3_direct_touch_pending() != 0) {
            dt_trace("field-target-click-wait-queue",
                     wc3_direct_touch_pending(), 0);
            return;
        }
        if (dt_pause_menu_active() || !dt_field_load_context_alive()) {
            dt_trace("field-target-click-cancel",
                     dt_bits(g_field_click_event.x),
                     dt_bits(g_field_click_event.y));
            g_field_click_pending = 0;
            g_field_click_attack_move = 0;
            return;
        }
        if (!dt_field_target_tap_ready(c, g_field_click_event.x,
                                       g_field_click_event.y)) {
            /* The A key target mode appears on the guest update after the
             * release.  Keep the attack-move click queued until the game
             * reports it live; a new input advances the epoch and cancels it,
             * and a lost context cancels it above.  Target/placement has no
             * such retry: its mode was already live before the gesture, so a
             * missing mode means the context changed and the click must be
             * dropped. */
            if (g_field_click_attack_move) return;
            dt_trace("field-target-click-cancel",
                     dt_bits(g_field_click_event.x),
                     dt_bits(g_field_click_event.y));
            g_field_click_pending = 0;
            return;
        }
        if (g_native_down) {
            /* Another direct-touch gesture owns the left button.  Posting only
             * this click's UP would release that gesture's press. */
            dt_trace("native-click-busy", dt_bits(g_field_click_event.x),
                     dt_bits(g_field_click_event.y));
            g_field_click_pending = 0;
            g_field_click_attack_move = 0;
            return;
        }
#if WC3_DEBUG && WC3_DEBUG_INPUT
        wc3_hle_direct_touch_probe_mark("native-click-down",
                                        dt_bits(g_field_click_event.x),
                                        dt_bits(g_field_click_event.y));
#endif
        if (!hle_android_mouse_click_if_epoch(g_field_click_event.x,
                                              g_field_click_event.y, 0,
                                              g_field_click_input_epoch)) {
            /* The mode was live when read, but a new input won the queue lock
             * before the click was posted.  The input owns the next command;
             * drop this click instead of confirming a target for it. */
            dt_trace("field-click-superseded-race",
                     dt_bits(g_field_click_event.x),
                     dt_bits(g_field_click_event.y));
            g_field_click_pending = 0;
            g_field_click_attack_move = 0;
            return;
        }
        dt_native_replay_done();
        dt_trace(g_field_click_attack_move ? "field-attack-move-click"
                                           : "field-target-click-deferred",
                 dt_bits(g_field_click_event.x),
                 dt_bits(g_field_click_event.y));
#if WC3_DEBUG && WC3_DEBUG_INPUT
        wc3_hle_direct_touch_probe_mark("native-click-up", 0, 0);
#endif
        g_field_click_pending = 0;
        g_field_click_attack_move = 0;
        return;
    }

    /* The held map-load tap belongs to the context that deferred it.  A pause
     * menu, a return to the main menu or a map teardown destroys that
     * context; waiting is only for a load/fade-in that keeps it. */
    if (dt_pause_menu_active() || !dt_field_load_context_alive()) {
        dt_trace("field-tap-context-cancel",
                 dt_bits(g_field_click_event.x),
                 dt_bits(g_field_click_event.y));
        g_field_click_pending = 0;
        return;
    }

    /* A held tap happened before any gesture still waiting in the queue.
     * Replay it first: draining the new gesture first would cancel the held
     * tap and lose the first contact after a map load. */
    if (wc3_direct_touch_pending() != 0) {
        dt_trace("field-replay-before-queue", wc3_direct_touch_pending(), 0);
    }

    DtContext ctx;
    memset(&ctx, 0, sizeof ctx);
    if (dt_ready_route(c, &ctx) != WC3_DIRECT_TOUCH_ROUTE_FIELD) return;

#if WC3_DEBUG && WC3_DEBUG_INPUT
    /* The held map-load tap is a contact of its own even though its DOWN/UP
     * were drained while the world was not ready.  Start the map trace here
     * so the replay's native click and queue entries are captured too. */
    dt_map_note_contact("replay", WC3_DIRECT_TOUCH_TAP, 0,
                        g_field_click_event.x, g_field_click_event.y,
                        WC3_DIRECT_TOUCH_ROUTE_FIELD,
                        WC3_DIRECT_TOUCH_ROUTE_FIELD);
    wc3_hle_direct_touch_probe_mark("field-replay-pre",
                                    dt_bits(g_field_click_event.x),
                                    dt_bits(g_field_click_event.y));
#endif
    dt_tap(c, &ctx, &g_field_click_event, width, height);
    dt_trace("field-tap-replay", dt_bits(g_field_click_event.x),
             dt_bits(g_field_click_event.y));
#if WC3_DEBUG && WC3_DEBUG_INPUT
    g_map_contact_open = 0;
#endif
#if WC3_DEBUG && WC3_DEBUG_INPUT
    wc3_hle_direct_touch_probe_mark("field-replay-post",
                                    dt_bits(g_field_click_event.x),
                                    dt_bits(g_field_click_event.y));
#endif
    g_field_click_pending = 0;
}

/* Replay a native right click.  The inventory uses it to enter move mode; the
 * command card uses it to toggle autocast.  The pending flag is shared because
 * both paths drive the same physical button. */
static void dt_native_right_click(float x, float y)
{
    dt_log("native right click x=%.4f y=%.4f pending=%d down=%d",
           x, y, g_native_release.right, hle_android_mouse_is_down(2));
    if (g_native_release.right && !hle_android_mouse_is_down(2)) {
        g_native_release.right = 0;
    }
    hle_android_mouse_press_replay(x, y, 2);
    g_native_release.right = 1;
}

/*
 * Resolve the release through the pure release rule.  Same slot is a tap and
 * uses the item; another slot is a swap/move confirmed by a left click on
 * that slot; a field point is a drop confirmed by a left click at the world
 * position.  A HUD release cancels, and a tap that drifted out of its slot
 * still uses the item.  The destination rect is read once and supplies both
 * the decision and the native click centre.
 */
static void dt_inventory_finish(const Wc3DirectTouchEvent *ev,
                                uint32_t width, uint32_t height)
{
    const uint32_t bar = g_inv.bar;
    const uint32_t src_slot = g_inv.slot;
    const float down_x = g_inv.down_x;
    const float down_y = g_inv.down_y;

    /* The UI may have been replaced between DOWN and UP.  Never post a click
     * into a new inventory bar or a menu, and never start a move from a slot
     * that no longer holds an item. */
    uint32_t live_bar = 0;
    if (!dt_inventory_context(&live_bar) || live_bar != bar) {
        dt_trace("inv-context", src_slot, live_bar);
        dt_inventory_reset();
        return;
    }
    if (dt_inventory_slot_at(bar, down_x, down_y, width, height, 1,
                             NULL) != (int)src_slot) {
        dt_trace("inv-source", src_slot, 0);
        dt_inventory_reset();
        return;
    }

    uint32_t dest_button = 0;
    const int dest_slot = dt_inventory_slot_at(bar, ev->x, ev->y, width,
                                               height, 0, &dest_button);
    float dest_left = 0.0f;
    float dest_top = 0.0f;
    float dest_right = 0.0f;
    float dest_bottom = 0.0f;
    int dest_rect_ok = 0;
    if (dest_slot >= 0 && dest_button) {
        dest_rect_ok = dt_console_rect_read(dest_button, width, height,
                                            &dest_left, &dest_top,
                                            &dest_right, &dest_bottom);
    }

    const int tap_slop_ok = !wc3_dt_past_slop(down_x, down_y, ev->x, ev->y,
                                              DT_INV_TAP_SLOP);
    const int inside_field = dt_field_contains(ev->x, ev->y);
    const int inside_hud = dt_inventory_hud_contains(ev->x, ev->y);
    const Wc3DtInventoryRelease action =
        wc3_dt_inventory_release((int)src_slot, dest_slot, dest_rect_ok,
                                 tap_slop_ok, inside_field, inside_hud);
    dt_inventory_reset();

    switch (action) {
    case WC3_DT_INV_USE:
        dt_native_left_click(down_x, down_y);
        dt_trace("inv-tap", src_slot, 0);
        return;
    case WC3_DT_INV_MOVE_SLOT:
        dt_native_right_click(down_x, down_y);
        dt_native_left_click((dest_left + dest_right) * 0.5f,
                             (dest_top + dest_bottom) * 0.5f);
        dt_trace("inv-slot", src_slot, (uint32_t)dest_slot);
        return;
    case WC3_DT_INV_DROP_FIELD:
        dt_native_right_click(down_x, down_y);
        dt_native_left_click(ev->x, ev->y);
        dt_trace("inv-drop", src_slot, 0);
        return;
    case WC3_DT_INV_CANCEL:
    default:
        if (dest_slot >= 0 && !dest_rect_ok) {
            dt_trace("inv-dest-rect", src_slot, (uint32_t)dest_slot);
        } else if (inside_hud) {
            dt_trace("inv-hud", src_slot, 0);
        } else {
            dt_trace("inv-cancel", src_slot, 0);
        }
        return;
    }
}

static int dt_inventory_pointer(const Wc3DirectTouchEvent *ev,
                                uint32_t width, uint32_t height)
{
    if (!ev || ev->route != WC3_DIRECT_TOUCH_ROUTE_NATIVE) return 0;

    switch (ev->kind) {
    case WC3_DIRECT_TOUCH_POINTER_DOWN: {
        if (g_inv.active) return 1;
        /* A grid gesture already owns the screen contact; let the grid
         * handler swallow this second DOWN instead of starting a second
         * gesture here. */
        if (g_grid.active) return 0;

        uint32_t bar = 0;
        if (!dt_inventory_context(&bar)) return 0;
        const int slot = dt_inventory_slot_at(bar, ev->x, ev->y, width,
                                              height, 1, NULL);
        if (slot < 0) return 0;

        g_inv.active = 1;
        g_inv.pointer = ev->pointer_id;
        g_inv.bar = bar;
        g_inv.slot = (uint32_t)slot;
        g_inv.down_x = ev->x;
        g_inv.down_y = ev->y;
#if WC3_DEBUG && WC3_DEBUG_INPUT
        wc3_hle_direct_touch_probe_begin("inv-select", (uint32_t)slot, bar);
#endif
        dt_trace("inv-down", (uint32_t)slot, bar);
        return 1;
    }
    case WC3_DIRECT_TOUCH_POINTER_MOVE:
        if (!g_inv.active || ev->pointer_id != g_inv.pointer) return 0;
        return 1;
    case WC3_DIRECT_TOUCH_POINTER_UP:
        if (!g_inv.active || ev->pointer_id != g_inv.pointer) return 0;
        dt_inventory_finish(ev, width, height);
        return 1;
    case WC3_DIRECT_TOUCH_POINTER_CANCEL:
        if (!g_inv.active || ev->pointer_id != g_inv.pointer) return 0;
        dt_trace("inv-abort", g_inv.slot, 0);
        dt_inventory_reset();
        return 1;
    default:
        return 0;
    }
}

/*
 * Command-card helpers.  CGameUI+0x3C8 is the CCommandBar; its CGridPanel
 * fields hold 3 rows x 4 columns at +0x124/+0x128, the row array at +0x154,
 * and each row's four CCommandButton* at row+8.  A button is hittable only
 * when the frame is visible (+0x54), the control is shown (+0x94), and the
 * command data pointer (+0x190) is live.  +0x90 is the focus/enabled state,
 * not a visibility gate.
 */
static void dt_command_reset(void)
{
    g_cmd_active = 0;
    g_cmd_pointer = 0;
    g_cmd_bar = 0;
    g_cmd_button = 0;
    g_cmd_data = 0;
    g_cmd_type = 0;
    g_cmd_down_x = 0.0f;
    g_cmd_down_y = 0.0f;
    g_cmd_down_ms = 0;
    g_cmd_moved = 0;
    g_cmd_long_pending = 0;
    g_cmd_long_fired = 0;
}

/* The queued confirming click outlives the card gesture, so a context change
 * must drop it explicitly.  dt_command_reset() is also called at the end of a
 * drag release, after the click has just been queued, and therefore must not
 * touch this state. */
static void dt_command_target_reset(void)
{
    g_cmd_target.pending = 0;
    g_cmd_target.bar = 0;
    g_cmd_target.x = 0.0f;
    g_cmd_target.y = 0.0f;
    g_cmd_target.button = 0;
    g_cmd_target.data = 0;
    g_cmd_target.type = 0;
    g_cmd_target.mode = 0;
    g_cmd_target.ms = 0;
}

/* Resolve CGameUI+0x3C8 -> CCommandBar from the live UI singleton. */
static uint32_t dt_command_live_bar(void)
{
    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui || !dt_guest_readable(ui, DT_UI_COMMAND_BAR + 4u)) return 0;
    return wc3_rd32g(ui + DT_UI_COMMAND_BAR);
}

/* Resolve the live command bar only in the normal in-game UI.  Target mode
 * owns the click while it is active, so a gesture there is forwarded to the
 * native handler unchanged.
 *
 * The click gate is required when a gesture starts on the card (the card is
 * only clickable in select mode), but not when a drag release is resolved:
 * the command's own target mode closes the gate, and requiring it there made
 * the release drop the queued confirmation.  The world/bar identity is the
 * release context. */
static int dt_command_context(uint32_t *bar_out, int allow_targeting,
                              int require_gate)
{
    if (!bar_out) return 0;
    *bar_out = 0;

    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) {
        dt_log("cmd-ctx no-ui-global");
        return 0;
    }
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui || !dt_guest_readable(ui, DT_UI_COMMAND_BAR + 4u)) {
        dt_log("cmd-ctx no-ui ui=%08X", ui);
        return 0;
    }
    const uint32_t world = wc3_rd32g(ui + DT_UI_WORLD_FRAME);
    const uint32_t gate = wc3_rd32g(ui + DT_UI_CLICK_GATE);
    const uint32_t select_mode = wc3_rd32g(ui + DT_UI_SELECT_MODE);
    const int targeting = dt_grid_targeting(ui);
    if (!world || (require_gate && !gate) || !select_mode ||
        (!allow_targeting && targeting)) {
        dt_log("cmd-ctx-fail world=%08X gate=%08X sel=%08X tgt=%d allow=%d "
               "req=%d", world, gate, select_mode, targeting, allow_targeting,
               require_gate);
        return 0;
    }

    const uint32_t bar = dt_command_live_bar();
    if (!bar || !dt_guest_readable(bar, DT_CMD_ROW_ARRAY + 4u)) {
        dt_log("cmd-ctx-no-bar bar=%08X", bar);
        return 0;
    }

    *bar_out = bar;
    return 1;
}

/* The game's own command target-mode state.  The word at command data +0x8 is
 * not a reliable type across the observed commands: T2 saw small pairs and
 * ASCII-like values from adjacent data, while commands that clearly entered
 * target mode had the same kind of value.  Gating the release on that word
 * dropped every drag confirmation.  The live input mode is the authoritative
 * signal: selecting a command leaves CSelectMode, and confirming the target
 * returns to it.  The select-mode object is also the only mode a field drag
 * can set, so a non-select mode during a command gesture is the target mode;
 * the dedicated camera mode is the one exception and is never confirmed. */
static int dt_command_targeting(uint32_t type)
{
    (void)type;
    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui) return 0;
    const uint32_t active = dt_ui_mode_read(ui, DT_UI_ACTIVE_MODE);
    const uint32_t select = dt_ui_mode_read(ui, DT_UI_SELECT_MODE);
    const uint32_t camera = dt_ui_mode_read(ui, DT_UI_CAMERA_MODE);
    /* The target mode keeps the click gate open in 1.27b, so the gate is not
     * evidence either way.  The live mode decides, and the first non-select
     * mode seen after the card click is remembered. */
    if (!wc3_dt_command_target_live(active, select)) return 0;
    /* A camera drag is not a command target.  If one is live here (camera
     * started before the card, or a leaked mode switch), confirming would
     * post the target click into the camera mode. */
    if (wc3_dt_camera_mode_active(active, camera)) {
        dt_trace("cmd-target-camera", active, camera);
        return 0;
    }
    /* The target mode object is stable for the command.  Record the first one
     * seen after the card click and require the same object: the field select
     * drag mode is also non-select, but it is a different object. */
    if (!g_cmd_target.mode) {
        g_cmd_target.mode = active;
        return 1;
    }
    return g_cmd_target.mode == active;
}

/*
 * Exact 0x6F3B69C0: return the entry index (0 or 1) for the mouse mask, or -1
 * when the command data has no usable entry for that button.  A left entry
 * requires flags != 0, bit 0x40 clear and bit 0x2000 clear; a right entry
 * requires the same first two conditions and bit 0x2000 set.
 */
static int dt_command_entry(uint32_t data, uint32_t mask)
{
    if (!data || !dt_guest_readable(data, DT_CMD_DATA_FLAGS + 8u)) return -1;

    for (uint32_t i = 0; i < 2; i++) {
        const uint32_t flags =
            wc3_rd32g(data + DT_CMD_DATA_FLAGS + i * 4u);
        if (!flags) continue;
        if (flags & 0x40u) continue;
        if (mask == DT_CMD_ENTRY_LEFT) {
            if (!(flags & 0x2000u)) return (int)i;
        } else if (mask == DT_CMD_ENTRY_RIGHT) {
            if (flags & 0x2000u) return (int)i;
        }
    }
    return -1;
}

/* A targeted/area command needs a world point or object after the button
 * click.  The flag word comes from the left entry selected above.  This is a
 * diagnostic only: the live target mode, not this flag, decides a release. */
#if WC3_DEBUG && WC3_DEBUG_INPUT
static int dt_command_targeted(uint32_t data)
{
    const int index = dt_command_entry(data, DT_CMD_ENTRY_LEFT);
    if (index < 0) return 0;
    const uint32_t flags =
        wc3_rd32g(data + DT_CMD_DATA_FLAGS + (uint32_t)index * 4u);
    return (flags & DT_CMD_TARGET_FLAGS) != 0;
}
#endif

/* Return the command data under the point and store the button, or 0. */
static uint32_t dt_command_button_at(uint32_t bar, float nx, float ny,
                                     uint32_t width, uint32_t height,
                                     uint32_t *button_out)
{
    if (button_out) *button_out = 0;
    if (!bar || !dt_guest_readable(bar, DT_CMD_ROW_ARRAY + 4u)) return 0;

    const uint32_t rows = wc3_rd32g(bar + DT_CMD_ROWS);
    const uint32_t cols = wc3_rd32g(bar + DT_CMD_COLS);
    if (!rows || !cols || rows > DT_CMD_MAX_ROWS || cols > DT_CMD_MAX_COLS) {
        return 0;
    }
    const uint32_t row_array = wc3_rd32g(bar + DT_CMD_ROW_ARRAY);
    if (!row_array ||
        !dt_guest_readable(row_array, rows * DT_CMD_ROW_STRIDE)) {
        return 0;
    }

    for (uint32_t row_index = 0; row_index < rows; row_index++) {
        const uint32_t row = row_array + row_index * DT_CMD_ROW_STRIDE;
        const uint32_t buttons = wc3_rd32g(row + DT_CMD_ROW_BUTTONS);
        if (!buttons || !dt_guest_readable(buttons, cols * 4u)) continue;

        for (uint32_t col = 0; col < cols; col++) {
            const uint32_t button = wc3_rd32g(buttons + col * 4u);
            if (!button ||
                !dt_guest_readable(button, DT_CMD_BUTTON_DATA + 4u)) {
                continue;
            }
            /* The frame visible bit is the real gate.  The +0x94 control
             * shown bit is not set on a visible command button in 1.27b
             * (T2 item 18), so requiring it made every hit-test miss and
             * the gesture fell through to the native path. */
            if (!wc3_rd32g(button + DT_CMD_BUTTON_VISIBLE)) continue;

            const uint32_t data = wc3_rd32g(button + DT_CMD_BUTTON_DATA);
            if (!data || !dt_guest_readable(data, DT_CMD_DATA_FLAGS + 8u)) {
                continue;
            }

            float left = 0.0f;
            float top = 0.0f;
            float right = 0.0f;
            float bottom = 0.0f;
            if (!dt_console_rect_read(button, width, height, &left, &top,
                                      &right, &bottom)) {
                continue;
            }
            float hit_right = right;
            if (col + 1u == cols) hit_right += DT_CMD_HIT_MARGIN_RIGHT;
            if (!(nx >= left && nx < hit_right &&
                  ny >= top && ny < bottom)) {
                continue;
            }
            if (button_out) *button_out = button;
            return data;
        }
    }
    return 0;
}

#if WC3_DEBUG && WC3_DEBUG_INPUT
/* Dump the live command-bar layout when a press misses every button.  The
 * dump is debug-only: it reads guest memory and writes the dt_trace file. */
static void dt_command_dump_bar(uint32_t bar, uint32_t width, uint32_t height)
{
    if (!bar || !dt_guest_readable(bar, DT_CMD_ROW_ARRAY + 4u)) {
        dt_log("cmd-bar bar=%08X unreadable", bar);
        return;
    }
    const uint32_t rows = wc3_rd32g(bar + DT_CMD_ROWS);
    const uint32_t cols = wc3_rd32g(bar + DT_CMD_COLS);
    const uint32_t row_array = wc3_rd32g(bar + DT_CMD_ROW_ARRAY);
    dt_log("cmd-bar bar=%08X rows=%u cols=%u row_array=%08X size=%ux%u", bar,
           rows, cols, row_array, width, height);
    float bar_l = 0.0f;
    float bar_t = 0.0f;
    float bar_r = 0.0f;
    float bar_b = 0.0f;
    if (dt_console_rect_read(bar, width, height, &bar_l, &bar_t, &bar_r,
                             &bar_b)) {
        dt_log("cmd-bar-rect %.4f,%.4f..%.4f,%.4f", bar_l, bar_t, bar_r,
               bar_b);
    }
    float local_l = 0.0f;
    float local_t = 0.0f;
    float local_r = 0.0f;
    float local_b = 0.0f;
    if (dt_frame_rect_read(bar, width, height, &local_l, &local_t, &local_r,
                           &local_b)) {
        dt_log("cmd-bar-local %.4f,%.4f..%.4f,%.4f", local_l, local_t,
               local_r, local_b);
    }
    if (dt_guest_readable(bar + DT_FRAME_RECT, 16u)) {
        dt_log("cmd-bar-raw t=%08X l=%08X b=%08X r=%08X",
               wc3_rd32g(bar + DT_FRAME_RECT + 0u),
               wc3_rd32g(bar + DT_FRAME_RECT + 4u),
               wc3_rd32g(bar + DT_FRAME_RECT + 8u),
               wc3_rd32g(bar + DT_FRAME_RECT + 12u));
    }
    if (!rows || !cols || rows > DT_CMD_MAX_ROWS || cols > DT_CMD_MAX_COLS) {
        return;
    }
    if (!row_array ||
        !dt_guest_readable(row_array, rows * DT_CMD_ROW_STRIDE)) {
        return;
    }
    for (uint32_t r = 0; r < rows; r++) {
        const uint32_t row = row_array + r * DT_CMD_ROW_STRIDE;
        const uint32_t buttons = wc3_rd32g(row + DT_CMD_ROW_BUTTONS);
        if (!buttons || !dt_guest_readable(buttons, cols * 4u)) continue;
        for (uint32_t c = 0; c < cols; c++) {
            const uint32_t button = wc3_rd32g(buttons + c * 4u);
            float l = 0.0f;
            float t = 0.0f;
            float rr = 0.0f;
            float b = 0.0f;
            if (!button ||
                !dt_console_rect_read(button, width, height, &l, &t, &rr,
                                      &b)) {
                dt_log("cmd-btn[%u,%u] btn=%08X rect=none", r, c, button);
                continue;
            }
            float local_l = 0.0f;
            float local_t = 0.0f;
            float local_r = 0.0f;
            float local_b = 0.0f;
            (void)dt_frame_rect_read(button, width, height, &local_l,
                                     &local_t, &local_r, &local_b);
            dt_log("cmd-btn[%u,%u] btn=%08X rect=%.4f,%.4f..%.4f,%.4f "
                   "local=%.4f,%.4f..%.4f,%.4f "
                   "raw=%08X,%08X,%08X,%08X vis=%u shown=%u data=%08X",
                   r, c, button, l, t, rr, b, local_l, local_t, local_r,
                   local_b,
                   wc3_rd32g(button + DT_FRAME_RECT + 0u),
                   wc3_rd32g(button + DT_FRAME_RECT + 4u),
                   wc3_rd32g(button + DT_FRAME_RECT + 8u),
                   wc3_rd32g(button + DT_FRAME_RECT + 12u),
                   wc3_rd32g(button + DT_CMD_BUTTON_VISIBLE),
                   wc3_rd32g(button + DT_CMD_BUTTON_SHOWN),
                   wc3_rd32g(button + DT_CMD_BUTTON_DATA));
        }
    }
}
#endif

/* Long press on a command button.  The right-click entry is the autocast
 * toggle; when the data has none the gesture is swallowed so the release
 * cannot turn into a second click. */
static void dt_command_long_press(uint32_t width, uint32_t height)
{
    uint32_t bar = 0;
    if (!dt_command_context(&bar, 0, 1) || bar != g_cmd_bar) {
        dt_trace("cmd-long-context", g_cmd_button, bar);
        g_cmd_long_pending = 0;
        g_cmd_long_fired = 1;
        return;
    }

    uint32_t button = 0;
    const uint32_t data = dt_command_button_at(bar, g_cmd_down_x, g_cmd_down_y,
                                               width, height, &button);
    if (!data || button != g_cmd_button ||
        dt_command_entry(data, DT_CMD_ENTRY_RIGHT) < 0) {
        dt_trace("cmd-long-none", g_cmd_button, data);
        g_cmd_long_pending = 0;
        g_cmd_long_fired = 1;
        return;
    }

    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    if (!dt_console_rect_read(button, width, height, &left, &top,
                              &right, &bottom)) {
        g_cmd_long_pending = 0;
        g_cmd_long_fired = 1;
        return;
    }

    dt_native_right_click((left + right) * 0.5f, (top + bottom) * 0.5f);
    g_cmd_long_pending = 0;
    g_cmd_long_fired = 1;
    dt_trace("cmd-long", button, data);
}

/* The recognizer emits only raw phases on the NATIVE route, so the long press
 * is timed here at the message-pump boundary.  Runs on the main guest thread
 * and only posts native input, never guest code. */
static void dt_command_tick(CPU *c, uint32_t width, uint32_t height)
{
    /* A field tap held during the map load is replayed here once the game's
     * input gate and select mode are live. */
    dt_field_click_tick(c, width, height);

    /* A drag release queued a confirming field click.  Wait until the game's
     * own update has left the select mode for target mode, then post it; an
     * instant command leaves select active and the queued click is dropped by
     * the timeout.  The card page can be rebuilt or the map can change while
     * the queue waits, so the same world/bar identity used at release is
     * re-checked here.  A new gesture in the event queue wins: let the service
     * process it first, then the event loop cancels this confirmation. */
    if (g_cmd_target.pending && wc3_direct_touch_pending() == 0) {
        const int targeted = dt_command_targeting(g_cmd_target.type);
        uint32_t live_bar = 0;
        const int context_ok = dt_command_context(&live_bar, 1, 0);
        const uint64_t now = dt_now_ms();
        if (!context_ok || live_bar != g_cmd_target.bar) {
            dt_trace("cmd-target-context", g_cmd_target.button, live_bar);
            dt_command_target_reset();
        } else if (wc3_dt_command_release(1, targeted, 1) ==
                   WC3_DT_COMMAND_TARGET) {
            dt_native_left_click(g_cmd_target.x, g_cmd_target.y);
            dt_log("cmd target button=%08X data=%08X x=%.4f y=%.4f",
                   g_cmd_target.button, g_cmd_target.data,
                   g_cmd_target.x, g_cmd_target.y);
            dt_trace("cmd-drag", g_cmd_target.button, g_cmd_target.data);
            dt_command_target_reset();
        } else if (g_cmd_target.ms && now && now >= g_cmd_target.ms &&
                   now - g_cmd_target.ms >= DT_CMD_TARGET_TIMEOUT_MS) {
            dt_trace("cmd-target-timeout", g_cmd_target.button,
                     g_cmd_target.data);
            dt_command_target_reset();
        }
    }

    if (!g_cmd_active || !g_cmd_long_pending || g_cmd_long_fired) return;
    if (g_cmd_moved) {
        g_cmd_long_pending = 0;
        return;
    }

    const uint64_t now = dt_now_ms();
    if (!now || !g_cmd_down_ms || now < g_cmd_down_ms) return;
    if (now - g_cmd_down_ms < DT_CMD_LONG_PRESS_MS) return;

    dt_command_long_press(width, height);
}

/* Resolve the release.  The DOWN already posted the native left click that
 * selects the command and, for a targeted/area command, enters target mode,
 * so a tap release is a no-op.  A drag into the field posts one native left
 * click at the release point to confirm the target.  Any other drag
 * cancels. */
static void dt_command_finish(CPU *c, const Wc3DirectTouchEvent *ev,
                              uint32_t width, uint32_t height)
{
#if !(WC3_DEBUG && WC3_DEBUG_INPUT)
    (void)width;
    (void)height;
#endif
    const uint32_t bar = g_cmd_bar;
    const uint32_t button = g_cmd_button;
    const uint32_t data = g_cmd_data;
    const uint32_t type = g_cmd_type;
    const float down_x = g_cmd_down_x;
    const float down_y = g_cmd_down_y;
    int moved = g_cmd_moved;
    const int long_fired = g_cmd_long_fired;

    if (long_fired) {
        /* The long press toggled autocast or was swallowed; it never enters a
         * target mode, so there is nothing to confirm. */
        dt_trace("cmd-long-release", button, 0);
        dt_command_reset();
        return;
    }

    /* The UI may have been replaced between DOWN and UP.  Never post a click
     * into a new command page or a menu.  The pressed button itself may be
     * rebuilt by the command the DOWN click ran, so it is not used as a
     * second hit-test gate; only the bar identity decides the context. */
    uint32_t live_bar = 0;
    /* The command's target mode closes the click gate, so the release only
     * requires the world/bar identity, not the gate. */
    if (!dt_command_context(&live_bar, 1, 0) || live_bar != bar) {
        dt_trace("cmd-context", button, live_bar);
        dt_command_reset();
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_INPUT
    {
        uint32_t live_button = 0;
        const uint32_t live_data =
            dt_command_button_at(live_bar, down_x, down_y, width, height,
                                 &live_button);
        if (!live_data || live_button != button) {
            dt_log("cmd-source-moved button=%08X live=%08X data=%08X "
                   "down=%.4f,%.4f", button, live_button, live_data,
                   down_x, down_y);
        }
    }
#endif

    /* The recognizer routes a MOVE that leaves the HUD over the field path,
     * so the command handler may never see it.  Recompute the movement from
     * the DOWN and UP points; otherwise a drag into the field is mistaken
     * for a tap and the target click is never posted. */
    if (!moved &&
        wc3_dt_command_moved(down_x, down_y, ev->x, ev->y,
                             DT_CMD_TAP_SLOP)) {
        moved = 1;
        g_cmd_moved = 1;
        g_cmd_long_pending = 0;
    }

    /* The DOWN click selected the command and, for a targeted/area command,
     * entered target mode.  A tap is complete: a targeted command stays in
     * target mode for the next field tap, and a non-targeted one already
     * ran.  Do not post a second card click; it would cancel target mode or
     * run the command twice. */
    if (!moved) {
        dt_trace("cmd-tap", button, data);
        dt_command_reset();
        return;
    }

    /* A targeted command dragged onto the portrait targets the unit shown
     * there (or the primary selection).  The portrait is below the field
     * rectangle, so the generic field branch would otherwise cancel it. */
    {
        float pl = 0.0f;
        float pt = 0.0f;
        float pr = 0.0f;
        float pb = 0.0f;
        DtContext portrait_ctx;
        memset(&portrait_ctx, 0, sizeof portrait_ctx);
        if (wc3_hle_direct_touch_portrait_rect(&pl, &pt, &pr, &pb) &&
            dt_context(&portrait_ctx)) {
            const int portrait_hit =
                ev->x >= pl && ev->x <= pr && ev->y >= pt && ev->y <= pb;
            const uint32_t target = dt_grid_target_mode(portrait_ctx.ui);
            if (wc3_dt_command_portrait_target(target != 0, portrait_hit,
                                               moved)) {
                const uint32_t unit = dt_portrait_unit(c, &portrait_ctx);
                if (unit) {
                    dt_grid_target(c, target, unit);
                    dt_command_target_reset();
                    dt_command_reset();
                    dt_trace("cmd-portrait-target", unit, target);
                    return;
                }
                dt_trace("cmd-portrait-no-unit", 0, 0);
            }
        }
    }

    /* A targeted command dragged onto a selected-unit grid button targets
     * that unit.  The grid is below the field rectangle, so the generic
     * field branch would otherwise cancel the command on release.  This uses
     * the same native target-mode event a grid press sends while targeting.
     * Only the UI half of the context is needed: resolving the selection here
     * would add a guest call to every command drag release. */
    {
        uint32_t grid_ui = 0;
        if (dt_grid_ui(&grid_ui)) {
            const uint32_t grid_unit = dt_grid_button_at(c, grid_ui, ev->x,
                                                          ev->y, width, height);
            /* The live target mode is authoritative: the command data word
             * cannot reliably identify a targeted button. */
            const uint32_t target = dt_grid_target_mode(grid_ui);
            if (wc3_dt_command_grid_target(target != 0, grid_unit != 0,
                                           moved)) {
                dt_grid_target(c, target, grid_unit);
                dt_command_target_reset();
                dt_command_reset();
                dt_trace("cmd-grid-target", grid_unit, target);
                return;
            }
        }
    }

    if (dt_field_contains(ev->x, ev->y)) {
        const int targeted = dt_command_targeting(type);
        if (wc3_dt_command_release(moved, targeted, 1) ==
            WC3_DT_COMMAND_TARGET) {
            /* Target mode is already live; confirm now. */
            dt_native_left_click(ev->x, ev->y);
            dt_log("cmd target button=%08X data=%08X type=%u x=%.4f y=%.4f",
                   button, data, type, ev->x, ev->y);
            dt_trace("cmd-drag", button, data);
        } else {
            /* The game updates the mode on its own tick; queue the click so
             * the pump posts it once target mode is live. */
            g_cmd_target.pending = 1;
            g_cmd_target.bar = bar;
            g_cmd_target.x = ev->x;
            g_cmd_target.y = ev->y;
            g_cmd_target.button = button;
            g_cmd_target.data = data;
            g_cmd_target.type = type;
            g_cmd_target.ms = dt_now_ms();
            dt_log("cmd target queued button=%08X data=%08X type=%u "
                   "x=%.4f y=%.4f", button, data, type, ev->x, ev->y);
            dt_trace("cmd-target-queued", button, data);
        }
        dt_command_reset();
        return;
    }

    /* Outside the field: keep the command selected and post no world
     * click.  A targeted command stays in target mode for the next field
     * tap. */
    dt_trace("cmd-cancel", button, (uint32_t)moved);
    dt_command_reset();
}

static int dt_command_pointer(CPU *c, const Wc3DirectTouchEvent *ev,
                              uint32_t width, uint32_t height)
{
    if (!ev || ev->route != WC3_DIRECT_TOUCH_ROUTE_NATIVE) return 0;

    switch (ev->kind) {
    case WC3_DIRECT_TOUCH_POINTER_DOWN: {
        if (g_cmd_active) return 1;
        uint32_t bar = 0;
        if (!dt_command_context(&bar, 0, 1)) return 0;
        uint32_t button = 0;
        const uint32_t data = dt_command_button_at(bar, ev->x, ev->y,
                                                   width, height, &button);
        if (!data) {
#if WC3_DEBUG && WC3_DEBUG_INPUT
            dt_log("cmd-hit-none bar=%08X n=%.4f,%.4f", bar, ev->x, ev->y);
            dt_command_dump_bar(bar, width, height);
            /* A background press on the command bar is still a panel
             * interaction.  Start the sequence here so the next world tap is
             * traced; the previous probe used to wait for a command button
             * and missed this repro. */
            wc3_hle_direct_touch_probe_begin("cmd-contact", bar, 0);
#endif
            return 0;
        }

        /* A new card gesture supersedes a still-waiting confirmation from an
         * earlier drag; the user is selecting a different command. */
        dt_command_target_reset();
        g_cmd_active = 1;
        g_cmd_pointer = ev->pointer_id;
        g_cmd_bar = bar;
        g_cmd_button = button;
        g_cmd_data = data;
        g_cmd_type = wc3_rd32g(data + DT_CMD_DATA_TYPE);
        g_cmd_down_x = ev->x;
        g_cmd_down_y = ev->y;
        g_cmd_down_ms = dt_now_ms();
        g_cmd_moved = 0;
        g_cmd_long_pending = 1;
        g_cmd_long_fired = 0;

        /* Select the command as soon as the finger lands: the left-click
         * equivalent is posted here, not on release.  A successful click on
         * the button is what enters the game's target mode for a targeted
         * command, so the drag only moves the cursor and the field release
         * confirms the target.  The old second "arm" click is not posted:
         * it would either cancel target mode or run the command twice. */
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        if (dt_console_rect_read(button, width, height, &left, &top,
                                 &right, &bottom)) {
            const float center_x = (left + right) * 0.5f;
            const float center_y = (top + bottom) * 0.5f;
#if WC3_DEBUG && WC3_DEBUG_INPUT
            wc3_hle_direct_touch_probe_begin("cmd-select", button, data);
            wc3_hle_direct_touch_probe_mark("cmd-click-pre",
                                            dt_bits(center_x),
                                            dt_bits(center_y));
#endif
            dt_native_left_click(center_x, center_y);
            dt_log("cmd select button=%08X data=%08X type=%u "
                   "center=%.4f,%.4f hle_tgt=%d",
                   button, data, g_cmd_type, center_x, center_y,
                   dt_command_targeted(data));
#if WC3_DEBUG && WC3_DEBUG_INPUT
            wc3_hle_direct_touch_probe_mark("cmd-click-post", button, data);
#endif
        }
        dt_trace("cmd-down", button, data);
        return 1;
    }
    case WC3_DIRECT_TOUCH_POINTER_MOVE:
        if (!g_cmd_active || ev->pointer_id != g_cmd_pointer) return 0;
        /* Follow the finger with the native cursor.  In target mode this is
         * the targeting cursor; outside it is a plain hover move.  The DOWN
         * click already selected/armed the command, so no extra click is
         * posted here. */
        hle_android_mouse_move(ev->x, ev->y);
        if (!g_cmd_moved) {
            const float dx = ev->x - g_cmd_down_x;
            const float dy = ev->y - g_cmd_down_y;
            if (dx * dx + dy * dy > DT_CMD_TAP_SLOP * DT_CMD_TAP_SLOP) {
                g_cmd_moved = 1;
                g_cmd_long_pending = 0;
            }
        }
        return 1;
    case WC3_DIRECT_TOUCH_POINTER_UP:
        if (!g_cmd_active || ev->pointer_id != g_cmd_pointer) return 0;
        dt_command_finish(c, ev, width, height);
        return 1;
    case WC3_DIRECT_TOUCH_POINTER_CANCEL:
        if (!g_cmd_active || ev->pointer_id != g_cmd_pointer) return 0;
        dt_trace("cmd-abort", g_cmd_button, 0);
        dt_command_reset();
        return 1;
    default:
        return 0;
    }
}

static void dt_dispatch(CPU *c, const DtContext *ctx,
                        const Wc3DirectTouchEvent *ev,
                        uint32_t width, uint32_t height)
{
#if WC3_DEBUG && WC3_DEBUG_INPUT
    dt_log("dispatch kind=%s(%u) ptr=%u n=%.4f,%.4f route=%s",
           dt_kind_name(ev->kind), ev->kind, ev->pointer_id,
           ev->x, ev->y, dt_route_name(ev->route));
    /* The probe dumps the guest's own ring for every discrete event.  Moves
     * only get a light line: a drag can produce dozens, and none of them
     * posts a native message by itself. */
    if (wc3_hle_direct_touch_probe_active()) {
        if (ev->kind == WC3_DIRECT_TOUCH_POINTER_MOVE ||
            ev->kind == WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE ||
            ev->kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE) {
            wc3_hle_direct_touch_tracef(
                "probe dispatch-move kind=%u ptr=%u n=%08X,%08X route=%u",
                ev->kind, ev->pointer_id, dt_bits(ev->x), dt_bits(ev->y),
                (uint32_t)ev->route);
        } else {
            wc3_hle_direct_touch_probe_mark("dispatch", ev->kind,
                                            ev->pointer_id);
        }
    }
#endif
    /* A camera gesture never owns the mouse and never enters the target-mode
     * gesture path: the action finger may be targeting at the same time. */
    if (ev->owner_role == WC3_DT_OWNER_CAMERA) {
        switch (ev->kind) {
        case WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN:
            g_beacon.armed = 0;
            (void)dt_camera_pan_begin(c, ctx, ev, width, height);
            break;
        case WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE:
            dt_camera_pan_move(ev);
            break;
        case WC3_DIRECT_TOUCH_FIELD_CAMERA_END:
            dt_camera_pan_end(c, ev);
            break;
        default:
            dt_trace("camera-role-unexpected", ev->kind, ev->pointer_id);
            break;
        }
        return;
    }
    switch (ev->kind) {
    case WC3_DIRECT_TOUCH_KEY_SIGNAL:
        /* A D-pad arrow is a direct binding signal, not a touch gesture and
         * not a keyboard message.  It works in every route, including menus. */
        dt_key_signal(c, ev->pointer_id, ev->route ? 1u : 0u);
        break;
    case WC3_DIRECT_TOUCH_GROUP_SIGNAL:
        dt_group_signal(c, ev->pointer_id, ev->route);
        break;
    case WC3_DIRECT_TOUCH_TAP: {
        /* The controls gate normally routes a press on the signal button to
         * NATIVE raw phases.  If the gate missed the frame (stale or empty
         * snapshot), the field recognizer can still deliver the tap here;
         * arm instead of turning a HUD press into a battlefield order. */
        float bl = 0.0f;
        float bt = 0.0f;
        float br = 0.0f;
        float bb = 0.0f;
        const int on_button =
            dt_beacon_signal_rect(width, height, &bl, &bt, &br, &bb) &&
            ev->x >= bl && ev->x <= br &&
            ev->y >= bt && ev->y <= bb;
        if (on_button) {
            g_beacon.armed = 1;
#if WC3_DEBUG && WC3_DEBUG_INPUT
            wc3_hle_direct_touch_probe_begin("beacon-tap-arm", dt_bits(ev->x),
                                             dt_bits(ev->y));
#endif
            dt_trace("beacon-tap-arm", dt_bits(ev->x), dt_bits(ev->y));
            break;
        }
        if (g_beacon.armed && ev->route != WC3_DIRECT_TOUCH_ROUTE_PORTRAIT &&
            ev->route != WC3_DIRECT_TOUCH_ROUTE_MINIMAP) {
            /* A signal-button tap armed the next battlefield tap.  This is
             * the fallback for a button that the controls layer classified as
             * FIELD; the raw gesture path above normally handles it.  A tap
             * that lands on the minimap still uses the minimap mapping even
             * if the controls gate missed the frame. */
            g_beacon.armed = 0;
            float ml = 0.0f;
            float mt = 0.0f;
            float mr = 0.0f;
            float mb = 0.0f;
            const int on_minimap =
                dt_minimap_native_rect(&ml, &mt, &mr, &mb) &&
                ev->x >= ml && ev->x <= mr &&
                ev->y >= mt && ev->y <= mb;
            const int on_field = dt_field_contains(ev->x, ev->y);
            int placed = 0;
            if (on_minimap) {
                placed = dt_beacon_place_minimap(c, ev->x, ev->y,
                                                 width, height);
            } else if (on_field) {
                placed = dt_beacon_place_field(c, ctx, ev->x, ev->y,
                                               width, height);
            }
            if (placed) {
                dt_trace(on_minimap ? "beacon-tap-minimap"
                                    : "beacon-tap-field",
                         dt_bits(ev->x), dt_bits(ev->y));
                break;
            }
        }
        if (ev->route == WC3_DIRECT_TOUCH_ROUTE_PORTRAIT) {
            dt_portrait_tap(c, ctx);
        } else {
            dt_tap(c, ctx, ev, width, height);
        }
        break;
    }
    case WC3_DIRECT_TOUCH_LONG_PRESS:
        g_beacon.armed = 0;
        if (ev->route == WC3_DIRECT_TOUCH_ROUTE_PORTRAIT) {
            /* Compatibility for an event queued by an older build.  The
             * current recognizer starts the portrait follow on DOWN. */
            dt_portrait_follow_start(c, ctx);
        } else {
            /* Reserved.  The current field long press is a native
             * left-button press expressed as FIELD_SELECT_BEGIN; do not run
             * the old semantic pick/select action. */
            dt_trace("long-press-ignored", dt_bits(ev->x), dt_bits(ev->y));
        }
        break;
    case WC3_DIRECT_TOUCH_FOLLOW_START:
        g_beacon.armed = 0;
        if (ev->route == WC3_DIRECT_TOUCH_ROUTE_PORTRAIT) {
            dt_portrait_follow_start(c, ctx);
        }
        break;
    case WC3_DIRECT_TOUCH_HOLD_RELEASE:
        dt_portrait_release(c);
        break;
    /* Legacy camera events (role 0) from an older producer: keep them as a
     * cursor-free pan rather than reviving the removed middle-button path. */
    case WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN:
        g_beacon.armed = 0;
        (void)dt_camera_pan_begin(c, ctx, ev, width, height);
        break;
    case WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE:
        dt_camera_pan_move(ev);
        break;
    case WC3_DIRECT_TOUCH_FIELD_CAMERA_END:
        dt_camera_pan_end(c, ev);
        break;
    case WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN:
        g_beacon.armed = 0;
        if (dt_field_target_active(c, ctx)) {
            dt_field_target_gesture_begin(ev);
        } else {
            dt_field_select_begin(c, ctx, ev);
        }
        break;
    case WC3_DIRECT_TOUCH_FIELD_SELECT_DRAG_BEGIN:
        g_beacon.armed = 0;
        if (!g_field_drag.target_active) {
            dt_field_select_drag_begin(c, ctx, ev);
        }
        break;
    case WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE:
        if (g_field_drag.target_active) {
            dt_field_target_gesture_move(ev);
        } else {
            dt_field_select_move(ev);
        }
        break;
    case WC3_DIRECT_TOUCH_FIELD_SELECT_END:
        if (g_field_drag.target_active) {
            dt_field_target_gesture_end(ev);
        } else {
            dt_field_select_end(c, ctx, ev, width, height);
        }
        break;
    case WC3_DIRECT_TOUCH_POINTER_DOWN:
    case WC3_DIRECT_TOUCH_POINTER_MOVE:
    case WC3_DIRECT_TOUCH_POINTER_UP:
    case WC3_DIRECT_TOUCH_POINTER_CANCEL:
        if (g_pause_menu_active) {
            /* Pause menu: the same raw native pointer path as the main menu.
             * No game subgesture (beacon, command card, inventory, grid,
             * minimap backstop or field tap resolver) may claim the contact.
             * dt_native_pointer() posts the DOWN/MOVE/UP phases through the
             * universal gateway exactly like a main-menu click. */
            dt_native_pointer(c, ev);
            dt_log("consumed=native-menu ptr=%u", ev->pointer_id);
            break;
        }
        if (dt_beacon_pointer(c, ctx, ev, width, height)) {
            /* A beacon gesture owns this world touch. */
            dt_log("consumed=beacon ptr=%u", ev->pointer_id);
            break;
        }
        if (ev->kind == WC3_DIRECT_TOUCH_POINTER_UP &&
            g_field_defer_active &&
            ev->pointer_id == g_field_defer_pointer &&
            (!g_field_defer_moved ||
             g_field_defer_kind == WC3_DT_FIELD_DEFER_TARGET)) {
            /* Every raw field press is held only until this release so it
             * can be classified once, by the live route.  A target gesture
             * follows the cursor and is resolved on release even when the
             * finger moved.  The total resolver decides between semantic
             * tap, map-load replay, native click and explicit drop; this
             * path never queues a live select-mode tap. */
            dt_trace(g_field_defer_release_trace[g_field_defer_kind],
                     dt_bits(ev->x), dt_bits(ev->y));
            dt_field_release_tap(c, ev, width, height);
            dt_log("consumed=field-tap ptr=%u", ev->pointer_id);
            dt_field_defer_reset();
            break;
        }
        if (ev->route == WC3_DIRECT_TOUCH_ROUTE_NATIVE &&
            dt_command_pointer(c, ev, width, height)) {
            dt_log("consumed=command ptr=%u", ev->pointer_id);
            break;
        }
        if (ev->route == WC3_DIRECT_TOUCH_ROUTE_NATIVE &&
            dt_inventory_pointer(ev, width, height)) {
            dt_log("consumed=inventory ptr=%u", ev->pointer_id);
            break;
        }
        if (ev->route == WC3_DIRECT_TOUCH_ROUTE_NATIVE &&
            dt_grid_pointer(c, ev, width, height)) {
            dt_log("consumed=grid ptr=%u", ev->pointer_id);
            break;
        }
        /* A native press outside the battlefield is HUD/menu input; the
         * minimap backstop handles its own rectangle and everything else is
         * posted as a normal native click. */
        dt_native_minimap_pointer(c, ev, width, height);
        dt_log("consumed=native ptr=%u", ev->pointer_id);
        break;
    default:
        dt_trace("unknown-kind", ev->kind, ev->pointer_id);
        break;
    }
}

static void dt_request_main_thread_wake(void *user)
{
    (void)user;
    wc3_android_request_direct_touch();
}

void wc3_hle_direct_touch_start(void)
{
    g_main_thread = pthread_self();
    g_main_thread_valid = 1;
    dt_scratch_reset();
    __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_portrait_release_pending, 0, __ATOMIC_RELEASE);
    dt_grid_reset();
    dt_inventory_reset();
    dt_command_reset();
    dt_command_target_reset();
    dt_field_reset();
    dt_camera_pan_reset();
    dt_field_click_reset();
    dt_native_release();
    dt_portrait_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_minimap_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_minimap_panel_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_beacon_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_command_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_grid_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_inventory_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_publish_route(WC3_DIRECT_TOUCH_ROUTE_LEGACY);
    g_portrait_follow_active = 0;
    g_last_ui = 0;
    g_last_world = 0;
    wc3_direct_touch_clear();
    wc3_direct_touch_set_wake(dt_request_main_thread_wake, NULL);
}

void wc3_hle_direct_touch_enqueue(const Wc3DirectTouchEvent *event, void *user)
{
    (void)user;
    if (!event) return;
    /* Advance the input epoch before publishing the event.  The bump takes
     * g_uq_lock and releases it before the direct-touch push, so a deferred
     * click either loses to this input or is already queued ahead of it; no
     * lock is nested across the two queues. */
    if (wc3_dt_event_bumps_input_epoch(event->kind))
        hle_android_input_epoch_bump();
    const int pushed = wc3_direct_touch_push(event);
#if WC3_DEBUG && WC3_DEBUG_INPUT
    if (!pushed) {
        dt_log("enqueue DROP kind=%s(%u) ptr=%u n=%.4f,%.4f route=%s",
               dt_kind_name(event->kind), event->kind, event->pointer_id,
               event->x, event->y, dt_route_name(event->route));
    }
#else
    (void)pushed;
#endif
}

void wc3_hle_direct_touch_reset(void)
{
    dt_scratch_invalidate();
    __atomic_store_n(&g_portrait_release_pending, 1, __ATOMIC_RELEASE);
    dt_grid_reset();
    dt_inventory_reset();
    dt_command_reset();
    dt_command_target_reset();
    dt_field_reset();
    dt_camera_pan_reset();
    dt_field_click_reset();
    dt_native_release();
    dt_portrait_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_minimap_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_minimap_panel_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_beacon_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_command_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_grid_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_inventory_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_publish_route(WC3_DIRECT_TOUCH_ROUTE_LEGACY);
    g_portrait_follow_active = 0;
    g_pause_menu_active = 0;
    wc3_direct_touch_clear();
}

int wc3_hle_direct_touch_route(void)
{
    return __atomic_load_n(&g_route, __ATOMIC_ACQUIRE);
}

int wc3_hle_direct_touch_can_service(void)
{
    if (!g_main_thread_valid) return 0;
    if (!pthread_equal(pthread_self(), g_main_thread)) return 0;
    return !__atomic_load_n(&g_busy, __ATOMIC_ACQUIRE);
}

/* A new touch supersedes a still-waiting command confirmation from the drag
 * release: the user has moved on to another explicit action.  A pending
 * confirmation is only valid while the game arms its target mode on the next
 * pump; any new gesture must win.  Moves and releases belong to the gesture
 * that is already running and do not cancel it. */
static int dt_event_starts_new_gesture(uint32_t kind)
{
    switch (kind) {
    case WC3_DIRECT_TOUCH_TAP:
    case WC3_DIRECT_TOUCH_LONG_PRESS:
    case WC3_DIRECT_TOUCH_POINTER_DOWN:
    case WC3_DIRECT_TOUCH_FOLLOW_START:
    case WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN:
    case WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN:
    case WC3_DIRECT_TOUCH_FIELD_SELECT_DRAG_BEGIN:
    case WC3_DIRECT_TOUCH_FIELD_HOVER:
        return 1;
    default:
        return 0;
    }
}

void wc3_hle_direct_touch_service(CPU *c, uint32_t width, uint32_t height)
{
    if (!c || !wc3_hle_direct_touch_can_service()) return;
    dt_scratch_reset();
    /* Detect the pause entry edge before the deferred-press branch below.
     * That branch returns while a field DOWN waits for its idle hover pass,
     * so a pause sync after it would never see the menu open and could leave
     * the armed press stuck or force it into the first menu tap. */
    int pause_menu = dt_pause_menu_sync();
    /* A modal guest loop (building placement) may never poll GetCursorPos, so
     * the gateway's normal flush point does not run.  Flush any armed+ready
     * press here first: otherwise the pending button would keep the input
     * barrier closed and the whole direct-touch service would stall.  Return
     * after a fallback flush so this pump does not also dispatch the matching
     * UP — the guest needs one pump between DOWN and UP to activate a button. */
    if (hle_android_mouse_flush_pending_ready()) return;
    /* A late-resolved native UI click armed its DOWN at release time.  It
     * follows the same idle hover pass as a normal press; this state only
     * supplies the matching UP after the DOWN has been published.  The first
     * service call after arming gives the guest the idle pass.  Only when
     * g_native_up_modal is set (pause menu) may a modal loop with no
     * GetCursorPos pass force the DOWN here.  A field selection never takes
     * this fallback: the idle pass is the whole point of the gateway, and
     * forcing the first field press hit-tests a stale hot control.  Either
     * way the guest sees DOWN and UP in separate pumps. */
    if (g_native_up_pending) {
        if (hle_android_mouse_press_pending()) {
            if (g_native_up_modal && g_native_up_wait++ > 0) {
                hle_android_mouse_flush_pending_down();
                dt_trace("native-up-fallback",
                         (uint32_t)g_native_up_wait, 0);
            }
            return;
        }
        /* The armed press is gone: the DOWN was published or cancelled.
         * Post the UP only while the logical button is still held; a cancel
         * path clears g_native_down and this state together. */
        if (g_native_down && hle_android_mouse_is_down(0)) {
            dt_native_end();
        } else {
            g_native_down = 0;
        }
        g_native_up_pending = 0;
        g_native_up_wait = 0;
        g_native_up_modal = 0;
        dt_trace("native-up-deferred", 0, 0);
        return;
    }
    /* The command-card long press has no semantic event: the recognizer emits
     * only raw phases on the NATIVE route.  Time it at the pump boundary so
     * the right-click autocast toggle fires while the finger is still down. */
    dt_command_tick(c, width, height);
    if (__atomic_exchange_n(&g_portrait_release_pending, 0,
                            __ATOMIC_ACQ_REL)) {
        dt_portrait_release(c);
    }
    /* Applied-input barrier.  A command-card click selects the command and
     * may change CGameUI's input mode.  The change exists only after the
     * guest wndproc has dispatched that click, so classifying a world release
     * before then reads the previous mode.  Leave the direct events in the
     * SPSC queue and let the Win32 pump apply the queued button messages
     * first; the next service call sees the new mode and the same contact is
     * resolved once, on its own release. */
    if (wc3_direct_touch_pending() != 0 &&
        hle_android_native_button_pending()) {
        dt_trace("input-barrier", wc3_direct_touch_pending(), 0);
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_INPUT
    /* Sample the post-tap selection even when the direct-event queue is
     * empty: the guest applies a native click on its own input pump.  Do not
     * call guest code while an earlier native button is still queued. */
    if (!hle_android_native_button_pending()) {
        dt_map_audit_tick(NULL, 0);
    }
#endif
    if (wc3_direct_touch_pending() == 0) {
        dt_sync_idle_context(width, height);
        return;
    }

    __atomic_store_n(&g_busy, 1, __ATOMIC_RELEASE);
#if WC3_DEBUG && WC3_DEBUG_INPUT
    /* An active pump already dispatches guest messages; sample the audit
     * here instead of from the idle pass, where guest calls were an isolated
     * cost on every empty queue. */
    if (!hle_android_native_button_pending())
        dt_map_audit_tick(c, 1);
#endif

    /* Pointer-only markers: dt_context() fails in menus and loading screens,
     * but a native menu click must still be serviced.  Keep the last non-zero
     * UI/world pointers; only a real replacement is a map transition. */
    uint32_t cur_ui = 0;
    uint32_t cur_world = 0;
    if (dt_guest_readable(DT_UI_GLOBAL, 4)) {
        cur_ui = wc3_rd32g(DT_UI_GLOBAL);
        cur_world = dt_world_from_ui(cur_ui);
    }
#if WC3_DEBUG && WC3_DEBUG_INPUT
    const int new_world = cur_world && cur_world != g_last_world;
#endif
    const int context_changed =
        (g_last_ui && cur_ui && g_last_ui != cur_ui) ||
        (g_last_world && cur_world && g_last_world != cur_world);
#if WC3_DEBUG && WC3_DEBUG_INPUT
    /* Arm the map-start trace as soon as a live world exists, before the
     * first touch.  A missing UI/world is a menu or loading transition; the
     * next world is a new map even when the allocator reuses its address. */
    if (!cur_ui || !cur_world) {
        dt_map_probe_clear();
    } else if (new_world || context_changed) {
        dt_map_probe_arm(cur_ui, cur_world,
                         context_changed ? "context" : "world", 1);
    } else {
        dt_map_probe_arm(cur_ui, cur_world, "service", 0);
    }
#endif
    if (cur_ui) g_last_ui = cur_ui;
    if (cur_world) g_last_world = cur_world;

    /* The portrait, minimap, beacon and command-card frames can change with
     * a selection or a UI transition; publish them before resolving the
     * batch so the next touch sees the new UI. */
    dt_portrait_snapshot(cur_ui, width, height);
    dt_minimap_snapshot(cur_ui, width, height);
    dt_minimap_panel_snapshot(cur_ui, width, height);
    dt_beacon_snapshot(width, height);
    dt_command_snapshot(cur_ui, width, height);
    dt_grid_snapshot(cur_ui, width, height);
    dt_inventory_snapshot(cur_ui, width, height);
#if WC3_DEBUG && WC3_DEBUG_INPUT
    dt_trace_snapshots("service");
#endif

    if (context_changed) {
        /* Queued semantic events belong to the previous map.  Native events
         * are cleared too so a stale DOWN cannot leak a held button into the
         * new UI; the age bound below is the backstop when the pointers were
         * reused. */
        dt_trace("context-change", g_last_ui, g_last_world);
        dt_scratch_invalidate();
        dt_grid_reset();
        dt_inventory_reset();
        dt_command_reset();
        dt_command_target_reset();
        dt_field_reset();
        dt_camera_pan_reset();
        dt_field_click_reset();
        dt_native_release();
        /* A queued HOLD_RELEASE may be dropped with the batch; clear the
         * camera follow before the old context goes away. */
        dt_portrait_release(c);
        wc3_direct_touch_clear();
    }

    DtContext ctx;
    memset(&ctx, 0, sizeof ctx);
    /* One exact readiness check and one physical snapshot per pump boundary.
     * Both are cached below and refreshed only when an event can change the
     * mode or the context; pure motion then costs neither guest dispatches
     * nor an atomic route store. */
    Wc3DirectTouchRoute route = dt_ready_route(c, &ctx);
    Wc3DirectTouchRoute physical = dt_route_snapshot();
    g_service_field_ready = route == WC3_DIRECT_TOUCH_ROUTE_FIELD;
    dt_publish_service_route(route, physical);
    /* The native press of an owned field drag can make the route read NATIVE
     * on the next pump.  Keep the drag until its END; only an idle field is
     * cancelled by a route change. */
    if (!wc3_dt_field_drag_keep(route, dt_field_drag_active())) {
        dt_field_reset();
    }

    for (uint32_t i = 0; i < DT_MAX_EVENTS_PER_PUMP; i++) {
        /* An event handled earlier in this batch may have posted a native
         * button (command select, target confirmation, field drag).  Stop
         * before the next direct contact so the mode read for that contact
         * includes the click.  Remaining events stay queued for the next
         * pump. */
        if (hle_android_native_button_pending()) {
            dt_trace("input-barrier-mid", wc3_direct_touch_pending(), 0);
            break;
        }
        Wc3DirectTouchEvent event;
        if (!wc3_direct_touch_pop(&event)) break;

#if WC3_DEBUG && WC3_DEBUG_INPUT
        /* Count contact starts, not the individual hover/tap events of one
         * gesture.  A TAP or LONG_PRESS without a preceding hover still
         * starts a contact, so a fast flick cannot escape the trace. */
        const int contact_start =
            event.kind == WC3_DIRECT_TOUCH_FIELD_HOVER ||
            event.kind == WC3_DIRECT_TOUCH_POINTER_DOWN ||
            event.kind == WC3_DIRECT_TOUCH_FOLLOW_START ||
            event.kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN ||
            event.kind == WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN ||
            event.kind == WC3_DIRECT_TOUCH_TAP ||
            event.kind == WC3_DIRECT_TOUCH_LONG_PRESS;
        const int contact_end =
            event.kind == WC3_DIRECT_TOUCH_TAP ||
            event.kind == WC3_DIRECT_TOUCH_LONG_PRESS ||
            event.kind == WC3_DIRECT_TOUCH_POINTER_UP ||
            event.kind == WC3_DIRECT_TOUCH_POINTER_CANCEL ||
            event.kind == WC3_DIRECT_TOUCH_HOLD_RELEASE ||
            event.kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END ||
            event.kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END;
        if (contact_start && !g_map_contact_open) {
            dt_map_note_contact("event", event.kind, event.pointer_id,
                                event.x, event.y, event.route, route);
        }
        if (contact_end) g_map_contact_open = 0;
#endif

        /* The input thread may have queued this event just before the game
         * left the map.  The context checks catch most transitions; the age
         * bound is the backstop when a map reload reuses the same UI/world
         * objects and the guest thread was blocked through the transition. */
        const uint64_t now_ms = dt_now_ms();
        if (now_ms && event.time_ms && now_ms > event.time_ms &&
            now_ms - event.time_ms > DT_EVENT_MAX_AGE_MS) {
            dt_trace("stale-event", event.kind, event.pointer_id);
            if (event.owner_role == WC3_DT_OWNER_CAMERA) {
                dt_camera_pan_reset();
            } else if (dt_is_field_drag_kind(event.kind)) {
                dt_field_reset();
            } else if (dt_is_native_pointer(event.kind)) {
                dt_grid_reset();
                dt_inventory_reset();
                dt_command_reset();
                dt_command_target_reset();
                dt_field_click_reset();
                dt_native_release();
            }
            continue;
        }

        /* While CEscMenu is open the gesture set is exactly the main menu's:
         * key signals and raw native pointer phases.  Drop group chords,
         * semantic field/portrait events and stale minimap drag phases.  A
         * POINTER_CANCEL is still accepted so a gesture that was active when
         * the menu opened can release its native button. */
        if (pause_menu &&
            !wc3_dt_menu_event_allowed(event.kind, event.route)) {
            dt_trace("pause-menu-drop", event.kind, event.pointer_id);
            continue;
        }

        /* A key signal belongs to the binding layer, not to a touch route.
         * Run it before every semantic route gate so the D-pad keeps working
         * in menus and while another finger owns a gesture. */
        if (event.kind == WC3_DIRECT_TOUCH_KEY_SIGNAL) {
            dt_key_signal(c, event.pointer_id, event.route ? 1u : 0u);
            /* Escape or a menu key can open/close CEscMenu.  Refresh before
             * the next queued event so the new gesture set is in effect. */
            pause_menu = dt_pause_menu_sync();
            continue;
        }

        /* A chord is already resolved into one game action by the controls
         * layer.  Run it with the same route-independent priority as a key
         * signal; no modifier state is published for it. */
        if (event.kind == WC3_DIRECT_TOUCH_GROUP_SIGNAL) {
            dt_group_signal(c, event.pointer_id, event.route);
            continue;
        }

        /* A held first tap is only valid until the next gesture starts.
         * The tick above already yields to a non-empty queue, so a new
         * DOWN/TAP/HOVER that arrived while the replay was waiting must
         * cancel the stale tap instead of letting it run after the new
         * gesture.  A camera begin cancels it later, only after the camera
         * accepts the gesture. */
        if (g_field_click_pending &&
            wc3_dt_event_cancels_field_click(event.kind, 0)) {
            dt_trace("field-click-cancel", dt_bits(event.x),
                     dt_bits(event.y));
            g_field_click_pending = 0;
            g_field_click_attack_move = 0;
        }

        /* A queued command confirmation is only valid until the next touch
         * starts.  The command-card path resets it in its own DOWN handler;
         * field, minimap, portrait and menu gestures must do the same so a
         * new action cannot be followed by a stale confirmation click. */
        if (g_cmd_target.pending && dt_event_starts_new_gesture(event.kind)) {
            dt_trace("cmd-target-cancel", event.kind, event.pointer_id);
            dt_command_target_reset();
        }

#if WC3_DEBUG && WC3_DEBUG_INPUT
        dt_log("event kind=%s(%u) ptr=%u n=%.4f,%.4f event_route=%s "
               "service_route=%s field=%d",
               dt_kind_name(event.kind), event.kind, event.pointer_id,
               event.x, event.y, dt_route_name(event.route),
               dt_route_name(route), dt_field_contains(event.x, event.y));
#endif

        if (event.kind == WC3_DIRECT_TOUCH_HOLD_RELEASE) {
            /* A portrait hold ends even if the UI moved to a menu: the
             * release only needs the current camera, not the semantic
             * context. */
            dt_dispatch(c, &ctx, &event, width, height);
        } else if (event.kind == WC3_DIRECT_TOUCH_FIELD_HOVER) {
            /* Give the game's targeting cursor the tap point while the
             * finger is still down.  Earlier traces show the first release
             * after a command card resolving against the card's cursor and
             * only the next contact confirming, so the mode has to see this
             * contact as a hover before its click.  Select mode ignores the
             * event; its semantic path has no native cursor.  When the tap
             * already left the queue, stop here: the click must be a later
             * message than the hover. */
            if (ctx.ui && ctx.world && dt_field_target_active(c, &ctx)) {
#if WC3_DEBUG && WC3_DEBUG_INPUT
                wc3_hle_direct_touch_probe_mark("field-hover-pre",
                                                dt_bits(event.x),
                                                dt_bits(event.y));
#endif
                hle_android_mouse_move(event.x, event.y);
                dt_trace("field-target-hover", dt_bits(event.x),
                         dt_bits(event.y));
#if WC3_DEBUG && WC3_DEBUG_INPUT
                wc3_hle_direct_touch_probe_mark("field-hover-post",
                                                dt_bits(event.x),
                                                dt_bits(event.y));
#endif
                if (wc3_direct_touch_pending() != 0) {
                    dt_trace("field-target-hover-barrier",
                             wc3_direct_touch_pending(), 0);
                    break;
                }
            }
        } else if (event.owner_role == WC3_DT_OWNER_CAMERA) {
            /* Camera events are independent of the semantic route and of the
             * target-mode gesture owned by another finger. */
            dt_dispatch(c, &ctx, &event, width, height);
        } else if (dt_is_field_drag_end(event.kind)) {
            /* The END only releases a native mouse button.  It must run even
             * if the game changed context between BEGIN and END; otherwise
             * the button would stay latched. */
            dt_dispatch(c, &ctx, &event, width, height);
        } else if (dt_is_field_drag_kind(event.kind)) {
            /* Once BEGIN has pressed the native button, keep feeding MOVE to
             * that drag even if the button changed the active mode and the
             * route now reads NATIVE.  A new field gesture during target or
             * a transient mode is a target gesture, not a camera/selection
             * drag; its END posts the native click pair. */
            const int target_begin =
                !dt_field_drag_active() &&
                event.route == WC3_DIRECT_TOUCH_ROUTE_FIELD &&
                (event.kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN ||
                 event.kind == WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN ||
                 event.kind == WC3_DIRECT_TOUCH_FIELD_SELECT_DRAG_BEGIN) &&
                dt_field_target_active(c, &ctx);
            if (target_begin ||
                wc3_dt_field_drag_keep(route, dt_field_drag_active())) {
                dt_dispatch(c, &ctx, &event, width, height);
            } else {
                dt_trace("field-drag-drop", event.kind, event.pointer_id);
                dt_field_reset();
            }
        } else if (dt_is_native_pointer(event.kind)) {
            /* Native UI input is independent of the semantic context: menus,
             * loading screens, HUD and targeting all use it. */
            dt_dispatch(c, &ctx, &event, width, height);
        } else if (event.route == WC3_DIRECT_TOUCH_ROUTE_PORTRAIT) {
            /* Portrait follow/release resolves against the portrait's
             * displayed unit, not the field pick.  It still needs the
             * in-game context; in a menu the event belongs to the previous
             * map. */
            if (ctx.ui && ctx.world) {
                dt_dispatch(c, &ctx, &event, width, height);
            } else {
                dt_trace("portrait-drop", event.kind, event.pointer_id);
            }
        } else if (route == WC3_DIRECT_TOUCH_ROUTE_FIELD) {
            dt_dispatch(c, &ctx, &event, width, height);
        } else if (event.kind == WC3_DIRECT_TOUCH_TAP &&
                   event.route == WC3_DIRECT_TOUCH_ROUTE_FIELD) {
            /* The recognizer classified the contact on the physical FIELD
             * route, but the live semantic route is not select.  Resolve it
             * once, on this release: map-load -> separate replay;
             * target/transient/menu -> native click; no UI -> drop. */
            dt_field_release_tap(c, &event, width, height);
        } else if (route == WC3_DIRECT_TOUCH_ROUTE_NATIVE) {
            /* A semantic event that was not a field TAP still has a native
             * recipient (menu/HUD/targeting).  Let the native click handler
             * consume it instead of losing the contact. */
            dt_trace("semantic-native", event.kind, event.pointer_id);
            dt_native_click(&event);
        } else {
            /* Not in game: the controls layer already routes menus as native
             * pointer phases, so a semantic event here belongs to a stale
             * map/menu transition. */
            dt_trace("semantic-drop", event.kind, event.pointer_id);
        }

        /* A context probe is pure guest-memory reads: no dispatch, no
         * readiness guest calls.  Run it for every event so a transition
         * during a long drag still aborts this batch exactly as before. */
        DtContext probe;
        memset(&probe, 0, sizeof probe);
        (void)dt_context(&probe);
        /* In pause dt_ready_route() intentionally returns a zero semantic
         * context, while the live UI/world remains readable under the panel.
         * Comparing the two would report a context switch for every pause
         * event, reset the gesture state and cancel the native DOWN that was
         * just armed: the guest would see the hover move and no click.  The
         * menu is a native overlay, so the live world is not a new context
         * until the menu closes and the normal route returns. */
        const int context_switch =
            wc3_dt_context_switch_relevant(pause_menu) &&
            (probe.ui != ctx.ui || probe.world != ctx.world);

        /* Re-evaluate readiness after events that can post a click or change
         * the input mode.  A semantic action can enter targeting or load a
         * map; a native click can dismiss a modal or start a cinematic.
         * MOVE/HOVER/FOLLOW_START never do, so they keep the cached route. */
        if (context_switch || probe.mode != ctx.mode ||
            dt_event_can_change_mode(event.kind)) {
            /* A native click can open or dismiss the pause menu.  Refresh
             * the gesture set before the next queued contact. */
            pause_menu = dt_pause_menu_sync();
            DtContext next_ctx;
            memset(&next_ctx, 0, sizeof next_ctx);
            const Wc3DirectTouchRoute next_route =
                dt_ready_route(c, &next_ctx);
            const Wc3DirectTouchRoute next_physical = dt_route_snapshot();
            route = next_route;
            physical = next_physical;
            g_service_field_ready = route == WC3_DIRECT_TOUCH_ROUTE_FIELD;
            dt_publish_service_route(route, physical);
            /* Do not cancel the drag that the event just started/continued. */
            if (!wc3_dt_field_drag_keep(route, dt_field_drag_active())) {
                dt_field_reset();
            }

            if (context_switch) {
                dt_trace("batch-context-change", ctx.ui ^ next_ctx.ui,
                         ctx.world ^ next_ctx.world);
                dt_scratch_invalidate();
                if (next_ctx.ui) g_last_ui = next_ctx.ui;
                if (next_ctx.world) g_last_world = next_ctx.world;
                dt_grid_reset();
                dt_inventory_reset();
                dt_command_reset();
                dt_command_target_reset();
                dt_field_reset();
                dt_camera_pan_reset();
                dt_field_click_reset();
                dt_native_release();
                /* The pending HOLD_RELEASE belongs to the old context;
                 * release the follow now instead of leaving it latched. */
                dt_portrait_release(c);
                wc3_direct_touch_clear();
                break;
            }
            ctx = next_ctx;
        } else {
            /* Same UI/world/mode; adopt the cheaply refreshed pointers. */
            ctx = probe;
        }
    }

    /* One native camera update per camera finger with a pending point.  The
     * updates are coalesced per batch, so a 120 Hz touch stream does not
     * become 120 guest dispatches per pump.  No host lock is held and no
     * mouse message is posted. */
    dt_camera_pan_apply(c, &ctx, width, height);

    /* A native click can dismiss a modal or open a menu; refresh the frames
     * once at the end of the batch so the input thread sees the new UI. */
    uint32_t final_ui = 0;
    if (dt_guest_readable(DT_UI_GLOBAL, 4u)) {
        final_ui = wc3_rd32g(DT_UI_GLOBAL);
    }
    dt_portrait_snapshot(final_ui, width, height);
    dt_minimap_snapshot(final_ui, width, height);
    dt_minimap_panel_snapshot(final_ui, width, height);
    dt_beacon_snapshot(width, height);
    dt_command_snapshot(final_ui, width, height);
    dt_grid_snapshot(final_ui, width, height);
    dt_inventory_snapshot(final_ui, width, height);
#if WC3_DEBUG && WC3_DEBUG_INPUT
    dt_trace_snapshots("final");
#endif

    __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
}
