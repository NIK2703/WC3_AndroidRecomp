/* audit_tls.c - audit the ELF TLS model of a built WC3 Android .so.
 *
 * This is the C implementation of the ELF TLS audit.  It is a
 * read-only check for O-51/O-52: it reports whether the final artifact uses
 * native ELF TLS or the emulated-TLS runtime, and separates prebuilt vendor
 * references (DXVK, libc++/libc++abi, GameActivity) from product references.
 *
 * Usage:
 *   audit_tls --so build/.../libwc3gameactivity.so
 *   audit_tls --so ... --expect native --json out.json
 *   audit_tls --so ... --no-calls
 *   audit_tls --self-test
 *
 * Exit codes: 0 = expectation met, 1 = expectation violated, 2 = tool/input
 * error.
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
#  include <process.h>
#  include <windows.h>
#  define WC3_MKDIR(p) _mkdir(p)
#  define WC3_POPEN _popen
#  define WC3_PCLOSE _pclose
#else
#  include <dirent.h>
#  include <unistd.h>
#  define WC3_MKDIR(p) mkdir((p), 0777)
#  define WC3_POPEN popen
#  define WC3_PCLOSE pclose
#endif

#include "sha256.h"

#define WC3_PATH_MAX 4096

#define EMUTLS_HELPER "__emutls_get_address"

typedef struct {
    char **v;
    size_t n;
    size_t cap;
} strvec;

typedef struct {
    char **v;
    size_t n;
    size_t cap;
} str_set;

typedef struct {
    char *name;
    char *address;
    char kind;
} symbol;

typedef struct {
    symbol *v;
    size_t n;
    size_t cap;
} symbol_list;

typedef struct {
    char *name;
    uint64_t size;
} section;

typedef struct {
    section *v;
    size_t n;
    size_t cap;
} section_list;

typedef struct {
    char *row;
    uint64_t *numbers;
    size_t nnumbers;
    int present;
} tls_segment;

typedef struct {
    char *caller;
    uint64_t calls;
    size_t seq;
} call_count;

typedef struct {
    char *caller;
    uint64_t calls;
    int vendor;
} top_caller;

typedef struct {
    int has_calls;
    uint64_t emutls_calls;
    uint64_t tls_get_addr_calls;
    uint64_t product_emutls_calls;
    uint64_t vendor_emutls_calls;
    call_count *counts;
    size_t ncounts;
    size_t capcounts;
    top_caller *top;
    size_t ntop;
} calls_result;

typedef struct {
    char *path;
    uint64_t refs;
} archive_ref;

typedef struct {
    char *artifact_path;
    uint64_t artifact_bytes;
    char artifact_sha256[65];
    const char *expect;
    const char *verdict;
    strvec failures;

    tls_segment segment;
    int has_tdata;
    uint64_t tdata_bytes;
    int has_tbss;
    uint64_t tbss_bytes;
    uint64_t total_bytes;
    strvec dynamic_tags;
    strvec tls_symbols;

    int helper_defined;
    strvec product_symbols;
    strvec vendor_symbols;
    size_t vendor_archive_symbols;
    archive_ref *refs;
    size_t nrefs;

    calls_result calls;

    char *nm;
    char *readelf;
    char *objdump;
} audit_report;

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "audit_tls: out of memory\n");
        exit(2);
    }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "audit_tls: out of memory\n");
        exit(2);
    }
    return q;
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

static void sv_push(strvec *v, char *s)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 32;
        v->v = (char **)xrealloc(v->v, v->cap * sizeof(*v->v));
    }
    v->v[v->n++] = s;
}

static void sv_push_copy(strvec *v, const char *s)
{
    sv_push(v, xstrdup(s));
}

static void sv_free(strvec *v)
{
    size_t i;
    for (i = 0; i < v->n; i++) free(v->v[i]);
    free(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

static void ss_push(str_set *s, const char *name)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->v = (char **)xrealloc(s->v, s->cap * sizeof(*s->v));
    }
    s->v[s->n++] = xstrdup(name);
}

static int cmp_str(const void *a, const void *b)
{
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

static int cmp_str_desc(const void *a, const void *b)
{
    return -cmp_str(a, b);
}

static void ss_finish(str_set *s)
{
    size_t i;
    size_t out;
    if (s->n > 1) qsort(s->v, s->n, sizeof(*s->v), cmp_str);
    out = 0;
    for (i = 0; i < s->n; i++) {
        if (out > 0 && strcmp(s->v[out - 1], s->v[i]) == 0) {
            free(s->v[i]);
        } else {
            s->v[out++] = s->v[i];
        }
    }
    s->n = out;
}

static int ss_contains(const str_set *s, const char *name)
{
    size_t lo = 0;
    size_t hi = s->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(s->v[mid], name);
        if (c == 0) return 1;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

static void ss_free(str_set *s)
{
    size_t i;
    for (i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

static char *path_join(const char *a, const char *b)
{
    size_t na = strlen(a);
    size_t nb = strlen(b);
    int sep = na > 0 && a[na - 1] != '/' && a[na - 1] != '\\';
    char *out = (char *)xmalloc(na + (size_t)sep + nb + 1);
    memcpy(out, a, na);
    if (sep) out[na++] = '/';
    memcpy(out + na, b, nb + 1);
    return out;
}

static char *to_backslashes(const char *s)
{
    char *out = xstrdup(s);
    char *p;
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    for (p = out; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#else
    (void)p;
#endif
    return out;
}

static char *path_basename(const char *path)
{
    size_t end = strlen(path);
    size_t start;
    while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\')) end--;
    start = end;
    while (start > 0 && path[start - 1] != '/' && path[start - 1] != '\\') start--;
    return xstrndup(path + start, end - start);
}

static int is_file(const char *path)
{
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static int is_dir(const char *path)
{
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int list_dirs(const char *dir, strvec *out)
{
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    char *pattern = path_join(dir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            strcmp(fd.cFileName, ".") != 0 &&
            strcmp(fd.cFileName, "..") != 0) {
            sv_push(out, xstrdup(fd.cFileName));
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
#else
    DIR *d = opendir(dir);
    struct dirent *ent;
    if (!d) return -1;
    while ((ent = readdir(d)) != NULL) {
        char *full;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        full = path_join(dir, ent->d_name);
        if (is_dir(full)) sv_push(out, xstrdup(ent->d_name));
        free(full);
    }
    closedir(d);
    return 0;
#endif
}

static char *path_resolve(const char *path)
{
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    char buf[WC3_PATH_MAX];
    DWORD n = GetFullPathNameA(path, (DWORD)sizeof(buf), buf, NULL);
    if (n == 0 || n >= sizeof(buf)) return xstrdup(path);
    return to_backslashes(buf);
#else
    char buf[WC3_PATH_MAX];
    if (!realpath(path, buf)) return xstrdup(path);
    return xstrdup(buf);
#endif
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
            if (buf[0] && !(i == 2 && buf[1] == ':') && !is_dir(buf)) {
                if (WC3_MKDIR(buf) != 0 && errno != EEXIST) return -1;
            }
            buf[i] = save;
        }
    }
    return 0;
}

static uint64_t file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    size = ftell(f);
    fclose(f);
    return size < 0 ? 0 : (uint64_t)size;
}

static char *quote_arg(const char *s)
{
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    size_t i;
    size_t extra = 2;
    char *out;
    char *p;
    for (i = 0; s[i]; i++) {
        if (s[i] == '"') extra++;
    }
    out = (char *)xmalloc(strlen(s) + extra + 1);
    p = out;
    *p++ = '"';
    for (i = 0; s[i]; i++) {
        if (s[i] == '"') *p++ = '\\';
        *p++ = s[i];
    }
    *p++ = '"';
    *p = 0;
    return out;
#else
    size_t i;
    size_t extra = 2;
    char *out;
    char *p;
    for (i = 0; s[i]; i++) {
        if (s[i] == '\'') extra += 3;
    }
    out = (char *)xmalloc(strlen(s) + extra + 1);
    p = out;
    *p++ = '\'';
    for (i = 0; s[i]; i++) {
        if (s[i] == '\'') {
            memcpy(p, "'\\''", 4);
            p += 4;
        } else {
            *p++ = s[i];
        }
    }
    *p++ = '\'';
    *p = 0;
    return out;
#endif
}

static char *build_command(char *const argv[])
{
    size_t cap = 256;
    size_t len = 0;
    char *cmd = (char *)xmalloc(cap);
    int i;
    cmd[0] = 0;
    for (i = 0; argv[i]; i++) {
        char *q = quote_arg(argv[i]);
        size_t n = strlen(q);
        while (len + n + 2 > cap) {
            cap *= 2;
            cmd = (char *)xrealloc(cmd, cap);
        }
        if (len) cmd[len++] = ' ';
        memcpy(cmd + len, q, n + 1);
        len += n;
        free(q);
    }
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    {
        char *wrapped = (char *)xmalloc(len + 3);
        wrapped[0] = '"';
        memcpy(wrapped + 1, cmd, len);
        wrapped[len + 1] = '"';
        wrapped[len + 2] = 0;
        free(cmd);
        cmd = wrapped;
    }
#endif
    return cmd;
}

static char *capture_argv(char *const argv[])
{
    char *cmd = build_command(argv);
    FILE *f = WC3_POPEN(cmd, "r");
    char *out = NULL;
    size_t n = 0;
    size_t cap = 0;
    char chunk[8192];
    size_t got;
    if (!f) {
        fprintf(stderr, "audit_tls: popen failed: %s\n", cmd);
        free(cmd);
        return NULL;
    }
    free(cmd);
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (n + got + 1 > cap) {
            size_t next = cap ? cap * 2 : 8192;
            while (next < n + got + 1) next *= 2;
            out = (char *)xrealloc(out, next);
            cap = next;
        }
        memcpy(out + n, chunk, got);
        n += got;
    }
    if (out) out[n] = 0;
    if (WC3_PCLOSE(f) != 0) {
        free(out);
        return NULL;
    }
    if (!out) out = xstrdup("");
    return out;
}

static char *run_argv(char *const argv[])
{
    char *out = capture_argv(argv);
    if (!out) {
        char *base = path_basename(argv[0]);
        fprintf(stderr, "audit_tls: %s failed\n", base);
        free(base);
    }
    return out;
}

static char *trim_line(char *s)
{
    size_t n;
    while (*s && isspace((unsigned char)*s)) s++;
    n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

static int is_word(unsigned char c)
{
    return isalnum(c) || c == '_';
}

static int is_hex_string(const char *s, size_t n)
{
    size_t i;
    if (n == 0) return 0;
    for (i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)s[i])) return 0;
    }
    return 1;
}

/* Parse "0000000000000001 T name" and "                 U name".
 * The archive form ("---------------- T name") is handled separately. */
