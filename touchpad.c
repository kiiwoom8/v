// touchpad.c - the touchpad is off while vim mouse's keys steer (touchpad_off).
//
// Main/UI thread only. A scroll key (d u j k, h l sideways, g g / Shift + g,
// Alt scrolling) or a pointer-move key (mouse mode) switches the precision
// touchpad off, so a palm resting on it does not move the pointer the keys
// are aiming with (scrolls go to the window under it). input.c posts
// WM_APP_TOUCHPAD_KEY at each press (at most every 250 ms). touchpad_back_ms
// (10 s) after the last such key, counted from its release when held, the
// touchpad goes back to what it was: on if it was on, and a touchpad that
// was already off when the keys came is left off (s_ours stays FALSE). So
// the touchpad works with the keyboard at any other time. Pausing, locking
// the keyboard (lock_hotkey), quitting or turning touchpad_off to 0 gives
// it back at once.
//
// Reading the state: SystemParametersInfo(SPI_GETTOUCHPADPARAMETERS)
// (Windows 11 24H2 and later). Writing it: SPI_SETTOUCHPADPARAMETERS changes
// tap and the other options but ignores touchpadEnabled (measured on build
// 26300: the call succeeds and nothing changes), so the switch is
// Ctrl+Win+F24, the shortcut a laptop's touchpad key sends, which flips the
// same setting as the Settings app's toggle (and its registry value). It is
// a toggle, so we read the state first and press only when it differs, and
// read it again afterwards. The shortcut is injected only while no modifier
// is physically down (never mix injected modifiers with held ones; the
// hook's key state counts too, since the lock swallows the unlock chord's
// Ctrl and Alt before GetAsyncKeyState sees them), in one
// SendInput call so no real key lands inside it; our hook passes injected
// keys untouched. So Alt scrolling turns it off once Alt is let go. Where it
// cannot work (no precision touchpad, older Windows, an elevated window in
// front swallowing the keys) we log and give up until the next key.
//
// Waking from sleep (Modern Standby) can bring the touchpad back working
// although Windows still reads it as off: the device resets and the off
// setting is not sent to it again. So after a wake, an unlock or the
// display coming back on (input.c posts WM_APP_TOUCHPAD_WAKE) while it is
// off for the keys, once the lock screen is gone, touchpad_wake switches it
// on and off again, which sends the setting anew; if it reads on, one press
// turns it off.
//
// The setting persists like the Settings toggle, so "we turned it off" is
// kept in HKCU\Software\vim mouse (TouchpadTurnedOff): if vim mouse is killed
// or Windows shuts down inside the off window, the next start turns it back
// on.

#include "common.h"

#ifndef SPI_GETTOUCHPADPARAMETERS
#define SPI_GETTOUCHPADPARAMETERS 0x00AE
#endif

// TOUCHPAD_PARAMETERS (version 1), declared by hand for older headers. The
// bitfields are spelled as words and masks so the layout cannot depend on
// the compiler: first word bit 0 touchpadPresent, bit 3 touchpadEnabled.
typedef struct {
    UINT versionNumber;
    UINT maxSupportedContacts;
    int  legacyTouchpadFeatures;
    UINT flags1;
    UINT flags2;
    int  sensitivityLevel;
    UINT cursorSpeed;
    UINT feedbackIntensity;
    UINT clickForceSensitivity;
    UINT rightClickZoneWidth;
    UINT rightClickZoneHeight;
} TouchpadParams;

#define TP_PRESENT 0x1u
#define TP_ENABLED 0x8u

#define TP_TIMER_MS     50     // modifiers and the result are checked this often
#define TP_CONFIRM_MS   600    // after a press, how long the state may take to flip
#define TP_WAIT_MAX_MS  15000  // how long we wait for the modifiers to be released
#define TP_PRESSES      2      // presses before giving up
#define TP_QUIT_WAIT_MS 2000   // at quit, the wait for the modifiers to be released
#define TP_WAKE_DELAY_MS 2000  // after a wake, how long the device gets to settle first
#define TP_HELD_POLL_MS 250    // while a scroll or move key is held, checked this often
#define TP_HELD_MAX_MS  60000  // a key read as held this long after its press is not trusted
                               // (a release the hook never saw)

#define TP_REG_KEY   L"Software\\vim mouse"
#define TP_REG_VALUE L"TouchpadTurnedOff"

enum { GOAL_NONE = 0, GOAL_OFF = 1, GOAL_ON = 2 };

