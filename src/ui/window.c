#include "ui/window.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <SDL.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#include "core/log.h"
#include "core/version.h"
#include "ui/text.h"
#include "ui/brand_art.h"
#include "ui/placement.h"

#define RESIZE_SETTLE_MS 400 /* a new picture size must last this long before resizing */
#define WAITING_W 960
#define WAITING_H 680
#define RAIL_W 56
#define TRANSFERS_W 248

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
    char title[320];
    char pull_hint[96];
    bool share_checked, share_available;
    Uint64 copied_until;
    int modal_scroll, modal_max_scroll;
    int chrome_hover;
    int chrome_press;
    bool chrome_down;
    bool chrome_drag;
    int drag_dx, drag_dy;
    bool rail_open;
    bool transfers_open;
    int transfer_tab;
};

static void place_window(ph_window *w, int pic_w, int pic_h, bool center);

enum {
    CHROME_NONE, CHROME_MIN, CHROME_MAX, CHROME_CLOSE, CHROME_DRAG,
    CHROME_SETUP, CHROME_HELP, CHROME_STATS, CHROME_COPY,
    CHROME_RAIL, CHROME_TRANSFERS, CHROME_TAB_FILES, CHROME_TAB_CLIP,
    CHROME_ACT
};

static int text_height(const char *text, int width, int scale)
{
    return ph_text_height(text, width, scale);
}

static int draw_text(SDL_Renderer *renderer, const char *text, int x, int y,
                     int width, int scale, SDL_Color color)
{
    return ph_text_draw(renderer, text, x, y, width, scale, color);
}

static const SDL_Color color_text = {17, 17, 17, 255};
static const SDL_Color color_muted = {104, 107, 112, 255};
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

static void rounded(SDL_Renderer *renderer, SDL_Rect box, int radius, SDL_Color color)
{
    if (box.w < 1 || box.h < 1) return;
    if (radius > box.w / 2) radius = box.w / 2;
    if (radius > box.h / 2) radius = box.h / 2;
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    for (int y = 0; y < box.h; y++) {
        int inset = 0;
        int dy = y < radius ? radius - y - 1 : y >= box.h - radius ? y - (box.h - radius) : 0;
        if (dy > 0) inset = radius - (int)sqrt((double)(radius * radius - dy * dy));
        SDL_RenderDrawLine(renderer, box.x + inset, box.y + y, box.x + box.w - inset - 1, box.y + y);
    }
}

static void surface_card(SDL_Renderer *renderer, SDL_Rect box)
{
    rounded(renderer, box, 4, (SDL_Color){228, 228, 228, 255});
    box.x++; box.y++; box.w -= 2; box.h -= 2;
    rounded(renderer, box, 3, color_on_ink);
}

static void draw_button(ph_window *w, SDL_Rect box, const char *label, bool primary,
                         bool disabled, bool hot)
{
    SDL_Color bg = primary ? color_text : color_on_ink;
    SDL_Color fg = primary ? color_on_ink : color_text;
    if (disabled) { bg = (SDL_Color){232, 234, 237, 255}; fg = color_muted; }
    else if (hot) bg = primary ? (SDL_Color){48, 51, 56, 255} : (SDL_Color){238, 240, 242, 255};
    if (!primary && !disabled) surface_card(w->renderer, box);
    SDL_Rect inside = box;
    if (!primary && !disabled) { inside.x++; inside.y++; inside.w -= 2; inside.h -= 2; }
    rounded(w->renderer, inside, 4, bg);
    int scale = box.h < 32 ? 1 : 2;
    int tw = ph_text_width(label, scale);
    draw_text(w->renderer, label, box.x + (box.w - tw) / 2,
               box.y + (box.h - ph_text_line_height(scale)) / 2 - 1, box.w, scale, fg);
}

typedef struct connect_boxes {
    SDL_Rect copy, setup, help, stats;
    int left, inner;
} connect_boxes;

static connect_boxes connect_layout(int width, int height, int top)
{
    connect_boxes g;
    int pad = width < 560 ? 20 : 56;
    int inner = width - pad * 2;

    if (inner > 640) inner = 640;
    g.left = (width - inner) / 2;
    g.inner = inner;
    int button_y = height - 96;
    if (button_y < top + 220) button_y = top + 220;
    if (button_y + 64 > height - 8) button_y = height - 72;
    int gap = 10;
    int bw = (inner - gap) / 2;
    g.copy = (SDL_Rect){g.left, button_y, bw, 40};
    g.setup = (SDL_Rect){g.left + bw + gap, button_y, inner - bw - gap, 40};
    g.help = (SDL_Rect){g.left, button_y + 48, 56, 22};
    g.stats = (SDL_Rect){g.left + 72, button_y + 48, 56, 22};
    return g;
}

static void hint_fields(const ph_window *w, char *address, size_t address_n, char *code, size_t code_n)
{
    char raw_address[80] = "", raw_code[32] = "", third[32] = "";
    int fields = sscanf(w->pull_hint, "%79s %31s %31s", raw_address, raw_code, third);
    char *colon;

    address[0] = 0;
    code[0] = 0;
    if (fields == 3 && !strcmp(raw_code, "code"))
        snprintf(raw_code, sizeof raw_code, "%s", third);
    if (fields < 2)
        return;
    colon = strrchr(raw_address, ':');
    if (colon && colon != raw_address && strspn(colon + 1, "0123456789") == strlen(colon + 1))
        *colon = 0;
    snprintf(address, address_n, "%s", raw_address);
    if (strlen(raw_code) == 8)
        snprintf(code, code_n, "%.4s %.4s", raw_code, raw_code + 4);
    else
        snprintf(code, code_n, "%s", raw_code);
}

