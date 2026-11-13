/* w3l_compat.c - apply the W3L / PvPGN / rubattle compatibility profile.
 *
 * Apply the optional W3L / PvPGN / rubattle compatibility patch to lifted C.
 * The generated corpus is changed by address comments, not line numbers; the
 * pass is idempotent and publishes a function-level change set.  The C pass
 * reproduces the retired Python reference on the full corpus.
 *
 *   w3l_compat --profile rubattle --latency 100 --lift IN --out OUT
 *              [--json REPORT] [--require-all] [--check] [--quiet]
 *   w3l_compat --profile rubattle --verify-coverage JSON...
 *   w3l_compat --self-test
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sha256.h"

#if defined(_WIN32)
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <unistd.h>
#define make_dir(p) mkdir((p), 0777)
#endif

#define PASS "w3l_compat"
#define PASS_REV 4

#define PROFILE_PVP 1
#define PROFILE_RUB 2
#define PROFILE_BOTH (PROFILE_PVP | PROFILE_RUB)

typedef struct {
    char *p;
    size_t n;
    size_t cap;
} Buf;

typedef struct {
    char **v;
    size_t n;
    size_t cap;
} StrVec;

typedef struct {
    const char *key;
    const char *function;
    const char *va;
    const char *anchor;
    int profiles;
    const char *marker;
    const char *reason;
} Site;

typedef enum {
    ST_ABSENT = 0,
    ST_APPLIED,
    ST_ALREADY
} Status;

typedef struct {
    const Site *site;
    Status status;
} Result;

typedef struct {
    size_t start;
    size_t end;
    char *replacement;
} Edit;

typedef struct {
    const char *function;
    size_t *idx;
    size_t n;
    size_t cap;
    char *before_sha;
    char *after_sha;
    char *reason;
} FuncChange;

typedef struct {
    char **keys;
    char **statuses;
    size_t n;
    size_t cap;
} SiteRecVec;

typedef struct {
    const char *s;
    size_t n;
    size_t p;
    char err[256];
} J;

static const Site SITES[] = {
    {
        "auth_req",
        "L_6F8DDC80",
        "0x6F8DDD94",
        "/* 6F8DDD94:",
        PROFILE_BOTH,
        "WC3_W3L_COMPAT auth_req",
        "wl27 auth_req: NOP the jne to the auth failure path",
    },
    {
        "logon_proof_hash_verify",
        "L_6F8DD71D",
        "0x6F8DD732",
        "/* 6F8DD732:",
        PROFILE_BOTH,
        "WC3_W3L_COMPAT logon_proof",
        "wl27 logon_proof_hash_verify: invert je to jne",
    },
    {
        "create_account",
        "L_6F9A7DB0",
        "0x6F9A7DFB",
        "/* 6F9A7DFB:",
        PROFILE_BOTH,
        "WC3_W3L_COMPAT create_account",
        "wl27 create_account: copy 32 plaintext password bytes",
    },
    {
        "hash_56",
        "L_6F8DD290",
        "0x6F8DD2A4",
        "/* 6F8DD2A4:",
        PROFILE_BOTH,
        "WC3_W3L_COMPAT hash_56",
        "wl27 PvPGN logon-proof hash replacement, packet 0x56",
    },
    {
        "hash_54",
        "L_6F8DD62D",
        "0x6F8DD63E",
        "/* 6F8DD63E:",
        PROFILE_BOTH,
        "WC3_W3L_COMPAT hash_54",
        "wl27 PvPGN logon-proof hash replacement, packet 0x54",
    },
    {
        "delay_reducer",
        "L_6F970FA0",
        "0x6F970FA0",
        "/* 6F970FA0:",
        PROFILE_BOTH,
        "WC3_W3L_COMPAT delay_reducer",
        "wl27 delay_reducer: replace 0xFA with WC3_W3L_LATENCY",
    },
    {
        "local_auth_308",
        "L_6F094308",
        "0x6F09439C",
        "/* 6F09439C:",
        PROFILE_BOTH,
        "WC3_W3L_COMPAT local_auth",
        "Android HLE workaround: accept SFileAuthenticateArchive status 1..4 "
        "so an unsigned version archive does not stop the host before "
        "SID_AUTH_CHECK (not a wl27 effect)",
    },
    {
        "local_auth_364",
        "L_6F094364",
        "0x6F09439C",
        "/* 6F09439C:",
        PROFILE_BOTH,
        "WC3_W3L_COMPAT local_auth",
        "Android HLE workaround: same local-auth branch in the duplicate "
        "block reachable from four callers (not a wl27 effect)",
    },
    {
        "local_auth_checkrev",
        "L_6F094807",
        "0x6F09482F",
        "/* 6F09482F:",
        PROFILE_BOTH,
        "WC3_W3L_COMPAT local_auth_checkrev",
        "Android HLE workaround: accept SFileAuthenticateArchive status 1..4 "
        "in the CheckRevision path before ver-IX86-*.mpq extraction "
        "(not a wl27 effect)",
    },
    {
        "local_auth_extrawork",
        "L_6F09497F",
        "0x6F0949A7",
        "/* 6F0949A7:",
        PROFILE_BOTH,
        "WC3_W3L_COMPAT local_auth_extrawork",
        "Android HLE workaround: accept SFileAuthenticateArchive status 1..4 "
        "in the ExtraWork path before ver-IX86-*.mpq extraction "
        "(not a wl27 effect)",
    },
    {
        "rubattle_0x50",
        "L_6F8D3EB0",
        "0x6F8D3F22",
        "/* 6F8D3F22:",
        PROFILE_RUB,
        "WC3_W3L_COMPAT rubattle_0x50",
        "rubattle SID_AUTH_INFO: append dword 3",
    },
    {
        "rubattle_0x83",
        "L_6F8DC630",
        "0x6F8DC63C",
        "/* 6F8DC63C:",
        PROFILE_RUB,
        "WC3_W3L_COMPAT rubattle_0x83",
        "rubattle packet 0x83 handler",
    },
};

#define NSITES (sizeof(SITES) / sizeof(SITES[0]))
#define NPOS ((size_t)-1)

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "w3l_compat: out of memory\n");
        exit(2);
    }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "w3l_compat: out of memory\n");
        exit(2);
    }
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *p = (char *)xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

static char *xstrndup(const char *s, size_t n)
{
    char *p = (char *)xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static void buf_reserve(Buf *b, size_t extra)
{
    if (b->n + extra + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + extra + 1) cap *= 2;
        b->p = (char *)xrealloc(b->p, cap);
        b->cap = cap;
    }
}

static void buf_ch(Buf *b, char c)
{
    buf_reserve(b, 1);
    b->p[b->n++] = c;
    b->p[b->n] = '\0';
}

static void buf_mem(Buf *b, const char *s, size_t n)
{
    buf_reserve(b, n);
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

static void buf_str(Buf *b, const char *s)
{
    buf_mem(b, s, strlen(s));
}

static char *buf_take(Buf *b)
{
    if (!b->p) return xstrdup("");
    return b->p;
}

static void vec_push(StrVec *v, char *s)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->v = (char **)xrealloc(v->v, v->cap * sizeof(*v->v));
    }
    v->v[v->n++] = s;
}

static void vec_free(StrVec *v)
{
    size_t i;
    for (i = 0; i < v->n; i++) free(v->v[i]);
    free(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

static char *vfmt(const char *fmt, va_list ap)
{
    va_list ap2;
    int n;
    char *p;

    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) n = 0;
    p = (char *)xmalloc((size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    return p;
}

static char *fmt(const char *f, ...)
{
    va_list ap;
    char *p;
    va_start(ap, f);
    p = vfmt(f, ap);
    va_end(ap);
    return p;
}

static void set_err(char **err, const char *f, ...)
{
    va_list ap;
    if (*err) free(*err);
    va_start(ap, f);
    *err = vfmt(f, ap);
    va_end(ap);
}

static size_t find_sub(const char *h, size_t hn, const char *needle,
                       size_t nn, size_t from)
{
    size_t i;
    if (from > hn || nn > hn - from) return NPOS;
    if (nn == 0) return from;
    for (i = from; i + nn <= hn; i++) {
        if (h[i] == needle[0] && memcmp(h + i, needle, nn) == 0)
            return i;
    }
    return NPOS;
}

static int contains_range(const char *h, size_t hn, size_t start, size_t end,
                          const char *needle)
{
    size_t nn = strlen(needle);
    size_t p;
    if (end > hn) end = hn;
    if (start > end) return 0;
    p = find_sub(h, end, needle, nn, start);
    return p != NPOS && p + nn <= end;
}

static int has_crlf(const char *s, size_t n)
{
    return find_sub(s, n, "\r\n", 2, 0) != NPOS;
}

static char *with_nl(const char *s, const char *nl)
{
    Buf b = {0};
    size_t i;
    for (i = 0; s[i]; i++) {
        if (s[i] == '\n') buf_str(&b, nl);
        else buf_ch(&b, s[i]);
    }
    return buf_take(&b);
}

static char *read_file(const char *path, size_t *out_n, char **err)
{
    FILE *f;
    long sz;
    size_t n;
    char *p;

    f = fopen(path, "rb");
    if (!f) {
        set_err(err, "cannot read %s: %s", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        set_err(err, "cannot read %s: %s", path, strerror(errno));
        return NULL;
    }
    n = (size_t)sz;
    p = (char *)xmalloc(n + 1);
    if (n && fread(p, 1, n, f) != n) {
        fclose(f);
        free(p);
        set_err(err, "cannot read %s: %s", path, strerror(errno));
        return NULL;
    }
    fclose(f);
    p[n] = '\0';
    *out_n = n;
    return p;
}

static int write_file(const char *path, const char *p, size_t n, char **err)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        set_err(err, "cannot write %s: %s", path, strerror(errno));
        return 0;
    }
    if (n && fwrite(p, 1, n, f) != n) {
        fclose(f);
        set_err(err, "cannot write %s: %s", path, strerror(errno));
        return 0;
    }
    if (fclose(f) != 0) {
        set_err(err, "cannot write %s: %s", path, strerror(errno));
        return 0;
    }
    return 1;
}

/* Python's Path.write_text() uses text mode: on Windows every LF in the
 * JSON report becomes CRLF.  Keep the report byte-identical to the Python
 * reference without touching the generated C, which uses newline="". */
