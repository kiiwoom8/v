// common.h - shared contract between modules. See SPEC.md for behaviour.
//
//   main.c   entry point, single instance, DPI, main window, message routing,
//            config file watching, logging/error helpers
//   config.c settings file (vim.ini next to the exe): defaults, parsing, key names
//   input.c  low-level keyboard hook (the single keyboard dispatcher), mouse
//            mode, movement/scroll thread, mouse actions, mode indicator,
//            sleep/lock handling
//   hint.c   click-hint overlay (main/UI thread)
//   tray.c   tray icon + menu, help window, start-with-Windows

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

typedef struct {
    // [general]
    Hotkey hint_hotkey;        // default Alt+F
    Hotkey quit_hotkey;        // default Ctrl+Alt+Q
    Hotkey pause_hotkey;       // default unbound
    UINT   mouse_mode_key;     // default VK_CAPITAL; tap toggles, hold = momentary
    int    indicator;          // INDICATOR_*; default INDICATOR_CURSOR
    int    log;                // 1 = also write log lines to vim.log; default 0

    // [mouse]  speeds in physical px per second; scroll in wheel-delta units per second
    double speed;              // default 500
    double max_speed;          // default 1500
    double accel_time;         // seconds to reach max_speed, default 0.65
    double accel_curve;        // easing exponent, default 1.8
    double slow_speed;         // while Shift is held (no acceleration), default 120
    double scroll_speed;       // default 600   (5 notches/s)
    double scroll_max_speed;   // default 2400  (20 notches/s)
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
} Config;

extern Config g_cfg;

void         config_set_defaults(Config *c);
// Loads the settings file into *c (defaults first, then file values).
// If the file does not exist it is created with commented defaults and
// *created is set to TRUE. Returns FALSE only if the file exists but could not
// be read. Problems (unknown keys, bad values) are collected in
// config_warnings(); bad values keep their defaults.
BOOL         config_load(Config *c, BOOL *created);
const WCHAR *config_path(void);       // full path of vim.ini (exe dir + exe basename + ".ini")
const WCHAR *config_warnings(void);   // L"" if none; one problem per line, from last load
BOOL         parse_key(const WCHAR *s, UINT *vk);        // "h", ",", "CapsLock", "F13", "Comma"...
BOOL         parse_hotkey(const WCHAR *s, Hotkey *out);  // "Alt+F", "Ctrl+Alt+Q", "none"
void         format_key(UINT vk, WCHAR *buf, int cch);   // vk -> display name ("h", "CapsLock", ",")
void         format_hotkey(const Hotkey *hk, WCHAR *buf, int cch);  // "Alt+F"; "none" if unbound

// ---------------------------------------------------------------------------
// Input (input.c). Runs its own thread (hook + message loop) and a movement
// thread. All functions are safe to call from any thread.

BOOL input_start(void);                 // FALSE if the keyboard hook could not be installed
void input_stop(void);
void input_set_paused(BOOL paused);     // paused: every key passes through except quit/pause hotkeys
BOOL input_is_paused(void);
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

enum { BTN_LEFT = 0, BTN_RIGHT = 1, BTN_MIDDLE = 2 };
void mouse_move_to(POINT pt);                          // absolute, physical px
void mouse_click_at(POINT pt, int button, int count);  // count 1 = click, 2 = double click

// ---------------------------------------------------------------------------
// Hint overlay (hint.c). All functions run on the main/UI thread.

void hint_init(HINSTANCE inst);
void hint_shutdown(void);
void hint_open(void);                   // on the monitor under the cursor
void hint_close(void);
BOOL hint_is_open(void);
void hint_on_key(UINT vk, UINT mods);

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
// App helpers (main.c)

extern HWND      g_main_wnd;
extern HINSTANCE g_hinst;

UINT dpi_for_monitor(HMONITOR mon);           // effective DPI (96 = 100%), works on Win7+
void app_log(const WCHAR *fmt, ...);          // OutputDebugString, plus vim.log if g_cfg.log
void app_reload_config(BOOL notify);          // reload g_cfg, notify modules, balloon on warnings
void app_quit(void);

#endif
