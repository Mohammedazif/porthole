#include "ui/window.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#include "core/log.h"
#include "ui/brand_art.h"
#include "ui/placement.h"

#define RESIZE_SETTLE_MS 400 /* a new picture size must last this long before resizing */
#define WAITING_W 360        /* phone-shaped window before the first frame */
#define WAITING_H 780

struct ph_window {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int tex_w, tex_h;
    SDL_YUV_CONVERSION_MODE yuv_mode;
    bool has_picture;
    SDL_Rect screen;          /* phone-screen part of the texture */
    int fit_w, fit_h;         /* screen size the window was last fitted to */
    int pending_w, pending_h; /* a different size waiting to settle */
    Uint64 pending_since;
    bool mirroring, control_ready, help_visible;
    char control_detail[96];
    ph_setup setup;
    bool diagnostics_visible;
    ph_metrics_snapshot metrics;
    unsigned int queued_packets;
    uint64_t discarded_packets, superseded_frames;
    SDL_Texture *lockup;
    SDL_Texture *mark;
    char title[160];
    char pull_hint[96];
    int chrome_hover;
    int chrome_press;
    bool chrome_down;
    bool chrome_drag;
    int drag_dx, drag_dy;
};

static void place_window(ph_window *w, int pic_w, int pic_h, bool center);

/* Small built-in font keeps setup guidance available without an extra font package.
 * Each row contains five pixels. */
static const struct {
    char c;
    uint8_t rows[7];
} font[] = {
    {'A', {14,17,17,31,17,17,17}}, {'B', {30,17,17,30,17,17,30}},
    {'C', {14,17,16,16,16,17,14}}, {'D', {30,17,17,17,17,17,30}},
    {'E', {31,16,16,30,16,16,31}}, {'F', {31,16,16,30,16,16,16}},
    {'G', {14,17,16,23,17,17,15}}, {'H', {17,17,17,31,17,17,17}},
    {'I', {14,4,4,4,4,4,14}}, {'J', {7,2,2,2,18,18,12}},
    {'K', {17,18,20,24,20,18,17}}, {'L', {16,16,16,16,16,16,31}},
    {'M', {17,27,21,21,17,17,17}}, {'N', {17,25,21,19,17,17,17}},
    {'O', {14,17,17,17,17,17,14}}, {'P', {30,17,17,30,16,16,16}},
    {'Q', {14,17,17,17,21,18,13}}, {'R', {30,17,17,30,20,18,17}},
    {'S', {15,16,16,14,1,1,30}}, {'T', {31,4,4,4,4,4,4}},
    {'U', {17,17,17,17,17,17,14}}, {'V', {17,17,17,17,17,10,4}},
    {'W', {17,17,17,21,21,21,10}}, {'X', {17,17,10,4,10,17,17}},
    {'Y', {17,17,10,4,4,4,4}}, {'Z', {31,1,2,4,8,16,31}},
    {'0', {14,17,19,21,25,17,14}}, {'1', {4,12,4,4,4,4,14}},
    {'2', {14,17,1,2,4,8,31}}, {'3', {30,1,1,14,1,1,30}},
    {'4', {2,6,10,18,31,2,2}}, {'5', {31,16,16,30,1,1,30}},
    {'6', {14,16,16,30,17,17,14}}, {'7', {31,1,2,4,8,8,8}},
    {'8', {14,17,17,14,17,17,14}}, {'9', {14,17,17,15,1,1,14}},
    {'.', {0,0,0,0,0,12,12}}, {',', {0,0,0,0,0,4,8}},
    {':', {0,12,12,0,12,12,0}}, {';', {0,12,12,0,4,4,8}},
    {'-', {0,0,0,31,0,0,0}}, {'+', {0,4,4,31,4,4,0}},
    {'/', {1,2,2,4,8,8,16}}, {'>', {16,8,4,2,4,8,16}},
    {'<', {1,2,4,8,4,2,1}}, {'(', {2,4,8,8,8,4,2}},
    {')', {8,4,2,2,2,4,8}}, {'?', {14,17,1,2,4,0,4}},
    {'!', {4,4,4,4,4,0,4}}, {'=', {0,31,0,31,0,0,0}},
    {'[', {14,8,8,8,8,8,14}}, {']', {14,2,2,2,2,2,14}},
    {'\'', {4,4,8,0,0,0,0}},
    {'a', {0,0,14,17,15,17,15}}, {'b', {16,16,30,17,17,17,30}},
    {'c', {0,0,14,16,16,16,14}}, {'d', {1,1,15,17,17,17,15}},
    {'e', {0,0,14,17,31,16,14}}, {'f', {6,9,8,28,8,8,8}},
    {'g', {0,0,15,17,15,1,14}}, {'h', {16,16,30,17,17,17,17}},
    {'i', {4,0,12,4,4,4,14}}, {'j', {2,0,6,2,2,18,12}},
    {'k', {16,16,18,20,24,20,18}}, {'l', {12,4,4,4,4,4,14}},
    {'m', {0,0,26,21,21,21,21}}, {'n', {0,0,30,17,17,17,17}},
    {'o', {0,0,14,17,17,17,14}}, {'p', {0,0,30,17,30,16,16}},
    {'q', {0,0,15,17,15,1,1}}, {'r', {0,0,22,25,16,16,16}},
    {'s', {0,0,15,16,14,1,30}}, {'t', {4,4,14,4,4,5,2}},
    {'u', {0,0,17,17,17,19,13}}, {'v', {0,0,17,17,17,10,4}},
    {'w', {0,0,17,17,21,21,10}}, {'x', {0,0,17,10,4,10,17}},
    {'y', {0,0,17,17,15,1,14}}, {'z', {0,0,31,2,4,8,31}},
};

static const uint8_t *glyph(char c)
{
    for (size_t i = 0; i < sizeof font / sizeof font[0]; i++)
        if (font[i].c == c)
            return font[i].rows;
    return NULL;
}

static void draw_glyph(SDL_Renderer *renderer, char c, int x, int y, int scale)
{
    const uint8_t *rows = glyph(c);
    if (!rows)
        return;
    SDL_Rect pixels[35];
    int count = 0;
    for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++)
            if (rows[row] & (1u << (4 - col)))
                pixels[count++] = (SDL_Rect){x + col * scale, y + row * scale, scale, scale};
    SDL_RenderFillRects(renderer, pixels, count);
}

/* Walk wrapped text the same way draw_text does. Returns the height in pixels. */
static int text_height(const char *text, int width, int scale)
{
    int x = 0, y = 0, step, line;

    if (scale < 1)
        scale = 1;
    step = 6 * scale;
    line = 10 * scale;
    if (!text || width < step)
        return line;
    while (*text) {
        if (*text == '\n') {
            x = 0;
            y += line;
            text++;
            continue;
        }
        if (*text == ' ') {
            if (x != 0)
                x += step;
            text++;
            continue;
        }
        size_t word = strcspn(text, " \n");
        if (x != 0 && x + (int)word * step > width) {
            x = 0;
            y += line;
        }
        for (size_t i = 0; i < word; i++) {
            if (x + 5 * scale > width && x != 0) {
                x = 0;
                y += line;
            }
            x += step;
        }
        text += word;
    }
    return y + line;
}

