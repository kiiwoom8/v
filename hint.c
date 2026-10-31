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
#define TIMER_REFRESH_ID 3
#define TIMER_SETTLE_ID 4
#define REFRESH_RETRY_MS 200
#define MAX_MONITORS    16
#define MAX_TARGETS     2000
#define MAX_LABEL_LEN   16
#define LABEL_QUEUE_MAX 100000
#define MAX_QUEUED_KEYS 32
#define DOT             L"\x00B7"
#define SCAN_DEFAULT_MS 400
#define SCAN_HARD_LIMIT_MS 3000
#define FONT_REF_PX     64

typedef struct {
    HMONITOR mon;
    RECT     rect;
} MonInfo;

typedef struct {
    WCHAR s[MAX_LABEL_LEN];
} HintLabel;

// A label that is no longer shown. It is never given to another element; making
// room may only turn it into a prefix of longer labels (expanded = TRUE), so a
// stale label the user still remembers can only narrow, never click.
typedef struct {
    WCHAR s[MAX_LABEL_LEN];
    BOOL  expanded;
} RetiredLabel;

typedef struct {
    UINT vk;
    UINT mods;
} QueuedKey;

// One measured width per character of one font, so labels are not measured
// with GDI again on every paint.
typedef struct {
    HFONT font;
    int   px;
    WCHAR chars[64];
    int   widths[64];
    int   count;
    int   height;
} CharWidths;

// What the cached GDI resources were built from.
typedef struct {
    BOOL     valid;
    int      dpi;
    int      base_w, base_h;
    int      font_size;
    COLORREF label_bg, label_fg, label_border, match_fg;
    WCHAR    alphabet[64];
    WCHAR    subgrid[64];
    int      sub_rows, sub_cols;
} ResKey;

static HINSTANCE g_hint_inst;
static HWND      g_hint_wnd;
static BOOL      g_open;
static int       g_mode;        // HINT_MODE_*
static int       g_start_mode;  // mode the overlay opened in (hotkey reloads it)
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
static CharWidths g_lw, g_sw;
static ResKey    g_res_key;

// Persistent back buffer (one memory DC + DIB sized to the monitor).
static HDC       g_mem_dc;
static HBITMAP   g_mem_bmp;
static HGDIOBJ   g_mem_old;
static int       g_mem_w, g_mem_h;
static BOOL      g_mem_blank;  // back buffer is known to be plain key colour

// Ready-to-show frame for the initial grid (labels + status); it only depends
// on the monitor geometry and the cached resources.
static HDC       g_grid_dc;
static HBITMAP   g_grid_bmp;
static HGDIOBJ   g_grid_old;
static int       g_grid_w, g_grid_h;
static BOOL      g_grid_valid;

static BOOL      g_scan_wait_first;  // timeout passed, waiting for the active window
static DWORD     g_scan_t0;          // scan start tick (timeout / hard limit reference)
static BOOL      g_scan_retry;       // last scan found nothing: retry after TIMER_REFRESH_ID
static BOOL      g_refresh_on;       // background scans keep the labels up to date

// Labels that were shown and then went away. They are kept for the rest of
// this open so they are never handed to a different element (see alloc_labels).
static RetiredLabel *g_retired;
static int       g_retired_count;
static int       g_retired_cap;

static BOOL      g_alloc_relabeled;  // make_room lengthened a shown label

// Early scan started when the hint hotkey's modifier goes down. g_pre_id 0 =
// none; the newest result for it is owned in g_pre_list.
static UINT      g_pre_id;
static HMONITOR  g_pre_mon;
static DWORD     g_pre_t0;
static ClickList *g_pre_list;

static MonInfo   g_mons[MAX_MONITORS];
static int       g_mon_count;
static RECT      g_last_show;
static BOOL      g_last_show_valid;

static void finalize_scan(void);
static void reset_input(void);
static void handle_key(UINT vk, UINT mods);
static void invalidate(void);
static void cycle_monitor(UINT mods);
static void clear_prescan(void);
static BOOL adopt_prescan(void);
static void clear_retired(void);
static BOOL retire_label(const WCHAR *s);
static void schedule_refresh(void);
static void start_refresh_scan(void);
static void begin_scan(void);
static void merge_scan(ClickList *list);

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
            if (r >= 8 || c >= 8) return FALSE;
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

// Measure once at a fixed reference size, scale to the cell, then verify the
// chosen size with one more measurement instead of trying every size.
static int pick_font_px(HDC dc, const WCHAR *chars, int nchars,
                        int cellw, int cellh, int start_px, BOOL two_chars)
{
    HFONT f;
    HGDIOBJ old;
    int maxw = 0, maxh = 0, i, px, tries;
    double per_w, per_h;

    if (start_px < 6) start_px = 6;
    if (nchars < 1) return 6;
    if (nchars > 64) nchars = 64;

    f = make_font(FONT_REF_PX, FW_BOLD);
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

    if (maxw < 1) maxw = 1;
    if (maxh < 1) maxh = 1;
    per_w = ((two_chars ? 2.0 : 1.0) * maxw) / FONT_REF_PX;
    per_h = (double)maxh / FONT_REF_PX;

    px = (int)((cellw - 2) / (per_w + 0.5));
    {
        int ph = (int)((cellh - 2) / (per_h + 0.5));
        if (ph < px) px = ph;
    }
    if (px > start_px) px = start_px;
    if (px < 6) px = 6;

    for (tries = 0; tries < 8; ++tries) {
        int tw, th, pad;

        f = make_font(px, FW_BOLD);
        if (!f) return 6;
        old = SelectObject(dc, f);
        maxw = maxh = 0;
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
        th = maxh;
        pad = px / 4;
        if (pad < 2) pad = 2;
        if (tw + 2 * pad <= cellw - 2 && th + 2 * pad <= cellh - 2)
            return px;
        if (px <= 6) return 6;
        --px;
    }
    return 6;
}

static void cache_font_widths(HDC dc, HFONT font, int px,
                              const WCHAR *chars, int n, CharWidths *cw)
{
    HGDIOBJ old;
    int i, maxh = 0;

    ZeroMemory(cw, sizeof(*cw));
    cw->font = font;
    cw->px = px;
    if (n < 0) n = 0;
    if (n > 64) n = 64;
    cw->count = n;

    if (!font) return;
    old = SelectObject(dc, font);
    for (i = 0; i < n; ++i) {
        SIZE sz;
        WCHAR ch = lower_ascii(chars[i]);
        cw->chars[i] = ch;
        if (GetTextExtentPoint32W(dc, &ch, 1, &sz)) {
            cw->widths[i] = sz.cx;
            if (sz.cy > maxh) maxh = sz.cy;
        }
    }
    SelectObject(dc, old);
    cw->height = maxh;
}

static int cached_char_width(const CharWidths *cw, WCHAR ch)
{
    int i;

    ch = lower_ascii(ch);
    for (i = 0; i < cw->count; ++i)
        if (cw->chars[i] == ch) return cw->widths[i];
    return 0;
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
    ZeroMemory(&g_lw, sizeof(g_lw));
    ZeroMemory(&g_sw, sizeof(g_sw));
    g_res_key.valid = FALSE;
    g_grid_valid = FALSE;
}

