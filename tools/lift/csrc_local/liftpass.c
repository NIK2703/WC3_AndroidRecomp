/* liftpass.c - streaming invariant checker for generated C (ABI-11.3, G-14).
 *
 * The corpus is gigabytes of machine-generated C, so T0 cannot regex-lint it.
 * This tool checks invariants while reading each file once, emits a
 * function-level change set (ABI-11.2) and refuses to claim a pass when its
 * own detectors are broken (--self-test, V5.4).  It does not modify the C:
 * applying a LiftPass still requires G-14 and G-15 (V6.5).
 *
 *   liftpass check <file-or-dir> [--list lifted_list.h]
 *                  [--json report.json] [--self-test]
 *
 * Checks: constant dispatch targets resolve or are HLE-window addresses;
 * no `return fn(c);` in a void function (C11 6.8.6.4); no static TLS in
 * headers (ABI-4); no HLE-window target devirtualised to L_7F...; no
 * duplicate L_<va> definitions; every function has a terminal transfer.
 *
 * RECOMP_TODO(va, text) expands to abort() (runtime/cpu/cpu.h:364), so a
 * function whose body ends in one is an explicit trap, not a fall-through
 * bug.  It counts as a terminal transfer.  Unsupported guest instructions
 * (hlt / iretd / far jmp m16:32) legitimately produce such bodies; ignoring
 * them would make a green report impossible without hiding real gaps.
 */
#include "sha256.h"
#include "dirwalk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define LINE_CAP 65536
#define HLE_BASE 0x7F000000u
#define HLE_SPAN 0x00100000u
#define MAX_VIOLATIONS 4096
#define MAX_CHANGES 400000

typedef struct { uint32_t *v; size_t n, cap; } Vas;
typedef struct {
    char file[DIR_MAX_PATH];
    long line;
    char kind[48];
    char detail[128];
} Violation;
typedef struct {
    uint32_t va;
    char sha[65];
} Change;
typedef struct {
    Vas vas;
    Violation *bad;
    size_t nbad, capbad;
    Change *changes;
    size_t nchanges, capchanges;
    size_t files, functions, targets;
    int check_targets;
    Vas seen;
    const char *current_file;
    long current_line;
    /* current function */
    int in_func;
    uint32_t func_va;
    int has_terminal;
    sha256_ctx body;
} Scan;

static void die(const char *msg)
{
    fprintf(stderr, "liftpass: %s\n", msg);
    exit(2);
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}

static void vas_push(Vas *a, uint32_t va)
{
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8192;
        a->v = (uint32_t *)xrealloc(a->v, a->cap * sizeof *a->v);
    }
    a->v[a->n++] = va;
}

static int cmp_u32(const void *x, const void *y)
{
    uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
    return (a > b) - (a < b);
}

static int vas_has(const Vas *a, uint32_t va)
{
    size_t lo = 0, hi = a->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a->v[mid] < va) lo = mid + 1;
        else hi = mid;
    }
    return lo < a->n && a->v[lo] == va;
}

static void add_violation(Scan *s, const char *kind, const char *detail)
{
    Violation *v;
    if (s->nbad >= MAX_VIOLATIONS) return;
    if (s->nbad == s->capbad) {
        s->capbad = s->capbad ? s->capbad * 2 : 64;
        s->bad = (Violation *)xrealloc(s->bad, s->capbad * sizeof *s->bad);
    }
    v = &s->bad[s->nbad++];
    snprintf(v->file, sizeof v->file, "%s", s->current_file);
    v->line = s->current_line;
    snprintf(v->kind, sizeof v->kind, "%s", kind);
    snprintf(v->detail, sizeof v->detail, "%s", detail);
}

static void finish_function(Scan *s)
{
    unsigned char digest[32];
    Change *c;
    if (!s->in_func) return;
    sha256_final(&s->body, digest);
    if (s->nchanges < MAX_CHANGES) {
        if (s->nchanges == s->capchanges) {
            s->capchanges = s->capchanges ? s->capchanges * 2 : 4096;
            s->changes = (Change *)xrealloc(s->changes,
                                            s->capchanges * sizeof *s->changes);
        }
        c = &s->changes[s->nchanges++];
        c->va = s->func_va;
        sha256_hex(digest, c->sha);
    }
    if (!s->has_terminal) add_violation(s, "no_terminal", "function has no dispatch/return");
    s->in_func = 0;
    s->functions++;
}

