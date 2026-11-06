/*
 * ledger - persistent probe-target ledger for the 1.31 lift loop.
 *
 * The ledger remembers every target seen in probe files, when it was first
 * and last seen, which probe produced it, and whether it is now present in
 * a catalog header or a generated lift source.  It prevents the "is this
 * new?" manual diff on every probe round and gives the next session a target
 * history.
 *
 * Commands:
 *   update --db FILE --probe FILE... [--known-dir DIR] [--known-prefix P]
 *   report --db FILE [--status STATUS] [--limit N] [--fail-new]
 *   mark ADDR --db FILE --status STATUS [--note TEXT]
 *   stats --db FILE
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <windows.h>

#define MAX_LINE 8192
#define MAX_FILE 260
#define MAX_NOTE 160
#define MAX_STATUS 24
#define MAX_PROBES 64

typedef struct {
    uint32_t addr;
    char status[MAX_STATUS];
    char first_seen[32];
    char last_seen[32];
    unsigned long long seen;
    char last_probe[MAX_FILE];
    uint32_t last_ra;
    char note[MAX_NOTE];
} Ent;

typedef struct {
    Ent *v;
    size_t n, cap;
} EntVec;

typedef struct {
    uint32_t addr, ra;
    unsigned count;
} PEnt;

typedef struct {
    PEnt *v;
    size_t n, cap;
} PVec;

typedef struct {
    uint32_t *v;
    size_t n, cap;
} AddrVec;

static void die(const char *msg) {
    fprintf(stderr, "ledger: %s\n", msg);
    exit(1);
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
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

static int parse_u32_hex(const char *s, uint32_t *out) {
    char *end = NULL;
    unsigned long v;
    if (!s || !*s) return 0;
    v = strtoul(s, &end, 16);
    if (end == s || *end) return 0;
    *out = (uint32_t)v;
    return 1;
}

static void now_iso(char *buf, size_t cap) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (!tm || !strftime(buf, cap, "%Y-%m-%dT%H:%M:%S", tm))
        snprintf(buf, cap, "unknown");
}

static int cmp_addr(const void *pa, const void *pb) {
    uint32_t a = *(const uint32_t *)pa, b = *(const uint32_t *)pb;
    return (a > b) - (a < b);
}

static void addr_add(AddrVec *a, uint32_t v) {
    if (a->n == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 4096;
        a->v = (uint32_t *)xrealloc(a->v, cap * sizeof(uint32_t));
        a->cap = cap;
    }
    a->v[a->n++] = v;
}

static int addr_has(const AddrVec *a, uint32_t v) {
    size_t lo = 0, hi = a->n;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (a->v[m] < v) lo = m + 1; else hi = m;
    }
    return lo < a->n && a->v[lo] == v;
}

static void collect_file_addrs(const char *path, AddrVec *known, int source_defs) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[MAX_LINE];
    const char *needle = source_defs ? "void L_" : "X(";
    const size_t skip = source_defs ? 7 : 2;
    while (fgets(line, sizeof(line), f)) {
        const char *p = line;
        while ((p = strstr(p, needle)) != NULL) {
            p += skip;
            uint32_t a;
            if (parse_hex8(p, &a)) addr_add(known, a);
            p += 8;
        }
    }
    fclose(f);
}

static void load_known(const char *dir, const char *prefix, AddrVec *known) {
    if (!dir) return;

    /* Catalog headers are authoritative, but an extraN.c may exist before
     * its list is linked.  Count both so a freshly lifted target is not
     * reported as "new" on the next probe. */
    const char *patterns[2] = { "%s\\*_list.h", "%s\\*.c" };
    for (int pass = 0; pass < 2; pass++) {
        char pattern[MAX_PATH];
        WIN32_FIND_DATAA fd;
        snprintf(pattern, sizeof(pattern), patterns[pass], dir);
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) {
            if (pass == 0) fprintf(stderr, "ledger: no *_list.h in %s\n", dir);
            continue;
        }
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (strstr(fd.cFileName, "_stale")) continue;
            if (prefix && strncmp(fd.cFileName, prefix, strlen(prefix)) != 0) continue;
            char path[MAX_PATH + 64];
            snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
            collect_file_addrs(path, known, pass == 1);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    qsort(known->v, known->n, sizeof(uint32_t), cmp_addr);
    size_t w = 0;
    for (size_t i = 0; i < known->n; i++)
        if (!w || known->v[i] != known->v[w - 1]) known->v[w++] = known->v[i];
    known->n = w;
}

