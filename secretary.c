// A layered, mouse-transparent-around-the-art desktop secretary.
#include "common.h"
#include <gdiplus.h>
#include <shellapi.h>
#include <windowsx.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <wchar.h>

#ifndef SPI_GETCLIENTAREAANIMATION
#define SPI_GETCLIENTAREAANIMATION 0x1042
#endif

#define PET_CLASS L"VimMouseSecretary"
#define BUBBLE_CLASS L"VimMouseSecretaryBubble"
#define POSITION_KEY L"Software\\vim mouse\\Secretary"
#define FRAME_MS 33
#define BUBBLE_MS 165

typedef struct {
    HDC dc;
    HBITMAP dib;
    HGDIOBJ old;
    DWORD *pixels;
    GpBitmap *bitmap;
    GpGraphics *graphics;
    SIZE size;
} Canvas;

static HWND pet, bubble;
static ULONG_PTR gdiplus;
static Canvas sprite, speech;
static GpImage *image;
static WCHAR image_path[APP_PATH_MAX];
static POINT anchor, pet_at, press, press_at, click_pixel, bubble_at;
static RECT work;
static UINT dpi;
static DWORD born, pulse, bubble_tick, last_hint;
static BOOL positioned, pressed, dragging, moved, opened, animate;
static BOOL bubble_left;
static int rows[SECRETARY_SLOTS], row_count, armed_key, selected = -1;
static int pending_slot = -1;
static BOOL (WINAPI *process_path)(HANDLE, DWORD, LPWSTR, PDWORD);

// A new browser window keeps the user's current tabs, profile, and sign-in.
// Track existing windows so F11 can only go to the one the shortcut opened.
static struct {
    const WCHAR *exe;
    HWND before[128], target;
    size_t count;
    DWORD began, sent;
    BOOL browser, overflow;
} launch;

static int px(int n) { return MulDiv(n, dpi, 96); }

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

static void present(HWND wnd, const Canvas *c, POINT at, BYTE opacity)
{
    POINT source = {0, 0};
    SIZE size = c->size;
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd, NULL, &at, &size, c->dc, &source, 0, &blend, ULW_ALPHA);
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

static BOOL draw_sprite(void)
{
    UINT w, h;
    int width, height, x, y, nearest = INT_MAX;
    int size = px(144);

    if (GdipGetImageWidth(image, &w) != Ok || GdipGetImageHeight(image, &h) != Ok ||
        !w || !h || w > INT_MAX || h > INT_MAX)
        return FALSE;
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
    for (y = 0; y < height; ++y) {
        for (x = 0; x < width; ++x) {
            int dx = x - width / 2, dy = y - height / 2;
            int distance = dx * dx + dy * dy;
            if (distance < nearest && sprite.pixels[y * width + x] >> 24 >= 128) {
                click_pixel = (POINT){x, y};
                nearest = distance;
            }
        }
    }
    return nearest != INT_MAX;
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
    anchor.y = clamp(anchor.y, work.top + margin, work.bottom - sprite.size.cy - margin);
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

static int footer_y(void) { return 49 + 37 * (row_count ? row_count : 1); }
static int bubble_height(void) { return footer_y() + 33; }

static void draw_bubble(void)
{
    GpGraphics *g = speech.graphics;
    GpSolidFill *brush = NULL;
    GpFontFamily *family = NULL;
    GpFont *regular = NULL, *bold = NULL, *small = NULL;
    GpStringFormat *format = NULL;
    int shift = bubble_left ? 0 : 14;
    int x = 6 + shift, right = x + 226, bottom = bubble_height() - 8;
    int tip_x = bubble_left ? right + 11 : x - 11;
    int tail_y = clamp(MulDiv(pet_at.y + sprite.size.cy / 2 - bubble_at.y,
                             96, dpi), 24, bottom - 18);
    int edge = bubble_left ? right - 2 : x + 2;
    GpPoint tail[3] = {{px(edge), px(tail_y - 8)}, {px(tip_x), px(tail_y)},
                      {px(edge), px(tail_y + 8)}};
    int i;

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
        GdipCreateFont(family, (REAL)px(12), FontStyleRegular, UnitPixel, &small) != Ok ||
        GdipCreateStringFormat(StringFormatFlagsNoWrap, 0, &format) != Ok)
        goto done;
    GdipSetStringFormatTrimming(format, StringTrimmingEllipsisCharacter);
    write_text(brush, bold, format, L"\xbc14\xb85c\xac00\xae30", x + 13, 16, 190, 25, 0xff302040);
    for (i = 0; i < row_count; ++i) {
        int slot = rows[i], y = 42 + 37 * i;
        WCHAR key[] = {(WCHAR)(L'1' + slot), 0};
        rounded(g, brush, (RECT){px(x + 10), px(y), px(right - 10), px(y + 33)},
                px(9), slot == selected ? 0xffded0f7 : 0xfff1eaf9);
        rounded(g, brush, (RECT){px(x + 16), px(y + 5), px(x + 39), px(y + 28)},
                px(6), slot == selected ? 0xff57368b : 0xff7149a4);
        write_text(brush, bold, format, key, x + 22, y + 7, 16, 22, 0xffffffff);
        write_text(brush, regular, format, g_cfg.secretary_shortcuts[slot].label,
                   x + 48, y + 7, 160, 22, 0xff362447);
    }
    if (!row_count)
        write_text(brush, regular, format, L"\xc124\xc815\xc5d0\xc11c \xbc14\xb85c\xac00\xae30\xb97c \xcd94\xac00\xd558\xc138\xc694",
                   x + 13, 50, 200, 24, 0xff554765);
    write_text(brush, small, format, L"Esc   \xb2eb\xae30", x + 13, footer_y(), 195, 18, 0xff685678);
done:
    if (format) GdipDeleteStringFormat(format);
    if (small) GdipDeleteFont(small);
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
    present(bubble, &speech, at, opacity);
}

