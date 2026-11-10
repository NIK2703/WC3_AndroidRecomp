/*
 * hle_abi.h - one import registry and one call bridge.
 *
 * A guest import is a thunk in the reserved HLE window.  The dispatcher maps a
 * thunk to a registry slot with integer arithmetic; there is no hash table, no
 * host lock and no allocation on the call path.
 *
 * Hot data is the whole entry in release.  DLL/symbol names and call counters
 * are diagnostic data and exist only under WC3_DEBUG.  A release build binds
 * imports through the generated import table, not through a name lookup.
 */
#ifndef WC3_HLE_ABI_H
#define WC3_HLE_ABI_H

#include <stdint.h>

#include "cpu.h"
/* The target ABI always uses the runtime/guest layout.  A plain
 * "guest_layout.h" is ambiguous: the legacy shim target also has
 * runtime/hle/android on its include path, and that copy predates GUEST_HLE_MAX. */
#include "../../guest/guest_layout.h"

#ifndef WC3_DEBUG
#define WC3_DEBUG 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define HLE_MAX GUEST_HLE_MAX
#define HLE_STRIDE GUEST_HLE_STRIDE

typedef void (*HleFn)(CPU *c);

enum {
    /* Caller cleans the arguments (cdecl).  The bridge removes only the
     * return address it pushed for the guest call. */
    HLE_CALL_CDECL = 1u << 0,
    /* The import is allowed to fail with a documented error result.  It is
     * never allowed to be absent from the table or to have an unknown arity. */
    HLE_OPTIONAL = 1u << 1
};

typedef struct HleEntry {
    /* Number of bytes removed from the guest stack after impl returns. */
    uint32_t stack_cleanup;
    uint16_t argc;
    uint8_t flags;
    uint8_t reserved;
    HleFn impl;
    /* COM/vtable slot, DirectSound object, or other per-thunk payload.  The
     * D3D9/DirectSound handlers use this instead of generating one function per
     * COM method.  NULL for ordinary Win32 imports. */
    void *user;
} HleEntry;

#if WC3_DEBUG
typedef struct HleDebugEntry {
    const char *dll;
    const char *name;
    uint32_t calls;
    uint16_t ordinal;
    uint16_t reserved;
} HleDebugEntry;
#endif

typedef struct HleRegistry {
    HleEntry entry[HLE_MAX];
    uint32_t count;
    uint32_t fatal_missing;
#if WC3_DEBUG
    HleDebugEntry debug[HLE_MAX];
#endif
} HleRegistry;

/* Startup only: reset the slot count.  Entries are overwritten as they are
 * registered; no hot-path state is touched. */
void hle_registry_init(HleRegistry *r);

/* Startup only.  impl must not be NULL.  A duplicate (module, name/ordinal)
 * entry is a configuration error and aborts in debug builds; release builds
 * bind by generated slot index and therefore do not need names. */
HleEntry *hle_register(HleRegistry *r, const char *dll, const char *name,
                       uint16_t ordinal, HleFn impl, uint16_t argc,
                       uint8_t flags);
/* Same registration with the per-thunk payload used by COM/DirectSound. */
HleEntry *hle_register_ex(HleRegistry *r, const char *dll, const char *name,
                          uint16_t ordinal, HleFn impl, uint16_t argc,
                          uint8_t flags, void *user);

/* Direct thunk conversion.  Both functions return NULL for an unaligned,
 * out-of-range or unregistered thunk. */
HleEntry *hle_entry_for(HleRegistry *r, uint32_t thunk);

/* Debug-only symbol lookup used by the legacy table adapter during cutover. */
#if WC3_DEBUG
HleEntry *hle_find(HleRegistry *r, const char *dll, const char *name,
                   uint16_t ordinal);
const char *hle_abi_describe(HleRegistry *r, uint32_t thunk);
#endif

/* Guest -> HLE call.  The guest stack contains the return address at c->esp
 * on entry.  The bridge removes exactly stack_cleanup bytes; a handler that
 * changes esp is corrected back to the caller frame before cleanup.
 *
 * The name differs from the as-built hle_call(uint32_t, CPU*) on purpose:
 * both translation units are linked into the product during the M5 cutover,
 * and C has no overloads. */
void hle_abi_call(HleRegistry *r, uint32_t thunk, CPU *c);

/* Mandatory import that is not in the table.  This is a startup fatal, never
 * a stub and never a no-op. */
void hle_fatal_missing(const char *dll, const char *name, uint16_t ordinal)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((noreturn))
#endif
    ;

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_ABI_H */
