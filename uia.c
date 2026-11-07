// uia.c - UI Automation element scan on a worker thread (see common.h).

#include "common.h"
#include <math.h>
#include <objbase.h>
#include <oleauto.h>
#include <stdarg.h>
#include <stdio.h>
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
#ifndef WS_EX_NOACTIVATE
#define WS_EX_NOACTIVATE 0x08000000L
#endif

#define UIA_TREESCOPE_DESCENDANTS 4
#define UIA_TREESCOPE_CHILDREN 2
#define UIA_TREESCOPE_SUBTREE 7
#define UIA_MODE_NONE 0
#define UIA_MODE_FULL 1
#define UIA_SCAN_TIMEOUT_MS 1500
#define UIA_MAX_TARGETS 2000
#define UIA_HELPER_THREADS 8

// Splitting a classic window's scan: containers this far below the root are
// asked in parts of their own, and one with more children than this is left
// to a single FindAll (a file list with hundreds of rows costs less that way
// than a children call plus that many work items).
#define SCAN_SPLIT_MAX_DEPTH 3
#define SCAN_SPLIT_MAX_CHILDREN 16
#define SCAN_PATH_MAX 8

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
#ifndef UIA_IsKeyboardFocusablePropertyId
#define UIA_IsKeyboardFocusablePropertyId 30009
#endif
#ifndef UIA_IsScrollPatternAvailablePropertyId
#define UIA_IsScrollPatternAvailablePropertyId 30034
#endif

#ifndef UIA_ScrollPatternId
#define UIA_ScrollPatternId 10004
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
#ifndef UIA_MenuControlTypeId
#define UIA_MenuControlTypeId 50009
#endif
#ifndef UIA_WindowControlTypeId
#define UIA_WindowControlTypeId 50032
#endif
// The container types the scan splits, and the title bar (its system menu is
// a MenuBar under it). Some SDKs name these further down this file, so each
// is guarded and so are the later definitions.
#ifndef UIA_CustomControlTypeId
#define UIA_CustomControlTypeId 50025
#endif
#ifndef UIA_GroupControlTypeId
#define UIA_GroupControlTypeId 50026
#endif
#ifndef UIA_PaneControlTypeId
#define UIA_PaneControlTypeId 50033
#endif
#ifndef UIA_TitleBarControlTypeId
#define UIA_TitleBarControlTypeId 50037
#endif
#ifndef UIA_MenuBarControlTypeId
#define UIA_MenuBarControlTypeId 50010
#endif
// Types a keyboard-focusable element may have without being worth a click
// (focusable_left_out).
#ifndef UIA_StatusBarControlTypeId
#define UIA_StatusBarControlTypeId 50017
#endif
#ifndef UIA_TabControlTypeId
#define UIA_TabControlTypeId 50018
#endif
#ifndef UIA_TextControlTypeId
#define UIA_TextControlTypeId 50020
#endif
#ifndef UIA_ToolBarControlTypeId
#define UIA_ToolBarControlTypeId 50021
#endif
#ifndef UIA_TreeControlTypeId
#define UIA_TreeControlTypeId 50023
#endif
#ifndef UIA_ThumbControlTypeId
#define UIA_ThumbControlTypeId 50027
#endif
#ifndef UIA_HeaderControlTypeId
#define UIA_HeaderControlTypeId 50034
#endif
#ifndef UIA_TableControlTypeId
#define UIA_TableControlTypeId 50036
#endif
#ifndef UIA_SeparatorControlTypeId
#define UIA_SeparatorControlTypeId 50038
#endif

typedef void *UIA_HWND;

typedef struct IUIAutomation IUIAutomation;
typedef struct IUIAutomation2 IUIAutomation2;
typedef struct IUIAutomationElement IUIAutomationElement;
typedef struct IUIAutomationElementArray IUIAutomationElementArray;
typedef struct IUIAutomationCacheRequest IUIAutomationCacheRequest;
typedef struct IUIAutomationCondition IUIAutomationCondition;
typedef struct IUIAutomationTreeWalker IUIAutomationTreeWalker;
typedef struct IUIAutomationScrollPattern IUIAutomationScrollPattern;

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
    HRESULT (STDMETHODCALLTYPE *ElementFromPoint)(IUIAutomation *This,
        POINT pt, IUIAutomationElement **element);
    void *GetFocusedElement;
    void *GetRootElementBuildCache;
    void *ElementFromHandleBuildCache;
    void *ElementFromPointBuildCache;
    HRESULT (STDMETHODCALLTYPE *GetFocusedElementBuildCache)(IUIAutomation *This,
        IUIAutomationCacheRequest *cacheRequest, IUIAutomationElement **element);
    void *CreateTreeWalker;
    void *get_ControlViewWalker;
    void *get_ContentViewWalker;
    HRESULT (STDMETHODCALLTYPE *get_RawViewWalker)(IUIAutomation *This,
        IUIAutomationTreeWalker **walker);
    void *get_RawViewCondition;
    void *get_ControlViewCondition;
    void *get_ContentViewCondition;
    HRESULT (STDMETHODCALLTYPE *CreateCacheRequest)(IUIAutomation *This,
        IUIAutomationCacheRequest **cacheRequest);
    HRESULT (STDMETHODCALLTYPE *CreateTrueCondition)(IUIAutomation *This,
        IUIAutomationCondition **newCondition);
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
    // esc_unfocus: the page Document takes the focus from a text field.
    HRESULT (STDMETHODCALLTYPE *SetFocus)(IUIAutomationElement *This);
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
    HRESULT (STDMETHODCALLTYPE *GetCurrentPatternAs)(IUIAutomationElement *This,
        int patternId, REFIID riid, void **patternObject);
    void *GetCachedPatternAs;
    void *GetCurrentPattern;
    void *GetCachedPattern;
    void *GetCachedParent;
    void *GetCachedChildren;
    HRESULT (STDMETHODCALLTYPE *get_CurrentProcessId)(
        IUIAutomationElement *This, int *retVal);
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

// The raw view's parent walk, used to find the scrollable area that contains
// the element under a point (a web page's list inside its document).
typedef struct IUIAutomationTreeWalkerVtbl {
    void *QueryInterface;
    void *AddRef;
    ULONG (STDMETHODCALLTYPE *Release)(IUIAutomationTreeWalker *This);
    HRESULT (STDMETHODCALLTYPE *GetParentElement)(IUIAutomationTreeWalker *This,
        IUIAutomationElement *element, IUIAutomationElement **parent);
    void *GetFirstChildElement;
    void *GetLastChildElement;
    void *GetNextSiblingElement;
    void *GetPreviousSiblingElement;
    void *NormalizeElement;
    // The cached walks of label_stand_in (a hidden checkbox's label).
    HRESULT (STDMETHODCALLTYPE *GetParentElementBuildCache)(
        IUIAutomationTreeWalker *This, IUIAutomationElement *element,
        IUIAutomationCacheRequest *cacheRequest, IUIAutomationElement **parent);
    HRESULT (STDMETHODCALLTYPE *GetFirstChildElementBuildCache)(
        IUIAutomationTreeWalker *This, IUIAutomationElement *element,
        IUIAutomationCacheRequest *cacheRequest, IUIAutomationElement **first);
    void *GetLastChildElementBuildCache;
    HRESULT (STDMETHODCALLTYPE *GetNextSiblingElementBuildCache)(
        IUIAutomationTreeWalker *This, IUIAutomationElement *element,
        IUIAutomationCacheRequest *cacheRequest, IUIAutomationElement **next);
    HRESULT (STDMETHODCALLTYPE *GetPreviousSiblingElementBuildCache)(
        IUIAutomationTreeWalker *This, IUIAutomationElement *element,
        IUIAutomationCacheRequest *cacheRequest,
        IUIAutomationElement **previous);
    void *NormalizeElementBuildCache;
    void *get_Condition;
} IUIAutomationTreeWalkerVtbl;

typedef struct IUIAutomationScrollPatternVtbl {
    void *QueryInterface;
    void *AddRef;
    ULONG (STDMETHODCALLTYPE *Release)(IUIAutomationScrollPattern *This);
    void *Scroll;
    HRESULT (STDMETHODCALLTYPE *SetScrollPercent)(
        IUIAutomationScrollPattern *This, double horizontalPercent,
        double verticalPercent);
    void *get_CurrentHorizontalScrollPercent;
    HRESULT (STDMETHODCALLTYPE *get_CurrentVerticalScrollPercent)(
        IUIAutomationScrollPattern *This, double *retVal);
    void *get_CurrentHorizontalViewSize;
    void *get_CurrentVerticalViewSize;
    void *get_CurrentHorizontallyScrollable;
    HRESULT (STDMETHODCALLTYPE *get_CurrentVerticallyScrollable)(
        IUIAutomationScrollPattern *This, BOOL *retVal);
    void *get_CachedHorizontalScrollPercent;
    void *get_CachedVerticalScrollPercent;
    void *get_CachedHorizontalViewSize;
    void *get_CachedVerticalViewSize;
    void *get_CachedHorizontallyScrollable;
    void *get_CachedVerticallyScrollable;
} IUIAutomationScrollPatternVtbl;

struct IUIAutomation { IUIAutomationVtbl *lpVtbl; };
struct IUIAutomation2 { IUIAutomation2Vtbl *lpVtbl; };
struct IUIAutomationElement { IUIAutomationElementVtbl *lpVtbl; };
struct IUIAutomationElementArray { IUIAutomationElementArrayVtbl *lpVtbl; };
struct IUIAutomationCacheRequest { IUIAutomationCacheRequestVtbl *lpVtbl; };
struct IUIAutomationCondition { IUIAutomationConditionVtbl *lpVtbl; };
struct IUIAutomationTreeWalker { IUIAutomationTreeWalkerVtbl *lpVtbl; };
struct IUIAutomationScrollPattern { IUIAutomationScrollPatternVtbl *lpVtbl; };

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
_Static_assert(sizeof(IUIAutomationTreeWalkerVtbl) == 16 * sizeof(void *),
               "IUIAutomationTreeWalker vtable size");
_Static_assert(sizeof(IUIAutomationScrollPatternVtbl) == 17 * sizeof(void *),
               "IUIAutomationScrollPattern vtable size");

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
static const GUID g_iid_scroll_pattern = {
    0x88f4d42a, 0xe881, 0x459d,
    { 0xa7, 0x7c, 0x73, 0xbb, 0xbb, 0x7e, 0x02, 0xdc }
};

typedef struct {
    IUIAutomation *automation;
    IUIAutomationCondition *condition;
    IUIAutomationCacheRequest *cache;
    // The same properties with full element references, which only
    // label_stand_in's walks need and which cost about a fifth more per
    // scan: used for a window whose last scan saw a hidden checkbox
    // (find_all_window). NULL when it could not be built: cache then.
    IUIAutomationCacheRequest *full_cache;
    // Browser windows (scan_web_window): the same plus keyboard-focusable
    // elements, which a page can make clickable without any pattern (the
    // click handler of a div with tabindex often sits on an ancestor, as
    // React attaches it). NULL when it could not be built: condition then.
    IUIAutomationCondition *web_condition;
    // log = 2: the same element types without the offscreen and enabled
    // tests, and a cache that also holds those two and the name, so the log
    // can say what UI Automation itself left out.
    IUIAutomationCondition *detail_condition;
    IUIAutomationCondition *web_detail_condition;
    IUIAutomationCacheRequest *detail_cache;
    // The children call that starts a split scan. It holds the same
    // properties as the cache in use plus IsEnabled and IsOffscreen (the
    // local condition test needs them), and Full element mode so the
    // children can be searched under. One per engine, next to the others.
    IUIAutomationCacheRequest *kids_cache;
    IUIAutomationCacheRequest *detail_kids_cache;
    IUIAutomationCondition *true_condition;
    BOOL with_names;
} UiaEngine;

typedef struct {
    ClickTarget target;
    int control_type;
    BOOL actionable;
    int order;                  // index in FindAll's (tree) order
    WCHAR name[128];
} ScanTarget;

// log = 2 collects its lines here and writes them after the scan is posted:
// app_log opens vim.log for every line, which must not slow the scan down.
typedef struct {
    WCHAR *text;                // lines separated by '\n'
    size_t len;
    size_t cap;
} DetailText;

typedef struct {
    ScanTarget *items;
    int count;
    int cap;
    BOOL fg_done;               // uia_scan_monitor: foreground window merged
    BOOL page_busy;             // uia_scan_monitor: it is a Chromium page still loading
    int pending;                // uia_scan_monitor: windows not merged yet
    DetailText detail;          // log = 2: what the scan made of each element
} ScanResult;

static BOOL page_busy(HWND root);

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

static void cached_name(IUIAutomationElement *el, WCHAR *out, int cch)
{
    BSTR name = NULL;

    out[0] = 0;
    if (SUCCEEDED(el->lpVtbl->get_CachedName(el, &name)) && name)
        lstrcpynW(out, name, cch);
    if (name)
        SysFreeString(name);
}

static void detail_add(DetailText *d, const WCHAR *fmt, ...)
{
    WCHAR line[512];
    va_list ap;
    size_t n;

    va_start(ap, fmt);
    _vsnwprintf(line, 511, fmt, ap);
    va_end(ap);
    line[511] = 0;
    n = wcslen(line);
    if (d->len + n + 2 > d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 8192;
        WCHAR *text;
        while (cap < d->len + n + 2)
            cap *= 2;
        text = (WCHAR *)realloc(d->text, cap * sizeof(*text));
        if (!text)
            return;
        d->text = text;
        d->cap = cap;
    }
    memcpy(d->text + d->len, line, n * sizeof(*line));
    d->len += n;
    d->text[d->len++] = L'\n';
    d->text[d->len] = 0;
}

// Moves src's lines to the end of d.
static void detail_take(DetailText *d, DetailText *src)
{
    WCHAR *line = src->text;

    while (line && *line) {
        WCHAR *next = wcschr(line, L'\n');
        if (next)
            *next++ = 0;
        else
            next = line + wcslen(line);
        detail_add(d, L"%ls", line);
        line = next;
    }
    free(src->text);
    memset(src, 0, sizeof(*src));
}

static void detail_flush(DetailText *d)
{
    WCHAR *line = d->text;

    while (line && *line) {
        WCHAR *next = wcschr(line, L'\n');
        if (next)
            *next++ = 0;
        else
            next = line + wcslen(line);
        app_log(L"%ls", line);
        line = next;
    }
    free(d->text);
    memset(d, 0, sizeof(*d));
}

static void control_type_label(int id, WCHAR *out, int cch)
{
    static const struct { int id; const WCHAR *name; } names[] = {
        { UIA_ButtonControlTypeId, L"Button" },
        { UIA_CheckBoxControlTypeId, L"CheckBox" },
        { UIA_ComboBoxControlTypeId, L"ComboBox" },
        { UIA_EditControlTypeId, L"Edit" },
        { UIA_HyperlinkControlTypeId, L"Hyperlink" },
        { UIA_ListItemControlTypeId, L"ListItem" },
        { UIA_ListControlTypeId, L"List" },
        { UIA_MenuItemControlTypeId, L"MenuItem" },
        { UIA_RadioButtonControlTypeId, L"RadioButton" },
        { UIA_TabItemControlTypeId, L"TabItem" },
        { UIA_TreeItemControlTypeId, L"TreeItem" },
        { UIA_DataItemControlTypeId, L"DataItem" },
        { UIA_DocumentControlTypeId, L"Document" },
        { UIA_SplitButtonControlTypeId, L"SplitButton" },
        { UIA_ScrollBarControlTypeId, L"ScrollBar" },
        { UIA_HeaderItemControlTypeId, L"HeaderItem" },
        { UIA_MenuControlTypeId, L"Menu" },
        { UIA_WindowControlTypeId, L"Dialog" }
    };
    int i;

    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); ++i) {
        if (names[i].id == id) {
            lstrcpynW(out, names[i].name, cch);
            return;
        }
    }
    _snwprintf(out, (size_t)cch, L"Type%d", id);
    out[cch - 1] = 0;
}

// One log = 2 line: type, position and size, what became of it, and the name.
static void detail_element(DetailText *d, const WCHAR *name, const RECT *rc,
                           int control_type, const WCHAR *what,
                           const POINT *click)
{
    WCHAR type[24];
    WCHAR clean[61];
    WCHAR at[40];
    int i;

    control_type_label(control_type, type, 24);
    for (i = 0; name && name[i] && i < 60; ++i)
        clean[i] = name[i] < 32 ? L' ' : name[i];
    clean[i] = 0;
    at[0] = 0;
    if (click) {
        _snwprintf(at, 39, L" at %ld,%ld", click->x, click->y);
        at[39] = 0;
    }
    detail_add(d, L"scan:   %ls %ld,%ld %ldx%ld %ls%ls \"%ls\"", type,
               rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
               what, at, clean);
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
    BOOL above;                 // above the desktop band (add_unlisted_windows)
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
// one entry per candidate, in index order. cands must be in tree order.
static void filter_containers(const ScanTarget *cands, int count,
                              const RECT *zrects, int z,
                              void (*debug)(const RECT *, int, const WCHAR *),
                              BOOL *drop, POINT *new_click)
{
    ClickGrid grid;
    LONG64 *areas;
    int *matches;
    int *content_end;
    int i;

    for (i = 0; i < count; ++i) {
        drop[i] = FALSE;
        new_click[i] = cands[i].target.click;
    }
    if (count < 2)
        return;

    areas = (LONG64 *)malloc((size_t)count * sizeof(*areas));
    matches = (int *)malloc((size_t)count * sizeof(*matches));
    content_end = (int *)malloc((size_t)count * sizeof(*content_end));
    if (!areas || !matches || !content_end ||
        !click_grid_build(cands, count, &grid)) {
        free(areas);
        free(matches);
        free(content_end);
        return;
    }
    for (i = 0; i < count; ++i)
        areas[i] = rect_area(&cands[i].target.rc);

    // Only an element's own content can make it a container. FindAll lists
    // elements in tree order, so its descendants follow it without a break;
    // the run of candidates right after it whose click points are inside it
    // stands in for them. A panel or menu drawn over a page is not the
    // content of the page elements under it, nor they of its rows: counting
    // them dropped the rows of Google's account menu and Zendesk's overlays
    // as containers of the links behind them.
    for (i = 0; i < count; ++i) {
        int e = i + 1;
        while (e < count &&
               PtInRect(&cands[i].target.rc, cands[e].target.click))
            ++e;
        content_end[i] = e;
    }

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
            int j = matches[m];
            const ClickTarget *t = &cands[j].target;
            if (j <= i || j >= content_end[i])
                continue;
            if (first >= 0 &&
                rects_duplicate(&cands[first].target.rc,
                                cands[first].target.click, &t->rc, t->click))
                continue;
            if (first < 0)
                first = j;
            ++inner;
            covered += rect_intersection_area(&t->rc, rc);
        }

        if (inner == 0)
            drop[i] = FALSE;
        else if (cands[i].actionable)
            drop[i] = !(inner == 1 || covered * 2 < area);
        else
            drop[i] = !(inner == 1 && covered * 4 < area);

        // The click point still keeps off every smaller target on it, its
        // own or one drawn over it.
        new_click[i] = cands[i].target.click;
        if (!drop[i] && matched > 0)
            new_click[i] = choose_outer_click_point(
                cands, matches, matched, i, zrects, z);
        if (drop[i] && debug)
            debug(rc, cands[i].control_type,
                  inner >= 2 ? L"container" : L"wrapper");
    }

    free(content_end);
    free(matches);
    free(areas);
    free(grid.order);
    free(grid.start);
}

// A dialog, menu or frame drawn over a web page. The page's elements under it
// stay in the tree (Chromium leaves them out only under an aria-modal
// dialog), so they got labels whose click points landed on the overlay:
// Google's knowledge panel links under the account menu, ticket links under
// a Zendesk menu or modal. UI Automation cannot say which of two overlapping
// elements is on top (Chromium's own ElementFromPoint is a bounds search
// that ignores z-index), but a dialog, an open menu or a frame is.
typedef struct {
    RECT rc;
    int order;                  // its index in FindAll's (tree) order
    int end;                    // first index after its content
    BOOL drops;                 // a dialog or menu: what it hides entirely goes
} Overlay;

