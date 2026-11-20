/* recomp_ratio.c - static instruction-count ratio per lifted function.
 *
 *   recomp_ratio --arm64 <libwc3gameactivity.so> --x86 <pe> [--x86 <pe> ...]
 *                [--funcs <funcs.txt>] [--funcs-dir <dir>]
 *                [--top N] [--min-x86 N] [--min-arm64 N]
 *                [--json <path>] [--csv <path>] [--self-test]
 *
 * The guest side is an x86-32 PE image (the licensed game files).  The
 * recompiled side is the unstripped arm64 libwc3gameactivity.so, whose
 * `L_<VA>` symbols are one lifted function each.  The tool matches both sides
 * by the guest VA encoded in the symbol name and reports arm64/x86 instruction
 * counts as a percentage.  Higher percentages mean the recompiler emitted more
 * instructions for the same guest function.
 *
 * Why capstone for the guest side: the lifter already decodes x86 with
 * capstone, so instruction boundaries and operand lengths must not differ
 * between the emitted C and this analysis.  The arm64 side is fixed-width, so
 * st_size/4 is exact for a function symbol and does not need a second decoder.
 *
 * Exact extents: the final corpus lift uses per-function byte extents from
 * functions.txt plus the closure round files (closed.rN.txt), not a single
 * global size.  Pass them with --funcs / --funcs-dir to reproduce the emitted
 * function exactly.  Without them the tool uses the merged corpus default of
 * 16 bytes and the lifter's block-growth heuristic; that is a useful estimate
 * but not the same input the final lift saw.
 *
 * The tool never runs the lifter and never writes into the build tree.  All
 * inputs are read-only; reports go to stdout or to explicit --json/--csv paths.
 */
#define NOMINMAX 1
#include "csglue.h"

#include "dirwalk.h"
#include "sha256.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOOL_NAME "recomp_ratio"
#define TOOL_SCHEMA 1

#define MAX_X86_IMAGES 32
#define MAX_FUNCS_ARGS 64
#define MAX_FUNCS_DIRS 16
#define PATH_CAP 4096

#define IMAGE_SCN_MEM_EXECUTE 0x20000000u
#define SHT_SYMTAB 2u
#define SHT_DYNSYM 11u
#define SHF_EXECINSTR 0x4u
#define STT_FUNC 2u
#define SHN_UNDEF 0u

#define DEFAULT_INIT_EXTENT 16u
#define MAX_INIT_EXTENT (1u << 20)

/* x86 instruction length is at most 15 bytes. */
#define X86_MAX_INSN 15u
/* The lifter's growth chunk is bounded by 300 bytes; keep the same bound. */
#define GROW_CHUNK 300u

typedef struct {
    char name[9];
    uint32_t va;
    uint32_t vsize;
    uint32_t rawptr;
    uint32_t rawsize;
    uint32_t chars;
} PeSection;

typedef struct {
    char path[PATH_CAP];
    char sha256[65];
    uint8_t *data;
    size_t size;
    uint32_t base;
    uint32_t image_size;
    uint16_t machine;
    PeSection *secs;
    size_t nsec;
    size_t exec_sections;
    int owns_data;
} PeImage;

typedef struct {
    char name[16];
    uint32_t guest;
    uint32_t arm_bytes;
    uint32_t arm_insns;
} ArmFunc;

typedef struct {
    char path[PATH_CAP];
    char sha256[65];
    uint8_t *data;
    size_t size;
    ArmFunc *funcs;
    size_t nfuncs;
    int owns_data;
} ElfImage;

typedef struct {
    uint32_t addr;
    uint32_t size;
    uint32_t seq;
} FuncSize;

typedef struct {
    char path[PATH_CAP];
    char sha256[65];
} FuncsFile;

typedef struct {
    FuncSize *v;
    size_t n;
    size_t cap;
    FuncsFile *files;
    size_t nfiles;
    size_t fcap;
    uint32_t seq;
} FuncMap;

typedef struct {
    uint32_t addr;
    uint16_t size;
    uint8_t term;
    uint8_t ret;
    uint8_t jmp_indirect;
} XInsn;

typedef struct {
    char name[16];
    uint32_t guest;
    uint32_t x86_insns;
    uint32_t arm_insns;
    uint32_t x86_bytes;
    uint16_t pe_index;
} Result;

typedef struct {
    size_t arm_functions;
    size_t matched;
    size_t included;
    size_t skipped_filter;
    size_t no_image;
    size_t not_in_funcs;
    size_t decode_failed;
    size_t ambiguous;
    uint64_t sum_x86;
    uint64_t sum_arm;
    double mean_ratio;
    double median_ratio;
    double p90_ratio;
    double max_ratio;
    double weighted_ratio;
} Stats;

typedef struct {
    const char *arm_path;
    const char *x86_paths[MAX_X86_IMAGES];
    size_t nx86;
    const char *funcs_paths[MAX_FUNCS_ARGS];
    size_t nfuncs;
    const char *funcs_dirs[MAX_FUNCS_DIRS];
    size_t nfuncs_dirs;
    uint32_t init_extent;
    unsigned top;
    unsigned min_x86;
    unsigned min_arm64;
    const char *json_path;
    const char *csv_path;
    int verbose;
} Options;

static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", TOOL_NAME);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

static void warn(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: warning: ", TOOL_NAME);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory (%llu bytes)", (unsigned long long)n);
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory (%llu bytes)", (unsigned long long)n);
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static const char *path_basename(const char *path)
{
    const char *a = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    const char *p = a > b ? a : b;
    return p ? p + 1 : path;
}

