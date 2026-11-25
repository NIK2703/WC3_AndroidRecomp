#include "controls.h"

#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wc3_debug.h"

#if WC3_DEBUG && WC3_DEBUG_INPUT
#include <android/log.h>
#define WC3_CTRL_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "Wc3Controls", __VA_ARGS__)
#else
#define WC3_CTRL_LOG(...) ((void)0)
#endif

#include "core/element_helpers.h"
#include "core/processor.h"
#include "core/state.h"
#include "profile/bind.h"
#include "profile/convert.h"
#include "profile/loader.h"
#include "direct_touch.h"
#include "overlay_mode.h"
#include "profile_mode.h"

/*
 * The vendored engine expects these three symbols.  g_current_finger_idx is
 * intentionally not defined here: core/touch_input.c owns it.
 */
pthread_mutex_t g_state_mutex __attribute__((aligned(64)))
    = PTHREAD_MUTEX_INITIALIZER;
int g_timer_interval_ms = 16;

void udp_mouse_send(int flags, int dx, int dy, int wheel) {
    (void)flags;
    (void)dx;
    (void)dy;
    (void)wheel;
}

static Wc3ActionSink g_sink;
static void *g_sink_user;
static char *g_profile_json;
static ProfileData *g_profile_full;
static ProfileData *g_profile_simple;
static ProfileData *g_profile_simple_esc;
static Wc3OverlayMode g_overlay_mode = WC3_OVERLAY_MODE_FULL;
static uint32_t g_layout_id;
static int g_simple_build_rc;
static int g_simple_esc_build_rc;
static int g_width;
static int g_height;
static uint64_t g_last_engine_tick;
static bool g_initialized;
static bool g_double_tap_enabled = true;
static int g_double_tap_timeout_ms;
static int g_long_press_timeout_ms;
static int g_tap_click_delay_ms;
/*
 * Product opacity of the control overlay, in percent of the alpha channel.
 * 0 means "keep the authored profile opacity".
 */
static int g_opacity_percent;
static char g_error[256];
static bool g_key_down[256];
static bool g_button_down[8];

/*
 * Direct-touch routing owns a pointer from DOWN to UP.  A touch that starts
 * on an overlay element, outside the phase-1 field viewport, or while the
 * feature is disabled stays on the vendored path.  The ownership table is
 * touched only by the Android input thread.
 */
#define DIRECT_OWNER_SLOTS 16
#define DIRECT_OWNER_NONE 0
#define DIRECT_OWNER_VENDOR 1
#define DIRECT_OWNER_FIELD 2
/* Raw pointer phases for native UI: command card, resource bar, menus,
 * targeting/placement.  The minimap has its own owner so its pointer can be
 * clamped to the minimap rectangle for the whole drag.  The portrait has its
 * own owner because it uses the portrait-follow events. */
#define DIRECT_OWNER_NATIVE 3
/* A second pointer that arrived while a native gesture was active.  It is
 * swallowed instead of being handed to the overlay engine: the native HUD
 * gesture is single-pointer, and mixing it with the vendor path would make
 * the same touch produce two different actions. */
#define DIRECT_OWNER_IGNORED 4
/* A gesture inside the visible portrait frame.  It starts the native camera
 * follow immediately and keeps it until the owning finger is released. */
#define DIRECT_OWNER_PORTRAIT 5
/* A gesture that started inside the visible minimap frame.  Its native
 * pointer phases stay clamped to the minimap until the finger is released,
 * even when the finger moves across the rest of the screen. */
#define DIRECT_OWNER_MINIMAP 6

typedef struct {
    int pointer_id;
    int owner;
    float last_x;
    float last_y;
} DirectOwner;

static bool g_direct_touch_enabled;
static Wc3DirectTouchSink g_direct_touch_sink;
static void *g_direct_touch_user;
static Wc3DirectTouchRouteGate g_direct_touch_route_gate;
static void *g_direct_touch_route_gate_user;
static Wc3DirectTouchPortraitRectGate g_direct_touch_portrait_gate;
static void *g_direct_touch_portrait_gate_user;
static Wc3DirectTouchMinimapRectGate g_direct_touch_minimap_gate;
static void *g_direct_touch_minimap_gate_user;
static Wc3DirectTouchBeaconRectGate g_direct_touch_beacon_gate;
static void *g_direct_touch_beacon_gate_user;
static Wc3DirectTouchCommandRectGate g_direct_touch_command_gate;
static void *g_direct_touch_command_gate_user;
static Wc3DirectTouchHapticSink g_direct_touch_haptic_sink;
static void *g_direct_touch_haptic_user;
/* Legacy profile default; direct_touch_configure() replaces it with the
 * authored gestureLongPressHaptic value. */
static int g_direct_touch_haptic_effect = 3;
static Wc3DirectTouch g_direct_touch;
static DirectOwner g_direct_owners[DIRECT_OWNER_SLOTS];

#if WC3_DEBUG && WC3_DEBUG_INPUT
static const char *direct_route_name(Wc3DirectTouchRoute route) {
    switch (route) {
    case WC3_DIRECT_TOUCH_ROUTE_LEGACY: return "LEGACY";
    case WC3_DIRECT_TOUCH_ROUTE_FIELD: return "FIELD";
    case WC3_DIRECT_TOUCH_ROUTE_NATIVE: return "NATIVE";
    case WC3_DIRECT_TOUCH_ROUTE_PORTRAIT: return "PORTRAIT";
    case WC3_DIRECT_TOUCH_ROUTE_MINIMAP: return "MINIMAP";
    default: return "?";
    }
}

static const char *direct_owner_name(int owner) {
    switch (owner) {
    case DIRECT_OWNER_NONE: return "NONE";
    case DIRECT_OWNER_VENDOR: return "VENDOR";
    case DIRECT_OWNER_FIELD: return "FIELD";
    case DIRECT_OWNER_NATIVE: return "NATIVE";
    case DIRECT_OWNER_IGNORED: return "IGNORED";
    case DIRECT_OWNER_PORTRAIT: return "PORTRAIT";
    case DIRECT_OWNER_MINIMAP: return "MINIMAP";
    default: return "?";
    }
}

static void direct_touch_trace_gate(const char *name,
                                    Wc3DirectTouchRectGate gate,
                                    void *user) {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    if (gate && gate(&left, &top, &right, &bottom, user)) {
        WC3_CTRL_LOG("dt %s=%.4f,%.4f..%.4f,%.4f",
                     name, left, top, right, bottom);
    } else {
        WC3_CTRL_LOG("dt %s=<none>", name);
    }
}

#define DT_ROUTE_RETURN(route, reason) \
    do { \
        WC3_CTRL_LOG("dt route-out => %s (%s)", \
                     direct_route_name(route), (reason)); \
        wc3_direct_touch_debug_log("gate => %s (%s)", \
                                   direct_route_name(route), (reason)); \
        return (route); \
    } while (0)
#else
#define DT_ROUTE_RETURN(route, reason) return (route)
#endif

static void direct_touch_configure(int width, int height,
                                   const ProfileData *profile);

static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, ap);
    va_end(ap);
    g_error[sizeof(g_error) - 1] = '\0';
}

static void clear_error(void) {
    g_error[0] = '\0';
}

static void reset_action_filter(void) {
    memset(g_key_down, 0, sizeof(g_key_down));
    memset(g_button_down, 0, sizeof(g_button_down));
}

/*
 * Touch taps click on finger-down.  The engine emits the cursor move before
 * the button, but both actions are normally delivered in one input drain; the
 * guest can process the click while the cursor is still at its old position.
 * In the SIMPLE overlays keep a configurable wall-clock gap between the move
 * and the LMB press.  This is not a timer tick: the tick engine still owns
 * every other delay.
 */
#define WC3_TAP_CLICK_DELAY_DEFAULT_MS 10
#define WC3_TAP_CLICK_DELAY_MIN_MS 8
#define WC3_TAP_CLICK_DELAY_MAX_MS 40

static int clamp_tap_click_delay_ms(int value) {
    if (value <= 0) return WC3_TAP_CLICK_DELAY_DEFAULT_MS;
    if (value < WC3_TAP_CLICK_DELAY_MIN_MS) return WC3_TAP_CLICK_DELAY_MIN_MS;
    if (value > WC3_TAP_CLICK_DELAY_MAX_MS) return WC3_TAP_CLICK_DELAY_MAX_MS;
    return value;
}

