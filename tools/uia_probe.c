// uia_probe.c - console probe for uia.c. Read-only: it never sends input.

#include "../uia.c"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>

static void print_wide(const WCHAR *text)
{
    char utf8[16384];
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8),
                                NULL, NULL);
    if (n > 1)
        fwrite(utf8, 1, (size_t)(n - 1), stdout);
}

static void print_line(const WCHAR *fmt, ...)
{
    WCHAR line[2048];
    va_list ap;

    va_start(ap, fmt);
    _vsnwprintf(line, 2048, fmt, ap);
    va_end(ap);
    line[2047] = 0;
    print_wide(line);
}

Config g_cfg;
HWND g_main_wnd = NULL;

void app_log(const WCHAR *fmt, ...)
{
    WCHAR line[1024];
    va_list ap;

    va_start(ap, fmt);
    _vsnwprintf(line, 1024, fmt, ap);
    va_end(ap);
    line[1023] = 0;
    print_wide(line);
    print_wide(L"\n");
}

UINT dpi_for_monitor(HMONITOR mon)
{
    (void)mon;
    return 96;
}

typedef BOOL (WINAPI *SetDpiCtxFn)(HANDLE);
typedef HRESULT (WINAPI *SetDpiAwarenessFn)(int);
typedef BOOL (WINAPI *SetDpiAwareFn)(void);

#define DPI_CTX_PER_MONITOR_V2 ((HANDLE)-4)