static int read_file(const char *path, uint8_t **out, size_t *out_size)
{
    FILE *f;
    long sz;
    uint8_t *buf;

    *out = NULL;
    *out_size = 0;
    f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    buf = (uint8_t *)xmalloc((size_t)sz + 1);
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    buf[sz] = 0;
    fclose(f);
    *out = buf;
    *out_size = (size_t)sz;
    return 0;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

/* ---------------------------------------------------------------- PE image */

static int pe_parse_buffer(PeImage *pe, const char *path, uint8_t *data,
                           size_t size, int owns_data)
{
    uint32_t peoff, optoff;
    uint16_t optsz, nsec, magic, i;
    uint8_t *opt;

    memset(pe, 0, sizeof *pe);
    snprintf(pe->path, sizeof pe->path, "%s", path);
    pe->data = data;
    pe->size = size;
    pe->owns_data = owns_data;

    if (size < 0x40 || data[0] != 'M' || data[1] != 'Z')
        return -1;
    peoff = rd32(data + 0x3C);
    if (peoff > size || size - peoff < 24)
        return -1;
    if (memcmp(data + peoff, "PE\0\0", 4) != 0)
        return -1;
    pe->machine = rd16(data + peoff + 4);
    nsec = rd16(data + peoff + 6);
    optsz = rd16(data + peoff + 20);
    optoff = peoff + 24;
    if (optoff > size || size - optoff < optsz || optsz < 0x60)
        return -1;
    opt = data + optoff;
    magic = rd16(opt);
    if (magic != 0x10B) {
        warn("%s: PE optional header magic 0x%04X is not PE32; skipped",
             path, magic);
        return -1;
    }
    if ((uint64_t)optoff + optsz + (uint64_t)nsec * 40 > size)
        return -1;

    pe->base = rd32(opt + 28);
    pe->image_size = rd32(opt + 56);
    pe->nsec = nsec;
    pe->secs = (PeSection *)xmalloc((size_t)nsec * sizeof *pe->secs);
    for (i = 0; i < nsec; i++) {
        const uint8_t *sh = data + optoff + optsz + (size_t)i * 40;
        PeSection *s = &pe->secs[i];
        memcpy(s->name, sh, 8);
        s->name[8] = 0;
        s->vsize = rd32(sh + 8);
        s->va = rd32(sh + 12);
        s->rawsize = rd32(sh + 16);
        s->rawptr = rd32(sh + 20);
        s->chars = rd32(sh + 36);
        if (s->chars & IMAGE_SCN_MEM_EXECUTE)
            pe->exec_sections++;
    }
    return 0;
}

static int pe_load(PeImage *pe, const char *path)
{
    uint8_t *data;
    size_t size;

    if (read_file(path, &data, &size) != 0) {
        fprintf(stderr, "%s: cannot read %s: %s\n", TOOL_NAME, path,
                strerror(errno));
        return -1;
    }
    if (pe_parse_buffer(pe, path, data, size, 1) != 0) {
        fprintf(stderr, "%s: not a supported PE32 image: %s\n", TOOL_NAME, path);
        free(pe->secs);
        free(data);
        memset(pe, 0, sizeof *pe);
        return -1;
    }
    if (sha256_file(path, pe->sha256) != 0)
        pe->sha256[0] = 0;
    if (pe->machine != 0x14C)
        warn("%s: machine 0x%04X is not i386", path, pe->machine);
    if (pe->exec_sections == 0)
        warn("%s: no executable section", path);
    return 0;
}

static void pe_free(PeImage *pe)
{
    free(pe->secs);
    if (pe->owns_data) free(pe->data);
    memset(pe, 0, sizeof *pe);
}

/* Match the lifter's lf_read_va(): one section, virtual size clamped by raw
 * size.  Reads never cross a section boundary. */
static size_t pe_read_va(const PeImage *pe, uint32_t va, uint8_t *buf, size_t n)
{
    size_t i;
    for (i = 0; i < pe->nsec; i++) {
        const PeSection *s = &pe->secs[i];
        uint32_t start = pe->base + s->va;
        uint32_t vsize = s->vsize ? s->vsize : s->rawsize;
        uint32_t off, avail, ravail;
        if (va < start || va - start >= vsize)
            continue;
        off = va - start;
        avail = vsize - off;
        if (s->rawsize) {
            ravail = s->rawsize > off ? s->rawsize - off : 0;
            if (ravail < avail) avail = ravail;
        } else {
            return 0;
        }
        if (avail > n) avail = (uint32_t)n;
        if (!avail) return 0;
        if ((uint64_t)s->rawptr + off + avail > pe->size)
            return 0;
        memcpy(buf, pe->data + s->rawptr + off, avail);
        return avail;
    }
    return 0;
}

static int pe_contains_code(const PeImage *pe, uint32_t va)
{
    size_t i;
    if (va < pe->base) return 0;
    for (i = 0; i < pe->nsec; i++) {
        const PeSection *s = &pe->secs[i];
        uint32_t rva = va - pe->base;
        uint32_t span = s->vsize ? s->vsize : s->rawsize;
        if (!(s->chars & IMAGE_SCN_MEM_EXECUTE)) continue;
        if (rva >= s->va && rva - s->va < span) {
            uint32_t off = rva - s->va;
            if (off < s->rawsize) return 1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------- ELF image */

static int elf_parse_buffer(ElfImage *elf, const char *path, uint8_t *data,
                            size_t size, int owns_data)
{
    uint64_t shoff;
    uint16_t shentsize, shnum, shstrndx, i;
    uint32_t symtab_index = 0xFFFFFFFFu, strtab_index = 0xFFFFFFFFu;
    const uint8_t *symtab = NULL;
    uint64_t symtab_size = 0, symtab_entsize = 0;
    const uint8_t *strtab = NULL;
    uint64_t strtab_size = 0;
    uint64_t n, k;

    memset(elf, 0, sizeof *elf);
    snprintf(elf->path, sizeof elf->path, "%s", path);
    elf->data = data;
    elf->size = size;
    elf->owns_data = owns_data;

    if (size < 64) return -1;
    if (memcmp(data, "\177ELF", 4) != 0) return -1;
    if (data[4] != 2 || data[5] != 1) {
        warn("%s: ELF is not 64-bit little-endian", path);
        return -1;
    }
    if (rd16(data + 18) != 183) {
        warn("%s: ELF machine 0x%04X is not AArch64", path, rd16(data + 18));
        return -1;
    }
    shoff = rd64(data + 40);
    shentsize = rd16(data + 58);
    shnum = rd16(data + 60);
    shstrndx = rd16(data + 62);
    (void)shstrndx;
    if (shentsize < 64 || shnum == 0)
        return -1;
    if (shoff > size || (uint64_t)shnum * shentsize > size - shoff)
        return -1;

    /* Prefer .symtab; a stripped product may only have .dynsym. */
    for (i = 0; i < shnum; i++) {
        const uint8_t *sh = data + shoff + (size_t)i * shentsize;
        uint32_t type = rd32(sh + 4);
        if (type == SHT_SYMTAB) {
            symtab_index = i;
            break;
        }
        if (type == SHT_DYNSYM && symtab_index == 0xFFFFFFFFu)
            symtab_index = i;
    }
    if (symtab_index == 0xFFFFFFFFu) {
        warn("%s: no .symtab/.dynsym; cannot read L_* symbols", path);
        return -1;
    }
    {
        const uint8_t *sh = data + shoff + (size_t)symtab_index * shentsize;
        uint32_t type = rd32(sh + 4);
        uint64_t off = rd64(sh + 24);
        uint64_t shsize = rd64(sh + 32);
        uint64_t link = rd32(sh + 40);
        uint64_t entsize = rd64(sh + 56);
        if (type == SHT_SYMTAB || type == SHT_DYNSYM) {
            if (entsize < 24) return -1;
            if (off > size || shsize > size - off) return -1;
            if (link >= shnum) return -1;
            symtab = data + off;
            symtab_size = shsize;
            symtab_entsize = entsize;
            strtab_index = (uint32_t)link;
        }
    }
    if (!symtab) return -1;
    {
        const uint8_t *sh = data + shoff + (size_t)strtab_index * shentsize;
        uint32_t type = rd32(sh + 4);
        uint64_t off = rd64(sh + 24);
        uint64_t shsize = rd64(sh + 32);
        if (type != 3 || off > size || shsize > size - off)
            return -1;
        strtab = data + off;
        strtab_size = shsize;
    }

    n = symtab_size / symtab_entsize;
    elf->funcs = (ArmFunc *)xmalloc((size_t)(n ? n : 1) * sizeof *elf->funcs);
    for (k = 0; k < n; k++) {
        const uint8_t *sym = symtab + (size_t)k * symtab_entsize;
        uint32_t nameoff = rd32(sym);
        uint8_t info = sym[4];
        uint16_t shndx = rd16(sym + 6);
        uint64_t value = rd64(sym + 8);
        uint64_t ssize = rd64(sym + 16);
        const char *name;
        const char *nul;
        size_t namelen;
        uint32_t guest;
        char *endp;

        if ((info & 0x0F) != STT_FUNC) continue;
        if (ssize == 0 || (ssize & 3) != 0) continue;
        if (shndx == SHN_UNDEF || shndx >= shnum) continue;
        if (nameoff >= strtab_size) continue;
        name = (const char *)(strtab + nameoff);
        nul = (const char *)memchr(name, 0, (size_t)(strtab_size - nameoff));
        if (!nul) continue;
        namelen = (size_t)(nul - name);
        if (namelen != 10 || name[0] != 'L' || name[1] != '_')
            continue;
        {
            int ok = 1, j;
            for (j = 2; j < 10; j++)
                if (!isxdigit((unsigned char)name[j])) { ok = 0; break; }
            if (!ok) continue;
        }
        {
            const uint8_t *sh = data + shoff + (size_t)shndx * shentsize;
            uint64_t shflags = rd64(sh + 8);
            uint64_t shaddr = rd64(sh + 16);
            uint64_t shsize = rd64(sh + 32);
            if (!(shflags & SHF_EXECINSTR)) continue;
            if (value < shaddr || value - shaddr > shsize) continue;
            if (ssize > shsize - (value - shaddr)) continue;
        }
        /* The symbol value is the arm64 code address; the name carries the
         * guest VA.  They are different address spaces, so the name is the
         * only matching key. */
        guest = (uint32_t)strtoul(name + 2, &endp, 16);
        if (!endp || *endp != 0) continue;
        {
            ArmFunc *f = &elf->funcs[elf->nfuncs++];
            memset(f, 0, sizeof *f);
            snprintf(f->name, sizeof f->name, "%s", name);
            f->guest = guest;
            f->arm_bytes = (uint32_t)ssize;
            f->arm_insns = (uint32_t)(ssize / 4);
        }
    }
    if (elf->nfuncs == 0) {
        warn("%s: no L_<VA> function symbols", path);
        return -1;
    }
    return 0;
}

static int elf_load(ElfImage *elf, const char *path)
{
    uint8_t *data;
    size_t size;

    if (read_file(path, &data, &size) != 0) {
        fprintf(stderr, "%s: cannot read %s: %s\n", TOOL_NAME, path,
                strerror(errno));
        return -1;
    }
    if (elf_parse_buffer(elf, path, data, size, 1) != 0) {
        free(elf->funcs);
        free(data);
        memset(elf, 0, sizeof *elf);
        return -1;
    }
    if (sha256_file(path, elf->sha256) != 0)
        elf->sha256[0] = 0;
    return 0;
}

static void elf_free(ElfImage *elf)
{
    free(elf->funcs);
    if (elf->owns_data) free(elf->data);
    memset(elf, 0, sizeof *elf);
}

static int cmp_arm(const void *pa, const void *pb)
{
    const ArmFunc *a = (const ArmFunc *)pa;
    const ArmFunc *b = (const ArmFunc *)pb;
    if (a->guest < b->guest) return -1;
    if (a->guest > b->guest) return 1;
    return strcmp(a->name, b->name);
}

static void elf_sort_dedupe(ElfImage *elf)
{
    size_t i, w = 0;
    if (elf->nfuncs < 2) return;
    qsort(elf->funcs, elf->nfuncs, sizeof *elf->funcs, cmp_arm);
    for (i = 1; i < elf->nfuncs; i++) {
        if (elf->funcs[i].guest != elf->funcs[w].guest)
            elf->funcs[++w] = elf->funcs[i];
        else
            warn("%s: duplicate L_%08X symbols (%s, %s)", elf->path,
                 elf->funcs[i].guest, elf->funcs[w].name,
                 elf->funcs[i].name);
    }
    elf->nfuncs = w + 1;
}

/* ------------------------------------------------------------- funcs files */

static int parse_funcs_line(const char *line, uint32_t *addr, uint32_t *size)
{
    const char *p = line;
    char *end;
    unsigned long a, s;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == 0 || *p == '#' || *p == ';') return 0;
    errno = 0;
    a = strtoul(p, &end, 16);
    if (end == p || errno != 0 || a > 0xFFFFFFFFul) return 0;
    p = end;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 0 || *p == '\r' || *p == '\n' || *p == '#' || *p == ';') {
        s = DEFAULT_INIT_EXTENT;
    } else if (isdigit((unsigned char)*p)) {
        errno = 0;
        s = strtoul(p, &end, 10);
        if (end == p || errno != 0 || s > MAX_INIT_EXTENT) return 0;
        /* The rest of the line is the lifter's symbolic name. */
    } else {
        s = DEFAULT_INIT_EXTENT;
    }
    if (s == 0) return 0;
    *addr = (uint32_t)a;
    *size = (uint32_t)s;
    return 1;
}

static void funcs_push(FuncMap *m, uint32_t addr, uint32_t size)
{
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 1024;
        m->v = (FuncSize *)xrealloc(m->v, m->cap * sizeof *m->v);
    }
    m->v[m->n].addr = addr;
    m->v[m->n].size = size;
    m->v[m->n].seq = m->seq++;
    m->n++;
}

static int funcs_add_file(FuncMap *m, const char *path)
{
    FILE *f;
    char line[512];
    size_t added = 0;

    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "%s: cannot read %s: %s\n", TOOL_NAME, path,
                strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        uint32_t addr, size;
        if (parse_funcs_line(line, &addr, &size)) {
            funcs_push(m, addr, size);
            added++;
        }
    }
    fclose(f);
    if (added == 0)
        warn("%s: no function extents parsed", path);
    if (m->nfiles == m->fcap) {
        m->fcap = m->fcap ? m->fcap * 2 : 16;
        m->files = (FuncsFile *)xrealloc(m->files, m->fcap * sizeof *m->files);
    }
    memset(&m->files[m->nfiles], 0, sizeof m->files[m->nfiles]);
    snprintf(m->files[m->nfiles].path, PATH_CAP, "%s", path);
    if (sha256_file(path, m->files[m->nfiles].sha256) != 0)
        m->files[m->nfiles].sha256[0] = 0;
    m->nfiles++;
    return 0;
}

