/* softfloat_context_gen.c - generate the explicit-state SoftFloat build.
 *
 * This is the C implementation of the explicit-state SoftFloat build.  It starts
 * from the x87 SoftFloat roots, reads the Clang JSON AST for every source in
 * third_party/softfloat/android_sources.txt, computes the callee closure and
 * the state-using caller set, and writes a complete generated source tree.
 *
 * The transformation is deliberately narrow and pinned: only the closure
 * functions that use SoftFloat's four thread-local state objects are renamed
 * to *_ctx and receive a softfloat_state * first argument.  EXPECTED_CONTEXT
 * makes an accidental widening of the hot path a configure failure.
 *
 * Usage:
 *   softfloat_context_gen --root . --out build/softfloat_ctx \
 *       --clang <ndk-clang> --target aarch64-linux-android26 --jobs 8
 */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
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

#define WC3_PATH_MAX 4096

static const char *const ROOTS[] = {
    "extF80_add",
    "extF80_sub",
    "extF80_mul",
    "extF80_div",
    "extF80_sqrt",
    "extF80_rem",
    "extF80_roundToInt",
    "extF80_to_f32",
    "extF80_to_f64",
    "extF80_to_i32",
    "extF80_to_i64",
    "extF80_to_i32_r_minMag",
    "extF80_to_i64_r_minMag",
    "f32_to_extF80",
    "f64_to_extF80",
    "extF80_to_f128",
    "f128_to_extF80"
};

static const char *const EXPECTED_CONTEXT[] = {
    "extF80_add",
    "extF80_div",
    "extF80_mul",
    "extF80_rem",
    "extF80_roundToInt",
    "extF80_sqrt",
    "extF80_sub",
    "extF80_to_f128",
    "extF80_to_f32",
    "extF80_to_f64",
    "extF80_to_i32",
    "extF80_to_i32_r_minMag",
    "extF80_to_i64",
    "extF80_to_i64_r_minMag",
    "f128_to_extF80",
    "f32_to_extF80",
    "f64_to_extF80",
    "softfloat_addMagsExtF80",
    "softfloat_extF80UIToCommonNaN",
    "softfloat_f128UIToCommonNaN",
    "softfloat_f32UIToCommonNaN",
    "softfloat_f64UIToCommonNaN",
    "softfloat_normRoundPackToExtF80",
    "softfloat_propagateNaNExtF80UI",
    "softfloat_raiseFlags",
    "softfloat_roundPackToExtF80",
    "softfloat_roundPackToF32",
    "softfloat_roundPackToF64",
    "softfloat_roundToI32",
    "softfloat_roundToI64",
    "softfloat_subMagsExtF80"
};

static const char *const X87_PUBLIC[] = {
    "extF80_add",
    "extF80_sub",
    "extF80_mul",
    "extF80_div",
    "extF80_rem",
    "extF80_sqrt",
    "extF80_roundToInt",
    "extF80_to_f128",
    "extF80_to_f32",
    "extF80_to_f64",
    "extF80_to_i32",
    "extF80_to_i64",
    "f128_to_extF80",
    "f32_to_extF80",
    "f64_to_extF80"
};

static const char *const STATE_NAMES[] = {
    "softfloat_roundingMode",
    "softfloat_detectTininess",
    "softfloat_exceptionFlags",
    "extF80_roundingPrecision"
};

static const char *const STATE_FIELDS[] = {
    "roundingMode",
    "detectTininess",
    "exceptionFlags",
    "extF80RoundingPrecision"
};

typedef struct {
    char **v;
    size_t n;
    size_t cap;
} strvec;

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "softfloat_context_gen: out of memory\n");
        exit(2);
    }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "softfloat_context_gen: out of memory\n");
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