static int write_json_file(const char *path, const char *p, size_t n, char **err)
{
#if defined(_WIN32)
    Buf b = {0};
    size_t i;
    int rc;
    for (i = 0; i < n; i++) {
        if (p[i] == '\n') buf_str(&b, "\r\n");
        else buf_ch(&b, p[i]);
    }
    rc = write_file(path, b.p, b.n, err);
    free(b.p);
    return rc;
#else
    return write_file(path, p, n, err);
#endif
}

static void make_parent_dirs(const char *path)
{
    char *tmp = xstrdup(path);
    char *p;
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char save = *p;
            *p = '\0';
            if (tmp[0] && !(strlen(tmp) == 2 && tmp[1] == ':'))
                make_dir(tmp);
            *p = save;
        }
    }
    free(tmp);
}

static int same_path(const char *a, const char *b)
{
#if defined(_WIN32)
    char *fa = _fullpath(NULL, a, 0);
    char *fb = _fullpath(NULL, b, 0);
    int same = fa && fb && _stricmp(fa, fb) == 0;
    free(fa);
    free(fb);
    return same;
#else
    char *fa = realpath(a, NULL);
    char *fb = realpath(b, NULL);
    int same = fa && fb && strcmp(fa, fb) == 0;
    free(fa);
    free(fb);
    return same;
#endif
}

static int site_selected(const Site *site, const char *profile)
{
    int mask = strcmp(profile, "pvpgn") == 0 ? PROFILE_PVP : PROFILE_RUB;
    return (site->profiles & mask) != 0;
}

static const char *status_name(Status st)
{
    switch (st) {
    case ST_APPLIED: return "applied";
    case ST_ALREADY: return "already";
    default: return "absent";
    }
}

/* Returns 1 when found, 0 when absent, -1 on a structural error. */
static int function_span(const char *text, size_t n, const char *name,
                         size_t *out_start, size_t *out_end, char **err)
{
    static const char *const forms[] = {
        "void %s(CPU *c)",
        "void %s(CPU *restrict c)",
    };
    size_t positions[2];
    size_t npos = 0;
    size_t brace;
    size_t start;
    size_t i;
    int depth;
    size_t fi;

    for (fi = 0; fi < sizeof forms / sizeof forms[0]; fi++) {
        char *needle = fmt(forms[fi], name);
        size_t nn = strlen(needle);
        size_t search = 0;
        size_t pos;
        while ((pos = find_sub(text, n, needle, nn, search)) != NPOS) {
            size_t ls = pos;
            while (ls > 0 && text[ls - 1] != '\n') ls--;
            if (pos == ls) {
                size_t t = pos + nn;
                if ((t + 5 <= n && memcmp(text + t, "\r\n{\r\n", 5) == 0) ||
                    (t + 3 <= n && memcmp(text + t, "\n{\n", 3) == 0)) {
                    if (npos < 2) positions[npos] = pos;
                    npos++;
                }
            }
            search = pos + 1;
        }
        free(needle);
    }

    if (npos == 0) return 0;
    if (npos != 1) {
        set_err(err, "%s appears %zu times; expected one", name, npos);
        return -1;
    }

    brace = find_sub(text, n, "{", 1, positions[0]);
    if (brace == NPOS) {
        set_err(err, "%s has no body brace", name);
        return -1;
    }
    start = brace + 1;
    if (start + 2 <= n && text[start] == '\r' && text[start + 1] == '\n')
        start += 2;
    else if (start < n && text[start] == '\n')
        start += 1;

    depth = 1;
    for (i = start; i < n; i++) {
        if (text[i] == '{') depth++;
        else if (text[i] == '}') {
            depth--;
            if (depth == 0) {
                *out_start = start;
                *out_end = i;
                return 1;
            }
        }
    }
    set_err(err, "unterminated body for %s", name);
    return -1;
}

static void normalize_range(Buf *out, const char *s, size_t n)
{
    Buf clean = {0};
    size_t i = 0;
    size_t j;
    int pending = 0;

    while (i < n) {
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            size_t close = find_sub(s, n, "*/", 2, i + 2);
            if (close != NPOS) {
                buf_ch(&clean, ' ');
                i = close + 2;
                continue;
            }
        }
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
            buf_ch(&clean, ' ');
            i += 2;
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        buf_ch(&clean, s[i]);
        i++;
    }

    for (j = 0; j < clean.n; j++) {
        unsigned char c = (unsigned char)clean.p[j];
        if (isspace(c)) {
            pending = 1;
        } else {
            if (pending && out->n) buf_ch(out, ' ');
            pending = 0;
            buf_ch(out, (char)c);
        }
    }
    free(clean.p);
}

static int function_hash(const char *text, size_t n, const char *name,
                         char out[65], char **err)
{
    size_t start, end;
    int rc = function_span(text, n, name, &start, &end, err);
    Buf norm = {0};
    sha256_ctx ctx;
    unsigned char digest[32];

    if (rc <= 0) {
        if (rc == 0) set_err(err, "%s not found while hashing", name);
        return 0;
    }
    normalize_range(&norm, text + start, end - start);
    sha256_init(&ctx);
    sha256_update(&ctx, norm.p ? norm.p : "", norm.n);
    sha256_final(&ctx, digest);
    sha256_hex(digest, out);
    free(norm.p);
    return 1;
}

static void line_info(const char *text, size_t n, size_t pos, size_t end,
                      size_t *line_start, size_t *content_end,
                      size_t *newline_end)
{
    size_t ls = pos;
    size_t ne;
    while (ls > 0 && text[ls - 1] != '\n') ls--;
    ne = pos;
    while (ne < end && text[ne] != '\n') ne++;
    *line_start = ls;
    *newline_end = ne;
    *content_end = ne;
    if (*content_end > ls && text[*content_end - 1] == '\r')
        (*content_end)--;
    (void)n;
}

static int anchor_pos(const char *text, size_t n, size_t start, size_t end,
                      const Site *site, size_t *out, char **err)
{
    size_t alen = strlen(site->anchor);
    size_t pos = find_sub(text, end, site->anchor, alen, start);
    size_t second;

    if (pos == NPOS) return 0;
    second = find_sub(text, end, site->anchor, alen, pos + 1);
    if (second != NPOS) {
        set_err(err, "%s: anchor %s appears more than once in %s",
                site->key, site->va, site->function);
        return -1;
    }
    (void)n;
    *out = pos;
    return 1;
}

static int validate_applied(const char *text, size_t n, size_t start,
                            size_t end, const Site *site, char **err)
{
    size_t apos;
    int arc = anchor_pos(text, n, start, end, site, &apos, err);
    size_t ls = start;
    size_t ce = start;

    if (arc < 0) return 0;
    if (arc > 0) {
        size_t ne;
        line_info(text, n, apos, end, &ls, &ce, &ne);
    }

    if (strcmp(site->key, "auth_req") == 0) {
        if (contains_range(text, n, ls, ce, "0x6F8DDE5Fu") &&
            contains_range(text, n, ls, ce, "if (!c->zf)")) {
            set_err(err, "auth_req marker present but the failure branch is "
                         "still active");
            return 0;
        }
    } else if (strcmp(site->key, "logon_proof_hash_verify") == 0) {
        if (contains_range(text, n, ls, ce, "if (c->zf)")) {
            set_err(err, "logon_proof marker present but je was not inverted");
            return 0;
        }
    } else if (strcmp(site->key, "create_account") == 0) {
        if (contains_range(text, n, ls, ce,
                           "c->ecx = (uint32_t)(rd32((c->ebp + 0x00000010u)))")) {
            set_err(err, "create_account marker present but the original mov "
                         "is still active");
            return 0;
        }
    } else if (strcmp(site->key, "hash_56") == 0 ||
               strcmp(site->key, "hash_54") == 0) {
        if (contains_range(text, n, ls, ce, "dispatch(c, 0x6F8E6D50u)")) {
            set_err(err, "%s marker present but the original hash call is "
                         "still active", site->key);
            return 0;
        }
    } else if (strcmp(site->key, "delay_reducer") == 0) {
        if (contains_range(text, n, ls, ce, "0xFAu")) {
            set_err(err, "delay_reducer marker present but 0xFA is still "
                         "active");
            return 0;
        }
    } else if (strcmp(site->key, "local_auth_checkrev") == 0 ||
               strcmp(site->key, "local_auth_extrawork") == 0) {
        if (contains_range(text, n, ls, ce, "if (!c->cf)")) {
            set_err(err, "%s marker present but the local-auth failure "
                         "branch is still active", site->key);
            return 0;
        }
    } else if (strncmp(site->key, "local_auth", 10) == 0) {
        if (contains_range(text, n, ls, ce,
                           "dispatch_jmp(c, 0x6F094739u)")) {
            set_err(err, "%s marker present but the local-auth failure "
                         "branch is still active", site->key);
            return 0;
        }
    } else if (strcmp(site->key, "rubattle_0x50") == 0) {
        if (!contains_range(text, n, start, end,
                            "push32(c, 0x00000003u);")) {
            set_err(err, "rubattle_0x50 marker present but the append-dword "
                         "call is missing");
            return 0;
        }
    } else if (strcmp(site->key, "rubattle_0x83") == 0) {
        if (!contains_range(text, n, start, end, "wc3_w3l_handle_0x83(c)")) {
            set_err(err, "rubattle_0x83 marker present but the native handler "
                         "call is missing");
            return 0;
        }
    }
    return 1;
}