static int cmp_func_size(const void *pa, const void *pb)
{
    const FuncSize *a = (const FuncSize *)pa;
    const FuncSize *b = (const FuncSize *)pb;
    if (a->addr < b->addr) return -1;
    if (a->addr > b->addr) return 1;
    if (a->seq < b->seq) return -1;
    if (a->seq > b->seq) return 1;
    return 0;
}

static void funcs_finalize(FuncMap *m)
{
    size_t i, w;
    if (m->n == 0) return;
    qsort(m->v, m->n, sizeof *m->v, cmp_func_size);
    w = 0;
    for (i = 1; i < m->n; i++) {
        if (m->v[i].addr == m->v[w].addr) {
            if (m->v[i].size != m->v[w].size)
                warn("duplicate extent for 0x%08X: keeping %u (first), "
                     "ignoring %u", m->v[i].addr, m->v[w].size, m->v[i].size);
            continue;
        }
        m->v[++w] = m->v[i];
    }
    m->n = w + 1;
}

static const FuncSize *funcs_find(const FuncMap *m, uint32_t addr)
{
    size_t lo = 0, hi = m->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (m->v[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < m->n && m->v[lo].addr == addr) return &m->v[lo];
    return NULL;
}

typedef struct {
    char **v;
    size_t n;
    size_t cap;
} StrVec;

static void strvec_push(StrVec *v, const char *s)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->v = (char **)xrealloc(v->v, v->cap * sizeof *v->v);
    }
    v->v[v->n++] = xstrdup(s);
}

static void strvec_free(StrVec *v)
{
    size_t i;
    for (i = 0; i < v->n; i++) free(v->v[i]);
    free(v->v);
    memset(v, 0, sizeof *v);
}

static int cmp_str(const void *pa, const void *pb)
{
    const char *const *a = (const char *const *)pa;
    const char *const *b = (const char *const *)pb;
    return strcmp(*a, *b);
}

