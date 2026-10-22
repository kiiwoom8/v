// hint.c - click-hint overlay (main/UI thread).

#include "common.h"
#include <stdlib.h>
#include <wchar.h>

#ifndef WS_EX_NOACTIVATE
#define WS_EX_NOACTIVATE 0x08000000L
#endif
#ifndef MAPVK_VK_TO_CHAR
#define MAPVK_VK_TO_CHAR 2
#endif
#ifndef LWA_COLORKEY
#define LWA_COLORKEY 0x1
#endif
#ifndef LWA_ALPHA
#define LWA_ALPHA 0x2
#endif
#ifndef CLEARTYPE_QUALITY
#define CLEARTYPE_QUALITY 5
#endif
#ifndef VK_NUMPAD0
#define VK_NUMPAD0 0x60
#endif
#ifndef VK_NUMPAD9
#define VK_NUMPAD9 0x69
#endif

#define HINT_CLASS      L"VimMouseHintOverlay"
#define KEY_COLOR       RGB(255, 0, 255)
#define TIMER_FLASH_ID  1
#define TIMER_SCAN_ID   2
#define MAX_MONITORS    16
#define MAX_TARGETS     2000
#define MAX_LABEL_LEN   16
#define MAX_QUEUED_KEYS 32
#define DOT             L"\x00B7"

typedef struct {
    HMONITOR mon;
    RECT     rect;
} MonInfo;

typedef struct {
    WCHAR s[MAX_LABEL_LEN];
} HintLabel;

typedef struct {
    UINT vk;
    UINT mods;
} QueuedKey;

static HINSTANCE g_hint_inst;
static HWND      g_hint_wnd;
static BOOL      g_open;
static int       g_mode;        // HINT_MODE_*
static int       g_start_mode;  // mode the overlay opened in (hotkey cycling)
static int       g_state;       // 0 = nothing, 1 = row chosen, 2 = cell chosen
static int       g_row;
static int       g_col;

static BOOL      g_scanning;
static UINT      g_scan_id;
static ClickList *g_scan_list;  // newest partial result, owned here
static WCHAR     g_prefix[16];
static int       g_prefix_len;
static QueuedKey g_queue[MAX_QUEUED_KEYS];
static int       g_queue_count;
static ClickTarget *g_targets;
static HintLabel *g_labels;
static int       g_target_count;
static WCHAR     g_notice[160]; // transient status for this open (grid fallback)

static HMONITOR  g_mon;
static RECT      g_mon_rect;
static int       g_mon_w, g_mon_h;
static int       g_dpi;
static int       g_cell_px;
static int       g_n;
static int       g_cols, g_rows;
static int       g_base_w, g_extra_w;
static int       g_base_h, g_extra_h;

static int       g_sub_rows, g_sub_cols;
static WCHAR     g_sub[8][8];

static BOOL      g_flash;

static COLORREF  g_fg, g_match, g_border;
static HFONT     g_font_label, g_font_sub, g_font_status;
static int       g_font_label_px, g_font_sub_px, g_font_status_px;
static HBRUSH    g_br_key, g_br_label, g_br_status, g_br_status_red;
static HPEN      g_pen_border, g_pen_sel;

static MonInfo   g_mons[MAX_MONITORS];
static int       g_mon_count;

static void finalize_scan(void);
static void reset_input(void);
static void handle_key(UINT vk, UINT mods);
static void invalidate(void);
static void cycle_monitor(UINT mods);

// ---------------------------------------------------------------------------
// small helpers

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static WCHAR lower_ascii(WCHAR ch)
{
    if (ch >= L'A' && ch <= L'Z') return (WCHAR)(ch - L'A' + L'a');
    return ch;
}

static COLORREF safe_color(COLORREF c, COLORREF fallback)
{
    return c == KEY_COLOR ? fallback : c;
}

// subgrid_keys: rows separated by '/'. Returns FALSE if malformed.
static BOOL parse_subgrid_into(const WCHAR *src, WCHAR out[8][8],
                               int *prows, int *pcols)
{
    int r = 0, c = 0, cols = -1;
    const WCHAR *p;

    if (!src || !*src) return FALSE;
    for (p = src; ; ++p) {
        WCHAR ch = *p;
        if (ch == 0 || ch == L'/') {
            if (c == 0) return FALSE;
            if (cols < 0) cols = c;
            else if (c != cols) return FALSE;
            ++r;
            if (r > 8) return FALSE;
            c = 0;
            if (ch == 0) break;
            if (p[1] == 0) return FALSE;  // trailing slash
        } else {
            if (c >= 8) return FALSE;
            out[r][c++] = lower_ascii(ch);
        }
    }
    if (r < 1 || cols < 1 || cols > 8) return FALSE;

    // keys must be unique
    {
        int i, j, k, l;
        for (i = 0; i < r; ++i) {
            for (j = 0; j < cols; ++j) {
                for (k = 0; k < i; ++k)
                    for (l = 0; l < cols; ++l)
                        if (out[k][l] == out[i][j]) return FALSE;
                for (l = 0; l < j; ++l)
                    if (out[i][l] == out[i][j]) return FALSE;
            }
        }
    }

    *prows = r;
    *pcols = cols;
    return TRUE;
}

static void parse_subgrid(void)
{
    if (!parse_subgrid_into(g_cfg.subgrid_keys, g_sub,
                            &g_sub_rows, &g_sub_cols)) {
        if (!parse_subgrid_into(L"qwer/asdf/zxcv", g_sub,
                                &g_sub_rows, &g_sub_cols)) {
            g_sub_rows = 1;
            g_sub_cols = 1;
            g_sub[0][0] = L'q';
        }
    }
}