static int      s_goal = GOAL_NONE;  // what we are bringing about (or brought about)
static BOOL     s_busy = FALSE;      // the timer runs until s_goal holds or we give up
static BOOL     s_ours = FALSE;      // the touchpad is off because we pressed it off
static UINT_PTR s_timer = 0;
static DWORD    s_start = 0;         // when the current goal was set
static DWORD    s_pressed = 0;       // when we last pressed the shortcut (0 = not yet)
static int      s_presses = 0;
static BOOL     s_settle = FALSE;    // a press for the previous goal may still flip it
static DWORD    s_settle_until = 0;
static BOOL     s_unsupported_logged = FALSE;
static BOOL     s_resync = FALSE;    // after a wake: going on now, then off again
static UINT_PTR s_wake_timer = 0;
static BOOL     s_active = FALSE;    // the keys steer: off until touchpad_back_ms after the last
static DWORD    s_last_key = 0;      // the last key report, or the last check that saw one held
static DWORD    s_key_report = 0;    // the last key report alone
static UINT_PTR s_back_timer = 0;
static BOOL     s_loaded = FALSE;    // the registry mark was read
static int      s_mark = -1;         // what the registry holds (-1 = not known)

// -1 = unknown (no precision touchpad, or Windows before 11 24H2), 0 = off, 1 = on.
static int tp_state(void)
{
    TouchpadParams p;

    ZeroMemory(&p, sizeof(p));
    p.versionNumber = 1;
    if (!SystemParametersInfoW(SPI_GETTOUCHPADPARAMETERS, sizeof(p), &p, 0) ||
        !(p.flags1 & TP_PRESENT))
        return -1;
    return (p.flags1 & TP_ENABLED) ? 1 : 0;
}

static void mark_load(void)
{
    HKEY key;
    DWORD val = 0, size = sizeof(val), type = 0;

    s_loaded = TRUE;
    s_mark = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, TP_REG_KEY, 0, KEY_QUERY_VALUE,
                      &key) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExW(key, TP_REG_VALUE, NULL, &type, (BYTE *)&val,
                         &size) == ERROR_SUCCESS && type == REG_DWORD && val)
        s_mark = 1;
    RegCloseKey(key);
    if (s_mark) {
        s_ours = TRUE;
        app_log(L"touchpad: vim mouse had turned it off and did not get to "
                L"turn it back on; turning it on");
    }
}

static void set_ours(BOOL ours)
{
    HKEY key;
    DWORD val = ours ? 1 : 0;

    s_ours = ours;
    if (s_mark == (int)val)
        return;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, TP_REG_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
        return;
    if (RegSetValueExW(key, TP_REG_VALUE, 0, REG_DWORD, (const BYTE *)&val,
                       sizeof(val)) == ERROR_SUCCESS)
        s_mark = (int)val;
    RegCloseKey(key);
}

static BOOL modifiers_down(void)
{
    static const int vks[] = {
        VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN
    };
    size_t i;

    for (i = 0; i < sizeof(vks) / sizeof(vks[0]); i++)
        if (GetAsyncKeyState(vks[i]) & 0x8000)
            return TRUE;
    return input_modifier_held();
}

static void key_input(INPUT *in, WORD vk, DWORD flags)
{
    ZeroMemory(in, sizeof(*in));
    in->type = INPUT_KEYBOARD;
    in->ki.wVk = vk;
    in->ki.dwFlags = flags;
}

// Ctrl+Win+F24 in one SendInput call, so no real key lands between them.
static BOOL press_toggle(void)
{
    INPUT in[6];

    key_input(&in[0], VK_LCONTROL, 0);
    key_input(&in[1], VK_LWIN, 0);
    key_input(&in[2], VK_F24, 0);
    key_input(&in[3], VK_F24, KEYEVENTF_KEYUP);
    key_input(&in[4], VK_LWIN, KEYEVENTF_KEYUP);
    key_input(&in[5], VK_LCONTROL, KEYEVENTF_KEYUP);
    if (SendInput(6, in, sizeof(INPUT)) != 6) {
        app_log(L"touchpad: SendInput failed (%lu)", GetLastError());
        return FALSE;
    }
    return TRUE;
}

static void stop_timer(void)
{
    if (s_timer) {
        KillTimer(NULL, s_timer);
        s_timer = 0;
    }
    s_busy = FALSE;
}

static void restart(int goal);