static int is_funcs_name(const char *base)
{
    if (strcmp(base, "functions.txt") == 0) return 1;
    if (strncmp(base, "closed.r", 8) == 0 && path_ends_with(base, ".txt"))
        return 1;
    return 0;
}

static int collect_funcs_file(const char *path, void *user)
{
    StrVec *v = (StrVec *)user;
    const char *base = path_basename(path);
    if (is_funcs_name(base)) strvec_push(v, path);
    return 0;
}

/* --------------------------------------------------------------- capstone */

static csh g_cs;
static cs_insn *g_insn;
static XInsn *g_insns;
static size_t g_ninsns, g_capinsns;

static int cs_init(void)
{
    if (csg_open_x86(&g_cs) != 0) {
        fprintf(stderr, "%s: cannot open capstone x86 decoder\n", TOOL_NAME);
        return -1;
    }
    g_insn = csg.malloc_insn(g_cs);
    if (!g_insn) {
        fprintf(stderr, "%s: capstone malloc failed\n", TOOL_NAME);
        return -1;
    }
    return 0;
}

static void cs_shutdown(void)
{
    if (g_insn) csg.free_insn(g_insn, 1);
    g_insn = NULL;
    if (g_cs) csg.close(&g_cs);
    g_cs = 0;
    free(g_insns);
    g_insns = NULL;
    g_ninsns = g_capinsns = 0;
}

static void insn_append(const cs_insn *ci)
{
    XInsn *x;
    if (g_ninsns == g_capinsns) {
        g_capinsns = g_capinsns ? g_capinsns * 2 : 512;
        g_insns = (XInsn *)xrealloc(g_insns, g_capinsns * sizeof *g_insns);
    }
    x = &g_insns[g_ninsns++];
    memset(x, 0, sizeof *x);
    x->addr = (uint32_t)ci->address;
    x->size = ci->size;
    {
        const char *m = ci->mnemonic;
        const char *w = strrchr(m, ' ');
        if (w) m = w + 1;
        x->term = !strcmp(m, "ret") || !strcmp(m, "retn") ||
                  !strcmp(m, "retf") || !strcmp(m, "jmp") ||
                  !strcmp(m, "iret") || !strcmp(m, "iretd") ||
                  !strcmp(m, "hlt");
        x->ret = strstr(ci->mnemonic, "ret") != NULL;
        x->jmp_indirect = strstr(ci->mnemonic, "jmp") != NULL &&
                          strchr(ci->op_str, '[') != NULL;
    }
}

static void decode_chunk(const uint8_t *code, size_t len, uint32_t addr,
                         uint32_t stop_addr)
{
    const uint8_t *p = code;
    size_t left = len;
    uint64_t a = addr;
    while (left && csg.iter(g_cs, &p, &left, &a, g_insn)) {
        if (stop_addr && g_insn->address >= stop_addr) break;
        insn_append(g_insn);
    }
}

/* Count the instructions the final lift emitted for a guest function.
 *
 * init_extent is the byte size the lifter was given for this entry.  The
 * block-mode path cuts at the first terminator; the whole-function heuristic
 * then grows past a direct jmp whose next byte is not padding, exactly as
 * liftcore.c does.  The final pass uses block_mode=1.
 */
static uint32_t x86_count(const PeImage *pe, uint32_t start, uint32_t init_extent,
                          uint32_t *end_out)
{
    uint8_t buf[GROW_CHUNK + X86_MAX_INSN + 32];
    uint8_t prev;
    uint8_t pad;
    uint8_t *initbuf;
    size_t init_cap, init_got, got;
    uint32_t limit;
    size_t n, i, before;
    int whole;
    uint32_t grown, cap_grow;

    g_ninsns = 0;
    if (init_extent == 0) init_extent = DEFAULT_INIT_EXTENT;
    if (init_extent > MAX_INIT_EXTENT) init_extent = MAX_INIT_EXTENT;

    whole = (start == pe->base);
    if (!whole && start > 0 && pe_read_va(pe, start - 1, &prev, 1) == 1)
        whole = (prev == 0xCC || prev == 0x90 || prev == 0x00);

    /* Initial extent: decode every instruction that starts before start+N.
     * The extra 15 bytes let an instruction that crosses the boundary finish,
     * which is what liftcore does when it extends the extent. */
    init_cap = (size_t)init_extent + X86_MAX_INSN;
    initbuf = (uint8_t *)xmalloc(init_cap);
    init_got = pe_read_va(pe, start, initbuf, init_cap);
    if (init_got == 0) {
        free(initbuf);
        if (end_out) *end_out = start;
        return 0;
    }
    got = init_got < init_extent ? init_got : init_extent;
    limit = start + (uint32_t)got;
    decode_chunk(initbuf, init_got, start, limit);
    free(initbuf);

    /* block-mode truncation at the first terminator */
    n = g_ninsns;
    for (i = 0; i < n; i++) {
        if (g_insns[i].term) { n = i + 1; break; }
    }
    g_ninsns = n;

    if (n > 0 && (whole || !g_insns[n - 1].term)) {
        cap_grow = whole ? 0x800u : 0x100u;
        grown = 0;
        while (grown < cap_grow) {
            XInsn *last = &g_insns[g_ninsns - 1];
            uint32_t nxt, budget;
            size_t got_more;

            if (last->term) {
                size_t pr = pe_read_va(pe, last->addr + last->size, &pad, 1);
                if (whole && last->ret) break;
                if (pr != 1 || pad == 0xCC || pad == 0x90) break;
                if (last->jmp_indirect) break;
            }
            nxt = last->addr + last->size;
            budget = cap_grow - grown;
            if (budget > GROW_CHUNK - X86_MAX_INSN)
                budget = GROW_CHUNK - X86_MAX_INSN;
            got_more = pe_read_va(pe, nxt, buf, (size_t)budget + X86_MAX_INSN);
            if (got_more == 0) break;
            before = g_ninsns;
            decode_chunk(buf, got_more, nxt, 0);
            if (g_ninsns == before) break;

            /* Whole-function ret cut, then block-mode terminator cut.  Both
             * act on the newly decoded chunk, as in liftcore.c. */
            if (whole) {
                size_t k;
                for (k = before; k < g_ninsns; k++) {
                    if (g_insns[k].ret) { g_ninsns = k + 1; break; }
                }
            }
            {
                size_t k;
                for (k = before; k < g_ninsns; k++) {
                    if (g_insns[k].term) { g_ninsns = k + 1; break; }
                }
            }
            {
                XInsn *ne = &g_insns[g_ninsns - 1];
                uint32_t end = ne->addr + ne->size;
                grown += end > nxt ? end - nxt : 0;
                if (ne->term) break;
            }
        }
    }

    n = g_ninsns;
    if (end_out)
        *end_out = n ? g_insns[n - 1].addr + g_insns[n - 1].size : start;
    return (uint32_t)n;
}

/* ---------------------------------------------------------------- results */

static int cmp_result(const void *pa, const void *pb)
{
    const Result *a = (const Result *)pa;
    const Result *b = (const Result *)pb;
    double ra = (double)a->arm_insns * 100.0 / (double)a->x86_insns;
    double rb = (double)b->arm_insns * 100.0 / (double)b->x86_insns;
    if (ra < rb) return 1;
    if (ra > rb) return -1;
    if (a->arm_insns != b->arm_insns)
        return a->arm_insns > b->arm_insns ? -1 : 1;
    if (a->guest != b->guest) return a->guest < b->guest ? -1 : 1;
    return strcmp(a->name, b->name);
}

