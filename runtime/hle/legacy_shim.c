/*
 * legacy_shim.c - R15/H6.3 boundary implementation.
 *
 * This is the only target-tree translation unit that includes the quarantined
 * as-built headers.  Everything else consumes legacy_shim.h.
 */
#include <string.h>

#include "hle_registry.h"
#include "hle_android.h"

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

void wc3_legacy_mouse_button(int down)
{
    hle_android_mouse_button(down);
}
