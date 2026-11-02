// shutdown.c - scheduled shutdown (tray "Schedule shutdown").
//
// One shutdown is pending at a time: a one-off from the tray menu ("In 15
// minutes" ... "At a time or after..."), or the next shutdown_at day from
// vim.ini, whichever comes first. A countdown window shows for
// shutdown_warning seconds before it, with Cancel and "10 more minutes", and
// then ExitWindowsEx turns the computer off.
//
// Times are wall-clock UTC in FILETIME units (100 ns), so a shutdown the
// computer slept through is noticed when it wakes: more than MISSED_MS late,
// it is skipped with a balloon instead of turning the computer off the moment
// someone wakes it. A shutdown is used up (one-off cleared, shutdown_at day
// marked done) before Windows asks the apps, so an app that stops it is not
// asked again a second later. The one-off lives in memory only: quitting or
// updating vim mouse drops it. Everything runs on the main/UI thread from one
// thread timer.

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef EWX_FORCEIFHUNG
#define EWX_FORCEIFHUNG 0x00000010
#endif
#ifndef SHTDN_REASON_MAJOR_OTHER
#define SHTDN_REASON_MAJOR_OTHER 0x00000000
#endif
#ifndef SHTDN_REASON_MINOR_OTHER
#define SHTDN_REASON_MINOR_OTHER 0x000000ff
#endif
#ifndef SHTDN_REASON_FLAG_PLANNED
#define SHTDN_REASON_FLAG_PLANNED 0x80000000
#endif

#define SEC_100NS     10000000ULL
#define MS_100NS      10000ULL
#define MINUTE_100NS  (60ULL * SEC_100NS)

#define MISSED_MS     (5 * 60 * 1000) // later than this: the computer slept through it
#define GAP_MS        5000            // a check this much later than its tick: it slept
#define NEAR_MS       (2 * 60 * 1000) // check every second this close to the countdown
#define TICK_FAR_MS   10000
#define TICK_NEAR_MS  1000
#define TICK_COUNT_MS 200             // while the countdown shows
#define POSTPONE_MIN  10              // the countdown's "10 more minutes"
#define NOW_DELAY_MS  1000            // "Shut down now" takes clicks after this
#define ONCE_MAX_MIN  (7 * 24 * 60)

#define CD_CLASS      L"VimMouseShutdownCountdown"
#define CD_TITLE      L"vim mouse - shutting down"
#define DLG_CLASS     L"VimMouseShutdownDialog"
#define DLG_TITLE     L"vim mouse - schedule shutdown"

#define ID_CD_TEXT    3001
#define ID_CD_NOTE    3002
#define ID_CD_CANCEL  3003
#define ID_CD_LATER   3004
#define ID_CD_NOW     3005
#define ID_DLG_LABEL  3101
#define ID_DLG_EDIT   3102
#define ID_DLG_NOTE   3103
#define ID_DLG_OK     3104
#define ID_DLG_CANCEL 3105

static const WCHAR DLG_NOTE[] =
    L"To shut down every day at the same time, set shutdown_at in vim.ini "
    L"(tray: Edit settings...).";
static const WCHAR DLG_BAD[] =
    L"Type a time such as 23:30, or how long to wait: 90 (minutes), 45m, "
    L"2h or 1h30 (up to 7 days).";

static BOOL      g_started;
static ULONGLONG g_once_due;       // one-off from the tray (UTC); 0 = none
static ULONGLONG g_daily_due;      // next shutdown_at (UTC); 0 = none
static int       g_daily_min = -1; // the shutdown_at g_daily_due follows
static LONG      g_daily_done;     // local day of the last shutdown_at day used up
static ULONGLONG g_deadline;       // the countdown ends here (UTC); 0 = no countdown
static ULONGLONG g_count_due;      // the shutdown the countdown is for
static ULONGLONG g_prev_now;       // wall clock at the last check
static LONG      g_tip_day;        // local day the tooltip's "tomorrow" was written on
static BOOL      g_restore;        // the one-off replaced a shutdown_at day...
static LONG      g_restore_done;   // ...and g_daily_done was this before
static LONG      g_restore_day;    // ...and is this since
static UINT_PTR  g_timer;
static UINT      g_timer_ms;

static HWND  g_cd_wnd;
static HWND  g_cd_text;
static HFONT g_cd_font;
static HFONT g_cd_big_font;
static int   g_cd_shown = -1;     // seconds the countdown text shows
static DWORD g_cd_opened;         // GetTickCount when it opened

static HWND  g_dlg_wnd;
static HWND  g_dlg_edit;
static HWND  g_dlg_note;
static HFONT g_dlg_font;
static WCHAR g_dlg_last[64] = L"1h";

static BOOL  g_classes;

static void sd_check(void);

// ---------------------------------------------------------------------------
// Time

