#include "ui/transfer_card.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "core/log.h"
#include "ui/text.h"
#include "ui/window.h"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(PH_HAVE_X11) && defined(PH_HAVE_XSHAPE)
#include <SDL_syswm.h>
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <SDL_syswm.h>
#endif

#define CARD_W 400
#define CARD_H 280
#define CARD_R 28
#define SLIDE_PX 40
#define IN_MS 280
#define OUT_MS 220
#define POP_MS 240
#define HOLD_MS 8000
#define WAIT_MS 14000
#define PAD 28

enum { PHASE_HIDDEN, PHASE_IN, PHASE_HOLD, PHASE_OUT };

struct ph_transfer_card {
    SDL_Window *window;
    SDL_Renderer *renderer;
    int shaped_w; /* pixel width the Windows corner region was cut for */
    SDL_Cursor *hand;
    SDL_Cursor *arrow;
    int phase;
    Uint64 phase_at;
    Uint64 touch;
    Uint64 check_at;
    Uint64 painted_at;
    int rest_x, rest_y;
    int placed_y;
    int op_milli;
    bool opacity_ok;
    bool dirty;
    bool inside;
    bool hot_open, hot_show, hot_close;
    bool received;
    bool can_show;
    bool video;
    int done, total;
    char path[4096];
    char name[160];
    char place[64]; /* folder name shown under the file, such as Downloads */
    SDL_Rect open_r, show_r, close_r;
};

static void rgba(SDL_Renderer *renderer, int red, int green, int blue, int alpha)
{
    SDL_SetRenderDrawColor(renderer, (Uint8)red, (Uint8)green, (Uint8)blue, (Uint8)alpha);
}

static void fill_rect(SDL_Renderer *renderer, int x, int y, int w, int h)
{
    SDL_Rect rect = {x, y, w, h};
    if (w > 0 && h > 0)
        SDL_RenderFillRect(renderer, &rect);
}

static int isqrt_int(int n)
{
    int x = 0;
    if (n <= 0)
        return 0;
    while ((x + 1) <= n / (x + 1))
        x++;
    return x;
}

static void fill_circle(SDL_Renderer *renderer, int cx, int cy, int rad)
{
    int dy;
    if (rad < 1)
        return;
    for (dy = -rad; dy <= rad; dy++) {
        int dx = isqrt_int(rad * rad - dy * dy);
        fill_rect(renderer, cx - dx, cy + dy, dx * 2 + 1, 1);
    }
}

static void fill_round(SDL_Renderer *renderer, int x, int y, int w, int h, int rad)
{
    if (w < 1 || h < 1)
        return;
    if (rad > w / 2)
        rad = w / 2;
    if (rad > h / 2)
        rad = h / 2;
    if (rad < 1) {
        fill_rect(renderer, x, y, w, h);
        return;
    }
    fill_rect(renderer, x + rad, y, w - 2 * rad, h);
    fill_rect(renderer, x, y + rad, rad, h - 2 * rad);
    fill_rect(renderer, x + w - rad, y + rad, rad, h - 2 * rad);
    fill_circle(renderer, x + rad, y + rad, rad);
    fill_circle(renderer, x + w - rad - 1, y + rad, rad);
    fill_circle(renderer, x + rad, y + h - rad - 1, rad);
    fill_circle(renderer, x + w - rad - 1, y + h - rad - 1, rad);
}

static void thick_line(SDL_Renderer *renderer, int x0, int y0, int x1, int y1, int rad)
{
    int steps = abs(x1 - x0) + abs(y1 - y0);
    int i;
    if (steps < 1)
        steps = 1;
    for (i = 0; i <= steps; i++)
        fill_circle(renderer, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, rad);
}

static bool ends_with(const char *text, const char *ext)
{
    size_t nt = strlen(text), ne = strlen(ext), i;
    if (nt < ne)
        return false;
    for (i = 0; i < ne; i++) {
        char a = text[nt - ne + i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != ext[i])
            return false;
    }
    return true;
}

static bool video_name(const char *name)
{
    return ends_with(name, ".mp4") || ends_with(name, ".mov") || ends_with(name, ".m4v")
           || ends_with(name, ".avi") || ends_with(name, ".mkv") || ends_with(name, ".webm");
}

