#include "files/xdnd.h"

#include "core/log.h"

#include <stdio.h>
#include <string.h>

#if defined(PH_HAVE_X11)

#include <SDL_syswm.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

static Display *display;
static Window source;
static Window target;
static Atom atom_aware, atom_enter, atom_position, atom_status, atom_leave, atom_drop;
static Atom atom_finished, atom_selection, atom_action, atom_uri, atom_types, atom_targets;
static char uri[1 << 17];
static unsigned long x_time;
static bool active;
static bool settling;
static bool accepted;
static int xdnd_error_flag;
static Atom atom_utf8, atom_wm_name;
static SDL_Window *skip_sdl;
static Window skip_xid;

static int xdnd_error(Display *d, XErrorEvent *event)
{
    (void)d;
    (void)event;
    xdnd_error_flag = 1;
    return 0;
}

static void send_client(Window window, Atom type, long a, long b, long c, long d, long e)
{
    XEvent ev;
    XErrorHandler old;

    if (!display || !window)
        return;
    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.display = display;
    ev.xclient.window = window;
    ev.xclient.message_type = type;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = a;
    ev.xclient.data.l[1] = b;
    ev.xclient.data.l[2] = c;
    ev.xclient.data.l[3] = d;
    ev.xclient.data.l[4] = e;
    xdnd_error_flag = 0;
    old = XSetErrorHandler(xdnd_error);
    XSendEvent(display, window, False, NoEventMask, &ev);
    XFlush(display);
    XSetErrorHandler(old);
}

/* path may hold several files, one per line; text/uri-list carries each on its own
 * CRLF-ended line, so a whole batch from the phone drops in one go. */
static void set_uri(const char *path)
{
    size_t i, o = 0;
    const char *prefix = "file://";
    bool line_start = true;

    if (!path)
        return;
    for (i = 0; path[i] && o + 16 < sizeof uri; i++) {
        unsigned char c = (unsigned char)path[i];

        if (c == '\n') {
            if (!line_start) {
                uri[o++] = '\r';
                uri[o++] = '\n';
            }
            line_start = true;
            continue;
        }
        if (line_start) {
            for (size_t k = 0; prefix[k]; k++)
                uri[o++] = prefix[k];
            line_start = false;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '/' || c == '.' || c == '_' || c == '-') {
            uri[o++] = (char)c;
        } else {
            snprintf(uri + o, sizeof uri - o, "%%%02X", c);
            o += 3;
        }
    }
    if (!line_start && o + 2 < sizeof uri) {
        uri[o++] = '\r';
        uri[o++] = '\n';
    }
    uri[o] = 0;
}

static Window ignored(void)
{
    SDL_SysWMinfo info;

    if (skip_xid || !skip_sdl || !display)
        return skip_xid;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(skip_sdl, &info) && info.subsystem == SDL_SYSWM_X11)
        skip_xid = info.info.x11.window;
    return skip_xid;
}

static void copy_title(const char *text, int len, char *buf, size_t n)
{
    size_t o = 0;
    int i;

    buf[0] = 0;
    if (!text || n < 2)
        return;
    if (len < 0)
        len = (int)strlen(text);
    for (i = 0; i < len && o + 1 < n && o < 72; i++) {
        unsigned char c = (unsigned char)text[i];

        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        if (c < 32 || c > 126)
            continue;
        if (c == ' ' && (o == 0 || buf[o - 1] == ' '))
            continue;
        buf[o++] = (char)c;
    }
    while (o > 0 && buf[o - 1] == ' ')
        o--;
    buf[o] = 0;
}

static bool read_title(Window w, char *buf, size_t n)
{
    Atom type = None;
    int format = 0;
    unsigned long count = 0, left = 0;
    unsigned char *data = NULL;
    XErrorHandler old;
    bool ok = false;

    buf[0] = 0;
    if (!display || !w || !atom_utf8 || !atom_wm_name)
        return false;
    xdnd_error_flag = 0;
    old = XSetErrorHandler(xdnd_error);
    if (XGetWindowProperty(display, w, atom_wm_name, 0, 128, False, atom_utf8, &type, &format,
                           &count, &left, &data)
            == Success
        && !xdnd_error_flag && data && type == atom_utf8 && format == 8 && count > 0) {
        copy_title((const char *)data, (int)count, buf, n);
        ok = buf[0] != 0;
    }
    if (data)
        XFree(data);
    if (!ok) {
        XTextProperty prop;
        char **list = NULL;
        int nlist = 0;

        memset(&prop, 0, sizeof prop);
        xdnd_error_flag = 0;
        if (XGetWMName(display, w, &prop) && !xdnd_error_flag && prop.value) {
            if (XmbTextPropertyToTextList(display, &prop, &list, &nlist) == Success && nlist > 0
                && list && list[0]) {
                copy_title(list[0], -1, buf, n);
                ok = buf[0] != 0;
            }
            if (list)
                XFreeStringList(list);
            XFree(prop.value);
        }
    }
    XSetErrorHandler(old);
    return ok;
}

