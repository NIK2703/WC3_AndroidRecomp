/* entry.c - enumerate every function entry in the code sections.
 *
 *   entry --module game.dll --known build27\wc3_all_list.h --out seeds.txt
 *   entry --module game.dll --known ... --referenced --quiet
 *
 * Why this exists
 * ---------------
 * Three rounds in a row ended on an "unknown guest target" that no existing
 * scan could find:
 *
 *   6F1A52D0   vtable slot            -> found by a run-of-3 dword scan
 *   6F2D4C60   `mov ecx, imm32`       -> invisible to every scan
 *   6F8FCA20   `mov [ebx+0x28], imm32`-> a struct field holding a callback
 *
 * `lift.exe targets` only sees `dispatch()/dispatch_jmp()` calls that the
 * corpus itself emits, `refs.exe` only sees `call/jmp rel32`, and the vtable
 * sweep only sees runs of >=3 consecutive code pointers.  A function pointer
 * stored into a struct field as an immediate is none of those, yet the guest
 * calls it.
 *
 * MSVC pads between functions with 0xCC and aligns every function to 16
 * bytes, so "a code byte whose predecessor is int3 padding" is the function
 * inventory of the image, independent of how the address is reached.  This
 * tool enumerates exactly that set and subtracts what is already lifted.
 *
 * The output is a seed file for `seed.exe` (0xVA size name).
 */
#include "pe.h"
#include <ctype.h>

#define MAX_ENTRY 400000

static uint32_t *g_known;
static size_t g_nknown;

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int is_known(uint32_t va)
{
    return bsearch(&va, g_known, g_nknown, sizeof(uint32_t), cmp_u32) != NULL;
}

/* Every value that appears as a 4-byte dword anywhere in the image and points
 * into the code span.  A function that is only reached by `call rel32` is not
 * in here, and does not need to be: the closure already lifts those.  What
 * this keeps is exactly the address-taken set - vtables, descriptor tables,
 * `mov ecx, imm32`, and `mov [reg+off], imm32` struct-field callbacks - which
 * is the class that keeps costing a round each. */
static uint32_t *g_ref;
static size_t g_nref;

static void build_refs_span(Pe *pe, uint32_t lo, uint32_t hi,
                            uint32_t **v_out, size_t *n_out)
{
    size_t cap = 1u << 16, n = 0;
    uint32_t *v = (uint32_t *)malloc(cap * sizeof(uint32_t));
    if (!v) return;
    for (int s = 0; s < pe->nsec; s++) {
        PeSec *sc = &pe->sec[s];
        if (!sc->rawsize) continue;
        uint8_t *buf = (uint8_t *)malloc(sc->rawsize);
        if (!buf) continue;
        if (!pe_read_at(pe, sc->rawptr, buf, sc->rawsize)) { free(buf); continue; }
        for (uint32_t off = 0; off + 4 <= sc->rawsize; off++) {
            uint32_t d = pe_rd32(buf + off);
            if (d < lo || d >= hi) continue;
            if (n == cap) {
                uint32_t *nv = (uint32_t *)realloc(v, cap * 2 * sizeof(uint32_t));
                if (!nv) { free(buf); goto done; }
                v = nv;
                cap *= 2;
            }
            v[n++] = d;
        }
        free(buf);
    }
done:
    *v_out = v;
    *n_out = n;
}

