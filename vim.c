// combined.c
// integrates the mouse agent (originally m.c) with the overlay program (originally main.c)

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef UNICODE
#define UNICODE
#endif

#define KEEP_MATCHING 1 // from main.c
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <inttypes.h>

/* Minimal WTS definitions used by the mouse agent to avoid depending
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

#ifdef _MSC_VER
#pragma comment(lib, "Wtsapi32.lib")
#endif

// ------------------------------------------------------------------
// mouse agent constants / globals / helpers (from m.c)
#define BASE_SPEED_PX_PER_SEC  500.0
#define MAX_SPEED_PX_PER_SEC   1500.0
#define ACCEL_TIME_SEC         0.65
#define ACCEL_CURVE            1.8
#define VELOCITY_SMOOTHING     0.12
#define THREAD_SLEEP_MS_ACTIVE 1
#define THREAD_SLEEP_MS_IDLE   6

// tuning to protect against long dt / resume
#define MAX_SIM_STEP_SEC       0.05
#define SUSPEND_RESET_SEC      0.50

volatile BOOL g_h = FALSE, g_l = FALSE, g_j = FALSE, g_k = FALSE;
volatile BOOL g_exit = FALSE;
volatile BOOL g_dragging = FALSE;

double accX = 0.0, accY = 0.0;

INPUT lastInput;

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
    if (left) { in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP; }
    else { in[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; in[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP; }
    SendInput(2, in, sizeof(INPUT));
}

static void send_left_down(void) { if (g_dragging) return; INPUT in; ZeroMemory(&in, sizeof(in)); in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; SendInput(1,&in,sizeof(in)); g_dragging = TRUE; }
static void send_left_up(void)   { if (!g_dragging) return; INPUT in; ZeroMemory(&in, sizeof(in)); in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_LEFTUP; SendInput(1,&in,sizeof(INPUT)); g_dragging = FALSE; }

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
        if (dt <= 0.0) dt = 1.0/1000.0;
        prev = now;

        if (dt > SUSPEND_RESET_SEC) {
            velX = velY = 0.0;
            accX = accY = 0.0;
            holdTimeX = holdTimeY = 0.0;
            prevSignX = prevSignY = 0;
            Sleep(THREAD_SLEEP_MS_IDLE);
            continue;
        }

        double remaining = dt;
        double accumDX = 0.0, accumDY = 0.0;
        int signX = (g_l ? 1 : 0) - (g_h ? 1 : 0);
        int signY = (g_j ? 1 : 0) - (g_k ? 1 : 0);
        if (prevSignX != 0 && signX == 0) { velX = 0.0; accX = 0.0; holdTimeX = 0.0; }
        if (prevSignY != 0 && signY == 0) { velY = 0.0; accY = 0.0; holdTimeY = 0.0; }
        prevSignX = signX; prevSignY = signY;

        while (remaining > 0.0) {
            double step = remaining > MAX_SIM_STEP_SEC ? MAX_SIM_STEP_SEC : remaining;
            remaining -= step;
            if (signX != 0) holdTimeX += step; else holdTimeX = 0.0;
            if (signY != 0) holdTimeY += step; else holdTimeY = 0.0;

            double tX = holdTimeX / ACCEL_TIME_SEC; if (tX < 0.0) tX = 0.0; if (tX > 1.0) tX = 1.0;
            double tY = holdTimeY / ACCEL_TIME_SEC; if (tY < 0.0) tY = 0.0; if (tY > 1.0) tY = 1.0;

            double easeX = pow(tX, ACCEL_CURVE);
            double easeY = pow(tY, ACCEL_CURVE);

            double targetSpeedX = (double)signX * (BASE_SPEED_PX_PER_SEC + (MAX_SPEED_PX_PER_SEC - BASE_SPEED_PX_PER_SEC) * easeX);
            double targetSpeedY = (double)signY * (BASE_SPEED_PX_PER_SEC + (MAX_SPEED_PX_PER_SEC - BASE_SPEED_PX_PER_SEC) * easeY);

            velX += (targetSpeedX - velX) * VELOCITY_SMOOTHING;
            velY += (targetSpeedY - velY) * VELOCITY_SMOOTHING;

            double moveX = velX * step;
            double moveY = velY * step;

            accX += moveX;
            accY += moveY;

            int dx = (int)round(accX);
            int dy = (int)round(accY);
            if (dx != 0 || dy != 0) {
                accX -= dx;
                accY -= dy;
                accumDX += dx;
                accumDY += dy;
            }
        }

        if ((int)accumDX != 0 || (int)accumDY != 0) {
            send_relative_move((int)accumDX, (int)accumDY);
        }

        if (signX == 0 && signY == 0) Sleep(THREAD_SLEEP_MS_IDLE);
        else Sleep(THREAD_SLEEP_MS_ACTIVE);
    }

    return 0;
}

// hook handling from m.c
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
        g_exit = TRUE;
        WaitForSingleObject(g_movementThread, 2000);
        CloseHandle(g_movementThread);
        g_movementThread = NULL;
        g_exit = FALSE;
    }
}

#define CLASSNAME L"MouseAgentHiddenWindow"

LRESULT CALLBACK MouseAgentWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE:
        WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
        break;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMESUSPEND || wParam == PBT_APMRESUMEAUTOMATIC) {
            UninstallHook();
            InstallHook();
        } else if (wParam == PBT_APMSUSPEND) {
            // nothing
        }
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_UNLOCK) {
            UninstallHook();
            InstallHook();
        } else if (wParam == WTS_SESSION_LOGON) {
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

// thread entry that runs the mouse agent message loop
DWORD WINAPI MouseAgentThread(LPVOID lpv) {
    (void)lpv;
    HWND hwnd;
    HANDLE hMutex = CreateMutex(NULL, FALSE, L"Local\\mouse_agent_single_instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        return 0;
    }

    WNDCLASS wc = {0};
    wc.lpfnWndProc = MouseAgentWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = CLASSNAME;
    RegisterClass(&wc);

    hwnd = CreateWindowEx(0, CLASSNAME, L"MouseAgentHidden", 0,
                          CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                          NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        fprintf(stderr, "MouseAgent CreateWindowEx failed\n");
        return 1;
    }

    InstallHook();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UninstallHook();
    CloseHandle(hMutex);
    return 0;
}

// ------------------------------------------------------------------
// overlay program definitions from original main.c

// width desired cell (controls columns)
#define DESIRED_CELL_PX 30
// NEW: desired cell height for rows: reduce this to pack more rows.
// Default 40 — tweak to taste (smaller -> more rows -> smaller labels)
#define DESIRED_CELL_H 30

#define INPUT_BUF 64
#define DENSITY_MULT 2
#define OVERLAY_ALPHA 150

static HWND g_mainWnd = NULL;
static HWND g_overlayWnd = NULL;
static HWND g_capsWnd = NULL;
static volatile BOOL g_capsThreadExit = FALSE;

// caps marker window procedure: paint a red square with white '*'
LRESULT CALLBACK CapsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rr; GetClientRect(hwnd, &rr);
        HBRUSH rb = CreateSolidBrush(RGB(255,0,0));
        FillRect(hdc, &rr, rb);
        DeleteObject(rb);
        SetTextColor(hdc, RGB(255,255,255));
        HFONT fsmall = CreateFontA(12,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,VARIABLE_PITCH | FF_SWISS, "Segoe UI");
        HFONT oldfs = (HFONT)SelectObject(hdc, fsmall);
        DrawTextA(hdc, "*", -1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldfs);
        DeleteObject(fsmall);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default: return DefWindowProcA(hwnd, msg, wp, lp);
    }
}
static HFONT g_font = NULL;
static char g_input[INPUT_BUF];
static int g_input_len = 0;
static CRITICAL_SECTION g_lock;

// forward declarations
void create_overlay();
void destroy_overlay();
void perform_click_at(POINT pt, BOOL right);
static int find_click_point_in_rect(RECT cell, POINT *outPt);

// overlay program implementation from original main.c follows

static HFONT create_font_for_cell(int cell_h) {
    int font_px = max(8, cell_h / 2);
    return CreateFontA(
        -font_px, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, "Segoe UI"
    );
}

static void compute_grid(int screen_w, int screen_h, int *pcols, int *prows, long long *ptotal) {
    // width uses DESIRED_CELL_PX, height uses DESIRED_CELL_H (so we can reduce height)
    int cols = max(1, screen_w / DESIRED_CELL_PX);
    int rows = max(1, screen_h / DESIRED_CELL_H);
    cols = cols * DENSITY_MULT;
    rows = rows * DENSITY_MULT;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    *pcols = cols;
    *prows = rows;
    *ptotal = (long long)cols * (long long)rows;
}

static int compute_label_length(long long total) {
    long long cap = 26;
    int L = 1;
    while (cap < total) {
        if (cap > (LLONG_MAX / 26)) break;
        cap *= 26;
        L++;
    }
    return L;
}

static void label_for_index(long long idx, char *buf, int L) {
    long long v = idx - 1;
    buf[L] = '\0';
    for (int i = L - 1; i >= 0; --i) {
        int d = (int)(v % 26);
        buf[i] = 'a' + d;
        v /= 26;
    }
}

static long long index_for_label(const char *s, int slen, int L, long long total) {
    if (slen != L) return -1;
    long long v = 0;
    for (int i = 0; i < L; ++i) {
        char ch = s[i];
        if (ch < 'a' || ch > 'z') return -1;
        v = v * 26 + (ch - 'a');
        if (v < 0) return -1;
    }
    long long idx = v + 1;
    if (idx < 1 || idx > total) return -1;
    return idx;
}

static long long numeric_unique_match(const char *s, int slen, long long total) {
    if (slen <= 0) return -1;
    long long matches = 0;
    long long last_match_idx = -1;
    char tmp[64];
    for (long long i = 1; i <= total; ++i) {
        int n = snprintf(tmp, sizeof(tmp), "%" PRId64, (int64_t)i);
        if (n <= 0) continue;
        if (n >= slen) {
            if (strncmp(tmp, s, (size_t)slen) == 0) {
                matches++;
                last_match_idx = i;
                if (matches > 1) return -1;
            }
        }
    }
    if (matches == 1) return last_match_idx;
    return -1;
}

// New helper: determine visibility of an index given current g_input.
// Behavior controlled by KEEP_MATCHING macro: if KEEP_MATCHING==1, we SHOW only labels that match the typed prefix.
// If KEEP_MATCHING==0, we HIDE labels that match the typed prefix (i.e. remove matching ones).
static int is_index_visible_for_input(long long idx, int L, long long total) {
    (void)total;
    // empty input -> show everything
    if (g_input_len == 0) return 1;

    // build english label
    char lbl[64];
    label_for_index(idx, lbl, L);

    // check if input is all letters
    int is_all_letters = 1;
    for (int i = 0; i < g_input_len; ++i) {
        char ch = g_input[i];
        if (!(ch >= 'a' && ch <= 'z')) { is_all_letters = 0; break; }
    }
    if (is_all_letters) {
        // compare typed prefix to label prefix
        if (g_input_len > L) {
            // typed longer than label length -> cannot match
#if KEEP_MATCHING
            return 0;
#else
            return 1;
#endif
        }
        int match = (strncmp(lbl, g_input, (size_t)g_input_len) == 0);
#if KEEP_MATCHING
        return match ? 1 : 0;
#else
        return match ? 0 : 1;
#endif
    }

    // check digits
    int is_all_digits = 1;
    for (int i = 0; i < g_input_len; ++i) {
        if (!(g_input[i] >= '0' && g_input[i] <= '9')) { is_all_digits = 0; break; }
    }
    if (is_all_digits) {
        char tmp[64];
        int n = snprintf(tmp, sizeof(tmp), "%" PRId64, (int64_t)idx);
        if (n <= 0) return 1; // show by default
        int match = (n >= g_input_len && strncmp(tmp, g_input, (size_t)g_input_len) == 0);
#if KEEP_MATCHING
        return match ? 1 : 0;
#else
        return match ? 0 : 1;
#endif
    }

    // mixed/invalid input: show nothing (safer)
    return 0;
}

// --- add near the top with other forward decls ---
static int find_click_point_in_rect(RECT cell, POINT *outPt);

// --- replace the existing click_by_index with the version below ---
static void click_by_index(HWND hwnd, long long v) {
    if (v < 1) return;
    RECT sr;
    GetWindowRect(hwnd, &sr);
    int screen_w = sr.right - sr.left;
    int screen_h = sr.bottom - sr.top;

    int cols, rows;
    long long total;
    compute_grid(screen_w, screen_h, &cols, &rows, &total);

    if (v > total) return;

    long long idx = v - 1;
    int r = (int)(idx / cols);
    int c = (int)(idx % cols);

    int base_w = screen_w / cols;
    int extra_w = screen_w % cols;
    int base_h = screen_h / rows;
    int extra_h = screen_h % rows;

    int x = sr.left;
    for (int i = 0; i < c; ++i) x += base_w + (i < extra_w ? 1 : 0);
    int wcell = base_w + (c < extra_w ? 1 : 0);
    int y = sr.top;
    for (int i = 0; i < r; ++i) y += base_h + (i < extra_h ? 1 : 0);
    int hcell = base_h + (r < extra_h ? 1 : 0);

    RECT cell = { x, y, x + wcell, y + hcell };

    // remove overlay so WindowFromPoint can see real windows
    destroy_overlay();
    Sleep(25);

    POINT clickPt;
    // try to find a clickable point inside the cell (center-first, then neighbors)
    if (!find_click_point_in_rect(cell, &clickPt)) {
        // fallback: center
        clickPt.x = cell.left + (wcell / 2);
        clickPt.y = cell.top + (hcell / 2);
    }

    perform_click_at(clickPt, FALSE);
}

////////////////////////////////////////////////////////////////////////////////
// helper: scan candidate points inside rect and pick the first point whose
// WindowFromPoint returns a visible & enabled window (not desktop).
// Returns 1 and fills outPt on success, otherwise returns 0.
static int find_click_point_in_rect(RECT cell, POINT *outPt) {
    if (!outPt) return 0;

    // fractional offsets to try (center first, then 8 neighbors, biased inward)
    const double offs[] = { 0.50, 0.25, 0.75, 0.10, 0.90 };
    const int n_offs = sizeof(offs) / sizeof(offs[0]);

    // First try exact center
    POINT pt;
    pt.x = cell.left + (cell.right - cell.left) / 2;
    pt.y = cell.top + (cell.bottom - cell.top) / 2;
    HWND wh = WindowFromPoint(pt);
    if (wh && wh != GetDesktopWindow() && IsWindowVisible(wh) && IsWindowEnabled(wh)) {
        *outPt = pt;
        return 1;
    }

    // Try small grid of candidate points, ordered to pick inner points first.
    for (int yi = 0; yi < n_offs; ++yi) {
        for (int xi = 0; xi < n_offs; ++xi) {
            // skip the exact center because we already tried it
            if (offs[xi] == 0.50 && offs[yi] == 0.50) continue;

            pt.x = cell.left + (int)((cell.right - cell.left) * offs[xi]);
            pt.y = cell.top  + (int)((cell.bottom - cell.top) * offs[yi]);

            HWND w = WindowFromPoint(pt);
            if (!w) continue;
            if (w == GetDesktopWindow()) continue;

            // ignore our own overlay window if for some reason it is still present
            if (g_overlayWnd && (w == g_overlayWnd)) continue;

            // accept only visible & enabled windows (basic heuristic for "clickable")
            if (!IsWindowVisible(w)) continue;
            if (!IsWindowEnabled(w)) continue;

            // additional heuristic: ignore invisible regions or transparent controls
            // (can't easily check transparency here; trusting IsWindowVisible/Enabled is usually enough)

            *outPt = pt;
            return 1;
        }
    }

    // nothing found
    return 0;
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        SetFocus(hwnd);
        return 0;
    }
    case WM_KEYDOWN: {
        if (wp == VK_ESCAPE) {
            destroy_overlay();
            return 0;
        } else if (wp == VK_BACK) {
            if (g_input_len > 0) {
                g_input_len--;
                g_input[g_input_len] = '\0';
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        } else if (wp == VK_RETURN) {
            if (g_input_len > 0) {
                RECT sr; GetWindowRect(hwnd, &sr);
                int screen_w = sr.right - sr.left;
                int screen_h = sr.bottom - sr.top;
                int cols, rows;
                long long total;
                compute_grid(screen_w, screen_h, &cols, &rows, &total);
                int L = compute_label_length(total);

                int is_all_letters = 1;
                for (int i = 0; i < g_input_len; ++i) {
                    char ch = g_input[i];
                    if (!(ch >= 'a' && ch <= 'z')) { is_all_letters = 0; break; }
                }
                if (is_all_letters) {
                    long long idx = index_for_label(g_input, g_input_len, L, total);
                    if (idx != -1) {
                        click_by_index(hwnd, idx);
                        return 0;
                    }
                }

                int is_all_digits = 1;
                for (int i = 0; i < g_input_len; ++i) {
                    if (!(g_input[i] >= '0' && g_input[i] <= '9')) { is_all_digits = 0; break; }
                }
                if (is_all_digits) {
                    long long v = atoll(g_input);
                    if (v >= 1 && v <= total) {
                        click_by_index(hwnd, v);
                        return 0;
                    }
                }

                // fallback: if pressing Enter and there is exactly one visible label, click it
                long long visible_count = 0, last_idx = -1;
                for (long long i = 1; i <= total; ++i) {
                    if (is_index_visible_for_input(i, L, total)) { visible_count++; last_idx = i; if (visible_count > 1) break; }
                }
                if (visible_count == 1) { click_by_index(hwnd, last_idx); return 0; }

                g_input_len = 0; g_input[0] = '\0';
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        } else {
            BOOL handled = FALSE;
            if ((wp >= '0' && wp <= '9') || (wp >= VK_NUMPAD0 && wp <= VK_NUMPAD9)) {
                char d;
                if (wp >= '0' && wp <= '9') d = (char)wp;
                else d = (char)('0' + (wp - VK_NUMPAD0));
                if (g_input_len < INPUT_BUF - 1) {
                    g_input[g_input_len++] = d;
                    g_input[g_input_len] = '\0';
                    handled = TRUE;
                }
            } else if ((wp >= 'A' && wp <= 'Z')) {
                char ch = (char)('a' + (wp - 'A'));
                if (g_input_len < INPUT_BUF - 1) {
                    g_input[g_input_len++] = ch;
                    g_input[g_input_len] = '\0';
                    handled = TRUE;
                }
            }

            if (handled) {
                InvalidateRect(hwnd, NULL, TRUE);

                RECT sr; GetWindowRect(hwnd, &sr);
                int screen_w = sr.right - sr.left;
                int screen_h = sr.bottom - sr.top;
                int cols, rows;
                long long total;
                compute_grid(screen_w, screen_h, &cols, &rows, &total);
                int L = compute_label_length(total);

                int is_all_letters = 1;
                for (int i = 0; i < g_input_len; ++i) {
                    char ch = g_input[i];
                    if (!(ch >= 'a' && ch <= 'z')) { is_all_letters = 0; break; }
                }
                if (is_all_letters) {
                    // If full-length label typed, try decode
                        if (g_input_len == L) {
                        long long idx = index_for_label(g_input, g_input_len, L, total);
                        if (idx != -1) {
                            BOOL shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                            // perform right-click if shift was held for the final letter
                            RECT sr; GetWindowRect(hwnd, &sr);
                            int screen_w = sr.right - sr.left;
                            int screen_h = sr.bottom - sr.top;
                            int cols, rows;
                            long long total2;
                            compute_grid(screen_w, screen_h, &cols, &rows, &total2);

                            // reuse click_by_index logic, but perform the click ourselves
                            long long v = idx;
                            if (v >= 1) {
                                RECT wr; GetWindowRect(hwnd, &wr);
                                int sw = wr.right - wr.left;
                                int sh = wr.bottom - wr.top;
                                int ccols, crows; long long ttotal;
                                compute_grid(sw, sh, &ccols, &crows, &ttotal);
                                if (v <= ttotal) {
                                    long long idx0 = v - 1;
                                    int r0 = (int)(idx0 / ccols);
                                    int c0 = (int)(idx0 % ccols);

                                    int base_w = sw / ccols;
                                    int extra_w = sw % ccols;
                                    int base_h = sh / crows;
                                    int extra_h = sh % crows;

                                    int x = wr.left;
                                    for (int i = 0; i < c0; ++i) x += base_w + (i < extra_w ? 1 : 0);
                                    int wcell = base_w + (c0 < extra_w ? 1 : 0);
                                    int y = wr.top;
                                    for (int i = 0; i < r0; ++i) y += base_h + (i < extra_h ? 1 : 0);
                                    int hcell = base_h + (r0 < extra_h ? 1 : 0);

                                    RECT cell = { x, y, x + wcell, y + hcell };
                                    destroy_overlay();
                                    Sleep(25);
                                    POINT clickPt;
                                    if (!find_click_point_in_rect(cell, &clickPt)) {
                                        clickPt.x = cell.left + (wcell / 2);
                                        clickPt.y = cell.top + (hcell / 2);
                                    }
                                    perform_click_at(clickPt, shiftDown);
                                    return 0;
                                }
                            }
                        }
                        else {
                            // invalid full label -> clear
                            g_input_len = 0; g_input[0] = '\0';
                            InvalidateRect(hwnd, NULL, TRUE);
                            return 0;
                        }
                    }
                    // otherwise: check visible count and auto-click if unique
                    long long visible_count = 0, last_idx = -1;
                    for (long long i = 1; i <= total; ++i) {
                        if (is_index_visible_for_input(i, L, total)) { visible_count++; last_idx = i; if (visible_count > 1) break; }
                    }
                    if (visible_count == 1) { click_by_index(hwnd, last_idx); return 0; }
                    // if none visible -> clear
                    if (visible_count == 0) { g_input_len = 0; g_input[0] = '\0'; InvalidateRect(hwnd, NULL, TRUE); return 0; }

                    return 0;
                }

                int is_all_digits = 1;
                for (int i = 0; i < g_input_len; ++i) {
                    if (!(g_input[i] >= '0' && g_input[i] <= '9')) { is_all_digits = 0; break; }
                }
                if (is_all_digits) {
                    long long match = numeric_unique_match(g_input, g_input_len, total);
                    if (match != -1) { click_by_index(hwnd, match); return 0; }

                    // otherwise try the visible-count approach for numbers too
                    long long visible_count = 0, last_idx = -1;
                    for (long long i = 1; i <= total; ++i) {
                        if (is_index_visible_for_input(i, L, total)) { visible_count++; last_idx = i; if (visible_count > 1) break; }
                    }
                    if (visible_count == 1) { click_by_index(hwnd, last_idx); return 0; }
                    if (visible_count == 0) { g_input_len = 0; g_input[0] = '\0'; InvalidateRect(hwnd, NULL, TRUE); return 0; }
                }

            }
            return 0;
        }
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT client; GetClientRect(hwnd, &client);

        int screen_w = client.right - client.left;
        int screen_h = client.bottom - client.top;

        // use compute_grid so width/height logic is centralized
        int cols, rows;
        long long total;
        compute_grid(screen_w, screen_h, &cols, &rows, &total);

        int base_w = screen_w / cols;
        int extra_w = screen_w % cols;
        int base_h = screen_h / rows;
        int extra_h = screen_h % rows;

        int approx_cell_h = base_h + (extra_h > 0 ? 1 : 0);
        if (g_font) { DeleteObject(g_font); g_font = NULL; }
        g_font = create_font_for_cell(approx_cell_h);

        HBRUSH bg = CreateSolidBrush(RGB(0,0,0));
        FillRect(hdc, &client, bg);
        DeleteObject(bg);

        HFONT oldf = (HFONT)SelectObject(hdc, g_font ? g_font : GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(hdc, TRANSPARENT);

        SetTextColor(hdc, RGB(255,255,255));

        long long idx = 0;
        int y = 0;
        char lbl[64];

        // Precompute label length and input-type flags once for efficiency and consistent behavior
        int L = compute_label_length(total);
        int is_all_letters_input = 1;
        int is_all_digits_input = 1;
        for (int i = 0; i < g_input_len; ++i) {
            char ch = g_input[i];
            if (!(ch >= 'a' && ch <= 'z')) is_all_letters_input = 0;
            if (!(ch >= '0' && ch <= '9')) is_all_digits_input = 0;
        }

        for (int r = 0; r < rows; ++r) {
            int hcell = base_h + (r < extra_h ? 1 : 0);
            int x = 0;
            for (int c = 0; c < cols; ++c) {
                int wcell = base_w + (c < extra_w ? 1 : 0);

                RECT cell = { x, y, x + wcell, y + hcell };

                ++idx;
                label_for_index(idx, lbl, L);

                int visible = is_index_visible_for_input(idx, L, total);

                // draw visible cells only; hidden ones are left as background to
                // effectively disable their grid and let the user focus on the
                // remaining labels.
                if (visible) {
                    RECT lblR = cell;
                    lblR.left += 2; lblR.top += 2; lblR.right -= 2; lblR.bottom -= 2;
                    SetTextColor(hdc, RGB(255,255,255));

                    // New: show only the character at the current input position
                    // for matching labels (i.e., first, second, third character)
                    char to_draw[128];
                    to_draw[0] = '\0';

                    if (g_input_len == 0) {
                        // initial state: show first character of alpha label.
                        if (L > 0) {
                            to_draw[0] = lbl[0];
                            to_draw[1] = '\0';
                        }
                    } else if (is_all_letters_input) {
                        // If typed prefix matches this label up to g_input_len,
                        // and there is a next character, show exactly that single char.
                        if (g_input_len < L && strncmp(lbl, g_input, (size_t)g_input_len) == 0) {
                            to_draw[0] = lbl[g_input_len];
                            to_draw[1] = '\0';
                        } else {
                            // either doesn't match, or typed length >= label length -> show nothing
                            to_draw[0] = '\0';
                        }
                    } else if (is_all_digits_input) {
                        // numeric labels - compute numeric string for idx
                        char numbuf[64];
                        int n = snprintf(numbuf, sizeof(numbuf), "%" PRId64, idx);
                        if (n > 0) {
                            if (g_input_len == 0) {
                                // initial: show first digit
                                to_draw[0] = numbuf[0];
                                to_draw[1] = '\0';
                            } else if (g_input_len < n && strncmp(numbuf, g_input, (size_t)g_input_len) == 0) {
                                // show next digit only
                                to_draw[0] = numbuf[g_input_len];
                                to_draw[1] = '\0';
                            } else {
                                to_draw[0] = '\0';
                            }
                        }
                    } else {
                        // mixed/other: fallback to existing behavior (draw full alpha label)
                        // but only if visible (visibility logic already decides)
                        strncpy(to_draw, lbl, sizeof(to_draw)-1);
                        to_draw[sizeof(to_draw)-1] = '\0';
                    }

                    // DrawTextA with the chosen string. If to_draw is empty, nothing is drawn.
                    if (to_draw[0] != '\0') {
                        DrawTextA(hdc, to_draw, -1, &lblR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    }
                }

                x += wcell;
            }
            y += hcell;
        }

        // update small caps marker window (separate topmost window) if present
        if (g_capsWnd) {
            BOOL caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
            if (caps) {
                // position at top-left of primary monitor (slightly inset)
                SetWindowPos(g_capsWnd, HWND_TOPMOST, 2, 2, 18, 18, SWP_SHOWWINDOW | SWP_NOACTIVATE);
                // paint the caps window content
                HDC chdc = GetDC(g_capsWnd);
                if (chdc) {
                    HBRUSH rb = CreateSolidBrush(RGB(200,32,32));
                    RECT rr = { 0, 0, 18, 18 };
                    FillRect(chdc, &rr, rb);
                    DeleteObject(rb);
                    SetTextColor(chdc, RGB(255,255,255));
                    HFONT fsmall = CreateFontA(12,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,VARIABLE_PITCH | FF_SWISS, "Segoe UI");
                    HFONT oldfs = (HFONT)SelectObject(chdc, fsmall);
                    DrawTextA(chdc, "*", -1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(chdc, oldfs);
                    DeleteObject(fsmall);
                    ReleaseDC(g_capsWnd, chdc);
                }
            } else {
                ShowWindow(g_capsWnd, SW_HIDE);
            }
        }

        SelectObject(hdc, oldf);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

void create_overlay() {
    if (g_overlayWnd) return;

    HINSTANCE hInst = GetModuleHandle(NULL);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "VimiumTiledOverlayClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);

    g_overlayWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        wc.lpszClassName,
        "Vimium Tiled Overlay",
        WS_POPUP | WS_VISIBLE,
        0, 0, sx, sy,
        NULL, NULL, hInst, NULL
    );
    if (!g_overlayWnd) {
        UnregisterClassA(wc.lpszClassName, hInst);
        return;
    }

    SetLayeredWindowAttributes(g_overlayWnd, 0, (BYTE)OVERLAY_ALPHA, LWA_ALPHA);

    g_input_len = 0;
    g_input[0] = '\0';

    // (caps marker created at program startup; do not recreate here)

    SetForegroundWindow(g_overlayWnd);
    SetFocus(g_overlayWnd);

    InvalidateRect(g_overlayWnd, NULL, TRUE);
    UpdateWindow(g_overlayWnd);
}

void destroy_overlay() {
    if (!g_overlayWnd) return;
    DestroyWindow(g_overlayWnd);
    UnregisterClassA("VimiumTiledOverlayClass", GetModuleHandle(NULL));
    g_overlayWnd = NULL;
    if (g_font) { DeleteObject(g_font); g_font = NULL; }
    // destroy caps marker window if present
    if (g_capsWnd) {
        // don't destroy the global caps window here; just hide it so it can
        // continue to be managed by the background caps monitor thread.
        ShowWindow(g_capsWnd, SW_HIDE);
    }
}

void perform_click_at(POINT pt, BOOL right) {
    SetCursorPos(pt.x, pt.y);
    Sleep(8);
    if (right) {
        INPUT events[2];
        ZeroMemory(events, sizeof(events));
        events[0].type = INPUT_MOUSE;
        events[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
        events[1].type = INPUT_MOUSE;
        events[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        SendInput(2, events, sizeof(INPUT));
    } else {
        INPUT events[2];
        ZeroMemory(events, sizeof(events));
        events[0].type = INPUT_MOUSE;
        events[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        events[1].type = INPUT_MOUSE;
        events[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(2, events, sizeof(INPUT));
    }
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY:
        if (wp == 1) {
            if (g_overlayWnd) destroy_overlay();
            else create_overlay();
        } else if (wp == 2) {
            PostQuitMessage(0);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

DWORD WINAPI ConsoleThread(LPVOID lp) {
    HWND main = (HWND)lp;
    char buf[128];
    // printf("Type 'go' to show overlay, 'q' to quit program. Hotkey: Shift+F\n");
    while (fgets(buf, sizeof(buf), stdin)) {
        size_t n = strlen(buf);
        if (n && buf[n-1] == '\n') buf[n-1] = '\0';
        if (strcmp(buf, "q") == 0) {
            PostMessage(main, WM_QUIT, 0, 0);
            break;
        } else if (strcmp(buf, "go") == 0) {
            PostMessage(main, WM_HOTKEY, (WPARAM)1, 0);
        } else {
            // removed user-facing instruction print
        }
    }
    return 0;
}

DWORD WINAPI CapsMonitorThread(LPVOID lp) {
    (void)lp;
    while (!g_capsThreadExit) {
        BOOL caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
        if (g_capsWnd) {
            if (caps) {
                SetWindowPos(g_capsWnd, HWND_TOPMOST, 2, 2, 18, 18, SWP_SHOWWINDOW | SWP_NOACTIVATE);
                InvalidateRect(g_capsWnd, NULL, TRUE);
            } else {
                ShowWindow(g_capsWnd, SW_HIDE);
            }
        }
        Sleep(150);
    }
    return 0;
}

int main(void) {
    InitializeCriticalSection((CRITICAL_SECTION*)&g_lock);

    HMODULE shcore = LoadLibraryA("shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI *SetProcDpiAwareness_t)(int);
        SetProcDpiAwareness_t spda = (SetProcDpiAwareness_t)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (spda) spda(2);
        FreeLibrary(shcore);
    } else {
        HMODULE user32 = LoadLibraryA("user32.dll");
        if (user32) {
            typedef BOOL (WINAPI *SetProcDPIAware_t)(void);
            SetProcDPIAware_t spda2 = (SetProcDPIAware_t)GetProcAddress(user32, "SetProcessDPIAware");
            if (spda2) spda2();
            FreeLibrary(user32);
        }
    }

    // start mouse agent thread before creating main window
    {
        DWORD tid;
        CreateThread(NULL, 0, MouseAgentThread, NULL, 0, &tid);
    }

    // create small caps marker window class and window early so it is always present
    {
        HINSTANCE hInst2 = GetModuleHandle(NULL);
        WNDCLASSA mwc = {0};
        mwc.lpfnWndProc = CapsWndProc;
        mwc.hInstance = hInst2;
        mwc.lpszClassName = "VimiumCapsMarkerClass";
        RegisterClassA(&mwc);
        g_capsWnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                                    mwc.lpszClassName,
                                    "CapsMarker",
                                    WS_POPUP,
                                    0, 0, 18, 18,
                                    NULL, NULL, hInst2, NULL);
        if (g_capsWnd) {
            SetLayeredWindowAttributes(g_capsWnd, 0, 255, LWA_ALPHA);
            ShowWindow(g_capsWnd, SW_HIDE);
        }
        // start caps monitor thread
        {
            DWORD ctid;
            CreateThread(NULL, 0, CapsMonitorThread, NULL, 0, &ctid);
        }
    }

    HINSTANCE hInst = GetModuleHandle(NULL);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "VimiumTiledMainClass";
    RegisterClassA(&wc);

    g_mainWnd = CreateWindowA(wc.lpszClassName, "Vimium Tiled Main", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                              NULL, NULL, hInst, NULL);
    if (!g_mainWnd) {
        fprintf(stderr, "Failed to create main window.\n");
        return 1;
    }

    if (!RegisterHotKey(g_mainWnd, 1, MOD_SHIFT, 'F')) {
        fprintf(stderr, "Warning: RegisterHotKey failed. Hotkey may be in use.\n");
    } else {
        // silent: hotkey registration succeeded (no instructional print)
    }

    // Register Ctrl+Alt+Q as a quit hotkey (id 2)
    if (!RegisterHotKey(g_mainWnd, 2, MOD_CONTROL | MOD_ALT, 'Q')) {
        fprintf(stderr, "Warning: RegisterHotKey Ctrl+Alt+Q failed. Hotkey may be in use.\n");
    }
    DWORD tid;
    HANDLE hThread = CreateThread(NULL, 0, ConsoleThread, g_mainWnd, 0, &tid);
    if (!hThread) {
        fprintf(stderr, "Failed to create console thread.\n");
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_overlayWnd) destroy_overlay();
    if (g_font) { DeleteObject(g_font); g_font = NULL; }
    if (hThread) {
        PostThreadMessage(tid, WM_QUIT, 0, 0);
        WaitForSingleObject(hThread, 500);
        CloseHandle(hThread);
    }
    // stop caps monitor and destroy caps window
    if (g_capsWnd) {
        g_capsThreadExit = TRUE;
        Sleep(200);
        DestroyWindow(g_capsWnd);
        UnregisterClassA("VimiumCapsMarkerClass", hInst);
        g_capsWnd = NULL;
    }
    UnregisterClassA("VimiumTiledMainClass", hInst);
    DeleteCriticalSection((CRITICAL_SECTION*)&g_lock);
    return 0;
}