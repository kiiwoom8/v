// zoom.c - Alt + = / Alt + -: zoom in and out like two fingers spreading or
// pinching on a touchpad, around the pointer.
//
// Ctrl + = / Ctrl + - (and Ctrl + wheel) change a browser's page zoom: the
// page reflows, the zoom is kept per site, and it zooms around the top of
// the page. A two-finger pinch on a precision touchpad is something else:
// Chromium, Edge and Firefox magnify the page around the fingers without a
// reflow, and Photos, Maps and PDF viewers zoom around that point. Windows
// cannot inject touchpad contacts, but it injects touch-screen contacts
// (InjectTouchInput, Windows 8), and apps treat a two-finger touch pinch the
// same way. So a held key puts two fingers down either side of the pointer
// and moves them apart (in) or together (out) while it is held; at the far
// or near limit they lift and grip again, as a hand would.
//
// The pinch runs on a thread of its own. The hook only records the press and
// wakes it: zoom_press, zoom_release and zoom_cancel never wait.
#include "common.h"

#include <math.h>

#ifndef MOUSEEVENTF_VIRTUALDESK
#define MOUSEEVENTF_VIRTUALDESK 0x4000
#endif

UINT WINAPI timeBeginPeriod(UINT uPeriod);
UINT WINAPI timeEndPeriod(UINT uPeriod);

// Touch injection, declared here under names of our own: MinGW.org has none
// of it, and mingw-w64 declares it only for WINVER 0x0602 and up (common.h
// sets 0x0601). The layout is winuser.h's POINTER_INFO and
// POINTER_TOUCH_INFO, checked against mingw-w64 on 32 and 64 bit.
typedef struct {
    DWORD     pointerType;
    UINT      pointerId;
    UINT      frameId;
    UINT      pointerFlags;
    HANDLE    sourceDevice;
    HWND      hwndTarget;
    POINT     ptPixelLocation;
    POINT     ptHimetricLocation;
    POINT     ptPixelLocationRaw;
    POINT     ptHimetricLocationRaw;
    DWORD     dwTime;
    UINT      historyCount;
    INT       InputData;
    DWORD     dwKeyStates;
    ULONGLONG PerformanceCount;
    DWORD     ButtonChangeType;
} VmPointerInfo;

typedef struct {
    VmPointerInfo pointerInfo;
    UINT  touchFlags;
    UINT  touchMask;
    RECT  rcContact;
    RECT  rcContactRaw;
    UINT  orientation;
    UINT  pressure;
} VmTouchInfo;

// A wrong layout would inject garbage; stop the build instead.
typedef char vm_touch_info_size_check[
    sizeof(VmTouchInfo) == (sizeof(void *) == 8 ? 144 : 136) ? 1 : -1];

typedef BOOL (WINAPI *InitTouchFn)(UINT, DWORD);
typedef BOOL (WINAPI *InjectTouchFn)(UINT, const VmTouchInfo *);

#define VM_PT_TOUCH            2
#define VM_POINTER_FLAG_INRANGE   0x00000002
#define VM_POINTER_FLAG_INCONTACT 0x00000004
#define VM_POINTER_FLAG_DOWN      0x00010000
#define VM_POINTER_FLAG_UPDATE    0x00020000
#define VM_POINTER_FLAG_UP        0x00040000
#define VM_TOUCH_MASK_CONTACTAREA 0x00000001
#define VM_TOUCH_MASK_ORIENTATION 0x00000002
#define VM_TOUCH_MASK_PRESSURE    0x00000004
// No touch circles drawn under the fingers.
#define VM_TOUCH_FEEDBACK_NONE    0x3

// A touch screen reports about 120 times a second.
#define ZOOM_FRAME_MS   8
// The fingers' spread in pixels at 100% scaling (the monitor's DPI scales
// them). Chromium starts a pinch only between fingers at least 125 apart
// (min_scaling_span) and only once their spread has changed by 30
// (span_slop, twice its touch slop), so the near limit stays well above
// the first and the first frame moves the fingers past the second at once:
// what follows is the zoom itself, not slop eaten by the browser. Zoom in
// starts at the near limit: at 160 (its first value) it did nothing for
// the user while zoom out, starting at the far one, worked, so the near
// limit is 240, clear of where a browser may count the fingers' own size
// against the spread or a DPI read wrong shrinks it under 125.
#define ZOOM_NEAR_DIP   240.0
#define ZOOM_FAR_DIP    800.0
#define ZOOM_SLOP_DIP   32.0
// Kept clear of the monitor's edges.
#define ZOOM_MARGIN_DIP 8.0
// A tap still zooms one step: the fingers move on, after the slop, until
// their spread has grown (or shrunk) by this factor, then lift.
#define ZOOM_TAP_STEP   1.25
// Fingers lifted at a limit grip again this long after.
#define ZOOM_REGRIP_MS  40