void secretary_dismiss(void)
{
    if (!opened) return;
    opened = FALSE;
    armed_key = 0;
    bubble_tick = GetTickCount();
    if (!animate) ShowWindow(bubble, SW_HIDE);
}

static void open_bubble(void)
{
    int i, width, height;
    if (input_is_locked() || hint_is_open() || pending_slot >= 0) return;
    row_count = 0;
    for (i = 0; i < SECRETARY_SLOTS; ++i)
        if (g_cfg.secretary_shortcuts[i].target[0]) rows[row_count++] = i;
    width = px(252);
    height = px(bubble_height());
    if (!make_canvas(&speech, width, height)) return;
    bubble_left = pet_at.x - width + px(10) >= work.left ||
                  pet_at.x > (work.left + work.right) / 2;
    bubble_at.x = bubble_left ? pet_at.x - width + px(12)
                              : pet_at.x + sprite.size.cx - px(12);
    bubble_at.x = clamp(bubble_at.x, work.left, work.right - width);
    bubble_at.y = clamp(pet_at.y + sprite.size.cy / 2 - px(45), work.top, work.bottom - height);
    selected = -1;
    draw_bubble();
    opened = TRUE;
    bubble_tick = GetTickCount();
    pulse = bubble_tick;
    present(bubble, &speech, bubble_at, animate ? 75 : 255);
    // It owns the keyboard only as long as it has normal window focus.
    ShowWindow(bubble, SW_SHOW);
    SetForegroundWindow(bubble);
}

