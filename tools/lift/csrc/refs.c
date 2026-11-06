/*
 * refs - static dispatch-reference audit for the generated lift catalogs.
 *
 * The runtime probe only sees targets that are actually executed.  A direct
 * `dispatch(c, 0x...)` or `dispatch_jmp(c, 0x...)` in a lifted function is
 * already proof that the target is reachable, even if the current run never
 * gets there.  This tool scans the generated C, compares every direct target
 * with the active list, lift map, and generated `void L_...` definitions, and
 * reports targets that have no covering lift.
 *
 * This is the static counterpart of probe.  It catches the two common
 * failures early:
 *   - a direct branch/call target was never lifted;
 *   - an in-.text target is being hidden by the runtime alias fallback.
 *
 * Read-only unless --out-block/--out-full are supplied.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <windows.h>
#include "pe.h"

#define MAX_LINE 8192
#define MAX_FILE 260
#define MAX_RANGES 16
#define MAX_FIELDS 12

typedef struct {
    uint32_t addr;
    char file[MAX_FILE];
    unsigned line;
    unsigned calls, jmps;
    unsigned pcalls, pjmps; /* refs originating inside the protector range */
} Ref;

typedef struct {
    Ref *v;
    size_t n, cap;
} RefVec;

typedef struct {
    uint32_t addr;
    unsigned calls, jmps;
    unsigned pcalls, pjmps;
    char file[MAX_FILE];
    unsigned line;
    int block;
} Miss;

typedef struct {
    Miss *v;
    size_t n, cap;
} MissVec;

typedef struct {
    uint32_t first, last;
} Range;

typedef struct {
    Range *v;
    size_t n, cap;
} RangeVec;

typedef struct {
    uint32_t *v;
    size_t n, cap;
} AddrVec;

typedef struct {
    uint32_t lo, hi;
} Prot;

static Prot g_prot[MAX_RANGES];
static int g_nprot;

static void die(const char *msg) {
    fprintf(stderr, "refs: %s\n", msg);
    exit(1);
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}

static int parse_hex32(const char *s, uint32_t *out) {
    char *end = NULL;
    unsigned long v;
    if (!s || !*s) return 0;
    v = strtoul(s, &end, 16);
    if (end == s) return 0;
    *out = (uint32_t)v;
    return 1;
}

static int is_hex8(const char *p) {
    for (int i = 0; i < 8; i++)
        if (!isxdigit((unsigned char)p[i])) return 0;
    return 1;
}

static void ref_add(RefVec *r, uint32_t addr, const char *file,
                    unsigned line, int is_jmp, int src_prot) {
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 8192;
        r->v = (Ref *)xrealloc(r->v, cap * sizeof(Ref));
        r->cap = cap;
    }
    Ref *e = &r->v[r->n++];
    memset(e, 0, sizeof(*e));
    e->addr = addr;
    snprintf(e->file, sizeof(e->file), "%s", file);
    e->line = line;
    if (src_prot) {
        if (is_jmp) e->pjmps = 1; else e->pcalls = 1;
    } else {
        if (is_jmp) e->jmps = 1; else e->calls = 1;
    }
}

static void miss_add(MissVec *m, uint32_t addr, unsigned calls, unsigned jmps,
                     unsigned pcalls, unsigned pjmps,
                     const char *file, unsigned line, int block) {
    if (m->n == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 1024;
        m->v = (Miss *)xrealloc(m->v, cap * sizeof(Miss));
        m->cap = cap;
    }
    Miss *e = &m->v[m->n++];
    e->addr = addr;
    e->calls = calls;
    e->jmps = jmps;
    e->pcalls = pcalls;
    e->pjmps = pjmps;
    snprintf(e->file, sizeof(e->file), "%s", file);
    e->line = line;
    e->block = block;
}

