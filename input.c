// input.c - low-level keyboard hook, mouse mode, movement/scroll, indicator.
#include "common.h"

#include <math.h>
#include <stdlib.h>

#ifndef WM_WTSSESSION_CHANGE
#define WM_WTSSESSION_CHANGE 0x02B1
#endif
#ifndef WTS_SESSION_LOCK
#define WTS_SESSION_LOCK 0x7
#endif
#ifndef WTS_SESSION_UNLOCK
#define WTS_SESSION_UNLOCK 0x8
#endif
#ifndef WTS_REMOTE_DISCONNECT
#define WTS_REMOTE_DISCONNECT 0x4
#endif
#ifndef WTS_CONSOLE_DISCONNECT
#define WTS_CONSOLE_DISCONNECT 0x2
#endif
// The mouse hook's "this event came from SendInput" flag, and the system
// cursor ids SetSystemCursor takes (winuser.h declares them only with
// OEMRESOURCE defined, and MinGW.org lacks the newer ones).
#ifndef LLMHF_INJECTED
#define LLMHF_INJECTED 0x00000001
#endif
#ifndef OCR_NORMAL
#define OCR_NORMAL      32512
#endif
#ifndef OCR_IBEAM
#define OCR_IBEAM       32513
#endif
#ifndef OCR_WAIT
#define OCR_WAIT        32514
#endif
#ifndef OCR_CROSS
#define OCR_CROSS       32515
#endif
#ifndef OCR_UP
#define OCR_UP          32516
#endif
#ifndef OCR_SIZENWSE
#define OCR_SIZENWSE    32642
#endif
#ifndef OCR_SIZENESW
#define OCR_SIZENESW    32643
#endif
#ifndef OCR_SIZEWE
#define OCR_SIZEWE      32644
#endif
#ifndef OCR_SIZENS
#define OCR_SIZENS      32645
#endif
#ifndef OCR_SIZEALL
#define OCR_SIZEALL     32646
#endif
#ifndef OCR_NO
#define OCR_NO          32648
#endif
#ifndef OCR_HAND
#define OCR_HAND        32649
#endif
#ifndef OCR_APPSTARTING
#define OCR_APPSTARTING 32650
#endif
#ifndef OCR_HELP
#define OCR_HELP        32651
#endif
#ifndef OCR_PIN
#define OCR_PIN         32671
#endif
#ifndef OCR_PERSON
#define OCR_PERSON      32672
#endif
#ifndef WTS_REMOTE_CONNECT
#define WTS_REMOTE_CONNECT 0x3
#endif
#ifndef WTS_CONSOLE_CONNECT
#define WTS_CONSOLE_CONNECT 0x1
#endif
#ifndef NOTIFY_FOR_THIS_SESSION
#define NOTIFY_FOR_THIS_SESSION 0
#endif
#ifndef MOUSEEVENTF_HWHEEL
#define MOUSEEVENTF_HWHEEL 0x01000
#endif
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif
#ifndef LLKHF_INJECTED
#define LLKHF_INJECTED 0x10
#endif
#ifndef LLKHF_EXTENDED
#define LLKHF_EXTENDED 0x01
#endif
#ifndef GUI_INMENUMODE
#define GUI_INMENUMODE 0x00000004
#endif
#ifndef GUI_SYSTEMMENUMODE
#define GUI_SYSTEMMENUMODE 0x00000008
#endif
#ifndef GUI_POPUPMENUMODE
#define GUI_POPUPMENUMODE 0x00000010
#endif
#ifndef MOUSEEVENTF_VIRTUALDESK
#define MOUSEEVENTF_VIRTUALDESK 0x4000
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#ifndef ProcessPowerThrottling
#define ProcessPowerThrottling 4
#endif
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

typedef struct {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} VM_PROCESS_POWER_THROTTLING_STATE;

BOOL WINAPI WTSRegisterSessionNotification(HWND hWnd, DWORD dwFlags);
BOOL WINAPI WTSUnRegisterSessionNotification(HWND hWnd);
UINT WINAPI timeBeginPeriod(UINT uPeriod);
UINT WINAPI timeEndPeriod(UINT uPeriod);

#ifndef KLF_SHIFTLOCK
#define KLF_SHIFTLOCK 0x00010000
#endif

#define POWER_CLASS L"VimMouseInputPower"
#define IND_CLASS   L"VimMouseInputIndicator"
#define IND_TIMER   1
// capslock_off: the power window checks the CapsLock state this often, and
// the hook asks for a check at once when CapsLock or a modifier comes up.
#define CAPS_TIMER       2
#define CAPS_CHECK_MS    250
// A tap that left CapsLock on (an administrator window has the focus, so
// Windows dropped it) is retried after 0.5 s, then 1, 2, 4 and 8 s.
#define CAPS_RETRY_MS    500
// hide_pointer: the power window's timer that hides the pointer again once
// the real mouse has been still for hide_pointer ms. The largest value
// config.c accepts; leaving mouse mode or a hint click backdates the last
// mouse event by it so the pointer hides at once.
#define PTR_TIMER        3
#define PTR_IDLE_MAX_MS  60000

#define IND_KEY_COLOR RGB(255, 0, 255)
#define IND_BLUE      RGB(0x2D, 0x7F, 0xF9)
#define IND_ORANGE    RGB(0xF2, 0x99, 0x4A)
#define IND_WHITE     RGB(0xFF, 0xFF, 0xFF)
#define IND_DARK      RGB(0x1E, 0x1E, 0x1E)

#define WM_INPUT_SET_MODE    (WM_APP + 20)
#define WM_INPUT_SET_PAUSED  (WM_APP + 21)
#define WM_INPUT_CONFIG      (WM_APP + 22)
#define WM_INPUT_IND_UPDATE  (WM_APP + 24)
#define WM_INPUT_SET_LOCKED  (WM_APP + 25)
#define WM_INPUT_CAPS_CHECK  (WM_APP + 26)  // to s_power_wnd; wParam = key just released
#define WM_INPUT_PTR_UPDATE  (WM_APP + 27)  // decide again whether the pointer is hidden
#define WM_INPUT_CENTER      (WM_APP + 28)  // key_center twice: pointer to the middle

#define MOVE_LEFT  0x01
#define MOVE_DOWN  0x02
#define MOVE_UP    0x04
#define MOVE_RIGHT 0x08

#define SCR_UP    0x01
#define SCR_DOWN  0x02
#define SCR_LEFT  0x04
#define SCR_RIGHT 0x08
#define SCR_FAST_UP   0x10
#define SCR_FAST_DOWN 0x20

#define VELOCITY_SMOOTHING 0.12
#define MAX_SIM_STEP_SEC   0.05
#define SUSPEND_RESET_SEC  2.0
// Round-off slack when wheel accumulators are compared with whole units.
#define SCROLL_EPS 1e-9
// Scroll speed ramps from its base toward its maximum after this delay. A tap
// still delivers at least one notch (fast_scroll_lines notches for the fast keys).
#define SCROLL_HOLD_DELAY_MS 150.0
// g g / Shift + g glide to the end instead of jumping: the fast stream starts at
// fast_scroll_speed and grows exponentially with this time constant. The exact
// distance delivered after t is v0 * tau * (e^(t/tau) - 1); it stops once it
// reaches SCROLL_JUMP_UNITS, 64 of the largest signed 16-bit wheel deltas.
// Apps clamp at their end, so the tail beyond it is invisible.
#define SCROLL_JUMP_TAU_MS 50.0
#define SCROLL_JUMP_UNITS (64.0 * 32760.0)
// How long Alt scrolling waits for the nearest scrollable area before it
// scrolls under the cursor instead.
#define AUTO_TARGET_TIMEOUT_MS 500
// While an auto-target stream runs, refresh its list this often in the
// background so the next press (a tap, a held key) does not wait for it.
#define SCROLL_PREFETCH_MS 500
// After a click or a key that moves the focus, ask whether the focus is in a
// text field only once the app has had this long to move it.
#define FOCUS_CLICK_DELAY_MS 150
#define FOCUS_KEY_DELAY_MS   100
// Shift + g scrolls to the bottom through UI Automation on press; only where
// that cannot scroll the area does the wheel wait for Shift to come up. If
// Shift stays held longer than this, the press is forgotten.
#define SCROLL_END_SHIFT_MS 1000
// How long the movement thread waits for that UI Automation scroll before it
// falls back to the wheel at Shift release.
#define SCROLL_END_UIA_MS 300
// The hook may wait this long for an answer that is still being asked. It must
// stay far below Windows' LowLevelHooksTimeout, or Windows removes the hook.
// Waiting inside the hook keeps key order intact: no other key is processed
// meanwhile.
#define BARE_WAIT_MS 100
// Keys that reach the app tell the hints their scan ahead is old, at most
// this often.
#define AHEAD_FORGET_POST_MS 200

enum {
    ACT_NONE = 0,
    ACT_MOVE_LEFT, ACT_MOVE_DOWN, ACT_MOVE_UP, ACT_MOVE_RIGHT,
    ACT_SCROLL_UP, ACT_SCROLL_DOWN, ACT_SCROLL_LEFT, ACT_SCROLL_RIGHT,
    ACT_FAST_UP, ACT_FAST_DOWN,
    ACT_LEFT, ACT_CLICK_RIGHT, ACT_CLICK_MIDDLE, ACT_TOGGLE,
    ACT_HINTS, ACT_EXIT, ACT_HELP,
    ACT_BACK, ACT_FORWARD, ACT_SCROLL_END
};

enum { OWN_NONE = 0, OWN_PASS, OWN_SWALLOW, OWN_MOVE, OWN_LEFT, OWN_CAPS, OWN_ALT_HELD };

#define LEFT_SRC_NONE 0
#define LEFT_SRC_KEY  1
#define LEFT_SRC_TOG  2

static volatile LONG s_paused = 0;
static volatile LONG s_locked = 0;
static volatile LONG s_mouse_mode = 0;
static volatile LONG s_capture = 0;
static volatile LONG s_left_held = 0;
static volatile LONG s_move_flags = 0;
static volatile LONG s_scroll_flags = 0;
// Alt scrolling aims at the nearest scrollable area: the movement thread
// finds it (s_target_gen changed) before starting that press's stream (pending).
static volatile LONG s_scroll_auto = 0;
static volatile LONG s_scroll_pending = 0;
static volatile LONG s_target_gen = 0;
// g g goes to the top of the page and Shift + g to the bottom. With smooth
// scrolling the movement thread glides there on that direction's fast stream;
// without it, it sends the whole jump at once. The bit marks the press and is
// cleared when the thread starts it.
static volatile LONG s_scroll_jump = 0;
static volatile LONG s_move_exit = 0;

static unsigned char s_down[256];
static unsigned char s_owner[256];
static unsigned char s_action[256];

static int  s_caps_held = 0;
static int  s_caps_used = 0;
static int  s_caps_enter = 0;
static int  s_caps_exit_on_release = 0;
static int  s_left_source = LEFT_SRC_NONE;

static BOOL s_alt_pending = FALSE;
static BOOL s_alt_consumed = FALSE;
static UINT s_alt_scan = 0;
static BOOL s_alt_extended = FALSE;

// The last key was the pause hotkey (Alt+P). A second one within pause_tap_ms
// pauses or resumes; any other key starts over. Read and cleared by
// handle_hotkeys, the first thing every key down goes through.
static BOOL  s_pause_tap = FALSE;
static DWORD s_pause_tick = 0;

// The last key was a lone g. A second g within scroll_end_tap_ms goes to the
// top; any other key starts over. The first g may have typed: outside mouse
// mode a bare g reaches the app while the focus is still being asked for
// (bare_key_ok, s_bare_asked), and it still arms the second.
static BOOL  s_end_tap = FALSE;
static DWORD s_end_tap_tick = 0;

// The last key was a lone key_center (0). A second one within
// center_tap_ms puts the pointer in the middle of the screen; any other key
// starts over. s_center_ok is whether the first press was somewhere the
// jump is allowed (mouse mode, or the focus known not to be in a text field).
static BOOL  s_center_tap = FALSE;
static BOOL  s_center_ok = FALSE;
static DWORD s_center_tick = 0;

// Shift + g asked for the bottom while Shift was still held. It scrolls
// through UI Automation on press (Shift does not matter there); the wheel
// sent once Shift comes up is only the fallback for where that cannot scroll
// (browsers turn Shift + wheel into sideways scrolling).
static BOOL  s_end_shift = FALSE;
static BOOL  s_end_shift_auto = FALSE;
static DWORD s_end_shift_tick = 0;
// Where that bottom jump has got to. The hook only reads and moves it with
// InterlockedCompareExchange; the movement thread asks UI Automation.
enum {
    END_UIA_IDLE = 0,    // nothing pending
    END_UIA_ASK = 1,     // pressed with Shift held; the movement thread will ask
    END_UIA_ASKING = 2,  // the movement thread is asking UI Automation
    END_UIA_DONE = 3,    // UI Automation scrolled to the bottom
    END_UIA_FAILED = 4,  // UI Automation could not; the hook sends the wheel
    END_UIA_WHEEL = 5    // Shift came up first; the movement thread sends it
};
static volatile LONG s_end_uia = END_UIA_IDLE;

// A press bumps the direction's generation. The movement thread owns all
// scroll timing and emission, so the hook only records the press and wakes it.
static volatile LONG s_scroll_gen[6];

// A hint prescan was started (WM_APP_HINT_PREPARE 1) and not yet cancelled or
// adopted by the hint overlay.
static BOOL s_prepared = FALSE;
// Alt went down: the movement thread caches the scrollable areas under the
// pointer, so a following Alt scroll finds its target at once.
static volatile LONG s_scroll_prefetch = 0;
// When this program last scrolled, clicked or went back or forward
// (GetTickCount, 0 = never), which of them (PAGE_*) and where (screen px).
// Chromium reports the page that results to UI Automation up to half a second
// later, so the hints wait for it (scan_settle) and ask it there to hurry.
// Written by the hook, movement and UI threads; a reader that mixes two
// actions' fields only picks a worse place to ask.
static volatile LONG s_page_acted = 0;
static volatile LONG s_page_kind = 0;
static volatile LONG s_page_x = 0;
static volatile LONG s_page_y = 0;

// Text shortcut being typed while Alt is held (see shortcut_begin).
static BOOL  s_sc_active = FALSE;
static BOOL  s_sc_cancelled = FALSE;
static WCHAR s_sc_buf[SHORTCUT_ABBR_MAX + 1];
static int   s_sc_len = 0;

static HANDLE s_move_event = NULL;
static HANDLE s_move_thread = NULL;
static HANDLE s_input_thread = NULL;
static HANDLE s_ready_event = NULL;
static DWORD  s_input_tid = 0;
static BOOL   s_started_ok = FALSE;

static HHOOK s_hook = NULL;
// Focus and foreground changes, for bare keys (uia_focus_changed).
static HWINEVENTHOOK s_focus_hooks[2];
static HWND  s_power_wnd = NULL;
static HWND  s_ind_wnd = NULL;
static HFONT s_ind_font = NULL;
static int   s_ind_font_h = 0;
static BOOL  s_ind_timer_on = FALSE;
static volatile LONG s_ind_posted = 0;   // a WM_INPUT_IND_UPDATE from a move is queued
// Windows silently removes a low-level hook whose callback is too slow. Raw
// keyboard input still arrives, so compare its count with the hook's to notice.
static volatile LONG s_kb_calls = 0;
static LONG  s_kb_calls_seen = 0;
static int   s_hook_misses = 0;
static DWORD s_hook_replace_tick = 0;
static BOOL  s_raw_kb_on = FALSE;
// capslock_off, input thread only: our last tap that turned CapsLock off
// (0 = none since it was last seen off), how many taps left it on, the
// foreground window then, and whether the timer runs.
static DWORD s_caps_fix_tick = 0;
static int   s_caps_fix_fails = 0;
static HWND  s_caps_fix_fg = NULL;
static BOOL  s_caps_timer_on = FALSE;
static BOOL  s_caps_shiftlock_logged = FALSE;
// hide_pointer. The input thread decides (ptr_update) and a pointer thread
// of its own swaps the system cursors, so neither the hook thread nor the
// UI waits on SetSystemCursor or SPI_SETCURSORS (the latter reads the
// cursor files). s_ptr_want is the decision, s_ptr_hidden what is in
// effect; the mouse hook notes the real mouse in s_ptr_phys_tick and asks
// for a new decision while the pointer is hidden (one post in flight).
// s_own_x / s_own_y is where our last SetCursorPos put the pointer: that
// move is not flagged injected, but lands exactly there.
static HHOOK  s_mouse_hook = NULL;
static HANDLE s_ptr_thread = NULL;
static HANDLE s_ptr_event = NULL;
static volatile LONG s_ptr_want = 0;
static volatile LONG s_ptr_hidden = 0;
static volatile LONG s_ptr_exit = 0;
static volatile LONG s_ptr_posted = 0;
static volatile LONG s_ptr_phys_tick = 0;
static volatile LONG s_own_x = -1;
static volatile LONG s_own_y = -1;
static BOOL   s_ptr_away = FALSE;      // session locked or asleep: pointer shown
static BOOL   s_ptr_timer_on = FALSE;

#ifdef INPUT_TEST
static BOOL s_test_mods_on = FALSE;
static UINT s_test_mods_value = 0;
static BOOL s_test_popup_on = FALSE;
static BOOL s_test_popup_value = FALSE;
#endif

static void indicator_schedule(void);
static void indicator_update(void);
static void indicator_moved(void);
static BOOL install_hook(void);
static void uninstall_hook(void);
static void reset_key_state(void);
static void suspend_input(void);
static void resume_input(void);
static BOOL handle_up(UINT vk);
static void prescan_cancel(void);
static void ptr_schedule(void);
static void ptr_forget_mouse(void);

