/* corpus.c - canonical corpus identity and VA inventory (ABI-3.1, BP-6, G-05).
 *
 * The hash covers the ACTUAL lift inputs, not the installed game's PE files:
 * rebased Storm/Mss modules are different bytes and therefore different
 * entries.  Entries are sorted by normalised relative path, so the result does
 * not depend on directory order, CRLF, or the machine that ran the lift.
 *
 *   corpus hash   --inputs <list.txt> [--root DIR] [--generator NAME]
 *                    [--lift-rev N] [--layout-sha256 HEX | --layout-file F]
 *                    [--out corpus.sha256] [--json report.json]
 *   corpus verify --inputs <list.txt> --expected <hex|file> [same metadata]
 *   corpus list   --dir <generated C dir> --out <va_list.txt>
 *
 * --layout-file hashes a frozen input (runtime/guest/guest_layout.h) instead
 * of taking a literal digest; the same option must be used by verify.
 *
 * inputs.txt: one path per line, '#' starts a comment, blank lines ignored.
 */
#include "sha256.h"
#include "dirwalk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#define MAX_ENTRIES 65536
#define LINE_CAP 8192

typedef struct {
    char path[DIR_MAX_PATH];
    uint64_t bytes;
    char sha[65];
} Entry;

typedef struct {
    Entry *v;
    size_t n, cap;
} Entries;

static void die(const char *msg)
{
    fprintf(stderr, "corpus: %s\n", msg);
    exit(1);
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}

static void entries_push(Entries *e, const char *path, uint64_t bytes, const char *sha)
{
    if (e->n == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 256;
        e->v = (Entry *)xrealloc(e->v, e->cap * sizeof *e->v);
    }
    if (strlen(path) >= sizeof e->v[e->n].path) die("path too long");
    strcpy(e->v[e->n].path, path);
    e->v[e->n].bytes = bytes;
    memcpy(e->v[e->n].sha, sha, 65);
    e->n++;
}

static int cmp_entry(const void *a, const void *b)
{
    return strcmp(((const Entry *)a)->path, ((const Entry *)b)->path);
}

static void feed(sha256_ctx *s, const char *text)
{
    sha256_update(s, text, strlen(text));
}

static void feedf(sha256_ctx *s, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    feed(s, buf);
}

/* Read a line of any length is unnecessary here: paths are short. */
static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == '\r' || end[-1] == '\n' ||
                       end[-1] == ' ' || end[-1] == '\t')) end--;
    *end = 0;
    return s;
}

static void load_inputs(const char *list, const char *root, Entries *out)
{
    FILE *f = fopen(list, "r");
    char line[LINE_CAP];
    if (!f) die("cannot open --inputs");

    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        char full[DIR_MAX_PATH];
        char sha[65];
        FILE *probe;
        if (!*p || *p == '#') continue;
        if (root && *root && p[0] != '/' && !(p[0] && p[1] == ':'))
            snprintf(full, sizeof full, "%s/%s", root, p);
        else
            snprintf(full, sizeof full, "%s", p);
        path_slash(full);
        if (sha256_file(full, sha) != 0) {
            fprintf(stderr, "corpus: missing input %s\n", full);
            fclose(f);
            exit(1);
        }
        probe = fopen(full, "rb");
        uint64_t size = 0;
        if (probe) {
            fseek(probe, 0, SEEK_END);
            long n = ftell(probe);
            if (n >= 0) size = (uint64_t)n;
            fclose(probe);
        }
        /* Store the normalised path as given by the list, not the resolved
         * absolute path: identity must survive a checkout in another dir. */
        entries_push(out, p, size, sha);
    }
    fclose(f);
    qsort(out->v, out->n, sizeof *out->v, cmp_entry);
    {
        size_t i, w = 0;
        for (i = 0; i < out->n; i++) {
            if (w && strcmp(out->v[w - 1].path, out->v[i].path) == 0) continue;
            if (w != i) out->v[w] = out->v[i];
            w++;
        }
        out->n = w;
    }
}

