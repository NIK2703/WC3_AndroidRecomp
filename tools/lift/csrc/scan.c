/*
 * scan - generic byte-pattern scanner for the dump image.
 *
 * The pattern language is intentionally small:
 *   90                 exact byte
 *   ?? or ?            wildcard byte
 *   A? or ?F           nibble wildcard
 *   ..                 wildcard byte (alternate spelling)
 *
 * Example:
 *   scan build\war31_unpacked2.exe --pattern "C7 45 FC 00 00 00 00 0F 0B" \
 *       --range 0x670000-0x690000 --context 24
 *
 * This complements expand: expand knows specific jump-table/trap shapes,
 * scan finds a new shape once and can emit lift seeds for it.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include "pe.h"

#define MAX_PAT 4096
#define MAX_LINE 8192

typedef struct {
    uint8_t val[MAX_PAT];
    uint8_t mask[MAX_PAT];
    size_t n;
    char text[MAX_LINE];
} Pattern;

static void die(const char *msg) {
    fprintf(stderr, "scan: %s\n", msg);
    exit(1);
}

static int parse_u32(const char *s, uint32_t *out) {
    char *end = NULL;
    unsigned long v;
    if (!s || !*s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    v = strtoul(s, &end, 16);
    if (end == s || *end) return 0;
    *out = (uint32_t)v;
    return 1;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_token(const char *tok, uint8_t *val, uint8_t *mask) {
    size_t n = strlen(tok);
    if (n == 2 && (strcmp(tok, "??") == 0 || strcmp(tok, "..") == 0)) {
        *val = 0; *mask = 0; return 1;
    }
    if (n == 1 && tok[0] == '?') {
        *val = 0; *mask = 0; return 1;
    }
    if (n == 2) {
        int a = hexval((unsigned char)tok[0]);
        int b = hexval((unsigned char)tok[1]);
        if (a >= 0 && b >= 0) {
            *val = (uint8_t)((a << 4) | b);
            *mask = 0xFF;
            return 1;
        }
        if (a >= 0 && tok[1] == '?') {
            *val = (uint8_t)(a << 4);
            *mask = 0xF0;
            return 1;
        }
        if (tok[0] == '?' && b >= 0) {
            *val = (uint8_t)b;
            *mask = 0x0F;
            return 1;
        }
    }
    if (n == 1) {
        int a = hexval((unsigned char)tok[0]);
        if (a >= 0) {
            *val = (uint8_t)a;
            *mask = 0xFF;
            return 1;
        }
    }
    return 0;
}

static void parse_pattern(Pattern *p, const char *text) {
    memset(p, 0, sizeof(*p));
    snprintf(p->text, sizeof(p->text), "%s", text);
    char *copy = (char *)malloc(strlen(text) + 1);
    if (!copy) die("out of memory");
    strcpy(copy, text);
    char *tok = strtok(copy, " ,\t\r\n");
    while (tok) {
        if (p->n >= MAX_PAT) die("pattern too long");
        if (!parse_token(tok, &p->val[p->n], &p->mask[p->n]))
            die("bad pattern token");
        p->n++;
        tok = strtok(NULL, " ,\t\r\n");
    }
    free(copy);
    if (!p->n) die("empty pattern");
}

static int match_at(const uint8_t *data, const Pattern *p) {
    for (size_t i = 0; i < p->n; i++)
        if ((data[i] & p->mask[i]) != p->val[i]) return 0;
    return 1;
}

/* Read one byte from any file-backed section.  print_context must work for
 * .rdata too: scanning for a vtable pointer is as common as scanning code. */
static int file_byte(const Pe *pe, const uint8_t *file, uint32_t va, uint8_t *out) {
    int si = pe_section_for_va((Pe *)pe, va);
    if (si < 0) return 0;
    uint32_t start = pe->base + pe->sec[si].va;
    if (va - start >= pe->sec[si].rawsize) return 0;
    *out = file[pe->sec[si].rawptr + (va - start)];
    return 1;
}

