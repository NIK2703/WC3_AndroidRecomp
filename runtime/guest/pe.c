/*
 * pe.c - minimal PE32 mapper and import/export reader for the Android guest.
 *
 * The file is read through Wc3FileApi; the image is mapped through
 * wc3_guest_map(), so the loader has no POSIX or stdio dependency on the
 * production path.  Validation is deliberately conservative: a malformed
 * header or a section that would leave the mapped span is rejected before it
 * can write outside the image.
 */
#define _GNU_SOURCE 1

#include "pe.h"

#include "guest_memory.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WC3_DEBUG
#define WC3_DEBUG 0
#endif

#define PE_MAX_FILE (256u * 1024u * 1024u)

static void pe_log(const Wc3Platform *platform, int priority, const char *fmt,
                   ...)
{
#if WC3_DEBUG
    if (!platform || !platform->log.write) return;
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    platform->log.write(platform->log.ctx, priority, "wc3pe", text);
#else
    (void)platform;
    (void)priority;
    (void)fmt;
#endif
}

static uint32_t round_up(uint32_t value, uint32_t alignment)
{
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) return 0;
    if (value > UINT32_MAX - (alignment - 1u)) return 0;
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static uint16_t load_u16(const void *address)
{
    uint16_t value;
    memcpy(&value, address, sizeof value);
    return value;
}

static uint32_t load_u32(const void *address)
{
    uint32_t value;
    memcpy(&value, address, sizeof value);
    return value;
}

static void store_u16(void *address, uint16_t value)
{
    memcpy(address, &value, sizeof value);
}

static void store_u32(void *address, uint32_t value)
{
    memcpy(address, &value, sizeof value);
}

/* A checked RVA range inside the mapped image.  rva == span with size == 0 is
 * the empty tail; callers that read bytes must pass size > 0. */
static int pe_range_ok(const Wc3PeImage *image, uint32_t rva, uint64_t size)
{
    return image && image->base && (uint64_t)rva <= image->span &&
           size <= (uint64_t)image->span - rva;
}

/* True only when a NUL-terminated string lies entirely inside the image. */
static int pe_cstring_ok(const Wc3PeImage *image, uint32_t rva)
{
    if (!image || !image->base || rva >= image->span) return 0;
    const char *text = (const char *)(uintptr_t)(image->base + rva);
    return memchr(text, '\0', image->span - rva) != NULL;
}

static int pe_data_dir_ok(const pe_data_dir *dir, uint32_t span)
{
    if (!dir) return 0;
    if (dir->VirtualAddress == 0u) return dir->Size == 0u;
    return dir->VirtualAddress < span &&
           (uint64_t)dir->VirtualAddress + dir->Size <= span;
}

static uint8_t *read_file(const Wc3Platform *platform, const char *path,
                          size_t *out_len)
{
    if (!platform || !platform->file.open || !platform->file.size ||
        !platform->file.read || !platform->file.close || !path || !out_len)
        return NULL;

    uintptr_t file = 0;
    if (platform->file.open(platform->file.ctx, path, WC3_FILE_READ, &file) !=
        0)
        return NULL;
    uint64_t size64 = 0;
    if (platform->file.size(platform->file.ctx, file, &size64) != 0 ||
        size64 == 0u || size64 > PE_MAX_FILE) {
        (void)platform->file.close(platform->file.ctx, file);
        return NULL;
    }

    size_t size = (size_t)size64;
    uint8_t *bytes = (uint8_t *)malloc(size);
    if (!bytes) {
        (void)platform->file.close(platform->file.ctx, file);
        return NULL;
    }

    size_t done = 0;
    while (done < size) {
        uint32_t want = (uint32_t)(size - done);
        uint32_t got = 0;
        if (platform->file.read(platform->file.ctx, file, bytes + done, want,
                                &got) != 0 ||
            got == 0u)
            break;
        done += got;
    }
    (void)platform->file.close(platform->file.ctx, file);
    if (done != size) {
        free(bytes);
        return NULL;
    }
    *out_len = size;
    return bytes;
}

