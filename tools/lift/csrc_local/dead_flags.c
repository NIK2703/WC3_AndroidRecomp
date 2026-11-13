/* dead_flags.c - P0-2 dead-flag elimination.
 *
 * P0-2: remove flag side effects from a flag-defining helper only when a
 * later flag-defining helper in the same straight-line block overwrites every
 * flag that could be observed.  Control flow, labels, calls, dispatcher
 * boundaries and unknown helpers are barriers.  Arithmetic/logic results are
 * always preserved.
 *
 *   dead_flags --lift IN --out OUT [--json REPORT] [--quiet]
 *   dead_flags --in-dir DIR --out-dir DIR [--json REPORT] [--quiet]
 *   dead_flags --self-test
 */
#include <ctype.h>
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

typedef struct {
    char *p;
    size_t n;
    size_t cap;
} Buf;

typedef struct {
    char *name;
    size_t start;
    size_t open;
    size_t close;
    char **args;
    size_t nargs;
} Call;

typedef struct {
    Call *v;
    size_t n;
    size_t cap;
} CallVec;

typedef struct {
    size_t line;
    size_t start;
    size_t end;
    char *source_text;
    char *repl_text;
    unsigned flags;
    unsigned field_count;
    int live;
} WriteRecord;

typedef struct {
    char *text;
    size_t len;
} Line;

typedef struct {
    Line *v;
    size_t n;
    size_t cap;
} LineVec;

typedef struct {
    char **v;
    size_t n;
    size_t cap;
} StrVec;

typedef struct {
    char *lift_file;
    char *output;
    size_t functions;
    size_t flag_writes;
    size_t dead_writes;
    size_t dead_flag_fields;
    size_t changed_functions;
    StrVec change_set;
    StrVec errors;
} FileReport;

enum {
    F_CF = 1u << 0,
    F_ZF = 1u << 1,
    F_SF = 1u << 2,
    F_OF = 1u << 3,
    F_AF = 1u << 4,
    F_PF = 1u << 5
};

#define ALL_FLAGS (F_CF | F_ZF | F_SF | F_OF | F_AF | F_PF)
#define LOGIC_FLAGS (F_CF | F_ZF | F_SF | F_OF | F_PF)
#define INC_FLAGS (F_ZF | F_SF | F_OF | F_AF | F_PF)

static const char *const FLAG_NAMES[6] = {
    "cf", "zf", "sf", "of", "af", "pf"
};

static void die(const char *msg)
{
    fprintf(stderr, "dead_flags: %s\n", msg);
    exit(2);
}

static void die_oom(void)
{
    die("out of memory");
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die_oom();
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)xrealloc(NULL, n);
    memcpy(p, s, n);
    return p;
}

