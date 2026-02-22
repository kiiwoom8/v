// mouse_control_windows_hook_accel.c
// gcc mouse_control_windows_hook_accel.c -o mouse_control -lgdi32 -luser32

#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>

#define BASE_SPEED_PX_PER_SEC  500.0
#define MAX_SPEED_PX_PER_SEC   1500.0
#define ACCEL_TIME_SEC         0.65
#define ACCEL_CURVE            1.8
#define VELOCITY_SMOOTHING     0.12
#define THREAD_SLEEP_MS_ACTIVE 1
#define THREAD_SLEEP_MS_IDLE   6

// Globals
HHOOK kbHook = NULL;
volatile BOOL g_run = TRUE;
volatile BOOL g_h = FALSE, g_l = FALSE, g_j = FALSE, g_k = FALSE;
volatile BOOL g_dragging = FALSE;      // true when left button is held down (A pressed)
volatile BOOL g_exit = FALSE;
volatile BOOL g_reset_state = FALSE;   // set by main thread (resume) to cause movement thread to reset
volatile BOOL g_capsOn = FALSE;        // tracked CapsLock state

// Fractional accumulators
double accX = 0.0, accY = 0.0;

// For message-only window
HWND g_message_hwnd = NULL;

// Logging helper
static void log_msg(const char *fmt, ...) {
    FILE *f = fopen("mouse_control.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    time_t t = time(NULL);
    struct tm tmv;
    struct tm *tmtmp = localtime(&t);
    if (tmtmp) tmv = *tmtmp; else tmv = (struct tm){0};
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(f, "[%s] ", buf);
    vfprintf(f, fmt, ap);
    fprintf(f, "\n");
    va_end(ap);
    fclose(f);
}

// Input helpers
static void send_relative_move(int dx, int dy) {
    INPUT inp;
    ZeroMemory(&inp, sizeof(inp));
    inp.type = INPUT_MOUSE;
    inp.mi.dx = dx;
    inp.mi.dy = dy;
    inp.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &inp, sizeof(inp));
}

static void send_click(BOOL left) {
    INPUT in[2];
    ZeroMemory(in, sizeof(in));
    in[0].type = in[1].type = INPUT_MOUSE;
    if (left) {
        in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    } else {
        in[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
        in[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    }
    SendInput(2, in, sizeof(INPUT));
}

static void send_left_down(void) {
    if (g_dragging) return;
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &in, sizeof(in));
    g_dragging = TRUE;
}

static void send_left_up(void) {
    if (!g_dragging) return;
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(in));
    g_dragging = FALSE;
}

// Hook install/uninstall helpers
static void install_hook();
static void uninstall_hook();

// Forward declaration for keyboard proc
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

static void install_hook() {
    if (kbHook) return;
    // Use module handle for robustness
    HMODULE mod = GetModuleHandle(NULL);
    kbHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, mod, 0);
    if (!kbHook) {
        log_msg("SetWindowsHookEx FAILED err=%lu", GetLastError());
    } else {
        log_msg("Keyboard hook installed");
    }
}

static void uninstall_hook() {
    if (!kbHook) return;
    if (UnhookWindowsHookEx(kbHook)) {
        log_msg("Keyboard hook uninstalled");
    } else {
        log_msg("UnhookWindowsHookEx FAILED err=%lu", GetLastError());
    }
    kbHook = NULL;
}

// Movement thread: calculates speed & sends mouse movements.
// It checks g_reset_state to immediately clear velocities/accumulators when needed.
DWORD WINAPI MovementThread(LPVOID lpv) {
    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    double velX = 0.0, velY = 0.0;
    double holdTimeX = 0.0, holdTimeY = 0.0;
    int prevSignX = 0, prevSignY = 0;

    log_msg("MovementThread started");

    while (!g_exit) {
        if (g_reset_state) {
            // Clear movement state immediately
            velX = velY = 0.0;
            holdTimeX = holdTimeY = 0.0;
            accX = accY = 0.0;
            prevSignX = prevSignY = 0;
            g_dragging = FALSE; // ensure no stuck dragging state
            g_reset_state = FALSE;
            log_msg("MovementThread: state reset");
        }

        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart;
        if (dt <= 0.0) dt = 1.0/1000.0;
        prev = now;

        int signX = (g_l ? 1 : 0) - (g_h ? 1 : 0);
        int signY = (g_j ? 1 : 0) - (g_k ? 1 : 0);

        if (prevSignX != 0 && signX == 0) {
            velX = 0.0;
            accX = 0.0;
            holdTimeX = 0.0;
        }
        if (prevSignY != 0 && signY == 0) {
            velY = 0.0;
            accY = 0.0;
            holdTimeY = 0.0;
        }
        prevSignX = signX;
        prevSignY = signY;

        if (signX != 0) holdTimeX += dt; else holdTimeX = 0.0;
        if (signY != 0) holdTimeY += dt; else holdTimeY = 0.0;

        double tX = holdTimeX / ACCEL_TIME_SEC;
        if (tX < 0.0) tX = 0.0;
        if (tX > 1.0) tX = 1.0;
        double tY = holdTimeY / ACCEL_TIME_SEC;
        if (tY < 0.0) tY = 0.0;
        if (tY > 1.0) tY = 1.0;

        double easeX = pow(tX, ACCEL_CURVE);
        double easeY = pow(tY, ACCEL_CURVE);

        double targetSpeedX = (double)signX * (BASE_SPEED_PX_PER_SEC + (MAX_SPEED_PX_PER_SEC - BASE_SPEED_PX_PER_SEC) * easeX);
        double targetSpeedY = (double)signY * (BASE_SPEED_PX_PER_SEC + (MAX_SPEED_PX_PER_SEC - BASE_SPEED_PX_PER_SEC) * easeY);

        velX += (targetSpeedX - velX) * VELOCITY_SMOOTHING;
        velY += (targetSpeedY - velY) * VELOCITY_SMOOTHING;

        double moveX = velX * dt;
        double moveY = velY * dt;

        accX += moveX;
        accY += moveY;
        int dx = (int)round(accX);
        int dy = (int)round(accY);

        if (dx != 0 || dy != 0) {
            accX -= dx;
            accY -= dy;
            send_relative_move(dx, dy);
        }

        if (signX == 0 && signY == 0) {
            Sleep(THREAD_SLEEP_MS_IDLE);
        } else {
            Sleep(THREAD_SLEEP_MS_ACTIVE);
        }
    }

    log_msg("MovementThread exiting");
    return 0;
}

