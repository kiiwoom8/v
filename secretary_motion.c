#include "common.h"
#include "secretary_motion.h"
#include <stdlib.h>
#include <string.h>

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

#define COMPRESS_XPRESS_HUFF 4
#define MAX_FRAME_SIDE 640
#define PALETTE_BYTES (256 * sizeof(DWORD))

typedef struct {
    char magic[4];
    WORD width, height, edges, frames;
} MotionHeader;

typedef struct {
    WORD from, to, first, count;
} MotionEntry;

typedef struct {
    int low, high;
    DWORD fraction;
} Sample;

static struct {
    HMODULE cabinet;
    HANDLE decoder;
    BOOL (WINAPI *decompress)(HANDLE, const void *, SIZE_T, void *, SIZE_T, SIZE_T *);
    BOOL (WINAPI *close)(HANDLE);
    const MotionHeader *header;
    const MotionEntry *entries;
    const DWORD *offsets;
    const BYTE *packets;
    BYTE *decoded;
    int width, height;
    Sample x[MAX_FRAME_SIDE], y[MAX_FRAME_SIDE];
} pack;

static void samples(Sample *lookup, int output, int source)
{
    int i;
    for (i = 0; i < output; ++i) {
        int at = ((2 * i + 1) * source * 128 / output) - 128;
        at = max(0, min(at, (source - 1) * 256));
        lookup[i].low = at / 256;
        lookup[i].high = min(lookup[i].low + 1, source - 1);
        lookup[i].fraction = at % 256;
    }
}

void secretary_motion_close(void)
{
    if (pack.decoder) pack.close(pack.decoder);
    if (pack.cabinet) FreeLibrary(pack.cabinet);
    free(pack.decoded);
    ZeroMemory(&pack, sizeof(pack));
}

void secretary_motion_open(HINSTANCE instance, int width, int height)
{
    HRSRC resource = FindResourceW(instance, L"TAILMON_MOTION", RT_RCDATA);
    const MotionHeader *header = resource ? LockResource(LoadResource(instance, resource)) : NULL;
    DWORD bytes = resource ? SizeofResource(instance, resource) : 0;
    BOOL (WINAPI *create)(DWORD, void *, HANDLE *);
    size_t data_at;
    unsigned i;

    secretary_motion_close();
    if (!header || bytes < sizeof(*header) || memcmp(header->magic, "VTM2", 4) ||
        !header->width || header->width > MAX_FRAME_SIDE || !header->height || header->height > MAX_FRAME_SIDE ||
        !header->edges || !header->frames || width <= 0 || width > MAX_FRAME_SIDE || height <= 0 || height > MAX_FRAME_SIDE)
        return;
    data_at = sizeof(*header) + header->edges * sizeof(MotionEntry) + (header->frames + 1) * sizeof(DWORD);
    if (data_at >= bytes) return;
    pack.entries = (const MotionEntry *)(header + 1);
    pack.offsets = (const DWORD *)(pack.entries + header->edges);
    for (i = 0; i < header->frames; ++i) {
        if (pack.offsets[i] >= pack.offsets[i + 1]) return;
    }
    if (pack.offsets[header->frames] != bytes - data_at) return;
    pack.packets = (const BYTE *)(pack.offsets + header->frames + 1);
    pack.cabinet = LoadLibraryExW(L"cabinet.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (pack.cabinet) {
        create = (void *)GetProcAddress(pack.cabinet, "CreateDecompressor");
        pack.decompress = (void *)GetProcAddress(pack.cabinet, "Decompress");
        pack.close = (void *)GetProcAddress(pack.cabinet, "CloseDecompressor");
        if (create && pack.decompress && pack.close && create(COMPRESS_XPRESS_HUFF, NULL, &pack.decoder)) {
            pack.decoded = malloc(PALETTE_BYTES + (size_t)header->width * header->height);
            if (pack.decoded) {
                pack.header = header;
                pack.width = width;
                pack.height = height;
                samples(pack.x, width, header->width);
                samples(pack.y, height, header->height);
                return;
            }
        }
    }
    secretary_motion_close();
}

SecretaryMotion secretary_motion_find(int from, int to, int pose_count)
{
    SecretaryMotion motion = {0};
    int low = 0, high, a, b;
    BOOL flip_from, flip_to;
    if (!pack.header || from < 0 || to < 0 || from == to) return motion;
    flip_from = from >= pose_count;
    flip_to = to >= pose_count;
    from %= pose_count;
    to %= pose_count;
    a = min(from, to);
    b = max(from, to) + (flip_from != flip_to ? pose_count : 0);
    high = pack.header->edges;
    while (low < high) {
        int mid = low + (high - low) / 2;
        const MotionEntry *edge = &pack.entries[mid];
        if (edge->from == a && edge->to == b) {
            if (edge->first + edge->count <= pack.header->frames) {
                motion.first = edge->first;
                motion.count = edge->count;
                motion.reverse = from > to;
                motion.flip = motion.reverse ? flip_to : flip_from;
            }
            break;
        }
        if (a < edge->from || (a == edge->from && b < edge->to)) high = mid;
        else low = mid + 1;
    }
    return motion;
}

static DWORD blend(DWORD a, DWORD b, DWORD fraction)
{
    const DWORD mask = 0x00ff00ff;
    DWORD rb = ((a & mask) * (256 - fraction) + (b & mask) * fraction) >> 8;
    DWORD ag = (((a >> 8) & mask) * (256 - fraction) + ((b >> 8) & mask) * fraction);
    return (rb & mask) | (ag & ~mask);
}

BOOL secretary_motion_paint(SecretaryMotion motion, unsigned frame, DWORD *pixels)
{
    const MotionHeader *header = pack.header;
    SIZE_T written, wanted;
    unsigned index;
    int x, y;
    const DWORD *palette = (const DWORD *)pack.decoded;
    const BYTE *image = pack.decoded ? pack.decoded + PALETTE_BYTES : NULL;
    if (!header || frame >= motion.count) return FALSE;
    index = motion.first + (motion.reverse ? motion.count - 1 - frame : frame);
    if (index >= header->frames) return FALSE;
    wanted = PALETTE_BYTES + (size_t)header->width * header->height;
    if (!pack.decompress(pack.decoder, pack.packets + pack.offsets[index],
                         pack.offsets[index + 1] - pack.offsets[index], pack.decoded, wanted, &written) || written != wanted)
        return FALSE;
    if (pack.width == header->width && pack.height == header->height) {
        for (y = 0; y < pack.height; ++y) {
            for (x = 0; x < pack.width; ++x)
                pixels[x] = palette[image[motion.flip ? pack.width - 1 - x : x]];
            image += pack.width;
            pixels += pack.width;
        }
        return TRUE;
    }
    for (y = 0; y < pack.height; ++y) {
        Sample sy = pack.y[y];
        const BYTE *top = image + sy.low * header->width, *bottom = image + sy.high * header->width;
        for (x = 0; x < pack.width; ++x) {
            Sample sx = pack.x[motion.flip ? pack.width - 1 - x : x];
            DWORD a = blend(palette[top[sx.low]], palette[top[sx.high]], sx.fraction);
            DWORD b = blend(palette[bottom[sx.low]], palette[bottom[sx.high]], sx.fraction);
            pixels[x] = blend(a, b, sy.fraction);
        }
        pixels += pack.width;
    }
    return TRUE;
}
