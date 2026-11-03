// common.h - shared contract between modules. User-facing behaviour: README.md.
//
//   main.c   entry point, single instance, DPI, main window, message routing,
//            config file watching, logging/error helpers
//   config.c settings file (vim.ini next to the exe, or in %APPDATA% when that
//            folder is read-only): defaults, parsing, key names, text shortcut
//            table
//   input.c  low-level keyboard hook (the single keyboard dispatcher), mouse
//            mode, movement/scroll thread, mouse actions, Alt scrolling, text
//            shortcut capture and typing, mode indicator, sleep/lock handling
//   hint.c   click-hint overlay (main/UI thread)
//   uia.c    UI Automation workers: clickable-element scan, Alt scroll
//            targets, text-field focus for bare keys
//   tray.c   tray icon + menu, help window, start-with-Windows
//   snippet.c text shortcut editor window (tray "Text shortcuts...")
//   update.c self-update from the latest GitHub release (tray "Update to
//            latest version")
//   shutdown.c scheduled shutdown (tray "Schedule shutdown", shutdown_at):
//            the schedule, its countdown window, turning the computer off

#ifndef VIM_COMMON_H
#define VIM_COMMON_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#define APP_NAME        L"vim mouse"
#define MAIN_WND_CLASS  L"VimMouseMainWindow"
#define INSTANCE_MUTEX  L"Local\\vim_mouse_single_instance"
#define APP_ICON_ID     1   // vim.rc: the LoadImageW resource, not the tray icon

// Settings paths can be longer than MAX_PATH (config.c keeps a 32768 buffer).
#define APP_PATH_MAX    32768

// ---------------------------------------------------------------------------
// Messages posted to the main window (g_main_wnd). All are handled on the
// main/UI thread by main.c, which forwards to the owning module.
#define WM_APP_HINT_OPEN        (WM_APP + 1)  // open hint overlay (no-op if open)
#define WM_APP_HINT_CLOSE       (WM_APP + 2)  // close hint overlay (no-op if closed)
#define WM_APP_HINT_KEY         (WM_APP + 3)  // wParam = vk, lParam = KMOD_* bits; -> hint_on_key
#define WM_APP_SHOW_HELP        (WM_APP + 4)  // toggle help window; -> help_toggle
#define WM_APP_STATE_CHANGED    (WM_APP + 5)  // mouse mode / paused / drag changed; -> tray_update
#define WM_APP_QUIT             (WM_APP + 6)  // quit the program
#define WM_APP_TRAY             (WM_APP + 7)  // tray icon callback; -> tray_on_message
#define WM_APP_ALREADY_RUNNING  (WM_APP + 8)  // a second instance was started
#define WM_APP_RELOAD_CONFIG    (WM_APP + 9)  // settings file changed on disk / reload requested
#define WM_APP_HINT_AGAIN       (WM_APP + 10) // hint hotkey pressed while hints are open; -> hint_hotkey_again
#define WM_APP_ELEMENTS         (WM_APP + 11) // lParam = ClickList* from uia.c; -> hint_on_elements (takes ownership)
#define WM_APP_TYPE_TEXT        (WM_APP + 12) // lParam = malloc'd WCHAR* from input.c; -> snippet_insert, then free
#define WM_APP_HINT_PREPARE     (WM_APP + 13) // wParam = HINT_PREPARE_*: the hint hotkey's modifiers went down
                                              // (start scanning early), the hotkey will not follow, or keys
                                              // reached the app (what was scanned ahead is old); -> hint_prepare
#define WM_APP_UPDATE_DONE      (WM_APP + 14) // wParam = result from the update worker; -> update_on_done
#define WM_APP_PAGE_ACTED       (WM_APP + 15) // wParam = PAGE_* kind: this program scrolled, clicked or went
                                              // back / forward (once per burst); -> hint_page_acted
#define WM_APP_FLUSH_DONE       (WM_APP + 16) // wParam = the hit test reached the page, lParam = seq of the
                                              // uia_flush request; -> hint_flush_done

