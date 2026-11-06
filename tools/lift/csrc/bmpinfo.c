/*
 * bmpinfo.c - describe a WC3_VK_SHOT capture as text.
 *
 * The frame is the only ground truth for the renderer, but an image viewer is
 * a dependency the loop does not always have (and a 1920x1080 BMP is not
 * something to eyeball every iteration).  This prints the same information in
 * a form that reads in a terminal: dimensions, brightness histogram, the
 * colour of a grid of sample points, and an ASCII luminance map.
 *
 *   bmpinfo <in.bmp> [-ascii COLS] [-gain F] [-grid N]
 *
 * -ascii draws the frame with a ramp from ' ' (black) to '#' (white); COLS is
 * the character width, the height follows the aspect ratio.  -gain multiplies
 * before mapping, for captures whose content is present but dark.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(int argc, char **argv)
{
    const char *in = NULL;
    int cols = 0, grid = 0;
    double gain = 1.0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-ascii") && i + 1 < argc) cols = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-gain") && i + 1 < argc) gain = atof(argv[++i]);
        else if (!strcmp(argv[i], "-grid") && i + 1 < argc) grid = atoi(argv[++i]);
        else if (argv[i][0] != '-') in = argv[i];
        else { fprintf(stderr, "bmpinfo: unknown option %s\n", argv[i]); return 2; }
    }
    if (!in) {
        fprintf(stderr, "usage: bmpinfo <in.bmp> [-ascii COLS] [-gain F] [-grid N]\n");
        return 2;
    }
    if (gain <= 0.0) gain = 1.0;

    FILE *f = fopen(in, "rb");
    if (!f) { fprintf(stderr, "bmpinfo: cannot open %s\n", in); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *bmp = (uint8_t *)malloc((size_t)size);
    if (!bmp || fread(bmp, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "bmpinfo: short read\n");
        return 1;
    }
    fclose(f);
    if (size < 54 || bmp[0] != 'B' || bmp[1] != 'M') {
        fprintf(stderr, "bmpinfo: not a BMP\n");
        return 1;
    }
    uint32_t off = rd32(bmp + 10);
    int32_t w = (int32_t)rd32(bmp + 18);
    int32_t hraw = (int32_t)rd32(bmp + 22);
    uint16_t bpp = (uint16_t)(bmp[28] | (bmp[29] << 8));
    if (bpp != 32 || w <= 0 || hraw == 0) {
        fprintf(stderr, "bmpinfo: need 32bpp, got %ux%u %ubpp\n", (unsigned)w,
                (unsigned)(hraw < 0 ? -hraw : hraw), (unsigned)bpp);
        return 1;
    }
    int bottom_up = hraw > 0;
    uint32_t h = (uint32_t)(hraw < 0 ? -hraw : hraw);
    uint32_t row = (uint32_t)w * 4;
    if ((uint64_t)off + (uint64_t)row * h > (uint64_t)size) {
        fprintf(stderr, "bmpinfo: truncated\n");
        return 1;
    }
#define PIX(x, y) (bmp + off + (size_t)(bottom_up ? (h - 1 - (y)) : (y)) * row + (size_t)(x) * 4)

    printf("%s: %ux%u 32bpp %s\n", in, (unsigned)w, h,
           bottom_up ? "bottom-up" : "top-down");

    /* Brightness histogram and a rough colour summary. */
    unsigned hist[8];
    memset(hist, 0, sizeof hist);
    uint64_t sum[3] = { 0, 0, 0 };
    unsigned nonblack = 0, saturated = 0;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = PIX(x, y);
            unsigned r = p[2], g = p[1], b = p[0];
            unsigned v = r > g ? (r > b ? r : b) : (g > b ? g : b);
            hist[v >> 5]++;
            sum[0] += r; sum[1] += g; sum[2] += b;
            if (v > 8) nonblack++;
            if (v > 250) saturated++;
        }
    }
    double n = (double)w * h;
    printf("  mean rgb       %.1f %.1f %.1f\n",
           sum[0] / n, sum[1] / n, sum[2] / n);
    printf("  non-black      %u (%.1f%%)\n", nonblack, 100.0 * nonblack / n);
    printf("  saturated      %u (%.2f%%)\n", saturated, 100.0 * saturated / n);
    printf("  brightness     ");
    for (int i = 0; i < 8; i++)
        printf("%u:%u ", i * 32, hist[i]);
    printf("\n");

    if (grid > 0) {
        printf("  grid %dx%d (rgb at sample points):\n", grid, grid);
        for (int gy = 0; gy < grid; gy++) {
            printf("   ");
            for (int gx = 0; gx < grid; gx++) {
                uint32_t x = (uint32_t)((double)gx / grid * (w - 1));
                uint32_t y = (uint32_t)((double)gy / grid * (h - 1));
                const uint8_t *p = PIX(x, y);
                printf(" %02X%02X%02X", p[2], p[1], p[0]);
            }
            printf("\n");
        }
    }

    if (cols > 0) {
        static const char ramp[] = " .:-=+*#%@";
        int rampn = (int)sizeof ramp - 1;
        int rows = (int)((double)cols * h / w * 0.5);   /* chars are ~2:1 */
        if (rows < 1) rows = 1;
        printf("  ascii %dx%d (gain %.2f):\n", cols, rows, gain);
        for (int ry = 0; ry < rows; ry++) {
            printf("   ");
            for (int rx = 0; rx < cols; rx++) {
                /* Average the block behind this character. */
                uint32_t x0 = (uint32_t)((double)rx / cols * w);
                uint32_t x1 = (uint32_t)((double)(rx + 1) / cols * w);
                uint32_t y0 = (uint32_t)((double)ry / rows * h);
                uint32_t y1 = (uint32_t)((double)(ry + 1) / rows * h);
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                uint64_t acc = 0, cnt = 0;
                for (uint32_t y = y0; y < y1; y += 2)
                    for (uint32_t x = x0; x < x1; x += 2) {
                        const uint8_t *p = PIX(x, y);
                        unsigned r = p[2], g = p[1], b = p[0];
                        unsigned v = r > g ? (r > b ? r : b) : (g > b ? g : b);
                        acc += v;
                        cnt++;
                    }
                double v = cnt ? (double)acc / cnt * gain : 0.0;
                if (v > 255.0) v = 255.0;
                int idx = (int)(v / 255.0 * (rampn - 1) + 0.5);
                putchar(ramp[idx]);
            }
            putchar('\n');
        }
    }
#undef PIX
    free(bmp);
    return 0;
}
