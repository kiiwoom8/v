// A layered desktop secretary that passes wheel input to the window underneath.
#include "common.h"
#include <objbase.h> // COM property types for the MinGW-w64 GDI+ headers
#include <gdiplus.h>
#include <shellapi.h>
#include <windowsx.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef SPI_GETCLIENTAREAANIMATION
#define SPI_GETCLIENTAREAANIMATION 0x1042
#endif

#define BUBBLE_CLASS L"VimMouseSecretaryBubble"
#define POSITION_KEY L"Software\\vim mouse\\Secretary"
#define FRAME_MS 8
#define FALLBACK_FRAME_MS 16
#define WM_SECRETARY_FRAME (WM_APP + 1)
#define WM_SECRETARY_BUTTON (WM_APP + 2)
#define MOTION_GRID 7
#define MAX_ATLAS_SIZE 512
#define PIXEL_UNIT 256
#define BUBBLE_MS 165
#define POSES_PER_ROW 4
#define POSES_PER_SHEET (POSES_PER_ROW * POSES_PER_ROW)
#define POSE_COUNT ((1 + SECRETARY_EXTRA_SHEETS) * POSES_PER_SHEET)
#define WALK_FIRST (POSES_PER_SHEET * (1 + SECRETARY_MOVEMENT))
#define REST_FIRST (POSES_PER_SHEET * (1 + SECRETARY_REST))
#define COMMAND_FIRST (POSES_PER_SHEET * (1 + SECRETARY_REACTIONS))
#define WALK_DETAIL_FIRST (POSES_PER_SHEET * (1 + SECRETARY_WALK_DETAIL))
#define ATTENTION_FIRST (POSES_PER_SHEET * (1 + SECRETARY_ATTENTION))
#define SCROLL_DETAIL_FIRST (POSES_PER_SHEET * (1 + SECRETARY_SCROLL_DETAIL))
#define IDLE_DETAIL_FIRST (POSES_PER_SHEET * (1 + SECRETARY_IDLE_DETAIL))
#define INTERACTION_FIRST (POSES_PER_SHEET * (1 + SECRETARY_INTERACTION_DETAIL))

typedef struct {
    HDC dc;
    HBITMAP dib;
    HGDIOBJ old;
    DWORD *pixels;
    GpBitmap *bitmap;
    GpGraphics *graphics;
    SIZE size;
} Canvas;

typedef struct {
    BYTE pose;
    WORD duration_ms;
} PoseStep;

enum { REACT_NONE, REACT_CLICK, REACT_MENU, REACT_LAND };
enum { PET_WAIT, PET_WALK, PET_SETTLE, PET_NAP, PET_GROOM, PET_STRETCH, PET_YAWN };
enum { BUBBLE_EDIT, BUBBLE_SCREEN, BUBBLE_ROAM, BUBBLE_HOME, BUBBLE_PROMPT, BUBBLE_ACTIONS };

static const PoseStep idle[] = {
    {0, 1150}, {1, 260}, {0, 450}, {2, 100}, {0, 675}, {3, 310}, {0, 820}
};
static const PoseStep bored[] = {
    {4, 680}, {5, 245}, {6, 570}, {7, 480}, {4, 400}
};
static const PoseStep grabbed[] = {
    {8, 160}, {9, 160}, {8, 120}, {10, 155}, {11, 140}
};
static const PoseStep clicked[] = {
    {12, 90}, {13, 170}, {14, 240}, {15, 230}
};
static const PoseStep breathing_detail[] = {
    {IDLE_DETAIL_FIRST, 570}, {IDLE_DETAIL_FIRST + 1, 420},
    {IDLE_DETAIL_FIRST + 2, 390}, {IDLE_DETAIL_FIRST + 3, 85},
    {IDLE_DETAIL_FIRST + 4, 90}, {IDLE_DETAIL_FIRST + 5, 70},
    {IDLE_DETAIL_FIRST + 6, 210}, {IDLE_DETAIL_FIRST + 7, 490},
    {IDLE_DETAIL_FIRST + 6, 375}, {IDLE_DETAIL_FIRST + 1, 410}
};
static const PoseStep yawn_detail[] = {
    {IDLE_DETAIL_FIRST + 8, 300}, {IDLE_DETAIL_FIRST + 9, 210},
    {IDLE_DETAIL_FIRST + 10, 150}, {IDLE_DETAIL_FIRST + 11, 150},
    {IDLE_DETAIL_FIRST + 12, 520}, {IDLE_DETAIL_FIRST + 13, 200},
    {IDLE_DETAIL_FIRST + 14, 240}, {IDLE_DETAIL_FIRST + 15, 220}
};
static const PoseStep grab_detail[] = {
    {INTERACTION_FIRST, 125}, {INTERACTION_FIRST + 1, 90},
    {INTERACTION_FIRST + 2, 105}, {INTERACTION_FIRST + 3, 130},
    {INTERACTION_FIRST + 4, 135}, {INTERACTION_FIRST + 5, 95},
    {INTERACTION_FIRST + 6, 105}, {INTERACTION_FIRST + 7, 115}
};
static const PoseStep click_detail[] = {
    {INTERACTION_FIRST + 8, 65}, {INTERACTION_FIRST + 9, 80},
    {INTERACTION_FIRST + 10, 90}, {INTERACTION_FIRST + 11, 95},
    {INTERACTION_FIRST + 12, 195}, {INTERACTION_FIRST + 13, 105},
    {INTERACTION_FIRST + 14, 115}, {INTERACTION_FIRST + 15, 130}
};
static const PoseStep landing_detail[] = {
    {INTERACTION_FIRST + 6, 60}, {INTERACTION_FIRST + 7, 70},
    {INTERACTION_FIRST + 14, 120}, {INTERACTION_FIRST + 15, 170}
};
static const PoseStep menu_reaction[] = {{12, 440}, {15, 210}};
static const PoseStep landed[] = {{11, 85}, {15, 260}};
static const PoseStep settling[] = {
    {WALK_FIRST + 12, 95}, {WALK_FIRST + 13, 175},
    {WALK_FIRST + 14, 145}, {WALK_FIRST + 15, 130}
};
static const PoseStep falling_asleep[] = {
    {REST_FIRST, 280}, {REST_FIRST + 1, 450}, {REST_FIRST + 2, 410}, {REST_FIRST + 3, 300}
};
static const PoseStep sleeping[] = {
    {REST_FIRST + 4, 520}, {REST_FIRST + 5, 460},
    {REST_FIRST + 6, 580}, {REST_FIRST + 7, 290}, {REST_FIRST + 6, 470}
};
static const PoseStep stretching[] = {
    {REST_FIRST + 8, 240}, {REST_FIRST + 9, 235}, {REST_FIRST + 10, 430}, {REST_FIRST + 11, 300}
};
static const PoseStep grooming[] = {
    {REST_FIRST + 12, 210}, {REST_FIRST + 13, 260},
    {REST_FIRST + 14, 295}, {REST_FIRST + 13, 210}, {REST_FIRST + 15, 240}
};

typedef struct {
    HWND window;
    RECT bounds;
    BYTE *mask;
    size_t capacity;
} PetHitMap;

static HWND pet, bubble;
// The UI fills the inactive bitmap before publishing it. The mouse hook
// takes a nonblocking read lock only when a button goes down.
static SRWLOCK hit_map_lock = {0};
static PetHitMap hit_maps[2];
static PetHitMap *active_hit_map;
static PetHitMap *next_hit_map = &hit_maps[0];
static HWND routed_left, routed_right; // input thread only
static volatile LONG routed_left_down;
static BOOL route_mouse, routed_press; // UI thread only
static HANDLE frame_timer, frame_wait;
static LARGE_INTEGER animation_frequency;
static volatile LONG frame_posted;
static ULONG_PTR gdiplus;
static Canvas sprite, speech;
static GpImage *image;
static GpImage *extra_images[SECRETARY_EXTRA_SHEETS];
static BOOL extra_ready[SECRETARY_EXTRA_SHEETS];
static WCHAR extra_settings[SECRETARY_EXTRA_SHEETS][MAX_PATH];
static DWORD *pose_pixels;
static POINT pose_clicks[POSE_COUNT];
static RECT pose_bounds[POSE_COUNT], sprite_bounds;
static int painted_pose = -1, reaction, queued_reaction;
static int blend_from_pose;
static POINT motion[MOTION_GRID][MOTION_GRID];
static DWORD blend_started, blend_ms, last_yawn;
static BOOL use_atlas;
static BYTE painted_opacity;
static WCHAR image_path[APP_PATH_MAX];
static POINT anchor, home, pet_at, press, press_at, click_pixel, bubble_at;
static RECT work;
static UINT dpi;
static DWORD born, pulse, reaction_started, bubble_tick, last_hint, last_activity, press_started;
static BOOL positioned, pressed, dragging, moved, opened, animate;
static BOOL bubble_left, activating_bubble;
static BOOL bubble_dirty;
static BYTE bubble_opacity;
static POINT bubble_painted_at;
static int rows[SECRETARY_SLOTS], row_count, armed_key, focus_item, selected = -1;
static int pending_slot = -1;
static volatile LONG pending_command;
static int command_pose, queued_command;
static DWORD command_started, attention_started;
static POINT attention_hit, sleeping_hit, hint_hit;
static BOOL attention_safe, sleeping_safe, hint_animate, hint_sleeping;
static BOOL hint_waiting;
static BOOL saying_goodbye;
static struct {
    double speed, cruise, distance, traveled, stop, gait;
    POINT origin, goal;
    int curve;
    DWORD started, next, last_frame, duration, seed;
    int state;
    BOOL left, fast;
} pet_motion;
static BOOL (WINAPI *process_path)(HANDLE, DWORD, LPWSTR, PDWORD);

// A web shortcut works in the open browser and checks for an existing tab.
static struct {
    const WCHAR *exe;
    HWND target;
    DWORD began, sent, browser_ready;
    UINT request;
    int lookup;
    BOOL maximizing;
    WCHAR browser_path[MAX_PATH];
    WCHAR url[SECRETARY_TARGET_MAX];
} launch;
static UINT next_browser_request;

static int px(int n) { return MulDiv(n, dpi, 96); }

static DWORD animation_now(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (DWORD)(now.QuadPart * 1000 / animation_frequency.QuadPart);
}

static VOID CALLBACK request_frame(PVOID context, BOOLEAN timed_out)
{
    (void)timed_out;
    // The timer can run ahead of painting. Keep at most one frame queued.
    if (InterlockedExchange(&frame_posted, 1) == 0 &&
        !PostMessageW((HWND)context, WM_SECRETARY_FRAME, 0, 0))
        InterlockedExchange(&frame_posted, 0);
}

static void stop_frame_timer(void)
{
    MSG msg;
    if (frame_wait) UnregisterWaitEx(frame_wait, INVALID_HANDLE_VALUE);
    if (frame_timer) CloseHandle(frame_timer);
    frame_wait = frame_timer = NULL;
    if (pet) {
        KillTimer(pet, 1);
        while (PeekMessageW(&msg, pet, WM_SECRETARY_FRAME, WM_SECRETARY_FRAME, PM_REMOVE)) {}
    }
    InterlockedExchange(&frame_posted, 0);
}

