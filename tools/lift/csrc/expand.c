/*
 * expand - static seed extractor for the WC3 recompiler.
 *
 * Finds constructs that a plain direct-branch closure misses:
 *   - absolute/relative 32-bit jump tables used by `jmp [reg*4+disp32]`
 *   - the packer's SEH `ud2` trap and its resume address
 *   - generic ud2 / int 0x29 sites (reported, not seeded)
 *   - optionally, direct rel32 call/jmp targets (noisy; opt-in)
 *
 * The image is a PE32 dumped at its preferred base (e.g. war31_unpacked2.exe).
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <direct.h>
#include <errno.h>
#include <windows.h>

#define MAX_SEC 96
#define MAX_TABLE 4096

typedef struct {
    char name[9];
    uint32_t vsize, va, rawsize, rawptr, chars;
} Sec;

typedef struct {
    FILE *f;
    long long fsize;
    uint32_t base, image_size;
    int nsec;
    Sec sec[MAX_SEC];
    uint32_t text_va, text_size, text_off;
    uint8_t *text;
} Pe;

typedef struct {
    uint32_t target, site, table;
    uint32_t index;
} Seed;

typedef struct {
    Seed *v;
    size_t n, cap;
} SeedVec;

typedef struct {
    uint32_t site, table;
    uint32_t count;
    uint32_t first, last;
    int mode; /* 1 = absolute, 2 = relative */
} TableRec;

typedef struct {
    TableRec *v;
    size_t n, cap;
} TableVec;

typedef struct {
    uint32_t trap, resume;
} TrapRec;

typedef struct {
    TrapRec *v;
    size_t n, cap;
} TrapVec;

static void die(const char *msg) {
    fprintf(stderr, "expand: %s\n", msg);
    exit(1);
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_at(Pe *pe, long long off, void *buf, size_t n) {
    if (off < 0 || (long long)n > pe->fsize - off) return 0;
    if (fseek(pe->f, (long)off, SEEK_SET) != 0) return 0;
    return fread(buf, 1, n, pe->f) == n;
}

static int va_to_off(Pe *pe, uint32_t va, uint32_t *off_out) {
    for (int i = 0; i < pe->nsec; i++) {
        uint32_t start = pe->base + pe->sec[i].va;
        uint32_t size = pe->sec[i].rawsize ? pe->sec[i].rawsize : pe->sec[i].vsize;
        if (va >= start && va - start < size) {
            *off_out = pe->sec[i].rawptr + (va - start);
            return 1;
        }
    }
    return 0;
}

static int pe_read_u32(Pe *pe, uint32_t va, uint32_t *out) {
    if (va >= pe->text_va && va - pe->text_va < pe->text_size) {
        *out = rd32(pe->text + (va - pe->text_va));
        return 1;
    }
    uint32_t off;
    if (!va_to_off(pe, va, &off)) return 0;
    uint8_t b[4];
    if (!read_at(pe, off, b, 4)) return 0;
    *out = rd32(b);
    return 1;
}

static int in_text(Pe *pe, uint32_t va) {
    return va >= pe->text_va && va - pe->text_va < pe->text_size;
}

static int pe_open(Pe *pe, const char *path) {
    uint8_t hdr[0x400];
    memset(pe, 0, sizeof(*pe));
    pe->f = fopen(path, "rb");
    if (!pe->f) return 0;
    if (fseek(pe->f, 0, SEEK_END) != 0) return 0;
    pe->fsize = ftell(pe->f);
    if (pe->fsize < 0x100) return 0;
    if (!read_at(pe, 0, hdr, 0x40)) return 0;
    if (hdr[0] != 'M' || hdr[1] != 'Z') return 0;
    uint32_t lfanew = rd32(hdr + 0x3C);
    uint8_t nt[24];
    if (!read_at(pe, lfanew, nt, sizeof(nt))) return 0;
    if (rd32(nt) != 0x00004550u) return 0;
    uint16_t nsec = rd16(nt + 6);
    uint16_t sizeopt = rd16(nt + 20);
    if (nsec < 1 || nsec > MAX_SEC || sizeopt < 96 || sizeopt > 0x400) return 0;

    uint8_t opt[0x400];
    if (!read_at(pe, (long long)lfanew + 24, opt, sizeopt)) return 0;
    if (rd16(opt) != 0x010B) return 0; /* PE32 only */
    pe->base = rd32(opt + 28);
    pe->image_size = rd32(opt + 56);
    pe->nsec = nsec;

    long long sec_off = (long long)lfanew + 24 + sizeopt;
    for (int i = 0; i < nsec; i++) {
        uint8_t s[40];
        if (!read_at(pe, sec_off + (long long)i * 40, s, 40)) return 0;
        memcpy(pe->sec[i].name, s, 8);
        pe->sec[i].name[8] = 0;
        pe->sec[i].vsize = rd32(s + 8);
        pe->sec[i].va = rd32(s + 12);
        pe->sec[i].rawsize = rd32(s + 16);
        pe->sec[i].rawptr = rd32(s + 20);
        pe->sec[i].chars = rd32(s + 36);
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
    pe->text_size = pe->sec[text].vsize ? pe->sec[text].vsize : pe->sec[text].rawsize;
    if (pe->text_size > pe->sec[text].rawsize && pe->sec[text].rawsize)
        pe->text_size = pe->sec[text].rawsize;
    if (pe->text_size < 16) return 0;
    pe->text = (uint8_t *)malloc(pe->text_size);
    if (!pe->text) return 0;
    if (!read_at(pe, pe->text_off, pe->text, pe->text_size)) return 0;
    return 1;
}

static void pe_close(Pe *pe) {
    if (pe->text) free(pe->text);
    if (pe->f) fclose(pe->f);
    pe->text = NULL;
    pe->f = NULL;
}

static void seed_add(SeedVec *v, uint32_t target, uint32_t site, uint32_t table, uint32_t idx) {
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 256;
        v->v = (Seed *)xrealloc(v->v, cap * sizeof(Seed));
        v->cap = cap;
    }
    v->v[v->n].target = target;
    v->v[v->n].site = site;
    v->v[v->n].table = table;
    v->v[v->n].index = idx;
    v->n++;
}

static int cmp_seed(const void *pa, const void *pb) {
    const Seed *a = (const Seed *)pa;
    const Seed *b = (const Seed *)pb;
    if (a->target != b->target) return (a->target > b->target) - (a->target < b->target);
    return (a->site > b->site) - (a->site < b->site);
}

static void table_add(TableVec *v, const TableRec *r) {
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 32;
        v->v = (TableRec *)xrealloc(v->v, cap * sizeof(TableRec));
        v->cap = cap;
    }
    v->v[v->n++] = *r;
}

