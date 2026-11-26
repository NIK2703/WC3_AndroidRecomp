#include "wc3_debug.h"
#include "direct_touch.h"

#include <math.h>
#include <string.h>

#if WC3_DEBUG && WC3_DEBUG_INPUT
#include <android/log.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define DT_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "Wc3DirectTouch", __VA_ARGS__)

/*
 * Debug-only touch trace.  logcat is a ring buffer and the controls logger
 * runs on the input thread; this file survives a long menu/map session and
 * records the raw DOWN/UP plus every semantic event, so a tap that produces
 * no HLE event can still be seen.  Only opened when WC3_DATA_DIR is set.
 */
static FILE *g_touch_log;
static int g_touch_log_failed;
/* The input thread starts before wc3_app_boot_start() publishes
 * WC3_DATA_DIR, so a menu touch can arrive first.  Do not latch a
 * pthread_once on that missing variable: retry until boot sets it.  The
 * lock covers file I/O only and is never held across a guest call. */
static pthread_mutex_t g_touch_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void touch_ensure_log_locked(void)
{
    const char *dir;
    char path[1024];

    if (g_touch_log || g_touch_log_failed) return;
    dir = getenv("WC3_DATA_DIR");
    if (!dir || !*dir) return;
    snprintf(path, sizeof(path), "%s/touch_trace.txt", dir);
    g_touch_log = fopen(path, "w");
    if (!g_touch_log) {
        g_touch_log_failed = 1;
        return;
    }
    setvbuf(g_touch_log, NULL, _IOLBF, 0);
    fprintf(g_touch_log, "[touch] trace start\n");
}

static uint64_t touch_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u +
           (uint64_t)(ts.tv_nsec / 1000000);
}

void wc3_direct_touch_debug_log(const char *fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&g_touch_log_lock);
    touch_ensure_log_locked();
    if (g_touch_log) {
        va_start(ap, fmt);
        fprintf(g_touch_log, "[touch] t=%llu ",
                (unsigned long long)touch_now_ms());
        vfprintf(g_touch_log, fmt, ap);
        fputc('\n', g_touch_log);
        va_end(ap);
    }
    pthread_mutex_unlock(&g_touch_log_lock);
}

#define TOUCH_LOG(...) wc3_direct_touch_debug_log(__VA_ARGS__)

static const char *dt_route_name(uint32_t route)
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
    case WC3_DIRECT_TOUCH_FIELD_HOVER: return "FIELD_HOVER";
    default: return "?";
    }
}

static const char *dt_mode_name(uint8_t mode)
{
    switch (mode) {
    case WC3_DIRECT_TOUCH_FIELD_PENDING: return "PENDING";
    case WC3_DIRECT_TOUCH_FIELD_CAMERA: return "CAMERA";
    case WC3_DIRECT_TOUCH_FIELD_SELECT: return "SELECT";
    default: return "?";
    }
}

static const char *dt_role_name(uint8_t role)
{
    switch (role) {
    case WC3_DT_CONTACT_UNASSIGNED: return "UNASSIGNED";
    case WC3_DT_CONTACT_ACTION: return "ACTION";
    case WC3_DT_CONTACT_CAMERA: return "CAMERA";
    case WC3_DT_CONTACT_IGNORED: return "IGNORED";
    default: return "?";
    }
}
#else
#define DT_LOG(...) ((void)0)
#define TOUCH_LOG(...) ((void)0)
#endif