static BOOL flag_get(volatile LONG *p)
{
    return InterlockedCompareExchange(p, 0, 0) != 0;
}

static void flag_set(volatile LONG *p, BOOL value)
{
    InterlockedExchange(p, value ? 1 : 0);
}

static LONG atomic_or(volatile LONG *p, LONG value)
{
    LONG old, next;
    do {
        old = InterlockedCompareExchange(p, 0, 0);
        next = old | value;
    } while (InterlockedCompareExchange(p, next, old) != old);
    return next;
}

static LONG atomic_and(volatile LONG *p, LONG value)
{
    LONG old, next;
    do {
        old = InterlockedCompareExchange(p, 0, 0);
        next = old & value;
    } while (InterlockedCompareExchange(p, next, old) != old);
    return next;
}

static void post_main(UINT msg, WPARAM wp, LPARAM lp)
{
    HWND wnd = g_main_wnd;
    if (wnd)
        PostMessageW(wnd, msg, wp, lp);
}

static void notify_state(void)
{
    post_main(WM_APP_STATE_CHANGED, 0, 0);
    // Mouse mode and pause decide whether the pointer is hidden.
    ptr_schedule();
}

// A burst of actions (a scroll stream sends a wheel event every frame) tells
// the hints once; their wait re-reads the newest action itself.
#define PAGE_ACTED_POST_MS 100

// at: where it happened; NULL = at the cursor (the wheel and clicks go there).
static void page_acted_at(int kind, const POINT *at)
{
    DWORD now = GetTickCount();
    POINT pt = { 0, 0 };
    LONG prev, prev_kind;

    if (at)
        pt = *at;
    else
        GetCursorPos(&pt);
    prev_kind = InterlockedExchange(&s_page_kind, kind);
    InterlockedExchange(&s_page_x, pt.x);
    InterlockedExchange(&s_page_y, pt.y);
    prev = InterlockedExchange(&s_page_acted, (LONG)(now ? now : 1));
    // A Chromium page reports the result late; the hints start waiting for
    // it now rather than when the hotkey comes (hint_page_acted).
    if (prev == 0 || prev_kind != kind ||
        now - (DWORD)prev > PAGE_ACTED_POST_MS)
        post_main(WM_APP_PAGE_ACTED, (WPARAM)kind, 0);
}

static void page_acted(int kind)
{
    page_acted_at(kind, NULL);
}

DWORD input_since_page_action(int *kind, POINT *pt)
{
    DWORD at = (DWORD)InterlockedCompareExchange(&s_page_acted, 0, 0);

    if (kind)
        *kind = (int)InterlockedCompareExchange(&s_page_kind, 0, 0);
    if (pt) {
        pt->x = InterlockedCompareExchange(&s_page_x, 0, 0);
        pt->y = InterlockedCompareExchange(&s_page_y, 0, 0);
    }
    return at ? GetTickCount() - at : INFINITE;
}

// Absolute moves are not distorted by "Enhance pointer precision", unlike
// relative MOUSEEVENTF_MOVE. Coordinates are physical pixels of the whole
// virtual desktop.
static void send_absolute_move(int x, int y, int vx, int vy, int vw, int vh)
{
    INPUT in;
    LONGLONG nx, ny;

    if (vw <= 0)
        vw = 1;
    if (vh <= 0)
        vh = 1;
    nx = ((LONGLONG)(x - vx) * 65536 + vw - 1) / vw;
    ny = ((LONGLONG)(y - vy) * 65536 + vh - 1) / vh;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)nx;
    in.mi.dy = (LONG)ny;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                    MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof(in));
}

// A click of ours may move the focus into or out of a text field, and open,
// close or move something on the page.
static void focus_clicked_at(const POINT *at)
{
    page_acted_at(PAGE_CLICK, at);
    if (g_cfg.bare_keys)
        uia_focus_changed(FOCUS_CLICK_DELAY_MS);
}

static void send_button(int button, BOOL down)
{
    INPUT in;
    focus_clicked_at(NULL);
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    if (button == BTN_LEFT)
        in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    else if (button == BTN_RIGHT)
        in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    else
        in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    SendInput(1, &in, sizeof(in));
}

static void click_button(int button)
{
    INPUT in[2];
    focus_clicked_at(NULL);
    ZeroMemory(in, sizeof(in));
    in[0].type = INPUT_MOUSE;
    in[1].type = INPUT_MOUSE;
    if (button == BTN_LEFT) {
        in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    } else if (button == BTN_RIGHT) {
        in[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
        in[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    } else {
        in[0].mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
        in[1].mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
    }
    SendInput(2, in, sizeof(INPUT));
}

static void send_wheel(BOOL vertical, int delta)
{
    INPUT in;
    page_acted(PAGE_SCROLL);
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.mouseData = (DWORD)delta;
    in.mi.dwFlags = vertical ? MOUSEEVENTF_WHEEL : MOUSEEVENTF_HWHEEL;
    SendInput(1, &in, sizeof(in));
}

// Without smooth scrolling, jumping to the end is 64 events of the largest
// delta a signed 16-bit wheel value can carry (273 notches); apps clamp at
// their end. Smooth scrolling glides there instead (see SCROLL_JUMP_*).
static void send_wheel_jump(BOOL vertical, int delta)
{
    INPUT in[64];
    int i;

    page_acted(PAGE_SCROLL);
    ZeroMemory(in, sizeof(in));
    for (i = 0; i < 64; i++) {
        in[i].type = INPUT_MOUSE;
        in[i].mi.mouseData = (DWORD)delta;
        in[i].mi.dwFlags = vertical ? MOUSEEVENTF_WHEEL : MOUSEEVENTF_HWHEEL;
    }
    SendInput(64, in, sizeof(INPUT));
}

static void send_mask_key(void)
{
    INPUT in[2];
    ZeroMemory(in, sizeof(in));
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = 0xE8;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = 0xE8;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static void fill_alt_down(INPUT *in)
{
    ZeroMemory(in, sizeof(*in));
    in->type = INPUT_KEYBOARD;
    in->ki.wVk = VK_LMENU;
    in->ki.wScan = (WORD)s_alt_scan;
    in->ki.dwFlags = s_alt_extended ? KEYEVENTF_EXTENDEDKEY : 0;
}

static void send_alt_replay(UINT vk, UINT scan, DWORD flags)
{
    INPUT in[2];
    fill_alt_down(&in[0]);
    ZeroMemory(&in[1], sizeof(in[1]));
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = (WORD)vk;
    in[1].ki.wScan = (WORD)scan;
    in[1].ki.dwFlags = (flags & LLKHF_EXTENDED) ? KEYEVENTF_EXTENDEDKEY : 0;
    SendInput(2, in, sizeof(INPUT));
}

static void send_alt_tap(void)
{
    INPUT in[2];
    fill_alt_down(&in[0]);
    ZeroMemory(&in[1], sizeof(in[1]));
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = VK_LMENU;
    in[1].ki.wScan = (WORD)s_alt_scan;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP |
                       (s_alt_extended ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(2, in, sizeof(INPUT));
}

// Back / forward: Alt+Left / Alt+Right, which browsers, Explorer and most
// apps share. With the real Alt held the app has seen its Alt go down already.
static void send_nav(BOOL forward, BOOL alt_held)
{
    INPUT in[4];
    UINT vk = forward ? VK_RIGHT : VK_LEFT;
    UINT n = 0;

    page_acted(PAGE_NAV);
    ZeroMemory(in, sizeof(in));
    if (!alt_held) {
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = VK_LMENU;
        in[n].ki.wScan = (WORD)MapVirtualKeyW(VK_LMENU, MAPVK_VK_TO_VSC);
        n++;
    }
    in[n].type = INPUT_KEYBOARD;
    in[n].ki.wVk = (WORD)vk;
    in[n].ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in[n].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    n++;
    in[n] = in[n - 1];
    in[n].ki.dwFlags |= KEYEVENTF_KEYUP;
    n++;
    if (!alt_held) {
        in[n] = in[0];
        in[n].ki.dwFlags = KEYEVENTF_KEYUP;
        n++;
    }
    SendInput(n, in, sizeof(INPUT));
}

static void left_set(BOOL held)
{
    LONG old = InterlockedExchange(&s_left_held, held ? 1 : 0);
    if ((old != 0) != (held != 0)) {
        notify_state();
        indicator_schedule();
    }
}

static void enter_mouse_mode(void)
{
    if (flag_get(&s_mouse_mode))
        return;
    InterlockedExchange(&s_mouse_mode, 1);
    notify_state();
    indicator_schedule();
}

static void leave_mouse_mode(void)
{
    if (flag_get(&s_left_held)) {
        send_button(BTN_LEFT, FALSE);
        s_left_source = LEFT_SRC_NONE;
        left_set(FALSE);
    }
    InterlockedExchange(&s_move_flags, 0);
    InterlockedExchange(&s_scroll_flags, 0);
    if (flag_get(&s_mouse_mode)) {
        InterlockedExchange(&s_mouse_mode, 0);
        // Back to the keyboard: the pointer hides at once, not after the
        // idle time the real mouse gets.
        ptr_forget_mouse();
        notify_state();
        indicator_schedule();
    }
}

static void set_mouse_mode(BOOL on)
{
    if (on)
        enter_mouse_mode();
    else
        leave_mouse_mode();
}

static void apply_paused(BOOL paused)
{
    LONG value = paused ? 1 : 0;
    LONG old = InterlockedExchange(&s_paused, value);
    if (old == value)
        return;
    if (value) {
        leave_mouse_mode();
        prescan_cancel();
        if (flag_get(&s_capture)) {
            flag_set(&s_capture, FALSE);
            post_main(WM_APP_HINT_CLOSE, 0, 0);
        }
    }
    notify_state();
}

// Every pointer move of ours through SetCursorPos goes through here, so the
// mouse hook can tell it from the real mouse (see s_own_x).
static void own_move(POINT pt)
{
    InterlockedExchange(&s_own_x, pt.x);
    InterlockedExchange(&s_own_y, pt.y);
    SetCursorPos(pt.x, pt.y);
}

void mouse_move_to(POINT pt)
{
    own_move(pt);
}

void mouse_click_at(POINT pt, int button, int count)
{
    INPUT in[4];
    POINT was;
    BOOL back;
    int n;
    int i;

    // click_return: once the click is in, the pointer goes back to where it
    // was, so it is not left on what was clicked (the move after the click
    // enters the same input queue, so the app sees the click there first).
    // Not in mouse mode, where the pointer is being driven and carries on
    // from the element.
    back = g_cfg.click_return && !flag_get(&s_mouse_mode) && GetCursorPos(&was);
    focus_clicked_at(&pt);
    own_move(pt);
    if (count == 2)
        n = 4;
    else
        n = 2;
    ZeroMemory(in, sizeof(in));
    for (i = 0; i < n; i += 2) {
        in[i].type = INPUT_MOUSE;
        in[i + 1].type = INPUT_MOUSE;
        if (button == BTN_RIGHT) {
            in[i].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
            in[i + 1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        } else if (button == BTN_MIDDLE) {
            in[i].mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
            in[i + 1].mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
        } else {
            in[i].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            in[i + 1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        }
    }
    SendInput((UINT)n, in, sizeof(INPUT));
    if (back)
        own_move(was);
    // A click off the keyboard: the pointer hides at once if it was shown
    // for the real mouse.
    if (!flag_get(&s_mouse_mode)) {
        ptr_forget_mouse();
        ptr_schedule();
    }
}

// Menus and flyouts are title-less popups owned by the app's window. The owner
// test keeps out the taskbar, which lives in explorer.exe above every
// Explorer window. Tooltips and Chrome's link status bubble are popups too but
// never take input, so they do not block bare keys.
static BOOL window_is_popup(HWND hwnd)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    LONG exstyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    WCHAR cls[32];

    if (!(style & WS_POPUP) || (style & WS_CAPTION) || !GetWindow(hwnd, GW_OWNER))
        return FALSE;
    GetClassNameW(hwnd, cls, 32);
    if ((exstyle & WS_EX_TRANSPARENT) ||
        lstrcmpW(cls, L"tooltips_class32") == 0)
        return FALSE;
    return lstrcmpW(cls, L"Shell_TrayWnd") != 0 &&
           lstrcmpW(cls, L"Shell_SecondaryTrayWnd") != 0;
}

static BOOL window_cloaked(HWND hwnd)
{
    typedef HRESULT (WINAPI *DwmGetWindowAttributeFn)(HWND, DWORD, PVOID, DWORD);
    static BOOL tried = FALSE;
    static DwmGetWindowAttributeFn get_attr = NULL;
    DWORD cloaked = 0;

    if (!tried) {
        HMODULE dll = LoadLibraryW(L"dwmapi.dll");
        tried = TRUE;
        if (dll)
            get_attr = (DwmGetWindowAttributeFn)(void *)
                GetProcAddress(dll, "DwmGetWindowAttribute");
    }
    if (!get_attr)
        return FALSE;
    // 14 = DWMWA_CLOAKED (avoid depending on dwmapi.h/lib).
    if (get_attr(hwnd, 14, &cloaked, sizeof(cloaked)) != S_OK)
        return FALSE;
    return cloaked != 0;
}

// which (may be NULL) gets the window that counts as open: the popup, or the
// foreground window while it is in menu mode. With skip_panels, Chrome's panels
// (downloads, extensions, permission prompts) do not count, neither as the
// foreground root nor in the z-order walk, which keeps looking for a real menu
// above them (one opened from inside the panel).
static BOOL popup_open(BOOL skip_panels, HWND *which)
{
#ifdef INPUT_TEST
    if (s_test_popup_on)
        return s_test_popup_value;
#endif
    HWND fg;
    HWND root;
    HWND w;
    DWORD pid = 0;
    DWORD tid;
    DWORD wpid;
    GUITHREADINFO gti;
    RECT rc;
    int steps;

    if (which)
        *which = NULL;
    fg = GetForegroundWindow();
    if (fg) {
        tid = GetWindowThreadProcessId(fg, &pid);
        ZeroMemory(&gti, sizeof(gti));
        gti.cbSize = sizeof(gti);
        if (tid && GetGUIThreadInfo(tid, &gti) &&
            (gti.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE |
                          GUI_SYSTEMMENUMODE))) {
            if (which)
                *which = fg;
            return TRUE;
        }
    }
    if (!fg || pid == 0)
        return FALSE;

    root = GetAncestor(fg, GA_ROOT);
    if (root && window_is_popup(root) &&
        !(skip_panels && uia_chromium_panel(root))) {
        if (which)
            *which = root;
        return TRUE;
    }

    steps = 0;
    w = GetTopWindow(NULL);
    while (w && steps < 256) {
        if (w == root)
            break;
        if (IsWindowVisible(w)) {
            GetWindowThreadProcessId(w, &wpid);
            if (wpid == pid && window_is_popup(w) &&
                !(skip_panels && uia_chromium_panel(w)) &&
                GetWindowRect(w, &rc) && !IsRectEmpty(&rc) &&
                !window_cloaked(w)) {
                if (which)
                    *which = w;
                return TRUE;
            }
        }
        w = GetWindow(w, GW_HWNDNEXT);
        steps++;
    }
    return FALSE;
}

static UINT current_mods(void)
{
#ifdef INPUT_TEST
    if (s_test_mods_on)
        return s_test_mods_value;
#endif
    UINT mods = 0;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        mods |= KMOD_SHIFT;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
        mods |= KMOD_CTRL;
    if (s_owner[VK_LMENU] == OWN_ALT_HELD ||
        (GetAsyncKeyState(VK_MENU) & 0x8000))
        mods |= KMOD_ALT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000))
        mods |= KMOD_WIN;
    return mods;
}

static BOOL is_modifier(UINT vk)
{
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
        return TRUE;
    }
    return FALSE;
}

// KMOD_* bit of a modifier key, 0 for any other key.
static UINT mod_bit(UINT vk)
{
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        return KMOD_SHIFT;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        return KMOD_CTRL;
    case VK_MENU: case VK_LMENU: case VK_RMENU:
        return KMOD_ALT;
    case VK_LWIN: case VK_RWIN:
        return KMOD_WIN;
    }
    return 0;
}

// The hotkey's modifier went down: start the hint scan early so it overlaps
// typing the hotkey. Inside the hook GetAsyncKeyState does not see the key
// being processed yet, so add its bit ourselves.
static void prescan_mod_down(UINT vk)
{
    UINT bit = mod_bit(vk);
    UINT mods;

    if (bit == 0 || !g_cfg.prescan)
        return;
    if (flag_get(&s_paused) || flag_get(&s_locked) || flag_get(&s_capture))
        return;
    mods = current_mods() | bit;
    // Alt scrolling may follow: the movement thread looks up its target now
    // (not here: the hook must not wait on other windows).
    if (mods == KMOD_ALT && g_cfg.alt_scroll && g_cfg.alt_scroll_auto) {
        InterlockedExchange(&s_scroll_prefetch, 1);
        if (s_move_event)
            SetEvent(s_move_event);
    }
    if (s_prepared || g_cfg.hint_hotkey.vk == 0 ||
        mods != g_cfg.hint_hotkey.mods)
        return;
    s_prepared = TRUE;
    post_main(WM_APP_HINT_PREPARE, 1, 0);
}

// The hotkey will not follow (another key or a modifier release): drop the
// prescan. Posting 0 when unsure is harmless.
static void prescan_cancel(void)
{
    if (!s_prepared)
        return;
    s_prepared = FALSE;
    post_main(WM_APP_HINT_PREPARE, 0, 0);
}