static ULONGLONG ft_value(const FILETIME *ft)
{
    return ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

static void ft_set(FILETIME *ft, ULONGLONG v)
{
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}

static ULONGLONG wall_now(void)
{
    FILETIME ft;

    GetSystemTimeAsFileTime(&ft);
    return ft_value(&ft);
}

static BOOL to_local(ULONGLONG t, SYSTEMTIME *lt)
{
    FILETIME   ft;
    SYSTEMTIME ut;

    ft_set(&ft, t);
    return FileTimeToSystemTime(&ft, &ut) &&
           SystemTimeToTzSpecificLocalTime(NULL, &ut, lt);
}

// The time zone rules of that date, so a shutdown_at across a daylight
// saving change still happens at the clock time asked for.
static BOOL from_local(const SYSTEMTIME *lt, ULONGLONG *t)
{
    SYSTEMTIME local = *lt, ut;
    FILETIME   ft;

    if (!TzSpecificLocalTimeToSystemTime(NULL, &local, &ut) ||
        !SystemTimeToFileTime(&ut, &ft))
        return FALSE;
    *t = ft_value(&ft);
    return TRUE;
}

// Days since 1601 of a local date (the clock time is ignored).
static LONG local_day(const SYSTEMTIME *lt)
{
    SYSTEMTIME d = *lt;
    FILETIME   ft;

    d.wHour = 0;
    d.wMinute = 0;
    d.wSecond = 0;
    d.wMilliseconds = 0;
    if (!SystemTimeToFileTime(&d, &ft))
        return 0;
    return (LONG)(ft_value(&ft) / (24ULL * 60ULL * MINUTE_100NS));
}

static BOOL next_local_day(SYSTEMTIME *lt)
{
    FILETIME ft;

    if (!SystemTimeToFileTime(lt, &ft))
        return FALSE;
    ft_set(&ft, ft_value(&ft) + 24ULL * 60ULL * MINUTE_100NS);
    return FileTimeToSystemTime(&ft, lt);
}

// The first moment after `after` when the local clock shows `minute` (after
// midnight) on a local day later than skip_day; 0 if none.
static ULONGLONG clock_next(int minute, LONG skip_day, ULONGLONG after)
{
    SYSTEMTIME lt;
    ULONGLONG  t;
    int        i;

    if (minute < 0 || minute >= 24 * 60 || !to_local(after, &lt))
        return 0;
    lt.wHour = (WORD)(minute / 60);
    lt.wMinute = (WORD)(minute % 60);
    lt.wSecond = 0;
    lt.wMilliseconds = 0;
    for (i = 0; i < 10; i++) {
        if (local_day(&lt) > skip_day && from_local(&lt, &t) && t > after)
            return t;
        if (!next_local_day(&lt))
            break;
    }
    return 0;
}

// "23:30", "1:00 tomorrow", "23:30 yesterday" or "23:30 on <date>", in the
// user's time and date format.
static void format_when(ULONGLONG t, WCHAR *buf, int cch)
{
    SYSTEMTIME lt, today;
    WCHAR      time[64], date[64];
    LONG       diff;

    if (cch <= 0)
        return;
    buf[0] = L'\0';
    if (!to_local(t, &lt) || !to_local(wall_now(), &today))
        return;
    if (!GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &lt, NULL, time,
                        64)) {
        _snwprintf(time, 63, L"%d:%02d", (int)lt.wHour, (int)lt.wMinute);
        time[63] = L'\0';
    }
    diff = local_day(&lt) - local_day(&today);
    if (diff == 0) {
        lstrcpynW(buf, time, cch);
    } else if (diff == 1) {
        _snwprintf(buf, (size_t)cch - 1, L"%s tomorrow", time);
    } else if (diff == -1) {
        _snwprintf(buf, (size_t)cch - 1, L"%s yesterday", time);
    } else {
        if (!GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &lt, NULL,
                            date, 64)) {
            _snwprintf(date, 63, L"%d-%02d-%02d", (int)lt.wYear,
                       (int)lt.wMonth, (int)lt.wDay);
            date[63] = L'\0';
        }
        _snwprintf(buf, (size_t)cch - 1, L"%s on %s", time, date);
    }
    buf[cch - 1] = L'\0';
}

// "in 45 s", "in 12 min", "in 2 h", "in 1 h 5 min" (rounded up).
static void format_left(ULONGLONG t, ULONGLONG now, WCHAR *buf, int cch)
{
    ULONGLONG s = t > now ? (t - now + SEC_100NS - 1) / SEC_100NS : 0;
    unsigned  m;

    if (cch <= 0)
        return;
    if (s < 60) {
        _snwprintf(buf, (size_t)cch - 1, L"in %u s", (unsigned)s);
    } else {
        m = (unsigned)((s + 59) / 60);
        if (m < 60)
            _snwprintf(buf, (size_t)cch - 1, L"in %u min", m);
        else if (m % 60 == 0)
            _snwprintf(buf, (size_t)cch - 1, L"in %u h", m / 60);
        else
            _snwprintf(buf, (size_t)cch - 1, L"in %u h %u min", m / 60,
                       m % 60);
    }
    buf[cch - 1] = L'\0';
}

