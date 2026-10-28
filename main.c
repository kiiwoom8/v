// main.c - entry point, single instance, DPI awareness, main window, message
// routing, settings file watcher, logging helpers.

#ifndef _WIN32_IE
#define _WIN32_IE 0x0601
#endif
#include "common.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

HWND      g_main_wnd;
HINSTANCE g_hinst;
Config    g_cfg;

// ---------------------------------------------------------------------------
// Logging

static CRITICAL_SECTION g_log_cs;
static volatile LONG    g_log_state;
static WCHAR            g_log_path[APP_PATH_MAX];
static BOOL             g_log_path_ready;

static void ensure_log_cs(void)
{
    LONG state;

    for (;;) {
        state = InterlockedCompareExchange(&g_log_state, 1, 0);
        if (state == 0) {
            InitializeCriticalSection(&g_log_cs);
            InterlockedExchange(&g_log_state, 2);
            return;
        }
        if (state == 2)
            return;
        Sleep(0);
    }
}

static void append_log_line(const WCHAR *line)
{
    WCHAR        text[1200];
    char         utf8[4096];
    const WCHAR *ini;
    const WCHAR *slash;
    DWORD        n;
    int          len;
    HANDLE       f;

    // The log lives next to the settings file actually in use.
    if (!g_log_path_ready) {
        ini = config_path();
        slash = wcsrchr(ini, L'\\');
        if (slash == NULL)
            return;
        n = (DWORD)(slash - ini) + 1;
        if (n + 7 > APP_PATH_MAX)
            return;
        lstrcpynW(g_log_path, ini, (int)n + 1);
        lstrcpynW(g_log_path + n, L"vim.log", (int)(APP_PATH_MAX - n));
        g_log_path_ready = TRUE;
    }

    _snwprintf(text, 1199, L"%s\r\n", line);
    text[1199] = L'\0';
    len = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, (int)sizeof(utf8),
                              NULL, NULL);
    if (len <= 1)
        return;

    f = CreateFileW(g_log_path, FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    {
        DWORD wrote = 0;
        WriteFile(f, utf8, (DWORD)(len - 1), &wrote, NULL);
    }
    CloseHandle(f);
}

void app_log(const WCHAR *fmt, ...)
{
    WCHAR    msg[1024];
    WCHAR    line[1152];
    va_list  ap;
    SYSTEMTIME st;

    va_start(ap, fmt);
    _vsnwprintf(msg, 1023, fmt, ap);
    msg[1023] = L'\0';
    va_end(ap);

    GetLocalTime(&st);
    _snwprintf(line, 1151, L"[%02d:%02d:%02d] %s",
               (int)st.wHour, (int)st.wMinute, (int)st.wSecond, msg);
    line[1151] = L'\0';

    ensure_log_cs();
    EnterCriticalSection(&g_log_cs);
    OutputDebugStringW(line);
    OutputDebugStringW(L"\n");
    if (g_cfg.log)
        append_log_line(line);
    LeaveCriticalSection(&g_log_cs);
}

// ---------------------------------------------------------------------------
// DPI

typedef BOOL (WINAPI *SetDpiCtxFn)(HANDLE);
typedef HRESULT (WINAPI *SetDpiAwarenessFn)(int);
typedef BOOL (WINAPI *SetDpiAwareFn)(void);
typedef HRESULT (WINAPI *GetDpiForMonitorFn)(HMONITOR, int, UINT *, UINT *);

#define DPI_CTX_PER_MONITOR_V2 ((HANDLE)-4)
#define MDT_EFFECTIVE_DPI 0