static int draw_card(SDL_Renderer *renderer, SDL_Rect card, int scale, const char *title,
                     const char *body)
{
    int pad = 8 * scale;
    int inner = card.w - 2 * pad;

    if (inner < 6 * scale)
        return card.y;
    surface_card(renderer, card);
    SDL_Rect old_clip;
    SDL_bool clipped = SDL_RenderIsClipEnabled(renderer);
    SDL_RenderGetClipRect(renderer, &old_clip);
    SDL_Rect clip = card;
    if (clipped) SDL_IntersectRect(&old_clip, &card, &clip);
    SDL_RenderSetClipRect(renderer, &clip);
    int y = draw_text(renderer, title, card.x + pad, card.y + pad, inner, scale, color_text);
    y = draw_text(renderer, body, card.x + pad, y + 6, inner, scale, color_muted);
    SDL_RenderSetClipRect(renderer, clipped ? &old_clip : NULL);
    return y;
}

static void status_dot(SDL_Renderer *renderer, int x, int y, int r, int g, int b)
{
    rounded(renderer, (SDL_Rect){x, y, 8, 8}, 4, (SDL_Color){(Uint8)r, (Uint8)g, (Uint8)b, 255});
}

static void draw_waiting(ph_window *w, int width, int height, int top)
{
    connect_boxes g = connect_layout(width, height, top);
    char address[80], code[40];
    bool have;
    int y = top + 28;
    int card_h;
    SDL_Rect card;
    const char *rows[4][2];
    int dots[4][3];

    hint_fields(w, address, sizeof address, code, sizeof code);
    have = address[0] && code[0];
    paint(w->renderer, (SDL_Rect){0, top, width, height - top}, 243, 244, 246, 255);
    draw_text(w->renderer, "Connect your iPhone", g.left, y, g.inner, width < 560 ? 2 : 3, color_text);
    y += width < 560 ? 28 : 44;
    draw_text(w->renderer,
              "Open Porthole on your iPhone and enter this PC. Keep both on the same Wi-Fi.",
              g.left, y, g.inner, 1, color_muted);
    y += 36;
    card_h = have ? 132 : 72;
    if (y + card_h > g.copy.y - 16) card_h = g.copy.y - 16 - y;
    if (card_h < 48) card_h = 48;
    card = (SDL_Rect){g.left, y, g.inner, card_h};
    rounded(w->renderer, card, 12, (SDL_Color){226, 228, 232, 255});
    card.x++; card.y++; card.w -= 2; card.h -= 2;
    rounded(w->renderer, card, 11, color_on_ink);
    if (have) {
        draw_text(w->renderer, "THIS PC", card.x + 16, card.y + 12, card.w - 32, 1, color_muted);
        draw_text(w->renderer, "Porthole", card.x + 16, card.y + 28, card.w - 32, 2, color_text);
        draw_text(w->renderer, "Address", card.x + 16, card.y + 64, 80, 1, color_muted);
        draw_text(w->renderer, address, card.x + 100, card.y + 62, card.w - 116, 2, color_text);
        draw_text(w->renderer, "Code", card.x + 16, card.y + 92, 80, 1, color_muted);
        draw_text(w->renderer, code, card.x + 100, card.y + 88, card.w - 116, 2, color_text);
    } else {
        draw_text(w->renderer, w->share_checked && !w->share_available
                    ? "The companion connection is unavailable in this build."
                    : "Starting the companion connection...",
                  card.x + 16, card.y + 24, card.w - 32, 1, color_muted);
    }
    rows[0][0] = "Wi-Fi";
    rows[0][1] = have ? "Listening on this network" : "Starting";
    dots[0][0] = have ? 22 : 154; dots[0][1] = have ? 128 : 88; dots[0][2] = have ? 61 : 18;
    rows[1][0] = "Screen";
    rows[1][1] = w->share_checked && !w->share_available ? "Unavailable in this build"
                 : "Starts when you tap Share screen";
    dots[1][0] = 92; dots[1][1] = 96; dots[1][2] = 104;
    rows[2][0] = "Mouse and keyboard";
    rows[2][1] = w->control_ready ? "Paired over Bluetooth" : "Optional. Set up controls to use the PC.";
    dots[2][0] = w->control_ready ? 22 : 92;
    dots[2][1] = w->control_ready ? 128 : 96;
    dots[2][2] = w->control_ready ? 61 : 104;
    rows[3][0] = "Sound";
    rows[3][1] = "Plays through this PC";
    dots[3][0] = 22; dots[3][1] = 128; dots[3][2] = 61;
    y = card.y + card.h + 14;
    if (y < g.copy.y - 28) {
        int room = g.copy.y - 12 - y;
        bool stacked = g.inner < 520;
        int row_h = room / 4;
        int cap = stacked ? 40 : 28;
        if (row_h > cap) row_h = cap;
        if (row_h >= (stacked ? 30 : 16)) {
            for (int i = 0; i < 4; i++) {
                int ry = y + i * row_h;
                status_dot(w->renderer, g.left, ry + 3, dots[i][0], dots[i][1], dots[i][2]);
                if (stacked) {
                    draw_text(w->renderer, rows[i][0], g.left + 16, ry, g.inner - 16, 1, color_text);
                    draw_text(w->renderer, rows[i][1], g.left + 16, ry + 14, g.inner - 16, 1, color_muted);
                } else {
                    draw_text(w->renderer, rows[i][0], g.left + 16, ry, 160, 1, color_text);
                    draw_text(w->renderer, rows[i][1], g.left + 188, ry, g.inner - 188, 1, color_muted);
                }
            }
        }
    }
    draw_button(w, g.copy, w->copied_until ? "Copied" : "Copy details", true, !have,
                w->chrome_hover == CHROME_COPY);
    draw_button(w, g.setup, "Set up controls", false, false, w->chrome_hover == CHROME_SETUP);
    draw_text(w->renderer, "F1  Help", g.help.x, g.help.y, 80, 1, color_muted);
    draw_text(w->renderer, "F3  Stats", g.stats.x, g.stats.y, 90, 1, color_muted);
}

