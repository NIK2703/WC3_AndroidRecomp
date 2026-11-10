/*
 * overlay_actions_legacy.c - TouchAction to HLE bridge.
 *
 * This is the only UI-side file that translates Winlator X11 keycodes and
 * pointer buttons into the guest input surface.  It calls runtime/hle through
 * wc3_legacy_* only, so the overlay module itself remains independent of the
 * as-built implementation.
 */
#include "overlay_actions_legacy.h"

#include "hle_bridge.h"

/* Standard Windows virtual-key values.  The overlay action layer is the only
 * UI-side owner of this translation; the HLE bridge forwards the result to
 * the as-built HLE without exposing an as-built header here. */
#define WC3_VK_ADD        0x6B
#define WC3_VK_BACK       0x08
#define WC3_VK_CAPITAL    0x14
#define WC3_VK_DECIMAL    0x6E
#define WC3_VK_DELETE     0x2E
#define WC3_VK_DIVIDE     0x6F
#define WC3_VK_DOWN       0x28
#define WC3_VK_END        0x23
#define WC3_VK_ESCAPE     0x1B
#define WC3_VK_F1         0x70
#define WC3_VK_F10        0x79
#define WC3_VK_F11        0x7A
#define WC3_VK_F12        0x7B
#define WC3_VK_F2         0x71
#define WC3_VK_F3         0x72
#define WC3_VK_F4         0x73
#define WC3_VK_F5         0x74
#define WC3_VK_F6         0x75
#define WC3_VK_F7         0x76
#define WC3_VK_F8         0x77
#define WC3_VK_F9         0x78
#define WC3_VK_HOME       0x24
#define WC3_VK_INSERT     0x2D
#define WC3_VK_LCONTROL   0xA2
#define WC3_VK_LEFT       0x25
#define WC3_VK_LMENU      0xA4
#define WC3_VK_LSHIFT     0xA0
#define WC3_VK_MULTIPLY   0x6A
#define WC3_VK_NEXT       0x22
#define WC3_VK_NUMLOCK    0x90
#define WC3_VK_NUMPAD0    0x60
#define WC3_VK_NUMPAD1    0x61
#define WC3_VK_NUMPAD2    0x62
#define WC3_VK_NUMPAD3    0x63
#define WC3_VK_NUMPAD4    0x64
#define WC3_VK_NUMPAD5    0x65
#define WC3_VK_NUMPAD6    0x66
#define WC3_VK_NUMPAD7    0x67
#define WC3_VK_NUMPAD8    0x68
#define WC3_VK_NUMPAD9    0x69
#define WC3_VK_OEM_1      0xBA
#define WC3_VK_OEM_2      0xBF
#define WC3_VK_OEM_3      0xC0
#define WC3_VK_OEM_4      0xDB
#define WC3_VK_OEM_5      0xDC
#define WC3_VK_OEM_6      0xDD
#define WC3_VK_OEM_7      0xDE
#define WC3_VK_OEM_COMMA  0xBC
#define WC3_VK_OEM_MINUS  0xBD
#define WC3_VK_OEM_PERIOD 0xBE
#define WC3_VK_OEM_PLUS   0xBB
#define WC3_VK_PRIOR      0x21
#define WC3_VK_RCONTROL   0xA3
#define WC3_VK_RETURN     0x0D
#define WC3_VK_RIGHT      0x27
#define WC3_VK_RMENU      0xA5
#define WC3_VK_RSHIFT     0xA1
#define WC3_VK_SNAPSHOT   0x2C
#define WC3_VK_SPACE      0x20
#define WC3_VK_SUBTRACT   0x6D
#define WC3_VK_TAB        0x09
#define WC3_VK_UP         0x26


#define WC3_MOUSE_MOVE_FLAG 0x0001

static int g_screen_width = 1;
static int g_screen_height = 1;
static Wc3OverlayHapticSink g_haptic_sink;
static void *g_haptic_user;

void overlay_actions_init(int screen_width, int screen_height)
{
    g_screen_width = screen_width > 0 ? screen_width : 1;
    g_screen_height = screen_height > 0 ? screen_height : 1;
}

void overlay_actions_set_haptic_sink(Wc3OverlayHapticSink sink, void *user)
{
    g_haptic_sink = sink;
    g_haptic_user = user;
}

