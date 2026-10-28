// tray.c - tray icon, tray menu, help window, start-with-Windows.

#ifndef _WIN32_IE
#define _WIN32_IE 0x0601
#endif
#include "common.h"
#include "version.h"

#include <basetyps.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define TRAY_ICON_ID   1
#define HELP_CLASS     L"VimMouseHelpWindow"

static const WCHAR HELP_TITLE[] =
    L"vim mouse " VIM_VERSION_STR L" - keyboard shortcuts";

#define IDM_HINTS      1001
#define IDM_MOUSE      1002
#define IDM_PAUSE      1003
#define IDM_SHORTCUTS  1004
#define IDM_EDIT       1005
#define IDM_RELOAD     1006
#define IDM_STARTUP    1007
#define IDM_QUIT       1008
#define IDM_TEXT       1009
#define IDM_LOCK       1010

#define HELP_MAX_ROWS  64
#define HELP_INF       0x3fffffff

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
    int          x;
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
static void startup_repair(void);

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
    if (input_is_paused() || input_is_locked())
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
               hint, mode,
               input_is_locked() ? L" (keyboard locked)" :
               input_is_paused() ? L" (paused)" : L"");
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
    startup_repair();
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

    // Say so whenever the keyboard lock changes: a locked keyboard looks broken.
    {
        static BOOL was_locked;
        BOOL locked = input_is_locked();
        if (locked != was_locked) {
            WCHAR hk[64], text[160];
            was_locked = locked;
            format_hotkey(&g_cfg.lock_hotkey, hk, 64);
            if (locked)
                _snwprintf(text, 159, L"Press %s again (or use the tray menu) "
                           L"to unlock.", hk);
            else
                _snwprintf(text, 159, L"Typing works again.");
            text[159] = L'\0';
            tray_balloon(locked ? L"Keyboard locked" : L"Keyboard unlocked",
                         text);
        }
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

#define RUN_VALUE_MAX 2048

static BOOL current_exe(WCHAR *buf, int cch)
{
    DWORD n = GetModuleFileNameW(NULL, buf, (DWORD)cch);

    return n != 0 && n < (DWORD)cch;
}

static BOOL quoted_exe(WCHAR *buf, int cch)
{
    WCHAR path[MAX_PATH];

    if (cch <= 0 || !current_exe(path, MAX_PATH))
        return FALSE;
    _snwprintf(buf, cch - 1, L"\"%s\"", path);
    buf[cch - 1] = L'\0';
    return TRUE;
}

// Extracts the program path from a Run value: "C:\path\vim.exe" [args], or an
// unquoted path. An unquoted value cannot contain spaces unambiguously, so it
// ends at the first one.
static BOOL run_value_path(const WCHAR *value, WCHAR *path, int cch)
{
    const WCHAR *p = value;
    int n = 0;

    while (*p == L' ' || *p == L'\t')
        p++;
    if (*p == L'"') {
        p++;
        while (p[n] && p[n] != L'"')
            n++;
    } else {
        while (p[n] && p[n] != L' ' && p[n] != L'\t')
            n++;
    }
    if (n == 0 || cch <= 0)
        return FALSE;
    if (n >= cch)
        n = cch - 1;
    memcpy(path, p, (size_t)n * sizeof(WCHAR));
    path[n] = L'\0';
    return TRUE;
}

static BOOL startup_enabled(void)
{
    HKEY    key;
    WCHAR   exe[MAX_PATH];
    WCHAR   got[RUN_VALUE_MAX];
    WCHAR   path[RUN_VALUE_MAX];
    DWORD   type = 0, size = (DWORD)sizeof(got);
    LONG    rc;

    if (!current_exe(exe, MAX_PATH))
        return FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS)
        return FALSE;
    rc = RegQueryValueExW(key, APP_NAME, NULL, &type, (BYTE *)got, &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ)
        return FALSE;
    got[(sizeof(got) / sizeof(WCHAR)) - 1] = L'\0';
    if (!run_value_path(got, path, RUN_VALUE_MAX))
        return FALSE;
    return lstrcmpiW(path, exe) == 0;
}