static BOOL is_typing_key(UINT vk)
{
    if (vk >= 'A' && vk <= 'Z')
        return TRUE;
    if (vk >= '0' && vk <= '9')
        return TRUE;
    if (vk == VK_SPACE)
        return TRUE;
    if (vk >= VK_OEM_1 && vk <= 0xE2)
        return TRUE;
    return FALSE;
}

static BOOL hotkey_match(const Hotkey *hk, UINT vk, UINT mods)
{
    return hk->vk != 0 && hk->vk == vk && hk->mods == mods;
}

static int binding_action(UINT vk)
{
    if (g_cfg.key_left && vk == g_cfg.key_left)
        return ACT_MOVE_LEFT;
    if (g_cfg.key_down && vk == g_cfg.key_down)
        return ACT_MOVE_DOWN;
    if (g_cfg.key_up && vk == g_cfg.key_up)
        return ACT_MOVE_UP;
    if (g_cfg.key_right && vk == g_cfg.key_right)
        return ACT_MOVE_RIGHT;
    if (g_cfg.key_scroll_up && vk == g_cfg.key_scroll_up)
        return ACT_SCROLL_UP;
    if (g_cfg.key_scroll_down && vk == g_cfg.key_scroll_down)
        return ACT_SCROLL_DOWN;
    if (g_cfg.key_scroll_left && vk == g_cfg.key_scroll_left)
        return ACT_SCROLL_LEFT;
    if (g_cfg.key_scroll_right && vk == g_cfg.key_scroll_right)
        return ACT_SCROLL_RIGHT;
    if (g_cfg.key_scroll_end && vk == g_cfg.key_scroll_end &&
        g_cfg.scroll_end_tap_ms > 0)
        return ACT_SCROLL_END;
    if (g_cfg.key_left_button && vk == g_cfg.key_left_button)
        return ACT_LEFT;
    if (g_cfg.key_right_click && vk == g_cfg.key_right_click)
        return ACT_CLICK_RIGHT;
    if (g_cfg.key_middle_click && vk == g_cfg.key_middle_click)
        return ACT_CLICK_MIDDLE;
    if (g_cfg.key_drag_toggle && vk == g_cfg.key_drag_toggle)
        return ACT_TOGGLE;
    if (g_cfg.key_hints && vk == g_cfg.key_hints)
        return ACT_HINTS;
    if (g_cfg.key_exit && vk == g_cfg.key_exit)
        return ACT_EXIT;
    if (g_cfg.key_help && vk == g_cfg.key_help)
        return ACT_HELP;
    return ACT_NONE;
}

static void move_press(int act)
{
    LONG bit = (LONG)(1 << (act - ACT_MOVE_LEFT));
    atomic_or(&s_move_flags, bit);
    if (s_move_event)
        SetEvent(s_move_event);
}

static void move_release(int act)
{
    LONG bit = (LONG)(1 << (act - ACT_MOVE_LEFT));
    atomic_and(&s_move_flags, ~bit);
}

// Scroll indexes 0-5: up, down, left, right, fast up, fast down.
static BOOL scroll_vertical(int idx)
{
    return idx != 2 && idx != 3;
}

static int scroll_notch(int idx)
{
    switch (idx) {
    case 0:  return 120;
    case 1:  return -120;
    case 2:  return -120;
    case 3:  return 120;
    case 4:  return 120 * g_cfg.fast_scroll_lines;
    default: return -120 * g_cfg.fast_scroll_lines;
    }
}

// The wheel normally goes to the window under the cursor. Aimed at another
// point, it is posted there instead, so the cursor does not move.
static void post_wheel(POINT pt, BOOL vertical, int delta)
{
    typedef BOOL (WINAPI *ToLogicalFn)(HWND, LPPOINT);
    static BOOL tried = FALSE;
    static ToLogicalFn to_logical = NULL;
    HWND hwnd = WindowFromPoint(pt);

    if (!hwnd) {
        send_wheel(vertical, delta);
        return;
    }
    page_acted_at(PAGE_SCROLL, &pt);
    if (!tried) {
        tried = TRUE;
        to_logical = (ToLogicalFn)(void *)GetProcAddress(
            GetModuleHandleW(L"user32.dll"),
            "PhysicalToLogicalPointForPerMonitorDPI");
    }
    // A DPI-unaware window expects its own (scaled) coordinates.
    if (to_logical)
        to_logical(hwnd, &pt);
    PostMessageW(hwnd, vertical ? WM_MOUSEWHEEL : WM_MOUSEHWHEEL,
                 MAKEWPARAM(0, (WORD)(SHORT)delta),
                 MAKELPARAM((WORD)(SHORT)pt.x, (WORD)(SHORT)pt.y));
}

static DWORD qpc_us_from(LARGE_INTEGER counter, LARGE_INTEGER freq)
{
    LONGLONG secs, rem;

    if (freq.QuadPart <= 0)
        return 0;
    secs = counter.QuadPart / freq.QuadPart;
    rem = counter.QuadPart % freq.QuadPart;
    return (DWORD)(secs * 1000000 + (rem * 1000000) / freq.QuadPart);
}

// auto_target: Alt scrolling, which finds the nearest scrollable area first.
// jump: g g or Shift + g, which scrolls to the top / bottom instead of
// starting a stream; it is not held.
// Only record the press here; the movement thread does all timing and emission.
static void scroll_press(int act, BOOL auto_target, BOOL jump)
{
    int idx = act - ACT_SCROLL_UP;
    LONG bit = (LONG)(1 << idx);

    if (jump)
        atomic_or(&s_scroll_jump, bit);
    if (auto_target) {
        atomic_or(&s_scroll_auto, bit);
        atomic_or(&s_scroll_pending, bit);
        InterlockedIncrement(&s_target_gen);
    } else {
        atomic_and(&s_scroll_auto, ~bit);
    }
    InterlockedIncrement(&s_scroll_gen[idx]);
    if (!jump)
        atomic_or(&s_scroll_flags, bit);
    if (s_move_event)
        SetEvent(s_move_event);
}

static void scroll_release(int act)
{
    int idx = act - ACT_SCROLL_UP;
    atomic_and(&s_scroll_flags, ~(LONG)(1 << idx));
}

// g g goes to the top and Shift + g to the bottom, as a jump on the fast up /
// down stream. A lone g only waits for the second; armed: the key right before
// was one.
static void scroll_end_press(BOOL shift, BOOL armed, BOOL auto_target)
{
    DWORD now = GetTickCount();

    if (shift) {
        // The movement thread scrolls through UI Automation on this press,
        // which a held Shift does not affect. The wheel it may send at Shift
        // up is the fallback: browsers read Shift with GetKeyState and turn
        // Shift + wheel into sideways scrolling, and a fake Shift up/down
        // could leave it stuck.
        if (s_down[VK_LSHIFT] || s_down[VK_RSHIFT]) {
            s_end_shift = TRUE;
            s_end_shift_auto = auto_target;
            s_end_shift_tick = now;
            // The movement thread sees ASK, asks UI Automation and only falls
            // back to the wheel itself if Shift came up meanwhile.
            InterlockedExchange(&s_end_uia, END_UIA_ASK);
        }
        scroll_press(ACT_FAST_DOWN, auto_target, TRUE);
    } else if (armed && (DWORD)(now - s_end_tap_tick) <=
                            (DWORD)g_cfg.scroll_end_tap_ms) {
        if (g_cfg.log)
            app_log(L"g g: top, second tap %lu ms after the first",
                    (unsigned long)(now - s_end_tap_tick));
        scroll_press(ACT_FAST_UP, auto_target, TRUE);
    } else {
        if (g_cfg.log && armed)
            app_log(L"g g: second tap %lu ms after the first, over "
                    L"scroll_end_tap_ms (%d); it is the first again",
                    (unsigned long)(now - s_end_tap_tick),
                    g_cfg.scroll_end_tap_ms);
        s_end_tap = TRUE;
        s_end_tap_tick = now;
    }
}

// The last Shift came up. UI Automation may already have scrolled; if it is
// still asking, the movement thread sends the wheel itself only when that
// fails; otherwise send the fallback wheel jump now. A stale press is dropped.
static void scroll_end_shift_up(void)
{
    DWORD now;

    if (!s_end_shift)
        return;
    if (s_down[VK_LSHIFT] || s_down[VK_RSHIFT])
        return;
    s_end_shift = FALSE;
    now = GetTickCount();
    if ((DWORD)(now - s_end_shift_tick) > SCROLL_END_SHIFT_MS) {
        InterlockedExchange(&s_end_uia, END_UIA_IDLE);
        return;
    }
    // Never block here: the movement thread owns the UI Automation ask.
    for (;;) {
        LONG state = InterlockedCompareExchange(&s_end_uia, 0, 0);

        if (state == END_UIA_ASK || state == END_UIA_ASKING) {
            if (InterlockedCompareExchange(&s_end_uia, END_UIA_WHEEL,
                                           state) == state)
                return;
            continue;
        }
        if (state == END_UIA_DONE) {
            InterlockedExchange(&s_end_uia, END_UIA_IDLE);
            return;
        }
        InterlockedExchange(&s_end_uia, END_UIA_IDLE);
        break;
    }
    scroll_press(ACT_FAST_DOWN, s_end_shift_auto, TRUE);
}

static void left_press_key(void)
{
    if (flag_get(&s_left_held)) {
        send_button(BTN_LEFT, FALSE);
        s_left_source = LEFT_SRC_NONE;
        left_set(FALSE);
    } else {
        send_button(BTN_LEFT, TRUE);
        s_left_source = LEFT_SRC_KEY;
        left_set(TRUE);
    }
}

static void left_release_key(void)
{
    if (s_left_source == LEFT_SRC_KEY) {
        if (flag_get(&s_left_held)) {
            send_button(BTN_LEFT, FALSE);
            left_set(FALSE);
        }
        s_left_source = LEFT_SRC_NONE;
    }
}

static void left_toggle_key(void)
{
    if (flag_get(&s_left_held)) {
        send_button(BTN_LEFT, FALSE);
        s_left_source = LEFT_SRC_NONE;
        left_set(FALSE);
    } else {
        send_button(BTN_LEFT, TRUE);
        s_left_source = LEFT_SRC_TOG;
        left_set(TRUE);
    }
}

static void mark_caps_used(void)
{
    if (s_caps_held)
        s_caps_used = TRUE;
}

static void caps_key_down(void)
{
    s_caps_held = TRUE;
    s_caps_used = FALSE;
    if (!flag_get(&s_mouse_mode)) {
        s_caps_enter = TRUE;
        s_caps_exit_on_release = FALSE;
        enter_mouse_mode();
    } else {
        // Leave on the press, not the release, so turning it off is instant.
        s_caps_enter = FALSE;
        s_caps_exit_on_release = FALSE;
        leave_mouse_mode();
    }
}

static void caps_key_up(void)
{
    BOOL leave = s_caps_exit_on_release || (s_caps_enter && s_caps_used);
    s_caps_held = FALSE;
    s_caps_used = FALSE;
    s_caps_enter = FALSE;
    s_caps_exit_on_release = FALSE;
    if (leave)
        leave_mouse_mode();
}

static void apply_action_down(int act)
{
    switch (act) {
    case ACT_MOVE_LEFT: case ACT_MOVE_DOWN: case ACT_MOVE_UP: case ACT_MOVE_RIGHT:
        move_press(act);
        break;
    case ACT_SCROLL_UP: case ACT_SCROLL_DOWN: case ACT_SCROLL_LEFT: case ACT_SCROLL_RIGHT:
        scroll_press(act, FALSE, FALSE);
        break;
    case ACT_LEFT:
        left_press_key();
        break;
    case ACT_CLICK_RIGHT:
        click_button(BTN_RIGHT);
        break;
    case ACT_CLICK_MIDDLE:
        click_button(BTN_MIDDLE);
        break;
    case ACT_TOGGLE:
        left_toggle_key();
        break;
    case ACT_HINTS:
        flag_set(&s_capture, TRUE);
        post_main(WM_APP_HINT_OPEN, 0, 0);
        break;
    case ACT_EXIT:
        leave_mouse_mode();
        break;
    case ACT_HELP:
        post_main(WM_APP_SHOW_HELP, 0, 0);
        break;
    default:
        break;
    }
}

// Alt + a move key scrolls that way, anywhere; Alt + the scroll up/down
// keys scroll fast and Alt + the scroll-end key goes to the top / bottom.
// With alt_nav, Alt + left / right go back / forward instead and the side
// scroll keys scroll sideways.
static int alt_key_action(UINT vk)
{
    if (!g_cfg.alt_scroll)
        return ACT_NONE;
    if (g_cfg.key_left && vk == g_cfg.key_left)
        return g_cfg.alt_nav ? ACT_BACK : ACT_SCROLL_LEFT;
    if (g_cfg.key_down && vk == g_cfg.key_down)
        return ACT_SCROLL_DOWN;
    if (g_cfg.key_up && vk == g_cfg.key_up)
        return ACT_SCROLL_UP;
    if (g_cfg.key_right && vk == g_cfg.key_right)
        return g_cfg.alt_nav ? ACT_FORWARD : ACT_SCROLL_RIGHT;
    if (g_cfg.alt_nav && g_cfg.key_scroll_left && vk == g_cfg.key_scroll_left)
        return ACT_SCROLL_LEFT;
    if (g_cfg.alt_nav && g_cfg.key_scroll_right &&
        vk == g_cfg.key_scroll_right)
        return ACT_SCROLL_RIGHT;
    if (g_cfg.key_scroll_up && vk == g_cfg.key_scroll_up)
        return ACT_FAST_UP;
    if (g_cfg.key_scroll_down && vk == g_cfg.key_scroll_down)
        return ACT_FAST_DOWN;
    if (g_cfg.key_scroll_end && vk == g_cfg.key_scroll_end &&
        g_cfg.scroll_end_tap_ms > 0)
        return ACT_SCROLL_END;
    return ACT_NONE;
}

static WCHAR shortcut_char(UINT vk)
{
    if (vk >= 'A' && vk <= 'Z')
        return (WCHAR)(vk - 'A' + L'a');
    if (vk >= '0' && vk <= '9')
        return (WCHAR)vk;
    return 0;
}

static void shortcut_reset(void)
{
    s_sc_active = FALSE;
    s_sc_cancelled = FALSE;
    s_sc_len = 0;
    s_sc_buf[0] = 0;
}

// Alt + the first letter of an abbreviation starts a shortcut: every key
// until Alt is released goes into it.
static BOOL shortcut_begin(UINT vk)
{
    WCHAR ch = shortcut_char(vk);

    if (ch == 0 || shortcut_lookup(&ch, 1) == 0)
        return FALSE;
    shortcut_reset();
    s_sc_active = TRUE;
    s_sc_buf[0] = ch;
    s_sc_buf[1] = 0;
    s_sc_len = 1;
    return TRUE;
}

static void shortcut_key(UINT vk)
{
    WCHAR ch;

    if (vk == VK_BACK) {
        if (s_sc_len > 0)
            s_sc_buf[--s_sc_len] = 0;
        return;
    }
    ch = shortcut_char(vk);
    // Esc or any other key cancels; Alt is still swallowed until released.
    if (ch == 0 || s_sc_len >= SHORTCUT_ABBR_MAX) {
        s_sc_cancelled = TRUE;
        return;
    }
    s_sc_buf[s_sc_len++] = ch;
    s_sc_buf[s_sc_len] = 0;
}

// Alt released. The main thread types the text after this Alt-up has gone
// through, so the app does not see it as Alt + characters.
static void shortcut_end(void)
{
    WCHAR *text = NULL;
    HWND wnd = g_main_wnd;

    if (!s_sc_cancelled)
        text = shortcut_text_dup(s_sc_buf, s_sc_len);
    shortcut_reset();
    if (text && (!wnd || !PostMessageW(wnd, WM_APP_TYPE_TEXT, 0, (LPARAM)text)))
        free(text);
}

// Runs on the hook thread.
static void apply_locked(BOOL locked)
{
    LONG value = locked ? 1 : 0;

    if (InterlockedExchange(&s_locked, value) == value)
        return;
    if (value) {
        leave_mouse_mode();
        shortcut_reset();
        prescan_cancel();
        if (flag_get(&s_capture)) {
            flag_set(&s_capture, FALSE);
            post_main(WM_APP_HINT_CLOSE, 0, 0);
        }
    }
    app_log(L"input: keyboard %s", value ? L"locked" : L"unlocked");
    notify_state();
}

// While locked the swallowed modifiers never reach GetAsyncKeyState, so the
// hook's own key state says which are held.
static UINT locked_mods(void)
{
    UINT mods = 0;
    if (s_down[VK_LSHIFT] || s_down[VK_RSHIFT])
        mods |= KMOD_SHIFT;
    if (s_down[VK_LCONTROL] || s_down[VK_RCONTROL])
        mods |= KMOD_CTRL;
    if (s_down[VK_LMENU] || s_down[VK_RMENU])
        mods |= KMOD_ALT;
    if (s_down[VK_LWIN] || s_down[VK_RWIN])
        mods |= KMOD_WIN;
    return mods;
}

// Every key is swallowed except the lock hotkey (which unlocks). Keys that
// were already down when the lock started still get their release through.
static BOOL handle_locked(UINT vk, BOOL down, BOOL repeat)
{
    if (!down) {
        if (s_owner[vk] == OWN_SWALLOW) {
            s_owner[vk] = OWN_NONE;
            return TRUE;
        }
        return handle_up(vk);
    }
    if (repeat)
        return s_owner[vk] != OWN_PASS;
    s_owner[vk] = OWN_SWALLOW;
    if (!is_modifier(vk) && hotkey_match(&g_cfg.lock_hotkey, vk, locked_mods()))
        apply_locked(FALSE);
    return TRUE;
}

