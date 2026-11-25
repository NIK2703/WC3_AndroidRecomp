/* Standalone prototype of the lift-time esp promotion pass (write-through).
 * Analysis helper only. Once validated it is ported into liftcore.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static const void *esp_memmem(const void *hay, size_t n,
                              const void *needle, size_t m)
{
    const unsigned char *h = (const unsigned char *)hay;
    const unsigned char *p = (const unsigned char *)needle;
    size_t i;
    if (!m) return hay;
    if (m > n) return NULL;
    for (i = 0; i + m <= n; i++)
        if (!memcmp(h + i, p, m)) return h + i;
    return NULL;
}

static void append_n(char **out, size_t *len, size_t *cap,
                     const char *s, size_t n)
{
    if (*len + n + 1 > *cap) {
        while (*len + n + 1 > *cap) *cap = *cap * 2 + 4096;
        *out = (char *)realloc(*out, *cap);
        if (!*out) abort();
    }
    memcpy(*out + *len, s, n);
    *len += n;
    (*out)[*len] = 0;
}

static void append_s(char **out, size_t *len, size_t *cap, const char *s)
{
    append_n(out, len, cap, s, strlen(s));
}

static int has(const char *s, size_t n, const char *needle)
{
    return esp_memmem(s, n, needle, strlen(needle)) != NULL;
}

static size_t line_code_len(const char *s, size_t n)
{
    const char *c = (const char *)esp_memmem(s, n, "/*", 2);
    return c ? (size_t)(c - s) : n;
}

static int line_is_label(const char *s, size_t n)
{
    return n > 2 && s[0] == 'L' && s[1] == '_' && memchr(s, ':', n) != NULL;
}

static int line_is_call(const char *s, size_t n)
{
    return has(s, n, "dispatch(") || has(s, n, "dispatch_jmp(") ||
           has(s, n, "WC3_DIRECT_CALL") || has(s, n, "WC3_DIRECT_TAIL") ||
           has(s, n, "WC3_CALLSITE_OUTLINE") ||
           has(s, n, "WC3_CLEAN_CALL_HELPER") ||
           has(s, n, "WC3_CALL_GS_COOKIE") ||
           has(s, n, "RECOMP_TODO") || has(s, n, "HLE_") ||
           has(s, n, "WC3_CLEAN_DRAIN") || has(s, n, "wc3_guest_drain");
}

static int line_is_ret(const char *s, size_t n)
{
    return has(s, n, "return;");
}

static int line_is_goto(const char *s, size_t n)
{
    return has(s, n, "goto L_");
}

/* A deferred c->esp write must land before the switch, not between
 * `switch (...) {` and the first `case`.  Statements in that gap are
 * unreachable for every case label: the compiler is allowed to drop them,
 * and clang does.  The same applies to the jump-target labels themselves:
 * flushing before a later case is correct for fallthrough and harmless for a
 * direct jump, because the skipped case body did not push anything. */
static int line_is_switch(const char *s, size_t n)
{
    return has(s, n, "switch (");
}

static int line_is_case_label(const char *s, size_t n)
{
    size_t p = 0;
    while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
    if (n - p >= 5 && memcmp(s + p, "case ", 5) == 0) return 1;
    if (n - p >= 8 && memcmp(s + p, "default:", 8) == 0) return 1;
    return 0;
}

static int line_is_break_or_continue(const char *s, size_t n)
{
    size_t p = 0;
    while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
    if (n - p >= 6 && memcmp(s + p, "break;", 6) == 0) return 1;
    if (n - p >= 9 && memcmp(s + p, "continue;", 9) == 0) return 1;
    return 0;
}

static int line_has_inline_push(const char *s, size_t n)
{
    size_t p = 0;
    while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
    return !(n - p >= 9 && memcmp(s + p, "push32(c,", 9) == 0) &&
           has(s, n, "push32(c,");
}

