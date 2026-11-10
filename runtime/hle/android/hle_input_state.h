#ifndef WC3_HLE_INPUT_STATE_H
#define WC3_HLE_INPUT_STATE_H

/*
 * Pure Win32 input state and key layout helpers shared by hle_android.c and
 * the host tests.  This header deliberately has no Android or guest
 * dependencies: the HLE owns the message queue, this file owns the state that
 * GetKeyState reads and the scancode/extended bits that go into lParam.
 *
 * The key state is updated when a WM_KEYDOWN/WM_KEYUP is dispatched, not when
 * the action is enqueued.  That mirrors the existing mouse release rule: a
 * short tap queues DOWN and UP before the game tick, and the window procedure
 * must observe DOWN inside the DOWN message and UP inside the UP message.
 */

#include <stdint.h>
#include <string.h>

#define HLE_VK_LBUTTON   0x01
#define HLE_VK_RBUTTON   0x02
#define HLE_VK_CANCEL    0x03
#define HLE_VK_MBUTTON   0x04
#define HLE_VK_XBUTTON1  0x05
#define HLE_VK_XBUTTON2  0x06
#define HLE_VK_BACK      0x08
#define HLE_VK_TAB       0x09
#define HLE_VK_CLEAR     0x0C
#define HLE_VK_RETURN    0x0D
#define HLE_VK_SHIFT     0x10
#define HLE_VK_CONTROL   0x11
#define HLE_VK_MENU      0x12
#define HLE_VK_PAUSE     0x13
#define HLE_VK_CAPITAL   0x14
#define HLE_VK_ESCAPE    0x1B
#define HLE_VK_SPACE     0x20
#define HLE_VK_PRIOR     0x21
#define HLE_VK_NEXT      0x22
#define HLE_VK_END       0x23
#define HLE_VK_HOME      0x24
#define HLE_VK_LEFT      0x25
#define HLE_VK_UP        0x26
#define HLE_VK_RIGHT     0x27
#define HLE_VK_DOWN      0x28
#define HLE_VK_SELECT    0x29
#define HLE_VK_PRINT     0x2A
#define HLE_VK_EXECUTE   0x2B
#define HLE_VK_SNAPSHOT  0x2C
#define HLE_VK_INSERT    0x2D
#define HLE_VK_DELETE    0x2E
#define HLE_VK_HELP      0x2F
#define HLE_VK_LWIN      0x5B
#define HLE_VK_RWIN      0x5C
#define HLE_VK_APPS      0x5D
#define HLE_VK_SLEEP     0x5F
#define HLE_VK_NUMPAD0   0x60
#define HLE_VK_NUMPAD1   0x61
#define HLE_VK_NUMPAD2   0x62
#define HLE_VK_NUMPAD3   0x63
#define HLE_VK_NUMPAD4   0x64
#define HLE_VK_NUMPAD5   0x65
#define HLE_VK_NUMPAD6   0x66
#define HLE_VK_NUMPAD7   0x67
#define HLE_VK_NUMPAD8   0x68
#define HLE_VK_NUMPAD9   0x69
#define HLE_VK_MULTIPLY  0x6A
#define HLE_VK_ADD       0x6B
#define HLE_VK_SEPARATOR 0x6C
#define HLE_VK_SUBTRACT  0x6D
#define HLE_VK_DECIMAL   0x6E
#define HLE_VK_DIVIDE    0x6F
#define HLE_VK_F1        0x70
#define HLE_VK_F2        0x71
#define HLE_VK_F3        0x72
#define HLE_VK_F4        0x73
#define HLE_VK_F5        0x74
#define HLE_VK_F6        0x75
#define HLE_VK_F7        0x76
#define HLE_VK_F8        0x77
#define HLE_VK_F9        0x78
#define HLE_VK_F10       0x79
#define HLE_VK_F11       0x7A
#define HLE_VK_F12       0x7B
#define HLE_VK_NUMLOCK   0x90
#define HLE_VK_SCROLL    0x91
#define HLE_VK_LSHIFT    0xA0
#define HLE_VK_RSHIFT    0xA1
#define HLE_VK_LCONTROL  0xA2
#define HLE_VK_RCONTROL  0xA3
#define HLE_VK_LMENU     0xA4
#define HLE_VK_RMENU     0xA5
#define HLE_VK_OEM_1     0xBA
#define HLE_VK_OEM_PLUS  0xBB
#define HLE_VK_OEM_COMMA 0xBC
#define HLE_VK_OEM_MINUS 0xBD
#define HLE_VK_OEM_PERIOD 0xBE
#define HLE_VK_OEM_2     0xBF
#define HLE_VK_OEM_3     0xC0
#define HLE_VK_OEM_4     0xDB
#define HLE_VK_OEM_5     0xDC
#define HLE_VK_OEM_6     0xDD
#define HLE_VK_OEM_7     0xDE
#define HLE_VK_OEM_8     0xDF
#define HLE_VK_OEM_102   0xE2