static bool ensure_display(SDL_Window *window)
{
    SDL_SysWMinfo info;

    if ((!display || !source) && window) {
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_X11)
            return false;
        display = info.info.x11.display;
        source = info.info.x11.window;
    }
    if (!display || !source)
        return false;
    if (!atom_aware)
        atom_aware = XInternAtom(display, "XdndAware", False);
    if (!atom_utf8)
        atom_utf8 = XInternAtom(display, "UTF8_STRING", False);
    if (!atom_wm_name)
        atom_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    return atom_aware != None;
}

static Window find_aware(int root_x, int root_y)
{
    Window root = DefaultRootWindow(display);
    Window found = 0;
    Window win = root;
    Window skip = ignored();
    int guard;
    bool lowered = false;

    for (guard = 0; guard < 32; guard++) {
        Window child = 0;
        int wx, wy;
        XErrorHandler old = XSetErrorHandler(xdnd_error);

        xdnd_error_flag = 0;
        if (!XTranslateCoordinates(display, root, win, root_x, root_y, &wx, &wy, &child)
            || xdnd_error_flag) {
            XSetErrorHandler(old);
            break;
        }
        XSetErrorHandler(old);
        if (!child || child == win)
            break;
        if (skip && child == skip) {
            if (lowered)
                break;
            /* The photo follows the cursor. Look through it once, then put it back. */
            XLowerWindow(display, skip);
            XFlush(display);
            lowered = true;
            continue;
        }
        win = child;
    }
    if (lowered) {
        XRaiseWindow(display, skip);
        XFlush(display);
    }
    for (guard = 0; win && win != root && guard < 32; guard++) {
        Atom type = None;
        int format = 0;
        unsigned long count = 0, left = 0;
        unsigned char *data = NULL;
        Window parent = 0, *kids = NULL;
        unsigned int nkids = 0;
        XErrorHandler old = XSetErrorHandler(xdnd_error);

        xdnd_error_flag = 0;
        if (XGetWindowProperty(display, win, atom_aware, 0, 1, False, AnyPropertyType, &type,
                               &format, &count, &left, &data) == Success
            && data && type != None)
            found = win;
        if (data)
            XFree(data);
        if (found) {
            XSetErrorHandler(old);
            break;
        }
        if (!XQueryTree(display, win, &root, &parent, &kids, &nkids) || xdnd_error_flag)
            parent = 0;
        if (kids)
            XFree(kids);
        XSetErrorHandler(old);
        win = parent;
    }
    if (found == source)
        return 0;
    return found;
}

static void answer_selection(const XSelectionRequestEvent *req)
{
    XEvent reply;
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);

    memset(&reply, 0, sizeof reply);
    reply.xselection.type = SelectionNotify;
    reply.xselection.display = display;
    reply.xselection.requestor = req->requestor;
    reply.xselection.selection = req->selection;
    reply.xselection.target = req->target;
    reply.xselection.property = None;
    reply.xselection.time = req->time;
    if (req->property != None && (req->target == atom_targets || req->target == atom_uri
                                  || req->target == utf8 || req->target == XA_STRING)) {
        if (req->target == atom_targets) {
            Atom list[3] = {atom_targets, atom_uri, utf8};
            XChangeProperty(display, req->requestor, req->property, XA_ATOM, 32, PropModeReplace,
                            (unsigned char *)list, 3);
        } else {
            XChangeProperty(display, req->requestor, req->property,
                            req->target == atom_uri ? atom_uri : utf8, 8, PropModeReplace,
                            (unsigned char *)uri, (int)strlen(uri));
        }
        reply.xselection.property = req->property;
    }
    XSendEvent(display, req->requestor, False, 0, &reply);
    XFlush(display);
}