static int apply_site(const char *text, size_t n, const Site *site,
                      int latency, Edit *out_edit, int *has_edit,
                      Status *out_status, char **err)
{
    size_t start, end;
    size_t mpos;
    size_t apos;
    int rc;
    int arc;
    size_t line_start, content_end, newline_end;
    const char *nl;
    char *replacement = NULL;
    size_t edit_start = 0;
    size_t edit_end = 0;

    (void)latency;
    *has_edit = 0;
    rc = function_span(text, n, site->function, &start, &end, err);
    if (rc < 0) return 0;
    if (rc == 0) {
        *out_status = ST_ABSENT;
        return 1;
    }

    mpos = find_sub(text, end, site->marker, strlen(site->marker), start);
    if (mpos != NPOS) {
        if (!validate_applied(text, n, start, end, site, err)) return 0;
        *out_status = ST_ALREADY;
        return 1;
    }

    arc = anchor_pos(text, n, start, end, site, &apos, err);
    if (arc < 0) return 0;
    if (arc == 0) {
        set_err(err, "%s: %s not found in %s; the corpus changed or the "
                     "wrong input was passed",
                site->key, site->va, site->function);
        return 0;
    }
    line_info(text, n, apos, end, &line_start, &content_end, &newline_end);
    nl = has_crlf(text, n) ? "\r\n" : "\n";

    if (strcmp(site->key, "auth_req") == 0) {
        replacement = xstrdup(
            "    /* WC3_W3L_COMPAT auth_req: VA 0x6F8DDD94 branch NOP "
            "(profile pvpgn/rubattle). */");
        edit_start = line_start;
        edit_end = content_end;
    } else if (strcmp(site->key, "logon_proof_hash_verify") == 0) {
        Buf b = {0};
        size_t p;
        if (!contains_range(text, n, line_start, content_end, "if (c->zf)")) {
            set_err(err, "logon_proof: expected 'if (c->zf)' at 0x6F8DD732");
            return 0;
        }
        p = find_sub(text, content_end, "if (c->zf)", 10, line_start);
        buf_str(&b,
                "    /* WC3_W3L_COMPAT logon_proof: VA 0x6F8DD732 je -> jne. */");
        buf_str(&b, nl);
        buf_mem(&b, text + line_start, p - line_start);
        buf_str(&b, "if (!c->zf)");
        buf_mem(&b, text + p + 10, content_end - (p + 10));
        replacement = buf_take(&b);
        edit_start = line_start;
        edit_end = content_end;
    } else if (strcmp(site->key, "create_account") == 0) {
        replacement = with_nl(
            "    /* WC3_W3L_COMPAT create_account: VA 0x6F9A7DFB.\n"
            "     * wl27 0x10001970 copies 32 plaintext password bytes from "
            "the saved ESI\n"
            "     * ([ebp-4], the caller's password string) to the account "
            "field pointed to\n"
            "     * by [ebp+0x14], then reloads ecx from [ebp+0x10]. */\n"
            "    {\n"
            "        uint32_t _wc3_src = rd32(c->ebp - 0x00000004u);\n"
            "        uint32_t _wc3_dst = rd32(c->ebp + 0x00000014u);\n"
            "        uint32_t _wc3_i;\n"
            "        for (_wc3_i = 0; _wc3_i < 0x20u; _wc3_i++) {\n"
            "            wr8(_wc3_dst + _wc3_i, rd8(_wc3_src + _wc3_i));\n"
            "        }\n"
            "        c->ecx = rd32(c->ebp + 0x00000010u);\n"
            "    }", nl);
        edit_start = line_start;
        edit_end = content_end;
    } else if (strcmp(site->key, "hash_56") == 0 ||
               strcmp(site->key, "hash_54") == 0) {
        const char *packet = strcmp(site->key, "hash_56") == 0 ? "0x56" : "0x54";
        char *fixed = fmt(
            "    c->eax = (uint32_t)wc3_w3l_pvpgn_hash(c, c->ecx, c->edx); "
            "/* WC3_W3L_COMPAT %s: VA %s, packet %s. */\n"
            "    c->esp += 8u; /* wl27 wrapper ret 8; the two ignored "
            "arguments are cleaned. */",
            site->key, site->va, packet);
        replacement = with_nl(fixed, nl);
        free(fixed);
        edit_start = line_start;
        edit_end = content_end;
    } else if (strcmp(site->key, "delay_reducer") == 0) {
        replacement = xstrdup(
            "    c->eax = (uint32_t)(WC3_W3L_LATENCY);"
            " /* WC3_W3L_COMPAT delay_reducer: VA 0x6F970FA0. */");
        edit_start = line_start;
        edit_end = content_end;
    } else if (strcmp(site->key, "local_auth_checkrev") == 0 ||
               strcmp(site->key, "local_auth_extrawork") == 0) {
        const char *success = strcmp(site->key, "local_auth_checkrev") == 0
                                  ? "0x6F09483B" : "0x6F0949B3";
        char *fixed = fmt(
            "    /* %s: VA %s, accept SFileAuthenticateArchive status 1..4. "
            "*/\n"
            "    dispatch_jmp(c, %su); return;",
            site->marker, site->va, success);
        replacement = with_nl(fixed, nl);
        free(fixed);
        edit_start = line_start;
        edit_end = content_end;
    } else if (strncmp(site->key, "local_auth", 10) == 0) {
        replacement = xstrdup(
            "    /* WC3_W3L_COMPAT local_auth: VA 0x6F09439C, accept "
            "SFileAuthenticateArchive status 1..4.  Android HLE workaround; "
            "wl27 has no such patch. */");
        edit_start = line_start;
        edit_end = content_end;
    } else if (strcmp(site->key, "rubattle_0x50") == 0) {
        char *fixed = with_nl(
            "    /* WC3_W3L_COMPAT rubattle_0x50: VA 0x6F8D3F22, append "
            "dword 3 to SID_AUTH_INFO. */\n"
            "    push32(c, 0x00000003u);\n"
            "    c->ecx = (uint32_t)(c->edi);\n"
            "    push32(c, 0x6F8D3F22u); dispatch(c, 0x6F06B3A0u);", nl);
        Buf b = {0};
        buf_str(&b, fixed);
        buf_str(&b, nl);
        free(fixed);
        replacement = buf_take(&b);
        edit_start = line_start;
        edit_end = line_start;
    } else if (strcmp(site->key, "rubattle_0x83") == 0) {
        char *fixed = with_nl(
            "    /* WC3_W3L_COMPAT rubattle_0x83: VA 0x6F8DC63C, native "
            "handler. */\n"
            "    if (c->eax == 0x00000083u) {\n"
            "        wc3_w3l_handle_0x83(c);\n"
            "        return;\n"
            "    }", nl);
        Buf b = {0};
        buf_str(&b, fixed);
        buf_str(&b, nl);
        free(fixed);
        replacement = buf_take(&b);
        edit_start = line_start;
        edit_end = line_start;
    } else {
        set_err(err, "unknown site %s", site->key);
        return 0;
    }

    out_edit->start = edit_start;
    out_edit->end = edit_end;
    out_edit->replacement = replacement;
    *has_edit = 1;
    *out_status = ST_APPLIED;
    (void)newline_end;
    return 1;
}

static int edit_cmp(const void *a, const void *b)
{
    const Edit *x = (const Edit *)a;
    const Edit *y = (const Edit *)b;
    if (x->start < y->start) return -1;
    if (x->start > y->start) return 1;
    if (x->end < y->end) return -1;
    if (x->end > y->end) return 1;
    return 0;
}