static int card_height(const char *title, const char *body, int inner, int scale)
{
    int pad = 8 * scale;

    return pad + text_height(title, inner, scale) + 6 + text_height(body, inner, scale) + pad;
}

static void modal_range(ph_window *w, int content, int visible)
{
    w->modal_max_scroll = content > visible ? content - visible : 0;
    if (w->modal_scroll > w->modal_max_scroll) w->modal_scroll = w->modal_max_scroll;
    if (w->modal_scroll < 0) w->modal_scroll = 0;
}

static void draw_scrollbar(ph_window *w, SDL_Rect viewport)
{
    if (w->modal_max_scroll < 1 || viewport.h < 1) return;
    int thumb = viewport.h * viewport.h / (viewport.h + w->modal_max_scroll);
    if (thumb < 28) thumb = 28;
    int y = viewport.y + (viewport.h - thumb) * w->modal_scroll / w->modal_max_scroll;
    rounded(w->renderer, (SDL_Rect){viewport.x + viewport.w + 7, y, 3, thumb}, 1,
              (SDL_Color){169, 175, 183, 255});
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
                       "Alt+K: iPhone keyboard\n"
                       "Alt+V types into the field\n"
                       "Copy both ways with the app\n"
                       "PC keys use Bluetooth\n"
                       "Esc cancels a hold\n"
                       "Media keys: playback";
    const char *setup = "AssistiveTouch on. Dwell off.\n"
                        "Alt+K shows or hides the iPhone keyboard.\n"
                        "PC keys stay on Bluetooth.\n"
                        PH_PAIR_LINE "\n"
                        "Buttons 3-6: Apps, Controls,\n"
                        "Notifications, Screenshot.";
    const char *tools = "F8  Save screenshot\n"
                        "F9  Start or stop recording\n"
                        "F6  Switch connected device\n"
                        "Alt+Enter  Fullscreen\n"
                        "Alt+T  Always on top\n"
                        "Alt+1  Actual pixels\n"
                        "Alt+V  Paste clipboard\n"
                        "Alt+M  Mute PC audio\n"
                        "Alt+minus / equals  Volume\n"
                        "Alt+comma / period  Audio sync";
    int pad = width < 400 ? 20 : 28;
    int content_w = width - 2 * pad;
    if (content_w > 900) content_w = 900;
    int left = (width - content_w) / 2;
    bool columns = content_w >= 650;
    int col = columns ? (content_w - 14) / 2 : content_w;
    int mouse_h = card_height("Mouse & gestures", mouse, col - 32, 2);
    int keys_h = card_height("Keyboard shortcuts", keys, col - 32, 2);
    int block = columns ? (mouse_h > keys_h ? mouse_h : keys_h) : mouse_h + 14 + keys_h;
    SDL_Rect viewport = {left, top + 105, content_w, height - top - 145};
    if (viewport.h < 1) return;
    paint(w->renderer, (SDL_Rect){0, top, width, height - top}, 255, 255, 255, 255);
    draw_text(w->renderer, "Help & shortcuts", left, top + 21, content_w, 3, color_text);
    draw_text(w->renderer, "Esc or F1 to close. Scroll for more.", left, top + 61,
                content_w, 1, color_muted);
    int foot_h = text_height(setup, content_w - 32, 1) + 32;
    int tools_h = card_height("Window, capture & audio", tools, content_w - 32, 2);
    modal_range(w, block + 28 + tools_h + foot_h, viewport.h);
    SDL_RenderSetClipRect(w->renderer, &viewport);
    int y = viewport.y - w->modal_scroll;
    draw_card(w->renderer, (SDL_Rect){left, y, col, mouse_h}, 2, "Mouse & gestures", mouse);
    draw_card(w->renderer, (SDL_Rect){columns ? left + col + 14 : left,
                columns ? y : y + mouse_h + 14, col, keys_h}, 2, "Keyboard shortcuts", keys);
    y += block + 14;
    draw_card(w->renderer, (SDL_Rect){left, y, content_w, tools_h}, 2,
                "Window, capture & audio", tools);
    y += tools_h + 14;
    surface_card(w->renderer, (SDL_Rect){left, y, content_w, foot_h});
    draw_text(w->renderer, setup, left + 16, y + 16, content_w - 32, 1, color_muted);
    SDL_RenderSetClipRect(w->renderer, NULL);
    draw_scrollbar(w, viewport);
    draw_text(w->renderer, "Porthole v" PORTHOLE_VERSION, left, height - 28, content_w, 1, color_muted);
}

static void setup_buttons(int width, int height, SDL_Rect buttons[3])
{
    int content = width - (width < 400 ? 40 : 56);
    if (content > 700) content = 700;
    int left = (width - content) / 2;
    int button_width = (content - 20) / 3;
    for (int i = 0; i < 3; i++)
        buttons[i] = (SDL_Rect){left + i * (button_width + 10), height - 66, button_width, 42};
}

static bool setup_next_enabled(const ph_setup *setup)
{
    ph_setup copy = *setup;
    if (copy.step == PH_SETUP_CALIBRATE && !copy.calibrated)
        return ph_setup_start_calibration(&copy, 0);
    return ph_setup_next(&copy);
}

