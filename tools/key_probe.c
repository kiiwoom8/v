// key_probe.c - who takes Alt + = / Alt + - before vim mouse (make keyprobe).
// usage: key_probe [seconds]
//   1. Tries RegisterHotKey for Alt + = / - (main row and number pad, with and
//      without Shift) and says which another program already owns.
//   2. For the given seconds (default 30) prints every press of Alt, = and -
//      (main row and number pad) seen by its own low-level hook and by raw
//      input. A key raw input sees but no hook does was taken by a hook or a
//      hotkey ahead of them; one neither sees never left the keyboard driver.
//   Only those keys are printed, never what else is typed.
#define _WIN32_WINNT 0x0601
#define WINVER 0x0601
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static HHOOK s_hook;
static DWORD s_t0;

static int watched(UINT vk, UINT scan)
{
    return scan == 0x0D || scan == 0x0C || scan == 0x4E || scan == 0x4A ||
           scan == 0x38 || vk == VK_OEM_PLUS || vk == VK_OEM_MINUS ||
           vk == VK_ADD || vk == VK_SUBTRACT || vk == VK_LMENU ||
           vk == VK_RMENU || vk == VK_MENU;
}

static LRESULT CALLBACK ll_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;

        if (watched(k->vkCode, k->scanCode))
            printf("%6lu hook vk 0x%02lX scan 0x%02lX %s flags 0x%02lX\n",
                   GetTickCount() - s_t0, (unsigned long)k->vkCode,
                   (unsigned long)k->scanCode,
                   (wp == WM_KEYUP || wp == WM_SYSKEYUP) ? "up  " : "down",
                   (unsigned long)k->flags);
    }
    return CallNextHookEx(s_hook, code, wp, lp);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_INPUT) {
        RAWINPUT ri;
        UINT size = sizeof(ri);

        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size,
                            sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEKEYBOARD &&
            watched(ri.data.keyboard.VKey, ri.data.keyboard.MakeCode))
            printf("%6lu raw  vk 0x%02X scan 0x%02X %s\n",
                   GetTickCount() - s_t0, ri.data.keyboard.VKey,
                   ri.data.keyboard.MakeCode,
                   (ri.data.keyboard.Flags & RI_KEY_BREAK) ? "up  " : "down");
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void try_hotkey(const char *name, UINT mods, UINT vk)
{
    if (RegisterHotKey(NULL, 1, mods | 0x4000 /* MOD_NOREPEAT */, vk)) {
        UnregisterHotKey(NULL, 1);
        printf("  %-22s free\n", name);
    } else {
        DWORD e = GetLastError();
        printf("  %-22s %s (%lu)\n", name,
               e == 1409 ? "TAKEN by another program" : "failed",
               (unsigned long)e);
    }
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 30;
    WNDCLASSW wc;
    RAWINPUTDEVICE rid;
    HWND wnd;
    MSG msg;
    DWORD end;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("hotkeys:\n");
    try_hotkey("Alt + =", MOD_ALT, VK_OEM_PLUS);
    try_hotkey("Alt + -", MOD_ALT, VK_OEM_MINUS);
    try_hotkey("Alt + Shift + =", MOD_ALT | MOD_SHIFT, VK_OEM_PLUS);
    try_hotkey("Alt + Shift + -", MOD_ALT | MOD_SHIFT, VK_OEM_MINUS);
    try_hotkey("Alt + numpad +", MOD_ALT, VK_ADD);
    try_hotkey("Alt + numpad -", MOD_ALT, VK_SUBTRACT);
    try_hotkey("Ctrl + Alt + =", MOD_CONTROL | MOD_ALT, VK_OEM_PLUS);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"VimKeyProbe";
    RegisterClassW(&wc);
    wnd = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                        NULL, wc.hInstance, NULL);
    rid.usUsagePage = 1;
    rid.usUsage = 6;
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = wnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
        printf("raw input failed (%lu)\n", GetLastError());
    s_hook = SetWindowsHookExW(WH_KEYBOARD_LL, ll_proc,
                               GetModuleHandleW(NULL), 0);
    if (!s_hook)
        printf("hook failed (%lu)\n", GetLastError());
    s_t0 = GetTickCount();
    end = s_t0 + (DWORD)secs * 1000;
    printf("watching Alt, = and - for %d s\n", secs);
    while ((LONG)(end - GetTickCount()) > 0) {
        if (MsgWaitForMultipleObjects(0, NULL, FALSE, 100, QS_ALLINPUT) ==
            WAIT_OBJECT_0)
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
    }
    UnhookWindowsHookEx(s_hook);
    printf("done\n");
    return 0;
}