bool ph_xdnd_begin(SDL_Window *window, const char *path)
{
    SDL_SysWMinfo info;

    if (!window || !path || !path[0])
        return false;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_X11)
        return false;
    display = info.info.x11.display;
    source = info.info.x11.window;
    if (!display || !source)
        return false;
    atom_aware = XInternAtom(display, "XdndAware", False);
    atom_enter = XInternAtom(display, "XdndEnter", False);
    atom_position = XInternAtom(display, "XdndPosition", False);
    atom_status = XInternAtom(display, "XdndStatus", False);
    atom_leave = XInternAtom(display, "XdndLeave", False);
    atom_drop = XInternAtom(display, "XdndDrop", False);
    atom_finished = XInternAtom(display, "XdndFinished", False);
    atom_selection = XInternAtom(display, "XdndSelection", False);
    atom_action = XInternAtom(display, "XdndActionCopy", False);
    atom_uri = XInternAtom(display, "text/uri-list", False);
    atom_types = XInternAtom(display, "XdndTypeList", False);
    atom_targets = XInternAtom(display, "TARGETS", False);
    atom_utf8 = XInternAtom(display, "UTF8_STRING", False);
    atom_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    set_uri(path);
    XChangeProperty(display, source, atom_types, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&atom_uri, 1);
    XSetSelectionOwner(display, atom_selection, source, CurrentTime);
    XFlush(display);
    target = 0;
    accepted = false;
    settling = false;
    active = true;
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
    return true;
}

void ph_xdnd_set_path(const char *path)
{
    if (active && path)
        set_uri(path);
}

void ph_xdnd_motion(int root_x, int root_y)
{
    Window next;
    long packed;

    if (!active || settling || !display)
        return;
    next = find_aware(root_x, root_y);
    if (next != target) {
        if (target)
            send_client(target, atom_leave, (long)source, 0, 0, 0, 0);
        target = next;
        accepted = false;
        if (target)
            send_client(target, atom_enter, (long)source, (long)5 << 24, (long)atom_uri, 0, 0);
    }
    if (!target)
        return;
    packed = ((long)(root_x & 0xffff) << 16) | (long)(root_y & 0xffff);
    send_client(target, atom_position, (long)source, 0, packed, (long)x_time, (long)atom_action);
}

bool ph_xdnd_drop(void)
{
    bool placed = false;

    if (!active)
        return false;
    if (target && accepted) {
        send_client(target, atom_drop, (long)source, 0, (long)x_time, 0, 0);
        placed = true;
    } else if (target) {
        send_client(target, atom_leave, (long)source, 0, 0, 0, 0);
    }
    settling = true;
    if (!placed) {
        XSetSelectionOwner(display, atom_selection, None, CurrentTime);
        active = false;
        settling = false;
        target = 0;
    }
    return placed;
}

void ph_xdnd_cancel(void)
{
    if (!active)
        return;
    if (target)
        send_client(target, atom_leave, (long)source, 0, 0, 0, 0);
    if (display)
        XSetSelectionOwner(display, atom_selection, None, CurrentTime);
    active = false;
    settling = false;
    target = 0;
    accepted = false;
}

bool ph_xdnd_busy(void)
{
    return active;
}

bool ph_xdnd_place(SDL_Window *window, int root_x, int root_y, char *buf, size_t n)
{
    Window hit;

    if (!buf || n < 16)
        return false;
    buf[0] = 0;
    if (!ensure_display(window))
        return false;
    hit = active ? target : find_aware(root_x, root_y);
    if (!hit)
        return false;
    if (!read_title(hit, buf, n) || !buf[0])
        snprintf(buf, n, "Desktop");
    return true;
}

void ph_xdnd_skip(SDL_Window *window)
{
    skip_sdl = window;
    skip_xid = 0;
}

void ph_xdnd_syswm(const void *msg_ptr)
{
    const SDL_SysWMmsg *msg = msg_ptr;
    const XEvent *ev;

    if (!msg || msg->subsystem != SDL_SYSWM_X11)
        return;
    ev = &msg->msg.x11.event;
    if (ev->type == MotionNotify)
        x_time = ev->xmotion.time;
    else if (ev->type == ButtonPress || ev->type == ButtonRelease)
        x_time = ev->xbutton.time;
    if (!active)
        return;
    if (ev->type == SelectionRequest && ev->xselectionrequest.selection == atom_selection) {
        answer_selection(&ev->xselectionrequest);
        return;
    }
    if (ev->type != ClientMessage || ev->xclient.format != 32)
        return;
    if (ev->xclient.message_type == atom_status) {
        accepted = (ev->xclient.data.l[1] & 1) != 0;
        return;
    }
    if (ev->xclient.message_type == atom_finished) {
        XSetSelectionOwner(display, atom_selection, None, CurrentTime);
        active = false;
        settling = false;
        target = 0;
    }
}

