/*
 * stack - post-mortem guest stack reconstruction.
 *
 * The host crash handler writes guest_stack_crash.bin plus a metadata line:
 *   [host31] guest_stack_dump=... base=... top=... size=...
 *
 * This tool walks the x86 EBP chain, resolves return addresses against the
 * lift map/list, scans the stack for code-looking dwords, and prints the last
 * trace ring entries.  It is deliberately read-only.
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
#define MAX_SYM 512

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

typedef struct {
    unsigned long long n;
    uint32_t target, esp, ebp;
} TraceEnt;

typedef struct {
    TraceEnt *v;
    size_t n, cap;
} TraceVec;

static void die(const char *msg) {
    fprintf(stderr, "stack: %s\n", msg);
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
        fprintf(stderr, "stack: cannot open list %s\n", path);
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
        unsigned start = 0, first = 0, last = 0, line_no = 0, todo = 0;
        int active = 0;
        unsigned long long ninstr = 0;
        char file[MAX_FILE] = {0};
        char flags[32] = {0};
        int n = sscanf(line, "%x\t%259[^\t]\t%u\t%x\t%x\t%llu\t%u\t%d\t%31s",
                       &start, file, &line_no, &first, &last, &ninstr,
                       &todo, &active, flags);
        if (n < 8) continue;
        r.start = (uint32_t)start;
        r.first = (uint32_t)first;
        r.last = (uint32_t)last;
        r.line = line_no;
        r.ninstr = ninstr;
        r.todo = todo;
        r.active = active;
        snprintf(r.file, sizeof(r.file), "%s", file);
        range_add(out, &r);
    }
    fclose(f);
}

static int has_code_image(const Pe *pe, const AddrVec *list, uint32_t va) {
    if (pe && pe->text && pe_in_text((Pe *)pe, va)) return 1;
    if (addr_has(list, va)) return 1;
    return 0;
}

static void symbol_for(const AddrVec *list, const RangeVec *ranges, uint32_t va,
                       char *buf, size_t cap) {
    if (addr_has(list, va)) {
        snprintf(buf, cap, "L_%08X", va);
        return;
    }
    for (size_t i = 0; i < ranges->n; i++) {
        const Range *r = &ranges->v[i];
        uint32_t l = r->ninstr ? r->first : r->start;
        uint32_t h = r->ninstr ? r->last : r->start;
        if (va >= l && va <= h) {
            if (va == r->start)
                snprintf(buf, cap, "L_%08X (start) %s:%u", r->start, r->file, r->line);
            else
                snprintf(buf, cap, "L_%08X+0x%X %s:%u", r->start, va - r->start,
                         r->file, r->line);
            return;
        }
    }
    snprintf(buf, cap, "<no lift>");
}

static uint32_t rd_stack(const uint8_t *mem, uint32_t base, uint32_t size,
                         uint32_t va) {
    if (va < base || va - base > size || size < 4 || va - base + 4 > size)
        return 0;
    const uint8_t *p = mem + (va - base);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void trace_add(TraceVec *t, unsigned long long n, uint32_t target,
                      uint32_t esp, uint32_t ebp) {
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 512;
        t->v = (TraceEnt *)xrealloc(t->v, cap * sizeof(TraceEnt));
        t->cap = cap;
    }
    t->v[t->n].n = n;
    t->v[t->n].target = target;
    t->v[t->n].esp = esp;
    t->v[t->n].ebp = ebp;
    t->n++;
}

typedef struct {
    uint32_t code, address, read;
    uint32_t eip, esp, ebp;
    uint32_t eax, ecx, edx, ebx, esi, edi;
    int have_crash;
    int have_regs;
    uint32_t dump_base, dump_top, dump_size;
    char dump_path[MAX_PATH];
    int have_dump_meta;
    TraceVec trace;
    char last_hle[1024];
} LogInfo;

static void parse_log(const char *path, LogInfo *li) {
    FILE *f = fopen(path, "r");
    if (!f) die("cannot open log");
    char line[MAX_LINE];
    int in_chain = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "guest call chain crash")) in_chain = 1;
        else if (in_chain && strncmp(line, "[host31]   #", 12) != 0) in_chain = 0;

        if (strstr(line, "[host31] CRASH")) {
            unsigned code, a1, a2, eip, esp, ebp;
            unsigned long long addr = 0, read = 0;
            int n = sscanf(line,
                           "[host31] CRASH code=%x address=%llx read=%llx guest_eip=%x esp=%x ebp=%x",
                           &code, &addr, &read, &eip, &esp, &ebp);
            if (n == 6) {
                li->code = code;
                li->address = (uint32_t)addr;
                li->read = (uint32_t)read;
                li->eip = eip;
                li->esp = esp;
                li->ebp = ebp;
                li->have_crash = 1;
            } else {
                /* A read value can be printed with a different width; retry. */
                n = sscanf(line,
                           "[host31] CRASH code=%x address=%x read=%x guest_eip=%x esp=%x ebp=%x",
                           &code, &a1, &a2, &eip, &esp, &ebp);
                if (n == 6) {
                    li->code = code; li->address = a1; li->read = a2;
                    li->eip = eip; li->esp = esp; li->ebp = ebp;
                    li->have_crash = 1;
                }
            }
        } else if (strstr(line, "[host31] regs ")) {
            unsigned eax, ecx, edx, ebx, esi, edi;
            if (sscanf(line, "[host31] regs eax=%x ecx=%x edx=%x ebx=%x esi=%x edi=%x",
                       &eax, &ecx, &edx, &ebx, &esi, &edi) == 6) {
                li->eax = eax; li->ecx = ecx; li->edx = edx;
                li->ebx = ebx; li->esi = esi; li->edi = edi;
                li->have_regs = 1;
            }
        } else if (strstr(line, "guest_stack_dump=")) {
            char *p = strstr(line, "guest_stack_dump=");
            p += strlen("guest_stack_dump=");
            char *base = strstr(p, " base=");
            char *top = strstr(p, " top=");
            char *size = strstr(p, " size=");
            if (base && top && size) {
                size_t n = (size_t)(base - p);
                if (n >= sizeof(li->dump_path)) n = sizeof(li->dump_path) - 1;
                memcpy(li->dump_path, p, n);
                li->dump_path[n] = 0;
                if (sscanf(base, " base=%x top=%x size=%x",
                           &li->dump_base, &li->dump_top, &li->dump_size) == 3)
                    li->have_dump_meta = 1;
            }
        } else if (in_chain) {
            unsigned long long n;
            unsigned target, esp, ebp;
            const char *p = line;
            const char *hash = strchr(p, '#');
            if (hash && sscanf(hash, "#%llu %x esp=%x ebp=%x", &n, &target, &esp, &ebp) == 4)
                trace_add(&li->trace, n, target, esp, ebp);
            else if (hash && sscanf(hash, "#%llu target=%x esp=%x ebp=%x", &n, &target, &esp, &ebp) == 4)
                trace_add(&li->trace, n, target, esp, ebp);
        }
        if (strstr(line, "[hle]") || strstr(line, "[hle31]")) {
            snprintf(li->last_hle, sizeof(li->last_hle), "%.*s",
                     (int)sizeof(li->last_hle) - 1, line);
            size_t n = strlen(li->last_hle);
            while (n && (li->last_hle[n - 1] == '\n' || li->last_hle[n - 1] == '\r'))
                li->last_hle[--n] = 0;
        }
    }
    fclose(f);
}