static void copy_name(ph_transfer_card *c, const char *path)
{
    const char *base = path ? path : "";
    const char *scan;
    size_t n;
    c->name[0] = 0;
    for (scan = base; *scan; scan++)
        if (*scan == '/' || *scan == '\\')
            base = scan + 1;
    n = strlen(base);
    if (n >= sizeof c->name)
        n = sizeof c->name - 1;
    memcpy(c->name, base, n);
    c->name[n] = 0;
    if (n > 72) {
        c->name[69] = '.';
        c->name[70] = '.';
        c->name[71] = '.';
        c->name[72] = 0;
    }
    c->video = video_name(c->name);
}

static SDL_Rect display_bounds(int x, int y)
{
    SDL_Rect bounds = {0, 0, 1920, 1080};
    int i, n = SDL_GetNumVideoDisplays();
    for (i = 0; i < n; i++) {
        SDL_Rect area;
        if (SDL_GetDisplayBounds(i, &area) != 0)
            continue;
        if (x >= area.x && y >= area.y && x < area.x + area.w && y < area.y + area.h)
            return area;
        if (i == 0)
            bounds = area;
    }
    return bounds;
}

static void shape_window(SDL_Window *window)
{
#if defined(PH_HAVE_X11) && defined(PH_HAVE_XSHAPE)
    SDL_SysWMinfo info;
    Region region;
    int base = 0, err = 0, y;

    SDL_VERSION(&info.version);
    if (!window || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_X11
        || !info.info.x11.display || !info.info.x11.window)
        return;
    if (!XShapeQueryExtension(info.info.x11.display, &base, &err))
        return;
    region = XCreateRegion();
    if (!region)
        return;
    for (y = 0; y < CARD_H; y++) {
        int inset = 0, dy = -1;
        XRectangle row;
        if (y < CARD_R)
            dy = CARD_R - 1 - y;
        else if (y >= CARD_H - CARD_R)
            dy = y - (CARD_H - CARD_R);
        if (dy >= 0)
            inset = CARD_R - isqrt_int(CARD_R * CARD_R - dy * dy);
        if (inset < 0)
            inset = 0;
        if (CARD_W - 2 * inset <= 0)
            continue;
        row.x = (short)inset;
        row.y = (short)y;
        row.width = (unsigned short)(CARD_W - 2 * inset);
        row.height = 1;
        XUnionRectWithRegion(&row, region, region);
    }
    XShapeCombineRegion(info.info.x11.display, info.info.x11.window, ShapeBounding, 0, 0, region,
                        ShapeSet);
    XDestroyRegion(region);
    XFlush(info.info.x11.display);
#else
    (void)window;
#endif
}

static void place_opacity(ph_transfer_card *c, int y, float opacity)
{
    int milli = (int)(opacity * 1000.0f + 0.5f);
    if (milli < 0)
        milli = 0;
    if (milli > 1000)
        milli = 1000;
    if (y != c->placed_y) {
        SDL_SetWindowPosition(c->window, c->rest_x, y);
        c->placed_y = y;
    }
    if (c->opacity_ok && milli != c->op_milli) {
        if (SDL_SetWindowOpacity(c->window, opacity) != 0) {
            c->opacity_ok = false;
            SDL_SetWindowOpacity(c->window, 1);
        }
        c->op_milli = milli;
    }
}

static void rest_on_display(ph_transfer_card *c)
{
    int mx = 0, my = 0;
    SDL_Rect bounds;
    SDL_GetGlobalMouseState(&mx, &my);
    bounds = display_bounds(mx, my);
    c->rest_x = bounds.x + (bounds.w - CARD_W) / 2;
    c->rest_y = bounds.y + (bounds.h - CARD_H) / 2;
    if (c->rest_y < bounds.y + 24)
        c->rest_y = bounds.y + 24;
}

/* One line. A long name keeps its ending, so the extension stays visible. */
static void fit_name(char *out, size_t cap, const char *text, int width, int scale)
{
    size_t n, tail, head;

    if (!text)
        text = "";
    snprintf(out, cap, "%s", text);
    if (width < 1 || ph_text_width(out, scale) <= width)
        return;
    n = strlen(text);
    tail = n > 12 ? 12 : n / 2;
    for (head = n > tail ? n - tail : 0; head > 0; head--) {
        char probe[180];
        int len = snprintf(probe, sizeof probe, "%.*s...%s", (int)head, text, text + (n - tail));
        if (len > 0 && len < (int)sizeof probe && ph_text_width(probe, scale) <= width) {
            snprintf(out, cap, "%s", probe);
            return;
        }
    }
    snprintf(out, cap, "%s", "...");
}