static int parse_nm_line(const char *line, char **address, char *kind,
                         char **name)
{
    const char *p = line;
    const char *addr_start = NULL;
    size_t addr_len = 0;
    const char *name_start;
    size_t name_len;

    while (*p && isspace((unsigned char)*p)) p++;
    if (isxdigit((unsigned char)*p)) {
        const char *q = p;
        while (isxdigit((unsigned char)*q)) q++;
        if (*q && isspace((unsigned char)*q)) {
            addr_start = p;
            addr_len = (size_t)(q - p);
            p = q;
            while (*p && isspace((unsigned char)*p)) p++;
        }
    }
    if (!(*p && (isalpha((unsigned char)*p) || *p == '?'))) return 0;
    *kind = *p++;
    if (!(*p && isspace((unsigned char)*p))) return 0;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return 0;
    name_start = p;
    name_len = strlen(p);
    while (name_len > 0 && isspace((unsigned char)name_start[name_len - 1]))
        name_len--;
    if (name_len == 0) return 0;
    *address = addr_start ? xstrndup(addr_start, addr_len) : NULL;
    *name = xstrndup(name_start, name_len);
    return 1;
}

static int parse_nm_archive_line(const char *line, char *kind, char **name)
{
    const char *p = line;
    const char *name_start;
    size_t name_len;
    if (*p != '-') return 0;
    while (*p == '-') p++;
    if (!(*p && isspace((unsigned char)*p))) return 0;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!(*p && (isalpha((unsigned char)*p) || *p == '?'))) return 0;
    *kind = *p++;
    if (!(*p && isspace((unsigned char)*p))) return 0;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return 0;
    name_start = p;
    name_len = strlen(p);
    while (name_len > 0 && isspace((unsigned char)name_start[name_len - 1]))
        name_len--;
    if (name_len == 0) return 0;
    *name = xstrndup(name_start, name_len);
    return 1;
}

static void symbol_add(symbol_list *v, char *address, char kind, char *name)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 128;
        v->v = (symbol *)xrealloc(v->v, v->cap * sizeof(*v->v));
    }
    v->v[v->n].address = address;
    v->v[v->n].kind = kind;
    v->v[v->n].name = name;
    v->n++;
}

static void symbol_list_free(symbol_list *v)
{
    size_t i;
    for (i = 0; i < v->n; i++) {
        free(v->v[i].address);
        free(v->v[i].name);
    }
    free(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

static void parse_symbols(const char *text, symbol_list *out)
{
    const char *p = text;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char *line = xstrndup(p, len);
        char *s = line;
        char *address = NULL;
        char *name = NULL;
        char kind = 0;
        while (*s) s++;
        if (len && line[len - 1] == '\r') line[len - 1] = 0;
        if (parse_nm_line(line, &address, &kind, &name) ||
            parse_nm_archive_line(line, &kind, &name)) {
            char *at = strchr(name, '@');
            if (at) {
                char *base = xstrndup(name, (size_t)(at - name));
                free(name);
                name = base;
            }
            symbol_add(out, address, kind, name);
        } else {
            free(address);
            free(name);
        }
        free(line);
        if (!end) break;
        p = end + 1;
    }
}

static uint64_t parse_hex_u64(const char *s, size_t n)
{
    uint64_t v = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint64_t)(c - 'A' + 10);
    }
    return v;
}