static int line_is_plain_push(const char *s, size_t n)
{
    size_t p = 0;
    while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
    return n - p >= 9 && memcmp(s + p, "push32(c,", 9) == 0;
}

static int modifies_esp(const char *s, size_t n)
{
    return has(s, n, "c->esp =") || has(s, n, "c->esp +=") ||
           has(s, n, "c->esp -=") || has(s, n, "push32(c,") ||
           has(s, n, "pop32(c)");
}

static void emit_plain(char **dst, size_t *dn, size_t *dcap,
                       const char *s, size_t n)
{
    size_t i = 0, last = 0;
    while (i < n) {
        if (i + 8 <= n && memcmp(s + i, "pop32(c)", 8) == 0) {
            append_n(dst, dn, dcap, s + last, i - last);
            append_s(dst, dn, dcap, "WC3_POP_ESP(c, esp)");
            i += 8;
            last = i;
            continue;
        }
        if (i + 6 <= n && memcmp(s + i, "c->esp", 6) == 0) {
            append_n(dst, dn, dcap, s + last, i - last);
            append_s(dst, dn, dcap, "esp");
            i += 6;
            last = i;
            continue;
        }
        i++;
    }
    append_n(dst, dn, dcap, s + last, n - last);
}

static void emit_code(char **dst, size_t *dn, size_t *dcap,
                      const char *s, size_t n)
{
    size_t i = 0, last = 0;
    while (i < n) {
        const char *h = (const char *)esp_memmem(s + i, n - i, "push32(c,", 9);
        if (!h) break;
        size_t p = (size_t)(h - s) + 9;
        size_t q = p;
        int depth = 0;
        while (q < n) {
            if (s[q] == '(') depth++;
            else if (s[q] == ')') {
                if (depth == 0) break;
                depth--;
            }
            q++;
        }
        if (q >= n) break;
        append_n(dst, dn, dcap, s + last, (size_t)(h - s) - last);
        size_t ep = p, eq = q;
        while (ep < eq && (s[ep] == ' ' || s[ep] == '\t')) ep++;
        while (eq > ep && (s[eq - 1] == ' ' || s[eq - 1] == '\t')) eq--;
        int temp = has(s + ep, eq - ep, "c->esp") ||
                   has(s + ep, eq - ep, "pop32(c)");
        if (temp) {
            append_s(dst, dn, dcap, "{ uint32_t _wc3_pv = (");
            emit_plain(dst, dn, dcap, s + ep, eq - ep);
            append_s(dst, dn, dcap,
                     "); esp -= 4u; wr32(STACK_BASE(c) + esp, _wc3_pv); }");
        } else {
            append_s(dst, dn, dcap,
                     "{ esp -= 4u; wr32(STACK_BASE(c) + esp, (");
            emit_plain(dst, dn, dcap, s + ep, eq - ep);
            append_s(dst, dn, dcap, ")); }");
        }
        last = q + 1;
        i = last;
    }
    emit_plain(dst, dn, dcap, s + last, n - last);
}

