// snippet.c - text shortcut editor window (tray "Text shortcuts...").
//
// Lists the [shortcuts] of vim.ini and adds, changes or deletes them. Every
// change rewrites that section (shortcuts_save) and reloads the settings, so
// the live table in config.c stays the only copy.

#include "common.h"

#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define ED_CLASS   L"VimMouseSnippetEditor"
#define ED_TITLE   L"vim mouse - text shortcuts"

#define ID_LIST    2001
#define ID_ABBR    2002
#define ID_TEXT    2003
#define ID_SAVE    2004
#define ID_DELETE  2005
#define ID_NEW     2006
#define ID_CLOSE   2007
#define ID_ABBR_LABEL 2008
#define ID_TEXT_LABEL 2009
#define ID_IMPORT  2010
#define ID_EXPORT  2011

#define FILE_FILTER L"Text shortcuts (*.txt, *.ini)\0*.txt;*.ini\0All files (*.*)\0*.*\0"

#define LIST_PREVIEW 160

static HWND  g_ed_wnd;
static HWND  g_ed_list;
static HWND  g_ed_abbr;
static HWND  g_ed_text;
static HFONT g_ed_font;
static BOOL  g_ed_class_registered;

// ---------------------------------------------------------------------------
// Helpers

static WCHAR *window_text_dup(HWND wnd)
{
    int len = GetWindowTextLengthW(wnd);
    WCHAR *s = (WCHAR *)malloc(((size_t)len + 1) * sizeof(WCHAR));

    if (s == NULL)
        return NULL;
    GetWindowTextW(wnd, s, len + 1);
    s[len] = 0;
    return s;
}

// Edit controls want \r\n; the table keeps plain \n.
static WCHAR *to_crlf(const WCHAR *s)
{
    size_t n = 0, i, k = 0;
    WCHAR *out;

    for (i = 0; s[i]; i++)
        n += s[i] == L'\n' ? 2 : 1;
    out = (WCHAR *)malloc((n + 1) * sizeof(WCHAR));
    if (out == NULL)
        return NULL;
    for (i = 0; s[i]; i++) {
        if (s[i] == L'\n')
            out[k++] = L'\r';
        out[k++] = s[i];
    }
    out[k] = 0;
    return out;
}

static void strip_cr(WCHAR *s)
{
    WCHAR *d = s;

    for (; *s; s++) {
        if (*s != L'\r')
            *d++ = *s;
    }
    *d = 0;
}

static void read_abbr(WCHAR *buf, int cch)
{
    WCHAR raw[SHORTCUT_ABBR_MAX + 8];
    WCHAR *s = raw;
    WCHAR *e;
    int i;

    GetWindowTextW(g_ed_abbr, raw, (int)(sizeof(raw) / sizeof(raw[0])));
    while (*s == L' ' || *s == L'\t')
        s++;
    e = s + wcslen(s);
    while (e > s && (e[-1] == L' ' || e[-1] == L'\t'))
        e--;
    *e = 0;
    for (i = 0; s[i] && i + 1 < cch; i++)
        buf[i] = (s[i] >= L'A' && s[i] <= L'Z') ? (WCHAR)(s[i] + 32) : s[i];
    buf[i] = 0;
}

static int find_abbr(const WCHAR *abbr)
{
    WCHAR cur[SHORTCUT_ABBR_MAX + 1];
    int i, n = shortcut_count();

    for (i = 0; i < n; i++) {
        if (shortcut_get(i, cur, SHORTCUT_ABBR_MAX + 1, NULL) &&
            wcscmp(cur, abbr) == 0)
            return i;
    }
    return -1;
}

static UINT first_vk(WCHAR first)
{
    return (first >= L'a' && first <= L'z') ? (UINT)(first - 32) : (UINT)first;
}

// The first letter after Alt is taken by these before a shortcut can start.
static BOOL first_key_taken(WCHAR first)
{
    UINT vk = first_vk(first);
    const Hotkey *hk[3];
    int i;

    hk[0] = &g_cfg.hint_hotkey;
    hk[1] = &g_cfg.quit_hotkey;
    hk[2] = &g_cfg.pause_hotkey;
    for (i = 0; i < 3; i++) {
        if (hk[i]->vk == vk && hk[i]->mods == KMOD_ALT)
            return TRUE;
    }
    if (!g_cfg.alt_scroll)
        return FALSE;
    return vk == g_cfg.key_left || vk == g_cfg.key_down ||
           vk == g_cfg.key_up || vk == g_cfg.key_right ||
           vk == g_cfg.key_scroll_up || vk == g_cfg.key_scroll_down;
}

