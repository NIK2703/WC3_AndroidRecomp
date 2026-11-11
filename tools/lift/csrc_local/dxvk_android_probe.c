/* dxvk_android_probe.c - compile vendored DXVK trees for aarch64 Android.
 *
 * This is the C implementation of the Android DXVK compile probe.  It
 * replays the source list and per-file flags from the pinned Meson
 * compile_commands.json, swaps the Windows compiler for the NDK clang, and
 * writes real aarch64 object files.  The archives are still assembled by
 * tools/android/build_dxvk.ps1.
 *
 * Usage:
 *   dxvk_android_probe --dxvk third_party/dxvk/3.1.1 --ndk <ndk> \
 *       --codegen --jobs 12 --stale-only [--ccache <ccache>]
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
#else
#  include <dirent.h>
#  include <unistd.h>
#  define WC3_MKDIR(p) mkdir((p), 0777)
#endif

#define WC3_PATH_MAX 4096
#define DEFAULT_NDK "C:/Users/Nikita/AppData/Local/Android/Sdk/ndk/29.0.14206865"

static const char *const DROP[] = {
    "-msse", "-msse2", "-msse3", "-mfpmath=sse",
    "-MD", "-Winvalid-pch", "-fdiagnostics-color=always",
    "-O3", "-O2", "-O1", "-std=c++17", "-std=c11", "-std=c17"
};

static const char *const DROP_WITH_ARG[] = {"-MQ", "-MF", "-o"};

typedef struct {
    char **v;
    size_t n;
    size_t cap;
} strvec;

typedef struct {
    char *file;
    char *command;
} entry;

typedef struct {
    entry *v;
    size_t n;
    size_t cap;
} entry_list;

typedef struct {
    int ok;
    char *file;
    char *error;
} compile_result;

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "dxvk_android_probe: out of memory\n");
        exit(2);
    }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "dxvk_android_probe: out of memory\n");
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

static char *to_slashes(const char *s)
{
    char *out = xstrdup(s);
    char *p;
    for (p = out; *p; p++) if (*p == '\\') *p = '/';
    return out;
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

static uint64_t file_mtime(const char *path)
{
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return 0;
    return ((uint64_t)data.ftLastWriteTime.dwHighDateTime << 32) |
           data.ftLastWriteTime.dwLowDateTime;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint64_t)st.st_mtime * 1000000000ULL;
#endif
}

/* ------------------------------------------------------------------------- */
/* Minimal JSON DOM for compile_commands.json. */

typedef enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ } jv_type;

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
    jp->p++;
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
    jp->p++;
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
    if (jp->depth++ > 2000) {
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
        v->inum = strtoll(copy, NULL, 10);
        free(copy);
    }
    jp->depth--;
    return v;
}