/* Draw wrapped text without changing the renderer's scale or viewport; those are
 * shared with the video/coordinate mapping. Returns the next available line. */
static int draw_text(SDL_Renderer *renderer, const char *text, int x, int y,
                     int width, int scale, SDL_Color color)
{
    int start_x = x, right = x + width, step = 6 * scale, line = 10 * scale;
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    while (*text) {
        if (*text == '\n') {
            x = start_x;
            y += line;
            text++;
            continue;
        }
        if (*text == ' ') {
            if (x != start_x)
                x += step;
            text++;
            continue;
        }
        size_t word = strcspn(text, " \n");
        if (x != start_x && x + (int)word * step > right) {
            x = start_x;
            y += line;
        }
        for (size_t i = 0; i < word; i++) {
            if (x + 5 * scale > right && x != start_x) {
                x = start_x;
                y += line;
            }
            draw_glyph(renderer, text[i], x, y, scale);
            x += step;
        }
        text += word;
    }
    return y + line;
}

static int text_scale(int width, int height)
{
    int scale = width / 160;
    int by_height = height / 280;

    if (by_height < scale)
        scale = by_height;
    if (scale < 1)
        scale = 1;
    if (scale > 3)
        scale = 3;
    return scale;
}

static bool wide_layout(int width, int height)
{
    return width > height + height / 5;
}

static const SDL_Color color_text = {17, 17, 17, 255};
static const SDL_Color color_muted = {104, 107, 112, 255};
static const SDL_Color color_accent = {17, 17, 17, 255};
static const SDL_Color color_warn = {154, 52, 18, 255};
static const SDL_Color color_on_ink = {255, 255, 255, 255};

#ifdef _WIN32
#define PH_PAIR_LINE "Pair this PC's Bluetooth name."
#else
#define PH_PAIR_LINE "Pair Porthole in Bluetooth."
#endif

