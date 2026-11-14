/*
 * hle_bridge_test.c - T0 contract test for the real H6.3 bridge.
 *
 * The memory/process/time module tests use fake bridge functions.  This test
 * links the actual runtime/hle/hle_bridge.c against a fake as-built registry and
 * a low-32-bit guest mapping, so the override matcher, argument reader, heap
 * header validation, and display/input forwarding are exercised as shipped.
 *
 * The direct guest-memory checks need an identity-mapped low address.  They
 * run on Windows hosts, where VirtualAlloc can reserve 0x10000000; other hosts
 * still run the registry and forwarding checks.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "cpu.h"
#include "hle_android.h"
#include "hle_registry.h"
#include "hle_bridge.h"

#define GUEST_BASE 0x10000000u
#define GUEST_SIZE 0x10000u
#define ARG_BASE (GUEST_BASE + 0x100u)
#define HEAP_BASE (GUEST_BASE + 0x1000u)

HleEnt g_hle[HLE_MAX];
unsigned g_hle_count;

static uint8_t *g_guest;
static uint32_t g_heap_end;

static uint32_t g_last_thunk;
static CPU *g_last_cpu;
static const char *g_describe = "bridge-test";
static uint32_t g_screen_w, g_screen_h;
static int g_display_default;
static uint32_t g_display_w, g_display_h;
static int g_display_select;
static float g_mouse_x, g_mouse_y;
static int g_mouse_down;
static int g_mouse_dx, g_mouse_dy;
static int g_mouse_button_ex;
static int g_mouse_wheel;
static int g_key_vk, g_key_down;
static int g_release_all;
static void (*g_keyboard_show)(uint32_t field, const HleKeyboardRect *rect);
static void (*g_keyboard_hide)(void);
static uint32_t g_show_field;
static int g_show_rect_valid;
static Wc3LegacyKeyboardRect g_show_rect;
static int g_show_calls;
static int g_hide_calls;
static int g_keyboard_visible;
static uint32_t g_post_char;
static int g_post_backspace;
static int g_post_enter;

static void *guest_ptr(uint32_t addr, uint32_t size)
{
    if (!g_guest || addr < GUEST_BASE) return NULL;
    const uint32_t off = addr - GUEST_BASE;
    if (off > GUEST_SIZE || size > GUEST_SIZE - off) return NULL;
    return g_guest + off;
}

static int map_guest(void)
{
#ifdef _WIN32
    g_guest = (uint8_t *)VirtualAlloc((void *)(uintptr_t)GUEST_BASE, GUEST_SIZE,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (g_guest) {
        g_heap_end = 0;
        return 0;
    }
#endif
    return -1;
}

int hle_android_guest_ok(uint32_t addr, uint32_t size)
{
    return guest_ptr(addr, size) != NULL;
}

uint32_t rd32g(uint32_t a)
{
    const void *p = guest_ptr(a, 4);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

void wr32g(uint32_t a, uint32_t v)
{
    void *p = guest_ptr(a, 4);
    if (p) memcpy(p, &v, 4);
}

uint32_t hle_guest_alloc(uint32_t size)
{
    if (!g_guest) return 0;
    if (!size) size = 1;
    size = (size + 7u) & ~7u;
    if (g_heap_end + 8u + size > GUEST_SIZE - 0x1000u) return 0;
    const uint32_t header = HEAP_BASE + g_heap_end;
    g_heap_end += 8u + size;
    wr32g(header, size);
    wr32g(header + 4u, 0u);
    return header + 8u;
}

void hle_guest_free(uint32_t addr)
{
    if (addr < 8u) return;
    wr32g(addr - 8u, 0u);
}

void hle_call(uint32_t thunk, CPU *c)
{
    g_last_thunk = thunk;
    g_last_cpu = c;
}

const char *hle_describe(uint32_t thunk)
{
    (void)thunk;
    return g_describe;
}

void hle_android_set_screen_size(uint32_t width, uint32_t height)
{
    g_screen_w = width;
    g_screen_h = height;
}

int hle_android_current_display_mode(uint32_t *width, uint32_t *height)
{
    if (width) *width = g_display_select ? g_display_w : 800u;
    if (height) *height = g_display_select ? g_display_h : 600u;
    return 1;
}

int hle_android_default_display_mode(uint32_t *width, uint32_t *height)
{
    if (width) *width = 800;
    if (height) *height = 600;
    g_display_default++;
    return 1;
}

int hle_android_select_display_mode(uint32_t width, uint32_t height)
{
    g_display_w = width;
    g_display_h = height;
    g_display_select++;
    return 1;
}

int hle_android_display_mode(uint32_t index, uint32_t *width, uint32_t *height)
{
    static const uint32_t widths[] = { 640, 800, 1024 };
    static const uint32_t heights[] = { 480, 600, 768 };
    if (index >= sizeof widths / sizeof widths[0]) return 0;
    if (width) *width = widths[index];
    if (height) *height = heights[index];
    return 1;
}

int hle_android_display_mode_supported(uint32_t width, uint32_t height)
{
    return (width == 640u && height == 480u) ||
           (width == 800u && height == 600u) ||
           (width == 1024u && height == 768u);
}

uint32_t hle_android_display_refresh_rate(void)
{
    return 60u;
}

void hle_android_mouse_move(float nx, float ny)
{
    g_mouse_x = nx;
    g_mouse_y = ny;
}

void hle_android_mouse_button(int down)
{
    g_mouse_down = down;
}

void hle_android_mouse_move_delta(int dx, int dy)
{
    g_mouse_dx = dx;
    g_mouse_dy = dy;
}

void hle_android_mouse_button_ex(int button, int down)
{
    g_mouse_button_ex = (button << 1) | (down ? 1 : 0);
}

void hle_android_mouse_wheel(int delta)
{
    g_mouse_wheel = delta;
}

void hle_android_key_event(int vk, int down)
{
    g_key_vk = vk;
    g_key_down = down;
}

void hle_android_release_all(void)
{
    g_release_all++;
}

void hle_android_set_keyboard_bridge(void (*show)(uint32_t field,
                                                  const HleKeyboardRect *rect),
                                     void (*hide)(void))
{
    g_keyboard_show = show;
    g_keyboard_hide = hide;
}

void hle_android_set_keyboard_visible(int visible)
{
    g_keyboard_visible = visible;
}

void hle_android_post_char(uint32_t ch)
{
    g_post_char = ch;
}

void hle_android_post_backspace(void)
{
    g_post_backspace++;
}

void hle_android_post_enter(void)
{
    g_post_enter++;
}

static void fake_show(uint32_t field, const Wc3LegacyKeyboardRect *rect)
{
    g_show_calls++;
    g_show_field = field;
    g_show_rect_valid = rect != NULL;
    if (rect)
        g_show_rect = *rect;
}

static void fake_hide(void) { g_hide_calls++; }

static void fn_a(CPU *c) { c->eax = 0xAAAAu; }
static void fn_b(CPU *c) { c->eax = 0xBBBBu; }

static uint32_t g_clobber_esp;
static void fn_clobber(CPU *c)
{
    g_clobber_esp = c->esp;
    c->eax = 0xC1A0B1E5u;
    c->ebx = 0xB0000001u;
    c->esi = 0xB0000002u;
    c->edi = 0xB0000003u;
    c->ebp = 0xB0000004u;
    /* A real handler may push a callback frame; hle_call() must force the
     * caller stack back before applying the import cleanup. */
    c->esp -= 12u;
}

