/*
 * lint - structural audit of generated lift catalogs.
 *
 * Unlike manifest, which checks list/definition bookkeeping, this tool
 * reads the instruction comments inside the generated C and checks:
 *   - the same guest instruction VA translated by more than one function
 *   - the same VA translated with different instruction text (hard conflict)
 *   - functions without any terminal dispatch/return (fall-through risk)
 *   - functions with no instruction at their own start address
 *   - RECOMP_TODO density per function
 *
 * It can also write a TSV range map used by map and stack.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <windows.h>

#define MAX_LINE 8192
#define MAX_FILE 260
#define MAX_HASHES 64

typedef struct {
    uint32_t start;
    uint32_t first_va, last_va;
    uint32_t line_no;
    uint64_t ninstr;
    uint32_t todo;
    unsigned has_dispatch;
    unsigned has_dispatch_jmp;
    unsigned has_return;
    int active;
    char file[MAX_FILE];
} Func;

typedef struct {
    uint32_t va;
    uint32_t func;
    uint64_t hash;
} Occ;

typedef struct {
    uint32_t va;
    size_t first;
    size_t nocc;
    uint32_t nfunc;
    uint32_t nhashes;
    int conflict;
    int active;
} Group;

typedef struct {
    Func *v;
    size_t n, cap;
} FuncVec;

typedef struct {
    Occ *v;
    size_t n, cap;
} OccVec;

typedef struct {
    Group *v;
    size_t n, cap;
} GroupVec;

typedef struct {
    uint32_t *v;
    size_t n, cap;
} AddrVec;

typedef struct {
    uint32_t *v;
    size_t n, cap;
} IdxVec;

static FuncVec g_funcs;
static OccVec g_occ;
static AddrVec g_active;
static GroupVec g_groups;
static uint64_t g_lines;

static void die(const char *msg) {
    fprintf(stderr, "lint: %s\n", msg);
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

static int parse_u32_hex(const char *s, uint32_t *out) {
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
        fprintf(stderr, "lint: cannot open active list %s\n", path);
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

static uint64_t hash_text(const char *s, const char *end) {
    while (s < end && isspace((unsigned char)*s)) s++;
    while (end > s && isspace((unsigned char)end[-1])) end--;
    uint64_t h = 1469598103934665603ULL;
    for (; s < end; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static int parse_comment_va(const char *p, uint32_t *va, uint64_t *hash) {
    while (*p == ' ' || *p == '\t') p++;
    if (!parse_hex8(p, va)) return 0;
    if (p[8] != ':') return 0;
    const char *s = p + 9;
    const char *e = strstr(s, "*/");
    if (!e) e = s + strlen(s);
    *hash = hash_text(s, e);
    return 1;
}

static int parse_func_line(const char *line, uint32_t *addr) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "void L_", 7) != 0) return 0;
    if (!parse_hex8(p + 7, addr)) return 0;
    if (p[15] != '(') return 0;
    return 1;
}

static uint32_t func_add(uint32_t start, const char *file, uint32_t line_no) {
    if (g_funcs.n == g_funcs.cap) {
        size_t cap = g_funcs.cap ? g_funcs.cap * 2 : 1024;
        g_funcs.v = (Func *)xrealloc(g_funcs.v, cap * sizeof(Func));
        g_funcs.cap = cap;
    }
    Func *f = &g_funcs.v[g_funcs.n];
    memset(f, 0, sizeof(*f));
    f->start = start;
    f->first_va = 0;
    f->last_va = 0;
    f->line_no = line_no;
    snprintf(f->file, sizeof(f->file), "%s", file);
    return (uint32_t)g_funcs.n++;
}

static void occ_add(uint32_t va, uint32_t func, uint64_t hash) {
    if (g_occ.n == g_occ.cap) {
        size_t cap = g_occ.cap ? g_occ.cap * 2 : 65536;
        g_occ.v = (Occ *)xrealloc(g_occ.v, cap * sizeof(Occ));
        g_occ.cap = cap;
    }
    g_occ.v[g_occ.n].va = va;
    g_occ.v[g_occ.n].func = func;
    g_occ.v[g_occ.n].hash = hash;
    g_occ.n++;
}

