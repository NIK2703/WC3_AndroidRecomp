/* addr.c - resolve host RVAs against the GNU ld linker map.
 *
 * The host crash handler prints the faulting instruction as
 * "<module>+<rva>".  With ~71k lifted functions in one translation unit the
 * DWARF line table is 20 MB and gcc emits a single CU that spans the whole
 * image, so addr2line answers "??" and gdb's `info symbol` only sees the
 * (stripped) minimal symbol table.  The linker map is the one artefact that
 * always has the answer: it lists every L_XXXXXXXX with its final address.
 *
 *   addr build27/wc3_host27.map 3E0958 3C69C8
 *   addr build27/wc3_host27.map =main            # symbol -> address
 *   addr build27/wc3_host27.map 3E0958 --base 140000000
 *
 * A query is either a hex number (with or without 0x; interpreted as an RVA
 * unless --abs) or "=name" to look a symbol up by name.  For a number the
 * output is the containing symbol plus the byte offset into it, which is what
 * turns "hf[0] rva=3E0958" into "L_6F04DAA0+0x1C0" and names the guest
 * function that was executing.
 *
 * The map is read once and the symbol table is sorted, so a batch of queries
 * costs one pass.  Nothing here is guest-specific: it works for any PE the
 * linker produced a map for.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned long long addr; char name[96]; } Sym;

static Sym *g_sym;
static size_t g_n, g_cap;

static void add(unsigned long long a, const char *name) {
    if (g_n == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 4096;
        g_sym = (Sym *)realloc(g_sym, g_cap * sizeof *g_sym);
        if (!g_sym) { fprintf(stderr, "addr: out of memory\n"); exit(1); }
    }
    g_sym[g_n].addr = a;
    snprintf(g_sym[g_n].name, sizeof g_sym[g_n].name, "%s", name);
    g_n++;
}

static int sym_cmp(const void *pa, const void *pb) {
    const Sym *a = (const Sym *)pa, *b = (const Sym *)pb;
    if (a->addr != b->addr) return (a->addr < b->addr) ? -1 : 1;
    return strcmp(a->name, b->name);
}

/* A map line that carries a symbol looks like
 *
 *                     0x00000001403fd4fe                L_6F04DAA0
 *
 * while a section line is "0xADDR 0xSIZE object.o" and a file line is
 * "path.o:" or " .text 0xADDR 0xSIZE".  Requiring exactly two tokens, the
 * first of which is a 0x-prefixed number, keeps the parser honest. */
static int parse_line(char *line) {
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (p[0] != '0' || p[1] != 'x') return 0;
    char *end = NULL;
    unsigned long long a = strtoull(p, &end, 16);
    if (end == p) return 0;
    p = end;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '\r' || *p == '\n') return 0;
    char *name = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    char *tail = p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p) return 0;            /* more than two tokens: section/object line */
    *tail = 0;
    if (!name[0]) return 0;
    /* Object files are listed as "0xADDR 0xSIZE path.o"; those were already
     * rejected, but a bare path can also appear.  Filter the obvious ones. */
    if (strstr(name, ".o") || strstr(name, ".obj")) return 0;
    add(a, name);
    return 1;
}

static void load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "addr: cannot read %s\n", path); exit(1); }
    static char line[4096];
    while (fgets(line, sizeof line, f)) parse_line(line);
    fclose(f);
    if (!g_n) { fprintf(stderr, "addr: %s: no symbols\n", path); exit(1); }
    qsort(g_sym, g_n, sizeof *g_sym, sym_cmp);
}

static void lookup_addr(unsigned long long q, unsigned long long base) {
    size_t lo = 0, hi = g_n;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (g_sym[m].addr <= q) lo = m + 1;
        else hi = m;
    }
    if (lo == 0) { printf("%llX  <below first symbol>\n", q); return; }
    const Sym *s = &g_sym[lo - 1];
    printf("%llX  %s+0x%llX", q, s->name, q - s->addr);
    if (lo < g_n)
        printf("   (next %s at %llX)", g_sym[lo].name, g_sym[lo].addr);
    printf("\n");
    (void)base;
}

static void lookup_name(const char *want) {
    int hits = 0;
    for (size_t i = 0; i < g_n; i++)
        if (!strcmp(g_sym[i].name, want)) {
            printf("%s  %llX\n", want, g_sym[i].addr);
            hits++;
        }
    if (!hits) printf("%s  <not found>\n", want);
}

static void usage(void) {
    fputs("usage: addr <map> <hex-rva|0xVA|--abs HEX|=name>...\n"
          "  --base HEX   image base added to plain hex queries (default 140000000)\n"
          "  --abs        queries are absolute VMAs, not RVAs\n"
          "  --list N     print N symbols starting at the first query\n", stderr);
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(); return 2; }
    unsigned long long base = 0x140000000ull;
    int abs_mode = 0, list = 0;
    const char *map = argv[1];
    const char *queries[64];
    int nq = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--base") && i + 1 < argc) base = strtoull(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--abs")) abs_mode = 1;
        else if (!strcmp(argv[i], "--list") && i + 1 < argc) list = atoi(argv[++i]);
        else if (nq < 64) queries[nq++] = argv[i];
    }
    load(map);
    for (int i = 0; i < nq; i++) {
        const char *q = queries[i];
        if (q[0] == '=') { lookup_name(q + 1); continue; }
        char *end = NULL;
        unsigned long long v = strtoull(q, &end, 16);
        if (end == q) { printf("%s  <not a number>\n", q); continue; }
        lookup_addr(abs_mode ? v : base + v, base);
        if (list) {
            size_t lo = 0, hi = g_n;
            while (lo < hi) {
                size_t m = lo + (hi - lo) / 2;
                if (g_sym[m].addr <= (abs_mode ? v : base + v)) lo = m + 1;
                else hi = m;
            }
            for (int k = 0; k < list && lo + (size_t)k < g_n; k++)
                printf("    +%d  %llX  %s\n", k, g_sym[lo + (size_t)k].addr,
                       g_sym[lo + (size_t)k].name);
        }
    }
    return 0;
}
