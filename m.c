// agent_with_resume_fixed.c
// Compile: gcc agent_with_resume_fixed.c -o mouse_agent -lgdi32 -luser32 -ladvapi32 -lwtsapi32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>

/* Minimal WTS definitions used by this program to avoid depending
    on potentially incompatible system headers in MinGW. The real
    implementations are in Wtsapi32.lib which we link against. */
#ifndef WM_WTSSESSION_CHANGE
#define WM_WTSSESSION_CHANGE 0x02B1
#endif

#ifndef WTS_SESSION_UNLOCK
#define WTS_SESSION_UNLOCK 0x8
#endif

#ifndef WTS_SESSION_LOGON
#define WTS_SESSION_LOGON 0x5
#endif

#ifndef NOTIFY_FOR_THIS_SESSION
#define NOTIFY_FOR_THIS_SESSION 0
#endif

BOOL WINAPI WTSRegisterSessionNotification(HWND hWnd, DWORD dwFlags);
BOOL WINAPI WTSUnRegisterSessionNotification(HWND hWnd);
#include <stdio.h>
#include <math.h>

#ifdef _MSC_VER
#pragma comment(lib, "Wtsapi32.lib")
#endif

// ---- copy your constants, globals and movement code here ----
#define BASE_SPEED_PX_PER_SEC  500.0
#define MAX_SPEED_PX_PER_SEC   1500.0
#define ACCEL_TIME_SEC         0.65
#define ACCEL_CURVE            1.8
#define VELOCITY_SMOOTHING     0.12
#define THREAD_SLEEP_MS_ACTIVE 1
#define THREAD_SLEEP_MS_IDLE   6

// New tuning constants to protect against long dt / resume spikes
#define MAX_SIM_STEP_SEC       0.05   // simulate in steps of at most 50 ms
#define SUSPEND_RESET_SEC      0.50   // if dt > 0.5s, treat as suspend/resume and reset state

volatile BOOL g_h = FALSE, g_l = FALSE, g_j = FALSE, g_k = FALSE;
volatile BOOL g_exit = FALSE;
volatile BOOL g_dragging = FALSE;

double accX = 0.0, accY = 0.0;

INPUT lastInput;

// --- your existing input helpers (unchanged) ---
static void send_relative_move(int dx, int dy) {
    INPUT inp;
    ZeroMemory(&inp, sizeof(inp));
    inp.type = INPUT_MOUSE;
    inp.mi.dx = dx;
    inp.mi.dy = dy;
    inp.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &inp, sizeof(inp));
}