static void scan_source(const char *path, const char *name) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "lint: cannot open %s\n", path);
        return;
    }
    char line[MAX_LINE];
    uint32_t lineno = 0;
    int cur = -1;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        g_lines++;
        uint32_t start;
        if (parse_func_line(line, &start)) {
            cur = (int)func_add(start, name, lineno);
            continue;
        }
        if (cur < 0) continue;
        Func *fn = &g_funcs.v[cur];

        for (const char *q = line; (q = strstr(q, "RECOMP_TODO")) != NULL;
             q += strlen("RECOMP_TODO"))
            fn->todo++;
        if (strstr(line, "dispatch_jmp(")) fn->has_dispatch_jmp = 1;
        if (strstr(line, "dispatch(")) fn->has_dispatch = 1;
        if (strstr(line, "return;")) fn->has_return = 1;

        const char *c = line;
        while ((c = strstr(c, "/*")) != NULL) {
            c += 2;
            uint32_t va;
            uint64_t hash;
            if (parse_comment_va(c, &va, &hash)) {
                occ_add(va, (uint32_t)cur, hash);
                if (fn->ninstr == 0 || va < fn->first_va) fn->first_va = va;
                if (fn->ninstr == 0 || va > fn->last_va) fn->last_va = va;
                fn->ninstr++;
            }
            const char *close = strstr(c, "*/");
            c = close ? close + 2 : c + 2;
        }
    }
    fclose(f);
}

static int cmp_occ(const void *pa, const void *pb) {
    const Occ *a = (const Occ *)pa;
    const Occ *b = (const Occ *)pb;
    if (a->va != b->va) return (a->va > b->va) - (a->va < b->va);
    if (a->func != b->func) return (a->func > b->func) - (a->func < b->func);
    if (a->hash != b->hash) return (a->hash > b->hash) - (a->hash < b->hash);
    return 0;
}

static void group_add(Group g) {
    if (g_groups.n == g_groups.cap) {
        size_t cap = g_groups.cap ? g_groups.cap * 2 : 4096;
        g_groups.v = (Group *)xrealloc(g_groups.v, cap * sizeof(Group));
        g_groups.cap = cap;
    }
    g_groups.v[g_groups.n++] = g;
}

static void build_groups(void) {
    qsort(g_occ.v, g_occ.n, sizeof(Occ), cmp_occ);
    for (size_t i = 0; i < g_occ.n; ) {
        size_t j = i + 1;
        while (j < g_occ.n && g_occ.v[j].va == g_occ.v[i].va) j++;
        uint32_t nfunc = 0;
        uint32_t nhashes = 0;
        uint64_t hashes[MAX_HASHES];
        int active = 0;
        for (size_t k = i; k < j; k++) {
            if (!k || g_occ.v[k].func != g_occ.v[k - 1].func) nfunc++;
            int seen = 0;
            for (uint32_t h = 0; h < nhashes; h++)
                if (hashes[h] == g_occ.v[k].hash) { seen = 1; break; }
            if (!seen && nhashes < MAX_HASHES) hashes[nhashes++] = g_occ.v[k].hash;
            if (g_funcs.v[g_occ.v[k].func].active) active = 1;
        }
        if (nfunc > 1 || j - i > 1) {
            Group g;
            g.va = g_occ.v[i].va;
            g.first = i;
            g.nocc = j - i;
            g.nfunc = nfunc;
            g.nhashes = nhashes;
            g.conflict = nhashes > 1;
            g.active = active;
            group_add(g);
        }
        i = j;
    }
}

