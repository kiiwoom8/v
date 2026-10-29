// config.c - settings file (vim.ini next to the exe, or in %APPDATA% when that
// folder is read-only) and key name handling.

#ifndef _WIN32_IE
#define _WIN32_IE 0x0601
#endif
#include "common.h"

#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef MAPVK_VK_TO_CHAR
#define MAPVK_VK_TO_CHAR 2
#endif
#ifndef VK_APPS
#define VK_APPS 0x5D
#endif
#ifndef VK_F24
#define VK_F24 0x87
#endif

static WCHAR g_exe_ini[APP_PATH_MAX];
static WCHAR g_appdata_ini[APP_PATH_MAX];
static BOOL  g_exe_ini_ready = FALSE;
static WCHAR g_warnings[4096];

typedef struct {
    WCHAR  abbr[SHORTCUT_ABBR_MAX + 1];
    WCHAR *text;
} Shortcut;

// The live table (read by the keyboard hook) and the one being parsed.
static CRITICAL_SECTION g_sc_lock;
static LONG     g_sc_lock_state = 0;   // 0 = not created, 1 = creating, 2 = ready
static Shortcut g_sc[SHORTCUT_MAX];
static int      g_sc_count = 0;
static Shortcut g_sc_new[SHORTCUT_MAX];
static int      g_sc_new_count = 0;

// ---------------------------------------------------------------------------
// Small helpers