// One step towards s_goal. Returns FALSE once there is nothing more to do.
static BOOL step(void)
{
    int want = s_goal == GOAL_OFF ? 0 : 1;
    int state;
    DWORD now = GetTickCount();

    // A press made for the goal before this one may not have flipped it
    // yet: reading now would see the old state and press it back.
    if (s_settle) {
        if ((LONG)(now - s_settle_until) < 0)
            return TRUE;
        s_settle = FALSE;
    }
    state = tp_state();
    if (state < 0) {
        if (!s_unsupported_logged) {
            app_log(L"touchpad: no precision touchpad state to read "
                    L"(none, or Windows before 11 24H2); touchpad_off does nothing");
            s_unsupported_logged = TRUE;
        }
        return FALSE;
    }
    if (state == want) {
        // Off by our press is ours; off already (the user's, or still ours
        // from before) stays as it was. On, by whoever, is nobody's.
        if (s_goal == GOAL_ON)
            set_ours(FALSE);
        else if (s_presses > 0)
            set_ours(TRUE);
        if (g_cfg.log && s_presses > 0)
            app_log(L"touchpad: %s", want ? L"on" : L"off");
        if (s_resync && s_goal == GOAL_ON) {
            // Halfway through the re-sync after a wake: off again, after
            // TP_CONFIRM_MS so the device has taken the "on".
            s_resync = FALSE;
            s_goal = GOAL_OFF;
            s_start = now;
            s_pressed = now ? now : 1;
            s_presses = 0;
            set_ours(TRUE);
            return TRUE;
        }
        return FALSE;
    }
    if (s_pressed && now - s_pressed < TP_CONFIRM_MS)
        return TRUE;  // give the last press time to take effect
    if (s_presses >= TP_PRESSES) {
        app_log(L"touchpad: Ctrl+Win+F24 did not turn it %s",
                want ? L"on" : L"off");
        return FALSE;
    }
    if (modifiers_down()) {
        if (now - s_start > TP_WAIT_MAX_MS) {
            app_log(L"touchpad: a modifier stayed down; not turned %s",
                    want ? L"on" : L"off");
            return FALSE;
        }
        return TRUE;
    }
    s_presses++;
    s_pressed = now ? now : 1;
    if (s_goal == GOAL_OFF)
        set_ours(TRUE);  // marked before it flips, so a kill right now still gives it back
    if (!press_toggle())
        return FALSE;
    return TRUE;
}

// Done with a goal (reached or given up). A re-sync given up halfway leaves
// the goal off: that is still what we want, tried again at the next key.
static void finish(void)
{
    stop_timer();
    if (s_resync) {
        s_resync = FALSE;
        s_goal = GOAL_OFF;
    }
}

static void CALLBACK tp_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick)
{
    (void)hwnd;
    (void)msg;
    (void)id;
    (void)tick;
    if (!step())
        finish();
}

static void set_goal(int goal)
{
    if (goal == s_goal) {
        // Paused during a re-sync's "on" half: stay on.
        s_resync = FALSE;
        return;
    }
    s_resync = FALSE;
    restart(goal);
}

// A new goal; a press still flipping the touchpad for the old one is waited out.
static void begin_goal(int goal)
{
    DWORD now = GetTickCount();

    s_goal = goal;
    s_settle = s_pressed && now - s_pressed < TP_CONFIRM_MS;
    s_settle_until = s_pressed + TP_CONFIRM_MS;
    s_start = now;
    s_pressed = 0;
    s_presses = 0;
}

static void restart(int goal)
{
    stop_timer();
    if (goal == GOAL_NONE) {
        s_goal = GOAL_NONE;
        return;
    }
    begin_goal(goal);
    if (!step()) {
        finish();
        return;
    }
    s_timer = SetTimer(NULL, 0, TP_TIMER_MS, tp_timer_proc);
    s_busy = s_timer != 0;
    if (!s_timer)
        app_log(L"touchpad: SetTimer failed (%lu)", GetLastError());
}

static void stop_back_timer(void)
{
    if (s_back_timer) {
        KillTimer(NULL, s_back_timer);
        s_back_timer = 0;
    }
}

void touchpad_update(void)
{
    int goal;

    if (!s_loaded)
        mark_load();
    if (!g_cfg.touchpad_off || input_is_paused() || input_is_locked()) {
        s_active = FALSE;
        stop_back_timer();
    }
    if (s_active)
        goal = GOAL_OFF;
    else
        goal = (s_ours || (s_busy && s_goal == GOAL_OFF)) ? GOAL_ON : GOAL_NONE;
    set_goal(goal);
}

static void CALLBACK back_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick);