/* The settings screen exposes the same 10..50 range; the native clamp is the
 * product boundary and does not trust the caller. */
#define WC3_OVERLAY_OPACITY_MIN_PERCENT 10
#define WC3_OVERLAY_OPACITY_MAX_PERCENT 50

static int clamp_opacity_percent(int value) {
    if (value <= 0) return 0;
    if (value < WC3_OVERLAY_OPACITY_MIN_PERCENT)
        return WC3_OVERLAY_OPACITY_MIN_PERCENT;
    if (value > WC3_OVERLAY_OPACITY_MAX_PERCENT)
        return WC3_OVERLAY_OPACITY_MAX_PERCENT;
    return value;
}

void controls_set_opacity_percent(int percent) {
    g_opacity_percent = clamp_opacity_percent(percent);
}

int controls_opacity_percent(void) {
    return g_opacity_percent;
}

/* A move delivered by the engine, but not yet followed by an LMB press. */
static bool g_pointer_move_pending;

/*
 * The vendored engine can release the same binding twice, and can schedule
 * two presses of the same binding without an intervening release (second
 * finger double tap).  The host contract is one live press per binding, so
 * drop a press while already down and a release with no live press.  This is
 * a boundary filter only; the engine's gesture state is untouched.
 */
static void emit_actions(const TouchActionResult *result) {
    if (!g_sink || !result || result->count <= 0) return;

    TouchAction filtered[MAX_TOUCH_RESULT_ACTIONS];
    int filtered_count = 0;

    for (int i = 0; i < (int)result->count; i++) {
        const TouchAction *action = &result->actions[i];
        bool keep = true;

        switch (action->type) {
            case ACT_KEY_PRESS: {
                int keycode = action->key.keycode;
                if (keycode >= 0 && keycode < (int)(sizeof(g_key_down) /
                                                    sizeof(g_key_down[0]))) {
                    if (g_key_down[keycode]) {
                        keep = false;
                    } else {
                        g_key_down[keycode] = true;
                    }
                }
                break;
            }
            case ACT_KEY_RELEASE: {
                int keycode = action->key.keycode;
                if (keycode >= 0 && keycode < (int)(sizeof(g_key_down) /
                                                    sizeof(g_key_down[0]))) {
                    if (!g_key_down[keycode]) {
                        keep = false;
                    } else {
                        g_key_down[keycode] = false;
                    }
                }
                break;
            }
            case ACT_POINTER_BUTTON_PRESS: {
                int button = action->pointer_button.button;
                if (button >= 0 && button < (int)(sizeof(g_button_down) /
                                                  sizeof(g_button_down[0]))) {
                    if (g_button_down[button]) {
                        keep = false;
                    } else {
                        g_button_down[button] = true;
                    }
                }
                break;
            }
            case ACT_POINTER_BUTTON_RELEASE: {
                int button = action->pointer_button.button;
                if (button >= 0 && button < (int)(sizeof(g_button_down) /
                                                  sizeof(g_button_down[0]))) {
                    if (!g_button_down[button]) {
                        keep = false;
                    } else {
                        g_button_down[button] = false;
                    }
                }
                break;
            }
            default:
                break;
        }

        if (keep && filtered_count < MAX_TOUCH_RESULT_ACTIONS) {
            filtered[filtered_count++] = *action;
        }
    }

    if (filtered_count <= 0) return;

    int press = -1;
    bool move_before_press = false;
    for (int i = 0; i < filtered_count; i++) {
        if (filtered[i].type == ACT_POINTER_MOVE ||
            filtered[i].type == ACT_POINTER_MOVE_DELTA)
            move_before_press = true;
        if (filtered[i].type == ACT_POINTER_BUTTON_PRESS &&
            filtered[i].pointer_button.button == 0) {
            press = i;
            break;
        }
    }

    if (press >= 0 && (g_pointer_move_pending || move_before_press) &&
        wc3_overlay_mode_is_simple(g_overlay_mode)) {
        /* Deliver the pointer move first, then let the guest observe the new
         * cursor position before the click is posted.  The delay is a
         * product setting for the simplified overlay only. */
        if (press > 0)
            g_sink(filtered, press, g_sink_user);
        usleep((useconds_t)g_tap_click_delay_ms * 1000u);
        g_pointer_move_pending = false;
        g_sink(filtered + press, filtered_count - press, g_sink_user);
        return;
    }

    g_sink(filtered, filtered_count, g_sink_user);
    for (int i = 0; i < filtered_count; i++) {
        if (filtered[i].type == ACT_POINTER_MOVE ||
            filtered[i].type == ACT_POINTER_MOVE_DELTA)
            g_pointer_move_pending = true;
    }
}

static void copy_label(char *out, size_t cap, const char *text) {
    if (cap == 0) return;
    if (!text) text = "";
    strncpy(out, text, cap - 1);
    out[cap - 1] = '\0';
}

/*
 * Small subset of ControlElement.getDisplayText() fallback logic.  The engine
 * keeps the first binding's encoded name, not the profile display string.
 */
static void fallback_label(const char *binding, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    if (!binding || binding[0] == '\0') return;

    if (strncmp(binding, "KEY_", 4) == 0) {
        const char *name = binding + 4;
        if (name[0] != '\0' && name[1] == '\0' &&
            name[0] >= '0' && name[0] <= '9') {
            snprintf(out, cap, "%c", name[0]);
            return;
        }
        if (strcmp(name, "TAB") == 0) {
            copy_label(out, cap, "TAB");
            return;
        }
        if (strcmp(name, "ESC") == 0) {
            copy_label(out, cap, "ESC");
            return;
        }
        if (strcmp(name, "ENTER") == 0) {
            copy_label(out, cap, "ENTER");
            return;
        }
        if (name[0] == 'F' && isdigit((unsigned char)name[1])) {
            copy_label(out, cap, name);
            return;
        }
        if (strcmp(name, "CTRL_L") == 0) {
            copy_label(out, cap, "L CTRL");
            return;
        }
        if (strcmp(name, "CTRL_R") == 0) {
            copy_label(out, cap, "R CTRL");
            return;
        }
        if (strcmp(name, "SHIFT_L") == 0) {
            copy_label(out, cap, "L SHIFT");
            return;
        }
        if (strcmp(name, "SHIFT_R") == 0) {
            copy_label(out, cap, "R SHIFT");
            return;
        }
        copy_label(out, cap, name);
        for (char *p = out; *p; p++) {
            if (*p == '_') *p = ' ';
        }
        return;
    }

    if (strncmp(binding, "MOD_", 4) == 0) {
        copy_label(out, cap, binding + 4);
        return;
    }
    if (strncmp(binding, "MOUSE_", 6) == 0) {
        copy_label(out, cap, "MOUSE");
        return;
    }
    copy_label(out, cap, binding);
    for (char *p = out; *p; p++) {
        if (*p == '_') *p = ' ';
    }
}

static void apply_profile_labels(const ProfileData *profile,
                                 TouchElement *elements, int count) {
    if (!profile || !elements) return;
    for (int i = 0; i < count && i < profile->element_count; i++) {
        if (elements[i].type != BUTTON || elements[i].display_text[0] != '\0') {
            continue;
        }
        const ProfileBindPackage *pkg =
            &profile->elements[i].slot_packages[0];
        if (pkg->count <= 0) continue;
        fallback_label(pkg->bindings[0].name, elements[i].display_text,
                       sizeof(elements[i].display_text));
    }
}

static void capture_engine_transform(float *xform_x, float *xform_y,
                                     float *view_x, float *view_y) {
    *xform_x = 1.0f;
    *xform_y = 1.0f;
    *view_x = 0.0f;
    *view_y = 0.0f;

    if (!g_initialized) return;

    TP_STATE_LOCK();
    if (g_state.cfg.xform_scale_x > 0.0f)
        *xform_x = g_state.cfg.xform_scale_x;
    if (g_state.cfg.xform_scale_y > 0.0f)
        *xform_y = g_state.cfg.xform_scale_y;
    *view_x = g_state.cfg.view_offset_x;
    *view_y = g_state.cfg.view_offset_y;
    TP_STATE_UNLOCK();
}

/* Build all variants from one parse.  SIMPLE and SIMPLE_ESC are derived from
 * the authored profile before the product gesture mode rewrites FULL.  They
 * share the same global gesture set; only SIMPLE_ESC keeps the authored ESC
 * element for skipping a cinematic scene. */