enum { COVER_NONE = 0, COVER_MOVED = 1, COVER_DROPPED = 2 };

// For each candidate whose click point is on an overlay it is not part of:
// keep the largest part of it outside the overlay as its rect (the label
// goes there) and click point. When none is left, drop it if a dialog or
// menu that comes after it in the page covers it (a portal over the page);
// otherwise keep it as it is (an element on top of a frame, say a sticky
// header, loses nothing either way). fate receives COVER_* per candidate.
static void cover_overlays(ScanTarget *cands, int count, const Overlay *ovs,
                           int ov_count, const RECT *zrects, int z, int *fate)
{
    int i, o, k;

    for (i = 0; i < count; ++i) {
        ScanTarget *c = &cands[i];
        const RECT *rc = &c->target.rc;
        RECT strips[4 * 8];
        int strip_count = 0;
        BOOL covered = FALSE, under_portal = FALSE, moved = FALSE;

        fate[i] = COVER_NONE;
        for (o = 0; o < ov_count; ++o) {
            const Overlay *ov = &ovs[o];
            RECT r;
            if (!PtInRect(&ov->rc, c->target.click) ||
                (c->order >= ov->order && c->order < ov->end))
                continue;
            covered = TRUE;
            if (ov->drops && c->order < ov->order)
                under_portal = TRUE;
            if (strip_count > 4 * 7)
                continue;
            r = *rc;                            // left of the overlay
            if (ov->rc.left < r.right) r.right = ov->rc.left;
            if (r.right > r.left) strips[strip_count++] = r;
            r = *rc;                            // right of it
            if (ov->rc.right > r.left) r.left = ov->rc.right;
            if (r.right > r.left) strips[strip_count++] = r;
            r = *rc;                            // above it
            if (ov->rc.top < r.bottom) r.bottom = ov->rc.top;
            if (r.bottom > r.top) strips[strip_count++] = r;
            r = *rc;                            // below it
            if (ov->rc.bottom > r.top) r.top = ov->rc.bottom;
            if (r.bottom > r.top) strips[strip_count++] = r;
        }
        if (!covered)
            continue;

        // Largest strip first; its click point must be off every overlay
        // that is not the candidate's own.
        while (!moved && strip_count > 0) {
            int best = 0;
            POINT pt;
            BOOL clear = TRUE;
            for (k = 1; k < strip_count; ++k) {
                if (rect_area(&strips[k]) > rect_area(&strips[best]))
                    best = k;
            }
            if (strips[best].right - strips[best].left >= 4 &&
                strips[best].bottom - strips[best].top >= 4) {
                pt = choose_click_point(zrects, z, &strips[best]);
                if (pt.x < 0 && pt.y < 0)
                    clear = FALSE;
                for (o = 0; clear && o < ov_count; ++o) {
                    if (PtInRect(&ovs[o].rc, pt) &&
                        !(c->order >= ovs[o].order && c->order < ovs[o].end))
                        clear = FALSE;
                }
                if (clear) {
                    c->target.rc = strips[best];
                    c->target.click = pt;
                    moved = TRUE;
                }
            }
            strips[best] = strips[--strip_count];
        }
        if (moved)
            fate[i] = COVER_MOVED;
        else if (under_portal)
            fate[i] = COVER_DROPPED;
    }
}

// Chromium can call nearly everything inside an open web dialog or menu
// offscreen. Its IsOffscreen walks its stored copy of the page, clipping by
// every box that clips its children, and a box inside the menu (the list
// that scrolls) can keep a stale, empty size there after the menu opens.
// BoundingRectangle is unclipped and still right. Google's account menu on
// a results page did this on Windows (2026-11-01): the "Account and
// settings" dialog and its Close button were on screen, all 17 rows of its
// scrolling list offscreen. A visible dialog shows most of what lies inside
// it, so when the offscreen elements inside one outnumber its on-screen
// ones, Chromium's clip is wrong and they are taken back
// (rescue_offscreen in scan_window_uia).
#define RESCUE_ONSCREEN_MAX 3   // more on screen: Chromium's clip is fine

// rescue_rect: the part of an element inside the dialog or menu, when that
// is at least half of it (rows scrolled past its edge stay out).
static BOOL rescue_rect(const RECT *rc, const RECT *popup, RECT *out)
{
    LONG64 a = rect_area(rc);

    if (a <= 0 || !IntersectRect(out, rc, popup))
        return FALSE;
    return rect_area(out) * 2 >= a;
}

// An on-screen element of the dialog or menu itself: in its run, mostly
// inside it, smaller than half of it (not a wrapper), and not a Document,
// dialog or menu.
static BOOL popup_own(const RECT *raw, const int *types, int e,
                      const RECT *popup)
{
    RECT in;

    return rect_area(&raw[e]) > 0 &&
           rect_area(&raw[e]) * 2 < rect_area(popup) &&
           types[e] != UIA_DocumentControlTypeId &&
           types[e] != UIA_WindowControlTypeId &&
           types[e] != UIA_MenuControlTypeId &&
           rescue_rect(&raw[e], popup, &in);
}

// How many on-screen elements the dialog or menu at index p has (popup_own):
// in the run after it in tree order whose rects touch it, as an overlay's
// content (raw: on-screen rects, zero for an element left out; types:
// control types). *end gets the first index after the run.
static int popup_onscreen(const RECT *raw, const int *types, int count,
                          int p, const RECT *popup, int *end)
{
    int n = 0;
    int e;

    for (e = p + 1; e < count; ++e) {
        RECT inter;
        if (rect_area(&raw[e]) <= 0)
            continue;
        if (!IntersectRect(&inter, &raw[e], popup))
            break;
        if (popup_own(raw, types, e, popup))
            ++n;
    }
    *end = e;
    return n;
}

// Of n offscreen elements inside the dialog or menu at index p (off[]: their
// rescue_rect), marks in take[] the ones to take back and returns how many.
// None unless they outnumber its own on-screen elements (popup_onscreen, its
// run ends at end), and at least two. Not one mostly under an own on-screen
// element (hidden behind it, or the same thing listed twice). None when they
// and the own ones fill the popup's rect exactly: Chromium gives a box of no
// size its children's bounds, which is what a menu closed by height 0 looks
// like.
static int rescue_pick(const RECT *raw, const int *types, int p, int end,
                       const RECT *popup, int onscreen, const RECT *off,
                       int n, BOOL *take)
{
    RECT all;
    int picked = 0;
    int i, e;

    for (i = 0; i < n; ++i)
        take[i] = FALSE;
    if (onscreen > RESCUE_ONSCREEN_MAX)
        return 0;
    SetRectEmpty(&all);
    for (i = 0; i < n; ++i) {
        take[i] = TRUE;
        for (e = p + 1; e < end && take[i]; ++e) {
            RECT inter;
            if (!popup_own(raw, types, e, popup))
                continue;
            if (IntersectRect(&inter, &off[i], &raw[e]) &&
                rect_area(&inter) * 2 >= rect_area(&off[i]))
                take[i] = FALSE;
        }
        if (take[i]) {
            UnionRect(&all, &all, &off[i]);
            ++picked;
        }
    }
    if (picked < 2 || picked <= onscreen) {
        for (i = 0; i < n; ++i)
            take[i] = FALSE;
        return 0;
    }
    for (e = p + 1; e < end; ++e)
        if (popup_own(raw, types, e, popup))
            UnionRect(&all, &all, &raw[e]);
    if (EqualRect(&all, popup)) {
        for (i = 0; i < n; ++i)
            take[i] = FALSE;
        return 0;
    }
    return picked;
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

// Chrome, Edge and Electron windows are Chrome_WidgetWin_*; WebView2 and CEF
// apps (the new Teams) host Chromium under a top-level window of their own
// class.
BOOL uia_chromium_window(HWND hwnd)
{
    WCHAR cls[64];
    BOOL chromium = FALSE;

    hwnd = hwnd ? GetAncestor(hwnd, GA_ROOT) : NULL;
    if (!hwnd)
        return FALSE;
    GetClassNameW(hwnd, cls, 64);
    if (wcsncmp(cls, L"Chrome_WidgetWin_", 17) == 0)
        return TRUE;
    EnumChildWindows(hwnd, find_chromium_child, (LPARAM)&chromium);
    return chromium;
}

BOOL uia_browser_window(HWND hwnd)
{
    WCHAR cls[64];

    hwnd = hwnd ? GetAncestor(hwnd, GA_ROOT) : NULL;
    if (!hwnd || !GetClassNameW(hwnd, cls, 64))
        return FALSE;
    if (wcscmp(cls, L"MozillaWindowClass") == 0)
        return TRUE;
    return uia_chromium_window(hwnd);
}

// Whether a window is searched with the web condition (scan_focusable):
// browsers and apps built on Chromium, where a page makes elements
// clickable that expose nothing but the keyboard focus.
static BOOL scan_web_window(HWND hwnd)
{
    return g_cfg.scan_focusable && uia_browser_window(hwnd);
}

typedef struct {
    POINT cursor;
    POINT prefer;
    BOOL has_prefer;
    BOOL prefer_on_page;    // prefer lies on one of the page areas
    RECT best;              // the page area under the cursor, else the largest
    BOOL under_cursor;
    BOOL found;
} PageAreaSearch;

// Chromium keeps a child window over each page it shows
// (Chrome_RenderWidgetHostHWND, "Chrome Legacy Window" in UI Automation).
static BOOL CALLBACK find_page_area(HWND hwnd, LPARAM lp)
{
    PageAreaSearch *search = (PageAreaSearch *)lp;
    WCHAR cls[64];
    RECT rc;
    BOOL under;

    if (!GetClassNameW(hwnd, cls, 64) ||
        wcscmp(cls, L"Chrome_RenderWidgetHostHWND") != 0 ||
        !IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &rc) ||
        rc.right <= rc.left || rc.bottom <= rc.top)
        return TRUE;
    if (search->has_prefer && PtInRect(&rc, search->prefer))
        search->prefer_on_page = TRUE;
    under = PtInRect(&rc, search->cursor);
    if (search->under_cursor && !under)
        return TRUE;
    if (!search->found || (under && !search->under_cursor) ||
        rect_area(&rc) > rect_area(&search->best)) {
        search->best = rc;
        search->found = TRUE;
        search->under_cursor = under;
    }
    return TRUE;
}

// A point of root that is on top (no other window, no menu over it).
static BOOL root_point_on_top(HWND root, POINT pt)
{
    HWND hit = WindowFromPoint(pt);

    return hit && GetAncestor(hit, GA_ROOT) == root;
}

BOOL uia_page_point(HWND root, const POINT *prefer, POINT *out)
{
    PageAreaSearch search;
    RECT rc;
    POINT pt;

    if (!root || !IsWindow(root))
        return FALSE;
    memset(&search, 0, sizeof(search));
    GetCursorPos(&search.cursor);
    if (prefer) {
        search.prefer = *prefer;
        search.has_prefer = TRUE;
    }
    EnumChildWindows(root, find_page_area, (LPARAM)&search);
    // Where we scrolled or clicked, unless it is the browser's own toolbar
    // or tab strip: a hit test there never reaches the page. Without page
    // area windows (an older or newer Chromium) any point on top will do.
    if (prefer && (search.prefer_on_page || !search.found) &&
        root_point_on_top(root, *prefer)) {
        *out = *prefer;
        return TRUE;
    }
    if (search.found) {
        pt.x = search.best.left + (search.best.right - search.best.left) / 2;
        pt.y = search.best.top + (search.best.bottom - search.best.top) / 2;
        if (root_point_on_top(root, pt)) {
            *out = pt;
            return TRUE;
        }
    }
    if (root_point_on_top(root, search.cursor)) {
        *out = search.cursor;
        return TRUE;
    }
    if (GetWindowRect(root, &rc)) {
        pt.x = rc.left + (rc.right - rc.left) / 2;
        pt.y = rc.top + (rc.bottom - rc.top) / 2;
        if (root_point_on_top(root, pt)) {
            *out = pt;
            return TRUE;
        }
    }
    return FALSE;
}

// Browsers and Electron apps build their tree lazily: a slow, nearly empty
// first scan there is normal and must not get them skipped. So does Explorer
// while a folder window it has just opened fills in.
static BOOL lazy_tree_window(HWND hwnd)
{
    WCHAR cls[64];

    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"MozillaWindowClass") == 0 ||
        wcscmp(cls, L"CabinetWClass") == 0 ||
        wcscmp(cls, L"ExploreWClass") == 0)
        return TRUE;
    return uia_chromium_window(hwnd);
}

// Chrome's panels (the downloads bubble, the extensions menu, a permission
// prompt) are owned WS_POPUP windows just like its menus, so popup_open counted
// them as open menus and bare keys typed into Chrome. Their styles tell them
// apart (Chromium's ui/views/widget/widget_hwnd_utils.cc, menu_host.cc): menus
// carry WS_EX_NOACTIVATE and WS_EX_TOPMOST (a menu with no parent widget only
// TOPMOST) and the address-bar dropdowns are NOACTIVATE, while a panel has
// neither. A panel takes the keyboard focus itself, and a letter there does
// nothing outside a text field. Only GetWindowLongW, GetClassNameW and
// GetWindow, so the keyboard hook may call this.
BOOL uia_chromium_panel(HWND hwnd)
{
    LONG style, exstyle;
    WCHAR cls[64];

    if (!hwnd)
        return FALSE;
    GetClassNameW(hwnd, cls, 64);
    if (wcsncmp(cls, L"Chrome_WidgetWin_", 17) != 0)
        return FALSE;
    style = GetWindowLongW(hwnd, GWL_STYLE);
    exstyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    return (style & WS_POPUP) != 0 &&
           GetWindow(hwnd, GW_OWNER) != NULL &&
           !(exstyle & (WS_EX_NOACTIVATE | WS_EX_TOPMOST));
}

// Optional observer for candidates scan_window_uia rejects (hint_check.c).
// NULL in the app, so the only cost there is the pointer test.
static void (*g_scan_debug)(const RECT *rc, int control_type,
                            const WCHAR *reason);

static void scan_window_uia(UiaEngine *engine, HWND hwnd,
                            const RECT *zrects, int z, const RECT *monitor,
                            ScanResult *out, int *answered, BOOL detail);

// "Next to nothing" counts every element the window answered with, not the
// targets kept: a window mostly covered by another keeps only a few. A scan
// dropped before it ended (Alt went on to scroll, not to the hint hotkey)
// ran while the app was busy, so it is no verdict.
// detail (log = 2): out->detail gets a line for every element and a summary.
// The app scans through the job below (which may split a window in parts);
// this synchronous one is what tools/hint_check.c calls.
static void __attribute__((unused))
scan_window(UiaEngine *engine, HWND hwnd, const RECT *zrects,
            int z, const RECT *monitor, ScanResult *out,
            UiaContinueCallback keep_going, void *keep_user, BOOL detail)
{
    DWORD pid = 0, started;
    int answered = 0;

    memset(&out->detail, 0, sizeof(out->detail));
    GetWindowThreadProcessId(hwnd, &pid);
    if (slow_check(hwnd, pid, FALSE)) {
        out->items = NULL;
        out->count = 0;
        out->cap = 0;
        if (detail)
            detail_add(&out->detail, L"scan: active window %p skipped: "
                       L"answered slowly before", (void *)hwnd);
        return;
    }
    started = GetTickCount();
    scan_window_uia(engine, hwnd, zrects, z, monitor, out, &answered, detail);
    if (detail)
        detail_add(&out->detail, L"scan: active window %p: UI Automation "
                   L"listed %d, %d kept, %lu ms", (void *)hwnd, answered,
                   out->count, (unsigned long)(GetTickCount() - started));
    if (GetTickCount() - started > SLOW_SCAN_MS &&
        answered <= SLOW_SCAN_TARGETS &&
        (!keep_going || keep_going(keep_user)) && !lazy_tree_window(hwnd))
        slow_check(hwnd, pid, TRUE);
}

// A checkbox or radio button a page hides behind its own label (Naver Mail's
// "select all": the input is a 1 px box, the <label> next to it is drawn as
// the square). The label is what shows and what a click must land on.
// Chromium lists such a <label> as a Group without a name, next to the box
// or around it when it wraps the input, so the box's stand-in is the next
// sibling, else the previous one, else the parent: a Group on screen, at
// least 4 px each way, under a 32nd of the monitor and within
// STAND_IN_NEAR_PX of the box. Each try is one call into the app, so a
// scan makes at most STAND_IN_MAX of them (scan_hidden_checkbox).
#define STAND_IN_MAX 40
#define STAND_IN_NEAR_PX 64

static BOOL stand_in_wanted(int control_type, const RECT *rc)
{
    return (control_type == UIA_CheckBoxControlTypeId ||
            control_type == UIA_RadioButtonControlTypeId) &&
           (rc->right - rc->left < 4 || rc->bottom - rc->top < 4);
}

// The gap between two rects in pixels (0 when they touch or overlap).
static LONG rect_gap(const RECT *a, const RECT *b)
{
    LONG dx = 0, dy = 0;

    if (b->left > a->right)
        dx = b->left - a->right;
    else if (a->left > b->right)
        dx = a->left - b->right;
    if (b->top > a->bottom)
        dy = b->top - a->bottom;
    else if (a->top > b->bottom)
        dy = a->top - b->bottom;
    return dx > dy ? dx : dy;
}

// TRUE when candidate (cached with kids_cache: rect, type, offscreen) is a
// label worth standing in for the box at rc; *out is then its rect.
// Releases candidate (NULL is allowed: nothing there).
static BOOL stand_in_take(IUIAutomationElement *candidate, const RECT *rc,
                          const RECT *monitor, RECT *out)
{
    int control_type = 0;
    RECT lab;
    BOOL ok;

    if (!candidate)
        return FALSE;
    ok = SUCCEEDED(candidate->lpVtbl->get_CachedControlType(candidate,
                                                            &control_type)) &&
         control_type == UIA_GroupControlTypeId &&
         !cached_bool_property(candidate, UIA_IsOffscreenPropertyId) &&
         SUCCEEDED(candidate->lpVtbl->get_CachedBoundingRectangle(candidate,
                                                                  &lab)) &&
         lab.right - lab.left >= 4 && lab.bottom - lab.top >= 4 &&
         rect_area(&lab) * 32 <= rect_area(monitor) &&
         rect_gap(rc, &lab) <= STAND_IN_NEAR_PX;
    candidate->lpVtbl->Release(candidate);
    if (ok)
        *out = lab;
    return ok;
}

static BOOL label_stand_in(IUIAutomationTreeWalker *walker,
                           IUIAutomationCacheRequest *cache,
                           IUIAutomationElement *el, const RECT *rc,
                           const RECT *monitor, RECT *out)
{
    IUIAutomationElement *c = NULL;

    if (SUCCEEDED(walker->lpVtbl->GetNextSiblingElementBuildCache(
            walker, el, cache, &c)) &&
        stand_in_take(c, rc, monitor, out))
        return TRUE;
    c = NULL;
    if (SUCCEEDED(walker->lpVtbl->GetPreviousSiblingElementBuildCache(
            walker, el, cache, &c)) &&
        stand_in_take(c, rc, monitor, out))
        return TRUE;
    c = NULL;
    return SUCCEEDED(walker->lpVtbl->GetParentElementBuildCache(
               walker, el, cache, &c)) &&
           stand_in_take(c, rc, monitor, out);
}

// An element the scan passes over. log = 2 writes why, with its name.
static void scan_reject(IUIAutomationElement *el, ScanResult *out,
                        BOOL detail, const RECT *rc, int control_type,
                        const WCHAR *reason)
{
    if (g_scan_debug)
        g_scan_debug(rc, control_type, reason);
    if (detail) {
        WCHAR name[128];
        cached_name(el, name, 128);
        detail_element(&out->detail, name, rc, control_type, reason, NULL);
    }
}

