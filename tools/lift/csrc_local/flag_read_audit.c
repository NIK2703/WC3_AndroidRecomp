/* flag_read_audit.c - conservative textual audit for live x86 PF/AF reads.
 *
 * P0-10 may skip parity and AF arithmetic only when no lifted instruction
 * observes PF or AF.  This tool scans generated C for direct c->pf/c->af
 * reads, eflags_pack()/lahf, and SETcc forms that consume PF.  It is
 * deliberately textual: a construct hidden behind a macro is reported as a
 * risk rather than assumed safe.
 *
 * Usage:
 *   flag_read_audit --lift build/.../game/closed.c
 *   flag_read_audit --in-dir build/.../game --json report.json
 *   flag_read_audit --self-test
 */
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
#  include <direct.h>
#  include <windows.h>
#  define WC3_MKDIR(p) _mkdir(p)
#else
#  include <dirent.h>
#  include <unistd.h>
#  define WC3_MKDIR(p) mkdir((p), 0777)
#endif

#define WC3_PATH_MAX 4096

typedef struct {
    char *file;
    char *va;
    int line;
    char *kind;
    char *text;
} finding;

typedef struct {
    char *file;
    int functions;
    finding *findings;
    size_t nfindings;
    size_t capfindings;
} report;

typedef struct {
    char **v;
    size_t n;
    size_t cap;
} strvec;

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "flag_read_audit: out of memory\n");
        exit(2);
    }
    return p;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static char *xstrndup(const char *s, size_t n)
{
    char *p = (char *)xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static void sv_push(strvec *sv, char *s)
{
    if (sv->n == sv->cap) {
        size_t cap = sv->cap ? sv->cap * 2 : 32;
        char **v = (char **)realloc(sv->v, cap * sizeof(*v));
        if (!v) {
            fprintf(stderr, "flag_read_audit: out of memory\n");
            exit(2);
        }
        sv->v = v;
        sv->cap = cap;
    }
    sv->v[sv->n++] = s;
}

static void sv_free(strvec *sv)
{
    size_t i;
    for (i = 0; i < sv->n; i++) free(sv->v[i]);
    free(sv->v);
    sv->v = NULL;
    sv->n = sv->cap = 0;
}

static int cmp_str(const void *a, const void *b)
{
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

static int read_file(const char *path, char **out)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *data;
    if (!f) {
        fprintf(stderr, "flag_read_audit: cannot open %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    data = (char *)xmalloc((size_t)size + 1);
    if (size > 0 && fread(data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(data);
        return -1;
    }
    fclose(f);
    data[size] = 0;
    *out = data;
    return 0;
}

static char *mask_code(const char *line)
{
    size_t n = strlen(line);
    char *out = xstrdup(line);
    size_t i = 0;
    while (i < n) {
        char c = line[i];
        if (c == '/' && i + 1 < n && line[i + 1] == '*') {
            size_t end;
            const char *close = strstr(line + i + 2, "*/");
            end = close ? (size_t)(close - line) + 2 : n;
            while (i < end) {
                if (out[i] != '\n') out[i] = ' ';
                i++;
            }
        } else if (c == '/' && i + 1 < n && line[i + 1] == '/') {
            size_t end = n;
            while (i < end) {
                out[i] = ' ';
                i++;
            }
        } else if (c == '"' || c == '\'') {
            char quote = c;
            size_t j = i + 1;
            while (j < n) {
                if (line[j] == '\\') {
                    j += 2;
                    continue;
                }
                if (line[j] == quote) {
                    j++;
                    break;
                }
                j++;
            }
            {
                size_t k;
                for (k = i; k < j && k < n; k++) out[k] = ' ';
            }
            i = j;
        } else {
            i++;
        }
    }
    return out;
}

static int is_word(unsigned char c)
{
    return isalnum(c) || c == '_';
}

static size_t uppercase_hex(char *dst, const char *src)
{
    size_t i;
    for (i = 0; i < 8; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[8] = 0;
    return 8;
}

#if 0
/* Kept as a reference for the textual rules; the scanner uses explicit
 * boundary checks instead of a regex library. */
#endif

static void add_finding(report *r, const char *file, const char *va,
                        int line, const char *kind, const char *text)
{
    finding *f;
    if (r->nfindings == r->capfindings) {
        size_t cap = r->capfindings ? r->capfindings * 2 : 16;
        finding *v = (finding *)realloc(r->findings, cap * sizeof(*v));
        if (!v) {
            fprintf(stderr, "flag_read_audit: out of memory\n");
            exit(2);
        }
        r->findings = v;
        r->capfindings = cap;
    }
    f = &r->findings[r->nfindings++];
    f->file = xstrdup(file);
    f->va = va ? xstrdup(va) : NULL;
    f->line = line;
    f->kind = xstrdup(kind);
    f->text = xstrdup(text);
}

static int assignment_write(const char *p)
{
    /* p points at c->pf or c->af.  A plain '=' is a write; '+=' and '==' are
     * live reads and must be reported. */
    p += 5;
    while (*p && isspace((unsigned char)*p)) p++;
    return *p == '=' && p[1] != '=';
}

static void scan_line(report *r, const char *file, const char *va, int line_no,
                      const char *plain, const char *original)
{
    size_t n = strlen(plain);
    size_t i;
    char *text = xstrdup(original);
    size_t tlen = strlen(text);
    while (tlen > 0 && (text[tlen - 1] == '\n' || text[tlen - 1] == '\r')) {
        text[--tlen] = 0;
    }

    /* c->pf / c->af */
    for (i = 0; i + 4 < n; i++) {
        if (plain[i] == 'c' && plain[i + 1] == '-' && plain[i + 2] == '>' &&
            (plain[i + 3] == 'p' || plain[i + 3] == 'a') &&
            plain[i + 4] == 'f' &&
            (i + 5 >= n || !is_word((unsigned char)plain[i + 5])) &&
            (i == 0 || !is_word((unsigned char)plain[i - 1]))) {
            const char *p = plain + i;
            if (assignment_write(p)) continue;
            add_finding(r, file, va, line_no,
                        plain[i + 3] == 'p' ? "c->pf" : "c->af", text);
        }
    }

    /* Bare pf/af tokens, including accesses through another CPU pointer. */
    for (i = 0; i + 1 < n; i++) {
        if ((plain[i] == 'p' || plain[i] == 'a') && plain[i + 1] == 'f' &&
            (i == 0 || !is_word((unsigned char)plain[i - 1])) &&
            (i + 2 >= n || !is_word((unsigned char)plain[i + 2]))) {
            if (i >= 3 && plain[i - 3] == 'c' && plain[i - 2] == '-' &&
                plain[i - 1] == '>') {
                continue;
            }
            add_finding(r, file, va, line_no,
                        plain[i] == 'p' ? "pf" : "af", text);
        }
    }

    /* eflags_pack() and lahf() */
    for (i = 0; i < n; i++) {
        size_t len = 0;
        if (strncmp(plain + i, "eflags_pack", 11) == 0 &&
            (i == 0 || !is_word((unsigned char)plain[i - 1]))) {
            len = 11;
        } else if (strncmp(plain + i, "lahf", 4) == 0 &&
                   (i == 0 || !is_word((unsigned char)plain[i - 1]))) {
            len = 4;
        }
        if (len) {
            size_t j = i + len;
            size_t kind_end;
            if (j < n && is_word((unsigned char)plain[j])) continue;
            while (j < n && isspace((unsigned char)plain[j])) j++;
            if (j < n && plain[j] == '(') {
                kind_end = j;
                add_finding(r, file, va, line_no,
                            xstrndup(plain + i, kind_end - i), text);
            }
        }
    }

    /* SETP/SETPE/SETNP/SETNPE */
    for (i = 0; i + 3 < n; i++) {
        const char *name = NULL;
        size_t len = 0;
        if (strncmp(plain + i, "SETNPE", 6) == 0) {
            name = "SETNPE";
            len = 6;
        } else if (strncmp(plain + i, "SETNP", 5) == 0) {
            name = "SETNP";
            len = 5;
        } else if (strncmp(plain + i, "SETPE", 5) == 0) {
            name = "SETPE";
            len = 5;
        } else if (strncmp(plain + i, "SETP", 4) == 0) {
            name = "SETP";
            len = 4;
        }
        if (name && (i == 0 || !is_word((unsigned char)plain[i - 1])) &&
            (i + len >= n || !is_word((unsigned char)plain[i + len]))) {
            add_finding(r, file, va, line_no, name, text);
        }
    }
    free(text);
}

static int parse_header(const char *line, char va[9], const char **after)
{
    const char *p = line;
    int i;
    if (strncmp(p, "void", 4) != 0 || !isspace((unsigned char)p[4])) return 0;
    p += 4;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p[0] != 'L' || p[1] != '_') return 0;
    p += 2;
    for (i = 0; i < 8; i++) {
        if (!isxdigit((unsigned char)p[i])) return 0;
    }
    uppercase_hex(va, p);
    p += 8;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '(') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "CPU", 3) != 0) return 0;
    p += 3;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '*') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != 'c') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ')') return 0;
    p++;
    if (after) *after = p;
    return 1;
}