static HANDLE s_zoom_thread = NULL;
static HANDLE s_zoom_event = NULL;
static volatile LONG s_zoom_exit = 0;
// The zoom key held now: 1 in, -1 out, 0 none. The last press wins.
static volatile LONG s_zoom_held = 0;
// Which way the last press went, and how many presses so far: a tap already
// released when the thread wakes still pinches one step.
static volatile LONG s_zoom_press_dir = 0;
static volatile LONG s_zoom_gen = 0;
// Bumped by zoom_cancel (pause, lock, sleep, quit): the fingers lift now.
static volatile LONG s_zoom_cancel = 0;

static InitTouchFn   s_init_touch = NULL;
static InjectTouchFn s_inject_touch = NULL;
static BOOL s_touch_ready = FALSE;
static BOOL s_touch_tried = FALSE;

static LONG read_long(volatile LONG *v)
{
    return InterlockedCompareExchange(v, 0, 0);
}

static DWORD now_ms(LARGE_INTEGER freq)
{
    LARGE_INTEGER c;

    QueryPerformanceCounter(&c);
    if (freq.QuadPart <= 0)
        return GetTickCount();
    return (DWORD)((c.QuadPart * 1000) / freq.QuadPart);
}

// Zoom thread, first pinch: Windows 8 and up have touch injection.
static BOOL touch_ready(void)
{
    HMODULE user32;

    if (s_touch_tried)
        return s_touch_ready;
    s_touch_tried = TRUE;
    user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        s_init_touch = (InitTouchFn)(void *)GetProcAddress(
            user32, "InitializeTouchInjection");
        s_inject_touch = (InjectTouchFn)(void *)GetProcAddress(
            user32, "InjectTouchInput");
    }
    if (!s_init_touch || !s_inject_touch) {
        app_log(L"zoom: this Windows has no touch injection (Windows 8 and "
                L"up); Alt + = / Alt + - do nothing");
        return FALSE;
    }
    if (!s_init_touch(2, VM_TOUCH_FEEDBACK_NONE)) {
        app_log(L"zoom: InitializeTouchInjection failed (%lu)",
                GetLastError());
        return FALSE;
    }
    s_touch_ready = TRUE;
    return TRUE;
}

// One grip: the middle between the fingers, along which axis they lie, and
// how far apart they may go.
typedef struct {
    double cx, cy;
    BOOL   vertical;
    double near_px, far_px, slop_px;
    double span;          // where the fingers are now
} Grip;

// The fingers lie either side of the pointer, along whichever axis of its
// monitor leaves them more room. Near an edge the far limit shrinks to fit;
// only in a corner too tight for any pinch does the middle move inward.
static BOOL grip_place(Grip *g, POINT at)
{
    HMONITOR mon = MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    double scale, margin, lo, hi, c, room, extent, need;

    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi))
        return FALSE;
    scale = (double)dpi_for_monitor(mon) / 96.0;
    if (scale <= 0.0)
        scale = 1.0;
    margin = ZOOM_MARGIN_DIP * scale;
    g->near_px = ZOOM_NEAR_DIP * scale;
    g->far_px = ZOOM_FAR_DIP * scale;
    g->slop_px = ZOOM_SLOP_DIP * scale;
    g->cx = at.x;
    g->cy = at.y;
    {
        double roomX = 2.0 * fmin(at.x - (mi.rcMonitor.left + margin),
                                  (mi.rcMonitor.right - 1 - margin) - at.x);
        double roomY = 2.0 * fmin(at.y - (mi.rcMonitor.top + margin),
                                  (mi.rcMonitor.bottom - 1 - margin) - at.y);
        g->vertical = roomY > roomX;
        room = g->vertical ? roomY : roomX;
    }
    if (g->vertical) {
        lo = mi.rcMonitor.top + margin;
        hi = mi.rcMonitor.bottom - 1 - margin;
        c = g->cy;
    } else {
        lo = mi.rcMonitor.left + margin;
        hi = mi.rcMonitor.right - 1 - margin;
        c = g->cx;
    }
    extent = hi - lo;
    // A pinch needs the far limit well past the near one and the slop.
    need = g->near_px * 1.5 + g->slop_px;
    if (extent < need)
        return FALSE;
    if (room < 0.0)
        room = 0.0;
    if (g->far_px > room)
        g->far_px = room;
    if (g->far_px < need) {
        g->far_px = need;
        if (c - need / 2.0 < lo)
            c = lo + need / 2.0;
        if (c + need / 2.0 > hi)
            c = hi - need / 2.0;
        if (g->vertical)
            g->cy = c;
        else
            g->cx = c;
    }
    return TRUE;
}

