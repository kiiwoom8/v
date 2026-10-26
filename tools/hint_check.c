// hint_check.c - compare Chrome's DOM with what the element scan finds.
// Read-only: it never sends input, clicks or moves the mouse.

#include "../uia.c"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>

#ifndef UIA_IsKeyboardFocusablePropertyId
#define UIA_IsKeyboardFocusablePropertyId 30009
#endif
#ifndef UIA_ClassNamePropertyId
#define UIA_ClassNamePropertyId 30012
#endif
#ifndef UIA_IsControlElementPropertyId
#define UIA_IsControlElementPropertyId 30016
#endif
#ifndef UIA_IsContentElementPropertyId
#define UIA_IsContentElementPropertyId 30017
#endif
#ifndef UIA_LegacyIAccessibleDefaultActionPropertyId
#define UIA_LegacyIAccessibleDefaultActionPropertyId 30100
#endif
#ifndef UIA_LegacyIAccessibleRolePropertyId
#define UIA_LegacyIAccessibleRolePropertyId 30095
#endif
#ifndef UIA_AriaRolePropertyId
#define UIA_AriaRolePropertyId 30101
#endif
#ifndef UIA_DocumentControlTypeId
#define UIA_DocumentControlTypeId 50030
#endif

#define MAX_LABEL_LEN 16

Config g_cfg;
HWND g_main_wnd = NULL;

void app_log(const WCHAR *fmt, ...)
{
    WCHAR line[1024];
    va_list ap;

    va_start(ap, fmt);
    _vsnwprintf(line, 1024, fmt, ap);
    va_end(ap);
    line[1023] = 0;
    fwprintf(stderr, L"%ls\n", line);
}

typedef HRESULT (WINAPI *GetDpiForMonitorFn)(HMONITOR, int, UINT *, UINT *);

UINT dpi_for_monitor(HMONITOR mon)
{
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    UINT dpi = 0;

    if (shcore) {
        GetDpiForMonitorFn fn = (GetDpiForMonitorFn)(void *)GetProcAddress(
            shcore, "GetDpiForMonitor");
        if (fn) {
            UINT x = 96, y = 96;
            HRESULT hr = fn(mon, 0, &x, &y);
            FreeLibrary(shcore);
            if (SUCCEEDED(hr) && x > 0)
                return x;
            return 96;
        }
        FreeLibrary(shcore);
    }

    {
        HDC dc = GetDC(NULL);
        if (dc) {
            dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
            ReleaseDC(NULL, dc);
        }
    }
    return dpi > 0 ? dpi : 96;
}

typedef BOOL (WINAPI *SetDpiCtxFn)(HANDLE);
typedef HRESULT (WINAPI *SetDpiAwarenessFn)(int);
typedef BOOL (WINAPI *SetDpiAwareFn)(void);

#define DPI_CTX_PER_MONITOR_V2 ((HANDLE)-4)

static void set_dpi_awareness(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    SetDpiCtxFn ctx_fn = NULL;
    SetDpiAwarenessFn awareness_fn = NULL;
    SetDpiAwareFn aware_fn = NULL;

    if (user32) {
        ctx_fn = (SetDpiCtxFn)(void *)GetProcAddress(
            user32, "SetProcessDpiAwarenessContext");
        aware_fn = (SetDpiAwareFn)(void *)GetProcAddress(
            user32, "SetProcessDPIAware");
    }
    if (shcore)
        awareness_fn = (SetDpiAwarenessFn)(void *)GetProcAddress(
            shcore, "SetProcessDpiAwareness");

    if (ctx_fn && ctx_fn(DPI_CTX_PER_MONITOR_V2)) {
        if (shcore)
            FreeLibrary(shcore);
        return;
    }
    if (awareness_fn && SUCCEEDED(awareness_fn(2))) {
        if (shcore)
            FreeLibrary(shcore);
        return;
    }
    if (aware_fn)
        aware_fn();
    if (shcore)
        FreeLibrary(shcore);
}

static void print_wide(const WCHAR *text)
{
    char utf8[16384];
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8),
                                NULL, NULL);
    if (n > 1)
        fwrite(utf8, 1, (size_t)(n - 1), stdout);
}

static void print_line(const WCHAR *fmt, ...)
{
    WCHAR line[4096];
    va_list ap;

    va_start(ap, fmt);
    _vsnwprintf(line, 4096, fmt, ap);
    va_end(ap);
    line[4095] = 0;
    print_wide(line);
}

static void utf8_to_wide(const char *in, WCHAR *out, int cch)
{
    int n;

    out[0] = 0;
    if (!in || !in[0] || cch <= 0)
        return;
    n = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, cch);
    if (n <= 0)
        out[0] = 0;
    else
        out[cch - 1] = 0;
}

static void clean_wide(const WCHAR *in, WCHAR *out, int cch, int maxchars)
{
    int i;
    int j = 0;

    out[0] = 0;
    if (!in)
        return;
    for (i = 0; in[i] && j < cch - 1 && j < maxchars; ++i) {
        WCHAR ch = in[i];
        if (ch < 32)
            ch = L' ';
        out[j++] = ch;
    }
    out[j] = 0;
}

static LONG iround(double v)
{
    return (LONG)(v >= 0 ? v + 0.5 : v - 0.5);
}

static const WCHAR *control_type_name(int id)
{
    switch (id) {
    case 50000: return L"Button";
    case 50001: return L"Calendar";
    case 50002: return L"CheckBox";
    case 50003: return L"ComboBox";
    case 50004: return L"Edit";
    case 50005: return L"Hyperlink";
    case 50006: return L"Image";
    case 50007: return L"ListItem";
    case 50008: return L"List";
    case 50009: return L"Menu";
    case 50010: return L"MenuBar";
    case 50011: return L"MenuItem";
    case 50012: return L"ProgressBar";
    case 50013: return L"RadioButton";
    case 50014: return L"ScrollBar";
    case 50015: return L"Slider";
    case 50016: return L"Spinner";
    case 50017: return L"StatusBar";
    case 50018: return L"Tab";
    case 50019: return L"TabItem";
    case 50020: return L"Text";
    case 50021: return L"ToolBar";
    case 50022: return L"ToolTip";
    case 50023: return L"Tree";
    case 50024: return L"TreeItem";
    case 50025: return L"Custom";
    case 50026: return L"Group";
    case 50027: return L"Thumb";
    case 50028: return L"DataGrid";
    case 50029: return L"DataItem";
    case 50030: return L"Document";
    case 50031: return L"SplitButton";
    case 50032: return L"Window";
    case 50033: return L"Pane";
    case 50034: return L"Header";
    case 50035: return L"HeaderItem";
    case 50036: return L"Table";
    case 50037: return L"TitleBar";
    case 50038: return L"Separator";
    default:   return NULL;
    }
}

static void type_text(int id, WCHAR *out, int cch)
{
    const WCHAR *name = control_type_name(id);

    if (name)
        lstrcpynW(out, name, cch);
    else
        _snwprintf(out, (size_t)cch, L"Type %d", id);
    out[cch - 1] = 0;
}

static BOOL type_in_scan_condition(int id)
{
    switch (id) {
    case UIA_ButtonControlTypeId:
    case UIA_CheckBoxControlTypeId:
    case UIA_ComboBoxControlTypeId:
    case UIA_EditControlTypeId:
    case UIA_HyperlinkControlTypeId:
    case UIA_ListItemControlTypeId:
    case UIA_MenuItemControlTypeId:
    case UIA_RadioButtonControlTypeId:
    case UIA_TabItemControlTypeId:
    case UIA_TreeItemControlTypeId:
    case UIA_DataItemControlTypeId:
    case UIA_SplitButtonControlTypeId:
    case UIA_HeaderItemControlTypeId:
    case UIA_ScrollBarControlTypeId:
        return TRUE;
    default:
        return FALSE;
    }
}