static Func *g_sort_funcs;
static int cmp_func_order(const void *pa, const void *pb) {
    uint32_t ia = *(const uint32_t *)pa, ib = *(const uint32_t *)pb;
    const Func *a = &g_sort_funcs[ia], *b = &g_sort_funcs[ib];
    if (a->start != b->start) return (a->start > b->start) - (a->start < b->start);
    return (ia > ib) - (ia < ib);
}

static int cmp_group_risk(const void *pa, const void *pb) {
    const Group *a = (const Group *)pa, *b = (const Group *)pb;
    if (a->conflict != b->conflict) return a->conflict ? -1 : 1;
    if (a->active != b->active) return a->active ? -1 : 1;
    if (a->nfunc != b->nfunc) return (a->nfunc < b->nfunc) ? 1 : -1;
    return (a->va > b->va) - (a->va < b->va);
}

static int cmp_group_va(const void *pa, const void *pb) {
    const Group *a = (const Group *)pa, *b = (const Group *)pb;
    return (a->va > b->va) - (a->va < b->va);
}

static void flags_of(const Func *f, char *buf, size_t cap) {
    size_t n = 0;
    if (cap < 8) { if (cap) buf[0] = 0; return; }
    if (f->active) buf[n++] = 'A';
    if (!f->has_dispatch && !f->has_dispatch_jmp && !f->has_return) buf[n++] = 'N';
    if (f->has_dispatch && !f->has_return) buf[n++] = 'R';
    if (f->todo) buf[n++] = 'T';
    if (!f->ninstr) buf[n++] = 'E';
    if (f->ninstr && f->first_va != f->start) buf[n++] = 'S';
    if (f->ninstr && f->last_va < f->first_va) buf[n++] = 'I';
    buf[n] = 0;
}

static void print_group(const Group *g, size_t show) {
    printf("  %08X  occ=%zu funcs=%u%s%s\n", g->va, g->nocc, g->nfunc,
           g->conflict ? " CONFLICT" : " overlap",
           g->active ? " [active]" : "");
    uint32_t last = 0xFFFFFFFFu;
    size_t printed = 0;
    for (size_t k = g->first; k < g->first + g->nocc && printed < show; k++) {
        if (g_occ.v[k].func == last) continue;
        last = g_occ.v[k].func;
        const Func *f = &g_funcs.v[last];
        printf("      L_%08X  %s:%u  range=%08X-%08X  n=%llu  todo=%u\n",
               f->start, f->file, f->line_no, f->first_va, f->last_va,
               (unsigned long long)f->ninstr, f->todo);
        printed++;
    }
    if (g->nfunc > printed) printf("      ... %u more function(s)\n", g->nfunc - (uint32_t)printed);
}

static void print_func_list(const char *title, const IdxVec *v, size_t top,
                            int by_todo) {
    printf("\n%s: %zu\n", title, v->n);
    if (!v->n) return;
    IdxVec tmp = *v;
    size_t nprint = top ? top : tmp.n;
    if (nprint > tmp.n) nprint = tmp.n;
    for (size_t i = 0; i < nprint; i++) {
        const Func *f = &g_funcs.v[tmp.v[i]];
        char flags[16];
        flags_of(f, flags, sizeof(flags));
        printf("  L_%08X  %-24s:%u  range=%08X-%08X  n=%llu  todo=%u  [%s]\n",
               f->start, f->file, f->line_no, f->first_va, f->last_va,
               (unsigned long long)f->ninstr, f->todo, flags);
    }
    if (tmp.n > nprint) printf("  ... %zu more\n", tmp.n - nprint);
    (void)by_todo;
}

static int cmp_todo_idx(const void *pa, const void *pb) {
    uint32_t ia = *(const uint32_t *)pa, ib = *(const uint32_t *)pb;
    const Func *a = &g_funcs.v[ia], *b = &g_funcs.v[ib];
    if (a->todo != b->todo) return (a->todo < b->todo) ? 1 : -1;
    if (a->ninstr != b->ninstr) return (a->ninstr < b->ninstr) ? 1 : -1;
    return (a->start > b->start) - (a->start < b->start);
}