static float clamp01(float value)
{
    if (!(value > 0.0f)) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static void emit_event(Wc3DirectTouch *dt, Wc3DirectTouchKind kind,
                       int pointer_id, float x, float y, uint64_t now_ms,
                       uint32_t route, uint32_t owner_role,
                       uint32_t gesture_id)
{
    if (!dt->sink) return;
    Wc3DirectTouchEvent event;
    memset(&event, 0, sizeof(event));
    event.kind = (uint32_t)kind;
    event.pointer_id = (uint32_t)pointer_id;
    event.x = clamp01(dt->width > 0.0f ? x / dt->width : 0.0f);
    event.y = clamp01(dt->height > 0.0f ? y / dt->height : 0.0f);
    event.time_ms = now_ms;
    event.route = route;
    event.gesture_id = gesture_id;
    event.owner_role = owner_role;
    DT_LOG("emit kind=%s(%u) ptr=%d px=%.1f,%.1f n=%.4f,%.4f "
           "route=%s role=%u gid=%u t=%llu",
           dt_kind_name((uint32_t)kind), (unsigned)kind, pointer_id, x, y,
           event.x, event.y, dt_route_name(route), owner_role, gesture_id,
           (unsigned long long)now_ms);
    /* Keep the file focused on contacts; the HLE trace already records the
     * move stream, and a camera drag can otherwise produce thousands of
     * lines. */
    if (kind != WC3_DIRECT_TOUCH_POINTER_MOVE) {
        TOUCH_LOG("emit kind=%s(%u) ptr=%d px=%.1f,%.1f n=%.4f,%.4f "
                  "route=%s role=%u gid=%u event_t=%llu",
                  dt_kind_name((uint32_t)kind), (unsigned)kind, pointer_id,
                  x, y, event.x, event.y, dt_route_name(route), owner_role,
                  gesture_id, (unsigned long long)now_ms);
    }
    dt->sink(&event, dt->user);
}

/* Emit from a classified pointer.  Hover is forced to ACTION even before the
 * contact has a role; every other semantic event follows the pointer's role. */
static void emit_pointer_event(Wc3DirectTouch *dt, Wc3DirectTouchKind kind,
                               const Wc3DirectTouchPointer *p, float x,
                               float y, uint64_t now_ms)
{
    const int is_camera = p->role == WC3_DT_CONTACT_CAMERA;
    emit_event(dt, kind, p->pointer_id, x, y, now_ms, p->route,
               is_camera ? WC3_DT_OWNER_CAMERA : WC3_DT_OWNER_ACTION,
               is_camera ? p->gesture_id : 0u);
}

static void emit_action_hover(Wc3DirectTouch *dt,
                              const Wc3DirectTouchPointer *p,
                              float x, float y, uint64_t now_ms)
{
    emit_event(dt, WC3_DIRECT_TOUCH_FIELD_HOVER, p->pointer_id, x, y, now_ms,
               p->route, WC3_DT_OWNER_ACTION, 0u);
}

static float distance_sq(float ax, float ay, float bx, float by)
{
    const float dx = ax - bx;
    const float dy = ay - by;
    return dx * dx + dy * dy;
}

static float drag_threshold_sq(const Wc3DirectTouch *dt)
{
    const float threshold = dt->drag_threshold_px > 0.0f
        ? dt->drag_threshold_px : 1.0f;
    return threshold * threshold;
}

static void fire_haptic(Wc3DirectTouch *dt)
{
    if (dt->haptic && dt->haptic_effect > 0)
        dt->haptic(dt->haptic_effect, dt->haptic_user);
}

/* Only a field pointer can become a native selection press.  The deadline is
 * checked in both tick() and move(): a batch can deliver a MOVE after the
 * deadline but before the next tick, and that move must become a selection
 * move, not a camera swipe. */
static int hold_due(const Wc3DirectTouch *dt,
                    const Wc3DirectTouchPointer *pointer, uint64_t now_ms)
{
    if (pointer->route != WC3_DIRECT_TOUCH_ROUTE_FIELD ||
        pointer->long_fired ||
        pointer->mode != WC3_DIRECT_TOUCH_FIELD_PENDING ||
        pointer->role == WC3_DT_CONTACT_CAMERA ||
        pointer->role == WC3_DT_CONTACT_IGNORED) {
        return 0;
    }
    const uint64_t timeout = dt->long_press_ms > 0
        ? (uint64_t)dt->long_press_ms : 0u;
    return now_ms >= pointer->down_ms &&
           now_ms - pointer->down_ms >= timeout;
}

static void arm_hold(Wc3DirectTouch *dt, Wc3DirectTouchPointer *pointer,
                     float x, float y, uint64_t now_ms)
{
    pointer->long_fired = 1;
    pointer->mode = WC3_DIRECT_TOUCH_FIELD_SELECT;
    pointer->role = WC3_DT_CONTACT_ACTION;
    pointer->camera_claim = 0;
    /* The legacy engine plays gestureLongPressHaptic exactly when the
     * long-press timer expires, before any drag. */
    fire_haptic(dt);
    /* The long press is the native left-button press-and-hold.  Emit the
     * BEGIN at the deadline so the executor posts WM_LBUTTONDOWN and keeps
     * it down; moves in between are the native selection rectangle and the
     * release posts the matching UP.  A stationary release is therefore a
     * native click that selects one unit. */
    DT_LOG("hold ptr=%d -> SELECT_BEGIN", pointer->pointer_id);
    TOUCH_LOG("hold ptr=%d -> SELECT_BEGIN n=%.4f,%.4f event_t=%llu",
              pointer->pointer_id,
              dt->width > 0.0f ? x / dt->width : 0.0f,
              dt->height > 0.0f ? y / dt->height : 0.0f,
              (unsigned long long)now_ms);
    /* The action finger owns the cursor: hover first so a target-mode click
     * resolves against this contact's point.  The service keeps the click in
     * a later pump. */
    emit_action_hover(dt, pointer, x, y, now_ms);
    emit_pointer_event(dt, WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN, pointer,
                       x, y, now_ms);
}

/* Claim the camera role for a FIELD contact that crossed the drag threshold.
 * No hover is emitted: the camera drives the native drag manager directly
 * and must not move the shared virtual cursor. */
static void claim_camera(Wc3DirectTouch *dt, Wc3DirectTouchPointer *pointer,
                         float x, float y, uint64_t now_ms)
{
    pointer->role = WC3_DT_CONTACT_CAMERA;
    pointer->camera_claim = 0;
    pointer->mode = WC3_DIRECT_TOUCH_FIELD_CAMERA;
    pointer->moved = 1;
    dt->g_camera_seq++;
    pointer->gesture_id = dt->g_camera_seq;
    DT_LOG("move ptr=%d PENDING -> CAMERA gid=%u", pointer->pointer_id,
           pointer->gesture_id);
    emit_pointer_event(dt, WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN, pointer,
                       x, y, now_ms);
}

static int active_count(const Wc3DirectTouch *dt)
{
    int count = 0;
    for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
        if (dt->pointers[i].active) count++;
    }
    return count;
}