typedef struct {
    uint8_t down[256];
    uint8_t toggled[256];
} HleKeyState;

static inline void hle_key_state_reset(HleKeyState *state)
{
    if (state) memset(state, 0, sizeof(*state));
}

static inline int hle_key_message_vk(int vk)
{
    switch (vk) {
    case HLE_VK_LCONTROL:
    case HLE_VK_RCONTROL:
        return HLE_VK_CONTROL;
    case HLE_VK_LSHIFT:
    case HLE_VK_RSHIFT:
        return HLE_VK_SHIFT;
    case HLE_VK_LMENU:
    case HLE_VK_RMENU:
        return HLE_VK_MENU;
    default:
        return vk;
    }
}

static inline int hle_key_scan(int vk)
{
    switch (vk) {
    case HLE_VK_ESCAPE: return 0x01;
    case '1': return 0x02;
    case '2': return 0x03;
    case '3': return 0x04;
    case '4': return 0x05;
    case '5': return 0x06;
    case '6': return 0x07;
    case '7': return 0x08;
    case '8': return 0x09;
    case '9': return 0x0A;
    case '0': return 0x0B;
    case HLE_VK_OEM_MINUS: return 0x0C;
    case HLE_VK_OEM_PLUS: return 0x0D;
    case HLE_VK_BACK: return 0x0E;
    case HLE_VK_TAB: return 0x0F;
    case 'Q': return 0x10;
    case 'W': return 0x11;
    case 'E': return 0x12;
    case 'R': return 0x13;
    case 'T': return 0x14;
    case 'Y': return 0x15;
    case 'U': return 0x16;
    case 'I': return 0x17;
    case 'O': return 0x18;
    case 'P': return 0x19;
    case HLE_VK_OEM_4: return 0x1A;
    case HLE_VK_OEM_6: return 0x1B;
    case HLE_VK_RETURN: return 0x1C;
    case HLE_VK_LCONTROL:
    case HLE_VK_RCONTROL:
    case HLE_VK_CONTROL: return 0x1D;
    case 'A': return 0x1E;
    case 'S': return 0x1F;
    case 'D': return 0x20;
    case 'F': return 0x21;
    case 'G': return 0x22;
    case 'H': return 0x23;
    case 'J': return 0x24;
    case 'K': return 0x25;
    case 'L': return 0x26;
    case HLE_VK_OEM_1: return 0x27;
    case HLE_VK_OEM_7: return 0x28;
    case HLE_VK_OEM_3: return 0x29;
    case HLE_VK_LSHIFT:
    case HLE_VK_SHIFT: return 0x2A;
    case HLE_VK_OEM_5: return 0x2B;
    case 'Z': return 0x2C;
    case 'X': return 0x2D;
    case 'C': return 0x2E;
    case 'V': return 0x2F;
    case 'B': return 0x30;
    case 'N': return 0x31;
    case 'M': return 0x32;
    case HLE_VK_OEM_COMMA: return 0x33;
    case HLE_VK_OEM_PERIOD: return 0x34;
    case HLE_VK_DIVIDE: return 0x35;
    case HLE_VK_OEM_2: return 0x35;
    case HLE_VK_RSHIFT: return 0x36;
    case HLE_VK_MULTIPLY: return 0x37;
    case HLE_VK_LMENU:
    case HLE_VK_RMENU:
    case HLE_VK_MENU: return 0x38;
    case HLE_VK_SPACE: return 0x39;
    case HLE_VK_CAPITAL: return 0x3A;
    case HLE_VK_F1: return 0x3B;
    case HLE_VK_F2: return 0x3C;
    case HLE_VK_F3: return 0x3D;
    case HLE_VK_F4: return 0x3E;
    case HLE_VK_F5: return 0x3F;
    case HLE_VK_F6: return 0x40;
    case HLE_VK_F7: return 0x41;
    case HLE_VK_F8: return 0x42;
    case HLE_VK_F9: return 0x43;
    case HLE_VK_F10: return 0x44;
    case HLE_VK_NUMLOCK: return 0x45;
    case HLE_VK_SCROLL: return 0x46;
    case HLE_VK_NUMPAD7: return 0x47;
    case HLE_VK_NUMPAD8: return 0x48;
    case HLE_VK_NUMPAD9: return 0x49;
    case HLE_VK_SUBTRACT: return 0x4A;
    case HLE_VK_NUMPAD4: return 0x4B;
    case HLE_VK_NUMPAD5: return 0x4C;
    case HLE_VK_NUMPAD6: return 0x4D;
    case HLE_VK_ADD: return 0x4E;
    case HLE_VK_NUMPAD1: return 0x4F;
    case HLE_VK_NUMPAD2: return 0x50;
    case HLE_VK_NUMPAD3: return 0x51;
    case HLE_VK_NUMPAD0: return 0x52;
    case HLE_VK_DECIMAL: return 0x53;
    case HLE_VK_F11: return 0x57;
    case HLE_VK_F12: return 0x58;
    case HLE_VK_LWIN: return 0x5B;
    case HLE_VK_RWIN: return 0x5C;
    case HLE_VK_APPS: return 0x5D;
    case HLE_VK_SLEEP: return 0x5F;
    case HLE_VK_HOME: return 0x47;
    case HLE_VK_UP: return 0x48;
    case HLE_VK_PRIOR: return 0x49;
    case HLE_VK_LEFT: return 0x4B;
    case HLE_VK_RIGHT: return 0x4D;
    case HLE_VK_END: return 0x4F;
    case HLE_VK_DOWN: return 0x50;
    case HLE_VK_NEXT: return 0x51;
    case HLE_VK_INSERT: return 0x52;
    case HLE_VK_DELETE: return 0x53;
    case HLE_VK_SNAPSHOT: return 0x37;
    default: return 0;
    }
}