// ---------------------------------------------------------------------------
// Schedule

// The shutdown that comes first; *daily says whether it is a shutdown_at day.
static ULONGLONG next_due(BOOL *daily)
{
    *daily = FALSE;
    if (g_once_due && (!g_daily_due || g_once_due <= g_daily_due))
        return g_once_due;
    if (g_daily_due) {
        *daily = TRUE;
        return g_daily_due;
    }
    return 0;
}

// The pending shutdown_at day is done (cancelled, missed, replaced or
// carried out); the next one is the following day's.
static void daily_consume(void)
{
    SYSTEMTIME lt;

    if (!g_daily_due)
        return;
    if (to_local(g_daily_due, &lt))
        g_daily_done = local_day(&lt);
    g_daily_due = clock_next(g_daily_min, g_daily_done, wall_now());
}

static void consume(BOOL daily)
{
    if (daily) {
        daily_consume();
    } else {
        g_once_due = 0;
        g_restore = FALSE;
    }
}

// Uses up every shutdown due from lo to hi, of both kinds: shutdowns that
// close together are one to the user, who cancels or sees it carried out.
static void consume_between(ULONGLONG lo, ULONGLONG hi)
{
    int i;

    if (g_once_due && g_once_due >= lo && g_once_due <= hi)
        consume(FALSE);
    for (i = 0; i < 16 && g_daily_due && g_daily_due >= lo &&
                g_daily_due <= hi; i++)
        daily_consume();
}

// The one-off that replaced a shutdown_at day is gone (cancelled, or replaced
// in turn): that day is back, unless it is too close to count down in full
// or another shutdown_at day was used up since.
static void undo_replace(ULONGLONG now)
{
    if (!g_restore)
        return;
    g_restore = FALSE;
    if (g_daily_done != g_restore_day)
        return;
    g_daily_done = g_restore_done;
    g_daily_due = clock_next(g_daily_min, g_daily_done, now);
    if (g_daily_due && g_daily_due <= now + (ULONGLONG)g_cfg.shutdown_warning *
                                               SEC_100NS + MINUTE_100NS)
        daily_consume();
}

static void timer_set(UINT ms);

static VOID CALLBACK tick_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
    (void)hwnd;
    (void)msg;
    (void)id;
    (void)time;
    sd_check();
}

static void timer_set(UINT ms)
{
    if (g_timer && ms == g_timer_ms)
        return;
    if (g_timer) {
        KillTimer(NULL, g_timer);
        g_timer = 0;
    }
    g_timer_ms = 0;
    if (ms == 0)
        return;
    g_timer = SetTimer(NULL, 0, ms, tick_proc);
    if (g_timer)
        g_timer_ms = ms;
    else
        app_log(L"shutdown: SetTimer failed (%lu)", GetLastError());
}

// ---------------------------------------------------------------------------
// Shared window helpers

static HFONT ui_font(UINT dpi, int px, int weight)
{
    return CreateFontW(-MulDiv(px, (int)dpi, 96), 0, 0, 0, weight, FALSE,
                       FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static HWND add_child(HWND parent, const WCHAR *cls, const WCHAR *text,
                      DWORD style, DWORD ex_style, int id, HFONT font)
{
    HWND wnd = CreateWindowExW(ex_style, cls, text,
                               WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                               parent, (HMENU)(INT_PTR)id, g_hinst, NULL);
    if (wnd && font)
        SendMessageW(wnd, WM_SETFONT, (WPARAM)font, FALSE);
    return wnd;
}

// Centres a window with a client area of cw x ch (at 96 dpi) on the work area
// of the monitor under the cursor; returns that monitor's DPI.
static UINT place_window(int cw, int ch, DWORD style, DWORD ex_style,
                         RECT *out)
{
    POINT       pt = { 0, 0 };
    HMONITOR    mon;
    MONITORINFO mi;
    RECT        work, rc;
    UINT        dpi;
    int         w, h, x, y;

    GetCursorPos(&pt);
    mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    dpi = dpi_for_monitor(mon);
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi))
        work = mi.rcWork;
    else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    SetRect(&rc, 0, 0, MulDiv(cw, (int)dpi, 96), MulDiv(ch, (int)dpi, 96));
    AdjustWindowRectEx(&rc, style, FALSE, ex_style);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    x = work.left + ((int)(work.right - work.left) - w) / 2;
    y = work.top + ((int)(work.bottom - work.top) - h) / 2;
    if (x < work.left)
        x = work.left;
    if (y < work.top)
        y = work.top;
    SetRect(out, x, y, x + w, y + h);
    return dpi;
}

static void delete_font(HFONT *font)
{
    if (*font) {
        DeleteObject(*font);
        *font = NULL;
    }
}

// ---------------------------------------------------------------------------
// Countdown window