static void build_refs(Pe *pe, const char **extra, int nextra)
{
    uint32_t *v = NULL;
    size_t n = 0, cap = 0;

    /* The primary image supplies the span; every image (primary included)
     * supplies values.  A callback installed into a Mss32 function pointer
     * lives in game.dll's .data, so scanning only the module being swept
     * misses it. */
    build_refs_span(pe, pe->code_lo, pe->code_hi, &v, &n);
    cap = n;
    for (int e = 0; e < nextra; e++) {
        Pe o;
        uint32_t *ov = NULL;
        size_t on = 0;
        if (!pe_open(&o, extra[e])) {
            fprintf(stderr, "entry: --refs-image %s is not PE32, skipped\n", extra[e]);
            continue;
        }
        build_refs_span(&o, pe->code_lo, pe->code_hi, &ov, &on);
        pe_close(&o);
        if (!on) { free(ov); continue; }
        uint32_t *nv = (uint32_t *)realloc(v, (n + on) * sizeof(uint32_t));
        if (!nv) { free(ov); break; }
        v = nv;
        memcpy(v + n, ov, on * sizeof(uint32_t));
        n += on;
        free(ov);
    }
    (void)cap;
    if (!n) { free(v); return; }
    qsort(v, n, sizeof(uint32_t), cmp_u32);
    /* dedupe in place */
    size_t w = 0;
    for (size_t k = 0; k < n; k++)
        if (!w || v[w - 1] != v[k]) v[w++] = v[k];
    g_ref = v;
    g_nref = w;
}

static int is_referenced(uint32_t va)
{
    return bsearch(&va, g_ref, g_nref, sizeof(uint32_t), cmp_u32) != NULL;
}

/* Accept both `X(6F001000)` headers and plain `6F001000` / `0x6f001000` lists. */
static size_t read_known(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    size_t n = 0, cap = 0;
    uint32_t *v = NULL;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if ((p[0] == 'X' || p[0] == 'x') && p[1] == '(') p += 2;
        else if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        else if (!isxdigit((unsigned char)*p)) continue;
        char *end;
        unsigned long a = strtoul(p, &end, 16);
        if (end == p) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 4096;
            v = (uint32_t *)realloc(v, cap * sizeof(uint32_t));
            if (!v) { fclose(f); return 0; }
        }
        v[n++] = (uint32_t)a;
    }
    fclose(f);
    if (n) qsort(v, n, sizeof(uint32_t), cmp_u32);
    g_known = v;
    g_nknown = n;
    return n;
}

/* A function can start with almost anything, but never with the padding that
 * precedes it, a 0x00 0x00 (`add [eax], al`) or a lone 0x90. */
static int plausible_start(uint8_t b)
{
    return b != 0xCC && b != 0x90 && b != 0x00;
}

/* "Something ended right before this byte."
 *
 * int3 padding is the common case, but it is not the only one: the linker only
 * pads to the function's alignment, so a function that already lands on a
 * 16-byte boundary directly after its predecessor's `ret` has *no* padding at
 * all.  6F134110 is exactly that - `... ret` at 6F13410F, `push ebp` at
 * 6F134110 - and the padded-start rule alone left it unlifted, which cost a
 * whole probe round.
 *
 * The multi-byte terminators are matched by their opcode byte at a fixed
 * distance from the start, so a `ret imm16` / `jmp rel32` / `jmp [mem]` ending
 * exactly here is recognised.  False hits are possible in isolation; that is
 * why --referenced is the normal way to run this. */
static int ended_before(const uint8_t *buf, uint32_t off)
{
    if (buf[off - 1] == 0xCC) return 1;                 /* int3 padding      */
    /* Some toolchains pad with nop instead (Mss32's 2110A9F0 sits behind a run
     * of 0x90).  One nop is an ordinary instruction, a run is padding. */
    if (buf[off - 1] == 0x90 && off >= 2 && buf[off - 2] == 0x90) return 1;
    if (buf[off - 1] == 0xC3) return 2;                 /* ret               */
    if (off >= 3 && buf[off - 3] == 0xC2) return 2;     /* ret imm16         */
    if (off >= 2 && buf[off - 2] == 0xEB) return 2;     /* jmp rel8          */
    if (off >= 5 && buf[off - 5] == 0xE9) return 2;     /* jmp rel32         */
    if (off >= 6 && buf[off - 6] == 0xFF && buf[off - 5] == 0x25)
        return 2;                                       /* jmp dword [mem]   */
    return 0;
}