static void section_add(section_list *v, const char *name, uint64_t size)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 32;
        v->v = (section *)xrealloc(v->v, v->cap * sizeof(*v->v));
    }
    v->v[v->n].name = xstrdup(name);
    v->v[v->n].size = size;
    v->n++;
}

static void section_list_free(section_list *v)
{
    size_t i;
    for (i = 0; i < v->n; i++) free(v->v[i].name);
    free(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

static void parse_sections(const char *text, section_list *out)
{
    const char *p = text;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char *line = xstrndup(p, len);
        const char *q = line;
        const char *name_start;
        size_t name_len;
        const char *size_start;
        size_t size_len;
        size_t i;

        if (len && line[len - 1] == '\r') line[len - 1] = 0;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q != '[') goto next;
        q++;
        while (*q && isspace((unsigned char)*q)) q++;
        if (!isdigit((unsigned char)*q)) goto next;
        while (isdigit((unsigned char)*q)) q++;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q != ']') goto next;
        q++;
        while (*q && isspace((unsigned char)*q)) q++;
        name_start = q;
        while (*q && !isspace((unsigned char)*q)) q++;
        name_len = (size_t)(q - name_start);
        if (name_len == 0) goto next;
        while (*q && isspace((unsigned char)*q)) q++;
        while (*q && !isspace((unsigned char)*q)) q++; /* type */
        while (*q && isspace((unsigned char)*q)) q++;
        for (i = 0; i < 2; i++) {
            const char *start = q;
            while (isxdigit((unsigned char)*q)) q++;
            if (q == start) goto next;
            while (*q && isspace((unsigned char)*q)) q++;
        }
        size_start = q;
        while (isxdigit((unsigned char)*q)) q++;
        size_len = (size_t)(q - size_start);
        if (size_len == 0) goto next;
        if (*q && !isspace((unsigned char)*q)) goto next;
        {
            char *name = xstrndup(name_start, name_len);
            section_add(out, name, parse_hex_u64(size_start, size_len));
            free(name);
        }
next:
        free(line);
        if (!end) break;
        p = end + 1;
    }
}

static void parse_program_headers(const char *text, tls_segment *out)
{
    const char *p = text;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char *line = xstrndup(p, len);
        char *row_copy;
        char *row_trim;
        char *fields[128];
        size_t nfields = 0;
        char *q;
        if (len && line[len - 1] == '\r') line[len - 1] = 0;
        row_copy = xstrdup(line);
        row_trim = trim_line(row_copy);
        q = line;
        while (*q) {
            while (*q && isspace((unsigned char)*q)) q++;
            if (!*q) break;
            if (nfields < sizeof(fields) / sizeof(fields[0])) {
                fields[nfields++] = q;
            }
            while (*q && !isspace((unsigned char)*q)) q++;
            if (*q) *q++ = 0;
        }
        if (nfields > 0 && strcmp(fields[0], "TLS") == 0) {
            uint64_t *numbers = NULL;
            size_t nnumbers = 0;
            size_t cap = 0;
            size_t i;
            for (i = 1; i < nfields; i++) {
                size_t n = strlen(fields[i]);
                if (n >= 3 && fields[i][0] == '0' &&
                    (fields[i][1] == 'x' || fields[i][1] == 'X') &&
                    is_hex_string(fields[i] + 2, n - 2)) {
                    if (nnumbers == cap) {
                        cap = cap ? cap * 2 : 8;
                        numbers = (uint64_t *)xrealloc(
                            numbers, cap * sizeof(*numbers));
                    }
                    numbers[nnumbers++] = parse_hex_u64(fields[i] + 2, n - 2);
                }
            }
            free(out->numbers);
            free(out->row);
            out->numbers = numbers;
            out->nnumbers = nnumbers;
            out->row = xstrdup(row_trim);
            out->present = 1;
        }
        free(row_copy);
        free(line);
        if (!end) break;
        p = end + 1;
    }
}

static int contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    const char *p;
    if (n == 0) return 1;
    for (p = hay; *p; p++) {
        size_t i;
        for (i = 0; i < n; i++) {
            if (!p[i] || tolower((unsigned char)p[i]) !=
                        tolower((unsigned char)needle[i])) break;
        }
        if (i == n) return 1;
    }
    return 0;
}

static void parse_dynamic(const char *text, strvec *out)
{
    const char *p = text;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char *line = xstrndup(p, len);
        if (len && line[len - 1] == '\r') line[len - 1] = 0;
        if (contains_ci(line, "TLS") || contains_ci(line, "TLSDESC")) {
            sv_push(out, xstrdup(trim_line(line)));
        }
        free(line);
        if (!end) break;
        p = end + 1;
    }
}

static int is_vendor(const char *name)
{
    static const char *const markers[] = {
        "wc3_dxvk", "dxbc", "spirv", "libdxvk"
    };
    size_t i;
    char *lower = xstrdup(name);
    int result = 0;
    for (i = 0; lower[i]; i++) lower[i] = (char)tolower((unsigned char)lower[i]);
    for (i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        if (strstr(lower, markers[i])) {
            result = 1;
            break;
        }
    }
    free(lower);
    return result;
}

static void find_in_ndk(const char *ndk, const char *name, char **result)
{
    char *prebuilt = path_join(ndk, "toolchains/llvm/prebuilt");
    strvec hosts = {0};
    size_t i;
    if (!is_dir(prebuilt)) {
        free(prebuilt);
        return;
    }
    if (list_dirs(prebuilt, &hosts) != 0) {
        free(prebuilt);
        return;
    }
    for (i = 0; i < hosts.n && !*result; i++) {
        char *host = path_join(prebuilt, hosts.v[i]);
        char *bin = path_join(host, "bin");
        char *tool = path_join(bin, name);
        if (is_file(tool)) *result = to_backslashes(tool);
        free(tool);
        free(bin);
        free(host);
    }
    sv_free(&hosts);
    free(prebuilt);
}