static void countdown_close(void)
{
    g_deadline = 0;
    g_count_due = 0;
    if (g_cd_wnd)
        DestroyWindow(g_cd_wnd);
}

static void countdown_layout(HWND hwnd)
{
    UINT dpi = dpi_for_monitor(MonitorFromWindow(hwnd,
                                                 MONITOR_DEFAULTTONEAREST));
    RECT rc;
    int  pad = MulDiv(16, (int)dpi, 96);
    int  gap = MulDiv(8, (int)dpi, 96);
    int  btn_h = MulDiv(30, (int)dpi, 96);
    int  w_cancel = MulDiv(104, (int)dpi, 96);
    int  w_later = MulDiv(136, (int)dpi, 96);
    int  w_now = MulDiv(128, (int)dpi, 96);
    int  text_h = MulDiv(30, (int)dpi, 96);
    int  note_h = MulDiv(46, (int)dpi, 96);
    int  inner, y;

    GetClientRect(hwnd, &rc);
    inner = rc.right - pad * 2;
    y = pad;
    MoveWindow(g_cd_text, pad, y, inner, text_h, TRUE);
    y += text_h + gap;
    MoveWindow(GetDlgItem(hwnd, ID_CD_NOTE), pad, y, inner, note_h, TRUE);
    y = rc.bottom - pad - btn_h;
    MoveWindow(GetDlgItem(hwnd, ID_CD_CANCEL), pad, y, w_cancel, btn_h, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_CD_LATER), pad + w_cancel + gap, y,
               w_later, btn_h, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_CD_NOW), rc.right - pad - w_now, y, w_now,
               btn_h, TRUE);
}

static void power_off(BOOL daily);

static LRESULT CALLBACK cd_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_CD_CANCEL:
        case IDCANCEL:
        case IDOK:
            shutdown_cancel();
            return 0;
        case ID_CD_LATER:
            shutdown_in(POSTPONE_MIN);
            return 0;
        case ID_CD_NOW: {
            BOOL daily;
            if (next_due(&daily))
                power_off(daily);
            return 0;
        }
        default:
            break;
        }
        break;
    case DM_GETDEFID:
        return MAKELONG(ID_CD_CANCEL, DC_HASDEFID);
    case WM_CLOSE:
        shutdown_cancel();
        return 0;
    case WM_DESTROY:
        g_cd_wnd = NULL;
        g_cd_text = NULL;
        g_cd_shown = -1;
        delete_font(&g_cd_font);
        delete_font(&g_cd_big_font);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void countdown_open(void)
{
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    DWORD ex_style = WS_EX_CONTROLPARENT | WS_EX_TOPMOST;
    RECT  pos;
    UINT  dpi;

    if (g_cd_wnd || !g_classes)
        return;
    dpi = place_window(460, 164, style, ex_style, &pos);
    g_cd_font = ui_font(dpi, 15, FW_NORMAL);
    g_cd_big_font = ui_font(dpi, 21, FW_BOLD);
    g_cd_wnd = CreateWindowExW(ex_style, CD_CLASS, CD_TITLE, style, pos.left,
                               pos.top, pos.right - pos.left,
                               pos.bottom - pos.top, NULL, NULL, g_hinst,
                               NULL);
    if (!g_cd_wnd) {
        app_log(L"shutdown: countdown window failed (%lu)", GetLastError());
        delete_font(&g_cd_font);
        delete_font(&g_cd_big_font);
        return;
    }
    g_cd_text = add_child(g_cd_wnd, L"STATIC", L"", SS_NOPREFIX, 0,
                          ID_CD_TEXT, g_cd_big_font);
    add_child(g_cd_wnd, L"STATIC",
              g_cfg.shutdown_force
                  ? L"Save your work now. Cancel (or Esc) keeps the computer "
                    L"on.\r\nOpen apps are closed even with unsaved work."
                  : L"Save your work now. Cancel (or Esc) keeps the computer "
                    L"on.\r\nApps with unsaved work may ask before they "
                    L"close.",
              SS_NOPREFIX, 0, ID_CD_NOTE, g_cd_font);
    // No mnemonics, and "Shut down now" is neither a tab stop nor in the
    // others' group: keys typed into this window by surprise (it takes the
    // focus) can cancel or postpone, never shut down.
    add_child(g_cd_wnd, L"BUTTON", L"Cancel",
              WS_TABSTOP | WS_GROUP | BS_DEFPUSHBUTTON, 0, ID_CD_CANCEL,
              g_cd_font);
    add_child(g_cd_wnd, L"BUTTON", L"10 more minutes",
              WS_TABSTOP | BS_PUSHBUTTON, 0, ID_CD_LATER, g_cd_font);
    // Disabled for a moment too: a click already on its way where the
    // window appears must not land on it.
    add_child(g_cd_wnd, L"BUTTON", L"Shut down now",
              WS_GROUP | WS_DISABLED | BS_PUSHBUTTON, 0, ID_CD_NOW, g_cd_font);
    countdown_layout(g_cd_wnd);
    g_cd_shown = -1;
    g_cd_opened = GetTickCount();
    ShowWindow(g_cd_wnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_cd_wnd);
    SetFocus(GetDlgItem(g_cd_wnd, ID_CD_CANCEL));
    MessageBeep(MB_ICONWARNING);
}

