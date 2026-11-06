/* seed.c - recover the extent of an address that has no catalog entry.
 *
 *   seed --module game.dll --catalog known.txt --seeds probe.txt --out funcs.txt
 *   seed --module game.dll --add 0x6F1402D0,0x6F13F790 --out funcs.txt
 *
 * A runtime probe reports addresses that are real code but not catalogued
 * functions, so their extents are unknown.  Decode from the seed until an
 * unconditional terminator that is followed by alignment padding, never
 * crossing the next catalogued start - the same rule lift_seeds.py used.
 *
 * Output: "0xADDR size seed_ADDR", the format the lifter's funcs file wants.
 */
#include "liftcore.h"

#define MAX_SEEDS 200000

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int is_terminator(const char *mnemonic)
{
    const char *p = strrchr(mnemonic, ' ');
    const char *w = p ? p + 1 : mnemonic;
    return !strcmp(w, "ret") || !strcmp(w, "retn") || !strcmp(w, "jmp") ||
           !strcmp(w, "iret") || !strcmp(w, "iretd");
}

static size_t read_catalog(const char *path, uint32_t **out)
{
    FILE *f = fopen(path, "r");
    char line[512];
    size_t n = 0, cap = 0;
    uint32_t *v = NULL;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line, *end;
        unsigned long a;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;
        a = strtoul(p, &end, 16);
        if (end == p) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 4096;
            v = (uint32_t *)realloc(v, cap * sizeof(uint32_t));
        }
        v[n++] = (uint32_t)a;
    }
    fclose(f);
    if (n) qsort(v, n, sizeof(uint32_t), cmp_u32);
    *out = v;
    return n;
}

/* Decode source for the executable section currently being walked.
 *
 * Mss32.dll carries two code sections: .text (21101000-211387D2) and MSSMIXER
 * (21139000-2113D712).  Walking only .text made every mixer seed fall into the
 * "skipped" bucket, so the probe loop never lifted the mixer and the engine
 * spun forever calling a stub that returned without doing the work.  The
 * section is copied out once and reused while the walk stays inside it. */
static uint8_t *g_code;
static uint32_t g_code_va, g_code_len;

static int code_load(Pe *pe, uint32_t va)
{
    int i = pe_code_section_for_va(pe, va);
    uint32_t sec_va, len;
    uint8_t *b;

    if (i < 0) return 0;
    sec_va = pe->base + pe->sec[i].va;
    len = pe->sec[i].rawsize;
    if (g_code && sec_va == g_code_va && len == g_code_len) return 1;
    b = (uint8_t *)realloc(g_code, len ? len : 1);
    if (!b) return 0;
    g_code = b;
    g_code_va = sec_va;
    g_code_len = len;
    if (!pe_read_at(pe, (long long)pe->sec[i].rawptr, g_code, len)) {
        g_code_len = 0;
        return 0;
    }
    return 1;
}

static size_t read_seeds(const char *path, uint32_t **out)
{
    FILE *f;
    char line[512];
    size_t n = 0, cap = 0;
    uint32_t *v = NULL;
    if (!path) { *out = NULL; return 0; }
    f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[!] cannot open %s\n", path); *out = NULL; return 0; }
    while (fgets(line, sizeof line, f)) {
        char *p = line, *end;
        unsigned long a;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;
        a = strtoul(p, &end, 16);
        if (end == p) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 1024;
            v = (uint32_t *)realloc(v, cap * sizeof(uint32_t));
        }
        v[n++] = (uint32_t)a;
    }
    fclose(f);
    *out = v;
    return n;
}