// ---------------------------------------------------------------------------
// Keys

#define KMOD_SHIFT 0x1
#define KMOD_CTRL  0x2
#define KMOD_ALT   0x4
#define KMOD_WIN   0x8

typedef struct {
    UINT vk;    // virtual-key code; 0 = unbound
    UINT mods;  // KMOD_* bits that must be held (exact match)
} Hotkey;

// ---------------------------------------------------------------------------
// Settings (config.c). g_cfg is written only by the main thread (load/reload);
// other threads only read it.

enum { INDICATOR_CURSOR = 0, INDICATOR_CORNER = 1, INDICATOR_OFF = 2 };
enum { HINT_MODE_ELEMENTS = 0, HINT_MODE_GRID = 1 };
enum { SCAN_MONITOR = 0, SCAN_WINDOW = 1 };

typedef struct {
    // [general]
    Hotkey hint_hotkey;        // default Alt+F
    Hotkey quit_hotkey;        // default Ctrl+Alt+Q
    Hotkey pause_hotkey;       // default Alt+P; twice within pause_tap_ms pauses / resumes
                               // everything (every key passes through while paused, except
                               // this hotkey and quit_hotkey)
    int    pause_tap_ms;       // two presses of pause_hotkey within this many ms (0-1000,
                               // default 300) pause or resume; 0 = one press does
    Hotkey lock_hotkey;        // default Ctrl+Alt+L; locks / unlocks the keyboard
    UINT   mouse_mode_key;     // default VK_CAPITAL; tap toggles, hold = momentary
    int    capslock_off;       // 1 = the CapsLock state is kept off while the program runs
                               // (not while paused): turned off at start and whenever it
                               // comes on (Shift+CapsLock, an admin window, the lock
                               // screen); default 1. 0 = Shift+CapsLock is the real CapsLock.
    int    indicator;          // INDICATOR_*; default INDICATOR_CURSOR
    int    hide_pointer;       // outside mouse mode the pointer is hidden (every system
                               // cursor is swapped for a blank one, session-wide, and put
                               // back on exit, pause, lock and sleep); the real mouse shows
                               // it, and it hides again this many ms after the mouse last
                               // moved or clicked (0-60000). Default 2000; 0 = never hidden.
    int    log;                // 1 = also write log lines to vim.log; 2 = also what each
                               // hint open's scan made of every element of the active
                               // window (and no early scan); default 0
    int    shortcut_paste;     // text shortcuts: 1 = paste via the clipboard (default), 0 = type keys
    int    shutdown_at;        // turn the computer off every day at this local time, in
                               // minutes after midnight (vim.ini "23:30"); -1 = none (default).
                               // The tray menu can cancel one day's.
    int    shutdown_warning;   // seconds the countdown window shows before a scheduled
                               // shutdown (0-300, default 60; 0 = none)
    int    shutdown_force;     // 1 = a scheduled shutdown closes apps even with unsaved work;
                               // default 0 (only hung apps; others may hold it up)

    // [mouse]  speeds in physical px per second; scroll in wheel-delta units per second
    double speed;              // default 600
    double max_speed;          // default 2400
    double accel_time;         // seconds to reach max_speed, default 0.45
    double accel_curve;        // easing exponent, default 1.8
    double slow_speed;         // while Shift is held (no acceleration), default 120
    double scroll_speed;       // default 1200  (10 notches/s)
    double scroll_max_speed;   // default 4800  (40 notches/s)
    int    smooth_scroll;      // 1 = small steps once per display frame; default 1
    int    alt_scroll;         // 1 = Alt + move keys scroll anywhere, Alt + u/d fast; default 1
    int    alt_scroll_auto;    // 1 = Alt scrolling goes to the nearest scrollable area in
                               // line with the pointer; default 1
    int    alt_nav;            // 1 = Alt + key_left / key_right go back / forward (Alt+Left /
                               // Alt+Right) and Alt + the side scroll keys scroll sideways;
                               // default 1
    int    bare_keys;          // 1 = outside text fields the Alt keys (hint hotkey, Alt
                               // scrolling, back / forward) work without Alt; default 1
    int    fast_scroll_lines;  // notches per Alt + u/d tap, default 5 (1-50)
    int    scroll_end_tap_ms;  // Alt + g g within this many ms goes to the top (Alt +
                               // Shift + g to the bottom), default 500 (0 = both off)
    int    center_tap_ms;      // key_center twice within this many ms puts the pointer in
                               // the middle of the active window's screen, default 300
                               // (0 = off)
    double fast_scroll_speed;  // Alt + u/d held, default 3600
    double fast_scroll_max_speed; // default 14400
    UINT key_left, key_down, key_up, key_right;          // h j k l
    UINT key_left_button;      // a  (hold = drag, tap = click, tap tap = double click)
    UINT key_right_click;      // s
    UINT key_middle_click;     // m
    UINT key_drag_toggle;      // v  (press = hold left button, press again = release)
    UINT key_scroll_up;        // u
    UINT key_scroll_down;      // d
    UINT key_scroll_left;      // ,
    UINT key_scroll_right;     // .
    UINT key_scroll_end;       // g  (g g = top, Shift + g = bottom)
    UINT key_center;           // 0  (tap twice = pointer to the middle of the screen;
                               // the key still reaches the app; in mouse mode, and outside
                               // it where the focus is not in a text field)
    UINT key_hints;            // f  (open click hints)
    UINT key_exit;             // i  (leave mouse mode; Esc always works too)
    UINT key_help;             // /  (also shown as ?)

    // [hints]
    WCHAR alphabet[64];        // label chars, default L"abcdefhjklmnopqrstuvwxz"
    int   cell_size;           // target cell size in px at 100% scaling, default 64
    int   subgrid;             // 1 = refine inside the chosen cell, default 1
    WCHAR subgrid_keys[64];    // rows separated by '/', default L"qwer/asdf/zxcv"
    int   font_size;           // label font px at 100% scaling; 0 = auto (default)
    BYTE  opacity;             // 0-255 for labels and status bar, default 235
    COLORREF label_bg;         // default #FFD633
    COLORREF label_fg;         // default #141414
    COLORREF label_border;     // default #7A5C00
    COLORREF match_fg;         // already-typed chars, default #9C7A10
    int   hint_mode;           // HINT_MODE_*: what the hotkey opens first, default elements
    UINT  key_grid;            // ;  while the hints are open, switches between the element
                               // labels and the grid (0 = off); the hotkey again reloads
                               // what it opened
    int   scan_scope;          // SCAN_MONITOR: every visible window on the monitor (default)
                               // SCAN_WINDOW: only the foreground window and the taskbar
    int   scan_timeout;        // how long to wait for every window's elements before
                               // showing the ones found so far (100-5000 ms, default 400).
                               // The active window's are always waited for, up to 3 s;
                               // with nothing found by then, the hints close.
    int   browser_first;       // 1 = with a browser active (Chrome, Edge, Firefox, Electron),
                               // show its labels as soon as it answered; the other windows'
                               // labels follow as they answer (needs scan_refresh). Default 1;
                               // 0 = wait for every window up to scan_timeout as elsewhere.
    int   scan_refresh;        // while the hints are open, rescan this often so new or moved
                               // elements get labels (0-5000 ms, default 500; 0 = off).
                               // Existing labels never change meaning.
    int   keep_labels;         // 1 = a refresh never moves, retires or lengthens a label
                               // already on screen; new elements get labels in free space,
                               // as many as fit. The hotkey again relabels everything.
                               // Default 1; 0 = labels follow their elements and disappear
                               // with them.
    int   click_return;        // 1 = after a click through the hints the pointer goes back to
                               // where it was, unless mouse mode is on (Alt + label and a
                               // drag's drop still move it there); default 1. 0 = the pointer
                               // stays on the element it clicked.
    int   prescan;             // 1 = scan (hints, Alt scroll targets) once Alt is held, default 1
    int   scan_ahead;          // 1 = scan for the hints as soon as the screen changed (after our
                               // scroll, click or back / forward once the page caught up, after a
                               // scroll the browser reported, after another window came to the
                               // front), so the hotkey finds its labels ready; keys that reach the
                               // app drop that scan. Default 1; 0 = scan on the hotkey only.
    int   scan_split;          // 1 = search the parts of a classic window (its title bar,
                               // list, tree, panes) at the same time, and skip the title
                               // bar's system menu; default 1. 0 = one search per window as
                               // before, which also labels the window icon's system menu.
    int   scan_settle;         // after this program scrolled or clicked, a scan of a Chromium
                               // window (Chrome, Edge, Electron, WebView2) waits at most this
                               // long from then for the page to catch up: Chromium tells UI
                               // Automation about moved and new elements up to 500 ms late,
                               // so an earlier scan labels old positions (0-2000 ms, default
                               // 650; 0 = off). The wait starts with the action itself, not
                               // with the hotkey (hint_page_acted), so a hotkey pressed after
                               // it ended scans at once. See scan_flush.
    int   scan_flush;          // 1 = during that wait, ask the browser to send its changes now
                               // and end it as soon as they are in (about 330 ms after a
                               // scroll instead of 650; back / forward still wait it out), and
                               // wait like that after scrolls the browser reports itself
                               // (mouse wheel, touchpad, keys) too, up to 300 ms; default 1.
                               // 0 = wait out scan_settle after our own actions only.
    int   scan_early;          // after a scroll, start the scan on the first flush that finds
                               // the page in and let the next flush only confirm it, instead
                               // of scanning after that confirmation: 1 = when Windows'
                               // animation effects are off (Chrome then scrolls at once and
                               // the first flush has the page where it stopped), default;
                               // 2 = always; 0 = never.
    int   scan_focusable;      // 1 = in a browser window, elements that UI Automation calls
                               // keyboard-focusable but that have no clickable control type
                               // or pattern (a div with tabindex whose click handler sits on
                               // an ancestor, a range input) get labels too, unless they are
                               // text, scroll, cover over a quarter of the page or lie
                               // outside it; default 1. 0 = only control types and patterns.
    int   scan_hidden_checkbox; // 1 = in a browser window, a checkbox or radio button a page
                               // hides behind its own label (a 1 px input, the label drawn as
                               // the box) gets the label's box and is clicked there; default 1.
                               // 0 = such inputs are too small for a label.
    int   labels_apart;        // 1 = a label whose box would cover another label's, or sit
                               // under it, is drawn to the right of that one (below it at
                               // the screen's edge); the smaller element keeps its corner.
                               // Default 1; 0 = every label at its element's top-left corner.
} Config;