static void trap_add(TrapVec *v, uint32_t trap, uint32_t resume) {
    for (size_t i = 0; i < v->n; i++)
        if (v->v[i].trap == trap) return;
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 16;
        v->v = (TrapRec *)xrealloc(v->v, cap * sizeof(TrapRec));
        v->cap = cap;
    }
    v->v[v->n].trap = trap;
    v->v[v->n].resume = resume;
    v->n++;
}

static int scan_table(Pe *pe, uint32_t site, uint32_t table, uint32_t maxent,
                      SeedVec *seeds, TableRec *out) {
    uint32_t vals[MAX_TABLE];
    uint32_t n = 0;
    int mode = 0;
    if (table & 3) return 0;
    if (table < pe->base || table - pe->base >= pe->image_size) return 0;
    if (maxent > MAX_TABLE) maxent = MAX_TABLE;
    for (uint32_t k = 0; k < maxent; k++) {
        uint32_t v;
        if (!pe_read_u32(pe, table + k * 4, &v)) break;
        if (v == 0xCCCCCCCCu || v == 0u) break;
        if (!mode) {
            if (in_text(pe, v)) mode = 1;
            else if (in_text(pe, table + v)) mode = 2;
            else break;
        }
        uint32_t target = (mode == 1) ? v : (table + v);
        if (!in_text(pe, target)) break;
        vals[n++] = target;
    }
    if (n < 2) return 0;
    out->site = site;
    out->table = table;
    out->count = n;
    out->first = vals[0];
    out->last = vals[n - 1];
    out->mode = mode;
    for (uint32_t i = 0; i < n; i++) seed_add(seeds, vals[i], site, table, i);
    return 1;
}

static void ensure_dir(const char *dir) {
    if (!dir || !*dir) return;
    _mkdir(dir);
}

static void path_join(char *dst, size_t cap, const char *dir, const char *name) {
    if (dir && *dir && strcmp(dir, ".") != 0)
        snprintf(dst, cap, "%s\\%s", dir, name);
    else
        snprintf(dst, cap, "%s", name);
}