static void arm_back_timer(DWORD ms)
{
    stop_back_timer();
    s_back_timer = SetTimer(NULL, 0, ms, back_timer_proc);
    if (!s_back_timer) {
        app_log(L"touchpad: SetTimer failed (%lu)", GetLastError());
        s_active = FALSE;  // never leave it off with nothing to bring it back
    }
}

// touchpad_back_ms after the last key, or after a held one is let go.
static void CALLBACK back_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick)
{
    DWORD now = GetTickCount();
    DWORD back = (DWORD)g_cfg.touchpad_back_ms;
    DWORD gone;

    (void)hwnd;
    (void)msg;
    (void)id;
    (void)tick;
    stop_back_timer();
    if (!s_active)
        return;
    if (input_keys_steering() && now - s_key_report < TP_HELD_MAX_MS) {
        s_last_key = now;
        arm_back_timer(TP_HELD_POLL_MS);
        if (s_back_timer)
            return;
    }
    gone = now - s_last_key;
    if (s_active && gone < back) {
        arm_back_timer(back - gone);
        if (s_back_timer)
            return;
    }
    s_active = FALSE;
    if (g_cfg.log && (s_ours || s_busy))
        app_log(L"touchpad: no scroll or move key for %lu ms; back on",
                (unsigned long)gone);
    touchpad_update();
}

void touchpad_key(void)
{
    if (!s_loaded)
        mark_load();
    if (!g_cfg.touchpad_off || input_is_paused() || input_is_locked())
        return;
    s_last_key = GetTickCount();
    s_key_report = s_last_key;
    s_active = TRUE;
    if (!s_back_timer)
        arm_back_timer((DWORD)g_cfg.touchpad_back_ms);
    if (!s_active)
        return;
    if (s_goal == GOAL_OFF) {
        // Off already, or on its way. Pressed back on by hand inside the
        // window, or given up on (Alt held through Alt scrolling for
        // TP_WAIT_MAX_MS): off again.
        if (!s_busy && !s_resync && tp_state() == 1)
            restart(GOAL_OFF);
        return;
    }
    touchpad_update();
}

// The lock screen (another desktop) takes no injected keys; its unlock
// posts WM_APP_TOUCHPAD_WAKE again.
static BOOL desktop_is_ours(void)
{
    HDESK desk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);

    if (!desk)
        return FALSE;
    CloseDesktop(desk);
    return TRUE;
}

static void CALLBACK wake_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick)
{
    int state;

    (void)hwnd;
    (void)msg;
    (void)id;
    (void)tick;
    if (s_wake_timer) {
        KillTimer(NULL, s_wake_timer);
        s_wake_timer = 0;
    }
    // Only while it is off for the keys and was ours to turn off.
    if (!g_cfg.touchpad_off || !s_active || s_goal != GOAL_OFF || !s_ours ||
        s_busy || input_is_paused() || input_is_locked())
        return;
    if (!desktop_is_ours()) {
        app_log(L"touchpad: woke on the lock screen; checked again after unlocking");
        return;
    }
    state = tp_state();
    if (state < 0)
        return;
    app_log(L"touchpad: after a wake it reads %s; switching it %s",
            state ? L"on" : L"off", state ? L"off" : L"on and off again");
    s_resync = state == 0;
    restart(s_resync ? GOAL_ON : GOAL_OFF);
}

void touchpad_wake(void)
{
    if (!g_cfg.touchpad_off)
        return;
    if (s_wake_timer)
        KillTimer(NULL, s_wake_timer);
    s_wake_timer = SetTimer(NULL, 0, TP_WAKE_DELAY_MS, wake_timer_proc);
}

void touchpad_stop(void)
{
    BOOL was_off = s_ours || (s_busy && s_goal == GOAL_OFF) || s_resync;
    DWORD start = GetTickCount();

    if (s_wake_timer) {
        KillTimer(NULL, s_wake_timer);
        s_wake_timer = 0;
    }
    stop_back_timer();
    s_active = FALSE;
    s_resync = FALSE;
    stop_timer();
    if (!was_off)
        return;
    // Quitting: the quit hotkey's Ctrl+Alt may still be down. The input
    // thread is gone by now, so waiting here blocks no hook. Not turned on,
    // the registry mark stays and the next start tries again.
    begin_goal(GOAL_ON);
    s_start = start;
    while (step()) {
        if (GetTickCount() - start > TP_QUIT_WAIT_MS + TP_CONFIRM_MS * 2) {
            app_log(L"touchpad: not turned back on at quit");
            break;
        }
        Sleep(20);
    }
}