// The manifest normally sets the awareness before this runs, so every call
// here fails with ERROR_ACCESS_DENIED / E_ACCESSDENIED. That is expected; this
// is only the fallback for an older host that ignores the manifest.
static void set_dpi_awareness(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    HMODULE shcore  = LoadLibraryW(L"shcore.dll");
    SetDpiCtxFn         ctx_fn = NULL;
    SetDpiAwarenessFn   awareness_fn = NULL;
    SetDpiAwareFn       aware_fn = NULL;

    if (user32) {
        ctx_fn = (SetDpiCtxFn)(void *)GetProcAddress(
            user32, "SetProcessDpiAwarenessContext");
        aware_fn = (SetDpiAwareFn)(void *)GetProcAddress(
            user32, "SetProcessDPIAware");
    }
    if (shcore) {
        awareness_fn = (SetDpiAwarenessFn)(void *)GetProcAddress(
            shcore, "SetProcessDpiAwareness");
    }

    if (ctx_fn && ctx_fn(DPI_CTX_PER_MONITOR_V2)) {
        if (shcore)
            FreeLibrary(shcore);
        return;
    }
    if (awareness_fn && SUCCEEDED(awareness_fn(2))) {
        if (shcore)
            FreeLibrary(shcore);
        return;
    }
    if (aware_fn)
        aware_fn();
    if (shcore)
        FreeLibrary(shcore);
}

static GetDpiForMonitorFn get_dpi_for_monitor_fn(void)
{
    static GetDpiForMonitorFn fn;
    static BOOL tried;

    if (!tried) {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore)
            fn = (GetDpiForMonitorFn)(void *)GetProcAddress(
                shcore, "GetDpiForMonitor");
        tried = TRUE;
    }
    return fn;
}

UINT dpi_for_monitor(HMONITOR mon)
{
    GetDpiForMonitorFn fn = get_dpi_for_monitor_fn();

    if (fn && mon) {
        UINT x = 0, y = 0;
        if (SUCCEEDED(fn(mon, MDT_EFFECTIVE_DPI, &x, &y)) && x != 0)
            return x;
    }
    {
        HDC  dc = GetDC(NULL);
        UINT dpi = 96;
        if (dc) {
            dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(NULL, dc);
        }
        return dpi ? dpi : 96;
    }
}

HICON app_load_icon(int cx, int cy)
{
    return (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(APP_ICON_ID),
                             IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
}

// ---------------------------------------------------------------------------
// Settings file watcher

static HANDLE g_watch_thread;
static HANDLE g_watch_stop;
static HANDLE g_watch_find;
static WCHAR  g_watch_dir[APP_PATH_MAX];
static WCHAR  g_watch_file[APP_PATH_MAX];

static BOOL read_write_time(FILETIME *ft)
{
    HANDLE h = CreateFileW(g_watch_file, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL ok;
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    ok = GetFileTime(h, NULL, NULL, ft);
    CloseHandle(h);
    return ok;
}

static BOOL same_time(const FILETIME *a, const FILETIME *b)
{
    return a->dwLowDateTime == b->dwLowDateTime &&
           a->dwHighDateTime == b->dwHighDateTime;
}

static DWORD WINAPI watch_thread_proc(LPVOID param)
{
    FILETIME last;
    HANDLE   handles[2];

    (void)param;
    ZeroMemory(&last, sizeof(last));
    read_write_time(&last);

    g_watch_find = FindFirstChangeNotificationW(
        g_watch_dir, FALSE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_SIZE);
    if (g_watch_find == INVALID_HANDLE_VALUE) {
        app_log(L"settings watcher: cannot watch %s", g_watch_dir);
        g_watch_find = NULL;
        return 0;
    }
    app_log(L"settings watcher: watching %s", g_watch_dir);

    handles[0] = g_watch_stop;
    handles[1] = g_watch_find;
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        FILETIME now;

        if (w == WAIT_OBJECT_0)
            break;
        if (w != WAIT_OBJECT_0 + 1)
            break;

        // Debounce: editors often produce a burst of writes.
        if (WaitForSingleObject(g_watch_stop, 300) == WAIT_OBJECT_0)
            break;

        if (read_write_time(&now) && !same_time(&last, &now)) {
            last = now;
            PostMessageW(g_main_wnd, WM_APP_RELOAD_CONFIG, 0, 0);
        }
        if (!FindNextChangeNotification(g_watch_find))
            break;
    }

    if (g_watch_find) {
        FindClose(g_watch_find);
        g_watch_find = NULL;
    }
    return 0;
}

static void start_watch_thread(void)
{
    WCHAR *slash;

    lstrcpynW(g_watch_file, config_path(), APP_PATH_MAX);
    lstrcpynW(g_watch_dir, g_watch_file, APP_PATH_MAX);
    slash = wcsrchr(g_watch_dir, L'\\');
    if (slash)
        *slash = L'\0';

    g_watch_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_watch_stop)
        return;
    g_watch_thread = CreateThread(NULL, 0, watch_thread_proc, NULL, 0, NULL);
    if (!g_watch_thread) {
        CloseHandle(g_watch_stop);
        g_watch_stop = NULL;
    }
}

