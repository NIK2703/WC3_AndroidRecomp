/*
 * watch - summarize WC3_WATCH and WC31_WATCH / MEMWATCH logs.
 *
 * The host emits one sample per watched dispatch and optional memory-change
 * events.  There are two host generations and the tool reads both:
 *
 *   [watch31] target=.. call#.. esp=.. ra=..          (1.31 host)
 *   [watch31]   stk: .. .. .. .. .. ..
 *   [watch] target=.. call#.. esp=.. ra=.. ecx=..
 *           [+20]=.. [+24]=..                         (1.26/1.27 host)
 *
 * The 1.27 line carries the __thiscall object and the first two fields of a
 * list walker.  A field that is non-zero but not 4-byte aligned (or below the
 * image) cannot be a frame/object pointer; the tool counts those separately
 * and prints the first one, which is usually the exact object that crashed the
 * walk.  The tool collapses a multi-megabyte log into:
 *   - call count and distinct return addresses per watch target
 *   - first/last ecx and [+20]/[+24] (1.27 format)
 *   - suspicious (odd or implausibly low) pointers in those fields
 *   - whether the six stack dwords changed at all
 *   - memory-watch first/last values and change count
 *   - the crash line and the last HLE line
 *
 * It does not attach to a process and does not execute guest code.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define MAX_LINE 8192
#define MAX_RA 256
#define MAX_STK 8

typedef struct {
    uint32_t target;
    uint64_t calls;
    uint64_t first_call, last_call;
    uint32_t first_ra, last_ra;
    uint32_t ra[MAX_RA];
    unsigned nra;
    uint32_t first_stk[MAX_STK], last_stk[MAX_STK];
    unsigned nstk;
    uint64_t first_stk_hash, last_stk_hash;
    unsigned distinct_stk;
    /* 1.27 [watch] target=... ecx=... [+20]=... [+24]=... */
    int fmt27;
    uint32_t first_ecx, last_ecx;
    uint32_t first_f20, last_f20, first_f24, last_f24;
    int have_f20, have_f24;
    uint64_t bad_fields;
    uint64_t first_bad_call;
    uint32_t first_bad_ecx;
    int first_bad_which;             /* 20 or 24 */
    uint32_t first_bad_value;
} Watch;

typedef struct {
    Watch *v;
    size_t n, cap;
} WatchVec;

typedef struct {
    uint32_t va;
    uint64_t samples, changes;
    uint32_t first, last;
    uint32_t last_target;
    uint64_t last_call;
} Mem;

typedef struct {
    Mem *v;
    size_t n, cap;
} MemVec;

static void die(const char *msg) {
    fprintf(stderr, "watch: %s\n", msg);
    exit(1);
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}

static uint64_t stk_hash(const uint32_t *s, unsigned n) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned i = 0; i < n; i++) {
        h ^= s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static Watch *watch_find(WatchVec *v, uint32_t target) {
    for (size_t i = 0; i < v->n; i++)
        if (v->v[i].target == target) return &v->v[i];
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 32;
        v->v = (Watch *)xrealloc(v->v, cap * sizeof(Watch));
        v->cap = cap;
    }
    Watch *w = &v->v[v->n++];
    memset(w, 0, sizeof(*w));
    w->target = target;
    return w;
}

static Mem *mem_find(MemVec *v, uint32_t va) {
    for (size_t i = 0; i < v->n; i++)
        if (v->v[i].va == va) return &v->v[i];
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 32;
        v->v = (Mem *)xrealloc(v->v, cap * sizeof(Mem));
        v->cap = cap;
    }
    Mem *m = &v->v[v->n++];
    memset(m, 0, sizeof(*m));
    m->va = va;
    return m;
}

static void watch_ra(Watch *w, uint32_t ra) {
    for (unsigned i = 0; i < w->nra; i++)
        if (w->ra[i] == ra) return;
    if (w->nra < MAX_RA) w->ra[w->nra++] = ra;
}