static Wc3DirectTouchPointer *find_pointer(Wc3DirectTouch *dt, int pointer_id)
{
    for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
        if (dt->pointers[i].active &&
            dt->pointers[i].pointer_id == pointer_id) {
            return &dt->pointers[i];
        }
    }
    return NULL;
}

static Wc3DirectTouchPointer *alloc_pointer(Wc3DirectTouch *dt)
{
    for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
        if (!dt->pointers[i].active) return &dt->pointers[i];
    }
    return NULL;
}

void wc3_direct_touch_init(Wc3DirectTouch *dt, float width, float height,
                           float drag_threshold_px, int long_press_ms,
                           Wc3DirectTouchSink sink, void *user)
{
    if (!dt) return;
    memset(dt, 0, sizeof(*dt));
    dt->width = width;
    dt->height = height;
    dt->drag_threshold_px = drag_threshold_px > 0.0f
        ? drag_threshold_px : 1.0f;
    dt->long_press_ms = long_press_ms;
    dt->sink = sink;
    dt->user = user;
}

void wc3_direct_touch_set_haptic(Wc3DirectTouch *dt,
                                 Wc3DirectTouchHaptic haptic, void *user,
                                 int effect)
{
    if (!dt) return;
    dt->haptic = haptic;
    dt->haptic_user = user;
    dt->haptic_effect = effect;
}

void wc3_direct_touch_set_sink(Wc3DirectTouch *dt, Wc3DirectTouchSink sink,
                               void *user)
{
    if (!dt) return;
    dt->sink = sink;
    dt->user = user;
}

void wc3_direct_touch_set_size(Wc3DirectTouch *dt, float width, float height)
{
    if (!dt) return;
    dt->width = width;
    dt->height = height;
}

