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
 *     finger release; a stationary release clicks one unit, a drag selects a
 *     box;
 *   - portrait: sticky-finger camera follow;
 *   - minimap: native pointer phases clamped to its frame;
 *   - selected-unit grid: activate the subgroup under the finger;
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
#define DT_UI_CAMERA       0x00000254u
#define DT_UI_MANAGER      0x0000024Cu
#define DT_UI_INFO_BAR     0x000003C4u
#define DT_UI_PORTRAIT     0x000003F4u
#define DT_UI_MINIMAP      0x000003C0u
#define DT_UI_COMMAND_BAR  0x000003C8u

#define DT_PORTRAIT_UNIT   0x00000238u
#define DT_PORTRAIT_FRAME  0x000000B4u
#define DT_PORTRAIT_OFF    0x00000260u
#define DT_MINIMAP_FRAME   0x000000B4u
/* CGameUI ctor resolves the MinimapSignal child frame into +0x440. */
#define DT_UI_MINIMAP_SIGNAL 0x00000440u
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
#define DT_FN_SELECT_EVENT     0x6F395410u
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
/* Set only while the service has posted a native WM_LBUTTONDOWN and has not
 * yet posted the matching release.  Main guest thread only. */
static int g_native_down;
/* Set after a direct-owned release was posted but before the guest dispatched
 * it.  A second tap can arrive in the same service batch; the HLE still
 * reports the button down, so the next DOWN must bypass that guard while the
 * queued UP is ahead of it in the message queue.  Main guest thread only. */
static int g_native_up_pending;
/* Native minimap backstop.  Raw pointer phases arrive as NATIVE, so the
 * service cannot see the controls layer's MINIMAP owner.  Track a DOWN that
 * lands in the published minimap rectangle, keep every later phase inside
 * it, and force the release: the native frame manager only delivers the
 * button-up to the minimap while the cursor is over the frame, and
 * SetCapture is a no-op in this HLE.  Main guest thread only. */
static int g_minimap_native_active;
static uint32_t g_minimap_native_pointer;
static int g_minimap_native_pressed;
static float g_minimap_native_x;
static float g_minimap_native_y;
/* Beacon button (MinimapSignal) gesture and armed minimap placement.  A tap
 * on the signal button arms the next minimap tap; a drag from the button
 * places the beacon at the release point.  The native signal mode is bypassed
 * because its two-step click path is not reliable through the HLE message
 * pump.  No native button is posted for this gesture, so there is no latched
 * button to release.  Main guest thread only. */
static int g_beacon_armed;
static int g_beacon_active;
static uint32_t g_beacon_pointer;
static float g_beacon_down_x;
static float g_beacon_down_y;
static int g_beacon_moved;
/* Field camera and selection drags own a native mouse button for the whole
 * gesture.  The recognizer starts them only after the finger crosses the
 * legacy drag threshold, so the button is never pressed for a tap.  Main
 * guest thread only. */
static int g_field_camera_active;
static int g_field_camera_pointer;
static int g_field_select_active;
static int g_field_select_pointer;
/* The middle button has the same queued-release hazard as the right button:
 * the next camera drag can begin before the guest has dispatched the
 * previous WM_MBUTTONUP.  Main guest thread only. */
static int g_native_middle_pending;
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
/* Set by wc3_hle_direct_touch_reset() (any thread) when the game leaves the
 * in-game UI.  The service, on the main guest thread, clears the camera
 * follow before it drops the queue. */
static int g_portrait_release_pending;
/* Selected-unit grid drag.  The native grid only reacts on a completed click,
 * so a touch that starts on a button is consumed here and the subgroup under
 * the finger is activated on DOWN and on every MOVE to a different button.
 * Main guest thread only. */
static int g_grid_active;
static uint32_t g_grid_pointer;
static uint32_t g_grid_ui;
static uint32_t g_grid_selection;
static uint32_t g_grid_unit;
/* Set when the grid touch selected a target instead of activating a group.
 * The rest of the gesture is consumed so a drag cannot retarget or reach the
 * native world handler. */
static int g_grid_target;

/* Inventory item drag.  A touch that starts on an occupied slot is consumed
 * so the release can be replayed as the native click pair: left click on the
 * source (use), or right click on the source followed by a left click on the
 * destination slot or world point (swap/move/drop).  Main guest thread only. */
static int g_inv_active;
static uint32_t g_inv_pointer;
static uint32_t g_inv_bar;
static uint32_t g_inv_slot;
static float g_inv_down_x;
static float g_inv_down_y;
/* Our right-button UP may still be queued when the next drag starts.  Force
 * the next DOWN past the logical button bit so the guest still sees a clean
 * DOWN/UP pair, the same way g_native_up_pending handles the left button.
 * Shared by the inventory move and the command-card autocast toggle. */
static int g_native_right_pending;

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
static float g_cmd_down_x;
static float g_cmd_down_y;
static uint64_t g_cmd_down_ms;
static int g_cmd_moved;
static int g_cmd_long_pending;
static int g_cmd_long_fired;
/* Confirming field click queued by a drag release.  It is posted by the pump
 * tick, after the game has switched to target mode. */
static int g_cmd_target_pending;
static float g_cmd_target_x;
static float g_cmd_target_y;
static uint32_t g_cmd_target_button;
static uint32_t g_cmd_target_data;
static uint64_t g_cmd_target_ms;

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

static void dt_log_write(const char *fmt, ...)
{
    va_list ap;

    pthread_once(&g_dt_log_once, dt_open_log);
    if (!g_dt_log)
        return;

    va_start(ap, fmt);
    fputs("[dt] ", g_dt_log);
    vfprintf(g_dt_log, fmt, ap);
    fputc('\n', g_dt_log);
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
    g_beacon_active = 0;
    g_beacon_pointer = 0;
    g_beacon_moved = 0;
}

static void dt_beacon_cancel(void)
{
    g_beacon_armed = 0;
    dt_beacon_reset();
}

/* Defined later; the beacon release path uses the published minimap
 * rectangle and the battlefield rectangle before those helpers appear. */
static int dt_minimap_native_rect(float *left, float *top,
                                  float *right, float *bottom);
static int dt_field_contains(float nx, float ny);
static void dt_native_right_click(float x, float y);

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

static int dt_guest_readable(uint32_t va, uint32_t size)
{
    return va && hle_android_guest_ok(va, size);
}

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

static void dt_publish_route(Wc3DirectTouchRoute route)
{
    __atomic_store_n(&g_route, (int)route, __ATOMIC_RELEASE);
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
    if (!dt_frame_rect_snapshot(portrait + DT_PORTRAIT_FRAME, width, height,
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
    if (!dt_frame_rect_snapshot(minimap + DT_MINIMAP_FRAME, width, height,
                                &l, &t, &r, &b)) {
        dt_minimap_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    dt_minimap_publish(l, t, r, b);
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
    const uint32_t minimap = dt_minimap_object();
    if (!minimap) return 0;

    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui || !dt_guest_readable(ui, DT_UI_MINIMAP_SIGNAL + 4u)) return 0;

    /* MinimapSignal is a CGameUI child positioned over the minimap; the
     * pointer lives at CGameUI+0x440.  Requiring the published minimap
     * frame to be live keeps a stale button rect from claiming a
     * menu/loading touch after the HUD has gone away. */
    float ml = 0.0f;
    float mt = 0.0f;
    float mr = 0.0f;
    float mb = 0.0f;
    if (!dt_minimap_native_rect(&ml, &mt, &mr, &mb)) return 0;

    const uint32_t frame = wc3_rd32g(ui + DT_UI_MINIMAP_SIGNAL);
    if (!frame) {
#if WC3_DEBUG && WC3_DEBUG_INPUT
        static uint32_t last_ui = 0;
        if (ui != last_ui) {
            last_ui = ui;
            dt_log("beacon-frame ui=%08X minimap=%08X frame=00000000",
                   ui, minimap);
        }
#endif
        return 0;
    }
#if WC3_DEBUG && WC3_DEBUG_INPUT
    {
        static uint32_t last_frame = 0;
        static uint32_t last_visible = 0xFFFFFFFFu;
        static uint32_t last_raw[4];
        uint32_t raw[4] = { 0u, 0u, 0u, 0u };
        const int readable = dt_guest_readable(frame, DT_FRAME_RECT + 20u);
        const uint32_t visible =
            readable ? wc3_rd32g(frame + DT_FRAME_VISIBLE) : 0u;
        if (readable) {
            raw[0] = wc3_rd32g(frame + DT_FRAME_RECT + 0u);
            raw[1] = wc3_rd32g(frame + DT_FRAME_RECT + 4u);
            raw[2] = wc3_rd32g(frame + DT_FRAME_RECT + 8u);
            raw[3] = wc3_rd32g(frame + DT_FRAME_RECT + 12u);
        }
        if (frame != last_frame || visible != last_visible ||
            memcmp(raw, last_raw, sizeof(raw)) != 0) {
            last_frame = frame;
            last_visible = visible;
            memcpy(last_raw, raw, sizeof(raw));
            dt_log("beacon-frame ui=%08X minimap=%08X frame=%08X "
                   "readable=%d visible=%u raw_t=%08X raw_l=%08X "
                   "raw_b=%08X raw_r=%08X float=%.4f,%.4f,%.4f,%.4f",
                   ui, minimap, frame, readable, visible, raw[0], raw[1],
                   raw[2], raw[3], dt_float(raw[0]), dt_float(raw[1]),
                   dt_float(raw[2]), dt_float(raw[3]));
        }
    }
#endif
    return dt_frame_rect_snapshot(frame, width, height, left, top, right,
                                  bottom);
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
    static float beacon[4];
    static float command[4];
    static int portrait_present = -1;
    static int minimap_present = -1;
    static int beacon_present = -1;
    static int command_present = -1;

    dt_trace_one_rect(where, "portrait", &g_portrait_snapshot,
                      portrait, &portrait_present);
    dt_trace_one_rect(where, "minimap", &g_minimap_snapshot,
                      minimap, &minimap_present);
    dt_trace_one_rect(where, "beacon", &g_beacon_snapshot,
                      beacon, &beacon_present);
    dt_trace_one_rect(where, "command", &g_command_snapshot,
                      command, &command_present);
}
#endif

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
    uint32_t active_mode = 0;
    uint32_t target_mode = 0;
    uint32_t world = 0;
    uint32_t manager = 0;
    int world_present = 0;
    int click_gate = 0;
    int select_active = 0;
    int target_active = 0;

    if (!dt_guest_readable(DT_UI_GLOBAL, 4)) {
        goto done;
    }
    ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui || !dt_guest_readable(ui, DT_UI_WORLD_FRAME + 4u)) {
        reason = "no-ui";
        goto done;
    }

    select_mode = wc3_rd32g(ui + DT_UI_SELECT_MODE);
    active_mode = wc3_rd32g(ui + DT_UI_ACTIVE_MODE);
    target_mode = wc3_rd32g(ui + DT_UI_TARGET_MODE);
    world = dt_world_from_ui(ui);
    world_present = world != 0;
    click_gate = wc3_rd32g(ui + DT_UI_CLICK_GATE) != 0;

    /* No battlefield yet (menu/loading) or a modal UI blocked clicks: the
     * native window is still the right recipient. */
    if (!select_mode || !world_present || !click_gate) {
        route = WC3_DIRECT_TOUCH_ROUTE_NATIVE;
        reason = !select_mode ? "no-select-mode"
               : !world_present ? "no-world-frame"
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

    /* CGameUI+0x1B4 is the active mode; 0x6F36E330 is exactly an equality
     * check against it.  The +0x1B8 stack holds the previous modes, so
     * membership in that stack says nothing about what is active now. */
    select_active = select_mode && active_mode == select_mode;
    target_active = target_mode && active_mode == target_mode;
    route = wc3_dt_route(1, select_active, target_active,
                         active_mode == select_mode);
    reason = route == WC3_DIRECT_TOUCH_ROUTE_FIELD ? "field-ready"
                                                   : "mode-gate";

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
                   target_mode, world_present, click_gate, world, manager,
                   select_active, target_active, active_mode == select_mode,
                   target_mode && active_mode == target_mode);
        }
    }