static const uint8_t *section_table(const uint8_t *file, size_t file_len,
                                    const pe_nt_headers32 *nt)
{
    const uint8_t *table =
        (const uint8_t *)&nt->OptionalHeader + nt->FileHeader.SizeOfOptionalHeader;
    size_t count = nt->FileHeader.NumberOfSections;
    if (count > 96u) return NULL;
    size_t table_off = (size_t)(table - file);
    if (table_off > file_len ||
        count > (file_len - table_off) / sizeof(pe_section_header))
        return NULL;
    return table;
}

int wc3_pe_load(const Wc3Platform *platform, const char *path,
                uint32_t preferred_base, Wc3PeImage *out, int allow_relocate)
{
    if (!platform || !path || !out) return -1;
    memset(out, 0, sizeof *out);
    out->path = path;

    uint32_t base = 0;
    uint32_t span = 0;
    int mapped = 0;

    size_t file_len = 0;
    uint8_t *file = read_file(platform, path, &file_len);
    if (!file) {
        pe_log(platform, 6, "cannot read %s", path);
        return -1;
    }

    if (file_len < sizeof(pe_dos_header)) goto malformed;
    const pe_dos_header *dos = (const pe_dos_header *)file;
    if (dos->e_magic != 0x5A4Du) goto malformed;
    if (dos->e_lfanew < 0 ||
        (size_t)dos->e_lfanew + sizeof(pe_nt_headers32) > file_len)
        goto malformed;

    const pe_nt_headers32 *nt =
        (const pe_nt_headers32 *)(file + (size_t)dos->e_lfanew);
    if (nt->Signature != 0x00004550u || nt->OptionalHeader.Magic != 0x10Bu ||
        nt->FileHeader.Machine != 0x014Cu)
        goto malformed;
    if (nt->FileHeader.SizeOfOptionalHeader < sizeof(pe_opt_header32) ||
        (uint64_t)dos->e_lfanew + 4u + sizeof(pe_file_header) +
                nt->FileHeader.SizeOfOptionalHeader >
            file_len)
        goto malformed;

    const pe_opt_header32 *opt = &nt->OptionalHeader;
    uint32_t image_base = opt->ImageBase;
    uint32_t headers = opt->SizeOfHeaders;
    span = round_up(opt->SizeOfImage, 0x1000u);
    if (span == 0u || headers == 0u || headers > file_len ||
        headers > span || headers > opt->SizeOfImage)
        goto malformed;
    if ((uint64_t)image_base + span > 0x100000000ull) goto malformed;

    base = preferred_base ? preferred_base : image_base;
    if (base != image_base && !allow_relocate) {
        pe_log(platform, 6, "%s: requested base %08X != ImageBase %08X", path,
               base, image_base);
        free(file);
        return -1;
    }
    if ((uint64_t)base + span > 0x100000000ull) goto malformed;
    if (opt->AddressOfEntryPoint >= span) goto malformed;

    const uint8_t *sections = section_table(file, file_len, nt);
    if (!sections) goto malformed;

    if (wc3_guest_map(base, span) != 0) {
        pe_log(platform, 6, "%s: cannot map at %08X (%X bytes)", path, base,
               span);
        free(file);
        return -1;
    }
    mapped = 1;

    memcpy((void *)(uintptr_t)base, file, headers);

    const pe_section_header *section = (const pe_section_header *)sections;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint64_t va_end = (uint64_t)section[i].VirtualAddress +
                          section[i].VirtualSize;
        if ((uint64_t)section[i].VirtualAddress > span || va_end > span)
            goto malformed;
        if (section[i].SizeOfRawData != 0u &&
            (uint64_t)section[i].PointerToRawData +
                    section[i].SizeOfRawData >
                file_len)
            goto malformed;

        uint32_t copy = section[i].SizeOfRawData;
        if (copy > section[i].VirtualSize) copy = section[i].VirtualSize;
        if (copy == 0u) continue;
        memcpy((void *)(uintptr_t)(base + section[i].VirtualAddress),
               file + section[i].PointerToRawData, copy);
    }

    pe_data_dir import_dir = opt->DataDirectory[PE_DIR_IMPORT];
    pe_data_dir export_dir = opt->DataDirectory[PE_DIR_EXPORT];
    pe_data_dir resource_dir = opt->DataDirectory[PE_DIR_RESOURCE];
    pe_data_dir rel = opt->DataDirectory[PE_DIR_BASERELOC];
    if (!pe_data_dir_ok(&import_dir, span) ||
        !pe_data_dir_ok(&export_dir, span) ||
        !pe_data_dir_ok(&resource_dir, span) ||
        !pe_data_dir_ok(&rel, span))
        goto malformed;

    int32_t delta = (int32_t)(base - image_base);
    if (delta != 0) {
        if (rel.VirtualAddress == 0u || rel.Size < sizeof(pe_base_reloc)) {
            pe_log(platform, 6, "%s: no relocations for base %08X", path,
                   base);
            goto malformed;
        }
        uint32_t off = 0;
        while (off < rel.Size) {
            if (rel.Size - off < sizeof(pe_base_reloc)) goto malformed;
            uint64_t block_rva64 = (uint64_t)rel.VirtualAddress + off;
            if (block_rva64 > span || span - block_rva64 < sizeof(pe_base_reloc))
                goto malformed;
            const uint8_t *block =
                (const uint8_t *)(uintptr_t)(base + (uint32_t)block_rva64);
            uint32_t block_va = load_u32(block);
            uint32_t block_size = load_u32(block + 4);
            if (block_size < sizeof(pe_base_reloc) ||
                (block_size & 1u) != 0u ||
                block_size > rel.Size - off)
                goto malformed;

            unsigned count =
                (block_size - (unsigned)sizeof(pe_base_reloc)) / 2u;
            const uint8_t *entries = block + sizeof(pe_base_reloc);
            for (unsigned i = 0; i < count; i++) {
                uint16_t entry = load_u16(entries + i * 2u);
                uint32_t type = entry >> 12;
                uint32_t where = entry & 0x0FFFu;
                if (type == PE_REL_ABSOLUTE) continue;

                uint64_t target = (uint64_t)block_va + where;
                uint64_t width = type == PE_REL_HIGHLOW ? 4u : 2u;
                if (target > span || width > (uint64_t)span - target)
                    goto malformed;
                void *p = (void *)(uintptr_t)(base + (uint32_t)target);
                switch (type) {
                case PE_REL_HIGH:
                    store_u16(p, (uint16_t)(load_u16(p) +
                                            (uint16_t)((uint32_t)delta >> 16)));
                    break;
                case PE_REL_LOW:
                    store_u16(p, (uint16_t)(load_u16(p) +
                                            (uint16_t)((uint32_t)delta &
                                                       0xFFFFu)));
                    break;
                case PE_REL_HIGHLOW:
                    store_u32(p, load_u32(p) + (uint32_t)delta);
                    break;
                default:
                    pe_log(platform, 6, "%s: unsupported relocation type %u",
                           path, type);
                    goto malformed;
                }
            }
            off += block_size;
        }
    }

    out->base = base;
    out->image_base = image_base;
    out->span = span;
    out->entry = base + opt->AddressOfEntryPoint;
    out->import_rva = import_dir.VirtualAddress;
    out->export_rva = export_dir.VirtualAddress;
    out->export_size = export_dir.Size;
    out->reloc_rva = rel.VirtualAddress;
    out->reloc_size = rel.Size;
    out->resource_rva = resource_dir.VirtualAddress;
    out->resource_size = resource_dir.Size;
    out->stamp = nt->FileHeader.TimeDateStamp;

    pe_log(platform, 4, "%s -> %08X, entry %08X, span %X", path, base,
           out->entry, span);
    free(file);
    return 0;