// ---------------------------------------------------------------------------
// List

static void refresh_list(const WCHAR *select)
{
    WCHAR abbr[SHORTCUT_ABBR_MAX + 1];
    WCHAR row[SHORTCUT_ABBR_MAX + LIST_PREVIEW + 16];
    int i, n, sel = -1;

    SendMessageW(g_ed_list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_ed_list, LB_RESETCONTENT, 0, 0);
    n = shortcut_count();
    for (i = 0; i < n; i++) {
        WCHAR *text = NULL;
        int k, j;

        if (!shortcut_get(i, abbr, SHORTCUT_ABBR_MAX + 1, &text))
            continue;
        _snwprintf(row, sizeof(row) / sizeof(row[0]), L"%ls\t", abbr);
        row[sizeof(row) / sizeof(row[0]) - 1] = 0;
        k = (int)wcslen(row);
        // One line: newlines show as a pilcrow, tabs as an arrow.
        for (j = 0; text[j] && j < LIST_PREVIEW; j++) {
            WCHAR ch = text[j];
            if (ch == L'\n')
                ch = 0x00B6;
            else if (ch == L'\t')
                ch = 0x2192;
            row[k++] = ch;
        }
        if (text[j])
            row[k++] = 0x2026;
        row[k] = 0;
        free(text);
        SendMessageW(g_ed_list, LB_ADDSTRING, 0, (LPARAM)row);
        if (select && wcscmp(abbr, select) == 0)
            sel = (int)SendMessageW(g_ed_list, LB_GETCOUNT, 0, 0) - 1;
    }
    SendMessageW(g_ed_list, LB_SETCURSEL, (WPARAM)sel, 0);
    SendMessageW(g_ed_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_ed_list, NULL, TRUE);
}

// The list rows are added in table order, so row i is shortcut i.
static void show_selected(void)
{
    WCHAR abbr[SHORTCUT_ABBR_MAX + 1];
    WCHAR *text = NULL;
    WCHAR *crlf;
    int sel = (int)SendMessageW(g_ed_list, LB_GETCURSEL, 0, 0);

    if (sel < 0 || !shortcut_get(sel, abbr, SHORTCUT_ABBR_MAX + 1, &text))
        return;
    SetWindowTextW(g_ed_abbr, abbr);
    crlf = to_crlf(text);
    SetWindowTextW(g_ed_text, crlf ? crlf : L"");
    free(crlf);
    free(text);
}

// ---------------------------------------------------------------------------
// Saving

// Writes the table with entry `skip` left out and, if abbr is set, abbr/text
// added or replaced. Returns TRUE once vim.ini is written and reloaded.
static BOOL save_table(int skip, const WCHAR *abbr, const WCHAR *text)
{
    WCHAR (*abbrs)[SHORTCUT_ABBR_MAX + 1];
    WCHAR **texts;
    const WCHAR **abbr_ptrs;
    int i, n = shortcut_count(), k = 0;
    BOOL ok = FALSE;

    abbrs = malloc(sizeof(*abbrs) * (size_t)(n + 1));
    texts = (WCHAR **)calloc((size_t)n + 1, sizeof(WCHAR *));
    abbr_ptrs = (const WCHAR **)calloc((size_t)n + 1, sizeof(WCHAR *));
    if (abbrs && texts && abbr_ptrs) {
        BOOL replaced = FALSE;

        for (i = 0; i < n; i++) {
            if (i == skip ||
                !shortcut_get(i, abbrs[k], SHORTCUT_ABBR_MAX + 1, &texts[k]))
                continue;
            if (abbr && wcscmp(abbrs[k], abbr) == 0) {
                free(texts[k]);
                texts[k] = _wcsdup(text);
                replaced = TRUE;
            }
            abbr_ptrs[k] = abbrs[k];
            k++;
        }
        if (abbr && !replaced) {
            lstrcpynW(abbrs[k], abbr, SHORTCUT_ABBR_MAX + 1);
            texts[k] = _wcsdup(text);
            abbr_ptrs[k] = abbrs[k];
            k++;
        }
        ok = shortcuts_save(abbr_ptrs, (const WCHAR *const *)texts, k);
    }
    if (texts) {
        for (i = 0; i < k; i++)
            free(texts[i]);
    }
    free(texts);
    free(abbr_ptrs);
    free(abbrs);

    if (!ok) {
        MessageBoxW(g_ed_wnd, L"The settings file could not be written.",
                    ED_TITLE, MB_OK | MB_ICONERROR);
        return FALSE;
    }
    app_config_written();
    return TRUE;
}

