// make_icon.c - writes vim.ico from the same drawing code tray.c uses for the
// tray icon. Run with: mingw32-make icon
//
// The small sizes are 32-bit BMP entries. The 256x256 image is stored as PNG
// (supported since Vista) so vim.exe does not double in size; WIC does the
// encoding and the COM vtables below are the few methods it needs.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char r, g, b, a;
} RGBA;

static const int g_sizes[] = { 16, 20, 24, 32, 40, 48, 64, 256 };
#define PNG_MIN_SIZE 256
#define WICBitmapEncoderNoCache 2

// ---------------------------------------------------------------------------
// WIC

typedef struct IWICImagingFactory IWICImagingFactory;
typedef struct IWICBitmapEncoder IWICBitmapEncoder;
typedef struct IWICBitmapFrameEncode IWICBitmapFrameEncode;

typedef struct IWICImagingFactoryVtbl {
    void *QueryInterface;
    void *AddRef;
    ULONG (STDMETHODCALLTYPE *Release)(IWICImagingFactory *This);
    void *CreateDecoderFromFilename;
    void *CreateDecoderFromStream;
    void *CreateDecoderFromFileHandle;
    void *CreateComponentInfo;
    void *CreateDecoder;
    HRESULT (STDMETHODCALLTYPE *CreateEncoder)(IWICImagingFactory *This,
        const GUID *format, const GUID *vendor, IWICBitmapEncoder **encoder);
} IWICImagingFactoryVtbl;

typedef struct IWICBitmapEncoderVtbl {
    void *QueryInterface;
    void *AddRef;
    ULONG (STDMETHODCALLTYPE *Release)(IWICBitmapEncoder *This);
    HRESULT (STDMETHODCALLTYPE *Initialize)(IWICBitmapEncoder *This,
        IStream *stream, DWORD cache_option);
    void *GetContainerFormat;
    void *GetEncoderInfo;
    void *SetColorContexts;
    void *SetPalette;
    void *SetThumbnail;
    void *SetPreview;
    HRESULT (STDMETHODCALLTYPE *CreateNewFrame)(IWICBitmapEncoder *This,
        IWICBitmapFrameEncode **frame, void *options);
    HRESULT (STDMETHODCALLTYPE *Commit)(IWICBitmapEncoder *This);
} IWICBitmapEncoderVtbl;

typedef struct IWICBitmapFrameEncodeVtbl {
    void *QueryInterface;
    void *AddRef;
    ULONG (STDMETHODCALLTYPE *Release)(IWICBitmapFrameEncode *This);
    HRESULT (STDMETHODCALLTYPE *Initialize)(IWICBitmapFrameEncode *This,
        void *options);
    HRESULT (STDMETHODCALLTYPE *SetSize)(IWICBitmapFrameEncode *This,
        UINT width, UINT height);
    void *SetResolution;
    HRESULT (STDMETHODCALLTYPE *SetPixelFormat)(IWICBitmapFrameEncode *This,
        GUID *format);
    void *SetColorContexts;
    void *SetPalette;
    void *SetThumbnail;
    HRESULT (STDMETHODCALLTYPE *WritePixels)(IWICBitmapFrameEncode *This,
        UINT lines, UINT stride, UINT size, BYTE *pixels);
    void *WriteSource;
    HRESULT (STDMETHODCALLTYPE *Commit)(IWICBitmapFrameEncode *This);
} IWICBitmapFrameEncodeVtbl;

struct IWICImagingFactory { IWICImagingFactoryVtbl *lpVtbl; };
struct IWICBitmapEncoder { IWICBitmapEncoderVtbl *lpVtbl; };
struct IWICBitmapFrameEncode { IWICBitmapFrameEncodeVtbl *lpVtbl; };

static const GUID g_clsid_wic_factory = {
    0xcacaf262, 0x9370, 0x4615,
    { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a }
};
static const GUID g_iid_wic_factory = {
    0xec5ec8a9, 0xc395, 0x4314,
    { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 }
};
static const GUID g_fmt_png = {
    0x1b7cfaf4, 0x713f, 0x473c,
    { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf }
};
static const GUID g_pf_bgra = {
    0x6fddc324, 0x4e03, 0x4bfe,
    { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f }
};

