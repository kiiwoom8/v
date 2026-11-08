// zoom_probe.c - runs zoom.c's pinch without the keyboard hook (make zoomprobe).
// usage: zoom_probe in|out [hold_ms] [x y]
//   presses the zoom key, holds it hold_ms (0 = a tap; default 0), releases
//   it and waits for the fingers to lift. With x y the pointer goes there
//   first. Prints zoom.c's log lines (log = 1), so it says whether Windows
//   took the touch and how far the fingers went. vim mouse ignores injected
//   keys, so this is how the pinch itself is tested; read the page's
//   visualViewport.scale before and after to see whether the app zoomed.
#include "../zoom.c"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Config g_cfg;

void app_log(const WCHAR *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    printf("%8lu ", (unsigned long)GetTickCount());
    vwprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

typedef HRESULT (WINAPI *GetDpiForMonitorFn)(HMONITOR, int, UINT *, UINT *);
typedef BOOL (WINAPI *SetDpiCtxFn)(HANDLE);

UINT dpi_for_monitor(HMONITOR mon)
{
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    GetDpiForMonitorFn fn = shcore ? (GetDpiForMonitorFn)(void *)
        GetProcAddress(shcore, "GetDpiForMonitor") : NULL;
    UINT x = 96, y = 96;

    if (!fn || fn(mon, 0, &x, &y) != S_OK)
        return 96;
    return x;
}

int main(int argc, char **argv)
{
    SetDpiCtxFn set_ctx = (SetDpiCtxFn)(void *)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    int hold = 0;
    BOOL in;
    POINT p;

    if (argc < 2 || (strcmp(argv[1], "in") && strcmp(argv[1], "out"))) {
        fprintf(stderr, "usage: zoom_probe in|out [hold_ms] [x y]\n");
        return 2;
    }
    in = strcmp(argv[1], "in") == 0;
    if (argc >= 3)
        hold = atoi(argv[2]);
    // Physical pixels, as vim mouse sees them (per-monitor v2).
    if (set_ctx)
        set_ctx((HANDLE)(LONG_PTR)-4);
    g_cfg.log = 1;
    g_cfg.zoom_speed = 150;
    if (argc >= 5)
        SetCursorPos(atoi(argv[3]), atoi(argv[4]));
    GetCursorPos(&p);
    printf("probe: %s, hold %d ms, pointer %ld,%ld\n", in ? "in" : "out",
           hold, p.x, p.y);
    zoom_start();
    zoom_press(in);
    Sleep(hold > 0 ? (DWORD)hold : 1);
    zoom_release(in);
    // A tap or the last grip finishes on its own; give it time, then stop.
    Sleep(1500);
    zoom_stop();
    GetCursorPos(&p);
    printf("probe: done, pointer %ld,%ld\n", p.x, p.y);
    return 0;
}
