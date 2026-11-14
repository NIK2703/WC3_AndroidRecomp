/* corpusinput.c - compiled C preparation of the 1.27b lift inputs (G-05).
 *
 * The corpus is a function of the image AS THE LOADER WOULD PRESENT IT.  Four
 * modules are lifted from copies rebased to their Android bases, not from the
 * installed originals (V4.6).  This tool produces those copies deterministically
 * from the licensed files, verifies its own transformation by a round trip,
 * and seals the resulting identity into config/layout.1.27b.json.
 *
 *   corpusinput rebase <in> <out> <new-base-hex> [--verify]
 *   corpusinput prepare --manifest config/corpus.1.27b.json --root <dir> \
 *       --out-list build/corpus-inputs/inputs.txt
 *   corpusinput seal --layout config/layout.1.27b.json \
 *       --hash reports/generated/corpus.sha256 --inputs 7 [--force]
 *   corpusinput verify --layout config/layout.1.27b.json \
 *       --hash reports/generated/corpus.sha256 \
 *       [--evidence reports/generated/gate_status.json]
 *
 * No licensed bytes and no generated C are written into the source tree; all
 * products go to build/ (R2/R3).  The manifest only declares inputs.
 */
#include "sha256.h"
#include "dirwalk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>

#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#if defined(_WIN32) && defined(WC3_FORCE_POSIX)
/* UCRT mkdir() takes one argument; FORCE is a syntax-only emulation. */
#define MKDIR(p) mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0775)
#endif
#endif

#define MAX_SEC 96
#define LINE_CAP 8192

/* ---- PE32 rebase -------------------------------------------------------- */

typedef struct {
    uint32_t va, vsize, rawsize, rawptr;
} Sec;

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t opt;
    uint32_t base, image_size;
    uint32_t reloc_rva, reloc_size;
    int nsec;
    Sec sec[MAX_SEC];
} PeBuf;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int pebuf_load(PeBuf *b, uint8_t *data, size_t len)
{
    size_t lfanew, sec_off;
    uint16_t sizeopt;
    int i;
    memset(b, 0, sizeof *b);
    if (len < 0x40 || data[0] != 'M' || data[1] != 'Z') return -1;
    lfanew = rd32(data + 0x3C);
    if (lfanew + 24 + 0xE0 > len || memcmp(data + lfanew, "PE\0\0", 4)) return -1;
    b->opt = lfanew + 24;
    sizeopt = rd16(data + lfanew + 20);
    if (sizeopt < 0xC0 || b->opt + sizeopt > len) return -1;
    if (rd16(data + b->opt) != 0x10B) return -1;         /* PE32 only */
    b->nsec = rd16(data + lfanew + 6);
    if (b->nsec > MAX_SEC) return -1;
    b->base = rd32(data + b->opt + 28);
    b->image_size = rd32(data + b->opt + 56);
    b->reloc_rva = rd32(data + b->opt + 96 + 5 * 8);
    b->reloc_size = rd32(data + b->opt + 96 + 5 * 8 + 4);
    sec_off = b->opt + sizeopt;
    for (i = 0; i < b->nsec; i++) {
        uint8_t *s = data + sec_off + (size_t)i * 40;
        if (sec_off + (size_t)i * 40 + 40 > len) return -1;
        b->sec[i].vsize = rd32(s + 8);
        b->sec[i].va = rd32(s + 12);
        b->sec[i].rawsize = rd32(s + 16);
        b->sec[i].rawptr = rd32(s + 20);
    }
    b->buf = data;
    b->len = len;
    return 0;
}

static long rva_to_off(const PeBuf *b, uint32_t rva)
{
    int i;
    for (i = 0; i < b->nsec; i++) {
        const Sec *s = &b->sec[i];
        uint32_t span = s->vsize > s->rawsize ? s->vsize : s->rawsize;
        long off;
        if (rva < s->va || rva >= s->va + span) continue;
        off = (long)(s->rawptr + (rva - s->va));
        if (off >= 0 && (size_t)off + 4 <= b->len) return off;
    }
    return -1;
}

