// mouse_control_windows_hook_accel.c
// Compile (MinGW):
// gcc mouse_control_windows_hook_accel.c -o mouse_control -lgdi32 -luser32

#include <windows.h>
#include <stdio.h>
#include <math.h>

#define BASE_SPEED_PX_PER_SEC  500.0   // starting slow speed (px/s) - adjust
#define MAX_SPEED_PX_PER_SEC   1500.0  // max speed after full accel (px/s) - adjust
#define ACCEL_TIME_SEC         0.65    // how many seconds to reach (almost) max speed
#define ACCEL_CURVE            1.8     // >1: slower start, faster late; <1: fast start
#define VELOCITY_SMOOTHING     0.12    // 0..1, bigger = snappier changes (0.12 is gentle)
#define THREAD_SLEEP_MS_ACTIVE 1       // sleep while moving
#define THREAD_SLEEP_MS_IDLE   6       // sleep when idle

HHOOK kbHook;
volatile BOOL g_run = TRUE;
volatile BOOL g_h = FALSE, g_l = FALSE, g_j = FALSE, g_k = FALSE;
volatile BOOL g_exit = FALSE;
volatile BOOL g_dragging = FALSE;      // true when left button is held down (A pressed)

// Accumulators for fractional movement so small dt still moves smoothly
double accX = 0.0, accY = 0.0;

// Helpers -------------------------------------------------------------------
static void send_relative_move(int dx, int dy) {
    INPUT inp;
    ZeroMemory(&inp, sizeof(inp));
    inp.type = INPUT_MOUSE;
    inp.mi.dx = dx;
    inp.mi.dy = dy;
    inp.mi.dwFlags = MOUSEEVENTF_MOVE; // relative move
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

// Movement thread -----------------------------------------------------------
DWORD WINAPI MovementThread(LPVOID lpv) {
    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    double velX = 0.0, velY = 0.0;      // current smoothed velocities (px/s)
    double holdTimeX = 0.0, holdTimeY = 0.0; // seconds keys have been held

    int prevSignX = 0, prevSignY = 0; // track previous sign to detect transition to 0

    while (!g_exit) {
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart;
        if (dt <= 0.0) dt = 1.0/1000.0;
        prev = now;

        // direction signs (-1,0,1)
        int signX = (g_l ? 1 : 0) - (g_h ? 1 : 0);
        int signY = (g_j ? 1 : 0) - (g_k ? 1 : 0); // j -> down (+y), k -> up (-y)

        // If an axis just became idle (non-zero -> zero), immediately clear velocity + accumulator.
        // This prevents the "coast" caused by smoothing and fractional accumulators.
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

        // update hold timers (used to compute current target speed)
        if (signX != 0) holdTimeX += dt; else holdTimeX = 0.0;
        if (signY != 0) holdTimeY += dt; else holdTimeY = 0.0;

        // compute multiplier [0..1] for how far through accel we are
        double tX = holdTimeX / ACCEL_TIME_SEC;
        if (tX < 0.0) tX = 0.0;
        if (tX > 1.0) tX = 1.0;
        double tY = holdTimeY / ACCEL_TIME_SEC;
        if (tY < 0.0) tY = 0.0;
        if (tY > 1.0) tY = 1.0;

        // ease curve (pow) to shape ramp-up. ACCEL_CURVE >1 makes a gentle start.
        double easeX = pow(tX, ACCEL_CURVE);
        double easeY = pow(tY, ACCEL_CURVE);

        // current target speeds (px/s) for each axis
        double targetSpeedX = (double)signX * (BASE_SPEED_PX_PER_SEC + (MAX_SPEED_PX_PER_SEC - BASE_SPEED_PX_PER_SEC) * easeX);
        double targetSpeedY = (double)signY * (BASE_SPEED_PX_PER_SEC + (MAX_SPEED_PX_PER_SEC - BASE_SPEED_PX_PER_SEC) * easeY);

        // smooth current velocity toward target velocity for nice easing
        velX += (targetSpeedX - velX) * VELOCITY_SMOOTHING;
        velY += (targetSpeedY - velY) * VELOCITY_SMOOTHING;

        // compute movement this tick (px)
        double moveX = velX * dt;
        double moveY = velY * dt;

        // accumulate fractional and send integer deltas
        accX += moveX;
        accY += moveY;
        int dx = (int)round(accX);
        int dy = (int)round(accY);

        if (dx != 0 || dy != 0) {
            accX -= dx;
            accY -= dy;
            send_relative_move(dx, dy);
        }

        // release CPU differently when idle/active
        if (signX == 0 && signY == 0) {
            Sleep(THREAD_SLEEP_MS_IDLE);
        } else {
            Sleep(THREAD_SLEEP_MS_ACTIVE);
        }
    }

    return 0;
}


// Keyboard hook -------------------------------------------------------------
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lParam;
        BOOL capsOn = (GetKeyState(VK_CAPITAL) & 1) != 0;

        // Key down
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            switch (k->vkCode) {
                case 'H':
                    if (capsOn) { g_h = TRUE; return 1; }
                    break;
                case 'L':
                    if (capsOn) { g_l = TRUE; return 1; }
                    break;
                case 'J':
                    if (capsOn) { g_j = TRUE; return 1; }
                    break;
                case 'K':
                    if (capsOn) { g_k = TRUE; return 1; }
                    break;

                case 'A':
                    if (capsOn) {
                        if (!g_dragging) send_left_down();
                        return 1;
                    }
                    break;

                case 'S':
                    if (capsOn) {
                        send_click(FALSE);
                        return 1;
                    }
                    break;
            }
        }
        // Key up - ALWAYS clear the internal flags so they can't get stuck.
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

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Main ----------------------------------------------------------------------
int main(void) {
    kbHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
    if (!kbHook) {
        fprintf(stderr, "Failed to install keyboard hook.\n");
        return 1;
    }

    HANDLE hThread = CreateThread(NULL, 0, MovementThread, NULL, 0, NULL);
    if (!hThread) {
        fprintf(stderr, "Failed to create movement thread.\n");
        UnhookWindowsHookEx(kbHook);
        return 1;
    }

    // message loop (needed for hook to work)
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // cleanup
    g_exit = TRUE;
    if (g_dragging) send_left_up();
    WaitForSingleObject(hThread, 2000);
    CloseHandle(hThread);
    UnhookWindowsHookEx(kbHook);
    return 0;
}