static size_t seed_write_unique(const char *path, SeedVec *v, const char *tag, int size) {
    if (!v->n) {
        FILE *f = fopen(path, "w");
        if (f) fclose(f);
        return 0;
    }
    qsort(v->v, v->n, sizeof(Seed), cmp_seed);
    FILE *f = fopen(path, "w");
    if (!f) die("cannot write seed file");
    size_t n = 0;
    uint32_t last = 0;
    for (size_t i = 0; i < v->n; i++) {
        if (i && v->v[i].target == last) continue;
        last = v->v[i].target;
        fprintf(f, "0x%08X %d %s_%08X\n", last, size, tag, last);
        n++;
    }
    fclose(f);
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: expand <image.exe> [options]\n"
                "  --out-dir DIR     output directory (default .)\n"
                "  --scan LO-HI      scan only this preferred-VA range (hex)\n"
                "  --max-table N     max entries per jump table (default 4096)\n"
                "  --rel32           also emit direct rel32 call/jmp targets (noisy)\n"
                "  --quiet           only print the summary line\n");
        return 2;
    }

    const char *image = argv[1];
    const char *out_dir = ".";
    uint32_t scan_lo = 0, scan_hi = 0;
    uint32_t max_table = MAX_TABLE;
    int do_rel32 = 0, quiet = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--out-dir") == 0 && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (strcmp(argv[i], "--scan") == 0 && i + 1 < argc) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s", argv[++i]);
            char *dash = strchr(buf, '-');
            if (!dash) die("--scan expects LO-HI");
            *dash = 0;
            scan_lo = (uint32_t)strtoul(buf, NULL, 16);
            scan_hi = (uint32_t)strtoul(dash + 1, NULL, 16);
            if (scan_lo >= scan_hi) die("bad --scan range");
        } else if (strcmp(argv[i], "--max-table") == 0 && i + 1 < argc) {
            max_table = (uint32_t)strtoul(argv[++i], NULL, 0);
            if (max_table < 2) max_table = 2;
        } else if (strcmp(argv[i], "--rel32") == 0) {
            do_rel32 = 1;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
        } else {
            fprintf(stderr, "expand: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    Pe pe;
    if (!pe_open(&pe, image)) die("not a PE32 image or cannot read .text");
    ensure_dir(out_dir);

    uint32_t lo = 0, hi = pe.text_size;
    if (scan_hi) {
        if (scan_lo <= pe.text_va) lo = 0;
        else if (scan_lo - pe.text_va < pe.text_size) lo = scan_lo - pe.text_va;
        else lo = pe.text_size;
        if (scan_hi <= pe.text_va) hi = 0;
        else if (scan_hi - pe.text_va < pe.text_size) hi = scan_hi - pe.text_va;
        else hi = pe.text_size;
        if (lo >= hi) die("--scan range does not intersect .text");
    }

    SeedVec jt = {0}, traps = {0}, rel = {0};
    TableVec tables = {0};
    TrapVec traprecs = {0};
    uint32_t generic_ud2 = 0, int29 = 0, trap_matches = 0;

    const uint8_t trap_pat[17] = {
        0xC7, 0x45, 0xFC, 0x00, 0x00, 0x00, 0x00,
        0x0F, 0x0B,
        0xC7, 0x45, 0xFC, 0xFE, 0xFF, 0xFF, 0xFF,
        0xE9
    };

    for (uint32_t i = lo; i < hi; i++) {
        if (i + 7 <= hi && pe.text[i] == 0xFF && pe.text[i + 1] == 0x24) {
            uint8_t sib = pe.text[i + 2];
            if ((sib & 7) == 5 && ((sib >> 3) & 7) != 4) {
                uint32_t table = rd32(pe.text + i + 3);
                TableRec tr;
                if (scan_table(&pe, pe.text_va + i, table, max_table, &jt, &tr))
                    table_add(&tables, &tr);
            }
        }
        if (i + 17 <= hi && memcmp(pe.text + i, trap_pat, 17) == 0) {
            uint32_t trap = pe.text_va + i;
            uint32_t resume = trap + 0x15;
            if (in_text(&pe, resume)) {
                trap_matches++;
                trap_add(&traprecs, trap, resume);
                seed_add(&traps, resume, trap, 0, 0);
            }
        }
        if (i + 2 <= hi && pe.text[i] == 0x0F && pe.text[i + 1] == 0x0B)
            generic_ud2++;
        if (i + 2 <= hi && pe.text[i] == 0xCD && pe.text[i + 1] == 0x29)
            int29++;
        if (do_rel32 && i + 5 <= hi && (pe.text[i] == 0xE8 || pe.text[i] == 0xE9)) {
            int32_t r = (int32_t)rd32(pe.text + i + 1);
            uint32_t t = pe.text_va + i + 5 + (uint32_t)r;
            if (in_text(&pe, t)) seed_add(&rel, t, pe.text_va + i, 0, 0);
        }
        if (do_rel32 && i + 6 <= hi && pe.text[i] == 0x0F &&
            pe.text[i + 1] >= 0x80 && pe.text[i + 1] <= 0x8F) {
            int32_t r = (int32_t)rd32(pe.text + i + 2);
            uint32_t t = pe.text_va + i + 6 + (uint32_t)r;
            if (in_text(&pe, t)) seed_add(&rel, t, pe.text_va + i, 0, 0);
        }
    }

    char p_jt[MAX_PATH], p_tr[MAX_PATH], p_rel[MAX_PATH], p_rep[MAX_PATH];
    path_join(p_jt, sizeof(p_jt), out_dir, "seeds_jumptables.txt");
    path_join(p_tr, sizeof(p_tr), out_dir, "seeds_traps.txt");
    path_join(p_rel, sizeof(p_rel), out_dir, "seeds_rel32.txt");
    path_join(p_rep, sizeof(p_rep), out_dir, "expand_report.txt");

    size_t n_jt = seed_write_unique(p_jt, &jt, "jt", 512);
    size_t n_tr = seed_write_unique(p_tr, &traps, "trap", 512);
    size_t n_rel = seed_write_unique(p_rel, &rel, "rel", 512);

    FILE *rep = fopen(p_rep, "w");
    if (!rep) die("cannot write report");
    fprintf(rep, "expand report\n");
    fprintf(rep, "image=%s\n", image);
    fprintf(rep, "base=%08X image_size=%08X\n", pe.base, pe.image_size);
    fprintf(rep, "text_va=%08X text_size=%08X text_off=%08X\n",
            pe.text_va, pe.text_size, pe.text_off);
    fprintf(rep, "scan=%08X-%08X\n", pe.text_va + lo, pe.text_va + hi);
    fprintf(rep, "jump_tables sites=%zu unique_targets=%zu\n", tables.n, n_jt);
    for (size_t i = 0; i < tables.n; i++) {
        fprintf(rep, "  site=%08X table=%08X mode=%s entries=%u first=%08X last=%08X\n",
                tables.v[i].site, tables.v[i].table,
                tables.v[i].mode == 1 ? "abs" : "rel",
                tables.v[i].count, tables.v[i].first, tables.v[i].last);
    }
    fprintf(rep, "ud2_traps pattern=%u unique=%zu generic_ud2=%u int29=%u\n",
            trap_matches, traprecs.n, generic_ud2, int29);
    for (size_t i = 0; i < traprecs.n; i++)
        fprintf(rep, "  trap=%08X resume=%08X\n",
                traprecs.v[i].trap, traprecs.v[i].resume);
    if (do_rel32)
        fprintf(rep, "rel32 unique_targets=%zu\n", n_rel);
    fclose(rep);

    printf("expand: base=%08X text=%08X+%08X\n",
           pe.base, pe.text_va, pe.text_size);
    printf("  jump tables: sites=%zu unique_targets=%zu -> %s\n",
           tables.n, n_jt, p_jt);
    printf("  ud2 traps: pattern=%u unique_resumes=%zu generic=%u int29=%u -> %s\n",
           trap_matches, n_tr, generic_ud2, int29, p_tr);
    if (do_rel32)
        printf("  rel32: unique_targets=%zu -> %s\n", n_rel, p_rel);
    printf("  report: %s\n", p_rep);
    if (!quiet) {
        printf("next: closure_blocks31.py %s <out.txt> <lo> <hi>\n", p_jt);
        printf("      then lift the new seeds in block mode.\n");
    }

    free(jt.v); free(traps.v); free(rel.v);
    free(tables.v); free(traprecs.v);
    pe_close(&pe);
    return 0;
}