static void watch_sample(WatchVec *v, uint32_t target, uint64_t call,
                         uint32_t ra, uint32_t esp) {
    (void)esp;
    Watch *w = watch_find(v, target);
    if (!w->calls) {
        w->first_call = call;
        w->first_ra = ra;
    }
    w->calls++;
    w->last_call = call;
    w->last_ra = ra;
    watch_ra(w, ra);
}

static void watch_stk(Watch *w, const uint32_t *s, unsigned n) {
    uint64_t h = stk_hash(s, n);
    if (!w->nstk) {
        w->first_stk_hash = h;
        w->last_stk_hash = h;
        w->nstk = n;
        memcpy(w->first_stk, s, n * sizeof(uint32_t));
        memcpy(w->last_stk, s, n * sizeof(uint32_t));
        w->distinct_stk = 1;
        return;
    }
    if (h != w->last_stk_hash) {
        w->last_stk_hash = h;
        memcpy(w->last_stk, s, n * sizeof(uint32_t));
        w->distinct_stk++;
    }
}

static uint32_t g_last_watch;
static int g_have_last_watch;
static char g_last_hle[1024];
static char g_crash[1024];

/* A frame/list pointer in this game is at least 4-byte aligned and lives in
 * the image or a heap above the 64 KiB null page.  A non-zero value that fails
 * either test is a mis-typed field, not a pointer; the 1.27 host prints these
 * before the walk dereferences them. */
static int suspect_ptr(uint32_t v) {
    return v != 0 && ((v & 3u) != 0 || v < 0x00010000u);
}

static void watch_field27(Watch *w, int which, uint32_t v, uint32_t ecx,
                          uint64_t call) {
    if (which == 20) {
        if (!w->have_f20) { w->first_f20 = v; w->have_f20 = 1; }
        w->last_f20 = v;
    } else {
        if (!w->have_f24) { w->first_f24 = v; w->have_f24 = 1; }
        w->last_f24 = v;
    }
    if (suspect_ptr(v)) {
        if (!w->bad_fields) {
            w->first_bad_call = call;
            w->first_bad_ecx = ecx;
            w->first_bad_which = which;
            w->first_bad_value = v;
        }
        w->bad_fields++;
    }
}