static void start_frame_timer(void)
{
    LARGE_INTEGER due;
    stop_frame_timer();
    if (animate) {
        due.QuadPart = -FRAME_MS * 10000;
        frame_timer = create_high_res_timer();
        if (frame_timer &&
            SetWaitableTimer(frame_timer, &due, FRAME_MS, NULL, NULL, FALSE) &&
            RegisterWaitForSingleObject(&frame_wait, frame_timer, request_frame, pet,
                                        INFINITE, WT_EXECUTEDEFAULT))
            return;
        stop_frame_timer();
    }
    SetTimer(pet, 1, FALLBACK_FRAME_MS, NULL);
}

static int layout_height(void) { return use_atlas ? sprite.size.cx : sprite.size.cy; }

static int clamp(int value, int low, int high)
{
    if (high < low || value < low) return low;
    return value > high ? high : value;
}

static void free_canvas(Canvas *c)
{
    if (c->graphics) GdipDeleteGraphics(c->graphics);
    if (c->bitmap) GdipDisposeImage((GpImage *)c->bitmap);
    if (c->old) SelectObject(c->dc, c->old);
    if (c->dib) DeleteObject(c->dib);
    if (c->dc) DeleteDC(c->dc);
    ZeroMemory(c, sizeof(*c));
}

static BOOL make_canvas(Canvas *c, int width, int height)
{
    BITMAPINFO info = {0};

    free_canvas(c);
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    c->dc = CreateCompatibleDC(NULL);
    if (!c->dc) return FALSE;
    c->dib = CreateDIBSection(c->dc, &info, DIB_RGB_COLORS,
                              (void **)&c->pixels, NULL, 0);
    if (!c->dib) goto fail;
    c->old = SelectObject(c->dc, c->dib);
    if (GdipCreateBitmapFromScan0(width, height, width * 4, PixelFormat32bppPARGB,
                                  (BYTE *)c->pixels, &c->bitmap) != Ok ||
        GdipGetImageGraphicsContext((GpImage *)c->bitmap, &c->graphics) != Ok)
        goto fail;
    c->size = (SIZE){width, height};
    GdipSetSmoothingMode(c->graphics, SmoothingModeAntiAlias);
    GdipSetTextRenderingHint(c->graphics, TextRenderingHintAntiAliasGridFit);
    GdipSetInterpolationMode(c->graphics, InterpolationModeHighQualityBicubic);
    return TRUE;
fail:
    free_canvas(c);
    return FALSE;
}

static BOOL present(HWND wnd, const Canvas *c, POINT at, BYTE opacity)
{
    POINT source = {0, 0};
    SIZE size = c->size;
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA};
    return UpdateLayeredWindow(wnd, NULL, &at, &size, c->dc, &source, 0, &blend, ULW_ALPHA);
}

static void publish_hit_map(void)
{
    PetHitMap *next = next_hit_map;
    size_t i, count = (size_t)sprite.size.cx * sprite.size.cy;
    size_t bytes = (count + 7) / 8;
    if (bytes > next->capacity) {
        BYTE *mask = (BYTE *)realloc(next->mask, bytes);
        if (!mask) return;
        next->mask = mask;
        next->capacity = bytes;
    }
    ZeroMemory(next->mask, bytes);
    for (i = 0; i < count; ++i)
        if (sprite.pixels[i] >> 24)
            next->mask[i / 8] |= (BYTE)(1u << (i % 8));
    next->window = pet;
    SetRect(&next->bounds, pet_at.x, pet_at.y,
            pet_at.x + sprite.size.cx, pet_at.y + sprite.size.cy);
    AcquireSRWLockExclusive(&hit_map_lock);
    active_hit_map = next;
    next_hit_map = next == &hit_maps[0] ? &hit_maps[1] : &hit_maps[0];
    ReleaseSRWLockExclusive(&hit_map_lock);
}

static void clear_hit_map(void)
{
    AcquireSRWLockExclusive(&hit_map_lock);
    active_hit_map = NULL;
    ReleaseSRWLockExclusive(&hit_map_lock);
}

BOOL secretary_route_button(UINT message, POINT screen)
{
    HWND *routed, window;
    BOOL down;
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)
        routed = &routed_left;
    else if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP)
        routed = &routed_right;
    else
        return FALSE;
    down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN;
    if (down) {
        PetHitMap *map;
        if (!TryAcquireSRWLockShared(&hit_map_lock)) return FALSE;
        map = active_hit_map;
        window = NULL;
        if (map && PtInRect(&map->bounds, screen)) {
            size_t i = (size_t)(screen.y - map->bounds.top) *
                       (map->bounds.right - map->bounds.left) + screen.x - map->bounds.left;
            if (map->mask[i / 8] & (1u << (i % 8))) window = map->window;
        }
        ReleaseSRWLockShared(&hit_map_lock);
        if (!window || !PostMessageW(window, WM_SECRETARY_BUTTON, message,
                                    MAKELPARAM((WORD)screen.x, (WORD)screen.y)))
            return FALSE;
        *routed = window;
        if (message == WM_LBUTTONDOWN) InterlockedExchange(&routed_left_down, 1);
    } else {
        window = *routed;
        if (!window) return FALSE;
        *routed = NULL;
        if (message == WM_LBUTTONUP) InterlockedExchange(&routed_left_down, 0);
        PostMessageW(window, WM_SECRETARY_BUTTON, message,
                     MAKELPARAM((WORD)screen.x, (WORD)screen.y));
    }
    return TRUE;
}

void secretary_reset_buttons(void)
{
    routed_left = routed_right = NULL;
    InterlockedExchange(&routed_left_down, 0);
}

static BOOL white(DWORD pixel)
{
    return (pixel & 255) >= 240 && ((pixel >> 8) & 255) >= 240 &&
           ((pixel >> 16) & 255) >= 240;
}

// JPEGs can use a flat white backdrop. Flood from the perimeter, preserving
// white areas enclosed by outlines and respecting existing PNG transparency.
static void clear_backdrop(Canvas *c, RECT art)
{
    int w = c->size.cx, h = c->size.cy, head = 0, tail = 0;
    int x, y, i, *queue;
    for (y = art.top; y < art.bottom; ++y)
        for (x = art.left; x < art.right; ++x)
            if ((c->pixels[y * w + x] >> 24) < 250)
                return;
    queue = (int *)malloc((size_t)w * h * sizeof(*queue));
    if (!queue) return;
    for (y = art.top; y < art.bottom; ++y) {
        for (x = art.left; x < art.right; ++x) {
            i = y * w + x;
            if ((x == art.left || x == art.right - 1 || y == art.top ||
                 y == art.bottom - 1) && white(c->pixels[i])) {
                c->pixels[i] = 0;
                queue[tail++] = i;
            }
        }
    }
    while (head < tail) {
        int adjacent[4], j;
        i = queue[head++];
        adjacent[0] = i % w ? i - 1 : i;
        adjacent[1] = i % w < w - 1 ? i + 1 : i;
        adjacent[2] = i >= w ? i - w : i;
        adjacent[3] = i + w < w * h ? i + w : i;
        for (j = 0; j < 4; ++j) {
            int p = adjacent[j];
            if (white(c->pixels[p])) {
                c->pixels[p] = 0;
                queue[tail++] = p;
            }
        }
    }
    free(queue);
}

static int measure_sprite(POINT *click, RECT *bounds)
{
    int width = sprite.size.cx, height = sprite.size.cy, center_y = layout_height() / 2;
    int x, y, area = 0, nearest = INT_MAX;
    SetRect(bounds, width, height, 0, 0);
    for (y = 0; y < height; ++y) {
        for (x = 0; x < width; ++x) {
            int dx = x - width / 2, dy = y - center_y;
            int distance = dx * dx + dy * dy;
            if (sprite.pixels[y * width + x] >> 24 < 128) continue;
            ++area;
            if (x < bounds->left) bounds->left = x;
            if (x >= bounds->right) bounds->right = x + 1;
            if (y < bounds->top) bounds->top = y;
            if (y >= bounds->bottom) bounds->bottom = y + 1;
            if (distance < nearest) {
                *click = (POINT){x, y};
                nearest = distance;
            }
        }
    }
    return area;
}

