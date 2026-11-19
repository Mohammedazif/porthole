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
static char uri[1024];
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

static void set_uri(const char *path)
{
    size_t i, o = 0;
    const char *prefix = "file://";

    if (!path)
        return;
    for (i = 0; prefix[i] && o + 1 < sizeof uri; i++)
        uri[o++] = prefix[i];
    for (i = 0; path[i] && o + 4 < sizeof uri; i++) {
        unsigned char c = (unsigned char)path[i];

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '/' || c == '.' || c == '_' || c == '-') {
            uri[o++] = (char)c;
        } else {
            snprintf(uri + o, sizeof uri - o, "%%%02X", c);
            o += 3;
        }
    }
    if (o + 2 < sizeof uri) {
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