static void draw_setup(ph_window *w, int width, int height, int top)
{
    const ph_setup *s = &w->setup;
    int margin = width < 400 ? 20 : 28;
    int content_width = width - 2 * margin;
    if (content_width > 700) content_width = 700;
    margin = (width - content_width) / 2;
    paint(w->renderer, (SDL_Rect){0, top, width, height - top}, 255, 255, 255, 255);
    SDL_Rect buttons[3];
    setup_buttons(width, height, buttons);
    int step = s->controls ? (int)s->step : s->step == PH_SETUP_MIRROR ? 0 : 1;
    int steps = s->controls ? 5 : 2;
    char heading[80];
    snprintf(heading, sizeof heading, "CONNECT YOUR IPHONE / %d OF %d", step + 1, steps);
    draw_text(w->renderer, heading, margin, top + 22, content_width, 1, color_muted);
    int segment = (content_width - (steps - 1) * 6) / steps;
    for (int i = 0; i < steps; i++)
        rounded(w->renderer, (SDL_Rect){margin + i * (segment + 6), top + 55, segment, 4}, 2,
                  i <= step ? color_text : (SDL_Color){220, 224, 229, 255});
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
        title = "Share your screen";
        body = "Open Porthole on your iPhone.\n"
               "Enter this PC's address and code, then tap Connect.\n\n"
               "Tap Share screen. In the iOS dialog choose Porthole, then Start Broadcast.\n\n"
               "Keep both devices on the same Wi-Fi. Screen Mirroring can stay off.";
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
                   ? "Point, click, drag, and type in the shared screen.\n"
                     "F1 lists keys. F2 opens this guide again.\n\n"
                     "Optional: assign AssistiveTouch buttons 3 to 6 to App Switcher, Control Center, Notifications, and Screenshot."
                   : "The iPhone screen is connected.\nThis window is viewing only.\nF2 opens this guide again.";
        break;
    }
    int y = draw_text(w->renderer, title, margin, top + 77, content_width, 3, color_text);
    SDL_Rect card = {margin, y + 18, content_width, buttons[0].y - y - 34};
    surface_card(w->renderer, card);
    SDL_Rect viewport = {card.x + 18, card.y + 14, card.w - 36, card.h - 28};
    if (viewport.h < 1) viewport.h = 1;
    char status[160] = "";
    if (s->step == PH_SETUP_BLUETOOTH)
        snprintf(status, sizeof status, "Controls: %s", s->bluetooth_ready ? "connected" :
                    w->control_detail[0] ? w->control_detail : "waiting for Bluetooth");
    else if (s->step == PH_SETUP_MIRROR)
        snprintf(status, sizeof status, "Screen: %s", s->mirroring ? "connected" : "waiting for your iPhone");
    else if (s->step == PH_SETUP_CALIBRATE)
        snprintf(status, sizeof status, "%s: %u/%d targets",
                    s->calibrated ? "Confirmed" : s->calibrating ? "Checking" : "Not checked",
                    s->target, PH_SETUP_TARGETS);
    const char *error = s->error == PH_SETUP_OK ? ""
                       : s->error == PH_SETUP_CONNECTION_LOST ? "Connection lost. Reconnect, then retry."
                       : s->error == PH_SETUP_CANCELED ? "Check paused. Press Start to retry."
                       : s->error == PH_SETUP_SAVE_FAILED ? "Could not save calibration. Check the configuration folder, then retry."
                       : "Pointer not confirmed. Check AssistiveTouch and Tracking Sensitivity, then retry.";
    char share_body[700];
    if (s->step == PH_SETUP_MIRROR) {
        snprintf(share_body, sizeof share_body, "%s\n\n%s%s", body,
                    w->pull_hint[0] ? "This PC: " : "Connection details are unavailable.", w->pull_hint);
        body = share_body;
    }
    int body_h = text_height(body, viewport.w, 2);
    int status_h = status[0] ? 18 + text_height(status, viewport.w, 1) : 0;
    int error_h = error[0] ? 18 + text_height(error, viewport.w, 1) : 0;
    modal_range(w, body_h + status_h + error_h, viewport.h);
    SDL_RenderSetClipRect(w->renderer, &viewport);
    y = draw_text(w->renderer, body, viewport.x, viewport.y - w->modal_scroll, viewport.w, 2, color_text);
    if (status[0]) y = draw_text(w->renderer, status, viewport.x, y + 18, viewport.w, 1, color_muted);
    if (error[0]) draw_text(w->renderer, error, viewport.x, y + 18, viewport.w, 1, color_warn);
    SDL_RenderSetClipRect(w->renderer, NULL);
    draw_scrollbar(w, viewport);
    const char *next = s->step == PH_SETUP_FINISH ? "Finish"
                       : s->calibrating ? "Wait"
                       : s->step == PH_SETUP_CALIBRATE && !s->calibrated
                             ? s->error != PH_SETUP_OK ? "Retry" : "Start" : "Next";
    const char *labels[] = {"Close", "Back", next};
    bool first = s->step == (s->controls ? PH_SETUP_PREPARE : PH_SETUP_MIRROR);
    for (int i = 0; i < 3; i++)
        draw_button(w, buttons[i], labels[i], i == 2,
                      i == 1 ? first : i == 2 ? !setup_next_enabled(s) : false, false);

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
    int scale = 1, margin = 16;
    bool wide = width >= 700;
    char lines[8][160];
    const char *note = "Local measurements. Helper ACK excludes radio and iPhone latency.";
    int y, i;

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
    int inner = width - 4 * margin;
    int col = wide ? (inner - margin) / 2 : inner;
    int left_h = 0, right_h = 0;
    for (i = 0; i < 8; i++) {
        int line_h = text_height(lines[i], col, scale);
        if (wide && i >= 4) right_h += line_h; else left_h += line_h;
    }
    int rows_h = left_h > right_h ? left_h : right_h;
    int panel_h = 2 * margin + ph_text_line_height(2) + 12 + rows_h +
                  12 + text_height(note, inner, 1);
    if (panel_h > height - top - 2 * margin) panel_h = height - top - 2 * margin;
    SDL_Rect panel = {margin, top + margin, width - 2 * margin, panel_h};
    surface_card(w->renderer, panel);
    SDL_Rect clip = {panel.x + margin, panel.y + margin, inner, panel.h - 2 * margin};
    SDL_RenderSetClipRect(w->renderer, &clip);
    y = draw_text(w->renderer, "Connection stats / F3 to close", clip.x, clip.y, inner, 2, color_text) + 12;
    int left = y, right = y;
    for (i = 0; i < 8; i++) {
        if (wide && i >= 4)
            right = draw_text(w->renderer, lines[i], clip.x + col + margin, right, col, scale, color_text);
        else left = draw_text(w->renderer, lines[i], clip.x, left, col, scale, color_text);
    }
    y = left > right ? left : right;
    draw_text(w->renderer, note, clip.x, y + 12, inner, 1, color_muted);
    SDL_RenderSetClipRect(w->renderer, NULL);

}

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
    int btn = 40;
    for (int i = 0; i < 3; i++)
        buttons[i] = (SDL_Rect){win_w - (3 - i) * btn, 0, btn, PH_WINDOW_BAR_PX};
}