static int draw_centered(SDL_Renderer *renderer, const char *text, int y, int scale, SDL_Color color)
{
    int box = CARD_W - PAD * 2;
    int width = ph_text_width(text, scale);
    int x = (CARD_W - width) / 2;
    if (width > box) {
        x = PAD;
        width = box;
    }
    return ph_ui_draw_text(renderer, text, x, y, width, scale, color);
}

static void draw_check(SDL_Renderer *renderer, int cx, int cy, int scale)
{
    int rad = 22 * scale / 100;
    if (rad < 1)
        return;
    rgba(renderer, 48, 209, 88, 255);
    fill_circle(renderer, cx, cy, rad);
    rgba(renderer, 255, 255, 255, 255);
    thick_line(renderer, cx + (-11 * scale / 100), cy + (1 * scale / 100), cx + (-4 * scale / 100),
               cy + (8 * scale / 100), scale >= 80 ? 2 : 1);
    thick_line(renderer, cx + (-4 * scale / 100), cy + (8 * scale / 100), cx + (12 * scale / 100),
               cy + (-8 * scale / 100), scale >= 80 ? 2 : 1);
}

static void draw_button(SDL_Renderer *renderer, SDL_Rect rect, const char *label, bool hot)
{
    SDL_Color ink = {255, 255, 255, 255};
    int tw, th;
    if (rect.w < 1)
        return;
    rgba(renderer, hot ? 66 : 44, hot ? 66 : 44, hot ? 72 : 48, 255);
    fill_round(renderer, rect.x, rect.y, rect.w, rect.h, rect.h / 2);
    tw = ph_text_width(label, 2);
    th = ph_text_line_height(2);
    ph_ui_draw_text(renderer, label, rect.x + (rect.w - tw) / 2, rect.y + (rect.h - th) / 2,
                    tw + 4, 2, ink);
}

static void layout_buttons(ph_transfer_card *c)
{
    int y = CARD_H - PAD - 52;
    int w = (CARD_W - PAD * 2 - 12) / 2;
    c->close_r = (SDL_Rect){CARD_W - PAD - 36, 16, 36, 36};
    if (!c->received || !c->path[0]) {
        c->open_r = (SDL_Rect){0, 0, 0, 0};
        c->show_r = (SDL_Rect){0, 0, 0, 0};
        return;
    }
    if (!c->can_show) {
        c->open_r = (SDL_Rect){(CARD_W - 200) / 2, y, 200, 52};
        c->show_r = (SDL_Rect){0, 0, 0, 0};
        return;
    }
    c->open_r = (SDL_Rect){PAD, y, w, 52};
    c->show_r = (SDL_Rect){PAD + w + 12, y, w, 52};
}

static int check_scale(const ph_transfer_card *c, Uint64 now)
{
    Uint64 elapsed;
    if (!c->received)
        return 0;
    if (!c->check_at)
        return 100;
    elapsed = now - c->check_at;
    if (elapsed >= POP_MS)
        return 100;
    return 55 + (int)(elapsed * 45 / POP_MS);
}