extern Config g_cfg;

void         config_set_defaults(Config *c);
// Loads the settings file into *c (defaults first, then file values).
// If the file does not exist it is created with commented defaults and
// *created is set to TRUE. Returns FALSE only if the file exists but could not
// be read. Problems (unknown keys, bad values) are collected in
// config_warnings(); bad values keep their defaults. A successful load also
// replaces the text shortcut table below.
BOOL         config_load(Config *c, BOOL *created);
const WCHAR *config_path(void);       // full path of the settings file in use (see config.c)
const WCHAR *config_warnings(void);   // L"" if none; one problem per line, from last load
BOOL         parse_key(const WCHAR *s, UINT *vk);        // "h", ",", "CapsLock", "F13", "Comma"...
BOOL         parse_hotkey(const WCHAR *s, Hotkey *out);  // "Alt+F", "Ctrl+Alt+Q", "none"
void         format_key(UINT vk, WCHAR *buf, int cch);   // vk -> display name ("h", "CapsLock", ",")
void         format_hotkey(const Hotkey *hk, WCHAR *buf, int cch);  // "Alt+F"; "none" if unbound
BOOL         parse_clock(const WCHAR *s, int *minute);   // "23:30", "7:05" -> minutes after midnight

// Text shortcuts: the [shortcuts] section of vim.ini, "abbreviation = text".
// Holding Alt and typing the abbreviation types the text when Alt is released.
// Abbreviations are 1-SHORTCUT_ABBR_MAX letters/digits, stored lower case.
// The table is guarded by a lock: every function is safe from any thread.
#define SHORTCUT_ABBR_MAX 31
#define SHORTCUT_TEXT_MAX 4096
#define SHORTCUT_MAX      200
#define SC_PREFIX 0x1   // shortcut_lookup: some abbreviation starts with it
#define SC_EXACT  0x2   // shortcut_lookup: an abbreviation is exactly it
UINT         shortcut_lookup(const WCHAR *abbr, int len);   // SC_* bits, 0 = none
WCHAR       *shortcut_text_dup(const WCHAR *abbr, int len); // malloc'd copy or NULL
int          shortcut_count(void);
// Copies entry i; *text is malloc'd (caller frees). FALSE if out of range.
BOOL         shortcut_get(int i, WCHAR *abbr, int abbr_cch, WCHAR **text);
BOOL         shortcut_abbr_valid(const WCHAR *s);
// Rewrites only the [shortcuts] section of vim.ini with these entries (text
// with real newlines/tabs; it is escaped on write). FALSE if writing failed.
BOOL         shortcuts_save(const WCHAR *const *abbrs, const WCHAR *const *texts,
                            int count);