void wc3_direct_touch_down_route(Wc3DirectTouch *dt, int pointer_id,
                                 float x, float y, uint64_t now_ms,
                                 uint32_t route)
{
    if (!dt) return;

    DT_LOG("down ptr=%d px=%.1f,%.1f route=%s active_before=%d",
           pointer_id, x, y, dt_route_name(route), active_count(dt));
    TOUCH_LOG("down ptr=%d px=%.1f,%.1f n=%.4f,%.4f route=%s "
              "active_before=%d event_t=%llu",
              pointer_id, x, y,
              dt->width > 0.0f ? x / dt->width : 0.0f,
              dt->height > 0.0f ? y / dt->height : 0.0f,
              dt_route_name(route), active_count(dt),
              (unsigned long long)now_ms);

    Wc3DirectTouchPointer *pointer = find_pointer(dt, pointer_id);
    if (pointer) {
        /* A duplicate DOWN for the same id starts a new contact. */
        memset(pointer, 0, sizeof(*pointer));
    } else {
        pointer = alloc_pointer(dt);
    }
    if (!pointer) {
        DT_LOG("down ptr=%d no-free-slot", pointer_id);
        return;
    }

    const int others = active_count(dt);
    if (others > 0) {
        for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
            if (dt->pointers[i].active) dt->pointers[i].multi = 1;
        }
    }

    /* Every FIELD contact is a camera candidate.  A second finger may land
     * while another camera drag is already active: it can still tap or hold,
     * and a quick swipe of its own must join the camera pan. */
    const int can_claim_camera = route == WC3_DIRECT_TOUCH_ROUTE_FIELD;
    const int too_many = others >= WC3_DIRECT_TOUCH_MAX_POINTERS;

    pointer->pointer_id = pointer_id;
    pointer->down_x = x;
    pointer->down_y = y;
    pointer->last_x = x;
    pointer->last_y = y;
    pointer->down_ms = now_ms;
    pointer->route = route;
    pointer->active = 1;
    pointer->multi = others > 0 ? 1 : 0;
    pointer->mode = WC3_DIRECT_TOUCH_FIELD_PENDING;
    pointer->role = too_many ? WC3_DT_CONTACT_IGNORED
                             : (can_claim_camera ? WC3_DT_CONTACT_UNASSIGNED
                                                 : WC3_DT_CONTACT_ACTION);
    pointer->camera_claim = (uint8_t)(can_claim_camera && !too_many);
    DT_LOG("down-state ptr=%d mode=%s role=%s claim=%d multi=%d others=%d "
           "down_ms=%llu",
           pointer_id, dt_mode_name(pointer->mode),
           dt_role_name(pointer->role), pointer->camera_claim,
           pointer->multi, others, (unsigned long long)now_ms);
    /* No hover on DOWN: the camera must not move the shared cursor, and the
     * action hover is emitted immediately before its click. */
    if (route == WC3_DIRECT_TOUCH_ROUTE_PORTRAIT) {
        /* The portrait is a tracking control, not a long-press gesture.
         * Start the native camera follow as soon as the finger lands on it;
         * the follow stays active until this pointer is released, even when
         * the finger moves outside the portrait. */
        pointer->long_fired = 1;
        emit_pointer_event(dt, WC3_DIRECT_TOUCH_FOLLOW_START, pointer,
                           x, y, now_ms);
    }
}

void wc3_direct_touch_down(Wc3DirectTouch *dt, int pointer_id,
                           float x, float y, uint64_t now_ms)
{
    wc3_direct_touch_down_route(dt, pointer_id, x, y, now_ms,
                                WC3_DIRECT_TOUCH_ROUTE_FIELD);
}

