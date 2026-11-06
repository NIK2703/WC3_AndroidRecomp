/*
 * triage - summarize a Warcraft III recompiler host run.
 *
 * Parses the host stderr log (dispatch ring, probe file, HLE gaps, traps,
 * crashes) and prints a compact, decision-oriented report.
 *
 * Build: see build.ps1 in this directory.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define MAX_LINE 8192

typedef struct {
    unsigned long long n;
    uint32_t target, esp, ebp;
} TraceEnt;

typedef struct {
    uint32_t target, ra;
    unsigned count;
} AddrCount;

typedef struct {
    AddrCount *v;
    size_t n, cap;
} AddrVec;

typedef struct {
    uint32_t addr;
    char text[256];
    unsigned count;
} MsgEnt;

typedef struct {
    MsgEnt *v;
    size_t n, cap;
} MsgVec;

static TraceEnt *g_trace;
static size_t g_ntrace, g_captrace;
static AddrVec g_unknown, g_probe;
static MsgVec g_todo, g_unimpl;
static unsigned long long g_lines;
static int g_crashes;
static uint32_t g_crash_code, g_crash_eip, g_crash_esp, g_crash_ebp;
static char g_crash_line[8192];
static unsigned g_key_window, g_key_d3d, g_key_dxgi, g_key_itdone, g_key_entryret;
static unsigned g_key_fastfail, g_key_client_sdk, g_key_pause;

static void die(const char *msg) {
    fprintf(stderr, "triage: %s\n", msg);
    exit(1);
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}

static void trace_add(unsigned long long n, uint32_t target, uint32_t esp, uint32_t ebp) {
    if (g_ntrace == g_captrace) {
        size_t cap = g_captrace ? g_captrace * 2 : 4096;
        g_trace = (TraceEnt *)xrealloc(g_trace, cap * sizeof(TraceEnt));
        g_captrace = cap;
    }
    g_trace[g_ntrace].n = n;
    g_trace[g_ntrace].target = target;
    g_trace[g_ntrace].esp = esp;
    g_trace[g_ntrace].ebp = ebp;
    g_ntrace++;
}

static void addr_add(AddrVec *a, uint32_t target, uint32_t ra) {
    for (size_t i = 0; i < a->n; i++) {
        if (a->v[i].target == target) {
            a->v[i].count++;
            if (!a->v[i].ra && ra) a->v[i].ra = ra;
            return;
        }
    }
    if (a->n == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 64;
        a->v = (AddrCount *)xrealloc(a->v, cap * sizeof(AddrCount));
        a->cap = cap;
    }
    a->v[a->n].target = target;
    a->v[a->n].ra = ra;
    a->v[a->n].count = 1;
    a->n++;
}

static void msg_add(MsgVec *m, const char *text, uint32_t addr) {
    for (size_t i = 0; i < m->n; i++) {
        if (m->v[i].addr == addr && strcmp(m->v[i].text, text) == 0) {
            m->v[i].count++;
            return;
        }
    }
    if (m->n == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 32;
        m->v = (MsgEnt *)xrealloc(m->v, cap * sizeof(MsgEnt));
        m->cap = cap;
    }
    m->v[m->n].addr = addr;
    snprintf(m->v[m->n].text, sizeof(m->v[m->n].text), "%s", text);
    m->v[m->n].count = 1;
    m->n++;
}

static int cmp_addr_desc(const void *pa, const void *pb) {
    const AddrCount *a = (const AddrCount *)pa;
    const AddrCount *b = (const AddrCount *)pb;
    if (a->count != b->count) return (a->count < b->count) ? 1 : -1;
    return (a->target > b->target) - (a->target < b->target);
}

static int cmp_msg_count(const void *pa, const void *pb) {
    const MsgEnt *a = (const MsgEnt *)pa;
    const MsgEnt *b = (const MsgEnt *)pb;
    if (a->count != b->count) return (a->count < b->count) ? 1 : -1;
    return (a->addr > b->addr) - (a->addr < b->addr);
}

static void parse_probe(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "triage: cannot open probe file %s\n", path);
        return;
    }
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        unsigned t, ra;
        if (sscanf(line, "%x %x", &t, &ra) == 2) addr_add(&g_probe, t, ra);
        else if (sscanf(line, "%x", &t) == 1) addr_add(&g_probe, t, 0);
    }
    fclose(f);
}

static void parse_log(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) die("cannot open log");
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        g_lines++;
        char *p;

        /* [host31]   #271710 0067B845 esp=307F96BC ebp=307FA150 */
        p = strstr(line, "[host31]   #");
        if (p) {
            unsigned long long n;
            unsigned t, e, b;
            if (sscanf(p, "[host31]   #%llu %x esp=%x ebp=%x", &n, &t, &e, &b) == 4)
                trace_add(n, t, e, b);
        }

        /* [t] 0067B845 ra=... esp=... ebp=... */
        if (strncmp(line, "[t] ", 4) == 0) {
            unsigned t, ra, e, b;
            if (sscanf(line, "[t] %x ra=%x esp=%x ebp=%x", &t, &ra, &e, &b) == 4)
                trace_add(0, t, e, b);
        }

        if ((p = strstr(line, "CRASH")) != NULL && strstr(line, "code=")) {
            g_crashes++;
            snprintf(g_crash_line, sizeof(g_crash_line), "%s", line);
            char *q = strstr(line, "code=");
            if (q) sscanf(q, "code=%x", &g_crash_code);
            q = strstr(line, "guest_eip=");
            if (q) sscanf(q, "guest_eip=%x esp=%x ebp=%x",
                          &g_crash_eip, &g_crash_esp, &g_crash_ebp);
        }

        if ((p = strstr(line, "[hle] UNIMPLEMENTED ")) != NULL) {
            char msg[240];
            snprintf(msg, sizeof(msg), "%s", p + strlen("[hle] UNIMPLEMENTED "));
            char *nl = strchr(msg, '\n');
            if (nl) *nl = 0;
            msg_add(&g_unimpl, msg, 0);
        }

        if ((p = strstr(line, "[todo] ")) != NULL) {
            unsigned a = 0;
            char text[200] = "";
            sscanf(p, "[todo] %x %199[^\n]", &a, text);
            msg_add(&g_todo, text, a);
            if (strstr(text, "pause")) g_key_pause++;
        }

        if ((p = strstr(line, "unknown guest target ")) != NULL) {
            unsigned t = 0;
            if (sscanf(p, "unknown guest target %x", &t) == 1)
                addr_add(&g_unknown, t, 0);
        }

        if (strstr(line, "entry returned")) g_key_entryret++;
        if (strstr(line, "[it] done")) g_key_itdone++;
        if (strstr(line, "CreateWindow")) g_key_window++;
        if (strstr(line, "D3D11")) g_key_d3d++;
        if (strstr(line, "DXGI")) g_key_dxgi++;
        if (strstr(line, "fastfail") || strstr(line, "int 0x29")) g_key_fastfail++;
        if (strstr(line, "ClientSdk.dll")) g_key_client_sdk++;
    }
    fclose(f);
}

