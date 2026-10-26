// uia.c - UI Automation element scan on a worker thread (see common.h).

#include "common.h"
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
#define UIA_HELPER_THREADS 3

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
    void *GetFocusedElementBuildCache;
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

static BOOL element_actionable(IUIAutomationElement *el)
{
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
// button), so try points that stay on the free part of the card.
static POINT choose_outer_click_point(const ScanTarget *cands, int count,
                                      int self, const RECT *zrects, int z)
{
    const RECT *rc = &cands[self].target.rc;
    LONG w = rc->right - rc->left;
    LONG h = rc->bottom - rc->top;
    LONG64 area = rect_area(rc);
    POINT pts[9];
    int i, j;

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
        for (j = 0; j < count; ++j) {
            if (j == self || rect_area(&cands[j].target.rc) >= area ||
                !PtInRect(rc, cands[j].target.click))
                continue;
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

// Apps that answer UI Automation very slowly with next to nothing (games,
// custom-drawn UIs) are skipped for a while, so the hints fall back to the
// grid at once instead of stalling for seconds on every open.
#define SLOW_SCAN_MS       600
#define SLOW_SCAN_TARGETS  4
#define SLOW_SKIP_MS       (10 * 60 * 1000)
#define SLOW_MAX           16

static struct { DWORD pid; DWORD since; } g_slow[SLOW_MAX];
static volatile LONG g_slow_lock;
static BOOL g_slow_memory = TRUE;   // the probe turns it off to report truthfully

static BOOL slow_check(DWORD pid, BOOL remember)
{
    DWORD now = GetTickCount();
    BOOL hit = FALSE;
    int i, free_slot = -1, oldest = 0;

    if (!g_slow_memory || !pid)
        return FALSE;
    while (InterlockedExchange(&g_slow_lock, 1))
        Sleep(0);
    for (i = 0; i < SLOW_MAX; ++i) {
        if (g_slow[i].pid && now - g_slow[i].since > SLOW_SKIP_MS)
            g_slow[i].pid = 0;
        if (g_slow[i].pid == pid)
            hit = TRUE;
        else if (!g_slow[i].pid && free_slot < 0)
            free_slot = i;
        if (now - g_slow[i].since > now - g_slow[oldest].since)
            oldest = i;
    }
    if (remember && !hit) {
        i = free_slot >= 0 ? free_slot : oldest;
        g_slow[i].pid = pid;
        g_slow[i].since = now;
    }
    InterlockedExchange(&g_slow_lock, 0);
    return hit;
}

// Browsers and Electron apps build their tree lazily: a slow, nearly empty
// first scan there is normal and must not get them skipped.
static BOOL lazy_tree_window(HWND hwnd)
{
    WCHAR cls[64];
    GetClassNameW(hwnd, cls, 64);
    return wcsncmp(cls, L"Chrome_WidgetWin_", 17) == 0 ||
           wcscmp(cls, L"MozillaWindowClass") == 0;
}

// Optional observer for candidates scan_window_uia rejects (hint_check.c).
// NULL in the app, so the only cost there is the pointer test.
static void (*g_scan_debug)(const RECT *rc, int control_type,
                            const WCHAR *reason);

static void scan_window_uia(UiaEngine *engine, HWND hwnd,
                            const RECT *zrects, int z, const RECT *monitor,
                            ScanResult *out);

static void scan_window(UiaEngine *engine, HWND hwnd, const RECT *zrects,
                        int z, const RECT *monitor, ScanResult *out)
{
    DWORD pid = 0, started;

    GetWindowThreadProcessId(hwnd, &pid);
    if (slow_check(pid, FALSE)) {
        out->items = NULL;
        out->count = 0;
        out->cap = 0;
        return;
    }
    started = GetTickCount();
    scan_window_uia(engine, hwnd, zrects, z, monitor, out);
    if (GetTickCount() - started > SLOW_SCAN_MS &&
        out->count <= SLOW_SCAN_TARGETS && !lazy_tree_window(hwnd))
        slow_check(pid, TRUE);
}

static void scan_window_uia(UiaEngine *engine, HWND hwnd,
                            const RECT *zrects, int z, const RECT *monitor,
                            ScanResult *out)
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

        target.actionable = element_actionable(el);
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
    // non-actionable element keep only inner == 1 with mostly free space.
    // Smaller targets that duplicate each other count once: Google wraps each
    // result's menu button in a clickable element of the same size, and that
    // pair alone must not turn the result link into a container. Decisions and
    // new click points use the original points and are applied afterwards.
    drop = cand_count ? (BOOL *)calloc((size_t)cand_count, sizeof(*drop))
                      : NULL;
    new_click = cand_count ? (POINT *)malloc((size_t)cand_count *
                                             sizeof(*new_click))
                           : NULL;
    if (drop && new_click) {
        for (i = 0; i < cand_count; ++i) {
            LONG64 area = rect_area(&cands[i].target.rc);
            LONG64 covered = 0;
            int inner = 0, first = -1, j;
            for (j = 0; j < cand_count; ++j) {
                const ClickTarget *t = &cands[j].target;
                if (j == i || rect_area(&t->rc) >= area ||
                    !PtInRect(&cands[i].target.rc, t->click))
                    continue;
                if (first >= 0 &&
                    rects_duplicate(&cands[first].target.rc,
                                    cands[first].target.click, &t->rc,
                                    t->click))
                    continue;
                if (first < 0)
                    first = j;
                ++inner;
                covered += rect_intersection_area(
                    &t->rc, &cands[i].target.rc);
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
                    cands, cand_count, i, zrects, z);
            if (drop[i] && g_scan_debug)
                g_scan_debug(&cands[i].target.rc, cands[i].control_type,
                             inner >= 2 ? L"container" : L"wrapper");
        }
    }
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
    HANDLE done;
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
    ScanTask *tasks;
    int task_count;
    volatile LONG next;
    volatile LONG refs;
    HANDLE refs_event;
} ScanJob;

static HANDLE g_helper_threads[UIA_HELPER_THREADS];
static int g_helper_count;
static HANDLE g_job_sem;
static HANDLE g_pool_quit;
static ScanJob *g_job;
static CRITICAL_SECTION g_pool_cs;
static BOOL g_pool_cs_ready;

static void job_release_ref(ScanJob *job)
{
    if (InterlockedDecrement(&job->refs) == 0)
        SetEvent(job->refs_event);
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
                SetEvent(task->done);
                continue;
            }
            started = GetTickCount();
            scan_window(job->engine, task->hwnd, job->zrects, task->z,
                        &job->monitor, &task->found);
            task->elapsed_ms = GetTickCount() - started;
            SetEvent(task->done);
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
    int *order = NULL;
    int *task_of_win = NULL;
    int order_count = 0;
    int task_count = 0;
    int fg_task = -1;
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

    job = (ScanJob *)calloc(1, sizeof(*job));
    if (!job)
        goto done;
    job->engine = engine;
    job->zrects = zrects;
    job->zcount = zcount;
    job->monitor = mi.rcMonitor;
    job->keep_going = keep_going;
    job->keep_user = keep_user;
    job->task_count = task_count;
    if (task_count > 0) {
        job->tasks = (ScanTask *)calloc((size_t)task_count,
                                        sizeof(*job->tasks));
        if (!job->tasks)
            goto done;
        for (i = 0; i < task_count; ++i) {
            job->tasks[i].done = CreateEventW(NULL, FALSE, FALSE, NULL);
            if (!job->tasks[i].done)
                goto done;
        }
        for (k = 0; k < order_count; ++k) {
            int ti = task_of_win[order[k]];
            if (ti >= 0) {
                job->tasks[ti].hwnd = ctx.items[order[k]].hwnd;
                job->tasks[ti].z = ctx.items[order[k]].z;
            }
        }
    }
    job->refs_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!job->refs_event)
        goto done;

    // Claim the foreground task before the helpers see the job.
    if (fg_index >= 0 && task_of_win[fg_index] >= 0) {
        fg_task = task_of_win[fg_index];
        job->next = fg_task + 1;
    }

    EnterCriticalSection(&g_pool_cs);
    g_job = job;
    job->refs = 1;
    LeaveCriticalSection(&g_pool_cs);
    if (g_helper_count > 0 && g_job_sem)
        ReleaseSemaphore(g_job_sem, (LONG)g_helper_count, NULL);

    // The foreground window starts first, on this thread.
    if (fg_task >= 0) {
        ScanTask *task = &job->tasks[fg_task];
        if (!(keep_going && !keep_going(keep_user))) {
            DWORD started = GetTickCount();
            scan_window(engine, task->hwnd, job->zrects, task->z,
                        &job->monitor, &task->found);
            task->elapsed_ms = GetTickCount() - started;
        }
        SetEvent(task->done);
    }

    for (k = 0; k < order_count; ++k) {
        int wi = order[k];
        WinInfo *win = &ctx.items[wi];
        int ti = task_of_win[wi];
        ScanTask *task;

        if (keep_going && !keep_going(keep_user))
            break;
        if (!win->do_scan) {
            report_skip(cb, cb_user, win, result);
            continue;
        }

        task = &job->tasks[ti];
        if (ti != fg_task) {
            for (;;) {
                LONG next = job->next;
                if (next == ti &&
                    InterlockedCompareExchange(&job->next, ti + 1, ti) ==
                        ti) {
                    if (!(keep_going && !keep_going(keep_user))) {
                        DWORD started = GetTickCount();
                        scan_window(engine, task->hwnd, job->zrects, task->z,
                                    &job->monitor, &task->found);
                        task->elapsed_ms = GetTickCount() - started;
                    }
                    SetEvent(task->done);
                    break;
                }
                if (next > ti) {
                    WaitForSingleObject(task->done, INFINITE);
                    break;
                }
                Sleep(0);
            }
            if (keep_going && !keep_going(keep_user))
                break;
        }

        {
            int before = result->count;
            int changes = 0;
            int j;

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
                event.targets = result->items;
                cb(cb_user, &event);
            }
        }
    }

    EnterCriticalSection(&g_pool_cs);
    g_job = NULL;
    LeaveCriticalSection(&g_pool_cs);
    job_release_ref(job);
    WaitForSingleObject(job->refs_event, INFINITE);

done:
    if (job) {
        if (job->tasks) {
            for (i = 0; i < job->task_count; ++i) {
                if (job->tasks[i].done)
                    CloseHandle(job->tasks[i].done);
                free(job->tasks[i].found.items);
            }
            free(job->tasks);
        }
        if (job->refs_event)
            CloseHandle(job->refs_event);
        free(job->zrects);
        free(job);
    } else {
        free(zrects);
    }
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

static BOOL uia_engine_init(UiaEngine *engine, BOOL with_names)
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

static void worker_on_window(void *user, const WindowEvent *event)
{
    WorkerScan *scan = (WorkerScan *)user;

    if (!event->skip && event->changed > 0)
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
        com_ready = TRUE;
        if (uia_engine_init(&engine, FALSE)) {
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
            SetEvent(g_uia_ready);
            uia_pool_stop();
        }
        CoUninitialize();
    } else {
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
