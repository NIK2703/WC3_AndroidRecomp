/* devirtualize.c - P0-1 direct-call LiftPass.
 *
 * This C pass is byte-identical to the retired Python reference on the full
 * corpus and is wired into build_tools.ps1 and Wc3ReliftedCorpus.cmake.
 * It is deliberately a separate tool so an exactness A/B can disable it
 * without editing the build.
 *
 *   devirtualize --list boot_dispatch_list.h --lift chunk.c --out chunk.c
 *   devirtualize --self-test
 *
 * The transformation is the P0-1 contract:
 *
 *   dispatch(c, 0xVA)              -> WC3_DIRECT_CALL(L_VA, c)
 *   dispatch_jmp(c, 0xVA); return; -> WC3_DIRECT_TAIL(L_VA, c); return;
 *
 * Only matches inside a lifted function body and outside comments, strings and
 * character literals are touched.  HLE-window targets and targets absent from
 * the active corpus list are left alone.  The output also gets the
 * direct_call.h include and prototypes for every converted target.
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HLE_LO 0x7F000000u
#define HLE_HI 0x7F100000u

typedef struct {
    char *p;
    size_t n;
    size_t cap;
} Buf;

typedef struct {
    uint32_t *v;
    size_t n;
    size_t cap;
} VaSet;

typedef struct {
    size_t start;
    size_t body;
} Func;

typedef struct {
    Func *v;
    size_t n;
    size_t cap;
} FuncVec;

typedef struct {
    uint32_t *v;
    size_t n;
    size_t cap;
} TargetVec;

static void die(const char *msg)
{
    fprintf(stderr, "devirtualize: %s\n", msg);
    exit(2);
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory");
    return q;
}

static char *slurp(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    size_t got;

    if (!f) {
        fprintf(stderr, "devirtualize: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *)xrealloc(NULL, (size_t)sz + 1);
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return NULL;
    }
    buf[got] = 0;
    *out_n = got;
    return buf;
}

static int write_file(const char *path, const char *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    size_t put;
    if (!f) {
        fprintf(stderr, "devirtualize: cannot create %s\n", path);
        return -1;
    }
    put = fwrite(data, 1, n, f);
    if (fclose(f) != 0 || put != n) {
        fprintf(stderr, "devirtualize: short write to %s\n", path);
        return -1;
    }
    return 0;
}

static void buf_need(Buf *b, size_t extra)
{
    size_t need = b->n + extra;
    size_t cap;
    if (need <= b->cap) return;
    cap = b->cap ? b->cap : 4096;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) die("output too large");
        cap *= 2;
    }
    b->p = (char *)xrealloc(b->p, cap);
    b->cap = cap;
}

static void buf_add(Buf *b, const char *s, size_t n)
{
    buf_need(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void buf_str(Buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

static void buf_ch(Buf *b, char c)
{
    buf_need(b, 1);
    b->p[b->n++] = c;
}

static void buf_free(Buf *b)
{
    free(b->p);
    memset(b, 0, sizeof(*b));
}

static int is_word(int c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static int is_hex(int c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

static int parse_hex8(const char *s, size_t n, size_t pos, uint32_t *out,
                      size_t *end)
{
    uint32_t v = 0;
    size_t i;
    if (pos + 8 > n) return 0;
    for (i = 0; i < 8; i++) {
        if (!is_hex((unsigned char)s[pos + i])) return 0;
        v = (v << 4) | (uint32_t)hexval((unsigned char)s[pos + i]);
    }
    *out = v;
    *end = pos + 8;
    return 1;
}

static void va_add(VaSet *s, uint32_t v)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 1024;
        s->v = (uint32_t *)xrealloc(s->v, s->cap * sizeof(*s->v));
    }
    s->v[s->n++] = v;
}

static int u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void va_sort_unique(VaSet *s)
{
    size_t i, j;
    if (!s->n) return;
    qsort(s->v, s->n, sizeof(*s->v), u32_cmp);
    j = 1;
    for (i = 1; i < s->n; i++)
        if (s->v[i] != s->v[j - 1]) s->v[j++] = s->v[i];
    s->n = j;
}

static int va_has(const VaSet *s, uint32_t v)
{
    size_t lo = 0, hi = s->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s->v[mid] == v) return 1;
        if (s->v[mid] < v) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

/* Python Path.read_text() uses universal newlines: CRLF and lone CR become
 * LF.  The generated corpus is written by Python with newline="", so matching
 * the reference requires the same normalisation before the transform. */
