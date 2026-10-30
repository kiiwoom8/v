// uia.c - UI Automation element scan on a worker thread (see common.h).

#include "common.h"
#include <math.h>
#include <objbase.h>
#include <oleauto.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif
#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif
#ifndef WS_EX_TRANSPARENT
#define WS_EX_TRANSPARENT 0x00000020L
#endif
#ifndef WS_EX_LAYERED
#define WS_EX_LAYERED 0x00080000L
#endif

#define UIA_TREESCOPE_DESCENDANTS 4
#define UIA_TREESCOPE_CHILDREN 2
#define UIA_MODE_NONE 0
#define UIA_SCAN_TIMEOUT_MS 1500
#define UIA_MAX_TARGETS 2000
#define UIA_HELPER_THREADS 8

#ifndef UIA_BoundingRectanglePropertyId
#define UIA_BoundingRectanglePropertyId 30001
#endif
#ifndef UIA_ControlTypePropertyId
#define UIA_ControlTypePropertyId 30003
#endif
#ifndef UIA_NamePropertyId
#define UIA_NamePropertyId 30005
#endif
#ifndef UIA_IsEnabledPropertyId
#define UIA_IsEnabledPropertyId 30010
#endif
#ifndef UIA_IsOffscreenPropertyId
#define UIA_IsOffscreenPropertyId 30022
#endif
#ifndef UIA_IsExpandCollapsePatternAvailablePropertyId
#define UIA_IsExpandCollapsePatternAvailablePropertyId 30028
#endif
#ifndef UIA_IsInvokePatternAvailablePropertyId
#define UIA_IsInvokePatternAvailablePropertyId 30031
#endif
#ifndef UIA_IsSelectionItemPatternAvailablePropertyId
#define UIA_IsSelectionItemPatternAvailablePropertyId 30036
#endif
#ifndef UIA_IsTogglePatternAvailablePropertyId
#define UIA_IsTogglePatternAvailablePropertyId 30041
#endif

#ifndef UIA_ButtonControlTypeId
#define UIA_ButtonControlTypeId 50000
#endif
#ifndef UIA_CheckBoxControlTypeId
#define UIA_CheckBoxControlTypeId 50002
#endif
#ifndef UIA_ComboBoxControlTypeId
#define UIA_ComboBoxControlTypeId 50003
#endif
#ifndef UIA_EditControlTypeId
#define UIA_EditControlTypeId 50004
#endif
#ifndef UIA_HyperlinkControlTypeId
#define UIA_HyperlinkControlTypeId 50005
#endif
#ifndef UIA_ListItemControlTypeId
#define UIA_ListItemControlTypeId 50007
#endif
#ifndef UIA_ListControlTypeId
#define UIA_ListControlTypeId 50008
#endif
#ifndef UIA_MenuItemControlTypeId
#define UIA_MenuItemControlTypeId 50011
#endif
#ifndef UIA_RadioButtonControlTypeId
#define UIA_RadioButtonControlTypeId 50013
#endif
#ifndef UIA_TabItemControlTypeId
#define UIA_TabItemControlTypeId 50019
#endif
#ifndef UIA_TreeItemControlTypeId
#define UIA_TreeItemControlTypeId 50024
#endif
#ifndef UIA_DataItemControlTypeId
#define UIA_DataItemControlTypeId 50029
#endif
#ifndef UIA_DocumentControlTypeId
#define UIA_DocumentControlTypeId 50030
#endif
#ifndef UIA_SplitButtonControlTypeId
#define UIA_SplitButtonControlTypeId 50031
#endif
#ifndef UIA_ScrollBarControlTypeId
#define UIA_ScrollBarControlTypeId 50014
#endif
#ifndef UIA_HeaderItemControlTypeId
#define UIA_HeaderItemControlTypeId 50035
#endif

typedef void *UIA_HWND;

typedef struct IUIAutomation IUIAutomation;
typedef struct IUIAutomation2 IUIAutomation2;
typedef struct IUIAutomationElement IUIAutomationElement;
typedef struct IUIAutomationElementArray IUIAutomationElementArray;
typedef struct IUIAutomationCacheRequest IUIAutomationCacheRequest;
typedef struct IUIAutomationCondition IUIAutomationCondition;

typedef struct IUIAutomationVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IUIAutomation *This,
        REFIID riid, void **ppvObject);
    ULONG (STDMETHODCALLTYPE *AddRef)(IUIAutomation *This);
    ULONG (STDMETHODCALLTYPE *Release)(IUIAutomation *This);
    void *CompareElements;
    void *CompareRuntimeIds;
    void *GetRootElement;
    HRESULT (STDMETHODCALLTYPE *ElementFromHandle)(IUIAutomation *This,
        UIA_HWND hwnd, IUIAutomationElement **element);
    void *ElementFromPoint;
    void *GetFocusedElement;
    void *GetRootElementBuildCache;
    void *ElementFromHandleBuildCache;
    void *ElementFromPointBuildCache;
    HRESULT (STDMETHODCALLTYPE *GetFocusedElementBuildCache)(IUIAutomation *This,
        IUIAutomationCacheRequest *cacheRequest, IUIAutomationElement **element);
    void *CreateTreeWalker;
    void *get_ControlViewWalker;
    void *get_ContentViewWalker;
    void *get_RawViewWalker;
    void *get_RawViewCondition;
    void *get_ControlViewCondition;
    void *get_ContentViewCondition;
    HRESULT (STDMETHODCALLTYPE *CreateCacheRequest)(IUIAutomation *This,
        IUIAutomationCacheRequest **cacheRequest);
    void *CreateTrueCondition;
    void *CreateFalseCondition;
    HRESULT (STDMETHODCALLTYPE *CreatePropertyCondition)(IUIAutomation *This,
        int propertyId, VARIANT value, IUIAutomationCondition **newCondition);
    void *CreatePropertyConditionEx;
    void *CreateAndCondition;
    void *CreateAndConditionFromArray;
    HRESULT (STDMETHODCALLTYPE *CreateAndConditionFromNativeArray)(
        IUIAutomation *This, IUIAutomationCondition **conditions,
        int conditionCount, IUIAutomationCondition **newCondition);
    void *CreateOrCondition;
    void *CreateOrConditionFromArray;
    HRESULT (STDMETHODCALLTYPE *CreateOrConditionFromNativeArray)(
        IUIAutomation *This, IUIAutomationCondition **conditions,
        int conditionCount, IUIAutomationCondition **newCondition);
    void *CreateNotCondition;
    void *AddAutomationEventHandler;
    void *RemoveAutomationEventHandler;
    void *AddPropertyChangedEventHandlerNativeArray;
    void *AddPropertyChangedEventHandler;
    void *RemovePropertyChangedEventHandler;
    void *AddStructureChangedEventHandler;
    void *RemoveStructureChangedEventHandler;
    void *AddFocusChangedEventHandler;
    void *RemoveFocusChangedEventHandler;
    void *RemoveAllEventHandlers;
    void *IntNativeArrayToSafeArray;
    void *IntSafeArrayToNativeArray;
    void *RectToVariant;
    void *VariantToRect;
    void *SafeArrayToRectNativeArray;
    void *CreateProxyFactoryEntry;
    void *get_ProxyFactoryMapping;
    void *GetPropertyProgrammaticName;
    void *GetPatternProgrammaticName;
    void *PollForPotentialSupportedPatterns;
    void *PollForPotentialSupportedProperties;
    void *CheckNotSupported;
    void *get_ReservedNotSupportedValue;
    void *get_ReservedMixedAttributeValue;
    void *ElementFromIAccessible;
    void *ElementFromIAccessibleBuildCache;
} IUIAutomationVtbl;

typedef struct IUIAutomation2Vtbl {
    IUIAutomationVtbl base;
    void *get_AutoSetFocus;
    void *put_AutoSetFocus;
    void *get_ConnectionTimeout;
    HRESULT (STDMETHODCALLTYPE *put_ConnectionTimeout)(
        IUIAutomation2 *This, DWORD timeout);
    void *get_TransactionTimeout;
    HRESULT (STDMETHODCALLTYPE *put_TransactionTimeout)(
        IUIAutomation2 *This, DWORD timeout);
} IUIAutomation2Vtbl;

typedef struct IUIAutomationElementVtbl {
    void *QueryInterface;
    void *AddRef;
    ULONG (STDMETHODCALLTYPE *Release)(IUIAutomationElement *This);
    void *SetFocus;
    void *GetRuntimeId;
    void *FindFirst;
    void *FindAll;
    void *FindFirstBuildCache;
    HRESULT (STDMETHODCALLTYPE *FindAllBuildCache)(IUIAutomationElement *This,
        int scope, IUIAutomationCondition *condition,
        IUIAutomationCacheRequest *cacheRequest,
        IUIAutomationElementArray **found);
    void *BuildUpdatedCache;
    void *GetCurrentPropertyValue;
    void *GetCurrentPropertyValueEx;
    HRESULT (STDMETHODCALLTYPE *GetCachedPropertyValue)(
        IUIAutomationElement *This, int propertyId, VARIANT *retVal);
    void *GetCachedPropertyValueEx;
    void *GetCurrentPatternAs;
    void *GetCachedPatternAs;
    void *GetCurrentPattern;
    void *GetCachedPattern;
    void *GetCachedParent;
    void *GetCachedChildren;
    void *get_CurrentProcessId;
    void *get_CurrentControlType;
    void *get_CurrentLocalizedControlType;
    void *get_CurrentName;
    void *get_CurrentAcceleratorKey;
    void *get_CurrentAccessKey;
    void *get_CurrentHasKeyboardFocus;
    void *get_CurrentIsKeyboardFocusable;
    void *get_CurrentIsEnabled;
    void *get_CurrentAutomationId;
    void *get_CurrentClassName;
    void *get_CurrentHelpText;
    void *get_CurrentCulture;
    void *get_CurrentIsControlElement;
    void *get_CurrentIsContentElement;
    void *get_CurrentIsPassword;
    void *get_CurrentNativeWindowHandle;
    void *get_CurrentItemType;
    void *get_CurrentIsOffscreen;
    void *get_CurrentOrientation;
    void *get_CurrentFrameworkId;
    void *get_CurrentIsRequiredForForm;
    void *get_CurrentItemStatus;
    void *get_CurrentBoundingRectangle;
    void *get_CurrentLabeledBy;
    void *get_CurrentAriaRole;
    void *get_CurrentAriaProperties;
    void *get_CurrentIsDataValidForForm;
    void *get_CurrentControllerFor;
    void *get_CurrentDescribedBy;
    void *get_CurrentFlowsTo;
    void *get_CurrentProviderDescription;
    void *get_CachedProcessId;
    HRESULT (STDMETHODCALLTYPE *get_CachedControlType)(IUIAutomationElement *This,
        int *retVal);
    void *get_CachedLocalizedControlType;
    HRESULT (STDMETHODCALLTYPE *get_CachedName)(IUIAutomationElement *This,
        BSTR *retVal);
    void *get_CachedAcceleratorKey;
    void *get_CachedAccessKey;
    void *get_CachedHasKeyboardFocus;
    void *get_CachedIsKeyboardFocusable;
    void *get_CachedIsEnabled;
    void *get_CachedAutomationId;
    void *get_CachedClassName;
    void *get_CachedHelpText;
    void *get_CachedCulture;
    void *get_CachedIsControlElement;
    void *get_CachedIsContentElement;
    void *get_CachedIsPassword;
    void *get_CachedNativeWindowHandle;
    void *get_CachedItemType;
    void *get_CachedIsOffscreen;
    void *get_CachedOrientation;
    void *get_CachedFrameworkId;
    void *get_CachedIsRequiredForForm;
    void *get_CachedItemStatus;
    HRESULT (STDMETHODCALLTYPE *get_CachedBoundingRectangle)(
        IUIAutomationElement *This, RECT *retVal);
    void *get_CachedLabeledBy;
    void *get_CachedAriaRole;
    void *get_CachedAriaProperties;
    void *get_CachedIsDataValidForForm;
    void *get_CachedControllerFor;
    void *get_CachedDescribedBy;
    void *get_CachedFlowsTo;
    void *get_CachedProviderDescription;
    void *GetClickablePoint;
} IUIAutomationElementVtbl;

typedef struct IUIAutomationElementArrayVtbl {
    void *QueryInterface;
    void *AddRef;
    ULONG (STDMETHODCALLTYPE *Release)(IUIAutomationElementArray *This);
    HRESULT (STDMETHODCALLTYPE *get_Length)(IUIAutomationElementArray *This,
        int *length);
    HRESULT (STDMETHODCALLTYPE *GetElement)(IUIAutomationElementArray *This,
        int index, IUIAutomationElement **element);
} IUIAutomationElementArrayVtbl;

typedef struct IUIAutomationCacheRequestVtbl {
    void *QueryInterface;
    void *AddRef;
    ULONG (STDMETHODCALLTYPE *Release)(IUIAutomationCacheRequest *This);
    HRESULT (STDMETHODCALLTYPE *AddProperty)(IUIAutomationCacheRequest *This,
        int propertyId);
    void *AddPattern;
    void *Clone;
    void *get_TreeScope;
    void *put_TreeScope;
    void *get_TreeFilter;
    void *put_TreeFilter;
    void *get_AutomationElementMode;
    HRESULT (STDMETHODCALLTYPE *put_AutomationElementMode)(
        IUIAutomationCacheRequest *This, int mode);
} IUIAutomationCacheRequestVtbl;

typedef struct IUIAutomationConditionVtbl {
    void *QueryInterface;
    void *AddRef;
    ULONG (STDMETHODCALLTYPE *Release)(IUIAutomationCondition *This);
} IUIAutomationConditionVtbl;

struct IUIAutomation { IUIAutomationVtbl *lpVtbl; };
struct IUIAutomation2 { IUIAutomation2Vtbl *lpVtbl; };
struct IUIAutomationElement { IUIAutomationElementVtbl *lpVtbl; };
struct IUIAutomationElementArray { IUIAutomationElementArrayVtbl *lpVtbl; };
struct IUIAutomationCacheRequest { IUIAutomationCacheRequestVtbl *lpVtbl; };
struct IUIAutomationCondition { IUIAutomationConditionVtbl *lpVtbl; };

_Static_assert(sizeof(IUIAutomationVtbl) == 58 * sizeof(void *),
               "IUIAutomation vtable size");
_Static_assert(sizeof(IUIAutomation2Vtbl) == 64 * sizeof(void *),
               "IUIAutomation2 vtable size");
_Static_assert(sizeof(IUIAutomationElementVtbl) == 85 * sizeof(void *),
               "IUIAutomationElement vtable size");
_Static_assert(sizeof(IUIAutomationElementArrayVtbl) == 5 * sizeof(void *),
               "IUIAutomationElementArray vtable size");
_Static_assert(sizeof(IUIAutomationCacheRequestVtbl) == 12 * sizeof(void *),
               "IUIAutomationCacheRequest vtable size");
_Static_assert(sizeof(IUIAutomationConditionVtbl) == 3 * sizeof(void *),
               "IUIAutomationCondition vtable size");

static const GUID g_clsid_uia8 = {
    0xe22ad333, 0xb25f, 0x460c,
    { 0x83, 0xd0, 0x05, 0x81, 0x10, 0x73, 0x95, 0xc9 }
};
static const GUID g_clsid_uia = {
    0xff48dba4, 0x60ef, 0x4201,
    { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e }
};
static const GUID g_iid_uia = {
    0x30cbe57d, 0xd9d0, 0x452a,
    { 0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee }
};
static const GUID g_iid_uia2 = {
    0x34723aff, 0x0c9d, 0x49d0,
    { 0x98, 0x96, 0x7a, 0xb5, 0x2d, 0xf8, 0xcd, 0x8a }
};

typedef struct {
    IUIAutomation *automation;
    IUIAutomationCondition *condition;
    IUIAutomationCacheRequest *cache;
    BOOL with_names;
} UiaEngine;

typedef struct {
    ClickTarget target;
    int control_type;
    BOOL actionable;
    WCHAR name[128];
} ScanTarget;

typedef struct {
    ScanTarget *items;
    int count;
    int cap;
    BOOL fg_done;               // uia_scan_monitor: foreground window merged
} ScanResult;

typedef struct {
    HWND hwnd;
    const WCHAR *cls;
    const RECT *rect;
    const WCHAR *skip;
    int added;
    int changed;
    int total;
    DWORD elapsed_ms;
    BOOL foreground;
    const ScanTarget *targets;
} WindowEvent;