// Some generated atlases leave low-opacity pixels across otherwise empty
// cells. Clear the haze in the rendered window and retain soft ink edges.
static void remove_alpha_haze(size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        DWORD p = sprite.pixels[i];
        int a = p >> 24;
        if (a <= 48) {
            sprite.pixels[i] = 0;
        } else {
            DWORD alpha = (DWORD)clamp((a - 48) * 255 / 200, 0, 255);
            DWORD r = ((p >> 16) & 255) * alpha / a;
            DWORD g = ((p >> 8) & 255) * alpha / a;
            DWORD b = (p & 255) * alpha / a;
            sprite.pixels[i] = (alpha << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

static void align_floor(RECT *bounds, POINT *click, int floor)
{
    int height = layout_height();
    int dy = clamp(floor - bounds->bottom, -bounds->top, height - bounds->bottom);
    size_t stride = (size_t)sprite.size.cx * sizeof(DWORD);
    BYTE *pixels = (BYTE *)sprite.pixels;
    if (dy > 0) {
        MoveMemory(pixels + dy * stride, pixels, (size_t)(height - dy) * stride);
        ZeroMemory(pixels, dy * stride);
    } else if (dy < 0) {
        MoveMemory(pixels, pixels + (size_t)(-dy) * stride, (size_t)(height + dy) * stride);
        ZeroMemory(pixels + (height + dy) * stride, -dy * stride);
    }
    OffsetRect(bounds, 0, dy);
    click->y += dy;
}

static int pose_group(int pose)
{
    int first = pose / POSES_PER_SHEET * POSES_PER_SHEET;
    int cel = pose - first;
    switch (first) {
    case ATTENTION_FIRST:
    case WALK_DETAIL_FIRST: return first;
    case WALK_FIRST: return cel < 12 ? first : first + 12;
    case SCROLL_DETAIL_FIRST:
    case IDLE_DETAIL_FIRST:
    case INTERACTION_FIRST: return first + cel / 8 * 8;
    default: return first + cel / 4 * 4;
    }
}

static int render_cel(GpImage *sheet, UINT width, UINT height, int index, int x, int y, int size, int clip_height)
{
    int cel = index % POSES_PER_SHEET, col = cel % POSES_PER_ROW, row = cel / POSES_PER_ROW;
    size_t count = (size_t)sprite.size.cx * clip_height;
    int left = col * width / POSES_PER_ROW, top = row * height / POSES_PER_ROW;
    int right = (col + 1) * width / POSES_PER_ROW, bottom = (row + 1) * height / POSES_PER_ROW;
    GdipGraphicsClear(sprite.graphics, 0);
    if (GdipDrawImageRectRectI(sprite.graphics, sheet, x, y, size, size,
                              left, top, right - left, bottom - top,
                              UnitPixel, NULL, NULL, NULL) != Ok) return 0;
    GdipFlush(sprite.graphics, FlushIntentionSync);
    ZeroMemory(sprite.pixels + count, (size_t)sprite.size.cx * (sprite.size.cy - clip_height) * sizeof(DWORD));
    remove_alpha_haze(count);
    return measure_sprite(&pose_clicks[index], &pose_bounds[index]);
}

static BOOL draw_atlas(GpImage *sheet, UINT width, UINT height, int first, int *idle_area)
{
    int index, begin, end = first + POSES_PER_SHEET, size = sprite.size.cx;
    int areas[POSES_PER_SHEET], height_limit = sprite.size.cy;
    int idle_reference = *idle_area;
    int safe_margin = px(4), floor = MulDiv(size, 88, 100);
    size_t count = (size_t)size * height_limit;
    if (width < 4 * 16 || height < 4 * 16 || width > 8192 || height > 8192) return FALSE;
    for (index = first; index < end; ++index) {
        areas[index - first] = render_cel(sheet, width, height, index, 0, 0, size, size);
        if (!areas[index - first]) return FALSE;
    }
    for (begin = first; begin < end;) {
        int stop = begin, edges[POSES_PER_SHEET], centers = 0;
        int span_x = 0, span_y = 0, scaled, center, area_total = 0;
        BOOL carried = begin == 8 || begin == INTERACTION_FIRST;
        int clip_height = carried ? height_limit : size;
        int bottom = carried ? clip_height - safe_margin : floor;
        // Use one scale for a whole clip. An ear flick or a pose that extends
        // a paw can change the silhouette without zooming the other drawings.
        for (; stop < end && pose_group(stop) == begin; ++stop) {
            RECT b = pose_bounds[stop];
            int w = b.right - b.left, h = b.bottom - b.top, j = stop - begin;
            int edge = max(w, h);
            while (j > 0 && edges[j - 1] > edge) {
                edges[j] = edges[j - 1];
                --j;
            }
            edges[j] = edge;
            centers += (b.left + b.right) / 2;
            area_total += areas[stop - first];
            span_x = max(span_x, w);
            span_y = max(span_y, h);
        }
        center = centers / (stop - begin);
        scaled = MulDiv(size, MulDiv(size, 84, 100), edges[(stop - begin) / 2]);
        // Dangling poses are longer and narrower. Match their visible ink to
        // idle instead of shrinking the whole character to match its height.
        if (carried)
            scaled = (int)(size * sqrt((double)idle_reference * (stop - begin) / area_total) + 0.5);
        scaled = clamp(scaled, MulDiv(size, 78, 100), MulDiv(size, 118, 100));
        scaled = min(scaled, (size - 2 * safe_margin) * size / span_x);
        scaled = min(scaled, (bottom - safe_margin) * size / span_y);
        for (index = begin; index < stop; ++index) {
            RECT b = pose_bounds[index];
            int baseline = floor;
            int x = size / 2 - MulDiv(center, scaled, size), y, area;
            if (index >= WALK_FIRST + 8 && index < WALK_FIRST + 12 && (index & 1))
                baseline -= px(6);
            x = clamp(x, safe_margin - MulDiv(b.left, scaled, size),
                      size - safe_margin - MulDiv(b.right, scaled, size));
            y = carried ? (size - scaled) / 2 : baseline - MulDiv(b.bottom, scaled, size);
            y = clamp(y, safe_margin - MulDiv(b.top, scaled, size),
                      clip_height - safe_margin - MulDiv(b.bottom, scaled, size));
            area = render_cel(sheet, width, height, index, x, y, scaled, clip_height);
            if (!area) return FALSE;
            if (index == 0 || index == IDLE_DETAIL_FIRST) idle_reference = area;
            if (!carried) align_floor(&pose_bounds[index], &pose_clicks[index], baseline);
            CopyMemory(pose_pixels + index * count, sprite.pixels, count * sizeof(DWORD));
        }
        begin = stop;
    }
    *idle_area = idle_reference;
    return TRUE;
}

// Keep the hint's click on the body through every frame it can display.
static BOOL stable_clip_hit(int first, int frames, const int *leading, int leading_count, POINT *hit)
{
    int w = sprite.size.cx, h = sprite.size.cy;
    int center_y = layout_height() * 5 / 8;
    size_t count = (size_t)w * h;
    int x, y, best = INT_MAX;
    for (y = 0; y < h; ++y) {
        for (x = 0; x < w; ++x) {
            int dx = x - w / 2, dy = y - center_y;
            int distance = dx * dx + dy * dy, pose;
            if (distance >= best) continue;
            if (leading && sprite.pixels[y * w + x] >> 24 < 160) continue;
            for (pose = first; pose < first + frames; ++pose)
                if (pose_pixels[pose * count + y * w + x] >> 24 < 160) break;
            if (pose == first + frames) {
                int i;
                for (i = 0; i < leading_count; ++i)
                    if (pose_pixels[leading[i] * count + y * w + x] >> 24 < 160) break;
                if (i != leading_count) continue;
                *hit = (POINT){x, y};
                best = distance;
            }
        }
    }
    return best != INT_MAX;
}

static BOOL draw_sprite(void)
{
    UINT w, h;
    int width, height;
    int size = px(144);

    free(pose_pixels);
    pose_pixels = NULL;
    blend_ms = 0;
    painted_pose = -1;
    if (GdipGetImageWidth(image, &w) != Ok || GdipGetImageHeight(image, &h) != Ok ||
        !w || !h || w > INT_MAX || h > INT_MAX)
        return FALSE;
    if (use_atlas) {
        int sheet, poses = POSES_PER_SHEET, idle_area = 0;
        size_t count;
        size = clamp(px(164), 64, MAX_ATLAS_SIZE);
        // Leave room for dangling poses below the normal square layout.
        if (!make_canvas(&sprite, size, size + size / 5)) return FALSE;
        count = (size_t)size * sprite.size.cy;
        for (sheet = 0; sheet < SECRETARY_EXTRA_SHEETS; ++sheet)
            if (extra_images[sheet]) poses = (sheet + 2) * POSES_PER_SHEET;
        pose_pixels = (DWORD *)malloc((size_t)poses * count * sizeof(DWORD));
        if (!pose_pixels || !draw_atlas(image, w, h, 0, &idle_area)) return FALSE;
        for (sheet = 0; sheet < SECRETARY_EXTRA_SHEETS; ++sheet) {
            GpImage *extra = extra_images[sheet];
            extra_ready[sheet] = extra && GdipGetImageWidth(extra, &w) == Ok &&
                GdipGetImageHeight(extra, &h) == Ok && draw_atlas(extra, w, h, (sheet + 1) * POSES_PER_SHEET, &idle_area);
            if (extra && !extra_ready[sheet])
                app_log(L"secretary: an extra animation sheet could not be drawn");
        }
        attention_safe = extra_ready[SECRETARY_ATTENTION] && stable_clip_hit(ATTENTION_FIRST, 16, NULL, 0, &attention_hit);
        sleeping_safe = extra_ready[SECRETARY_REST] && stable_clip_hit(REST_FIRST, 8, NULL, 0, &sleeping_hit);
        return TRUE;
    }
    width = w >= h ? size : clamp(MulDiv(size, (int)w, (int)h), 1, size);
    height = h >= w ? size : clamp(MulDiv(size, (int)h, (int)w), 1, size);
    if (!make_canvas(&sprite, width, height)) return FALSE;
    GdipGraphicsClear(sprite.graphics, 0);
    // TileFlipXY keeps resampling at a JPEG's outer edge fully opaque.
    {
        GpImageAttributes *attributes = NULL;
        if (GdipCreateImageAttributes(&attributes) == Ok)
            GdipSetImageAttributesWrapMode(attributes, WrapModeTileFlipXY, 0, FALSE);
        GdipDrawImageRectRectI(sprite.graphics, image, 0, 0, width, height,
                              0, 0, w, h, UnitPixel, attributes, NULL, NULL);
        if (attributes) GdipDisposeImageAttributes(attributes);
    }
    GdipFlush(sprite.graphics, FlushIntentionSync);
    clear_backdrop(&sprite, (RECT){0, 0, width, height});
    return measure_sprite(&click_pixel, &sprite_bounds) != 0;
}

static BOOL update_monitor(POINT point)
{
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {sizeof(info), {0}, {0}, 0};
    UINT new_dpi = dpi_for_monitor(monitor);

    if (GetMonitorInfoW(monitor, &info)) work = info.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (new_dpi == dpi && sprite.dib) return TRUE;
    dpi = new_dpi;
    return draw_sprite();
}

static void clamp_anchor(void)
{
    int margin = px(10);
    anchor.x = clamp(anchor.x, work.left + margin, work.right - sprite.size.cx - margin);
    anchor.y = clamp(anchor.y, work.top + margin, work.bottom - layout_height() - margin);
}

static void store_position(void)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, POSITION_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(key, L"Position", 0, REG_BINARY, (BYTE *)&anchor, sizeof(anchor));
        RegCloseKey(key);
    }
}

static void rounded(GpGraphics *g, GpSolidFill *brush, RECT r, int radius, ARGB colour)
{
    GpPath *path;
    int d = radius * 2;
    if (GdipCreatePath(FillModeAlternate, &path) != Ok) return;
    GdipAddPathArcI(path, r.left, r.top, d, d, 180, 90);
    GdipAddPathArcI(path, r.right - d, r.top, d, d, 270, 90);
    GdipAddPathArcI(path, r.right - d, r.bottom - d, d, d, 0, 90);
    GdipAddPathArcI(path, r.left, r.bottom - d, d, d, 90, 90);
    GdipClosePathFigure(path);
    GdipSetSolidFillColor(brush, colour);
    GdipFillPath(g, (GpBrush *)brush, path);
    GdipDeletePath(path);
}

static void write_text(GpSolidFill *brush, GpFont *font, GpStringFormat *format,
                       const WCHAR *text, int x, int y, int width, int height, ARGB colour)
{
    RectF rect = {(REAL)px(x), (REAL)px(y), (REAL)px(width), (REAL)px(height)};
    GdipSetSolidFillColor(brush, colour);
    GdipDrawString(speech.graphics, text, -1, font, &rect, format, (GpBrush *)brush);
}

static int actions_y(void) { return 49 + 37 * (row_count ? row_count : 1); }
static int footer_y(void) { return actions_y() + 109; }
static int bubble_height(void) { return footer_y() + 33; }

static RECT action_rect(int action)
{
    int x = (bubble_left ? 16 : 30) + (action % 2) * 106;
    int y = actions_y() + (action / 2) * 34;
    return (RECT){x, y, x + (action == BUBBLE_PROMPT ? 206 : 100), y + 29};
}

static void bubble_focus_box(GpGraphics *g, GpSolidFill *brush, RECT r, BOOL focused, ARGB fill)
{
    RECT box = {px(r.left), px(r.top), px(r.right), px(r.bottom)};
    rounded(g, brush, box, px(8), focused ? 0xff7149a4 : fill);
    if (focused) {
        InflateRect(&box, -px(2), -px(2));
        rounded(g, brush, box, px(6), fill);
    }
}

static void draw_bubble(void)
{
    GpGraphics *g = speech.graphics;
    GpSolidFill *brush = NULL;
    GpFontFamily *family = NULL;
    GpFont *regular = NULL, *bold = NULL, *footer_font = NULL;
    GpStringFormat *format = NULL;
    int shift = bubble_left ? 0 : 14;
    int x = 6 + shift, right = x + 226, bottom = bubble_height() - 8;
    int tip_x = bubble_left ? right + 11 : x - 11;
    int tail_y = clamp(MulDiv(pet_at.y + layout_height() / 2 - bubble_at.y,
                             96, dpi), 24, bottom - 18);
    int edge = bubble_left ? right - 2 : x + 2;
    GpPoint tail[3] = {{px(edge), px(tail_y - 8)}, {px(tip_x), px(tail_y)},
                      {px(edge), px(tail_y + 8)}};
    int i;

    bubble_dirty = TRUE;
    GdipGraphicsClear(g, 0);
    if (GdipCreateSolidFill(0xfffdfaff, &brush) != Ok) return;
    rounded(g, brush, (RECT){px(x), px(7), px(right), px(bottom + 5)}, px(18), 0x503a2354);
    rounded(g, brush, (RECT){px(x), px(3), px(right), px(bottom)}, px(18), 0xffd8c9ea);
    rounded(g, brush, (RECT){px(x + 1), px(4), px(right - 1), px(bottom - 1)}, px(17), 0xfffdfaff);
    GdipSetSolidFillColor(brush, 0xfffdfaff);
    GdipFillPolygonI(g, (GpBrush *)brush, tail, 3, FillModeAlternate);
    if (GdipCreateFontFamilyFromName(L"Malgun Gothic", NULL, &family) != Ok &&
        GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &family) != Ok)
        goto done;
    if (GdipCreateFont(family, (REAL)px(14), FontStyleRegular, UnitPixel, &regular) != Ok ||
        GdipCreateFont(family, (REAL)px(15), FontStyleBold, UnitPixel, &bold) != Ok ||
        GdipCreateFont(family, (REAL)px(12), FontStyleRegular, UnitPixel, &footer_font) != Ok ||
        GdipCreateStringFormat(StringFormatFlagsNoWrap, 0, &format) != Ok)
        goto done;
    GdipSetStringFormatTrimming(format, StringTrimmingEllipsisCharacter);
    write_text(brush, bold, format, L"\xbc14\xb85c\xac00\xae30", x + 13, 16, 190, 25, 0xff302040);
    for (i = 0; i < row_count; ++i) {
        int slot = rows[i], y = 42 + 37 * i;
        WCHAR key[] = {(WCHAR)(L'1' + slot), 0};
        bubble_focus_box(g, brush, (RECT){x + 10, y, right - 10, y + 33},
                         i == focus_item, slot == selected ? 0xffded0f7 : 0xfff1eaf9);
        rounded(g, brush, (RECT){px(x + 16), px(y + 5), px(x + 39), px(y + 28)},
                px(6), slot == selected ? 0xff57368b : 0xff7149a4);
        write_text(brush, bold, format, key, x + 22, y + 7, 16, 22, 0xffffffff);
        write_text(brush, regular, format, g_cfg.secretary_shortcuts[slot].label,
                   x + 48, y + 7, 160, 22, 0xff362447);
    }
    if (!row_count)
        write_text(brush, regular, format, L"\xc124\xc815\xc5d0\xc11c \xbc14\xb85c\xac00\xae30\xb97c \xcd94\xac00\xd558\xc138\xc694",
                   x + 13, 50, 200, 24, 0xff554765);
    for (i = 0; i < BUBBLE_ACTIONS; ++i) {
        const WCHAR *labels[] = {
            L"바로가기 편집", GetSystemMetrics(SM_CMONITORS) > 1 ? L"다음 화면" : L"Vim 힌트",
            g_cfg.secretary_roam ? L"이동 멈춤" : L"자유 이동", L"제자리로", L"Claude 질문 준비"
        };
        RECT box = action_rect(i);
        bubble_focus_box(g, brush, box, row_count + i == focus_item, 0xffece1f6);
        write_text(brush, footer_font, format, labels[i], box.left + 7, box.top + 6,
                   box.right - box.left - 7, 20, 0xff47305c);
    }
    write_text(brush, footer_font, format, L"Tab 이동   Enter 선택   Esc 닫기",
               x + 13, footer_y(), 205, 18, 0xff685678);
done:
    if (format) GdipDeleteStringFormat(format);
    if (footer_font) GdipDeleteFont(footer_font);
    if (bold) GdipDeleteFont(bold);
    if (regular) GdipDeleteFont(regular);
    if (family) GdipDeleteFontFamily(family);
    GdipDeleteBrush((GpBrush *)brush);
    GdipFlush(g, FlushIntentionSync);
}