static void hint_hotkey_fire(void)
{
    // hint_open adopts the early scan instead of starting a new one.
    s_prepared = FALSE;
    if (!flag_get(&s_capture)) {
        flag_set(&s_capture, TRUE);
        post_main(WM_APP_HINT_OPEN, 0, 0);
    } else {
        post_main(WM_APP_HINT_AGAIN, 0, 0);
    }
}

static BOOL handle_hotkeys(UINT vk, UINT mods, BOOL send_mask)
{
    // Any key ends a pending pause tap; the pause hotkey itself reads it.
    BOOL pause_tap = s_pause_tap;

    s_pause_tap = FALSE;
    if (hotkey_match(&g_cfg.lock_hotkey, vk, mods)) {
        if (send_mask && (mods & (KMOD_ALT | KMOD_WIN)))
            send_mask_key();
        apply_locked(TRUE);
        return TRUE;
    }
    if (hotkey_match(&g_cfg.quit_hotkey, vk, mods)) {
        if (send_mask && (mods & (KMOD_ALT | KMOD_WIN)))
            send_mask_key();
        post_main(WM_APP_QUIT, 0, 0);
        return TRUE;
    }
    if (hotkey_match(&g_cfg.pause_hotkey, vk, mods)) {
        DWORD now = GetTickCount();

        if (send_mask && (mods & (KMOD_ALT | KMOD_WIN)))
            send_mask_key();
        // Alt+P P: the first press only arms the second (a held key's
        // repeats never get here), so a stray Alt+P does nothing. Both are
        // swallowed, like the other Alt keys of the program.
        if (g_cfg.pause_tap_ms <= 0 ||
            (pause_tap && (DWORD)(now - s_pause_tick) <=
                              (DWORD)g_cfg.pause_tap_ms)) {
            apply_paused(!flag_get(&s_paused));
        } else {
            s_pause_tap = TRUE;
            s_pause_tick = now;
        }
        return TRUE;
    }
    if (!flag_get(&s_paused) && hotkey_match(&g_cfg.hint_hotkey, vk, mods)) {
        if (send_mask && (mods & (KMOD_ALT | KMOD_WIN)))
            send_mask_key();
        hint_hotkey_fire();
        return TRUE;
    }
    return FALSE;
}

// Bare keys: outside text fields the Alt keys work without Alt. Only the
// hint hotkey's own key counts, and only when Alt is its sole modifier.
static BOOL bare_hint_key(UINT vk)
{
    return g_cfg.hint_hotkey.vk == vk && g_cfg.hint_hotkey.mods == KMOD_ALT;
}

// With log = 1, says why bare keys work or type as usual, once per change of
// reason, so an app that looks like a text field can be told apart.
static void bare_log(int kind, HWND popup)
{
    static int  last_kind = -1;
    static HWND last_popup = NULL;
    static HWND last_focus = NULL;
    GUITHREADINFO gti;
    HWND fg = GetForegroundWindow();
    HWND focus = fg;
    WCHAR cls[64], caret_cls[64];
    DWORD tid;

    ZeroMemory(&gti, sizeof(gti));
    gti.cbSize = sizeof(gti);
    tid = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    if (tid && GetGUIThreadInfo(tid, &gti) && gti.hwndFocus)
        focus = gti.hwndFocus;
    if (kind == last_kind && popup == last_popup && focus == last_focus)
        return;
    last_kind = kind;
    last_popup = popup;
    last_focus = focus;
    cls[0] = caret_cls[0] = 0;
    if (popup) {
        GetClassNameW(popup, cls, 64);
        app_log(L"bare keys: off, popup %p (%ls) is open", (void *)popup, cls);
        return;
    }
    if (focus)
        GetClassNameW(focus, cls, 64);
    if (gti.hwndCaret)
        GetClassNameW(gti.hwndCaret, caret_cls, 64);
    app_log(L"bare keys: %ls, focus %ls in %p (%ls), caret %p (%ls)%ls",
            kind == FOCUS_OTHER ? L"on" : L"off",
            kind == FOCUS_TEXT ? L"text field" :
            kind == FOCUS_OTHER ? L"no text field" : L"unknown",
            (void *)focus, cls, (void *)gti.hwndCaret, caret_cls,
            (gti.flags & GUI_CARETBLINKING) ? L" blinking" : L"");
}

// Set by bare_key_ok: the key types only because the focus is not known yet
// (still asking, or a classification that failed), not because it is in a
// text field or a menu is open. A lone g in that state still arms g g.
static BOOL s_bare_asked = FALSE;

// Checked only for an unmodified press of one of those keys. Anything not
// known to be outside a text field (still asking, a menu open) types as usual.
static BOOL bare_key_ok(UINT vk)
{
    HWND popup = NULL;
    int kind;

    s_bare_asked = FALSE;
    if (!g_cfg.bare_keys || flag_get(&s_mouse_mode))
        return FALSE;
    if (!bare_hint_key(vk) && alt_key_action(vk) == ACT_NONE)
        return FALSE;
    // No wait while a menu is open; otherwise give a pending answer a moment.
    if (popup_open(TRUE, &popup)) {
        if (g_cfg.log)
            bare_log(FOCUS_UNKNOWN, popup);
        return FALSE;
    }
    kind = uia_focus_wait(BARE_WAIT_MS);
    if (g_cfg.log)
        bare_log(kind, NULL);
    s_bare_asked = kind != FOCUS_OTHER && kind != FOCUS_TEXT;
    return kind == FOCUS_OTHER;
}

// key_center twice (0 0 within center_tap_ms, no modifiers) puts the
// pointer in the middle of the active window's screen, from the input thread
// once the hook has returned (both presses reach the app as usual). Outside
// mouse mode only where the focus is known not to be in a text field, like
// the bare keys, read when the first press arms it: that press asks the
// focus again (focus_key_passed), so the answer at the second is often still
// pending, and nothing here waits for it.
static void center_key_down(UINT vk, UINT mods, BOOL armed)
{
    DWORD now;
    BOOL ok;

    if (!g_cfg.key_center || vk != g_cfg.key_center || mods != 0 ||
        g_cfg.center_tap_ms <= 0)
        return;
    now = GetTickCount();
    if (armed && s_center_ok &&
        (DWORD)(now - s_center_tick) <= (DWORD)g_cfg.center_tap_ms) {
        ok = flag_get(&s_mouse_mode) ||
             (!popup_open(TRUE, NULL) && uia_focus_wait(0) != FOCUS_TEXT);
        if (ok && s_input_tid)
            PostThreadMessageW(s_input_tid, WM_INPUT_CENTER, 0, 0);
        return;
    }
    s_center_tap = TRUE;
    s_center_tick = now;
    s_center_ok = flag_get(&s_mouse_mode) ||
                  (!popup_open(TRUE, NULL) && uia_focus_wait(0) == FOCUS_OTHER);
}

// Tab, Enter, Esc and F6 move the focus by keyboard; the app may not report
// it, so ask again once it has.
static void focus_key_passed(UINT vk)
{
    if (g_cfg.bare_keys &&
        (vk == VK_TAB || vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_F6))
        uia_focus_changed(FOCUS_KEY_DELAY_MS);
}

// A key reached the app: typing and shortcuts open and move things, so what
// the hints scanned ahead (scan_ahead) is old. Once per burst; a post only,
// the hook must not wait.
static void ahead_key_passed(void)
{
    static DWORD last;
    DWORD now = GetTickCount();

    if (!g_cfg.scan_ahead || flag_get(&s_capture))
        return;
    if (last && now - last < AHEAD_FORGET_POST_MS)
        return;
    last = now ? now : 1;
    post_main(WM_APP_HINT_PREPARE, HINT_PREPARE_FORGET, 0);
}

static BOOL alt_hotkey_configured(void)
{
    return (g_cfg.hint_hotkey.mods & KMOD_ALT) ||
           (g_cfg.lock_hotkey.mods & KMOD_ALT) ||
           (g_cfg.quit_hotkey.mods & KMOD_ALT) ||
           (g_cfg.pause_hotkey.mods & KMOD_ALT);
}

static BOOL alt_should_withhold(void)
{
    if (!alt_hotkey_configured())
        return FALSE;
    // Capture can be on while input is paused (the tray opened the hints),
    // and the overlay still needs Alt hidden so its keys arrive intact.
    if (flag_get(&s_capture))
        return TRUE;
    return !flag_get(&s_paused) && popup_open(FALSE, NULL);
}

// The Alt scroll keys, from a normal Alt press and from a withheld Alt.
// alt_seen: the app has already seen Alt go down, so the 0xE8 mask keeps the
// Alt release from opening its menu bar; when it has not (bare keys, withheld
// Alt), no mask is needed and send_nav injects its own Alt.
// Returns FALSE for any other key, so the caller can continue.
static BOOL alt_action_down(UINT vk, int act, BOOL shift, BOOL end_tap,
                            BOOL alt_seen)
{
    if (act == ACT_BACK || act == ACT_FORWARD) {
        // One step per press: repeats are swallowed.
        mark_caps_used();
        s_owner[vk] = OWN_SWALLOW;
        send_nav(act == ACT_FORWARD, alt_seen);
        return TRUE;
    }
    if (act == ACT_SCROLL_END) {
        // Not held: repeats are swallowed and never make a g g.
        if (alt_seen)
            send_mask_key();
        mark_caps_used();
        s_owner[vk] = OWN_SWALLOW;
        scroll_end_press(shift, end_tap, g_cfg.alt_scroll_auto);
        return TRUE;
    }
    if (act != ACT_NONE) {
        if (alt_seen)
            send_mask_key();
        mark_caps_used();
        s_action[vk] = (unsigned char)act;
        s_owner[vk] = OWN_MOVE;
        scroll_press(act, g_cfg.alt_scroll_auto, FALSE);
        return TRUE;
    }
    return FALSE;
}

// The app never saw Alt, so hotkeys act without the 0xE8 menu mask.
static BOOL alt_resolve(UINT vk, UINT scan, DWORD flags)
{
    UINT mods = current_mods();
    UINT base;
    int act;
    BOOL shift;

    if (handle_hotkeys(vk, mods, FALSE)) {
        s_owner[vk] = OWN_SWALLOW;
        s_alt_pending = FALSE;
        s_alt_consumed = TRUE;
        return TRUE;
    }
    // Over the open hints, Shift/Ctrl still pass as usual (they pick the
    // click kind); only the Alt stays hidden.
    if (flag_get(&s_capture) && is_modifier(vk)) {
        s_owner[vk] = OWN_PASS;
        return FALSE;
    }
    if (flag_get(&s_capture)) {
        s_owner[vk] = OWN_SWALLOW;
        if (vk == VK_ESCAPE)
            flag_set(&s_capture, FALSE);
        // The hints used this Alt (Alt + label moves the pointer only), so
        // its release must not be replayed as a tap once they have closed.
        s_alt_consumed = TRUE;
        post_main(WM_APP_HINT_KEY, vk, mods);
        return TRUE;
    }
    // Chrome's downloads bubble, extensions menu and notification popups are
    // owned popups, so popup_open sees a menu and Alt is withheld. Replaying
    // Alt in front of the first key sent Chrome Alt+J rather than a scroll;
    // act on the Alt keys now, with Alt still hidden.
    if (!flag_get(&s_paused)) {
        act = alt_key_action(vk);
        shift = act == ACT_SCROLL_END && (mods & KMOD_SHIFT);
        base = shift ? mods & ~KMOD_SHIFT : mods;
        if (base == KMOD_ALT) {
            BOOL end_tap = s_end_tap;

            s_end_tap = FALSE;
            s_end_shift = FALSE;
            if (alt_action_down(vk, act, shift, end_tap, FALSE)) {
                s_alt_consumed = TRUE;
                return TRUE;
            }
        }
    }
    send_alt_replay(vk, scan, flags);
    ahead_key_passed();
    s_owner[vk] = OWN_PASS;
    s_owner[VK_LMENU] = OWN_PASS;
    s_alt_pending = FALSE;
    s_alt_consumed = FALSE;
    return TRUE;
}

static BOOL handle_down(UINT vk)
{
    UINT mods = current_mods();
    UINT base;
    int act;
    BOOL end_tap = s_end_tap;
    BOOL center_tap = s_center_tap;
    BOOL shift;
    BOOL bare;

    // Any key ends a pending g g (a second g is armed by end_tap) and a
    // pending 0 0, and drops a bottom jump still waiting for Shift;
    // modifiers never reach here.
    s_end_tap = FALSE;
    s_center_tap = FALSE;
    s_end_shift = FALSE;

    if (handle_hotkeys(vk, mods, TRUE)) {
        s_owner[vk] = OWN_SWALLOW;
        return TRUE;
    }
    // The hint overlay can be open while input is paused (from the tray); its
    // keys must still reach it, including Esc to close it.
    if (flag_get(&s_capture)) {
        s_owner[vk] = OWN_SWALLOW;
        // Esc drops capture right here too, so typing comes back even if
        // the UI thread never processes the close.
        if (vk == VK_ESCAPE)
            flag_set(&s_capture, FALSE);
        // An Alt the app has seen (the right Alt, or no Alt hotkey to
        // withhold for): the mask keeps its release from opening a menu bar.
        if (mods & (KMOD_ALT | KMOD_WIN))
            send_mask_key();
        post_main(WM_APP_HINT_KEY, vk, mods);
        return TRUE;
    }
    if (flag_get(&s_paused)) {
        s_owner[vk] = OWN_PASS;
        return FALSE;
    }
    // Shift goes along only with the scroll-end key (Shift + g: bottom); the
    // other Alt keys take Alt alone.
    act = alt_key_action(vk);
    shift = act == ACT_SCROLL_END && (mods & KMOD_SHIFT);
    base = shift ? mods & ~KMOD_SHIFT : mods;
    // Outside text fields the Alt keys below work without Alt.
    bare = base == 0 && bare_key_ok(vk);
    if (bare && bare_hint_key(vk)) {
        s_owner[vk] = OWN_SWALLOW;
        hint_hotkey_fire();
        return TRUE;
    }
    // Alt + letter reaches the app with Alt already down; the mask key keeps
    // the Alt release from opening its menu bar.
    if (base == KMOD_ALT || bare) {
        if (alt_action_down(vk, act, shift, end_tap, !bare))
            return TRUE;
        if (!bare && shortcut_begin(vk)) {
            send_mask_key();
            mark_caps_used();
            s_owner[vk] = OWN_SWALLOW;
            return TRUE;
        }
    }
    if (g_cfg.mouse_mode_key != 0 && vk == g_cfg.mouse_mode_key &&
        !(mods & (KMOD_CTRL | KMOD_ALT | KMOD_WIN | KMOD_SHIFT))) {
        s_owner[vk] = OWN_CAPS;
        caps_key_down();
        return TRUE;
    }
    // capslock_off: a CapsLock press that is not the mouse-mode key (with
    // Shift, Ctrl or Alt, or with another mouse_mode_key) would turn the
    // CapsLock state on; it does nothing instead. caps_check catches the
    // presses this hook never sees.
    if (vk == VK_CAPITAL && g_cfg.capslock_off) {
        s_owner[vk] = OWN_SWALLOW;
        return TRUE;
    }
    if (flag_get(&s_mouse_mode)) {
        if (mods & (KMOD_CTRL | KMOD_ALT | KMOD_WIN)) {
            s_owner[vk] = OWN_PASS;
            ahead_key_passed();
            return FALSE;
        }
        if (vk == VK_ESCAPE) {
            s_owner[vk] = OWN_SWALLOW;
            mark_caps_used();
            leave_mouse_mode();
            return TRUE;
        }
        act = binding_action(vk);
        if (act == ACT_SCROLL_END) {
            // Like the other mouse-mode scroll keys, under the cursor.
            mark_caps_used();
            s_owner[vk] = OWN_SWALLOW;
            scroll_end_press((mods & KMOD_SHIFT) != 0, end_tap, FALSE);
            return TRUE;
        }
        if (act != ACT_NONE) {
            mark_caps_used();
            s_action[vk] = (unsigned char)act;
            if (act >= ACT_MOVE_LEFT && act <= ACT_MOVE_RIGHT)
                s_owner[vk] = OWN_MOVE;
            else if (act >= ACT_SCROLL_UP && act <= ACT_SCROLL_RIGHT)
                s_owner[vk] = OWN_MOVE;
            else if (act == ACT_LEFT)
                s_owner[vk] = OWN_LEFT;
            else
                s_owner[vk] = OWN_SWALLOW;
            apply_action_down(act);
            return TRUE;
        }
        if (is_typing_key(vk)) {
            // A typing key as key_center still counts, swallowed or not.
            center_key_down(vk, mods, center_tap);
            s_owner[vk] = OWN_SWALLOW;
            mark_caps_used();
            return TRUE;
        }
    }
    // A lone g types here when the focus is not known yet (right after a
    // click, a page still loading). It is still the first of a g g: the
    // second g jumps if by then the answer says the focus is outside a text
    // field, where the typed g did nothing, and types as usual otherwise.
    // Without this, g g right after a click needed g g g.
    if (act == ACT_SCROLL_END && mods == 0 && s_bare_asked) {
        s_end_tap = TRUE;
        s_end_tap_tick = GetTickCount();
        if (g_cfg.log)
            app_log(L"g g: the first g typed while the focus was being "
                    L"asked for; a second g still goes to the top");
    }
    center_key_down(vk, mods, center_tap);
    focus_key_passed(vk);
    ahead_key_passed();
    s_owner[vk] = OWN_PASS;
    return FALSE;
}