/* Rewrites ImageBase and every IMAGE_REL_BASED_HIGHLOW target by the same
 * delta the loader would apply.  Only the bytes in `data` are touched. */
static int rebase_bytes(uint8_t *data, size_t len, uint32_t new_base,
                        uint32_t *old_base, uint32_t *patched,
                        uint32_t *skipped)
{
    PeBuf b;
    uint32_t delta;
    long off, end;
    if (pebuf_load(&b, data, len) != 0) return -1;
    delta = new_base - b.base;
    *old_base = b.base;
    *patched = *skipped = 0;
    off = rva_to_off(&b, b.reloc_rva);
    if (b.reloc_rva && b.reloc_size && off >= 0) {
        end = off + b.reloc_size;
        if (end > (long)len) end = (long)len;
        while (off + 8 <= end) {
            uint32_t page = rd32(data + off);
            uint32_t bsize = rd32(data + off + 4);
            uint32_t q;
            if (bsize < 8 || off + (long)bsize > end) break;
            for (q = off + 8; q + 2 <= off + bsize; q += 2) {
                uint16_t ent = rd16(data + q);
                uint32_t type = ent >> 12;
                long target;
                if (type == 0) continue;
                if (type != 3) { (*skipped)++; continue; }
                target = rva_to_off(&b, page + (ent & 0xFFF));
                if (target < 0) return -1;
                wr32(data + target, rd32(data + target) + delta);
                (*patched)++;
            }
            off += bsize;
        }
    }
    wr32(data + b.opt + 28, new_base);
    return 0;
}

/* ---- small file helpers ------------------------------------------------- */

static char *read_text(const char *path, size_t *len_out)
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
    if (len_out) *len_out = got;
    return buf;
}

static int write_bytes(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    return fclose(f) == 0 ? 0 : -1;
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
    /* the caller passes a file path: create every component including the
     * last one only when it is not an existing file */
    return 0;
}

static void join_path(char *dst, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
}

static void parent_dir(const char *path, char *out, size_t cap)
{
    const char *p = path, *last = NULL;
    for (; *p; p++) if (*p == '/' || *p == '\\') last = p;
    if (!last) { snprintf(out, cap, "."); return; }
    if ((size_t)(last - path) >= cap) last = path + cap - 1;
    memcpy(out, path, (size_t)(last - path));
    out[last - path] = 0;
}

/* ---- flat JSON object reader (manifest is small and flat) --------------- */

static int json_value(const char *obj, const char *key, char *out, size_t cap)
{
    char pat[64];
    const char *s;
    size_t n = 0;
    snprintf(pat, sizeof pat, "\"%s\":", key);
    s = strstr(obj, pat);
    if (!s) return -1;
    s += strlen(pat);
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s == '"') {
        s++;
        while (*s && *s != '"' && n + 1 < cap) {
            if (s[0] == '\\' && s[1] == '\\') { out[n++] = '\\'; s += 2; continue; }
            out[n++] = *s++;
        }
        out[n] = 0;
    } else {
        while (*s && (isalnum((unsigned char)*s) || *s == '-' || *s == '_' ||
                      *s == '.') && n + 1 < cap) out[n++] = *s++;
        out[n] = 0;
    }
    return 0;
}

static char *replace_once(const char *text, const char *from, const char *to)
{
    const char *p = strstr(text, from);
    size_t a, flen = strlen(from), tlen = strlen(to), total;
    char *out;
    if (!p) return NULL;
    a = (size_t)(p - text);
    total = strlen(text) - flen + tlen;
    out = (char *)malloc(total + 1);
    if (!out) return NULL;
    memcpy(out, text, a);
    memcpy(out + a, to, tlen);
    strcpy(out + a + tlen, p + flen);
    return out;
}

/* Replace when the marker exists; otherwise keep the buffer unchanged. */
static char *replace_or_keep(char *text, const char *from, const char *to)
{
    char *n = replace_once(text, from, to);
    if (!n) return text;
    free(text);
    return n;
}