typedef void (*UiaWindowCallback)(void *user, const WindowEvent *event);
typedef BOOL (*UiaContinueCallback)(void *user);
typedef HRESULT (WINAPI *DwmGetWindowAttributeFn)(HWND, DWORD, PVOID, DWORD);

static DwmGetWindowAttributeFn dwm_get_window_attribute(void)
{
    static DwmGetWindowAttributeFn fn;
    static BOOL tried;

    if (!tried) {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm)
            fn = (DwmGetWindowAttributeFn)(void *)GetProcAddress(
                dwm, "DwmGetWindowAttribute");
        tried = TRUE;
    }
    return fn;
}

static LONG64 rect_area(const RECT *r)
{
    LONG64 w = (LONG64)r->right - r->left;
    LONG64 h = (LONG64)r->bottom - r->top;
    if (w <= 0 || h <= 0)
        return 0;
    return w * h;
}

static LONG64 rect_intersection_area(const RECT *a, const RECT *b)
{
    RECT r;
    if (!IntersectRect(&r, a, b))
        return 0;
    return rect_area(&r);
}

static BOOL click_points_close(POINT a, POINT b)
{
    LONG64 dx = (LONG64)a.x - b.x;
    LONG64 dy = (LONG64)a.y - b.y;
    return dx * dx + dy * dy <= 36;
}

static BOOL rects_duplicate(const RECT *a, POINT click_a,
                            const RECT *b, POINT click_b)
{
    LONG64 inter = rect_intersection_area(a, b);
    LONG64 uni = rect_area(a) + rect_area(b) - inter;

    if (inter > 0 && uni > 0 && inter * 10 > uni * 7)
        return TRUE;
    return click_points_close(click_a, click_b);
}

static BOOL cached_bool_property(IUIAutomationElement *el, int property_id)
{
    VARIANT v;
    BOOL value = FALSE;

    VariantInit(&v);
    if (SUCCEEDED(el->lpVtbl->GetCachedPropertyValue(el, property_id, &v)) &&
        v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE)
        value = TRUE;
    VariantClear(&v);
    return value;
}

// List, table and tree items are fetched by type alone. Without a pattern
// they are only the box around a link or button (a web li or td); every
// other candidate is clickable in its own right.
static BOOL element_actionable(IUIAutomationElement *el, int control_type)
{
    if (control_type != UIA_ListItemControlTypeId &&
        control_type != UIA_DataItemControlTypeId &&
        control_type != UIA_TreeItemControlTypeId &&
        control_type != UIA_HeaderItemControlTypeId)
        return TRUE;
    return cached_bool_property(el, UIA_IsInvokePatternAvailablePropertyId) ||
           cached_bool_property(el, UIA_IsTogglePatternAvailablePropertyId) ||
           cached_bool_property(
               el, UIA_IsSelectionItemPatternAvailablePropertyId) ||
           cached_bool_property(
               el, UIA_IsExpandCollapsePatternAvailablePropertyId);
}

typedef struct {
    HWND hwnd;
    RECT rect;
    RECT clip;
    WCHAR cls[64];
    const WCHAR *base_skip;
    const WCHAR *scan_skip;
    BOOL do_scan;
    BOOL is_fg;
    BOOL is_taskbar;
    int z;
} WinInfo;

static BOOL target_add(ScanResult *result, const ScanTarget *target)
{
    LONG64 target_area = rect_area(&target->target.rc);
    int i;

    for (i = 0; i < result->count; ++i) {
        ScanTarget *old = &result->items[i];
        if (!rects_duplicate(&old->target.rc, old->target.click,
                             &target->target.rc, target->target.click))
            continue;
        if (target_area < rect_area(&old->target.rc)) {
            *old = *target;
            return TRUE;
        }
        return FALSE;
    }

    if (result->count >= UIA_MAX_TARGETS)
        return FALSE;
    if (result->count == result->cap) {
        int cap = result->cap ? result->cap * 2 : 64;
        ScanTarget *items;
        if (cap > UIA_MAX_TARGETS)
            cap = UIA_MAX_TARGETS;
        items = (ScanTarget *)realloc(result->items,
                                      (size_t)cap * sizeof(*items));
        if (!items)
            return FALSE;
        result->items = items;
        result->cap = cap;
    }
    result->items[result->count++] = *target;
    return TRUE;
}

// zrects: bounds of the scanned-or-skipped visible windows, top of the
// z-order first; z: index of the element's own window in that array.
static BOOL point_visible(const RECT *zrects, int z, POINT pt)
{
    int i;
    for (i = 0; i < z; ++i) {
        if (PtInRect(&zrects[i], pt))
            return FALSE;
    }
    return TRUE;
}

static POINT choose_click_point(const RECT *zrects, int z, const RECT *rc)
{
    LONG w = rc->right - rc->left;
    LONG h = rc->bottom - rc->top;
    POINT pts[5];
    int i;

    pts[0].x = rc->left + w / 2;
    pts[0].y = rc->top + h / 2;
    pts[1].x = rc->left + w / 4;
    pts[1].y = rc->top + h / 4;
    pts[2].x = rc->left + (3 * w) / 4;
    pts[2].y = rc->top + h / 4;
    pts[3].x = rc->left + w / 4;
    pts[3].y = rc->top + (3 * h) / 4;
    pts[4].x = rc->left + (3 * w) / 4;
    pts[4].y = rc->top + (3 * h) / 4;

    for (i = 0; i < 5; ++i) {
        if (PtInRect(rc, pts[i]) && point_visible(zrects, z, pts[i]))
            return pts[i];
    }
    pts[0].x = -1;
    pts[0].y = -1;
    return pts[0];
}

static BOOL inside_any(const RECT *rects, int count, const RECT *rc)
{
    int i;
    for (i = 0; i < count; ++i) {
        if (rc->left >= rects[i].left && rc->right <= rects[i].right &&
            rc->top >= rects[i].top && rc->bottom <= rects[i].bottom)
            return TRUE;
    }
    return FALSE;
}

// A kept clickable card must not click its own inner targets (title, menu
// button), so try points that stay on the free part of the card. matches
// holds every smaller target whose click point is on the card.
static POINT choose_outer_click_point(const ScanTarget *cands,
                                      const int *matches, int match_count,
                                      int self, const RECT *zrects, int z)
{
    const RECT *rc = &cands[self].target.rc;
    LONG w = rc->right - rc->left;
    LONG h = rc->bottom - rc->top;
    POINT pts[9];
    int i, m;

    pts[0].x = rc->left + w / 2;
    pts[0].y = rc->top + h / 2;
    pts[1].x = rc->left + w / 4;
    pts[1].y = rc->top + h / 4;
    pts[2].x = rc->left + (3 * w) / 4;
    pts[2].y = rc->top + h / 4;
    pts[3].x = rc->left + w / 4;
    pts[3].y = rc->top + (3 * h) / 4;
    pts[4].x = rc->left + (3 * w) / 4;
    pts[4].y = rc->top + (3 * h) / 4;
    pts[5].x = rc->left + w / 2;
    pts[5].y = rc->top + h / 4;
    pts[6].x = rc->left + w / 2;
    pts[6].y = rc->top + (3 * h) / 4;
    pts[7].x = rc->left + w / 4;
    pts[7].y = rc->top + h / 2;
    pts[8].x = rc->left + (3 * w) / 4;
    pts[8].y = rc->top + h / 2;

    for (i = 0; i < 9; ++i) {
        BOOL clear = TRUE;
        if (!PtInRect(rc, pts[i]) || !point_visible(zrects, z, pts[i]))
            continue;
        for (m = 0; m < match_count; ++m) {
            int j = matches[m];
            if (PtInRect(&cands[j].target.rc, pts[i])) {
                clear = FALSE;
                break;
            }
        }
        if (clear)
            return pts[i];
    }
    return cands[self].target.click;
}

// Uniform grid of click points. The container pass only cares about smaller
// targets whose click point is inside a candidate, so a leaf visits the few
// cells under its own rect instead of every other candidate.
typedef struct {
    int cols;
    int rows;
    LONG64 min_x;
    LONG64 min_y;
    LONG64 max_x;
    LONG64 max_y;
    LONG64 cell_w;
    LONG64 cell_h;
    int *start;
    int *order;
} ClickGrid;

static int click_grid_cell(const ClickGrid *grid, POINT pt)
{
    int cx = (int)(((LONG64)pt.x - grid->min_x) / grid->cell_w);
    int cy = (int)(((LONG64)pt.y - grid->min_y) / grid->cell_h);

    if (cx < 0)
        cx = 0;
    else if (cx >= grid->cols)
        cx = grid->cols - 1;
    if (cy < 0)
        cy = 0;
    else if (cy >= grid->rows)
        cy = grid->rows - 1;
    return cy * grid->cols + cx;
}

static BOOL click_grid_build(const ScanTarget *cands, int count,
                             ClickGrid *grid)
{
    LONG64 width, height;
    double aspect, side;
    int i, cols, rows, cells;
    int *start = NULL;
    int *cursor = NULL;
    int *order = NULL;

    grid->start = NULL;
    grid->order = NULL;
    if (count <= 0)
        return FALSE;

    grid->min_x = grid->max_x = cands[0].target.click.x;
    grid->min_y = grid->max_y = cands[0].target.click.y;
    for (i = 1; i < count; ++i) {
        LONG64 x = cands[i].target.click.x;
        LONG64 y = cands[i].target.click.y;
        if (x < grid->min_x)
            grid->min_x = x;
        if (x > grid->max_x)
            grid->max_x = x;
        if (y < grid->min_y)
            grid->min_y = y;
        if (y > grid->max_y)
            grid->max_y = y;
    }

    width = grid->max_x - grid->min_x + 1;
    height = grid->max_y - grid->min_y + 1;
    aspect = (double)width / (double)height;
    side = sqrt((double)count * aspect);
    if (side < 1.0)
        side = 1.0;
    if (side > (double)count)
        side = (double)count;
    cols = (int)(side + 0.5);
    if (cols < 1)
        cols = 1;
    if (cols > count)
        cols = count;
    rows = (count + cols - 1) / cols;
    if (rows < 1)
        rows = 1;

    grid->cols = cols;
    grid->rows = rows;
    grid->cell_w = (width + cols - 1) / cols;
    grid->cell_h = (height + rows - 1) / rows;
    if (grid->cell_w < 1)
        grid->cell_w = 1;
    if (grid->cell_h < 1)
        grid->cell_h = 1;

    cells = cols * rows;
    start = (int *)calloc((size_t)cells + 1, sizeof(*start));
    cursor = (int *)malloc((size_t)cells * sizeof(*cursor));
    order = (int *)malloc((size_t)count * sizeof(*order));
    if (!start || !cursor || !order) {
        free(start);
        free(cursor);
        free(order);
        return FALSE;
    }

    for (i = 0; i < count; ++i)
        ++start[click_grid_cell(grid, cands[i].target.click) + 1];
    for (i = 1; i <= cells; ++i)
        start[i] += start[i - 1];
    memcpy(cursor, start, (size_t)cells * sizeof(*cursor));
    for (i = 0; i < count; ++i)
        order[cursor[click_grid_cell(grid, cands[i].target.click)]++] = i;

    free(cursor);
    grid->start = start;
    grid->order = order;
    return TRUE;
}

static int compare_indices(const void *a, const void *b)
{
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);
}

// Decides which candidates are containers and picks the click point for the
// ones that are kept. Candidates are unchanged; drop and new_click receive
// one entry per candidate, in index order.
static void filter_containers(const ScanTarget *cands, int count,
                              const RECT *zrects, int z,
                              void (*debug)(const RECT *, int, const WCHAR *),
                              BOOL *drop, POINT *new_click)
{
    ClickGrid grid;
    LONG64 *areas;
    int *matches;
    int i;

    for (i = 0; i < count; ++i) {
        drop[i] = FALSE;
        new_click[i] = cands[i].target.click;
    }
    if (count < 2)
        return;

    areas = (LONG64 *)malloc((size_t)count * sizeof(*areas));
    matches = (int *)malloc((size_t)count * sizeof(*matches));
    if (!areas || !matches || !click_grid_build(cands, count, &grid)) {
        free(areas);
        free(matches);
        return;
    }
    for (i = 0; i < count; ++i)
        areas[i] = rect_area(&cands[i].target.rc);

    for (i = 0; i < count; ++i) {
        const RECT *rc = &cands[i].target.rc;
        LONG64 area = areas[i];
        LONG64 covered = 0;
        LONG64 qx0, qx1, qy0, qy1;
        int inner = 0, first = -1, matched = 0, m;
        int cx, cy, cx0, cx1, cy0, cy1;

        qx0 = rc->left;
        if (qx0 < grid.min_x)
            qx0 = grid.min_x;
        qx1 = rc->right;
        if (qx1 > grid.max_x + 1)
            qx1 = grid.max_x + 1;
        qy0 = rc->top;
        if (qy0 < grid.min_y)
            qy0 = grid.min_y;
        qy1 = rc->bottom;
        if (qy1 > grid.max_y + 1)
            qy1 = grid.max_y + 1;

        if (qx1 > qx0 && qy1 > qy0) {
            cx0 = (int)((qx0 - grid.min_x) / grid.cell_w);
            cx1 = (int)((qx1 - 1 - grid.min_x) / grid.cell_w);
            cy0 = (int)((qy0 - grid.min_y) / grid.cell_h);
            cy1 = (int)((qy1 - 1 - grid.min_y) / grid.cell_h);
            for (cy = cy0; cy <= cy1; ++cy) {
                for (cx = cx0; cx <= cx1; ++cx) {
                    int cell = cy * grid.cols + cx;
                    int p;
                    for (p = grid.start[cell]; p < grid.start[cell + 1];
                         ++p) {
                        int j = grid.order[p];
                        if (j == i || areas[j] >= area ||
                            !PtInRect(rc, cands[j].target.click))
                            continue;
                        matches[matched++] = j;
                    }
                }
            }
        }
        if (matched > 1)
            qsort(matches, (size_t)matched, sizeof(*matches),
                  compare_indices);

        for (m = 0; m < matched; ++m) {
            const ClickTarget *t = &cands[matches[m]].target;
            if (first >= 0 &&
                rects_duplicate(&cands[first].target.rc,
                                cands[first].target.click, &t->rc, t->click))
                continue;
            if (first < 0)
                first = matches[m];
            ++inner;
            covered += rect_intersection_area(&t->rc, rc);
        }

        if (inner == 0)
            drop[i] = FALSE;
        else if (cands[i].actionable)
            drop[i] = !(inner == 1 || covered * 2 < area);
        else
            drop[i] = !(inner == 1 && covered * 4 < area);

        new_click[i] = cands[i].target.click;
        if (!drop[i] && inner > 0)
            new_click[i] = choose_outer_click_point(
                cands, matches, matched, i, zrects, z);
        if (drop[i] && debug)
            debug(rc, cands[i].control_type,
                  inner >= 2 ? L"container" : L"wrapper");
    }

    free(matches);
    free(areas);
    free(grid.order);
    free(grid.start);
}

// Windows that answer UI Automation very slowly with next to nothing (games,
// custom-drawn UIs) are skipped for a while, so the hints fall back to the
// grid at once instead of stalling for seconds on every open. They are
// remembered per window: explorer.exe runs every folder window, the taskbar
// and the desktop, and one slow window must not turn off the others.
#define SLOW_SCAN_MS       600
#define SLOW_SCAN_TARGETS  4
#define SLOW_SKIP_MS       (10 * 60 * 1000)
#define SLOW_MAX           16

static struct { HWND hwnd; DWORD pid; DWORD since; } g_slow[SLOW_MAX];
static volatile LONG g_slow_lock;
static BOOL g_slow_memory = TRUE;   // the probe turns it off to report truthfully

