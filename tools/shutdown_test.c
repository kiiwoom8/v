// shutdown_test.c - checks the scheduled shutdown's timing (shutdown.c) with a
// fake clock and a fake ExitWindowsEx, so it never shuts anything down.
//
//   mingw32-make shutdown-test
//   tools\shutdown_test.exe [-v]        (on Linux: wine tools/shutdown_test.exe)
//
// -v also prints the log lines and balloons. Exit code 0 = every check passed.
// The scenarios use fixed dates around 2026-11-01, when daylight saving time
// ends in the US; with the time zone set to one of those (TZ=America/New_York
// under Wine) the two 23:30s around it are 25 hours apart.

#include <stdio.h>

#include "common.h"

static ULONGLONG g_fake_now;
static int       g_exits;
static int       g_verbose;
static WCHAR     g_last_title[128];

static void WINAPI fake_now(LPFILETIME ft)
{
    ft->dwLowDateTime = (DWORD)g_fake_now;
    ft->dwHighDateTime = (DWORD)(g_fake_now >> 32);
}

static BOOL WINAPI fake_exit(UINT flags, DWORD reason)
{
    (void)flags;
    (void)reason;
    g_exits++;
    return TRUE;
}

#define GetSystemTimeAsFileTime fake_now
#define ExitWindowsEx fake_exit
#include "shutdown.c"
#undef GetSystemTimeAsFileTime
#undef ExitWindowsEx
#include "config.c"

// What the rest of the program would provide.
Config    g_cfg;
HWND      g_main_wnd;
HINSTANCE g_hinst;

void app_log(const WCHAR *fmt, ...)
{
    WCHAR   buf[1024];
    va_list ap;

    if (!g_verbose)
        return;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    va_end(ap);
    buf[1023] = L'\0';
    wprintf(L"    log: %ls\n", buf);
}

void tray_balloon(const WCHAR *title, const WCHAR *text)
{
    lstrcpynW(g_last_title, title, 128);
    if (g_verbose)
        wprintf(L"    balloon: [%ls] %ls\n", title, text);
}

void tray_update(void)
{
}

UINT dpi_for_monitor(HMONITOR mon)
{
    (void)mon;
    return 96;
}

HICON app_load_icon(int cx, int cy)
{
    (void)cx;
    (void)cy;
    return NULL;
}

static int g_fails, g_checks;

