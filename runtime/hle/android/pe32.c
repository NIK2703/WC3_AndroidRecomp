/* Minimal PE32 mapper and import/export reader. */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t round_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[pe] cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "[pe] cannot read %s\n", path);
        free(b); fclose(f); return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

int pe_load(const char *path, uint32_t preferred_base, pe_image *out, int allow_relocate) {
    size_t flen = 0;
    uint8_t *file = slurp(path, &flen);
    if (!file) return 1;
    pe_dos_header *dos = (pe_dos_header *)file;
    if (flen < sizeof *dos || dos->e_magic != 0x5A4D) {
        fprintf(stderr, "[pe] %s: not MZ\n", path); free(file); return 1;
    }
    if ((size_t)dos->e_lfanew + sizeof(pe_nt_headers32) > flen) {
        fprintf(stderr, "[pe] %s: bad e_lfanew\n", path); free(file); return 1;
    }
    pe_nt_headers32 *nt = (pe_nt_headers32 *)(file + dos->e_lfanew);
    if (nt->Signature != 0x00004550 || nt->OptionalHeader.Magic != 0x10B) {
        fprintf(stderr, "[pe] %s: not PE32\n", path); free(file); return 1;
    }
    pe_opt_header32 *opt = &nt->OptionalHeader;
    uint32_t image_base = opt->ImageBase;
    uint32_t span = round_up(opt->SizeOfImage, 0x1000);
    uint32_t base = preferred_base ? preferred_base : image_base;
    if (base != image_base && !allow_relocate) {
        fprintf(stderr, "[pe] %s: requested base %08X != ImageBase %08X\n",
                path, base, image_base);
        free(file); return 1;
    }
    if (!guest_map(base, span)) {
        fprintf(stderr, "[pe] %s: cannot map at %08X (%X bytes)\n", path, base, span);
        free(file); return 1;
    }
    uint32_t hdr = opt->SizeOfHeaders;
    if (hdr > flen) hdr = (uint32_t)flen;
    memcpy((void *)(uintptr_t)base, file, hdr);

    pe_section_header *sec = (pe_section_header *)((uint8_t *)&nt->OptionalHeader
                                                   + nt->FileHeader.SizeOfOptionalHeader);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t copy = sec[i].SizeOfRawData;
        if (copy > sec[i].VirtualSize) copy = sec[i].VirtualSize;
        if (copy && (size_t)sec[i].PointerToRawData + copy <= flen)
            memcpy((void *)(uintptr_t)(base + sec[i].VirtualAddress),
                   file + sec[i].PointerToRawData, copy);
    }
    int32_t delta = (int32_t)(base - image_base);
    pe_data_dir rel = opt->DataDirectory[PE_DIR_BASERELOC];
    if (delta && rel.VirtualAddress && rel.Size) {
        uint32_t off = 0;
        while (off < rel.Size) {
            pe_base_reloc *blk = (pe_base_reloc *)(uintptr_t)(base + rel.VirtualAddress + off);
            if (blk->SizeOfBlock < sizeof *blk) break;
            unsigned n = (blk->SizeOfBlock - (unsigned)sizeof *blk) / 2;
            uint16_t *e = (uint16_t *)(blk + 1);
            for (unsigned i = 0; i < n; i++) {
                uint32_t type = e[i] >> 12, where = e[i] & 0xFFF;
                if (type == PE_REL_HIGHLOW) {
                    uint32_t *p = (uint32_t *)(uintptr_t)(base + blk->VirtualAddress + where);
                    *p += (uint32_t)delta;
                }
            }
            off += blk->SizeOfBlock;
        }
    }
    memset(out, 0, sizeof *out);
    out->base = base;
    out->image_base = image_base;
    out->span = span;
    out->entry = base + opt->AddressOfEntryPoint;
    out->import_rva = opt->DataDirectory[PE_DIR_IMPORT].VirtualAddress;
    out->export_rva = opt->DataDirectory[PE_DIR_EXPORT].VirtualAddress;
    out->export_size = opt->DataDirectory[PE_DIR_EXPORT].Size;
    out->reloc_rva = opt->DataDirectory[PE_DIR_BASERELOC].VirtualAddress;
    out->reloc_size = opt->DataDirectory[PE_DIR_BASERELOC].Size;
    out->resource_rva = opt->DataDirectory[PE_DIR_RESOURCE].VirtualAddress;
    out->resource_size = opt->DataDirectory[PE_DIR_RESOURCE].Size;
    out->stamp = nt->FileHeader.TimeDateStamp;
    out->path = path;
    free(file);
    fprintf(stderr, "[pe] %s -> %08X, entry %08X, span %X\n",
            path, base, out->entry, span);
    return 0;
}