static void paint(SDL_Renderer *renderer, SDL_Rect rect, int r, int g, int b, int a)
{
    SDL_SetRenderDrawBlendMode(renderer, a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a);
    SDL_RenderFillRect(renderer, &rect);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

static int draw_card(SDL_Renderer *renderer, SDL_Rect card, int scale, const char *title,
                     const char *body)
{
    int pad = 8 * scale;
    int inner = card.w - 2 * pad;

    if (inner < 6 * scale)
        return card.y;
    paint(renderer, card, 255, 255, 255, 255);
    SDL_SetRenderDrawColor(renderer, 222, 223, 220, 255);
    SDL_RenderDrawRect(renderer, &card);
    SDL_Rect old_clip;
    SDL_bool clipped = SDL_RenderIsClipEnabled(renderer);
    SDL_RenderGetClipRect(renderer, &old_clip);
    SDL_RenderSetClipRect(renderer, &card);
    int y = draw_text(renderer, title, card.x + pad, card.y + pad, inner, scale, color_text);
    y = draw_text(renderer, body, card.x + pad, y, inner, scale, color_text);
    SDL_RenderSetClipRect(renderer, clipped ? &old_clip : NULL);
    return y;
}

static void draw_key_hints(SDL_Renderer *renderer, int x, int y, int width, int scale)
{
    const char *line = "F1 help   F2 setup   F3";
    int use = scale;
    int chars = (int)strlen(line);

    while (use > 1 && chars * 6 * use > width)
        use--;
    draw_text(renderer, line, x, y, width, use, color_muted);
}

static void draw_waiting(ph_window *w, int width, int height, int top)
{
    int scale = text_scale(width, height);
    int margin = 14 * scale;
    int gap = 8 * scale;
    bool wide = wide_layout(width, height);
    int card_h = 7 * 10 * scale;
    int stack_h = 10 * (scale + 1) + gap + (wide ? card_h : card_h * 2 + gap) + gap +
                  (wide ? 12 : 24) * scale;
    int area = height - top;
    int y = top + (area - stack_h) / 2;
    char status[160];
    SDL_Rect screen = {0, 0, width, height};
    const char *control = PH_PAIR_LINE "\nAssistiveTouch on.\nDwell Control off.";
    const char *mirror = "Same Wi-Fi as this PC.\nControl Center.\nScreen Mirroring > Porthole.";

    paint(w->renderer, screen, 245, 244, 241, 255);
    if (width < margin * 2 + 6 * scale)
        return;
    if (y < top + margin)
        y = top + margin;
    y = draw_text(w->renderer, "iPhone on this PC", margin, y, width - 2 * margin, scale + 1,
                  color_text);
    y += gap;
    if (wide) {
        int col = (width - 2 * margin - gap) / 2;
        SDL_Rect left = {margin, y, col, card_h};
        SDL_Rect right = {margin + col + gap, y, width - 2 * margin - col - gap, card_h};
        draw_card(w->renderer, left, scale, "Screen", mirror);
        draw_card(w->renderer, right, scale, "Controls", control);
        y += card_h + gap;
        snprintf(status, sizeof status, "Controls: %s",
                 w->control_ready ? "connected" : w->control_detail[0] ? w->control_detail : "waiting");
        draw_text(w->renderer, status, margin, y, width / 2 - margin, scale, color_accent);
        draw_key_hints(w->renderer, width / 2, y, width / 2 - margin, scale);
    } else {
        if (y + card_h * 2 + gap > height - margin)
            card_h = (height - margin - y - gap) / 2;
        if (card_h < 20 * scale)
            card_h = 20 * scale;
        SDL_Rect upper = {margin, y, width - 2 * margin, card_h};
        SDL_Rect bottom = {margin, y + card_h + gap, width - 2 * margin, card_h};
        draw_card(w->renderer, upper, scale, "Screen", mirror);
        draw_card(w->renderer, bottom, scale, "Controls", control);
        y += card_h * 2 + gap * 2;
        snprintf(status, sizeof status, "Controls: %s",
                 w->control_ready ? "connected"
                                  : w->control_detail[0] ? w->control_detail : "waiting");
        y = draw_text(w->renderer, status, margin, y, width - 2 * margin, scale, color_accent);
        draw_key_hints(w->renderer, margin, y, width - 2 * margin, scale);
    }
}

static int card_height(const char *title, const char *body, int inner, int scale)
{
    int pad = 8 * scale;

    return pad + text_height(title, inner, scale) + text_height(body, inner, scale) + pad;
}

static void measure_help(int width, int scale, bool columns, const char *mouse, const char *keys,
                         const char *setup, int *margin, int *gap, int *mouse_h, int *keys_h,
                         int *total)
{
    int inner, head, foot, block;

    *margin = 12 * scale;
    *gap = 8 * scale;
    if (columns)
        inner = (width - 2 * *margin - *gap) / 2 - 16 * scale;
    else
        inner = width - 2 * *margin - 16 * scale;
    if (inner < 6 * scale)
        inner = 6 * scale;
    *mouse_h = card_height("Mouse", mouse, inner, scale);
    *keys_h = card_height("Keyboard", keys, inner, scale);
    head = *margin + text_height("Help", width - 2 * *margin, scale + 1) +
           text_height("F1 closes", width - 2 * *margin, scale) + *gap;
    foot = *gap + text_height(setup, width - 2 * *margin, scale) + *margin;
    block = columns ? (*mouse_h > *keys_h ? *mouse_h : *keys_h) : (*mouse_h + *gap + *keys_h);
    *total = head + block + foot;
}

static void draw_help(ph_window *w, int width, int height, int top)
{
    char mouse[700];

    snprintf(mouse, sizeof mouse,
             "Move cursor to point.\n"
             "Left click: tap\n"
             "Right: secondary tap\n"
             "Drag: swipe\n"
             "Drag a photo off the window\n"
             "to take its file.\n"
             "Wheel: scroll\n"
             "Shift+wheel: sideways\n"
             "Middle click: action\n"
             "Side buttons: actions\n"
             "F4: feed wheel on/off%s%s",
             w->pull_hint[0] ? "\n" : "", w->pull_hint);
    const char *keys = "Type directly.\n"
                       "Ctrl is Command.\n"
                       "Alt+H Home\n"
                       "Alt+F Search\n"
                       "Alt+S Apps\n"
                       "Alt+C Controls\n"
                       "Alt+N Notifications\n"
                       "Alt+P Screenshot\n"
                       "Alt+Left: Back\n"
                       "Alt+Up, Down: volume\n"
                       "Alt+PgUp, PgDn: swipe\n"
                       "Alt+Shift+arrow swipe\n"
                       "Alt+D: double-tap\n"
                       "Alt+L: hold\n"
                       "Alt+U: bottom swipe\n"
                       "Alt+K: on or off\n"
                       "PC keys use Bluetooth\n"
                       "Esc cancels a hold\n"
                       "Media keys: playback";
    const char *setup = "AssistiveTouch on. Dwell off.\n"
                        "Alt+K shows or hides the iPhone keyboard.\n"
                        "PC keys stay on Bluetooth.\n"
                        PH_PAIR_LINE "\n"
                        "Buttons 3-6: Apps, Controls,\n"
                        "Notifications, Screenshot.";
    int content_h = height - top;
    int scale = text_scale(width, content_h);
    bool columns = false;
    int margin = 12, gap = 8;
    int mouse_h = 0, keys_h = 0;
    int chosen_scale = 1;
    bool found = false;
    SDL_Rect panel = {0, top, width, content_h};
    SDL_Rect old_clip;
    SDL_bool clipped;

    if (content_h < 8 || width < 8)
        return;
    paint(w->renderer, panel, 245, 244, 241, 255);
    for (int s = scale; s >= 1 && !found; s--) {
        int total;

        measure_help(width, s, false, mouse, keys, setup, &margin, &gap, &mouse_h, &keys_h,
                     &total);
        if (total <= content_h) {
            chosen_scale = s;
            columns = false;
            found = true;
        }
    }
    if (!found) {
        for (int s = scale; s >= 1 && !found; s--) {
            int total, col_inner;

            measure_help(width, s, true, mouse, keys, setup, &margin, &gap, &mouse_h, &keys_h,
                         &total);
            col_inner = (width - 2 * margin - gap) / 2 - 16 * s;
            if (col_inner >= 21 * 6 * s && total <= content_h) {
                chosen_scale = s;
                columns = true;
                found = true;
            }
        }
    }
    if (!found) {
        int stack_m, stack_g, stack_mouse, stack_keys, stack_total;
        int col_m, col_g, col_mouse, col_keys, col_total;

        measure_help(width, 1, false, mouse, keys, setup, &stack_m, &stack_g, &stack_mouse,
                     &stack_keys, &stack_total);
        measure_help(width, 1, true, mouse, keys, setup, &col_m, &col_g, &col_mouse, &col_keys,
                     &col_total);
        chosen_scale = 1;
        if (col_total < stack_total) {
            columns = true;
            margin = col_m;
            gap = col_g;
            mouse_h = col_mouse;
            keys_h = col_keys;
        } else {
            columns = false;
            margin = stack_m;
            gap = stack_g;
            mouse_h = stack_mouse;
            keys_h = stack_keys;
        }
    }
    scale = chosen_scale;
    SDL_RenderGetClipRect(w->renderer, &old_clip);
    clipped = SDL_RenderIsClipEnabled(w->renderer);
    SDL_RenderSetClipRect(w->renderer, &panel);
    int y = draw_text(w->renderer, "Help", margin, top + margin, width - 2 * margin, scale + 1,
                      color_accent);
    y = draw_text(w->renderer, "F1 closes", margin, y, width - 2 * margin, scale, color_muted);
    y += gap;
    if (columns) {
        int col = (width - 2 * margin - gap) / 2;
        int card_h = mouse_h > keys_h ? mouse_h : keys_h;
        SDL_Rect left = {margin, y, col, card_h};
        SDL_Rect right = {margin + col + gap, y, width - 2 * margin - col - gap, card_h};
        draw_card(w->renderer, left, scale, "Mouse", mouse);
        draw_card(w->renderer, right, scale, "Keyboard", keys);
        y += card_h + gap;
    } else {
        SDL_Rect card = {margin, y, width - 2 * margin, mouse_h};
        draw_card(w->renderer, card, scale, "Mouse", mouse);
        y += mouse_h + gap;
        card.y = y;
        card.h = keys_h;
        draw_card(w->renderer, card, scale, "Keyboard", keys);
        y += keys_h + gap;
    }
    draw_text(w->renderer, setup, margin, y, width - 2 * margin, scale, color_muted);
    SDL_RenderSetClipRect(w->renderer, clipped ? &old_clip : NULL);
}

static void setup_buttons(int width, int height, SDL_Rect buttons[3])
{
    int scale = text_scale(width, height);
    int margin = 12 * scale;
    int gap = 6 * scale;
    int button_h = 28 * scale;
    int button_width = (width - 2 * margin - 2 * gap) / 3;

    if (button_width < scale)
        button_width = scale;
    for (int i = 0; i < 3; i++)
        buttons[i] = (SDL_Rect){margin + i * (button_width + gap), height - margin - button_h,
                                button_width, button_h};
}

static void draw_setup(ph_window *w, int width, int height, int top)
{
    const ph_setup *s = &w->setup;
    int scale = text_scale(width, height), margin = 12 * scale, content_width = width - 2 * margin;
    SDL_Rect panel = {0, 0, width, height};
    paint(w->renderer, panel, 245, 244, 241, 255);
    SDL_Rect buttons[3];
    setup_buttons(width, height, buttons);
    SDL_Rect clip = {margin, top + margin, content_width, buttons[0].y - top - 2 * margin};
    if (clip.h < 1)
        clip.h = 1;
    SDL_RenderSetClipRect(w->renderer, &clip);
    char heading[80];
    snprintf(heading, sizeof heading, "Step %d of %d", s->controls ? (int)s->step + 1
                                                                   : s->step == PH_SETUP_MIRROR ? 1 : 2,
             s->controls ? 5 : 2);
    int y = draw_text(w->renderer, heading, margin, top + margin, content_width, scale + 1,
                      color_accent);
    y += 8 * scale;
    const char *title = "", *body = "";
    switch (s->step) {
    case PH_SETUP_PREPARE:
        title = "Prepare your iPhone";
        body = "Settings > Accessibility > Touch > AssistiveTouch.\n"
               "Turn AssistiveTouch on.\nTurn Dwell Control off.\n"
               "Leave Show Onscreen Keyboard off.\n\n"
               "Alt+K shows or hides the iPhone keyboard.\n"
               "PC keys use Bluetooth.\n"
               "Leave Tracking Sensitivity as it is.\nPress Next when that is done.";
        break;
    case PH_SETUP_BLUETOOTH:
        title = "Connect Bluetooth";
#ifdef _WIN32
        body = "On iPhone: Settings > Bluetooth.\n"
               "Select this PC's Bluetooth name under Other Devices.\n\n"
               "Accept the Windows pairing prompt.\n"
               "If pairing fails, remove the old entry on both devices and pair again.";
#else
        body = "On iPhone: Settings > Bluetooth.\n"
               "Select Porthole under Other Devices.\n\n"
               "Wait until the controls connect.\n"
               "If pairing fails, forget the old Porthole entry and pair again.";
#endif
        break;
    case PH_SETUP_MIRROR:
        title = "Mirror your screen";
        body = "Use the same Wi-Fi network.\n"
               "Control Center > Screen Mirroring > Porthole.\n\n"
               "Close the iPhone's mirroring panel once the picture appears.";
        break;
    case PH_SETUP_CALIBRATE:
        title = "Check pointer alignment";
        body = "Leave the iPhone on a still page, such as Calculator.\n"
               "The pointer moves to three targets. No taps are sent.\n\n"
               "Do not touch the phone during the check. The result is saved only after all three targets match.";
        break;
    case PH_SETUP_FINISH:
        title = "Ready to use";
        body = s->controls
                   ? "Point, click, drag, and type through the mirror.\n"
                     "F1 lists keys. F2 opens this guide again.\n\n"
                     "Optional: assign AssistiveTouch buttons 3 to 6 to App Switcher, Control Center, Notifications, and Screenshot."
                   : "The iPhone screen is connected.\nThis window is viewing only.\nF2 opens this guide again.";
        break;
    }
    y = draw_text(w->renderer, title, margin, y, content_width, scale, color_accent);
    y = draw_text(w->renderer, body, margin, y + 6 * scale, content_width, scale, color_text);
    y += 8 * scale;
    char status[128] = "";
    if (s->step == PH_SETUP_BLUETOOTH)
        snprintf(status, sizeof status, "Controls: %s", s->bluetooth_ready ? "connected" : w->control_detail);
    else if (s->step == PH_SETUP_MIRROR)
        snprintf(status, sizeof status, "Screen: %s", s->mirroring ? "connected" : "waiting for iPhone");
    else if (s->step == PH_SETUP_CALIBRATE)
        snprintf(status, sizeof status, "%s: %u/%d targets",
                 s->calibrated ? "Confirmed" : s->calibrating ? "Checking" : "Not checked",
                 s->target, PH_SETUP_TARGETS);
    y = draw_text(w->renderer, status, margin, y, content_width, scale, color_accent);
    if (s->error != PH_SETUP_OK) {
        const char *error = s->error == PH_SETUP_CONNECTION_LOST ? "Connection lost. Reconnect, then retry."
                            : s->error == PH_SETUP_CANCELED ? "Check paused. Press Start to retry."
                            : s->error == PH_SETUP_SAVE_FAILED ? "Could not save calibration. Check the configuration folder, then retry."
                            : "Pointer not confirmed. Check AssistiveTouch and Tracking Sensitivity, then retry.";
        draw_text(w->renderer, error, margin, y + 6 * scale, content_width, scale, color_warn);
    }
    SDL_RenderSetClipRect(w->renderer, NULL);
    const char *next = s->step == PH_SETUP_FINISH ? "Finish"
                       : s->calibrating ? "Wait"
                       : s->step == PH_SETUP_CALIBRATE && !s->calibrated
                             ? s->error != PH_SETUP_OK ? "Retry" : "Start" : "Next";
    const char *labels[] = {"Close", "Back", next};
    for (int i = 0; i < 3; i++) {
        if (i == 2)
            paint(w->renderer, buttons[i], 17, 17, 17, 255);
        else {
            paint(w->renderer, buttons[i], 255, 255, 255, 255);
            SDL_SetRenderDrawColor(w->renderer, 222, 223, 220, 255);
            SDL_RenderDrawRect(w->renderer, &buttons[i]);
        }
        int text_width = (int)strlen(labels[i]) * 6 * scale;
        int text_x = buttons[i].x + (buttons[i].w - text_width) / 2;
        int text_y = buttons[i].y + (buttons[i].h - 7 * scale) / 2;
        if (text_x < buttons[i].x)
            text_x = buttons[i].x;
        draw_text(w->renderer, labels[i], text_x, text_y, buttons[i].w, scale,
                  i == 2 ? color_on_ink : color_text);
    }
}

static void diagnostic_line(char out[160], const char *name, const ph_metric_summary *value)
{
    if (value->known)
        snprintf(out, 160, "%s: %.0f ms (p95 %.0f)", name, value->last_ms, value->p95_ms);
    else
        snprintf(out, 160, "%s: --", name);
}

static void draw_diagnostics(ph_window *w, int width, int height, int top)
{
    int scale = text_scale(width, height);
    int margin = 10 * scale;
    bool wide = wide_layout(width, height);
    int rows = wide ? 6 : 10;
    int panel_h = margin + rows * 10 * scale + margin;
    char lines[8][160];
    char note[] = "Local timings. The helper ACK excludes the radio and the phone.";
    SDL_Rect panel;
    int y, i;

    if (panel_h > height - top)
        panel_h = height - top;
    if (panel_h < 1)
        panel_h = 1;
    panel = (SDL_Rect){0, top, width, panel_h};
    paint(w->renderer, panel, 245, 244, 241, 255);
    if (w->metrics.fps_known)
        snprintf(lines[0], sizeof lines[0], "Video %.1f fps", w->metrics.fps);
    else
        snprintf(lines[0], sizeof lines[0], "Video fps --");
    diagnostic_line(lines[1], "Decode", &w->metrics.receive_to_decode);
    diagnostic_line(lines[2], "Present", &w->metrics.receive_to_present);
    diagnostic_line(lines[3], "Input", &w->metrics.input_handoff);
    diagnostic_line(lines[4], "Helper ACK", &w->metrics.helper_ack);
    if (w->metrics.bluetooth_interval_known)
        snprintf(lines[5], sizeof lines[5], "Bluetooth %.2f ms", w->metrics.bluetooth_interval_ms);
    else
        snprintf(lines[5], sizeof lines[5], "Bluetooth not measured");
    snprintf(lines[6], sizeof lines[6], "HID pending %zu, expired %llu", w->metrics.pending_acks,
             (unsigned long long)w->metrics.acks_expired);
    snprintf(lines[7], sizeof lines[7], "Queue %u, discarded %llu, replaced %llu", w->queued_packets,
             (unsigned long long)w->discarded_packets, (unsigned long long)w->superseded_frames);
    y = draw_text(w->renderer, "Timings    F3 closes", margin, top + margin, width - 2 * margin,
                  scale, color_accent);
    if (wide && width > 80 * scale) {
        int col = (width - 2 * margin) / 2;
        int left = y;
        int right = y;
        for (i = 0; i < 4; i++)
            left = draw_text(w->renderer, lines[i], margin, left, col - margin, scale, color_text);
        for (i = 4; i < 8; i++)
            right = draw_text(w->renderer, lines[i], margin + col, right, col - margin, scale,
                              color_text);
        y = left > right ? left : right;
    } else {
        for (i = 0; i < 8; i++)
            y = draw_text(w->renderer, lines[i], margin, y, width - 2 * margin, scale, color_text);
    }
    draw_text(w->renderer, note, margin, y, width - 2 * margin, scale, color_muted);
}

enum {
    CHROME_NONE = 0,
    CHROME_MIN = 1,
    CHROME_MAX = 2,
    CHROME_CLOSE = 3,
    CHROME_DRAG = 4
};

static SDL_Texture *texture_from_rgba(SDL_Renderer *renderer, const unsigned char *pixels,
                                      int width, int height)
{
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom((void *)pixels, width, height, 32,
                                                             width * 4, SDL_PIXELFORMAT_RGBA32);
    SDL_Texture *texture;

    if (!surface)
        return NULL;
    texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    if (texture)
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return texture;
}

static int bar_output_px(const ph_window *w, int out_h)
{
    int win_w = 1, win_h = 1, bar;

    SDL_GetWindowSize(w->window, &win_w, &win_h);
    (void)win_w;
    if (win_h < 1)
        win_h = 1;
    bar = PH_WINDOW_BAR_PX * out_h / win_h;
    if (bar < 1)
        bar = 1;
    if (out_h > 4 && bar > out_h - 4)
        bar = out_h / 4;
    return bar;
}

static void chrome_buttons(int win_w, SDL_Rect buttons[3])
{
    int btn = 32, gap = 8, y = (PH_WINDOW_BAR_PX - btn) / 2, x = win_w - 10 - btn;

    if (y < 0)
        y = 0;
    buttons[2] = (SDL_Rect){x, y, btn, btn};
    x -= gap + btn;
    buttons[1] = (SDL_Rect){x, y, btn, btn};
    x -= gap + btn;
    buttons[0] = (SDL_Rect){x, y, btn, btn};
}

static int chrome_at(int win_w, int x, int y)
{
    SDL_Rect buttons[3];
    SDL_Point point = {x, y};

    if (y < 0 || y >= PH_WINDOW_BAR_PX || x < 0 || x >= win_w)
        return CHROME_NONE;
    chrome_buttons(win_w, buttons);
    if (SDL_PointInRect(&point, &buttons[0]))
        return CHROME_MIN;
    if (SDL_PointInRect(&point, &buttons[1]))
        return CHROME_MAX;
    if (SDL_PointInRect(&point, &buttons[2]))
        return CHROME_CLOSE;
    return CHROME_DRAG;
}

static void fill_px(SDL_Renderer *renderer, int x, int y, int w, int h)
{
    SDL_Rect rect;

    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    rect = (SDL_Rect){x, y, w, h};
    SDL_RenderFillRect(renderer, &rect);
}

static void draw_chrome_glyph(SDL_Renderer *renderer, int kind, SDL_Rect box, int thick, bool on_ink)
{
    int cx = box.x + box.w / 2, cy = box.y + box.h / 2, arm = box.w / 5, i;

    if (arm < thick)
        arm = thick;
    SDL_SetRenderDrawColor(renderer, on_ink ? 255 : 17, on_ink ? 255 : 17, on_ink ? 255 : 17, 255);
    if (kind == CHROME_MIN) {
        fill_px(renderer, cx - arm, cy - thick / 2, arm * 2, thick);
        return;
    }
    if (kind == CHROME_MAX) {
        fill_px(renderer, cx - arm, cy - arm, arm * 2, thick);
        fill_px(renderer, cx - arm, cy + arm - thick, arm * 2, thick);
        fill_px(renderer, cx - arm, cy - arm, thick, arm * 2);
        fill_px(renderer, cx + arm - thick, cy - arm, thick, arm * 2);
        return;
    }
    for (i = -arm; i <= arm; i++) {
        fill_px(renderer, cx + i, cy + i - thick / 2, thick, thick);
        fill_px(renderer, cx + i, cy - i - thick / 2, thick, thick);
    }
}

static SDL_Rect scale_rect(SDL_Rect rect, int win_w, int win_h, int out_w, int out_h)
{
    if (win_w < 1)
        win_w = 1;
    if (win_h < 1)
        win_h = 1;
    rect.x = rect.x * out_w / win_w;
    rect.y = rect.y * out_h / win_h;
    rect.w = rect.w * out_w / win_w;
    if (rect.w < 1)
        rect.w = 1;
    rect.h = rect.h * out_h / win_h;
    if (rect.h < 1)
        rect.h = 1;
    return rect;
}

static void draw_bar(ph_window *w, int out_w, int out_h)
{
    int win_w = 1, win_h = 1, bar = bar_output_px(w, out_h), thick, pad, logo_h, logo_w;
    int buttons_w, room;
    bool status = w->has_picture && !w->control_ready && !w->help_visible && !w->setup.visible;
    SDL_Rect strip = {0, 0, out_w, bar}, hairline, buttons[3], logo;

    SDL_GetWindowSize(w->window, &win_w, &win_h);
    thick = win_h > 0 ? out_h / win_h : 1;
    if (thick < 1)
        thick = 1;
    paint(w->renderer, strip, 255, 255, 255, 255);
    hairline = (SDL_Rect){0, bar - thick, out_w, thick};
    paint(w->renderer, hairline, 222, 223, 220, 255);
    chrome_buttons(win_w, buttons);
    pad = bar / 8;
    if (pad < thick)
        pad = thick;
    buttons_w = out_w - buttons[0].x * out_w / (win_w < 1 ? 1 : win_w);
    if (buttons_w < 0)
        buttons_w = 0;
    room = out_w - buttons_w - pad * 3;
    logo_h = bar - pad * 2;
    if (logo_h < 1)
        logo_h = 1;
    logo_w = logo_h * ph_brand_lockup_w / ph_brand_lockup_h;
    if (logo_w > room)
        logo_w = room;
    if (logo_w < 1)
        logo_w = 1;
    logo_h = logo_w * ph_brand_lockup_h / ph_brand_lockup_w;
    if (logo_h < 1)
        logo_h = 1;
    if (!status && w->lockup && logo_w >= 160 * thick) {
        logo = (SDL_Rect){pad, (bar - logo_h) / 2, logo_w, logo_h};
        SDL_RenderCopy(w->renderer, w->lockup, NULL, &logo);
    } else {
        int side = logo_h > 32 * thick ? 32 * thick : logo_h;
        SDL_Rect icon = {pad, (bar - side) / 2, side, side};
        char line[180];
        int scale = 2 * thick, text_x, text_w;

        if (side < thick)
            side = thick;
        icon.w = icon.h = side;
        icon.y = (bar - side) / 2;
        if (w->mark)
            SDL_RenderCopy(w->renderer, w->mark, NULL, &icon);
        if (status) {
            text_x = icon.x + icon.w + pad;
            text_w = out_w - buttons_w - text_x - pad;
            while (scale > 1 && 8 * scale > bar - pad)
                scale--;
            snprintf(line, sizeof line, "Viewing only. %s  F2 setup",
                     w->control_detail[0] ? w->control_detail : "Controls disconnected.");
            if (text_w > 6 * scale)
                draw_text(w->renderer, line, text_x, (bar - 7 * scale) / 2, text_w, scale,
                          color_muted);
        }
    }
    for (int i = 0; i < 3; i++) {
        SDL_Rect box = scale_rect(buttons[i], win_w, win_h, out_w, out_h);
        int kind = i + 1;
        bool hot = w->chrome_hover == kind;

        if (hot)
            paint(w->renderer, box, kind == CHROME_CLOSE ? 17 : 239, kind == CHROME_CLOSE ? 17 : 239,
                  kind == CHROME_CLOSE ? 17 : 236, 255);
        draw_chrome_glyph(w->renderer, kind, box, thick, hot && kind == CHROME_CLOSE);
    }
}

static void toggle_zoom(SDL_Window *window)
{
    Uint32 flags = SDL_GetWindowFlags(window);

    if (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN_DESKTOP))
        SDL_RestoreWindow(window);
    else
        SDL_MaximizeWindow(window);
}

