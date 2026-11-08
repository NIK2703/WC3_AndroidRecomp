/* splitcorpus.c - cut generated lift output into build-sized translation units.
 *
 *   splitcorpus <generated.c> <out-dir> <prefix> [funcs-per-chunk] [max-mb]
 *
 * The lift generator emits one C file per module.  game.dll alone is ~300 MB,
 * which is not a translation unit a compiler can optimise in bounded time or
 * memory.  This tool cuts at function boundaries (a line starting with
 * "void L_") and writes chunks of at most N functions and M megabytes.  The
 * output is deterministic: same input, same chunk names and bytes.
 *
 * Functions are ordered by their call graph before the cut.  The generated
 * bodies do not call each other directly; they use dispatch(c, 0x...) and
 * dispatch_jmp(c, 0x...), so those literals are the call edges.  Connected
 * components are emitted together, then the normal function/size caps split
 * them into translation units.  This is the P1-12/O-03 input point: a later
 * devirtualisation pass gets caller and callee in the same TU instead of
 * relying on the old counter-based order.
 *
 * The original file header is replaced by a minimal header that includes
 * "cpu.h" for every chunk.  Lines before the first function are dropped.
 *
 *   splitcorpus build/corpus-full/game/closed.c \
 *       build/relifted/game game 500 64
 *
 * Exit codes: 0 pass, 1 I/O or usage error.
 */
#include "dirwalk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#if defined(_WIN32) && defined(WC3_FORCE_POSIX)
#define MKDIR(p) mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0775)
#endif
#endif

#define LINE_CAP (1u << 20)
#define COPY_CAP (1u << 16)

typedef struct {
    uint32_t va;
    long off;
    long len;
} Func;

typedef struct {
    uint32_t va;
    uint32_t idx;
} VaIndex;

typedef struct {
    uint32_t from;
    uint32_t to_va;
} RawEdge;

static int mkdir_p(const char *path)
{
    char tmp[DIR_MAX_PATH];
    size_t i, n;
    snprintf(tmp, sizeof tmp, "%s", path);
    path_slash(tmp);
    n = strlen(tmp);
    for (i = 1; i <= n; i++) {
        if (tmp[i] == '/' || tmp[i] == 0) {
            char save = tmp[i];
            tmp[i] = 0;
            if (tmp[0] && !path_is_dir(tmp)) MKDIR(tmp);
            tmp[i] = save;
        }
    }
    return path_is_dir(path) ? 0 : -1;
}

static const char *base_name(const char *path)
{
    const char *base = path, *p;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

static int cmp_va(const void *a, const void *b)
{
    const VaIndex *x = (const VaIndex *)a;
    const VaIndex *y = (const VaIndex *)b;
    if (x->va < y->va) return -1;
    if (x->va > y->va) return 1;
    return 0;
}

static size_t find_va(const VaIndex *map, size_t n, uint32_t va, int *found)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (map[mid].va < va) lo = mid + 1;
        else hi = mid;
    }
    *found = lo < n && map[lo].va == va;
    return lo < n ? map[lo].idx : 0;
}

static int grow_edges(RawEdge **edges, size_t *cap, size_t need)
{
    if (need <= *cap) return 0;
    size_t next = *cap ? *cap * 2 : 4096;
    while (next < need) next *= 2;
    RawEdge *p = (RawEdge *)realloc(*edges, next * sizeof *p);
    if (!p) return -1;
    *edges = p;
    *cap = next;
    return 0;
}

static void scan_dispatch(const char *line, uint32_t from,
                          RawEdge **edges, size_t *n, size_t *cap)
{
    const char *p = line;
    for (;;) {
        const char *a = strstr(p, "dispatch(c, 0x");
        const char *b = strstr(p, "dispatch_jmp(c, 0x");
        const char *q = a && b ? (a < b ? a : b) : (a ? a : b);
        if (!q) return;
        const char *hex = strstr(q, "0x");
        char *end = NULL;
        unsigned long v = strtoul(hex + 2, &end, 16);
        if (end && end > hex + 2 && (v & ~0xFFFFFFFFul) == 0) {
            if (grow_edges(edges, cap, *n + 1) != 0) return;
            (*edges)[*n].from = from;
            (*edges)[*n].to_va = (uint32_t)v;
            (*n)++;
        }
        p = end && end > q ? end : q + 1;
    }
}

/* Copy one function block exactly as it appeared in the source.  Rewinding
 * the input before every block keeps the first pass streaming and bounded. */