static Ent *ent_find(EntVec *v, uint32_t addr) {
    for (size_t i = 0; i < v->n; i++)
        if (v->v[i].addr == addr) return &v->v[i];
    return NULL;
}

static Ent *ent_add(EntVec *v, uint32_t addr) {
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 1024;
        v->v = (Ent *)xrealloc(v->v, cap * sizeof(Ent));
        v->cap = cap;
    }
    Ent *e = &v->v[v->n++];
    memset(e, 0, sizeof(*e));
    e->addr = addr;
    snprintf(e->status, sizeof(e->status), "new");
    return e;
}

static int cmp_ent(const void *pa, const void *pb) {
    const Ent *a = (const Ent *)pa, *b = (const Ent *)pb;
    return (a->addr > b->addr) - (a->addr < b->addr);
}

static int db_load(const char *path, EntVec *v) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (strncmp(line, "addr\t", 5) == 0) continue;
        unsigned addr, ra;
        char status[MAX_STATUS] = {0}, first[32] = {0}, last[32] = {0};
        char probe[MAX_FILE] = {0}, note[MAX_NOTE] = {0};
        unsigned long long seen = 0;
        int n = sscanf(line, "%x\t%23[^\t]\t%31[^\t]\t%31[^\t]\t%llu\t%259[^\t]\t%x\t%159[^\n]",
                       &addr, status, first, last, &seen, probe, &ra, note);
        if (n < 7) {
            n = sscanf(line, "%x\t%23[^\t]\t%31[^\t]\t%31[^\t]\t%llu\t%259[^\t]\t%x",
                       &addr, status, first, last, &seen, probe, &ra);
            note[0] = 0;
        }
        if (n < 7) continue;
        Ent *e = ent_add(v, (uint32_t)addr);
        snprintf(e->status, sizeof(e->status), "%s", status);
        snprintf(e->first_seen, sizeof(e->first_seen), "%s", first);
        snprintf(e->last_seen, sizeof(e->last_seen), "%s", last);
        e->seen = seen;
        snprintf(e->last_probe, sizeof(e->last_probe), "%s", probe);
        e->last_ra = (uint32_t)ra;
        snprintf(e->note, sizeof(e->note), "%s", note);
    }
    fclose(f);
    qsort(v->v, v->n, sizeof(Ent), cmp_ent);
    return 1;
}

static int db_save(const char *path, EntVec *v) {
    char tmp[MAX_PATH + 16];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return 0;
    fprintf(f, "# ledger v1\n");
    fprintf(f, "addr\tstatus\tfirst_seen\tlast_seen\tseen\tlast_probe\tlast_ra\tnote\n");
    qsort(v->v, v->n, sizeof(Ent), cmp_ent);
    for (size_t i = 0; i < v->n; i++) {
        Ent *e = &v->v[i];
        fprintf(f, "%08X\t%s\t%s\t%s\t%llu\t%s\t%08X\t%s\n",
                e->addr, e->status, e->first_seen, e->last_seen, e->seen,
                e->last_probe, e->last_ra, e->note);
    }
    fclose(f);
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        remove(tmp);
        return 0;
    }
    return 1;
}

static void probe_add(PVec *v, uint32_t addr, uint32_t ra) {
    for (size_t i = 0; i < v->n; i++) {
        if (v->v[i].addr == addr) {
            v->v[i].count++;
            if (!v->v[i].ra && ra) v->v[i].ra = ra;
            return;
        }
    }
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 64;
        v->v = (PEnt *)xrealloc(v->v, cap * sizeof(PEnt));
        v->cap = cap;
    }
    v->v[v->n].addr = addr;
    v->v[v->n].ra = ra;
    v->v[v->n].count = 1;
    v->n++;
}

static int probe_load(const char *path, PVec *v) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        unsigned t, ra;
        if (sscanf(line, "%x %x", &t, &ra) == 2) probe_add(v, t, ra);
        else if (sscanf(line, "%x", &t) == 1) probe_add(v, t, 0);
    }
    fclose(f);
    return 1;
}

static int cmp_probe(const void *pa, const void *pb) {
    const PEnt *a = (const PEnt *)pa, *b = (const PEnt *)pb;
    return (a->addr > b->addr) - (a->addr < b->addr);
}