static int transform(const char *text, size_t n, const char *profile,
                     int latency, int require_all, char **out, size_t *outn,
                     Result **results, size_t *nresults, char **err)
{
    size_t i;
    Result *res;
    size_t nres = 0;
    Edit *edits = NULL;
    size_t nedits = 0;
    size_t ecap = 0;
    Buf b = {0};
    size_t last = 0;

    if (strcmp(profile, "pvpgn") != 0 && strcmp(profile, "rubattle") != 0) {
        set_err(err, "unknown profile '%s'", profile);
        return 0;
    }
    if (latency < 30 || latency > 255) {
        set_err(err, "latency must be in the wl27 range 30..255");
        return 0;
    }

    res = (Result *)xmalloc(NSITES * sizeof(*res));
    for (i = 0; i < NSITES; i++) {
        const Site *site = &SITES[i];
        Edit edit;
        int has_edit = 0;
        Status st;
        if (!site_selected(site, profile)) continue;
        if (!apply_site(text, n, site, latency, &edit, &has_edit, &st, err)) {
            size_t k;
            for (k = 0; k < nedits; k++) free(edits[k].replacement);
            free(edits);
            free(res);
            return 0;
        }
        if (st == ST_ABSENT && require_all) {
            size_t k;
            set_err(err, "%s: required function %s not found",
                    site->key, site->function);
            for (k = 0; k < nedits; k++) free(edits[k].replacement);
            free(edits);
            free(res);
            return 0;
        }
        if (has_edit) {
            if (nedits == ecap) {
                ecap = ecap ? ecap * 2 : 8;
                edits = (Edit *)xrealloc(edits, ecap * sizeof(*edits));
            }
            edits[nedits++] = edit;
        }
        res[nres].site = site;
        res[nres].status = st;
        nres++;
    }

    qsort(edits, nedits, sizeof(*edits), edit_cmp);
    for (i = 0; i < nedits; i++) {
        buf_mem(&b, text + last, edits[i].start - last);
        buf_str(&b, edits[i].replacement);
        last = edits[i].end;
    }
    buf_mem(&b, text + last, n - last);
    for (i = 0; i < nedits; i++) free(edits[i].replacement);
    free(edits);

    *out = buf_take(&b);
    *outn = b.n;
    *results = res;
    *nresults = nres;
    return 1;
}

static int build_funcs(const char *before, size_t bn, const char *after,
                       size_t an, Result *res, size_t nres,
                       FuncChange **out, size_t *nout, char **err)
{
    FuncChange *f = NULL;
    size_t nf = 0;
    size_t cap = 0;
    size_t i, j;

    for (i = 0; i < nres; i++) {
        FuncChange *g = NULL;
        if (res[i].status == ST_ABSENT) continue;
        for (j = 0; j < nf; j++) {
            if (strcmp(f[j].function, res[i].site->function) == 0) {
                g = &f[j];
                break;
            }
        }
        if (!g) {
            if (nf == cap) {
                cap = cap ? cap * 2 : 8;
                f = (FuncChange *)xrealloc(f, cap * sizeof(*f));
            }
            g = &f[nf++];
            memset(g, 0, sizeof(*g));
            g->function = res[i].site->function;
        }
        if (g->n == g->cap) {
            g->cap = g->cap ? g->cap * 2 : 4;
            g->idx = (size_t *)xrealloc(g->idx, g->cap * sizeof(*g->idx));
        }
        g->idx[g->n++] = i;
    }

    for (i = 0; i < nf; i++) {
        Buf reason = {0};
        char hash[65];
        if (!function_hash(before, bn, f[i].function, hash, err)) goto fail;
        f[i].before_sha = xstrdup(hash);
        if (!function_hash(after, an, f[i].function, hash, err)) goto fail;
        f[i].after_sha = xstrdup(hash);
        for (j = 0; j < f[i].n; j++) {
            if (j) buf_str(&reason, "; ");
            buf_str(&reason, res[f[i].idx[j]].site->reason);
        }
        f[i].reason = buf_take(&reason);
    }

    *out = f;
    *nout = nf;
    return 1;

fail:
    for (i = 0; i < nf; i++) {
        free(f[i].idx);
        free(f[i].before_sha);
        free(f[i].after_sha);
        free(f[i].reason);
    }
    free(f);
    return 0;
}

static void free_funcs(FuncChange *f, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        free(f[i].idx);
        free(f[i].before_sha);
        free(f[i].after_sha);
        free(f[i].reason);
    }
    free(f);
}

static void json_escape(Buf *b, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    buf_ch(b, '"');
    while (*p) {
        unsigned char c = *p++;
        switch (c) {
        case '"': buf_str(b, "\\\""); break;
        case '\\': buf_str(b, "\\\\"); break;
        case '\b': buf_str(b, "\\b"); break;
        case '\f': buf_str(b, "\\f"); break;
        case '\n': buf_str(b, "\\n"); break;
        case '\r': buf_str(b, "\\r"); break;
        case '\t': buf_str(b, "\\t"); break;
        default:
            if (c < 0x20) {
                char tmp[7];
                snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                buf_str(b, tmp);
            } else {
                buf_ch(b, (char)c);
            }
            break;
        }
    }
    buf_ch(b, '"');
}

static void json_indent(Buf *b, int n)
{
    int i;
    for (i = 0; i < n; i++) buf_ch(b, ' ');
}

static void kv_str(Buf *b, int ind, const char *key, const char *val, int comma)
{
    json_indent(b, ind);
    buf_ch(b, '"');
    buf_str(b, key);
    buf_str(b, "\": ");
    json_escape(b, val);
    if (comma) buf_ch(b, ',');
    buf_ch(b, '\n');
}

static void kv_int(Buf *b, int ind, const char *key, int val, int comma)
{
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%d", val);
    json_indent(b, ind);
    buf_ch(b, '"');
    buf_str(b, key);
    buf_str(b, "\": ");
    buf_str(b, tmp);
    if (comma) buf_ch(b, ',');
    buf_ch(b, '\n');
}

static void kv_bool(Buf *b, int ind, const char *key, int val, int comma)
{
    json_indent(b, ind);
    buf_ch(b, '"');
    buf_str(b, key);
    buf_str(b, "\": ");
    buf_str(b, val ? "true" : "false");
    if (comma) buf_ch(b, ',');
    buf_ch(b, '\n');
}

static void kv_null(Buf *b, int ind, const char *key, int comma)
{
    json_indent(b, ind);
    buf_ch(b, '"');
    buf_str(b, key);
    buf_str(b, "\": null");
    if (comma) buf_ch(b, ',');
    buf_ch(b, '\n');
}

static void json_site(Buf *b, const Result *r, int ind)
{
    json_indent(b, ind);
    buf_str(b, "{\n");
    kv_str(b, ind + 2, "key", r->site->key, 1);
    kv_str(b, ind + 2, "va", r->site->va, 1);
    kv_str(b, ind + 2, "function", r->site->function, 1);
    kv_str(b, ind + 2, "reason", r->site->reason, 1);
    kv_str(b, ind + 2, "status", status_name(r->status), 0);
    json_indent(b, ind);
    buf_ch(b, '}');
}

static void site_array_all(Buf *b, Result *res, size_t n, int ind)
{
    size_t i;
    if (n == 0) {
        buf_str(b, "[]");
        return;
    }
    buf_str(b, "[\n");
    for (i = 0; i < n; i++) {
        json_site(b, &res[i], ind + 2);
        if (i + 1 < n) buf_str(b, ",\n");
        else buf_ch(b, '\n');
    }
    json_indent(b, ind);
    buf_ch(b, ']');
}

static void site_array_indices(Buf *b, Result *res, const size_t *idx,
                               size_t n, int ind)
{
    size_t i;
    if (n == 0) {
        buf_str(b, "[]");
        return;
    }
    buf_str(b, "[\n");
    for (i = 0; i < n; i++) {
        json_site(b, &res[idx[i]], ind + 2);
        if (i + 1 < n) buf_str(b, ",\n");
        else buf_ch(b, '\n');
    }
    json_indent(b, ind);
    buf_ch(b, ']');
}

static void json_func(Buf *b, const FuncChange *f, Result *res, int ind)
{
    json_indent(b, ind);
    buf_str(b, "{\n");
    kv_str(b, ind + 2, "va", res[f->idx[0]].site->va, 1);
    kv_str(b, ind + 2, "function", f->function, 1);
    kv_str(b, ind + 2, "before_sha256", f->before_sha, 1);
    kv_str(b, ind + 2, "after_sha256", f->after_sha, 1);
    json_indent(b, ind + 2);
    buf_str(b, "\"sites\": ");
    site_array_indices(b, res, f->idx, f->n, ind + 2);
    buf_str(b, ",\n");
    kv_str(b, ind + 2, "reason", f->reason, 0);
    json_indent(b, ind);
    buf_ch(b, '}');
}

static void func_array(Buf *b, FuncChange *f, size_t n, Result *res, int ind)
{
    size_t i;
    if (n == 0) {
        buf_str(b, "[]");
        return;
    }
    buf_str(b, "[\n");
    for (i = 0; i < n; i++) {
        json_func(b, &f[i], res, ind + 2);
        if (i + 1 < n) buf_str(b, ",\n");
        else buf_ch(b, '\n');
    }
    json_indent(b, ind);
    buf_ch(b, ']');
}

static void missing_array(Buf *b, Result *res, size_t n, int ind)
{
    size_t i;
    size_t count = 0;
    int first = 1;
    for (i = 0; i < n; i++) if (res[i].status == ST_ABSENT) count++;
    if (count == 0) {
        buf_str(b, "[]");
        return;
    }
    buf_str(b, "[\n");
    for (i = 0; i < n; i++) {
        if (res[i].status != ST_ABSENT) continue;
        if (!first) buf_str(b, ",\n");
        first = 0;
        json_site(b, &res[i], ind + 2);
    }
    buf_ch(b, '\n');
    json_indent(b, ind);
    buf_ch(b, ']');
}