static void set_dpi_awareness(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    SetDpiCtxFn ctx_fn = NULL;
    SetDpiAwarenessFn awareness_fn = NULL;
    SetDpiAwareFn aware_fn = NULL;

    if (user32) {
        ctx_fn = (SetDpiCtxFn)(void *)GetProcAddress(
            user32, "SetProcessDpiAwarenessContext");
        aware_fn = (SetDpiAwareFn)(void *)GetProcAddress(
            user32, "SetProcessDPIAware");
    }
    if (shcore)
        awareness_fn = (SetDpiAwarenessFn)(void *)GetProcAddress(
            shcore, "SetProcessDpiAwareness");

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

static void clean_title(HWND hwnd, WCHAR *out, int cch)
{
    WCHAR tmp[512];
    int n = GetWindowTextW(hwnd, tmp, 512);
    int i;
    int j = 0;

    out[0] = 0;
    if (n <= 0)
        return;
    for (i = 0; i < n && j < cch - 1 && j < 40; ++i) {
        WCHAR ch = tmp[i];
        if (ch < 32)
            ch = L' ';
        out[j++] = ch;
    }
    out[j] = 0;
}

static void clean_element_name(const WCHAR *in, WCHAR *out, int cch)
{
    int i;
    int j = 0;

    out[0] = 0;
    if (!in)
        return;
    for (i = 0; in[i] && j < cch - 1; ++i) {
        WCHAR ch = in[i];
        if (ch < 32)
            ch = L' ';
        out[j++] = ch;
    }
    out[j] = 0;
}

static const WCHAR *control_type_name(int id)
{
    switch (id) {
    case UIA_ButtonControlTypeId:      return L"Button";
    case UIA_CheckBoxControlTypeId:    return L"CheckBox";
    case UIA_ComboBoxControlTypeId:    return L"ComboBox";
    case UIA_EditControlTypeId:        return L"Edit";
    case UIA_HyperlinkControlTypeId:   return L"Hyperlink";
    case UIA_ListItemControlTypeId:    return L"ListItem";
    case UIA_MenuItemControlTypeId:    return L"MenuItem";
    case UIA_RadioButtonControlTypeId: return L"RadioButton";
    case UIA_TabItemControlTypeId:     return L"TabItem";
    case UIA_TreeItemControlTypeId:    return L"TreeItem";
    case UIA_DataItemControlTypeId:    return L"DataItem";
    case UIA_SplitButtonControlTypeId: return L"SplitButton";
    case UIA_HeaderItemControlTypeId:  return L"HeaderItem";
    default:                           return NULL;
    }
}

typedef struct {
    int windows;
    int scanned;
} ProbeReport;

static void probe_on_window(void *user, const WindowEvent *event)
{
    ProbeReport *report = (ProbeReport *)user;
    WCHAR title[41];

    clean_title(event->hwnd, title, 41);
    ++report->windows;
    if (event->skip) {
        print_line(L"%4d %-24ls \"%ls\" skipped (%ls)\n",
                   report->windows, event->cls ? event->cls : L"", title,
                   event->skip);
    } else {
        ++report->scanned;
        print_line(L"%4d %-24ls \"%ls\" %d targets, %lu ms\n",
                   report->windows, event->cls ? event->cls : L"", title,
                   event->added, event->elapsed_ms);
    }
}

static void print_targets(const ScanResult *result)
{
    int i;

    print_line(L"  targets:\n");
    for (i = 0; i < result->count; ++i) {
        const ScanTarget *target = &result->items[i];
        const WCHAR *type = control_type_name(target->control_type);
        WCHAR name[129];
        WCHAR type_buf[32];

        clean_element_name(target->name, name, 129);
        if (!type) {
            _snwprintf(type_buf, 32, L"Type %d", target->control_type);
            type_buf[31] = 0;
            type = type_buf;
        }
        print_line(L"    %-12ls \"%ls\" (%ld,%ld)-(%ld,%ld) click (%ld,%ld)\n",
                   type, name,
                   target->target.rc.left, target->target.rc.top,
                   target->target.rc.right, target->target.rc.bottom,
                   target->target.click.x, target->target.click.y);
    }
}

static void usage(void)
{
    print_line(L"usage: uia_probe [monitor|window] [-v] [-n runs]\n");
}

int main(int argc, char **argv)
{
    int scope = SCAN_MONITOR;
    int verbose = 0;
    int runs = 1;
    int i;
    int run;
    HRESULT hr;

    for (i = 1; i < argc; ++i) {
        if (!_stricmp(argv[i], "monitor")) {
            scope = SCAN_MONITOR;
        } else if (!_stricmp(argv[i], "window")) {
            scope = SCAN_WINDOW;
        } else if (!strcmp(argv[i], "-v")) {
            verbose = 1;
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            runs = atoi(argv[++i]);
            if (runs < 1)
                runs = 1;
        } else {
            usage();
            return 2;
        }
    }

    set_dpi_awareness();
    g_cfg.scan_scope = scope;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr != S_OK && hr != S_FALSE) {
        print_line(L"COM initialization failed (0x%08lx)\n",
                   (unsigned long)hr);
        return 1;
    }

    {
        UiaEngine engine;
        POINT cursor;
        HMONITOR mon;
        MONITORINFO mi;

        if (!uia_engine_init(&engine)) {
            print_line(L"UI Automation unavailable\n");
            CoUninitialize();
            return 1;
        }

        GetCursorPos(&cursor);
        mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);

        print_line(L"monitor (%ld,%ld)-(%ld,%ld), scope %ls\n",
                   mi.rcMonitor.left, mi.rcMonitor.top,
                   mi.rcMonitor.right, mi.rcMonitor.bottom,
                   scope == SCAN_WINDOW ? L"window" : L"monitor");

        for (run = 0; run < runs; ++run) {
            ScanResult result;
            ProbeReport report;
            DWORD started;
            DWORD elapsed;

            memset(&result, 0, sizeof(result));
            memset(&report, 0, sizeof(report));
            print_line(L"run %d:\n", run + 1);
            started = GetTickCount();
            uia_scan_monitor(&engine, mon, NULL, NULL, probe_on_window,
                             &report, &result);
            elapsed = GetTickCount() - started;
            print_line(L"total: %d targets, %lu ms, %d windows (%d scanned)\n",
                       result.count, elapsed, report.windows, report.scanned);
            if (verbose)
                print_targets(&result);
            free(result.items);
        }

        uia_engine_cleanup(&engine);
    }

    CoUninitialize();
    return 0;
}
