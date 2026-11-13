/* atomiccheck.c - reject non-atomic lowering of guest xchg / lock RMW.
 *
 * The lifter writes one comment per guest instruction:
 *
 *   ... atomic_xchg32((c->ebx), _v);  /* 6F78EF82: xchg dword ptr [ebx], eax *\/
 *
 * A memory xchg is implicitly locked, and every lock-prefixed read-modify-
 * write is one atomic operation.  The generated C must use the matching
 * atomic_* helper.  This checker does not modify the corpus: it fails the
 * build when the invariant is broken, so a stale lift tool cannot silently
 * reintroduce the save freeze (V5.4, V5.9, ABI-11).
 *
 * Usage:
 *   atomiccheck [--file <generated.c>]... [--corpus-root <dir>]
 *               [--report <json>] [--require-any]
 *
 * Exit codes: 0 pass, 1 violation, 2 unreadable input / usage.
 */
#include "dirwalk.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_CAP (1u << 20)
#define MAX_DETAIL 200

typedef enum {
    WANT_NONE = 0,
    WANT_ANY,
    WANT_XCHG32,
    WANT_XADD32,
    WANT_CMPXCHG32,
    WANT_CMPXCHG64
} Want;

enum {
    TOK_XCHG32 = 1u << 0,
    TOK_XADD32 = 1u << 1,
    TOK_CMPXCHG32 = 1u << 2,
    TOK_CMPXCHG64 = 1u << 3,
    TOK_OTHER = 1u << 4
};

typedef struct {
    uint32_t va;
    Want want;
    unsigned mask;
    int line;
    int plain;
    int conflict;
    char ins[192];
} Rec;

typedef struct {
    char file[DIR_MAX_PATH];
    long line;
    uint32_t va;
    char kind[32];
    char ins[192];
} Detail;

typedef struct {
    Rec *rec;
    size_t nrec, caprec;
    Detail detail[MAX_DETAIL];
    size_t ndetail;
    size_t files, records, atomic_ops, violations;
    int file_errors;
    char *line;
} Scan;

static unsigned want_mask(Want w)
{
    switch (w) {
    case WANT_XCHG32:    return TOK_XCHG32;
    case WANT_XADD32:    return TOK_XADD32;
    case WANT_CMPXCHG32: return TOK_CMPXCHG32;
    case WANT_CMPXCHG64: return TOK_CMPXCHG64;
    case WANT_ANY:       return TOK_XCHG32 | TOK_XADD32 |
                                TOK_CMPXCHG32 | TOK_CMPXCHG64 | TOK_OTHER;
    default:             return 0;
    }
}

static int is_dword_mem(const char *ins)
{
    return strstr(ins, "dword ptr [") != NULL;
}

/* Classify the instruction text after "ADDR: ".  Only memory operands are
 * interesting; register-only xchg and ordinary arithmetic are not atomic
 * memory operations. */
static Want classify(const char *ins)
{
    if (strstr(ins, "ptr [") == NULL)
        return WANT_NONE;

    if (strncmp(ins, "xchg ", 5) == 0)
        return is_dword_mem(ins) ? WANT_XCHG32 : WANT_ANY;

    if (strncmp(ins, "lock ", 5) == 0) {
        const char *m = ins + 5;
        if (strncmp(m, "cmpxchg8b", 9) == 0)
            return WANT_CMPXCHG64;
        if (strncmp(m, "cmpxchg", 7) == 0)
            return is_dword_mem(ins) ? WANT_CMPXCHG32 : WANT_ANY;
        if (strncmp(m, "xadd", 4) == 0)
            return is_dword_mem(ins) ? WANT_XADD32 : WANT_ANY;
        if (strncmp(m, "xchg", 4) == 0)
            return is_dword_mem(ins) ? WANT_XCHG32 : WANT_ANY;
        if (strncmp(m, "inc", 3) == 0 || strncmp(m, "dec", 3) == 0 ||
            strncmp(m, "add", 3) == 0)
            return is_dword_mem(ins) ? WANT_XADD32 : WANT_ANY;
        if (strncmp(m, "sub", 3) == 0)
            return is_dword_mem(ins) ? WANT_ANY : WANT_NONE;
        /* bts/btr/btc and any future locked memory RMW also need an
         * atomic helper; reject a plain lowering even if this checker does
         * not know the exact helper name yet. */
        return WANT_ANY;
    }

    return WANT_NONE;
}

/* Parse the generated instruction comment.  It must have exactly eight hex
 * digits, a colon and a non-empty instruction; the address is the guest VA. */