// The pid guards against a reused window handle.
static BOOL slow_check(HWND hwnd, DWORD pid, BOOL remember)
{
    DWORD now = GetTickCount();
    BOOL hit = FALSE;
    int i, free_slot = -1, oldest = 0;

    if (!g_slow_memory || !hwnd || !pid)
        return FALSE;
    while (InterlockedExchange(&g_slow_lock, 1))
        Sleep(0);
    for (i = 0; i < SLOW_MAX; ++i) {
        if (g_slow[i].hwnd && now - g_slow[i].since > SLOW_SKIP_MS)
            g_slow[i].hwnd = NULL;
        if (g_slow[i].hwnd == hwnd && g_slow[i].pid == pid)
            hit = TRUE;
        else if (!g_slow[i].hwnd && free_slot < 0)
            free_slot = i;
        if (now - g_slow[i].since > now - g_slow[oldest].since)
            oldest = i;
    }
    if (remember && !hit) {
        i = free_slot >= 0 ? free_slot : oldest;
        g_slow[i].hwnd = hwnd;
        g_slow[i].pid = pid;
        g_slow[i].since = now;
    }
    InterlockedExchange(&g_slow_lock, 0);
    return hit;
}

void uia_forget_slow(void)
{
    while (InterlockedExchange(&g_slow_lock, 1))
        Sleep(0);
    memset(g_slow, 0, sizeof(g_slow));
    InterlockedExchange(&g_slow_lock, 0);
}

static BOOL CALLBACK find_chromium_child(HWND hwnd, LPARAM lp)
{
    WCHAR cls[64];
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"Chrome_RenderWidgetHostHWND") == 0 ||
        wcsncmp(cls, L"Chrome_WidgetWin_", 17) == 0) {
        *(BOOL *)lp = TRUE;
        return FALSE;
    }
    return TRUE;
}

// Browsers and Electron apps build their tree lazily: a slow, nearly empty
// first scan there is normal and must not get them skipped. So does Explorer
// while a folder window it has just opened fills in. WebView2 and CEF apps
// (the new Teams) host Chromium under a top-level window of their own class.
static BOOL lazy_tree_window(HWND hwnd)
{
    WCHAR cls[64];
    BOOL chromium = FALSE;

    GetClassNameW(hwnd, cls, 64);
    if (wcsncmp(cls, L"Chrome_WidgetWin_", 17) == 0 ||
        wcscmp(cls, L"MozillaWindowClass") == 0 ||
        wcscmp(cls, L"CabinetWClass") == 0 ||
        wcscmp(cls, L"ExploreWClass") == 0)
        return TRUE;
    EnumChildWindows(hwnd, find_chromium_child, (LPARAM)&chromium);
    return chromium;
}

// Optional observer for candidates scan_window_uia rejects (hint_check.c).
// NULL in the app, so the only cost there is the pointer test.
static void (*g_scan_debug)(const RECT *rc, int control_type,
                            const WCHAR *reason);

static void scan_window_uia(UiaEngine *engine, HWND hwnd,
                            const RECT *zrects, int z, const RECT *monitor,
                            ScanResult *out, int *answered);

// "Next to nothing" counts every element the window answered with, not the
// targets kept: a window mostly covered by another keeps only a few. A scan
// dropped before it ended (Alt went on to scroll, not to the hint hotkey)
// ran while the app was busy, so it is no verdict.
static void scan_window(UiaEngine *engine, HWND hwnd, const RECT *zrects,
                        int z, const RECT *monitor, ScanResult *out,
                        UiaContinueCallback keep_going, void *keep_user)
{
    DWORD pid = 0, started;
    int answered = 0;

    GetWindowThreadProcessId(hwnd, &pid);
    if (slow_check(hwnd, pid, FALSE)) {
        out->items = NULL;
        out->count = 0;
        out->cap = 0;
        return;
    }
    started = GetTickCount();
    scan_window_uia(engine, hwnd, zrects, z, monitor, out, &answered);
    if (GetTickCount() - started > SLOW_SCAN_MS &&
        answered <= SLOW_SCAN_TARGETS &&
        (!keep_going || keep_going(keep_user)) && !lazy_tree_window(hwnd))
        slow_check(hwnd, pid, TRUE);
}

static void scan_window_uia(UiaEngine *engine, HWND hwnd,
                            const RECT *zrects, int z, const RECT *monitor,
                            ScanResult *out, int *answered)
{
    // Scroll bars and their arrow/page buttons are not worth a label.
    RECT scrollbars[64];
    int scrollbar_count = 0;
    RECT documents[16];
    int document_count = 0;
    ScanTarget *cands = NULL;
    int cand_count = 0, cand_cap = 0;
    int kept = 0;
    BOOL *drop;
    POINT *new_click;
    IUIAutomationElement *root = NULL;
    IUIAutomationElementArray *array = NULL;
    int length = 0;
    int i;

    out->items = NULL;
    out->count = 0;
    out->cap = 0;
    *answered = 0;

    if (FAILED(engine->automation->lpVtbl->ElementFromHandle(
            engine->automation, (UIA_HWND)hwnd, &root)) || !root)
        return;

    if (FAILED(root->lpVtbl->FindAllBuildCache(
            root, UIA_TREESCOPE_DESCENDANTS, engine->condition,
            engine->cache, &array)) || !array) {
        root->lpVtbl->Release(root);
        return;
    }
    root->lpVtbl->Release(root);

    if (FAILED(array->lpVtbl->get_Length(array, &length)) || length < 0)
        length = 0;
    *answered = length;

    for (i = 0; i < length; ++i) {
        IUIAutomationElement *el = NULL;
        int control_type = 0;
        if (FAILED(array->lpVtbl->GetElement(array, i, &el)) || !el)
            continue;
        if (SUCCEEDED(el->lpVtbl->get_CachedControlType(el, &control_type))) {
            if (control_type == UIA_ScrollBarControlTypeId &&
                scrollbar_count < 64 &&
                SUCCEEDED(el->lpVtbl->get_CachedBoundingRectangle(
                    el, &scrollbars[scrollbar_count])))
                ++scrollbar_count;
            else if (control_type == UIA_DocumentControlTypeId &&
                     document_count < 16 &&
                     SUCCEEDED(el->lpVtbl->get_CachedBoundingRectangle(
                         el, &documents[document_count])))
                ++document_count;
        }
        el->lpVtbl->Release(el);
    }

    // A frame's Document is unclipped too and can reach past its page.
    // FindAll lists elements in tree order, so an earlier Document it
    // overlaps is the page around it.
    for (i = 1; i < document_count; ++i) {
        int d;
        for (d = 0; d < i; ++d) {
            RECT inter;
            if (IntersectRect(&inter, &documents[i], &documents[d]))
                documents[i] = inter;
        }
    }

    for (i = 0; i < length; ++i) {
        IUIAutomationElement *el = NULL;
        RECT rc;
        RECT clip;
        POINT click;
        int control_type = 0;
        BSTR name = NULL;
        ScanTarget target;
        LONG64 orig_area;
        LONG64 clip_area;
        LONG64 monitor_area;

        if (FAILED(array->lpVtbl->GetElement(array, i, &el)) || !el)
            continue;

        if (FAILED(el->lpVtbl->get_CachedBoundingRectangle(el, &rc)) ||
            FAILED(el->lpVtbl->get_CachedControlType(el, &control_type))) {
            el->lpVtbl->Release(el);
            continue;
        }

        if (control_type == UIA_DocumentControlTypeId) {
            if (g_scan_debug)
                g_scan_debug(&rc, control_type, L"document");
            el->lpVtbl->Release(el);
            continue;
        }

        orig_area = rect_area(&rc);
        if (orig_area <= 0) {
            if (g_scan_debug)
                g_scan_debug(&rc, control_type, L"zero area");
            el->lpVtbl->Release(el);
            continue;
        }
        if (control_type == UIA_ScrollBarControlTypeId ||
            inside_any(scrollbars, scrollbar_count, &rc)) {
            if (g_scan_debug)
                g_scan_debug(&rc, control_type, L"scrollbar");
            el->lpVtbl->Release(el);
            continue;
        }
        if (!IntersectRect(&clip, &rc, monitor)) {
            if (g_scan_debug)
                g_scan_debug(&rc, control_type, L"off monitor");
            el->lpVtbl->Release(el);
            continue;
        }

        clip_area = rect_area(&clip);
        monitor_area = rect_area(monitor);
        if (clip_area * 2 < orig_area) {
            if (g_scan_debug)
                g_scan_debug(&clip, control_type, L"mostly off monitor");
            el->lpVtbl->Release(el);
            continue;
        }

        // Chrome reports unclipped page rects: clip page elements to the
        // Document so a click point cannot land on the browser toolbar.
        {
            const RECT *best_doc = NULL;
            LONG64 best_area = 0;
            int d;
            for (d = 0; d < document_count; ++d) {
                RECT doc_mon;
                RECT inter;
                LONG64 area;
                if (!IntersectRect(&doc_mon, &documents[d], monitor) ||
                    !IntersectRect(&inter, &rc, &documents[d]))
                    continue;
                area = rect_area(&doc_mon);
                if (area > best_area) {
                    best_area = area;
                    best_doc = &documents[d];
                }
            }
            if (best_doc)
                IntersectRect(&clip, &clip, best_doc);
        }

        clip_area = rect_area(&clip);
        if (clip.right - clip.left < 4 || clip.bottom - clip.top < 4) {
            if (g_scan_debug)
                g_scan_debug(&clip, control_type, L"too small");
            el->lpVtbl->Release(el);
            continue;
        }
        if (monitor_area > 0 && clip_area * 100 > monitor_area * 40) {
            if (g_scan_debug)
                g_scan_debug(&clip, control_type, L"too large");
            el->lpVtbl->Release(el);
            continue;
        }

        click = choose_click_point(zrects, z, &clip);
        if (click.x < 0 && click.y < 0) {
            if (g_scan_debug)
                g_scan_debug(&clip, control_type, L"hidden");
            el->lpVtbl->Release(el);
            continue;
        }

        target.actionable = element_actionable(el, control_type);
        target.name[0] = 0;
        if (engine->with_names) {
            if (SUCCEEDED(el->lpVtbl->get_CachedName(el, &name)) && name)
                lstrcpynW(target.name, name, 128);
            if (name)
                SysFreeString(name);
        }
        el->lpVtbl->Release(el);

        target.target.rc = clip;
        target.target.click = click;
        target.control_type = control_type;
        if (cand_count == cand_cap) {
            int cap = cand_cap ? cand_cap * 2 : 64;
            ScanTarget *tmp = (ScanTarget *)realloc(cands,
                                                    (size_t)cap * sizeof(*tmp));
            if (!tmp)
                break;
            cands = tmp;
            cand_cap = cap;
        }
        cands[cand_count++] = target;
    }
    array->lpVtbl->Release(array);

    // An element holding the click points of smaller targets is a container
    // (toolbar strip, row group), not something to click. A plain row or cell
    // (li/td) around its whole link is not worth a second label at the same
    // spot, but an actionable card can be: YouTube's video card exposes
    // Invoke and holds the title link and menu button, while the thumbnail is
    // only clickable through the card. So keep inner == 0; for an actionable
    // element keep inner == 1, or inner >= 2 when enough of it is free; for a
    // plain list or table item keep only inner == 1 with mostly free space.
    // Smaller targets that duplicate each other count once: Google wraps each
    // result's menu button in a clickable element of the same size, and that
    // pair alone must not turn the result link into a container. Decisions and
    // new click points use the original points and are applied afterwards.
    drop = cand_count ? (BOOL *)calloc((size_t)cand_count, sizeof(*drop))
                      : NULL;
    new_click = cand_count ? (POINT *)malloc((size_t)cand_count *
                                             sizeof(*new_click))
                           : NULL;
    if (drop && new_click)
        filter_containers(cands, cand_count, zrects, z, g_scan_debug,
                          drop, new_click);
    for (i = 0; i < cand_count; ++i) {
        if (!drop || !drop[i]) {
            cands[kept] = cands[i];
            if (drop && new_click)
                cands[kept].target.click = new_click[i];
            ++kept;
        }
    }
    free(new_click);
    free(drop);
    out->items = cands;
    out->count = kept;
    out->cap = cand_cap;
}

typedef struct {
    RECT monitor;
    WinInfo *items;
    int count;
    int cap;
} EnumContext;

static BOOL append_window(EnumContext *ctx, const WinInfo *win)
{
    if (ctx->count == ctx->cap) {
        int cap = ctx->cap ? ctx->cap * 2 : 32;
        WinInfo *items = (WinInfo *)realloc(ctx->items,
                                            (size_t)cap * sizeof(*items));
        if (!items)
            return FALSE;
        ctx->items = items;
        ctx->cap = cap;
    }
    ctx->items[ctx->count++] = *win;
    return TRUE;
}

static BOOL CALLBACK enum_window_cb(HWND hwnd, LPARAM lp)
{
    EnumContext *ctx = (EnumContext *)lp;
    WinInfo win;
    DwmGetWindowAttributeFn dwm = dwm_get_window_attribute();
    RECT rect;
    DWORD cloaked = 0;
    DWORD pid = 0;
    LONG exstyle;
    BOOL have_rect = FALSE;

    memset(&win, 0, sizeof(win));
    win.hwnd = hwnd;
    win.z = -1;
    GetClassNameW(hwnd, win.cls, 64);

    if (!IsWindowVisible(hwnd)) {
        win.base_skip = L"invisible";
    } else if (IsIconic(hwnd)) {
        win.base_skip = L"minimized";
    } else if (dwm && SUCCEEDED(dwm(hwnd, DWMWA_CLOAKED, &cloaked,
                                   sizeof(cloaked))) && cloaked) {
        win.base_skip = L"cloaked";
    } else {
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) {
            win.base_skip = L"own process";
        } else {
            // Only layered + transparent windows are invisible to the mouse;
            // plenty of normal apps (Chrome, Electron) are just layered.
            exstyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
            if ((exstyle & (WS_EX_TRANSPARENT | WS_EX_LAYERED)) ==
                (WS_EX_TRANSPARENT | WS_EX_LAYERED)) {
                win.base_skip = L"click-through";
            }
        }
    }

    if (!win.base_skip) {
        if (dwm && SUCCEEDED(dwm(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect,
                                 sizeof(rect))) &&
            rect.right > rect.left && rect.bottom > rect.top)
            have_rect = TRUE;
        else if (GetWindowRect(hwnd, &rect) &&
                 rect.right > rect.left && rect.bottom > rect.top)
            have_rect = TRUE;
        if (!have_rect)
            win.base_skip = L"no bounds";
        else if (!IntersectRect(&win.clip, &rect, &ctx->monitor))
            win.base_skip = L"off monitor";
        else
            win.rect = rect;
    }

    if (!append_window(ctx, &win))
        return FALSE;
    return TRUE;
}

static const WCHAR *window_skip(const WinInfo *win)
{
    if (win->base_skip)
        return win->base_skip;
    if (win->scan_skip)
        return win->scan_skip;
    return L"not scanned";
}

static BOOL region_covers_rect(HRGN covered, const RECT *rc)
{
    HRGN rect_rgn = CreateRectRgnIndirect(rc);
    HRGN diff = CreateRectRgn(0, 0, 0, 0);
    RECT box;
    int type;
    BOOL covered_rect;

    if (!rect_rgn || !diff) {
        if (rect_rgn)
            DeleteObject(rect_rgn);
        if (diff)
            DeleteObject(diff);
        return FALSE;
    }
    type = CombineRgn(diff, rect_rgn, covered, RGN_DIFF);
    if (type == ERROR) {
        covered_rect = FALSE;
    } else {
        GetRgnBox(diff, &box);
        covered_rect = (box.right <= box.left || box.bottom <= box.top);
    }
    DeleteObject(rect_rgn);
    DeleteObject(diff);
    return covered_rect;
}

static void add_region_rect(HRGN covered, const RECT *rc)
{
    HRGN rect_rgn = CreateRectRgnIndirect(rc);
    if (rect_rgn) {
        CombineRgn(covered, covered, rect_rgn, RGN_OR);
        DeleteObject(rect_rgn);
    }
}

static int window_index(const WinInfo *wins, int count, HWND hwnd)
{
    int i;
    for (i = 0; i < count; ++i) {
        if (wins[i].hwnd == hwnd)
            return i;
    }
    return -1;
}

static void report_skip(UiaWindowCallback cb, void *cb_user,
                        const WinInfo *win, const ScanResult *result)
{
    WindowEvent event;
    if (!cb)
        return;
    memset(&event, 0, sizeof(event));
    event.hwnd = win->hwnd;
    event.cls = win->cls;
    event.rect = &win->rect;
    event.skip = window_skip(win);
    event.total = result->count;
    event.targets = result->items;
    cb(cb_user, &event);
}

