#define COBJMACROS
#define CONST_VTABLE
#include "ui/accessibility.h"

#include <SDL_syswm.h>
#include <commctrl.h>
#include <oleacc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct ph_accessibility {
    LONG references;
    HWND window;
    SDL_Window *sdl_window;
    bool com_initialized;
    ph_workspace *workspace;
    ph_accessibility_action run_action;
    void *context;
    ph_workspace_control *snapshot;
    int count, focused;
};

typedef struct workspace_accessible {
    IAccessible iface;
    LONG references;
    ph_accessibility *access;
    ITypeInfo *type_info;
    int count;
    ph_workspace_control controls[];
} workspace_accessible;

static void release_access(ph_accessibility *access)
{
    if (!InterlockedDecrement(&access->references)) {
        free(access->snapshot);
        free(access);
    }
}

static workspace_accessible *provider(IAccessible *self)
{
    return (workspace_accessible *)self;
}

static ULONG STDMETHODCALLTYPE add_ref(IAccessible *self)
{
    return (ULONG)InterlockedIncrement(&provider(self)->references);
}

static ULONG STDMETHODCALLTYPE release(IAccessible *self)
{
    workspace_accessible *object = provider(self);
    LONG remaining = InterlockedDecrement(&object->references);
    if (!remaining) {
        if (object->type_info)
            ITypeInfo_Release(object->type_info);
        release_access(object->access);
        free(object);
    }
    return (ULONG)remaining;
}

static HRESULT STDMETHODCALLTYPE query(IAccessible *self, REFIID id, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(id, &IID_IUnknown) && !IsEqualIID(id, &IID_IDispatch) &&
        !IsEqualIID(id, &IID_IAccessible))
        return E_NOINTERFACE;
    *out = self;
    add_ref(self);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE type_count(IAccessible *self, UINT *out)
{
    if (!out)
        return E_POINTER;
    *out = provider(self)->type_info != NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE type_info(IAccessible *self, UINT index, LCID locale,
                                           ITypeInfo **out)
{
    (void)locale;
    if (!out)
        return E_POINTER;
    *out = NULL;
    ITypeInfo *info = provider(self)->type_info;
    if (index || !info)
        return DISP_E_BADINDEX;
    ITypeInfo_AddRef(info);
    *out = info;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ids_of_names(IAccessible *self, REFIID id, LPOLESTR *names,
                                              UINT count, LCID locale, DISPID *out)
{
    (void)locale;
    if (!IsEqualIID(id, &IID_NULL))
        return DISP_E_UNKNOWNINTERFACE;
    ITypeInfo *info = provider(self)->type_info;
    return info ? DispGetIDsOfNames(info, names, count, out) : E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dispatch(IAccessible *self, DISPID id, REFIID iid, LCID locale,
                                          WORD flags, DISPPARAMS *params, VARIANT *out,
                                          EXCEPINFO *exception, UINT *argument)
{
    (void)locale;
    if (!IsEqualIID(iid, &IID_NULL))
        return DISP_E_UNKNOWNINTERFACE;
    ITypeInfo *info = provider(self)->type_info;
    return info ? DispInvoke(self, info, id, flags, params, out, exception, argument) : E_NOTIMPL;
}

static int child_id(IAccessible *self, VARIANT child)
{
    if (V_VT(&child) != VT_I4 || V_I4(&child) < 0 || V_I4(&child) > provider(self)->count)
        return -1;
    return (int)V_I4(&child);
}

static const ph_workspace_control *control(IAccessible *self, VARIANT child)
{
    int index = child_id(self, child);
    return index > 0 ? &provider(self)->controls[index - 1] : NULL;
}

static HRESULT string_result(BSTR *out, const char *utf8)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    int length = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (!length)
        return E_FAIL;
    *out = SysAllocStringLen(NULL, (UINT)length - 1);
    if (!*out)
        return E_OUTOFMEMORY;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, *out, length))
        return S_OK;
    SysFreeString(*out);
    *out = NULL;
    return E_FAIL;
}

