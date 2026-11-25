// main.c - entry point, single instance, DPI awareness, main window, message
// routing, settings file watcher, logging helpers.

#ifndef _WIN32_IE
#define _WIN32_IE 0x0601
#endif
#include "common.h"

#include <basetyps.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>
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

// "[hh:mm:ss] msg" into line (1152 characters); log = 2 is for timing, so
// its lines carry the milliseconds too.
static void stamp_line(WCHAR *line, const WCHAR *msg)
{
    SYSTEMTIME st;

    GetLocalTime(&st);
    if (g_cfg.log >= 2)
        _snwprintf(line, 1151, L"[%02d:%02d:%02d.%03d] %s",
                   (int)st.wHour, (int)st.wMinute, (int)st.wSecond,
                   (int)st.wMilliseconds, msg);
    else
        _snwprintf(line, 1151, L"[%02d:%02d:%02d] %s",
                   (int)st.wHour, (int)st.wMinute, (int)st.wSecond, msg);
    line[1151] = L'\0';
}

void app_log(const WCHAR *fmt, ...)
{
    WCHAR    msg[1024];
    WCHAR    line[1152];
    va_list  ap;

    // Nobody reads it: no file (log = 0) and no debugger. Formatting the
    // line and OutputDebugString (an exception raised and caught, then a
    // look for a viewer) cost the keyboard hook and the scan threads time
    // on every line otherwise.
    if (!g_cfg.log && !IsDebuggerPresent())
        return;

    va_start(ap, fmt);
    _vsnwprintf(msg, 1023, fmt, ap);
    msg[1023] = L'\0';
    va_end(ap);

    stamp_line(line, msg);

    ensure_log_cs();
    EnterCriticalSection(&g_log_cs);
    OutputDebugStringW(line);
    OutputDebugStringW(L"\n");
    if (g_cfg.log)
        append_log_line(line);
    LeaveCriticalSection(&g_log_cs);
}

// A line vim.log gets whatever log says: how the last run ended (rare, and
// the only trace of it).
static void log_always(const WCHAR *fmt, ...)
{
    WCHAR    msg[1024];
    WCHAR    line[1152];
    va_list  ap;

    va_start(ap, fmt);
    _vsnwprintf(msg, 1023, fmt, ap);
    msg[1023] = L'\0';
    va_end(ap);

    stamp_line(line, msg);

    ensure_log_cs();
    EnterCriticalSection(&g_log_cs);
    append_log_line(line);
    LeaveCriticalSection(&g_log_cs);
}

// ---------------------------------------------------------------------------
// Leaving the job of the program that started us
//
// A program can keep the processes it starts in a job that Windows ends
// with it (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE): terminals, editors and the
// Claude desktop app do. vim mouse started from one of them then vanished
// whenever that program closed, restarted or updated itself: no quit, no
// crash report, the pointer left hidden and the touchpad off (measured
// 2026-11-16: every vim.exe a session had restarted with Start-Process sat
// in the app's kill-on-close job; one started by Explorer did not). So when
// something other than Explorer starts us inside a job that lets processes
// leave, we start ourselves again outside it (CREATE_BREAKAWAY_FROM_JOB,
// which leaves every nested job up to the first that forbids it) and the
// first process ends. NOJOB_ARG marks the second start, and given by hand
// keeps vim mouse in the job.

#ifndef CREATE_BREAKAWAY_FROM_JOB
#define CREATE_BREAKAWAY_FROM_JOB 0x01000000
#endif
#ifndef JOB_OBJECT_LIMIT_BREAKAWAY_OK
#define JOB_OBJECT_LIMIT_BREAKAWAY_OK 0x00000800
#endif
#ifndef JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK
#define JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK 0x00001000
#endif

#define NOJOB_ARG L"--nojob"

// MinGW.org's kernel32 import library has no IsProcessInJob.
typedef BOOL (WINAPI *InJobFn)(HANDLE, HANDLE, PBOOL);

static BOOL has_arg(const WCHAR *arg)
{
    int     argc = 0, i;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    BOOL    found = FALSE;

    if (!argv)
        return FALSE;
    for (i = 1; i < argc; i++)
        if (lstrcmpiW(argv[i], arg) == 0)
            found = TRUE;
    LocalFree(argv);
    return found;
}

