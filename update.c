// update.c - self-update from the tray menu: downloads the latest release's
// vim.exe from GitHub on a worker thread, swaps it in and restarts.
//
// A running exe cannot be overwritten or deleted, but it can be renamed. So
// the download goes to "<exe>.new"; the running exe becomes "<exe>.old", the
// new file takes its name (the Run value and vim.ini keep working), and the
// new process starts with --updated. It waits for this instance's mutex
// (main.c) and deletes "<exe>.old" once this process has gone.

#include "common.h"
#include "version.h"

#include <basetyps.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wininet.h>

#ifndef INTERNET_FLAG_PRAGMA_NOCACHE
#define INTERNET_FLAG_PRAGMA_NOCACHE 0x00000100
#endif
#ifndef INTERNET_FLAG_NO_UI
#define INTERNET_FLAG_NO_UI 0x00000200
#endif

#define UPDATE_URL    L"https://github.com/kiiwoom8/v/releases/latest/download/vim.exe"
#define RELEASE_PAGE  L"https://github.com/kiiwoom8/v/releases/latest"
#define UPDATED_ARG   L"--updated"

#define UPDATE_TIMEOUT_MS   30000
#define UPDATE_MIN_BYTES    4096
#define UPDATE_MAX_BYTES    (32u * 1024u * 1024u)
// The old instance shuts its threads down with 3 s waits each; give it room.
#define UPDATE_WAIT_OLD_MS  30000
// Right after an update the old process may still be unmapping its exe.
#define OLD_DELETE_TRIES    20
#define OLD_DELETE_WAIT_MS  100

enum { UPDATE_FAILED = 0, UPDATE_SAME = 1, UPDATE_READY = 2 };

static volatile LONG g_running;     // a download is in progress
static int           g_restarted = -1;  // -1 = command line not parsed yet
static BOOL          g_paths_ready;
static WCHAR         g_exe_path[APP_PATH_MAX];
static WCHAR         g_new_path[APP_PATH_MAX];
static WCHAR         g_old_path[APP_PATH_MAX];
static WCHAR         g_cmd_line[APP_PATH_MAX];
// Written by the worker before it posts WM_APP_UPDATE_DONE, read after.
static WCHAR         g_error[1024];

// ---------------------------------------------------------------------------
// Helpers

static void set_error(const WCHAR *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    _vsnwprintf(g_error, 1023, fmt, ap);
    g_error[1023] = L'\0';
    va_end(ap);
    app_log(L"update: %s", g_error);
}

// WinINet keeps the text for its own codes (12000-12999) in wininet.dll.
static void error_text(DWORD err, WCHAR *buf, int cch)
{
    DWORD n = 0;

    if (err >= 12000 && err <= 12999)
        n = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE |
                               FORMAT_MESSAGE_IGNORE_INSERTS,
                           GetModuleHandleW(L"wininet.dll"), err, 0, buf,
                           (DWORD)cch, NULL);
    if (n == 0)
        n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
                               FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL, err, 0, buf, (DWORD)cch, NULL);
    if (n == 0 || n >= (DWORD)cch) {
        _snwprintf(buf, cch - 1, L"error %lu", err);
        buf[cch - 1] = L'\0';
        return;
    }
    while (n > 0 && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' ||
                     buf[n - 1] == L' ' || buf[n - 1] == L'.'))
        buf[--n] = L'\0';
}

static BOOL add_suffix(WCHAR *out, const WCHAR *path, const WCHAR *suffix)
{
    size_t n = wcslen(path), s = wcslen(suffix);

    if (n + s + 1 > APP_PATH_MAX)
        return FALSE;
    memcpy(out, path, n * sizeof(WCHAR));
    memcpy(out + n, suffix, (s + 1) * sizeof(WCHAR));
    return TRUE;
}

