/* closure_pipeline.c - compiled driver for the static closure loop (BP-6.4).
 *
 *   entry.exe -> seed.exe -> lift32.exe -> closure2.exe -> known.exe
 *             -> liftpass.exe (clean corpus + deliberately corrupted fixture)
 *
 * Every step that reads the image or the generated corpus is a compiled C
 * tool.  This driver only sequences them, hashes small artefacts and writes
 * one JSON report; it never parses generated C itself.  Generated C stays
 * under build/ (R2/R3) and is never published.
 *
 * Start discovery is instruction-bound by default: entry.exe runs with
 * --referenced, so a padding-looking byte run is only a start when the image
 * also contains its VA as a value, and the PE entry point, export table and
 * TLS callbacks are added explicitly (they are RVA-valued and invisible to a
 * dword scan).  Referenced candidates whose bytes are a run of code pointers
 * are jump-table bases, not function entries, and are dropped with the
 * evidence written next to the report.  --all-starts restores the old padded
 * inventory for diagnostics.
 *
 * Seeds are lifted in BLOCK mode by default.  A full-mode linear lift keeps
 * decoding past the real `ret` when the following bytes are an inline data
 * table rather than 0xCC/0x90 padding (seed.exe's extent rule), and emits
 * bogus dispatch targets such as 0xE8C9EC10.  Block mode stops at the first
 * terminator and closure2.exe then lifts every real target; --full-seeds
 * keeps the linear mode available as a diagnostic.
 *
 *   closure_pipeline --image <PE> --out-dir build/lift-closure \
 *       [--max-functions N] [--full-seeds] [--all-starts] \
 *       [--refs-image OTHER_PE ...] \
 *       [--report reports/generated/closure_pipeline.json]
 *   closure_pipeline --self-test     # asset-free: prove the detectors fire
 *
 * Exit codes: 0 pass, 1 fail, 2 unavailable.
 */
#include "sha256.h"
#include "dirwalk.h"
#include "pe.h"                 /* pinned upstream header, read-only */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>

#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#if defined(_WIN32) && defined(WC3_FORCE_POSIX)
/* Syntax-only POSIX emulation on a Windows toolchain: UCRT's mkdir() takes
 * one argument and has no setenv()/unsetenv().  Real POSIX uses the branch
 * below.  WC3_FORCE_POSIX is never defined for a shipped build. */
#define MKDIR(p) mkdir(p)
#define WC3_SHIM_NO_SETENV 1
#else
#define MKDIR(p) mkdir(p, 0775)
#endif
#endif

#define MAX_ROUNDS 64
#define MAX_EXTERNAL 4096
#define MAX_LINES 64
#define MAX_EXCLUDED 4096
#define MAX_REFS 8
#define CMD_CAP 8192

static const char *TOOLS[] = {"entry.exe", "seed.exe", "lift32.exe",
                              "closure2.exe", "known.exe", "liftpass.exe",
                              "thunkseeds.exe"};
#define NTOOLS 7
static const char *DETECTORS[] = {"unknown_target", "return_in_void",
                                  "hle_devirtualized", "static_tls_header",
                                  "duplicate_definition"};
#define NDETECTORS 5

typedef struct {
    char root[DIR_MAX_PATH];
    char tools[DIR_MAX_PATH];
    char image[DIR_MAX_PATH];
    char image_name[128];
    char out_dir[DIR_MAX_PATH];
    char work_dir[DIR_MAX_PATH];
    char report[DIR_MAX_PATH];
    char bad_fix[DIR_MAX_PATH];
    char good_fix[DIR_MAX_PATH];
    long max_functions;
    int block_seeds;
    int skip_identity;
    char exclude[DIR_MAX_PATH];
    int no_exclude;
    int referenced;
    int no_table_filter;
    char extra_seeds[DIR_MAX_PATH];
    char refs[MAX_REFS][DIR_MAX_PATH];
    int nrefs;
} Opts;

typedef struct {
    char image_path[DIR_MAX_PATH];
    char image_sha[65];
    char layout_sha[65];
    char image_name[128];
    int identity_verified;
    char tool_sha[NTOOLS][65];
    int inventory_entries, seed_entries, block_seeds;
    int excluded_seeds;
    int referenced, pe_meta_seeds, table_filtered, not_code_seeds;
    int thunk_seed_entries;
    int extra_seed_entries;
    char exclude_file[DIR_MAX_PATH];
    char table_filtered_file[DIR_MAX_PATH];
    char thunk_seeds_file[DIR_MAX_PATH];
    char extra_seeds_file[DIR_MAX_PATH];
    char catalog_sha[65];
    int lift32_entries;
    char seed_c_sha[65];
    long seed_c_bytes;
    int converged, rounds, round_targets[MAX_ROUNDS], nrounds, added;
    char closed_c_sha[65];
    long closed_c_bytes;
    int known_entries;
    char merged_sha[65];
    int lp_exit, lp_pass, lp_files, lp_functions, lp_targets, lp_violations;
    int lp_unknown, lp_in_image_unknown, lp_no_terminal;
    int no_terminal_lines[MAX_LINES], n_no_terminal_lines;
    uint32_t external[MAX_EXTERNAL];
    int nexternal;
    int detectors_ok, detectors[NDETECTORS], ndetectors;
    int bad_exit, bad_violations, self_test_ok;
    int closure_pass, verdict_pass, residual;
    char out_dir[DIR_MAX_PATH], work_dir[DIR_MAX_PATH];
    char good_json[DIR_MAX_PATH], bad_json[DIR_MAX_PATH];
} Report;

static void die(const char *msg)
{
    fprintf(stderr, "closure_pipeline: %s\n", msg);
    exit(2);
}

static void join(char *dst, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
}

static int mkdir_p(const char *path)
{
    char tmp[DIR_MAX_PATH];
    size_t i, n;
    snprintf(tmp, sizeof tmp, "%s", path);
    path_slash(tmp);
    n = strlen(tmp);
    for (i = 1; i <= n; i++) {
        if (tmp[i] == '/' || tmp[i] == 0) {
            char save = tmp[i];
            tmp[i] = 0;
            if (tmp[0] && !path_is_dir(tmp)) MKDIR(tmp);
            tmp[i] = save;
        }
    }
    return path_is_dir(path) ? 0 : -1;
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    size_t got;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

static long file_bytes(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) n = 0;
    fclose(f);
    return n;
}

static int append_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    char buf[1 << 16];
    size_t n;
    if (!in) return -1;
    out = fopen(dst, "ab");
    if (!out) { fclose(in); return -1; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return -1; }
    fclose(in);
    fclose(out);
    return 0;
}

static void hash_into(const char *path, char out[65])
{
    if (!path || !*path || sha256_file(path, out) != 0) out[0] = 0;
}

static int count_lines(const char *path)
{
    char *t = read_text(path);
    int n = 0;
    char *p;
    if (!t) return 0;
    for (p = t; *p; p++) if (*p == '\n') n++;
    free(t);
    return n;
}

/* Evidence-based exclusion list: confirmed non-code starts, never a guess.
 * Each line is "0xVA  # reason"; blanks and comments are ignored. */
