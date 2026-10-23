// input.c - low-level keyboard hook, mouse mode, movement/scroll, indicator.
#include "common.h"

#include <math.h>

#ifndef WM_WTSSESSION_CHANGE
#define WM_WTSSESSION_CHANGE 0x02B1
#endif
#ifndef WTS_SESSION_LOCK
#define WTS_SESSION_LOCK 0x7
#endif
#ifndef WTS_SESSION_UNLOCK
#define WTS_SESSION_UNLOCK 0x8
#endif
#ifndef NOTIFY_FOR_THIS_SESSION
#define NOTIFY_FOR_THIS_SESSION 0
#endif
#ifndef MOUSEEVENTF_HWHEEL
#define MOUSEEVENTF_HWHEEL 0x01000
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

BOOL WINAPI WTSRegisterSessionNotification(HWND hWnd, DWORD dwFlags);
BOOL WINAPI WTSUnRegisterSessionNotification(HWND hWnd);
UINT WINAPI timeBeginPeriod(UINT uPeriod);
UINT WINAPI timeEndPeriod(UINT uPeriod);

#define POWER_CLASS L"VimMouseInputPower"
#define IND_CLASS   L"VimMouseInputIndicator"
#define IND_TIMER   1

#define IND_KEY_COLOR RGB(255, 0, 255)
#define IND_BLUE      RGB(0x2D, 0x7F, 0xF9)
#define IND_ORANGE    RGB(0xF2, 0x99, 0x4A)
#define IND_WHITE     RGB(0xFF, 0xFF, 0xFF)
#define IND_DARK      RGB(0x1E, 0x1E, 0x1E)

#define WM_INPUT_SET_MODE    (WM_APP + 20)
#define WM_INPUT_SET_PAUSED  (WM_APP + 21)
#define WM_INPUT_CONFIG      (WM_APP + 22)
#define WM_INPUT_IND_UPDATE  (WM_APP + 24)

#define MOVE_LEFT  0x01
#define MOVE_DOWN  0x02
#define MOVE_UP    0x04
#define MOVE_RIGHT 0x08

#define SCR_UP    0x01
#define SCR_DOWN  0x02
#define SCR_LEFT  0x04
#define SCR_RIGHT 0x08

#define VELOCITY_SMOOTHING 0.12
#define MAX_SIM_STEP_SEC   0.05
#define SUSPEND_RESET_SEC  2.0
// Holding a scroll key repeats after this delay (a tap scrolls one notch).
#define SCROLL_HOLD_DELAY_MS 150.0

enum {
    ACT_NONE = 0,
    ACT_MOVE_LEFT, ACT_MOVE_DOWN, ACT_MOVE_UP, ACT_MOVE_RIGHT,
    ACT_SCROLL_UP, ACT_SCROLL_DOWN, ACT_SCROLL_LEFT, ACT_SCROLL_RIGHT,
    ACT_LEFT, ACT_CLICK_RIGHT, ACT_CLICK_MIDDLE, ACT_TOGGLE,
    ACT_HINTS, ACT_EXIT, ACT_HELP
};

enum { OWN_NONE = 0, OWN_PASS, OWN_SWALLOW, OWN_MOVE, OWN_LEFT, OWN_CAPS, OWN_ALT_HELD };

#define LEFT_SRC_NONE 0
#define LEFT_SRC_KEY  1
#define LEFT_SRC_TOG  2

static volatile LONG s_paused = 0;
static volatile LONG s_mouse_mode = 0;
static volatile LONG s_capture = 0;
static volatile LONG s_left_held = 0;
static volatile LONG s_move_flags = 0;
static volatile LONG s_scroll_flags = 0;
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

static DWORD s_scroll_start[4];

static HANDLE s_move_event = NULL;
static HANDLE s_move_thread = NULL;
static HANDLE s_input_thread = NULL;
static HANDLE s_ready_event = NULL;
static DWORD  s_input_tid = 0;
static BOOL   s_started_ok = FALSE;

static HHOOK s_hook = NULL;
static HWND  s_power_wnd = NULL;
static HWND  s_ind_wnd = NULL;
static HFONT s_ind_font = NULL;
static int   s_ind_font_h = 0;
static BOOL  s_ind_timer_on = FALSE;