static inline int hle_key_is_extended(int vk)
{
    switch (vk) {
    case HLE_VK_RCONTROL:
    case HLE_VK_RMENU:
    case HLE_VK_UP:
    case HLE_VK_DOWN:
    case HLE_VK_LEFT:
    case HLE_VK_RIGHT:
    case HLE_VK_HOME:
    case HLE_VK_END:
    case HLE_VK_PRIOR:
    case HLE_VK_NEXT:
    case HLE_VK_INSERT:
    case HLE_VK_DELETE:
    case HLE_VK_DIVIDE:
    case HLE_VK_SNAPSHOT:
    case HLE_VK_LWIN:
    case HLE_VK_RWIN:
    case HLE_VK_APPS:
        return 1;
    default:
        return 0;
    }
}

static inline uint32_t hle_key_lparam(int vk, int down)
{
    uint32_t lp = 1u | ((uint32_t)(hle_key_scan(vk) & 0xFF) << 16);
    if (hle_key_is_extended(vk)) lp |= 1u << 24;
    if (!down) lp |= (1u << 30) | (1u << 31);
    return lp;
}

static inline void hle_key_update_generic(HleKeyState *state, int generic)
{
    int left = 0, right = 0;
    switch (generic) {
    case HLE_VK_CONTROL:
        left = state->down[HLE_VK_LCONTROL];
        right = state->down[HLE_VK_RCONTROL];
        break;
    case HLE_VK_SHIFT:
        left = state->down[HLE_VK_LSHIFT];
        right = state->down[HLE_VK_RSHIFT];
        break;
    case HLE_VK_MENU:
        left = state->down[HLE_VK_LMENU];
        right = state->down[HLE_VK_RMENU];
        break;
    default:
        return;
    }
    state->down[generic] = (uint8_t)(left || right);
}

static inline void hle_key_state_set(HleKeyState *state, int vk, int down)
{
    if (!state || vk < 0 || vk > 255) return;

    if (down && !state->down[vk] &&
        (vk == HLE_VK_CAPITAL || vk == HLE_VK_NUMLOCK ||
         vk == HLE_VK_SCROLL)) {
        state->toggled[vk] ^= 1;
    }

    state->down[vk] = down ? 1 : 0;

    switch (vk) {
    case HLE_VK_LCONTROL:
    case HLE_VK_RCONTROL:
        hle_key_update_generic(state, HLE_VK_CONTROL);
        break;
    case HLE_VK_LSHIFT:
    case HLE_VK_RSHIFT:
        hle_key_update_generic(state, HLE_VK_SHIFT);
        break;
    case HLE_VK_LMENU:
    case HLE_VK_RMENU:
        hle_key_update_generic(state, HLE_VK_MENU);
        break;
    case HLE_VK_CONTROL:
        state->down[HLE_VK_LCONTROL] = down ? 1 : 0;
        state->down[HLE_VK_RCONTROL] = down ? 1 : 0;
        break;
    case HLE_VK_SHIFT:
        state->down[HLE_VK_LSHIFT] = down ? 1 : 0;
        state->down[HLE_VK_RSHIFT] = down ? 1 : 0;
        break;
    case HLE_VK_MENU:
        state->down[HLE_VK_LMENU] = down ? 1 : 0;
        state->down[HLE_VK_RMENU] = down ? 1 : 0;
        break;
    default:
        break;
    }
}