ph_window *ph_window_create(const char *title)
{
    ph_window *w = calloc(1, sizeof *w);

    if (!w)
        return NULL;
    w->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WAITING_W,
                                 WAITING_H,
                                 SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_BORDERLESS);
    snprintf(w->title, sizeof w->title, "%s", title ? title : "Porthole");
    if (!w->window)
        goto fail;
    place_window(w, WAITING_W, WAITING_H, true);
    /* No vsync: presenting a frame must not block this thread, which also forwards
     * mouse and keyboard input, for up to a whole display refresh. */
#if !defined(_WIN32)
    /* llvmpipe reports itself as accelerated OpenGL and otherwise starts one
     * raster thread per core for a single textured quad. */
    setenv("LP_NUM_THREADS", "4", 0);
#endif
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
    w->renderer = SDL_CreateRenderer(w->window, -1, SDL_RENDERER_ACCELERATED);
    if (!w->renderer) {
        PH_LOGW("no accelerated renderer (%s); using software rendering", SDL_GetError());
        w->renderer = SDL_CreateRenderer(w->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!w->renderer)
        goto fail;
    {
        SDL_RendererInfo info;
        if (SDL_GetRendererInfo(w->renderer, &info) == 0)
            PH_LOGI("renderer %s (%s)", info.name,
                    (info.flags & SDL_RENDERER_ACCELERATED) ? "gpu" : "software");
    }
    w->lockup = texture_from_rgba(w->renderer, ph_brand_lockup, ph_brand_lockup_w, ph_brand_lockup_h);
    w->mark = texture_from_rgba(w->renderer, ph_brand_icon, ph_brand_icon_w, ph_brand_icon_h);
    {
        SDL_Surface *icon = SDL_CreateRGBSurfaceWithFormatFrom(
            (void *)ph_brand_icon, ph_brand_icon_w, ph_brand_icon_h, 32, ph_brand_icon_w * 4,
            SDL_PIXELFORMAT_RGBA32);
        if (icon) {
            SDL_SetWindowIcon(w->window, icon);
            SDL_FreeSurface(icon);
        }
    }
    SDL_SetWindowMinimumSize(w->window, 320, 480);
    ph_window_redraw(w);
    return w;

fail:
    PH_LOGE("couldn't create the window: %s", SDL_GetError());
    ph_window_destroy(w);
    return NULL;
}