static int controls_build_variants(const char *profile_json,
                                   ProfileData **out_full,
                                   ProfileData **out_simple,
                                   ProfileData **out_simple_esc,
                                   int *out_simple_rc,
                                   int *out_simple_esc_rc) {
    ProfileData *full = NULL;
    ProfileData *simple = NULL;
    ProfileData *simple_esc = NULL;

    if (!profile_json || !out_full || !out_simple || !out_simple_esc ||
        !out_simple_rc || !out_simple_esc_rc)
        return -1;

    full = (ProfileData *)calloc(1, sizeof(*full));
    simple = (ProfileData *)calloc(1, sizeof(*simple));
    simple_esc = (ProfileData *)calloc(1, sizeof(*simple_esc));
    if (!full || !simple || !simple_esc) {
        set_error("out of memory building profiles");
        goto fail;
    }

    if (profile_load_from_string(profile_json, full) != 0) {
        const char *detail = profile_last_error();
        set_error("profile parse failed: %s", detail ? detail : "unknown");
        goto fail;
    }

    int simple_rc = wc3_profile_build_simple(full, simple);
    if (simple_rc != 0) {
        free(simple);
        simple = NULL;
    }

    int simple_esc_rc = wc3_profile_build_simple_esc(full, simple_esc);
    if (simple_esc_rc != 0) {
        free(simple_esc);
        simple_esc = NULL;
    }

    wc3_profile_apply_gesture_mode(full, g_double_tap_enabled);
    wc3_profile_apply_gesture_timings(full, g_double_tap_timeout_ms,
                                      g_long_press_timeout_ms);

    *out_full = full;
    *out_simple = simple;
    *out_simple_esc = simple_esc;
    *out_simple_rc = simple_rc;
    *out_simple_esc_rc = simple_esc_rc;
    return 0;

fail:
    free(simple_esc);
    free(simple);
    free(full);
    return -1;
}

static int controls_apply_profile(const ProfileData *profile, int width,
                                  int height, bool initial,
                                  Wc3OverlayMode mode,
                                  float xform_x, float xform_y,
                                  float view_x, float view_y) {
    TouchProcessorConfig *config = NULL;
    TouchElement *elements = NULL;
    int count = 0;
    int rc = -1;

    if (!profile || width <= 0 || height <= 0) {
        set_error("invalid profile apply arguments");
        return -1;
    }

    config = (TouchProcessorConfig *)calloc(1, sizeof(*config));
    elements = (TouchElement *)calloc(MAX_ELEMENTS, sizeof(*elements));
    if (!config || !elements) {
        set_error("out of memory applying profile");
        goto out;
    }

    g_timer_interval_ms = profile->timer_interval_ms > 0
                              ? profile->timer_interval_ms
                              : 16;

    profile_to_config(profile, width, height, 1.0f, config);
    config->direct_gesture_mode =
        wc3_overlay_mode_is_simple(mode) || !g_double_tap_enabled;
    config->xform_scale_x = xform_x > 0.0f ? xform_x : 1.0f;
    config->xform_scale_y = xform_y > 0.0f ? xform_y : 1.0f;
    config->view_offset_x = view_x;
    config->view_offset_y = view_y;

    /* Convert and validate before touching the live engine.  A bad profile
     * leaves the current mode and its input state intact.  An empty element
     * list is valid: the menu SIMPLE variant has no on-screen controls. */
    count = profile_to_elements(profile, width, height, elements,
                                MAX_ELEMENTS);
    if (count < 0) {
        set_error("profile element conversion failed");
        goto out;
    }
    if (count > WC3_OVERLAY_MAX_ELEMENTS) {
        set_error("profile has %d elements; overlay supports %d", count,
                  WC3_OVERLAY_MAX_ELEMENTS);
        goto out;
    }
    for (int i = 0; i < count; i++) {
        if (elements[i].type != BUTTON && elements[i].type != D_PAD) {
            set_error("unsupported element type %u at index %d",
                      (unsigned)elements[i].type, i);
            goto out;
        }
    }
    apply_profile_labels(profile, elements, count);

    /* touch_processor_set_elements() treats count 0 as "leave the current
     * elements alone", so an empty variant is installed through init(): it
     * clears the previous FULL layout even without cancelling it there.  The
     * switch and resize paths have already cancelled all input. */
    if (initial || count == 0) {
        touch_processor_init(config);
    } else {
        touch_processor_update_config(config);
    }
    reset_action_filter();

    int snap = width / 100;
    if (snap <= 0) snap = 10;
    touch_processor_set_snapping_size((float)snap);

    touch_processor_set_elements(elements, count);
    touch_processor_set_resolution_scale(1.0f);
    touch_processor_set_xform_scale(config->xform_scale_x,
                                    config->xform_scale_y);
    touch_processor_set_sim_touch_screen(false);

    g_width = width;
    g_height = height;
    /* The recognizer is sized in surface pixels and has no active pointers
     * here: init, mode switch and resize all cancel input (or are the first
     * initialisation) before applying a profile. */
    direct_touch_configure(width, height, profile);
    if (initial) g_last_engine_tick = 0;
    g_layout_id++;
    g_overlay_mode = mode;
    g_initialized = true;
    WC3_CTRL_LOG("dt overlay-mode=%d size=%dx%d direct=%d sink=%d",
                 (int)mode, width, height, (int)g_direct_touch_enabled,
                 g_direct_touch.sink != NULL);
    clear_error();
    rc = 0;

out:
    free(elements);
    free(config);
    return rc;
}

int controls_init_ex(const char *profile_json, int width, int height,
                     bool double_tap_enabled, int double_tap_timeout_ms,
                     int long_press_timeout_ms, int tap_click_delay_ms) {
    ProfileData *full = NULL;
    ProfileData *simple = NULL;
    ProfileData *simple_esc = NULL;
    int simple_rc = -1;
    int simple_esc_rc = -1;

    if (!profile_json || width <= 0 || height <= 0) {
        set_error("invalid init arguments");
        return -1;
    }

    char *copy = (char *)malloc(strlen(profile_json) + 1u);
    if (!copy) {
        set_error("out of memory copying profile");
        return -1;
    }
    strcpy(copy, profile_json);

    g_double_tap_enabled = double_tap_enabled;
    g_double_tap_timeout_ms = double_tap_timeout_ms;
    g_long_press_timeout_ms = long_press_timeout_ms;
    g_tap_click_delay_ms = clamp_tap_click_delay_ms(tap_click_delay_ms);

    if (controls_build_variants(profile_json, &full, &simple, &simple_esc,
                                &simple_rc, &simple_esc_rc) != 0) {
        free(copy);
        return -1;
    }

    if (g_initialized) {
        controls_cancel_all();
    }

    if (controls_apply_profile(full, width, height, true,
                               WC3_OVERLAY_MODE_FULL, 1.0f, 1.0f,
                               0.0f, 0.0f) != 0) {
        free(simple_esc);
        free(simple);
        free(full);
        free(copy);
        return -1;
    }

    free(g_profile_simple_esc);
    free(g_profile_simple);
    free(g_profile_full);
    free(g_profile_json);
    g_profile_full = full;
    g_profile_simple = simple;
    g_profile_simple_esc = simple_esc;
    g_profile_json = copy;
    g_simple_build_rc = simple_rc;
    g_simple_esc_build_rc = simple_esc_rc;
    return 0;
}

int controls_init(const char *profile_json, int width, int height) {
    return controls_init_ex(profile_json, width, height, true, 0, 0, 0);
}

void controls_set_action_sink(Wc3ActionSink sink, void *user) {
    g_sink = sink;
    g_sink_user = user;
}

static uint64_t engine_tick_from_ms(uint64_t time_ms);

static void direct_owners_clear(void) {
    memset(g_direct_owners, 0, sizeof(g_direct_owners));
}

static DirectOwner *direct_owner_find(int pointer_id) {
    for (int i = 0; i < DIRECT_OWNER_SLOTS; i++) {
        if (g_direct_owners[i].owner != DIRECT_OWNER_NONE &&
            g_direct_owners[i].pointer_id == pointer_id) {
            return &g_direct_owners[i];
        }
    }
    return NULL;
}