static char *find_tool(const char *name, const char *explicit_path)
{
    strvec ndks = {0};
    const char *env;
    size_t i;
    char *found = NULL;

    if (explicit_path && *explicit_path) return xstrdup(explicit_path);

    env = getenv("ANDROID_NDK_HOME");
    if (env && *env) sv_push_copy(&ndks, env);
    env = getenv("ANDROID_NDK_ROOT");
    if (env && *env) sv_push_copy(&ndks, env);

    {
        const char *sdk_env[] = {"ANDROID_SDK_ROOT", "ANDROID_HOME"};
        for (i = 0; i < sizeof(sdk_env) / sizeof(sdk_env[0]); i++) {
            env = getenv(sdk_env[i]);
            if (env && *env) {
                char *ndk = path_join(env, "ndk");
                strvec dirs = {0};
                if (is_dir(ndk) && list_dirs(ndk, &dirs) == 0) {
                    if (dirs.n > 1)
                        qsort(dirs.v, dirs.n, sizeof(*dirs.v), cmp_str_desc);
                    {
                        size_t k;
                        for (k = 0; k < dirs.n; k++) {
                            char *full = path_join(ndk, dirs.v[k]);
                            sv_push(&ndks, full);
                        }
                    }
                }
                sv_free(&dirs);
                free(ndk);
            }
        }
    }
    env = getenv("LOCALAPPDATA");
    if (env && *env) {
        char *sdk = path_join(env, "Android/Sdk");
        char *ndk = path_join(sdk, "ndk");
        strvec dirs = {0};
        if (is_dir(ndk) && list_dirs(ndk, &dirs) == 0) {
            if (dirs.n > 1) qsort(dirs.v, dirs.n, sizeof(*dirs.v), cmp_str_desc);
            {
                size_t k;
                for (k = 0; k < dirs.n; k++) {
                    char *full = path_join(ndk, dirs.v[k]);
                    sv_push(&ndks, full);
                }
            }
        }
        sv_free(&dirs);
        free(ndk);
        free(sdk);
    }
    {
        const char *fallback = "E:/Android/Sdk";
        char *ndk = path_join(fallback, "ndk");
        strvec dirs = {0};
        if (is_dir(ndk) && list_dirs(ndk, &dirs) == 0) {
            if (dirs.n > 1) qsort(dirs.v, dirs.n, sizeof(*dirs.v), cmp_str_desc);
            {
                size_t k;
                for (k = 0; k < dirs.n; k++) {
                    char *full = path_join(ndk, dirs.v[k]);
                    sv_push(&ndks, full);
                }
            }
        }
        sv_free(&dirs);
        free(ndk);
    }

    for (i = 0; i < ndks.n && !found; i++) {
        find_in_ndk(ndks.v[i], name, &found);
    }
    sv_free(&ndks);
    if (found) return found;

#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    {
        char buf[WC3_PATH_MAX];
        DWORD n = SearchPathA(NULL, name, NULL, (DWORD)sizeof(buf), buf, NULL);
        if (n > 0 && n < sizeof(buf)) return to_backslashes(buf);
    }
#else
    {
        const char *path = getenv("PATH");
        if (path) {
            char *copy = xstrdup(path);
            char *save = copy;
            char *part;
            while ((part = strchr(save, ':')) != NULL) {
                *part = 0;
                if (*save) {
                    char *cand = path_join(save, name);
                    if (is_file(cand)) {
                        free(copy);
                        return cand;
                    }
                    free(cand);
                }
                save = part + 1;
            }
            if (*save) {
                char *cand = path_join(save, name);
                if (is_file(cand)) {
                    free(copy);
                    return cand;
                }
                free(cand);
            }
            free(copy);
        }
    }
#endif
    return NULL;
}

/* ioctl-free stdio line helper for streaming objdump output. */
static int strip_eol(char *s)
{
    size_t n = strlen(s);
    int stripped = 0;
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = 0;
        stripped = 1;
    }
    return stripped;
}

static int parse_objdump_symbol(const char *line, char **name)
{
    const char *p = line;
    const char *start;
    if (!isxdigit((unsigned char)*p)) return 0;
    while (isxdigit((unsigned char)*p)) p++;
    if (!(*p && isspace((unsigned char)*p))) return 0;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '<') return 0;
    p++;
    start = p;
    while (*p && *p != '>') p++;
    if (*p != '>') return 0;
    *name = xstrndup(start, (size_t)(p - start));
    p++;
    if (*p != ':') {
        free(*name);
        *name = NULL;
        return 0;
    }
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p) {
        free(*name);
        *name = NULL;
        return 0;
    }
    return 1;
}

static void call_count_add(calls_result *calls, const char *caller)
{
    size_t i;
    for (i = 0; i < calls->ncounts; i++) {
        if (strcmp(calls->counts[i].caller, caller) == 0) {
            calls->counts[i].calls++;
            return;
        }
    }
    if (calls->ncounts == calls->capcounts) {
        calls->capcounts = calls->capcounts ? calls->capcounts * 2 : 32;
        calls->counts = (call_count *)xrealloc(
            calls->counts, calls->capcounts * sizeof(*calls->counts));
    }
    calls->counts[calls->ncounts].caller = xstrdup(caller);
    calls->counts[calls->ncounts].calls = 1;
    calls->counts[calls->ncounts].seq = calls->ncounts;
    calls->ncounts++;
}

static int scan_call(const char *line, const char *current, calls_result *out)
{
    const char *p;
    for (p = line; *p; p++) {
        const char *q;
        const char *name_start;
        size_t name_len;
        char *name;
        if (*p != 'b') continue;
        if (p > line && is_word((unsigned char)p[-1])) continue;
        if (p[1] == 'l' && p[2] && isspace((unsigned char)p[2])) {
            q = p + 2;
        } else if (p[1] && isspace((unsigned char)p[1])) {
            q = p + 1;
        } else {
            continue;
        }
        while (*q && isspace((unsigned char)*q)) q++;
        if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) {
            const char *h = q + 2;
            while (isxdigit((unsigned char)*h)) h++;
            if (h == q + 2) continue;
            q = h;
            while (*q && isspace((unsigned char)*q)) q++;
        }
        if (*q != '<') continue;
        q++;
        name_start = q;
        while (*q && *q != '>') q++;
        if (*q != '>') continue;
        name_len = (size_t)(q - name_start);
        name = xstrndup(name_start, name_len);
        {
            char *at = strchr(name, '@');
            if (at) *at = 0;
        }
        if (strcmp(name, EMUTLS_HELPER) == 0) {
            out->emutls_calls++;
            call_count_add(out, current);
            free(name);
            return 1;
        }
        if (strcmp(name, "__tls_get_addr") == 0) {
            out->tls_get_addr_calls++;
            free(name);
            return 1;
        }
        free(name);
    }
    return 0;
}

static int cmp_call_count(const void *a, const void *b)
{
    const call_count *ca = (const call_count *)a;
    const call_count *cb = (const call_count *)b;
    if (ca->calls != cb->calls) {
        return ca->calls < cb->calls ? 1 : -1;
    }
    if (ca->seq != cb->seq) {
        return ca->seq < cb->seq ? -1 : 1;
    }
    return 0;
}

static int caller_is_vendor(const char *name, const str_set *vendor_names)
{
    return is_vendor(name) || ss_contains(vendor_names, name);
}