malformed:
    if (mapped) (void)wc3_guest_unmap(base, span);
    pe_log(platform, 6, "%s: malformed PE32", path);
    free(file);
    return -1;
}

static int export_range_ok(const Wc3PeImage *image, uint32_t rva,
                           uint64_t size)
{
    if (!image || !image->base || image->export_rva == 0u ||
        image->export_size == 0u)
        return 0;
    uint64_t start = image->export_rva;
    uint64_t end = start + image->export_size;
    if (end > image->span || rva < start ||
        (uint64_t)rva + size > end)
        return 0;
    return 1;
}

static int export_cstring_ok(const Wc3PeImage *image, uint32_t rva)
{
    if (!export_range_ok(image, rva, 1u)) return 0;
    uint64_t end = (uint64_t)image->export_rva + image->export_size;
    const char *text = (const char *)(uintptr_t)(image->base + rva);
    return memchr(text, '\0', (size_t)(end - rva)) != NULL;
}

static const pe_export_dir *export_dir(const Wc3PeImage *image)
{
    if (!export_range_ok(image, image->export_rva,
                         sizeof(pe_export_dir)))
        return NULL;
    return (const pe_export_dir *)(uintptr_t)(image->base + image->export_rva);
}

static uint32_t export_function_rva(const Wc3PeImage *image,
                                    const pe_export_dir *dir,
                                    uint32_t function_index)
{
    if (!dir || function_index >= dir->NumberOfFunctions) return 0;
    uint64_t bytes = (uint64_t)dir->NumberOfFunctions * sizeof(uint32_t);
    if (!export_range_ok(image, dir->AddressOfFunctions, bytes)) return 0;
    return load_u32((const void *)(uintptr_t)(image->base +
                                              dir->AddressOfFunctions +
                                              function_index * 4u));
}