static int cmp_str(const void *a, const void *b)
{
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
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

static char *path_dirname(const char *path)
{
    size_t i = strlen(path);
    while (i > 0 && path[i - 1] != '/' && path[i - 1] != '\\') i--;
    if (i == 0) return xstrdup(".");
    while (i > 1 && (path[i - 1] == '/' || path[i - 1] == '\\')) i--;
    return xstrndup(path, i);
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

static char *path_resolve(const char *path)
{
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    char buf[WC3_PATH_MAX];
    DWORD n = GetFullPathNameA(path, (DWORD)sizeof(buf), buf, NULL);
    if (n == 0 || n >= sizeof(buf)) return xstrdup(path);
    return xstrdup(buf);
#else
    char buf[WC3_PATH_MAX];
    if (!realpath(path, buf)) return xstrdup(path);
    return xstrdup(buf);
#endif
}

static int path_equal(const char *a, const char *b)
{
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    return _stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}

static int make_dirs(const char *path)
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
    if (!is_dir(buf) && WC3_MKDIR(buf) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int remove_tree(const char *path)
{
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    char *pattern = path_join(path, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        char *child;
        if (strcmp(fd.cFileName, ".") == 0 ||
            strcmp(fd.cFileName, "..") == 0) continue;
        child = path_join(path, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            remove_tree(child);
        else
            DeleteFileA(child);
        free(child);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return _rmdir(path);
#else
    DIR *d = opendir(path);
    struct dirent *ent;
    if (!d) return -1;
    while ((ent = readdir(d)) != NULL) {
        char *child;
        struct stat st;
        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0) continue;
        child = path_join(path, ent->d_name);
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode))
            remove_tree(child);
        else
            unlink(child);
        free(child);
    }
    closedir(d);
    return rmdir(path);
#endif
}

static char *read_text(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *data;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    data = (char *)xmalloc((size_t)size + 1);
    if (size > 0 && fread(data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(data);
        return NULL;
    }
    fclose(f);
    data[size] = 0;
    if (out_len) *out_len = (size_t)size;
    return data;
}

static int write_text(const char *path, const char *text, size_t len)
{
    FILE *f = fopen(path, "wb");
    size_t i;
    if (!f) return -1;
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    for (i = 0; i < len; i++) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
            if (fputc('\r', f) == EOF) {
                fclose(f);
                return -1;
            }
        }
        if (fputc((unsigned char)text[i], f) == EOF) {
            fclose(f);
            return -1;
        }
    }
#else
    if (len && fwrite(text, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
#endif
    if (fclose(f) != 0) return -1;
    return 0;
}

static int write_text_cstr(const char *path, const char *text)
{
    return write_text(path, text, strlen(text));
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
    size_t cap = 512;
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
    char chunk[16384];
    size_t got;
    if (!f) {
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

/* ------------------------------------------------------------------------- */
/* Minimal JSON DOM.  Clang emits only arrays, objects, strings, numbers,
 * booleans and null here; the parser is intentionally small. */

typedef enum {
    JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ
} jv_type;

typedef struct jval jval;

struct jval {
    jv_type type;
    int boolean;
    long long inum;
    char *str;
    jval **items;
    size_t nitems;
    char **keys;
    jval **vals;
    size_t nkeys;
};

typedef struct {
    const char *p;
    const char *end;
    int error;
    int depth;
} jparser;

static jval *jv_new(jv_type type)
{
    jval *v = (jval *)xmalloc(sizeof(*v));
    memset(v, 0, sizeof(*v));
    v->type = type;
    return v;
}

static void jv_free(jval *v)
{
    size_t i;
    if (!v) return;
    if (v->type == JV_STR) free(v->str);
    for (i = 0; i < v->nitems; i++) jv_free(v->items[i]);
    free(v->items);
    for (i = 0; i < v->nkeys; i++) {
        free(v->keys[i]);
        jv_free(v->vals[i]);
    }
    free(v->keys);
    free(v->vals);
    free(v);
}

static void jv_array_add(jval *v, jval *item)
{
    v->items = (jval **)xrealloc(v->items,
                                 (v->nitems + 1) * sizeof(*v->items));
    v->items[v->nitems++] = item;
}

static void jv_object_add(jval *v, char *key, jval *val)
{
    v->keys = (char **)xrealloc(v->keys, (v->nkeys + 1) * sizeof(*v->keys));
    v->vals = (jval **)xrealloc(v->vals, (v->nkeys + 1) * sizeof(*v->vals));
    v->keys[v->nkeys] = key;
    v->vals[v->nkeys] = val;
    v->nkeys++;
}

static void jp_skip_ws(jparser *jp)
{
    while (jp->p < jp->end &&
           (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\r' ||
            *jp->p == '\n')) {
        jp->p++;
    }
}

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *jp_string(jparser *jp)
{
    size_t cap = 64;
    size_t n = 0;
    char *out;
    if (jp->p >= jp->end || *jp->p != '"') {
        jp->error = 1;
        return NULL;
    }
    jp->p++;
    out = (char *)xmalloc(cap);
    while (jp->p < jp->end) {
        unsigned char c = (unsigned char)*jp->p++;
        if (c == '"') {
            out[n] = 0;
            return out;
        }
        if (c == '\\') {
            if (jp->p >= jp->end) break;
            c = (unsigned char)*jp->p++;
            if (c == '"' || c == '\\' || c == '/') {
                /* literal */
            } else if (c == 'b') c = '\b';
            else if (c == 'f') c = '\f';
            else if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c == 'u') {
                unsigned cp = 0;
                int k;
                if (jp->end - jp->p < 4) break;
                for (k = 0; k < 4; k++) {
                    int d = hex_digit((unsigned char)jp->p[k]);
                    if (d < 0) break;
                    cp = (cp << 4) | (unsigned)d;
                }
                jp->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF &&
                    jp->end - jp->p >= 6 && jp->p[0] == '\\' &&
                    jp->p[1] == 'u') {
                    unsigned lo = 0;
                    int ok = 1;
                    for (k = 0; k < 4; k++) {
                        int d = hex_digit((unsigned char)jp->p[2 + k]);
                        if (d < 0) {
                            ok = 0;
                            break;
                        }
                        lo = (lo << 4) | (unsigned)d;
                    }
                    if (ok) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        jp->p += 6;
                    }
                }
                if (cp < 0x80) {
                    c = (unsigned char)cp;
                } else if (cp < 0x800) {
                    if (n + 2 > cap) {
                        cap *= 2;
                        out = (char *)xrealloc(out, cap);
                    }
                    out[n++] = (char)(0xC0 | (cp >> 6));
                    out[n++] = (char)(0x80 | (cp & 0x3F));
                    continue;
                } else {
                    if (n + 3 > cap) {
                        cap *= 2;
                        out = (char *)xrealloc(out, cap);
                    }
                    out[n++] = (char)(0xE0 | (cp >> 12));
                    out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[n++] = (char)(0x80 | (cp & 0x3F));
                    continue;
                }
            } else {
                break;
            }
        }
        if (n + 1 >= cap) {
            cap *= 2;
            out = (char *)xrealloc(out, cap);
        }
        out[n++] = (char)c;
    }
    free(out);
    jp->error = 1;
    return NULL;
}

static jval *jp_value(jparser *jp);

static jval *jp_array(jparser *jp)
{
    jval *v = jv_new(JV_ARR);
    jp->p++; /* [ */
    jp_skip_ws(jp);
    if (jp->p < jp->end && *jp->p == ']') {
        jp->p++;
        return v;
    }
    while (jp->p < jp->end) {
        jval *item;
        jp_skip_ws(jp);
        item = jp_value(jp);
        if (!item) {
            jv_free(v);
            return NULL;
        }
        jv_array_add(v, item);
        jp_skip_ws(jp);
        if (jp->p < jp->end && *jp->p == ',') {
            jp->p++;
            continue;
        }
        if (jp->p < jp->end && *jp->p == ']') {
            jp->p++;
            return v;
        }
        jp->error = 1;
        jv_free(v);
        return NULL;
    }
    jp->error = 1;
    jv_free(v);
    return NULL;
}

static jval *jp_object(jparser *jp)
{
    jval *v = jv_new(JV_OBJ);
    jp->p++; /* { */
    jp_skip_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') {
        jp->p++;
        return v;
    }
    while (jp->p < jp->end) {
        char *key;
        jval *item;
        jp_skip_ws(jp);
        key = jp_string(jp);
        if (!key) {
            jv_free(v);
            return NULL;
        }
        jp_skip_ws(jp);
        if (jp->p >= jp->end || *jp->p != ':') {
            free(key);
            jp->error = 1;
            jv_free(v);
            return NULL;
        }
        jp->p++;
        item = jp_value(jp);
        if (!item) {
            free(key);
            jv_free(v);
            return NULL;
        }
        jv_object_add(v, key, item);
        jp_skip_ws(jp);
        if (jp->p < jp->end && *jp->p == ',') {
            jp->p++;
            continue;
        }
        if (jp->p < jp->end && *jp->p == '}') {
            jp->p++;
            return v;
        }
        jp->error = 1;
        jv_free(v);
        return NULL;
    }
    jp->error = 1;
    jv_free(v);
    return NULL;
}

static jval *jp_value(jparser *jp)
{
    jval *v;
    jp_skip_ws(jp);
    if (jp->depth++ > 20000) {
        jp->error = 1;
        jp->depth--;
        return NULL;
    }
    if (jp->p >= jp->end) {
        jp->error = 1;
        jp->depth--;
        return NULL;
    }
    if (*jp->p == '{') v = jp_object(jp);
    else if (*jp->p == '[') v = jp_array(jp);
    else if (*jp->p == '"') {
        char *s = jp_string(jp);
        if (!s) {
            jp->depth--;
            return NULL;
        }
        v = jv_new(JV_STR);
        v->str = s;
    } else if (*jp->p == 't' && jp->end - jp->p >= 4 &&
               memcmp(jp->p, "true", 4) == 0) {
        jp->p += 4;
        v = jv_new(JV_BOOL);
        v->boolean = 1;
    } else if (*jp->p == 'f' && jp->end - jp->p >= 5 &&
               memcmp(jp->p, "false", 5) == 0) {
        jp->p += 5;
        v = jv_new(JV_BOOL);
        v->boolean = 0;
    } else if (*jp->p == 'n' && jp->end - jp->p >= 4 &&
               memcmp(jp->p, "null", 4) == 0) {
        jp->p += 4;
        v = jv_new(JV_NULL);
    } else {
        const char *start = jp->p;
        char *endptr;
        char *copy;
        if (*jp->p == '-') jp->p++;
        if (jp->p >= jp->end || !isdigit((unsigned char)*jp->p)) {
            jp->error = 1;
            jp->depth--;
            return NULL;
        }
        while (jp->p < jp->end &&
               (isdigit((unsigned char)*jp->p) || *jp->p == '.' ||
                *jp->p == 'e' || *jp->p == 'E' || *jp->p == '+' ||
                *jp->p == '-')) {
            jp->p++;
        }
        copy = xstrndup(start, (size_t)(jp->p - start));
        v = jv_new(JV_NUM);
        v->inum = strtoll(copy, &endptr, 10);
        free(copy);
    }
    jp->depth--;
    return v;
}

static jval *json_parse(const char *text, size_t len, char *err, size_t errlen)
{
    jparser jp;
    jval *v;
    jp.p = text;
    jp.end = text + len;
    jp.error = 0;
    jp.depth = 0;
    jp_skip_ws(&jp);
    v = jp_value(&jp);
    if (!v) {
        snprintf(err, errlen, "invalid AST JSON");
        return NULL;
    }
    jp_skip_ws(&jp);
    if (jp.p != jp.end) {
        snprintf(err, errlen, "trailing data in AST JSON");
        jv_free(v);
        return NULL;
    }
    return v;
}

static jval *jget(const jval *v, const char *key)
{
    size_t i;
    if (!v || v->type != JV_OBJ) return NULL;
    for (i = 0; i < v->nkeys; i++) {
        if (strcmp(v->keys[i], key) == 0) return v->vals[i];
    }
    return NULL;
}

static const char *jstr(const jval *v)
{
    return v && v->type == JV_STR ? v->str : NULL;
}

static long long jint(const jval *v, long long def)
{
    return v && v->type == JV_NUM ? v->inum : def;
}

static int jbool(const jval *v)
{
    return v && v->type == JV_BOOL ? v->boolean : 0;
}

/* ------------------------------------------------------------------------- */
/* AST extraction. */

typedef struct {
    char *callee;
    long long name_start;
    long long open_paren;
} call_ref;

typedef struct {
    char *name;
    long long offset;
} state_ref;

typedef struct {
    char *name;
    char *file;
    char *ret;
    char *args;
    char *storage;
    strvec params;
    long long name_offset;
    long long name_len;
    long long open_paren;
    long long close_paren;
    long long body_begin;
    long long body_end;
    call_ref *calls;
    size_t ncalls;
    size_t capcalls;
    long long *indirect;
    size_t nindirect;
    size_t capindirect;
    state_ref *refs;
    size_t nrefs;
    size_t caprefs;
} definition;

typedef struct {
    definition **v;
    size_t n;
    size_t cap;
} def_list;

typedef struct {
    char *rel;
    def_list defs;
    char *error;
} unit_result;

typedef struct {
    const char *rel;
    const char *base;
    const char *source;
    size_t source_len;
    const char *abs_path;
    def_list *defs;
    char error[512];
    int failed;
} tu_ctx;

static void def_list_add(def_list *v, definition *d)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->v = (definition **)xrealloc(v->v, v->cap * sizeof(*v->v));
    }
    v->v[v->n++] = d;
}

