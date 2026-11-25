/* x87_fpsr.c - batch patch stage: deferred x87 IE via ARM64 FPSR.IOC.
 *
 * The canonical corpus is generated once and the build applies patch stages to
 * it; changing the C lifter is not enough.  This stage rewrites eligible leaf
 * functions in the generated C:
 *
 *   - wraps the body with WC3_X87_FPSR_BEGIN/END;
 *   - replaces x87_add/sub/mul/div with the _fpsr variants;
 *   - closes the regime before every return/dispatch_jmp.
 *
 * Eligible functions are additionally gated by a measured size policy: FPSR
 * costs a fixed prologue and one epilogue per exit, so low-arithmetic functions
 * with many exits grow.  Use --no-size-policy for the old all-eligible
 * behaviour; the default keeps only profitable functions.
 *
 * A function is eligible only if it performs IEEE add/sub/mul/div and has no
 * guest call and no instruction that observes or raises x87 state outside that
 * allowlist.  Everything else keeps the exact software path.  Guest calls
 * include the generated direct-call macros, not only dispatch(c, va): a callee
 * must observe the guest FPCR/FPSR and may execute fldcw/fclex/fstsw.
 *
 * Usage: x87_fpsr --inputs FILE --out-dir DIR [--report JSON] [--quiet]
 *        x87_fpsr --self-test
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char *m)
{
    fprintf(stderr, "x87_fpsr: %s\n", m);
    exit(1);
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    size_t cap = 4096, n = 0;
    char *s;
    if (!f) die(path);
    s = (char *)malloc(cap);
    for (;;) {
        size_t got;
        if (n + 1024 > cap) { cap *= 2; s = (char *)realloc(s, cap); }
        got = fread(s + n, 1, 1024, f);
        n += got;
        if (got < 1024) break;
    }
    fclose(f);
    s[n] = 0;
    *len = n;
    return s;
}

static void write_file(const char *path, const char *s, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) die(path);
    if (n && fwrite(s, 1, n, f) != n) die("write failed");
    fclose(f);
}

static const char *path_base(const char *p)
{
    const char *b = p, *q;
    for (q = p; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

static char *replace_all(const char *s, const char *from, const char *to)
{
    size_t fl = strlen(from), tl = strlen(to), n = strlen(s), cap = n + 256;
    char *out = (char *)malloc(cap), *d = out;
    const char *p = s;
    while (*p) {
        if (!strncmp(p, from, fl)) {
            size_t need = (size_t)(d - out) + tl + 1;
            if (need > cap) {
                size_t off = (size_t)(d - out);
                while (need > cap) cap *= 2;
                out = (char *)realloc(out, cap);
                d = out + off;
            }
            memcpy(d, to, tl);
            d += tl;
            p += fl;
        } else {
            size_t need = (size_t)(d - out) + 2;
            if (need > cap) {
                size_t off = (size_t)(d - out);
                while (need > cap) cap *= 2;
                out = (char *)realloc(out, cap);
                d = out + off;
            }
            *d++ = *p++;
        }
    }
    *d = 0;
    return out;
}

static int is_arity_line(const char *line, const char *pat)
{
    return strstr(line, pat) != NULL;
}

static int is_arith_line(const char *line)
{
    return strstr(line, "x87_add(") || strstr(line, "x87_sub(") ||
           strstr(line, "x87_mul(") || strstr(line, "x87_div(");
}

/* A guest call is an observation point: the callee must see the guest FPCR
 * and FPSR, and it may execute fldcw/fclex/fstsw while the caller's IE is
 * still deferred.  Tail calls are different: rewrite() emits END before every
 * dispatch_jmp/return, so the callee enters with the guest state restored.
 *
 * The generated corpus uses direct-call macros rather than dispatch(c, va);
 * checking only dispatch() let 132 functions with guest calls through. */
static int is_guest_call_line(const char *line)
{
    return strstr(line, "dispatch(c,") ||
           strstr(line, "WC3_DIRECT_CALL") ||
           strstr(line, "WC3_CALLSITE_OUTLINE") ||
           strstr(line, "WC3_CLEAN_CALL_HELPER") ||
           strstr(line, "WC3_CALL_GS_COOKIE");
}

