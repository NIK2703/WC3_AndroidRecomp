/*
 * probe - turn a WC3 host probe file into a deduplicated lift plan.
 *
 * Probe lines are "TARGET RA" (hex). Targets inside protector ranges are
 * classified as block-mode, the rest as full-mode. Optional --known-dir
 * filters addresses already present in *_list.h catalogs or generated lift
 * sources (*.c), so an unbuilt extraN.c is not lifted twice.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <windows.h>

#define MAX_LINE 4096
#define MAX_RANGES 16

typedef struct {
    uint32_t target, ra;
    unsigned count;
} Ent;

typedef struct {
    Ent *v;
    size_t n, cap;
} Vec;

typedef struct {
    uint32_t lo, hi; /* [lo, hi) */
} Range;

static Range g_ranges[MAX_RANGES];
static int g_nranges;

static void die(const char *msg) {
    fprintf(stderr, "probe: %s\n", msg);
    exit(1);
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}

static int parse_u32(const char *s, uint32_t *out) {
    char *end = NULL;
    unsigned long v;
    if (!s || !*s) return 0;
    v = strtoul(s, &end, 16);
    if (end == s || *end) return 0;
    *out = (uint32_t)v;
    return 1;
}

static void vec_append(Vec *v, uint32_t target, uint32_t ra) {
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 64;
        v->v = (Ent *)xrealloc(v->v, cap * sizeof(Ent));
        v->cap = cap;
    }
    v->v[v->n].target = target;
    v->v[v->n].ra = ra;
    v->v[v->n].count = 1;
    v->n++;
}

static void vec_add(Vec *v, uint32_t target, uint32_t ra) {
    for (size_t i = 0; i < v->n; i++) {
        if (v->v[i].target == target) {
            v->v[i].count++;
            if (!v->v[i].ra && ra) v->v[i].ra = ra;
            return;
        }
    }
    vec_append(v, target, ra);
}

static int cmp_count(const void *pa, const void *pb) {
    const Ent *a = (const Ent *)pa;
    const Ent *b = (const Ent *)pb;
    if (a->count != b->count) return (a->count < b->count) ? 1 : -1;
    return (a->target > b->target) - (a->target < b->target);
}

static int cmp_target(const void *pa, const void *pb) {
    const Ent *a = (const Ent *)pa;
    const Ent *b = (const Ent *)pb;
    return (a->target > b->target) - (a->target < b->target);
}

static int in_prot(uint32_t a) {
    for (int i = 0; i < g_nranges; i++)
        if (a >= g_ranges[i].lo && a < g_ranges[i].hi) return 1;
    return 0;
}

static int is_hex8(const char *p) {
    for (int i = 0; i < 8; i++)
        if (!isxdigit((unsigned char)p[i])) return 0;
    return 1;
}

static void known_add_hex(Vec *known, const char *p) {
    if (!is_hex8(p)) return;
    char tmp[9];
    memcpy(tmp, p, 8);
    tmp[8] = 0;
    vec_append(known, (uint32_t)strtoul(tmp, NULL, 16), 0);
}

static void known_scan_file(const char *path, Vec *known) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        /* Catalog header: X(00612345). */
        char *p = strstr(line, "X(");
        while (p) {
            known_add_hex(known, p + 2);
            p = strstr(p + 2, "X(");
        }
        /* Generated lift source: void L_00612345(CPU *c).  Scanning the
         * sources as well as the headers prevents a batch from re-lifting a
         * target whose extraN.c exists but has not been built into the
         * active list yet. */
        p = strstr(line, "void L_");
        if (p) known_add_hex(known, p + 7);
    }
    fclose(f);
}

/* Read known addresses from DIR\*_list.h and generated *.c catalogs,
 * skipping stale catalogs. */
static int known_load(const char *dir, const char *prefix, Vec *known) {
    static const char *patterns[] = { "%s\\*_list.h", "%s\\*.c" };
    int found = 0;
    for (int k = 0; k < 2; k++) {
        char pattern[MAX_PATH];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pattern, sizeof(pattern), patterns[k], dir);
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        found = 1;
        do {
            char path[MAX_PATH + 64];
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (strstr(fd.cFileName, "_stale")) continue;
            if (prefix && strncmp(fd.cFileName, prefix, strlen(prefix)) != 0) continue;
            snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
            known_scan_file(path, known);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!found) fprintf(stderr, "probe: no catalogs in %s\n", dir);
    return found;
}

