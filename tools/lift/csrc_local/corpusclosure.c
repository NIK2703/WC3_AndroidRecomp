/* corpusclosure.c - compiled C driver for the whole 1.27b module set.
 *
 * G-05 declared seven real lift inputs; closure_pipeline.exe only ever lifted
 * one of them.  This tool closes that gap: it verifies the pinned input set,
 * runs the compiled closure loop for every module (including the four rebased
 * PE copies), and publishes one aggregate report.  No generated C leaves
 * build/ (R2/R3), and no Python is involved in the loop.
 *
 *   corpusclosure --input-list build/corpus-inputs/inputs.txt \
 *       --root . --tools build/lift-tools \
 *       --out-root build/corpus-closure --work-root build/corpus-closure-work \
 *       --corpus-hash reports/generated/corpus.sha256 \
 *       --layout-file runtime/guest/guest_layout.h \
 *       --report reports/generated/corpus_closure.json \
 *       [--max-functions N]
 *
 * Exit codes: 0 all modules pass, 1 a module failed, 2 unavailable.
 */
#include "sha256.h"
#include "dirwalk.h"

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
#define MKDIR(p) mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0775)
#endif
#endif

#define MAX_MODULES 16
/* Seven DIR_MAX_PATH arguments can theoretically exceed a smaller buffer;
 * snprintf truncation warnings are not acceptable in a build gate. */
#define CMD_CAP 32768

typedef struct {
    char name[128];
    char path[DIR_MAX_PATH];
    char sha[65];
    char report[DIR_MAX_PATH];
    int present;
    int verdict_pass;
    int seed_entries;
    int rounds;
    int definitions;
    int constant_targets;
    int residual;
    int in_image_unknown;
    int closure_pass;
    int unknown_count;
    int no_terminal_count;
    char external[256];
} Module;

typedef struct {
    char root[DIR_MAX_PATH];
    char tools[DIR_MAX_PATH];
    char input_list[DIR_MAX_PATH];
    char out_root[DIR_MAX_PATH];
    char work_root[DIR_MAX_PATH];
    char corpus_hash[DIR_MAX_PATH];
    char layout_file[DIR_MAX_PATH];
    char report[DIR_MAX_PATH];
    long max_functions;
    Module mod[MAX_MODULES];
    int nmod;
} Opts;

static void join_path(char *dst, size_t cap, const char *fmt, ...)
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

static int json_int(const char *t, const char *key, int dflt)
{
    char pat[64];
    const char *p;
    if (!t) return dflt;
    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(t, pat);
    if (!p) return dflt;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '-' && !isdigit((unsigned char)*p)) return dflt;
    return atoi(p);
}

static int json_string(const char *t, const char *key, char *out, size_t cap)
{
    char pat[64];
    const char *p;
    size_t n = 0;
    if (!t) return -1;
    snprintf(pat, sizeof pat, "\"%s\": \"", key);
    p = strstr(t, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p && *p != '"' && n + 1 < cap) {
        if (p[0] == '\\' && p[1] == '\\') { out[n++] = '\\'; p += 2; continue; }
        out[n++] = *p++;
    }
    out[n] = 0;
    return 0;
}

/* Run a compiled tool, capturing stdout+stderr into a log.  cmd.exe mangles a
 * command line starting with a quoted path containing spaces, so Windows uses
 * CreateProcessA with inherited handles (same approach as closure_pipeline.c).
 */