static void countdown_update(ULONGLONG now)
{
    WCHAR text[96];
    int   s;

    if (!g_cd_text)
        return;
    if (GetTickCount() - g_cd_opened >= NOW_DELAY_MS) {
        HWND now_btn = GetDlgItem(g_cd_wnd, ID_CD_NOW);
        if (now_btn && !IsWindowEnabled(now_btn))
            EnableWindow(now_btn, TRUE);
    }
    s = g_deadline > now
            ? (int)((g_deadline - now + SEC_100NS - 1) / SEC_100NS)
            : 0;
    if (s == g_cd_shown)
        return;
    g_cd_shown = s;
    _snwprintf(text, 95, L"The computer shuts down in %d:%02d", s / 60,
               s % 60);
    text[95] = L'\0';
    SetWindowTextW(g_cd_text, text);
}

// ---------------------------------------------------------------------------
// Shutting down

static void power_off(BOOL daily)
{
    HANDLE           token;
    TOKEN_PRIVILEGES tp;
    UINT             flags;
    WCHAR            text[256];
    DWORD            err;

    // Used up first: if an app stops the shutdown, it is not tried again,
    // and any other one due before its countdown could end goes with it.
    consume(daily);
    consume_between(0, wall_now() +
                           (ULONGLONG)g_cfg.shutdown_warning * SEC_100NS +
                           MINUTE_100NS);
    countdown_close();
    tray_update();

    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        ZeroMemory(&tp, sizeof(tp));
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueW(NULL, L"SeShutdownPrivilege",
                                  &tp.Privileges[0].Luid))
            AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
        CloseHandle(token);
    }

    // EWX_FORCEIFHUNG: an app that does not answer is closed; one that asks
    // to save its work may still hold the shutdown up (shutdown_force).
    flags = EWX_SHUTDOWN | EWX_POWEROFF |
            (g_cfg.shutdown_force ? EWX_FORCE : EWX_FORCEIFHUNG);
    if (ExitWindowsEx(flags, SHTDN_REASON_MAJOR_OTHER |
                                 SHTDN_REASON_MINOR_OTHER |
                                 SHTDN_REASON_FLAG_PLANNED)) {
        app_log(L"shutdown: shutting down%s",
                g_cfg.shutdown_force ? L" (apps closed by force)" : L"");
        return;
    }
    err = GetLastError();
    app_log(L"shutdown: ExitWindowsEx failed (%lu)", err);
    _snwprintf(text, 255, L"Windows did not shut down (error %lu). Use the "
               L"Start menu to shut down.", err);
    text[255] = L'\0';
    tray_balloon(L"Could not shut down", text);
}

// A shutdown the computer slept (or was off) through.
static void missed(BOOL daily, ULONGLONG due)
{
    WCHAR at[96], text[256];

    format_when(due, at, 96);
    consume(daily);
    countdown_close();
    app_log(L"shutdown: the one at %s was missed (asleep, or the clock "
            L"moved)", at);
    _snwprintf(text, 255, L"The computer was asleep at %s (or its clock was "
               L"changed), so it was not shut down and stays on.", at);
    text[255] = L'\0';
    tray_balloon(L"Shutdown skipped", text);
    tray_update();
}

static void sd_check(void)
{
    int guard;

    if (!g_started)
        return;
    // A missed or carried-out shutdown can leave another one: look again.
    for (guard = 0; guard < 8; guard++) {
        ULONGLONG now = wall_now(), due, warn;
        BOOL      daily, gap;

        gap = g_prev_now != 0 &&
              (now > g_prev_now + (ULONGLONG)(g_timer_ms + GAP_MS) * MS_100NS ||
               now + SEC_100NS < g_prev_now);
        g_prev_now = now;

        due = next_due(&daily);
        if (g_deadline && due != g_count_due)
            countdown_close();
        if (!due) {
            timer_set(0);
            return;
        }
        warn = (ULONGLONG)g_cfg.shutdown_warning * SEC_100NS;

        // After midnight the tooltip's "1:00 tomorrow" is today's.
        {
            SYSTEMTIME lt;
            if (to_local(now, &lt) && local_day(&lt) != g_tip_day) {
                g_tip_day = local_day(&lt);
                tray_update();
            }
        }

        if (g_deadline) {
            if (now > g_deadline + (ULONGLONG)MISSED_MS * MS_100NS) {
                missed(daily, due);
                continue;
            }
            // The clock was set back: count down again when it is due.
            if (due > now + warn + SEC_100NS) {
                countdown_close();
                continue;
            }
            // It slept with the countdown on screen: show it in full again.
            if (gap)
                g_deadline = due > now + warn ? due : now + warn;
            if (now >= g_deadline) {
                power_off(daily);
                continue;
            }
            countdown_update(now);
            timer_set(TICK_COUNT_MS);
            return;
        }

        if (now > due + (ULONGLONG)MISSED_MS * MS_100NS) {
            missed(daily, due);
            continue;
        }
        if (now + warn >= due) {
            // On time it ends at the shutdown; late (after a sleep) it
            // still counts down in full first.
            g_count_due = due;
            g_deadline = due > now ? due : now + warn;
            if (now >= g_deadline) {
                power_off(daily);
                continue;
            }
            app_log(L"shutdown: counting down");
            countdown_open();
            countdown_update(now);
            timer_set(TICK_COUNT_MS);
            return;
        }
        timer_set(due - warn - now > (ULONGLONG)NEAR_MS * MS_100NS
                      ? TICK_FAR_MS
                      : TICK_NEAR_MS);
        return;
    }
}

