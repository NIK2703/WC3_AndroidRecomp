/* softfloat_context_fnclex.c - compiled bridge for old corpora.
 *
 * The current C lift emitter writes x87_fnclex(c) directly.  Corpora lifted
 * before that emitter change still contain
 *
 *     c->fpu_sw &= ~0x003Fu; softfloat_exceptionFlags = 0;
 *
 * In a WC3_SOFTFLOAT_CONTEXT build that clears the cold TLS fallback instead
 * of CPU.sf_state.  This tool rewrites the exact old statement and refuses to
 * pass if any direct softfloat_exceptionFlags reference remains.
 *
 * It replaces the retired Python pass byte-for-byte, including the
 * out-manifest and JSON report.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <unistd.h>
#define make_dir(p) mkdir((p), 0777)
#endif

static void die_oom(void)
{
    fputs("softfloat_context_fnclex: out of memory\n", stderr);
    exit(1);
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die_oom();
    return q;
}

typedef struct {
    char *p;
    size_t n;
    size_t cap;
} Buf;

static void buf_need(Buf *b, size_t extra)
{
    size_t need = b->n + extra;
    if (need < b->cap) return;
    if (b->cap == 0) b->cap = 4096;
    while (b->cap <= need) {
        if (b->cap > (size_t)-1 / 2) die_oom();
        b->cap *= 2;
    }
    b->p = (char *)xrealloc(b->p, b->cap);
    b->p[b->n] = 0;
}

static void buf_add(Buf *b, const void *p, size_t n)
{
    buf_need(b, n);
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void buf_str(Buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

static char *slurp(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *p;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    p = (char *)xrealloc(NULL, (size_t)sz + 1);
    if (fread(p, 1, (size_t)sz, f) != (size_t)sz) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    p[sz] = 0;
    *out_n = (size_t)sz;
    return p;
}

static int write_all(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    size_t put;
    if (!f) return -1;
    put = fwrite(data, 1, n, f);
    if (fclose(f) != 0 || put != n) return -1;
    return 0;
}

static int write_json_string(FILE *f, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (fputc('"', f) == EOF) return -1;
    while (*p) {
        switch (*p) {
        case '\\': if (fputs("\\\\", f) == EOF) return -1; break;
        case '"':  if (fputs("\\\"", f) == EOF) return -1; break;
        case '\n': if (fputs("\\n", f) == EOF) return -1; break;
        case '\r': if (fputs("\\r", f) == EOF) return -1; break;
        case '\t': if (fputs("\\t", f) == EOF) return -1; break;
        default:
            if (*p < 0x20) {
                if (fprintf(f, "\\u%04x", *p) < 0) return -1;
            } else if (fputc(*p, f) == EOF) return -1;
            break;
        }
        p++;
    }
    return fputc('"', f) == EOF ? -1 : 0;
}

static int starts_line_ws(const char *s, size_t p)
{
    if (p == 0) return 1;
    return s[p - 1] == '\n';
}

static int line_has_old_statement(const char *s, size_t n, size_t p,
                                  size_t *indent_end, size_t *stmt_end,
                                  size_t *line_end)
{
    size_t i = p;
    static const char stmt[] =
        "c->fpu_sw &= ~0x003Fu; softfloat_exceptionFlags = 0;";
    const size_t slen = sizeof(stmt) - 1;
    if (!starts_line_ws(s, p)) return 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i + slen > n || memcmp(s + i, stmt, slen) != 0) return 0;
    *indent_end = i;
    *stmt_end = i + slen;
    i += slen;
    while (i < n && s[i] != '\r' && s[i] != '\n') i++;
    *line_end = i;
    return 1;
}

/* Returns 1 on success, 0 on a remaining direct state reference. */
static int transform(const char *s, size_t n, Buf *out, size_t *sites,
                     size_t *bad_line)
{
    size_t i = 0;
    *sites = 0;
    while (i < n) {
        size_t ie, se, le;
        if (line_has_old_statement(s, n, i, &ie, &se, &le)) {
            buf_add(out, s + i, ie - i);
            buf_str(out, "x87_fnclex(c);");
            buf_add(out, s + se, le - se);
            i = le;
            (*sites)++;
            continue;
        }
        {
            size_t e = i;
            while (e < n && s[e] != '\n') e++;
            if (e < n) e++;
            buf_add(out, s + i, e - i);
            i = e;
        }
    }
    {
        const char *needle = "softfloat_exceptionFlags";
        const char *hit = NULL;
        size_t j;
        for (j = 0; j + 24 <= out->n; j++)
            if (memcmp(out->p + j, needle, 24) == 0) { hit = out->p + j; break; }
        if (hit) {
            size_t line = 1;
            for (j = 0; out->p + j < hit; j++)
                if (out->p[j] == '\n') line++;
            *bad_line = line;
            return 0;
        }
    }
    return 1;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)xrealloc(NULL, n);
    memcpy(p, s, n);
    return p;
}