static char *display_path(const char *path)
{
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    char *out = xstrdup(path);
    char *p;
    for (p = out; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    return out;
#else
    return xstrdup(path);
#endif
}

static report *scan_text(const char *file, const char *text)
{
    report *r = (report *)calloc(1, sizeof(*r));
    const char *p = text;
    int line_no = 0;
    char *va = NULL;
    if (!r) {
        fprintf(stderr, "flag_read_audit: out of memory\n");
        exit(2);
    }
    r->file = xstrdup(file);
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char *line = xstrndup(p, len);
        char header_va[9];
        char *plain;
        line_no++;
        if (parse_header(line, header_va, NULL)) {
            if (!va) va = (char *)xmalloc(9);
            memcpy(va, header_va, 9);
            r->functions++;
        }
        plain = mask_code(line);
        scan_line(r, file, va, line_no, plain, line);
        free(plain);
        free(line);
        if (!end) break;
        p = end + 1;
    }
    free(va);
    return r;
}

static void report_free(report *r)
{
    size_t i;
    if (!r) return;
    for (i = 0; i < r->nfindings; i++) {
        free(r->findings[i].file);
        free(r->findings[i].va);
        free(r->findings[i].kind);
        free(r->findings[i].text);
    }
    free(r->findings);
    free(r->file);
    free(r);
}