typedef struct {
    int id;
    double x, y, w, h;
    char kind[16];
    char tag[64];
    char role[80];
    WCHAR text[128];
    RECT rc;
    LONG64 area;
    int hit;
} DomItem;

typedef struct {
    RECT rc;
    int control_type;
    WCHAR reason[64];
} DropItem;

static DropItem *g_drops;
static int g_drop_count;
static int g_drop_cap;

static void scan_debug_cb(const RECT *rc, int control_type,
                          const WCHAR *reason)
{
    DropItem *item;

    if (g_drop_count == g_drop_cap) {
        int cap = g_drop_cap ? g_drop_cap * 2 : 64;
        DropItem *tmp = (DropItem *)realloc(g_drops,
                                            (size_t)cap * sizeof(*tmp));
        if (!tmp)
            return;
        g_drops = tmp;
        g_drop_cap = cap;
    }
    item = &g_drops[g_drop_count++];
    memset(item, 0, sizeof(*item));
    if (rc)
        item->rc = *rc;
    item->control_type = control_type;
    lstrcpynW(item->reason, reason ? reason : L"", 64);
}

typedef HRESULT (STDMETHODCALLTYPE *GetPropFn)(IUIAutomationElement *, int,
                                               VARIANT *);
typedef HRESULT (STDMETHODCALLTYPE *FindAllFn)(IUIAutomationElement *, int,
                                               IUIAutomationCondition *,
                                               IUIAutomationElementArray **);
typedef HRESULT (STDMETHODCALLTYPE *ElementFromPointFn)(IUIAutomation *, POINT,
                                                        IUIAutomationElement **);

typedef struct HintTreeWalker HintTreeWalker;
typedef struct {
    void *QueryInterface;
    void *AddRef;
    ULONG (STDMETHODCALLTYPE *Release)(HintTreeWalker *This);
    HRESULT (STDMETHODCALLTYPE *GetParentElement)(HintTreeWalker *This,
        IUIAutomationElement *element, IUIAutomationElement **parent);
} HintTreeWalkerVtbl;
struct HintTreeWalker { HintTreeWalkerVtbl *lpVtbl; };

typedef HRESULT (STDMETHODCALLTYPE *GetWalkerFn)(IUIAutomation *,
                                                 HintTreeWalker **);

static HRESULT get_prop(IUIAutomationElement *el, int property_id,
                        VARIANT *value)
{
    GetPropFn fn = (GetPropFn)el->lpVtbl->GetCurrentPropertyValue;

    VariantInit(value);
    return fn(el, property_id, value);
}

static BOOL prop_i4(IUIAutomationElement *el, int property_id, int *out)
{
    VARIANT v;
    BOOL ok = FALSE;

    if (SUCCEEDED(get_prop(el, property_id, &v))) {
        if (v.vt == VT_I4) {
            *out = (int)v.lVal;
            ok = TRUE;
        } else if (v.vt == VT_UI4) {
            *out = (int)v.ulVal;
            ok = TRUE;
        }
    }
    VariantClear(&v);
    return ok;
}

static BOOL prop_bool(IUIAutomationElement *el, int property_id, int *out)
{
    VARIANT v;
    BOOL ok = FALSE;

    if (SUCCEEDED(get_prop(el, property_id, &v))) {
        if (v.vt == VT_BOOL) {
            *out = v.boolVal != 0;
            ok = TRUE;
        }
    }
    VariantClear(&v);
    return ok;
}

static BOOL prop_bstr(IUIAutomationElement *el, int property_id,
                      WCHAR *out, int cch)
{
    VARIANT v;
    BOOL ok = FALSE;

    out[0] = 0;
    if (SUCCEEDED(get_prop(el, property_id, &v))) {
        if (v.vt == VT_BSTR && v.bstrVal) {
            lstrcpynW(out, v.bstrVal, cch);
            ok = TRUE;
        }
    }
    VariantClear(&v);
    return ok;
}

static BOOL variant_rect(const VARIANT *v, RECT *rc)
{
    double *p;
    BOOL ok = FALSE;

    if ((v->vt & VT_ARRAY) && (v->vt & VT_TYPEMASK) == VT_R8 && v->parray &&
        SUCCEEDED(SafeArrayAccessData(v->parray, (void **)&p))) {
        rc->left = iround(p[0]);
        rc->top = iround(p[1]);
        rc->right = iround(p[0] + p[2]);
        rc->bottom = iround(p[1] + p[3]);
        SafeArrayUnaccessData(v->parray);
        ok = TRUE;
    }
    return ok;
}

static BOOL prop_rect(IUIAutomationElement *el, int property_id, RECT *rc)
{
    VARIANT v;
    BOOL ok = FALSE;

    if (SUCCEEDED(get_prop(el, property_id, &v)))
        ok = variant_rect(&v, rc);
    VariantClear(&v);
    return ok;
}

static BOOL find_document_rect(UiaEngine *engine, HWND hwnd, RECT *out)
{
    VARIANT value;
    IUIAutomationElement *root = NULL;
    IUIAutomationElementArray *array = NULL;
    IUIAutomationCondition *condition = NULL;
    FindAllFn find_all = (FindAllFn)NULL;
    RECT best;
    LONG64 best_area = 0;
    int length = 0;
    int i;
    BOOL ok = FALSE;

    memset(&best, 0, sizeof(best));
    if (FAILED(engine->automation->lpVtbl->ElementFromHandle(
            engine->automation, (UIA_HWND)hwnd, &root)) || !root)
        return FALSE;

    VariantInit(&value);
    value.vt = VT_I4;
    value.lVal = UIA_DocumentControlTypeId;
    if (FAILED(engine->automation->lpVtbl->CreatePropertyCondition(
            engine->automation, UIA_ControlTypePropertyId, value,
            &condition)) || !condition) {
        VariantClear(&value);
        root->lpVtbl->Release(root);
        return FALSE;
    }
    VariantClear(&value);

    find_all = (FindAllFn)root->lpVtbl->FindAll;
    if (FAILED(find_all(root, UIA_TREESCOPE_DESCENDANTS, condition, &array)) ||
        !array)
        goto done;
    if (FAILED(array->lpVtbl->get_Length(array, &length)) || length < 0)
        length = 0;

    for (i = 0; i < length; ++i) {
        IUIAutomationElement *el = NULL;
        RECT rc;
        LONG64 area;

        if (FAILED(array->lpVtbl->GetElement(array, i, &el)) || !el)
            continue;
        if (prop_rect(el, UIA_BoundingRectanglePropertyId, &rc)) {
            area = rect_area(&rc);
            if (area > best_area) {
                best_area = area;
                best = rc;
            }
        }
        el->lpVtbl->Release(el);
    }
    ok = best_area > 0;
    if (ok)
        *out = best;

done:
    if (array)
        array->lpVtbl->Release(array);
    condition->lpVtbl->Release(condition);
    root->lpVtbl->Release(root);
    return ok;
}

// Chrome can take a moment to expose the page Document again after a scroll.
// The scan has already been measured; this only settles the mapping.
static BOOL find_document_rect_wait(UiaEngine *engine, HWND hwnd, RECT *out,
                                    DWORD timeout_ms)
{
    DWORD started = GetTickCount();
    int tries = 0;

    for (;;) {
        ++tries;
        if (find_document_rect(engine, hwnd, out)) {
            if (tries > 1)
                print_line(L"document found after %d tries, %lu ms\n", tries,
                           (unsigned long)(GetTickCount() - started));
            return TRUE;
        }
        if (GetTickCount() - started >= timeout_ms)
            return FALSE;
        Sleep(250);
    }
}

typedef struct {
    HWND hwnd;
    DWORD pid;
    RECT rect;
    LONG64 area;
    WCHAR title[256];
} WindowFind;

