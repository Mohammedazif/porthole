#include "ui/transfer_card.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "core/log.h"
#include "core/upath.h"
#include "files/places.h"
#include "ui/brand_art.h"
#include "ui/control_icons.h"
#include "ui/display_scale.h"
#include "ui/drawing.h"
#include "ui/text.h"

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

#define CARD_W 356
#define CARD_H 174
#define CARD_R 14
#define SLIDE_PX 40
#define IN_MS 280
#define OUT_MS 220
#define POP_MS 240
#define HOLD_MS 8000
#define WAIT_MS 14000
#define PAD 17
#define ROW_H 34
#define MAX_ROWS 8

enum { PHASE_HIDDEN, PHASE_IN, PHASE_HOLD, PHASE_OUT };

struct ph_transfer_card {
    SDL_Window *window;
    SDL_Renderer *renderer;
    float content_scale;
    SDL_Texture *logo, *light_logo;
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
    bool dark, action_failed;
    int done, total;
    int h;            /* card height in points: grows when the list opens */
    SDL_Rect bounds;  /* the display it rests on */
    /* A batch from the iPhone: the item arriving now, then every saved one. */
    bool batch, expanded, hot_more;
    int hot_row;
    char current[160];
    unsigned long long item_bytes, item_size;
    char (*items)[1024];
    int item_count, item_cap;
    SDL_Rect more_r;
    char path[4096];
    char name[160];
    char place[64]; /* folder name shown under the file, such as Downloads */
    SDL_Rect open_r, show_r, close_r;
};

#if defined(PH_HAVE_X11) && defined(PH_HAVE_XSHAPE)
static int isqrt_int(int n)
{
    int x = 0;
    while ((x + 1) <= n / (x + 1))
        x++;
    return x;
}
#endif

static void copy_name(ph_transfer_card *c, const char *path)
{
    const char *base = path ? path : "";
    const char *scan;
    size_t n;
    c->name[0] = 0;
    for (scan = base; *scan; scan++)
        if (*scan == '/' || *scan == '\\')
            base = scan + 1;
    n = SDL_min(strlen(base), sizeof c->name - 1);
    while (n && ((unsigned char)base[n] & 192) == 128)
        n--;
    memcpy(c->name, base, n);
    c->name[n] = 0;
}

static SDL_Rect display_bounds(int x, int y)
{
    SDL_Rect bounds = {0, 0, 1920, 1080};
    int i, n = SDL_GetNumVideoDisplays();
    for (i = 0; i < n; i++) {
        SDL_Rect area;
        if (SDL_GetDisplayUsableBounds(i, &area) != 0)
            continue;
        if (x >= area.x && y >= area.y && x < area.x + area.w && y < area.y + area.h)
            return area;
        if (i == 0)
            bounds = area;
    }
    return bounds;
}

