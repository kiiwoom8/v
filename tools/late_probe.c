// late_probe.c - when content a Chromium page shows late reaches UI
// Automation, and what the browser reports on the way (make lateprobe).
// usage: late_probe <browser pid> <seconds> <element name> [flush ms]
// tools/late_probe.mjs runs it once per scenario. Every 10 ms it looks for
// the element named <element name> in the browser window (FindFirst, as a
// scan would see it: its rect and IsOffscreen) and prints each change of
// what it finds. It prints every WinEvent of the browser process on a page
// window (Chrome_RenderWidgetHostHWND) with its time, and with [flush ms]
// it sends an MSAA hit test to the page every that many ms (what uia.c
// flush_at does through UI Automation, without needing the page in front),
// to see whether asking makes the page send its late content sooner. Lines
// start with the epoch time in ms (an event's own time), so they merge with
// the driver's.
#include "../uia.c"
#include <oleacc.h>
#include <stdarg.h>
#include <stdio.h>

Config g_cfg;
HWND g_main_wnd = NULL;
void app_log(const WCHAR *fmt, ...) { (void)fmt; }
UINT dpi_for_monitor(HMONITOR mon) { (void)mon; return 96; }

static DWORD g_pid;
static HWND g_root;

static double epoch_ms(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (double)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

static const char *ev_name(DWORD e)
{
    switch (e) {
    case EVENT_OBJECT_CREATE: return "CREATE";
    case EVENT_OBJECT_DESTROY: return "DESTROY";
    case EVENT_OBJECT_SHOW: return "SHOW";
    case EVENT_OBJECT_HIDE: return "HIDE";
    case EVENT_OBJECT_REORDER: return "REORDER";
    case EVENT_OBJECT_FOCUS: return "FOCUS";
    case EVENT_OBJECT_SELECTION: return "SELECTION";
    case EVENT_OBJECT_STATECHANGE: return "STATECHANGE";
    case EVENT_OBJECT_LOCATIONCHANGE: return "LOCATIONCHANGE";
    case EVENT_OBJECT_NAMECHANGE: return "NAMECHANGE";
    case EVENT_OBJECT_VALUECHANGE: return "VALUECHANGE";
    case EVENT_SYSTEM_SCROLLINGSTART: return "SCROLLINGSTART";
    case EVENT_SYSTEM_SCROLLINGEND: return "SCROLLINGEND";
    case EVENT_SYSTEM_ALERT: return "ALERT";
    }
    return NULL;
}

static void CALLBACK ev_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                             LONG obj, LONG child, DWORD thread, DWORD time)
{
    WCHAR cls[64];
    const char *name = ev_name(event);
    // When the browser fired it, not when this thread got to it (a query
    // in progress holds the delivery back).
    double at = epoch_ms() - (double)(GetTickCount() - time);

    (void)hook; (void)thread;
    if (!hwnd || !GetClassNameW(hwnd, cls, 64) ||
        wcscmp(cls, L"Chrome_RenderWidgetHostHWND") != 0)
        return;
    if (name)
        printf("%.0f EV %s obj %ld child %ld\n", at, name, obj, child);
    else
        printf("%.0f EV 0x%lx obj %ld child %ld\n", at,
               (unsigned long)event, obj, child);
    fflush(stdout);
}

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

typedef struct { HWND best; LONG64 area; } Page;
static BOOL CALLBACK page_proc(HWND h, LPARAM lp)
{
    Page *p = (Page *)lp;
    WCHAR cls[64];
    RECT r;
    LONG64 a;

    if (!IsWindowVisible(h) || !GetClassNameW(h, cls, 64) ||
        wcscmp(cls, L"Chrome_RenderWidgetHostHWND") != 0 ||
        !GetWindowRect(h, &r))
        return TRUE;
    a = (LONG64)(r.right - r.left) * (r.bottom - r.top);
    if (a > p->area) { p->area = a; p->best = h; }
    return TRUE;
}