static DirectOwner *direct_owner_add(int pointer_id, int owner,
                                     float x, float y) {
    DirectOwner *slot = direct_owner_find(pointer_id);
    if (!slot) {
        for (int i = 0; i < DIRECT_OWNER_SLOTS; i++) {
            if (g_direct_owners[i].owner == DIRECT_OWNER_NONE) {
                slot = &g_direct_owners[i];
                break;
            }
        }
    }
    if (!slot) return NULL;
    slot->pointer_id = pointer_id;
    slot->owner = owner;
    slot->last_x = x;
    slot->last_y = y;
    return slot;
}

static void direct_owner_remove(int pointer_id) {
    DirectOwner *slot = direct_owner_find(pointer_id);
    if (slot) {
        slot->owner = DIRECT_OWNER_NONE;
        slot->pointer_id = -1;
    }
}

static int direct_owner_any_vendor(void) {
    for (int i = 0; i < DIRECT_OWNER_SLOTS; i++) {
        if (g_direct_owners[i].owner == DIRECT_OWNER_VENDOR) return 1;
    }
    return 0;
}

/* A native HUD gesture and any extra pointers swallowed while it was active
 * form one unit: a new gesture must not start until every one of them is up.
 * A minimap drag is a native gesture too. */
static int direct_owner_native_gesture(void) {
    for (int i = 0; i < DIRECT_OWNER_SLOTS; i++) {
        if (g_direct_owners[i].owner == DIRECT_OWNER_NATIVE ||
            g_direct_owners[i].owner == DIRECT_OWNER_MINIMAP ||
            g_direct_owners[i].owner == DIRECT_OWNER_IGNORED) {
            return 1;
        }
    }
    return 0;
}

/* The portrait follow belongs to one finger.  A second finger must not
 * cancel it; the caller either leaves an overlay press to the legacy engine
 * or swallows the extra pointer. */
static int direct_owner_portrait_active(void) {
    for (int i = 0; i < DIRECT_OWNER_SLOTS; i++) {
        if (g_direct_owners[i].owner == DIRECT_OWNER_PORTRAIT) return 1;
    }
    return 0;
}

/*
 * The field rectangle is a product boundary, not a native hit test.  It is
 * the battlefield region down to the console's top edge; everything below
 * it is native HUD (minimap, portrait, command card, resource bar, top
 * menu).  In surface coordinates the minimap starts at about 0.855 and the
 * command card at 0.782, so the card's top row overlaps the field; the
 * command gate claims it for NATIVE before the field test.  A 0.72 bottom
 * dropped real world releases above the console.  The overlay hit test and
 * the portrait/minimap/beacon/command rect gates run first, so an on-screen
 * control keeps its own semantics.
 */
static int direct_touch_field_contains(float x, float y) {
    if (g_width <= 0 || g_height <= 0) return 0;
    const float left = (float)g_width * 0.03f;
    const float right = (float)g_width * 0.97f;
    const float top = (float)g_height * 0.10f;
    const float bottom = (float)g_height * 0.85f;
    return x >= left && x <= right && y >= top && y <= bottom;
}

static int direct_touch_ui_contains(float x, float y) {
    ELEMENTS_LOCK();
    const int hit = hit_test_element(x, y) != NULL;
    ELEMENTS_UNLOCK();
    return hit;
}

/*
 * The portrait and minimap gates are lock-free snapshots published by the
 * guest thread.  They are checked before the field rectangle because both
 * sit in the native HUD area and must keep their own semantics instead of raw
 * pointer phases.  A missing or empty rectangle means the frame is not on
 * screen; the touch keeps its normal FIELD/NATIVE route.
 */
static int direct_touch_rect_contains(Wc3DirectTouchRectGate gate, void *user,
                                      float x, float y) {
    if (!gate || g_width <= 0 || g_height <= 0) return 0;
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    if (!gate(&left, &top, &right, &bottom, user)) return 0;
    if (!(right > left) || !(bottom > top)) return 0;
    const float nx = x / (float)g_width;
    const float ny = y / (float)g_height;
    return nx >= left && nx <= right && ny >= top && ny <= bottom;
}

static int direct_touch_portrait_contains(float x, float y) {
    return direct_touch_rect_contains(g_direct_touch_portrait_gate,
                                      g_direct_touch_portrait_gate_user,
                                      x, y);
}

static int direct_touch_minimap_contains(float x, float y) {
    return direct_touch_rect_contains(g_direct_touch_minimap_gate,
                                      g_direct_touch_minimap_gate_user,
                                      x, y);
}

static int direct_touch_beacon_contains(float x, float y) {
    return direct_touch_rect_contains(g_direct_touch_beacon_gate,
                                      g_direct_touch_beacon_gate_user,
                                      x, y);
}

static int direct_touch_command_contains(float x, float y) {
    return direct_touch_rect_contains(g_direct_touch_command_gate,
                                      g_direct_touch_command_gate_user,
                                      x, y);
}

/*
 * Decide who owns a non-overlay touch.  Direct mode never hands a game touch
 * to the legacy overlay engine: a battlefield touch in CSelectMode uses the
 * semantic recognizer, and every other game touch (native HUD, menu,
 * targeting/placement) is forwarded as raw pointer phases to the guest-side
 * native mouse path.  The only LEGACY result is an authored overlay control,
 * which is an app action, not game input.  The guest publishes the route
 * lock-free; the input thread never calls guest code.  A LEGACY snapshot is
 * not a field hint: it means the guest has no live CGameUI, so the touch is
 * native (the HLE resolves the map-load race on release).
 */
static Wc3DirectTouchRoute direct_touch_route_for(float x, float y) {
    if (!g_direct_touch_enabled) {
        DT_ROUTE_RETURN(WC3_DIRECT_TOUCH_ROUTE_LEGACY, "disabled");
    }
    if (direct_touch_ui_contains(x, y)) {
        DT_ROUTE_RETURN(WC3_DIRECT_TOUCH_ROUTE_LEGACY, "overlay-element");
    }
    /* A missing sink or an uninitialised recognizer means the semantic FIELD
     * path is not available yet.  Raw native phases are still safe and keep
     * the game touch out of the legacy engine. */
    if (!g_initialized || !g_direct_touch.sink) {
        DT_ROUTE_RETURN(WC3_DIRECT_TOUCH_ROUTE_NATIVE, "direct-not-ready");
    }
    /* A NULL gate is the unit-test/host-only mode: preserve the phase-1
     * semantic field behavior. */
    const Wc3DirectTouchRoute route = g_direct_touch_route_gate
        ? g_direct_touch_route_gate(g_direct_touch_route_gate_user)
        : WC3_DIRECT_TOUCH_ROUTE_FIELD;
#if WC3_DEBUG && WC3_DEBUG_INPUT
    {
        const float nx = g_width > 0 ? x / (float)g_width : 0.0f;
        const float ny = g_height > 0 ? y / (float)g_height : 0.0f;
        const float fl = (float)g_width * 0.03f;
        const float fr = (float)g_width * 0.97f;
        const float ft = (float)g_height * 0.10f;
        const float fb = (float)g_height * 0.85f;
        WC3_CTRL_LOG("dt route-in px=%.1f,%.1f n=%.4f,%.4f "
                     "size=%dx%d overlay=%d enabled=%d sink=%d "
                     "gate=%s field=%d field_rect=%.1f,%.1f..%.1f,%.1f",
                     x, y, nx, ny, g_width, g_height, (int)g_overlay_mode,
                     (int)g_direct_touch_enabled,
                     g_direct_touch.sink != NULL, direct_route_name(route),
                     direct_touch_field_contains(x, y), fl, ft, fr, fb);
        direct_touch_trace_gate("beacon", g_direct_touch_beacon_gate,
                                g_direct_touch_beacon_gate_user);
        direct_touch_trace_gate("portrait", g_direct_touch_portrait_gate,
                                g_direct_touch_portrait_gate_user);
        direct_touch_trace_gate("minimap", g_direct_touch_minimap_gate,
                                g_direct_touch_minimap_gate_user);
        direct_touch_trace_gate("command", g_direct_touch_command_gate,
                                g_direct_touch_command_gate_user);
    }
#endif
    if (route == WC3_DIRECT_TOUCH_ROUTE_LEGACY) {
        /* LEGACY from the guest means it has no live CGameUI: startup, a
         * loading screen or the main menu.  The main menu uses the same
         * rectangle as the battlefield, so geometry cannot turn this into a
         * FIELD gesture.  Direct mode forwards every game touch as raw native
         * pointer phases; the HLE resolves a live FIELD at release when the
         * published snapshot was only one pump stale, and otherwise posts the
         * native menu click.  Overlay elements and a disabled recognizer have
         * already returned above, so this cannot intercept an authored
         * control. */
        DT_ROUTE_RETURN(WC3_DIRECT_TOUCH_ROUTE_NATIVE, "snapshot-native");
    }
    /* The portrait, minimap and beacon button are native panels, but the
     * portrait frame has no click action of its own and the minimap drag must
     * stay owned by the minimap when the finger leaves it.  The beacon button
     * is forced to NATIVE even when it overlaps the field: the guest claims
     * the press and turns it into a beacon instead of a battlefield order. */
    if (direct_touch_beacon_contains(x, y)) {
        DT_ROUTE_RETURN(WC3_DIRECT_TOUCH_ROUTE_NATIVE, "beacon");
    }
    if (direct_touch_portrait_contains(x, y)) {
        DT_ROUTE_RETURN(WC3_DIRECT_TOUCH_ROUTE_PORTRAIT, "portrait");
    }
    if (direct_touch_minimap_contains(x, y)) {
        DT_ROUTE_RETURN(WC3_DIRECT_TOUCH_ROUTE_MINIMAP, "minimap");
    }
    /* The command card's top row starts above the field bottom, so the
     * generic FIELD test would claim it.  Claim the whole card for NATIVE
     * first; the guest command handler then hit-tests the real buttons. */
    if (direct_touch_command_contains(x, y)) {
        DT_ROUTE_RETURN(WC3_DIRECT_TOUCH_ROUTE_NATIVE, "command");
    }
    if (route == WC3_DIRECT_TOUCH_ROUTE_FIELD &&
        direct_touch_field_contains(x, y)) {
        DT_ROUTE_RETURN(WC3_DIRECT_TOUCH_ROUTE_FIELD, "field");
    }
    if (route == WC3_DIRECT_TOUCH_ROUTE_FIELD ||
        route == WC3_DIRECT_TOUCH_ROUTE_NATIVE) {
        DT_ROUTE_RETURN(WC3_DIRECT_TOUCH_ROUTE_NATIVE,
                        "gate-native-or-outside-field");
    }
    /* PORTRAIT/MINIMAP were not claimed by their geometric gates (for
     * example, the frame is not published yet).  The guest still owns the
     * click; native phases are the only non-legacy route. */
    DT_ROUTE_RETURN(WC3_DIRECT_TOUCH_ROUTE_NATIVE, "gate-native");
}

