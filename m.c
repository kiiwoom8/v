// mouse_control_windows_hook.c
// Compile (MinGW):
// gcc mouse_control_windows_hook.c -o mouse_control -lgdi32 -luser32

#include <windows.h>
#include <stdio.h>
#include <math.h>

#define BASE_SPEED_PX_PER_SEC  600.0   // base speed when key held (adjust)
#define THREAD_SLEEP_MS        1       // minimal sleep fallback

HHOOK kbHook;
volatile BOOL g_run = TRUE;
volatile BOOL g_h = FALSE, g_l = FALSE, g_j = FALSE, g_k = FALSE;
volatile BOOL g_exit = FALSE;
volatile BOOL g_dragging = FALSE;      // true when left button is held down (A pressed)

// Accumulators for fractional movement so small dt still moves smoothly
double accX = 0.0, accY = 0.0;

// Helper: send relative mouse move using SendInput
static void send_relative_move(int dx, int dy) {
    INPUT inp;
    ZeroMemory(&inp, sizeof(inp));
    inp.type = INPUT_MOUSE;
    inp.mi.dx = dx;
    inp.mi.dy = dy;
    inp.mi.mouseData = 0;
    inp.mi.dwFlags = MOUSEEVENTF_MOVE; // relative move
    inp.mi.time = 0;
    inp.mi.dwExtraInfo = 0;
    SendInput(1, &inp, sizeof(inp));
}

// Click helper (left/right click - press+release)
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

// Press-and-hold left button (start dragging)
static void send_left_down(void) {
    if (g_dragging) return; // already down
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &in, sizeof(in));
    g_dragging = TRUE;
}

// Release left button (stop dragging)
static void send_left_up(void) {
    if (!g_dragging) return;
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(in));
    g_dragging = FALSE;
}

// Movement thread: runs while program runs, moves cursor according to pressed flags
DWORD WINAPI MovementThread(LPVOID lpv) {
    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    // current velocity smoothing (for ease-in/out if you want)
    double velX = 0.0, velY = 0.0;
    const double smoothing = 0.15; // 0..1, bigger = snappier; smaller = smoother

    while (!g_exit) {
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart;
        if (dt <= 0) dt = 1.0 / 1000.0;
        prev = now;

        // desired velocity based on keys
        double desiredX = 0.0, desiredY = 0.0;
        if (g_h) desiredX -= BASE_SPEED_PX_PER_SEC;
        if (g_l) desiredX += BASE_SPEED_PX_PER_SEC;
        if (g_j) desiredY += BASE_SPEED_PX_PER_SEC;
        if (g_k) desiredY -= BASE_SPEED_PX_PER_SEC;

        // smooth velocity toward desired (simple low-pass)
        velX += (desiredX - velX) * smoothing;
        velY += (desiredY - velY) * smoothing;

        // compute movement this tick (may be fractional)
        double moveX = velX * dt;
        double moveY = velY * dt;

        // accumulate fractional parts and convert to ints
        accX += moveX;
        accY += moveY;
        int dx = (int)round(accX);
        int dy = (int)round(accY);

        if (dx != 0 || dy != 0) {
            // subtract the integer part we consumed
            accX -= dx;
            accY -= dy;

            // send the relative motion
            send_relative_move(dx, dy);
        }

        // If no keys pressed, yield/sleep a bit to avoid busy-loop
        if (!g_h && !g_l && !g_j && !g_k) {
            Sleep(5);
        } else {
            // for active movement we sleep very shortly to keep high frame rate
            // you can tune Sleep(0) / Sleep(1) depending on CPU usage and smoothness
            Sleep(1);
        }
    }

    return 0;
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lParam;

        // only act when Caps Lock is ON (same as your original)
        if (GetKeyState(VK_CAPITAL) & 1) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                switch (k->vkCode) {
                    case 'H': g_h = TRUE;  return 1;
                    case 'L': g_l = TRUE;  return 1;
                    case 'J': g_j = TRUE;  return 1;
                    case 'K': g_k = TRUE;  return 1;

                    // A becomes a drag-modifier: press -> left down, release -> left up
                    case 'A':
                        // only send left-down once when the key becomes pressed
                        if (!g_dragging) {
                            send_left_down();
                        }
                        return 1;

                    // keep S as a simple right-click (press+release)
                    case 'S':
                        send_click(FALSE);
                        return 1;
                }
            } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                switch (k->vkCode) {
                    case 'H': g_h = FALSE; return 1;
                    case 'L': g_l = FALSE; return 1;
                    case 'J': g_j = FALSE; return 1;
                    case 'K': g_k = FALSE; return 1;

                    case 'A':
                        // release left button when A is released
                        if (g_dragging) {
                            send_left_up();
                        }
                        return 1;
                }
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

int main(void) {
    kbHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
    if (!kbHook) {
        fprintf(stderr, "Failed to install keyboard hook.\n");
        return 1;
    }

    // start movement thread
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

    // cleanup - ensure we release left button if still down
    g_exit = TRUE;
    if (g_dragging) {
        send_left_up();
    }
    WaitForSingleObject(hThread, 2000);
    CloseHandle(hThread);
    UnhookWindowsHookEx(kbHook);
    return 0;
}