static char *xstrndup(const char *s, size_t n)
{
    char *p = (char *)xrealloc(NULL, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

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

static void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) die("formatting failed");
    buf_need(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    b->n += (size_t)n;
    b->p[b->n] = 0;
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

static void mkdir_parent(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *last = slash;
    if (bslash && (!last || bslash > last)) last = bslash;
    if (last && last != path) {
        char *parent = xstrdup(path);
        parent[(size_t)(last - path)] = 0;
        mkdir_p(parent);
        free(parent);
    }
}

static int starts_line(const char *s, size_t p)
{
    return p == 0 || s[p - 1] == '\n';
}

static int is_word(int c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static int is_hex(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/* Returns the end of the function header (after the opening brace), or 0. */
static size_t function_header_at(const char *s, size_t n, size_t p,
                                 char va[9])
{
    size_t i = p;
    if (!starts_line(s, p)) return 0;
    if (i + 4 > n || memcmp(s + i, "void", 4) != 0) return 0;
    i += 4;
    while (i < n && isspace((unsigned char)s[i])) i++;
    if (i + 2 > n || s[i] != 'L' || s[i + 1] != '_') return 0;
    i += 2;
    if (i + 8 > n) return 0;
    {
        int k;
        for (k = 0; k < 8; k++) {
            if (!is_hex((unsigned char)s[i + k])) return 0;
            va[k] = (char)toupper((unsigned char)s[i + k]);
        }
        va[8] = 0;
    }
    i += 8;
    while (i < n && isspace((unsigned char)s[i])) i++;
    if (i >= n || s[i] != '(') return 0;
    i++;
    while (i < n && isspace((unsigned char)s[i])) i++;
    if (i + 3 > n || memcmp(s + i, "CPU", 3) != 0) return 0;
    i += 3;
    while (i < n && isspace((unsigned char)s[i])) i++;
    if (i >= n || s[i] != '*') return 0;
    i++;
    while (i < n && isspace((unsigned char)s[i])) i++;
    if (i + 8 <= n && memcmp(s + i, "restrict", 8) == 0 &&
        (i + 8 == n || isspace((unsigned char)s[i + 8]))) {
        i += 8;
        while (i < n && isspace((unsigned char)s[i])) i++;
    }
    if (i >= n || s[i] != 'c') return 0;
    i++;
    while (i < n && isspace((unsigned char)s[i])) i++;
    if (i >= n || s[i] != ')') return 0;
    i++;
    while (i < n && isspace((unsigned char)s[i])) i++;
    if (i >= n || s[i] != '{') return 0;
    return i + 1;
}

typedef struct {
    size_t start;
    size_t end;
    char va[9];
} FuncSpan;

static FuncSpan *find_functions(const char *s, size_t n, size_t *out_n)
{
    FuncSpan *v = NULL;
    size_t count = 0, cap = 0, i;
    for (i = 0; i < n; i++) {
        char va[9];
        size_t end;
        if (s[i] != 'v') continue;
        end = function_header_at(s, n, i, va);
        if (!end) continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 64;
            v = (FuncSpan *)xrealloc(v, cap * sizeof(*v));
        }
        v[count].start = i;
        v[count].end = end;
        memcpy(v[count].va, va, 9);
        count++;
        i = end - 1;
    }
    *out_n = count;
    return v;
}

static char *mask_code(const char *line, size_t n)
{
    char *out = xstrndup(line, n);
    size_t i = 0;
    while (i < n) {
        if (out[i] == '/' && i + 1 < n && out[i + 1] == '*') {
            size_t end = i + 2;
            while (end + 1 < n && !(out[end] == '*' && out[end + 1] == '/')) end++;
            end = (end + 1 < n) ? end + 2 : n;
            {
                size_t j;
                for (j = i; j < end; j++)
                    if (out[j] != '\n') out[j] = ' ';
            }
            i = end;
        } else if (out[i] == '/' && i + 1 < n && out[i + 1] == '/') {
            size_t end = i + 2;
            while (end < n && out[end] != '\n') end++;
            {
                size_t j;
                for (j = i; j < end; j++) out[j] = ' ';
            }
            i = end;
        } else if (out[i] == '"' || out[i] == '\'') {
            char quote = out[i];
            size_t j = i + 1;
            while (j < n) {
                if (out[j] == '\\') { j += 2; continue; }
                if (out[j] == quote) { j++; break; }
                j++;
            }
            {
                size_t k;
                for (k = i; k < (j < n ? j : n); k++) out[k] = ' ';
            }
            i = j;
        } else {
            i++;
        }
    }
    return out;
}

static void call_free(Call *c)
{
    size_t i;
    free(c->name);
    for (i = 0; i < c->nargs; i++) free(c->args[i]);
    free(c->args);
}

static void callvec_free(CallVec *v)
{
    size_t i;
    for (i = 0; i < v->n; i++) call_free(&v->v[i]);
    free(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

static void callvec_push(CallVec *v, Call c)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->v = (Call *)xrealloc(v->v, v->cap * sizeof(*v->v));
    }
    v->v[v->n++] = c;
}

static char *trim_dup(const char *s, size_t n)
{
    size_t a = 0, b = n;
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return xstrndup(s + a, b - a);
}

static CallVec find_calls(const char *s, size_t n)
{
    CallVec v = {0};
    size_t i = 0;
    while (i < n) {
        size_t start, name_end, open, depth, j;
        Call c;
        if (!(isalpha((unsigned char)s[i]) || s[i] == '_')) { i++; continue; }
        start = i;
        i++;
        while (i < n && is_word((unsigned char)s[i])) i++;
        name_end = i;
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i >= n || s[i] != '(') continue;
        open = i;
        depth = 1;
        j = open + 1;
        while (j < n && depth) {
            if (s[j] == '(') depth++;
            else if (s[j] == ')') depth--;
            j++;
        }
        if (depth) continue;
        c.name = xstrndup(s + start, name_end - start);
        c.start = start;
        c.open = open;
        c.close = j;
        c.args = NULL;
        c.nargs = 0;
        {
            size_t raw_start = open + 1, raw_end = j - 1;
            size_t part = raw_start, k;
            int d = 0;
            for (k = raw_start; k < raw_end; k++) {
                if (s[k] == '(') d++;
                else if (s[k] == ')') d--;
                else if (s[k] == ',' && d == 0) {
                    c.args = (char **)xrealloc(c.args,
                                               (c.nargs + 1) * sizeof(*c.args));
                    c.args[c.nargs++] = trim_dup(s + part, k - part);
                    part = k + 1;
                }
            }
            c.args = (char **)xrealloc(c.args, (c.nargs + 1) * sizeof(*c.args));
            c.args[c.nargs++] = trim_dup(s + part, raw_end - part);
        }
        callvec_push(&v, c);
    }
    return v;
}

static int flag_index(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < 6; i++)
        if (strlen(FLAG_NAMES[i]) == n &&
            memcmp(s, FLAG_NAMES[i], n) == 0)
            return (int)i;
    return -1;
}

static unsigned flag_reads(const char *s, size_t n)
{
    unsigned mask = 0;
    size_t i;
    for (i = 0; i + 3 < n; i++) {
        int idx;
        size_t j;
        if (memcmp(s + i, "c->", 3) != 0) continue;
        if (i > 0 && is_word((unsigned char)s[i - 1])) continue;
        j = i + 3;
        while (j < n && is_word((unsigned char)s[j])) j++;
        idx = flag_index(s + i + 3, j - (i + 3));
        if (idx >= 0) mask |= 1u << idx;
        i = j ? j - 1 : i;
    }
    return mask;
}

static int word_at(const char *s, size_t n, size_t p, const char *w)
{
    size_t wl = strlen(w);
    if (p + wl > n || memcmp(s + p, w, wl) != 0) return 0;
    if (p > 0 && is_word((unsigned char)s[p - 1])) return 0;
    if (p + wl < n && is_word((unsigned char)s[p + wl])) return 0;
    return 1;
}

static int control_word(const char *s, size_t n)
{
    static const char *const words[] = {
        "if", "else", "while", "for", "switch", "goto", "return",
        "break", "continue", "do"
    };
    size_t i, k;
    for (i = 0; i < n; i++)
        for (k = 0; k < sizeof(words) / sizeof(words[0]); k++)
            if (word_at(s, n, i, words[k])) return 1;
    return 0;
}

static int starts_with(const char *s, size_t n, const char *prefix)
{
    size_t pl = strlen(prefix);
    return n >= pl && memcmp(s, prefix, pl) == 0;
}

static int is_neutral_call(const char *name)
{
    static const char *const neutral[] = {
        "rd8", "rd16", "rd32", "rd64", "rdss", "rdsd", "rdxm",
        "wr8", "wr16", "wr32", "wr64", "wrss", "wrsd", "wrxm",
        "push32", "pop32", "GVA", "R8L", "R8H", "R16", "R16L", "R32L",
        "mask_sz", "sign_sz", "parity8",
        "FS_RD32", "FS_WR32",
        "fst", "fpop", "fpush", "x87_ld_f32", "x87_ld_f64",
        "x87_st_f32", "x87_st_f64", "x87_add", "x87_mul",
        "sse_load_ss", "sse_load_sd", "sse_compare", "sse_cvtt_i32",
        "atomic_load32", "atomic_store32", "atomic_xchg32", "atomic_xadd32",
        "atomic_cmpxchg32", "atomic_cmpxchg64",
        "memcpy", "memmove", "memset", "strlen", "sizeof",
        "__builtin_expect", "__builtin_unreachable"
    };
    size_t i;
    for (i = 0; i < sizeof(neutral) / sizeof(neutral[0]); i++)
        if (strcmp(name, neutral[i]) == 0) return 1;
    return 0;
}

static int is_control_name(const char *name)
{
    static const char *const names[] = {
        "if", "else", "while", "for", "switch", "case", "default",
        "goto", "return", "break", "continue", "do"
    };
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(name, names[i]) == 0) return 1;
    return 0;
}