static BOOL CALLBACK find_window_cb(HWND hwnd, LPARAM lp)
{
    WindowFind *find = (WindowFind *)lp;
    WCHAR cls[64];
    WCHAR title[512];
    DWORD pid = 0;
    RECT rc;
    LONG64 area;

    if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
        return TRUE;
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"Chrome_WidgetWin_1") != 0)
        return TRUE;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != find->pid)
        return TRUE;
    GetWindowTextW(hwnd, title, 512);
    if (!title[0])
        return TRUE;
    if (!GetWindowRect(hwnd, &rc))
        return TRUE;
    area = rect_area(&rc);
    if (area > find->area) {
        find->area = area;
        find->hwnd = hwnd;
        find->rect = rc;
        lstrcpynW(find->title, title, 256);
    }
    return TRUE;
}

static BOOL find_target_window(DWORD pid, WindowFind *out)
{
    memset(out, 0, sizeof(*out));
    out->pid = pid;
    EnumWindows(find_window_cb, (LPARAM)out);
    return out->hwnd != NULL;
}

static char *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[size] = 0;
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

static RECT css_to_screen(const RECT *doc, double x, double y,
                          double w, double h, double dpr)
{
    RECT rc;

    rc.left = iround((double)doc->left + x * dpr);
    rc.top = iround((double)doc->top + y * dpr);
    rc.right = iround((double)doc->left + (x + w) * dpr);
    rc.bottom = iround((double)doc->top + (y + h) * dpr);
    return rc;
}