static void shape_window(SDL_Window *window, int card_h)
{
#if defined(PH_HAVE_X11) && defined(PH_HAVE_XSHAPE)
    SDL_SysWMinfo info;
    Region region;
    int base = 0, err = 0, width, height;

    SDL_VERSION(&info.version);
    if (!window || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_X11
        || !info.info.x11.display || !info.info.x11.window)
        return;
    if (!XShapeQueryExtension(info.info.x11.display, &base, &err))
        return;
    region = XCreateRegion();
    if (!region)
        return;
    SDL_GetWindowSize(window, &width, &height);
    int radius = ph_display_units(CARD_R, (float)height / card_h);
    for (int y = 0; y < height; y++) {
        int inset = 0, dy = -1;
        XRectangle row;
        if (y < radius)
            dy = radius - 1 - y;
        else if (y >= height - radius)
            dy = y - (height - radius);
        if (dy >= 0)
            inset = radius - isqrt_int(radius * radius - dy * dy);
        if (inset < 0)
            inset = 0;
        if (width - 2 * inset <= 0)
            continue;
        row.x = (short)inset;
        row.y = (short)y;
        row.width = (unsigned short)(width - 2 * inset);
        row.height = 1;
        XUnionRectWithRegion(&row, region, region);
    }
    XShapeCombineRegion(info.info.x11.display, info.info.x11.window, ShapeBounding, 0, 0, region,
                        ShapeSet);
    XDestroyRegion(region);
    XFlush(info.info.x11.display);
#else
    (void)window;
    (void)card_h;
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
    c->bounds = bounds;
    float fit_x = (float)SDL_max(1, bounds.w) / CARD_W;
    float fit_y = (float)SDL_max(1, bounds.h) / c->h;
    c->content_scale = SDL_min(ph_display_content_scale(c->window), SDL_min(fit_x, fit_y));
    int width = SDL_max(1, ph_display_units(CARD_W, c->content_scale));
    int height = SDL_max(1, ph_display_units(c->h, c->content_scale));
    int margin = ph_display_units(24, c->content_scale);
    SDL_SetWindowSize(c->window, width, height);
    c->rest_x = SDL_max(bounds.x, bounds.x + bounds.w - width - margin);
    c->rest_y = SDL_max(bounds.y, bounds.y + bounds.h - height - margin);
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
    while (tail && ((unsigned char)text[n - tail] & 192) == 128)
        tail--;
    for (head = n > tail ? n - tail : 0; head > 0; head--) {
        if (((unsigned char)text[head] & 192) == 128)
            continue;
        char probe[180];
        int len = snprintf(probe, sizeof probe, "%.*s...%s", (int)head, text, text + (n - tail));
        if (len > 0 && len < (int)sizeof probe && ph_text_width(probe, scale) <= width) {
            snprintf(out, cap, "%s", probe);
            return;
        }
    }
    snprintf(out, cap, "%s", "...");
}

static void draw_button(SDL_Renderer *renderer, SDL_Rect box, const char *label,
                        ph_control_icon icon, SDL_Color bg, SDL_Color ink)
{
    if (box.w < 1)
        return;
    ph_ui_round_rect(renderer, box, 8, bg);
    int font = PH_TEXT(12, PH_FONT_MEDIUM);
    int tw = ph_text_width(label, font);
    bool with_icon = (int)icon >= 0;
    int left = box.x + (box.w - tw - (with_icon ? 22 : 0)) / 2;
    if (with_icon)
        ph_icon_draw(renderer, icon, (SDL_Rect){left, box.y + (box.h - 15) / 2, 15, 15}, ink);
    ph_text_draw(renderer, label, left + (with_icon ? 22 : 0), box.y + (box.h - ph_text_line_height(font)) / 2,
                 tw + 3, font, ink);
}

static bool several(const ph_transfer_card *c)
{
    return c->batch && c->item_count > 1;
}

static int list_rows(const ph_transfer_card *c)
{
    if (!c->expanded || !several(c))
        return 0;
    return SDL_min(c->item_count, MAX_ROWS) + (c->item_count > MAX_ROWS ? 1 : 0);
}

static int wanted_height(const ph_transfer_card *c)
{
    int rows = list_rows(c);
    return rows ? CARD_H + rows * ROW_H + 6 : CARD_H;
}

static void layout_buttons(ph_transfer_card *c)
{
    c->close_r = (SDL_Rect){CARD_W - PAD - 26, 12, 28, 28};
    int y = CARD_H - PAD - 33;
    if (c->received && several(c)) {
        /* Several items: the list opens instead of one file. */
        c->open_r = (SDL_Rect){0};
        c->more_r = (SDL_Rect){PAD, y, 100, 33};
        c->show_r = c->can_show ? (SDL_Rect){PAD + 108, y, 136, 33} : (SDL_Rect){0};
        return;
    }
    c->more_r = (SDL_Rect){0};
    c->open_r = c->received ? (SDL_Rect){PAD, y, 100, 33} : (SDL_Rect){0};
    c->show_r = c->received && c->can_show ? (SDL_Rect){PAD + 108, y, 136, 33} : (SDL_Rect){0};
}

static SDL_Rect row_rect(int index)
{
    return (SDL_Rect){PAD - 6, CARD_H - 4 + index * ROW_H, CARD_W - 2 * PAD + 12, ROW_H - 2};
}