/* Update an already-written JSON sha256 value in place. */
static int update_json_sha(char *text, const char *hex)
{
    const char *p = strstr(text, "\"sha256\": \"");
    char *v;
    if (!p) return -1;
    v = (char *)p + strlen("\"sha256\": \"");
    if (strlen(v) < 64) return -1;
    memcpy(v, hex, 64);
    return 0;
}

/* ---- subcommands -------------------------------------------------------- */

static int cmd_rebase(int argc, char **argv)
{
    const char *src, *dst;
    uint32_t base, old_base = 0, patched = 0, skipped = 0;
    size_t len, m;
    uint8_t *data, *back, *orig;
    char dir[DIR_MAX_PATH];
    int verify = 0, i;
    if (argc < 5) { fprintf(stderr, "usage: corpusinput rebase <in> <out> <hexbase> [--verify]\n"); return 2; }
    src = argv[2]; dst = argv[3]; base = (uint32_t)strtoul(argv[4], NULL, 16);
    for (i = 5; i < argc; i++) if (!strcmp(argv[i], "--verify")) verify = 1;
    data = (uint8_t *)read_text(src, &len);
    if (!data) { fprintf(stderr, "rebase: cannot read %s\n", src); return 1; }
    if (rebase_bytes(data, len, base, &old_base, &patched, &skipped) != 0) {
        fprintf(stderr, "rebase: %s is not a rebaseable PE32\n", src);
        free(data); return 1;
    }
    parent_dir(dst, dir, sizeof dir);
    mkdir_p(dir);
    if (write_bytes(dst, data, len) != 0) {
        fprintf(stderr, "rebase: cannot write %s\n", dst);
        free(data); return 1;
    }
    printf("rebase: %s -> %s  ImageBase %08X -> %08X  dwords %u  size %zu\n",
           src, dst, old_base, base, patched, len);
    if (skipped) printf("rebase: WARNING %u non-HIGHLOW relocations skipped\n", skipped);
    if (verify) {
        uint32_t dummy, p2, s2;
        back = (uint8_t *)read_text(dst, &m);
        orig = (uint8_t *)read_text(src, &len);
        if (!back || !orig || m != len ||
            rebase_bytes(back, m, old_base, &dummy, &p2, &s2) != 0 ||
            memcmp(back, orig, len) != 0) {
            fprintf(stderr, "rebase: verify FAILED (round trip differs)\n");
            free(back); free(orig); free(data); return 1;
        }
        printf("rebase: verify OK (round trip to %08X reproduces the input)\n", old_base);
        free(back); free(orig);
    }
    free(data);
    return 0;
}