static BOOL compute_geometry(HMONITOR mon)
{
    MONITORINFO mi;
    int w, h, cell, n, cols, rows;

    if (!mon) return FALSE;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return FALSE;

    w = mi.rcMonitor.right - mi.rcMonitor.left;
    h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (w <= 0 || h <= 0) return FALSE;

    g_dpi = dpi_for_monitor(mon);
    if (g_dpi <= 0) g_dpi = 96;

    cell = g_cfg.cell_size * g_dpi / 96;
    if (cell < 1) cell = 1;

    n = (int)wcslen(g_cfg.alphabet);
    if (n < 1) return FALSE;
    if (n > 63) n = 63;

    cols = (w + cell / 2) / cell;
    rows = (h + cell / 2) / cell;
    cols = clamp_int(cols, 1, n);
    rows = clamp_int(rows, 1, n);

    g_mon = mon;
    g_mon_rect = mi.rcMonitor;
    g_mon_w = w;
    g_mon_h = h;
    g_cell_px = cell;
    g_n = n;
    g_cols = cols;
    g_rows = rows;
    g_base_w = w / cols;
    g_extra_w = w % cols;
    g_base_h = h / rows;
    g_extra_h = h % rows;

    parse_subgrid();
    return TRUE;
}

static void cell_rect(int r, int c, RECT *out)
{
    int x = c * g_base_w + (c < g_extra_w ? c : g_extra_w);
    int y = r * g_base_h + (r < g_extra_h ? r : g_extra_h);
    int w = g_base_w + (c < g_extra_w ? 1 : 0);
    int h = g_base_h + (r < g_extra_h ? 1 : 0);

    out->left = x;
    out->top = y;
    out->right = x + w;
    out->bottom = y + h;
}

static void subcell_rect(RECT cell, int sr, int sc, RECT *out)
{
    int cw = cell.right - cell.left;
    int ch = cell.bottom - cell.top;
    int sw = cw / g_sub_cols;
    int ew = cw % g_sub_cols;
    int sh = ch / g_sub_rows;
    int eh = ch % g_sub_rows;
    int x = cell.left + sc * sw + (sc < ew ? sc : ew);
    int y = cell.top + sr * sh + (sr < eh ? sr : eh);
    int w = sw + (sc < ew ? 1 : 0);
    int h = sh + (sr < eh ? 1 : 0);

    out->left = x;
    out->top = y;
    out->right = x + w;
    out->bottom = y + h;
}

static POINT cell_center(int r, int c)
{
    RECT rc;
    POINT pt;

    cell_rect(r, c, &rc);
    pt.x = g_mon_rect.left + (rc.left + rc.right) / 2;
    pt.y = g_mon_rect.top + (rc.top + rc.bottom) / 2;
    return pt;
}

static POINT subcell_center(RECT cell, int sr, int sc)
{
    RECT sub;
    POINT pt;

    subcell_rect(cell, sr, sc, &sub);
    pt.x = g_mon_rect.left + (sub.left + sub.right) / 2;
    pt.y = g_mon_rect.top + (sub.top + sub.bottom) / 2;
    return pt;
}

// ---------------------------------------------------------------------------
// GDI resources

static HFONT make_font(int px, int weight)
{
    if (px < 1) px = 1;
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
}

static int pick_font_px(HDC dc, const WCHAR *chars, int nchars,
                        int cellw, int cellh, int start_px, BOOL two_chars)
{
    int px;

    if (start_px < 6) start_px = 6;
    for (px = start_px; px >= 6; --px) {
        HFONT f = make_font(px, FW_BOLD);
        HGDIOBJ old;
        int maxw = 0, maxh = 0, i, pad, tw;

        if (!f) return 6;
        old = SelectObject(dc, f);
        for (i = 0; i < nchars; ++i) {
            SIZE sz;
            if (GetTextExtentPoint32W(dc, &chars[i], 1, &sz)) {
                if (sz.cx > maxw) maxw = sz.cx;
                if (sz.cy > maxh) maxh = sz.cy;
            }
        }
        SelectObject(dc, old);
        DeleteObject(f);

        tw = two_chars ? maxw * 2 : maxw;
        pad = px / 4;
        if (pad < 2) pad = 2;
        if (tw + 2 * pad <= cellw - 2 && maxh + 2 * pad <= cellh - 2)
            return px;
    }
    return 6;
}

static void destroy_resources(void)
{
    if (g_font_label) { DeleteObject(g_font_label); g_font_label = NULL; }
    if (g_font_sub) { DeleteObject(g_font_sub); g_font_sub = NULL; }
    if (g_font_status) { DeleteObject(g_font_status); g_font_status = NULL; }
    if (g_br_key) { DeleteObject(g_br_key); g_br_key = NULL; }
    if (g_br_label) { DeleteObject(g_br_label); g_br_label = NULL; }
    if (g_br_status) { DeleteObject(g_br_status); g_br_status = NULL; }
    if (g_br_status_red) { DeleteObject(g_br_status_red); g_br_status_red = NULL; }
    if (g_pen_border) { DeleteObject(g_pen_border); g_pen_border = NULL; }
    if (g_pen_sel) { DeleteObject(g_pen_sel); g_pen_sel = NULL; }
}