int main(int argc, char **argv)
{
    const char *module = NULL, *out_path = NULL, *catalog = NULL, *seeds_path = NULL;
    const char *add = NULL;
    long limit = 0;
    Pe pe;
    Lifter L;
    csh h;
    cs_insn *insn;
    uint32_t *starts = NULL, *seeds = NULL;
    size_t nstarts = 0, nseeds = 0, i;
    FILE *out;
    size_t lifted = 0, skipped = 0;
    long long bytes = 0;

    for (i = 1; i < (size_t)argc; i++) {
        if (!strcmp(argv[i], "--module") && i + 1 < (size_t)argc) module = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < (size_t)argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--catalog") && i + 1 < (size_t)argc) catalog = argv[++i];
        else if (!strcmp(argv[i], "--seeds") && i + 1 < (size_t)argc) seeds_path = argv[++i];
        else if (!strcmp(argv[i], "--add") && i + 1 < (size_t)argc) add = argv[++i];
        else if (!strcmp(argv[i], "--limit") && i + 1 < (size_t)argc) limit = atol(argv[++i]);
        else {
            fprintf(stderr, "usage: %s --module <pe> --out <funcs.txt> "
                            "[--catalog <txt>] [--seeds <txt>] [--add 0x..,0x..]\n", argv[0]);
            return 2;
        }
    }
    if (!module || !out_path) { fprintf(stderr, "seed: --module and --out are required\n"); return 2; }

    memset(&L, 0, sizeof L);
    if (!pe_open(&pe, module)) { fprintf(stderr, "[!] cannot open %s\n", module); return 1; }
    L.pe = &pe;
    L.image_lo = pe.base;
    L.image_hi = pe.base + pe.image_size;
    if (csg_open_x86_ex(&h, 1) != 0) { pe_close(&pe); return 1; }
    L.h = h;
    insn = csg.malloc_insn(h);
    if (!insn) { fprintf(stderr, "[!] cs_malloc failed\n"); return 1; }

    if (catalog) nstarts = read_catalog(catalog, &starts);
    nseeds = read_seeds(seeds_path, &seeds);
    if (add) {
        char buf[4096];
        char *tok;
        snprintf(buf, sizeof buf, "%s", add);
        for (tok = strtok(buf, ",;"); tok; tok = strtok(NULL, ",;")) {
            unsigned long a = strtoul(tok, NULL, 16);
            if (!a) continue;
            seeds = (uint32_t *)realloc(seeds, (nseeds + 1) * sizeof(uint32_t));
            seeds[nseeds++] = (uint32_t)a;
        }
    }
    if (!nseeds) { fprintf(stderr, "[!] no seeds\n"); return 1; }
    qsort(seeds, nseeds, sizeof(uint32_t), cmp_u32);
    if (limit > 0 && (size_t)limit < nseeds) nseeds = (size_t)limit;

    out = fopen(out_path, "wb");   /* LF only: the lifter reads these verbatim */
    if (!out) { fprintf(stderr, "[!] cannot write %s\n", out_path); return 1; }

    for (i = 0; i < nseeds; i++) {
        uint32_t seed = seeds[i];
        uint32_t lo, limit_va;
        uint32_t size = 0;
        const uint8_t *p;
        size_t left;
        uint64_t a;
        size_t k;

        if (i && seeds[i] == seeds[i - 1]) continue;
        if (!code_load(&pe, seed)) { skipped++; continue; }

        /* next catalogued start above the seed, clamped to the section end */
        limit_va = g_code_va + g_code_len;
        for (k = 0; k < nstarts; k++)
            if (starts[k] > seed) { limit_va = starts[k] < limit_va ? starts[k] : limit_va; break; }
        lo = seed;
        if (limit_va <= lo) { skipped++; continue; }

        p = g_code + (seed - g_code_va);
        left = limit_va - seed;
        a = seed;
        while (left && csg.iter(h, &p, &left, &a, insn)) {
            if (insn->id == 0) break;              /* skipdata: data, not code */
            size = (uint32_t)(insn->address + insn->size - seed);
            if (is_terminator(insn->mnemonic)) {
                uint32_t after = seed + size;
                if (after - g_code_va >= g_code_len) break;
                {
                    uint8_t pad = g_code[after - g_code_va];
                    if (pad == 0xCC || pad == 0x90) break;
                }
            }
        }
        if (!size) { skipped++; continue; }
        fprintf(out, "0x%08x %u seed_%08x\n", seed, size, seed);
        lifted++;
        bytes += size;
    }
    fclose(out);
    fprintf(stderr, "seeds=%zu lifted=%zu skipped=%zu bytes=%lld -> %s\n",
            nseeds, lifted, skipped, bytes, out_path);

    csg.free_insn(insn, 1);
    csg.close(&h);
    pe_close(&pe);
    free(g_code);
    free(starts);
    free(seeds);
    /* "skipped every seed" is a normal outcome, not a failure: the probe
     * reported a target that an existing extent already covers (typically a
     * vtable slot inside a function lifted earlier).  Only a bad invocation or
     * an unreadable image is an error, and those returned 1/2 above.  Making
     * this non-zero aborted the loop on the first already-covered target. */
    return 0;
}