#elif defined(_WIN32)

/* Windows: a real shell drag, so the original can be dropped into any app
 * that takes files (File Explorer, a terminal, a browser, chat apps). It
 * starts once the file is here and the pointer has left Porthole with the
 * button still held; Windows then runs the drag until the button goes up. */
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wchar.h>
#include <SDL_syswm.h>

static const GUID ph_bhid_data_object = {0xb8c0bd9f, 0xed24, 0x455c, {0x83, 0xe6, 0xd5, 0x39, 0x0c, 0x4f, 0xe8, 0xc4}};
static const IID ph_iid_data_object = {0x0000010e, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

static char drag_paths[1 << 15];
static bool pending, ran, dropped;
static HWND owner, skip_window;

static HWND window_handle(SDL_Window *window)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!window || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS)
        return NULL;
    return info.info.win.window;
}

static bool wide(const char *utf8, wchar_t *out, int cap)
{
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, cap) > 0;
}

/* The files, one per line, as the shell's own data object (what File
 * Explorer itself offers when a file is dragged). */
static IDataObject *shell_data(void)
{
    PIDLIST_ABSOLUTE items[64];
    UINT count = 0;
    char copy[sizeof drag_paths];
    snprintf(copy, sizeof copy, "%s", drag_paths);
    for (char *line = copy, *next; line && *line && count < 64; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        wchar_t path[MAX_PATH * 4];
        if (wide(line, path, (int)(sizeof path / sizeof path[0])) &&
            SUCCEEDED(SHParseDisplayName(path, NULL, &items[count], 0, NULL)))
            count++;
    }
    IDataObject *data = NULL;
    IShellItemArray *array = NULL;
    if (count && SUCCEEDED(SHCreateShellItemArrayFromIDLists(count, (PCIDLIST_ABSOLUTE_ARRAY)items, &array))) {
        if (FAILED(array->lpVtbl->BindToHandler(array, NULL, &ph_bhid_data_object, &ph_iid_data_object,
                                                (void **)&data)))
            data = NULL;
        array->lpVtbl->Release(array);
    }
    for (UINT i = 0; i < count; i++)
        CoTaskMemFree(items[i]);
    return data;
}

static bool over_porthole(int x, int y)
{
    POINT point = {x, y};
    HWND under = WindowFromPoint(point);
    HWND root = under ? GetAncestor(under, GA_ROOT) : NULL;
    return root && (root == owner || root == skip_window);
}

