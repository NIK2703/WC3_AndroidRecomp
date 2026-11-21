/*
 * find_loops.c - find loop tops and parallelization candidates in lifted C.
 *
 * The lifted WC3 corpus is split into many L_* functions and across many .c
 * files.  One guest instruction is emitted per line, with its guest address in
 * a comment.  A backward direct jump is a loop when both ends belong to the
 * same original guest function; otherwise it is a tail call.  Both generated
 * dispatch_jmp(c, 0x...) and goto L_<addr> edges are collected: the latter is
 * how most intra-function loops are emitted.
 *
 * The scanner builds an approximate original-function map from:
 *   - direct call targets: push32(c, ret); dispatch(c, 0x...);
 *   - function prologues: push ebp / push esi / mov edi, edi / sub esp, ...;
 *   - an optional functions.txt seed list (address + size + name).
 * Seed ranges take precedence: a prologue or call target inside a known seed
 * function cannot split that function into a false region.
 *
 * For a backward edge, the enclosing original function is the nearest start
 * at or below the address.  Edges crossing a function start are tail calls and
 * are dropped.  Remaining edges are grouped by target address, which is the
 * loop top.  The body is the address range [top, furthest back edge] restricted
 * to the same original function.  Per-body counters (guest writes, calls,
 * atomics, stack traffic) feed a static parallelization score; an optional
 * simpleperf flat report adds measured self samples, clipped to the same
 * range.  The score is a triage filter, not proof that iterations are
 * independent.
 *
 * Build:
 *   ccache gcc -std=c11 -O2 -pthread tools/find_loops.c -o build/host/find_loops -lm
 *
 * Usage:
 *   find_loops [-v] [-f functions.txt] [-p flat_report.txt] \
 *              [-o loops.tsv] [-e edges.tsv] <dir|file>...
 *
 * With no path it scans build/android_app/debug/cmake/relifted/game and, if
 * present, loads build/corpus-full/game/functions.txt automatically.
 */
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PATH_CAP 192
#define NAME_CAP 24
#define INSN_CAP 64

typedef struct {
    uint32_t base;
    uint32_t instr_count;
    uint32_t instr_off; /* first instruction in the global instruction stream */
    uint32_t start_line;
    uint32_t end_line;
    uint32_t calls;
    uint32_t writes;
    uint32_t reads;
    uint32_t atomics;
    uint32_t jumps;
    uint32_t stackops;
    uint8_t prologue;
    double self_pct;
    char name[NAME_CAP];
    char file[PATH_CAP];
} FuncRec;

typedef struct {
    uint32_t base;
    uint32_t end;
} SeedRange;

typedef struct {
    char name[NAME_CAP];
    double pct;
} ProfileRec;

typedef struct {
    uint32_t src;
    uint32_t target;
    uint32_t current;
    uint32_t line;
} EdgeRec;

typedef struct {
    uint32_t target;       /* loop top */
    uint32_t max_current;  /* furthest backward edge */
    uint32_t func_start;   /* enclosing original guest function */
    uint32_t header_func;  /* L_* function containing the top */
    uint32_t header_off;   /* top - header_func base */
    uint32_t span;         /* max_current - target */
    uint32_t edges;
    uint32_t body_funcs;
    uint64_t body_instr;
    uint32_t writes;
    uint32_t calls;
    uint32_t atomics;
    uint32_t reads;
    uint32_t jumps;
    uint32_t stackops;
    uint32_t depth;
    uint32_t exact;
    uint32_t src;
    uint32_t src_line;
    uint8_t parallel_class; /* 0=A pure, 1=B low-effect, 2=C candidate, 3=D serial */
    double hot_pct;         /* sum of self samples in the loop body, percent */
    double score;
} LoopRec;

static FuncRec *g_funcs;
static size_t g_nfuncs;
static size_t g_capfuncs;

static EdgeRec *g_edges;
static size_t g_nedges;
static size_t g_capedges;

static LoopRec *g_loops;
static size_t g_nloops;
static size_t g_caploops;

static uint32_t *g_starts;
static size_t g_nstarts;
static size_t g_capstarts;

static SeedRange *g_ranges;
static size_t g_nranges;
static size_t g_capranges;

/* Per-instruction stream, used to clip loop body stats to [top, bottom]. */
enum {
    IF_WRITE = 1u << 0,
    IF_READ = 1u << 1,
    IF_ATOMIC = 1u << 2,
    IF_STACK = 1u << 3,
    IF_JUMP = 1u << 4,
    IF_CALL = 1u << 5,
};
static uint32_t *g_iaddr;
static uint8_t *g_iflags;
static size_t g_ninstr;
static size_t g_capinstr;

static ProfileRec *g_prof;
static size_t g_nprof;
static size_t g_cappprof;
static int g_profile_loaded;
static unsigned long g_profile_matches;
static const char *g_profile_file;

static unsigned long g_files;
static unsigned long g_duplicate_bases;
static unsigned long g_seed_starts;
static const char *g_seed_file;

static inline int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint32_t parse_hex(const char *s)
{
    uint32_t v = 0;
    while (*s) {
        int h = hexval((unsigned char)*s);
        if (h < 0) break;
        v = (v << 4) | (uint32_t)h;
        ++s;
    }
    return v;
}

static void ensure_funcs(size_t need)
{
    if (need <= g_capfuncs) return;
    size_t cap = g_capfuncs ? g_capfuncs : 8192;
    while (cap < need) cap *= 2;
    FuncRec *p = (FuncRec *)realloc(g_funcs, cap * sizeof(*p));
    if (!p) {
        fprintf(stderr, "find_loops: out of memory (functions)\n");
        exit(1);
    }
    g_funcs = p;
    g_capfuncs = cap;
}

