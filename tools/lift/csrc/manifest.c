/*
 * manifest - audit the generated lift catalogs.
 *
 * Scans reports/lift/<catalog>.c and *_list.h, then reports:
 *   - addresses defined in more than one .c file
 *   - list entries without a definition
 *   - definitions that are not in any list header (orphans)
 *   - RECOMP_TODO density per file
 *
 * Read-only by default. --write-canonical emits a deduplicated list header.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <windows.h>

#define MAX_LINE 8192
#define MAX_FILE 260

typedef struct {
    uint32_t addr;
    char file[MAX_FILE];
} Rec;

typedef struct {
    Rec *v;
    size_t n, cap;
} RecVec;

typedef struct {
    char file[MAX_FILE];
    unsigned count;
} TodoRec;

typedef struct {
    TodoRec *v;
    size_t n, cap;
} TodoVec;

static void die(const char *msg) {
    fprintf(stderr, "manifest: %s\n", msg);
    exit(1);
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}

static void rec_add(RecVec *v, uint32_t addr, const char *file) {
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 1024;
        v->v = (Rec *)xrealloc(v->v, cap * sizeof(Rec));
        v->cap = cap;
    }
    v->v[v->n].addr = addr;
    snprintf(v->v[v->n].file, MAX_FILE, "%s", file);
    v->n++;
}

static int cmp_rec(const void *pa, const void *pb) {
    const Rec *a = (const Rec *)pa;
    const Rec *b = (const Rec *)pb;
    if (a->addr != b->addr) return (a->addr > b->addr) - (a->addr < b->addr);
    return strcmp(a->file, b->file);
}

static int ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (m > n) return 0;
    return _stricmp(s + n - m, suffix) == 0;
}

static int parse_hex8(const char *p, uint32_t *out) {
    for (int i = 0; i < 8; i++)
        if (!isxdigit((unsigned char)p[i])) return 0;
    char tmp[9];
    memcpy(tmp, p, 8);
    tmp[8] = 0;
    *out = (uint32_t)strtoul(tmp, NULL, 16);
    return 1;
}

static void todo_add(TodoVec *t, const char *file, unsigned count) {
    if (!count) return;
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 16;
        t->v = (TodoRec *)xrealloc(t->v, cap * sizeof(TodoRec));
        t->cap = cap;
    }
    snprintf(t->v[t->n].file, MAX_FILE, "%s", file);
    t->v[t->n].count = count;
    t->n++;
}

static int cmp_todo(const void *pa, const void *pb) {
    const TodoRec *a = (const TodoRec *)pa;
    const TodoRec *b = (const TodoRec *)pb;
    if (a->count != b->count) return (a->count < b->count) ? 1 : -1;
    return strcmp(a->file, b->file);
}

static void scan_source(const char *path, const char *name, RecVec *defs, TodoVec *todos) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[MAX_LINE];
    unsigned todo = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "void L_");
        while (p) {
            p += strlen("void L_");
            uint32_t a;
            if (parse_hex8(p, &a) && p[8] == '(') rec_add(defs, a, name);
            p = strstr(p, "void L_");
        }
        const char *q = line;
        while ((q = strstr(q, "RECOMP_TODO")) != NULL) {
            todo++;
            q += strlen("RECOMP_TODO");
        }
    }
    fclose(f);
    todo_add(todos, name, todo);
}

static void scan_list(const char *path, const char *name, RecVec *lists) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "X(");
        while (p) {
            p += 2;
            uint32_t a;
            if (parse_hex8(p, &a)) rec_add(lists, a, name);
            p = strstr(p, "X(");
        }
    }
    fclose(f);
}

static int has_addr(const RecVec *v, uint32_t a) {
    size_t lo = 0, hi = v->n;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (v->v[m].addr < a) lo = m + 1; else hi = m;
    }
    return lo < v->n && v->v[lo].addr == a;
}

/* Count unique files for one address group in a sorted RecVec. */
static unsigned group_files(Rec *v, size_t n, size_t start, char files[][MAX_FILE], unsigned maxfiles) {
    unsigned nf = 0;
    for (size_t i = start; i < n; i++) {
        int seen = 0;
        for (unsigned k = 0; k < nf; k++) {
            if (strcmp(files[k], v[i].file) == 0) { seen = 1; break; }
        }
        if (!seen && nf < maxfiles)
            snprintf(files[nf++], MAX_FILE, "%s", v[i].file);
    }
    return nf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: manifest <lift-dir> [options]\n"
                "  --write-canonical FILE  write a deduplicated list header\n"
                "  --strict                exit 1 on duplicates or missing definitions\n"
                "  --prefix P              only scan files whose name starts with P\n"
                "  --active-list FILE      only audit addresses present in this list header\n"
                "  --top N                 report limit (default 40)\n");
        return 2;
    }

    const char *dir = argv[1];
    const char *canonical = NULL;
    const char *prefix = NULL;
    const char *active_path = NULL;
    int strict = 0;
    size_t top = 40;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--write-canonical") == 0 && i + 1 < argc) {
            canonical = argv[++i];
        } else if (strcmp(argv[i], "--strict") == 0) {
            strict = 1;
        } else if (strcmp(argv[i], "--prefix") == 0 && i + 1 < argc) {
            prefix = argv[++i];
        } else if (strcmp(argv[i], "--active-list") == 0 && i + 1 < argc) {
            active_path = argv[++i];
        } else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            top = (size_t)strtoul(argv[++i], NULL, 0);
        } else {
            fprintf(stderr, "manifest: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    RecVec active = {0};
    if (active_path) {
        scan_list(active_path, "active", &active);
        qsort(active.v, active.n, sizeof(Rec), cmp_rec);
    }

    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) die("cannot enumerate lift directory");

    RecVec defs = {0}, lists = {0};
    TodoVec todos = {0};
    unsigned nsrc = 0, nlist = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char *name = fd.cFileName;
        if (strstr(name, "_stale") || ends_with_ci(name, ".bak")) continue;
        if (prefix && strncmp(name, prefix, strlen(prefix)) != 0) continue;
        int is_list = ends_with_ci(name, "_list.h");
        int is_src = !is_list && ends_with_ci(name, ".c");
        if (!is_src && !is_list) continue;
        char path[MAX_PATH + 64];
        snprintf(path, sizeof(path), "%s\\%s", dir, name);
        if (is_src) {
            scan_source(path, name, &defs, &todos);
            nsrc++;
        } else {
            scan_list(path, name, &lists);
            nlist++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    qsort(defs.v, defs.n, sizeof(Rec), cmp_rec);
    qsort(lists.v, lists.n, sizeof(Rec), cmp_rec);

    size_t unique_defs = 0, unique_lists = 0;
    for (size_t i = 0; i < defs.n; i++)
        if (!i || defs.v[i].addr != defs.v[i - 1].addr) unique_defs++;
    for (size_t i = 0; i < lists.n; i++)
        if (!i || lists.v[i].addr != lists.v[i - 1].addr) unique_lists++;

    printf("== manifest ==\n");
    printf("dir: %s\n", dir);
    printf("sources=%u lists=%u\n", nsrc, nlist);
    printf("definitions: records=%zu unique=%zu\n", defs.n, unique_defs);
    printf("list entries: records=%zu unique=%zu\n", lists.n, unique_lists);
    if (active.n) printf("active list: %zu addresses (audit filtered)\n", active.n);

    /* Duplicate definitions: the same address in more than one .c file. */
    size_t dup_count = 0;
    printf("\nduplicate definitions (same address in >1 source file):\n");
    for (size_t i = 0; i < defs.n; ) {
        size_t j = i + 1;
        while (j < defs.n && defs.v[j].addr == defs.v[i].addr) j++;
        uint32_t a = defs.v[i].addr;
        if (active.n && !has_addr(&active, a)) { i = j; continue; }
        char files[16][MAX_FILE];
        unsigned nf = group_files(defs.v, j, i, files, 16);
        if (nf > 1) {
            dup_count++;
            if (dup_count <= top) {
                printf("  %08X:", defs.v[i].addr);
                for (unsigned k = 0; k < nf; k++) printf(" %s", files[k]);
                printf("\n");
            }
        }
        i = j;
    }
    if (!dup_count) printf("  none\n");
    else if (dup_count > top) printf("  ... %zu more\n", dup_count - top);

    /* Missing definitions: listed but no source defines it. */
    size_t missing = 0;
    printf("\nmissing definitions (listed but no L_ function):\n");
    for (size_t i = 0; i < lists.n; ) {
        size_t j = i + 1;
        while (j < lists.n && lists.v[j].addr == lists.v[i].addr) j++;
        size_t lo = 0, hi = defs.n;
        uint32_t a = lists.v[i].addr;
        if (active.n && !has_addr(&active, a)) { i = j; continue; }
        while (lo < hi) {
            size_t m = lo + (hi - lo) / 2;
            if (defs.v[m].addr < a) lo = m + 1; else hi = m;
        }
        if (lo >= defs.n || defs.v[lo].addr != a) {
            missing++;
            if (missing <= top) printf("  %08X in %s\n", a, lists.v[i].file);
        }
        i = j;
    }
    if (!missing) printf("  none\n");
    else if (missing > top) printf("  ... %zu more\n", missing - top);

    /* Orphans: defined but not present in any list header. */
    size_t orphans = 0;
    printf("\norphan definitions (not in any list header):\n");
    for (size_t i = 0; i < defs.n; ) {
        size_t j = i + 1;
        while (j < defs.n && defs.v[j].addr == defs.v[i].addr) j++;
        size_t lo = 0, hi = lists.n;
        uint32_t a = defs.v[i].addr;
        if (active.n && !has_addr(&active, a)) { i = j; continue; }
        while (lo < hi) {
            size_t m = lo + (hi - lo) / 2;
            if (lists.v[m].addr < a) lo = m + 1; else hi = m;
        }
        if (lo >= lists.n || lists.v[lo].addr != a) {
            orphans++;
            if (orphans <= top) printf("  %08X in %s\n", a, defs.v[i].file);
        }
        i = j;
    }
    if (!orphans) printf("  none\n");
    else if (orphans > top) printf("  ... %zu more\n", orphans - top);

    qsort(todos.v, todos.n, sizeof(TodoRec), cmp_todo);
    printf("\nRECOMP_TODO density:\n");
    if (!todos.n) printf("  none\n");
    for (size_t i = 0; i < todos.n && i < 12; i++)
        printf("  %-40s %u\n", todos.v[i].file, todos.v[i].count);

    if (canonical) {
        FILE *f = fopen(canonical, "w");
        if (!f) die("cannot write canonical header");
        fprintf(f, "/* generated by manifest - deduplicated, defined entries only */\n");
        size_t written = 0, skipped = 0;
        RecVec *src = active.n ? &active : &lists;
        for (size_t i = 0; i < src->n; ) {
            size_t j = i + 1;
            while (j < src->n && src->v[j].addr == src->v[i].addr) j++;
            uint32_t a = src->v[i].addr;
            size_t lo = 0, hi = defs.n;
            while (lo < hi) {
                size_t m = lo + (hi - lo) / 2;
                if (defs.v[m].addr < a) lo = m + 1; else hi = m;
            }
            if (lo < defs.n && defs.v[lo].addr == a) {
                if (written == 0) fprintf(f, "#define LIFTED_FUNCS(X) \\\n");
                else fprintf(f, " \\\n");
                fprintf(f, "    X(%08X)", a);
                written++;
            } else {
                skipped++;
            }
            i = j;
        }
        if (written) fprintf(f, "\n");
        else fprintf(f, "/* no defined list entries */\n");
        fprintf(f, "\n/* %zu entries, %zu missing skipped */\n", written, skipped);
        fclose(f);
        printf("\ncanonical: wrote %zu entries (%zu missing skipped) -> %s\n",
               written, skipped, canonical);
    }

    printf("\nverdict:\n");
    if (dup_count || missing) {
        printf("  BUILD RISK: %zu duplicate definition address(es), %zu missing.\n",
               dup_count, missing);
        printf("  Move conflicting catalogs to a stale/ directory and rebuild the list.\n");
    } else {
        printf("  CONSISTENT: no duplicate definitions and no missing entries.\n");
    }
    if (orphans)
        printf("  NOTE: %zu definition(s) are not linked by any list header.\n", orphans);

    free(defs.v); free(lists.v); free(active.v); free(todos.v);
    return (strict && (dup_count || missing)) ? 1 : 0;
}