int main(int argc, char **argv)
{
    const char *module = NULL, *known_path = NULL, *out_path = NULL;
    const char *refs_image[8];
    int nrefs_image = 0;
    int min_pad = 0, quiet = 0, referenced = 0, align = 16, i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--module") && i + 1 < argc) module = argv[++i];
        else if (!strcmp(argv[i], "--known") && i + 1 < argc) known_path = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--min-pad") && i + 1 < argc) min_pad = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--align") && i + 1 < argc) align = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--refs-image") && i + 1 < argc) {
            if (nrefs_image < 8) refs_image[nrefs_image++] = argv[++i];
            else i++;
        }
        else if (!strcmp(argv[i], "--referenced")) referenced = 1;
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else {
            fprintf(stderr, "usage: entry --module IMAGE [--known FILE] "
                            "[--out FILE] [--referenced] [--refs-image IMAGE] "
                            "[--min-pad N] [--align N] [--quiet]\n");
            return 2;
        }
    }
    if (!module) {
        fprintf(stderr, "usage: entry --module IMAGE [--known FILE] "
                        "[--out FILE] [--referenced] [--refs-image IMAGE] "
                        "[--min-pad N] [--align N] [--quiet]\n");
        return 2;
    }

    Pe pe;
    if (!pe_open(&pe, module)) {
        fprintf(stderr, "entry: cannot open %s as PE32\n", module);
        return 1;
    }
    if (known_path) read_known(known_path);
    if (referenced) build_refs(&pe, refs_image, nrefs_image);

    uint32_t *hits = (uint32_t *)malloc(MAX_ENTRY * sizeof(uint32_t));
    if (!hits) return 1;
    size_t nhit = 0, nseen = 0, nknown = 0, npad1 = 0;

    for (int s = 0; s < pe.nsec; s++) {
        PeSec *sc = &pe.sec[s];
        if (!(sc->chars & PE_SCN_MEM_EXECUTE) || !sc->rawsize) continue;
        uint32_t sec_va = pe.base + sc->va;
        uint32_t len = sc->rawsize;
        uint8_t *buf = (uint8_t *)malloc(len);
        if (!buf) continue;
        if (!pe_read_at(&pe, sc->rawptr, buf, len)) { free(buf); continue; }

        for (uint32_t off = 1; off < len; off++) {
            int how = ended_before(buf, off);
            if (!how) continue;
            /* count the int3 run ending at off-1, for --min-pad */
            int pad = 0;
            while ((uint32_t)pad < off && buf[off - 1 - pad] == 0xCC) pad++;
            if (pad < min_pad) continue;
            /* A terminator with no padding behind it only means "new function"
             * if the address is one the compiler would align to.  Without this
             * the opcode bytes of an ordinary `ret`/`jmp` inside a body match
             * constantly and the sweep is half garbage. */
            if (how == 2 && (sec_va + off) % (unsigned)align) continue;
            if (!plausible_start(buf[off])) continue;
            uint32_t va = sec_va + off;
            nseen++;
            if (g_nknown && is_known(va)) { nknown++; continue; }
            if (referenced && !is_referenced(va)) continue;
            npad1++;
            if (nhit < MAX_ENTRY) hits[nhit++] = va;
        }
        free(buf);
    }

    qsort(hits, nhit, sizeof(uint32_t), cmp_u32);

    if (out_path) {
        FILE *o = fopen(out_path, "w");
        if (!o) { fprintf(stderr, "entry: cannot write %s\n", out_path); return 1; }
        for (size_t k = 0; k < nhit; k++)
            fprintf(o, "0x%08X 512 entry_%08X\n", hits[k], hits[k]);
        fclose(o);
    }

    if (!quiet || !out_path)
        printf("entry: %s code=%d span=%08X-%08X padded_starts=%zu "
               "already_known=%zu refs=%zu new=%zu\n",
               module, pe.ncode, pe.code_lo, pe.code_hi,
               nseen, nknown, g_nref, nhit);
    if (out_path) printf("entry: wrote %zu seeds -> %s\n", nhit, out_path);

    free(hits);
    pe_close(&pe);
    return 0;
}