// Export writes the current shortcuts to a file in the [shortcuts] format.
// Import reads "abbreviation = text" lines (a plain list, or the [shortcuts]
// section of a vim.ini). It returns a malloc'd array (free it with
// shortcut_entries_free) or NULL if the file could not be read; *skipped
// counts lines that are not valid shortcuts.
typedef struct {
    WCHAR  abbr[SHORTCUT_ABBR_MAX + 1];
    WCHAR *text;
} ShortcutEntry;
BOOL           shortcuts_export(const WCHAR *path);
ShortcutEntry *shortcuts_read_file(const WCHAR *path, int *count, int *skipped);
void           shortcut_entries_free(ShortcutEntry *entries, int count);

// ---------------------------------------------------------------------------
// Input (input.c). Runs its own thread (hook + message loop) and a movement
// thread. All functions are safe to call from any thread.

BOOL input_start(void);                 // FALSE if the keyboard hook could not be installed
void input_stop(void);
void input_set_paused(BOOL paused);     // paused: every key passes through except quit/pause hotkeys
BOOL input_is_paused(void);
// Keyboard lock: every key is swallowed except the lock hotkey, which unlocks.
void input_set_locked(BOOL locked);
BOOL input_is_locked(void);
void input_set_mouse_mode(BOOL on);
BOOL input_mouse_mode_active(void);
BOOL input_left_button_held(void);      // a drag is in progress (via hold-a or v)
// Key capture for the hint overlay: while on, every non-modifier key is
// swallowed and posted to the main window as WM_APP_HINT_KEY. The hook turns
// capture on by itself at the moment it posts WM_APP_HINT_OPEN (so fast typing
// after the hotkey is not lost); hint.c turns it on when opened another way
// and off when it closes.
void input_set_key_capture(BOOL on);
void input_config_changed(void);        // g_cfg was reloaded
// Types text into the focused window (KEYEVENTF_UNICODE; \n = Enter, \t = Tab).
// Text shortcuts use it only with shortcut_method = type (see snippet_insert).
void input_type_text(const WCHAR *text);
// Milliseconds since this program last scrolled, clicked or went back or
// forward (INFINITE if it never did); kind gets PAGE_SCROLL, PAGE_CLICK or
// PAGE_NAV (back / forward) and pt where it happened, physical screen px
// (the cursor for PAGE_NAV; either may be NULL). The hints wait for browsers
// to catch up; each action also posts WM_APP_PAGE_ACTED (once per burst) so
// that wait starts right away.
enum { PAGE_SCROLL = 1, PAGE_CLICK = 2, PAGE_NAV = 3 };
DWORD input_since_page_action(int *kind, POINT *pt);

