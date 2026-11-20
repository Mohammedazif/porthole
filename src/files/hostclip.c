#include "files/hostclip.h"
#include "files/winclip.h"

#include "core/log.h"
#include "files/clip.h"

#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#if defined(PH_HAVE_X11)

#include <errno.h>
#include <poll.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#undef None

static Display *dpy;
static Window win;
static Atom atom_clipboard, atom_png, atom_targets, atom_incr, atom_prop;
static uint8_t *owned_png, *queued_png;
static size_t owned_len, queued_len;
static bool we_own;
static bool incr_on;
static Window incr_window;
static Atom incr_prop;
static size_t incr_off;
static Window last_owner;
static Uint64 last_fetch;
static int xerr;

static int on_xerr(Display *display, XErrorEvent *event)
{
    (void)display;
    xerr = event->error_code;
    return 0;
}

static void note_xerr(void)
{
    if (xerr)
        PH_LOGD("clipboard x11 error %d", xerr);
    xerr = 0;
}

static bool is_png(const void *data, size_t len)
{
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

    return data && len >= sizeof sig && memcmp(data, sig, sizeof sig) == 0;
}

static void reply_selection(const XSelectionRequestEvent *req, Atom property)
{
    XEvent ev;

    memset(&ev, 0, sizeof ev);
    ev.xselection.type = SelectionNotify;
    ev.xselection.display = req->display;
    ev.xselection.requestor = req->requestor;
    ev.xselection.selection = req->selection;
    ev.xselection.target = req->target;
    ev.xselection.property = property;
    ev.xselection.time = req->time;
    XSendEvent(dpy, req->requestor, False, 0, &ev);
    XFlush(dpy);
}

static size_t max_prop_bytes(void)
{
    unsigned long units = XExtendedMaxRequestSize(dpy);

    if (!units)
        units = XMaxRequestSize(dpy);
    if (units < 64)
        units = 64;
    return (units - 64) * 4;
}

static void incr_chunk(void)
{
    size_t left = owned_len > incr_off ? owned_len - incr_off : 0;
    size_t n = left > 65536 ? 65536 : left;
    const unsigned char *bytes = n ? owned_png + incr_off : (const unsigned char *)"";

    XChangeProperty(dpy, incr_window, incr_prop, atom_png, 8, PropModeReplace, bytes, (int)n);
    incr_off += n;
    if (n == 0) {
        incr_on = false;
        if (queued_png) {
            free(owned_png);
            owned_png = queued_png;
            owned_len = queued_len;
            queued_png = NULL;
            queued_len = 0;
        }
    }
    XFlush(dpy);
}

static void serve_request(const XSelectionRequestEvent *req)
{
    Atom prop = req->property ? req->property : req->target;

    if (req->selection != atom_clipboard || !we_own || !owned_png) {
        reply_selection(req, 0);
        return;
    }
    if (req->target == atom_targets) {
        long list[2];

        list[0] = (long)atom_targets;
        list[1] = (long)atom_png;
        XChangeProperty(dpy, req->requestor, prop, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)list, 2);
        reply_selection(req, prop);
        return;
    }
    if (req->target != atom_png) {
        reply_selection(req, 0);
        return;
    }
    if (owned_len <= max_prop_bytes()) {
        XChangeProperty(dpy, req->requestor, prop, atom_png, 8, PropModeReplace, owned_png,
                        (int)owned_len);
        reply_selection(req, prop);
        return;
    }
    {
        long size = (long)owned_len;

        XSelectInput(dpy, req->requestor, PropertyChangeMask);
        XChangeProperty(dpy, req->requestor, prop, atom_incr, 32, PropModeReplace,
                        (unsigned char *)&size, 1);
        incr_on = true;
        incr_window = req->requestor;
        incr_prop = prop;
        incr_off = 0;
        reply_selection(req, prop);
    }
}

static void handle_event(XEvent *ev)
{
    if (ev->type == SelectionClear && ev->xselectionclear.window == win
        && ev->xselectionclear.selection == atom_clipboard) {
        we_own = false;
        incr_on = false;
        return;
    }
    if (ev->type == PropertyNotify && incr_on && ev->xproperty.window == incr_window
        && ev->xproperty.atom == incr_prop && ev->xproperty.state == PropertyDelete) {
        incr_chunk();
        return;
    }
    if (ev->type == SelectionRequest)
        serve_request(&ev->xselectionrequest);
}

static bool read_prop(Atom prop, uint8_t **out, size_t *len, Atom *type_out)
{
    Atom type = 0;
    int format = 0;
    unsigned long nitems = 0, left = 0;
    unsigned char *data = NULL;
    size_t nbytes;

    *out = NULL;
    *len = 0;
    if (XGetWindowProperty(dpy, win, prop, 0, (long)(PH_CLIP_IMAGE_MAX / 4), False, AnyPropertyType,
                           &type, &format, &nitems, &left, &data) != Success
        || left)
        goto fail;
    if (!data) {
        *out = malloc(1);
        if (!*out)
            return false;
        *len = 0;
        *type_out = type;
        return true;
    }
    nbytes = format == 8 ? nitems : format == 16 ? nitems * 2 : nitems * 4;
    *out = malloc(nbytes ? nbytes : 1);
    if (!*out)
        goto fail;
    if (nbytes)
        memcpy(*out, data, nbytes);
    *len = nbytes;
    *type_out = type;
    XFree(data);
    return true;
fail:
    if (data)
        XFree(data);
    free(*out);
    *out = NULL;
    return false;
}