static void schedule_once(ULONGLONG due)
{
    ULONGLONG now = wall_now();
    WCHAR     at[96], left[48], skipped[96], first[96], text[256];
    BOOL      daily;

    if (!due)
        return;
    // A new one-off replaces the old one, and with it the old one's
    // replacement of a shutdown_at day.
    if (g_once_due)
        undo_replace(now);
    g_once_due = 0;
    skipped[0] = L'\0';
    // The next shutdown_at day before it would turn the computer off first;
    // the one-off asked for now takes its place (wanting to stay on longer
    // tonight). Later days still come first if it is days away.
    if (g_daily_due && g_daily_due < due) {
        format_when(g_daily_due, skipped, 96);
        g_restore_done = g_daily_done;
        daily_consume();
        g_restore_day = g_daily_done;
        g_restore = TRUE;
    }
    g_once_due = due;
    countdown_close();

    format_when(due, at, 96);
    format_left(due, now, left, 48);
    app_log(L"shutdown: scheduled for %s (%s)", at, left);
    if (next_due(&daily) != due) {
        format_when(g_daily_due, first, 96);
        _snwprintf(text, 255, L"Scheduled for %s (%s), but shutdown_at turns "
                   L"the computer off before that, at %s.", at, left, first);
    } else if (skipped[0]) {
        _snwprintf(text, 255, L"The computer shuts down at %s (%s), instead "
                   L"of at %s (shutdown_at). Cancel it from the tray menu.",
                   at, left, skipped);
    } else {
        _snwprintf(text, 255, L"The computer shuts down at %s (%s). Cancel "
                   L"it from the tray menu.", at, left);
    }
    text[255] = L'\0';
    tray_balloon(L"Shutdown scheduled", text);
    tray_update();
    sd_check();
}

// ---------------------------------------------------------------------------
// "At a time or after..." window

// "23:30" is a clock time (*minutes after midnight); "90", "90m", "45 min",
// "2h", "1h30", "1h30m" a wait (*minutes from now).
static BOOL parse_when(const WCHAR *s, BOOL *clock, int *minutes)
{
    WCHAR        buf[64];
    const WCHAR *p;
    long         hours = -1, mins = -1, num;
    int          n = 0;

    for (; *s && n < 63; s++) {
        WCHAR ch = *s;
        if (ch == L' ' || ch == L'\t')
            continue;
        if (ch >= L'A' && ch <= L'Z')
            ch = (WCHAR)(ch + 32);
        buf[n++] = ch;
    }
    buf[n] = L'\0';
    if (n == 0)
        return FALSE;
    if (wcschr(buf, L':')) {
        *clock = TRUE;
        return parse_clock(buf, minutes);
    }

    *clock = FALSE;
    p = buf;
    while (*p) {
        if (*p < L'0' || *p > L'9')
            return FALSE;
        num = 0;
        while (*p >= L'0' && *p <= L'9') {
            num = num * 10 + (*p - L'0');
            if (num > ONCE_MAX_MIN)
                return FALSE;
            p++;
        }
        if (*p == L'h' && hours < 0 && mins < 0) {
            hours = num;
            p++;
            if (wcsncmp(p, L"ours", 4) == 0)
                p += 4;
            else if (wcsncmp(p, L"our", 3) == 0)
                p += 3;
        } else if (mins < 0) {
            mins = num;
            if (wcsncmp(p, L"min", 3) == 0)
                p += 3;
            else if (*p == L'm')
                p++;
            if (*p)
                return FALSE;
        } else {
            return FALSE;
        }
    }
    num = (hours > 0 ? hours * 60 : 0) + (mins > 0 ? mins : 0);
    if (num < 1 || num > ONCE_MAX_MIN)
        return FALSE;
    *minutes = (int)num;
    return TRUE;
}

