// common.h - shared contract between modules. User-facing behaviour: README.md.
//
//   main.c   entry point, single instance, DPI, main window, message routing,
//            config file watching, logging/error helpers
//   config.c settings file (vim.ini next to the exe): defaults, parsing, key
//            names, text shortcut table
//   input.c  low-level keyboard hook (the single keyboard dispatcher), mouse
//            mode, movement/scroll thread, mouse actions, Alt scrolling, text
//            shortcut capture and typing, mode indicator, sleep/lock handling
//   hint.c   click-hint overlay (main/UI thread)
//   tray.c   tray icon + menu, help window, start-with-Windows
//   snippet.c text shortcut editor window (tray "Text shortcuts...")

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
#define WM_APP_HINT_PREPARE     (WM_APP + 13) // wParam 1 = the hint hotkey's modifiers went down, start scanning early;
                                              // 0 = the hotkey will not follow; -> hint_prepare

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
    Hotkey pause_hotkey;       // default unbound
    Hotkey lock_hotkey;        // default Ctrl+Alt+L; locks / unlocks the keyboard
    UINT   mouse_mode_key;     // default VK_CAPITAL; tap toggles, hold = momentary
    int    indicator;          // INDICATOR_*; default INDICATOR_CURSOR
    int    log;                // 1 = also write log lines to vim.log; default 0
    int    shortcut_paste;     // text shortcuts: 1 = paste via the clipboard (default), 0 = type keys

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
    int    alt_scroll_auto;    // 1 = Alt scrolling goes to the nearest scrollable area; default 1
    int    fast_scroll_lines;  // notches per Alt + u/d tap, default 5 (1-50)
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
    int   scan_scope;          // SCAN_MONITOR: every visible window on the monitor (default)
                               // SCAN_WINDOW: only the foreground window and the taskbar
    int   scan_timeout;        // how long to wait for every window's elements before
                               // showing the ones found so far (100-5000 ms, default 400).
                               // If none have arrived by then, the scan keeps running
                               // until the first results or 3 s, then falls back to the grid.
    int   prescan;             // 1 = scan (hints, Alt scroll targets) once Alt is held, default 1
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
const WCHAR *config_path(void);       // full path of vim.ini (exe dir + exe basename + ".ini")
const WCHAR *config_warnings(void);   // L"" if none; one problem per line, from last load
BOOL         parse_key(const WCHAR *s, UINT *vk);        // "h", ",", "CapsLock", "F13", "Comma"...
BOOL         parse_hotkey(const WCHAR *s, Hotkey *out);  // "Alt+F", "Ctrl+Alt+Q", "none"
void         format_key(UINT vk, WCHAR *buf, int cch);   // vk -> display name ("h", "CapsLock", ",")
void         format_hotkey(const Hotkey *hk, WCHAR *buf, int cch);  // "Alt+F"; "none" if unbound

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

enum { BTN_LEFT = 0, BTN_RIGHT = 1, BTN_MIDDLE = 2 };
void mouse_move_to(POINT pt);                          // absolute, physical px
void mouse_click_at(POINT pt, int button, int count);  // count 1 = click, 2 = double click

// ---------------------------------------------------------------------------
// Hint overlay (hint.c). All functions run on the main/UI thread.

struct ClickList;

void hint_init(HINSTANCE inst);
void hint_shutdown(void);
void hint_open(void);                   // on the monitor under the cursor
void hint_prepare(BOOL on);             // WM_APP_HINT_PREPARE: scan before the hotkey completes
void hint_close(void);
BOOL hint_is_open(void);
void hint_on_key(UINT vk, UINT mods);
void hint_on_elements(struct ClickList *list);  // WM_APP_ELEMENTS; always frees list
void hint_hotkey_again(void);           // reload: rescan the elements, or reset the grid

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
    int  count;
    ClickTarget items[];   // cumulative: every post contains everything found so far
} ClickList;

BOOL uia_start(void);       // FALSE if UI Automation is unavailable (hints fall back to grid)
void uia_stop(void);
BOOL uia_available(void);
// Asynchronous scan of the monitor (g_cfg.scan_scope decides which windows).
// Posts WM_APP_ELEMENTS with a malloc'd ClickList to g_main_wnd after each
// window that added targets (done = FALSE) and once at the end (done = TRUE).
// A newer uia_scan or uia_cancel makes the old scan stop posting.
void uia_scan(HMONITOR mon, UINT scan_id);
void uia_cancel(void);
// Alt scrolling: finds the element nearest to pt that scrolls that way in the
// window under pt (UI Automation, on its own thread; waits up to timeout_ms).
// TRUE with *target set when pt is not over such an element and the wheel
// should go to *target instead. One caller thread at a time (the movement
// thread). uia_stop stops it too.
BOOL uia_scroll_target(POINT pt, BOOL vertical, DWORD timeout_ms, POINT *target);
void uia_scroll_prefetch(POINT pt);     // start caching that window, never waits
void uia_scroll_stop(void);

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
// Inserts a text shortcut's text into the focused window: pastes it through
// the clipboard (restored afterwards), or types it (shortcut_method = type).
void snippet_insert(const WCHAR *text);

// ---------------------------------------------------------------------------
// App helpers (main.c)

extern HWND      g_main_wnd;
extern HINSTANCE g_hinst;

UINT dpi_for_monitor(HMONITOR mon);           // effective DPI (96 = 100%), works on Win7+
void app_log(const WCHAR *fmt, ...);          // OutputDebugString, plus vim.log if g_cfg.log
void app_reload_config(BOOL notify);          // reload g_cfg, notify modules, balloon on warnings
void app_config_written(void);                // we rewrote vim.ini: reload quietly
void app_quit(void);

#endif