static uint8_t *read_file(const char *path, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    uint8_t *p = (uint8_t *)malloc((size_t)n);
    if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p); fclose(f); return NULL;
    }
    fclose(f);
    *size_out = (size_t)n;
    return p;
}

static void join_path(char *out, size_t cap, const char *dir, const char *name) {
    if (!dir || !*dir) {
        snprintf(out, cap, "%s", name);
        return;
    }
    size_t n = strlen(dir);
    if (n && (dir[n - 1] == '\\' || dir[n - 1] == '/'))
        snprintf(out, cap, "%s%s", dir, name);
    else
        snprintf(out, cap, "%s\\%s", dir, name);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: stack <log> [options]\n"
                "  --dump FILE        guest stack dump (default: metadata or log-dir guest_stack_crash.bin)\n"
                "  --base HEX         dump base override\n"
                "  --top HEX          dump top override\n"
                "  --list FILE        active *_list.h\n"
                "  --map FILE         TSV map from lint --map\n"
                "  --lift-dir DIR     scan catalogs if --map is absent\n"
                "  --prefix P         catalog prefix (default war31_lifted)\n"
                "  --image PATH       PE image for code-range detection\n"
                "  --frames N         EBP chain limit (default 32)\n"
                "  --scan N           stack dwords to scan (default 256)\n"
                "  --trace N          trace entries to print (default 24)\n");
        return 2;
    }

    const char *log_path = argv[1];
    const char *dump_path = NULL;
    const char *list_path = NULL;
    const char *map_path = NULL;
    const char *lift_dir = NULL;
    const char *prefix = "war31_lifted";
    const char *image = NULL;
    uint32_t base_override = 0, top_override = 0;
    int have_base = 0, have_top = 0;
    size_t frames = 32, scan_n = 256, trace_n = 24;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) dump_path = argv[++i];
        else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &base_override)) die("bad --base");
            have_base = 1;
        } else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &top_override)) die("bad --top");
            have_top = 1;
        } else if (strcmp(argv[i], "--list") == 0 && i + 1 < argc) list_path = argv[++i];
        else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) map_path = argv[++i];
        else if (strcmp(argv[i], "--lift-dir") == 0 && i + 1 < argc) lift_dir = argv[++i];
        else if (strcmp(argv[i], "--prefix") == 0 && i + 1 < argc) prefix = argv[++i];
        else if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) image = argv[++i];
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = (size_t)strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--scan") == 0 && i + 1 < argc) scan_n = (size_t)strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) trace_n = (size_t)strtoul(argv[++i], NULL, 0);
        else {
            fprintf(stderr, "stack: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    LogInfo li;
    memset(&li, 0, sizeof(li));
    parse_log(log_path, &li);
    printf("== stack ==\n");
    printf("log=%s\n", log_path);
    if (li.have_crash) {
        printf("crash code=%08X address=%08X read=%08X guest_eip=%08X esp=%08X ebp=%08X\n",
               li.code, li.address, li.read, li.eip, li.esp, li.ebp);
    } else {
        printf("crash: no CRASH line found\n");
    }
    if (li.have_regs) {
        printf("regs eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X\n",
               li.eax, li.ecx, li.edx, li.ebx, li.esi, li.edi);
    }

    /* Resolve the dump path relative to the log if the metadata is relative. */
    char auto_dump[MAX_PATH] = {0};
    if (!dump_path) {
        if (li.dump_path[0]) {
            dump_path = li.dump_path;
            if (!strchr(dump_path, '\\') && !strchr(dump_path, '/')) {
                char dir[MAX_PATH];
                snprintf(dir, sizeof(dir), "%s", log_path);
                char *sl = strrchr(dir, '\\');
                char *sl2 = strrchr(dir, '/');
                if (sl2 > sl) sl = sl2;
                if (sl) { *sl = 0; join_path(auto_dump, sizeof(auto_dump), dir, dump_path); dump_path = auto_dump; }
            }
        } else {
            char dir[MAX_PATH];
            snprintf(dir, sizeof(dir), "%s", log_path);
            char *sl = strrchr(dir, '\\');
            char *sl2 = strrchr(dir, '/');
            if (sl2 > sl) sl = sl2;
            if (sl) *sl = 0;
            join_path(auto_dump, sizeof(auto_dump), dir, "guest_stack_crash.bin");
            dump_path = auto_dump;
        }
    }

    Pe pe;
    Pe *pep = NULL;
    if (image) {
        if (pe_open(&pe, image)) pep = &pe;
        else fprintf(stderr, "stack: cannot open image %s\n", image);
    }
    AddrVec list = {0};
    if (list_path) load_list(list_path, &list);
    RangeVec ranges = {0};
    if (map_path) load_map(map_path, &ranges);
    else if (lift_dir) load_catalogs(lift_dir, prefix, &ranges);
    if (ranges.n) qsort(ranges.v, ranges.n, sizeof(Range), cmp_range);
    if (li.have_crash) {
        char sym[MAX_SYM];
        symbol_for(&list, &ranges, li.eip, sym, sizeof(sym));
        printf("guest_eip symbol: %s\n", sym);
    }

    size_t dump_size = 0;
    uint8_t *mem = dump_path ? read_file(dump_path, &dump_size) : NULL;
    uint32_t base = have_base ? base_override : li.dump_base;
    uint32_t top = have_top ? top_override : li.dump_top;
    if (mem && !li.have_dump_meta && have_base && !have_top)
        top = base + (uint32_t)dump_size;
    if (mem && have_base && have_top) {
        /* both overridden */
    } else if (mem && have_top && !have_base) {
        base = top > (uint32_t)dump_size ? top - (uint32_t)dump_size : 0;
    }
    if (mem && (!top || top < base) && base)
        top = base + (uint32_t)dump_size;

    if (mem) {
        printf("dump=%s base=%08X top=%08X bytes=%zu\n", dump_path, base, top, dump_size);
    } else {
        printf("dump=%s  (not found; rebuild the host with WC31_CRASH_STACK=1)\n",
               dump_path ? dump_path : "<none>");
    }

    uint32_t chain_ebp = li.ebp;
    size_t chain_frames = 0;
    if (mem && base && top > base && chain_ebp >= base && chain_ebp + 8 <= top) {
        printf("\nEBP chain:\n");
        uint32_t prev = 0;
        for (size_t i = 0; i < frames; i++) {
            if (chain_ebp < base || chain_ebp + 8 > top || (chain_ebp & 3)) break;
            uint32_t next = rd_stack(mem, base, (uint32_t)dump_size, chain_ebp);
            uint32_t ret = rd_stack(mem, base, (uint32_t)dump_size, chain_ebp + 4);
            char sym[MAX_SYM];
            symbol_for(&list, &ranges, ret, sym, sizeof(sym));
            printf("  #%02zu ebp=%08X ret=%08X  %s\n", i, chain_ebp, ret, sym);
            printf("      args: %08X %08X %08X %08X\n",
                   rd_stack(mem, base, (uint32_t)dump_size, chain_ebp + 8),
                   rd_stack(mem, base, (uint32_t)dump_size, chain_ebp + 12),
                   rd_stack(mem, base, (uint32_t)dump_size, chain_ebp + 16),
                   rd_stack(mem, base, (uint32_t)dump_size, chain_ebp + 20));
            chain_frames++;
            if (next <= chain_ebp || next >= top || (next & 3) || next == prev) {
                printf("      chain stops: saved_ebp=%08X\n", next);
                break;
            }
            prev = chain_ebp;
            chain_ebp = next;
        }
        if (!chain_frames) printf("  no usable frame at ebp=%08X\n", li.ebp);
    } else if (li.have_crash) {
        printf("\nEBP chain: unavailable (no dump or ebp outside dump)\n");
    }

    if (mem && base && li.have_crash) {
        uint32_t start = li.esp > base ? li.esp : base;
        uint32_t limit = top > base ? top : base + (uint32_t)dump_size;
        uint32_t end = start + (uint32_t)scan_n * 4;
        if (end > limit) end = limit;
        printf("\nstack scan (code-looking dwords from esp=%08X):\n", start);
        size_t found = 0;
        for (uint32_t va = start; va + 4 <= end; va += 4) {
            uint32_t v = rd_stack(mem, base, (uint32_t)dump_size, va);
            if (!has_code_image(pep, &list, v)) continue;
            char sym[MAX_SYM];
            symbol_for(&list, &ranges, v, sym, sizeof(sym));
            printf("  [%08X] = %08X  %s\n", va, v, sym);
            found++;
        }
        if (!found) printf("  none\n");
    }

    if (li.trace.n) {
        printf("\ntrace ring: %zu entries\n", li.trace.n);
        size_t start = li.trace.n > trace_n ? li.trace.n - trace_n : 0;
        for (size_t i = start; i < li.trace.n; i++) {
            char sym[MAX_SYM];
            symbol_for(&list, &ranges, li.trace.v[i].target, sym, sizeof(sym));
            printf("  #%llu %08X esp=%08X ebp=%08X  %s\n",
                   li.trace.v[i].n, li.trace.v[i].target, li.trace.v[i].esp,
                   li.trace.v[i].ebp, sym);
        }
    }
    if (li.last_hle[0]) printf("\nlast HLE: %s\n", li.last_hle);

    printf("\nverdict:\n");
    printf("  stack_dump=%s  ebp_frames=%zu  trace=%zu\n",
           mem ? "yes" : "no", chain_frames, li.trace.n);
    if (!mem) printf("  add WC31_CRASH_STACK=1 (default) and rebuild the host to get a full stack dump.\n");

    free(mem);
    free(li.trace.v);
    free(list.v);
    free(ranges.v);
    if (pep) pe_close(pep);
    return 0;
}