// Explorer started us: a double click, the Start menu, start with Windows.
// Its job ends nothing. When in doubt TRUE: then nothing is done.
static BOOL started_by_explorer(void)
{
    HANDLE          snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    DWORD           me = GetCurrentProcessId();
    DWORD           parent = 0;
    BOOL            explorer = FALSE;

    if (snap == INVALID_HANDLE_VALUE)
        return TRUE;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == me) {
                parent = pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    // A parent that is gone already was a launcher (cmd /c start): not Explorer.
    pe.dwSize = sizeof(pe);
    if (parent && Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == parent) {
                explorer = lstrcmpiW(pe.szExeFile, L"explorer.exe") == 0;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return explorer;
}

// TRUE when a copy of this process now runs outside the starter's job; the
// caller then ends without doing anything else.
static BOOL leave_starters_job(void)
{
    static WCHAR exe[APP_PATH_MAX];
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    STARTUPINFOW        si;
    PROCESS_INFORMATION pi;
    InJobFn      in_job_fn;
    BOOL         in_job = FALSE;
    const WCHAR *args;
    WCHAR       *cmd;
    size_t       cch;
    DWORD        flags, n;
    BOOL         ok;

    if (has_arg(NOJOB_ARG))
        return FALSE;
    in_job_fn = (InJobFn)(void *)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "IsProcessInJob");
    if (!in_job_fn || !in_job_fn(GetCurrentProcess(), NULL, &in_job) || !in_job)
        return FALSE;
    ZeroMemory(&li, sizeof(li));
    if (!QueryInformationJobObject(NULL, JobObjectExtendedLimitInformation,
                                   &li, sizeof(li), NULL))
        return FALSE;
    flags = li.BasicLimitInformation.LimitFlags;
    if (!(flags & (JOB_OBJECT_LIMIT_BREAKAWAY_OK |
                   JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)))
        return FALSE;
    if (started_by_explorer())
        return FALSE;

    n = GetModuleFileNameW(NULL, exe, APP_PATH_MAX);
    if (n == 0 || n >= APP_PATH_MAX)
        return FALSE;
    // The same arguments (--updated among them) plus the mark.
    args = GetCommandLineW();
    cch = wcslen(args) + wcslen(NOJOB_ARG) + 2;
    cmd = (WCHAR *)malloc(cch * sizeof(WCHAR));
    if (!cmd)
        return FALSE;
    _snwprintf(cmd, cch, L"%s %s", args, NOJOB_ARG);
    cmd[cch - 1] = L'\0';

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    // A job with silent breakaway lets every child out by itself, and takes
    // the flag only if it allows the asked-for kind too.
    ok = CreateProcessW(exe, cmd, NULL, NULL, FALSE,
                        (flags & JOB_OBJECT_LIMIT_BREAKAWAY_OK)
                            ? CREATE_BREAKAWAY_FROM_JOB : 0,
                        NULL, NULL, &si, &pi);
    free(cmd);
    if (!ok)
        return FALSE;   // (the UIAccess build may not start itself) stay here
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

// ---------------------------------------------------------------------------
// The run record: how the last run ended
//
// "<settings>.run" next to the settings file exists while vim mouse runs;
// quitting (and Windows ending the session) removes it. One found at
// startup, written since Windows last started, means the last run ended
// without quitting: it crashed (the crash filter below added where), or
// Windows or another program ended it (the job above, a kill from Task
// Manager). Either way nothing could be said at the time, so the next start
// says it: a line in vim.log whatever log is, and a balloon.

static WCHAR g_run_path[APP_PATH_MAX];
static char  g_run_text[512];        // what the record holds for this run
static int   g_run_len;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter;
static volatile LONG g_crash_once;

typedef ULONGLONG (WINAPI *Tick64Fn)(void);

// When Windows started, in seconds since 1970: the same for the whole time
// it has been up (sleep included), so it tells this start of Windows from
// the one the record was written in.
static unsigned long boot_seconds(void)
{
    Tick64Fn       tick64 = (Tick64Fn)(void *)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "GetTickCount64");
    ULONGLONG      up_ms = tick64 ? tick64() : (ULONGLONG)GetTickCount();
    FILETIME       ft;
    ULARGE_INTEGER now;

    GetSystemTimeAsFileTime(&ft);
    now.LowPart = ft.dwLowDateTime;
    now.HighPart = ft.dwHighDateTime;
    // 100 ns units since 1601; 11644473600 s lie between 1601 and 1970.
    return (unsigned long)(now.QuadPart / 10000000ULL - 11644473600ULL -
                           up_ms / 1000ULL);
}