static void on_save(void)
{
    WCHAR abbr[SHORTCUT_ABBR_MAX + 8];
    WCHAR *text;
    int existing;

    read_abbr(abbr, (int)(sizeof(abbr) / sizeof(abbr[0])));
    if (!shortcut_abbr_valid(abbr)) {
        MessageBoxW(g_ed_wnd,
                    L"The abbreviation must be 1-31 letters (a-z) or digits.",
                    ED_TITLE, MB_OK | MB_ICONWARNING);
        SetFocus(g_ed_abbr);
        return;
    }
    text = window_text_dup(g_ed_text);
    if (text == NULL)
        return;
    strip_cr(text);
    if (text[0] == 0) {
        free(text);
        MessageBoxW(g_ed_wnd, L"Enter the text to type.", ED_TITLE,
                    MB_OK | MB_ICONWARNING);
        SetFocus(g_ed_text);
        return;
    }
    existing = find_abbr(abbr);
    if (existing < 0 && shortcut_count() >= SHORTCUT_MAX) {
        free(text);
        MessageBoxW(g_ed_wnd, L"There are already 200 shortcuts.", ED_TITLE,
                    MB_OK | MB_ICONWARNING);
        return;
    }
    if (first_key_taken(abbr[0])) {
        WCHAR msg[256];
        WCHAR key[16];
        format_key(first_vk(abbr[0]), key, 16);
        _snwprintf(msg, 256,
                   L"Alt+%ls is already a vim mouse key (scrolling or a "
                   L"hotkey), so this shortcut cannot be typed.\n\n"
                   L"Save it anyway?", key);
        msg[255] = 0;
        if (MessageBoxW(g_ed_wnd, msg, ED_TITLE,
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            free(text);
            return;
        }
    }
    if (save_table(-1, abbr, text))
        refresh_list(abbr);
    free(text);
}

static void on_delete(void)
{
    int sel = (int)SendMessageW(g_ed_list, LB_GETCURSEL, 0, 0);

    if (sel < 0)
        return;
    if (save_table(sel, NULL, NULL)) {
        refresh_list(NULL);
        SetWindowTextW(g_ed_abbr, L"");
        SetWindowTextW(g_ed_text, L"");
    }
}

// ---------------------------------------------------------------------------
// Import / export

static BOOL ask_file(BOOL save, WCHAR *path, DWORD cch)
{
    OPENFILENAMEW ofn;

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_ed_wnd;
    ofn.lpstrFilter = FILE_FILTER;
    ofn.lpstrFile = path;
    ofn.nMaxFile = cch;
    ofn.lpstrDefExt = L"txt";
    if (save) {
        ofn.lpstrTitle = L"Export text shortcuts";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        return GetSaveFileNameW(&ofn);
    }
    ofn.lpstrTitle = L"Import text shortcuts";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn);
}