// Low-level keyboard proc
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lParam;

        // Track CapsLock toggling by flipping internal boolean on keydown of VK_CAPITAL.
        if (k->vkCode == VK_CAPITAL && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
            // toggle our tracked caps state - this assumes CapsLock toggles when the keydown occurs.
            g_capsOn = !g_capsOn;
            log_msg("CapsLock toggled (tracked) -> %d", (int)g_capsOn);
            // Do not swallow the VK_CAPITAL event; let it pass so system toggles real state.
            return CallNextHookEx(kbHook, nCode, wParam, lParam);
        }

        // Key down
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            if (g_capsOn) {
                switch (k->vkCode) {
                    case 'H':
                        g_h = TRUE;
                        return 1;
                    case 'L':
                        g_l = TRUE;
                        return 1;
                    case 'J':
                        g_j = TRUE;
                        return 1;
                    case 'K':
                        g_k = TRUE;
                        return 1;
                    case 'A':
                        if (!g_dragging) send_left_down();
                        return 1;
                    case 'S':
                        send_click(FALSE);
                        return 1;
                }
            }
        }
        // Key up - ALWAYS clear internal flags so they can't get stuck.
        else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            switch (k->vkCode) {
                case 'H':
                    if (g_h) { g_h = FALSE; return 1; }
                    break;
                case 'L':
                    if (g_l) { g_l = FALSE; return 1; }
                    break;
                case 'J':
                    if (g_j) { g_j = FALSE; return 1; }
                    break;
                case 'K':
                    if (g_k) { g_k = FALSE; return 1; }
                    break;
                case 'A': {
                    BOOL wasDragging = g_dragging;
                    if (wasDragging) send_left_up();
                    if (wasDragging) return 1;
                    break;
                }
            }
        }
    }
    return CallNextHookEx(kbHook, nCode, wParam, lParam);
}


// Window procedure for the message-only window. We'll receive WM_POWERBROADCAST here.
LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMSUSPEND) {
                log_msg("WM_POWERBROADCAST: PBT_APMSUSPEND");
                // On suspend, nothing special here; movement thread will stop while suspended.
            } else if (wParam == PBT_APMRESUMECRITICAL || wParam == PBT_APMRESUMEAUTOMATIC) {
                log_msg("WM_POWERBROADCAST: Resume - will reinstall hook and reset state");
                // Uninstall & reinstall hook to be safe
                uninstall_hook();
                install_hook();
                // Reset movement state
                g_reset_state = TRUE;
                // Clear input flags
                g_h = g_l = g_j = g_k = FALSE;
                g_dragging = FALSE;
            }
            return TRUE;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int main(void) {
    // Initialize logging
    log_msg("Program start");

    // Read initial CapsLock state from system
    g_capsOn = (GetKeyState(VK_CAPITAL) & 1) != 0;
    log_msg("Initial CapsLock state = %d", (int)g_capsOn);

    // Create a message-only window to receive power broadcasts
    WNDCLASS wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "MouseControlMsgWndClass";
    if (!RegisterClass(&wc)) {
        log_msg("RegisterClass FAILED err=%lu", GetLastError());
        fprintf(stderr, "RegisterClass failed\n");
        return 1;
    }

    // Create a message-only window (HWND_MESSAGE)
    g_message_hwnd = CreateWindowEx(0, wc.lpszClassName, "mouse_control_msgwnd",
                                    0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!g_message_hwnd) {
        log_msg("CreateWindowEx FAILED err=%lu", GetLastError());
        fprintf(stderr, "CreateWindowEx failed\n");
        return 1;
    }

    // Install hook
    install_hook();
    if (!kbHook) {
        fprintf(stderr, "Failed to install keyboard hook. See mouse_control.log for details.\n");
        // continue anyway? safer to exit
        return 1;
    }

    // Start movement thread
    HANDLE hThread = CreateThread(NULL, 0, MovementThread, NULL, 0, NULL);
    if (!hThread) {
        log_msg("Failed to create movement thread err=%lu", GetLastError());
        fprintf(stderr, "Failed to create movement thread.\n");
        uninstall_hook();
        return 1;
    }

    // Message loop (for our message-only window)
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup
    log_msg("Main loop exiting, cleaning up");
    g_exit = TRUE;
    // ensure dragging stopped
    if (g_dragging) {
        send_left_up();
        g_dragging = FALSE;
    }
    // wait for movement thread to exit
    WaitForSingleObject(hThread, 2000);
    CloseHandle(hThread);
    uninstall_hook();

    DestroyWindow(g_message_hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);

    log_msg("Program exit");
    return 0;
}