static int is_flag_helper(const char *name)
{
    return strcmp(name, "flags_add") == 0 ||
           strcmp(name, "flags_sub") == 0 ||
           strcmp(name, "flags_adc") == 0 ||
           strcmp(name, "flags_sbb") == 0 ||
           strcmp(name, "flags_logicz") == 0 ||
           strcmp(name, "flags_logic32") == 0 ||
           strcmp(name, "flags_logic8") == 0 ||
           strcmp(name, "flags_incs") == 0 ||
           strcmp(name, "flags_decs") == 0;
}

static unsigned helper_reads(const char *name)
{
    if (strcmp(name, "flags_adc") == 0 || strcmp(name, "flags_sbb") == 0 ||
        strcmp(name, "flags_incs") == 0 || strcmp(name, "flags_decs") == 0)
        return F_CF;
    return 0;
}

static int is_barrier(const char *masked, size_t n, const CallVec *calls)
{
    size_t a = 0, b = n;
    while (a < b && isspace((unsigned char)masked[a])) a++;
    while (b > a && isspace((unsigned char)masked[b - 1])) b--;
    if (a == b) return 0;
    if (masked[a] == '#') return 1;
    if (masked[a] == '{' || masked[a] == '}') return 1;
    {
        size_t i = a;
        if (isalpha((unsigned char)masked[i]) || masked[i] == '_') {
            while (i < b && is_word((unsigned char)masked[i])) i++;
            while (i < b && (masked[i] == ' ' || masked[i] == '\t')) i++;
            if (i < b && masked[i] == ':') return 1;
        }
    }
    {
        static const char *const prefixes[] = {
            "if", "else", "while", "for", "switch", "case", "default",
            "goto", "return", "break", "continue", "do"
        };
        size_t k;
        for (k = 0; k < sizeof(prefixes) / sizeof(prefixes[0]); k++) {
            size_t pl = strlen(prefixes[k]);
            if (b - a == pl && memcmp(masked + a, prefixes[k], pl) == 0)
                return 1;
            if (b - a > pl && memcmp(masked + a, prefixes[k], pl) == 0 &&
                (masked[a + pl] == ' ' || masked[a + pl] == '\t' ||
                 masked[a + pl] == '('))
                return 1;
        }
    }
    if (control_word(masked, n)) return 1;
    {
        size_t i = a;
        if (isalpha((unsigned char)masked[i]) || masked[i] == '_') {
            while (i < b && is_word((unsigned char)masked[i])) i++;
            {
                size_t j = i;
                while (j < b && (masked[j] == ' ' || masked[j] == '\t')) j++;
                if (j < b && masked[j] == '=' && (j + 1 >= b || masked[j + 1] != '=')) {
                    unsigned reads = flag_reads(masked + a, i - a);
                    if (reads) return 1;
                }
            }
        }
    }
    if (strstr(masked, "WC3_DIRECT") || strstr(masked, "WC3_GUEST_DRAIN"))
        return 1;
    if (strstr(masked, "dispatch") || strstr(masked, "dispatch_jmp"))
        return 1;
    if (strstr(masked, "__asm") || strstr(masked, "asm volatile"))
        return 1;
    {
        size_t i;
        for (i = 0; i < calls->n; i++) {
            const char *name = calls->v[i].name;
            if (is_flag_helper(name) || is_neutral_call(name) ||
                is_control_name(name))
                continue;
            if (starts_with(name, strlen(name), "dispatch") ||
                starts_with(name, strlen(name), "WC3_DIRECT_CALL") ||
                starts_with(name, strlen(name), "WC3_DIRECT_TAIL") ||
                starts_with(name, strlen(name), "WC3_GUEST_DRAIN") ||
                starts_with(name, strlen(name), "hle_") ||
                starts_with(name, strlen(name), "L_") ||
                starts_with(name, strlen(name), "RECOMP_TODO"))
                return 1;
            return 1;
        }
    }
    return 0;
}