static int load_exclusions(const char *path, uint32_t *vas, int cap)
{
    char *t = read_text(path);
    char *line, *next;
    int n = 0;
    if (!t) return 0;
    for (line = t; *line; line = next) {
        char *end = strchr(line, '\n'), *p, *endp;
        unsigned long va;
        next = end ? end + 1 : line + strlen(line);
        if (end) *end = 0;
        p = line;
        {
            char *hash = strchr(p, '#');
            if (hash) *hash = 0;
        }
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        {
            char *tail = p + strlen(p);
            while (tail > p && (tail[-1] == ' ' || tail[-1] == '\t' ||
                                tail[-1] == '\r')) *--tail = 0;
        }
        if (!*p) continue;
        va = strtoul(p, &endp, 16);
        if (endp == p || !va) continue;
        if (n < cap) vas[n++] = (uint32_t)va;
    }
    free(t);
    return n;
}

static int seed_is_excluded(uint32_t va, const uint32_t *ex, int nex)
{
    int i;
    for (i = 0; i < nex; i++) if (ex[i] == va) return 1;
    return 0;
}

/* Rewrites the entry inventory without the excluded VAs.  Returns how many
 * seeds were dropped, or -1 on I/O failure. */
static int filter_seed_file(const char *src, const char *dst,
                            const uint32_t *ex, int nex)
{
    char *t = read_text(src);
    char *line, *next;
    FILE *f;
    int dropped = 0;
    if (!t) return -1;
    f = fopen(dst, "w");
    if (!f) { free(t); return -1; }
    for (line = t; *line; line = next) {
        char *end = strchr(line, '\n'), *p, *endp;
        unsigned long va;
        next = end ? end + 1 : line + strlen(line);
        if (end) *end = 0;
        line[strcspn(line, "\r")] = 0;
        p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;
        va = strtoul(p, &endp, 16);
        if (endp != p && va && seed_is_excluded((uint32_t)va, ex, nex)) {
            dropped++;
            continue;
        }
        fprintf(f, "%s\n", line);
    }
    fclose(f);
    free(t);
    return dropped;
}

/* Concatenate two seed inventories.  The second file is the explicit
 * thunk-tail supplement and never contains a VA already present in the
 * first, so no de-duplication pass is needed here. */
static int merge_seed_files(const char *a, const char *b, const char *dst)
{
    char *ta = read_text(a), *tb = read_text(b);
    FILE *f;
    size_t na, nb;
    if (!ta || !tb) { free(ta); free(tb); return -1; }
    f = fopen(dst, "w");
    if (!f) { free(ta); free(tb); return -1; }
    na = strlen(ta);
    nb = strlen(tb);
    if (na) fwrite(ta, 1, na, f);
    if (na && ta[na - 1] != '\n') fputc('\n', f);
    if (nb) fwrite(tb, 1, nb, f);
    if (nb && tb[nb - 1] != '\n') fputc('\n', f);
    fclose(f);
    free(ta);
    free(tb);
    return 0;
}

/* ------------------------------------------------------- referenced starts */

typedef struct { uint32_t *v; size_t n, cap; } U32Vec;

static void u32_add(U32Vec *a, uint32_t v)
{
    if (!v) return;
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 4096;
        a->v = (uint32_t *)realloc(a->v, a->cap * sizeof(uint32_t));
        if (!a->v) die("out of memory (seed vector)");
    }
    a->v[a->n++] = v;
}

static int u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void u32_sort_unique(U32Vec *a)
{
    size_t i, w = 0;
    if (a->n < 2) return;
    qsort(a->v, a->n, sizeof(uint32_t), u32_cmp);
    for (i = 0; i < a->n; i++)
        if (!w || a->v[w - 1] != a->v[i]) a->v[w++] = a->v[i];
    a->n = w;
}

static int u32_contains(const uint32_t *v, size_t n, uint32_t x)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (v[mid] < x) lo = mid + 1; else hi = mid;
    }
    return lo < n && v[lo] == x;
}

/* Build the comma-separated argument consumed by `seed.exe --add`.  The file
 * is a tracked supplement for address-taken functions that neither entry.exe
 * nor thunkseeds.exe can prove from an executable reference.  Each line is a
 * hex VA; anything after it is a human-readable reason and is ignored. */
static int build_extra_seed_arg(const char *path, char *out, size_t cap,
                                int *count)
{
    char *t = read_text(path);
    char *line, *next;
    U32Vec seen = {0};
    size_t used = 0;
    int n = 0;
    if (!t) return -1;
    out[0] = 0;
    for (line = t; *line; line = next) {
        char *p = line, *endp;
        unsigned long va;
        size_t k;
        int dup = 0;
        next = strchr(line, '\n');
        if (next) *next++ = 0; else next = line + strlen(line);
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p || *p == '#') continue;
        va = strtoul(p, &endp, 16);
        if (endp == p || va == 0) continue;
        for (k = 0; k < seen.n; k++)
            if (seen.v[k] == (uint32_t)va) { dup = 1; break; }
        if (dup) continue;
        if (used + 16 >= cap) { free(seen.v); free(t); return -2; }
        used += (size_t)snprintf(out + used, cap - used, "%s0x%08lX",
                                 n ? "," : "", va);
        u32_add(&seen, (uint32_t)va);
        n++;
    }
    free(seen.v);
    free(t);
    *count = n;
    return 0;
}

static int load_seed_vas(const char *path, U32Vec *out)
{
    char *t = read_text(path);
    char *line, *next;
    if (!t) return -1;
    for (line = t; *line; line = next) {
        char *end = strchr(line, '\n'), *p = line, *endp;
        unsigned long va;
        next = end ? end + 1 : line + strlen(line);
        if (end) *end = 0;
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        va = strtoul(p, &endp, 16);
        if (endp != p && va) u32_add(out, (uint32_t)va);
    }
    free(t);
    return 0;
}

static int pe_lfanew(Pe *pe, uint32_t *out)
{
    uint8_t b[4];
    if (!pe_read_at(pe, 0x3C, b, 4)) return 0;
    *out = pe_rd32(b);
    return 1;
}

static int pe_opt_dword(Pe *pe, uint32_t rel, uint32_t *out)
{
    uint32_t lf;
    uint8_t b[4];
    if (!pe_lfanew(pe, &lf)) return 0;
    if (!pe_read_at(pe, (long long)lf + 24 + rel, b, 4)) return 0;
    *out = pe_rd32(b);
    return 1;
}

static int pe_data_dir(Pe *pe, int idx, uint32_t *rva, uint32_t *size)
{
    uint32_t lf, nrva;
    uint8_t b[8];
    if (!pe_opt_dword(pe, 92, &nrva)) return 0;   /* NumberOfRvaAndSizes */
    if ((uint32_t)idx >= nrva) return 0;
    if (!pe_lfanew(pe, &lf)) return 0;
    if (!pe_read_at(pe, (long long)lf + 24 + 96 + (long long)idx * 8, b, 8))
        return 0;
    *rva = pe_rd32(b);
    *size = pe_rd32(b + 4);
    return 1;
}

/* The PE entry point is an RVA, so the dword scan cannot see it. */
static int pe_entry_va(Pe *pe, uint32_t *out)
{
    uint32_t rva;
    if (!pe_opt_dword(pe, 16, &rva) || !rva) return 0;  /* AddressOfEntryPoint */
    *out = pe->base + rva;
    return pe_code_section_for_va(pe, *out) >= 0;
}