typedef struct {
    HWND hwnd;
    int z;
    int win_index;              // caller-only; index into the enumerated WinInfo
    volatile LONG finished;     // set by the scanning thread before signaling
    LONG merged;                // caller-only
    DWORD elapsed_ms;
    ScanResult found;
} ScanTask;

typedef struct {
    UiaEngine *engine;
    RECT *zrects;
    int zcount;
    RECT monitor;
    UiaContinueCallback keep_going;
    void *keep_user;
    UINT keep_scan_id;          // keep_user's pointee; see uia_scan_monitor
    ScanTask *tasks;
    int task_count;
    volatile LONG next;
    volatile LONG refs;
    HANDLE done_event;          // auto-reset, signaled by every finished task
} ScanJob;

static HANDLE g_helper_threads[UIA_HELPER_THREADS];
static int g_helper_count;
static HANDLE g_job_sem;
static HANDLE g_pool_quit;
static ScanJob *g_job;
static CRITICAL_SECTION g_pool_cs;
static BOOL g_pool_cs_ready;

// Whoever drops the last reference frees the whole job, including the result
// arrays, the done event and the zrects. A helper finishing after the caller
// has already returned therefore still has everything it touches.
static void job_release_ref(ScanJob *job)
{
    int i;

    if (InterlockedDecrement(&job->refs) != 0)
        return;
    if (job->tasks) {
        for (i = 0; i < job->task_count; ++i)
            free(job->tasks[i].found.items);
        free(job->tasks);
    }
    if (job->done_event)
        CloseHandle(job->done_event);
    free(job->zrects);
    free(job);
}

static BOOL pool_is_stopping(void)
{
    return g_pool_quit &&
           WaitForSingleObject(g_pool_quit, 0) == WAIT_OBJECT_0;
}

static DWORD WINAPI uia_helper(void *arg)
{
    HRESULT hr;
    BOOL com_ready;
    HANDLE waits[2];
    (void)arg;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    com_ready = (hr == S_OK || hr == S_FALSE);

    for (;;) {
        ScanJob *job;
        DWORD wait;

        waits[0] = g_pool_quit;
        waits[1] = g_job_sem;
        wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wait != WAIT_OBJECT_0 + 1)
            break;

        EnterCriticalSection(&g_pool_cs);
        job = g_job;
        if (job)
            InterlockedIncrement(&job->refs);
        LeaveCriticalSection(&g_pool_cs);
        if (!job)
            continue;

        for (;;) {
            LONG index;
            ScanTask *task;
            DWORD started;

            if (pool_is_stopping())
                break;
            if (job->keep_going && !job->keep_going(job->keep_user))
                break;
            index = InterlockedIncrement(&job->next) - 1;
            if (index >= job->task_count)
                break;
            task = &job->tasks[index];
            if (pool_is_stopping() ||
                (job->keep_going && !job->keep_going(job->keep_user))) {
                InterlockedExchange(&task->finished, 1);
                SetEvent(job->done_event);
                continue;
            }
            started = GetTickCount();
            scan_window(job->engine, task->hwnd, job->zrects, task->z,
                        &job->monitor, &task->found, job->keep_going,
                        job->keep_user);
            task->elapsed_ms = GetTickCount() - started;
            InterlockedExchange(&task->finished, 1);
            SetEvent(job->done_event);
        }
        job_release_ref(job);
    }

    if (com_ready)
        CoUninitialize();
    return 0;
}

static BOOL uia_pool_start(void)
{
    int i;

    if (!g_pool_cs_ready) {
        InitializeCriticalSection(&g_pool_cs);
        g_pool_cs_ready = TRUE;
    }
    if (g_helper_count > 0)
        return TRUE;

    if (!g_pool_quit) {
        g_pool_quit = CreateEventW(NULL, TRUE, FALSE, NULL);
        g_job_sem = CreateSemaphoreW(NULL, 0, 0x7fffffff, NULL);
        if (!g_pool_quit || !g_job_sem) {
            if (g_job_sem) {
                CloseHandle(g_job_sem);
                g_job_sem = NULL;
            }
            if (g_pool_quit) {
                CloseHandle(g_pool_quit);
                g_pool_quit = NULL;
            }
            return FALSE;
        }
    }

    for (i = 0; i < UIA_HELPER_THREADS; ++i) {
        g_helper_threads[i] = CreateThread(NULL, 0, uia_helper, NULL, 0,
                                           NULL);
        if (!g_helper_threads[i])
            break;
        ++g_helper_count;
    }
    return g_helper_count > 0;
}

static void uia_pool_stop(void)
{
    int i;

    if (!g_pool_cs_ready)
        return;
    if (g_pool_quit)
        SetEvent(g_pool_quit);
    for (i = 0; i < g_helper_count; ++i) {
        if (g_helper_threads[i]) {
            WaitForSingleObject(g_helper_threads[i], INFINITE);
            CloseHandle(g_helper_threads[i]);
            g_helper_threads[i] = NULL;
        }
    }
    g_helper_count = 0;
    if (g_job_sem) {
        CloseHandle(g_job_sem);
        g_job_sem = NULL;
    }
    if (g_pool_quit) {
        CloseHandle(g_pool_quit);
        g_pool_quit = NULL;
    }
    DeleteCriticalSection(&g_pool_cs);
    g_pool_cs_ready = FALSE;
}

static void uia_scan_monitor(UiaEngine *engine, HMONITOR mon,
                             UiaContinueCallback keep_going, void *keep_user,
                             UiaWindowCallback cb, void *cb_user,
                             ScanResult *result)
{
    MONITORINFO mi;
    EnumContext ctx;
    HWND foreground;
    HWND fg_root;
    int *zlist = NULL;
    RECT *zrects = NULL;
    int zcount = 0;
    int zcap = 0;
    int fg_index = -1;
    int fg_task = -1;
    int *order = NULL;
    int *task_of_win = NULL;
    int order_count = 0;
    int task_count = 0;
    int merged_count = 0;
    ScanJob *job = NULL;
    int i;
    int k;
    HRGN covered;

    if (!g_pool_cs_ready)
        uia_pool_start();

    memset(result, 0, sizeof(*result));
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi))
        return;

    memset(&ctx, 0, sizeof(ctx));
    ctx.monitor = mi.rcMonitor;
    EnumWindows(enum_window_cb, (LPARAM)&ctx);
    if (!ctx.items)
        return;

    foreground = GetForegroundWindow();
    fg_root = foreground ? GetAncestor(foreground, GA_ROOT) : NULL;
    if (fg_root)
        fg_index = window_index(ctx.items, ctx.count, fg_root);

    for (i = 0; i < ctx.count; ++i) {
        WinInfo *win = &ctx.items[i];
        if (win->base_skip)
            continue;
        if (zcount == zcap) {
            int cap = zcap ? zcap * 2 : 32;
            int *tmp = (int *)realloc(zlist, (size_t)cap * sizeof(*tmp));
            if (!tmp)
                break;
            zlist = tmp;
            zcap = cap;
        }
        zlist[zcount++] = i;
        if (win->hwnd == fg_root)
            win->is_fg = TRUE;
        if (!lstrcmpiW(win->cls, L"Shell_TrayWnd") ||
            !lstrcmpiW(win->cls, L"Shell_SecondaryTrayWnd"))
            win->is_taskbar = TRUE;
    }

    zrects = (RECT *)malloc((size_t)(zcount ? zcount : 1) * sizeof(*zrects));
    if (!zrects)
        goto done;
    for (i = 0; i < zcount; ++i)
        zrects[i] = ctx.items[zlist[i]].rect;

    covered = CreateRectRgn(0, 0, 0, 0);
    for (i = 0; i < zcount; ++i) {
        WinInfo *win = &ctx.items[zlist[i]];
        win->z = i;
        if (g_cfg.scan_scope == SCAN_WINDOW &&
            !win->is_fg && !win->is_taskbar) {
            win->scan_skip = L"not in scan scope";
            continue;
        }
        if (covered && region_covers_rect(covered, &win->clip)) {
            win->scan_skip = L"covered";
            continue;
        }
        win->do_scan = TRUE;
        if (covered)
            add_region_rect(covered, &win->clip);
    }
    if (covered)
        DeleteObject(covered);

    // Deterministic order: foreground window first, then z-order.
    order = (int *)malloc((size_t)ctx.count * sizeof(*order));
    task_of_win = (int *)malloc((size_t)ctx.count * sizeof(*task_of_win));
    if (!order || !task_of_win)
        goto done;
    if (fg_index >= 0)
        order[order_count++] = fg_index;
    for (i = 0; i < ctx.count; ++i) {
        if (i != fg_index)
            order[order_count++] = i;
    }
    for (i = 0; i < ctx.count; ++i)
        task_of_win[i] = -1;
    for (k = 0; k < order_count; ++k) {
        if (ctx.items[order[k]].do_scan)
            task_of_win[order[k]] = task_count++;
    }
    if (fg_index >= 0)
        fg_task = task_of_win[fg_index];
    result->fg_done = (fg_task < 0);

    job = (ScanJob *)calloc(1, sizeof(*job));
    if (!job)
        goto done;
    job->refs = 1;
    job->engine = engine;
    job->zrects = zrects;
    zrects = NULL;              // owned by the job from here on
    job->zcount = zcount;
    job->monitor = mi.rcMonitor;
    job->keep_going = keep_going;
    // keep_user points into the worker's stack. Copy what the callback reads
    // (the scan id) into the job so a late helper never touches that stack.
    if (keep_going) {
        job->keep_scan_id = keep_user ? *(const UINT *)keep_user : 0;
        job->keep_user = &job->keep_scan_id;
    }
    job->task_count = task_count;
    job->done_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!job->done_event)
        goto done;
    if (task_count > 0) {
        job->tasks = (ScanTask *)calloc((size_t)task_count,
                                        sizeof(*job->tasks));
        if (!job->tasks)
            goto done;
        for (k = 0; k < order_count; ++k) {
            int ti = task_of_win[order[k]];
            if (ti >= 0) {
                job->tasks[ti].hwnd = ctx.items[order[k]].hwnd;
                job->tasks[ti].z = ctx.items[order[k]].z;
                job->tasks[ti].win_index = order[k];
            }
        }
    }

    EnterCriticalSection(&g_pool_cs);
    g_job = job;
    LeaveCriticalSection(&g_pool_cs);
    if (g_helper_count > 0 && g_job_sem)
        ReleaseSemaphore(g_job_sem, (LONG)g_helper_count, NULL);

    // Skips are independent of the scan order; report them up front.
    for (k = 0; k < order_count; ++k) {
        WinInfo *win = &ctx.items[order[k]];
        if (!win->do_scan)
            report_skip(cb, cb_user, win, result);
    }

    // Merge in completion order. Tasks that finished together are merged in
    // task order, so the foreground window (task 0) still goes first.
    for (;;) {
        int ti;

        if (keep_going && !keep_going(job->keep_user))
            break;
        if (pool_is_stopping())
            break;
        if (merged_count >= job->task_count)
            break;

        if (g_helper_count == 0) {
            LONG index;

            if (job->next >= job->task_count)
                break;
            index = InterlockedIncrement(&job->next) - 1;
            if (index < job->task_count) {
                ScanTask *task = &job->tasks[index];
                DWORD started = GetTickCount();
                scan_window(job->engine, task->hwnd, job->zrects, task->z,
                            &job->monitor, &task->found, job->keep_going,
                            job->keep_user);
                task->elapsed_ms = GetTickCount() - started;
                InterlockedExchange(&task->finished, 1);
            }
        } else {
            WaitForSingleObject(job->done_event, 20);
        }

        for (ti = 0; ti < job->task_count; ++ti) {
            ScanTask *task = &job->tasks[ti];
            WinInfo *win;
            int before;
            int changes = 0;
            int j;

            if (!InterlockedCompareExchange(&task->finished, 0, 0))
                continue;
            if (task->merged)
                continue;
            task->merged = 1;
            if (ti == fg_task)
                result->fg_done = TRUE;

            win = &ctx.items[task->win_index];
            before = result->count;
            for (j = 0; j < task->found.count; ++j) {
                if (target_add(result, &task->found.items[j]))
                    ++changes;
                if (result->count >= UIA_MAX_TARGETS)
                    break;
            }
            if (cb) {
                WindowEvent event;
                memset(&event, 0, sizeof(event));
                event.hwnd = win->hwnd;
                event.cls = win->cls;
                event.rect = &win->rect;
                event.added = result->count - before;
                event.changed = changes;
                event.total = result->count;
                event.elapsed_ms = task->elapsed_ms;
                event.foreground = (ti == fg_task);
                event.targets = result->items;
                cb(cb_user, &event);
            }
            ++merged_count;
        }
    }

    EnterCriticalSection(&g_pool_cs);
    if (g_job == job)
        g_job = NULL;
    LeaveCriticalSection(&g_pool_cs);
    job_release_ref(job);
    job = NULL;

done:
    if (job)
        job_release_ref(job);
    free(zrects);
    free(task_of_win);
    free(order);
    free(zlist);
    free(ctx.items);
}

static IUIAutomationCondition *property_condition(IUIAutomation *automation,
                                                  int property_id,
                                                  VARTYPE type, LONG value)
{
    VARIANT v;
    IUIAutomationCondition *condition = NULL;
    HRESULT hr;

    VariantInit(&v);
    v.vt = type;
    if (type == VT_BOOL)
        v.boolVal = (VARIANT_BOOL)(value ? -1 : 0);
    else
        v.lVal = value;
    hr = automation->lpVtbl->CreatePropertyCondition(
        automation, property_id, v, &condition);
    VariantClear(&v);
    if (FAILED(hr))
        return NULL;
    return condition;
}

static BOOL uia_build_condition(UiaEngine *engine)
{
    static const int types[] = {
        UIA_ButtonControlTypeId,
        UIA_CheckBoxControlTypeId,
        UIA_ComboBoxControlTypeId,
        UIA_EditControlTypeId,
        UIA_HyperlinkControlTypeId,
        UIA_ListItemControlTypeId,
        UIA_MenuItemControlTypeId,
        UIA_RadioButtonControlTypeId,
        UIA_TabItemControlTypeId,
        UIA_TreeItemControlTypeId,
        UIA_DataItemControlTypeId,
        UIA_SplitButtonControlTypeId,
        UIA_HeaderItemControlTypeId,
        UIA_ScrollBarControlTypeId,   // fetched only to drop its buttons
        UIA_DocumentControlTypeId     // fetched only to clip page elements
    };
    IUIAutomationCondition *off;
    IUIAutomationCondition *enabled;
    IUIAutomationCondition *items[32];
    IUIAutomationCondition *or_cond = NULL;
    IUIAutomationCondition *and_items[3];
    int count = 0;
    int i;
    BOOL ok = TRUE;
    HRESULT hr;

    off = property_condition(engine->automation, UIA_IsOffscreenPropertyId,
                             VT_BOOL, FALSE);
    enabled = property_condition(engine->automation, UIA_IsEnabledPropertyId,
                                 VT_BOOL, TRUE);
    if (!off || !enabled) {
        if (off)
            off->lpVtbl->Release(off);
        if (enabled)
            enabled->lpVtbl->Release(enabled);
        return FALSE;
    }

    for (i = 0; i < (int)(sizeof(types) / sizeof(types[0])); ++i) {
        IUIAutomationCondition *c = property_condition(
            engine->automation, UIA_ControlTypePropertyId, VT_I4, types[i]);
        if (!c) {
            ok = FALSE;
            break;
        }
        items[count++] = c;
    }
    if (ok) {
        static const int patterns[] = {
            UIA_IsInvokePatternAvailablePropertyId,
            UIA_IsTogglePatternAvailablePropertyId,
            UIA_IsSelectionItemPatternAvailablePropertyId,
            UIA_IsExpandCollapsePatternAvailablePropertyId
        };
        for (i = 0; i < (int)(sizeof(patterns) / sizeof(patterns[0])); ++i) {
            IUIAutomationCondition *c = property_condition(
                engine->automation, patterns[i], VT_BOOL, TRUE);
            if (!c) {
                ok = FALSE;
                break;
            }
            items[count++] = c;
        }
    }

    if (ok) {
        hr = engine->automation->lpVtbl->CreateOrConditionFromNativeArray(
            engine->automation, items, count, &or_cond);
        if (FAILED(hr) || !or_cond)
            ok = FALSE;
    }
    for (i = 0; i < count; ++i)
        items[i]->lpVtbl->Release(items[i]);

    if (ok) {
        and_items[0] = or_cond;
        and_items[1] = enabled;
        and_items[2] = off;
        hr = engine->automation->lpVtbl->CreateAndConditionFromNativeArray(
            engine->automation, and_items, 3, &engine->condition);
        if (FAILED(hr) || !engine->condition)
            ok = FALSE;
    }

    off->lpVtbl->Release(off);
    enabled->lpVtbl->Release(enabled);
    if (or_cond)
        or_cond->lpVtbl->Release(or_cond);
    return ok;
}