static void paint(ph_transfer_card *c, Uint64 now)
{
    SDL_Color white = {255, 255, 255, 255};
    SDL_Color gray = {174, 174, 178, 255};
    char sub[96];
    char frac[32];
    int y, scale;

    if (!c->renderer || c->phase == PHASE_HIDDEN)
        return;
    c->dirty = false;
    c->painted_at = now;
    layout_buttons(c);
    rgba(c->renderer, 28, 28, 30, 255);
    SDL_RenderClear(c->renderer);
    /* Lay out in window points: on a 150% or 200% display the window has more pixels
     * than points, and drawing 1:1 left the card small in its top-left corner. */
    {
        int ww = 0, wh = 0, ow = 0, oh = 0;
        SDL_GetWindowSize(c->window, &ww, &wh);
        if (ww > 0 && wh > 0 && SDL_GetRendererOutputSize(c->renderer, &ow, &oh) == 0 && ow > 0) {
            SDL_RenderSetScale(c->renderer, (float)ow / (float)ww, (float)oh / (float)wh);
#ifdef _WIN32
            if (ow != c->shaped_w) {
                SDL_SysWMinfo info;
                int r = CARD_R * ow / ww;
                SDL_VERSION(&info.version);
                if (SDL_GetWindowWMInfo(c->window, &info) && info.subsystem == SDL_SYSWM_WINDOWS) {
                    HRGN round = CreateRoundRectRgn(0, 0, ow + 1, oh + 1, 2 * r, 2 * r);
                    if (round && SetWindowRgn(info.info.win.window, round, TRUE))
                        c->shaped_w = ow;
                    else if (round)
                        DeleteObject(round);
                }
            }
#endif
        }
    }
    rgba(c->renderer, 255, 255, 255, 255);
    fill_circle(c->renderer, PAD + 8, 34, 8);
    ph_ui_draw_text(c->renderer, "Porthole", PAD + 24, 22, 200, 2, white);
    if (c->hot_close) {
        rgba(c->renderer, 58, 58, 62, 255);
        fill_circle(c->renderer, c->close_r.x + c->close_r.w / 2, c->close_r.y + c->close_r.h / 2,
                    14);
    }
    rgba(c->renderer, 210, 210, 214, 255);
    thick_line(c->renderer, c->close_r.x + 11, c->close_r.y + 11, c->close_r.x + 25,
               c->close_r.y + 25, 1);
    thick_line(c->renderer, c->close_r.x + 25, c->close_r.y + 11, c->close_r.x + 11,
               c->close_r.y + 25, 1);

    scale = check_scale(c, now);
    y = 78;
    if (c->name[0]) {
        char shown[180];
        int text_x = PAD + 56;
        int text_w = CARD_W - text_x - PAD;
        rgba(c->renderer, 246, 246, 248, 255);
        fill_round(c->renderer, PAD, y, 40, 50, 6);
        if (scale > 0)
            draw_check(c->renderer, PAD + 32, y + 42, scale > 36 ? 36 : scale);
        fit_name(shown, sizeof shown, c->name, text_w, 2);
        y = ph_ui_draw_text(c->renderer, shown, text_x, y + 14, text_w, 2, white);
        y += 2;
    }
    if (!c->received)
        snprintf(sub, sizeof sub, "Receiving from iPhone");
    else if (c->place[0] && c->total > 1)
        snprintf(sub, sizeof sub, "Saved %d items in %s", c->total, c->place);
    else if (c->place[0])
        snprintf(sub, sizeof sub, "Saved in %s", c->place);
    else if (c->total > 1)
        snprintf(sub, sizeof sub, "Received %d items from iPhone", c->total);
    else
        snprintf(sub, sizeof sub, "Received from iPhone");
    y = ph_ui_draw_text(c->renderer, sub, c->name[0] ? PAD + 56 : PAD, y, CARD_W - PAD * 2 - (c->name[0] ? 56 : 0),
                        2, gray) + 10;
    if (!c->received && c->total > 0) {
        int track = CARD_W - PAD * 2;
        int fill = c->done > 0 ? track * c->done / c->total : 0;
        if (fill > track)
            fill = track;
        rgba(c->renderer, 58, 58, 62, 255);
        fill_round(c->renderer, PAD, y, track, 6, 3);
        rgba(c->renderer, 255, 255, 255, 255);
        if (fill > 0)
            fill_round(c->renderer, PAD, y, fill < 6 ? 6 : fill, 6, 3);
        y += 16;
        snprintf(frac, sizeof frac, "%d of %d", c->done, c->total);
        draw_centered(c->renderer, frac, y, 2, gray);
    }
    if (c->received) {
        draw_button(c->renderer, c->open_r, "Open", c->hot_open);
        draw_button(c->renderer, c->show_r, "Show", c->hot_show);
    }
    SDL_RenderPresent(c->renderer);
}

static bool hit(SDL_Rect rect, int x, int y)
{
    return rect.w > 0 && rect.h > 0 && x >= rect.x && y >= rect.y && x < rect.x + rect.w
           && y < rect.y + rect.h;
}