static bool picture_name(const char *name)
{
    static const char *const kinds[] = {"jpg", "jpeg", "png", "heic", "heif", "gif", "webp", "tif", "tiff",
                                        "bmp", "dng", "mov", "mp4", "m4v", "hevc", "avi"};
    const char *dot = strrchr(name ? name : "", '.');
    if (!dot)
        return false;
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
        if (!SDL_strcasecmp(dot + 1, kinds[i]))
            return true;
    return false;
}

/* "2.1 MB" */
static void format_size(char *out, size_t cap, unsigned long long bytes)
{
    static const char *const units[] = {"bytes", "KB", "MB", "GB", "TB"};
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1000 && unit < 4) {
        value /= 1000;
        unit++;
    }
    if (unit == 0)
        snprintf(out, cap, "%llu bytes", bytes);
    else
        snprintf(out, cap, value < 10 ? "%.1f %s" : "%.0f %s", value, units[unit]);
}

/* Resizes and re-seats the card when its list opens or closes. */
static void fit_height(ph_transfer_card *c)
{
    int wanted = wanted_height(c);
    if (wanted == c->h)
        return;
    c->h = wanted;
    int width = SDL_max(1, ph_display_units(CARD_W, c->content_scale));
    int height = SDL_max(1, ph_display_units(c->h, c->content_scale));
    int margin = ph_display_units(24, c->content_scale);
    SDL_SetWindowSize(c->window, width, height);
    c->rest_y = SDL_max(c->bounds.y, c->bounds.y + c->bounds.h - height - margin);
    c->shaped_w = 0;
    if (c->phase == PHASE_HOLD)
        place_opacity(c, c->rest_y, 1);
    shape_window(c->window, c->h);
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
    if (!c->renderer || c->phase == PHASE_HIDDEN)
        return;
    c->dirty = false;
    c->painted_at = now;
    layout_buttons(c);
    SDL_Color bg = c->dark ? (SDL_Color){27, 30, 34, 255} : (SDL_Color){250, 251, 253, 255};
    SDL_Color ink = c->dark ? (SDL_Color){245, 246, 248, 255} : (SDL_Color){20, 27, 36, 255};
    SDL_Color muted = c->dark ? (SDL_Color){173, 184, 197, 255} : (SDL_Color){77, 91, 109, 255};
    SDL_Color tile = c->dark ? (SDL_Color){42, 48, 57, 255} : (SDL_Color){232, 236, 241, 255};
    SDL_Color hover = c->dark ? (SDL_Color){58, 67, 80, 255} : (SDL_Color){215, 224, 234, 255};
    SDL_Renderer *r = c->renderer;
    SDL_SetRenderDrawColor(r, bg.r, bg.g, bg.b, 255);
    SDL_RenderClear(r);
    /* Render the fixed point layout at the actual drawable density. */
    {
        int ow = 0, oh = 0;
        if (SDL_GetRendererOutputSize(c->renderer, &ow, &oh) == 0 && ow > 0 && oh > 0) {
            SDL_RenderSetScale(c->renderer, (float)ow / CARD_W, (float)oh / c->h);
#ifdef _WIN32
            if (ow != c->shaped_w) {
                SDL_SysWMinfo info;
                int radius = CARD_R * ow / CARD_W;
                SDL_VERSION(&info.version);
                if (SDL_GetWindowWMInfo(c->window, &info) && info.subsystem == SDL_SYSWM_WINDOWS) {
                    HRGN round = CreateRoundRectRgn(0, 0, ow + 1, oh + 1, 2 * radius, 2 * radius);
                    if (round && SetWindowRgn(info.info.win.window, round, TRUE))
                        c->shaped_w = ow;
                    else if (round)
                        DeleteObject(round);
                }
            }
#endif
        }
    }
    int title_font = PH_TEXT(13, PH_FONT_MEDIUM);
    int secondary_font = PH_TEXT(12, PH_FONT_REGULAR);
    SDL_Rect mark = {PAD, 17, 18, 18};
    SDL_Texture *logo = c->dark && c->light_logo ? c->light_logo : c->logo;
    if (logo)
        SDL_RenderCopy(r, logo, NULL, &mark);
    ph_text_draw(r, "Porthole", PAD + 25, 17, 180, secondary_font, muted);
    if (c->hot_close)
        ph_ui_round_rect(r, c->close_r, 7, hover);
    ph_icon_draw(r, PH_ICON_CLOSE,
                 (SDL_Rect){c->close_r.x + 6, c->close_r.y + 6, 16, 16}, ink);

    SDL_Rect file = {PAD, 53, 44, 46};
    if (c->received && several(c)) {
        /* A stack of files, with how many. */
        SDL_Color back = c->dark ? (SDL_Color){34, 39, 46, 255} : (SDL_Color){222, 227, 234, 255};
        ph_ui_round_rect(r, (SDL_Rect){file.x + 7, file.y - 5, file.w - 4, file.h - 2}, 9, back);
        ph_ui_round_rect(r, (SDL_Rect){file.x + 3, file.y - 2, file.w - 2, file.h - 1}, 9, hover);
    }
    ph_ui_round_rect(r, file, 9, tile);
    ph_icon_draw(r, !c->received ? PH_ICON_RECEIVE
                    : several(c) || !picture_name(c->name) ? PH_ICON_FILE : PH_ICON_SHOT,
                 (SDL_Rect){file.x + 11, file.y + 11, 22, 22}, ink);
    if (c->received && several(c)) {
        char count[16];
        snprintf(count, sizeof count, "%d", c->item_count);
        int count_font = PH_TEXT(11, PH_FONT_MEDIUM);
        int cw = SDL_max(20, ph_text_width(count, count_font) + 10);
        SDL_Rect bubble = {file.x + file.w - cw / 2 - 2, file.y - 9, cw, 20};
        ph_ui_round_rect(r, bubble, 10, ink);
        ph_text_draw(r, count, bubble.x + (cw - ph_text_width(count, count_font)) / 2,
                     bubble.y + (20 - ph_text_line_height(count_font)) / 2, cw, count_font, bg);
    }
    if (c->received) {
        int side = 15 * check_scale(c, now) / 100;
        SDL_Rect badge = {file.x + 39 - side / 2, file.y + 40 - side / 2, side, side};
        ph_ui_round_rect(r, badge, side / 2, (SDL_Color){23, 126, 70, 255});
        ph_icon_draw(r, PH_ICON_CHECK, badge, (SDL_Color){255, 255, 255, 255});
    }
    char heading[180], sub[200], shown[200];
    if (!c->received) {
        snprintf(heading, sizeof heading, "Receiving from iPhone");
        if (c->batch && c->current[0] && c->total > 1)
            snprintf(sub, sizeof sub, "%d of %d · %.150s", SDL_min(c->done + 1, c->total), c->total, c->current);
        else if (c->batch && c->current[0])
            snprintf(sub, sizeof sub, "%.150s", c->current);
        else
            snprintf(sub, sizeof sub, "%d of %d", c->done, c->total);
    } else {
        if (several(c))
            snprintf(heading, sizeof heading, "%d items received", c->item_count);
        else if (c->total > 1)
            snprintf(heading, sizeof heading, "%d files received", c->total);
        else
            snprintf(heading, sizeof heading, "%s", c->name);
        if (c->action_failed)
            snprintf(sub, sizeof sub, "Unable to open the saved location");
        else if (c->place[0])
            snprintf(sub, sizeof sub, "Saved in %s", c->place);
        else
            snprintf(sub, sizeof sub, "Received from iPhone");
    }
    int text_x = file.x + file.w + 13, text_width = CARD_W - text_x - PAD;
    fit_name(shown, sizeof shown, heading, text_width, title_font);
    ph_text_draw(r, shown, text_x, 56, text_width, title_font, ink);
    fit_name(shown, sizeof shown, sub, text_width, secondary_font);
    ph_text_draw(r, shown, text_x, 78, text_width, secondary_font, muted);
    if (!c->received && c->total > 0) {
        /* Live: whole items plus how far into the one arriving now. */
        int track = CARD_W - PAD * 2;
        double part = c->item_size ? (double)c->item_bytes / (double)c->item_size : 0;
        double fraction = ((double)c->done + SDL_min(part, 1.0)) / c->total;
        int fill = (int)(track * SDL_min(fraction, 1.0));
        ph_ui_round_rect(r, (SDL_Rect){PAD, 118, track, 5}, 2, tile);
        if (fill > 0)
            ph_ui_round_rect(r, (SDL_Rect){PAD, 118, fill, 5}, 2, ink);
        if (c->item_size) {
            char got[32], all[32], line[96];
            format_size(got, sizeof got, c->item_bytes);
            format_size(all, sizeof all, c->item_size);
            snprintf(line, sizeof line, "%s of %s", got, all);
            ph_text_draw(r, line, PAD, 132, track, secondary_font, muted);
            snprintf(line, sizeof line, "%d%%", (int)(fraction * 100));
            int lw = ph_text_width(line, secondary_font);
            ph_text_draw(r, line, PAD + track - lw, 132, lw + 2, secondary_font, muted);
        }
    }
    if (c->received) {
        if (several(c))
            draw_button(r, c->more_r, c->expanded ? "Hide" : "Show all",
                        c->expanded ? (ph_control_icon)-1 : PH_ICON_DOWN, c->hot_more ? muted : ink, bg);
        else
            draw_button(r, c->open_r, "Open", PH_ICON_SEND, c->hot_open ? muted : ink, bg);
        draw_button(r, c->show_r, "Show in folder", PH_ICON_FOLDER,
                    c->hot_show ? hover : tile, ink);
    }
    for (int i = 0, rows = list_rows(c); i < rows; i++) {
        SDL_Rect row = row_rect(i);
        bool more = i == MAX_ROWS && c->item_count > MAX_ROWS;
        if (i == c->hot_row)
            ph_ui_round_rect(r, row, 8, hover);
        char line[200];
        if (more) {
            snprintf(line, sizeof line, "and %d more in the folder", c->item_count - MAX_ROWS);
            ph_text_draw(r, line, row.x + 34, row.y + (row.h - ph_text_line_height(secondary_font)) / 2,
                         row.w - 40, secondary_font, muted);
            continue;
        }
        const char *path = c->items[c->item_count - 1 - i]; /* newest first */
        const char *base = ph_path_base(path);
        ph_icon_draw(r, picture_name(base) ? PH_ICON_SHOT : PH_ICON_FILE,
                     (SDL_Rect){row.x + 8, row.y + (row.h - 18) / 2, 18, 18}, muted);
        fit_name(line, sizeof line, base, row.w - 44, secondary_font);
        ph_text_draw(r, line, row.x + 34, row.y + (row.h - ph_text_line_height(secondary_font)) / 2,
                     row.w - 40, secondary_font, ink);
    }
    SDL_RenderPresent(r);
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
    wchar_t *wide = ph_wide(path);
    if (wide) {
        ShellExecuteW(NULL, L"open", wide, NULL, NULL, SW_SHOWNORMAL);
        free(wide);
    }
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


static void hide_card(ph_transfer_card *c)
{
    c->phase = PHASE_HIDDEN;
    c->op_milli = 0;
    c->inside = false;
    c->hot_open = c->hot_show = c->hot_close = c->hot_more = false;
    c->hot_row = -1;
    c->expanded = false;
    if (c->arrow)
        SDL_SetCursor(c->arrow);
    if (c->window)
        SDL_HideWindow(c->window);
    fit_height(c);
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
                   | SDL_WINDOW_SKIP_TASKBAR | SDL_WINDOW_TOOLTIP | SDL_WINDOW_ALLOW_HIGHDPI;

    if (!c)
        return NULL;
    c->h = CARD_H;
    c->hot_row = -1;
    c->window = SDL_CreateWindow("Porthole", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, CARD_W,
                                 CARD_H, flags);
    if (!c->window)
        goto fail;
    c->renderer = SDL_CreateRenderer(c->window, -1, SDL_RENDERER_SOFTWARE);
    if (!c->renderer || !ph_text_attach(c->renderer))
        goto fail;
    SDL_Surface *mark = SDL_CreateRGBSurfaceWithFormat(
        0, ph_brand_icon_w, ph_brand_icon_h, 32, SDL_PIXELFORMAT_RGBA32);
    if (mark) {
        for (int y = 0; y < mark->h; y++)
            memcpy((unsigned char *)mark->pixels + y * mark->pitch,
                   ph_brand_icon + y * mark->w * 4, (size_t)mark->w * 4);
        c->logo = SDL_CreateTextureFromSurface(c->renderer, mark);
        if (c->logo)
            SDL_SetTextureScaleMode(c->logo, SDL_ScaleModeLinear);
        for (int y = 0; y < mark->h; y++) {
            unsigned char *row = (unsigned char *)mark->pixels + y * mark->pitch;
            for (int x = 0; x < mark->w; x++, row += 4)
                for (int channel = 0; channel < 3; channel++)
                    row[channel] = (unsigned char)(255 - row[channel]);
        }
        c->light_logo = SDL_CreateTextureFromSurface(c->renderer, mark);
        if (c->light_logo)
            SDL_SetTextureScaleMode(c->light_logo, SDL_ScaleModeLinear);
        SDL_FreeSurface(mark);
    }
    c->hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    c->arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    c->opacity_ok = true;
    c->placed_y = -100000;
    shape_window(c->window, c->h);
    return c;

fail:
    ph_transfer_card_destroy(c);
    return NULL;
}

void ph_transfer_card_destroy(ph_transfer_card *c)
{
    if (!c)
        return;
    if (c->renderer) {
        ph_text_detach(c->renderer);
        ph_icon_release(c->renderer);
        ph_ui_release_drawing(c->renderer);
        if (c->logo)
            SDL_DestroyTexture(c->logo);
        if (c->light_logo)
            SDL_DestroyTexture(c->light_logo);
        SDL_DestroyRenderer(c->renderer);
    }
    if (c->window)
        SDL_DestroyWindow(c->window);
    if (c->hand)
        SDL_FreeCursor(c->hand);
    if (c->arrow)
        SDL_FreeCursor(c->arrow);
    free(c->items);
    free(c);
}

Uint32 ph_transfer_card_id(const ph_transfer_card *c)
{
    return c ? SDL_GetWindowID(c->window) : 0;
}

void ph_transfer_card_set_theme(ph_transfer_card *c, bool dark)
{
    if (c && c->dark != dark) {
        c->dark = dark;
        c->dirty = true;
    }
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
    c->batch = false;
    c->item_count = 0;
    c->expanded = false;
    if (path && path[0]) {
        snprintf(c->path, sizeof c->path, "%s", path);
        copy_name(c, c->path);
    } else {
        c->path[0] = 0;
        c->name[0] = 0;
    }
    c->done = done;
    c->total = total;
    c->can_show = can_show;
    c->action_failed = false;
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
        place_opacity(c, c->rest_y + ph_display_units(SLIDE_PX, c->content_scale), 0);
        SDL_ShowWindow(c->window);
        shape_window(c->window, c->h);
    }
    fit_height(c);
    paint(c, now);
    if (c->received)
        PH_LOGI("transfer: received %s", c->name[0] ? c->name : "a file");
    else
        PH_LOGI("transfer: receiving %d of %d", done, total);
}