static void json_report(Buf *b, const char *src, const char *dst,
                        const char *profile, int latency, int changed,
                        Result *res, size_t nres, FuncChange *funcs,
                        size_t nfuncs)
{
    buf_str(b, "{\n");
    kv_str(b, 2, "pass", PASS, 1);
    kv_int(b, 2, "pass_rev", PASS_REV, 1);
    kv_str(b, 2, "profile", profile, 1);
    kv_int(b, 2, "latency", latency, 1);
    json_indent(b, 2);
    buf_str(b, "\"files\": [\n");
    json_indent(b, 4);
    buf_str(b, "{\n");
    kv_str(b, 6, "path", src, 1);
    if (dst) kv_str(b, 6, "output", dst, 1);
    else kv_null(b, 6, "output", 1);
    kv_bool(b, 6, "applied", changed, 1);
    json_indent(b, 6);
    buf_str(b, "\"sites\": ");
    site_array_all(b, res, nres, 6);
    buf_str(b, ",\n");
    json_indent(b, 6);
    buf_str(b, "\"functions\": ");
    func_array(b, funcs, nfuncs, res, 6);
    buf_ch(b, '\n');
    json_indent(b, 4);
    buf_str(b, "}\n");
    json_indent(b, 2);
    buf_str(b, "],\n");
    kv_bool(b, 2, "applied", changed, 1);
    json_indent(b, 2);
    buf_str(b, "\"missing\": ");
    missing_array(b, res, nres, 2);
    buf_str(b, ",\n");
    kv_str(b, 2, "g15", "unavailable", 1);
    kv_str(b, 2, "risk",
           "ADR-028/ADR-029: LiftPass change without a G-15 "
           "semantic-equivalence report", 0);
    buf_str(b, "}\n");
}

static int process_file(const char *src, const char *dst, const char *json_path,
                        const char *profile, int latency, int require_all,
                        int check, int quiet)
{
    size_t n = 0;
    char *text;
    char *out = NULL;
    size_t outn = 0;
    Result *res = NULL;
    size_t nres = 0;
    FuncChange *funcs = NULL;
    size_t nfuncs = 0;
    char *err = NULL;
    int changed;
    int rc = 2;

    text = read_file(src, &n, &err);
    if (!text) {
        fprintf(stderr, "%s: %s\n", PASS, err ? err : "read failed");
        free(err);
        return 2;
    }
    if (!transform(text, n, profile, latency, require_all, &out, &outn,
                   &res, &nres, &err)) {
        fprintf(stderr, "%s: %s\n", PASS, err ? err : "transform failed");
        free(err);
        free(text);
        return 2;
    }
    changed = outn != n || memcmp(out, text, n) != 0;

    if (!check) {
        if (!dst) {
            fprintf(stderr, "%s: --out is required unless --check is used\n",
                    PASS);
            goto done;
        }
        {
            char *old_err = NULL;
            size_t oldn = 0;
            char *old = read_file(dst, &oldn, &old_err);
            int same = old && oldn == outn && memcmp(old, out, outn) == 0;
            free(old);
            free(old_err);
            if (!same) {
                make_parent_dirs(dst);
                if (!write_file(dst, out, outn, &err)) {
                    fprintf(stderr, "%s: %s\n", PASS,
                            err ? err : "write failed");
                    goto done;
                }
            }
        }
    }

    if (!build_funcs(text, n, out, outn, res, nres, &funcs, &nfuncs, &err)) {
        fprintf(stderr, "%s: %s\n", PASS, err ? err : "hash failed");
        goto done;
    }

    if (json_path) {
        Buf report = {0};
        make_parent_dirs(json_path);
        json_report(&report, src, check ? NULL : dst, profile, latency,
                    changed, res, nres, funcs, nfuncs);
        if (!write_json_file(json_path, report.p, report.n, &err)) {
            fprintf(stderr, "%s: %s\n", PASS, err ? err : "write failed");
            free(report.p);
            goto done;
        }
        free(report.p);
    }

    if (!quiet) {
        const char *verb = check ? "check" : "apply";
        const char *state = changed ? "applied"
                                    : "already applied (idempotent)";
        printf("%s: %s %s (%s, %zu site(s), %zu function(s)): %s\n",
               PASS, verb, src, profile, nres, nfuncs, state);
    }
    rc = 0;

done:
    free(err);
    free(text);
    free(out);
    free(res);
    free_funcs(funcs, nfuncs);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Minimal JSON reader used by --verify-coverage.                      */

static int j_fail(J *j, const char *msg)
{
    if (!j->err[0])
        snprintf(j->err, sizeof(j->err), "%s at byte %zu", msg, j->p);
    return 0;
}

static void j_ws(J *j)
{
    while (j->p < j->n && isspace((unsigned char)j->s[j->p])) j->p++;
}

static int j_char(J *j, char c)
{
    j_ws(j);
    if (j->p >= j->n || j->s[j->p] != c) return j_fail(j, "unexpected token");
    j->p++;
    return 1;
}

static int j_hex4(J *j, unsigned *out)
{
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        unsigned char c;
        if (j->p >= j->n) return j_fail(j, "short unicode escape");
        c = (unsigned char)j->s[j->p++];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return j_fail(j, "bad unicode escape");
    }
    *out = v;
    return 1;
}

static void buf_utf8(Buf *b, unsigned cp)
{
    if (cp < 0x80) {
        buf_ch(b, (char)cp);
    } else if (cp < 0x800) {
        buf_ch(b, (char)(0xC0 | (cp >> 6)));
        buf_ch(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        buf_ch(b, (char)(0xE0 | (cp >> 12)));
        buf_ch(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_ch(b, (char)(0x80 | (cp & 0x3F)));
    }
}

static int j_string(J *j, char **out)
{
    Buf b = {0};
    j_ws(j);
    if (j->p >= j->n || j->s[j->p] != '"')
        return j_fail(j, "expected string");
    j->p++;
    while (j->p < j->n) {
        unsigned char c = (unsigned char)j->s[j->p++];
        if (c == '"') {
            char *s = buf_take(&b);
            if (out) *out = s;
            else free(s);
            return 1;
        }
        if (c == '\\') {
            if (j->p >= j->n) return j_fail(j, "short escape");
            c = (unsigned char)j->s[j->p++];
            switch (c) {
            case '"': buf_ch(&b, '"'); break;
            case '\\': buf_ch(&b, '\\'); break;
            case '/': buf_ch(&b, '/'); break;
            case 'b': buf_ch(&b, '\b'); break;
            case 'f': buf_ch(&b, '\f'); break;
            case 'n': buf_ch(&b, '\n'); break;
            case 'r': buf_ch(&b, '\r'); break;
            case 't': buf_ch(&b, '\t'); break;
            case 'u': {
                unsigned cp;
                if (!j_hex4(j, &cp)) { free(b.p); return 0; }
                buf_utf8(&b, cp);
                break;
            }
            default:
                free(b.p);
                return j_fail(j, "bad escape");
            }
        } else {
            buf_ch(&b, (char)c);
        }
    }
    free(b.p);
    return j_fail(j, "unterminated string");
}

static int j_skip(J *j)
{
    j_ws(j);
    if (j->p >= j->n) return j_fail(j, "unexpected end");
    if (j->s[j->p] == '"') return j_string(j, NULL);
    if (j->s[j->p] == '{') {
        j->p++;
        j_ws(j);
        if (j->p < j->n && j->s[j->p] == '}') { j->p++; return 1; }
        while (1) {
            if (!j_string(j, NULL)) return 0;
            if (!j_char(j, ':')) return 0;
            if (!j_skip(j)) return 0;
            j_ws(j);
            if (j->p < j->n && j->s[j->p] == ',') { j->p++; continue; }
            return j_char(j, '}');
        }
    }
    if (j->s[j->p] == '[') {
        j->p++;
        j_ws(j);
        if (j->p < j->n && j->s[j->p] == ']') { j->p++; return 1; }
        while (1) {
            if (!j_skip(j)) return 0;
            j_ws(j);
            if (j->p < j->n && j->s[j->p] == ',') { j->p++; continue; }
            return j_char(j, ']');
        }
    }
    if (j->p + 4 <= j->n && memcmp(j->s + j->p, "true", 4) == 0) {
        j->p += 4; return 1;
    }
    if (j->p + 5 <= j->n && memcmp(j->s + j->p, "false", 5) == 0) {
        j->p += 5; return 1;
    }
    if (j->p + 4 <= j->n && memcmp(j->s + j->p, "null", 4) == 0) {
        j->p += 4; return 1;
    }
    if (j->s[j->p] == '-' || isdigit((unsigned char)j->s[j->p])) {
        j->p++;
        while (j->p < j->n &&
               (isdigit((unsigned char)j->s[j->p]) ||
                j->s[j->p] == '.' || j->s[j->p] == 'e' ||
                j->s[j->p] == 'E' || j->s[j->p] == '+' ||
                j->s[j->p] == '-'))
            j->p++;
        return 1;
    }
    return j_fail(j, "unexpected value");
}

static int site_rec_push(SiteRecVec *v, char *key, char *status)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->keys = (char **)xrealloc(v->keys, v->cap * sizeof(*v->keys));
        v->statuses = (char **)xrealloc(v->statuses,
                                        v->cap * sizeof(*v->statuses));
    }
    v->keys[v->n] = key;
    v->statuses[v->n] = status;
    v->n++;
    return 1;
}

static void site_rec_free(SiteRecVec *v)
{
    size_t i;
    for (i = 0; i < v->n; i++) {
        free(v->keys[i]);
        free(v->statuses[i]);
    }
    free(v->keys);
    free(v->statuses);
    memset(v, 0, sizeof(*v));
}