static BOOL touch_send(const Grip *g, UINT flags)
{
    VmTouchInfo t[2];
    int i;

    ZeroMemory(t, sizeof(t));
    for (i = 0; i < 2; i++) {
        double off = (i == 0 ? -0.5 : 0.5) * g->span;
        LONG x = (LONG)floor((g->vertical ? g->cx : g->cx + off) + 0.5);
        LONG y = (LONG)floor((g->vertical ? g->cy + off : g->cy) + 0.5);

        t[i].pointerInfo.pointerType = VM_PT_TOUCH;
        t[i].pointerInfo.pointerId = (UINT)i;
        t[i].pointerInfo.pointerFlags = flags;
        t[i].pointerInfo.ptPixelLocation.x = x;
        t[i].pointerInfo.ptPixelLocation.y = y;
        t[i].touchMask = VM_TOUCH_MASK_CONTACTAREA |
                         VM_TOUCH_MASK_ORIENTATION | VM_TOUCH_MASK_PRESSURE;
        t[i].rcContact.left = x - 2;
        t[i].rcContact.top = y - 2;
        t[i].rcContact.right = x + 2;
        t[i].rcContact.bottom = y + 2;
        t[i].orientation = 90;
        t[i].pressure = 32000;
    }
    return s_inject_touch(2, t);
}

// The pointer goes back where it was: touch moves it to the first finger.
// An injected move (not SetCursorPos) also brings back a pointer Windows
// hid for the touch; the mouse hook ignores injected moves.
static void pointer_back(POINT at)
{
    INPUT in;
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (vw <= 0)
        vw = 1;
    if (vh <= 0)
        vh = 1;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)(((LONGLONG)(at.x - vx) * 65536 + vw - 1) / vw);
    in.mi.dy = (LONG)(((LONGLONG)(at.y - vy) * 65536 + vh - 1) / vh);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                    MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof(in));
}

// The spread after `ms` of this grip: the slop at once, then growing (in) or
// shrinking (out) by zoom_speed percent a second, the same rate at any
// spread, as a pinch magnifies by the ratio of the spreads.
static double grip_span(const Grip *g, int dir, DWORD ms)
{
    double rate = 1.0 + g_cfg.zoom_speed / 100.0;
    double f = pow(rate, ms / 1000.0);

    if (dir > 0)
        return (g->near_px + g->slop_px) * f;
    return (g->far_px - g->slop_px) / f;
}

// One press, held or tapped, from the first grip to the last lift. Returns
// when the fingers are up.
static void pinch(int dir, LONG *seen_gen, LONG seen_cancel,
                  LARGE_INTEGER freq)
{
    static DWORD fail_logged_gen;
    POINT at = { 0, 0 };
    Grip g;
    DWORD start, ms;
    double tap_target, limit;
    BOOL held, lift, tapped;

    for (;;) {
        if (!GetCursorPos(&at) || !grip_place(&g, at)) {
            app_log(L"zoom: no room for two fingers at %ld,%ld",
                    at.x, at.y);
            return;
        }
        g.span = dir > 0 ? g.near_px : g.far_px;
        limit = dir > 0 ? g.far_px : g.near_px;
        tap_target = dir > 0 ? (g.near_px + g.slop_px) * ZOOM_TAP_STEP
                             : (g.far_px - g.slop_px) / ZOOM_TAP_STEP;
        if (!touch_send(&g, VM_POINTER_FLAG_DOWN | VM_POINTER_FLAG_INRANGE |
                                VM_POINTER_FLAG_INCONTACT)) {
            // Once per press: a window running as administrator refuses it
            // (ERROR_ACCESS_DENIED) unless vim mouse does too.
            if (fail_logged_gen != (DWORD)*seen_gen) {
                fail_logged_gen = (DWORD)*seen_gen;
                app_log(L"zoom: InjectTouchInput (down) failed (%lu)",
                        GetLastError());
            }
            pointer_back(at);
            return;
        }
        if (g_cfg.log)
            app_log(L"zoom: %ls at %ld,%ld, fingers %ls, %.0f to %.0f px",
                    dir > 0 ? L"in" : L"out", at.x, at.y,
                    g.vertical ? L"above and below" : L"left and right",
                    g.span, limit);
        start = now_ms(freq);
        tapped = FALSE;
        lift = FALSE;
        for (;;) {
            LONG gen;

            Sleep(ZOOM_FRAME_MS);
            if (read_long(&s_zoom_exit) ||
                read_long(&s_zoom_cancel) != seen_cancel) {
                touch_send(&g, VM_POINTER_FLAG_UP);
                pointer_back(at);
                return;
            }
            // A new press, either way: this grip ends and the press starts
            // its own.
            gen = read_long(&s_zoom_gen);
            if (gen != *seen_gen) {
                *seen_gen = gen;
                dir = read_long(&s_zoom_press_dir);
                touch_send(&g, VM_POINTER_FLAG_UP);
                pointer_back(at);
                if (dir == 0)
                    return;
                Sleep(ZOOM_REGRIP_MS);
                break;
            }
            ms = now_ms(freq) - start;
            g.span = grip_span(&g, dir, ms);
            if (dir > 0 ? g.span >= tap_target : g.span <= tap_target)
                tapped = TRUE;
            if (dir > 0 ? g.span >= limit : g.span <= limit) {
                g.span = limit;
                lift = TRUE;
            }
            held = read_long(&s_zoom_held) == dir;
            if (!held && tapped)
                lift = TRUE;
            if (!touch_send(&g, VM_POINTER_FLAG_UPDATE |
                                    VM_POINTER_FLAG_INRANGE |
                                    VM_POINTER_FLAG_INCONTACT)) {
                touch_send(&g, VM_POINTER_FLAG_UP);
                pointer_back(at);
                return;
            }
            if (lift) {
                touch_send(&g, VM_POINTER_FLAG_UP);
                pointer_back(at);
                // Still held at the limit: grip again, as a hand would.
                if (!held)
                    return;
                Sleep(ZOOM_REGRIP_MS);
                if (read_long(&s_zoom_held) != dir ||
                    read_long(&s_zoom_gen) != *seen_gen)
                    return;
                break;
            }
        }
    }
}