static void render_bubble(DWORD now)
{
    POINT at = bubble_at;
    unsigned elapsed = now - bubble_tick;
    BYTE opacity = 255;
    if (!IsWindowVisible(bubble)) return;
    if (!opened && (!animate || elapsed >= BUBBLE_MS)) {
        ShowWindow(bubble, SW_HIDE);
        return;
    }
    if (animate && elapsed < BUBBLE_MS) {
        int progress = (int)(elapsed * 255 / BUBBLE_MS);
        opacity = opened ? (BYTE)(75 + progress * 180 / 255) : (BYTE)(255 - progress);
        at.x += (bubble_left ? 1 : -1) * px(8) * (255 - opacity) / 255;
    }
    if (bubble_dirty || bubble_opacity != opacity ||
        bubble_painted_at.x != at.x || bubble_painted_at.y != at.y) {
        present(bubble, &speech, at, opacity);
        bubble_dirty = FALSE;
        bubble_opacity = opacity;
        bubble_painted_at = at;
    }
}

void secretary_dismiss(void)
{
    if (!opened) return;
    opened = FALSE;
    armed_key = 0;
    bubble_tick = animation_now();
    if (!animate) ShowWindow(bubble, SW_HIDE);
}

static void open_bubble(void)
{
    int i, width, height, left = pet_at.x + sprite_bounds.left;
    int right = pet_at.x + sprite_bounds.right;
    if (input_is_locked() || hint_is_open() || pending_slot >= 0) return;
    row_count = 0;
    for (i = 0; i < SECRETARY_SLOTS; ++i)
        if (g_cfg.secretary_shortcuts[i].target[0]) rows[row_count++] = i;
    width = px(252);
    height = px(bubble_height());
    if (!make_canvas(&speech, width, height)) return;
    bubble_left = left - width + px(10) >= work.left ||
                  pet_at.x > (work.left + work.right) / 2;
    bubble_at.x = bubble_left ? left - width + px(12) : right - px(12);
    bubble_at.x = clamp(bubble_at.x, work.left, work.right - width);
    bubble_at.y = clamp(pet_at.y + layout_height() / 2 - px(45), work.top, work.bottom - height);
    selected = -1;
    focus_item = 0;
    draw_bubble();
    opened = TRUE;
    bubble_tick = animation_now();
    present(bubble, &speech, bubble_at, animate ? 75 : 255);
    // A button handled by the mouse hook never reaches this thread as
    // native input, so it may need to borrow the foreground input queue.
    activating_bubble = TRUE;
    ShowWindow(bubble, SW_SHOW);
    if (!SetForegroundWindow(bubble)) {
        DWORD current = GetCurrentThreadId();
        DWORD foreground = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
        if (foreground && foreground != current && AttachThreadInput(current, foreground, TRUE)) {
            SetForegroundWindow(bubble);
            AttachThreadInput(current, foreground, FALSE);
        }
    }
    activating_bubble = FALSE;
}

static void react(int action)
{
    queued_reaction = action;
    last_activity = pulse = animation_now();
    pet_motion.next = pulse + 3000;
    if (pet_motion.state == PET_NAP) {
        pet_motion.state = PET_STRETCH;
        pet_motion.started = pulse;
    }
}

typedef struct { HMONITOR handle; RECT area; } PetMonitor;
typedef struct { PetMonitor items[16]; int count; } PetMonitors;

static BOOL CALLBACK collect_monitors(HMONITOR monitor, HDC dc, LPRECT bounds, LPARAM param)
{
    PetMonitors *list = (PetMonitors *)param;
    MONITORINFO info = {sizeof(info), {0}, {0}, 0};
    (void)dc;
    (void)bounds;
    if (GetMonitorInfoW(monitor, &info))
        list->items[list->count++] = (PetMonitor){monitor, info.rcWork};
    return list->count < (int)(sizeof(list->items) / sizeof(list->items[0]));
}

static int monitor_order(const void *a, const void *b)
{
    const PetMonitor *ma = (const PetMonitor *)a, *mb = (const PetMonitor *)b;
    if (ma->area.left != mb->area.left) return ma->area.left < mb->area.left ? -1 : 1;
    return (ma->area.top > mb->area.top) - (ma->area.top < mb->area.top);
}

static BOOL next_screen(void)
{
    PetMonitors list = {0};
    HMONITOR current = MonitorFromWindow(pet, MONITOR_DEFAULTTONEAREST);
    RECT area;
    POINT center;
    int i;
    EnumDisplayMonitors(NULL, NULL, collect_monitors, (LPARAM)&list);
    if (list.count < 2) return FALSE;
    qsort(list.items, (size_t)list.count, sizeof(list.items[0]), monitor_order);
    for (i = 0; i < list.count; ++i)
        if (list.items[i].handle == current) break;
    area = list.items[(i + 1) % list.count].area;
    center = (POINT){(area.left + area.right) / 2, (area.top + area.bottom) / 2};
    if (!update_monitor(center)) return FALSE;
    anchor.x = area.right - sprite.size.cx - px(24);
    anchor.y = area.bottom - layout_height() - px(24);
    clamp_anchor();
    home = anchor;
    moved = TRUE;
    store_position();
    return TRUE;
}

static void run_bubble_action(int action)
{
    secretary_dismiss();
    react(REACT_CLICK);
    switch (action) {
    case BUBBLE_EDIT:
        secretary_editor_open();
        break;
    case BUBBLE_SCREEN:
        if (!next_screen()) PostMessageW(g_main_wnd, WM_APP_HINT_OPEN, 0, 0);
        break;
    case BUBBLE_ROAM:
        if (secretary_save_setting(-1, NULL, !g_cfg.secretary_roam)) app_config_written();
        else tray_balloon(L"Desktop secretary", L"Could not save the roaming setting.");
        break;
    case BUBBLE_HOME:
        anchor = home;
        update_monitor(anchor);
        clamp_anchor();
        break;
    case BUBBLE_PROMPT:
        secretary_prompt_open();
        break;
    }
}

static void select_slot(int slot)
{
    if (!opened || slot < 0 || slot >= SECRETARY_SLOTS ||
        !g_cfg.secretary_shortcuts[slot].target[0] || pending_slot >= 0) return;
    selected = slot;
    draw_bubble();
    pending_slot = slot;
    react(REACT_CLICK);
    secretary_dismiss();
}

static BOOL app_window(HWND wnd, const WCHAR *exe)
{
    WCHAR cls[64], path[MAX_PATH];
    DWORD pid, len = MAX_PATH;
    HANDLE process;
    BOOL match = FALSE;

    if (!process_path || !wnd || !IsWindowVisible(wnd) || GetWindow(wnd, GW_OWNER) ||
        !GetWindowTextLengthW(wnd) || !GetClassNameW(wnd, cls, 64) ||
        wcscmp(cls, L"Chrome_WidgetWin_1") != 0) return FALSE;
    GetWindowThreadProcessId(wnd, &pid);
    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        if (process_path(process, 0, path, &len)) {
            const WCHAR *name = wcsrchr(path, L'\\');
            match = name && _wcsicmp(name + 1, exe) == 0;
        }
        CloseHandle(process);
    }
    return match;
}

static BOOL CALLBACK find_browser_window(HWND wnd, LPARAM lp)
{
    if (app_window(wnd, launch.exe)) {
        *(HWND *)lp = wnd;
        return FALSE;
    }
    return TRUE;
}

static BOOL in_fullscreen(HWND wnd)
{
    MONITORINFO info = {sizeof(info), {0}, {0}, 0};
    RECT r;
    if (!GetWindowRect(wnd, &r) ||
        !GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &info)) return FALSE;
    // A maximized Chromium window extends a few pixels past the work area.
    return EqualRect(&r, &info.rcMonitor);
}