static void on_export(void)
{
    WCHAR path[MAX_PATH] = L"vim-mouse-shortcuts.txt";
    WCHAR msg[MAX_PATH + 64];

    if (shortcut_count() == 0) {
        MessageBoxW(g_ed_wnd, L"There are no text shortcuts to export.",
                    ED_TITLE, MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!ask_file(TRUE, path, MAX_PATH))
        return;
    if (!shortcuts_export(path)) {
        MessageBoxW(g_ed_wnd, L"The file could not be written.", ED_TITLE,
                    MB_OK | MB_ICONERROR);
        return;
    }
    _snwprintf(msg, sizeof(msg) / sizeof(msg[0]),
               L"Exported %d text shortcuts to\n%ls", shortcut_count(), path);
    msg[sizeof(msg) / sizeof(msg[0]) - 1] = 0;
    MessageBoxW(g_ed_wnd, msg, ED_TITLE, MB_OK | MB_ICONINFORMATION);
}

// Adds the file's shortcuts to the current ones. When some abbreviations
// exist already, the user picks: replace them, or keep the current ones.
static void on_import(void)
{
    WCHAR path[MAX_PATH] = L"";
    WCHAR msg[512];
    ShortcutEntry *in;
    WCHAR (*abbrs)[SHORTCUT_ABBR_MAX + 1] = NULL;
    WCHAR **texts = NULL;
    const WCHAR **abbr_ptrs = NULL;
    int count = 0, skipped = 0, clashes = 0, added = 0, replaced = 0;
    int dropped = 0, i, j, n, k = 0, answer = IDYES;
    BOOL ok = FALSE;

    if (!ask_file(FALSE, path, MAX_PATH))
        return;
    in = shortcuts_read_file(path, &count, &skipped);
    if (in == NULL) {
        MessageBoxW(g_ed_wnd, L"The file could not be read.", ED_TITLE,
                    MB_OK | MB_ICONERROR);
        return;
    }
    if (count == 0) {
        shortcut_entries_free(in, count);
        MessageBoxW(g_ed_wnd,
                    L"The file has no text shortcuts (lines like "
                    L"\"reor = text\").", ED_TITLE, MB_OK | MB_ICONWARNING);
        return;
    }
    n = shortcut_count();
    for (i = 0; i < count; i++) {
        if (find_abbr(in[i].abbr) >= 0)
            clashes++;
    }
    if (clashes > 0) {
        _snwprintf(msg, 512,
                   L"%d of the %d shortcuts in the file already exist.\n\n"
                   L"Yes: replace them with the imported ones\n"
                   L"No: keep the current ones and add only the new shortcuts",
                   clashes, count);
        msg[511] = 0;
        answer = MessageBoxW(g_ed_wnd, msg, ED_TITLE,
                             MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL) {
            shortcut_entries_free(in, count);
            return;
        }
    }

    abbrs = malloc(sizeof(*abbrs) * (size_t)(n + count));
    texts = (WCHAR **)calloc((size_t)(n + count), sizeof(WCHAR *));
    abbr_ptrs = (const WCHAR **)calloc((size_t)(n + count), sizeof(WCHAR *));
    if (abbrs && texts && abbr_ptrs) {
        for (i = 0; i < n; i++) {
            if (shortcut_get(i, abbrs[k], SHORTCUT_ABBR_MAX + 1, &texts[k])) {
                abbr_ptrs[k] = abbrs[k];
                k++;
            }
        }
        for (i = 0; i < count; i++) {
            for (j = 0; j < k; j++) {
                if (wcscmp(abbrs[j], in[i].abbr) == 0)
                    break;
            }
            if (j < k) {
                if (answer == IDYES) {
                    free(texts[j]);
                    texts[j] = _wcsdup(in[i].text);
                    replaced++;
                }
                continue;
            }
            if (k >= SHORTCUT_MAX) {
                dropped++;
                continue;
            }
            lstrcpynW(abbrs[k], in[i].abbr, SHORTCUT_ABBR_MAX + 1);
            texts[k] = _wcsdup(in[i].text);
            abbr_ptrs[k] = abbrs[k];
            k++;
            added++;
        }
        ok = shortcuts_save(abbr_ptrs, (const WCHAR *const *)texts, k);
    }
    if (texts) {
        for (i = 0; i < k; i++)
            free(texts[i]);
    }
    free(texts);
    free(abbr_ptrs);
    free(abbrs);
    shortcut_entries_free(in, count);

    if (!ok) {
        MessageBoxW(g_ed_wnd, L"The settings file could not be written.",
                    ED_TITLE, MB_OK | MB_ICONERROR);
        return;
    }
    app_config_written();
    refresh_list(NULL);
    _snwprintf(msg, 512, L"Imported: %d added, %d replaced.", added, replaced);
    if (skipped > 0)
        _snwprintf(msg + wcslen(msg), 512 - wcslen(msg),
                   L"\n%d lines were not valid shortcuts and were skipped.",
                   skipped);
    if (dropped > 0)
        _snwprintf(msg + wcslen(msg), 512 - wcslen(msg),
                   L"\n%d were not added: the limit is %d shortcuts.",
                   dropped, SHORTCUT_MAX);
    msg[511] = 0;
    MessageBoxW(g_ed_wnd, msg, ED_TITLE, MB_OK | MB_ICONINFORMATION);
}

static void on_new(void)
{
    SendMessageW(g_ed_list, LB_SETCURSEL, (WPARAM)-1, 0);
    SetWindowTextW(g_ed_abbr, L"");
    SetWindowTextW(g_ed_text, L"");
    SetFocus(g_ed_abbr);
}

// ---------------------------------------------------------------------------
// Window

static HWND add_control(const WCHAR *cls, const WCHAR *text, DWORD style,
                        DWORD ex_style, int id)
{
    HWND wnd = CreateWindowExW(ex_style, cls, text, WS_CHILD | WS_VISIBLE | style,
                               0, 0, 10, 10, g_ed_wnd,
                               (HMENU)(INT_PTR)id, g_hinst, NULL);
    if (wnd && g_ed_font)
        SendMessageW(wnd, WM_SETFONT, (WPARAM)g_ed_font, FALSE);
    return wnd;
}

static void layout(int width, int height, UINT dpi)
{
    int pad = MulDiv(12, (int)dpi, 96);
    int gap = MulDiv(8, (int)dpi, 96);
    int label_h = MulDiv(18, (int)dpi, 96);
    int edit_h = MulDiv(24, (int)dpi, 96);
    int text_h = MulDiv(72, (int)dpi, 96);
    int btn_w = MulDiv(84, (int)dpi, 96);
    int btn_h = MulDiv(28, (int)dpi, 96);
    int inner = width - pad * 2;
    int list_h, y;

    // Bottom up: buttons, text, abbreviation; the list takes the rest.
    list_h = height - pad * 2 - btn_h - gap - (label_h + text_h + gap) -
             (label_h + edit_h + gap) - gap;
    if (list_h < MulDiv(60, (int)dpi, 96))
        list_h = MulDiv(60, (int)dpi, 96);
    y = pad;
    MoveWindow(g_ed_list, pad, y, inner, list_h, TRUE);
    y += list_h + gap * 2;
    MoveWindow(GetDlgItem(g_ed_wnd, ID_ABBR_LABEL), pad, y, inner, label_h, TRUE);
    y += label_h;
    MoveWindow(g_ed_abbr, pad, y, MulDiv(220, (int)dpi, 96), edit_h, TRUE);
    y += edit_h + gap;
    MoveWindow(GetDlgItem(g_ed_wnd, ID_TEXT_LABEL), pad, y, inner, label_h, TRUE);
    y += label_h;
    MoveWindow(g_ed_text, pad, y, inner, text_h, TRUE);
    y += text_h + gap;
    MoveWindow(GetDlgItem(g_ed_wnd, ID_SAVE), pad, y, btn_w, btn_h, TRUE);
    MoveWindow(GetDlgItem(g_ed_wnd, ID_DELETE), pad + btn_w + gap, y, btn_w,
               btn_h, TRUE);
    MoveWindow(GetDlgItem(g_ed_wnd, ID_NEW), pad + (btn_w + gap) * 2, y, btn_w,
               btn_h, TRUE);
    MoveWindow(GetDlgItem(g_ed_wnd, ID_IMPORT), pad + (btn_w + gap) * 3, y,
               btn_w, btn_h, TRUE);
    MoveWindow(GetDlgItem(g_ed_wnd, ID_EXPORT), pad + (btn_w + gap) * 4, y,
               btn_w, btn_h, TRUE);
    MoveWindow(GetDlgItem(g_ed_wnd, ID_CLOSE), width - pad - btn_w, y, btn_w,
               btn_h, TRUE);
}

static void create_controls(void)
{
    int tab = 60;   // dialog units

    g_ed_list = add_control(L"LISTBOX", L"",
                            WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                                LBS_NOINTEGRALHEIGHT | LBS_USETABSTOPS,
                            WS_EX_CLIENTEDGE, ID_LIST);
    SendMessageW(g_ed_list, LB_SETTABSTOPS, 1, (LPARAM)&tab);
    add_control(L"STATIC",
                L"Abbreviation (letters and digits; hold Alt, type it, release Alt):",
                SS_NOPREFIX, 0, ID_ABBR_LABEL);
    g_ed_abbr = add_control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
                            WS_EX_CLIENTEDGE, ID_ABBR);
    SendMessageW(g_ed_abbr, EM_LIMITTEXT, SHORTCUT_ABBR_MAX, 0);
    add_control(L"STATIC", L"Text to type:", SS_NOPREFIX, 0, ID_TEXT_LABEL);
    g_ed_text = add_control(L"EDIT", L"",
                            WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
                                ES_AUTOVSCROLL | ES_WANTRETURN,
                            WS_EX_CLIENTEDGE, ID_TEXT);
    SendMessageW(g_ed_text, EM_LIMITTEXT, SHORTCUT_TEXT_MAX, 0);
    add_control(L"BUTTON", L"&Add / Update", WS_TABSTOP | BS_DEFPUSHBUTTON, 0,
                ID_SAVE);
    add_control(L"BUTTON", L"&Delete", WS_TABSTOP | BS_PUSHBUTTON, 0,
                ID_DELETE);
    add_control(L"BUTTON", L"&New", WS_TABSTOP | BS_PUSHBUTTON, 0, ID_NEW);
    add_control(L"BUTTON", L"&Import...", WS_TABSTOP | BS_PUSHBUTTON, 0,
                ID_IMPORT);
    add_control(L"BUTTON", L"E&xport...", WS_TABSTOP | BS_PUSHBUTTON, 0,
                ID_EXPORT);
    add_control(L"BUTTON", L"Close", WS_TABSTOP | BS_PUSHBUTTON, 0, ID_CLOSE);
}