static void compute(const Entries *e, const char *generator, long lift_rev,
                    const char *layout_sha, char hex[65])
{
    sha256_ctx s;
    unsigned char digest[32];
    size_t i;
    sha256_init(&s);
    for (i = 0; i < e->n; i++)
        feedf(&s, "%s %s\n", e->v[i].path, e->v[i].sha);
    feedf(&s, "generator=%s\n", generator);
    feedf(&s, "lift_rev=%ld\n", lift_rev);
    feedf(&s, "layout_sha256=%s\n", layout_sha);
    sha256_final(&s, digest);
    sha256_hex(digest, hex);
}

static void json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { fputc('\\', f); fputc(*s, f); }
        else if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", (unsigned)*s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

static void write_report(const char *path, const Entries *e, const char *hex,
                         const char *generator, long lift_rev, const char *layout_sha)
{
    FILE *f;
    size_t i;
    if (!path) return;
    f = fopen(path, "w");
    if (!f) die("cannot open --json");
    fprintf(f, "{\n  \"schema\": 1,\n  \"corpus_sha256\": \"%s\",\n", hex);
    fprintf(f, "  \"generator\": "); json_string(f, generator); fprintf(f, ",\n");
    fprintf(f, "  \"lift_rev\": %ld,\n", lift_rev);
    fprintf(f, "  \"layout_sha256\": "); json_string(f, layout_sha); fprintf(f, ",\n");
    fprintf(f, "  \"inputs\": %lu,\n  \"files\": [\n", (unsigned long)e->n);
    for (i = 0; i < e->n; i++) {
        fprintf(f, "    {\"path\": "); json_string(f, e->v[i].path);
        fprintf(f, ", \"bytes\": %llu, \"sha256\": \"%s\"}%s\n",
                (unsigned long long)e->v[i].bytes, e->v[i].sha,
                i + 1 < e->n ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

/* ---- VA inventory ------------------------------------------------------ */

typedef struct { uint32_t *v; size_t n, cap; } Vas;

static void vas_push(Vas *a, uint32_t va)
{
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 4096;
        a->v = (uint32_t *)xrealloc(a->v, a->cap * sizeof *a->v);
    }
    a->v[a->n++] = va;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void scan_line(const char *line, Vas *vas)
{
    const char *p = line;
    while ((p = strstr(p, "void L_")) != NULL) {
        char hex[9];
        p += 7;
        if (strlen(p) >= 8) {
            memcpy(hex, p, 8);
            hex[8] = 0;
            char *end = NULL;
            unsigned long v = strtoul(hex, &end, 16);
            if (end == hex + 8) vas_push(vas, (uint32_t)v);
        }
    }
}

typedef struct { Vas *vas; size_t files; } ListCtx;

static int list_visit(const char *path, void *user)
{
    ListCtx *ctx = (ListCtx *)user;
    FILE *f;
    char line[LINE_CAP];
    if (!path_ends_with(path, ".c") && !path_ends_with(path, ".h")) return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    ctx->files++;
    while (fgets(line, sizeof line, f)) scan_line(line, ctx->vas);
    fclose(f);
    return 0;
}

static int cmd_list(const char *dir, const char *out)
{
    ListCtx ctx;
    FILE *f;
    size_t i, w = 0;
    memset(&ctx, 0, sizeof ctx);
    ctx.vas = (Vas *)xrealloc(NULL, sizeof(Vas));
    memset(ctx.vas, 0, sizeof(Vas));
    if (dir_walk(dir, list_visit, &ctx) != 0) die("walk failed");
    qsort(ctx.vas->v, ctx.vas->n, sizeof *ctx.vas->v, cmp_u32);
    f = fopen(out, "w");
    if (!f) die("cannot open --out");
    for (i = 0; i < ctx.vas->n; i++) {
        if (w && ctx.vas->v[w - 1] == ctx.vas->v[i]) continue;
        ctx.vas->v[w++] = ctx.vas->v[i];
    }
    for (i = 0; i < w; i++) fprintf(f, "%08X\n", ctx.vas->v[i]);
    fclose(f);
    printf("corpus: %lu unique VA from %lu file(s) -> %s\n",
           (unsigned long)w, (unsigned long)ctx.files, out);
    free(ctx.vas->v);
    free(ctx.vas);
    return 0;
}

static const char *arg(int argc, char **argv, int *i, const char *name)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "corpus: %s needs a value\n", name);
        exit(1);
    }
    return argv[++(*i)];
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "";
    const char *inputs = NULL, *root = ".", *out = NULL, *json = NULL;
    const char *generator = "wc3lift", *layout_sha = "", *expected = NULL;
    const char *dir = NULL;
    char layout_hex[65] = "";
    long lift_rev = 6;
    int i;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--inputs") == 0) inputs = arg(argc, argv, &i, argv[i]);
        else if (strcmp(argv[i], "--root") == 0) root = arg(argc, argv, &i, argv[i]);
        else if (strcmp(argv[i], "--out") == 0) out = arg(argc, argv, &i, argv[i]);
        else if (strcmp(argv[i], "--json") == 0) json = arg(argc, argv, &i, argv[i]);
        else if (strcmp(argv[i], "--generator") == 0) generator = arg(argc, argv, &i, argv[i]);
        else if (strcmp(argv[i], "--layout-sha256") == 0) layout_sha = arg(argc, argv, &i, argv[i]);
        else if (strcmp(argv[i], "--layout-file") == 0) {
            const char *p = arg(argc, argv, &i, argv[i]);
            if (sha256_file(p, layout_hex) != 0) die("cannot hash --layout-file");
            layout_sha = layout_hex;
        }
        else if (strcmp(argv[i], "--lift-rev") == 0) lift_rev = strtol(arg(argc, argv, &i, argv[i]), NULL, 10);
        else if (strcmp(argv[i], "--expected") == 0) expected = arg(argc, argv, &i, argv[i]);
        else if (strcmp(argv[i], "--dir") == 0) dir = arg(argc, argv, &i, argv[i]);
        else { fprintf(stderr, "corpus: unknown option %s\n", argv[i]); return 1; }
    }

    if (strcmp(cmd, "list") == 0) {
        if (!dir || !out) die("list needs --dir and --out");
        return cmd_list(dir, out);
    }
    if (strcmp(cmd, "hash") != 0 && strcmp(cmd, "verify") != 0) {
        fprintf(stderr, "usage: corpus hash|verify|list [options]\n");
        return 1;
    }
    if (!inputs) die("hash/verify need --inputs");

    {
        Entries e;
        char hex[65];
        memset(&e, 0, sizeof e);
        load_inputs(inputs, root, &e);
        if (e.n == 0) die("empty input set");
        compute(&e, generator, lift_rev, layout_sha, hex);
        if (out) {
            FILE *f = fopen(out, "w");
            if (!f) die("cannot open --out");
            fprintf(f, "%s\n", hex);
            fclose(f);
        }
        write_report(json, &e, hex, generator, lift_rev, layout_sha);
        if (strcmp(cmd, "verify") == 0) {
            char want[256];
            if (!expected) die("verify needs --expected");
            if (strlen(expected) == 64) {
                memcpy(want, expected, 65);
            } else {
                FILE *f = fopen(expected, "r");
                if (!f || !fgets(want, sizeof want, f)) die("cannot read --expected");
                want[strcspn(want, "\r\n")] = 0;
                if (f) fclose(f);
            }
            if (strcmp(want, hex) != 0) {
                printf("corpus: FAIL expected %s got %s\n", want, hex);
                free(e.v);
                return 1;
            }
            printf("corpus: pass %s (%lu inputs)\n", hex, (unsigned long)e.n);
        } else {
            printf("corpus: %s (%lu inputs)\n", hex, (unsigned long)e.n);
        }
        free(e.v);
    }
    return 0;
}