static void launch_error(const WCHAR *message)
{
    uia_cancel_browser_tab();
    launch.exe = NULL;
    launch.lookup = 0;
    app_log(L"secretary: %ls", message);
    tray_balloon(L"Desktop secretary", message);
}

static BOOL read_registry(HKEY root, const WCHAR *subkey, const WCHAR *value,
                          DWORD expected, void *data, DWORD *size)
{
    HKEY key;
    DWORD type;
    LONG result;
    if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return FALSE;
    result = RegQueryValueExW(key, value, NULL, &type, data, size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && type == expected;
}

static BOOL browser_path(const WCHAR *exe, WCHAR *path)
{
    WCHAR key[128];
    _snwprintf(key, 128, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%ls", exe);
    path[MAX_PATH - 1] = 0;
    return read_registry(HKEY_CURRENT_USER, key, NULL, REG_SZ, path,
                         &(DWORD){(MAX_PATH - 1) * sizeof(WCHAR)}) ||
           read_registry(HKEY_LOCAL_MACHINE, key, NULL, REG_SZ, path,
                         &(DWORD){(MAX_PATH - 1) * sizeof(WCHAR)});
}

static BOOL send_fullscreen_key(void)
{
    INPUT keys[2] = {{0}, {0}};
    keys[0].type = keys[1].type = INPUT_KEYBOARD;
    keys[0].ki.wVk = keys[1].ki.wVk = VK_F11;
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(2, keys, sizeof(INPUT)) == 2;
}

BOOL secretary_modifiers_down(void)
{
    return ((GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_CONTROL) |
             GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_LWIN) |
             GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
}

static void open_browser_url(DWORD now)
{
    WCHAR command[SECRETARY_TARGET_MAX + 32];
    HINSTANCE result;
    uia_cancel_browser_tab();
    launch.lookup = 0;
    launch.target = NULL;
    launch.began = now;
    launch.sent = 0;
    launch.maximizing = FALSE;
    app_log(L"secretary: opening a browser tab");
    _snwprintf(command, sizeof(command) / sizeof(*command), L"\"%ls\"", launch.url);
    result = ShellExecuteW(NULL, L"open", launch.browser_path, command, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) launch_error(L"Windows could not open this shortcut.");
}

static void start_target(const WCHAR *target, DWORD now)
{
    HWND foreground = GetForegroundWindow(), existing = NULL;
    HINSTANCE result;

    uia_cancel_browser_tab();
    ZeroMemory(&launch, sizeof(launch));
    launch.began = now;
    if (_wcsicmp(target, L"claude") == 0) {
        launch.exe = L"claude.exe";
        if (app_window(foreground, launch.exe)) return;
        result = ShellExecuteW(NULL, L"open", L"shell:AppsFolder\\Claude_pzs8sxrjxfjjc!Claude",
                               NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)result <= 32) launch_error(L"Windows could not open Claude.");
        return;
    }
    launch.exe = app_window(foreground, L"msedge.exe") ? L"msedge.exe" : L"chrome.exe";
    if (!browser_path(launch.exe, launch.browser_path)) {
        launch.exe = L"msedge.exe";
        if (!browser_path(launch.exe, launch.browser_path)) {
            launch_error(L"Install Chrome or Edge for web shortcuts.");
            return;
        }
    }
    lstrcpynW(launch.url, target, SECRETARY_TARGET_MAX);
    if (app_window(foreground, launch.exe)) existing = foreground;
    else EnumWindows(find_browser_window, (LPARAM)&existing);
    if (!existing && _wcsicmp(launch.exe, L"chrome.exe") == 0) {
        launch.exe = L"msedge.exe";
        EnumWindows(find_browser_window, (LPARAM)&existing);
        if (!existing || !browser_path(launch.exe, launch.browser_path)) {
            launch.exe = L"chrome.exe";
            browser_path(launch.exe, launch.browser_path);
            existing = NULL;
        }
    }
    if (!existing) {
        open_browser_url(now);
        return;
    }
    if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
    SetForegroundWindow(existing);
    launch.target = existing;
    launch.lookup = 1;
}

static void start_shortcut(int slot, DWORD now)
{
    start_target(g_cfg.secretary_shortcuts[slot].target, now);
}

void secretary_open_claude(void) { start_target(L"claude", animation_now()); }

void secretary_browser_result(UINT request, HWND browser)
{
    DWORD now = animation_now();
    if (launch.lookup != 2 || request != launch.request) return;
    if (!app_window(browser, launch.exe)) {
        open_browser_url(now);
        return;
    }
    launch.target = browser;
    app_log(L"secretary: reused a browser tab");
    launch.lookup = 0;
    launch.began = now;
    launch.sent = 0;
    launch.maximizing = FALSE;
}

static void finish_launch(DWORD now)
{
    HWND foreground;
    BOOL web = launch.url[0] != 0;
    if (!launch.exe) return;
    foreground = GetForegroundWindow();
    if (launch.lookup) {
        if (now - launch.began >= (launch.lookup == 1 ? 2500u : 4200u)) {
            open_browser_url(now);
        } else if (launch.lookup == 1 && foreground == launch.target && !secretary_modifiers_down()) {
            if (in_fullscreen(foreground)) {
                if (!launch.sent && send_fullscreen_key()) launch.sent = now;
            } else if (!IsZoomed(foreground)) {
                if (!launch.maximizing) launch.maximizing = ShowWindowAsync(foreground, SW_MAXIMIZE);
            } else if (!launch.browser_ready) {
                launch.browser_ready = now;
            } else if (now - launch.browser_ready >= 350) {
                launch.request = ++next_browser_request;
                if (uia_find_browser_tab(foreground, launch.url, launch.request)) {
                    launch.lookup = 2;
                    launch.began = now;
                } else open_browser_url(now);
            }
        }
        return;
    }
    if (now - launch.began >= 10000 || (launch.sent && now - launch.sent >= 2500)) {
        launch_error(web ? L"The browser window did not maximize."
                         : L"The shortcut did not enter full screen. Try F11 in the opened window.");
        return;
    }
    if (!launch.target) {
        if (!app_window(foreground, launch.exe)) return;
        launch.target = foreground;
    }
    if (web) {
        if (foreground != launch.target || now - launch.began < 250) return;
        if (in_fullscreen(launch.target)) {
            // F11 here only leaves a full-screen mode the browser was already in.
            if (!launch.sent && !secretary_modifiers_down() && send_fullscreen_key()) launch.sent = now;
        } else if (IsZoomed(launch.target)) {
            launch.exe = NULL;
        } else if (!launch.maximizing) {
            launch.maximizing = ShowWindowAsync(launch.target, SW_MAXIMIZE);
        }
        return;
    }
    if (in_fullscreen(launch.target)) {
        launch.exe = NULL;
        return;
    }
    if (launch.sent || now - launch.began < 250 || foreground != launch.target ||
        secretary_modifiers_down()) return;
    if (send_fullscreen_key()) launch.sent = now;
}

static void move_from_press(POINT pointer)
{
    if (!dragging && (abs(pointer.x - press.x) >= px(5) || abs(pointer.y - press.y) >= px(5))) {
        dragging = TRUE;
        reaction = queued_reaction = REACT_NONE;
        command_pose = queued_command = 0;
        secretary_dismiss();
    }
    if (dragging) {
        int height = use_atlas && animate ? sprite.size.cy : layout_height();
        anchor.x = press_at.x + pointer.x - press.x;
        anchor.y = press_at.y + pointer.y - press.y;
        anchor.x = clamp(anchor.x, GetSystemMetrics(SM_XVIRTUALSCREEN),
                         GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) - sprite.size.cx);
        anchor.y = clamp(anchor.y, GetSystemMetrics(SM_YVIRTUALSCREEN),
                         GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) - height);
    }
}

static void press_pet(POINT point, BOOL routed)
{
    press = point;
    pressed = TRUE;
    routed_press = routed;
    last_activity = press_started = animation_now();
    press_at = pet_at;
    if (!routed) SetCapture(pet);
}

static void release_pet(POINT pointer)
{
    if (!pressed) return;
    move_from_press(pointer);
    pressed = routed_press = FALSE;
    if (GetCapture() == pet) ReleaseCapture();
    if (dragging) {
        dragging = FALSE;
        update_monitor(pointer);
        clamp_anchor();
        moved = TRUE;
        home = anchor;
        store_position();
        react(REACT_LAND);
    } else {
        react(REACT_CLICK);
        if (opened) secretary_dismiss();
        else open_bubble();
    }
}

static int step_at(const PoseStep *steps, size_t count, DWORD elapsed, BOOL loop)
{
    size_t i;
    if (loop) {
        DWORD duration = 0;
        for (i = 0; i < count; ++i) duration += steps[i].duration_ms;
        elapsed %= duration;
    }
    for (i = 0; i < count; ++i) {
        if (elapsed < steps[i].duration_ms) return steps[i].pose;
        elapsed -= steps[i].duration_ms;
    }
    return -1;
}

#define CLIP(steps) (steps), (sizeof(steps) / sizeof((steps)[0]))

static unsigned random_below(unsigned limit)
{
    DWORD n = pet_motion.seed;
    n ^= n << 13;
    n ^= n >> 17;
    n ^= n << 5;
    pet_motion.seed = n;
    return n % limit;
}

static void wait_to_roam(DWORD now, DWORD delay)
{
    pet_motion.state = PET_WAIT;
    pet_motion.next = now + delay;
}

static void start_walk(DWORD now)
{
    int margin = px(17), distance = px(105 + random_below(270));
    int left = work.left + margin, right = work.right - sprite.size.cx - margin;
    int top = work.top + (work.bottom - work.top) * 2 / 5;
    int bottom = work.bottom - layout_height() - margin;
    int direction = random_below(2) ? 1 : -1;
    int x = clamp(anchor.x + direction * distance, left, right);
    BOOL stay_low = random_below(3) != 0;
    int y_drift = (int)random_below(stay_low ? 111 : 271) - (stay_low ? 55 : 135);

    if (abs(x - anchor.x) < px(55)) x = clamp(anchor.x - direction * distance, left, right);
    pet_motion.goal = (POINT){x, clamp(anchor.y + px(y_drift), top, bottom)};
    pet_motion.origin = anchor;
    pet_motion.distance = hypot(pet_motion.goal.x - anchor.x, pet_motion.goal.y - anchor.y);
    pet_motion.stop = pet_motion.distance;
    pet_motion.curve = px((int)random_below(49) - 24);
    pet_motion.left = x < anchor.x;
    pet_motion.fast = abs(x - anchor.x) > px(200) && random_below(8) == 0;
    pet_motion.cruise = px(pet_motion.fast ? 68 + random_below(17) : 37 + random_below(19));
    pet_motion.speed = pet_motion.gait = pet_motion.traveled = 0.0;
    pet_motion.state = PET_WALK;
    pet_motion.started = now;
}