static char *normalize_newlines(const char *s, size_t n, size_t *out_n)
{
    Buf b = {0};
    size_t i = 0;
    while (i < n) {
        if (s[i] == '\r') {
            if (i + 1 < n && s[i + 1] == '\n') i++;
            buf_ch(&b, '\n');
        } else {
            buf_ch(&b, s[i]);
        }
        i++;
    }
    *out_n = b.n;
    return b.p;
}

static VaSet parse_va_list(const char *text, size_t n)
{
    VaSet s = {0};
    size_t i = 0;
    while (i < n) {
        if (text[i] == 'X' && i + 2 < n && text[i + 1] == '(') {
            uint32_t v;
            size_t e;
            if (parse_hex8(text, n, i + 2, &v, &e) && e < n && text[e] == ')') {
                va_add(&s, v);
                i = e + 1;
                continue;
            }
        }
        if (text[i] == '{') {
            size_t p = i + 1;
            while (p < n && isspace((unsigned char)text[p])) p++;
            if (p + 2 < n && text[p] == '0' && text[p + 1] == 'x') {
                uint32_t v;
                size_t e;
                if (parse_hex8(text, n, p + 2, &v, &e) &&
                    e < n && (text[e] == 'u' || text[e] == 'U')) {
                    size_t q = e + 1;
                    while (q < n && isspace((unsigned char)text[q])) q++;
                    if (q < n && text[q] == ',') {
                        q++;
                        while (q < n && isspace((unsigned char)text[q])) q++;
                        if (q + 2 <= n && text[q] == 'L' && text[q + 1] == '_') {
                            va_add(&s, v);
                            i = q + 2;
                            continue;
                        }
                    }
                }
            }
        }
        i++;
    }
    va_sort_unique(&s);
    return s;
}

static int skip_ws(const char *s, size_t n, size_t *pos)
{
    size_t p = *pos;
    while (p < n && isspace((unsigned char)s[p])) p++;
    *pos = p;
    return p < n;
}

static int expect_bytes(const char *s, size_t n, size_t *pos,
                        const char *lit)
{
    size_t len = strlen(lit);
    if (*pos + len > n || memcmp(s + *pos, lit, len) != 0) return 0;
    *pos += len;
    return 1;
}

/* Matches ^void\s+L_([0-9A-Fa-f]{8})\s*\(\s*CPU\s*\*\s*c\s*\)\s*\{ */
static int match_func_start(const char *s, size_t n, size_t pos,
                            size_t *body, uint32_t *va)
{
    size_t p = pos;
    uint32_t v;
    size_t e;

    if (p + 4 > n || memcmp(s + p, "void", 4) != 0) return 0;
    p += 4;
    if (!skip_ws(s, n, &p)) return 0;
    if (!expect_bytes(s, n, &p, "L_")) return 0;
    if (!parse_hex8(s, n, p, &v, &e)) return 0;
    p = e;
    if (!skip_ws(s, n, &p)) return 0;
    if (!expect_bytes(s, n, &p, "(")) return 0;
    if (!skip_ws(s, n, &p)) return 0;
    if (!expect_bytes(s, n, &p, "CPU")) return 0;
    if (!skip_ws(s, n, &p)) return 0;
    if (!expect_bytes(s, n, &p, "*")) return 0;
    if (!skip_ws(s, n, &p)) return 0;
    if (!expect_bytes(s, n, &p, "c")) return 0;
    if (!skip_ws(s, n, &p)) return 0;
    if (!expect_bytes(s, n, &p, ")")) return 0;
    if (!skip_ws(s, n, &p)) return 0;
    if (!expect_bytes(s, n, &p, "{")) return 0;
    *body = p;
    *va = v;
    return 1;
}

static FuncVec find_functions(const char *s, size_t n)
{
    FuncVec f = {0};
    size_t i = 0;
    while (i < n) {
        size_t body;
        uint32_t va;
        if ((i == 0 || s[i - 1] == '\n') &&
            match_func_start(s, n, i, &body, &va)) {
            if (f.n == f.cap) {
                f.cap = f.cap ? f.cap * 2 : 1024;
                f.v = (Func *)xrealloc(f.v, f.cap * sizeof(*f.v));
            }
            f.v[f.n].start = i;
            f.v[f.n].body = body;
            f.n++;
        }
        while (i < n && s[i] != '\n') i++;
        if (i < n) i++;
    }
    return f;
}