void ph_window_set_title(ph_window *w, const char *title)
{
    snprintf(w->title, sizeof w->title, "%s", title ? title : "");
    SDL_SetWindowTitle(w->window, w->title);
}

void ph_window_set_status(ph_window *w, bool mirroring, bool control_ready, const char *detail)
{
    if (w->mirroring == mirroring && w->control_ready == control_ready
        && !strcmp(w->control_detail, detail ? detail : ""))
        return;
    w->mirroring = mirroring;
    w->control_ready = control_ready;
    snprintf(w->control_detail, sizeof w->control_detail, "%s", detail ? detail : "");
    ph_window_redraw(w);
}

void ph_window_set_detail(ph_window *w, const char *detail)
{
    if (!w)
        return;
    ph_window_set_status(w, w->mirroring, w->control_ready, detail);
}

void ph_window_set_pull_hint(ph_window *w, const char *hint)
{
    if (!w)
        return;
    snprintf(w->pull_hint, sizeof w->pull_hint, "%s", hint ? hint : "");
    if (w->help_visible)
        ph_window_redraw(w);
}

void ph_window_toggle_help(ph_window *w)
{
    w->help_visible = !w->help_visible;
    ph_window_redraw(w);
}

bool ph_window_help_visible(const ph_window *w)
{
    return w->help_visible;
}