static BOOL load_dom(const char *path, const RECT *doc, DomItem **out_items,
                     int *out_count)
{
    size_t size = 0;
    char *buf = read_file(path, &size);
    char *line;
    BOOL first = TRUE;
    BOOL have_viewport = FALSE;
    double view_w = 0, view_h = 0, dpr = 1;
    DomItem *items = NULL;
    int count = 0, cap = 0;

    if (!buf)
        return FALSE;

    line = buf;
    if (size >= 3 && (unsigned char)line[0] == 0xEF &&
        (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
        line += 3;

    while (line && *line) {
        char *next = strchr(line, '\n');
        size_t len;

        if (next) {
            *next = 0;
            ++next;
        }
        len = strlen(line);
        if (len && line[len - 1] == '\r')
            line[len - 1] = 0;

        if (first) {
            first = FALSE;
            if (strncmp(line, "viewport\t", 9) == 0) {
                char *a = line + 9;
                char *b = strchr(a, '\t');
                if (b) {
                    char *c;
                    *b = 0;
                    view_w = atof(a);
                    a = b + 1;
                    c = strchr(a, '\t');
                    if (c) {
                        *c = 0;
                        view_h = atof(a);
                        dpr = atof(c + 1);
                        if (dpr <= 0)
                            dpr = 1;
                        have_viewport = TRUE;
                    }
                }
            }
        } else {
            char *fields[9];
            int nf = 0;
            char *s = line;

            while (nf < 8) {
                char *tab = strchr(s, '\t');
                if (!tab)
                    break;
                *tab = 0;
                fields[nf++] = s;
                s = tab + 1;
            }
            if (nf == 8) {
                DomItem item;
                double x, y, w, h;
                double x0, y0, x1, y1;

                fields[8] = s;
                if ((strcmp(fields[5], "semantic") == 0 ||
                     strcmp(fields[5], "pointer") == 0)) {
                    memset(&item, 0, sizeof(item));
                    item.id = atoi(fields[0]);
                    x = atof(fields[1]);
                    y = atof(fields[2]);
                    w = atof(fields[3]);
                    h = atof(fields[4]);
                    x0 = x;
                    y0 = y;
                    x1 = x + w;
                    y1 = y + h;
                    if (have_viewport) {
                        if (x0 < 0)
                            x0 = 0;
                        if (y0 < 0)
                            y0 = 0;
                        if (x1 > view_w)
                            x1 = view_w;
                        if (y1 > view_h)
                            y1 = view_h;
                    }
                    if (x1 - x0 >= 4 && y1 - y0 >= 4) {
                        lstrcpynA(item.kind, fields[5],
                                  (int)sizeof(item.kind));
                        lstrcpynA(item.tag, fields[6],
                                  (int)sizeof(item.tag));
                        lstrcpynA(item.role, fields[7],
                                  (int)sizeof(item.role));
                        utf8_to_wide(fields[8], item.text, 128);
                        item.x = x0;
                        item.y = y0;
                        item.w = x1 - x0;
                        item.h = y1 - y0;
                        item.rc = css_to_screen(doc, x0, y0, x1 - x0,
                                                y1 - y0, dpr);
                        item.area = rect_area(&item.rc);
                        if (item.area > 0) {
                            if (count == cap) {
                                int newcap = cap ? cap * 2 : 128;
                                DomItem *tmp = (DomItem *)realloc(
                                    items,
                                    (size_t)newcap * sizeof(*tmp));
                                if (!tmp)
                                    break;
                                items = tmp;
                                cap = newcap;
                            }
                            items[count++] = item;
                        }
                    }
                }
            }
        }

        line = next;
    }

    free(buf);
    if (!have_viewport) {
        free(items);
        return FALSE;
    }
    *out_items = items;
    *out_count = count;
    return TRUE;
}

static RECT inflate_rect(const RECT *rc, LONG amount)
{
    RECT out;

    out.left = rc->left - amount;
    out.top = rc->top - amount;
    out.right = rc->right + amount;
    out.bottom = rc->bottom + amount;
    return out;
}

static BOOL point_in_rect(const RECT *rc, POINT pt)
{
    return PtInRect(rc, pt) != 0;
}

static BOOL drop_matches_item(const RECT *drop, const RECT *item)
{
    LONG64 inter = rect_intersection_area(drop, item);
    LONG64 uni = rect_area(drop) + rect_area(item) - inter;
    POINT centre;

    if (inter > 0 && uni > 0 && inter * 2 >= uni)
        return TRUE;
    centre.x = drop->left + (drop->right - drop->left) / 2;
    centre.y = drop->top + (drop->bottom - drop->top) / 2;
    return point_in_rect(item, centre);
}

static void merge_target(ScanResult *merged, const ScanTarget *target)
{
    int i;

    for (i = 0; i < merged->count; ++i) {
        ScanTarget *old = &merged->items[i];
        if (!rects_duplicate(&old->target.rc, old->target.click,
                             &target->target.rc, target->target.click))
            continue;
        if (rect_area(&target->target.rc) < rect_area(&old->target.rc)) {
            if (g_scan_debug)
                g_scan_debug(&old->target.rc, old->control_type,
                             L"duplicate");
            target_add(merged, target);
        } else if (g_scan_debug) {
            g_scan_debug(&target->target.rc, target->control_type,
                         L"duplicate");
        }
        return;
    }
    target_add(merged, target);
}

typedef struct {
    int control_type;
    WCHAR name[128];
    int is_control;
    int is_content;
    int is_offscreen;
    int is_enabled;
    int is_focusable;
    int invoke;
    int toggle;
    int selitem;
    int expand;
    WCHAR default_action[128];
    int legacy_role;
    int legacy_role_valid;
    WCHAR aria_role[128];
    WCHAR cls[128];
    RECT rc;
    int rc_valid;
    int satisfies;
} ChainNode;

static void fill_chain_node(IUIAutomationElement *el, ChainNode *node)
{
    int v;

    memset(node, 0, sizeof(*node));
    node->control_type = -1;
    node->is_control = -1;
    node->is_content = -1;
    node->is_offscreen = -1;
    node->is_enabled = -1;
    node->is_focusable = -1;
    node->invoke = -1;
    node->toggle = -1;
    node->selitem = -1;
    node->expand = -1;
    node->legacy_role = -1;
    node->rc_valid = FALSE;

    if (prop_i4(el, UIA_ControlTypePropertyId, &v))
        node->control_type = v;
    {
        WCHAR tmp[512];
        if (prop_bstr(el, UIA_NamePropertyId, tmp, 512))
            clean_wide(tmp, node->name, 128, 40);
    }
    if (prop_bool(el, UIA_IsControlElementPropertyId, &v))
        node->is_control = v;
    if (prop_bool(el, UIA_IsContentElementPropertyId, &v))
        node->is_content = v;
    if (prop_bool(el, UIA_IsOffscreenPropertyId, &v))
        node->is_offscreen = v;
    if (prop_bool(el, UIA_IsEnabledPropertyId, &v))
        node->is_enabled = v;
    if (prop_bool(el, UIA_IsKeyboardFocusablePropertyId, &v))
        node->is_focusable = v;
    if (prop_bool(el, UIA_IsInvokePatternAvailablePropertyId, &v))
        node->invoke = v;
    if (prop_bool(el, UIA_IsTogglePatternAvailablePropertyId, &v))
        node->toggle = v;
    if (prop_bool(el, UIA_IsSelectionItemPatternAvailablePropertyId, &v))
        node->selitem = v;
    if (prop_bool(el, UIA_IsExpandCollapsePatternAvailablePropertyId, &v))
        node->expand = v;
    {
        WCHAR tmp[512];
        if (prop_bstr(el, UIA_LegacyIAccessibleDefaultActionPropertyId,
                      tmp, 512))
            clean_wide(tmp, node->default_action, 128, 80);
    }
    if (prop_i4(el, UIA_LegacyIAccessibleRolePropertyId, &v)) {
        node->legacy_role = v;
        node->legacy_role_valid = TRUE;
    }
    {
        WCHAR tmp[512];
        if (prop_bstr(el, UIA_AriaRolePropertyId, tmp, 512))
            clean_wide(tmp, node->aria_role, 128, 60);
    }
    {
        WCHAR tmp[512];
        if (prop_bstr(el, UIA_ClassNamePropertyId, tmp, 512))
            clean_wide(tmp, node->cls, 128, 60);
    }
    if (prop_rect(el, UIA_BoundingRectanglePropertyId, &node->rc))
        node->rc_valid = TRUE;

    node->satisfies = (node->control_type >= 0 &&
                       (type_in_scan_condition(node->control_type) ||
                        node->invoke == 1 || node->toggle == 1 ||
                        node->selitem == 1 || node->expand == 1)) &&
                      node->is_enabled == 1 && node->is_offscreen == 0;
}

static int build_chain(UiaEngine *engine, POINT pt, ChainNode *nodes, int max)
{
    ElementFromPointFn from_point =
        (ElementFromPointFn)engine->automation->lpVtbl->ElementFromPoint;
    GetWalkerFn get_walker =
        (GetWalkerFn)engine->automation->lpVtbl->get_RawViewWalker;
    IUIAutomationElement *el = NULL;
    HintTreeWalker *walker = NULL;
    int count = 0;

    if (!from_point ||
        FAILED(from_point(engine->automation, pt, &el)) || !el)
        return 0;
    if (get_walker)
        get_walker(engine->automation, &walker);

    while (el && count < max) {
        int type;

        fill_chain_node(el, &nodes[count]);
        type = nodes[count].control_type;
        ++count;
        if (type == UIA_DocumentControlTypeId) {
            el->lpVtbl->Release(el);
            el = NULL;
            break;
        }
        if (walker) {
            IUIAutomationElement *parent = NULL;
            HRESULT hr = walker->lpVtbl->GetParentElement(walker, el, &parent);
            if (SUCCEEDED(hr) && parent == el) {
                parent->lpVtbl->Release(parent);
                parent = NULL;
            }
            if (FAILED(hr) && parent) {
                parent->lpVtbl->Release(parent);
                parent = NULL;
            }
            el->lpVtbl->Release(el);
            el = SUCCEEDED(hr) ? parent : NULL;
        } else {
            el->lpVtbl->Release(el);
            el = NULL;
        }
    }
    if (el)
        el->lpVtbl->Release(el);
    if (walker)
        walker->lpVtbl->Release(walker);
    return count;
}

typedef struct {
    int filtered;
    int not_control;
    int shadowed;
    int other;
    int dropped;
    struct {
        WCHAR reason[64];
        int count;
    } reasons[32];
    int reason_count;
} MissStats;

static void stats_add_reason(MissStats *stats, const WCHAR *reason)
{
    int i;

    ++stats->dropped;
    for (i = 0; i < stats->reason_count; ++i) {
        if (lstrcmpW(stats->reasons[i].reason, reason) == 0) {
            ++stats->reasons[i].count;
            return;
        }
    }
    if (stats->reason_count < 32) {
        lstrcpynW(stats->reasons[stats->reason_count].reason, reason, 64);
        stats->reasons[stats->reason_count].count = 1;
        ++stats->reason_count;
    }
}

static const WCHAR *bool_text(int value, WCHAR *buf, int cch)
{
    if (value < 0)
        lstrcpynW(buf, L"?", cch);
    else
        lstrcpynW(buf, value ? L"1" : L"0", cch);
    return buf;
}

static void print_miss(UiaEngine *engine, const DomItem *item, int index,
                       const ScanTarget *targets, int target_count,
                       const int *assigned, MissStats *stats)
{
    RECT inflated = inflate_rect(&item->rc, 2);
    int matched_drop = -1;
    int shadowed = 0;
    ChainNode nodes[6];
    int chain_count;
    int any_satisfies = 0;
    int any_not_control = 0;
    POINT centre;
    int i;

    centre.x = item->rc.left + (item->rc.right - item->rc.left) / 2;
    centre.y = item->rc.top + (item->rc.bottom - item->rc.top) / 2;

    print_line(L"MISS #%d %hs <%hs> role=\"%hs\" text=\"%ls\"\n",
               item->id, item->kind, item->tag, item->role, item->text);
    print_line(L"  css (%.2f,%.2f) %.2fx%.2f  screen (%ld,%ld)-(%ld,%ld)  "
               L"centre (%ld,%ld)\n",
               item->x, item->y, item->w, item->h,
               item->rc.left, item->rc.top, item->rc.right, item->rc.bottom,
               centre.x, centre.y);

    for (i = 0; i < g_drop_count; ++i) {
        if (!drop_matches_item(&g_drops[i].rc, &item->rc))
            continue;
        if (matched_drop < 0)
            matched_drop = i;
        print_line(L"  drop \"%ls\" type=%d (%ld,%ld)-(%ld,%ld)\n",
                   g_drops[i].reason, g_drops[i].control_type,
                   g_drops[i].rc.left, g_drops[i].rc.top,
                   g_drops[i].rc.right, g_drops[i].rc.bottom);
    }

    for (i = 0; i < target_count; ++i) {
        if (assigned[i] < 0 || assigned[i] == index)
            continue;
        if (!point_in_rect(&inflated, targets[i].target.click))
            continue;
        shadowed = 1;
        break;
    }

    chain_count = build_chain(engine, centre, nodes, 6);
    if (chain_count == 0) {
        print_line(L"  chain: none (ElementFromPoint returned nothing)\n");
    } else {
        print_line(L"  chain:\n");
    }
    for (i = 0; i < chain_count; ++i) {
        const ChainNode *node = &nodes[i];
        WCHAR type[32];
        WCHAR rectbuf[80];
        WCHAR control[8], content[8], offscreen[8], enabled[8];
        WCHAR focusable[8], invoke[8], toggle[8], selitem[8], expand[8];
        WCHAR rolebuf[32];

        type_text(node->control_type, type, 32);
        if (node->rc_valid)
            _snwprintf(rectbuf, 80, L"(%ld,%ld)-(%ld,%ld)",
                       node->rc.left, node->rc.top,
                       node->rc.right, node->rc.bottom);
        else
            lstrcpynW(rectbuf, L"none", 80);
        rectbuf[79] = 0;
        if (node->legacy_role_valid)
            _snwprintf(rolebuf, 32, L"%d", node->legacy_role);
        else
            lstrcpynW(rolebuf, L"?", 32);
        rolebuf[31] = 0;
        if (node->satisfies)
            any_satisfies = 1;
        if (node->satisfies && node->is_control == 0)
            any_not_control = 1;
        print_line(L"    [%d] %ls(%d) name=\"%ls\" control=%ls content=%ls "
                   L"enabled=%ls offscreen=%ls focusable=%ls invoke=%ls "
                   L"toggle=%ls selitem=%ls expand=%ls legacy_action=\"%ls\" "
                   L"legacy_role=%ls aria_role=\"%ls\" class=\"%ls\" "
                   L"rect=%ls scan=%ls\n",
                   i, type, node->control_type, node->name,
                   bool_text(node->is_control, control, 8),
                   bool_text(node->is_content, content, 8),
                   bool_text(node->is_enabled, enabled, 8),
                   bool_text(node->is_offscreen, offscreen, 8),
                   bool_text(node->is_focusable, focusable, 8),
                   bool_text(node->invoke, invoke, 8),
                   bool_text(node->toggle, toggle, 8),
                   bool_text(node->selitem, selitem, 8),
                   bool_text(node->expand, expand, 8),
                   node->default_action, rolebuf, node->aria_role, node->cls,
                   rectbuf, node->satisfies ? L"yes" : L"no");
    }

    if (shadowed) {
        print_line(L"  class shadowed\n");
        ++stats->shadowed;
    } else if (matched_drop >= 0) {
        print_line(L"  class dropped:%ls\n", g_drops[matched_drop].reason);
        stats_add_reason(stats, g_drops[matched_drop].reason);
    } else if (any_not_control) {
        print_line(L"  class not-control\n");
        ++stats->not_control;
    } else if (!any_satisfies) {
        print_line(L"  class filtered\n");
        ++stats->filtered;
    } else {
        print_line(L"  class other\n");
        ++stats->other;
    }
}

static void print_target(const ScanTarget *target, int index)
{
    WCHAR type[32];

    type_text(target->control_type, type, 32);
    print_line(L"    %d %ls \"%ls\" (%ld,%ld)-(%ld,%ld) click (%ld,%ld)\n",
               index, type, target->name,
               target->target.rc.left, target->target.rc.top,
               target->target.rc.right, target->target.rc.bottom,
               target->target.click.x, target->target.click.y);
}

// ---------------------------------------------------------------------------
// label layout, copied from hint.c's element hint drawing

static WCHAR lower_ascii(WCHAR ch)
{
    if (ch >= L'A' && ch <= L'Z')
        return (WCHAR)(ch - L'A' + L'a');
    return ch;
}

typedef struct {
    WCHAR s[MAX_LABEL_LEN];
} LabelStr;

typedef struct {
    ClickTarget target;
    int orig;
    WCHAR label[MAX_LABEL_LEN];
    RECT box;
    LONG64 box_area;
    int hidden;
} LabelBox;

static int g_label_dpi = 96;

static int label_target_band(int top)
{
    int band = MulDiv(16, g_label_dpi, 96);

    if (band < 1)
        band = 1;
    return top >= 0 ? top / band : (top - band + 1) / band;
}

static int __cdecl label_target_cmp(const void *pa, const void *pb)
{
    const LabelBox *a = (const LabelBox *)pa;
    const LabelBox *b = (const LabelBox *)pb;
    int ba = label_target_band(a->target.rc.top);
    int bb = label_target_band(b->target.rc.top);

    if (ba != bb)
        return ba < bb ? -1 : 1;
    if (a->target.rc.left != b->target.rc.left)
        return a->target.rc.left < b->target.rc.left ? -1 : 1;
    if (a->target.rc.top != b->target.rc.top)
        return a->target.rc.top < b->target.rc.top ? -1 : 1;
    return 0;
}

// Vimium's prefix-free labelling, copied from hint.c build_labels.
static void build_labels(int n, LabelBox *boxes)
{
    int alen = (int)wcslen(g_cfg.alphabet);
    int cap, count, offset, i, j;
    LabelStr *pool;

    if (n <= 0 || alen < 1)
        return;
    if (alen > 60)
        alen = 60;
    cap = 2 * n + 2 * alen + 16;
    pool = (LabelStr *)calloc((size_t)cap, sizeof(*pool));
    if (!pool)
        return;

    count = 1;
    offset = 0;
    pool[0].s[0] = 0;
    while (count - offset < n || count == 1) {
        const WCHAR *base;
        int len;

        if (offset >= cap)
            break;
        base = pool[offset].s;
        len = (int)wcslen(base);
        if (len > MAX_LABEL_LEN - 2)
            len = MAX_LABEL_LEN - 2;
        ++offset;

        for (j = 0; j < alen && count < cap; ++j) {
            WCHAR *dst = pool[count].s;
            for (i = 0; i < len; ++i)
                dst[i] = base[i];
            dst[len] = lower_ascii(g_cfg.alphabet[j]);
            dst[len + 1] = 0;
            ++count;
        }
    }

    for (i = 0; i < n; ++i) {
        int src = offset + i;
        if (src >= count)
            src = count - 1;
        if (src < 0)
            src = 0;
        lstrcpynW(boxes[i].label, pool[src].s, MAX_LABEL_LEN);
    }
    free(pool);
}

static void layout_labels(LabelBox *boxes, int n, int dpi,
                          const RECT *monitor)
{
    HDC dc;
    HFONT font;
    HGDIOBJ old;
    int px, pad, maxh = 0;
    int mon_w = monitor->right - monitor->left;
    int mon_h = monitor->bottom - monitor->top;
    int i;

    px = MulDiv(13, dpi, 96);
    if (px < 1)
        px = 1;
    pad = px / 4;
    if (pad < 2)
        pad = 2;

    dc = GetDC(NULL);
    if (!dc)
        return;
    font = CreateFontW(-px, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    if (!font) {
        ReleaseDC(NULL, dc);
        return;
    }
    old = SelectObject(dc, font);

    for (i = 0; g_cfg.alphabet[i]; ++i) {
        SIZE sz;
        WCHAR ch = lower_ascii(g_cfg.alphabet[i]);
        if (GetTextExtentPoint32W(dc, &ch, 1, &sz) && sz.cy > maxh)
            maxh = sz.cy;
    }
    if (maxh <= 0) {
        TEXTMETRICW tm;
        if (GetTextMetricsW(dc, &tm))
            maxh = tm.tmHeight;
    }

    for (i = 0; i < n; ++i) {
        SIZE sz;
        int len = (int)wcslen(boxes[i].label);
        int bw, bh, left, top;

        if (!GetTextExtentPoint32W(dc, boxes[i].label, len, &sz))
            sz.cx = 0;
        bw = sz.cx + 2 * pad;
        bh = maxh + 2 * pad;
        left = boxes[i].target.rc.left - monitor->left;
        top = boxes[i].target.rc.top - monitor->top;
        if (left + bw > mon_w)
            left = mon_w - bw;
        if (top + bh > mon_h)
            top = mon_h - bh;
        if (left < 0)
            left = 0;
        if (top < 0)
            top = 0;
        if (bw > mon_w)
            bw = mon_w;
        if (bh > mon_h)
            bh = mon_h;
        if (bw < 1)
            bw = 1;
        if (bh < 1)
            bh = 1;
        boxes[i].box.left = left;
        boxes[i].box.top = top;
        boxes[i].box.right = left + bw;
        boxes[i].box.bottom = top + bh;
        boxes[i].box_area = (LONG64)bw * bh;
    }

    SelectObject(dc, old);
    DeleteObject(font);
    ReleaseDC(NULL, dc);
}

static LONG64 region_area(HRGN rgn)
{
    DWORD size = GetRegionData(rgn, 0, NULL);
    RGNDATA *data;
    LONG64 area = 0;
    DWORD i;

    if (!size)
        return 0;
    data = (RGNDATA *)malloc(size);
    if (!data)
        return 0;
    if (GetRegionData(rgn, size, data)) {
        const RECT *rects = (const RECT *)data->Buffer;
        for (i = 0; i < data->rdh.nCount; ++i) {
            LONG64 w = (LONG64)rects[i].right - rects[i].left;
            LONG64 h = (LONG64)rects[i].bottom - rects[i].top;
            if (w > 0 && h > 0)
                area += w * h;
        }
    }
    free(data);
    return area;
}

static void compute_hidden(LabelBox *boxes, int n)
{
    HRGN acc = CreateRectRgn(0, 0, 0, 0);
    HRGN box = CreateRectRgn(0, 0, 0, 0);
    HRGN inter = CreateRectRgn(0, 0, 0, 0);
    int i;

    if (!acc || !box || !inter) {
        if (acc)
            DeleteObject(acc);
        if (box)
            DeleteObject(box);
        if (inter)
            DeleteObject(inter);
        return;
    }

    for (i = n - 1; i >= 0; --i) {
        LONG64 covered;

        SetRectRgn(box, boxes[i].box.left, boxes[i].box.top,
                   boxes[i].box.right, boxes[i].box.bottom);
        CombineRgn(inter, box, acc, RGN_AND);
        covered = region_area(inter);
        boxes[i].hidden = boxes[i].box_area > 0 &&
                          covered * 5 >= boxes[i].box_area * 2;
        CombineRgn(acc, acc, box, RGN_OR);
    }

    DeleteObject(acc);
    DeleteObject(box);
    DeleteObject(inter);
}

static int print_hidden_labels(const LabelBox *boxes, int label_count,
                               const ScanTarget *targets, int target_count,
                               const DomItem *dom, int dom_count,
                               const int *assigned, const int *pos_of,
                               const int *hidden_of, const RECT *monitor)
{
    int hit_hidden = 0;
    int d;

    print_line(L"HIDDEN LABELS (DOM hit, every assigned target label hidden):\n");
    for (d = 0; d < dom_count; ++d) {
        int all_hidden = 1;
        int target_count_for_dom = 0;
        int t;

        if (!dom[d].hit)
            continue;
        for (t = 0; t < target_count; ++t) {
            if (assigned[t] != d)
                continue;
            ++target_count_for_dom;
            if (!hidden_of[t])
                all_hidden = 0;
        }
        if (!target_count_for_dom || !all_hidden)
            continue;

        ++hit_hidden;
        print_line(L"  #%d %hs <%hs> role=\"%hs\" text=\"%ls\" "
                   L"screen=(%ld,%ld)-(%ld,%ld)\n",
                   dom[d].id, dom[d].kind, dom[d].tag, dom[d].role,
                   dom[d].text, dom[d].rc.left, dom[d].rc.top,
                   dom[d].rc.right, dom[d].rc.bottom);

        for (t = 0; t < target_count; ++t) {
            int pos;
            int j;
            WCHAR type[32];

            if (assigned[t] != d)
                continue;
            pos = pos_of[t];
            if (pos < 0 || pos >= label_count || !boxes[pos].hidden)
                continue;
            type_text(targets[boxes[pos].orig].control_type, type, 32);
            print_line(L"    label \"%ls\" target %d %ls(%d) \"%ls\" "
                       L"rect=(%ld,%ld)-(%ld,%ld) box=(%ld,%ld)-(%ld,%ld)\n",
                       boxes[pos].label, boxes[pos].orig, type,
                       targets[boxes[pos].orig].control_type,
                       targets[boxes[pos].orig].name,
                       targets[boxes[pos].orig].target.rc.left,
                       targets[boxes[pos].orig].target.rc.top,
                       targets[boxes[pos].orig].target.rc.right,
                       targets[boxes[pos].orig].target.rc.bottom,
                       boxes[pos].box.left + monitor->left,
                       boxes[pos].box.top + monitor->top,
                       boxes[pos].box.right + monitor->left,
                       boxes[pos].box.bottom + monitor->top);

            for (j = pos + 1; j < label_count; ++j) {
                RECT overlap;

                if (!IntersectRect(&overlap, &boxes[pos].box,
                                   &boxes[j].box))
                    continue;
                type_text(targets[boxes[j].orig].control_type, type, 32);
                print_line(L"      covered by %ls(%d) \"%ls\" "
                           L"rect=(%ld,%ld)-(%ld,%ld) label \"%ls\" "
                           L"box=(%ld,%ld)-(%ld,%ld)\n",
                           type,
                           targets[boxes[j].orig].control_type,
                           targets[boxes[j].orig].name,
                           targets[boxes[j].orig].target.rc.left,
                           targets[boxes[j].orig].target.rc.top,
                           targets[boxes[j].orig].target.rc.right,
                           targets[boxes[j].orig].target.rc.bottom,
                           boxes[j].label,
                           boxes[j].box.left + monitor->left,
                           boxes[j].box.top + monitor->top,
                           boxes[j].box.right + monitor->left,
                           boxes[j].box.bottom + monitor->top);
            }
        }
    }
    if (!hit_hidden)
        print_line(L"  (none)\n");
    return hit_hidden;
}

static void print_all_hidden_labels(const LabelBox *boxes, int n,
                                    const ScanTarget *targets,
                                    const RECT *monitor)
{
    int any = 0;
    int i;

    print_line(L"HIDDEN LABEL DETAIL (all hidden labels):\n");
    for (i = 0; i < n; ++i) {
        int j;
        WCHAR type[32];

        if (!boxes[i].hidden)
            continue;
        any = 1;
        type_text(targets[boxes[i].orig].control_type, type, 32);
        print_line(L"  label \"%ls\" target %d %ls(%d) \"%ls\" "
                   L"rect=(%ld,%ld)-(%ld,%ld) box=(%ld,%ld)-(%ld,%ld)\n",
                   boxes[i].label, boxes[i].orig, type,
                   targets[boxes[i].orig].control_type,
                   targets[boxes[i].orig].name,
                   targets[boxes[i].orig].target.rc.left,
                   targets[boxes[i].orig].target.rc.top,
                   targets[boxes[i].orig].target.rc.right,
                   targets[boxes[i].orig].target.rc.bottom,
                   boxes[i].box.left + monitor->left,
                   boxes[i].box.top + monitor->top,
                   boxes[i].box.right + monitor->left,
                   boxes[i].box.bottom + monitor->top);
        for (j = i + 1; j < n; ++j) {
            RECT overlap;

            if (!IntersectRect(&overlap, &boxes[i].box, &boxes[j].box))
                continue;
            type_text(targets[boxes[j].orig].control_type, type, 32);
            print_line(L"    covered by %ls(%d) \"%ls\" "
                       L"rect=(%ld,%ld)-(%ld,%ld) label \"%ls\" "
                       L"box=(%ld,%ld)-(%ld,%ld)\n",
                       type, targets[boxes[j].orig].control_type,
                       targets[boxes[j].orig].name,
                       targets[boxes[j].orig].target.rc.left,
                       targets[boxes[j].orig].target.rc.top,
                       targets[boxes[j].orig].target.rc.right,
                       targets[boxes[j].orig].target.rc.bottom,
                       boxes[j].label,
                       boxes[j].box.left + monitor->left,
                       boxes[j].box.top + monitor->top,
                       boxes[j].box.right + monitor->left,
                       boxes[j].box.bottom + monitor->top);
        }
    }
    if (!any)
        print_line(L"  (none)\n");
}

static int print_outside_doc(const RECT *doc, const ScanTarget *targets,
                             int target_count)
{
    int outside = 0;
    int i;

    print_line(L"OUTSIDE DOC (target rect intersects document, click outside):\n");
    for (i = 0; i < target_count; ++i) {
        RECT inter;
        POINT click = targets[i].target.click;

        if (!IntersectRect(&inter, &targets[i].target.rc, doc))
            continue;
        if (PtInRect(doc, click))
            continue;
        print_target(&targets[i], i);
        ++outside;
    }
    if (!outside)
        print_line(L"  (none)\n");
    return outside;
}

typedef struct {
    ScanResult final;
    DWORD first_ms;
    DWORD last_ms;
} ScanRun;

static void run_scans(UiaEngine *engine, HWND hwnd, const RECT *winrect,
                      const RECT *monitor, int runs, ScanRun *out)
{
    int run;
    int i;

    memset(out, 0, sizeof(*out));
    for (run = 0; run < runs; ++run) {
        ScanResult found;
        ScanResult merged;
        DWORD started;
        DWORD elapsed;

        memset(&found, 0, sizeof(found));
        memset(&merged, 0, sizeof(merged));
        if (run == runs - 1) {
            g_drop_count = 0;
            g_scan_debug = scan_debug_cb;
        }
        started = GetTickCount();
        scan_window(engine, hwnd, winrect, 0, monitor, &found);
        elapsed = GetTickCount() - started;
        for (i = 0; i < found.count; ++i)
            merge_target(&merged, &found.items[i]);
        if (run == runs - 1)
            g_scan_debug = NULL;
        if (run == 0)
            out->first_ms = elapsed;
        out->last_ms = elapsed;
        print_line(L"run %d: %d targets (raw %d), %lu ms\n", run + 1,
                   merged.count, found.count, (unsigned long)elapsed);
        free(found.items);
        if (run == runs - 1) {
            out->final = merged;
        } else {
            free(merged.items);
            Sleep(800);
        }
    }
}

static BOOL wait_for_file(const char *path, DWORD timeout_ms)
{
    DWORD started = GetTickCount();

    for (;;) {
        FILE *f = fopen(path, "rb");
        if (f) {
            fclose(f);
            return TRUE;
        }
        if (GetTickCount() - started >= timeout_ms)
            return FALSE;
        Sleep(100);
    }
}

static void print_summary(int dom_count, int semantic, int pointer,
                          int hit_semantic, int hit_pointer, int extra,
                          int targets, DWORD first_ms, DWORD last_ms,
                          int labels_hidden, int hit_hidden, int outside_doc,
                          int failed, const MissStats *stats)
{
    int i;

    print_line(L"SUMMARY dom=%d semantic=%d pointer=%d hit_semantic=%d "
               L"hit_pointer=%d miss_semantic=%d miss_pointer=%d extra=%d "
               L"targets=%d scan_ms_first=%lu scan_ms_last=%lu "
               L"labels_hidden=%d hit_hidden=%d outside_doc=%d",
               dom_count, semantic, pointer, hit_semantic, hit_pointer,
               semantic - hit_semantic, pointer - hit_pointer, extra,
               targets, (unsigned long)first_ms, (unsigned long)last_ms,
               labels_hidden, hit_hidden, outside_doc);
    print_line(L" class_filtered=%d class_not_control=%d class_shadowed=%d "
               L"class_other=%d class_dropped=%d",
               stats->filtered, stats->not_control, stats->shadowed,
               stats->other, stats->dropped);
    for (i = 0; i < stats->reason_count; ++i) {
        char slug[64];
        int j;
        int k = 0;

        for (j = 0; stats->reasons[i].reason[j] && k < 63; ++j) {
            WCHAR ch = stats->reasons[i].reason[j];
            if (ch >= L'A' && ch <= L'Z')
                ch = (WCHAR)(ch - L'A' + L'a');
            if ((ch >= L'a' && ch <= L'z') ||
                (ch >= L'0' && ch <= L'9')) {
                slug[k++] = (char)ch;
            } else if (k > 0 && slug[k - 1] != '_') {
                slug[k++] = '_';
            }
        }
        slug[k] = 0;
        print_line(L" class_drop_%hs=%d", slug[0] ? slug : "other",
                   stats->reasons[i].count);
    }
    print_line(L" failed=%d\n", failed);
}

static void usage(void)
{
    print_line(L"usage: hint_check <chrome_browser_pid> <dom.tsv> "
               L"[-n runs] [-s] [-v]\n");
}

int main(int argc, char **argv)
{
    DWORD pid;
    const char *dom_path;
    int runs = 2;
    int verbose = 0;
    int scan_first = 0;
    int i;
    HRESULT hr;
    UiaEngine engine;
    WindowFind win;
    RECT monitor_rect = { 0, 0, 0, 0 };
    RECT doc_rect = { 0, 0, 0, 0 };
    DomItem *dom = NULL;
    int dom_count = 0;
    ScanResult final;
    ScanRun scan_run;
    DWORD first_ms = 0, last_ms = 0;
    int failed = 1;
    int dpi = 96;
    LabelBox *labels = NULL;
    int *pos_of = NULL;
    int *hidden_of = NULL;
    int labels_hidden = 0, hit_hidden = 0, outside_doc = 0;
    MissStats stats;

    if (argc < 3) {
        usage();
        return 2;
    }
    pid = (DWORD)strtoul(argv[1], NULL, 10);
    dom_path = argv[2];
    for (i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            runs = atoi(argv[++i]);
            if (runs < 1)
                runs = 1;
        } else if (strcmp(argv[i], "-s") == 0) {
            scan_first = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else {
            usage();
            return 2;
        }
    }
    if (!pid) {
        print_line(L"invalid process id\n");
        return 2;
    }

    memset(&stats, 0, sizeof(stats));
    memset(&final, 0, sizeof(final));
    memset(&scan_run, 0, sizeof(scan_run));
    lstrcpynW(g_cfg.alphabet, L"abcdefhjklmnopqrstuvwxz", 64);
    set_dpi_awareness();
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr != S_OK && hr != S_FALSE) {
        print_line(L"COM initialization failed (0x%08lx)\n",
                   (unsigned long)hr);
        return 1;
    }

    g_slow_memory = FALSE;
    if (!uia_engine_init(&engine, TRUE)) {
        print_line(L"UI Automation unavailable\n");
        print_summary(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, &stats);
        CoUninitialize();
        return 1;
    }
    uia_pool_start();

    if (!find_target_window(pid, &win)) {
        print_line(L"no visible Chrome_WidgetWin_1 window for pid %lu\n",
                   (unsigned long)pid);
        print_summary(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, &stats);
        goto finish_runs;
    }

    {
        HMONITOR mon = MonitorFromWindow(win.hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;

        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (!mon || !GetMonitorInfoW(mon, &mi)) {
            print_line(L"monitor lookup failed\n");
            print_summary(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, &stats);
            goto finish_runs;
        }
        monitor_rect = mi.rcMonitor;
        dpi = (int)dpi_for_monitor(mon);
        if (dpi <= 0)
            dpi = 96;
    }
    g_label_dpi = dpi;
    {
        HWND fg = GetForegroundWindow();
        WCHAR fgcls[64] = L"";
        WCHAR fgtitle[128] = L"";
        if (fg) {
            GetClassNameW(fg, fgcls, 64);
            GetWindowTextW(fg, fgtitle, 128);
        }
        print_line(L"foreground class=\"%ls\" title=\"%ls\"\n",
                   fgcls, fgtitle);
    }
    print_line(L"window (%ld,%ld)-(%ld,%ld)  monitor (%ld,%ld)-(%ld,%ld)  "
               L"dpi=%d title=\"%ls\"\n",
               win.rect.left, win.rect.top, win.rect.right, win.rect.bottom,
               monitor_rect.left, monitor_rect.top,
               monitor_rect.right, monitor_rect.bottom, dpi, win.title);

    if (scan_first) {
        run_scans(&engine, win.hwnd, &win.rect, &monitor_rect, runs,
                  &scan_run);
        final = scan_run.final;
        scan_run.final.items = NULL;
        print_line(L"waiting for %hs (scan-first)\n", dom_path);
        if (!wait_for_file(dom_path, 30000)) {
            print_line(L"timed out waiting for the DOM file\n");
            print_summary(0, 0, 0, 0, 0, 0, final.count, scan_run.first_ms,
                          scan_run.last_ms, 0, 0, 0, 1, &stats);
            goto finish_runs;
        }
    }

    if (scan_first) {
        if (!find_document_rect_wait(&engine, win.hwnd, &doc_rect, 30000)) {
            print_line(L"no Document element found in the Chrome window\n");
            print_summary(0, 0, 0, 0, 0, 0, final.count,
                          scan_run.first_ms, scan_run.last_ms, 0, 0, 0, 1,
                          &stats);
            goto finish_runs;
        }
    } else if (!find_document_rect(&engine, win.hwnd, &doc_rect)) {
        print_line(L"no Document element found in the Chrome window\n");
        print_summary(0, 0, 0, 0, 0, 0, final.count, scan_run.first_ms,
                      scan_run.last_ms, 0, 0, 0, 1, &stats);
        goto finish_runs;
    }
    print_line(L"document (%ld,%ld)-(%ld,%ld)\n",
               doc_rect.left, doc_rect.top, doc_rect.right, doc_rect.bottom);

    if (!load_dom(dom_path, &doc_rect, &dom, &dom_count)) {
        print_line(L"could not read %hs\n", dom_path);
        print_summary(0, 0, 0, 0, 0, 0, final.count, scan_run.first_ms,
                      scan_run.last_ms, 0, 0, 0, 1, &stats);
        goto finish_runs;
    }
    print_line(L"DOM items: %d\n", dom_count);

    if (!scan_first) {
        run_scans(&engine, win.hwnd, &win.rect, &monitor_rect, runs,
                  &scan_run);
        final = scan_run.final;
        scan_run.final.items = NULL;
    }
    first_ms = scan_run.first_ms;
    last_ms = scan_run.last_ms;
    failed = 0;

    if (final.count > 0) {
        labels = (LabelBox *)calloc((size_t)final.count, sizeof(*labels));
        pos_of = (int *)malloc((size_t)final.count * sizeof(*pos_of));
        hidden_of = (int *)malloc((size_t)final.count * sizeof(*hidden_of));
        if (labels && pos_of && hidden_of) {
            for (i = 0; i < final.count; ++i) {
                labels[i].orig = i;
                labels[i].target = final.items[i].target;
            }
            g_label_dpi = dpi;
            qsort(labels, (size_t)final.count, sizeof(*labels),
                  label_target_cmp);
            build_labels(final.count, labels);
            layout_labels(labels, final.count, dpi, &monitor_rect);
            compute_hidden(labels, final.count);
            for (i = 0; i < final.count; ++i) {
                int orig;
                pos_of[i] = -1;
                hidden_of[i] = 0;
                orig = labels[i].orig;
                pos_of[orig] = i;
                hidden_of[orig] = labels[i].hidden;
                if (labels[i].hidden)
                    ++labels_hidden;
            }
        } else {
            free(labels);
            labels = NULL;
            free(pos_of);
            pos_of = NULL;
            free(hidden_of);
            hidden_of = NULL;
            print_line(L"out of memory (labels)\n");
            failed = 1;
        }
    }

    {
        int *assigned;
        int semantic = 0, pointer = 0, hit_semantic = 0, hit_pointer = 0;
        int extra = 0;
        int t;

        assigned = final.count
            ? (int *)malloc((size_t)final.count * sizeof(*assigned))
            : NULL;
        if (final.count && !assigned) {
            print_line(L"out of memory\n");
            failed = 1;
            print_summary(dom_count, 0, 0, 0, 0, 0, final.count, first_ms,
                          last_ms, labels_hidden, 0, 0, failed, &stats);
            goto finish_runs;
        }
        for (t = 0; t < final.count; ++t)
            assigned[t] = -1;

        for (t = 0; t < final.count; ++t) {
            int best = -1;
            LONG64 best_area = 0;
            int d;

            for (d = 0; d < dom_count; ++d) {
                RECT inflated = inflate_rect(&dom[d].rc, 2);
                if (!point_in_rect(&inflated, final.items[t].target.click))
                    continue;
                if (best < 0 || dom[d].area < best_area) {
                    best = d;
                    best_area = dom[d].area;
                }
            }
            assigned[t] = best;
            if (best >= 0)
                dom[best].hit = 1;
        }

        for (i = 0; i < dom_count; ++i) {
            if (strcmp(dom[i].kind, "semantic") == 0)
                ++semantic;
            else if (strcmp(dom[i].kind, "pointer") == 0)
                ++pointer;
            if (dom[i].hit) {
                if (strcmp(dom[i].kind, "semantic") == 0)
                    ++hit_semantic;
                else if (strcmp(dom[i].kind, "pointer") == 0)
                    ++hit_pointer;
            }
        }

        for (i = 0; i < dom_count; ++i) {
            if (!dom[i].hit)
                print_miss(&engine, &dom[i], i, final.items, final.count,
                           assigned, &stats);
        }

        if (labels) {
            hit_hidden = print_hidden_labels(
                labels, final.count, final.items, final.count, dom,
                dom_count, assigned, pos_of, hidden_of, &monitor_rect);
        } else {
            print_line(L"HIDDEN LABELS "
                       L"(DOM hit, every assigned target label hidden):\n");
            print_line(L"  (none)\n");
        }
        if (labels)
            print_all_hidden_labels(labels, final.count, final.items,
                                    &monitor_rect);

        outside_doc = print_outside_doc(&doc_rect, final.items,
                                        final.count);

        print_line(L"EXTRA targets (not assigned to any DOM item):\n");
        for (t = 0; t < final.count; ++t) {
            if (assigned[t] >= 0)
                continue;
            ++extra;
            print_target(&final.items[t], t);
        }

        if (verbose) {
            print_line(L"ALL targets:\n");
            for (t = 0; t < final.count; ++t) {
                WCHAR type[32];
                type_text(final.items[t].control_type, type, 32);
                print_line(L"    %d %ls \"%ls\" (%ld,%ld)-(%ld,%ld) "
                           L"click (%ld,%ld) assigned=%d hidden=%d "
                           L"label=\"%ls\"\n",
                           t, type, final.items[t].name,
                           final.items[t].target.rc.left,
                           final.items[t].target.rc.top,
                           final.items[t].target.rc.right,
                           final.items[t].target.rc.bottom,
                           final.items[t].target.click.x,
                           final.items[t].target.click.y, assigned[t],
                           hidden_of ? hidden_of[t] : 0,
                           labels && pos_of && pos_of[t] >= 0
                               ? labels[pos_of[t]].label : L"");
            }
        }

        print_summary(dom_count, semantic, pointer, hit_semantic,
                      hit_pointer, extra, final.count, first_ms, last_ms,
                      labels_hidden, hit_hidden, outside_doc, failed, &stats);
        free(assigned);
    }

finish_runs:
    free(final.items);
    free(dom);
    free(g_drops);
    free(labels);
    free(pos_of);
    free(hidden_of);

    uia_pool_stop();
    uia_engine_cleanup(&engine);
    CoUninitialize();
    return failed ? 1 : 0;
}