static unsigned char *code_mask(const char *s, size_t n, const FuncVec *f)
{
    unsigned char *m = (unsigned char *)calloc(n ? n : 1, 1);
    size_t i;
    if (!m) die("out of memory");
    for (i = 0; i < f->n; i++) {
        size_t end = (i + 1 < f->n) ? f->v[i + 1].start : n;
        if (f->v[i].body < end)
            memset(m + f->v[i].body, 1, end - f->v[i].body);
    }
    i = 0;
    while (i < n) {
        if (!m[i]) {
            i++;
            continue;
        }
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            size_t e = i + 2;
            while (e + 1 < n && !(s[e] == '*' && s[e + 1] == '/')) e++;
            e = (e + 1 < n) ? e + 2 : n;
            memset(m + i, 0, e - i);
            i = e;
        } else if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
            size_t e = i + 2;
            while (e < n && s[e] != '\n') e++;
            memset(m + i, 0, e - i);
            i = e;
        } else if (s[i] == '"' || s[i] == '\'') {
            char q = s[i];
            size_t e = i + 1;
            while (e < n) {
                if (s[e] == '\\') {
                    e += 2;
                    continue;
                }
                if (s[e] == q) {
                    e++;
                    break;
                }
                e++;
            }
            if (e > n) e = n;
            memset(m + i, 0, e - i);
            i = e;
        } else {
            i++;
        }
    }
    return m;
}

static int is_code_match(const char *s, const unsigned char *m,
                         size_t start, size_t end)
{
    size_t i;
    for (i = start; i < end; i++)
        if (!m[i] && !isspace((unsigned char)s[i])) return 0;
    return 1;
}

/* Matches \b(dispatch_jmp|dispatch)\s*\(\s*c\s*,\s*0x([0-9A-Fa-f]{8})u?\s*\) */
static int match_site(const char *s, size_t n, size_t pos, size_t *end,
                      int *is_tail, uint32_t *va, size_t *hexpos)
{
    size_t p = pos;
    int tail;
    size_t e;
    if (pos > 0 && is_word((unsigned char)s[pos - 1])) return 0;
    if (p + 12 <= n && memcmp(s + p, "dispatch_jmp", 12) == 0 &&
        (p + 12 == n || !is_word((unsigned char)s[p + 12]))) {
        tail = 1;
        p += 12;
    } else if (p + 8 <= n && memcmp(s + p, "dispatch", 8) == 0 &&
               (p + 8 == n || !is_word((unsigned char)s[p + 8]))) {
        tail = 0;
        p += 8;
    } else {
        return 0;
    }
    if (!skip_ws(s, n, &p)) return 0;
    if (!expect_bytes(s, n, &p, "(")) return 0;
    if (!skip_ws(s, n, &p)) return 0;
    if (!expect_bytes(s, n, &p, "c")) return 0;
    if (!skip_ws(s, n, &p)) return 0;
    if (!expect_bytes(s, n, &p, ",")) return 0;
    if (!skip_ws(s, n, &p)) return 0;
    if (!expect_bytes(s, n, &p, "0x")) return 0;
    if (!parse_hex8(s, n, p, va, &e)) return 0;
    *hexpos = p;
    p = e;
    if (p < n && s[p] == 'u') p++;
    if (!skip_ws(s, n, &p)) return 0;
    if (!expect_bytes(s, n, &p, ")")) return 0;
    *end = p;
    *is_tail = tail;
    return 1;
}

static void target_add(TargetVec *t, uint32_t v)
{
    size_t i;
    for (i = 0; i < t->n; i++)
        if (t->v[i] == v) return;
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->v = (uint32_t *)xrealloc(t->v, t->cap * sizeof(*t->v));
    }
    t->v[t->n++] = v;
}

static int line_is_include(const char *s, size_t n, size_t pos,
                           const char *name, size_t *line_end)
{
    size_t p = pos;
    size_t len = strlen(name);
    while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
    if (p + 8 > n || memcmp(s + p, "#include", 8) != 0) return 0;
    p += 8;
    if (p >= n || (s[p] != ' ' && s[p] != '\t')) return 0;
    while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
    if (p >= n || s[p] != '"') return 0;
    p++;
    if (p + len > n || memcmp(s + p, name, len) != 0) return 0;
    p += len;
    if (p >= n || s[p] != '"') return 0;
    p++;
    while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
    if (p < n && s[p] != '\n') return 0;
    *line_end = p;
    return 1;
}