void lf_esp_local(char **out, size_t *len)
{
    char *s = *out;
    size_t n = *len;
    if (!has(s, n, "c->esp") && !has(s, n, "push32(c,") &&
        !has(s, n, "pop32(c)"))
        return;
    if (has(s, n, "uint32_t esp = c->esp"))
        return;

    const char *body = (const char *)esp_memmem(s, n, "\n{\n", 3);
    if (!body) return;
    size_t start = (size_t)(body - s) + 3;

    char *bd = NULL;
    size_t bn = 0, bcap = 0;
    int pending = 0, changed = 0;

#define FLUSH_PENDING() do { \
        if (pending) { \
            append_s(&bd, &bn, &bcap, "    c->esp = esp;\n"); \
            pending = 0; changed = 1; \
        } \
    } while (0)

    size_t pos = start;
    while (pos < n) {
        size_t le = pos;
        while (le < n && s[le] != '\n') le++;
        size_t ll = le - pos;
        const char *line = s + pos;
        size_t cl = line_code_len(line, ll);

        if (pending &&
            (line_is_switch(line, cl) || line_is_case_label(line, cl) ||
             line_is_break_or_continue(line, cl))) {
            FLUSH_PENDING();
        }

        if (line_is_label(line, ll)) {
            FLUSH_PENDING();
            append_n(&bd, &bn, &bcap, line, ll);
            if (le < n) append_s(&bd, &bn, &bcap, "\n");
            append_s(&bd, &bn, &bcap, "    esp = c->esp;\n");
            changed = 1;
            pos = (le < n) ? le + 1 : le;
            continue;
        }
        int blank_code = 1;
        for (size_t k = 0; k < cl; k++) {
            if (line[k] != ' ' && line[k] != '\t') {
                blank_code = 0;
                break;
            }
        }
        if (blank_code) {
            FLUSH_PENDING();
            append_n(&bd, &bn, &bcap, line, ll);
            if (le < n) append_s(&bd, &bn, &bcap, "\n");
            pos = (le < n) ? le + 1 : le;
            continue;
        }

        int call = line_is_call(line, cl);
        int ret = line_is_ret(line, cl);
        int go = line_is_goto(line, cl);
        int inline_push = line_has_inline_push(line, cl);
        int plain_push = line_is_plain_push(line, cl);
        int plain_pop = has(line, cl, "pop32(c)");

        if (call || inline_push) {
            FLUSH_PENDING();
            append_n(&bd, &bn, &bcap, line, ll);
            if (le < n) append_s(&bd, &bn, &bcap, "\n");
            if (!ret) append_s(&bd, &bn, &bcap, "    esp = c->esp;\n");
            changed = 1;
        } else if (ret || go) {
            FLUSH_PENDING();
            append_n(&bd, &bn, &bcap, line, ll);
            if (le < n) append_s(&bd, &bn, &bcap, "\n");
        } else {
            size_t before = bn;
            emit_code(&bd, &bn, &bcap, line, cl);
            append_n(&bd, &bn, &bcap, line + cl, ll - cl);
            if (le < n) append_s(&bd, &bn, &bcap, "\n");
            if (bn != before + ll + (le < n ? 1 : 0)) changed = 1;
            if (plain_push || plain_pop)
                pending = 1;
            else if (modifies_esp(line, cl)) {
                append_s(&bd, &bn, &bcap, "    c->esp = esp;\n");
                changed = 1;
            }
        }
        pos = (le < n) ? le + 1 : le;
    }
    FLUSH_PENDING();
#undef FLUSH_PENDING

    if (!changed) {
        free(bd);
        return;
    }

    char *dst = NULL;
    size_t dn = 0, dcap = 0;
    append_n(&dst, &dn, &dcap, s, start);
    append_s(&dst, &dn, &dcap, "    uint32_t esp = c->esp;\n");
    append_n(&dst, &dn, &dcap, bd, bn);
    free(bd);
    free(*out);
    *out = dst;
    *len = dn;
}


/* ------------------------------------------------------------------ */
/* Batch CLI: process every chunk from --inputs into --out-dir.        */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t files;
    size_t files_rewritten;
    size_t functions;
    size_t functions_rewritten;
    size_t inputs;
} Stats;

static void die(const char *msg)
{
    fprintf(stderr, "esp_local: %s\n", msg);
    exit(2);
}

/* Hard guard against the regression that produced the 2026-11-25 startup
 * crash: a deferred write-back placed after `switch (...) {` is unreachable
 * for every case, so the target function reads the caller's selector as the
 * object pointer.  The transform above must always flush before the switch;
 * this scan makes a future ordering mistake fail the lift instead of the
 * device. */