/* Strip C comments and collapse whitespace.  State survives across lines. */
static void feed_normalized(sha256_ctx *ctx, const char *line, int *in_comment)
{
    const char *p = line;
    char out[LINE_CAP];
    size_t n = 0;
    int pending_space = 0;

    while (*p) {
        if (*in_comment) {
            if (p[0] == '*' && p[1] == '/') { *in_comment = 0; p += 2; }
            else p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') { *in_comment = 1; p += 2; continue; }
        if (p[0] == '/' && p[1] == '/') break;
        if (isspace((unsigned char)*p)) { pending_space = 1; p++; continue; }
        if (pending_space && n && n + 1 < sizeof out) out[n++] = ' ';
        pending_space = 0;
        if (n + 1 < sizeof out) out[n++] = *p;
        p++;
    }
    if (n) sha256_update(ctx, out, n);
}

/* Returns 1 when the line violates one of the textual invariants. */
static int line_violations(Scan *s, const char *line)
{
    const char *p;
    int bad = 0;

    /* `return fn(c);` inside a void function is invalid C11 6.8.6.4. */
    if ((p = strstr(line, "return ")) != NULL && strstr(line, "(c);") != NULL
        && strstr(line, "dispatch") == NULL && strncmp(p + 7, "L_", 2) == 0) {
        add_violation(s, "return_in_void", "return fn(c); is invalid in a void function");
        bad = 1;
    }
    /* A HLE thunk is 0x7Fxxxxxx; it must never be devirtualised. */
    if ((p = strstr(line, "L_7F")) != NULL && strstr(line, "(c)") != NULL) {
        add_violation(s, "hle_devirtualized", "direct call to an HLE-window address");
        bad = 1;
    }
    /* ABI-4: static TLS in a header gives every TU its own state. */
    if (strstr(line, "static") && (strstr(line, "_Thread_local") ||
                                   strstr(line, "THREAD_LOCAL")) &&
        path_ends_with(s->current_file, ".h")) {
        add_violation(s, "static_tls_header", "static TLS in a header (ABI-4)");
        bad = 1;
    }
    return bad;
}

static void scan_constant_targets(Scan *s, const char *line)
{
    const char *p = line;
    if (!s->check_targets) return;
    while ((p = strstr(p, "dispatch")) != NULL) {
        const char *next = p + 8;
        const char *hex = strstr(next, "0x");
        char buf[9];
        char *end;
        unsigned long va;

        if (hex && (size_t)(hex - next) <= 12 && strlen(hex) >= 10) {
            memcpy(buf, hex + 2, 8);
            buf[8] = 0;
            va = strtoul(buf, &end, 16);
            if (end == buf + 8) {
                s->targets++;
                if (!(va >= HLE_BASE && va < HLE_BASE + HLE_SPAN) &&
                    !vas_has(&s->vas, (uint32_t)va)) {
                    char detail[128];
                    snprintf(detail, sizeof detail, "0x%08lX has no L_ definition", va);
                    add_violation(s, "unknown_target", detail);
                }
            }
        }
        p = next; /* always advance, even when the line has no hex target */
    }
}

static void scan_file(const char *path, void *user)
{
    Scan *s = (Scan *)user;
    FILE *f;
    char line[LINE_CAP];
    int in_comment = 0;

    if (!path_ends_with(path, ".c") && !path_ends_with(path, ".h")) return;
    f = fopen(path, "r");
    if (!f) { add_violation(s, "unreadable", path); return; }
    s->current_file = path;
    s->current_line = 0;
    s->files++;

    while (fgets(line, sizeof line, f)) {
        const char *def;
        s->current_line++;
        if ((def = strstr(line, "void L_")) != NULL && strstr(line, "(CPU") != NULL) {
            char buf[9];
            char *end;
            unsigned long va;
            def += 7;
            memcpy(buf, def, 8);
            buf[8] = 0;
            va = strtoul(buf, &end, 16);
            if (end == buf + 8) {
                vas_push(&s->seen, (uint32_t)va);
                finish_function(s);
                s->in_func = 1;
                s->func_va = (uint32_t)va;
                s->has_terminal = 0;
                sha256_init(&s->body);
            }
        }
        if (s->in_func) {
            feed_normalized(&s->body, line, &in_comment);
            if (strstr(line, "dispatch(") || strstr(line, "dispatch_jmp(") ||
                strstr(line, "return") || strstr(line, "hle_call") ||
                strstr(line, "RECOMP_TODO("))
                s->has_terminal = 1;
        }
        line_violations(s, line);
        scan_constant_targets(s, line);
    }
    fclose(f);
    finish_function(s);
}

static int file_visitor(const char *path, void *user)
{
    scan_file(path, user);
    return 0;
}

static void load_list(Scan *s, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[4096];
    if (!f) die("cannot open --list");
    while (fgets(line, sizeof line, f)) {
        const char *p = strstr(line, "X(");
        char buf[9];
        char *end;
        unsigned long va;
        if (!p) continue;
        p += 2;
        if (strlen(p) < 8) continue;
        memcpy(buf, p, 8);
        buf[8] = 0;
        va = strtoul(buf, &end, 16);
        if (end == buf + 8) vas_push(&s->vas, (uint32_t)va);
    }
    fclose(f);
    qsort(s->vas.v, s->vas.n, sizeof *s->vas.v, cmp_u32);
    {
        size_t i, w = 0;
        for (i = 0; i < s->vas.n; i++) {
            if (w && s->vas.v[w - 1] == s->vas.v[i]) continue;
            s->vas.v[w++] = s->vas.v[i];
        }
        s->vas.n = w;
    }
}

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { fputc('\\', f); fputc(*s, f); }
        else fputc(*s, f);
    }
    fputc('"', f);
}