void ph_window_set_setup(ph_window *w, const ph_setup *setup)
{
    if (memcmp(&w->setup, setup, sizeof *setup) == 0)
        return;
    w->setup = *setup;
    if (setup->visible)
        w->help_visible = false;
    ph_window_redraw(w);
}

ph_setup_action ph_window_setup_click(const ph_window *w, int x, int y)
{
    if (!w->setup.visible)
        return PH_SETUP_ACTION_NONE;
    int width, height, out_w, out_h;
    SDL_GetWindowSize(w->window, &width, &height);
    if (width <= 0 || height <= 0 || SDL_GetRendererOutputSize(w->renderer, &out_w, &out_h))
        return PH_SETUP_ACTION_NONE;
    SDL_Point point = {x * out_w / width, y * out_h / height};
    SDL_Rect buttons[3];
    setup_buttons(out_w, out_h, buttons);
    if (SDL_PointInRect(&point, &buttons[0]))
        return PH_SETUP_ACTION_CLOSE;
    if (SDL_PointInRect(&point, &buttons[1]))
        return PH_SETUP_ACTION_BACK;
    if (SDL_PointInRect(&point, &buttons[2]))
        return w->setup.calibrating ? PH_SETUP_ACTION_NONE
               : w->setup.step == PH_SETUP_CALIBRATE && !w->setup.calibrated
                   ? PH_SETUP_ACTION_CALIBRATE : PH_SETUP_ACTION_NEXT;
    return PH_SETUP_ACTION_NONE;
}