static void send_click(BOOL left) { /* same as you had */
    INPUT in[2];
    ZeroMemory(in, sizeof(in));
    in[0].type = in[1].type = INPUT_MOUSE;
    if (left) { in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP; }
    else { in[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; in[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP; }
    SendInput(2, in, sizeof(INPUT));
}

static void send_left_down(void) { if (g_dragging) return; INPUT in; ZeroMemory(&in, sizeof(in)); in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; SendInput(1,&in,sizeof(in)); g_dragging = TRUE; }
static void send_left_up(void)   { if (!g_dragging) return; INPUT in; ZeroMemory(&in, sizeof(in)); in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_LEFTUP; SendInput(1,&in,sizeof(in)); g_dragging = FALSE; }

// MovementThread (improved robustness to long dt / resume)
DWORD WINAPI MovementThread(LPVOID lpv) {
    (void)lpv;
    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    double velX = 0.0, velY = 0.0;
    double holdTimeX = 0.0, holdTimeY = 0.0;
    int prevSignX = 0, prevSignY = 0;

    while (!g_exit) {
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart;
        if (dt <= 0.0) dt = 1.0/1000.0; // safety minimum
        prev = now;

        // If dt is extremely large, it's likely the system suspended / resumed or we were paused.
        // Reset transient state to avoid huge cursor jumps.
        if (dt > SUSPEND_RESET_SEC) {
            velX = velY = 0.0;
            accX = accY = 0.0;
            holdTimeX = holdTimeY = 0.0;
            prevSignX = prevSignY = 0;
            // avoid sending any movement on this iteration
            Sleep(THREAD_SLEEP_MS_IDLE);
            continue;
        }

        // Break up dt into smaller simulation steps so smoothing & integration behave predictably,
        // even if the scheduling caused a longer-than-normal dt (but below the suspend threshold).
        double remaining = dt;
        double accumDX = 0.0, accumDY = 0.0;

        // read the current sign once per outer loop (keys may change asynchronously, but volatile reads are cheap)
        int signX = (g_l ? 1 : 0) - (g_h ? 1 : 0);
        int signY = (g_j ? 1 : 0) - (g_k ? 1 : 0);

        // If a direction was released since last frame, clear velocity & accumulator for that axis
        if (prevSignX != 0 && signX == 0) { velX = 0.0; accX = 0.0; holdTimeX = 0.0; }
        if (prevSignY != 0 && signY == 0) { velY = 0.0; accY = 0.0; holdTimeY = 0.0; }
        prevSignX = signX; prevSignY = signY;

        while (remaining > 0.0) {
            double step = remaining > MAX_SIM_STEP_SEC ? MAX_SIM_STEP_SEC : remaining;
            remaining -= step;

            // update hold times in small steps for smooth acceleration
            if (signX != 0) holdTimeX += step; else holdTimeX = 0.0;
            if (signY != 0) holdTimeY += step; else holdTimeY = 0.0;

            double tX = holdTimeX / ACCEL_TIME_SEC; if (tX < 0.0) tX = 0.0; if (tX > 1.0) tX = 1.0;
            double tY = holdTimeY / ACCEL_TIME_SEC; if (tY < 0.0) tY = 0.0; if (tY > 1.0) tY = 1.0;

            double easeX = pow(tX, ACCEL_CURVE);
            double easeY = pow(tY, ACCEL_CURVE);

            double targetSpeedX = (double)signX * (BASE_SPEED_PX_PER_SEC + (MAX_SPEED_PX_PER_SEC - BASE_SPEED_PX_PER_SEC) * easeX);
            double targetSpeedY = (double)signY * (BASE_SPEED_PX_PER_SEC + (MAX_SPEED_PX_PER_SEC - BASE_SPEED_PX_PER_SEC) * easeY);

            // velocity smoothing applied per small step
            velX += (targetSpeedX - velX) * VELOCITY_SMOOTHING;
            velY += (targetSpeedY - velY) * VELOCITY_SMOOTHING;

            double moveX = velX * step;
            double moveY = velY * step;

            accX += moveX;
            accY += moveY;

            // accumulate integer movement to send after finishing steps
            int dx = (int)round(accX);
            int dy = (int)round(accY);
            if (dx != 0 || dy != 0) {
                accX -= dx;
                accY -= dy;
                accumDX += dx;
                accumDY += dy;
            }
        }

        // send a single relative move for the whole dt iteration (if any)
        if ((int)accumDX != 0 || (int)accumDY != 0) {
            send_relative_move((int)accumDX, (int)accumDY);
        }

        // sleep small amount depending on whether active movement requested
        if (signX == 0 && signY == 0) Sleep(THREAD_SLEEP_MS_IDLE);
        else Sleep(THREAD_SLEEP_MS_ACTIVE);
    }

    return 0;
}

// ---- hook handling ----
HHOOK g_kbHook = NULL;
HANDLE g_movementThread = NULL;

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lParam;
        BOOL capsOn = (GetKeyState(VK_CAPITAL) & 1) != 0;

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            switch (k->vkCode) {
                case 'H': if (capsOn) { g_h = TRUE; return 1; } break;
                case 'L': if (capsOn) { g_l = TRUE; return 1; } break;
                case 'J': if (capsOn) { g_j = TRUE; return 1; } break;
                case 'K': if (capsOn) { g_k = TRUE; return 1; } break;
                case 'A': if (capsOn) { if (!g_dragging) send_left_down(); return 1; } break;
                case 'S': if (capsOn) { send_click(FALSE); return 1; } break;
            }
        } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            switch (k->vkCode) {
                case 'H': if (g_h) { g_h = FALSE; return 1; } break;
                case 'L': if (g_l) { g_l = FALSE; return 1; } break;
                case 'J': if (g_j) { g_j = FALSE; return 1; } break;
                case 'K': if (g_k) { g_k = FALSE; return 1; } break;
                case 'A': { BOOL wasDragging = g_dragging; if (wasDragging) send_left_up(); if (wasDragging) return 1; break; }
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

void InstallHook(void) {
    if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
    g_kbHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
    if (!g_kbHook) {
        fprintf(stderr, "SetWindowsHookEx failed: %lu\n", GetLastError());
    } else {
        // start movement thread if not running
        if (!g_movementThread) {
            DWORD tid;
            g_movementThread = CreateThread(NULL, 0, MovementThread, NULL, 0, &tid);
        }
    }
}

void UninstallHook(void) {
    if (g_kbHook) {
        UnhookWindowsHookEx(g_kbHook);
        g_kbHook = NULL;
    }
    if (g_movementThread) {
        // ask thread to stop and wait a bit for it
        g_exit = TRUE;
        WaitForSingleObject(g_movementThread, 2000);
        CloseHandle(g_movementThread);
        g_movementThread = NULL;
        // reset flag for next start
        g_exit = FALSE;
    }
}

// ---- message window + power/session handling ----
#define CLASSNAME L"MouseAgentHiddenWindow"

LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE:
        // Register for session notifications so we can get unlock events
        WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
        break;

    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMESUSPEND || wParam == PBT_APMRESUMEAUTOMATIC) {
            // system resumed -> reinstall hook and restart movement thread (clean start)
            UninstallHook();
            InstallHook();
        } else if (wParam == PBT_APMSUSPEND) {
            // system suspending -> optional safe cleanup
            // we don't aggressively uninstall here to keep it simple, but you could if desired:
            // UninstallHook();
        }
        return TRUE;

    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_UNLOCK) {
            // user unlocked session -> reinstall hook
            UninstallHook();
            InstallHook();
        } else if (wParam == WTS_SESSION_LOGON) {
            // optional: ensure hook is present when user logs on
            InstallHook();
        }
        break;

    case WM_DESTROY:
        WTSUnRegisterSessionNotification(hwnd);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, PWSTR pCmdLine, int nCmdShow) {
    (void)hPrev; (void)pCmdLine; (void)nCmdShow;
    // single instance per session (mutex name could include session id)
    HANDLE hMutex = CreateMutex(NULL, FALSE, L"Local\\mouse_agent_single_instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        return 0;
    }

    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASSNAME;
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0, CLASSNAME, L"MouseAgentHidden", 0, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        fprintf(stderr, "CreateWindowEx failed\n");
        return 1;
    }

    // Install hook at startup
    InstallHook();

    // standard message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // cleanup
    UninstallHook();
    CloseHandle(hMutex);
    return 0;
}

#ifdef __MINGW32__
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow) {
    (void)lpCmdLine; (void)nCmdShow;
    return (int)wWinMain(hInstance, hPrev, GetCommandLineW(), nCmdShow);
}
#endif