static int known_has(Vec *known, uint32_t a) {
    size_t lo = 0, hi = known->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (known->v[mid].target == a) return 1;
        if (known->v[mid].target < a) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: probe <probe.txt> [options]\n"
                "  --prot LO-HI       protector VA range, repeatable (hex, hi exclusive)\n"
                "  --known-dir DIR    skip targets already in DIR\\*_list.h\n"
                "  --known-prefix P   only read catalog files whose name starts with P\n"
                "  --out-block FILE   write block-mode seed list (512-byte units)\n"
                "  --out-full FILE    write full-mode seed list (2048-byte units)\n"
                "  --image PATH       image path used only in the printed lift command\n"
                "  --top N            display limit (default 60)\n"
                "  --min-count N      ignore targets seen fewer than N times\n");
        return 2;
    }

    const char *probe = argv[1];
    const char *out_block = NULL, *out_full = NULL;
    const char *known_dir = NULL, *known_prefix = NULL, *image = "build\\war31_unpacked2.exe";
    size_t top = 60;
    unsigned min_count = 1;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--prot") == 0 && i + 1 < argc) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s", argv[++i]);
            char *dash = strchr(buf, '-');
            if (!dash) die("--prot expects LO-HI");
            *dash = 0;
            uint32_t lo, hi;
            if (!parse_u32(buf, &lo) || !parse_u32(dash + 1, &hi) || lo >= hi)
                die("bad --prot range");
            if (g_nranges < MAX_RANGES) {
                g_ranges[g_nranges].lo = lo;
                g_ranges[g_nranges].hi = hi;
                g_nranges++;
            }
        } else if (strcmp(argv[i], "--known-dir") == 0 && i + 1 < argc) {
            known_dir = argv[++i];
        } else if (strcmp(argv[i], "--known-prefix") == 0 && i + 1 < argc) {
            known_prefix = argv[++i];
        } else if (strcmp(argv[i], "--out-block") == 0 && i + 1 < argc) {
            out_block = argv[++i];
        } else if (strcmp(argv[i], "--out-full") == 0 && i + 1 < argc) {
            out_full = argv[++i];
        } else if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            image = argv[++i];
        } else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            top = (size_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--min-count") == 0 && i + 1 < argc) {
            min_count = (unsigned)strtoul(argv[++i], NULL, 0);
        } else {
            fprintf(stderr, "probe: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    Vec probev = {0}, known = {0};
    FILE *f = fopen(probe, "r");
    if (!f) die("cannot open probe file");
    char line[MAX_LINE];
    unsigned long long samples = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned t, ra;
        if (sscanf(line, "%x %x", &t, &ra) == 2) {
            vec_add(&probev, t, ra);
            samples++;
        } else if (sscanf(line, "%x", &t) == 1) {
            vec_add(&probev, t, 0);
            samples++;
        }
    }
    fclose(f);

    if (known_dir) known_load(known_dir, known_prefix, &known);
    if (known.n > 1) qsort(known.v, known.n, sizeof(Ent), cmp_target);

    size_t skipped = 0;
    Vec fresh = {0};
    for (size_t i = 0; i < probev.n; i++) {
        if (probev.v[i].count < min_count) continue;
        if (known_has(&known, probev.v[i].target)) { skipped++; continue; }
        vec_add(&fresh, probev.v[i].target, probev.v[i].ra);
        fresh.v[fresh.n - 1].count = probev.v[i].count;
    }

    printf("== probe ==\n");
    printf("probe: %s\n", probe);
    printf("samples=%llu unique=%zu known_skipped=%zu fresh=%zu\n",
           samples, probev.n, skipped, fresh.n);
    if (g_nranges) {
        printf("protector ranges:");
        for (int i = 0; i < g_nranges; i++)
            printf(" %08X-%08X", g_ranges[i].lo, g_ranges[i].hi);
        printf("\n");
    } else {
        printf("protector ranges: none (all targets classified full)\n");
    }

    qsort(fresh.v, fresh.n, sizeof(Ent), cmp_count);
    printf("\nfresh targets:\n");
    size_t shown = fresh.n < top ? fresh.n : top;
    size_t nblock = 0, nfull = 0;
    for (size_t i = 0; i < fresh.n; i++)
        if (in_prot(fresh.v[i].target)) nblock++; else nfull++;
    for (size_t i = 0; i < shown; i++) {
        printf("  %08X count=%-6u ra=%08X mode=%s\n",
               fresh.v[i].target, fresh.v[i].count, fresh.v[i].ra,
               in_prot(fresh.v[i].target) ? "block" : "full");
    }
    if (fresh.n > shown) printf("  ... %zu more\n", fresh.n - shown);
    printf("modes: block=%zu full=%zu\n", nblock, nfull);

    qsort(fresh.v, fresh.n, sizeof(Ent), cmp_target);
    if (out_block) {
        FILE *o = fopen(out_block, "w");
        if (!o) die("cannot write --out-block");
        size_t n = 0;
        for (size_t i = 0; i < fresh.n; i++) {
            if (!in_prot(fresh.v[i].target)) continue;
            fprintf(o, "0x%08X 512 probe_%08X\n", fresh.v[i].target, fresh.v[i].target);
            n++;
        }
        fclose(o);
        printf("wrote %zu block seeds -> %s\n", n, out_block);
    }
    if (out_full) {
        FILE *o = fopen(out_full, "w");
        if (!o) die("cannot write --out-full");
        size_t n = 0;
        for (size_t i = 0; i < fresh.n; i++) {
            if (in_prot(fresh.v[i].target)) continue;
            fprintf(o, "0x%08X 2048 probe_%08X\n", fresh.v[i].target, fresh.v[i].target);
            n++;
        }
        fclose(o);
        printf("wrote %zu full seeds -> %s\n", n, out_full);
    }

    printf("\nnext (run from the recompiler root):\n");
    printf("  $env:WC3_LIFT_BLOCK='1'; python E:\\projects\\refs\\pcrecomp\\tools\\lift\\lift32_cpu.py "
           "%s %s reports\\lift\\war31_lifted_extraNNN.c\n", image,
           out_block ? out_block : "BLOCK_SEEDS");
    printf("  Remove-Item Env:WC3_LIFT_BLOCK; python E:\\projects\\refs\\pcrecomp\\tools\\lift\\lift32_cpu.py "
           "%s %s reports\\lift\\war31_lifted_extraNNN.c\n", image,
           out_full ? out_full : "FULL_SEEDS");
    printf("  powershell -ExecutionPolicy Bypass -File tools\\build_host31.ps1\n");
    return 0;
}