static jval *json_parse(const char *text, size_t len)
{
    jparser jp;
    jval *v;
    jp.p = text;
    jp.end = text + len;
    jp.error = 0;
    jp.depth = 0;
    jp_skip_ws(&jp);
    v = jp_value(&jp);
    if (!v) return NULL;
    jp_skip_ws(&jp);
    if (jp.p != jp.end) {
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

/* ------------------------------------------------------------------------- */
/* Command preprocessing. */

static int split_shlex(const char *line, strvec *out, char *err, size_t errlen)
{
    const char *p = line;
    while (*p) {
        char *buf;
        size_t n = 0;
        size_t cap = 32;
        int quote = 0;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        buf = (char *)xmalloc(cap);
        while (*p) {
            char c = *p;
            if (quote == 0) {
                if (isspace((unsigned char)c)) break;
                if (c == '\'') {
                    quote = 1;
                    p++;
                    continue;
                }
                if (c == '"') {
                    quote = 2;
                    p++;
                    continue;
                }
                if (c == '\\' && p[1]) {
                    c = p[1];
                    p += 2;
                } else {
                    p++;
                }
            } else if (quote == 1) {
                if (c == '\'') {
                    quote = 0;
                    p++;
                    continue;
                }
                p++;
            } else {
                if (c == '"') {
                    quote = 0;
                    p++;
                    continue;
                }
                if (c == '\\' && p[1] &&
                    (p[1] == '"' || p[1] == '\\' || p[1] == '$' ||
                     p[1] == '`' || p[1] == '\n')) {
                    c = p[1];
                    p += 2;
                } else {
                    p++;
                }
            }
            if (n + 2 > cap) {
                cap *= 2;
                buf = (char *)xrealloc(buf, cap);
            }
            buf[n++] = c;
        }
        if (quote != 0) {
            free(buf);
            snprintf(err, errlen, "unterminated quote");
            return -1;
        }
        buf[n] = 0;
        sv_push(out, buf);
    }
    return 0;
}

static int in_list(const char *const *list, size_t n, const char *s)
{
    size_t i;
    for (i = 0; i < n; i++) if (strcmp(list[i], s) == 0) return 1;
    return 0;
}

static void rewrite_command(const char *command, strvec *out,
                            char *err, size_t errlen)
{
    char *slashed = to_slashes(command);
    strvec args = {0};
    size_t i;
    int skip = 0;
    if (split_shlex(slashed, &args, err, errlen) != 0) {
        free(slashed);
        return;
    }
    free(slashed);
    for (i = 1; i < args.n; i++) {
        const char *a = args.v[i];
        if (skip) {
            skip = 0;
            continue;
        }
        if (in_list(DROP_WITH_ARG,
                    sizeof(DROP_WITH_ARG) / sizeof(DROP_WITH_ARG[0]), a)) {
            skip = 1;
            continue;
        }
        if (strncmp(a, "-o", 2) == 0) continue;
        if (in_list(DROP, sizeof(DROP) / sizeof(DROP[0]), a)) continue;
        sv_push_copy(out, a);
    }
    sv_free(&args);
}

static int ends_with(const char *s, const char *suffix)
{
    size_t ns = strlen(s);
    size_t nx = strlen(suffix);
    return ns >= nx && strcmp(s + ns - nx, suffix) == 0;
}

static char *replace_clangxx(const char *clang)
{
    const char *at = strstr(clang, "clang++");
    if (!at) return xstrdup(clang);
    {
        size_t prefix = (size_t)(at - clang);
        size_t suffix = strlen(at + strlen("clang++"));
        char *out = (char *)xmalloc(prefix + strlen("clang") + suffix + 1);
        memcpy(out, clang, prefix);
        memcpy(out + prefix, "clang", strlen("clang"));
        memcpy(out + prefix + strlen("clang"), at + strlen("clang++"),
               suffix + 1);
        return out;
    }
}

static char *object_path(const char *objdir, const char *file)
{
    char *slashed = to_slashes(file);
    char *p;
    char *out;
    size_t n;
    /* Remove all occurrences of "../". */
    while ((p = strstr(slashed, "../")) != NULL) {
        memmove(p, p + 3, strlen(p + 3) + 1);
    }
    for (p = slashed; *p; p++) if (*p == '/') *p = '_';
    n = strlen(objdir) + strlen(slashed) + 5;
    out = (char *)xmalloc(n);
    snprintf(out, n, "%s/%s.o", objdir, slashed);
    free(slashed);
    return out;
}

static void build_command_argv(const char *clang, const char *sysroot,
                               const char *shim, const char *directx,
                               entry *e, int api, const char *ccache_wrapper,
                               const char *ccache, const char *obj,
                               strvec *cmd, char *err, size_t errlen)
{
    strvec args = {0};
    char api_arg[128];
    char sysroot_arg[WC3_PATH_MAX];
    char *compiler;
    int is_c = ends_with(e->file, ".c");

    rewrite_command(e->command, &args, err, errlen);
    if (err[0]) {
        sv_free(&args);
        return;
    }
    compiler = is_c ? replace_clangxx(clang) : xstrdup(clang);
    snprintf(api_arg, sizeof(api_arg), "--target=aarch64-linux-android%d", api);
    snprintf(sysroot_arg, sizeof(sysroot_arg), "--sysroot=%s", sysroot);

    sv_push(cmd, compiler);
    sv_push_copy(cmd, api_arg);
    sv_push_copy(cmd, sysroot_arg);
    if (is_c) {
        sv_push_copy(cmd, "-std=c11");
    } else {
        sv_push_copy(cmd, "-std=c++17");
    }
    sv_push_copy(cmd, "-O0");
    sv_push_copy(cmd, "-fPIC");
    if (!is_c) {
        sv_push_copy(cmd, "-fshort-wchar");
        sv_push_copy(cmd, "-fdeclspec");
    }
    sv_push_copy(cmd, "-I");
    sv_push_copy(cmd, shim);
    sv_push_copy(cmd, "-I");
    sv_push_copy(cmd, directx);
    sv_push_copy(cmd, "-ferror-limit=0");
    {
        size_t i;
        for (i = 0; i < args.n; i++) sv_push_copy(cmd, args.v[i]);
    }
    if (obj) {
        sv_push_copy(cmd, "-c");
        sv_push_copy(cmd, "-o");
        sv_push_copy(cmd, obj);
    } else {
        sv_push_copy(cmd, "-fsyntax-only");
    }
    if (ccache) {
        strvec prefixed = {0};
        size_t i;
        sv_push_copy(&prefixed, ccache_wrapper);
        sv_push_copy(&prefixed, ccache);
        for (i = 0; i < cmd->n; i++) sv_push_copy(&prefixed, cmd->v[i]);
        sv_free(cmd);
        *cmd = prefixed;
    }
    sv_free(&args);
}

/* ------------------------------------------------------------------------- */
/* Process execution with a private capture file. */

static char *quote_arg(const char *s)
{
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    size_t i;
    size_t extra = 2;
    char *out;
    char *p;
    for (i = 0; s[i]; i++) if (s[i] == '"') extra++;
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
    for (i = 0; s[i]; i++) if (s[i] == '\'') extra += 3;
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

static char *build_cmdline(strvec *argv)
{
    size_t cap = 512;
    size_t len = 0;
    char *cmd = (char *)xmalloc(cap);
    size_t i;
    cmd[0] = 0;
    for (i = 0; i < argv->n; i++) {
        char *q = quote_arg(argv->v[i]);
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
    return cmd;
}

#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
static int run_capture(strvec *argv, char **out_text, int *out_exit)
{
    char *cmd = build_cmdline(argv);
    SECURITY_ATTRIBUTES sa;
    HANDLE read_h = NULL;
    HANDLE write_h = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    char *text = NULL;
    size_t len = 0;
    size_t cap = 0;
    char chunk[8192];
    DWORD got;

    *out_text = NULL;
    *out_exit = 1;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&read_h, &write_h, &sa, 0)) {
        free(cmd);
        return -1;
    }
    if (!SetHandleInformation(read_h, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(read_h);
        CloseHandle(write_h);
        free(cmd);
        return -1;
    }
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    if (si.hStdInput == INVALID_HANDLE_VALUE) si.hStdInput = NULL;
    si.hStdOutput = write_h;
    si.hStdError = write_h;
    ZeroMemory(&pi, sizeof(pi));
    /*
     * Match Python's subprocess.run(capture_output=True): pipes and no
     * CREATE_NO_WINDOW.  MSYS2 ccache uses its POSIX fork/exec path; a temp
     * file on stdin and the hidden-console creation flag left its children
     * suspended.  Creating the child on the parent's console with real
     * pipes keeps the fork handshake working.
     */
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0,
                        NULL, NULL, &si, &pi)) {
        DWORD err = GetLastError();
        fprintf(stderr,
                "dxvk_android_probe: CreateProcess failed (%lu), cmdlen=%zu\n",
                (unsigned long)err, strlen(cmd));
        fprintf(stderr, "cmd=%s\n", cmd);
        CloseHandle(read_h);
        CloseHandle(write_h);
        free(cmd);
        return -1;
    }
    CloseHandle(write_h);
    free(cmd);

    for (;;) {
        if (!ReadFile(read_h, chunk, sizeof(chunk), &got, NULL) || got == 0) {
            break;
        }
        if (len + got + 1 > cap) {
            cap = cap ? cap * 2 : 8192;
            while (cap < len + got + 1) cap *= 2;
            text = (char *)xrealloc(text, cap);
        }
        memcpy(text + len, chunk, got);
        len += got;
    }
    CloseHandle(read_h);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (!text) text = xstrdup("");
    else text[len] = 0;
    *out_text = text;
    *out_exit = (int)code;
    return 0;
}
#else
static int run_capture(strvec *argv, char **out_text, int *out_exit)
{
    char *cmd = build_cmdline(argv);
    FILE *f = popen(cmd, "r");
    char *out = NULL;
    size_t n = 0;
    size_t cap = 0;
    char chunk[8192];
    size_t got;
    int status;
    free(cmd);
    if (!f) return -1;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (n + got + 1 > cap) {
            cap = cap ? cap * 2 : 8192;
            while (cap < n + got + 1) cap *= 2;
            out = (char *)xrealloc(out, cap);
        }
        memcpy(out + n, chunk, got);
        n += got;
    }
    status = pclose(f);
    if (!out) out = xstrdup("");
    else out[n] = 0;
    *out_text = out;
    *out_exit = status == 0 ? 0 : 1;
    return 0;
}
#endif

