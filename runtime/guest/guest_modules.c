/*
 * guest_modules.c - see guest_modules.h.
 *
 * No host lock and no allocation: the catalog is written once during
 * bootstrap, before the first guest thread.  Import binding is a second
 * startup pass over the already mapped images.
 */
#include "guest_modules.h"

#include "guest_memory.h"

#include <string.h>

static int ascii_lower(int ch)
{
    return (ch >= 'A' && ch <= 'Z') ? ch + ('a' - 'A') : ch;
}

/* Import descriptors normally carry a bare name, but LoadLibrary paths can
 * reach the same lookup.  Windows compares the basename, so do the same. */
static const char *base_name(const char *path)
{
    const char *base = path;
    if (!path) return "";
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    return base;
}

static int copy_text(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0u) return -1;
    if (!src) {
        dst[0] = '\0';
        return 0;
    }
    size_t n = 0;
    for (; src[n]; n++) {
        if (n + 1u >= cap) return -1;
        dst[n] = src[n];
    }
    dst[n] = '\0';
    return 0;
}

static int name_equal(const char *a, const char *b)
{
    a = base_name(a);
    b = base_name(b);
    while (*a && *b) {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

void wc3_module_registry_init(Wc3ModuleRegistry *registry)
{
    if (!registry) return;
    memset(registry, 0, sizeof *registry);
}

int wc3_module_add(Wc3ModuleRegistry *registry, const char *dll,
                   const Wc3PeImage *image)
{
    if (!registry || !dll || !image) return -1;
    if (image->base == 0u || image->span == 0u) return -1;
    if (registry->count >= WC3_MODULE_MAX) return -1;

    for (uint32_t i = 0; i < registry->count; i++) {
        if (name_equal(registry->module[i].dll, dll)) return -1;
    }

    Wc3Module *module = &registry->module[registry->count];
    if (copy_text(module->dll, sizeof module->dll, base_name(dll)) != 0)
        return -1;
    module->image = *image;
    registry->count++;
    return 0;
}

Wc3Module *wc3_module_find(Wc3ModuleRegistry *registry, const char *dll)
{
    if (!registry || !dll) return NULL;
    for (uint32_t i = 0; i < registry->count; i++) {
        if (name_equal(registry->module[i].dll, dll))
            return &registry->module[i];
    }
    return NULL;
}

uint32_t wc3_module_lookup(Wc3ModuleRegistry *registry, const char *dll,
                           const char *name, uint16_t ordinal)
{
    Wc3Module *module = wc3_module_find(registry, dll);
    if (!module) return 0;
    if (name) return wc3_pe_export(&module->image, name);
    return wc3_pe_export_ordinal(&module->image, ordinal);
}

int wc3_module_load(Wc3ModuleRegistry *registry, const Wc3Platform *platform,
                    const Wc3ModuleDesc *desc, Wc3PeImage *out)
{
    if (!registry || !platform || !desc || !desc->path) return -1;

    Wc3PeImage image;
    if (wc3_pe_load(platform, desc->path, desc->base, &image,
                    desc->allow_relocate) != 0)
        return -1;

    const char *dll = desc->dll ? desc->dll : base_name(desc->path);
    if (wc3_module_add(registry, dll, &image) != 0) {
        (void)wc3_guest_unmap(image.base, image.span);
        return -1;
    }
    if (out) *out = image;
    return 0;
}

void wc3_module_stats_init(Wc3BindStats *stats)
{
    if (!stats) return;
    memset(stats, 0, sizeof *stats);
}

typedef struct Wc3BindCtx {
    Wc3ModuleRegistry *registry;
    Wc3HleImportFn hle;
    void *hle_user;
    Wc3BindStats *stats;
} Wc3BindCtx;

#if WC3_DEBUG
static void record_missing(Wc3BindStats *stats, const char *dll,
                           const char *name, uint16_t ordinal)
{
    if (stats->first_missing_dll[0] != '\0') return;
    (void)copy_text(stats->first_missing_dll,
                    sizeof stats->first_missing_dll, base_name(dll));
    (void)copy_text(stats->first_missing_name,
                    sizeof stats->first_missing_name, name);
    stats->first_missing_ordinal = ordinal;
}

static void record_export_missing(Wc3BindStats *stats, const char *dll,
                                  const char *name, uint16_t ordinal)
{
    if (stats->first_export_missing_dll[0] != '\0') return;
    (void)copy_text(stats->first_export_missing_dll,
                    sizeof stats->first_export_missing_dll, base_name(dll));
    (void)copy_text(stats->first_export_missing_name,
                    sizeof stats->first_export_missing_name, name);
    stats->first_export_missing_ordinal = ordinal;
}
#endif

static uint32_t resolve_import(const char *dll, const char *name,
                               uint16_t ordinal, void *user)
{
    Wc3BindCtx *ctx = (Wc3BindCtx *)user;
    if (!ctx || !ctx->stats) return 0;

    /* A loaded guest module owns this DLL.  Its export is the original PE VA
     * that the lifted dispatcher already knows. */
    if (dll) {
        Wc3Module *module = wc3_module_find(ctx->registry, dll);
        if (module) {
            uint32_t va = name ? wc3_pe_export(&module->image, name)
                               : wc3_pe_export_ordinal(&module->image, ordinal);
            if (va) {
                ctx->stats->bound_module++;
                return va;
            }
            /* The legacy boot path also falls back to HLE here, but counts the
             * mismatch: a missing lifted export is a lift defect, not proof
             * that the OS DLL should own the symbol. */
            ctx->stats->export_missing++;
#if WC3_DEBUG
            record_export_missing(ctx->stats, dll, name, ordinal);
#endif
        }
    }

    if (ctx->hle) {
        uint32_t thunk = ctx->hle(ctx->hle_user, dll, name, ordinal);
        if (thunk) {
            ctx->stats->bound_hle++;
            return thunk;
        }
    }

    ctx->stats->missing++;
#if WC3_DEBUG
    record_missing(ctx->stats, dll, name, ordinal);
#endif
    return 0;
}

int wc3_module_bind(Wc3ModuleRegistry *registry, const Wc3PeImage *image,
                    Wc3HleImportFn hle, void *hle_user, Wc3BindStats *stats)
{
    if (!registry || !image || !stats) return -1;

    uint32_t before = stats->missing;
    Wc3BindCtx ctx = {registry, hle, hle_user, stats};
    if (wc3_pe_bind_imports(image, resolve_import, &ctx) < 0)
        return -1;
    return stats->missing > before ? 1 : 0;
}

int wc3_module_bind_all(Wc3ModuleRegistry *registry, Wc3HleImportFn hle,
                        void *hle_user, Wc3BindStats *stats)
{
    if (!registry || !stats) return -1;

    int unresolved = 0;
    for (uint32_t i = 0; i < registry->count; i++) {
        int rc = wc3_module_bind(registry, &registry->module[i].image, hle,
                                 hle_user, stats);
        if (rc < 0) return -1;
        if (rc > 0) unresolved = 1;
    }
    return unresolved;
}
