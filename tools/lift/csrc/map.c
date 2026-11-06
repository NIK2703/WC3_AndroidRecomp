/*
 * map - resolve guest addresses against the PE image and the lift map.
 *
 * Examples:
 *   map 00678084 --image build\war31_unpacked2.exe --map build31\lift_map.tsv
 *   map 00B78084 --runtime-base 00B10000 --preferred-base 00400000 ...
 *   echo 0067AD10 | map --stdin ...
 *
 * The lift map is produced by `lint --map`.  If no map is supplied,
 * --lift-dir scans the generated C catalogs directly (slower).
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
#define MAX_RANGES 4

typedef struct {
    uint32_t start, first, last;
    uint32_t line;
    uint64_t ninstr;
    uint32_t todo;
    int active;
    char file[MAX_FILE];
} Range;

typedef struct {
    Range *v;
    size_t n, cap;
} RangeVec;

typedef struct {
    uint32_t *v;
    size_t n, cap;
} AddrVec;

static void die(const char *msg) {
    fprintf(stderr, "map: %s\n", msg);
    exit(1);
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}

static int parse_hex8(const char *p, uint32_t *out) {
    for (int i = 0; i < 8; i++)
        if (!isxdigit((unsigned char)p[i])) return 0;
    char tmp[9];
    memcpy(tmp, p, 8);
    tmp[8] = 0;
    *out = (uint32_t)strtoul(tmp, NULL, 16);
    return 1;
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

static int ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (m > n) return 0;
    return _stricmp(s + n - m, suffix) == 0;
}

static void addr_add(AddrVec *a, uint32_t v) {
    if (a->n == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 4096;
        a->v = (uint32_t *)xrealloc(a->v, cap * sizeof(uint32_t));
        a->cap = cap;
    }
    a->v[a->n++] = v;
}

static int cmp_u32(const void *pa, const void *pb) {
    uint32_t a = *(const uint32_t *)pa, b = *(const uint32_t *)pb;
    return (a > b) - (a < b);
}

static int addr_has(const AddrVec *a, uint32_t v) {
    size_t lo = 0, hi = a->n;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (a->v[m] < v) lo = m + 1; else hi = m;
    }
    return lo < a->n && a->v[lo] == v;
}

static void load_list(const char *path, AddrVec *out) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "map: cannot open list %s\n", path);
        return;
    }
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        const char *p = line;
        while ((p = strstr(p, "X(")) != NULL) {
            p += 2;
            uint32_t a;
            if (parse_hex8(p, &a)) addr_add(out, a);
            p += 8;
        }
    }
    fclose(f);
    qsort(out->v, out->n, sizeof(uint32_t), cmp_u32);
    size_t w = 0;
    for (size_t i = 0; i < out->n; i++)
        if (!w || out->v[i] != out->v[w - 1]) out->v[w++] = out->v[i];
    out->n = w;
}

static void range_add(RangeVec *r, const Range *x) {
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 1024;
        r->v = (Range *)xrealloc(r->v, cap * sizeof(Range));
        r->cap = cap;
    }
    r->v[r->n++] = *x;
}

static int cmp_range(const void *pa, const void *pb) {
    const Range *a = (const Range *)pa, *b = (const Range *)pb;
    if (a->start != b->start) return (a->start > b->start) - (a->start < b->start);
    return strcmp(a->file, b->file);
}

static int parse_func_line(const char *line, uint32_t *addr) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "void L_", 7) != 0) return 0;
    if (!parse_hex8(p + 7, addr)) return 0;
    if (p[15] != '(') return 0;
    return 1;
}

static int parse_comment_va(const char *p, uint32_t *va) {
    while (*p == ' ' || *p == '\t') p++;
    if (!parse_hex8(p, va)) return 0;
    return p[8] == ':';
}

static void scan_catalog(const char *path, const char *name, RangeVec *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[MAX_LINE];
    uint32_t lineno = 0;
    int cur = -1;
    Range r;
    memset(&r, 0, sizeof(r));
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        uint32_t start;
        if (parse_func_line(line, &start)) {
            if (cur >= 0) range_add(out, &r);
            memset(&r, 0, sizeof(r));
            r.start = start;
            r.line = lineno;
            snprintf(r.file, sizeof(r.file), "%s", name);
            cur = 0;
            continue;
        }
        if (cur < 0) continue;
        const char *c = line;
        while ((c = strstr(c, "/*")) != NULL) {
            c += 2;
            uint32_t va;
            if (parse_comment_va(c, &va)) {
                if (r.ninstr == 0 || va < r.first) r.first = va;
                if (r.ninstr == 0 || va > r.last) r.last = va;
                r.ninstr++;
            }
            const char *close = strstr(c, "*/");
            c = close ? close + 2 : c + 2;
        }
    }
    if (cur >= 0) range_add(out, &r);
    fclose(f);
}