static void direct_touch_configure(int width, int height,
                                   const ProfileData *profile) {
    /* The battlefield gestures mirror the authored legacy profile: one drag
     * threshold (11 px) decides tap vs. camera drag and hold vs. selection
     * drag, the long-press timer is the profile's 120 ms, and the hold plays
     * gestureLongPressHaptic.  A missing profile (host-only tests) keeps the
     * same defaults. */
    float drag_threshold = 11.0f;
    int long_press_ms = 120;
    int haptic_effect = 3;
    if (profile) {
        if (profile->drag_threshold_px > 0)
            drag_threshold = (float)profile->drag_threshold_px;
        if (profile->gesture_settings.long_press_timeout_ms > 0)
            long_press_ms = profile->gesture_settings.long_press_timeout_ms;
        haptic_effect = profile->gesture_long_press_haptic;
    }
    g_direct_touch_haptic_effect = haptic_effect;
    wc3_direct_touch_init(&g_direct_touch, (float)width, (float)height,
                          drag_threshold, long_press_ms,
                          g_direct_touch_sink, g_direct_touch_user);
    wc3_direct_touch_set_haptic(&g_direct_touch,
                                g_direct_touch_haptic_sink,
                                g_direct_touch_haptic_user,
                                g_direct_touch_haptic_effect);
}

void controls_set_direct_touch_haptic_sink(Wc3DirectTouchHapticSink sink,
                                           void *user) {
    g_direct_touch_haptic_sink = sink;
    g_direct_touch_haptic_user = user;
    if (g_initialized) {
        wc3_direct_touch_set_haptic(&g_direct_touch, sink, user,
                                    g_direct_touch_haptic_effect);
    }
}

void controls_set_direct_touch_enabled(bool enabled) {
    g_direct_touch_enabled = enabled;
    if (!enabled && g_initialized) controls_cancel_all();
}

void controls_set_direct_touch_sink(Wc3DirectTouchSink sink, void *user) {
    g_direct_touch_sink = sink;
    g_direct_touch_user = user;
    if (!sink) {
        /* No sink means no direct owner; do not keep a stale guest gate. */
        g_direct_touch_route_gate = NULL;
        g_direct_touch_route_gate_user = NULL;
        g_direct_touch_portrait_gate = NULL;
        g_direct_touch_portrait_gate_user = NULL;
        g_direct_touch_minimap_gate = NULL;
        g_direct_touch_minimap_gate_user = NULL;
        g_direct_touch_beacon_gate = NULL;
        g_direct_touch_beacon_gate_user = NULL;
        g_direct_touch_command_gate = NULL;
        g_direct_touch_command_gate_user = NULL;
    }
    if (g_initialized) {
        wc3_direct_touch_set_sink(&g_direct_touch, sink, user);
    }
}

void controls_set_direct_touch_route_gate(Wc3DirectTouchRouteGate gate,
                                           void *user) {
    g_direct_touch_route_gate = gate;
    g_direct_touch_route_gate_user = user;
}

void controls_set_direct_touch_portrait_gate(
    Wc3DirectTouchPortraitRectGate gate, void *user) {
    g_direct_touch_portrait_gate = gate;
    g_direct_touch_portrait_gate_user = user;
}

void controls_set_direct_touch_minimap_gate(
    Wc3DirectTouchMinimapRectGate gate, void *user) {
    g_direct_touch_minimap_gate = gate;
    g_direct_touch_minimap_gate_user = user;
}

void controls_set_direct_touch_beacon_gate(
    Wc3DirectTouchBeaconRectGate gate, void *user) {
    g_direct_touch_beacon_gate = gate;
    g_direct_touch_beacon_gate_user = user;
}

void controls_set_direct_touch_command_gate(
    Wc3DirectTouchCommandRectGate gate, void *user) {
    g_direct_touch_command_gate = gate;
    g_direct_touch_command_gate_user = user;
}

/*
 * A second finger no longer hijacks the first field contact.  The recognizer
 * assigns roles: one contact may claim the cursor-free camera, the other owns
 * the virtual mouse.  A camera drag holds no native button, so there is no
 * handoff and no swallow here.  Native HUD/portrait gestures still swallow
 * extra pointers because they really own the cursor/follow.
 */