static int parse_sites(J *j, SiteRecVec *out)
{
    if (!j_char(j, '[')) return 0;
    j_ws(j);
    if (j->p < j->n && j->s[j->p] == ']') { j->p++; return 1; }
    while (1) {
        char *key = NULL;
        char *status = NULL;
        if (!j_char(j, '{')) goto fail;
        while (1) {
            char *k = NULL;
            j_ws(j);
            if (j->p < j->n && j->s[j->p] == '}') { j->p++; break; }
            if (!j_string(j, &k)) goto fail;
            if (!j_char(j, ':')) { free(k); goto fail; }
            if (strcmp(k, "key") == 0) {
                free(key);
                if (!j_string(j, &key)) { free(k); goto fail; }
            } else if (strcmp(k, "status") == 0) {
                free(status);
                if (!j_string(j, &status)) { free(k); goto fail; }
            } else {
                if (!j_skip(j)) { free(k); goto fail; }
            }
            free(k);
            j_ws(j);
            if (j->p < j->n && j->s[j->p] == ',') { j->p++; continue; }
            if (!j_char(j, '}')) goto fail;
            break;
        }
        if (!key || !status) {
            j_fail(j, "malformed site entry");
            goto fail;
        }
        site_rec_push(out, key, status);
        key = status = NULL;
        j_ws(j);
        if (j->p < j->n && j->s[j->p] == ',') { j->p++; continue; }
        if (!j_char(j, ']')) return 0;
        return 1;
fail:
        free(key);
        free(status);
        return 0;
    }
}

static int parse_files(J *j, SiteRecVec *out)
{
    size_t files = 0;
    if (!j_char(j, '[')) return 0;
    j_ws(j);
    if (j->p < j->n && j->s[j->p] == ']')
        return j_fail(j, "change set has no files");
    while (1) {
        int have_sites = 0;
        if (!j_char(j, '{')) return 0;
        while (1) {
            char *k = NULL;
            j_ws(j);
            if (j->p < j->n && j->s[j->p] == '}') { j->p++; break; }
            if (!j_string(j, &k)) return 0;
            if (!j_char(j, ':')) { free(k); return 0; }
            if (strcmp(k, "sites") == 0) {
                if (!parse_sites(j, out)) { free(k); return 0; }
                have_sites = 1;
            } else {
                if (!j_skip(j)) { free(k); return 0; }
            }
            free(k);
            j_ws(j);
            if (j->p < j->n && j->s[j->p] == ',') { j->p++; continue; }
            if (!j_char(j, '}')) return 0;
            break;
        }
        if (!have_sites) return j_fail(j, "malformed site list");
        files++;
        j_ws(j);
        if (j->p < j->n && j->s[j->p] == ',') { j->p++; continue; }
        if (!j_char(j, ']')) return 0;
        break;
    }
    if (files == 0) return j_fail(j, "change set has no files");
    return 1;
}

static int parse_report(const char *text, size_t n, const char *label,
                        const char *profile, SiteRecVec *out, char **err)
{
    J j;
    int have_pass = 0;
    int have_profile = 0;
    int have_files = 0;

    memset(&j, 0, sizeof(j));
    j.s = text;
    j.n = n;

    if (!j_char(&j, '{')) goto fail;
    while (1) {
        char *k = NULL;
        j_ws(&j);
        if (j.p < j.n && j.s[j.p] == '}') { j.p++; break; }
        if (!j_string(&j, &k)) goto fail;
        if (!j_char(&j, ':')) { free(k); goto fail; }
        if (strcmp(k, "pass") == 0) {
            char *v = NULL;
            if (!j_string(&j, &v)) { free(k); goto fail; }
            have_pass = strcmp(v, PASS) == 0;
            free(v);
            if (!have_pass) {
                free(k);
                set_err(err, "%s: not a %s change set", label, PASS);
                return 0;
            }
        } else if (strcmp(k, "profile") == 0) {
            char *v = NULL;
            if (!j_string(&j, &v)) { free(k); goto fail; }
            have_profile = 1;
            if (strcmp(v, profile) != 0) {
                set_err(err, "%s: profile '%s', expected '%s'",
                        label, v, profile);
                free(v);
                free(k);
                return 0;
            }
            free(v);
        } else if (strcmp(k, "files") == 0) {
            if (!parse_files(&j, out)) { free(k); goto fail; }
            have_files = 1;
        } else {
            if (!j_skip(&j)) { free(k); goto fail; }
        }
        free(k);
        j_ws(&j);
        if (j.p < j.n && j.s[j.p] == ',') { j.p++; continue; }
        if (!j_char(&j, '}')) goto fail;
        break;
    }
    if (!have_pass || !have_profile || !have_files) {
        j_fail(&j, "change set is missing pass, profile or files");
        goto fail;
    }
    return 1;

fail:
    set_err(err, "%s: invalid change set: %s", label,
            j.err[0] ? j.err : "malformed JSON");
    return 0;
}