static int self_test(void)
{
    static const char *clean =
        "void L_00000001(CPU *c)\n"
        "{\n"
        "    c->eax = flags_add(c, c->eax, 1u, 4);\n"
        "    c->pf = 0; c->af = 1;\n"
        "    return;\n"
        "}\n";
    static const char *dirty =
        "void L_00000002(CPU *c)\n"
        "{\n"
        "    if (c->pf) return;\n"
        "    if (c->af) return;\n"
        "    c->eax = eflags_pack(c);\n"
        "    return;\n"
        "}\n";
    report *r1 = scan_text("clean.c", clean);
    report *r2 = scan_text("dirty.c", dirty);
    int failed = 0;
    if (r1->nfindings != 0) {
        fprintf(stderr, "flag_read_audit self-test: clean fixture flagged\n");
        failed = 1;
    }
    if (r2->nfindings < 3) {
        fprintf(stderr,
                "flag_read_audit self-test: dirty findings=%zu, want >=3\n",
                r2->nfindings);
        failed = 1;
    }
    report_free(r1);
    report_free(r2);
    if (!failed) printf("flag_read_audit self-test: OK\n");
    return failed ? 1 : 0;
}

static int list_c_files(const char *dir, strvec *out)
{
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    char pattern[WC3_PATH_MAX];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof(pattern), "%s\\*.c", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            sv_push(out, xstrdup(fd.cFileName));
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *ent;
    if (!d) return -1;
    while ((ent = readdir(d)) != NULL) {
        size_t n = strlen(ent->d_name);
        if (n > 2 && strcmp(ent->d_name + n - 2, ".c") == 0) {
            sv_push(out, xstrdup(ent->d_name));
        }
    }
    closedir(d);
#endif
    qsort(out->v, out->n, sizeof(*out->v), cmp_str);
    return 0;
}