static int count_calls(const char *objdump, const char *so,
                       const str_set *vendor_names, calls_result *out)
{
    char *argv[] = {
        (char *)objdump, (char *)"-d", (char *)"--no-show-raw-insn",
        (char *)so, NULL
    };
    char *cmd = build_command(argv);
    FILE *f = WC3_POPEN(cmd, "r");
    char line[8192];
    char *current = xstrdup("<unknown>");
    call_count *sorted = NULL;
    size_t i;
    int rc;

    if (!f) {
        fprintf(stderr, "audit_tls: popen failed: %s\n", cmd);
        free(cmd);
        free(current);
        return -1;
    }
    free(cmd);
    while (fgets(line, sizeof(line), f)) {
        char *name = NULL;
        strip_eol(line);
        if (parse_objdump_symbol(line, &name)) {
            free(current);
            current = name;
            continue;
        }
        scan_call(line, current, out);
    }
    rc = WC3_PCLOSE(f);
    free(current);
    if (rc != 0) {
        fprintf(stderr, "audit_tls: llvm-objdump -d failed\n");
        return -1;
    }
    out->has_calls = 1;
    for (i = 0; i < out->ncounts; i++) {
        if (caller_is_vendor(out->counts[i].caller, vendor_names))
            out->vendor_emutls_calls += out->counts[i].calls;
        else
            out->product_emutls_calls += out->counts[i].calls;
    }
    if (out->ncounts > 1) {
        sorted = (call_count *)xmalloc(out->ncounts * sizeof(*sorted));
        memcpy(sorted, out->counts, out->ncounts * sizeof(*sorted));
        qsort(sorted, out->ncounts, sizeof(*sorted), cmp_call_count);
    } else if (out->ncounts == 1) {
        sorted = (call_count *)xmalloc(sizeof(*sorted));
        sorted[0] = out->counts[0];
    }
    out->ntop = out->ncounts < 20 ? out->ncounts : 20;
    if (out->ntop) {
        out->top = (top_caller *)xmalloc(out->ntop * sizeof(*out->top));
    }
    for (i = 0; i < out->ntop; i++) {
        out->top[i].caller = xstrdup(sorted[i].caller);
        out->top[i].calls = sorted[i].calls;
        out->top[i].vendor = caller_is_vendor(sorted[i].caller, vendor_names);
    }
    free(sorted);
    return 0;
}

static void calls_free(calls_result *c)
{
    size_t i;
    for (i = 0; i < c->ncounts; i++) free(c->counts[i].caller);
    free(c->counts);
    for (i = 0; i < c->ntop; i++) free(c->top[i].caller);
    free(c->top);
    c->counts = NULL;
    c->ncounts = c->capcounts = 0;
    c->top = NULL;
    c->ntop = 0;
}

static int parse_archive_defined(const char *nm, const char *archive,
                                 str_set *set)
{
    char *argv[] = {(char *)nm, (char *)"--defined-only",
                    (char *)archive, NULL};
    char *out = run_argv(argv);
    symbol_list symbols = {0};
    size_t i;
    if (!out) return -1;
    parse_symbols(out, &symbols);
    for (i = 0; i < symbols.n; i++) ss_push(set, symbols.v[i].name);
    symbol_list_free(&symbols);
    free(out);
    return 0;
}

static int parse_archive_emutls(const char *nm, const char *archive,
                                uint64_t *refs)
{
    char *argv[] = {(char *)nm, (char *)"-u", (char *)archive, NULL};
    char *out = run_argv(argv);
    symbol_list symbols = {0};
    size_t i;
    if (!out) return -1;
    parse_symbols(out, &symbols);
    *refs = 0;
    for (i = 0; i < symbols.n; i++) {
        if (strcmp(symbols.v[i].name, EMUTLS_HELPER) == 0) (*refs)++;
    }
    symbol_list_free(&symbols);
    free(out);
    return 0;
}

static void json_string(FILE *f, const char *s)
{
    const unsigned char *p;
    fputc('"', f);
    for (p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc(c, f);
        } else if (c == '\n') {
            fputs("\\n", f);
        } else if (c == '\r') {
            fputs("\\r", f);
        } else if (c == '\t') {
            fputs("\\t", f);
        } else if (c < 0x20) {
            fprintf(f, "\\u%04x", c);
        } else {
            fputc(c, f);
        }
    }
    fputc('"', f);
}

static void json_indent(FILE *f, int n)
{
    int i;
    for (i = 0; i < n; i++) fputc(' ', f);
}

static void json_str_array(FILE *f, const strvec *v, int indent)
{
    size_t i;
    if (v->n == 0) {
        fputs("[]", f);
        return;
    }
    fputs("[\n", f);
    for (i = 0; i < v->n; i++) {
        json_indent(f, indent + 2);
        json_string(f, v->v[i]);
        if (i + 1 < v->n) fputc(',', f);
        fputc('\n', f);
    }
    json_indent(f, indent);
    fputc(']', f);
}

static void json_u64(FILE *f, int has, uint64_t value)
{
    if (!has) fputs("null", f);
    else fprintf(f, "%llu", (unsigned long long)value);
}

static int cmp_archive_ref(const void *a, const void *b)
{
    const archive_ref *const *ra = (const archive_ref *const *)a;
    const archive_ref *const *rb = (const archive_ref *const *)b;
    return strcmp((*ra)->path, (*rb)->path);
}