uint32_t wc3_pe_export(const Wc3PeImage *image, const char *name)
{
    if (!image || !name) return 0;
    const pe_export_dir *dir = export_dir(image);
    if (!dir || dir->NumberOfFunctions == 0u || dir->NumberOfNames == 0u)
        return 0;
    if (dir->NumberOfNames > dir->NumberOfFunctions) return 0;

    uint64_t name_bytes = (uint64_t)dir->NumberOfNames * sizeof(uint32_t);
    uint64_t ordinal_bytes = (uint64_t)dir->NumberOfNames * sizeof(uint16_t);
    if (!export_range_ok(image, dir->AddressOfNames, name_bytes) ||
        !export_range_ok(image, dir->AddressOfNameOrdinals, ordinal_bytes))
        return 0;

    for (uint32_t i = 0; i < dir->NumberOfNames; i++) {
        uint32_t name_rva = load_u32(
            (const void *)(uintptr_t)(image->base + dir->AddressOfNames +
                                      i * 4u));
        if (!export_cstring_ok(image, name_rva)) return 0;
        const char *candidate =
            (const char *)(uintptr_t)(image->base + name_rva);
        if (strcmp(candidate, name) != 0) continue;

        uint16_t ordinal = load_u16(
            (const void *)(uintptr_t)(image->base +
                                      dir->AddressOfNameOrdinals + i * 2u));
        uint32_t rva = export_function_rva(image, dir, ordinal);
        if (rva == 0u) return 0;
        if (rva >= image->export_rva &&
            rva < image->export_rva + image->export_size) {
            pe_log(NULL, 5, "forwarder export %s ignored", name);
            return 0;
        }
        if (!pe_range_ok(image, rva, 1u)) return 0;
        return image->base + rva;
    }
    return 0;
}

uint32_t wc3_pe_export_ordinal(const Wc3PeImage *image, uint16_t ordinal)
{
    if (!image) return 0;
    const pe_export_dir *dir = export_dir(image);
    if (!dir || ordinal < dir->Base) return 0;
    uint32_t index = (uint32_t)ordinal - dir->Base;
    uint32_t rva = export_function_rva(image, dir, index);
    if (rva == 0u) return 0;
    if (rva >= image->export_rva &&
        rva < image->export_rva + image->export_size) {
        pe_log(NULL, 5, "forwarder ordinal %u ignored", ordinal);
        return 0;
    }
    if (!pe_range_ok(image, rva, 1u)) return 0;
    return image->base + rva;
}

typedef struct {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint16_t NumberOfNamedEntries;
    uint16_t NumberOfIdEntries;
} res_dir;

typedef struct {
    uint32_t Name;
    uint32_t OffsetToData;
} res_dir_entry;

typedef struct {
    uint32_t OffsetToData;
    uint32_t Size;
    uint32_t CodePage;
    uint32_t Reserved;
} res_data_entry;

