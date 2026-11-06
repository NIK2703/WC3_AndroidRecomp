/*
 * pe.h - small read-only PE32 reader shared by the helper executables.
 *
 * The image is expected to be a dumped PE32 mapped at its preferred base
 * (for example build\war31_unpacked2.exe).  Only the headers and the .text
 * payload are loaded; this is not a loader and never executes code.
 */
#ifndef PE_H
#define PE_H

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define PE_MAX_SEC 96

typedef struct {
    char name[9];
    uint32_t vsize, va, rawsize, rawptr, chars;
} PeSec;

typedef struct {
    FILE *f;
    long long fsize;
    uint32_t base, image_size;
    int nsec;
    PeSec sec[PE_MAX_SEC];
    uint32_t text_va, text_size, text_off;
    uint8_t *text;
    /* Span of *every* executable section, not just .text.  Mss32.dll puts its
     * mixer in a second code section called MSSMIXER (21139000-2113D712), so a
     * tool that bounds itself to .text silently refuses to touch half of the
     * sound engine. */
    uint32_t code_lo, code_hi;
    int ncode;
} Pe;

#define PE_SCN_MEM_EXECUTE 0x20000000u

static inline uint16_t pe_rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t pe_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int pe_read_at(Pe *pe, long long off, void *buf, size_t n) {
    if (off < 0 || (long long)n > pe->fsize - off) return 0;
    if (fseek(pe->f, (long)off, SEEK_SET) != 0) return 0;
    return fread(buf, 1, n, pe->f) == n;
}

static inline int pe_open(Pe *pe, const char *path) {
    uint8_t hdr[0x40];
    uint8_t nt[24];
    uint8_t opt[0x400];
    memset(pe, 0, sizeof(*pe));
    pe->f = fopen(path, "rb");
    if (!pe->f) return 0;
    if (fseek(pe->f, 0, SEEK_END) != 0) return 0;
    pe->fsize = ftell(pe->f);
    if (pe->fsize < 0x100) return 0;
    if (!pe_read_at(pe, 0, hdr, sizeof(hdr))) return 0;
    if (hdr[0] != 'M' || hdr[1] != 'Z') return 0;
    uint32_t lfanew = pe_rd32(hdr + 0x3C);
    if (!pe_read_at(pe, lfanew, nt, sizeof(nt))) return 0;
    if (pe_rd32(nt) != 0x00004550u) return 0;
    uint16_t nsec = pe_rd16(nt + 6);
    uint16_t sizeopt = pe_rd16(nt + 20);
    if (nsec < 1 || nsec > PE_MAX_SEC || sizeopt < 96 || sizeopt > 0x400)
        return 0;
    if (!pe_read_at(pe, (long long)lfanew + 24, opt, sizeopt)) return 0;
    if (pe_rd16(opt) != 0x010B) return 0; /* PE32 only */
    pe->base = pe_rd32(opt + 28);
    pe->image_size = pe_rd32(opt + 56);
    pe->nsec = nsec;

    long long sec_off = (long long)lfanew + 24 + sizeopt;
    for (int i = 0; i < nsec; i++) {
        uint8_t s[40];
        if (!pe_read_at(pe, sec_off + (long long)i * 40, s, 40)) return 0;
        memcpy(pe->sec[i].name, s, 8);
        pe->sec[i].name[8] = 0;
        pe->sec[i].vsize = pe_rd32(s + 8);
        pe->sec[i].va = pe_rd32(s + 12);
        pe->sec[i].rawsize = pe_rd32(s + 16);
        pe->sec[i].rawptr = pe_rd32(s + 20);
        pe->sec[i].chars = pe_rd32(s + 36);
    }

    int text = -1;
    for (int i = 0; i < nsec; i++) {
        if (strncmp(pe->sec[i].name, ".text", 5) == 0) { text = i; break; }
    }
    if (text < 0) {
        for (int i = 0; i < nsec; i++) {
            if ((pe->sec[i].chars & 0x20000000u) && pe->sec[i].rawsize) {
                text = i;
                break;
            }
        }
    }
    if (text < 0) return 0;
    pe->text_va = pe->base + pe->sec[text].va;
    pe->text_off = pe->sec[text].rawptr;
    pe->text_size = pe->sec[text].vsize ? pe->sec[text].vsize
                                        : pe->sec[text].rawsize;
    if (pe->sec[text].rawsize && pe->text_size > pe->sec[text].rawsize)
        pe->text_size = pe->sec[text].rawsize;
    if (pe->text_size < 16) return 0;
    pe->text = (uint8_t *)malloc(pe->text_size ? pe->text_size : 1);
    if (!pe->text) return 0;
    if (!pe_read_at(pe, pe->text_off, pe->text, pe->text_size)) return 0;

    /* Executable-section span.  `text` above stays "the first code section"
     * because most images have exactly one and every existing caller assumes
     * that; these two extra fields answer "is this VA code?" for the rest. */
    pe->code_lo = 0;
    pe->code_hi = 0;
    pe->ncode = 0;
    for (int i = 0; i < nsec; i++) {
        if (!(pe->sec[i].chars & PE_SCN_MEM_EXECUTE)) continue;
        if (!pe->sec[i].rawsize) continue;
        uint32_t lo = pe->base + pe->sec[i].va;
        uint32_t hi = lo + pe->sec[i].rawsize;
        if (!pe->ncode || lo < pe->code_lo) pe->code_lo = lo;
        if (!pe->ncode || hi > pe->code_hi) pe->code_hi = hi;
        pe->ncode++;
    }
    return 1;
}