enum { BTN_LEFT = 0, BTN_RIGHT = 1, BTN_MIDDLE = 2 };
void mouse_move_to(POINT pt);                          // absolute, physical px
void mouse_click_at(POINT pt, int button, int count);  // count 1 = click, 2 = double click

// ---------------------------------------------------------------------------
// Hint overlay (hint.c). All functions run on the main/UI thread.

struct ClickList;

void hint_init(HINSTANCE inst);
void hint_shutdown(void);
void hint_open(void);                   // on the monitor under the cursor
// WM_APP_HINT_PREPARE: ALT = the hotkey's modifiers went down, scan before
// it completes; CANCEL = it will not follow; FORGET = keys reached the app.
enum { HINT_PREPARE_CANCEL = 0, HINT_PREPARE_ALT = 1, HINT_PREPARE_FORGET = 2 };
void hint_prepare(int what);
void hint_close(void);
BOOL hint_is_open(void);
void hint_on_key(UINT vk, UINT mods);
void hint_on_elements(struct ClickList *list);  // WM_APP_ELEMENTS; always frees list
void hint_hotkey_again(void);           // reload: rescan the elements, or reset the grid
void hint_page_acted(void);             // WM_APP_PAGE_ACTED: start waiting for the page now
void hint_flush_done(BOOL reached, UINT seq);  // WM_APP_FLUSH_DONE: a flush came back

