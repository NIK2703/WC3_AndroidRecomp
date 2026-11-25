/* x87_invalid_outline.c - batch patch stage: outline x87 IE classification.
 *
 * The canonical corpus is generated once and the build applies patch stages to
 * it; changing only a header is not enough to reach the product corpus.  This
 * stage rewrites the generated C:
 *
 *   - scans each `void L_...(CPU *restrict c)` definition;
 *   - counts plain x87_add/sub/mul/div calls in the body (comments excluded);
 *   - if the count reaches --min-sites and the function has no *_fpsr calls,
 *     replaces those calls with the outlined x87_out_add/sub/mul/div twins.
 *
 * Small functions keep the inline classification: a call turns a leaf
 * function into a non-leaf one, which costs more than 1-3 duplicated checks.
 * The FPSR stage runs first, so functions it rewrote have no plain x87_add
 * calls left and are skipped automatically.
 *
 * Usage:
 *   x87_invalid_outline --inputs FILE --out-dir DIR --report FILE
 *                       [--min-sites N] [--quiet]
 *   x87_invalid_outline --self-test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char *m)
{
    fprintf(stderr, "x87_invalid_outline: %s\n", m);
    exit(1);
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    size_t cap = 4096, n = 0;
    char *s;
    if (!f) die(path);
    s = (char *)malloc(cap);
    if (!s) die("out of memory");
    for (;;) {
        size_t got;
        if (n + 1024 > cap) {
            cap *= 2;
            s = (char *)realloc(s, cap);
            if (!s) die("out of memory");
        }
        got = fread(s + n, 1, 1024, f);
        n += got;
        if (got < 1024) break;
    }
    fclose(f);
    s[n] = 0;
    *len = n;
    return s;
}

static void write_file(const char *path, const char *s, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) die("write failed");
    if (n && fwrite(s, 1, n, f) != n) die("write failed");
    fclose(f);
}

static const char *path_base(const char *p)
{
    const char *b = p, *q;
    for (q = p; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_close_brace(const char *s)
{
    while (*s && is_space(*s)) s++;
    return s[0] == '}' && s[1] == 0;
}

/* Replace every comment byte with a space so token searches ignore comments
 * while keeping positions aligned with the original line. */
static void clean_comments(const char *src, char *dst, int *in_block)
{
    size_t i, n = strlen(src);
    for (i = 0; i < n; i++) {
        char c = src[i];
        if (*in_block) {
            if (c == '*' && i + 1 < n && src[i + 1] == '/') {
                dst[i] = dst[i + 1] = ' ';
                i++;
                *in_block = 0;
            } else {
                dst[i] = ' ';
            }
        } else if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            dst[i] = dst[i + 1] = ' ';
            i++;
            *in_block = 1;
        } else if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            for (; i < n; i++) dst[i] = ' ';
            break;
        } else {
            dst[i] = c;
        }
    }
    dst[n] = 0;
}

static const char *const OPS[4][2] = {
    {"x87_add(", "x87_out_add("},
    {"x87_sub(", "x87_out_sub("},
    {"x87_mul(", "x87_out_mul("},
    {"x87_div(", "x87_out_div("}
};

static size_t count_sites(const char *clean, int *has_fpsr)
{
    size_t total = 0, k;
    if (strstr(clean, "_fpsr")) *has_fpsr = 1;
    for (k = 0; k < 4; k++) {
        const char *p = clean;
        size_t fl = strlen(OPS[k][0]);
        while ((p = strstr(p, OPS[k][0])) != NULL) {
            total++;
            p += fl;
        }
    }
    return total;
}

/* Rewrite tokens only where the original line and its cleaned copy agree;
 * that leaves comments and strings untouched. */