static void push_instr(uint32_t addr, uint8_t flags)
{
    if (g_ninstr == g_capinstr) {
        size_t cap = g_capinstr ? g_capinstr * 2 : 65536;
        uint32_t *a = (uint32_t *)realloc(g_iaddr, cap * sizeof(*a));
        uint8_t *f = (uint8_t *)realloc(g_iflags, cap * sizeof(*f));
        if (!a || !f) {
            fprintf(stderr, "find_loops: out of memory (instructions)\n");
            exit(1);
        }
        g_iaddr = a;
        g_iflags = f;
        g_capinstr = cap;
    }
    g_iaddr[g_ninstr] = addr;
    g_iflags[g_ninstr] = flags;
    ++g_ninstr;
}

static void push_edge(uint32_t src, uint32_t target, uint32_t current,
                      uint32_t line)
{
    if (g_nedges == g_capedges) {
        size_t cap = g_capedges ? g_capedges * 2 : 16384;
        EdgeRec *p = (EdgeRec *)realloc(g_edges, cap * sizeof(*p));
        if (!p) {
            fprintf(stderr, "find_loops: out of memory (edges)\n");
            exit(1);
        }
        g_edges = p;
        g_capedges = cap;
    }
    g_edges[g_nedges].src = src;
    g_edges[g_nedges].target = target;
    g_edges[g_nedges].current = current;
    g_edges[g_nedges].line = line;
    ++g_nedges;
}

static void push_loop(const LoopRec *r)
{
    if (g_nloops == g_caploops) {
        size_t cap = g_caploops ? g_caploops * 2 : 4096;
        LoopRec *p = (LoopRec *)realloc(g_loops, cap * sizeof(*p));
        if (!p) {
            fprintf(stderr, "find_loops: out of memory (loops)\n");
            exit(1);
        }
        g_loops = p;
        g_caploops = cap;
    }
    g_loops[g_nloops++] = *r;
}

static void push_start(uint32_t addr)
{
    if (!addr) return;
    if (g_nstarts == g_capstarts) {
        size_t cap = g_capstarts ? g_capstarts * 2 : 16384;
        uint32_t *p = (uint32_t *)realloc(g_starts, cap * sizeof(*p));
        if (!p) {
            fprintf(stderr, "find_loops: out of memory (starts)\n");
            exit(1);
        }
        g_starts = p;
        g_capstarts = cap;
    }
    g_starts[g_nstarts++] = addr;
}

static void push_range(uint32_t base, uint32_t end)
{
    if (!base || end <= base) return;
    if (g_nranges == g_capranges) {
        size_t cap = g_capranges ? g_capranges * 2 : 16384;
        SeedRange *p = (SeedRange *)realloc(g_ranges, cap * sizeof(*p));
        if (!p) {
            fprintf(stderr, "find_loops: out of memory (ranges)\n");
            exit(1);
        }
        g_ranges = p;
        g_capranges = cap;
    }
    g_ranges[g_nranges].base = base;
    g_ranges[g_nranges].end = end;
    ++g_nranges;
}

/* First comment address on the line, or 0. */
static uint32_t line_addr(const char *s)
{
    const char *p = s;
    while ((p = strstr(p, "/* ")) != NULL) {
        const char *h = p + 3;
        int n = 0;
        while (n < 8 && hexval((unsigned char)h[n]) >= 0) ++n;
        if (n == 8 && h[8] == ':')
            return parse_hex(h);
        p += 3;
    }
    return 0;
}

/* Instruction text after the address comment, e.g. "push ebp". */
static int line_insn(const char *s, char *out, size_t cap)
{
    const char *p = s;
    while ((p = strstr(p, "/* ")) != NULL) {
        const char *h = p + 3;
        int n = 0;
        while (n < 8 && hexval((unsigned char)h[n]) >= 0) ++n;
        if (n == 8 && h[8] == ':') {
            const char *t = h + 9;
            while (*t == ' ' || *t == '\t') ++t;
            const char *end = strstr(t, " */");
            size_t len = end ? (size_t)(end - t) : strlen(t);
            if (len >= cap) len = cap - 1;
            memcpy(out, t, len);
            out[len] = '\0';
            while (len > 0 &&
                   (out[len - 1] == ' ' || out[len - 1] == '\t' ||
                    out[len - 1] == '\n' || out[len - 1] == '\r'))
                out[--len] = '\0';
            return 1;
        }
        p += 3;
    }
    return 0;
}

/* Only a direct dispatch(c, 0x...) call target. */
static uint32_t direct_call_target(const char *s)
{
    const char *p = strstr(s, "dispatch(c,");
    if (!p) return 0;
    p += strlen("dispatch(c,");
    while (*p == ' ' || *p == '\t') ++p;
    if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X')) return 0;
    return parse_hex(p + 2);
}

/* Only a direct dispatch_jmp(c, 0x...) target. */
static uint32_t direct_jmp_target(const char *s)
{
    const char *p = strstr(s, "dispatch_jmp");
    if (!p) return 0;
    p += strlen("dispatch_jmp");
    p = strchr(p, '(');
    if (!p) return 0;
    p = strchr(p, ',');
    if (!p) return 0;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X')) return 0;
    return parse_hex(p + 2);
}