static void add_row(const char *dll, const char *name, HleFn fn)
{
    HleEnt *e = &g_hle[g_hle_count++];
    memset(e, 0, sizeof *e);
    strncpy(e->dll, dll, sizeof e->dll - 1u);
    strncpy(e->name, name, sizeof e->name - 1u);
    e->fn = fn;
    e->argc = 1;
}

#define CHECK(condition, message)                                         \
    do {                                                                  \
        if (!(condition)) {                                               \
            printf("FAIL: %s\n", message);                                \
            return 1;                                                     \
        }                                                                 \
    } while (0)

static int close_to(float actual, float expected)
{
    const float delta = actual - expected;
    return delta < 1e-4f && delta > -1e-4f;
}

int main(void)
{
    add_row("KERNEL32.dll", "TestA", fn_a);
    add_row("MSVCR80.dll", "TestB", fn_b);

    CHECK(wc3_legacy_hle_override("kernel32.DLL", "testa", fn_b) == 0,
          "override case-insensitive");
    CHECK(g_hle[0].fn == fn_b, "override replaces matching row");
    CHECK(g_hle[1].fn == fn_b, "override leaves other row");
    g_hle[0].fn = fn_a;
    CHECK(wc3_legacy_hle_override("KERNEL32.dll", "Missing", fn_b) == -1,
          "override missing row");
    CHECK(wc3_legacy_hle_override(NULL, "TestA", fn_b) == -1,
          "override NULL dll");
    CHECK(wc3_legacy_hle_override("KERNEL32.dll", NULL, fn_b) == -1,
          "override NULL name");
    CHECK(wc3_legacy_hle_override("KERNEL32.dll", "TestA", NULL) == -1,
          "override NULL fn");

    CHECK(wc3_legacy_hle_add("MSVCR80.dll", "TestC", 2, 1, fn_a) == 0,
          "add new row");
    CHECK(g_hle_count == 3u, "add appends");
    CHECK(g_hle[2].argc == 2u && g_hle[2].cdecl_abi == 1u,
          "add stores arity and convention");
    CHECK(wc3_legacy_hle_add("msvcr80.DLL", "testc", 3, 0, fn_b) == 0,
          "add updates existing row");
    CHECK(g_hle_count == 3u, "add update does not append");
    CHECK(g_hle[2].fn == fn_b && g_hle[2].argc == 3u &&
              g_hle[2].cdecl_abi == 0u,
          "add update replaces fields");
    CHECK(wc3_legacy_hle_add(NULL, "TestC", 1, 1, fn_a) == -1,
          "add NULL dll");
    CHECK(wc3_legacy_hle_add("KERNEL32.dll", NULL, 1, 1, fn_a) == -1,
          "add NULL name");
    CHECK(wc3_legacy_hle_add("KERNEL32.dll", "TestD", 1, 1, NULL) == -1,
          "add NULL fn");
    CHECK(wc3_legacy_hle_add("KERNEL32.dll", "TestD", -1, 1, fn_a) == -1,
          "add negative argc");
    CHECK(wc3_legacy_hle_add("KERNEL32.dll", "TestD", 0x10000, 1, fn_a) == -1,
          "add oversized argc");

    /* O-46: a row appended by the as-built HLE is not fast-bound, so release
     * must call its as-built handler directly.  That handler deliberately
     * moves esp and clobbers every callee-saved field; the direct path has to
     * preserve hle_call()'s forced cleanup and register contract, not just
     * return the handler's eax. */
    add_row("KERNEL32.dll", "TestClobber", fn_clobber);
    g_hle[3].argc = 3;
    CPU clobber;
    memset(&clobber, 0, sizeof clobber);
    clobber.esp = 0x1000u;
    clobber.ebx = 1u;
    clobber.esi = 2u;
    clobber.edi = 3u;
    clobber.ebp = 4u;
    wc3_legacy_hle_call(GUEST_HLE_BASE + 3u * GUEST_HLE_STRIDE, &clobber);
#if !WC3_DEBUG
    CHECK(g_clobber_esp == 0x1000u, "direct legacy entry esp");
    CHECK(clobber.eax == 0xC1A0B1E5u, "direct legacy clobber result");
    CHECK(clobber.esp == 0x1000u + 16u, "direct legacy forced cleanup");
    CHECK(clobber.ebx == 1u && clobber.esi == 2u &&
              clobber.edi == 3u && clobber.ebp == 4u,
          "direct legacy callee-saved registers");
    CHECK(g_hle[3].calls == 0u, "direct legacy drops diagnostic counter");
#else
    /* Debug keeps the as-built hle_call() path; the fake here only records
     * the forwarding, so the handler itself is not entered. */
    CHECK(g_clobber_esp == 0u, "debug clobber stays in hle_call");
    CHECK(g_last_thunk == GUEST_HLE_BASE + 3u * GUEST_HLE_STRIDE,
          "debug clobber forwards thunk");
#endif

    add_row("KERNEL32.dll", "TestCdeclClobber", fn_clobber);
    g_hle[4].argc = 2;
    g_hle[4].cdecl_abi = 1;
    CPU cdecl_clobber;
    memset(&cdecl_clobber, 0, sizeof cdecl_clobber);
    cdecl_clobber.esp = 0x2000u;
    wc3_legacy_hle_call(GUEST_HLE_BASE + 4u * GUEST_HLE_STRIDE, &cdecl_clobber);
#if !WC3_DEBUG
    CHECK(g_clobber_esp == 0x2000u, "direct legacy cdecl entry esp");
    CHECK(cdecl_clobber.esp == 0x2004u, "direct legacy cdecl cleanup");
#endif

    CPU c;
    memset(&c, 0, sizeof c);
    wc3_legacy_hle_call(GUEST_HLE_BASE + GUEST_HLE_STRIDE, &c);
#if WC3_DEBUG
    CHECK(g_last_thunk == GUEST_HLE_BASE + GUEST_HLE_STRIDE, "hle_call thunk");
    CHECK(g_last_cpu == &c, "hle_call cpu");
#else
    /* O-46 release path: the as-built handler runs directly after the single
     * slot conversion, so the diagnostic hle_call() hook is not entered. */
    CHECK(c.eax == 0xBBBBu, "release direct legacy handler result");
    CHECK(c.esp == 8u, "release direct legacy stack cleanup");
    CHECK(g_last_thunk == 0u, "release direct bypasses hle_call");
#endif
    CHECK(strcmp(wc3_legacy_hle_describe(0), "bridge-test") == 0,
          "hle_describe");

    /* O-08: an overridden slot goes through the clean fast bridge and never
     * reaches the as-built hle_call ring. */
    g_last_thunk = 0u;
    CHECK(wc3_legacy_hle_override("MSVCR80.dll", "TestB", fn_b) == 0,
          "override binds fast slot");
    CPU fast_cpu;
    memset(&fast_cpu, 0, sizeof fast_cpu);
    wc3_legacy_hle_call(GUEST_HLE_BASE + GUEST_HLE_STRIDE, &fast_cpu);
    CHECK(fast_cpu.eax == 0xBBBBu, "override fast bridge result");
    CHECK(g_last_thunk == 0u, "override bypasses as-built hle_call");

    wc3_legacy_set_screen_size(1280, 720);
    CHECK(g_screen_w == 1280u && g_screen_h == 720u, "screen size");
    uint32_t w = 0, h = 0;
    CHECK(wc3_legacy_default_display_mode(&w, &h) == 1 && w == 800u &&
              h == 600u && g_display_default == 1,
          "default display mode");
    CHECK(wc3_legacy_select_display_mode(1024, 768) == 1 &&
              g_display_w == 1024u && g_display_h == 768u,
          "select display mode");
    CHECK(wc3_legacy_current_display_mode(&w, &h) == 1 && w == 1024u &&
              h == 768u,
          "current display mode");
    CHECK(display_mode_at(2, &w, &h) == 1 && w == 1024u && h == 768u,
          "display mode at index");
    CHECK(display_mode_at(3, &w, &h) == 0, "display mode past end");
    CHECK(display_mode_supported(1024, 768) == 1 &&
              display_mode_supported(1280, 720) == 0,
          "display mode supported");
    CHECK(display_refresh_hz() == 60u, "display refresh");
    wc3_legacy_mouse_move(0.25f, 0.75f);
    wc3_legacy_mouse_button(1);
    CHECK(g_mouse_x == 0.25f && g_mouse_y == 0.75f && g_mouse_down == 1,
          "mouse forwarding");
    wc3_legacy_mouse_move_delta(7, -3);
    wc3_legacy_mouse_button_ex(2, 1);
    wc3_legacy_mouse_wheel(-120);
    wc3_legacy_key_event(0x41, 1);
    wc3_legacy_release_all();
    CHECK(g_mouse_dx == 7 && g_mouse_dy == -3, "mouse delta forwarding");
    CHECK(g_mouse_button_ex == 5, "mouse button forwarding");
    CHECK(g_mouse_wheel == -120, "mouse wheel forwarding");
    CHECK(g_key_vk == 0x41 && g_key_down == 1, "key forwarding");
    CHECK(g_release_all == 1, "release_all forwarding");

    wc3_legacy_set_keyboard_bridge(fake_show, fake_hide);
    wc3_legacy_set_keyboard_visible(1);
    wc3_legacy_post_char(0x20ACu);
    wc3_legacy_post_backspace();
    wc3_legacy_post_enter();
    CHECK(g_keyboard_show != NULL && g_keyboard_hide == fake_hide,
          "keyboard bridge forwarding");
    CHECK(g_keyboard_visible == 1, "keyboard visibility forwarding");
    CHECK(g_post_char == 0x20ACu, "post char forwarding");
    CHECK(g_post_backspace == 1 && g_post_enter == 1,
          "post editing forwarding");

    {
        const HleKeyboardRect hle_rect = {0.125f, 0.25f, 0.5f, 0.75f};
        g_keyboard_show(0x1234u, &hle_rect);
        CHECK(g_show_calls == 1 && g_show_field == 0x1234u &&
                  g_show_rect_valid,
              "keyboard field passed to host");
        CHECK(close_to(g_show_rect.left, 0.125f) &&
                  close_to(g_show_rect.top, 0.25f) &&
                  close_to(g_show_rect.right, 0.5f) &&
                  close_to(g_show_rect.bottom, 0.75f),
              "keyboard rect copied to host");
        g_keyboard_show(0x5678u, NULL);
        CHECK(g_show_calls == 2 && g_show_field == 0x5678u &&
                  !g_show_rect_valid,
              "missing keyboard rect stays NULL");
        g_keyboard_hide();
        CHECK(g_hide_calls == 1, "keyboard hide forwarded");
        wc3_legacy_set_keyboard_bridge(NULL, NULL);
        CHECK(g_keyboard_show == NULL && g_keyboard_hide == NULL,
              "keyboard bridge unregisters");
    }

    {
        HleKeyboardRect r = {0.0f, 0.0f, 0.0f, 0.0f};
        CHECK(hle_keyboard_rect_from_ddc(0.0f, 0.0f, 0.6f, 0.8f, &r) == 1 &&
                  r.left == 0.0f && r.top == 0.0f &&
                  r.right == 1.0f && r.bottom == 1.0f,
              "DDC full screen maps to surface");
        CHECK(hle_keyboard_rect_from_ddc(0.15f, 0.2f, 0.45f, 0.4f, &r) == 1 &&
                  close_to(r.left, 0.25f) &&
                  close_to(r.top, 0.25f) &&
                  close_to(r.right, 0.5f) &&
                  close_to(r.bottom, 0.75f),
              "DDC field maps to surface");
        CHECK(hle_keyboard_rect_from_ddc(0.6f, 0.0f, 0.0f, 0.8f, &r) == 0,
              "DDC inverted rect rejected");
        CHECK(hle_keyboard_rect_from_ddc(0.0f, 0.0f, 0.6f, 0.8f, NULL) == 0,
              "DDC NULL output rejected");
    }

    {
        const Wc3LegacyKeyboardRect visible = {0.0f, 0.2f, 1.0f, 0.5f};
        const Wc3LegacyKeyboardRect covered = {0.0f, 0.2f, 1.0f, 0.7f};
        const Wc3LegacyKeyboardRect full = {0.0f, 0.0f, 1.0f, 1.0f};
        CHECK(wc3_legacy_keyboard_pan_px(&visible, 400, 1000) == 0.0f,
              "keyboard pan leaves visible field alone");
        CHECK(close_to(wc3_legacy_keyboard_pan_px(&covered, 400, 1000), 100.0f),
              "keyboard pan moves covered field to IME edge");
        CHECK(wc3_legacy_keyboard_pan_px(&covered, 0, 1000) == 0.0f,
              "keyboard pan ignores closed IME");
        CHECK(wc3_legacy_keyboard_pan_px(&covered, 400, 0) == 0.0f,
              "keyboard pan ignores empty surface");
        CHECK(wc3_legacy_keyboard_pan_px(NULL, 400, 1000) == 0.0f,
              "keyboard pan ignores missing rect");
        CHECK(close_to(wc3_legacy_keyboard_pan_px(&full, 400, 1000), 400.0f),
              "keyboard pan clamps full-screen field");
    }

    {
        uint16_t unit = 0;
        CHECK(wc3_legacy_mutf8_next("A", 1, 0, &unit) == 1 && unit == 'A',
              "mutf8 ASCII");
        const char nul[] = {(char)0xC0, (char)0x80};
        CHECK(wc3_legacy_mutf8_next(nul, 2, 0, &unit) == 2 && unit == 0,
              "mutf8 encoded NUL");
        const char euro[] = {(char)0xE2, (char)0x82, (char)0xAC};
        CHECK(wc3_legacy_mutf8_next(euro, 3, 0, &unit) == 3 &&
                  unit == 0x20ACu,
              "mutf8 BMP character");
        CHECK(wc3_legacy_mutf8_next(euro, 3, 3, &unit) == 0,
              "mutf8 end of text");
    }

    {
        Wc3LegacyTextDiff diff;
        wc3_legacy_text_diff("abc", 3, "abd", 3, &diff);
        CHECK(diff.old_start == 2 && diff.new_start == 2 &&
                  diff.removed == 1 && diff.added == 1,
              "text diff replaces suffix");
        wc3_legacy_text_diff("ab", 2, "abcd", 4, &diff);
        CHECK(diff.old_start == 2 && diff.new_start == 2 &&
                  diff.removed == 0 && diff.added == 2,
              "text diff appends");
        wc3_legacy_text_diff("abcd", 4, "ab", 2, &diff);
        CHECK(diff.old_start == 2 && diff.new_start == 2 &&
                  diff.removed == 2 && diff.added == 0,
              "text diff removes suffix");
        const char old_euro[] = {(char)0xE2, (char)0x82, (char)0xAC};
        wc3_legacy_text_diff(old_euro, 3, "", 0, &diff);
        CHECK(diff.removed == 1, "text diff counts code units");
    }

    if (map_guest() != 0) {
        printf("hle bridge OK (registry only; no low guest mapping)\n");
        return 0;
    }

    CPU arg_cpu;
    memset(&arg_cpu, 0, sizeof arg_cpu);
    arg_cpu.esp = ARG_BASE;
    wr32g(ARG_BASE + 4u, 0x11111111u);
    wr32g(ARG_BASE + 8u, 0x22222222u);
    CHECK(wc3_legacy_arg32(&arg_cpu, 0) == 0x11111111u, "arg32[0]");
    CHECK(wc3_legacy_arg32(&arg_cpu, 1) == 0x22222222u, "arg32[1]");

    const uint32_t p = wc3_legacy_guest_alloc(8);
    CHECK(p != 0u && wc3_legacy_guest_size(p) == 8u, "guest_alloc");
    memset(guest_ptr(p, 8), 0xCC, 8);
    wc3_legacy_guest_zero(p, 8);
    for (unsigned i = 0; i < 8; i++)
        CHECK(((uint8_t *)guest_ptr(p, 8))[i] == 0u, "guest_zero");

    const uint32_t q = wc3_legacy_guest_alloc0(8);
    CHECK(q != 0u && wc3_legacy_guest_size(q) == 8u, "guest_alloc0");
    for (unsigned i = 0; i < 8; i++)
        CHECK(((uint8_t *)guest_ptr(q, 8))[i] == 0u, "guest_alloc0 zero");

    memcpy(guest_ptr(q, 8), "shimtest", 8);
    wc3_legacy_guest_copy(p, q, 8);
    CHECK(memcmp(guest_ptr(p, 8), "shimtest", 8) == 0, "guest_copy");

    wc3_legacy_guest_move(p + 2u, p, 4u);
    CHECK(memcmp(guest_ptr(p, 8), "shshimst", 8) == 0, "guest_move overlap");
    wc3_legacy_guest_fill(p, 0x5A, 8);
    for (unsigned i = 0; i < 8; i++)
        CHECK(((uint8_t *)guest_ptr(p, 8))[i] == 0x5Au, "guest_fill");

    wc3_legacy_guest_free(p);
    CHECK(wc3_legacy_guest_size(p) == 0u, "guest_free");
    CHECK(wc3_legacy_guest_size(0xFFFFFFFFu) == 0u, "guest_size invalid");

    /* O-57: a row appended by the as-built HLE after the lookup index was built
     * must be found on the next lookup, not only after a fresh process start. */
    HleEnt *direct = &g_hle[g_hle_count++];
    memset(direct, 0, sizeof *direct);
    strncpy(direct->dll, "Direct.dll", sizeof direct->dll - 1u);
    strncpy(direct->name, "DirectRow", sizeof direct->name - 1u);
    direct->fn = fn_a;
    direct->argc = 1;
    direct->used = 1;
    CHECK(wc3_legacy_hle_override("DIRECT.DLL", "directrow", fn_b) == 0,
          "index rebuild after direct append");
    CHECK(direct->fn == fn_b, "index rebuild updates row");

    /* Exercise append and update beyond the two hand-written rows.  The names
     * are unique, so a missed index entry makes the override return -1. */
    for (unsigned i = 0; i < 512u; i++) {
        char dll[32];
        char name[32];
        snprintf(dll, sizeof dll, "Dll%u.dll", i & 7u);
        snprintf(name, sizeof name, "Name%u", i);
        CHECK(wc3_legacy_hle_add(dll, name, 1, 1, fn_a) == 0, "index add");
        CHECK(wc3_legacy_hle_override(dll, name, fn_b) == 0,
              "index override");
    }

    printf("hle bridge OK (registry + guest memory)\n");
    return 0;
}