#define CHECK(c)                                                         \
    do {                                                                 \
        g_checks++;                                                      \
        if (!(c)) {                                                      \
            g_fails++;                                                   \
            wprintf(L"  FAIL line %d: %hs\n", __LINE__, #c);             \
        }                                                                \
    } while (0)

static ULONGLONG at_local(int y, int mo, int d, int h, int mi, int s)
{
    SYSTEMTIME lt;
    ULONGLONG  t = 0;

    ZeroMemory(&lt, sizeof(lt));
    lt.wYear = (WORD)y;
    lt.wMonth = (WORD)mo;
    lt.wDay = (WORD)d;
    lt.wHour = (WORD)h;
    lt.wMinute = (WORD)mi;
    lt.wSecond = (WORD)s;
    from_local(&lt, &t);
    return t;
}

// Moves the clock on in 200 ms steps, with a timer tick at each.
static void advance(int secs)
{
    int i;

    for (i = 0; i < secs * 5; i++) {
        g_fake_now += 2000000ULL;
        sd_check();
    }
}

static void reset(const WCHAR *name, ULONGLONG now)
{
    wprintf(L"%ls\n", name);
    g_once_due = 0;
    g_daily_due = 0;
    g_daily_min = -1;
    g_daily_done = 0;
    countdown_close();
    g_prev_now = 0;
    g_exits = 0;
    g_last_title[0] = L'\0';
    g_cfg.shutdown_at = -1;
    g_cfg.shutdown_warning = 60;
    g_fake_now = now;
}

static void set_daily(int h, int m)
{
    g_cfg.shutdown_at = h * 60 + m;
    shutdown_config_changed();
}

static void test_parsing(void)
{
    BOOL clock = FALSE;
    int  v = 0;

    wprintf(L"typed times and waits\n");
    CHECK(parse_when(L"23:30", &clock, &v) && clock && v == 23 * 60 + 30);
    CHECK(parse_when(L" 7:05 ", &clock, &v) && clock && v == 7 * 60 + 5);
    CHECK(!parse_when(L"24:00", &clock, &v));
    CHECK(!parse_when(L"7:5", &clock, &v));
    CHECK(!parse_when(L"123:00", &clock, &v));
    CHECK(parse_when(L"90", &clock, &v) && !clock && v == 90);
    CHECK(parse_when(L"45m", &clock, &v) && v == 45);
    CHECK(parse_when(L"45 min", &clock, &v) && v == 45);
    CHECK(parse_when(L"2h", &clock, &v) && v == 120);
    CHECK(parse_when(L"2 Hours", &clock, &v) && v == 120);
    CHECK(parse_when(L"1h30", &clock, &v) && v == 90);
    CHECK(parse_when(L"1h 30m", &clock, &v) && v == 90);
    CHECK(parse_when(L"1hour30min", &clock, &v) && v == 90);
    CHECK(parse_when(L"168h", &clock, &v) && v == 7 * 24 * 60);
    CHECK(!parse_when(L"0", &clock, &v));
    CHECK(!parse_when(L"", &clock, &v));
    CHECK(!parse_when(L"h", &clock, &v));
    CHECK(!parse_when(L"30m1h", &clock, &v));
    CHECK(!parse_when(L"1h2h", &clock, &v));
    CHECK(!parse_when(L"1.5h", &clock, &v));
    CHECK(!parse_when(L"200h", &clock, &v));
    CHECK(!parse_when(L"soon", &clock, &v));

    wprintf(L"vim.ini settings\n");
    {
        Config c;
        int    sec = SEC_GENERAL;
        WCHAR  l1[] = L"shutdown_at = 23:30";
        WCHAR  l2[] = L"shutdown_warning = 900";
        WCHAR  l3[] = L"shutdown_force = 1";
        WCHAR  l4[] = L"shutdown_at = none";
        WCHAR  l5[] = L"shutdown_at = 25:00";
        WCHAR *tpl;

        config_set_defaults(&c);
        CHECK(c.shutdown_at == -1 && c.shutdown_warning == 60 &&
              c.shutdown_force == 0);
        g_warnings[0] = 0;
        process_line(&c, 1, l1, &sec);
        CHECK(c.shutdown_at == 23 * 60 + 30);
        process_line(&c, 2, l2, &sec);
        CHECK(c.shutdown_warning == 300);
        process_line(&c, 3, l3, &sec);
        CHECK(c.shutdown_force == 1);
        process_line(&c, 4, l4, &sec);
        CHECK(c.shutdown_at == -1);
        process_line(&c, 5, l5, &sec);
        CHECK(c.shutdown_at == -1 && wcsstr(g_warnings, L"25:00") != NULL);

        // The default file has to fit write_default_file's buffer.
        tpl = (WCHAR *)calloc(65536, sizeof(WCHAR));
        if (tpl) {
            build_default_ini(tpl, 65536);
            CHECK(wcslen(tpl) < 16000);
            CHECK(wcsstr(tpl, L"shutdown_at = none") != NULL);
            free(tpl);
        }
    }
}

static void test_clock(void)
{
    ULONGLONG  t, t2;
    SYSTEMTIME lt;

    wprintf(L"next shutdown_at, across the end of daylight saving time\n");
    g_fake_now = at_local(2026, 10, 31, 23, 0, 0);
    t = clock_next(23 * 60 + 30, 0, g_fake_now);
    CHECK(t == at_local(2026, 10, 31, 23, 30, 0));
    to_local(t, &lt);
    t2 = clock_next(23 * 60 + 30, local_day(&lt), g_fake_now);
    CHECK(t2 == at_local(2026, 11, 1, 23, 30, 0));
    wprintf(L"  %.2f hours between the two\n", (double)(t2 - t) / 36e9);
    g_fake_now = at_local(2026, 10, 31, 23, 45, 0);
    CHECK(clock_next(23 * 60 + 30, 0, g_fake_now) ==
          at_local(2026, 11, 1, 23, 30, 0));
}

static void test_schedule(void)
{
    WCHAR at[96], left[48];

    g_started = TRUE;

    reset(L"one-off in 15 min: countdown a minute before, then shut down",
          at_local(2026, 11, 2, 20, 0, 0));
    shutdown_in(15);
    CHECK(g_once_due == g_fake_now + 15 * MINUTE_100NS);
    advance(14 * 60 - 2);
    CHECK(g_deadline == 0);
    advance(4);
    CHECK(g_deadline == g_once_due);
    advance(50);
    CHECK(g_exits == 0);
    advance(12);
    CHECK(g_exits == 1 && g_once_due == 0 && g_deadline == 0);
    advance(120);
    CHECK(g_exits == 1 && g_timer == 0);

    reset(L"slept through it: skipped with a balloon",
          at_local(2026, 11, 2, 20, 0, 0));
    shutdown_in(15);
    advance(60);
    g_fake_now += 60 * MINUTE_100NS;
    sd_check();
    CHECK(g_exits == 0 && g_once_due == 0 &&
          wcscmp(g_last_title, L"Shutdown skipped") == 0);

    reset(L"woke 2 min late: a full countdown first",
          at_local(2026, 11, 2, 20, 0, 0));
    shutdown_in(15);
    advance(60);
    g_fake_now += 16 * MINUTE_100NS;
    sd_check();
    CHECK(g_exits == 0 && g_deadline >= g_fake_now + 59 * SEC_100NS);
    advance(58);
    CHECK(g_exits == 0);
    advance(3);
    CHECK(g_exits == 1);

    reset(L"slept 2 min with the countdown at 0:30: shown in full again",
          at_local(2026, 11, 2, 20, 0, 0));
    shutdown_in(1);
    advance(30);
    CHECK(g_deadline != 0 && g_exits == 0);
    g_fake_now += 2 * MINUTE_100NS;
    sd_check();
    CHECK(g_exits == 0 && g_deadline >= g_fake_now + 59 * SEC_100NS);
    advance(61);
    CHECK(g_exits == 1);

    reset(L"shutdown_warning = 0: no countdown",
          at_local(2026, 11, 2, 20, 0, 0));
    g_cfg.shutdown_warning = 0;
    shutdown_in(1);
    advance(59);
    CHECK(g_exits == 0 && g_deadline == 0);
    advance(2);
    CHECK(g_exits == 1);

    reset(L"shutdown_at 23:30 at 23:00: cancelling skips that day only",
          at_local(2026, 11, 2, 23, 0, 0));
    set_daily(23, 30);
    CHECK(g_daily_due == at_local(2026, 11, 2, 23, 30, 0));
    shutdown_cancel();
    CHECK(g_daily_due == at_local(2026, 11, 3, 23, 30, 0));
    advance(40 * 60);
    CHECK(g_exits == 0);
    shutdown_time_changed();
    CHECK(g_daily_due == at_local(2026, 11, 3, 23, 30, 0));

    reset(L"shutdown_at 23:30 shuts down, and the next day is armed",
          at_local(2026, 11, 2, 23, 25, 0));
    set_daily(23, 30);
    advance(6 * 60);
    CHECK(g_exits == 1 && g_daily_due == at_local(2026, 11, 3, 23, 30, 0));

    reset(L"a one-off after that day's shutdown_at replaces it",
          at_local(2026, 11, 2, 23, 0, 0));
    set_daily(23, 30);
    shutdown_in(120);
    CHECK(g_once_due == at_local(2026, 11, 3, 1, 0, 0) &&
          g_daily_due == at_local(2026, 11, 3, 23, 30, 0));
    advance(35 * 60);
    CHECK(g_exits == 0);
    shutdown_cancel();
    CHECK(g_once_due == 0 &&
          g_daily_due == at_local(2026, 11, 3, 23, 30, 0));

    reset(L"a one-off before that day's shutdown_at leaves it",
          at_local(2026, 11, 2, 23, 0, 0));
    set_daily(23, 30);
    shutdown_in(15);
    CHECK(g_daily_due == at_local(2026, 11, 2, 23, 30, 0));
    shutdown_cancel();
    CHECK(g_once_due == 0 &&
          g_daily_due == at_local(2026, 11, 2, 23, 30, 0));

    reset(L"10 more minutes during shutdown_at's countdown",
          at_local(2026, 11, 2, 23, 29, 30));
    set_daily(23, 30);
    CHECK(g_deadline != 0);
    shutdown_in(10);
    CHECK(g_deadline == 0 &&
          g_daily_due == at_local(2026, 11, 3, 23, 30, 0));
    advance(9 * 60);
    CHECK(g_exits == 0 && g_deadline != 0);
    advance(70);
    CHECK(g_exits == 1 && g_once_due == 0);

    reset(L"a one-off at the same time as shutdown_at: one shutdown",
          at_local(2026, 11, 2, 23, 0, 0));
    set_daily(23, 30);
    schedule_once(clock_next(23 * 60 + 30, 0, g_fake_now));
    advance(31 * 60);
    CHECK(g_exits == 1 && g_once_due == 0 &&
          g_daily_due == at_local(2026, 11, 3, 23, 30, 0));

    reset(L"cancelling a one-off typed for shutdown_at's time cancels both",
          at_local(2026, 11, 2, 23, 0, 0));
    set_daily(23, 30);
    schedule_once(clock_next(23 * 60 + 30, 0, g_fake_now));
    advance(29 * 60 + 10);
    CHECK(g_deadline != 0);
    shutdown_cancel();
    CHECK(g_deadline == 0 && g_once_due == 0 &&
          g_daily_due == at_local(2026, 11, 3, 23, 30, 0));
    advance(3 * 60);
    CHECK(g_exits == 0 && g_deadline == 0);

    reset(L"a one-off days away replaces only the next shutdown_at day",
          at_local(2026, 11, 2, 23, 0, 0));
    set_daily(23, 30);
    shutdown_in(72 * 60);
    CHECK(g_daily_due == at_local(2026, 11, 3, 23, 30, 0));
    CHECK(wcscmp(g_last_title, L"Shutdown scheduled") == 0);

    reset(L"cancelling that one-off gives the day back",
          at_local(2026, 11, 2, 23, 0, 0));
    set_daily(23, 30);
    shutdown_in(120);
    CHECK(g_daily_due == at_local(2026, 11, 3, 23, 30, 0));
    shutdown_cancel();
    CHECK(g_once_due == 0 &&
          g_daily_due == at_local(2026, 11, 2, 23, 30, 0));

    reset(L"...but not when it is too close to count down in full",
          at_local(2026, 11, 2, 23, 0, 0));
    set_daily(23, 30);
    shutdown_in(120);
    advance(29 * 60 + 20);
    shutdown_cancel();
    CHECK(g_daily_due == at_local(2026, 11, 3, 23, 30, 0));
    advance(2 * 60);
    CHECK(g_exits == 0 && g_deadline == 0);

    reset(L"a second one-off before shutdown_at gives the day back",
          at_local(2026, 11, 2, 23, 0, 0));
    set_daily(23, 30);
    shutdown_in(120);
    shutdown_in(15);
    CHECK(g_once_due == at_local(2026, 11, 2, 23, 15, 0) &&
          g_daily_due == at_local(2026, 11, 2, 23, 30, 0));

    reset(L"a clock resync on waking does not hide the sleep",
          at_local(2026, 11, 2, 20, 0, 0));
    shutdown_in(1);
    advance(30);
    CHECK(g_deadline != 0);
    g_fake_now += 2 * MINUTE_100NS;
    shutdown_time_changed();
    CHECK(g_exits == 0 && g_deadline >= g_fake_now + 59 * SEC_100NS);

    reset(L"...nor a shutdown_at that passed while asleep",
          at_local(2026, 11, 2, 23, 0, 0));
    set_daily(23, 30);
    advance(10);
    g_fake_now = at_local(2026, 11, 2, 23, 32, 0);
    shutdown_time_changed();
    CHECK(g_exits == 0 && g_deadline >= g_fake_now + 59 * SEC_100NS);
    advance(61);
    CHECK(g_exits == 1);

    reset(L"...and one slept through still gets its balloon",
          at_local(2026, 11, 2, 23, 0, 0));
    set_daily(23, 30);
    advance(10);
    g_fake_now = at_local(2026, 11, 3, 8, 0, 0);
    shutdown_time_changed();
    CHECK(g_exits == 0 && wcscmp(g_last_title, L"Shutdown skipped") == 0 &&
          g_daily_due == at_local(2026, 11, 3, 23, 30, 0));

    reset(L"the clock set back during the countdown closes it",
          at_local(2026, 11, 2, 20, 0, 0));
    shutdown_in(1);
    advance(20);
    CHECK(g_deadline != 0);
    g_fake_now -= 60 * MINUTE_100NS;
    shutdown_time_changed();
    CHECK(g_deadline == 0 && g_exits == 0);
    advance(59 * 60 + 30);
    CHECK(g_deadline == 0);
    advance(20);
    CHECK(g_deadline != 0 && g_exits == 0);
    advance(55);
    CHECK(g_exits == 1);

    reset(L"what the tray menu shows", at_local(2026, 11, 2, 23, 0, 0));
    shutdown_in(65);
    CHECK(shutdown_next(at, 96, left, 48));
    CHECK(wcscmp(left, L"in 1 h 5 min") == 0);
    wprintf(L"  \"Shutdown at %ls\", \"(%ls)\"\n", at, left);
    shutdown_cancel();
    CHECK(!shutdown_next(at, 96, NULL, 0));
}

int main(int argc, char **argv)
{
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    config_set_defaults(&g_cfg);
    g_hinst = GetModuleHandleW(NULL);

    test_parsing();
    test_clock();
    test_schedule();

    wprintf(L"\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails != 0;
}
