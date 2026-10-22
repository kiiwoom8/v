// tray.c - tray icon, tray menu, help window, start-with-Windows.

#ifndef _WIN32_IE
#define _WIN32_IE 0x0601
#endif
#include "common.h"

#include <basetyps.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define TRAY_ICON_ID   1
#define HELP_CLASS     L"VimMouseHelpWindow"

#define IDM_HINTS      1001
#define IDM_MOUSE      1002
#define IDM_PAUSE      1003
#define IDM_SHORTCUTS  1004
#define IDM_EDIT       1005
#define IDM_RELOAD     1006
#define IDM_STARTUP    1007
#define IDM_QUIT       1008

#define HELP_MAX_ROWS  64

#ifndef NIN_SELECT
#define NIN_SELECT     (WM_USER + 0)
#endif
#ifndef NIN_KEYSELECT
#define NIN_KEYSELECT  (WM_USER + 1)
#endif

static const WCHAR RUN_KEY[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

typedef struct {
    BYTE r, g, b, a;
} RGBA;

typedef struct {
    const WCHAR *section;
    WCHAR        key[96];
    WCHAR        desc[192];
    int          y;
} HelpRow;

static HWND    g_tray_wnd;
static HICON   g_icons[3];
static BOOL    g_icon_added;
static BOOL    g_tray_v4;
static DWORD   g_last_menu_tick;

static HWND    g_help_wnd;
static DWORD   g_help_shown_tick;
static BOOL    g_help_class_registered;

static HelpRow g_help_rows[HELP_MAX_ROWS];
static int     g_help_row_count;

static HFONT   g_help_title_font;
static HFONT   g_help_section_font;
static HFONT   g_help_body_font;
static int     g_help_pad;
static int     g_help_gap;
static int     g_help_keyw;
static int     g_help_footer_y;
static int     g_help_footer_h;
static int     g_help_hint_y;

static void help_close(void);

// ---------------------------------------------------------------------------
// Small string helpers

static void copy_wstr(WCHAR *dst, int cch, const WCHAR *src)
{
    if (cch <= 0)
        return;
    lstrcpynW(dst, src ? src : L"", cch);
}

static void truncate_wstr(WCHAR *dst, int cch, const WCHAR *src)
{
    copy_wstr(dst, cch, src);
}

// ---------------------------------------------------------------------------
// Icon drawing: a rounded square with a white mouse pointer, 32-bit alpha.

static void blend_px(RGBA *px, int r, int g, int b, int a)
{
    int out_a, out_r, out_g, out_b;

    if (a <= 0)
        return;
    if (a >= 255) {
        px->r = (BYTE)r;
        px->g = (BYTE)g;
        px->b = (BYTE)b;
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
    px->r = (BYTE)out_r;
    px->g = (BYTE)out_g;
    px->b = (BYTE)out_b;
    px->a = (BYTE)out_a;
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

static HICON create_icon(int r, int g, int b, int alpha)
{
    int       size = GetSystemMetrics(SM_CXSMICON);
    int       radius;
    RGBA     *px;
    BITMAPINFO bi;
    void     *bits = NULL;
    HBITMAP   color_bmp, mask_bmp;
    ICONINFO  ii;
    HICON     icon;
    DWORD    *dst;
    int       i;

    if (size <= 0)
        size = 16;
    if (GetSystemMetrics(SM_CYSMICON) > size)
        size = GetSystemMetrics(SM_CYSMICON);

    px = (RGBA *)calloc((size_t)size * (size_t)size, sizeof(RGBA));
    if (!px)
        return NULL;

    radius = size / 4;
    if (radius < 1)
        radius = 1;
    fill_round_rect(px, size, radius, r, g, b, alpha);
    fill_pointer(px, size);

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = size;
    bi.bmiHeader.biHeight      = -size;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    color_bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!color_bmp || !bits) {
        free(px);
        return NULL;
    }

    // Premultiplied alpha, BGRA in memory on little-endian Windows.
    dst = (DWORD *)bits;
    for (i = 0; i < size * size; i++) {
        unsigned a = px[i].a;
        unsigned rr = (unsigned)px[i].r * a / 255;
        unsigned gg = (unsigned)px[i].g * a / 255;
        unsigned bb = (unsigned)px[i].b * a / 255;
        dst[i] = (a << 24) | (rr << 16) | (gg << 8) | bb;
    }
    free(px);

    mask_bmp = CreateBitmap(size, size, 1, 1, NULL);
    if (mask_bmp) {
        int stride = ((size + 31) / 32) * 4;
        BYTE *mask_bits = (BYTE *)calloc((size_t)stride, (size_t)size);
        if (mask_bits) {
            SetBitmapBits(mask_bmp, (LONG)((size_t)stride * (size_t)size),
                          mask_bits);
            free(mask_bits);
        }
    }

    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon    = TRUE;
    ii.hbmColor = color_bmp;
    ii.hbmMask  = mask_bmp;
    icon = CreateIconIndirect(&ii);

    DeleteObject(color_bmp);
    if (mask_bmp)
        DeleteObject(mask_bmp);
    return icon;
}

static HICON icon_for_state(void)
{
    if (input_is_paused())
        return g_icons[2];
    if (input_mouse_mode_active())
        return g_icons[1];
    return g_icons[0];
}

// ---------------------------------------------------------------------------
// Tray icon

static void nid_init(NOTIFYICONDATAW *nid, DWORD flags)
{
    ZeroMemory(nid, sizeof(*nid));
    nid->cbSize  = sizeof(*nid);
    nid->hWnd    = g_tray_wnd;
    nid->uID     = TRAY_ICON_ID;
    nid->uFlags  = flags;
}

static void build_tooltip(WCHAR *dst, int cch)
{
    WCHAR hint[64], mode[64], text[256];

    format_hotkey(&g_cfg.hint_hotkey, hint, 64);
    format_key(g_cfg.mouse_mode_key, mode, 64);
    _snwprintf(text, 255, L"vim mouse - %s hints, %s mouse mode%s",
               hint, mode, input_is_paused() ? L" (paused)" : L"");
    text[255] = L'\0';
    copy_wstr(dst, cch, text);
}

static void tray_add(void)
{
    NOTIFYICONDATAW nid;

    if (!g_tray_wnd)
        return;

    nid_init(&nid, NIF_MESSAGE | NIF_ICON | NIF_TIP);
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon            = icon_for_state();
    build_tooltip(nid.szTip, (int)(sizeof(nid.szTip) / sizeof(WCHAR)));

    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        app_log(L"tray: NIM_ADD failed (%lu)", GetLastError());
        return;
    }
    g_icon_added = TRUE;
    app_log(L"tray: icon added");

#ifdef NOTIFYICON_VERSION_4
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize  = sizeof(nid);
    nid.hWnd    = g_tray_wnd;
    nid.uID     = TRAY_ICON_ID;
    nid.uVersion = NOTIFYICON_VERSION_4;
    if (Shell_NotifyIconW(NIM_SETVERSION, &nid)) {
        g_tray_v4 = TRUE;
        app_log(L"tray: callback version 4");
    } else {
        app_log(L"tray: NIM_SETVERSION failed (%lu)", GetLastError());
    }
#endif
}

void tray_init(HWND main_wnd)
{
    g_tray_wnd = main_wnd;
    g_icons[0] = create_icon(58, 58, 60, 255);     // normal, dark grey
    g_icons[1] = create_icon(45, 127, 249, 255);   // mouse mode, blue
    g_icons[2] = create_icon(138, 138, 138, 255);  // paused, faded grey
    tray_add();
}

void tray_shutdown(void)
{
    int i;

    if (g_icon_added && g_tray_wnd) {
        NOTIFYICONDATAW nid;
        nid_init(&nid, 0);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_icon_added = FALSE;
        app_log(L"tray: icon removed");
    }
    for (i = 0; i < 3; i++) {
        if (g_icons[i]) {
            DestroyIcon(g_icons[i]);
            g_icons[i] = NULL;
        }
    }
    help_close();
    g_tray_wnd = NULL;
}

void tray_update(void)
{
    NOTIFYICONDATAW nid;

    if (!g_tray_wnd)
        return;
    if (!g_icon_added) {
        tray_add();
        return;
    }

    nid_init(&nid, NIF_ICON | NIF_TIP);
    nid.hIcon = icon_for_state();
    build_tooltip(nid.szTip, (int)(sizeof(nid.szTip) / sizeof(WCHAR)));
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid))
        app_log(L"tray: NIM_MODIFY failed (%lu)", GetLastError());
}