static void validate_switch_writes(const char *s, size_t n)
{
    size_t pos = 0;
    int in_switch = 0;
    while (pos < n) {
        size_t le = pos;
        while (le < n && s[le] != '\n') le++;
        size_t ll = le - pos;
        size_t cl = line_code_len(s + pos, ll);
        const char *line = s + pos;
        size_t a = 0;
        while (a < cl && (line[a] == ' ' || line[a] == '\t')) a++;
        if (has(line, cl, "switch (")) {
            in_switch = 1;
        } else if (in_switch) {
            if (cl - a >= 5 && memcmp(line + a, "case ", 5) == 0) {
                in_switch = 0;
            } else if (cl - a >= 8 && memcmp(line + a, "default:", 8) == 0) {
                in_switch = 0;
            } else if (has(line, cl, "c->esp = esp;")) {
                die("c->esp write-back inside switch before first case");
            }
        }
        pos = (le < n) ? le + 1 : le;
    }
}

static char *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    char *p = NULL;
    size_t cap = 0, len = 0;
    if (!f) die("cannot open input");
    for (;;) {
        size_t got;
        if (len + 65536 + 1 > cap) {
            cap = cap ? cap * 2 + 65536 : 131072;
            p = (char *)realloc(p, cap);
            if (!p) die("out of memory");
        }
        got = fread(p + len, 1, 65536, f);
        len += got;
        if (got < 65536) break;
    }
    if (ferror(f)) die("read failed");
    fclose(f);
    p[len] = 0;
    *n = len;
    return p;
}

static void write_file(const char *path, const char *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot open output");
    if (n && fwrite(p, 1, n, f) != n) die("write failed");
    fclose(f);
}

static void load_paths(const char *path, char ***out, size_t *count)
{
    size_t dummy;
    char *text = read_file(path, &dummy);
    char **v = (char **)malloc(16 * sizeof(char *));
    size_t cap = 16, n = 0, pos = 0;
    while (pos < dummy) {
        size_t le = pos;
        size_t a, b;
        while (le < dummy && text[le] != '\n') le++;
        a = pos; b = le;
        while (a < b && (text[a] == ' ' || text[a] == '\t' || text[a] == '\r')) a++;
        while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r')) b--;
        if (b > a) {
            if (n == cap) {
                cap *= 2;
                v = (char **)realloc(v, cap * sizeof(char *));
                if (!v) die("out of memory");
            }
            v[n] = (char *)malloc(b - a + 1);
            memcpy(v[n], text + a, b - a);
            v[n][b - a] = 0;
            n++;
        }
        pos = (le < dummy) ? le + 1 : le;
    }
    free(text);
    *out = v;
    *count = n;
}