static void fill_res_key(ResKey *k)
{
    ZeroMemory(k, sizeof(*k));
    k->valid = TRUE;
    k->dpi = g_dpi;
    k->base_w = g_base_w;
    k->base_h = g_base_h;
    k->font_size = g_cfg.font_size;
    k->label_bg = g_cfg.label_bg;
    k->label_fg = g_cfg.label_fg;
    k->label_border = g_cfg.label_border;
    k->match_fg = g_cfg.match_fg;
    wcsncpy(k->alphabet, g_cfg.alphabet, 63);
    wcsncpy(k->subgrid, g_cfg.subgrid_keys, 63);
    k->sub_rows = g_sub_rows;
    k->sub_cols = g_sub_cols;
}

static BOOL resource_key_matches(void)
{
    ResKey k;

    if (!g_res_key.valid) return FALSE;
    fill_res_key(&k);
    return memcmp(&k, &g_res_key, sizeof(k)) == 0;
}

static BOOL create_resources(void)
{
    HDC dc;
    int min_cw, min_ch, start, alen;
    WCHAR subchars[64];
    int sn = 0, r, c, subw, subh, substart;
    ResKey key;

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

    if (!g_font_label || !g_font_sub || !g_font_status) {
        ReleaseDC(NULL, dc);
        destroy_resources();
        return FALSE;
    }

    cache_font_widths(dc, g_font_label, g_font_label_px,
                      g_cfg.alphabet, alen, &g_lw);
    cache_font_widths(dc, g_font_sub, g_font_sub_px, subchars, sn, &g_sw);
    ReleaseDC(NULL, dc);

    fill_res_key(&key);
    g_res_key = key;
    return TRUE;
}

static BOOL ensure_resources(void)
{
    if (resource_key_matches()) return TRUE;
    return create_resources();
}

// ---------------------------------------------------------------------------
// drawing

static void measure_label(const CharWidths *cw, const WCHAR *text, int len,
                          int *w, int *tw, int *th)
{
    int i;

    *tw = 0;
    *th = cw ? cw->height : 0;
    for (i = 0; i < len && i < MAX_LABEL_LEN; ++i) {
        w[i] = cw ? cached_char_width(cw, text[i]) : 0;
        *tw += w[i];
    }
}

// Draws the box and text already positioned at left/top. The label brush and
// border pen are selected by the caller for the whole label pass. Typed chars
// at the start of the label use split_col (the typed prefix).
static void draw_box_text(HDC dc, int left, int top, int bw, int bh,
                          const WCHAR *text, int len, int typed,
                          COLORREF split_col, const int *wchars, int tw, int th)
{
    int i, rad, x, y;

    rad = MulDiv(4, g_dpi, 96);
    if (rad < 2) rad = 2;
    if (rad > bh / 2) rad = bh / 2;
    if (rad < 1) rad = 1;

    RoundRect(dc, left, top, left + bw, top + bh, rad, rad);

    x = left + (bw - tw) / 2;
    y = top + (bh - th) / 2;
    if (typed <= 0) {
        for (i = 0; i < len && i < MAX_LABEL_LEN; ++i) {
            TextOutW(dc, x, y, &text[i], 1);
            x += wchars[i];
        }
        return;
    }
    for (i = 0; i < len && i < MAX_LABEL_LEN; ) {
        COLORREF col = i < typed ? split_col : g_fg;
        int j = i;

        while (j < len && (j < typed ? split_col : g_fg) == col) ++j;
        SetTextColor(dc, col);
        for (; i < j; ++i) {
            TextOutW(dc, x, y, &text[i], 1);
            x += wchars[i];
        }
    }
}

typedef struct {
    HGDIOBJ old_font, old_br, old_pen;
} LabelPass;

static void labels_begin(HDC dc, HFONT font, LabelPass *pass)
{
    pass->old_font = SelectObject(dc, font ? font
                                           : GetStockObject(DEFAULT_GUI_FONT));
    pass->old_br = SelectObject(dc, g_br_label);
    pass->old_pen = SelectObject(dc, g_pen_border);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, g_fg);
}

static void labels_end(HDC dc, LabelPass *pass)
{
    SelectObject(dc, pass->old_pen);
    SelectObject(dc, pass->old_br);
    SelectObject(dc, pass->old_font);
}

// corner: pin the box to the top-left of the cell instead of centring it
static void draw_label_box(HDC dc, RECT cell, const WCHAR *text, int len,
                           int typed, COLORREF split_col,
                           int font_px, BOOL corner, const CharWidths *cw)
{
    int w[MAX_LABEL_LEN];
    int tw, th, pad, bw, bh, cw_px, ch, cx, cy, left, top;

    if (len <= 0) return;
    if (len > MAX_LABEL_LEN) len = MAX_LABEL_LEN;

    measure_label(cw, text, len, w, &tw, &th);

    pad = font_px / 4;
    if (pad < 2) pad = 2;
    bw = tw + 2 * pad;
    bh = th + 2 * pad;

    cw_px = cell.right - cell.left;
    ch = cell.bottom - cell.top;
    if (bw > cw_px) bw = cw_px;
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
                  w, tw, th);
}