static char *json_escape(const char *s)
{
    size_t n = 0;
    const char *p;
    char *out;
    char *q;
    for (p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') n += 2;
        else if (c < 0x20) n += 6;
        else n++;
    }
    out = (char *)xmalloc(n + 1);
    q = out;
    for (p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            *q++ = '\\';
            *q++ = (char)c;
        } else if (c == '\n') {
            *q++ = '\\'; *q++ = 'n';
        } else if (c == '\r') {
            *q++ = '\\'; *q++ = 'r';
        } else if (c == '\t') {
            *q++ = '\\'; *q++ = 't';
        } else if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            *q++ = '\\'; *q++ = 'u'; *q++ = '0'; *q++ = '0';
            *q++ = hex[(c >> 4) & 0xf];
            *q++ = hex[c & 0xf];
        } else {
            *q++ = (char)c;
        }
    }
    *q = 0;
    return out;
}

static int make_parent_dirs(const char *path)
{
    char buf[WC3_PATH_MAX];
    size_t i;
    size_t n = strlen(path);
    if (n >= sizeof(buf)) return -1;
    memcpy(buf, path, n + 1);
    for (i = 1; i < n; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char save = buf[i];
            buf[i] = 0;
            if (buf[0] && !(i == 2 && buf[1] == ':')) {
                if (WC3_MKDIR(buf) != 0 && errno != EEXIST) return -1;
            }
            buf[i] = save;
        }
    }
    return 0;
}

static int write_json(const char *path, report **reports, size_t nreports,
                      size_t total)
{
    FILE *f;
    size_t i, j;
    if (make_parent_dirs(path) != 0) return -1;
    f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "{\n  \"schema\": 1,\n  \"status\": \"%s\",\n  \"files\": [\n",
            total ? "fail" : "pass");
    for (i = 0; i < nreports; i++) {
        char *file = json_escape(reports[i]->file);
        fprintf(f,
                "    {\n      \"file\": \"%s\",\n      \"functions\": %d,\n"
                "      \"findings\": %zu,\n      \"errors\": []\n    }%s\n",
                file, reports[i]->functions, reports[i]->nfindings,
                i + 1 < nreports ? "," : "");
        free(file);
    }
    fprintf(f, "  ],\n  \"findings\": [\n");
    {
        size_t written = 0;
        for (i = 0; i < nreports; i++) {
            for (j = 0; j < reports[i]->nfindings; j++) {
                finding *v = &reports[i]->findings[j];
                char *file = json_escape(v->file);
                char *va = v->va ? json_escape(v->va) : NULL;
                char *kind = json_escape(v->kind);
                char *text = json_escape(v->text);
                fprintf(f,
                        "    {\n      \"file\": \"%s\",\n      \"va\": %s%s%s,\n"
                        "      \"line\": %d,\n      \"kind\": \"%s\",\n"
                        "      \"text\": \"%s\"\n    }%s\n",
                        file, va ? "\"" : "", va ? va : "null",
                        va ? "\"" : "", v->line, kind, text,
                        ++written < total ? "," : "");
                free(file);
                free(va);
                free(kind);
                free(text);
            }
        }
    }
    fprintf(f, "  ],\n  \"total\": %zu\n}\n", total);
    if (fclose(f) != 0) return -1;
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: flag_read_audit (--lift FILE | --in-dir DIR) [--json FILE]\n"
            "       flag_read_audit --self-test\n");
}