static int find_include(const char *s, size_t n, const char *name,
                        size_t *end)
{
    size_t i = 0;
    while (i < n) {
        size_t e;
        if (line_is_include(s, n, i, name, &e)) {
            *end = e;
            return 1;
        }
        while (i < n && s[i] != '\n') i++;
        if (i < n) i++;
    }
    return 0;
}

static Buf ensure_include(const Buf *in)
{
    Buf out = {0};
    size_t e;
    if (find_include(in->p, in->n, "direct_call.h", &e))
        return *in;
    if (find_include(in->p, in->n, "cpu.h", &e)) {
        buf_add(&out, in->p, e);
        buf_str(&out, "\n#include \"direct_call.h\"");
        buf_add(&out, in->p + e, in->n - e);
        return out;
    }
    buf_str(&out, "#include \"direct_call.h\"\n");
    buf_add(&out, in->p, in->n);
    return out;
}

static Buf insert_prototypes(const Buf *in, TargetVec *t)
{
    Buf out = {0};
    size_t e;
    size_t i;
    if (!t->n) return *in;
    qsort(t->v, t->n, sizeof(*t->v), u32_cmp);
    if (find_include(in->p, in->n, "direct_call.h", &e)) {
        buf_add(&out, in->p, e);
        buf_str(&out, "\n\n/* P0-1: converted call targets (may be in another TU). */\n");
        for (i = 0; i < t->n; i++) {
            char line[64];
            if (i) buf_ch(&out, '\n');
            snprintf(line, sizeof(line), "void L_%08X(CPU *c);", t->v[i]);
            buf_str(&out, line);
        }
        buf_add(&out, in->p + e, in->n - e);
        return out;
    }
    for (i = 0; i < t->n; i++) {
        char line[64];
        if (i) buf_ch(&out, '\n');
        snprintf(line, sizeof(line), "void L_%08X(CPU *c);", t->v[i]);
        buf_str(&out, line);
    }
    buf_str(&out, "\n\n");
    buf_add(&out, in->p, in->n);
    return out;
}

typedef struct {
    size_t constant_calls;
    size_t constant_tails;
    size_t converted_calls;
    size_t converted_tails;
    size_t skipped_unknown;
    size_t skipped_hle;
} Counters;

static Buf transform(const char *s, size_t n, const VaSet *lift, Counters *c)
{
    Buf out = {0};
    TargetVec targets = {0};
    FuncVec funcs = find_functions(s, n);
    unsigned char *mask = code_mask(s, n, &funcs);
    size_t i = 0;

    while (i < n) {
        size_t end, hexpos;
        int tail;
        uint32_t va;
        if (s[i] == 'd' && match_site(s, n, i, &end, &tail, &va, &hexpos)) {
            if (!is_code_match(s, mask, i, end)) {
                buf_ch(&out, s[i++]);
                continue;
            }
            if (tail) c->constant_tails++;
            else c->constant_calls++;
            if (va >= HLE_LO && va < HLE_HI) {
                c->skipped_hle++;
                buf_add(&out, s + i, end - i);
                i = end;
                continue;
            }
            if (!va_has(lift, va)) {
                c->skipped_unknown++;
                buf_add(&out, s + i, end - i);
                i = end;
                continue;
            }
            buf_str(&out, tail ? "WC3_DIRECT_TAIL(L_" : "WC3_DIRECT_CALL(L_");
            buf_add(&out, s + hexpos, 8);
            buf_str(&out, ", c)");
            target_add(&targets, va);
            if (tail) c->converted_tails++;
            else c->converted_calls++;
            i = end;
            continue;
        }
        buf_ch(&out, s[i++]);
    }
    free(mask);
    free(funcs.v);

    if (targets.n) {
        Buf inc = ensure_include(&out);
        Buf fin = insert_prototypes(&inc, &targets);
        if (inc.p != out.p) buf_free(&inc);
        buf_free(&out);
        out = fin;
    }
    free(targets.v);
    return out;
}

