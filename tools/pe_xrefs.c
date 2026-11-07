/*
 * pe_xrefs - fast static cross-reference scan for a 32-bit PE image.
 *
 * This is a diagnostic helper, not part of the product build.  It parses the
 * PE section table, then makes one linear pass over each raw section looking
 * for
 *   - 4-byte little-endian occurrences of each requested address (absolute
 *     immediates and [disp32] memory operands),
 *   - direct rel32 call/jmp/jcc targets.
 *
 * The result is deliberately raw: a hit is a place to disassemble, not a
 * promise that the bytes are an instruction.  That is enough to find callers
 * of a lifted function or writers of a global without a full disassembler.
 *
 * Usage: pe_xrefs <image> <address> [address ...]
 * Addresses are hexadecimal, with or without 0x.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char name[9];
    uint32_t va;
    uint32_t vsize;
    uint32_t rawptr;
    uint32_t rawsize;
} Section;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int parse_hex(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    v = strtoul(s, &end, 16);
    if (!s[0] || (end && *end) || v > 0xfffffffful)
        return 0;
    *out = (uint32_t)v;
    return 1;
}

int main(int argc, char **argv)
{
    FILE *f;
    long len;
    uint8_t *data;
    uint32_t pe, nsec, optsz, image_base;
    Section *secs;
    uint32_t *targets;
    int ntargets;
    int i;
    int found_any = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <pe-image> <address> [address ...]\n", argv[0]);
        return 2;
    }

    f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "%s: cannot size file\n", argv[1]);
        fclose(f);
        return 2;
    }
    data = (uint8_t *)malloc((size_t)len);
    if (!data || fread(data, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "%s: cannot read file\n", argv[1]);
        free(data);
        fclose(f);
        return 2;
    }
    fclose(f);

    if ((size_t)len < 0x40 || rd16(data) != 0x5a4d) {
        fprintf(stderr, "%s: not an MZ image\n", argv[1]);
        free(data);
        return 2;
    }
    pe = rd32(data + 0x3c);
    if ((size_t)pe + 24 > (size_t)len || memcmp(data + pe, "PE\0\0", 4) != 0) {
        fprintf(stderr, "%s: not a PE image\n", argv[1]);
        free(data);
        return 2;
    }

    nsec = rd16(data + pe + 6);
    optsz = rd16(data + pe + 20);
    image_base = rd32(data + pe + 24 + 28);
    if ((size_t)pe + 24 + optsz + (size_t)nsec * 40 > (size_t)len) {
        fprintf(stderr, "%s: truncated section table\n", argv[1]);
        free(data);
        return 2;
    }

    secs = (Section *)calloc(nsec, sizeof(*secs));
    targets = (uint32_t *)calloc((size_t)argc - 2, sizeof(*targets));
    if (!secs || !targets) {
        fprintf(stderr, "out of memory\n");
        free(secs);
        free(targets);
        free(data);
        return 2;
    }
    ntargets = argc - 2;
    for (i = 0; i < ntargets; i++) {
        if (!parse_hex(argv[i + 2], &targets[i])) {
            fprintf(stderr, "bad address: %s\n", argv[i + 2]);
            free(secs);
            free(targets);
            free(data);
            return 2;
        }
    }

    for (i = 0; i < (int)nsec; i++) {
        const uint8_t *hdr = data + pe + 24 + optsz + (size_t)i * 40;
        memcpy(secs[i].name, hdr, 8);
        secs[i].vsize = rd32(hdr + 8);
        secs[i].va = rd32(hdr + 12);
        secs[i].rawsize = rd32(hdr + 16);
        secs[i].rawptr = rd32(hdr + 20);
    }

    for (i = 0; i < (int)nsec; i++) {
        Section *s = &secs[i];
        const uint8_t *raw;
        uint32_t n;
        uint32_t off;

        if (!s->rawsize || !s->vsize || s->rawptr >= (uint32_t)len)
            continue;
        n = s->rawsize;
        if ((uint64_t)s->rawptr + n > (uint64_t)len)
            n = (uint32_t)((uint64_t)len - s->rawptr);
        raw = data + s->rawptr;
        if (n > s->vsize)
            n = s->vsize;

        for (off = 0; off + 4 <= n; off++) {
            uint32_t v = rd32(raw + off);
            for (int t = 0; t < ntargets; t++) {
                if (v == targets[t]) {
                    printf("%08X abs %08X  %s +%X  %s\n",
                           image_base + s->va + off, targets[t], s->name,
                           off, s->name);
                    found_any = 1;
                }
            }
        }

        /* Direct rel32 branches.  Only the executable section matters, but the
         * scan is harmless elsewhere. */
        for (off = 0; off + 5 <= n; off++) {
            uint8_t op = raw[off];
            int is_call = 0, is_jmp = 0;
            uint32_t insn_len = 5;
            uint32_t target;
            if (op == 0xe8) is_call = 1;
            else if (op == 0xe9) is_jmp = 1;
            else if (off + 6 <= n && op == 0x0f &&
                     raw[off + 1] >= 0x80 && raw[off + 1] <= 0x8f) {
                insn_len = 6;
                is_jmp = 1;
            } else {
                continue;
            }
            target = image_base + s->va + off + insn_len + rd32(raw + off + 1);
            for (int t = 0; t < ntargets; t++) {
                if (target == targets[t]) {
                    printf("%08X %s %08X  %s +%X\n",
                           image_base + s->va + off,
                           is_call ? "call" : "jmp", targets[t], s->name, off);
                    found_any = 1;
                }
            }
        }
    }

    free(secs);
    free(targets);
    free(data);
    return found_any ? 0 : 1;
}