static int key_count_find(char **keys, size_t n, const char *key)
{
    size_t i;
    for (i = 0; i < n; i++) if (strcmp(keys[i], key) == 0) return (int)i;
    return -1;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int coverage_check_vec(SiteRecVec *recs, const char *profile, char **err)
{
    char **keys = NULL;
    int *counts = NULL;
    size_t nkeys = 0;
    size_t i;
    size_t nexpected = 0;
    char **expected = NULL;
    char **missing = NULL;
    size_t nmissing = 0;
    char **dups = NULL;
    size_t ndups = 0;
    char **unexpected = NULL;
    size_t nunexpected = 0;
    Buf msg = {0};
    int rc = 0;

    for (i = 0; i < NSITES; i++) {
        if (site_selected(&SITES[i], profile)) nexpected++;
    }
    expected = (char **)xmalloc(nexpected * sizeof(*expected));
    nexpected = 0;
    for (i = 0; i < NSITES; i++) {
        if (site_selected(&SITES[i], profile))
            expected[nexpected++] = (char *)SITES[i].key;
    }

    for (i = 0; i < recs->n; i++) {
        const char *key = recs->keys[i];
        const char *status = recs->statuses[i];
        int pos;
        if (strcmp(status, "applied") != 0 &&
            strcmp(status, "already") != 0 &&
            strcmp(status, "absent") != 0) {
            set_err(err, "coverage: unknown status '%s' for '%s'",
                    status, key);
            goto done;
        }
        if (strcmp(status, "absent") == 0) continue;
        pos = key_count_find(keys, nkeys, key);
        if (pos < 0) {
            keys = (char **)xrealloc(keys, (nkeys + 1) * sizeof(*keys));
            counts = (int *)xrealloc(counts, (nkeys + 1) * sizeof(*counts));
            keys[nkeys] = (char *)key;
            counts[nkeys] = 1;
            nkeys++;
        } else {
            counts[pos]++;
        }
    }

    for (i = 0; i < nkeys; i++) {
        int j;
        int is_expected = 0;
        for (j = 0; j < (int)nexpected; j++) {
            if (strcmp(keys[i], expected[j]) == 0) { is_expected = 1; break; }
        }
        if (counts[i] > 1) {
            dups = (char **)xrealloc(dups, (ndups + 1) * sizeof(*dups));
            dups[ndups++] = keys[i];
        }
        if (!is_expected) {
            unexpected = (char **)xrealloc(
                unexpected, (nunexpected + 1) * sizeof(*unexpected));
            unexpected[nunexpected++] = keys[i];
        }
    }
    for (i = 0; i < nexpected; i++) {
        if (key_count_find(keys, nkeys, expected[i]) < 0) {
            missing = (char **)xrealloc(missing,
                                        (nmissing + 1) * sizeof(*missing));
            missing[nmissing++] = expected[i];
        }
    }

    if (nmissing) qsort(missing, nmissing, sizeof(*missing), cmp_str);
    if (ndups) qsort(dups, ndups, sizeof(*dups), cmp_str);
    if (nunexpected) qsort(unexpected, nunexpected, sizeof(*unexpected), cmp_str);

    if (nmissing) {
        buf_str(&msg, "site(s) not found in any change set: ");
        for (i = 0; i < nmissing; i++) {
            if (i) buf_str(&msg, ", ");
            buf_str(&msg, missing[i]);
        }
    }
    if (ndups) {
        if (msg.n) buf_str(&msg, "; ");
        buf_str(&msg, "site(s) found more than once: ");
        for (i = 0; i < ndups; i++) {
            if (i) buf_str(&msg, ", ");
            buf_str(&msg, dups[i]);
        }
    }
    if (nunexpected) {
        if (msg.n) buf_str(&msg, "; ");
        buf_str(&msg, "site(s) outside the profile: ");
        for (i = 0; i < nunexpected; i++) {
            if (i) buf_str(&msg, ", ");
            buf_str(&msg, unexpected[i]);
        }
    }
    if (msg.n) {
        set_err(err, "coverage: %s", msg.p);
        goto done;
    }
    rc = 1;

done:
    free(msg.p);
    free(expected);
    free(missing);
    free(dups);
    free(unexpected);
    free(keys);
    free(counts);
    return rc;
}

static int verify_coverage(char **paths, size_t npaths, const char *profile,
                           int quiet)
{
    SiteRecVec recs = {0};
    size_t i;
    char *err = NULL;
    int rc = 2;

    if (strcmp(profile, "pvpgn") != 0 && strcmp(profile, "rubattle") != 0) {
        fprintf(stderr, "%s: unknown profile '%s'\n", PASS, profile);
        return 2;
    }
    for (i = 0; i < npaths; i++) {
        size_t n = 0;
        char *text = read_file(paths[i], &n, &err);
        if (!text) {
            fprintf(stderr, "%s: %s\n", PASS, err ? err : "read failed");
            goto done;
        }
        if (!parse_report(text, n, paths[i], profile, &recs, &err)) {
            free(text);
            fprintf(stderr, "%s: %s\n", PASS, err ? err : "invalid report");
            goto done;
        }
        free(text);
    }
    if (!coverage_check_vec(&recs, profile, &err)) {
        fprintf(stderr, "%s: %s\n", PASS, err ? err : "coverage failed");
        goto done;
    }
    if (!quiet)
        printf("%s: coverage ok for %s (%zu change set(s))\n",
               PASS, profile, npaths);
    rc = 0;

done:
    free(err);
    site_rec_free(&recs);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Self-test                                                          */

static const char *SELF_TEST_FIXTURE =
"/* self-test */\n"
"void L_6F8DDC80(CPU *restrict c)\n"
"{\n"
"    if (!c->zf) { dispatch_jmp(c, 0x6F8DDE5Fu); return; }        "
"/* 6F8DDD94: jne 0x6f8dde5f */\n"
"    c->ecx += 4;\n"
"}\n"
"void L_6F8DD71D(CPU *c)\n"
"{\n"
"    if (c->zf) { dispatch_jmp(c, 0x6F8DD754u); return; }         "
"/* 6F8DD732: je 0x6f8dd754 */\n"
"}\n"
"void L_6F9A7DB0(CPU *c)\n"
"{\n"
"    c->ecx = (uint32_t)(rd32((c->ebp + 0x00000010u)));           "
"/* 6F9A7DFB: mov ecx, dword ptr [ebp + 0x10] */\n"
"}\n"
"void L_6F8DD290(CPU *c)\n"
"{\n"
"    push32(c, 0x6F8DD2A9u); dispatch(c, 0x6F8E6D50u);            "
"/* 6F8DD2A4: call 0x6f8e6d50 */\n"
"}\n"
"void L_6F8DD62D(CPU *c)\n"
"{\n"
"    push32(c, 0x6F8DD643u); dispatch(c, 0x6F8E6D50u);            "
"/* 6F8DD63E: call 0x6f8e6d50 */\n"
"}\n"
"void L_6F970FA0(CPU *c)\n"
"{\n"
"    c->eax = (uint32_t)(0xFAu);                                  "
"/* 6F970FA0: mov eax, 0xfa */\n"
"}\n"
"void L_6F8D3EB0(CPU *c)\n"
"{\n"
"    c->eax = (uint32_t)(c->edi);                                 "
"/* 6F8D3F22: mov eax, edi */\n"
"}\n"
"void L_6F8DC630(CPU *c)\n"
"{\n"
"    flags_sub(c, c->eax, 0x82u, 4);                              "
"/* 6F8DC63C: cmp eax, 0x82 */\n"
"}\n"
"void L_6F094308(CPU *c)\n"
"{\n"
"    if (c->cf) { dispatch_jmp(c, 0x6F094739u); return; }         "
"/* 6F09439C: jb 0x6f094739 */\n"
"}\n"
"void L_6F094364(CPU *c)\n"
"{\n"
"    if (c->cf) { dispatch_jmp(c, 0x6F094739u); return; }         "
"/* 6F09439C: jb 0x6f094739 */\n"
"}\n"
"void L_6F094807(CPU *c)\n"
"{\n"
"    if (!c->cf) { dispatch_jmp(c, 0x6F09483Bu); return; }        "
"/* 6F09482F: jae 0x6f09483b */\n"
"    c->esi = (uint32_t)(0x4u);                                   "
"/* 6F094831: mov esi, 4 */\n"
"    dispatch_jmp(c, 0x6F0948ECu); return;                        "
"/* 6F094836: jmp 0x6f0948ec */\n"
"}\n"
"void L_6F09497F(CPU *c)\n"
"{\n"
"    if (!c->cf) { dispatch_jmp(c, 0x6F0949B3u); return; }        "
"/* 6F0949A7: jae 0x6f0949b3 */\n"
"    c->esi = (uint32_t)(0x4u);                                   "
"/* 6F0949A9: mov esi, 4 */\n"
"    dispatch_jmp(c, 0x6F094A50u); return;                        "
"/* 6F0949AE: jmp 0x6f094a50 */\n"
"}\n";

static size_t count_sub(const char *s, const char *needle)
{
    size_t n = strlen(needle);
    size_t count = 0;
    size_t p = 0;
    if (n == 0) return 0;
    while ((p = find_sub(s, strlen(s), needle, n, p)) != NPOS) {
        count++;
        p += n;
    }
    return count;
}

static char *replace_first(const char *s, const char *from, const char *to)
{
    size_t n = strlen(s);
    size_t p = find_sub(s, n, from, strlen(from), 0);
    Buf b = {0};
    if (p == NPOS) return xstrdup(s);
    buf_mem(&b, s, p);
    buf_str(&b, to);
    buf_str(&b, s + p + strlen(from));
    return buf_take(&b);
}

static int self_test(void)
{
    char *rub = NULL;
    size_t rubn = 0;
    Result *res = NULL;
    size_t nres = 0;
    char *again = NULL;
    size_t againn = 0;
    Result *res2 = NULL;
    size_t nres2 = 0;
    char *pvp = NULL;
    size_t pvpn = 0;
    Result *res3 = NULL;
    size_t nres3 = 0;
    char *err = NULL;
    size_t flen = strlen(SELF_TEST_FIXTURE);
    int problems = 0;
    char *missing_fixture;
    char *require_fixture;

    if (!transform(SELF_TEST_FIXTURE, flen, "rubattle", 100, 0, &rub, &rubn,
                   &res, &nres, &err)) {
        fprintf(stderr, "w3l_compat self-test: %s\n", err ? err : "failed");
        free(err);
        return 1;
    }
    if (!transform(rub, rubn, "rubattle", 100, 0, &again, &againn, &res2,
                   &nres2, &err)) {
        fprintf(stderr, "w3l_compat self-test: %s\n", err ? err : "failed");
        free(err);
        return 1;
    }
    if (!transform(SELF_TEST_FIXTURE, flen, "pvpgn", 100, 0, &pvp, &pvpn,
                   &res3, &nres3, &err)) {
        fprintf(stderr, "w3l_compat self-test: %s\n", err ? err : "failed");
        free(err);
        return 1;
    }

    missing_fixture = replace_first(SELF_TEST_FIXTURE, "/* 6F8DDD94:",
                                    "/* 6F8DDD95:");
    {
        char *bad = NULL;
        size_t badn = 0;
        Result *bres = NULL;
        size_t bn = 0;
        char *berr = NULL;
        if (transform(missing_fixture, strlen(missing_fixture), "rubattle",
                      100, 0, &bad, &badn, &bres, &bn, &berr)) {
            problems++;
            fprintf(stderr, "w3l_compat self-test: missing site did not fail\n");
            free(bad);
            free(bres);
        }
        free(berr);
    }
    free(missing_fixture);

    require_fixture = replace_first(SELF_TEST_FIXTURE,
                                    "void L_6F8DDC80(CPU *restrict c)",
                                    "void L_NOT_PRESENT(CPU *c)");
    {
        char *bad = NULL;
        size_t badn = 0;
        Result *bres = NULL;
        size_t bn = 0;
        char *berr = NULL;
        if (transform(require_fixture, strlen(require_fixture), "pvpgn",
                      100, 1, &bad, &badn, &bres, &bn, &berr)) {
            problems++;
            fprintf(stderr,
                    "w3l_compat self-test: --require-all did not fail\n");
            free(bad);
            free(bres);
        }
        free(berr);
    }
    free(require_fixture);

    if (nres != 12) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: rubattle transformed %zu "
                        "sites, expected 12\n", nres);
    }
    if (count_sub(rub, "WC3_W3L_COMPAT") != 12) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: rubattle marker count is not "
                        "12\n");
    }
    if (count_sub(rub, "dispatch_jmp(c, 0x6F094739u)") != 0) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: local_auth failure branch is "
                        "still active\n");
    }
    if (strstr(rub, "if (!c->cf) { dispatch_jmp(c, 0x6F09483Bu)")) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: CheckRevision local-auth "
                        "branch is still active\n");
    }
    if (strstr(rub, "if (!c->cf) { dispatch_jmp(c, 0x6F0949B3u)")) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: ExtraWork local-auth branch is "
                        "still active\n");
    }
    if (count_sub(rub, "wc3_w3l_pvpgn_hash") != 2) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: hash helper call count is not "
                        "2\n");
    }
    if (count_sub(rub, "wc3_w3l_handle_0x83") != 1) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: 0x83 handler call missing\n");
    }
    if (count_sub(rub, "0x00000003u") != 1) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: 0x50 append-dword call "
                        "missing\n");
    }
    if (!strstr(rub, "WC3_W3L_LATENCY")) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: delay reducer did not use "
                        "WC3_W3L_LATENCY\n");
    }
    if (!strstr(rub, "if (!c->zf) { dispatch_jmp(c, 0x6F8DD754u)")) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: logon_proof je was not "
                        "inverted\n");
    }
    if (strstr(rub, "if (!c->zf) { dispatch_jmp(c, 0x6F8DDE5Fu)")) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: auth_req branch is still "
                        "active\n");
    }
    if (againn != rubn || memcmp(again, rub, rubn) != 0) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: pass is not idempotent\n");
    }
    if (count_sub(pvp, "WC3_W3L_COMPAT") != 10) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: pvpgn marker count is not 10\n");
    }
    if (count_sub(pvp, "dispatch_jmp(c, 0x6F094739u)") != 0) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: pvpgn local_auth failure "
                        "branch is still active\n");
    }
    if (strstr(pvp, "if (!c->cf) { dispatch_jmp(c, 0x6F09483Bu)")) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: pvpgn CheckRevision local-auth "
                        "branch is still active\n");
    }
    if (strstr(pvp, "if (!c->cf) { dispatch_jmp(c, 0x6F0949B3u)")) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: pvpgn ExtraWork local-auth "
                        "branch is still active\n");
    }
    if (strstr(pvp, "wc3_w3l_handle_0x83")) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: pvpgn profile applied the "
                        "rubattle 0x83 handler\n");
    }
    if (strstr(pvp, "0x00000003u")) {
        problems++;
        fprintf(stderr, "w3l_compat self-test: pvpgn profile applied the "
                        "rubattle 0x50 tail\n");
    }
    {
        Buf nb = {0};
        Buf ob = {0};
        normalize_range(&nb, rub, rubn);
        normalize_range(&ob, SELF_TEST_FIXTURE, flen);
        if (nb.n == ob.n) {
            problems++;
            fprintf(stderr, "w3l_compat self-test: normalizer did not see a "
                            "function change\n");
        }
        free(nb.p);
        free(ob.p);
    }
    {
        FuncChange *funcs = NULL;
        size_t nfuncs = 0;
        Buf report = {0};
        SiteRecVec recs = {0};
        char *cerr = NULL;
        char *short_json =
            "{\"pass\":\"w3l_compat\",\"profile\":\"rubattle\","
            "\"files\":[{\"sites\":[{\"key\":\"auth_req\","
            "\"status\":\"applied\"}]}]}";
        if (!build_funcs(SELF_TEST_FIXTURE, flen, rub, rubn, res, nres,
                         &funcs, &nfuncs, &err)) {
            problems++;
            fprintf(stderr, "w3l_compat self-test: %s\n",
                    err ? err : "hash failed");
        } else {
            json_report(&report, "self", NULL, "rubattle", 100, 1, res,
                        nres, funcs, nfuncs);
            if (!parse_report(report.p, report.n, "self", "rubattle", &recs,
                              &cerr)) {
                problems++;
                fprintf(stderr, "w3l_compat self-test: coverage parser "
                                "rejected a report: %s\n",
                        cerr ? cerr : "parse failed");
            } else if (!coverage_check_vec(&recs, "rubattle", &cerr)) {
                problems++;
                fprintf(stderr, "w3l_compat self-test: coverage rejected a "
                                "complete change set: %s\n",
                        cerr ? cerr : "coverage failed");
            }
            free(cerr);
            cerr = NULL;
            site_rec_free(&recs);
            if (!parse_report(short_json, strlen(short_json), "short",
                              "rubattle", &recs, &cerr)) {
                problems++;
                fprintf(stderr, "w3l_compat self-test: short report parse "
                                "failed: %s\n", cerr ? cerr : "parse failed");
            } else if (coverage_check_vec(&recs, "rubattle", &cerr)) {
                problems++;
                fprintf(stderr, "w3l_compat self-test: coverage did not "
                                "notice a missing site\n");
            }
            free(cerr);
            site_rec_free(&recs);
            free_funcs(funcs, nfuncs);
        }
        free(report.p);
    }

    free(err);
    free(rub);
    free(res);
    free(again);
    free(res2);
    free(pvp);
    free(res3);

    if (problems) return 1;
    printf("w3l_compat self-test: ok\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI                                                                */

static void usage(FILE *f)
{
    fprintf(f,
            "usage: w3l_compat [--profile pvpgn|rubattle] [--latency N]\n"
            "                  --lift IN --out OUT [--json REPORT]\n"
            "                  [--require-all] [--check] [--quiet]\n"
            "       w3l_compat --profile P --verify-coverage JSON...\n"
            "       w3l_compat --self-test\n");
}

static int expand_at_args(int argc, char **argv, StrVec *out)
{
    int i;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '@' && a[1]) {
            size_t n = 0;
            char *err = NULL;
            char *text = read_file(a + 1, &n, &err);
            size_t p = 0;
            if (!text) {
                fprintf(stderr, "%s: %s\n", PASS, err ? err : "read failed");
                free(err);
                return 0;
            }
            while (p < n) {
                size_t start = p;
                size_t end;
                while (p < n && text[p] != '\n') p++;
                end = p;
                if (p < n) p++;
                while (start < end && isspace((unsigned char)text[start]))
                    start++;
                while (end > start && isspace((unsigned char)text[end - 1]))
                    end--;
                if (end > start)
                    vec_push(out, xstrndup(text + start, end - start));
            }
            free(text);
        } else {
            vec_push(out, xstrdup(a));
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    StrVec args = {0};
    StrVec cov_paths = {0};
    const char *lift = NULL;
    const char *out = NULL;
    const char *json_path = NULL;
    const char *profile = "rubattle";
    int latency = 100;
    int require_all = 0;
    int check = 0;
    int quiet = 0;
    int self = 0;
    int coverage = 0;
    size_t i;

    if (!expand_at_args(argc, argv, &args)) {
        vec_free(&args);
        return 2;
    }

    for (i = 0; i < args.n; i++) {
        const char *a = args.v[i];
        if (strcmp(a, "--self-test") == 0) {
            self = 1;
        } else if (strcmp(a, "--require-all") == 0) {
            require_all = 1;
        } else if (strcmp(a, "--check") == 0) {
            check = 1;
        } else if (strcmp(a, "--quiet") == 0) {
            quiet = 1;
        } else if (strcmp(a, "--verify-coverage") == 0) {
            coverage = 1;
            i++;
            while (i < args.n &&
                   !(args.v[i][0] == '-' && args.v[i][1] == '-')) {
                vec_push(&cov_paths, xstrdup(args.v[i]));
                i++;
            }
            if (i > 0) i--;
        } else if (strcmp(a, "--lift") == 0) {
            if (++i >= args.n) {
                fprintf(stderr, "%s: --lift needs a value\n", PASS);
                vec_free(&args);
                return 2;
            }
            lift = args.v[i];
        } else if (strcmp(a, "--out") == 0) {
            if (++i >= args.n) {
                fprintf(stderr, "%s: --out needs a value\n", PASS);
                vec_free(&args);
                return 2;
            }
            out = args.v[i];
        } else if (strcmp(a, "--json") == 0) {
            if (++i >= args.n) {
                fprintf(stderr, "%s: --json needs a value\n", PASS);
                vec_free(&args);
                return 2;
            }
            json_path = args.v[i];
        } else if (strcmp(a, "--profile") == 0) {
            if (++i >= args.n) {
                fprintf(stderr, "%s: --profile needs a value\n", PASS);
                vec_free(&args);
                return 2;
            }
            profile = args.v[i];
        } else if (strcmp(a, "--latency") == 0) {
            char *endp = NULL;
            long v;
            if (++i >= args.n) {
                fprintf(stderr, "%s: --latency needs a value\n", PASS);
                vec_free(&args);
                return 2;
            }
            v = strtol(args.v[i], &endp, 10);
            if (!endp || *endp != '\0') {
                fprintf(stderr, "%s: invalid --latency '%s'\n",
                        PASS, args.v[i]);
                vec_free(&args);
                return 2;
            }
            latency = (int)v;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            vec_free(&args);
            return 0;
        } else {
            fprintf(stderr, "%s: unknown option '%s'\n", PASS, a);
            usage(stderr);
            vec_free(&args);
            return 2;
        }
    }

    if (self) {
        vec_free(&args);
        return self_test();
    }
    if (coverage) {
        int rc;
        if (cov_paths.n == 0) {
            fprintf(stderr, "%s: --verify-coverage needs at least one JSON\n",
                    PASS);
            vec_free(&cov_paths);
            vec_free(&args);
            return 2;
        }
        rc = verify_coverage(cov_paths.v, cov_paths.n, profile, quiet);
        vec_free(&cov_paths);
        vec_free(&args);
        return rc;
    }
    if (!lift) {
        fprintf(stderr, "%s: --lift is required unless --self-test is used\n",
                PASS);
        vec_free(&args);
        return 2;
    }
    if (!check && !out) {
        fprintf(stderr, "%s: --out is required unless --check is used\n", PASS);
        vec_free(&args);
        return 2;
    }
    if (out && same_path(lift, out)) {
        fprintf(stderr, "%s: --out must differ from --lift; replace the "
                        "corpus explicitly\n", PASS);
        vec_free(&args);
        return 2;
    }
    if (latency < 30 || latency > 255) {
        fprintf(stderr, "%s: --latency must be in the wl27 range 30..255\n",
                PASS);
        vec_free(&args);
        return 2;
    }

    {
        int rc = process_file(lift, out, json_path, profile, latency,
                              require_all, check, quiet);
        vec_free(&args);
        return rc;
    }
}