static void print_addr_vec(const char *title, AddrVec *a, size_t top) {
    if (!a->n) return;
    qsort(a->v, a->n, sizeof(AddrCount), cmp_addr_desc);
    printf("\n%s (%zu unique):\n", title, a->n);
    size_t n = a->n < top ? a->n : top;
    for (size_t i = 0; i < n; i++) {
        printf("  %08X count=%-6u first_ra=%08X\n",
               a->v[i].target, a->v[i].count, a->v[i].ra);
    }
    if (a->n > n) printf("  ... %zu more\n", a->n - n);
}

static void print_msg_vec(const char *title, MsgVec *m, size_t top) {
    if (!m->n) return;
    qsort(m->v, m->n, sizeof(MsgEnt), cmp_msg_count);
    printf("\n%s (%zu unique):\n", title, m->n);
    size_t n = m->n < top ? m->n : top;
    for (size_t i = 0; i < n; i++) {
        if (m->v[i].addr)
            printf("  %08X x%-4u %s\n", m->v[i].addr, m->v[i].count, m->v[i].text);
        else
            printf("  x%-4u %s\n", m->v[i].count, m->v[i].text);
    }
    if (m->n > n) printf("  ... %zu more\n", m->n - n);
}

/* Find the smallest repeated target period in the last part of the trace. */
static size_t detect_cycle(size_t *start_out) {
    size_t n = g_ntrace;
    *start_out = 0;
    if (n < 6) return 0;
    size_t maxp = n / 3;
    if (maxp > 512) maxp = 512;
    for (size_t p = 1; p <= maxp; p++) {
        int ok = 1;
        for (size_t k = 1; k <= 2 && ok; k++) {
            for (size_t i = 0; i < p; i++) {
                if (g_trace[n - k * p + i].target !=
                    g_trace[n - (k + 1) * p + i].target) {
                    ok = 0;
                    break;
                }
            }
        }
        if (ok) {
            *start_out = n - 3 * p;
            return p;
        }
    }
    return 0;
}

static void print_tail(size_t count, size_t maxlines) {
    if (!g_ntrace) return;
    size_t start = g_ntrace > count ? g_ntrace - count : 0;
    printf("\ntrace tail (%zu of %zu entries, run-length compressed):\n",
           g_ntrace - start, g_ntrace);
    size_t shown = 0;
    for (size_t i = start; i < g_ntrace && shown < maxlines; ) {
        size_t j = i + 1;
        while (j < g_ntrace && g_trace[j].target == g_trace[i].target) j++;
        printf("  #%llu..#%llu %08X x%zu esp=%08X ebp=%08X\n",
               g_trace[i].n, g_trace[j - 1].n, g_trace[i].target, j - i,
               g_trace[i].esp, g_trace[i].ebp);
        i = j;
        shown++;
    }
}