static int encode_png(const RGBA *px, int size, unsigned char **out,
                      unsigned *out_size)
{
    IStream *stream = NULL;
    HGLOBAL global = NULL;
    IWICImagingFactory *factory = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    GUID format = g_pf_bgra;
    BYTE *bgra = NULL;
    LARGE_INTEGER zero;
    ULARGE_INTEGER end;
    void *locked;
    int x, y, ok = 0;
    HRESULT hr = S_OK;
    const char *step = "start";

    ZeroMemory(&zero, sizeof(zero));
    ZeroMemory(&end, sizeof(end));

    step = "CoCreateInstance";
    hr = CoCreateInstance(&g_clsid_wic_factory, NULL, CLSCTX_INPROC_SERVER,
                          &g_iid_wic_factory, (void **)&factory);
    if (FAILED(hr))
        goto done;
    step = "CreateStreamOnHGlobal";
    hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
    if (FAILED(hr))
        goto done;
    step = "CreateEncoder";
    hr = factory->lpVtbl->CreateEncoder(factory, &g_fmt_png, NULL, &encoder);
    if (FAILED(hr))
        goto done;
    step = "EncoderInitialize";
    hr = encoder->lpVtbl->Initialize(encoder, stream, WICBitmapEncoderNoCache);
    if (FAILED(hr))
        goto done;
    step = "CreateNewFrame";
    hr = encoder->lpVtbl->CreateNewFrame(encoder, &frame, NULL);
    if (FAILED(hr))
        goto done;
    step = "FrameInitialize";
    hr = frame->lpVtbl->Initialize(frame, NULL);
    if (FAILED(hr))
        goto done;
    step = "SetSize";
    hr = frame->lpVtbl->SetSize(frame, (UINT)size, (UINT)size);
    if (FAILED(hr))
        goto done;
    step = "SetPixelFormat";
    hr = frame->lpVtbl->SetPixelFormat(frame, &format);
    if (FAILED(hr))
        goto done;
    if (memcmp(&format, &g_pf_bgra, sizeof(GUID)) != 0) {
        step = "pixel format changed";
        hr = E_FAIL;
        goto done;
    }

    bgra = (BYTE *)malloc((size_t)size * (size_t)size * 4);
    if (!bgra)
        goto done;
    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            const RGBA *p = &px[y * size + x];
            BYTE *d = &bgra[((size_t)y * (size_t)size + (size_t)x) * 4];
            d[0] = p->b;
            d[1] = p->g;
            d[2] = p->r;
            d[3] = p->a;
        }
    }
    step = "WritePixels";
    hr = frame->lpVtbl->WritePixels(frame, (UINT)size, (UINT)size * 4,
                                    (UINT)size * (UINT)size * 4, bgra);
    if (FAILED(hr))
        goto done;
    step = "FrameCommit";
    hr = frame->lpVtbl->Commit(frame);
    if (FAILED(hr))
        goto done;
    step = "EncoderCommit";
    hr = encoder->lpVtbl->Commit(encoder);
    if (FAILED(hr))
        goto done;

    step = "StreamSeek";
    hr = stream->lpVtbl->Seek(stream, zero, STREAM_SEEK_END, &end);
    if (FAILED(hr))
        goto done;
    if (end.QuadPart <= 0 || end.QuadPart > 0x7fffffff) {
        step = "stream length";
        hr = E_FAIL;
        goto done;
    }
    step = "GetHGlobalFromStream";
    hr = GetHGlobalFromStream(stream, &global);
    if (FAILED(hr))
        goto done;
    locked = GlobalLock(global);
    if (!locked)
        goto done;
    *out_size = (unsigned)end.QuadPart;
    *out = (unsigned char *)malloc(*out_size);
    if (*out)
        memcpy(*out, locked, *out_size);
    GlobalUnlock(global);
    if (!*out)
        goto done;
    ok = 1;

done:
    if (!ok)
        fprintf(stderr, "encode_png failed at %s (hr=0x%08lx)\n", step,
                (unsigned long)hr);
    if (!ok && out && *out) {
        free(*out);
        *out = NULL;
    }
    if (frame)
        frame->lpVtbl->Release(frame);
    if (encoder)
        encoder->lpVtbl->Release(encoder);
    if (stream)
        stream->lpVtbl->Release(stream);
    if (factory)
        factory->lpVtbl->Release(factory);
    free(bgra);
    return ok;
}

// ---------------------------------------------------------------------------
// Drawing

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

// ---------------------------------------------------------------------------
// ICO writing

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
    unsigned char *png[sizeof(g_sizes) / sizeof(g_sizes[0])];
    unsigned png_size[sizeof(g_sizes) / sizeof(g_sizes[0])];
    unsigned entry_size[sizeof(g_sizes) / sizeof(g_sizes[0])];
    unsigned offset;
    HRESULT co;
    FILE *f;
    int i;

    if (argc > 2) {
        fprintf(stderr, "usage: %s [vim.ico]\n", argv[0]);
        return 2;
    }

    memset(png, 0, sizeof(png));
    memset(png_size, 0, sizeof(png_size));

    co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(co)) {
        fprintf(stderr, "cannot start COM for the PNG encoder\n");
        return 1;
    }

    // Encode the large image first: its compressed size is needed in the
    // directory, while the BMP entries have a fixed size.
    for (i = 0; i < count; i++) {
        int size = g_sizes[i];

        if (size >= PNG_MIN_SIZE) {
            RGBA *px = (RGBA *)malloc((size_t)size * (size_t)size *
                                      sizeof(RGBA));
            if (!px || !encode_png(px, size, &png[i], &png_size[i])) {
                free(px);
                fprintf(stderr, "cannot encode the %dx%d PNG\n", size, size);
                goto fail;
            }
            free(px);
            entry_size[i] = png_size[i];
        } else {
            entry_size[i] = (unsigned)dib_size(size);
        }
    }

    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        goto fail;
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
        put_u32(f, entry_size[i]);
        put_u32(f, offset);
        offset += entry_size[i];
    }

    for (i = 0; i < count; i++) {
        int size = g_sizes[i];

        if (size >= PNG_MIN_SIZE) {
            fwrite(png[i], 1, (size_t)png_size[i], f);
        } else {
            RGBA *px = (RGBA *)malloc((size_t)size * (size_t)size *
                                      sizeof(RGBA));
            if (!px) {
                fclose(f);
                goto fail;
            }
            render(px, size);
            put_dib(f, size, px);
            free(px);
        }
    }

    if (fclose(f) != 0) {
        fprintf(stderr, "cannot write %s\n", path);
        goto fail;
    }
    for (i = 0; i < count; i++)
        free(png[i]);
    CoUninitialize();
    return 0;

fail:
    for (i = 0; i < count; i++)
        free(png[i]);
    CoUninitialize();
    return 1;
}