static void strip_eol(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

static char *trim(char *s)
{
    size_t n;
    while (*s && isspace((unsigned char)*s)) s++;
    n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

static char *extract_error(const char *text)
{
    const char *p = text;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char *line = xstrndup(p, len);
        char *at;
        strip_eol(line);
        at = strstr(line, "error:");
        if (at) {
            char *msg = xstrdup(trim(at + 6));
            free(line);
            return msg;
        }
        free(line);
        if (!end) break;
        p = end + 1;
    }
    return xstrdup("unknown error");
}

static compile_result compile_one(entry *e, const char *clang,
                                  const char *sysroot, const char *shim,
                                  const char *directx, int api,
                                  const char *ccache_wrapper,
                                  const char *ccache, const char *objdir)
{
    compile_result r;
    strvec cmd = {0};
    char err[256] = {0};
    char *obj = objdir ? object_path(objdir, e->file) : NULL;
    char *output = NULL;
    int code = 1;
    memset(&r, 0, sizeof(r));
    r.file = e->file;
    build_command_argv(clang, sysroot, shim, directx, e, api,
                       ccache_wrapper, ccache, obj, &cmd, err, sizeof(err));
    if (err[0]) {
        r.error = xstrdup(err);
        free(obj);
        sv_free(&cmd);
        return r;
    }
    if (run_capture(&cmd, &output, &code) != 0) {
        r.error = xstrdup("failed to start compiler");
        free(output);
        free(obj);
        sv_free(&cmd);
        return r;
    }
    if (code == 0) {
        r.ok = 1;
        r.error = NULL;
    } else {
        r.error = extract_error(output);
    }
    free(output);
    free(obj);
    sv_free(&cmd);
    return r;
}

/* ------------------------------------------------------------------------- */
/* Entry loading and filtering. */

static void entry_list_add(entry_list *v, entry e)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->v = (entry *)xrealloc(v->v, v->cap * sizeof(*v->v));
    }
    v->v[v->n++] = e;
}