static int cmp_span_idx(const void *pa, const void *pb) {
    uint32_t ia = *(const uint32_t *)pa, ib = *(const uint32_t *)pb;
    const Func *a = &g_funcs.v[ia], *b = &g_funcs.v[ib];
    uint64_t sa = a->last_va >= a->first_va ? (uint64_t)a->last_va - a->first_va : 0;
    uint64_t sb = b->last_va >= b->first_va ? (uint64_t)b->last_va - b->first_va : 0;
    if (sa != sb) return (sa < sb) ? 1 : -1;
    return (a->start > b->start) - (a->start < b->start);
}

static int cmp_start_idx(const void *pa, const void *pb) {
    uint32_t ia = *(const uint32_t *)pa, ib = *(const uint32_t *)pb;
    return (g_funcs.v[ia].start > g_funcs.v[ib].start) -
           (g_funcs.v[ia].start < g_funcs.v[ib].start);
}

static void idx_add(IdxVec *v, uint32_t i) {
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 256;
        v->v = (uint32_t *)xrealloc(v->v, cap * sizeof(uint32_t));
        v->cap = cap;
    }
    v->v[v->n++] = i;
}

static void write_map(const char *path, const uint32_t *order) {
    FILE *f = fopen(path, "w");
    if (!f) die("cannot write map file");
    fprintf(f, "# lint map v1\n");
    fprintf(f, "addr\tfile\tline\tfirst\tlast\tninstr\ttodo\tactive\tflags\n");
    for (size_t i = 0; i < g_funcs.n; i++) {
        const Func *fn = &g_funcs.v[order[i]];
        char flags[16];
        flags_of(fn, flags, sizeof(flags));
        fprintf(f, "%08X\t%s\t%u\t%08X\t%08X\t%llu\t%u\t%d\t%s\n",
                fn->start, fn->file, fn->line_no, fn->first_va, fn->last_va,
                (unsigned long long)fn->ninstr, fn->todo, fn->active, flags);
    }
    fclose(f);
    printf("map: wrote %zu functions -> %s\n", g_funcs.n, path);
}

