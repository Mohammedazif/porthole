#include "ui/drag_ghost.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include "core/log.h"
#include "ui/display_scale.h"
#include "ui/text.h"
#include "ui/window.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <SDL_syswm.h>
#endif

#if defined(PH_HAVE_X11) && defined(PH_HAVE_XSHAPE)
#include <SDL_syswm.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#endif

#define GHOST_LONG 112
#define GHOST_MIN_W 168
#define GHOST_SCALE 2

struct ph_drag_ghost {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    float content_scale;
    int img_w, img_h;
    int win_w, win_h;
    int mx, my;
    bool shown;
    bool aimed;
    char caption[160];
};

static void punch_input(SDL_Window *window)
{
#if defined(PH_HAVE_X11) && defined(PH_HAVE_XSHAPE)
    SDL_SysWMinfo info;
    Region empty;
    int base = 0, err = 0;

    SDL_VERSION(&info.version);
    if (!window || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_X11
        || !info.info.x11.display || !info.info.x11.window)
        return;
    if (!XShapeQueryExtension(info.info.x11.display, &base, &err))
        return;
    empty = XCreateRegion();
    if (!empty)
        return;
    XShapeCombineRegion(info.info.x11.display, info.info.x11.window, ShapeInput, 0, 0, empty,
                        ShapeSet);
    XDestroyRegion(empty);
    XFlush(info.info.x11.display);
#elif defined(_WIN32)
    /* Clicks and the release go through to whatever is under the card, as on Linux:
     * a release landing on the card left the drag stuck to the mouse. */
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (window && SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_WINDOWS) {
        HWND hwnd = info.info.win.window;
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE);
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    }
#else
    (void)window;
#endif
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

static void place(ph_drag_ghost *g)
{
    SDL_Rect bounds = display_bounds(g->mx, g->my);
    if (g->win_w < 1 || g->win_h < 1)
        return;
    int width = ph_display_units(g->win_w, g->content_scale);
    int height = ph_display_units(g->win_h, g->content_scale);
    int offset_x = ph_display_units(16, g->content_scale);
    int offset_y = ph_display_units(20, g->content_scale);
    int x = g->mx + offset_x;
    int y = g->my + offset_y;
    if (x + width > bounds.x + bounds.w)
        x = g->mx - width - offset_x;
    if (y + height > bounds.y + bounds.h)
        y = g->my - height - offset_y;
    SDL_SetWindowPosition(g->window, x, y);
}

static void layout(ph_drag_ghost *g)
{
    int text_w, text_h, card_w, card_h;

    card_w = g->img_w + 20;
    if (card_w < GHOST_MIN_W)
        card_w = GHOST_MIN_W;
    text_w = card_w - 20;
    text_h = g->caption[0] ? ph_ui_text_height(g->caption, text_w, GHOST_SCALE) : 10 * GHOST_SCALE;
    card_h = 10 + g->img_h + 8 + text_h + 10;
    if (card_w == g->win_w && card_h == g->win_h)
        return;
    g->win_w = card_w;
    g->win_h = card_h;
    SDL_SetWindowSize(g->window, ph_display_units(card_w, g->content_scale),
                      ph_display_units(card_h, g->content_scale));
    SDL_RenderSetViewport(g->renderer, NULL);
}

static void paint(ph_drag_ghost *g)
{
    SDL_Rect frame, dest;
    SDL_Color ink;
    int text_w;

    if (!g->renderer)
        return;
    SDL_SetRenderDrawColor(g->renderer, 28, 28, 30, 255);
    SDL_RenderClear(g->renderer);
    int ow = 0, oh = 0;
    if (g->win_w > 0 && g->win_h > 0 &&
        SDL_GetRendererOutputSize(g->renderer, &ow, &oh) == 0 && ow > 0 && oh > 0)
        SDL_RenderSetScale(g->renderer, (float)ow / g->win_w, (float)oh / g->win_h);
    frame = (SDL_Rect){0, 0, g->win_w, g->win_h};
    SDL_SetRenderDrawColor(g->renderer, 92, 92, 96, 255);
    SDL_RenderDrawRect(g->renderer, &frame);
    if (g->texture && g->img_w > 0 && g->img_h > 0) {
        dest = (SDL_Rect){(g->win_w - g->img_w) / 2, 10, g->img_w, g->img_h};
        SDL_RenderCopy(g->renderer, g->texture, NULL, &dest);
    }
    text_w = g->win_w - 20;
    if (text_w < 6 * GHOST_SCALE)
        text_w = 6 * GHOST_SCALE;
    ink = g->aimed ? (SDL_Color){255, 255, 255, 255} : (SDL_Color){186, 186, 186, 255};
    if (g->caption[0])
        ph_ui_draw_text(g->renderer, g->caption, 10, 10 + g->img_h + 8, text_w, GHOST_SCALE, ink);
    SDL_RenderPresent(g->renderer);
}

