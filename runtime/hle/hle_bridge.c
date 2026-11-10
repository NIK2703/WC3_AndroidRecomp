/*
 * hle_bridge.c - shared registry bridge implementation.
 *
 * This is the only target-tree translation unit that includes the as-built
 * Android HLE headers.  Everything else consumes hle_bridge.h.
 */
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "hle_registry.h"
#include "hle_android.h"
#include "hle_input_state.h"
#include "hle_fast.h"
#include "guest_memory.h"

#include "hle_bridge.h"

_Static_assert(HLE_MAX == WC3_LEGACY_HLE_MAX,
               "as-built HLE registry size changed; update hle_bridge.h");
_Static_assert(HLE_STRIDE == WC3_LEGACY_HLE_STRIDE,
               "as-built HLE thunk stride changed; update hle_bridge.h");

#if !WC3_DEBUG
/* O-46: release keeps one thunk -> slot conversion for the whole call.  A slot
 * that is not overridden by a target HLE module is still an as-built handler,
 * so preserve hle_call()'s callee-saved register and stack-cleanup contract.
 * The call ring, the per-import counter and g_last_cpu belong to diagnostics;
 * they stay on the debug path, where hle_call() remains the source of truth.
 * Missing-import logging is likewise diagnostic and is intentionally absent
 * from release. */
static void legacy_call_direct(uint32_t slot, uint32_t thunk, CPU *c)
{
    HleEnt *e = &g_hle[slot];
    if (!e->fn) return;

    c->eip = thunk;
    uint32_t sbx = c->ebx, ssi = c->esi, sdi = c->edi, sbp = c->ebp;
    uint32_t esp_in = c->esp;
    e->fn(c);
    c->ebx = sbx;
    c->esi = ssi;
    c->edi = sdi;
    c->ebp = sbp;
    c->esp = esp_in + (e->cdecl_abi ? 4u : 4u + (uint32_t)e->argc * 4u);
}
#endif

void wc3_legacy_hle_call(uint32_t thunk, CPU *c)
{
#if !WC3_DEBUG
    if (thunk >= GUEST_HLE_BASE) {
        uint32_t off = thunk - GUEST_HLE_BASE;
        if ((off % HLE_STRIDE) == 0u) {
            uint32_t slot = off / HLE_STRIDE;
            if (slot < g_hle_count && c) {
                if (wc3_hle_fast_call_slot(slot, thunk, c)) return;
                if (g_hle[slot].fn) {
                    legacy_call_direct(slot, thunk, c);
                    return;
                }
            }
        }
    }
#else
    /* Debug keeps the as-built path, including its per-call ring, trace and
     * missing-import/stack diagnostics. */
    if (wc3_hle_fast_call(thunk, c)) return;
    hle_call(thunk, c);
    return;
#endif
    if (wc3_hle_fast_call(thunk, c)) return;
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

/* O-57: startup override matching used to scan up to HLE_MAX rows for every
 * wc3_legacy_hle_override/chain/add call.  The registry is append-only and its
 * names do not change after registration, so a case-folded open-addressed
 * index is enough.  g_hle_index_count records how many rows the table covers;
 * if the as-built HLE appends directly, the next lookup sees the mismatch and
 * rebuilds once.  This is startup-only; the registry has no hot-path lock. */
#define HLE_INDEX_CAP (1u << 13)
_Static_assert(HLE_MAX <= HLE_INDEX_CAP, "HLE index too small");
_Static_assert((HLE_INDEX_CAP & (HLE_INDEX_CAP - 1u)) == 0u,
               "HLE index must be a power of two");
_Static_assert(HLE_MAX <= 0xFFFF, "HLE row does not fit in index");

static uint16_t g_hle_index[HLE_INDEX_CAP];
static unsigned g_hle_index_count;

static uint32_t hle_name_hash(const char *dll, const char *name)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)dll; *p; p++) {
        unsigned c = *p;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        h = (h ^ c) * 16777619u;
    }
    h = (h ^ 0xFFu) * 16777619u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        unsigned c = *p;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        h = (h ^ c) * 16777619u;
    }
    return h;
}

static void hle_index_insert(unsigned row)
{
    uint32_t h = hle_name_hash(g_hle[row].dll, g_hle[row].name);
    for (unsigned probe = 0; probe < HLE_INDEX_CAP; probe++) {
        uint16_t *slot = &g_hle_index[(h + probe) & (HLE_INDEX_CAP - 1u)];
        if (*slot == 0u) {
            *slot = (uint16_t)(row + 1u);
            return;
        }
    }
}

static void hle_index_ensure(void)
{
    if (g_hle_index_count == g_hle_count) return;
    memset(g_hle_index, 0, sizeof g_hle_index);
    for (unsigned i = 0; i < g_hle_count; i++) hle_index_insert(i);
    g_hle_index_count = g_hle_count;
}

static int find_hle_row(const char *dll, const char *name)
{
    if (!dll || !name) return -1;
    hle_index_ensure();
    uint32_t h = hle_name_hash(dll, name);
    for (unsigned probe = 0; probe < HLE_INDEX_CAP; probe++) {
        uint16_t v = g_hle_index[(h + probe) & (HLE_INDEX_CAP - 1u)];
        if (v == 0u) return -1;
        unsigned row = (unsigned)v - 1u;
        if (row >= g_hle_count) continue;
        HleEnt *e = &g_hle[row];
        if (ascii_equal_ci(e->dll, dll) && ascii_equal_ci(e->name, name))
            return (int)row;
    }
    return -1;
}