int main(int argc, char **argv)
{
    const char *lift = NULL;
    const char *in_dir = NULL;
    const char *json = NULL;
    report **reports = NULL;
    size_t nreports = 0;
    size_t capreports = 0;
    size_t total = 0;
    size_t functions = 0;
    int i;
    int rc = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--self-test") == 0) {
            return self_test();
        } else if (strcmp(a, "--lift") == 0 && i + 1 < argc) {
            lift = argv[++i];
        } else if (strncmp(a, "--lift=", 7) == 0) {
            lift = a + 7;
        } else if (strcmp(a, "--in-dir") == 0 && i + 1 < argc) {
            in_dir = argv[++i];
        } else if (strncmp(a, "--in-dir=", 9) == 0) {
            in_dir = a + 9;
        } else if (strcmp(a, "--json") == 0 && i + 1 < argc) {
            json = argv[++i];
        } else if (strncmp(a, "--json=", 7) == 0) {
            json = a + 7;
        } else {
            usage();
            return 2;
        }
    }
    if (!lift && !in_dir) {
        usage();
        return 2;
    }

    if (lift) {
        char *text = NULL;
        char *display;
        report *r;
        if (read_file(lift, &text) != 0) return 2;
        display = display_path(lift);
        r = scan_text(display, text);
        free(display);
        free(text);
        reports = (report **)xmalloc(sizeof(*reports));
        reports[nreports++] = r;
        capreports = 1;
    } else {
        strvec files = {0};
        size_t k;
        if (list_c_files(in_dir, &files) != 0 || files.n == 0) {
            fprintf(stderr, "flag_read_audit: no .c files in %s\n", in_dir);
            sv_free(&files);
            return 2;
        }
        for (k = 0; k < files.n; k++) {
            char path[WC3_PATH_MAX];
            char *text = NULL;
            char *display;
            report *r;
            snprintf(path, sizeof(path), "%s/%s", in_dir, files.v[k]);
            if (read_file(path, &text) != 0) {
                sv_free(&files);
                rc = 2;
                goto done;
            }
            display = display_path(path);
            r = scan_text(display, text);
            free(display);
            free(text);
            if (nreports == capreports) {
                size_t cap = capreports ? capreports * 2 : 8;
                report **v = (report **)realloc(reports,
                                                cap * sizeof(*v));
                if (!v) {
                    fprintf(stderr, "flag_read_audit: out of memory\n");
                    exit(2);
                }
                reports = v;
                capreports = cap;
            }
            reports[nreports++] = r;
        }
        sv_free(&files);
    }

    for (i = 0; i < (int)nreports; i++) {
        total += reports[i]->nfindings;
        functions += (size_t)reports[i]->functions;
    }
    if (json && write_json(json, reports, nreports, total) != 0) {
        fprintf(stderr, "flag_read_audit: cannot write %s\n", json);
        rc = 2;
        goto done;
    }
    if (total) {
        size_t shown = 0;
        fprintf(stderr, "flag_read_audit: %zu live PF/AF read(s); "
                        "WC3_FLAGS_FAST_PF_AF must stay 0\n", total);
        for (i = 0; i < (int)nreports && shown < 20; i++) {
            size_t j;
            for (j = 0; j < reports[i]->nfindings && shown < 20; j++) {
                finding *v = &reports[i]->findings[j];
                fprintf(stderr, "%s:%d: %s: %s\n",
                        v->file, v->line, v->kind, v->text);
                shown++;
            }
        }
        rc = 1;
    } else {
        printf("flag_read_audit: pass (%zu functions)\n", functions);
    }

done:
    for (i = 0; i < (int)nreports; i++) report_free(reports[i]);
    free(reports);
    return rc;
}