static void definition_free(definition *d)
{
    size_t i;
    if (!d) return;
    free(d->name);
    free(d->file);
    free(d->ret);
    free(d->args);
    free(d->storage);
    sv_free(&d->params);
    for (i = 0; i < d->ncalls; i++) free(d->calls[i].callee);
    free(d->calls);
    free(d->indirect);
    for (i = 0; i < d->nrefs; i++) free(d->refs[i].name);
    free(d->refs);
    free(d);
}

static void def_list_free(def_list *v)
{
    size_t i;
    for (i = 0; i < v->n; i++) definition_free(v->v[i]);
    free(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

static definition *def_find(def_list *v, const char *name)
{
    size_t i;
    for (i = 0; i < v->n; i++) {
        if (strcmp(v->v[i]->name, name) == 0) return v->v[i];
    }
    return NULL;
}

static void set_error(tu_ctx *ctx, const char *fmt, ...)
{
    va_list ap;
    if (ctx->failed) return;
    ctx->failed = 1;
    va_start(ap, fmt);
    vsnprintf(ctx->error, sizeof(ctx->error), fmt, ap);
    va_end(ap);
}

static long long source_offset(const jval *node)
{
    const jval *nested;
    if (!node || node->type != JV_OBJ) return -1;
    if (jget(node, "offset")) return jint(jget(node, "offset"), -1);
    nested = jget(node, "expansionLoc");
    if (nested) {
        const jval *off = jget(nested, "offset");
        if (off) return jint(off, -1);
    }
    nested = jget(node, "spellingLoc");
    if (nested) {
        const jval *off = jget(nested, "offset");
        if (off) return jint(off, -1);
    }
    return -1;
}

static int is_absolute_path(const char *p)
{
    if (!p || !*p) return 0;
    if (p[0] == '/' || p[0] == '\\') return 1;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') return 1;
    return 0;
}

static int file_is_main(tu_ctx *ctx, const jval *loc)
{
    const char *file = jstr(jget(loc, "file"));
    if (!file) return 0;
    if (is_absolute_path(file)) {
        char *abs = path_resolve(file);
        int same = path_equal(abs, ctx->abs_path);
        free(abs);
        return same;
    }
    {
        char *base = path_basename(file);
        int same = strcmp(base, ctx->base) == 0;
        free(base);
        return same;
    }
}

static long long find_char_from(const char *s, size_t len, long long from,
                                char want)
{
    long long i;
    if (from < 0) from = 0;
    for (i = from; i < (long long)len; i++) {
        if (s[i] == want) return i;
    }
    return -1;
}

static long long find_matching_paren(const char *s, size_t len, long long open)
{
    long long depth = 0;
    long long i;
    for (i = open; i < (long long)len; i++) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

static char *trimmed_slice(const char *s, long long start, long long end)
{
    while (start < end && isspace((unsigned char)s[start])) start++;
    while (end > start && isspace((unsigned char)s[end - 1])) end--;
    return xstrndup(s + start, (size_t)(end - start));
}

static void add_call(definition *d, char *callee, long long name_start,
                     long long open_paren)
{
    if (d->ncalls == d->capcalls) {
        d->capcalls = d->capcalls ? d->capcalls * 2 : 16;
        d->calls = (call_ref *)xrealloc(
            d->calls, d->capcalls * sizeof(*d->calls));
    }
    d->calls[d->ncalls].callee = callee;
    d->calls[d->ncalls].name_start = name_start;
    d->calls[d->ncalls].open_paren = open_paren;
    d->ncalls++;
}

static void add_indirect(definition *d, long long offset)
{
    if (d->nindirect == d->capindirect) {
        d->capindirect = d->capindirect ? d->capindirect * 2 : 8;
        d->indirect = (long long *)xrealloc(
            d->indirect, d->capindirect * sizeof(*d->indirect));
    }
    d->indirect[d->nindirect++] = offset;
}

static void add_state_ref(definition *d, const char *name, long long offset)
{
    if (d->nrefs == d->caprefs) {
        d->caprefs = d->caprefs ? d->caprefs * 2 : 8;
        d->refs = (state_ref *)xrealloc(
            d->refs, d->caprefs * sizeof(*d->refs));
    }
    d->refs[d->nrefs].name = xstrdup(name);
    d->refs[d->nrefs].offset = offset;
    d->nrefs++;
}

static jval *unwrap_expr(jval *node)
{
    while (node && node->type == JV_OBJ) {
        const char *kind = jstr(jget(node, "kind"));
        jval *inner;
        if (!kind) break;
        if (strcmp(kind, "ImplicitCastExpr") != 0 &&
            strcmp(kind, "ParenExpr") != 0 &&
            strcmp(kind, "CStyleCastExpr") != 0 &&
            strcmp(kind, "ExprWithCleanups") != 0) {
            break;
        }
        inner = jget(node, "inner");
        if (!inner || inner->type != JV_ARR || inner->nitems == 0) return NULL;
        node = inner->items[0];
    }
    return node;
}

static int call_target(jval *call, tu_ctx *ctx, char **out_name,
                       long long *out_start, long long *out_open)
{
    jval *inner = jget(call, "inner");
    jval *callee;
    const char *kind;
    const char *name;
    jval *ref;
    long long name_start;
    long long end;
    long long open;
    if (!inner || inner->type != JV_ARR || inner->nitems == 0) return 0;
    callee = unwrap_expr(inner->items[0]);
    if (!callee || callee->type != JV_OBJ) return 0;
    kind = jstr(jget(callee, "kind"));
    if (!kind || strcmp(kind, "DeclRefExpr") != 0) return 0;
    ref = jget(callee, "referencedDecl");
    kind = jstr(jget(ref, "kind"));
    if (!kind || strcmp(kind, "FunctionDecl") != 0) return 0;
    name = jstr(jget(ref, "name"));
    if (!name) return 0;
    name_start = source_offset(jget(jget(callee, "range"), "begin"));
    end = source_offset(jget(jget(callee, "range"), "end"));
    if (name_start < 0 || end < 0) return 0;
    open = find_char_from(ctx->source, ctx->source_len, end, '(');
    if (open < 0) return 0;
    *out_name = xstrdup(name);
    *out_start = name_start;
    *out_open = open;
    return 1;
}

static void walk_body(jval *node, tu_ctx *ctx, definition *d)
{
    jval *inner;
    size_t i;
    if (!node || node->type != JV_OBJ) return;
    if (jstr(jget(node, "kind")) &&
        strcmp(jstr(jget(node, "kind")), "CallExpr") == 0) {
        long long begin = source_offset(jget(jget(node, "range"), "begin"));
        if (begin >= d->body_begin && begin <= d->body_end) {
            char *name = NULL;
            long long name_start = 0;
            long long open = 0;
            if (call_target(node, ctx, &name, &name_start, &open)) {
                add_call(d, name, name_start, open);
            } else {
                add_indirect(d, begin);
            }
        }
    }
    inner = jget(node, "inner");
    if (!inner || inner->type != JV_ARR) return;
    for (i = 0; i < inner->nitems; i++) walk_body(inner->items[i], ctx, d);
}

static void add_state_refs(tu_ctx *ctx, definition *d)
{
    long long start = d->body_begin;
    long long end = d->body_end;
    long long i;
    size_t s;
    if (start < 0 || end < start || end >= (long long)ctx->source_len) return;
    for (i = start; i <= end; i++) {
        for (s = 0; s < sizeof(STATE_NAMES) / sizeof(STATE_NAMES[0]); s++) {
            size_t n = strlen(STATE_NAMES[s]);
            if ((size_t)(end - i + 1) < n) continue;
            if (memcmp(ctx->source + i, STATE_NAMES[s], n) != 0) continue;
            if (i > start && (isalnum((unsigned char)ctx->source[i - 1]) ||
                              ctx->source[i - 1] == '_')) continue;
            if (i + (long long)n <= end &&
                (isalnum((unsigned char)ctx->source[i + n]) ||
                 ctx->source[i + n] == '_')) continue;
            add_state_ref(d, STATE_NAMES[s], i);
        }
    }
}

static definition *handle_function(jval *node, tu_ctx *ctx)
{
    const char *name = jstr(jget(node, "name"));
    jval *loc = jget(node, "loc");
    jval *inner = jget(node, "inner");
    jval *body = NULL;
    jval *range_begin;
    definition *d;
    long long ret_start;
    long long name_offset;
    long long name_len;
    long long open;
    long long close;
    long long body_begin;
    long long body_end;
    size_t i;
    if (!name || !loc || !inner || inner->type != JV_ARR) return NULL;
    if (!file_is_main(ctx, loc)) return NULL;
    for (i = 0; i < inner->nitems; i++) {
        const char *kind = jstr(jget(inner->items[i], "kind"));
        if (kind && strcmp(kind, "CompoundStmt") == 0) {
            body = inner->items[i];
            break;
        }
    }
    if (!body) return NULL;
    name_offset = jint(jget(loc, "offset"), -1);
    if (name_offset < 0) {
        set_error(ctx, "missing source range for %s in %s", name, ctx->rel);
        return NULL;
    }
    name_len = jint(jget(loc, "tokLen"), (long long)strlen(name));
    range_begin = jget(jget(node, "range"), "begin");
    ret_start = source_offset(range_begin);
    if (ret_start < 0) {
        set_error(ctx, "missing source range for %s in %s", name, ctx->rel);
        return NULL;
    }
    open = find_char_from(ctx->source, ctx->source_len,
                          name_offset + name_len, '(');
    if (open < 0) {
        set_error(ctx, "missing parameter list for %s in %s", name, ctx->rel);
        return NULL;
    }
    close = find_matching_paren(ctx->source, ctx->source_len, open);
    if (close < 0) {
        set_error(ctx, "unbalanced parenthesis at offset %lld", open);
        return NULL;
    }
    body_begin = source_offset(jget(jget(body, "range"), "begin"));
    body_end = source_offset(jget(jget(body, "range"), "end"));
    if (body_begin < 0 || body_end < 0) {
        set_error(ctx, "missing body range for %s in %s", name, ctx->rel);
        return NULL;
    }
    d = (definition *)xmalloc(sizeof(*d));
    memset(d, 0, sizeof(*d));
    d->name = xstrdup(name);
    d->file = xstrdup(ctx->rel);
    d->ret = trimmed_slice(ctx->source, ret_start, name_offset);
    d->args = trimmed_slice(ctx->source, open + 1, close);
    d->storage = xstrdup(jstr(jget(node, "storageClass")) ?
                         jstr(jget(node, "storageClass")) : "");
    d->name_offset = name_offset;
    d->name_len = name_len;
    d->open_paren = open;
    d->close_paren = close;
    d->body_begin = body_begin;
    d->body_end = body_end;
    for (i = 0; i < inner->nitems; i++) {
        const char *kind = jstr(jget(inner->items[i], "kind"));
        if (kind && strcmp(kind, "ParmVarDecl") == 0) {
            const char *pname = jstr(jget(inner->items[i], "name"));
            char tmp[64];
            if (!pname) {
                snprintf(tmp, sizeof(tmp), "arg%zu", d->params.n);
                pname = tmp;
            }
            sv_push_copy(&d->params, pname);
        }
    }
    walk_body(body, ctx, d);
    add_state_refs(ctx, d);
    def_list_add(ctx->defs, d);
    return d;
}

static void walk_ast(jval *node, tu_ctx *ctx)
{
    jval *inner;
    size_t i;
    if (!node || node->type != JV_OBJ || ctx->failed) return;
    if (jstr(jget(node, "kind")) &&
        strcmp(jstr(jget(node, "kind")), "FunctionDecl") == 0) {
        handle_function(node, ctx);
        if (ctx->failed) return;
    }
    inner = jget(node, "inner");
    if (!inner || inner->type != JV_ARR) return;
    for (i = 0; i < inner->nitems; i++) {
        walk_ast(inner->items[i], ctx);
        if (ctx->failed) return;
    }
}

static unit_result *parse_unit(const char *root, const char *softfloat,
                               const char *clang, const char *target,
                               const char *rel)
{
    unit_result *result = (unit_result *)xmalloc(sizeof(*result));
    char *path = path_join(softfloat, rel);
    char *abs_path = path_resolve(path);
    char *base = path_basename(path);
    char *source = NULL;
    size_t source_len = 0;
    char *out = NULL;
    char *argv[24];
    int argc = 0;
    char target_arg[256];
    char inc1[WC3_PATH_MAX];
    char inc2[WC3_PATH_MAX];
    char inc3[WC3_PATH_MAX];
    tu_ctx ctx;
    jval *ast = NULL;
    char json_err[256];

    memset(result, 0, sizeof(*result));
    result->rel = xstrdup(rel);
    source = read_text(path, &source_len);
    if (!source) {
        result->error = xstrdup("cannot read SoftFloat source");
        free(path);
        free(abs_path);
        free(base);
        return result;
    }
    (void)root;

    snprintf(inc1, sizeof(inc1), "-I%s/source/include", softfloat);
    snprintf(inc2, sizeof(inc2), "-I%s/source/8086-SSE", softfloat);
    snprintf(inc3, sizeof(inc3), "-I%s", softfloat);
    snprintf(target_arg, sizeof(target_arg), "--target=%s", target);

    argv[argc++] = (char *)clang;
    argv[argc++] = target_arg;
    argv[argc++] = (char *)"-O2";
    argv[argc++] = (char *)"-fPIC";
    argv[argc++] = (char *)"-fno-emulated-tls";
    argv[argc++] = (char *)"-DTHREAD_LOCAL=_Thread_local";
    argv[argc++] = (char *)"-DSOFTFLOAT_FAST_INT64";
    argv[argc++] = (char *)"-DLITTLEENDIAN";
    argv[argc++] = (char *)"-DINLINE_LEVEL=2";
    argv[argc++] = inc3;
    argv[argc++] = inc1;
    argv[argc++] = inc2;
    argv[argc++] = (char *)"-Xclang";
    argv[argc++] = (char *)"-ast-dump=json";
    argv[argc++] = (char *)"-fsyntax-only";
    argv[argc++] = path;
    argv[argc] = NULL;

    out = capture_argv(argv);
    free(path);
    if (!out) {
        result->error = xstrdup("clang AST failed");
        free(source);
        free(abs_path);
        free(base);
        return result;
    }
    ast = json_parse(out, strlen(out), json_err, sizeof(json_err));
    free(out);
    if (!ast) {
        result->error = xstrdup(json_err);
        free(source);
        free(abs_path);
        free(base);
        return result;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.rel = rel;
    ctx.base = base;
    ctx.source = source;
    ctx.source_len = source_len;
    ctx.abs_path = abs_path;
    ctx.defs = &result->defs;
    walk_ast(ast, &ctx);
    jv_free(ast);
    free(source);
    free(abs_path);
    free(base);
    if (ctx.failed) {
        result->error = xstrdup(ctx.error);
    }
    return result;
}

/* ------------------------------------------------------------------------- */
/* Parallel driver. */

#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
typedef struct {
    const char *root;
    const char *softfloat;
    const char *clang;
    const char *target;
    strvec *files;
    unit_result *results;
    volatile LONG next;
} tu_pool;

static DWORD WINAPI tu_worker(LPVOID arg)
{
    tu_pool *pool = (tu_pool *)arg;
    for (;;) {
        LONG i = InterlockedIncrement(&pool->next) - 1;
        unit_result *unit;
        if ((size_t)i >= pool->files->n) break;
        unit = parse_unit(pool->root, pool->softfloat, pool->clang,
                          pool->target, pool->files->v[i]);
        pool->results[i] = *unit;
        free(unit);
    }
    return 0;
}
#endif

static void parse_all(const char *root, const char *softfloat,
                      const char *clang, const char *target, strvec *files,
                      int jobs, unit_result **out_results)
{
    unit_result *results = (unit_result *)calloc(
        files->n ? files->n : 1, sizeof(*results));
    size_t i;
    if (!results) {
        fprintf(stderr, "softfloat_context_gen: out of memory\n");
        exit(2);
    }
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    if (jobs > 1) {
        tu_pool pool;
        HANDLE *threads;
        int nthreads = jobs;
        if ((size_t)nthreads > files->n) nthreads = (int)files->n;
        threads = (HANDLE *)xmalloc((size_t)nthreads * sizeof(*threads));
        pool.root = root;
        pool.softfloat = softfloat;
        pool.clang = clang;
        pool.target = target;
        pool.files = files;
        pool.results = results;
        pool.next = 0;
        for (i = 0; i < (size_t)nthreads; i++) {
            threads[i] = CreateThread(NULL, 0, tu_worker, &pool, 0, NULL);
        }
        WaitForMultipleObjects((DWORD)nthreads, threads, TRUE, INFINITE);
        for (i = 0; i < (size_t)nthreads; i++) CloseHandle(threads[i]);
        free(threads);
    } else
#endif
    {
        for (i = 0; i < files->n; i++) {
            unit_result *unit = parse_unit(root, softfloat, clang, target,
                                           files->v[i]);
            results[i] = *unit;
            free(unit);
        }
    }
    *out_results = results;
}

/* ------------------------------------------------------------------------- */
/* Context computation. */

typedef struct {
    unsigned char *closure;
    unsigned char *context;
    size_t nclosure;
    size_t ncontext;
} context_sets;

static int contains_name(const char *const *list, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (strcmp(list[i], name) == 0) return 1;
    }
    return 0;
}

static int compute_context(def_list *defs, context_sets *sets,
                           char *err, size_t errlen)
{
    size_t n = defs->n;
    int *queue = (int *)xmalloc(n * sizeof(*queue));
    size_t qh = 0;
    size_t qt = 0;
    size_t i;

    sets->closure = (unsigned char *)calloc(n ? n : 1, 1);
    sets->context = (unsigned char *)calloc(n ? n : 1, 1);
    sets->nclosure = 0;
    sets->ncontext = 0;
    for (i = 0; i < sizeof(ROOTS) / sizeof(ROOTS[0]); i++) {
        definition *d = def_find(defs, ROOTS[i]);
        if (!d) {
            snprintf(err, errlen,
                     "x87 roots missing from the SoftFloat source: %s",
                     ROOTS[i]);
            free(queue);
            return -1;
        }
        {
            size_t idx = 0;
            size_t k;
            for (k = 0; k < n; k++) {
                if (defs->v[k] == d) {
                    idx = k;
                    break;
                }
            }
            if (!sets->closure[idx]) {
                sets->closure[idx] = 1;
                sets->nclosure++;
                queue[qt++] = (int)idx;
            }
        }
    }
    while (qh < qt) {
        size_t idx = (size_t)queue[qh++];
        definition *d = defs->v[idx];
        for (i = 0; i < d->ncalls; i++) {
            definition *callee = def_find(defs, d->calls[i].callee);
            size_t k;
            if (!callee) continue;
            for (k = 0; k < n; k++) {
                if (defs->v[k] == callee) break;
            }
            if (k < n && !sets->closure[k]) {
                sets->closure[k] = 1;
                sets->nclosure++;
                queue[qt++] = (int)k;
            }
        }
    }
    for (i = 0; i < n; i++) {
        if (sets->closure[i] && defs->v[i]->nrefs) {
            sets->context[i] = 1;
            sets->ncontext++;
        }
    }
    for (;;) {
        int added = 0;
        for (i = 0; i < n; i++) {
            size_t k;
            if (!sets->closure[i] || sets->context[i]) continue;
            for (k = 0; k < defs->v[i]->ncalls; k++) {
                definition *callee = def_find(defs, defs->v[i]->calls[k].callee);
                if (!callee) continue;
                {
                    size_t ci;
                    for (ci = 0; ci < n; ci++) {
                        if (defs->v[ci] == callee) break;
                    }
                    if (ci < n && sets->context[ci]) {
                        sets->context[i] = 1;
                        sets->ncontext++;
                        added = 1;
                        break;
                    }
                }
            }
        }
        if (!added) break;
    }
    free(queue);
    return 0;
}

static int validate_context(def_list *defs, context_sets *sets,
                            char *err, size_t errlen)
{
    size_t n = defs->n;
    strvec missing = {0};
    strvec extra = {0};
    size_t i;
    for (i = 0; i < sizeof(EXPECTED_CONTEXT) / sizeof(EXPECTED_CONTEXT[0]); i++) {
        const char *name = EXPECTED_CONTEXT[i];
        definition *d = def_find(defs, name);
        size_t k;
        int found = 0;
        if (!d) {
            sv_push_copy(&missing, name);
            continue;
        }
        for (k = 0; k < n; k++) {
            if (defs->v[k] == d && sets->context[k]) {
                found = 1;
                break;
            }
        }
        if (!found) sv_push_copy(&missing, name);
    }
    for (i = 0; i < n; i++) {
        if (sets->context[i] &&
            !contains_name(EXPECTED_CONTEXT,
                           sizeof(EXPECTED_CONTEXT) / sizeof(EXPECTED_CONTEXT[0]),
                           defs->v[i]->name)) {
            sv_push_copy(&extra, defs->v[i]->name);
        }
    }
    if (missing.n || extra.n) {
        if (missing.n > 1) qsort(missing.v, missing.n, sizeof(*missing.v), cmp_str);
        if (extra.n > 1) qsort(extra.v, extra.n, sizeof(*extra.v), cmp_str);
        {
            size_t off = 0;
            off += (size_t)snprintf(err + off, errlen - off,
                                    "SoftFloat context set changed; review the new hot path before updating EXPECTED_CONTEXT (");
            if (missing.n) {
                size_t k;
                off += (size_t)snprintf(err + off, errlen - off, "missing: ");
                for (k = 0; k < missing.n; k++)
                    off += (size_t)snprintf(err + off, errlen - off,
                                            "%s%s", k ? ", " : "",
                                            missing.v[k]);
            }
            if (missing.n && extra.n)
                off += (size_t)snprintf(err + off, errlen - off, "; ");
            if (extra.n) {
                size_t k;
                off += (size_t)snprintf(err + off, errlen - off, "extra: ");
                for (k = 0; k < extra.n; k++)
                    off += (size_t)snprintf(err + off, errlen - off,
                                            "%s%s", k ? ", " : "",
                                            extra.v[k]);
            }
            snprintf(err + off, errlen - off, ")");
        }
        sv_free(&missing);
        sv_free(&extra);
        return -1;
    }
    sv_free(&missing);
    sv_free(&extra);
    for (i = 0; i < n; i++) {
        definition *d = defs->v[i];
        size_t k;
        if (!sets->context[i]) continue;
        if (strcmp(d->storage, "static") == 0) {
            snprintf(err, errlen,
                     "static context function is not supported: %s", d->name);
            return -1;
        }
        if (d->nindirect) {
            size_t off = (size_t)snprintf(
                err, errlen,
                "indirect call inside context function %s at offsets ",
                d->name);
            for (k = 0; k < d->nindirect; k++)
                off += (size_t)snprintf(err + off, errlen - off,
                                        "%s%lld", k ? ", " : "",
                                        d->indirect[k]);
            return -1;
        }
        for (k = 0; k < d->ncalls; k++) {
            definition *callee = def_find(defs, d->calls[k].callee);
            size_t ci;
            if (!callee) continue;
            for (ci = 0; ci < n; ci++) {
                if (defs->v[ci] == callee) break;
            }
            if (ci < n && sets->context[ci]) continue;
            if (callee->nrefs) {
                snprintf(err, errlen,
                         "%s calls state-using %s without context",
                         d->name, callee->name);
                return -1;
            }
        }
        for (k = 0; k < d->nrefs; k++) {
            if (!contains_name(STATE_NAMES,
                               sizeof(STATE_NAMES) / sizeof(STATE_NAMES[0]),
                               d->refs[k].name)) {
                snprintf(err, errlen,
                         "unexpected state name %s in %s",
                         d->refs[k].name, d->name);
                return -1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Source transformation. */

typedef struct {
    long long start;
    long long end;
    char *replacement;
    size_t seq;
} edit;

static int cmp_edit(const void *a, const void *b)
{
    const edit *ea = (const edit *)a;
    const edit *eb = (const edit *)b;
    if (ea->start != eb->start) return ea->start > eb->start ? -1 : 1;
    if (ea->end != eb->end) return ea->end > eb->end ? -1 : 1;
    if (ea->seq != eb->seq) return ea->seq < eb->seq ? -1 : 1;
    return 0;
}

static char *apply_edits(const char *source, size_t len, edit *edits,
                         size_t nedits)
{
    size_t i;
    long long last_start = (long long)len + 1;
    char *out = xstrndup(source, len);
    size_t out_len = len;
    if (nedits > 1) qsort(edits, nedits, sizeof(*edits), cmp_edit);
    for (i = 0; i < nedits; i++) {
        edit *e = &edits[i];
        size_t repl_len = strlen(e->replacement);
        char *next;
        if (e->end > last_start) {
            fprintf(stderr,
                    "softfloat_context_gen: error: overlapping generator "
                    "edits at %lld..%lld\n", e->start, e->end);
            free(out);
            return NULL;
        }
        if (e->start < 0 || e->end > (long long)out_len ||
            e->start > e->end) {
            fprintf(stderr,
                    "softfloat_context_gen: error: edit out of range\n");
            free(out);
            return NULL;
        }
        next = (char *)xmalloc(out_len - (size_t)(e->end - e->start) +
                               repl_len + 1);
        memcpy(next, out, (size_t)e->start);
        memcpy(next + e->start, e->replacement, repl_len);
        memcpy(next + e->start + repl_len, out + e->end,
               out_len - (size_t)e->end);
        out_len = out_len - (size_t)(e->end - e->start) + repl_len;
        next[out_len] = 0;
        free(out);
        out = next;
        last_start = e->start;
    }
    return out;
}

static long long last_include_end(const char *source, size_t len)
{
    long long i = 0;
    long long last = -1;
    while (i < (long long)len) {
        long long line_start = i;
        long long j = i;
        while (j < (long long)len && source[j] != '\n') j++;
        {
            long long k = line_start;
            while (k < j && (source[k] == ' ' || source[k] == '\t')) k++;
            if (k + 8 <= j && memcmp(source + k, "#include", 8) == 0)
                last = j < (long long)len ? j + 1 : j;
        }
        i = j < (long long)len ? j + 1 : j;
    }
    return last;
}

static char *wrapper_text(definition *d)
{
    size_t cap = 256 + strlen(d->name) * 4 + strlen(d->ret) +
                 strlen(d->args) + 1024;
    char *out = (char *)xmalloc(cap);
    size_t off = 0;
    size_t i;
    off += (size_t)snprintf(out + off, cap - off,
                            "\n\n/* Legacy TLS entry point; the hot x87 path "
                            "calls %s_ctx. */\n", d->name);
    off += (size_t)snprintf(out + off, cap - off, "%s %s( %s )\n{\n",
                            d->ret, d->name, d->args);
    off += (size_t)snprintf(out + off, cap - off, "    ");
    if (strcmp(d->ret, "void") == 0) {
        off += (size_t)snprintf(out + off, cap - off,
                                "%s_ctx( &softfloat_tls_state", d->name);
    } else {
        off += (size_t)snprintf(out + off, cap - off,
                                "return %s_ctx( &softfloat_tls_state",
                                d->name);
    }
    for (i = 0; i < d->params.n; i++) {
        off += (size_t)snprintf(out + off, cap - off, ", %s", d->params.v[i]);
    }
    off += (size_t)snprintf(out + off, cap - off, " );\n}\n");
    return out;
}

static char *prototype_text(definition *d)
{
    size_t cap = strlen(d->ret) + strlen(d->name) + strlen(d->args) + 128;
    char *out = (char *)xmalloc(cap);
    if (d->args[0]) {
        snprintf(out, cap, "%s %s_ctx( softfloat_state *st, %s );",
                 d->ret, d->name, d->args);
    } else {
        snprintf(out, cap, "%s %s_ctx( softfloat_state *st );",
                 d->ret, d->name);
    }
    return out;
}

static char *transform_file(tu_ctx *ctx, def_list *defs, context_sets *sets)
{
    edit *edits = NULL;
    size_t nedits = 0;
    size_t capedits = 0;
    size_t i;
    long long include_at = last_include_end(ctx->source, ctx->source_len);
    char *out;
    if (include_at < 0) {
        fprintf(stderr,
                "softfloat_context_gen: error: %s: no include line found\n",
                ctx->rel);
        return NULL;
    }
#define ADD_EDIT(S, E, R) do { \
    if (nedits == capedits) { \
        capedits = capedits ? capedits * 2 : 32; \
        edits = (edit *)xrealloc(edits, capedits * sizeof(*edits)); \
    } \
    edits[nedits].start = (S); \
    edits[nedits].end = (E); \
    edits[nedits].replacement = (R); \
    edits[nedits].seq = nedits; \
    nedits++; \
} while (0)

    ADD_EDIT(include_at, include_at,
             xstrdup("\n#include \"softfloat_ctx_private.h\"\n"));
    for (i = 0; i < defs->n; i++) {
        definition *d = defs->v[i];
        size_t k;
        char *name_repl;
        char *param_repl;
        char *wrapper;
        if (!sets->context[i] || strcmp(d->file, ctx->rel) != 0) continue;
        name_repl = (char *)xmalloc(strlen(d->name) + 5);
        sprintf(name_repl, "%s_ctx", d->name);
        ADD_EDIT(d->name_offset, d->name_offset + d->name_len, name_repl);
        if (d->args[0]) {
            param_repl = xstrdup(" softfloat_state *st,");
        } else {
            param_repl = xstrdup(" softfloat_state *st ");
        }
        ADD_EDIT(d->open_paren + 1, d->open_paren + 1, param_repl);
        for (k = 0; k < d->nrefs; k++) {
            size_t s;
            for (s = 0; s < sizeof(STATE_NAMES) / sizeof(STATE_NAMES[0]); s++) {
                if (strcmp(d->refs[k].name, STATE_NAMES[s]) == 0) {
                    size_t cap = strlen(STATE_FIELDS[s]) + 6;
                    char *repl = (char *)xmalloc(cap);
                    snprintf(repl, cap, "st->%s", STATE_FIELDS[s]);
                    ADD_EDIT(d->refs[k].offset,
                             d->refs[k].offset + (long long)strlen(d->refs[k].name),
                             repl);
                    break;
                }
            }
        }
        for (k = 0; k < d->ncalls; k++) {
            definition *callee = def_find(defs, d->calls[k].callee);
            size_t ci;
            char *repl;
            if (!callee) continue;
            for (ci = 0; ci < defs->n; ci++) {
                if (defs->v[ci] == callee) break;
            }
            if (ci >= defs->n || !sets->context[ci]) continue;
            repl = (char *)xmalloc(strlen(d->calls[k].callee) + 5);
            sprintf(repl, "%s_ctx", d->calls[k].callee);
            ADD_EDIT(d->calls[k].name_start,
                     d->calls[k].name_start +
                         (long long)strlen(d->calls[k].callee),
                     repl);
            if (ctx->source[d->calls[k].open_paren + 1] != ')') {
                ADD_EDIT(d->calls[k].open_paren + 1,
                         d->calls[k].open_paren + 1, xstrdup("st, "));
            } else {
                ADD_EDIT(d->calls[k].open_paren + 1,
                         d->calls[k].open_paren + 1, xstrdup("st"));
            }
        }
        wrapper = wrapper_text(d);
        ADD_EDIT(d->body_end + 1, d->body_end + 1, wrapper);
    }
#undef ADD_EDIT
    if (nedits == 0) {
        free(edits);
        return xstrdup(ctx->source);
    }
    out = apply_edits(ctx->source, ctx->source_len, edits, nedits);
    for (i = 0; i < nedits; i++) free(edits[i].replacement);
    free(edits);
    return out;
}

static char *add_state_header(const char *source)
{
    const char *marker = "#include \"softfloat.h\"";
    const char *found = strstr(source, marker);
    size_t n;
    char *out;
    size_t prefix;
    if (!found) return xstrdup(source);
    prefix = (size_t)(found - source) + strlen(marker);
    n = strlen(source) + strlen("\n#include \"softfloat_state.h\"") + 1;
    out = (char *)xmalloc(n);
    memcpy(out, source, prefix);
    memcpy(out + prefix, "\n#include \"softfloat_state.h\"",
           strlen("\n#include \"softfloat_state.h\""));
    strcpy(out + prefix + strlen("\n#include \"softfloat_state.h\""),
           source + prefix);
    return out;
}

static void write_headers(const char *out, def_list *defs, context_sets *sets,
                          strvec *context_names)
{
    strvec public_protos = {0};
    strvec private_protos = {0};
    strvec public_names = {0};
    size_t i;
    char *path;
    char *text;
    size_t cap;
    size_t off;
    for (i = 0; i < context_names->n; i++) {
        definition *d = def_find(defs, context_names->v[i]);
        if (!d) continue;
        sv_push(&private_protos, prototype_text(d));
        if (contains_name(X87_PUBLIC,
                          sizeof(X87_PUBLIC) / sizeof(X87_PUBLIC[0]),
                          d->name)) {
            sv_push(&public_names, xstrdup(d->name));
            sv_push(&public_protos, prototype_text(d));
        }
    }
    (void)public_names;
    cap = 512;
    for (i = 0; i < public_protos.n; i++)
        cap += strlen(public_protos.v[i]) + 2;
    text = (char *)xmalloc(cap);
    off = 0;
    off += (size_t)snprintf(text + off, cap - off,
                            "/* Generated by tools/lift/csrc_local/softfloat_context_gen.c; do not edit. */\n"
                            "#ifndef WC3_SOFTFLOAT_CTX_H\n"
                            "#define WC3_SOFTFLOAT_CTX_H\n"
                            "#include \"softfloat_state.h\"\n"
                            "#ifdef __cplusplus\n"
                            "extern \"C\" {\n"
                            "#endif\n\n");
    for (i = 0; i < public_protos.n; i++)
        off += (size_t)snprintf(text + off, cap - off, "%s\n",
                                public_protos.v[i]);
    off += (size_t)snprintf(text + off, cap - off,
                            "\n#ifdef __cplusplus\n}\n#endif\n#endif\n");
    path = path_join(out, "softfloat_ctx.h");
    write_text_cstr(path, text);
    free(path);
    free(text);

    cap = 512;
    for (i = 0; i < private_protos.n; i++)
        cap += strlen(private_protos.v[i]) + 2;
    text = (char *)xmalloc(cap);
    off = 0;
    off += (size_t)snprintf(text + off, cap - off,
                            "/* Generated by tools/lift/csrc_local/softfloat_context_gen.c; do not edit.\n"
                            " * Included by each generated context source so internal _ctx calls\n"
                            " * have prototypes.  x87 includes only the public header. */\n"
                            "#ifndef WC3_SOFTFLOAT_CTX_PRIVATE_H\n"
                            "#define WC3_SOFTFLOAT_CTX_PRIVATE_H\n"
                            "#include \"platform.h\"\n"
                            "#include \"internals.h\"\n"
                            "#include \"specialize.h\"\n"
                            "#include \"softfloat_ctx.h\"\n"
                            "#ifdef __cplusplus\n"
                            "extern \"C\" {\n"
                            "#endif\n\n");
    for (i = 0; i < private_protos.n; i++)
        off += (size_t)snprintf(text + off, cap - off, "%s\n",
                                private_protos.v[i]);
    off += (size_t)snprintf(text + off, cap - off,
                            "\n#ifdef __cplusplus\n}\n#endif\n#endif\n");
    path = path_join(out, "softfloat_ctx_private.h");
    write_text_cstr(path, text);
    free(path);
    free(text);

    sv_free(&public_names);
    sv_free(&public_protos);
    sv_free(&private_protos);
}

static void write_state_source(const char *out)
{
    char *path = path_join(out, "source/softfloat_state.c");
    const char *text =
        "/* Generated by tools/lift/csrc_local/softfloat_context_gen.c; do not edit. */\n"
        "#include \"platform.h\"\n"
        "#include \"internals.h\"\n"
        "#include \"specialize.h\"\n"
        "#include \"softfloat.h\"\n"
        "#include \"softfloat_state.h\"\n\n"
        "THREAD_LOCAL softfloat_state softfloat_tls_state = {\n"
        "    softfloat_round_near_even,\n"
        "    init_detectTininess,\n"
        "    0,\n"
        "    80\n"
        "};\n";
    write_text_cstr(path, text);
    free(path);
}

static void write_sources_cmake(const char *out, strvec *files)
{
    char *path = path_join(out, "sources.cmake");
    char *posix = xstrdup(out);
    size_t i;
    size_t cap = 256;
    size_t off = 0;
    char *text;
    for (i = 0; posix[i]; i++) if (posix[i] == '\\') posix[i] = '/';
    for (i = 0; i < files->n; i++)
        cap += strlen(files->v[i]) + strlen(posix) + 16;
    text = (char *)xmalloc(cap);
    off += (size_t)snprintf(text + off, cap - off,
                            "# Generated by tools/lift/csrc_local/softfloat_context_gen.c; do not edit.\n"
                            "set(WC3_SOFTFLOAT_CTX_SOURCES\n");
    for (i = 0; i < files->n; i++) {
        off += (size_t)snprintf(text + off, cap - off, "    \"%s/%s\"\n",
                                posix, files->v[i]);
    }
    off += (size_t)snprintf(text + off, cap - off, ")\n");
    write_text_cstr(path, text);
    free(text);
    free(posix);
    free(path);
}

static void json_write_str_array(FILE *f, strvec *v, int indent)
{
    size_t i;
    if (v->n == 0) {
        fputs("[]", f);
        return;
    }
    fputs("[\n", f);
    for (i = 0; i < v->n; i++) {
        int k;
        for (k = 0; k < indent + 2; k++) fputc(' ', f);
        fputc('"', f);
        fputs(v->v[i], f);
        fputc('"', f);
        if (i + 1 < v->n) fputc(',', f);
        fputc('\n', f);
    }
    for (i = 0; i < (size_t)indent; i++) fputc(' ', f);
    fputc(']', f);
}

static void write_report(const char *out, def_list *defs, context_sets *sets,
                         strvec *files, strvec *context_names)
{
    char *path = path_join(out, "report.json");
    strvec closure_names = {0};
    strvec roots = {0};
    FILE *f;
    size_t i;
    for (i = 0; i < defs->n; i++)
        if (sets->closure[i]) sv_push_copy(&closure_names, defs->v[i]->name);
    if (closure_names.n > 1)
        qsort(closure_names.v, closure_names.n, sizeof(*closure_names.v), cmp_str);
    for (i = 0; i < sizeof(ROOTS) / sizeof(ROOTS[0]); i++)
        sv_push_copy(&roots, ROOTS[i]);
    f = fopen(path, "w");
    if (!f) {
        free(path);
        sv_free(&closure_names);
        sv_free(&roots);
        return;
    }
    fputs("{\n  \"context\": ", f);
    json_write_str_array(f, context_names, 2);
    fputs(",\n  \"closure\": ", f);
    json_write_str_array(f, &closure_names, 2);
    fputs(",\n  \"roots\": ", f);
    json_write_str_array(f, &roots, 2);
    fprintf(f, ",\n  \"definitions\": %zu,\n  \"sources\": %zu\n}\n",
            defs->n, files->n);
    fclose(f);
    free(path);
    sv_free(&closure_names);
    sv_free(&roots);
}

/* ------------------------------------------------------------------------- */
/* Manifest. */

static int read_manifest(const char *softfloat, strvec *files)
{
    char *manifest = path_join(softfloat, "android_sources.txt");
    size_t len = 0;
    char *text = read_text(manifest, &len);
    char *p;
    free(manifest);
    if (!text) return -1;
    p = text;
    while (*p) {
        char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        char *line = xstrndup(p, n);
        char *s = line;
        size_t k;
        while (*s && isspace((unsigned char)*s)) s++;
        n = strlen(s);
        while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
        if (*s && *s != '#') {
            for (k = 0; k < n; k++) if (s[k] == '\\') s[k] = '/';
            {
                char *full = path_join(softfloat, s);
                if (!is_file(full)) {
                    fprintf(stderr,
                            "softfloat_context_gen: error: missing SoftFloat "
                            "source: %s\n", full);
                    free(full);
                    free(line);
                    free(text);
                    return -1;
                }
                free(full);
            }
            sv_push_copy(files, s);
        }
        free(line);
        if (!end) break;
        p = end + 1;
    }
    free(text);
    if (files->n != 302) {
        fprintf(stderr,
                "softfloat_context_gen: error: SoftFloat manifest has %zu "
                "sources, expected 302\n", files->n);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Driver. */

int main(int argc, char **argv)
{
    const char *root_arg = NULL;
    const char *out_arg = NULL;
    const char *clang = NULL;
    const char *target = "aarch64-linux-android26";
    int jobs = 8;
    int i;
    char *root;
    char *softfloat;
    char *out;
    strvec files = {0};
    unit_result *results = NULL;
    def_list defs = {0};
    context_sets sets;
    strvec context_names = {0};
    char err[1024];
    int rc = 1;

    memset(&sets, 0, sizeof(sets));
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--root") == 0 && i + 1 < argc) root_arg = argv[++i];
        else if (strncmp(a, "--root=", 7) == 0) root_arg = a + 7;
        else if (strcmp(a, "--out") == 0 && i + 1 < argc) out_arg = argv[++i];
        else if (strncmp(a, "--out=", 6) == 0) out_arg = a + 6;
        else if (strcmp(a, "--clang") == 0 && i + 1 < argc) clang = argv[++i];
        else if (strncmp(a, "--clang=", 8) == 0) clang = a + 8;
        else if (strcmp(a, "--target") == 0 && i + 1 < argc) target = argv[++i];
        else if (strncmp(a, "--target=", 9) == 0) target = a + 9;
        else if (strcmp(a, "--jobs") == 0 && i + 1 < argc) jobs = atoi(argv[++i]);
        else if (strncmp(a, "--jobs=", 7) == 0) jobs = atoi(a + 7);
        else {
            fprintf(stderr,
                    "usage: softfloat_context_gen --root DIR --out DIR "
                    "--clang FILE [--target T] [--jobs N]\n");
            return 2;
        }
    }
    if (!out_arg || !clang) {
        fprintf(stderr,
                "usage: softfloat_context_gen --root DIR --out DIR "
                "--clang FILE [--target T] [--jobs N]\n");
        return 2;
    }
    if (!root_arg) {
        char *exe = path_resolve(argv[0]);
        char *dir = path_dirname(exe);
        char *dir2 = path_dirname(dir);
        root = path_dirname(dir2);
        free(dir2);
        free(dir);
        free(exe);
    } else {
        root = path_resolve(root_arg);
    }
    softfloat = path_join(root, "third_party/softfloat");
    out = path_resolve(out_arg);
    if (path_equal(out, root) || path_equal(out, softfloat)) {
        fprintf(stderr,
                "softfloat_context_gen: error: refusing to generate into "
                "the source tree\n");
        free(root);
        free(softfloat);
        free(out);
        return 1;
    }
    if (read_manifest(softfloat, &files) != 0) {
        free(root);
        free(softfloat);
        free(out);
        sv_free(&files);
        return 1;
    }
    if (jobs < 1) jobs = 1;
    if (jobs > 32) jobs = 32;
    parse_all(root, softfloat, clang, target, &files, jobs, &results);
    for (i = 0; i < (int)files.n; i++) {
        unit_result *r = &results[i];
        size_t k;
        if (r->error) {
            fprintf(stderr, "softfloat_context_gen: error: %s\n", r->error);
            goto cleanup;
        }
        for (k = 0; k < r->defs.n; k++) {
            definition *d = r->defs.v[k];
            definition *existing = def_find(&defs, d->name);
            if (existing) {
                fprintf(stderr,
                        "softfloat_context_gen: error: duplicate SoftFloat "
                        "definition %s: %s and %s\n",
                        d->name, existing->file, d->file);
                goto cleanup;
            }
            def_list_add(&defs, d);
            r->defs.v[k] = NULL;
        }
    }
    for (i = 0; i < (int)files.n; i++) {
        size_t k;
        for (k = 0; k < results[i].defs.n; k++) {
            if (results[i].defs.v[k]) definition_free(results[i].defs.v[k]);
        }
        free(results[i].defs.v);
        free(results[i].rel);
        free(results[i].error);
    }
    free(results);
    results = NULL;
    if (defs.n != 287) {
        fprintf(stderr,
                "softfloat_context_gen: error: parsed %zu SoftFloat "
                "definitions, expected 287\n", defs.n);
        goto cleanup;
    }
    if (compute_context(&defs, &sets, err, sizeof(err)) != 0) {
        fprintf(stderr, "softfloat_context_gen: error: %s\n", err);
        goto cleanup;
    }
    if (validate_context(&defs, &sets, err, sizeof(err)) != 0) {
        fprintf(stderr, "softfloat_context_gen: error: %s\n", err);
        goto cleanup;
    }
    for (i = 0; i < (int)defs.n; i++) {
        if (sets.context[i]) sv_push_copy(&context_names, defs.v[i]->name);
    }
    if (context_names.n > 1)
        qsort(context_names.v, context_names.n, sizeof(*context_names.v), cmp_str);

    if (is_dir(out) && remove_tree(out) != 0) {
        fprintf(stderr,
                "softfloat_context_gen: error: cannot remove %s\n", out);
        goto cleanup;
    }
    {
        char *source_dir = path_join(out, "source");
        if (make_dirs(source_dir) != 0) {
            fprintf(stderr,
                    "softfloat_context_gen: error: cannot create %s\n",
                    source_dir);
            free(source_dir);
            goto cleanup;
        }
        free(source_dir);
    }
    for (i = 0; i < (int)files.n; i++) {
        const char *rel = files.v[i];
        char *original = path_join(softfloat, rel);
        char *target = path_join(out, rel);
        char *target_dir = path_dirname(target);
        char *source;
        size_t source_len = 0;
        char *base = path_basename(rel);
        make_dirs(target_dir);
        free(target_dir);
        source = read_text(original, &source_len);
        if (!source) {
            fprintf(stderr,
                    "softfloat_context_gen: error: cannot read %s\n", original);
            free(original);
            free(target);
            free(base);
            goto cleanup;
        }
        if (strcmp(base, "softfloat_state.c") == 0) {
            write_state_source(out);
        } else {
            int has_context = 0;
            size_t k;
            for (k = 0; k < defs.n; k++) {
                if (sets.context[k] && strcmp(defs.v[k]->file, rel) == 0) {
                    has_context = 1;
                    break;
                }
            }
            if (has_context) {
                tu_ctx ctx;
                char *transformed;
                memset(&ctx, 0, sizeof(ctx));
                ctx.rel = rel;
                ctx.base = base;
                ctx.source = source;
                ctx.source_len = source_len;
                ctx.abs_path = original;
                ctx.defs = &defs;
                transformed = transform_file(&ctx, &defs, &sets);
                if (!transformed) {
                    free(source);
                    free(original);
                    free(target);
                    free(base);
                    goto cleanup;
                }
                if (write_text(target, transformed, strlen(transformed)) != 0) {
                    fprintf(stderr,
                            "softfloat_context_gen: error: cannot write %s\n",
                            target);
                    free(transformed);
                    free(source);
                    free(original);
                    free(target);
                    free(base);
                    goto cleanup;
                }
                free(transformed);
            } else {
                char *with_header = add_state_header(source);
                if (write_text(target, with_header, strlen(with_header)) != 0) {
                    fprintf(stderr,
                            "softfloat_context_gen: error: cannot write %s\n",
                            target);
                    free(with_header);
                    free(source);
                    free(original);
                    free(target);
                    free(base);
                    goto cleanup;
                }
                free(with_header);
            }
        }
        free(source);
        free(original);
        free(target);
        free(base);
    }
    write_headers(out, &defs, &sets, &context_names);
    write_sources_cmake(out, &files);
    write_report(out, &defs, &sets, &files, &context_names);
    printf("softfloat_context_gen: %zu context functions, %zu closure "
           "functions, %zu sources -> %s\n",
           context_names.n, sets.nclosure, files.n, out);
    rc = 0;

cleanup:
    if (results) {
        for (i = 0; i < (int)files.n; i++) {
            size_t k;
            for (k = 0; k < results[i].defs.n; k++) {
                if (results[i].defs.v[k]) definition_free(results[i].defs.v[k]);
            }
            free(results[i].defs.v);
            free(results[i].rel);
            free(results[i].error);
        }
        free(results);
    }
    def_list_free(&defs);
    sv_free(&files);
    sv_free(&context_names);
    free(sets.closure);
    free(sets.context);
    free(root);
    free(softfloat);
    free(out);
    return rc;
}