void wc3_direct_touch_move(Wc3DirectTouch *dt, int pointer_id,
                           float x, float y, uint64_t now_ms)
{
    if (!dt) return;
    Wc3DirectTouchPointer *pointer = find_pointer(dt, pointer_id);
    if (!pointer) {
        DT_LOG("move ptr=%d no-pointer", pointer_id);
        return;
    }

    pointer->last_x = x;
    pointer->last_y = y;
    if (pointer->route == WC3_DIRECT_TOUCH_ROUTE_PORTRAIT) {
        /* The portrait follow is owned by the pointer, not by the rectangle.
         * Moving anywhere on the screen must not cancel it. */
        DT_LOG("move ptr=%d px=%.1f,%.1f route=PORTRAIT ignored",
               pointer_id, x, y);
        return;
    }
    /* NATIVE and MINIMAP pointers are forwarded as raw phases by controls.c;
     * they must never enter the field state machine or emit semantic events.
     * Keep last_x/last_y updated above for a reset/handoff. */
    if (pointer->route != WC3_DIRECT_TOUCH_ROUTE_FIELD) {
        DT_LOG("move ptr=%d px=%.1f,%.1f route=%s ignored",
               pointer_id, x, y, dt_route_name(pointer->route));
        return;
    }
    if (pointer->role == WC3_DT_CONTACT_IGNORED) {
        DT_LOG("move ptr=%d px=%.1f,%.1f role=IGNORED", pointer_id, x, y);
        return;
    }

    /* A MOVE can arrive in the same batch as the deadline but before
     * controls_tick().  Arm the hold here so the gesture below is classified
     * as a selection drag, not as a camera swipe. */
    if (hold_due(dt, pointer, now_ms)) {
        DT_LOG("move ptr=%d hold-due -> arm", pointer_id);
        arm_hold(dt, pointer, x, y, now_ms);
    }

    const float moved_sq =
        distance_sq(x, y, pointer->down_x, pointer->down_y);
    DT_LOG("move ptr=%d px=%.1f,%.1f mode=%s role=%s claim=%d moved_sq=%.1f "
           "threshold_sq=%.1f route=%s",
           pointer_id, x, y, dt_mode_name(pointer->mode),
           dt_role_name(pointer->role), pointer->camera_claim, moved_sq,
           drag_threshold_sq(dt), dt_route_name(pointer->route));
    switch (pointer->mode) {
    case WC3_DIRECT_TOUCH_FIELD_CAMERA:
        emit_pointer_event(dt, WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE, pointer,
                           x, y, now_ms);
        break;
    case WC3_DIRECT_TOUCH_FIELD_SELECT:
        emit_pointer_event(dt, WC3_DIRECT_TOUCH_FIELD_SELECT_MOVE, pointer,
                           x, y, now_ms);
        break;
    case WC3_DIRECT_TOUCH_FIELD_PENDING:
    default:
        if (pointer->role == WC3_DT_CONTACT_UNASSIGNED &&
            pointer->camera_claim &&
            moved_sq > drag_threshold_sq(dt)) {
            claim_camera(dt, pointer, x, y, now_ms);
        }
        /* An ACTION contact that moved before the long-press deadline is a
         * quick flick, not an order.  Wait for the deadline or the release. */
        break;
    }
}

void wc3_direct_touch_up(Wc3DirectTouch *dt, int pointer_id,
                         float x, float y, uint64_t now_ms)
{
    if (!dt) return;
    Wc3DirectTouchPointer *pointer = find_pointer(dt, pointer_id);
    if (!pointer) {
        DT_LOG("up ptr=%d no-pointer", pointer_id);
        return;
    }

    pointer->last_x = x;
    pointer->last_y = y;
    DT_LOG("up ptr=%d px=%.1f,%.1f route=%s mode=%s multi=%d "
           "long_fired=%d",
           pointer_id, x, y, dt_route_name(pointer->route),
           dt_mode_name(pointer->mode), pointer->multi,
           pointer->long_fired);

    if (pointer->route == WC3_DIRECT_TOUCH_ROUTE_PORTRAIT) {
        /* The follow started on DOWN and is owned by this finger.  Lifting
         * it stops the follow unconditionally; the portrait has no separate
         * tap or long-press action. */
        emit_pointer_event(dt, WC3_DIRECT_TOUCH_HOLD_RELEASE, pointer,
                           x, y, now_ms);
        memset(pointer, 0, sizeof(*pointer));
        return;
    }
    if (pointer->route != WC3_DIRECT_TOUCH_ROUTE_FIELD) {
        /* Raw NATIVE/MINIMAP ownership ends here; controls.c already sent
         * the matching raw UP.  Never turn it into TAP or a field selection. */
        DT_LOG("up ptr=%d route=%s raw -> no semantic", pointer_id,
               dt_route_name(pointer->route));
        memset(pointer, 0, sizeof(*pointer));
        return;
    }

    const float moved_sq = distance_sq(x, y, pointer->down_x, pointer->down_y);
    const uint64_t timeout = dt->long_press_ms > 0
        ? (uint64_t)dt->long_press_ms : 0u;
    const int held_long = now_ms >= pointer->down_ms &&
                          now_ms - pointer->down_ms >= timeout;
    DT_LOG("up-state ptr=%d moved_sq=%.1f threshold_sq=%.1f held_long=%d",
           pointer_id, moved_sq, drag_threshold_sq(dt), held_long);
    TOUCH_LOG("up ptr=%d px=%.1f,%.1f n=%.4f,%.4f route=%s mode=%s "
              "multi=%d long_fired=%d moved_sq=%.1f threshold_sq=%.1f "
              "held_long=%d event_t=%llu",
              pointer_id, x, y,
              dt->width > 0.0f ? x / dt->width : 0.0f,
              dt->height > 0.0f ? y / dt->height : 0.0f,
              dt_route_name(pointer->route), dt_mode_name(pointer->mode),
              pointer->multi, pointer->long_fired, moved_sq,
              drag_threshold_sq(dt), held_long,
              (unsigned long long)now_ms);

    switch (pointer->mode) {
    case WC3_DIRECT_TOUCH_FIELD_CAMERA:
        DT_LOG("up ptr=%d camera end gid=%u", pointer_id,
               pointer->gesture_id);
        emit_pointer_event(dt, WC3_DIRECT_TOUCH_FIELD_CAMERA_END, pointer,
                           x, y, now_ms);
        memset(pointer, 0, sizeof(*pointer));
        return;
    case WC3_DIRECT_TOUCH_FIELD_SELECT:
        DT_LOG("up ptr=%d select end", pointer_id);
        emit_pointer_event(dt, WC3_DIRECT_TOUCH_FIELD_SELECT_END, pointer,
                           x, y, now_ms);
        memset(pointer, 0, sizeof(*pointer));
        return;
    case WC3_DIRECT_TOUCH_FIELD_PENDING:
    default:
        if (pointer->role == WC3_DT_CONTACT_IGNORED) {
            DT_LOG("up ptr=%d IGNORED", pointer_id);
        } else if (held_long) {
            /* The tick was late.  The finger is already up, so press
             * and release the native left button in one step instead of
             * degrading into a tap or leaving the button latched. */
            DT_LOG("up ptr=%d late-hold -> SELECT_BEGIN/END", pointer_id);
            arm_hold(dt, pointer, x, y, now_ms);
            emit_pointer_event(dt, WC3_DIRECT_TOUCH_FIELD_SELECT_END,
                               pointer, x, y, now_ms);
        } else if (moved_sq <= drag_threshold_sq(dt)) {
            /* A flick can arrive as DOWN+UP with no MOVE: the release
             * point must still be inside the tap slop, otherwise the tap
             * is dropped. */
            DT_LOG("up ptr=%d PENDING -> TAP", pointer_id);
            emit_action_hover(dt, pointer, x, y, now_ms);
            emit_pointer_event(dt, WC3_DIRECT_TOUCH_TAP, pointer,
                               x, y, now_ms);
        } else {
            DT_LOG("up ptr=%d PENDING -> no event moved_sq=%.1f", pointer_id,
                   moved_sq);
        }
        memset(pointer, 0, sizeof(*pointer));
        return;
    }
}