static void select_slot(int slot)
{
    if (!opened || slot < 0 || slot >= SECRETARY_SLOTS ||
        !g_cfg.secretary_shortcuts[slot].target[0] || pending_slot >= 0) return;
    selected = slot;
    draw_bubble();
    pending_slot = slot;
    pulse = GetTickCount();
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

static BOOL CALLBACK note_window(HWND wnd, LPARAM lp)
{
    (void)lp;
    if (app_window(wnd, launch.exe)) {
        if (launch.count == sizeof(launch.before) / sizeof(launch.before[0])) {
            launch.overflow = TRUE;
            return FALSE;
        }
        launch.before[launch.count++] = wnd;
    }
    return TRUE;
}

static BOOL before_launch(HWND wnd)
{
    size_t i;
    for (i = 0; i < launch.count; ++i)
        if (launch.before[i] == wnd) return TRUE;
    return FALSE;
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
    launch.exe = NULL;
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

static void start_shortcut(int slot, DWORD now)
{
    const WCHAR *target = g_cfg.secretary_shortcuts[slot].target;
    const WCHAR *file, *args = NULL;
    WCHAR path[MAX_PATH], command[SECRETARY_TARGET_MAX + 48];
    HINSTANCE result;

    ZeroMemory(&launch, sizeof(launch));
    launch.began = now;
    if (_wcsicmp(target, L"claude") == 0) {
        HWND foreground = GetForegroundWindow();
        launch.exe = L"claude.exe";
        if (app_window(foreground, launch.exe)) return;
        file = L"shell:AppsFolder\\Claude_pzs8sxrjxfjjc!Claude";
    } else {
        launch.exe = L"chrome.exe";
        launch.browser = TRUE;
        if (!browser_path(launch.exe, path)) {
            launch.exe = L"msedge.exe";
            if (!browser_path(launch.exe, path)) {
                launch_error(L"Install Chrome or Edge for full-screen web shortcuts.");
                return;
            }
        }
        EnumWindows(note_window, 0);
        if (launch.overflow) {
            launch_error(L"Too many browser windows are open. Close one and try again.");
            return;
        }
        _snwprintf(command, sizeof(command) / sizeof(*command),
                   L"--new-window --start-fullscreen \"%ls\"", target);
        file = path;
        args = command;
    }
    result = ShellExecuteW(NULL, L"open", file, args, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) launch_error(L"Windows could not open this shortcut.");
}

static void finish_launch(DWORD now)
{
    HWND foreground;
    INPUT keys[2] = {{0}, {0}};
    if (!launch.exe) return;
    if (now - launch.began >= 10000 || (launch.sent && now - launch.sent >= 2500)) {
        launch_error(L"The shortcut did not enter full screen. Try F11 in the opened window.");
        return;
    }
    foreground = GetForegroundWindow();
    if (!launch.target) {
        if (!app_window(foreground, launch.exe) ||
            (launch.browser && before_launch(foreground))) return;
        launch.target = foreground;
    }
    if (in_fullscreen(launch.target)) {
        launch.exe = NULL;
        return;
    }
    if (launch.sent || now - launch.began < 250 || foreground != launch.target ||
        input_modifier_held()) return;
    keys[0].type = keys[1].type = INPUT_KEYBOARD;
    keys[0].ki.wVk = keys[1].ki.wVk = VK_F11;
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(2, keys, sizeof(INPUT)) == 2) launch.sent = now;
}

static void move_from_press(POINT pointer)
{
    if (!dragging && (abs(pointer.x - press.x) >= px(5) || abs(pointer.y - press.y) >= px(5))) {
        dragging = TRUE;
        secretary_dismiss();
    }
    if (dragging) {
        anchor.x = press_at.x + pointer.x - press.x;
        anchor.y = press_at.y + pointer.y - press.y;
        anchor.x = clamp(anchor.x, GetSystemMetrics(SM_XVIRTUALSCREEN),
                         GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) - sprite.size.cx);
        anchor.y = clamp(anchor.y, GetSystemMetrics(SM_YVIRTUALSCREEN),
                         GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) - sprite.size.cy);
    }
}

static void release_pet(POINT pointer)
{
    if (!pressed) return;
    move_from_press(pointer);
    pressed = FALSE;
    if (GetCapture() == pet) ReleaseCapture();
    if (dragging) {
        dragging = FALSE;
        update_monitor(pointer);
        clamp_anchor();
        moved = TRUE;
        store_position();
        pulse = GetTickCount();
    } else if (opened) {
        secretary_dismiss();
    } else {
        open_bubble();
    }
}

static void frame(void)
{
    DWORD now = GetTickCount();
    POINT pointer, at;
    BYTE opacity = 255;
    BOOL hints = hint_is_open();

    if (GetCursorPos(&pointer) && pressed) {
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) move_from_press(pointer);
        else release_pet(pointer);
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
    if (!dragging && now - last_hint < 400) return; // Keep the click point through selection.
    at = pressed && !dragging ? press_at : anchor;
    if (animate && !pressed && !opened) {
        if (!moved) at.x += (int)(px(6) * sin((now - born) / 1450.0));
        at.y -= (int)(px(4) * (1.0 + sin((now - born) / 540.0)));
        if (now - born < 320) opacity = (BYTE)(80 + (now - born) * 175 / 320);
        if (now - pulse < 290) at.y -= (int)(px(10) * sin((now - pulse) * 3.14159 / 290.0));
    }
    if (dragging) opacity = 218;
    pet_at = at;
    present(pet, &sprite, pet_at, opacity);
}

static void update_animation(void)
{
    animate = g_cfg.secretary_animate != 0;
    if (g_cfg.secretary_animate < 0)
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
}