static void load_catalogs(const char *dir, const char *prefix, RangeVec *out) {
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    snprintf(pattern, sizeof(pattern), "%s\\*.c", dir);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) die("cannot enumerate catalog directory");
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (strstr(fd.cFileName, "_stale")) continue;
        if (ends_with_ci(fd.cFileName, ".bak")) continue;
        if (prefix && strncmp(fd.cFileName, prefix, strlen(prefix)) != 0) continue;
        char path[MAX_PATH + 64];
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        scan_catalog(path, fd.cFileName, out);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void load_map(const char *path, RangeVec *out) {
    FILE *f = fopen(path, "r");
    if (!f) die("cannot open lift map");
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (strncmp(line, "addr\t", 5) == 0) continue;
        Range r;
        memset(&r, 0, sizeof(r));
        int active = 0;
        unsigned line_no = 0, todo = 0;
        unsigned long long ninstr = 0;
        unsigned start = 0, first = 0, last = 0;
        char file[MAX_FILE] = {0};
        char flags[32] = {0};
        int n = sscanf(line, "%x\t%259[^\t]\t%u\t%x\t%x\t%llu\t%u\t%d\t%31s",
                       &start, file, &line_no, &first, &last, &ninstr,
                       &todo, &active, flags);
        if (n < 8) continue;
        r.start = (uint32_t)start;
        r.first = (uint32_t)first;
        r.last = (uint32_t)last;
        r.line = (uint32_t)line_no;
        r.ninstr = (uint64_t)ninstr;
        r.todo = (uint32_t)todo;
        r.active = active;
        snprintf(r.file, sizeof(r.file), "%s", file);
        range_add(out, &r);
    }
    fclose(f);
}

static void print_addr(const AddrVec *list, const RangeVec *ranges, Pe *pe,
                       uint32_t input, uint32_t va, uint32_t nbytes) {
    printf("%08X", input);
    if (va != input) printf("  preferred=%08X", va);

    int si = pe ? pe_section_for_va(pe, va) : -1;
    if (si >= 0) {
        uint32_t sec_va = pe->base + pe->sec[si].va;
        uint32_t off = pe->sec[si].rawptr + (va - sec_va);
        printf("  rva=%08X  %s+%X  fileoff=%08X", va - pe->base,
               pe->sec[si].name, va - sec_va, off);
    } else if (pe) {
        printf("  not-in-image");
    }
    printf("\n");

    if (addr_has(list, va)) printf("  list: L_%08X (start)\n", va);
    else printf("  list: not an entry\n");

    unsigned shown = 0;
    for (size_t i = 0; i < ranges->n; i++) {
        const Range *r = &ranges->v[i];
        uint32_t l = r->ninstr ? r->first : r->start;
        uint32_t h = r->ninstr ? r->last : r->start;
        if (va < l || va > h) continue;
        if (r->ninstr && va == r->start)
            printf("  lift: L_%08X (start)  %s:%u  range=%08X-%08X%s\n",
                   r->start, r->file, r->line, l, h, r->active ? " [active]" : "");
        else
            printf("  lift: L_%08X +0x%X  %s:%u  range=%08X-%08X%s\n",
                   r->start, va - r->start, r->file, r->line, l, h,
                   r->active ? " [active]" : "");
        if (++shown >= MAX_RANGES) {
            printf("  lift: ... more containing ranges\n");
            break;
        }
    }
    if (!shown) printf("  lift: no generated function contains this VA\n");

    if (nbytes) {
        uint32_t off;
        uint8_t buf[256];
        uint32_t n = nbytes > sizeof(buf) ? (uint32_t)sizeof(buf) : nbytes;
        if (pe && pe_va_to_off(pe, va, &off) && pe_read_at(pe, off, buf, n)) {
            printf("  bytes:");
            for (uint32_t i = 0; i < n; i++) printf(" %02X", buf[i]);
            printf("\n");
        } else {
            printf("  bytes: unmapped\n");
        }
    }
}