// ---------------------------------------------------------------------------
// Clickable elements (uia.c). UI Automation runs on its own MTA worker thread;
// never on the UI thread (querying our own windows from there can deadlock).

typedef struct {
    RECT  rc;       // element bounds, physical screen px, clipped to the monitor
    POINT click;    // visible point inside rc to click (physical screen px)
} ClickTarget;

typedef struct ClickList {
    UINT scan_id;   // the id passed to uia_scan
    BOOL done;      // FALSE = partial result, more windows still being scanned
    BOOL fg_done;   // the foreground window is in (or is not scanned); TRUE when done
    int  pending;   // windows still being scanned (0 when done)
    int  count;
    ClickTarget items[];   // cumulative: every post contains everything found so far
} ClickList;

BOOL uia_start(void);       // FALSE if UI Automation is unavailable (hints do not open)
void uia_stop(void);
BOOL uia_available(void);
// Asynchronous scan of the monitor (g_cfg.scan_scope decides which windows).
// Posts WM_APP_ELEMENTS with a malloc'd ClickList to g_main_wnd after each
// window that added targets and after the foreground window (done = FALSE),
// and once at the end (done = TRUE).
// A newer uia_scan or uia_cancel makes the old scan stop posting.
// detail (log = 2): once the scan is posted, writes the windows on the monitor
// and every element of the active window with what became of it to vim.log.
void uia_scan(HMONITOR mon, UINT scan_id, BOOL detail);
void uia_cancel(void);
// Forgets the windows skipped for answering slowly, so the next scan asks
// them again (another hint hotkey press).
void uia_forget_slow(void);
// Alt scrolling: where the wheel of a press at pt should go, from the cache
// of scrollable areas of the window under pt (UI Automation, on its own
// thread). Never waits, so the stream starts under the pointer at once and
// asks again every tick: SCROLL_AIM_ASKING means a search is running;
// SCROLL_AIM_TARGET sets *target, a point of the area nearest to pt that
// scrolls that way (pt is not over one); SCROLL_AIM_POINTER leaves the wheel
// at the pointer. since_ms is how long ago the press was: a slightly old list
// that would redirect gets SCROLL_STALE_WAIT_MS for its refresh, then
// answers with what it has. One caller thread at a time (the movement
// thread). uia_stop stops it too.
enum { SCROLL_AIM_POINTER = 0, SCROLL_AIM_TARGET = 1, SCROLL_AIM_ASKING = -1 };
int  uia_scroll_aim(POINT pt, BOOL vertical, DWORD since_ms, POINT *target);
void uia_scroll_prefetch(POINT pt);     // start caching that window, never waits
void uia_scroll_stop(void);
// Shift + g (bottom) through UI Automation: scrolls the area at pt to the
// bottom through its ScrollPattern on its own thread, so a held Shift does
// not matter. Waits up to timeout_ms. TRUE when it scrolled, or when every
// vertically scrollable area there was already at the bottom; FALSE when UI
// Automation could not scroll there, and the caller falls back to the wheel
// once Shift is released. One caller thread at a time (the movement thread).
// uia_scroll_stop (and so uia_stop) stops it too.
BOOL uia_scroll_end(POINT pt, DWORD timeout_ms);
// Bare keys: whether the keyboard focus is in a text field (UI Automation, on
// its own thread). uia_focus_changed forgets the answer and asks again, not
// before delay_ms from now (a click the app has not handled yet); the first
// call starts the worker. uia_focus_state returns the cached answer for the
// current focus, or FOCUS_UNKNOWN while it is being asked. Neither waits, so
// both are safe from the keyboard hook. uia_focus_wait is the same as
// uia_focus_state but blocks up to max_ms while the answer is being asked (and
// the ask is recent), for the hook to keep a key in order. uia_stop stops it
// too.
enum { FOCUS_UNKNOWN = 0, FOCUS_TEXT = 1, FOCUS_OTHER = 2 };
void uia_focus_changed(DWORD delay_ms);
BOOL uia_chromium_panel(HWND hwnd);      // hwnd is one of Chrome's owned panels, not a menu
// hwnd's top-level window shows Chromium content (Chrome, Edge, Electron,
// WebView2, CEF), which reports moved and new elements late. Window functions
// only (no UI Automation), so the UI thread may call it.
BOOL uia_chromium_window(HWND hwnd);
// hwnd's top-level window is a web browser or shows web content: Chromium as
// above, or Firefox. Window functions only.
BOOL uia_browser_window(HWND hwnd);
// Picks a point of root (a Chromium top-level window) where a UI Automation
// hit test reaches its web page: prefer, if it is on top and over a page
// area (Chrome_RenderWidgetHostHWND), then the centre of its page area.
// Window functions only. FALSE when no such point of root is on top.
BOOL uia_page_point(HWND root, const POINT *prefer, POINT *out);
// Makes the Chromium window under pt send what it holds back from UI
// Automation now: a UI Automation hit test there has its page send every
// pending tree, position and scroll change at once (Blink serializes
// immediately for an action), instead of up to 500 ms later. Asynchronous, on
// a thread of its own; returns at once and posts WM_APP_FLUSH_DONE (wParam =
// the hit test reached the page, lParam = seq) when the hit test is back. The
// page's scroll updates arrive as EVENT_SYSTEM_SCROLLINGEND from Chromium
// (hint.c listens), ahead of that message.
void uia_flush(POINT pt, UINT seq);
int  uia_focus_state(void);
int  uia_focus_wait(DWORD max_ms);
// Posts msg (wParam, lParam 0) to thread tid whenever an answer arrives, so a
// key that typed while the focus was being asked for can still act on the
// answer without the hook waiting for it; tid 0 turns it off. Never waits.
void uia_focus_notify(DWORD tid, UINT msg);
void uia_focus_stop(void);