static void open_external(const char *path)
{
    if (!path || !path[0])
        return;
#ifdef _WIN32
    ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
#else
    pid_t pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
            closefrom(3);
#else
            for (int fd = 3, max = (int)sysconf(_SC_OPEN_MAX); fd < max && fd < 65536; fd++)
                close(fd);
#endif
            execlp("xdg-open", "xdg-open", path, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
#endif
    PH_LOGI("transfer: open %s", path);
}

static void open_folder(const char *path)
{
    char dir[4096];
    const char *slash = path ? strrchr(path, '/') : NULL;
    size_t n;
    if (!slash || slash == path)
        return;
    n = (size_t)(slash - path);
    if (n >= sizeof dir)
        n = sizeof dir - 1;
    memcpy(dir, path, n);
    dir[n] = 0;
    open_external(dir);
}

static void hide_card(ph_transfer_card *c)
{
    c->phase = PHASE_HIDDEN;
    c->op_milli = 0;
    c->inside = false;
    c->hot_open = c->hot_show = c->hot_close = false;
    if (c->arrow)
        SDL_SetCursor(c->arrow);
    if (c->window)
        SDL_HideWindow(c->window);
}

static float ease(Uint64 elapsed, Uint64 duration)
{
    float t;
    if (duration == 0 || elapsed >= duration)
        return 1;
    t = (float)elapsed / (float)duration;
    return t * t * (3.0f - 2.0f * t);
}

ph_transfer_card *ph_transfer_card_create(void)
{
    ph_transfer_card *c = calloc(1, sizeof *c);
    Uint32 flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP
                   | SDL_WINDOW_SKIP_TASKBAR | SDL_WINDOW_TOOLTIP;

    if (!c)
        return NULL;
    c->window = SDL_CreateWindow("Porthole", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, CARD_W,
                                 CARD_H, flags);
    if (!c->window)
        goto fail;
    c->renderer = SDL_CreateRenderer(c->window, -1, SDL_RENDERER_SOFTWARE);
    if (!c->renderer || !ph_text_attach(c->renderer))
        goto fail;
    c->hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    c->arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    c->opacity_ok = true;
    c->placed_y = -100000;
    shape_window(c->window);
    return c;

fail:
    ph_transfer_card_destroy(c);
    return NULL;
}

void ph_transfer_card_destroy(ph_transfer_card *c)
{
    if (!c)
        return;
    if (c->renderer)
        ph_text_detach(c->renderer);
    if (c->renderer)
        SDL_DestroyRenderer(c->renderer);
    if (c->window)
        SDL_DestroyWindow(c->window);
    if (c->hand)
        SDL_FreeCursor(c->hand);
    if (c->arrow)
        SDL_FreeCursor(c->arrow);
    free(c);
}

void ph_transfer_card_place(ph_transfer_card *c, const char *place)
{
    if (!c)
        return;
    snprintf(c->place, sizeof c->place, "%s", place ? place : "");
    c->dirty = true;
}

void ph_transfer_card_show(ph_transfer_card *c, const char *path, int done, int total,
                           bool can_show, Uint64 now)
{
    bool same_path = false;
    if (!c || !c->window)
        return;
    if (done < 0)
        done = 0;
    if (total < done)
        total = done;
    if (path && path[0] && strcmp(path, c->path) == 0)
        same_path = true;
    if (path && path[0]) {
        snprintf(c->path, sizeof c->path, "%s", path);
        copy_name(c, c->path);
    } else {
        c->path[0] = 0;
        c->name[0] = 0;
        c->video = false;
    }
    c->done = done;
    c->total = total;
    c->can_show = can_show;
    c->received = total > 0 && done >= total && c->path[0];
    c->touch = now;
    c->dirty = true;
    if (c->received && !same_path)
        c->check_at = now;
    else if (!c->received)
        c->check_at = 0;
    if (c->phase == PHASE_HIDDEN || c->phase == PHASE_OUT) {
        rest_on_display(c);
        c->phase = PHASE_IN;
        c->phase_at = now;
        c->placed_y = -100000;
        SDL_ShowWindow(c->window);
        shape_window(c->window);
        place_opacity(c, c->rest_y + SLIDE_PX, 0);
    }
    paint(c, now);
    if (c->received)
        PH_LOGI("transfer: received %s", c->name[0] ? c->name : "a file");
    else
        PH_LOGI("transfer: receiving %d of %d", done, total);
}

void ph_transfer_card_dismiss(ph_transfer_card *c, Uint64 now)
{
    if (!c || c->phase == PHASE_HIDDEN || c->phase == PHASE_OUT)
        return;
    c->phase = PHASE_OUT;
    c->phase_at = now;
}

void ph_transfer_card_tick(ph_transfer_card *c, Uint64 now)
{
    float t;
    int y;
    float opacity;
    Uint64 limit;
    bool popping;

    if (!c || c->phase == PHASE_HIDDEN)
        return;
    if (c->phase == PHASE_IN) {
        t = ease(now - c->phase_at, IN_MS);
        y = c->rest_y + (int)((1.0f - t) * SLIDE_PX);
        opacity = t;
        if (now - c->phase_at >= IN_MS) {
            c->phase = PHASE_HOLD;
            c->phase_at = now;
            y = c->rest_y;
            opacity = 1;
        }
    } else if (c->phase == PHASE_OUT) {
        t = ease(now - c->phase_at, OUT_MS);
        y = c->rest_y + (int)(t * SLIDE_PX);
        opacity = 1.0f - t;
        if (now - c->phase_at >= OUT_MS) {
            hide_card(c);
            return;
        }
    } else {
        y = c->rest_y;
        opacity = 1;
        if (c->inside)
            c->touch = now;
        limit = c->received ? HOLD_MS : WAIT_MS;
        if (!c->inside && c->touch && now - c->touch > limit) {
            c->phase = PHASE_OUT;
            c->phase_at = now;
        }
    }
    place_opacity(c, y, opacity);
    popping = c->check_at && now - c->check_at < POP_MS + 40;
    if (c->check_at && now - c->check_at >= POP_MS)
        c->check_at = 0;
    if (c->dirty || (popping && now - c->painted_at >= 32))
        paint(c, now);
}

bool ph_transfer_card_animating(const ph_transfer_card *c)
{
    if (!c)
        return false;
    if (c->phase == PHASE_IN || c->phase == PHASE_OUT)
        return true;
    return c->check_at != 0;
}

bool ph_transfer_card_event(ph_transfer_card *c, const SDL_Event *event, Uint64 now)
{
    Uint32 window_id = 0;
    int x = 0, y = 0;
    bool open, show, close, changed;
    if (!c || !c->window || !event || c->phase == PHASE_HIDDEN)
        return false;
    if (event->type == SDL_MOUSEMOTION)
        window_id = event->motion.windowID;
    else if (event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP)
        window_id = event->button.windowID;
    else if (event->type == SDL_WINDOWEVENT)
        window_id = event->window.windowID;
    else
        return false;
    if (window_id != SDL_GetWindowID(c->window))
        return false;

    if (event->type == SDL_WINDOWEVENT) {
        if (event->window.event == SDL_WINDOWEVENT_LEAVE) {
            c->inside = false;
            c->hot_open = c->hot_show = c->hot_close = false;
            if (c->arrow)
                SDL_SetCursor(c->arrow);
            c->dirty = true;
            paint(c, now);
        } else if (event->window.event == SDL_WINDOWEVENT_EXPOSED
                   || event->window.event == SDL_WINDOWEVENT_SHOWN) {
            c->dirty = true;
            paint(c, now);
        }
        return true;
    }

    if (event->type == SDL_MOUSEMOTION) {
        x = event->motion.x;
        y = event->motion.y;
    } else {
        x = event->button.x;
        y = event->button.y;
    }
    c->inside = true;
    c->touch = now;
    open = hit(c->open_r, x, y);
    show = hit(c->show_r, x, y);
    close = hit(c->close_r, x, y);
    changed = open != c->hot_open || show != c->hot_show || close != c->hot_close;
    c->hot_open = open;
    c->hot_show = show;
    c->hot_close = close;
    if (c->hand && c->arrow)
        SDL_SetCursor((open || show || close) ? c->hand : c->arrow);
    if (changed) {
        c->dirty = true;
        paint(c, now);
    }
    if (event->type == SDL_MOUSEBUTTONDOWN && event->button.button == SDL_BUTTON_LEFT) {
        if (close) {
            ph_transfer_card_dismiss(c, now);
        } else if (open) {
            open_external(c->path);
            c->touch = now;
        } else if (show) {
            open_folder(c->path);
            c->touch = now;
        }
    }
    return true;
}