static bool crop_rgb(const AVFrame *frame, ph_rect rect, AVFrame *rgb, int *dw, int *dh)
{
    int x, y, w, h, long_edge, out_w, out_h;
    struct SwsContext *sws;
    const uint8_t *src[4];
    int src_lines[4];
    enum AVPixelFormat fmt;

    fmt = (enum AVPixelFormat)frame->format;
    if ((fmt != AV_PIX_FMT_YUV420P && fmt != AV_PIX_FMT_YUVJ420P) || !frame->data[0]
        || !frame->data[1] || !frame->data[2])
        return false;
    if (frame->linesize[0] <= 0 || frame->linesize[1] <= 0 || frame->linesize[2] <= 0)
        return false;
    x = rect.x & ~1;
    y = rect.y & ~1;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    w = rect.w & ~1;
    h = rect.h & ~1;
    if (x >= frame->width || y >= frame->height)
        return false;
    if (x + w > frame->width)
        w = (frame->width - x) & ~1;
    if (y + h > frame->height)
        h = (frame->height - y) & ~1;
    if (w < 2 || h < 2)
        return false;
    long_edge = w > h ? w : h;
    out_w = w;
    out_h = h;
    if (long_edge > GHOST_LONG) {
        out_w = w * GHOST_LONG / long_edge;
        out_h = h * GHOST_LONG / long_edge;
    }
    if (out_w < 1)
        out_w = 1;
    if (out_h < 1)
        out_h = 1;
    src[0] = frame->data[0] + (size_t)y * (size_t)frame->linesize[0] + (size_t)x;
    src[1] = frame->data[1] + (size_t)(y / 2) * (size_t)frame->linesize[1] + (size_t)(x / 2);
    src[2] = frame->data[2] + (size_t)(y / 2) * (size_t)frame->linesize[2] + (size_t)(x / 2);
    src[3] = NULL;
    src_lines[0] = frame->linesize[0];
    src_lines[1] = frame->linesize[1];
    src_lines[2] = frame->linesize[2];
    src_lines[3] = 0;
    rgb->format = AV_PIX_FMT_RGBA;
    rgb->width = out_w;
    rgb->height = out_h;
    if (av_frame_get_buffer(rgb, 32) < 0)
        return false;
    sws = sws_getContext(w, h, fmt, out_w, out_h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) {
        av_frame_unref(rgb);
        return false;
    }
    if (sws_scale(sws, src, src_lines, 0, h, rgb->data, rgb->linesize) <= 0) {
        sws_freeContext(sws);
        av_frame_unref(rgb);
        return false;
    }
    sws_freeContext(sws);
    *dw = out_w;
    *dh = out_h;
    return true;
}

ph_drag_ghost *ph_drag_ghost_create(void)
{
    ph_drag_ghost *g = calloc(1, sizeof *g);
    Uint32 flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP
                   | SDL_WINDOW_SKIP_TASKBAR | SDL_WINDOW_TOOLTIP | SDL_WINDOW_ALLOW_HIGHDPI;

    if (!g)
        return NULL;
    g->window = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 16, 16, flags);
    if (!g->window) {
        free(g);
        return NULL;
    }
    g->renderer = SDL_CreateRenderer(g->window, -1, SDL_RENDERER_SOFTWARE);
    if (!g->renderer) {
        SDL_DestroyWindow(g->window);
        free(g);
        return NULL;
    }
    if (!ph_text_attach(g->renderer))
        PH_LOGW("photo drag caption unavailable");
    return g;
}

void ph_drag_ghost_destroy(ph_drag_ghost *g)
{
    if (!g)
        return;
    if (g->texture)
        SDL_DestroyTexture(g->texture);
    if (g->renderer)
        ph_text_detach(g->renderer);
    if (g->renderer)
        SDL_DestroyRenderer(g->renderer);
    if (g->window)
        SDL_DestroyWindow(g->window);
    free(g);
}

bool ph_drag_ghost_show(ph_drag_ghost *g, const struct AVFrame *frame, ph_rect rect)
{
    AVFrame *rgb;
    int dw = 0, dh = 0;

    if (!g || !g->window || !g->renderer || !frame)
        return false;
    rgb = av_frame_alloc();
    if (!rgb || !crop_rgb(frame, rect, rgb, &dw, &dh)) {
        av_frame_free(&rgb);
        PH_LOGW("photo pull: couldn't copy the photo under the cursor");
        return false;
    }
    if (g->texture)
        SDL_DestroyTexture(g->texture);
    g->texture = SDL_CreateTexture(g->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, dw,
                                   dh);
    if (!g->texture || SDL_UpdateTexture(g->texture, NULL, rgb->data[0], rgb->linesize[0]) != 0) {
        if (g->texture) {
            SDL_DestroyTexture(g->texture);
            g->texture = NULL;
        }
        av_frame_free(&rgb);
        PH_LOGW("photo pull: couldn't show the photo under the cursor");
        return false;
    }
    av_frame_free(&rgb);
    g->img_w = dw;
    g->img_h = dh;
    float scale = ph_display_content_scale(g->window);
    if (scale != g->content_scale)
        g->win_w = 0;
    g->content_scale = scale;
    snprintf(g->caption, sizeof g->caption, "Drag onto the desktop or a folder");
    g->aimed = false;
    layout(g);
    SDL_GetGlobalMouseState(&g->mx, &g->my);
    place(g);
    g->shown = true;
    SDL_ShowWindow(g->window);
    punch_input(g->window);
    paint(g);
    return true;
}

void ph_drag_ghost_caption(ph_drag_ghost *g, const char *text, bool aimed)
{
    if (!g || !g->shown || !text)
        return;
    if (g->aimed == aimed && strcmp(g->caption, text) == 0)
        return;
    snprintf(g->caption, sizeof g->caption, "%s", text);
    g->aimed = aimed;
    layout(g);
    place(g);
    paint(g);
}

void ph_drag_ghost_move(ph_drag_ghost *g, int root_x, int root_y)
{
    if (!g || !g->shown)
        return;
    if (g->mx == root_x && g->my == root_y)
        return;
    g->mx = root_x;
    g->my = root_y;
    place(g);
}

void ph_drag_ghost_hide(ph_drag_ghost *g)
{
    if (!g || !g->window)
        return;
    g->shown = false;
    g->caption[0] = 0;
    SDL_HideWindow(g->window);
}

SDL_Window *ph_drag_ghost_window(const ph_drag_ghost *g)
{
    return g ? g->window : NULL;
}