static LRESULT CALLBACK secretary_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT point;
    DWORD message_pos;
    if (wnd == bubble) {
        switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) secretary_dismiss();
            break;
        case WM_KEYDOWN:
            if (!(lp & (1L << 30)) && !input_modifier_held()) armed_key = (int)wp;
            return 0;
        case WM_KEYUP:
            if ((int)wp != armed_key) return 0;
            armed_key = 0;
            if (wp == VK_ESCAPE) secretary_dismiss();
            else if (wp >= '1' && wp <= '9') select_slot((int)wp - '1');
            else if (wp >= VK_NUMPAD1 && wp <= VK_NUMPAD9) select_slot((int)wp - VK_NUMPAD1);
            return 0;
        case WM_LBUTTONUP: {
            int row = GET_Y_LPARAM(lp) - px(42);
            int x = GET_X_LPARAM(lp), left = px(bubble_left ? 16 : 30);
            int index = row / px(37);
            if (row >= 0 && index < row_count && x >= left && x < left + px(206))
                select_slot(rows[index]);
            return 0;
        }
        }
    } else if (wnd == pet) {
        switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
            // Vim's hint click may already have returned the real cursor.
            message_pos = GetMessagePos();
            press = (POINT){GET_X_LPARAM(message_pos), GET_Y_LPARAM(message_pos)};
            pressed = TRUE;
            press_at = pet_at;
            SetCapture(pet);
            return 0;
        case WM_LBUTTONUP:
            message_pos = GetMessagePos();
            point = (POINT){GET_X_LPARAM(message_pos), GET_Y_LPARAM(message_pos)};
            release_pet(point);
            return 0;
        case WM_RBUTTONUP:
            secretary_dismiss();
            pulse = GetTickCount();
            message_pos = GetMessagePos();
            PostMessageW(g_main_wnd, WM_APP_SECRETARY_MENU,
                         GET_X_LPARAM(message_pos), GET_Y_LPARAM(message_pos));
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
    POINT point = {pet_at.x + click_pixel.x, pet_at.y + click_pixel.y};
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
    opened = pressed = dragging = FALSE;
    pending_slot = -1;
    launch.exe = NULL;
    if (pet) DestroyWindow(pet);
    if (bubble) DestroyWindow(bubble);
    pet = bubble = NULL;
    free_canvas(&speech);
    free_canvas(&sprite);
    if (image) GdipDisposeImage(image);
    image = NULL;
    if (gdiplus) GdiplusShutdown(gdiplus);
    gdiplus = 0;
}

void secretary_config_changed(void)
{
    const WCHAR *art = g_cfg.secretary_image;
    WCHAR path[APP_PATH_MAX];
    GdiplusStartupInput input = {1, NULL, FALSE, TRUE};
    DWORD styles = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    const WCHAR *ini = config_path(), *slash = wcsrchr(ini, L'\\');

    if (!g_cfg.secretary_enabled || !*art) {
        secretary_shutdown();
        return;
    }
    if ((art[0] && art[1] == L':') || art[0] == L'\\' || !slash)
        lstrcpynW(path, art, APP_PATH_MAX);
    else
        _snwprintf(path, APP_PATH_MAX, L"%.*ls%ls", (int)(slash - ini + 1), ini, art);
    path[APP_PATH_MAX - 1] = 0;
    pending_slot = -1;
    secretary_dismiss();
    update_animation();
    if (pet && _wcsicmp(image_path, path) == 0) return;
    secretary_shutdown();
    if (GdiplusStartup(&gdiplus, &input, NULL) != Ok) goto fail;
    if (GdipLoadImageFromFile(path, &image) != Ok) goto fail;
    if (!positioned) {
        DWORD bytes = sizeof(anchor);
        moved = read_registry(HKEY_CURRENT_USER, POSITION_KEY, L"Position", REG_BINARY,
                              &anchor, &bytes) && bytes == sizeof(anchor);
        if (!moved) GetCursorPos(&anchor);
    }
    if (!update_monitor(anchor)) goto fail;
    if (!positioned && !moved) {
        anchor.x = work.right - sprite.size.cx - px(27);
        anchor.y = work.bottom - sprite.size.cy - px(25);
    }
    positioned = TRUE;
    clamp_anchor();
    pet = CreateWindowExW(styles | WS_EX_NOACTIVATE, PET_CLASS, L"Desktop secretary", WS_POPUP,
                          0, 0, 0, 0, NULL, NULL, g_hinst, NULL);
    bubble = CreateWindowExW(styles, BUBBLE_CLASS, L"Secretary shortcuts", WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, g_hinst, NULL);
    if (!pet || !bubble) goto fail;
    lstrcpynW(image_path, path, APP_PATH_MAX);
    born = GetTickCount();
    pulse = born - 300;
    last_hint = born - 400;
    frame();
    ShowWindow(pet, SW_SHOWNOACTIVATE);
    SetTimer(pet, 1, FRAME_MS, NULL);
    return;
fail:
    secretary_shutdown();
    launch_error(L"Could not show the desktop secretary. Check the image in [secretary].");
}

void secretary_init(void)
{
    WNDCLASSW cls = {0};
    FARPROC fn = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueryFullProcessImageNameW");
    CopyMemory(&process_path, &fn, sizeof(process_path));
    cls.hInstance = g_hinst;
    cls.lpfnWndProc = secretary_proc;
    cls.hCursor = LoadCursorW(NULL, IDC_HAND);
    cls.lpszClassName = PET_CLASS;
    if (!RegisterClassW(&cls)) return;
    cls.lpszClassName = BUBBLE_CLASS;
    if (!RegisterClassW(&cls)) return;
    secretary_config_changed();
}