static BOOL uia_build_cache(UiaEngine *engine, BOOL with_names)
{
    HRESULT hr;

    hr = engine->automation->lpVtbl->CreateCacheRequest(
        engine->automation, &engine->cache);
    if (FAILED(hr) || !engine->cache)
        return FALSE;

    hr = engine->cache->lpVtbl->AddProperty(
        engine->cache, UIA_BoundingRectanglePropertyId);
    if (FAILED(hr))
        return FALSE;
    hr = engine->cache->lpVtbl->AddProperty(
        engine->cache, UIA_ControlTypePropertyId);
    if (FAILED(hr))
        return FALSE;
    hr = engine->cache->lpVtbl->AddProperty(
        engine->cache, UIA_IsInvokePatternAvailablePropertyId);
    if (FAILED(hr))
        return FALSE;
    hr = engine->cache->lpVtbl->AddProperty(
        engine->cache, UIA_IsTogglePatternAvailablePropertyId);
    if (FAILED(hr))
        return FALSE;
    hr = engine->cache->lpVtbl->AddProperty(
        engine->cache, UIA_IsSelectionItemPatternAvailablePropertyId);
    if (FAILED(hr))
        return FALSE;
    hr = engine->cache->lpVtbl->AddProperty(
        engine->cache, UIA_IsExpandCollapsePatternAvailablePropertyId);
    if (FAILED(hr))
        return FALSE;
    if (with_names) {
        hr = engine->cache->lpVtbl->AddProperty(
            engine->cache, UIA_NamePropertyId);
        if (FAILED(hr))
            return FALSE;
    }
    hr = engine->cache->lpVtbl->put_AutomationElementMode(
        engine->cache, UIA_MODE_NONE);
    return SUCCEEDED(hr);
}

static void uia_engine_cleanup(UiaEngine *engine)
{
    if (engine->cache)
        engine->cache->lpVtbl->Release(engine->cache);
    if (engine->condition)
        engine->condition->lpVtbl->Release(engine->condition);
    if (engine->automation)
        engine->automation->lpVtbl->Release(engine->automation);
    memset(engine, 0, sizeof(*engine));
}

// Two threads setting up UI Automation in this process at the same time can
// make one of them fail (the scan engine then never starts). Every engine
// (scan, scroll target, focus) is created under this lock.
static volatile LONG g_engine_lock;

static void engine_lock(void)
{
    while (InterlockedCompareExchange(&g_engine_lock, 1, 0) != 0)
        Sleep(1);
}

static void engine_unlock(void)
{
    InterlockedExchange(&g_engine_lock, 0);
}

static BOOL uia_engine_create(UiaEngine *engine, BOOL with_names)
{
    IUIAutomation2 *automation2 = NULL;
    HRESULT hr;

    memset(engine, 0, sizeof(*engine));
    engine->with_names = with_names;
    hr = CoCreateInstance(&g_clsid_uia8, NULL, CLSCTX_INPROC_SERVER,
                          &g_iid_uia, (void **)&engine->automation);
    if (FAILED(hr) || !engine->automation) {
        engine->automation = NULL;
        hr = CoCreateInstance(&g_clsid_uia, NULL, CLSCTX_INPROC_SERVER,
                              &g_iid_uia, (void **)&engine->automation);
    }
    if (FAILED(hr) || !engine->automation)
        return FALSE;

    hr = engine->automation->lpVtbl->QueryInterface(
        engine->automation, &g_iid_uia2, (void **)&automation2);
    if (SUCCEEDED(hr) && automation2) {
        automation2->lpVtbl->put_TransactionTimeout(
            automation2, UIA_SCAN_TIMEOUT_MS);
        automation2->lpVtbl->put_ConnectionTimeout(
            automation2, UIA_SCAN_TIMEOUT_MS);
        automation2->lpVtbl->base.Release((IUIAutomation *)automation2);
    }

    if (!uia_build_condition(engine) || !uia_build_cache(engine, with_names)) {
        uia_engine_cleanup(engine);
        return FALSE;
    }
    return TRUE;
}

static BOOL uia_engine_init(UiaEngine *engine, BOOL with_names)
{
    BOOL ok;

    engine_lock();
    ok = uia_engine_create(engine, with_names);
    engine_unlock();
    return ok;
}

typedef struct {
    UINT scan_id;
    HMONITOR mon;
} ScanRequest;

static HANDLE g_uia_thread;
static HANDLE g_uia_ready;
static HANDLE g_uia_wake;
static CRITICAL_SECTION g_uia_cs;
static BOOL g_uia_cs_ready;
static volatile LONG g_uia_quit;
static volatile LONG g_uia_available;
static ScanRequest g_uia_request;

static BOOL uia_still_current(UINT scan_id)
{
    BOOL current;
    if (InterlockedCompareExchange(&g_uia_quit, 0, 0))
        return FALSE;
    EnterCriticalSection(&g_uia_cs);
    current = (g_uia_request.scan_id == scan_id && scan_id != 0);
    LeaveCriticalSection(&g_uia_cs);
    return current;
}

static BOOL worker_keep_going(void *user)
{
    return uia_still_current(*(const UINT *)user);
}

static ClickList *make_click_list(UINT scan_id, BOOL done,
                                  const ScanResult *result)
{
    ClickList *list;
    size_t size;
    int i;

    size = sizeof(ClickList) + (size_t)result->count * sizeof(ClickTarget);
    list = (ClickList *)malloc(size);
    if (!list)
        return NULL;
    list->scan_id = scan_id;
    list->done = done;
    list->fg_done = done || result->fg_done;
    list->count = result->count;
    for (i = 0; i < result->count; ++i)
        list->items[i] = result->items[i].target;
    return list;
}

static void post_list(UINT scan_id, BOOL done, const ScanResult *result)
{
    ClickList *list;

    if (!g_main_wnd || !uia_still_current(scan_id))
        return;
    list = make_click_list(scan_id, done, result);
    if (!list)
        return;
    if (!PostMessageW(g_main_wnd, WM_APP_ELEMENTS, 0, (LPARAM)list))
        free(list);
}

typedef struct {
    UINT scan_id;
    const ScanResult *result;
} WorkerScan;

// The overlay waits for the foreground window, so its turn is posted even
// when it added nothing.
static void worker_on_window(void *user, const WindowEvent *event)
{
    WorkerScan *scan = (WorkerScan *)user;

    if (!event->skip && (event->changed > 0 || event->foreground))
        post_list(scan->scan_id, FALSE, scan->result);
}

// Chromium, Electron and Firefox build their accessibility tree only after
// the first UI Automation request, so the first scan of such a window finds
// almost nothing. Poke each new foreground window once, ahead of time.
static void warm_foreground(UiaEngine *engine)
{
    static HWND last;
    HWND fg = GetForegroundWindow();
    IUIAutomationElement *root = NULL;
    IUIAutomationElementArray *array = NULL;
    DWORD pid = 0;

    fg = fg ? GetAncestor(fg, GA_ROOT) : NULL;
    if (!fg || fg == last)
        return;
    last = fg;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId())
        return;
    if (FAILED(engine->automation->lpVtbl->ElementFromHandle(
            engine->automation, (UIA_HWND)fg, &root)) || !root)
        return;
    if (SUCCEEDED(root->lpVtbl->FindAllBuildCache(
            root, UIA_TREESCOPE_CHILDREN, engine->condition, engine->cache,
            &array)) && array)
        array->lpVtbl->Release(array);
    root->lpVtbl->Release(root);
}

static DWORD WINAPI uia_worker(void *arg)
{
    UiaEngine engine;
    HRESULT hr;
    BOOL com_ready = FALSE;
    (void)arg;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        BOOL engine_ok = FALSE;
        int attempt;

        com_ready = TRUE;
        for (attempt = 0; attempt < 3 && !engine_ok; ++attempt) {
            if (attempt > 0)
                Sleep(100);
            engine_ok = uia_engine_init(&engine, FALSE);
        }
        if (engine_ok) {
            InterlockedExchange(&g_uia_available, TRUE);
            SetEvent(g_uia_ready);

            for (;;) {
                ScanRequest request;
                ScanResult result;
                WorkerScan scan;

                DWORD wait = WaitForSingleObject(g_uia_wake, 500);
                if (InterlockedCompareExchange(&g_uia_quit, 0, 0))
                    break;
                if (wait == WAIT_TIMEOUT) {
                    warm_foreground(&engine);
                    continue;
                }
                if (wait != WAIT_OBJECT_0)
                    break;

                EnterCriticalSection(&g_uia_cs);
                request = g_uia_request;
                LeaveCriticalSection(&g_uia_cs);
                if (!request.scan_id || !request.mon)
                    continue;

                scan.scan_id = request.scan_id;
                memset(&result, 0, sizeof(result));
                scan.result = &result;
                uia_scan_monitor(&engine, request.mon, worker_keep_going,
                                 &scan.scan_id, worker_on_window, &scan,
                                 &result);
                if (uia_still_current(request.scan_id))
                    post_list(request.scan_id, TRUE, &result);
                free(result.items);
            }
            uia_pool_stop();
            uia_engine_cleanup(&engine);
        } else {
            app_log(L"uia: engine init failed");
            SetEvent(g_uia_ready);
            uia_pool_stop();
        }
        CoUninitialize();
    } else {
        app_log(L"uia: CoInitializeEx failed (0x%08lx)", (unsigned long)hr);
        SetEvent(g_uia_ready);
        uia_pool_stop();
    }

    if (!com_ready)
        InterlockedExchange(&g_uia_available, FALSE);
    return 0;
}

BOOL uia_start(void)
{
    if (g_uia_thread)
        return g_uia_available != 0;

    InitializeCriticalSection(&g_uia_cs);
    g_uia_cs_ready = TRUE;
    g_uia_request.scan_id = 0;
    g_uia_request.mon = NULL;
    g_uia_quit = 0;
    g_uia_available = 0;

    g_uia_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_uia_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_uia_ready || !g_uia_wake)
        goto fail;

    uia_pool_start();

    g_uia_thread = CreateThread(NULL, 0, uia_worker, NULL, 0, NULL);
    if (!g_uia_thread) {
        uia_pool_stop();
        goto fail;
    }

    if (WaitForSingleObject(g_uia_ready, 5000) != WAIT_OBJECT_0)
        goto fail_running;
    return g_uia_available != 0;

fail_running:
    uia_stop();
    return FALSE;
fail:
    if (g_uia_ready)
        CloseHandle(g_uia_ready);
    if (g_uia_wake)
        CloseHandle(g_uia_wake);
    g_uia_ready = NULL;
    g_uia_wake = NULL;
    if (g_uia_cs_ready) {
        DeleteCriticalSection(&g_uia_cs);
        g_uia_cs_ready = FALSE;
    }
    return FALSE;
}

void uia_stop(void)
{
    HANDLE thread;

    uia_scroll_stop();
    uia_focus_stop();
    if (!g_uia_thread)
        return;

    InterlockedExchange(&g_uia_quit, 1);
    if (g_uia_wake)
        SetEvent(g_uia_wake);
    thread = g_uia_thread;
    if (WaitForSingleObject(thread, 3000) != WAIT_OBJECT_0)
        return;

    CloseHandle(thread);
    g_uia_thread = NULL;
    if (g_uia_ready)
        CloseHandle(g_uia_ready);
    if (g_uia_wake)
        CloseHandle(g_uia_wake);
    g_uia_ready = NULL;
    g_uia_wake = NULL;
    if (g_uia_cs_ready) {
        EnterCriticalSection(&g_uia_cs);
        g_uia_request.scan_id = 0;
        g_uia_request.mon = NULL;
        LeaveCriticalSection(&g_uia_cs);
        DeleteCriticalSection(&g_uia_cs);
        g_uia_cs_ready = FALSE;
    }
    InterlockedExchange(&g_uia_available, FALSE);
    InterlockedExchange(&g_uia_quit, 0);
}

BOOL uia_available(void)
{
    return g_uia_available != 0;
}

void uia_scan(HMONITOR mon, UINT scan_id)
{
    if (!g_uia_thread || !g_uia_available || !g_uia_cs_ready)
        return;
    EnterCriticalSection(&g_uia_cs);
    g_uia_request.scan_id = scan_id;
    g_uia_request.mon = mon;
    LeaveCriticalSection(&g_uia_cs);
    SetEvent(g_uia_wake);
}

void uia_cancel(void)
{
    if (!g_uia_cs_ready)
        return;
    EnterCriticalSection(&g_uia_cs);
    g_uia_request.scan_id = 0;
    g_uia_request.mon = NULL;
    LeaveCriticalSection(&g_uia_cs);
}

// ---------------------------------------------------------------------------
// Scroll target (Alt scrolling). Its own MTA thread and engine, so a running
// hint scan never delays a scroll.

#ifndef UIA_IsScrollPatternAvailablePropertyId
#define UIA_IsScrollPatternAvailablePropertyId 30034
#endif
#ifndef UIA_ScrollHorizontallyScrollablePropertyId
#define UIA_ScrollHorizontallyScrollablePropertyId 30057
#endif
#ifndef UIA_ScrollVerticallyScrollablePropertyId
#define UIA_ScrollVerticallyScrollablePropertyId 30058
#endif
#define UIA_TREESCOPE_SUBTREE 7
#define SCROLL_MIN_SIZE 16
#define SCROLL_CACHE_ROOTS 8
#define SCROLL_STALE_MS 10000   // older than this: refresh and wait
#define SCROLL_FRESH_MS 1000    // older than this: refresh in the background
#define SCROLL_STALE_WAIT_MS 150 // how long a stale list waits for its refresh
#define SCROLL_SIDE_REACH 64    // px a redirect may reach across the scroll
#define SCROLL_WARM_MS 8000     // keep the cache warm this long after a press
#define SCROLL_WARM_POLL_MS 700 // how often the idle worker warms it
#define SCROLL_WARM_REFRESH_MS 600 // a hotter list is still fresh next press
#define SCROLL_ENGINE_RETRY_MS 5000 // retry a failed engine this often

typedef struct {
    RECT rc;
    BOOL vertical;      // scrolls vertically
    BOOL horizontal;    // scrolls horizontally
} ScrollArea;

typedef struct {
    BOOL        used;
    HWND        root;
    ScrollArea *areas;
    int         count;
    int         cap;
    BOOL        valid;       // a refresh has completed at least once
    DWORD       tick;        // completion tick
    DWORD       last_used;   // LRU tick
    UINT        gen;         // incremented when a refresh completes
    BOOL        refreshing;  // queued or in flight
    int         waiters;     // uia_scroll_target callers waiting for it
    HANDLE      done;        // manual reset; set when a refresh completes
} ScrollCache;

static HANDLE g_sc_thread;
static HANDLE g_sc_wake;
static CRITICAL_SECTION g_sc_cs;
static volatile LONG g_sc_state;      // 0 = off, 1 = starting, 2 = running, 3 = failed
static volatile LONG g_sc_quit;
static volatile LONG g_sc_last_use;   // tick of the last uia_scroll_target call
static ScrollCache g_sc_cache[SCROLL_CACHE_ROOTS];

// The refresh request for the worker: one at a time, latest pending root wins.
static HWND g_sc_req_root;
static BOOL g_sc_req_pending;

static LONG64 point_rect_distance2(POINT pt, const RECT *rc)
{
    LONG64 dx = 0, dy = 0;

    if (pt.x < rc->left)
        dx = rc->left - pt.x;
    else if (pt.x >= rc->right)
        dx = pt.x - (rc->right - 1);
    if (pt.y < rc->top)
        dy = rc->top - pt.y;
    else if (pt.y >= rc->bottom)
        dy = pt.y - (rc->bottom - 1);
    return dx * dx + dy * dy;
}

