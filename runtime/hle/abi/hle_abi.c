#include "hle_abi.h"
#include "hle_fast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The guest HLE window has HLE_MAX slots, but only a small startup subset is
 * overridden by the new HLE modules.  Keep those entries dense and map a guest
 * slot to its dense index; HLE_FAST_UNBOUND means the legacy path owns it. */
#define HLE_FAST_MAX 4096u
#define HLE_FAST_UNBOUND 0xFFFFu

_Static_assert(HLE_FAST_MAX <= 0xFFFFu,
               "fast registry index must fit the uint16_t sentinel");

static HleEntry g_fast_entry[HLE_FAST_MAX];
static uint16_t g_fast_slot_to_index[HLE_MAX];
static uint32_t g_fast_count;
static int g_fast_ready;
#if WC3_DEBUG
static uint32_t g_fast_calls[HLE_FAST_MAX];
#endif

static void hle_fast_init(void)
{
    for (uint32_t i = 0; i < HLE_MAX; i++)
        g_fast_slot_to_index[i] = HLE_FAST_UNBOUND;
    g_fast_ready = 1;
}

#if WC3_DEBUG
static int hle_ascii_lower(int ch)
{
    return (ch >= 'A' && ch <= 'Z') ? ch + ('a' - 'A') : ch;
}