static BOOL create_resources(void)
{
    HDC dc;
    int min_cw, min_ch, start, alen;
    WCHAR subchars[64];
    int sn = 0, r, c, subw, subh, substart;

    destroy_resources();

    g_fg = safe_color(g_cfg.label_fg, RGB(0, 0, 0));
    g_match = safe_color(g_cfg.match_fg, RGB(96, 96, 96));
    g_border = safe_color(g_cfg.label_border, RGB(0, 0, 0));

    g_br_key = CreateSolidBrush(KEY_COLOR);
    g_br_label = CreateSolidBrush(safe_color(g_cfg.label_bg, RGB(255, 255, 255)));
    g_br_status = CreateSolidBrush(RGB(0x1E, 0x1E, 0x1E));
    g_br_status_red = CreateSolidBrush(RGB(0xC0, 0x39, 0x2B));
    g_pen_border = CreatePen(PS_SOLID, 1, g_border);
    g_pen_sel = CreatePen(PS_SOLID, 2, g_border);

    if (!g_br_key || !g_br_label || !g_br_status || !g_br_status_red ||
        !g_pen_border || !g_pen_sel) {
        destroy_resources();
        return FALSE;
    }

    dc = GetDC(NULL);
    if (!dc) {
        destroy_resources();
        return FALSE;
    }

    min_cw = g_base_w > 0 ? g_base_w : 1;
    min_ch = g_base_h > 0 ? g_base_h : 1;
    start = MulDiv(g_cfg.font_size > 0 ? g_cfg.font_size : 13, g_dpi, 96);
    if (start < 6) start = 6;

    alen = (int)wcslen(g_cfg.alphabet);
    if (alen < 1) alen = 1;
    g_font_label_px = pick_font_px(dc, g_cfg.alphabet, alen,
                                   min_cw, min_ch, start, TRUE);
    g_font_label = make_font(g_font_label_px, FW_BOLD);

    for (r = 0; r < g_sub_rows; ++r)
        for (c = 0; c < g_sub_cols; ++c)
            if (sn < (int)(sizeof(subchars) / sizeof(subchars[0])))
                subchars[sn++] = g_sub[r][c];

    subw = g_base_w / g_sub_cols;
    subh = g_base_h / g_sub_rows;
    if (subw < 1) subw = 1;
    if (subh < 1) subh = 1;
    substart = g_font_label_px * 3 / 4;
    if (substart < 6) substart = 6;
    g_font_sub_px = pick_font_px(dc, subchars, sn, subw, subh, substart, FALSE);
    g_font_sub = make_font(g_font_sub_px, FW_BOLD);

    g_font_status_px = MulDiv(12, g_dpi, 96);
    if (g_font_status_px < 6) g_font_status_px = 6;
    g_font_status = make_font(g_font_status_px, FW_NORMAL);

    ReleaseDC(NULL, dc);

    if (!g_font_label || !g_font_sub || !g_font_status) {
        destroy_resources();
        return FALSE;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// drawing

static void measure_label(HDC dc, const WCHAR *text, int len,
                          int *w, int *tw, int *th)
{
    int i;

    *tw = 0;
    *th = 0;
    for (i = 0; i < len && i < MAX_LABEL_LEN; ++i) {
        SIZE sz;
        w[i] = 0;
        if (GetTextExtentPoint32W(dc, &text[i], 1, &sz)) {
            w[i] = sz.cx;
            if (sz.cy > *th) *th = sz.cy;
        }
        *tw += w[i];
    }
}

// Draws the box and text already positioned at left/top. typed chars at the
// start of the label use split_col (the typed prefix).
static void draw_box_text(HDC dc, int left, int top, int bw, int bh,
                          const WCHAR *text, int len, int typed,
                          COLORREF split_col, int font_px,
                          const int *wchars, int tw, int th)
{
    HGDIOBJ old_br, old_pen;
    int i, rad, x, y;

    rad = MulDiv(4, g_dpi, 96);
    if (rad < 2) rad = 2;
    if (rad > bh / 2) rad = bh / 2;
    if (rad < 1) rad = 1;

    old_br = SelectObject(dc, g_br_label);
    old_pen = SelectObject(dc, g_pen_border);
    RoundRect(dc, left, top, left + bw, top + bh, rad, rad);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_br);

    x = left + (bw - tw) / 2;
    y = top + (bh - th) / 2;
    for (i = 0; i < len && i < MAX_LABEL_LEN; ++i) {
        SetTextColor(dc, i < typed ? split_col : g_fg);
        TextOutW(dc, x, y, &text[i], 1);
        x += wchars[i];
    }
    (void)font_px;
}

// corner: pin the box to the top-left of the cell instead of centring it
static void draw_label_box(HDC dc, RECT cell, const WCHAR *text, int len,
                           int typed, COLORREF split_col,
                           HFONT font, int font_px, BOOL corner)
{
    HGDIOBJ old_font;
    int w[MAX_LABEL_LEN];
    int tw, th, pad, bw, bh, cw, ch, cx, cy, left, top;

    if (len <= 0) return;
    if (len > MAX_LABEL_LEN) len = MAX_LABEL_LEN;

    old_font = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    measure_label(dc, text, len, w, &tw, &th);

    pad = font_px / 4;
    if (pad < 2) pad = 2;
    bw = tw + 2 * pad;
    bh = th + 2 * pad;

    cw = cell.right - cell.left;
    ch = cell.bottom - cell.top;
    if (bw > cw) bw = cw;
    if (bh > ch) bh = ch;
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;

    if (corner) {
        left = cell.left + 2;
        top = cell.top + 2;
    } else {
        cx = (cell.left + cell.right) / 2;
        cy = (cell.top + cell.bottom) / 2;
        left = cx - bw / 2;
        top = cy - bh / 2;
    }

    draw_box_text(dc, left, top, bw, bh, text, len, typed, split_col,
                  font_px, w, tw, th);
    SelectObject(dc, old_font);
}

// A label pinned at the target's top-left, nudged inside the monitor.
static void draw_element_label(HDC dc, const ClickTarget *t,
                               const WCHAR *text, int len, int typed)
{
    HGDIOBJ old_font;
    int w[MAX_LABEL_LEN];
    int tw, th, pad, bw, bh, left, top;

    if (len <= 0) return;
    if (len > MAX_LABEL_LEN) len = MAX_LABEL_LEN;

    old_font = SelectObject(dc, g_font_label ? g_font_label
                                             : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    measure_label(dc, text, len, w, &tw, &th);

    pad = g_font_label_px / 4;
    if (pad < 2) pad = 2;
    bw = tw + 2 * pad;
    bh = th + 2 * pad;

    left = t->rc.left - g_mon_rect.left;
    top = t->rc.top - g_mon_rect.top;
    if (left + bw > g_mon_w) left = g_mon_w - bw;
    if (top + bh > g_mon_h) top = g_mon_h - bh;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (bw > g_mon_w) bw = g_mon_w;
    if (bh > g_mon_h) bh = g_mon_h;
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;

    draw_box_text(dc, left, top, bw, bh, text, len, typed, g_match,
                  g_font_label_px, w, tw, th);
    SelectObject(dc, old_font);
}

static void draw_target_outline(HDC dc, RECT rc)
{
    HGDIOBJ old_br, old_pen;

    old_br = SelectObject(dc, GetStockObject(NULL_BRUSH));
    old_pen = SelectObject(dc, g_pen_border);
    Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_br);
}

// ---------------------------------------------------------------------------
// element labels / targets

static BOOL label_has_prefix(const WCHAR *label, const WCHAR *prefix)
{
    while (*prefix) {
        if (*label != *prefix) return FALSE;
        ++label;
        ++prefix;
    }
    return TRUE;
}

static BOOL element_matches(int i)
{
    return label_has_prefix(g_labels[i].s, g_prefix);
}

static int count_matches(void)
{
    int i, n = 0;

    for (i = 0; i < g_target_count; ++i)
        if (element_matches(i)) ++n;
    return n;
}

static int exact_match(void)
{
    int i;

    for (i = 0; i < g_target_count; ++i)
        if (wcscmp(g_labels[i].s, g_prefix) == 0) return i;
    return -1;
}

static int only_match(void)
{
    int i, found = -1;

    for (i = 0; i < g_target_count; ++i) {
        if (!element_matches(i)) continue;
        if (found >= 0) return -1;
        found = i;
    }
    return found;
}

static void free_elements(void)
{
    free(g_targets);
    free(g_labels);
    g_targets = NULL;
    g_labels = NULL;
    g_target_count = 0;
}

static int target_band(int top)
{
    int band = MulDiv(16, g_dpi, 96);

    if (band < 1) band = 1;
    return top >= 0 ? top / band : (top - band + 1) / band;
}

static int __cdecl target_cmp(const void *pa, const void *pb)
{
    const ClickTarget *a = (const ClickTarget *)pa;
    const ClickTarget *b = (const ClickTarget *)pb;
    int ba = target_band(a->rc.top);
    int bb = target_band(b->rc.top);

    if (ba != bb) return ba < bb ? -1 : 1;
    if (a->rc.left != b->rc.left) return a->rc.left < b->rc.left ? -1 : 1;
    if (a->rc.top != b->rc.top) return a->rc.top < b->rc.top ? -1 : 1;
    return 0;
}

// Vimium's prefix-free labelling: grow labels by prefixing each earlier hint
// with every alphabet character, then take the n oldest unexpanded ones.
static HintLabel *build_labels(int n)
{
    int alen = (int)wcslen(g_cfg.alphabet);
    int cap, count, offset, i, j;
    HintLabel *pool, *out;

    if (n <= 0 || alen < 1) return NULL;
    if (alen > 60) alen = 60;
    cap = 2 * n + 2 * alen + 16;
    pool = (HintLabel *)calloc((size_t)cap, sizeof(*pool));
    out = (HintLabel *)calloc((size_t)n, sizeof(*out));
    if (!pool || !out) {
        free(pool);
        free(out);
        return NULL;
    }

    count = 1;
    offset = 0;
    pool[0].s[0] = 0;
    while (count - offset < n || count == 1) {
        const WCHAR *base;
        int len;

        if (offset >= cap) break;
        base = pool[offset].s;
        len = (int)wcslen(base);
        if (len > MAX_LABEL_LEN - 2) len = MAX_LABEL_LEN - 2;
        ++offset;

        for (j = 0; j < alen && count < cap; ++j) {
            WCHAR *dst = pool[count].s;
            for (i = 0; i < len; ++i) dst[i] = base[i];
            dst[len] = lower_ascii(g_cfg.alphabet[j]);
            dst[len + 1] = 0;
            ++count;
        }
    }

    for (i = 0; i < n; ++i) {
        int src = offset + i;
        if (src >= count) src = count - 1;
        if (src < 0) src = 0;
        memcpy(out[i].s, pool[src].s, sizeof(out[i].s));
    }
    free(pool);
    return out;
}

static BOOL element_status_overlaps(RECT br)
{
    int i;

    if (g_mode != HINT_MODE_ELEMENTS || !g_targets) return FALSE;
    for (i = 0; i < g_target_count; ++i) {
        RECT t, hit;
        if (!element_matches(i)) continue;
        t.left = g_targets[i].rc.left - g_mon_rect.left;
        t.top = g_targets[i].rc.top - g_mon_rect.top;
        t.right = g_targets[i].rc.right - g_mon_rect.left;
        t.bottom = g_targets[i].rc.bottom - g_mon_rect.top;
        if (IntersectRect(&hit, &t, &br)) return TRUE;
    }
    return FALSE;
}

static void build_status(WCHAR *buf, int cch)
{
    BOOL drop = input_left_button_held();
    WCHAR tmp[16];
    WCHAR hk[64];

    (void)cch;
    buf[0] = 0;
    format_hotkey(&g_cfg.hint_hotkey, hk, 64);

    if (g_mode == HINT_MODE_ELEMENTS) {
        if (g_scanning) {
            lstrcatW(buf, L"Finding clickable elements " DOT L" ");
            lstrcatW(buf, hk);
            lstrcatW(buf, L" again: grid " DOT L" Esc cancel");
            return;
        }
        if (drop) lstrcatW(buf, L"Drop target: ");
        if (g_prefix_len == 0) {
            lstrcatW(buf, L"Type a label " DOT L" ");
            lstrcatW(buf, hk);
            lstrcatW(buf, L" again: grid " DOT L" Esc cancel");
        } else {
            lstrcatW(buf, g_prefix);
            lstrcatW(buf, L"_ " DOT L" Backspace undo");
            if (!drop)
                lstrcatW(buf, L" " DOT L" Shift: right " DOT L" Ctrl: double");
        }
        return;
    }

    if (drop) lstrcatW(buf, L"Drop target: ");
    if (g_notice[0]) {
        lstrcatW(buf, g_notice);
        lstrcatW(buf, L" " DOT L" ");
    }

    if (g_state == 0) {
        lstrcatW(buf, L"Type a label " DOT L" Esc cancel "
                      DOT L" Tab next screen");
    } else if (g_state == 1) {
        wsprintfW(tmp, L"%c_", (WCHAR)g_cfg.alphabet[g_row]);
        lstrcatW(buf, tmp);
        lstrcatW(buf, L" " DOT L" Backspace undo " DOT L" Esc cancel");
    } else {
        wsprintfW(tmp, L"%c%c", (WCHAR)g_cfg.alphabet[g_row],
                  (WCHAR)g_cfg.alphabet[g_col]);
        lstrcatW(buf, tmp);
        if (drop) {
            lstrcatW(buf, L" " DOT L" subgrid key: move there "
                          DOT L" Enter: centre " DOT L" Space: move only");
        } else {
            lstrcatW(buf, L" " DOT L" subgrid key: click there "
                          DOT L" Enter: centre " DOT L" Space: move only "
                          DOT L" Shift: right " DOT L" Ctrl: double");
        }
    }
}

static void draw_status(HDC dc, int w, int h)
{
    WCHAR text[1024];
    HGDIOBJ old_font, old_br, old_pen;
    SIZE sz;
    int padx, pady, boxw, boxh, maxw, gap, bottom, top, left, rad;
    RECT br, tr;

    build_status(text, (int)(sizeof(text) / sizeof(text[0])));

    old_font = SelectObject(dc, g_font_status ? g_font_status
                                              : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    if (!GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz)) {
        SelectObject(dc, old_font);
        return;
    }

    padx = MulDiv(14, g_dpi, 96);
    pady = MulDiv(6, g_dpi, 96);
    if (padx < 4) padx = 4;
    if (pady < 2) pady = 2;

    boxw = sz.cx + 2 * padx;
    boxh = sz.cy + 2 * pady;
    maxw = w - MulDiv(8, g_dpi, 96);
    if (maxw < 20) maxw = w;
    if (boxw > maxw) boxw = maxw;
    if (boxw < 1) boxw = 1;

    gap = MulDiv(24, g_dpi, 96);
    left = (w - boxw) / 2;
    if (left < 0) left = 0;

    if (g_mode == HINT_MODE_ELEMENTS) {
        bottom = h - gap;
        top = bottom - boxh;
        if (top < 0) {
            top = 0;
            bottom = boxh;
        }
        br.left = left;
        br.top = top;
        br.right = left + boxw;
        br.bottom = bottom;
        if (element_status_overlaps(br)) {
            top = gap;
            if (top < 0) top = 0;
            bottom = top + boxh;
            if (bottom > h) {
                bottom = h;
                top = h - boxh;
                if (top < 0) top = 0;
            }
            br.top = top;
            br.bottom = bottom;
        }
    } else {
        bottom = h - gap;
        top = bottom - boxh;
        if (g_rows >= 2) {
            // Sit in the gap between two label rows so no label is covered:
            // the last two rows, or the first two when the chosen cell is low.
            RECT row_rc;
            int boundary = g_rows - 1;
            if (g_state == 2 && g_row >= g_rows - 2) boundary = 1;
            cell_rect(boundary, 0, &row_rc);
            top = row_rc.top - boxh / 2;
            bottom = top + boxh;
        }
        if (top < 0) {
            top = 0;
            bottom = boxh;
        }
        if (bottom > h) {
            bottom = h;
            top = h - boxh;
            if (top < 0) top = 0;
        }

        br.left = left;
        br.top = top;
        br.right = left + boxw;
        br.bottom = bottom;
    }

    old_br = SelectObject(dc, g_flash ? g_br_status_red : g_br_status);
    old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    rad = (br.bottom - br.top) / 2;
    if (rad < 1) rad = 1;
    RoundRect(dc, br.left, br.top, br.right, br.bottom, rad, rad);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_br);

    SetTextColor(dc, RGB(255, 255, 255));
    tr = br;
    tr.left += padx;
    tr.right -= padx;
    if (tr.right < tr.left) tr.right = tr.left;
    DrawTextW(dc, text, -1, &tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(dc, old_font);
}

static void draw_outline(HDC dc, RECT cell)
{
    HGDIOBJ old_br, old_pen;

    old_br = SelectObject(dc, GetStockObject(NULL_BRUSH));
    old_pen = SelectObject(dc, g_pen_sel);
    Rectangle(dc, cell.left + 1, cell.top + 1,
              cell.right - 1, cell.bottom - 1);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_br);
}