// The point in rc nearest to pt, kept a little inside the edge so it lands on
// the scrolling area itself and not on its border or scroll bar.
static POINT point_into_rect(POINT pt, const RECT *rc)
{
    int inset_x = (rc->right - rc->left) / 4;
    int inset_y = (rc->bottom - rc->top) / 4;
    POINT out = pt;

    if (inset_x > 24)
        inset_x = 24;
    if (inset_y > 24)
        inset_y = 24;
    if (out.x < rc->left + inset_x)
        out.x = rc->left + inset_x;
    if (out.x > rc->right - 1 - inset_x)
        out.x = rc->right - 1 - inset_x;
    if (out.y < rc->top + inset_y)
        out.y = rc->top + inset_y;
    if (out.y > rc->bottom - 1 - inset_y)
        out.y = rc->bottom - 1 - inset_y;
    return out;
}

static BOOL scroll_list_add(ScrollArea **items, int *count, int *cap,
                            const ScrollArea *area)
{
    ScrollArea *p;
    int ncap;

    if (*count == *cap) {
        ncap = *cap ? *cap * 2 : 16;
        p = realloc(*items, (size_t)ncap * sizeof(*p));
        if (!p)
            return FALSE;
        *items = p;
        *cap = ncap;
    }
    (*items)[*count] = *area;
    ++*count;
    return TRUE;
}

// Collects every on-screen scrollable area inside the window. Runs on the
// worker thread; the list is handed to the cache under the lock.
static void scroll_collect(IUIAutomation *automation,
                           IUIAutomationCondition *condition,
                           IUIAutomationCacheRequest *cache,
                           HWND root, ScrollArea **items, int *count, int *cap)
{
    IUIAutomationElement *root_el = NULL;
    IUIAutomationElementArray *array = NULL;
    RECT win;
    int i, n = 0;

    *items = NULL;
    *count = 0;
    *cap = 0;
    if (!GetWindowRect(root, &win))
        return;
    if (FAILED(automation->lpVtbl->ElementFromHandle(
            automation, (UIA_HWND)root, &root_el)) || !root_el)
        return;
    if (FAILED(root_el->lpVtbl->FindAllBuildCache(
            root_el, UIA_TREESCOPE_SUBTREE, condition, cache, &array)) ||
        !array) {
        root_el->lpVtbl->Release(root_el);
        return;
    }
    array->lpVtbl->get_Length(array, &n);
    for (i = 0; i < n; ++i) {
        IUIAutomationElement *el = NULL;
        ScrollArea area;
        RECT rc;

        if (FAILED(array->lpVtbl->GetElement(array, i, &el)) || !el)
            continue;
        area.vertical = cached_bool_property(
            el, UIA_ScrollVerticallyScrollablePropertyId);
        area.horizontal = cached_bool_property(
            el, UIA_ScrollHorizontallyScrollablePropertyId);
        if ((area.vertical || area.horizontal) &&
            SUCCEEDED(el->lpVtbl->get_CachedBoundingRectangle(el, &rc)) &&
            IntersectRect(&rc, &rc, &win) &&
            rc.right - rc.left >= SCROLL_MIN_SIZE &&
            rc.bottom - rc.top >= SCROLL_MIN_SIZE) {
            area.rc = rc;
            if (!scroll_list_add(items, count, cap, &area)) {
                el->lpVtbl->Release(el);
                break;
            }
        }
        el->lpVtbl->Release(el);
    }
    array->lpVtbl->Release(array);
    root_el->lpVtbl->Release(root_el);
}

static ScrollCache *scroll_cache_find(HWND root)
{
    int i;

    for (i = 0; i < SCROLL_CACHE_ROOTS; ++i)
        if (g_sc_cache[i].used && g_sc_cache[i].root == root)
            return &g_sc_cache[i];
    return NULL;
}

// Reuses a free slot, or evicts the least recently used entry that nobody is
// refreshing or waiting for.
static ScrollCache *scroll_cache_alloc(HWND root, DWORD now)
{
    ScrollCache *e = NULL;
    int i, victim = -1;
    DWORD oldest = 0;

    for (i = 0; i < SCROLL_CACHE_ROOTS; ++i) {
        if (!g_sc_cache[i].used) {
            e = &g_sc_cache[i];
            break;
        }
    }
    if (!e) {
        for (i = 0; i < SCROLL_CACHE_ROOTS; ++i) {
            ScrollCache *c = &g_sc_cache[i];
            if (c->refreshing || c->waiters > 0)
                continue;
            if (victim < 0 || (LONG)(c->last_used - oldest) < 0) {
                victim = i;
                oldest = c->last_used;
            }
        }
        if (victim < 0)
            return NULL;
        e = &g_sc_cache[victim];
    }
    free(e->areas);
    e->areas = NULL;
    e->count = 0;
    e->cap = 0;
    e->used = TRUE;
    e->root = root;
    e->valid = FALSE;
    e->tick = 0;
    e->last_used = now;
    e->gen = 0;
    e->refreshing = FALSE;
    e->waiters = 0;
    ResetEvent(e->done);
    return e;
}

// Queues a refresh for root, replacing a pending request for another root.
// Requests for the same root coalesce. Caller holds g_sc_cs.
static void scroll_queue_locked(HWND root, DWORD now)
{
    ScrollCache *e = scroll_cache_find(root);

    if (!e)
        e = scroll_cache_alloc(root, now);
    if (!e || e->refreshing)
        return;
    if (g_sc_req_pending && g_sc_req_root != root) {
        ScrollCache *old = scroll_cache_find(g_sc_req_root);
        if (old)
            old->refreshing = FALSE;
    }
    e->refreshing = TRUE;
    ResetEvent(e->done);
    g_sc_req_root = root;
    g_sc_req_pending = TRUE;
    SetEvent(g_sc_wake);
}

// How far rc lies beside pt across the scroll direction: 0 when it is
// directly above or below pt (left or right of it when scrolling sideways).
static LONG scroll_side_gap(POINT pt, const RECT *rc, BOOL vertical)
{
    LONG p = vertical ? pt.x : pt.y;
    LONG lo = vertical ? rc->left : rc->top;
    LONG hi = vertical ? rc->right : rc->bottom;

    if (p < lo)
        return lo - p;
    if (p >= hi)
        return p - (hi - 1);
    return 0;
}

// The same decision scroll_find used to make, from a cached area list.
// Over the window, the wheel goes only to an area in line with the pointer
// (a list under its toolbar) or just beside it. One further across is
// another panel, and the panel under the pointer may scroll without UI
// Automation saying so (Teams' conversation), as a plain wheel shows.
static BOOL scroll_area_answer(const ScrollCache *e, POINT pt, BOOL vertical,
                               POINT *target)
{
    LONG64 best_dist = -1, inside_area = -1;
    RECT best;
    RECT win;
    BOOL over_window;
    int i;

    over_window = GetWindowRect(e->root, &win) && PtInRect(&win, pt);
    SetRectEmpty(&best);
    for (i = 0; i < e->count; ++i) {
        const ScrollArea *a = &e->areas[i];

        if (!(vertical ? a->vertical : a->horizontal))
            continue;
        if (PtInRect(&a->rc, pt)) {
            LONG64 area = rect_area(&a->rc);
            if (inside_area < 0 || area < inside_area)
                inside_area = area;
        } else {
            LONG64 d;

            if (over_window &&
                scroll_side_gap(pt, &a->rc, vertical) > SCROLL_SIDE_REACH)
                continue;
            d = point_rect_distance2(pt, &a->rc);
            if (best_dist < 0 || d < best_dist) {
                best_dist = d;
                best = a->rc;
            }
        }
    }
    if (inside_area >= 0 || best_dist < 0)
        return FALSE;
    *target = point_into_rect(pt, &best);
    return TRUE;
}

static BOOL scroll_engine_init(IUIAutomation **automation,
                               IUIAutomationCondition **condition,
                               IUIAutomationCacheRequest **cache)
{
    IUIAutomationCondition *conds[2];
    IUIAutomation2 *automation2 = NULL;
    HRESULT hr;

    *automation = NULL;
    *condition = NULL;
    *cache = NULL;
    hr = CoCreateInstance(&g_clsid_uia8, NULL, CLSCTX_INPROC_SERVER,
                          &g_iid_uia, (void **)automation);
    if (FAILED(hr) || !*automation) {
        *automation = NULL;
        hr = CoCreateInstance(&g_clsid_uia, NULL, CLSCTX_INPROC_SERVER,
                              &g_iid_uia, (void **)automation);
    }
    if (FAILED(hr) || !*automation)
        return FALSE;
    if (SUCCEEDED((*automation)->lpVtbl->QueryInterface(
            *automation, &g_iid_uia2, (void **)&automation2)) && automation2) {
        automation2->lpVtbl->put_TransactionTimeout(automation2, 1000);
        automation2->lpVtbl->put_ConnectionTimeout(automation2, 1000);
        automation2->lpVtbl->base.Release((IUIAutomation *)automation2);
    }

    conds[0] = property_condition(*automation,
                                  UIA_IsScrollPatternAvailablePropertyId,
                                  VT_BOOL, 1);
    conds[1] = property_condition(*automation, UIA_IsOffscreenPropertyId,
                                  VT_BOOL, 0);
    if (conds[0] && conds[1])
        (*automation)->lpVtbl->CreateAndConditionFromNativeArray(
            *automation, conds, 2, condition);
    if (conds[0])
        conds[0]->lpVtbl->Release(conds[0]);
    if (conds[1])
        conds[1]->lpVtbl->Release(conds[1]);
    if (*condition &&
        SUCCEEDED((*automation)->lpVtbl->CreateCacheRequest(*automation,
                                                            cache)) &&
        *cache &&
        SUCCEEDED((*cache)->lpVtbl->AddProperty(
            *cache, UIA_BoundingRectanglePropertyId)) &&
        SUCCEEDED((*cache)->lpVtbl->AddProperty(
            *cache, UIA_ScrollVerticallyScrollablePropertyId)) &&
        SUCCEEDED((*cache)->lpVtbl->AddProperty(
            *cache, UIA_ScrollHorizontallyScrollablePropertyId)) &&
        SUCCEEDED((*cache)->lpVtbl->put_AutomationElementMode(
            *cache, UIA_MODE_NONE)))
        return TRUE;

    if (*cache)
        (*cache)->lpVtbl->Release(*cache);
    if (*condition)
        (*condition)->lpVtbl->Release(*condition);
    (*automation)->lpVtbl->Release(*automation);
    *automation = NULL;
    *condition = NULL;
    *cache = NULL;
    return FALSE;
}

static HWND scroll_root_for(POINT pt);

static DWORD WINAPI scroll_worker(void *arg)
{
    IUIAutomation *automation = NULL;
    IUIAutomationCondition *condition = NULL;
    IUIAutomationCacheRequest *cache = NULL;
    HRESULT hr;
    BOOL ready, com_ready;
    DWORD last_engine_try;
    (void)arg;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    com_ready = hr == S_OK || hr == S_FALSE;
    if (com_ready) {
        engine_lock();
        ready = scroll_engine_init(&automation, &condition, &cache);
        engine_unlock();
    } else {
        ready = FALSE;
    }
    last_engine_try = GetTickCount();

    for (;;) {
        DWORD now = GetTickCount();
        DWORD wait_ms, woke;
        ScrollArea *items = NULL;
        int count = 0, cap = 0;
        HWND root;

        // While the user has been scrolling lately, wake up now and then and
        // refresh the list under the pointer, so the next press finds a fresh
        // one instead of waiting for a full UI Automation search.
        wait_ms = (DWORD)(now - (DWORD)InterlockedCompareExchange(
                              &g_sc_last_use, 0, 0)) < SCROLL_WARM_MS
                      ? SCROLL_WARM_POLL_MS
                      : INFINITE;
        woke = WaitForSingleObject(g_sc_wake, wait_ms);
        if (woke != WAIT_OBJECT_0 && woke != WAIT_TIMEOUT)
            break;
        if (woke == WAIT_TIMEOUT) {
            POINT pt;
            HWND warm_root;

            if (InterlockedCompareExchange(&g_sc_quit, 0, 0))
                break;
            if (!GetCursorPos(&pt))
                continue;
            // scroll_root_for makes no UI Automation calls, so it is fine on
            // this thread, but everything shared stays under the lock.
            warm_root = scroll_root_for(pt);
            if (!warm_root)
                continue;
            now = GetTickCount();
            EnterCriticalSection(&g_sc_cs);
            {
                ScrollCache *e = scroll_cache_find(warm_root);

                if (e) {
                    e->last_used = now;
                    // A list this young is still fresh at the next press.
                    if (!e->refreshing &&
                        !(e->valid && (DWORD)(now - e->tick) <
                          SCROLL_WARM_REFRESH_MS))
                        scroll_queue_locked(warm_root, now);
                } else {
                    e = scroll_cache_alloc(warm_root, now);
                    if (e)
                        scroll_queue_locked(warm_root, now);
                }
            }
            LeaveCriticalSection(&g_sc_cs);
            continue;
        }
        if (InterlockedCompareExchange(&g_sc_quit, 0, 0))
            break;
        now = GetTickCount();

        EnterCriticalSection(&g_sc_cs);
        root = g_sc_req_pending ? g_sc_req_root : NULL;
        g_sc_req_pending = FALSE;
        g_sc_req_root = NULL;
        LeaveCriticalSection(&g_sc_cs);
        if (!root)
            continue;

        // An engine that failed to be created (several at once is enough) is
        // retried now and then; until then auto-targeting has no list.
        if (!ready && com_ready &&
            (DWORD)(now - last_engine_try) >= SCROLL_ENGINE_RETRY_MS) {
            engine_lock();
            ready = scroll_engine_init(&automation, &condition, &cache);
            engine_unlock();
            last_engine_try = now;
        }
        if (ready)
            scroll_collect(automation, condition, cache, root,
                           &items, &count, &cap);

        EnterCriticalSection(&g_sc_cs);
        {
            ScrollCache *e = scroll_cache_find(root);

            if (e) {
                free(e->areas);
                e->areas = items;
                e->count = count;
                e->cap = cap;
                e->valid = TRUE;
                e->tick = GetTickCount();
                e->last_used = e->tick;
                ++e->gen;
                e->refreshing = FALSE;
                SetEvent(e->done);
                items = NULL;
            }
        }
        LeaveCriticalSection(&g_sc_cs);
        free(items);
    }

    if (cache)
        cache->lpVtbl->Release(cache);
    if (condition)
        condition->lpVtbl->Release(condition);
    if (automation)
        automation->lpVtbl->Release(automation);
    if (hr == S_OK || hr == S_FALSE)
        CoUninitialize();
    return 0;
}

static BOOL scroll_worker_init(void)
{
    int i;

    InitializeCriticalSection(&g_sc_cs);
    memset(g_sc_cache, 0, sizeof(g_sc_cache));
    g_sc_req_root = NULL;
    g_sc_req_pending = FALSE;
    g_sc_quit = 0;
    g_sc_last_use = 0;
    g_sc_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_sc_wake) {
        DeleteCriticalSection(&g_sc_cs);
        return FALSE;
    }
    for (i = 0; i < SCROLL_CACHE_ROOTS; ++i) {
        g_sc_cache[i].done = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!g_sc_cache[i].done) {
            while (i-- > 0)
                CloseHandle(g_sc_cache[i].done);
            CloseHandle(g_sc_wake);
            g_sc_wake = NULL;
            DeleteCriticalSection(&g_sc_cs);
            return FALSE;
        }
    }
    g_sc_thread = CreateThread(NULL, 0, scroll_worker, NULL, 0, NULL);
    if (!g_sc_thread) {
        for (i = 0; i < SCROLL_CACHE_ROOTS; ++i) {
            CloseHandle(g_sc_cache[i].done);
            g_sc_cache[i].done = NULL;
        }
        CloseHandle(g_sc_wake);
        g_sc_wake = NULL;
        DeleteCriticalSection(&g_sc_cs);
        return FALSE;
    }
    return TRUE;
}

static BOOL scroll_worker_start(void)
{
    if (InterlockedCompareExchange(&g_sc_state, 1, 0) == 0)
        InterlockedExchange(&g_sc_state, scroll_worker_init() ? 2 : 3);
    while (InterlockedCompareExchange(&g_sc_state, 1, 1) == 1)
        Sleep(0);
    return InterlockedCompareExchange(&g_sc_state, 2, 2) == 2;
}