// A moved vim.exe leaves the Run value pointing at a file that no longer
// exists, so Windows tries to start it at every login. Repair only that case:
// another existing exe may be a second copy that owns the value.
static void startup_repair(void)
{
    HKEY  key;
    WCHAR value[RUN_VALUE_MAX];
    WCHAR path[RUN_VALUE_MAX];
    WCHAR want[MAX_PATH + 4];
    DWORD type = 0, size = (DWORD)sizeof(value);
    LONG  rc;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    rc = RegQueryValueExW(key, APP_NAME, NULL, &type, (BYTE *)value, &size);
    if (rc != ERROR_SUCCESS || type != REG_SZ) {
        RegCloseKey(key);
        return;
    }
    value[(sizeof(value) / sizeof(WCHAR)) - 1] = L'\0';
    if (!run_value_path(value, path, RUN_VALUE_MAX) ||
        GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        RegCloseKey(key);
        return;
    }
    if (!quoted_exe(want, MAX_PATH + 4)) {
        RegCloseKey(key);
        return;
    }
    rc = RegSetValueExW(key, APP_NAME, 0, REG_SZ, (const BYTE *)want,
                        (DWORD)((wcslen(want) + 1) * sizeof(WCHAR)));
    RegCloseKey(key);
    app_log(L"startup: repaired stale Run value pointing at %s (%ld)", path,
            rc);
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
        static WCHAR args[APP_PATH_MAX + 4];
        _snwprintf(args, APP_PATH_MAX + 3, L"\"%s\"", path);
        args[APP_PATH_MAX + 3] = L'\0';
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

    format_hotkey(&g_cfg.lock_hotkey, hk, 64);
    append_item(menu, IDM_LOCK, L"Lock keyboard", hk, input_is_locked());

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    append_item(menu, IDM_SHORTCUTS, L"Keyboard shortcuts...", NULL, FALSE);
    append_item(menu, IDM_TEXT, L"Text shortcuts...", NULL, FALSE);
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
    case IDM_TEXT:
        snippet_editor_open();
        return TRUE;
    case IDM_LOCK:
        input_set_locked(!input_is_locked());
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
                    L"Open click hints (press again to rescan)");
    format_key(g_cfg.mouse_mode_key, mode, 64);
    _snwprintf(buf, 127, L"%s tap / hold", mode);
    buf[127] = L'\0';
    help_add_row(buf, L"Mouse mode on/off; hold for momentary");
    _snwprintf(buf, 127, L"Shift + %s", mode);
    buf[127] = L'\0';
    help_add_row(buf, L"Real CapsLock (passes through)");
    help_add_hotkey(&g_cfg.quit_hotkey, L"Quit vim mouse");
    if (g_cfg.lock_hotkey.vk)
        help_add_hotkey(&g_cfg.lock_hotkey,
                        L"Lock / unlock the keyboard (all keys ignored)");
    if (g_cfg.pause_hotkey.vk)
        help_add_hotkey(&g_cfg.pause_hotkey, L"Pause / resume everything");
    if (g_cfg.alt_scroll) {
        WCHAR k1[16], k2[16], k3[16], k4[16];
        format_key(g_cfg.key_left, k1, 16);
        format_key(g_cfg.key_down, k2, 16);
        format_key(g_cfg.key_up, k3, 16);
        format_key(g_cfg.key_right, k4, 16);
        _snwprintf(buf, 127, L"Alt + %s %s %s %s", k1, k2, k3, k4);
        buf[127] = L'\0';
        help_add_row(buf, L"Scroll left / down / up / right");
        format_key(g_cfg.key_scroll_up, k1, 16);
        format_key(g_cfg.key_scroll_down, k2, 16);
        _snwprintf(buf, 127, L"Alt + %s / %s", k1, k2);
        buf[127] = L'\0';
        help_add_row(buf, L"Fast scroll up / down");
        if (g_cfg.scroll_end_tap_ms > 0) {
            _snwprintf(buf, 127, L"Alt + %s %s / %s %s", k1, k1, k2, k2);
            buf[127] = L'\0';
            help_add_row(buf, L"Top / bottom (tap twice)");
        }
    }
    help_add_row(L"Alt + abbreviation",
                 L"Type a text shortcut (tray: Text shortcuts...)");

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
    help_add_row(L"type the label", L"Click the element showing it");
    help_add_row(L"Shift / Ctrl + label", L"Right click / double click");
    format_hotkey(&g_cfg.hint_hotkey, mode, 64);
    _snwprintf(buf, 127, L"%s again", mode);
    buf[127] = L'\0';
    help_add_row(buf, L"Rescan (Esc closes)");

    help_add_header(L"Grid");
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