static void stop_watch_thread(void)
{
    if (g_watch_stop)
        SetEvent(g_watch_stop);
    if (g_watch_thread) {
        WaitForSingleObject(g_watch_thread, 3000);
        CloseHandle(g_watch_thread);
        g_watch_thread = NULL;
    }
    if (g_watch_stop) {
        CloseHandle(g_watch_stop);
        g_watch_stop = NULL;
    }
}

// ---------------------------------------------------------------------------
// Public app helpers

// When we rewrote vim.ini ourselves (text shortcut editor); the watcher's
// reload that follows stays quiet.
static DWORD g_self_write_tick;
static BOOL  g_self_write;

void app_config_written(void)
{
    g_self_write_tick = GetTickCount();
    g_self_write = TRUE;
    app_reload_config(FALSE);
}

void app_reload_config(BOOL notify)
{
    Config tmp;
    BOOL   created = FALSE;

    if (!config_load(&tmp, &created)) {
        const WCHAR *w = config_warnings();
        app_log(L"config: reload failed");
        if (notify)
            tray_balloon(L"Settings problem",
                         (w && w[0]) ? w
                                     : L"Could not read the settings file.");
        return;
    }

    g_cfg = tmp;
    input_config_changed();
    tray_update();
    app_log(L"config: reloaded");

    if (notify) {
        const WCHAR *w = config_warnings();
        if (w && w[0])
            tray_balloon(L"Settings problem", w);
        else
            tray_balloon(L"Settings reloaded", L"Changes apply immediately.");
    }
}

void app_quit(void)
{
    if (g_main_wnd)
        DestroyWindow(g_main_wnd);
    else
        PostQuitMessage(0);
}

// ---------------------------------------------------------------------------
// Main window

static UINT g_taskbar_created;

