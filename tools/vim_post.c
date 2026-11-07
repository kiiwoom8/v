// vim_post.c - drives the running vim mouse for tests (make loadprobe).
// usage: vim_post open|close|again   posts WM_APP_HINT_OPEN / _CLOSE / _AGAIN
//        vim_post front <pid>         brings that process's browser window to
//                                     the front; prints the window that was
//        vim_post back <hwnd>         gives the front back to that window
//        vim_post fg                  prints the window in front
// open does what the hotkey does (hints on the monitor under the pointer)
// without injecting a key; tools/load_probe.mjs --open uses all of it. A
// console process may not take the foreground (it has had no input), so
// front joins the foreground window's input queue for the switch.
#include "../common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DWORD g_pid;
static HWND g_found;

static BOOL CALLBACK top_proc(HWND h, LPARAM lp)
{
    DWORD pid = 0;
    WCHAR cls[64];

    (void)lp;
    GetWindowThreadProcessId(h, &pid);
    if (pid == g_pid && IsWindowVisible(h) && GetClassNameW(h, cls, 64) &&
        wcsncmp(cls, L"Chrome_WidgetWin_", 17) == 0) {
        g_found = h;
        return FALSE;
    }
    return TRUE;
}

static void to_front(HWND target)
{
    HWND fg = GetForegroundWindow();
    DWORD me = GetCurrentThreadId();
    DWORD them = fg ? GetWindowThreadProcessId(fg, NULL) : 0;

    if (them && them != me) AttachThreadInput(me, them, TRUE);
    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    BringWindowToTop(target);
    SetForegroundWindow(target);
    if (them && them != me) AttachThreadInput(me, them, FALSE);
}

static void print_front(const char *what)
{
    HWND fg = GetForegroundWindow();
    WCHAR cls[64] = L"";
    DWORD pid = 0;

    if (fg) {
        GetClassNameW(fg, cls, 64);
        GetWindowThreadProcessId(fg, &pid);
    }
    printf("%s %lu %ls pid %lu\n", what, (unsigned long)(ULONG_PTR)fg, cls,
           (unsigned long)pid);
}

int main(int argc, char **argv)
{
    HWND wnd;
    UINT msg;

    if (argc >= 3 && strcmp(argv[1], "front") == 0) {
        g_pid = (DWORD)strtoul(argv[2], NULL, 10);
        print_front("was");
        EnumWindows(top_proc, 0);
        if (!g_found) {
            fprintf(stderr, "no browser window for pid %lu\n",
                    (unsigned long)g_pid);
            return 1;
        }
        to_front(g_found);
        Sleep(100);
        print_front("now");
        return GetForegroundWindow() == g_found ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "fg") == 0) {
        print_front("was");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "back") == 0) {
        HWND target = (HWND)(ULONG_PTR)strtoul(argv[2], NULL, 10);

        if (target && IsWindow(target)) to_front(target);
        print_front("now");
        return 0;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: vim_post open|close|again|fg|front <pid>|back <hwnd>\n");
        return 2;
    }
    if (strcmp(argv[1], "open") == 0) msg = WM_APP_HINT_OPEN;
    else if (strcmp(argv[1], "close") == 0) msg = WM_APP_HINT_CLOSE;
    else if (strcmp(argv[1], "again") == 0) msg = WM_APP_HINT_AGAIN;
    else {
        fprintf(stderr, "unknown message %s\n", argv[1]);
        return 2;
    }
    wnd = FindWindowExW(NULL, NULL, L"VimMouseMainWindow", NULL);
    if (!wnd) {
        fprintf(stderr, "vim mouse is not running\n");
        return 1;
    }
    print_front("front");
    return PostMessageW(wnd, msg, 0, 0) ? 0 : 1;
}