void tray_on_taskbar_created(void)
{
    g_icon_added = FALSE;
    g_tray_v4 = FALSE;
    tray_add();
    app_log(L"tray: taskbar recreated");
}

void tray_balloon(const WCHAR *title, const WCHAR *text)
{
    NOTIFYICONDATAW nid;

    if (!g_tray_wnd)
        return;
    if (!g_icon_added)
        tray_add();
    if (!g_icon_added)
        return;

    nid_init(&nid, NIF_INFO);
    truncate_wstr(nid.szInfoTitle,
                  (int)(sizeof(nid.szInfoTitle) / sizeof(WCHAR)),
                  title ? title : APP_NAME);
    truncate_wstr(nid.szInfo,
                  (int)(sizeof(nid.szInfo) / sizeof(WCHAR)),
                  text ? text : L"");
    nid.dwInfoFlags = NIIF_INFO;
    nid.uTimeout    = 6000;
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid))
        app_log(L"tray: balloon failed (%lu)", GetLastError());
}

// ---------------------------------------------------------------------------
// Start with Windows

static BOOL quoted_exe(WCHAR *buf, int cch)
{
    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);

    if (n == 0 || n >= MAX_PATH)
        return FALSE;
    _snwprintf(buf, cch - 1, L"\"%s\"", path);
    buf[cch - 1] = L'\0';
    return TRUE;
}