/* Generated C uses goto L_<guest address> for intra-block branches. */
static uint32_t direct_goto_target(const char *s)
{
    const char *p = strstr(s, "goto L_");
    if (!p) return 0;
    p += strlen("goto L_");
    return parse_hex(p);
}

static int looks_like_prologue(const char *insn)
{
    if (strncmp(insn, "push ebp", 8) == 0) return 1;
    if (strncmp(insn, "push esi", 8) == 0) return 1;
    if (strncmp(insn, "push edi", 8) == 0) return 1;
    if (strncmp(insn, "push ebx", 8) == 0) return 1;
    if (strncmp(insn, "mov edi, edi", 12) == 0) return 1;
    if (strncmp(insn, "sub esp,", 8) == 0) return 1;
    if (strncmp(insn, "push -1", 7) == 0) return 1;
    if (strncmp(insn, "push 0xffffffff", 15) == 0) return 1;
    if (strncmp(insn, "mov eax, dword ptr fs:", 22) == 0) return 1;
    return 0;
}

static int has_mem_write(const char *s)
{
    if (!strstr(s, "wr8(") && !strstr(s, "wr16(") && !strstr(s, "wr32("))
        return 0;
    if (strstr(s, "c->ebp") || strstr(s, "c->esp")) return 0;
    return 1;
}

static int has_mem_read(const char *s)
{
    if (!strstr(s, "rd8(") && !strstr(s, "rd16(") && !strstr(s, "rd32("))
        return 0;
    if (strstr(s, "c->ebp") || strstr(s, "c->esp")) return 0;
    return 1;
}

static int has_stack_op(const char *s)
{
    return strstr(s, "push32(") || strstr(s, "pop32(") ||
           strstr(s, "wr32((c->ebp") || strstr(s, "wr32((c->esp") ||
           strstr(s, "wr16((c->ebp") || strstr(s, "wr16((c->esp") ||
           strstr(s, "wr8((c->ebp") || strstr(s, "wr8((c->esp");
}

static int parse_function_start(const char *line, char *name, size_t name_cap,
                                uint32_t *base)
{
    if (line[0] != 'v' || strncmp(line, "void L_", 7) != 0)
        return 0;

    const char *paren = strchr(line, '(');
    if (!paren) return 0;
    if (strchr(paren, ';')) return 0;
    if (!strstr(line, "CPU *restrict c")) return 0;

    size_t nlen = (size_t)(paren - (line + 5));
    if (nlen < 3 || nlen >= name_cap) return 0;
    memcpy(name, line + 5, nlen);
    name[nlen] = '\0';
    if (name[0] != 'L' || name[1] != '_') return 0;

    *base = parse_hex(name + 2);
    return *base != 0;
}

static void scan_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    ++g_files;

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    uint32_t lineno = 0;
    int in_func = 0;
    size_t cur = 0;

    while ((n = getline(&line, &cap, f)) != -1) {
        ++lineno;
        (void)n;

        if (!in_func) {
            char name[NAME_CAP];
            uint32_t base;
            if (!parse_function_start(line, name, sizeof(name), &base))
                continue;

            ensure_funcs(g_nfuncs + 1);
            cur = g_nfuncs++;
            FuncRec *r = &g_funcs[cur];
            memset(r, 0, sizeof(*r));
            r->base = base;
            r->start_line = lineno;
            snprintf(r->name, sizeof(r->name), "%s", name);
            snprintf(r->file, sizeof(r->file), "%s", path);
            in_func = 1;
            continue;
        }

        if (line[0] == '}') {
            g_funcs[cur].end_line = lineno;
            in_func = 0;
            continue;
        }

        uint32_t addr = line_addr(line);
        if (!addr) continue;

        FuncRec *r = &g_funcs[cur];
        if (r->instr_count == 0) {
            r->instr_off = (uint32_t)g_ninstr;
            char insn[INSN_CAP];
            if (line_insn(line, insn, sizeof(insn)) &&
                looks_like_prologue(insn)) {
                r->prologue = 1;
                push_start(r->base);
            }
        }
        ++r->instr_count;

        uint8_t flags = 0;
        if (has_mem_write(line)) {
            ++r->writes;
            flags |= IF_WRITE;
        }
        if (has_mem_read(line)) {
            ++r->reads;
            flags |= IF_READ;
        }
        if (strstr(line, "atomic_")) {
            ++r->atomics;
            flags |= IF_ATOMIC;
        }
        if (has_stack_op(line)) {
            ++r->stackops;
            flags |= IF_STACK;
        }
        if (strstr(line, "dispatch_jmp") || strstr(line, "goto L_")) {
            ++r->jumps;
            flags |= IF_JUMP;
        }
        if (strstr(line, "dispatch(c,")) {
            ++r->calls;
            flags |= IF_CALL;
        }

        uint32_t call = direct_call_target(line);
        if (call) push_start(call);

        uint32_t target = direct_jmp_target(line);
        if (!target) target = direct_goto_target(line);
        if (target && target < addr)
            push_edge((uint32_t)cur, target, addr, lineno);

        push_instr(addr, flags);
    }

    free(line);
    fclose(f);
}

static void scan_path(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISLNK(st.st_mode)) return;

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char child[1024];
            int len = snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            if (len <= 0 || (size_t)len >= sizeof(child)) continue;
            scan_path(child);
        }
        closedir(d);
        return;
    }

    if (!S_ISREG(st.st_mode)) return;
    const char *dot = strrchr(path, '.');
    if (dot && strcmp(dot, ".c") == 0)
        scan_file(path);
}