static int scaled_px(int px, int dpi, int scale)
{
    return MulDiv(MulDiv(px, dpi, 96), scale, 100);
}

static BOOL help_make_fonts(int dpi, int scale)
{
    int title   = scaled_px(20, dpi, scale);
    int section = scaled_px(15, dpi, scale);
    int body    = scaled_px(13, dpi, scale);

    if (title < 1)
        title = 1;
    if (section < 1)
        section = 1;
    if (body < 1)
        body = 1;
    g_help_title_font = make_font(title, FW_BOLD);
    g_help_section_font = make_font(section, FW_BOLD);
    g_help_body_font = make_font(body, FW_NORMAL);
    if (!g_help_title_font || !g_help_section_font || !g_help_body_font) {
        help_delete_fonts();
        return FALSE;
    }
    return TRUE;
}

// Splits the contiguous sections into k columns, minimising the tallest column.
static int help_partition(const int *prefix, int count, int k,
                          int split[][HELP_MAX_ROWS + 1])
{
    int best[HELP_MAX_ROWS + 1][HELP_MAX_ROWS + 1];
    int i, j, p, sum, cand;

    for (j = 0; j <= k; j++) {
        for (i = 0; i <= count; i++) {
            best[j][i] = HELP_INF;
            split[j][i] = -1;
        }
    }
    best[0][0] = 0;
    for (j = 1; j <= k; j++) {
        for (i = 1; i <= count; i++) {
            for (p = j - 1; p < i; p++) {
                if (best[j - 1][p] >= HELP_INF)
                    continue;
                sum = prefix[i] - prefix[p];
                cand = best[j - 1][p] > sum ? best[j - 1][p] : sum;
                if (cand < best[j][i]) {
                    best[j][i] = cand;
                    split[j][i] = p;
                }
            }
        }
    }
    return best[k][count];
}