// A label pinned at the target's top-left, nudged inside the monitor.
static void draw_element_label(HDC dc, const ClickTarget *t,
                               const WCHAR *text, int len, int typed)
{
    int w[MAX_LABEL_LEN];
    int tw, th, pad, bw, bh, left, top;

    if (len <= 0) return;
    if (len > MAX_LABEL_LEN) len = MAX_LABEL_LEN;

    measure_label(&g_lw, text, len, w, &tw, &th);

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
                  w, tw, th);
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
    clear_retired();
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
// With keep_labels a shown label can never be lengthened, so an element that
// appears while the hints are open (the panel a click opened, a menu that
// slides in, a page that catches up) can only get a code nobody holds. Plan
// the codes for a quarter more elements, and at least an alphabet's worth,
// and leave the extra ones free for them.
static HintLabel *build_labels(int n)
{
    int alen = (int)wcslen(g_cfg.alphabet);
    int want, cap, count, offset, i, j;
    HintLabel *pool, *out;

    if (n <= 0 || alen < 1) return NULL;
    if (alen > 60) alen = 60;
    want = n;
    if (g_cfg.keep_labels)
        want += n / 4 > alen ? n / 4 : alen;
    cap = 2 * want + 2 * alen + 16;
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
    while (count - offset < want || count == 1) {
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

// ---------------------------------------------------------------------------
// Label retirement / merge allocation
//
// A label that has been on screen is never given to another element, even
// after that element goes away: the user may still remember and type it. Such
// labels go to g_retired and keep blocking their whole subtree, so a stale
// label can only narrow the remaining matches. When room is needed, a retired
// label may be "expanded" into longer labels (the stale shorter label then
// only narrows), never reused as a whole.

static void clear_retired(void)
{
    free(g_retired);
    g_retired = NULL;
    g_retired_count = 0;
    g_retired_cap = 0;
}

static BOOL retire_label(const WCHAR *s)
{
    RetiredLabel *p;

    if (g_retired_count >= g_retired_cap) {
        int cap = g_retired_cap ? g_retired_cap * 2 : 64;
        p = (RetiredLabel *)realloc(g_retired, (size_t)cap * sizeof(*p));
        if (!p) return FALSE;
        g_retired = p;
        g_retired_cap = cap;
    }
    p = &g_retired[g_retired_count++];
    p->expanded = FALSE;
    wcsncpy(p->s, s, MAX_LABEL_LEN - 1);
    p->s[MAX_LABEL_LEN - 1] = 0;
    return TRUE;
}

// 1 = a shown label, 2 = a retired label, 3 = a retired label opened up with
// longer labels; 0 = free.
static int occupied_equal(const WCHAR *s)
{
    int i;

    for (i = 0; i < g_target_count; ++i)
        if (wcscmp(g_labels[i].s, s) == 0) return 1;
    for (i = 0; i < g_retired_count; ++i)
        if (wcscmp(g_retired[i].s, s) == 0)
            return g_retired[i].expanded ? 3 : 2;
    return 0;
}

// TRUE when an occupied label strictly extends s, so s cannot be used as a
// whole (a shorter candidate is always a prefix of a live label); the caller
// expands it instead. Descending into an equal label is never done, which is
// what keeps a new label from extending a live one.
static BOOL occupied_extends(const WCHAR *s)
{
    size_t len = wcslen(s);
    int i;

    for (i = 0; i < g_target_count; ++i) {
        if (wcslen(g_labels[i].s) > len &&
            label_has_prefix(g_labels[i].s, s)) return TRUE;
    }
    for (i = 0; i < g_retired_count; ++i) {
        if (wcslen(g_retired[i].s) > len &&
            label_has_prefix(g_retired[i].s, s)) return TRUE;
    }
    return FALSE;
}

// Shortlex BFS over the alphabet; returns how many of out[0..need) got a label,
// or -1 when out of memory. cap bounds the queue (it grows on the next try).
static int alloc_pass(int need, HintLabel *out, int alen, int cap)
{
    HintLabel *queue;
    int head = 0, tail = 0, got = 0, j;

    if (cap < alen + 1) cap = alen + 1;
    queue = (HintLabel *)malloc((size_t)cap * sizeof(*queue));
    if (!queue) return -1;

    for (j = 0; j < alen && tail < cap; ++j) {
        queue[tail].s[0] = lower_ascii(g_cfg.alphabet[j]);
        queue[tail].s[1] = 0;
        ++tail;
    }

    while (head < tail && got < need) {
        WCHAR cur[MAX_LABEL_LEN];
        int len, eq;

        wcscpy(cur, queue[head].s);
        ++head;
        len = (int)wcslen(cur);
        eq = occupied_equal(cur);

        if (eq == 1 || eq == 2) continue;      // a real label: never reuse
        if (eq == 3 || occupied_extends(cur)) {
            // A live label is under here: only its longer labels may be used.
            if (len >= MAX_LABEL_LEN - 1) continue;
            for (j = 0; j < alen && tail < cap; ++j) {
                wcscpy(queue[tail].s, cur);
                queue[tail].s[len] = lower_ascii(g_cfg.alphabet[j]);
                queue[tail].s[len + 1] = 0;
                ++tail;
            }
            continue;
        }
        wcscpy(out[got].s, cur);
        ++got;
    }

    free(queue);
    return got;
}

static int alphabet_len(void)
{
    int alen = (int)wcslen(g_cfg.alphabet);
    if (alen > 60) alen = 60;
    return alen > 0 ? alen : 0;
}

// Frees room when the code is full: open up a retired label first (its longer
// labels become candidates, the stale one only narrows), else make the
// shortest shown label one character longer (typing the old one narrows).
// With may_lengthen = FALSE (keep_labels), only the first branch may run: a
// label the user can see must keep its letters.
static BOOL make_room(BOOL may_lengthen)
{
    int alen = alphabet_len();
    int i, j, best = -1, bestlen = 0, len;

    if (alen < 2) return FALSE;   // one letter cannot be split any further
    for (i = 0; i < g_retired_count; ++i) {
        if (g_retired[i].expanded) continue;
        len = (int)wcslen(g_retired[i].s);
        if (len >= MAX_LABEL_LEN - 1) continue;
        if (best < 0 || len < bestlen) {
            best = i;
            bestlen = len;
        }
    }
    if (best >= 0) {
        g_retired[best].expanded = TRUE;
        return TRUE;
    }
    if (!may_lengthen) return FALSE;

    // Find the shortest shown label with a free extension, then split it.
    // Never pick an extension that is itself taken, or the stale label would
    // point at two elements.
    for (i = 0; i < g_target_count; ++i) {
        len = (int)wcslen(g_labels[i].s);
        if (len >= MAX_LABEL_LEN - 1) continue;
        if (best < 0 || len < bestlen) {
            WCHAR probe[MAX_LABEL_LEN];
            int hit = -1;
            for (j = 0; j < alen; ++j) {
                wcscpy(probe, g_labels[i].s);
                probe[len] = lower_ascii(g_cfg.alphabet[j]);
                probe[len + 1] = 0;
                if (occupied_equal(probe) == 0 && !occupied_extends(probe)) {
                    hit = j;
                    break;
                }
            }
            if (hit < 0) continue;
            best = i;
            bestlen = len;
        }
    }
    if (best < 0) return FALSE;

    // Keep the old label itself unavailable (it must only narrow), then move
    // the element to L + a free alphabet character.
    {
        WCHAR probe[MAX_LABEL_LEN];
        int hit = -1;
        for (j = 0; j < alen; ++j) {
            wcscpy(probe, g_labels[best].s);
            probe[bestlen] = lower_ascii(g_cfg.alphabet[j]);
            probe[bestlen + 1] = 0;
            if (occupied_equal(probe) == 0 && !occupied_extends(probe)) {
                hit = j;
                break;
            }
        }
        if (hit < 0) return FALSE;
        if (!retire_label(g_labels[best].s)) return FALSE;
        g_retired[g_retired_count - 1].expanded = TRUE;
        g_labels[best].s[bestlen] = lower_ascii(g_cfg.alphabet[hit]);
        g_labels[best].s[bestlen + 1] = 0;
    }
    g_alloc_relabeled = TRUE;   // the caller must repaint
    return TRUE;
}

// Allocates labels a merge needs in one pass, so they stay as short as
// possible and are prefix-free against shown and retired labels alike.
// Returns how many of out[0..need) got a label, or -1 when out of memory.
// With keep_labels, a shown label may not grow: once every retired label is
// open, the allocation stops at what fits and the rest stays unlabelled.
static int alloc_labels(int need, HintLabel *out)
{
    int alen = alphabet_len();
    int tries, got = 0, limit, cap;
    BOOL lenient = g_cfg.keep_labels != 0;

    if (need <= 0) return 0;
    if (alen < 1) return -1;
    limit = need + 2048;

    for (tries = 0; tries < limit; ++tries) {
        cap = (g_target_count + g_retired_count + need + 8) * alen;
        if (cap < alen + 1) cap = alen + 1;
        if (cap > LABEL_QUEUE_MAX) cap = LABEL_QUEUE_MAX;
        got = alloc_pass(need, out, alen, cap);
        if (got < 0) return -1;
        if (got == need) return got;
        if (!make_room(!lenient)) return lenient ? got : -1;
    }
    return lenient ? got : -1;
}

static BOOL same_element(const RECT *a, const RECT *b)
{
    int tol = 2 * g_dpi / 96;
    int dl, dt, dr, db;

    if (tol < 2) tol = 2;
    dl = (int)(a->left - b->left);
    dt = (int)(a->top - b->top);
    dr = (int)(a->right - b->right);
    db = (int)(a->bottom - b->bottom);
    if (dl < 0) dl = -dl;
    if (dt < 0) dt = -dt;
    if (dr < 0) dr = -dr;
    if (db < 0) db = -db;
    return dl <= tol && dt <= tol && dr <= tol && db <= tol;
}

// Area of the intersection of two rects as long long (0 when they do not meet).
static long long rect_intersection_area(const RECT *a, const RECT *b)
{
    long long l = a->left > b->left ? a->left : b->left;
    long long t = a->top > b->top ? a->top : b->top;
    long long r = a->right < b->right ? a->right : b->right;
    long long btm = a->bottom < b->bottom ? a->bottom : b->bottom;

    if (r <= l || btm <= t) return 0;
    return (r - l) * (btm - t);
}

// TRUE when the rects overlap by at least half of their union (IoU >= 0.5):
// a new element that only shifted a few pixels is the same target, not a
// second one to label. A small button inside a large labelled card keeps a
// much smaller overlap and still gets its own label.
static BOOL overlaps_heavily(const RECT *a, const RECT *b)
{
    long long inter = rect_intersection_area(a, b);
    long long area_a, area_b, uni;

    if (inter <= 0) return FALSE;
    area_a = (long long)(a->right - a->left) * (a->bottom - a->top);
    area_b = (long long)(b->right - b->left) * (b->bottom - b->top);
    uni = area_a + area_b - inter;
    if (uni <= 0) return FALSE;
    return inter * 2 >= uni;
}

// Folds one refresh post into the shown targets. Labels of surviving elements
// are kept; removed ones are retired; new ones get labels in one allocated
// pass. Partial posts never remove anything (they have not seen every window).
// With keep_labels, a shown label is frozen for the rest of the open: it keeps
// its text, rect and click point and is never retired by a merge, so nothing
// the user is reading moves under them. An incoming element that overlaps a
// shown one is treated as the same element; the rest get free labels, as many
// as fit, without lengthening a shown label.
static void merge_scan(ClickList *list)
{
    ClickTarget *nt;
    HintLabel *nl;
    char *used, *gone;
    int incoming = list ? list->count : 0;
    int i, j, survivors = 0, need = 0, got;
    BOOL keep = g_cfg.keep_labels != 0;
    BOOL changed = FALSE;

    if (incoming > MAX_TARGETS) incoming = MAX_TARGETS;
    nt = (ClickTarget *)malloc((size_t)(g_target_count + incoming + 1) *
                               sizeof(*nt));
    nl = (HintLabel *)calloc((size_t)(g_target_count + incoming + 1),
                             sizeof(*nl));
    used = (char *)calloc((size_t)(incoming > 0 ? incoming : 1), sizeof(*used));
    gone = (char *)calloc((size_t)(g_target_count > 0 ? g_target_count : 1),
                          sizeof(*gone));
    if (!nt || !nl || !used || !gone) {
        free(nt);
        free(nl);
        free(used);
        free(gone);
        return;
    }

    // Pass 1: match, update moved elements, retire the ones that went away.
    for (i = 0; i < g_target_count; ++i) {
        int match = -1;
        for (j = 0; j < incoming; ++j) {
            if (used[j]) continue;
            if (same_element(&g_targets[i].rc, &list->items[j].rc)) {
                match = j;
                break;
            }
        }
        if (match >= 0) {
            used[match] = 1;
            // A frozen label keeps its rect and click point too: moving them
            // would move the label and the click under the user.
            if (!keep &&
                (g_targets[i].rc.left != list->items[match].rc.left ||
                 g_targets[i].rc.top != list->items[match].rc.top ||
                 g_targets[i].rc.right != list->items[match].rc.right ||
                 g_targets[i].rc.bottom != list->items[match].rc.bottom ||
                 g_targets[i].click.x != list->items[match].click.x ||
                 g_targets[i].click.y != list->items[match].click.y)) {
                g_targets[i].rc = list->items[match].rc;
                g_targets[i].click = list->items[match].click;
                changed = TRUE;
            }
        } else if (!keep && list->done && retire_label(g_labels[i].s)) {
            // A done post covered every window: the element is really gone.
            gone[i] = 1;
            changed = TRUE;
        }
        // A partial post proves nothing about the elements it did not see.
    }

    // keep_labels: an incoming element that heavily overlaps a label already
    // on screen is that element after a small shift, so it gets no second
    // label. Done against the shown rects before compaction; nothing is
    // removed in this mode.
    if (keep) {
        for (j = 0; j < incoming; ++j) {
            if (used[j]) continue;
            for (i = 0; i < g_target_count; ++i) {
                if (overlaps_heavily(&g_targets[i].rc, &list->items[j].rc)) {
                    used[j] = 1;
                    break;
                }
            }
        }
    }

    // Compact the survivors, then swap them in so the allocator only sees
    // labels that are still on screen (retired ones block separately).
    for (i = 0; i < g_target_count; ++i) {
        if (gone[i]) continue;
        nt[survivors] = g_targets[i];
        wcscpy(nl[survivors].s, g_labels[i].s);
        ++survivors;
    }
    free(g_targets);
    free(g_labels);
    g_targets = nt;
    g_labels = nl;
    g_target_count = survivors;

    // Pass 2: one allocation for every new element, in shortlex order.
    for (j = 0; j < incoming; ++j)
        if (!used[j]) ++need;
    if (need > MAX_TARGETS - g_target_count)
        need = MAX_TARGETS - g_target_count;
    if (need > 0) {
        HintLabel *allocated = (HintLabel *)calloc((size_t)need, sizeof(*allocated));
        int added = 0;

        g_alloc_relabeled = FALSE;
        got = allocated ? alloc_labels(need, allocated) : -1;
        if (got < 0) {
            // No room (or no memory): keep the survivors, retry next refresh.
            free(allocated);
            free(used);
            free(gone);
            if (changed || g_alloc_relabeled) invalidate();
            return;
        }
        if (g_alloc_relabeled) changed = TRUE;
        // With keep_labels, alloc_labels may return fewer than need: label as
        // many as fit and leave the rest for a reload by the hotkey.
        for (j = 0; j < incoming && added < got; ++j) {
            if (used[j]) continue;
            g_targets[g_target_count].rc = list->items[j].rc;
            g_targets[g_target_count].click = list->items[j].click;
            wcscpy(g_labels[g_target_count].s, allocated[added].s);
            ++g_target_count;
            ++added;
            changed = TRUE;
        }
        free(allocated);
    }

    free(used);
    free(gone);
    if (changed) invalidate();
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
            lstrcatW(buf, L" again: reload " DOT L" Esc cancel");
            return;
        }
        if (drop) lstrcatW(buf, L"Drop target: ");
        if (g_prefix_len == 0) {
            lstrcatW(buf, L"Type a label " DOT L" ");
            lstrcatW(buf, hk);
            lstrcatW(buf, L" again: reload " DOT L" Esc cancel");
        } else {
            lstrcatW(buf, g_prefix);
            lstrcatW(buf, L"_ " DOT L" Backspace undo");
            if (!drop)
                lstrcatW(buf, L" " DOT L" Shift: right " DOT L" Ctrl: double");
        }
        return;
    }

    if (drop) lstrcatW(buf, L"Drop target: ");

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

static void free_back_buffer(void)
{
    if (g_mem_dc && g_mem_old) {
        SelectObject(g_mem_dc, g_mem_old);
        g_mem_old = NULL;
    }
    if (g_mem_bmp) {
        DeleteObject(g_mem_bmp);
        g_mem_bmp = NULL;
    }
    if (g_mem_dc) {
        DeleteDC(g_mem_dc);
        g_mem_dc = NULL;
    }
    g_mem_w = g_mem_h = 0;
    g_mem_blank = FALSE;
}

static BOOL ensure_back_buffer(int w, int h)
{
    BITMAPINFO bmi;
    void *bits = NULL;

    if (w <= 0 || h <= 0) return FALSE;
    if (g_mem_dc && g_mem_bmp && g_mem_w == w && g_mem_h == h) return TRUE;

    free_back_buffer();
    g_mem_dc = CreateCompatibleDC(NULL);
    if (!g_mem_dc) return FALSE;

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    g_mem_bmp = CreateDIBSection(g_mem_dc, &bmi, DIB_RGB_COLORS,
                                 &bits, NULL, 0);
    if (!g_mem_bmp) {
        free_back_buffer();
        return FALSE;
    }
    g_mem_old = SelectObject(g_mem_dc, g_mem_bmp);
    g_mem_w = w;
    g_mem_h = h;
    g_mem_blank = FALSE;
    return TRUE;
}

static void free_grid_cache(void)
{
    if (g_grid_dc && g_grid_old) {
        SelectObject(g_grid_dc, g_grid_old);
        g_grid_old = NULL;
    }
    if (g_grid_bmp) {
        DeleteObject(g_grid_bmp);
        g_grid_bmp = NULL;
    }
    if (g_grid_dc) {
        DeleteDC(g_grid_dc);
        g_grid_dc = NULL;
    }
    g_grid_w = g_grid_h = 0;
    g_grid_valid = FALSE;
}

static BOOL ensure_grid_cache(int w, int h)
{
    BITMAPINFO bmi;
    void *bits = NULL;

    if (w <= 0 || h <= 0) return FALSE;
    if (g_grid_dc && g_grid_bmp && g_grid_w == w && g_grid_h == h)
        return TRUE;

    free_grid_cache();
    g_grid_dc = CreateCompatibleDC(NULL);
    if (!g_grid_dc) return FALSE;

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    g_grid_bmp = CreateDIBSection(g_grid_dc, &bmi, DIB_RGB_COLORS,
                                  &bits, NULL, 0);
    if (!g_grid_bmp) {
        free_grid_cache();
        return FALSE;
    }
    g_grid_old = SelectObject(g_grid_dc, g_grid_bmp);
    g_grid_w = w;
    g_grid_h = h;
    g_grid_valid = FALSE;
    return TRUE;
}

// Draw the initial grid into dc (also used to fill the cached frame).
static void render_grid0(HDC dc, int w, int h)
{
    RECT rc;
    WCHAR text[2];
    LabelPass pass;
    int r, c;

    rc.left = 0;
    rc.top = 0;
    rc.right = w;
    rc.bottom = h;
    FillRect(dc, &rc, g_br_key ? g_br_key
                               : (HBRUSH)GetStockObject(BLACK_BRUSH));

    labels_begin(dc, g_font_label, &pass);
    for (r = 0; r < g_rows; ++r) {
        text[0] = g_cfg.alphabet[r];
        for (c = 0; c < g_cols; ++c) {
            RECT cell;
            text[1] = g_cfg.alphabet[c];
            cell_rect(r, c, &cell);
            draw_label_box(dc, cell, text, 2, 0, 0,
                           g_font_label_px, FALSE, &g_lw);
        }
    }
    labels_end(dc, &pass);
    draw_status(dc, w, h);
}

static void paint_overlay(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc, dc;
    RECT rc, dirty;
    int w, h, r, c;

    hdc = BeginPaint(hwnd, &ps);
    GetClientRect(hwnd, &rc);
    w = rc.right;
    h = rc.bottom;

    dc = ensure_back_buffer(w, h) ? g_mem_dc : hdc;

    if (g_open && g_mode == HINT_MODE_GRID && g_state == 0 && !g_scanning &&
        !g_flash && !input_left_button_held() &&
        g_font_label && g_font_sub && ensure_grid_cache(w, h)) {
        if (!g_grid_valid) {
            render_grid0(g_grid_dc, w, h);
            g_grid_valid = TRUE;
        }
        dirty = ps.rcPaint;
        if (dirty.right <= dirty.left || dirty.bottom <= dirty.top)
            dirty = rc;
        BitBlt(hdc, dirty.left, dirty.top,
               dirty.right - dirty.left, dirty.bottom - dirty.top,
               g_grid_dc, dirty.left, dirty.top, SRCCOPY);
        // Leave the normal back buffer ready to draw the next state without a
        // full clear.
        if (dc == g_mem_dc && !g_mem_blank) {
            FillRect(dc, &rc, g_br_key ? g_br_key
                                       : (HBRUSH)GetStockObject(BLACK_BRUSH));
            g_mem_blank = TRUE;
        }
        EndPaint(hwnd, &ps);
        return;
    }

    if (!g_mem_blank || dc != g_mem_dc)
        FillRect(dc, &rc, g_br_key ? g_br_key
                                   : (HBRUSH)GetStockObject(BLACK_BRUSH));

    if (g_open && g_font_label && g_font_sub && g_mode == HINT_MODE_ELEMENTS) {
        int i;

        if (!g_scanning) {
            LabelPass pass;
            labels_begin(dc, g_font_label, &pass);
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
            labels_end(dc, &pass);
        }
        draw_status(dc, w, h);
    } else if (g_open && g_font_label && g_font_sub) {
        WCHAR text[2];

        if (g_state == 0) {
            LabelPass pass;
            labels_begin(dc, g_font_label, &pass);
            for (r = 0; r < g_rows; ++r) {
                text[0] = g_cfg.alphabet[r];
                for (c = 0; c < g_cols; ++c) {
                    RECT cell;
                    text[1] = g_cfg.alphabet[c];
                    cell_rect(r, c, &cell);
                    draw_label_box(dc, cell, text, 2, 0, 0,
                                   g_font_label_px, FALSE, &g_lw);
                }
            }
            labels_end(dc, &pass);
        } else if (g_state == 1) {
            RECT cell;
            LabelPass pass;
            text[0] = g_cfg.alphabet[g_row];
            labels_begin(dc, g_font_label, &pass);
            for (c = 0; c < g_cols; ++c) {
                text[1] = g_cfg.alphabet[c];
                cell_rect(g_row, c, &cell);
                draw_label_box(dc, cell, text, 2, 1, g_match,
                               g_font_label_px, FALSE, &g_lw);
            }
            labels_end(dc, &pass);
        } else if (g_state == 2) {
            RECT cell, sub;
            LabelPass pass;
            int sr, sc;

            // Thin dividers and corner labels keep the content under the
            // cell (and each subcell's centre, where the click lands) visible.
            cell_rect(g_row, g_col, &cell);
            draw_subgrid_lines(dc, cell);
            labels_begin(dc, g_font_sub, &pass);
            for (sr = 0; sr < g_sub_rows; ++sr) {
                for (sc = 0; sc < g_sub_cols; ++sc) {
                    WCHAR ch = g_sub[sr][sc];
                    subcell_rect(cell, sr, sc, &sub);
                    draw_label_box(dc, sub, &ch, 1, 0, 0,
                                   g_font_sub_px, TRUE, &g_sw);
                }
            }
            labels_end(dc, &pass);
            draw_outline(dc, cell);
        }
        draw_status(dc, w, h);
    }

    g_mem_blank = FALSE;

    if (dc != hdc) {
        dirty = ps.rcPaint;
        if (dirty.right <= dirty.left || dirty.bottom <= dirty.top)
            dirty = rc;
        BitBlt(hdc, dirty.left, dirty.top,
               dirty.right - dirty.left, dirty.bottom - dirty.top,
               dc, dirty.left, dirty.top, SRCCOPY);
    }

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
        } else if (wp == TIMER_REFRESH_ID) {
            KillTimer(hwnd, TIMER_REFRESH_ID);
            if (!g_open || g_mode != HINT_MODE_ELEMENTS) return 0;
            if (g_scan_retry) {
                // An empty first scan keeps trying (same scan, same input).
                g_scan_retry = FALSE;
                ++g_scan_id;
                uia_scan(g_mon, g_scan_id, g_cfg.log >= 2);
                return 0;
            }
            if (!g_scanning && g_refresh_on)
                start_refresh_scan();
        } else if (wp == TIMER_SETTLE_ID) {
            KillTimer(hwnd, TIMER_SETTLE_ID);
            if (g_open && g_scanning && g_mode == HINT_MODE_ELEMENTS)
                begin_scan();
        } else if (wp == TIMER_SCAN_ID) {
            KillTimer(hwnd, TIMER_SCAN_ID);
            if (!g_scanning) return 0;
            if (g_scan_wait_first) {
                finalize_scan();  // hard limit reached
                return 0;
            }
            if (g_scan_list && g_scan_list->count > 0 &&
                g_scan_list->fg_done) {
                finalize_scan();
                return 0;
            }
            {
                DWORD elapsed = GetTickCount() - g_scan_t0;
                if (elapsed >= SCAN_HARD_LIMIT_MS) {
                    finalize_scan();
                } else {
                    // Nothing yet, or not the active window's elements (a
                    // folder window Explorer has just opened answers late):
                    // keep the status text and wait, but not past the hard
                    // limit.
                    g_scan_wait_first = TRUE;
                    SetTimer(hwnd, TIMER_SCAN_ID,
                             SCAN_HARD_LIMIT_MS - elapsed, NULL);
                }
            }
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
    if (g_hint_wnd) KillTimer(g_hint_wnd, TIMER_FLASH_ID);
}

static void cancel_scan(void)
{
    if (g_hint_wnd) {
        KillTimer(g_hint_wnd, TIMER_SCAN_ID);
        KillTimer(g_hint_wnd, TIMER_REFRESH_ID);
        KillTimer(g_hint_wnd, TIMER_SETTLE_ID);
    }
    g_scan_wait_first = FALSE;
    g_scan_retry = FALSE;
    if (g_scanning || g_refresh_on)
        uia_cancel();  // drops the first scan or a background refresh
    g_scanning = FALSE;
    g_refresh_on = FALSE;
    if (g_scan_list) {
        free(g_scan_list);
        g_scan_list = NULL;
    }
}

static UINT scan_limit_ms(void)
{
    UINT ms = g_cfg.scan_timeout > 0 ? (UINT)g_cfg.scan_timeout
                                     : SCAN_DEFAULT_MS;
    if (ms > SCAN_HARD_LIMIT_MS) ms = SCAN_HARD_LIMIT_MS;
    return ms;
}

// Milliseconds left of the initial timeout measured from t0, at least 1.
static UINT scan_rest_ms(DWORD t0)
{
    DWORD elapsed = GetTickCount() - t0;
    UINT limit = scan_limit_ms();
    return elapsed < limit ? limit - elapsed : 1;
}

// Chromium sends UI Automation the new positions of elements that moved at
// most every 500 ms, and elements that appeared 150 ms later (Blink's
// ax_object_cache_impl.cc: GetLocationSerializationDelay,
// GetDeferredEventsDelay). A scan right after our own scroll or click there
// labels where the page used to be, or misses the panel the click opened, and
// keep_labels then holds those labels for the whole open. So the first scan
// waits until scan_settle has passed since that scroll or click. Returns the
// milliseconds left to wait, 0 when the scan may start now.
static UINT settle_wait_ms(void)
{
    DWORD since;
    POINT pt;

    if (g_cfg.scan_settle <= 0) return 0;
    since = input_since_page_action();
    if (since >= (DWORD)g_cfg.scan_settle) return 0;
    if (!uia_chromium_window(GetForegroundWindow()) &&
        !(GetCursorPos(&pt) && uia_chromium_window(WindowFromPoint(pt))))
        return 0;
    return (UINT)g_cfg.scan_settle - (UINT)since;
}

// Keeps the labels current while the overlay stays open, but only after the
// first scan has finished (g_scanning FALSE) so keys are never delayed.
static void schedule_refresh(void)
{
    if (!g_open || g_mode != HINT_MODE_ELEMENTS) return;
    if (g_cfg.scan_refresh <= 0 || !uia_available()) return;
    g_refresh_on = TRUE;
    if (g_hint_wnd)
        SetTimer(g_hint_wnd, TIMER_REFRESH_ID, (UINT)g_cfg.scan_refresh, NULL);
}

// Starts one background scan of this monitor. g_scanning, the typed prefix and
// the key queue are left alone: the overlay stays usable during the refresh.
static void start_refresh_scan(void)
{
    if (!g_open || !g_refresh_on) return;
    if (g_scan_list) {
        free(g_scan_list);
        g_scan_list = NULL;
    }
    ++g_scan_id;
    uia_scan(g_mon, g_scan_id, FALSE);
}

static void clear_prescan(void)
{
    if (g_pre_list) {
        free(g_pre_list);
        g_pre_list = NULL;
    }
    g_pre_id = 0;
    g_pre_mon = NULL;
    g_pre_t0 = 0;
}

// Drops a prescan that will not be adopted, stopping its worker.
static void discard_prescan(void)
{
    if (g_pre_id != 0)
        uia_cancel();
    clear_prescan();
}

// Adopts the prescan as the open session's scan. Returns FALSE when it is not
// adoptable (any prescan is then dropped) and a normal scan must start.
static BOOL adopt_prescan(void)
{
    if (g_pre_id == 0)
        return FALSE;
    if (g_pre_id != g_scan_id || g_pre_mon != g_mon ||
        GetTickCount() - g_pre_t0 > 2000 || settle_wait_ms() > 0) {
        discard_prescan();
        return FALSE;
    }

    reset_input();
    free_elements();
    g_refresh_on = FALSE;
    g_scan_retry = FALSE;
    if (g_scan_list) {
        free(g_scan_list);
        g_scan_list = NULL;
    }
    g_scanning = TRUE;
    g_scan_wait_first = FALSE;
    g_scan_t0 = g_pre_t0;
    g_scan_list = g_pre_list;   // ownership moves
    g_pre_list = NULL;
    g_pre_id = 0;
    g_pre_mon = NULL;
    g_pre_t0 = 0;

    if (g_scan_list && g_scan_list->done) {
        finalize_scan();
        return TRUE;
    }
    if (g_hint_wnd)
        SetTimer(g_hint_wnd, TIMER_SCAN_ID, scan_rest_ms(g_scan_t0), NULL);
    return TRUE;
}

// The first scan of an open (or a reload). Its timeouts count from here.
// With log = 2 it writes what it made of each element of the active window.
static void begin_scan(void)
{
    g_scan_t0 = GetTickCount();
    ++g_scan_id;
    uia_scan(g_mon, g_scan_id, g_cfg.log >= 2);
    if (g_hint_wnd)
        SetTimer(g_hint_wnd, TIMER_SCAN_ID, scan_limit_ms(), NULL);
}

static void start_scan(BOOL repaint)
{
    UINT wait;

    discard_prescan();
    reset_input();
    free_elements();
    if (g_hint_wnd) {
        KillTimer(g_hint_wnd, TIMER_REFRESH_ID);
        KillTimer(g_hint_wnd, TIMER_SETTLE_ID);
    }
    g_refresh_on = FALSE;
    g_scan_retry = FALSE;
    if (g_scan_list) {
        free(g_scan_list);
        g_scan_list = NULL;
    }
    g_scanning = TRUE;
    g_scan_wait_first = FALSE;
    // While the browser catches up, the status text shows and typed keys
    // queue as during any scan; TIMER_SETTLE_ID starts the scan.
    wait = g_hint_wnd ? settle_wait_ms() : 0;
    if (wait > 0) {
        ++g_scan_id;        // an older scan's posts no longer count
        uia_cancel();
        g_scan_t0 = GetTickCount();
        app_log(L"hint: waiting %u ms for the page to settle", wait);
        SetTimer(g_hint_wnd, TIMER_SETTLE_ID, wait, NULL);
    } else {
        begin_scan();
    }
    if (repaint) {
        invalidate();
        if (g_hint_wnd) UpdateWindow(g_hint_wnd);
    }
}

static void enter_grid_mode(void)
{
    cancel_scan();
    free_elements();
    g_mode = HINT_MODE_GRID;
    reset_input();
    invalidate();
    if (g_hint_wnd) UpdateWindow(g_hint_wnd);
}

static void start_element_mode(void)
{
    cancel_scan();
    free_elements();
    g_mode = HINT_MODE_ELEMENTS;
    start_scan(TRUE);
}

static void replay_queue(void)
{
    QueuedKey keys[MAX_QUEUED_KEYS];
    int n = g_queue_count, i;

    if (n <= 0) return;
    memcpy(keys, g_queue, sizeof(keys[0]) * (size_t)n);
    g_queue_count = 0;
    // Through hint_on_key: a Tab among them starts a new scan, and the keys
    // after it must wait for that scan instead of missing every label.
    for (i = 0; i < n && g_open; ++i)
        hint_on_key(keys[i].vk, keys[i].mods);
}

static void finalize_scan(void)
{
    ClickList *list = g_scan_list;
    HintLabel *labels;
    BOOL keep_scan;
    int n;

    g_scan_list = NULL;
    n = list ? list->count : 0;
    if (n > MAX_TARGETS) n = MAX_TARGETS;

    // A page or app still starting may expose nothing yet: retry on the
    // refresh timer instead of closing straight away. g_scanning, g_scan_t0
    // and the queued keys stay as they are, so keys keep being queued and the
    // 3 s hard limit still ends this.
    if (n <= 0 && g_cfg.scan_refresh > 0 && g_scanning &&
        GetTickCount() - g_scan_t0 < SCAN_HARD_LIMIT_MS) {
        DWORD elapsed = GetTickCount() - g_scan_t0;

        free(list);
        g_scan_retry = TRUE;
        if (g_hint_wnd) {
            // The scan timer carries on as for the first scan (its handler
            // may have killed it before calling here): the initial timeout,
            // then the wait for the active window, so a retry that finds it
            // shows at once instead of waiting for every other window.
            if (elapsed < scan_limit_ms()) {
                g_scan_wait_first = FALSE;
                SetTimer(g_hint_wnd, TIMER_SCAN_ID, scan_rest_ms(g_scan_t0),
                         NULL);
            } else {
                g_scan_wait_first = TRUE;
                SetTimer(g_hint_wnd, TIMER_SCAN_ID,
                         SCAN_HARD_LIMIT_MS - elapsed, NULL);
            }
            SetTimer(g_hint_wnd, TIMER_REFRESH_ID, REFRESH_RETRY_MS, NULL);
        }
        return;
    }

    g_scanning = FALSE;
    g_scan_wait_first = FALSE;
    g_scan_retry = FALSE;
    if (g_hint_wnd) KillTimer(g_hint_wnd, TIMER_SCAN_ID);

    // Nothing to label: close. The grid opens only with hint_mode = grid.
    if (n <= 0) {
        uia_cancel();
        free(list);
        app_log(L"hint: no clickable elements found; closing");
        hint_close();
        return;
    }

    // Showing early does not mean the scan is over: keep it and merge its
    // remaining posts, then refresh in the background.
    keep_scan = list && !list->done && g_cfg.scan_refresh > 0;
    if (!keep_scan) {
        uia_cancel();
        if (g_cfg.scan_refresh > 0) schedule_refresh();
    } else {
        g_refresh_on = TRUE;
    }

    g_targets = (ClickTarget *)malloc((size_t)n * sizeof(*g_targets));
    if (!g_targets) {
        free(list);
        hint_close();
        return;
    }
    memcpy(g_targets, list->items, (size_t)n * sizeof(*g_targets));
    g_target_count = n;
    free(list);
    app_log(L"hint: %d targets after %lu ms", n,
            (unsigned long)(GetTickCount() - g_scan_t0));

    qsort(g_targets, (size_t)n, sizeof(*g_targets), target_cmp);

    labels = build_labels(n);
    if (!labels) {
        hint_close();
        return;
    }
    g_labels = labels;

    g_prefix_len = 0;
    g_prefix[0] = 0;
    g_flash = FALSE;
    g_state = 0;
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
            g_flash = FALSE;
            invalidate();
        } else if (g_flash) {
            g_flash = FALSE;
            invalidate();
        }
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
    RECT want;

    if (!g_hint_wnd) return FALSE;
    want.left = g_mon_rect.left;
    want.top = g_mon_rect.top;
    want.right = g_mon_rect.left + g_mon_w;
    want.bottom = g_mon_rect.top + g_mon_h;

    if (g_last_show_valid && EqualRect(&want, &g_last_show)) {
        // Already in place: skip the move/resize, but still raise it above
        // topmost popups that opened since (Chrome menus are topmost).
        return SetWindowPos(g_hint_wnd, HWND_TOPMOST, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                            SWP_SHOWWINDOW) != FALSE;
    }
    if (!SetWindowPos(g_hint_wnd, HWND_TOPMOST,
                      want.left, want.top, g_mon_w, g_mon_h,
                      SWP_NOACTIVATE | SWP_SHOWWINDOW))
        return FALSE;
    g_last_show = want;
    g_last_show_valid = TRUE;
    return TRUE;
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

    if (!ensure_resources()) {
        hint_close();
        return;
    }
    if (!show_on_monitor()) {
        hint_close();
        return;
    }
    SetLayeredWindowAttributes(g_hint_wnd, KEY_COLOR, g_cfg.opacity,
                               LWA_COLORKEY | LWA_ALPHA);
    if (g_mode == HINT_MODE_ELEMENTS) start_scan(FALSE);
    InvalidateRect(g_hint_wnd, NULL, TRUE);
    UpdateWindow(g_hint_wnd);
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
        g_last_show_valid = FALSE;
        UnregisterClassW(HINT_CLASS, g_hint_inst);
    }
    discard_prescan();
    cancel_scan();
    free_elements();
    reset_input();
    g_open = FALSE;
    destroy_resources();
    free_back_buffer();
    free_grid_cache();
}