static HRESULT integer_result(VARIANT *out, LONG value)
{
    if (!out)
        return E_POINTER;
    VariantInit(out);
    V_VT(out) = VT_I4;
    V_I4(out) = value;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE get_parent(IAccessible *self, IDispatch **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    HWND window = provider(self)->access->window;
    return window ? AccessibleObjectFromWindow(window, OBJID_WINDOW, &IID_IAccessible, (void **)out)
                  : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE get_count(IAccessible *self, LONG *out)
{
    if (!out)
        return E_POINTER;
    *out = provider(self)->count;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE get_child(IAccessible *self, VARIANT child, IDispatch **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL; /* Controls are simple children of this client object. */
    return child_id(self, child) > 0 ? S_FALSE : E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE get_name(IAccessible *self, VARIANT child, BSTR *out)
{
    const ph_workspace_control *item = control(self, child);
    if (item)
        return string_result(out, item->name);
    return child_id(self, child) == CHILDID_SELF ? string_result(out, "Porthole") : E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE get_value(IAccessible *self, VARIANT child, BSTR *out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    const ph_workspace_control *item = control(self, child);
    return item && !item->sensitive && (item->editable || item->value[0])
               ? string_result(out, item->value)
               : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE no_string(IAccessible *self, VARIANT child, BSTR *out)
{
    (void)self;
    (void)child;
    if (!out)
        return E_POINTER;
    *out = NULL;
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE get_role(IAccessible *self, VARIANT child, VARIANT *out)
{
    const ph_workspace_control *item = control(self, child);
    if (item)
        return integer_result(out, item->editable ? ROLE_SYSTEM_TEXT
                                   : item->toggle ? ROLE_SYSTEM_CHECKBUTTON
                                                  : ROLE_SYSTEM_PUSHBUTTON);
    return child_id(self, child) == CHILDID_SELF ? integer_result(out, ROLE_SYSTEM_CLIENT)
                                                  : E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE get_state(IAccessible *self, VARIANT child, VARIANT *out)
{
    const ph_workspace_control *item = control(self, child);
    if (!item && child_id(self, child) != CHILDID_SELF)
        return E_INVALIDARG;
    LONG value = item ? STATE_SYSTEM_FOCUSABLE : 0;
    if (item && item->focused)
        value |= STATE_SYSTEM_FOCUSED;
    if (item && item->checked)
        value |= STATE_SYSTEM_CHECKED;
    if (item && item->sensitive)
        value |= STATE_SYSTEM_PROTECTED;
    if (!provider(self)->access->window)
        value |= STATE_SYSTEM_UNAVAILABLE;
    return integer_result(out, value);
}

static HRESULT STDMETHODCALLTYPE no_help_topic(IAccessible *self, BSTR *out, VARIANT child,
                                               LONG *topic)
{
    (void)topic;
    return no_string(self, child, out);
}

static HRESULT STDMETHODCALLTYPE get_focus(IAccessible *self, VARIANT *out)
{
    workspace_accessible *object = provider(self);
    for (int i = 0; i < object->count; ++i)
        if (object->controls[i].focused)
            return integer_result(out, i + 1);
    return integer_result(out, CHILDID_SELF);
}

static HRESULT STDMETHODCALLTYPE get_selection(IAccessible *self, VARIANT *out)
{
    (void)self;
    if (!out)
        return E_POINTER;
    VariantInit(out);
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE default_action(IAccessible *self, VARIANT child, BSTR *out)
{
    const ph_workspace_control *item = control(self, child);
    if (item)
        return string_result(out, item->editable ? "Edit" : item->toggle ? "Toggle" : "Press");
    return no_string(self, child, out);
}

static HRESULT invoke(IAccessible *self, VARIANT child, bool focus_only, const char *value)
{
    const ph_workspace_control *item = control(self, child);
    ph_accessibility *access = provider(self)->access;
    if (!item)
        return E_INVALIDARG;
    if (!access->window || GetWindowThreadProcessId(access->window, NULL) != GetCurrentThreadId())
        return E_FAIL;
    if (!ph_workspace_accessible_invoke(access->workspace, item, focus_only, value))
        return S_FALSE; /* The visible control has changed since the client read it. */
    access->run_action(access->context);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE select_child(IAccessible *self, LONG flags, VARIANT child)
{
    return flags == SELFLAG_TAKEFOCUS ? invoke(self, child, true, NULL) : E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE location(IAccessible *self, LONG *left, LONG *top, LONG *width,
                                          LONG *height, VARIANT child)
{
    if (!left || !top || !width || !height)
        return E_POINTER;
    ph_accessibility *access = provider(self)->access;
    HWND window = access->window;
    const ph_workspace_control *item = control(self, child);
    RECT client = {0};
    if (!window || !GetClientRect(window, &client))
        return E_FAIL;
    POINT first = {0, 0}, last = {client.right, client.bottom};
    if (item) {
        int logical_w, logical_h;
        SDL_GetWindowSize(access->sdl_window, &logical_w, &logical_h);
        if (logical_w <= 0 || logical_h <= 0)
            return E_FAIL;
        SDL_Rect box = item->bounds;
        first = (POINT){MulDiv(box.x, client.right, logical_w),
                        MulDiv(box.y, client.bottom, logical_h)};
        last = (POINT){MulDiv(box.x + box.w, client.right, logical_w),
                       MulDiv(box.y + box.h, client.bottom, logical_h)};
    }
    if (child_id(self, child) < 0 || !ClientToScreen(window, &first) ||
        !ClientToScreen(window, &last))
        return E_INVALIDARG;
    *left = first.x;
    *top = first.y;
    *width = last.x - first.x;
    *height = last.y - first.y;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE navigate(IAccessible *self, LONG direction, VARIANT child,
                                          VARIANT *out)
{
    if (!out)
        return E_POINTER;
    VariantInit(out);
    int count = provider(self)->count, id = child_id(self, child);
    if (id < 0)
        return E_INVALIDARG;
    if (direction == NAVDIR_FIRSTCHILD && id == CHILDID_SELF)
        id = 1;
    else if (direction == NAVDIR_LASTCHILD && id == CHILDID_SELF)
        id = count;
    else if (direction == NAVDIR_NEXT && id > 0)
        id++;
    else if (direction == NAVDIR_PREVIOUS && id > 0)
        id--;
    else
        return S_FALSE;
    return id > 0 && id <= count ? integer_result(out, id) : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE hit_test(IAccessible *self, LONG x, LONG y, VARIANT *out)
{
    if (!out)
        return E_POINTER;
    VariantInit(out);
    workspace_accessible *object = provider(self);
    HWND window = object->access->window;
    POINT point = {x, y};
    RECT bounds = {0};
    if (!window || !ScreenToClient(window, &point) || !GetClientRect(window, &bounds) ||
        !PtInRect(&bounds, point))
        return S_FALSE;
    if (!bounds.right || !bounds.bottom)
        return S_FALSE;
    int logical_w, logical_h;
    SDL_GetWindowSize(object->access->sdl_window, &logical_w, &logical_h);
    SDL_Point p = {MulDiv(point.x, logical_w, bounds.right),
                   MulDiv(point.y, logical_h, bounds.bottom)};
    for (int i = object->count - 1; i >= 0; --i)
        if (SDL_PointInRect(&p, &object->controls[i].bounds))
            return integer_result(out, i + 1);
    return integer_result(out, CHILDID_SELF);
}

static HRESULT STDMETHODCALLTYPE do_action(IAccessible *self, VARIANT child)
{
    return invoke(self, child, false, NULL);
}

static HRESULT STDMETHODCALLTYPE put_name(IAccessible *self, VARIANT child, BSTR value)
{
    (void)self;
    (void)child;
    (void)value;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE put_value(IAccessible *self, VARIANT child, BSTR value)
{
    const ph_workspace_control *item = control(self, child);
    char utf8[256];
    if (!item || !item->editable || item->sensitive || !value ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, utf8, sizeof utf8, NULL,
                            NULL))
        return E_INVALIDARG;
    return invoke(self, child, true, utf8);
}

static const IAccessibleVtbl methods = {
    .QueryInterface = query,
    .AddRef = add_ref,
    .Release = release,
    .GetTypeInfoCount = type_count,
    .GetTypeInfo = type_info,
    .GetIDsOfNames = ids_of_names,
    .Invoke = dispatch,
    .get_accParent = get_parent,
    .get_accChildCount = get_count,
    .get_accChild = get_child,
    .get_accName = get_name,
    .get_accValue = get_value,
    .get_accDescription = no_string,
    .get_accRole = get_role,
    .get_accState = get_state,
    .get_accHelp = no_string,
    .get_accHelpTopic = no_help_topic,
    .get_accKeyboardShortcut = no_string,
    .get_accFocus = get_focus,
    .get_accSelection = get_selection,
    .get_accDefaultAction = default_action,
    .accSelect = select_child,
    .accLocation = location,
    .accNavigate = navigate,
    .accHitTest = hit_test,
    .accDoDefaultAction = do_action,
    .put_accName = put_name,
    .put_accValue = put_value,
};

static workspace_accessible *snapshot(ph_accessibility *access)
{
    size_t count = (size_t)access->count;
    workspace_accessible *object = calloc(1, sizeof *object + count * sizeof object->controls[0]);
    if (!object)
        return NULL;
    object->iface.lpVtbl = &methods;
    object->references = 1;
    object->access = access;
    object->count = access->count;
    memcpy(object->controls, access->snapshot, count * sizeof object->controls[0]);
    InterlockedIncrement(&access->references);
    ITypeLib *library = NULL;
    if (SUCCEEDED(LoadRegTypeLib(&LIBID_Accessibility, 1, 1, LOCALE_SYSTEM_DEFAULT, &library))) {
        ITypeLib_GetTypeInfoOfGuid(library, &IID_IAccessible, &object->type_info);
        ITypeLib_Release(library);
    }
    return object;
}

static LRESULT CALLBACK window_accessibility(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                             UINT_PTR id, DWORD_PTR context)
{
    ph_accessibility *access = (ph_accessibility *)context;
    if (message == WM_GETOBJECT && (LONG)lp == OBJID_CLIENT && access->count > 0) {
        workspace_accessible *object = snapshot(access);
        if (object) {
            LRESULT result = LresultFromObject(&IID_IAccessible, wp, (IUnknown *)&object->iface);
            release(&object->iface);
            return result;
        }
    }
    if (message == WM_NCDESTROY) {
        access->window = NULL;
        RemoveWindowSubclass(window, window_accessibility, id);
    }
    return DefSubclassProc(window, message, wp, lp);
}

ph_accessibility *ph_accessibility_attach(SDL_Window *window, ph_accessibility_action run_action,
                                          void *context)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS)
        return NULL;
    ph_accessibility *access = calloc(1, sizeof *access);
    if (!access)
        return NULL;
    access->references = 1;
    access->window = info.info.win.window;
    access->sdl_window = window;
    access->run_action = run_action;
    access->context = context;
    access->com_initialized = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));
    access->snapshot = calloc(PH_WORKSPACE_MAX_CONTROLS, sizeof access->snapshot[0]);
    if (!access->snapshot ||
        !SetWindowSubclass(access->window, window_accessibility, 1, (DWORD_PTR)access)) {
        ph_accessibility_detach(access);
        return NULL;
    }
    return access;
}

void ph_accessibility_update(ph_accessibility *access, ph_workspace *workspace)
{
    if (!access || !access->window)
        return;
    int count = ph_workspace_accessible_count(workspace);
    int focused = 0;
    bool changed = count != access->count;
    for (int i = 0; i < count; ++i) {
        ph_workspace_control item;
        if (!ph_workspace_accessible_control(workspace, i, &item))
            break;
        ph_workspace_control *previous = &access->snapshot[i];
        changed = changed || previous->action != item.action || previous->item != item.item ||
                  previous->revision != item.revision || strcmp(previous->name, item.name);
        if (item.focused)
            focused = i + 1;
        *previous = item;
    }
    access->workspace = workspace;
    access->count = count;
    if (changed)
        NotifyWinEvent(EVENT_OBJECT_REORDER, access->window, OBJID_CLIENT, CHILDID_SELF);
    if (focused && (changed || focused != access->focused))
        NotifyWinEvent(EVENT_OBJECT_FOCUS, access->window, OBJID_CLIENT, focused);
    access->focused = focused;
}

void ph_accessibility_detach(ph_accessibility *access)
{
    if (!access)
        return;
    if (access->window)
        RemoveWindowSubclass(access->window, window_accessibility, 1);
    access->window = NULL;
    access->workspace = NULL;
    if (access->com_initialized)
        CoUninitialize();
    release_access(access);
}
