#include "core/internal.h"
#include "core/timer.h"


static int pointer_button_idx(const TouchBinding* b);
static int schedule_action(TouchBinding binding, int action_type, uint64_t delay_from_now_ticks);

// Keyboard modifier keycodes (X11): 0x25=KEY_CTRL_L(37), 0x32=KEY_SHIFT_L(50), 0x40=KEY_ALT_L(64)
#define MOD_KEYCODE_CTRL  0x25
#define MOD_KEYCODE_SHIFT 0x32
#define MOD_KEYCODE_ALT   0x40
#define MOVE_DIRECTION_COUNT 4

bool is_modifier_binding(const TouchBinding* b) {
    if (is_keyboard_binding(b)) {
        int kc = b->keycode;
        return kc == MOD_KEYCODE_SHIFT || kc == MOD_KEYCODE_CTRL || kc == MOD_KEYCODE_ALT;
    }
    return false;
}

static inline bool is_mouse_button_binding(const TouchBinding* b) {
    return b->type >= MOUSE_LEFT_BUTTON && b->type <= MOUSE_BUTTON5;
}

static inline int action_type_for_binding(const TouchBinding* b) {
    return is_keyboard_binding(b) ? ACT_KEY_PRESS :
           is_mouse_button_binding(b) ? ACT_POINTER_BUTTON_PRESS :
           ACT_GAMEPAD_STATE;
}

static inline int release_action_type_for_binding(const TouchBinding* b) {
    return is_keyboard_binding(b) ? ACT_KEY_RELEASE :
           is_mouse_button_binding(b) ? ACT_POINTER_BUTTON_RELEASE :
           ACT_GAMEPAD_RELEASE;
}

static inline bool should_hold_binding(const TouchBinding* b, bool force_hold) {
    return force_hold || (b->modifiers != 0)
        || (b->flags & (TOUCH_BINDING_FLAG_AUTO_REPEAT | TOUCH_BINDING_FLAG_LAST_IN_PACKAGE));
}

static inline int gamepad_button_index(const TouchBinding* b) {
    return b->type - GAMEPAD_BASE;
}



static int schedule_action(TouchBinding binding, int action_type, uint64_t delay_from_now_ticks) {
    if (g_state.scheduled_action_count >= MAX_SCHEDULED_ACTIONS) {
        return -1;
    }
    int idx = g_state.scheduled_action_count++;
    g_state.scheduled_actions[idx].binding = binding;
    g_state.scheduled_actions[idx].action_type = action_type;
    g_state.scheduled_actions[idx].scheduled_tick = now_tick() + delay_from_now_ticks;
    g_state.scheduled_actions[idx].active = true;
    g_state.scheduled_actions[idx].needs_toggle_record = false;
    return idx;
}

static void compact_scheduled_actions(int old_count);

void release_held_actions(TouchActionResult* restrict result, uint64_t current_tick) {
    (void)current_tick;
    (void)result;
    if (!g_state.gesture_is_action_held) return;

    if (g_state.passthrough_active) {
        g_state.gesture_held_count = 0;
        g_state.gesture_is_action_held = false;
        memset(g_state.gesture_auto_repeat_last_tick_held, 0, sizeof(g_state.gesture_auto_repeat_last_tick_held));
        return;
    }

    // Process in reverse press order (LIFO): LAST pressed → first released.
    // cumulative_delay tracks the delay from now for each RELEASE in order.
    // Updated per iteration so each RELEASE fires after the previous one.
    uint64_t cumulative_delay = 0;
    uint64_t now = now_tick();
    for (int j = g_state.gesture_held_count - 1; j >= 0; j--) {
        TouchBinding* hb = &g_state.gesture_held_actions[j];
        if (hb->type == NONE) continue;

        // Search for this bind's pending DOWN in the schedule
        uint64_t down_at = 0;
        bool down_pending = false;
        for (int s = 0; s < g_state.scheduled_action_count; s++) {
            ScheduledAction* sa = &g_state.scheduled_actions[s];
            if (!sa->active) continue;
            if (sa->binding.type == hb->type &&
                sa->binding.keycode == hb->keycode &&
                (sa->action_type == ACT_KEY_PRESS ||
                 sa->action_type == ACT_POINTER_BUTTON_PRESS ||
                 sa->action_type == ACT_GAMEPAD_STATE)) {
                down_pending = true;
                down_at = sa->scheduled_tick;
                break;
            }
        }

        if (hb->flags & TOUCH_BINDING_FLAG_LAST_IN_PACKAGE) {
            cumulative_delay += g_state.cfg.release_hold_min_gap_ticks;
            if (down_pending) {
                uint64_t needed = (down_at > now) ? (down_at - now + g_state.cfg.release_hold_min_gap_ticks) : g_state.cfg.release_hold_min_gap_ticks;
                if (needed > cumulative_delay) cumulative_delay = needed;
            }
            schedule_action(*hb, release_action_type_for_binding(hb), cumulative_delay);
        } else {
            cumulative_delay += hb->bind_delay_ticks;
            uint64_t release_at = now + cumulative_delay;
            if (down_pending && release_at <= down_at) {
                cumulative_delay = (down_at > now) ? (down_at - now + g_state.cfg.release_hold_min_gap_ticks) : g_state.cfg.release_hold_min_gap_ticks;
            }
            schedule_action(*hb, release_action_type_for_binding(hb), cumulative_delay);
        }
    }

    g_state.gesture_held_count = 0;
    g_state.gesture_is_action_held = false;
    memset(g_state.gesture_auto_repeat_last_tick_held, 0, sizeof(g_state.gesture_auto_repeat_last_tick_held));
}