static int direct_touch_begin(int pointer_id, float x, float y,
                              uint64_t event_time_ms) {
    /* Once any pointer belongs to the vendor engine, the gesture is a
     * camera/pinch or an overlay interaction.  Keep later pointers on that
     * path until every pointer is up; starting a fresh direct gesture in the
     * middle of a two-finger gesture would mix an order into a pan. */
    if (direct_owner_any_vendor()) {
        WC3_CTRL_LOG("dt begin ptr=%d px=%.1f,%.1f -> vendor-active",
                     pointer_id, x, y);
        return 0;
    }

    /* Resolve the route before swallowing a second native pointer: an
     * overlay element pressed while a HUD gesture is active must still reach
     * the legacy engine, not become an ignored direct pointer. */
    const Wc3DirectTouchRoute route = direct_touch_route_for(x, y);

    WC3_CTRL_LOG("dt begin ptr=%d px=%.1f,%.1f route=%s native=%d "
                 "portrait=%d field_drag=%d active=%d",
                 pointer_id, x, y, direct_route_name(route),
                 direct_owner_native_gesture(),
                 direct_owner_portrait_active(),
                 wc3_direct_touch_field_drag_active(&g_direct_touch),
                 wc3_direct_touch_active_count(&g_direct_touch));
#if WC3_DEBUG && WC3_DEBUG_INPUT
    wc3_direct_touch_debug_log("route ptr=%d px=%.1f,%.1f route=%s "
                               "native=%d portrait=%d field_drag=%d "
                               "active=%d event_t=%llu",
                               pointer_id, x, y, direct_route_name(route),
                               direct_owner_native_gesture(),
                               direct_owner_portrait_active(),
                               wc3_direct_touch_field_drag_active(
                                   &g_direct_touch),
                               wc3_direct_touch_active_count(
                                   &g_direct_touch),
                               (unsigned long long)event_time_ms);
#endif

    if (direct_owner_native_gesture()) {
        /* Native HUD gestures are single-pointer.  Swallow an extra pointer
         * over another HUD control instead of handing it to the vendor engine:
         * a second finger on the minimap or command card must not turn the
         * first click into a camera gesture.  A finger in the battlefield is
         * different: the first finger may already be the native cursor, and a
         * quick swipe in the world must still be able to pan the camera.  The
         * recognizer keeps that pointer isolated from the virtual mouse. */
        if (route == WC3_DIRECT_TOUCH_ROUTE_LEGACY) return 0;
        if (route != WC3_DIRECT_TOUCH_ROUTE_FIELD) {
            (void)direct_owner_add(pointer_id, DIRECT_OWNER_IGNORED, x, y);
            return 1;
        }
    }
    if (direct_owner_portrait_active()) {
        /* The follow belongs to the finger that landed on the portrait.  A
         * second finger must not reset it.  Overlay controls still work, so
         * an overlay press goes to the legacy engine; any other extra
         * pointer is swallowed until the portrait finger is released. */
        if (route == WC3_DIRECT_TOUCH_ROUTE_LEGACY) return 0;
        (void)direct_owner_add(pointer_id, DIRECT_OWNER_IGNORED, x, y);
        return 1;
    }
    /* An active camera drag holds no mouse button; a second finger is routed
     * normally and may become the action cursor.  A selection drag still owns
     * the left button, but the recognizer handles its owner by role, so no
     * pointer is swallowed here. */

    if (route == WC3_DIRECT_TOUCH_ROUTE_LEGACY) return 0;
    if (route == WC3_DIRECT_TOUCH_ROUTE_MINIMAP) {
        if (!direct_owner_add(pointer_id, DIRECT_OWNER_MINIMAP, x, y)) {
            return 0;
        }
        wc3_direct_touch_emit_pointer(&g_direct_touch,
                                      WC3_DIRECT_TOUCH_POINTER_DOWN,
                                      pointer_id, x, y, event_time_ms);
        return 1;
    }
    if (route == WC3_DIRECT_TOUCH_ROUTE_PORTRAIT) {
        if (!direct_owner_add(pointer_id, DIRECT_OWNER_PORTRAIT, x, y)) {
            return 0;
        }
        wc3_direct_touch_down_route(&g_direct_touch, pointer_id, x, y,
                                    event_time_ms,
                                    WC3_DIRECT_TOUCH_ROUTE_PORTRAIT);
        return 1;
    }
    if (route == WC3_DIRECT_TOUCH_ROUTE_FIELD) {
        if (!direct_owner_add(pointer_id, DIRECT_OWNER_FIELD, x, y)) return 0;
        wc3_direct_touch_down(&g_direct_touch, pointer_id, x, y,
                              event_time_ms);
        return 1;
    }

    if (!direct_owner_add(pointer_id, DIRECT_OWNER_NATIVE, x, y)) return 0;
    wc3_direct_touch_emit_pointer(&g_direct_touch,
                                  WC3_DIRECT_TOUCH_POINTER_DOWN,
                                  pointer_id, x, y, event_time_ms);
    return 1;
}

static uint64_t engine_tick_from_ms(uint64_t time_ms) {
    int interval = g_timer_interval_ms > 0 ? g_timer_interval_ms : 16;
    uint64_t tick = time_ms / (uint64_t)interval;
    if (tick < g_last_engine_tick) tick = g_last_engine_tick;
    return tick;
}

/*
 * Keep a minimap-owned pointer inside the published minimap rectangle.  The
 * native minimap handler only receives mouse moves while the cursor is over
 * the minimap frame, so a finger that leaves the rectangle would otherwise
 * stop dragging.  Clamping keeps the drag alive and drives the camera to the
 * edge; the pointer remains owned by the minimap until it is released.
 */
static void direct_touch_clamp_minimap(float *x, float *y,
                                       const DirectOwner *owner) {
    if (g_width <= 0 || g_height <= 0) return;
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    if (!g_direct_touch_minimap_gate ||
        !g_direct_touch_minimap_gate(&left, &top, &right, &bottom,
                                     g_direct_touch_minimap_gate_user) ||
        !(right > left) || !(bottom > top)) {
        /* The frame disappeared while the drag was active (UI transition).
         * Keep the last in-rect position so the native handler still sees a
         * coherent release instead of a jump. */
        *x = owner->last_x;
        *y = owner->last_y;
        return;
    }
    const float px_left = left * (float)g_width;
    const float px_right = right * (float)g_width;
    const float px_top = top * (float)g_height;
    const float px_bottom = bottom * (float)g_height;
    if (*x < px_left) *x = px_left;
    if (*x > px_right) *x = px_right;
    if (*y < px_top) *y = px_top;
    if (*y > px_bottom) *y = px_bottom;
}

int controls_touch(Wc3TouchPhase phase, int pointer_id, float x, float y,
                       uint64_t event_time_ms) {
#if WC3_DEBUG && WC3_DEBUG_INPUT
    if (phase != WC3_TOUCH_MOVE) {
        wc3_direct_touch_debug_log(
            "controls phase=%d ptr=%d px=%.1f,%.1f event_t=%llu",
            (int)phase, pointer_id, x, y,
            (unsigned long long)event_time_ms);
    }
#endif
    if (phase == WC3_TOUCH_CANCEL) {
        controls_cancel_all();
        return 0;
    }
    if (!g_initialized) {
        set_error("touch before init");
        return -1;
    }

    if (phase == WC3_TOUCH_DOWN) {
        if (direct_touch_begin(pointer_id, x, y, event_time_ms)) return 0;
        WC3_CTRL_LOG("dt down ptr=%d px=%.1f,%.1f -> vendor", pointer_id, x, y);
        direct_owner_add(pointer_id, DIRECT_OWNER_VENDOR, x, y);
    } else {
        DirectOwner *owner = direct_owner_find(pointer_id);
        if (owner) {
            if (phase == WC3_TOUCH_UP) {
                WC3_CTRL_LOG("dt up ptr=%d px=%.1f,%.1f owner=%s "
                             "last=%.1f,%.1f",
                             pointer_id, x, y,
                             direct_owner_name(owner->owner),
                             owner->last_x, owner->last_y);
#if WC3_DEBUG && WC3_DEBUG_INPUT
                wc3_direct_touch_debug_log(
                    "owner-up ptr=%d px=%.1f,%.1f owner=%s last=%.1f,%.1f "
                    "event_t=%llu", pointer_id, x, y,
                    direct_owner_name(owner->owner), owner->last_x,
                    owner->last_y, (unsigned long long)event_time_ms);
#endif
            } else if (owner->owner == DIRECT_OWNER_IGNORED ||
                       owner->owner == DIRECT_OWNER_VENDOR) {
                WC3_CTRL_LOG("dt move ptr=%d px=%.1f,%.1f owner=%s",
                             pointer_id, x, y,
                             direct_owner_name(owner->owner));
            }
            if (owner->owner == DIRECT_OWNER_MINIMAP) {
                float cx = x;
                float cy = y;
                direct_touch_clamp_minimap(&cx, &cy, owner);
                owner->last_x = cx;
                owner->last_y = cy;
                if (phase == WC3_TOUCH_MOVE) {
                    wc3_direct_touch_emit_pointer(
                        &g_direct_touch, WC3_DIRECT_TOUCH_POINTER_MOVE,
                        pointer_id, cx, cy, event_time_ms);
                } else if (phase == WC3_TOUCH_UP) {
                    wc3_direct_touch_emit_pointer(
                        &g_direct_touch, WC3_DIRECT_TOUCH_POINTER_UP,
                        pointer_id, cx, cy, event_time_ms);
                    direct_owner_remove(pointer_id);
                }
                return 0;
            }
            owner->last_x = x;
            owner->last_y = y;
            if (owner->owner == DIRECT_OWNER_FIELD ||
                owner->owner == DIRECT_OWNER_PORTRAIT) {
                if (phase == WC3_TOUCH_MOVE) {
                    wc3_direct_touch_move(&g_direct_touch, pointer_id, x, y,
                                          event_time_ms);
                } else if (phase == WC3_TOUCH_UP) {
                    wc3_direct_touch_up(&g_direct_touch, pointer_id, x, y,
                                        event_time_ms);
                    direct_owner_remove(pointer_id);
                }
                return 0;
            }
            if (owner->owner == DIRECT_OWNER_NATIVE) {
                if (phase == WC3_TOUCH_MOVE) {
                    wc3_direct_touch_emit_pointer(
                        &g_direct_touch, WC3_DIRECT_TOUCH_POINTER_MOVE,
                        pointer_id, x, y, event_time_ms);
                } else if (phase == WC3_TOUCH_UP) {
                    wc3_direct_touch_emit_pointer(
                        &g_direct_touch, WC3_DIRECT_TOUCH_POINTER_UP,
                        pointer_id, x, y, event_time_ms);
                    direct_owner_remove(pointer_id);
                }
                return 0;
            }
            if (owner->owner == DIRECT_OWNER_IGNORED) {
                if (phase == WC3_TOUCH_UP) direct_owner_remove(pointer_id);
                return 0;
            }
        }
    }

    if (phase != WC3_TOUCH_MOVE) {
        WC3_CTRL_LOG("dt vendor phase=%d ptr=%d px=%.1f,%.1f",
                     (int)phase, pointer_id, x, y);
    }

    uint64_t tick = engine_tick_from_ms(event_time_ms);
    TouchActionResult result;
    memset(&result, 0, sizeof(result));

    TP_STATE_LOCK();
    ELEMENTS_LOCK();
    switch (phase) {
        case WC3_TOUCH_DOWN:
            touch_processor_on_finger_down(&result, pointer_id, x, y, tick);
            break;
        case WC3_TOUCH_MOVE:
            touch_processor_on_finger_move(&result, pointer_id, x, y, tick);
            break;
        case WC3_TOUCH_UP:
            touch_processor_on_finger_up(&result, pointer_id, x, y, tick);
            break;
        default:
            ELEMENTS_UNLOCK();
            TP_STATE_UNLOCK();
            set_error("invalid touch phase %d", (int)phase);
            return -1;
    }
    ELEMENTS_UNLOCK();
    TP_STATE_UNLOCK();

    if (phase == WC3_TOUCH_UP) direct_owner_remove(pointer_id);
    g_last_engine_tick = tick;
    emit_actions(&result);
    return 0;
}