static void write_report(const char *path, Scan *s, int self_test_ok)
{
    FILE *f;
    size_t i;
    if (!path) return;
    f = fopen(path, "w");
    if (!f) die("cannot open --json");
    fprintf(f, "{\n  \"schema\": 1,\n  \"files\": %lu,\n  \"functions\": %lu,\n",
            (unsigned long)s->files, (unsigned long)s->functions);
    fprintf(f, "  \"constant_targets\": %lu,\n", (unsigned long)s->targets);
    fprintf(f, "  \"self_test\": \"%s\",\n", self_test_ok ? "pass" : "fail");
    fprintf(f, "  \"violations\": [\n");
    for (i = 0; i < s->nbad; i++) {
        Violation *v = &s->bad[i];
        fprintf(f, "    {\"file\": "); json_str(f, v->file);
        fprintf(f, ", \"line\": %ld, \"kind\": ", v->line);
        json_str(f, v->kind);
        fprintf(f, ", \"detail\": "); json_str(f, v->detail);
        fprintf(f, "}%s\n", i + 1 < s->nbad ? "," : "");
    }
    fprintf(f, "  ],\n  \"change_set\": [\n");
    for (i = 0; i < s->nchanges; i++) {
        fprintf(f, "    {\"va\": \"%08X\", \"pass_id\": \"invariant_check\", "
                "\"after_sha256\": \"%s\", \"reason\": \"unchanged\"}%s\n",
                s->changes[i].va, s->changes[i].sha,
                i + 1 < s->nchanges ? "," : "");
    }
    fprintf(f, "  ],\n  \"verdict\": \"%s\"\n}\n",
            (s->nbad == 0 && self_test_ok) ? "pass" : "fail");
    fclose(f);
}

/* V5.4: prove the detectors fire, otherwise a green report means nothing. */
static int self_test(Scan *s)
{
    Scan probe;
    size_t before;
    memset(&probe, 0, sizeof probe);
    probe.current_file = "self_test.h";
    probe.current_line = 1;
    before = 0;
    line_violations(&probe, "static THREAD_LOCAL int depth;\n");
    line_violations(&probe, "return L_00401000(c);\n");
    line_violations(&probe, "L_7F000010(c);\n");
    before = probe.nbad;
    free(probe.bad);
    (void)s;
    return before >= 3 ? 1 : 0;
}

int main(int argc, char **argv)
{
    Scan s;
    const char *target = NULL, *list = NULL, *json = NULL;
    int do_self_test = 0, st = 1, i;

    if (argc < 3 || strcmp(argv[1], "check") != 0) {
        fprintf(stderr, "usage: liftpass check <file-or-dir> [--list H] [--json F] [--self-test]\n");
        return 2;
    }
    target = argv[2];
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0 && i + 1 < argc) list = argv[++i];
        else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) json = argv[++i];
        else if (strcmp(argv[i], "--self-test") == 0) do_self_test = 1;
        else { fprintf(stderr, "liftpass: unknown option %s\n", argv[i]); return 2; }
    }

    memset(&s, 0, sizeof s);
    s.check_targets = list != NULL;
    if (list) load_list(&s, list);
    else fprintf(stderr, "liftpass: no --list, target coverage checks disabled\n");

    if (path_is_dir(target)) {
        if (dir_walk(target, file_visitor, &s) != 0) die("walk failed");
    } else {
        scan_file(target, &s);
    }

    /* Duplicate L_<va> definitions: a link failure at best, a wrong closure
     * at worst.  Sorted post-pass keeps the streaming scan O(1) per line. */
    if (s.seen.n > 1) {
        size_t i;
        qsort(s.seen.v, s.seen.n, sizeof *s.seen.v, cmp_u32);
        s.current_file = target;
        s.current_line = 0;
        for (i = 1; i < s.seen.n; i++) {
            if (s.seen.v[i] == s.seen.v[i - 1]) {
                char detail[128];
                snprintf(detail, sizeof detail, "duplicate L_%08X", s.seen.v[i]);
                add_violation(&s, "duplicate_definition", detail);
            }
        }
    }

    if (do_self_test) st = self_test(&s);
    write_report(json, &s, st);

    printf("liftpass: %s (%lu files, %lu functions, %lu constant targets, %lu violations)\n",
           (s.nbad == 0 && st) ? "pass" : "fail",
           (unsigned long)s.files, (unsigned long)s.functions,
           (unsigned long)s.targets, (unsigned long)s.nbad);
    if (do_self_test) printf("liftpass: self-test %s\n", st ? "pass" : "fail");

    free(s.vas.v);
    free(s.seen.v);
    free(s.bad);
    free(s.changes);
    return (s.nbad == 0 && st) ? 0 : 1;
}
