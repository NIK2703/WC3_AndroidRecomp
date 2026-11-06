/*
 * hle - persistent HLE-gap ledger for the Warcraft III host.
 *
 * The probe ledger answers "which guest target is new?".  This answers the
 * other recurring question: "which imported API is still unimplemented, and
 * where was it first seen?".  It parses `[hle] UNIMPLEMENTED ...` lines and
 * keeps a small TSV database so a later session does not have to grep every
 * run log again.
 *
 * Commands:
 *   update --db FILE --log FILE... [--note TEXT]
 *   report --db FILE [--status STATUS] [--limit N] [--fail-missing]
 *   mark API --db FILE --status STATUS [--note TEXT]
 *   stats --db FILE
 *
 * Statuses: missing, stub, implemented, ignored.
 * Read-only commands never rewrite the database.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>

#define MAX_LINE 8192
#define MAX_API 128
#define MAX_STATUS 24
#define MAX_NOTE 200
#define MAX_FILE 260
#define MAX_FIELDS 10

typedef struct {
    char api[MAX_API];
    char status[MAX_STATUS];
    char first_seen[32];
    char last_seen[32];
    unsigned long long seen;
    uint32_t last_ret, last_gid;
    char last_log[MAX_FILE];
    char note[MAX_NOTE];
} Ent;

typedef struct {
    Ent *v;
    size_t n, cap;
} EntVec;

static void die(const char *msg) {
    fprintf(stderr, "hle: %s\n", msg);
    exit(1);
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}

static void sanitize(char *s) {
    for (; *s; s++)
        if (*s == '\t' || *s == '\r' || *s == '\n') *s = ' ';
}

static void now_iso(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (!tm || !strftime(buf, n, "%Y-%m-%dT%H:%M:%S", tm))
        snprintf(buf, n, "unknown");
}

static int split_fields(char *line, char **f, int maxf) {
    int n = 0;
    char *p = line;
    while (n < maxf) {
        f[n++] = p;
        char *tab = strchr(p, '\t');
        if (!tab) break;
        *tab = 0;
        p = tab + 1;
    }
    return n;
}

static Ent *find_ent(EntVec *v, const char *api) {
    for (size_t i = 0; i < v->n; i++)
        if (strcmp(v->v[i].api, api) == 0) return &v->v[i];
    return NULL;
}

static Ent *add_ent(EntVec *v, const char *api) {
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 256;
        v->v = (Ent *)xrealloc(v->v, cap * sizeof(Ent));
        v->cap = cap;
    }
    Ent *e = &v->v[v->n++];
    memset(e, 0, sizeof(*e));
    snprintf(e->api, sizeof(e->api), "%s", api);
    snprintf(e->status, sizeof(e->status), "missing");
    return e;
}

static void load_db(const char *path, EntVec *v) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char *fld[MAX_FIELDS] = {0};
        int nf = split_fields(line, fld, MAX_FIELDS);
        if (nf < 2) continue;
        if (strcmp(fld[0], "api") == 0) continue;
        if (find_ent(v, fld[0])) continue;
        Ent *e = add_ent(v, fld[0]);
        if (nf > 1) snprintf(e->status, sizeof(e->status), "%s", fld[1]);
        if (nf > 2) snprintf(e->first_seen, sizeof(e->first_seen), "%s", fld[2]);
        if (nf > 3) snprintf(e->last_seen, sizeof(e->last_seen), "%s", fld[3]);
        if (nf > 4) e->seen = strtoull(fld[4], NULL, 10);
        if (nf > 5) e->last_ret = (uint32_t)strtoul(fld[5], NULL, 16);
        if (nf > 6) e->last_gid = (uint32_t)strtoul(fld[6], NULL, 16);
        if (nf > 7) snprintf(e->last_log, sizeof(e->last_log), "%s", fld[7]);
        if (nf > 8) snprintf(e->note, sizeof(e->note), "%s", fld[8]);
    }
    fclose(f);
}

static int cmp_ent(const void *pa, const void *pb) {
    const Ent *a = (const Ent *)pa;
    const Ent *b = (const Ent *)pb;
    int pa_rank = strcmp(a->status, "missing") == 0 ? 0 :
                  strcmp(a->status, "stub") == 0 ? 1 :
                  strcmp(a->status, "implemented") == 0 ? 2 :
                  strcmp(a->status, "ignored") == 0 ? 3 : 4;
    int pb_rank = strcmp(b->status, "missing") == 0 ? 0 :
                  strcmp(b->status, "stub") == 0 ? 1 :
                  strcmp(b->status, "implemented") == 0 ? 2 :
                  strcmp(b->status, "ignored") == 0 ? 3 : 4;
    if (pa_rank != pb_rank) return pa_rank - pb_rank;
    if (a->seen != b->seen) return a->seen > b->seen ? -1 : 1;
    return strcmp(a->api, b->api);
}

static void save_db(const char *path, EntVec *v) {
    qsort(v->v, v->n, sizeof(Ent), cmp_ent);
    FILE *f = fopen(path, "w");
    if (!f) die("cannot write database");
    fprintf(f, "# hle v1\n");
    fprintf(f, "api\tstatus\tfirst_seen\tlast_seen\tseen\tlast_ret\tlast_gid\tlast_log\tnote\n");
    for (size_t i = 0; i < v->n; i++) {
        Ent *e = &v->v[i];
        sanitize(e->api);
        sanitize(e->status);
        sanitize(e->first_seen);
        sanitize(e->last_seen);
        sanitize(e->last_log);
        sanitize(e->note);
        fprintf(f, "%s\t%s\t%s\t%s\t%llu\t%08X\t%08X\t%s\t%s\n",
                e->api, e->status, e->first_seen, e->last_seen, e->seen,
                e->last_ret, e->last_gid, e->last_log, e->note);
    }
    fclose(f);
}

static int valid_status(const char *s) {
    return strcmp(s, "missing") == 0 || strcmp(s, "stub") == 0 ||
           strcmp(s, "implemented") == 0 || strcmp(s, "ignored") == 0;
}

static void update_log(EntVec *v, const char *log) {
    FILE *f = fopen(log, "r");
    if (!f) {
        fprintf(stderr, "hle: cannot open log %s\n", log);
        return;
    }
    char line[MAX_LINE];
    char ts[32];
    now_iso(ts, sizeof(ts));
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "[hle] UNIMPLEMENTED ");
        if (!p) continue;
        p += strlen("[hle] UNIMPLEMENTED ");
        char api[MAX_API];
        size_t n = 0;
        while (*p && !isspace((unsigned char)*p) && n + 1 < sizeof(api))
            api[n++] = *p++;
        api[n] = 0;
        if (!api[0]) continue;
        uint32_t ret = 0, gid = 0;
        char *q = strstr(p, "ret=");
        if (q) ret = (uint32_t)strtoul(q + 4, NULL, 16);
        q = strstr(p, "gid=");
        if (q) gid = (uint32_t)strtoul(q + 4, NULL, 16);
        Ent *e = find_ent(v, api);
        if (!e) e = add_ent(v, api);
        if (strcmp(e->status, "missing") != 0 &&
            strcmp(e->status, "new") != 0 &&
            strcmp(e->status, "stub") != 0 &&
            strcmp(e->status, "implemented") != 0 &&
            strcmp(e->status, "ignored") != 0)
            snprintf(e->status, sizeof(e->status), "missing");
        if (e->seen == 0) snprintf(e->first_seen, sizeof(e->first_seen), "%s", ts);
        snprintf(e->last_seen, sizeof(e->last_seen), "%s", ts);
        e->seen++;
        e->last_ret = ret;
        e->last_gid = gid;
        snprintf(e->last_log, sizeof(e->last_log), "%s", log);
    }
    fclose(f);
}

static void print_report(EntVec *v, const char *status, size_t limit, int fail_missing) {
    qsort(v->v, v->n, sizeof(Ent), cmp_ent);
    size_t counts[5] = {0};
    for (size_t i = 0; i < v->n; i++) {
        int r = strcmp(v->v[i].status, "missing") == 0 ? 0 :
                strcmp(v->v[i].status, "stub") == 0 ? 1 :
                strcmp(v->v[i].status, "implemented") == 0 ? 2 :
                strcmp(v->v[i].status, "ignored") == 0 ? 3 : 4;
        counts[r]++;
    }
    printf("== hle ==\n");
    printf("entries=%zu missing=%zu stub=%zu implemented=%zu ignored=%zu other=%zu\n",
           v->n, counts[0], counts[1], counts[2], counts[3], counts[4]);
    printf("\n%-32s %-12s %8s %-10s %s\n", "api", "status", "seen", "last_ret", "last_log");
    size_t shown = 0;
    for (size_t i = 0; i < v->n; i++) {
        Ent *e = &v->v[i];
        if (status && strcmp(status, e->status) != 0) continue;
        if (shown < limit) {
            printf("%-32s %-12s %8llu %08X   %s\n",
                   e->api, e->status, e->seen, e->last_ret, e->last_log);
            if (e->note[0]) printf("  note: %s\n", e->note);
        }
        shown++;
    }
    if (!shown) printf("  (none)\n");
    else if (shown > limit) printf("  ... %zu more\n", shown - limit);
    if (fail_missing && counts[0]) {
        fprintf(stderr, "hle: %zu missing API(s)\n", counts[0]);
        exit(1);
    }
}

static void usage(void) {
    fprintf(stderr,
            "usage: hle <command> [options]\n"
            "  update --db FILE --log FILE... [--note TEXT]\n"
            "  report --db FILE [--status missing|stub|implemented|ignored] [--limit N] [--fail-missing]\n"
            "  mark API --db FILE --status STATUS [--note TEXT]\n"
            "  stats --db FILE\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1];

    if (strcmp(cmd, "update") == 0) {
        const char *db = NULL, *note = NULL;
        const char *logs[64];
        int nlogs = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) db = argv[++i];
            else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
                if (nlogs < 64) logs[nlogs++] = argv[++i];
            } else if (strcmp(argv[i], "--note") == 0 && i + 1 < argc) note = argv[++i];
            else { fprintf(stderr, "hle: unknown option %s\n", argv[i]); return 2; }
        }
        if (!db || !nlogs) { usage(); return 2; }
        EntVec v = {0};
        load_db(db, &v);
        for (int i = 0; i < nlogs; i++) update_log(&v, logs[i]);
        if (note) {
            for (size_t i = 0; i < v.n; i++)
                if (!v.v[i].note[0])
                    snprintf(v.v[i].note, sizeof(v.v[i].note), "%s", note);
        }
        save_db(db, &v);
        printf("== hle update ==\n");
        printf("db=%s logs=%d entries=%zu\n", db, nlogs, v.n);
        free(v.v);
        return 0;
    }

    if (strcmp(cmd, "report") == 0 || strcmp(cmd, "stats") == 0) {
        const char *db = NULL, *status = NULL;
        size_t limit = 80;
        int fail_missing = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) db = argv[++i];
            else if (strcmp(argv[i], "--status") == 0 && i + 1 < argc) status = argv[++i];
            else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) limit = (size_t)strtoul(argv[++i], NULL, 0);
            else if (strcmp(argv[i], "--fail-missing") == 0) fail_missing = 1;
            else { fprintf(stderr, "hle: unknown option %s\n", argv[i]); return 2; }
        }
        if (!db) { usage(); return 2; }
        EntVec v = {0};
        load_db(db, &v);
        if (strcmp(cmd, "stats") == 0) {
            size_t counts[5] = {0};
            for (size_t i = 0; i < v.n; i++) {
                int r = strcmp(v.v[i].status, "missing") == 0 ? 0 :
                        strcmp(v.v[i].status, "stub") == 0 ? 1 :
                        strcmp(v.v[i].status, "implemented") == 0 ? 2 :
                        strcmp(v.v[i].status, "ignored") == 0 ? 3 : 4;
                counts[r]++;
            }
            printf("== hle stats ==\ndb=%s entries=%zu missing=%zu stub=%zu implemented=%zu ignored=%zu other=%zu\n",
                   db, v.n, counts[0], counts[1], counts[2], counts[3], counts[4]);
        } else {
            print_report(&v, status, limit, fail_missing);
        }
        free(v.v);
        return 0;
    }

    if (strcmp(cmd, "mark") == 0) {
        if (argc < 3) { usage(); return 2; }
        const char *api = argv[2];
        const char *db = NULL, *status = NULL, *note = NULL;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) db = argv[++i];
            else if (strcmp(argv[i], "--status") == 0 && i + 1 < argc) status = argv[++i];
            else if (strcmp(argv[i], "--note") == 0 && i + 1 < argc) note = argv[++i];
            else { fprintf(stderr, "hle: unknown option %s\n", argv[i]); return 2; }
        }
        if (!db || !status || !valid_status(status)) {
            fprintf(stderr, "hle: need --db and a valid --status\n");
            return 2;
        }
        EntVec v = {0};
        load_db(db, &v);
        Ent *e = find_ent(&v, api);
        if (!e) {
            e = add_ent(&v, api);
            now_iso(e->first_seen, sizeof(e->first_seen));
        }
        snprintf(e->status, sizeof(e->status), "%s", status);
        if (note) snprintf(e->note, sizeof(e->note), "%s", note);
        save_db(db, &v);
        printf("marked %s as %s in %s\n", api, status, db);
        free(v.v);
        return 0;
    }

    usage();
    return 2;
}