static LRESULT CALLBACK ed_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE)
                show_selected();
            return 0;
        case ID_SAVE:
        case IDOK:
            on_save();
            return 0;
        case ID_DELETE:
            on_delete();
            return 0;
        case ID_NEW:
            on_new();
            return 0;
        case ID_IMPORT:
            on_import();
            return 0;
        case ID_EXPORT:
            on_export();
            return 0;
        case ID_CLOSE:
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case DM_GETDEFID:
        return MAKELONG(ID_SAVE, DC_HASDEFID);
    case WM_SIZE:
        if (g_ed_list)
            layout(LOWORD(lp), HIWORD(lp), dpi_for_monitor(
                       MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)));
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        UINT dpi = dpi_for_monitor(MonitorFromWindow(hwnd,
                                                     MONITOR_DEFAULTTONEAREST));
        mm->ptMinTrackSize.x = MulDiv(580, (int)dpi, 96);
        mm->ptMinTrackSize.y = MulDiv(340, (int)dpi, 96);
        return 0;
    }
    case WM_DESTROY:
        g_ed_wnd = NULL;
        g_ed_list = NULL;
        g_ed_abbr = NULL;
        g_ed_text = NULL;
        if (g_ed_font) {
            DeleteObject(g_ed_font);
            g_ed_font = NULL;
        }
        app_log(L"shortcuts: editor closed");
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static BOOL ed_register_class(void)
{
    WNDCLASSEXW wc;

    if (g_ed_class_registered)
        return TRUE;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = ed_wnd_proc;
    wc.hInstance     = g_hinst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon         = app_load_icon(GetSystemMetrics(SM_CXICON),
                                     GetSystemMetrics(SM_CYICON));
    wc.hIconSm       = app_load_icon(GetSystemMetrics(SM_CXSMICON),
                                     GetSystemMetrics(SM_CYSMICON));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = ED_CLASS;
    if (!RegisterClassExW(&wc))
        return FALSE;
    g_ed_class_registered = TRUE;
    return TRUE;
}

