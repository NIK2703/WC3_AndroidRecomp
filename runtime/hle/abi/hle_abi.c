#include "hle_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

const char *hle_describe(HleRegistry *r, uint32_t thunk)
{
    uint32_t idx = hle_index_for(r, thunk);
    if (idx == HLE_MAX) return "";
    HleDebugEntry *d = &r->debug[idx];
    if (d->name) return d->name;
    return d->dll ? d->dll : "";
}
#endif

void hle_call(HleRegistry *r, uint32_t thunk, CPU *c)
{
    uint32_t idx = hle_index_for(r, thunk);
    if (idx == HLE_MAX) abort();
    HleEntry *e = &r->entry[idx];
    if (!e->impl || !c) abort();

#if WC3_DEBUG
    HleDebugEntry *d = &r->debug[idx];
    if (d->calls != UINT32_MAX) d->calls++;
#endif

    uint32_t saved_ebx = c->ebx;
    uint32_t saved_esi = c->esi;
    uint32_t saved_edi = c->edi;
    uint32_t saved_ebp = c->ebp;
    uint32_t esp_in = c->esp;

    c->eip = thunk;
    e->impl(c);

    c->ebx = saved_ebx;
    c->esi = saved_esi;
    c->edi = saved_edi;
    c->ebp = saved_ebp;

#if WC3_DEBUG
    if (c->esp != esp_in) {
        fprintf(stderr, "[hle] stack moved by %s!%s: %+d\n",
                d->dll ? d->dll : "?",
                d->name ? d->name : "?",
                (int)(c->esp - esp_in));
    }
#endif

    /* Force the caller frame instead of adding cleanup to a handler that
     * already moved esp; the latter compounds a corrupted guest stack. */
    c->esp = esp_in + e->stack_cleanup;
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