int main(int argc, char **argv) {
    const char *image = NULL;
    const char *list_path = NULL;
    const char *map_path = NULL;
    const char *lift_dir = NULL;
    const char *prefix = "war31_lifted";
    uint32_t runtime_base = 0, preferred_base = 0x00400000u;
    uint32_t nbytes = 0;
    int stdin_mode = 0;
    uint32_t args[256];
    size_t nargs = 0;

    if (argc < 2) {
        fprintf(stderr,
                "usage: map <va>... [options]\n"
                "  --image PATH           PE32 image for section/file-offset lookup\n"
                "  --list FILE            active *_list.h for exact starts\n"
                "  --map FILE             TSV map from lint --map\n"
                "  --lift-dir DIR         scan catalogs if --map is absent\n"
                "  --prefix P             catalog prefix (default war31_lifted)\n"
                "  --runtime-base HEX     convert runtime VA to preferred VA\n"
                "  --preferred-base HEX   preferred image base (default 00400000)\n"
                "  --bytes N              dump up to 256 raw bytes at each VA\n"
                "  --stdin                read addresses from stdin\n");
        return 2;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) image = argv[++i];
        else if (strcmp(argv[i], "--list") == 0 && i + 1 < argc) list_path = argv[++i];
        else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) map_path = argv[++i];
        else if (strcmp(argv[i], "--lift-dir") == 0 && i + 1 < argc) lift_dir = argv[++i];
        else if (strcmp(argv[i], "--prefix") == 0 && i + 1 < argc) prefix = argv[++i];
        else if (strcmp(argv[i], "--runtime-base") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &runtime_base)) die("bad --runtime-base");
        } else if (strcmp(argv[i], "--preferred-base") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &preferred_base)) die("bad --preferred-base");
        } else if (strcmp(argv[i], "--bytes") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long v = strtoul(argv[++i], &end, 0);
            if (end == argv[i] || *end || v > 256) die("bad --bytes");
            nbytes = (uint32_t)v;
        } else if (strcmp(argv[i], "--stdin") == 0) stdin_mode = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "map: unknown option %s\n", argv[i]);
            return 2;
        } else {
            uint32_t a;
            if (nargs >= sizeof(args) / sizeof(args[0])) die("too many addresses");
            if (!parse_u32(argv[i], &a)) die("bad address");
            args[nargs++] = a;
        }
    }

    Pe pe;
    Pe *pep = NULL;
    if (image) {
        if (!pe_open(&pe, image)) die("cannot open PE image");
        pep = &pe;
        /* The range printed here is what probe_loop27.ps1 uses to decide which
         * module owns a probe target, so it has to cover every executable
         * section - not just .text.  Mss32.dll's MSSMIXER lives above .text and
         * would otherwise be attributed to no module at all. */
        printf("image=%s base=%08X text=%08X-%08X",
               image, pe.base,
               pe.ncode ? pe.code_lo : pe.text_va,
               pe.ncode ? pe.code_hi : pe.text_va + pe.text_size);
        if (pe.ncode > 1) {
            printf(" codesecs=");
            int first = 1;
            for (int i = 0; i < pe.nsec; i++) {
                if (!(pe.sec[i].chars & PE_SCN_MEM_EXECUTE) || !pe.sec[i].rawsize)
                    continue;
                uint32_t lo = pe.base + pe.sec[i].va;
                printf("%s%s=%08X-%08X", first ? "" : ",",
                       pe.sec[i].name, lo, lo + pe.sec[i].rawsize);
                first = 0;
            }
        }
        printf("\n");
    }

    AddrVec list = {0};
    if (list_path) load_list(list_path, &list);

    RangeVec ranges = {0};
    if (map_path) load_map(map_path, &ranges);
    else if (lift_dir) load_catalogs(lift_dir, prefix, &ranges);
    if (ranges.n) {
        qsort(ranges.v, ranges.n, sizeof(Range), cmp_range);
        printf("lift ranges=%zu (%s)\n", ranges.n,
               map_path ? map_path : lift_dir ? lift_dir : "");
    }
    printf("\n");

    for (size_t i = 0; i < nargs; i++) {
        uint32_t input = args[i];
        uint32_t va = input;
        if (runtime_base && input >= runtime_base)
            va = input - runtime_base + preferred_base;
        print_addr(&list, &ranges, pep, input, va, nbytes);
    }

    if (stdin_mode) {
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), stdin)) {
            char *p = line;
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p || *p == '#') continue;
            char *end = p;
            while (*end && !isspace((unsigned char)*end)) end++;
            char save = *end;
            *end = 0;
            uint32_t a;
            if (parse_u32(p, &a)) {
                uint32_t va = a;
                if (runtime_base && a >= runtime_base)
                    va = a - runtime_base + preferred_base;
                print_addr(&list, &ranges, pep, a, va, nbytes);
            }
            *end = save;
        }
    }

    if (pep) pe_close(pep);
    free(list.v);
    free(ranges.v);
    return 0;
}