static int eligible(const char *const *lines, size_t a, size_t b)
{
    static const char *const disallow[] = {
        "x87_compare(", "x87_sqrt(", "x87_fprem(", "x87_roundint(",
        "x87_fldcw(", "x87_fstsw", "x87_fnstsw", "x87_fstenv",
        "x87_fs", "x87_fwait",
        "fnstsw", "fstsw", "fstenv", "fnstenv", "fsave", "fnsave",
        "fwait", "fnclex", "fclex", "fldcw", "fiadd", "fisub", "fimul",
        "fidiv", "fcom", "fucom", "fprem", "fsqrt", "frndint",
        "f2xm1", "fyl2x", "fpatan", "fxam", "finit", "fninit"
    };
    size_t i, k;
    int arith = 0;
    int first_arith = -1, first_todo = -1;
    for (i = a; i <= b; i++) {
        if (first_arith < 0 && is_arith_line(lines[i]))
            first_arith = (int)i;
        if (first_todo < 0 && strstr(lines[i], "RECOMP_TODO"))
            first_todo = (int)i;
        if (is_arith_line(lines[i])) arith = 1;
        if (is_guest_call_line(lines[i])) return 0;
        for (k = 0; k < sizeof disallow / sizeof disallow[0]; k++)
            if (strstr(lines[i], disallow[k])) return 0;
    }
    /* An unconditional abort before the first x87 arithmetic makes the
     * arithmetic dead.  The FPSR prologue only adds instructions to a body
     * that already aborts; rejecting it also avoids dead-code "regressions". */
    if (first_todo >= 0 && (first_arith < 0 || first_todo < first_arith))
        return 0;
    return arith;
}