static int write_json(const char *path, const audit_report *r)
{
    FILE *f;
    size_t i;

    if (make_parent_dirs(path) != 0) return -1;
    f = fopen(path, "w");
    if (!f) return -1;

    fputs("{\n  \"artifact\": {\n", f);
    fprintf(f, "    \"bytes\": %llu,\n",
            (unsigned long long)r->artifact_bytes);
    fputs("    \"path\": ", f);
    json_string(f, r->artifact_path);
    fputs(",\n    \"sha256\": ", f);
    json_string(f, r->artifact_sha256);
    fputs("\n  },\n  \"calls\": {\n", f);
    fputs("    \"emutls_calls\": ", f);
    json_u64(f, r->calls.has_calls, r->calls.emutls_calls);
    fputs(",\n    \"product_emutls_calls\": ", f);
    json_u64(f, r->calls.has_calls, r->calls.product_emutls_calls);
    fputs(",\n    \"tls_get_addr_calls\": ", f);
    json_u64(f, r->calls.has_calls, r->calls.tls_get_addr_calls);
    fputs(",\n    \"top_callers\": [", f);
    if (r->calls.ntop == 0) {
        fputs("]", f);
    } else {
        fputc('\n', f);
        for (i = 0; i < r->calls.ntop; i++) {
            json_indent(f, 6);
            fputs("{\n", f);
            json_indent(f, 8);
            fputs("\"caller\": ", f);
            json_string(f, r->calls.top[i].caller);
            fputs(",\n", f);
            json_indent(f, 8);
            fprintf(f, "\"calls\": %llu,\n",
                    (unsigned long long)r->calls.top[i].calls);
            json_indent(f, 8);
            fprintf(f, "\"vendor\": %s\n",
                    r->calls.top[i].vendor ? "true" : "false");
            json_indent(f, 6);
            fputs("}", f);
            if (i + 1 < r->calls.ntop) fputc(',', f);
            fputc('\n', f);
        }
        json_indent(f, 4);
        fputc(']', f);
    }
    fputs(",\n    \"vendor_emutls_calls\": ", f);
    json_u64(f, r->calls.has_calls, r->calls.vendor_emutls_calls);
    fputs("\n  },\n  \"emutls\": {\n", f);
    fprintf(f, "    \"helper_defined\": %s,\n",
            r->helper_defined ? "true" : "false");
    fputs("    \"product_archive_refs\": ", f);
    if (r->nrefs == 0) {
        fputs("{}", f);
    } else {
        archive_ref **sorted = (archive_ref **)xmalloc(
            r->nrefs * sizeof(*sorted));
        for (i = 0; i < r->nrefs; i++) sorted[i] = &r->refs[i];
        qsort(sorted, r->nrefs, sizeof(*sorted), cmp_archive_ref);
        fputs("{\n", f);
        for (i = 0; i < r->nrefs; i++) {
            json_indent(f, 6);
            json_string(f, sorted[i]->path);
            fprintf(f, ": %llu",
                    (unsigned long long)sorted[i]->refs);
            if (i + 1 < r->nrefs) fputc(',', f);
            fputc('\n', f);
        }
        json_indent(f, 4);
        fputc('}', f);
        free(sorted);
    }
    fputs(",\n    \"product_calls\": ", f);
    json_u64(f, r->calls.has_calls, r->calls.product_emutls_calls);
    fputs(",\n    \"product_symbols\": ", f);
    json_str_array(f, &r->product_symbols, 4);
    fprintf(f, ",\n    \"vendor_archive_symbols\": %llu,\n",
            (unsigned long long)r->vendor_archive_symbols);
    fputs("    \"vendor_calls\": ", f);
    json_u64(f, r->calls.has_calls, r->calls.vendor_emutls_calls);
    fputs(",\n    \"vendor_symbols\": ", f);
    json_str_array(f, &r->vendor_symbols, 4);
    fputs("\n  },\n  \"expect\": ", f);
    json_string(f, r->expect);
    fputs(",\n  \"failures\": ", f);
    json_str_array(f, &r->failures, 2);
    fputs(",\n  \"tls\": {\n    \"dynamic_tags\": ", f);
    json_str_array(f, &r->dynamic_tags, 4);
    fputs(",\n    \"runtime_symbols\": ", f);
    json_str_array(f, &r->tls_symbols, 4);
    fputs(",\n    \"segment\": ", f);
    if (!r->segment.present) {
        fputs("null", f);
    } else {
        fputs("{\n", f);
        fputs("      \"numbers\": ", f);
        if (r->segment.nnumbers == 0) {
            fputs("[]", f);
        } else {
            size_t k;
            fputs("[\n", f);
            for (k = 0; k < r->segment.nnumbers; k++) {
                json_indent(f, 8);
                fprintf(f, "%llu",
                        (unsigned long long)r->segment.numbers[k]);
                if (k + 1 < r->segment.nnumbers) fputc(',', f);
                fputc('\n', f);
            }
            json_indent(f, 6);
            fputc(']', f);
        }
        fputs(",\n      \"row\": ", f);
        json_string(f, r->segment.row);
        fputs("\n    }", f);
    }
    fputs(",\n    \"tbss_bytes\": ", f);
    json_u64(f, r->has_tbss, r->tbss_bytes);
    fputs(",\n    \"tdata_bytes\": ", f);
    json_u64(f, r->has_tdata, r->tdata_bytes);
    fprintf(f, ",\n    \"total_bytes\": %llu\n  },\n",
            (unsigned long long)r->total_bytes);
    fputs("  \"tools\": {\n    \"nm\": ", f);
    json_string(f, r->nm);
    fputs(",\n    \"objdump\": ", f);
    if (r->objdump) json_string(f, r->objdump);
    else fputs("null", f);
    fputs(",\n    \"readelf\": ", f);
    json_string(f, r->readelf);
    fputs("\n  },\n  \"verdict\": ", f);
    json_string(f, r->verdict);
    fputs("\n}\n", f);
    if (fclose(f) != 0) return -1;
    return 0;
}

static int self_test(void)
{
    static const char *nm_text =
        "0000000000000001 T foo@GLIBC\n"
        "                 U __emutls_get_address\n"
        "---------------- T wc3_dxvk3_bar\n";
    static const char *sections_text =
        "  [ 1] .tdata            PROGBITS        0000000000000000 "
        "000001 000004 00 WA  0   0  8\n";
    static const char *headers_text =
        "  TLS      0x0000000000001234 0x0000000000000004 "
        "0x0000000000000010 0x0000000000000008 0x0000000000000004 "
        "R      0x4\n";
    static const char *dynamic_text =
        " 0x000000000000001c (PLTGOT) 0x0\n"
        " 0x000000000000001d (TLS)    0x1\n";
    symbol_list symbols = {0};
    section_list sections = {0};
    tls_segment segment = {0};
    strvec dynamic = {0};
    int ok = 1;

    parse_symbols(nm_text, &symbols);
    if (symbols.n != 3 ||
        strcmp(symbols.v[0].name, "foo") != 0 ||
        symbols.v[0].kind != 'T' ||
        strcmp(symbols.v[1].name, "__emutls_get_address") != 0 ||
        symbols.v[1].kind != 'U' ||
        strcmp(symbols.v[2].name, "wc3_dxvk3_bar") != 0) {
        fprintf(stderr, "audit_tls self-test: symbol parser failed\n");
        ok = 0;
    }
    parse_sections(sections_text, &sections);
    if (sections.n != 1 || strcmp(sections.v[0].name, ".tdata") != 0 ||
        sections.v[0].size != 4) {
        fprintf(stderr, "audit_tls self-test: section parser failed\n");
        ok = 0;
    }
    parse_program_headers(headers_text, &segment);
    if (!segment.present || segment.nnumbers != 6 ||
        segment.numbers[1] != 4) {
        fprintf(stderr, "audit_tls self-test: program header parser failed\n");
        ok = 0;
    }
    parse_dynamic(dynamic_text, &dynamic);
    if (dynamic.n != 1 || strstr(dynamic.v[0], "TLS") == NULL) {
        fprintf(stderr, "audit_tls self-test: dynamic parser failed\n");
        ok = 0;
    }
    if (!is_vendor("wc3_dxvk3_bar") || is_vendor("plain_product")) {
        fprintf(stderr, "audit_tls self-test: vendor classifier failed\n");
        ok = 0;
    }
    symbol_list_free(&symbols);
    section_list_free(&sections);
    free(segment.row);
    free(segment.numbers);
    sv_free(&dynamic);
    if (ok) printf("audit_tls self-test: OK\n");
    return ok ? 0 : 1;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: audit_tls --so FILE [--expect any|emulated|native] "
            "[--json FILE] [--nm EXE] [--readelf EXE] [--objdump EXE]\n"
            "       [--vendor-archive FILE]... [--product-archive FILE]... "
            "[--no-calls]\n"
            "       audit_tls --self-test\n");
}

static void report_free(audit_report *r)
{
    free(r->artifact_path);
    sv_free(&r->failures);
    free(r->segment.row);
    free(r->segment.numbers);
    sv_free(&r->dynamic_tags);
    sv_free(&r->tls_symbols);
    sv_free(&r->product_symbols);
    sv_free(&r->vendor_symbols);
    {
        size_t i;
        for (i = 0; i < r->nrefs; i++) free(r->refs[i].path);
        free(r->refs);
    }
    calls_free(&r->calls);
    free(r->nm);
    free(r->readelf);
    free(r->objdump);
}