static int parse_comment(const char *line, uint32_t *va, Want *want,
                         char *ins, size_t cap)
{
    const char *c = strstr(line, "/*");
    const char *p;
    char *end = NULL;
    unsigned long a;
    char *e;
    int i;

    if (!c)
        return 0;
    c += 2;
    while (*c == ' ' || *c == '\t')
        c++;
    for (i = 0; i < 8; i++) {
        if (!isxdigit((unsigned char)c[i]))
            return 0;
    }
    if (c[8] != ':')
        return 0;

    a = strtoul(c, &end, 16);
    if (end != c + 8 || a > 0xFFFFFFFFul)
        return 0;
    p = end + 1;
    while (*p == ' ' || *p == '\t')
        p++;
    *want = classify(p);
    if (*want == WANT_NONE)
        return 0;
    *va = (uint32_t)a;
    snprintf(ins, cap, "%s", p);
    e = strstr(ins, " */");
    if (e)
        *e = 0;
    return 1;
}

static unsigned atomic_mask(const char *line)
{
    unsigned m = 0;
    if (strstr(line, "atomic_xchg32"))
        m |= TOK_XCHG32;
    if (strstr(line, "atomic_xadd32"))
        m |= TOK_XADD32;
    if (strstr(line, "atomic_cmpxchg32"))
        m |= TOK_CMPXCHG32;
    if (strstr(line, "atomic_cmpxchg64"))
        m |= TOK_CMPXCHG64;
    if (strstr(line, "atomic_") && m == 0)
        m |= TOK_OTHER;
    return m;
}

static Rec *rec_for(Scan *s, uint32_t va, Want want, const char *ins, int line)
{
    Rec *r;
    size_t i;

    for (i = 0; i < s->nrec; i++) {
        if (s->rec[i].va == va) {
            r = &s->rec[i];
            if (r->want != WANT_NONE && r->want != want)
                r->conflict = 1;
            return r;
        }
    }

    if (s->nrec == s->caprec) {
        size_t next = s->caprec ? s->caprec * 2 : 64;
        Rec *p = (Rec *)realloc(s->rec, next * sizeof *p);
        if (!p) {
            fprintf(stderr, "atomiccheck: out of memory\n");
            exit(2);
        }
        s->rec = p;
        s->caprec = next;
    }
    r = &s->rec[s->nrec++];
    memset(r, 0, sizeof *r);
    r->va = va;
    r->want = want;
    r->line = line;
    snprintf(r->ins, sizeof r->ins, "%s", ins);
    return r;
}

static void add_violation(Scan *s, const char *file, long line, uint32_t va,
                          const char *kind, const char *ins)
{
    Detail *d;

    s->violations++;
    if (s->ndetail < MAX_DETAIL) {
        d = &s->detail[s->ndetail++];
        snprintf(d->file, sizeof d->file, "%s", file);
        d->line = line;
        d->va = va;
        snprintf(d->kind, sizeof d->kind, "%s", kind);
        snprintf(d->ins, sizeof d->ins, "%s", ins);
    }
    fprintf(stderr, "atomiccheck: %s:%ld: %08X: %s: %s\n",
            file, line, va, kind, ins);
}

/* Scan one generated C file.  Records are per-file: the same guest address may
 * appear in several functions, so all comment lines at that address must agree
 * on the atomic helper.  Returns -1 on read error, 1 on violations, 0 pass. */
static int scan_file(Scan *s, const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t start_viol = s->violations;
    size_t i;
    long lineno = 0;

    if (!f) {
        fprintf(stderr, "atomiccheck: cannot read %s\n", path);
        s->file_errors++;
        return -1;
    }

    s->nrec = 0;
    while (fgets(s->line, (int)LINE_CAP, f)) {
        uint32_t va;
        Want want;
        char ins[192];
        Rec *r;
        lineno++;
        if (!parse_comment(s->line, &va, &want, ins, sizeof ins))
            continue;
        r = rec_for(s, va, want, ins, (int)lineno);
        if (r->want == WANT_NONE)
            r->want = want;
        r->mask |= atomic_mask(s->line);
        if (strstr(s->line, "rd32(") || strstr(s->line, "wr32("))
            r->plain = 1;
    }

    if (ferror(f)) {
        fprintf(stderr, "atomiccheck: read failed: %s\n", path);
        fclose(f);
        s->file_errors++;
        return -1;
    }
    fclose(f);

    s->files++;
    s->records += s->nrec;
    for (i = 0; i < s->nrec; i++) {
        Rec *r = &s->rec[i];
        if (r->mask)
            s->atomic_ops++;
        if (r->conflict)
            add_violation(s, path, r->line, r->va, "conflicting_instruction", r->ins);
        else if (r->plain)
            add_violation(s, path, r->line, r->va, "non_atomic_lowering", r->ins);
        else if (r->want == WANT_ANY) {
            if (r->mask == 0)
                add_violation(s, path, r->line, r->va,
                              "missing_atomic_operation", r->ins);
        } else if ((r->mask & want_mask(r->want)) == 0) {
            if (r->mask)
                add_violation(s, path, r->line, r->va,
                              "wrong_atomic_helper", r->ins);
            else
                add_violation(s, path, r->line, r->va,
                              "missing_atomic_operation", r->ins);
        }
    }

    return s->violations > start_viol ? 1 : 0;
}

