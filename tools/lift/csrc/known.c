/* known.c - collect the set of lift entries that the build actually compiles.
 *
 *   known <dir> --list out.h [--funcs out.txt] [--prefix NAME] [--quiet]
 *
 * Walks <dir> recursively, finds every `void L_XXXXXXXX(CPU` definition, and
 * writes the deduplicated address set in the two shapes the build needs:
 *
 *   --list    an X-macro header for the dispatcher (`LIFTED_FUNCS(X)`)
 *   --funcs   a `0x.. 16 name` catalog for seed/lift32
 *
 * The authoritative set has to come from the C sources, not from the *_list.h
 * headers: several lift files concatenate fragments whose addresses were never
 * added to a header, and a second definition of the same address breaks the
 * link.  Scanning the sources is the only safe answer.
 *
 * Replaces the PowerShell Get-KnownAddrs/Write-MergedKnown pair, which pushed
 * the whole ~350 MB corpus through .NET regexes twice per probe round (~4 s
 * each on a warm cache, far worse cold).  This walks the same bytes with
 * memchr and finishes in well under a second.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MAX_DEPTH 16

static unsigned *g_addr;
static size_t g_n, g_cap;

static void die(const char *msg)
{
    fprintf(stderr, "[!] %s\n", msg);
    exit(1);
}

static void push(unsigned a)
{
    if (g_n == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 8192;
        g_addr = (unsigned *)realloc(g_addr, g_cap * sizeof(unsigned));
        if (!g_addr) die("out of memory");
    }
    g_addr[g_n++] = a;
}

static int cmp_u32(const void *a, const void *b)
{
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    return x < y ? -1 : x > y;
}

static int hexval(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* `void L_1234ABCD(CPU *c)` -> 0x1234ABCD.  The definition always starts at
 * column 0, so anchoring on "\nvoid L_" (plus the start of the buffer) is
 * enough and skips the thousands of `L_...` call sites in the body. */
static void scan_source(const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf, *p, *end;
    if (!f) { fprintf(stderr, "[!] cannot read %s\n", path); return; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); die("out of memory"); }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return; }
    buf[sz] = 0;
    fclose(f);

    end = buf + sz;
    p = buf;
    while (p < end) {
        char *hit = (char *)memchr(p, 'v', (size_t)(end - p));
        if (!hit) break;
        if ((hit == buf || hit[-1] == '\n') && end - hit > 15 &&
            !memcmp(hit, "void L_", 7) &&
            !memcmp(hit + 15, "(CPU", 4)) {
            unsigned v = 0;
            int k, ok = 1;
            for (k = 0; k < 8; k++) {
                int d = hexval((unsigned char)hit[7 + k]);
                if (d < 0) { ok = 0; break; }
                v = (v << 4) | (unsigned)d;
            }
            if (ok) push(v);
        }
        p = hit + 1;
    }
    free(buf);
}

static int ends_with_ci(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    if (ls < lf) return 0;
    return _stricmp(s + ls - lf, suf) == 0;
}

static void walk(const char *dir, int depth)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    if (depth > MAX_DEPTH) return;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[!] cannot enumerate %s\n", dir);
        return;
    }
    do {
        char path[MAX_PATH * 2];
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!strcmp(fd.cFileName, ".git") || !strcmp(fd.cFileName, "__pycache__")) continue;
            walk(path, depth + 1);
        } else if (ends_with_ci(fd.cFileName, ".c")) {
            scan_source(path);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

int main(int argc, char **argv)
{
    const char *dir = NULL, *list_out = NULL, *funcs_out = NULL;
    const char *prefix = "known_";
    int quiet = 0, i;
    size_t k, unique;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list") && i + 1 < argc) list_out = argv[++i];
        else if (!strcmp(argv[i], "--funcs") && i + 1 < argc) funcs_out = argv[++i];
        else if (!strcmp(argv[i], "--prefix") && i + 1 < argc) prefix = argv[++i];
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (argv[i][0] != '-' && !dir) dir = argv[i];
        else {
            fprintf(stderr, "usage: %s <dir> --list <out.h> [--funcs <out.txt>] "
                            "[--prefix known_] [--quiet]\n", argv[0]);
            return 2;
        }
    }
    if (!dir || (!list_out && !funcs_out)) {
        fprintf(stderr, "usage: %s <dir> --list <out.h> [--funcs <out.txt>]\n", argv[0]);
        return 2;
    }

    walk(dir, 0);
    if (!g_n) die("no lift entries found");

    qsort(g_addr, g_n, sizeof(unsigned), cmp_u32);
    unique = 0;
    for (k = 0; k < g_n; k++)
        if (!unique || g_addr[unique - 1] != g_addr[k]) g_addr[unique++] = g_addr[k];

    if (list_out) {
        FILE *f = fopen(list_out, "wb");
        if (!f) { fprintf(stderr, "[!] cannot write %s\n", list_out); return 1; }
        fputs("/* AUTO-GENERATED by lift32_cpu.py */\n#define LIFTED_FUNCS(X) \\\n", f);
        for (k = 0; k < unique; k++)
            fprintf(f, "    X(%08X)%s\n", g_addr[k], k + 1 < unique ? " \\" : "");
        fclose(f);
    }
    if (funcs_out) {
        FILE *f = fopen(funcs_out, "wb");
        if (!f) { fprintf(stderr, "[!] cannot write %s\n", funcs_out); return 1; }
        for (k = 0; k < unique; k++)
            fprintf(f, "0x%08x 16 %s%08X\n", g_addr[k], prefix, g_addr[k]);
        fclose(f);
    }
    if (!quiet)
        fprintf(stderr, "%s: %zu entries (%zu definitions, %zu duplicates)\n",
                dir, unique, g_n, g_n - unique);
    free(g_addr);
    return 0;
}