static void add_pe_exports(Pe *pe, U32Vec *out)
{
    uint32_t rva, size, off, nfunc, funcs_rva, foff, exp_lo, exp_hi, i;
    uint8_t dir[40];
    if (!pe_data_dir(pe, 0, &rva, &size) || !rva || size < 40) return;
    if (!pe_va_to_off(pe, pe->base + rva, &off)) return;
    if (!pe_read_at(pe, off, dir, 40)) return;
    nfunc = pe_rd32(dir + 20);
    funcs_rva = pe_rd32(dir + 28);
    if (!nfunc || !funcs_rva) return;
    if (!pe_va_to_off(pe, pe->base + funcs_rva, &foff)) return;
    exp_lo = pe->base + rva;
    exp_hi = exp_lo + size;
    for (i = 0; i < nfunc; i++) {
        uint8_t b[4];
        uint32_t frva, va;
        if (!pe_read_at(pe, (long long)foff + (long long)i * 4, b, 4)) break;
        frva = pe_rd32(b);
        if (!frva) continue;
        /* An RVA inside the export directory is a forwarder string, not code. */
        if (frva >= rva && frva < rva + size) continue;
        va = pe->base + frva;
        if (pe_code_section_for_va(pe, va) >= 0) u32_add(out, va);
    }
}

static void add_pe_tls(Pe *pe, U32Vec *out)
{
    uint32_t rva, size, off, cb;
    uint8_t dir[24];
    int i;
    if (!pe_data_dir(pe, 9, &rva, &size) || !rva || size < 24) return;
    if (!pe_va_to_off(pe, pe->base + rva, &off)) return;
    if (!pe_read_at(pe, off, dir, 24)) return;
    cb = pe_rd32(dir + 12);                 /* AddressOfCallBacks (VA) */
    if (!cb) return;
    for (i = 0; i < 64; i++) {
        uint32_t v;
        if (!pe_read_u32(pe, cb + (uint32_t)i * 4, &v) || !v) break;
        if (pe_code_section_for_va(pe, v) >= 0) u32_add(out, v);
    }
}

/* A referenced VA whose first bytes are a run of pointers into the executable
 * sections is a jump table, not a function entry.  The reference itself is an
 * instruction displacement such as `jmp [eax*4 + 0x6F2B4380]`, which the raw
 * dword scan cannot distinguish from a data pointer.  Three consecutive code
 * pointers is the evidence; real code essentially never starts that way, and
 * PE entry/export/TLS starts are exempt because the format defines them. */
static int seed_is_table_base(Pe *pe, uint32_t va)
{
    uint32_t v;
    int k, run = 0;
    for (k = 0; k < 4; k++) {
        if (!pe_read_u32(pe, va + (uint32_t)k * 4, &v)) break;
        if (pe_code_section_for_va(pe, v) >= 0) {
            if (++run >= 3) return 1;
        } else {
            run = 0;
        }
    }
    return 0;
}

static void print_table_evidence(FILE *f, Pe *pe, uint32_t va)
{
    uint32_t v;
    int k;
    fprintf(f, "0x%08X", va);
    for (k = 0; k < 4; k++) {
        if (!pe_read_u32(pe, va + (uint32_t)k * 4, &v)) break;
        fprintf(f, " %08X", v);
    }
    fputc('\n', f);
}

/* Rewrite the entry inventory as 0xVA 512 seed_VA, adding the PE entry point,
 * exports and TLS callbacks, applying the exclusion ledger and dropping
 * jump-table bases.  Returns 0 on success. */
static int normalize_referenced_seeds(const char *src, const char *dst,
                                      const char *drop_path, Pe *pe,
                                      const uint32_t *ex, int nex,
                                      int filter_tables, Report *r)
{
    U32Vec seeds = { 0 }, meta = { 0 };
    FILE *f, *drop = NULL;
    size_t i;
    int excluded = 0, not_code = 0, filtered = 0;

    if (load_seed_vas(src, &seeds) != 0) return -1;
    if (pe) {
        uint32_t va;
        if (pe_entry_va(pe, &va)) u32_add(&meta, va);
        add_pe_exports(pe, &meta);
        add_pe_tls(pe, &meta);
        u32_sort_unique(&meta);
        for (i = 0; i < meta.n; i++) u32_add(&seeds, meta.v[i]);
    }
    u32_sort_unique(&seeds);

    f = fopen(dst, "w");
    if (!f) { free(seeds.v); free(meta.v); return -1; }
    if (filter_tables && drop_path) drop = fopen(drop_path, "w");
    for (i = 0; i < seeds.n; i++) {
        uint32_t va = seeds.v[i];
        if (seed_is_excluded(va, ex, nex)) { excluded++; continue; }
        if (pe && pe_code_section_for_va(pe, va) < 0) { not_code++; continue; }
        if (pe && filter_tables && !u32_contains(meta.v, meta.n, va) &&
            seed_is_table_base(pe, va)) {
            filtered++;
            if (drop) print_table_evidence(drop, pe, va);
            continue;
        }
        fprintf(f, "0x%08X 512 seed_%08X\n", va, va);
    }
    fclose(f);
    if (drop) fclose(drop);
    r->pe_meta_seeds = (int)meta.n;
    r->excluded_seeds += excluded;
    r->table_filtered = filtered;
    r->not_code_seeds = not_code;
    free(seeds.v);
    free(meta.v);
    return 0;
}

static int count_x(const char *path)
{
    char *t = read_text(path);
    char *p;
    int n = 0;
    if (!t) return 0;
    p = t;
    while ((p = strstr(p, "X(")) != NULL) {
        int k, ok = 1;
        p += 2;
        for (k = 0; k < 8; k++)
            if (!isxdigit((unsigned char)p[k])) { ok = 0; break; }
        if (ok) n++;
    }
    free(t);
    return n;
}

/* lift32.exe reads WC3_LIFT_BLOCK once at startup; children inherit the
 * parent environment on both CreateProcessA and system(). */
static void set_block_mode(int on)
{
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    SetEnvironmentVariableA("WC3_LIFT_BLOCK", on ? "1" : "0");
#elif defined(WC3_SHIM_NO_SETENV)
    (void)on;  /* syntax-only emulation; no process is spawned */
#else
    if (on) setenv("WC3_LIFT_BLOCK", "1", 1);
    else unsetenv("WC3_LIFT_BLOCK");
#endif
}

static int run_step(const char *log_path, const char *fmt, ...)
{
    char cmd[CMD_CAP];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    printf("+ %s\n", cmd);
    fflush(stdout);
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    /* cmd.exe /c mangles a command line that starts with a quoted path that
     * contains spaces ("E:/Games/Warcraft III/...").  CreateProcessA parses
     * the quoted argv itself and the child writes straight into the log. */
    {
        SECURITY_ATTRIBUTES sa;
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        HANDLE h;
        char *cmdline = (char *)malloc(strlen(cmd) + 1);
        BOOL ok;
        DWORD rc = 1;
        if (!cmdline) return -1;
        strcpy(cmdline, cmd);
        memset(&sa, 0, sizeof sa);
        sa.bInheritHandle = TRUE;
        h = CreateFileA(log_path, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) { free(cmdline); return -1; }
        memset(&si, 0, sizeof si);
        memset(&pi, 0, sizeof pi);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = h;
        si.hStdError = h;
        ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0,
                            NULL, NULL, &si, &pi);
        free(cmdline);
        if (ok) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            GetExitCodeProcess(pi.hProcess, &rc);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            fprintf(stderr, "closure_pipeline: CreateProcessA failed (%lu)\n",
                    (unsigned long)GetLastError());
        }
        CloseHandle(h);
        return ok ? (int)rc : -1;
    }
