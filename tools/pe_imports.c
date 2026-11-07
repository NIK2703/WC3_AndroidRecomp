/*
 * pe_imports - print the import address table slots of a 32-bit PE image.
 *
 * Diagnostic helper only.  It maps each IAT entry back to
 * "<dll>!<function>", which is what turns an indirect call site such as
 * "[0x6FA7C260]" into a named Win32 import.
 *
 * Usage: pe_imports <pe-image>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct {
    uint32_t va;
    uint32_t vsize;
    uint32_t rawptr;
    uint32_t rawsize;
} Section;

static const uint8_t *rva_to_ptr(const uint8_t *data, size_t len,
                                 const Section *secs, uint32_t nsec,
                                 uint32_t rva)
{
    for (uint32_t i = 0; i < nsec; i++) {
        const Section *s = &secs[i];
        uint32_t span = s->vsize > s->rawsize ? s->vsize : s->rawsize;
        if (rva >= s->va && rva - s->va < span) {
            uint32_t off = rva - s->va;
            if (off >= s->rawsize)
                return NULL;
            if ((uint64_t)s->rawptr + off >= (uint64_t)len)
                return NULL;
            return data + s->rawptr + off;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    FILE *f;
    long flen;
    uint8_t *data;
    uint32_t pe, optsz, nsec, image_base;
    uint32_t import_rva = 0, import_size = 0;
    Section *secs = NULL;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <pe-image>\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (flen = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "%s: cannot size file\n", argv[1]);
        fclose(f);
        return 2;
    }
    data = (uint8_t *)malloc((size_t)flen);
    if (!data || fread(data, 1, (size_t)flen, f) != (size_t)flen) {
        fprintf(stderr, "%s: cannot read file\n", argv[1]);
        free(data);
        fclose(f);
        return 2;
    }
    fclose(f);

    if ((size_t)flen < 0x40 || rd16(data) != 0x5a4d) {
        fprintf(stderr, "%s: not an MZ image\n", argv[1]);
        free(data);
        return 2;
    }
    pe = rd32(data + 0x3c);
    if ((size_t)pe + 24 > (size_t)flen || memcmp(data + pe, "PE\0\0", 4) != 0) {
        fprintf(stderr, "%s: not a PE image\n", argv[1]);
        free(data);
        return 2;
    }

    nsec = rd16(data + pe + 6);
    optsz = rd16(data + pe + 20);
    image_base = rd32(data + pe + 24 + 28);
    if ((size_t)pe + 24 + optsz + (size_t)nsec * 40 > (size_t)flen) {
        fprintf(stderr, "%s: truncated section table\n", argv[1]);
        free(data);
        return 2;
    }

    /* DataDirectory: PE32 is at optional-header offset 96, PE32+ at 112. */
    {
        uint32_t dd = (rd16(data + pe + 24) == 0x20b) ? 112u : 96u;
        uint32_t import_entry = pe + 24 + dd + 1 * 8;
        if ((size_t)import_entry + 8 <= (size_t)flen) {
            import_rva = rd32(data + import_entry);
            import_size = rd32(data + import_entry + 4);
        }
    }
    if (!import_rva) {
        fprintf(stderr, "%s: no import directory\n", argv[1]);
        free(data);
        return 1;
    }

    secs = (Section *)calloc(nsec, sizeof(*secs));
    if (!secs) {
        free(data);
        return 2;
    }
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *hdr = data + pe + 24 + optsz + (size_t)i * 40;
        secs[i].vsize = rd32(hdr + 8);
        secs[i].va = rd32(hdr + 12);
        secs[i].rawsize = rd32(hdr + 16);
        secs[i].rawptr = rd32(hdr + 20);
    }

    const uint8_t *desc = rva_to_ptr(data, (size_t)flen, secs, nsec, import_rva);
    if (!desc) {
        fprintf(stderr, "%s: import directory not mapped\n", argv[1]);
        free(secs);
        free(data);
        return 2;
    }

    for (uint32_t d = 0; d < import_size / 20; d++) {
        const uint8_t *de = desc + (size_t)d * 20;
        uint32_t oft = rd32(de + 0);
        uint32_t name_rva = rd32(de + 12);
        uint32_t ft = rd32(de + 16);
        const char *dll;
        const uint8_t *thunks;
        const uint8_t *thunk;

        if (!name_rva && !ft)
            break;
        dll = (const char *)rva_to_ptr(data, (size_t)flen, secs, nsec, name_rva);
        if (!dll)
            continue;
        thunks = rva_to_ptr(data, (size_t)flen, secs, nsec,
                            oft ? oft : ft);
        if (!thunks)
            continue;

        for (uint32_t i = 0;; i++) {
            uint32_t v;
            thunk = thunks + (size_t)i * 4;
            if ((uint64_t)(thunk - data) + 4 > (uint64_t)flen)
                break;
            v = rd32(thunk);
            if (!v)
                break;
            printf("%08X %s!", image_base + ft + i * 4, dll);
            if (v & 0x80000000u) {
                printf("#%u\n", v & 0xffffu);
            } else {
                const uint8_t *by_name =
                    rva_to_ptr(data, (size_t)flen, secs, nsec, v);
                if (by_name)
                    printf("%s\n", (const char *)(by_name + 2));
                else
                    printf("?bad-name@%08X\n", v);
            }
        }
    }

    free(secs);
    free(data);
    return 0;
}