static BOOL handle_up(UINT vk)
{
    int owner;
    int act;

    if (is_modifier(vk))
        prescan_cancel();
    owner = s_owner[vk];
    act = s_action[vk];
    s_owner[vk] = OWN_NONE;
    s_action[vk] = ACT_NONE;
    switch (owner) {
    case OWN_PASS:
        return FALSE;
    case OWN_CAPS:
        caps_key_up();
        return TRUE;
    case OWN_LEFT:
        left_release_key();
        return TRUE;
    case OWN_MOVE:
        if (act >= ACT_MOVE_LEFT && act <= ACT_MOVE_RIGHT)
            move_release(act);
        else if (act >= ACT_SCROLL_UP && act <= ACT_FAST_DOWN)
            scroll_release(act);
        return TRUE;
    case OWN_SWALLOW:
        return TRUE;
    case OWN_ALT_HELD:
        if (s_alt_pending && !s_alt_consumed && !flag_get(&s_capture))
            send_alt_tap();
        s_alt_pending = FALSE;
        s_alt_consumed = FALSE;
        return TRUE;
    default:
        return FALSE;
    }
}

// ---------------------------------------------------------------------------
// capslock_off: keep the CapsLock state off.
//
// The hook swallows CapsLock presses (mouse mode, and every other one in
// handle_down), yet the state still comes on: a window running as
// administrator and the lock screen get keys this hook never sees, it may be
// on when the program starts, and CapsLock passes through while paused. The
// input thread reads the state and taps CapsLock back off with SendInput.
// GetKeyState is current here: Windows resyncs the key state of a thread that
// receives no keyboard input when it asks. The tap is injected, so the hook
// lets it through untouched.

static BOOL caps_watch_key(UINT vk)
{
    switch (vk) {
    case VK_CAPITAL:
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
        return TRUE;
    default:
        return FALSE;
    }
}

// From the hook (a post, never a wait): check right after this key comes up
// instead of at the next tick.
static void caps_check_soon(UINT vk)
{
    if (g_cfg.capslock_off && s_power_wnd)
        PostMessageW(s_power_wnd, WM_INPUT_CAPS_CHECK, (WPARAM)vk, 0);
}

// Windows' "To turn off Caps Lock: press the SHIFT key" setting. The CapsLock
// key cannot turn it off then, and we never inject Shift.
static BOOL caps_shift_lock(void)
{
    HKEY key;
    DWORD value = 0, size = sizeof(value), type = 0;
    BOOL on = FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Keyboard Layout", 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return FALSE;
    if (RegQueryValueExW(key, L"Attributes", NULL, &type, (BYTE *)&value,
                         &size) == ERROR_SUCCESS && type == REG_DWORD)
        on = (value & KLF_SHIFTLOCK) != 0;
    RegCloseKey(key);
    return on;
}

// A modifier held now would turn the tap into Shift, Ctrl or Alt + CapsLock,
// which some IMEs act on; wait for it to come up (the next check is at most
// CAPS_CHECK_MS away). The key whose release asked for this check can still
// read as down: Windows updates it after the hook returns.
static BOOL caps_mods_held(UINT released)
{
    static const UINT keys[] = {
        VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU,
        VK_LWIN, VK_RWIN, VK_CAPITAL
    };
    int i;

    for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++) {
        if (keys[i] == released)
            continue;
        if (GetAsyncKeyState((int)keys[i]) & 0x8000)
            return TRUE;
    }
    return FALSE;
}

static void caps_send_tap(void)
{
    INPUT in[2];

    ZeroMemory(in, sizeof(in));
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CAPITAL;
    in[0].ki.wScan = (WORD)MapVirtualKeyW(VK_CAPITAL, MAPVK_VK_TO_VSC);
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(2, in, sizeof(INPUT)) != 2)
        app_log(L"input: turning CapsLock off failed (%lu)", GetLastError());
}

// Input thread only (the power window's timer and WM_INPUT_CAPS_CHECK).
static void caps_check(UINT released)
{
    DWORD now;
    HWND fg;

    // Paused means hands off; suspended (no hook) is the lock screen, where
    // a tap cannot reach and the state is checked again after unlocking.
    if (!g_cfg.capslock_off || flag_get(&s_paused) || !s_hook)
        return;
    if (!(GetKeyState(VK_CAPITAL) & 1)) {
        s_caps_fix_tick = 0;
        s_caps_fix_fails = 0;
        return;
    }
    if (caps_mods_held(released))
        return;
    now = GetTickCount();
    fg = GetForegroundWindow();
    if (fg != s_caps_fix_fg) {
        // Another window may take the tap an administrator window dropped.
        s_caps_fix_fg = fg;
        s_caps_fix_tick = 0;
        s_caps_fix_fails = 0;
    }
    if (s_caps_fix_tick != 0) {
        // Still on after a tap: Windows dropped it (an administrator window
        // has the focus). Try again less and less often.
        int shift = s_caps_fix_fails < 4 ? s_caps_fix_fails : 4;
        if (now - s_caps_fix_tick < ((DWORD)CAPS_RETRY_MS << shift))
            return;
        if (s_caps_fix_fails < 4)
            ++s_caps_fix_fails;
    }
    if (caps_shift_lock()) {
        if (!s_caps_shiftlock_logged) {
            s_caps_shiftlock_logged = TRUE;
            app_log(L"input: CapsLock is on, but Windows turns it off with "
                    L"Shift here, so capslock_off cannot");
        }
        s_caps_fix_tick = now ? now : 1;
        return;
    }
    if (s_caps_fix_tick == 0)
        app_log(L"input: CapsLock was on; turning it off (capslock_off)");
    caps_send_tap();
    s_caps_fix_tick = now ? now : 1;
}

// Input thread: the check timer runs while capslock_off is on.
static void caps_timer_update(void)
{
    BOOL want = g_cfg.capslock_off && s_power_wnd != NULL;

    if (want && !s_caps_timer_on) {
        s_caps_timer_on =
            SetTimer(s_power_wnd, CAPS_TIMER, CAPS_CHECK_MS, NULL) != 0;
        caps_check(0);
    } else if (!want && s_caps_timer_on) {
        KillTimer(s_power_wnd, CAPS_TIMER);
        s_caps_timer_on = FALSE;
    }
}

static BOOL handle_key(UINT vk, BOOL down, DWORD flags, UINT scan)
{
    int owner;

    if (vk > 255)
        return FALSE;
    if (flags & LLKHF_INJECTED)
        return FALSE;
    if (!down && caps_watch_key(vk))
        caps_check_soon(vk);
    if (flag_get(&s_locked)) {
        BOOL repeat = down && s_down[vk] != 0;
        if (!down && !s_down[vk])
            return TRUE;
        s_down[vk] = down ? 1 : 0;
        return handle_locked(vk, down, repeat);
    }
    if (down) {
        BOOL repeat = s_down[vk] != 0;
        s_down[vk] = 1;
        if (!repeat) {
            if (is_modifier(vk))
                prescan_mod_down(vk);
            else if (vk != g_cfg.hint_hotkey.vk)
                prescan_cancel();
        }
        if (vk == VK_LMENU) {
            if (repeat) {
                if (s_owner[vk] == OWN_ALT_HELD)
                    return TRUE;
            } else if (alt_should_withhold()) {
                s_owner[vk] = OWN_ALT_HELD;
                s_alt_pending = TRUE;
                s_alt_consumed = FALSE;
                s_alt_scan = scan;
                s_alt_extended = (flags & LLKHF_EXTENDED) ? TRUE : FALSE;
                return TRUE;
            }
            s_owner[vk] = OWN_PASS;
            return FALSE;
        }
        // Resolve a withheld Alt before the modifier pass-through rule, so
        // Alt+Shift and friends are replayed too. This also covers keys
        // pressed after Alt+hotkey while Alt is still held: once the hints
        // have closed, they reach the app with their Alt replayed.
        if (s_owner[VK_LMENU] == OWN_ALT_HELD && !repeat)
            return alt_resolve(vk, scan, flags);
        if (is_modifier(vk)) {
            s_owner[vk] = OWN_PASS;
            return FALSE;
        }
        if (s_sc_active && !repeat) {
            // Ctrl/Win combinations (Ctrl+Alt+Q) drop the shortcut and act.
            if (current_mods() & (KMOD_CTRL | KMOD_WIN)) {
                shortcut_reset();
            } else {
                shortcut_key(vk);
                s_owner[vk] = OWN_SWALLOW;
                return TRUE;
            }
        }
        if (repeat) {
            owner = s_owner[vk];
            return owner != OWN_PASS && owner != OWN_NONE;
        }
        return handle_down(vk);
    }
    if (!s_down[vk])
        return FALSE;
    s_down[vk] = 0;
    // The app gets this Shift up before the wheel events queued after it, so
    // the fallback bottom jump can go out now where UI Automation could not
    // scroll.
    if (vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_SHIFT)
        scroll_end_shift_up();
    if (s_sc_active && (vk == VK_LMENU || vk == VK_RMENU))
        shortcut_end();
    return handle_up(vk);
}

static LRESULT CALLBACK kb_proc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lParam;

        InterlockedIncrement(&s_kb_calls);
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            if (handle_key(k->vkCode, TRUE, k->flags, k->scanCode))
                return 1;
        } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            if (handle_key(k->vkCode, FALSE, k->flags, k->scanCode))
                return 1;
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static void reset_key_state(void)
{
    prescan_cancel();
    ZeroMemory(s_down, sizeof(s_down));
    ZeroMemory(s_owner, sizeof(s_owner));
    ZeroMemory(s_action, sizeof(s_action));
    s_caps_held = 0;
    s_caps_used = 0;
    s_caps_enter = 0;
    s_caps_exit_on_release = 0;
    s_alt_pending = FALSE;
    s_alt_consumed = FALSE;
    s_alt_scan = 0;
    s_alt_extended = FALSE;
    s_end_shift = FALSE;
    s_center_tap = FALSE;
    s_pause_tap = FALSE;
    InterlockedExchange(&s_end_uia, END_UIA_IDLE);
    shortcut_reset();
}

static BOOL install_hook(void)
{
    if (s_hook)
        return TRUE;
    s_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_proc, GetModuleHandleW(NULL), 0);
    return s_hook != NULL;
}

static void uninstall_hook(void)
{
    if (s_hook) {
        UnhookWindowsHookEx(s_hook);
        s_hook = NULL;
    }
}

// Install the replacement before removing the old handle, with no message
// pumping in between, so there is no gap without a hook and no double delivery.
static void replace_hook(void)
{
    DWORD now = GetTickCount();
    HHOOK hook;

    if (s_hook_replace_tick != 0 &&
        (DWORD)(now - s_hook_replace_tick) < 1000)
        return;
    hook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_proc, GetModuleHandleW(NULL), 0);
    if (!hook)
        return;
    UnhookWindowsHookEx(s_hook);
    s_hook = hook;
    s_hook_replace_tick = now;
    s_hook_misses = 0;
    app_log(L"input: keyboard hook was removed by Windows; reinstalled");
}

static void CALLBACK focus_event_proc(HWINEVENTHOOK hook, DWORD event,
                                      HWND hwnd, LONG id_object,
                                      LONG id_child, DWORD thread, DWORD time)
{
    DWORD fg_pid = 0, pid = 0;
    HWND fg;

    (void)hook;
    (void)id_object;
    (void)id_child;
    (void)thread;
    (void)time;
    // Background apps and IME windows report focus too; only the foreground
    // window's focus changes concern it.
    if (event == EVENT_OBJECT_FOCUS && hwnd) {
        fg = GetForegroundWindow();
        if (fg) {
            GetWindowThreadProcessId(fg, &fg_pid);
            GetWindowThreadProcessId(hwnd, &pid);
        }
        if (!fg || (GetAncestor(hwnd, GA_ROOT) != fg &&
                    (!pid || pid != fg_pid)))
            return;
    }
    uia_focus_changed(0);
}

// Bare keys hear about every focus and foreground change; without bare keys
// nothing is hooked and nothing is asked.
static void focus_hooks_update(void)
{
    static const DWORD events[2] = { EVENT_SYSTEM_FOREGROUND, EVENT_OBJECT_FOCUS };
    int i;

    for (i = 0; i < 2; i++) {
        if (g_cfg.bare_keys && !s_focus_hooks[i]) {
            s_focus_hooks[i] = SetWinEventHook(
                events[i], events[i], NULL, focus_event_proc, 0, 0,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            if (!s_focus_hooks[i])
                app_log(L"input: focus event hook failed (%lu)", GetLastError());
        } else if (!g_cfg.bare_keys && s_focus_hooks[i]) {
            UnhookWinEvent(s_focus_hooks[i]);
            s_focus_hooks[i] = NULL;
        }
    }
    if (g_cfg.bare_keys)
        uia_focus_changed(0);
}

// ---------------------------------------------------------------------------
// hide_pointer

// The real mouse shows the pointer again. Only the real one: moves and
// clicks of ours carry LLMHF_INJECTED (SendInput), and a move of ours
// through SetCursorPos lands exactly where own_move said. Runs on the input
// thread for every mouse event, so it only notes the time and posts.
static LRESULT CALLBACK mouse_proc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        const MSLLHOOKSTRUCT *m = (const MSLLHOOKSTRUCT *)lParam;
        if (!(m->flags & LLMHF_INJECTED) &&
            !(wParam == WM_MOUSEMOVE &&
              m->pt.x == InterlockedCompareExchange(&s_own_x, 0, 0) &&
              m->pt.y == InterlockedCompareExchange(&s_own_y, 0, 0))) {
            InterlockedExchange(&s_ptr_phys_tick, (LONG)m->time);
            if (flag_get(&s_ptr_want))
                ptr_schedule();
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// The mouse hook exists while the pointer can be hidden.
static void ptr_hook_update(void)
{
    BOOL want = g_cfg.hide_pointer > 0 && !s_ptr_away;

    if (want && !s_mouse_hook) {
        s_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc,
                                         GetModuleHandleW(NULL), 0);
        if (!s_mouse_hook)
            app_log(L"input: mouse hook install failed");
    } else if (!want && s_mouse_hook) {
        UnhookWindowsHookEx(s_mouse_hook);
        s_mouse_hook = NULL;
    }
}

// Every system cursor becomes a blank one. SetSystemCursor takes the cursor
// over (and frees it), so each id gets a fresh one. Session-wide, like
// SPI_SETCURSORS, which brings the user's scheme back from the registry:
// the pointer thread restores it before it ends, and a crash leaves the
// pointer blank until vim mouse runs again or the user signs in again.
static void ptr_cursors_blank(void)
{
    static const DWORD ids[] = {
        OCR_NORMAL, OCR_IBEAM, OCR_WAIT, OCR_CROSS, OCR_UP, OCR_SIZENWSE,
        OCR_SIZENESW, OCR_SIZEWE, OCR_SIZENS, OCR_SIZEALL, OCR_NO, OCR_HAND,
        OCR_APPSTARTING, OCR_HELP, OCR_PIN, OCR_PERSON
    };
    BYTE and_mask[128];
    BYTE xor_mask[128];
    size_t i;

    FillMemory(and_mask, sizeof(and_mask), 0xFF);
    ZeroMemory(xor_mask, sizeof(xor_mask));
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        HCURSOR cur = CreateCursor(GetModuleHandleW(NULL), 0, 0, 32, 32,
                                   and_mask, xor_mask);
        if (cur && !SetSystemCursor(cur, ids[i]))
            DestroyCursor(cur);
    }
}

static void ptr_cursors_restore(void)
{
    SystemParametersInfoW(SPI_SETCURSORS, 0, NULL, 0);
}

static DWORD WINAPI ptr_thread_proc(LPVOID param)
{
    (void)param;
    for (;;) {
        BOOL want;

        if (WaitForSingleObject(s_ptr_event, INFINITE) != WAIT_OBJECT_0)
            break;
        if (flag_get(&s_ptr_exit))
            break;
        want = flag_get(&s_ptr_want);
        if (want == flag_get(&s_ptr_hidden))
            continue;
        if (want)
            ptr_cursors_blank();
        else
            ptr_cursors_restore();
        flag_set(&s_ptr_hidden, want);
    }
    if (flag_get(&s_ptr_hidden)) {
        ptr_cursors_restore();
        flag_set(&s_ptr_hidden, FALSE);
    }
    return 0;
}

static void ptr_thread_start(void)
{
    if (s_ptr_thread)
        return;
    s_ptr_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!s_ptr_event)
        return;
    flag_set(&s_ptr_exit, FALSE);
    s_ptr_thread = CreateThread(NULL, 0, ptr_thread_proc, NULL, 0, NULL);
    if (!s_ptr_thread) {
        CloseHandle(s_ptr_event);
        s_ptr_event = NULL;
    }
}

// Ends the pointer thread, which puts the user's cursors back first.
static void ptr_thread_stop(void)
{
    if (s_ptr_thread) {
        flag_set(&s_ptr_exit, TRUE);
        SetEvent(s_ptr_event);
        WaitForSingleObject(s_ptr_thread, 3000);
        CloseHandle(s_ptr_thread);
        s_ptr_thread = NULL;
    }
    if (s_ptr_event) {
        CloseHandle(s_ptr_event);
        s_ptr_event = NULL;
    }
}

// Decides on the input thread whether the pointer is hidden now: never in
// mouse mode, while paused, while the session is locked or asleep, or with
// hide_pointer off; otherwise once the real mouse has been still for
// hide_pointer ms (the power window's timer calls back when it has been).
static void ptr_update(void)
{
    BOOL want = FALSE;
    DWORD arm_ms = 0;

    InterlockedExchange(&s_ptr_posted, 0);
    if (s_ptr_thread && g_cfg.hide_pointer > 0 && !s_ptr_away &&
        !flag_get(&s_paused) && !flag_get(&s_mouse_mode)) {
        DWORD last = (DWORD)InterlockedCompareExchange(&s_ptr_phys_tick, 0, 0);
        DWORD idle = GetTickCount() - last;

        if (idle >= (DWORD)g_cfg.hide_pointer)
            want = TRUE;
        else
            arm_ms = (DWORD)g_cfg.hide_pointer - idle + 1;
    }
    if (arm_ms && s_power_wnd) {
        SetTimer(s_power_wnd, PTR_TIMER, arm_ms, NULL);
        s_ptr_timer_on = TRUE;
    } else if (s_ptr_timer_on) {
        KillTimer(s_power_wnd, PTR_TIMER);
        s_ptr_timer_on = FALSE;
    }
    flag_set(&s_ptr_want, want);
    if (s_ptr_event)
        SetEvent(s_ptr_event);
}