static void advance_walk(DWORD now, double seconds, BOOL stopping)
{
    double acceleration = px(pet_motion.fast ? 115 : 80);
    double remaining, goal_speed, step, progress;
    if (stopping && pet_motion.stop == pet_motion.distance)
        pet_motion.stop = fmin(pet_motion.distance, pet_motion.traveled +
                              pet_motion.speed * pet_motion.speed / (2.0 * acceleration) + px(4));
    remaining = pet_motion.stop - pet_motion.traveled;
    goal_speed = fmin(pet_motion.cruise, sqrt(2.0 * acceleration * remaining));

    if (pet_motion.speed < goal_speed)
        pet_motion.speed = fmin(goal_speed, pet_motion.speed + acceleration * seconds);
    else
        pet_motion.speed = fmax(goal_speed, pet_motion.speed - acceleration * seconds);
    step = fmin(remaining, pet_motion.speed * seconds);
    if (remaining <= 1.0 || step >= remaining) {
        step = remaining;
        pet_motion.state = PET_SETTLE;
        pet_motion.started = now;
    }
    pet_motion.traveled += step;
    pet_motion.gait += step / px(extra_ready[SECRETARY_WALK_DETAIL] ? (pet_motion.fast ? 5 : 4) : 7);
    progress = pet_motion.distance > 0 ? pet_motion.traveled / pet_motion.distance : 1.0;
    anchor.x = pet_motion.origin.x + (LONG)lround((pet_motion.goal.x - pet_motion.origin.x) * progress);
    anchor.y = pet_motion.origin.y + (LONG)lround((pet_motion.goal.y - pet_motion.origin.y) * progress +
                                                 pet_motion.curve * sin(progress * 3.14159265));
    clamp_anchor();
}

static void advance_pet(DWORD now, POINT pointer)
{
    double seconds = clamp((int)(now - pet_motion.last_frame), 0, 60) / 1000.0;
    DWORD elapsed = now - pet_motion.started;
    RECT hover_zone = {pet_at.x, pet_at.y, pet_at.x + sprite.size.cx, pet_at.y + layout_height()};
    BOOL hovered;
    unsigned choice;

    pet_motion.last_frame = now;
    InflateRect(&hover_zone, px(25), px(19));
    hovered = PtInRect(&hover_zone, pointer);
    if (!animate || !use_atlas || (input_is_locked() &&
        pet_motion.state != PET_NAP && pet_motion.state != PET_STRETCH)) {
        wait_to_roam(now, 3000);
        return;
    }
    switch (pet_motion.state) {
    case PET_WALK:
        if (!pressed && !opened && g_cfg.secretary_roam)
            advance_walk(now, seconds, hovered || queued_command || queued_reaction);
        else {
            pet_motion.state = PET_SETTLE;
            pet_motion.started = now;
        }
        return;
    case PET_SETTLE:
        if (elapsed >= 545) wait_to_roam(now, 4000 + random_below(6500));
        return;
    case PET_NAP:
        if (elapsed >= pet_motion.duration) {
            pet_motion.started = now;
            pet_motion.state = PET_STRETCH;
        }
        return;
    case PET_STRETCH:
        if (elapsed >= 1205) wait_to_roam(now, 3800 + random_below(3200));
        return;
    case PET_GROOM:
        if (elapsed >= 1215) wait_to_roam(now, 4300 + random_below(5500));
        return;
    case PET_YAWN:
        if (elapsed >= (extra_ready[SECRETARY_IDLE_DETAIL] ? 1990 : 2375))
            wait_to_roam(now, 3200 + random_below(4500));
        return;
    }
    if (pressed || opened || secretary_editor_visible() || secretary_prompt_has_focus() ||
        reaction || queued_reaction || command_pose || queued_command || hovered) {
        if ((LONG)(now - pet_motion.next) >= 0) pet_motion.next = now + 1500;
        return;
    }
    if (now - last_activity >= 14000 && now - last_yawn >= 27000) {
        pet_motion.state = PET_YAWN;
        pet_motion.started = last_yawn = now;
        return;
    }
    if ((LONG)(now - pet_motion.next) < 0) return;
    choice = random_below(100);
    if (g_cfg.secretary_roam && extra_ready[SECRETARY_MOVEMENT] && (choice < 68 || now - born < 6000)) {
        start_walk(now);
    } else if (extra_ready[SECRETARY_REST]) {
        pet_motion.started = now;
        if (choice < 83) pet_motion.state = PET_GROOM;
        else {
            pet_motion.state = PET_NAP;
            pet_motion.duration = 8500 + random_below(11500);
            command_pose = queued_command = 0;
        }
    } else {
        wait_to_roam(now, 2700 + random_below(3000));
    }
}

void secretary_command(int command)
{
    // Hooks and worker threads leave a single event. Nothing here touches a window.
    if (g_cfg.secretary_enabled && g_cfg.secretary_react)
        InterlockedExchange(&pending_command, command);
}

static void receive_command(DWORD now)
{
    int command = (int)InterlockedExchange(&pending_command, 0);
    if (!g_cfg.secretary_react) command_pose = queued_command = 0;
    if (!animate || !use_atlas || !g_cfg.secretary_react || pressed || pet_motion.state == PET_NAP) return;
    if (!command) command = input_secretary_motion();
    if (!command) return;
    if (command != command_pose) queued_command = command;
    last_activity = now;
    pet_motion.next = now + 2400;
}

static int command_frame(DWORD now)
{
    DWORD elapsed = now - command_started;
    int row, col;
    static const BYTE down[] = {0, 2, 4, 5, 6, 6, 8, 10, 13, 15};
    static const BYTE up[] = {0, 1, 2, 3, 4, 5, 7, 10, 13, 15};
    if (!command_pose || !g_cfg.secretary_react || pet_motion.state == PET_NAP) return -1;
    if (command_pose == SECRETARY_SCROLL || command_pose == SECRETARY_SCROLL_UP || command_pose == SECRETARY_SCROLL_DOWN) {
        DWORD cycle = extra_ready[SECRETARY_SCROLL_DETAIL] ? 8 * 110 : 10 * 100;
        BOOL upward = command_pose == SECRETARY_SCROLL_UP;
        if (elapsed >= cycle) return -1;
        if (extra_ready[SECRETARY_SCROLL_DETAIL])
            return SCROLL_DETAIL_FIRST + (upward ? 8 : 0) + (int)(elapsed / 110 % 8);
        if (extra_ready[SECRETARY_ATTENTION]) {
            col = (int)(elapsed / 100 % 10);
            if (!upward && extra_ready[SECRETARY_REACTIONS] && col >= 3 && col <= 5)
                return COMMAND_FIRST + down[col];
            return ATTENTION_FIRST + (upward ? up[col] : down[col]);
        }
    }
    if (extra_ready[SECRETARY_ATTENTION] && (command_pose == SECRETARY_MOVE || command_pose == SECRETARY_HINT)) {
        if (elapsed >= 16 * 108) return -1;
        return ATTENTION_FIRST + (int)(elapsed / 108);
    }
    if (elapsed >= 600) return -1;
    if (!extra_ready[SECRETARY_REACTIONS]) return 12 + (int)(elapsed / 150);
    switch (command_pose) {
    case SECRETARY_MOVE:
    case SECRETARY_HINT: row = 0; break;
    case SECRETARY_SCROLL:
    case SECRETARY_SCROLL_UP:
    case SECRETARY_SCROLL_DOWN: row = 1; break;
    case SECRETARY_CLICK: row = 2; break;
    default: row = 3; break;
    }
    col = (int)(elapsed / 150);
    return COMMAND_FIRST + row * POSES_PER_ROW + col;
}

static int reaction_frame(DWORD now)
{
    DWORD elapsed = now - reaction_started;
    BOOL interactions = extra_ready[SECRETARY_INTERACTION_DETAIL];
    switch (reaction) {
    case REACT_CLICK:
        return interactions ? step_at(CLIP(click_detail), elapsed, FALSE)
                            : step_at(CLIP(clicked), elapsed, FALSE);
    case REACT_MENU: return step_at(CLIP(menu_reaction), elapsed, FALSE);
    case REACT_LAND:
        return interactions ? step_at(CLIP(landing_detail), elapsed, FALSE)
                            : step_at(CLIP(landed), elapsed, FALSE);
    }
    return -1;
}

static int animated_pose(DWORD now)
{
    DWORD elapsed = now - pet_motion.started;
    int pose = -1;
    BOOL detailed_idle = extra_ready[SECRETARY_IDLE_DETAIL];

    if (!animate) {
        reaction = queued_reaction = REACT_NONE;
        command_pose = queued_command = 0;
        return 0;
    }
    if (dragging || (pressed && now - press_started >= 160)) {
        elapsed = now - press_started - 160;
        return extra_ready[SECRETARY_INTERACTION_DETAIL] ? step_at(CLIP(grab_detail), elapsed, TRUE)
                                                        : step_at(CLIP(grabbed), elapsed, TRUE);
    }
    pose = reaction_frame(now);
    if (pose >= 0) return pose;
    reaction = REACT_NONE;
    pose = command_frame(now);
    if (pose >= 0) return pose;
    if (command_pose && !queued_command && command_pose == input_secretary_motion())
        queued_command = command_pose;
    command_pose = 0;
    switch (pet_motion.state) {
    case PET_WALK:
        if (extra_ready[SECRETARY_WALK_DETAIL]) return WALK_DETAIL_FIRST + (int)pet_motion.gait % 16;
        return WALK_FIRST + (pet_motion.fast ? 8 + (int)pet_motion.gait % 4 : (int)pet_motion.gait % 8);
    case PET_SETTLE:
        pose = step_at(CLIP(settling), elapsed, FALSE);
        break;
    case PET_NAP:
        pose = step_at(CLIP(falling_asleep), elapsed, FALSE);
        if (pose < 0) pose = step_at(CLIP(sleeping), elapsed - 1440, TRUE);
        break;
    case PET_STRETCH:
        pose = step_at(CLIP(stretching), elapsed, FALSE);
        break;
    case PET_GROOM:
        pose = step_at(CLIP(grooming), elapsed, FALSE);
        break;
    case PET_YAWN:
        pose = detailed_idle ? step_at(CLIP(yawn_detail), elapsed, FALSE)
                             : step_at(CLIP(bored), elapsed, FALSE);
        break;
    }
    if (pose >= 0) return pose;
    if (queued_reaction) {
        reaction = queued_reaction;
        queued_reaction = REACT_NONE;
        reaction_started = now;
        return reaction_frame(now);
    }
    if (queued_command && g_cfg.secretary_react) {
        command_pose = queued_command;
        queued_command = 0;
        command_started = now;
        return command_frame(now);
    }
    elapsed = now - born;
    return detailed_idle ? step_at(CLIP(breathing_detail), elapsed, TRUE)
                         : step_at(CLIP(idle), elapsed, TRUE);
}

static int hint_pose(DWORD now)
{
    if (hint_waiting) {
        int pose = reaction_frame(now);
        if (pose < 0) pose = command_frame(now);
        if (pose >= 0) return pose;
        reaction = REACT_NONE;
        command_pose = 0;
        hint_waiting = FALSE;
        attention_started = now;
    }
    return ATTENTION_FIRST + (int)((now - attention_started) / 108 % 16);
}