static int hle_name_equal(const char *a, const char *b)
{
    if (a == b) return 1;
    if (!a || !b) return 0;
    while (*a && *b) {
        if (hle_ascii_lower((unsigned char)*a) !=
            hle_ascii_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}
#endif

void hle_registry_init(HleRegistry *r)
{
    if (!r) abort();
    r->count = 0;
    r->fatal_missing = 0;
}

HleEntry *hle_register_ex(HleRegistry *r, const char *dll, const char *name,
                          uint16_t ordinal, HleFn impl, uint16_t argc,
                          uint8_t flags, void *user)
{
    if (!r || !impl) abort();
    if (r->count >= HLE_MAX) abort();

#if WC3_DEBUG
    for (uint32_t i = 0; i < r->count; i++) {
        HleDebugEntry *d = &r->debug[i];
        if (!hle_name_equal(d->dll, dll)) continue;
        if (!name) {
            if (!d->name && d->ordinal == ordinal) abort();
        } else if (d->name && hle_name_equal(d->name, name)) {
            abort();
        }
    }
#endif

    uint32_t slot = r->count++;
    HleEntry *e = &r->entry[slot];
    e->argc = argc;
    e->flags = flags;
    e->reserved = 0;
    e->impl = impl;
    e->user = user;
    /* The guest call pushes a return address before entering the thunk.  A
     * cdecl callee leaves its arguments for the caller; stdcall and fastcall
     * remove them here. */
    e->stack_cleanup = 4u + ((flags & HLE_CALL_CDECL) ? 0u : (uint32_t)argc * 4u);

#if WC3_DEBUG
    r->debug[slot].dll = dll;
    r->debug[slot].name = name;
    r->debug[slot].calls = 0;
    r->debug[slot].ordinal = ordinal;
    r->debug[slot].reserved = 0;
#else
    (void)dll;
    (void)name;
    (void)ordinal;
#endif
    return e;
}

HleEntry *hle_register(HleRegistry *r, const char *dll, const char *name,
                       uint16_t ordinal, HleFn impl, uint16_t argc,
                       uint8_t flags)
{
    return hle_register_ex(r, dll, name, ordinal, impl, argc, flags, NULL);
}

static uint32_t hle_index_for(HleRegistry *r, uint32_t thunk)
{
    if (!r || thunk < GUEST_HLE_BASE) return HLE_MAX;
    uint32_t off = thunk - GUEST_HLE_BASE;
    if ((off % HLE_STRIDE) != 0u) return HLE_MAX;
    uint32_t idx = off / HLE_STRIDE;
    return (idx < r->count) ? idx : HLE_MAX;
}

HleEntry *hle_entry_for(HleRegistry *r, uint32_t thunk)
{
    uint32_t idx = hle_index_for(r, thunk);
    if (idx == HLE_MAX) return NULL;
    return &r->entry[idx];
}

#if WC3_DEBUG
HleEntry *hle_find(HleRegistry *r, const char *dll, const char *name,
                   uint16_t ordinal)
{
    if (!r || !dll) return NULL;
    for (uint32_t i = 0; i < r->count; i++) {
        HleDebugEntry *d = &r->debug[i];
        if (!hle_name_equal(d->dll, dll)) continue;
        if (name) {
            if (d->name && hle_name_equal(d->name, name)) return &r->entry[i];
        } else if (!d->name && d->ordinal == ordinal) {
            return &r->entry[i];
        }
    }
    return NULL;
}

const char *hle_abi_describe(HleRegistry *r, uint32_t thunk)
{
    uint32_t idx = hle_index_for(r, thunk);
    if (idx == HLE_MAX) return "";
    HleDebugEntry *d = &r->debug[idx];
    if (d->name) return d->name;
    return d->dll ? d->dll : "";
}
#endif

#if WC3_DEBUG
static void hle_debug_note_stack_move(const HleEntry *e, uint32_t esp_out,
                                      CPU *c, const char *dll,
                                      const char *name)
{
    const uint32_t esp_in = c->esp - e->stack_cleanup;
    if (esp_out == esp_in) return;
    fprintf(stderr, "[hle] stack moved by %s!%s: %+d\n",
            dll ? dll : "?",
            name ? name : "?",
            (int)(esp_out - esp_in));
}
#endif

/* Common thunk body.  Returns the handler's esp after impl so debug callers
 * can report a moved stack with their own symbol names. */
static uint32_t hle_abi_call_entry(HleEntry *e, uint32_t thunk, CPU *c)
{
    if (!e || !e->impl || !c) abort();

    /* Win32 callee-saved registers are part of the guest ABI: every target
     * handler must leave ebx/esi/edi/ebp unchanged.  A source audit of all
     * target HLE handlers found exactly one writer, waveout_hle.c's
     * w2_invoke(), and it saves/restores the four fields itself around its
     * guest callback.  The bridge deliberately does not save or restore them:
     * that would hide a handler which violates the ABI and add four loads and
     * stores to every HLE call. */
    uint32_t esp_in = c->esp;

    c->eip = thunk;
    e->impl(c);

    uint32_t esp_out = c->esp;

    /* Force the caller frame instead of adding cleanup to a handler that
     * already moved esp; the latter compounds a corrupted guest stack. */
    c->esp = esp_in + e->stack_cleanup;
    return esp_out;
}

void hle_abi_call(HleRegistry *r, uint32_t thunk, CPU *c)
{
    uint32_t idx = hle_index_for(r, thunk);
    if (idx == HLE_MAX) abort();
    HleEntry *e = &r->entry[idx];
    if (!e->impl || !c) abort();

#if WC3_DEBUG
    HleDebugEntry *d = &r->debug[idx];
    if (d->calls != UINT32_MAX) d->calls++;
#endif

    uint32_t esp_out = hle_abi_call_entry(e, thunk, c);

#if WC3_DEBUG
    hle_debug_note_stack_move(e, esp_out, c, d->dll, d->name);
#else
    (void)esp_out;
#endif
}

void wc3_hle_fast_bind(uint32_t slot, void (*impl)(CPU *), uint16_t argc,
                       uint8_t cdecl_abi)
{
    if (slot >= HLE_MAX || !impl) return;
    if (!g_fast_ready) hle_fast_init();

    uint16_t index = g_fast_slot_to_index[slot];
    int is_new = 0;
    if (index == HLE_FAST_UNBOUND) {
        if (g_fast_count >= HLE_FAST_MAX) return;
        index = (uint16_t)g_fast_count;
        is_new = 1;
    }

    HleEntry *e = &g_fast_entry[index];
    e->argc = argc;
    e->flags = cdecl_abi ? HLE_CALL_CDECL : 0u;
    e->reserved = 0;
    e->impl = impl;
    e->user = NULL;
    e->stack_cleanup = 4u +
        ((e->flags & HLE_CALL_CDECL) ? 0u : (uint32_t)argc * 4u);

    if (is_new) {
        g_fast_slot_to_index[slot] = index;
        g_fast_count = (uint32_t)index + 1u;
    }
}

int wc3_hle_fast_call_slot(uint32_t slot, uint32_t thunk, CPU *c)
{
    if (!g_fast_ready || !c || slot >= HLE_MAX) return 0;

    uint16_t index = g_fast_slot_to_index[slot];
    if (index == HLE_FAST_UNBOUND) return 0;

    HleEntry *e = &g_fast_entry[index];
#if WC3_DEBUG
    if (g_fast_calls[index] != UINT32_MAX) g_fast_calls[index]++;
#endif
    uint32_t esp_out = hle_abi_call_entry(e, thunk, c);

#if WC3_DEBUG
    hle_debug_note_stack_move(e, esp_out, c, NULL, NULL);
#else
    (void)esp_out;
#endif
    return 1;
}

int wc3_hle_fast_call(uint32_t thunk, CPU *c)
{
    if (thunk < GUEST_HLE_BASE) return 0;
    uint32_t off = thunk - GUEST_HLE_BASE;
    if (off % HLE_STRIDE) return 0;
    uint32_t slot = off / HLE_STRIDE;
    return wc3_hle_fast_call_slot(slot, thunk, c);
}

void hle_fatal_missing(const char *dll, const char *name, uint16_t ordinal)
{
#if WC3_DEBUG
    if (name)
        fprintf(stderr, "[hle] fatal: missing mandatory import %s!%s\n",
                dll ? dll : "?", name);
    else
        fprintf(stderr, "[hle] fatal: missing mandatory import %s!#%u\n",
                dll ? dll : "?", (unsigned)ordinal);
#else
    (void)dll;
    (void)name;
    (void)ordinal;
#endif
    abort();
}