int controls_tick(uint64_t now_ms) {
    if (!g_initialized) {
        set_error("tick before init");
        return -1;
    }

    uint64_t tick = engine_tick_from_ms(now_ms);
    TouchActionResult result;
    memset(&result, 0, sizeof(result));

    /*
     * touch_processor_tick() takes ELEMENTS_LOCK itself.  Holding it here
     * would deadlock on the non-recursive elements_mutex.
     */
    TP_STATE_LOCK();
    touch_processor_tick(&result, tick);
    TP_STATE_UNLOCK();

    wc3_direct_touch_tick(&g_direct_touch, now_ms);

    g_last_engine_tick = tick;
    emit_actions(&result);
    return 0;
}

int controls_needs_tick(void) {
    if (!g_initialized) return 0;
    TP_STATE_LOCK();
    int needed = touch_processor_needs_tick();
    TP_STATE_UNLOCK();
    return needed || wc3_direct_touch_next_deadline_ms(&g_direct_touch) != 0;
}

uint64_t controls_next_deadline_ms(uint64_t now_ms) {
    if (!g_initialized) return 0;

    TP_STATE_LOCK();
    uint64_t deadline_tick = touch_processor_next_deadline_tick();
    int interval = g_timer_interval_ms > 0 ? g_timer_interval_ms : 16;
    TP_STATE_UNLOCK();

    uint64_t direct_deadline = 0;
    const uint64_t direct_pending =
        wc3_direct_touch_next_deadline_ms(&g_direct_touch);
    if (direct_pending != 0) direct_deadline = direct_pending;

    uint64_t deadline_ms = direct_deadline;
    if (deadline_tick != TOUCH_PROCESSOR_DEADLINE_NONE) {
        uint64_t vendor_deadline;
        if (deadline_tick > UINT64_MAX / (uint64_t)interval)
            vendor_deadline = UINT64_MAX;
        else
            vendor_deadline = deadline_tick * (uint64_t)interval;
        if (deadline_ms == 0 || vendor_deadline < deadline_ms)
            deadline_ms = vendor_deadline;
    }

    if (deadline_ms == 0) return 0;
    // A due deadline must still be a real wake request: 0 is the API's
    // "no deadline" sentinel, not "now".
    if (deadline_ms <= now_ms) return now_ms != 0 ? now_ms : 1;
    return deadline_ms;
}

static void direct_touch_native_cancel(void) {
    for (int i = 0; i < DIRECT_OWNER_SLOTS; i++) {
        DirectOwner *slot = &g_direct_owners[i];
        if (slot->owner != DIRECT_OWNER_NATIVE &&
            slot->owner != DIRECT_OWNER_MINIMAP) {
            continue;
        }
        wc3_direct_touch_emit_pointer(&g_direct_touch,
                                      WC3_DIRECT_TOUCH_POINTER_CANCEL,
                                      slot->pointer_id, slot->last_x,
                                      slot->last_y, 0);
    }
}

void controls_cancel_all(void) {
    if (!g_initialized) return;

    /* Release a native mouse button before the pointer table disappears.
     * The guest-side service turns this into WM_LBUTTONUP; without it a
     * cancelled minimap drag or command-card press could leave the button
     * logically held. */
    direct_touch_native_cancel();
    wc3_direct_touch_reset(&g_direct_touch);
    direct_owners_clear();

    TouchActionResult result;
    memset(&result, 0, sizeof(result));

    /* cancel_all takes ELEMENTS_LOCK itself. */
    TP_STATE_LOCK();
    touch_processor_cancel_all(&result);
    TP_STATE_UNLOCK();

    emit_actions(&result);
}

int controls_switch_safe(void) {
    if (!g_initialized) return 0;
    if (wc3_direct_touch_active_count(&g_direct_touch) != 0) return 0;
    if (direct_owner_native_gesture()) return 0;

    TP_STATE_LOCK();
    int safe = g_state.active_finger_count == 0 &&
               g_state.scheduled_action_count == 0 &&
               g_state.gesture_deferred_tap_count == 0 &&
               g_state.gesture_pending_double_count == 0 &&
               g_state.gesture_pending_deferred_double_count == 0 &&
               g_state.gesture_held_count == 0 &&
               g_state.gesture_toggled_count == 0 &&
               g_state.dt_waiting_count == 0 &&
               g_state.sim_click_ptr_id < 0 &&
               !g_state.sim_continue_click &&
               g_state.pending_left_release_tick == 0 &&
               g_state.pending_right_release_tick == 0 &&
               !g_state.mouse_move_active &&
               !g_state.scrolling &&
               !g_state.gesture_second_active;
    TP_STATE_UNLOCK();
    return safe ? 1 : 0;
}

static const ProfileData *profile_for_mode(Wc3OverlayMode mode) {
    switch (mode) {
    case WC3_OVERLAY_MODE_FULL:
        return g_profile_full;
    case WC3_OVERLAY_MODE_SIMPLE:
        return g_profile_simple;
    case WC3_OVERLAY_MODE_SIMPLE_ESC:
        return g_profile_simple_esc;
    default:
        return NULL;
    }
}

int controls_set_overlay_mode(Wc3OverlayMode mode) {
    if (!g_initialized || !g_profile_full) {
        set_error("mode switch before init");
        return -1;
    }
    if (mode != WC3_OVERLAY_MODE_FULL &&
        mode != WC3_OVERLAY_MODE_SIMPLE &&
        mode != WC3_OVERLAY_MODE_SIMPLE_ESC) {
        set_error("invalid overlay mode %d", (int)mode);
        return -1;
    }
    if (mode == g_overlay_mode) return 0;

    const ProfileData *profile = profile_for_mode(mode);
    if (!profile) {
        if (mode == WC3_OVERLAY_MODE_SIMPLE_ESC) {
            set_error("SIMPLE_ESC unavailable (builder rc %d)",
                      g_simple_esc_build_rc);
        } else {
            set_error("SIMPLE unavailable (builder rc %d)",
                      g_simple_build_rc);
        }
        return -1;
    }
    if (!controls_switch_safe()) {
        set_error("mode switch requires idle input");
        return -2;
    }

    float xform_x, xform_y, view_x, view_y;
    capture_engine_transform(&xform_x, &xform_y, &view_x, &view_y);

    /* The guard above means this releases nothing; it also makes the exact
     * switch point explicit and keeps the engine contract in one place. */
    controls_cancel_all();

    return controls_apply_profile(profile, g_width, g_height, false, mode,
                                  xform_x, xform_y, view_x, view_y);
}

