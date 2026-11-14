/* lift32.c - command line front end for the compiled lifter.
 *
 *   lift32 <module> <funcs.txt> <out.c> [0xADDR ...]
 *
 * funcs.txt lines: "0xADDR size name".  With no addresses every entry is
 * lifted.  The output is byte-identical to lift32_cpu.py's.
 */
#include "liftcore.h"

size_t lf_read_va(Lifter *L, uint32_t va, uint8_t *buf, size_t n);
char  *lf_lift_function(Lifter *L, uint32_t start, uint32_t size);

/* Read the base-relocation table: every 32-bit absolute address the loader
 * fixes up.  An instruction immediate or displacement at one of these VAs is
 * an address, not a constant, and must be emitted through GVA(). */
static void load_relocs(Lifter *L, const char *path)
{
    Pe *pe = L->pe;
    FILE *f = fopen(path, "rb");
    uint8_t hdr[0x40], nt[24], opt[0x400];
    uint32_t lfanew, rva = 0, size = 0;
    uint16_t sizeopt;
    uint8_t *blob;
    uint32_t off;
    size_t cap = 0, n = 0;

    if (!f) return;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || hdr[0] != 'M') { fclose(f); return; }
    lfanew = pe_rd32(hdr + 0x3C);
    if (fseek(f, (long)lfanew, SEEK_SET) != 0) { fclose(f); return; }
    if (fread(nt, 1, sizeof nt, f) != sizeof nt) { fclose(f); return; }
    sizeopt = pe_rd16(nt + 20);
    if (sizeopt < 0xC0 || sizeopt > 0x400) { fclose(f); return; }
    if (fseek(f, (long)lfanew + 24, SEEK_SET) != 0) { fclose(f); return; }
    if (fread(opt, 1, sizeopt, f) != sizeopt) { fclose(f); return; }
    rva = pe_rd32(opt + 96 + 5 * 8);
    size = pe_rd32(opt + 96 + 5 * 8 + 4);
    if (!rva || !size) { fclose(f); return; }
    if (!pe_va_to_off(pe, pe->base + rva, &off)) { fclose(f); return; }

    blob = (uint8_t *)malloc(size);
    if (!blob) { fclose(f); return; }
    if (fseek(f, (long)off, SEEK_SET) != 0 || fread(blob, 1, size, f) != size) {
        free(blob); fclose(f); return;
    }
    fclose(f);

    {
        uint32_t p = 0;
        while (p + 8 <= size) {
            uint32_t page = pe_rd32(blob + p);
            uint32_t bsz = pe_rd32(blob + p + 4);
            uint32_t q;
            if (bsz < 8 || p + bsz > size) break;
            if (!page && !bsz) break;
            for (q = p + 8; q + 2 <= p + bsz; q += 2) {
                uint16_t ent = (uint16_t)(blob[q] | (blob[q + 1] << 8));
                uint32_t type = ent >> 12;
                if (type == 3) {
                    if (n == cap) {
                        cap = cap ? cap * 2 : 4096;
                        L->reloc = (uint32_t *)realloc(L->reloc, cap * sizeof(uint32_t));
                    }
                    L->reloc[n++] = (pe->base + page + (ent & 0xFFF)) & 0xFFFFFFFFu;
                }
            }
            p += bsz;
        }
    }
    L->nreloc = n;
    free(blob);

    /* sort for the membership test */
    if (n > 1) {
        size_t i;
        for (i = 1; i < n; i++) {
            uint32_t v = L->reloc[i];
            size_t j = i;
            while (j > 0 && L->reloc[j - 1] > v) { L->reloc[j] = L->reloc[j - 1]; j--; }
            L->reloc[j] = v;
        }
    }
}

static int lf_func_size_cmp(const void *a, const void *b)
{
    const LfFuncSize *x = (const LfFuncSize *)a;
    const LfFuncSize *y = (const LfFuncSize *)b;
    return (x->addr > y->addr) - (x->addr < y->addr);
}