void snippet_editor_open(void)
{
    POINT pt = { 0, 0 };
    HMONITOR mon;
    MONITORINFO mi;
    RECT work, rc;
    UINT dpi;
    int width, height, x, y;
    DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX;

    if (g_ed_wnd) {
        if (IsIconic(g_ed_wnd))
            ShowWindow(g_ed_wnd, SW_RESTORE);
        SetForegroundWindow(g_ed_wnd);
        return;
    }
    if (!ed_register_class())
        return;

    GetCursorPos(&pt);
    mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    dpi = dpi_for_monitor(mon);
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi))
        work = mi.rcWork;
    else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    g_ed_font = CreateFontW(-MulDiv(15, (int)dpi, 96), 0, 0, 0, FW_NORMAL,
                            FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                            L"Segoe UI");

    SetRect(&rc, 0, 0, MulDiv(640, (int)dpi, 96), MulDiv(440, (int)dpi, 96));
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_CONTROLPARENT);
    width = rc.right - rc.left;
    height = rc.bottom - rc.top;
    x = work.left + ((int)(work.right - work.left) - width) / 2;
    y = work.top + ((int)(work.bottom - work.top) - height) / 2;
    if (x < work.left)
        x = work.left;
    if (y < work.top)
        y = work.top;

    g_ed_wnd = CreateWindowExW(WS_EX_CONTROLPARENT, ED_CLASS, ED_TITLE, style,
                               x, y, width, height, NULL, NULL, g_hinst, NULL);
    if (!g_ed_wnd) {
        app_log(L"shortcuts: CreateWindowEx failed (%lu)", GetLastError());
        if (g_ed_font) {
            DeleteObject(g_ed_font);
            g_ed_font = NULL;
        }
        return;
    }
    create_controls();
    GetClientRect(g_ed_wnd, &rc);
    layout(rc.right, rc.bottom, dpi);
    refresh_list(NULL);

    ShowWindow(g_ed_wnd, SW_SHOW);
    SetForegroundWindow(g_ed_wnd);
    SetFocus(g_ed_abbr);
    app_log(L"shortcuts: editor opened");
}