#ifdef INPUT_TEST
static BOOL s_test_mods_on = FALSE;
static UINT s_test_mods_value = 0;
static BOOL s_test_popup_on = FALSE;
static BOOL s_test_popup_value = FALSE;
#endif

static void indicator_schedule(void);
static void indicator_update(void);
static BOOL install_hook(void);
static void uninstall_hook(void);
static void reset_key_state(void);
static void suspend_input(void);
static void resume_input(void);

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
}

static void send_relative_move(int dx, int dy)
{
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

static void send_button(int button, BOOL down)
{
    INPUT in;
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
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.mouseData = (DWORD)delta;
    in.mi.dwFlags = vertical ? MOUSEEVENTF_WHEEL : MOUSEEVENTF_HWHEEL;
    SendInput(1, &in, sizeof(in));
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
        if (flag_get(&s_capture)) {
            flag_set(&s_capture, FALSE);
            post_main(WM_APP_HINT_CLOSE, 0, 0);
        }
    }
    notify_state();
}

void mouse_move_to(POINT pt)
{
    SetCursorPos(pt.x, pt.y);
}

void mouse_click_at(POINT pt, int button, int count)
{
    INPUT in[4];
    int n;
    int i;

    SetCursorPos(pt.x, pt.y);
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
}

// Menus and flyouts are title-less popups owned by the app's window. The owner
// test keeps out the taskbar, which lives in explorer.exe above every
// Explorer window.
static BOOL window_is_popup(HWND hwnd)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    WCHAR cls[32];

    if (!(style & WS_POPUP) || (style & WS_CAPTION) || !GetWindow(hwnd, GW_OWNER))
        return FALSE;
    GetClassNameW(hwnd, cls, 32);
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