// Match the silhouette and shading around a small grid of points. Moving
// the outlines before blending keeps a flicking ear or paw from appearing twice.
static void match_motion(const DWORD *from, const DWORD *to, int w, int h)
{
    int reach = clamp(w / 24, 3, 14), patch = max(2, reach / 2);
    int gx, gy;
    for (gy = 0; gy < MOTION_GRID; ++gy) {
        for (gx = 0; gx < MOTION_GRID; ++gx) {
            POINT best = {0, 0};
            int x = gx * (w - 1) / (MOTION_GRID - 1);
            int y = gy * (h - 1) / (MOTION_GRID - 1);
            int best_error = INT_MAX, dx, dy;
            if (x < reach + patch || x >= w - reach - patch ||
                y < reach + patch || y >= h - reach - patch) {
                motion[gy][gx] = best;
                continue;
            }
            for (dy = -reach; dy <= reach; ++dy) {
                for (dx = -reach; dx <= reach; ++dx) {
                    int error = 12 * (abs(dx) + abs(dy)), row, col;
                    for (row = -patch; row <= patch; row += patch) {
                        for (col = -patch; col <= patch; col += patch) {
                            DWORD a = from[(y + row) * w + x + col];
                            DWORD b = to[(y + row + dy) * w + x + col + dx];
                            error += 2 * abs((int)(a >> 24) - (int)(b >> 24)) +
                                     abs((int)((a >> 8) & 255) - (int)((b >> 8) & 255));
                        }
                    }
                    if (error < best_error) {
                        best = (POINT){dx, dy};
                        best_error = error;
                    }
                }
            }
            motion[gy][gx] = best;
        }
    }
}

static DWORD mix_pixels(DWORD a, DWORD b, DWORD weight)
{
    const DWORD mask = 0x00ff00ff;
    DWORD remaining = PIXEL_UNIT - weight;
    DWORD rb = ((a & mask) * remaining + (b & mask) * weight) >> 8;
    DWORD ag = (((a >> 8) & mask) * remaining + ((b >> 8) & mask) * weight);
    return (rb & mask) | (ag & ~mask);
}

static DWORD sample_pose(const DWORD *pixels, int w, int h, int x, int y)
{
    int col, row, next_col, next_row;
    x = clamp(x, 0, (w - 1) * PIXEL_UNIT);
    y = clamp(y, 0, (h - 1) * PIXEL_UNIT);
    col = x / PIXEL_UNIT;
    row = y / PIXEL_UNIT;
    next_col = min(col + 1, w - 1);
    next_row = min(row + 1, h - 1);
    return mix_pixels(mix_pixels(pixels[row * w + col], pixels[row * w + next_col], x % PIXEL_UNIT),
                      mix_pixels(pixels[next_row * w + col], pixels[next_row * w + next_col], x % PIXEL_UNIT),
                      y % PIXEL_UNIT);
}

static void morph_pose(const DWORD *from, const DWORD *to, int w, int h, BOOL flip, int weight)
{
    int x, y, grid_x[MAX_ATLAS_SIZE];
    for (x = 0; x < w; ++x)
        grid_x[x] = x * (MOTION_GRID - 1) * PIXEL_UNIT / (w - 1);
    for (y = 0; y < h; ++y) {
        int gy = y * (MOTION_GRID - 1) * PIXEL_UNIT / (h - 1);
        int row = min(gy / PIXEL_UNIT, MOTION_GRID - 2);
        int fy = gy - row * PIXEL_UNIT, col;
        int dx[MOTION_GRID], dy[MOTION_GRID];
        for (col = 0; col < MOTION_GRID; ++col) {
            POINT a = motion[row][col], b = motion[row + 1][col];
            dx[col] = a.x * (PIXEL_UNIT - fy) + b.x * fy;
            dy[col] = a.y * (PIXEL_UNIT - fy) + b.y * fy;
        }
        for (x = 0; x < w; ++x) {
            int gx = grid_x[x];
            int fx, mx, my;
            DWORD a, b;
            col = min(gx / PIXEL_UNIT, MOTION_GRID - 2);
            fx = gx - col * PIXEL_UNIT;
            mx = (dx[col] * (PIXEL_UNIT - fx) + dx[col + 1] * fx) / PIXEL_UNIT;
            my = (dy[col] * (PIXEL_UNIT - fx) + dy[col + 1] * fx) / PIXEL_UNIT;
            a = sample_pose(from, w, h, x * PIXEL_UNIT - mx * weight / PIXEL_UNIT,
                             y * PIXEL_UNIT - my * weight / PIXEL_UNIT);
            b = sample_pose(to, w, h, x * PIXEL_UNIT + mx * (PIXEL_UNIT - weight) / PIXEL_UNIT,
                             y * PIXEL_UNIT + my * (PIXEL_UNIT - weight) / PIXEL_UNIT);
            sprite.pixels[y * w + (flip ? w - 1 - x : x)] = mix_pixels(a, b, (DWORD)weight);
        }
    }
}

static void paint_atlas_pose(int pose, BOOL flip, int key, DWORD now, BOOL pinned)
{
    int x, y, w = sprite.size.cx, h = sprite.size.cy;
    size_t count = (size_t)w * h;
    const DWORD *target = pose_pixels + count * pose;
    if (painted_pose != key) {
        int previous = painted_pose % POSE_COUNT;
        int group = pose_group(pose);
        blend_ms = 0;
        if (painted_pose >= 0 && animate && previous != pose &&
            (painted_pose >= POSE_COUNT) == flip && pose_group(previous) == group) {
            blend_started = now;
            blend_from_pose = previous;
            blend_ms = (group == WALK_FIRST || group == WALK_DETAIL_FIRST) ? 40 : 64;
            match_motion(pose_pixels + count * previous, target, w, h);
        }
    }
    if (blend_ms && animate && now - blend_started < blend_ms) {
        int weight = (int)((now - blend_started) * PIXEL_UNIT / blend_ms);
        morph_pose(pose_pixels + count * blend_from_pose, target, w, h, flip, weight);
        if ((!pinned || sprite.pixels[hint_hit.y * w + hint_hit.x] >> 24 >= 128) &&
            measure_sprite(&click_pixel, &sprite_bounds)) return;
    }
    blend_ms = 0;
    if (!flip) {
        CopyMemory(sprite.pixels, target, count * sizeof(DWORD));
    } else {
        for (y = 0; y < h; ++y)
            for (x = 0; x < w; ++x)
                sprite.pixels[y * w + x] = target[y * w + w - 1 - x];
    }
    click_pixel = pose_clicks[pose];
    sprite_bounds = pose_bounds[pose];
    if (flip) {
        click_pixel.x = w - 1 - click_pixel.x;
        sprite_bounds.left = w - pose_bounds[pose].right;
        sprite_bounds.right = w - pose_bounds[pose].left;
    }
}

static void frame(void)
{
    DWORD now = animation_now();
    POINT pointer = {INT_MIN / 2, INT_MIN / 2}, at;
    BYTE opacity = 255;
    BOOL hints = hint_is_open();
    BOOL pinned;
    int pose = 0, pose_key;
    BOOL flip, can_route = input_mouse_hook_active();

    if (GetCursorPos(&pointer) && pressed) {
        if (routed_press && can_route) {
            // A quick hint click can already have returned the real pointer.
            // If the hook saw the release, wait for its queued coordinates.
            if (InterlockedCompareExchange(&routed_left_down, 0, 0))
                move_from_press(pointer);
        } else if (!routed_press && (GetAsyncKeyState(VK_LBUTTON) & 0x8000))
            move_from_press(pointer);
        else
            release_pet(pointer);
    }
    if (can_route != route_mouse) {
        LONG style = GetWindowLongW(pet, GWL_EXSTYLE);
        route_mouse = can_route;
        if (route_mouse) {
            if (painted_pose >= 0) publish_hit_map();
            SetWindowLongW(pet, GWL_EXSTYLE, style | WS_EX_TRANSPARENT);
        } else {
            SetWindowLongW(pet, GWL_EXSTYLE, style & ~WS_EX_TRANSPARENT);
            clear_hit_map();
        }
    }
    if (pending_slot >= 0 && (!animate || now - bubble_tick >= BUBBLE_MS)) {
        int slot = pending_slot;
        pending_slot = -1;
        ShowWindow(bubble, SW_HIDE);
        start_shortcut(slot, now);
    }
    finish_launch(now);
    render_bubble(now);
    if (hints) last_hint = now;
    pinned = !dragging && now - last_hint < 400 && (hints || now - pulse > now - last_hint);
    if (pinned) {
        pet_motion.last_frame = now;
        if (pet_motion.state == PET_NAP) InterlockedExchange(&pending_command, 0);
        else InterlockedCompareExchange(&pending_command, 0, SECRETARY_HINT);
        if (!hint_animate || !animate) return;
    } else if (!saying_goodbye) {
        receive_command(now);
        advance_pet(now, pointer);
    }
    at = pinned ? pet_at : (pressed && !dragging ? press_at : anchor);
    if (use_atlas) {
        if (!pose_pixels) return;
        pose = pinned && !hint_sleeping ? hint_pose(now) : animated_pose(now);
        if (animate && dragging) at.y -= px(5);
    } else if (animate && !pressed && !opened) {
        if (!moved) at.x += (int)(px(6) * sin((now - born) / 1450.0));
        at.y -= (int)(px(4) * (1.0 + sin((now - born) / 540.0)));
        if (now - born < 320) opacity = (BYTE)(80 + (now - born) * 175 / 320);
        if (now - pulse < 290) at.y -= (int)(px(10) * sin((now - pulse) * 3.14159 / 290.0));
    }
    if (dragging && !use_atlas) opacity = 218;
    flip = use_atlas && pet_motion.left && ((pose >= WALK_FIRST && pose < REST_FIRST) ||
           (pose >= WALK_DETAIL_FIRST && pose < ATTENTION_FIRST));
    pose_key = pose + (flip ? POSE_COUNT : 0);
    if (!blend_ms && painted_pose == pose_key && painted_opacity == opacity &&
        pet_at.x == at.x && pet_at.y == at.y) return;
    if (use_atlas && (painted_pose != pose_key || blend_ms))
        paint_atlas_pose(pose, flip, pose_key, now, pinned);
    painted_pose = pose_key;
    painted_opacity = opacity;
    pet_at = at;
    if (present(pet, &sprite, pet_at, opacity) && route_mouse)
        publish_hit_map();
}

void secretary_hint_reaction(void)
{
    DWORD now = animation_now();
    int leading[POSES_PER_SHEET * 2], count = 0;
    if (!pet || hint_is_open()) return;
    hint_sleeping = pet_motion.state == PET_NAP;
    if (hint_sleeping) InterlockedExchange(&pending_command, 0);
    else {
        InterlockedCompareExchange(&pending_command, 0, SECRETARY_HINT);
        if (queued_command == SECRETARY_HINT) queued_command = 0;
        if (pet_motion.state == PET_WALK) {
            pet_motion.state = PET_SETTLE;
            pet_motion.started = now;
        }
    }
    last_hint = now - 400;
    hint_animate = use_atlas && animate && (hint_sleeping ? sleeping_safe :
                   g_cfg.secretary_react && attention_safe);
    hint_hit = hint_animate ? (hint_sleeping ? sleeping_hit : attention_hit) : click_pixel;
    if (hint_animate && !hint_sleeping) {
        DWORD t;
        for (t = 0; t <= 16 * 108 && count < (int)(sizeof(leading) / sizeof(leading[0])); t += FRAME_MS) {
            int pose = reaction_frame(now + t);
            if (pose < 0) pose = command_frame(now + t);
            if (pose < 0) break;
            if (!count || leading[count - 1] != pose) leading[count++] = pose;
        }
    }
    hint_waiting = count > 0;
    if (hint_animate && (hint_waiting || sprite.pixels[hint_hit.y * sprite.size.cx + hint_hit.x] >> 24 < 160)) {
        hint_animate = stable_clip_hit(hint_sleeping ? REST_FIRST : ATTENTION_FIRST,
                                      hint_sleeping ? 8 : 16, leading, count, &hint_hit);
        if (!hint_animate) hint_hit = click_pixel;
    }
    attention_started = now;
}