static const pe_export_dir *export_dir(const pe_image *img) {
    if (!img->export_rva) return NULL;
    return (const pe_export_dir *)(uintptr_t)(img->base + img->export_rva);
}

uint32_t pe_export(const pe_image *img, const char *name) {
    const pe_export_dir *d = export_dir(img);
    if (!d) return 0;
    const uint32_t *funcs = (const uint32_t *)(uintptr_t)(img->base + d->AddressOfFunctions);
    const uint32_t *names = (const uint32_t *)(uintptr_t)(img->base + d->AddressOfNames);
    const uint16_t *ords = (const uint16_t *)(uintptr_t)(img->base + d->AddressOfNameOrdinals);
    for (uint32_t i = 0; i < d->NumberOfNames; i++) {
        const char *n = (const char *)(uintptr_t)(img->base + names[i]);
        if (strcmp(n, name) == 0) {
            uint32_t rva = funcs[ords[i]];
            if (rva >= img->export_rva && rva < img->export_rva + img->export_size) {
                fprintf(stderr, "[pe] %s: forwarder export %s ignored\n", img->path, name);
                return 0;
            }
            return img->base + rva;
        }
    }
    return 0;
}

uint32_t pe_export_ordinal(const pe_image *img, uint16_t ordinal) {
    const pe_export_dir *d = export_dir(img);
    if (!d) return 0;
    if (ordinal < d->Base) return 0;
    uint32_t index = (uint32_t)ordinal - d->Base;
    if (index >= d->NumberOfFunctions) return 0;
    const uint32_t *funcs = (const uint32_t *)(uintptr_t)(img->base + d->AddressOfFunctions);
    uint32_t rva = funcs[index];
    if (!rva) return 0;
    if (rva >= img->export_rva && rva < img->export_rva + img->export_size) {
        fprintf(stderr, "[pe] %s: forwarder ordinal %u ignored\n", img->path, ordinal);
        return 0;
    }
    return img->base + rva;
}

/* ---- resources: STRINGTABLE ---------------------------------------- */

typedef struct {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion, MinorVersion;
    uint16_t NumberOfNamedEntries, NumberOfIdEntries;
} res_dir;

typedef struct {
    uint32_t Name;          /* high bit set -> offset to a name string */
    uint32_t OffsetToData;  /* high bit set -> offset to a subdirectory */
} res_dir_entry;

typedef struct {
    uint32_t OffsetToData;  /* RVA */
    uint32_t Size;
    uint32_t CodePage;
    uint32_t Reserved;
} res_data_entry;

#define RT_STRING 6