static const char *trim_left(const char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static int trim_right_is(const char *s, char c)
{
    const char *p = s + strlen(s);
    while (p > s && isspace((unsigned char)p[-1])) p--;
    return p > s && p[-1] == c;
}

/* Generated labels are alone on their line: "L_3530104B:". */
static int label_name(const char *line, char *buf, size_t cap)
{
    const char *p = trim_left(line), *q;
    size_t k = 0;
    if (p[0] != 'L' || p[1] != '_') return 0;
    q = p;
    while (*q && *q != ':') {
        if (k + 1 >= cap) return 0;
        buf[k++] = *q++;
    }
    buf[k] = 0;
    if (*q != ':') return 0;
    while (*++q)
        if (!isspace((unsigned char)*q)) return 0;
    return (int)k;
}

static int goto_name(const char *line, char *buf, size_t cap)
{
    const char *p = strstr(line, "goto "), *q;
    size_t k = 0;
    if (!p) return 0;
    p = trim_left(p + 5);
    if (p[0] != 'L' || p[1] != '_') return 0;
    q = p;
    while (*q && (*q == '_' || isalnum((unsigned char)*q))) {
        if (k + 1 >= cap) return 0;
        buf[k++] = *q++;
    }
    buf[k] = 0;
    if (*q != ';') return 0;
    return (int)k;
}

static int is_exit_line(const char *line)
{
    return strstr(line, "return;") != NULL ||
           strstr(line, "dispatch_jmp(") != NULL;
}

/* Size gate measured on the full corpus.
 *
 * The FPSR regime costs a fixed prologue plus an epilogue per exit, while the
 * software path costs a short NaN test per arithmetic site.  A function with
 * one or two sites and many exits therefore grows.  The measured boundary is
 * close to one arithmetic site per ten body lines, with one line of slack for
 * the wrap: accept when lines <= 10*ops + 8.  One-site functions with at most
 * two exits are also profitable regardless of length.  On the 263-chunk
 * corpus this keeps 127 of 181 functions, removes all 42 static regressions
 * and still saves more than applying FPSR to every eligible function. */
static int size_policy_ok(size_t ops, size_t ends, size_t lines)
{
    if (ops == 1 && ends <= 2) return 1;
    return lines <= 10 * ops + 8;
}

/* Mark exits that cannot be reached from any x87 arithmetic site.  IE cannot
 * be set on such a path, so the full END there can be replaced by END_CLEAN:
 * FPCR/FPSR are still restored, only the IE transfer is skipped. */
static void mark_clean_exits(const char *const *lines, size_t lo, size_t hi,
                             unsigned char *clean, unsigned char *visit,
                             size_t visit_cap)
{
    size_t i, k, n = hi - lo + 1;
    char name[64];
    size_t *labels = (size_t *)malloc(n * sizeof(size_t));
    size_t nlabels = 0;
    size_t *stack = (size_t *)malloc(n * sizeof(size_t));
    int *skip = (int *)malloc(n * sizeof(int));
    int depth = 0;

    for (i = 0; i < n; i++) {
        const char *line = lines[lo + i];
        skip[i] = -1;
        clean[lo + i] = is_exit_line(line) ? 1 : 0;
        if (label_name(line, name, sizeof name)) {
            if (nlabels < n) labels[nlabels++] = lo + i;
        }
        /* Brace depth counts raw characters; generated comments do not
         * contain braces. */
        for (k = 0; line[k]; k++) {
            if (line[k] == '{') depth++;
            else if (line[k] == '}' && depth > 0) depth--;
        }
    }
    /* Find the statement after the close brace for lines opening an if body. */
    depth = 0;
    for (i = 0; i < n; i++) {
        const char *line = lines[lo + i];
        if (trim_right_is(line, '{')) {
            int base = depth, found = -1;
            depth++;
            for (k = i + 1; k < n; k++) {
                int before = depth;
                const char *l = lines[lo + k];
                size_t j;
                for (j = 0; l[j]; j++) {
                    if (l[j] == '{') depth++;
                    else if (l[j] == '}' && depth > 0) depth--;
                }
                if (before == base + 1 && depth == base &&
                    trim_right_is(l, '}')) { found = (int)k + 1; break; }
            }
            skip[i] = found;
        } else {
            for (k = 0; line[k]; k++) {
                if (line[k] == '{') depth++;
                else if (line[k] == '}' && depth > 0) depth--;
            }
        }
    }

    for (i = 0; i < n; i++) {
        const char *line = lines[lo + i];
        if (!is_arith_line(line)) continue;
        memset(visit, 0, visit_cap);
        {
            size_t sp = 0;
            visit[i] = 1; stack[sp++] = i;
            while (sp) {
                size_t at = stack[--sp];
                const char *l = lines[lo + at];
                const char *lt = trim_left(l);
                int conditional = lt[0] == 'i' && lt[1] == 'f' &&
                                  (lt[2] == ' ' || lt[2] == '(');
                int terminal = is_exit_line(l) && !conditional;
                size_t j;
                if (is_exit_line(l))
                    clean[lo + at] = 0; /* reachable full END */
                if (terminal)
                    continue;
                /* sequential (unless this is an unconditional goto) */
                if (!(lt[0] == 'g' && !strncmp(lt, "goto ", 5)) && at + 1 < n && !visit[at + 1]) {
                    visit[at + 1] = 1; stack[sp++] = at + 1;
                }
                /* goto target */
                if (goto_name(l, name, sizeof name)) {
                    char lname[64];
                    for (j = 0; j < nlabels; j++) {
                        size_t t = labels[j];
                        if (label_name(lines[t], lname, sizeof lname) &&
                            !strcmp(name, lname) &&
                            !visit[t - lo]) {
                            visit[t - lo] = 1; stack[sp++] = t - lo;
                        }
                    }
                }
                /* if-body skip */
                if (skip[at] >= 0 && skip[at] < (int)n && !visit[skip[at]]) {
                    visit[skip[at]] = 1; stack[sp++] = (size_t)skip[at];
                }
            }
        }
    }
    free(labels); free(stack); free(skip);
}

static void rewrite(const char *text, size_t n, const char *out_path,
                    size_t *funcs, size_t *rewritten, size_t *size_skipped,
                    int use_size_policy);

static int clean_rewrite_test(void)
{
    static const char src[] =
        "void L_00000010(CPU *restrict c)\n"
        "{\n"
        "    if (c->eax) { return; }\n"
        "    double r = x87_add(c, a, b);\n"
        "    WC3_DIRECT_TAIL(L_00000011, c); return;\n"
        "}\n";
    char in_path[L_tmpnam], out_path[L_tmpnam];
    char *out;
    size_t n, funcs = 0, rewritten = 0, size_skipped = 0;
    int ok;

    if (!tmpnam(in_path) || !tmpnam(out_path)) return 0;
    write_file(in_path, src, strlen(src));
    rewrite(src, strlen(src), out_path, &funcs, &rewritten,
            &size_skipped, 0);
    out = read_file(out_path, &n);
    ok = rewritten == 1 &&
         strstr(out, "WC3_X87_FPSR_END_CLEAN(c);") != NULL &&
         strstr(out, "    WC3_X87_FPSR_END(c);") != NULL;
    free(out);
    remove(in_path);
    remove(out_path);
    return ok;
}

static int self_test(void)
{
    static const char *const tail[] = {
        "void L_00000000(CPU *restrict c)",
        "{",
        "    double r = x87_add(c, a, b);",
        "    WC3_DIRECT_TAIL(L_00000001, c); return;",
        "}"
    };
    static const char *const direct_call[] = {
        "void L_00000002(CPU *restrict c)",
        "{",
        "    double r = x87_add(c, a, b);",
        "    WC3_DIRECT_CALL_DEPTH(L_00000003, c);",
        "    return;",
        "}"
    };
    static const char *const outline_call[] = {
        "void L_00000004(CPU *restrict c)",
        "{",
        "    double r = x87_add(c, a, b);",
        "    WC3_CALLSITE_OUTLINE(c, 0x10, L_00000005);",
        "    return;",
        "}"
    };
    static const char *const clean_call[] = {
        "void L_00000006(CPU *restrict c)",
        "{",
        "    double r = x87_add(c, a, b);",
        "    WC3_CLEAN_CALL_HELPER(L_00000007, c);",
        "    return;",
        "}"
    };
    static const char *const gs_call[] = {
        "void L_00000008(CPU *restrict c)",
        "{",
        "    double r = x87_add(c, a, b);",
        "    WC3_CALL_GS_COOKIE(c, 0x10, L_00000009);",
        "    return;",
        "}"
    };
    static const char *const dispatch_call[] = {
        "void L_0000000A(CPU *restrict c)",
        "{",
        "    double r = x87_add(c, a, b);",
        "    dispatch(c, 0x0000000B);",
        "    return;",
        "}"
    };
    static const char *const early_abort[] = {
        "void L_0000000C(CPU *restrict c)",
        "{",
        "    RECOMP_TODO(0x0000000D, \"unsupported\");",
        "    double r = x87_add(c, a, b);",
        "    return;",
        "}"
    };
    static const char *const observer[] = {
        "void L_0000000E(CPU *restrict c)",
        "{",
        "    double r = x87_add(c, a, b);",
        "    x87_fstsw(c);",
        "    return;",
        "}"
    };
    static const char *const no_arith[] = {
        "void L_0000000F(CPU *restrict c)",
        "{",
        "    c->eax = 1;",
        "    return;",
        "}"
    };

    if (!eligible(tail, 0, sizeof tail / sizeof tail[0] - 1)) return 0;
    if (eligible(direct_call, 0, sizeof direct_call / sizeof direct_call[0] - 1)) return 0;
    if (eligible(outline_call, 0, sizeof outline_call / sizeof outline_call[0] - 1)) return 0;
    if (eligible(clean_call, 0, sizeof clean_call / sizeof clean_call[0] - 1)) return 0;
    if (eligible(gs_call, 0, sizeof gs_call / sizeof gs_call[0] - 1)) return 0;
    if (eligible(dispatch_call, 0, sizeof dispatch_call / sizeof dispatch_call[0] - 1)) return 0;
    if (eligible(early_abort, 0, sizeof early_abort / sizeof early_abort[0] - 1)) return 0;
    if (eligible(observer, 0, sizeof observer / sizeof observer[0] - 1)) return 0;
    if (eligible(no_arith, 0, sizeof no_arith / sizeof no_arith[0] - 1)) return 0;
    /* Size gate boundaries measured on the full corpus. */
    if (size_policy_ok(1, 3, 40)) return 0;   /* ijl15 regression */
    if (size_policy_ok(1, 9, 76)) return 0;   /* Mss32 regression */
    if (!size_policy_ok(1, 2, 40)) return 0;  /* one op, two exits */
    if (!size_policy_ok(2, 5, 28)) return 0;  /* 10*ops + 8 */
    if (size_policy_ok(2, 5, 29)) return 0;
    if (!size_policy_ok(3, 9, 38)) return 0;
    if (size_policy_ok(3, 9, 112)) return 0;  /* L_35305B31 regression */
    if (!size_policy_ok(4, 9, 48)) return 0;
    if (size_policy_ok(4, 9, 49)) return 0;
    if (!clean_rewrite_test()) {
        fprintf(stderr, "x87_fpsr: clean-exit rewrite self-test failed\n");
        return 0;
    }
    printf("x87_fpsr: self-test ok\n");
    return 1;
}

static void rewrite(const char *text, size_t n, const char *out_path,
                    size_t *funcs, size_t *rewritten, size_t *size_skipped,
                    int use_size_policy)
{
    size_t cap = 16, nl = 0, i, k;
    char **lines = (char **)malloc(cap * sizeof(char *));
    char *copy = (char *)malloc(n + 1);
    char *out = NULL;
    size_t dn = 0, dcap = 0;
    unsigned char *eligible_line = (unsigned char *)calloc(cap, 1);
    unsigned char *brace_mark = (unsigned char *)calloc(cap, 1);
    unsigned char *clean_exit = (unsigned char *)calloc(cap, 1);
    unsigned char *visit = (unsigned char *)calloc(cap, 1);

    memcpy(copy, text, n + 1);
    i = 0;
    while (i < n) {
        size_t s = i, e = i;
        while (e < n && copy[e] != '\n') e++;
        if (nl == cap) {
            cap *= 2;
            lines = (char **)realloc(lines, cap * sizeof(char *));
            eligible_line = (unsigned char *)realloc(eligible_line, cap);
            brace_mark = (unsigned char *)realloc(brace_mark, cap);
            clean_exit = (unsigned char *)realloc(clean_exit, cap);
            visit = (unsigned char *)realloc(visit, cap);
            memset(eligible_line + nl, 0, cap - nl);
            memset(brace_mark + nl, 0, cap - nl);
            memset(clean_exit + nl, 0, cap - nl);
            memset(visit + nl, 0, cap - nl);
        }
        lines[nl++] = copy + s;
        i = (e < n) ? e + 1 : n;
        copy[e] = 0;
    }

    for (i = 0; i < nl; i++) {
        size_t brace, end;
        if (strncmp(lines[i], "void L_", 7) || !strstr(lines[i], "(CPU"))
            continue;
        (*funcs)++;
        /* Generated corpus puts the opening brace on the next line. */
        brace = i + 1;
        if (brace >= nl || !is_arity_line(lines[brace], "{")) continue;
        end = brace + 1;
        while (end < nl && strcmp(lines[end], "}")) end++;
        if (end >= nl) continue;
        if (eligible((const char *const *)lines, i, end)) {
            size_t ops = 0, exits = 0, body_lines = end - brace + 1;
            size_t patched_lines;
            for (k = brace; k <= end; k++) {
                if (is_arith_line(lines[k])) ops++;
                if (is_exit_line(lines[k])) exits++;
            }
            /* The measured thresholds use the patched body: this stage adds
             * one BEGIN line and one END line per exit. */
            patched_lines = body_lines + 1 + exits;
            if (use_size_policy && !size_policy_ok(ops, exits, patched_lines)) {
                (*size_skipped)++;
                continue;
            }
            for (k = brace; k <= end; k++) {
                eligible_line[k] = 1;
                clean_exit[k] = 0;
            }
            mark_clean_exits((const char *const *)lines, brace, end,
                             clean_exit, visit, nl);
            brace_mark[brace] = 1;
            (*rewritten)++;
        }
    }

    for (i = 0; i < nl; i++) {
        char *line = lines[i];
        int prev_end = dn >= 32 &&
                       strstr(out + dn - 32, "WC3_X87_FPSR_END") != NULL;
        if (eligible_line[i] && !prev_end &&
            (is_arity_line(line, "return;") || is_arity_line(line, "dispatch_jmp("))) {
            const char *ed = clean_exit[i]
                                 ? "    WC3_X87_FPSR_END_CLEAN(c);\n"
                                 : "    WC3_X87_FPSR_END(c);\n";
            size_t el = strlen(ed);
            if (dn + el + 1 > dcap) {
                while (dn + el + 1 > dcap) dcap = dcap * 2 + 4096;
                out = (char *)realloc(out, dcap);
            }
            memcpy(out + dn, ed, el);
            dn += el;
        }
        if (eligible_line[i]) {
            static const char *const ops[][2] = {
                {"x87_add(", "x87_add_fpsr("}, {"x87_sub(", "x87_sub_fpsr("},
                {"x87_mul(", "x87_mul_fpsr("}, {"x87_div(", "x87_div_fpsr("}
            };
            size_t f, olen;
            char *owned = NULL;
            for (f = 0; f < 4; f++) {
                char *r = replace_all(line, ops[f][0], ops[f][1]);
                if (owned) free(owned);
                owned = r;
                line = r;
            }
            olen = strlen(line);
            if (dn + olen + 2 > dcap) {
                while (dn + olen + 2 > dcap) dcap = dcap * 2 + 4096;
                out = (char *)realloc(out, dcap);
            }
            memcpy(out + dn, line, olen);
            dn += olen;
            if (owned) free(owned);
        } else {
            size_t olen = strlen(line);
            if (dn + olen + 2 > dcap) {
                while (dn + olen + 2 > dcap) dcap = dcap * 2 + 4096;
                out = (char *)realloc(out, dcap);
            }
            memcpy(out + dn, line, olen);
            dn += olen;
        }
        if (dn + 2 > dcap) { while (dn + 2 > dcap) dcap = dcap * 2 + 4096; out = (char *)realloc(out, dcap); }
        out[dn++] = '\n';
        out[dn] = 0;
        if (brace_mark[i]) {
            const char *beg = "    WC3_X87_FPSR_BEGIN(c);\n";
            size_t bl = strlen(beg);
            if (dn + bl + 1 > dcap) {
                while (dn + bl + 1 > dcap) dcap = dcap * 2 + 4096;
                out = (char *)realloc(out, dcap);
            }
            memcpy(out + dn, beg, bl);
            dn += bl;
            out[dn] = 0;
        }
    }
    write_file(out_path, out, dn);
    free(out);
    free(copy);
    free(lines);
    free(eligible_line);
    free(brace_mark);
    free(clean_exit);
    free(visit);
}

int main(int argc, char **argv)
{
    const char *inputs = NULL, *out_dir = NULL, *report = NULL;
    int quiet = 0, want_self_test = 0, use_size_policy = 1, i;
    char *text;
    size_t n, count = 0, funcs = 0, rewritten = 0, size_skipped = 0;
    FILE *in;
    char line[2048];
    FILE *rep;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--inputs") && i + 1 < argc) inputs = argv[++i];
        else if (!strcmp(argv[i], "--out-dir") && i + 1 < argc) out_dir = argv[++i];
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) report = argv[++i];
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "--no-size-policy")) use_size_policy = 0;
        else if (!strcmp(argv[i], "--self-test")) want_self_test = 1;
        else die("unknown argument");
    }
    if (want_self_test)
        return self_test() ? 0 : 1;
    if (!inputs || !out_dir) die("--inputs and --out-dir are required");
    in = fopen(inputs, "rb");
    if (!in) die(inputs);
    while (fgets(line, sizeof line, in)) {
        char *p = line, *outp;
        size_t pl;
        while (*p == ' ' || *p == '\t') p++;
        pl = strlen(p);
        while (pl && (p[pl - 1] == '\n' || p[pl - 1] == '\r')) p[--pl] = 0;
        if (!pl) continue;
        text = read_file(p, &n);
        outp = (char *)malloc(strlen(out_dir) + strlen(path_base(p)) + 4);
        sprintf(outp, "%s/%s", out_dir, path_base(p));
        rewrite(text, n, outp, &funcs, &rewritten, &size_skipped,
                use_size_policy);
        free(outp);
        free(text);
        count++;
    }
    fclose(in);
    if (!count) die("no inputs");
    if (report) {
        rep = fopen(report, "wb");
        if (!rep) die(report);
        fprintf(rep,
                "{\"verdict\":\"pass\",\"inputs\":%zu,\"functions\":%zu,"
                "\"rewritten\":%zu,\"size_skipped\":%zu,"
                "\"size_policy\":\"line_budget_10_8_or_one_op_two_ends\","
                "\"engine\":\"x87_fpsr\"}\n",
                count, funcs, rewritten, size_skipped);
        fclose(rep);
    }
    if (!quiet)
        printf("x87_fpsr: %zu inputs, %zu functions, %zu rewritten, "
               "%zu skipped by size policy\n",
               count, funcs, rewritten, size_skipped);
    return 0;
}