int lf_lift_files(const char *module, const char *funcs_path, const char *out_c,
                  const char *out_list, int block_mode, int verbose)
{
    Pe pe;
    Lifter L;
    FILE *f;
    char line[512];
    uint32_t *addrs = NULL, *sizes = NULL;
    size_t nf = 0, cap = 0, i;
    FILE *oc, *ol;
    size_t written = 0;

    memset(&L, 0, sizeof L);
#if WC3_DEBUG && WC3_DEBUG_LIFT
    lf_x87_dbg_init();
#endif
    if (!pe_open(&pe, module)) {
        fprintf(stderr, "[!] cannot open %s\n", module);
        return 1;
    }
    L.pe = &pe;
    L.image_lo = pe.base;
    L.image_hi = pe.base + pe.image_size;
    L.block_mode = block_mode;
    /* These are part of the generator, not a tunable profile.  The GPR-cache
     * prototype was measured negative and is not an active optimization. */
    L.flag_fusion = 1;
    L.flagless = 1;
    L.gpr_cache = 0;
    L.flag_boundary = 1;
    if (csg_open_x86(&L.h) != 0) { pe_close(&pe); return 1; }
    load_relocs(&L, module);
    if (verbose)
        fprintf(stderr, "[*] image base %#x, size %#x, %zu relocations\n",
                pe.base, pe.image_size, L.nreloc);

    f = fopen(funcs_path, "r");
    if (!f) { fprintf(stderr, "[!] cannot open %s\n", funcs_path); csg.close(&L.h); pe_close(&pe); return 1; }
    while (fgets(line, sizeof line, f)) {
        char *p = line, *end;
        unsigned long a;
        unsigned long sz;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '0') continue;
        a = strtoul(p, &end, 16);
        if (end == p) continue;
        sz = strtoul(end, &end, 10);
        if (!sz) continue;
        if (nf == cap) {
            cap = cap ? cap * 2 : 1024;
            addrs = (uint32_t *)realloc(addrs, cap * sizeof(uint32_t));
            sizes = (uint32_t *)realloc(sizes, cap * sizeof(uint32_t));
        }
        addrs[nf] = (uint32_t)a;
        sizes[nf] = (uint32_t)sz;
        nf++;
    }
    fclose(f);
    if (!nf) { fprintf(stderr, "[!] %s: no functions\n", funcs_path); free(addrs); free(sizes); csg.close(&L.h); pe_close(&pe); return 1; }

    L.func_map = (LfFuncSize *)malloc((size_t)nf * sizeof(LfFuncSize));
    if (!L.func_map) { fprintf(stderr, "[!] out of memory\n"); free(addrs); free(sizes); csg.close(&L.h); pe_close(&pe); return 1; }
    for (i = 0; i < nf; i++) {
        L.func_map[i].addr = addrs[i];
        L.func_map[i].size = sizes[i];
    }
    qsort(L.func_map, (size_t)nf, sizeof(LfFuncSize), lf_func_size_cmp);
    L.func_n = (size_t)nf;
    lf_summary_init(&L, (size_t)nf * 4);
    lf_precompute_summaries(&L);

    oc = fopen(out_c, "w");
    if (!oc) { fprintf(stderr, "[!] cannot write %s\n", out_c); return 1; }
    ol = fopen(out_list, "w");
    if (!ol) { fprintf(stderr, "[!] cannot write %s\n", out_list); fclose(oc); return 1; }
    fputs("/* AUTO-GENERATED by lift32_cpu.py - do not edit */\n#include \"cpu.h\"\n\n", oc);
    fputs("/* AUTO-GENERATED by lift32_cpu.py */\n#define LIFTED_FUNCS(X) \\\n", ol);

    for (i = 0; i < nf; i++) {
        char *txt = lf_lift_function(&L, addrs[i], sizes[i]);
        if (!txt) { fprintf(stderr, "[!] lift failed at %08X\n", addrs[i]); continue; }
        fputs(txt, oc);
        fputs(i + 1 < nf ? "\n\n" : "\n", oc);
        if (written) fputs(" \\\n", ol);
        fprintf(ol, "    X(%08X)", addrs[i]);
        written++;
        if (verbose)
            fprintf(stderr, "[+] lifted L_%08X (%u bytes)\n", addrs[i], sizes[i]);
        free(txt);
    }
    fputs("\n", ol);
    fclose(oc);
    fclose(ol);
    if (verbose)
        fprintf(stderr, "[*] wrote %s and %s (%zu funcs)\n", out_c, out_list, written);

    free(addrs);
    free(sizes);
    free(L.func_map);
    free(L.summary_table);
    free(L.x87_callee_state);
    free(L.x87_callee_may);
    free(L.reloc);
    csg.close(&L.h);
    pe_close(&pe);
#if WC3_DEBUG && WC3_DEBUG_LIFT
    lf_x87_dbg_print();
#endif
    return written ? 0 : 1;
}

#ifndef WC3_NO_LIFT_MAIN
int main(int argc, char **argv)
{
    const char *out_c, *out_list;
    char listbuf[1024];
    const char *dot;
    int block = getenv("WC3_LIFT_BLOCK") && !strcmp(getenv("WC3_LIFT_BLOCK"), "1");

    if (argc == 2 && !strcmp(argv[1], "--self-test-cond")) {
        if (lf_cond_fold_selftest() != 0) {
            fprintf(stderr, "lift32: cond-fold self-test FAIL\n");
            return 1;
        }
        puts("lift32: cond-fold self-test OK");
        return 0;
    }

    if (argc == 2 && !strcmp(argv[1], "--self-test-x87")) {
        int rc = lf_x87_summary_selftest();
        if (rc == 2) {
            fprintf(stderr, "lift32: x87 summary self-test unavailable "
                            "(capstone)\n");
            return 2;
        }
        if (rc != 0) {
            fprintf(stderr, "lift32: x87 summary self-test FAIL\n");
            return 1;
        }
        puts("lift32: x87 summary self-test OK");
        return 0;
    }

    if (argc < 4) {
        fprintf(stderr, "usage: %s <module> <funcs.txt> <out.c> [0xADDR ...]\n", argv[0]);
        return 2;
    }
    out_c = argv[3];
    dot = strrchr(out_c, '.');
    if (dot) snprintf(listbuf, sizeof listbuf, "%.*s_list.h", (int)(dot - out_c), out_c);
    else     snprintf(listbuf, sizeof listbuf, "%s_list.h", out_c);
    out_list = listbuf;

    return lf_lift_files(argv[1], argv[2], out_c, out_list, block, 1);
}
#endif /* WC3_NO_LIFT_MAIN */