void wc3_direct_touch_reset(Wc3DirectTouch *dt)
{
    if (!dt) return;
    for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
        Wc3DirectTouchPointer *pointer = &dt->pointers[i];
        if (!pointer->active) continue;
        DT_LOG("reset ptr=%d route=%s mode=%s", pointer->pointer_id,
               dt_route_name(pointer->route), dt_mode_name(pointer->mode));
        TOUCH_LOG("reset ptr=%d route=%s mode=%s", pointer->pointer_id,
                  dt_route_name(pointer->route),
                  dt_mode_name(pointer->mode));
        if (pointer->route == WC3_DIRECT_TOUCH_ROUTE_PORTRAIT) {
            emit_pointer_event(dt, WC3_DIRECT_TOUCH_HOLD_RELEASE, pointer,
                               pointer->last_x, pointer->last_y, 0);
        } else if (pointer->role == WC3_DT_CONTACT_CAMERA) {
            emit_pointer_event(dt, WC3_DIRECT_TOUCH_FIELD_CAMERA_END, pointer,
                               pointer->last_x, pointer->last_y, 0);
        } else if (pointer->mode == WC3_DIRECT_TOUCH_FIELD_SELECT) {
            emit_pointer_event(dt, WC3_DIRECT_TOUCH_FIELD_SELECT_END, pointer,
                               pointer->last_x, pointer->last_y, 0);
        }
    }
    memset(dt->pointers, 0, sizeof(dt->pointers));
}

int wc3_direct_touch_tick(Wc3DirectTouch *dt, uint64_t now_ms)
{
    if (!dt) return 0;
    int armed = 0;
    for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
        Wc3DirectTouchPointer *pointer = &dt->pointers[i];
        if (!pointer->active || !hold_due(dt, pointer, now_ms)) continue;
        DT_LOG("tick ptr=%d hold-due -> arm", pointer->pointer_id);
        arm_hold(dt, pointer, pointer->last_x, pointer->last_y, now_ms);
        armed++;
    }
    return armed;
}