static void appear(ph_transfer_card *c, Uint64 now)
{
    if (c->phase == PHASE_HIDDEN || c->phase == PHASE_OUT) {
        c->expanded = false;
        c->h = CARD_H;
        rest_on_display(c);
        c->phase = PHASE_IN;
        c->phase_at = now;
        c->placed_y = -100000;
        place_opacity(c, c->rest_y + ph_display_units(SLIDE_PX, c->content_scale), 0);
        SDL_ShowWindow(c->window);
        shape_window(c->window, c->h);
    }
    fit_height(c);
    paint(c, now);
}

void ph_transfer_card_receiving(ph_transfer_card *c, const char *name, unsigned long long bytes,
                                unsigned long long size, int done, int total, Uint64 now)
{
    if (!c || !c->window)
        return;
    if (!c->batch || c->received || (done == 0 && (!name || !name[0]))) {
        /* A new batch. */
        c->item_count = 0;
        c->batch = true;
        c->received = false;
        c->expanded = false;
        c->check_at = 0;
        c->path[0] = 0;
        c->name[0] = 0;
    }
    if (name && name[0])
        snprintf(c->current, sizeof c->current, "%s", name);
    c->item_bytes = bytes;
    c->item_size = size;
    c->done = SDL_max(done, c->item_count);
    c->total = SDL_max(total, c->done);
    c->touch = now;
    c->dirty = true;
    appear(c, now);
}