static char *path_slash(const char *path)
{
#if defined(_WIN32)
    char *full = _fullpath(NULL, path, 0);
#else
    char *full = NULL;
    if (path[0] == '/') {
        full = xstrdup(path);
    } else {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))) {
            size_t dl = strlen(cwd);
            size_t pl = strlen(path);
            full = (char *)xrealloc(NULL, dl + pl + 2);
            memcpy(full, cwd, dl);
            full[dl] = '/';
            memcpy(full + dl + 1, path, pl + 1);
        }
    }
#endif
    char *p;
    if (!full) full = xstrdup(path);
    for (p = full; *p; p++)
        if (*p == '\\') *p = '/';
    return full;
}

static char *path_basename(const char *path)
{
    const char *p = path;
    const char *last = path;
    for (; *p; p++)
        if (*p == '/' || *p == '\\') last = p + 1;
    return xstrdup(last);
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static int mkdir_p(const char *path)
{
    char *tmp = xstrdup(path);
    char *p;
    for (p = tmp; *p; p++) {
        if ((*p == '/' || *p == '\\') && p != tmp && p[1]) {
            char c = *p;
            *p = 0;
            if (!is_dir(tmp)) make_dir(tmp);
            *p = c;
        }
    }
    if (!is_dir(tmp)) make_dir(tmp);
    if (!is_dir(tmp)) {
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

static int self_test(void)
{
    static const char sample[] =
        "void L_1(CPU *c)\n{\n"
        "    c->fpu_sw &= ~0x003Fu; softfloat_exceptionFlags = 0;"
        "         /* 1: fnclex */\n"
        "}\n";
    static const char bad[] =
        "void L_1(CPU *c) { softfloat_exceptionFlags = 0; }\n";
    Buf out = {0};
    size_t sites, line;
    int ok;
    if (!transform(sample, sizeof(sample) - 1, &out, &sites, &line) ||
        sites != 1 || strstr(out.p, "x87_fnclex(c);") == NULL) {
        fputs("softfloat_context_fnclex: self-test did not lower FNCLEX\n",
              stderr);
        free(out.p);
        return 1;
    }
    {
        Buf again = {0};
        size_t s2, l2;
        int r = transform(out.p, out.n, &again, &s2, &l2);
        ok = r && s2 == 0 && again.n == out.n &&
             memcmp(again.p, out.p, out.n) == 0;
        free(again.p);
    }
    if (!ok) {
        fputs("softfloat_context_fnclex: self-test is not idempotent\n", stderr);
        free(out.p);
        return 1;
    }
    {
        Buf reject = {0};
        size_t s3, l3;
        int r = transform(bad, sizeof(bad) - 1, &reject, &s3, &l3);
        free(reject.p);
        if (r) {
            fputs("softfloat_context_fnclex: self-test accepted a direct "
                  "state write\n", stderr);
            free(out.p);
            return 1;
        }
    }
    free(out.p);
    puts("softfloat_context_fnclex: self-test OK");
    return 0;
}

static void usage(void)
{
    fputs("usage: softfloat_context_fnclex --manifest FILE --out-dir DIR "
          "--out-manifest FILE [--json FILE]\n"
          "       softfloat_context_fnclex --self-test\n", stderr);
}

int main(int argc, char **argv)
{
    const char *manifest = NULL;
    const char *out_dir = NULL;
    const char *out_manifest = NULL;
    const char *json = NULL;
    char *manifest_text = NULL;
    size_t manifest_n = 0;
    Buf outputs = {0};
    size_t inputs = 0, changed = 0, sites_total = 0;
    size_t i, line_start;
    int rc = 0;

    for (i = 1; i < (size_t)argc; i++) {
        if (!strcmp(argv[i], "--self-test")) return self_test();
        else if (!strcmp(argv[i], "--manifest") && i + 1 < (size_t)argc)
            manifest = argv[++i];
        else if (!strcmp(argv[i], "--out-dir") && i + 1 < (size_t)argc)
            out_dir = argv[++i];
        else if (!strcmp(argv[i], "--out-manifest") && i + 1 < (size_t)argc)
            out_manifest = argv[++i];
        else if (!strcmp(argv[i], "--json") && i + 1 < (size_t)argc)
            json = argv[++i];
        else { usage(); return 2; }
    }
    if (!manifest || !out_dir || !out_manifest) {
        usage();
        return 2;
    }
    manifest_text = slurp(manifest, &manifest_n);
    if (!manifest_text) {
        fprintf(stderr, "softfloat_context_fnclex: cannot read %s\n", manifest);
        return 1;
    }
    if (mkdir_p(out_dir) != 0) {
        fprintf(stderr, "softfloat_context_fnclex: cannot create %s\n", out_dir);
        free(manifest_text);
        return 1;
    }
    {
        char *slash = path_slash(out_manifest);
        size_t n = strlen(slash);
        size_t cut = n;
        while (cut > 0 && slash[cut - 1] != '/' && slash[cut - 1] != '\\') cut--;
        if (cut) {
            char c = slash[cut];
            slash[cut] = 0;
            mkdir_p(slash);
            slash[cut] = c;
        }
        free(slash);
    }

    line_start = 0;
    for (i = 0; i <= manifest_n; i++) {
        if (i == manifest_n || manifest_text[i] == '\n') {
            size_t a = line_start, b = i;
            char *path;
            char *src_abs;
            char *data;
            size_t data_n;
            Buf out = {0};
            size_t sites = 0, bad_line = 0;

            while (a < b && (manifest_text[a] == ' ' || manifest_text[a] == '\t' ||
                             manifest_text[a] == '\r')) a++;
            while (b > a && (manifest_text[b - 1] == ' ' ||
                             manifest_text[b - 1] == '\t' ||
                             manifest_text[b - 1] == '\r')) b--;
            if (a != b) {
                path = (char *)xrealloc(NULL, b - a + 1);
                memcpy(path, manifest_text + a, b - a);
                path[b - a] = 0;
                inputs++;
                data = slurp(path, &data_n);
                if (!data) {
                    fprintf(stderr,
                            "softfloat_context_fnclex: cannot read %s\n", path);
                    free(path);
                    free(out.p);
                    rc = 1;
                    line_start = i + 1;
                    break;
                }
                if (!transform(data, data_n, &out, &sites, &bad_line)) {
                    fprintf(stderr,
                            "%s:%zu: direct softfloat_exceptionFlags remains "
                            "after FNCLEX lowering; extend the context pass "
                            "before building\n",
                            path, bad_line);
                    free(data);
                    free(path);
                    free(out.p);
                    rc = 1;
                    line_start = i + 1;
                    break;
                }
                if (sites) {
                    char name[32];
                    char *base = path_basename(path);
                    char *dst;
                    snprintf(name, sizeof(name), "%04zu_", inputs - 1);
                    dst = (char *)xrealloc(NULL, strlen(out_dir) + strlen(name) +
                                           strlen(base) + 3);
                    strcpy(dst, out_dir);
                    if (*dst && dst[strlen(dst) - 1] != '/' &&
                        dst[strlen(dst) - 1] != '\\')
                        strcat(dst, "/");
                    strcat(dst, name);
                    strcat(dst, base);
                    if (write_all(dst, out.p, out.n) != 0) {
                        fprintf(stderr,
                                "softfloat_context_fnclex: cannot write %s\n",
                                dst);
                        free(dst);
                        free(base);
                        free(data);
                        free(path);
                        free(out.p);
                        rc = 1;
                        line_start = i + 1;
                        break;
                    }
                    src_abs = path_slash(dst);
                    buf_add(&outputs, src_abs, strlen(src_abs));
                    buf_add(&outputs, "\n", 1);
                    free(src_abs);
                    free(dst);
                    free(base);
                    changed++;
                    sites_total += sites;
                } else {
                    src_abs = path_slash(path);
                    buf_add(&outputs, src_abs, strlen(src_abs));
                    buf_add(&outputs, "\n", 1);
                    free(src_abs);
                }
                free(data);
                free(path);
                free(out.p);
            }
            line_start = i + 1;
        }
    }
    free(manifest_text);
    if (rc == 0) {
        FILE *f = fopen(out_manifest, "wb");
        if (!f || fwrite(outputs.p, 1, outputs.n, f) != outputs.n ||
            fclose(f) != 0) {
            fprintf(stderr, "softfloat_context_fnclex: cannot write %s\n",
                    out_manifest);
            rc = 1;
        }
    }
    if (rc == 0 && json) {
        FILE *f;
        char *slash;
        slash = path_slash(json);
        {
            size_t n = strlen(slash);
            size_t cut = n;
            while (cut > 0 && slash[cut - 1] != '/' && slash[cut - 1] != '\\')
                cut--;
            if (cut) {
                char c = slash[cut];
                slash[cut] = 0;
                mkdir_p(slash);
                slash[cut] = c;
            }
        }
        free(slash);
        f = fopen(json, "wb");
        if (f) {
            char *out_abs = path_slash(out_manifest);
            fprintf(f,
                    "{\n  \"pass\": \"softfloat_context_fnclex\",\n"
                    "  \"rev\": 1,\n  \"inputs\": %zu,\n"
                    "  \"changed_files\": %zu,\n  \"replaced_sites\": %zu,\n"
                    "  \"out_manifest\": ",
                    inputs, changed, sites_total);
            write_json_string(f, out_abs);
            fputs("\n}\n", f);
            fclose(f);
            free(out_abs);
        }
    }
    if (rc == 0)
        printf("softfloat_context_fnclex: %zu translation units, %zu changed, "
               "%zu FNCLEX sites\n", inputs, changed, sites_total);
    free(outputs.p);
    return rc;
}