static double result_ratio(const Result *r)
{
    return (double)r->arm_insns * 100.0 / (double)r->x86_insns;
}

static void compute_stats(const Result *res, size_t n, Stats *st)
{
    size_t i;
    double sum = 0.0;

    st->included = n;
    st->sum_x86 = 0;
    st->sum_arm = 0;
    for (i = 0; i < n; i++) {
        st->sum_x86 += res[i].x86_insns;
        st->sum_arm += res[i].arm_insns;
        sum += result_ratio(&res[i]);
    }
    st->weighted_ratio = st->sum_x86
        ? (double)st->sum_arm * 100.0 / (double)st->sum_x86 : 0.0;
    st->mean_ratio = n ? sum / (double)n : 0.0;
    st->max_ratio = n ? result_ratio(&res[0]) : 0.0;
    if (n == 0) {
        st->median_ratio = 0.0;
        st->p90_ratio = 0.0;
        return;
    }
    if (n & 1) {
        st->median_ratio = result_ratio(&res[n / 2]);
    } else {
        st->median_ratio = (result_ratio(&res[n / 2 - 1]) +
                            result_ratio(&res[n / 2])) / 2.0;
    }
    {
        size_t k = (size_t)((n * 9 + 9) / 10); /* ceil(0.9*n) */
        if (k == 0) k = 1;
        if (k > n) k = n;
        st->p90_ratio = result_ratio(&res[k - 1]);
    }
}

/* --------------------------------------------------------------- printing */

static void json_string(FILE *f, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    fputc('"', f);
    for (; *p; p++) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\b': fputs("\\b", f); break;
        case '\f': fputs("\\f", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (*p < 0x20) fprintf(f, "\\u%04x", (unsigned)*p);
            else fputc(*p, f);
            break;
        }
    }
    fputc('"', f);
}