BOOL snippet_editor_dialog_message(MSG *msg)
{
    return g_ed_wnd != NULL && IsDialogMessageW(g_ed_wnd, msg);
}

void snippet_editor_close(void)
{
    if (g_ed_wnd)
        DestroyWindow(g_ed_wnd);
}

// ---------------------------------------------------------------------------
// Inserting a shortcut's text

// Pasting is the reliable way into browsers (address bar, web editors) and
// past an IME, where injected characters can get lost. The clipboard is
// saved first and put back once the app has had time to paste.
#define RESTORE_DELAY_MS 600
#define SAVE_LIMIT       (64 * 1024 * 1024)

typedef struct {
    UINT    format;
    HGLOBAL data;
} SavedFormat;

static SavedFormat *g_saved;
static int   g_saved_count;
static BOOL  g_saved_valid;     // g_saved holds the user's clipboard
static DWORD g_paste_seq;       // clipboard sequence number after our text
static UINT_PTR g_restore_timer;

static BOOL open_clipboard(void)
{
    int i;

    // Another program may hold it for a moment.
    for (i = 0; i < 20; i++) {
        if (OpenClipboard(g_main_wnd))
            return TRUE;
        Sleep(10);
    }
    return FALSE;
}

static void saved_free(void)
{
    int i;

    for (i = 0; i < g_saved_count; i++)
        GlobalFree(g_saved[i].data);
    free(g_saved);
    g_saved = NULL;
    g_saved_count = 0;
    g_saved_valid = FALSE;
}

// Formats whose handle is not memory (bitmaps, metafiles, palettes) are
// skipped; Windows synthesizes most of them from the ones that are.
static BOOL format_is_memory(UINT f)
{
    switch (f) {
    case CF_BITMAP: case CF_METAFILEPICT: case CF_PALETTE:
    case CF_ENHMETAFILE: case CF_OWNERDISPLAY: case CF_DSPBITMAP:
    case CF_DSPMETAFILEPICT: case CF_DSPENHMETAFILE:
        return FALSE;
    }
    return !(f >= CF_GDIOBJFIRST && f <= CF_GDIOBJLAST);
}

// Call with the clipboard open.
static void save_clipboard(void)
{
    UINT f = 0;
    SIZE_T total = 0;
    int cap = 0;

    saved_free();
    while ((f = EnumClipboardFormats(f)) != 0) {
        HANDLE h;
        SIZE_T size;
        void *src, *dst;
        HGLOBAL copy;

        if (!format_is_memory(f))
            continue;
        h = GetClipboardData(f);
        if (!h)
            continue;
        size = GlobalSize(h);
        if (size == 0)
            continue;
        total += size;
        if (total > SAVE_LIMIT) {
            app_log(L"shortcuts: clipboard too large to save, not restored");
            saved_free();
            return;
        }
        copy = GlobalAlloc(GMEM_MOVEABLE, size);
        if (!copy)
            continue;
        src = GlobalLock(h);
        dst = GlobalLock(copy);
        if (src && dst)
            memcpy(dst, src, size);
        if (src)
            GlobalUnlock(h);
        if (dst)
            GlobalUnlock(copy);
        if (!src || !dst) {
            GlobalFree(copy);
            continue;
        }
        if (g_saved_count == cap) {
            int ncap = cap ? cap * 2 : 8;
            SavedFormat *grown = (SavedFormat *)realloc(
                g_saved, (size_t)ncap * sizeof(SavedFormat));
            if (!grown) {
                GlobalFree(copy);
                continue;
            }
            g_saved = grown;
            cap = ncap;
        }
        g_saved[g_saved_count].format = f;
        g_saved[g_saved_count].data = copy;
        g_saved_count++;
    }
    g_saved_valid = TRUE;
}

