/*
 * legacy_shim.c - R15/H6.3 boundary implementation.
 *
 * This is the only target-tree translation unit that includes the quarantined
 * as-built headers.  Everything else consumes legacy_shim.h.
 */
#include <string.h>

#include "hle_registry.h"
#include "hle_android.h"
#include "hle_input_state.h"

#include "legacy_shim.h"

_Static_assert(HLE_MAX == WC3_LEGACY_HLE_MAX,
               "legacy HLE registry size changed; update legacy_shim.h");
_Static_assert(HLE_STRIDE == WC3_LEGACY_HLE_STRIDE,
               "legacy HLE thunk stride changed; update legacy_shim.h");

void wc3_legacy_hle_call(uint32_t thunk, CPU *c)
{
    hle_call(thunk, c);
}

const char *wc3_legacy_hle_describe(uint32_t thunk)
{
    return hle_describe(thunk);
}

static int ascii_equal_ci(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned ca = (unsigned char)*a++;
        unsigned cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    if (!dll || !name || !impl) return -1;
    for (unsigned i = 0; i < g_hle_count; i++) {
        HleEnt *e = &g_hle[i];
        if (!ascii_equal_ci(e->dll, dll)) continue;
        if (!ascii_equal_ci(e->name, name)) continue;
        e->fn = impl;
        return 0;
    }
    return -1;
}

int wc3_legacy_hle_chain(const char *dll, const char *name,
                         void (*impl)(CPU *c),
                         void (**previous)(CPU *c))
{
    if (!dll || !name || !impl || !previous) return -1;
    for (unsigned i = 0; i < g_hle_count; i++) {
        HleEnt *e = &g_hle[i];
        if (!ascii_equal_ci(e->dll, dll)) continue;
        if (!ascii_equal_ci(e->name, name)) continue;
        if (!e->fn) return -1;
        *previous = e->fn;
        e->fn = impl;
        return 0;
    }
    return -1;
}

static HleFn find_hle_fn(const char *dll, const char *name)
{
    if (!dll || !name) return NULL;
    for (unsigned i = 0; i < g_hle_count; i++) {
        HleEnt *e = &g_hle[i];
        if (!ascii_equal_ci(e->dll, dll)) continue;
        if (!ascii_equal_ci(e->name, name)) continue;
        return e->fn;
    }
    return NULL;
}

/* The as-built last-error cell is thread-local static state in the quarantine.
 * Call its registered accessor rather than reaching for the symbol.  The
 * fabricated stack only carries the one argument the setter reads. */
void wc3_legacy_set_last_error(uint32_t code)
{
    HleFn set_last_error = find_hle_fn("KERNEL32.dll", "SetLastError");
    if (!set_last_error) return;
    uint32_t stack = hle_guest_alloc(8u);
    if (!stack) return;
    wr32g(stack, 0u);
    wr32g(stack + 4u, code);
    CPU c;
    memset(&c, 0, sizeof c);
    c.esp = stack;
    set_last_error(&c);
    hle_guest_free(stack);
}

uint32_t wc3_legacy_get_last_error(void)
{
    HleFn get_last_error = find_hle_fn("KERNEL32.dll", "GetLastError");
    if (!get_last_error) return 0u;
    CPU c;
    memset(&c, 0, sizeof c);
    get_last_error(&c);
    return c.eax;
}

static void copy_bounded(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    if (!dll || !name || !impl || argc < 0 || argc > 0xFFFF) return -1;

    for (unsigned i = 0; i < g_hle_count; i++) {
        HleEnt *e = &g_hle[i];
        if (!ascii_equal_ci(e->dll, dll)) continue;
        if (!ascii_equal_ci(e->name, name)) continue;
        e->fn = impl;
        e->argc = (uint16_t)argc;
        e->cdecl_abi = cdecl_abi ? 1u : 0u;
        return 0;
    }

    if (g_hle_count >= HLE_MAX) return -1;
    HleEnt *e = &g_hle[g_hle_count++];
    memset(e, 0, sizeof *e);
    copy_bounded(e->dll, sizeof e->dll, dll);
    copy_bounded(e->name, sizeof e->name, name);
    e->fn = impl;
    e->argc = (uint16_t)argc;
    e->cdecl_abi = cdecl_abi ? 1u : 0u;
    e->used = 1;
    return 0;
}

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    return hle_android_guest_ok(addr, size);
}

uint8_t wc3_legacy_rd8g(uint32_t addr)
{
    return *(volatile uint8_t *)(uintptr_t)addr;
}

uint16_t wc3_legacy_rd16g(uint32_t addr)
{
    return *(volatile uint16_t *)(uintptr_t)addr;
}

uint32_t wc3_legacy_rd32g(uint32_t addr)
{
    return rd32g(addr);
}

void wc3_legacy_wr8g(uint32_t addr, uint8_t value)
{
    *(volatile uint8_t *)(uintptr_t)addr = value;
}

void wc3_legacy_wr16g(uint32_t addr, uint16_t value)
{
    *(volatile uint16_t *)(uintptr_t)addr = value;
}