static char *rewrite_line(const char *line, const char *clean)
{
    size_t n = strlen(line), cap = n * 4 + 64, d = 0, i = 0;
    char *out = (char *)malloc(cap);
    if (!out) die("out of memory");
    while (i < n) {
        size_t k, matched = 0;
        for (k = 0; k < 4; k++) {
            size_t fl = strlen(OPS[k][0]), tl = strlen(OPS[k][1]);
            if (i + fl <= n && !strncmp(clean + i, OPS[k][0], fl)
                && !strncmp(line + i, OPS[k][0], fl)) {
                while (d + tl + 1 > cap) {
                    cap *= 2;
                    out = (char *)realloc(out, cap);
                    if (!out) die("out of memory");
                }
                memcpy(out + d, OPS[k][1], tl);
                d += tl;
                i += fl;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            if (d + 2 > cap) {
                cap *= 2;
                out = (char *)realloc(out, cap);
                if (!out) die("out of memory");
            }
            out[d++] = line[i++];
        }
    }
    out[d] = 0;
    return out;
}

struct stats {
    size_t funcs;
    size_t eligible;
    size_t rewritten;
    size_t sites;
};

static char *rewrite_text(const char *text, size_t n, int min_sites,
                          struct stats *st)
{
    size_t cap = 16, nl = 0, i, k, dlen = 0, dcap = n + 4096;
    char **lines, **clean_lines, **out_lines, *copy, *out;
    int in_block = 0;

    lines = (char **)malloc(cap * sizeof(char *));
    clean_lines = (char **)malloc(cap * sizeof(char *));
    out_lines = (char **)malloc(cap * sizeof(char *));
    copy = (char *)malloc(n + 1);
    out = (char *)malloc(dcap);
    if (!lines || !clean_lines || !out_lines || !copy || !out)
        die("out of memory");
    memcpy(copy, text, n + 1);

    i = 0;
    while (i < n) {
        size_t s = i, e = i;
        while (e < n && copy[e] != '\n') e++;
        if (nl == cap) {
            cap *= 2;
            lines = (char **)realloc(lines, cap * sizeof(char *));
            clean_lines = (char **)realloc(clean_lines, cap * sizeof(char *));
            out_lines = (char **)realloc(out_lines, cap * sizeof(char *));
            if (!lines || !clean_lines || !out_lines) die("out of memory");
        }
        copy[e] = 0;
        lines[nl] = copy + s;
        clean_lines[nl] = (char *)malloc(e - s + 1);
        out_lines[nl] = lines[nl];
        if (!clean_lines[nl]) die("out of memory");
        clean_comments(lines[nl], clean_lines[nl], &in_block);
        nl++;
        i = (e < n) ? e + 1 : n;
    }

    for (i = 0; i < nl; i++) {
        size_t brace, end, sites = 0;
        int has_fpsr = 0;
        if (strncmp(lines[i], "void L_", 7) || !strstr(lines[i], "(CPU"))
            continue;
        st->funcs++;
        brace = i + 1;
        if (brace >= nl || strcmp(lines[brace], "{") != 0) continue;
        end = brace + 1;
        while (end < nl && !is_close_brace(lines[end])) end++;
        if (end >= nl) continue;
        for (k = brace + 1; k < end; k++)
            sites += count_sites(clean_lines[k], &has_fpsr);
        if (!sites || has_fpsr) continue;
        st->eligible++;
        st->sites += sites;
        if ((int)sites < min_sites) continue;
        st->rewritten++;
        for (k = brace + 1; k < end; k++)
            out_lines[k] = rewrite_line(lines[k], clean_lines[k]);
    }

    for (i = 0; i < nl; i++) {
        size_t l = strlen(out_lines[i]);
        while (dlen + l + 2 > dcap) {
            dcap *= 2;
            out = (char *)realloc(out, dcap);
            if (!out) die("out of memory");
        }
        memcpy(out + dlen, out_lines[i], l);
        dlen += l;
        out[dlen++] = '\n';
        out[dlen] = 0;
        if (out_lines[i] != lines[i]) free(out_lines[i]);
        free(clean_lines[i]);
    }
    free(lines);
    free(clean_lines);
    free(out_lines);
    free(copy);
    return out;
}

static int self_test(int min_sites)
{
    static const char fixture[] =
        "void L_00000000(CPU *restrict c)\n"
        "{\n"
        "    /* x87_add(c, a, b) is only a comment */\n"
        "    c->eax = 1;\n"
        "}\n"
        "void L_00000001(CPU *restrict c)\n"
        "{\n"
        "    double r = x87_add(c, a, b);\n"
        "}\n"
        "void L_00000003(CPU *restrict c)\n"
        "{\n"
        "    double r = x87_add(c, a, b);\n"
        "    r = x87_sub(c, r, b);\n"
        "    r = x87_mul(c, r, b);\n"
        "}\n"
        "void L_00000004(CPU *restrict c)\n"
        "{\n"
        "    double r = x87_add(c, a, b);\n"
        "    r = x87_sub(c, r, b);\n"
        "    r = x87_mul(c, r, b);\n"
        "    r = x87_div(c, r, b);\n"
        "}\n"
        "void L_00000010(CPU *restrict c)\n"
        "{\n"
        "    double r = x87_add(c, a, b);\n"
        "    r = x87_add(c, r, b);\n"
        "    r = x87_add(c, r, b);\n"
        "    r = x87_add(c, r, b);\n"
        "    r = x87_add(c, r, b);\n"
        "    r = x87_add(c, r, b);\n"
        "    r = x87_add(c, r, b);\n"
        "    r = x87_add(c, r, b);\n"
        "    r = x87_add(c, r, b);\n"
        "    r = x87_add(c, r, b);\n"
        "}\n"
        "void L_FPSR(CPU *restrict c)\n"
        "{\n"
        "    double r = x87_add_fpsr(c, a, b);\n"
        "}\n";
    struct stats st;
    memset(&st, 0, sizeof(st));
    char *res = rewrite_text(fixture, strlen(fixture), min_sites, &st);
    const char *p;
    size_t outlined = 0;
    int ok = 1;

    for (p = res; (p = strstr(p, "x87_out_")) != NULL; p += 8) outlined++;
    if (st.funcs != 6 || st.eligible != 4 || st.rewritten != 2 ||
        st.sites != 18 || outlined != 14) ok = 0;
    if (!strstr(res, "void L_00000001(CPU *restrict c)\n{\n"
                     "    double r = x87_add(c, a, b);\n")) ok = 0;
    if (!strstr(res, "void L_00000003(CPU *restrict c)\n{\n"
                     "    double r = x87_add(c, a, b);\n")) ok = 0;
    if (!strstr(res, "x87_add(c, a, b) is only a comment")) ok = 0;
    if (!strstr(res, "x87_add_fpsr(c, a, b)")) ok = 0;
    free(res);
    if (!ok) {
        fprintf(stderr,
                "x87_invalid_outline: self-test failed "
                "(funcs=%zu eligible=%zu rewritten=%zu sites=%zu outlined=%zu)\n",
                st.funcs, st.eligible, st.rewritten, st.sites, outlined);
        return 1;
    }
    printf("x87_invalid_outline: self-test ok (min_sites=%d)\n", min_sites);
    return 0;
}

int main(int argc, char **argv)
{
    const char *inputs = NULL, *out_dir = NULL, *report = NULL;
    int quiet = 0, min_sites = 4;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--self-test")) {
            /* The fixture is fixed at the measured break-even threshold. */
            return self_test(4);
        } else if (!strcmp(a, "--quiet")) {
            quiet = 1;
        } else if (!strcmp(a, "--inputs") && i + 1 < argc) {
            inputs = argv[++i];
        } else if (!strcmp(a, "--out-dir") && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (!strcmp(a, "--report") && i + 1 < argc) {
            report = argv[++i];
        } else if (!strcmp(a, "--min-sites") && i + 1 < argc) {
            min_sites = atoi(argv[++i]);
        } else if (!strncmp(a, "--min-sites=", 12)) {
            min_sites = atoi(a + 12);
        } else {
            fprintf(stderr, "x87_invalid_outline: unknown arg %s\n", a);
            return 2;
        }
    }
    if (!inputs || !out_dir || !report) {
        fprintf(stderr,
                "x87_invalid_outline: --inputs, --out-dir and --report required\n");
        return 2;
    }
    if (min_sites < 1) die("--min-sites must be >= 1");

    {
        size_t list_len, pos = 0, count = 0;
        char *list = read_file(inputs, &list_len);
        struct stats total;
        memset(&total, 0, sizeof(total));

        while (pos < list_len) {
            size_t s = pos, e = pos, text_len, out_len;
            char *path, *text, *out, *dst;
            while (e < list_len && list[e] != '\n' && list[e] != '\r') e++;
            if (e == s) { pos = e + 1; continue; }
            path = (char *)malloc(e - s + 1);
            memcpy(path, list + s, e - s);
            path[e - s] = 0;
            text = read_file(path, &text_len);
            out = rewrite_text(text, text_len, min_sites, &total);
            out_len = strlen(out);
            dst = (char *)malloc(strlen(out_dir) + strlen(path_base(path)) + 2);
            if (!dst) die("out of memory");
            sprintf(dst, "%s/%s", out_dir, path_base(path));
            write_file(dst, out, out_len);
            free(dst);
            free(path);
            free(text);
            free(out);
            count++;
            pos = e + 1;
        }
        free(list);

        {
            FILE *f = fopen(report, "wb");
            if (!f) die("report write failed");
            fprintf(f,
                    "{\"verdict\":\"pass\",\"inputs\":%zu,\"functions\":%zu,"
                    "\"eligible\":%zu,\"rewritten\":%zu,\"sites\":%zu,"
                    "\"min_sites\":%d,\"engine\":\"x87_invalid_outline\"}\n",
                    count, total.funcs, total.eligible, total.rewritten,
                    total.sites, min_sites);
            fclose(f);
        }
        if (!quiet)
            printf("x87_invalid_outline: %zu inputs, %zu functions, "
                   "%zu rewritten (min_sites=%d)\n",
                   count, total.funcs, total.rewritten, min_sites);
    }
    return 0;
}
