/*
 * guest_modules.h - module catalog and cross-module import binding.
 *
 * The lifted corpus keeps the original PE virtual addresses.  A call from
 * game.dll to Storm.dll must therefore land on the lifted Storm export at its
 * PE base, while a call to an OS DLL must land on an HLE thunk.  The PE mapper
 * in pe.c can rewrite an IAT, but it deliberately knows nothing about which
 * module is loaded or where an import comes from; that policy lives here.
 *
 * Rules:
 *   - a loaded guest module is resolved by export name/ordinal to its mapped
 *     PE address (the dispatcher then picks the lifted body for that VA);
 *   - an unloaded module (an OS DLL) falls through to the HLE resolver;
 *   - a loaded module with a missing export is counted separately and then
 *     follows the legacy boot path: HLE may still serve it, otherwise the
 *     import is unresolved;
 *   - an unresolved mandatory import is reported, never invented.
 *
 * The catalog is startup state: it is append-only, has no host lock and does
 * not allocate.  Callers load modules in dependency order and bind each image
 * after all modules it imports are registered.
 */
#ifndef WC3_GUEST_MODULES_H
#define WC3_GUEST_MODULES_H

#include <stddef.h>
#include <stdint.h>

#include "pe.h"
#include "platform.h"

#ifndef WC3_DEBUG
#define WC3_DEBUG 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define WC3_MODULE_MAX 16u
#define WC3_MODULE_NAME_MAX 32u
#define WC3_MODULE_SYMBOL_MAX 80u

typedef struct Wc3Module {
    /* Basename as registered, for diagnostics and case-insensitive lookup. */
    char dll[WC3_MODULE_NAME_MAX];
    Wc3PeImage image;
} Wc3Module;

typedef struct Wc3ModuleRegistry {
    Wc3Module module[WC3_MODULE_MAX];
    uint32_t count;
} Wc3ModuleRegistry;

typedef struct Wc3ModuleDesc {
    const char *dll;
    const char *path;
    /* Preferred guest base, or 0 to use the PE ImageBase. */
    uint32_t base;
    /* Apply HIGHLOW relocations when base differs from ImageBase.  The
     * shipped corpus is lifted from rebased inputs, so the Android product
     * passes 0 and a base mismatch is a hard load error. */
    int allow_relocate;
} Wc3ModuleDesc;

/* Resolver for an import of a DLL that is not a loaded guest module.  The
 * returned value is the guest VA of the HLE thunk, or 0 when the HLE layer has
 * no implementation. */
typedef uint32_t (*Wc3HleImportFn)(void *user, const char *dll,
                                   const char *name, uint16_t ordinal);

typedef struct Wc3BindStats {
    /* Imports served by a loaded guest module (the lifted corpus). */
    uint32_t bound_module;
    /* Imports served by the HLE resolver. */
    uint32_t bound_hle;
    /* Imports served by neither. */
    uint32_t missing;
    /* Imports whose DLL was loaded but whose export lookup failed.  They are
     * a subset of bound_hle + missing and point at a lift/policy mismatch. */
    uint32_t export_missing;
#if WC3_DEBUG
    char first_missing_dll[WC3_MODULE_NAME_MAX];
    char first_missing_name[WC3_MODULE_SYMBOL_MAX];
    uint16_t first_missing_ordinal;
    char first_export_missing_dll[WC3_MODULE_NAME_MAX];
    char first_export_missing_name[WC3_MODULE_SYMBOL_MAX];
    uint16_t first_export_missing_ordinal;
#endif
} Wc3BindStats;

/* Zero a catalog.  Startup only. */
void wc3_module_registry_init(Wc3ModuleRegistry *registry);

/* Register an already mapped image under its import-directory name.  The name
 * is reduced to a basename and compared case-insensitively.  Returns 0 on
 * success, -1 for a bad argument, a duplicate name or a full catalog. */
int wc3_module_add(Wc3ModuleRegistry *registry, const char *dll,
                   const Wc3PeImage *image);

/* Look up a registered module by a name that may contain a path and any case.
 * Returns NULL when absent. */
Wc3Module *wc3_module_find(Wc3ModuleRegistry *registry, const char *dll);

/* Resolve dll!name (or dll!#ordinal when name is NULL) to the mapped guest VA
 * of a lifted export, or 0 when the module or export is absent.  This does not
 * consult HLE: an OS DLL simply has no registered module. */
uint32_t wc3_module_lookup(Wc3ModuleRegistry *registry, const char *dll,
                           const char *name, uint16_t ordinal);

/* Map a PE through Wc3FileApi and register it.  On a registration failure the
 * mapping is released again.  Returns 0 on success.
 *
 * The catalog copies the DLL name but keeps the image's `path` pointer for
 * diagnostics, so `desc` and `desc->path` must live for the registry's
 * lifetime (bootstrap/static storage, not a stack buffer). */
int wc3_module_load(Wc3ModuleRegistry *registry, const Wc3Platform *platform,
                    const Wc3ModuleDesc *desc, Wc3PeImage *out);

/* Initialize the counters before a bind pass. */
void wc3_module_stats_init(Wc3BindStats *stats);

/* Rewrite one image's IAT.  `hle` may be NULL when only lifted modules are
 * expected.  Returns 0 when every import was bound, 1 when at least one
 * import stayed zero, and -1 for an invalid argument or a malformed image.
 * Stats accumulate across calls. */
int wc3_module_bind(Wc3ModuleRegistry *registry, const Wc3PeImage *image,
                    Wc3HleImportFn hle, void *hle_user, Wc3BindStats *stats);

/* Bind every registered module.  Same return contract as wc3_module_bind. */
int wc3_module_bind_all(Wc3ModuleRegistry *registry, Wc3HleImportFn hle,
                        void *hle_user, Wc3BindStats *stats);

#ifdef __cplusplus
}
#endif

#endif /* WC3_GUEST_MODULES_H */