static void toolbar_buttons(int width, SDL_Rect buttons[3])
{
    buttons[0] = (SDL_Rect){0, 0, 0, 0};
    buttons[1] = (SDL_Rect){width - 156, 0, 32, PH_WINDOW_BAR_PX};
    buttons[2] = (SDL_Rect){0, 0, 0, 0};
}

static bool mirror_chrome(const ph_window *w)
{
    return w->has_picture && !w->help_visible && !w->setup.visible && !w->diagnostics_visible;
}

static int side_panels_w(const ph_window *w)
{
    if (!mirror_chrome(w))
        return 0;
    return (w->rail_open ? RAIL_W : 0) + (w->transfers_open ? TRANSFERS_W : 0);
}

static int picture_column_w(const ph_window *w, int win_w)
{
    int column = win_w - side_panels_w(w);
    return column < 1 ? 1 : column;
}

static SDL_Rect rail_toggle_box(int win_w)
{
    return (SDL_Rect){win_w - 120 - 8 - 64, 3, 28, PH_WINDOW_BAR_PX - 6};
}

static SDL_Rect transfers_toggle_box(int win_w)
{
    return (SDL_Rect){win_w - 120 - 8 - 32, 3, 28, PH_WINDOW_BAR_PX - 6};
}

static SDL_Rect action_box(const ph_window *w, int win_w, int index)
{
    int x = picture_column_w(w, win_w);
    return (SDL_Rect){x + 8, PH_WINDOW_BAR_PX + 8 + index * 44, 40, 40};
}