static BOOL startup_enabled(void)
{
    HKEY    key;
    WCHAR   want[MAX_PATH + 4];
    WCHAR   got[MAX_PATH + 8];
    DWORD   type = 0, size = (DWORD)sizeof(got);
    LONG    rc;

    if (!quoted_exe(want, MAX_PATH + 4))
        return FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS)
        return FALSE;
    rc = RegQueryValueExW(key, APP_NAME, NULL, &type, (BYTE *)got, &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ)
        return FALSE;
    got[(sizeof(got) / sizeof(WCHAR)) - 1] = L'\0';
    return wcscmp(got, want) == 0;
}

static void toggle_startup(void)
{
    HKEY  key;
    WCHAR want[MAX_PATH + 4];
    LONG  rc;

    if (!quoted_exe(want, MAX_PATH + 4))
        return;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key,
                        NULL) != ERROR_SUCCESS) {
        app_log(L"startup: cannot open Run key");
        return;
    }

    if (startup_enabled()) {
        rc = RegDeleteValueW(key, APP_NAME);
        app_log(L"startup: disabled (%ld)", rc);
    } else {
        rc = RegSetValueExW(key, APP_NAME, 0, REG_SZ, (const BYTE *)want,
                            (DWORD)((wcslen(want) + 1) * sizeof(WCHAR)));
        app_log(L"startup: enabled (%ld)", rc);
    }
    RegCloseKey(key);
}

static void edit_settings(void)
{
    const WCHAR *path = config_path();
    HINSTANCE    r = ShellExecuteW(NULL, L"open", path, NULL, NULL,
                                   SW_SHOWNORMAL);

    if ((INT_PTR)r <= 32) {
        WCHAR args[MAX_PATH + 4];
        _snwprintf(args, MAX_PATH + 3, L"\"%s\"", path);
        args[MAX_PATH + 3] = L'\0';
        r = ShellExecuteW(NULL, L"open", L"notepad.exe", args, NULL,
                          SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32)
            app_log(L"tray: cannot open settings file");
    }
}

// ---------------------------------------------------------------------------
// Tray menu