static void dlg_submit(void)
{
    WCHAR text[64];
    BOOL  clock = FALSE;
    int   v = 0;

    GetWindowTextW(g_dlg_edit, text, 64);
    text[63] = L'\0';
    if (!parse_when(text, &clock, &v)) {
        SetWindowTextW(g_dlg_note, DLG_BAD);
        MessageBeep(MB_ICONWARNING);
        SetFocus(g_dlg_edit);
        SendMessageW(g_dlg_edit, EM_SETSEL, 0, -1);
        return;
    }
    lstrcpynW(g_dlg_last, text, 64);
    DestroyWindow(g_dlg_wnd);
    if (clock)
        schedule_once(clock_next(v, 0, wall_now()));
    else
        shutdown_in(v);
}

static void dlg_layout(HWND hwnd)
{
    UINT dpi = dpi_for_monitor(MonitorFromWindow(hwnd,
                                                 MONITOR_DEFAULTTONEAREST));
    RECT rc;
    int  pad = MulDiv(14, (int)dpi, 96);
    int  gap = MulDiv(8, (int)dpi, 96);
    int  label_h = MulDiv(40, (int)dpi, 96);
    int  edit_h = MulDiv(26, (int)dpi, 96);
    int  note_h = MulDiv(40, (int)dpi, 96);
    int  btn_w = MulDiv(96, (int)dpi, 96);
    int  btn_h = MulDiv(30, (int)dpi, 96);
    int  inner, y;

    GetClientRect(hwnd, &rc);
    inner = rc.right - pad * 2;
    y = pad;
    MoveWindow(GetDlgItem(hwnd, ID_DLG_LABEL), pad, y, inner, label_h, TRUE);
    y += label_h + gap / 2;
    MoveWindow(g_dlg_edit, pad, y, MulDiv(180, (int)dpi, 96), edit_h, TRUE);
    y += edit_h + gap;
    MoveWindow(g_dlg_note, pad, y, inner, note_h, TRUE);
    y = rc.bottom - pad - btn_h;
    MoveWindow(GetDlgItem(hwnd, ID_DLG_OK), rc.right - pad - btn_w * 2 - gap,
               y, btn_w, btn_h, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_DLG_CANCEL), rc.right - pad - btn_w, y,
               btn_w, btn_h, TRUE);
}