static const char *path_base(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int trimmed_is(const char *s, size_t n, const char *want)
{
    size_t a = 0, b = n, wl = strlen(want);
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return b - a == wl && !memcmp(s + a, want, wl);
}

static void transform_text(const char *s, size_t n, char **out, Stats *st)
{
    char *dst = NULL;
    size_t dn = 0, dcap = 0, pos = 0;
    while (pos < n) {
        size_t le = pos, next, open_end, bend, fnn;
        const char *line = s + pos;
        char *fn, *res;
        int is_header;

        while (le < n && s[le] != '\n') le++;
        {
            size_t cl = line_code_len(line, le - pos);
            is_header = has(line, cl, "void L_") &&
                        has(line, cl, "(CPU *restrict c)");
        }
        if (!is_header || le >= n) {
            append_n(&dst, &dn, &dcap, s + pos, (le < n) ? (le - pos + 1) : (le - pos));
            pos = (le < n) ? le + 1 : le;
            continue;
        }

        next = le + 1;
        open_end = next;
        while (open_end < n && s[open_end] != '\n') open_end++;
        if (!trimmed_is(s + next, open_end - next, "{")) {
            append_n(&dst, &dn, &dcap, s + pos, (open_end < n) ? (open_end - pos + 1) : (open_end - pos));
            pos = (open_end < n) ? open_end + 1 : open_end;
            continue;
        }

        bend = open_end + 1;
        while (bend < n) {
            size_t e = bend;
            while (e < n && s[e] != '\n') e++;
            if (trimmed_is(s + bend, e - bend, "}")) break;
            bend = (e < n) ? e + 1 : e;
        }
        if (bend >= n) bend = n;
        else {
            size_t e = bend;
            while (e < n && s[e] != '\n') e++;
            bend = (e < n) ? e + 1 : e;
        }

        fnn = bend - pos;
        fn = (char *)malloc(fnn + 1);
        memcpy(fn, s + pos, fnn);
        fn[fnn] = 0;
        res = (char *)fn;
        lf_esp_local(&res, &fnn);
        validate_switch_writes(res, fnn);
        st->functions++;
        if (res != fn) st->functions_rewritten++;
        append_n(&dst, &dn, &dcap, res, fnn);
        /* lf_esp_local frees its input buffer when it changes the function,
         * so only one of the two buffers is owned here. */
        if (res != fn) free(res);
        else free(fn);
        pos = bend;
    }
    if (!dst) {
        dst = (char *)malloc(1);
        dn = 0;
        dst[0] = 0;
    }
    *out = dst;
}

static void write_report(const char *path, const Stats *st)
{
    char buf[512];
    int n;
    FILE *f;
    if (!path) return;
    n = snprintf(buf, sizeof buf,
        "{\"verdict\":\"pass\",\"files\":%zu,\"files_rewritten\":%zu,"
        "\"functions\":%zu,\"functions_rewritten\":%zu,"
        "\"inputs\":%zu,\"files_skipped_existing\":0}\n",
        st->files, st->files_rewritten, st->functions,
        st->functions_rewritten, st->inputs);
    if (n < 0) die("report format failed");
    f = fopen(path, "wb");
    if (!f) die("cannot open report");
    if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) die("report write failed");
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *inputs = NULL, *out_dir = NULL, *report = NULL;
    int quiet = 0;
    char **paths = NULL;
    size_t count = 0, i;
    Stats st = {0};

    for (i = 1; i < (size_t)argc; i++) {
        if (!strcmp(argv[i], "--inputs") && i + 1 < (size_t)argc) inputs = argv[++i];
        else if (!strcmp(argv[i], "--out-dir") && i + 1 < (size_t)argc) out_dir = argv[++i];
        else if (!strcmp(argv[i], "--report") && i + 1 < (size_t)argc) report = argv[++i];
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "--help")) {
            puts("esp_local --inputs FILE --out-dir DIR [--report JSON] [--quiet]");
            return 0;
        } else die("unknown argument");
    }
    if (!inputs || !out_dir) die("--inputs and --out-dir are required");
    load_paths(inputs, &paths, &count);
    st.inputs = count;
    st.files = count;

    for (i = 0; i < count; i++) {
        size_t n, on;
        char *text = read_file(paths[i], &n);
        char *out = NULL;
        char *before = (char *)malloc(n + 1);
        size_t need = strlen(out_dir) + strlen(path_base(paths[i])) + 2;
        char *out_path = (char *)malloc(need);
        memcpy(before, text, n);
        before[n] = 0;
        transform_text(text, n, &out, &st);
        snprintf(out_path, need, "%s/%s", out_dir, path_base(paths[i]));
        write_file(out_path, out, strlen(out));
        if (strcmp(before, out)) st.files_rewritten++;
        on = strlen(out);
        (void)on;
        free(out_path);
        free(before);
        free(out);
        free(text);
    }
    if (!quiet)
        fprintf(stderr, "esp_local: %zu files, %zu rewritten, %zu functions, %zu rewritten\n",
                st.files, st.files_rewritten, st.functions, st.functions_rewritten);
    write_report(report, &st);
    return 0;
}