uint64_t wc3_direct_touch_next_deadline_ms(const Wc3DirectTouch *dt)
{
    if (!dt) return 0;
    uint64_t next = 0;
    for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
        const Wc3DirectTouchPointer *pointer = &dt->pointers[i];
        if (!pointer->active || pointer->long_fired ||
            pointer->route != WC3_DIRECT_TOUCH_ROUTE_FIELD ||
            pointer->mode != WC3_DIRECT_TOUCH_FIELD_PENDING ||
            pointer->role == WC3_DT_CONTACT_CAMERA ||
            pointer->role == WC3_DT_CONTACT_IGNORED) {
            continue;
        }
        const uint64_t timeout = dt->long_press_ms > 0
            ? (uint64_t)dt->long_press_ms : 0u;
        uint64_t deadline = pointer->down_ms + timeout;
        if (deadline < pointer->down_ms) deadline = UINT64_MAX;
        if (next == 0 || deadline < next) next = deadline;
    }
    return next;
}

int wc3_direct_touch_field_drag_active(const Wc3DirectTouch *dt)
{
    if (!dt) return 0;
    for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
        const Wc3DirectTouchPointer *pointer = &dt->pointers[i];
        if (!pointer->active) continue;
        /* Only the left-button selection drag owns a mouse button.  A camera
         * drag uses no cursor, so a second finger must stay actionable. */
        if (pointer->mode == WC3_DIRECT_TOUCH_FIELD_SELECT) return 1;
    }
    return 0;
}

int wc3_direct_touch_active_count(const Wc3DirectTouch *dt)
{
    return dt ? active_count(dt) : 0;
}

int wc3_direct_touch_pointer_at(const Wc3DirectTouch *dt, int index,
                                int *pointer_id, float *x, float *y)
{
    if (!dt || index < 0) return 0;
    for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
        const Wc3DirectTouchPointer *pointer = &dt->pointers[i];
        if (!pointer->active) continue;
        if (index-- != 0) continue;
        if (pointer_id) *pointer_id = pointer->pointer_id;
        if (x) *x = pointer->last_x;
        if (y) *y = pointer->last_y;
        return 1;
    }
    return 0;
}

void wc3_direct_touch_emit_pointer(Wc3DirectTouch *dt,
                                   Wc3DirectTouchKind kind, int pointer_id,
                                   float x, float y, uint64_t now_ms)
{
    if (!dt) return;
    switch (kind) {
    case WC3_DIRECT_TOUCH_POINTER_DOWN:
    case WC3_DIRECT_TOUCH_POINTER_MOVE:
    case WC3_DIRECT_TOUCH_POINTER_UP:
    case WC3_DIRECT_TOUCH_POINTER_CANCEL:
        emit_event(dt, kind, pointer_id, x, y, now_ms,
                   WC3_DIRECT_TOUCH_ROUTE_NATIVE, WC3_DT_OWNER_ACTION, 0u);
        break;
    default:
        break;
    }
}

int wc3_direct_touch_camera_pointer(const Wc3DirectTouch *dt)
{
    if (!dt) return -1;
    for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
        const Wc3DirectTouchPointer *p = &dt->pointers[i];
        if (p->active && p->role == WC3_DT_CONTACT_CAMERA)
            return p->pointer_id;
    }
    return -1;
}

int wc3_direct_touch_action_pointer(const Wc3DirectTouch *dt)
{
    if (!dt) return -1;
    for (int i = 0; i < WC3_DIRECT_TOUCH_MAX_POINTERS; i++) {
        const Wc3DirectTouchPointer *p = &dt->pointers[i];
        if (p->active && p->role == WC3_DT_CONTACT_ACTION)
            return p->pointer_id;
    }
    return -1;
}

uint32_t wc3_direct_touch_contact_role(const Wc3DirectTouch *dt, int pointer_id)
{
    if (!dt) return WC3_DT_CONTACT_IGNORED;
    const Wc3DirectTouchPointer *p = find_pointer((Wc3DirectTouch *)dt,
                                                  pointer_id);
    return p ? p->role : (uint32_t)WC3_DT_CONTACT_IGNORED;
}

uint32_t wc3_direct_touch_gesture_id(const Wc3DirectTouch *dt, int pointer_id)
{
    if (!dt) return 0u;
    const Wc3DirectTouchPointer *p = find_pointer((Wc3DirectTouch *)dt,
                                                  pointer_id);
    return p && p->role == WC3_DT_CONTACT_CAMERA ? p->gesture_id : 0u;
}