static bool wait_event(int timeout_ms, int type, Atom atom, XEvent *found)
{
    int waited = 0;

    while (waited <= timeout_ms) {
        while (XPending(dpy)) {
            XEvent ev;

            XNextEvent(dpy, &ev);
            if (ev.type == type && type == SelectionNotify && ev.xselection.requestor == win
                && ev.xselection.selection == atom_clipboard) {
                *found = ev;
                return true;
            }
            if (ev.type == type && type == PropertyNotify && ev.xproperty.window == win
                && ev.xproperty.atom == atom && ev.xproperty.state == PropertyNewValue) {
                *found = ev;
                return true;
            }
            handle_event(&ev);
        }
        {
            struct pollfd p = {.fd = ConnectionNumber(dpy), .events = POLLIN};
            int pr = poll(&p, 1, 20);

            if (pr < 0 && errno != EINTR)
                return false;
        }
        waited += 20;
    }
    return false;
}

static bool read_incr(Atom prop, uint8_t **out, size_t *len)
{
    uint8_t *buf = NULL;
    size_t used = 0;
    int spins = 0;

    XDeleteProperty(dpy, win, prop);
    XFlush(dpy);
    while (spins < 200) {
        XEvent ev;
        uint8_t *chunk = NULL;
        size_t n = 0;
        Atom type = 0;

        if (!wait_event(200, PropertyNotify, prop, &ev))
            break;
        if (!read_prop(prop, &chunk, &n, &type)) {
            free(buf);
            return false;
        }
        XDeleteProperty(dpy, win, prop);
        XFlush(dpy);
        if (n == 0) {
            free(chunk);
            break;
        }
        if (used > PH_CLIP_IMAGE_MAX || n > PH_CLIP_IMAGE_MAX - used) {
            free(chunk);
            free(buf);
            return false;
        }
        {
            uint8_t *grown = realloc(buf, used + n);

            if (!grown) {
                free(chunk);
                free(buf);
                return false;
            }
            buf = grown;
        }
        memcpy(buf + used, chunk, n);
        used += n;
        free(chunk);
        spins++;
    }
    if (!is_png(buf, used)) {
        free(buf);
        return false;
    }
    *out = buf;
    *len = used;
    return true;
}

static bool fetch_png(uint8_t **out, size_t *len)
{
    XEvent ev;
    Window owner;
    uint8_t *data = NULL;
    size_t n = 0;
    Atom type = 0;
    Uint64 now = SDL_GetTicks64();

    *out = NULL;
    owner = XGetSelectionOwner(dpy, atom_clipboard);
    if (!owner || owner == win)
        return false;
    if (owner == last_owner && now - last_fetch < 2000)
        return false;
    last_owner = owner;
    last_fetch = now;
    XConvertSelection(dpy, atom_clipboard, atom_png, atom_prop, win, CurrentTime);
    XFlush(dpy);
    memset(&ev, 0, sizeof ev);
    if (!wait_event(200, SelectionNotify, 0, &ev) || !ev.xselection.property)
        return false;
    if (!read_prop(ev.xselection.property, &data, &n, &type))
        return false;
    if (type == atom_incr) {
        free(data);
        return read_incr(ev.xselection.property, out, len);
    }
    XDeleteProperty(dpy, win, ev.xselection.property);
    XFlush(dpy);
    if (!is_png(data, n) || n > PH_CLIP_IMAGE_MAX) {
        free(data);
        return false;
    }
    *out = data;
    *len = n;
    return true;
}

static void own_png(const void *data, size_t len)
{
    uint8_t *copy = malloc(len);

    if (!copy)
        return;
    memcpy(copy, data, len);
    if (incr_on) {
        free(queued_png);
        queued_png = copy;
        queued_len = len;
        return;
    }
    free(owned_png);
    owned_png = copy;
    owned_len = len;
    XSetSelectionOwner(dpy, atom_clipboard, win, CurrentTime);
    XFlush(dpy);
    we_own = XGetSelectionOwner(dpy, atom_clipboard) == win;
    if (!we_own)
        PH_LOGW("clipboard image was not accepted");
}

static void open_clipboard(void)
{
    int screen;

    XSetErrorHandler(on_xerr);
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        PH_LOGW("clipboard images need a desktop session");
        return;
    }
    screen = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(dpy, win, PropertyChangeMask);
    atom_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    atom_png = XInternAtom(dpy, "image/png", False);
    atom_targets = XInternAtom(dpy, "TARGETS", False);
    atom_incr = XInternAtom(dpy, "INCR", False);
    atom_prop = XInternAtom(dpy, "PORTHOLE_CLIP", False);
    XFlush(dpy);
    note_xerr();
}