static int cmp_miss(const void *pa, const void *pb) {
    const Miss *a = (const Miss *)pa;
    const Miss *b = (const Miss *)pb;
    if (a->calls != b->calls) return a->calls > b->calls ? -1 : 1;
    if (a->jmps != b->jmps) return a->jmps > b->jmps ? -1 : 1;
    return (a->addr > b->addr) - (a->addr < b->addr);
}

static void range_add(RangeVec *r, uint32_t first, uint32_t last) {
    if (first > last) return;
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 4096;
        r->v = (Range *)xrealloc(r->v, cap * sizeof(Range));
        r->cap = cap;
    }
    r->v[r->n].first = first;
    r->v[r->n].last = last;
    r->n++;
}

static void addr_add(AddrVec *a, uint32_t addr) {
    if (a->n == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 4096;
        a->v = (uint32_t *)xrealloc(a->v, cap * sizeof(uint32_t));
        a->cap = cap;
    }
    a->v[a->n++] = addr;
}

static int cmp_ref(const void *pa, const void *pb) {
    const Ref *a = (const Ref *)pa;
    const Ref *b = (const Ref *)pb;
    if (a->addr != b->addr) return (a->addr > b->addr) - (a->addr < b->addr);
    if (a->line != b->line) return (a->line > b->line) - (a->line < b->line);
    return strcmp(a->file, b->file);
}

static int cmp_range(const void *pa, const void *pb) {
    const Range *a = (const Range *)pa;
    const Range *b = (const Range *)pb;
    if (a->first != b->first) return (a->first > b->first) - (a->first < b->first);
    return (a->last > b->last) - (a->last < b->last);
}

static int cmp_u32(const void *pa, const void *pb) {
    uint32_t a = *(const uint32_t *)pa, b = *(const uint32_t *)pb;
    return (a > b) - (a < b);
}

static int addr_has(const AddrVec *a, uint32_t x) {
    size_t lo = 0, hi = a->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a->v[mid] == x) return 1;
        if (a->v[mid] < x) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

static int in_prot(uint32_t a) {
    for (int i = 0; i < g_nprot; i++)
        if (a >= g_prot[i].lo && a < g_prot[i].hi) return 1;
    return 0;
}

static int active_has(const AddrVec *a, uint32_t x) {
    size_t lo = 0, hi = a->n;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (a->v[m] < x) lo = m + 1; else hi = m;
    }
    return lo < a->n && a->v[lo] == x;
}

static int range_contains(const RangeVec *r, uint32_t x) {
    size_t lo = 0, hi = r->n;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (r->v[m].first <= x) lo = m + 1; else hi = m;
    }
    if (!lo) return 0;
    return x <= r->v[lo - 1].last;
}

/* Load active list addresses from X(XXXXXXXX) entries. */
static void load_active(const char *path, AddrVec *a) {
    FILE *f = fopen(path, "r");
    if (!f) die("cannot open --active-list");
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "X(");
        while (p) {
            p += 2;
            if (is_hex8(p)) {
                char tmp[9];
                memcpy(tmp, p, 8);
                tmp[8] = 0;
                addr_add(a, (uint32_t)strtoul(tmp, NULL, 16));
            }
            p = strstr(p, "X(");
        }
    }
    fclose(f);
    qsort(a->v, a->n, sizeof(uint32_t), cmp_u32);
    size_t w = 0;
    for (size_t i = 0; i < a->n; i++)
        if (!i || a->v[i] != a->v[i - 1]) a->v[w++] = a->v[i];
    a->n = w;
}