// Like scroll_worker_start but never spins. Returns FALSE while another thread
// is still starting the worker.
static BOOL scroll_worker_start_nonblocking(void)
{
    if (InterlockedCompareExchange(&g_sc_state, 1, 0) == 0)
        InterlockedExchange(&g_sc_state, scroll_worker_init() ? 2 : 3);
    return InterlockedCompareExchange(&g_sc_state, 2, 2) == 2;
}

// The window to search: the one under the pointer, or the foreground window
// when the pointer is over the desktop, the taskbar or one of ours.
static HWND scroll_root_for(POINT pt)
{
    HWND hwnd = WindowFromPoint(pt);
    HWND root = hwnd ? GetAncestor(hwnd, GA_ROOT) : NULL;
    WCHAR cls[64];
    DWORD pid = 0;

    if (root) {
        GetWindowThreadProcessId(root, &pid);
        cls[0] = 0;
        GetClassNameW(root, cls, 64);
        if (pid == GetCurrentProcessId() || root == GetShellWindow() ||
            lstrcmpW(cls, L"Progman") == 0 || lstrcmpW(cls, L"WorkerW") == 0 ||
            lstrcmpW(cls, L"Shell_TrayWnd") == 0 ||
            lstrcmpW(cls, L"Shell_SecondaryTrayWnd") == 0)
            root = NULL;
    }
    if (!root) {
        root = GetForegroundWindow();
        root = root ? GetAncestor(root, GA_ROOT) : NULL;
    }
    return root;
}

BOOL uia_scroll_target(POINT pt, BOOL vertical, DWORD timeout_ms,
                       POINT *target)
{
    HWND root;
    ScrollCache *e;
    HANDLE done;
    DWORD start, wait_ms;
    UINT gen;
    BOOL redirect, stale_answer = FALSE;

    root = scroll_root_for(pt);
    if (!root)
        return FALSE;
    if (!scroll_worker_start())
        return FALSE;

    start = GetTickCount();
    InterlockedExchange(&g_sc_last_use, (LONG)start);
    wait_ms = timeout_ms;
    EnterCriticalSection(&g_sc_cs);
    e = scroll_cache_find(root);
    if (e && e->valid &&
        (DWORD)(start - e->tick) < SCROLL_STALE_MS) {
        BOOL fresh = (DWORD)(start - e->tick) <= SCROLL_FRESH_MS;

        if (!fresh && !e->refreshing)
            scroll_queue_locked(root, start);
        e->last_used = start;
        redirect = scroll_area_answer(e, pt, vertical, target);
        // Only a fresh list may send the wheel away from the pointer at
        // once: the area under it may be new since (another chat opened in
        // Teams), and an older list would aim at a side panel instead. A
        // slightly old list that would redirect waits only briefly for its
        // refresh, then still redirects with what it has: waiting the whole
        // timeout for a slow search feels far worse than a stale answer.
        if (fresh || !redirect) {
            LeaveCriticalSection(&g_sc_cs);
            return redirect;
        }
        stale_answer = TRUE;
        wait_ms = SCROLL_STALE_WAIT_MS;
    }
    if (!e) {
        e = scroll_cache_alloc(root, start);
        if (!e) {
            LeaveCriticalSection(&g_sc_cs);
            return FALSE;
        }
    }
    if (!e->refreshing)
        scroll_queue_locked(root, start);
    e->last_used = start;
    gen = e->gen;
    ++e->waiters;
    done = e->done;
    LeaveCriticalSection(&g_sc_cs);

    for (;;) {
        DWORD elapsed = GetTickCount() - start;

        if (elapsed >= wait_ms ||
            WaitForSingleObject(done, wait_ms - elapsed) != WAIT_OBJECT_0)
            break;
        EnterCriticalSection(&g_sc_cs);
        e = scroll_cache_find(root);
        if (e && e->valid && e->gen != gen) {
            e->last_used = GetTickCount();
            redirect = scroll_area_answer(e, pt, vertical, target);
            --e->waiters;
            LeaveCriticalSection(&g_sc_cs);
            return redirect;
        }
        LeaveCriticalSection(&g_sc_cs);
    }

    EnterCriticalSection(&g_sc_cs);
    e = scroll_cache_find(root);
    if (e && e->waiters > 0)
        --e->waiters;
    // The refresh missed its short deadline: the stale list's answer, still
    // better than scrolling under the pointer when it is over another panel.
    redirect = e && stale_answer &&
               scroll_area_answer(e, pt, vertical, target);
    LeaveCriticalSection(&g_sc_cs);
    return redirect;
}

// Starts caching the scrollable areas of the window under pt. Never waits and
// makes no UI Automation calls on the caller's thread.
void uia_scroll_prefetch(POINT pt)
{
    HWND root;
    ScrollCache *e;
    DWORD now;

    root = scroll_root_for(pt);
    if (!root)
        return;
    if (!scroll_worker_start_nonblocking())
        return;
    now = GetTickCount();
    EnterCriticalSection(&g_sc_cs);
    e = scroll_cache_find(root);
    if (e) {
        e->last_used = now;
        if (e->refreshing ||
            (e->valid && (DWORD)(now - e->tick) < SCROLL_FRESH_MS)) {
            LeaveCriticalSection(&g_sc_cs);
            return;
        }
        scroll_queue_locked(root, now);
    } else {
        e = scroll_cache_alloc(root, now);
        if (e)
            scroll_queue_locked(root, now);
    }
    LeaveCriticalSection(&g_sc_cs);
}

void uia_scroll_stop(void)
{
    int i;

    while (InterlockedCompareExchange(&g_sc_state, 1, 1) == 1)
        Sleep(0);
    if (InterlockedCompareExchange(&g_sc_state, 0, 0) != 2)
        return;
    InterlockedExchange(&g_sc_quit, 1);
    SetEvent(g_sc_wake);
    if (WaitForSingleObject(g_sc_thread, 3000) != WAIT_OBJECT_0)
        return;
    CloseHandle(g_sc_thread);
    CloseHandle(g_sc_wake);
    g_sc_thread = NULL;
    g_sc_wake = NULL;
    EnterCriticalSection(&g_sc_cs);
    for (i = 0; i < SCROLL_CACHE_ROOTS; ++i) {
        free(g_sc_cache[i].areas);
        g_sc_cache[i].areas = NULL;
        g_sc_cache[i].count = 0;
        g_sc_cache[i].cap = 0;
        g_sc_cache[i].used = FALSE;
        g_sc_cache[i].refreshing = FALSE;
        g_sc_cache[i].waiters = 0;
        if (g_sc_cache[i].done) {
            CloseHandle(g_sc_cache[i].done);
            g_sc_cache[i].done = NULL;
        }
    }
    g_sc_req_root = NULL;
    g_sc_req_pending = FALSE;
    LeaveCriticalSection(&g_sc_cs);
    DeleteCriticalSection(&g_sc_cs);
    InterlockedExchange(&g_sc_state, 0);
}

// ---------------------------------------------------------------------------
// Typing context (bare keys): is the keyboard focus in a text field? Its own
// MTA thread and engine. Each focus change (uia_focus_changed) makes the
// answer unknown until the worker has asked UI Automation again; while the
// user is active it also asks every FOCUS_POLL_MS, in case a change went
// unreported. The hook only reads the cached answer (uia_focus_state).

#ifndef UIA_IsPasswordPropertyId
#define UIA_IsPasswordPropertyId 30019
#endif
#ifndef UIA_IsTextPatternAvailablePropertyId
#define UIA_IsTextPatternAvailablePropertyId 30040
#endif
#ifndef UIA_IsValuePatternAvailablePropertyId
#define UIA_IsValuePatternAvailablePropertyId 30043
#endif
#ifndef UIA_ValueIsReadOnlyPropertyId
#define UIA_ValueIsReadOnlyPropertyId 30046
#endif
#ifndef UIA_IsLegacyIAccessiblePatternAvailablePropertyId
#define UIA_IsLegacyIAccessiblePatternAvailablePropertyId 30090
#endif
#ifndef UIA_LegacyIAccessibleStatePropertyId
#define UIA_LegacyIAccessibleStatePropertyId 30096
#endif
#ifndef UIA_CustomControlTypeId
#define UIA_CustomControlTypeId 50025
#endif
#ifndef UIA_WindowControlTypeId
#define UIA_WindowControlTypeId 50032
#endif
#ifndef UIA_PaneControlTypeId
#define UIA_PaneControlTypeId 50033
#endif
#ifndef STATE_SYSTEM_READONLY
#define STATE_SYSTEM_READONLY 0x40
#endif
#ifndef GUI_CARETBLINKING
#define GUI_CARETBLINKING 0x00000001
#endif
#ifndef GUI_INMENUMODE
#define GUI_INMENUMODE 0x00000004
#endif
#ifndef GUI_SYSTEMMENUMODE
#define GUI_SYSTEMMENUMODE 0x00000008
#endif
#ifndef GUI_POPUPMENUMODE
#define GUI_POPUPMENUMODE 0x00000010
#endif
#ifndef WM_GETOBJECT
#define WM_GETOBJECT 0x003D
#endif
#ifndef OBJID_CARET
#define OBJID_CARET ((LONG)0xFFFFFFF8)
#endif
#ifndef CHILDID_SELF
#define CHILDID_SELF 0
#endif
#ifndef STATE_SYSTEM_INVISIBLE
#define STATE_SYSTEM_INVISIBLE 0x00008000
#endif

#define FOCUS_POLL_MS 500       // recheck this often while the user is active
#define FOCUS_ACTIVE_MS 30000   // active = some input within this long
#define FOCUS_RETRY_MS 100      // re-ask a change this soon while unknown
#define FOCUS_RETRIES 3         // ... at most this many times per change
#define FOCUS_WAIT_FRESH_MS 400 // a pending ask older than this is not waited for
#define FOCUS_ENGINE_RETRY_MS 5000  // retry a failed engine init this often
#define FOCUS_CARET_TIMEOUT_MS 200  // how long a window may take to name its caret
#define FOCUS_FAILED (-1)       // internal: the ask failed, not a classification

static CRITICAL_SECTION g_fx_cs;
static HANDLE g_fx_wake;
static HANDLE g_fx_answered;        // manual reset: the current gen has an answer
static HANDLE g_fx_thread;
static volatile LONG g_fx_state;    // 0 stopped, 1 starting, 2 running, 3 failed
static volatile LONG g_fx_quit;
// g_fx_cs guards the rest. Every change bumps g_fx_gen; the answer counts
// only while g_fx_answer_gen still equals it and the focus is still g_fx_hwnd.
static LONG  g_fx_gen;
static LONG  g_fx_answer_gen = -1;
static HWND  g_fx_hwnd;
static int   g_fx_answer = FOCUS_UNKNOWN;
static DWORD g_fx_not_before;       // do not ask before this tick
static DWORD g_fx_pending_since;    // when the current unanswered gen started

// The window with the keyboard focus (the foreground window if its thread
// names none), and the GUI thread state behind it. FALSE if unknown.
static BOOL focus_window(HWND *focus, GUITHREADINFO *gti, DWORD *pid)
{
    HWND fg = GetForegroundWindow();
    DWORD tid;

    *focus = NULL;
    *pid = 0;
    if (!fg)
        return FALSE;
    tid = GetWindowThreadProcessId(fg, pid);
    ZeroMemory(gti, sizeof(*gti));
    gti->cbSize = sizeof(*gti);
    if (!tid || !GetGUIThreadInfo(tid, gti))
        return FALSE;
    *focus = gti->hwndFocus ? gti->hwndFocus : fg;
    return TRUE;
}

static BOOL class_in(const WCHAR *cls, const WCHAR *const *list)
{
    int i;

    for (i = 0; list[i]; ++i)
        if (lstrcmpiW(cls, list[i]) == 0)
            return TRUE;
    return FALSE;
}

// Windows that take typing whatever UI Automation says: edit controls, and
// terminals and editors whose text area is the whole window.
static BOOL text_window(HWND focus)
{
    static const WCHAR *const focus_classes[] = {
        L"Edit", L"Scintilla", NULL
    };
    static const WCHAR *const root_classes[] = {
        L"ConsoleWindowClass", L"CASCADIA_HOSTING_WINDOW_CLASS",
        L"PseudoConsoleWindow", L"mintty", L"PuTTY", L"VirtualConsoleClass",
        L"Vim", L"Emacs", NULL
    };
    WCHAR cls[64];
    HWND root;

    cls[0] = 0;
    GetClassNameW(focus, cls, 64);
    if (class_in(cls, focus_classes) || _wcsnicmp(cls, L"RichEdit", 8) == 0)
        return TRUE;
    root = GetAncestor(focus, GA_ROOT);
    cls[0] = 0;
    if (root)
        GetClassNameW(root, cls, 64);
    return class_in(cls, root_classes);
}

static LONG cached_long_property(IUIAutomationElement *el, int property_id)
{
    VARIANT v;
    LONG value = 0;

    VariantInit(&v);
    if (SUCCEEDED(el->lpVtbl->GetCachedPropertyValue(el, property_id, &v)) &&
        v.vt == VT_I4)
        value = v.lVal;
    VariantClear(&v);
    return value;
}

// Controls whose value is not typed in, though it may be writable (a web
// checkbox's "on").
static BOOL control_takes_no_text(int ct)
{
    switch (ct) {
    case UIA_ButtonControlTypeId: case UIA_CheckBoxControlTypeId:
    case UIA_HyperlinkControlTypeId: case UIA_ListItemControlTypeId:
    case UIA_MenuItemControlTypeId: case UIA_RadioButtonControlTypeId:
    case UIA_ScrollBarControlTypeId: case UIA_TabItemControlTypeId:
    case UIA_TreeItemControlTypeId: case UIA_SplitButtonControlTypeId:
    case UIA_HeaderItemControlTypeId:
    case 50006:     // Image
    case 50012:     // ProgressBar
    case 50015:     // Slider
        return TRUE;
    }
    return FALSE;
}

// What typing into the focused element would do. A web page itself is a
// read-only document; its inputs and contenteditable areas take a value.
static int focus_element_kind(IUIAutomationElement *el)
{
    int ct = 0;
    BOOL value, value_ro, readonly;

    el->lpVtbl->get_CachedControlType(el, &ct);
    if (cached_bool_property(el, UIA_IsPasswordPropertyId))
        return FOCUS_TEXT;
    if (control_takes_no_text(ct))
        return FOCUS_OTHER;
    // A closed dropdown or a list box picks an item with letters but takes no
    // text; an editable combobox has the Text pattern.
    if ((ct == UIA_ComboBoxControlTypeId || ct == UIA_ListControlTypeId) &&
        !cached_bool_property(el, UIA_IsTextPatternAvailablePropertyId))
        return FOCUS_OTHER;
    value = cached_bool_property(el, UIA_IsValuePatternAvailablePropertyId);
    value_ro = value &&
               cached_bool_property(el, UIA_ValueIsReadOnlyPropertyId);
    // Input fields, contenteditable areas, spreadsheet cells.
    if (value && !value_ro)
        return FOCUS_TEXT;
    readonly = value_ro ||
               (cached_bool_property(
                    el, UIA_IsLegacyIAccessiblePatternAvailablePropertyId) &&
                (cached_long_property(el, UIA_LegacyIAccessibleStatePropertyId) &
                 STATE_SYSTEM_READONLY));
    if (ct == UIA_EditControlTypeId || ct == UIA_DocumentControlTypeId)
        return readonly ? FOCUS_OTHER : FOCUS_TEXT;
    // Terminals and custom editors.
    if (cached_bool_property(el, UIA_IsTextPatternAvailablePropertyId))
        return FOCUS_TEXT;
    // A bare window says nothing: a game, a remote desktop, a canvas.
    if (ct == 0 || ct == UIA_PaneControlTypeId ||
        ct == UIA_WindowControlTypeId || ct == UIA_CustomControlTypeId)
        return FOCUS_UNKNOWN;
    return FOCUS_OTHER;
}

// The MSAA caret object (IAccessible); only get_accState is called.
typedef struct IAccessibleCaret IAccessibleCaret;
typedef struct IAccessibleCaretVtbl {
    void *QueryInterface;
    void *AddRef;
    ULONG (STDMETHODCALLTYPE *Release)(IAccessibleCaret *This);
    void *GetTypeInfoCount;
    void *GetTypeInfo;
    void *GetIDsOfNames;
    void *Invoke;
    void *get_accParent;
    void *get_accChildCount;
    void *get_accChild;
    void *get_accName;
    void *get_accValue;
    void *get_accDescription;
    void *get_accRole;
    HRESULT (STDMETHODCALLTYPE *get_accState)(
        IAccessibleCaret *This, VARIANT child, VARIANT *state);
} IAccessibleCaretVtbl;
struct IAccessibleCaret { IAccessibleCaretVtbl *lpVtbl; };