static void append_item(HMENU menu, UINT id, const WCHAR *label,
                        const WCHAR *accel, BOOL checked)
{
    WCHAR text[256];
    UINT  flags = MF_STRING;

    if (checked)
        flags |= MF_CHECKED;
    if (accel && accel[0] && wcscmp(accel, L"none") != 0)
        _snwprintf(text, 255, L"%s\t%s", label, accel);
    else
        copy_wstr(text, 256, label);
    text[255] = L'\0';
    AppendMenuW(menu, flags, id, text);
}

static void show_menu(void)
{
    HMENU  menu;
    WCHAR  hk[64];
    POINT  pt;
    UINT   cmd;

    // v4 can deliver both WM_CONTEXTMENU and WM_RBUTTONUP for one click.
    {
        DWORD now = GetTickCount();
        if (now - g_last_menu_tick < 250)
            return;
        g_last_menu_tick = now;
    }

    menu = CreatePopupMenu();
    if (!menu)
        return;

    format_hotkey(&g_cfg.hint_hotkey, hk, 64);
    append_item(menu, IDM_HINTS, L"Show click hints", hk, FALSE);

    format_key(g_cfg.mouse_mode_key, hk, 64);
    append_item(menu, IDM_MOUSE, L"Mouse mode", hk,
                input_mouse_mode_active());

    format_hotkey(&g_cfg.pause_hotkey, hk, 64);
    append_item(menu, IDM_PAUSE, L"Pause", hk, input_is_paused());

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    append_item(menu, IDM_SHORTCUTS, L"Keyboard shortcuts...", NULL, FALSE);
    append_item(menu, IDM_EDIT, L"Edit settings...", NULL, FALSE);
    append_item(menu, IDM_RELOAD, L"Reload settings", NULL, FALSE);
    append_item(menu, IDM_STARTUP, L"Start with Windows", NULL,
                startup_enabled());
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    append_item(menu, IDM_QUIT, L"Quit", NULL, FALSE);

    GetCursorPos(&pt);
    SetForegroundWindow(g_tray_wnd);
    cmd = (UINT)TrackPopupMenu(menu,
                               TPM_RIGHTBUTTON | TPM_RETURNCMD |
                                   TPM_NONOTIFY,
                               pt.x, pt.y, 0, g_tray_wnd, NULL);
    DestroyMenu(menu);
    PostMessageW(g_tray_wnd, WM_NULL, 0, 0);

    if (cmd)
        tray_on_command((WORD)cmd);
}

BOOL tray_on_command(WORD id)
{
    switch (id) {
    case IDM_HINTS:
        hint_open();
        return TRUE;
    case IDM_MOUSE:
        input_set_mouse_mode(!input_mouse_mode_active());
        tray_update();
        return TRUE;
    case IDM_PAUSE: {
        BOOL paused = !input_is_paused();
        input_set_paused(paused);
        if (paused)
            hint_close();
        tray_update();
        return TRUE;
    }
    case IDM_SHORTCUTS:
        help_toggle();
        return TRUE;
    case IDM_EDIT:
        edit_settings();
        return TRUE;
    case IDM_RELOAD:
        app_reload_config(TRUE);
        return TRUE;
    case IDM_STARTUP:
        toggle_startup();
        return TRUE;
    case IDM_QUIT:
        app_quit();
        return TRUE;
    default:
        return FALSE;
    }
}