/* Load active ranges from the lint TSV map. */
static void load_map(const char *path, RangeVec *r) {
    FILE *f = fopen(path, "r");
    if (!f) die("cannot open --map");
    char line[MAX_LINE];
    unsigned long long lines = 0, skipped = 0;
    while (fgets(line, sizeof(line), f)) {
        lines++;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char *fields[MAX_FIELDS] = {0};
        int nf = 0;
        char *p = line;
        while (nf < MAX_FIELDS && p && *p) {
            fields[nf++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = 0;
            p = tab + 1;
        }
        if (nf < 8) { skipped++; continue; }
        uint32_t first, last;
        if (!parse_hex32(fields[3], &first) ||
            !parse_hex32(fields[4], &last)) {
            skipped++;
            continue;
        }
        if (strtoul(fields[7], NULL, 10) != 1) continue;
        range_add(r, first, last);
    }
    fclose(f);
    qsort(r->v, r->n, sizeof(Range), cmp_range);
    /* Merge overlapping and adjacent ranges. */
    size_t w = 0;
    for (size_t i = 0; i < r->n; i++) {
        if (!w || (uint64_t)r->v[i].first > (uint64_t)r->v[w - 1].last + 1) {
            r->v[w++] = r->v[i];
        } else if (r->v[i].last > r->v[w - 1].last) {
            r->v[w - 1].last = r->v[i].last;
        }
    }
    r->n = w;
    printf("map: %s (%llu lines, %zu active ranges, %llu skipped)\n",
           path, lines, r->n, skipped);
}

static void scan_file(const char *path, const char *name, RefVec *refs,
                      AddrVec *defined) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[MAX_LINE];
    unsigned lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        /* A generated function definition is proof that the target is lifted,
         * even if its extraN.c has not been built into the active list yet. */
        const char *def = strstr(line, "void L_");
        if (def && is_hex8(def + 7)) {
            char dtmp[9];
            memcpy(dtmp, def + 7, 8);
            dtmp[8] = 0;
            addr_add(defined, (uint32_t)strtoul(dtmp, NULL, 16));
        }
        static const char *tokens[2] = { "dispatch(c, 0x", "dispatch_jmp(c, 0x" };
        for (int k = 0; k < 2; k++) {
            const char *p = line;
            while ((p = strstr(p, tokens[k])) != NULL) {
                const char *q = p + strlen(tokens[k]);
                if (is_hex8(q)) {
                    char tmp[9];
                    memcpy(tmp, q, 8);
                    tmp[8] = 0;
                    uint32_t target = (uint32_t)strtoul(tmp, NULL, 16);
                    uint32_t src = 0;
                    const char *cm = strstr(q, "/* ");
                    if (cm && is_hex8(cm + 3) && cm[11] == ':') {
                        char stmp[9];
                        memcpy(stmp, cm + 3, 8);
                        stmp[8] = 0;
                        src = (uint32_t)strtoul(stmp, NULL, 16);
                    }
                    ref_add(refs, target, name, lineno, k == 1,
                            src && in_prot(src));
                }
                p = q;
            }
        }
    }
    fclose(f);
}

static int ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (m > n) return 0;
    return _stricmp(s + n - m, suffix) == 0;
}