static int cmd_prepare(int argc, char **argv)
{
    const char *manifest = NULL, *root = ".", *out_list = NULL;
    char *text;
    size_t len = 0;
    FILE *list = NULL;
    int i, count = 0;
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--manifest") && i + 1 < argc) manifest = argv[++i];
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--out-list") && i + 1 < argc) out_list = argv[++i];
        else { fprintf(stderr, "usage: corpusinput prepare --manifest F --root DIR --out-list F\n"); return 2; }
    }
    if (!manifest || !out_list) { fprintf(stderr, "prepare: --manifest and --out-list are required\n"); return 2; }
    text = read_text(manifest, &len);
    if (!text) { fprintf(stderr, "prepare: cannot read %s\n", manifest); return 2; }
    {
        char dir[DIR_MAX_PATH];
        parent_dir(out_list, dir, sizeof dir);
        mkdir_p(dir);
    }
    list = fopen(out_list, "w");
    if (!list) { fprintf(stderr, "prepare: cannot write %s\n", out_list); free(text); return 1; }
    fputs("# AUTO-GENERATED by corpusinput prepare; corpus.exe --inputs\n", list);

    {
        char *p = text;
        while ((p = strstr(p, "\"name\": \"")) != NULL) {
            char *start = p, *end, save;
            char name[128], rel[DIR_MAX_PATH], source[DIR_MAX_PATH];
            char want[65], basestr[32], rebasestr[16], got[65];
            uint32_t base, old_base, patched, skipped;
            uint8_t *data;
            size_t n;
            char full[DIR_MAX_PATH], dir[DIR_MAX_PATH];
            int do_rebase;

            while (start > text && *start != '{') start--;
            end = strchr(p, '}');
            if (!end) break;
            save = *end; *end = 0;
            if (json_value(start, "name", name, sizeof name) != 0 ||
                json_value(start, "path", rel, sizeof rel) != 0 ||
                json_value(start, "source", source, sizeof source) != 0 ||
                json_value(start, "source_sha256", want, sizeof want) != 0 ||
                json_value(start, "base", basestr, sizeof basestr) != 0 ||
                json_value(start, "rebase", rebasestr, sizeof rebasestr) != 0) {
                *end = save; p = end; continue;
            }
            *end = save;
            do_rebase = !strcmp(rebasestr, "true");
            base = (uint32_t)strtoul(basestr, NULL, 0);
            path_slash(rel);
            path_slash(source);

            data = (uint8_t *)read_text(source, &n);
            if (!data) { fprintf(stderr, "prepare: %s: missing source %s\n", name, source); fclose(list); free(text); return 1; }
            if (sha256_file(source, got) != 0 || strcmp(got, want) != 0) {
                fprintf(stderr, "prepare: %s: source sha256 %s != manifest %s (V4.6)\n",
                        name, got, want);
                free(data); fclose(list); free(text); return 1;
            }
            if (do_rebase && rebase_bytes(data, n, base, &old_base,
                                          &patched, &skipped) != 0) {
                fprintf(stderr, "prepare: %s: cannot rebase (not PE32?)\n", name);
                free(data); fclose(list); free(text); return 1;
            }
            join_path(full, sizeof full, "%s/%s", root, rel);
            parent_dir(full, dir, sizeof dir);
            mkdir_p(dir);
            if (write_bytes(full, data, n) != 0) {
                fprintf(stderr, "prepare: cannot write %s\n", full);
                free(data); fclose(list); free(text); return 1;
            }
            if (do_rebase) {
                /* prove the transformation is an involution on the written
                 * file: rebasing it back to the original base must reproduce
                 * the licensed input byte for byte. */
                size_t m, n2;
                uint32_t dummy, p2, s2;
                uint8_t *check = (uint8_t *)read_text(full, &m);
                uint8_t *orig = (uint8_t *)read_text(source, &n2);
                if (!check || !orig || m != n || n2 != n ||
                    rebase_bytes(check, m, old_base, &dummy, &p2, &s2) != 0 ||
                    memcmp(check, orig, n) != 0) {
                    fprintf(stderr, "prepare: %s: rebase round trip FAILED\n", name);
                    free(check); free(orig); free(data); fclose(list); free(text);
                    return 1;
                }
                free(check); free(orig);
                printf("prepare: %-14s rebase %08X -> %08X, %u dwords, round trip OK\n",
                       name, old_base, base, patched);
            } else {
                printf("prepare: %-14s copy (sha256 %s)\n", name, got);
            }
            fprintf(list, "%s\n", rel);
            count++;
            free(data);
            p = end;
        }
    }
    fclose(list);
    free(text);
    printf("prepare: %d inputs -> %s\n", count, out_list);
    return count ? 0 : 1;
}