void ph_window_toggle_diagnostics(ph_window *w)
{
    w->diagnostics_visible = !w->diagnostics_visible;
    ph_window_redraw(w);
}

void ph_window_set_metrics(ph_window *w, const ph_metrics_snapshot *metrics,
                           unsigned int queued_packets, uint64_t discarded_packets,
                           uint64_t superseded_frames)
{
    w->metrics = *metrics;
    w->queued_packets = queued_packets;
    w->discarded_packets = discarded_packets;
    w->superseded_frames = superseded_frames;
    if (w->diagnostics_visible && !w->setup.visible && !w->help_visible)
        ph_window_redraw(w);
}

/* Letterboxed picture rectangle inside an area of the given size. */
static SDL_Rect fit_rect(int area_w, int area_h, int pic_w, int pic_h)
{
    SDL_Rect r = {0, 0, area_w, area_h};

    if (pic_w <= 0 || pic_h <= 0)
        return r;
    if ((long long)area_w * pic_h > (long long)area_h * pic_w) {
        r.w = (int)((long long)area_h * pic_w / pic_h);
        r.x = (area_w - r.w) / 2;
    } else {
        r.h = (int)((long long)area_w * pic_h / pic_w);
        r.y = (area_h - r.h) / 2;
    }
    return r;
}

static ph_window_insets frame_insets(SDL_Window *window)
{
    ph_window_insets insets = {0, 0, 0, 0};
    int top = 0, left = 0, bottom = 0, right = 0;

    if (window && (SDL_GetWindowFlags(window) & SDL_WINDOW_BORDERLESS))
        return insets;
    if (SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right) == 0 && top >= 0 &&
        left >= 0 && bottom >= 0 && right >= 0 && (top | left | bottom | right) != 0) {
        insets.top = top;
        insets.left = left;
        insets.bottom = bottom;
        insets.right = right;
        return insets;
    }
    /* Borders are often unknown until the window manager maps the window. */
#ifdef _WIN32
    insets.top = 32;
    insets.left = insets.right = insets.bottom = 8;
#else
    insets.top = 32;
#endif
    return insets;
}

static void usable_bounds(SDL_Window *window, SDL_Rect *usable)
{
    int display = SDL_GetWindowDisplayIndex(window);

    usable->x = usable->y = 0;
    if (display < 0 || SDL_GetDisplayUsableBounds(display, usable) != 0) {
        usable->x = usable->y = 0;
        usable->w = 1280;
        usable->h = 800;
    }
}

static void place_window(ph_window *w, int pic_w, int pic_h, bool center)
{
    SDL_Rect usable;
    ph_window_insets insets;
    int client_w, client_h, x, y, cur_x, cur_y, cur_w, cur_h;

    if (SDL_GetWindowFlags(w->window) &
        (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MAXIMIZED))
        return;
    usable_bounds(w->window, &usable);
    insets = frame_insets(w->window);
    {
        int limit_h = usable.h - PH_WINDOW_BAR_PX;
        if (limit_h < 1)
            limit_h = 1;
        ph_chrome_fit_client(pic_w, pic_h, usable.w, limit_h, &insets, &client_w, &client_h);
    }
    client_h += PH_WINDOW_BAR_PX;
    SDL_GetWindowPosition(w->window, &cur_x, &cur_y);
    SDL_GetWindowSize(w->window, &cur_w, &cur_h);
    ph_chrome_place_client(client_w, client_h, usable.x, usable.y, usable.w, usable.h, &insets,
                           cur_x, cur_y, cur_w, cur_h, center, &x, &y);
    SDL_SetWindowSize(w->window, client_w, client_h);
    SDL_SetWindowPosition(w->window, x, y);
}

static void fit_window_to_picture(ph_window *w, int pic_w, int pic_h)
{
    bool first = w->fit_w == 0;
    bool turned = !first && ((pic_w > pic_h) != (w->fit_w > w->fit_h));

    w->fit_w = pic_w;
    w->fit_h = pic_h;
    w->pending_w = w->pending_h = 0;
    place_window(w, pic_w, pic_h, first || turned);
}

static SDL_YUV_CONVERSION_MODE yuv_mode_for(const AVFrame *f)
{
    if (f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P)
        return SDL_YUV_CONVERSION_JPEG;
    if (f->colorspace == AVCOL_SPC_BT470BG || f->colorspace == AVCOL_SPC_SMPTE170M)
        return SDL_YUV_CONVERSION_BT601;
    return SDL_YUV_CONVERSION_BT709; /* iPhone streams are BT.709 */
}

static void track_size(ph_window *w, int sw, int sh)
{
    if (!w->fit_w) {
        fit_window_to_picture(w, sw, sh); /* first picture: fit right away */
    } else if (sw == w->fit_w && sh == w->fit_h) {
        w->pending_w = w->pending_h = 0;
    } else if (sw != w->pending_w || sh != w->pending_h) {
        w->pending_w = sw;
        w->pending_h = sh;
        w->pending_since = SDL_GetTicks64();
    }
}