int main(int argc, char **argv)
{
    const char *so_arg = NULL;
    const char *expect = "any";
    const char *json_path = NULL;
    const char *nm_arg = NULL;
    const char *readelf_arg = NULL;
    const char *objdump_arg = NULL;
    strvec vendor_args = {0};
    strvec product_args = {0};
    int no_calls = 0;
    int i;
    audit_report report;
    symbol_list symbols = {0};
    section_list sections = {0};
    str_set vendor_names = {0};
    char *nm_output = NULL;
    char *headers_output = NULL;
    char *sections_output = NULL;
    char *dynamic_output = NULL;
    int status = 2;

    memset(&report, 0, sizeof(report));
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--self-test") == 0) {
            return self_test();
        } else if (strcmp(a, "--so") == 0 && i + 1 < argc) {
            so_arg = argv[++i];
        } else if (strncmp(a, "--so=", 5) == 0) {
            so_arg = a + 5;
        } else if (strcmp(a, "--expect") == 0 && i + 1 < argc) {
            expect = argv[++i];
        } else if (strncmp(a, "--expect=", 9) == 0) {
            expect = a + 9;
        } else if (strcmp(a, "--json") == 0 && i + 1 < argc) {
            json_path = argv[++i];
        } else if (strncmp(a, "--json=", 7) == 0) {
            json_path = a + 7;
        } else if (strcmp(a, "--nm") == 0 && i + 1 < argc) {
            nm_arg = argv[++i];
        } else if (strncmp(a, "--nm=", 5) == 0) {
            nm_arg = a + 5;
        } else if (strcmp(a, "--readelf") == 0 && i + 1 < argc) {
            readelf_arg = argv[++i];
        } else if (strncmp(a, "--readelf=", 10) == 0) {
            readelf_arg = a + 10;
        } else if (strcmp(a, "--objdump") == 0 && i + 1 < argc) {
            objdump_arg = argv[++i];
        } else if (strncmp(a, "--objdump=", 10) == 0) {
            objdump_arg = a + 10;
        } else if (strcmp(a, "--vendor-archive") == 0 && i + 1 < argc) {
            sv_push_copy(&vendor_args, argv[++i]);
        } else if (strncmp(a, "--vendor-archive=", 17) == 0) {
            sv_push_copy(&vendor_args, a + 17);
        } else if (strcmp(a, "--product-archive") == 0 && i + 1 < argc) {
            sv_push_copy(&product_args, argv[++i]);
        } else if (strncmp(a, "--product-archive=", 18) == 0) {
            sv_push_copy(&product_args, a + 18);
        } else if (strcmp(a, "--no-calls") == 0) {
            no_calls = 1;
        } else {
            usage();
            goto cleanup;
        }
    }
    if (!so_arg || (strcmp(expect, "any") != 0 &&
                    strcmp(expect, "emulated") != 0 &&
                    strcmp(expect, "native") != 0)) {
        usage();
        goto cleanup;
    }

    report.artifact_path = path_resolve(so_arg);
    if (!is_file(report.artifact_path)) {
        fprintf(stderr, "audit_tls: artifact not found: %s\n",
                report.artifact_path);
        goto cleanup;
    }
    report.artifact_bytes = file_size(report.artifact_path);
    if (sha256_file(report.artifact_path, report.artifact_sha256) != 0) {
        fprintf(stderr, "audit_tls: cannot hash %s\n", report.artifact_path);
        goto cleanup;
    }
    report.expect = expect;

    report.nm = find_tool(
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
        "llvm-nm.exe", nm_arg);
#else
        "llvm-nm", nm_arg);
#endif
    report.readelf = find_tool(
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
        "llvm-readelf.exe", readelf_arg);
#else
        "llvm-readelf", readelf_arg);
#endif
    if (!report.nm || !report.readelf) {
        fprintf(stderr,
                "audit_tls: tool not found; pass --nm/--readelf/--objdump "
                "or set ANDROID_NDK_HOME\n");
        goto cleanup;
    }
    if (!no_calls) {
        report.objdump = find_tool(
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
            "llvm-objdump.exe", objdump_arg);
#else
            "llvm-objdump", objdump_arg);