static int parse_size(const char *s, size_t n, int *out)
{
    char *t = trim_dup(s, n);
    size_t l = strlen(t);
    char *end = NULL;
    long v;
    while (l && (t[l - 1] == 'u' || t[l - 1] == 'U' ||
                 t[l - 1] == 'l' || t[l - 1] == 'L'))
        t[--l] = 0;
    v = strtol(t, &end, 0);
    if (!end || *end) {
        free(t);
        return 0;
    }
    free(t);
    if (v != 1 && v != 2 && v != 4) return 0;
    *out = (int)v;
    return 1;
}

static char *mask_expr(const char *expr, int size)
{
    Buf b = {0};
    if (size == 4) {
        buf_printf(&b, "(uint32_t)(%s)", expr);
    } else {
        unsigned bits = (unsigned)size * 8;
        unsigned mask = (1u << bits) - 1u;
        buf_printf(&b, "(uint32_t)((%s) & 0x%0*Xu)", expr,
                   (int)(bits / 4), mask);
    }
    return b.p;
}

static int make_replacement(const Call *call, char **out, unsigned *flags)
{
    const char *name = call->name;
    *out = NULL;
    *flags = 0;
    if (strcmp(name, "flags_add") == 0 || strcmp(name, "flags_sub") == 0 ||
        strcmp(name, "flags_adc") == 0 || strcmp(name, "flags_sbb") == 0) {
        int sz;
        Buf expr = {0};
        if (call->nargs != 4) return 0;
        if (!parse_size(call->args[3], strlen(call->args[3]), &sz)) return 0;
        if (strcmp(name, "flags_add") == 0)
            buf_printf(&expr, "(%s) + (%s)", call->args[1], call->args[2]);
        else if (strcmp(name, "flags_sub") == 0)
            buf_printf(&expr, "(%s) - (%s)", call->args[1], call->args[2]);
        else if (strcmp(name, "flags_adc") == 0)
            buf_printf(&expr, "(%s) + (%s) + (c->cf & 1u)",
                       call->args[1], call->args[2]);
        else
            buf_printf(&expr, "(%s) - (%s) - (c->cf & 1u)",
                       call->args[1], call->args[2]);
        *out = mask_expr(expr.p, sz);
        free(expr.p);
        *flags = ALL_FLAGS;
        return 1;
    }
    if (strcmp(name, "flags_logicz") == 0) {
        int sz;
        Buf expr = {0};
        if (call->nargs != 3) return 0;
        if (!parse_size(call->args[2], strlen(call->args[2]), &sz)) return 0;
        buf_printf(&expr, "(%s)", call->args[1]);
        *out = mask_expr(expr.p, sz);
        free(expr.p);
        *flags = LOGIC_FLAGS;
        return 1;
    }
    if (strcmp(name, "flags_incs") == 0 || strcmp(name, "flags_decs") == 0) {
        int sz;
        Buf expr = {0};
        if (call->nargs != 3) return 0;
        if (!parse_size(call->args[2], strlen(call->args[2]), &sz)) return 0;
        buf_printf(&expr, "(%s) %s 1u", call->args[1],
                   strcmp(name, "flags_incs") == 0 ? "+" : "-");
        *out = mask_expr(expr.p, sz);
        free(expr.p);
        *flags = INC_FLAGS;
        return 1;
    }
    if (strcmp(name, "flags_logic32") == 0 || strcmp(name, "flags_logic8") == 0) {
        Buf b = {0};
        if (call->nargs != 2) return 0;
        buf_printf(&b, "(void)(%s)", call->args[1]);
        *out = b.p;
        *flags = LOGIC_FLAGS;
        return 1;
    }
    return 0;
}

static LineVec split_lines(const char *s, size_t n)
{
    LineVec v = {0};
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && s[j] != '\n') j++;
        if (j < n) j++;
        if (v.n == v.cap) {
            v.cap = v.cap ? v.cap * 2 : 64;
            v.v = (Line *)xrealloc(v.v, v.cap * sizeof(*v.v));
        }
        v.v[v.n].text = xstrndup(s + i, j - i);
        v.v[v.n].len = j - i;
        v.n++;
        i = j;
    }
    return v;
}