// What the normal condition leaves out (log = 2 asks without it).
static const WCHAR *detail_left_out(IUIAutomationElement *el)
{
    if (cached_bool_property(el, UIA_IsOffscreenPropertyId))
        return L"offscreen (UI Automation)";
    if (!cached_bool_property(el, UIA_IsEnabledPropertyId))
        return L"disabled (UI Automation)";
    return NULL;
}


// What shapes the rest of a window's scan: its scroll bars, Documents and
// web dialogs and menus.
typedef struct {
    RECT scrollbars[64];
    int scrollbar_count;
    RECT documents[16];
    int doc_order[16];
    int document_count;
    Overlay popups[16];         // dialogs and menus; end not known yet
    int popup_count;
} ScanLayout;

// Fills lay, and raw and types for every element in tree order: raw is zero
// for one that log = 2 lists but the normal condition leaves out, and a
// rescued element (rescued[i] not empty) has its rescued rect.
static void scan_layout(IUIAutomationElement **els, const RECT *rescued,
                        int count, BOOL detail, ScanLayout *lay, RECT *raw,
                        int *types)
{
    int i;

    memset(lay, 0, sizeof(*lay));
    for (i = 0; i < count; ++i) {
        IUIAutomationElement *el = els[i];
        int control_type = 0;
        RECT rc;

        SetRectEmpty(&raw[i]);
        types[i] = 0;
        if (!el ||
            FAILED(el->lpVtbl->get_CachedControlType(el, &control_type)))
            continue;
        types[i] = control_type;
        if (rect_area(&rescued[i]) > 0) {
            raw[i] = rescued[i];
            continue;
        }
        if ((detail && detail_left_out(el)) ||
            FAILED(el->lpVtbl->get_CachedBoundingRectangle(el, &rc)))
            continue;
        raw[i] = rc;
        if (control_type == UIA_ScrollBarControlTypeId &&
            lay->scrollbar_count < 64) {
            lay->scrollbars[lay->scrollbar_count++] = rc;
        } else if (control_type == UIA_DocumentControlTypeId &&
                   lay->document_count < 16) {
            lay->doc_order[lay->document_count] = i;
            lay->documents[lay->document_count++] = rc;
        } else if ((control_type == UIA_WindowControlTypeId ||
                    control_type == UIA_MenuControlTypeId) &&
                   lay->popup_count < 16 && rect_area(&rc) > 0) {
            Overlay *pop = &lay->popups[lay->popup_count++];
            pop->rc = rc;
            pop->order = i;
            pop->end = i + 1;
            pop->drops = TRUE;
        }
    }

    // A frame's Document is unclipped too and can reach past its page.
    // FindAll lists elements in tree order, so an earlier Document it
    // overlaps is the page around it.
    for (i = 1; i < lay->document_count; ++i) {
        int d;
        for (d = 0; d < i; ++d) {
            RECT inter;
            if (IntersectRect(&inter, &lay->documents[i], &lay->documents[d]))
                lay->documents[i] = inter;
        }
    }
}

// The page: the Document with most of itself on the monitor (-1: none).
static int page_document(const ScanLayout *lay, const RECT *monitor,
                         LONG64 *page_area)
{
    int page = -1, d;

    *page_area = 0;
    for (d = 0; d < lay->document_count; ++d) {
        RECT on;
        LONG64 area = IntersectRect(&on, &lay->documents[d], monitor)
                          ? rect_area(&on) : 0;
        if (area > *page_area) {
            *page_area = area;
            page = d;
        }
    }
    return page;
}

// A dialog or menu that is an overlay on the page: on it, and leaving part
// of it free (a web app that is one big dialog covers nothing).
static BOOL popup_on_page(const RECT *popup, const RECT *page_rc,
                          LONG64 page_area)
{
    RECT inter;

    return IntersectRect(&inter, popup, page_rc) &&
           rect_area(popup) * 10 < page_area * 9;
}

typedef struct {
    int after;                  // index of its dialog or menu
    IUIAutomationElement *el;   // NULL: already in els (log = 2) at index
    int index;
    RECT rc;
} Rescued;

#define RESCUE_MAX 256

// An offscreen element worth taking back into a dialog or menu: enabled,
// clickable (not a Document, dialog, menu or scroll bar) and mostly inside.
static BOOL rescue_candidate(IUIAutomationElement *el, int control_type,
                             const RECT *popup, RECT *clip)
{
    RECT rc;

    return control_type != UIA_DocumentControlTypeId &&
           control_type != UIA_WindowControlTypeId &&
           control_type != UIA_MenuControlTypeId &&
           control_type != UIA_ScrollBarControlTypeId &&
           cached_bool_property(el, UIA_IsOffscreenPropertyId) &&
           cached_bool_property(el, UIA_IsEnabledPropertyId) &&
           SUCCEEDED(el->lpVtbl->get_CachedBoundingRectangle(el, &rc)) &&
           rescue_rect(&rc, popup, clip);
}

// Chromium only (see popup_onscreen): a dialog or menu on the page with few
// elements on screen gets back its offscreen ones that rescue_pick takes,
// clipped to it. With log = 2 they are already in els (left out as
// offscreen) and are marked in rescued; otherwise they are asked for under
// the dialog or menu alone and inserted right after it, in tree order, so
// *els, *rescued and *count may be replaced. Returns how many came back.
static int rescue_offscreen(UiaEngine *engine, HWND hwnd,
                            IUIAutomationElement ***els_io,
                            RECT **rescued_io, int *count_io,
                            const ScanLayout *lay, const RECT *raw,
                            const int *types, const RECT *monitor,
                            BOOL detail, ScanResult *out)
{
    IUIAutomationElement **els = *els_io;
    RECT *rescued = *rescued_io;
    int count = *count_io;
    Rescued *found = NULL;      // taken, in tree order
    int found_count = 0;
    int inserted = 0;
    Rescued *cand = NULL;       // one popup's offscreen elements
    RECT *off = NULL;
    BOOL *take = NULL;
    int taken_until = -1;
    LONG64 page_area;
    int page;
    int k;

    if (lay->popup_count == 0 || lay->document_count == 0 ||
        !uia_chromium_window(hwnd))
        return 0;
    page = page_document(lay, monitor, &page_area);
    if (page < 0)
        return 0;

    for (k = 0; k < lay->popup_count && found_count < RESCUE_MAX; ++k) {
        const Overlay *pop = &lay->popups[k];
        int end, onscreen, n = 0, picked, e;

        // One inside a dialog or menu already taken came with it.
        if (pop->order < taken_until ||
            !popup_on_page(&pop->rc, &lay->documents[page], page_area))
            continue;
        onscreen = popup_onscreen(raw, types, count, pop->order, &pop->rc,
                                  &end);
        if (onscreen > RESCUE_ONSCREEN_MAX)
            continue;
        if (!cand) {
            cand = (Rescued *)malloc(RESCUE_MAX * sizeof(*cand));
            off = (RECT *)malloc(RESCUE_MAX * sizeof(*off));
            take = (BOOL *)malloc(RESCUE_MAX * sizeof(*take));
            found = (Rescued *)malloc(RESCUE_MAX * sizeof(*found));
            if (!cand || !off || !take || !found)
                break;
        }

        if (detail) {
            for (e = pop->order + 1; e < end && n < RESCUE_MAX; ++e) {
                RECT clip;
                if (rect_area(&raw[e]) > 0 || !els[e] ||
                    !rescue_candidate(els[e], types[e], &pop->rc, &clip))
                    continue;
                cand[n].after = pop->order;
                cand[n].el = NULL;
                cand[n].index = e;
                cand[n].rc = clip;
                off[n++] = clip;
            }
        } else {
            IUIAutomationElementArray *sub = NULL;
            int sub_length = 0;
            if (!els[pop->order] ||
                FAILED(els[pop->order]->lpVtbl->FindAllBuildCache(
                    els[pop->order], UIA_TREESCOPE_DESCENDANTS,
                    scan_web_window(hwnd) && engine->web_detail_condition
                        ? engine->web_detail_condition
                        : engine->detail_condition,
                    engine->detail_cache, &sub)) ||
                !sub)
                continue;
            if (FAILED(sub->lpVtbl->get_Length(sub, &sub_length)))
                sub_length = 0;
            for (e = 0; e < sub_length && n < RESCUE_MAX; ++e) {
                IUIAutomationElement *el = NULL;
                int control_type = 0;
                RECT clip;
                if (FAILED(sub->lpVtbl->GetElement(sub, e, &el)) || !el)
                    continue;
                if (FAILED(el->lpVtbl->get_CachedControlType(
                        el, &control_type)) ||
                    !rescue_candidate(el, control_type, &pop->rc, &clip)) {
                    el->lpVtbl->Release(el);
                    continue;
                }
                cand[n].after = pop->order;
                cand[n].el = el;
                cand[n].index = -1;
                cand[n].rc = clip;
                off[n++] = clip;
            }
            sub->lpVtbl->Release(sub);
        }

        picked = rescue_pick(raw, types, pop->order, end, &pop->rc, onscreen,
                             off, n, take);
        for (e = 0; e < n; ++e) {
            if (take[e] && found_count < RESCUE_MAX) {
                found[found_count++] = cand[e];
                if (cand[e].el)
                    ++inserted;
            } else if (cand[e].el) {
                cand[e].el->lpVtbl->Release(cand[e].el);
            }
        }
        if (picked > 0)
            taken_until = end;
    }

    if (detail) {
        for (k = 0; k < found_count; ++k) {
            int e = found[k].index;
            rescued[e] = found[k].rc;
            scan_reject(els[e], out, TRUE, &found[k].rc, types[e],
                        L"offscreen per Chromium, taken back into its "
                        L"dialog or menu");
        }
    } else if (inserted > 0) {
        int total = count + inserted;
        IUIAutomationElement **new_els =
            (IUIAutomationElement **)calloc((size_t)total, sizeof(*new_els));
        RECT *new_rescued =
            (RECT *)calloc((size_t)total, sizeof(*new_rescued));
        if (new_els && new_rescued) {
            int i, f = 0, j = 0;
            for (i = 0; i < count; ++i) {
                new_els[j] = els[i];
                new_rescued[j++] = rescued[i];
                while (f < found_count && found[f].after == i) {
                    new_els[j] = found[f].el;
                    new_rescued[j++] = found[f].rc;
                    ++f;
                }
            }
            free(els);
            free(rescued);
            *els_io = new_els;
            *rescued_io = new_rescued;
            *count_io = total;
        } else {
            free(new_els);
            free(new_rescued);
            for (k = 0; k < found_count; ++k)
                found[k].el->lpVtbl->Release(found[k].el);
            found_count = 0;
        }
    }
    free(cand);
    free(off);
    free(take);
    free(found);
    return found_count;
}

// The element types and patterns the scan keeps. One table serves both the
// UI Automation condition (uia_build_condition) and the cached check of a
// split container (cached_matches_condition), so the two cannot drift apart.
static const int g_scan_types[] = {
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
    UIA_DocumentControlTypeId,    // fetched only to clip page elements
    UIA_MenuControlTypeId,        // fetched only to find overlays
    UIA_WindowControlTypeId       // (a web dialog is a Window)
};
static const int g_scan_patterns[] = {
    UIA_IsInvokePatternAvailablePropertyId,
    UIA_IsTogglePatternAvailablePropertyId,
    UIA_IsSelectionItemPatternAvailablePropertyId,
    UIA_IsExpandCollapsePatternAvailablePropertyId
};

static BOOL scan_type_listed(int control_type)
{
    int i;

    for (i = 0; i < (int)(sizeof(g_scan_types) / sizeof(g_scan_types[0]));
         ++i)
        if (g_scan_types[i] == control_type)
            return TRUE;
    return FALSE;
}

// Any of the four patterns the condition asks for (cached).
static BOOL element_patterns(IUIAutomationElement *el)
{
    int i;

    for (i = 0;
         i < (int)(sizeof(g_scan_patterns) / sizeof(g_scan_patterns[0])); ++i)
        if (cached_bool_property(el, g_scan_patterns[i]))
            return TRUE;
    return FALSE;
}

// An element the web condition listed for its keyboard focus alone (no
// listed type, none of the patterns): Chromium exposes a div whose click
// handler sits on an ancestor (React, jsaction) and a range input that way,
// and nothing else about them says "clickable". Blink also calls focusable
// what tabindex=-1 marks for scripts (a main region, a heading after a
// route change), every scroll area since keyboard-focusable scrollers, and
// the toolbar's own focus stops. Returns why it is left out, or NULL to
// keep it: not text, not a scroll area, inside the page (page: the page
// Document, NULL when the window has none) and under a quarter of the
// page's visible area. Containers around real targets go the usual way
// (filter_containers).
static const WCHAR *focusable_left_out(IUIAutomationElement *el,
                                       int control_type, const RECT *rc,
                                       const RECT *page, const RECT *monitor)
{
    RECT inter, on;

    if (!cached_bool_property(el, UIA_IsKeyboardFocusablePropertyId))
        return L"no clickable type or pattern";
    switch (control_type) {
    case UIA_TextControlTypeId:
    case UIA_PaneControlTypeId:
    case UIA_ListControlTypeId:
    case UIA_TableControlTypeId:
    case UIA_TreeControlTypeId:
    case UIA_MenuBarControlTypeId:
    case UIA_TitleBarControlTypeId:
    case UIA_ToolBarControlTypeId:
    case UIA_HeaderControlTypeId:
    case UIA_StatusBarControlTypeId:
    case UIA_TabControlTypeId:
    case UIA_SeparatorControlTypeId:
    case UIA_ThumbControlTypeId:
        return L"focusable only: text or a container";
    default:
        break;
    }
    if (cached_bool_property(el, UIA_IsScrollPatternAvailablePropertyId))
        return L"focusable only: a scroll area";
    if (!page || !IntersectRect(&inter, rc, page))
        return L"focusable only: outside the page";
    if (IntersectRect(&on, page, monitor) &&
        rect_area(rc) * 4 > rect_area(&on))
        return L"focusable only: too large";
    return NULL;
}

// Copies a FindAll array into an owned reference list, with empty rescued
// rects (one per element). Returns the element count (0 when out of memory or
// the array cannot be read); *els and *rescued are then NULL.
static int element_array_take(IUIAutomationElementArray *array,
                              IUIAutomationElement ***els_out,
                              RECT **rescued_out)
{
    IUIAutomationElement **els = NULL;
    RECT *rescued = NULL;
    int length = 0;
    int i;

    *els_out = NULL;
    *rescued_out = NULL;
    if (FAILED(array->lpVtbl->get_Length(array, &length)) || length < 0)
        length = 0;
    if (length > 0) {
        els = (IUIAutomationElement **)calloc((size_t)length, sizeof(*els));
        rescued = (RECT *)calloc((size_t)length, sizeof(*rescued));
    }
    if (!els || !rescued) {
        free(els);
        free(rescued);
        return 0;
    }
    for (i = 0; i < length; ++i)
        if (FAILED(array->lpVtbl->GetElement(array, i, &els[i])))
            els[i] = NULL;
    *els_out = els;
    *rescued_out = rescued;
    return length;
}

// The same condition as uia_build_condition, tested on an element whose
// cached properties already hold the answers: its control type or one of the
// four patterns, and (unless log = 2 uses the types alone) enabled and on
// screen. A container split off the tree is added to the window's list only
// when this says the FindAll would have listed it.
static BOOL cached_matches_condition(IUIAutomationElement *el, BOOL detail)
{
    int control_type = 0;
    int i;
    BOOL type = FALSE;

    if (FAILED(el->lpVtbl->get_CachedControlType(el, &control_type)))
        return FALSE;
    for (i = 0; i < (int)(sizeof(g_scan_types) / sizeof(g_scan_types[0]));
         ++i) {
        if (g_scan_types[i] == control_type) {
            type = TRUE;
            break;
        }
    }
    if (!type) {
        for (i = 0;
             i < (int)(sizeof(g_scan_patterns) / sizeof(g_scan_patterns[0]));
             ++i) {
            if (cached_bool_property(el, g_scan_patterns[i])) {
                type = TRUE;
                break;
            }
        }
    }
    if (!type)
        return FALSE;
    if (detail)
        return TRUE;
    return cached_bool_property(el, UIA_IsEnabledPropertyId) &&
           !cached_bool_property(el, UIA_IsOffscreenPropertyId);
}