void ph_transfer_card_saved(ph_transfer_card *c, const char *path, int total, bool can_show, Uint64 now)
{
    if (!c || !c->window || !path || !path[0])
        return;
    if (!c->batch || c->received) {
        c->item_count = 0;
        c->batch = true;
        c->expanded = false;
    }
    if (c->item_count == c->item_cap) {
        int cap = c->item_cap ? c->item_cap * 2 : 8;
        char(*grown)[1024] = realloc(c->items, (size_t)cap * sizeof *grown);
        if (!grown)
            return;
        c->items = grown;
        c->item_cap = cap;
    }
    snprintf(c->items[c->item_count++], sizeof c->items[0], "%s", path);
    snprintf(c->path, sizeof c->path, "%s", path);
    copy_name(c, c->path);
    c->done = c->item_count;
    c->total = SDL_max(total, c->item_count);
    c->item_bytes = c->item_size = 0;
    c->can_show = can_show;
    c->action_failed = false;
    c->received = c->done >= c->total;
    if (c->received) {
        c->check_at = now;
        if (c->item_count > 1)
            PH_LOGI("transfer: received %d items", c->item_count);
        else
            PH_LOGI("transfer: received %s", c->name[0] ? c->name : "a file");
    }
    c->touch = now;
    c->dirty = true;
    appear(c, now);
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
        y = c->rest_y + (int)((1.0f - t) * SLIDE_PX * c->content_scale);
        opacity = t;
        if (now - c->phase_at >= IN_MS) {
            c->phase = PHASE_HOLD;
            c->phase_at = now;
            y = c->rest_y;
            opacity = 1;
        }
    } else if (c->phase == PHASE_OUT) {
        t = ease(now - c->phase_at, OUT_MS);
        y = c->rest_y + (int)(t * SLIDE_PX * c->content_scale);
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
    popping = c->check_at != 0;
    if (popping && now - c->check_at >= POP_MS) {
        c->check_at = 0;
        c->dirty = true;
    }
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
            c->hot_open = c->hot_show = c->hot_close = c->hot_more = false;
            c->hot_row = -1;
            if (c->arrow)
                SDL_SetCursor(c->arrow);
            c->dirty = true;
            paint(c, now);
        } else if (event->window.event == SDL_WINDOWEVENT_EXPOSED
                   || event->window.event == SDL_WINDOWEVENT_SHOWN
                   || event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED
                   || event->window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) {
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
    x = ph_display_point(x, c->content_scale);
    y = ph_display_point(y, c->content_scale);
    c->inside = true;
    c->touch = now;
    open = hit(c->open_r, x, y);
    show = hit(c->show_r, x, y);
    close = hit(c->close_r, x, y);
    bool more = hit(c->more_r, x, y);
    int row = -1;
    for (int i = 0, rows = list_rows(c); i < rows; i++)
        if (hit(row_rect(i), x, y))
            row = i;
    changed = open != c->hot_open || show != c->hot_show || close != c->hot_close || more != c->hot_more ||
              row != c->hot_row;
    c->hot_open = open;
    c->hot_show = show;
    c->hot_close = close;
    c->hot_more = more;
    c->hot_row = row;
    if (c->hand && c->arrow)
        SDL_SetCursor((open || show || close || more || row >= 0) ? c->hand : c->arrow);
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
        } else if (more) {
            c->expanded = !c->expanded;
            c->hot_row = -1;
            fit_height(c);
            c->dirty = true;
            paint(c, now);
            c->touch = now;
        } else if (row >= 0) {
            if (row < MAX_ROWS)
                open_external(c->items[c->item_count - 1 - row]);
            else
                c->action_failed = !ph_place_reveal(c->path, true);
            c->touch = now;
        } else if (show) {
            c->action_failed = !ph_place_reveal(c->path, true);
            c->dirty = true;
            paint(c, now);
            c->touch = now;
        }
    }
    return true;
}