// Asks the input thread for a new decision; one post in flight at a time.
// Called from the hooks and from the UI thread, so it never swaps cursors
// itself.
static void ptr_schedule(void)
{
    if (!s_input_tid || !s_ptr_thread)
        return;
    if (InterlockedCompareExchange(&s_ptr_posted, 1, 0) == 0)
        PostThreadMessageW(s_input_tid, WM_INPUT_PTR_UPDATE, 0, 0);
}

// WM_INPUT_CENTER, on the input thread: the pointer goes to the middle of
// the active window's monitor (the pointer's own when there is none). A move
// of ours, so the mouse hook ignores it; outside mouse mode the pointer is
// shown there as the real mouse would show it, and hides again after
// hide_pointer ms. In mouse mode the pill follows.
static void center_pointer(void)
{
    HWND fg = GetForegroundWindow();
    HMONITOR mon = NULL;
    MONITORINFO mi;
    POINT pt;

    if (fg)
        mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    if (!mon && GetCursorPos(&pt))
        mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!mon)
        return;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi))
        return;
    pt.x = (mi.rcMonitor.left + mi.rcMonitor.right) / 2;
    pt.y = (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
    own_move(pt);
    indicator_moved();
    InterlockedExchange(&s_ptr_phys_tick, (LONG)GetTickCount());
    ptr_update();
    if (g_cfg.log)
        app_log(L"center: pointer to %ld,%ld", pt.x, pt.y);
}

// The keyboard is in use: the real mouse's last event counts as long ago,
// so the next decision hides the pointer at once.
static void ptr_forget_mouse(void)
{
    InterlockedExchange(&s_ptr_phys_tick,
                        (LONG)(GetTickCount() - PTR_IDLE_MAX_MS - 1));
}

static void suspend_input(void)
{
    leave_mouse_mode();
    reset_key_state();
    uninstall_hook();
    // The lock screen and the next session get the user's own pointer.
    s_ptr_away = TRUE;
    ptr_hook_update();
    ptr_update();
}

static void resume_input(void)
{
    reset_key_state();
    uninstall_hook();
    if (!install_hook())
        app_log(L"input: keyboard hook reinstall failed");
    s_ptr_away = FALSE;
    ptr_hook_update();
    ptr_update();
}

static int round_i(double value)
{
    return (int)floor(value + 0.5);
}

// The direction's base speed from the press on; ramps toward its maximum from
// SCROLL_HOLD_DELAY_MS on, with the same curve as the original formula.
static double scroll_speed_for(double elapsed_ms, double base, double maximum,
                               double accel_time, double curve)
{
    double t;
    if (elapsed_ms < SCROLL_HOLD_DELAY_MS)
        return base;
    t = (elapsed_ms - SCROLL_HOLD_DELAY_MS) / 1000.0 / accel_time;
    if (t < 0.0)
        t = 0.0;
    if (t > 1.0)
        t = 1.0;
    return base + (maximum - base) * pow(t, curve);
}

typedef HANDLE (WINAPI *CreateWaitableTimerExWFn)(LPSECURITY_ATTRIBUTES,
                                                  LPCWSTR, DWORD, DWORD);

static HANDLE create_move_timer(void)
{
    CreateWaitableTimerExWFn create;

    create = (CreateWaitableTimerExWFn)(void *)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "CreateWaitableTimerExW");
    if (!create)
        return NULL;
    return create(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                  TIMER_ALL_ACCESS);
}

static void wait_tick(HANDLE timer)
{
    LARGE_INTEGER due;

    if (timer) {
        due.QuadPart = -10000;  // relative: 1 ms
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep(1);
}

// Alt went down (prescan_mod_down): start caching the scrollable areas of
// the window under the pointer.
static void scroll_prefetch_now(void)
{
    POINT pt;

    if (InterlockedExchange(&s_scroll_prefetch, 0) && GetCursorPos(&pt))
        uia_scroll_prefetch(pt);
}

// dwmapi.h is #pragma pack(1), and DWM refuses the call unless cbSize is the
// exact size of that layout, so mirror the full field list here.
#pragma pack(push, 1)
typedef struct {
    UINT32 cbSize;
    struct {
        UINT32 uiNumerator;
        UINT32 uiDenominator;
    } rateRefresh;
    ULONGLONG qpcRefreshPeriod;
    struct {
        UINT32 uiNumerator;
        UINT32 uiDenominator;
    } rateCompose;
    ULONGLONG qpcVBlank;
    ULONGLONG cRefresh;
    UINT32 cDXRefresh;
    ULONGLONG qpcCompose;
    ULONGLONG cFrame;
    UINT32 cDXPresent;
    ULONGLONG cRefreshFrame;
    ULONGLONG cFrameSubmitted;
    UINT32 cDXPresentSubmitted;
    ULONGLONG cFrameConfirmed;
    UINT32 cDXPresentConfirmed;
    ULONGLONG cRefreshConfirmed;
    UINT32 cDXRefreshConfirmed;
    ULONGLONG cFramesLate;
    UINT32 cFramesOutstanding;
    ULONGLONG cFrameDisplayed;
    ULONGLONG qpcFrameDisplayed;
    ULONGLONG cRefreshFrameDisplayed;
    ULONGLONG cFrameComplete;
    ULONGLONG qpcFrameComplete;
    ULONGLONG cFramePending;
    ULONGLONG qpcFramePending;
    ULONGLONG cFramesDisplayed;
    ULONGLONG cFramesComplete;
    ULONGLONG cFramesPending;
    ULONGLONG cFramesAvailable;
    ULONGLONG cFramesDropped;
    ULONGLONG cFramesMissed;
    ULONGLONG cRefreshNextDisplayed;
    ULONGLONG cRefreshNextPresented;
    ULONGLONG cRefreshesDisplayed;
    ULONGLONG cRefreshesPresented;
    ULONGLONG cRefreshStarted;
    ULONGLONG cPixelsReceived;
    ULONGLONG cPixelsDrawn;
    ULONGLONG cBuffersEmpty;
} ScrollTimingInfo;
#pragma pack(pop)

typedef struct {
    LONGLONG period;    // QPC units per display frame
    LONGLONG anchor;    // QPC time of a frame boundary
    DWORD refreshed_us;
    BOOL valid;
} ScrollFrameClock;

static void scroll_clock_refresh(ScrollFrameClock *clock, LARGE_INTEGER now,
                                 LARGE_INTEGER freq, DWORD now_us)
{
    typedef HRESULT (WINAPI *TimingFn)(HWND, ScrollTimingInfo *);
    static BOOL tried = FALSE;
    static TimingFn timing_fn = NULL;
    ScrollTimingInfo info;
    LONGLONG period = 0;

    if (!tried) {
        HMODULE dwm;

        tried = TRUE;
        dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm)
            timing_fn = (TimingFn)(void *)GetProcAddress(
                dwm, "DwmGetCompositionTimingInfo");
    }
    if (timing_fn) {
        ZeroMemory(&info, sizeof(info));
        info.cbSize = sizeof(info);
        if (timing_fn(NULL, &info) == S_OK &&
            info.qpcRefreshPeriod >= (ULONGLONG)freq.QuadPart / 1000 &&
            info.qpcRefreshPeriod < (ULONGLONG)freq.QuadPart &&
            info.qpcVBlank != 0) {
            period = (LONGLONG)info.qpcRefreshPeriod;
            clock->anchor = (LONGLONG)info.qpcVBlank;
        }
    }
    if (period <= 0) {
        // No compositor timing: assume 60 Hz from this burst on.
        period = freq.QuadPart / 60;
        if (period <= 0)
            period = 1;
        clock->anchor = now.QuadPart;
    }
    clock->period = period;
    clock->refreshed_us = now_us;
    clock->valid = TRUE;
}

static LONGLONG scroll_frame_index(const ScrollFrameClock *clock,
                                   LARGE_INTEGER now)
{
    LONGLONG diff;

    if (!clock->valid || clock->period <= 0)
        return 0;
    diff = now.QuadPart - clock->anchor;
    if (diff >= 0)
        return diff / clock->period;
    return -(((-diff) + clock->period - 1) / clock->period);
}

// A press the movement thread has not started yet still needs its stream,
// even if the key was already released (a tap).
static BOOL scroll_press_pending(const LONG *seen)
{
    int idx;

    for (idx = 0; idx < 6; idx++) {
        if (InterlockedCompareExchange(&s_scroll_gen[idx], 0, 0) != seen[idx])
            return TRUE;
    }
    return FALSE;
}

static BOOL scroll_active_any(const unsigned char *active)
{
    int idx;

    for (idx = 0; idx < 6; idx++) {
        if (active[idx])
            return TRUE;
    }
    return FALSE;
}

// The stream stops as soon as the key is released and its minimum is met, but
// distance already accrued still has to go out on the next frame.
static BOOL scroll_flush_pending(double accX, double accY)
{
    if (g_cfg.smooth_scroll)
        return accX >= 1.0 - SCROLL_EPS || accX <= -1.0 + SCROLL_EPS ||
               accY >= 1.0 - SCROLL_EPS || accY <= -1.0 + SCROLL_EPS;
    return accX >= 120.0 - SCROLL_EPS || accX <= -120.0 + SCROLL_EPS ||
           accY >= 120.0 - SCROLL_EPS || accY <= -120.0 + SCROLL_EPS;
}

