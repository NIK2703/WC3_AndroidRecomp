/* dxvk_isolate_archive_elf.c - version-prefix DXVK ELF archives.
 *
 * This is the C implementation of DXVK ELF archive isolation.  It
 * extracts the members of one or more GNU ar archives, renames every global
 * DXVK symbol through llvm-objcopy, and writes one regular archive that can
 * be linked with --whole-archive for both DXVK majors.
 *
 * The platform shim object (dxvk_win32.o / dxvk_win32.obj) is deliberately
 * left unprefixed: both DXVK versions call the same Win32 shim and only one
 * copy may exist in the final image.
 *
 * Usage:
 *   AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy \
 *     dxvk_isolate_archive_elf <prefix> <output.a> <archive>...
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
#  define WC3_RMDIR(p) _rmdir(p)
#  define WC3_POPEN _popen
#  define WC3_PCLOSE _pclose
#else
#  include <dirent.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  define WC3_MKDIR(p) mkdir((p), 0777)
#  define WC3_RMDIR(p) rmdir(p)
#  define WC3_POPEN popen
#  define WC3_PCLOSE pclose
#endif

#define WC3_PATH_MAX 4096

typedef struct {
    char **v;
    size_t n;
    size_t cap;
} strvec;

typedef struct {
    unsigned char *data;
    size_t size;
} buffer;

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "dxvk_isolate_archive_elf: out of memory\n");
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

static void sv_push(strvec *sv, char *s)
{
    if (sv->n == sv->cap) {
        size_t cap = sv->cap ? sv->cap * 2 : 32;
        char **v = (char **)realloc(sv->v, cap * sizeof(*v));
        if (!v) {
            fprintf(stderr, "dxvk_isolate_archive_elf: out of memory\n");
            exit(2);
        }
        sv->v = v;
        sv->cap = cap;
    }
    sv->v[sv->n++] = s;
}

static void sv_free_items(strvec *sv)
{
    size_t i;
    for (i = 0; i < sv->n; i++) free(sv->v[i]);
    free(sv->v);
    sv->v = NULL;
    sv->n = sv->cap = 0;
}

static void sv_free_refs(strvec *sv)
{
    free(sv->v);
    sv->v = NULL;
    sv->n = sv->cap = 0;
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
    {
        char *out = (char *)xmalloc(i + 1);
        memcpy(out, path, i);
        out[i] = 0;
        return out;
    }
}

static char *path_basename(const char *path)
{
    size_t end = strlen(path);
    size_t start;
    while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\')) end--;
    start = end;
    while (start > 0 && path[start - 1] != '/' && path[start - 1] != '\\') start--;
    {
        char *out = (char *)xmalloc(end - start + 1);
        memcpy(out, path + start, end - start);
        out[end - start] = 0;
        return out;
    }
}

static char *slashes_forward(const char *s)
{
    char *out = xstrdup(s);
    char *p;
    for (p = out; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    return out;
}

static int read_file(const char *path, buffer *buf)
{
    FILE *f = fopen(path, "rb");
    long size;
    if (!f) {
        fprintf(stderr, "dxvk_isolate_archive_elf: cannot open %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    buf->data = (unsigned char *)xmalloc((size_t)size + 1);
    buf->size = (size_t)size;
    if (size > 0 && fread(buf->data, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "dxvk_isolate_archive_elf: short read %s\n", path);
        fclose(f);
        free(buf->data);
        buf->data = NULL;
        return -1;
    }
    fclose(f);
    return 0;
}

static int write_file(const char *path, const void *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "dxvk_isolate_archive_elf: cannot write %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    if (size && fwrite(data, 1, size, f) != size) {
        fprintf(stderr, "dxvk_isolate_archive_elf: short write %s\n", path);
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) return -1;
    return 0;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    char tmp[65536];
    size_t n;
    if (!in) return 0;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    while ((n = fread(tmp, 1, sizeof(tmp), in)) > 0) {
        if (fwrite(tmp, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return 0;
        }
    }
    if (ferror(in)) {
        fclose(in);
        fclose(out);
        return 0;
    }
    fclose(in);
    if (fclose(out) != 0) return 0;
    return 1;
}

static int run_argv(char *const argv[])
{
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    int rc = (int)_spawnv(_P_WAIT, argv[0],
                          (const char *const *)argv);
    if (rc == -1) {
        fprintf(stderr, "dxvk_isolate_archive_elf: cannot run %s: %s\n",
                argv[0], strerror(errno));
        return 127;
    }
    return rc;
#else
    pid_t pid = fork();
    int status;
    if (pid < 0) {
        fprintf(stderr, "dxvk_isolate_archive_elf: fork failed: %s\n",
                strerror(errno));
        return 127;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "dxvk_isolate_archive_elf: cannot run %s: %s\n",
                argv[0], strerror(errno));
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) return 127;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 127;
#endif
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
            cmd = (char *)realloc(cmd, cap);
            if (!cmd) {
                fprintf(stderr, "dxvk_isolate_archive_elf: out of memory\n");
                exit(2);
            }
        }
        if (len) cmd[len++] = ' ';
        memcpy(cmd + len, q, n + 1);
        len += n;
        free(q);
    }
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    /* _popen runs "cmd /c <command>".  cmd strips the first and last quote
     * from a command line, which would split the quoted executable path from
     * its quoted arguments.  Wrap the whole line once more so cmd removes
     * only the outer pair. */
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
        fprintf(stderr, "dxvk_isolate_archive_elf: popen failed: %s\n", cmd);
        free(cmd);
        return NULL;
    }
    free(cmd);
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (n + got + 1 > cap) {
            size_t next = cap ? cap * 2 : 8192;
            while (next < n + got + 1) next *= 2;
            out = (char *)realloc(out, next);
            if (!out) {
                fprintf(stderr, "dxvk_isolate_archive_elf: out of memory\n");
                exit(2);
            }
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

static int is_shim_object(const char *path)
{
    char *base = path_basename(path);
    char *p;
    int result;
    for (p = base; *p; p++) *p = (char)tolower((unsigned char)*p);
    result = (strstr(base, "dxvk_win32.o") != NULL &&
              strlen(base) >= strlen("dxvk_win32.o") &&
              strcmp(base + strlen(base) - strlen("dxvk_win32.o"),
                     "dxvk_win32.o") == 0) ||
             (strstr(base, "dxvk_win32.obj") != NULL &&
              strlen(base) >= strlen("dxvk_win32.obj") &&
              strcmp(base + strlen(base) - strlen("dxvk_win32.obj"),
                     "dxvk_win32.obj") == 0);
    free(base);
    return result;
}

static const char *const exact_skip[] = {
    "__CTOR_LIST__", "__CTOR_END__", "__DTOR_LIST__", "__DTOR_END__",
    "__RUNTIME_PSEUDO_RELOC_LIST__", "__RUNTIME_PSEUDO_RELOC_LIST_END__",
    "__bss_start__", "__bss_end__", "__data_start__", "__data_end__",
    "__preinit_array_start", "__preinit_array_end",
    "__init_array_start", "__init_array_end",
    "__fini_array_start", "__fini_array_end",
    "_tls_index", "_tls_start", "_tls_end", "_tls_used",
    "__ImageBase", "__dso_handle", "_end", "_etext", "_edata",
    "__head_libd3d9_a", "__tail_libd3d9_a",
    NULL
};

static const char *const section_prefixes[] = {
    ".text", ".data", ".bss", ".rdata", ".pdata", ".xdata", ".debug",
    ".ctors", ".dtors", ".tls", ".CRT", ".reloc", ".rsrc", ".edata",
    ".idata", ".gfids", ".giats", ".00cfg", ".buildid", ".comment",
    NULL
};

static const char *const runtime_prefixes[] = {
    "_ZSt", "_ZNSt", "_ZNKSt", "_ZTISt", "_ZTSSt", "_ZTVSt", "_ZTTSt",
    "_ZTINSt", "_ZTSNSt", "_ZTVNSt", "_ZTTNSt",
    "_ZTIN10__cxxabiv1", "_ZTSN10__cxxabiv1", "_ZTVN10__cxxabiv1",
    "__cxa_", "_Unwind_", "__gxx_personality", "__gcc_", "__mingw_",
    "__emutls_get_address", "__emutls_register_common",
    NULL
};

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int starts_with_any(const char *s, const char *const *prefixes)
{
    size_t i;
    for (i = 0; prefixes[i]; i++) {
        if (starts_with(s, prefixes[i])) return 1;
    }
    return 0;
}

static int is_dxvk_runtime(const char *name)
{
    if (!starts_with_any(name, runtime_prefixes)) return 0;
    return strstr(name, "dxvk") != NULL || strstr(name, "Dxvk") != NULL;
}

static int wanted(const char *name)
{
    size_t i;
    if (!name || !*name) return 0;
    for (i = 0; exact_skip[i]; i++) {
        if (strcmp(name, exact_skip[i]) == 0) return 0;
    }
    if (starts_with(name, ".text$") || starts_with(name, ".rdata$") ||
        starts_with(name, ".data$") || starts_with(name, ".bss$") ||
        starts_with(name, ".pdata$") || starts_with(name, ".xdata$")) {
        const char *embedded = strchr(name, '$');
        if (embedded && starts_with_any(embedded + 1, runtime_prefixes)) {
            return 0;
        }
    }
    if (starts_with_any(name, section_prefixes)) return 0;
    if (starts_with_any(name, runtime_prefixes)) return 0;
    return 1;
}

static int is_defined_type(char type)
{
    return strchr("TDBRWVCS", type) != NULL;
}

static void collect_symbols(const char *obj, strvec *names)
{
    char *nm = getenv("NM");
    char *argv[5];
    char *out;
    char *line;
    if (!nm || !*nm) nm = "nm";
    argv[0] = nm;
    argv[1] = "-g";
    argv[2] = "--defined-only";
    argv[3] = (char *)obj;
    argv[4] = NULL;
    out = capture_argv(argv);
    if (!out) {
        fprintf(stderr, "dxvk_isolate_archive_elf: nm failed for %s\n", obj);
        exit(1);
    }
    line = out;
    while (*line) {
        char *end = strchr(line, '\n');
        char *p = line;
        char type;
        char *name;
        size_t len;
        if (end) *end = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (!isxdigit((unsigned char)*p)) goto next;
        while (isxdigit((unsigned char)*p)) p++;
        if (*p != ' ' && *p != '\t') goto next;
        while (*p == ' ' || *p == '\t') p++;
        if (!isalpha((unsigned char)*p)) goto next;
        type = *p++;
        if (*p != ' ' && *p != '\t') goto next;
        while (*p == ' ' || *p == '\t') p++;
        name = p;
        len = strlen(name);
        while (len > 0 && isspace((unsigned char)name[len - 1])) len--;
        if (len == 0 || !is_defined_type(type)) goto next;
        {
            char *copy = (char *)xmalloc(len + 1);
            memcpy(copy, name, len);
            copy[len] = 0;
            if (wanted(copy) || is_dxvk_runtime(copy)) {
                sv_push(names, copy);
            } else {
                free(copy);
            }
        }
next:
        if (!end) break;
        line = end + 1;
    }
    free(out);
}

static int cmp_str(const void *a, const void *b)
{
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

static void sort_unique(strvec *names)
{
    size_t i;
    size_t w = 0;
    if (names->n == 0) return;
    qsort(names->v, names->n, sizeof(*names->v), cmp_str);
    for (i = 0; i < names->n; i++) {
        if (w == 0 || strcmp(names->v[i], names->v[w - 1]) != 0) {
            names->v[w++] = names->v[i];
        } else {
            free(names->v[i]);
        }
    }
    names->n = w;
}

static char *member_basename(const char *member)
{
    char *base = path_basename(member);
    if (!base[0]) {
        free(base);
        return xstrdup("member");
    }
    return base;
}

static char *object_path(const char *tmpdir, const char *tag, size_t index,
                         const char *member)
{
    char *base = member_basename(member);
    size_t n = strlen(tmpdir) + strlen(tag) + strlen(base) + 64;
    char *out = (char *)xmalloc(n);
    snprintf(out, n, "%s/%s_%04zu_%s", tmpdir, tag, index, base);
    free(base);
    return out;
}

static int extract_regular(const char *archive, const buffer *raw,
                           const char *tmpdir, const char *tag,
                           strvec *objects)
{
    size_t pos = 8;
    const unsigned char *longnames = NULL;
    size_t longnames_len = 0;
    size_t index = 0;

    while (pos + 60 <= raw->size) {
        const unsigned char *hdr = raw->data + pos;
        char name[17];
        size_t name_len = 0;
        size_t member_size;
        size_t body;
        int special;
        char *stored = NULL;
        char *out;
        if (hdr[58] != '`' || hdr[59] != '\n') {
            fprintf(stderr, "dxvk_isolate_archive_elf: %s: bad header at %zu\n",
                    archive, pos);
            return -1;
        }
        while (name_len < 16 && hdr[name_len] != ' ') {
            name[name_len] = (char)hdr[name_len];
            name_len++;
        }
        name[name_len] = 0;
        member_size = (size_t)strtoul(
            (const char *)hdr + 48, NULL, 10);
        body = pos + 60;
        if (body + member_size > raw->size) {
            fprintf(stderr, "dxvk_isolate_archive_elf: %s: truncated member\n",
                    archive);
            return -1;
        }
        if ((strcmp(name, "/") != 0 && strcmp(name, "//") != 0) &&
            name_len > 1 && name[0] == '/' && name[name_len - 1] == '/') {
            name[name_len - 1] = 0;
        }
        special = strcmp(name, "/") == 0 || strcmp(name, "//") == 0 ||
                  strncmp(name, "/SYM64/", 7) == 0;
        if (strcmp(name, "//") == 0) {
            longnames = raw->data + body;
            longnames_len = member_size;
        } else if (!special) {
            if (name[0] == '/' && isdigit((unsigned char)name[1])) {
                size_t off = (size_t)strtoul(name + 1, NULL, 10);
                size_t end;
                if (off > longnames_len) {
                    fprintf(stderr,
                            "dxvk_isolate_archive_elf: %s: bad long name\n",
                            archive);
                    return -1;
                }
                end = off;
                while (end + 1 < longnames_len &&
                       !(longnames[end] == '/' && longnames[end + 1] == '\n')) {
                    end++;
                }
                stored = (char *)xmalloc(end - off + 1);
                memcpy(stored, longnames + off, end - off);
                stored[end - off] = 0;
            } else if (strncmp(name, "#1/", 3) == 0) {
                size_t n = (size_t)strtoul(name + 3, NULL, 10);
                if (n > member_size) {
                    fprintf(stderr,
                            "dxvk_isolate_archive_elf: %s: bad BSD name\n",
                            archive);
                    return -1;
                }
                stored = (char *)xmalloc(n + 1);
                memcpy(stored, raw->data + body, n);
                stored[n] = 0;
                body += n;
                member_size -= n;
            } else {
                stored = xstrdup(name);
            }
            out = object_path(tmpdir, tag, index, stored);
            if (write_file(out, raw->data + body, member_size) != 0) {
                free(stored);
                free(out);
                return -1;
            }
            sv_push(objects, out);
            free(stored);
            index++;
        }
        pos = body + member_size + (member_size & 1);
    }
    return 0;
}

static char *current_dir(void)
{
    char buf[WC3_PATH_MAX];
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    if (_getcwd(buf, (int)sizeof(buf)) == NULL) return xstrdup(".");
#else
    if (getcwd(buf, sizeof(buf)) == NULL) return xstrdup(".");
#endif
    return xstrdup(buf);
}

static int extract_thin(const char *archive, const char *tmpdir,
                        const char *tag, strvec *objects)
{
    char *ar = getenv("AR");
    char *argv[4];
    char *out;
    char *line;
    char *archive_dir;
    size_t index = 0;
    if (!ar || !*ar) ar = "ar";
    argv[0] = ar;
    argv[1] = "t";
    argv[2] = (char *)archive;
    argv[3] = NULL;
    out = capture_argv(argv);
    if (!out) {
        fprintf(stderr, "dxvk_isolate_archive_elf: ar t failed for %s\n",
                archive);
        return -1;
    }
    archive_dir = path_dirname(archive);
    line = out;
    while (*line) {
        char *end = strchr(line, '\n');
        char *member;
        char *norm;
        char *dest;
        int copied = 0;
        char *cwd;
        if (end) *end = 0;
        while (*line && isspace((unsigned char)*line)) line++;
        if (!*line) {
            if (!end) break;
            line = end + 1;
            continue;
        }
        member = xstrdup(line);
        norm = slashes_forward(member);
        dest = object_path(tmpdir, tag, index, member);
        cwd = current_dir();
        {
            char *candidates[6];
            size_t i;
            candidates[0] = xstrdup(member);
            candidates[1] = xstrdup(norm);
            candidates[2] = path_join(archive_dir, member);
            candidates[3] = path_join(archive_dir, norm);
            candidates[4] = path_join(cwd, member);
            candidates[5] = path_join(cwd, norm);
            for (i = 0; i < 6; i++) {
                if (!copied && copy_file(candidates[i], dest)) copied = 1;
                free(candidates[i]);
            }
        }
        free(cwd);
        if (!copied) {
            fprintf(stderr,
                    "dxvk_isolate_archive_elf: %s: thin member %s not found\n",
                    archive, member);
            free(dest);
            free(norm);
            free(member);
            free(archive_dir);
            free(out);
            return -1;
        }
        sv_push(objects, dest);
        free(norm);
        free(member);
        index++;
        if (!end) break;
        line = end + 1;
    }
    free(archive_dir);
    free(out);
    return 0;
}

static int extract_archive(const char *archive, const char *tmpdir,
                           const char *tag, strvec *objects)
{
    buffer raw;
    int rc;
    if (read_file(archive, &raw) != 0) return -1;
    if (raw.size >= 8 && memcmp(raw.data, "!<thin>\n", 8) == 0) {
        free(raw.data);
        return extract_thin(archive, tmpdir, tag, objects);
    }
    if (raw.size < 8 || memcmp(raw.data, "!<arch>\n", 8) != 0) {
        fprintf(stderr, "dxvk_isolate_archive_elf: %s: not a GNU ar archive\n",
                archive);
        free(raw.data);
        return -1;
    }
    rc = extract_regular(archive, &raw, tmpdir, tag, objects);
    free(raw.data);
    return rc;
}

static int make_temp_dir(char *out, size_t cap)
{
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    char base[WC3_PATH_MAX];
    DWORD len = GetTempPathA((DWORD)sizeof(base), base);
    unsigned i;
    if (len == 0 || len >= (DWORD)sizeof(base)) return -1;
    for (i = 0; i < 10000; i++) {
        int n = snprintf(out, cap, "%swc3_dxvk_elf_%lu_%u", base,
                         (unsigned long)GetCurrentProcessId(), i);
        if (n < 0 || (size_t)n >= cap) return -1;
        if (WC3_MKDIR(out) == 0) return 0;
        if (errno != EEXIST) return -1;
    }
    return -1;
#else
    const char *base = getenv("TMPDIR");
    int n;
    if (!base || !*base) base = "/tmp";
    n = snprintf(out, cap, "%s/wc3_dxvk_elf_XXXXXX", base);
    if (n < 0 || (size_t)n >= cap) return -1;
    return mkdtemp(out) ? 0 : -1;
#endif
}

static void remove_tree(const char *path)
{
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    char pattern[WC3_PATH_MAX];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char *child;
            if (strcmp(fd.cFileName, ".") == 0 ||
                strcmp(fd.cFileName, "..") == 0) continue;
            child = path_join(path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_tree(child);
            } else {
                remove(child);
            }
            free(child);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    WC3_RMDIR(path);
#else
    DIR *d = opendir(path);
    struct dirent *ent;
    if (d) {
        while ((ent = readdir(d)) != NULL) {
            char *child;
            struct stat st;
            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0) continue;
            child = path_join(path, ent->d_name);
            if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
                remove_tree(child);
            } else {
                unlink(child);
            }
            free(child);
        }
        closedir(d);
    }
    WC3_RMDIR(path);
#endif
}