static int miss_pass(const Miss *e, int calls_only, unsigned min_calls, int prot_only) {
    if (calls_only && !e->calls) return 0;
    if (min_calls && e->calls < min_calls) return 0;
    if (prot_only && !e->block) return 0;
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: refs <lift-dir> [options]\n"
                "  --prefix P          only scan *.c whose name starts with P\n"
                "  --active-list FILE  list header with X(XXXXXXXX) entries\n"
                "  --map FILE          lint TSV range map (recommended)\n"
                "  --image PATH        PE image for .text classification\n"
                "  --prot LO-HI        protector VA range, repeatable (hex, hi exclusive)\n"
                "  --out-block FILE    write missing protector seeds (512-byte units)\n"
                "  --out-full FILE     write missing non-protector seeds (2048-byte units)\n"
                "  --top N             display limit (default 80)\n"
                "  --calls-only        only report missing direct-call targets\n"
                "  --min-calls N       only report targets with at least N call refs\n"
                "  --prot-only         only report missing protector-range targets\n"
                "  --max-seeds N       cap seed lines per output file (0 = all)\n"
                "  --include-prot-source  count refs that originate in the protector range\n"
                "  --strict            exit 1 if a missing direct-call target remains\n"
                "  --strict-all        exit 1 if any missing in-.text target remains\n");
        return 2;
    }

    const char *dir = argv[1];
    const char *prefix = "war31_lifted";
    const char *active_path = NULL;
    const char *map_path = NULL;
    const char *image_path = "build\\war31_unpacked2.exe";
    const char *out_block = NULL;
    const char *out_full = NULL;
    size_t top = 80;
    size_t max_seeds = 0;
    unsigned min_calls = 0;
    int strict = 0, strict_all = 0, calls_only = 0, prot_only = 0;
    int include_prot = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--prefix") == 0 && i + 1 < argc) {
            prefix = argv[++i];
        } else if (strcmp(argv[i], "--active-list") == 0 && i + 1 < argc) {
            active_path = argv[++i];
        } else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            map_path = argv[++i];
        } else if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            image_path = argv[++i];
        } else if (strcmp(argv[i], "--prot") == 0 && i + 1 < argc) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s", argv[++i]);
            char *dash = strchr(buf, '-');
            if (!dash) die("--prot expects LO-HI");
            *dash = 0;
            uint32_t lo, hi;
            if (!parse_hex32(buf, &lo) || !parse_hex32(dash + 1, &hi) || lo >= hi)
                die("bad --prot range");
            if (g_nprot < MAX_RANGES) {
                g_prot[g_nprot].lo = lo;
                g_prot[g_nprot].hi = hi;
                g_nprot++;
            }
        } else if (strcmp(argv[i], "--out-block") == 0 && i + 1 < argc) {
            out_block = argv[++i];
        } else if (strcmp(argv[i], "--out-full") == 0 && i + 1 < argc) {
            out_full = argv[++i];
        } else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            top = (size_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--max-seeds") == 0 && i + 1 < argc) {
            max_seeds = (size_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--min-calls") == 0 && i + 1 < argc) {
            min_calls = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--calls-only") == 0) {
            calls_only = 1;
        } else if (strcmp(argv[i], "--prot-only") == 0) {
            prot_only = 1;
        } else if (strcmp(argv[i], "--include-prot-source") == 0) {
            include_prot = 1;
        } else if (strcmp(argv[i], "--strict") == 0) {
            strict = 1;
        } else if (strcmp(argv[i], "--strict-all") == 0) {
            strict_all = 1;
        } else {
            fprintf(stderr, "refs: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    if (!active_path && !map_path)
        die("need --active-list and/or --map");

    AddrVec active = {0}, defined = {0};
    RangeVec ranges = {0};
    if (active_path) load_active(active_path, &active);
    if (map_path) load_map(map_path, &ranges);

    Pe pe;
    int have_pe = 0;
    if (image_path) {
        have_pe = pe_open(&pe, image_path);
        if (!have_pe)
            fprintf(stderr, "refs: warning: cannot open image %s; .text classification disabled\n",
                    image_path);
        else
            printf("image: %s base=%08X text=%08X-%08X\n",
                   image_path, pe.base, pe.text_va, pe.text_va + pe.text_size);
    }

    RefVec refs = {0};
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof(pattern), "%s\\*.c", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) die("cannot enumerate lift directory");
    unsigned nsrc = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char *name = fd.cFileName;
        if (strstr(name, "_stale") || ends_with_ci(name, ".bak")) continue;
        if (prefix && strncmp(name, prefix, strlen(prefix)) != 0) continue;
        if (!ends_with_ci(name, ".c")) continue;
        char path[MAX_PATH + 64];
        snprintf(path, sizeof(path), "%s\\%s", dir, name);
        scan_file(path, name, &refs, &defined);
        nsrc++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    qsort(refs.v, refs.n, sizeof(Ref), cmp_ref);
    qsort(defined.v, defined.n, sizeof(uint32_t), cmp_u32);

    size_t unique = 0;
    unsigned long long calls = 0, jmps = 0;
    for (size_t i = 0; i < refs.n; ) {
        size_t j = i + 1;
        while (j < refs.n && refs.v[j].addr == refs.v[i].addr) j++;
        for (size_t k = i; k < j; k++) {
            calls += refs.v[k].calls + refs.v[k].pcalls;
            jmps += refs.v[k].jmps + refs.v[k].pjmps;
        }
        unique++;
        i = j;
    }

    printf("== refs ==\n");
    printf("dir: %s\n", dir);
    printf("sources=%u refs=%zu unique_targets=%zu calls=%llu jmps=%llu source_defs=%zu\n",
           nsrc, refs.n, unique, calls, jmps, defined.n);
    if (active_path) printf("active-list: %s (%zu addresses)\n", active_path, active.n);
    if (map_path) printf("map: %s (%zu merged ranges)\n", map_path, ranges.n);
    if (!ranges.n && !active.n && !defined.n)
        die("no active addresses loaded");

    MissVec miss = {0}, ext = {0}, obf = {0};
    size_t known = 0;
    for (size_t i = 0; i < refs.n; ) {
        size_t j = i + 1;
        unsigned c = refs.v[i].calls, m = refs.v[i].jmps;
        unsigned pc = refs.v[i].pcalls, pm = refs.v[i].pjmps;
        while (j < refs.n && refs.v[j].addr == refs.v[i].addr) {
            c += refs.v[j].calls;
            m += refs.v[j].jmps;
            pc += refs.v[j].pcalls;
            pm += refs.v[j].pjmps;
            j++;
        }
        uint32_t a = refs.v[i].addr;
        int covered = active_has(&active, a) || addr_has(&defined, a) ||
                      (ranges.n && range_contains(&ranges, a));
        int in_text = have_pe ? pe_in_text(&pe, a) : 1;
        if (!in_text) {
            miss_add(&ext, a, c + pc, m + pm, pc, pm,
                     refs.v[i].file, refs.v[i].line, 0);
        } else if (covered) {
            known++;
        } else if (c || m || include_prot) {
            miss_add(&miss, a, c + (include_prot ? pc : 0),
                     m + (include_prot ? pm : 0), pc, pm,
                     refs.v[i].file, refs.v[i].line, in_prot(a));
        } else if (pc || pm) {
            miss_add(&obf, a, 0, 0, pc, pm,
                     refs.v[i].file, refs.v[i].line, in_prot(a));
        }
        i = j;
    }
    qsort(miss.v, miss.n, sizeof(Miss), cmp_miss);
    qsort(ext.v, ext.n, sizeof(Miss), cmp_miss);
    qsort(obf.v, obf.n, sizeof(Miss), cmp_miss);

    size_t nblock = 0, nfull = 0, miss_with_calls = 0, miss_with_jmps = 0;
    for (size_t i = 0; i < miss.n; i++) {
        if (miss.v[i].block) nblock++; else nfull++;
        if (miss.v[i].calls) miss_with_calls++;
        if (miss.v[i].jmps) miss_with_jmps++;
    }

    printf("\ncoverage: known=%zu external=%zu missing=%zu (calls=%zu jmps=%zu block=%zu full=%zu) obf-source=%zu\n",
           known, ext.n, miss.n, miss_with_calls, miss_with_jmps, nblock, nfull, obf.n);
    if (calls_only || min_calls || prot_only)
        printf("filters: calls_only=%d min_calls=%u prot_only=%d max_seeds=%zu\n",
               calls_only, min_calls, prot_only, max_seeds);

    size_t filtered = 0, filtered_calls = 0;
    for (size_t i = 0; i < miss.n; i++) {
        if (!miss_pass(&miss.v[i], calls_only, min_calls, prot_only)) continue;
        filtered++;
        if (miss.v[i].calls) filtered_calls++;
    }

    printf("\nmissing in-.text targets:\n");
    size_t shown = 0;
    for (size_t i = 0; i < miss.n; i++) {
        Miss *e = &miss.v[i];
        if (!miss_pass(e, calls_only, min_calls, prot_only)) continue;
        if (shown < top) {
            printf("  %08X calls=%-3u jmps=%-3u", e->addr, e->calls, e->jmps);
            if (e->pcalls || e->pjmps)
                printf(" prot=%u/%u", e->pcalls, e->pjmps);
            printf(" first=%s:%u mode=%s\n", e->file, e->line,
                   e->block ? "block" : "full");
        }
        shown++;
    }
    if (!filtered) printf("  none\n");
    else if (shown > top) printf("  ... %zu more\n", shown - top);
    printf("filtered: %zu target(s), %zu with direct-call refs\n", filtered, filtered_calls);

    if (ext.n) {
        printf("\nexternal/non-.text targets (not liftable; inspect alias/data):\n");
        size_t ex = ext.n < 20 ? ext.n : 20;
        for (size_t i = 0; i < ex; i++)
            printf("  %08X calls=%u jmps=%u first=%s:%u\n",
                   ext.v[i].addr, ext.v[i].calls, ext.v[i].jmps,
                   ext.v[i].file, ext.v[i].line);
        if (ext.n > ex) printf("  ... %zu more\n", ext.n - ex);
    }

    if (obf.n) {
        printf("\nobfuscated-source-only targets (excluded by default; use --include-prot-source):\n");
        size_t ex = obf.n < 20 ? obf.n : 20;
        for (size_t i = 0; i < ex; i++)
            printf("  %08X prot_calls=%u prot_jmps=%u first=%s:%u\n",
                   obf.v[i].addr, obf.v[i].pcalls, obf.v[i].pjmps,
                   obf.v[i].file, obf.v[i].line);
        if (obf.n > ex) printf("  ... %zu more\n", obf.n - ex);
    }

    if (out_block) {
        FILE *o = fopen(out_block, "w");
        if (!o) die("cannot write --out-block");
        size_t n = 0;
        for (size_t i = 0; i < miss.n; i++) {
            Miss *e = &miss.v[i];
            if (!e->block || !miss_pass(e, calls_only, min_calls, prot_only)) continue;
            if (max_seeds && n >= max_seeds) break;
            fprintf(o, "0x%08X 512 refs_%08X\n", e->addr, e->addr);
            n++;
        }
        fclose(o);
        printf("\nwrote %zu block seeds -> %s\n", n, out_block);
    }
    if (out_full) {
        FILE *o = fopen(out_full, "w");
        if (!o) die("cannot write --out-full");
        size_t n = 0;
        for (size_t i = 0; i < miss.n; i++) {
            Miss *e = &miss.v[i];
            if (e->block || !miss_pass(e, calls_only, min_calls, prot_only)) continue;
            if (max_seeds && n >= max_seeds) break;
            fprintf(o, "0x%08X 2048 refs_%08X\n", e->addr, e->addr);
            n++;
        }
        fclose(o);
        printf("wrote %zu full seeds -> %s\n", n, out_full);
    }

    printf("\nverdict:\n");
    if (filtered_calls) {
        printf("  MISSING DIRECT-CALL TARGETS: %zu filtered target(s) have call refs.\n", filtered_calls);
        printf("  These are high-confidence function entries; lift them first.\n");
    } else if (filtered) {
        printf("  MISSING BRANCH TARGETS: %zu filtered target(s) are branch-only.\n", filtered);
        printf("  They may be block-lift gaps or dead/obfuscated labels; review before bulk lifting.\n");
    } else if (miss.n) {
        printf("  CLEAN for the selected filter; %zu unfiltered missing target(s) remain.\n", miss.n);
    } else {
        printf("  CLEAN: every direct in-.text dispatch target is covered by an active lift.\n");
    }

    if (have_pe) pe_close(&pe);
    free(refs.v);
    free(miss.v);
    free(ext.v);
    free(obf.v);
    free(active.v);
    free(defined.v);
    free(ranges.v);
    if (strict_all) return filtered ? 1 : 0;
    return (strict && filtered_calls) ? 1 : 0;
}