static DWORD WINAPI zoom_thread(LPVOID param)
{
    LARGE_INTEGER freq;
    LONG seen_gen = 0;

    (void)param;
    // Steady frames while other programs load the CPU, as for movement.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    QueryPerformanceFrequency(&freq);
    for (;;) {
        LONG gen, cancel;
        int dir;

        WaitForSingleObject(s_zoom_event, INFINITE);
        if (read_long(&s_zoom_exit))
            break;
        gen = read_long(&s_zoom_gen);
        if (gen == seen_gen)
            continue;
        seen_gen = gen;
        cancel = read_long(&s_zoom_cancel);
        dir = (int)read_long(&s_zoom_press_dir);
        if (dir == 0 || !touch_ready())
            continue;
        // Frames 8 ms apart need the 1 ms timer while a pinch runs.
        timeBeginPeriod(1);
        pinch(dir, &seen_gen, cancel, freq);
        timeEndPeriod(1);
    }
    return 0;
}

void zoom_start(void)
{
    if (s_zoom_thread)
        return;
    InterlockedExchange(&s_zoom_exit, 0);
    s_zoom_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!s_zoom_event)
        return;
    s_zoom_thread = CreateThread(NULL, 0, zoom_thread, NULL, 0, NULL);
    if (!s_zoom_thread) {
        CloseHandle(s_zoom_event);
        s_zoom_event = NULL;
    }
}

void zoom_stop(void)
{
    if (!s_zoom_thread)
        return;
    InterlockedExchange(&s_zoom_held, 0);
    InterlockedExchange(&s_zoom_exit, 1);
    SetEvent(s_zoom_event);
    // The thread lifts the fingers within a frame.
    WaitForSingleObject(s_zoom_thread, 2000);
    CloseHandle(s_zoom_thread);
    s_zoom_thread = NULL;
    CloseHandle(s_zoom_event);
    s_zoom_event = NULL;
}

void zoom_press(BOOL in)
{
    LONG dir = in ? 1 : -1;

    if (!s_zoom_event)
        return;
    InterlockedExchange(&s_zoom_held, dir);
    InterlockedExchange(&s_zoom_press_dir, dir);
    InterlockedIncrement(&s_zoom_gen);
    SetEvent(s_zoom_event);
}

void zoom_release(BOOL in)
{
    LONG dir = in ? 1 : -1;

    // Only the key that is driving the pinch; a key released after the
    // other one took over leaves it alone.
    InterlockedCompareExchange(&s_zoom_held, 0, dir);
}

void zoom_cancel(void)
{
    InterlockedExchange(&s_zoom_held, 0);
    InterlockedIncrement(&s_zoom_cancel);
    if (s_zoom_event)
        SetEvent(s_zoom_event);
}