#else
    {
        char full[CMD_CAP + 512];
        snprintf(full, sizeof full, "%s > \"%s\" 2>&1", cmd, log_path);
        return system(full);
    }
#endif
}

/* ---- tiny JSON readers (small generated reports only) ------------------ */

static int json_int(const char *t, const char *key, int dflt)
{
    char pat[64];
    const char *p;
    if (!t) return dflt;
    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(t, pat);
    if (!p) return dflt;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '-' && !isdigit((unsigned char)*p)) return dflt;
    return atoi(p);
}

static int json_has(const char *t, const char *needle)
{
    return t && strstr(t, needle) != NULL;
}

/* Copy a `"key": "value"` string, unescaping \\ (layout manifest paths). */
static int json_string_after(const char *p, const char *key,
                             char *out, size_t cap)
{
    char pat[64];
    const char *s;
    size_t n = 0;
    snprintf(pat, sizeof pat, "\"%s\": \"", key);
    s = strstr(p, pat);
    if (!s) return -1;
    s += strlen(pat);
    while (*s && *s != '"' && n + 1 < cap) {
        if (s[0] == '\\' && s[1] == '\\') { out[n++] = '\\'; s += 2; continue; }
        out[n++] = *s++;
    }
    out[n] = 0;
    return 0;
}

/* The pin file is small and stable: find the image object by name, then read
 * source/sha256 from inside that object only. */
static int layout_lookup(const char *layout, const char *name,
                         char *source, char *sha)
{
    char needle[192];
    char *t = read_text(layout);
    char *p, *end, save;
    int rc = -1;
    if (!t) return -1;
    snprintf(needle, sizeof needle, "\"name\": \"%s\"", name);
    p = strstr(t, needle);
    if (!p) { free(t); return -1; }
    end = strchr(p, '}');
    if (!end) end = t + strlen(t);
    save = *end;
    *end = 0;
    if (json_string_after(p, "sha256", sha, 65) == 0 &&
        json_string_after(p, "source", source, DIR_MAX_PATH) == 0)
        rc = 0;
    *end = save;
    free(t);
    return rc;
}

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { fputc('\\', f); fputc(*s, f); }
        else if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", (unsigned)*s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

static void add_external(Report *r, uint32_t va)
{
    int i;
    for (i = 0; i < r->nexternal; i++) if (r->external[i] == va) return;
    if (r->nexternal < MAX_EXTERNAL) r->external[r->nexternal++] = va;
}

static void add_no_terminal_line(Report *r, int line)
{
    if (r->n_no_terminal_lines < MAX_LINES)
        r->no_terminal_lines[r->n_no_terminal_lines++] = line;
}

static int prev_line_number(const char *text, const char *pos)
{
    const char *p = text, *last = NULL;
    while (p && p < pos) {
        const char *q = strstr(p, "\"line\":");
        if (!q || q >= pos) break;
        last = q;
        p = q + 7;
    }
    if (!last) return 0;
    last += 7;
    while (*last == ' ' || *last == '\t') last++;
    return atoi(last);
}

/* Classify every unknown_target: in-image code VAs are closure gaps, the
 * rest are external/pseudo targets that this PE cannot define. */
static void classify_violations(Report *r, const char *good, int have_pe, Pe *pe)
{
    const char *p;
    if (!good) return;
    p = good;
    while ((p = strstr(p, "\"kind\": \"")) != NULL) {
        char kind[48];
        const char *s = p + 9;
        size_t n = 0;
        while (*s && *s != '"' && n + 1 < sizeof kind) kind[n++] = *s++;
        kind[n] = 0;
        if (!strcmp(kind, "unknown_target")) {
            const char *d = strstr(s, "\"detail\": \"0x");
            r->lp_unknown++;
            if (d) {
                uint32_t va = (uint32_t)strtoul(d + 13, NULL, 16);
                int in_code = have_pe && pe_code_section_for_va(pe, va) >= 0;
                if (in_code) r->lp_in_image_unknown++;
                else add_external(r, va);
            } else {
                r->lp_in_image_unknown++;
            }
        } else if (!strcmp(kind, "no_terminal")) {
            r->lp_no_terminal++;
            add_no_terminal_line(r, prev_line_number(good, p));
        }
        p = s;
    }
}

static void parse_closure_log(Report *r, const char *path)
{
    char *t = read_text(path);
    char *line;
    if (!t) return;
    for (line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
        int round, n;
        char *p;
        line[strcspn(line, "\r")] = 0;
        if (sscanf(line, "round %d: %d targets", &round, &n) == 2) {
            if (r->nrounds < MAX_ROUNDS) r->round_targets[r->nrounds] = n;
            r->nrounds++;
            r->rounds++;
        }
        p = strstr(line, " entries in ");
        if (p) {
            char *q = p;
            while (q > line && isdigit((unsigned char)q[-1])) q--;
            r->added = atoi(q);
            sscanf(p, " entries in %d rounds", &r->rounds);
        }
    }
    free(t);
}

static void parse_bad_report(Report *r, const char *path)
{
    char *t = read_text(path);
    const char *p;
    int i;
    if (!t) return;
    p = t;
    while ((p = strstr(p, "\"kind\": \"")) != NULL) {
        char kind[48];
        const char *s = p + 9;
        size_t n = 0;
        while (*s && *s != '"' && n + 1 < sizeof kind) kind[n++] = *s++;
        kind[n] = 0;
        for (i = 0; i < NDETECTORS; i++)
            if (!strcmp(kind, DETECTORS[i])) r->detectors[i] = 1;
        p = s;
    }
    /* count kinds directly: the report has one "kind" per violation */
    {
        const char *q = t;
        while ((q = strstr(q, "\"kind\":")) != NULL) { r->bad_violations++; q += 7; }
    }
    free(t);
}

