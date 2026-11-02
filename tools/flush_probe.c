// flush_probe.c - how a Chromium page reports a scroll (make flushprobe).
// With Chrome in front: posts one wheel notch down, then up, to its page
// area in a series of runs, sends UI Automation hit tests (the flushes of
// hint.c's settle wait, uia_flush) at chosen times, and prints every
// EVENT_SYSTEM_SCROLLINGSTART/END report of that process with its time in
// ms from the wheel. The page scrolls one notch and back. Measured on
// 2026-11-03 (HISTORY.md, section 5): with Windows' animation effects off
// the first report came 2 ms after the wheel, a hit test's report came with
// its answer, and nothing more was pending afterwards.
#include "../uia.c"
#include <stdarg.h>
#include <mmsystem.h>
#include <stdio.h>

Config g_cfg;
HWND g_main_wnd = NULL;
void app_log(const WCHAR *fmt, ...) { (void)fmt; }
UINT dpi_for_monitor(HMONITOR mon) { (void)mon; return 96; }

typedef HRESULT (STDMETHODCALLTYPE *CurrentPidFn)(IUIAutomationElement *, int *);

static LARGE_INTEGER g_freq, g_t0;
static double now_ms(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)(c.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}
static void mark(void) { QueryPerformanceCounter(&g_t0); }

static DWORD g_pid;
static void CALLBACK ev_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                             LONG obj, LONG child, DWORD thread, DWORD time)
{
    DWORD pid = 0;
    (void)hook; (void)thread; (void)time;
    if (hwnd) GetWindowThreadProcessId(hwnd, &pid);
    if (pid != g_pid) return;
    printf("  %7.1f  report event %lx hwnd %p obj %ld child %ld\n", now_ms(),
           (unsigned long)event, (void *)hwnd, obj, child);
    fflush(stdout);
}

static void pump(double until_ms)
{
    MSG msg;
    while (now_ms() < until_ms) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(1);
    }
}

static void flush(UiaEngine *engine, POINT pt)
{
    IUIAutomationElement *el = NULL;
    double t = now_ms();
    int pid = -1;
    HRESULT hr = engine->automation->lpVtbl->ElementFromPoint(engine->automation, pt, &el);
    if (SUCCEEDED(hr) && el) {
        CurrentPidFn f = (CurrentPidFn)el->lpVtbl->get_CurrentProcessId;
        f(el, &pid);
        el->lpVtbl->Release(el);
    }
    printf("  %7.1f  flush sent (took %.1f ms, hr %lx, element pid %d%s)\n", t,
           now_ms() - t, (unsigned long)hr, pid, (DWORD)pid == g_pid ? " = chrome" : "");
    fflush(stdout);
}

static void wheel(HWND hwnd, POINT pt, int delta)
{
    printf("  %7.1f  wheel %+d posted\n", now_ms(), delta);
    fflush(stdout);
    PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)(SHORT)delta),
                 MAKELPARAM((WORD)(SHORT)pt.x, (WORD)(SHORT)pt.y));
}

typedef struct { int delta; int flushes[12]; int nflush; const char *name; } Run;

int main(void)
{
    UiaEngine engine;
    HWND fg = GetForegroundWindow(), at;
    POINT pt;
    HWINEVENTHOOK hook;
    typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
    SetCtxFn setctx = (SetCtxFn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    Run runs[] = {
        { -120, {0}, 0, "A: down, no flush (natural reports)" },
        { 120, {230}, 1, "B: up, flush at 230" },
        { -120, {60, 100, 140, 180, 220, 260, 300, 340}, 8, "C: down, flushes every 40 ms" },
        { 120, {230, 265}, 2, "D: up, flushes at 230 and 265 (as vim.exe)" },
        { 0, {0}, 1, "E: no wheel, flush at 0" },
        { -120, {30, 60, 90, 120, 150, 180, 210, 240, 270, 300}, 10, "F: down, flushes every 30 ms" },
        { 120, {250}, 1, "G: up, flush at 250" },
    };
    int r, i;

    if (setctx) setctx((HANDLE)-4);
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&g_freq);
    if (CoInitializeEx(NULL, COINIT_MULTITHREADED) != S_OK) return 1;
    g_slow_memory = FALSE;
    if (!uia_engine_init(&engine, FALSE)) { printf("no UIA\n"); return 1; }
    fg = fg ? GetAncestor(fg, GA_ROOT) : NULL;
    if (!fg || !uia_chromium_window(fg)) { printf("front window is not Chromium\n"); return 1; }
    GetWindowThreadProcessId(fg, &g_pid);
    if (!uia_page_point(fg, NULL, &pt)) { printf("no page point on top\n"); return 1; }
    at = WindowFromPoint(pt);
    {
        WCHAR cls[64]; GetClassNameW(at, cls, 64);
        printf("chrome pid %lu, page point (%ld,%ld) in %ls\n", (unsigned long)g_pid, pt.x, pt.y, cls);
    }
    hook = SetWinEventHook(EVENT_SYSTEM_SCROLLINGSTART, EVENT_SYSTEM_SCROLLINGEND, NULL,
                           ev_proc, g_pid, 0, WINEVENT_OUTOFCONTEXT);
    if (!hook) { printf("no hook\n"); return 1; }

    for (r = 0; r < (int)(sizeof(runs) / sizeof(runs[0])); ++r) {
        Run *run = &runs[r];
        printf("run %s\n", run->name);
        mark();
        pump(1);
        if (run->delta) wheel(at, pt, run->delta);
        for (i = 0; i < run->nflush; ++i) {
            pump(run->flushes[i]);
            flush(&engine, pt);
        }
        pump(1500);
    }
    UnhookWinEvent(hook);
    uia_engine_cleanup(&engine);
    CoUninitialize();
    return 0;
}