static void print_cycle(size_t period, size_t start) {
    if (!period) return;
    printf("\ncycle detected: period=%zu targets (starting trace index %zu)\n",
           period, start);
    size_t n = period < 80 ? period : 80;
    for (size_t i = 0; i < n; i++) {
        TraceEnt *e = &g_trace[start + i];
        printf("  %3zu. %08X esp=%08X ebp=%08X\n", i + 1, e->target, e->esp, e->ebp);
    }
    if (period > n) printf("  ... %zu more in the period\n", period - n);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: triage <host.err> [probe.txt] [--tail N] [--cycles]\n"
                "  Parses a wc3_host31/wc3_host stderr log and prints a triage report.\n"
                "  --tail N   number of trace entries to show (default 80)\n"
                "  --cycles   print one full cycle instead of the tail\n");
        return 2;
    }

    const char *log = argv[1];
    const char *probe = NULL;
    size_t tail = 80;
    int cycles_only = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--tail") == 0 && i + 1 < argc) {
            tail = (size_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--cycles") == 0) {
            cycles_only = 1;
        } else if (argv[i][0] != '-') {
            probe = argv[i];
        } else {
            fprintf(stderr, "triage: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    if (probe) parse_probe(probe);
    parse_log(log);

    size_t period = 0, cycle_start = 0;
    if (g_ntrace) period = detect_cycle(&cycle_start);

    printf("== triage ==\n");
    printf("log: %s\n", log);
    if (probe) printf("probe: %s\n", probe);
    printf("lines=%llu trace=%zu probe_unique=%zu\n",
           g_lines, g_ntrace, g_probe.n);

    printf("\nevents:\n");
    printf("  crashes=%d unimplemented=%zu todos=%zu\n",
           g_crashes, g_unimpl.n, g_todo.n);
    printf("  initterm_done=%u entry_returned=%u\n", g_key_itdone, g_key_entryret);
    printf("  window=%u d3d11=%u dxgi=%u fastfail/int29=%u client_sdk=%u pause=%u\n",
           g_key_window, g_key_d3d, g_key_dxgi, g_key_fastfail,
           g_key_client_sdk, g_key_pause);

    if (g_crashes) {
        printf("\ncrash:\n");
        printf("  code=%08X guest_eip=%08X esp=%08X ebp=%08X\n",
               g_crash_code, g_crash_eip, g_crash_esp, g_crash_ebp);
        printf("  raw: %s", g_crash_line);
        if (g_crash_line[0] && g_crash_line[strlen(g_crash_line) - 1] != '\n')
            printf("\n");
    }

    print_addr_vec("unknown targets from log", &g_unknown, 40);
    print_addr_vec("probe targets (deduplicated)", &g_probe, 40);
    print_msg_vec("HLE unimplemented", &g_unimpl, 30);
    print_msg_vec("TODO / trap sites", &g_todo, 30);

    if (period) {
        printf("\ncycle summary: period=%zu\n", period);
        if (!cycles_only) print_cycle(period, cycle_start);
    }
    if (!cycles_only) print_tail(tail, 60);

    printf("\nverdict:\n");
    if (g_crashes) {
        if (g_crash_code == 0xC00000FDu) {
            printf("  HOST STACK OVERFLOW (C00000FD).\n");
            if (period)
                printf("  A tail-dispatch cycle is present. The host dispatch currently\n"
                       "  recurses for every tail jmp; make dispatch_jmp iterative, or\n"
                       "  break the cycle at %08X.\n", g_trace[cycle_start].target);
            else
                printf("  No short cycle found; inspect the last trace entries and the\n"
                       "  guest call chain. Increase host stack only as a temporary probe.\n");
        } else {
            printf("  GUEST/HOST CRASH. Inspect guest_eip and the last trace entries.\n");
        }
    } else if (g_unimpl.n) {
        printf("  HLE GAP: implement or soft-stub the first UNIMPLEMENTED API above.\n");
    } else if (g_probe.n || g_unknown.n) {
        printf("  UNKNOWN TARGETS: run probe on the probe file, then lift in batch.\n");
    } else if (period) {
        printf("  LONG-RUNNING CYCLE with no crash yet: inspect the loop counter/exit\n"
               "  condition; use WC31_DISPATCH_LIMIT and watchpoints before assuming a hang.\n");
    } else if (g_key_entryret) {
        printf("  ENTRY RETURNED: the guest returned to the host; inspect eax and the log.\n");
    } else {
        printf("  NO DECISIVE EVENT: likely timeout or an unlogged state. Re-run with\n"
               "  WC3_TRACE_N=500 and WC3_SOFT_UNKNOWN=1, then triage again.\n");
    }

    printf("\nnext:\n");
    if (probe) {
        printf("  probe \"%s\" --prot 0x670000-0x690000 "
               "--known-dir reports\\lift --known-prefix war31_lifted "
               "--out-block build31\\seeds_block.txt --out-full build31\\seeds_full.txt\n", probe);
    }
    printf("  expand build\\war31_unpacked2.exe --scan 0x670000-0x690000 "
           "--out-dir build31\\expand\n");
    printf("  manifest reports\\lift\n");
    return 0;
}