static void write_report(const char *path, const Report *r)
{
    FILE *f = fopen(path, "w");
    int i;
    if (!f) die("cannot open --report");
    fprintf(f, "{\n  \"schema\": 1,\n  \"published\": false,\n");
    fprintf(f, "  \"verdict\": \"%s\",\n", r->verdict_pass ? "pass" : "fail");
    fprintf(f, "  \"closure_verdict\": \"%s\",\n", r->closure_pass ? "pass" : "fail");
    fprintf(f, "  \"fixed_point\": %s,\n", r->closure_pass ? "true" : "false");
    fprintf(f, "  \"residual_violations\": %d,\n", r->residual);

    fprintf(f, "  \"image\": {\"path\": "); json_str(f, r->image_path);
    fprintf(f, ", \"name\": "); json_str(f, r->image_name);
    fprintf(f, ", \"sha256\": \"%s\", \"layout_sha256\": \"%s\", "
            "\"verified_against_layout\": %s, "
            "\"source\": \"config/layout.1.27b.json\"},\n",
            r->image_sha, r->layout_sha, r->identity_verified ? "true" : "false");

    fprintf(f, "  \"tools_sha256\": {\n");
    for (i = 0; i < NTOOLS; i++)
        fprintf(f, "    \"%s\": \"%s\"%s\n", TOOLS[i], r->tool_sha[i],
                i + 1 < NTOOLS ? "," : "");
    fprintf(f, "  },\n");

    fprintf(f, "  \"discover\": {\"tool\": "
            "\"entry.exe + thunkseeds.exe + seed.exe\", "
            "\"mode\": \"%s\", \"referenced\": %s, "
            "\"seed_mode\": \"%s\", "
            "\"inventory_entries\": %d, \"seed_entries\": %d, "
            "\"pe_meta_seeds\": %d, \"table_filtered\": %d, "
            "\"not_code_seeds\": %d, \"excluded_seeds\": %d, "
            "\"thunk_seed_entries\": %d, \"extra_seed_entries\": %d, "
            "\"exclude_file\": ",
            r->referenced
                ? (r->extra_seed_entries ? "referenced+pe-meta+thunk+extra"
                                         : "referenced+pe-meta+thunk")
                : (r->extra_seed_entries ? "padded-inventory+thunk+extra"
                                         : "padded-inventory+thunk"),
            r->referenced ? "true" : "false",
            r->block_seeds ? "block" : "full",
            r->inventory_entries, r->seed_entries,
            r->pe_meta_seeds, r->table_filtered,
            r->not_code_seeds, r->excluded_seeds,
            r->thunk_seed_entries, r->extra_seed_entries);
    json_str(f, r->exclude_file);
    fprintf(f, ", \"table_filtered_file\": ");
    json_str(f, r->table_filtered_file);
    fprintf(f, ", \"thunk_seeds_file\": ");
    json_str(f, r->thunk_seeds_file);
    fprintf(f, ", \"extra_seeds_file\": ");
    json_str(f, r->extra_seeds_file);
    fprintf(f, ", \"catalog_sha256\": \"%s\"},\n", r->catalog_sha);
    fprintf(f, "  \"lift32\": {\"entries\": %d, \"bytes\": %ld, "
            "\"c_sha256\": \"%s\"},\n", r->lift32_entries, r->seed_c_bytes,
            r->seed_c_sha);
    fprintf(f, "  \"closure\": {\"converged\": %s, \"rounds\": %d, "
            "\"round_targets\": [", r->converged ? "true" : "false", r->rounds);
    for (i = 0; i < r->nrounds && i < MAX_ROUNDS; i++)
        fprintf(f, "%s%d", i ? ", " : "", r->round_targets[i]);
    fprintf(f, "], \"added\": %d, \"bytes\": %ld, \"c_sha256\": \"%s\"},\n",
            r->added, r->closed_c_bytes, r->closed_c_sha);
    fprintf(f, "  \"known\": {\"merged_entries\": %d, "
            "\"list_sha256\": \"%s\"},\n", r->known_entries, r->merged_sha);

    fprintf(f, "  \"liftpass\": {\"verdict\": \"%s\", \"exit\": %d, "
            "\"files\": %d, \"functions\": %d, "
            "\"constant_targets\": %d, \"violations\": %d,\n",
            r->lp_pass ? "pass" : "fail", r->lp_exit, r->lp_files,
            r->lp_functions, r->lp_targets, r->lp_violations);
    fprintf(f, "    \"violations_by_kind\": {\"unknown_target\": %d, "
            "\"no_terminal\": %d},\n", r->lp_unknown, r->lp_no_terminal);
    fprintf(f, "    \"in_image_unknown_target\": %d,\n", r->lp_in_image_unknown);
    fprintf(f, "    \"external_pseudo_targets\": [");
    for (i = 0; i < r->nexternal; i++)
        fprintf(f, "%s\"0x%08X\"", i ? ", " : "", r->external[i]);
    fprintf(f, "],\n    \"no_terminal_lines\": [");
    for (i = 0; i < r->n_no_terminal_lines; i++)
        fprintf(f, "%s%d", i ? ", " : "", r->no_terminal_lines[i]);
    fprintf(f, "],\n    \"self_test\": \"%s\",\n",
            r->self_test_ok ? "pass" : "fail");
    fprintf(f, "    \"negative_fixture_exit\": %d, \"detectors\": [",
            r->bad_exit);
    {
        int first = 1;
        for (i = 0; i < NDETECTORS; i++) {
            if (!r->detectors[i]) continue;
            fprintf(f, "%s\"%s\"", first ? "" : ", ", DETECTORS[i]);
            first = 0;
        }
    }
    fprintf(f, "], \"detectors_ok\": %s,\n", r->detectors_ok ? "true" : "false");
    fprintf(f, "    \"reports\": ["); json_str(f, r->good_json);
    fprintf(f, ", "); json_str(f, r->bad_json); fprintf(f, "]\n  },\n");

    fprintf(f, "  \"outputs\": {\"out_dir\": "); json_str(f, r->out_dir);
    fprintf(f, ", \"work_dir\": "); json_str(f, r->work_dir);
    fprintf(f, ", \"generated_c\": [\"seed.c\", \"closed.c\"]}\n}\n");
    fclose(f);
}

/* ---- asset-free V5.4 self-test ------------------------------------------ */

static int self_test(const Opts *o)
{
    char log[DIR_MAX_PATH], json[DIR_MAX_PATH], list[DIR_MAX_PATH];
    char lp[DIR_MAX_PATH];
    char *text;
    int i, rc;
    join(lp, sizeof lp, "%s/liftpass.exe", o->tools);
    join(list, sizeof list, "%s/lifted_smoke.h", o->good_fix);
    join(log, sizeof log, "%s/selftest.log", o->out_dir);
    join(json, sizeof json, "%s/selftest.json", o->out_dir);
    if (mkdir_p(o->out_dir) != 0) die("cannot create --out-dir");
    rc = run_step(log, "\"%s\" check \"%s\" --list \"%s\" --json \"%s\" --self-test",
                  lp, o->bad_fix, list, json);
    if (rc == 0) { fprintf(stderr, "self-test: corrupted fixture was accepted\n"); return 1; }
    text = read_text(json);
    if (!text) { fprintf(stderr, "self-test: no liftpass report\n"); return 1; }
    for (i = 0; i < NDETECTORS; i++) {
        char needle[80];
        snprintf(needle, sizeof needle, "\"kind\": \"%s\"", DETECTORS[i]);
        if (!strstr(text, needle)) {
            fprintf(stderr, "self-test: detector %s did not fire\n", DETECTORS[i]);
            free(text);
            return 1;
        }
    }
    free(text);
    printf("self-test: pass (%d/%d detectors on the corrupted fixture)\n",
           NDETECTORS, NDETECTORS);
    return 0;
}

/* ---- full pipeline ------------------------------------------------------ */