static void entry_list_free(entry_list *v)
{
    size_t i;
    for (i = 0; i < v->n; i++) {
        free(v->v[i].file);
        free(v->v[i].command);
    }
    free(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

static int load_entries(const char *path, entry_list *out)
{
    size_t len = 0;
    char *text = read_text(path, &len);
    jval *root;
    size_t i;
    if (!text) return -1;
    root = json_parse(text, len);
    free(text);
    if (!root || root->type != JV_ARR) {
        jv_free(root);
        return -1;
    }
    for (i = 0; i < root->nitems; i++) {
        jval *o = root->items[i];
        const char *file = jstr(jget(o, "file"));
        const char *command = jstr(jget(o, "command"));
        if (file && command) {
            entry e;
            e.file = xstrdup(file);
            e.command = xstrdup(command);
            entry_list_add(out, e);
        }
    }
    jv_free(root);
    return 0;
}

static int contains_slashed(const char *hay, const char *needle)
{
    char *h = to_slashes(hay);
    char *n = to_slashes(needle);
    int found = strstr(h, n) != NULL;
    free(h);
    free(n);
    return found;
}

static int any_contains(strvec *needles, const char *hay)
{
    size_t i;
    for (i = 0; i < needles->n; i++)
        if (contains_slashed(hay, needles->v[i])) return 1;
    return 0;
}

static char *find_build_dir(const char *dxvk)
{
    static const char *const names[] = {
        "build-win64-ucrt", "build-win64", "build"
    };
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char *dir = path_join(dxvk, names[i]);
        char *cc = path_join(dir, "compile_commands.json");
        int ok = is_file(cc);
        free(cc);
        if (ok) return dir;
        free(dir);
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Parallel compile driver. */

#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
typedef struct {
    entry *entries;
    size_t n;
    compile_result *results;
    const char *clang;
    const char *sysroot;
    const char *shim;
    const char *directx;
    int api;
    const char *ccache_wrapper;
    const char *ccache;
    const char *objdir;
    volatile LONG next;
} job_pool;

static unsigned __stdcall worker(void *arg)
{
    job_pool *p = (job_pool *)arg;
    for (;;) {
        LONG i = InterlockedIncrement(&p->next) - 1;
        if ((size_t)i >= p->n) break;
        p->results[i] = compile_one(&p->entries[i], p->clang, p->sysroot,
                                    p->shim, p->directx, p->api,
                                    p->ccache_wrapper, p->ccache, p->objdir);
    }
    return 0;
}
#endif

static void run_jobs(entry_list *entries, int jobs, const char *clang,
                     const char *sysroot, const char *shim,
                     const char *directx, int api,
                     const char *ccache_wrapper, const char *ccache,
                     const char *objdir, compile_result **out_results)
{
    compile_result *results = (compile_result *)calloc(
        entries->n ? entries->n : 1, sizeof(*results));
    size_t i;
    if (!results) {
        fprintf(stderr, "dxvk_android_probe: out of memory\n");
        exit(2);
    }
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    if (jobs > 1 && entries->n > 0) {
        job_pool p;
        HANDLE *threads;
        int nthreads = jobs;
        if ((size_t)nthreads > entries->n) nthreads = (int)entries->n;
        threads = (HANDLE *)xmalloc((size_t)nthreads * sizeof(*threads));
        p.entries = entries->v;
        p.n = entries->n;
        p.results = results;
        p.clang = clang;
        p.sysroot = sysroot;
        p.shim = shim;
        p.directx = directx;
        p.api = api;
        p.ccache_wrapper = ccache_wrapper;
        p.ccache = ccache;
        p.objdir = objdir;
        p.next = 0;
        for (i = 0; i < (size_t)nthreads; i++) {
            threads[i] = (HANDLE)_beginthreadex(NULL, 0, worker, &p, 0, NULL);
            if (!threads[i]) {
                fprintf(stderr,
                        "dxvk_android_probe: cannot create worker thread\n");
                exit(2);
            }
        }
        WaitForMultipleObjects((DWORD)nthreads, threads, TRUE, INFINITE);
        for (i = 0; i < (size_t)nthreads; i++) CloseHandle(threads[i]);
        free(threads);
    } else
#endif
    {
        for (i = 0; i < entries->n; i++) {
            results[i] = compile_one(&entries->v[i], clang, sysroot, shim,
                                     directx, api, ccache_wrapper, ccache,
                                     objdir);
        }
    }
    *out_results = results;
}

/* ------------------------------------------------------------------------- */
/* Self-test. */

static int self_test(void)
{
    strvec toks = {0};
    char err[128] = {0};
    char *obj;
    int ok = 1;
    if (split_shlex("clang++ -DNAME=\"a b\" -o foo.o foo.cpp", &toks,
                    err, sizeof(err)) != 0 ||
        toks.n != 5 || strcmp(toks.v[1], "-DNAME=a b") != 0 ||
        strcmp(toks.v[2], "-o") != 0 || strcmp(toks.v[3], "foo.o") != 0) {
        fprintf(stderr, "dxvk_android_probe self-test: shlex failed\n");
        ok = 0;
    }
    sv_free(&toks);
    obj = object_path("obj", "../src/a.cpp");
    if (strcmp(obj, "obj/src_a.cpp.o") != 0) {
        fprintf(stderr, "dxvk_android_probe self-test: object path failed: %s\n",
                obj);
        ok = 0;
    }
    free(obj);
    if (!ok) return 1;
    printf("dxvk_android_probe self-test: OK\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Driver. */

static void usage(void)
{
    fprintf(stderr,
            "usage: dxvk_android_probe --dxvk DIR [--ndk DIR] [--api N] "
            "[--jobs N] [--limit N]\n"
            "       [--verbose] [--exclude SUBSTR]... [--only SUBSTR]... "
            "[--codegen] [--objdir DIR]\n"
            "       [--dump SUBSTR] [--stale-only] [--ccache FILE] "
            "[--root DIR]\n"
            "       dxvk_android_probe --self-test\n");
}

int main(int argc, char **argv)
{
    const char *dxvk = NULL;
    const char *ndk = NULL;
    const char *root_arg = NULL;
    const char *objdir_arg = NULL;
    char *objdir = NULL;
    const char *dump = NULL;
    const char *ccache = NULL;
    int api = 26;
    int jobs = 8;
    int limit = 0;
    int verbose = 0;
    int codegen = 0;
    int stale_only = 0;
    int i;
    char *root;
    char *build;
    char *cc_path;
    char *clang;
    char *sysroot;
    char *shim;
    char *directx;
    char *ccache_wrapper;
    entry_list entries = {0};
    entry_list filtered = {0};
    compile_result *results = NULL;
    int rc = 1;

    setvbuf(stdout, NULL, _IONBF, 0);

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--self-test") == 0) return self_test();
        else if (strcmp(a, "--dxvk") == 0 && i + 1 < argc) dxvk = argv[++i];
        else if (strncmp(a, "--dxvk=", 7) == 0) dxvk = a + 7;
        else if (strcmp(a, "--ndk") == 0 && i + 1 < argc) ndk = argv[++i];
        else if (strncmp(a, "--ndk=", 6) == 0) ndk = a + 6;
        else if (strcmp(a, "--api") == 0 && i + 1 < argc) api = atoi(argv[++i]);
        else if (strncmp(a, "--api=", 6) == 0) api = atoi(a + 6);
        else if (strcmp(a, "--jobs") == 0 && i + 1 < argc) jobs = atoi(argv[++i]);
        else if (strncmp(a, "--jobs=", 7) == 0) jobs = atoi(a + 7);
        else if (strcmp(a, "--limit") == 0 && i + 1 < argc) limit = atoi(argv[++i]);
        else if (strncmp(a, "--limit=", 8) == 0) limit = atoi(a + 8);
        else if (strcmp(a, "--verbose") == 0) verbose = 1;
        else if (strcmp(a, "--codegen") == 0) codegen = 1;
        else if (strcmp(a, "--stale-only") == 0) stale_only = 1;
        else if (strcmp(a, "--objdir") == 0 && i + 1 < argc) objdir_arg = argv[++i];
        else if (strncmp(a, "--objdir=", 9) == 0) objdir_arg = a + 9;
        else if (strcmp(a, "--dump") == 0 && i + 1 < argc) dump = argv[++i];
        else if (strncmp(a, "--dump=", 7) == 0) dump = a + 7;
        else if (strcmp(a, "--ccache") == 0 && i + 1 < argc) ccache = argv[++i];
        else if (strncmp(a, "--ccache=", 9) == 0) ccache = a + 9;
        else if (strcmp(a, "--root") == 0 && i + 1 < argc) root_arg = argv[++i];
        else if (strncmp(a, "--root=", 7) == 0) root_arg = a + 7;
        else if (strcmp(a, "--exclude") == 0 && i + 1 < argc) {
            i++; /* collected in the second pass */
        } else if (strncmp(a, "--exclude=", 10) == 0) {
            /* collected in the second pass */
        } else if (strcmp(a, "--only") == 0 && i + 1 < argc) {
            i++; /* collected in the second pass */
        } else if (strncmp(a, "--only=", 7) == 0) {
            /* collected in the second pass */
        } else {
            usage();
            return 2;
        }
    }
    if (!dxvk) {
        usage();
        return 2;
    }
    if (!ndk) {
        ndk = getenv("WC3_NDK");
        if (!ndk || !*ndk) ndk = DEFAULT_NDK;
    }
    if (!root_arg) {
        char *exe = path_resolve(argv[0]);
        char *d1 = path_dirname(exe);
        char *d2 = path_dirname(d1);
        root = path_dirname(d2);
        free(d2);
        free(d1);
        free(exe);
    } else {
        root = path_resolve(root_arg);
    }
    shim = path_join(root, "third_party/dxvk/shim");
    directx = path_join(root,
                        "third_party/dxvk/3.1.1/include/native/directx");
    ccache_wrapper = getenv("WC3_CCACHE_WRAPPER");
    if (ccache_wrapper && *ccache_wrapper) {
        ccache_wrapper = xstrdup(ccache_wrapper);
    } else {
        ccache_wrapper = path_join(root, "build/lift-tools/ccache_wrapper.exe");
    }
    clang = path_join(ndk,
        "toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe");
    sysroot = path_join(ndk,
        "toolchains/llvm/prebuilt/windows-x86_64/sysroot");
    if (!is_file(clang)) {
        fprintf(stderr, "dxvk_android_probe: NDK clang not found: %s\n", clang);
        goto cleanup_paths;
    }
    if (ccache) {
        if (!is_file(ccache)) {
            fprintf(stderr, "dxvk_android_probe: ccache not found: %s\n",
                    ccache);
            goto cleanup_paths;
        }
        if (!is_file(ccache_wrapper)) {
            fprintf(stderr, "dxvk_android_probe: ccache wrapper not found: %s\n",
                    ccache_wrapper);
            goto cleanup_paths;
        }
    }
    build = find_build_dir(dxvk);
    if (!build) {
        fprintf(stderr, "dxvk_android_probe: no compile_commands.json under %s\n",
                dxvk);
        goto cleanup_paths;
    }
    {
        char *abs_build = path_resolve(build);
        free(build);
        build = abs_build;
    }
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    if (!SetCurrentDirectoryA(build)) {
        fprintf(stderr, "dxvk_android_probe: cannot cd to %s\n", build);
        free(build);
        goto cleanup_paths;
    }
#else
    if (chdir(build) != 0) {
        fprintf(stderr, "dxvk_android_probe: cannot cd to %s\n", build);
        free(build);
        goto cleanup_paths;
    }
#endif
    cc_path = path_join(build, "compile_commands.json");
    if (load_entries(cc_path, &entries) != 0) {
        fprintf(stderr, "dxvk_android_probe: cannot read %s\n", cc_path);
        free(cc_path);
        free(build);
        goto cleanup_paths;
    }
    free(cc_path);
    free(build);

    /* Re-scan argv for repeatable filters; keeping this separate makes the
     * first pass simple and preserves order. */
    {
        strvec only = {0};
        strvec exclude = {0};
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--only") == 0 && i + 1 < argc)
                sv_push_copy(&only, argv[++i]);
            else if (strncmp(argv[i], "--only=", 7) == 0)
                sv_push_copy(&only, argv[i] + 7);
            else if (strcmp(argv[i], "--exclude") == 0 && i + 1 < argc)
                sv_push_copy(&exclude, argv[++i]);
            else if (strncmp(argv[i], "--exclude=", 10) == 0)
                sv_push_copy(&exclude, argv[i] + 10);
        }
        for (i = 0; i < (int)entries.n; i++) {
            entry *e = &entries.v[i];
            if (exclude.n && any_contains(&exclude, e->file)) continue;
            if (only.n && !any_contains(&only, e->file)) continue;
            if (limit > 0 && filtered.n >= (size_t)limit) break;
            filtered.v = (entry *)xrealloc(
                filtered.v, (filtered.n + 1) * sizeof(*filtered.v));
            filtered.v[filtered.n++] = *e;
            e->file = NULL;
            e->command = NULL;
        }
        sv_free(&only);
        sv_free(&exclude);
    }
    entry_list_free(&entries);

    if (dump) {
        for (i = 0; i < (int)filtered.n; i++) {
            if (contains_slashed(filtered.v[i].file, dump)) {
                strvec cmd = {0};
                char err[256] = {0};
                char *output = NULL;
                int code = 0;
                build_command_argv(clang, sysroot, shim, directx,
                                   &filtered.v[i], api, ccache_wrapper,
                                   ccache, NULL, &cmd, err, sizeof(err));
                if (err[0]) {
                    fprintf(stderr, "dxvk_android_probe: %s\n", err);
                    sv_free(&cmd);
                    goto cleanup;
                }
                if (run_capture(&cmd, &output, &code) != 0) {
                    fprintf(stderr, "dxvk_android_probe: failed to start compiler\n");
                    sv_free(&cmd);
                    goto cleanup;
                }
                fputs(output ? output : "", stdout);
                free(output);
                sv_free(&cmd);
                rc = 0;
                goto cleanup;
            }
        }
        fprintf(stderr, "dxvk_android_probe: no TU matching %s\n", dump);
        goto cleanup;
    }

    if (codegen) {
        if (objdir_arg) {
            objdir = path_resolve(objdir_arg);
        } else {
            char *tag = path_basename(dxvk);
            char *objroot = path_join(root, "build/android/dxvk_obj");
            objdir = path_join(objroot, tag);
            free(objroot);
            free(tag);
        }
        make_dirs(objdir);
    } else {
        objdir = NULL;
    }

    if (objdir && stale_only) {
        entry_list fresh = {0};
        for (i = 0; i < (int)filtered.n; i++) {
            entry *e = &filtered.v[i];
            char *src = path_join(".", e->file);
            char *obj = object_path(objdir, e->file);
            uint64_t ot = file_mtime(obj);
            uint64_t st = file_mtime(src);
            int keep = !(ot && st && ot >= st);
            free(src);
            free(obj);
            if (keep) {
                fresh.v = (entry *)xrealloc(
                    fresh.v, (fresh.n + 1) * sizeof(*fresh.v));
                fresh.v[fresh.n++] = *e;
                e->file = NULL;
                e->command = NULL;
            }
        }
        printf("=== stale: %zu of %zu ===\n", fresh.n, filtered.n);
        entry_list_free(&filtered);
        filtered = fresh;
    }

    run_jobs(&filtered, jobs, clang, sysroot, shim, directx, api,
             ccache_wrapper, ccache, objdir, &results);

    {
        int ok = 0;
        int bad = 0;
        for (i = 0; i < (int)filtered.n; i++) {
            if (results[i].ok) ok++;
            else bad++;
        }
        printf("=== %s: ok=%d failed=%d of %zu ===\n", dxvk, ok, bad,
               filtered.n);
        if (bad) {
            typedef struct { char *msg; int count; size_t seq; } msg_count;
            msg_count *counts = NULL;
            size_t ncounts = 0;
            size_t cap = 0;
            size_t k;
            for (i = 0; i < (int)filtered.n; i++) {
                const char *msg;
                size_t j;
                int found = 0;
                if (!results[i].error) continue;
                msg = results[i].error;
                for (j = 0; j < ncounts; j++) {
                    if (strcmp(counts[j].msg, msg) == 0) {
                        counts[j].count++;
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    if (ncounts == cap) {
                        cap = cap ? cap * 2 : 16;
                        counts = (msg_count *)xrealloc(
                            counts, cap * sizeof(*counts));
                    }
                    counts[ncounts].msg = xstrdup(msg);
                    counts[ncounts].count = 1;
                    counts[ncounts].seq = ncounts;
                    ncounts++;
                }
            }
            for (k = 0; k + 1 < ncounts; k++) {
                size_t j;
                for (j = 0; j + 1 < ncounts - k; j++) {
                    if (counts[j + 1].count > counts[j].count) {
                        msg_count tmp = counts[j];
                        counts[j] = counts[j + 1];
                        counts[j + 1] = tmp;
                    }
                }
            }
            for (k = 0; k < ncounts && k < 20; k++) {
                char tmp[101];
                size_t n = strlen(counts[k].msg);
                if (n > 100) n = 100;
                memcpy(tmp, counts[k].msg, n);
                tmp[n] = 0;
                printf("  %3dx %s\n", counts[k].count, tmp);
            }
            for (k = 0; k < ncounts; k++) free(counts[k].msg);
            free(counts);
        }
        if (verbose) {
            printf("\n--- per file ---\n");
            for (i = 0; i < (int)filtered.n; i++) {
                if (results[i].error) {
                    char tmp[121];
                    size_t n = strlen(results[i].error);
                    if (n > 120) n = 120;
                    memcpy(tmp, results[i].error, n);
                    tmp[n] = 0;
                    printf("  %s\n      %s\n", results[i].file, tmp);
                }
            }
        }
        rc = bad ? 1 : 0;
    }

cleanup:
    if (results) {
        for (i = 0; i < (int)filtered.n; i++) free(results[i].error);
        free(results);
    }
    entry_list_free(&filtered);
    free(objdir);
    free(clang);
    free(sysroot);
    free(shim);
    free(directx);
    free(ccache_wrapper);
    free(root);
    return rc;

cleanup_paths:
    free(clang);
    free(sysroot);
    free(shim);
    free(directx);
    free(ccache_wrapper);
    free(root);
    return rc;
}