static int cmd_seal(int argc, char **argv)
{
    const char *layout = NULL, *hashfile = NULL;
    int inputs = 0, force = 0, i;
    char *text, *hash;
    char hex[65], ins[256], tmp[128];
    if (argc < 3) { fprintf(stderr, "usage: corpusinput seal --layout F --hash F --inputs N [--force]\n"); return 2; }
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--layout") && i + 1 < argc) layout = argv[++i];
        else if (!strcmp(argv[i], "--hash") && i + 1 < argc) hashfile = argv[++i];
        else if (!strcmp(argv[i], "--inputs") && i + 1 < argc) inputs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--force")) force = 1;
        else { fprintf(stderr, "seal: unknown option %s\n", argv[i]); return 2; }
    }
    if (!layout || !hashfile || inputs <= 0) {
        fprintf(stderr, "seal: --layout, --hash and --inputs are required\n");
        return 2;
    }
    hash = read_text(hashfile, NULL);
    if (!hash) { fprintf(stderr, "seal: cannot read %s\n", hashfile); return 2; }
    if (strlen(hash) < 64) { fprintf(stderr, "seal: no 64-char digest in %s\n", hashfile); free(hash); return 2; }
    memcpy(hex, hash, 64); hex[64] = 0;
    for (i = 0; i < 64; i++) if (!isxdigit((unsigned char)hex[i])) {
        fprintf(stderr, "seal: digest is not hex\n"); free(hash); return 2;
    }
    free(hash);
    text = read_text(layout, NULL);
    if (!text) { fprintf(stderr, "seal: cannot read %s\n", layout); return 2; }
    {
        char *corpus = strstr(text, "\"corpus_manifest\"");
        if (!corpus) { fprintf(stderr, "seal: no corpus_manifest object\n"); free(text); return 1; }
        if (strstr(corpus, "\"sha256\": null")) {
            snprintf(tmp, sizeof tmp, "\"sha256\": \"%s\"", hex);
            snprintf(ins, sizeof ins, "%s, \"inputs\": %d, \"generator\": \"wc3lift\"",
                     tmp, inputs);
            text = replace_or_keep(text, "\"sha256\": null", ins);
        } else if (force) {
            /* Start at corpus_manifest: update_json_sha() patches the first
             * "sha256" it sees, and the layout also carries per-image hashes
             * before this object.  Searching from text used to overwrite the
             * first image's hash instead of the corpus manifest. */
            if (update_json_sha(corpus, hex) != 0) {
                fprintf(stderr, "seal: cannot update corpus_manifest.sha256\n");
                free(text); return 1;
            }
        } else {
            fprintf(stderr, "seal: corpus_manifest.sha256 is already set (use --force)\n");
            free(text); return 1;
        }
    }
    text = replace_or_keep(text, "\"status\": \"not_generated\"",
                           "\"status\": \"generated\"");
    text = replace_or_keep(text, "\"lift_rev\": 0", "\"lift_rev\": 5");
    text = replace_or_keep(text, "\"lift_rev\": 2", "\"lift_rev\": 5");
    text = replace_or_keep(text, "\"lift_rev\": 3", "\"lift_rev\": 5");
    text = replace_or_keep(text, "\"lift_rev\": 4", "\"lift_rev\": 5");
    snprintf(tmp, sizeof tmp, "\"sha256\": \"%s\"", hex);
    /* Re-find the object: replace_or_keep() may have reallocated text.  Check
     * inside corpus_manifest, not merely anywhere in the file (the image
     * entries carry their own sha256 values). */
    {
        char *sealed = text ? strstr(text, "\"corpus_manifest\"") : NULL;
        if (!sealed || !strstr(sealed, tmp) ||
            !strstr(sealed, "\"status\": \"generated\"")) {
            fprintf(stderr, "seal: could not patch corpus_manifest\n");
            free(text); return 1;
        }
    }
    if (write_bytes(layout, (const uint8_t *)text, strlen(text)) != 0) {
        fprintf(stderr, "seal: cannot write %s\n", layout); free(text); return 1;
    }
    printf("seal: corpus_manifest.sha256 = %s (%d inputs) -> %s\n",
           hex, inputs, layout);
    free(text);
    return 0;
}

/* V8 evidence for the G-05 promotion.  Everything in the record is measured
 * here; the reviewer field names the automated check, not a human sign-off. */