// Measures the rows and stores where each one goes, in one or more balanced
// columns. Returns FALSE only when strict and no layout fits the work area.
static BOOL help_layout(HDC dc, const RECT *work, int dpi, int scale,
                        BOOL strict, int *out_w, int *out_h)
{
    int      i, j, k, p;
    int      maxkey = 0, maxdesc = 0, titlew;
    int      pad, gap, line, title_line, section_line, sec_gap, footer_gap;
    int      colw, colgap, width, footer_h, avail, maxcol;
    int      workw, workh, w_limit, h_limit, y0, tail, bottom;
    int      sec_start[HELP_MAX_ROWS], sec_rows[HELP_MAX_ROWS];
    int      sec_h[HELP_MAX_ROWS], prefix[HELP_MAX_ROWS + 1];
    int      starts[HELP_MAX_ROWS], ends[HELP_MAX_ROWS];
    int      split[HELP_MAX_ROWS + 1][HELP_MAX_ROWS + 1];
    int      sec_count = 0, chosen_k = 0;
    BOOL     found = FALSE;
    WCHAR    footer[512];
    RECT     fr;
    HelpRow *r;

    pad          = scaled_px(20, dpi, scale);
    gap          = scaled_px(20, dpi, scale);
    line         = scaled_px(22, dpi, scale);
    title_line   = scaled_px(30, dpi, scale);
    section_line = scaled_px(26, dpi, scale);
    sec_gap      = scaled_px(10, dpi, scale);
    footer_gap   = scaled_px(12, dpi, scale);
    tail         = scaled_px(10, dpi, scale);
    if (line < 1)
        line = 1;
    if (title_line < 1)
        title_line = 1;
    if (section_line < 1)
        section_line = 1;

    titlew = text_width(dc, g_help_title_font, HELP_TITLE);
    for (i = 0; i < g_help_row_count; i++) {
        int w;
        r = &g_help_rows[i];
        if (r->section)
            continue;
        w = text_width(dc, g_help_body_font, r->key);
        if (w > maxkey)
            maxkey = w;
        w = text_width(dc, g_help_body_font, r->desc);
        if (w > maxdesc)
            maxdesc = w;
    }

    g_help_pad  = pad;
    g_help_gap  = gap;
    g_help_keyw = maxkey + scaled_px(28, dpi, scale);

    workw = (int)(work->right - work->left);
    workh = (int)(work->bottom - work->top);
    w_limit = workw - scaled_px(40, dpi, scale);
    h_limit = workh - scaled_px(40, dpi, scale);
    if (w_limit < 1)
        w_limit = workw;
    if (h_limit < 1)
        h_limit = workh;

    y0 = pad + title_line + scaled_px(4, dpi, scale);

    for (i = 0; i < g_help_row_count; i++) {
        if (g_help_rows[i].section) {
            if (sec_count >= HELP_MAX_ROWS)
                break;
            sec_start[sec_count] = i;
            sec_rows[sec_count] = 0;
            sec_count++;
        } else if (sec_count > 0) {
            sec_rows[sec_count - 1]++;
        }
    }
    prefix[0] = 0;
    for (i = 0; i < sec_count; i++) {
        sec_h[i] = (i ? sec_gap : 0) + section_line + sec_rows[i] * line;
        prefix[i + 1] = prefix[i] + sec_h[i];
    }

    _snwprintf(footer, 511, L"Settings: %s (saved changes apply immediately)",
               config_path());
    footer[511] = L'\0';

    colgap = pad;
    colw = g_help_keyw + gap + maxdesc;

    for (k = 1; k <= sec_count; k++) {
        width = pad * 2 + k * colw + (k - 1) * colgap;
        if (width < titlew + pad * 2)
            width = titlew + pad * 2;
        fr.left   = 0;
        fr.top    = 0;
        fr.right  = width - pad * 2;
        if (fr.right < 1)
            fr.right = 1;
        fr.bottom = 0;
        DrawTextW(dc, footer, -1, &fr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        footer_h = fr.bottom;
        avail = h_limit - y0 - footer_gap - footer_h - tail - line - pad;
        maxcol = help_partition(prefix, sec_count, k, split);
        if (maxcol < HELP_INF && maxcol <= avail &&
            (!strict || width <= w_limit)) {
            found = TRUE;
            chosen_k = k;
            break;
        }
    }

    if (!found) {
        if (strict)
            return FALSE;
        // Last resort: as many columns as sections, then clip to the work area.
        chosen_k = sec_count > 0 ? sec_count : 1;
        width = pad * 2 + chosen_k * colw + (chosen_k - 1) * colgap;
        if (width < titlew + pad * 2)
            width = titlew + pad * 2;
        fr.left   = 0;
        fr.top    = 0;
        fr.right  = width - pad * 2;
        if (fr.right < 1)
            fr.right = 1;
        fr.bottom = 0;
        DrawTextW(dc, footer, -1, &fr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        footer_h = fr.bottom;
        maxcol = help_partition(prefix, sec_count, chosen_k, split);
        if (maxcol >= HELP_INF)
            maxcol = 0;
    }

    p = sec_count;
    for (k = chosen_k; k >= 1; k--) {
        int start = split[k][p];
        if (start < 0)
            start = 0;
        starts[k - 1] = start;
        ends[k - 1] = p;
        p = start;
    }
    for (k = 0; k < chosen_k; k++) {
        int col_left = pad + k * (colw + colgap);
        int y = y0;
        for (j = starts[k]; j < ends[k]; j++) {
            int row;
            if (j != starts[k])
                y += sec_gap;
            r = &g_help_rows[sec_start[j]];
            r->x = col_left;
            r->y = y;
            y += section_line;
            for (row = sec_start[j] + 1;
                 row <= sec_start[j] + sec_rows[j]; row++) {
                g_help_rows[row].x = col_left;
                g_help_rows[row].y = y;
                y += line;
            }
        }
    }

    bottom = y0 + maxcol;
    g_help_footer_y = bottom + footer_gap;
    g_help_footer_h = footer_h;
    g_help_hint_y   = g_help_footer_y + footer_h + tail;
    *out_h = g_help_hint_y + line + pad;
    *out_w = width;

    if (!strict) {
        if (*out_w > w_limit)
            *out_w = w_limit;
        if (*out_h > h_limit)
            *out_h = h_limit;
    }
    return TRUE;
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
    TextOutW(mem, g_help_pad, g_help_pad, HELP_TITLE,
             (int)wcslen(HELP_TITLE));

    for (i = 0; i < g_help_row_count; i++) {
        HelpRow *r = &g_help_rows[i];
        if (r->section) {
            SelectObject(mem, g_help_section_font);
            SetTextColor(mem, RGB(154, 160, 166));
            TextOutW(mem, r->x, r->y, r->section,
                     (int)wcslen(r->section));
        } else {
            SelectObject(mem, g_help_body_font);
            SetTextColor(mem, RGB(77, 163, 255));
            TextOutW(mem, r->x, r->y, r->key, (int)wcslen(r->key));
            SetTextColor(mem, RGB(221, 221, 221));
            TextOutW(mem, r->x + g_help_keyw + g_help_gap, r->y,
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
    wc.hIcon         = app_load_icon(GetSystemMetrics(SM_CXICON),
                                     GetSystemMetrics(SM_CYICON));
    wc.hIconSm       = app_load_icon(GetSystemMetrics(SM_CXSMICON),
                                     GetSystemMetrics(SM_CYSMICON));
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
    HFONT       old;
    RECT        work;
    int         scale, width = 0, height = 0, x, yy;
    BOOL        fit = FALSE;

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

    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi))
        work = mi.rcWork;
    else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    for (scale = 100; scale >= 50; scale -= 5) {
        if (!help_make_fonts((int)dpi, scale))
            return;
        dc = GetDC(NULL);
        old = (HFONT)SelectObject(dc, g_help_body_font);
        fit = help_layout(dc, &work, (int)dpi, scale, TRUE, &width, &height);
        SelectObject(dc, old);
        ReleaseDC(NULL, dc);
        if (fit)
            break;
        help_delete_fonts();
    }
    if (!fit) {
        scale = 50;
        if (!help_make_fonts((int)dpi, scale))
            return;
        dc = GetDC(NULL);
        old = (HFONT)SelectObject(dc, g_help_body_font);
        help_layout(dc, &work, (int)dpi, scale, FALSE, &width, &height);
        SelectObject(dc, old);
        ReleaseDC(NULL, dc);
    }

    if (work.right > work.left && work.bottom > work.top) {
        x = (int)work.left + ((int)(work.right - work.left) - width) / 2;
        yy = (int)work.top + ((int)(work.bottom - work.top) - height) / 2;
    } else {
        x = (int)work.left;
        yy = (int)work.top;
    }
    if (x + width > work.right)
        x = (int)work.right - width;
    if (yy + height > work.bottom)
        yy = (int)work.bottom - height;
    if (x < work.left)
        x = (int)work.left;
    if (yy < work.top)
        yy = (int)work.top;

    g_help_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                 HELP_CLASS, HELP_TITLE,
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