int overlay_actions_keycode_to_vk(int keycode)
{
    switch (keycode) {
    /* Modifiers use the specific left/right VK.  The HLE maps them to the
     * generic WM_KEYDOWN wParam and keeps GetKeyState(VK_CONTROL) etc. true
     * while either side is held. */
    case 37: return WC3_VK_LCONTROL;
    case 105: return WC3_VK_RCONTROL;
    case 50: return WC3_VK_LSHIFT;
    case 62: return WC3_VK_RSHIFT;
    case 64: return WC3_VK_LMENU;
    case 108: return WC3_VK_RMENU;

    case 9: return WC3_VK_ESCAPE;
    case 10: return '1';
    case 11: return '2';
    case 12: return '3';
    case 13: return '4';
    case 14: return '5';
    case 15: return '6';
    case 16: return '7';
    case 17: return '8';
    case 18: return '9';
    case 19: return '0';
    case 20: return WC3_VK_OEM_MINUS;
    case 21: return WC3_VK_OEM_PLUS;
    case 22: return WC3_VK_BACK;
    case 23: return WC3_VK_TAB;
    case 24: return 'Q';
    case 25: return 'W';
    case 26: return 'E';
    case 27: return 'R';
    case 28: return 'T';
    case 29: return 'Y';
    case 30: return 'U';
    case 31: return 'I';
    case 32: return 'O';
    case 33: return 'P';
    case 34: return WC3_VK_OEM_4;
    case 35: return WC3_VK_OEM_6;
    case 36: return WC3_VK_RETURN;
    case 38: return 'A';
    case 39: return 'S';
    case 40: return 'D';
    case 41: return 'F';
    case 42: return 'G';
    case 43: return 'H';
    case 44: return 'J';
    case 45: return 'K';
    case 46: return 'L';
    case 47: return WC3_VK_OEM_1;
    case 48: return WC3_VK_OEM_7;
    case 49: return WC3_VK_OEM_3;
    case 51: return WC3_VK_OEM_5;
    case 52: return 'Z';
    case 53: return 'X';
    case 54: return 'C';
    case 55: return 'V';
    case 56: return 'B';
    case 57: return 'N';
    case 58: return 'M';
    case 59: return WC3_VK_OEM_COMMA;
    case 60: return WC3_VK_OEM_PERIOD;
    case 61: return WC3_VK_OEM_2;
    case 63: return WC3_VK_MULTIPLY;
    case 65: return WC3_VK_SPACE;
    case 66: return WC3_VK_CAPITAL;
    case 67: return WC3_VK_F1;
    case 68: return WC3_VK_F2;
    case 69: return WC3_VK_F3;
    case 70: return WC3_VK_F4;
    case 71: return WC3_VK_F5;
    case 72: return WC3_VK_F6;
    case 73: return WC3_VK_F7;
    case 74: return WC3_VK_F8;
    case 75: return WC3_VK_F9;
    case 76: return WC3_VK_F10;
    case 77: return WC3_VK_NUMLOCK;
    case 79: return WC3_VK_NUMPAD7;
    case 80: return WC3_VK_NUMPAD8;
    case 81: return WC3_VK_NUMPAD9;
    case 82: return WC3_VK_SUBTRACT;
    case 83: return WC3_VK_NUMPAD4;
    case 84: return WC3_VK_NUMPAD5;
    case 85: return WC3_VK_NUMPAD6;
    case 86: return WC3_VK_ADD;
    case 87: return WC3_VK_NUMPAD1;
    case 88: return WC3_VK_NUMPAD2;
    case 89: return WC3_VK_NUMPAD3;
    case 90: return WC3_VK_NUMPAD0;
    case 91: return WC3_VK_DECIMAL;
    case 95: return WC3_VK_F11;
    case 96: return WC3_VK_F12;
    case 106: return WC3_VK_DIVIDE;
    case 107: return WC3_VK_SNAPSHOT;
    case 110: return WC3_VK_HOME;
    case 111: return WC3_VK_UP;
    case 112: return WC3_VK_PRIOR;
    case 113: return WC3_VK_LEFT;
    case 114: return WC3_VK_RIGHT;
    case 115: return WC3_VK_END;
    case 116: return WC3_VK_DOWN;
    case 117: return WC3_VK_NEXT;
    case 118: return WC3_VK_INSERT;
    case 119: return WC3_VK_DELETE;
    default: return 0;
    }
}

static float clamp01(float value)
{
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static void handle_pointer_move(const TouchAction *action)
{
    float nx = ((float)action->pointer_move.x + 0.5f) / (float)g_screen_width;
    float ny = ((float)action->pointer_move.y + 0.5f) / (float)g_screen_height;
    wc3_legacy_mouse_move(clamp01(nx), clamp01(ny));
}

static void handle_action(const TouchAction *action)
{
    int vk;

    switch (action->type) {
    case ACT_POINTER_MOVE:
        handle_pointer_move(action);
        break;
    case ACT_POINTER_MOVE_DELTA:
        wc3_legacy_mouse_move_delta(action->pointer_delta.dx,
                                     action->pointer_delta.dy);
        break;
    case ACT_POINTER_BUTTON_PRESS:
        wc3_legacy_mouse_button_ex(action->pointer_button.button, 1);
        break;
    case ACT_POINTER_BUTTON_RELEASE:
        wc3_legacy_mouse_button_ex(action->pointer_button.button, 0);
        break;
    case ACT_KEY_PRESS:
        vk = overlay_actions_keycode_to_vk(action->key.keycode);
        if (vk) wc3_legacy_key_event(vk, 1);
        break;
    case ACT_KEY_RELEASE:
        vk = overlay_actions_keycode_to_vk(action->key.keycode);
        if (vk) wc3_legacy_key_event(vk, 0);
        break;
    case ACT_MOUSE_EVENT:
        if (action->mouse_event.flags & WC3_MOUSE_MOVE_FLAG)
            wc3_legacy_mouse_move_delta(action->mouse_event.dx,
                                         action->mouse_event.dy);
        if (action->mouse_event.flags & 0x0800) /* MOUSEEVENTF_WHEEL */
            wc3_legacy_mouse_wheel(action->mouse_event.wheel);
        break;
    case ACT_SCROLL:
        /* Winlator: -1 is one notch up.  Windows: +WHEEL_DELTA is up. */
        wc3_legacy_mouse_wheel(-(int)action->scroll.amount * 120);
        break;
    case ACT_HAPTIC:
        if (g_haptic_sink && action->haptic.effect > 0)
            g_haptic_sink(action->haptic.effect, g_haptic_user);
        break;
    case ACT_START_MOUSE_MOVE:
    case ACT_STOP_MOUSE_MOVE:
    case ACT_SET_CURSOR_SPEED:
    case ACT_GAMEPAD_STATE:
    case ACT_GAMEPAD_RELEASE:
    case ACT_GAMEPAD_AXIS:
    case ACT_NONE:
    default:
        break;
    }
}

void overlay_actions_handle(const TouchAction *actions, int count, void *user)
{
    (void)user;
    if (!actions || count <= 0) return;
    for (int i = 0; i < count; i++)
        handle_action(&actions[i]);
}

void overlay_actions_release_all(void)
{
    wc3_legacy_release_all();
}