#endif
        if (!report.objdump) {
            fprintf(stderr,
                    "audit_tls: llvm-objdump not found; pass --objdump or "
                    "use --no-calls\n");
            goto cleanup;
        }
    }

    {
        char *a1[] = {report.nm, (char *)"-D", report.artifact_path, NULL};
        nm_output = run_argv(a1);
        if (!nm_output) goto cleanup;
    }
    {
        char *a1[] = {report.readelf, (char *)"-l", report.artifact_path, NULL};
        headers_output = run_argv(a1);
        if (!headers_output) goto cleanup;
    }
    {
        char *a1[] = {report.readelf, (char *)"-S", (char *)"--wide",
                      report.artifact_path, NULL};
        sections_output = run_argv(a1);
        if (!sections_output) goto cleanup;
    }
    {
        char *a1[] = {report.readelf, (char *)"-d", report.artifact_path, NULL};
        dynamic_output = run_argv(a1);
        if (!dynamic_output) goto cleanup;
    }

    parse_symbols(nm_output, &symbols);
    parse_sections(sections_output, &sections);
    parse_program_headers(headers_output, &report.segment);
    parse_dynamic(dynamic_output, &report.dynamic_tags);

    for (i = 0; i < (int)vendor_args.n; i++) {
        if (parse_archive_defined(report.nm, vendor_args.v[i],
                                  &vendor_names) != 0)
            goto cleanup;
    }
    ss_finish(&vendor_names);
    report.vendor_archive_symbols = vendor_names.n;

    if (product_args.n) {
        report.refs = (archive_ref *)xmalloc(
            product_args.n * sizeof(*report.refs));
        report.nrefs = product_args.n;
        for (i = 0; i < (int)product_args.n; i++) {
            char *display = to_backslashes(product_args.v[i]);
            report.refs[i].path = display;
            report.refs[i].refs = 0;
            if (parse_archive_emutls(report.nm, product_args.v[i],
                                     &report.refs[i].refs) != 0)
                goto cleanup;
        }
    }

    if (!no_calls) {
        if (count_calls(report.objdump, report.artifact_path,
                        &vendor_names, &report.calls) != 0)
            goto cleanup;
    }

    {
        size_t k;
        for (k = 0; k < symbols.n; k++) {
            symbol *s = &symbols.v[k];
            if (strstr(s->name, "__emutls_")) {
                if (strcmp(s->name, EMUTLS_HELPER) != 0 && !is_vendor(s->name))
                    sv_push_copy(&report.product_symbols, s->name);
                if (is_vendor(s->name))
                    sv_push_copy(&report.vendor_symbols, s->name);
            }
            if (strcmp(s->name, "__tls_get_addr") == 0 ||
                strcmp(s->name, "__tls_guard") == 0 ||
                strncmp(s->name, "_ZTH", 4) == 0 ||
                strstr(s->name, "__tls_get_addr") != NULL) {
                sv_push_copy(&report.tls_symbols, s->name);
            }
            if (strcmp(s->name, EMUTLS_HELPER) == 0 && s->kind != 'U')
                report.helper_defined = 1;
        }
        for (k = 0; k < sections.n; k++) {
            if (strcmp(sections.v[k].name, ".tdata") == 0) {
                report.has_tdata = 1;
                report.tdata_bytes = sections.v[k].size;
            } else if (strcmp(sections.v[k].name, ".tbss") == 0) {
                report.has_tbss = 1;
                report.tbss_bytes = sections.v[k].size;
            }
        }
        report.total_bytes = (report.has_tdata ? report.tdata_bytes : 0) +
                             (report.has_tbss ? report.tbss_bytes : 0);
    }

    {
        uint64_t ref_total = 0;
        int has_product_emutls;
        size_t k;
        for (k = 0; k < report.nrefs; k++) ref_total += report.refs[k].refs;
        has_product_emutls = report.product_symbols.n > 0 ||
            (report.calls.has_calls && report.calls.product_emutls_calls > 0) ||
            ref_total > 0;

        if (strcmp(expect, "native") == 0) {
            if (report.product_symbols.n) {
                size_t z;
                size_t len = strlen("product emutls symbols remain: ") + 1;
                for (z = 0; z < report.product_symbols.n; z++)
                    len += strlen(report.product_symbols.v[z]) + 2;
                {
                    char *msg = (char *)xmalloc(len);
                    size_t off = 0;
                    off += (size_t)snprintf(msg, len, "product emutls symbols remain: ");
                    for (z = 0; z < report.product_symbols.n; z++) {
                        if (z) msg[off++] = ',';
                        if (z) msg[off++] = ' ';
                        memcpy(msg + off, report.product_symbols.v[z],
                               strlen(report.product_symbols.v[z]));
                        off += strlen(report.product_symbols.v[z]);
                    }
                    msg[off] = 0;
                    sv_push(&report.failures, msg);
                }
            }
            if (report.calls.has_calls && report.calls.product_emutls_calls) {
                char tmp[128];
                snprintf(tmp, sizeof(tmp),
                         "product emutls call sites remain: %llu",
                         (unsigned long long)report.calls.product_emutls_calls);
                sv_push_copy(&report.failures, tmp);
            }
            if (ref_total) {
                char tmp[128];
                snprintf(tmp, sizeof(tmp),
                         "product archive emutls references remain: %llu",
                         (unsigned long long)ref_total);
                sv_push_copy(&report.failures, tmp);
            }
            if (!report.segment.present)
                sv_push_copy(&report.failures, "no PT_TLS program header");
            report.verdict = report.failures.n ? "fail" : "pass";
        } else if (strcmp(expect, "emulated") == 0) {
            if (!has_product_emutls)
                sv_push_copy(&report.failures,
                             "no product emulated-TLS symbols or call sites");
            if (report.segment.present && report.product_symbols.n == 0)
                sv_push_copy(&report.failures,
                             "artifact has PT_TLS but WC3_NATIVE_TLS expects emulated");
            report.verdict = report.failures.n ? "fail" : "pass";
        } else {
            report.verdict = "report";
        }
    }

    printf("WC3 TLS audit\n");
    printf("  artifact: %s\n", report.artifact_path);
    printf("  sha256:   %s\n", report.artifact_sha256);
    printf("  TLS:      segment=%s .tdata=",
           report.segment.present ? "yes" : "no");
    if (report.has_tdata)
        printf("%llu", (unsigned long long)report.tdata_bytes);
    else
        printf("absent");
    printf(" .tbss=");
    if (report.has_tbss)
        printf("%llu", (unsigned long long)report.tbss_bytes);
    else
        printf("absent");
    printf(" runtime_symbols=%zu\n", report.tls_symbols.n);
    printf("  emutls:   helper=%s product_symbols=%zu vendor_symbols=%zu\n",
           report.helper_defined ? "yes" : "no",
           report.product_symbols.n, report.vendor_symbols.n);
    if (report.calls.has_calls) {
        printf("  calls:    product=%llu vendor=%llu total=%llu\n",
               (unsigned long long)report.calls.product_emutls_calls,
               (unsigned long long)report.calls.vendor_emutls_calls,
               (unsigned long long)report.calls.emutls_calls);
    } else {
        printf("  calls:    skipped (--no-calls)\n");
    }
    if (report.vendor_archive_symbols || report.nrefs) {
        uint64_t ref_total = 0;
        size_t k;
        for (k = 0; k < report.nrefs; k++) ref_total += report.refs[k].refs;
        printf("  archives: vendor_symbols=%zu product_emutls_refs=%llu\n",
               report.vendor_archive_symbols,
               (unsigned long long)ref_total);
        if (ref_total) {
            for (k = 0; k < report.nrefs; k++) {
                if (report.refs[k].refs)
                    printf("    %6llu  %s\n",
                           (unsigned long long)report.refs[k].refs,
                           report.refs[k].path);
            }
        }
    }
    if (report.product_symbols.n) {
        size_t k;
        printf("  product emutls symbols: ");
        for (k = 0; k < report.product_symbols.n; k++)
            printf("%s%s", k ? ", " : "", report.product_symbols.v[k]);
        printf("\n");
    }
    if (report.vendor_symbols.n) {
        size_t k;
        printf("  vendor emutls symbols:  ");
        for (k = 0; k < report.vendor_symbols.n; k++)
            printf("%s%s", k ? ", " : "", report.vendor_symbols.v[k]);
        printf("\n");
    }
    if (report.calls.ntop) {
        size_t k;
        printf("  top emutls callers:\n");
        for (k = 0; k < report.calls.ntop && k < 10; k++) {
            printf("    %6llu  %-7s  %s\n",
                   (unsigned long long)report.calls.top[k].calls,
                   report.calls.top[k].vendor ? "vendor" : "product",
                   report.calls.top[k].caller);
        }
    }
    printf("  verdict:  %s (expect %s)\n", report.verdict, report.expect);
    {
        size_t k;
        for (k = 0; k < report.failures.n; k++)
            printf("  failure:  %s\n", report.failures.v[k]);
    }

    if (json_path) {
        if (write_json(json_path, &report) != 0) {
            fprintf(stderr, "audit_tls: cannot write %s\n", json_path);
            goto cleanup;
        }
        printf("  json:     %s\n", json_path);
    }

    if (strcmp(report.verdict, "pass") == 0 ||
        strcmp(report.verdict, "report") == 0)
        status = 0;
    else
        status = 1;

cleanup:
    free(nm_output);
    free(headers_output);
    free(sections_output);
    free(dynamic_output);
    symbol_list_free(&symbols);
    section_list_free(&sections);
    ss_free(&vendor_names);
    sv_free(&vendor_args);
    sv_free(&product_args);
    report_free(&report);
    return status;
}