static int write_map(const char *mapfile, const strvec *names,
                     const char *prefix)
{
    FILE *f = fopen(mapfile, "wb");
    size_t i;
    if (!f) {
        fprintf(stderr, "dxvk_isolate_archive_elf: cannot write %s: %s\n",
                mapfile, strerror(errno));
        return -1;
    }
    for (i = 0; i < names->n; i++) {
        if (fprintf(f, "%s %s%s\n", names->v[i], prefix, names->v[i]) < 0) {
            fclose(f);
            return -1;
        }
    }
    if (fclose(f) != 0) return -1;
    return 0;
}

static int isolate_objects(const strvec *objects, const char *mapfile,
                           const char *tmpdir, const char *output,
                           strvec *isolated)
{
    char *objcopy = getenv("OBJCOPY");
    char *ar = getenv("AR");
    size_t i;
    if (!objcopy || !*objcopy) objcopy = "objcopy";
    if (!ar || !*ar) ar = "ar";

    for (i = 0; i < objects->n; i++) {
        if (is_shim_object(objects->v[i])) {
            sv_push(isolated, xstrdup(objects->v[i]));
            continue;
        }
        {
            char out[WC3_PATH_MAX];
            int n = snprintf(out, sizeof(out), "%s/i%04zu.o", tmpdir, i);
            char redefine[WC3_PATH_MAX];
            char *argv[5];
            if (n < 0 || (size_t)n >= sizeof(out)) return -1;
            n = snprintf(redefine, sizeof(redefine), "--redefine-syms=%s",
                         mapfile);
            if (n < 0 || (size_t)n >= sizeof(redefine)) return -1;
            argv[0] = objcopy;
            argv[1] = redefine;
            argv[2] = objects->v[i];
            argv[3] = out;
            argv[4] = NULL;
            if (run_argv(argv) != 0) {
                fprintf(stderr,
                        "dxvk_isolate_archive_elf: objcopy failed for %s\n",
                        objects->v[i]);
                return -1;
            }
            sv_push(isolated, xstrdup(out));
        }
    }

    if (output[0] && remove(output) != 0 && errno != ENOENT) {
        fprintf(stderr, "dxvk_isolate_archive_elf: cannot remove %s\n",
                output);
        return -1;
    }
    {
        char **argv = (char **)xmalloc((isolated->n + 4) * sizeof(*argv));
        argv[0] = ar;
        argv[1] = "rcs";
        argv[2] = (char *)output;
        for (i = 0; i < isolated->n; i++) argv[3 + i] = isolated->v[i];
        argv[3 + isolated->n] = NULL;
        if (run_argv(argv) != 0) {
            fprintf(stderr, "dxvk_isolate_archive_elf: ar failed for %s\n",
                    output);
            free(argv);
            return -1;
        }
        free(argv);
    }
    return 0;
}