static void csv_field(FILE *f, const char *s)
{
    int need = 0;
    const char *p;
    for (p = s; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            need = 1;
            break;
        }
    }
    if (!need) {
        fputs(s, f);
        return;
    }
    fputc('"', f);
    for (p = s; *p; p++) {
        if (*p == '"') fputc('"', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

static void print_text(const Options *o, const ElfImage *elf,
                       const PeImage *pes, const FuncMap *fm,
                       const Result *res, size_t n, const Stats *st)
{
    size_t i, top;
    uint64_t arm_total = 0;
    for (i = 0; i < elf->nfuncs; i++) arm_total += elf->funcs[i].arm_insns;
    printf("%s: arm64/x86 instruction ratio\n", TOOL_NAME);
    printf("arm64: %s", elf->path);
    if (elf->sha256[0]) printf("  sha256=%s", elf->sha256);
    printf("\n");
    printf("  L_* functions: %llu, arm64 instructions: %llu\n",
           (unsigned long long)elf->nfuncs,
           (unsigned long long)arm_total);
    for (i = 0; i < o->nx86; i++) {
        printf("x86[%llu]: %s base=0x%08X image=0x%X",
               (unsigned long long)i, pes[i].path, pes[i].base,
               pes[i].image_size);
        if (pes[i].sha256[0]) printf(" sha256=%s", pes[i].sha256);
        printf("\n");
    }
    if (fm->n)
        printf("extents: %llu entries from %llu file(s) (per-function sizes "
               "from the funcs files)\n",
               (unsigned long long)fm->n, (unsigned long long)fm->nfiles);
    else
        printf("extents: heuristic initial size %u (pass --funcs/--funcs-dir "
               "for exact lifter sizes)\n", o->init_extent);

    printf("\n");
    printf("matched: %llu/%llu  included: %llu  filtered: %llu  "
           "no-image: %llu  not-in-funcs: %llu  decode-failed: %llu  "
           "ambiguous: %llu\n",
           (unsigned long long)st->matched,
           (unsigned long long)st->arm_functions,
           (unsigned long long)st->included,
           (unsigned long long)st->skipped_filter,
           (unsigned long long)st->no_image,
           (unsigned long long)st->not_in_funcs,
           (unsigned long long)st->decode_failed,
           (unsigned long long)st->ambiguous);
    printf("totals: x86=%llu arm64=%llu weighted=%.2f%%  "
           "mean=%.2f%% median=%.2f%% p90=%.2f%% max=%.2f%%\n",
           (unsigned long long)st->sum_x86,
           (unsigned long long)st->sum_arm,
           st->weighted_ratio, st->mean_ratio, st->median_ratio,
           st->p90_ratio, st->max_ratio);
    if (st->decode_failed)
        printf("note: %llu function(s) had no decodable x86 instruction "
               "(for example an extent that ends at a section boundary) and "
               "are excluded\n",
               (unsigned long long)st->decode_failed);
    printf("\n");

    top = o->top ? o->top : n;
    if (top > n) top = n;
    printf("%5s  %-10s %8s %8s %10s %10s  %s\n",
           "rank", "function", "x86", "arm64", "ratio", "delta", "module");
    for (i = 0; i < top; i++) {
        const Result *r = &res[i];
        printf("%5llu  %-10s %8u %8u %9.2f%% %+10lld  %s\n",
               (unsigned long long)(i + 1), r->name, r->x86_insns,
               r->arm_insns, result_ratio(r),
               (long long)r->arm_insns - (long long)r->x86_insns,
               path_basename(pes[r->pe_index].path));
    }
    if (top < n)
        printf("... %llu more included function(s); use --top 0 for all\n",
               (unsigned long long)(n - top));
}

static int write_csv(const char *path, const Result *res, size_t n,
                     const PeImage *pes)
{
    FILE *f = fopen(path, "w");
    size_t i;
    if (!f) {
        fprintf(stderr, "%s: cannot write %s: %s\n", TOOL_NAME, path,
                strerror(errno));
        return -1;
    }
    fputs("rank,function,guest_address,x86_instructions,x86_bytes,"
          "arm64_instructions,arm64_bytes,ratio_percent,delta,module\n", f);
    for (i = 0; i < n; i++) {
        const Result *r = &res[i];
        fprintf(f, "%llu,%s,0x%08X,%u,%u,%u,%llu,%.4f,%lld,",
                (unsigned long long)(i + 1), r->name, r->guest,
                r->x86_insns, r->x86_bytes, r->arm_insns,
                (unsigned long long)r->arm_insns * 4, result_ratio(r),
                (long long)r->arm_insns - (long long)r->x86_insns);
        csv_field(f, path_basename(pes[r->pe_index].path));
        fputc('\n', f);
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "%s: cannot close %s: %s\n", TOOL_NAME, path,
                strerror(errno));
        return -1;
    }
    return 0;
}

static int write_json(const char *path, const Options *o, const ElfImage *elf,
                      const PeImage *pes, const FuncMap *fm,
                      const Result *res, size_t n, const Stats *st)
{
    FILE *f = fopen(path, "w");
    size_t i;
    uint64_t arm_total = 0;
    if (!f) {
        fprintf(stderr, "%s: cannot write %s: %s\n", TOOL_NAME, path,
                strerror(errno));
        return -1;
    }
    for (i = 0; i < elf->nfuncs; i++) arm_total += elf->funcs[i].arm_insns;
    fprintf(f, "{\n  \"schema\": %d,\n  \"tool\": \"%s\",\n",
            TOOL_SCHEMA, TOOL_NAME);
    fputs("  \"arm64\": {", f);
    fputs("\"path\": ", f); json_string(f, elf->path);
    fputs(", \"sha256\": ", f); json_string(f, elf->sha256);
    fprintf(f, ", \"bytes\": %llu, \"functions\": %llu, "
               "\"instructions\": %llu},\n",
            (unsigned long long)elf->size,
            (unsigned long long)elf->nfuncs,
            (unsigned long long)arm_total);
    fputs("  \"x86\": [", f);
    for (i = 0; i < o->nx86; i++) {
        if (i) fputs(", ", f);
        fputs("{\"path\": ", f); json_string(f, pes[i].path);
        fputs(", \"sha256\": ", f); json_string(f, pes[i].sha256);
        fprintf(f, ", \"bytes\": %llu, \"base\": \"0x%08X\", "
                   "\"image_size\": %u, \"machine\": \"0x%04X\"}",
                (unsigned long long)pes[i].size, pes[i].base,
                pes[i].image_size, pes[i].machine);
    }
    fputs("],\n  \"funcs\": [", f);
    for (i = 0; i < fm->nfiles; i++) {
        if (i) fputs(", ", f);
        fputs("{\"path\": ", f); json_string(f, fm->files[i].path);
        fputs(", \"sha256\": ", f); json_string(f, fm->files[i].sha256);
        fputc('}', f);
    }
    fprintf(f, "],\n  \"thresholds\": {\"min_x86\": %u, \"min_arm64\": %u, "
               "\"init_extent\": %u},\n",
            o->min_x86, o->min_arm64, o->init_extent);
    fprintf(f, "  \"counts\": {\"arm_functions\": %llu, \"matched\": %llu, "
               "\"included\": %llu, \"skipped_filter\": %llu, "
               "\"unmatched_no_image\": %llu, \"unmatched_not_in_funcs\": %llu, "
               "\"decode_failed\": %llu, \"ambiguous\": %llu},\n",
            (unsigned long long)st->arm_functions,
            (unsigned long long)st->matched,
            (unsigned long long)st->included,
            (unsigned long long)st->skipped_filter,
            (unsigned long long)st->no_image,
            (unsigned long long)st->not_in_funcs,
            (unsigned long long)st->decode_failed,
            (unsigned long long)st->ambiguous);
    fprintf(f, "  \"totals\": {\"x86_instructions\": %llu, "
               "\"arm64_instructions\": %llu, "
               "\"weighted_ratio_percent\": %.6f},\n",
            (unsigned long long)st->sum_x86,
            (unsigned long long)st->sum_arm, st->weighted_ratio);
    fprintf(f, "  \"stats\": {\"mean_ratio_percent\": %.6f, "
               "\"median_ratio_percent\": %.6f, "
               "\"p90_ratio_percent\": %.6f, "
               "\"max_ratio_percent\": %.6f},\n",
            st->mean_ratio, st->median_ratio, st->p90_ratio, st->max_ratio);
    fputs("  \"entries\": [\n", f);
    for (i = 0; i < n; i++) {
        const Result *r = &res[i];
        fprintf(f, "    {\"rank\": %llu, \"function\": ",
                (unsigned long long)(i + 1));
        json_string(f, r->name);
        fprintf(f, ", \"guest_address\": \"0x%08X\", "
                   "\"x86_instructions\": %u, \"x86_bytes\": %u, "
                   "\"arm64_instructions\": %u, \"arm64_bytes\": %llu, "
                   "\"ratio_percent\": %.6f, \"delta\": %lld, \"module\": ",
                r->guest, r->x86_insns, r->x86_bytes, r->arm_insns,
                (unsigned long long)r->arm_insns * 4, result_ratio(r),
                (long long)r->arm_insns - (long long)r->x86_insns);
        json_string(f, path_basename(pes[r->pe_index].path));
        fputs(i + 1 == n ? "}\n" : "},\n", f);
    }
    fputs("  ]\n}\n", f);
    if (fclose(f) != 0) {
        fprintf(stderr, "%s: cannot close %s: %s\n", TOOL_NAME, path,
                strerror(errno));
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- self-test */

static void test_put_section(uint8_t *p, const char *name, uint32_t type,
                             uint64_t flags, uint64_t addr, uint64_t off,
                             uint64_t size, uint32_t link, uint64_t entsize)
{
    (void)name; /* the parser does not consume section names */
    put32(p + 0, 0); /* sh_name */
    put32(p + 4, type);
    put64(p + 8, flags);
    put64(p + 16, addr);
    put64(p + 24, off);
    put64(p + 32, size);
    put32(p + 40, link);
    put32(p + 44, 0);
    put64(p + 48, 8);
    put64(p + 56, entsize);
}

static int self_test(void)
{
    int failures = 0;
    uint8_t pebuf[0x600];
    uint8_t elfbuf[0x600];
    PeImage pe;
    ElfImage elf;
    uint32_t end;

    if (cs_init() != 0) return 1;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "self-test: FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                #cond); \
        failures++; \
    } \
} while (0)

    /* --- synthetic PE32 ------------------------------------------------- */
    memset(pebuf, 0, sizeof pebuf);
    pebuf[0] = 'M'; pebuf[1] = 'Z';
    put32(pebuf + 0x3C, 0x40);
    memcpy(pebuf + 0x40, "PE\0\0", 4);
    put16(pebuf + 0x44, 0x14C);      /* machine */
    put16(pebuf + 0x46, 1);          /* NumberOfSections */
    put16(pebuf + 0x54, 0xE0);       /* SizeOfOptionalHeader */
    put16(pebuf + 0x58, 0x10B);      /* PE32 */
    put32(pebuf + 0x58 + 16, 0x1000);/* AddressOfEntryPoint */
    put32(pebuf + 0x58 + 28, 0x400000); /* ImageBase */
    put32(pebuf + 0x58 + 32, 0x1000);   /* SectionAlignment */
    put32(pebuf + 0x58 + 36, 0x200);    /* FileAlignment */
    put32(pebuf + 0x58 + 56, 0x2000);   /* SizeOfImage */
    put32(pebuf + 0x58 + 60, 0x200);    /* SizeOfHeaders */
    put32(pebuf + 0x58 + 92, 16);       /* NumberOfRvaAndSizes */
    {
        uint8_t *sh = pebuf + 0x58 + 0xE0;
        memcpy(sh, ".text\0\0\0", 8);
        put32(sh + 8, 0x200);        /* VirtualSize */
        put32(sh + 12, 0x1000);      /* VirtualAddress */
        put32(sh + 16, 0x200);       /* SizeOfRawData */
        put32(sh + 20, 0x200);       /* PointerToRawData */
        put32(sh + 36, 0x60000020);  /* code|exec|read */
    }
    {
        uint8_t *code = pebuf + 0x200;
        /* 0x401000: mov eax,1; add eax,2; ret */
        static const uint8_t f1[] = {0xB8,1,0,0,0, 0x83,0xC0,2, 0xC3};
        /* 0x401010: push ebp; mov ebp,esp; pop ebp; ret */
        static const uint8_t f2[] = {0x55, 0x8B,0xEC, 0x5D, 0xC3};
        /* 0x401020: jmp +2; padding */
        static const uint8_t f3[] = {0xEB,2, 0xCC,0xCC};
        /* 0x401030: jmp +2; mov eax,1; ret (direct jmp, non-padding) */
        static const uint8_t f4[] = {0xEB,2, 0xB8,1,0,0,0, 0xC3};
        memset(code, 0xCC, 0x200);
        memcpy(code + 0x00, f1, sizeof f1);
        memcpy(code + 0x10, f2, sizeof f2);
        memcpy(code + 0x20, f3, sizeof f3);
        memcpy(code + 0x30, f4, sizeof f4);
    }
    CHECK(pe_parse_buffer(&pe, "<self-test-pe>", pebuf, sizeof pebuf, 0) == 0);
    CHECK(pe.base == 0x400000);
    CHECK(pe.nsec == 1);
    CHECK(pe.exec_sections == 1);
    CHECK(pe_contains_code(&pe, 0x401000));
    CHECK(!pe_contains_code(&pe, 0x500000));
    CHECK(x86_count(&pe, 0x401000, 16, &end) == 3);
    CHECK(end == 0x401009);
    CHECK(x86_count(&pe, 0x401000, 5, &end) == 3);
    CHECK(x86_count(&pe, 0x401010, 16, &end) == 4);
    CHECK(x86_count(&pe, 0x401020, 16, &end) == 1);
    CHECK(x86_count(&pe, 0x401030, 16, &end) == 3);
    pe_free(&pe);

    /* --- synthetic ELF64 ------------------------------------------------ */
    memset(elfbuf, 0, sizeof elfbuf);
    elfbuf[0] = 0x7F; elfbuf[1] = 'E'; elfbuf[2] = 'L'; elfbuf[3] = 'F';
    elfbuf[4] = 2; elfbuf[5] = 1; elfbuf[6] = 1;
    put16(elfbuf + 16, 3);           /* ET_DYN */
    put16(elfbuf + 18, 183);         /* AArch64 */
    put32(elfbuf + 20, 1);
    put64(elfbuf + 40, 0x100);       /* e_shoff */
    put16(elfbuf + 52, 64);          /* e_ehsize */
    put16(elfbuf + 58, 64);          /* e_shentsize */
    put16(elfbuf + 60, 4);           /* e_shnum */
    put16(elfbuf + 62, 3);           /* e_shstrndx */
    test_put_section(elfbuf + 0x100 + 64, ".text", 1, 0x6, 0x1000, 0x400,
                     0x40, 0, 0);
    test_put_section(elfbuf + 0x100 + 128, ".symtab", 2, 0, 0, 0x440,
                     72, 3, 24);
    test_put_section(elfbuf + 0x100 + 192, ".strtab", 3, 0, 0, 0x490,
                     0x60, 0, 0);
    {
        /* string table: offsets 0, 1, 7, 15, 23, 34 */
        uint8_t *st = elfbuf + 0x490;
        static const char s[] =
            "\0.text\0.symtab\0.strtab\0L_00001000\0L_00001010";
        memcpy(st, s, sizeof s);
        {
            uint8_t *sym = elfbuf + 0x440 + 24;
            put32(sym + 0, 23);
            sym[4] = 0x12;           /* GLOBAL | FUNC */
            put16(sym + 6, 1);
            put64(sym + 8, 0x1000);
            put64(sym + 16, 12);
            sym = elfbuf + 0x440 + 48;
            put32(sym + 0, 34);
            sym[4] = 0x12;
            put16(sym + 6, 1);
            put64(sym + 8, 0x1010);
            put64(sym + 16, 8);
        }
    }
    CHECK(elf_parse_buffer(&elf, "<self-test-elf>", elfbuf, sizeof elfbuf, 0)
          == 0);
    CHECK(elf.nfuncs == 2);
    elf_sort_dedupe(&elf);
    CHECK(elf.nfuncs == 2);
    CHECK(elf.funcs[0].guest == 0x1000);
    CHECK(strcmp(elf.funcs[0].name, "L_00001000") == 0);
    CHECK(elf.funcs[0].arm_insns == 3);
    CHECK(elf.funcs[1].guest == 0x1010);
    CHECK(elf.funcs[1].arm_insns == 2);
    elf_free(&elf);

    /* --- funcs parser --------------------------------------------------- */
    {
        uint32_t a, s;
        CHECK(parse_funcs_line("0x6f303db5 5 block_x\n", &a, &s) == 1);
        CHECK(a == 0x6F303DB5 && s == 5);
        CHECK(parse_funcs_line("  0x401000\n", &a, &s) == 1);
        CHECK(a == 0x401000 && s == DEFAULT_INIT_EXTENT);
        CHECK(parse_funcs_line("# comment\n", &a, &s) == 0);
        CHECK(parse_funcs_line("\n", &a, &s) == 0);
        CHECK(parse_funcs_line("0x401000 0\n", &a, &s) == 0);
    }

    cs_shutdown();
    if (failures == 0) {
        puts("recomp_ratio self-test: OK");
        return 0;
    }
    fprintf(stderr, "recomp_ratio self-test: %d failure(s)\n", failures);
    return 1;
#undef CHECK
}

/* -------------------------------------------------------------------- main */

static void usage(FILE *f)
{
    fprintf(f,
"Usage:\n"
"  %s --arm64 <libwc3gameactivity.so> --x86 <pe> [--x86 <pe> ...] [options]\n"
"\n"
"Required:\n"
"  --arm64 PATH        unstripped arm64 ELF with L_<VA> function symbols\n"
"  --x86 PATH          guest x86-32 PE image; repeat for every module\n"
"\n"
"Extents (optional):\n"
"  --funcs FILE        lifter funcs list (0xADDR SIZE [name]); repeatable\n"
"  --funcs-dir DIR     recursively load functions.txt and closed.r*.txt\n"
"                      from DIR; repeatable.  Point it at the corpus output\n"
"                      and work directories for exact lifter extents.\n"
"  --init-size N       initial extent when --funcs is absent (default %u)\n"
"\n"
"Output:\n"
"  --top N             rows on stdout; 0 = all (default 50)\n"
"  --min-x86 N         skip functions with fewer guest instructions (default 1)\n"
"  --min-arm64 N       skip functions with fewer arm64 instructions (default 1)\n"
"  --json PATH         write the full included set as JSON\n"
"  --csv PATH          write the full included set as CSV\n"
"  --verbose           per-input details on stderr\n"
"  --self-test         run built-in PE/ELF/decoder checks\n"
"  --help              this text\n"
"\n"
"Examples:\n"
"  %s --arm64 build/android_app/debug/cmake/libwc3gameactivity.so \\\n"
"       --x86 build/corpus-inputs/war3.exe \\\n"
"       --x86 build/corpus-inputs/game.dll --top 100\n"
"\n"
"  %s --arm64 build/android_app/debug/cmake/libwc3gameactivity.so \\\n"
"       --x86 build/corpus-inputs/*.exe --x86 build/corpus-inputs/*.dll \\\n"
"       --funcs-dir build/corpus-full --funcs-dir build/corpus-full-work \\\n"
"       --json reports/generated/recomp_ratio.json\n"
"\n"
"Ratio is arm64_instructions * 100 / x86_instructions.  Higher means the\n"
"recompiler emitted more instructions for the same guest function.\n"
"\n"
"Exit codes: 0 success, 1 runtime/input error, 2 usage error.\n",
            TOOL_NAME, DEFAULT_INIT_EXTENT, TOOL_NAME, TOOL_NAME);
}

static unsigned parse_uint_arg(const char *opt, const char *v)
{
    char *end;
    long n;
    errno = 0;
    n = strtol(v, &end, 10);
    if (end == v || *end != 0 || errno != 0 || n < 0 || n > 100000000)
        die("%s expects a non-negative integer, got '%s'", opt, v);
    return (unsigned)n;
}

static int parse_args(int argc, char **argv, Options *o)
{
    int i;
    memset(o, 0, sizeof *o);
    o->init_extent = DEFAULT_INIT_EXTENT;
    o->top = 50;
    o->min_x86 = 1;
    o->min_arm64 = 1;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = NULL;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            exit(0);
        } else if (!strcmp(a, "--self-test")) {
            return 2;
        } else if (!strcmp(a, "--verbose") || !strcmp(a, "-v")) {
            o->verbose = 1;
            continue;
        }
        if (i + 1 >= argc) die("%s requires a value", a);
        v = argv[++i];
        if (!strcmp(a, "--arm64")) {
            o->arm_path = v;
        } else if (!strcmp(a, "--x86")) {
            if (o->nx86 == MAX_X86_IMAGES) die("too many --x86 images");
            o->x86_paths[o->nx86++] = v;
        } else if (!strcmp(a, "--funcs")) {
            if (o->nfuncs == MAX_FUNCS_ARGS) die("too many --funcs files");
            o->funcs_paths[o->nfuncs++] = v;
        } else if (!strcmp(a, "--funcs-dir")) {
            if (o->nfuncs_dirs == MAX_FUNCS_DIRS) die("too many --funcs-dir");
            o->funcs_dirs[o->nfuncs_dirs++] = v;
        } else if (!strcmp(a, "--top")) {
            o->top = parse_uint_arg(a, v);
        } else if (!strcmp(a, "--min-x86")) {
            o->min_x86 = parse_uint_arg(a, v);
        } else if (!strcmp(a, "--min-arm64")) {
            o->min_arm64 = parse_uint_arg(a, v);
        } else if (!strcmp(a, "--init-size")) {
            unsigned n = parse_uint_arg(a, v);
            if (n == 0 || n > MAX_INIT_EXTENT)
                die("--init-size must be in 1..%u", MAX_INIT_EXTENT);
            o->init_extent = n;
        } else if (!strcmp(a, "--json")) {
            o->json_path = v;
        } else if (!strcmp(a, "--csv")) {
            o->csv_path = v;
        } else {
            die("unknown option: %s (try --help)", a);
        }
    }
    if (!o->arm_path) die("--arm64 is required");
    if (o->nx86 == 0) die("at least one --x86 image is required");
    return 0;
}

int main(int argc, char **argv)
{
    Options o;
    ElfImage elf;
    PeImage pes[MAX_X86_IMAGES];
    FuncMap fm;
    Result *res = NULL;
    Stats st;
    size_t i, nres = 0;
    int rc = 0;

    if (parse_args(argc, argv, &o) == 2)
        return self_test();

    memset(&elf, 0, sizeof elf);
    memset(pes, 0, sizeof pes);
    memset(&fm, 0, sizeof fm);
    memset(&st, 0, sizeof st);

    if (elf_load(&elf, o.arm_path) != 0) return 1;
    elf_sort_dedupe(&elf);
    for (i = 0; i < o.nx86; i++) {
        if (pe_load(&pes[i], o.x86_paths[i]) != 0) {
            rc = 1;
            goto done;
        }
    }
    if (cs_init() != 0) { rc = 1; goto done; }

    for (i = 0; i < o.nfuncs; i++) {
        if (funcs_add_file(&fm, o.funcs_paths[i]) != 0) {
            rc = 1;
            goto done;
        }
    }
    for (i = 0; i < o.nfuncs_dirs; i++) {
        StrVec paths;
        memset(&paths, 0, sizeof paths);
        if (!path_is_dir(o.funcs_dirs[i])) {
            fprintf(stderr, "%s: --funcs-dir is not a directory: %s\n",
                    TOOL_NAME, o.funcs_dirs[i]);
            strvec_free(&paths);
            rc = 1;
            goto done;
        }
        if (dir_walk(o.funcs_dirs[i], collect_funcs_file, &paths) != 0) {
            fprintf(stderr, "%s: cannot walk %s\n", TOOL_NAME,
                    o.funcs_dirs[i]);
            strvec_free(&paths);
            rc = 1;
            goto done;
        }
        if (paths.n == 0)
            warn("%s: no functions.txt or closed.r*.txt found",
                 o.funcs_dirs[i]);
        qsort(paths.v, paths.n, sizeof *paths.v, cmp_str);
        for (size_t k = 0; k < paths.n; k++) {
            if (funcs_add_file(&fm, paths.v[k]) != 0) {
                strvec_free(&paths);
                rc = 1;
                goto done;
            }
        }
        strvec_free(&paths);
    }
    funcs_finalize(&fm);

    res = (Result *)xmalloc(elf.nfuncs * sizeof *res);
    st.arm_functions = elf.nfuncs;
    for (i = 0; i < elf.nfuncs; i++) {
        const ArmFunc *af = &elf.funcs[i];
        const FuncSize *fs = NULL;
        uint16_t pi;
        int found = -1;
        uint32_t init_extent, end;
        uint32_t x86n;

        for (pi = 0; pi < (uint16_t)o.nx86; pi++) {
            if (pe_contains_code(&pes[pi], af->guest)) {
                if (found >= 0) {
                    if (st.ambiguous < 10)
                        warn("0x%08X is in both %s and %s; using the first",
                             af->guest, pes[found].path, pes[pi].path);
                    st.ambiguous++;
                } else {
                    found = pi;
                }
            }
        }
        if (found < 0) {
            st.no_image++;
            continue;
        }
        if (fm.n) {
            fs = funcs_find(&fm, af->guest);
            if (!fs) {
                st.not_in_funcs++;
                continue;
            }
            init_extent = fs->size;
        } else {
            init_extent = o.init_extent;
        }
        x86n = x86_count(&pes[found], af->guest, init_extent, &end);
        if (x86n == 0) {
            st.decode_failed++;
            continue;
        }
        st.matched++;
        if (x86n < o.min_x86 || af->arm_insns < o.min_arm64) {
            st.skipped_filter++;
            continue;
        }
        memset(&res[nres], 0, sizeof res[nres]);
        snprintf(res[nres].name, sizeof res[nres].name, "%s", af->name);
        res[nres].guest = af->guest;
        res[nres].x86_insns = x86n;
        res[nres].arm_insns = af->arm_insns;
        res[nres].x86_bytes = end > af->guest ? end - af->guest : 0;
        res[nres].pe_index = (uint16_t)found;
        nres++;
    }
    if (nres == 0) {
        fprintf(stderr, "%s: no functions matched; check --x86 images and "
                "extent filters\n", TOOL_NAME);
        rc = 1;
        goto done;
    }
    qsort(res, nres, sizeof *res, cmp_result);
    compute_stats(res, nres, &st);

    print_text(&o, &elf, pes, &fm, res, nres, &st);
    if (o.csv_path && write_csv(o.csv_path, res, nres, pes) != 0) rc = 1;
    if (o.json_path && write_json(o.json_path, &o, &elf, pes, &fm, res, nres,
                                  &st) != 0) rc = 1;

    if (o.verbose) {
        fprintf(stderr, "[*] arm64 symbols: %llu\n",
                (unsigned long long)elf.nfuncs);
        fprintf(stderr, "[*] matched: %llu included: %llu filtered: %llu\n",
                (unsigned long long)st.matched,
                (unsigned long long)st.included,
                (unsigned long long)st.skipped_filter);
        for (i = 0; i < fm.nfiles; i++)
            fprintf(stderr, "[*] extents: %s\n", fm.files[i].path);
    }

done:
    free(res);
    free(fm.v);
    free(fm.files);
    cs_shutdown();
    for (i = 0; i < o.nx86; i++) pe_free(&pes[i]);
    elf_free(&elf);
    return rc;
}