static HMONITOR cursor_monitor(void)
{
    POINT cur;
    POINT zero = { 0, 0 };
    HMONITOR mon;

    if (GetCursorPos(&cur))
        mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    else
        mon = MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY);
    if (!mon)
        mon = MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY);
    return mon;
}

static BOOL open_overlay(HMONITOR mon, int mode, BOOL paint)
{
    if (!compute_geometry(mon))
        return FALSE;

    reset_input();
    free_elements();
    if (g_hint_wnd) KillTimer(g_hint_wnd, TIMER_REFRESH_ID);
    g_refresh_on = FALSE;
    g_scan_retry = FALSE;
    g_mode = mode;
    g_start_mode = mode;
    g_scanning = (mode == HINT_MODE_ELEMENTS);

    if (!ensure_resources()) {
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
        return FALSE;
    }

    // Element mode starts or adopts its scan first, then paints, so the
    // worker runs while the status text goes up. Grid mode paints here.
    if (paint) {
        InvalidateRect(g_hint_wnd, NULL, TRUE);
        UpdateWindow(g_hint_wnd);
    }
    return TRUE;
}

void hint_open(void)
{
    HMONITOR mon;
    int mode;

    // While the keyboard is locked no key could reach the overlay, not even
    // Esc, so the tray item must not open one.
    if (g_open || input_is_locked()) return;

    input_set_key_capture(TRUE);

    if (!g_hint_wnd) {
        app_log(L"hint: no overlay window; not opening");
        discard_prescan();
        input_set_key_capture(FALSE);
        return;
    }

    mon = cursor_monitor();

    mode = g_cfg.hint_mode;
    app_log(L"hint: open (%ls)",
            mode == HINT_MODE_GRID ? L"grid" : L"elements");
    // Without UI Automation there is nothing to label; the grid only opens
    // when hint_mode = grid asks for it.
    if (mode == HINT_MODE_ELEMENTS && !uia_available()) {
        app_log(L"hint: UI Automation unavailable; not opening");
        discard_prescan();
        input_set_key_capture(FALSE);
        return;
    }

    if (!open_overlay(mon, mode, mode != HINT_MODE_ELEMENTS)) {
        app_log(L"hint: the overlay could not be shown");
        g_open = FALSE;
        discard_prescan();
        input_set_key_capture(FALSE);
        return;
    }

    if (mode == HINT_MODE_ELEMENTS) {
        // A finished prescan is finalized (and painted) before the first
        // paint, so the status text never flashes.
        if (!adopt_prescan())
            start_scan(FALSE);
        if (g_scanning) {
            invalidate();
            UpdateWindow(g_hint_wnd);
        }
    } else {
        discard_prescan();
    }
}