static int cmp_range(const void *a, const void *b)
{
    const SeedRange *x = (const SeedRange *)a;
    const SeedRange *y = (const SeedRange *)b;
    if (x->base != y->base) return (x->base > y->base) - (x->base < y->base);
    return (x->end > y->end) - (x->end < y->end);
}

static void load_start_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "find_loops: cannot read %s\n", path);
        return;
    }
    g_seed_file = path;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) != -1) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X')) continue;
        uint32_t addr = parse_hex(p + 2);
        if (!addr) continue;
        push_start(addr);
        ++g_seed_starts;

        const char *q = p + 2;
        while (hexval((unsigned char)*q) >= 0) ++q;
        while (*q == ' ' || *q == '\t') ++q;
        char *end = NULL;
        unsigned long size = strtoul(q, &end, 10);
        if (end != q && size > 0) {
            uint64_t e = (uint64_t)addr + (uint64_t)size;
            if (e > 0xFFFFFFFFu) e = 0xFFFFFFFFu;
            push_range(addr, (uint32_t)e);
        }
    }
    free(line);
    fclose(f);

    if (g_nranges > 1) {
        qsort(g_ranges, g_nranges, sizeof(*g_ranges), cmp_range);
        size_t w = 1;
        for (size_t i = 1; i < g_nranges; ++i) {
            SeedRange *cur = &g_ranges[w - 1];
            const SeedRange *r = &g_ranges[i];
            if (r->base < cur->end) {
                if (r->end > cur->end) cur->end = r->end;
            } else {
                g_ranges[w++] = *r;
            }
        }
        g_nranges = w;
    }
}

static void push_profile(const char *name, size_t len, double pct)
{
    if (len == 0 || len >= NAME_CAP) return;
    if (g_nprof == g_cappprof) {
        size_t cap = g_cappprof ? g_cappprof * 2 : 512;
        ProfileRec *p = (ProfileRec *)realloc(g_prof, cap * sizeof(*p));
        if (!p) {
            fprintf(stderr, "find_loops: out of memory (profile)\n");
            exit(1);
        }
        g_prof = p;
        g_cappprof = cap;
    }
    memcpy(g_prof[g_nprof].name, name, len);
    g_prof[g_nprof].name[len] = '\0';
    g_prof[g_nprof].pct = pct;
    ++g_nprof;
}

/*
 * simpleperf flat report: one line per symbol, first token is percent.
 * We use self samples, not inclusive children, to avoid double counting the
 * call tree.  Non-L_* symbols are ignored.
 */
static void load_profile(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "find_loops: cannot read %s\n", path);
        return;
    }
    g_profile_loaded = 1;
    g_profile_file = path;

    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) != -1) {
        char *end = NULL;
        double pct = strtod(line, &end);
        if (end == line) continue;

        const char *p = line;
        const char *name = NULL;
        while ((p = strstr(p, "L_")) != NULL) {
            name = p;
            p += 2;
        }
        if (!name) continue;

        size_t len = 0;
        while (name[len] && !isspace((unsigned char)name[len]) &&
               name[len] != '(')
            ++len;
        push_profile(name, len, pct);
    }
    free(line);
    fclose(f);
}

static int cmp_profile_name(const void *a, const void *b)
{
    const ProfileRec *x = (const ProfileRec *)a;
    const ProfileRec *y = (const ProfileRec *)b;
    return strcmp(x->name, y->name);
}

static double profile_lookup(const char *name)
{
    if (!g_profile_loaded || g_nprof == 0) return 0.0;
    ProfileRec key;
    memset(&key, 0, sizeof(key));
    snprintf(key.name, sizeof(key.name), "%s", name);
    ProfileRec *r = (ProfileRec *)bsearch(&key, g_prof, g_nprof,
                                         sizeof(*g_prof), cmp_profile_name);
    return r ? r->pct : 0.0;
}

static void apply_profile(void)
{
    if (!g_profile_loaded) return;
    qsort(g_prof, g_nprof, sizeof(*g_prof), cmp_profile_name);
    for (size_t i = 0; i < g_nfuncs; ++i) {
        double pct = profile_lookup(g_funcs[i].name);
        g_funcs[i].self_pct = pct;
        if (pct > 0.0) ++g_profile_matches;
    }
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static size_t lower_bound_u32(const uint32_t *a, size_t n, uint32_t v)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a[mid] < v) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static size_t upper_bound_u32(const uint32_t *a, size_t n, uint32_t v)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a[mid] <= v) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static uint32_t enclosing_start(uint32_t addr)
{
    if (g_nranges) {
        size_t lo = 0, hi = g_nranges;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (g_ranges[mid].base <= addr)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo > 0) {
            const SeedRange *r = &g_ranges[lo - 1];
            if (addr < r->end) return r->base;
        }
    }
    size_t i = upper_bound_u32(g_starts, g_nstarts, addr);
    return i ? g_starts[i - 1] : 0;
}

static int cmp_func_order(const void *a, const void *b)
{
    uint32_t ia = *(const uint32_t *)a;
    uint32_t ib = *(const uint32_t *)b;
    uint32_t x = g_funcs[ia].base;
    uint32_t y = g_funcs[ib].base;
    if (x != y) return (x > y) - (x < y);
    return (ia > ib) - (ia < ib);
}