static LRESULT CALLBACK dlg_wnd_proc(HWND hwnd, UINT msg, WPARAM wp,
                                     LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_DLG_OK:
        case IDOK:
            dlg_submit();
            return 0;
        case ID_DLG_CANCEL:
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case DM_GETDEFID:
        return MAKELONG(ID_DLG_OK, DC_HASDEFID);
    case WM_DESTROY:
        g_dlg_wnd = NULL;
        g_dlg_edit = NULL;
        g_dlg_note = NULL;
        delete_font(&g_dlg_font);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void shutdown_dialog_open(void)
{
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    DWORD ex_style = WS_EX_CONTROLPARENT;
    RECT  pos;
    UINT  dpi;

    if (g_dlg_wnd) {
        SetForegroundWindow(g_dlg_wnd);
        return;
    }
    if (!g_classes)
        return;
    dpi = place_window(440, 182, style, ex_style, &pos);
    g_dlg_font = ui_font(dpi, 15, FW_NORMAL);
    g_dlg_wnd = CreateWindowExW(ex_style, DLG_CLASS, DLG_TITLE, style,
                                pos.left, pos.top, pos.right - pos.left,
                                pos.bottom - pos.top, NULL, NULL, g_hinst,
                                NULL);
    if (!g_dlg_wnd) {
        app_log(L"shutdown: dialog failed (%lu)", GetLastError());
        delete_font(&g_dlg_font);
        return;
    }
    add_child(g_dlg_wnd, L"STATIC",
              L"Shut the computer down at a time, such as 23:30, or after a "
              L"while: 90 (minutes), 45m, 2h or 1h30.",
              SS_NOPREFIX, 0, ID_DLG_LABEL, g_dlg_font);
    g_dlg_edit = add_child(g_dlg_wnd, L"EDIT", g_dlg_last,
                           WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
                           ID_DLG_EDIT, g_dlg_font);
    SendMessageW(g_dlg_edit, EM_LIMITTEXT, 32, 0);
    g_dlg_note = add_child(g_dlg_wnd, L"STATIC", DLG_NOTE, SS_NOPREFIX, 0,
                           ID_DLG_NOTE, g_dlg_font);
    add_child(g_dlg_wnd, L"BUTTON", L"&Schedule",
              WS_TABSTOP | BS_DEFPUSHBUTTON, 0, ID_DLG_OK, g_dlg_font);
    add_child(g_dlg_wnd, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 0,
              ID_DLG_CANCEL, g_dlg_font);
    dlg_layout(g_dlg_wnd);
    ShowWindow(g_dlg_wnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_dlg_wnd);
    SetFocus(g_dlg_edit);
    SendMessageW(g_dlg_edit, EM_SETSEL, 0, -1);
}

// ---------------------------------------------------------------------------
// Public

static BOOL register_classes(void)
{
    WNDCLASSEXW wc;

    if (g_classes)
        return TRUE;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = cd_wnd_proc;
    wc.hInstance     = g_hinst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon         = app_load_icon(GetSystemMetrics(SM_CXICON),
                                     GetSystemMetrics(SM_CYICON));
    wc.hIconSm       = app_load_icon(GetSystemMetrics(SM_CXSMICON),
                                     GetSystemMetrics(SM_CYSMICON));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = CD_CLASS;
    if (!RegisterClassExW(&wc))
        return FALSE;
    wc.lpfnWndProc   = dlg_wnd_proc;
    wc.lpszClassName = DLG_CLASS;
    if (!RegisterClassExW(&wc)) {
        UnregisterClassW(CD_CLASS, g_hinst);
        return FALSE;
    }
    g_classes = TRUE;
    return TRUE;
}

void shutdown_init(void)
{
    if (!register_classes())
        app_log(L"shutdown: RegisterClassEx failed (%lu)", GetLastError());
    g_started = TRUE;
    g_daily_min = -1;
    shutdown_config_changed();
}

void shutdown_stop(void)
{
    g_started = FALSE;
    timer_set(0);
    countdown_close();
    if (g_dlg_wnd)
        DestroyWindow(g_dlg_wnd);
}

void shutdown_config_changed(void)
{
    if (!g_started)
        return;
    if (g_cfg.shutdown_at != g_daily_min) {
        g_daily_min = g_cfg.shutdown_at;
        g_daily_done = 0;
        g_restore = FALSE;
        g_daily_due = clock_next(g_daily_min, 0, wall_now());
        if (g_daily_due) {
            WCHAR at[96];
            format_when(g_daily_due, at, 96);
            app_log(L"shutdown: shutdown_at %d:%02d, next at %s",
                    g_daily_min / 60, g_daily_min % 60, at);
        } else {
            app_log(L"shutdown: no shutdown_at");
        }
        tray_update();
    }
    sd_check();
}

void shutdown_time_changed(void)
{
    ULONGLONG now = wall_now();
    BOOL      daily;

    if (!g_started)
        return;
    // The time zone may have changed: shutdown_at follows the local clock.
    // One already due stays for sd_check to count down late or skip (Windows
    // also resyncs the clock on waking), and a countdown keeps its time.
    // g_prev_now stays, so a jump still counts as a sleep.
    if (g_daily_min >= 0 && g_daily_due > now &&
        !(g_deadline && next_due(&daily) && daily))
        g_daily_due = clock_next(g_daily_min, g_daily_done, now);
    tray_update();
    sd_check();
}

void shutdown_in(int minutes)
{
    if (minutes < 1)
        minutes = 1;
    if (minutes > ONCE_MAX_MIN)
        minutes = ONCE_MAX_MIN;
    schedule_once(wall_now() + (ULONGLONG)minutes * MINUTE_100NS);
}

void shutdown_cancel(void)
{
    ULONGLONG now = wall_now(), due, hi;
    BOOL      daily;
    WCHAR     at[96], next_at[96], left[48], text[256];

    due = next_due(&daily);
    if (!due) {
        countdown_close();
        return;
    }
    format_when(due, at, 96);
    hi = (g_deadline > due ? g_deadline : due) + MINUTE_100NS;
    if (daily) {
        daily_consume();
    } else {
        g_once_due = 0;
        undo_replace(now);
    }
    // One due with it (a one-off typed for shutdown_at's time) is cancelled
    // too, or it would count down again at once.
    consume_between(due, hi);
    countdown_close();
    app_log(L"shutdown: the one at %s cancelled", at);

    due = next_due(&daily);
    if (due) {
        format_when(due, next_at, 96);
        format_left(due, now, left, 48);
        _snwprintf(text, 255, L"The shutdown at %s is cancelled. The next "
                   L"one is at %s (%s%s).", at, next_at, left,
                   daily ? L", shutdown_at" : L"");
    } else {
        _snwprintf(text, 255, L"The shutdown at %s is cancelled. The "
                   L"computer stays on.", at);
    }
    text[255] = L'\0';
    tray_balloon(L"Shutdown cancelled", text);
    tray_update();
    sd_check();
}

BOOL shutdown_next(WCHAR *at, int at_cch, WCHAR *left, int left_cch)
{
    BOOL      daily;
    ULONGLONG due = g_started ? next_due(&daily) : 0;

    if (!due)
        return FALSE;
    if (at)
        format_when(due, at, at_cch);
    if (left)
        format_left(g_deadline ? g_deadline : due, wall_now(), left,
                    left_cch);
    return TRUE;
}

BOOL shutdown_dialog_message(MSG *msg)
{
    if (g_cd_wnd && IsDialogMessageW(g_cd_wnd, msg))
        return TRUE;
    return g_dlg_wnd != NULL && IsDialogMessageW(g_dlg_wnd, msg);
}