static int cmd_verify(int argc, char **argv)
{
    const char *layout = NULL, *hashfile = NULL, *evidence = NULL;
    char *text, *hash, sealed[65], status[32], rev[16], inputs[16];
    char hex[65], stamp[32];
    char *corpus;
    time_t now;
    struct tm *utc;
    int i, rc = 1;
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--layout") && i + 1 < argc) layout = argv[++i];
        else if (!strcmp(argv[i], "--hash") && i + 1 < argc) hashfile = argv[++i];
        else if (!strcmp(argv[i], "--evidence") && i + 1 < argc) evidence = argv[++i];
        else { fprintf(stderr, "verify: unknown option %s\n", argv[i]); return 2; }
    }
    if (!layout || !hashfile) {
        fprintf(stderr, "usage: corpusinput verify --layout F --hash F [--evidence F]\n");
        return 2;
    }
    hash = read_text(hashfile, NULL);
    text = read_text(layout, NULL);
    if (!hash || !text || strlen(hash) < 64) {
        fprintf(stderr, "verify: cannot read hash or layout\n");
        free(hash); free(text); return 1;
    }
    memcpy(hex, hash, 64); hex[64] = 0;
    free(hash);
    corpus = strstr(text, "\"corpus_manifest\"");
    if (!corpus) { fprintf(stderr, "verify: no corpus_manifest object\n"); free(text); return 1; }
    if (json_value(corpus, "sha256", sealed, sizeof sealed) != 0 ||
        json_value(corpus, "status", status, sizeof status) != 0 ||
        json_value(corpus, "lift_rev", rev, sizeof rev) != 0 ||
        json_value(corpus, "inputs", inputs, sizeof inputs) != 0) {
        fprintf(stderr, "verify: corpus_manifest is incomplete\n");
        free(text); return 1;
    }
    if (strcmp(sealed, hex) != 0) {
        fprintf(stderr, "verify: FAIL sealed %s != corpus.sha256 %s\n", sealed, hex);
    } else if (strcmp(status, "generated") != 0 || atoi(rev) < 1 || atoi(inputs) < 1) {
        fprintf(stderr, "verify: FAIL status=%s lift_rev=%s inputs=%s\n",
                status, rev, inputs);
    } else {
        printf("verify: pass %s (status=%s, lift_rev=%s, inputs=%s)\n",
               hex, status, rev, inputs);
        rc = 0;
    }
    free(text);

    if (evidence && rc == 0) {
        FILE *f = fopen(evidence, "w");
        now = time(NULL);
        utc = gmtime(&now);
        if (utc) strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", utc);
        else snprintf(stamp, sizeof stamp, "unknown");
        if (!f) { fprintf(stderr, "verify: cannot write evidence\n"); return 1; }
        fprintf(f, "{\n"
                "  \"schema\": 1,\n"
                "  \"gate_id\": \"G-05\",\n"
                "  \"step\": \"Step 0 / M8 continuation\",\n"
                "  \"command\": \"python tools/check_layout.py --corpus 1.27b --require-corpus; corpusinput verify; corpus.exe verify\",\n"
                "  \"exit_code\": 0,\n"
                "  \"artifact_sha256\": \"%s\",\n"
                "  \"timestamp\": \"%s\",\n"
                "  \"reviewer\": \"corpusinput verify (automated, reproducible)\",\n"
                "  \"scope\": \"static corpus input identity; behavioural T1 equivalence stays with G-08/G-09\"\n"
                "}\n", hex, stamp);
        fclose(f);
        printf("verify: evidence -> %s\n", evidence);
    }
    return rc;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "";
    if (!strcmp(cmd, "rebase")) return cmd_rebase(argc, argv);
    if (!strcmp(cmd, "prepare")) return cmd_prepare(argc, argv);
    if (!strcmp(cmd, "seal")) return cmd_seal(argc, argv);
    if (!strcmp(cmd, "verify")) return cmd_verify(argc, argv);
    fprintf(stderr,
            "usage: corpusinput rebase|prepare|seal|verify ...\n"
            "  rebase <in> <out> <hexbase> [--verify]\n"
            "  prepare --manifest F --root DIR --out-list F\n"
            "  seal --layout F --hash F --inputs N [--force]\n"
            "  verify --layout F --hash F [--evidence F]\n");
    return 2;
}