int main(int argc, char **argv) {
    const char *dir = "reports\\lift";
    const char *prefix = "war31_lifted";
    const char *active_path = NULL;
    const char *map_path = NULL;
    const char *file_arg = NULL;
    uint32_t around = 0;
    uint32_t radius = 0x1000;
    int have_around = 0;
    int strict = 0, strict_structure = 0, active_only = 0;
    size_t top = 30, show = 4;

    if (argc < 2) {
        fprintf(stderr,
                "usage: lint [options]\n"
                "  --dir DIR              catalog directory (default reports\\lift)\n"
                "  --prefix P             only scan files starting with P\n"
                "  --file PATH            scan one file instead of the directory\n"
                "  --active-list FILE     mark functions listed in a *_list.h\n"
                "  --active-only          report only groups touching active functions\n"
                "  --around VA            show functions/groups near VA\n"
                "  --radius N             radius for --around (default 0x1000)\n"
                "  --map FILE             write a TSV function-range map\n"
                "  --top N                report limit (default 30)\n"
                "  --show N               functions shown per overlap group (default 4)\n"
                "  --strict               exit 1 on conflicting instruction overlaps\n"
                "  --strict-structure     exit 1 on fall-through/empty functions\n");
        return 2;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) dir = argv[++i];
        else if (strcmp(argv[i], "--prefix") == 0 && i + 1 < argc) prefix = argv[++i];
        else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) file_arg = argv[++i];
        else if (strcmp(argv[i], "--active-list") == 0 && i + 1 < argc) active_path = argv[++i];
        else if (strcmp(argv[i], "--active-only") == 0) active_only = 1;
        else if (strcmp(argv[i], "--around") == 0 && i + 1 < argc) {
            if (!parse_u32_hex(argv[++i], &around)) die("bad --around address");
            have_around = 1;
        } else if (strcmp(argv[i], "--radius") == 0 && i + 1 < argc) {
            if (!parse_u32_hex(argv[++i], &radius)) die("bad --radius");
        } else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) map_path = argv[++i];
        else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) top = (size_t)strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--show") == 0 && i + 1 < argc) show = (size_t)strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--strict") == 0) strict = 1;
        else if (strcmp(argv[i], "--strict-structure") == 0) strict_structure = 1;
        else {
            fprintf(stderr, "lint: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    if (active_path) load_list(active_path, &g_active);

    if (file_arg) {
        scan_source(file_arg, file_arg);
    } else {
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
            if (prefix && !ends_with_ci(fd.cFileName, ".c")) continue;
            char path[MAX_PATH + 64];
            snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
            scan_source(path, fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    for (size_t i = 0; i < g_funcs.n; i++)
        if (g_active.n && addr_has(&g_active, g_funcs.v[i].start)) g_funcs.v[i].active = 1;

    build_groups();

    size_t nconflict = 0, noverlap = 0, nactive_groups = 0;
    for (size_t i = 0; i < g_groups.n; i++) {
        if (g_groups.v[i].conflict) nconflict++;
        else noverlap++;
        if (g_groups.v[i].active) nactive_groups++;
    }

    IdxVec no_term = {0}, empty = {0}, start_mismatch = {0}, todo = {0}, span = {0};
    size_t dispatch_no_ret = 0;
    for (uint32_t i = 0; i < g_funcs.n; i++) {
        const Func *f = &g_funcs.v[i];
        if (!f->has_dispatch && !f->has_dispatch_jmp && !f->has_return) idx_add(&no_term, i);
        if (!f->ninstr && !f->has_dispatch && !f->has_dispatch_jmp && !f->has_return)
            idx_add(&empty, i);
        if (f->ninstr && f->first_va != f->start) idx_add(&start_mismatch, i);
        if (f->todo) idx_add(&todo, i);
        if (f->ninstr) idx_add(&span, i);
        if (f->has_dispatch && !f->has_return) dispatch_no_ret++;
    }
    qsort(todo.v, todo.n, sizeof(uint32_t), cmp_todo_idx);
    qsort(span.v, span.n, sizeof(uint32_t), cmp_span_idx);
    qsort(no_term.v, no_term.n, sizeof(uint32_t), cmp_start_idx);

    uint32_t *order = NULL;
    if (map_path) {
        order = (uint32_t *)xrealloc(NULL, (g_funcs.n ? g_funcs.n : 1) * sizeof(uint32_t));
        for (size_t i = 0; i < g_funcs.n; i++) order[i] = (uint32_t)i;
        g_sort_funcs = g_funcs.v;
        qsort(order, g_funcs.n, sizeof(uint32_t), cmp_func_order);
    }

    printf("== lint ==\n");
    printf("sources=%s  files=%s\n", file_arg ? file_arg : dir,
           file_arg ? "1" : prefix ? prefix : "*");
    printf("functions=%zu  instructions=%zu  log-lines=%llu\n",
           g_funcs.n, g_occ.n, (unsigned long long)g_lines);
    if (g_active.n) printf("active list: %zu addresses\n", g_active.n);
    printf("instruction groups with >1 occurrence: %zu\n", g_groups.n);
    printf("  conflicting translations: %zu  consistent overlaps: %zu\n",
           nconflict, noverlap);
    if (g_active.n) printf("  groups touching active functions: %zu\n", nactive_groups);

    if (have_around) {
        printf("\n-- around %08X (+/- %X) --\n", around, radius);
        uint32_t lo = around > radius ? around - radius : 0;
        uint32_t hi = around + radius;
        size_t nf = 0;
        for (size_t i = 0; i < g_funcs.n; i++) {
            const Func *f = &g_funcs.v[i];
            uint32_t l = f->ninstr ? f->first_va : f->start;
            uint32_t h = f->ninstr ? f->last_va : f->start;
            if (h >= lo && l <= hi) {
                char flags[16];
                flags_of(f, flags, sizeof(flags));
                printf("  L_%08X  %s:%u  range=%08X-%08X  n=%llu  todo=%u  [%s]%s\n",
                       f->start, f->file, f->line_no, l, h,
                       (unsigned long long)f->ninstr, f->todo, flags,
                       (l <= around && around <= h) ? "  <== contains VA" : "");
                nf++;
            }
        }
        if (!nf) printf("  no lifted function intersects this range\n");
        size_t ng = 0, ng_total = 0;
        printf("  overlap groups in range:\n");
        for (size_t i = 0; i < g_groups.n; i++) {
            if (g_groups.v[i].va >= lo && g_groups.v[i].va <= hi) {
                if (active_only && !g_groups.v[i].active) continue;
                ng_total++;
                if (top && ng >= top) continue;
                print_group(&g_groups.v[i], show);
                ng++;
            }
        }
        if (!ng_total) printf("    none\n");
        else if (ng_total > ng)
            printf("    ... %zu more group(s) in range (use --top 0 for all)\n",
                   ng_total - ng);
    }

    printf("\nconflicting instruction overlaps (same VA, different text):\n");
    qsort(g_groups.v, g_groups.n, sizeof(Group), cmp_group_risk);
    size_t shown = 0;
    for (size_t i = 0; i < g_groups.n; i++) {
        if (!g_groups.v[i].conflict) continue;
        if (active_only && !g_groups.v[i].active) continue;
        if (top && shown >= top) break;
        print_group(&g_groups.v[i], show);
        shown++;
    }
    if (!nconflict) printf("  none\n");
    else if (top && nconflict > shown) printf("  ... %zu more conflict group(s)\n", nconflict - shown);

    printf("\nconsistent overlaps (same VA, same text, multiple functions):\n");
    shown = 0;
    for (size_t i = 0; i < g_groups.n; i++) {
        if (g_groups.v[i].conflict) continue;
        if (active_only && !g_groups.v[i].active) continue;
        if (top && shown >= top) break;
        print_group(&g_groups.v[i], show);
        shown++;
    }
    if (!noverlap) printf("  none\n");
    else if (top && noverlap > shown) printf("  ... %zu more overlap group(s)\n", noverlap - shown);

    if (have_around) {
        /* Keep the group order VA-sorted for callers that parse the output. */
        qsort(g_groups.v, g_groups.n, sizeof(Group), cmp_group_va);
    }

    print_func_list("functions without terminal dispatch/return", &no_term, top, 0);
    print_func_list("functions with no instruction at their start VA", &start_mismatch, top, 0);
    print_func_list("empty functions (no instruction comments)", &empty, top, 0);
    print_func_list("functions with RECOMP_TODO", &todo, top, 1);
    print_func_list("largest instruction spans", &span, top, 0);
    if (dispatch_no_ret)
        printf("\nfunctions calling dispatch() but never containing return;: %zu\n",
               dispatch_no_ret);

    printf("\nverdict:\n");
    if (nconflict)
        printf("  LINT RISK: %zu conflicting instruction overlap(s). Inspect before the next run.\n",
               nconflict);
    else
        printf("  overlaps: no conflicting translation found.\n");
    if (no_term.n || empty.n || start_mismatch.n)
        printf("  structure: %zu no-terminal, %zu empty, %zu start-mismatch.\n",
               no_term.n, empty.n, start_mismatch.n);
    else
        printf("  structure: no fall-through/empty/start-mismatch function.\n");

    if (map_path) write_map(map_path, order);

    free(order);
    free(no_term.v); free(empty.v); free(start_mismatch.v); free(todo.v); free(span.v);
    free(g_groups.v); free(g_occ.v); free(g_funcs.v); free(g_active.v);
    return (strict && nconflict) || (strict_structure && (no_term.n || empty.n || start_mismatch.n)) ? 1 : 0;
}