static int cmp_edge(const void *a, const void *b)
{
    const EdgeRec *x = (const EdgeRec *)a;
    const EdgeRec *y = (const EdgeRec *)b;
    if (x->target != y->target)
        return (x->target > y->target) - (x->target < y->target);
    if (x->current != y->current)
        return (x->current > y->current) - (x->current < y->current);
    return (x->src > y->src) - (x->src < y->src);
}

static uint32_t lfunc_containing(uint32_t addr, const uint32_t *order,
                                 const uint32_t *bases, size_t n)
{
    size_t i = lower_bound_u32(bases, n, addr);
    if (i < n && bases[i] == addr) return order[i];
    return i ? order[i - 1] : order[0];
}

static size_t instr_lower(const FuncRec *f, uint32_t addr)
{
    size_t lo = 0, hi = f->instr_count;
    if (f->instr_off >= g_ninstr) return 0;
    if (hi > g_ninstr - f->instr_off) hi = g_ninstr - f->instr_off;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_iaddr[f->instr_off + mid] < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static size_t instr_upper(const FuncRec *f, uint32_t addr)
{
    size_t lo = 0, hi = f->instr_count;
    if (f->instr_off >= g_ninstr) return 0;
    if (hi > g_ninstr - f->instr_off) hi = g_ninstr - f->instr_off;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_iaddr[f->instr_off + mid] <= addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void finish_loop_stats(LoopRec *r, const uint32_t *order,
                              const uint32_t *bases, const uint32_t *region,
                              size_t n)
{
    size_t lb = lower_bound_u32(bases, n, r->target);
    int exact = (lb < n && bases[lb] == r->target);
    size_t body_lo = lb;
    if (!exact && body_lo > 0) --body_lo; /* L_ function containing the top */

    size_t hi = upper_bound_u32(bases, n, r->max_current);
    if (hi < body_lo) hi = body_lo;

    r->exact = (uint32_t)exact;
    r->body_funcs = 0;
    r->body_instr = 0;
    r->hot_pct = 0.0;
    r->writes = r->calls = r->atomics = r->reads = r->jumps = r->stackops = 0;

    for (size_t i = body_lo; i < hi; ++i) {
        uint32_t fi = order[i];
        if (region[fi] != r->func_start) continue;
        const FuncRec *f = &g_funcs[fi];
        size_t a = instr_lower(f, r->target);
        size_t b = instr_upper(f, r->max_current);
        if (b <= a) continue;

        ++r->body_funcs;
        size_t cnt = b - a;
        r->body_instr += cnt;
        for (size_t k = a; k < b; ++k) {
            uint8_t fl = g_iflags[f->instr_off + k];
            if (fl & IF_WRITE) ++r->writes;
            if (fl & IF_CALL) ++r->calls;
            if (fl & IF_ATOMIC) ++r->atomics;
            if (fl & IF_READ) ++r->reads;
            if (fl & IF_JUMP) ++r->jumps;
            if (fl & IF_STACK) ++r->stackops;
        }
        if (f->instr_count)
            r->hot_pct += f->self_pct * (double)cnt / (double)f->instr_count;
    }

    uint32_t hf = lfunc_containing(r->target, order, bases, n);
    r->header_func = hf;
    r->header_off = r->target - g_funcs[hf].base;

    if (r->body_instr == 0) {
        r->parallel_class = 3;
        r->score = 0.0;
        return;
    }

    if (r->atomics == 0 && r->writes == 0 && r->calls == 0)
        r->parallel_class = 0;
    else if (r->atomics == 0 &&
             (uint64_t)r->writes * 100 <= r->body_instr * 2 &&
             (uint64_t)r->calls * 100 <= r->body_instr)
        r->parallel_class = 1;
    else if (r->atomics == 0 &&
             (uint64_t)r->writes * 100 <= r->body_instr * 5 &&
             (uint64_t)r->calls * 100 <= r->body_instr * 3)
        r->parallel_class = 2;
    else
        r->parallel_class = 3;

    double serial = 1.0 + 2.0 * (double)r->writes + 4.0 * (double)r->calls +
                    16.0 * (double)r->atomics;
    if (g_profile_loaded && g_profile_matches > 0)
        r->score = r->hot_pct * sqrt((double)r->body_instr) / serial;
    else
        r->score = (double)r->body_instr * sqrt((double)r->body_instr) / serial;
}

static int cmp_loop_body(const void *a, const void *b)
{
    const LoopRec *x = (const LoopRec *)a;
    const LoopRec *y = (const LoopRec *)b;
    if (x->body_instr != y->body_instr)
        return (x->body_instr < y->body_instr) ? 1 : -1;
    if (x->span != y->span) return (x->span < y->span) ? 1 : -1;
    if (x->edges != y->edges) return (x->edges < y->edges) ? 1 : -1;
    return (x->target > y->target) - (x->target < y->target);
}

static int cmp_loop_span(const void *a, const void *b)
{
    const LoopRec *x = (const LoopRec *)a;
    const LoopRec *y = (const LoopRec *)b;
    if (x->span != y->span) return (x->span < y->span) ? 1 : -1;
    if (x->body_instr != y->body_instr)
        return (x->body_instr < y->body_instr) ? 1 : -1;
    return (x->edges < y->edges) ? 1 : -1;
}

static int cmp_loop_edges(const void *a, const void *b)
{
    const LoopRec *x = (const LoopRec *)a;
    const LoopRec *y = (const LoopRec *)b;
    if (x->edges != y->edges) return (x->edges < y->edges) ? 1 : -1;
    if (x->body_instr != y->body_instr)
        return (x->body_instr < y->body_instr) ? 1 : -1;
    return (x->span < y->span) ? 1 : -1;
}

static int cmp_loop_score(const void *a, const void *b)
{
    const LoopRec *x = (const LoopRec *)a;
    const LoopRec *y = (const LoopRec *)b;
    if (x->score != y->score) return (x->score < y->score) ? 1 : -1;
    if (x->body_instr != y->body_instr)
        return (x->body_instr < y->body_instr) ? 1 : -1;
    if (x->depth != y->depth) return (x->depth > y->depth) ? 1 : -1;
    return (x->target > y->target) - (x->target < y->target);
}

static int cmp_loop_hot(const void *a, const void *b)
{
    const LoopRec *x = (const LoopRec *)a;
    const LoopRec *y = (const LoopRec *)b;
    if (x->hot_pct != y->hot_pct) return (x->hot_pct < y->hot_pct) ? 1 : -1;
    if (x->body_instr != y->body_instr)
        return (x->body_instr < y->body_instr) ? 1 : -1;
    return (x->target > y->target) - (x->target < y->target);
}

static void header_text(const LoopRec *r, char *buf, size_t cap)
{
    const FuncRec *f = &g_funcs[r->header_func];
    if (r->header_off)
        snprintf(buf, cap, "%s+0x%X", f->name, r->header_off);
    else
        snprintf(buf, cap, "%s", f->name);
}

static const char *class_name(uint8_t c)
{
    switch (c) {
    case 0: return "A";
    case 1: return "B";
    case 2: return "C";
    default: return "D";
    }
}

static void print_members(const LoopRec *r, const uint32_t *order,
                          const uint32_t *bases, const uint32_t *region,
                          size_t n)
{
    size_t lb = lower_bound_u32(bases, n, r->target);
    int exact = (lb < n && bases[lb] == r->target);
    size_t body_lo = lb;
    if (!exact && body_lo > 0) --body_lo;
    size_t hi = upper_bound_u32(bases, n, r->max_current);
    if (hi < body_lo) hi = body_lo;

    size_t shown = 0;
    for (size_t i = body_lo; i < hi && shown < 10; ++i) {
        uint32_t fi = order[i];
        if (region[fi] != r->func_start) continue;
        const FuncRec *f = &g_funcs[fi];
        printf("      %-12s %s:%u  instr=%u wr=%u call=%u atom=%u\n",
               f->name, f->file, f->start_line, f->instr_count, f->writes,
               f->calls, f->atomics);
        ++shown;
    }
    if (r->body_funcs > shown)
        printf("      ... +%u more functions\n", r->body_funcs - (uint32_t)shown);
}

static void write_loops_tsv(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "find_loops: cannot write %s\n", path);
        return;
    }
    fprintf(f, "loop_top\tloop_bottom\tspan\tdepth\tedges\tbody_funcs\t"
               "body_instr\twrites\tcalls\tatomics\treads\tjumps\tstackops\t"
               "hot_pct\tclass\tscore\texact\tfunc_start\theader_lfunc\t"
               "header_off\tsrc_func\tsrc_file\tsrc_line\n");
    for (size_t i = 0; i < g_nloops; ++i) {
        const LoopRec *r = &g_loops[i];
        const FuncRec *h = &g_funcs[r->header_func];
        const FuncRec *s = &g_funcs[r->src];
        fprintf(f, "%08X\t%08X\t%u\t%u\t%u\t%u\t%llu\t%u\t%u\t%u\t%u\t%u\t%u\t"
                   "%.4f\t%s\t%.2f\t%u\t%08X\t%s\t%u\t%s\t%s\t%u\n",
                r->target, r->max_current, r->span, r->depth, r->edges,
                r->body_funcs, (unsigned long long)r->body_instr, r->writes,
                r->calls, r->atomics, r->reads, r->jumps, r->stackops,
                r->hot_pct, class_name(r->parallel_class), r->score, r->exact,
                r->func_start, h->name, r->header_off, s->name, s->file,
                r->src_line);
    }
    fclose(f);
}