static DWORD WINAPI movement_thread(LPVOID param)
{
    LARGE_INTEGER freq, prev, now;
    double velX = 0.0, velY = 0.0;
    double holdX = 0.0, holdY = 0.0;
    double scrAccX = 0.0, scrAccY = 0.0;
    double scrFed[6] = { 0 };
    double scrMin[6] = { 0 };
    double scrJumpDelivered[6] = { 0 };
    DWORD scrStartUs[6] = { 0 };
    DWORD scrLastUs[6] = { 0 };
    DWORD scrJumpStartUs[6] = { 0 };
    LONG scrSeenGen[6] = { 0 };
    unsigned char scrActive[6] = { 0 };
    unsigned char scrJump[6] = { 0 };
    BOOL scrAimX = FALSE, scrAimY = FALSE;
    BOOL scrFrameValid = FALSE;
    LONGLONG scrFrame = 0;
    ScrollFrameClock clock;
    double posX = 0.0, posY = 0.0;
    POINT ref = { 0, 0 };
    BOOL have_ref = FALSE;
    int prevSignX = 0, prevSignY = 0;
    BOOL timing = FALSE;
    LONG seen_gen = 0;
    POINT target = { 0, 0 };
    BOOL target_ok = FALSE;
    // The lookup of the last Alt press: where it was made, which way, when,
    // and whether it is still being answered. scrOwed is what each stream
    // sent to the pointer while asking.
    BOOL aim_asking = FALSE;
    POINT aim_pt = { 0, 0 };
    BOOL aim_vertical = TRUE;
    DWORD aim_start_us = 0;
    double scrOwed[6] = { 0 };
    DWORD last_prefetch = 0;
    HANDLE timer;

    (void)param;
    // Keep the ticks steady while other programs load the CPU.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    clock.period = 0;
    clock.anchor = 0;
    clock.refreshed_us = 0;
    clock.valid = FALSE;

    // A high-resolution waitable timer ticks at ~1.7 ms on Windows 11, where
    // Sleep(1) takes ~6 ms even with timeBeginPeriod(1). Keep the old timer
    // resolution trick only as a fallback, and only while the movement loop
    // is actually running so the system-wide 1 ms tick is not held while idle.
    timer = create_move_timer();

    for (;;) {
        if (flag_get(&s_move_exit))
            break;
        scroll_prefetch_now();
        if (!flag_get(&s_move_flags) && !flag_get(&s_scroll_flags) &&
            !flag_get(&s_scroll_pending) &&
            !scroll_active_any(scrActive) &&
            !scroll_press_pending(scrSeenGen)) {
            WaitForSingleObject(s_move_event, INFINITE);
            if (flag_get(&s_move_exit))
                break;
            scroll_prefetch_now();
            if (!flag_get(&s_move_flags) && !flag_get(&s_scroll_flags) &&
                !flag_get(&s_scroll_pending) &&
                !scroll_active_any(scrActive) &&
                !scroll_press_pending(scrSeenGen))
                continue;
        }

        velX = velY = 0.0;
        holdX = holdY = 0.0;
        scrAccX = scrAccY = 0.0;
        scrAimX = scrAimY = FALSE;
        scrFrameValid = FALSE;
        prevSignX = prevSignY = 0;
        have_ref = FALSE;
        QueryPerformanceCounter(&prev);

        if (!timer && !timing) {
            timeBeginPeriod(1);
            timing = TRUE;
        }
        while (!flag_get(&s_move_exit)) {
            LONG mf = InterlockedCompareExchange(&s_move_flags, 0, 0);
            LONG sf = InterlockedCompareExchange(&s_scroll_flags, 0, 0);
            LONG gen = InterlockedCompareExchange(&s_target_gen, 0, 0);
            LONG auto_bits, aimed;
            DWORD now_us;
            double dt, remaining, tickDX = 0.0, tickDY = 0.0;
            double speed, max_speed, accel_time, curve, slow_speed;
            double scroll_base, scroll_max, fast_base, fast_max;
            int signX, signY, idx;
            BOOL slow, had_streams, any_streams;
            POINT cur;
            int vx, vy, vw, vh;
            BOOL have_cur;

            scroll_prefetch_now();
            // A new Alt scroll press: its stream starts under the pointer at
            // once, as a wheel would, and moves to the nearest scrollable
            // area as soon as the lookup answers (right away from a warm
            // cache, up to AUTO_TARGET_TIMEOUT_MS from a cold one). Waiting
            // for the answer first held back the scroll and the pointer for
            // that long. Clear only the bits this press covers; a press made
            // meanwhile gets its own lookup.
            if (gen != seen_gen) {
                LONG bits = InterlockedCompareExchange(&s_scroll_pending, 0, 0);

                seen_gen = gen;
                target_ok = FALSE;
                aim_asking = GetCursorPos(&aim_pt) != 0;
                aim_vertical = !(bits & (SCR_LEFT | SCR_RIGHT));
                QueryPerformanceCounter(&now);
                aim_start_us = qpc_us_from(now, freq);
                for (idx = 0; idx < 6; idx++)
                    scrOwed[idx] = 0.0;
                InterlockedCompareExchange(&s_scroll_pending, 0, bits);
            }
            auto_bits = InterlockedCompareExchange(&s_scroll_auto, 0, 0);
            if (aim_asking) {
                DWORD since_ms;
                int answer;

                QueryPerformanceCounter(&now);
                since_ms = (DWORD)(qpc_us_from(now, freq) - aim_start_us) / 1000;
                answer = uia_scroll_aim(aim_pt, aim_vertical, since_ms, &target);
                if (answer == SCROLL_AIM_ASKING &&
                    since_ms >= AUTO_TARGET_TIMEOUT_MS)
                    answer = SCROLL_AIM_POINTER;
                if (answer != SCROLL_AIM_ASKING) {
                    aim_asking = FALSE;
                    target_ok = answer == SCROLL_AIM_TARGET;
                    now_us = qpc_us_from(now, freq);
                    for (idx = 0; idx < 6; idx++) {
                        double owed = scrOwed[idx];

                        scrOwed[idx] = 0.0;
                        if (!target_ok || owed <= 0.0 || scrJump[idx])
                            continue;
                        // What went to the pointer while asking went to a
                        // place that does not scroll that way (that is what
                        // a redirect means), so the target still gets it: a
                        // tap's notch arrives there in full, a held key only
                        // keeps going.
                        if (scrActive[idx]) {
                            scrFed[idx] -= owed;
                            if (scrFed[idx] < 0.0)
                                scrFed[idx] = 0.0;
                        } else if (!(sf & (1 << idx))) {
                            scrMin[idx] = owed;
                            scrFed[idx] = 0.0;
                            scrStartUs[idx] = now_us;
                            scrLastUs[idx] = now_us;
                            scrActive[idx] = 1;
                        }
                    }
                }
            }
            aimed = target_ok ? auto_bits : 0;
            // A bare key has no Alt press to prefetch on, so keep the list
            // warm here: the next tap or held key then finds a fresh one
            // instead of waiting for a search. uia_scroll_prefetch never
            // waits.
            if (aimed) {
                DWORD tick = GetTickCount();
                POINT cur_pt;

                if ((last_prefetch == 0 ||
                     (DWORD)(tick - last_prefetch) >= SCROLL_PREFETCH_MS) &&
                    GetCursorPos(&cur_pt)) {
                    uia_scroll_prefetch(cur_pt);
                    last_prefetch = tick;
                }
            }

            QueryPerformanceCounter(&now);
            dt = (double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart;
            prev = now;
            if (dt <= 0.0)
                dt = 0.001;
            if (dt > SUSPEND_RESET_SEC) {
                velX = velY = 0.0;
                holdX = holdY = 0.0;
                scrAccX = scrAccY = 0.0;
                prevSignX = prevSignY = 0;
                aim_asking = FALSE;
                // A glide does not survive a suspend; the gap would otherwise
                // arrive all at once.
                for (idx = 0; idx < 6; idx++) {
                    scrOwed[idx] = 0.0;
                    if (!scrJump[idx])
                        continue;
                    scrJump[idx] = 0;
                    scrActive[idx] = 0;
                }
                wait_tick(timer);
                continue;
            }

            signX = ((mf & MOVE_RIGHT) ? 1 : 0) - ((mf & MOVE_LEFT) ? 1 : 0);
            signY = ((mf & MOVE_DOWN) ? 1 : 0) - ((mf & MOVE_UP) ? 1 : 0);
            if (prevSignX != 0 && signX == 0) {
                velX = holdX = 0.0;
            }
            if (prevSignY != 0 && signY == 0) {
                velY = holdY = 0.0;
            }
            prevSignX = signX;
            prevSignY = signY;

            speed = g_cfg.speed;
            max_speed = g_cfg.max_speed;
            accel_time = g_cfg.accel_time;
            curve = g_cfg.accel_curve;
            slow_speed = g_cfg.slow_speed;
            scroll_base = g_cfg.scroll_speed;
            scroll_max = g_cfg.scroll_max_speed;
            fast_base = g_cfg.fast_scroll_speed;
            fast_max = g_cfg.fast_scroll_max_speed;
            if (accel_time <= 0.001)
                accel_time = 0.001;
            if (curve <= 0.0)
                curve = 1.0;
            if (max_speed < speed)
                max_speed = speed;
            if (scroll_max < scroll_base)
                scroll_max = scroll_base;
            if (fast_max < fast_base)
                fast_max = fast_base;
            slow = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

            remaining = dt;
            while (remaining > 0.0) {
                double step = remaining > MAX_SIM_STEP_SEC ? MAX_SIM_STEP_SEC : remaining;
                double targetX, targetY;

                remaining -= step;
                if (signX != 0)
                    holdX += step;
                else
                    holdX = 0.0;
                if (signY != 0)
                    holdY += step;
                else
                    holdY = 0.0;

                if (slow) {
                    velX = (double)signX * slow_speed;
                    velY = (double)signY * slow_speed;
                } else {
                    double tX = holdX / accel_time;
                    double tY = holdY / accel_time;
                    // VELOCITY_SMOOTHING was tuned for 1 ms steps; scale it
                    // by the actual step so responsiveness does not depend on
                    // the tick rate.
                    double alpha = 1.0 - pow(1.0 - VELOCITY_SMOOTHING,
                                             step / 0.001);
                    if (tX < 0.0) tX = 0.0;
                    if (tX > 1.0) tX = 1.0;
                    if (tY < 0.0) tY = 0.0;
                    if (tY > 1.0) tY = 1.0;
                    targetX = (double)signX *
                        (speed + (max_speed - speed) * pow(tX, curve));
                    targetY = (double)signY *
                        (speed + (max_speed - speed) * pow(tY, curve));
                    velX += (targetX - velX) * alpha;
                    velY += (targetY - velY) * alpha;
                }

                tickDX += velX * step;
                tickDY += velY * step;
            }

            // Absolute moves are exact. Keep a double position and resync to
            // the real cursor whenever it was not left where we put it (an
            // external move, or a clamp at a screen edge). Sub-pixel progress
            // is dropped on a resync; until the first move of a burst the
            // cursor itself is the reference pixel.
            have_cur = GetCursorPos(&cur);
            vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (have_cur) {
                if (!have_ref ||
                    cur.x != ref.x || cur.y != ref.y) {
                    posX = cur.x;
                    posY = cur.y;
                    ref = cur;
                    have_ref = TRUE;
                }
            }
            if ((tickDX != 0.0 || tickDY != 0.0) && have_ref) {
                int px, py;

                if (vw <= 0)
                    vw = 1;
                if (vh <= 0)
                    vh = 1;
                posX += tickDX;
                posY += tickDY;
                if (posX < vx)
                    posX = vx;
                if (posX > vx + vw - 1)
                    posX = vx + vw - 1;
                if (posY < vy)
                    posY = vy;
                if (posY > vy + vh - 1)
                    posY = vy + vh - 1;
                px = round_i(posX);
                py = round_i(posY);
                if (px != ref.x || py != ref.y) {
                    send_absolute_move(px, py, vx, vy, vw, vh);
                    ref.x = px;
                    ref.y = py;
                    indicator_moved();
                }
            }

            had_streams = scroll_active_any(scrActive);
            now_us = qpc_us_from(now, freq);
            for (idx = 0; idx < 6; idx++) {
                LONG gen_i;
                BOOL held, dir_aimed;
                DWORD last_us;
                double elapsed_ms, base, maximum, sp, dt_i, delta, dir;

                gen_i = InterlockedCompareExchange(&s_scroll_gen[idx], 0, 0);
                held = (sf & (1 << idx)) != 0;

                if (scrSeenGen[idx] != gen_i) {
                    LONG jump;
                    int j, k, delta;

                    // The bottom jump through UI Automation is one shot at
                    // one point, so it alone waits for the lookup; streams
                    // start at once and are re-aimed.
                    if (aim_asking && idx == 5 && (auto_bits & (1 << idx)) &&
                        (InterlockedCompareExchange(&s_scroll_jump, 0, 0) &
                         (1 << idx)))
                        continue;
                    scrSeenGen[idx] = gen_i;

                    // Any new press ends a jump still gliding there; its
                    // accumulator is dropped so the next stream starts clean.
                    // Jumps are vertical only.
                    for (j = 0; j < 6; j++) {
                        if (!scrJump[j])
                            continue;
                        scrJump[j] = 0;
                        scrActive[j] = 0;
                        scrAccY = 0.0;
                    }

                    jump = InterlockedCompareExchange(&s_scroll_jump, 0, 0);
                    if (jump & (1 << idx)) {
                        atomic_and(&s_scroll_jump, ~(LONG)(1 << idx));
                        // The bottom jump goes through UI Automation first, so
                        // a held Shift does not turn it sideways. Only when
                        // that cannot scroll this area does the wheel go out,
                        // at Shift release.
                        if (idx == 5) {
                            LONG prev_uia = InterlockedCompareExchange(
                                &s_end_uia, END_UIA_ASKING, END_UIA_ASK);
                            BOOL send_wheel = TRUE;

                            if (prev_uia == END_UIA_ASK) {
                                POINT pt = { 0, 0 };
                                LONG after;

                                if ((aimed & (1 << idx)) && target_ok)
                                    pt = target;
                                else
                                    GetCursorPos(&pt);
                                if (uia_scroll_end(pt, SCROLL_END_UIA_MS)) {
                                    page_acted_at(PAGE_SCROLL, &pt);
                                    after = InterlockedCompareExchange(
                                        &s_end_uia, END_UIA_DONE,
                                        END_UIA_ASKING);
                                    // WHEEL: Shift came up while we asked and
                                    // the wheel was held back for us; it is
                                    // not needed now.
                                    if (after == END_UIA_WHEEL)
                                        InterlockedExchange(&s_end_uia,
                                                            END_UIA_IDLE);
                                    send_wheel = FALSE;
                                } else {
                                    after = InterlockedCompareExchange(
                                        &s_end_uia, END_UIA_FAILED,
                                        END_UIA_ASKING);
                                    // WHEEL: Shift is already up, so the wheel
                                    // is safe. Any other state means the press
                                    // was reset or replaced: send nothing (the
                                    // hook sends it at Shift up if FAILED).
                                    if (after == END_UIA_WHEEL)
                                        InterlockedExchange(&s_end_uia,
                                                            END_UIA_IDLE);
                                    else
                                        send_wheel = FALSE;
                                }
                                // The wait does not count as scrolling time,
                                // and a wheel jump after it starts from now.
                                QueryPerformanceCounter(&now);
                                prev = now;
                                now_us = qpc_us_from(now, freq);
                            } else if (prev_uia == END_UIA_WHEEL) {
                                // Shift came up before the thread got here.
                                InterlockedExchange(&s_end_uia, END_UIA_IDLE);
                            }
                            if (!send_wheel) {
                                // Stop the vertical streams, as a wheel jump
                                // does, and leave the rest to UI Automation.
                                scrActive[0] = scrActive[1] = 0;
                                scrActive[4] = scrActive[5] = 0;
                                scrAccY = 0.0;
                                continue;
                            }
                        }
                        if (!g_cfg.smooth_scroll) {
                            // No smooth scrolling: stop the axis and send
                            // the whole jump at once.
                            scrActive[0] = scrActive[1] = 0;
                            scrActive[4] = scrActive[5] = 0;
                            scrAccY = 0.0;
                            delta = idx == 4 ? 32760 : -32760;
                            dir_aimed = (aimed & (1 << idx)) != 0;
                            if (dir_aimed && target_ok) {
                                for (k = 0; k < 64; k++)
                                    post_wheel(target, TRUE, delta);
                            } else {
                                send_wheel_jump(TRUE, delta);
                            }
                            continue;
                        }
                        // Smooth: glide on this direction's fast stream from
                        // wherever the axis is, while the other vertical
                        // streams stop.
                        scrActive[0] = scrActive[1] = 0;
                        scrActive[idx == 4 ? 5 : 4] = 0;
                        scrActive[idx] = 1;
                        scrJump[idx] = 1;
                        scrJumpStartUs[idx] = now_us;
                        scrJumpDelivered[idx] = 0.0;
                        scrFed[idx] = 0.0;
                        scrLastUs[idx] = now_us;
                        continue;
                    }
                    scrMin[idx] = (double)abs(scroll_notch(idx));
                    scrFed[idx] = 0.0;
                    scrStartUs[idx] = now_us;
                    scrLastUs[idx] = now_us;
                    scrActive[idx] = 1;
                    if (!g_cfg.smooth_scroll) {
                        // Apps that treat each message as a whole notch get
                        // the first one now; the rest follow as distance
                        // accrues.
                        dir = scroll_notch(idx) < 0 ? -1.0 : 1.0;
                        scrFed[idx] = 120.0;
                        if (aim_asking && (auto_bits & (1 << idx)))
                            scrOwed[idx] = 120.0;
                        dir_aimed = (aimed & (1 << idx)) != 0;
                        if (scroll_vertical(idx)) {
                            scrAccY += dir * 120.0;
                            if (dir_aimed)
                                scrAimY = TRUE;
                        } else {
                            scrAccX += dir * 120.0;
                            if (dir_aimed)
                                scrAimX = TRUE;
                        }
                    }
                }

                if (!scrActive[idx])
                    continue;
                if (scrJump[idx]) {
                    // A glide ignores the key: its exact distance after t is
                    // v0 * tau * (e^(t/tau) - 1), fed frame by frame until it
                    // has covered the whole jump. Ticks do not affect accuracy.
                    double tau = SCROLL_JUMP_TAU_MS / 1000.0;
                    double total, jdelta;
                    DWORD elapsed_us;

                    last_us = scrLastUs[idx];
                    scrLastUs[idx] = now_us;
                    if ((DWORD)(now_us - last_us) >
                        (DWORD)(SUSPEND_RESET_SEC * 1000000.0))
                        continue;
                    elapsed_us = (DWORD)(now_us - scrJumpStartUs[idx]);
                    total = fast_base * tau *
                        (exp((double)elapsed_us / 1000.0 / tau) - 1.0);
                    if (total > SCROLL_JUMP_UNITS)
                        total = SCROLL_JUMP_UNITS;
                    jdelta = total - scrJumpDelivered[idx];
                    scrJumpDelivered[idx] = total;
                    dir = scroll_notch(idx) < 0 ? -1.0 : 1.0;
                    dir_aimed = (aimed & (1 << idx)) != 0;
                    if (jdelta > 0.0) {
                        scrAccY += dir * jdelta;
                        if (dir_aimed)
                            scrAimY = TRUE;
                    }
                    if (total >= SCROLL_JUMP_UNITS) {
                        scrJump[idx] = 0;
                        scrActive[idx] = 0;
                    }
                    continue;
                }
                if (!held && scrFed[idx] >= scrMin[idx]) {
                    // Released after the minimum: stop right away.
                    scrActive[idx] = 0;
                    continue;
                }

                last_us = scrLastUs[idx];
                scrLastUs[idx] = now_us;
                if ((DWORD)(now_us - last_us) >
                    (DWORD)(SUSPEND_RESET_SEC * 1000000.0))
                    continue;
                dt_i = (double)(DWORD)(now_us - last_us) / 1000000.0;
                if (dt_i <= 0.0)
                    continue;
                elapsed_ms = (double)(DWORD)(now_us - scrStartUs[idx]) / 1000.0;
                if (idx == 4 || idx == 5) {
                    base = fast_base;
                    maximum = fast_max;
                } else {
                    base = scroll_base;
                    maximum = scroll_max;
                }
                sp = scroll_speed_for(elapsed_ms, base, maximum,
                                      accel_time, curve);
                delta = sp * dt_i;
                if (!held) {
                    // A tap keeps going until its minimum is delivered, and
                    // no further.
                    if (scrFed[idx] + delta >= scrMin[idx])
                        delta = scrMin[idx] - scrFed[idx];
                }
                if (delta <= 0.0)
                    continue;
                scrFed[idx] += delta;
                if (aim_asking && (auto_bits & (1 << idx)))
                    scrOwed[idx] += delta;
                dir = scroll_notch(idx) < 0 ? -1.0 : 1.0;
                dir_aimed = (aimed & (1 << idx)) != 0;
                if (scroll_vertical(idx)) {
                    scrAccY += dir * delta;
                    if (dir_aimed)
                        scrAimY = TRUE;
                } else {
                    scrAccX += dir * delta;
                    if (dir_aimed)
                        scrAimX = TRUE;
                }
                if (!held && scrFed[idx] >= scrMin[idx])
                    scrActive[idx] = 0;
            }

            any_streams = scroll_active_any(scrActive);
            if (g_cfg.smooth_scroll && any_streams &&
                (!clock.valid || !had_streams)) {
                // A new burst: follow the compositor's frame boundaries.
                if (!clock.valid ||
                    (DWORD)(now_us - clock.refreshed_us) >= 500000)
                    scroll_clock_refresh(&clock, now, freq, now_us);
                scrFrameValid = FALSE;
            }

            if (g_cfg.smooth_scroll) {
                LONGLONG frame = scroll_frame_index(&clock, now);

                if (!scrFrameValid || frame != scrFrame) {
                    int dx = 0, dy = 0, part;

                    scrFrameValid = TRUE;
                    scrFrame = frame;
                    if (scrAccY >= 1.0 - SCROLL_EPS ||
                        scrAccY <= -1.0 + SCROLL_EPS) {
                        dy = (int)(scrAccY +
                            (scrAccY >= 0.0 ? SCROLL_EPS : -SCROLL_EPS));
                        scrAccY -= (double)dy;
                    }
                    if (scrAccX >= 1.0 - SCROLL_EPS ||
                        scrAccX <= -1.0 + SCROLL_EPS) {
                        dx = (int)(scrAccX +
                            (scrAccX >= 0.0 ? SCROLL_EPS : -SCROLL_EPS));
                        scrAccX -= (double)dx;
                    }
                    // A wheel delta is a signed 16-bit value, so a frame's
                    // distance may take several events.
                    while (dy != 0) {
                        part = dy > 32760 ? 32760 :
                               dy < -32760 ? -32760 : dy;
                        if (scrAimY && target_ok)
                            post_wheel(target, TRUE, part);
                        else
                            send_wheel(TRUE, part);
                        dy -= part;
                    }
                    while (dx != 0) {
                        part = dx > 32760 ? 32760 :
                               dx < -32760 ? -32760 : dx;
                        if (scrAimX && target_ok)
                            post_wheel(target, FALSE, part);
                        else
                            send_wheel(FALSE, part);
                        dx -= part;
                    }
                }
            } else {
                // Whole notches only, one per event, rounding up.
                while (scrAccY >= 120.0 - SCROLL_EPS ||
                       scrAccY <= -120.0 + SCROLL_EPS) {
                    int notch = scrAccY > 0.0 ? 120 : -120;

                    if (scrAimY && target_ok)
                        post_wheel(target, TRUE, notch);
                    else
                        send_wheel(TRUE, notch);
                    scrAccY -= (double)notch;
                }
                while (scrAccX >= 120.0 - SCROLL_EPS ||
                       scrAccX <= -120.0 + SCROLL_EPS) {
                    int notch = scrAccX > 0.0 ? 120 : -120;

                    if (scrAimX && target_ok)
                        post_wheel(target, FALSE, notch);
                    else
                        send_wheel(FALSE, notch);
                    scrAccX -= (double)notch;
                }
            }

            // A lookup still asking keeps the loop ticking: a tap's notch
            // may yet have to go to the target.
            if (!mf && !sf && !any_streams && !aim_asking &&
                !scroll_flush_pending(scrAccX, scrAccY) &&
                !scroll_press_pending(scrSeenGen) &&
                !InterlockedCompareExchange(&s_scroll_pending, 0, 0))
                break;

            wait_tick(timer);
        }
        if (timing) {
            timeEndPeriod(1);
            timing = FALSE;
        }
    }

    if (timing)
        timeEndPeriod(1);
    if (timer)
        CloseHandle(timer);
    return 0;
}

static HFONT indicator_font(int height)
{
    if (height < 6)
        height = 6;
    if (s_ind_font && s_ind_font_h == height)
        return s_ind_font;
    if (s_ind_font) {
        DeleteObject(s_ind_font);
        s_ind_font = NULL;
    }
    s_ind_font = CreateFontW(-height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    s_ind_font_h = height;
    return s_ind_font;
}

static void indicator_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc;
    HDC mem;
    HBITMAP bmp;
    HGDIOBJ old_bmp;
    HGDIOBJ old_brush;
    HGDIOBJ old_pen;
    HGDIOBJ old_font = NULL;
    HBRUSH key_brush;
    HBRUSH brush;
    HFONT font;
    RECT rc;
    int w, h, font_h;
    BOOL drag;
    COLORREF bg;

    hdc = BeginPaint(hwnd, &ps);
    GetClientRect(hwnd, &rc);
    w = rc.right;
    h = rc.bottom;
    if (w <= 0 || h <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }

    mem = CreateCompatibleDC(hdc);
    bmp = CreateCompatibleBitmap(hdc, w, h);
    old_bmp = SelectObject(mem, bmp);

    key_brush = CreateSolidBrush(IND_KEY_COLOR);
    FillRect(mem, &rc, key_brush);
    DeleteObject(key_brush);

    drag = flag_get(&s_left_held);
    bg = drag ? IND_ORANGE : IND_BLUE;
    brush = CreateSolidBrush(bg);
    old_brush = SelectObject(mem, brush);
    old_pen = SelectObject(mem, GetStockObject(NULL_PEN));
    RoundRect(mem, 0, 0, w, h, h, h);
    SelectObject(mem, old_pen);
    SelectObject(mem, old_brush);
    DeleteObject(brush);

    font_h = MulDiv(h, 52, 100);
    font = indicator_font(font_h);
    if (font)
        old_font = SelectObject(mem, font);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, drag ? IND_DARK : IND_WHITE);
    DrawTextW(mem, drag ? L"DRAG" : L"MOUSE", -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (old_font)
        SelectObject(mem, old_font);

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK ind_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == IND_TIMER)
            indicator_update();
        return 0;
    case WM_PAINT:
        indicator_paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_INPUT_IND_UPDATE:
        // Cleared before the cursor is read, so a move in between posts again.
        InterlockedExchange(&s_ind_posted, 0);
        indicator_update();
        return 0;
    case WM_DESTROY:
        if (s_ind_font) {
            DeleteObject(s_ind_font);
            s_ind_font = NULL;
            s_ind_font_h = 0;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void indicator_update(void)
{
    POINT pt;
    HMONITOR mon;
    MONITORINFO mi;
    RECT rc;
    UINT dpi;
    int w, h, x, y;

    if (!s_ind_wnd)
        return;
    if (!flag_get(&s_mouse_mode) || g_cfg.indicator == INDICATOR_OFF) {
        if (s_ind_timer_on) {
            KillTimer(s_ind_wnd, IND_TIMER);
            s_ind_timer_on = FALSE;
        }
        ShowWindow(s_ind_wnd, SW_HIDE);
        return;
    }

    GetCursorPos(&pt);
    mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    dpi = dpi_for_monitor(mon);
    if (dpi == 0)
        dpi = 96;
    w = MulDiv(76, (int)dpi, 96);
    h = MulDiv(26, (int)dpi, 96);

    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        rc = mi.rcMonitor;
    } else {
        rc.left = 0;
        rc.top = 0;
        rc.right = GetSystemMetrics(SM_CXSCREEN);
        rc.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    if (g_cfg.indicator == INDICATOR_CORNER) {
        x = rc.left;
        y = rc.top;
    } else {
        x = pt.x + MulDiv(18, (int)dpi, 96);
        y = pt.y + MulDiv(22, (int)dpi, 96);
    }
    if (x + w > rc.right)
        x = rc.right - w;
    if (y + h > rc.bottom)
        y = rc.bottom - h;
    if (x < rc.left)
        x = rc.left;
    if (y < rc.top)
        y = rc.top;

    // The timer runs every 15 ms; only touch the window when something changed.
    {
        static RECT last;
        static BOOL last_drag;
        BOOL drag = flag_get(&s_left_held);
        BOOL moved = last.left != x || last.top != y ||
                     last.right != x + w || last.bottom != y + h ||
                     !IsWindowVisible(s_ind_wnd);
        if (moved)
            SetWindowPos(s_ind_wnd, HWND_TOPMOST, x, y, w, h,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (moved || drag != last_drag)
            InvalidateRect(s_ind_wnd, NULL, FALSE);
        SetRect(&last, x, y, x + w, y + h);
        last_drag = drag;
    }
    if (!s_ind_timer_on) {
        SetTimer(s_ind_wnd, IND_TIMER, 15, NULL);
        s_ind_timer_on = TRUE;
    }
}

static void indicator_schedule(void)
{
    if (s_ind_wnd)
        PostMessageW(s_ind_wnd, WM_INPUT_IND_UPDATE, 0, 0);
    else if (s_input_tid)
        PostThreadMessageW(s_input_tid, WM_INPUT_IND_UPDATE, 0, 0);
}

// The movement thread moved the pointer: move the pill with it now, not at
// the next tick of the 15 ms timer, which left it trailing and stepping
// behind a pointer that moves every millisecond. The timer stays for the
// physical mouse. One post in flight at a time; the input thread is idle and
// at the highest priority, so it lands within the same millisecond. The
// window outlives the movement thread (cleanup_input stops that first).
static void indicator_moved(void)
{
    if (!s_ind_wnd || g_cfg.indicator != INDICATOR_CURSOR ||
        !flag_get(&s_mouse_mode))
        return;
    if (InterlockedCompareExchange(&s_ind_posted, 1, 0) == 0)
        PostMessageW(s_ind_wnd, WM_INPUT_IND_UPDATE, 0, 0);
}

static BOOL create_indicator_window(void)
{
    WNDCLASSEXW wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ind_wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = IND_CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return FALSE;

    s_ind_wnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        IND_CLASS, L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
    if (!s_ind_wnd)
        return FALSE;
    SetLayeredWindowAttributes(s_ind_wnd, IND_KEY_COLOR, 255, LWA_COLORKEY);
    return TRUE;
}

static BOOL register_raw_keyboard(HWND target, BOOL on)
{
    RAWINPUTDEVICE device;

    ZeroMemory(&device, sizeof(device));
    device.usUsagePage = 0x01;
    device.usUsage = 0x06;
    device.dwFlags = on ? RIDEV_INPUTSINK : RIDEV_REMOVE;
    device.hwndTarget = on ? target : NULL;
    return RegisterRawInputDevices(&device, 1, sizeof(device));
}

// A live hook is always called before the raw input for the same key event is
// produced, so a raw message with an unchanged callback count means the hook
// is gone. Keys the hook swallows produce no raw input, which keeps this
// one-sided.
static void raw_input_check(HRAWINPUT raw)
{
    RAWINPUT input;
    UINT size = sizeof(input);
    LONG calls;

    if (GetRawInputData(raw, RID_INPUT, &input, &size,
                        sizeof(RAWINPUTHEADER)) == (UINT)-1)
        return;
    if (input.header.dwType != RIM_TYPEKEYBOARD)
        return;
    calls = InterlockedCompareExchange(&s_kb_calls, 0, 0);
    if (calls != s_kb_calls_seen) {
        s_kb_calls_seen = calls;
        s_hook_misses = 0;
        return;
    }
    s_kb_calls_seen = calls;
    if (!s_hook) {
        // Suspended on purpose: nothing to replace.
        s_hook_misses = 0;
        return;
    }
    if (++s_hook_misses < 3)
        return;
    replace_hook();
}

static LRESULT CALLBACK power_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_CREATE:
        WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND)
            suspend_input();
        else if (wp == PBT_APMRESUMESUSPEND || wp == PBT_APMRESUMEAUTOMATIC)
            resume_input();
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        if (wp == (WPARAM)WTS_SESSION_LOCK ||
            wp == (WPARAM)WTS_CONSOLE_DISCONNECT ||
            wp == (WPARAM)WTS_REMOTE_DISCONNECT)
            suspend_input();
        else if (wp == (WPARAM)WTS_SESSION_UNLOCK ||
                 wp == (WPARAM)WTS_CONSOLE_CONNECT ||
                 wp == (WPARAM)WTS_REMOTE_CONNECT)
            resume_input();
        return 0;
    case WM_INPUT:
        raw_input_check((HRAWINPUT)lp);
        break;
    case WM_TIMER:
        if (wp == CAPS_TIMER) {
            caps_check(0);
            return 0;
        }
        if (wp == PTR_TIMER) {
            ptr_update();
            return 0;
        }
        break;
    case WM_INPUT_CAPS_CHECK:
        caps_check((UINT)wp);
        return 0;
    case WM_DESTROY:
        WTSUnRegisterSessionNotification(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static BOOL create_power_window(void)
{
    WNDCLASSEXW wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = power_wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = POWER_CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return FALSE;

    s_power_wnd = CreateWindowExW(0, POWER_CLASS, L"", WS_OVERLAPPED,
                                  0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    return s_power_wnd != NULL;
}

static void start_movement_thread(void)
{
    if (s_move_thread)
        return;
    InterlockedExchange(&s_move_exit, 0);
    s_move_thread = CreateThread(NULL, 0, movement_thread, NULL, 0, NULL);
}

static void stop_movement_thread(void)
{
    if (!s_move_thread)
        return;
    InterlockedExchange(&s_move_exit, 1);
    if (s_move_event)
        SetEvent(s_move_event);
    WaitForSingleObject(s_move_thread, 3000);
    CloseHandle(s_move_thread);
    s_move_thread = NULL;
}

static void cleanup_input(void)
{
    int i;

    leave_mouse_mode();
    uninstall_hook();
    if (s_mouse_hook) {
        UnhookWindowsHookEx(s_mouse_hook);
        s_mouse_hook = NULL;
    }
    if (s_ptr_timer_on) {
        KillTimer(s_power_wnd, PTR_TIMER);
        s_ptr_timer_on = FALSE;
    }
    ptr_thread_stop();
    for (i = 0; i < 2; i++) {
        if (s_focus_hooks[i]) {
            UnhookWinEvent(s_focus_hooks[i]);
            s_focus_hooks[i] = NULL;
        }
    }
    stop_movement_thread();
    if (s_caps_timer_on) {
        KillTimer(s_power_wnd, CAPS_TIMER);
        s_caps_timer_on = FALSE;
    }
    if (s_ind_timer_on) {
        KillTimer(s_ind_wnd, IND_TIMER);
        s_ind_timer_on = FALSE;
    }
    if (s_ind_wnd) {
        DestroyWindow(s_ind_wnd);
        s_ind_wnd = NULL;
    }
    if (s_raw_kb_on) {
        register_raw_keyboard(NULL, FALSE);
        s_raw_kb_on = FALSE;
    }
    if (s_power_wnd) {
        DestroyWindow(s_power_wnd);
        s_power_wnd = NULL;
    }
}

// Windows 11 ignores timer-resolution requests from processes without a
// visible window. Opting out keeps the Sleep(1) fallback tick at ~1 ms.
static void enable_timer_resolution(void)
{
    typedef BOOL (WINAPI *SetProcessInformationFn)(HANDLE, int, LPVOID, DWORD);
    static BOOL done = FALSE;
    VM_PROCESS_POWER_THROTTLING_STATE state;
    SetProcessInformationFn set_info;

    if (done)
        return;
    done = TRUE;
    set_info = (SetProcessInformationFn)(void *)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation");
    if (!set_info)
        return;
    ZeroMemory(&state, sizeof(state));
    state.Version = 1;
    state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0;
    set_info(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
}

static DWORD WINAPI input_thread_proc(LPVOID param)
{
    MSG msg;
    int result;

    (void)param;
    s_input_tid = GetCurrentThreadId();
    // A starved hook thread delays every key press system-wide.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    create_power_window();
    create_indicator_window();
    start_movement_thread();

    s_started_ok = install_hook();
    if (s_started_ok && s_power_wnd) {
        s_raw_kb_on = register_raw_keyboard(s_power_wnd, TRUE);
        if (!s_raw_kb_on)
            app_log(L"input: raw input registration failed");
    }
    if (s_ready_event)
        SetEvent(s_ready_event);
    if (!s_started_ok) {
        cleanup_input();
        s_input_tid = 0;
        return 1;
    }
    focus_hooks_update();
    caps_timer_update();
    ptr_thread_start();
    ptr_hook_update();
    ptr_update();

    while ((result = GetMessageW(&msg, NULL, 0, 0)) > 0) {
        if (msg.hwnd == NULL) {
            if (msg.message == WM_INPUT_SET_MODE) {
                set_mouse_mode(msg.wParam != 0);
                continue;
            }
            if (msg.message == WM_INPUT_SET_LOCKED) {
                apply_locked(msg.wParam != 0);
                continue;
            }
            if (msg.message == WM_INPUT_SET_PAUSED) {
                if (msg.wParam) {
                    prescan_cancel();
                    leave_mouse_mode();
                }
                continue;
            }
            if (msg.message == WM_INPUT_CONFIG) {
                focus_hooks_update();
                caps_timer_update();
                indicator_update();
                ptr_hook_update();
                ptr_update();
                continue;
            }
            if (msg.message == WM_INPUT_IND_UPDATE) {
                indicator_update();
                continue;
            }
            if (msg.message == WM_INPUT_PTR_UPDATE) {
                ptr_update();
                continue;
            }
            if (msg.message == WM_INPUT_CENTER) {
                center_pointer();
                continue;
            }
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    cleanup_input();
    s_input_tid = 0;
    return 0;
}

BOOL input_start(void)
{
    DWORD tid = 0;

    if (s_input_thread)
        return TRUE;
    enable_timer_resolution();
    s_started_ok = FALSE;
    s_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!s_ready_event)
        return FALSE;
    s_move_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!s_move_event) {
        CloseHandle(s_ready_event);
        s_ready_event = NULL;
        return FALSE;
    }
    InterlockedExchange(&s_move_exit, 0);
    s_input_thread = CreateThread(NULL, 0, input_thread_proc, NULL, 0, &tid);
    if (!s_input_thread) {
        CloseHandle(s_move_event);
        s_move_event = NULL;
        CloseHandle(s_ready_event);
        s_ready_event = NULL;
        return FALSE;
    }
    WaitForSingleObject(s_ready_event, 5000);
    if (!s_started_ok) {
        WaitForSingleObject(s_input_thread, 3000);
        CloseHandle(s_input_thread);
        s_input_thread = NULL;
        CloseHandle(s_move_event);
        s_move_event = NULL;
        CloseHandle(s_ready_event);
        s_ready_event = NULL;
        return FALSE;
    }
    return TRUE;
}

void input_stop(void)
{
    if (!s_input_thread)
        return;
    if (GetCurrentThreadId() == s_input_tid) {
        PostThreadMessageW(s_input_tid, WM_QUIT, 0, 0);
        return;
    }
    if (s_input_tid)
        PostThreadMessageW(s_input_tid, WM_QUIT, 0, 0);
    WaitForSingleObject(s_input_thread, 5000);
    CloseHandle(s_input_thread);
    s_input_thread = NULL;
    if (s_move_event) {
        CloseHandle(s_move_event);
        s_move_event = NULL;
    }
    if (s_ready_event) {
        CloseHandle(s_ready_event);
        s_ready_event = NULL;
    }
    s_input_tid = 0;
}

void input_set_paused(BOOL paused)
{
    LONG value = paused ? 1 : 0;
    LONG old = InterlockedExchange(&s_paused, value);

    if (old == value)
        return;
    notify_state();
    if (value) {
        if (s_input_tid && GetCurrentThreadId() != s_input_tid)
            PostThreadMessageW(s_input_tid, WM_INPUT_SET_PAUSED, 1, 0);
        else
            leave_mouse_mode();
    }
}

BOOL input_is_paused(void)
{
    return flag_get(&s_paused);
}

void input_set_locked(BOOL locked)
{
    if (s_input_tid && GetCurrentThreadId() != s_input_tid)
        PostThreadMessageW(s_input_tid, WM_INPUT_SET_LOCKED, locked ? 1 : 0, 0);
    else
        apply_locked(locked);
}

BOOL input_is_locked(void)
{
    return flag_get(&s_locked);
}

void input_set_mouse_mode(BOOL on)
{
    if (on) {
        if (flag_get(&s_mouse_mode))
            return;
        InterlockedExchange(&s_mouse_mode, 1);
        notify_state();
        indicator_schedule();
    } else {
        if (s_input_tid && GetCurrentThreadId() != s_input_tid)
            PostThreadMessageW(s_input_tid, WM_INPUT_SET_MODE, 0, 0);
        else
            leave_mouse_mode();
    }
}

BOOL input_mouse_mode_active(void)
{
    return flag_get(&s_mouse_mode);
}

BOOL input_left_button_held(void)
{
    return flag_get(&s_left_held);
}

void input_set_key_capture(BOOL on)
{
    flag_set(&s_capture, on);
}

void input_type_text(const WCHAR *text)
{
    INPUT *in;
    size_t len, i;
    UINT n = 0;

    if (text == NULL || text[0] == 0)
        return;
    len = wcslen(text);
    in = (INPUT *)calloc(len * 2, sizeof(INPUT));
    if (in == NULL)
        return;
    for (i = 0; i < len; i++) {
        WCHAR ch = text[i];
        INPUT *down = &in[n];
        INPUT *up = &in[n + 1];

        if (ch == L'\r')
            continue;
        down->type = INPUT_KEYBOARD;
        up->type = INPUT_KEYBOARD;
        if (ch == L'\n' || ch == L'\t') {
            down->ki.wVk = ch == L'\n' ? VK_RETURN : VK_TAB;
            up->ki.wVk = down->ki.wVk;
            up->ki.dwFlags = KEYEVENTF_KEYUP;
        } else {
            down->ki.wScan = ch;
            down->ki.dwFlags = KEYEVENTF_UNICODE;
            up->ki.wScan = ch;
            up->ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        }
        n += 2;
    }
    // One call, so the user's own keys cannot land in the middle.
    if (n > 0 && SendInput(n, in, sizeof(INPUT)) != n)
        app_log(L"input: typing a text shortcut failed (%lu)", GetLastError());
    free(in);
}

void input_config_changed(void)
{
    if (s_input_tid)
        PostThreadMessageW(s_input_tid, WM_INPUT_CONFIG, 0, 0);
}