static void draw_subgrid_lines(HDC dc, RECT cell)
{
    HGDIOBJ old_pen = SelectObject(dc, g_pen_border);
    RECT sub;
    int i;

    for (i = 1; i < g_sub_cols; ++i) {
        subcell_rect(cell, 0, i, &sub);
        MoveToEx(dc, sub.left, cell.top, NULL);
        LineTo(dc, sub.left, cell.bottom - 1);
    }
    for (i = 1; i < g_sub_rows; ++i) {
        subcell_rect(cell, i, 0, &sub);
        MoveToEx(dc, cell.left, sub.top, NULL);
        LineTo(dc, cell.right - 1, sub.top);
    }
    SelectObject(dc, old_pen);
}

static void paint_overlay(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc, mem, dc;
    HBITMAP bmp = NULL;
    HGDIOBJ old_bmp = NULL;
    RECT rc;
    int w, h, r, c;

    hdc = BeginPaint(hwnd, &ps);
    GetClientRect(hwnd, &rc);
    w = rc.right;
    h = rc.bottom;

    mem = CreateCompatibleDC(hdc);
    dc = hdc;
    if (mem) {
        bmp = CreateCompatibleBitmap(hdc, w, h);
        if (bmp) {
            old_bmp = SelectObject(mem, bmp);
            dc = mem;
        } else {
            DeleteDC(mem);
            mem = NULL;
        }
    }

    FillRect(dc, &rc, g_br_key ? g_br_key
                               : (HBRUSH)GetStockObject(BLACK_BRUSH));

    if (g_open && g_font_label && g_font_sub && g_mode == HINT_MODE_ELEMENTS) {
        int i;

        if (!g_scanning) {
            for (i = 0; i < g_target_count; ++i) {
                RECT trc;
                if (!element_matches(i)) continue;
                draw_element_label(dc, &g_targets[i], g_labels[i].s,
                                   (int)wcslen(g_labels[i].s), g_prefix_len);
                if (g_prefix_len > 0) {
                    trc.left = g_targets[i].rc.left - g_mon_rect.left;
                    trc.top = g_targets[i].rc.top - g_mon_rect.top;
                    trc.right = g_targets[i].rc.right - g_mon_rect.left;
                    trc.bottom = g_targets[i].rc.bottom - g_mon_rect.top;
                    draw_target_outline(dc, trc);
                }
            }
        }
        draw_status(dc, w, h);
    } else if (g_open && g_font_label && g_font_sub) {
        WCHAR text[2];

        if (g_state == 0) {
            for (r = 0; r < g_rows; ++r) {
                text[0] = g_cfg.alphabet[r];
                for (c = 0; c < g_cols; ++c) {
                    RECT cell;
                    text[1] = g_cfg.alphabet[c];
                    cell_rect(r, c, &cell);
                    draw_label_box(dc, cell, text, 2, 0, 0,
                                   g_font_label, g_font_label_px, FALSE);
                }
            }
        } else if (g_state == 1) {
            RECT cell;
            text[0] = g_cfg.alphabet[g_row];
            for (c = 0; c < g_cols; ++c) {
                text[1] = g_cfg.alphabet[c];
                cell_rect(g_row, c, &cell);
                draw_label_box(dc, cell, text, 2, 1, g_match,
                               g_font_label, g_font_label_px, FALSE);
            }
        } else if (g_state == 2) {
            RECT cell, sub;
            int sr, sc;

            // Thin dividers and corner labels keep the content under the
            // cell (and each subcell's centre, where the click lands) visible.
            cell_rect(g_row, g_col, &cell);
            draw_subgrid_lines(dc, cell);
            for (sr = 0; sr < g_sub_rows; ++sr) {
                for (sc = 0; sc < g_sub_cols; ++sc) {
                    WCHAR ch = g_sub[sr][sc];
                    subcell_rect(cell, sr, sc, &sub);
                    draw_label_box(dc, sub, &ch, 1, 0, 0,
                                   g_font_sub, g_font_sub_px, TRUE);
                }
            }
            draw_outline(dc, cell);
        }
        draw_status(dc, w, h);
    }

    if (mem) {
        BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        if (old_bmp) SelectObject(mem, old_bmp);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);

    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------
// window

static LRESULT CALLBACK hint_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_overlay(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_FLASH_ID) {
            KillTimer(hwnd, TIMER_FLASH_ID);
            g_flash = FALSE;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wp == TIMER_SCAN_ID) {
            KillTimer(hwnd, TIMER_SCAN_ID);
            if (g_scanning) finalize_scan();
        }
        return 0;
    case WM_DISPLAYCHANGE:
        if (g_open) hint_close();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// keys / actions

static int vk_to_char(UINT vk)
{
    UINT ch;

    if (vk >= 'A' && vk <= 'Z') return (int)(L'a' + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return (int)vk;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return (int)(L'0' + (vk - VK_NUMPAD0));

    ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR);
    ch &= 0x7FFFFFFFu;  // clear the dead-key high bit
    if (ch == 0 || ch > 0xFFFFu) return 0;

    return (int)lower_ascii((WCHAR)ch);
}

static int alphabet_index(WCHAR ch)
{
    WCHAR lc = lower_ascii(ch);
    int i;

    for (i = 0; i < g_n; ++i)
        if (lower_ascii(g_cfg.alphabet[i]) == lc) return i;
    return -1;
}

static BOOL find_subkey(WCHAR ch, int *sr, int *sc)
{
    WCHAR lc = lower_ascii(ch);
    int r, c;

    for (r = 0; r < g_sub_rows; ++r) {
        for (c = 0; c < g_sub_cols; ++c) {
            if (g_sub[r][c] == lc) {
                *sr = r;
                *sc = c;
                return TRUE;
            }
        }
    }
    return FALSE;
}

static void invalidate(void)
{
    if (g_hint_wnd) InvalidateRect(g_hint_wnd, NULL, FALSE);
}

static void flash_invalid(void)
{
    g_flash = TRUE;
    if (g_hint_wnd) {
        SetTimer(g_hint_wnd, TIMER_FLASH_ID, 180, NULL);
        InvalidateRect(g_hint_wnd, NULL, FALSE);
    }
}

static void act_at(POINT pt, UINT mods)
{
    // The overlay and capture must be gone before any synthetic input.
    hint_close();

    if (input_left_button_held()) {
        mouse_move_to(pt);
        return;
    }
    if (mods & KMOD_SHIFT)
        mouse_click_at(pt, BTN_RIGHT, 1);
    else if (mods & KMOD_CTRL)
        mouse_click_at(pt, BTN_LEFT, 2);
    else
        mouse_click_at(pt, BTN_LEFT, 1);
}

static void reset_input(void)
{
    g_state = 0;
    g_row = -1;
    g_col = -1;
    g_flash = FALSE;
    g_prefix_len = 0;
    g_prefix[0] = 0;
    g_queue_count = 0;
    g_notice[0] = 0;
    if (g_hint_wnd) KillTimer(g_hint_wnd, TIMER_FLASH_ID);
}

static void cancel_scan(void)
{
    if (g_hint_wnd) KillTimer(g_hint_wnd, TIMER_SCAN_ID);
    if (g_scanning) {
        g_scanning = FALSE;
        uia_cancel();
    }
    if (g_scan_list) {
        free(g_scan_list);
        g_scan_list = NULL;
    }
}

static void start_scan(void)
{
    UINT ms;

    reset_input();
    free_elements();
    if (g_scan_list) {
        free(g_scan_list);
        g_scan_list = NULL;
    }
    g_scanning = TRUE;
    ++g_scan_id;
    uia_scan(g_mon, g_scan_id);
    if (g_hint_wnd) {
        ms = g_cfg.scan_timeout > 0 ? (UINT)g_cfg.scan_timeout : 1000u;
        SetTimer(g_hint_wnd, TIMER_SCAN_ID, ms, NULL);
    }
    invalidate();
    if (g_hint_wnd) UpdateWindow(g_hint_wnd);
}

static void enter_grid_mode(const WCHAR *notice)
{
    cancel_scan();
    free_elements();
    g_mode = HINT_MODE_GRID;
    reset_input();
    if (notice && *notice) {
        wcsncpy(g_notice, notice, sizeof(g_notice) / sizeof(g_notice[0]) - 1);
        g_notice[sizeof(g_notice) / sizeof(g_notice[0]) - 1] = 0;
    }
    invalidate();
    if (g_hint_wnd) UpdateWindow(g_hint_wnd);
}

static void start_element_mode(void)
{
    cancel_scan();
    free_elements();
    g_mode = HINT_MODE_ELEMENTS;
    start_scan();
}

static void replay_queue(void)
{
    QueuedKey keys[MAX_QUEUED_KEYS];
    int n = g_queue_count, i;

    if (n <= 0) return;
    memcpy(keys, g_queue, sizeof(keys[0]) * (size_t)n);
    g_queue_count = 0;
    for (i = 0; i < n && g_open; ++i)
        handle_key(keys[i].vk, keys[i].mods);
}

static void finalize_scan(void)
{
    ClickList *list = g_scan_list;
    HintLabel *labels;
    int n;

    g_scan_list = NULL;
    g_scanning = FALSE;
    if (g_hint_wnd) KillTimer(g_hint_wnd, TIMER_SCAN_ID);
    uia_cancel();

    n = list ? list->count : 0;
    if (n > MAX_TARGETS) n = MAX_TARGETS;
    if (n <= 0) {
        free(list);
        enter_grid_mode(L"No clickable elements found - grid");
        return;
    }

    g_targets = (ClickTarget *)malloc((size_t)n * sizeof(*g_targets));
    if (!g_targets) {
        free(list);
        enter_grid_mode(L"No clickable elements found - grid");
        return;
    }
    memcpy(g_targets, list->items, (size_t)n * sizeof(*g_targets));
    g_target_count = n;
    free(list);

    qsort(g_targets, (size_t)n, sizeof(*g_targets), target_cmp);

    labels = build_labels(n);
    if (!labels) {
        free_elements();
        enter_grid_mode(L"No clickable elements found - grid");
        return;
    }
    g_labels = labels;

    g_prefix_len = 0;
    g_prefix[0] = 0;
    g_flash = FALSE;
    g_state = 0;
    g_notice[0] = 0;
    invalidate();
    if (g_hint_wnd) UpdateWindow(g_hint_wnd);

    replay_queue();
}

static void element_append(int ch, UINT mods)
{
    int idx;

    if (g_prefix_len >= (int)sizeof(g_prefix) - 2) {
        flash_invalid();
        return;
    }
    g_prefix[g_prefix_len++] = (WCHAR)ch;
    g_prefix[g_prefix_len] = 0;

    idx = exact_match();
    if (idx >= 0) {
        act_at(g_targets[idx].click, mods);
        return;
    }
    if (count_matches() == 0) {
        --g_prefix_len;
        g_prefix[g_prefix_len] = 0;
        flash_invalid();
        return;
    }
    g_flash = FALSE;
    invalidate();
}

static void element_key(UINT vk, UINT mods)
{
    int ch, idx;

    if (vk == VK_ESCAPE) {
        hint_close();
        return;
    }
    if (vk == VK_TAB) {
        cycle_monitor(mods);
        return;
    }
    if (vk == VK_BACK) {
        if (g_prefix_len > 0) {
            --g_prefix_len;
            g_prefix[g_prefix_len] = 0;
        }
        g_flash = FALSE;
        invalidate();
        return;
    }
    if (vk == VK_RETURN) {
        idx = only_match();
        if (idx >= 0) act_at(g_targets[idx].click, mods);
        else flash_invalid();
        return;
    }
    if (vk == VK_SPACE) {
        idx = only_match();
        if (idx >= 0) {
            POINT pt = g_targets[idx].click;
            hint_close();
            mouse_move_to(pt);
        } else {
            flash_invalid();
        }
        return;
    }

    ch = vk_to_char(vk);
    if (ch == 0) return;
    element_append(ch, mods);
}

static BOOL CALLBACK enum_monitor_proc(HMONITOR mon, HDC dc,
                                       LPRECT rect, LPARAM data)
{
    (void)dc;
    (void)data;
    if (g_mon_count < MAX_MONITORS) {
        g_mons[g_mon_count].mon = mon;
        g_mons[g_mon_count].rect = *rect;
        ++g_mon_count;
    }
    return TRUE;
}

static void sort_monitors(void)
{
    int i, j;

    for (i = 1; i < g_mon_count; ++i) {
        MonInfo key = g_mons[i];
        j = i - 1;
        while (j >= 0 &&
               (g_mons[j].rect.left > key.rect.left ||
                (g_mons[j].rect.left == key.rect.left &&
                 g_mons[j].rect.top > key.rect.top))) {
            g_mons[j + 1] = g_mons[j];
            --j;
        }
        g_mons[j + 1] = key;
    }
}

static BOOL show_on_monitor(void)
{
    if (!g_hint_wnd) return FALSE;
    return SetWindowPos(g_hint_wnd, HWND_TOPMOST,
                        g_mon_rect.left, g_mon_rect.top,
                        g_mon_w, g_mon_h,
                        SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE;
}

static void cycle_monitor(UINT mods)
{
    int i, idx = -1, next, step;
    HMONITOR target;

    g_mon_count = 0;
    EnumDisplayMonitors(NULL, NULL, enum_monitor_proc, 0);
    if (g_mon_count <= 0) return;
    sort_monitors();

    for (i = 0; i < g_mon_count; ++i) {
        if (g_mons[i].mon == g_mon) {
            idx = i;
            break;
        }
    }
    if (idx < 0) idx = 0;

    step = (mods & KMOD_SHIFT) ? -1 : 1;
    next = idx + step;
    if (next < 0) next = g_mon_count - 1;
    if (next >= g_mon_count) next = 0;
    target = g_mons[next].mon;

    if (g_mode == HINT_MODE_ELEMENTS) {
        cancel_scan();
        free_elements();
    }
    if (!compute_geometry(target)) {
        hint_close();
        return;
    }

    reset_input();

    destroy_resources();
    if (!create_resources()) {
        hint_close();
        return;
    }
    if (!show_on_monitor()) {
        hint_close();
        return;
    }
    SetLayeredWindowAttributes(g_hint_wnd, KEY_COLOR, g_cfg.opacity,
                               LWA_COLORKEY | LWA_ALPHA);
    InvalidateRect(g_hint_wnd, NULL, TRUE);
    UpdateWindow(g_hint_wnd);

    if (g_mode == HINT_MODE_ELEMENTS) start_scan();
}

// ---------------------------------------------------------------------------
// public API

void hint_init(HINSTANCE inst)
{
    WNDCLASSEXW wc;
    DWORD exstyle;

    if (g_hint_wnd) return;
    g_hint_inst = inst;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = hint_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = HINT_CLASS;

    if (!RegisterClassExW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        app_log(L"hint: RegisterClassExW failed (%lu)", GetLastError());
        return;
    }

    exstyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
              WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    g_hint_wnd = CreateWindowExW(exstyle, HINT_CLASS, L"", WS_POPUP,
                                 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_hint_wnd) {
        app_log(L"hint: CreateWindowExW failed (%lu)", GetLastError());
        return;
    }

    SetLayeredWindowAttributes(g_hint_wnd, KEY_COLOR, g_cfg.opacity,
                               LWA_COLORKEY | LWA_ALPHA);
}

void hint_shutdown(void)
{
    if (g_hint_wnd) {
        if (g_open) hint_close();
        DestroyWindow(g_hint_wnd);
        g_hint_wnd = NULL;
        UnregisterClassW(HINT_CLASS, g_hint_inst);
    }
    cancel_scan();
    free_elements();
    reset_input();
    g_open = FALSE;
    destroy_resources();
}

static BOOL open_overlay(HMONITOR mon, int mode)
{
    if (!compute_geometry(mon))
        return FALSE;

    reset_input();
    free_elements();
    g_mode = mode;
    g_start_mode = mode;
    // Paint the scanning status straight away; start_scan() follows.
    g_scanning = (mode == HINT_MODE_ELEMENTS);

    if (!create_resources()) {
        g_scanning = FALSE;
        return FALSE;
    }

    SetLayeredWindowAttributes(g_hint_wnd, KEY_COLOR, g_cfg.opacity,
                               LWA_COLORKEY | LWA_ALPHA);

    g_open = TRUE;
    if (!show_on_monitor()) {
        g_open = FALSE;
        g_scanning = FALSE;
        ShowWindow(g_hint_wnd, SW_HIDE);
        destroy_resources();
        return FALSE;
    }

    InvalidateRect(g_hint_wnd, NULL, TRUE);
    UpdateWindow(g_hint_wnd);
    return TRUE;
}

void hint_open(void)
{
    POINT cur;
    POINT zero = { 0, 0 };
    HMONITOR mon;
    int mode;

    if (g_open) return;

    input_set_key_capture(TRUE);

    if (!g_hint_wnd) {
        input_set_key_capture(FALSE);
        return;
    }

    if (GetCursorPos(&cur))
        mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    else
        mon = MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY);
    if (!mon)
        mon = MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY);

    mode = (g_cfg.hint_mode == HINT_MODE_ELEMENTS && uia_available())
           ? HINT_MODE_ELEMENTS : HINT_MODE_GRID;

    if (!open_overlay(mon, mode)) {
        g_open = FALSE;
        input_set_key_capture(FALSE);
        return;
    }

    if (mode == HINT_MODE_ELEMENTS)
        start_scan();
}