static int visit_file(const char *path, void *user)
{
    Scan *s = (Scan *)user;
    if (!path_ends_with(path, ".c"))
        return 0;
    return scan_file(s, path) < 0 ? 1 : 0;
}

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (c < 0x20)
                fprintf(f, "\\u%04x", (unsigned)c);
            else
                fputc(c, f);
            break;
        }
    }
    fputc('"', f);
}

static int write_report(const char *path, Scan *s, int require_any)
{
    FILE *f = fopen(path, "wb");
    size_t i;
    int pass = s->file_errors == 0 && s->violations == 0 &&
               (!require_any || s->records > 0);

    if (!f) {
        fprintf(stderr, "atomiccheck: cannot write %s\n", path);
        return -1;
    }

    fprintf(f, "{\n  \"schema\": 1,\n");
    fprintf(f, "  \"verdict\": \"%s\",\n", pass ? "pass" : "fail");
    fprintf(f, "  \"files\": %lu,\n", (unsigned long)s->files);
    fprintf(f, "  \"records\": %lu,\n", (unsigned long)s->records);
    fprintf(f, "  \"atomic_ops\": %lu,\n", (unsigned long)s->atomic_ops);
    fprintf(f, "  \"violations\": %lu,\n", (unsigned long)s->violations);
    fprintf(f, "  \"require_any\": %s,\n", require_any ? "true" : "false");
    fprintf(f, "  \"details_truncated\": %s,\n",
            s->violations > s->ndetail ? "true" : "false");
    fprintf(f, "  \"details\": [");
    for (i = 0; i < s->ndetail; i++) {
        Detail *d = &s->detail[i];
        fprintf(f, "%s\n    {\"file\": ", i ? "," : "");
        json_str(f, d->file);
        fprintf(f, ", \"line\": %ld, \"address\": \"%08X\", \"kind\": ",
                d->line, d->va);
        json_str(f, d->kind);
        fprintf(f, ", \"instruction\": ");
        json_str(f, d->ins);
        fputc('}', f);
    }
    fprintf(f, "%s]\n}\n", s->ndetail ? "\n  " : "");
    fclose(f);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: atomiccheck [--file <generated.c>]... "
            "[--corpus-root <dir>] [--report <json>] [--require-any]\n");
}

int main(int argc, char **argv)
{
    static Scan s;
    const char **files;
    const char *root = NULL;
    const char *report = NULL;
    int nfiles = 0;
    int require_any = 0;
    int walk_rc;
    int i;
    int pass;

    files = (const char **)calloc((size_t)argc, sizeof *files);
    s.line = (char *)malloc(LINE_CAP);
    if (!files || !s.line) {
        fprintf(stderr, "atomiccheck: out of memory\n");
        return 2;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            files[nfiles++] = argv[++i];
        } else if (strcmp(argv[i], "--corpus-root") == 0 && i + 1 < argc) {
            root = argv[++i];
        } else if (strcmp(argv[i], "--report") == 0 && i + 1 < argc) {
            report = argv[++i];
        } else if (strcmp(argv[i], "--require-any") == 0) {
            require_any = 1;
        } else {
            usage();
            return 2;
        }
    }

    if (nfiles == 0 && !root) {
        usage();
        return 2;
    }

    for (i = 0; i < nfiles; i++)
        (void)scan_file(&s, files[i]);

    if (root) {
        walk_rc = dir_walk(root, visit_file, &s);
        if (walk_rc != 0 && s.file_errors == 0) {
            fprintf(stderr, "atomiccheck: cannot walk %s\n", root);
            s.file_errors++;
        }
    }

    if (report && write_report(report, &s, require_any) != 0)
        return 2;

    if (s.records == 0 && require_any)
        fprintf(stderr, "atomiccheck: no memory xchg/lock instruction comments found\n");

    pass = s.file_errors == 0 && s.violations == 0 &&
           (!require_any || s.records > 0);
    printf("atomiccheck: %s (%lu files, %lu atomic instructions, "
           "%lu violations)\n",
           pass ? "pass" : "fail",
           (unsigned long)s.files, (unsigned long)s.records,
           (unsigned long)s.violations);

    if (s.file_errors)
        return 2;
    return pass ? 0 : 1;
}
