#include "ui/window.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#include "core/log.h"

#define FIT_SCREEN_FRACTION 0.9 /* window: at most 90% of the usable screen */
#define RESIZE_SETTLE_MS 400    /* a new picture size must last this long before resizing */

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
};

/* Small built-in font keeps setup guidance available without an extra font package.
 * Each row contains five pixels. Lowercase is rendered using the uppercase glyph. */
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
};

static const uint8_t *glyph(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
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

static int text_scale(int width)
{
    int scale = (width + 249) / 250;
    return scale > 0 ? scale : 1;
}

static const SDL_Color color_text = {232, 239, 246, 255};
static const SDL_Color color_muted = {166, 182, 198, 255};
static const SDL_Color color_accent = {89, 199, 255, 255};

static void draw_waiting(ph_window *w, int width, int height)
{
    int scale = text_scale(width), margin = 16 * scale;
    int content_width = width - 2 * margin;
    if (content_width < 6 * scale)
        return;
    int y = height > 600 * scale ? (height - 500 * scale) / 2 : 22 * scale;
    y = draw_text(w->renderer, "Porthole", margin, y, content_width, scale + 1, color_accent);
    y += 12 * scale;
    y = draw_text(w->renderer, "Mirror your iPhone", margin, y, content_width, scale, color_text);
    y += 5 * scale;
    y = draw_text(w->renderer,
                  "1. Use the same Wi-Fi network.\n"
                  "2. On iPhone, open Control Center.\n"
                  "3. Screen Mirroring > Porthole.",
                  margin, y, content_width, scale, color_muted);
    y += 14 * scale;
    y = draw_text(w->renderer, "Mouse and keyboard setup", margin, y, content_width,
                  scale, color_text);
    y += 5 * scale;
    y = draw_text(w->renderer,
                  "Pair Porthole in iPhone Bluetooth.\n"
                  "Accessibility > Touch > AssistiveTouch: On.\n"
                  "Dwell Control: Off.",
                  margin, y, content_width, scale, color_muted);
    y += 14 * scale;
    char status[160];
    snprintf(status, sizeof status, "Controls: %s",
             w->control_ready ? "connected" : w->control_detail[0] ? w->control_detail : "waiting");
    y = draw_text(w->renderer, status, margin, y, content_width, scale, color_accent);
    draw_text(w->renderer, "F1: help  F2: guided setup\nF3: diagnostics", margin, y + 8 * scale,
               content_width, scale, color_muted);
}

static void draw_help(ph_window *w, int width, int height)
{
    int scale = text_scale(width), margin = 12 * scale;
    SDL_Rect panel = {margin / 2, margin / 2, width - margin, height - margin};
    SDL_SetRenderDrawBlendMode(w->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(w->renderer, 12, 23, 35, 245);
    SDL_RenderFillRect(w->renderer, &panel);
    SDL_SetRenderDrawBlendMode(w->renderer, SDL_BLENDMODE_NONE);
    int content_width = width - 2 * margin;
    if (content_width < 6 * scale)
        return;
    SDL_Rect old_clip;
    SDL_RenderGetClipRect(w->renderer, &old_clip);
    SDL_bool clipped = SDL_RenderIsClipEnabled(w->renderer);
    SDL_RenderSetClipRect(w->renderer, &panel);
    int y = margin;
    y = draw_text(w->renderer, "Porthole help", margin, y, content_width,
                  scale + 1, color_accent);
    y = draw_text(w->renderer, "F1: close  F2: guided setup\nF3: diagnostics", margin, y + 6 * scale,
                  content_width, scale, color_muted);
    y += 8 * scale;
    y = draw_text(w->renderer, "Mouse", margin, y, content_width, scale, color_accent);
    y = draw_text(w->renderer,
                  "Move over the picture to point.\n"
                  "Left click: tap\nRight click: secondary click\n"
                  "Hold and drag: swipe\nWheel: scroll at the pointer",
                  margin, y, content_width, scale, color_text);
    y += 6 * scale;
    y = draw_text(w->renderer, "Keyboard", margin, y, content_width, scale, color_accent);
    y = draw_text(w->renderer,
                  "Type directly. Ctrl acts as Command.\n"
                  "Alt+H: Home\nAlt+F: Spotlight\nAlt+S: App Switcher\n"
                  "Alt+C: Control Center\nAlt+N: Notifications\nAlt+P: Screenshot\n"
                  "Alt+Left: Back\nAlt+Up/Down: Volume",
                  margin, y, content_width, scale, color_text);
    y += 6 * scale;
    y = draw_text(w->renderer, "Setup", margin, y, content_width, scale, color_accent);
    draw_text(w->renderer,
               "AssistiveTouch: On. Dwell Control: Off.\n"
               "Pair Porthole in iPhone Bluetooth.\n"
               "Assign buttons 3-6 to App Switcher, Control Center, Notifications, Screenshot.",
               margin, y, content_width, scale, color_muted);
    SDL_RenderSetClipRect(w->renderer, clipped ? &old_clip : NULL);
}

static void draw_control_status(ph_window *w, int width)
{
    int scale = text_scale(width), margin = 6 * scale;
    SDL_SetRenderDrawBlendMode(w->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(w->renderer, 12, 23, 35, 235);
    SDL_Rect panel = {0, 0, width, 28 * scale};
    SDL_RenderFillRect(w->renderer, &panel);
    SDL_SetRenderDrawBlendMode(w->renderer, SDL_BLENDMODE_NONE);
    char line[160];
    snprintf(line, sizeof line, "Viewing only: %s\nF2: guided setup",
             w->control_detail[0] ? w->control_detail : "controls disconnected");
    draw_text(w->renderer, line, margin, margin, width - 2 * margin, scale, color_text);
}

static void setup_buttons(int width, int height, SDL_Rect buttons[3])
{
    int scale = text_scale(width), margin = 12 * scale, gap = 4 * scale;
    int button_width = (width - 2 * margin - 2 * gap) / 3;
    for (int i = 0; i < 3; i++)
        buttons[i] = (SDL_Rect){margin + i * (button_width + gap),
                                height - margin - 24 * scale, button_width, 24 * scale};
}

static void draw_setup(ph_window *w, int width, int height)
{
    const ph_setup *s = &w->setup;
    int scale = text_scale(width), margin = 12 * scale, content_width = width - 2 * margin;
    SDL_SetRenderDrawBlendMode(w->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(w->renderer, 12, 23, 35, 245);
    SDL_Rect panel = {0, 0, width, height};
    SDL_RenderFillRect(w->renderer, &panel);
    SDL_SetRenderDrawBlendMode(w->renderer, SDL_BLENDMODE_NONE);
    SDL_Rect buttons[3];
    setup_buttons(width, height, buttons);
    SDL_Rect clip = {margin, margin, content_width, buttons[0].y - 2 * margin};
    SDL_RenderSetClipRect(w->renderer, &clip);
    char heading[80];
    snprintf(heading, sizeof heading, "Setup %d/%d", s->controls ? (int)s->step + 1
                                                                : s->step == PH_SETUP_MIRROR ? 1 : 2,
             s->controls ? 5 : 2);
    int y = draw_text(w->renderer, heading, margin, margin, content_width, scale + 1, color_accent);
    y += 8 * scale;
    const char *title = "", *body = "";
    switch (s->step) {
    case PH_SETUP_PREPARE:
        title = "Prepare your iPhone";
        body = "On your iPhone:\nSettings > Accessibility > Touch > AssistiveTouch.\n"
               "Turn AssistiveTouch On.\nTurn Dwell Control Off.\n\n"
               "Keep Tracking Sensitivity fixed.\nPress Next once you have checked these settings.";
        break;
    case PH_SETUP_BLUETOOTH:
        title = "Connect Bluetooth";
        body = "On iPhone, open Settings > Bluetooth.\nSelect Porthole under Other Devices.\n\n"
               "Wait for all controls to connect. If pairing fails, forget the old Porthole entry and pair again.";
        break;
    case PH_SETUP_MIRROR:
        title = "Mirror your screen";
        body = "Use the same Wi-Fi network.\nOn iPhone, open Control Center > Screen Mirroring.\n"
               "Select Porthole.\n\nClose the iPhone's mirroring panel once connected.";
        break;
    case PH_SETUP_CALIBRATE:
        title = "Check pointer alignment";
        body = "Leave your iPhone on a still page, such as Calculator.\n"
               "The pointer will move to three targets. No taps are sent.\n\n"
               "Leave the phone untouched during the check. Calibration is saved only after all three targets are confirmed.";
        break;
    case PH_SETUP_FINISH:
        title = "Ready to use";
        body = s->controls
                   ? "Point, click, drag and type through the mirror.\nF2 opens this guide again.\n\n"
                     "Optional shortcuts: assign AssistiveTouch buttons 3-6 to App Switcher, Control Center, Notifications and Screenshot. F1 lists the keys."
                   : "Your iPhone screen is connected.\nThis build is viewing only.\nF2 opens this guide again.";
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
        draw_text(w->renderer, error, margin, y + 6 * scale, content_width, scale, color_muted);
    }
    SDL_RenderSetClipRect(w->renderer, NULL);
    const char *next = s->step == PH_SETUP_FINISH ? "Finish"
                       : s->calibrating ? "Wait"
                       : s->step == PH_SETUP_CALIBRATE && !s->calibrated
                             ? s->error != PH_SETUP_OK ? "Retry" : "Start" : "Next";
    const char *labels[] = {"Close", "Back", next};
    for (int i = 0; i < 3; i++) {
        SDL_SetRenderDrawColor(w->renderer, 31, 59, 78, 255);
        SDL_RenderFillRect(w->renderer, &buttons[i]);
        int text_width = (int)strlen(labels[i]) * 6 * scale;
        draw_text(w->renderer, labels[i], buttons[i].x + (buttons[i].w - text_width) / 2,
                   buttons[i].y + 8 * scale, buttons[i].w, scale, color_text);
    }
}

static void diagnostic_line(char out[160], const char *name, const ph_metric_summary *value)
{
    if (value->known)
        snprintf(out, 160, "%s: %.0f ms (p95 %.0f)", name, value->last_ms, value->p95_ms);
    else
        snprintf(out, 160, "%s: --", name);
}

static void draw_diagnostics(ph_window *w, int width, int height)
{
    int scale = text_scale(width), margin = 8 * scale;
    SDL_Rect panel = {0, 0, width, height < 260 * scale ? height : 260 * scale};
    SDL_SetRenderDrawBlendMode(w->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(w->renderer, 12, 23, 35, 240);
    SDL_RenderFillRect(w->renderer, &panel);
    SDL_SetRenderDrawBlendMode(w->renderer, SDL_BLENDMODE_NONE);
    int y = draw_text(w->renderer, "F3: close diagnostics", margin, margin,
                       width - 2 * margin, scale, color_accent);
    char line[160];
    if (w->metrics.fps_known)
        snprintf(line, sizeof line, "Displayed video FPS: %.1f", w->metrics.fps);
    else
        snprintf(line, sizeof line, "Displayed video FPS: --");
    y = draw_text(w->renderer, line, margin, y, width - 2 * margin, scale, color_text);
    const char *names[] = {"Receive to decode", "Receive to present", "Input handoff", "Helper queue ACK"};
    const ph_metric_summary *values[] = {&w->metrics.receive_to_decode, &w->metrics.receive_to_present,
                                         &w->metrics.input_handoff, &w->metrics.helper_ack};
    for (int i = 0; i < 4; i++) {
        diagnostic_line(line, names[i], values[i]);
        y = draw_text(w->renderer, line, margin, y, width - 2 * margin, scale, color_text);
    }
    if (w->metrics.bluetooth_interval_known)
        snprintf(line, sizeof line, "Bluetooth interval: %.2f ms", w->metrics.bluetooth_interval_ms);
    else
        snprintf(line, sizeof line, "Bluetooth interval: not measured");
    y = draw_text(w->renderer, line, margin, y, width - 2 * margin, scale, color_text);
    snprintf(line, sizeof line, "HID pending: %zu  expired: %llu", w->metrics.pending_acks,
             (unsigned long long)w->metrics.acks_expired);
    y = draw_text(w->renderer, line, margin, y, width - 2 * margin, scale, color_text);
    snprintf(line, sizeof line, "Video queue: %u  discarded: %llu  superseded: %llu", w->queued_packets,
             (unsigned long long)w->discarded_packets, (unsigned long long)w->superseded_frames);
    y = draw_text(w->renderer, line, margin, y, width - 2 * margin, scale, color_text);
    draw_text(w->renderer, "Local timings only. Helper ACK excludes radio and phone response. Phone capture latency is not measured.",
               margin, y + 6 * scale, width - 2 * margin, scale, color_muted);
}

ph_window *ph_window_create(const char *title)
{
    ph_window *w = calloc(1, sizeof *w);

    if (!w)
        return NULL;
    w->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 360, 720,
                                 SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!w->window)
        goto fail;
    /* No vsync: presenting a frame must not block this thread, which also forwards
     * mouse and keyboard input, for up to a whole display refresh. */
    w->renderer = SDL_CreateRenderer(w->window, -1, SDL_RENDERER_ACCELERATED);
    if (!w->renderer) {
        PH_LOGW("no accelerated renderer (%s); using software rendering", SDL_GetError());
        w->renderer = SDL_CreateRenderer(w->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!w->renderer)
        goto fail;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    ph_window_redraw(w);
    return w;

fail:
    PH_LOGE("couldn't create the window: %s", SDL_GetError());
    ph_window_destroy(w);
    return NULL;
}

void ph_window_set_title(ph_window *w, const char *title)
{
    SDL_SetWindowTitle(w->window, title);
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

static void fit_window_to_picture(ph_window *w, int pic_w, int pic_h)
{
    SDL_Rect usable;
    int display = SDL_GetWindowDisplayIndex(w->window);
    double scale;

    if (display < 0 || SDL_GetDisplayUsableBounds(display, &usable) != 0) {
        usable.w = 1280;
        usable.h = 800;
    }
    scale = SDL_min(FIT_SCREEN_FRACTION * usable.w / pic_w, FIT_SCREEN_FRACTION * usable.h / pic_h);
    scale = SDL_min(scale, 1.0); /* don't upscale beyond the stream's own size by default */
    SDL_SetWindowSize(w->window, (int)(pic_w * scale), (int)(pic_h * scale));
    w->fit_w = pic_w;
    w->fit_h = pic_h;
    w->pending_w = w->pending_h = 0;
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
    int out_w, out_h;

    SDL_SetRenderDrawColor(w->renderer, 0, 0, 0, 255);
    SDL_RenderClear(w->renderer);
    if (SDL_GetRendererOutputSize(w->renderer, &out_w, &out_h) != 0)
        return;
    if (w->has_picture && w->texture) {
        SDL_Rect dst = fit_rect(out_w, out_h, w->screen.w, w->screen.h);
        SDL_RenderCopy(w->renderer, w->texture, &w->screen, &dst);
        if (!w->control_ready)
            draw_control_status(w, out_w);
    } else {
        draw_waiting(w, out_w, out_h);
    }
    if (w->help_visible)
        draw_help(w, out_w, out_h);
    if (w->diagnostics_visible && !w->help_visible && !w->setup.visible)
        draw_diagnostics(w, out_w, out_h);
    if (w->setup.visible)
        draw_setup(w, out_w, out_h);
    SDL_RenderPresent(w->renderer);
}

void ph_window_clear(ph_window *w)
{
    w->has_picture = false;
    ph_window_redraw(w);
}

bool ph_window_to_video(const ph_window *w, int x, int y, double *vx, double *vy)
{
    int win_w, win_h;
    SDL_Rect r;

    if (!w->has_picture || w->help_visible || w->setup.visible || !w->control_ready)
        return false;
    SDL_GetWindowSize(w->window, &win_w, &win_h);
    r = fit_rect(win_w, win_h, w->screen.w, w->screen.h);
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

void ph_window_destroy(ph_window *w)
{
    if (!w)
        return;
    if (w->texture)
        SDL_DestroyTexture(w->texture);
    if (w->renderer)
        SDL_DestroyRenderer(w->renderer);
    if (w->window)
        SDL_DestroyWindow(w->window);
    free(w);
}