static void set_global(UINT format, const void *data, SIZE_T size)
{
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, size);
    void *p;

    if (!h)
        return;
    p = GlobalLock(h);
    if (!p) {
        GlobalFree(h);
        return;
    }
    memcpy(p, data, size);
    GlobalUnlock(h);
    if (!SetClipboardData(format, h))
        GlobalFree(h);
}

// Keeps what we put on the clipboard out of Win+V history and cloud sync.
static void exclude_from_history(void)
{
    static UINT history, cloud, monitor;
    DWORD zero = 0;

    if (!history) {
        history = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
        cloud = RegisterClipboardFormatW(L"CanUploadToCloudClipboard");
        monitor = RegisterClipboardFormatW(
            L"ExcludeClipboardContentFromMonitorProcessing");
    }
    if (history)
        set_global(history, &zero, sizeof(zero));
    if (cloud)
        set_global(cloud, &zero, sizeof(zero));
    if (monitor)
        set_global(monitor, &zero, sizeof(zero));
}

static void CALLBACK restore_clipboard(HWND hwnd, UINT msg, UINT_PTR id,
                                       DWORD tick)
{
    int i;

    (void)hwnd; (void)msg; (void)tick;
    KillTimer(NULL, id);
    g_restore_timer = 0;
    // Something new was copied meanwhile: keep it.
    if (!g_saved_valid || GetClipboardSequenceNumber() != g_paste_seq) {
        saved_free();
        return;
    }
    if (!open_clipboard()) {
        saved_free();
        return;
    }
    EmptyClipboard();
    for (i = 0; i < g_saved_count; i++) {
        // On success the clipboard owns the memory.
        if (SetClipboardData(g_saved[i].format, g_saved[i].data))
            g_saved[i].data = NULL;
    }
    exclude_from_history();
    CloseClipboard();
    for (i = 0; i < g_saved_count; i++) {
        if (g_saved[i].data)
            GlobalFree(g_saved[i].data);
    }
    free(g_saved);
    g_saved = NULL;
    g_saved_count = 0;
    g_saved_valid = FALSE;
}

static void send_ctrl_v(void)
{
    INPUT in[4];
    int i;

    ZeroMemory(in, sizeof(in));
    for (i = 0; i < 4; i++)
        in[i].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[1].ki.wVk = 'V';
    in[2].ki.wVk = 'V';
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_CONTROL;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    for (i = 0; i < 4; i++)
        in[i].ki.wScan = (WORD)MapVirtualKeyW(in[i].ki.wVk, 0);
    SendInput(4, in, sizeof(INPUT));
}

static BOOL paste_text(const WCHAR *text)
{
    WCHAR *crlf = to_crlf(text);

    if (!crlf || !open_clipboard()) {
        free(crlf);
        return FALSE;
    }
    // A paste still waiting to restore: the clipboard holds our last text,
    // and the user's clipboard is already saved.
    if (!g_saved_valid)
        save_clipboard();
    EmptyClipboard();
    set_global(CF_UNICODETEXT, crlf, (wcslen(crlf) + 1) * sizeof(WCHAR));
    exclude_from_history();
    CloseClipboard();
    free(crlf);
    g_paste_seq = GetClipboardSequenceNumber();

    send_ctrl_v();
    if (g_restore_timer)
        KillTimer(NULL, g_restore_timer);
    g_restore_timer = SetTimer(NULL, 0, RESTORE_DELAY_MS, restore_clipboard);
    return TRUE;
}

void snippet_insert(const WCHAR *text)
{
    if (text == NULL || text[0] == 0)
        return;
    if (g_cfg.shortcut_paste && paste_text(text))
        return;
    input_type_text(text);
}