static void parse_args(int argc, char **argv, Opts *o, int *do_self_test)
{
    char cwd[DIR_MAX_PATH];
    int i;
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    GetCurrentDirectoryA(sizeof cwd, cwd);
#else
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
#endif
    path_slash(cwd);
    /* First pass: --root changes every default path below. */
    snprintf(o->root, sizeof o->root, "%s", cwd);
    for (i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--root")) {
            snprintf(o->root, sizeof o->root, "%s", argv[i + 1]);
            path_slash(o->root);
        }
    join(o->tools, sizeof o->tools, "%s/build/lift-tools", o->root);
    join(o->out_dir, sizeof o->out_dir, "%s/build/lift-closure", o->root);
    join(o->work_dir, sizeof o->work_dir, "%s/build/lift-closure-work", o->root);
    join(o->report, sizeof o->report, "%s/reports/generated/closure_pipeline.json", o->root);
    join(o->bad_fix, sizeof o->bad_fix, "%s/tests/fixtures/lift_tools_bad", o->root);
    join(o->good_fix, sizeof o->good_fix, "%s/tests/fixtures/lift_tools", o->root);
    join(o->exclude, sizeof o->exclude,
         "%s/config/excluded_seeds.1.27b.txt", o->root);
    snprintf(o->image_name, sizeof o->image_name, "war3.exe");
    o->image[0] = 0;
    o->max_functions = 0;
    o->block_seeds = 1;
    o->referenced = 1;
    o->no_table_filter = 0;
    o->extra_seeds[0] = 0;
    o->nrefs = 0;
    *do_self_test = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--self-test")) *do_self_test = 1;
        else if (!strcmp(a, "--full-seeds")) o->block_seeds = 0;
        else if (!strcmp(a, "--all-starts")) o->referenced = 0;
        else if (!strcmp(a, "--no-table-filter")) o->no_table_filter = 1;
        else if (!strcmp(a, "--refs-image") && i + 1 < argc &&
                 o->nrefs < MAX_REFS) {
            snprintf(o->refs[o->nrefs], sizeof o->refs[0], "%s", argv[++i]);
            o->nrefs++;
        }
        else if (!strcmp(a, "--skip-identity-check")) o->skip_identity = 1;
        else if (!strcmp(a, "--no-exclude")) o->no_exclude = 1;
        else if (!strcmp(a, "--exclude") && i + 1 < argc) snprintf(o->exclude, sizeof o->exclude, "%s", argv[++i]);
        else if (!strcmp(a, "--extra-seeds") && i + 1 < argc) snprintf(o->extra_seeds, sizeof o->extra_seeds, "%s", argv[++i]);
        else if (!strcmp(a, "--image") && i + 1 < argc) snprintf(o->image, sizeof o->image, "%s", argv[++i]);
        else if (!strcmp(a, "--name") && i + 1 < argc) snprintf(o->image_name, sizeof o->image_name, "%s", argv[++i]);
        else if (!strcmp(a, "--root") && i + 1 < argc) i++;
        else if (!strcmp(a, "--tools") && i + 1 < argc) snprintf(o->tools, sizeof o->tools, "%s", argv[++i]);
        else if (!strcmp(a, "--out-dir") && i + 1 < argc) snprintf(o->out_dir, sizeof o->out_dir, "%s", argv[++i]);
        else if (!strcmp(a, "--work-dir") && i + 1 < argc) snprintf(o->work_dir, sizeof o->work_dir, "%s", argv[++i]);
        else if (!strcmp(a, "--report") && i + 1 < argc) snprintf(o->report, sizeof o->report, "%s", argv[++i]);
        else if (!strcmp(a, "--bad-fixture") && i + 1 < argc) snprintf(o->bad_fix, sizeof o->bad_fix, "%s", argv[++i]);
        else if (!strcmp(a, "--max-functions") && i + 1 < argc) o->max_functions = atol(argv[++i]);
        else {
            fprintf(stderr,
                    "usage: closure_pipeline [--image <PE>] [--name <layout image>] "
                    "[--root DIR] [--tools DIR] [--out-dir DIR] [--work-dir DIR] "
                    "[--report JSON] [--max-functions N] [--full-seeds] "
                    "[--all-starts] [--refs-image PE ...] [--no-table-filter] "
                    "[--skip-identity-check] [--exclude F | --no-exclude] "
                    "[--extra-seeds F] [--self-test]\n");
            exit(2);
        }
    }
    path_slash(o->tools);
    path_slash(o->out_dir);
    path_slash(o->work_dir);
    path_slash(o->report);
    path_slash(o->bad_fix);
    path_slash(o->good_fix);
    path_slash(o->image);
    path_slash(o->extra_seeds);
}

