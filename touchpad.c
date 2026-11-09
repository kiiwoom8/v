// touchpad.c - the touchpad is off while vim mouse is active (touchpad_off).
//
// Main/UI thread only. While vim mouse runs and is not paused the precision
// touchpad is switched off; pausing, locking the keyboard (lock_hotkey),
// quitting or turning touchpad_off to 0 switches it back on, and resuming
// or unlocking switches it off again.
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
// keys untouched. Where it cannot work (no precision touchpad, older
// Windows, an elevated window in front swallowing the keys) we log and give
// up until the next change.
//
// Waking from sleep (Modern Standby) can bring the touchpad back working
// although Windows still reads it as off: the device resets and the off
// setting is not sent to it again. So after a wake, an unlock or the
// display coming back on (input.c posts WM_APP_TOUCHPAD_WAKE), once the
// lock screen is gone, touchpad_wake switches it on and off again, which
// sends the setting anew; if it reads on, one press turns it off.
//
// The setting persists like the Settings toggle: if vim mouse is killed or
// Windows shuts down while it is off, it stays off until vim mouse runs and
// pauses or quits, or the user switches it on.

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

enum { GOAL_NONE = 0, GOAL_OFF = 1, GOAL_ON = 2 };

static int      s_goal = GOAL_NONE;  // what we are bringing about (or brought about)
static BOOL     s_busy = FALSE;      // the timer runs until s_goal holds or we give up
static BOOL     s_ours = FALSE;      // the touchpad is off because touchpad_off is on
static UINT_PTR s_timer = 0;
static DWORD    s_start = 0;         // when the current goal was set
static DWORD    s_pressed = 0;       // when we last pressed the shortcut (0 = not yet)
static int      s_presses = 0;
static BOOL     s_unsupported_logged = FALSE;
static BOOL     s_resync = FALSE;    // after a wake: going on now, then off again
static UINT_PTR s_wake_timer = 0;

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
    int state = tp_state();
    DWORD now = GetTickCount();

    if (state < 0) {
        if (!s_unsupported_logged) {
            app_log(L"touchpad: no precision touchpad state to read "
                    L"(none, or Windows before 11 24H2); touchpad_off does nothing");
            s_unsupported_logged = TRUE;
        }
        return FALSE;
    }
    if (state == want) {
        s_ours = s_goal == GOAL_OFF;
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
    if (!press_toggle())
        return FALSE;
    return TRUE;
}

// Done with a goal (reached or given up). A re-sync given up halfway leaves
// the goal off: that is still what we want, tried again at the next change.
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

static void restart(int goal)
{
    s_goal = goal;
    stop_timer();
    if (goal == GOAL_NONE)
        return;
    s_start = GetTickCount();
    s_pressed = 0;
    s_presses = 0;
    if (!step()) {
        finish();
        return;
    }
    s_timer = SetTimer(NULL, 0, TP_TIMER_MS, tp_timer_proc);
    s_busy = s_timer != 0;
    if (!s_timer)
        app_log(L"touchpad: SetTimer failed (%lu)", GetLastError());
}

void touchpad_update(void)
{
    int goal;

    if (g_cfg.touchpad_off)
        goal = (input_is_paused() || input_is_locked()) ? GOAL_ON : GOAL_OFF;
    else
        goal = (s_ours || (s_busy && s_goal == GOAL_OFF)) ? GOAL_ON : GOAL_NONE;
    set_goal(goal);
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
    if (!g_cfg.touchpad_off || s_goal != GOAL_OFF || s_busy ||
        input_is_paused() || input_is_locked())
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
    s_resync = FALSE;
    stop_timer();
    if (!was_off)
        return;
    // Quitting: the quit hotkey's Ctrl+Alt may still be down. The input
    // thread is gone by now, so waiting here blocks no hook.
    s_goal = GOAL_ON;
    s_start = start;
    s_pressed = 0;
    s_presses = 0;
    while (step()) {
        if (GetTickCount() - start > TP_QUIT_WAIT_MS + TP_CONFIRM_MS) {
            app_log(L"touchpad: not turned back on at quit");
            break;
        }
        Sleep(20);
    }
    s_ours = FALSE;
}