static void close_clipboard(void)
{
    if (!dpy)
        return;
    free(owned_png);
    free(queued_png);
    owned_png = queued_png = NULL;
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    dpy = NULL;
}

#else

static bool is_png(const void *data, size_t len)
{
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

    return data && len >= sizeof sig && memcmp(data, sig, sizeof sig) == 0;
}

static void open_clipboard(void) {}
static void close_clipboard(void) {}
static void ph_hostclip_pump_platform(void) {}

#endif

static char seen_kind, applied_kind;
static uint8_t *seen, *applied;
static size_t seen_len, applied_len;

static void remember(char kind, const void *data, size_t len, char *slot_kind, uint8_t **slot,
                     size_t *slot_len)
{
    uint8_t *copy = malloc(len);

    if (!copy)
        return;
    memcpy(copy, data, len);
    free(*slot);
    *slot = copy;
    *slot_len = len;
    *slot_kind = kind;
}

static bool same(char kind, const void *data, size_t len, char slot_kind, const uint8_t *slot,
                 size_t slot_len)
{
    return slot_kind == kind && slot_len == len && slot && memcmp(slot, data, len) == 0;
}

static bool already(char kind, const void *data, size_t len)
{
    if (same(kind, data, len, seen_kind, seen, seen_len))
        return true;
    if (same(kind, data, len, applied_kind, applied, applied_len)) {
        remember(kind, data, len, &seen_kind, &seen, &seen_len);
        return true;
    }
    return false;
}

void ph_hostclip_init(void)
{
    open_clipboard();
}

void ph_hostclip_shutdown(void)
{
    close_clipboard();
    free(seen);
    free(applied);
    seen = applied = NULL;
    seen_len = applied_len = 0;
    seen_kind = applied_kind = 0;
}

void ph_hostclip_pump(void)
{
#if defined(PH_HAVE_X11)
    if (!dpy)
        return;
    while (XPending(dpy)) {
        XEvent ev;

        XNextEvent(dpy, &ev);
        handle_event(&ev);
    }
#else
    ph_hostclip_pump_platform();
#endif
}

bool ph_hostclip_changed(char *kind, void **data, size_t *len)
{
    char *text;
    size_t n;

    if (!kind || !data || !len)
        return false;
    *data = NULL;
    text = SDL_GetClipboardText();
    n = text ? strlen(text) : 0;
    if (text && n > 0 && n <= PH_CLIP_TEXT_MAX && !memchr(text, 0, n) && !already('T', text, n)) {
        *data = malloc(n);
        if (!*data) {
            SDL_free(text);
            return false;
        }
        memcpy(*data, text, n);
        *len = n;
        *kind = 'T';
        remember('T', text, n, &seen_kind, &seen, &seen_len);
        SDL_free(text);
        return true;
    }
    SDL_free(text);
#if defined(PH_HAVE_X11)
    if (dpy) {
        uint8_t *png = NULL;
        size_t png_len = 0;

        if (fetch_png(&png, &png_len) && !already('P', png, png_len)) {
            *data = png;
            *len = png_len;
            *kind = 'P';
            remember('P', png, png_len, &seen_kind, &seen, &seen_len);
            return true;
        }
        free(png);
    }
#elif defined(_WIN32)
    {
        uint8_t *png = NULL;
        size_t png_len = 0;

        if (ph_winclip_read_png(&png, &png_len, PH_CLIP_IMAGE_MAX) && !already('P', png, png_len)) {
            *data = png;
            *len = png_len;
            *kind = 'P';
            remember('P', png, png_len, &seen_kind, &seen, &seen_len);
            return true;
        }
        free(png);
    }
#endif
    return false;
}

void ph_hostclip_apply(char kind, const void *data, size_t len)
{
    if (!data || !len)
        return;
    if (kind == 'T') {
        char *text;

        if (len > PH_CLIP_TEXT_MAX || memchr(data, 0, len))
            return;
        text = malloc(len + 1);
        if (!text)
            return;
        memcpy(text, data, len);
        text[len] = 0;
        if (SDL_SetClipboardText(text) == 0) {
            remember('T', data, len, &seen_kind, &seen, &seen_len);
            remember('T', data, len, &applied_kind, &applied, &applied_len);
#if defined(PH_HAVE_X11)
            we_own = false;
#endif
        } else {
            PH_LOGW("clipboard text was not accepted");
        }
        free(text);
        return;
    }
    if (kind == 'P' && is_png(data, len) && len <= PH_CLIP_IMAGE_MAX) {
#if defined(PH_HAVE_X11)
        if (!dpy) {
            PH_LOGW("clipboard image was not accepted");
            return;
        }
        own_png(data, len);
        if (!we_own && !incr_on)
            return;
        remember('P', data, len, &seen_kind, &seen, &seen_len);
        remember('P', data, len, &applied_kind, &applied, &applied_len);
#elif defined(_WIN32)
        if (ph_winclip_write_png(data, len)) {
            remember('P', data, len, &seen_kind, &seen, &seen_len);
            remember('P', data, len, &applied_kind, &applied, &applied_len);
        }
#else
        PH_LOGW("clipboard image was not accepted");
#endif
    }
}