static int chrome_at(const ph_window *w, int win_w, int win_h, int x, int y)
{
    SDL_Rect buttons[3];
    SDL_Point point = {x, y};
    if (x < 0 || x >= win_w || y < 0 || y >= win_h) return CHROME_NONE;
    if (y < PH_WINDOW_BAR_PX) {
        chrome_buttons(win_w, buttons);
        for (int i = 0; i < 3; i++)
            if (SDL_PointInRect(&point, &buttons[i])) return CHROME_MIN + i;
        if (mirror_chrome(w)) {
            SDL_Rect rail = rail_toggle_box(win_w);
            SDL_Rect transfers = transfers_toggle_box(win_w);
            if (SDL_PointInRect(&point, &rail)) return CHROME_RAIL;
            if (SDL_PointInRect(&point, &transfers)) return CHROME_TRANSFERS;
        } else {
            toolbar_buttons(win_w, buttons);
            for (int i = 0; i < 3; i++)
                if (SDL_PointInRect(&point, &buttons[i])) return CHROME_SETUP + i;
        }
        return CHROME_DRAG;
    }
    if (mirror_chrome(w)) {
        int column = picture_column_w(w, win_w);
        if (w->rail_open && x >= column && x < column + RAIL_W) {
            for (int i = 0; i < 8; i++) {
                SDL_Rect box = action_box(w, win_w, i);
                if (SDL_PointInRect(&point, &box)) return CHROME_ACT + i;
            }
            return CHROME_RAIL;
        }
        if (w->transfers_open && x >= column + (w->rail_open ? RAIL_W : 0)) {
            int panel = column + (w->rail_open ? RAIL_W : 0);
            SDL_Rect files = {panel + 12, PH_WINDOW_BAR_PX + 40, (TRANSFERS_W - 32) / 2, 28};
            SDL_Rect clip = files;
            clip.x += files.w + 8;
            if (SDL_PointInRect(&point, &files)) return CHROME_TAB_FILES;
            if (SDL_PointInRect(&point, &clip)) return CHROME_TAB_CLIP;
            return CHROME_TRANSFERS;
        }
    }
    if (!w->has_picture && !w->help_visible && !w->setup.visible) {
        connect_boxes g = connect_layout(win_w, win_h, PH_WINDOW_BAR_PX);
        char address[80], code[40];
        hint_fields(w, address, sizeof address, code, sizeof code);
        if (SDL_PointInRect(&point, &g.copy)) return address[0] ? CHROME_COPY : CHROME_NONE;
        if (SDL_PointInRect(&point, &g.setup)) return CHROME_SETUP;
        if (SDL_PointInRect(&point, &g.help)) return CHROME_HELP;
        if (SDL_PointInRect(&point, &g.stats)) return CHROME_STATS;
    }
    return CHROME_NONE;
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
    int cx = box.x + box.w / 2, cy = box.y + box.h / 2, arm = 5, i;

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

static void draw_bar(ph_window *w, int width, int height)
{
    bool live = mirror_chrome(w);
    (void)height;
    paint(w->renderer, (SDL_Rect){0, 0, width, PH_WINDOW_BAR_PX}, live ? 11 : 255, live ? 12 : 255,
          live ? 13 : 255, 255);
    if (w->mark) {
        SDL_Rect mark = {10, 7, 18, 18};
        SDL_RenderCopy(w->renderer, w->mark, NULL, &mark);
    }
    if (live) {
        SDL_Rect rail = rail_toggle_box(width);
        SDL_Rect transfers = transfers_toggle_box(width);
        status_dot(w->renderer, 34, 12, 61, 214, 140);
        draw_text(w->renderer, "iPhone", 48, 6, 80, 1, color_on_ink);
        if (w->rail_open || w->chrome_hover == CHROME_RAIL)
            rounded(w->renderer, rail, 6, (SDL_Color){40, 42, 46, 255});
        if (w->transfers_open || w->chrome_hover == CHROME_TRANSFERS)
            rounded(w->renderer, transfers, 6, (SDL_Color){40, 42, 46, 255});
        draw_text(w->renderer, "C", rail.x + 9, 6, 16, 1, color_on_ink);
        draw_text(w->renderer, "T", transfers.x + 9, 6, 16, 1, color_on_ink);
    } else {
        draw_text(w->renderer, "Porthole", 35, 6, 100, 1, color_text);
        SDL_Rect buttons[3];
        toolbar_buttons(width, buttons);
        if (w->chrome_hover == CHROME_HELP)
            paint(w->renderer, buttons[1], 235, 235, 235, 255);
        draw_text(w->renderer, "?", buttons[1].x + 12, 6, 20, 1, color_muted);
    }
    SDL_Rect buttons[3];
    chrome_buttons(width, buttons);
    for (int i = 0; i < 3; i++) {
        int kind = i + CHROME_MIN;
        bool hot = w->chrome_hover == kind;
        if (hot) paint(w->renderer, buttons[i], kind == CHROME_CLOSE ? 210 : (live ? 40 : 233),
                        kind == CHROME_CLOSE ? 40 : (live ? 42 : 233),
                        kind == CHROME_CLOSE ? 40 : (live ? 46 : 233), 255);
        draw_chrome_glyph(w->renderer, kind, buttons[i], 1,
                          (hot && kind == CHROME_CLOSE) || (live && !(hot && kind == CHROME_CLOSE)));
    }
    if (!live)
        paint(w->renderer, (SDL_Rect){0, PH_WINDOW_BAR_PX - 1, width, 1}, 229, 229, 229, 255);
}

static void toggle_zoom(SDL_Window *window)
{
    Uint32 flags = SDL_GetWindowFlags(window);

    if (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN_DESKTOP))
        SDL_RestoreWindow(window);
    else
        SDL_MaximizeWindow(window);
}