void tray_on_message(WPARAM wp, LPARAM lp)
{
    UINT event;

    (void)wp;
    event = g_tray_v4 ? (UINT)LOWORD(lp) : (UINT)lp;
    switch (event) {
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
    case NIN_SELECT:
    case NIN_KEYSELECT:
        show_menu();
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Help window

static void help_add_header(const WCHAR *title)
{
    HelpRow *r;
    if (g_help_row_count >= HELP_MAX_ROWS)
        return;
    r = &g_help_rows[g_help_row_count++];
    ZeroMemory(r, sizeof(*r));
    r->section = title;
}

static void help_add_row(const WCHAR *key, const WCHAR *desc)
{
    HelpRow *r;
    if (g_help_row_count >= HELP_MAX_ROWS)
        return;
    r = &g_help_rows[g_help_row_count++];
    r->section = NULL;
    copy_wstr(r->key, 96, key);
    copy_wstr(r->desc, 192, desc);
}

static void help_add_hotkey(const Hotkey *hk, const WCHAR *desc)
{
    WCHAR buf[64];
    format_hotkey(hk, buf, 64);
    help_add_row(buf, desc);
}

static void help_add_key(UINT vk, const WCHAR *desc)
{
    WCHAR buf[64];
    format_key(vk, buf, 64);
    help_add_row(buf, desc);
}

static void build_help_rows(void)
{
    WCHAR buf[128];
    WCHAR mode[64];

    g_help_row_count = 0;

    help_add_header(L"Global");
    help_add_hotkey(&g_cfg.hint_hotkey,
                    L"Open click hints (press again to close)");
    format_key(g_cfg.mouse_mode_key, mode, 64);
    _snwprintf(buf, 127, L"%s tap / hold", mode);
    buf[127] = L'\0';
    help_add_row(buf, L"Mouse mode on/off; hold for momentary");
    _snwprintf(buf, 127, L"Shift + %s", mode);
    buf[127] = L'\0';
    help_add_row(buf, L"Real CapsLock (passes through)");
    help_add_hotkey(&g_cfg.quit_hotkey, L"Quit vim mouse");
    if (g_cfg.pause_hotkey.vk)
        help_add_hotkey(&g_cfg.pause_hotkey, L"Pause / resume everything");

    help_add_header(L"Mouse mode");
    help_add_key(g_cfg.key_left, L"Move cursor left");
    help_add_key(g_cfg.key_down, L"Move cursor down");
    help_add_key(g_cfg.key_up, L"Move cursor up");
    help_add_key(g_cfg.key_right, L"Move cursor right");
    help_add_row(L"Shift + move", L"Slow, precise movement");
    help_add_key(g_cfg.key_left_button,
                 L"Left click: tap, double-tap, hold to drag");
    help_add_key(g_cfg.key_right_click, L"Right click");
    help_add_key(g_cfg.key_middle_click, L"Middle click");
    help_add_key(g_cfg.key_drag_toggle,
                 L"Hold / release the left button (drag)");
    help_add_key(g_cfg.key_scroll_up, L"Scroll up");
    help_add_key(g_cfg.key_scroll_down, L"Scroll down");
    help_add_key(g_cfg.key_scroll_left, L"Scroll left");
    help_add_key(g_cfg.key_scroll_right, L"Scroll right");
    help_add_key(g_cfg.key_hints, L"Click hints (mouse mode stays on)");
    help_add_key(g_cfg.key_help, L"This help window (also ?)");
    format_key(g_cfg.key_exit, mode, 64);
    _snwprintf(buf, 127, L"Esc / %s", mode);
    buf[127] = L'\0';
    help_add_row(buf, L"Leave mouse mode");

    help_add_header(L"Hint mode");
    help_add_row(L"row letter, column letter", L"Pick a grid cell");
    help_add_row(L"subgrid key", L"Pick a point inside the cell");
    help_add_row(L"Enter", L"Click the cell centre");
    help_add_row(L"Space", L"Move the cursor only");
    help_add_row(L"Shift + final key", L"Right click");
    help_add_row(L"Ctrl + final key", L"Double click");
    help_add_row(L"Backspace", L"Undo one step");
    help_add_row(L"Tab / Shift+Tab", L"Next / previous monitor");
    help_add_row(L"Esc", L"Cancel");
}

static int text_width(HDC dc, HFONT font, const WCHAR *text)
{
    HFONT old = (HFONT)SelectObject(dc, font);
    SIZE  sz;
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

static HFONT make_font(int px, int weight)
{
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static void help_delete_fonts(void)
{
    if (g_help_title_font) {
        DeleteObject(g_help_title_font);
        g_help_title_font = NULL;
    }
    if (g_help_section_font) {
        DeleteObject(g_help_section_font);
        g_help_section_font = NULL;
    }
    if (g_help_body_font) {
        DeleteObject(g_help_body_font);
        g_help_body_font = NULL;
    }
}

static void help_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC         dc = BeginPaint(hwnd, &ps);
    RECT        rc;
    HDC         mem;
    HBITMAP     bmp, old_bmp;
    HBRUSH      bg;
    int         i;
    WCHAR       footer[512];
    const WCHAR *hint = L"Press any key or click to close";
    RECT        fr;

    GetClientRect(hwnd, &rc);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    old_bmp = (HBITMAP)SelectObject(mem, bmp);

    bg = CreateSolidBrush(RGB(30, 30, 30));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);

    SelectObject(mem, g_help_title_font);
    SetTextColor(mem, RGB(255, 255, 255));
    TextOutW(mem, g_help_pad, g_help_pad,
             L"vim mouse - keyboard shortcuts",
             (int)wcslen(L"vim mouse - keyboard shortcuts"));

    for (i = 0; i < g_help_row_count; i++) {
        HelpRow *r = &g_help_rows[i];
        if (r->section) {
            SelectObject(mem, g_help_section_font);
            SetTextColor(mem, RGB(154, 160, 166));
            TextOutW(mem, g_help_pad, r->y, r->section,
                     (int)wcslen(r->section));
        } else {
            SelectObject(mem, g_help_body_font);
            SetTextColor(mem, RGB(77, 163, 255));
            TextOutW(mem, g_help_pad, r->y, r->key, (int)wcslen(r->key));
            SetTextColor(mem, RGB(221, 221, 221));
            TextOutW(mem, g_help_pad + g_help_keyw + g_help_gap, r->y,
                     r->desc, (int)wcslen(r->desc));
        }
    }

    _snwprintf(footer, 511, L"Settings: %s (saved changes apply immediately)",
               config_path());
    footer[511] = L'\0';
    SelectObject(mem, g_help_body_font);
    SetTextColor(mem, RGB(170, 170, 170));
    fr.left   = g_help_pad;
    fr.top    = g_help_footer_y;
    fr.right  = rc.right - g_help_pad;
    fr.bottom = g_help_footer_y + g_help_footer_h;
    DrawTextW(mem, footer, -1, &fr, DT_WORDBREAK | DT_NOPREFIX);

    SetTextColor(mem, RGB(130, 130, 130));
    TextOutW(mem, g_help_pad, g_help_hint_y, hint, (int)wcslen(hint));

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK help_wnd_proc(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        help_paint(hwnd);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        help_close();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE &&
            GetTickCount() - g_help_shown_tick > 250)
            help_close();
        return 0;
    case WM_DESTROY:
        help_delete_fonts();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static BOOL help_register_class(void)
{
    WNDCLASSEXW wc;

    if (g_help_class_registered)
        return TRUE;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = help_wnd_proc;
    wc.hInstance     = g_hinst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = HELP_CLASS;
    if (!RegisterClassExW(&wc))
        return FALSE;
    g_help_class_registered = TRUE;
    return TRUE;
}

static void help_open(void)
{
    MONITORINFO mi;
    POINT       pt;
    HMONITOR    mon;
    UINT        dpi;
    HDC         dc;
    int         i, width, height, y;
    int         title_line, section_line, line, maxkey = 0, maxdesc = 0;
    int         titlew = 0;
    int         workw, x, yy;
    WCHAR       footer[512];
    RECT        fr;
    HFONT       old;
    RECT        work;

    if (g_help_wnd)
        return;
    if (!help_register_class())
        return;

    build_help_rows();

    pt.x = 0;
    pt.y = 0;
    GetCursorPos(&pt);
    mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    dpi = dpi_for_monitor(mon);

    g_help_pad = MulDiv(20, (int)dpi, 96);
    g_help_gap = MulDiv(20, (int)dpi, 96);
    line       = MulDiv(22, (int)dpi, 96);
    title_line = MulDiv(30, (int)dpi, 96);
    section_line = MulDiv(26, (int)dpi, 96);

    g_help_title_font = make_font(MulDiv(20, (int)dpi, 96), FW_BOLD);
    g_help_section_font = make_font(MulDiv(15, (int)dpi, 96), FW_BOLD);
    g_help_body_font = make_font(MulDiv(13, (int)dpi, 96), FW_NORMAL);
    if (!g_help_title_font || !g_help_section_font || !g_help_body_font) {
        help_delete_fonts();
        return;
    }

    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi))
        work = mi.rcMonitor;
    else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    dc = GetDC(NULL);
    old = (HFONT)SelectObject(dc, g_help_body_font);
    titlew = text_width(dc, g_help_title_font,
                        L"vim mouse - keyboard shortcuts");
    for (i = 0; i < g_help_row_count; i++) {
        HelpRow *r = &g_help_rows[i];
        int w;
        if (r->section)
            continue;
        w = text_width(dc, g_help_body_font, r->key);
        if (w > maxkey)
            maxkey = w;
        w = text_width(dc, g_help_body_font, r->desc);
        if (w > maxdesc)
            maxdesc = w;
    }

    _snwprintf(footer, 511, L"Settings: %s (saved changes apply immediately)",
               config_path());
    footer[511] = L'\0';

    g_help_keyw = maxkey + MulDiv(28, (int)dpi, 96);
    width = g_help_pad * 2 + g_help_keyw + g_help_gap + maxdesc;
    if (width < titlew + g_help_pad * 2)
        width = titlew + g_help_pad * 2;
    workw = (int)(work.right - work.left) - MulDiv(40, (int)dpi, 96);
    if (workw < MulDiv(200, (int)dpi, 96))
        workw = MulDiv(200, (int)dpi, 96);
    if (width > workw)
        width = workw;

    // Height: title, sections/rows, footer (wrapped), close hint.
    y = g_help_pad + title_line + MulDiv(4, (int)dpi, 96);
    {
        int prev_section = 0;
        for (i = 0; i < g_help_row_count; i++) {
            HelpRow *r = &g_help_rows[i];
            if (r->section) {
                if (prev_section)
                    y += MulDiv(10, (int)dpi, 96);
                prev_section = 1;
                r->y = y;
                y += section_line;
            } else {
                r->y = y;
                y += line;
            }
        }
    }
    y += MulDiv(12, (int)dpi, 96);
    g_help_footer_y = y;

    fr.left   = 0;
    fr.top    = 0;
    fr.right  = width - g_help_pad * 2;
    fr.bottom = 0;
    DrawTextW(dc, footer, -1, &fr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    g_help_footer_h = fr.bottom;
    y += g_help_footer_h + MulDiv(10, (int)dpi, 96);
    g_help_hint_y = y;
    y += line + g_help_pad;
    height = y;

    SelectObject(dc, old);
    ReleaseDC(NULL, dc);

    if (work.right > work.left) {
        x = work.left + ((int)(work.right - work.left) - width) / 2;
        yy = work.top + ((int)(work.bottom - work.top) - height) / 2;
    } else {
        x = 0;
        yy = 0;
    }
    if (x < work.left)
        x = work.left;
    if (yy < work.top)
        yy = work.top;

    g_help_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                 HELP_CLASS, L"vim mouse - keyboard shortcuts",
                                 WS_POPUP, x, yy, width, height, NULL, NULL,
                                 g_hinst, NULL);
    if (!g_help_wnd) {
        app_log(L"help: CreateWindowEx failed (%lu)", GetLastError());
        help_delete_fonts();
        return;
    }

    g_help_shown_tick = GetTickCount();
    ShowWindow(g_help_wnd, SW_SHOW);
    SetForegroundWindow(g_help_wnd);
    SetFocus(g_help_wnd);
    InvalidateRect(g_help_wnd, NULL, TRUE);
    app_log(L"help: opened");
}

static void help_close(void)
{
    HWND h = g_help_wnd;

    if (!h)
        return;
    g_help_wnd = NULL;
    DestroyWindow(h);
    app_log(L"help: closed");
}

void help_toggle(void)
{
    if (g_help_wnd)
        help_close();
    else
        help_open();
}