void hint_close(void)
{
    if (!g_open) return;

    if (g_hint_wnd) {
        KillTimer(g_hint_wnd, TIMER_FLASH_ID);
        KillTimer(g_hint_wnd, TIMER_SCAN_ID);
        ShowWindow(g_hint_wnd, SW_HIDE);
    }

    cancel_scan();
    uia_cancel();
    free_elements();
    reset_input();
    g_open = FALSE;
    destroy_resources();
    input_set_key_capture(FALSE);
}

BOOL hint_is_open(void)
{
    return g_open;
}

static void grid_key(UINT vk, UINT mods)
{
    int ch, idx;

    if (vk == VK_ESCAPE) {
        hint_close();
        return;
    }
    if (vk == VK_TAB) {
        cycle_monitor(mods);
        return;
    }
    if (vk == VK_BACK) {
        if (g_state == 2) {
            g_state = 1;
            g_col = -1;
            g_flash = FALSE;
            invalidate();
        } else if (g_state == 1) {
            g_state = 0;
            g_row = -1;
            g_flash = FALSE;
            invalidate();
        }
        return;
    }
    if (vk == VK_RETURN) {
        if (g_state == 2) {
            act_at(cell_center(g_row, g_col), mods);
        } else {
            flash_invalid();
        }
        return;
    }
    if (vk == VK_SPACE) {
        if (g_state == 2) {
            POINT pt = cell_center(g_row, g_col);
            hint_close();
            mouse_move_to(pt);
        } else {
            flash_invalid();
        }
        return;
    }

    ch = vk_to_char(vk);
    if (ch == 0) return;  // unknown non-character key: ignore silently

    idx = alphabet_index((WCHAR)ch);
    if (g_state == 0) {
        if (idx < 0 || idx >= g_rows) {
            flash_invalid();
            return;
        }
        g_row = idx;
        g_col = -1;
        g_state = 1;
        g_flash = FALSE;
        invalidate();
    } else if (g_state == 1) {
        if (idx < 0 || idx >= g_cols) {
            flash_invalid();
            return;
        }
        g_col = idx;
        if (g_cfg.subgrid == 0) {
            act_at(cell_center(g_row, g_col), mods);
        } else {
            g_state = 2;
            g_flash = FALSE;
            mouse_move_to(cell_center(g_row, g_col));
            invalidate();
        }
    } else {
        int sr = 0, sc = 0;
        RECT cell;

        if (!find_subkey((WCHAR)ch, &sr, &sc)) {
            flash_invalid();
            return;
        }
        cell_rect(g_row, g_col, &cell);
        act_at(subcell_center(cell, sr, sc), mods);
    }
}