/* Walk type -> name -> language and return the data entry, or NULL. */
static const res_data_entry *res_find(const pe_image *img, unsigned type,
                                      unsigned name)
{
    if (!img->resource_rva || !img->resource_size) return NULL;
    uint32_t root = img->base + img->resource_rva;
    const res_dir *d = (const res_dir *)(uintptr_t)root;
    const res_dir_entry *e = (const res_dir_entry *)(d + 1);

    const res_dir *found = NULL;
    unsigned total = d->NumberOfNamedEntries + d->NumberOfIdEntries;
    for (unsigned i = 0; i < total; i++) {
        if (!(e[i].Name & 0x80000000u) && (e[i].Name & 0xFFFFu) == type &&
            (e[i].OffsetToData & 0x80000000u)) {
            found = (const res_dir *)(uintptr_t)(root + (e[i].OffsetToData & 0x7FFFFFFFu));
            break;
        }
    }
    if (!found) return NULL;

    d = found;
    e = (const res_dir_entry *)(d + 1);
    total = d->NumberOfNamedEntries + d->NumberOfIdEntries;
    found = NULL;
    for (unsigned i = 0; i < total; i++) {
        if (!(e[i].Name & 0x80000000u) && (e[i].Name & 0xFFFFu) == name &&
            (e[i].OffsetToData & 0x80000000u)) {
            found = (const res_dir *)(uintptr_t)(root + (e[i].OffsetToData & 0x7FFFFFFFu));
            break;
        }
    }
    if (!found) return NULL;

    d = found;
    e = (const res_dir_entry *)(d + 1);
    total = d->NumberOfNamedEntries + d->NumberOfIdEntries;
    if (!total) return NULL;
    /* First language is enough: the launcher's messages are the same text. */
    return (const res_data_entry *)(uintptr_t)(root + e[0].OffsetToData);
}

int pe_string_resource(const pe_image *img, unsigned id, char *out, size_t cap)
{
    if (!out || cap < 2) return 0;
    out[0] = 0;
    const res_data_entry *de = res_find(img, RT_STRING, (id >> 4) + 1);
    if (!de) return 0;

    const uint8_t *blk = (const uint8_t *)(uintptr_t)(img->base + de->OffsetToData);
    uint32_t size = de->Size;
    unsigned want = id & 15;
    uint32_t off = 0;
    for (unsigned i = 0; i < 16 && off + 2 <= size; i++) {
        uint16_t len = (uint16_t)(blk[off] | (blk[off + 1] << 8));
        off += 2;
        if (off + (uint32_t)len * 2 > size) return 0;
        if (i == want) {
            size_t o = 0;
            for (uint16_t k = 0; k < len && o + 1 < cap; k++) {
                uint16_t w = (uint16_t)(blk[off + k * 2] | (blk[off + k * 2 + 1] << 8));
                out[o++] = (w < 0x80) ? (char)w : '?';   /* ASCII subset */
            }
            out[o] = 0;
            return (int)o;
        }
        off += (uint32_t)len * 2;
    }
    return 0;
}

int pe_bind_imports(const pe_image *img,
                    uint32_t (*resolve)(const char *, const char *, uint16_t, void *),
                    void *user) {
    if (!img->import_rva) return 0;
    unsigned bound = 0, missing = 0;
    pe_import_descriptor *d = (pe_import_descriptor *)(uintptr_t)(img->base + img->import_rva);
    for (; d->Name; d++) {
        const char *dll = (const char *)(uintptr_t)(img->base + d->Name);
        uint32_t *ilt = (uint32_t *)(uintptr_t)(img->base
                        + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        uint32_t *iat = (uint32_t *)(uintptr_t)(img->base + d->FirstThunk);
        for (; *ilt; ilt++, iat++) {
            uint32_t va = 0;
            if (*ilt & PE_ORDINAL_FLAG) {
                va = resolve(dll, NULL, (uint16_t)(*ilt & 0xFFFF), user);
                if (!va) { fprintf(stderr, "[pe] %s: unresolved %s#%u\n",
                                   img->path, dll, *ilt & 0xFFFF); missing++; }
            } else {
                const char *nm = (const char *)(uintptr_t)(img->base + *ilt + 2);
                va = resolve(dll, nm, 0, user);
                if (!va) { fprintf(stderr, "[pe] %s: unresolved %s!%s\n",
                                   img->path, dll, nm); missing++; }
            }
            if (va) bound++;
            *iat = va;
        }
    }
    fprintf(stderr, "[pe] %s: bound %u imports, %u unresolved\n", img->path, bound, missing);
    return (int)missing;
}
