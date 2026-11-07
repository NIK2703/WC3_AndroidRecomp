/* splitcorpus.c - cut generated lift output into build-sized translation units.
 *
 *   splitcorpus <generated.c> <out-dir> <prefix> [funcs-per-chunk] [max-mb]
 *
 * The lift generator emits one C file per module.  game.dll alone is ~300 MB,
 * which is not a translation unit a compiler can optimise in bounded time or
 * memory.  This tool cuts at function boundaries (a line starting with
 * "void L_") and writes chunks of at most N functions and M megabytes.  The
 * output is deterministic: same input, same chunk names and bytes.
 *
 * The original file header is replaced by a minimal header that includes
 * "cpu.h" for every chunk.  Lines before the first function are dropped.
 *
 *   splitcorpus build/corpus-full/game/closed.c \
 *       build/relifted/game game 500 64
 *
 * Exit codes: 0 pass, 1 I/O or usage error.
 */
#include "dirwalk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
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

#define LINE_CAP (1u << 20)

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

static const char *base_name(const char *path)
{
    const char *base = path, *p;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

int main(int argc, char **argv)
{
    const char *src, *outdir, *prefix;
    long per = 500, max_mb = 0;
    char header[DIR_MAX_PATH + 128];
    char outpath[DIR_MAX_PATH];
    FILE *in, *out = NULL;
    char *line;
    size_t header_len, written = 0;
    long count = 0, total = 0, chunk = 0;
    int at_line_start = 1;

    if (argc < 4 || argc > 6) {
        fprintf(stderr,
                "usage: splitcorpus <generated.c> <out-dir> <prefix> "
                "[funcs-per-chunk] [max-mb]\n");
        return 1;
    }
    src = argv[1];
    outdir = argv[2];
    prefix = argv[3];
    if (argc > 4) per = strtol(argv[4], NULL, 10);
    if (argc > 5) max_mb = strtol(argv[5], NULL, 10);
    if (per <= 0) {
        fprintf(stderr, "splitcorpus: funcs-per-chunk must be positive\n");
        return 1;
    }
    if (mkdir_p(outdir) != 0) {
        fprintf(stderr, "splitcorpus: cannot create %s\n", outdir);
        return 1;
    }
    in = fopen(src, "rb");
    if (!in) {
        fprintf(stderr, "splitcorpus: cannot open %s\n", src);
        return 1;
    }
    line = (char *)malloc(LINE_CAP);
    if (!line) {
        fprintf(stderr, "splitcorpus: out of memory\n");
        fclose(in);
        return 1;
    }
    snprintf(header, sizeof header,
             "/* AUTO-GENERATED split of %s - do not edit */\n"
             "#include \"cpu.h\"\n\n",
             base_name(src));
    header_len = strlen(header);

    while (fgets(line, (int)LINE_CAP, in)) {
        size_t len = strlen(line);
        int is_func = at_line_start && !strncmp(line, "void L_", 7);

        if (is_func) {
            if (!out || count >= per ||
                (max_mb > 0 && count > 0 &&
                 written + len > (size_t)max_mb * 1024u * 1024u)) {
                if (out) fclose(out);
                snprintf(outpath, sizeof outpath, "%s/%s_%04ld.c",
                         outdir, prefix, chunk++);
                out = fopen(outpath, "wb");
                if (!out) {
                    fprintf(stderr, "splitcorpus: cannot write %s\n", outpath);
                    free(line);
                    fclose(in);
                    return 1;
                }
                if (fwrite(header, 1, header_len, out) != header_len) {
                    fprintf(stderr, "splitcorpus: write failed: %s\n", outpath);
                    free(line);
                    fclose(in);
                    fclose(out);
                    return 1;
                }
                written = header_len;
                count = 0;
            }
            count++;
            total++;
        }
        if (!out) {
            /* Header lines before the first function are intentionally dropped. */
            at_line_start = len > 0 && line[len - 1] == '\n';
            continue;
        }
        if (fwrite(line, 1, len, out) != len) {
            fprintf(stderr, "splitcorpus: write failed: %s\n", outpath);
            free(line);
            fclose(in);
            fclose(out);
            return 1;
        }
        written += len;
        at_line_start = len > 0 && line[len - 1] == '\n';
        if (!at_line_start) {
            /* A line longer than LINE_CAP: keep consuming it.  Function
             * detection is only valid at a real line start, and the next
             * fgets call returns the continuation with at_line_start=0. */
        }
    }
    if (out) fclose(out);
    fclose(in);
    free(line);
    if (total == 0) {
        fprintf(stderr, "splitcorpus: no L_ functions in %s\n", src);
        return 1;
    }
    printf("splitcorpus: %s: %ld functions -> %ld file(s) of <=%ld%s in %s\n",
           base_name(src), total, chunk, per,
           max_mb > 0 ? " and size cap" : "", outdir);
    return 0;
}