Wc3OverlayMode controls_overlay_mode(void) {
    return g_overlay_mode;
}

uint32_t controls_layout_id(void) {
    return g_layout_id;
}

int controls_resize(int width, int height) {
    if (!g_initialized || !g_profile_full || width <= 0 || height <= 0) {
        return -1;
    }
    if (width == g_width && height == g_height) return 0;

    const ProfileData *profile = profile_for_mode(g_overlay_mode);
    if (!profile) profile = g_profile_full;

    float xform_x, xform_y, view_x, view_y;
    capture_engine_transform(&xform_x, &xform_y, &view_x, &view_y);
    controls_cancel_all();

    if (controls_apply_profile(profile, width, height, false, g_overlay_mode,
                               xform_x, xform_y, view_x, view_y) != 0) {
        set_error("resize profile apply failed");
        return -1;
    }
    /* controls_apply_profile() already re-initialised the direct recognizer
     * with the new size after cancelling input. */
    return 0;
}

static uint8_t visual_layers_from_engine(uint32_t layers) {
    return (uint8_t)(layers &
                     (WC3_OVERLAY_LAYER_FILL |
                      WC3_OVERLAY_LAYER_STROKE_TEXT |
                      WC3_OVERLAY_LAYER_GLOW |
                      WC3_OVERLAY_LAYER_OUTER_STROKE));
}

static uint8_t active_petals_from_engine(const TouchElement *element) {
    uint8_t mask = 0;
    for (int p = 0; p < MAX_PETALS; p++) {
        if (element->petal_active[p]) mask |= (uint8_t)(1u << p);
    }
    return mask;
}

int controls_populate_overlay(Wc3Overlay *overlay) {
    if (!overlay || !g_initialized) {
        set_error("populate_overlay before init");
        return -1;
    }
    if (overlay->width <= 0 || overlay->height <= 0) {
        overlay_init(overlay, g_width, g_height, NULL);
    }

    int n = 0;
    int rc = -1;

    TP_STATE_LOCK();
    ELEMENTS_LOCK();
    n = g_state.element_count;
    if (n < 0 || n > WC3_OVERLAY_MAX_ELEMENTS) {
        ELEMENTS_UNLOCK();
        TP_STATE_UNLOCK();
        set_error("invalid engine element count %d", n);
        return -1;
    }

    overlay_clear_elements(overlay);
    for (int i = 0; i < n; i++) {
        const TouchElement *src = &g_state.elements[i];
        if (src->type != BUTTON && src->type != D_PAD) {
            set_error("unsupported element type %u at index %d",
                      (unsigned)src->type, i);
            goto out_locked;
        }

        Wc3OverlayElement dst;
        memset(&dst, 0, sizeof(dst));
        dst.type = (src->type == D_PAD) ? WC3_OVERLAY_ELEMENT_DPAD
                                        : WC3_OVERLAY_ELEMENT_BUTTON;
        /* Engine and renderer use opposite shape enum values. */
        dst.shape = (src->shape == RECT) ? WC3_OVERLAY_SHAPE_RECT
                                         : WC3_OVERLAY_SHAPE_CIRCLE;
        dst.nx = (float)src->x / (float)overlay->width;
        dst.ny = (float)src->y / (float)overlay->height;
        dst.width_units = src->w;
        dst.height_units = src->h;
        dst.scale = src->scale;
        dst.corner_radius = src->corner_radius;
        dst.dpad_corner_radius = src->dpad_corner_radius;
        dst.icon_id = src->icon_id;
        copy_label(dst.text, sizeof(dst.text), src->display_text);
        dst.visual_layers = visual_layers_from_engine(src->visual_layers);
        dst.alpha_outer = (uint8_t)(src->visual_alpha >> 24);
        dst.alpha_stroke = (uint8_t)(src->visual_alpha >> 16);
        dst.alpha_fill = (uint8_t)(src->visual_alpha >> 8);
        dst.alpha_glow = (uint8_t)(src->visual_alpha);
        dst.engaged = (src->flags & TOUCH_BIT_ENGAGED) != 0;
        dst.selected = (src->flags & TOUCH_BIT_SELECTED) != 0;
        dst.active_petals = active_petals_from_engine(src);
        /* The product opacity is one value for every control element, so it
         * replaces the authored per-element opacity instead of competing with
         * it.  Without the setting the element keeps the profile value and
         * the overlay style decides. */
        if (g_opacity_percent > 0) {
            dst.has_opacity = true;
            dst.opacity = (float)g_opacity_percent / 100.0f;
        }

        if (overlay_add_element(overlay, &dst) < 0) {
            set_error("overlay add failed at index %d", i);
            goto out_locked;
        }
    }
    overlay_commit(overlay);
    rc = 0;

out_locked:
    ELEMENTS_UNLOCK();
    TP_STATE_UNLOCK();

    if (rc == 0) {
        overlay->layout_id = g_layout_id;
        /* add_element() promotes zero alphas to 255; restore engine values. */
        controls_sync_visuals(overlay);
        clear_error();
    }
    return rc;
}

int controls_visual_dirty(void) {
    if (!g_initialized) return 0;
    int dirty = 0;
    TP_STATE_LOCK();
    for (int i = 0; i < DIRTY_MASK_WORDS; i++) {
        if (g_state.visual_dirty_mask[i] != 0) {
            dirty = 1;
            break;
        }
    }
    TP_STATE_UNLOCK();
    return dirty;
}

int controls_sync_visuals(Wc3Overlay *overlay) {
    if (!overlay || !g_initialized) return -1;
    if (overlay->layout_id != g_layout_id) return -1;

    Wc3OverlayElement snapshot[WC3_OVERLAY_MAX_ELEMENTS];
    memset(snapshot, 0, sizeof(snapshot));
    int n = 0;
    bool valid = false;

    TP_STATE_LOCK();
    ELEMENTS_LOCK();
    n = g_state.element_count;
    valid = (n >= 0 && n <= WC3_OVERLAY_MAX_ELEMENTS &&
             n == overlay->element_count);
    for (int i = 0; valid && i < n; i++) {
        const TouchElement *src = &g_state.elements[i];
        Wc3OverlayElement *dst = &snapshot[i];
        dst->visual_layers = visual_layers_from_engine(src->visual_layers);
        dst->alpha_outer = (uint8_t)(src->visual_alpha >> 24);
        dst->alpha_stroke = (uint8_t)(src->visual_alpha >> 16);
        dst->alpha_fill = (uint8_t)(src->visual_alpha >> 8);
        dst->alpha_glow = (uint8_t)(src->visual_alpha);
        dst->engaged = (src->flags & TOUCH_BIT_ENGAGED) != 0;
        dst->selected = (src->flags & TOUCH_BIT_SELECTED) != 0;
        dst->active_petals = active_petals_from_engine(src);
    }
    if (valid)
        memset(g_state.visual_dirty_mask, 0, sizeof(g_state.visual_dirty_mask));
    ELEMENTS_UNLOCK();
    TP_STATE_UNLOCK();

#if WC3_DEBUG && WC3_DEBUG_INPUT
    {
        static int last_n = -1;
        static int last_valid = -1;
        static int last_layers = -1;
        int active = 0;
        for (int i = 0; valid && i < n; i++) {
            if (snapshot[i].visual_layers != 0) active++;
        }
        if (n != last_n || (int)valid != last_valid || active != last_layers) {
            last_n = n;
            last_valid = (int)valid;
            last_layers = active;
            WC3_CTRL_LOG("sync n=%d overlay=%d valid=%d active=%d", n,
                         overlay->element_count, (int)valid, active);
        }
    }
#endif

    if (!valid) return -1;

    for (int i = 0; i < n; i++) {
        Wc3OverlayElement *dst = &overlay->elements[i];
        const Wc3OverlayElement *src = &snapshot[i];
        dst->visual_layers = src->visual_layers;
        dst->alpha_outer = src->alpha_outer;
        dst->alpha_stroke = src->alpha_stroke;
        dst->alpha_fill = src->alpha_fill;
        dst->alpha_glow = src->alpha_glow;
        dst->engaged = src->engaged;
        dst->selected = src->selected;
        dst->active_petals = src->active_petals;
    }
    return 0;
}

const char *controls_last_error(void) {
    return g_error[0] ? g_error : "no error";
}