static void write_edges_tsv(const char *path, const uint32_t *order,
                            const uint32_t *bases, const uint32_t *region,
                            size_t n)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "find_loops: cannot write %s\n", path);
        return;
    }
    fprintf(f, "src\tsrc_file\tsrc_line\ttarget\tcurrent\tspan\tfunc_start\t"
               "body_funcs\tbody_instr\twrites\tcalls\tatomics\thot_pct\texact\n");
    for (size_t i = 0; i < g_nedges; ++i) {
        const EdgeRec *e = &g_edges[i];
        LoopRec r;
        memset(&r, 0, sizeof(r));
        r.target = e->target;
        r.max_current = e->current;
        r.func_start = enclosing_start(e->target);
        r.span = e->current - e->target;
        finish_loop_stats(&r, order, bases, region, n);
        const FuncRec *s = &g_funcs[e->src];
        fprintf(f, "%s\t%s\t%u\t%08X\t%08X\t%u\t%08X\t%u\t%llu\t%u\t%u\t%u\t"
                   "%.4f\t%u\n",
                s->name, s->file, e->line, e->target, e->current, r.span,
                r.func_start, r.body_funcs, (unsigned long long)r.body_instr,
                r.writes, r.calls, r.atomics, r.hot_pct, r.exact);
    }
    fclose(f);
}

static void print_table_header(void)
{
    printf("%-10s %-18s %-10s %7s %4s %4s %4s %6s %3s %3s %6s %8s\n",
           "loop_top", "header_lfunc", "func_start", "instr", "wr", "call",
           "atom", "hot%", "cls", "dep", "edges", "span");
}