static inline void hle_key_state_set_message(HleKeyState *state, int msg_vk,
                                             uint32_t lp, int down)
{
    int scan = (int)((lp >> 16) & 0xFF);
    int extended = (int)((lp >> 24) & 1);
    int specific = msg_vk;

    switch (msg_vk) {
    case HLE_VK_CONTROL:
        specific = extended ? HLE_VK_RCONTROL : HLE_VK_LCONTROL;
        break;
    case HLE_VK_MENU:
        specific = extended ? HLE_VK_RMENU : HLE_VK_LMENU;
        break;
    case HLE_VK_SHIFT:
        specific = scan == 0x36 ? HLE_VK_RSHIFT : HLE_VK_LSHIFT;
        break;
    default:
        break;
    }
    hle_key_state_set(state, specific, down);
}

static inline uint16_t hle_key_state_get(const HleKeyState *state, int vk)
{
    if (!state || vk < 0 || vk > 255) return 0;

    int down = state->down[vk] != 0;
    switch (vk) {
    case HLE_VK_CONTROL:
        down = state->down[HLE_VK_LCONTROL] || state->down[HLE_VK_RCONTROL];
        break;
    case HLE_VK_SHIFT:
        down = state->down[HLE_VK_LSHIFT] || state->down[HLE_VK_RSHIFT];
        break;
    case HLE_VK_MENU:
        down = state->down[HLE_VK_LMENU] || state->down[HLE_VK_RMENU];
        break;
    default:
        break;
    }
    return (uint16_t)((down ? 0x8000u : 0u) | (state->toggled[vk] ? 1u : 0u));
}

/*
 * Return the text a WM_KEYDOWN for `vk` should translate into, using the key
 * state that belongs to that message.  This is the US layout the game's own
 * hotkeys use.  Control and Alt combinations never produce text, Shift and
 * Caps Lock choose letter case, and editing/navigation keys return zero
 * because they are posted as explicit WM_KEYDOWN/WM_CHAR pairs elsewhere.
 *
 * The character is a UTF-16 code unit, which is what WM_CHAR carries.
 */
static inline uint16_t hle_key_char_for_state(const HleKeyState *state, int vk)
{
    int shift = 0, control = 0, alt = 0, caps = 0, numlock = 0;
    if (state) {
        shift = state->down[HLE_VK_SHIFT] || state->down[HLE_VK_LSHIFT] ||
                state->down[HLE_VK_RSHIFT];
        control = state->down[HLE_VK_CONTROL] ||
                  state->down[HLE_VK_LCONTROL] ||
                  state->down[HLE_VK_RCONTROL];
        alt = state->down[HLE_VK_MENU] || state->down[HLE_VK_LMENU] ||
              state->down[HLE_VK_RMENU];
        caps = state->toggled[HLE_VK_CAPITAL] != 0;
        numlock = state->toggled[HLE_VK_NUMLOCK] != 0;
    }
    if (control || alt) return 0;

    if (vk >= 'A' && vk <= 'Z') {
        int upper = shift ^ caps;
        return (uint16_t)(upper ? vk : vk + ('a' - 'A'));
    }
    if (vk >= '0' && vk <= '9') {
        static const char shifted_digits[] = ")!@#$%^&*(";
        return (uint16_t)(shift ? shifted_digits[vk - '0'] : vk);
    }
    if (vk >= HLE_VK_NUMPAD0 && vk <= HLE_VK_NUMPAD9) {
        /* With NumLock off, or with Shift held, the numpad is a navigation
         * pad.  The HLE does not synthesise those navigation keys. */
        if (!numlock || shift) return 0;
        return (uint16_t)('0' + (vk - HLE_VK_NUMPAD0));
    }

    switch (vk) {
    case HLE_VK_SPACE:      return ' ';
    case HLE_VK_OEM_1:      return (uint16_t)(shift ? ':' : ';');
    case HLE_VK_OEM_PLUS:   return (uint16_t)(shift ? '+' : '=');
    case HLE_VK_OEM_COMMA:  return (uint16_t)(shift ? '<' : ',');
    case HLE_VK_OEM_MINUS:  return (uint16_t)(shift ? '_' : '-');
    case HLE_VK_OEM_PERIOD: return (uint16_t)(shift ? '>' : '.');
    case HLE_VK_OEM_2:      return (uint16_t)(shift ? '?' : '/');
    case HLE_VK_OEM_3:      return (uint16_t)(shift ? '~' : '`');
    case HLE_VK_OEM_4:      return (uint16_t)(shift ? '{' : '[');
    case HLE_VK_OEM_5:      return (uint16_t)(shift ? '|' : '\\');
    case HLE_VK_OEM_6:      return (uint16_t)(shift ? '}' : ']');
    case HLE_VK_OEM_7:      return (uint16_t)(shift ? '"' : '\'');
    case HLE_VK_OEM_102:    return (uint16_t)(shift ? '|' : '\\');
    case HLE_VK_MULTIPLY:   return '*';
    case HLE_VK_ADD:        return '+';
    case HLE_VK_SUBTRACT:   return '-';
    case HLE_VK_DECIMAL:    return '.';
    case HLE_VK_DIVIDE:     return '/';
    default:                return 0;
    }
}

