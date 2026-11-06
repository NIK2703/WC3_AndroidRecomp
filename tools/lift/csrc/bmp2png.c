/*
 * bmp2png.c - turn a WC3_VK_SHOT capture into a PNG.
 *
 * The host writes a 32bpp bottom-up BMP because that needs no encoder; a PNG
 * is what the image tooling and the reports can display.  This is the compiled
 * replacement for the one-off python version, per the project rule that
 * everything in the loop is a binary in bin\ rather than an interpreter.
 *
 * No zlib dependency: the IDAT stream is emitted as stored (uncompressed)
 * deflate blocks, which is a valid PNG.  The files are larger than a compressed
 * PNG would be, but this is a diagnostic artefact, not a deliverable.
 *
 *   bmp2png <in.bmp> [-o out.png] [-div N] [-gain F]
 *
 * -div N takes every Nth pixel (no averaging), which is enough to make a
 * 1920x1080 capture readable at 960x540.  -gain F multiplies each channel,
 * for captures whose content is there but too dark to judge by eye.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

/* Incremental: seed with 0xFFFFFFFF, fold each run, finish with ^0xFFFFFFFF.
 * Inverting per call (the one-shot formulation) breaks a running CRC. */
static uint32_t crc_fold(uint32_t c, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Adler-32 over the *uncompressed* bytes: the zlib stream trailer, which PNG
 * requires and which bare deflate blocks do not carry.  Two sums modulo 65521,
 * folded in runs small enough that s2 cannot overflow a uint32. */
static uint32_t adler32(const uint8_t *p, size_t n)
{
    uint32_t s1 = 1, s2 = 0;
    while (n) {
        size_t run = n > 5552 ? 5552 : n;
        n -= run;
        while (run--) { s1 += *p++; s2 += s1; }
        s1 %= 65521;
        s2 %= 65521;
    }
    return (s2 << 16) | s1;
}

/* One chunk, written from a buffer so the length is known up front.  An fseek
 * backfill is not an option: the stream is open for writing, so any seek has to
 * be preceded by a flush and the length would still be racy to get right. */
static int write_chunk(FILE *f, const char type[4], const uint8_t *data,
                       uint32_t len)
{
    uint8_t hdr[8], tail[4];
    put32(hdr, len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return 0;
    if (len && fwrite(data, 1, len, f) != len) return 0;
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc_fold(crc, (const uint8_t *)type, 4);
    if (len) crc = crc_fold(crc, data, len);
    put32(tail, crc ^ 0xFFFFFFFFu);
    return fwrite(tail, 1, 4, f) == 4;
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL;
    int div = 1;
    double gain = 1.0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-div") && i + 1 < argc) div = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-gain") && i + 1 < argc) gain = atof(argv[++i]);
        else if (argv[i][0] != '-') in = argv[i];
        else { fprintf(stderr, "bmp2png: unknown option %s\n", argv[i]); return 2; }
    }
    if (!in) {
        fprintf(stderr, "usage: bmp2png <in.bmp> [-o out.png] [-div N] [-gain F]\n");
        return 2;
    }
    if (div < 1) div = 1;
    if (gain <= 0.0) gain = 1.0;

    FILE *f = fopen(in, "rb");
    if (!f) { fprintf(stderr, "bmp2png: cannot open %s\n", in); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *bmp = (uint8_t *)malloc((size_t)size);
    if (!bmp || fread(bmp, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "bmp2png: short read on %s\n", in);
        return 1;
    }
    fclose(f);

    if (size < 54 || bmp[0] != 'B' || bmp[1] != 'M') {
        fprintf(stderr, "bmp2png: %s is not a BMP\n", in);
        return 1;
    }
    uint32_t off = rd32(bmp + 10);
    int32_t w = (int32_t)rd32(bmp + 18);
    int32_t hraw = (int32_t)rd32(bmp + 22);
    uint16_t bpp = (uint16_t)(bmp[28] | (bmp[29] << 8));
    if (bpp != 32 || w <= 0 || hraw == 0) {
        fprintf(stderr, "bmp2png: need a 32bpp bitmap, got %ux%u %ubpp\n",
                (unsigned)w, (unsigned)(hraw < 0 ? -hraw : hraw), (unsigned)bpp);
        return 1;
    }
    int bottom_up = hraw > 0;
    uint32_t h = (uint32_t)(hraw < 0 ? -hraw : hraw);
    uint32_t row = (uint32_t)w * 4;
    if ((uint64_t)off + (uint64_t)row * h > (uint64_t)size) {
        fprintf(stderr, "bmp2png: truncated pixel data\n");
        return 1;
    }

    uint32_t ow = (uint32_t)w / (uint32_t)div;
    uint32_t oh = h / (uint32_t)div;
    if (!ow || !oh) { fprintf(stderr, "bmp2png: -div too large\n"); return 1; }

    if (!out) {
        static char buf[1024];
        snprintf(buf, sizeof buf, "%s", in);
        char *dot = strrchr(buf, '.');
        if (dot) strcpy(dot, ".png");
        else strncat(buf, ".png", sizeof buf - strlen(buf) - 1);
        out = buf;
    }

    /* Filtered scanlines: one filter byte (0 = none) then RGB triples. */
    size_t stride = (size_t)ow * 3 + 1;
    size_t rawsize = stride * oh;
    uint8_t *raw = (uint8_t *)malloc(rawsize);
    if (!raw) { fprintf(stderr, "bmp2png: out of memory\n"); return 1; }
    for (uint32_t y = 0; y < oh; y++) {
        uint32_t sy = bottom_up ? (h - 1 - y * (uint32_t)div) : (y * (uint32_t)div);
        const uint8_t *src = bmp + off + (size_t)sy * row;
        uint8_t *dst = raw + (size_t)y * stride;
        *dst++ = 0;
        for (uint32_t x = 0; x < ow; x++) {
            const uint8_t *p = src + (size_t)x * (uint32_t)div * 4;
            for (int k = 0; k < 3; k++) {
                double v = p[2 - k] * gain;      /* BMP is BGRX */
                dst[k] = (uint8_t)(v > 255.0 ? 255.0 : v);
            }
            dst += 3;
        }
    }

    /* Stored deflate blocks: 5 bytes of framing per 64 KiB of input.  Plus the
     * two-byte zlib header and the four-byte Adler-32 trailer that make this a
     * zlib stream rather than a bare deflate stream. */
    size_t blocks = (rawsize + 65534) / 65535;
    size_t zsize = rawsize + blocks * 5 + 1 + 6;
    uint8_t *z = (uint8_t *)malloc(zsize);
    if (!z) { fprintf(stderr, "bmp2png: out of memory\n"); return 1; }
    size_t zn = 0;
    z[zn++] = 0x78;     /* CM=8 (deflate), CINFO=7 (32K window) */
    z[zn++] = 0x01;     /* FCHECK so that 0x7801 % 31 == 0, no dict, fastest */
    size_t left = rawsize;
    const uint8_t *p = raw;
    while (left) {
        size_t blk = left > 65535 ? 65535 : left;
        z[zn++] = (left == blk) ? 1 : 0;    /* BFINAL, BTYPE=00 */
        z[zn++] = (uint8_t)(blk & 0xFF);
        z[zn++] = (uint8_t)(blk >> 8);
        z[zn++] = (uint8_t)(~blk & 0xFF);
        z[zn++] = (uint8_t)((~blk >> 8) & 0xFF);
        memcpy(z + zn, p, blk);
        zn += blk;
        p += blk;
        left -= blk;
    }
    /* An empty IDAT is legal but some readers dislike it; one empty final block
     * costs five bytes and removes the corner case. */
    if (!rawsize) { z[zn++] = 1; z[zn++] = 0; z[zn++] = 0; z[zn++] = 0xFF; z[zn++] = 0xFF; }
    put32(z + zn, adler32(raw, rawsize));
    zn += 4;

    crc_init();
    FILE *g = fopen(out, "wb");
    if (!g) { fprintf(stderr, "bmp2png: cannot write %s\n", out); return 1; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    fwrite(sig, 1, 8, g);

    uint8_t ihdr[13];
    put32(ihdr + 0, ow);
    put32(ihdr + 4, oh);
    ihdr[8] = 8;    /* bit depth */
    ihdr[9] = 2;    /* colour type: truecolour */
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    int ok = write_chunk(g, "IHDR", ihdr, sizeof ihdr) &&
             write_chunk(g, "IDAT", z, (uint32_t)zn) &&
             write_chunk(g, "IEND", NULL, 0);
    fclose(g);
    if (!ok) { fprintf(stderr, "bmp2png: write failed on %s\n", out); return 1; }

    printf("%s -> %s (%ux%u)\n", in, out, ow, oh);
    free(z);
    free(raw);
    free(bmp);
    return 0;
}
