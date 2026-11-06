/*
 * pe.h - minimal PE32 image mapper for the Android guest.
 *
 * The lifted corpus is linked against the original image bases; this loader
 * maps the licensed PE files at those bases and applies base relocations when
 * a caller explicitly allows it.  It reads files through Wc3FileApi and does
 * not include stdio or POSIX headers on the production path.
 */
#ifndef WC3_PE_H
#define WC3_PE_H

#include <stddef.h>
#include <stdint.h>

#include "platform.h"
#include "pe_format.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Wc3PeImage {
    uint32_t base;
    uint32_t image_base;
    uint32_t span;
    uint32_t entry;
    uint32_t import_rva;
    uint32_t export_rva;
    uint32_t export_size;
    uint32_t reloc_rva;
    uint32_t reloc_size;
    uint32_t resource_rva;
    uint32_t resource_size;
    uint32_t stamp;
    const char *path;
} Wc3PeImage;

/* Map path at preferred_base (0 means the PE ImageBase).  allow_relocate
 * permits a base mismatch and applies HIGHLOW relocations.  Returns 0 on
 * success, -1 on a malformed file, allocation failure or address conflict. */
int wc3_pe_load(const Wc3Platform *platform, const char *path,
                uint32_t preferred_base, Wc3PeImage *out, int allow_relocate);

uint32_t wc3_pe_export(const Wc3PeImage *image, const char *name);
uint32_t wc3_pe_export_ordinal(const Wc3PeImage *image, uint16_t ordinal);

/* RT_STRING lookup.  Non-ASCII code units become '?'; returns the written
 * length, or 0 when the resource is absent. */
int wc3_pe_string_resource(const Wc3PeImage *image, unsigned id, char *out,
                           size_t cap);

/* Bind the import address table.  The resolver returns the guest VA for a
 * (dll, name, ordinal) triple, or 0 when the import is unresolved.  Returns
 * the number of unresolved imports, or -1 for a malformed image. */
int wc3_pe_bind_imports(
    const Wc3PeImage *image,
    uint32_t (*resolve)(const char *dll, const char *name, uint16_t ordinal,
                        void *user),
    void *user);

#ifdef __cplusplus
}
#endif

#endif /* WC3_PE_H */