static int self_test(void)
{
    static const char fixture[] =
        "#include \"cpu.h\"\n"
        "void L_00401000(CPU *c) { dispatch(c, 0x00401010u); return; }\n"
        "void L_00401010(CPU *c) { dispatch_jmp(c, 0x00401020u); return; }\n"
        "void L_00401020(CPU *c) { dispatch(c, 0x7F000010u); return; }\n"
        "void L_00401030(CPU *c) { dispatch(c, 0xDEADBEEFu); return; }\n"
        "void L_00401040(CPU *c)\n"
        "{\n"
        "    /* dispatch(c, 0x00401010u); */\n"
        "    const char *s = \"dispatch(c, 0x00401010u)\";\n"
        "    (void)s;\n"
        "}\n";
    VaSet lift = {0};
    Counters c = {0};
    Buf out;
    int ok;
    va_add(&lift, 0x00401000u);
    va_add(&lift, 0x00401010u);
    va_add(&lift, 0x00401020u);
    va_add(&lift, 0x00401030u);
    va_add(&lift, 0x00401040u);
    va_sort_unique(&lift);
    out = transform(fixture, sizeof(fixture) - 1, &lift, &c);
    ok = c.converted_calls == 1 && c.converted_tails == 1 &&
         strstr(out.p, "WC3_DIRECT_CALL(L_00401010, c)") != NULL &&
         strstr(out.p, "WC3_DIRECT_TAIL(L_00401020, c)") != NULL &&
         strstr(out.p, "dispatch(c, 0x7F000010u)") != NULL &&
         strstr(out.p, "dispatch(c, 0xDEADBEEFu)") != NULL &&
         strstr(out.p, "#include \"direct_call.h\"") != NULL &&
         strstr(out.p, "void L_00401010(CPU *c);") != NULL;
    if (!ok) {
        fprintf(stderr, "devirtualize self-test: FAIL\n%s\n", out.p ? out.p : "");
        buf_free(&out);
        free(lift.v);
        return 1;
    }
    buf_free(&out);
    free(lift.v);
    puts("devirtualize self-test: OK");
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: devirtualize --list LIST --lift IN --out OUT [--json JSON]\n"
            "       devirtualize --self-test\n");
}

int main(int argc, char **argv)
{
    const char *list_path = NULL;
    const char *in_path = NULL;
    const char *out_path = NULL;
    const char *json_path = NULL;
    char *list_text, *in_text;
    size_t list_n, in_n;
    VaSet lift;
    Counters c = {0};
    Buf out;
    int i;
    int rc;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--self-test")) return self_test();
        else if (!strcmp(argv[i], "--list") && i + 1 < argc) list_path = argv[++i];
        else if (!strcmp(argv[i], "--lift") && i + 1 < argc) in_path = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) json_path = argv[++i];
        else {
            usage();
            return 2;
        }
    }
    if (!list_path || !in_path || !out_path) {
        usage();
        return 2;
    }
    list_text = slurp(list_path, &list_n);
    if (!list_text) return 2;
    in_text = slurp(in_path, &in_n);
    if (!in_text) {
        free(list_text);
        return 2;
    }
    {
        size_t norm_n = 0;
        char *norm = normalize_newlines(in_text, in_n, &norm_n);
        free(in_text);
        in_text = norm;
        in_n = norm_n;
    }
    lift = parse_va_list(list_text, list_n);
    if (!lift.n) {
        fprintf(stderr, "devirtualize: no VAs in %s\n", list_path);
        free(list_text);
        free(in_text);
        free(lift.v);
        return 2;
    }
    out = transform(in_text, in_n, &lift, &c);
    rc = write_file(out_path, out.p ? out.p : "", out.n);
    if (json_path) {
        FILE *f = fopen(json_path, "wb");
        if (f) {
            fprintf(f,
                    "{\n  \"schema\": 1,\n  \"pass_id\": \"devirtualize\",\n"
                    "  \"files\": 1,\n  \"converted\": %zu,\n"
                    "  \"skipped_unknown\": %zu,\n"
                    "  \"skipped_hle_window\": %zu,\n"
                    "  \"verdict\": \"%s\"\n}\n",
                    c.converted_calls + c.converted_tails,
                    c.skipped_unknown, c.skipped_hle,
                    rc == 0 ? "pass" : "fail");
            fclose(f);
        }
    }
    if (rc == 0)
        printf("devirtualize: pass (1 files, %zu sites)\n",
               c.converted_calls + c.converted_tails);
    buf_free(&out);
    free(lift.v);
    free(list_text);
    free(in_text);
    return rc == 0 ? 0 : 1;
}