static bool execute_toggle_actions(TouchActionResult* restrict result, const TouchBinding* b,
                                   int binding_delay_ticks) {
    if (!g_state.gesture_is_down_event) return true;

    bool found = false;
    for (int t = 0; t < g_state.gesture_toggled_count; t++) {
        if (g_state.gesture_toggled_actions[t].type == b->type &&
            g_state.gesture_toggled_actions[t].keycode == b->keycode) {
            found = true;
            break;
        }
    }
    if (found) {
        release_binding(result, b);
        gesture_remove_toggled_action(b->type, b->keycode);
    } else {
        uint64_t toggle_delay_ticks = b->bind_delay_ticks > 0
            ? (uint64_t)b->bind_delay_ticks
            : 0;
        uint64_t toggle_delay = toggle_delay_ticks;
        if (toggle_delay > 0) {
            bool pending = false;
            int si = -1;
            for (si = 0; si < g_state.scheduled_action_count; si++) {
                if (g_state.scheduled_actions[si].active
                    && g_state.scheduled_actions[si].needs_toggle_record
                    && g_state.scheduled_actions[si].binding.type == b->type
                    && g_state.scheduled_actions[si].binding.keycode == b->keycode) {
                    pending = true;
                    break;
                }
            }
            if (pending) {
                release_binding(result, b);
                g_state.scheduled_actions[si].active = false;
                g_state.scheduled_actions[si].needs_toggle_record = false;
            } else {
                int sched_idx = schedule_action(*b, action_type_for_binding(b), toggle_delay);
                if (sched_idx >= 0)
                    g_state.scheduled_actions[sched_idx].needs_toggle_record = true;
            }
        } else {
            press_binding(result, b, true);
            if (g_state.gesture_toggled_count < MAX_HELD_ACTIONS)
                g_state.gesture_toggled_actions[g_state.gesture_toggled_count++] = *b;
        }
    }
    return true;
}

// Schedule all binds with RELEASE-based per-bind delays. Non-pinned binds
// get press+release scheduled inline (self-contained). Only pinned binds
// (sticky/auto_repeat) are added to held_actions for release_held_actions.
// Each bind's DOWN fires after the previous bind's RELEASE + its delay,
// NOT from the start of the package (prevents RELEASE/DOWN collisions).
static void schedule_bind_package(TouchActionResult* restrict result, const TouchBinding* restrict actions, int count) {
    if (g_state.passthrough_active || count == 0) {
        return;
    }

    int binding_delay_ticks = g_state.cfg.binding_delay_ticks;
    uint64_t prev_release_tick = 0;

    for (int i = 0; i < count; i++) {
        const TouchBinding* b = &actions[i];
        if (b->type == NONE) continue;
        if (is_mouse_move_binding(b)) continue;

        // Scroll: immediate press+release (not part of timed sequence)
        if (b->type == MOUSE_SCROLL_UP || b->type == MOUSE_SCROLL_DOWN) {
            press_binding(result, b, false);
            continue;
        }
        // Toggle
        if (b->flags & TOUCH_BINDING_FLAG_TOGGLE) {
            execute_toggle_actions(result, b, binding_delay_ticks);
            continue;
        }

        // Delay from previous RELEASE — accumulated at this point per non-pinned.
        // Global binding_delay applies as spacing between binds (not before first).
        // Minimum gap prevents RELEASE/DOWN collision at zero delay.
        int per_bind_total = b->bind_delay_ticks;
        if (i > 0) {
            per_bind_total += binding_delay_ticks;
            if (per_bind_total <= 0) per_bind_total = g_state.cfg.inter_bind_min_gap_ticks;
        }
        uint64_t down_tick = prev_release_tick + (uint64_t)per_bind_total;

        bool is_pinned = should_hold_binding(b, false);

        {
            if (is_pinned && g_state.gesture_held_count >= MAX_HELD_ACTIONS) {
                continue;
            }
            uint64_t now = now_tick();
            schedule_action(*b, action_type_for_binding(b), down_tick);

            if (is_pinned) {
                g_state.gesture_held_actions[g_state.gesture_held_count++] = *b;
                g_state.gesture_is_action_held = true;
                prev_release_tick = down_tick + g_state.cfg.bind_press_width_ticks;
            } else {
                // RELEASE fires after a configurable press width
                uint64_t release_tick = down_tick + g_state.cfg.bind_press_width_ticks;
                schedule_action(*b, release_action_type_for_binding(b), release_tick);
                prev_release_tick = release_tick;
            }
        }
    }
}