static BOOL popup_open(void)
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

    fg = GetForegroundWindow();
    if (fg) {
        tid = GetWindowThreadProcessId(fg, &pid);
        ZeroMemory(&gti, sizeof(gti));
        gti.cbSize = sizeof(gti);
        if (tid && GetGUIThreadInfo(tid, &gti) &&
            (gti.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE |
                          GUI_SYSTEMMENUMODE)))
            return TRUE;
    }
    if (!fg || pid == 0)
        return FALSE;

    root = GetAncestor(fg, GA_ROOT);
    if (root && window_is_popup(root))
        return TRUE;

    steps = 0;
    w = GetTopWindow(NULL);
    while (w && steps < 256) {
        if (w == root)
            break;
        if (IsWindowVisible(w)) {
            GetWindowThreadProcessId(w, &wpid);
            if (wpid == pid && window_is_popup(w) &&
                GetWindowRect(w, &rc) && !IsRectEmpty(&rc) &&
                !window_cloaked(w))
                return TRUE;
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

static void scroll_press(int act)
{
    int idx = act - ACT_SCROLL_UP;
    LONG bit = (LONG)(1 << idx);
    DWORD now = GetTickCount();
    s_scroll_start[idx] = now;
    atomic_or(&s_scroll_flags, bit);
    if (s_move_event)
        SetEvent(s_move_event);
    if (idx == 0)
        send_wheel(TRUE, 120);
    else if (idx == 1)
        send_wheel(TRUE, -120);
    else if (idx == 2)
        send_wheel(FALSE, -120);
    else
        send_wheel(FALSE, 120);
}

static void scroll_release(int act)
{
    int idx = act - ACT_SCROLL_UP;
    atomic_and(&s_scroll_flags, ~(LONG)(1 << idx));
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
        s_caps_enter = FALSE;
        s_caps_exit_on_release = TRUE;
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
        scroll_press(act);
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

static BOOL handle_hotkeys(UINT vk, UINT mods, BOOL send_mask)
{
    if (hotkey_match(&g_cfg.quit_hotkey, vk, mods)) {
        if (send_mask && (mods & (KMOD_ALT | KMOD_WIN)))
            send_mask_key();
        post_main(WM_APP_QUIT, 0, 0);
        return TRUE;
    }
    if (hotkey_match(&g_cfg.pause_hotkey, vk, mods)) {
        if (send_mask && (mods & (KMOD_ALT | KMOD_WIN)))
            send_mask_key();
        apply_paused(!flag_get(&s_paused));
        return TRUE;
    }
    if (!flag_get(&s_paused) && hotkey_match(&g_cfg.hint_hotkey, vk, mods)) {
        if (send_mask && (mods & (KMOD_ALT | KMOD_WIN)))
            send_mask_key();
        if (!flag_get(&s_capture)) {
            flag_set(&s_capture, TRUE);
            post_main(WM_APP_HINT_OPEN, 0, 0);
        } else {
            post_main(WM_APP_HINT_AGAIN, 0, 0);
        }
        return TRUE;
    }
    return FALSE;
}

static BOOL alt_hotkey_configured(void)
{
    return (g_cfg.hint_hotkey.mods & KMOD_ALT) ||
           (g_cfg.quit_hotkey.mods & KMOD_ALT) ||
           (g_cfg.pause_hotkey.mods & KMOD_ALT);
}

static BOOL alt_should_withhold(void)
{
    if (flag_get(&s_paused) || !alt_hotkey_configured())
        return FALSE;
    return flag_get(&s_capture) || popup_open();
}

// The app never saw Alt, so hotkeys act without the 0xE8 menu mask.
static BOOL alt_resolve(UINT vk, UINT scan, DWORD flags)
{
    UINT mods = current_mods();

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
        post_main(WM_APP_HINT_KEY, vk, mods);
        return TRUE;
    }
    send_alt_replay(vk, scan, flags);
    s_owner[vk] = OWN_PASS;
    s_owner[VK_LMENU] = OWN_PASS;
    s_alt_pending = FALSE;
    s_alt_consumed = FALSE;
    return TRUE;
}

static BOOL handle_down(UINT vk)
{
    UINT mods = current_mods();
    int act;

    if (handle_hotkeys(vk, mods, TRUE)) {
        s_owner[vk] = OWN_SWALLOW;
        return TRUE;
    }
    if (flag_get(&s_paused)) {
        s_owner[vk] = OWN_PASS;
        return FALSE;
    }
    if (flag_get(&s_capture)) {
        s_owner[vk] = OWN_SWALLOW;
        // Esc drops capture right here too, so typing comes back even if
        // the UI thread never processes the close.
        if (vk == VK_ESCAPE)
            flag_set(&s_capture, FALSE);
        post_main(WM_APP_HINT_KEY, vk, mods);
        return TRUE;
    }
    if (g_cfg.mouse_mode_key != 0 && vk == g_cfg.mouse_mode_key &&
        !(mods & (KMOD_CTRL | KMOD_ALT | KMOD_WIN | KMOD_SHIFT))) {
        s_owner[vk] = OWN_CAPS;
        caps_key_down();
        return TRUE;
    }
    if (flag_get(&s_mouse_mode)) {
        if (mods & (KMOD_CTRL | KMOD_ALT | KMOD_WIN)) {
            s_owner[vk] = OWN_PASS;
            return FALSE;
        }
        if (vk == VK_ESCAPE) {
            s_owner[vk] = OWN_SWALLOW;
            mark_caps_used();
            leave_mouse_mode();
            return TRUE;
        }
        act = binding_action(vk);
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
            s_owner[vk] = OWN_SWALLOW;
            mark_caps_used();
            return TRUE;
        }
    }
    s_owner[vk] = OWN_PASS;
    return FALSE;
}

static BOOL handle_up(UINT vk)
{
    int owner;
    int act;

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
        else if (act >= ACT_SCROLL_UP && act <= ACT_SCROLL_RIGHT)
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

static BOOL handle_key(UINT vk, BOOL down, DWORD flags, UINT scan)
{
    int owner;

    if (vk > 255)
        return FALSE;
    if (flags & LLKHF_INJECTED)
        return FALSE;
    if (down) {
        BOOL repeat = s_down[vk] != 0;
        s_down[vk] = 1;
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
        if (repeat) {
            owner = s_owner[vk];
            return owner != OWN_PASS && owner != OWN_NONE;
        }
        return handle_down(vk);
    }
    if (!s_down[vk])
        return FALSE;
    s_down[vk] = 0;
    return handle_up(vk);
}

static LRESULT CALLBACK kb_proc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lParam;
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

static void suspend_input(void)
{
    leave_mouse_mode();
    reset_key_state();
    uninstall_hook();
}

static void resume_input(void)
{
    reset_key_state();
    uninstall_hook();
    if (!install_hook())
        app_log(L"input: keyboard hook reinstall failed");
}

static int round_i(double value)
{
    return (int)floor(value + 0.5);
}

static double scroll_speed_for(LONG flags, int idx, double base, double maximum,
                               double accel_time, double curve)
{
    double elapsed;
    double t;
    if (!(flags & (LONG)(1 << idx)))
        return 0.0;
    elapsed = (double)(DWORD)(GetTickCount() - s_scroll_start[idx]);
    if (elapsed < SCROLL_HOLD_DELAY_MS)
        return 0.0;
    t = (elapsed - SCROLL_HOLD_DELAY_MS) / 1000.0 / accel_time;
    if (t < 0.0)
        t = 0.0;
    if (t > 1.0)
        t = 1.0;
    return base + (maximum - base) * pow(t, curve);
}

static DWORD WINAPI movement_thread(LPVOID param)
{
    LARGE_INTEGER freq, prev, now;
    double velX = 0.0, velY = 0.0;
    double accX = 0.0, accY = 0.0;
    double holdX = 0.0, holdY = 0.0;
    double scrAccX = 0.0, scrAccY = 0.0;
    int prevSignX = 0, prevSignY = 0;
    BOOL timing = FALSE;

    (void)param;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    for (;;) {
        if (flag_get(&s_move_exit))
            break;
        if (!flag_get(&s_move_flags) && !flag_get(&s_scroll_flags)) {
            WaitForSingleObject(s_move_event, INFINITE);
            if (flag_get(&s_move_exit))
                break;
            if (!flag_get(&s_move_flags) && !flag_get(&s_scroll_flags))
                continue;
        }

        timeBeginPeriod(1);
        timing = TRUE;
        velX = velY = accX = accY = 0.0;
        holdX = holdY = 0.0;
        scrAccX = scrAccY = 0.0;
        prevSignX = prevSignY = 0;
        QueryPerformanceCounter(&prev);

        while (!flag_get(&s_move_exit)) {
            LONG mf = InterlockedCompareExchange(&s_move_flags, 0, 0);
            LONG sf = InterlockedCompareExchange(&s_scroll_flags, 0, 0);
            double dt, remaining, accumDX = 0.0, accumDY = 0.0;
            double speed, max_speed, accel_time, curve, slow_speed;
            double scroll_base, scroll_max;
            int signX, signY;
            BOOL slow;

            if (!mf && !sf)
                break;

            QueryPerformanceCounter(&now);
            dt = (double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart;
            prev = now;
            if (dt <= 0.0)
                dt = 0.001;
            if (dt > SUSPEND_RESET_SEC) {
                velX = velY = accX = accY = 0.0;
                holdX = holdY = 0.0;
                scrAccX = scrAccY = 0.0;
                prevSignX = prevSignY = 0;
                Sleep(1);
                continue;
            }

            signX = ((mf & MOVE_RIGHT) ? 1 : 0) - ((mf & MOVE_LEFT) ? 1 : 0);
            signY = ((mf & MOVE_DOWN) ? 1 : 0) - ((mf & MOVE_UP) ? 1 : 0);
            if (prevSignX != 0 && signX == 0) {
                velX = accX = holdX = 0.0;
            }
            if (prevSignY != 0 && signY == 0) {
                velY = accY = holdY = 0.0;
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
            if (accel_time <= 0.001)
                accel_time = 0.001;
            if (curve <= 0.0)
                curve = 1.0;
            if (max_speed < speed)
                max_speed = speed;
            if (scroll_max < scroll_base)
                scroll_max = scroll_base;
            slow = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

            remaining = dt;
            while (remaining > 0.0) {
                double step = remaining > MAX_SIM_STEP_SEC ? MAX_SIM_STEP_SEC : remaining;
                double targetX, targetY;
                double moveX, moveY;
                int dx, dy;

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
                    if (tX < 0.0) tX = 0.0;
                    if (tX > 1.0) tX = 1.0;
                    if (tY < 0.0) tY = 0.0;
                    if (tY > 1.0) tY = 1.0;
                    targetX = (double)signX *
                        (speed + (max_speed - speed) * pow(tX, curve));
                    targetY = (double)signY *
                        (speed + (max_speed - speed) * pow(tY, curve));
                    velX += (targetX - velX) * VELOCITY_SMOOTHING;
                    velY += (targetY - velY) * VELOCITY_SMOOTHING;
                }

                moveX = velX * step;
                moveY = velY * step;
                accX += moveX;
                accY += moveY;

                dx = round_i(accX);
                dy = round_i(accY);
                if (dx != 0 || dy != 0) {
                    accX -= dx;
                    accY -= dy;
                    accumDX += dx;
                    accumDY += dy;
                }
            }

            if ((int)accumDX != 0 || (int)accumDY != 0)
                send_relative_move((int)accumDX, (int)accumDY);

            if (sf) {
                double sx = 0.0, sy = 0.0;
                sy += scroll_speed_for(sf, 0, scroll_base, scroll_max, accel_time, curve);
                sy -= scroll_speed_for(sf, 1, scroll_base, scroll_max, accel_time, curve);
                sx -= scroll_speed_for(sf, 2, scroll_base, scroll_max, accel_time, curve);
                sx += scroll_speed_for(sf, 3, scroll_base, scroll_max, accel_time, curve);
                scrAccY += sy * dt;
                scrAccX += sx * dt;
                while (scrAccY >= 40.0 || scrAccY <= -40.0) {
                    int chunk = (int)scrAccY;
                    send_wheel(TRUE, chunk);
                    scrAccY -= chunk;
                }
                while (scrAccX >= 40.0 || scrAccX <= -40.0) {
                    int chunk = (int)scrAccX;
                    send_wheel(FALSE, chunk);
                    scrAccX -= chunk;
                }
            } else {
                scrAccX = scrAccY = 0.0;
            }

            Sleep(1);
        }

        if (timing) {
            timeEndPeriod(1);
            timing = FALSE;
        }
    }
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
        if (wp == (WPARAM)WTS_SESSION_LOCK)
            suspend_input();
        else if (wp == (WPARAM)WTS_SESSION_UNLOCK)
            resume_input();
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
    leave_mouse_mode();
    uninstall_hook();
    stop_movement_thread();
    if (s_ind_timer_on) {
        KillTimer(s_ind_wnd, IND_TIMER);
        s_ind_timer_on = FALSE;
    }
    if (s_ind_wnd) {
        DestroyWindow(s_ind_wnd);
        s_ind_wnd = NULL;
    }
    if (s_power_wnd) {
        DestroyWindow(s_power_wnd);
        s_power_wnd = NULL;
    }
}

static DWORD WINAPI input_thread_proc(LPVOID param)
{
    MSG msg;
    int result;

    (void)param;
    s_input_tid = GetCurrentThreadId();
    create_power_window();
    create_indicator_window();
    start_movement_thread();

    s_started_ok = install_hook();
    if (s_ready_event)
        SetEvent(s_ready_event);
    if (!s_started_ok) {
        cleanup_input();
        s_input_tid = 0;
        return 1;
    }

    while ((result = GetMessageW(&msg, NULL, 0, 0)) > 0) {
        if (msg.hwnd == NULL) {
            if (msg.message == WM_INPUT_SET_MODE) {
                set_mouse_mode(msg.wParam != 0);
                continue;
            }
            if (msg.message == WM_INPUT_SET_PAUSED) {
                if (msg.wParam)
                    leave_mouse_mode();
                continue;
            }
            if (msg.message == WM_INPUT_CONFIG ||
                msg.message == WM_INPUT_IND_UPDATE) {
                indicator_update();
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

void input_config_changed(void)
{
    if (s_input_tid)
        PostThreadMessageW(s_input_tid, WM_INPUT_CONFIG, 0, 0);
}