static void print_loop_row(const LoopRec *r, const uint32_t *order,
                           const uint32_t *bases, size_t n)
{
    char hdr[48];
    char fst[24];
    header_text(r, hdr, sizeof(hdr));
    uint32_t fi = lfunc_containing(r->func_start, order, bases, n);
    snprintf(fst, sizeof(fst), "%s", g_funcs[fi].name);
    if (g_funcs[fi].base != r->func_start)
        snprintf(fst, sizeof(fst), "%08X", r->func_start);

    printf("%08X   %-18s %-10s %7llu %4u %4u %4u %6.2f %3s %3u %6u %8u\n",
           r->target, hdr, fst, (unsigned long long)r->body_instr, r->writes,
           r->calls, r->atomics, r->hot_pct,
           class_name(r->parallel_class), r->depth, r->edges, r->span);
}

int main(int argc, char **argv)
{
    const char *out = NULL;
    const char *edge_out = NULL;
    const char *profile_path = NULL;
    int verbose = 0;
    int seed_loaded = 0;
    int argi = 1;

    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "-v") == 0) {
            verbose = 1;
            ++argi;
        } else if (strcmp(argv[argi], "-f") == 0 && argi + 1 < argc) {
            load_start_file(argv[argi + 1]);
            seed_loaded = 1;
            argi += 2;
        } else if (strcmp(argv[argi], "-p") == 0 && argi + 1 < argc) {
            profile_path = argv[argi + 1];
            load_profile(profile_path);
            argi += 2;
        } else if (strcmp(argv[argi], "-o") == 0 && argi + 1 < argc) {
            out = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "-e") == 0 && argi + 1 < argc) {
            edge_out = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "-h") == 0 ||
                   strcmp(argv[argi], "--help") == 0) {
            printf("usage: %s [-v] [-f functions.txt] [-p flat_report.txt] "
                   "[-o loops.tsv] [-e edges.tsv] <file.c|dir> [...]\n",
                   argv[0]);
            return 0;
        } else {
            fprintf(stderr, "find_loops: unknown option %s\n", argv[argi]);
            return 2;
        }
    }

    if (!seed_loaded) {
        const char *cands[] = {
            "build/corpus-full/game/functions.txt",
            "build/corpus-smoke/game/functions.txt",
            NULL
        };
        for (int i = 0; cands[i]; ++i) {
            if (access(cands[i], R_OK) == 0) {
                load_start_file(cands[i]);
                break;
            }
        }
    }

    const char *def = "build/android_app/debug/cmake/relifted/game";
    if (argi == argc)
        scan_path(def);
    else
        for (; argi < argc; ++argi)
            scan_path(argv[argi]);

    if (g_nfuncs == 0) {
        fprintf(stderr, "find_loops: no L_* functions found\n");
        return 1;
    }

    apply_profile();

    uint64_t total_instr = 0;
    for (size_t i = 0; i < g_nfuncs; ++i)
        total_instr += g_funcs[i].instr_count;

    uint32_t *order = (uint32_t *)malloc(g_nfuncs * sizeof(*order));
    uint32_t *bases = (uint32_t *)malloc(g_nfuncs * sizeof(*bases));
    uint32_t *region = (uint32_t *)malloc(g_nfuncs * sizeof(*region));
    if (!order || !bases || !region) {
        fprintf(stderr, "find_loops: out of memory (index)\n");
        return 1;
    }

    for (size_t i = 0; i < g_nfuncs; ++i)
        order[i] = (uint32_t)i;
    qsort(order, g_nfuncs, sizeof(*order), cmp_func_order);

    for (size_t i = 0; i < g_nfuncs; ++i) {
        bases[i] = g_funcs[order[i]].base;
        if (i > 0 && bases[i] == bases[i - 1])
            ++g_duplicate_bases;
    }

    /* Always give every address a containing region, even in sparse data. */
    if (g_nstarts == 0)
        for (size_t i = 0; i < g_nfuncs; ++i)
            push_start(g_funcs[i].base);
    push_start(bases[0]);

    qsort(g_starts, g_nstarts, sizeof(*g_starts), cmp_u32);
    size_t uniq = 0;
    for (size_t i = 0; i < g_nstarts; ++i)
        if (i == 0 || g_starts[i] != g_starts[i - 1])
            g_starts[uniq++] = g_starts[i];
    g_nstarts = uniq;

    for (size_t i = 0; i < g_nfuncs; ++i)
        region[i] = enclosing_start(g_funcs[i].base);

    size_t filtered = 0;
    for (size_t i = 0; i < g_nedges; ++i) {
        EdgeRec e = g_edges[i];
        uint32_t et = enclosing_start(e.target);
        uint32_t ec = enclosing_start(e.current);
        if (et && et == ec)
            g_edges[filtered++] = e;
    }
    size_t dropped = g_nedges - filtered;
    g_nedges = filtered;

    qsort(g_edges, g_nedges, sizeof(*g_edges), cmp_edge);

    for (size_t i = 0; i < g_nedges;) {
        size_t j = i + 1;
        while (j < g_nedges && g_edges[j].target == g_edges[i].target)
            ++j;

        const EdgeRec *best = &g_edges[i];
        LoopRec r;
        memset(&r, 0, sizeof(r));
        r.target = best->target;
        r.max_current = best->current;
        r.span = best->current - best->target;
        r.edges = (uint32_t)(j - i);
        r.func_start = enclosing_start(r.target);
        r.src = best->src;
        r.src_line = best->line;
        finish_loop_stats(&r, order, bases, region, g_nfuncs);
        push_loop(&r);
        i = j;
    }

    /* Nesting depth: number of other loops in the same original function whose
     * address range strictly contains this one. */
    if (g_nloops <= 20000) {
        for (size_t i = 0; i < g_nloops; ++i) {
            LoopRec *b = &g_loops[i];
            for (size_t j = 0; j < g_nloops; ++j) {
                if (i == j) continue;
                const LoopRec *a = &g_loops[j];
                if (a->func_start != b->func_start) continue;
                if (a->target <= b->target && b->max_current <= a->max_current &&
                    (a->target < b->target || b->max_current < a->max_current))
                    ++b->depth;
            }
        }
    }

    printf("files=%lu functions=%zu instructions=%llu backward_edges=%zu "
           "loop_headers=%zu starts=%zu seeds=%lu duplicate_bases=%lu "
           "tail_edges_dropped=%zu\n",
           g_files, g_nfuncs, (unsigned long long)total_instr, g_nedges,
           g_nloops, g_nstarts, g_seed_starts, g_duplicate_bases, dropped);
    if (g_seed_file)
        printf("seed_file=%s\n", g_seed_file);
    if (g_profile_loaded)
        printf("profile=%s matched_lfuncs=%lu\n", g_profile_file,
               g_profile_matches);
    int profile_active = g_profile_loaded && g_profile_matches > 0;
    if (g_profile_loaded && !profile_active)
        printf("profile has no L_* matches; using static ranking\n");
    printf("parallel classes: A=pure, B=low writes/calls, C=candidate, "
           "D=serial/atomic-bound; ranking=%s\n",
           profile_active ? "profile_self_pct*sqrt(body_instr)/serial_ops"
                          : "static_body_instr^1.5/serial_ops");

    if (profile_active) {
        qsort(g_loops, g_nloops, sizeof(*g_loops), cmp_loop_hot);
        size_t hot_shown = 0;
        printf("\nHot loop tops (up to 40, profile self samples in body):\n");
        print_table_header();
        for (size_t i = 0; i < g_nloops && hot_shown < 40; ++i) {
            const LoopRec *r = &g_loops[i];
            if (r->hot_pct <= 0.0) break;
            print_loop_row(r, order, bases, g_nfuncs);
            if (verbose)
                print_members(r, order, bases, region, g_nfuncs);
            ++hot_shown;
        }
        if (hot_shown == 0) printf("(none)\n");
    }

    qsort(g_loops, g_nloops, sizeof(*g_loops), cmp_loop_body);
    size_t top = g_nloops < 80 ? g_nloops : 80;
    printf("\nTop %zu loop tops by lifted body instructions:\n", top);
    print_table_header();
    for (size_t i = 0; i < top; ++i) {
        print_loop_row(&g_loops[i], order, bases, g_nfuncs);
        if (verbose) print_members(&g_loops[i], order, bases, region, g_nfuncs);
    }

    qsort(g_loops, g_nloops, sizeof(*g_loops), cmp_loop_score);
    top = g_nloops < 80 ? g_nloops : 80;
    size_t min_body = profile_active ? 16 : 200;
    printf("\n%s parallelization candidates (%s, class A/B):\n",
           profile_active ? "Hot" : "Static",
           profile_active ? "profile-weighted, hot%>0"
                          : "body_instr>=200");
    print_table_header();
    size_t shown = 0;
    for (size_t i = 0; i < g_nloops && shown < top; ++i) {
        const LoopRec *r = &g_loops[i];
        if (profile_active && r->hot_pct <= 0.0) break;
        if (profile_active && r->body_instr < min_body) continue;
        if (!profile_active && r->body_instr < 200) continue;
        if (r->parallel_class > 1) continue;
        print_loop_row(r, order, bases, g_nfuncs);
        if (verbose) print_members(r, order, bases, region, g_nfuncs);
        ++shown;
    }
    if (shown == 0)
        printf("(none)\n");
    if (profile_active)
        printf("note: hot%% is self samples clipped to [loop_top,loop_bottom]; "
               "class is static. Verify independence and same-scene A/B.\n");

    qsort(g_loops, g_nloops, sizeof(*g_loops), cmp_loop_span);
    top = g_nloops < 40 ? g_nloops : 40;
    printf("\nTop %zu loop tops by backward span:\n", top);
    print_table_header();
    for (size_t i = 0; i < top; ++i)
        print_loop_row(&g_loops[i], order, bases, g_nfuncs);

    qsort(g_loops, g_nloops, sizeof(*g_loops), cmp_loop_edges);
    top = g_nloops < 40 ? g_nloops : 40;
    printf("\nTop %zu loop tops by back-edge count:\n", top);
    print_table_header();
    for (size_t i = 0; i < top; ++i)
        print_loop_row(&g_loops[i], order, bases, g_nfuncs);

    qsort(g_loops, g_nloops, sizeof(*g_loops), cmp_loop_score);
    if (out) {
        write_loops_tsv(out);
        printf("\nwrote %s\n", out);
    }
    if (edge_out) {
        write_edges_tsv(edge_out, order, bases, region, g_nfuncs);
        printf("wrote %s\n", edge_out);
    }

    free(region);
    free(bases);
    free(order);
    free(g_loops);
    free(g_edges);
    free(g_iflags);
    free(g_iaddr);
    free(g_funcs);
    free(g_starts);
    free(g_ranges);
    free(g_prof);
    return 0;
}