void execute_actions(TouchActionResult* restrict result, const TouchBinding* actions, int count) {
    schedule_bind_package(result, actions, count);
}

void execute_actions_hold(TouchActionResult* restrict result, const TouchBinding* actions, int count) {
    // Drag/scroll mode executes single BINDs, not bind packages.
    // Each bind fires independently with its own per-bind delay (not cumulative).
    // Releases happen on finger-up via release_held_actions.
    if (g_state.passthrough_active || count == 0) {
        return;
    }

    int binding_delay_ticks = g_state.cfg.binding_delay_ticks;
    uint64_t cum_ticks = 0;

    for (int i = 0; i < count; i++) {
        const TouchBinding* b = &actions[i];
        if (b->type == NONE) continue;
        if (is_mouse_move_binding(b)) continue;

        // Scroll: immediate press+release
        if (b->type == MOUSE_SCROLL_UP || b->type == MOUSE_SCROLL_DOWN) {
            press_binding(result, b, false);
            continue;
        }
        // Toggle: existing mechanism
        if (b->flags & TOUCH_BINDING_FLAG_TOGGLE) {
            execute_toggle_actions(result, b, binding_delay_ticks);
            continue;
        }

        // Tick-based: value is already in ticks
        cum_ticks += b->bind_delay_ticks;
        uint64_t delay_ticks = cum_ticks;

        if (g_state.gesture_held_count >= MAX_HELD_ACTIONS) continue;
        schedule_action(*b, action_type_for_binding(b), delay_ticks);

        g_state.gesture_held_actions[g_state.gesture_held_count++] = *b;
        g_state.gesture_is_action_held = true;
    }
}

static int pointer_button_idx(const TouchBinding* b) {
    // C enum: LEFT=1, RIGHT=2, MIDDLE=3, BUTTON4=4, BUTTON5=5
    // Java Pointer.Button.values(): LEFT=0, MIDDLE=1, RIGHT=2, SCROLL_UP=3, SCROLL_DOWN=4
    switch (b->type) {
        case MOUSE_LEFT_BUTTON: return 0;
        case MOUSE_RIGHT_BUTTON: return 2;
        case MOUSE_MIDDLE_BUTTON: return 1;
        case MOUSE_BUTTON4: return 3;
        case MOUSE_BUTTON5: return 4;
        default: return 0; /* MOUSE_LEFT fallback */
    }
}

