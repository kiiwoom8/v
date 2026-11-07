// load_probe.c - what a Chromium page tells us while it loads (make loadprobe).
// usage: load_probe <browser pid> <seconds>   (tools/load_probe.mjs runs it)
// Every 100 ms: uia.c's page_busy for the browser's window (the flag a scan
// puts in ClickList.page_busy). Every event the hint overlay's page watch
// takes (hint.c load_event_proc: EVENT_OBJECT_SHOW / EVENT_OBJECT_REORDER
// with OBJID_CLIENT on a Chrome_RenderWidgetHostHWND, and
// IA2_EVENT_DOCUMENT_LOAD_COMPLETE), with its time. Lines start with the
// epoch time in ms, so they merge with the driver's own (navigation, load).
// Measured 2026-11-08 (HISTORY.md, section 5): busy from the navigation
// until the first content is in the tree, then SHOW of the new document's
// root; later parts REORDER it; LOAD_COMPLETE with the load event.
#include "../uia.c"
#include <stdarg.h>
#include <stdio.h>

Config g_cfg;
HWND g_main_wnd = NULL;
void app_log(const WCHAR *fmt, ...) { (void)fmt; }
UINT dpi_for_monitor(HMONITOR mon) { (void)mon; return 96; }

#ifndef IA2_EVENT_DOCUMENT_LOAD_COMPLETE
#define IA2_EVENT_DOCUMENT_LOAD_COMPLETE 0x105
#endif

static DWORD g_pid;

static double epoch_ms(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (double)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

static void CALLBACK ev_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                             LONG obj, LONG child, DWORD thread, DWORD time)
{
    WCHAR cls[64];
    const char *name;

    (void)hook; (void)thread; (void)time;
    if (!hwnd || obj != (LONG)OBJID_CLIENT ||
        !GetClassNameW(hwnd, cls, 64) ||
        wcscmp(cls, L"Chrome_RenderWidgetHostHWND") != 0)
        return;
    if (event == EVENT_OBJECT_SHOW) name = "SHOW";
    else if (event == EVENT_OBJECT_REORDER) name = "REORDER";
    else if (event == IA2_EVENT_DOCUMENT_LOAD_COMPLETE) name = "LOAD_COMPLETE";
    else return;
    printf("%.0f EV %s child %ld\n", epoch_ms(), name, child);
    fflush(stdout);
}

static HWND g_root;
static BOOL CALLBACK top_proc(HWND h, LPARAM lp)
{
    DWORD pid = 0;
    (void)lp;
    GetWindowThreadProcessId(h, &pid);
    if (pid == g_pid && IsWindowVisible(h) && uia_chromium_window(h)) {
        g_root = h;
        return FALSE;
    }
    return TRUE;
}

int main(int argc, char **argv)
{
    HWINEVENTHOOK hook, done_hook;
    double end, next = 0;
    int last = -1;
    MSG msg;

    if (argc < 3) {
        fprintf(stderr, "usage: load_probe <browser pid> <seconds>\n");
        return 2;
    }
    g_pid = (DWORD)strtoul(argv[1], NULL, 10);
    g_cfg.scan_loading = 1;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_REORDER, NULL,
                           ev_proc, g_pid, 0, WINEVENT_OUTOFCONTEXT);
    done_hook = SetWinEventHook(IA2_EVENT_DOCUMENT_LOAD_COMPLETE,
                                IA2_EVENT_DOCUMENT_LOAD_COMPLETE, NULL,
                                ev_proc, g_pid, 0, WINEVENT_OUTOFCONTEXT);
    printf("%.0f START pid %lu hooks %s\n", epoch_ms(), (unsigned long)g_pid,
           hook && done_hook ? "ok" : "FAILED");
    fflush(stdout);
    end = epoch_ms() + atoi(argv[2]) * 1000.0;
    while (epoch_ms() < end) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (epoch_ms() >= next) {
            int busy;
            double t = epoch_ms();

            g_root = NULL;
            EnumWindows(top_proc, 0);
            busy = g_root ? page_busy(g_root) : -1;
            if (busy != last) {
                printf("%.0f BUSY %d (%.0f ms)\n", t, busy, epoch_ms() - t);
                fflush(stdout);
                last = busy;
            }
            next = t + 100;
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 5, QS_ALLINPUT);
    }
    UnhookWinEvent(hook);
    UnhookWinEvent(done_hook);
    CoUninitialize();
    return 0;
}