static void linevec_free(LineVec *v)
{
    size_t i;
    for (i = 0; i < v->n; i++) free(v->v[i].text);
    free(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

static int trim_is_brace(const char *s, size_t n)
{
    size_t a = 0, b = n;
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' ||
                     s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return b - a == 1 && s[a] == '}';
}

static void report_add_error(FileReport *r, const char *fmt, ...)
{
    Buf b = {0};
    va_list ap;
    va_start(ap, fmt);
    {
        int n = vsnprintf(NULL, 0, fmt, ap);
        va_end(ap);
        if (n < 0) die("formatting failed");
        buf_need(&b, (size_t)n);
        va_start(ap, fmt);
        vsnprintf(b.p + b.n, b.cap - b.n, fmt, ap);
        va_end(ap);
        b.n += (size_t)n;
    }
    if (r->errors.n == r->errors.cap) {
        r->errors.cap = r->errors.cap ? r->errors.cap * 2 : 4;
        r->errors.v = (char **)xrealloc(r->errors.v,
                                        r->errors.cap * sizeof(*r->errors.v));
    }
    r->errors.v[r->errors.n++] = b.p;
}

static char *process_body(const char *body, size_t n, const char *va,
                          FileReport *report)
{
    LineVec lines = split_lines(body, n);
    WriteRecord *records = NULL;
    size_t nrecords = 0, rec_cap = 0;
    int pending[6] = {-1, -1, -1, -1, -1, -1};
    size_t *dead = NULL;
    size_t ndead = 0, dead_cap = 0;
    size_t li;

    for (li = 0; li < lines.n; li++) {
        char *masked = mask_code(lines.v[li].text, lines.v[li].len);
        CallVec calls = find_calls(masked, strlen(masked));
        CallVec helpers = {0};
        size_t ci;
        unsigned reads;
        int same_line_read;
        for (ci = 0; ci < calls.n; ci++)
            if (is_flag_helper(calls.v[ci].name))
                callvec_push(&helpers, calls.v[ci]);
        /* The shallow copy above is intentional: helpers points into calls.
         * Do not free helper arguments separately. */
        if (is_barrier(masked, strlen(masked), &calls)) {
            for (ci = 0; ci < 6; ci++)
                if (pending[ci] >= 0) {
                    records[pending[ci]].live = 1;
                    pending[ci] = -1;
                }
            if (trim_is_brace(lines.v[li].text, lines.v[li].len)) {
                free(masked);
                free(helpers.v);
                callvec_free(&calls);
                break;
            }
            free(masked);
            free(helpers.v);
            callvec_free(&calls);
            continue;
        }
        if (helpers.n > 1) {
            for (ci = 0; ci < 6; ci++)
                if (pending[ci] >= 0) {
                    records[pending[ci]].live = 1;
                    pending[ci] = -1;
                }
            free(masked);
            free(helpers.v);
            callvec_free(&calls);
            continue;
        }
        reads = flag_reads(masked, strlen(masked));
        same_line_read = reads != 0;
        for (ci = 0; ci < 6; ci++)
            if ((reads & (1u << ci)) && pending[ci] >= 0) {
                size_t ri = (size_t)pending[ci];
                unsigned fi;
                records[ri].live = 1;
                for (fi = 0; fi < 6; fi++)
                    if ((records[ri].flags & (1u << fi)) &&
                        pending[fi] == (int)ri)
                        pending[fi] = -1;
                records[ri].flags = 0;
            }
        if (!helpers.n) {
            free(masked);
            free(helpers.v);
            callvec_free(&calls);
            continue;
        }
        {
            Call *call = &helpers.v[0];
            char *repl = NULL;
            unsigned wflags = 0;
            if (!make_replacement(call, &repl, &wflags)) {
                for (ci = 0; ci < 6; ci++)
                    if (pending[ci] >= 0) {
                        records[pending[ci]].live = 1;
                        pending[ci] = -1;
                    }
                free(masked);
                free(helpers.v);
                callvec_free(&calls);
                continue;
            }
            report->flag_writes++;
            {
                unsigned rr = helper_reads(call->name);
                for (ci = 0; ci < 6; ci++)
                    if ((rr & (1u << ci)) && pending[ci] >= 0) {
                        size_t ri = (size_t)pending[ci];
                        unsigned fi;
                        records[ri].live = 1;
                        for (fi = 0; fi < 6; fi++)
                            if ((records[ri].flags & (1u << fi)) &&
                                pending[fi] == (int)ri)
                                pending[fi] = -1;
                        records[ri].flags = 0;
                    }
            }
            if (nrecords == rec_cap) {
                rec_cap = rec_cap ? rec_cap * 2 : 64;
                records = (WriteRecord *)xrealloc(records,
                                                  rec_cap * sizeof(*records));
            }
            {
                size_t ri = nrecords++;
                size_t k;
                records[ri].line = li;
                records[ri].start = call->start;
                records[ri].end = call->close;
                records[ri].source_text = xstrndup(lines.v[li].text + call->start,
                                                    call->close - call->start);
                records[ri].repl_text = repl;
                records[ri].flags = wflags;
                records[ri].field_count = 0;
                for (k = 0; k < 6; k++)
                    if (wflags & (1u << k)) records[ri].field_count++;
                records[ri].live = same_line_read;
                for (k = 0; k < 6; k++) {
                    if (!(wflags & (1u << k))) continue;
                    if (pending[k] >= 0) {
                        size_t oi = (size_t)pending[k];
                        records[oi].flags &= ~(1u << k);
                        if (!records[oi].flags && !records[oi].live) {
                            if (ndead == dead_cap) {
                                dead_cap = dead_cap ? dead_cap * 2 : 8;
                                dead = (size_t *)xrealloc(dead,
                                                          dead_cap * sizeof(*dead));
                            }
                            dead[ndead++] = oi;
                        }
                        if (!records[oi].flags) {
                            size_t q;
                            for (q = 0; q < 6; q++)
                                if (pending[q] == (int)oi) pending[q] = -1;
                        }
                    }
                    if (!same_line_read) pending[k] = (int)ri;
                }
            }
            free(masked);
            free(helpers.v);
            callvec_free(&calls);
        }
    }
    for (li = 0; li < 6; li++)
        if (pending[li] >= 0) records[pending[li]].live = 1;

    /* Apply replacements from the end of each line.  Only one helper per line
     * is accepted above, so spans cannot overlap. */
    {
        size_t i, j;
        for (i = 0; i < ndead; i++)
            for (j = i + 1; j < ndead; j++) {
                WriteRecord *a = &records[dead[i]];
                WriteRecord *b = &records[dead[j]];
                if (a->line > b->line ||
                    (a->line == b->line && a->start < b->start)) {
                    size_t t = dead[i];
                    dead[i] = dead[j];
                    dead[j] = t;
                }
            }
    }
    for (li = 0; li < ndead; li++) {
        WriteRecord *r = &records[dead[li]];
        Line *line = &lines.v[r->line];
        if (r->end > line->len ||
            memcmp(line->text + r->start, r->source_text,
                   r->end - r->start) != 0) {
            report_add_error(report, "L_%s: replacement span mismatch on line %zu",
                             va, r->line + 1);
            continue;
        }
        {
            Buf nb = {0};
            buf_add(&nb, line->text, r->start);
            buf_str(&nb, r->repl_text);
            buf_add(&nb, line->text + r->end, line->len - r->end);
            free(line->text);
            line->text = nb.p;
            line->len = nb.n;
        }
        report->dead_writes++;
        report->dead_flag_fields += r->field_count;
    }
    if (ndead) report->changed_functions++;

    {
        Buf out = {0};
        size_t i;
        for (i = 0; i < lines.n; i++) buf_add(&out, lines.v[i].text, lines.v[i].len);
        linevec_free(&lines);
        free(dead);
        for (i = 0; i < nrecords; i++) {
            free(records[i].source_text);
            free(records[i].repl_text);
        }
        free(records);
        return out.p;
    }
}

static void body_sha(const char *body, size_t n, char out[65])
{
    unsigned char digest[32];
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, body, n);
    sha256_final(&ctx, digest);
    sha256_hex(digest, out);
}

static char *transform_text(const char *text, size_t n, FileReport *report)
{
    FuncSpan *funcs;
    size_t nfuncs, i;
    Buf out = {0};
    size_t cursor = 0;
    char **before = NULL, **after = NULL;
    funcs = find_functions(text, n, &nfuncs);
    if (!nfuncs) {
        report_add_error(report, "no lifted functions found");
        free(funcs);
        return xstrndup(text, n);
    }
    report->functions = nfuncs;
    before = (char **)xrealloc(NULL, nfuncs * sizeof(*before));
    after = (char **)xrealloc(NULL, nfuncs * sizeof(*after));
    for (i = 0; i < nfuncs; i++) {
        size_t start = funcs[i].end;
        size_t end = i + 1 < nfuncs ? funcs[i + 1].start : n;
        char *body = xstrndup(text + start, end - start);
        char *new_body = process_body(body, end - start, funcs[i].va, report);
        before[i] = body;
        after[i] = new_body;
        buf_add(&out, text + cursor, start - cursor);
        buf_add(&out, new_body, strlen(new_body));
        cursor = end;
    }
    buf_add(&out, text + cursor, n - cursor);
    for (i = 0; i < nfuncs; i++) {
        if (strcmp(before[i], after[i]) != 0) {
            char h1[65], h2[65];
            Buf cs = {0};
            body_sha(before[i], strlen(before[i]), h1);
            body_sha(after[i], strlen(after[i]), h2);
            buf_printf(&cs,
                       "{\"va\": \"%s\", \"pass_id\": \"dead_flags\", "
                       "\"before_sha256\": \"%s\", \"after_sha256\": \"%s\", "
                       "\"reason\": \"dead flag write elided in a straight-line "
                       "basic block\"}",
                       funcs[i].va, h1, h2);
            if (report->change_set.n == report->change_set.cap) {
                report->change_set.cap = report->change_set.cap
                                             ? report->change_set.cap * 2
                                             : 4;
                report->change_set.v = (char **)xrealloc(
                    report->change_set.v,
                    report->change_set.cap * sizeof(*report->change_set.v));
            }
            report->change_set.v[report->change_set.n++] = cs.p;
        }
    }
    for (i = 0; i < nfuncs; i++) {
        free(before[i]);
        free(after[i]);
    }
    free(before);
    free(after);
    free(funcs);
    return out.p;
}

static int helper_regex(const char *s)
{
    return strstr(s, "flags_add(") || strstr(s, "flags_sub(") ||
           strstr(s, "flags_adc(") || strstr(s, "flags_sbb(") ||
           strstr(s, "flags_logicz(") || strstr(s, "flags_logic32(") ||
           strstr(s, "flags_logic8(") || strstr(s, "flags_incs(") ||
           strstr(s, "flags_decs(");
}

static int verify(const char *before, size_t bn, const char *after, size_t an,
                  FileReport *report)
{
    FuncSpan *b, *a;
    size_t nb, na, i;
    if (report->errors.n) return 0;
    b = find_functions(before, bn, &nb);
    a = find_functions(after, an, &na);
    if (nb != na) {
        report_add_error(report, "function count changed");
        free(b);
        free(a);
        return 0;
    }
    for (i = 0; i < nb; i++) {
        size_t bs = b[i].end;
        size_t be = i + 1 < nb ? b[i + 1].start : bn;
        size_t as = a[i].end;
        size_t ae = i + 1 < na ? a[i + 1].start : an;
        if (strcmp(b[i].va, a[i].va) != 0) {
            report_add_error(report, "function order changed: %s -> %s",
                             b[i].va, a[i].va);
            free(b);
            free(a);
            return 0;
        }
        if (be - bs == ae - as && memcmp(before + bs, after + as, be - bs) == 0)
            continue;
        {
            LineVec bl = split_lines(before + bs, be - bs);
            LineVec al = split_lines(after + as, ae - as);
            size_t k;
            if (bl.n != al.n) {
                report_add_error(report, "L_%s: line count changed", b[i].va);
                linevec_free(&bl);
                linevec_free(&al);
                free(b);
                free(a);
                return 0;
            }
            for (k = 0; k < bl.n; k++) {
                char *mx, *my;
                if (bl.v[k].len == al.v[k].len &&
                    memcmp(bl.v[k].text, al.v[k].text, bl.v[k].len) == 0)
                    continue;
                mx = mask_code(bl.v[k].text, bl.v[k].len);
                my = mask_code(al.v[k].text, al.v[k].len);
                if (!helper_regex(mx)) {
                    report_add_error(report,
                                     "L_%s:%zu: changed line has no flag helper",
                                     b[i].va, k + 1);
                    free(mx);
                    free(my);
                    linevec_free(&bl);
                    linevec_free(&al);
                    free(b);
                    free(a);
                    return 0;
                }
                if (strstr(my, "flags_")) {
                    report_add_error(report,
                                     "L_%s:%zu: flag helper survived rewrite",
                                     b[i].va, k + 1);
                    free(mx);
                    free(my);
                    linevec_free(&bl);
                    linevec_free(&al);
                    free(b);
                    free(a);
                    return 0;
                }
                free(mx);
                free(my);
            }
            linevec_free(&bl);
            linevec_free(&al);
        }
    }
    free(b);
    free(a);
    return 1;
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

static int write_report(const char *path, FileReport **reports, size_t nreports)
{
    FILE *f;
    size_t i, j;
    size_t funcs = 0, writes = 0, dead = 0, fields = 0, changed = 0, errors = 0;
    if (!path) return 0;
    mkdir_parent(path);
    f = fopen(path, "wb");
    if (!f) return -1;
    for (i = 0; i < nreports; i++) {
        funcs += reports[i]->functions;
        writes += reports[i]->flag_writes;
        dead += reports[i]->dead_writes;
        fields += reports[i]->dead_flag_fields;
        changed += reports[i]->changed_functions;
        errors += reports[i]->errors.n;
    }
    fputs("{\n  \"pass\": \"dead_flags\",\n  \"files\": [\n", f);
    for (i = 0; i < nreports; i++) {
        FileReport *r = reports[i];
        fputs("    {\n      \"lift_file\": ", f);
        write_json_string(f, r->lift_file);
        fputs(",\n      \"output\": ", f);
        if (r->output) write_json_string(f, r->output);
        else fputs("null", f);
        fprintf(f, ",\n      \"functions\": %zu,\n      \"flag_writes\": %zu,\n"
                   "      \"dead_writes\": %zu,\n      \"dead_flag_fields\": %zu,\n"
                   "      \"changed_functions\": %zu,\n      \"change_set\": [",
                r->functions, r->flag_writes, r->dead_writes,
                r->dead_flag_fields, r->changed_functions);
        for (j = 0; j < r->change_set.n; j++) {
            fputs("\n        ", f);
            fputs(r->change_set.v[j], f);
            if (j + 1 != r->change_set.n) fputc(',', f);
        }
        fputs("\n      ],\n      \"errors\": [", f);
        for (j = 0; j < r->errors.n; j++) {
            fputs("\n        ", f);
            write_json_string(f, r->errors.v[j]);
            if (j + 1 != r->errors.n) fputc(',', f);
        }
        fputs("\n      ]\n    }", f);
        if (i + 1 != nreports) fputc(',', f);
        fputc('\n', f);
    }
    fprintf(f,
            "  ],\n  \"totals\": {\n"
            "    \"functions\": %zu,\n    \"flag_writes\": %zu,\n"
            "    \"dead_writes\": %zu,\n    \"dead_flag_fields\": %zu,\n"
            "    \"changed_functions\": %zu,\n    \"errors\": %zu\n"
            "  }\n}\n",
            funcs, writes, dead, fields, changed, errors);
    return fclose(f) == 0 ? 0 : -1;
}

static const char SELF_TEST[] =
    "/* auto-generated fixture */\n"
    "void L_00000001(CPU *restrict c)\n"
    "{\n"
    "    c->eax = flags_add(c, c->eax, 1u, 4);       /* 1: add eax, 1 */\n"
    "    c->edx = flags_add(c, c->edx, 2u, 4);       /* 2: add edx, 2 */\n"
    "    c->ecx = flags_logicz(c, c->ecx ^ c->edx, 4); /* 3: xor ecx, edx */\n"
    "    if (c->zf) { dispatch_jmp(c, 0x00000002u); return; } /* 4: je */\n"
    "    c->ebx = flags_add(c, c->ebx, 3u, 4);       /* 5: read below */\n"
    "    c->esi = flags_add(c, c->esi, 4u, 4);       /* 6: dead */\n"
    "    c->edi = flags_sub(c, c->edi, 5u, 4);       /* 7: dead before inc? */\n"
    "    c->ebp = flags_incs(c, c->ebp, 4);          /* 8: reads old cf */\n"
    "    c->eax = flags_add(c, c->eax, 6u, 4);       /* 9: live, checked below */\n"
    "    if (c->cf) { return; }                       /* 10: read */\n"
    "    flags_logic32(c, c->eax);                    /* 11: void, dead before add */\n"
    "    c->edx = flags_add(c, c->edx, 7u, 4);       /* 12: kill 11 */\n"
    "    c->ecx = rd32(c->esp);                       /* 13: neutral */\n"
    "    c->ebx = flags_add(c, c->ebx, 8u, 4);       /* 14: dead before ret? ret barrier keeps */\n"
    "    return;\n"
    "}\n"
    "void L_00000002(CPU *c)\n"
    "{\n"
    "    c->eax = flags_add(c, c->eax, 1u, 4);\n"
    "    unknown_hle(c);\n"
    "    c->edx = flags_add(c, c->edx, 2u, 4);\n"
    "    return;\n"
    "}\n";

static int self_test(void)
{
    FileReport report;
    char *out;
    static const char *const expected[] = {
        "c->eax = (uint32_t)((c->eax) + (1u));",
        "c->edx = flags_add(c, c->edx, 2u, 4);",
        "flags_logicz(c, c->ecx ^ c->edx, 4)",
        "flags_sub(c, c->edi, 5u, 4)",
        "c->ebp = (uint32_t)((c->ebp) + 1u);",
        "flags_add(c, c->eax, 6u, 4)",
        "flags_add(c, c->ebx, 8u, 4)",
        "unknown_hle(c)",
    };
    size_t i;
    memset(&report, 0, sizeof(report));
    report.lift_file = xstrdup("self-test");
    out = transform_text(SELF_TEST, sizeof(SELF_TEST) - 1, &report);
    if (!verify(SELF_TEST, sizeof(SELF_TEST) - 1, out, strlen(out), &report)) {
        fputs("dead_flags self-test: verification failed\n", stderr);
        for (i = 0; i < report.errors.n; i++)
            fprintf(stderr, "  %s\n", report.errors.v[i]);
        return 1;
    }
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
        if (!strstr(out, expected[i])) {
            fprintf(stderr, "dead_flags self-test: missing %s\n", expected[i]);
            return 1;
        }
    if (strstr(out, "flags_logic32")) {
        fputs("dead_flags self-test: void logic not elided\n", stderr);
        return 1;
    }
    if (!strstr(out, "(void)(c->eax)")) {
        fputs("dead_flags self-test: logic value not preserved\n", stderr);
        return 1;
    }
    if (report.dead_writes != 6) {
        fprintf(stderr, "dead_flags self-test: dead_writes=%zu, want 6\n",
                report.dead_writes);
        return 1;
    }
    puts("dead_flags self-test: OK");
    free(out);
    return 0;
}

static void report_free(FileReport *r)
{
    size_t i;
    free(r->lift_file);
    free(r->output);
    for (i = 0; i < r->change_set.n; i++) free(r->change_set.v[i]);
    free(r->change_set.v);
    for (i = 0; i < r->errors.n; i++) free(r->errors.v[i]);
    free(r->errors.v);
    memset(r, 0, sizeof(*r));
}

static int process_file(const char *src, const char *dst, FileReport *report)
{
    char *text;
    size_t n;
    char *out;
    text = slurp(src, &n);
    if (!text) {
        fprintf(stderr, "dead_flags: cannot read %s\n", src);
        return 2;
    }
    report->lift_file = xstrdup(src);
    out = transform_text(text, n, report);
    if (!verify(text, n, out, strlen(out), report)) {
        free(text);
        free(out);
        return 1;
    }
    mkdir_parent(dst);
    if (write_all(dst, out, strlen(out)) != 0) {
        fprintf(stderr, "dead_flags: cannot write %s\n", dst);
        free(text);
        free(out);
        return 2;
    }
    report->output = xstrdup(dst);
    free(text);
    free(out);
    return 0;
}

static void usage(void)
{
    fputs("usage: dead_flags --lift IN --out OUT [--json REPORT] [--quiet]\n"
          "       dead_flags --in-dir DIR --out-dir DIR [--json REPORT] [--quiet]\n"
          "       dead_flags --self-test\n", stderr);
}

int main(int argc, char **argv)
{
    const char *lift = NULL, *out = NULL, *json = NULL;
    const char *in_dir = NULL, *out_dir = NULL;
    int quiet = 0, i, rc = 0;
    FileReport **reports = NULL;
    size_t nreports = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--self-test")) return self_test();
        else if (!strcmp(argv[i], "--lift") && i + 1 < argc) lift = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) json = argv[++i];
        else if (!strcmp(argv[i], "--in-dir") && i + 1 < argc) in_dir = argv[++i];
        else if (!strcmp(argv[i], "--out-dir") && i + 1 < argc) out_dir = argv[++i];
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else { usage(); return 2; }
    }
    if (!lift && !in_dir) { usage(); return 2; }
    if (lift && (!out || in_dir)) { usage(); return 2; }
    if (in_dir && (!out_dir || lift)) { usage(); return 2; }
    if (lift) {
        FileReport *r = (FileReport *)calloc(1, sizeof(*r));
        if (!r) die_oom();
        rc = process_file(lift, out, r);
        reports = (FileReport **)xrealloc(NULL, sizeof(*reports));
        reports[nreports++] = r;
    } else {
        /* Minimal directory mode: process every *.c in name order. */
        fprintf(stderr, "dead_flags: --in-dir mode is not used by the build; "
                        "use --lift for a chunk\n");
        return 2;
    }
    if (write_report(json, reports, nreports) != 0) {
        fprintf(stderr, "dead_flags: cannot write report %s\n", json);
        rc = 2;
    }
    if (!quiet && reports[0]) {
        FileReport *r = reports[0];
        printf("dead_flags: functions=%zu writes=%zu dead=%zu fields=%zu "
               "changed=%zu errors=%zu\n", r->functions, r->flag_writes,
               r->dead_writes, r->dead_flag_fields, r->changed_functions,
               r->errors.n);
    }
    {
        size_t k;
        for (k = 0; k < nreports; k++) {
            size_t e;
            for (e = 0; e < reports[k]->errors.n; e++)
                fprintf(stderr, "%s: %s\n", reports[k]->lift_file,
                        reports[k]->errors.v[e]);
            report_free(reports[k]);
            free(reports[k]);
        }
        free(reports);
    }
    return rc;
}