#define RT_STRING 6u

static int res_view(const Wc3PeImage *image, uint32_t offset,
                    res_dir *out_dir, const res_dir_entry **out_entries)
{
    if (!image || !image->base || image->resource_rva == 0u ||
        image->resource_size < sizeof(res_dir))
        return 0;
    if (!pe_range_ok(image, image->resource_rva, image->resource_size))
        return 0;
    if (offset > image->resource_size ||
        image->resource_size - offset < sizeof(res_dir))
        return 0;

    const uint8_t *root =
        (const uint8_t *)(uintptr_t)(image->base + image->resource_rva);
    memcpy(out_dir, root + offset, sizeof *out_dir);
    uint64_t count = (uint64_t)out_dir->NumberOfNamedEntries +
                     out_dir->NumberOfIdEntries;
    uint64_t available = (uint64_t)image->resource_size - offset -
                         sizeof(res_dir);
    if (count > available / sizeof(res_dir_entry)) return 0;
    *out_entries = (const res_dir_entry *)(const void *)(root + offset +
                                                        sizeof(res_dir));
    return 1;
}

static uint32_t res_entry_name(const res_dir_entry *entries, uint32_t index)
{
    return load_u32((const uint8_t *)(const void *)entries +
                    (size_t)index * sizeof(res_dir_entry));
}

static uint32_t res_entry_offset(const res_dir_entry *entries, uint32_t index)
{
    return load_u32((const uint8_t *)(const void *)entries +
                    (size_t)index * sizeof(res_dir_entry) + 4u);
}

static const res_data_entry *res_find(const Wc3PeImage *image, unsigned type,
                                      unsigned name)
{
    if (!image || !image->base || image->resource_rva == 0u ||
        image->resource_size == 0u)
        return NULL;

    uint32_t offset = 0;
    uint32_t data_offset = 0;
    for (unsigned level = 0; level < 3u; level++) {
        res_dir dir;
        const res_dir_entry *entries = NULL;
        if (!res_view(image, offset, &dir, &entries)) return NULL;

        uint32_t total = (uint32_t)dir.NumberOfNamedEntries +
                         dir.NumberOfIdEntries;
        unsigned want = level == 0u ? type : name;
        int found = 0;
        for (uint32_t i = 0; i < total; i++) {
            uint32_t entry_name = res_entry_name(entries, i);
            if (entry_name & 0x80000000u) continue;
            if ((entry_name & 0xFFFFu) != want) continue;
            uint32_t entry_offset = res_entry_offset(entries, i);
            if (level < 2u) {
                if (!(entry_offset & 0x80000000u)) return NULL;
                offset = entry_offset & 0x7FFFFFFFu;
            } else {
                if (entry_offset & 0x80000000u) return NULL;
                data_offset = entry_offset;
            }
            found = 1;
            break;
        }
        if (!found) return NULL;
    }

    if (data_offset > image->resource_size ||
        image->resource_size - data_offset < sizeof(res_data_entry))
        return NULL;
    uint64_t data_rva = (uint64_t)image->resource_rva + data_offset;
    if (data_rva > UINT32_MAX ||
        !pe_range_ok(image, (uint32_t)data_rva, sizeof(res_data_entry)))
        return NULL;
    return (const res_data_entry *)(uintptr_t)(image->base + data_rva);
}

int wc3_pe_string_resource(const Wc3PeImage *image, unsigned id, char *out,
                           size_t cap)
{
    if (!out || cap < 2u) return 0;
    out[0] = '\0';
    const res_data_entry *data = res_find(image, RT_STRING, (id >> 4) + 1u);
    if (!data) return 0;

    res_data_entry resource;
    memcpy(&resource, data, sizeof resource);
    if (!pe_range_ok(image, resource.OffsetToData, resource.Size)) return 0;
    const uint8_t *block =
        (const uint8_t *)(uintptr_t)(image->base + resource.OffsetToData);
    uint32_t size = resource.Size;
    unsigned want = id & 15u;
    uint32_t off = 0;
    for (unsigned i = 0; i < 16u && off + 2u <= size; i++) {
        uint16_t len = load_u16(block + off);
        off += 2u;
        if ((uint32_t)len > (size - off) / 2u) return 0;
        if (i == want) {
            size_t o = 0;
            for (uint16_t k = 0; k < len && o + 1u < cap; k++) {
                uint16_t w = load_u16(block + off + (uint32_t)k * 2u);
                out[o++] = (w < 0x80u) ? (char)w : '?';
            }
            out[o] = '\0';
            return (int)o;
        }
        off += (uint32_t)len * 2u;
    }
    return 0;
}