BOOL secretary_farewell(void)
{
    DWORD now = animation_now();
    if (!pet || !animate || !use_atlas || !g_cfg.secretary_react || input_is_locked() || pet_motion.state == PET_NAP)
        return FALSE;
    if (saying_goodbye) return TRUE;
    if (!SetTimer(g_main_wnd, SECRETARY_FAREWELL_TIMER, 590, NULL)) return FALSE;
    saying_goodbye = TRUE;
    hint_close();
    secretary_dismiss();
    pressed = dragging = FALSE;
    if (GetCapture() == pet) ReleaseCapture();
    pending_slot = -1;
    launch.exe = NULL;
    uia_cancel_browser_tab();
    reaction = queued_reaction = REACT_NONE;
    queued_command = 0;
    command_pose = SECRETARY_ACTION;
    command_started = now;
    last_hint = now - 400;
    frame();
    return TRUE;
}

static void update_animation(void)
{
    BOOL previous = animate;
    animate = g_cfg.secretary_animate != 0;
    if (g_cfg.secretary_animate < 0)
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
    if (pet && animate != previous) start_frame_timer();
}

static LRESULT CALLBACK secretary_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT point;
    DWORD message_pos;
    if (wnd == bubble) {
        switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && !activating_bubble) secretary_dismiss();
            break;
        case WM_KEYDOWN:
            if (wp == VK_TAB && !(lp & (1L << 30))) {
                int count = row_count + BUBBLE_ACTIONS;
                int offset = GetKeyState(VK_SHIFT) < 0 ? -1 : 1;
                focus_item = (focus_item + count + offset) % count;
                armed_key = 0;
                draw_bubble();
                render_bubble(animation_now());
                return 0;
            }
            if (!(lp & (1L << 30)) && !secretary_modifiers_down()) armed_key = (int)wp;
            return 0;
        case WM_KEYUP:
            if ((int)wp != armed_key) return 0;
            armed_key = 0;
            if (wp == VK_ESCAPE) secretary_dismiss();
            else if (wp == VK_RETURN || wp == VK_SPACE) {
                if (focus_item < row_count) select_slot(rows[focus_item]);
                else run_bubble_action(focus_item - row_count);
            }
            else if (wp >= '1' && wp <= '9') select_slot((int)wp - '1');
            else if (wp >= VK_NUMPAD1 && wp <= VK_NUMPAD9) select_slot((int)wp - VK_NUMPAD1);
            return 0;
        case WM_LBUTTONUP: {
            int row = GET_Y_LPARAM(lp) - px(42);
            int x = GET_X_LPARAM(lp), left = px(bubble_left ? 16 : 30);
            POINT logical = {MulDiv(x, 96, dpi), MulDiv(GET_Y_LPARAM(lp), 96, dpi)};
            int index = row / px(37);
            int action;
            if (!opened) return 0;
            for (action = 0; action < BUBBLE_ACTIONS; ++action) {
                RECT box = action_rect(action);
                if (PtInRect(&box, logical)) {
                    run_bubble_action(action);
                    return 0;
                }
            }
            if (row >= 0 && index < row_count && x >= left && x < left + px(206))
                select_slot(rows[index]);
            return 0;
        }
        }
    } else if (wnd == pet) {
        switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            point = (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            input_forward_wheel(point, msg, wp);
            return 0;
        case WM_SECRETARY_BUTTON:
            point = (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (wp == WM_LBUTTONDOWN) press_pet(point, TRUE);
            else if (wp == WM_LBUTTONUP) release_pet(point);
            else if (wp == WM_RBUTTONUP && !pressed) {
                secretary_dismiss();
                react(REACT_MENU);
                PostMessageW(g_main_wnd, WM_APP_SECRETARY_MENU, point.x, point.y);
            }
            return 0;
        case WM_LBUTTONDOWN:
            message_pos = GetMessagePos();
            press_pet((POINT){GET_X_LPARAM(message_pos), GET_Y_LPARAM(message_pos)}, FALSE);
            return 0;
        case WM_LBUTTONUP:
            message_pos = GetMessagePos();
            point = (POINT){GET_X_LPARAM(message_pos), GET_Y_LPARAM(message_pos)};
            release_pet(point);
            return 0;
        case WM_RBUTTONUP:
            secretary_dismiss();
            react(REACT_MENU);
            message_pos = GetMessagePos();
            PostMessageW(g_main_wnd, WM_APP_SECRETARY_MENU,
                         GET_X_LPARAM(message_pos), GET_Y_LPARAM(message_pos));
            return 0;
        case WM_SECRETARY_FRAME:
            InterlockedExchange(&frame_posted, 0);
            frame();
            return 0;
        case WM_TIMER:
            frame();
            return 0;
        case WM_DISPLAYCHANGE:
            if (update_monitor(anchor)) clamp_anchor();
            return 0;
        case WM_SETTINGCHANGE:
            update_animation();
            if (update_monitor(anchor)) clamp_anchor();
            return 0;
        }
    }
    if (msg == WM_CLOSE) { secretary_dismiss(); return 0; }
    return DefWindowProcW(wnd, msg, wp, lp);
}

BOOL secretary_has_focus(void)
{
    HWND wnd = bubble;
    return wnd && GetForegroundWindow() == wnd;
}

BOOL secretary_hint_target(const RECT *monitor, ClickTarget *target)
{
    POINT hit = hint_is_open() ? hint_hit : click_pixel;
    POINT point = {pet_at.x + hit.x, pet_at.y + hit.y};
    if (!pet || !IsWindowVisible(pet) || !PtInRect(monitor, point) || dragging)
        return FALSE;
    target->click = point;
    SetRect(&target->rc, point.x - px(14), point.y - px(14),
            point.x + px(14), point.y + px(14));
    IntersectRect(&target->rc, &target->rc, monitor);
    return TRUE;
}

void secretary_shutdown(void)
{
    int extra;
    opened = pressed = dragging = routed_press = route_mouse = FALSE;
    clear_hit_map();
    free(hit_maps[0].mask);
    free(hit_maps[1].mask);
    ZeroMemory(hit_maps, sizeof(hit_maps));
    pending_slot = -1;
    launch.exe = NULL;
    uia_cancel_browser_tab();
    stop_frame_timer();
    if (pet) DestroyWindow(pet);
    if (bubble) DestroyWindow(bubble);
    pet = bubble = NULL;
    free_canvas(&speech);
    free_canvas(&sprite);
    free(pose_pixels);
    pose_pixels = NULL;
    if (image) GdipDisposeImage(image);
    image = NULL;
    for (extra = 0; extra < SECRETARY_EXTRA_SHEETS; ++extra) {
        if (extra_images[extra]) GdipDisposeImage(extra_images[extra]);
        extra_images[extra] = NULL;
        extra_ready[extra] = FALSE;
    }
    if (gdiplus) GdiplusShutdown(gdiplus);
    gdiplus = 0;
}

static void resolve_art_path(WCHAR *path, const WCHAR *art)
{
    const WCHAR *ini = config_path(), *slash = wcsrchr(ini, L'\\');
    if ((art[0] && art[1] == L':') || art[0] == L'\\' || !slash)
        lstrcpynW(path, art, APP_PATH_MAX);
    else
        _snwprintf(path, APP_PATH_MAX, L"%.*ls%ls", (int)(slash - ini + 1), ini, art);
    path[APP_PATH_MAX - 1] = 0;
}

void secretary_config_changed(void)
{
    BOOL atlas = g_cfg.secretary_sprite_sheet[0] != 0;
    const WCHAR *art = atlas ? g_cfg.secretary_sprite_sheet : g_cfg.secretary_image;
    WCHAR path[APP_PATH_MAX];
    GdiplusStartupInput input = {1, NULL, FALSE, TRUE};
    DWORD styles = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    int extra;

    if (!g_cfg.secretary_enabled || !*art) {
        secretary_shutdown();
        return;
    }
    resolve_art_path(path, art);
    pending_slot = -1;
    secretary_dismiss();
    update_animation();
    if (pet && use_atlas == atlas && _wcsicmp(image_path, path) == 0 &&
        memcmp(extra_settings, g_cfg.secretary_extra, sizeof(extra_settings)) == 0) return;
    secretary_shutdown();
    use_atlas = atlas;
    if (GdiplusStartup(&gdiplus, &input, NULL) != Ok) goto fail;
    if (GdipLoadImageFromFile(path, &image) != Ok) goto fail;
    lstrcpynW(image_path, path, APP_PATH_MAX);
    CopyMemory(extra_settings, g_cfg.secretary_extra, sizeof(extra_settings));
    for (extra = 0; atlas && extra < SECRETARY_EXTRA_SHEETS; ++extra) {
        if (!extra_settings[extra][0]) continue;
        resolve_art_path(path, extra_settings[extra]);
        if (GdipLoadImageFromFile(path, &extra_images[extra]) != Ok)
            app_log(L"secretary: could not read animation sheet %ls", path);
    }
    if (!positioned) {
        DWORD bytes = sizeof(anchor);
        moved = read_registry(HKEY_CURRENT_USER, POSITION_KEY, L"Position", REG_BINARY,
                              &anchor, &bytes) && bytes == sizeof(anchor);
        if (!moved) GetCursorPos(&anchor);
    }
    if (!update_monitor(anchor)) goto fail;
    if (!positioned && !moved) {
        anchor.x = work.right - sprite.size.cx - px(27);
        anchor.y = work.bottom - layout_height() - px(25);
    }
    clamp_anchor();
    if (!positioned) home = anchor;
    positioned = TRUE;
    pet = CreateWindowExW(styles | WS_EX_NOACTIVATE, SECRETARY_PET_CLASS, L"Desktop secretary", WS_POPUP,
                          0, 0, 0, 0, NULL, NULL, g_hinst, NULL);
    bubble = CreateWindowExW(styles, BUBBLE_CLASS, L"Secretary shortcuts", WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, g_hinst, NULL);
    if (!pet || !bubble) goto fail;
    born = animation_now();
    last_activity = born;
    reaction = queued_reaction = REACT_NONE;
    command_pose = queued_command = 0;
    last_yawn = born - 27000;
    pet_motion.seed = (born ^ GetCurrentProcessId() ^ GetCurrentThreadId()) | 1;
    pet_motion.last_frame = born;
    wait_to_roam(born, 2500);
    pulse = born - 300;
    last_hint = born - 400;
    frame();
    ShowWindow(pet, SW_SHOWNOACTIVATE);
    start_frame_timer();
    return;
fail:
    secretary_shutdown();
    launch_error(L"Could not show the desktop secretary. Check the image or sprite sheet in [secretary].");
}

void secretary_init(void)
{
    WNDCLASSW cls = {0};
    FARPROC fn = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueryFullProcessImageNameW");
    CopyMemory(&process_path, &fn, sizeof(process_path));
    cls.hInstance = g_hinst;
    cls.lpfnWndProc = secretary_proc;
    QueryPerformanceFrequency(&animation_frequency);
    // The system hand cursor is Vim's signal to pause on links.
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.lpszClassName = SECRETARY_PET_CLASS;
    if (!RegisterClassW(&cls)) return;
    cls.hCursor = LoadCursorW(NULL, IDC_HAND);
    cls.lpszClassName = BUBBLE_CLASS;
    if (!RegisterClassW(&cls)) return;
    secretary_config_changed();
}