int main(int argc, char **argv)
{
    HWINEVENTHOOK hooks[3];
    UiaEngine engine;
    IUIAutomationElement *top = NULL;
    IUIAutomationCondition *by_name = NULL;
    IUIAutomationCacheRequest *cache = NULL;
    IAccessible *page_acc = NULL;
    POINT page_mid = { 0, 0 };
    WCHAR wname[128];
    VARIANT v;
    double end, next = 0, next_flush = 0;
    int flush_ms = argc > 4 ? atoi(argv[4]) : 0;
    int last = -2, flushes = 0;
    RECT last_rc = { 0, 0, 0, 0 };
    MSG msg;
    Page page = { NULL, 0 };

    if (argc < 4) {
        fprintf(stderr, "usage: late_probe <pid> <seconds> <name> [flush ms]\n");
        return 2;
    }
    g_pid = (DWORD)strtoul(argv[1], NULL, 10);
    MultiByteToWideChar(CP_UTF8, 0, argv[3], -1, wname, 128);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (!uia_engine_init(&engine, FALSE)) {
        printf("%.0f FAIL engine\n", epoch_ms());
        return 1;
    }
    EnumWindows(top_proc, 0);
    if (!g_root ||
        FAILED(engine.automation->lpVtbl->ElementFromHandle(
            engine.automation, (UIA_HWND)g_root, &top)) || !top) {
        printf("%.0f FAIL no browser window\n", epoch_ms());
        return 1;
    }
    EnumChildWindows(g_root, page_proc, (LPARAM)&page);
    if (page.best) {
        RECT r;
        GetWindowRect(page.best, &r);
        page_mid.x = (r.left + r.right) / 2;
        page_mid.y = r.bottom - 20;     // clear of the links at the top
        if (FAILED(AccessibleObjectFromWindow(page.best, (DWORD)OBJID_CLIENT,
                                              &IID_IAccessible,
                                              (void **)&page_acc)))
            page_acc = NULL;
    }
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(wname);
    engine.automation->lpVtbl->CreatePropertyCondition(
        engine.automation, UIA_NamePropertyId, v, &by_name);
    VariantClear(&v);
    engine.automation->lpVtbl->CreateCacheRequest(engine.automation, &cache);
    if (cache) {
        cache->lpVtbl->AddProperty(cache, UIA_BoundingRectanglePropertyId);
        cache->lpVtbl->AddProperty(cache, UIA_IsOffscreenPropertyId);
    }
    if (!by_name || !cache) {
        printf("%.0f FAIL condition\n", epoch_ms());
        return 1;
    }
    hooks[0] = SetWinEventHook(EVENT_OBJECT_CREATE, 0x80FF, NULL,
                               ev_proc, g_pid, 0, WINEVENT_OUTOFCONTEXT);
    hooks[1] = SetWinEventHook(EVENT_SYSTEM_SCROLLINGSTART,
                               EVENT_SYSTEM_SCROLLINGEND, NULL, ev_proc,
                               g_pid, 0, WINEVENT_OUTOFCONTEXT);
    hooks[2] = SetWinEventHook(0x100, 0x1FF, NULL, ev_proc, g_pid, 0,
                               WINEVENT_OUTOFCONTEXT);
    printf("%.0f READY pid %lu page %p flush %d ms (%s)\n", epoch_ms(),
           (unsigned long)g_pid, (void *)page.best, flush_ms,
           page_acc ? "msaa" : "none");
    fflush(stdout);

    end = epoch_ms() + atoi(argv[2]) * 1000.0;
    while (epoch_ms() < end) {
        double t = epoch_ms();

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (flush_ms > 0 && page_acc && t >= next_flush) {
            VARIANT hit;
            VariantInit(&hit);
            page_acc->lpVtbl->accHitTest(page_acc, page_mid.x, page_mid.y,
                                         &hit);
            VariantClear(&hit);
            ++flushes;
            next_flush = t + flush_ms;
        }
        if (t >= next) {
            IUIAutomationElement *el = NULL;
            int state = -1;
            RECT rc = { 0, 0, 0, 0 };

            if (SUCCEEDED(top->lpVtbl->FindFirstBuildCache(
                    top, UIA_TREESCOPE_DESCENDANTS, by_name, cache, &el)) &&
                el) {
                BOOL off = cached_bool_property(el, UIA_IsOffscreenPropertyId);
                el->lpVtbl->get_CachedBoundingRectangle(el, &rc);
                state = off ? 1 : 2;
                el->lpVtbl->Release(el);
            } else {
                state = 0;
            }
            if (state != last || (state > 0 && !EqualRect(&rc, &last_rc))) {
                printf("%.0f FOUND %s rect %ld,%ld %ldx%ld (query %.0f ms)\n",
                       t, state == 0 ? "none" : state == 1 ? "offscreen"
                                                            : "on screen",
                       rc.left, rc.top, rc.right - rc.left,
                       rc.bottom - rc.top, epoch_ms() - t);
                fflush(stdout);
                last = state;
                last_rc = rc;
            }
            next = t + 10;
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 2, QS_ALLINPUT);
    }
    printf("%.0f END %d flushes\n", epoch_ms(), flushes);
    fflush(stdout);
    for (int i = 0; i < 3; ++i)
        if (hooks[i]) UnhookWinEvent(hooks[i]);
    if (page_acc) page_acc->lpVtbl->Release(page_acc);
    top->lpVtbl->Release(top);
    CoUninitialize();
    return 0;
}