// Main thread only; the worker reads the paths after CreateThread.
static BOOL init_paths(void)
{
    DWORD n;

    if (g_paths_ready)
        return TRUE;
    n = GetModuleFileNameW(NULL, g_exe_path, APP_PATH_MAX);
    if (n == 0 || n >= APP_PATH_MAX)
        return FALSE;
    if (!add_suffix(g_new_path, g_exe_path, L".new") ||
        !add_suffix(g_old_path, g_exe_path, L".old"))
        return FALSE;
    g_paths_ready = TRUE;
    return TRUE;
}

// Reads a whole file of at most UPDATE_MAX_BYTES; malloc'd, NULL on failure.
static BYTE *read_all(const WCHAR *path, DWORD *size)
{
    HANDLE f;
    DWORD  n, got = 0;
    BYTE  *buf;

    // The running exe is mapped by the loader, which shares read and delete.
    f = CreateFileW(path, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    n = GetFileSize(f, NULL);
    if (n == INVALID_FILE_SIZE || n > UPDATE_MAX_BYTES) {
        CloseHandle(f);
        return NULL;
    }
    buf = (BYTE *)malloc(n ? n : 1);
    if (buf && (!ReadFile(f, buf, n, &got, NULL) || got != n)) {
        free(buf);
        buf = NULL;
    }
    CloseHandle(f);
    if (buf)
        *size = n;
    return buf;
}

// "MZ", and e_lfanew pointing at "PE\0\0" inside the file. Catches an HTML
// error page or a cut-off download before it replaces a working exe.
static BOOL looks_like_exe(const BYTE *p, DWORD n)
{
    DWORD pe;

    if (n < UPDATE_MIN_BYTES || p[0] != 'M' || p[1] != 'Z')
        return FALSE;
    pe = (DWORD)p[0x3c] | ((DWORD)p[0x3d] << 8) | ((DWORD)p[0x3e] << 16) |
         ((DWORD)p[0x3f] << 24);
    return pe <= n - 4 && memcmp(p + pe, "PE\0\0", 4) == 0;
}

// ---------------------------------------------------------------------------
// Worker

static BOOL download(void)
{
    HINTERNET net = NULL, url = NULL;
    HANDLE    out = INVALID_HANDLE_VALUE;
    DWORD     timeout = UPDATE_TIMEOUT_MS;
    DWORD     status = 0, expected = 0, total = 0, len;
    WCHAR     why[256];
    BYTE      buf[16384];
    BOOL      ok = FALSE;

    net = InternetOpenW(L"vim-mouse/" VIM_VERSION_STR,
                        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!net) {
        error_text(GetLastError(), why, 256);
        set_error(L"Could not start the download: %s.", why);
        goto done;
    }
    InternetSetOptionW(net, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout,
                       sizeof(timeout));
    InternetSetOptionW(net, INTERNET_OPTION_SEND_TIMEOUT, &timeout,
                       sizeof(timeout));
    InternetSetOptionW(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout,
                       sizeof(timeout));

    // GitHub redirects to its download host; WinINet follows that itself.
    url = InternetOpenUrlW(net, UPDATE_URL, NULL, 0,
                           INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                               INTERNET_FLAG_PRAGMA_NOCACHE |
                               INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI,
                           0);
    if (!url) {
        error_text(GetLastError(), why, 256);
        set_error(L"Could not connect to GitHub: %s.", why);
        goto done;
    }
    len = sizeof(status);
    if (!HttpQueryInfoW(url, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                        &status, &len, NULL))
        status = 0;
    if (status != 200) {
        set_error(L"GitHub answered %lu instead of the file.", status);
        goto done;
    }
    len = sizeof(expected);
    if (!HttpQueryInfoW(url,
                        HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER,
                        &expected, &len, NULL))
        expected = 0;
    if (expected > UPDATE_MAX_BYTES) {
        set_error(L"The download is too large (%lu bytes).", expected);
        goto done;
    }

    out = CreateFileW(g_new_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        error_text(err, why, 256);
        set_error(err == ERROR_ACCESS_DENIED
                      ? L"Could not write %s: %s. The folder may be "
                        L"read-only (for example under Program Files)."
                      : L"Could not write %s: %s.",
                  g_new_path, why);
        goto done;
    }

    for (;;) {
        DWORD got = 0, wrote = 0;

        if (!InternetReadFile(url, buf, sizeof(buf), &got)) {
            error_text(GetLastError(), why, 256);
            set_error(L"The download stopped: %s.", why);
            goto done;
        }
        if (got == 0)
            break;
        total += got;
        if (total > UPDATE_MAX_BYTES) {
            set_error(L"The download is too large.");
            goto done;
        }
        if (!WriteFile(out, buf, got, &wrote, NULL) || wrote != got) {
            error_text(GetLastError(), why, 256);
            set_error(L"Could not write %s: %s.", g_new_path, why);
            goto done;
        }
    }
    if (expected != 0 && total != expected) {
        set_error(L"The download is incomplete (%lu of %lu bytes).", total,
                  expected);
        goto done;
    }
    app_log(L"update: downloaded %lu bytes", total);
    ok = TRUE;

done:
    if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
    if (url)
        InternetCloseHandle(url);
    if (net)
        InternetCloseHandle(net);
    return ok;
}

static int check_download(void)
{
    BYTE *fresh, *mine;
    DWORD fresh_n = 0, mine_n = 0;
    int   result;

    fresh = read_all(g_new_path, &fresh_n);
    if (!fresh) {
        set_error(L"Could not read the downloaded file back.");
        return UPDATE_FAILED;
    }
    if (!looks_like_exe(fresh, fresh_n)) {
        free(fresh);
        set_error(L"The download is not a Windows program.");
        return UPDATE_FAILED;
    }
    // Unreadable running exe: install anyway, the user asked for the latest.
    mine = read_all(g_exe_path, &mine_n);
    result = (mine && mine_n == fresh_n && memcmp(mine, fresh, fresh_n) == 0)
                 ? UPDATE_SAME
                 : UPDATE_READY;
    free(mine);
    free(fresh);
    return result;
}

static DWORD WINAPI update_worker(LPVOID param)
{
    int result = UPDATE_FAILED;

    (void)param;
    if (download())
        result = check_download();
    if (result != UPDATE_READY)
        DeleteFileW(g_new_path);
    app_log(L"update: %s", result == UPDATE_READY  ? L"new version downloaded"
                           : result == UPDATE_SAME ? L"already up to date"
                                                   : L"failed");
    // g_running stays set until update_on_done, so no second download can
    // overwrite the file being installed.
    if (!PostMessageW(g_main_wnd, WM_APP_UPDATE_DONE, (WPARAM)result, 0)) {
        DeleteFileW(g_new_path);
        InterlockedExchange(&g_running, 0);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Install (main thread)

static BOOL install_and_restart(void)
{
    static WCHAR        dir[APP_PATH_MAX];
    STARTUPINFOW        si;
    PROCESS_INFORMATION pi;
    WCHAR              *slash;
    WCHAR               why[256];
    DWORD               err;

    if (wcslen(g_exe_path) + wcslen(UPDATED_ARG) + 4 > APP_PATH_MAX) {
        set_error(L"The program's path is too long to restart it.");
        DeleteFileW(g_new_path);
        return FALSE;
    }
    if (!MoveFileExW(g_exe_path, g_old_path, MOVEFILE_REPLACE_EXISTING)) {
        error_text(GetLastError(), why, 256);
        set_error(L"Could not rename %s: %s.", g_exe_path, why);
        DeleteFileW(g_new_path);
        return FALSE;
    }
    if (!MoveFileExW(g_new_path, g_exe_path, MOVEFILE_REPLACE_EXISTING)) {
        error_text(GetLastError(), why, 256);
        set_error(L"Could not put the new version in place: %s.", why);
        MoveFileExW(g_old_path, g_exe_path, 0);
        DeleteFileW(g_new_path);
        return FALSE;
    }

    lstrcpynW(dir, g_exe_path, APP_PATH_MAX);
    slash = wcsrchr(dir, L'\\');
    if (slash)
        *slash = L'\0';
    _snwprintf(g_cmd_line, APP_PATH_MAX - 1, L"\"%s\" " UPDATED_ARG,
               g_exe_path);
    g_cmd_line[APP_PATH_MAX - 1] = L'\0';

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(g_exe_path, g_cmd_line, NULL, NULL, FALSE, 0, NULL,
                        slash ? dir : NULL, &si, &pi)) {
        err = GetLastError();
        // Put the old exe back: the next start must run something that works.
        MoveFileExW(g_exe_path, g_new_path, MOVEFILE_REPLACE_EXISTING);
        MoveFileExW(g_old_path, g_exe_path, 0);
        DeleteFileW(g_new_path);
        error_text(err, why, 256);
        set_error(L"Could not start the new version: %s.", why);
        return FALSE;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    app_log(L"update: installed, restarting");
    app_quit();
    return TRUE;
}

// ---------------------------------------------------------------------------
// Public

BOOL update_running(void)
{
    return g_running != 0;
}

void update_start(void)
{
    HANDLE thread;

    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) {
        tray_balloon(L"Updating", L"Already downloading the update.");
        return;
    }
    if (!init_paths()) {
        InterlockedExchange(&g_running, 0);
        MessageBoxW(NULL, L"The program's own path could not be read, so it "
                          L"cannot be updated.",
                    APP_NAME, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        return;
    }
    g_error[0] = L'\0';
    app_log(L"update: downloading %s", UPDATE_URL);
    thread = CreateThread(NULL, 0, update_worker, NULL, 0, NULL);
    if (!thread) {
        InterlockedExchange(&g_running, 0);
        app_log(L"update: CreateThread failed (%lu)", GetLastError());
        tray_balloon(L"Update failed", L"Could not start the download.");
        return;
    }
    CloseHandle(thread);
    tray_balloon(L"Updating", L"Downloading the latest version...");
}

void update_on_done(WPARAM result)
{
    WCHAR msg[1400];

    InterlockedExchange(&g_running, 0);
    if (result == UPDATE_SAME) {
        tray_balloon(L"Up to date", L"You already have the latest version.");
        return;
    }
    if (result == UPDATE_READY && install_and_restart())
        return;

    _snwprintf(msg, 1399,
               L"The update did not work.\n\n%s\n\nOpen the download page to "
               L"get vim.exe yourself?",
               g_error[0] ? g_error : L"Unknown error.");
    msg[1399] = L'\0';
    if (MessageBoxW(NULL, msg, APP_NAME,
                    MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND) == IDYES)
        ShellExecuteW(NULL, L"open", RELEASE_PAGE, NULL, NULL, SW_SHOWNORMAL);
}

BOOL update_restarted(void)
{
    if (g_restarted < 0) {
        int     argc = 0, i;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

        g_restarted = 0;
        if (argv) {
            for (i = 1; i < argc; i++)
                if (lstrcmpiW(argv[i], UPDATED_ARG) == 0)
                    g_restarted = 1;
            LocalFree(argv);
        }
    }
    return g_restarted == 1;
}

BOOL update_wait_for_old(HANDLE mutex)
{
    DWORD w;

    if (!update_restarted())
        return FALSE;
    w = WaitForSingleObject(mutex, UPDATE_WAIT_OLD_MS);
    // WAIT_ABANDONED: the old instance exited holding it; it is ours now.
    return w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
}

void update_cleanup(void)
{
    int   tries = update_restarted() ? OLD_DELETE_TRIES : 1;
    int   i;
    DWORD err = 0;

    if (!init_paths())
        return;
    // A download cut off by quitting leaves a partial file.
    DeleteFileW(g_new_path);
    for (i = 0; i < tries; i++) {
        if (DeleteFileW(g_old_path))
            return;
        err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND)
            return;
        if (i + 1 < tries)
            Sleep(OLD_DELETE_WAIT_MS);
    }
    app_log(L"update: could not delete %s (%lu)", g_old_path, err);
}