int wc3_pe_bind_imports(
    const Wc3PeImage *image,
    uint32_t (*resolve)(const char *dll, const char *name, uint16_t ordinal,
                        void *user),
    void *user)
{
    if (!image || !resolve || !image->base || image->span == 0u) return -1;
    if (!image->import_rva) return 0;
    if (!pe_range_ok(image, image->import_rva,
                     sizeof(pe_import_descriptor)))
        return -1;

    uint32_t span = image->span;
    uint32_t import_rva = image->import_rva;
    uint32_t max_descriptors =
        (span - import_rva) / (uint32_t)sizeof(pe_import_descriptor);
    unsigned bound = 0;
    unsigned missing = 0;
    int terminated = 0;

    for (uint32_t i = 0; i < max_descriptors; i++) {
        uint32_t descriptor_rva =
            import_rva + i * (uint32_t)sizeof(pe_import_descriptor);
        pe_import_descriptor descriptor;
        memcpy(&descriptor,
               (const void *)(uintptr_t)(image->base + descriptor_rva),
               sizeof descriptor);
        if (descriptor.Name == 0u && descriptor.FirstThunk == 0u &&
            descriptor.OriginalFirstThunk == 0u) {
            terminated = 1;
            break;
        }
        if (descriptor.Name == 0u || descriptor.FirstThunk == 0u) return -1;
        if (!pe_cstring_ok(image, descriptor.Name)) return -1;
        const char *dll =
            (const char *)(uintptr_t)(image->base + descriptor.Name);

        uint32_t lookup_rva = descriptor.OriginalFirstThunk
                                  ? descriptor.OriginalFirstThunk
                                  : descriptor.FirstThunk;
        if (lookup_rva == 0u ||
            !pe_range_ok(image, lookup_rva, sizeof(uint32_t)) ||
            !pe_range_ok(image, descriptor.FirstThunk,
                         sizeof(uint32_t)))
            return -1;

        uint64_t thunk_rva = lookup_rva;
        uint64_t iat_rva = descriptor.FirstThunk;
        for (;;) {
            if (thunk_rva > span || span - thunk_rva < sizeof(uint32_t) ||
                iat_rva > span || span - iat_rva < sizeof(uint32_t))
                return -1;

            uint32_t lookup =
                load_u32((const void *)(uintptr_t)(image->base +
                                                   (uint32_t)thunk_rva));
            if (lookup == 0u) {
                store_u32((void *)(uintptr_t)(image->base +
                                              (uint32_t)iat_rva),
                          0u);
                break;
            }

            uint32_t va = 0;
            if (lookup & PE_ORDINAL_FLAG) {
                va = resolve(dll, NULL, (uint16_t)(lookup & 0xFFFFu), user);
            } else {
                uint64_t name_rva = (uint64_t)lookup + 2u;
                if (name_rva > UINT32_MAX ||
                    !pe_cstring_ok(image, (uint32_t)name_rva))
                    return -1;
                va = resolve(
                    dll, (const char *)(uintptr_t)(image->base + name_rva),
                    0u, user);
            }
            if (va)
                bound++;
            else
                missing++;
            store_u32((void *)(uintptr_t)(image->base + (uint32_t)iat_rva),
                      va);

            if (thunk_rva > UINT32_MAX - 4u || iat_rva > UINT32_MAX - 4u)
                return -1;
            thunk_rva += 4u;
            iat_rva += 4u;
        }
    }

    if (!terminated) return -1;
    pe_log(NULL, 4, "%s: bound %u imports, %u unresolved", image->path, bound,
           missing);
    return (int)missing;
}