static void print_context(const Pe *pe, const uint8_t *file, uint32_t va, size_t ctx) {
    uint32_t lo = va > ctx ? va - (uint32_t)ctx : 0;
    uint32_t hi = va + (uint32_t)ctx + 16;
    if (lo < pe->base) lo = pe->base;
    if (hi > pe->base + pe->image_size) hi = pe->base + pe->image_size;
    if (hi < lo) return;
    printf("  ctx %08X:", lo);
    for (uint32_t a = lo; a < hi; a++) {
        uint8_t b;
        if (file_byte(pe, file, a, &b)) printf(" %02X", b);
        else printf(" ??");
    }
    printf("\n       ");
    for (uint32_t a = lo; a < hi; a++) {
        uint8_t b = 0;
        unsigned char c = file_byte(pe, file, a, &b) ? b : '.';
        putchar(isprint(c) ? c : '.');
    }
    putchar('\n');
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: scan <image> --pattern PATTERN [options]\n"
                "  --pattern P        hex bytes, ?? wildcard, A? nibble wildcard\n"
                "  --range LO-HI      scan range in preferred VAs (default .text)\n"
                "  --context N        bytes of context to print (default 16)\n"
                "  --max N            stop after N hits (0 = unlimited, default 200)\n"
                "  --out FILE         write lift seeds: 0xVA 512 scan_VA\n"
                "  --seed-size N      seed size written to --out (default 512)\n"
                "  --quiet            only print the summary\n");
        return 2;
    }

    const char *image = NULL;
    const char *pattern_text = NULL;
    const char *out_path = NULL;
    uint32_t range_lo = 0, range_hi = 0;
    int have_range = 0;
    size_t context = 16;
    size_t max_hits = 200;
    unsigned seed_size = 512;
    int quiet = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pattern") == 0 && i + 1 < argc) pattern_text = argv[++i];
        else if (strcmp(argv[i], "--range") == 0 && i + 1 < argc) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s", argv[++i]);
            char *dash = strchr(buf, '-');
            if (!dash) die("--range expects LO-HI");
            *dash = 0;
            if (!parse_u32(buf, &range_lo) || !parse_u32(dash + 1, &range_hi) ||
                range_lo >= range_hi)
                die("bad --range");
            have_range = 1;
        } else if (strcmp(argv[i], "--context") == 0 && i + 1 < argc) {
            context = (size_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--max") == 0 && i + 1 < argc) {
            max_hits = (size_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--seed-size") == 0 && i + 1 < argc) {
            seed_size = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "scan: unknown option %s\n", argv[i]);
            return 2;
        } else if (!image) {
            image = argv[i];
        } else {
            fprintf(stderr, "scan: extra argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!image) die("missing image path");
    if (!pattern_text) die("missing --pattern");

    Pattern pat;
    parse_pattern(&pat, pattern_text);

    Pe pe;
    if (!pe_open(&pe, image)) die("cannot open PE32 image");

    FILE *imf = fopen(image, "rb");
    if (!imf) die("cannot open image file");
    uint8_t *file = (uint8_t *)malloc(pe.fsize > 0 ? (size_t)pe.fsize : 1);
    if (!file) die("out of memory");
    if (fread(file, 1, (size_t)pe.fsize, imf) != (size_t)pe.fsize)
        die("cannot read image");
    fclose(imf);

    uint32_t lo = have_range ? range_lo : pe.text_va;
    uint32_t hi = have_range ? range_hi : pe.text_va + pe.text_size;
    if (lo < pe.base) lo = pe.base;
    if (hi > pe.base + pe.image_size) hi = pe.base + pe.image_size;
    if (lo >= hi || hi - lo < pat.n) die("empty scan range");

    printf("== scan ==\n");
    printf("image=%s base=%08X text=%08X-%08X\n", image, pe.base,
           pe.text_va, pe.text_va + pe.text_size);
    printf("pattern=%s (%zu bytes)\n", pat.text, pat.n);
    printf("range=%08X-%08X\n", lo, hi);

    FILE *out = NULL;
    if (out_path) {
        out = fopen(out_path, "w");
        if (!out) die("cannot write --out file");
        fprintf(out, "; scan seeds for %s\n", pat.text);
    }

    size_t hits = 0;
    for (uint32_t va = lo; va + pat.n <= hi; va++) {
        int si = pe_section_for_va(&pe, va);
        if (si < 0) continue;
        uint32_t sec_va = pe.base + pe.sec[si].va;
        uint32_t rawsize = pe.sec[si].rawsize;
        if (rawsize < pat.n || va - sec_va + pat.n > rawsize) continue;
        const uint8_t *data = file + pe.sec[si].rawptr + (va - sec_va);
        if (!match_at(data, &pat)) continue;
        hits++;
        if (!quiet) {
            printf("\n%08X", va);
            printf("  %s+%X  fileoff=%08X", pe.sec[si].name, va - sec_va,
                   pe.sec[si].rawptr + (va - sec_va));
            printf("\n");
            print_context(&pe, file, va, context);
        }
        if (out) fprintf(out, "0x%08X %u scan_%08X\n", va, seed_size, va);
        if (max_hits && hits >= max_hits) break;
    }

    if (out) {
        fclose(out);
        printf("\nseeds: %s (%zu hits)\n", out_path, hits);
    }
    printf("hits=%zu%s\n", hits, max_hits && hits >= max_hits ? " (limit reached)" : "");

    pe_close(&pe);
    return hits ? 0 : 1;
}