static void push_shortcut(ph_window *w, SDL_Scancode key, SDL_Keymod mod)
{
    /* Route clicks through the keyboard path so modal input cancellation is identical. */
    SDL_Event event = {0};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(w->window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = key;
    event.key.keysym.sym = SDL_GetKeyFromScancode(key);
    event.key.keysym.mod = mod;
    SDL_PushEvent(&event);
    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    SDL_PushEvent(&event);
}

static void flip_panel(ph_window *w, bool *flag, int width_px)
{
    int delta = *flag ? -width_px : width_px;
    Uint32 flags = SDL_GetWindowFlags(w->window);
    int ww, wh;

    *flag = !*flag;
    if ((flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) == 0) {
        SDL_GetWindowSize(w->window, &ww, &wh);
        if (ww + delta >= 320)
            SDL_SetWindowSize(w->window, ww + delta, wh);
    }
    ph_window_redraw(w);
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
    place_window(w, WAITING_W, WAITING_H - PH_WINDOW_BAR_PX, true);
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
    if (!ph_text_attach(w->renderer))
        goto fail;
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
    if (w->help_visible || !w->has_picture)
        ph_window_redraw(w);
}

void ph_window_set_share_available(ph_window *w, bool available)
{
    if (!w) return;
    w->share_checked = true;
    w->share_available = available;
    if (!w->has_picture) ph_window_redraw(w);
}

void ph_window_toggle_help(ph_window *w)
{
    w->help_visible = !w->help_visible;
    w->modal_scroll = 0;
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
    if (w->setup.step != setup->step || w->setup.visible != setup->visible)
        w->modal_scroll = 0;
    w->setup = *setup;
    if (setup->visible)
        w->help_visible = false;
    ph_window_redraw(w);
}

ph_setup_action ph_window_setup_click(const ph_window *w, int x, int y)
{
    if (!w->setup.visible)
        return PH_SETUP_ACTION_NONE;
    int width, height;
    SDL_GetWindowSize(w->window, &width, &height);
    if (width <= 0 || height <= 0)
        return PH_SETUP_ACTION_NONE;
    SDL_Point point = {x, y};
    SDL_Rect buttons[3];
    setup_buttons(width, height, buttons);
    if (SDL_PointInRect(&point, &buttons[0]))
        return PH_SETUP_ACTION_CLOSE;
    if (SDL_PointInRect(&point, &buttons[1]))
        return w->setup.step == (w->setup.controls ? PH_SETUP_PREPARE : PH_SETUP_MIRROR)
                   ? PH_SETUP_ACTION_NONE : PH_SETUP_ACTION_BACK;
    if (SDL_PointInRect(&point, &buttons[2]))
        return !setup_next_enabled(&w->setup) ? PH_SETUP_ACTION_NONE
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
    if (w->copied_until && SDL_GetTicks64() >= w->copied_until) {
        w->copied_until = 0;
        ph_window_redraw(w);
    }
    if (w->pending_w && SDL_GetTicks64() - w->pending_since >= RESIZE_SETTLE_MS) {
        fit_window_to_picture(w, w->pending_w, w->pending_h);
        ph_window_redraw(w);
    }
}

static void stroke(SDL_Renderer *renderer, int x1, int y1, int x2, int y2)
{
    SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
    SDL_RenderDrawLine(renderer, x1 + 1, y1, x2 + 1, y2);
}

static void draw_action_icon(SDL_Renderer *renderer, int index, SDL_Rect box)
{
    int x = box.x + 11, y = box.y + 11;

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    switch (index) {
    case 0: /* Home */
        stroke(renderer, x, y + 8, x + 8, y);
        stroke(renderer, x + 8, y, x + 16, y + 8);
        stroke(renderer, x + 3, y + 8, x + 3, y + 16);
        stroke(renderer, x + 13, y + 8, x + 13, y + 16);
        stroke(renderer, x + 3, y + 16, x + 13, y + 16);
        break;
    case 1: /* App Switcher */
        for (int i = 0; i < 4; i++)
            SDL_RenderDrawRect(renderer, &(SDL_Rect){x + (i % 2) * 9, y + (i / 2) * 9, 7, 7});
        break;
    case 2: /* Control Center */
        stroke(renderer, x, y + 3, x + 16, y + 3);
        stroke(renderer, x, y + 8, x + 16, y + 8);
        stroke(renderer, x, y + 13, x + 16, y + 13);
        break;
    case 3: /* Notifications */
        stroke(renderer, x + 8, y, x + 8, y + 2);
        stroke(renderer, x + 3, y + 7, x + 13, y + 7);
        stroke(renderer, x + 4, y + 4, x + 4, y + 11);
        stroke(renderer, x + 12, y + 4, x + 12, y + 11);
        stroke(renderer, x + 4, y + 11, x + 12, y + 11);
        break;
    case 4: /* Screenshot */
        stroke(renderer, x, y + 5, x, y);
        stroke(renderer, x, y, x + 5, y);
        stroke(renderer, x + 11, y, x + 16, y);
        stroke(renderer, x + 16, y, x + 16, y + 5);
        stroke(renderer, x, y + 11, x, y + 16);
        stroke(renderer, x, y + 16, x + 5, y + 16);
        stroke(renderer, x + 11, y + 16, x + 16, y + 16);
        stroke(renderer, x + 16, y + 11, x + 16, y + 16);
        break;
    case 5: /* Volume up */
    case 6: /* Volume down */
        stroke(renderer, x, y + 6, x + 4, y + 6);
        stroke(renderer, x + 4, y + 6, x + 9, y + 2);
        stroke(renderer, x + 4, y + 10, x + 9, y + 14);
        stroke(renderer, x, y + 6, x, y + 10);
        stroke(renderer, x, y + 10, x + 4, y + 10);
        stroke(renderer, x + 12, y + 8, x + 16, y + 8);
        if (index == 5) stroke(renderer, x + 14, y + 6, x + 14, y + 10);
        break;
    default: /* Feed scrolling */
        stroke(renderer, x + 8, y + 14, x + 8, y + 2);
        stroke(renderer, x + 4, y + 6, x + 8, y + 2);
        stroke(renderer, x + 12, y + 6, x + 8, y + 2);
        break;
    }
}

static void draw_mirror_panels(ph_window *w, int width, int height)
{
    int column = picture_column_w(w, width);
    int x = column;

    if (w->rail_open) {
        paint(w->renderer, (SDL_Rect){x, PH_WINDOW_BAR_PX, RAIL_W, height - PH_WINDOW_BAR_PX},
              11, 12, 13, 255);
        for (int i = 0; i < 8; i++) {
            SDL_Rect box = action_box(w, width, i);
            if (w->chrome_hover == CHROME_ACT + i)
                rounded(w->renderer, box, 8, (SDL_Color){40, 42, 46, 255});
            draw_action_icon(w->renderer, i, box);
        }
        x += RAIL_W;
    }
    if (w->transfers_open) {
        SDL_Rect files, clip;
        const char *body = w->transfer_tab
            ? "Copy on either side while Porthole is open on the iPhone. Paste there with Ctrl+V."
            : "Share from any app on the iPhone. Files save to Downloads.\n\n"
              "Hold a photo in this picture, then drag it out. That takes the original file.";
        paint(w->renderer, (SDL_Rect){x, PH_WINDOW_BAR_PX, TRANSFERS_W, height - PH_WINDOW_BAR_PX},
              255, 255, 255, 255);
        draw_text(w->renderer, "Transfers", x + 14, PH_WINDOW_BAR_PX + 12, TRANSFERS_W - 28, 2, color_text);
        files = (SDL_Rect){x + 12, PH_WINDOW_BAR_PX + 40, (TRANSFERS_W - 32) / 2, 28};
        clip = files;
        clip.x += files.w + 8;
        rounded(w->renderer, files, 6, w->transfer_tab ? (SDL_Color){243, 244, 246, 255} : color_text);
        rounded(w->renderer, clip, 6, w->transfer_tab ? color_text : (SDL_Color){243, 244, 246, 255});
        draw_text(w->renderer, "Files", files.x + 18, files.y + 4, files.w - 8, 1,
                  w->transfer_tab ? color_text : color_on_ink);
        draw_text(w->renderer, "Clipboard", clip.x + 6, clip.y + 4, clip.w - 8, 1,
                  w->transfer_tab ? color_on_ink : color_text);
        draw_text(w->renderer, body, x + 14, PH_WINDOW_BAR_PX + 80, TRANSFERS_W - 28, 1, color_muted);
    }
}

void ph_window_redraw(ph_window *w)
{
    int out_w, out_h, top, width, height;

    SDL_SetRenderDrawColor(w->renderer, 245, 244, 241, 255);
    SDL_RenderClear(w->renderer);
    if (SDL_GetRendererOutputSize(w->renderer, &out_w, &out_h) != 0)
        return;
    SDL_GetWindowSize(w->window, &width, &height);
    if (width < 1 || height < 1) return;
    top = bar_output_px(w, out_h);
    if (w->has_picture && w->texture) {
        int panels = side_panels_w(w);
        int panel_px = width > 0 ? (int)(panels * ((float)out_w / (float)width) + 0.5f) : 0;
        int pic_out_w = out_w - panel_px;
        SDL_Rect content, dst;
        if (pic_out_w < 1) pic_out_w = 1;
        content = (SDL_Rect){0, top, pic_out_w, out_h - top};
        dst = fit_rect(pic_out_w, content.h, w->screen.w, w->screen.h);
        paint(w->renderer, content, 11, 12, 13, 255);
        dst.y += top;
        SDL_RenderCopy(w->renderer, w->texture, &w->screen, &dst);
    }
    /* Draw chrome in window points so text, spacing and hit areas agree on
     * high-DPI displays. Video keeps its full physical-pixel output above. */
    SDL_RenderSetScale(w->renderer, (float)out_w / (float)width, (float)out_h / (float)height);
    if (!w->has_picture || !w->texture)
        draw_waiting(w, width, height, PH_WINDOW_BAR_PX);
    if (mirror_chrome(w) && (w->rail_open || w->transfers_open))
        draw_mirror_panels(w, width, height);
    if (w->help_visible)
        draw_help(w, width, height, PH_WINDOW_BAR_PX);
    if (w->diagnostics_visible && !w->help_visible && !w->setup.visible)
        draw_diagnostics(w, width, height, PH_WINDOW_BAR_PX);
    if (w->setup.visible)
        draw_setup(w, width, height, PH_WINDOW_BAR_PX);
    draw_bar(w, width, height);
    SDL_RenderSetScale(w->renderer, 1, 1);
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
    win_w = picture_column_w(w, win_w);
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
    if ((w->help_visible || w->setup.visible) && event->type == SDL_MOUSEWHEEL) {
        double amount = event->wheel.y;
#if SDL_VERSION_ATLEAST(2, 0, 18)
        if (event->wheel.preciseY != 0) amount = event->wheel.preciseY;
#endif
        if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) amount = -amount;
        if (amount > 20) amount = 20;
        if (amount < -20) amount = -20;
        w->modal_scroll -= (int)(amount * 40);
        ph_window_redraw(w);
        return true;
    }
    if ((w->help_visible || w->setup.visible) &&
        (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP)) {
        SDL_Scancode key = event->key.keysym.scancode;
        if (key == SDL_SCANCODE_PAGEDOWN || key == SDL_SCANCODE_PAGEUP ||
            (w->help_visible && (key == SDL_SCANCODE_HOME || key == SDL_SCANCODE_END ||
                                key == SDL_SCANCODE_UP || key == SDL_SCANCODE_DOWN))) {
            if (event->type == SDL_KEYDOWN) {
                if (key == SDL_SCANCODE_HOME) w->modal_scroll = 0;
                else if (key == SDL_SCANCODE_END) w->modal_scroll = w->modal_max_scroll;
                else w->modal_scroll += key == SDL_SCANCODE_PAGEUP ? -240
                                          : key == SDL_SCANCODE_UP ? -40
                                          : key == SDL_SCANCODE_DOWN ? 40 : 240;
                ph_window_redraw(w);
            }
            return true;
        }
    }
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
        hit = chrome_at(w, win_w, win_h, x, y);
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
    hit = chrome_at(w, win_w, win_h, x, y);
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
        else if (hit == CHROME_COPY && w->pull_hint[0]) {
            if (SDL_SetClipboardText(w->pull_hint) == 0) w->copied_until = SDL_GetTicks64() + 1800;
            ph_window_redraw(w);
        }
        else if (hit == CHROME_SETUP)
            push_shortcut(w, SDL_SCANCODE_F2, KMOD_NONE);
        else if (hit == CHROME_HELP)
            push_shortcut(w, SDL_SCANCODE_F1, KMOD_NONE);
        else if (hit == CHROME_STATS)
            push_shortcut(w, SDL_SCANCODE_F3, KMOD_NONE);
        else if (hit == CHROME_RAIL)
            flip_panel(w, &w->rail_open, RAIL_W);
        else if (hit == CHROME_TRANSFERS)
            flip_panel(w, &w->transfers_open, TRANSFERS_W);
        else if (hit == CHROME_TAB_FILES || hit == CHROME_TAB_CLIP) {
            w->transfer_tab = hit == CHROME_TAB_CLIP;
            ph_window_redraw(w);
        }
        else if (hit >= CHROME_ACT && hit < CHROME_ACT + 8) {
            static const SDL_Scancode keys[8] = {
                SDL_SCANCODE_H, SDL_SCANCODE_S, SDL_SCANCODE_C, SDL_SCANCODE_N,
                SDL_SCANCODE_P, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_F4
            };
            int index = hit - CHROME_ACT;
            push_shortcut(w, keys[index], index == 7 ? KMOD_NONE : KMOD_LALT);
        }
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
        ph_text_detach(w->renderer);
    if (w->renderer)
        SDL_DestroyRenderer(w->renderer);
    if (w->window)
        SDL_DestroyWindow(w->window);
    free(w);
}