static void parse_line(char *line, WatchVec *wv, MemVec *mv) {
    if (strstr(line, "[watch31] target=")) {
        unsigned target, call, esp, ra;
        if (sscanf(line, "[watch31] target=%x call#%u esp=%x ra=%x",
                   &target, &call, &esp, &ra) == 4) {
            watch_sample(wv, target, call, ra, esp);
            g_last_watch = target;
            g_have_last_watch = 1;
        }
    } else if (strstr(line, "[watch31]   stk:")) {
        unsigned s[6];
        if (g_have_last_watch &&
            sscanf(line, "[watch31]   stk: %x %x %x %x %x %x",
                   &s[0], &s[1], &s[2], &s[3], &s[4], &s[5]) == 6) {
            Watch *w = watch_find(wv, g_last_watch);
            watch_stk(w, s, 6);
        }
    } else if (strstr(line, "[watch] target=")) {
        unsigned target, call, esp, ra, ecx, f20, f24;
        int n = sscanf(line,
                       "[watch] target=%x call#%u esp=%x ra=%x ecx=%x"
                       " [+20]=%x [+24]=%x",
                       &target, &call, &esp, &ra, &ecx, &f20, &f24);
        if (n >= 5) {
            Watch *w = watch_find(wv, target);
            watch_sample(wv, target, call, ra, esp);
            w->fmt27 = 1;
            if (w->calls == 1) w->first_ecx = ecx;
            w->last_ecx = ecx;
            if (n >= 6) watch_field27(w, 20, f20, ecx, call);
            if (n >= 7) watch_field27(w, 24, f24, ecx, call);
            g_last_watch = target;
            g_have_last_watch = 1;
        }
    } else if (strstr(line, "[memwatch]")) {
        unsigned target, va, value, oldv, esp;
        if (sscanf(line, "[memwatch] init target=%x va=%x value=%x esp=%x",
                   &target, &va, &value, &esp) >= 3) {
            Mem *m = mem_find(mv, va);
            m->samples++;
            m->first = value;
            m->last = value;
            m->last_target = target;
        } else if (sscanf(line, "[memwatch] change target=%x va=%x old=%x new=%x esp=%x",
                          &target, &va, &oldv, &value, &esp) >= 4) {
            Mem *m = mem_find(mv, va);
            m->samples++;
            m->changes++;
            if (m->samples == 1) m->first = oldv;
            m->last = value;
            m->last_target = target;
        }
    } else if (strstr(line, "[host31] CRASH")) {
        snprintf(g_crash, sizeof(g_crash), "%.*s",
                 (int)sizeof(g_crash) - 1, line);
        size_t n = strlen(g_crash);
        while (n && (g_crash[n - 1] == '\n' || g_crash[n - 1] == '\r'))
            g_crash[--n] = 0;
    }
    if (strstr(line, "[hle]") || strstr(line, "[hle31]")) {
        snprintf(g_last_hle, sizeof(g_last_hle), "%.*s",
                 (int)sizeof(g_last_hle) - 1, line);
        size_t n = strlen(g_last_hle);
        while (n && (g_last_hle[n - 1] == '\n' || g_last_hle[n - 1] == '\r'))
            g_last_hle[--n] = 0;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: watch <log>... [--target VA] [--top N]\n"
                "  Summarizes WC31_WATCH and WC31_MEMWATCH samples.\n");
        return 2;
    }
    uint32_t target_filter = 0;
    int have_filter = 0;
    size_t top = 50;
    const char *files[64];
    int nfiles = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            char *end = NULL;
            target_filter = (uint32_t)strtoul(argv[++i], &end, 16);
            if (!argv[i][0] || (end && *end)) die("bad --target");
            have_filter = 1;
        } else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            top = (size_t)strtoul(argv[++i], NULL, 0);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "watch: unknown option %s\n", argv[i]);
            return 2;
        } else if (nfiles < (int)(sizeof(files) / sizeof(files[0]))) {
            files[nfiles++] = argv[i];
        } else {
            die("too many log files");
        }
    }
    if (!nfiles) die("missing log file");

    WatchVec wv = {0};
    MemVec mv = {0};
    unsigned long long lines = 0;
    for (int i = 0; i < nfiles; i++) {
        FILE *f = fopen(files[i], "r");
        if (!f) {
            fprintf(stderr, "watch: cannot open %s\n", files[i]);
            continue;
        }
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), f)) {
            lines++;
            parse_line(line, &wv, &mv);
        }
        fclose(f);
    }

    printf("== watch ==\n");
    printf("logs=%d lines=%llu  watch_targets=%zu  memwatch_addrs=%zu\n",
           nfiles, lines, wv.n, mv.n);

    if (g_crash[0]) printf("crash: %s\n", g_crash);
    if (g_last_hle[0]) printf("last HLE: %s\n", g_last_hle);

    printf("\nwatch targets:\n");
    size_t shown = 0;
    for (size_t i = 0; i < wv.n; i++) {
        Watch *w = &wv.v[i];
        if (have_filter && w->target != target_filter) continue;
        if (top && shown >= top) break;
        printf("  %08X calls=%llu unique_ra=%u distinct_stk=%u\n",
               w->target, w->calls, w->nra, w->distinct_stk);
        printf("      first_call=%llu first_ra=%08X last_call=%llu last_ra=%08X\n",
               w->first_call, w->first_ra, w->last_call, w->last_ra);
        if (w->fmt27) {
            printf("      ecx first=%08X last=%08X\n", w->first_ecx, w->last_ecx);
            if (w->have_f20)
                printf("      [+20] first=%08X last=%08X\n", w->first_f20, w->last_f20);
            if (w->have_f24)
                printf("      [+24] first=%08X last=%08X\n", w->first_f24, w->last_f24);
            if (w->bad_fields)
                printf("      BAD FIELDS: %llu (first call#%llu ecx=%08X [+%d]=%08X)\n",
                       (unsigned long long)w->bad_fields,
                       (unsigned long long)w->first_bad_call, w->first_bad_ecx,
                       w->first_bad_which, w->first_bad_value);
            else
                printf("      fields: all non-zero [+20]/[+24] are aligned pointers\n");
        }
        if (w->nstk) {
            printf("      first_stk:");
            for (unsigned k = 0; k < w->nstk; k++) printf(" %08X", w->first_stk[k]);
            printf("\n      last_stk:");
            for (unsigned k = 0; k < w->nstk; k++) printf(" %08X", w->last_stk[k]);
            printf("\n");
            if (w->first_stk_hash == w->last_stk_hash)
                printf("      stack: STALLED (same six dwords in every sample)\n");
            else
                printf("      stack: CHANGED (%u distinct sampled tuples)\n", w->distinct_stk);
        }
        shown++;
    }
    if (!wv.n)
        printf("  none (no [watch]/[watch31] lines; set WC3_WATCH or WC31_WATCH=<va>)\n");
    else if (top && shown >= top && shown < wv.n)
        printf("  ... %zu more (use --top 0)\n", wv.n - shown);

    printf("\nmemwatch:\n");
    shown = 0;
    for (size_t i = 0; i < mv.n; i++) {
        Mem *m = &mv.v[i];
        if (top && shown >= top) break;
        printf("  %08X samples=%llu changes=%llu first=%08X last=%08X last_target=%08X\n",
               m->va, m->samples, m->changes, m->first, m->last, m->last_target);
        if (m->changes == 0)
            printf("      value: STALLED\n");
        else
            printf("      value: CHANGED %08X -> %08X\n", m->first, m->last);
        shown++;
    }
    if (!mv.n) printf("  none (set WC31_MEMWATCH=<va>[,<va>...])\n");
    else if (top && shown >= top && shown < mv.n)
        printf("  ... %zu more\n", mv.n - shown);

    printf("\nverdict:\n");
    int changed_stk = 0, changed_mem = 0, ongoing_mem = 0, have_stk = 0;
    uint64_t bad_total = 0;
    for (size_t i = 0; i < wv.n; i++) {
        if (wv.v[i].nstk) {
            have_stk = 1;
            if (wv.v[i].first_stk_hash != wv.v[i].last_stk_hash)
                changed_stk = 1;
        }
        bad_total += wv.v[i].bad_fields;
    }
    for (size_t i = 0; i < mv.n; i++) {
        if (mv.v[i].changes) changed_mem = 1;
        if (mv.v[i].changes > 1) ongoing_mem = 1;
    }
    if (!wv.n && !mv.n)
        printf("  no watch data; enable WC3_WATCH/WC31_WATCH or WC3_MEMWATCH/WC31_MEMWATCH.\n");
    else if (ongoing_mem)
        printf("  memory watch is progressing; inspect the change targets/values above.\n");
    else if (changed_mem)
        printf("  only initialization changes seen; no ongoing memory progression.\n");
    else if (bad_total)
        printf("  %llu watched field(s) are not pointers; the first BAD FIELD above is\n"
               "  the object that breaks the walk.\n", (unsigned long long)bad_total);
    else if (wv.n && have_stk && !changed_stk)
        printf("  watch stack is stable; the loop is likely not advancing its visible frame.\n");
    else
        printf("  watch data is present; inspect the per-target counters.\n");

    free(wv.v);
    free(mv.v);
    return 0;
}