static const char *arg_value(int argc, char **argv, int *i, const char *name) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "ledger: %s expects a value\n", name);
        exit(2);
    }
    return argv[++(*i)];
}

static void usage(void) {
    fprintf(stderr,
            "usage:\n"
            "  ledger update --db FILE --probe FILE [--probe FILE...] \\\n"
            "                   [--known-dir DIR] [--known-prefix P] [--note TEXT] [--dry-run]\n"
            "  ledger report --db FILE [--status STATUS] [--limit N] [--fail-new]\n"
            "  ledger mark ADDR --db FILE --status STATUS [--note TEXT]\n"
            "  ledger stats --db FILE\n"
            "statuses: new, lifted, ignored, missing\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const char *cmd = argv[1];
    const char *db_path = "reports\\lift\\war31_ledger.tsv";
    const char *known_dir = NULL;
    const char *known_prefix = "war31_lifted";
    const char *note = "";
    const char *filter_status = NULL;
    const char *mark_status = NULL;
    const char *probes[MAX_PROBES];
    int nprobes = 0;
    size_t limit = 50;
    int fail_new = 0, dry_run = 0;
    uint32_t mark_addr = 0;
    int have_mark_addr = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--db") == 0) db_path = arg_value(argc, argv, &i, "--db");
        else if (strcmp(argv[i], "--probe") == 0) {
            if (nprobes >= MAX_PROBES) die("too many --probe files");
            probes[nprobes++] = arg_value(argc, argv, &i, "--probe");
        } else if (strcmp(argv[i], "--known-dir") == 0) known_dir = arg_value(argc, argv, &i, "--known-dir");
        else if (strcmp(argv[i], "--known-prefix") == 0) known_prefix = arg_value(argc, argv, &i, "--known-prefix");
        else if (strcmp(argv[i], "--note") == 0) note = arg_value(argc, argv, &i, "--note");
        else if (strcmp(argv[i], "--status") == 0) {
            const char *v = arg_value(argc, argv, &i, "--status");
            if (strcmp(cmd, "mark") == 0) mark_status = v;
            else filter_status = v;
        } else if (strcmp(argv[i], "--limit") == 0) limit = (size_t)strtoul(arg_value(argc, argv, &i, "--limit"), NULL, 0);
        else if (strcmp(argv[i], "--fail-new") == 0) fail_new = 1;
        else if (strcmp(argv[i], "--dry-run") == 0) dry_run = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "ledger: unknown option %s\n", argv[i]);
            return 2;
        } else if (strcmp(cmd, "mark") == 0 && !have_mark_addr) {
            if (!parse_u32_hex(argv[i], &mark_addr)) {
                fprintf(stderr, "ledger: bad address %s\n", argv[i]);
                return 2;
            }
            have_mark_addr = 1;
        } else {
            fprintf(stderr, "ledger: unexpected argument %s\n", argv[i]);
            return 2;
        }
    }

    EntVec db = {0};
    int had_db = db_load(db_path, &db);
    char now[32];
    now_iso(now, sizeof(now));

    if (strcmp(cmd, "update") == 0) {
        if (!nprobes) die("update requires at least one --probe");
        AddrVec known = {0};
        load_known(known_dir, known_prefix, &known);
        size_t total_samples = 0, total_unique = 0, fresh = 0, newly_lifted = 0;
        for (int pi = 0; pi < nprobes; pi++) {
            PVec pv = {0};
            if (!probe_load(probes[pi], &pv)) {
                fprintf(stderr, "ledger: cannot open probe %s\n", probes[pi]);
                continue;
            }
            qsort(pv.v, pv.n, sizeof(PEnt), cmp_probe);
            total_unique += pv.n;
            for (size_t i = 0; i < pv.n; i++) {
                total_samples += pv.v[i].count;
                Ent *e = ent_find(&db, pv.v[i].addr);
                int is_known = addr_has(&known, pv.v[i].addr);
                if (!e) {
                    e = ent_add(&db, pv.v[i].addr);
                    snprintf(e->first_seen, sizeof(e->first_seen), "%s", now);
                    e->seen = pv.v[i].count;
                    snprintf(e->last_probe, sizeof(e->last_probe), "%s", probes[pi]);
                    e->last_ra = pv.v[i].ra;
                    snprintf(e->status, sizeof(e->status), "%s", is_known ? "lifted" : "new");
                    if (is_known) newly_lifted++;
                    else fresh++;
                } else {
                    if (strcmp(e->status, "ignored") != 0) {
                        if (is_known && strcmp(e->status, "new") == 0) {
                            snprintf(e->status, sizeof(e->status), "lifted");
                            newly_lifted++;
                        } else if (!is_known && strcmp(e->status, "lifted") == 0) {
                            snprintf(e->status, sizeof(e->status), "missing");
                        }
                    }
                    e->seen += pv.v[i].count;
                    if (pv.v[i].ra) e->last_ra = pv.v[i].ra;
                    snprintf(e->last_probe, sizeof(e->last_probe), "%s", probes[pi]);
                }
                snprintf(e->last_seen, sizeof(e->last_seen), "%s", now);
                if (*note) snprintf(e->note, sizeof(e->note), "%s", note);
            }
            free(pv.v);
        }
        printf("== ledger update ==\n");
        printf("db=%s%s\n", db_path, had_db ? "" : " (new)");
        printf("probe files=%d  unique targets=%zu  samples=%zu  known=%zu\n",
               nprobes, total_unique, total_samples, known.n);
        printf("new=%zu  newly_lifted=%zu  entries=%zu\n", fresh, newly_lifted, db.n);
        if (!dry_run) {
            if (!db_save(db_path, &db)) die("cannot save ledger");
            printf("saved.\n");
        } else {
            printf("dry-run: not saved.\n");
        }
        free(known.v);
    } else if (strcmp(cmd, "report") == 0 || strcmp(cmd, "stats") == 0) {
        if (!had_db) {
            printf("== ledger ==\n");
            printf("db=%s (missing; run update first)\n", db_path);
            free(db.v);
            return 0;
        }
        size_t nnew = 0, nlifted = 0, nignored = 0, nmissing = 0;
        for (size_t i = 0; i < db.n; i++) {
            const char *s = db.v[i].status;
            if (strcmp(s, "new") == 0) nnew++;
            else if (strcmp(s, "lifted") == 0) nlifted++;
            else if (strcmp(s, "ignored") == 0) nignored++;
            else if (strcmp(s, "missing") == 0) nmissing++;
        }
        printf("== ledger %s ==\n", cmd);
        printf("db=%s\n", db_path);
        printf("entries=%zu  new=%zu  lifted=%zu  ignored=%zu  missing=%zu\n",
               db.n, nnew, nlifted, nignored, nmissing);
        size_t shown = 0;
        if (strcmp(cmd, "report") == 0) {
            printf("\n");
            for (size_t i = 0; i < db.n; i++) {
                Ent *e = &db.v[i];
                if (filter_status && strcmp(e->status, filter_status) != 0) continue;
                if (limit && shown >= limit) break;
                printf("%08X  %-8s seen=%-6llu first=%s last=%s ra=%08X probe=%s%s%s\n",
                       e->addr, e->status, e->seen, e->first_seen, e->last_seen,
                       e->last_ra, e->last_probe,
                       e->note[0] ? " note=" : "", e->note);
                shown++;
            }
            if (limit && shown >= limit && shown < db.n)
                printf("... limit %zu reached (use --limit 0 for all)\n", limit);
        }
        if (fail_new && nnew) {
            free(db.v);
            return 1;
        }
    } else if (strcmp(cmd, "mark") == 0) {
        if (!have_mark_addr) die("mark requires an address");
        if (!mark_status) die("mark requires --status");
        Ent *e = ent_find(&db, mark_addr);
        if (!e) {
            e = ent_add(&db, mark_addr);
            snprintf(e->first_seen, sizeof(e->first_seen), "%s", now);
        }
        snprintf(e->status, sizeof(e->status), "%s", mark_status);
        snprintf(e->last_seen, sizeof(e->last_seen), "%s", now);
        if (*note) snprintf(e->note, sizeof(e->note), "%s", note);
        if (!dry_run) {
            if (!db_save(db_path, &db)) die("cannot save ledger");
        }
        printf("%08X -> %s%s\n", mark_addr, mark_status, dry_run ? " (dry-run)" : "");
    } else {
        usage();
        free(db.v);
        return 2;
    }

    free(db.v);
    return 0;
}