// ---------------------------------------------------------------------------
// Tray + help (tray.c). Main/UI thread.

void tray_init(HWND main_wnd);
void tray_shutdown(void);
void tray_update(void);                 // refresh icon, tooltip (mouse mode / paused)
void tray_on_message(WPARAM wp, LPARAM lp);    // WM_APP_TRAY
BOOL tray_on_command(WORD id);          // WM_COMMAND from the tray menu; TRUE if handled
void tray_on_taskbar_created(void);     // Explorer restarted: re-add icon
void tray_balloon(const WCHAR *title, const WCHAR *text);
void help_toggle(void);

// ---------------------------------------------------------------------------
// Text shortcut editor (snippet.c). Main/UI thread.

void snippet_editor_open(void);         // opens or brings the editor to front
BOOL snippet_editor_dialog_message(MSG *msg);  // IsDialogMessage for its window
void snippet_editor_close(void);
// Rebuilds the list after the shortcut table changed on disk, keeping the
// selection and the text in the edit fields; no-op if the editor is closed.
void snippet_editor_refresh(void);
// Inserts a text shortcut's text into the focused window: pastes it through
// the clipboard (restored afterwards), or types it (shortcut_method = type).
void snippet_insert(const WCHAR *text);

// ---------------------------------------------------------------------------
// Self-update (update.c). Main/UI thread; the download runs on a worker.