void hint_prepare(BOOL on)
{
    if (!on) {
        if (!g_open && g_pre_id != 0)
            discard_prescan();
        return;
    }

    if (g_open || !g_hint_wnd || g_cfg.hint_mode != HINT_MODE_ELEMENTS ||
        !uia_available())
        return;

    discard_prescan();
    // Right after our scroll or click a browser still reports the old page:
    // hint_open starts the scan once it has caught up (settle_wait_ms).
    // log = 2 skips the early scan too: every Alt press would log one, and
    // the open's own scan is the one to report.
    if (settle_wait_ms() > 0 || g_cfg.log >= 2)
        return;
    ++g_scan_id;
    g_pre_id = g_scan_id;
    g_pre_mon = cursor_monitor();
    g_pre_t0 = GetTickCount();
    uia_scan(g_pre_mon, g_pre_id, FALSE);
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
    clear_prescan();
    reset_input();
    g_open = FALSE;
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

    if (!g_open) {
        // Keep the newest result for the early scan until hint_open adopts it.
        if (g_pre_id != 0 && list->scan_id == g_pre_id) {
            if (g_pre_list) free(g_pre_list);
            g_pre_list = list;
            return;
        }
        free(list);
        return;
    }

    if (list->scan_id != g_scan_id || (!g_scanning && !g_refresh_on)) {
        free(list);
        return;
    }

    // A background refresh merges straight into the shown labels: g_scanning
    // stays FALSE so keys are handled immediately and the status does not
    // flicker back to "Finding clickable elements".
    if (g_refresh_on && !g_scanning) {
        BOOL done = list->done;

        if (g_prefix_len > 0) {
            // Mid-type: leave the shown labels alone; catch up next time.
            free(list);
            if (done) schedule_refresh();
            return;
        }
        merge_scan(list);
        free(list);
        if (done) schedule_refresh();
        return;
    }

    if (g_scan_list) free(g_scan_list);
    g_scan_list = list;

    if (list->done) {
        finalize_scan();
    } else if (g_scan_wait_first && list->count > 0 && list->fg_done) {
        // The active window answered after the initial timeout: show now.
        finalize_scan();
    }
}

void hint_hotkey_again(void)
{
    if (!g_open) return;

    // Another press reloads what the hotkey opened: a fresh element scan,
    // or the grid back at its start (hint_mode = grid). The fresh scan asks
    // windows skipped for being slow again too.
    if (g_start_mode == HINT_MODE_ELEMENTS && uia_available()) {
        uia_forget_slow();
        start_element_mode();
    } else {
        enter_grid_mode();
    }
}