static int copy_block(FILE *in, FILE *out, long off, long len, char *buf)
{
    if (fseek(in, off, SEEK_SET) != 0) return -1;
    while (len > 0) {
        size_t want = len < COPY_CAP ? (size_t)len : COPY_CAP;
        size_t got = fread(buf, 1, want, in);
        if (got != want) return -1;
        if (fwrite(buf, 1, got, out) != got) return -1;
        len -= (long)got;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *src, *outdir, *prefix;
    long per = 500, max_mb = 0;
    char header[DIR_MAX_PATH + 128];
    char outpath[DIR_MAX_PATH];
    FILE *in, *out = NULL;
    char *line, *copy;
    size_t header_len, written = 0;
    long count = 0, chunk = 0;
    int at_line_start = 1;
    long current_off = -1, pos;
    size_t nfunc = 0, capfunc = 0;
    Func *funcs = NULL;
    RawEdge *edges = NULL;
    size_t nedges = 0, capedge = 0;
    VaIndex *vamap = NULL;
    size_t *adj_start = NULL, *adj_fill = NULL;
    uint32_t *adj = NULL;
    uint32_t *order = NULL, *queue = NULL;
    unsigned char *seen = NULL;
    size_t i, k, norder = 0;
    int rc = 1;

    if (argc < 4 || argc > 6) {
        fprintf(stderr,
                "usage: splitcorpus <generated.c> <out-dir> <prefix> "
                "[funcs-per-chunk] [max-mb]\n");
        return 1;
    }
    src = argv[1];
    outdir = argv[2];
    prefix = argv[3];
    if (argc > 4) per = strtol(argv[4], NULL, 10);
    if (argc > 5) max_mb = strtol(argv[5], NULL, 10);
    if (per <= 0) {
        fprintf(stderr, "splitcorpus: funcs-per-chunk must be positive\n");
        return 1;
    }
    if (mkdir_p(outdir) != 0) {
        fprintf(stderr, "splitcorpus: cannot create %s\n", outdir);
        return 1;
    }
    in = fopen(src, "rb");
    if (!in) {
        fprintf(stderr, "splitcorpus: cannot open %s\n", src);
        return 1;
    }
    line = (char *)malloc(LINE_CAP);
    copy = (char *)malloc(COPY_CAP);
    if (!line || !copy) {
        fprintf(stderr, "splitcorpus: out of memory\n");
        goto done;
    }

    /* Pass 1: function boundaries and dispatch edges. */
    while ((pos = ftell(in)) >= 0 && fgets(line, (int)LINE_CAP, in)) {
        size_t len = strlen(line);
        int is_func = at_line_start && !strncmp(line, "void L_", 7);
        if (is_func) {
            char *end = NULL;
            unsigned long va = strtoul(line + 7, &end, 16);
            if (end && end > line + 7 && *end == '(' &&
                (va & ~0xFFFFFFFFul) == 0) {
                if (nfunc == capfunc) {
                    size_t next = capfunc ? capfunc * 2 : 4096;
                    Func *p = (Func *)realloc(funcs, next * sizeof *p);
                    if (!p) {
                        fprintf(stderr, "splitcorpus: out of memory\n");
                        goto done;
                    }
                    funcs = p;
                    capfunc = next;
                }
                if (nfunc) funcs[nfunc - 1].len = pos - funcs[nfunc - 1].off;
                funcs[nfunc].va = (uint32_t)va;
                funcs[nfunc].off = pos;
                funcs[nfunc].len = 0;
                nfunc++;
                current_off = pos;
            }
        }
        if (nfunc) {
            scan_dispatch(line, (uint32_t)(nfunc - 1), &edges, &nedges, &capedge);
        }
        at_line_start = len > 0 && line[len - 1] == '\n';
    }
    if (ferror(in)) {
        fprintf(stderr, "splitcorpus: read failed: %s\n", src);
        goto done;
    }
    pos = ftell(in);
    if (nfunc && current_off >= 0 && pos >= current_off)
        funcs[nfunc - 1].len = pos - current_off;
    if (nfunc == 0) {
        fprintf(stderr, "splitcorpus: no L_ functions in %s\n", src);
        goto done;
    }

    /* Map dispatch targets to function indices, then build an undirected
     * adjacency list.  Cross-module targets are intentionally ignored: they
     * cannot share a translation unit with this source file. */
    vamap = (VaIndex *)malloc(nfunc * sizeof *vamap);
    adj_start = (size_t *)calloc(nfunc + 1, sizeof *adj_start);
    adj_fill = (size_t *)calloc(nfunc + 1, sizeof *adj_fill);
    order = (uint32_t *)malloc(nfunc * sizeof *order);
    queue = (uint32_t *)malloc(nfunc * sizeof *queue);
    seen = (unsigned char *)calloc(nfunc, 1);
    if (!vamap || !adj_start || !adj_fill || !order || !queue || !seen) {
        fprintf(stderr, "splitcorpus: out of memory\n");
        goto done;
    }
    for (i = 0; i < nfunc; i++) {
        vamap[i].va = funcs[i].va;
        vamap[i].idx = (uint32_t)i;
    }
    qsort(vamap, nfunc, sizeof *vamap, cmp_va);
    for (i = 0; i < nedges; i++) {
        int found = 0;
        size_t to = find_va(vamap, nfunc, edges[i].to_va, &found);
        if (!found) continue;
        adj_start[edges[i].from + 1]++;
        adj_start[to + 1]++;
    }
    for (i = 1; i <= nfunc; i++) adj_start[i] += adj_start[i - 1];
    adj = (uint32_t *)malloc((adj_start[nfunc] ? adj_start[nfunc] : 1) *
                             sizeof *adj);
    if (!adj) {
        fprintf(stderr, "splitcorpus: out of memory\n");
        goto done;
    }
    memcpy(adj_fill, adj_start, (nfunc + 1) * sizeof *adj_fill);
    for (i = 0; i < nedges; i++) {
        int found = 0;
        size_t to = find_va(vamap, nfunc, edges[i].to_va, &found);
        if (!found) continue;
        adj[adj_fill[edges[i].from]++] = (uint32_t)to;
        adj[adj_fill[to]++] = edges[i].from;
    }

    /* Emit components in source order, BFS inside each component.  The order
     * is deterministic and keeps neighbours close without changing the chunk
     * header or the per-chunk caps. */
    for (i = 0; i < nfunc; i++) {
        size_t qh = 0, qt = 0;
        if (seen[i]) continue;
        seen[i] = 1;
        queue[qt++] = (uint32_t)i;
        while (qh < qt) {
            uint32_t v = queue[qh++];
            order[norder++] = v;
            for (k = adj_start[v]; k < adj_start[v + 1]; k++) {
                uint32_t w = adj[k];
                if (!seen[w]) {
                    seen[w] = 1;
                    queue[qt++] = w;
                }
            }
        }
    }

    /* Pass 2: write the same function blocks in graph order. */
    snprintf(header, sizeof header,
             "/* AUTO-GENERATED split of %s - do not edit */\n"
             "#include \"cpu.h\"\n\n",
             base_name(src));
    header_len = strlen(header);

    for (i = 0; i < norder; i++) {
        Func *f = &funcs[order[i]];
        long len = f->len;
        if (!out || count >= per ||
            (max_mb > 0 && count > 0 &&
             written + (size_t)len > (size_t)max_mb * 1024u * 1024u)) {
            if (out) fclose(out);
            snprintf(outpath, sizeof outpath, "%s/%s_%04ld.c",
                     outdir, prefix, chunk++);
            out = fopen(outpath, "wb");
            if (!out) {
                fprintf(stderr, "splitcorpus: cannot write %s\n", outpath);
                goto done;
            }
            if (fwrite(header, 1, header_len, out) != header_len) {
                fprintf(stderr, "splitcorpus: write failed: %s\n", outpath);
                fclose(out);
                out = NULL;
                goto done;
            }
            written = header_len;
            count = 0;
        }
        if (copy_block(in, out, f->off, len, copy) != 0) {
            fprintf(stderr, "splitcorpus: read/write failed: %s\n", outpath);
            goto done;
        }
        written += (size_t)len;
        count++;
    }

    printf("splitcorpus: %s: %ld functions -> %ld file(s) of <=%ld%s in %s\n",
           base_name(src), (long)nfunc, chunk, per,
           max_mb > 0 ? " and size cap" : "", outdir);
    rc = 0;

done:
    if (out) fclose(out);
    fclose(in);
    free(line);
    free(copy);
    free(funcs);
    free(edges);
    free(vamap);
    free(adj_start);
    free(adj_fill);
    free(adj);
    free(order);
    free(queue);
    free(seen);
    return rc;
}