static void dispatch_binding_action(TouchActionResult* restrict result, const TouchBinding* b, bool is_press) {
    if (b->type == NONE) return;
    switch (b->type) {
        case MOUSE_LEFT_BUTTON:
        case MOUSE_RIGHT_BUTTON:
        case MOUSE_MIDDLE_BUTTON:
        case MOUSE_BUTTON4:
        case MOUSE_BUTTON5: {
            int btn = pointer_button_idx(b);
            add_action(result, is_press ? ACT_POINTER_BUTTON_PRESS : ACT_POINTER_BUTTON_RELEASE, btn, 0, 0);
            break;
        }
        case MOUSE_SCROLL_UP:
            if (is_press) add_action(result, ACT_SCROLL, -1, 0, 0);
            break;
        case MOUSE_SCROLL_DOWN:
            if (is_press) add_action(result, ACT_SCROLL, 1, 0, 0);
            break;
        case MOUSE_MOVE_LEFT:
        case MOUSE_MOVE_RIGHT:
        case MOUSE_MOVE_UP:
        case MOUSE_MOVE_DOWN:
            if (!is_press) add_action(result, ACT_STOP_MOUSE_MOVE, 0, 0, 0);
            break;
        default:
            if (is_gamepad_binding(b)) {
                int btn_idx = gamepad_button_index(b);
                add_action(result, ACT_GAMEPAD_STATE, btn_idx, is_press ? 1 : 0, 0);
            } else if (is_keyboard_binding(b)) {
                add_action(result, is_press ? ACT_KEY_PRESS : ACT_KEY_RELEASE, b->keycode, is_press ? 1 : 0, 0);
            }
            break;
    }
}

void release_binding(TouchActionResult* restrict result, const TouchBinding* b) {
    dispatch_binding_action(result, b, false);
}

void press_binding(TouchActionResult* restrict result, const TouchBinding* b, bool hold) {
    if (b->type == NONE) return;
    if (is_mouse_move_binding(b)) {
        static const int move_dx[] = { -1, 1, 0, 0 };
        static const int move_dy[] = { 0, 0, -1, 1 };
        int idx = b->type - MOUSE_MOVE_LEFT;
        if (idx >= 0 && idx < MOVE_DIRECTION_COUNT) {
            add_action(result, ACT_START_MOUSE_MOVE, move_dx[idx], move_dy[idx], hold ? 1 : 0);
            if (!hold) { add_action(result, ACT_STOP_MOUSE_MOVE, 0, 0, 0); }
        }
        return;
    }
    dispatch_binding_action(result, b, true);
    if (!hold) { dispatch_binding_action(result, b, false); }
}

static void compact_scheduled_actions(int old_count) {
    ScheduledAction* sched_actions = g_state.scheduled_actions;
    int write_idx = 0;
    for (int i = 0; i < g_state.scheduled_action_count; i++) {
        if (sched_actions[i].active) {
            if (write_idx != i)
                sched_actions[write_idx] = sched_actions[i];
            write_idx++;
        }
    }
    g_state.scheduled_action_count = write_idx;
    for (int i = write_idx; i < old_count; i++) {
        sched_actions[i].active = false;
        sched_actions[i].binding.type = NONE;
    }
}

void process_scheduled_actions(TouchActionResult* restrict result, uint64_t now_tick) {
    if (g_state.scheduled_action_count == 0) return;
    // Compact inactive entries first so sort only sees active entries.
    compact_scheduled_actions(g_state.scheduled_action_count);
    int old_count = g_state.scheduled_action_count;
    if (old_count == 0) return;
    // Stable sort by scheduled_tick so cross-key actions fire in
    // chronological order regardless of insertion order.
    // After compaction, all entries are active — no inactive barrier issue.
    ScheduledAction* sched_actions = g_state.scheduled_actions;
    for (int i = 1; i < old_count; i++) {
        ScheduledAction key = sched_actions[i];
        int j = i - 1;
        while (j >= 0 && sched_actions[j].scheduled_tick > key.scheduled_tick) {
            sched_actions[j + 1] = sched_actions[j];
            j--;
        }
        sched_actions[j + 1] = key;
    }
    int fired_count = 0;
    int pending_count = 0;
    for (int i = 0; i < old_count; i++) {
        ScheduledAction* sa = &sched_actions[i];
        if (!sa->active) continue;
        pending_count++;
        if (now_tick >= sa->scheduled_tick) {
            sa->active = false;
            fired_count++;
            switch (sa->action_type) {
                case ACT_KEY_PRESS:
                case ACT_POINTER_BUTTON_PRESS:
                case ACT_GAMEPAD_STATE:
                    press_binding(result, &sa->binding, true);
                    break;
                case ACT_KEY_RELEASE:
                case ACT_POINTER_BUTTON_RELEASE:
                case ACT_GAMEPAD_RELEASE:
                    release_binding(result, &sa->binding);
                    break;
            }
            if (sa->needs_toggle_record) {
                if (g_state.gesture_toggled_count < MAX_HELD_ACTIONS)
                    g_state.gesture_toggled_actions[g_state.gesture_toggled_count++] = sa->binding;
                sa->needs_toggle_record = false;
            }
        }
    }
    if (fired_count > 0) {
        compact_scheduled_actions(old_count);
    }
}
