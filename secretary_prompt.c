// Assemble an explicit prompt locally. Opening Claude never submits the text.
#include "common.h"
#include <stdlib.h>
#include <wchar.h>

#define PROMPT_CLASS L"VimMouseSecretaryPrompt"
#define PROMPT_LIMIT 24000
enum { TASK = 101, SOURCE, PASTE_SOURCE, COPY_AND_OPEN };

static const WCHAR *const tasks[] = {
    L"요약", L"쉬운 설명", L"한국어 번역", L"문장 다듬기"
};
static const WCHAR *const instructions[] = {
    L"다음 내용을 요약해 줘. 핵심과 다음 할 일은 있으면 구분해 줘.",
    L"다음 내용을 쉬운 한국어로 설명해 줘. 핵심 개념과 필요한 예를 들어 줘.",
    L"다음 내용을 자연스러운 한국어로 번역해 줘. 이름과 숫자는 정확히 유지해 줘.",
    L"다음 글의 뜻을 유지하면서 어색한 문장을 다듬어 줘. 중요한 수정은 짧게 설명해 줘."
};

static HWND window, task_box, source_box, status;
static HFONT font;
static UINT dpi;

static int px(int n) { return MulDiv(n, dpi, 96); }

static HWND control(HWND parent, const WCHAR *class_name, const WCHAR *text,
                    DWORD style, int id, int x, int y, int width, int height)
{
    HWND wnd = CreateWindowExW(id == SOURCE ? WS_EX_CLIENTEDGE : 0, class_name, text,
                               WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(width), px(height),
                               parent, (HMENU)(INT_PTR)id, g_hinst, NULL);
    if (wnd && font) SendMessageW(wnd, WM_SETFONT, (WPARAM)font, TRUE);
    return wnd;
}

static BOOL copy_prompt(void)
{
    size_t prefix, capacity;
    int n = GetWindowTextLengthW(source_box);
    int task = (int)SendMessageW(task_box, CB_GETCURSEL, 0, 0);
    HGLOBAL clipboard;
    WCHAR *buffer;
    BOOL copied = FALSE;

    if (n <= 0 || task < 0 || task >= (int)(sizeof(tasks) / sizeof(tasks[0]))) return FALSE;
    prefix = wcslen(instructions[task]);
    capacity = prefix + (size_t)n + 5;
    clipboard = GlobalAlloc(GMEM_MOVEABLE, capacity * sizeof(WCHAR));
    if (!clipboard) return FALSE;
    buffer = (WCHAR *)GlobalLock(clipboard);
    if (buffer) {
        wcscpy(buffer, instructions[task]);
        wcscpy(buffer + prefix, L"\r\n\r\n");
        GetWindowTextW(source_box, buffer + prefix + 4, n + 1);
        GlobalUnlock(clipboard);
        if (OpenClipboard(window)) {
            if (EmptyClipboard()) copied = SetClipboardData(CF_UNICODETEXT, clipboard) != NULL;
            CloseClipboard();
        }
    }
    if (!copied) GlobalFree(clipboard);
    return copied;
}

static LRESULT CALLBACK prompt_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    unsigned i;
    switch (message) {
    case WM_CREATE:
        font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Malgun Gothic");
        control(hwnd, L"STATIC", L"Claude에게 부탁할 작업", 0, 0, 20, 17, 380, 22);
        task_box = control(hwnd, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST,
                           TASK, 20, 41, 428, 165);
        for (i = 0; i < sizeof(tasks) / sizeof(tasks[0]); ++i)
            SendMessageW(task_box, CB_ADDSTRING, 0, (LPARAM)tasks[i]);
        SendMessageW(task_box, CB_SETCURSEL, 0, 0);
        control(hwnd, L"STATIC", L"질문에 넣을 내용", 0, 0, 20, 81, 245, 23);
        control(hwnd, L"BUTTON", L"붙여넣기", WS_TABSTOP, PASTE_SOURCE, 351, 76, 97, 26);
        source_box = control(hwnd, L"EDIT", L"", WS_TABSTOP | WS_VSCROLL |
                             ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                             SOURCE, 20, 109, 428, 159);
        SendMessageW(source_box, EM_SETLIMITTEXT, PROMPT_LIMIT, 0);
        status = control(hwnd, L"STATIC", L"복사한 질문은 Claude에서 직접 붙여넣어 보내세요.",
                         0, 0, 20, 281, 428, 23);
        control(hwnd, L"BUTTON", L"복사하고 Claude 열기", WS_TABSTOP | BS_DEFPUSHBUTTON,
                COPY_AND_OPEN, 200, 320, 157, 30);
        control(hwnd, L"BUTTON", L"닫기", WS_TABSTOP, IDCANCEL, 367, 320, 81, 30);
        return task_box && source_box && status ? 0 : -1;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case PASTE_SOURCE:
            SetFocus(source_box);
            SendMessageW(source_box, WM_PASTE, 0, 0);
            return 0;
        case IDOK:
        case COPY_AND_OPEN:
            if (!copy_prompt()) {
                SetWindowTextW(status, L"질문 내용을 입력하고 클립보드를 사용할 수 있는지 확인하세요.");
                return 0;
            }
            DestroyWindow(hwnd);
            secretary_open_claude();
            return 0;
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_ACTIVATE:
        secretary_dialog_state(SECRETARY_DIALOG_PROMPT, hwnd, LOWORD(wp) != WA_INACTIVE);
        break;
    case WM_DESTROY:
        secretary_dialog_state(SECRETARY_DIALOG_PROMPT, NULL, FALSE);
        window = task_box = source_box = status = NULL;
        if (font) DeleteObject(font);
        font = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

void secretary_prompt_open(void)
{
    WNDCLASSW cls = {0};
    POINT cursor = {0, 0};
    MONITORINFO monitor = {sizeof(monitor), {0}, {0}, 0};
    RECT r;
    HMONITOR display;
    DWORD style = WS_CAPTION | WS_SYSMENU;
    int w, h;

    if (window) { SetForegroundWindow(window); return; }
    cls.hInstance = g_hinst;
    cls.lpfnWndProc = prompt_proc;
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    cls.lpszClassName = PROMPT_CLASS;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    GetCursorPos(&cursor);
    display = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    dpi = dpi_for_monitor(display);
    GetMonitorInfoW(display, &monitor);
    r = (RECT){0, 0, px(468), px(370)};
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_DLGMODALFRAME);
    w = r.right - r.left; h = r.bottom - r.top;
    window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, PROMPT_CLASS,
                             L"Claude 질문 준비", style,
                             (monitor.rcWork.left + monitor.rcWork.right - w) / 2,
                             (monitor.rcWork.top + monitor.rcWork.bottom - h) / 2,
                             w, h, g_main_wnd, NULL, g_hinst, NULL);
    if (!window) return;
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(source_box);
}

void secretary_prompt_close(void) { if (window) DestroyWindow(window); }
BOOL secretary_prompt_message(MSG *message) { return window && IsDialogMessageW(window, message); }
BOOL secretary_prompt_has_focus(void) { return window && GetForegroundWindow() == window; }