static LRESULT CALLBACK main_wnd_proc(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp)
{
    if (g_taskbar_created != 0 && msg == g_taskbar_created) {
        tray_on_taskbar_created();
        return 0;
    }

    switch (msg) {
    case WM_APP_HINT_OPEN:
        hint_open();
        return 0;
    case WM_APP_HINT_PREPARE:
        hint_prepare(wp != 0);
        return 0;
    case WM_APP_HINT_CLOSE:
        hint_close();
        return 0;
    case WM_APP_HINT_KEY:
        hint_on_key((UINT)wp, (UINT)lp);
        return 0;
    case WM_APP_HINT_AGAIN:
        hint_hotkey_again();
        return 0;
    case WM_APP_ELEMENTS:
        hint_on_elements((ClickList *)lp);
        return 0;
    case WM_APP_SHOW_HELP:
        help_toggle();
        return 0;
    case WM_APP_STATE_CHANGED:
        tray_update();
        return 0;
    case WM_APP_QUIT:
        app_quit();
        return 0;
    case WM_APP_TRAY:
        tray_on_message(wp, lp);
        return 0;
    case WM_COMMAND:
        if (tray_on_command((WORD)LOWORD(wp)))
            return 0;
        break;
    case WM_APP_ALREADY_RUNNING:
        tray_balloon(L"Already running",
                     L"vim mouse is already running.\n"
                     L"Right-click the tray icon for the menu.");
        help_toggle();
        return 0;
    case WM_APP_RELOAD_CONFIG:
        app_reload_config(!(g_self_write &&
                            GetTickCount() - g_self_write_tick < 3000));
        return 0;
    case WM_APP_TYPE_TEXT:
        snippet_insert((const WCHAR *)lp);
        free((void *)lp);
        return 0;
    case WM_CLOSE:
        app_quit();
        return 0;
    case WM_DESTROY:
        stop_watch_thread();
        snippet_editor_close();
        hint_shutdown();
        // Stop input first: its movement thread calls into UIA until joined.
        input_stop();
        uia_stop();
        tray_shutdown();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(void)
{
    MSG           msg;
    HANDLE        mutex;
    BOOL          created = FALSE;
    WNDCLASSEXW   wc;
    HWND          existing;

    g_hinst = GetModuleHandleW(NULL);

    mutex = CreateMutexW(NULL, TRUE, INSTANCE_MUTEX);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        existing = FindWindowW(MAIN_WND_CLASS, NULL);
        if (existing)
            PostMessageW(existing, WM_APP_ALREADY_RUNNING, 0, 0);
        CloseHandle(mutex);
        return 0;
    }

    set_dpi_awareness();

    // Pick the settings folder once, before any worker thread can ask for it.
    config_path();

    if (!config_load(&g_cfg, &created))
        app_log(L"config: initial load failed");
    app_log(L"config: settings file %s", config_path());

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = main_wnd_proc;
    wc.hInstance     = g_hinst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon         = app_load_icon(GetSystemMetrics(SM_CXICON),
                                     GetSystemMetrics(SM_CYICON));
    wc.hIconSm       = app_load_icon(GetSystemMetrics(SM_CXSMICON),
                                     GetSystemMetrics(SM_CYSMICON));
    wc.lpszClassName = MAIN_WND_CLASS;
    if (!RegisterClassExW(&wc)) {
        app_log(L"main: RegisterClassEx failed (%lu)", GetLastError());
        if (mutex)
            CloseHandle(mutex);
        return 1;
    }

    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    g_main_wnd = CreateWindowExW(0, MAIN_WND_CLASS, APP_NAME,
                                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                 NULL, NULL, g_hinst, NULL);
    if (!g_main_wnd) {
        app_log(L"main: CreateWindowEx failed (%lu)", GetLastError());
        if (mutex)
            CloseHandle(mutex);
        return 1;
    }

    hint_init(g_hinst);
    tray_init(g_main_wnd);

    if (!input_start()) {
        MessageBoxW(NULL,
                    L"The keyboard hook could not be installed, so vim mouse "
                    L"cannot work.\n\nAnother program may be blocking it. Try "
                    L"again, or restart Windows.",
                    APP_NAME, MB_OK | MB_ICONERROR);
        app_log(L"input: keyboard hook could not be installed");
        DestroyWindow(g_main_wnd);
        if (mutex)
            CloseHandle(mutex);
        return 1;
    }

    // Without UI Automation the hints simply fall back to the grid.
    if (!uia_start())
        app_log(L"uia: UI Automation unavailable, element hints disabled");

    start_watch_thread();

    if (created) {
        WCHAR hint[64], mode[64], text[512];
        format_hotkey(&g_cfg.hint_hotkey, hint, 64);
        format_key(g_cfg.mouse_mode_key, mode, 64);
        _snwprintf(text, 511,
                   L"%s: click hints \u00b7 %s: mouse mode \u00b7 "
                   L"Right-click this icon for settings",
                   hint, mode);
        text[511] = L'\0';
        tray_balloon(L"vim mouse is running", text);
    }
    if (config_warnings()[0])
        tray_balloon(L"Settings problem", config_warnings());

    app_log(L"main: started");

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (snippet_editor_dialog_message(&msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mutex)
        CloseHandle(mutex);
    return (int)msg.wParam;
}