static int self_test(void)
{
    int failures = 0;
    struct {
        const char *name;
        int expected;
    } cases[] = {
        {"Direct3DCreate9", 1},
        {"_ZN4dxvk10DxvkDeviceC1Ev", 1},
        {"_ZSt4cout", 0},
        {"__cxa_atexit", 0},
        {"__emutls_get_address", 0},
        {".text", 0},
        {".text$_ZN4dxvk", 0},
        {"_tls_index", 0},
        {"__head_libd3d9_a", 0},
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (wanted(cases[i].name) != cases[i].expected) {
            fprintf(stderr, "self-test: wanted(%s) != %d\n",
                    cases[i].name, cases[i].expected);
            failures++;
        }
    }
    if (!is_dxvk_runtime("_ZSt6vectorIN4dxvk4utilEE")) {
        fprintf(stderr, "self-test: dxvk std:: runtime not detected\n");
        failures++;
    }
    if (is_dxvk_runtime("_ZSt4cout")) {
        fprintf(stderr, "self-test: pure std:: runtime renamed\n");
        failures++;
    }
    if (!is_shim_object("a/dxvk_win32.o") ||
        !is_shim_object("a/DXVK_WIN32.OBJ") ||
        is_shim_object("a/main.o")) {
        fprintf(stderr, "self-test: shim object detection failed\n");
        failures++;
    }
    if (failures == 0) printf("self-test: OK\n");
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *prefix;
    const char *output;
    char tmpdir[WC3_PATH_MAX];
    strvec objects = {0};
    strvec names = {0};
    strvec isolated = {0};
    char *mapfile = NULL;
    int i;
    int rc = 1;

    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) {
        return self_test();
    }
    if (argc < 4) {
        fprintf(stderr,
                "usage: dxvk_isolate_archive_elf <prefix> <output.a> "
                "<archive>...\n");
        return 2;
    }
    prefix = argv[1];
    output = argv[2];
    if (make_temp_dir(tmpdir, sizeof(tmpdir)) != 0) {
        fprintf(stderr, "dxvk_isolate_archive_elf: cannot create temp dir\n");
        return 2;
    }

    for (i = 3; i < argc; i++) {
        char tag[32];
        snprintf(tag, sizeof(tag), "a%d", i - 3);
        if (extract_archive(argv[i], tmpdir, tag, &objects) != 0) goto done;
    }
    fprintf(stderr, "extracted %zu objects\n", objects.n);

    for (i = 0; i < (int)objects.n; i++) {
        if (!is_shim_object(objects.v[i])) collect_symbols(objects.v[i], &names);
    }
    sort_unique(&names);
    fprintf(stderr, "renaming %zu global symbols\n", names.n);

    {
        size_t n = strlen(tmpdir) + 32;
        mapfile = (char *)xmalloc(n);
        snprintf(mapfile, n, "%s/rename.map", tmpdir);
    }
    if (write_map(mapfile, &names, prefix) != 0) goto done;
    if (isolate_objects(&objects, mapfile, tmpdir, output, &isolated) != 0) {
        goto done;
    }
    fprintf(stderr, "wrote %s\n", output);
    rc = 0;

done:
    if (mapfile) free(mapfile);
    sv_free_items(&isolated);
    sv_free_items(&names);
    sv_free_items(&objects);
    remove_tree(tmpdir);
    return rc;
}