void wc3_legacy_wr32g(uint32_t addr, uint32_t value)
{
    wr32g(addr, value);
}

uint32_t wc3_legacy_arg32(const CPU *cpu, unsigned index)
{
    if (!cpu) return 0;
    return rd32g(cpu->esp + 4u + index * 4u);
}

uint32_t wc3_legacy_guest_alloc(uint32_t size)
{
    return hle_guest_alloc(size);
}

uint32_t wc3_legacy_guest_alloc0(uint32_t size)
{
    uint32_t n = size ? size : 1u;
    uint32_t p = hle_guest_alloc(n);
    if (p) memset((void *)(uintptr_t)p, 0, n);
    return p;
}

void wc3_legacy_guest_free(uint32_t addr)
{
    hle_guest_free(addr);
}

uint32_t wc3_legacy_guest_size(uint32_t addr)
{
    /* GBlk { uint32_t size; uint32_t next; } lives immediately before the
     * payload.  Validate both the header range and the recorded size before
     * trusting either. */
    if (!addr || addr < 8u || !hle_android_guest_ok(addr - 8u, 8u)) return 0;
    uint32_t size = rd32g(addr - 8u);
    return (size && size <= GUEST_HLE_HEAP_SIZE - 8u) ? size : 0;
}

void wc3_legacy_guest_copy(uint32_t dst, uint32_t src, uint32_t size)
{
    if (!size || !hle_android_guest_ok(dst, size) ||
        !hle_android_guest_ok(src, size))
        return;
    memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, size);
}

void wc3_legacy_guest_move(uint32_t dst, uint32_t src, uint32_t size)
{
    if (!size || !hle_android_guest_ok(dst, size) ||
        !hle_android_guest_ok(src, size))
        return;
    memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, size);
}

void wc3_legacy_guest_fill(uint32_t dst, uint8_t value, uint32_t size)
{
    if (!size || !hle_android_guest_ok(dst, size)) return;
    memset((void *)(uintptr_t)dst, (int)value, size);
}

void wc3_legacy_guest_zero(uint32_t dst, uint32_t size)
{
    if (!size || !hle_android_guest_ok(dst, size)) return;
    memset((void *)(uintptr_t)dst, 0, size);
}

void wc3_legacy_set_screen_size(uint32_t width, uint32_t height)
{
    hle_android_set_screen_size(width, height);
}

int wc3_legacy_current_display_mode(uint32_t *width, uint32_t *height)
{
    return hle_android_current_display_mode(width, height);
}

int wc3_legacy_default_display_mode(uint32_t *width, uint32_t *height)
{
    return hle_android_default_display_mode(width, height);
}

int wc3_legacy_select_display_mode(uint32_t width, uint32_t height)
{
    return hle_android_select_display_mode(width, height);
}

int display_mode_at(uint32_t index, uint32_t *width, uint32_t *height)
{
    return hle_android_display_mode(index, width, height);
}

int display_mode_supported(uint32_t width, uint32_t height)
{
    return hle_android_display_mode_supported(width, height);
}

uint32_t display_refresh_hz(void)
{
    return hle_android_display_refresh_rate();
}

void wc3_legacy_mouse_move(float nx, float ny)
{
    hle_android_mouse_move(nx, ny);
}

void wc3_legacy_mouse_move_delta(int dx, int dy)
{
    hle_android_mouse_move_delta(dx, dy);
}

void wc3_legacy_mouse_button(int down)
{
    hle_android_mouse_button(down);
}

void wc3_legacy_mouse_button_ex(int button, int down)
{
    hle_android_mouse_button_ex(button, down);
}

void wc3_legacy_mouse_wheel(int delta)
{
    hle_android_mouse_wheel(delta);
}

void wc3_legacy_key_event(int vk, int down)
{
    hle_android_key_event(vk, down);
}

void wc3_legacy_release_all(void)
{
    hle_android_release_all();
}

void wc3_legacy_set_keyboard_bridge(void (*show)(void), void (*hide)(void))
{
    hle_android_set_keyboard_bridge(show, hide);
}

void wc3_legacy_set_keyboard_visible(int visible)
{
    hle_android_set_keyboard_visible(visible);
}

void wc3_legacy_post_char(uint32_t ch)
{
    hle_android_post_char(ch);
}

void wc3_legacy_post_backspace(void)
{
    hle_android_post_backspace();
}

void wc3_legacy_post_enter(void)
{
    hle_android_post_enter();
}

int wc3_legacy_mutf8_next(const char *text, int length, int offset,
                          uint16_t *unit)
{
    return hle_mutf8_next(text, length, offset, unit);
}

void wc3_legacy_text_diff(const char *old_text, int old_length,
                          const char *new_text, int new_length,
                          Wc3LegacyTextDiff *diff)
{
    if (!diff) return;
    HleTextDiff legacy;
    hle_text_diff(old_text, old_length, new_text, new_length, &legacy);
    diff->old_start = legacy.old_start;
    diff->new_start = legacy.new_start;
    diff->removed = legacy.removed;
    diff->added = legacy.added;
}
