// make_icon.c - writes vim.ico from the same drawing code tray.c uses for the
// tray icon. Run with: mingw32-make icon

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char r, g, b, a;
} RGBA;

static const int g_sizes[] = { 16, 20, 24, 32, 40, 48, 64, 256 };

static void blend_px(RGBA *px, int r, int g, int b, int a)
{
    int out_a, out_r, out_g, out_b;

    if (a <= 0)
        return;
    if (a >= 255) {
        px->r = (unsigned char)r;
        px->g = (unsigned char)g;
        px->b = (unsigned char)b;
        px->a = 255;
        return;
    }

    out_a = a + px->a * (255 - a) / 255;
    if (out_a <= 0) {
        px->a = 0;
        return;
    }
    out_r = (r * a + px->r * px->a * (255 - a) / 255) / out_a;
    out_g = (g * a + px->g * px->a * (255 - a) / 255) / out_a;
    out_b = (b * a + px->b * px->a * (255 - a) / 255) / out_a;
    px->r = (unsigned char)out_r;
    px->g = (unsigned char)out_g;
    px->b = (unsigned char)out_b;
    px->a = (unsigned char)out_a;
}

static void fill_round_rect(RGBA *px, int size, int radius,
                            int r, int g, int b, int a)
{
    int x, y;

    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            int dx = 0, dy = 0;
            if (x < radius)
                dx = radius - x;
            else if (x >= size - radius)
                dx = x - (size - radius) + 1;
            if (y < radius)
                dy = radius - y;
            else if (y >= size - radius)
                dy = y - (size - radius) + 1;
            if (dx == 0 && dy == 0)
                blend_px(&px[y * size + x], r, g, b, a);
            else if (dx * dx + dy * dy <= radius * radius)
                blend_px(&px[y * size + x], r, g, b, a);
        }
    }
}

static int point_in_poly(double x, double y, const double *pts, int n)
{
    int i, j, inside = 0;

    for (i = 0, j = n - 1; i < n; j = i++) {
        double xi = pts[2 * i], yi = pts[2 * i + 1];
        double xj = pts[2 * j], yj = pts[2 * j + 1];
        if (((yi > y) != (yj > y)) &&
            (x < (xj - xi) * (y - yi) / (yj - yi) + xi))
            inside = !inside;
    }
    return inside;
}

static void fill_pointer(RGBA *px, int size)
{
    static const double pts[] = {
        0.34, 0.18,
        0.34, 0.82,
        0.46, 0.68,
        0.55, 0.90,
        0.64, 0.86,
        0.55, 0.64,
        0.72, 0.61
    };
    int x, y;

    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            double fx = ((double)x + 0.5) / (double)size;
            double fy = ((double)y + 0.5) / (double)size;
            if (point_in_poly(fx, fy, pts, 7))
                blend_px(&px[y * size + x], 255, 255, 255, 255);
        }
    }
}

static void render(RGBA *px, int size)
{
    int radius = size / 4;

    if (radius < 1)
        radius = 1;
    memset(px, 0, (size_t)size * (size_t)size * sizeof(RGBA));
    fill_round_rect(px, size, radius, 58, 58, 60, 255);
    fill_pointer(px, size);
}

static int and_stride(int size)
{
    return ((size + 31) / 32) * 4;
}

static int dib_size(int size)
{
    return 40 + size * size * 4 + and_stride(size) * size;
}

static int put_u8(FILE *f, unsigned v)
{
    return fputc((int)(v & 0xff), f) != EOF;
}

static int put_u16(FILE *f, unsigned v)
{
    return put_u8(f, v) && put_u8(f, v >> 8);
}

static int put_u32(FILE *f, unsigned v)
{
    return put_u16(f, v & 0xffff) && put_u16(f, v >> 16);
}

static int put_dib(FILE *f, int size, const RGBA *px)
{
    int stride = and_stride(size);
    int x, y;

    put_u32(f, 40);                     // BITMAPINFOHEADER
    put_u32(f, (unsigned)size);
    put_u32(f, (unsigned)size * 2);     // XOR + AND bitmaps
    put_u16(f, 1);
    put_u16(f, 32);
    put_u32(f, 0);                      // BI_RGB
    put_u32(f, (unsigned)dib_size(size) - 40);
    put_u32(f, 0);
    put_u32(f, 0);
    put_u32(f, 0);
    put_u32(f, 0);

    // Bottom-up BGRA, straight (not premultiplied) alpha as an .ico expects.
    for (y = size - 1; y >= 0; y--) {
        for (x = 0; x < size; x++) {
            const RGBA *p = &px[y * size + x];
            put_u8(f, p->b);
            put_u8(f, p->g);
            put_u8(f, p->r);
            put_u8(f, p->a);
        }
    }

    // The AND mask marks fully transparent pixels for old icon renderers.
    for (y = size - 1; y >= 0; y--) {
        unsigned char row[128];
        memset(row, 0, (size_t)stride);
        for (x = 0; x < size; x++) {
            if (px[y * size + x].a == 0)
                row[x / 8] |= (unsigned char)(0x80 >> (x % 8));
        }
        fwrite(row, 1, (size_t)stride, f);
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "vim.ico";
    int count = (int)(sizeof(g_sizes) / sizeof(g_sizes[0]));
    unsigned offset;
    FILE *f;
    int i;

    if (argc > 2) {
        fprintf(stderr, "usage: %s [vim.ico]\n", argv[0]);
        return 2;
    }

    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }

    put_u16(f, 0);
    put_u16(f, 1);
    put_u16(f, (unsigned)count);

    offset = 6 + (unsigned)count * 16;
    for (i = 0; i < count; i++) {
        int size = g_sizes[i];
        put_u8(f, (unsigned)(size >= 256 ? 0 : size));
        put_u8(f, (unsigned)(size >= 256 ? 0 : size));
        put_u8(f, 0);
        put_u8(f, 0);
        put_u16(f, 1);
        put_u16(f, 32);
        put_u32(f, (unsigned)dib_size(size));
        put_u32(f, offset);
        offset += (unsigned)dib_size(size);
    }

    for (i = 0; i < count; i++) {
        int size = g_sizes[i];
        RGBA *px = (RGBA *)malloc((size_t)size * (size_t)size *
                                  sizeof(RGBA));
        if (!px) {
            fclose(f);
            return 1;
        }
        render(px, size);
        put_dib(f, size, px);
        free(px);
    }

    if (fclose(f) != 0) {
        fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    return 0;
}