static inline void pe_close(Pe *pe) {
    if (!pe) return;
    if (pe->text) free(pe->text);
    if (pe->f) fclose(pe->f);
    pe->text = NULL;
    pe->f = NULL;
}

static inline int pe_va_to_off(Pe *pe, uint32_t va, uint32_t *off_out) {
    for (int i = 0; i < pe->nsec; i++) {
        uint32_t start = pe->base + pe->sec[i].va;
        uint32_t size = pe->sec[i].rawsize ? pe->sec[i].rawsize
                                           : pe->sec[i].vsize;
        if (va >= start && va - start < size) {
            *off_out = pe->sec[i].rawptr + (va - start);
            return 1;
        }
    }
    return 0;
}

static inline int pe_in_text(Pe *pe, uint32_t va) {
    return va >= pe->text_va && va - pe->text_va < pe->text_size;
}

static inline int pe_read_u32(Pe *pe, uint32_t va, uint32_t *out) {
    if (pe_in_text(pe, va)) {
        *out = pe_rd32(pe->text + (va - pe->text_va));
        return 1;
    }
    uint32_t off;
    if (!pe_va_to_off(pe, va, &off)) return 0;
    uint8_t b[4];
    if (!pe_read_at(pe, off, b, 4)) return 0;
    *out = pe_rd32(b);
    return 1;
}

/* Section containing VA.  Returns index or -1. */
static inline int pe_section_for_va(Pe *pe, uint32_t va) {
    for (int i = 0; i < pe->nsec; i++) {
        uint32_t start = pe->base + pe->sec[i].va;
        uint32_t size = pe->sec[i].vsize ? pe->sec[i].vsize
                                         : pe->sec[i].rawsize;
        if (va >= start && va - start < size) return i;
    }
    return -1;
}

/* Index of the *executable* section holding VA, or -1.  Differs from
 * pe_section_for_va in that it requires IMAGE_SCN_MEM_EXECUTE and file-backed
 * bytes, so callers can trust the result to be decodable code. */
static inline int pe_code_section_for_va(Pe *pe, uint32_t va) {
    for (int i = 0; i < pe->nsec; i++) {
        if (!(pe->sec[i].chars & PE_SCN_MEM_EXECUTE)) continue;
        if (!pe->sec[i].rawsize) continue;
        uint32_t start = pe->base + pe->sec[i].va;
        if (va >= start && va - start < pe->sec[i].rawsize) return i;
    }
    return -1;
}

/* Exclusive end VA of the executable section holding VA, or 0. */
static inline uint32_t pe_code_end(Pe *pe, uint32_t va) {
    int i = pe_code_section_for_va(pe, va);
    if (i < 0) return 0;
    return pe->base + pe->sec[i].va + pe->sec[i].rawsize;
}

/* File offset of VA inside its executable section, or -1. */
static inline long long pe_code_off(Pe *pe, uint32_t va) {
    int i = pe_code_section_for_va(pe, va);
    if (i < 0) return -1;
    return (long long)pe->sec[i].rawptr + (va - (pe->base + pe->sec[i].va));
}

#endif /* PE_H */
