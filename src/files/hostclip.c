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

#ifdef _WIN32
typedef struct clip_image_job {
    ph_winclip_image *image;
    uint32_t clipboard_sequence;
    uint64_t generation;
    size_t len;
    uint8_t data[];
} clip_image_job;

static SDL_mutex *clip_lock;
static SDL_cond *clip_wake;
static SDL_Thread *clip_worker;
static clip_image_job *clip_request, *clip_done;
static uint64_t clip_generation;
static bool clip_stop, clip_pending;

static void free_image_job(clip_image_job *job)
{
    if (job) {
        ph_winclip_discard_image(job->image);
        free(job);
    }
}

static int SDLCALL process_clip_images(void *unused)
{
    (void)unused;
    SDL_LockMutex(clip_lock);
    while (!clip_stop) {
        while (!clip_stop && !clip_request)
            SDL_CondWait(clip_wake, clip_lock);
        if (clip_stop)
            break;
        clip_image_job *job = clip_request;
        clip_request = NULL;
        SDL_UnlockMutex(clip_lock);
        job->image = ph_winclip_prepare_image(job->data, job->len);
        SDL_LockMutex(clip_lock);
        if (!clip_stop && job->generation == clip_generation) {
            clip_done = job;
        } else {
            SDL_UnlockMutex(clip_lock);
            free_image_job(job);
            SDL_LockMutex(clip_lock);
        }
    }
    SDL_UnlockMutex(clip_lock);
    return 0;
}

static bool queue_clip_image(const void *data, size_t len)
{
    if (!clip_worker || !ph_winclip_image_supported(data, len))
        return false;
    clip_image_job *job = calloc(1, sizeof *job + len);
    if (!job)
        return false;
    job->clipboard_sequence = ph_winclip_sequence();
    job->len = len;
    memcpy(job->data, data, len);
    ph_hostclip_cancel_pending();
    SDL_LockMutex(clip_lock);
    job->generation = clip_generation;
    clip_request = job;
    clip_pending = true;
    SDL_CondSignal(clip_wake);
    SDL_UnlockMutex(clip_lock);
    return true;
}

static int finish_clip_image(void)
{
    if (!clip_worker)
        return 0;
    SDL_LockMutex(clip_lock);
    clip_image_job *job = clip_done;
    clip_done = NULL;
    if (job)
        clip_pending = false;
    SDL_UnlockMutex(clip_lock);
    if (!job)
        return 0;
    int result = ph_winclip_publish_image(job->image, job->clipboard_sequence);
    if (result > 0) {
        remember('P', job->data, job->len, &seen_kind, &seen, &seen_len);
        remember('P', job->data, job->len, &applied_kind, &applied, &applied_len);
    }
    free_image_job(job);
    return result;
}
#endif

bool ph_hostclip_pending(void)
{
#ifdef _WIN32
    /* Only the UI thread sets or clears pending. */
    return clip_pending;
#else
    return false;
#endif
}

void ph_hostclip_cancel_pending(void)
{
#ifdef _WIN32
    if (!clip_worker)
        return;
    SDL_LockMutex(clip_lock);
    clip_generation++;
    clip_pending = false;
    clip_image_job *request = clip_request, *completed = clip_done;
    clip_request = clip_done = NULL;
    SDL_UnlockMutex(clip_lock);
    free_image_job(request);
    free_image_job(completed);
#endif
}

void ph_hostclip_init(void)
{
    open_clipboard();
#ifdef _WIN32
    ph_winclip_init();
    clip_lock = SDL_CreateMutex();
    clip_wake = SDL_CreateCond();
    clip_stop = false;
    if (clip_lock && clip_wake)
        clip_worker = SDL_CreateThread(process_clip_images, "Clipboard images", NULL);
#endif
}

void ph_hostclip_shutdown(void)
{
#ifdef _WIN32
    ph_hostclip_cancel_pending();
    if (clip_worker) {
        SDL_LockMutex(clip_lock);
        clip_stop = true;
        SDL_CondSignal(clip_wake);
        SDL_UnlockMutex(clip_lock);
        SDL_WaitThread(clip_worker, NULL);
        clip_worker = NULL;
    }
    SDL_DestroyCond(clip_wake);
    SDL_DestroyMutex(clip_lock);
    clip_wake = NULL;
    clip_lock = NULL;
    ph_winclip_shutdown();
#endif
    close_clipboard();
    free(seen);
    free(applied);
    seen = applied = NULL;
    seen_len = applied_len = 0;
    seen_kind = applied_kind = 0;
}

int ph_hostclip_pump(void)
{
#if defined(PH_HAVE_X11)
    if (!dpy)
        return 0;
    while (XPending(dpy)) {
        XEvent ev;

        XNextEvent(dpy, &ev);
        handle_event(&ev);
    }
#elif defined(_WIN32)
    return finish_clip_image();
#endif
    return 0;
}

bool ph_hostclip_changed(char *kind, void **data, size_t *len)
{
    char *text = NULL;
    size_t n = 0;

    if (!kind || !data || !len)
        return false;
    *data = NULL;
#if defined(_WIN32)
    if (!ph_winclip_read_text(&text, &n, PH_CLIP_TEXT_MAX)) {
        text = NULL;
        n = 0;
    }
#else
    text = SDL_GetClipboardText();
    n = text ? strlen(text) : 0;
#endif
    if (text && n > 0 && n <= PH_CLIP_TEXT_MAX && !memchr(text, 0, n) && !already('T', text, n)) {
        *data = malloc(n);
        if (!*data) {
#if defined(_WIN32)
            free(text);
#else
            SDL_free(text);
#endif
            return false;
        }
        memcpy(*data, text, n);
        *len = n;
        *kind = 'T';
        remember('T', text, n, &seen_kind, &seen, &seen_len);
#if defined(_WIN32)
        free(text);
#else
        SDL_free(text);
#endif
        return true;
    }
#if defined(_WIN32)
    free(text);
#else
    SDL_free(text);
#endif
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

bool ph_hostclip_apply(char kind, const void *data, size_t len)
{
    if (!data || !len)
        return false;
    if (kind == 'T') {
        if (len > PH_CLIP_TEXT_MAX || memchr(data, 0, len))
            return false;
        char *text = malloc(len + 1);
        if (!text)
            return false;
        memcpy(text, data, len);
        text[len] = 0;
#if defined(_WIN32)
        ph_hostclip_cancel_pending();
        bool ok = ph_winclip_write_text(text, len);
#else
        bool ok = SDL_SetClipboardText(text) == 0;
#if defined(PH_HAVE_X11)
        if (ok)
            we_own = false;
#endif
#endif
        free(text);
        if (!ok)
            return false;
    } else if (kind == 'P' && len <= PH_CLIP_IMAGE_MAX && is_png(data, len)) {
#if defined(PH_HAVE_X11)
        if (!dpy)
            return false;
        own_png(data, len);
        if (!we_own && !incr_on)
            return false;
#elif defined(_WIN32)
        return queue_clip_image(data, len);
#else
        return false;
#endif
    } else {
        return false;
    }
    remember(kind, data, len, &seen_kind, &seen, &seen_len);
    remember(kind, data, len, &applied_kind, &applied, &applied_len);
    return true;
}