static int run_tool(const char *log_path, const char *fmt, ...)
{
    char cmd[CMD_CAP];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    printf("+ %s\n", cmd);
    fflush(stdout);
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
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

static void basename_stem(const char *path, char *out, size_t cap)
{
    const char *base = path, *p, *dot;
    for (p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    dot = strrchr(base, '.');
    if (!dot) dot = base + strlen(base);
    if ((size_t)(dot - base) >= cap) dot = base + cap - 1;
    memcpy(out, base, (size_t)(dot - base));
    out[dot - base] = 0;
}

static void parse_args(int argc, char **argv, Opts *o)
{
    char cwd[DIR_MAX_PATH];
    int i;
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    GetCurrentDirectoryA(sizeof cwd, cwd);
#else
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
#endif
    path_slash(cwd);
    /* --root changes every default. */
    snprintf(o->root, sizeof o->root, "%s", cwd);
    for (i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--root")) {
            snprintf(o->root, sizeof o->root, "%s", argv[i + 1]);
            path_slash(o->root);
        }
    join_path(o->tools, sizeof o->tools, "%s/build/lift-tools", o->root);
    join_path(o->input_list, sizeof o->input_list,
              "%s/build/corpus-inputs/inputs.txt", o->root);
    join_path(o->out_root, sizeof o->out_root,
              "%s/build/corpus-closure", o->root);
    join_path(o->work_root, sizeof o->work_root,
              "%s/build/corpus-closure-work", o->root);
    join_path(o->corpus_hash, sizeof o->corpus_hash,
              "%s/reports/generated/corpus.sha256", o->root);
    join_path(o->layout_file, sizeof o->layout_file,
              "%s/runtime/guest/guest_layout.h", o->root);
    join_path(o->report, sizeof o->report,
              "%s/reports/generated/corpus_closure.json", o->root);
    o->max_functions = 0;
    o->nmod = 0;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--root") && i + 1 < argc) i++;
        else if (!strcmp(a, "--tools") && i + 1 < argc) snprintf(o->tools, sizeof o->tools, "%s", argv[++i]);
        else if (!strcmp(a, "--input-list") && i + 1 < argc) snprintf(o->input_list, sizeof o->input_list, "%s", argv[++i]);
        else if (!strcmp(a, "--out-root") && i + 1 < argc) snprintf(o->out_root, sizeof o->out_root, "%s", argv[++i]);
        else if (!strcmp(a, "--work-root") && i + 1 < argc) snprintf(o->work_root, sizeof o->work_root, "%s", argv[++i]);
        else if (!strcmp(a, "--corpus-hash") && i + 1 < argc) snprintf(o->corpus_hash, sizeof o->corpus_hash, "%s", argv[++i]);
        else if (!strcmp(a, "--layout-file") && i + 1 < argc) snprintf(o->layout_file, sizeof o->layout_file, "%s", argv[++i]);
        else if (!strcmp(a, "--report") && i + 1 < argc) snprintf(o->report, sizeof o->report, "%s", argv[++i]);
        else if (!strcmp(a, "--max-functions") && i + 1 < argc) o->max_functions = atol(argv[++i]);
        else {
            fprintf(stderr, "usage: corpusclosure --input-list F [--root DIR] "
                    "[--tools DIR] [--out-root DIR] [--work-root DIR] "
                    "[--corpus-hash F] [--layout-file F] [--report F] "
                    "[--max-functions N]\n");
            exit(2);
        }
    }
}

static int load_inputs(Opts *o)
{
    char *text = read_text(o->input_list);
    char *line, *next;
    if (!text) { fprintf(stderr, "corpusclosure: cannot read %s\n", o->input_list); return -1; }
    for (line = text; *line; line = next) {
        char *end = strchr(line, '\n');
        char *p;
        Module *m;
        next = end ? end + 1 : line + strlen(line);
        if (end) *end = 0;
        line[strcspn(line, "\r")] = 0;
        p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        if (o->nmod >= MAX_MODULES) { fprintf(stderr, "too many inputs\n"); free(text); return -1; }
        m = &o->mod[o->nmod++];
        memset(m, 0, sizeof *m);
        snprintf(m->path, sizeof m->path, "%s", p);
        basename_stem(p, m->name, sizeof m->name);
    }
    free(text);
    return o->nmod ? 0 : -1;
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

/* First "0xXXXXXXXX" inside the external_pseudo_targets array, for triage. */
static void first_external(const char *rep, char *out, size_t cap)
{
    const char *p = rep ? strstr(rep, "\"external_pseudo_targets\": [") : NULL;
    char hex[16];
    if (!p) { if (cap) out[0] = 0; return; }
    p = strstr(p, "0x");
    if (!p) { if (cap) out[0] = 0; return; }
    snprintf(hex, sizeof hex, "%.8s", p + 2);
    snprintf(out, cap, "0x%s", hex);
}

static int write_report(Opts *o, const char *corpus_sha,
                        int passed, int total_def, int total_targets,
                        int total_residual)
{
    FILE *f = fopen(o->report, "w");
    int i;
    if (!f) return -1;
    fprintf(f, "{\n  \"schema\": 1,\n  \"published\": false,\n");
    fprintf(f, "  \"corpus_sha256\": \"%s\",\n", corpus_sha);
    fprintf(f, "  \"generator\": \"wc3lift\",\n  \"lift_rev\": 1,\n");
    fprintf(f, "  \"seed_mode\": \"block\",\n");
    fprintf(f, "  \"max_functions\": %ld,\n", o->max_functions);
    fprintf(f, "  \"modules\": [\n");
    for (i = 0; i < o->nmod; i++) {
        Module *m = &o->mod[i];
        fprintf(f, "    {\"name\": "); json_str(f, m->name);
        fprintf(f, ", \"path\": "); json_str(f, m->path);
        fprintf(f, ", \"sha256\": \"%s\", \"verdict\": \"%s\", "
                "\"closure_verdict\": \"%s\", \"lint_verdict\": \"%s\", "
                "\"seed_entries\": %d, \"rounds\": %d, "
                "\"definitions\": %d, \"constant_targets\": %d, "
                "\"residual_violations\": %d, \"in_image_unknown_target\": %d, "
                "\"report\": ", m->sha, m->verdict_pass ? "pass" : "fail",
                m->closure_pass ? "pass" : "fail",
                m->residual == 0 ? "pass" : "fail",
                m->seed_entries, m->rounds, m->definitions,
                m->constant_targets, m->residual, m->in_image_unknown);
        json_str(f, m->report);
        fprintf(f, "}%s\n", i + 1 < o->nmod ? "," : "");
    }
    fprintf(f, "  ],\n  \"totals\": {\"modules\": %d, \"passed\": %d, "
            "\"definitions\": %d, \"constant_targets\": %d, "
            "\"residual_violations\": %d},\n",
            o->nmod, passed, total_def, total_targets, total_residual);
    {
        int closure_ok = 0, lint_ok = 0, first = 1;
        for (i = 0; i < o->nmod; i++) {
            if (o->mod[i].closure_pass) closure_ok++;
            if (o->mod[i].residual == 0) lint_ok++;
        }
        fprintf(f, "  \"closure_verdict\": \"%s\",\n",
                closure_ok == o->nmod ? "pass" : "fail");
        fprintf(f, "  \"lint_verdict\": \"%s\",\n",
                lint_ok == o->nmod ? "pass" : "fail");
        fprintf(f, "  \"closure_passed\": %d, \"lint_passed\": %d,\n",
                closure_ok, lint_ok);
        fprintf(f, "  \"findings\": [");
        for (i = 0; i < o->nmod; i++) {
            Module *m = &o->mod[i];
            if (m->residual == 0) continue;
            if (!first) fputs(", ", f);
            first = 0;
            fprintf(f, "{\"module\": \"%s\", \"unknown_target\": %d, "
                    "\"no_terminal\": %d, \"external\": \"%s\"}",
                    m->name, m->unknown_count, m->no_terminal_count,
                    m->external[0] ? m->external : "");
        }
        fprintf(f, "],\n");
    }
    fprintf(f, "  \"verdict\": \"%s\",\n",
            (passed == o->nmod && total_residual == 0) ? "pass" : "fail");
    fprintf(f, "  \"outputs\": {\"out_root\": "); json_str(f, o->out_root);
    fprintf(f, ", \"work_root\": "); json_str(f, o->work_root);
    fprintf(f, "}\n}\n");
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    Opts o;
    char cmd[CMD_CAP], log[DIR_MAX_PATH], outdir[DIR_MAX_PATH];
    char workdir[DIR_MAX_PATH], tmp[DIR_MAX_PATH], corpus_sha[65] = "";
    char *verify_log_text, *hash_text;
    int i, rc, passed = 0, total_def = 0, total_targets = 0, total_residual = 0;

    parse_args(argc, argv, &o);
    /* sanity: required binaries and files exist */
    {
        const char *need[] = {"closure_pipeline.exe", "corpus.exe", NULL};
        for (i = 0; need[i]; i++) {
            join_path(tmp, sizeof tmp, "%s/%s", o.tools, need[i]);
            FILE *probe = fopen(tmp, "rb");
            if (!probe) {
                fprintf(stderr, "corpusclosure: unavailable (%s missing; run build_tools.ps1)\n",
                        need[i]);
                return 2;
            }
            fclose(probe);
        }
    }
    hash_text = read_text(o.corpus_hash);
    if (!hash_text || strlen(hash_text) < 64) {
        fprintf(stderr, "corpusclosure: unavailable (run corpusinput prepare + corpus hash)\n");
        free(hash_text);
        return 2;
    }
    memcpy(corpus_sha, hash_text, 64); corpus_sha[64] = 0;
    free(hash_text);
    if (load_inputs(&o) != 0) return 2;
    if (mkdir_p(o.out_root) != 0 || mkdir_p(o.work_root) != 0) return 2;

    /* 1. The whole input set is verified before any module is lifted. */
    join_path(log, sizeof log, "%s/00_corpus_verify.log", o.out_root);
    rc = run_tool(log,
                  "\"%s/corpus.exe\" verify --inputs \"%s\" --root \"%s\" "
                  "--generator wc3lift --lift-rev 1 --layout-file \"%s\" "
                  "--expected \"%s\"",
                  o.tools, o.input_list, o.root, o.layout_file, o.corpus_hash);
    verify_log_text = read_text(log);
    if (rc != 0) {
        fprintf(stderr, "corpusclosure: corpus identity verify FAILED\n%s\n",
                verify_log_text ? verify_log_text : "");
        free(verify_log_text);
        return 1;
    }
    free(verify_log_text);
    printf("corpusclosure: corpus identity pass %s (%d inputs)\n",
           corpus_sha, o.nmod);

    /* 2. One compiled closure loop per module. */
    for (i = 0; i < o.nmod; i++) {
        Module *m = &o.mod[i];
        char *rep;
        char image[DIR_MAX_PATH];
        join_path(image, sizeof image, "%s/%s", o.root, m->path);
        join_path(outdir, sizeof outdir, "%s/%s", o.out_root, m->name);
        join_path(workdir, sizeof workdir, "%s/%s", o.work_root, m->name);
        join_path(m->report, sizeof m->report, "%s/%s.closure.json",
                  outdir, m->name);
        if (mkdir_p(outdir) != 0 || mkdir_p(workdir) != 0) return 2;
        if (sha256_file(image, m->sha) != 0) {
            fprintf(stderr, "corpusclosure: %s: image missing\n", m->name);
            continue;
        }
        join_path(log, sizeof log, "%s/pipeline.log", outdir);
        if (o.max_functions > 0)
            snprintf(cmd, sizeof cmd,
                     "\"%s/closure_pipeline.exe\" --image \"%s\" "
                     "--skip-identity-check --out-dir \"%s\" "
                     "--work-dir \"%s\" --report \"%s\" --root \"%s\" "
                     "--tools \"%s\" --max-functions %ld",
                     o.tools, image, outdir, workdir, m->report, o.root,
                     o.tools, o.max_functions);
        else
            snprintf(cmd, sizeof cmd,
                     "\"%s/closure_pipeline.exe\" --image \"%s\" "
                     "--skip-identity-check --out-dir \"%s\" "
                     "--work-dir \"%s\" --report \"%s\" --root \"%s\" "
                     "--tools \"%s\"",
                     o.tools, image, outdir, workdir, m->report, o.root,
                     o.tools);
        rc = run_tool(log, "%s", cmd);
        rep = read_text(m->report);
        if (!rep) {
            fprintf(stderr, "corpusclosure: %s: no report (rc=%d)\n", m->name, rc);
            m->verdict_pass = 0;
            m->residual = -1;
        } else {
            char verdict[32] = "", cv[32] = "";
            json_string(rep, "verdict", verdict, sizeof verdict);
            json_string(rep, "closure_verdict", cv, sizeof cv);
            m->verdict_pass = !strcmp(verdict, "pass");
            m->closure_pass = !strcmp(cv, "pass");
            m->seed_entries = json_int(rep, "seed_entries", 0);
            m->rounds = json_int(rep, "rounds", 0);
            m->definitions = json_int(rep, "merged_entries", 0);
            m->constant_targets = json_int(rep, "constant_targets", 0);
            m->residual = json_int(rep, "residual_violations", 0);
            m->in_image_unknown = json_int(rep, "in_image_unknown_target", 0);
            m->unknown_count = json_int(rep, "unknown_target", 0);
            m->no_terminal_count = json_int(rep, "no_terminal", 0);
            first_external(rep, m->external, sizeof m->external);
            free(rep);
        }
        m->present = 1;
        if (m->verdict_pass) passed++;
        if (m->residual > 0) total_residual += m->residual;
        total_def += m->definitions;
        total_targets += m->constant_targets;
        printf("corpusclosure: %-16s %s (%d definitions, %d targets, %d residual)\n",
               m->name, m->verdict_pass ? "pass" : "FAIL",
               m->definitions, m->constant_targets, m->residual);
    }

    if (write_report(&o, corpus_sha, passed, total_def, total_targets,
                     total_residual) != 0) {
        fprintf(stderr, "corpusclosure: cannot write %s\n", o.report);
        return 1;
    }
    printf("corpusclosure: %s (%d/%d modules, %d definitions, %d targets, "
           "%d residual) -> %s\n",
           (passed == o.nmod && total_residual == 0) ? "pass" : "fail",
           passed, o.nmod, total_def, total_targets, total_residual, o.report);
    return (passed == o.nmod && total_residual == 0) ? 0 : 1;
}