_Static_assert(sizeof(IAccessibleCaretVtbl) == 15 * sizeof(void *),
               "IAccessible vtable size up to get_accState");

static const GUID g_iid_iaccessible = {
    0x618736e0, 0x3c3d, 0x11cf,
    { 0x81, 0x0c, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

typedef HRESULT (WINAPI *ObjectFromLresultFn)(LRESULT, REFIID, WPARAM,
                                              void **);

// Chromium (Chrome, Edge, Electron apps, WebView2) makes no Win32 caret, but
// its window answers OBJID_CARET with a caret that is visible exactly while
// typing goes into a text input. That holds where UI Automation names
// something else: an input with aria-activedescendant (Zendesk's ticket
// fields, any autocomplete box) reports the highlighted option as the focus.
// Windows that do not answer themselves are skipped; their Win32 caret is
// already in the GUI thread info. Runs on the focus worker only.
static BOOL caret_visible(HWND hwnd)
{
    static ObjectFromLresultFn from_lresult;
    static BOOL loaded;
    DWORD_PTR result = 0;
    IAccessibleCaret *caret = NULL;
    VARIANT self, state;
    BOOL visible = FALSE;

    if (!loaded) {
        HMODULE oleacc = LoadLibraryW(L"oleacc.dll");

        loaded = TRUE;
        if (oleacc)
            from_lresult = (ObjectFromLresultFn)(void *)GetProcAddress(
                oleacc, "ObjectFromLresult");
    }
    if (!from_lresult || !hwnd)
        return FALSE;
    if (!SendMessageTimeoutW(hwnd, WM_GETOBJECT, 0, (LPARAM)OBJID_CARET,
                             SMTO_ABORTIFHUNG, FOCUS_CARET_TIMEOUT_MS,
                             &result) ||
        (LONG_PTR)result <= 0)
        return FALSE;
    if (FAILED(from_lresult((LRESULT)result, &g_iid_iaccessible, 0,
                            (void **)&caret)) || !caret)
        return FALSE;
    VariantInit(&self);
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    VariantInit(&state);
    if (SUCCEEDED(caret->lpVtbl->get_accState(caret, self, &state)) &&
        state.vt == VT_I4)
        visible = (state.lVal & STATE_SYSTEM_INVISIBLE) == 0;
    VariantClear(&state);
    caret->lpVtbl->Release(caret);
    return visible;
}

static int focus_classify(IUIAutomation *automation,
                          IUIAutomationCacheRequest *cache, HWND *out)
{
    IUIAutomationElement *el = NULL;
    GUITHREADINFO gti;
    HWND focus, again;
    DWORD pid;
    int kind;

    *out = NULL;
    if (!focus_window(&focus, &gti, &pid))
        return FOCUS_UNKNOWN;
    *out = focus;
    if (gti.hwndCaret || (gti.flags & GUI_CARETBLINKING))
        return FOCUS_TEXT;
    if (gti.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE))
        return FOCUS_UNKNOWN;
    // Our own windows: never query the UI thread from here.
    if (pid == GetCurrentProcessId())
        return FOCUS_UNKNOWN;
    if (text_window(focus))
        return FOCUS_TEXT;
    if (caret_visible(focus))
        return FOCUS_TEXT;
    if (!automation || !cache ||
        FAILED(automation->lpVtbl->GetFocusedElementBuildCache(
            automation, cache, &el)) || !el)
        return FOCUS_FAILED;
    kind = focus_element_kind(el);
    el->lpVtbl->Release(el);
    // The focus moved to another window while we asked.
    if (!focus_window(&again, &gti, &pid) || again != focus)
        return FOCUS_FAILED;
    return kind;
}

static BOOL focus_engine_init(IUIAutomation **automation,
                              IUIAutomationCacheRequest **cache)
{
    static const int props[] = {
        UIA_ControlTypePropertyId, UIA_IsPasswordPropertyId,
        UIA_IsValuePatternAvailablePropertyId, UIA_ValueIsReadOnlyPropertyId,
        UIA_IsTextPatternAvailablePropertyId,
        UIA_IsLegacyIAccessiblePatternAvailablePropertyId,
        UIA_LegacyIAccessibleStatePropertyId
    };
    IUIAutomation2 *automation2 = NULL;
    HRESULT hr;
    size_t i;

    *automation = NULL;
    *cache = NULL;
    hr = CoCreateInstance(&g_clsid_uia8, NULL, CLSCTX_INPROC_SERVER,
                          &g_iid_uia, (void **)automation);
    if (FAILED(hr) || !*automation) {
        *automation = NULL;
        hr = CoCreateInstance(&g_clsid_uia, NULL, CLSCTX_INPROC_SERVER,
                              &g_iid_uia, (void **)automation);
    }
    if (FAILED(hr) || !*automation) {
        *automation = NULL;
        return FALSE;
    }
    if (SUCCEEDED((*automation)->lpVtbl->QueryInterface(
            *automation, &g_iid_uia2, (void **)&automation2)) && automation2) {
        automation2->lpVtbl->put_TransactionTimeout(automation2, 1000);
        automation2->lpVtbl->put_ConnectionTimeout(automation2, 1000);
        automation2->lpVtbl->base.Release((IUIAutomation *)automation2);
    }
    if (FAILED((*automation)->lpVtbl->CreateCacheRequest(*automation, cache)) ||
        !*cache) {
        *cache = NULL;
        (*automation)->lpVtbl->Release(*automation);
        *automation = NULL;
        return FALSE;
    }
    for (i = 0; i < sizeof(props) / sizeof(props[0]); ++i)
        (*cache)->lpVtbl->AddProperty(*cache, props[i]);
    (*cache)->lpVtbl->put_AutomationElementMode(*cache, UIA_MODE_NONE);
    return TRUE;
}

static BOOL user_active(void)
{
    LASTINPUTINFO lii;

    lii.cbSize = sizeof(lii);
    if (!GetLastInputInfo(&lii))
        return TRUE;
    return (DWORD)(GetTickCount() - lii.dwTime) < FOCUS_ACTIVE_MS;
}

static const WCHAR *focus_kind_name(int kind)
{
    switch (kind) {
    case FOCUS_TEXT:  return L"text field";
    case FOCUS_OTHER: return L"no text field";
    default:          return L"unknown";
    }
}

static DWORD WINAPI focus_worker(void *arg)
{
    IUIAutomation *automation = NULL;
    IUIAutomationCacheRequest *cache = NULL;
    HRESULT hr;
    HWND last_hwnd = NULL;
    int last_kind = -1;
    DWORD engine_tried = GetTickCount();
    LONG retry_gen = -1;        // gen whose unknown answer gets fast retries
    int retry_done = 0;
    DWORD retry_at = 0;
    (void)arg;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        engine_lock();
        focus_engine_init(&automation, &cache);
        engine_unlock();
    }

    while (!InterlockedCompareExchange(&g_fx_quit, 0, 0)) {
        LONG gen;
        BOOL pending, retrying;
        DWORD wait = 0, now;
        HWND hwnd;
        int kind;

        EnterCriticalSection(&g_fx_cs);
        gen = g_fx_gen;
        pending = g_fx_answer_gen != gen;
        now = GetTickCount();
        retrying = !pending && retry_gen == gen && retry_done < FOCUS_RETRIES;
        if (pending) {
            if ((LONG)(g_fx_not_before - now) > 0)
                wait = g_fx_not_before - now;
        } else if (retrying && (LONG)(retry_at - now) > 0) {
            wait = retry_at - now;
        }
        LeaveCriticalSection(&g_fx_cs);

        if (wait > 0) {
            WaitForSingleObject(g_fx_wake, wait);
            continue;
        }
        // Answered: poll now and then. The old answer stays valid while the
        // poll asks again; any change wakes us and invalidates it.
        if (!pending && !retrying) {
            BOOL active = g_cfg.bare_keys && user_active();

            if (WaitForSingleObject(g_fx_wake, active ? FOCUS_POLL_MS : 5000) !=
                    WAIT_TIMEOUT || !active)
                continue;
        }

        // The engine may have failed to start (or died); try again now and
        // then, but never more often than FOCUS_ENGINE_RETRY_MS.
        now = GetTickCount();
        if (!automation &&
            (DWORD)(now - engine_tried) >= FOCUS_ENGINE_RETRY_MS) {
            engine_tried = now;
            engine_lock();
            focus_engine_init(&automation, &cache);
            engine_unlock();
        }

        if (pending) {
            retry_gen = -1;
            retry_done = 0;
        }

        kind = focus_classify(automation, cache, &hwnd);

        EnterCriticalSection(&g_fx_cs);
        if (g_fx_gen == gen) {
            if (pending) {
                // The old answer no longer applies. An ask that failed is an
                // unknown, but ask again soon: a page navigation often makes
                // the first ask fail or return a bare pane.
                g_fx_hwnd = hwnd;
                g_fx_answer = kind == FOCUS_FAILED ? FOCUS_UNKNOWN : kind;
                g_fx_answer_gen = gen;
                if (g_fx_answer == FOCUS_UNKNOWN) {
                    retry_gen = gen;
                    retry_done = 0;
                    retry_at = GetTickCount() + FOCUS_RETRY_MS;
                }
                kind = g_fx_answer;
                SetEvent(g_fx_answered);
            } else {
                BOOL keep = (kind == FOCUS_FAILED ||
                             kind == FOCUS_UNKNOWN) &&
                            hwnd == g_fx_hwnd;

                // A poll that failed says nothing about the focus: keep the
                // answer it asked about.
                if (!keep) {
                    g_fx_hwnd = hwnd;
                    g_fx_answer = kind == FOCUS_FAILED ? FOCUS_UNKNOWN : kind;
                    g_fx_answer_gen = gen;
                }
                kind = g_fx_answer;
                hwnd = g_fx_hwnd;
                SetEvent(g_fx_answered);
                if (retry_gen == gen) {
                    if (g_fx_answer == FOCUS_UNKNOWN &&
                        retry_done < FOCUS_RETRIES) {
                        ++retry_done;
                        retry_at = GetTickCount() + FOCUS_RETRY_MS;
                    } else {
                        retry_gen = -1;    // upgraded: back to normal polls
                    }
                }
            }
        }
        LeaveCriticalSection(&g_fx_cs);

        if (hwnd != last_hwnd || kind != last_kind) {
            app_log(L"focus: %ls (window %p)", focus_kind_name(kind),
                    (void *)hwnd);
            // Bare scrolling may follow: cache the areas under the pointer.
            if (kind == FOCUS_OTHER) {
                POINT pt;
                if (GetCursorPos(&pt))
                    uia_scroll_prefetch(pt);
            }
            last_hwnd = hwnd;
            last_kind = kind;
        }
    }

    if (cache)
        cache->lpVtbl->Release(cache);
    if (automation)
        automation->lpVtbl->Release(automation);
    if (hr == S_OK || hr == S_FALSE)
        CoUninitialize();
    return 0;
}

static BOOL focus_worker_init(void)
{
    InitializeCriticalSection(&g_fx_cs);
    g_fx_gen = 0;
    g_fx_answer_gen = -1;
    g_fx_hwnd = NULL;
    g_fx_answer = FOCUS_UNKNOWN;
    g_fx_not_before = GetTickCount();
    g_fx_pending_since = g_fx_not_before;
    g_fx_quit = 0;
    g_fx_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_fx_answered = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_fx_wake || !g_fx_answered) {
        if (g_fx_wake)
            CloseHandle(g_fx_wake);
        if (g_fx_answered)
            CloseHandle(g_fx_answered);
        g_fx_wake = NULL;
        g_fx_answered = NULL;
        DeleteCriticalSection(&g_fx_cs);
        return FALSE;
    }
    g_fx_thread = CreateThread(NULL, 0, focus_worker, NULL, 0, NULL);
    if (!g_fx_thread) {
        CloseHandle(g_fx_wake);
        CloseHandle(g_fx_answered);
        g_fx_wake = NULL;
        g_fx_answered = NULL;
        DeleteCriticalSection(&g_fx_cs);
        return FALSE;
    }
    return TRUE;
}

void uia_focus_changed(DWORD delay_ms)
{
    DWORD at;

    if (InterlockedCompareExchange(&g_fx_state, 1, 0) == 0)
        InterlockedExchange(&g_fx_state, focus_worker_init() ? 2 : 3);
    if (InterlockedCompareExchange(&g_fx_state, 2, 2) != 2)
        return;
    at = GetTickCount() + delay_ms;
    EnterCriticalSection(&g_fx_cs);
    // A new change starts a new pending request; while one is already pending
    // its start stays, so a hung app cannot make every key wait.
    if (g_fx_answer_gen == g_fx_gen)
        g_fx_pending_since = GetTickCount();
    ++g_fx_gen;
    ResetEvent(g_fx_answered);
    if ((LONG)(at - g_fx_not_before) > 0)
        g_fx_not_before = at;
    LeaveCriticalSection(&g_fx_cs);
    SetEvent(g_fx_wake);
}

// The cached state, plus whether an answer for the current focus is still
// being asked for. Bumps the generation when the focus moved unannounced.
static int focus_state_run(BOOL *asking)
{
    GUITHREADINFO gti;
    HWND focus;
    DWORD pid;
    int kind = FOCUS_UNKNOWN;
    BOOL moved = FALSE;

    *asking = FALSE;
    if (InterlockedCompareExchange(&g_fx_state, 2, 2) != 2)
        return FOCUS_UNKNOWN;
    if (!focus_window(&focus, &gti, &pid))
        return FOCUS_UNKNOWN;
    if (gti.hwndCaret || (gti.flags & GUI_CARETBLINKING))
        return FOCUS_TEXT;
    EnterCriticalSection(&g_fx_cs);
    if (g_fx_answer_gen == g_fx_gen) {
        if (g_fx_hwnd == focus) {
            kind = g_fx_answer;
        } else {
            // The focus moved without an event (or before it arrived).
            g_fx_pending_since = GetTickCount();
            ++g_fx_gen;
            ResetEvent(g_fx_answered);
            moved = TRUE;
        }
    }
    *asking = g_fx_answer_gen != g_fx_gen;
    LeaveCriticalSection(&g_fx_cs);
    if (moved)
        SetEvent(g_fx_wake);
    return kind;
}

int uia_focus_state(void)
{
    BOOL asking;

    return focus_state_run(&asking);
}

int uia_focus_wait(DWORD max_ms)
{
    DWORD deadline = GetTickCount() + max_ms;
    BOOL asking;
    DWORD now, left;
    int kind;

    for (;;) {
        kind = focus_state_run(&asking);
        if (!asking)
            return kind;
        now = GetTickCount();
        if ((LONG)(deadline - now) <= 0)
            return kind;
        EnterCriticalSection(&g_fx_cs);
        // A request that has been pending this long is a hung app: never pay
        // for it on every key. Nor wait for one that cannot start in time.
        if ((DWORD)(now - g_fx_pending_since) >= FOCUS_WAIT_FRESH_MS ||
            (LONG)(g_fx_not_before - deadline) > 0) {
            LeaveCriticalSection(&g_fx_cs);
            return kind;
        }
        LeaveCriticalSection(&g_fx_cs);
        left = (DWORD)(deadline - now);
        WaitForSingleObject(g_fx_answered, left);
        // Wakeups can be spurious or for an older generation: re-check.
    }
}

void uia_focus_stop(void)
{
    while (InterlockedCompareExchange(&g_fx_state, 1, 1) == 1)
        Sleep(0);
    if (InterlockedCompareExchange(&g_fx_state, 0, 0) != 2)
        return;
    InterlockedExchange(&g_fx_quit, 1);
    SetEvent(g_fx_wake);
    if (WaitForSingleObject(g_fx_thread, 3000) != WAIT_OBJECT_0)
        return;
    CloseHandle(g_fx_thread);
    CloseHandle(g_fx_wake);
    CloseHandle(g_fx_answered);
    g_fx_thread = NULL;
    g_fx_wake = NULL;
    g_fx_answered = NULL;
    DeleteCriticalSection(&g_fx_cs);
    InterlockedExchange(&g_fx_state, 0);
}