void ph_window_present(ph_window *w, const AVFrame *f, const ph_rect *screen)
{
    SDL_YUV_CONVERSION_MODE mode = yuv_mode_for(f);

    if (!w->texture || f->width != w->tex_w || f->height != w->tex_h || mode != w->yuv_mode) {
        if (w->texture)
            SDL_DestroyTexture(w->texture);
        SDL_SetYUVConversionMode(mode); /* applies to textures created after this call */
        w->texture = SDL_CreateTexture(w->renderer, SDL_PIXELFORMAT_IYUV,
                                       SDL_TEXTUREACCESS_STREAMING, f->width, f->height);
        if (!w->texture) {
            PH_LOGE("couldn't create a %dx%d video texture: %s", f->width, f->height,
                    SDL_GetError());
            w->tex_w = w->tex_h = 0;
            return;
        }
        w->tex_w = f->width;
        w->tex_h = f->height;
        w->yuv_mode = mode;
    }
    SDL_UpdateYUVTexture(w->texture, NULL, f->data[0], f->linesize[0], f->data[1], f->linesize[1],
                         f->data[2], f->linesize[2]);
    w->screen.x = screen->x;
    w->screen.y = screen->y;
    w->screen.w = screen->w;
    w->screen.h = screen->h;
    w->has_picture = true;
    track_size(w, screen->w, screen->h);
    ph_window_redraw(w);
}

void ph_window_tick(ph_window *w)
{
    if (w->pending_w && SDL_GetTicks64() - w->pending_since >= RESIZE_SETTLE_MS) {
        fit_window_to_picture(w, w->pending_w, w->pending_h);
        ph_window_redraw(w);
    }
}

void ph_window_redraw(ph_window *w)
{
    int out_w, out_h, top;

    SDL_SetRenderDrawColor(w->renderer, 245, 244, 241, 255);
    SDL_RenderClear(w->renderer);
    if (SDL_GetRendererOutputSize(w->renderer, &out_w, &out_h) != 0)
        return;
    top = bar_output_px(w, out_h);
    if (w->has_picture && w->texture) {
        SDL_Rect content = {0, top, out_w, out_h - top};
        SDL_Rect dst = fit_rect(out_w, content.h, w->screen.w, w->screen.h);
        paint(w->renderer, content, 0, 0, 0, 255);
        dst.y += top;
        SDL_RenderCopy(w->renderer, w->texture, &w->screen, &dst);
    } else {
        draw_waiting(w, out_w, out_h, top);
    }
    if (w->help_visible)
        draw_help(w, out_w, out_h, top);
    if (w->diagnostics_visible && !w->help_visible && !w->setup.visible)
        draw_diagnostics(w, out_w, out_h, top);
    if (w->setup.visible)
        draw_setup(w, out_w, out_h, top);
    draw_bar(w, out_w, out_h);
    SDL_RenderPresent(w->renderer);
}

void ph_window_clear(ph_window *w)
{
    w->has_picture = false;
    ph_window_redraw(w);
}

bool ph_window_to_video(const ph_window *w, int x, int y, double *vx, double *vy)
{
    if (!w->control_ready)
        return false;
    return ph_window_picture_point(w, x, y, vx, vy);
}

bool ph_window_picture_point(const ph_window *w, int x, int y, double *vx, double *vy)
{
    int win_w, win_h;
    SDL_Rect r;

    if (!w->has_picture || w->help_visible || w->setup.visible)
        return false;
    if (y < PH_WINDOW_BAR_PX)
        return false;
    SDL_GetWindowSize(w->window, &win_w, &win_h);
    if (win_h - PH_WINDOW_BAR_PX < 1)
        return false;
    r = fit_rect(win_w, win_h - PH_WINDOW_BAR_PX, w->screen.w, w->screen.h);
    r.y += PH_WINDOW_BAR_PX;
    if (r.w <= 0 || r.h <= 0 || x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h)
        return false;
    *vx = (x - r.x + 0.5) / r.w;
    *vy = (y - r.y + 0.5) / r.h;
    return true;
}

void ph_window_video_size(const ph_window *w, int *width, int *height)
{
    *width = w->has_picture ? w->screen.w : 0;
    *height = w->has_picture ? w->screen.h : 0;
}

unsigned int ph_window_id(const ph_window *w)
{
    return SDL_GetWindowID(w->window);
}

bool ph_window_chrome_event(ph_window *w, const SDL_Event *event)
{
    int win_w = 1, win_h = 1, x, y, hit;

    if (!w || !event)
        return false;
    SDL_GetWindowSize(w->window, &win_w, &win_h);
    if (event->type == SDL_MOUSEMOTION) {
        Uint32 held;
        x = event->motion.x;
        y = event->motion.y;
        if (w->chrome_drag) {
            int gx, gy;
            SDL_GetGlobalMouseState(&gx, &gy);
            SDL_SetWindowPosition(w->window, gx - w->drag_dx, gy - w->drag_dy);
            return true;
        }
        hit = chrome_at(win_w, x, y);
        if (hit != w->chrome_hover) {
            w->chrome_hover = hit;
            ph_window_redraw(w);
        }
        held = SDL_GetMouseState(NULL, NULL);
        if ((held & SDL_BUTTON_LMASK) && !w->chrome_down)
            return false;
        return hit != CHROME_NONE;
    }
    if (event->type != SDL_MOUSEBUTTONDOWN && event->type != SDL_MOUSEBUTTONUP)
        return false;
    x = event->button.x;
    y = event->button.y;
    hit = chrome_at(win_w, x, y);
    if (event->button.button != SDL_BUTTON_LEFT)
        return hit != CHROME_NONE;
    if (event->type == SDL_MOUSEBUTTONDOWN) {
        if (hit == CHROME_NONE)
            return false;
        w->chrome_down = true;
        w->chrome_press = hit;
        SDL_CaptureMouse(SDL_TRUE);
        if (hit == CHROME_DRAG && event->button.clicks >= 2) {
            w->chrome_drag = false;
            toggle_zoom(w->window);
        } else if (hit == CHROME_DRAG &&
                   (SDL_GetWindowFlags(w->window) & SDL_WINDOW_MAXIMIZED) == 0) {
            int gx, gy, wx, wy;
            SDL_GetGlobalMouseState(&gx, &gy);
            SDL_GetWindowPosition(w->window, &wx, &wy);
            w->drag_dx = gx - wx;
            w->drag_dy = gy - wy;
            w->chrome_drag = true;
        }
        return true;
    }
    if (!w->chrome_down)
        return false;
    w->chrome_down = false;
    w->chrome_drag = false;
    SDL_CaptureMouse(SDL_FALSE);
    if (hit == w->chrome_press) {
        if (hit == CHROME_MIN)
            SDL_MinimizeWindow(w->window);
        else if (hit == CHROME_MAX)
            toggle_zoom(w->window);
        else if (hit == CHROME_CLOSE) {
            SDL_Event quit = {.type = SDL_QUIT};
            SDL_PushEvent(&quit);
        }
    }
    w->chrome_press = CHROME_NONE;
    return true;
}

void ph_window_destroy(ph_window *w)
{
    if (!w)
        return;
    if (w->texture)
        SDL_DestroyTexture(w->texture);
    if (w->lockup)
        SDL_DestroyTexture(w->lockup);
    if (w->mark)
        SDL_DestroyTexture(w->mark);
    if (w->renderer)
        SDL_DestroyRenderer(w->renderer);
    if (w->window)
        SDL_DestroyWindow(w->window);
    free(w);
}