// Everything scan_window_uia does with the elements a FindAll returned:
// scroll bars, Documents, dialogs and menus, the candidate tests, container
// filtering and overlays. It takes over els and rescued (releasing every
// element) and fills out.
static void scan_filter(UiaEngine *engine, HWND hwnd, const RECT *zrects,
                        int z, const RECT *monitor,
                        IUIAutomationElement **els, RECT *rescued,
                        int length, ScanResult *out, BOOL detail, BOOL full)
{
    // Scroll bars and their arrow/page buttons are not worth a label.
    ScanLayout lay;
    const RECT *scrollbars = lay.scrollbars;
    const RECT *documents = lay.documents;
    Overlay ovs[32];
    int ov_count = 0;
    RECT *raw = NULL;           // every listed element's rect, in tree order
    int *types = NULL;
    int *fate = NULL;
    ScanTarget *cands = NULL;
    int cand_count = 0, cand_cap = 0;
    int kept = 0;
    BOOL *drop;
    POINT *new_click;
    int el_count = length;      // entries in els (each holds a reference)
    int page = -1;              // the page Document (focusable_left_out)
    LONG64 page_area = 0;
    BOOL web = scan_web_window(hwnd);
    IUIAutomationTreeWalker *walker = NULL;     // label_stand_in, on demand
    int stand_ins = 0;
    int i;

    if (length > 0) {
        raw = (RECT *)calloc((size_t)length, sizeof(*raw));
        types = (int *)calloc((size_t)length, sizeof(*types));
    }
    if (!raw || !types)
        length = 0;
    scan_layout(els, rescued, length, detail, &lay, raw, types);
    if (length > 0 &&
        rescue_offscreen(engine, hwnd, &els, &rescued, &el_count, &lay, raw,
                         types, monitor, detail, out) > 0) {
        RECT *raw2;
        int *types2;
        length = el_count;
        raw2 = (RECT *)realloc(raw, (size_t)length * sizeof(*raw));
        types2 = (int *)realloc(types, (size_t)length * sizeof(*types));
        if (raw2)
            raw = raw2;
        if (types2)
            types = types2;
        if (raw2 && types2)
            scan_layout(els, rescued, length, detail, &lay, raw, types);
        else
            length = 0;     // out of memory: nothing labelled this time
    }
    if (length > 0)
        page = page_document(&lay, monitor, &page_area);

    for (i = 0; i < length; ++i) {
        IUIAutomationElement *el = els[i];
        BOOL taken = rect_area(&rescued[i]) > 0;
        RECT rc;
        RECT clip;
        POINT click;
        int control_type = 0;
        ScanTarget target;
        LONG64 orig_area;
        LONG64 clip_area;
        LONG64 monitor_area;
        BOOL focusable_only = FALSE;

        if (!el)
            continue;           // els holds the reference, released below

        if (FAILED(el->lpVtbl->get_CachedBoundingRectangle(el, &rc)) ||
            FAILED(el->lpVtbl->get_CachedControlType(el, &control_type)))
            continue;
        if (taken)
            rc = rescued[i];

        if (detail && !taken) {
            const WCHAR *left_out = detail_left_out(el);
            if (left_out) {
                scan_reject(el, out, TRUE, &rc, control_type, left_out);
                continue;
            }
        }

        if (control_type == UIA_DocumentControlTypeId) {
            scan_reject(el, out, detail, &rc, control_type, L"document");
            continue;
        }
        // Fetched to find overlays. One that offers a pattern matched the
        // condition before they were added and stays a target.
        if ((control_type == UIA_WindowControlTypeId ||
             control_type == UIA_MenuControlTypeId) &&
            !cached_bool_property(el, UIA_IsInvokePatternAvailablePropertyId) &&
            !cached_bool_property(el, UIA_IsTogglePatternAvailablePropertyId) &&
            !cached_bool_property(
                el, UIA_IsSelectionItemPatternAvailablePropertyId) &&
            !cached_bool_property(
                el, UIA_IsExpandCollapsePatternAvailablePropertyId)) {
            scan_reject(el, out, detail, &rc, control_type,
                        L"dialog or menu itself");
            continue;
        }
        // In for its keyboard focus alone (the web condition): only what is
        // worth a click.
        if (!scan_type_listed(control_type) && !element_patterns(el)) {
            const WCHAR *why = focusable_left_out(
                el, control_type, &rc, page >= 0 ? &documents[page] : NULL,
                monitor);
            if (why) {
                scan_reject(el, out, detail, &rc, control_type, why);
                continue;
            }
            focusable_only = TRUE;
        }

        // A checkbox or radio button hidden behind its label: its label's
        // box stands in (label_stand_in), or the 1 px box is too small.
        // The walks need elements with full references (find_all_window).
        if (web && full && g_cfg.scan_hidden_checkbox && !taken &&
            stand_in_wanted(control_type, &rc) && stand_ins < STAND_IN_MAX) {
            RECT lab;

            ++stand_ins;
            if (!walker)
                engine->automation->lpVtbl->get_RawViewWalker(
                    engine->automation, &walker);
            if (walker && engine->kids_cache &&
                label_stand_in(walker, engine->kids_cache, el, &rc, monitor,
                               &lab)) {
                if (detail) {
                    WCHAR name[128];
                    cached_name(el, name, 128);
                    detail_element(&out->detail, name, &rc, control_type,
                                   L"hidden behind its label, which stands in",
                                   NULL);
                }
                rc = lab;
            }
        }

        orig_area = rect_area(&rc);
        if (orig_area <= 0) {
            scan_reject(el, out, detail, &rc, control_type, L"zero area");
            continue;
        }
        if (control_type == UIA_ScrollBarControlTypeId ||
            inside_any(scrollbars, lay.scrollbar_count, &rc)) {
            scan_reject(el, out, detail, &rc, control_type, L"scrollbar");
            continue;
        }
        if (!IntersectRect(&clip, &rc, monitor)) {
            scan_reject(el, out, detail, &rc, control_type, L"off monitor");
            continue;
        }

        clip_area = rect_area(&clip);
        monitor_area = rect_area(monitor);
        if (clip_area * 2 < orig_area) {
            scan_reject(el, out, detail, &clip, control_type,
                        L"mostly off monitor");
            continue;
        }

        // Chrome reports unclipped page rects: clip page elements to the
        // Document so a click point cannot land on the browser toolbar.
        {
            const RECT *best_doc = NULL;
            LONG64 best_area = 0;
            int d;
            for (d = 0; d < lay.document_count; ++d) {
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
            scan_reject(el, out, detail, &clip, control_type, L"too small");
            continue;
        }
        if (monitor_area > 0 && clip_area * 100 > monitor_area * 40) {
            scan_reject(el, out, detail, &clip, control_type, L"too large");
            continue;
        }

        click = choose_click_point(zrects, z, &clip);
        if (click.x < 0 && click.y < 0) {
            scan_reject(el, out, detail, &clip, control_type,
                        L"hidden by a window");
            continue;
        }

        // Nothing says a focusable-only element does anything itself: around
        // smaller targets it is a wrapper (a tabindex=-1 region holding
        // links), not an actionable card.
        target.actionable = !focusable_only &&
                            element_actionable(el, control_type);
        target.name[0] = 0;
        if (engine->with_names || detail)
            cached_name(el, target.name, 128);

        target.target.rc = clip;
        target.target.click = click;
        target.control_type = control_type;
        target.order = i;
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
    for (i = 0; i < el_count; ++i)
        if (els[i])
            els[i]->lpVtbl->Release(els[i]);
    free(els);
    free(rescued);
    free(types);

    // An element holding the click points of smaller targets is a container
    // (toolbar strip, row group), not something to click. A plain row or cell
    // (li/td) around its whole link is not worth a second label at the same
    // spot, but an actionable card can be: YouTube's video card exposes
    // Invoke and holds the title link and menu button, while the thumbnail is
    // only clickable through the card. So keep inner == 0; for an actionable
    // element keep inner == 1, or inner >= 2 when enough of it is free; for a
    // plain list or table item keep only inner == 1 with mostly free space.
    // Inner targets are the element's own content only (filter_containers),
    // never a panel drawn over it or the page under a panel.
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
        if (drop && drop[i]) {
            if (detail)
                detail_element(&out->detail, cands[i].name,
                               &cands[i].target.rc, cands[i].control_type,
                               L"container", NULL);
            continue;
        }
        cands[kept] = cands[i];
        if (drop && new_click)
            cands[kept].target.click = new_click[i];
        ++kept;
    }
    free(new_click);
    free(drop);

    // Overlays exist only in web content (a window with a Document): the
    // frames (every Document but the page's own, the largest) and the
    // dialogs and menus on the page that leave part of it free (a web app
    // that is one big dialog covers nothing). An overlay's content is the
    // run of elements after it in tree order that touch it.
    if (lay.document_count > 0 && raw) {
        int page, d, k;
        LONG64 page_area;
        page = page_document(&lay, monitor, &page_area);
        if (page < 0)
            page = 0;
        for (d = 0; d < lay.document_count && ov_count < 32; ++d) {
            if (d == page || rect_area(&documents[d]) <= 0)
                continue;
            ovs[ov_count].rc = documents[d];
            ovs[ov_count].order = lay.doc_order[d];
            ovs[ov_count].drops = FALSE;
            ++ov_count;
        }
        for (k = 0; k < lay.popup_count && ov_count < 32; ++k) {
            if (!popup_on_page(&lay.popups[k].rc, &documents[page], page_area))
                continue;
            ovs[ov_count++] = lay.popups[k];
        }
        for (k = 0; k < ov_count; ++k) {
            int e = ovs[k].order + 1;
            RECT inter;
            while (e < length && (rect_area(&raw[e]) <= 0 ||
                                  IntersectRect(&inter, &raw[e], &ovs[k].rc)))
                ++e;
            ovs[k].end = e;
        }
    }
    if (ov_count > 0 && kept > 0)
        fate = (int *)calloc((size_t)kept, sizeof(*fate));
    if (fate)
        cover_overlays(cands, kept, ovs, ov_count, zrects, z, fate);

    cand_count = kept;
    kept = 0;
    for (i = 0; i < cand_count; ++i) {
        if (fate && fate[i] == COVER_DROPPED) {
            if (g_scan_debug)
                g_scan_debug(&cands[i].target.rc, cands[i].control_type,
                             L"under a dialog or menu");
            if (detail)
                detail_element(&out->detail, cands[i].name,
                               &cands[i].target.rc, cands[i].control_type,
                               L"under a dialog or menu", NULL);
            continue;
        }
        if (detail)
            detail_element(&out->detail, cands[i].name, &cands[i].target.rc,
                           cands[i].control_type,
                           fate && fate[i] == COVER_MOVED
                               ? L"kept, moved off an overlay" : L"kept",
                           &cands[i].target.click);
        cands[kept++] = cands[i];
    }
    if (detail)
        for (i = 0; i < ov_count; ++i)
            detail_add(&out->detail, L"scan:   overlay %ls %ld,%ld %ldx%ld, "
                       L"tree %d-%d", ovs[i].drops ? L"dialog/menu" : L"frame",
                       ovs[i].rc.left, ovs[i].rc.top,
                       ovs[i].rc.right - ovs[i].rc.left,
                       ovs[i].rc.bottom - ovs[i].rc.top, ovs[i].order,
                       ovs[i].end);
    if (walker)
        walker->lpVtbl->Release(walker);
    free(fate);
    free(raw);
    out->items = cands;
    out->count = kept;
    out->cap = cand_cap;
}

// One FindAll under an element, as the scan cache asks for it: every element
// with the cached properties, in tree order; web adds the keyboard-focusable
// ones (scan_web_window). Returns FALSE when UI Automation could not answer
// (the caller then falls back or drops the piece).
static BOOL find_all_under(UiaEngine *engine, IUIAutomationElement *el,
                           int scope, BOOL detail, BOOL web, BOOL full,
                           IUIAutomationElement ***els_out,
                           RECT **rescued_out, int *count_out)
{
    IUIAutomationCondition *condition = engine->condition;
    IUIAutomationCacheRequest *cache = engine->cache;
    IUIAutomationElementArray *array = NULL;
    int count = 0;

    *els_out = NULL;
    *rescued_out = NULL;
    *count_out = 0;
    if (web && engine->web_condition)
        condition = engine->web_condition;
    if (full && engine->full_cache)
        cache = engine->full_cache;
    if (detail) {
        if (engine->detail_condition && engine->detail_cache) {
            condition = web && engine->web_detail_condition
                            ? engine->web_detail_condition
                            : engine->detail_condition;
            cache = engine->detail_cache;
        } else {
            detail = FALSE;
        }
    }
    if (FAILED(el->lpVtbl->FindAllBuildCache(el, scope, condition, cache,
                                             &array)) || !array)
        return FALSE;
    count = element_array_take(array, els_out, rescued_out);
    array->lpVtbl->Release(array);
    *count_out = count;
    return TRUE;
}

// Windows whose last scan listed a checkbox or radio button hidden behind
// its label (stand_in_wanted), so the next scan fetches full references
// (full_cache) for label_stand_in's walks at once. Per window, as the slow
// memory (HISTORY section 5); an entry goes when a scan finds no such box,
// or after FULL_KEEP_MS. op: -1 asks, 1 remembers, 0 forgets.
#define FULL_MAX     8
#define FULL_KEEP_MS (10 * 60 * 1000)

static struct { HWND hwnd; DWORD pid; DWORD since; } g_full[FULL_MAX];
static volatile LONG g_full_lock;

static BOOL full_memory(HWND hwnd, DWORD pid, int op)
{
    DWORD now = GetTickCount();
    BOOL hit = FALSE;
    int i, found = -1, free_slot = -1, oldest = 0;

    if (!hwnd || !pid)
        return FALSE;
    while (InterlockedExchange(&g_full_lock, 1))
        Sleep(0);
    for (i = 0; i < FULL_MAX; ++i) {
        if (g_full[i].hwnd && now - g_full[i].since > FULL_KEEP_MS)
            g_full[i].hwnd = NULL;
        if (g_full[i].hwnd == hwnd && g_full[i].pid == pid) {
            hit = TRUE;
            found = i;
        } else if (!g_full[i].hwnd && free_slot < 0) {
            free_slot = i;
        }
        if (now - g_full[i].since > now - g_full[oldest].since)
            oldest = i;
    }
    if (op == 1) {
        i = found >= 0 ? found : (free_slot >= 0 ? free_slot : oldest);
        g_full[i].hwnd = hwnd;
        g_full[i].pid = pid;
        g_full[i].since = now;
    } else if (op == 0 && found >= 0) {
        g_full[found].hwnd = NULL;
    }
    InterlockedExchange(&g_full_lock, 0);
    return hit;
}

// TRUE when the listed elements hold a checkbox or radio button hidden
// behind its label (cached properties only: no call into the app).
static BOOL tiny_boxes_listed(IUIAutomationElement **els, int count)
{
    int i;

    for (i = 0; i < count; ++i) {
        int control_type = 0;
        RECT rc;

        if (els[i] &&
            SUCCEEDED(els[i]->lpVtbl->get_CachedControlType(els[i],
                                                            &control_type)) &&
            (control_type == UIA_CheckBoxControlTypeId ||
             control_type == UIA_RadioButtonControlTypeId) &&
            SUCCEEDED(els[i]->lpVtbl->get_CachedBoundingRectangle(els[i],
                                                                  &rc)) &&
            stand_in_wanted(control_type, &rc))
            return TRUE;
    }
    return FALSE;
}

static void release_elements(IUIAutomationElement **els, RECT *rescued,
                             int count)
{
    int i;

    for (i = 0; i < count; ++i)
        if (els[i])
            els[i]->lpVtbl->Release(els[i]);
    free(els);
    free(rescued);
}

// The FindAll of a whole window from its root. An element the scan cache
// lists carries no reference to the app (AutomationElementMode_None), and
// the tree walks of label_stand_in find nothing from such an element
// (measured 2026-11-03); elements with full references cost about a fifth
// more per scan (Wikipedia, 116 targets: 141 to 172 ms). So a browser
// window gets them only when its last scan saw a hidden checkbox
// (full_memory), and the first scan that sees one fetches again with them,
// once. log = 2 (detail) always has them. *full says what els carry.
static BOOL find_all_window(UiaEngine *engine, IUIAutomationElement *root,
                            HWND hwnd, BOOL detail,
                            IUIAutomationElement ***els, RECT **rescued,
                            int *length, BOOL *full)
{
    BOOL web = scan_web_window(hwnd);
    BOOL want = web && g_cfg.scan_hidden_checkbox;
    DWORD pid = 0;
    BOOL tiny;

    GetWindowThreadProcessId(hwnd, &pid);
    *full = detail || (want && full_memory(hwnd, pid, -1));
    if (!find_all_under(engine, root, UIA_TREESCOPE_DESCENDANTS, detail, web,
                        *full, els, rescued, length))
        return FALSE;
    if (!want)
        return TRUE;
    tiny = tiny_boxes_listed(*els, *length);
    full_memory(hwnd, pid, tiny ? 1 : 0);
    if (tiny && !*full) {
        IUIAutomationElement **els2 = NULL;
        RECT *rescued2 = NULL;
        int length2 = 0;

        if (find_all_under(engine, root, UIA_TREESCOPE_DESCENDANTS, detail,
                           web, TRUE, &els2, &rescued2, &length2)) {
            release_elements(*els, *rescued, *length);
            *els = els2;
            *rescued = rescued2;
            *length = length2;
            *full = TRUE;
        }
    }
    return TRUE;
}

// The single FindAll of the whole window (and what the split falls back to).
static void scan_window_uia(UiaEngine *engine, HWND hwnd,
                            const RECT *zrects, int z, const RECT *monitor,
                            ScanResult *out, int *answered, BOOL detail)
{
    IUIAutomationElement *root = NULL;
    IUIAutomationElement **els = NULL;
    RECT *rescued = NULL;
    int length = 0;
    BOOL full = FALSE;

    out->items = NULL;
    out->count = 0;
    out->cap = 0;
    *answered = 0;
    if (detail && !(engine->detail_condition && engine->detail_cache))
        detail = FALSE;

    if (FAILED(engine->automation->lpVtbl->ElementFromHandle(
            engine->automation, (UIA_HWND)hwnd, &root)) || !root) {
        if (detail)
            detail_add(&out->detail, L"scan:   no UI Automation element");
        return;
    }
    if (!find_all_window(engine, root, hwnd, detail, &els, &rescued, &length,
                         &full)) {
        if (detail)
            detail_add(&out->detail, L"scan:   FindAll failed");
        root->lpVtbl->Release(root);
        return;
    }
    root->lpVtbl->Release(root);
    *answered = length;
    scan_filter(engine, hwnd, zrects, z, monitor, els, rescued, length, out,
                detail, full);
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

// What the window list records about one top-level window: its class, its
// bounds clipped to the monitor, or why it is out (base_skip).
static void window_info_fill(const EnumContext *ctx, HWND hwnd, WinInfo *win)
{
    DwmGetWindowAttributeFn dwm = dwm_get_window_attribute();
    RECT rect;
    DWORD cloaked = 0;
    DWORD pid = 0;
    LONG exstyle;
    BOOL have_rect = FALSE;

    memset(win, 0, sizeof(*win));
    win->hwnd = hwnd;
    win->z = -1;
    GetClassNameW(hwnd, win->cls, 64);

    if (!IsWindowVisible(hwnd)) {
        win->base_skip = L"invisible";
    } else if (IsIconic(hwnd)) {
        win->base_skip = L"minimized";
    } else if (dwm && SUCCEEDED(dwm(hwnd, DWMWA_CLOAKED, &cloaked,
                                   sizeof(cloaked))) && cloaked) {
        win->base_skip = L"cloaked";
    } else {
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) {
            win->base_skip = L"own process";
        } else {
            // Only layered + transparent windows are invisible to the mouse;
            // plenty of normal apps (Chrome, Electron) are just layered.
            exstyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
            if ((exstyle & (WS_EX_TRANSPARENT | WS_EX_LAYERED)) ==
                (WS_EX_TRANSPARENT | WS_EX_LAYERED)) {
                win->base_skip = L"click-through";
            }
        }
    }

    if (!win->base_skip) {
        if (dwm && SUCCEEDED(dwm(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect,
                                 sizeof(rect))) &&
            rect.right > rect.left && rect.bottom > rect.top)
            have_rect = TRUE;
        else if (GetWindowRect(hwnd, &rect) &&
                 rect.right > rect.left && rect.bottom > rect.top)
            have_rect = TRUE;
        if (!have_rect)
            win->base_skip = L"no bounds";
        else if (!IntersectRect(&win->clip, &rect, &ctx->monitor))
            win->base_skip = L"off monitor";
        else
            win->rect = rect;
    }
}

static BOOL CALLBACK enum_window_cb(HWND hwnd, LPARAM lp)
{
    EnumContext *ctx = (EnumContext *)lp;
    WinInfo win;

    window_info_fill(ctx, hwnd, &win);
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

// Windows above the desktop band. The Start menu, Search, the notification
// centre, Quick Settings and notification banners (Windows.UI.Core.CoreWindow
// windows of StartMenuExperienceHost, SearchHost and ShellExperienceHost) sit
// in z-order bands above every ordinary window, topmost ones included, and
// EnumWindows leaves them out for a process without UIAccess. Measured on
// Windows 11 26300: GetForegroundWindow, WindowFromPoint, GetWindowRect,
// IsWindowVisible and UI Automation's ElementFromHandle all answer for them,
// while EnumWindows and the UI Automation root's children do not list them.
// So the foreground window is taken even when the list has no such window,
// and the monitor is sampled with WindowFromPoint every BAND_STEP_PX for
// windows the list does not hold (a banner is about 400x200). They go in
// front of the listed windows, higher band first (GetWindowBand is
// undocumented but exported since Windows 8; without it the foreground
// window leads). Knowing them keeps labels off the elements they cover; the
// overlay itself is drawn above them only when the process has UIAccess (a
// window owned by one of them stays in the desktop band: the owner is
// refused). hint_check.c's synchronous scan does not go through here.
#define BAND_STEP_PX 64
#define BAND_MAX 8

typedef BOOL (WINAPI *GetWindowBandFn)(HWND, DWORD *);

static DWORD window_band(HWND hwnd)
{
    static GetWindowBandFn fn;
    static BOOL tried;
    DWORD band = 0;

    if (!tried) {
        HMODULE user = GetModuleHandleW(L"user32.dll");
        if (user)
            fn = (GetWindowBandFn)(void *)GetProcAddress(user,
                                                          "GetWindowBand");
        tried = TRUE;
    }
    if (fn && fn(hwnd, &band))
        return band;
    return 0;
}

// Returns how many windows were added (in front), with the time it took.
static int add_unlisted_windows(EnumContext *ctx, HWND fg_root,
                                DWORD *elapsed_ms)
{
    HWND found[BAND_MAX];
    DWORD bands[BAND_MAX];
    DWORD started = GetTickCount();
    WinInfo *items;
    int n = 0;
    int i, k;
    LONG x, y;

    if (fg_root && window_index(ctx->items, ctx->count, fg_root) < 0)
        found[n++] = fg_root;
    for (y = ctx->monitor.top + BAND_STEP_PX / 2;
         y < ctx->monitor.bottom && n < BAND_MAX; y += BAND_STEP_PX) {
        for (x = ctx->monitor.left + BAND_STEP_PX / 2;
             x < ctx->monitor.right && n < BAND_MAX; x += BAND_STEP_PX) {
            POINT pt;
            HWND hit, root;

            pt.x = x;
            pt.y = y;
            hit = WindowFromPoint(pt);
            root = hit ? GetAncestor(hit, GA_ROOT) : NULL;
            if (!root || window_index(ctx->items, ctx->count, root) >= 0)
                continue;
            for (i = 0; i < n; ++i) {
                if (found[i] == root)
                    break;
            }
            if (i < n)
                continue;
            found[n++] = root;
        }
    }
    *elapsed_ms = GetTickCount() - started;
    if (n == 0)
        return 0;

    // Higher band first; the insertion sort is stable, so the foreground
    // window stays ahead of the rest of its band.
    for (i = 0; i < n; ++i)
        bands[i] = window_band(found[i]);
    for (i = 1; i < n; ++i) {
        HWND h = found[i];
        DWORD b = bands[i];
        for (k = i; k > 0 && bands[k - 1] < b; --k) {
            found[k] = found[k - 1];
            bands[k] = bands[k - 1];
        }
        found[k] = h;
        bands[k] = b;
    }

    items = (WinInfo *)realloc(ctx->items,
                               (size_t)(ctx->count + n) * sizeof(*items));
    if (!items)
        return 0;
    ctx->items = items;
    ctx->cap = ctx->count + n;
    memmove(items + n, items, (size_t)ctx->count * sizeof(*items));
    for (i = 0; i < n; ++i) {
        window_info_fill(ctx, found[i], &items[i]);
        items[i].above = TRUE;
    }
    ctx->count += n;
    return n;
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

// One piece of a split window scan: either a part (one FindAll under an
// element) or a container whose children are split further. Nodes hang off
// their parent in tree order, so the window's last item can walk them into
// the tree-ordered element list the filters expect.
#define SN_PART 0
#define SN_CONTAINER 1

typedef struct ScanNode ScanNode;
struct ScanNode {
    int kind;                   // SN_PART: a FindAll result; SN_CONTAINER
    int depth;                  // levels below the window root (root = 0)
    ScanNode **kids;
    int kid_count;
    int kid_cap;
    IUIAutomationElement **els; // SN_PART: its FindAll result, tree order
    RECT *rescued;
    int count;
    IUIAutomationElement *self; // SN_CONTAINER: own element, when it matched
};

// What a work item does: start a window, split a container, split a title
// bar, or run one FindAll. Each is a separate item on the helper pool so the
// trees, lists and title bars of a window are asked at the same time.
#define WK_ROOT 0
#define WK_SPLIT 1
#define WK_TITLEBAR 2
#define WK_PART 3

typedef struct ScanTask ScanTask;
typedef struct ScanJob ScanJob;

typedef struct ScanItem {
    struct ScanItem *next;
    ScanTask *win;
    ScanNode *node;
    IUIAutomationElement *el;   // searched under; NULL once run
    int kind;                   // WK_*
    int depth;
    int scope;                  // UIA_TREESCOPE_* for WK_PART
} ScanItem;

struct ScanTask {
    HWND hwnd;
    DWORD pid;
    int z;
    int win_index;              // caller-only; index into the enumerated WinInfo
    BOOL detail;                // this is the active window's log = 2 scan
    BOOL full;                  // its elements carry full references (find_all_window)
    volatile LONG finished;     // set by the scanning thread before signaling
    LONG merged;                // caller-only
    DWORD elapsed_ms;
    DWORD started;              // the root item's start
    ScanResult found;
    LONG answered;              // elements across all parts
    volatile LONG pending;      // work items not finished yet
    ScanNode *tree;             // root node (SN_CONTAINER, depth 0)
    BOOL skipped;               // known slow: not scanned at all
    BOOL cancelled;             // the job was cancelled: result unused
    // A piece UI Automation could not answer (an element gone meanwhile):
    // the rest is still labelled, but it is no verdict on slowness.
    BOOL partial;
    BOOL split;                 // the parallel split produced the pieces
    BOOL skipped_menu;          // a title bar's system menu was dropped
    volatile LONG parts;        // work items this window was split into
};

struct ScanJob {
    UiaEngine *engine;
    RECT *zrects;
    int zcount;
    RECT monitor;
    UiaContinueCallback keep_going;
    void *keep_user;
    UINT keep_scan_id;          // keep_user's pointee; see uia_scan_monitor
    HWND detail_hwnd;           // log = 2: the active window, else NULL
    ScanTask *tasks;
    int task_count;
    ScanItem *queue;            // protected by g_pool_cs; LIFO
    volatile LONG outstanding;  // items queued or running
    volatile LONG refs;
    HANDLE done_event;          // auto-reset, signaled by every finished task
    // One count per queued item, so every item queued at once wakes its own
    // helper (an auto-reset event woke one; the rest sat out the 20 ms poll).
    // The last item done releases one per helper so the waiting ones leave.
    HANDLE work_sem;
};

static HANDLE g_helper_threads[UIA_HELPER_THREADS];
static int g_helper_count;
static HANDLE g_job_sem;
static HANDLE g_pool_quit;
static ScanJob *g_job;
static CRITICAL_SECTION g_pool_cs;
static BOOL g_pool_cs_ready;

// A node and (when parent is not NULL) the place it takes in the parent's
// tree-ordered child list. Only the item that owns a node appends children
// to it, so the list itself needs no lock.
static ScanNode *scan_node_new(ScanNode *parent, int kind, int depth)
{
    ScanNode *node = (ScanNode *)calloc(1, sizeof(*node));

    if (!node)
        return NULL;
    node->kind = kind;
    node->depth = depth;
    if (parent) {
        if (parent->kid_count == parent->kid_cap) {
            int cap = parent->kid_cap ? parent->kid_cap * 2 : 8;
            ScanNode **kids = (ScanNode **)realloc(
                parent->kids, (size_t)cap * sizeof(*kids));
            if (!kids) {
                free(node);
                return NULL;
            }
            parent->kids = kids;
            parent->kid_cap = cap;
        }
        parent->kids[parent->kid_count++] = node;
    }
    return node;
}

// Releases everything a node still owns. After scan_tree_copy the element
// references belong to the flat list, so they are NULL there and this only
// frees the nodes.
static void scan_node_free(ScanNode *node)
{
    int i;

    if (!node)
        return;
    for (i = 0; i < node->kid_count; ++i)
        scan_node_free(node->kids[i]);
    free(node->kids);
    for (i = 0; i < node->count; ++i)
        if (node->els && node->els[i])
            node->els[i]->lpVtbl->Release(node->els[i]);
    free(node->els);
    free(node->rescued);
    if (node->self)
        node->self->lpVtbl->Release(node->self);
    free(node);
}

static int scan_tree_count(const ScanNode *node)
{
    int total = node->kind == SN_PART ? node->count : (node->self ? 1 : 0);
    int i;

    for (i = 0; i < node->kid_count; ++i)
        total += scan_tree_count(node->kids[i]);
    return total;
}

// Moves the references of every node into one tree-ordered list. A part's
// elements come in tree order; a container's own element comes before its
// children, exactly as the single FindAll listed it.
static void scan_tree_copy(ScanNode *node, IUIAutomationElement **els,
                           RECT *rescued, int *at)
{
    int i;

    if (node->kind == SN_PART) {
        for (i = 0; i < node->count; ++i) {
            els[*at] = node->els[i];
            rescued[*at] = node->rescued[i];
            node->els[i] = NULL;
            ++*at;
        }
        node->count = 0;
    } else if (node->self) {
        els[*at] = node->self;
        SetRectEmpty(&rescued[*at]);
        node->self = NULL;
        ++*at;
    }
    for (i = 0; i < node->kid_count; ++i)
        scan_tree_copy(node->kids[i], els, rescued, at);
}

// The window's elements in tree order. Takes over the node tree.
static int scan_tree_flatten(ScanTask *win, IUIAutomationElement ***els_out,
                             RECT **rescued_out)
{
    IUIAutomationElement **els = NULL;
    RECT *rescued = NULL;
    int total;
    int at = 0;

    *els_out = NULL;
    *rescued_out = NULL;
    if (!win->tree)
        return 0;
    total = scan_tree_count(win->tree);
    if (total > 0) {
        els = (IUIAutomationElement **)calloc((size_t)total, sizeof(*els));
        rescued = (RECT *)calloc((size_t)total, sizeof(*rescued));
    }
    if (!els || !rescued) {
        free(els);
        free(rescued);
        scan_node_free(win->tree);
        win->tree = NULL;
        return 0;
    }
    scan_tree_copy(win->tree, els, rescued, &at);
    scan_node_free(win->tree);
    win->tree = NULL;
    *els_out = els;
    *rescued_out = rescued;
    return total;
}


// Whoever drops the last reference frees the whole job, including the result
// arrays, the done event and the zrects. A helper finishing after the caller
// has already returned therefore still has everything it touches.
static void job_release_ref(ScanJob *job)
{
    int i;

    if (InterlockedDecrement(&job->refs) != 0)
        return;
    if (job->tasks) {
        for (i = 0; i < job->task_count; ++i) {
            free(job->tasks[i].found.items);
            free(job->tasks[i].found.detail.text);
            scan_node_free(job->tasks[i].tree);
        }
        free(job->tasks);
    }
    while (job->queue) {
        ScanItem *item = job->queue;
        job->queue = item->next;
        if (item->el)
            item->el->lpVtbl->Release(item->el);
        free(item);
    }
    if (job->done_event)
        CloseHandle(job->done_event);
    if (job->work_sem)
        CloseHandle(job->work_sem);
    free(job->zrects);
    free(job);
}

static BOOL pool_is_stopping(void)
{
    return g_pool_quit &&
           WaitForSingleObject(g_pool_quit, 0) == WAIT_OBJECT_0;
}

static BOOL job_is_cancelled(ScanJob *job)
{
    return pool_is_stopping() ||
           (job->keep_going && !job->keep_going(job->keep_user));
}

// May this window's scan be split? Not where the parallel search would only
// queue behind itself: Chromium answers every accessibility call on its UI
// thread one at a time (Firefox the same), so splitting there was equal or
// slower. The single FindAll also runs without helper threads, when the
// setting is off, and when the engine lacks the children cache.
static BOOL scan_can_split(UiaEngine *engine, HWND hwnd)
{
    WCHAR cls[64];

    if (!g_cfg.scan_split || g_helper_count == 0 || !engine->kids_cache ||
        !engine->true_condition)
        return FALSE;
    if (uia_chromium_window(hwnd))
        return FALSE;
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"MozillaWindowClass") == 0)
        return FALSE;
    return TRUE;
}

// Queues a work item on the running job. The item owns el's reference until
// it runs. It counts against the job (helpers waiting on a momentarily empty
// queue may not leave) and against its window (its last item builds the
// window's element list and filters it).
static void scan_queue_push(ScanJob *job, ScanItem *item)
{
    EnterCriticalSection(&g_pool_cs);
    item->next = job->queue;
    job->queue = item;
    InterlockedIncrement(&job->outstanding);
    InterlockedIncrement(&item->win->pending);
    if (item->kind != WK_ROOT)
        InterlockedIncrement(&item->win->parts);
    LeaveCriticalSection(&g_pool_cs);
    ReleaseSemaphore(job->work_sem, 1, NULL);
}

// The finish runs on whichever helper (or the scan worker, without helpers)
// ran the window's last item. One window at a time, so its detail text and
// its element tree need no lock.
static void scan_window_finish(ScanJob *job, ScanTask *win, BOOL cancelled);

// An item is done: count it off its window and the job, and let the window's
// last one build and filter its element list. cancelled marks a dropped item,
// so the window is neither filtered nor judged slow.
static void scan_item_done(ScanJob *job, ScanItem *item, BOOL cancelled)
{
    ScanTask *win = item->win;

    if (cancelled)
        win->cancelled = TRUE;
    free(item);
    if (InterlockedDecrement(&win->pending) == 0)
        scan_window_finish(job, win, win->cancelled);
    if (InterlockedDecrement(&job->outstanding) == 0)
        ReleaseSemaphore(job->work_sem, UIA_HELPER_THREADS, NULL);
}

// An item that will not run: release its element, but still count it done, or
// the window and the job would wait for it forever.
static void scan_item_drop(ScanJob *job, ScanItem *item)
{
    if (item->el) {
        item->el->lpVtbl->Release(item->el);
        item->el = NULL;
    }
    scan_item_done(job, item, TRUE);
}

// The children cache matching the log level in use.
static IUIAutomationCacheRequest *kids_cache_for(UiaEngine *engine,
                                                 BOOL detail)
{
    if (detail && engine->detail_kids_cache)
        return engine->detail_kids_cache;
    return engine->kids_cache;
}

// The root's (or a container's) children, with the split cache: Full element
// mode so they can be searched under, and the local condition's properties.
static BOOL find_children(UiaEngine *engine, IUIAutomationElement *el,
                          BOOL detail, IUIAutomationElement ***els_out,
                          int *count_out)
{
    IUIAutomationElementArray *array = NULL;
    IUIAutomationElement **els = NULL;
    RECT *rescued = NULL;
    int count = 0;

    *els_out = NULL;
    *count_out = 0;
    if (FAILED(el->lpVtbl->FindAllBuildCache(
            el, UIA_TREESCOPE_CHILDREN, engine->true_condition,
            kids_cache_for(engine, detail), &array)) || !array)
        return FALSE;
    count = element_array_take(array, &els, &rescued);
    array->lpVtbl->Release(array);
    free(rescued);
    *els_out = els;
    *count_out = count;
    return TRUE;
}

static BOOL is_split_container(int control_type)
{
    return control_type == UIA_PaneControlTypeId ||
           control_type == UIA_GroupControlTypeId ||
           control_type == UIA_CustomControlTypeId;
}

static void scan_item_run(ScanJob *job, ScanItem *item);

// A child of the root or of a split container. A title bar is searched for
// its children and loses its system menu (the MenuBar on the window icon,
// one label that Alt+Space opens anyway and that UI Automation's non-client
// proxy answers in 60 ms or more); a container near the root splits again;
// everything else is one subtree FindAll. Takes el over.
static void scan_dispatch_child(ScanJob *job, ScanTask *win, ScanNode *parent,
                                int depth, IUIAutomationElement *el)
{
    ScanNode *node;
    ScanItem *item;
    int control_type = 0;

    if (!el)
        return;
    if (FAILED(el->lpVtbl->get_CachedControlType(el, &control_type))) {
        el->lpVtbl->Release(el);
        win->partial = TRUE;
        return;
    }

    node = scan_node_new(parent, SN_CONTAINER, depth);
    item = node ? (ScanItem *)calloc(1, sizeof(*item)) : NULL;
    if (!node || !item) {
        free(item);
        el->lpVtbl->Release(el);
        win->partial = TRUE;
        return;
    }
    item->win = win;
    item->node = node;
    item->el = el;
    item->depth = depth;
    if (control_type == UIA_TitleBarControlTypeId) {
        item->kind = WK_TITLEBAR;
    } else if (is_split_container(control_type) &&
               depth < SCAN_SPLIT_MAX_DEPTH) {
        item->kind = WK_SPLIT;
    } else {
        node->kind = SN_PART;
        item->kind = WK_PART;
        item->scope = UIA_TREESCOPE_SUBTREE;
    }
    scan_queue_push(job, item);
}

// The window root: one children call, then a work item per child. When the
// split is not allowed (or the children call fails) this is the single
// FindAll the scan has always made.
static void scan_root_item(ScanJob *job, ScanItem *item)
{
    ScanTask *win = item->win;
    UiaEngine *engine = job->engine;
    IUIAutomationElement *root = NULL;
    IUIAutomationElement **kids = NULL;
    IUIAutomationElement **els = NULL;
    RECT *rescued = NULL;
    int count = 0;
    int length = 0;
    int i;

    if (slow_check(win->hwnd, win->pid, FALSE)) {
        win->skipped = TRUE;
        if (win->detail)
            detail_add(&win->found.detail, L"scan: active window %p skipped: "
                       L"answered slowly before", (void *)win->hwnd);
        return;
    }
    win->started = GetTickCount();
    if (!win->tree)
        return;
    if (FAILED(engine->automation->lpVtbl->ElementFromHandle(
            engine->automation, (UIA_HWND)win->hwnd, &root)) || !root) {
        if (win->detail)
            detail_add(&win->found.detail, L"scan:   no UI Automation "
                       L"element");
        return;
    }

    if (!scan_can_split(engine, win->hwnd) ||
        !find_children(engine, root, win->detail, &kids, &count)) {
        free(kids);
        kids = NULL;
        count = 0;
        if (!find_all_window(engine, root, win->hwnd, win->detail, &els,
                             &rescued, &length, &win->full)) {
            if (win->detail)
                detail_add(&win->found.detail, L"scan:   FindAll failed");
        } else {
            win->tree->kind = SN_PART;
            win->tree->els = els;
            win->tree->rescued = rescued;
            win->tree->count = length;
            win->answered = length;
        }
        root->lpVtbl->Release(root);
        return;
    }

    win->split = TRUE;
    // In order, so the tree the filters see keeps FindAll's order.
    for (i = 0; i < count; ++i)
        scan_dispatch_child(job, win, win->tree, 1, kids[i]);
    free(kids);
    root->lpVtbl->Release(root);
}

// A container near the root: its children decide. More than a handful goes
// back to one FindAll (a file list with hundreds of rows is cheaper that
// way); otherwise the container itself is kept when it matches the condition
// and each child is handled by the same rules. Takes el over.
static void scan_split_item(ScanJob *job, ScanItem *item,
                            IUIAutomationElement *el)
{
    ScanTask *win = item->win;
    UiaEngine *engine = job->engine;
    ScanNode *node = item->node;
    IUIAutomationElement **kids = NULL;
    IUIAutomationElement **els = NULL;
    RECT *rescued = NULL;
    int count = 0;
    int length = 0;
    int i;

    if (!find_children(engine, el, win->detail, &kids, &count)) {
        free(kids);
        if (find_all_under(engine, el, UIA_TREESCOPE_SUBTREE, win->detail,
                           FALSE, FALSE, &els, &rescued, &length)) {
            node->kind = SN_PART;
            node->els = els;
            node->rescued = rescued;
            node->count = length;
            InterlockedExchangeAdd(&win->answered, length);
        } else {
            win->partial = TRUE;
        }
        el->lpVtbl->Release(el);
        return;
    }

    if (count > SCAN_SPLIT_MAX_CHILDREN) {
        if (find_all_under(engine, el, UIA_TREESCOPE_SUBTREE, win->detail,
                           FALSE, FALSE, &els, &rescued, &length)) {
            node->kind = SN_PART;
            node->els = els;
            node->rescued = rescued;
            node->count = length;
            InterlockedExchangeAdd(&win->answered, length);
        } else {
            win->partial = TRUE;
        }
        for (i = 0; i < count; ++i)
            if (kids[i])
                kids[i]->lpVtbl->Release(kids[i]);
        free(kids);
        el->lpVtbl->Release(el);
        return;
    }

    // The container itself was listed only when the condition matches; tree
    // order puts it before its children, as the single FindAll did.
    if (cached_matches_condition(el, win->detail)) {
        node->self = el;
        InterlockedIncrement(&win->answered);
    } else {
        el->lpVtbl->Release(el);
    }
    for (i = 0; i < count; ++i)
        scan_dispatch_child(job, win, node, item->depth + 1, kids[i]);
    free(kids);
}

// A title bar: children only. Its own element is never a target, and its
// system menu (a MenuBar) is dropped; Minimize, Maximize, Close and the rest
// still get labels. Takes el over.
static void scan_titlebar_item(ScanJob *job, ScanItem *item,
                               IUIAutomationElement *el)
{
    ScanTask *win = item->win;
    UiaEngine *engine = job->engine;
    ScanNode *node = item->node;
    IUIAutomationElement **kids = NULL;
    IUIAutomationElement **els = NULL;
    RECT *rescued = NULL;
    int count = 0;
    int length = 0;
    int i;

    if (!find_children(engine, el, win->detail, &kids, &count)) {
        free(kids);
        if (find_all_under(engine, el, UIA_TREESCOPE_SUBTREE, win->detail,
                           FALSE, FALSE, &els, &rescued, &length)) {
            node->kind = SN_PART;
            node->els = els;
            node->rescued = rescued;
            node->count = length;
            InterlockedExchangeAdd(&win->answered, length);
        } else {
            win->partial = TRUE;
        }
        el->lpVtbl->Release(el);
        return;
    }
    for (i = 0; i < count; ++i) {
        ScanNode *kid_node;
        ScanItem *kid_item;
        int control_type = 0;

        if (!kids[i])
            continue;
        if (FAILED(kids[i]->lpVtbl->get_CachedControlType(
                kids[i], &control_type))) {
            kids[i]->lpVtbl->Release(kids[i]);
            continue;
        }
        if (control_type == UIA_MenuBarControlTypeId) {
            kids[i]->lpVtbl->Release(kids[i]);
            win->skipped_menu = TRUE;
            continue;
        }
        // Every other title bar child is one subtree FindAll (Minimize,
        // Maximize, Close, the application icon's button).
        kid_node = scan_node_new(node, SN_PART, item->depth + 1);
        kid_item = kid_node ? (ScanItem *)calloc(1, sizeof(*kid_item))
                            : NULL;
        if (!kid_item) {
            kids[i]->lpVtbl->Release(kids[i]);
            win->partial = TRUE;
            continue;
        }
        kid_item->win = win;
        kid_item->node = kid_node;
        kid_item->el = kids[i];
        kid_item->kind = WK_PART;
        kid_item->scope = UIA_TREESCOPE_SUBTREE;
        scan_queue_push(job, kid_item);
    }
    free(kids);
    el->lpVtbl->Release(el);
}

static void scan_item_run(ScanJob *job, ScanItem *item)
{
    UiaEngine *engine = job->engine;
    ScanNode *node = item->node;
    IUIAutomationElement *el = item->el;

    item->el = NULL;
    switch (item->kind) {
    case WK_ROOT:
        scan_root_item(job, item);
        break;
    case WK_SPLIT:
        scan_split_item(job, item, el);
        el = NULL;
        break;
    case WK_TITLEBAR:
        scan_titlebar_item(job, item, el);
        el = NULL;
        break;
    default: {
        IUIAutomationElement **els = NULL;
        RECT *rescued = NULL;
        int count = 0;
        // Parts are split off classic windows only (scan_can_split).
        if (find_all_under(engine, el, item->scope, item->win->detail, FALSE,
                           FALSE, &els, &rescued, &count)) {
            node->els = els;
            node->rescued = rescued;
            node->count = count;
            InterlockedExchangeAdd(&item->win->answered, count);
        } else {
            item->win->partial = TRUE;
        }
        el->lpVtbl->Release(el);
        el = NULL;
        break;
    }
    }
    scan_item_done(job, item, FALSE);
}

// The window's elements are in; filter them in tree order and mark the
// window finished. A cancelled window never filters, and neither it nor a
// partial one judges the window slow (HISTORY section 5).
static void scan_window_finish(ScanJob *job, ScanTask *win, BOOL cancelled)
{
    UiaEngine *engine = job->engine;
    IUIAutomationElement **els = NULL;
    RECT *rescued = NULL;
    int length;

    length = scan_tree_flatten(win, &els, &rescued);
    if (cancelled)
        win->cancelled = TRUE;
    if (!win->cancelled && !win->skipped) {
        scan_filter(engine, win->hwnd, job->zrects, win->z, &job->monitor,
                    els, rescued, length, &win->found, win->detail,
                    win->full);
    } else {
        int i;
        for (i = 0; i < length; ++i)
            if (els[i])
                els[i]->lpVtbl->Release(els[i]);
        free(els);
        free(rescued);
    }

    if (!win->skipped) {
        win->elapsed_ms = win->started ? GetTickCount() - win->started : 0;
        if (win->detail) {
            detail_add(&win->found.detail, L"scan: active window %p: UI "
                       L"Automation listed %ld in %ld part%ls, %d kept, %lu "
                       L"ms%ls", (void *)win->hwnd, win->answered,
                       win->split && win->parts > 0 ? win->parts : 1,
                       win->split && win->parts > 1 ? L"s" : L"",
                       win->found.count,
                       (unsigned long)win->elapsed_ms,
                       win->skipped_menu
                           ? L"; title bar system menu skipped" : L"");
        }
        // A cancelled scan (Alt went on to scroll, a new scan started) must
        // not judge the window slow: only a scan that ran to its end may.
        if (!win->cancelled && !win->partial &&
            win->elapsed_ms > SLOW_SCAN_MS &&
            win->answered <= SLOW_SCAN_TARGETS &&
            (!job->keep_going || job->keep_going(job->keep_user)))
            slow_check(win->hwnd, win->pid, TRUE);
    }
    InterlockedExchange(&win->finished, 1);
    SetEvent(job->done_event);
}

static DWORD WINAPI uia_helper(void *arg)
{
    HRESULT hr;
    BOOL com_ready;
    (void)arg;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    com_ready = (hr == S_OK || hr == S_FALSE);

    for (;;) {
        ScanJob *job;
        HANDLE waits[2];
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

        // Take items until the job has none queued or running. The queue can
        // be empty for a moment while an item still adds work, so wait and
        // look again instead of leaving.
        for (;;) {
            ScanItem *item;

            if (pool_is_stopping())
                break;
            EnterCriticalSection(&g_pool_cs);
            item = job->queue;
            if (item)
                job->queue = item->next;
            LeaveCriticalSection(&g_pool_cs);
            if (!item) {
                if (InterlockedCompareExchange(&job->outstanding, 0, 0) == 0)
                    break;
                WaitForSingleObject(job->work_sem, 20);
                continue;
            }
            if (job_is_cancelled(job)) {
                scan_item_drop(job, item);
                continue;
            }
            scan_item_run(job, item);
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

// detail (log = 2): result->detail gets the windows on the monitor and what
// became of every element of the active window.
static void uia_scan_monitor(UiaEngine *engine, HMONITOR mon,
                             UiaContinueCallback keep_going, void *keep_user,
                             UiaWindowCallback cb, void *cb_user,
                             ScanResult *result, BOOL detail)
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
    int above_count = 0;
    DWORD above_ms = 0;
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
    above_count = add_unlisted_windows(&ctx, fg_root, &above_ms);
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
    // Asked before the foreground window's search starts, so the flag is
    // never newer than its elements: a page whose content came in during the
    // search counts as busy and is labelled afresh after (hint.c), while the
    // other way round its toolbar-only labels would stay.
    if (fg_task >= 0)
        result->page_busy = page_busy(ctx.items[fg_index].hwnd);
    result->pending = task_count;

    // Every visible window on the monitor, top first. An owned popup (a
    // menu, Chrome's panels) shows up here as its own window.
    if (detail) {
        detail_add(&result->detail, L"scan: detail (log = 2), monitor "
                   L"%ld,%ld %ldx%ld", mi.rcMonitor.left, mi.rcMonitor.top,
                   mi.rcMonitor.right - mi.rcMonitor.left,
                   mi.rcMonitor.bottom - mi.rcMonitor.top);
        detail_add(&result->detail, L"scan: %d window%ls above the desktop "
                   L"band (EnumWindows leaves them out), sampled in %lu ms",
                   above_count, above_count == 1 ? L"" : L"s",
                   (unsigned long)above_ms);
        for (i = 0; i < ctx.count; ++i) {
            WinInfo *win = &ctx.items[i];
            if (win->base_skip && wcscmp(win->base_skip, L"click-through") &&
                !win->above)
                continue;
            detail_add(&result->detail, L"scan: window %p %ls ex %08lx "
                       L"owner %p %ld,%ld %ldx%ld%ls%ls: %ls",
                       (void *)win->hwnd, win->cls,
                       (unsigned long)GetWindowLongW(win->hwnd, GWL_EXSTYLE),
                       (void *)GetWindow(win->hwnd, GW_OWNER),
                       win->rect.left, win->rect.top,
                       win->rect.right - win->rect.left,
                       win->rect.bottom - win->rect.top,
                       win->is_fg ? L" active" : L"",
                       win->above ? L" above the desktop band" : L"",
                       win->do_scan ? L"scanned" : window_skip(win));
        }
        if (fg_task < 0)
            detail_add(&result->detail, L"scan: the active window %p is not "
                       L"scanned", (void *)fg_root);
    }

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
    job->detail_hwnd = (detail && fg_task >= 0) ? fg_root : NULL;
    job->done_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    job->work_sem = CreateSemaphoreW(NULL, 0, 0x7fffffff, NULL);
    if (!job->done_event || !job->work_sem)
        goto done;
    if (task_count > 0) {
        job->tasks = (ScanTask *)calloc((size_t)task_count,
                                        sizeof(*job->tasks));
        if (!job->tasks)
            goto done;
        for (k = 0; k < order_count; ++k) {
            int ti = task_of_win[order[k]];
            if (ti >= 0) {
                ScanTask *task = &job->tasks[ti];
                task->hwnd = ctx.items[order[k]].hwnd;
                task->z = ctx.items[order[k]].z;
                task->win_index = order[k];
                task->detail = (task->hwnd == job->detail_hwnd);
                GetWindowThreadProcessId(task->hwnd, &task->pid);
                task->tree = scan_node_new(NULL, SN_CONTAINER, 0);
            }
        }
    }

    // Queue one root item per window, last first so the foreground window's
    // (task 0) is taken first, as before. All of them before the helpers see
    // the job: a helper that found it with nothing outstanding would leave it
    // for good, and with every helper gone the scan would never end.
    for (k = task_count - 1; k >= 0; --k) {
        ScanTask *task = &job->tasks[k];
        ScanItem *item = (ScanItem *)calloc(1, sizeof(*item));

        if (!item) {
            // No item to run: the window still has to finish and report.
            task->skipped = TRUE;
            task->finished = 1;
            continue;
        }
        item->win = task;
        item->node = task->tree;
        item->kind = WK_ROOT;
        scan_queue_push(job, item);
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
    // task order, so the foreground window (task 0) still goes first. Without
    // helper threads this runs the work items itself, one at a time.
    for (;;) {
        int ti;

        if (keep_going && !keep_going(job->keep_user))
            break;
        if (pool_is_stopping())
            break;
        if (merged_count >= job->task_count)
            break;

        if (g_helper_count == 0) {
            ScanItem *item;

            EnterCriticalSection(&g_pool_cs);
            item = job->queue;
            if (item)
                job->queue = item->next;
            LeaveCriticalSection(&g_pool_cs);
            if (!item) {
                if (InterlockedCompareExchange(&job->outstanding, 0, 0) == 0)
                    break;
                continue;
            }
            if (job_is_cancelled(job))
                scan_item_drop(job, item);
            else
                scan_item_run(job, item);
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
            result->pending = job->task_count - merged_count - 1;

            win = &ctx.items[task->win_index];
            before = result->count;
            for (j = 0; j < task->found.count; ++j) {
                if (target_add(result, &task->found.items[j]))
                    ++changes;
                else if (task->hwnd == job->detail_hwnd)
                    detail_element(&task->found.detail,
                                   task->found.items[j].name,
                                   &task->found.items[j].target.rc,
                                   task->found.items[j].control_type,
                                   L"dropped: same spot as another target",
                                   NULL);
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
    if (detail && fg_task >= 0) {
        ScanTask *task = &job->tasks[fg_task];
        if (InterlockedCompareExchange(&task->finished, 0, 0))
            detail_take(&result->detail, &task->found.detail);
        else
            detail_add(&result->detail, L"scan: stopped before the active "
                       L"window answered");
    }
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

    for (i = 0; i < (int)(sizeof(g_scan_types) / sizeof(g_scan_types[0]));
         ++i) {
        IUIAutomationCondition *c = property_condition(
            engine->automation, UIA_ControlTypePropertyId, VT_I4,
            g_scan_types[i]);
        if (!c) {
            ok = FALSE;
            break;
        }
        items[count++] = c;
    }
    if (ok) {
        for (i = 0;
             i < (int)(sizeof(g_scan_patterns) / sizeof(g_scan_patterns[0]));
             ++i) {
            IUIAutomationCondition *c = property_condition(
                engine->automation, g_scan_patterns[i], VT_BOOL, TRUE);
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

    // Browser windows: the same, or keyboard-focusable. Optional: without
    // it those windows get the plain condition.
    if (ok) {
        IUIAutomationCondition *focusable = property_condition(
            engine->automation, UIA_IsKeyboardFocusablePropertyId, VT_BOOL,
            TRUE);
        IUIAutomationCondition *web_or = NULL;
        IUIAutomationCondition *web_items[2];

        if (focusable) {
            web_items[0] = or_cond;
            web_items[1] = focusable;
            if (FAILED(engine->automation->lpVtbl->
                       CreateOrConditionFromNativeArray(
                           engine->automation, web_items, 2, &web_or)))
                web_or = NULL;
            focusable->lpVtbl->Release(focusable);
        }
        if (web_or) {
            and_items[0] = web_or;
            and_items[1] = enabled;
            and_items[2] = off;
            if (FAILED(engine->automation->lpVtbl->
                       CreateAndConditionFromNativeArray(
                           engine->automation, and_items, 3,
                           &engine->web_condition)))
                engine->web_condition = NULL;
            if (engine->web_condition)
                engine->web_detail_condition = web_or;
            else
                web_or->lpVtbl->Release(web_or);
        }
    }

    off->lpVtbl->Release(off);
    enabled->lpVtbl->Release(enabled);
    // The types alone serve log = 2 (engine->detail_condition).
    if (ok)
        engine->detail_condition = or_cond;
    else if (or_cond)
        or_cond->lpVtbl->Release(or_cond);
    return ok;
}

// detail adds what log = 2 reports: the name, and the offscreen and enabled
// states the normal condition tests. kids makes the children call of a split
// scan: the same properties, always with IsEnabled and IsOffscreen (the local
// condition test reads them), and Full element mode so the elements found can
// be searched under.
static IUIAutomationCacheRequest *uia_new_cache(IUIAutomation *automation,
                                                BOOL with_names, BOOL detail,
                                                BOOL kids, BOOL full)
{
    static const int props[] = {
        UIA_BoundingRectanglePropertyId,
        UIA_ControlTypePropertyId,
        UIA_IsInvokePatternAvailablePropertyId,
        UIA_IsTogglePatternAvailablePropertyId,
        UIA_IsSelectionItemPatternAvailablePropertyId,
        UIA_IsExpandCollapsePatternAvailablePropertyId,
        // For an element in through the web condition alone
        // (focusable_left_out).
        UIA_IsKeyboardFocusablePropertyId,
        UIA_IsScrollPatternAvailablePropertyId
    };
    IUIAutomationCacheRequest *cache = NULL;
    BOOL ok;
    int i;

    if (FAILED(automation->lpVtbl->CreateCacheRequest(automation, &cache)) ||
        !cache)
        return NULL;
    ok = TRUE;
    for (i = 0; ok && i < (int)(sizeof(props) / sizeof(props[0])); ++i)
        ok = SUCCEEDED(cache->lpVtbl->AddProperty(cache, props[i]));
    if (ok && (with_names || detail))
        ok = SUCCEEDED(cache->lpVtbl->AddProperty(cache, UIA_NamePropertyId));
    if (ok && (detail || kids))
        ok = SUCCEEDED(cache->lpVtbl->AddProperty(
                 cache, UIA_IsOffscreenPropertyId)) &&
             SUCCEEDED(cache->lpVtbl->AddProperty(
                 cache, UIA_IsEnabledPropertyId));
    // Full references: the children call searches under its elements, the
    // log = 2 scan and full_cache walk from theirs (label_stand_in).
    if (ok)
        ok = SUCCEEDED(cache->lpVtbl->put_AutomationElementMode(
                 cache, kids || detail || full ? UIA_MODE_FULL
                                               : UIA_MODE_NONE));
    if (!ok) {
        cache->lpVtbl->Release(cache);
        return NULL;
    }
    return cache;
}

static BOOL uia_build_cache(UiaEngine *engine, BOOL with_names)
{
    engine->cache = uia_new_cache(engine->automation, with_names, FALSE,
                                  FALSE, FALSE);
    if (!engine->cache)
        return FALSE;
    // Without it a hidden checkbox stays unlabelled (find_all_window).
    engine->full_cache = uia_new_cache(engine->automation, with_names, FALSE,
                                       FALSE, TRUE);
    // Without it log = 2 only says that it could not add the details.
    engine->detail_cache = uia_new_cache(engine->automation, with_names,
                                         TRUE, FALSE, FALSE);
    engine->kids_cache = uia_new_cache(engine->automation, with_names, FALSE,
                                       TRUE, FALSE);
    engine->detail_kids_cache = uia_new_cache(engine->automation, with_names,
                                              TRUE, TRUE, FALSE);
    // A split without the children cache falls back to the single FindAll.
    return TRUE;
}

// The condition for "everything" (the root's children call). Kept per engine
// so a scan does not create one per window.
static BOOL uia_build_true_condition(UiaEngine *engine)
{
    return !FAILED(engine->automation->lpVtbl->CreateTrueCondition(
               engine->automation, &engine->true_condition)) &&
           engine->true_condition != NULL;
}

static void uia_engine_cleanup(UiaEngine *engine)
{
    if (engine->cache)
        engine->cache->lpVtbl->Release(engine->cache);
    if (engine->full_cache)
        engine->full_cache->lpVtbl->Release(engine->full_cache);
    if (engine->kids_cache)
        engine->kids_cache->lpVtbl->Release(engine->kids_cache);
    if (engine->detail_kids_cache)
        engine->detail_kids_cache->lpVtbl->Release(engine->detail_kids_cache);
    if (engine->true_condition)
        engine->true_condition->lpVtbl->Release(engine->true_condition);
    if (engine->condition)
        engine->condition->lpVtbl->Release(engine->condition);
    if (engine->web_condition)
        engine->web_condition->lpVtbl->Release(engine->web_condition);
    if (engine->detail_cache)
        engine->detail_cache->lpVtbl->Release(engine->detail_cache);
    if (engine->detail_condition)
        engine->detail_condition->lpVtbl->Release(engine->detail_condition);
    if (engine->web_detail_condition)
        engine->web_detail_condition->lpVtbl->Release(
            engine->web_detail_condition);
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

    if (!uia_build_condition(engine) || !uia_build_cache(engine, with_names) ||
        !uia_build_true_condition(engine)) {
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
    BOOL detail;
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
    list->page_busy = result->page_busy;
    list->pending = done ? 0 : result->pending;
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

// A UI Automation hit test on a Chromium page (uia_flush). Its fragment root
// answers ElementFromPoint through BrowserAccessibility::HitTestSync, which
// starts an asynchronous hit test in the page's renderer
// (CachingAsyncHitTest). The renderer fires a hover event for it with
// immediate serialization (RenderAccessibilityImpl::HitTest), and that
// serialization takes every pending position and scroll change along
// (AXObjectCacheImpl::SerializeUpdatesAndEvents), past both the 500 ms
// location throttle and the 150 ms deferral of new elements. Measured on
// Windows (2026-11-03): the scroll report for what the hit test sent out
// arrives within a millisecond of the call returning. Returns whether the
// hit test reached the page: the element found belongs to the process of
// the window at the point (our own overlay, or another window over it,
// answers for another process and proves nothing about the page).
static BOOL flush_at(UiaEngine *engine, POINT pt)
{
    IUIAutomationElement *el = NULL;
    HWND hwnd = WindowFromPoint(pt);
    DWORD pid = 0;
    int el_pid = 0;
    BOOL reached = FALSE;

    if (!hwnd)
        return FALSE;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId())
        return FALSE;
    if (SUCCEEDED(engine->automation->lpVtbl->ElementFromPoint(
            engine->automation, pt, &el)) && el) {
        reached = SUCCEEDED(el->lpVtbl->get_CurrentProcessId(el, &el_pid)) &&
                  (DWORD)el_pid == pid;
        el->lpVtbl->Release(el);
    }
    return reached;
}

// The flush runs on a thread of its own, with an engine of its own, so it is
// never queued behind a scan on the worker (the wait reads its timing, and
// the scan it starts early must not delay the flush that confirms it). It
// posts WM_APP_FLUSH_DONE (wParam = reached, lParam = the request's seq).
static HANDLE g_fl_thread;
static HANDLE g_fl_wake;
static CRITICAL_SECTION g_fl_cs;
static BOOL  g_fl_cs_ready;
static volatile LONG g_fl_quit;
static BOOL  g_fl_pending;
static POINT g_fl_pt;
static UINT  g_fl_seq;

static DWORD WINAPI flush_worker(void *arg)
{
    UiaEngine engine;
    HRESULT hr;
    (void)arg;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr != S_OK && hr != S_FALSE)
        return 0;
    if (uia_engine_init(&engine, FALSE)) {
        for (;;) {
            BOOL pending;
            POINT pt;
            UINT seq;
            BOOL reached;

            if (WaitForSingleObject(g_fl_wake, INFINITE) != WAIT_OBJECT_0)
                break;
            if (InterlockedCompareExchange(&g_fl_quit, 0, 0))
                break;
            EnterCriticalSection(&g_fl_cs);
            pending = g_fl_pending;
            pt = g_fl_pt;
            seq = g_fl_seq;
            g_fl_pending = FALSE;
            LeaveCriticalSection(&g_fl_cs);
            if (!pending)
                continue;
            reached = flush_at(&engine, pt);
            if (g_main_wnd)
                PostMessageW(g_main_wnd, WM_APP_FLUSH_DONE, (WPARAM)reached,
                             (LPARAM)seq);
        }
        uia_engine_cleanup(&engine);
    } else {
        app_log(L"uia: flush engine init failed");
    }
    CoUninitialize();
    return 0;
}

static void flush_worker_start(void)
{
    if (g_fl_thread)
        return;
    InitializeCriticalSection(&g_fl_cs);
    g_fl_cs_ready = TRUE;
    g_fl_quit = 0;
    g_fl_pending = FALSE;
    g_fl_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (g_fl_wake)
        g_fl_thread = CreateThread(NULL, 0, flush_worker, NULL, 0, NULL);
    if (!g_fl_thread) {
        if (g_fl_wake)
            CloseHandle(g_fl_wake);
        g_fl_wake = NULL;
        DeleteCriticalSection(&g_fl_cs);
        g_fl_cs_ready = FALSE;
    }
}

static void flush_worker_stop(void)
{
    if (!g_fl_thread)
        return;
    InterlockedExchange(&g_fl_quit, 1);
    SetEvent(g_fl_wake);
    if (WaitForSingleObject(g_fl_thread, 3000) != WAIT_OBJECT_0)
        return;
    CloseHandle(g_fl_thread);
    g_fl_thread = NULL;
    CloseHandle(g_fl_wake);
    g_fl_wake = NULL;
    if (g_fl_cs_ready) {
        DeleteCriticalSection(&g_fl_cs);
        g_fl_cs_ready = FALSE;
    }
    InterlockedExchange(&g_fl_quit, 0);
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
            // A wake without a new request must not run the last scan again.
            UINT last_scan = 0;

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
                if (!request.scan_id || !request.mon ||
                    request.scan_id == last_scan)
                    continue;
                last_scan = request.scan_id;

                scan.scan_id = request.scan_id;
                memset(&result, 0, sizeof(result));
                scan.result = &result;
                uia_scan_monitor(&engine, request.mon, worker_keep_going,
                                 &scan.scan_id, worker_on_window, &scan,
                                 &result, request.detail);
                if (uia_still_current(request.scan_id))
                    post_list(request.scan_id, TRUE, &result);
                detail_flush(&result.detail);
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
    if (g_uia_available)
        flush_worker_start();
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
    flush_worker_stop();
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

void uia_scan(HMONITOR mon, UINT scan_id, BOOL detail)
{
    if (!g_uia_thread || !g_uia_available || !g_uia_cs_ready)
        return;
    EnterCriticalSection(&g_uia_cs);
    g_uia_request.scan_id = scan_id;
    g_uia_request.mon = mon;
    g_uia_request.detail = detail;
    LeaveCriticalSection(&g_uia_cs);
    SetEvent(g_uia_wake);
}

void uia_flush(POINT pt, UINT seq)
{
    // No flush thread (or it died at its start): answer "not reached", so
    // the wait falls back to its time limit.
    if (!g_fl_thread || !g_fl_cs_ready ||
        WaitForSingleObject(g_fl_thread, 0) == WAIT_OBJECT_0) {
        if (g_main_wnd)
            PostMessageW(g_main_wnd, WM_APP_FLUSH_DONE, 0, (LPARAM)seq);
        return;
    }
    EnterCriticalSection(&g_fl_cs);
    g_fl_pt = pt;
    g_fl_seq = seq;
    g_fl_pending = TRUE;
    LeaveCriticalSection(&g_fl_cs);
    SetEvent(g_fl_wake);
}

void uia_cancel(void)
{
    if (!g_uia_cs_ready)
        return;
    EnterCriticalSection(&g_uia_cs);
    g_uia_request.scan_id = 0;
    g_uia_request.mon = NULL;
    g_uia_request.detail = FALSE;
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
            if (c->refreshing)
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

// Starts the worker on first use; never spins. Returns FALSE while another
// thread is still starting it (g_sc_state 1) and when that failed (3).
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

// The movement thread used to wait here for the answer (up to 500 ms from a
// cold cache), which held back the scroll and the pointer with it: a press
// felt late, and h/j/k/l held at the same time stopped. Now it never waits.
// The stream scrolls under the pointer meanwhile and asks again every tick;
// the caller gives up on a search after its own time limit.
int uia_scroll_aim(POINT pt, BOOL vertical, DWORD since_ms, POINT *target)
{
    HWND root;
    ScrollCache *e;
    DWORD now;
    int answer;

    root = scroll_root_for(pt);
    if (!root)
        return SCROLL_AIM_POINTER;
    if (!scroll_worker_start_nonblocking()) {
        // Still starting: ask again. Failed for good: no list, ever.
        return InterlockedCompareExchange(&g_sc_state, 1, 1) == 1
                   ? SCROLL_AIM_ASKING : SCROLL_AIM_POINTER;
    }

    now = GetTickCount();
    if (since_ms == 0)
        InterlockedExchange(&g_sc_last_use, (LONG)now);
    EnterCriticalSection(&g_sc_cs);
    e = scroll_cache_find(root);
    if (e && e->valid && (DWORD)(now - e->tick) < SCROLL_STALE_MS) {
        BOOL fresh = (DWORD)(now - e->tick) <= SCROLL_FRESH_MS;
        BOOL redirect;

        if (!fresh && !e->refreshing)
            scroll_queue_locked(root, now);
        e->last_used = now;
        redirect = scroll_area_answer(e, pt, vertical, target);
        // Only a fresh list may send the wheel away from the pointer at
        // once: the area under it may be new since (another chat opened in
        // Teams), and an older list would aim at a side panel instead. A
        // slightly old list that would redirect gets its refresh a short
        // while, then still redirects with what it has: a slow search is
        // far worse than a stale answer.
        if (fresh || !redirect)
            answer = redirect ? SCROLL_AIM_TARGET : SCROLL_AIM_POINTER;
        else if (since_ms >= SCROLL_STALE_WAIT_MS)
            answer = SCROLL_AIM_TARGET;
        else
            answer = SCROLL_AIM_ASKING;
        LeaveCriticalSection(&g_sc_cs);
        return answer;
    }
    if (!e) {
        e = scroll_cache_alloc(root, now);
        if (!e) {
            LeaveCriticalSection(&g_sc_cs);
            return SCROLL_AIM_POINTER;
        }
    }
    if (!e->refreshing)
        scroll_queue_locked(root, now);
    e->last_used = now;
    LeaveCriticalSection(&g_sc_cs);
    return SCROLL_AIM_ASKING;
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

static void scroll_end_stop(void);

void uia_scroll_stop(void)
{
    int i;

    scroll_end_stop();
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
// Scroll to the top (g g) or the bottom (Shift + g) on press. Its own MTA
// thread and engine, not the scroll-target worker: that one may be busy for
// hundreds of milliseconds in a FindAll, which would delay the jump.
// ScrollPattern's SetScrollPercent lands exactly, at once, and does not pass
// through the app's wheel handling: it ignores the held Shift, where a wheel
// event does not (browsers turn Shift + wheel into sideways scrolling), and
// it cannot be cut short the way the wheel glide to the top was in Chrome.

// UIA's "leave this axis as it is" scroll percent. Its own name: the SDK
// headers may spell the constant differently.
#define SCROLL_PERCENT_UNCHANGED (-1.0)
#define UIA_SCROLL_END_BELOW 99.9  // a percent this close to 100 is the bottom
#define UIA_SCROLL_END_ABOVE 0.1   // a percent this close to 0 is the top
#define SCROLL_END_TREE_MAX 64     // walk at most this far up the raw view

typedef struct {
    BOOL  set;
    LONG  seq;
    POINT pt;
    BOOL  top;      // the top (g g); otherwise the bottom (Shift + g)
} ScrollEndRequest;

static HANDLE g_se_thread;
static HANDLE g_se_wake;
static HANDLE g_se_answered;          // auto reset: an answer has arrived
static CRITICAL_SECTION g_se_cs;
static volatile LONG g_se_state;      // 0 off, 1 starting, 2 running, 3 failed
static volatile LONG g_se_quit;
// g_se_cs guards the request slot and the answer; a late answer for an older
// request is ignored by its sequence number.
static ScrollEndRequest g_se_req;
static LONG g_se_seq;
static LONG g_se_answer_seq;
static BOOL g_se_answer;

// Scrolls the area at pt to the top or the bottom, chaining up like a wheel
// does: an element already at that end passes the ask to its parent. TRUE
// when a vertically scrollable element was found at all (all at that end).
static BOOL scroll_end_at(IUIAutomation *automation, POINT pt, BOOL top)
{
    IUIAutomationElement *el = NULL;
    IUIAutomationTreeWalker *walker = NULL;
    HWND hwnd;
    DWORD pid = 0;
    BOOL found = FALSE;
    int depth;

    hwnd = WindowFromPoint(pt);
    if (!hwnd)
        return FALSE;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId())
        return FALSE;
    if (FAILED(automation->lpVtbl->ElementFromPoint(automation, pt, &el)) ||
        !el)
        return FALSE;
    if (FAILED(automation->lpVtbl->get_RawViewWalker(automation, &walker)) ||
        !walker) {
        el->lpVtbl->Release(el);
        return FALSE;
    }
    for (depth = 0; depth < SCROLL_END_TREE_MAX; ++depth) {
        IUIAutomationScrollPattern *sp = NULL;
        IUIAutomationElement *parent = NULL;
        BOOL vertical = FALSE;
        HRESULT hr;

        if (SUCCEEDED(el->lpVtbl->GetCurrentPatternAs(
                el, UIA_ScrollPatternId, &g_iid_scroll_pattern,
                (void **)&sp)) && sp) {
            if (SUCCEEDED(sp->lpVtbl->get_CurrentVerticallyScrollable(
                    sp, &vertical)) && vertical) {
                double pct = 0.0;
                BOOL there;

                found = TRUE;
                // Unreadable counts as "not at that end": try the scroll.
                if (FAILED(sp->lpVtbl->get_CurrentVerticalScrollPercent(
                        sp, &pct)))
                    there = FALSE;
                else
                    there = top ? pct <= UIA_SCROLL_END_ABOVE
                                : pct >= UIA_SCROLL_END_BELOW;
                if (!there) {
                    hr = sp->lpVtbl->SetScrollPercent(
                        sp, SCROLL_PERCENT_UNCHANGED, top ? 0.0 : 100.0);
                    sp->lpVtbl->Release(sp);
                    el->lpVtbl->Release(el);
                    walker->lpVtbl->Release(walker);
                    return SUCCEEDED(hr);
                }
            }
            sp->lpVtbl->Release(sp);
        }
        hr = walker->lpVtbl->GetParentElement(walker, el, &parent);
        el->lpVtbl->Release(el);
        el = NULL;
        if (FAILED(hr) || !parent)
            break;
        el = parent;
    }
    if (el)
        el->lpVtbl->Release(el);
    walker->lpVtbl->Release(walker);
    return found;
}

static DWORD WINAPI scroll_end_worker(void *arg)
{
    UiaEngine engine;
    HRESULT hr;
    BOOL ready = FALSE, com_ready;
    DWORD last_engine_try;
    (void)arg;

    memset(&engine, 0, sizeof(engine));
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    com_ready = hr == S_OK || hr == S_FALSE;
    // uia_engine_init takes the engine lock itself; it is not reentrant.
    if (com_ready)
        ready = uia_engine_init(&engine, FALSE);
    last_engine_try = GetTickCount();

    for (;;) {
        ScrollEndRequest req;
        BOOL answered = FALSE;
        DWORD now, wait_ms;
        DWORD woke;

        // Without an engine, wake up now and then to retry it; while it runs,
        // a request is the only thing that wakes the worker. A failed COM
        // start can never get one, so there is nothing to wake up for.
        wait_ms = ready || !com_ready ? INFINITE : SCROLL_ENGINE_RETRY_MS;
        woke = WaitForSingleObject(g_se_wake, wait_ms);
        if (woke != WAIT_OBJECT_0 && woke != WAIT_TIMEOUT)
            break;
        if (woke == WAIT_TIMEOUT) {
            now = GetTickCount();
            if (com_ready)
                ready = uia_engine_init(&engine, FALSE);
            last_engine_try = now;
            if (InterlockedCompareExchange(&g_se_quit, 0, 0))
                break;
            continue;
        }
        if (InterlockedCompareExchange(&g_se_quit, 0, 0))
            break;
        EnterCriticalSection(&g_se_cs);
        req = g_se_req;
        g_se_req.set = FALSE;
        LeaveCriticalSection(&g_se_cs);
        if (!req.set)
            continue;

        // A request may have woken us before the retry interval was over.
        now = GetTickCount();
        if (!ready && com_ready &&
            (DWORD)(now - last_engine_try) >= SCROLL_ENGINE_RETRY_MS) {
            ready = uia_engine_init(&engine, FALSE);
            last_engine_try = now;
        }
        if (ready)
            answered = scroll_end_at(engine.automation, req.pt, req.top);

        EnterCriticalSection(&g_se_cs);
        g_se_answer_seq = req.seq;
        g_se_answer = answered;
        LeaveCriticalSection(&g_se_cs);
        SetEvent(g_se_answered);
    }

    uia_engine_cleanup(&engine);
    if (com_ready)
        CoUninitialize();
    return 0;
}

static BOOL scroll_end_worker_init(void)
{
    InitializeCriticalSection(&g_se_cs);
    memset(&g_se_req, 0, sizeof(g_se_req));
    g_se_seq = 0;
    g_se_answer_seq = 0;
    g_se_answer = FALSE;
    g_se_quit = 0;
    g_se_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_se_answered = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_se_wake || !g_se_answered) {
        if (g_se_wake)
            CloseHandle(g_se_wake);
        if (g_se_answered)
            CloseHandle(g_se_answered);
        g_se_wake = NULL;
        g_se_answered = NULL;
        DeleteCriticalSection(&g_se_cs);
        return FALSE;
    }
    g_se_thread = CreateThread(NULL, 0, scroll_end_worker, NULL, 0, NULL);
    if (!g_se_thread) {
        CloseHandle(g_se_wake);
        CloseHandle(g_se_answered);
        g_se_wake = NULL;
        g_se_answered = NULL;
        DeleteCriticalSection(&g_se_cs);
        return FALSE;
    }
    return TRUE;
}

static BOOL scroll_end_worker_start(void)
{
    if (InterlockedCompareExchange(&g_se_state, 1, 0) == 0)
        InterlockedExchange(&g_se_state, scroll_end_worker_init() ? 2 : 3);
    while (InterlockedCompareExchange(&g_se_state, 1, 1) == 1)
        Sleep(0);
    return InterlockedCompareExchange(&g_se_state, 2, 2) == 2;
}

static void scroll_end_stop(void)
{
    while (InterlockedCompareExchange(&g_se_state, 1, 1) == 1)
        Sleep(0);
    if (InterlockedCompareExchange(&g_se_state, 0, 0) != 2)
        return;
    InterlockedExchange(&g_se_quit, 1);
    SetEvent(g_se_wake);
    if (WaitForSingleObject(g_se_thread, 3000) != WAIT_OBJECT_0)
        return;
    CloseHandle(g_se_thread);
    CloseHandle(g_se_wake);
    CloseHandle(g_se_answered);
    g_se_thread = NULL;
    g_se_wake = NULL;
    g_se_answered = NULL;
    DeleteCriticalSection(&g_se_cs);
    InterlockedExchange(&g_se_state, 0);
}

BOOL uia_scroll_end(POINT pt, BOOL top, DWORD timeout_ms)
{
    DWORD start;
    LONG seq;

    if (!scroll_end_worker_start())
        return FALSE;
    EnterCriticalSection(&g_se_cs);
    seq = ++g_se_seq;
    g_se_req.pt = pt;
    g_se_req.top = top;
    g_se_req.seq = seq;
    g_se_req.set = TRUE;
    LeaveCriticalSection(&g_se_cs);
    SetEvent(g_se_wake);

    start = GetTickCount();
    for (;;) {
        DWORD elapsed;

        EnterCriticalSection(&g_se_cs);
        if (g_se_answer_seq == seq) {
            BOOL answer = g_se_answer;

            LeaveCriticalSection(&g_se_cs);
            return answer;
        }
        LeaveCriticalSection(&g_se_cs);
        elapsed = GetTickCount() - start;
        if (elapsed >= timeout_ms)
            return FALSE;
        if (WaitForSingleObject(g_se_answered, timeout_ms - elapsed) !=
            WAIT_OBJECT_0)
            return FALSE;
    }
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
#define PAGE_BUSY_TIMEOUT_MS 50     // how long a page window may take to say it is loading
#ifndef STATE_SYSTEM_BUSY
#define STATE_SYSTEM_BUSY 0x00000800
#endif
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
// uia_focus_notify: the thread told of every answer (0 = none), its message.
static volatile LONG g_fx_notify_tid;
static UINT g_fx_notify_msg;

void uia_focus_notify(DWORD tid, UINT msg)
{
    g_fx_notify_msg = msg;
    InterlockedExchange(&g_fx_notify_tid, (LONG)tid);
}

// An answer is in: a key that typed while it was pending decides on it now
// (input.c tap_answer). A post only; the worker never waits for that thread.
static void focus_notify(void)
{
    DWORD tid = (DWORD)InterlockedCompareExchange(&g_fx_notify_tid, 0, 0);

    if (tid)
        PostThreadMessageW(tid, g_fx_notify_msg, 0, 0);
}

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

// oleacc's ObjectFromLresult, loaded once; the focus and scan workers both
// call this, so the pointer is published with an interlocked exchange.
static ObjectFromLresultFn oleacc_from_lresult(void)
{
    static PVOID fn;
    static LONG tried;

    if (!InterlockedCompareExchange(&tried, 1, 0)) {
        HMODULE oleacc = LoadLibraryW(L"oleacc.dll");

        if (oleacc)
            InterlockedExchangePointer(
                &fn, (PVOID)GetProcAddress(oleacc, "ObjectFromLresult"));
    }
    return (ObjectFromLresultFn)InterlockedCompareExchangePointer(&fn, NULL,
                                                                  NULL);
}

// The MSAA state of hwnd's object objid (OBJID_CARET, OBJID_CLIENT), asked
// with WM_GETOBJECT under a timeout so a hung window cannot hold a worker.
static BOOL msaa_state(HWND hwnd, LONG objid, UINT timeout_ms, LONG *out)
{
    ObjectFromLresultFn from_lresult = oleacc_from_lresult();
    DWORD_PTR result = 0;
    IAccessibleCaret *acc = NULL;
    VARIANT self, state;
    BOOL ok = FALSE;

    if (!from_lresult || !hwnd)
        return FALSE;
    if (!SendMessageTimeoutW(hwnd, WM_GETOBJECT, 0, (LPARAM)objid,
                             SMTO_ABORTIFHUNG, timeout_ms, &result) ||
        (LONG_PTR)result <= 0)
        return FALSE;
    if (FAILED(from_lresult((LRESULT)result, &g_iid_iaccessible, 0,
                            (void **)&acc)) || !acc)
        return FALSE;
    VariantInit(&self);
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    VariantInit(&state);
    if (SUCCEEDED(acc->lpVtbl->get_accState(acc, self, &state)) &&
        state.vt == VT_I4) {
        *out = state.lVal;
        ok = TRUE;
    }
    VariantClear(&state);
    acc->lpVtbl->Release(acc);
    return ok;
}

// Chromium (Chrome, Edge, Electron apps, WebView2) makes no Win32 caret, but
// its window answers OBJID_CARET with a caret that is visible exactly while
// typing goes into a text input. That holds where UI Automation names
// something else: an input with aria-activedescendant (Zendesk's ticket
// fields, any autocomplete box) reports the highlighted option as the focus.
// Windows that do not answer themselves are skipped; their Win32 caret is
// already in the GUI thread info. Runs on the focus worker only.
static BOOL caret_visible(HWND hwnd)
{
    LONG state;

    if (!msaa_state(hwnd, OBJID_CARET, FOCUS_CARET_TIMEOUT_MS, &state))
        return FALSE;
    return (state & STATE_SYSTEM_INVISIBLE) == 0;
}

// Finds the largest visible Chromium page window (Chrome_RenderWidgetHostHWND)
// under a top-level window: the page, not DevTools or a side panel.
typedef struct {
    HWND best;
    LONG64 area;
} PageWindowSearch;

static BOOL CALLBACK page_window_proc(HWND hwnd, LPARAM lp)
{
    PageWindowSearch *s = (PageWindowSearch *)lp;
    WCHAR cls[64];
    RECT r;

    if (!IsWindowVisible(hwnd) ||
        !GetClassNameW(hwnd, cls, (int)(sizeof(cls) / sizeof(cls[0]))) ||
        wcscmp(cls, L"Chrome_RenderWidgetHostHWND") != 0 ||
        !GetWindowRect(hwnd, &r))
        return TRUE;
    if (rect_area(&r) > s->area) {
        s->area = rect_area(&r);
        s->best = hwnd;
    }
    return TRUE;
}

// A Chromium page still loading. Measured on 2026-11-08
// (tools/load_probe.c): from the navigation until its first content reaches
// the accessibility tree (1.5 s on a page streamed slowly), the document
// answers with STATE_SYSTEM_BUSY and has no children, so a scan finds only
// the browser's own toolbar. The flag clears as the content comes in; the
// page's load event (images and all) comes later and is not waited for.
// The old page stays, not busy, until the new one commits. Runs on the scan
// worker; one call is a message and a state query, about a millisecond.
static BOOL page_busy(HWND root)
{
    PageWindowSearch s;
    LONG state;

    if (!g_cfg.scan_loading || !root || !uia_chromium_window(root))
        return FALSE;
    s.best = NULL;
    s.area = 0;
    EnumChildWindows(root, page_window_proc, (LPARAM)&s);
    if (!s.best ||
        !msaa_state(s.best, OBJID_CLIENT, PAGE_BUSY_TIMEOUT_MS, &state))
        return FALSE;
    return (state & STATE_SYSTEM_BUSY) != 0;
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
    // A panel's own root or pane holds the focus when none of its controls
    // does, and a panel has no game or canvas that needs letters; its text
    // fields are already caught by the caret check and focus_element_kind.
    if (kind == FOCUS_UNKNOWN && uia_chromium_panel(GetAncestor(focus, GA_ROOT)))
        kind = FOCUS_OTHER;
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

// ---------------------------------------------------------------------------
// esc_unfocus: Esc in a text field of a web page, or in a browser's address
// bar, moves the keyboard focus to the page itself, so the keys without Alt
// work at once. Measured on 2026-11-08 in Chrome (managed UI Automation and
// CDP): SetFocus on the page's Document element leaves document.activeElement
// on <body> (Blink focuses the document element for that action), and from
// the address bar it moves the focus into the page; the Document is the child
// of the Pane that Chrome_RenderWidgetHostHWND answers with. Esc still reaches
// the app first (it closes a suggestion list, reverts the address bar), and
// the same press then leaves the field: Google's search box opens its
// suggestions on every click, and giving the first Esc to an open list
// meant two presses there (removed on the user's report).
//
// The hook arms it at the Esc press (uia_focus_leave_down) and the worker
// finds the field and where the focus goes at once. The move waits for the release (uia_focus_leave_up) plus LEAVE_AFTER_MS,
// and only if the focus is still in a text field of that window: an Esc that
// closed a dialog or moved the focus itself is left alone. Native apps are
// never touched: their windows hand the focus straight back to their edit
// control, and their Esc means too much (cancel, close).

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

#define LEAVE_AFTER_MS 30       // after the Esc release, let the app finish with it
#define LEAVE_HOLD_MS 1500      // an Esc held longer than this does nothing
#define LEAVE_DEPTH_MAX 64      // parents walked looking for the field's Document
#define LEAVE_SETTLE_MS 50      // ask the focus again this long after the move

// Guarded by g_fx_cs. g_lv_seq counts the armed presses; the worker handles
// each once (LeaveState.done_seq).
static LONG  g_lv_seq;
static BOOL  g_lv_up;
static BOOL  g_lv_dead;
static DWORD g_lv_down_tick;
static DWORD g_lv_up_tick;

typedef struct {
    LONG seq;                           // the press looked at
    LONG done_seq;                      // the press finished with
    IUIAutomationElement *target;       // where the focus goes
    HWND focus;                         // the focus window when it was looked at
    IUIAutomationCacheRequest *cache;   // full elements, control type and more
} LeaveState;

typedef BOOL (WINAPI *QueryImageNameFn)(HANDLE, DWORD, LPWSTR, PDWORD);

// esc_unfocus = 1: a web browser's own process (not an app built on
// Chromium, whose editors use Esc for themselves: VS Code, Slack).
static BOOL browser_process(DWORD pid)
{
    static const WCHAR *const names[] = {
        L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe",
        L"vivaldi.exe", L"opera.exe", L"whale.exe", L"chromium.exe",
        L"arc.exe", L"thorium.exe", L"librewolf.exe", L"waterfox.exe",
        L"floorp.exe", L"zen.exe", NULL
    };
    static QueryImageNameFn query;
    static DWORD last_pid;
    static BOOL last_answer;
    WCHAR path[MAX_PATH];
    DWORD len = MAX_PATH;
    const WCHAR *base;
    HANDLE proc;
    BOOL ok;
    int i;

    if (pid == last_pid && pid)
        return last_answer;
    if (!query)
        query = (QueryImageNameFn)(PVOID)GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "QueryFullProcessImageNameW");
    if (!query)
        return FALSE;
    proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc)
        return FALSE;
    ok = query(proc, 0, path, &len);
    CloseHandle(proc);
    if (!ok)
        return FALSE;
    base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    last_pid = pid;
    last_answer = FALSE;
    for (i = 0; names[i]; ++i)
        if (_wcsicmp(base, names[i]) == 0)
            last_answer = TRUE;
    return last_answer;
}

static BOOL leave_cache_ready(IUIAutomation *automation, LeaveState *lv)
{
    static const int props[] = {
        UIA_ControlTypePropertyId, UIA_IsPasswordPropertyId,
        UIA_IsValuePatternAvailablePropertyId, UIA_ValueIsReadOnlyPropertyId,
        UIA_IsTextPatternAvailablePropertyId,
        UIA_IsLegacyIAccessiblePatternAvailablePropertyId,
        UIA_LegacyIAccessibleStatePropertyId
    };
    size_t i;

    if (lv->cache)
        return TRUE;
    if (!automation ||
        FAILED(automation->lpVtbl->CreateCacheRequest(automation,
                                                      &lv->cache)) ||
        !lv->cache) {
        lv->cache = NULL;
        return FALSE;
    }
    for (i = 0; i < sizeof(props) / sizeof(props[0]); ++i)
        lv->cache->lpVtbl->AddProperty(lv->cache, props[i]);
    lv->cache->lpVtbl->put_AutomationElementMode(lv->cache, UIA_MODE_FULL);
    return TRUE;
}

static int cached_control_type(IUIAutomationElement *el)
{
    int ct = 0;

    el->lpVtbl->get_CachedControlType(el, &ct);
    return ct;
}

// The nearest Document above el: the page, or the frame, the field is in.
static IUIAutomationElement *leave_document_above(
    IUIAutomation *automation, IUIAutomationCacheRequest *cache,
    IUIAutomationElement *el)
{
    IUIAutomationTreeWalker *walker = NULL;
    IUIAutomationElement *cur = el, *parent;
    int depth;

    if (FAILED(automation->lpVtbl->get_RawViewWalker(automation, &walker)) ||
        !walker)
        return NULL;
    // cur is el (the caller's) at first, then a parent of our own.
    for (depth = 0; depth < LEAVE_DEPTH_MAX; ++depth) {
        parent = NULL;
        if (FAILED(walker->lpVtbl->GetParentElementBuildCache(
                walker, cur, cache, &parent)) || !parent)
            break;
        if (cur != el)
            cur->lpVtbl->Release(cur);
        cur = parent;
        if (cached_control_type(cur) == UIA_DocumentControlTypeId) {
            walker->lpVtbl->Release(walker);
            return cur;
        }
    }
    if (cur != el)
        cur->lpVtbl->Release(cur);
    walker->lpVtbl->Release(walker);
    return NULL;
}

// A Chromium window's page Document, for a field outside the page (the
// address bar): the child of the Pane its largest page window answers with.
static IUIAutomationElement *leave_page_document(
    IUIAutomation *automation, IUIAutomationCacheRequest *cache, HWND root)
{
    IUIAutomationTreeWalker *walker = NULL;
    IUIAutomationElement *pane = NULL, *c = NULL, *next;
    PageWindowSearch s;
    int n;

    s.best = NULL;
    s.area = 0;
    EnumChildWindows(root, page_window_proc, (LPARAM)&s);
    if (!s.best ||
        FAILED(automation->lpVtbl->ElementFromHandle(automation, s.best,
                                                     &pane)) || !pane)
        return NULL;
    if (FAILED(automation->lpVtbl->get_RawViewWalker(automation, &walker)) ||
        !walker) {
        pane->lpVtbl->Release(pane);
        return NULL;
    }
    if (FAILED(walker->lpVtbl->GetFirstChildElementBuildCache(
            walker, pane, cache, &c)))
        c = NULL;
    for (n = 0; c && n < 8; ++n) {
        if (cached_control_type(c) == UIA_DocumentControlTypeId)
            break;
        next = NULL;
        if (FAILED(walker->lpVtbl->GetNextSiblingElementBuildCache(
                walker, c, cache, &next)))
            next = NULL;
        c->lpVtbl->Release(c);
        c = next;
    }
    if (c && cached_control_type(c) != UIA_DocumentControlTypeId) {
        c->lpVtbl->Release(c);
        c = NULL;
    }
    walker->lpVtbl->Release(walker);
    pane->lpVtbl->Release(pane);
    return c;
}

static void leave_drop(LeaveState *lv)
{
    if (lv->target)
        lv->target->lpVtbl->Release(lv->target);
    lv->target = NULL;
    lv->focus = NULL;
}

// Whether typing goes into a text field in focus (the window with the
// keyboard focus) now; *el gets the focused element when asked for.
static BOOL leave_in_text(IUIAutomation *automation, LeaveState *lv,
                          HWND focus, const GUITHREADINFO *gti,
                          IUIAutomationElement **el)
{
    BOOL caret = gti->hwndCaret || (gti->flags & GUI_CARETBLINKING) ||
                 caret_visible(focus);
    IUIAutomationElement *e = NULL;
    BOOL text;

    if (FAILED(automation->lpVtbl->GetFocusedElementBuildCache(
            automation, lv->cache, &e)))
        e = NULL;
    text = caret || (e && focus_element_kind(e) == FOCUS_TEXT);
    if (el)
        *el = e;
    else if (e)
        e->lpVtbl->Release(e);
    return text;
}

// At the Esc press: is the focus in a text field we may leave, and where
// does the focus go? Sets lv->target, or logs why not.
static void leave_look(IUIAutomation *automation, LeaveState *lv)
{
    IUIAutomationElement *el = NULL;
    GUITHREADINFO gti;
    HWND focus, root;
    DWORD pid;
    const WCHAR *why = NULL;

    leave_drop(lv);
    if (!automation || !leave_cache_ready(automation, lv)) {
        why = L"no UI Automation";
    } else if (!focus_window(&focus, &gti, &pid) ||
               pid == GetCurrentProcessId()) {
        why = L"no focus window";
    } else if (gti.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE |
                            GUI_SYSTEMMENUMODE)) {
        why = L"a menu is open";
    } else if (!(root = GetAncestor(focus, GA_ROOT)) ||
               !uia_browser_window(root) ||
               (g_cfg.esc_unfocus < 2 && !browser_process(pid))) {
        why = g_cfg.esc_unfocus < 2 ? L"not a web browser"
                                    : L"no web content";
    } else if (!leave_in_text(automation, lv, focus, &gti, &el)) {
        why = L"not in a text field";
    } else {
        if (el)
            lv->target = leave_document_above(automation, lv->cache, el);
        if (!lv->target && uia_chromium_window(root))
            lv->target = leave_page_document(automation, lv->cache, root);
        if (!lv->target)
            why = L"no page to move the focus to";
        else
            lv->focus = focus;
    }
    if (el)
        el->lpVtbl->Release(el);
    if (why && g_cfg.log)
        app_log(L"esc: the focus stays, %ls", why);
}

// After the release: move the focus to the page, if Esc left it in the field.
static void leave_act(IUIAutomation *automation, LeaveState *lv)
{
    GUITHREADINFO gti;
    HWND focus;
    DWORD pid;
    HRESULT hr;

    if (!focus_window(&focus, &gti, &pid) || focus != lv->focus) {
        if (g_cfg.log)
            app_log(L"esc: the focus stays, it moved to another window");
        return;
    }
    if (!leave_in_text(automation, lv, focus, &gti, NULL)) {
        if (g_cfg.log)
            app_log(L"esc: the focus stays, Esc already took it out of the "
                    L"field");
        return;
    }
    hr = lv->target->lpVtbl->SetFocus(lv->target);
    if (g_cfg.log)
        app_log(L"esc: focus to the page (hr 0x%08lx)", (unsigned long)hr);
    uia_focus_changed(LEAVE_SETTLE_MS);
}

// Runs what is due for the latest armed Esc; returns how long until the
// next step is due (0 = nothing pending; a key-up wakes the worker anyway).
static DWORD leave_step(IUIAutomation *automation, LeaveState *lv)
{
    LONG seq;
    BOOL up, dead;
    DWORD down_tick, up_tick, now;

    EnterCriticalSection(&g_fx_cs);
    seq = g_lv_seq;
    up = g_lv_up;
    dead = g_lv_dead;
    down_tick = g_lv_down_tick;
    up_tick = g_lv_up_tick;
    LeaveCriticalSection(&g_fx_cs);
    if (seq == lv->done_seq)
        return 0;
    if (dead) {
        leave_drop(lv);
        lv->done_seq = seq;
        return 0;
    }
    if (lv->seq != seq) {
        lv->seq = seq;
        leave_look(automation, lv);
        if (!lv->target) {
            lv->done_seq = seq;
            return 0;
        }
    }
    now = GetTickCount();
    if (!up) {
        if ((DWORD)(now - down_tick) < LEAVE_HOLD_MS)
            return LEAVE_HOLD_MS - (DWORD)(now - down_tick);
        if (g_cfg.log)
            app_log(L"esc: the focus stays, Esc was held");
        leave_drop(lv);
        lv->done_seq = seq;
        return 0;
    }
    if ((DWORD)(now - up_tick) < LEAVE_AFTER_MS)
        return LEAVE_AFTER_MS - (DWORD)(now - up_tick);
    leave_act(automation, lv);
    leave_drop(lv);
    lv->done_seq = seq;
    return 0;
}

static DWORD WINAPI focus_worker(void *arg)
{
    IUIAutomation *automation = NULL;
    IUIAutomationCacheRequest *cache = NULL;
    LeaveState lv;
    HRESULT hr;
    HWND last_hwnd = NULL;
    int last_kind = -1;
    DWORD engine_tried = GetTickCount();
    LONG retry_gen = -1;        // gen whose unknown answer gets fast retries
    int retry_done = 0;
    DWORD retry_at = 0;
    (void)arg;

    memset(&lv, 0, sizeof(lv));
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        engine_lock();
        focus_engine_init(&automation, &cache);
        engine_unlock();
    }

    while (!InterlockedCompareExchange(&g_fx_quit, 0, 0)) {
        LONG gen;
        BOOL pending, retrying;
        DWORD wait = 0, now, leave_wait;
        HWND hwnd;
        int kind;

        // esc_unfocus first: its look has to beat the app's handling of Esc.
        leave_wait = leave_step(automation, &lv);

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
            WaitForSingleObject(g_fx_wake,
                                leave_wait && leave_wait < wait ? leave_wait
                                                                : wait);
            continue;
        }
        // Answered: poll now and then. The old answer stays valid while the
        // poll asks again; any change wakes us and invalidates it.
        if (!pending && !retrying) {
            BOOL active = g_cfg.bare_keys && user_active();
            DWORD idle = active ? FOCUS_POLL_MS : 5000;

            if (leave_wait && leave_wait < idle) {
                WaitForSingleObject(g_fx_wake, leave_wait);
                continue;
            }
            if (WaitForSingleObject(g_fx_wake, idle) != WAIT_TIMEOUT ||
                !active)
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
                focus_notify();
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
                focus_notify();
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

    leave_drop(&lv);
    if (lv.cache)
        lv.cache->lpVtbl->Release(lv.cache);
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
    g_lv_seq = 0;
    g_lv_up = FALSE;
    g_lv_dead = FALSE;
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

// Starts the focus worker on first use; TRUE once it runs.
static BOOL focus_worker_ready(void)
{
    if (InterlockedCompareExchange(&g_fx_state, 1, 0) == 0)
        InterlockedExchange(&g_fx_state, focus_worker_init() ? 2 : 3);
    return InterlockedCompareExchange(&g_fx_state, 2, 2) == 2;
}

void uia_focus_leave_down(void)
{
    if (!focus_worker_ready())
        return;
    EnterCriticalSection(&g_fx_cs);
    ++g_lv_seq;
    g_lv_up = FALSE;
    g_lv_dead = FALSE;
    g_lv_down_tick = GetTickCount();
    LeaveCriticalSection(&g_fx_cs);
    SetEvent(g_fx_wake);
}

void uia_focus_leave_up(void)
{
    if (InterlockedCompareExchange(&g_fx_state, 2, 2) != 2)
        return;
    EnterCriticalSection(&g_fx_cs);
    g_lv_up = TRUE;
    g_lv_up_tick = GetTickCount();
    LeaveCriticalSection(&g_fx_cs);
    SetEvent(g_fx_wake);
}

void uia_focus_leave_cancel(void)
{
    if (InterlockedCompareExchange(&g_fx_state, 2, 2) != 2)
        return;
    EnterCriticalSection(&g_fx_cs);
    g_lv_dead = TRUE;
    LeaveCriticalSection(&g_fx_cs);
    SetEvent(g_fx_wake);
}

void uia_focus_changed(DWORD delay_ms)
{
    DWORD at;

    if (!focus_worker_ready())
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