static int is_space(WCHAR ch)
{
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

static WCHAR lower_ascii(WCHAR ch)
{
    if (ch >= L'A' && ch <= L'Z')
        return (WCHAR)(ch + 32);
    return ch;
}

static void trim(WCHAR **ps)
{
    WCHAR *s = *ps;
    WCHAR *e;

    while (*s && is_space(*s))
        s++;
    e = s + wcslen(s);
    while (e > s && is_space(e[-1]))
        e--;
    *e = 0;
    *ps = s;
}

static void warn_add(int line, const WCHAR *fmt, ...)
{
    WCHAR msg[512];
    WCHAR full[560];
    size_t len, room;
    va_list ap;

    va_start(ap, fmt);
    _vsnwprintf(msg, 512, fmt, ap);
    va_end(ap);
    msg[511] = 0;

    if (line > 0)
        _snwprintf(full, 560, L"line %d: %ls", line, msg);
    else
        wcsncpy(full, msg, 559);
    full[559] = 0;

    len = wcslen(g_warnings);
    room = sizeof(g_warnings) / sizeof(g_warnings[0]) - len - 1;
    if (room <= 1)
        return;
    if (len > 0) {
        g_warnings[len++] = L'\n';
        room--;
    }
    wcsncat(g_warnings, full, room);
}

static BOOL parse_long(const WCHAR *s, long *out)
{
    WCHAR *end;
    long v;

    if (s == NULL || *s == 0)
        return FALSE;
    v = wcstol(s, &end, 10);
    if (end == s)
        return FALSE;
    while (*end && is_space(*end))
        end++;
    if (*end != 0)
        return FALSE;
    *out = v;
    return TRUE;
}

static BOOL parse_double(const WCHAR *s, double *out)
{
    WCHAR *end;
    double v;

    if (s == NULL || *s == 0)
        return FALSE;
    v = wcstod(s, &end);
    if (end == s)
        return FALSE;
    while (*end && is_space(*end))
        end++;
    if (*end != 0)
        return FALSE;
    *out = v;
    return TRUE;
}

static BOOL parse_bool(const WCHAR *s, int *out)
{
    if (_wcsicmp(s, L"1") == 0 || _wcsicmp(s, L"true") == 0 ||
        _wcsicmp(s, L"yes") == 0 || _wcsicmp(s, L"on") == 0) {
        *out = 1;
        return TRUE;
    }
    if (_wcsicmp(s, L"0") == 0 || _wcsicmp(s, L"false") == 0 ||
        _wcsicmp(s, L"no") == 0 || _wcsicmp(s, L"off") == 0) {
        *out = 0;
        return TRUE;
    }
    return FALSE;
}

static int hex_value(WCHAR ch)
{
    if (ch >= L'0' && ch <= L'9')
        return ch - L'0';
    if (ch >= L'a' && ch <= L'f')
        return ch - L'a' + 10;
    if (ch >= L'A' && ch <= L'F')
        return ch - L'A' + 10;
    return -1;
}

static BOOL parse_colour(const WCHAR *s, COLORREF *out)
{
    unsigned v = 0;
    int i, d;

    if (s == NULL || s[0] != L'#' || wcslen(s) != 7)
        return FALSE;
    for (i = 1; i < 7; i++) {
        d = hex_value(s[i]);
        if (d < 0)
            return FALSE;
        v = (v << 4) | (unsigned)d;
    }
    *out = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    return TRUE;
}

static BOOL parse_alphabet(const WCHAR *s, WCHAR *out, size_t cch)
{
    size_t n = wcslen(s);
    size_t i, j;

    if (n < 2 || n > 60 || n + 1 > cch)
        return FALSE;
    for (i = 0; i < n; i++) {
        WCHAR ch = lower_ascii(s[i]);
        if (!((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9')))
            return FALSE;
        for (j = 0; j < i; j++) {
            if (lower_ascii(s[j]) == ch)
                return FALSE;
        }
        out[i] = ch;
    }
    out[n] = 0;
    return TRUE;
}

static BOOL parse_subgrid_keys(const WCHAR *s, WCHAR *out, size_t cch)
{
    size_t n = wcslen(s);
    size_t i, j;
    int rows = 0, row_len = -1, cur = 0;

    if (n == 0 || n + 1 > cch)
        return FALSE;
    for (i = 0; i <= n; i++) {
        WCHAR ch = s[i];
        if (i == n || ch == L'/') {
            if (cur < 1 || cur > 8)
                return FALSE;
            if (row_len < 0)
                row_len = cur;
            else if (cur != row_len)
                return FALSE;
            rows++;
            cur = 0;
        } else {
            if (ch <= 32)
                return FALSE;
            for (j = 0; j < i; j++) {
                if (s[j] != L'/' && lower_ascii(s[j]) == lower_ascii(ch))
                    return FALSE;
            }
            cur++;
        }
    }
    if (rows < 1 || rows > 8)
        return FALSE;
    wcscpy(out, s);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Key names

static const struct {
    const WCHAR *name;
    UINT vk;
} g_named_keys[] = {
    { L"CapsLock",   VK_CAPITAL },
    { L"Space",      VK_SPACE },
    { L"Enter",      VK_RETURN },
    { L"Tab",        VK_TAB },
    { L"Esc",        VK_ESCAPE },
    { L"Escape",     VK_ESCAPE },
    { L"Backspace",  VK_BACK },
    { L"Delete",     VK_DELETE },
    { L"Insert",     VK_INSERT },
    { L"Home",       VK_HOME },
    { L"End",        VK_END },
    { L"PgUp",       VK_PRIOR },
    { L"PgDn",       VK_NEXT },
    { L"Left",       VK_LEFT },
    { L"Right",      VK_RIGHT },
    { L"Up",         VK_UP },
    { L"Down",       VK_DOWN },
    { L"ScrollLock", VK_SCROLL },
    { L"Pause",      VK_PAUSE },
    { L"Apps",       VK_APPS }
};

static const struct {
    const WCHAR *name;
    WCHAR ch;
} g_char_keys[] = {
    { L"Comma",     L',' },
    { L"Period",    L'.' },
    { L"Slash",     L'/' },
    { L"Semicolon", L';' },
    { L"Quote",     L'\'' },
    { L"LBracket",  L'[' },
    { L"RBracket",  L']' },
    { L"Backslash", L'\\' },
    { L"Minus",     L'-' },
    { L"Equals",    L'=' },
    { L"Backtick",  L'`' },
    { L"Hash",      L'#' }
};

static UINT vk_for_char(WCHAR ch)
{
    SHORT v = VkKeyScanW(ch);

    if (v == (SHORT)-1)
        return 0;
    return (UINT)(v & 0xFF);
}

BOOL parse_key(const WCHAR *s, UINT *vk)
{
    WCHAR buf[64];
    WCHAR *t;
    size_t len, i;
    int n;

    if (s == NULL || vk == NULL)
        return FALSE;
    if (wcslen(s) >= sizeof(buf) / sizeof(buf[0]))
        return FALSE;
    wcscpy(buf, s);
    t = buf;
    trim(&t);
    if (*t == 0)
        return FALSE;
    if (_wcsicmp(t, L"none") == 0) {
        *vk = 0;
        return TRUE;
    }

    len = wcslen(t);
    if (len == 1) {
        WCHAR ch = t[0];
        if (ch >= L'a' && ch <= L'z') {
            *vk = (UINT)(ch - 32);
            return TRUE;
        }
        if (ch >= L'A' && ch <= L'Z') {
            *vk = (UINT)ch;
            return TRUE;
        }
        if (ch >= L'0' && ch <= L'9') {
            *vk = (UINT)ch;
            return TRUE;
        }
        *vk = vk_for_char(ch);
        return *vk != 0;
    }

    for (i = 0; i < sizeof(g_named_keys) / sizeof(g_named_keys[0]); i++) {
        if (_wcsicmp(t, g_named_keys[i].name) == 0) {
            *vk = g_named_keys[i].vk;
            return TRUE;
        }
    }

    for (i = 0; i < sizeof(g_char_keys) / sizeof(g_char_keys[0]); i++) {
        if (_wcsicmp(t, g_char_keys[i].name) == 0) {
            *vk = vk_for_char(g_char_keys[i].ch);
            return *vk != 0;
        }
    }

    if ((t[0] == L'f' || t[0] == L'F') && t[1] != 0) {
        int ok = 1;
        n = 0;
        for (i = 1; t[i]; i++) {
            if (t[i] < L'0' || t[i] > L'9') {
                ok = 0;
                break;
            }
            n = n * 10 + (int)(t[i] - L'0');
            if (n > 24) {
                ok = 0;
                break;
            }
        }
        if (ok && n >= 1) {
            *vk = (UINT)(VK_F1 + n - 1);
            return TRUE;
        }
    }

    return FALSE;
}

BOOL parse_hotkey(const WCHAR *s, Hotkey *out)
{
    WCHAR buf[128];
    WCHAR *parts[16];
    WCHAR *t;
    int n = 0, i;
    UINT mods = 0;

    if (s == NULL || out == NULL)
        return FALSE;
    if (wcslen(s) >= sizeof(buf) / sizeof(buf[0]))
        return FALSE;
    wcscpy(buf, s);
    t = buf;
    trim(&t);
    if (_wcsicmp(t, L"none") == 0) {
        out->vk = 0;
        out->mods = 0;
        return TRUE;
    }
    if (*t == 0)
        return FALSE;

    parts[n++] = t;
    for (i = 0; t[i]; i++) {
        if (t[i] == L'+') {
            t[i] = 0;
            if (n >= (int)(sizeof(parts) / sizeof(parts[0])))
                return FALSE;
            parts[n++] = t + i + 1;
        }
    }

    for (i = 0; i < n; i++) {
        WCHAR *part = parts[i];

        trim(&part);
        if (*part == 0)
            return FALSE;

        if (_wcsicmp(part, L"ctrl") == 0 || _wcsicmp(part, L"control") == 0) {
            if (i == n - 1)
                return FALSE;
            mods |= KMOD_CTRL;
        } else if (_wcsicmp(part, L"alt") == 0) {
            if (i == n - 1)
                return FALSE;
            mods |= KMOD_ALT;
        } else if (_wcsicmp(part, L"shift") == 0) {
            if (i == n - 1)
                return FALSE;
            mods |= KMOD_SHIFT;
        } else if (_wcsicmp(part, L"win") == 0) {
            if (i == n - 1)
                return FALSE;
            mods |= KMOD_WIN;
        } else {
            UINT vk;
            if (i != n - 1)
                return FALSE;
            if (!parse_key(part, &vk))
                return FALSE;
            out->vk = vk;
            out->mods = mods;
            return TRUE;
        }
    }
    return FALSE;
}

void format_key(UINT vk, WCHAR *buf, int cch)
{
    size_t i;
    WCHAR ch;

    if (buf == NULL || cch <= 0)
        return;
    if (vk == 0) {
        _snwprintf(buf, (size_t)cch, L"none");
        buf[cch - 1] = 0;
        return;
    }
    if (vk >= (UINT)L'A' && vk <= (UINT)L'Z') {
        buf[0] = (WCHAR)(vk + 32);
        buf[1] = 0;
        return;
    }
    if (vk >= (UINT)L'0' && vk <= (UINT)L'9') {
        buf[0] = (WCHAR)vk;
        buf[1] = 0;
        return;
    }
    for (i = 0; i < sizeof(g_named_keys) / sizeof(g_named_keys[0]); i++) {
        if (vk == g_named_keys[i].vk) {
            _snwprintf(buf, (size_t)cch, L"%ls", g_named_keys[i].name);
            buf[cch - 1] = 0;
            return;
        }
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        _snwprintf(buf, (size_t)cch, L"F%u", (unsigned)(vk - VK_F1 + 1));
        buf[cch - 1] = 0;
        return;
    }
    ch = (WCHAR)(MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0xFFFF);
    if (ch >= 32 && ch != 127) {
        buf[0] = ch;
        buf[1] = 0;
        return;
    }
    _snwprintf(buf, (size_t)cch, L"VK%02X", (unsigned)vk);
    buf[cch - 1] = 0;
}

void format_hotkey(const Hotkey *hk, WCHAR *buf, int cch)
{
    WCHAR key[64];
    WCHAR out[128];

    if (buf == NULL || cch <= 0)
        return;
    if (hk == NULL || hk->vk == 0) {
        _snwprintf(buf, (size_t)cch, L"none");
        buf[cch - 1] = 0;
        return;
    }

    format_key(hk->vk, key, (int)(sizeof(key) / sizeof(key[0])));
    if (key[0] >= L'a' && key[0] <= L'z' && key[1] == 0)
        key[0] = (WCHAR)(key[0] - 32);

    out[0] = 0;
    if (hk->mods & KMOD_CTRL)
        wcscat(out, L"Ctrl+");
    if (hk->mods & KMOD_ALT)
        wcscat(out, L"Alt+");
    if (hk->mods & KMOD_SHIFT)
        wcscat(out, L"Shift+");
    if (hk->mods & KMOD_WIN)
        wcscat(out, L"Win+");
    wcsncat(out, key, 127 - wcslen(out));
    out[127] = 0;

    _snwprintf(buf, (size_t)cch, L"%ls", out);
    buf[cch - 1] = 0;
}

// ---------------------------------------------------------------------------
// Defaults

void config_set_defaults(Config *c)
{
    if (c == NULL)
        return;
    memset(c, 0, sizeof(*c));

    parse_hotkey(L"Alt+F", &c->hint_hotkey);
    parse_hotkey(L"Ctrl+Alt+Q", &c->quit_hotkey);
    c->pause_hotkey.vk = 0;
    c->pause_hotkey.mods = 0;
    parse_hotkey(L"Ctrl+Alt+L", &c->lock_hotkey);
    c->mouse_mode_key = VK_CAPITAL;
    c->indicator = INDICATOR_CURSOR;
    c->log = 0;

    c->speed = 600.0;
    c->max_speed = 2400.0;
    c->accel_time = 0.45;
    c->accel_curve = 1.8;
    c->slow_speed = 120.0;
    c->scroll_speed = 1200.0;
    c->scroll_max_speed = 4800.0;
    c->smooth_scroll = 1;
    c->alt_scroll = 1;
    c->alt_scroll_auto = 1;
    c->alt_nav = 1;
    c->bare_keys = 1;
    c->shortcut_paste = 1;
    c->fast_scroll_lines = 5;
    c->scroll_end_tap_ms = 300;
    c->fast_scroll_speed = 3600.0;
    c->fast_scroll_max_speed = 14400.0;

    parse_key(L"h", &c->key_left);
    parse_key(L"j", &c->key_down);
    parse_key(L"k", &c->key_up);
    parse_key(L"l", &c->key_right);
    parse_key(L"a", &c->key_left_button);
    parse_key(L"s", &c->key_right_click);
    parse_key(L"m", &c->key_middle_click);
    parse_key(L"v", &c->key_drag_toggle);
    parse_key(L"u", &c->key_scroll_up);
    parse_key(L"d", &c->key_scroll_down);
    parse_key(L",", &c->key_scroll_left);
    parse_key(L".", &c->key_scroll_right);
    parse_key(L"f", &c->key_hints);
    parse_key(L"i", &c->key_exit);
    parse_key(L"/", &c->key_help);

    wcscpy(c->alphabet, L"abcdefhjklmnopqrstuvwxz");
    c->cell_size = 64;
    c->subgrid = 1;
    wcscpy(c->subgrid_keys, L"qwer/asdf/zxcv");
    c->font_size = 0;
    c->opacity = 235;
    c->label_bg = RGB(0xFF, 0xD6, 0x33);
    c->label_fg = RGB(0x14, 0x14, 0x14);
    c->label_border = RGB(0x7A, 0x5C, 0x00);
    c->match_fg = RGB(0x9C, 0x7A, 0x10);
    c->hint_mode = HINT_MODE_ELEMENTS;
    c->scan_scope = SCAN_MONITOR;
    c->scan_timeout = 400;
    c->scan_refresh = 500;
    c->prescan = 1;
}

// ---------------------------------------------------------------------------
// Option application

static void apply_double_positive(double *dst, int line, const WCHAR *key,
                                  const WCHAR *val)
{
    double v;

    if (!parse_double(val, &v)) {
        warn_add(line, L"bad number \"%ls\" for \"%ls\"", val, key);
        return;
    }
    if (!(v > 0.0)) {
        warn_add(line, L"\"%ls\" must be greater than 0", key);
        return;
    }
    *dst = v;
}

static void apply_int_range(int *dst, int line, const WCHAR *key,
                            const WCHAR *val, int lo, int hi)
{
    long v;

    if (!parse_long(val, &v)) {
        warn_add(line, L"bad number \"%ls\" for \"%ls\"", val, key);
        return;
    }
    if (v < lo || v > hi) {
        long clamped = v < lo ? lo : hi;
        warn_add(line, L"\"%ls\" = %ld is out of range, clamped to %ld",
                 key, v, clamped);
        *dst = (int)clamped;
    } else {
        *dst = (int)v;
    }
}

static void apply_font_size(int *dst, int line, const WCHAR *key,
                            const WCHAR *val)
{
    long v;

    if (!parse_long(val, &v)) {
        warn_add(line, L"bad number \"%ls\" for \"%ls\"", val, key);
        return;
    }
    if (v == 0) {
        *dst = 0;
        return;
    }
    if (v < 6 || v > 72) {
        long clamped = v < 6 ? 6 : 72;
        warn_add(line, L"\"%ls\" = %ld is out of range, clamped to %ld",
                 key, v, clamped);
        *dst = (int)clamped;
    } else {
        *dst = (int)v;
    }
}

static void apply_opacity(BYTE *dst, int line, const WCHAR *key,
                          const WCHAR *val)
{
    long v;

    if (!parse_long(val, &v)) {
        warn_add(line, L"bad number \"%ls\" for \"%ls\"", val, key);
        return;
    }
    if (v < 30 || v > 255) {
        long clamped = v < 30 ? 30 : 255;
        warn_add(line, L"\"%ls\" = %ld is out of range, clamped to %ld",
                 key, v, clamped);
        *dst = (BYTE)clamped;
    } else {
        *dst = (BYTE)v;
    }
}

static void apply_key(UINT *dst, int line, const WCHAR *key, const WCHAR *val)
{
    UINT vk;

    if (!parse_key(val, &vk)) {
        warn_add(line, L"bad key \"%ls\" for \"%ls\"", val, key);
        return;
    }
    *dst = vk;
}

static void apply_hotkey(Hotkey *dst, int line, const WCHAR *key,
                         const WCHAR *val)
{
    Hotkey hk;

    if (!parse_hotkey(val, &hk)) {
        warn_add(line, L"bad hotkey \"%ls\" for \"%ls\"", val, key);
        return;
    }
    *dst = hk;
}

static void apply_bool(int *dst, int line, const WCHAR *key, const WCHAR *val)
{
    int v;

    if (!parse_bool(val, &v)) {
        warn_add(line, L"bad boolean \"%ls\" for \"%ls\" (use 0/1)", val, key);
        return;
    }
    *dst = v;
}

static void apply_colour(COLORREF *dst, int line, const WCHAR *key,
                         const WCHAR *val)
{
    COLORREF colour;

    if (!parse_colour(val, &colour)) {
        warn_add(line, L"bad colour \"%ls\" for \"%ls\" (use #RRGGBB)",
                 val, key);
        return;
    }
    *dst = colour;
}

static void apply_alphabet(WCHAR *dst, int line, const WCHAR *val)
{
    WCHAR tmp[64];

    if (!parse_alphabet(val, tmp, sizeof(tmp) / sizeof(tmp[0]))) {
        warn_add(line, L"bad alphabet \"%ls\" (2-60 unique letters/digits)",
                 val);
        return;
    }
    wcscpy(dst, tmp);
}

static void apply_subgrid_keys(WCHAR *dst, int line, const WCHAR *val)
{
    WCHAR tmp[64];

    if (!parse_subgrid_keys(val, tmp, sizeof(tmp) / sizeof(tmp[0]))) {
        warn_add(line, L"bad subgrid_keys \"%ls\" (rows of equal length, "
                 L"unique chars, 1x1 to 8x8)", val);
        return;
    }
    wcscpy(dst, tmp);
}

// ---------------------------------------------------------------------------
// INI parser

enum { SEC_NONE = 0, SEC_GENERAL, SEC_MOUSE, SEC_HINTS, SEC_SHORTCUTS,
       SEC_UNKNOWN };

static const WCHAR *section_name(int sec)
{
    switch (sec) {
    case SEC_GENERAL: return L"general";
    case SEC_MOUSE:   return L"mouse";
    case SEC_HINTS:   return L"hints";
    case SEC_SHORTCUTS: return L"shortcuts";
    case SEC_UNKNOWN: return L"unknown";
    default:          return L"";
    }
}

static void process_general(Config *c, int line, const WCHAR *key,
                            const WCHAR *val)
{
    if (_wcsicmp(key, L"hint_hotkey") == 0)
        apply_hotkey(&c->hint_hotkey, line, key, val);
    else if (_wcsicmp(key, L"quit_hotkey") == 0)
        apply_hotkey(&c->quit_hotkey, line, key, val);
    else if (_wcsicmp(key, L"pause_hotkey") == 0)
        apply_hotkey(&c->pause_hotkey, line, key, val);
    else if (_wcsicmp(key, L"lock_hotkey") == 0)
        apply_hotkey(&c->lock_hotkey, line, key, val);
    else if (_wcsicmp(key, L"mouse_mode_key") == 0)
        apply_key(&c->mouse_mode_key, line, key, val);
    else if (_wcsicmp(key, L"indicator") == 0) {
        if (_wcsicmp(val, L"cursor") == 0)
            c->indicator = INDICATOR_CURSOR;
        else if (_wcsicmp(val, L"corner") == 0)
            c->indicator = INDICATOR_CORNER;
        else if (_wcsicmp(val, L"off") == 0)
            c->indicator = INDICATOR_OFF;
        else
            warn_add(line, L"bad value \"%ls\" for \"indicator\" "
                     L"(use cursor, corner or off)", val);
    } else if (_wcsicmp(key, L"log") == 0)
        apply_bool(&c->log, line, key, val);
    else if (_wcsicmp(key, L"shortcut_method") == 0) {
        if (_wcsicmp(val, L"paste") == 0)
            c->shortcut_paste = 1;
        else if (_wcsicmp(val, L"type") == 0)
            c->shortcut_paste = 0;
        else
            warn_add(line, L"bad value \"%ls\" for \"shortcut_method\" "
                     L"(use paste or type)", val);
    }
    else
        warn_add(line, L"unknown key \"%ls\" in [general]", key);
}

static void process_mouse(Config *c, int line, const WCHAR *key,
                          const WCHAR *val)
{
    if (_wcsicmp(key, L"speed") == 0)
        apply_double_positive(&c->speed, line, key, val);
    else if (_wcsicmp(key, L"max_speed") == 0)
        apply_double_positive(&c->max_speed, line, key, val);
    else if (_wcsicmp(key, L"accel_time") == 0)
        apply_double_positive(&c->accel_time, line, key, val);
    else if (_wcsicmp(key, L"accel_curve") == 0)
        apply_double_positive(&c->accel_curve, line, key, val);
    else if (_wcsicmp(key, L"slow_speed") == 0)
        apply_double_positive(&c->slow_speed, line, key, val);
    else if (_wcsicmp(key, L"scroll_speed") == 0)
        apply_double_positive(&c->scroll_speed, line, key, val);
    else if (_wcsicmp(key, L"scroll_max_speed") == 0)
        apply_double_positive(&c->scroll_max_speed, line, key, val);
    else if (_wcsicmp(key, L"smooth_scroll") == 0)
        apply_bool(&c->smooth_scroll, line, key, val);
    else if (_wcsicmp(key, L"alt_scroll") == 0)
        apply_bool(&c->alt_scroll, line, key, val);
    else if (_wcsicmp(key, L"alt_scroll_auto") == 0)
        apply_bool(&c->alt_scroll_auto, line, key, val);
    else if (_wcsicmp(key, L"alt_nav") == 0)
        apply_bool(&c->alt_nav, line, key, val);
    else if (_wcsicmp(key, L"bare_keys") == 0)
        apply_bool(&c->bare_keys, line, key, val);
    else if (_wcsicmp(key, L"fast_scroll_lines") == 0)
        apply_int_range(&c->fast_scroll_lines, line, key, val, 1, 50);
    else if (_wcsicmp(key, L"scroll_end_tap_ms") == 0)
        apply_int_range(&c->scroll_end_tap_ms, line, key, val, 0, 1000);
    else if (_wcsicmp(key, L"fast_scroll_speed") == 0)
        apply_double_positive(&c->fast_scroll_speed, line, key, val);
    else if (_wcsicmp(key, L"fast_scroll_max_speed") == 0)
        apply_double_positive(&c->fast_scroll_max_speed, line, key, val);
    else if (_wcsicmp(key, L"key_left") == 0)
        apply_key(&c->key_left, line, key, val);
    else if (_wcsicmp(key, L"key_down") == 0)
        apply_key(&c->key_down, line, key, val);
    else if (_wcsicmp(key, L"key_up") == 0)
        apply_key(&c->key_up, line, key, val);
    else if (_wcsicmp(key, L"key_right") == 0)
        apply_key(&c->key_right, line, key, val);
    else if (_wcsicmp(key, L"key_left_button") == 0)
        apply_key(&c->key_left_button, line, key, val);
    else if (_wcsicmp(key, L"key_right_click") == 0)
        apply_key(&c->key_right_click, line, key, val);
    else if (_wcsicmp(key, L"key_middle_click") == 0)
        apply_key(&c->key_middle_click, line, key, val);
    else if (_wcsicmp(key, L"key_drag_toggle") == 0)
        apply_key(&c->key_drag_toggle, line, key, val);
    else if (_wcsicmp(key, L"key_scroll_up") == 0)
        apply_key(&c->key_scroll_up, line, key, val);
    else if (_wcsicmp(key, L"key_scroll_down") == 0)
        apply_key(&c->key_scroll_down, line, key, val);
    else if (_wcsicmp(key, L"key_scroll_left") == 0)
        apply_key(&c->key_scroll_left, line, key, val);
    else if (_wcsicmp(key, L"key_scroll_right") == 0)
        apply_key(&c->key_scroll_right, line, key, val);
    else if (_wcsicmp(key, L"key_hints") == 0)
        apply_key(&c->key_hints, line, key, val);
    else if (_wcsicmp(key, L"key_exit") == 0)
        apply_key(&c->key_exit, line, key, val);
    else if (_wcsicmp(key, L"key_help") == 0)
        apply_key(&c->key_help, line, key, val);
    else
        warn_add(line, L"unknown key \"%ls\" in [mouse]", key);
}

static void process_hints(Config *c, int line, const WCHAR *key,
                          const WCHAR *val)
{
    if (_wcsicmp(key, L"alphabet") == 0)
        apply_alphabet(c->alphabet, line, val);
    else if (_wcsicmp(key, L"cell_size") == 0)
        apply_int_range(&c->cell_size, line, key, val, 24, 400);
    else if (_wcsicmp(key, L"subgrid") == 0)
        apply_bool(&c->subgrid, line, key, val);
    else if (_wcsicmp(key, L"subgrid_keys") == 0)
        apply_subgrid_keys(c->subgrid_keys, line, val);
    else if (_wcsicmp(key, L"font_size") == 0)
        apply_font_size(&c->font_size, line, key, val);
    else if (_wcsicmp(key, L"opacity") == 0)
        apply_opacity(&c->opacity, line, key, val);
    else if (_wcsicmp(key, L"label_bg") == 0)
        apply_colour(&c->label_bg, line, key, val);
    else if (_wcsicmp(key, L"label_fg") == 0)
        apply_colour(&c->label_fg, line, key, val);
    else if (_wcsicmp(key, L"label_border") == 0)
        apply_colour(&c->label_border, line, key, val);
    else if (_wcsicmp(key, L"match_fg") == 0)
        apply_colour(&c->match_fg, line, key, val);
    else if (_wcsicmp(key, L"hint_mode") == 0) {
        if (_wcsicmp(val, L"elements") == 0)
            c->hint_mode = HINT_MODE_ELEMENTS;
        else if (_wcsicmp(val, L"grid") == 0)
            c->hint_mode = HINT_MODE_GRID;
        else
            warn_add(line, L"bad value \"%ls\" for \"hint_mode\" "
                     L"(use elements or grid)", val);
    } else if (_wcsicmp(key, L"scan_scope") == 0) {
        if (_wcsicmp(val, L"monitor") == 0)
            c->scan_scope = SCAN_MONITOR;
        else if (_wcsicmp(val, L"window") == 0)
            c->scan_scope = SCAN_WINDOW;
        else
            warn_add(line, L"bad value \"%ls\" for \"scan_scope\" "
                     L"(use monitor or window)", val);
    } else if (_wcsicmp(key, L"scan_timeout") == 0)
        apply_int_range(&c->scan_timeout, line, key, val, 100, 5000);
    else if (_wcsicmp(key, L"scan_refresh") == 0)
        apply_int_range(&c->scan_refresh, line, key, val, 0, 5000);
    else if (_wcsicmp(key, L"prescan") == 0)
        apply_bool(&c->prescan, line, key, val);
    else
        warn_add(line, L"unknown key \"%ls\" in [hints]", key);
}

// ---------------------------------------------------------------------------
// Text shortcuts

static void sc_lock(void)
{
    if (InterlockedCompareExchange(&g_sc_lock_state, 1, 0) == 0) {
        InitializeCriticalSection(&g_sc_lock);
        InterlockedExchange(&g_sc_lock_state, 2);
    }
    while (InterlockedCompareExchange(&g_sc_lock_state, 2, 2) != 2)
        Sleep(0);
    EnterCriticalSection(&g_sc_lock);
}

static void sc_unlock(void)
{
    LeaveCriticalSection(&g_sc_lock);
}

static void sc_free(Shortcut *list, int *count)
{
    int i;

    for (i = 0; i < *count; i++) {
        free(list[i].text);
        list[i].text = NULL;
    }
    *count = 0;
}

// Swaps the parsed table in. Runs only on the thread that loads the config.
static void sc_commit(void)
{
    Shortcut old[SHORTCUT_MAX];
    int old_count;

    sc_lock();
    old_count = g_sc_count;
    memcpy(old, g_sc, sizeof(Shortcut) * (size_t)old_count);
    memcpy(g_sc, g_sc_new, sizeof(Shortcut) * (size_t)g_sc_new_count);
    g_sc_count = g_sc_new_count;
    sc_unlock();
    g_sc_new_count = 0;
    sc_free(old, &old_count);
}

BOOL shortcut_abbr_valid(const WCHAR *s)
{
    int n = 0;

    if (s == NULL)
        return FALSE;
    for (; *s; s++, n++) {
        WCHAR ch = lower_ascii(*s);
        if (!((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9')))
            return FALSE;
    }
    return n >= 1 && n <= SHORTCUT_ABBR_MAX;
}

// "\n" -> newline, "\t" -> tab, "\s" -> space, "\\" -> backslash.
static void sc_unescape(const WCHAR *src, WCHAR *dst, size_t cch)
{
    size_t n = 0;

    for (; *src && n + 1 < cch; src++) {
        WCHAR ch = *src;
        if (ch == L'\\' && src[1]) {
            src++;
            switch (*src) {
            case L'n': ch = L'\n'; break;
            case L't': ch = L'\t'; break;
            case L's': ch = L' ';  break;
            default:   ch = *src;  break;
            }
        }
        dst[n++] = ch;
    }
    dst[n] = 0;
}

static void process_shortcut(int line, const WCHAR *key, const WCHAR *val)
{
    static WCHAR text[SHORTCUT_TEXT_MAX + 1];
    Shortcut *sc;
    int i;

    if (!shortcut_abbr_valid(key)) {
        warn_add(line, L"bad shortcut \"%ls\" (use 1-%d letters or digits)",
                 key, SHORTCUT_ABBR_MAX);
        return;
    }
    for (i = 0; i < g_sc_new_count; i++) {
        if (_wcsicmp(g_sc_new[i].abbr, key) == 0) {
            warn_add(line, L"shortcut \"%ls\" is defined twice", key);
            return;
        }
    }
    if (g_sc_new_count >= SHORTCUT_MAX) {
        warn_add(line, L"more than %d shortcuts", SHORTCUT_MAX);
        return;
    }
    if (wcslen(val) > SHORTCUT_TEXT_MAX)
        warn_add(line, L"shortcut \"%ls\" is longer than %d characters",
                 key, SHORTCUT_TEXT_MAX);
    sc_unescape(val, text, sizeof(text) / sizeof(text[0]));
    sc = &g_sc_new[g_sc_new_count];
    sc->text = _wcsdup(text);
    if (sc->text == NULL)
        return;
    for (i = 0; key[i]; i++)
        sc->abbr[i] = lower_ascii(key[i]);
    sc->abbr[i] = 0;
    g_sc_new_count++;
}

UINT shortcut_lookup(const WCHAR *abbr, int len)
{
    UINT result = 0;
    int i;

    if (len <= 0)
        return 0;
    sc_lock();
    for (i = 0; i < g_sc_count; i++) {
        if (_wcsnicmp(g_sc[i].abbr, abbr, (size_t)len) != 0)
            continue;
        if (g_sc[i].abbr[len] == 0)
            result |= SC_EXACT;
        else
            result |= SC_PREFIX;
    }
    sc_unlock();
    return result;
}

WCHAR *shortcut_text_dup(const WCHAR *abbr, int len)
{
    WCHAR *text = NULL;
    int i;

    if (len <= 0)
        return NULL;
    sc_lock();
    for (i = 0; i < g_sc_count; i++) {
        if (_wcsnicmp(g_sc[i].abbr, abbr, (size_t)len) == 0 &&
            g_sc[i].abbr[len] == 0) {
            text = _wcsdup(g_sc[i].text);
            break;
        }
    }
    sc_unlock();
    return text;
}

int shortcut_count(void)
{
    int n;

    sc_lock();
    n = g_sc_count;
    sc_unlock();
    return n;
}

BOOL shortcut_get(int i, WCHAR *abbr, int abbr_cch, WCHAR **text)
{
    BOOL ok = FALSE;

    if (text != NULL)
        *text = NULL;
    sc_lock();
    if (i >= 0 && i < g_sc_count) {
        if (abbr != NULL && abbr_cch > 0)
            lstrcpynW(abbr, g_sc[i].abbr, abbr_cch);
        if (text != NULL)
            *text = _wcsdup(g_sc[i].text);
        ok = text == NULL || *text != NULL;
    }
    sc_unlock();
    return ok;
}

static void process_line(Config *c, int line, WCHAR *text, int *section)
{
    WCHAR *s = text;
    WCHAR *eq, *key, *val;

    trim(&s);
    if (*s == 0)
        return;
    if (*s == L';' || *s == L'#')
        return;

    if (*s == L'[') {
        WCHAR *close = wcschr(s, L']');
        WCHAR *name;
        WCHAR *rest;

        if (close == NULL) {
            warn_add(line, L"missing ']' in section header");
            return;
        }
        *close = 0;
        name = s + 1;
        trim(&name);
        rest = close + 1;
        trim(&rest);
        if (*rest != 0)
            warn_add(line, L"unexpected text after section header");

        if (_wcsicmp(name, L"general") == 0)
            *section = SEC_GENERAL;
        else if (_wcsicmp(name, L"mouse") == 0)
            *section = SEC_MOUSE;
        else if (_wcsicmp(name, L"hints") == 0)
            *section = SEC_HINTS;
        else if (_wcsicmp(name, L"shortcuts") == 0)
            *section = SEC_SHORTCUTS;
        else {
            warn_add(line, L"unknown section \"[%ls]\"", name);
            *section = SEC_UNKNOWN;
        }
        return;
    }

    eq = wcschr(s, L'=');
    if (eq == NULL) {
        warn_add(line, L"expected key = value");
        return;
    }
    *eq = 0;
    key = s;
    val = eq + 1;
    trim(&key);
    trim(&val);
    if (*key == 0) {
        warn_add(line, L"missing key before '='");
        return;
    }
    if (*section == SEC_NONE) {
        warn_add(line, L"key \"%ls\" outside a section", key);
        return;
    }
    if (*section == SEC_UNKNOWN) {
        warn_add(line, L"unknown key \"%ls\" in [%ls]", key,
                 section_name(*section));
        return;
    }

    switch (*section) {
    case SEC_GENERAL: process_general(c, line, key, val); break;
    case SEC_MOUSE:   process_mouse(c, line, key, val);   break;
    case SEC_HINTS:   process_hints(c, line, key, val);   break;
    case SEC_SHORTCUTS: process_shortcut(line, key, val);  break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Path

static BOOL path_exists(const WCHAR *path)
{
    HANDLE h;

    h = CreateFileW(path, FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    CloseHandle(h);
    return TRUE;
}

// Probes the folder by creating the file, then removes the empty test file.
// TRUE if it exists already or the folder accepts a new file.
static BOOL file_can_be_created(const WCHAR *path)
{
    HANDLE h;
    DWORD  err;

    h = CreateFileW(path, GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        return err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS;
    }
    CloseHandle(h);
    DeleteFileW(path);
    return TRUE;
}

// %APPDATA%\vim mouse\<exe stem>.ini; FALSE if %APPDATA% is unknown.
static BOOL appdata_ini_path(WCHAR *out, DWORD cch, const WCHAR *exe_stem)
{
    WCHAR        appdata[MAX_PATH];
    const WCHAR *base;
    int          n;

    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL,
                                SHGFP_TYPE_CURRENT, appdata)))
        return FALSE;
    base = wcsrchr(exe_stem, L'\\');
    base = base ? base + 1 : exe_stem;
    n = _snwprintf(out, cch, L"%s\\" APP_NAME L"\\%s.ini", appdata, base);
    out[cch - 1] = 0;
    return n >= 0 && (DWORD)n < cch;
}

static void create_parent_dir(const WCHAR *path)
{
    static WCHAR dir[APP_PATH_MAX];
    WCHAR       *slash;

    lstrcpynW(dir, path, APP_PATH_MAX);
    slash = wcsrchr(dir, L'\\');
    if (slash) {
        *slash = 0;
        CreateDirectoryW(dir, NULL);
    }
}

const WCHAR *config_path(void)
{
    DWORD n, cap;
    WCHAR *dot, *slash;
    BOOL  have_appdata;

    if (g_exe_ini_ready)
        return g_exe_ini;

    cap = (DWORD)(sizeof(g_exe_ini) / sizeof(g_exe_ini[0]));
    n = GetModuleFileNameW(NULL, g_exe_ini, cap);
    if (n == 0 || n >= cap)
        wcscpy(g_exe_ini, L"vim.exe");
    else
        g_exe_ini[n] = 0;

    slash = wcsrchr(g_exe_ini, L'\\');
    dot = wcsrchr(g_exe_ini, L'.');
    if (dot != NULL && (slash == NULL || dot > slash))
        *dot = 0;

    have_appdata = appdata_ini_path(g_appdata_ini,
                                    sizeof(g_appdata_ini) /
                                        sizeof(g_appdata_ini[0]),
                                    g_exe_ini);

    n = (DWORD)wcslen(g_exe_ini);
    if (n + 5 < cap)
        wcscpy(g_exe_ini + n, L".ini");
    else
        lstrcpynW(g_exe_ini, L"vim.ini", (int)cap);

    // An existing file next to the exe wins. Otherwise keep using %APPDATA%
    // if settings were already saved there (the install used to be read-only).
    // Only when neither exists probe whether the exe folder accepts a file; the
    // real file is written later, by config_load.
    if (path_exists(g_exe_ini)) {
        // use it
    } else if (have_appdata && path_exists(g_appdata_ini)) {
        lstrcpynW(g_exe_ini, g_appdata_ini, (int)cap);
    } else if (file_can_be_created(g_exe_ini)) {
        // use it
    } else if (have_appdata) {
        create_parent_dir(g_appdata_ini);
        lstrcpynW(g_exe_ini, g_appdata_ini, (int)cap);
    }
    g_exe_ini[cap - 1] = 0;
    g_exe_ini_ready = TRUE;
    return g_exe_ini;
}

const WCHAR *config_warnings(void)
{
    return g_warnings;
}

// ---------------------------------------------------------------------------
// Default file (the documentation)

// Written at the top of [shortcuts], by the default file and by shortcuts_save.
static const WCHAR SHORTCUTS_COMMENT[] =
    L"; Text shortcuts: hold Alt, type the abbreviation, release Alt to\r\n"
    L"; type the text. abbreviation = text; abbreviations are 1-31 letters\r\n"
    L"; or digits. In the text \\n is a new line, \\t a tab, \\s a space (for\r\n"
    L"; leading/trailing spaces) and \\\\ is a backslash. The tray menu's\r\n"
    L"; \"Text shortcuts...\" rewrites this section.\r\n";

static void appendf(WCHAR *buf, size_t cch, const WCHAR *fmt, ...)
{
    size_t len = wcslen(buf);
    va_list ap;

    if (len + 1 >= cch)
        return;
    va_start(ap, fmt);
    _vsnwprintf(buf + len, cch - len, fmt, ap);
    va_end(ap);
    buf[cch - 1] = 0;
}

static void fmt_double(double v, WCHAR *buf, size_t cch)
{
    int prec;

    if (v == (double)(long long)v && v > -1.0e9 && v < 1.0e9) {
        _snwprintf(buf, cch, L"%.0f", v);
        buf[cch - 1] = 0;
        return;
    }
    for (prec = 1; prec <= 17; prec++) {
        WCHAR tmp[64];
        WCHAR *end;
        double back;

        _snwprintf(tmp, 64, L"%.*g", prec, v);
        tmp[63] = 0;
        back = wcstod(tmp, &end);
        if (end != NULL && *end == 0 && back == v) {
            wcscpy(buf, tmp);
            return;
        }
    }
    _snwprintf(buf, cch, L"%.17g", v);
    buf[cch - 1] = 0;
}

static void add_key_line(WCHAR *buf, size_t cch, const WCHAR *comment,
                         const WCHAR *name, UINT vk)
{
    WCHAR key[64];

    appendf(buf, cch, L"; %ls\r\n", comment);
    format_key(vk, key, (int)(sizeof(key) / sizeof(key[0])));
    appendf(buf, cch, L"%ls = %ls\r\n", name, key);
}

static void build_default_ini(WCHAR *buf, size_t cch)
{
    Config d;
    WCHAR key[64];
    WCHAR num[64];

    config_set_defaults(&d);
    buf[0] = 0;

    appendf(buf, cch, L"; vim mouse settings\r\n");
    appendf(buf, cch, L";\r\n");
    appendf(buf, cch, L"; The program reads this file at startup and reloads it automatically\r\n");
    appendf(buf, cch, L"; when you save it. Lines starting with ; or # are comments; there are\r\n");
    appendf(buf, cch, L"; no inline comments. Delete a line to use its default value.\r\n");
    appendf(buf, cch, L";\r\n");
    appendf(buf, cch, L"; Hotkeys look like Alt+F, Ctrl+Alt+Q or Ctrl+Shift+F12; write none for\r\n");
    appendf(buf, cch, L"; no hotkey. Keys are a single character (h, comma, period, slash) or a\r\n");
    appendf(buf, cch, L"; name such as CapsLock, Space, Enter, F13, Comma. Ranges are clamped.\r\n");

    appendf(buf, cch, L"\r\n[general]\r\n");
    appendf(buf, cch, L"; Hotkey that opens or closes the click hints. Modifiers: Ctrl\r\n");
    appendf(buf, cch, L"; (or Control), Alt, Shift, Win. Use none for no hotkey.\r\n");
    format_hotkey(&d.hint_hotkey, key, (int)(sizeof(key) / sizeof(key[0])));
    appendf(buf, cch, L"hint_hotkey = %ls\r\n", key);
    appendf(buf, cch, L"; Hotkey that quits the program.\r\n");
    format_hotkey(&d.quit_hotkey, key, (int)(sizeof(key) / sizeof(key[0])));
    appendf(buf, cch, L"quit_hotkey = %ls\r\n", key);
    appendf(buf, cch, L"; Hotkey that pauses or resumes everything. none = disabled.\r\n");
    format_hotkey(&d.pause_hotkey, key, (int)(sizeof(key) / sizeof(key[0])));
    appendf(buf, cch, L"pause_hotkey = %ls\r\n", key);
    appendf(buf, cch, L"; Hotkey that locks the keyboard (every key is ignored) and, pressed\r\n");
    appendf(buf, cch, L"; again, unlocks it. The tray menu can unlock it too. none = disabled.\r\n");
    format_hotkey(&d.lock_hotkey, key, (int)(sizeof(key) / sizeof(key[0])));
    appendf(buf, cch, L"lock_hotkey = %ls\r\n", key);
    appendf(buf, cch, L"; The key that toggles mouse mode: tap toggles, hold for momentary.\r\n");
    format_key(d.mouse_mode_key, key, (int)(sizeof(key) / sizeof(key[0])));
    appendf(buf, cch, L"mouse_mode_key = %ls\r\n", key);
    appendf(buf, cch, L"; Where the MOUSE/DRAG indicator is shown: cursor, corner or off.\r\n");
    switch (d.indicator) {
    case INDICATOR_CORNER:
        appendf(buf, cch, L"indicator = corner\r\n");
        break;
    case INDICATOR_OFF:
        appendf(buf, cch, L"indicator = off\r\n");
        break;
    default:
        appendf(buf, cch, L"indicator = cursor\r\n");
        break;
    }
    appendf(buf, cch, L"; 1 = also write debug log lines to vim.log next to this file.\r\n");
    appendf(buf, cch, L"log = %d\r\n", d.log ? 1 : 0);
    appendf(buf, cch, L"; How text shortcuts insert their text: paste (through the clipboard,\r\n");
    appendf(buf, cch, L"; which is restored afterwards; works best in browsers and with an IME)\r\n");
    appendf(buf, cch, L"; or type (key by key, for apps that block pasting).\r\n");
    appendf(buf, cch, L"shortcut_method = %ls\r\n", d.shortcut_paste ? L"paste" : L"type");

    appendf(buf, cch, L"\r\n[mouse]\r\n");
    appendf(buf, cch, L"; Movement speeds in pixels per second. speed is where movement starts\r\n");
    appendf(buf, cch, L"; and max_speed is reached after accel_time seconds; accel_curve shapes it.\r\n");
    fmt_double(d.speed, num, sizeof(num) / sizeof(num[0]));
    appendf(buf, cch, L"speed = %ls\r\n", num);
    fmt_double(d.max_speed, num, sizeof(num) / sizeof(num[0]));
    appendf(buf, cch, L"max_speed = %ls\r\n", num);
    fmt_double(d.accel_time, num, sizeof(num) / sizeof(num[0]));
    appendf(buf, cch, L"accel_time = %ls\r\n", num);
    fmt_double(d.accel_curve, num, sizeof(num) / sizeof(num[0]));
    appendf(buf, cch, L"accel_curve = %ls\r\n", num);
    appendf(buf, cch, L"; Constant movement speed while Shift is held (precise), px/second.\r\n");
    fmt_double(d.slow_speed, num, sizeof(num) / sizeof(num[0]));
    appendf(buf, cch, L"slow_speed = %ls\r\n", num);
    appendf(buf, cch, L"; Scrolling speeds in wheel units per second (one notch = 120, so 600 = 5 notches per second).\r\n");
    fmt_double(d.scroll_speed, num, sizeof(num) / sizeof(num[0]));
    appendf(buf, cch, L"scroll_speed = %ls\r\n", num);
    fmt_double(d.scroll_max_speed, num, sizeof(num) / sizeof(num[0]));
    appendf(buf, cch, L"scroll_max_speed = %ls\r\n", num);
    appendf(buf, cch, L"; 1 = scroll in small steps once per display frame (smooth);\r\n");
    appendf(buf, cch, L"; 0 = only whole notches, for apps that treat each wheel message as one.\r\n");
    appendf(buf, cch, L"smooth_scroll = %d\r\n", d.smooth_scroll ? 1 : 0);
    appendf(buf, cch, L"; 1 = Alt + the move keys (h j k l) scroll anywhere, also outside mouse\r\n");
    appendf(buf, cch, L"; mode, and Alt + the scroll up/down keys (u d) scroll fast.\r\n");
    appendf(buf, cch, L"alt_scroll = %d\r\n", d.alt_scroll ? 1 : 0);
    appendf(buf, cch, L"; 1 = Alt scrolling finds the scrollable area nearest to the pointer\r\n");
    appendf(buf, cch, L"; (when the pointer is not over one) and scrolls that. Within the window\r\n");
    appendf(buf, cch, L"; only an area in line with the pointer or right beside it counts.\r\n");
    appendf(buf, cch, L"alt_scroll_auto = %d\r\n", d.alt_scroll_auto ? 1 : 0);
    appendf(buf, cch, L"; 1 = Alt + h / Alt + l go back / forward (as Alt+Left / Alt+Right) in\r\n");
    appendf(buf, cch, L"; browsers, Explorer and most apps, and Alt + , / Alt + . scroll left /\r\n");
    appendf(buf, cch, L"; right instead. 0 = Alt + h / Alt + l scroll left / right.\r\n");
    appendf(buf, cch, L"alt_nav = %d\r\n", d.alt_nav ? 1 : 0);
    appendf(buf, cch, L"; 1 = when the keyboard focus is not in a text field, the Alt keys also\r\n");
    appendf(buf, cch, L"; work without Alt: f opens the hints, h j k l u d , . scroll and go\r\n");
    appendf(buf, cch, L"; back / forward. In a text field they need Alt and type as usual.\r\n");
    appendf(buf, cch, L"bare_keys = %d\r\n", d.bare_keys ? 1 : 0);
    appendf(buf, cch, L"; Notches per Alt + u / Alt + d tap (1-50), and the speeds while held.\r\n");
    appendf(buf, cch, L"fast_scroll_lines = %d\r\n", d.fast_scroll_lines);
    fmt_double(d.fast_scroll_speed, num, sizeof(num) / sizeof(num[0]));
    appendf(buf, cch, L"fast_scroll_speed = %ls\r\n", num);
    fmt_double(d.fast_scroll_max_speed, num, sizeof(num) / sizeof(num[0]));
    appendf(buf, cch, L"fast_scroll_max_speed = %ls\r\n", num);
    appendf(buf, cch, L"; Alt + u u / Alt + d d (two taps within this many ms) scroll to the\r\n");
    appendf(buf, cch, L"; top / bottom; 0 = off.\r\n");
    appendf(buf, cch, L"scroll_end_tap_ms = %d\r\n", d.scroll_end_tap_ms);

    add_key_line(buf, cch, L"Move the mouse left.", L"key_left", d.key_left);
    add_key_line(buf, cch, L"Move the mouse down.", L"key_down", d.key_down);
    add_key_line(buf, cch, L"Move the mouse up.", L"key_up", d.key_up);
    add_key_line(buf, cch, L"Move the mouse right.", L"key_right", d.key_right);
    add_key_line(buf, cch, L"Left button: tap clicks, tap twice double-clicks, hold drags.",
                 L"key_left_button", d.key_left_button);
    add_key_line(buf, cch, L"Right click.", L"key_right_click",
                 d.key_right_click);
    add_key_line(buf, cch, L"Middle click.", L"key_middle_click",
                 d.key_middle_click);
    add_key_line(buf, cch, L"Press and hold the left button; press again to release.",
                 L"key_drag_toggle", d.key_drag_toggle);
    add_key_line(buf, cch, L"Scroll up.", L"key_scroll_up", d.key_scroll_up);
    add_key_line(buf, cch, L"Scroll down.", L"key_scroll_down",
                 d.key_scroll_down);
    add_key_line(buf, cch, L"Scroll left.", L"key_scroll_left",
                 d.key_scroll_left);
    add_key_line(buf, cch, L"Scroll right.", L"key_scroll_right",
                 d.key_scroll_right);
    add_key_line(buf, cch, L"Open the click hints.", L"key_hints",
                 d.key_hints);
    add_key_line(buf, cch, L"Leave mouse mode (Esc and CapsLock also leave it).",
                 L"key_exit", d.key_exit);
    add_key_line(buf, cch, L"Show the help window.", L"key_help", d.key_help);

    appendf(buf, cch, L"\r\n[hints]\r\n");
    appendf(buf, cch, L"; Characters used for the hint grid labels: 2-60 unique letters/digits.\r\n");
    appendf(buf, cch, L"alphabet = %ls\r\n", d.alphabet);
    appendf(buf, cch, L"; Target size of one grid cell in pixels at 100%% scaling (24-400).\r\n");
    appendf(buf, cch, L"cell_size = %d\r\n", d.cell_size);
    appendf(buf, cch, L"; 1 = after the second letter, refine inside the chosen cell.\r\n");
    appendf(buf, cch, L"subgrid = %d\r\n", d.subgrid ? 1 : 0);
    appendf(buf, cch, L"; Subgrid rows separated by /, equal row lengths, 1x1 to 8x8.\r\n");
    appendf(buf, cch, L"subgrid_keys = %ls\r\n", d.subgrid_keys);
    appendf(buf, cch, L"; Label font size in pixels at 100%% scaling; 0 = automatic (or 6-72).\r\n");
    appendf(buf, cch, L"font_size = %d\r\n", d.font_size);
    appendf(buf, cch, L"; Opacity of the labels and status bar (30-255).\r\n");
    appendf(buf, cch, L"opacity = %u\r\n", (unsigned)d.opacity);
    appendf(buf, cch, L"; Label background colour as #RRGGBB.\r\n");
    _snwprintf(num, 64, L"#%02X%02X%02X", GetRValue(d.label_bg),
               GetGValue(d.label_bg), GetBValue(d.label_bg));
    appendf(buf, cch, L"label_bg = %ls\r\n", num);
    appendf(buf, cch, L"; Label text colour as #RRGGBB.\r\n");
    _snwprintf(num, 64, L"#%02X%02X%02X", GetRValue(d.label_fg),
               GetGValue(d.label_fg), GetBValue(d.label_fg));
    appendf(buf, cch, L"label_fg = %ls\r\n", num);
    appendf(buf, cch, L"; Label border colour as #RRGGBB.\r\n");
    _snwprintf(num, 64, L"#%02X%02X%02X", GetRValue(d.label_border),
               GetGValue(d.label_border), GetBValue(d.label_border));
    appendf(buf, cch, L"label_border = %ls\r\n", num);
    appendf(buf, cch, L"; Colour of already typed characters as #RRGGBB.\r\n");
    _snwprintf(num, 64, L"#%02X%02X%02X", GetRValue(d.match_fg),
               GetGValue(d.match_fg), GetBValue(d.match_fg));
    appendf(buf, cch, L"match_fg = %ls\r\n", num);
    appendf(buf, cch, L"; What the hint hotkey opens first: elements (clickable element\r\n");
    appendf(buf, cch, L"; labels) or grid. Another hotkey press reloads it.\r\n");
    appendf(buf, cch, L"hint_mode = %ls\r\n",
            d.hint_mode == HINT_MODE_GRID ? L"grid" : L"elements");
    appendf(buf, cch, L"; Element scan scope: monitor (every visible window) or window\r\n");
    appendf(buf, cch, L"; (only the active window and the taskbar).\r\n");
    appendf(buf, cch, L"scan_scope = %ls\r\n",
            d.scan_scope == SCAN_WINDOW ? L"window" : L"monitor");
    appendf(buf, cch, L"; Milliseconds to wait for every window's elements before showing\r\n");
    appendf(buf, cch, L"; the ones found so far (100-5000, default 400). The active window's\r\n");
    appendf(buf, cch, L"; elements are always waited for, up to 3 seconds; with nothing found by\r\n");
    appendf(buf, cch, L"; then, the hints fall back to the grid.\r\n");
    appendf(buf, cch, L"scan_timeout = %d\r\n", d.scan_timeout);
    appendf(buf, cch, L"; While the hints are open, rescan this often (ms) so elements that\r\n");
    appendf(buf, cch, L"; appear or move get labels by themselves (0-5000, default 500; 0 = off).\r\n");
    appendf(buf, cch, L"scan_refresh = %d\r\n", d.scan_refresh);
    appendf(buf, cch, L"; 1 = start finding clickable elements (and, for Alt scrolling, the\r\n");
    appendf(buf, cch, L"; scrollable areas) as soon as Alt goes down, so the labels and the\r\n");
    appendf(buf, cch, L"; first scroll come sooner. 0 = wait for the whole key combination.\r\n");
    appendf(buf, cch, L"prescan = %d\r\n", d.prescan ? 1 : 0);

    appendf(buf, cch, L"\r\n[shortcuts]\r\n%ls", SHORTCUTS_COMMENT);
    appendf(buf, cch, L"; Example:\r\n");
    appendf(buf, cch, L"; reor = \uB2E4\uC2DC \uC8FC\uBB38\uD558\uC2DC\uACA0\uC2B5\uB2C8\uAE4C?\r\n");
}

// Reads vim.ini or an imported shortcut file into a malloc'd, NUL-terminated
// string without a leading BOM. UTF-16 (with BOM) is decoded as such; UTF-8
// without a BOM falls back to the system code page so files saved by older
// editors are not lost. A UTF-8 BOM with bad bytes is an error.
enum { READ_OK = 0, READ_NOT_FOUND, READ_OPEN_FAILED, READ_FAILED,
       READ_BAD_UTF8 };

static int read_text_file(const WCHAR *path, WCHAR **out, BOOL warn_ansi)
{
    HANDLE h;
    DWORD size, total = 0, got = 0;
    char *raw;
    WCHAR *text;
    int len_bytes, widelen, skip = 0;
    BOOL utf16le = FALSE, utf16be = FALSE, utf8bom = FALSE, ansi = FALSE;

    *out = NULL;
    h = CreateFileW(path, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return READ_NOT_FOUND;
        return READ_OPEN_FAILED;
    }

    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > 4 * 1024 * 1024) {
        CloseHandle(h);
        return READ_FAILED;
    }
    raw = (char *)malloc(size > 0 ? size : 1);
    if (raw == NULL) {
        CloseHandle(h);
        return READ_FAILED;
    }
    while (total < size) {
        if (!ReadFile(h, raw + total, size - total, &got, NULL) || got == 0) {
            free(raw);
            CloseHandle(h);
            return READ_FAILED;
        }
        total += got;
    }
    CloseHandle(h);

    len_bytes = (int)total;
    if (len_bytes >= 2 && (unsigned char)raw[0] == 0xFF &&
        (unsigned char)raw[1] == 0xFE) {
        utf16le = TRUE;
        skip = 2;
    } else if (len_bytes >= 2 && (unsigned char)raw[0] == 0xFE &&
               (unsigned char)raw[1] == 0xFF) {
        utf16be = TRUE;
        skip = 2;
    } else if (len_bytes >= 3 && (unsigned char)raw[0] == 0xEF &&
               (unsigned char)raw[1] == 0xBB &&
               (unsigned char)raw[2] == 0xBF) {
        utf8bom = TRUE;
        skip = 3;
    }

    if (utf16le || utf16be) {
        int n, i;

        if ((len_bytes - skip) % 2 != 0) {
            free(raw);
            return READ_BAD_UTF8;
        }
        n = (len_bytes - skip) / 2;
        text = (WCHAR *)malloc(((size_t)n + 1) * sizeof(WCHAR));
        if (text == NULL) {
            free(raw);
            return READ_FAILED;
        }
        memcpy(text, raw + skip, (size_t)n * sizeof(WCHAR));
        if (utf16be) {
            for (i = 0; i < n; i++)
                text[i] = (WCHAR)((text[i] << 8) | (text[i] >> 8));
        }
        text[n] = 0;
        free(raw);
        *out = text;
        return READ_OK;
    }

    widelen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw + skip,
                                  len_bytes - skip, NULL, 0);
    if (widelen == 0 && len_bytes > skip) {
        if (utf8bom) {
            free(raw);
            return READ_BAD_UTF8;
        }
        // Not UTF-8: an older Notepad's ANSI file. The shortcut editor
        // rewrites it as UTF-8, so say so before that happens.
        skip = 0;
        ansi = TRUE;
        widelen = MultiByteToWideChar(CP_ACP, 0, raw, len_bytes, NULL, 0);
        if (widelen == 0) {
            free(raw);
            return READ_FAILED;
        }
        if (warn_ansi)
            warn_add(0, L"the settings file is not UTF-8; read as the system "
                        L"code page, it will be saved as UTF-8 when "
                        L"shortcuts are saved");
    }
    text = (WCHAR *)malloc(((size_t)widelen + 1) * sizeof(WCHAR));
    if (text == NULL) {
        free(raw);
        return READ_FAILED;
    }
    if (widelen > 0)
        MultiByteToWideChar(ansi ? CP_ACP : CP_UTF8,
                            ansi ? 0 : MB_ERR_INVALID_CHARS, raw + skip,
                            len_bytes - skip, text, widelen);
    text[widelen] = 0;
    free(raw);
    if (text[0] == 0xFEFF)
        memmove(text, text + 1, (size_t)widelen * sizeof(WCHAR));
    *out = text;
    return READ_OK;
}

static BOOL write_text_file(const WCHAR *path, const WCHAR *text)
{
    char *utf8;
    int bytes;
    DWORD written = 0;
    BOOL ok;
    HANDLE h;

    bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (bytes <= 0)
        return FALSE;
    utf8 = (char *)malloc((size_t)bytes);
    if (utf8 == NULL)
        return FALSE;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, bytes, NULL, NULL);

    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        free(utf8);
        return FALSE;
    }
    ok = WriteFile(h, utf8, (DWORD)(bytes - 1), &written, NULL) &&
         written == (DWORD)(bytes - 1);
    CloseHandle(h);
    free(utf8);
    return ok;
}

static void write_default_file(const WCHAR *path)
{
    static WCHAR text[16384];

    build_default_ini(text, sizeof(text) / sizeof(text[0]));
    if (!write_text_file(path, text))
        warn_add(0, L"could not create the settings file");
}

// ---------------------------------------------------------------------------
// Saving the [shortcuts] section

typedef struct {
    WCHAR *s;
    size_t len, cap;
    BOOL   failed;
} WBuf;

static void wbuf_add(WBuf *b, const WCHAR *s, size_t n)
{
    if (b->failed)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        WCHAR *grown;
        while (b->len + n + 1 > cap)
            cap *= 2;
        grown = (WCHAR *)realloc(b->s, cap * sizeof(WCHAR));
        if (grown == NULL) {
            b->failed = TRUE;
            return;
        }
        b->s = grown;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n * sizeof(WCHAR));
    b->len += n;
    b->s[b->len] = 0;
}

static void wbuf_str(WBuf *b, const WCHAR *s)
{
    wbuf_add(b, s, wcslen(s));
}

// Inverse of sc_unescape. \r is dropped (edit controls give \r\n), and
// leading/trailing spaces become \s so the parser's trim keeps them.
static void wbuf_escaped(WBuf *b, const WCHAR *text)
{
    size_t n = wcslen(text);
    size_t i;

    for (i = 0; i < n; i++) {
        WCHAR ch = text[i];
        if (ch == L'\r')
            continue;
        if (ch == L'\\')
            wbuf_str(b, L"\\\\");
        else if (ch == L'\n')
            wbuf_str(b, L"\\n");
        else if (ch == L'\t')
            wbuf_str(b, L"\\t");
        else if (ch == L' ' && (i == 0 || i == n - 1))
            wbuf_str(b, L"\\s");
        else
            wbuf_add(b, &ch, 1);
    }
}

static BOOL is_shortcuts_header(const WCHAR *line, size_t n)
{
    WCHAR name[32];
    size_t i = 0, k = 0;

    while (i < n && is_space(line[i]))
        i++;
    if (i >= n || line[i] != L'[')
        return FALSE;
    for (i++; i < n && line[i] != L']'; i++) {
        if (is_space(line[i]))
            continue;
        if (k + 1 >= sizeof(name) / sizeof(name[0]))
            return FALSE;
        name[k++] = line[i];
    }
    name[k] = 0;
    return _wcsicmp(name, L"shortcuts") == 0;
}

static BOOL is_comment_or_blank(const WCHAR *line, size_t n)
{
    size_t i = 0;

    while (i < n && is_space(line[i]))
        i++;
    return i >= n || line[i] == L';' || line[i] == L'#';
}

static BOOL is_section_header(const WCHAR *line, size_t n)
{
    size_t i = 0;

    while (i < n && is_space(line[i]))
        i++;
    return i < n && line[i] == L'[';
}

BOOL shortcuts_save(const WCHAR *const *abbrs, const WCHAR *const *texts,
                    int count)
{
    static WCHAR defaults[16384];
    const WCHAR *path = config_path();
    WCHAR tmp_path[APP_PATH_MAX];
    WCHAR *text = NULL;
    const WCHAR *p;
    const WCHAR *trailer = NULL;
    WBuf out;
    BOOL skipping = FALSE;
    BOOL seen = FALSE;
    BOOL ok;
    int i;

    ZeroMemory(&out, sizeof(out));
    switch (read_text_file(path, &text, FALSE)) {
    case READ_OK:
        break;
    case READ_NOT_FOUND:
        build_default_ini(defaults, sizeof(defaults) / sizeof(defaults[0]));
        text = _wcsdup(defaults);
        if (text == NULL)
            return FALSE;
        break;
    default:
        return FALSE;
    }

    // Keep every line except the old [shortcuts] section, byte for byte.
    // The run of comment and blank lines directly before the next section
    // header belongs to that header, so it stays too. Before the first
    // shortcut it is the header shortcuts_save writes itself, which must not
    // be copied and then written a second time.
    p = text;
    while (*p) {
        const WCHAR *e = p;
        size_t n;

        while (*e && *e != L'\r' && *e != L'\n')
            e++;
        n = (size_t)(e - p);
        if (*e == L'\r')
            e++;
        if (*e == L'\n')
            e++;
        if (is_section_header(p, n)) {
            if (skipping && trailer)
                wbuf_add(&out, trailer, (size_t)(p - trailer));
            trailer = NULL;
            skipping = is_shortcuts_header(p, n);
            seen = FALSE;
            if (skipping) {
                p = e;
                continue;
            }
        }
        if (!skipping)
            wbuf_add(&out, p, (size_t)(e - p));
        else if (!is_comment_or_blank(p, n)) {
            seen = TRUE;
            trailer = NULL;
        } else if (seen && trailer == NULL) {
            trailer = p;
        }
        p = e;
    }
    free(text);
    while (out.len > 0 && is_space(out.s[out.len - 1]))
        out.s[--out.len] = 0;
    if (out.len > 0)
        wbuf_str(&out, L"\r\n\r\n");

    wbuf_str(&out, L"[shortcuts]\r\n");
    wbuf_str(&out, SHORTCUTS_COMMENT);
    for (i = 0; i < count; i++) {
        if (!shortcut_abbr_valid(abbrs[i]))
            continue;
        wbuf_str(&out, abbrs[i]);
        wbuf_str(&out, L" = ");
        wbuf_escaped(&out, texts[i] ? texts[i] : L"");
        wbuf_str(&out, L"\r\n");
    }
    if (out.failed) {
        free(out.s);
        return FALSE;
    }

    // Write a temporary file and move it over, so a failed write never
    // leaves a half-written vim.ini.
    _snwprintf(tmp_path, sizeof(tmp_path) / sizeof(tmp_path[0]), L"%ls.tmp",
               path);
    tmp_path[sizeof(tmp_path) / sizeof(tmp_path[0]) - 1] = 0;
    ok = write_text_file(tmp_path, out.s);
    free(out.s);
    if (ok)
        ok = MoveFileExW(tmp_path, path, MOVEFILE_REPLACE_EXISTING);
    if (!ok)
        DeleteFileW(tmp_path);
    return ok;
}

// ---------------------------------------------------------------------------
// Load

BOOL config_load(Config *c, BOOL *created)
{
    const WCHAR *path;
    WCHAR *text = NULL;
    WCHAR *p;
    int line_no = 0, section = SEC_NONE;

    if (c == NULL)
        return FALSE;
    if (created != NULL)
        *created = FALSE;
    g_warnings[0] = 0;
    config_set_defaults(c);
    sc_free(g_sc_new, &g_sc_new_count);
    path = config_path();

    switch (read_text_file(path, &text, TRUE)) {
    case READ_OK:
        break;
    case READ_NOT_FOUND:
        if (created != NULL)
            *created = TRUE;
        write_default_file(path);
        sc_commit();
        return TRUE;
    case READ_OPEN_FAILED:
        warn_add(0, L"could not open the settings file");
        return FALSE;
    case READ_BAD_UTF8:
        warn_add(0, L"could not decode the settings file as UTF-8");
        return FALSE;
    default:
        warn_add(0, L"could not read the settings file");
        return FALSE;
    }

    p = text;
    while (*p) {
        WCHAR *line = p;
        WCHAR *e = p;

        while (*e && *e != L'\r' && *e != L'\n')
            e++;
        if (*e == L'\r') {
            *e = 0;
            e++;
            if (*e == L'\n')
                e++;
        } else if (*e == L'\n') {
            *e = 0;
            e++;
        }
        line_no++;
        process_line(c, line_no, line, &section);
        p = e;
    }
    free(text);
    sc_commit();
    return TRUE;
}

// ---------------------------------------------------------------------------
// Import / export

BOOL shortcuts_export(const WCHAR *path)
{
    WBuf out;
    WCHAR abbr[SHORTCUT_ABBR_MAX + 1];
    BOOL ok;
    int i, n = shortcut_count();

    ZeroMemory(&out, sizeof(out));
    wbuf_str(&out, L"; vim mouse text shortcuts. Import them from the tray menu's\r\n");
    wbuf_str(&out, L"; \"Text shortcuts...\" window.\r\n");
    wbuf_str(&out, L"[shortcuts]\r\n");
    wbuf_str(&out, SHORTCUTS_COMMENT);
    for (i = 0; i < n; i++) {
        WCHAR *text = NULL;
        if (!shortcut_get(i, abbr, SHORTCUT_ABBR_MAX + 1, &text))
            continue;
        wbuf_str(&out, abbr);
        wbuf_str(&out, L" = ");
        wbuf_escaped(&out, text);
        wbuf_str(&out, L"\r\n");
        free(text);
    }
    if (out.failed) {
        free(out.s);
        return FALSE;
    }
    ok = write_text_file(path, out.s);
    free(out.s);
    return ok;
}

void shortcut_entries_free(ShortcutEntry *entries, int count)
{
    int i;

    if (entries == NULL)
        return;
    for (i = 0; i < count; i++)
        free(entries[i].text);
    free(entries);
}

ShortcutEntry *shortcuts_read_file(const WCHAR *path, int *count,
                                   int *skipped)
{
    static WCHAR value[SHORTCUT_TEXT_MAX + 1];
    ShortcutEntry *entries;
    WCHAR *text = NULL;
    WCHAR *p;
    int n = 0, bad = 0;
    // Lines before any [section] count (a plain list); in a full vim.ini
    // only the [shortcuts] section does.
    BOOL accept = TRUE;

    *count = 0;
    *skipped = 0;
    if (read_text_file(path, &text, FALSE) != READ_OK)
        return NULL;
    entries = (ShortcutEntry *)calloc(SHORTCUT_MAX, sizeof(ShortcutEntry));
    if (entries == NULL) {
        free(text);
        return NULL;
    }

    p = text;
    while (*p) {
        WCHAR *line = p;
        WCHAR *e = p;
        WCHAR *eq, *key, *val;
        int i, j;

        while (*e && *e != L'\r' && *e != L'\n')
            e++;
        if (*e == L'\r') {
            *e++ = 0;
            if (*e == L'\n')
                e++;
        } else if (*e == L'\n') {
            *e++ = 0;
        }
        p = e;
        trim(&line);
        if (*line == 0 || *line == L';' || *line == L'#')
            continue;
        if (*line == L'[') {
            WCHAR *close = wcschr(line, L']');
            if (close) {
                *close = 0;
                key = line + 1;
                trim(&key);
                accept = _wcsicmp(key, L"shortcuts") == 0;
            }
            continue;
        }
        if (!accept)
            continue;
        eq = wcschr(line, L'=');
        if (eq == NULL) {
            bad++;
            continue;
        }
        *eq = 0;
        key = line;
        val = eq + 1;
        trim(&key);
        trim(&val);
        if (!shortcut_abbr_valid(key) || *val == 0) {
            bad++;
            continue;
        }
        sc_unescape(val, value, sizeof(value) / sizeof(value[0]));
        for (j = 0; j < n; j++) {
            if (_wcsicmp(entries[j].abbr, key) == 0)
                break;
        }
        if (j == n) {
            if (n >= SHORTCUT_MAX) {
                bad++;
                continue;
            }
            n++;
        }
        // A later line for the same abbreviation wins.
        free(entries[j].text);
        entries[j].text = _wcsdup(value);
        for (i = 0; key[i]; i++)
            entries[j].abbr[i] = lower_ascii(key[i]);
        entries[j].abbr[i] = 0;
        if (entries[j].text == NULL) {
            n--;
            bad++;
        }
    }
    free(text);
    *count = n;
    *skipped = bad;
    return entries;
}