#endif
    return route;
}

/* hle_guest_alloc() is 8-byte aligned and the guest mapping may live in a
 * 4 GiB host window, so host access always goes through guest_host_ptr().
 * The scratch is freed after one event. */
static uint32_t dt_scratch_alloc(void)
{
    uint32_t p = hle_guest_alloc(DT_SCRATCH_SIZE);
    if (!p || !hle_android_guest_ok(p, DT_SCRATCH_SIZE)) return 0;
    memset(guest_host_ptr(p), 0, DT_SCRATCH_SIZE);
    return p;
}

static void dt_scratch_free(uint32_t p)
{
    if (p) hle_guest_free(p);
}

static uint32_t dt_call_thiscall(CPU *c, uint32_t this_va, uint32_t fn,
                                 int argc, const uint32_t *args)
{
    c->ecx = this_va;
    c->edx = 0;
    return wc3_hle_call_guest(c, fn, argc, args);
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

    const float local_top = dt_float(wc3_rd32g(minimap + 0x644u));
    const float local_left = dt_float(wc3_rd32g(minimap + 0x648u));
    const float local_bottom = dt_float(wc3_rd32g(minimap + 0x64Cu));
    const float local_right = dt_float(wc3_rd32g(minimap + 0x650u));

    float sx = 0.0f;
    float sy = 0.0f;
    if (!wc3_dt_minimap_local_point(nx, ny, frame_top, frame_left,
                                    frame_bottom, frame_right, local_top,
                                    local_left, local_bottom, local_right,
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

static int dt_context(DtContext *ctx)
{
    if (!dt_guest_readable(DT_UI_GLOBAL, 4)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!dt_guest_readable(ui, DT_UI_WORLD_FRAME + 4)) return 0;
    const uint32_t world = dt_world_from_ui(ui);
    if (!world) return 0;
    if (!wc3_rd32g(ui + DT_UI_CLICK_GATE)) return 0;

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
    dt_beacon_snapshot(width, height);
    dt_command_snapshot(ui, width, height);
#if WC3_DEBUG && WC3_DEBUG_INPUT
    dt_trace_snapshots("idle");
#endif
    /* The snapshot decides between FIELD and NATIVE, including menus and
     * loading screens.  Only a missing UI is LEGACY. */
    dt_publish_route(dt_route_snapshot());
    /* A partial context is a transition in progress.  Keep the previous
     * marker: an event produced now (for example a tap on the loading
     * screen) must still compare against the old map and be dropped. */
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
    uint32_t cmd_228 = 0;
    uint32_t cmd_238 = 0;
    uint32_t cmd_23c = 0;
    uint32_t cmd_240 = 0;
    int cmd_target = 0;

    if (!dt_context(ctx)) {
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
    cmd_228 = dt_ui_mode_read(ctx->ui, DT_UI_CMD_MODE_228);
    cmd_238 = dt_ui_mode_read(ctx->ui, DT_UI_CMD_MODE_238);
    cmd_23c = dt_ui_mode_read(ctx->ui, DT_UI_CMD_MODE_23C);
    cmd_240 = dt_ui_mode_read(ctx->ui, DT_UI_CMD_MODE_240);
    cmd_target = wc3_dt_command_target_mode(active_mode, target_mode,
                                            cmd_228, cmd_238, cmd_23c,
                                            cmd_240);
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

static void dt_publish_service_route(Wc3DirectTouchRoute route)
{
    Wc3DirectTouchRoute published = route;
    if (route != WC3_DIRECT_TOUCH_ROUTE_FIELD) {
        /* Menus/loading/modal UI are native recipients even though the
         * semantic executor has no context. */
        published = dt_route_snapshot();
        if (published == WC3_DIRECT_TOUCH_ROUTE_FIELD) {
            /* The exact check rejected FIELD.  The pointer snapshot can still
             * see an active select mode while the map is not in game
             * (loading screen), so never publish FIELD for a route the
             * executor rejects. */
            published = WC3_DIRECT_TOUCH_ROUTE_NATIVE;
        }
    }
    dt_publish_route(published);
#if WC3_DEBUG && WC3_DEBUG_INPUT
    {
        static int last_published = -1;
        if ((int)published != last_published) {
            last_published = (int)published;
            dt_log("publish-service ready=%s published=%s",
                   dt_route_name(route), dt_route_name(published));
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
    dt_trace("portrait-follow", unit, 0);
}

/* Release: a null unit makes the native follow path drop its reference. */
static void dt_portrait_release(CPU *c)
{
    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    const uint32_t camera = dt_camera(ui);
    if (!camera) return;
    const uint32_t args[5] = { 0u, 0u, 0u, 0u, 0u };
    (void)dt_call_thiscall(c, camera, DT_FN_CAMERA_FOLLOW, 5, args);
    dt_trace("portrait-release", 0, 0);
}

static int dt_selection_nonempty(CPU *c, uint32_t selection)
{
    if (!selection) return 0;
    const int count = (int)dt_call_thiscall(c, selection,
                                             DT_FN_SELECTION_COUNT, 0, NULL);
    return count > 0 && count <= 40;
}

/* True when unit is in the selected-unit list (node+8, next at node+4). */
static int dt_selection_contains_unit(uint32_t selection, uint32_t unit)
{
    if (!selection || !unit) return 0;
    uint32_t node = wc3_rd32g(selection + 0x1F4u);
    for (uint32_t i = 0; i < 40u && node; i++) {
        if (!dt_guest_readable(node, 0x0Cu)) return 0;
        if (wc3_rd32g(node + 0x08u) == unit) return 1;
        node = wc3_rd32g(node + 0x04u);
    }
    return 0;
}

/* 0x6F293550 sets CSelectionWar3+0x20C from the last subgroup-list node
 * ([+0x204]+8).  A group selection can leave +0x20C zero, empty, or pointing
 * at a subgroup that is no longer part of the selection; the native order
 * paths (0x6F290240) then resolve a wrong or missing primary and silently
 * issue nothing.  Replicate the native write in those cases.  A populated
 * active subgroup whose first unit is still selected is kept: +0x20C is also
 * the user's active grid subgroup, not just the last one. */
static void dt_selection_ensure_active_group(CPU *c, uint32_t selection)
{
    if (!selection || !dt_guest_readable(selection + 0x20Cu, 4u)) return;
    const uint32_t active = wc3_rd32g(selection + 0x20Cu);
    if (active && dt_guest_readable(active, 0x14u)) {
        const uint32_t count = wc3_rd32g(active + 0x10u);
        if (count > 0) {
            const uint32_t index = 0;
            const uint32_t first = dt_call_thiscall(c, active, DT_FN_GROUP_AT,
                                                    1, &index);
            if (first && dt_selection_contains_unit(selection, first)) return;
            dt_log("selection active-group stale selection=%08X active=%08X "
                   "first=%08X", selection, active, first);
        } else {
            dt_log("selection active-group empty selection=%08X active=%08X",
                   selection, active);
        }
    }
    if (!dt_guest_readable(selection + 0x204u, 4u)) {
        dt_log("selection active-group no-list selection=%08X active=%08X",
               selection, active);
        return;
    }
    const uint32_t last = wc3_rd32g(selection + 0x204u);
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
    wc3_wr32g(selection + 0x20Cu, group);
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
 * node, and +0x20C is the active subgroup.  The walk uses the same list
 * shape as 0x6F293FB0/0x6F293D30: node+8 is the CUnit, node+4 is next. */
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

    const uint32_t active = dt_trace_word(selection, 0x20Cu);
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

    uint32_t node = dt_trace_word(selection, 0x1F4u);
    for (uint32_t i = 0; i < 40u && node && node != 0xFFFFFFFFu; i++) {
        if (!dt_guest_readable(node, 0x0Cu)) {
            dt_log("sel[%s] node[%u]=%08X unreadable", where, i, node);
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
     * predicate is the authority. */
    if (!primary || !dt_guest_readable(primary, 0x60u)) {
        dt_log("controlled selection=%08X primary=%08X readable=0",
               selection, primary);
        return 0;
    }
    const int can_order =
        dt_call_thiscall(c, primary, DT_FN_UNIT_CONTROLLABLE, 0, NULL) != 0;
    dt_log("controlled selection=%08X primary=%08X can_order=%d",
           selection, primary, can_order);
    return can_order;
}

static void dt_clear_selection(CPU *c, const DtContext *ctx)
{
    const uint32_t selection = dt_selection(c, ctx);
    if (!selection) return;
    const uint32_t mode = 0;
    (void)dt_call_thiscall(c, selection, DT_FN_CLEAR_SELECTION, 1, &mode);
}

/*
 * Canonical unit-select path.  0x6F395410 checks the pause gate, rejects a
 * right-button event, calls the native 0x6F358B40 thunk and then selects
 * through 0x6F6BCE50([unit+0x6A0]) or the 0x6F6BD380 fallback.  Reusing it
 * keeps the exact native sequence instead of replaying only its tail.
 */
static void dt_select_unit(CPU *c, uint32_t unit, uint32_t event)
{
    if (!dt_guest_readable(unit, 0x6A8u)) {
        dt_trace("select-unreadable", unit, 0);
        return;
    }
    memset(guest_host_ptr(event), 0, 0x14u);
    wc3_wr32g(event + 0x0Cu, unit);
    wc3_wr32g(event + 0x10u, 1u); /* left button: select, not order */
    const uint32_t args[1] = { event };
    (void)wc3_hle_call_guest(c, DT_FN_SELECT_EVENT, 1, args);
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
    g_grid_active = 0;
    g_grid_pointer = 0;
    g_grid_ui = 0;
    g_grid_selection = 0;
    g_grid_unit = 0;
    g_grid_target = 0;
}

/*
 * Resolve the live UI and selection without requiring the full field context.
 * A modal menu clears [ui+0x1AC]; in that state the grid is not a valid
 * recipient even if its frames are still visible underneath.
 */
static int dt_grid_context(CPU *c, uint32_t *ui_out, uint32_t *selection_out)
{
    if (!c || !ui_out || !selection_out) return 0;
    *ui_out = 0;
    *selection_out = 0;

    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui || !dt_guest_readable(ui, DT_UI_INFO_BAR + 4u)) return 0;
    if (!wc3_rd32g(ui + DT_UI_WORLD_FRAME)) return 0;
    if (!wc3_rd32g(ui + DT_UI_CLICK_GATE)) return 0;
    if (!wc3_rd32g(ui + DT_UI_SELECT_MODE)) return 0;

    if (!dt_guest_readable(DT_PLAYER_MANAGER, 4u)) return 0;
    const uint32_t manager = wc3_rd32g(DT_PLAYER_MANAGER);
    if (!manager || !dt_guest_readable(manager, 0x60u)) return 0;

    DtContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ui = ui;
    ctx.manager = manager;
    const uint32_t selection = dt_selection(c, &ctx);
    if (!selection) return 0;

    *ui_out = ui;
    *selection_out = selection;
    return 1;
}

static int dt_grid_targeting(uint32_t ui)
{
    if (!ui || !dt_guest_readable(ui, DT_UI_TARGET_MODE + 4u)) return 0;
    const uint32_t target = wc3_rd32g(ui + DT_UI_TARGET_MODE);
    if (!target) return 0;
    return wc3_rd32g(ui + DT_UI_ACTIVE_MODE) == target;
}

/* Return the CUnit under the normalised surface point, or 0 when the point is
 * outside the active grid or the cell has no live unit. */
static uint32_t dt_grid_button_at(CPU *c, uint32_t ui, float nx, float ny,
                                  uint32_t width, uint32_t height)
{
    if (!c || !ui || !dt_guest_readable(ui + DT_UI_INFO_BAR, 4u)) return 0;
    const uint32_t infobar = wc3_rd32g(ui + DT_UI_INFO_BAR);
    if (!infobar || !dt_guest_readable(infobar + DT_INFO_GROUP, 4u)) return 0;
    const uint32_t group = wc3_rd32g(infobar + DT_INFO_GROUP);
    if (!group || !dt_guest_readable(group, DT_GROUP_ACTIVE_GRID + 4u)) return 0;
    if (!wc3_rd32g(group + DT_GROUP_VISIBLE)) return 0;

    const uint32_t grid = wc3_rd32g(group + DT_GROUP_ACTIVE_GRID);
    if (!grid || !dt_guest_readable(grid, DT_GRID_ROW_ARRAY + 4u)) return 0;
    if (!wc3_rd32g(grid + DT_GRID_VISIBLE)) return 0;

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
            if (!dt_console_rect_snapshot(button, width, height, &left, &top,
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

/* Native 0x50065: a unit already in the active subgroup is selected
 * individually; otherwise 0x6F293480 makes its subgroup current and the
 * native command follows that with 0x6F291240(selection, 0).  Never call the
 * realize alone: it is a mirror update, not a selection change.
 *
 * The native handler also consults the player's selection options through
 * 0x6F0731A0(0)/(1): option 0 forces individual selection, and option 1
 * switches to the multi-unit 0x6F3968B0 branch.  Direct touch intentionally
 * follows the standard option-1-off "activate subgroup, then select the
 * individual unit" rule requested for the panel; the option overrides are
 * not reproduced. */
static void dt_grid_activate(CPU *c, uint32_t ui, uint32_t selection,
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
            /* Legacy semantics: the first touch activates the subgroup, a
             * touch on an already-active subgroup selects that unit. */
            dt_grid_select_unit(c, ui, holder);
            return;
        }
    }

    const uint32_t arg = holder;
    dt_grid_prepare_command_bar(c, ui);
    (void)dt_call_thiscall(c, selection, DT_FN_GRID_ACTIVATE, 1, &arg);
    const uint32_t mode = 0;
    (void)dt_call_thiscall(c, selection, DT_FN_SELECTION_REALIZE, 1, &mode);
    (void)dt_call_thiscall(c, 0u, DT_FN_UI_DIRTY, 0, NULL);
    dt_trace("grid-activate", holder,
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
        if (g_grid_active) return 1;

        uint32_t ui = 0;
        uint32_t selection = 0;
        if (!dt_grid_context(c, &ui, &selection)) return 0;
        const uint32_t unit =
            dt_grid_button_at(c, ui, ev->x, ev->y, width, height);
        if (!unit) return 0;

        g_grid_active = 1;
        g_grid_pointer = ev->pointer_id;
        g_grid_ui = ui;
        g_grid_selection = selection;
        g_grid_unit = unit;
        g_grid_target = 0;

        if (dt_grid_targeting(ui)) {
            const uint32_t target = wc3_rd32g(ui + DT_UI_TARGET_MODE);
            dt_grid_target(c, target, unit);
            g_grid_target = 1;
        } else {
            dt_grid_activate(c, ui, selection, unit);
        }
        dt_trace("grid-down", unit, g_grid_target);
        return 1;
    }
    case WC3_DIRECT_TOUCH_POINTER_MOVE: {
        if (!g_grid_active || ev->pointer_id != g_grid_pointer) return 0;
        if (g_grid_target) return 1;

        const uint32_t unit =
            dt_grid_button_at(c, g_grid_ui, ev->x, ev->y, width, height);
        if (unit && unit != g_grid_unit) {
            g_grid_unit = unit;
            dt_grid_activate(c, g_grid_ui, g_grid_selection, unit);
            dt_trace("grid-move", unit, 0);
        }
        return 1;
    }
    case WC3_DIRECT_TOUCH_POINTER_UP:
    case WC3_DIRECT_TOUCH_POINTER_CANCEL:
        if (!g_grid_active || ev->pointer_id != g_grid_pointer) return 0;
        dt_trace("grid-up", g_grid_unit, g_grid_target);
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

static void dt_tap(CPU *c, const DtContext *ctx, const Wc3DirectTouchEvent *ev,
                   uint32_t width, uint32_t height)
{
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

    /* The live selection decides select vs. order.  Compute it before the
     * pick because the two actions need different native pick contexts: a
     * smart order is a right-button pick and yields a CGhostImage at
     * pick+0x0C, while a selection is a left-button pick and yields a
     * CUnit.  0x6F21B730 only accepts the former; 0x6F1D73B0 validates the
     * latter. */
    dt_trace_selection(c, ctx, "tap-pre");
    const int controlled = dt_selection_controlled(c, ctx);
    dt_trace_selection(c, ctx, "tap-post");

    const uint32_t pick_button = controlled ? 4u : 1u;
    uint32_t object = 0, wx = 0, wy = 0, wz = 0;
    const int r = dt_pick(c, ctx, wc3_rd32g(point), wc3_rd32g(point + 4u),
                          pick, pick_button, &object, &wx, &wy, &wz);
    const uint32_t candidate = wc3_rd32g(pick + 8u);
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
    dt_log("tap pick r=%d object=%08X world=%08X,%08X,%08X controlled=%d "
           "button=%u", r, object, wx, wy, wz, controlled, pick_button);

    if (r == 1 && controlled) {
        /* Native right-button OnSelect path: validate the raw CGhostImage,
         * then let 0x6F392FA0 issue the smart order over the whole
         * selection.  A rejected unit hit must not become a ground order;
         * replay the native right click, which resolves the same point
         * through the window procedure. */
        const uint32_t ghost = dt_validate_object(c, object, unit_out);
        dt_log("tap ghost object=%08X validated=%08X", object, ghost);
        if (ghost) {
            const uint32_t order = dt_order_unit(c, ctx, scratch, object,
                                                 candidate, wx, wy, wz);
            dt_log("tap order-unit ret=%u", order);
            dt_trace("tap-order-unit", ghost, object);
            if (!order) {
                dt_log("tap order-unit fallback native pending=%d down=%d",
                       g_native_right_pending, hle_android_mouse_is_down(2));
                dt_native_right_click(ev->x, ev->y);
                dt_trace("tap-order-unit-native", ghost, object);
            }
            dt_release_object(c, ghost);
            dt_scratch_free(scratch);
            return;
        }
        dt_log("tap invalid-ghost object=%08X", object);
        dt_native_right_click(ev->x, ev->y);
        dt_trace("tap-order-native", object, 0);
        dt_scratch_free(scratch);
        return;
    }

    if (r == 1 && !controlled) {
        /* Left-button pick: the object is a CUnit, not a CGhostImage. */
        const uint32_t unit = dt_validate_unit(c, object, unit_out);
        const int readable = unit && dt_guest_readable(unit, 0x10u);
        const uint32_t unit_class =
            readable ? wc3_rd32g(unit + 0x0Cu) : 0;
        /* 0x6F1D73B0 already checked the CUnit class; do not repeat the
         * check through the CGhostImage-only class tag. */
        const int is_unit = unit != 0;
        const int intent = wc3_dt_tap_intent(r, 1, is_unit, controlled);
        dt_log("tap unit object=%08X unit=%08X readable=%d class=%08X "
               "is_unit=%d intent=%d",
               object, unit, readable, unit_class, is_unit, intent);
        if (unit) {
            switch (intent) {
            case WC3_DT_INTENT_SELECT_UNIT:
                dt_select_unit(c, unit, scratch + DT_EVENT_OFFSET);
                dt_trace("tap-select-unit", unit, 0);
                break;
            case WC3_DT_INTENT_CLEAR_SELECTION:
                dt_clear_selection(c, ctx);
                dt_trace("tap-clear-unit", unit, 0);
                break;
            case WC3_DT_INTENT_NONE:
                dt_trace("tap-non-unit", object, unit_class);
                break;
            default:
                break;
            }
            dt_release_object(c, unit);
            dt_scratch_free(scratch);
            return;
        }
        /* An object that no longer validates is the native "invalid sprite"
         * case: with select mode active and a left button, OnSelect
         * substitutes a terrain click.  Fall through to the world point. */
        dt_log("tap invalid-unit object=%08X", object);
    }

    if (r == 1 || r == 2) {
        const int intent = wc3_dt_tap_intent(r, 0, 0, controlled);
        dt_log("tap terrain r=%d controlled=%d intent=%d world=%08X,%08X",
               r, controlled, intent, wx, wy);
        switch (intent) {
        case WC3_DT_INTENT_ORDER_GROUND: {
            /* 0x6F394680 already walks the whole selection; the group case
             * does not need a native replay.  Keep the replay only when the
             * native path returned 0 (paused, no primary, uncontrollable). */
            const uint32_t order = dt_order_ground(c, ctx, wx, wy);
            dt_log("tap order-ground ret=%u", order);
            dt_trace("tap-order-ground", wx, wy);
            if (!order) {
                dt_log("tap order-ground fallback native pending=%d down=%d",
                       g_native_right_pending,
                       hle_android_mouse_is_down(2));
                dt_native_right_click(ev->x, ev->y);
                dt_trace("tap-order-ground-native", wx, wy);
            }
            break;
        }
        case WC3_DT_INTENT_CLEAR_SELECTION:
            dt_clear_selection(c, ctx);
            dt_trace("tap-clear", 0, 0);
            break;
        default:
            break;
        }
    } else {
        dt_log("tap no-hit r=%d controlled=%d", r, controlled);
        if (controlled) {
            /* 0x6F3840E0 can reject before its raycast (world/viewport/pick
             * bound gates), leaving no semantic target.  Replay the same
             * gesture as a native right click: the window procedure owns
             * those gates and still resolves the smart order for the live
             * selection. */
            dt_log("tap order-native fallback pending=%d down=%d",
                   g_native_right_pending, hle_android_mouse_is_down(2));
            dt_native_right_click(ev->x, ev->y);
            dt_trace("tap-order-native", 0, 0);
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
        *x = g_minimap_native_x;
        *y = g_minimap_native_y;
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
    g_minimap_native_x = *x;
    g_minimap_native_y = *y;
}

static void dt_minimap_native_reset(void)
{
    g_minimap_native_active = 0;
    g_minimap_native_pointer = 0;
    g_minimap_native_pressed = 0;
}

static void dt_native_release(void)
{
    /* A beacon gesture is never backed by a native mouse button, but a
     * context change, reset or stale release must still drop its armed and
     * in-flight state. */
    dt_beacon_cancel();
    const int pressed = g_native_down || g_minimap_native_pressed;
    if (pressed && g_minimap_native_active) {
        /* Context change/stale release: put the cursor back at the last
         * clamped minimap point before the UP, so the native frame manager
         * still routes the release to the minimap. */
        hle_android_mouse_move(g_minimap_native_x, g_minimap_native_y);
    }
    dt_minimap_native_reset();
    if (!pressed) return;
    /* The logical bit may already be clear (overlay release_all, surface
     * stop, duplicate cancel).  The guest may still own a native drag, so
     * post the matching UP unconditionally. */
    hle_android_mouse_button_up_force(0);
    g_native_down = 0;
    g_native_up_pending = 1;
}

/*
 * Native UI input.  The event coordinates are surface-normalised, exactly
 * what hle_android_mouse_move expects.  Posting WM_MOUSEMOVE/WM_LBUTTONDOWN/
 * WM_LBUTTONUP is the same path a real mouse uses; it does not touch the
 * overlay touch engine.  The guest window procedure then runs the minimap,
 * command card, portrait, resource bar and targeting/placement handlers with
 * their native semantics.
 */
static void dt_native_pointer(const Wc3DirectTouchEvent *ev)
{
#if WC3_DEBUG && WC3_DEBUG_INPUT
    {
        static uint32_t move_count;
        if (ev->kind != WC3_DIRECT_TOUCH_POINTER_MOVE ||
            (move_count++ & 7u) == 0u) {
            dt_log("native-ptr kind=%s ptr=%u n=%.4f,%.4f down=%d "
                   "up_pending=%d hw_down=%d",
                   dt_kind_name(ev->kind), ev->pointer_id, ev->x, ev->y,
                   g_native_down, g_native_up_pending,
                   hle_android_mouse_is_down(0));
        }
    }
#endif
    switch (ev->kind) {
    case WC3_DIRECT_TOUCH_POINTER_DOWN:
        if (g_native_up_pending && !hle_android_mouse_is_down(0)) {
            /* The queued release has been dispatched; a normal DOWN will do. */
            g_native_up_pending = 0;
        }
        if (g_native_down ||
            (!g_native_up_pending && hle_android_mouse_is_down(0))) {
            /* The overlay engine already owns the left button, or a previous
             * native press has not been released yet.  Do not post a second
             * DOWN: the matching UP would then cancel the other owner. */
            dt_trace("native-down-busy", ev->pointer_id, 0);
            break;
        }
        hle_android_mouse_move(ev->x, ev->y);
        if (g_native_up_pending) {
            /* Our own UP is queued but not dispatched yet.  The queue order
             * keeps it ahead of this DOWN, so the guest still sees a clean
             * DOWN/UP pair instead of losing a rapid second tap. */
            hle_android_mouse_button_down_force(0);
        } else {
            hle_android_mouse_button_ex(0, 1);
        }
        g_native_down = 1;
        g_native_up_pending = 0;
        dt_trace("native-down", dt_bits(ev->x), dt_bits(ev->y));
        break;
    case WC3_DIRECT_TOUCH_POINTER_MOVE:
        hle_android_mouse_move(ev->x, ev->y);
        break;
    case WC3_DIRECT_TOUCH_POINTER_UP:
        hle_android_mouse_move(ev->x, ev->y);
        if (g_native_down) {
            /* Force the UP: another release path may have cleared the
             * logical bit while the guest still owns the native press. */
            hle_android_mouse_button_up_force(0);
            g_native_down = 0;
            g_native_up_pending = 1;
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
 *   DOWN on the signal button          -> start a gesture;
 *   DOWN on the minimap while armed    -> start an armed placement;
 *   MOVE                               -> consume, remember whether it moved;
 *   UP on the minimap                  -> place at the release point;
 *   UP in the field                    -> pick and place at the release point;
 *   UP on the button without movement  -> arm the next minimap press;
 *   UP anywhere else after movement    -> cancel.
 *
 * A press anywhere else disarms the one-shot mode and is handled normally.
 */
static int dt_beacon_pointer(CPU *c, const DtContext *ctx,
                             const Wc3DirectTouchEvent *ev,
                             uint32_t width, uint32_t height)
{
    if (!ev) return 0;

    if (ev->kind == WC3_DIRECT_TOUCH_POINTER_DOWN) {
        if (g_beacon_active) {
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
               ev->pointer_id, ev->x, ev->y, g_beacon_active, g_beacon_armed,
               have_button, left, top, right, bottom, on_button);
        if (on_button) {
            g_beacon_armed = 0;
            g_beacon_active = 1;
            g_beacon_pointer = ev->pointer_id;
            g_beacon_down_x = ev->x;
            g_beacon_down_y = ev->y;
            g_beacon_moved = 0;
            dt_trace("beacon-down", dt_bits(ev->x), dt_bits(ev->y));
            return 1;
        }

        if (g_beacon_armed) {
            float ml = 0.0f;
            float mt = 0.0f;
            float mr = 0.0f;
            float mb = 0.0f;
            const int have_minimap =
                dt_minimap_native_rect(&ml, &mt, &mr, &mb);
            const int on_minimap =
                have_minimap &&
                ev->x >= ml && ev->x <= mr &&
                ev->y >= mt && ev->y <= mb;
            dt_log("beacon-armed-down ptr=%u n=%.4f,%.4f have_minimap=%d "
                   "minimap=%.4f,%.4f..%.4f,%.4f on_minimap=%d",
                   ev->pointer_id, ev->x, ev->y, have_minimap,
                   ml, mt, mr, mb, on_minimap);
            if (on_minimap) {
                g_beacon_active = 1;
                g_beacon_pointer = ev->pointer_id;
                g_beacon_down_x = ev->x;
                g_beacon_down_y = ev->y;
                g_beacon_moved = 0;
                dt_trace("beacon-armed-down", dt_bits(ev->x),
                         dt_bits(ev->y));
                return 1;
            }
            /* One-shot: a press outside the minimap cancels the armed mode
             * and is handled by the normal route. */
            g_beacon_armed = 0;
            dt_trace("beacon-disarm", dt_bits(ev->x), dt_bits(ev->y));
        }
        return 0;
    }

    if (ev->kind == WC3_DIRECT_TOUCH_POINTER_CANCEL) {
        if (!g_beacon_active && !g_beacon_armed) return 0;
        const int owned = g_beacon_active;
        dt_beacon_cancel();
        dt_trace("beacon-cancel", ev->pointer_id, 0);
        return owned ? 1 : 0;
    }

    if (!g_beacon_active || ev->pointer_id != g_beacon_pointer) return 0;

    if (ev->kind == WC3_DIRECT_TOUCH_POINTER_MOVE) {
        if (!g_beacon_moved) {
            const float dx = ev->x - g_beacon_down_x;
            const float dy = ev->y - g_beacon_down_y;
            if (dx * dx + dy * dy >
                DT_BEACON_TAP_SLOP * DT_BEACON_TAP_SLOP) {
                g_beacon_moved = 1;
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
        dt_minimap_native_rect(&ml, &mt, &mr, &mb) &&
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
    const int inside_field = dt_field_contains(ev->x, ev->y);
    const Wc3DtBeaconRelease release =
        wc3_dt_beacon_release(inside_button, inside_minimap, inside_field,
                              g_beacon_moved);
    dt_log("beacon-up ptr=%u n=%.4f,%.4f moved=%d button=%d"
           "(%.4f,%.4f..%.4f,%.4f) minimap=%d(%.4f,%.4f..%.4f,%.4f) "
           "field=%d release=%d",
           ev->pointer_id, ev->x, ev->y, g_beacon_moved, inside_button,
           bl, bt, br, bb, inside_minimap, ml, mt, mr, mb, inside_field,
           (int)release);

    g_beacon_active = 0;
    g_beacon_pointer = 0;
    g_beacon_moved = 0;

    switch (release) {
    case WC3_DT_BEACON_MINIMAP:
        g_beacon_armed = 0;
        if (dt_beacon_place_minimap(c, ev->x, ev->y, width, height)) {
            dt_trace("beacon-place-minimap", dt_bits(ev->x),
                     dt_bits(ev->y));
        }
        return 1;
    case WC3_DT_BEACON_FIELD:
        g_beacon_armed = 0;
        if (dt_beacon_place_field(c, ctx, ev->x, ev->y, width, height)) {
            dt_trace("beacon-place-field", dt_bits(ev->x),
                     dt_bits(ev->y));
        }
        return 1;
    case WC3_DT_BEACON_ARM:
        g_beacon_armed = 1;
        dt_trace("beacon-arm", dt_bits(ev->x), dt_bits(ev->y));
        return 1;
    default:
        g_beacon_armed = 0;
        dt_trace("beacon-none", dt_bits(ev->x), dt_bits(ev->y));
        return 1;
    }
}

/*
 * Native minimap backstop.  The controls layer clamps a pointer it owns as
 * MINIMAP, but the service sees only raw NATIVE phases: the route is not
 * carried in the event.  A DOWN inside the published rectangle starts a
 * minimap-owned native drag here too, so the release stays inside the frame
 * even if the controls gate missed (stale/empty snapshot at DOWN) and the
 * finger came up far outside.  The UP is forced because the logical left bit
 * may already be clear by the time the raw phase arrives.
 */
static void dt_native_minimap_pointer(const Wc3DirectTouchEvent *ev,
                                      uint32_t width, uint32_t height)
{
    if (!ev) return;

    if (ev->kind == WC3_DIRECT_TOUCH_POINTER_DOWN) {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        const int inside =
            !g_native_down && !g_field_select_active &&
            !g_field_camera_active &&
            (!hle_android_mouse_is_down(0) || g_native_up_pending) &&
            dt_minimap_native_rect(&left, &top, &right, &bottom) &&
            ev->x >= left && ev->x <= right &&
            ev->y >= top && ev->y <= bottom;
        dt_log("minimap-native-down ptr=%u n=%.4f,%.4f inside=%d "
               "native_down=%d field_select=%d field_camera=%d "
               "up_pending=%d hw_down=%d rect=%.4f,%.4f..%.4f,%.4f",
               ev->pointer_id, ev->x, ev->y, inside, g_native_down,
               g_field_select_active, g_field_camera_active,
               g_native_up_pending, hle_android_mouse_is_down(0),
               left, top, right, bottom);
        if (!inside) {
            dt_native_pointer(ev);
            return;
        }
        Wc3DirectTouchEvent clamped = *ev;
        g_minimap_native_active = 1;
        g_minimap_native_pointer = ev->pointer_id;
        g_minimap_native_x = ev->x;
        g_minimap_native_y = ev->y;
        dt_minimap_native_clamp(&clamped.x, &clamped.y, width, height);
        dt_native_pointer(&clamped);
        g_minimap_native_pressed = g_native_down;
        dt_trace("minimap-native-down", dt_bits(clamped.x),
                 dt_bits(clamped.y));
        return;
    }

    if (!g_minimap_native_active ||
        ev->pointer_id != g_minimap_native_pointer ||
        (ev->kind != WC3_DIRECT_TOUCH_POINTER_MOVE &&
         ev->kind != WC3_DIRECT_TOUCH_POINTER_UP &&
         ev->kind != WC3_DIRECT_TOUCH_POINTER_CANCEL)) {
        dt_native_pointer(ev);
        return;
    }

    Wc3DirectTouchEvent clamped = *ev;
    dt_minimap_native_clamp(&clamped.x, &clamped.y, width, height);

    if (ev->kind == WC3_DIRECT_TOUCH_POINTER_MOVE) {
        dt_native_pointer(&clamped);
        dt_trace("minimap-native-move", dt_bits(clamped.x),
                 dt_bits(clamped.y));
        return;
    }

    /* UP or CANCEL.  Post the final in-rect move first, then the release.
     * dt_native_pointer() would skip the release when g_native_down is
     * already clear, which is exactly the latched-drag case this backstop
     * exists for. */
    hle_android_mouse_move(clamped.x, clamped.y);
    dt_log("minimap-native-release ptr=%u n=%.4f,%.4f kind=%s "
           "native_down=%d pressed=%d",
           ev->pointer_id, clamped.x, clamped.y, dt_kind_name(ev->kind),
           g_native_down, g_minimap_native_pressed);
    if (g_native_down || g_minimap_native_pressed) {
        hle_android_mouse_button_up_force(0);
        g_native_down = 0;
        g_native_up_pending = 1;
    }
    dt_minimap_native_reset();
    dt_trace(ev->kind == WC3_DIRECT_TOUCH_POINTER_CANCEL
                 ? "minimap-native-cancel" : "minimap-native-up",
             dt_bits(clamped.x), dt_bits(clamped.y));
}

/*
 * Field camera drag.  Legacy singleTapDragAction is MOUSE_MIDDLE_BUTTON; the
 * native game rotates the camera for a middle-button drag.  The recognizer
 * starts it only after the finger crosses the legacy drag threshold, so a
 * tap never presses the middle button.
 */
static void dt_field_camera_begin(const Wc3DirectTouchEvent *ev)
{
    dt_log("field-camera-begin ptr=%u n=%.4f,%.4f active=%d "
           "middle_pending=%d hw_middle=%d",
           ev->pointer_id, ev->x, ev->y, g_field_camera_active,
           g_native_middle_pending, hle_android_mouse_is_down(1));
    if (g_field_camera_active) {
        /* Defensive: a duplicate BEGIN must not leave the previous press
         * without its release. */
        hle_android_mouse_button_up_force(1);
        g_native_middle_pending = 1;
    }
    hle_android_mouse_move(ev->x, ev->y);
    if (g_native_middle_pending && !hle_android_mouse_is_down(1)) {
        /* The queued release has been dispatched; a normal DOWN will do. */
        g_native_middle_pending = 0;
    }
    if (g_native_middle_pending) {
        /* The matching UP is still queued ahead of this DOWN. */
        hle_android_mouse_button_down_force(1);
    } else {
        hle_android_mouse_button_ex(1, 1);
    }
    g_native_middle_pending = 0;
    g_field_camera_active = 1;
    g_field_camera_pointer = (int)ev->pointer_id;
    dt_trace("field-camera-begin", dt_bits(ev->x), dt_bits(ev->y));
}

static void dt_field_camera_move(const Wc3DirectTouchEvent *ev)
{
    if (!g_field_camera_active ||
        (int)ev->pointer_id != g_field_camera_pointer) {
        dt_log("field-camera-move ignored ptr=%u active=%d owner=%d",
               ev->pointer_id, g_field_camera_active,
               g_field_camera_pointer);
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_INPUT
    {
        static uint32_t move_count;
        if ((move_count++ & 7u) == 0u) {
            dt_log("field-camera-move ptr=%u n=%.4f,%.4f",
                   ev->pointer_id, ev->x, ev->y);
        }
    }
#endif
    hle_android_mouse_move(ev->x, ev->y);
}

static void dt_field_camera_end(const Wc3DirectTouchEvent *ev)
{
    dt_log("field-camera-end ptr=%u n=%.4f,%.4f active=%d owner=%d",
           ev->pointer_id, ev->x, ev->y, g_field_camera_active,
           g_field_camera_pointer);
    if (!g_field_camera_active ||
        (int)ev->pointer_id != g_field_camera_pointer) {
        return;
    }
    hle_android_mouse_move(ev->x, ev->y);
    hle_android_mouse_button_up_force(1);
    g_native_middle_pending = 1;
    g_field_camera_active = 0;
    g_field_camera_pointer = 0;
    dt_trace("field-camera-end", dt_bits(ev->x), dt_bits(ev->y));
}

/*
 * Field selection.  Legacy longPressDragAction is MOUSE_LEFT_BUTTON, so the
 * native game draws and commits its own selection rectangle.  The recognizer
 * emits BEGIN at the long-press deadline, every move in between emits MOVE,
 * and the finger release emits END.  The executor posts WM_LBUTTONDOWN at
 * BEGIN and keeps it down, so a stationary release is a native click that
 * selects one unit and a drag is the native box selection.  MOVE/END reuse
 * the native pointer state machine so a rapid gesture gets the same
 * queued-release handling as a HUD click.
 */
static void dt_field_select_begin(const Wc3DirectTouchEvent *ev)
{
    dt_log("field-select-begin ptr=%u n=%.4f,%.4f active=%d native_down=%d "
           "up_pending=%d hw_down=%d",
           ev->pointer_id, ev->x, ev->y, g_field_select_active,
           g_native_down, g_native_up_pending,
           hle_android_mouse_is_down(0));
    if (g_field_select_active) {
        Wc3DirectTouchEvent release = *ev;
        release.kind = WC3_DIRECT_TOUCH_POINTER_UP;
        dt_native_pointer(&release);
        g_field_select_active = 0;
        g_field_select_pointer = 0;
    }
    hle_android_mouse_move(ev->x, ev->y);
    if (g_native_down ||
        (!g_native_up_pending && hle_android_mouse_is_down(0))) {
        /* A HUD click or another native press still owns the left button.
         * Do not steal it: the selection drag is dropped instead of posting
         * a DOWN whose matching UP would cancel the other owner. */
        dt_trace("field-select-busy", ev->pointer_id, 0);
        return;
    }
    if (g_native_up_pending) {
        hle_android_mouse_button_down_force(0);
    } else {
        hle_android_mouse_button_ex(0, 1);
    }
    g_native_down = 1;
    g_native_up_pending = 0;
    g_field_select_active = 1;
    g_field_select_pointer = (int)ev->pointer_id;
    dt_trace("field-select-begin", dt_bits(ev->x), dt_bits(ev->y));
}

static void dt_field_select_move(const Wc3DirectTouchEvent *ev)
{
    if (!g_field_select_active ||
        (int)ev->pointer_id != g_field_select_pointer) {
        dt_log("field-select-move ignored ptr=%u active=%d owner=%d",
               ev->pointer_id, g_field_select_active,
               g_field_select_pointer);
        return;
    }
    Wc3DirectTouchEvent move = *ev;
    move.kind = WC3_DIRECT_TOUCH_POINTER_MOVE;
    dt_native_pointer(&move);
}

static void dt_field_select_end(const Wc3DirectTouchEvent *ev)
{
    dt_log("field-select-end ptr=%u n=%.4f,%.4f active=%d owner=%d",
           ev->pointer_id, ev->x, ev->y, g_field_select_active,
           g_field_select_pointer);
    if (!g_field_select_active ||
        (int)ev->pointer_id != g_field_select_pointer) {
        return;
    }
    Wc3DirectTouchEvent release = *ev;
    release.kind = WC3_DIRECT_TOUCH_POINTER_UP;
    dt_native_pointer(&release);
    g_field_select_active = 0;
    g_field_select_pointer = 0;
    dt_trace("field-select-end", dt_bits(ev->x), dt_bits(ev->y));
}

/* Release a field drag without an END event (map change, reset, stale queue
 * entry).  The recognizer's END event is preferred; this is the safety net
 * that keeps a native button from staying latched. */
static void dt_field_reset(void)
{
    if (g_field_camera_active) {
        dt_log("field-reset camera ptr=%d", g_field_camera_pointer);
        hle_android_mouse_button_up_force(1);
        g_native_middle_pending = 1;
        g_field_camera_active = 0;
        g_field_camera_pointer = 0;
    }
    if (g_field_select_active) {
        dt_log("field-reset select ptr=%d native_down=%d",
               g_field_select_pointer, g_native_down);
        if (g_native_down) {
            hle_android_mouse_button_up_force(0);
            g_native_down = 0;
            g_native_up_pending = 1;
        }
        g_field_select_active = 0;
        g_field_select_pointer = 0;
    }
}

static int dt_field_drag_active(void)
{
    return g_field_camera_active || g_field_select_active;
}

static int dt_is_field_drag_kind(uint32_t kind)
{
    return kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN ||
           kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE ||
           kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END ||
           kind == WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN ||
           kind == WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE ||
           kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END;
}

static int dt_is_field_drag_end(uint32_t kind)
{
    return kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_END ||
           kind == WC3_DIRECT_TOUCH_FIELD_SELECT_END;
}

/*
 * A semantic gesture can be recognized just as the game enters targeting or
 * placement.  The controls layer decided FIELD on DOWN, so the event arrives
 * as TAP/LONG_PRESS, but the native UI now owns the click.  Replay it as a
 * native click instead of dropping the user's target selection.
 */
static void dt_native_click(const Wc3DirectTouchEvent *ev)
{
    Wc3DirectTouchEvent pointer = *ev;
    pointer.kind = WC3_DIRECT_TOUCH_POINTER_DOWN;
    dt_native_pointer(&pointer);
    pointer.kind = WC3_DIRECT_TOUCH_POINTER_UP;
    dt_native_pointer(&pointer);
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
    g_inv_active = 0;
    g_inv_pointer = 0;
    g_inv_bar = 0;
    g_inv_slot = 0;
    g_inv_down_x = 0.0f;
    g_inv_down_y = 0.0f;
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

/* Replay a native left click.  dt_native_pointer keeps the pending-release
 * bookkeeping correct, so a tap right after another native click is not
 * dropped by the logical button state. */
static void dt_native_left_click(float x, float y)
{
    Wc3DirectTouchEvent pointer;
    memset(&pointer, 0, sizeof pointer);
    pointer.kind = WC3_DIRECT_TOUCH_POINTER_DOWN;
    pointer.x = x;
    pointer.y = y;
    dt_native_pointer(&pointer);
    pointer.kind = WC3_DIRECT_TOUCH_POINTER_UP;
    dt_native_pointer(&pointer);
}

/* Replay a native right click.  The inventory uses it to enter move mode; the
 * command card uses it to toggle autocast.  The pending flag is shared because
 * both paths drive the same physical button. */
static void dt_native_right_click(float x, float y)
{
    dt_log("native right click x=%.4f y=%.4f pending=%d down=%d",
           x, y, g_native_right_pending, hle_android_mouse_is_down(2));
    if (g_native_right_pending && !hle_android_mouse_is_down(2)) {
        g_native_right_pending = 0;
    }
    hle_android_mouse_move(x, y);
    if (g_native_right_pending) {
        hle_android_mouse_button_down_force(2);
    } else {
        hle_android_mouse_button_ex(2, 1);
    }
    hle_android_mouse_button_ex(2, 0);
    g_native_right_pending = 1;
}

/*
 * Resolve the release.  Same slot is a tap and uses the item; another slot
 * is a swap/move confirmed by a left click on that slot; a field point is a
 * drop confirmed by a left click at the world position.  Anywhere else
 * cancels, so a release over the command card or a HUD panel never turns
 * into a world order.
 */
static void dt_inventory_finish(const Wc3DirectTouchEvent *ev,
                                uint32_t width, uint32_t height)
{
    const uint32_t bar = g_inv_bar;
    const uint32_t src_slot = g_inv_slot;
    const float down_x = g_inv_down_x;
    const float down_y = g_inv_down_y;

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
    dt_inventory_reset();

    if (dest_slot == (int)src_slot) {
        dt_native_left_click(down_x, down_y);
        dt_trace("inv-tap", src_slot, 0);
        return;
    }
    if (dest_slot >= 0 && dest_button) {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        if (dt_console_rect_read(dest_button, width, height, &left, &top,
                                 &right, &bottom)) {
            dt_native_right_click(down_x, down_y);
            dt_native_left_click((left + right) * 0.5f,
                                    (top + bottom) * 0.5f);
            dt_trace("inv-slot", src_slot, (uint32_t)dest_slot);
            return;
        }
    }
    if (dt_field_contains(ev->x, ev->y)) {
        dt_native_right_click(down_x, down_y);
        dt_native_left_click(ev->x, ev->y);
        dt_trace("inv-drop", src_slot, 0);
        return;
    }
    dt_trace("inv-cancel", src_slot, 0);
}

static int dt_inventory_pointer(const Wc3DirectTouchEvent *ev,
                                uint32_t width, uint32_t height)
{
    if (!ev || ev->route != WC3_DIRECT_TOUCH_ROUTE_NATIVE) return 0;

    switch (ev->kind) {
    case WC3_DIRECT_TOUCH_POINTER_DOWN: {
        if (g_inv_active) return 1;
        /* A grid gesture already owns the screen contact; let the grid
         * handler swallow this second DOWN instead of starting a second
         * gesture here. */
        if (g_grid_active) return 0;

        uint32_t bar = 0;
        if (!dt_inventory_context(&bar)) return 0;
        const int slot = dt_inventory_slot_at(bar, ev->x, ev->y, width,
                                              height, 1, NULL);
        if (slot < 0) return 0;

        g_inv_active = 1;
        g_inv_pointer = ev->pointer_id;
        g_inv_bar = bar;
        g_inv_slot = (uint32_t)slot;
        g_inv_down_x = ev->x;
        g_inv_down_y = ev->y;
        dt_trace("inv-down", (uint32_t)slot, bar);
        return 1;
    }
    case WC3_DIRECT_TOUCH_POINTER_MOVE:
        if (!g_inv_active || ev->pointer_id != g_inv_pointer) return 0;
        return 1;
    case WC3_DIRECT_TOUCH_POINTER_UP:
        if (!g_inv_active || ev->pointer_id != g_inv_pointer) return 0;
        dt_inventory_finish(ev, width, height);
        return 1;
    case WC3_DIRECT_TOUCH_POINTER_CANCEL:
        if (!g_inv_active || ev->pointer_id != g_inv_pointer) return 0;
        dt_trace("inv-abort", g_inv_slot, 0);
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
    g_cmd_down_x = 0.0f;
    g_cmd_down_y = 0.0f;
    g_cmd_down_ms = 0;
    g_cmd_moved = 0;
    g_cmd_long_pending = 0;
    g_cmd_long_fired = 0;
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
 * native handler unchanged. */
static int dt_command_context(uint32_t *bar_out, int allow_targeting)
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
    if (!world || !gate || !select_mode || (!allow_targeting && targeting)) {
        dt_log("cmd-ctx-fail world=%08X gate=%08X sel=%08X tgt=%d allow=%d",
               world, gate, select_mode, targeting, allow_targeting);
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

/* The game's own command target-mode state.  The command card click handler
 * (0x6F3BE1E0) selects an input mode by the command data type; those modes
 * live at CGameUI+0x228/+0x238/+0x23C/+0x240, not at the selected-unit grid
 * target mode +0x210.  A live mode decides the release; the command data
 * flags are only diagnostic. */
static int dt_command_targeting(void)
{
    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui) return 0;
    return wc3_dt_command_target_mode(
        dt_ui_mode_read(ui, DT_UI_ACTIVE_MODE),
        dt_ui_mode_read(ui, DT_UI_TARGET_MODE),
        dt_ui_mode_read(ui, DT_UI_CMD_MODE_228),
        dt_ui_mode_read(ui, DT_UI_CMD_MODE_238),
        dt_ui_mode_read(ui, DT_UI_CMD_MODE_23C),
        dt_ui_mode_read(ui, DT_UI_CMD_MODE_240));
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
    if (!dt_command_context(&bar, 0) || bar != g_cmd_bar) {
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
static void dt_command_tick(uint32_t width, uint32_t height)
{
    /* A drag release queued a confirming field click.  Wait until the game's
     * own update has made target mode live, then post it; a non-targeted
     * command never arms and the queued click is dropped by the timeout. */
    if (g_cmd_target_pending) {
        const int targeted = dt_command_targeting();
        const uint64_t now = dt_now_ms();
        if (wc3_dt_command_release(1, targeted, 1) ==
            WC3_DT_COMMAND_TARGET) {
            dt_native_left_click(g_cmd_target_x, g_cmd_target_y);
            dt_log("cmd target button=%08X data=%08X x=%.4f y=%.4f",
                   g_cmd_target_button, g_cmd_target_data,
                   g_cmd_target_x, g_cmd_target_y);
            dt_trace("cmd-drag", g_cmd_target_button, g_cmd_target_data);
            g_cmd_target_pending = 0;
        } else if (g_cmd_target_ms && now && now >= g_cmd_target_ms &&
                   now - g_cmd_target_ms >= DT_CMD_TARGET_TIMEOUT_MS) {
            dt_trace("cmd-target-timeout", g_cmd_target_button,
                     g_cmd_target_data);
            g_cmd_target_pending = 0;
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
static void dt_command_finish(const Wc3DirectTouchEvent *ev,
                              uint32_t width, uint32_t height)
{
    const uint32_t bar = g_cmd_bar;
    const uint32_t button = g_cmd_button;
    const uint32_t data = g_cmd_data;
    const float down_x = g_cmd_down_x;
    const float down_y = g_cmd_down_y;
    int moved = g_cmd_moved;
    const int long_fired = g_cmd_long_fired;

    if (long_fired) {
        dt_trace("cmd-long-release", button, 0);
        dt_command_reset();
        return;
    }

    /* The UI may have been replaced between DOWN and UP.  Never post a click
     * into a new command page or a menu.  The pressed button itself may be
     * rebuilt by the command the DOWN click ran, so it is not used as a
     * second hit-test gate; only the bar identity decides the context. */
    uint32_t live_bar = 0;
    if (!dt_command_context(&live_bar, 1) || live_bar != bar) {
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

    if (dt_field_contains(ev->x, ev->y)) {
        const int targeted = dt_command_targeting();
        if (wc3_dt_command_release(moved, targeted, 1) ==
            WC3_DT_COMMAND_TARGET) {
            /* Target mode is already live; confirm now. */
            dt_native_left_click(ev->x, ev->y);
            dt_log("cmd target button=%08X data=%08X x=%.4f y=%.4f",
                   button, data, ev->x, ev->y);
            dt_trace("cmd-drag", button, data);
        } else {
            /* The game updates the mode on its own tick; queue the click so
             * the pump posts it once target mode is live. */
            g_cmd_target_pending = 1;
            g_cmd_target_x = ev->x;
            g_cmd_target_y = ev->y;
            g_cmd_target_button = button;
            g_cmd_target_data = data;
            g_cmd_target_ms = dt_now_ms();
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

static int dt_command_pointer(const Wc3DirectTouchEvent *ev,
                              uint32_t width, uint32_t height)
{
    if (!ev || ev->route != WC3_DIRECT_TOUCH_ROUTE_NATIVE) return 0;

    switch (ev->kind) {
    case WC3_DIRECT_TOUCH_POINTER_DOWN: {
        if (g_cmd_active) return 1;
        uint32_t bar = 0;
        if (!dt_command_context(&bar, 0)) return 0;
        uint32_t button = 0;
        const uint32_t data = dt_command_button_at(bar, ev->x, ev->y,
                                                   width, height, &button);
        if (!data) {
#if WC3_DEBUG && WC3_DEBUG_INPUT
            dt_log("cmd-hit-none bar=%08X n=%.4f,%.4f", bar, ev->x, ev->y);
            dt_command_dump_bar(bar, width, height);
#endif
            return 0;
        }

        g_cmd_active = 1;
        g_cmd_pointer = ev->pointer_id;
        g_cmd_bar = bar;
        g_cmd_button = button;
        g_cmd_data = data;
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
            dt_native_left_click(center_x, center_y);
            dt_log("cmd select button=%08X data=%08X center=%.4f,%.4f "
                   "hle_tgt=%d",
                   button, data, center_x, center_y,
                   dt_command_targeted(data));
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
        dt_command_finish(ev, width, height);
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
#endif
    switch (ev->kind) {
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
            g_beacon_armed = 1;
            dt_trace("beacon-tap-arm", dt_bits(ev->x), dt_bits(ev->y));
            break;
        }
        if (g_beacon_armed && ev->route != WC3_DIRECT_TOUCH_ROUTE_PORTRAIT &&
            ev->route != WC3_DIRECT_TOUCH_ROUTE_MINIMAP) {
            /* A signal-button tap armed the next battlefield tap.  This is
             * the fallback for a button that the controls layer classified as
             * FIELD; the raw gesture path above normally handles it.  A tap
             * that lands on the minimap still uses the minimap mapping even
             * if the controls gate missed the frame. */
            g_beacon_armed = 0;
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
        g_beacon_armed = 0;
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
        g_beacon_armed = 0;
        if (ev->route == WC3_DIRECT_TOUCH_ROUTE_PORTRAIT) {
            dt_portrait_follow_start(c, ctx);
        }
        break;
    case WC3_DIRECT_TOUCH_HOLD_RELEASE:
        dt_portrait_release(c);
        break;
    case WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN:
        g_beacon_armed = 0;
        dt_field_camera_begin(ev);
        break;
    case WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE:
        dt_field_camera_move(ev);
        break;
    case WC3_DIRECT_TOUCH_FIELD_CAMERA_END:
        dt_field_camera_end(ev);
        break;
    case WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN:
        g_beacon_armed = 0;
        dt_field_select_begin(ev);
        break;
    case WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE:
        dt_field_select_move(ev);
        break;
    case WC3_DIRECT_TOUCH_FIELD_SELECT_END:
        dt_field_select_end(ev);
        break;
    case WC3_DIRECT_TOUCH_POINTER_DOWN:
    case WC3_DIRECT_TOUCH_POINTER_MOVE:
    case WC3_DIRECT_TOUCH_POINTER_UP:
    case WC3_DIRECT_TOUCH_POINTER_CANCEL:
        if (dt_beacon_pointer(c, ctx, ev, width, height)) {
            dt_log("consumed=beacon ptr=%u", ev->pointer_id);
            break;
        }
        if (ev->route == WC3_DIRECT_TOUCH_ROUTE_NATIVE &&
            dt_command_pointer(ev, width, height)) {
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
        dt_native_minimap_pointer(ev, width, height);
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
    __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_portrait_release_pending, 0, __ATOMIC_RELEASE);
    dt_grid_reset();
    dt_inventory_reset();
    dt_command_reset();
    dt_field_reset();
    dt_native_release();
    dt_portrait_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_minimap_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_beacon_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_command_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_publish_route(WC3_DIRECT_TOUCH_ROUTE_LEGACY);
    g_last_ui = 0;
    g_last_world = 0;
    wc3_direct_touch_clear();
    wc3_direct_touch_set_wake(dt_request_main_thread_wake, NULL);
}

void wc3_hle_direct_touch_enqueue(const Wc3DirectTouchEvent *event, void *user)
{
    (void)user;
    if (!event) return;
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
    __atomic_store_n(&g_portrait_release_pending, 1, __ATOMIC_RELEASE);
    dt_grid_reset();
    dt_inventory_reset();
    dt_command_reset();
    dt_field_reset();
    dt_native_release();
    dt_portrait_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_minimap_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_beacon_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_command_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_publish_route(WC3_DIRECT_TOUCH_ROUTE_LEGACY);
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

void wc3_hle_direct_touch_service(CPU *c, uint32_t width, uint32_t height)
{
    if (!c || !wc3_hle_direct_touch_can_service()) return;
    /* The command-card long press has no semantic event: the recognizer emits
     * only raw phases on the NATIVE route.  Time it at the pump boundary so
     * the right-click autocast toggle fires while the finger is still down. */
    dt_command_tick(width, height);
    if (__atomic_exchange_n(&g_portrait_release_pending, 0,
                            __ATOMIC_ACQ_REL)) {
        dt_portrait_release(c);
    }
    if (wc3_direct_touch_pending() == 0) {
        dt_sync_idle_context(width, height);
        return;
    }

    __atomic_store_n(&g_busy, 1, __ATOMIC_RELEASE);

    /* Pointer-only markers: dt_context() fails in menus and loading screens,
     * but a native menu click must still be serviced.  Keep the last non-zero
     * UI/world pointers; only a real replacement is a map transition. */
    uint32_t cur_ui = 0;
    uint32_t cur_world = 0;
    if (dt_guest_readable(DT_UI_GLOBAL, 4)) {
        cur_ui = wc3_rd32g(DT_UI_GLOBAL);
        cur_world = dt_world_from_ui(cur_ui);
    }
    const int context_changed =
        (g_last_ui && cur_ui && g_last_ui != cur_ui) ||
        (g_last_world && cur_world && g_last_world != cur_world);
    if (cur_ui) g_last_ui = cur_ui;
    if (cur_world) g_last_world = cur_world;

    /* The portrait, minimap, beacon and command-card frames can change with
     * a selection or a UI transition; publish them before resolving the
     * batch so the next touch sees the new UI. */
    dt_portrait_snapshot(cur_ui, width, height);
    dt_minimap_snapshot(cur_ui, width, height);
    dt_beacon_snapshot(width, height);
    dt_command_snapshot(cur_ui, width, height);
#if WC3_DEBUG && WC3_DEBUG_INPUT
    dt_trace_snapshots("service");
#endif

    if (context_changed) {
        /* Queued semantic events belong to the previous map.  Native events
         * are cleared too so a stale DOWN cannot leak a held button into the
         * new UI; the age bound below is the backstop when the pointers were
         * reused. */
        dt_trace("context-change", g_last_ui, g_last_world);
        dt_grid_reset();
        dt_inventory_reset();
        dt_command_reset();
        dt_field_reset();
        dt_native_release();
        /* A queued HOLD_RELEASE may be dropped with the batch; clear the
         * camera follow before the old context goes away. */
        dt_portrait_release(c);
        wc3_direct_touch_clear();
    }

    DtContext ctx;
    memset(&ctx, 0, sizeof ctx);
    Wc3DirectTouchRoute route = dt_ready_route(c, &ctx);
    dt_publish_service_route(route);
    /* The native press of an owned field drag can make the route read NATIVE
     * on the next pump.  Keep the drag until its END; only an idle field is
     * cancelled by a route change. */
    if (!wc3_dt_field_drag_keep(route, dt_field_drag_active())) {
        dt_field_reset();
    }

    for (uint32_t i = 0; i < DT_MAX_EVENTS_PER_PUMP; i++) {
        Wc3DirectTouchEvent event;
        if (!wc3_direct_touch_pop(&event)) break;

        /* The input thread may have queued this event just before the game
         * left the map.  The context checks catch most transitions; the age
         * bound is the backstop when a map reload reuses the same UI/world
         * objects and the guest thread was blocked through the transition. */
        const uint64_t now_ms = dt_now_ms();
        if (now_ms && event.time_ms && now_ms > event.time_ms &&
            now_ms - event.time_ms > DT_EVENT_MAX_AGE_MS) {
            dt_trace("stale-event", event.kind, event.pointer_id);
            if (dt_is_field_drag_kind(event.kind)) {
                dt_field_reset();
            } else if (dt_is_native_pointer(event.kind)) {
                dt_grid_reset();
                dt_inventory_reset();
                dt_command_reset();
                dt_native_release();
            }
            continue;
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
        } else if (dt_is_field_drag_end(event.kind)) {
            /* The END only releases a native mouse button.  It must run even
             * if the game changed context between BEGIN and END; otherwise
             * the button would stay latched. */
            dt_dispatch(c, &ctx, &event, width, height);
        } else if (dt_is_field_drag_kind(event.kind)) {
            /* Once BEGIN has pressed the native button, keep feeding MOVE to
             * that drag even if the button changed the active mode and the
             * route now reads NATIVE.  A BEGIN with no active drag still
             * requires FIELD: it is the gesture's first native press. */
            if (wc3_dt_field_drag_keep(route, dt_field_drag_active())) {
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
        } else if (route == WC3_DIRECT_TOUCH_ROUTE_NATIVE) {
            /* The gesture was classified as a field tap before the game
             * entered targeting/placement.  Let the native click handler
             * consume it instead of losing the target. */
            dt_trace("semantic-native", event.kind, event.pointer_id);
            dt_native_click(&event);
        } else {
            /* Not in game: the controls layer already routes menus as native
             * pointer phases, so a semantic event here belongs to a stale
             * map/menu transition. */
            dt_trace("semantic-drop", event.kind, event.pointer_id);
        }

        /* Re-evaluate after every event.  A semantic action can enter
         * targeting or load a map; a native click can dismiss a modal or
         * start a cinematic. */
        DtContext next_ctx;
        memset(&next_ctx, 0, sizeof next_ctx);
        route = dt_ready_route(c, &next_ctx);
        dt_publish_service_route(route);
        /* Do not cancel the drag that the event just started/continued. */
        if (!wc3_dt_field_drag_keep(route, dt_field_drag_active())) {
            dt_field_reset();
        }

        if (next_ctx.ui != ctx.ui || next_ctx.world != ctx.world) {
            dt_trace("batch-context-change", ctx.ui ^ next_ctx.ui,
                     ctx.world ^ next_ctx.world);
            if (next_ctx.ui) g_last_ui = next_ctx.ui;
            if (next_ctx.world) g_last_world = next_ctx.world;
            dt_grid_reset();
            dt_inventory_reset();
            dt_command_reset();
            dt_field_reset();
            dt_native_release();
            /* The pending HOLD_RELEASE belongs to the old context; release
             * the follow now instead of leaving it latched on the camera. */
            dt_portrait_release(c);
            wc3_direct_touch_clear();
            break;
        }
        ctx = next_ctx;
    }

    /* A native click can dismiss a modal or open a menu; refresh the frames
     * once at the end of the batch so the input thread sees the new UI. */
    uint32_t final_ui = 0;
    if (dt_guest_readable(DT_UI_GLOBAL, 4u)) {
        final_ui = wc3_rd32g(DT_UI_GLOBAL);
    }
    dt_portrait_snapshot(final_ui, width, height);
    dt_minimap_snapshot(final_ui, width, height);
    dt_beacon_snapshot(width, height);
    dt_command_snapshot(final_ui, width, height);
#if WC3_DEBUG && WC3_DEBUG_INPUT
    dt_trace_snapshots("final");
#endif

    __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
}