static BOOL run_path_ready(void)
{
    const WCHAR *ini;
    const WCHAR *dot, *slash;
    size_t       stem;

    if (g_run_path[0])
        return TRUE;
    ini = config_path();
    dot = wcsrchr(ini, L'.');
    slash = wcsrchr(ini, L'\\');
    stem = (dot && (!slash || dot > slash)) ? (size_t)(dot - ini) : wcslen(ini);
    if (stem == 0 || stem + 5 > APP_PATH_MAX)
        return FALSE;
    memcpy(g_run_path, ini, stem * sizeof(WCHAR));
    lstrcpynW(g_run_path + stem, L".run", 5);
    return TRUE;
}

static void run_record_write(const char *crash)
{
    HANDLE f;
    DWORD  wrote = 0;

    if (!g_run_path[0])
        return;
    f = CreateFileW(g_run_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    WriteFile(f, g_run_text, (DWORD)g_run_len, &wrote, NULL);
    if (crash)
        WriteFile(f, crash, (DWORD)strlen(crash), &wrote, NULL);
    CloseHandle(f);
}

static void run_record_clear(void)
{
    if (g_run_path[0])
        DeleteFileW(g_run_path);
}

// The value of "key=" in the record's text, up to the end of its line.
static BOOL run_value(const char *text, const char *key, char *out, int cch)
{
    const char *p = strstr(text, key);
    int         i = 0;

    if (!p)
        return FALSE;
    p += strlen(key);
    while (*p && *p != '\r' && *p != '\n' && i < cch - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return TRUE;
}

// Startup, once the instance mutex is ours (so the last run is over): says
// how it ended if it did not quit, then starts this run's record.
static void run_record_start(void)
{
    char       old[512];
    char       boot[32], since[64], crash[200];
    WCHAR      text[256];
    HANDLE     f;
    DWORD      got = 0;
    SYSTEMTIME st;

    if (!run_path_ready())
        return;
    f = CreateFileW(g_run_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        if (!ReadFile(f, old, sizeof(old) - 1, &got, NULL))
            got = 0;
        CloseHandle(f);
        old[got] = '\0';
        // A record from before Windows last started ended with Windows (a
        // power cut, a forced shutdown): nothing to say.
        if (run_value(old, "boot=", boot, 32) &&
            run_value(old, "since=", since, 64)) {
            unsigned long then = strtoul(boot, NULL, 10);
            unsigned long now = boot_seconds();
            unsigned long apart = then > now ? then - now : now - then;

            if (apart <= 30 && run_value(old, "crash=", crash, 200)) {
                log_always(L"main: the last run (since %hs) crashed: %hs",
                           since, crash);
                _snwprintf(text, 255, L"%hs. The line is in vim.log, next "
                           L"to the settings file.", crash);
                text[255] = L'\0';
                tray_balloon(L"vim mouse crashed last time", text);
            } else if (apart <= 30) {
                log_always(L"main: the last run (since %hs) was ended from "
                           L"outside: it did not quit and did not crash",
                           since);
                _snwprintf(text, 255, L"It was running since %hs and did "
                           L"not quit or crash: Windows or another program "
                           L"ended it. The pointer and the touchpad are as "
                           L"they should be again.", since);
                text[255] = L'\0';
                tray_balloon(L"vim mouse was ended last time", text);
            }
        }
    }

    GetLocalTime(&st);
    g_run_len = _snprintf(g_run_text, sizeof(g_run_text) - 1,
                          "boot=%lu\r\nsince=%02d:%02d on %04d-%02d-%02d\r\n",
                          boot_seconds(), (int)st.wHour, (int)st.wMinute,
                          (int)st.wYear, (int)st.wMonth, (int)st.wDay);
    if (g_run_len < 0)
        g_run_len = 0;
    g_run_text[g_run_len] = '\0';
    run_record_write(NULL);
}

// What crashed, for the helper thread below.
static DWORD g_crash_code;
static void *g_crash_addr;

// A thread of its own, so a stack overflow's handler needs no stack of the
// thread that ran out of it: the pointer comes back (the blank cursors
// would outlive us), and the record and vim.log get the place.
static DWORD WINAPI crash_note_thread(LPVOID param)
{
    static WCHAR path[MAX_PATH];
    static char  note[200];
    static WCHAR line[400];
    MEMORY_BASIC_INFORMATION mbi;
    const WCHAR *name = L"?";
    ULONG_PTR    offset = (ULONG_PTR)g_crash_addr;
    SYSTEMTIME   st;

    (void)param;
    SystemParametersInfoW(SPI_SETCURSORS, 0, NULL, 0);

    if (VirtualQuery(g_crash_addr, &mbi, sizeof(mbi)) && mbi.AllocationBase &&
        GetModuleFileNameW((HMODULE)mbi.AllocationBase, path, MAX_PATH)) {
        const WCHAR *slash = wcsrchr(path, L'\\');

        path[MAX_PATH - 1] = L'\0';
        name = slash ? slash + 1 : path;
        offset = (ULONG_PTR)g_crash_addr - (ULONG_PTR)mbi.AllocationBase;
    }
    _snprintf(note, sizeof(note) - 1, "crash=exception %08lX at %ls+0x%lX\r\n",
              (unsigned long)g_crash_code, name, (unsigned long)offset);
    note[sizeof(note) - 1] = '\0';
    run_record_write(note);

    GetLocalTime(&st);
    _snwprintf(line, 399, L"[%02d:%02d:%02d] main: crashed: exception %08lX "
               L"at %ls+0x%lX (thread %lu), built " __DATE__ L" " __TIME__,
               (int)st.wHour, (int)st.wMinute, (int)st.wSecond,
               (unsigned long)g_crash_code, name, (unsigned long)offset,
               (unsigned long)(ULONG_PTR)param);
    line[399] = L'\0';
    // Without the log lock: the thread that crashed may hold it.
    append_log_line(line);
    return 0;
}

// Runs when nothing handled an exception, before Windows reports it. The
// crash goes on to Windows Error Reporting as before.
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    if (ep && ep->ExceptionRecord &&
        InterlockedExchange(&g_crash_once, 1) == 0) {
        HANDLE t;

        g_crash_code = ep->ExceptionRecord->ExceptionCode;
        g_crash_addr = ep->ExceptionRecord->ExceptionAddress;
        t = CreateThread(NULL, 0, crash_note_thread,
                         (LPVOID)(ULONG_PTR)GetCurrentThreadId(), 0, NULL);
        if (t) {
            // A thread that cannot start (the crash holds the loader lock)
            // is not waited for long.
            WaitForSingleObject(t, 2000);
            CloseHandle(t);
        }
    }
    return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
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
    snippet_editor_refresh();
    input_config_changed();
    shutdown_config_changed();
    secretary_config_changed();
    tray_update();
    touchpad_update();
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
        secretary_dismiss();
        secretary_hint_reaction();
        hint_open();
        return 0;
    case WM_APP_HINT_PREPARE:
        hint_prepare((int)wp);
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
    case WM_APP_PAGE_ACTED:
        hint_page_acted();
        return 0;
    case WM_APP_FLUSH_DONE:
        hint_flush_done(wp != 0, (UINT)lp);
        return 0;
    case WM_APP_ELEMENTS:
        hint_on_elements((ClickList *)lp);
        return 0;
    case WM_APP_SHOW_HELP:
        help_toggle();
        return 0;
    case WM_APP_STATE_CHANGED:
        secretary_command(SECRETARY_MODE);
        if (input_is_locked())
            secretary_dismiss();
        tray_update();
        touchpad_update();
        return 0;
    case WM_APP_TOUCHPAD_WAKE:
        touchpad_wake();
        return 0;
    case WM_APP_TOUCHPAD_TOGGLE:
        touchpad_toggle();
        return 0;
    case WM_APP_QUIT:
        app_log(L"main: quitting (the quit hotkey)");
        if (!secretary_farewell()) app_quit();
        return 0;
    case WM_TIMER:
        if (wp == SECRETARY_FAREWELL_TIMER) {
            KillTimer(hwnd, SECRETARY_FAREWELL_TIMER);
            app_quit();
            return 0;
        }
        break;
    case WM_APP_TRAY:
        tray_on_message(wp, lp);
        return 0;
    case WM_APP_SECRETARY_MENU:
        tray_show_menu((POINT){(LONG)wp, (LONG)lp});
        return 0;
    case WM_APP_SECRETARY_TAB:
        secretary_browser_result((UINT)wp, (HWND)lp);
        return 0;
    case WM_APP_SECRETARY_PAGE:
        secretary_page_result((UINT)wp, (BrowserPage *)lp);
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
    case WM_APP_UPDATE_DONE:
        update_on_done(wp);
        return 0;
    case WM_TIMECHANGE:
        shutdown_time_changed();
        return 0;
    case WM_CLOSE:
        // The window is never shown: this is another program asking.
        app_log(L"main: quitting (asked to close by another program)");
        app_quit();
        return 0;
    case WM_ENDSESSION:
        // Signing out or shutting down ends this run as it should; without
        // this the next start would say it was ended from outside.
        if (wp) {
            app_log(L"main: Windows is ending the session");
            run_record_clear();
        }
        return 0;
    case WM_DESTROY:
        stop_watch_thread();
        snippet_editor_close();
        secretary_editor_close();
        shutdown_stop();
        hint_shutdown();
        // Stop input first: its movement thread calls into UIA until joined.
        input_stop();
        secretary_shutdown();
        uia_stop();
        // After input_stop: the wait for the quit hotkey's keys blocks no hook.
        touchpad_stop();
        tray_shutdown();
        run_record_clear();
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

    // Started inside the job of a program that ends its children with
    // itself: the copy outside it carries on, this one is done.
    if (leave_starters_job())
        return 0;

    // After an update the new exe starts while the old instance is still
    // quitting; update_wait_for_old waits for it to let go of the mutex.
    mutex = CreateMutexW(NULL, TRUE, INSTANCE_MUTEX);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS &&
        !update_wait_for_old(mutex)) {
        existing = FindWindowW(MAIN_WND_CLASS, NULL);
        if (existing)
            PostMessageW(existing, WM_APP_ALREADY_RUNNING, 0, 0);
        CloseHandle(mutex);
        return 0;
    }

    set_dpi_awareness();

    // Pick the settings folder once, before any worker thread can ask for it.
    config_path();
    g_prev_filter = SetUnhandledExceptionFilter(crash_filter);

    if (!config_load(&g_cfg, &created))
        app_log(L"config: initial load failed");
    app_log(L"config: settings file %s", config_path());
    app_log(L"uiaccess: %s", app_has_uiaccess()
                ? L"on (the overlay may be drawn above the Start menu, "
                  L"Search, the notification centre and banners)"
                : L"off (labels inside the Start menu, Search, the "
                  L"notification centre and banners stay hidden under "
                  L"them; see README)");

    update_cleanup();

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
    // After the tray: how the last run ended may need a balloon.
    run_record_start();

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
    secretary_init();

    // Without UI Automation only hint_mode = grid opens anything.
    if (!uia_start())
        app_log(L"uia: UI Automation unavailable, element hints disabled");

    start_watch_thread();
    shutdown_init();
    touchpad_update();

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
    if (update_restarted())
        tray_balloon(L"vim mouse updated", L"Now running the latest version.");
    if (config_warnings()[0])
        tray_balloon(L"Settings problem", config_warnings());

    // Which build is running (also in the help window's title).
    app_log(L"main: started, built " __DATE__ L" " __TIME__);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (snippet_editor_dialog_message(&msg) ||
            secretary_editor_message(&msg) ||
            shutdown_dialog_message(&msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mutex)
        CloseHandle(mutex);
    return (int)msg.wParam;
}

// Whether Windows gave this process UIAccess: the build with uiAccess="true"
// in its manifest (make uiaccess), signed by a certificate the machine trusts
// and started from Program Files (tools/uiaccess_setup.ps1). Then a topmost
// window of ours sits above the Start menu, Search, the notification centre
// and notification banners, which otherwise cover the hint overlay, and the
// keyboard hook and clicks reach administrator windows. TokenUIAccess is 26
// in TOKEN_INFORMATION_CLASS; the number keeps old MinGW.org headers happy.
BOOL app_has_uiaccess(void)
{
    static int known = -1;
    HANDLE token;
    DWORD ui = 0;
    DWORD size = 0;

    if (known < 0) {
        known = 0;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            if (GetTokenInformation(token, (TOKEN_INFORMATION_CLASS)26, &ui,
                                    sizeof(ui), &size) && ui)
                known = 1;
            CloseHandle(token);
        }
    }
    return known > 0;
}