static void handle_key(UINT vk, UINT mods)
{
    if (!g_open) return;
    if (g_mode == HINT_MODE_ELEMENTS)
        element_key(vk, mods);
    else
        grid_key(vk, mods);
}

void hint_on_key(UINT vk, UINT mods)
{
    if (!g_open) return;

    if (g_scanning) {
        if (vk == VK_ESCAPE) {
            cancel_scan();
            hint_close();
            return;
        }
        if (g_queue_count < MAX_QUEUED_KEYS) {
            g_queue[g_queue_count].vk = vk;
            g_queue[g_queue_count].mods = mods;
            ++g_queue_count;
        }
        return;
    }

    handle_key(vk, mods);
}

void hint_on_elements(ClickList *list)
{
    if (!list) return;

    if (!g_open || !g_scanning || list->scan_id != g_scan_id) {
        free(list);
        return;
    }

    if (g_scan_list) free(g_scan_list);
    g_scan_list = list;

    if (list->done) finalize_scan();
}

void hint_hotkey_again(void)
{
    if (!g_open) return;

    // First press switches to the other mode; a later press closes.
    if (g_mode != g_start_mode) {
        hint_close();
        return;
    }
    if (g_mode == HINT_MODE_ELEMENTS) {
        enter_grid_mode(NULL);
    } else if (uia_available()) {
        start_element_mode();
    } else {
        hint_close();
    }
}
