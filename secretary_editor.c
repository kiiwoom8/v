// Native editor for the nine launch slots in [secretary].
#include "common.h"
#include <wchar.h>

#define EDITOR_CLASS L"VimMouseSecretaryEditor"

enum { SLOT_BOX = 101, LABEL_BOX, TARGET_BOX, SAVE_SLOT, CLEAR_SLOT };

static HWND editor, slot_box, label_box, target_box, status;
static HFONT font;
static UINT dpi;
static int selected_slot;
static SecretaryShortcut drafts[SECRETARY_SLOTS];

static int px(int n) { return MulDiv(n, dpi, 96); }

static HWND control(HWND parent, const WCHAR *class_name, const WCHAR *text,
                    DWORD style, int id, int x, int y, int width, int height)
{
    HWND wnd = CreateWindowExW(wcscmp(class_name, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0,
                              class_name, text, WS_CHILD | WS_VISIBLE | style,
                              px(x), px(y), px(width), px(height), parent,
                              (HMENU)(INT_PTR)id, g_hinst, NULL);
    if (wnd && font) SendMessageW(wnd, WM_SETFONT, (WPARAM)font, TRUE);
    return wnd;
}

static void refresh_slots(void)
{
    WCHAR text[104];
    int n;
    SendMessageW(slot_box, CB_RESETCONTENT, 0, 0);
    for (n = 0; n < SECRETARY_SLOTS; ++n) {
        _snwprintf(text, sizeof(text) / sizeof(*text), L"%d   %ls", n + 1,
                   g_cfg.secretary_shortcuts[n].label[0] ? g_cfg.secretary_shortcuts[n].label : L"비어 있음");
        SendMessageW(slot_box, CB_ADDSTRING, 0, (LPARAM)text);
    }
    SendMessageW(slot_box, CB_SETCURSEL, selected_slot, 0);
}

static void select_draft(int slot)
{
    selected_slot = slot;
    SetWindowTextW(label_box, drafts[slot].label);
    SetWindowTextW(target_box, drafts[slot].target);
    SetWindowTextW(status, L"");
}

static void get_trimmed(HWND wnd, WCHAR *text, int length)
{
    WCHAR *start = text, *end;
    GetWindowTextW(wnd, text, length);
    while (*start == L' ' || *start == L'\t') ++start;
    end = start + wcslen(start);
    while (end > start && (end[-1] == L' ' || end[-1] == L'\t')) --end;
    MoveMemory(text, start, (size_t)(end - start) * sizeof(WCHAR));
    text[end - start] = 0;
}

static void remember_draft(void)
{
    SecretaryShortcut *slot = &drafts[selected_slot];
    get_trimmed(label_box, slot->label, sizeof(slot->label) / sizeof(WCHAR));
    get_trimmed(target_box, slot->target, SECRETARY_TARGET_MAX);
}

static void save_slot(void)
{
    remember_draft();
    if (!secretary_shortcut_valid(&drafts[selected_slot])) {
        SetWindowTextW(status, L"이름과 claude 또는 http(s) 주소를 입력하세요.");
        return;
    }
    if (!secretary_save_setting(selected_slot, &drafts[selected_slot], 0)) {
        SetWindowTextW(status, L"설정을 저장하지 못했습니다. 다시 시도하세요.");
        return;
    }
    app_config_written();
    refresh_slots();
    SetWindowTextW(status, L"저장했습니다.");
}

static LRESULT CALLBACK editor_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    switch (message) {
    case WM_CREATE:
        font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Malgun Gothic");
        control(hwnd, L"STATIC", L"번호", 0, 0, 20, 19, 70, 24);
        slot_box = control(hwnd, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST,
                           SLOT_BOX, 20, 43, 408, 250);
        control(hwnd, L"STATIC", L"이름", 0, 0, 20, 87, 70, 22);
        label_box = control(hwnd, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
                            LABEL_BOX, 20, 111, 408, 28);
        control(hwnd, L"STATIC", L"열기", 0, 0, 20, 153, 70, 22);
        target_box = control(hwnd, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
                             TARGET_BOX, 20, 177, 408, 28);
        control(hwnd, L"STATIC", L"Claude 앱은 claude, 웹은 주소를 입력하세요.",
                0, 0, 20, 214, 408, 24);
        status = control(hwnd, L"STATIC", L"", 0, 0, 20, 247, 408, 23);
        control(hwnd, L"BUTTON", L"비우기", WS_TABSTOP | BS_PUSHBUTTON,
                CLEAR_SLOT, 20, 281, 82, 30);
        control(hwnd, L"BUTTON", L"저장", WS_TABSTOP | BS_DEFPUSHBUTTON,
                SAVE_SLOT, 252, 281, 82, 30);
        control(hwnd, L"BUTTON", L"닫기", WS_TABSTOP | BS_PUSHBUTTON,
                IDCANCEL, 346, 281, 82, 30);
        SendMessageW(label_box, EM_SETLIMITTEXT, sizeof(drafts[0].label) / sizeof(WCHAR) - 1, 0);
        SendMessageW(target_box, EM_SETLIMITTEXT, SECRETARY_TARGET_MAX - 1, 0);
        if (!slot_box || !label_box || !target_box || !status) return -1;
        refresh_slots();
        select_draft(selected_slot);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case SLOT_BOX:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int slot = (int)SendMessageW(slot_box, CB_GETCURSEL, 0, 0);
                if (slot >= 0 && slot < SECRETARY_SLOTS) {
                    remember_draft();
                    select_draft(slot);
                }
            }
            return 0;
        case IDOK:
        case SAVE_SLOT:
            save_slot();
            return 0;
        case CLEAR_SLOT:
            ZeroMemory(&drafts[selected_slot], sizeof(drafts[selected_slot]));
            select_draft(selected_slot);
            SetFocus(label_box);
            return 0;
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_DESTROY:
        editor = slot_box = label_box = target_box = status = NULL;
        if (font) DeleteObject(font);
        font = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

void secretary_editor_open(void)
{
    POINT cursor = {0, 0};
    MONITORINFO monitor = {sizeof(monitor), {0}, {0}, 0};
    RECT r;
    WNDCLASSW cls = {0};
    HMONITOR display;
    const DWORD style = WS_CAPTION | WS_SYSMENU;
    int w, h;

    if (editor) {
        SetForegroundWindow(editor);
        return;
    }
    cls.hInstance = g_hinst;
    cls.lpfnWndProc = editor_proc;
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    cls.lpszClassName = EDITOR_CLASS;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    GetCursorPos(&cursor);
    display = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    dpi = dpi_for_monitor(display);
    GetMonitorInfoW(display, &monitor);
    r = (RECT){0, 0, px(448), px(332)};
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_DLGMODALFRAME);
    w = r.right - r.left;
    h = r.bottom - r.top;
    CopyMemory(drafts, g_cfg.secretary_shortcuts, sizeof(drafts));
    selected_slot = 0;
    editor = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                             EDITOR_CLASS, L"비서 바로가기 편집", style,
                             (monitor.rcWork.left + monitor.rcWork.right - w) / 2,
                             (monitor.rcWork.top + monitor.rcWork.bottom - h) / 2,
                             w, h, g_main_wnd, NULL, g_hinst, NULL);
    if (!editor) return;
    ShowWindow(editor, SW_SHOW);
    SetForegroundWindow(editor);
    SetFocus(slot_box);
}

void secretary_editor_close(void) { if (editor) DestroyWindow(editor); }
BOOL secretary_editor_message(MSG *message) { return editor && IsDialogMessageW(editor, message); }
BOOL secretary_editor_visible(void) { return editor != NULL; }
BOOL secretary_editor_has_focus(void) { return editor && GetForegroundWindow() == editor; }