static const char *stem_of(const char *path, char *out, size_t cap)
{
    const char *base = path, *p;
    const char *dot;
    for (p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    dot = strrchr(base, '.');
    if (!dot) dot = base + strlen(base);
    if ((size_t)(dot - base) >= cap) dot = base + cap - 1;
    memcpy(out, base, (size_t)(dot - base));
    out[dot - base] = 0;
    return out;
}

int main(int argc, char **argv)
{
    Opts o;
    Report r;
    char layout[DIR_MAX_PATH], source[DIR_MAX_PATH], want_sha[65];
    char entry_seeds[DIR_MAX_PATH], funcs[DIR_MAX_PATH];
    char seed_c[DIR_MAX_PATH], seed_list[DIR_MAX_PATH];
    char closed_c[DIR_MAX_PATH], closed_list[DIR_MAX_PATH];
    char merged[DIR_MAX_PATH], report_dir[DIR_MAX_PATH];
    char stem[256], log[DIR_MAX_PATH], tmp[DIR_MAX_PATH];
    char seeds_used[DIR_MAX_PATH], filtered_seeds[DIR_MAX_PATH];
    char normalized_seeds[DIR_MAX_PATH], table_drops[DIR_MAX_PATH];
    char thunk_seeds[DIR_MAX_PATH], combined_seeds[DIR_MAX_PATH];
    char edge_c[DIR_MAX_PATH], closure_seed_c[DIR_MAX_PATH];
    char ref_args[CMD_CAP / 2] = "";
    char extra_arg[2048] = "";
    char extra_add[2048] = "";
    int extra_count = 0;
    uint32_t excluded[MAX_EXCLUDED];
    int nexcluded = 0;
    char limit_arg[128] = "";
    Pe pe;
    int i, rc, have_pe, do_self_test = 0;
    char *good = NULL;

    parse_args(argc, argv, &o, &do_self_test);
    memset(&r, 0, sizeof r);

    for (i = 0; i < NTOOLS; i++) {
        join(tmp, sizeof tmp, "%s/%s", o.tools, TOOLS[i]);
        if (!path_is_dir(o.tools) || sha256_file(tmp, r.tool_sha[i]) != 0) {
            fprintf(stderr, "closure_pipeline: unavailable (%s missing; run build_tools.ps1)\n",
                    TOOLS[i]);
            return 2;
        }
    }
    if (!path_is_dir(o.bad_fix)) {
        fprintf(stderr, "closure_pipeline: unavailable (V5.4 fixture missing)\n");
        return 2;
    }
    if (do_self_test) return self_test(&o);

    join(layout, sizeof layout, "%s/config/layout.1.27b.json", o.root);
    if (o.skip_identity) {
        /* Multi-module driver: rebased inputs intentionally do not match the
         * layout manifest's hashes of the ORIGINAL PEs.  The parent verifies
         * the whole input set with corpus.exe verify before starting us, and
         * the report below states that the layout check was skipped. */
        if (!o.image[0]) {
            fprintf(stderr, "closure_pipeline: --skip-identity-check needs --image\n");
            return 2;
        }
        want_sha[0] = 0;
    } else {
        if (layout_lookup(layout, o.image_name, source, want_sha) != 0) {
            fprintf(stderr, "closure_pipeline: %s has no image %s (V4.6)\n",
                    layout, o.image_name);
            return 2;
        }
        if (!o.image[0]) snprintf(o.image, sizeof o.image, "%s", source);
    }
    if (sha256_file(o.image, r.image_sha) != 0) {
        fprintf(stderr, "closure_pipeline: unavailable (image missing: %s)\n", o.image);
        return 2;
    }
    snprintf(r.image_path, sizeof r.image_path, "%s", o.image);
    snprintf(r.image_name, sizeof r.image_name, "%s", o.image_name);
    snprintf(r.layout_sha, sizeof r.layout_sha, "%s", want_sha);
    r.identity_verified = (!o.skip_identity) && strcmp(r.image_sha, want_sha) == 0;
    if (!o.skip_identity && !r.identity_verified) {
        fprintf(stderr, "closure_pipeline: image sha256 %s != layout %s (V4.6)\n",
                r.image_sha, want_sha);
        return 1;
    }
    have_pe = pe_open(&pe, o.image) ? 1 : 0;

    if (mkdir_p(o.out_dir) != 0 || mkdir_p(o.work_dir) != 0)
        die("cannot create --out-dir / --work-dir");
    if (!strncmp(o.work_dir, o.out_dir, strlen(o.out_dir)) &&
        (o.work_dir[strlen(o.out_dir)] == '/' || o.work_dir[strlen(o.out_dir)] == 0))
        die("--work-dir must be outside --out-dir (duplicate L_ definitions)");
    snprintf(r.out_dir, sizeof r.out_dir, "%s", o.out_dir);
    snprintf(r.work_dir, sizeof r.work_dir, "%s", o.work_dir);

    join(entry_seeds, sizeof entry_seeds, "%s/entry_seeds.txt", o.out_dir);
    join(funcs, sizeof funcs, "%s/functions.txt", o.out_dir);
    join(seed_c, sizeof seed_c, "%s/seed.c", o.out_dir);
    join(seed_list, sizeof seed_list, "%s/seed_list.h", o.out_dir);
    join(closed_c, sizeof closed_c, "%s/closed.c", o.out_dir);
    join(closed_list, sizeof closed_list, "%s/closed_list.h", o.out_dir);
    join(merged, sizeof merged, "%s/corpus.list.h", o.out_dir);
    if (o.max_functions > 0)
        snprintf(limit_arg, sizeof limit_arg, " --limit %ld", o.max_functions);

    /* 1/2. Compiled discovery: inventory -> recovered extents.  seed.exe is
     * given the same inventory as its catalog so an extent never crosses the
     * next known function start. */
    for (i = 0; i < o.nrefs; i++) {
        size_t used = strlen(ref_args);
        if (used < sizeof ref_args)
            snprintf(ref_args + used, sizeof ref_args - used,
                     " --refs-image \"%s\"", o.refs[i]);
    }
    join(log, sizeof log, "%s/step_entry.log", o.out_dir);
    rc = run_step(log, "\"%s/entry.exe\" --module \"%s\" --out \"%s\"%s%s --quiet",
                  o.tools, o.image, entry_seeds,
                  o.referenced ? " --referenced" : "", ref_args);
    if (rc != 0) { fprintf(stderr, "entry.exe failed (see %s)\n", log); return 1; }
    r.inventory_entries = count_lines(entry_seeds);
    r.referenced = o.referenced;

    /* Evidence-based exclusions of confirmed non-code starts.  The list is
     * explicit and carries a reason per VA; nothing is guessed from bytes.
     * Both the seed set and the extent-boundary catalog use the filtered
     * inventory, so a data address cannot cap a real function either. */
    if (!o.no_exclude) {
        nexcluded = load_exclusions(o.exclude, excluded, MAX_EXCLUDED);
        if (nexcluded > 0) {
            snprintf(r.exclude_file, sizeof r.exclude_file, "%s", o.exclude);
            printf("closure_pipeline: %d confirmed non-code start(s) in ledger\n",
                   nexcluded);
        }
    }
    if (o.referenced) {
        join(normalized_seeds, sizeof normalized_seeds,
             "%s/entry_seeds_referenced.txt", o.out_dir);
        join(table_drops, sizeof table_drops,
             "%s/entry_seeds_table_dropped.txt", o.out_dir);
        rc = normalize_referenced_seeds(entry_seeds, normalized_seeds,
                                        table_drops, have_pe ? &pe : NULL,
                                        o.no_exclude ? NULL : excluded,
                                        o.no_exclude ? 0 : nexcluded,
                                        !o.no_table_filter, &r);
        if (rc != 0) { fprintf(stderr, "cannot normalize referenced seeds\n"); return 1; }
        snprintf(seeds_used, sizeof seeds_used, "%s", normalized_seeds);
        if (r.table_filtered > 0)
            snprintf(r.table_filtered_file, sizeof r.table_filtered_file,
                     "%s", table_drops);
        printf("closure_pipeline: referenced discovery: %d raw, %d PE starts, "
               "%d table bases dropped, %d non-code dropped\n",
               r.inventory_entries, r.pe_meta_seeds, r.table_filtered,
               r.not_code_seeds);
    } else {
        snprintf(seeds_used, sizeof seeds_used, "%s", entry_seeds);
        if (!o.no_exclude && nexcluded > 0) {
            join(filtered_seeds, sizeof filtered_seeds,
                 "%s/entry_seeds_filtered.txt", o.out_dir);
            rc = filter_seed_file(entry_seeds, filtered_seeds,
                                  excluded, nexcluded);
            if (rc < 0) { fprintf(stderr, "cannot filter seed inventory\n"); return 1; }
            r.excluded_seeds = rc;
            snprintf(seeds_used, sizeof seeds_used, "%s", filtered_seeds);
            printf("closure_pipeline: excluded %d confirmed non-code start(s)\n", rc);
        }
    }

    /* entry.exe drops an address-taken function that begins immediately after
     * a six-byte jmp thunk because it is not 16-byte aligned.  Recover exactly
     * that class here; thunkseeds.exe scans all sections for an in-image dword
     * pointing at such a tail and writes the same seed format. */
    join(thunk_seeds, sizeof thunk_seeds,
         "%s/entry_seeds_thunks.txt", o.out_dir);
    join(combined_seeds, sizeof combined_seeds,
         "%s/entry_seeds_combined.txt", o.out_dir);
    join(log, sizeof log, "%s/step_thunkseeds.log", o.out_dir);
    join(edge_c, sizeof edge_c, "%s/jump_edges.c", o.work_dir);
    join(closure_seed_c, sizeof closure_seed_c,
         "%s/closure_seed.c", o.work_dir);
    if (!o.no_exclude && nexcluded > 0)
        rc = run_step(log,
                      "\"%s/thunkseeds.exe\" --module \"%s\" --known \"%s\" "
                      "--exclude \"%s\" --out \"%s\" --edges-out \"%s\"",
                      o.tools, o.image, seeds_used, o.exclude, thunk_seeds,
                      edge_c);
    else
        rc = run_step(log,
                      "\"%s/thunkseeds.exe\" --module \"%s\" --known \"%s\" "
                      "--out \"%s\" --edges-out \"%s\"",
                      o.tools, o.image, seeds_used, thunk_seeds, edge_c);
    if (rc != 0) { fprintf(stderr, "thunkseeds.exe failed (see %s)\n", log); return 1; }
    r.thunk_seed_entries = count_lines(thunk_seeds);
    if (r.thunk_seed_entries > 0) {
        if (merge_seed_files(seeds_used, thunk_seeds, combined_seeds) != 0) {
            fprintf(stderr, "cannot merge thunk seeds\n");
            return 1;
        }
        snprintf(r.thunk_seeds_file, sizeof r.thunk_seeds_file, "%s", thunk_seeds);
        snprintf(seeds_used, sizeof seeds_used, "%s", combined_seeds);
        printf("closure_pipeline: added %d thunk seed(s)\n",
               r.thunk_seed_entries);
    }
    if (o.extra_seeds[0]) {
        int n = 0;
        rc = build_extra_seed_arg(o.extra_seeds, extra_add, sizeof extra_add, &n);
        if (rc != 0) {
            fprintf(stderr, "closure_pipeline: cannot read --extra-seeds %s\n",
                    o.extra_seeds);
            return 1;
        }
        r.extra_seed_entries = n;
        snprintf(r.extra_seeds_file, sizeof r.extra_seeds_file, "%s",
                 o.extra_seeds);
        if (n > 0) {
            snprintf(extra_arg, sizeof extra_arg, " --add \"%s\"", extra_add);
            printf("closure_pipeline: added %d explicit seed(s) from %s\n",
                   n, o.extra_seeds);
        } else {
            printf("closure_pipeline: %s contains no seed addresses\n",
                   o.extra_seeds);
        }
    }

    join(log, sizeof log, "%s/step_seed.log", o.out_dir);
    rc = run_step(log,
                  "\"%s/seed.exe\" --module \"%s\" --catalog \"%s\" "
                  "--seeds \"%s\" --out \"%s\"%s%s",
                  o.tools, o.image, seeds_used, seeds_used, funcs, limit_arg,
                  extra_arg);
    if (rc != 0) { fprintf(stderr, "seed.exe failed (see %s)\n", log); return 1; }
    r.seed_entries = count_lines(funcs);
    hash_into(funcs, r.catalog_sha);

    /* 3. Seed lift.  Block mode by default; see the file header for why a
     * linear full lift over inline data tables fabricates dispatch targets. */
    r.block_seeds = o.block_seeds;
    set_block_mode(o.block_seeds);
    join(log, sizeof log, "%s/step_lift32.log", o.out_dir);
    rc = run_step(log, "\"%s/lift32.exe\" \"%s\" \"%s\" \"%s\"",
                  o.tools, o.image, funcs, seed_c);
    set_block_mode(0);
    if (rc != 0) { fprintf(stderr, "lift32.exe failed (see %s)\n", log); return 1; }
    r.lift32_entries = count_x(seed_list);
    r.seed_c_bytes = file_bytes(seed_c);
    hash_into(seed_c, r.seed_c_sha);

    /* 4. Closure to a fixed point (closure2.exe loops internally).  The local
     * build gives every target a strict block extent; see closure2.c.  Jump-
     * table arms found in code-resident pointer tables are injected as
     * synthetic edges, so they go through closure2's strict extent instead of
     * the seed lift's whole-function heuristic. */
    remove(closed_c);
    remove(closed_list);
    remove(closure_seed_c);
    if (append_file(seed_c, closure_seed_c) != 0 ||
        append_file(edge_c, closure_seed_c) != 0) {
        fprintf(stderr, "cannot assemble closure seed (see %s / %s)\n",
                seed_c, edge_c);
        if (have_pe) pe_close(&pe);
        return 1;
    }
    join(log, sizeof log, "%s/step_closure.log", o.out_dir);
    rc = run_step(log,
                  "\"%s/closure2.exe\" \"%s\" \"%s\" \"%s\" \"%s\" "
                  "--workdir \"%s\"",
                  o.tools, o.image, closure_seed_c, seed_list, closed_c,
                  o.work_dir);
    if (rc != 0) {
        fprintf(stderr, "closure2.exe did not converge (see %s)\n", log);
        if (have_pe) pe_close(&pe);
        return 1;
    }
    r.converged = 1;
    parse_closure_log(&r, log);
    r.closed_c_bytes = file_bytes(closed_c);
    hash_into(closed_c, r.closed_c_sha);

    /* 5. Authoritative union from the C sources, not the per-round headers. */
    join(log, sizeof log, "%s/step_known.log", o.out_dir);
    rc = run_step(log, "\"%s/known.exe\" \"%s\" --list \"%s\"",
                  o.tools, o.out_dir, merged);
    if (rc != 0) { fprintf(stderr, "known.exe failed (see %s)\n", log); return 1; }
    r.known_entries = count_x(merged);
    hash_into(merged, r.merged_sha);

    /* 6/7. Lift invariants on the closed corpus and on the V5.4 fixture. */
    join(report_dir, sizeof report_dir, "%s/reports/generated", o.root);
    if (mkdir_p(report_dir) != 0) die("cannot create reports/generated");
    stem_of(o.report, stem, sizeof stem);
    join(r.good_json, sizeof r.good_json, "%s/%s_liftpass.json", report_dir, stem);
    join(r.bad_json, sizeof r.bad_json, "%s/%s_liftpass_bad.json", report_dir, stem);

    join(log, sizeof log, "%s/step_liftpass.log", o.out_dir);
    r.lp_exit = run_step(log,
                         "\"%s/liftpass.exe\" check \"%s\" --list \"%s\" "
                         "--json \"%s\" --self-test",
                         o.tools, o.out_dir, merged, r.good_json);
    good = read_text(r.good_json);
    r.lp_pass = json_has(good, "\"verdict\": \"pass\"");
    r.self_test_ok = json_has(good, "\"self_test\": \"pass\"");
    r.lp_files = json_int(good, "files", 0);
    r.lp_functions = json_int(good, "functions", 0);
    r.lp_targets = json_int(good, "constant_targets", 0);
    r.lp_violations = 0;
    if (good) {
        const char *q = good;
        while ((q = strstr(q, "\"kind\":")) != NULL) { r.lp_violations++; q += 7; }
    }
    classify_violations(&r, good, have_pe, &pe);

    join(log, sizeof log, "%s/step_liftpass_bad.log", o.out_dir);
    r.bad_exit = run_step(log,
                          "\"%s/liftpass.exe\" check \"%s\" --list \"%s\" "
                          "--json \"%s\" --self-test",
                          o.tools, o.bad_fix, merged, r.bad_json);
    parse_bad_report(&r, r.bad_json);
    r.detectors_ok = r.bad_exit != 0;
    for (i = 0; i < NDETECTORS; i++) if (!r.detectors[i]) r.detectors_ok = 0;
    r.ndetectors = 0;
    for (i = 0; i < NDETECTORS; i++) if (r.detectors[i]) r.ndetectors++;

    r.closure_pass = (r.converged && r.lp_in_image_unknown == 0 &&
                      r.detectors_ok && r.self_test_ok);
    r.residual = r.lp_violations;
    r.verdict_pass = r.closure_pass && r.lp_exit == 0 && r.lp_pass &&
                     r.residual == 0;
    if (have_pe) pe_close(&pe);
    free(good);

    if (mkdir_p(report_dir) == 0) write_report(o.report, &r);
    printf("closure_pipeline: %s (closure %s, %d residual violations) -> %s\n",
           r.verdict_pass ? "pass" : "fail",
           r.closure_pass ? "pass" : "fail", r.residual, o.report);
    return r.verdict_pass ? 0 : 1;
}
