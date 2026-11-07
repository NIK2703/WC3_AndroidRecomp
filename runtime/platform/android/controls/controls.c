#include "controls.h"

#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wc3_debug.h"

#if WC3_DEBUG && WC3_DEBUG_INPUT
#include <android/log.h>
#define WC3_CTRL_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "Wc3Controls", __VA_ARGS__)
#else
#define WC3_CTRL_LOG(...) ((void)0)
#endif

#include "core/processor.h"
#include "core/state.h"
#include "profile/bind.h"
#include "profile/convert.h"
#include "profile/loader.h"

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
static int g_width;
static int g_height;
static uint64_t g_last_engine_tick;
static bool g_initialized;
static char g_error[256];
static bool g_key_down[256];
static bool g_button_down[8];

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

    if (filtered_count > 0) {
        g_sink(filtered, filtered_count, g_sink_user);
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

static int controls_init_internal(const char *profile_json, int width,
                                  int height) {
    ProfileData *profile = NULL;
    TouchProcessorConfig *config = NULL;
    TouchElement *elements = NULL;
    int rc = -1;

    if (!profile_json || width <= 0 || height <= 0) {
        set_error("invalid init arguments");
        return -1;
    }

    profile = (ProfileData *)calloc(1, sizeof(*profile));
    config = (TouchProcessorConfig *)calloc(1, sizeof(*config));
    elements = (TouchElement *)calloc(MAX_ELEMENTS, sizeof(*elements));
    if (!profile || !config || !elements) {
        set_error("out of memory during init");
        goto out;
    }

    if (profile_load_from_string(profile_json, profile) != 0) {
        const char *detail = profile_last_error();
        set_error("profile parse failed: %s", detail ? detail : "unknown");
        goto out;
    }

    g_timer_interval_ms = profile->timer_interval_ms > 0
                              ? profile->timer_interval_ms
                              : 16;

    profile_to_config(profile, width, height, 1.0f, config);
    touch_processor_init(config);
    reset_action_filter();

    int snap = width / 100;
    if (snap <= 0) snap = 10;
    touch_processor_set_snapping_size((float)snap);

    int count = profile_to_elements(profile, width, height, elements,
                                    MAX_ELEMENTS);
    if (count <= 0) {
        set_error("profile contains no elements");
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

    touch_processor_set_elements(elements, count);
    touch_processor_set_resolution_scale(1.0f);
    touch_processor_set_xform_scale(1.0f, 1.0f);
    touch_processor_set_sim_touch_screen(false);

    g_width = width;
    g_height = height;
    g_last_engine_tick = 0;
    g_initialized = true;
    clear_error();
    rc = 0;

out:
    free(elements);
    free(config);
    free(profile);
    if (rc != 0) g_initialized = false;
    return rc;
}

int controls_init(const char *profile_json, int width, int height) {
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

    if (g_initialized) {
        controls_cancel_all();
    }

    if (controls_init_internal(profile_json, width, height) != 0) {
        free(copy);
        return -1;
    }

    free(g_profile_json);
    g_profile_json = copy;
    return 0;
}

void controls_set_action_sink(Wc3ActionSink sink, void *user) {
    g_sink = sink;
    g_sink_user = user;
}

static uint64_t engine_tick_from_ms(uint64_t time_ms) {
    int interval = g_timer_interval_ms > 0 ? g_timer_interval_ms : 16;
    uint64_t tick = time_ms / (uint64_t)interval;
    if (tick < g_last_engine_tick) tick = g_last_engine_tick;
    return tick;
}

int controls_touch(Wc3TouchPhase phase, int pointer_id, float x, float y,
                       uint64_t event_time_ms) {
    if (phase == WC3_TOUCH_CANCEL) {
        controls_cancel_all();
        return 0;
    }
    if (!g_initialized) {
        set_error("touch before init");
        return -1;
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

    g_last_engine_tick = tick;
    emit_actions(&result);
    return 0;
}

uint64_t controls_next_deadline_ms(uint64_t now_ms) {
    int interval = g_timer_interval_ms > 0 ? g_timer_interval_ms : 16;
    uint64_t from_last = (g_last_engine_tick + 1u) * (uint64_t)interval;
    uint64_t from_now = (now_ms / (uint64_t)interval + 1u) * (uint64_t)interval;
    return from_last > from_now ? from_last : from_now;
}

void controls_cancel_all(void) {
    if (!g_initialized) return;

    TouchActionResult result;
    memset(&result, 0, sizeof(result));

    /* cancel_all takes ELEMENTS_LOCK itself. */
    TP_STATE_LOCK();
    touch_processor_cancel_all(&result);
    TP_STATE_UNLOCK();

    emit_actions(&result);
}

void controls_resize(int width, int height) {
    if (!g_initialized || !g_profile_json || width <= 0 || height <= 0) {
        return;
    }

    controls_cancel_all();
    if (controls_init_internal(g_profile_json, width, height) != 0) {
        g_initialized = false;
    }
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
    if (n <= 0 || n > WC3_OVERLAY_MAX_ELEMENTS) {
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
        copy_label(dst.text, sizeof(dst.text), src->display_text);
        dst.visual_layers = visual_layers_from_engine(src->visual_layers);
        dst.alpha_outer = (uint8_t)(src->visual_alpha >> 24);
        dst.alpha_stroke = (uint8_t)(src->visual_alpha >> 16);
        dst.alpha_fill = (uint8_t)(src->visual_alpha >> 8);
        dst.alpha_glow = (uint8_t)(src->visual_alpha);
        dst.engaged = (src->flags & TOUCH_BIT_ENGAGED) != 0;
        dst.selected = (src->flags & TOUCH_BIT_SELECTED) != 0;
        dst.active_petals = active_petals_from_engine(src);

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
        /* add_element() promotes zero alphas to 255; restore engine values. */
        controls_sync_visuals(overlay);
        clear_error();
    }
    return rc;
}

int controls_sync_visuals(Wc3Overlay *overlay) {
    if (!overlay || !g_initialized) return -1;

    Wc3OverlayElement snapshot[WC3_OVERLAY_MAX_ELEMENTS];
    memset(snapshot, 0, sizeof(snapshot));
    int n = 0;
    bool valid = false;

    TP_STATE_LOCK();
    ELEMENTS_LOCK();
    n = g_state.element_count;
    valid = (n > 0 && n <= WC3_OVERLAY_MAX_ELEMENTS &&
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

    bool changed = false;
    for (int i = 0; i < n; i++) {
        Wc3OverlayElement *dst = &overlay->elements[i];
        const Wc3OverlayElement *src = &snapshot[i];
        if (dst->visual_layers != src->visual_layers ||
            dst->alpha_outer != src->alpha_outer ||
            dst->alpha_stroke != src->alpha_stroke ||
            dst->alpha_fill != src->alpha_fill ||
            dst->alpha_glow != src->alpha_glow ||
            dst->engaged != src->engaged ||
            dst->selected != src->selected ||
            dst->active_petals != src->active_petals)
            changed = true;

        dst->visual_layers = src->visual_layers;
        dst->alpha_outer = src->alpha_outer;
        dst->alpha_stroke = src->alpha_stroke;
        dst->alpha_fill = src->alpha_fill;
        dst->alpha_glow = src->alpha_glow;
        dst->engaged = src->engaged;
        dst->selected = src->selected;
        dst->active_petals = src->active_petals;
    }
    return changed ? 1 : 0;
}

const char *controls_last_error(void) {
    return g_error[0] ? g_error : "no error";
}