static void bind_fast_slot(HleEnt *e)
{
    if (!e || !e->fn) return;
    wc3_hle_fast_bind((uint32_t)(e - &g_hle[0]), e->fn, e->argc,
                      e->cdecl_abi);
}

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    if (!dll || !name || !impl) return -1;
    int row = find_hle_row(dll, name);
    if (row < 0) return -1;
    HleEnt *e = &g_hle[row];
    e->fn = impl;
    bind_fast_slot(e);
    return 0;
}

int wc3_legacy_hle_fast_bind(const char *dll, const char *name)
{
    if (!dll || !name) return -1;
    int row = find_hle_row(dll, name);
    if (row < 0) return -1;
    HleEnt *e = &g_hle[row];
    if (!e->fn) return -1;
    bind_fast_slot(e);
    return 0;
}

int wc3_legacy_hle_chain(const char *dll, const char *name,
                         void (*impl)(CPU *c),
                         void (**previous)(CPU *c))
{
    if (!dll || !name || !impl || !previous) return -1;
    int row = find_hle_row(dll, name);
    if (row < 0) return -1;
    HleEnt *e = &g_hle[row];
    if (!e->fn) return -1;
    *previous = e->fn;
    e->fn = impl;
    bind_fast_slot(e);
    return 0;
}

static HleFn find_hle_fn(const char *dll, const char *name)
{
    int row = find_hle_row(dll, name);
    return row < 0 ? NULL : g_hle[row].fn;
}

uint32_t wc3_legacy_hle_thunk_of(const char *dll, const char *name)
{
    int row = find_hle_row(dll, name);
    if (row < 0) return 0u;
    return GUEST_HLE_BASE + (uint32_t)row * (uint32_t)HLE_STRIDE;
}

/* The as-built last-error cell is thread-local static state in the as-built HLE.
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

    int row = find_hle_row(dll, name);
    if (row >= 0) {
        HleEnt *e = &g_hle[row];
        e->fn = impl;
        e->argc = (uint16_t)argc;
        e->cdecl_abi = cdecl_abi ? 1u : 0u;
        bind_fast_slot(e);
        return 0;
    }

    if (g_hle_count >= HLE_MAX) return -1;
    unsigned old_count = g_hle_count;
    HleEnt *e = &g_hle[g_hle_count++];
    memset(e, 0, sizeof *e);
    copy_bounded(e->dll, sizeof e->dll, dll);
    copy_bounded(e->name, sizeof e->name, name);
    e->fn = impl;
    e->argc = (uint16_t)argc;
    e->cdecl_abi = cdecl_abi ? 1u : 0u;
    e->used = 1;
    if (g_hle_index_count == old_count) {
        hle_index_insert(old_count);
        g_hle_index_count = g_hle_count;
    }
    bind_fast_slot(e);
    return 0;
}

int wc3_legacy_hle_bind_ordinal(const char *dll, uint16_t ordinal, int argc,
                                int cdecl_abi, void (*impl)(CPU *c))
{
    if (!dll || !dll[0] || !impl || argc < 0 || argc > 0xFFFF) return -1;

    HleEnt *e = NULL;
    for (unsigned i = 0; i < g_hle_count; i++) {
        HleEnt *candidate = &g_hle[i];
        if (candidate->ord != ordinal || candidate->name[0]) continue;
        if (ascii_equal_ci(candidate->dll, dll)) {
            e = candidate;
            break;
        }
    }

    if (!e) {
        if (g_hle_count >= HLE_MAX) return -1;
        e = &g_hle[g_hle_count++];
        memset(e, 0, sizeof *e);
        copy_bounded(e->dll, sizeof e->dll, dll);
        e->used = 1;
        /* The name index deliberately does not cover ordinal rows.  Leave its
         * count untouched so the next name lookup sees the mismatch and
         * rebuilds once. */
    }

    e->ord = ordinal;
    e->fn = impl;
    e->argc = (uint16_t)argc;
    e->cdecl_abi = cdecl_abi ? 1u : 0u;
    bind_fast_slot(e);
    return 0;
}

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    /* O-07: the new validator owns the static-arena page map.  Its dynamic
     * range log is not identical to the as-built one yet, so a rejection
     * still falls through to the as-built check.  A positive answer from the
     * new table is always an actually mapped or statically owned range. */
    if (wc3_guest_ok(addr, size)) return 1;
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
    /* wc3_legacy_guest_ok takes the target page-map fast path first and still
     * falls back to the as-built check, so the accepted set is unchanged. */
    if (!size || !wc3_legacy_guest_ok(dst, size) ||
        !wc3_legacy_guest_ok(src, size))
        return;
    memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, size);
}

void wc3_legacy_guest_move(uint32_t dst, uint32_t src, uint32_t size)
{
    if (!size || !wc3_legacy_guest_ok(dst, size) ||
        !wc3_legacy_guest_ok(src, size))
        return;
    memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, size);
}

void wc3_legacy_guest_fill(uint32_t dst, uint8_t value, uint32_t size)
{
    if (!size || !wc3_legacy_guest_ok(dst, size)) return;
    memset((void *)(uintptr_t)dst, (int)value, size);
}

void wc3_legacy_guest_zero(uint32_t dst, uint32_t size)
{
    if (!size || !wc3_legacy_guest_ok(dst, size)) return;
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