// Downloads the latest release's vim.exe (no-op with a balloon while one is
// running). The worker posts WM_APP_UPDATE_DONE; update_on_done then says it
// is up to date, reports the error, or swaps the exe in and restarts: the
// running exe is renamed to "<exe>.old", the download takes its name, and the
// new process starts with --updated, then this one quits.
void update_start(void);
void update_on_done(WPARAM result);     // WM_APP_UPDATE_DONE
BOOL update_running(void);              // a download is in progress
BOOL update_restarted(void);            // this process was started with --updated
// Startup, when the instance mutex already exists: with --updated, waits up to
// 30 s for the old instance to exit and returns TRUE once this process owns
// the mutex. FALSE (at once without --updated): it is a second instance.
BOOL update_wait_for_old(HANDLE mutex);
// Startup: deletes "<exe>.old" (retrying briefly after an update) and any
// partial "<exe>.new" left by quitting during a download.
void update_cleanup(void);

// ---------------------------------------------------------------------------
// Scheduled shutdown (shutdown.c). Main/UI thread.
//
// The next shutdown is the earlier of a one-off from the tray menu (kept in
// memory only) and the next shutdown_at day. shutdown_warning seconds before
// it a countdown window opens (Cancel, 10 more minutes, Shut down now); then
// ExitWindowsEx powers off. One the computer slept through by more than
// 5 minutes is skipped with a balloon.

void shutdown_init(void);               // after the settings and the tray: arms shutdown_at
void shutdown_stop(void);               // quit: closes its windows, stops its timer
void shutdown_config_changed(void);     // g_cfg was reloaded (shutdown_at may have changed)
void shutdown_time_changed(void);       // WM_TIMECHANGE: shutdown_at follows the local clock
void shutdown_in(int minutes);          // one-off this many minutes from now (replaces one)
void shutdown_dialog_open(void);        // tray "At a time or after...": type 23:30 or 90
// Cancels the next shutdown: the one-off, or that day's shutdown_at (the next
// day's still happens). A balloon names the next one, if any.
void shutdown_cancel(void);
// TRUE when a shutdown is scheduled: at = "23:30" / "1:00 tomorrow", left =
// "in 1 h 5 min" (either may be NULL).
BOOL shutdown_next(WCHAR *at, int at_cch, WCHAR *left, int left_cch);
BOOL shutdown_dialog_message(MSG *msg); // IsDialogMessage for its windows

// ---------------------------------------------------------------------------
// App helpers (main.c)

extern HWND      g_main_wnd;
extern HINSTANCE g_hinst;

HICON app_load_icon(int cx, int cy);          // embedded icon at the given size
UINT dpi_for_monitor(HMONITOR mon);           // effective DPI (96 = 100%), works on Win7+
void app_log(const WCHAR *fmt, ...);          // OutputDebugString, plus vim.log if g_cfg.log
void app_reload_config(BOOL notify);          // reload g_cfg, notify modules, balloon on warnings
void app_config_written(void);                // we rewrote vim.ini: reload quietly
void app_quit(void);
BOOL app_has_uiaccess(void);                  // the signed UIAccess install (README): the overlay
                                              // may be drawn above the Start menu and banners

#endif