/*
 * Decode one UTF-16 code unit from Java's modified UTF-8.  GameTextInput
 * documents text_UTF8 as modified UTF-8: U+0000 is C0 80 and supplementary
 * characters are encoded as two three-byte surrogate sequences.  That maps
 * exactly onto the WM_CHAR stream this file generates.
 *
 * Returns bytes consumed, or zero at the end of the buffer.  Invalid bytes
 * are returned as one unit so a diff can always make progress.
 */
static inline int hle_mutf8_next(const char *text, int length, int offset,
                                 uint16_t *unit)
{
    if (!text || length <= 0 || offset < 0 || offset >= length)
        return 0;

    const unsigned char *p = (const unsigned char *)text + offset;
    unsigned char b = p[0];
    if (b < 0x80) {
        if (unit) *unit = b;
        return 1;
    }
    if (b == 0xC0 && offset + 1 < length && p[1] == 0x80) {
        if (unit) *unit = 0;
        return 2;
    }
    if ((b & 0xE0) == 0xC0 && offset + 1 < length &&
        (p[1] & 0xC0) == 0x80) {
        if (unit) *unit = (uint16_t)(((b & 0x1F) << 6) | (p[1] & 0x3F));
        return 2;
    }
    if ((b & 0xF0) == 0xE0 && offset + 2 < length &&
        (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        if (unit)
            *unit = (uint16_t)(((b & 0x0F) << 12) |
                               ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
        return 3;
    }
    if (unit) *unit = b;
    return 1;
}

static inline int hle_mutf8_count(const char *text, int length, int offset)
{
    int count = 0;
    while (offset < length) {
        int step = hle_mutf8_next(text, length, offset, NULL);
        if (step <= 0) break;
        offset += step;
        count++;
    }
    return count;
}

typedef struct {
    /* Byte offsets of the first character that differs. */
    int old_start;
    int new_start;
    /* UTF-16 units to delete from the old text and to append from new_text. */
    int removed;
    int added;
} HleTextDiff;

/*
 * The editor always owns the tail of the field, so the old Java bridge
 * represented every change as "delete the old suffix, then type the new
 * suffix".  Keep that rule but compare decoded code units, not bytes, so a
 * multi-byte character is one backspace and one WM_CHAR.
 */
static inline void hle_text_diff(const char *old_text, int old_length,
                                 const char *new_text, int new_length,
                                 HleTextDiff *diff)
{
    if (!diff) return;

    int old_pos = 0, new_pos = 0;
    for (;;) {
        uint16_t old_unit = 0, new_unit = 0;
        int old_step = hle_mutf8_next(old_text, old_length, old_pos, &old_unit);
        int new_step = hle_mutf8_next(new_text, new_length, new_pos, &new_unit);
        if (!old_step && !new_step)
            break;
        if (!old_step || !new_step || old_unit != new_unit)
            break;
        old_pos += old_step;
        new_pos += new_step;
    }

    diff->old_start = old_pos;
    diff->new_start = new_pos;
    diff->removed = hle_mutf8_count(old_text, old_length, old_pos);
    diff->added = hle_mutf8_count(new_text, new_length, new_pos);
}

#endif /* WC3_HLE_INPUT_STATE_H */
