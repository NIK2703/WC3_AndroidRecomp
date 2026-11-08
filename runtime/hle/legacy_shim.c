/*
 * legacy_shim.c - R15/H6.3 boundary implementation.
 *
 * This is the only target-tree translation unit that includes the quarantined
 * as-built headers.  Everything else consumes legacy_shim.h.
 */
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "hle_registry.h"
#include "hle_android.h"
#include "hle_input_state.h"
#include "hle_fast.h"
#include "guest_memory.h"

#include "legacy_shim.h"

_Static_assert(HLE_MAX == WC3_LEGACY_HLE_MAX,
               "legacy HLE registry size changed; update legacy_shim.h");
_Static_assert(HLE_STRIDE == WC3_LEGACY_HLE_STRIDE,
               "legacy HLE thunk stride changed; update legacy_shim.h");

void wc3_legacy_hle_call(uint32_t thunk, CPU *c)
{
    /* Overridden slots use the clean hle_abi bridge (no release-time call
     * ring).  Unbound slots keep the as-built path, including its missing
     * import diagnostics. */
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
 * if the quarantine appends directly, the next lookup sees the mismatch and
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

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    /* O-07: the new validator owns the static-arena page map.  Its dynamic
     * range log is not identical to the quarantine one yet, so a rejection
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
     * falls back to the quarantine check, so the accepted set is unchanged. */
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

/* ------------------------------------------------------------------------- *
 * A-12.4 S2: a real guest thread for winmm's WOM_DONE callback.
 *
 * The as-built guest_thread_spawn() maps a TEB page, a guest stack, sets
 * g_self_thread (TLS, guest id, wait state), builds a CPU with fs_base/segment
 * registers and finally calls dispatch(c, start).  All of that lives behind
 * the static boundary of runtime/legacy/hle_android.c, but its public entry
 * KERNEL32!CreateThread is an HLE registry row we can call with a fabricated
 * stdcall frame.  Registering a private row as `start` gives us a guest
 * address that dispatch() resolves back to our own handler -- so the loop body
 * runs with a genuine guest CPU/TEB/TLS instead of a borrowed one.
 * ------------------------------------------------------------------------- */

#define WC3_CB_THREAD_ROW "Wc3WaveOutCallbackThread"

static Wc3LegacyGuestLoop g_cb_body;
static void *g_cb_opaque;
static sem_t g_cb_sem;

/* O-34: the callback thread is a one-shot startup object.  The old g_cb_lock
 * was held across hle_guest_alloc(), the guest stack writes and CreateThread,
 * so the new guest thread could enter wc3_guest_callback_thunk() while the host
 * lock was still held.  This lock only serialises start callers and is released
 * before any guest state is written or CreateThread is called.  g_cb_ready is
 * published before CreateThread so the new thread can enter wait immediately;
 * g_cb_state becomes READY only after CreateThread returns a handle. */
enum {
    CB_IDLE = 0,
    CB_STARTING = 1,
    CB_READY = 2
};
static pthread_mutex_t g_cb_start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cb_start_cond = PTHREAD_COND_INITIALIZER;
static int g_cb_state;
static _Atomic int g_cb_ready;

uint32_t wc3_legacy_hle_thunk_of(const char *dll, const char *name)
{
    if (!dll || !name) return 0u;
    for (unsigned i = 0; i < g_hle_count; i++) {
        HleEnt *e = &g_hle[i];
        if (!ascii_equal_ci(e->dll, dll)) continue;
        if (!ascii_equal_ci(e->name, name)) continue;
        return GUEST_HLE_BASE + (uint32_t)i * (uint32_t)HLE_STRIDE;
    }
    return 0u;
}

/* Registered with argc=1: guest_thread_proc pushes lpParameter as `arg` and a
 * return slot, so hle_call() sees one stdcall argument.  The handler never
 * returns while the process is alive; on the way out it restores esp so the
 * as-built stack check stays quiet. */
static void wc3_guest_callback_thunk(CPU *c)
{
    uint32_t esp = c->esp;
    if (atomic_load_explicit(&g_cb_ready, memory_order_acquire) && g_cb_body)
        g_cb_body(c, g_cb_opaque);
    c->esp = esp;
}

/* The start owner has set g_cb_state = CB_STARTING and released
 * g_cb_start_lock.  sem_initialized says whether sem_destroy is safe.  No guest
 * thread can be waiting here: this path is only reached before CreateThread
 * returns a handle (or before CreateThread is called at all). */
static int cb_start_failed(int sem_initialized)
{
    atomic_store_explicit(&g_cb_ready, 0, memory_order_release);
    if (sem_initialized) sem_destroy(&g_cb_sem);
    pthread_mutex_lock(&g_cb_start_lock);
    g_cb_state = CB_IDLE;
    pthread_cond_broadcast(&g_cb_start_cond);
    pthread_mutex_unlock(&g_cb_start_lock);
    return -1;
}

int wc3_legacy_guest_callback_thread_start(Wc3LegacyGuestLoop body,
                                           void *opaque)
{
    if (!body) return -1;

    /* Serialise start callers, but never hold this lock across a guest call or
     * the actual CreateThread dispatch.  A concurrent caller waits for the
     * owner to finish, matching the old g_cb_lock ordering without making the
     * new guest thread run under it. */
    pthread_mutex_lock(&g_cb_start_lock);
    while (g_cb_state == CB_STARTING)
        pthread_cond_wait(&g_cb_start_cond, &g_cb_start_lock);
    if (g_cb_state == CB_READY) {
        pthread_mutex_unlock(&g_cb_start_lock);
        return 0;
    }
    g_cb_state = CB_STARTING;
    pthread_mutex_unlock(&g_cb_start_lock);

    g_cb_body = body;
    g_cb_opaque = opaque;
    if (sem_init(&g_cb_sem, 0, 0) != 0) return cb_start_failed(0);
    if (wc3_legacy_hle_add("WINMM.dll", WC3_CB_THREAD_ROW, 1, 0,
                           wc3_guest_callback_thunk) != 0)
        return cb_start_failed(1);
    uint32_t thunk = wc3_legacy_hle_thunk_of("WINMM.dll", WC3_CB_THREAD_ROW);
    HleFn create = find_hle_fn("KERNEL32.dll", "CreateThread");
    if (!thunk || !create) return cb_start_failed(1);

    /* CreateThread(lpThreadAttributes, dwStackSize, lpStartAddress,
     *              lpParameter, dwCreationFlags, lpThreadId) -- argc 6.
     * h_create_thread() reads arg32(c,2..5), so [esp] is the return slot and
     * the six arguments start at esp+4.  No host lock is held here: the new
     * guest thread may enter wc3_guest_callback_thunk() immediately. */
    uint32_t esp = hle_guest_alloc(32u);
    if (!esp) return cb_start_failed(1);
    wr32g(esp + 0u, 0x7EFFFFF0u);
    wr32g(esp + 4u, 0u);
    wr32g(esp + 8u, 0u);
    wr32g(esp + 12u, thunk);
    wr32g(esp + 16u, 0u);
    wr32g(esp + 20u, 0u);
    wr32g(esp + 24u, 0u);
    /* Publish callback readiness before CreateThread: the new guest thread
     * enters wc3_legacy_guest_callback_wait() immediately and must not observe
     * a false ready flag and exit.  Start completion stays CB_STARTING until
     * CreateThread returns a handle. */
    atomic_store_explicit(&g_cb_ready, 1, memory_order_release);
    CPU c;
    memset(&c, 0, sizeof c);
    c.esp = esp;
    create(&c);
    uint32_t handle = c.eax;
    hle_guest_free(esp);

    if (!handle) return cb_start_failed(1);

    pthread_mutex_lock(&g_cb_start_lock);
    g_cb_state = CB_READY;
    pthread_cond_broadcast(&g_cb_start_cond);
    pthread_mutex_unlock(&g_cb_start_lock);
    return 0;
}

void wc3_legacy_guest_callback_wake(void)
{
    if (atomic_load_explicit(&g_cb_ready, memory_order_acquire))
        (void)sem_post(&g_cb_sem);
}

int wc3_legacy_guest_callback_wait(void)
{
    if (!atomic_load_explicit(&g_cb_ready, memory_order_acquire)) return -1;
    for (;;) {
        if (sem_wait(&g_cb_sem) == 0) return 0;
        if (errno != EINTR) return -1;
    }
}