static void run_drag(void)
{
    ran = true;
    HRESULT init = OleInitialize(NULL);
    IDataObject *data = shell_data();
    if (!data) {
        PH_LOGW("photo pull: Windows could not offer the file for dragging");
    } else {
        DWORD effect = DROPEFFECT_NONE;
        SDL_CaptureMouse(SDL_FALSE);
        ReleaseCapture();
        HRESULT hr = SHDoDragDrop(owner, data, NULL, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
        dropped = hr == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE;
        PH_LOGI("photo pull: dragged out of Porthole, %s", dropped ? "dropped into another app" : "not dropped");
        data->lpVtbl->Release(data);
    }
    if (SUCCEEDED(init))
        OleUninitialize();
}

bool ph_xdnd_begin(SDL_Window *window, const char *path)
{
    if (!path || !path[0] || strlen(path) >= sizeof drag_paths)
        return false;
    owner = window_handle(window);
    if (!owner)
        return false;
    snprintf(drag_paths, sizeof drag_paths, "%s", path);
    pending = true;
    ran = dropped = false;
    return true;
}

void ph_xdnd_set_path(const char *path)
{
    if (pending && path && strlen(path) < sizeof drag_paths)
        snprintf(drag_paths, sizeof drag_paths, "%s", path);
}

void ph_xdnd_motion(int root_x, int root_y)
{
    /* Only with the button still down: after it went up there is nothing
     * left to drag, and the file lands where it was let go. */
    if (!pending || ran || !(GetAsyncKeyState(VK_LBUTTON) & 0x8000) || over_porthole(root_x, root_y))
        return;
    run_drag();
}

bool ph_xdnd_drop(void)
{
    bool result = pending && ran && dropped;
    pending = ran = dropped = false;
    drag_paths[0] = 0;
    return result;
}

void ph_xdnd_cancel(void)
{
    pending = ran = dropped = false;
    drag_paths[0] = 0;
}

bool ph_xdnd_busy(void)
{
    return pending;
}

void ph_xdnd_syswm(const void *msg)
{
    (void)msg;
}

bool ph_xdnd_place(SDL_Window *window, int root_x, int root_y, char *buf, size_t n)
{
    if (!buf || !n)
        return false;
    buf[0] = 0;
    if (!owner)
        owner = window_handle(window);
    POINT point = {root_x, root_y};
    HWND under = WindowFromPoint(point);
    HWND root = under ? GetAncestor(under, GA_ROOT) : NULL;
    if (!root || root == owner || root == skip_window)
        return false;
    wchar_t title[256] = L"", kind[64] = L"";
    GetClassNameW(root, kind, (int)(sizeof kind / sizeof kind[0]));
    if (!wcscmp(kind, L"Progman") || !wcscmp(kind, L"WorkerW")) {
        snprintf(buf, n, "Desktop");
        return true;
    }
    if (GetWindowTextW(root, title, (int)(sizeof title / sizeof title[0])) <= 0 ||
        !WideCharToMultiByte(CP_UTF8, 0, title, -1, buf, (int)n, NULL, NULL)) {
        snprintf(buf, n, "this window");
        return true;
    }
    buf[n - 1] = 0;
    return true;
}

void ph_xdnd_skip(SDL_Window *window)
{
    skip_window = window_handle(window);
}

bool ph_xdnd_copy_files(const char *paths)
{
    if (!paths || !paths[0])
        return false;
    /* CF_HDROP: a DROPFILES header, then each path in UTF-16, then an extra 0. */
    size_t chars = 0;
    char copy[1 << 15];
    snprintf(copy, sizeof copy, "%s", paths);
    wchar_t list[1 << 15];
    for (char *line = copy, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        int got = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line, -1, list + chars,
                                      (int)(sizeof list / sizeof list[0] - chars - 1));
        if (got <= 0)
            return false;
        chars += (size_t)got;
    }
    list[chars++] = 0;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(wchar_t));
    if (!memory)
        return false;
    DROPFILES *drop = GlobalLock(memory);
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    memcpy((char *)drop + sizeof(DROPFILES), list, chars * sizeof(wchar_t));
    GlobalUnlock(memory);
    HGLOBAL effect = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (effect) {
        *(DWORD *)GlobalLock(effect) = DROPEFFECT_COPY;
        GlobalUnlock(effect);
    }
    if (!OpenClipboard(owner)) {
        GlobalFree(memory);
        if (effect)
            GlobalFree(effect);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_HDROP, memory) != NULL;
    if (!ok)
        GlobalFree(memory);
    UINT preferred = RegisterClipboardFormatW(L"Preferred DropEffect");
    if (effect && (!preferred || !SetClipboardData(preferred, effect)))
        GlobalFree(effect);
    CloseClipboard();
    return ok;
}

#else

bool ph_xdnd_begin(SDL_Window *window, const char *path)
{
    (void)window;
    (void)path;
    return false;
}

void ph_xdnd_set_path(const char *path)
{
    (void)path;
}

void ph_xdnd_motion(int root_x, int root_y)
{
    (void)root_x;
    (void)root_y;
}

bool ph_xdnd_drop(void)
{
    return false;
}

void ph_xdnd_cancel(void)
{
}

bool ph_xdnd_busy(void)
{
    return false;
}

void ph_xdnd_syswm(const void *msg)
{
    (void)msg;
}

bool ph_xdnd_place(SDL_Window *window, int root_x, int root_y, char *buf, size_t n)
{
    (void)window;
    (void)root_x;
    (void)root_y;
    if (buf && n)
        buf[0] = 0;
    return false;
}

void ph_xdnd_skip(SDL_Window *window)
{
    (void)window;
}

#endif

#if !defined(_WIN32)
bool ph_xdnd_copy_files(const char *paths)
{
    (void)paths;
    return false;
}
#endif
