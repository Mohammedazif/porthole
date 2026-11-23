#include "ui/window.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <SDL.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#include "core/log.h"
#include "core/config.h"
#include "core/version.h"
#include "files/places.h"
#include "ui/text.h"
#include "ui/brand_art.h"
#include "ui/control_icons.h"
#include "ui/drawing.h"
#include "ui/accessibility.h"
#include "ui/placement.h"
#include "ui/vendor/qrcodegen.h"

#define RESIZE_SETTLE_MS 400 /* a new picture size must last this long before resizing */
#define WAITING_W 1024
#define WAITING_H 680
#define RESIZE_EDGE_PX 12
#define RAIL_W 60
#define TRANSFERS_W 300

struct ph_window {
    SDL_Window *window;
    SDL_Renderer *renderer;
    ph_workspace *workspace;
#ifdef _WIN32
    ph_accessibility *accessibility;
#endif
    ph_workspace_action workspace_action;
    ph_workspace_view page;
    uint64_t action_item;
    char incoming_file[128];
    uint64_t incoming_bytes, incoming_size;
    int settings_width, settings_expansion, rail_expansion, transfers_expansion;
    int receiving_done, receiving_total, volume;
    char notice[160];
    Uint64 notice_until;
    bool workspace_drawn, companion_connected, recording, dark, clipboard_available;
    unsigned unread_files;
    char unread_key[80];
    SDL_Texture *texture;
    int tex_w, tex_h;
    SDL_YUV_CONVERSION_MODE yuv_mode;
    bool has_picture;
    SDL_Rect screen;          /* phone-screen part of the texture */
    int fit_w, fit_h;         /* screen size the window was last fitted to */
    int pending_w, pending_h; /* a different size waiting to settle */
    Uint64 pending_since;
    bool mirroring, control_ready, help_visible;
    bool bluetooth_ready;
    ph_control_policy input_policy;
    char control_detail[96];
    ph_setup setup;
    bool diagnostics_visible;
    ph_metrics_snapshot metrics;
    unsigned int queued_packets;
    uint64_t discarded_packets, superseded_frames;
    SDL_Texture *lockup;
    SDL_Texture *mark;
    SDL_Texture *mark_on_dark; /* the same mark, light, for the dark phone bar */
    char title[320];
    char pull_hint[96];
    char pc_name[64];
    char phone_label[64];
    bool code_open;
    bool pairing_visible;
    bool share_checked, share_available;
    bool new_code;
    bool settings_visible;
    bool ctrl_command;
    bool release_blur;
    bool media_keys;
    bool feed_on, paused, sound_on; /* app state shown in Settings */
    bool next_relative;
    char save_label[128];
    char save_about[256];
    bool save_custom;
    ph_window_action pending_action;
    char transfer_lines[6][72];
    int transfer_count;
    char transfer_active[72];
    int transfer_percent;
    int sized_w, sized_h; /* window size after the last aspect fit */
    bool transfer_cancel;
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
    bool transfers_unseen;
};

static void place_window(ph_window *w, int pic_w, int pic_h, bool center);
static void place_waiting(ph_window *w);
static SDL_HitTestResult hit_edges(SDL_Window *window, const SDL_Point *p, void *data);
#ifdef _WIN32
static void accessible_action(void *context);
#endif

enum {
    CHROME_NONE,
    CHROME_MIN,
    CHROME_MAX,
    CHROME_CLOSE,
    CHROME_DRAG,
    CHROME_SETUP,
    CHROME_HELP,
    CHROME_STATS,
    CHROME_COPY,
    CHROME_NEW_CODE,
    CHROME_SETTINGS,
    CHROME_CANCEL,
    CHROME_STOP_RECORDING,
    CHROME_DISMISS_STATS,
    CHROME_RAIL,
    CHROME_TRANSFERS,
    CHROME_TAB_FILES,
    CHROME_TAB_CLIP,
    CHROME_PANEL,
    CHROME_SET = 40,
    CHROME_ACT = 100
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
static const SDL_Color color_muted = {92, 96, 104, 255};
static const SDL_Color color_warn = {154, 52, 18, 255};
static const SDL_Color color_on_ink = {255, 255, 255, 255};
static const SDL_Color color_stage = {11, 12, 13, 255};
static const SDL_Color color_stage_raised = {42, 45, 49, 255};
static const SDL_Color color_stage_muted = {150, 154, 160, 255};
static const SDL_Color color_live = {61, 214, 140, 255};
static const SDL_Color color_line = {226, 228, 232, 255};
static const SDL_Color color_ground = {243, 244, 246, 255};
static const SDL_Color color_border = {213, 216, 221, 255};
static const SDL_Color color_ok = {22, 128, 60, 255};

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
    ph_ui_round_rect(renderer, box, radius, color);
}

static void fill_px(SDL_Renderer *renderer, int x, int y, int w, int h);
static void paint_icon(SDL_Renderer *renderer, SDL_Rect dest, SDL_Color color,
                        void (*build)(float *cov, int n, int kind), int kind);
static void icon_ready(float *cov, int n, int kind);
static void modal_range(ph_window *w, int content, int visible);
static void stroke(SDL_Renderer *renderer, int x1, int y1, int x2, int y2);
static void draw_line_fit(SDL_Renderer *renderer, const char *text, int x, int y, int width, int scale,
                          SDL_Color color);

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
    if (!primary && !disabled) rounded(w->renderer, box, 8, (SDL_Color){213, 216, 221, 255});
    SDL_Rect inside = box;
    if (!primary && !disabled) { inside.x++; inside.y++; inside.w -= 2; inside.h -= 2; }
    rounded(w->renderer, inside, primary || disabled ? 8 : 7, bg);
    int scale = PH_TEXT(box.h < 32 ? 13 : 14, PH_FONT_MEDIUM);
    int tw = ph_text_width(label, scale);
    draw_text(w->renderer, label, box.x + (box.w - tw) / 2,
               box.y + (box.h - ph_text_line_height(scale)) / 2 - 1, box.w, scale, fg);
}

#define CONNECT_SUBTITLE \
    "Open Porthole on your iPhone and tap this PC in the list. Keep both on the same Wi-Fi network."

typedef struct connect_boxes {
    SDL_Rect copy, setup, help, stats, fresh, gear;
    SDL_Rect card, ready, qr;
    int left, inner, title_y, footer_y;
    int details_x, details_y; /* where the name, address and code start */
    bool wide;
} connect_boxes;

/* The connect screen: a pairing card (QR, address, code) and what is ready on this
 * PC, side by side when wide and stacked when narrow, over a footer of shortcuts. */
static connect_boxes connect_layout(int width, int height, int top)
{
    connect_boxes g;
    int pad = width < 560 ? 20 : 48;
    int body_y, qr = 164;

    memset(&g, 0, sizeof g);
    g.inner = width - pad * 2;
    if (g.inner > 1120) g.inner = 1120;
    g.left = (width - g.inner) / 2;
    g.wide = g.inner >= 760;
    g.title_y = top + (g.wide ? 36 : 22);
    body_y = g.title_y + (g.wide ? 48 : 36)
             + ph_text_height(CONNECT_SUBTITLE, g.wide ? 820 : g.inner, PH_TEXT(g.wide ? 15 : 14, PH_FONT_REGULAR))
             + (g.wide ? 24 : 16);
    g.footer_y = height - 44;
    if (g.wide) {
        int card_w = (g.inner - 24) * 57 / 100;
        g.card = (SDL_Rect){g.left, body_y, card_w, 330};
        g.ready = (SDL_Rect){g.left + card_w + 24, body_y + 24, g.inner - card_w - 24, 4 * 64 + 24};
    } else {
        /* Narrow: the code and buttons stay, the note under them goes, rows are compact.
         * A phone-sized window puts the QR above the address instead of beside it. */
        bool on_top = g.inner < 420;
        qr = g.inner < 560 ? 132 : 164;
        if (on_top && body_y + 20 + qr + 42 + 228 > g.footer_y - 8)
            on_top = false, qr = 0; /* the smallest windows keep the address and code only */
        g.card = (SDL_Rect){g.left, body_y, g.inner, on_top ? 20 + qr + 42 + 228 : 252};
        g.ready = (SDL_Rect){g.left, g.card.y + g.card.h + 44, g.inner, 4 * 48};
        if (on_top) {
            g.qr = (SDL_Rect){g.card.x + (g.card.w - qr) / 2, g.card.y + 20, qr, qr};
            g.details_x = g.card.x + 24;
            g.details_y = g.qr.y + qr + 42;
        }
    }
    if (qr && !g.qr.w)
        g.qr = (SDL_Rect){g.card.x + 24, g.card.y + 24, qr, qr};
    if (!g.details_y) {
        g.details_x = qr ? g.qr.x + qr + 28 : g.card.x + 24;
        g.details_y = g.card.y + 24;
    }
    {
        int dx = g.details_x;
        int dw = g.card.x + g.card.w - 24 - dx;
        int by = g.card.y + g.card.h - 24 - 36;
        int cw = dw < 242 ? (dw - 10) * 55 / 100 : 128;
        g.copy = (SDL_Rect){dx, by, cw, 36};
        g.fresh = (SDL_Rect){dx + cw + 10, by, dw < 242 ? dw - cw - 10 : 104, 36};
    }
    /* The link sits in the third row; no row, no link. */
    if (g.wide && g.ready.y + 64 * 2 + 88 <= g.footer_y - 12)
        g.setup = (SDL_Rect){g.ready.x + 56, g.ready.y + 64 * 2 + 52, 120, 26};
    else if (!g.wide && g.ready.y + 48 * 3 <= g.footer_y - 12)
        g.setup = (SDL_Rect){g.ready.x + g.ready.w - 18 - 104, g.ready.y + 96 + 12, 108, 24};
    g.help = (SDL_Rect){g.left, g.footer_y + 10, 150, 24};
    g.stats = (SDL_Rect){g.left + (g.wide ? 174 : 70), g.footer_y + 10, g.wide ? 160 : 60, 24};
    g.gear = (SDL_Rect){g.left + g.inner - 64, g.footer_y + 10, 64, 24};
    if (!g.wide) {
        g.help.w = 60;
    }
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

static void draw_qr(SDL_Renderer *renderer, const char *text, SDL_Rect box)
{
    uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
    int n, span, scale, side, ox, oy, x, y;

    if (!text || !text[0]
        || !qrcodegen_encodeText(text, temp, qr, qrcodegen_Ecc_MEDIUM, 1, 8,
                                 qrcodegen_Mask_AUTO, true))
        return;
    n = qrcodegen_getSize(qr);
    span = n + 4;
    scale = box.w < box.h ? box.w / span : box.h / span;
    if (scale < 2)
        return;
    side = span * scale;
    ox = box.x + (box.w - side) / 2;
    oy = box.y + (box.h - side) / 2;
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    {
        SDL_Rect quiet = {ox, oy, side, side};
        SDL_RenderFillRect(renderer, &quiet);
    }
    SDL_SetRenderDrawColor(renderer, 17, 17, 17, 255);
    for (y = 0; y < n; y++) {
        for (x = 0; x < n; x++) {
            SDL_Rect dot;
            if (!qrcodegen_getModule(qr, x, y))
                continue;
            dot = (SDL_Rect){ox + (x + 2) * scale, oy + (y + 2) * scale, scale, scale};
            SDL_RenderFillRect(renderer, &dot);
        }
    }
}

/* Settings: one column of sections. Each row is a name, a line about it, and one
 * control: a switch, a two-way choice, or a button. */
enum { SET_SWITCH, SET_CHOICE, SET_BUTTON };
enum {
    SET_POINTER, SET_WHEEL, SET_PAUSE, SET_GUIDE, SET_CTRL, SET_MEDIA, SET_BLUR, SET_SOUND,
    SET_SAVE, SET_COPY, SET_FORGET, SET_QUIT, SET_ROWS
};

typedef struct setting_row {
    const char *section; /* starts a section when set */
    const char *name;
    const char *about;
    int kind;
    const char *left, *right; /* choice labels, or the button label in left */
} setting_row;

static const setting_row setting_rows[SET_ROWS] = {
    [SET_POINTER] = {"CONTROLS", "Pointer", "Absolute puts the iPhone pointer where your mouse is. "
                     "A change applies next time Porthole opens, after pairing again.",
                     SET_CHOICE, "Absolute", "Relative"},
    [SET_WHEEL] = {NULL, "Wheel scrolling", "Feed turns each wheel burst into one swipe, for Reels "
                   "and Shorts. F4 switches.", SET_CHOICE, "Normal", "Feed"},
    [SET_PAUSE] = {NULL, "Pause controls", "Keep watching without sending your mouse and keyboard.",
                   SET_SWITCH, NULL, NULL},
    [SET_GUIDE] = {NULL, "Setup guide", "Pair over Bluetooth and check the pointer lines up.",
                   SET_BUTTON, "Open", NULL},
    [SET_CTRL] = {"KEYBOARD", "Ctrl acts as Command", "Ctrl+C, Ctrl+V and Ctrl+A work on the iPhone.",
                  SET_SWITCH, NULL, NULL},
    [SET_MEDIA] = {NULL, "Media keys go to the iPhone", "Play, pause, next track and volume keys.",
                   SET_SWITCH, NULL, NULL},
    [SET_BLUR] = {NULL, "Release keys when Porthole loses focus", "Stops a key staying held on the iPhone.",
                  SET_SWITCH, NULL, NULL},
    [SET_SOUND] = {"SOUND", "Play iPhone sound on this PC", "Uses this PC's default speakers.",
                   SET_SWITCH, NULL, NULL},
    [SET_SAVE] = {"FILES", "Save files from the iPhone", NULL, SET_CHOICE, "Downloads", "Folder"},
    [SET_COPY] = {"PAIRING", "Pairing details", NULL, SET_BUTTON, "Copy", NULL},
    [SET_FORGET] = {NULL, "Forget this iPhone", "It needs the pairing code again to connect.",
                    SET_BUTTON, "Forget", NULL},
    [SET_QUIT] = {"ABOUT", "Porthole " PORTHOLE_VERSION, "Free software under GPL-3.0.", SET_BUTTON,
                  "Quit", NULL},
};

static bool setting_on(const ph_window *w, int row)
{
    switch (row) {
    case SET_POINTER: return w->next_relative;
    case SET_WHEEL: return w->feed_on;
    case SET_PAUSE: return w->paused;
    case SET_CTRL: return w->ctrl_command;
    case SET_MEDIA: return w->media_keys;
    case SET_BLUR: return w->release_blur;
    case SET_SOUND: return w->sound_on;
    case SET_SAVE: return w->save_custom;
    default: return false;
    }
}

/* The save-folder line changes the height of that settings row, so refresh it
 * before laying the rows out. */
static void load_save_place(ph_window *w)
{
    char label[128];

    if (!ph_place_save_label(label, sizeof label))
        snprintf(label, sizeof label, "Folder unavailable");
    snprintf(w->save_label, sizeof w->save_label, "%s", label);
    w->save_custom = ph_place_save_is_custom();
    if (w->save_custom)
        snprintf(w->save_about, sizeof w->save_about, "Saving in %s.", label);
}

typedef struct setting_box {
    SDL_Rect row, control, left, right;
    int text_w;
    bool stacked;
} setting_box;

static int settings_column(int width, int *left)
{
    int inner = width - 40;
    if (inner > 600) inner = 600;
    *left = (width - inner) / 2;
    return inner;
}

static const char *setting_about(const ph_window *w, int row)
{
    if (row == SET_COPY)
        return w->pull_hint[0] ? w->pull_hint : "Shown once the iPhone connection starts.";
    if (row == SET_SAVE)
        return w->save_custom && w->save_about[0] ? w->save_about
                                                  : "Downloads, unless you choose another folder.";
    return setting_rows[row].about;
}

/* Lays out rows top to bottom from the top of the scrolled content. */
static setting_box setting_layout(const ph_window *w, int width, int row, int *y)
{
    setting_box b;
    int left, inner = settings_column(width, &left);
    int control_w = setting_rows[row].kind == SET_CHOICE ? 184 : setting_rows[row].kind == SET_SWITCH ? 40 : 84;
    int text_h;

    memset(&b, 0, sizeof b);
    if (setting_rows[row].section)
        *y += 44;
    b.stacked = inner - 32 - control_w - 16 < 180;
    b.text_w = b.stacked ? inner - 32 : inner - 32 - control_w - 16;
    text_h = 20 + ph_text_height(setting_about(w, row), b.text_w, PH_TEXT(13, PH_FONT_REGULAR));
    b.row = (SDL_Rect){left, *y, inner, (b.stacked ? text_h + 12 + 32 : (text_h > 32 ? text_h : 32)) + 28};
    b.control = b.stacked
        ? (SDL_Rect){left + 16, *y + 14 + text_h + 12, control_w, 32}
        : (SDL_Rect){left + inner - 16 - control_w, *y + (b.row.h - 32) / 2, control_w, 32};
    if (setting_rows[row].kind == SET_SWITCH)
        b.control = (SDL_Rect){b.control.x, b.control.y + 6, 40, 22};
    b.left = (SDL_Rect){b.control.x, b.control.y, b.control.w / 2, b.control.h};
    b.right = (SDL_Rect){b.control.x + b.control.w / 2, b.control.y, b.control.w - b.control.w / 2, b.control.h};
    *y += b.row.h;
    return b;
}

static int settings_top(void)
{
    return PH_WINDOW_BAR_PX + 76;
}

static SDL_Rect settings_done_box(int width)
{
    int left, inner = settings_column(width, &left);
    return (SDL_Rect){left + inner - 72, PH_WINDOW_BAR_PX + 22, 72, 32};
}

/* The hit code for a point: CHROME_SET + row * 2 + side (side 1 is a choice's right half). */
static int settings_hit(const ph_window *w, int width, int height, SDL_Point p)
{
    int y = settings_top() - w->modal_scroll;
    SDL_Rect done = settings_done_box(width);

    if (SDL_PointInRect(&p, &done))
        return CHROME_SETTINGS;
    if (p.y < settings_top() - 8)
        return CHROME_PANEL;
    for (int row = 0; row < SET_ROWS; row++) {
        setting_box b = setting_layout(w, width, row, &y);
        SDL_Rect hit = b.control;
        if (b.row.y > height)
            break;
        if (setting_rows[row].kind == SET_SWITCH)
            hit = b.row; /* the whole row flips a switch */
        if (SDL_PointInRect(&p, &hit))
            return CHROME_SET + row * 2
                   + (setting_rows[row].kind == SET_CHOICE && SDL_PointInRect(&p, &b.right));
    }
    return CHROME_PANEL;
}

static void draw_switch(SDL_Renderer *r, SDL_Rect box, bool on)
{
    rounded(r, box, box.h / 2, on ? color_text : (SDL_Color){213, 216, 221, 255});
    rounded(r, (SDL_Rect){on ? box.x + box.w - box.h + 2 : box.x + 2, box.y + 2, box.h - 4, box.h - 4},
            (box.h - 4) / 2, color_on_ink);
}

static void draw_settings(ph_window *w, int width, int height)
{
    SDL_Renderer *r = w->renderer;
    int left, inner = settings_column(width, &left);

    load_save_place(w);
    int y = settings_top() - w->modal_scroll, content;
    SDL_Rect done = settings_done_box(width), clip = {0, settings_top() - 8, width, height};

    paint(r, (SDL_Rect){0, PH_WINDOW_BAR_PX, width, height - PH_WINDOW_BAR_PX}, 243, 244, 246, 255);
    SDL_RenderSetClipRect(r, &clip);
    for (int row = 0; row < SET_ROWS; row++) {
        const setting_row *s = &setting_rows[row];
        int top_of_section = y;
        setting_box b = setting_layout(w, width, row, &y);
        bool on = setting_on(w, row), first = s->section != NULL;
        bool last = row + 1 == SET_ROWS || setting_rows[row + 1].section;
        bool hot_row = w->chrome_hover >= CHROME_SET + row * 2 && w->chrome_hover <= CHROME_SET + row * 2 + 1;

        if (first)
            draw_text(r, s->section, left + 4, top_of_section + 20, inner, PH_TEXT(11, PH_FONT_MEDIUM),
                      color_muted);
        /* One white card per section: corners only at its first and last rows. */
        {
            SDL_Rect card = b.row;
            rounded(r, card, first || last ? 12 : 0, color_line);
            if (!first) { paint(r, (SDL_Rect){card.x, card.y, card.w, 12}, 226, 228, 232, 255); }
            if (!last) { paint(r, (SDL_Rect){card.x, card.y + card.h - 12, card.w, 12}, 226, 228, 232, 255); }
            SDL_Rect fill = {card.x + 1, card.y + (first ? 1 : 0), card.w - 2, card.h - (first ? 1 : 0) - (last ? 1 : 0)};
            rounded(r, fill, first || last ? 11 : 0, color_on_ink);
            if (!first) paint(r, (SDL_Rect){fill.x, fill.y, fill.w, 12}, 255, 255, 255, 255);
            if (!last) paint(r, (SDL_Rect){fill.x, fill.y + fill.h - 12, fill.w, 12}, 255, 255, 255, 255);
            if (!last) paint(r, (SDL_Rect){card.x + 16, card.y + card.h - 1, card.w - 32, 1}, 226, 228, 232, 255);
        }
        draw_text(r, s->name, b.row.x + 16, b.row.y + 14, b.text_w, PH_TEXT(14, PH_FONT_MEDIUM),
                  row == SET_FORGET ? color_warn : color_text);
        draw_text(r, setting_about(w, row), b.row.x + 16, b.row.y + 34, b.text_w,
                  row == SET_COPY && w->pull_hint[0] ? PH_TEXT(13, PH_FONT_MONO) : PH_TEXT(13, PH_FONT_REGULAR),
                  color_muted);
        if (s->kind == SET_SWITCH) {
            draw_switch(r, b.control, on);
        } else if (s->kind == SET_CHOICE) {
            SDL_Rect pick = on ? b.right : b.left;
            rounded(r, b.control, 9, color_ground);
            rounded(r, (SDL_Rect){pick.x + 3, pick.y + 3, pick.w - 6, pick.h - 6}, 7, color_border);
            rounded(r, (SDL_Rect){pick.x + 3, pick.y + 3, pick.w - 6, pick.h - 7}, 7, color_on_ink);
            draw_text(r, s->left, b.left.x + (b.left.w - ph_text_width(s->left, PH_TEXT(13, PH_FONT_MEDIUM))) / 2,
                      b.left.y + 7, b.left.w, PH_TEXT(13, PH_FONT_MEDIUM), on ? color_muted : color_text);
            draw_text(r, s->right, b.right.x + (b.right.w - ph_text_width(s->right, PH_TEXT(13, PH_FONT_MEDIUM))) / 2,
                      b.right.y + 7, b.right.w, PH_TEXT(13, PH_FONT_MEDIUM), on ? color_text : color_muted);
        } else {
            const char *label = row == SET_COPY && w->copied_until ? "Copied" : s->left;
            bool disabled = row == SET_COPY && !w->pull_hint[0];
            draw_button(w, b.control, label, false, disabled, hot_row);
        }
    }
    SDL_RenderSetClipRect(r, NULL);
    content = y + w->modal_scroll - settings_top() + 24;
    modal_range(w, content, height - settings_top());
    paint(r, (SDL_Rect){0, PH_WINDOW_BAR_PX, width, settings_top() - 8 - PH_WINDOW_BAR_PX}, 243, 244, 246, 255);
    draw_text(r, "Settings", left, PH_WINDOW_BAR_PX + 22, inner - 90, PH_TEXT(24, PH_FONT_SEMIBOLD), color_text);
    draw_button(w, done, "Done", true, false, w->chrome_hover == CHROME_SETTINGS);
}

/* Live bar, right to left: window buttons, a divider, Transfers, Controls, Settings. */
static SDL_Rect live_settings_box(int win_w)
{
    return (SDL_Rect){win_w - 120 - 9 - 3 * 32, 4, 30, PH_WINDOW_BAR_PX - 8};
}

static void outline(SDL_Renderer *r, SDL_Rect box, int radius, SDL_Color line, SDL_Color fill)
{
    rounded(r, box, radius, line);
    rounded(r, (SDL_Rect){box.x + 1, box.y + 1, box.w - 2, box.h - 2}, radius - 1, fill);
}

/* Small line icons in a 32 px tile: 0 Wi-Fi, 1 screen, 2 mouse, 3 sound. */
static void ready_icon(SDL_Renderer *r, int kind, int x, int y)
{
    int cx = x + 16, cy = y + 16;

    rounded(r, (SDL_Rect){x, y, 32, 32}, 8, color_ground);
    (void)cx;
    (void)cy;
    paint_icon(r, (SDL_Rect){x + 4, y + 4, 24, 24}, color_text, icon_ready, kind);
}

/* A coloured dot and label, right-aligned at right. tone: 0 muted ring, 1 ok, 2 warn. */
static void state_pill(SDL_Renderer *r, int right, int y, const char *label, int tone)
{
    SDL_Color ink = tone == 1 ? color_ok : tone == 2 ? color_warn : color_muted;
    int tw = ph_text_width(label, PH_TEXT(12, PH_FONT_MEDIUM));
    int x = right - tw;

    if (tone)
        rounded(r, (SDL_Rect){x - 14, y + 5, 8, 8}, 4, ink);
    else {
        rounded(r, (SDL_Rect){x - 14, y + 5, 8, 8}, 4, ink);
        rounded(r, (SDL_Rect){x - 12, y + 7, 4, 4}, 2, color_on_ink);
    }
    draw_text(r, label, x, y, tw + 4, PH_TEXT(12, PH_FONT_MEDIUM), ink);
}

static void kbd(SDL_Renderer *r, int x, int y, const char *key, const char *label, bool hot)
{
    int kw = ph_text_width(key, PH_TEXT(12, PH_FONT_MONO)) + 12;

    outline(r, (SDL_Rect){x, y, kw, 22}, 4, color_border, color_on_ink);
    draw_text(r, key, x + 6, y + 3, kw, PH_TEXT(12, PH_FONT_MONO), color_text);
    draw_text(r, label, x + kw + 8, y + 3, 200, PH_TEXT(12, PH_FONT_REGULAR), hot ? color_text : color_muted);
}

static void draw_waiting(ph_window *w, int width, int height, int top)
{
    SDL_Renderer *r = w->renderer;
    connect_boxes g = connect_layout(width, height, top);
    char address[80], code[40];
    bool have, unavailable;
    int dx, dw, y;

    hint_fields(w, address, sizeof address, code, sizeof code);
    have = address[0] && code[0];
    unavailable = w->share_checked && !w->share_available;
    paint(r, (SDL_Rect){0, top, width, height - top}, 243, 244, 246, 255);

    draw_text(r, "Connect your iPhone", g.left, g.title_y, g.inner,
              PH_TEXT(g.wide ? 32 : 24, PH_FONT_SEMIBOLD), color_text);
    draw_text(r, CONNECT_SUBTITLE, g.left, g.title_y + (g.wide ? 48 : 36), g.wide ? 820 : g.inner,
              PH_TEXT(g.wide ? 15 : 14, PH_FONT_REGULAR), color_muted);

    /* Pairing card */
    outline(r, g.card, 14, color_line, color_on_ink);
    if (g.qr.w) {
        char payload[128], compact[16];
        size_t n = 0;
        const char *cap = "Or scan with the iPhone app";
        outline(r, g.qr, 10, color_line, color_on_ink);
        for (const char *p = code; *p && n + 1 < sizeof compact; p++)
            if (*p != ' ')
                compact[n++] = *p;
        compact[n] = 0;
        if (have) {
            snprintf(payload, sizeof payload, "porthole://%s:47731/%s", address, compact);
            draw_qr(r, payload, (SDL_Rect){g.qr.x + 10, g.qr.y + 10, g.qr.w - 20, g.qr.h - 20});
        }
        draw_text(r, cap, g.qr.x + (g.qr.w - ph_text_width(cap, PH_TEXT(12, PH_FONT_REGULAR))) / 2,
                  g.qr.y + g.qr.h + 10, g.qr.w + 40, PH_TEXT(12, PH_FONT_REGULAR), color_muted);
    }
    dx = g.details_x;
    dw = g.card.x + g.card.w - 24 - dx;
    y = g.details_y;
    draw_text(r, "THIS PC APPEARS AS", dx, y, dw, PH_TEXT(11, PH_FONT_MEDIUM), color_muted);
    draw_text(r, w->pc_name[0] ? w->pc_name : "This PC", dx, y + 18, dw, PH_TEXT(20, PH_FONT_SEMIBOLD),
              color_text);
    y += 60;
    {
        SDL_Rect table = {dx, y, dw, 116};
        outline(r, table, 10, color_line, color_on_ink);
        paint(r, (SDL_Rect){dx + 1, y + 56, dw - 2, 1}, 226, 228, 232, 255);
        draw_text(r, "Address", dx + 14, y + 6, dw - 28, PH_TEXT(12, PH_FONT_REGULAR), color_muted);
        draw_text(r, "Pairing code", dx + 14, y + 62, dw - 28, PH_TEXT(12, PH_FONT_REGULAR), color_muted);
        if (have) {
            draw_text(r, address, dx + 14, y + 24, dw - 28, PH_TEXT(15, PH_FONT_MONO), color_text);
            if (w->code_open)
                draw_text(r, code, dx + 14, y + 80, dw - 28, PH_TEXT(16, PH_FONT_MONO_MEDIUM), color_text);
            else
                draw_text(r, "Used", dx + 14, y + 80, dw - 28, PH_TEXT(15, PH_FONT_MEDIUM), color_muted);
        } else {
            const char *wait = unavailable ? "Unavailable" : "Starting...";
            int ww = ph_text_width(wait, PH_TEXT(14, PH_FONT_REGULAR));
            draw_text(r, wait, dx + dw - 14 - ww, y + 22, ww + 4, PH_TEXT(14, PH_FONT_REGULAR), color_muted);
        }
        y += 116 + 14;
    }
    if (y + 40 < g.copy.y)
        draw_text(r, unavailable ? "The iPhone connection could not start. Another program may be using port 47731."
                                 : "Only needed if the PC doesn't show up in the app. New code stops the old one working.",
                  dx, y, dw, PH_TEXT(13, PH_FONT_REGULAR), unavailable ? color_warn : color_muted);
    draw_button(w, g.copy, w->copied_until ? "Copied" : "Copy details", true, !have,
                w->chrome_hover == CHROME_COPY);
    draw_button(w, g.fresh, "New code", false, !have, w->chrome_hover == CHROME_NEW_CODE);

    /* Ready on this PC */
    if (g.ready.y + 64 <= g.footer_y - 12) {
        static const char *const names[4] = {"Wi-Fi link", "Screen", "Mouse and keyboard", "Sound"};
        const char *subs[4], *states[4];
        int tones[4];
        SDL_Rect box = g.ready;
        int rows;

        /* Only whole rows: the Mouse and keyboard row is taller for its link. */
        for (rows = 0, box.h = 0; rows < 4; rows++) {
            int h = !g.wide ? 48 : rows == 2 ? 88 : 64;
            if (box.y + box.h + h > g.footer_y - 12)
                break;
            box.h += h;
        }
        if (!rows)
            box.h = 0;
        subs[0] = have ? "Listening on this Wi-Fi network" : unavailable ? "Could not start" : "Starting";
        states[0] = have ? "Ready" : unavailable ? "Unavailable" : "Starting";
        tones[0] = have ? 1 : unavailable ? 2 : 0;
        subs[1] = w->mirroring ? "Showing the iPhone screen" : "Starts when you tap Share screen on the iPhone";
        states[1] = w->mirroring ? "Live" : "Waiting";
        tones[1] = w->mirroring ? 1 : 0;
        bool input_inactive = !strcmp(w->control_detail, "controls inactive");
        subs[2] = w->control_ready ? "Connected via Bluetooth"
                  : input_inactive ? "Configure mouse and keyboard in the iPhone app."
                                   : "Set up Bluetooth controls to use a mouse and keyboard.";
        states[2] = w->control_ready ? "Connected" : input_inactive ? "Inactive" : "Setup required";
        tones[2] = w->control_ready ? 1 : input_inactive ? 0 : 2;
        subs[3] = "Plays through this PC's speakers";
        states[3] = "Ready";
        tones[3] = 1;
        draw_text(r, "READY ON THIS PC", box.x, box.y - 22, box.w, PH_TEXT(11, PH_FONT_MEDIUM), color_muted);
        if (box.h)
            outline(r, box, 14, color_line, color_on_ink);
        for (int i = 0, ry = box.y; i < rows; i++) {
            int h = !g.wide ? 48 : i == 2 ? 88 : 64;
            int pill = ph_text_width(states[i], PH_TEXT(12, PH_FONT_MEDIUM)) + 30;
            if (ry + 40 > box.y + box.h)
                break;
            if (!g.wide) {
                ready_icon(r, i, box.x + 12, ry + 8);
                draw_text(r, names[i], box.x + 56, ry + 14, box.w - 76 - pill, PH_TEXT(14, PH_FONT_MEDIUM),
                          color_text);
                if (i == 2 && !w->control_ready && g.setup.w) {
                    SDL_Color ink = w->chrome_hover == CHROME_SETUP ? color_muted : color_text;
                    int lw = ph_text_width("Set up controls", PH_TEXT(13, PH_FONT_MEDIUM));
                    draw_text(r, "Set up controls", box.x + box.w - 18 - lw, ry + 15, lw + 4,
                              PH_TEXT(13, PH_FONT_MEDIUM), ink);
                    paint(r, (SDL_Rect){box.x + box.w - 18 - lw, ry + 33, lw, 1}, ink.r, ink.g, ink.b, 255);
                } else {
                    state_pill(r, box.x + box.w - 18, ry + 15, states[i], tones[i]);
                }
                ry += h;
                if (i < rows - 1)
                    paint(r, (SDL_Rect){box.x + 1, ry, box.w - 2, 1}, 226, 228, 232, 255);
                continue;
            }
            ready_icon(r, i, box.x + 16, ry + 14);
            draw_text(r, names[i], box.x + 60, ry + 12, box.w - 76 - pill, PH_TEXT(14, PH_FONT_MEDIUM), color_text);
            draw_line_fit(r, subs[i], box.x + 60, ry + 32, box.w - 76, PH_TEXT(13, PH_FONT_REGULAR),
                          color_muted);
            state_pill(r, box.x + box.w - 18, ry + 13, states[i], tones[i]);
            if (i == 2) {
                SDL_Color ink = w->chrome_hover == CHROME_SETUP ? color_muted : color_text;
                draw_text(r, "Set up controls", box.x + 60, ry + 56, 140, PH_TEXT(13, PH_FONT_MEDIUM), ink);
                paint(r, (SDL_Rect){box.x + 60, ry + 74, ph_text_width("Set up controls", PH_TEXT(13, PH_FONT_MEDIUM)), 1},
                      ink.r, ink.g, ink.b, 255);
            }
            ry += h;
            if (i < rows - 1 && ry < box.y + box.h - 1)
                paint(r, (SDL_Rect){box.x + 1, ry, box.w - 2, 1}, 226, 228, 232, 255);
        }
    }

    /* Footer */
    paint(r, (SDL_Rect){0, g.footer_y, width, 1}, 226, 228, 232, 255);
    kbd(r, g.help.x, g.help.y, "F1", g.wide ? "Help and shortcuts" : "Help", w->chrome_hover == CHROME_HELP);
    kbd(r, g.stats.x, g.stats.y, "F3", g.wide ? "Connection stats" : "Stats", w->chrome_hover == CHROME_STATS);
    draw_text(r, "Settings", g.gear.x + g.gear.w - ph_text_width("Settings", PH_TEXT(12, PH_FONT_REGULAR)),
              g.gear.y + 3, g.gear.w, PH_TEXT(12, PH_FONT_REGULAR),
              w->chrome_hover == CHROME_SETTINGS ? color_text : color_muted);
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
                       "Ctrl+V pastes plain text into the field\n"
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
                        "Ctrl+V  Paste text into the field\n"
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

/* Setup: a numbered step list on the left when wide, the step's content beside it. */
typedef struct setup_frame {
    bool wide;
    SDL_Rect rail;     /* step list; empty when narrow */
    int left, width;   /* content column */
} setup_frame;

static setup_frame setup_layout(int width)
{
    setup_frame f;
    int outer = width - (width < 400 ? 40 : 56);

    memset(&f, 0, sizeof f);
    f.wide = width >= 760;
    if (outer > 980) outer = 980;
    f.left = (width - outer) / 2;
    f.width = outer;
    if (f.wide) {
        f.rail = (SDL_Rect){f.left, PH_WINDOW_BAR_PX + 32, 236, 5 * 56};
        f.left += 236 + 32;
        f.width = outer - 236 - 32;
    }
    return f;
}

static void setup_buttons(int width, int height, SDL_Rect buttons[3])
{
    setup_frame f = setup_layout(width);
    int button_width = (f.width - 20) / 3;
    if (f.wide && button_width > 132) button_width = 132;
    int x = f.wide ? f.left + f.width - 3 * button_width - 20 : f.left;
    for (int i = 0; i < 3; i++)
        buttons[i] = (SDL_Rect){x + i * (button_width + 10), height - 66, button_width, 40};
    buttons[0] = (SDL_Rect){width - 40, PH_WINDOW_BAR_PX + 8, 32, 32};
}

static bool setup_next_enabled(const ph_setup *setup)
{
    ph_setup copy = *setup;
    if (copy.step == PH_SETUP_CALIBRATE && !copy.calibrated)
        return ph_setup_start_calibration(&copy, 0);
    return ph_setup_next(&copy);
}

typedef struct {
    SDL_Color ink, muted, edge, bg, surface, success, warning;
} dialog_colors;
static dialog_colors dialog_theme(ph_window *w)
{
    if (ph_workspace_dark(w->workspace))
        return (dialog_colors){{233, 236, 242, 255}, {157, 170, 187, 255}, {55, 63, 75, 255},
                               {21, 25, 30, 255},    {30, 35, 42, 255},    {85, 206, 154, 255},
                               {252, 161, 143, 255}};
    return (dialog_colors){color_text,   color_muted, color_line, color_ground,
                           color_on_ink, color_ok,    color_warn};
}
static void dismiss_icon(SDL_Renderer *r, SDL_Rect b, SDL_Color color)
{
    int x = b.x + b.w / 2, y = b.y + b.h / 2;
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, 255);
    stroke(r, x - 5, y - 5, x + 5, y + 5);
    stroke(r, x - 5, y + 5, x + 5, y - 5);
}

static void draw_setup(ph_window *w, int width, int height, int top)
{
    dialog_colors ui = dialog_theme(w);
    const ph_setup *s = &w->setup;
    setup_frame frame = setup_layout(width);
    int margin = frame.left, content_width = frame.width;
    rounded(w->renderer, (SDL_Rect){0, top, width, height - top}, 0, ui.bg);
    SDL_Rect buttons[3];
    setup_buttons(width, height, buttons);
    int step = s->controls ? (int)s->step : s->step == PH_SETUP_MIRROR ? 0 : 1;
    int steps = s->controls ? 5 : 2;
    char heading[80];
    snprintf(heading, sizeof heading, "STEP %d OF %d", step + 1, steps);
    if (frame.wide) {
        static const char *const all[5] = {"Prepare your iPhone", "Pair over Bluetooth", "Share your screen",
                                           "Check pointer alignment", "Ready to use"};
        static const char *const view[2] = {"Share your screen", "Ready to use"};
        for (int i = 0; i < steps; i++) {
            SDL_Rect dot = {frame.rail.x, frame.rail.y + i * 56, 28, 28};
            bool done = i < step, now = i == step;
            char n[4];
            if (i + 1 < steps)
                paint(w->renderer, (SDL_Rect){dot.x + 13, dot.y + 32, 2, 20}, done ? 17 : 213, done ? 17 : 216,
                      done ? 17 : 221, 255);
            rounded(w->renderer, dot, 14, done || now ? ui.ink : ui.edge);
            if (!done && !now)
                rounded(w->renderer, (SDL_Rect){dot.x + 1, dot.y + 1, 26, 26}, 13, ui.bg);
            snprintf(n, sizeof n, "%d", i + 1);
            if (done) {
                SDL_SetRenderDrawColor(w->renderer, 255, 255, 255, 255);
                stroke(w->renderer, dot.x + 8, dot.y + 14, dot.x + 12, dot.y + 18);
                stroke(w->renderer, dot.x + 12, dot.y + 18, dot.x + 19, dot.y + 10);
            } else {
                draw_text(w->renderer, n,
                          dot.x + (28 - ph_text_width(n, PH_TEXT(13, PH_FONT_MEDIUM))) / 2,
                          dot.y + 5, 20, PH_TEXT(13, PH_FONT_MEDIUM), now ? ui.surface : ui.muted);
            }
            draw_text(w->renderer, s->controls ? all[i] : view[i], dot.x + 40, dot.y + 5,
                      frame.rail.w - 40, PH_TEXT(14, now ? PH_FONT_SEMIBOLD : PH_FONT_REGULAR),
                      now || done ? ui.ink : ui.muted);
        }
        draw_text(w->renderer, heading, margin, top + 32, content_width,
                  PH_TEXT(11, PH_FONT_MEDIUM), ui.muted);
    } else {
        int segment = (content_width - (steps - 1) * 6) / steps;
        draw_text(w->renderer, heading, margin, top + 22, content_width,
                  PH_TEXT(11, PH_FONT_MEDIUM), ui.muted);
        for (int i = 0; i < steps; i++)
            rounded(w->renderer, (SDL_Rect){margin + i * (segment + 6), top + 46, segment, 4}, 2,
                    i <= step ? ui.ink : (SDL_Color){213, 216, 221, 255});
    }
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
               "Pair this PC in the iPhone app. Use the QR code or enter the connection details.\n\n"
               "Tap Share screen. In the iOS dialog choose Porthole, then Start Broadcast.\n\n"
               "Keep both devices on the same Wi-Fi.";
        if (s->controls)
            body = "Open Porthole on your iPhone and pair with this PC.\n\n"
                   "Under Settings > Mouse and keyboard, select Verify Bluetooth.\n\n"
                   "Tap Share screen, then choose Porthole and Start Broadcast.";
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
                     "Find keyboard shortcuts in Settings > Help and shortcuts.\n\n"
                     "Optional: assign AssistiveTouch buttons 3 to 6 to App Switcher, Control Center, Notifications, and Screenshot."
                   : "Your iPhone screen is shared.";
        break;
    }
    int y = draw_text(w->renderer, title, margin, top + (frame.wide ? 52 : 64), content_width,
                      PH_TEXT(frame.wide ? 28 : 22, PH_FONT_SEMIBOLD), ui.ink);
    SDL_Rect card = {margin, y + 14, content_width, buttons[1].y - y - 34};
    rounded(w->renderer, card, 14, ui.edge);
    rounded(w->renderer, (SDL_Rect){card.x + 1, card.y + 1, card.w - 2, card.h - 2}, 13,
            ui.surface);
    SDL_Rect viewport = {card.x + 20, card.y + 18, card.w - 40, card.h - 36};
    if (viewport.h < 1) viewport.h = 1;
    char status[160] = "";
    if (s->step == PH_SETUP_BLUETOOTH)
        snprintf(status, sizeof status, "Bluetooth: %s", s->bluetooth_ready ? "connected" :
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
    if (s->step == PH_SETUP_MIRROR && w->code_open && w->pull_hint[0]) {
        snprintf(share_body, sizeof share_body, "%s\n\n%s%s", body,
                 "This PC: ", w->pull_hint);
        body = share_body;
    }
    int body_h = text_height(body, viewport.w, PH_TEXT(15, PH_FONT_REGULAR));
    int status_h = status[0] ? 18 + text_height(status, viewport.w - 16, PH_TEXT(13, PH_FONT_MEDIUM)) : 0;
    int error_h = error[0] ? 18 + text_height(error, viewport.w, PH_TEXT(13, PH_FONT_REGULAR)) : 0;
    bool good = (s->step == PH_SETUP_BLUETOOTH && s->bluetooth_ready)
                || (s->step == PH_SETUP_MIRROR && s->mirroring)
                || (s->step == PH_SETUP_CALIBRATE && s->calibrated);
    modal_range(w, body_h + status_h + error_h, viewport.h);
    SDL_RenderSetClipRect(w->renderer, &viewport);
    y = draw_text(w->renderer, body, viewport.x, viewport.y - w->modal_scroll, viewport.w,
                  PH_TEXT(15, PH_FONT_REGULAR), ui.ink);
    if (status[0]) {
        rounded(w->renderer, (SDL_Rect){viewport.x, y + 23, 8, 8}, 4, good ? ui.success : ui.muted);
        y = draw_text(w->renderer, status, viewport.x + 16, y + 18, viewport.w - 16,
                      PH_TEXT(13, PH_FONT_MEDIUM), good ? ui.success : ui.muted);
    }
    if (error[0])
        draw_text(w->renderer, error, viewport.x, y + 18, viewport.w, PH_TEXT(13, PH_FONT_REGULAR),
                  ui.warning);
    SDL_RenderSetClipRect(w->renderer, NULL);
    draw_scrollbar(w, viewport);
    const char *next = s->step == PH_SETUP_FINISH ? "Finish"
                       : s->calibrating ? "Wait"
                       : s->step == PH_SETUP_CALIBRATE && !s->calibrated
                             ? s->error != PH_SETUP_OK ? "Retry" : "Start" : "Next";
    const char *labels[] = {NULL, "Back", next};
    bool first = s->step == (s->controls ? PH_SETUP_PREPARE : PH_SETUP_MIRROR);
    dismiss_icon(w->renderer, buttons[0], ui.muted);
    for (int i = 1; i < 3; i++)
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
    dialog_colors ui = dialog_theme(w);
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
    rounded(w->renderer, panel, 9, ui.edge);
    rounded(w->renderer, (SDL_Rect){panel.x + 1, panel.y + 1, panel.w - 2, panel.h - 2}, 8,
            ui.surface);
    SDL_Rect clip = {panel.x + margin, panel.y + margin, inner, panel.h - 2 * margin};
    SDL_RenderSetClipRect(w->renderer, &clip);
    y = draw_text(w->renderer, "Connection statistics", clip.x, clip.y, inner, 2, ui.ink) + 12;
    int left = y, right = y;
    for (i = 0; i < 8; i++) {
        if (wide && i >= 4)
            right =
                draw_text(w->renderer, lines[i], clip.x + col + margin, right, col, scale, ui.ink);
        else
            left = draw_text(w->renderer, lines[i], clip.x, left, col, scale, ui.ink);
    }
    y = left > right ? left : right;
    draw_text(w->renderer, note, clip.x, y + 12, inner, 1, ui.muted);
    SDL_RenderSetClipRect(w->renderer, NULL);
    dismiss_icon(w->renderer, (SDL_Rect){width - 58, top + 20, 28, 28}, ui.muted);
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
    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);
    }
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
    return w->has_picture && !w->help_visible && !w->setup.visible && !w->diagnostics_visible &&
           !w->pairing_visible;
}

static int side_panels_w(const ph_window *w)
{
    if (!mirror_chrome(w))
        return 0;
    return (w->rail_open ? RAIL_W : 0) + (w->settings_visible ? w->settings_width
                                          : w->transfers_open ? TRANSFERS_W
                                                              : 0);
}

static int picture_column_w(const ph_window *w, int win_w)
{
    int panels = side_panels_w(w);
    int rail = mirror_chrome(w) && w->rail_open ? RAIL_W : 0;
    int pane = panels - rail;
    int available = SDL_max(0, win_w - rail);
    if (!pane)
        return available;
    /* A narrow window shows the panel full width instead of a sliver of video. */
    if (available < 420)
        return 0;
    return SDL_max(160, available - pane);
}

static SDL_Rect rail_toggle_box(int win_w)
{
    return (SDL_Rect){win_w - 120 - 9 - 2 * 32, 4, 30, PH_WINDOW_BAR_PX - 8};
}

static SDL_Rect transfers_toggle_box(int win_w)
{
    return (SDL_Rect){win_w - 120 - 9 - 32, 4, 30, PH_WINDOW_BAR_PX - 8};
}

/* Home, App Switcher, Control Center, Notifications | Screenshot | Volume | Feed. */
static SDL_Rect action_box(const ph_window *w, int win_w, int index)
{
    static const int group_gap[8] = {0, 0, 0, 0, 1, 2, 2, 3};
    int x = picture_column_w(w, win_w);
    return (SDL_Rect){x + 8, PH_WINDOW_BAR_PX + 10 + index * 44 + group_gap[index] * 13, 44, 40};
}

typedef struct transfer_boxes {
    int x, width;
    SDL_Rect files, clip, card, cancel, drop;
} transfer_boxes;

static transfer_boxes transfers_layout(const ph_window *w, int win_w, int win_h)
{
    transfer_boxes t;
    t.x = picture_column_w(w, win_w) + (w->rail_open ? RAIL_W : 0);
    t.width = win_w - t.x;
    int inner = t.width - 32;
    t.files = (SDL_Rect){t.x + 16 + 3, PH_WINDOW_BAR_PX + 50, inner / 2 - 3, 28};
    t.clip = (SDL_Rect){t.x + 16 + inner / 2, PH_WINDOW_BAR_PX + 50, inner / 2 - 3, 28};
    t.card = (SDL_Rect){t.x + 16, PH_WINDOW_BAR_PX + 96, inner, 74};
    t.cancel = (SDL_Rect){t.card.x + t.card.w - 66, t.card.y + 10, 56, 24};
    t.drop = (SDL_Rect){t.x + 16, win_h - 16 - 92, inner, 92};
    return t;
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
            SDL_Rect record = {live_settings_box(win_w).x - 32, 3, 28, 26};
            if (w->recording && SDL_PointInRect(&point, &record))
                return CHROME_STOP_RECORDING;
            SDL_Rect rail = rail_toggle_box(win_w);
            SDL_Rect transfers = transfers_toggle_box(win_w);
            SDL_Rect gear = live_settings_box(win_w);
            if (SDL_PointInRect(&point, &gear)) return CHROME_SETTINGS;
            if (SDL_PointInRect(&point, &rail))
                return CHROME_RAIL;
            if (!w->settings_visible && SDL_PointInRect(&point, &transfers)) return CHROME_TRANSFERS;
        } else if (!w->workspace) {
            toolbar_buttons(win_w, buttons);
            for (int i = 0; i < 3; i++)
                if (SDL_PointInRect(&point, &buttons[i])) return CHROME_SETUP + i;
        }
        return CHROME_DRAG;
    }
    if (w->diagnostics_visible &&
        SDL_PointInRect(&point, &(SDL_Rect){win_w - 58, PH_WINDOW_BAR_PX + 20, 28, 28}))
        return CHROME_DISMISS_STATS;
    if (w->settings_visible && !w->workspace)
        return settings_hit(w, win_w, win_h, point);
    if (mirror_chrome(w)) {
        int column = picture_column_w(w, win_w);
        if (w->rail_open && x >= column && x < column + RAIL_W) {
            for (int i = 0; i < 8; i++) {
                SDL_Rect box = action_box(w, win_w, i);
                if (SDL_PointInRect(&point, &box)) return CHROME_ACT + i;
            }
            return CHROME_PANEL;
        }
        if (!w->settings_visible && w->transfers_open &&
            x >= column + (w->rail_open ? RAIL_W : 0)) {
            transfer_boxes t = transfers_layout(w, win_w, win_h);
            if (SDL_PointInRect(&point, &t.files))
                return CHROME_TAB_FILES;
            if (SDL_PointInRect(&point, &t.clip))
                return CHROME_TAB_CLIP;
            if (!w->transfer_tab && w->transfer_active[0] && SDL_PointInRect(&point, &t.cancel))
                return CHROME_CANCEL;
            return CHROME_PANEL;
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
        if (SDL_PointInRect(&point, &g.fresh)) return address[0] ? CHROME_NEW_CODE : CHROME_NONE;
        if (SDL_PointInRect(&point, &g.gear)) return CHROME_SETTINGS;
    }
    return CHROME_NONE;
}

/* Chrome is drawn in window points and then scaled. Snap each mark onto whole
 * output pixels so a 150% display does not smear a 1px icon. */
static void crisp_rect(SDL_Renderer *renderer, int x, int y, int w, int h, SDL_Rect *out,
                        float *sx, float *sy)
{
    SDL_RenderGetScale(renderer, sx, sy);
    if (*sx < 0.5f)
        *sx = 1;
    if (*sy < 0.5f)
        *sy = 1;
    out->x = (int)lroundf((float)x * *sx);
    out->y = (int)lroundf((float)y * *sy);
    out->w = (int)lroundf((float)(x + w) * *sx) - out->x;
    out->h = (int)lroundf((float)(y + h) * *sy) - out->y;
    if (out->w < 1)
        out->w = 1;
    if (out->h < 1)
        out->h = 1;
}

static void fill_px(SDL_Renderer *renderer, int x, int y, int w, int h)
{
    SDL_Rect rect;
    float sx, sy;

    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    crisp_rect(renderer, x, y, w, h, &rect, &sx, &sy);
    SDL_RenderSetScale(renderer, 1, 1);
    SDL_RenderFillRect(renderer, &rect);
    SDL_RenderSetScale(renderer, sx, sy);
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void cov_put(float *c, int n, int x, int y, float v)
{
    if (x < 0 || y < 0 || x >= n || y >= n || v <= 0)
        return;
    if (v > c[y * n + x])
        c[y * n + x] = v > 1 ? 1 : v;
}

static void cov_capsule(float *c, int n, float x0, float y0, float x1, float y1, float rad)
{
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    int minx = (int)floorf(fminf(x0, x1) - rad - 1);
    int maxx = (int)ceilf(fmaxf(x0, x1) + rad + 1);
    int miny = (int)floorf(fminf(y0, y1) - rad - 1);
    int maxy = (int)ceilf(fmaxf(y0, y1) + rad + 1);
    int x, y;

    for (y = miny; y <= maxy; y++) {
        for (x = minx; x <= maxx; x++) {
            float t = len2 < 0.01f ? 0 : ((x + 0.5f - x0) * dx + (y + 0.5f - y0) * dy) / len2;
            float px, py, dist;
            t = clampf(t, 0, 1);
            px = x0 + t * dx;
            py = y0 + t * dy;
            dist = hypotf(x + 0.5f - px, y + 0.5f - py);
            cov_put(c, n, x, y, rad + 0.5f - dist);
        }
    }
}

static void cov_ring(float *c, int n, float cx, float cy, float r, float thick)
{
    float half = thick * 0.5f;
    int minx = (int)floorf(cx - r - half - 1), maxx = (int)ceilf(cx + r + half + 1);
    int miny = (int)floorf(cy - r - half - 1), maxy = (int)ceilf(cy + r + half + 1);
    int x, y;

    for (y = miny; y <= maxy; y++) {
        for (x = minx; x <= maxx; x++) {
            float dist = hypotf(x + 0.5f - cx, y + 0.5f - cy);
            float cover = fminf(r + half + 0.5f - dist, dist - (r - half - 0.5f));
            cov_put(c, n, x, y, cover);
        }
    }
}

static void cov_arc(float *c, int n, float cx, float cy, float r, float thick, float a0, float a1)
{
    float half = thick * 0.5f;
    int minx = (int)floorf(cx - r - half - 1), maxx = (int)ceilf(cx + r + half + 1);
    int miny = (int)floorf(cy - r - half - 1), maxy = (int)ceilf(cy + r + half + 1);
    int x, y;

    for (y = miny; y <= maxy; y++) {
        for (x = minx; x <= maxx; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, ang = atan2f(dy, dx);
            float dist, cover;
            if (ang < a0)
                ang += 6.2831853f;
            if (ang < a0 || ang > a1)
                continue;
            dist = hypotf(dx, dy);
            cover = fminf(r + half + 0.5f - dist, dist - (r - half - 0.5f));
            cov_put(c, n, x, y, cover);
        }
    }
}

static void cov_rrect(float *c, int n, float x, float y, float w, float h, float rad, float thick)
{
    float r = rad < thick ? thick : rad;
    cov_capsule(c, n, x + r, y + thick * 0.5f, x + w - r, y + thick * 0.5f, thick * 0.5f);
    cov_capsule(c, n, x + r, y + h - thick * 0.5f, x + w - r, y + h - thick * 0.5f, thick * 0.5f);
    cov_capsule(c, n, x + thick * 0.5f, y + r, x + thick * 0.5f, y + h - r, thick * 0.5f);
    cov_capsule(c, n, x + w - thick * 0.5f, y + r, x + w - thick * 0.5f, y + h - r, thick * 0.5f);
    cov_ring(c, n, x + r, y + r, r - thick * 0.5f, thick);
    cov_ring(c, n, x + w - r, y + r, r - thick * 0.5f, thick);
    cov_ring(c, n, x + r, y + h - r, r - thick * 0.5f, thick);
    cov_ring(c, n, x + w - r, y + h - r, r - thick * 0.5f, thick);
}

/* Smooth icon in a 32-unit square, painted at the destination's real pixel size. */
static void paint_icon(SDL_Renderer *renderer, SDL_Rect dest, SDL_Color color,
                        void (*build)(float *cov, int n, int kind), int kind)
{
    float sx = 1, sy = 1;
    int n, i;
    float *cov;
    SDL_Surface *surface;
    SDL_Texture *tex;

    if (dest.w < 4 || dest.h < 4)
        return;
    SDL_Rect pixels;
    crisp_rect(renderer, dest.x, dest.y, dest.w, dest.h, &pixels, &sx, &sy);
    n = SDL_clamp(SDL_min(pixels.w, pixels.h), 1, 96);
    cov = calloc((size_t)n * (size_t)n, sizeof *cov);
    surface = SDL_CreateRGBSurfaceWithFormat(0, n, n, 32, SDL_PIXELFORMAT_RGBA32);
    if (!cov || !surface) {
        free(cov);
        if (surface)
            SDL_FreeSurface(surface);
        return;
    }
    build(cov, n, kind);
    for (i = 0; i < n * n; i++) {
        unsigned char *p = (unsigned char *)surface->pixels + i * 4;
        p[0] = p[1] = p[2] = 255;
        p[3] = (unsigned char)(clampf(cov[i], 0, 1) * 255);
    }
    tex = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    free(cov);
    if (!tex)
        return;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(tex, color.r, color.g, color.b);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
    SDL_FRect output = ph_ui_pixel_rect(pixels.x, pixels.y, pixels.w, pixels.h, sx, sy);
    SDL_RenderCopyF(renderer, tex, NULL, &output);
    SDL_DestroyTexture(tex);
}

static float iu(int n, float u)
{
    return u * (float)n / 32.f;
}

static void icon_window(float *c, int n, int kind)
{
    if (kind == CHROME_MIN) {
        cov_capsule(c, n, iu(n, 8), iu(n, 16), iu(n, 24), iu(n, 16), iu(n, 1.35f));
        return;
    }
    if (kind == CHROME_MAX) {
        cov_rrect(c, n, iu(n, 8), iu(n, 8), iu(n, 16), iu(n, 16), iu(n, 2.2f), iu(n, 1.45f));
        return;
    }
    cov_capsule(c, n, iu(n, 9), iu(n, 9), iu(n, 23), iu(n, 23), iu(n, 1.35f));
    cov_capsule(c, n, iu(n, 23), iu(n, 9), iu(n, 9), iu(n, 23), iu(n, 1.35f));
}

static void icon_ready(float *c, int n, int kind)
{
    if (kind == 0) {
        cov_ring(c, n, iu(n, 16), iu(n, 22), iu(n, 1.5f), iu(n, 3));
        cov_arc(c, n, iu(n, 16), iu(n, 22), iu(n, 6), iu(n, 1.6f), -2.7f, -0.45f);
        cov_arc(c, n, iu(n, 16), iu(n, 22), iu(n, 10), iu(n, 1.6f), -2.7f, -0.45f);
        cov_arc(c, n, iu(n, 16), iu(n, 22), iu(n, 14), iu(n, 1.6f), -2.5f, -0.65f);
        return;
    }
    if (kind == 1) {
        cov_rrect(c, n, iu(n, 10), iu(n, 5), iu(n, 12), iu(n, 20), iu(n, 2), iu(n, 1.5f));
        cov_capsule(c, n, iu(n, 14), iu(n, 21), iu(n, 18), iu(n, 21), iu(n, 0.8f));
        return;
    }
    if (kind == 2) {
        cov_rrect(c, n, iu(n, 11), iu(n, 5), iu(n, 10), iu(n, 20), iu(n, 4), iu(n, 1.5f));
        cov_capsule(c, n, iu(n, 16), iu(n, 8), iu(n, 16), iu(n, 14), iu(n, 0.75f));
        return;
    }
    cov_capsule(c, n, iu(n, 8), iu(n, 13), iu(n, 8), iu(n, 19), iu(n, 1.6f));
    cov_capsule(c, n, iu(n, 8), iu(n, 13), iu(n, 14), iu(n, 10), iu(n, 1.15f));
    cov_capsule(c, n, iu(n, 8), iu(n, 19), iu(n, 14), iu(n, 22), iu(n, 1.15f));
    cov_capsule(c, n, iu(n, 14), iu(n, 10), iu(n, 14), iu(n, 22), iu(n, 1.15f));
    cov_arc(c, n, iu(n, 14), iu(n, 16), iu(n, 5), iu(n, 1.4f), -1.1f, 1.1f);
    cov_arc(c, n, iu(n, 14), iu(n, 16), iu(n, 8), iu(n, 1.4f), -0.9f, 0.9f);
}

static void draw_chrome_glyph(SDL_Renderer *renderer, int kind, SDL_Rect box, int thick, bool on_ink)
{
    SDL_Rect glyph = {box.x + box.w / 2 - 7, box.y + box.h / 2 - 7, 14, 14};
    (void)thick;
    paint_icon(renderer, glyph, on_ink ? color_on_ink : color_text, icon_window, kind);
}


static void stroke(SDL_Renderer *renderer, int x1, int y1, int x2, int y2);

/* 0 Settings (gear), 1 Controls (sliders), 2 Transfers (up and down arrows). */
static void bar_toggle(ph_window *w, SDL_Rect box, int kind, bool on, bool hot)
{
    SDL_Renderer *r = w->renderer;
    if (on || hot)
        rounded(r, box, 6, on || hot ? color_stage_raised : color_stage);
    {
        SDL_Rect glyph = {box.x + (box.w - 16) / 2, box.y + (box.h - 16) / 2, 16, 16};
        ph_icon_draw(r,
                     kind == 0   ? PH_ICON_GEAR
                     : kind == 1 ? PH_ICON_SLIDERS
                                 : PH_ICON_TRANSFER,
                     glyph, color_on_ink);
    }
}

static void draw_bar(ph_window *w, int width, int height)
{
    bool live = mirror_chrome(w);
    (void)height;
    bool dark = live || ph_workspace_dark(w->workspace);
    paint(w->renderer, (SDL_Rect){0, 0, width, PH_WINDOW_BAR_PX}, dark ? 15 : 255, dark ? 17 : 255,
          dark ? 20 : 255, 255);
    SDL_Rect mark = {12, (PH_WINDOW_BAR_PX - 18) / 2, 18, 18};
    SDL_Texture *logo = dark ? w->mark_on_dark : w->mark;
    if (logo)
        SDL_RenderCopy(w->renderer, logo, NULL, &mark);
    if (!live)
        draw_text(w->renderer, "Porthole", 38, 7, 120, PH_TEXT(14, PH_FONT_MEDIUM),
                  dark ? color_on_ink : color_text);
    if (live) {
        SDL_Rect rail = rail_toggle_box(width);
        SDL_Rect transfers = transfers_toggle_box(width);
        SDL_Rect gear = live_settings_box(width);
        const char *who = w->phone_label[0] ? w->phone_label : "Connected";
        int name_x = 52;
        int name_y = (PH_WINDOW_BAR_PX - ph_text_line_height(PH_TEXT(13, PH_FONT_MEDIUM))) / 2;
        status_dot(w->renderer, 38, (PH_WINDOW_BAR_PX - 8) / 2, 61, 214, 140);
        draw_text(w->renderer, who, name_x, name_y, 160, PH_TEXT(13, PH_FONT_MEDIUM), color_on_ink);
        if (w->recording) {
            SDL_Rect stop = {gear.x - 32, 3, 28, 26};
            rounded(w->renderer, stop, 6, color_stage_raised);
            rounded(w->renderer, (SDL_Rect){stop.x + 9, stop.y + 8, 10, 10}, 2,
                    (SDL_Color){251, 99, 91, 255});
            if (stop.x > name_x + ph_text_width(who, PH_TEXT(13, PH_FONT_MEDIUM)) + 84)
                draw_text(w->renderer, "Recording", stop.x - 66, 8, 66, PH_TEXT(11, PH_FONT_MEDIUM),
                          color_on_ink);
        }
        bar_toggle(w, rail, 1, w->rail_open, w->chrome_hover == CHROME_RAIL);
        bar_toggle(w, transfers, 2, w->transfers_open, w->chrome_hover == CHROME_TRANSFERS);
        bar_toggle(w, gear, 0, w->settings_visible, w->chrome_hover == CHROME_SETTINGS);
        if (w->transfers_unseen && !w->transfers_open) {
            rounded(w->renderer, (SDL_Rect){transfers.x + transfers.w - 10, transfers.y + 1, 10, 10}, 5,
                    color_stage);
            rounded(w->renderer, (SDL_Rect){transfers.x + transfers.w - 8, transfers.y + 3, 6, 6}, 3,
                    color_live);
        }
        paint(w->renderer, (SDL_Rect){width - 120 - 5, 9, 1, PH_WINDOW_BAR_PX - 18}, 42, 45, 49, 255);
        if ((w->rail_open || w->transfers_open) && w->metrics.fps_known && w->metrics.fps > 0) {
            char fps[24];
            snprintf(fps, sizeof fps, "%.0f fps", w->metrics.fps);
            draw_text(w->renderer, fps, gear.x - 10 - ph_text_width(fps, 1), 6, 80, 1,
                      color_stage_muted);
        }
    } else if (!w->workspace) {
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
                          (hot && kind == CHROME_CLOSE) ||
                              (dark && !(hot && kind == CHROME_CLOSE)));
    }
    if (!live)
        paint(w->renderer, (SDL_Rect){0, PH_WINDOW_BAR_PX - 1, width, 1}, dark ? 55 : 229,
              dark ? 63 : 229, dark ? 73 : 229, 255);
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
    SDL_Event event;
    SDL_zero(event);
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
    int *expansion = flag == &w->settings_visible ? &w->settings_expansion
                     : flag == &w->rail_open      ? &w->rail_expansion
                                                  : &w->transfers_expansion;
    Uint32 flags = SDL_GetWindowFlags(w->window);
    int ww, wh;

    *flag = !*flag;
    int delta = *flag ? width_px : -*expansion;
    if ((flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) == 0) {
        SDL_GetWindowSize(w->window, &ww, &wh);
        int width = SDL_max(320, ww + delta);
        SDL_Rect usable;
        if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(w->window), &usable) == 0) {
            int x, y;
            SDL_GetWindowPosition(w->window, &x, &y);
            width = SDL_max(320, SDL_min(width, usable.w));
            x = SDL_clamp(x, usable.x, usable.x + SDL_max(0, usable.w - width));
            SDL_SetWindowPosition(w->window, x, y);
        }
        SDL_SetWindowSize(w->window, width, wh);
        if (*flag)
            *expansion = width - ww;
    } else if (*flag) {
        *expansion = 0;
    }
    ph_window_redraw(w);
}

ph_window *ph_window_create(const char *title)
{
    ph_window *w = calloc(1, sizeof *w);
    if (w)
        w->code_open = true;

    if (!w)
        return NULL;
    w->ctrl_command = true;
    w->release_blur = true;
    w->media_keys = true;
    w->sound_on = true;
    w->transfer_percent = -1;
#ifdef _WIN32
    /* Without this, a borderless window gets no thick frame and Windows will not resize it. */
    SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");
#endif
    w->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WAITING_W,
                                 WAITING_H,
                                 SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_BORDERLESS |
                                     SDL_WINDOW_HIDDEN);
    snprintf(w->title, sizeof w->title, "%s", title ? title : "Porthole");
    if (!w->window)
        goto fail;
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
    w->workspace = ph_workspace_create();
#ifdef _WIN32
    w->accessibility = ph_accessibility_attach(w->window, accessible_action, w);
#endif
    load_save_place(w);
    if (!ph_text_attach(w->renderer))
        goto fail;
    w->lockup = texture_from_rgba(w->renderer, ph_brand_lockup, ph_brand_lockup_w, ph_brand_lockup_h);
    w->mark = texture_from_rgba(w->renderer, ph_brand_icon, ph_brand_icon_w, ph_brand_icon_h);
    {
        /* Inverting keeps the shape and its grey reflection: ink becomes white. */
        size_t n = (size_t)ph_brand_icon_w * (size_t)ph_brand_icon_h * 4;
        unsigned char *light = malloc(n);
        if (light) {
            for (size_t i = 0; i < n; i += 4) {
                light[i] = (unsigned char)(255 - ph_brand_icon[i]);
                light[i + 1] = (unsigned char)(255 - ph_brand_icon[i + 1]);
                light[i + 2] = (unsigned char)(255 - ph_brand_icon[i + 2]);
                light[i + 3] = ph_brand_icon[i + 3];
            }
            w->mark_on_dark = texture_from_rgba(w->renderer, light, ph_brand_icon_w, ph_brand_icon_h);
            free(light);
        }
    }
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
    place_waiting(w);
    if (SDL_SetWindowHitTest(w->window, hit_edges, w) != 0)
        PH_LOGW("window edges cannot resize: %s", SDL_GetError());
    ph_window_redraw(w);
    return w;

fail:
    PH_LOGE("couldn't create the window: %s", SDL_GetError());
    ph_window_destroy(w);
    return NULL;
}

void ph_window_show(ph_window *w)
{
    if (!w || !w->window)
        return;
    ph_window_redraw(w);
    SDL_ShowWindow(w->window);
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

void ph_window_set_input_policy(ph_window *w, bool bluetooth_ready, ph_control_policy policy)
{
    if (w->bluetooth_ready == bluetooth_ready && w->input_policy.managed == policy.managed &&
        w->input_policy.enabled == policy.enabled && w->input_policy.screen_only == policy.screen_only)
        return;
    w->bluetooth_ready = bluetooth_ready;
    w->input_policy = policy;
    ph_window_redraw(w);
}

void ph_window_set_detail(ph_window *w, const char *detail)
{
    if (!w)
        return;
    snprintf(w->notice, sizeof w->notice, "%s", detail ? detail : "");
    w->notice_until = SDL_GetTicks64() + 4500;
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

bool ph_window_take_new_code(ph_window *w)
{
    if (!w || !w->new_code)
        return false;
    w->new_code = false;
    return true;
}

ph_window_action ph_window_take_action(ph_window *w)
{
    ph_window_action action;

    if (!w)
        return PH_WINDOW_ACTION_NONE;
    action = w->pending_action;
    w->pending_action = PH_WINDOW_ACTION_NONE;
    return action;
}

bool ph_window_ctrl_is_command(const ph_window *w)
{
    return !w || w->ctrl_command;
}

bool ph_window_release_on_blur(const ph_window *w)
{
    return !w || w->release_blur;
}

bool ph_window_next_relative(const ph_window *w)
{
    return w && w->next_relative;
}

void ph_window_set_prefs(ph_window *w, bool ctrl_command, bool release_blur, bool next_relative)
{
    if (!w)
        return;
    w->ctrl_command = ctrl_command;
    w->release_blur = release_blur;
    w->next_relative = next_relative;
}

void ph_window_note_transfer(ph_window *w, const char *line)
{
    if (!w || !line || !line[0])
        return;
    if (w->transfer_count == 6) {
        memmove(w->transfer_lines, w->transfer_lines + 1, 5 * sizeof w->transfer_lines[0]);
        w->transfer_count = 5;
    }
    snprintf(w->transfer_lines[w->transfer_count], sizeof w->transfer_lines[0], "%s", line);
    w->transfer_count++;
    w->transfer_active[0] = 0;
    if (!w->transfers_open)
        w->transfers_unseen = true;
    ph_window_redraw(w);
}

void ph_window_set_live_state(ph_window *w, bool feed, bool paused, bool sound_on)
{
    if (!w || (w->feed_on == feed && w->paused == paused && w->sound_on == sound_on))
        return;
    w->feed_on = feed;
    w->paused = paused;
    w->sound_on = sound_on;
    if (w->settings_visible)
        ph_window_redraw(w);
}

bool ph_window_media_keys(const ph_window *w)
{
    return !w || w->media_keys;
}

void ph_window_set_media_keys(ph_window *w, bool on)
{
    if (w)
        w->media_keys = on;
}

bool ph_window_settings_target(ph_window *w, int row, int side, int *x, int *y)
{
    int width = 1, height = 1, top;

    if (!w || !x || !y || row < -1 || row >= SET_ROWS)
        return false;
    load_save_place(w);
    SDL_GetWindowSize(w->window, &width, &height);
    if (row == -1) {
        SDL_Rect done = settings_done_box(width);
        *x = done.x + done.w / 2;
        *y = done.y + done.h / 2;
        return true;
    }
    top = settings_top() - w->modal_scroll;
    for (int i = 0; i <= row; i++) {
        setting_box b = setting_layout(w, width, i, &top);
        if (i == row) {
            SDL_Rect c = setting_rows[i].kind == SET_CHOICE ? (side ? b.right : b.left) : b.control;
            *x = c.x + c.w / 2;
            *y = c.y + c.h / 2;
            return *y < height;
        }
    }
    return false;
}

void ph_window_set_phone_name(ph_window *w, const char *name)
{
    if (!w)
        return;
    if (!name)
        name = "";
    if (!strcmp(w->phone_label, name))
        return;
    snprintf(w->phone_label, sizeof w->phone_label, "%s", name);
    ph_window_redraw(w);
}

void ph_window_set_code_open(ph_window *w, bool open)
{
    if (!w || w->code_open == open)
        return;
    w->code_open = open;
    if (!open)
        w->pairing_visible = false;
    ph_window_redraw(w);
}

void ph_window_set_pc_name(ph_window *w, const char *name)
{
    if (!w || !name)
        return;
    snprintf(w->pc_name, sizeof w->pc_name, "%s", name);
    ph_window_redraw(w);
}

bool ph_window_connect_target(ph_window *w, ph_window_target target, int *x, int *y)
{
    int width = 1, height = 1;
    connect_boxes g;
    SDL_Rect box;

    if (!w || !x || !y || w->has_picture)
        return false;
    SDL_GetWindowSize(w->window, &width, &height);
    g = connect_layout(width, height, PH_WINDOW_BAR_PX);
    switch (target) {
    case PH_TARGET_COPY: box = g.copy; break;
    case PH_TARGET_NEW_CODE: box = g.fresh; break;
    case PH_TARGET_SETUP: box = g.setup; break;
    case PH_TARGET_HELP: box = g.help; break;
    case PH_TARGET_STATS: box = g.stats; break;
    default: box = g.gear; break;
    }
    *x = box.x + box.w / 2;
    *y = box.y + box.h / 2;
    return true;
}

void ph_window_copy_pairing(ph_window *w)
{
    if (w && w->code_open && w->pull_hint[0] && SDL_SetClipboardText(w->pull_hint) == 0) {
        w->copied_until = SDL_GetTicks64() + 1800;
        ph_window_redraw(w);
    }
}

bool ph_window_has_pairing(const ph_window *w)
{
    return w && w->code_open && w->pull_hint[0];
}

static void set_settings(ph_window *w, bool open)
{
    if (w->settings_visible == open)
        return;
    if (mirror_chrome(w)) {
        int width, height;
        SDL_Rect bounds;
        SDL_GetWindowSize(w->window, &width, &height);
        int previous = w->transfers_open ? TRANSFERS_W : 0;
        if (open) {
            int display = SDL_GetWindowDisplayIndex(w->window);
            int available = SDL_GetDisplayUsableBounds(display, &bounds) == 0 ? bounds.w : 1280;
            w->settings_width = SDL_max(380, SDL_min(760, available - width + previous));
        }
        flip_panel(w, &w->settings_visible, w->settings_width - previous);
    } else {
        w->settings_visible = open;
    }
    ph_window_redraw(w);
}

void ph_window_open_settings(ph_window *w)
{
    if (!w || w->settings_visible)
        return;
    set_settings(w, true);
}

void ph_window_set_transfer_percent(ph_window *w, int percent)
{
    if (!w || w->transfer_percent == percent)
        return;
    w->transfer_percent = percent;
    if (w->transfers_open || !w->has_picture)
        ph_window_redraw(w);
}

bool ph_window_set_active_transfer(ph_window *w, const char *line)
{
    if (!w)
        return false;
    if (!line)
        line = "";
    if (!strcmp(w->transfer_active, line))
        return false;
    snprintf(w->transfer_active, sizeof w->transfer_active, "%s", line);
    ph_window_redraw(w);
    return true;
}

void ph_window_show_transfers(ph_window *w)
{
    if (!w || !w->has_picture || w->settings_visible || w->transfers_open)
        return;
    w->transfers_unseen = false;
    flip_panel(w, &w->transfers_open, TRANSFERS_W);
}

bool ph_window_take_cancel(ph_window *w)
{
    if (!w || !w->transfer_cancel)
        return false;
    w->transfer_cancel = false;
    return true;
}

void ph_window_set_share_available(ph_window *w, bool available)
{
    if (!w) return;
    w->share_checked = true;
    w->share_available = available;
    if (!w->has_picture) ph_window_redraw(w);
}

void ph_window_set_clipboard_available(ph_window *w, bool available)
{
    if (!w || w->clipboard_available == available)
        return;
    w->clipboard_available = available;
    ph_window_redraw(w);
}

static void save_unread(ph_window *w)
{
    if (!w->unread_key[0])
        return;
    char value[16];
    snprintf(value, sizeof value, "%u", w->unread_files);
    if (!ph_config_write_line(w->unread_key, value))
        PH_LOGW("Could not save the unread file count");
}

void ph_window_set_history_profile(ph_window *w, const char *profile)
{
    if (!w)
        return;
    char key[sizeof w->unread_key] = "";
    if (profile && strlen(profile) == 64 && strspn(profile, "0123456789abcdef") == 64)
        snprintf(key, sizeof key, "unread-%s", profile);
    if (!strcmp(key, w->unread_key))
        return;
    snprintf(w->unread_key, sizeof w->unread_key, "%s", key);
    char value[24];
    unsigned count;
    w->unread_files = key[0] && ph_config_read_line(key, value, sizeof value) &&
                              sscanf(value, "%u", &count) == 1
                          ? SDL_min(count, 999u)
                          : 0;
    w->transfers_unseen = w->unread_files > 0;
    ph_window_redraw(w);
}

void ph_window_note_received(ph_window *w, unsigned files)
{
    if (!w || !files || w->page == PH_WORKSPACE_HISTORY)
        return;
    w->unread_files = SDL_min(w->unread_files + SDL_min(files, 999u), 999u);
    w->transfers_unseen = true;
    save_unread(w);
    ph_window_redraw(w);
}

static void mark_received_read(ph_window *w)
{
    if (!w->unread_files)
        return;
    w->unread_files = 0;
    w->transfers_unseen = false;
    save_unread(w);
}

void ph_window_open_received_history(ph_window *w)
{
    if (!w || !w->workspace)
        return;
    ph_workspace_open_history(w->workspace, true);
    w->diagnostics_visible = false;
    w->help_visible = false;
    w->page = PH_WORKSPACE_HISTORY;
    mark_received_read(w);
    if (mirror_chrome(w))
        set_settings(w, true);
    ph_window_redraw(w);
}

void ph_window_open_pairing(ph_window *w)
{
    if (!w || !w->workspace)
        return;
    w->diagnostics_visible = false;
    w->help_visible = false;
    w->page = PH_WORKSPACE_HOME;
    set_settings(w, false);
    w->pairing_visible = w->has_picture;
    ph_window_redraw(w);
}

void ph_window_toggle_help(ph_window *w)
{
    if (w->workspace) {
        if (w->page == PH_WORKSPACE_HOME && ph_workspace_current_section(w->workspace) == 5 &&
            w->settings_visible)
            set_settings(w, false);
        else {
            w->page = PH_WORKSPACE_HOME;
            ph_workspace_open_section(w->workspace, 5);
            set_settings(w, true);
            ph_window_redraw(w);
        }
        return;
    }
    w->help_visible = !w->help_visible;
    w->modal_scroll = 0;
    ph_window_redraw(w);
}

void ph_window_toggle_statistics(ph_window *w)
{
    if (!w->workspace || w->diagnostics_visible) {
        ph_window_toggle_diagnostics(w);
        return;
    }
    if (w->settings_visible && w->page == PH_WORKSPACE_HOME &&
        ph_workspace_current_section(w->workspace) == 6) {
        set_settings(w, false);
        return;
    }
    w->page = PH_WORKSPACE_HOME;
    ph_workspace_open_section(w->workspace, 6);
    set_settings(w, true);
    ph_window_redraw(w);
}

bool ph_window_help_visible(const ph_window *w)
{
    return w &&
           (w->help_visible || w->settings_visible || w->diagnostics_visible || w->pairing_visible ||
            (w->workspace_drawn && ph_workspace_owns_keyboard(w->workspace)) ||
            w->page != PH_WORKSPACE_HOME);
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
    if (!w->setup.visible && !w->help_visible &&
        (w->diagnostics_visible || (w->settings_visible && w->page == PH_WORKSPACE_HOME &&
                                    ph_workspace_current_section(w->workspace) == 6)))
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

static void place_waiting(ph_window *w)
{
    SDL_Rect usable;
    int client_w = WAITING_W;
    int client_h = WAITING_H;
    int x, y;

    usable_bounds(w->window, &usable);
    if (usable.w < 1)
        usable.w = client_w;
    if (usable.h < 1)
        usable.h = client_h;
    if (client_w > usable.w)
        client_w = usable.w;
    if (client_h > usable.h)
        client_h = usable.h;
    x = usable.x + (usable.w - client_w) / 2;
    y = usable.y + (usable.h - client_h) / 2;
    SDL_SetWindowSize(w->window, client_w, client_h);
    SDL_SetWindowPosition(w->window, x, y);
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
        if (w->settings_visible && !w->settings_width)
            w->settings_width = SDL_min(720, usable.w / 2);
        int panels = SDL_min(side_panels_w(w), SDL_max(0, usable.w - 160));
        ph_chrome_fit_client(pic_w, pic_h, usable.w - panels, limit_h, &insets, &client_w,
                             &client_h);
        client_w += panels;
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

/* After a resize from an edge, the other side follows so the phone keeps its shape:
 * dragging the right edge sets the height, the bottom edge sets the width. */
void ph_window_keep_aspect(ph_window *w)
{
    int ww, wh, panels, content_w, content_h;
    Uint32 flags;
    bool width_moved;

    if (!w || !w->has_picture || w->pairing_visible || !w->texture ||
        w->screen.w < 2 || w->screen.h < 2)
        return;
    flags = SDL_GetWindowFlags(w->window);
    SDL_GetWindowSize(w->window, &ww, &wh);
    if (!ph_workspace_keep_aspect(w->workspace) ||
        (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP))) {
        w->sized_w = ww;
        w->sized_h = wh;
        return;
    }
    panels = side_panels_w(w);
    width_moved = w->sized_w && abs(ww - w->sized_w) > abs(wh - w->sized_h);
    if (width_moved) {
        content_w = ww - panels;
        content_h = (int)lround((double)content_w * w->screen.h / w->screen.w);
        wh = content_h + PH_WINDOW_BAR_PX;
    } else {
        content_h = wh - PH_WINDOW_BAR_PX;
        content_w = (int)lround((double)content_h * w->screen.w / w->screen.h);
        ww = content_w + panels;
    }
    if (ww < 320 || wh < 480)
        return;
    {
        int cw, ch;
        SDL_GetWindowSize(w->window, &cw, &ch);
        if (abs(cw - ww) > 2 || abs(ch - wh) > 2)
            SDL_SetWindowSize(w->window, ww, wh);
    }
    w->sized_w = ww;
    w->sized_h = wh;
}

/* While the phone picture is showing, only the right edge and bottom-right corner
 * resize. The left and bottom edges stay free for Back and Home. The connect screen
 * resizes from every edge. */
static SDL_HitTestResult hit_edges(SDL_Window *window, const SDL_Point *p, void *data)
{
    const ph_window *w = data;
    int ww, wh;
    bool left, right, top, bottom;

    if (!p || (SDL_GetWindowFlags(window) &
               (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)))
        return SDL_HITTEST_NORMAL;
    SDL_GetWindowSize(window, &ww, &wh);
    if (ww < 1 || wh < 1)
        return SDL_HITTEST_NORMAL;
    right = p->x >= ww - RESIZE_EDGE_PX;
    bottom = p->y >= wh - RESIZE_EDGE_PX;
    if (w && mirror_chrome(w)) {
        if (right && bottom)
            return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (right && p->y >= PH_WINDOW_BAR_PX)
            return SDL_HITTEST_RESIZE_RIGHT;
        return SDL_HITTEST_NORMAL;
    }
    left = p->x < RESIZE_EDGE_PX;
    top = p->y < RESIZE_EDGE_PX;
    if (top && left)
        return SDL_HITTEST_RESIZE_TOPLEFT;
    if (top && right)
        return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left)
        return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right)
        return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (top)
        return SDL_HITTEST_RESIZE_TOP;
    if (bottom)
        return SDL_HITTEST_RESIZE_BOTTOM;
    if (left)
        return SDL_HITTEST_RESIZE_LEFT;
    if (right)
        return SDL_HITTEST_RESIZE_RIGHT;
    return SDL_HITTEST_NORMAL;
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
    if (!w->pairing_visible)
        track_size(w, screen->w, screen->h);
    ph_window_redraw(w);
}

void ph_window_tick(ph_window *w)
{
    if (w->notice_until && SDL_GetTicks64() >= w->notice_until) {
        w->notice_until = 0;
        ph_window_redraw(w);
    }
    if (w->dark != ph_workspace_dark(w->workspace)) {
        w->dark = ph_workspace_dark(w->workspace);
        ph_window_redraw(w);
    }
    if (w->copied_until && SDL_GetTicks64() >= w->copied_until) {
        w->copied_until = 0;
        ph_window_redraw(w);
    }
    if (!w->pairing_visible && w->pending_w &&
        SDL_GetTicks64() - w->pending_since >= RESIZE_SETTLE_MS) {
        fit_window_to_picture(w, w->pending_w, w->pending_h);
        ph_window_redraw(w);
    }
}

static void stroke(SDL_Renderer *renderer, int x1, int y1, int x2, int y2)
{
    float sx, sy;
    SDL_RenderGetScale(renderer, &sx, &sy);
    if (sx < 0.5f)
        sx = 1;
    if (sy < 0.5f)
        sy = 1;
    SDL_RenderSetScale(renderer, 1, 1);
    SDL_RenderDrawLine(renderer, (int)lroundf(x1 * sx), (int)lroundf(y1 * sy),
                       (int)lroundf(x2 * sx), (int)lroundf(y2 * sy));
    SDL_RenderSetScale(renderer, sx, sy);
}

static void draw_action_icon(SDL_Renderer *renderer, int index, SDL_Rect box)
{
    SDL_Rect glyph = {box.x + (box.w - 22) / 2, box.y + (box.h - 22) / 2, 22, 22};
    ph_icon_draw(renderer, (ph_control_icon)index, glyph, color_on_ink);
}

/* One line, cut with "..." to fit. */
static void draw_line_fit(SDL_Renderer *renderer, const char *text, int x, int y, int width, int scale,
                          SDL_Color color)
{
    char buf[128];
    size_t n;

    snprintf(buf, sizeof buf, "%s", text);
    n = strlen(buf);
    if (ph_text_width(buf, scale) > width) {
        while (n > 0) {
            buf[--n] = 0;
            char probe[132];
            snprintf(probe, sizeof probe, "%s...", buf);
            if (ph_text_width(probe, scale) <= width) {
                snprintf(buf + n, sizeof buf - n, "...");
                break;
            }
        }
    }
    draw_text(renderer, buf, x, y, width + 8, scale, color);
}

static SDL_Color kind_color(const char *name)
{
    const char *dot = strrchr(name, '.');
    char ext[8] = "";

    if (dot) {
        size_t i;
        for (i = 0; i + 1 < sizeof ext && dot[1 + i]; i++)
            ext[i] = (char)((dot[1 + i] >= 'A' && dot[1 + i] <= 'Z') ? dot[1 + i] + 32 : dot[1 + i]);
        ext[i] = 0;
    }
    if (!strcmp(ext, "pdf")) return (SDL_Color){180, 35, 24, 255};
    if (!strcmp(ext, "zip") || !strcmp(ext, "7z") || !strcmp(ext, "rar"))
        return (SDL_Color){122, 90, 18, 255};
    if (!strcmp(ext, "m4a") || !strcmp(ext, "mp3") || !strcmp(ext, "wav") || !strcmp(ext, "aac"))
        return (SDL_Color){91, 62, 138, 255};
    if (!strcmp(ext, "jpg") || !strcmp(ext, "jpeg") || !strcmp(ext, "png") || !strcmp(ext, "heic")
        || !strcmp(ext, "gif") || !strcmp(ext, "mov") || !strcmp(ext, "mp4") || !strcmp(ext, "webp"))
        return (SDL_Color){47, 95, 138, 255};
    return (SDL_Color){74, 78, 85, 255};
}

static bool image_name(const char *name)
{
    const char *dot = name ? strrchr(name, '.') : NULL;
    char ext[8] = "";
    size_t i;

    if (!dot)
        return false;
    for (i = 0; i + 1 < sizeof ext && dot[1 + i]; i++)
        ext[i] = (char)((dot[1 + i] >= 'A' && dot[1 + i] <= 'Z') ? dot[1 + i] + 32 : dot[1 + i]);
    ext[i] = 0;
    return !strcmp(ext, "jpg") || !strcmp(ext, "jpeg") || !strcmp(ext, "png")
           || !strcmp(ext, "heic") || !strcmp(ext, "gif") || !strcmp(ext, "webp")
           || !strcmp(ext, "mov") || !strcmp(ext, "mp4") || !strcmp(ext, "m4v");
}

static void type_tag(const char *name, char *out, size_t len)
{
    const char *dot = name ? strrchr(name, '.') : NULL;
    size_t i = 0;

    if (!out || len < 2)
        return;
    if (!dot || !dot[1]) {
        snprintf(out, len, "FILE");
        return;
    }
    for (const char *p = dot + 1; *p && i + 1 < len && i < 4; p++, i++)
        out[i] = (char)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
    out[i] = 0;
}

/* A page with its type on it. Pictures are a filled mark; other files name the type. */
static void file_tile(SDL_Renderer *renderer, int x, int y, const char *name)
{
    char tag[8];
    SDL_Color ink = kind_color(name);

    type_tag(name, tag, sizeof tag);
    if (image_name(name)) {
        rounded(renderer, (SDL_Rect){x, y, 26, 32}, 4, ink);
        return;
    }
    rounded(renderer, (SDL_Rect){x, y, 26, 32}, 4, (SDL_Color){213, 216, 221, 255});
    rounded(renderer, (SDL_Rect){x + 1, y + 1, 24, 30}, 3, color_on_ink);
    rounded(renderer, (SDL_Rect){x - 2, y + 18, 22, 12}, 2, ink);
    {
        int tw = ph_text_width(tag, PH_TEXT(8, PH_FONT_SEMIBOLD));
        draw_text(renderer, tag, x - 2 + (22 - tw) / 2, y + 19, 22, PH_TEXT(8, PH_FONT_SEMIBOLD),
                  color_on_ink);
    }
}

typedef struct transfer_row {
    const char *title;
    const char *sub;
    SDL_Color sub_color;
} transfer_row;

static transfer_row read_transfer(const char *line)
{
    transfer_row row = {line, "", color_muted};

    if (!strncmp(line, "Sent ", 5)) { row.title = line + 5; row.sub = "Sent to iPhone"; }
    else if (!strncmp(line, "Cancelled ", 10)) { row.title = line + 10; row.sub = "Cancelled"; }
    else if (!strncmp(line, "Could not send ", 15)) {
        row.title = line + 15;
        row.sub = "Could not send";
        row.sub_color = color_warn;
    } else if (!strncmp(line, "Received ", 9)) {
        static char sub[96];
        char label[64];

        row.title = line + 9;
        if (!ph_place_save_label(label, sizeof label))
            snprintf(label, sizeof label, "Downloads");
        snprintf(sub, sizeof sub, "From iPhone, in %s", label);
        row.sub = sub;
    }
    return row;
}

static void dashed_box(SDL_Renderer *renderer, SDL_Rect box, int radius)
{
    SDL_SetRenderDrawColor(renderer, 196, 200, 206, 255);
    for (int x = box.x + radius; x < box.x + box.w - radius; x += 8) {
        int len = x + 4 > box.x + box.w - radius ? box.x + box.w - radius - x : 4;
        fill_px(renderer, x, box.y, len, 1);
        fill_px(renderer, x, box.y + box.h - 1, len, 1);
    }
    for (int y = box.y + radius; y < box.y + box.h - radius; y += 8) {
        int len = y + 4 > box.y + box.h - radius ? box.y + box.h - radius - y : 4;
        fill_px(renderer, box.x, y, 1, len);
        fill_px(renderer, box.x + box.w - 1, y, 1, len);
    }
}

static const char *const action_names[8] = {
    "Home", "App Switcher", "Control Center", "Notifications", "Screenshot", "Volume up",
    "Volume down", "Feed scrolling"
};
static const char *const action_keys[8] = {
    "Alt+H", "Alt+S", "Alt+C", "Alt+N", "Alt+P", "Alt+Up", "Alt+Down", "F4"
};

static void draw_controls(ph_window *w, int x, int width, int height)
{
    paint(w->renderer, (SDL_Rect){x, PH_WINDOW_BAR_PX, RAIL_W, height - PH_WINDOW_BAR_PX},
          19, 21, 23, 255);
    paint(w->renderer, (SDL_Rect){x, PH_WINDOW_BAR_PX, 1, height - PH_WINDOW_BAR_PX}, 42, 45, 49, 255);
    for (int i = 0; i < 8; i++) {
        SDL_Rect box = action_box(w, width, i);
        bool hot = w->chrome_hover == CHROME_ACT + i;
        if (box.y + box.h > height - 4)
            break;
        if (hot)
            rounded(w->renderer, box, 10, color_stage_raised);
        if (i == 4 || i == 5 || i == 7)
            paint(w->renderer, (SDL_Rect){x + 18, box.y - 8, RAIL_W - 36, 1}, 42, 45, 49, 255);
        draw_action_icon(w->renderer, i, box);
    }
}

/* Hover label over the picture, left of the rail: "Home  Alt+H". */
static void draw_action_tip(ph_window *w, int x, int win_w)
{
    int i = w->chrome_hover - CHROME_ACT;
    SDL_Rect box;
    int name_w, key_w, width;

    if (i < 0 || i >= 8)
        return;
    if (x < 80)
        return;
    box = action_box(w, win_w, i);
    name_w = ph_text_width(action_names[i], 1);
    key_w = ph_text_width(action_keys[i], 1);
    width = name_w + key_w + 34;
    rounded(w->renderer, (SDL_Rect){x - width - 6, box.y + 7, width, 26}, 8, color_stage_raised);
    draw_text(w->renderer, action_names[i], x - width + 6, box.y + 10, name_w + 4, 1, color_on_ink);
    draw_text(w->renderer, action_keys[i], x - key_w - 18, box.y + 10, key_w + 4, 1, color_stage_muted);
}

static void draw_transfers(ph_window *w, int width, int height)
{
    SDL_Renderer *r = w->renderer;
    transfer_boxes t = transfers_layout(w, width, height);
    int inner = t.width - 32;
    int y, list_end;

    bool dark = w->workspace && ph_workspace_dark(w->workspace);
    SDL_Color panel = dark ? (SDL_Color){30, 35, 42, 255} : ((SDL_Color){255, 255, 255, 255});
    rounded(r, (SDL_Rect){t.x, PH_WINDOW_BAR_PX, t.width, height - PH_WINDOW_BAR_PX}, 0, panel);
    draw_text(r, "Transfers", t.x + 16, PH_WINDOW_BAR_PX + 10, inner,
              PH_TEXT(15, PH_FONT_SEMIBOLD), dark ? color_on_ink : color_text);
    SDL_Color inset = dark ? (SDL_Color){21, 25, 30, 255} : color_ground;
    rounded(r, (SDL_Rect){t.files.x - 3, t.files.y - 3, inner, t.files.h + 6}, 7, inset);
    for (int i = 0; i < 2; i++) {
        SDL_Rect tab = i ? t.clip : t.files;
        const char *label = i ? "Clipboard" : "Files";
        bool selected = w->transfer_tab == i;
        if (selected)
            rounded(r, tab, 5, dark ? color_stage_raised : panel);
        int font = PH_TEXT(12, selected ? PH_FONT_MEDIUM : PH_FONT_REGULAR);
        draw_text(r, label, tab.x + (tab.w - ph_text_width(label, font)) / 2, tab.y + 5,
                  tab.w - 8, font, selected ? (dark ? color_on_ink : color_text)
                                             : (dark ? color_stage_muted : color_muted));
    }
    if (w->workspace || w->transfer_tab)
        return;
    y = t.files.y + t.files.h + 16;
    if (w->transfer_active[0]) {
        const char *name = !strncmp(w->transfer_active, "Sending ", 8) ? w->transfer_active + 8
                                                                       : w->transfer_active;
        Uint64 now = SDL_GetTicks64();
        int track = t.card.w - 28, seg = track / 3;
        int pos = (int)((now / 6) % (Uint64)(track + seg)) - seg;
        int a = pos < 0 ? 0 : pos, b = pos + seg > track ? track : pos + seg;
        char status[48];

        /* A known share fills the bar; otherwise a short piece travels along it. */
        if (w->transfer_percent >= 0) {
            a = 0;
            b = track * (w->transfer_percent > 100 ? 100 : w->transfer_percent) / 100;
            snprintf(status, sizeof status, "Sending to iPhone, %d%%", w->transfer_percent);
        } else {
            snprintf(status, sizeof status, "Sending to iPhone");
        }

        surface_card(r, t.card);
        file_tile(r, t.card.x + 14, t.card.y + 12, name);
        draw_line_fit(r, name, t.card.x + 52, t.card.y + 10, t.card.w - 52 - 76, 1, color_text);
        draw_text(r, status, t.card.x + 52, t.card.y + 30, t.card.w - 120, 1, color_muted);
        rounded(r, t.cancel, 6, w->chrome_hover == CHROME_CANCEL ? color_line : color_ground);
        draw_text(r, "Cancel", t.cancel.x + (t.cancel.w - ph_text_width("Cancel", 1)) / 2,
                  t.cancel.y + 3, t.cancel.w, 1, color_text);
        rounded(r, (SDL_Rect){t.card.x + 14, t.card.y + t.card.h - 14, track, 4}, 2, color_line);
        if (b > a)
            rounded(r, (SDL_Rect){t.card.x + 14 + a, t.card.y + t.card.h - 14, b - a, 4}, 2, color_text);
        y = t.card.y + t.card.h + 18;
    }

    list_end = t.drop.y - 12;
    if (t.drop.y < y + 60)
        list_end = height - 12;
    if (!w->transfer_count) {
        if (!w->transfer_active[0])
            draw_text(r, "Nothing sent yet. Share from the iPhone to save files in the folder from "
                         "Settings, or drop files here to send them to the iPhone.",
                      t.x + 16, y, inner, 1, color_muted);
    } else {
        int received[6], sent[6], nr = 0, ns = 0;
        const int *groups[2];
        int counts[2];
        const char *heads[2] = {"RECEIVED", "SENT"};

        for (int i = w->transfer_count - 1; i >= 0; i--) {
            if (!strncmp(w->transfer_lines[i], "Received ", 9))
                received[nr++] = i;
            else
                sent[ns++] = i;
        }
        groups[0] = received;
        groups[1] = sent;
        counts[0] = nr;
        counts[1] = ns;
        for (int g = 0; g < 2; g++) {
            if (!counts[g] || y + 44 > list_end)
                continue;
            draw_text(r, heads[g], t.x + 16, y, inner, 1, color_muted);
            y += 22;
            for (int n = 0; n < counts[g] && y + 44 <= list_end; n++) {
                transfer_row row = read_transfer(w->transfer_lines[groups[g][n]]);
                file_tile(r, t.x + 18, y + 4, row.title);
                draw_line_fit(r, row.title, t.x + 56, y + 2, inner - 44, 1, color_text);
                draw_text(r, row.sub, t.x + 56, y + 21, inner - 44, 1, row.sub_color);
                y += 48;
                if (n + 1 < counts[g])
                    paint(r, (SDL_Rect){t.x + 56, y - 4, inner - 40, 1}, 226, 228, 232, 255);
            }
            y += 8;
        }
    }

    if (t.drop.y >= y + 12 && t.drop.y > t.card.y + 40) {
        int cx = t.drop.x + t.drop.w / 2;
        dashed_box(r, t.drop, 10);
        SDL_SetRenderDrawColor(r, 92, 96, 104, 255);
        stroke(r, cx, t.drop.y + 16, cx, t.drop.y + 30);
        stroke(r, cx - 5, t.drop.y + 21, cx, t.drop.y + 16);
        stroke(r, cx + 5, t.drop.y + 21, cx, t.drop.y + 16);
        {
            const char *head = "Drop files to send to iPhone";
            const char *sub = "Pictures go to Photos, the rest to Files";
            draw_text(r, head, cx - ph_text_width(head, 1) / 2, t.drop.y + 38, inner, 1, color_text);
            draw_text(r, sub, cx - ph_text_width(sub, 1) / 2, t.drop.y + 60, inner, 1, color_muted);
        }
    }
}

static void draw_mirror_panels(ph_window *w, int width, int height)
{
    int column = picture_column_w(w, width);

    if (w->rail_open)
        draw_controls(w, column, width, height);
    if (w->transfers_open && !w->settings_visible)
        draw_transfers(w, width, height);
    if (w->rail_open)
        draw_action_tip(w, column, width);
}

void ph_window_refresh_folder(ph_window *w)
{
    if (w) {
        load_save_place(w);
        ph_window_redraw(w);
    }
}
ph_workspace *ph_window_workspace(ph_window *w)
{
    return w ? w->workspace : NULL;
}

void ph_window_set_companion(ph_window *w, bool connected, bool recording, int volume)
{
    if (!w ||
        (w->companion_connected == connected && w->recording == recording && w->volume == volume))
        return;
    w->companion_connected = connected;
    w->recording = recording;
    w->volume = volume;
    ph_window_redraw(w);
}

void ph_window_set_incoming(ph_window *w, const char *name, uint64_t bytes, uint64_t total)
{
    if (!w)
        return;
    snprintf(w->incoming_file, sizeof w->incoming_file, "%s", name);
    w->incoming_bytes = bytes;
    w->incoming_size = total;
    ph_window_redraw(w);
}

void ph_window_set_receiving(ph_window *w, int done, int total)
{
    if (!w)
        return;
    w->receiving_done = done;
    w->receiving_total = total;
    ph_window_redraw(w);
}

ph_workspace_action ph_window_take_workspace_action(ph_window *w, uint64_t *item)
{
    if (!w)
        return PH_WORKSPACE_NONE;
    ph_workspace_action a = w->workspace_action;
    if (item)
        *item = w->action_item;
    w->workspace_action = PH_WORKSPACE_NONE;
    return a;
}

static void draw_workspace(ph_window *w, SDL_Rect area, ph_workspace_view page)
{
    ph_workspace_live live = {.paired = !w->code_open,
                              .connected = w->companion_connected,
                              .mirroring = w->has_picture,
                              .controls = w->control_ready,
                              .paused = w->paused,
                              .feed = w->feed_on,
                              .toolbar = w->rail_open,
                              .ctrl_command = w->ctrl_command,
                              .media_keys = w->media_keys,
                              .release_blur = w->release_blur,
                              .relative = w->next_relative,
                              .sound = w->sound_on,
                              .recording = w->recording,
                              .share_checked = w->share_checked,
                              .share_available = w->share_available,
                              .clipboard_available = w->clipboard_available,
                              .always_on_top = (SDL_GetWindowFlags(w->window) & SDL_WINDOW_ALWAYS_ON_TOP) != 0,
                              .controls_connected = w->bluetooth_ready,
                              .phone_controls_managed = w->input_policy.managed,
                              .phone_controls_allowed = w->input_policy.enabled,
                              .phone_controls_screen_only = w->input_policy.screen_only,
                              .metrics = w->metrics,
                              .modal = w->settings_visible || w->page != PH_WORKSPACE_HOME ||
                                       (area.x <= RAIL_W && w->has_picture),
                              .unread_files = w->unread_files,
                              .percent = w->transfer_percent,
                              .volume = w->volume,
                              .receiving_done = w->receiving_done,
                              .receiving_total = w->receiving_total};
    char second[40] = "", third[40] = "";
    if (sscanf(w->pull_hint, "%95s %39s %39s", live.address, second, third) >= 2)
        snprintf(live.code, sizeof live.code, "%s", strcmp(second, "code") == 0 ? third : second);
    snprintf(live.phone, sizeof live.phone, "%s", w->phone_label);
    snprintf(live.pc, sizeof live.pc, "%s", w->pc_name);
    snprintf(live.folder, sizeof live.folder, "%s", w->save_label);
    snprintf(live.incoming_file, sizeof live.incoming_file, "%s", w->incoming_file);
    live.incoming_bytes = w->incoming_bytes;
    live.incoming_size = w->incoming_size;
    snprintf(live.active_file, sizeof live.active_file, "%s", w->transfer_active);
    snprintf(live.detail, sizeof live.detail, "%s", w->control_detail);
    ph_workspace_draw(w->workspace, w->renderer, area, page, &live);
    w->workspace_drawn = true;
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
    if (w->has_picture && w->texture && !w->pairing_visible) {
        int pic_out_w = (int)(picture_column_w(w, width) * ((float)out_w / (float)width) + 0.5f);
        if (pic_out_w > 0) {
            SDL_Rect content = {0, top, pic_out_w, out_h - top};
            SDL_Rect dst = fit_rect(pic_out_w, content.h, w->screen.w, w->screen.h);
            paint(w->renderer, content, 11, 12, 13, 255);
            dst.y += top;
            SDL_RenderCopy(w->renderer, w->texture, &w->screen, &dst);
        }
    }
    /* Draw chrome in window points so text, spacing and hit areas agree on
     * high-DPI displays. Video keeps its full physical-pixel output above. */
    SDL_RenderSetScale(w->renderer, (float)out_w / (float)width, (float)out_h / (float)height);
    w->workspace_drawn = false;
    SDL_Rect content = {0, PH_WINDOW_BAR_PX, width, height - PH_WINDOW_BAR_PX};
    if ((!w->has_picture || !w->texture || w->pairing_visible) && w->workspace) {
        if (!w->settings_visible && !w->setup.visible && !w->diagnostics_visible)
            draw_workspace(w, content, w->page);
    } else if (!w->has_picture || !w->texture)
        draw_waiting(w, width, height, PH_WINDOW_BAR_PX);
    if (mirror_chrome(w) && (w->rail_open || w->transfers_open))
        draw_mirror_panels(w, width, height);
    if (w->help_visible)
        draw_help(w, width, height, PH_WINDOW_BAR_PX);
    if (w->diagnostics_visible && !w->help_visible && !w->setup.visible)
        draw_diagnostics(w, width, height, PH_WINDOW_BAR_PX);
    if (w->setup.visible)
        draw_setup(w, width, height, PH_WINDOW_BAR_PX);
    if (w->workspace && mirror_chrome(w) && !w->settings_visible && w->transfers_open) {
        transfer_boxes t = transfers_layout(w, width, height);
        int y = t.clip.y + t.clip.h + 8;
        draw_workspace(w, (SDL_Rect){t.x, y, t.width, height - y},
                       w->transfer_tab ? PH_WORKSPACE_CLIPBOARD : PH_WORKSPACE_FILES);
    }
    if (w->settings_visible && !w->help_visible && !w->setup.visible && !w->diagnostics_visible) {
        if (w->workspace) {
            if (mirror_chrome(w)) {
                content.x = picture_column_w(w, width) + (w->rail_open ? RAIL_W : 0);
                content.w = width - content.x;
            }
            draw_workspace(w, content,
                           w->page == PH_WORKSPACE_HOME ? PH_WORKSPACE_SETTINGS : w->page);
        } else
            draw_settings(w, width, height);
    }
    if (w->notice_until && w->notice[0] && !w->setup.visible) {
        int tw = SDL_min(width - 40, ph_text_width(w->notice, PH_TEXT(12, PH_FONT_MEDIUM)) + 28);
        int th = ph_text_height(w->notice, tw - 24, PH_TEXT(12, PH_FONT_MEDIUM)) + 16;
        SDL_Rect toast = {(width - tw) / 2, height - th - 16, tw, th};
        rounded(w->renderer, toast, 8, color_stage_raised);
        draw_text(w->renderer, w->notice, toast.x + 12, toast.y + 8, tw - 24,
                  PH_TEXT(12, PH_FONT_MEDIUM), color_on_ink);
    }
    draw_bar(w, width, height);
    SDL_RenderSetScale(w->renderer, 1, 1);
#ifdef _WIN32
    ph_accessibility_update(w->accessibility, w->workspace_drawn ? w->workspace : NULL);
#endif
    SDL_RenderPresent(w->renderer);
}

void ph_window_clear(ph_window *w)
{
    if (w->has_picture)
        place_waiting(w);
    w->has_picture = false;
    w->pairing_visible = false;
    w->fit_w = w->fit_h = w->pending_w = w->pending_h = 0;
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

    if (!w->has_picture || ph_window_help_visible(w) || w->setup.visible)
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

static void dispatch_workspace_action(ph_window *w)
{
    uint64_t item = 0;
    ph_workspace_action action = ph_workspace_take_action(w->workspace, &item);
    switch (action) {
    case PH_WORKSPACE_SETTINGS_OPEN:
        w->page = PH_WORKSPACE_HOME;
        set_settings(w, true);
        break;
    case PH_WORKSPACE_HISTORY_OPEN:
    case PH_WORKSPACE_CLIPBOARD_OPEN:
        w->page =
            action == PH_WORKSPACE_HISTORY_OPEN ? PH_WORKSPACE_HISTORY : PH_WORKSPACE_CLIPBOARD;
        if (action == PH_WORKSPACE_HISTORY_OPEN)
            mark_received_read(w);
        if (w->has_picture)
            set_settings(w, true);
        break;
    case PH_WORKSPACE_CLOSE:
        if (w->settings_visible || w->page != PH_WORKSPACE_HOME) {
            w->page = PH_WORKSPACE_HOME;
            set_settings(w, false);
        } else if (w->transfers_open)
            flip_panel(w, &w->transfers_open, TRANSFERS_W);
        break;
    case PH_WORKSPACE_STATS:
        ph_window_toggle_statistics(w);
        break;
    case PH_WORKSPACE_SETUP:
        w->page = PH_WORKSPACE_HOME;
        set_settings(w, false);
        push_shortcut(w, SDL_SCANCODE_F2, KMOD_NONE);
        break;
    case PH_WORKSPACE_NEW_CODE:
    case PH_WORKSPACE_FORGET:
        w->new_code = true;
        break;
    case PH_WORKSPACE_CONTROLS:
        w->pending_action = PH_WINDOW_ACTION_PAUSE;
        break;
    case PH_WORKSPACE_SOUND:
        w->pending_action = PH_WINDOW_ACTION_MUTE;
        break;
    case PH_WORKSPACE_FEED:
        w->pending_action = PH_WINDOW_ACTION_FEED;
        break;
    case PH_WORKSPACE_FOLDER:
        w->pending_action = PH_WINDOW_ACTION_PICK_FOLDER;
        break;
    case PH_WORKSPACE_POINTER:
        w->next_relative = !w->next_relative;
        w->pending_action = PH_WINDOW_ACTION_SAVE;
        break;
    case PH_WORKSPACE_CTRL_COMMAND:
        w->ctrl_command = !w->ctrl_command;
        w->pending_action = PH_WINDOW_ACTION_SAVE;
        break;
    case PH_WORKSPACE_MEDIA:
        w->media_keys = !w->media_keys;
        w->pending_action = PH_WINDOW_ACTION_SAVE;
        break;
    case PH_WORKSPACE_RELEASE:
        w->release_blur = !w->release_blur;
        w->pending_action = PH_WINDOW_ACTION_SAVE;
        break;
    case PH_WORKSPACE_TOOLBAR:
        flip_panel(w, &w->rail_open, RAIL_W);
        break;
    case PH_WORKSPACE_CANCEL_SEND:
        w->transfer_cancel = true;
        break;
    case PH_WORKSPACE_QUIT:
        w->pending_action = PH_WINDOW_ACTION_QUIT;
        break;
    case PH_WORKSPACE_NONE:
        break;
    default:
        w->workspace_action = action;
        w->action_item = item;
        break;
    }
}

#ifdef _WIN32
static void accessible_action(void *context)
{
    ph_window *w = context;
    dispatch_workspace_action(w);
    ph_window_redraw(w);
}
#endif

bool ph_window_chrome_event(ph_window *w, const SDL_Event *event)
{
    int win_w = 1, win_h = 1, x, y, hit;

    if (!w || !event)
        return false;
    if (w->pairing_visible && !w->settings_visible && !w->setup.visible &&
        w->page == PH_WORKSPACE_HOME && event->type == SDL_KEYDOWN &&
        event->key.keysym.scancode == SDL_SCANCODE_ESCAPE &&
        !ph_workspace_owns_keyboard(w->workspace)) {
        w->pairing_visible = false;
        ph_window_redraw(w);
        return true;
    }
    if (w->workspace_drawn && !w->chrome_down && ph_workspace_event(w->workspace, event)) {
        dispatch_workspace_action(w);
        if (event->type != SDL_MOUSEMOTION)
            ph_window_redraw(w);
        return true;
    }
    if ((w->help_visible || w->setup.visible || w->settings_visible) && event->type == SDL_MOUSEWHEEL) {
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
    if (w->diagnostics_visible && !w->setup.visible && !w->help_visible) {
        if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
            SDL_Scancode key = event->key.keysym.scancode;
            if (event->type == SDL_KEYDOWN && !event->key.repeat &&
                (key == SDL_SCANCODE_ESCAPE || key == SDL_SCANCODE_F3))
                ph_window_toggle_diagnostics(w);
            return true;
        }
        if (event->type == SDL_MOUSEWHEEL)
            return true;
        if (event->type == SDL_MOUSEMOTION && !w->chrome_drag &&
            chrome_at(w, win_w, win_h, event->motion.x, event->motion.y) == CHROME_NONE)
            return true;
        if ((event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP) &&
            !w->chrome_down &&
            chrome_at(w, win_w, win_h, event->button.x, event->button.y) == CHROME_NONE)
            return true;
    }
    if (w->settings_visible && !w->workspace)
        load_save_place(w);
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
        else if (hit == CHROME_STOP_RECORDING)
            w->workspace_action = PH_WORKSPACE_RECORD;
        else if (hit == CHROME_DISMISS_STATS)
            ph_window_toggle_diagnostics(w);
        else if (hit == CHROME_MAX)
            toggle_zoom(w->window);
        else if (hit == CHROME_COPY && w->pull_hint[0]) {
            if (SDL_SetClipboardText(w->pull_hint) == 0) w->copied_until = SDL_GetTicks64() + 1800;
            ph_window_redraw(w);
        }
        else if (hit == CHROME_NEW_CODE && w->pull_hint[0]) {
            w->new_code = true;
            ph_window_redraw(w);
        }
        else if (hit == CHROME_SETTINGS) {
            w->page = PH_WORKSPACE_HOME;
            set_settings(w, !w->settings_visible);
            w->modal_scroll = 0;
            ph_window_redraw(w);
        }
        else if (hit == CHROME_CANCEL) {
            w->transfer_cancel = true;
            ph_window_redraw(w);
        }
        else if (hit >= CHROME_SET && hit < CHROME_SET + SET_ROWS * 2) {
            int row = (hit - CHROME_SET) / 2, side = (hit - CHROME_SET) % 2;
            switch (row) {
            case SET_POINTER:
                if (w->next_relative != (side == 1)) {
                    w->next_relative = side == 1;
                    w->pending_action = PH_WINDOW_ACTION_SAVE;
                }
                break;
            case SET_WHEEL:
                if (w->feed_on != (side == 1)) {
                    w->feed_on = side == 1;
                    w->pending_action = PH_WINDOW_ACTION_FEED;
                }
                break;
            case SET_PAUSE:
                w->paused = !w->paused;
                w->pending_action = PH_WINDOW_ACTION_PAUSE;
                break;
            case SET_GUIDE:
                w->settings_visible = false;
                push_shortcut(w, SDL_SCANCODE_F2, KMOD_NONE);
                break;
            case SET_CTRL:
                w->ctrl_command = !w->ctrl_command;
                w->pending_action = PH_WINDOW_ACTION_SAVE;
                break;
            case SET_MEDIA:
                w->media_keys = !w->media_keys;
                w->pending_action = PH_WINDOW_ACTION_SAVE;
                break;
            case SET_BLUR:
                w->release_blur = !w->release_blur;
                w->pending_action = PH_WINDOW_ACTION_SAVE;
                break;
            case SET_SOUND:
                w->sound_on = !w->sound_on;
                w->pending_action = PH_WINDOW_ACTION_MUTE;
                break;
            case SET_SAVE:
                if (side == 1)
                    w->pending_action = PH_WINDOW_ACTION_PICK_FOLDER;
                else if (w->save_custom)
                    w->pending_action = PH_WINDOW_ACTION_SAVE_DOWNLOADS;
                break;
            case SET_COPY:
                if (w->pull_hint[0] && SDL_SetClipboardText(w->pull_hint) == 0)
                    w->copied_until = SDL_GetTicks64() + 1800;
                break;
            case SET_FORGET:
                w->pending_action = PH_WINDOW_ACTION_FORGET;
                break;
            case SET_QUIT:
                w->pending_action = PH_WINDOW_ACTION_QUIT;
                break;
            }
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
        else if (hit == CHROME_TRANSFERS) {
            w->transfers_unseen = false;
            flip_panel(w, &w->transfers_open, TRANSFERS_W);
        }
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
            if (w->workspace) {
                w->workspace_action = PH_WORKSPACE_HIDE;
                return true;
            }
            SDL_Event quit;
            SDL_zero(quit);
            quit.type = SDL_QUIT;
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
#ifdef _WIN32
    ph_accessibility_detach(w->accessibility);
#endif
    ph_workspace_destroy(w->workspace);
    if (w->texture)
        SDL_DestroyTexture(w->texture);
    if (w->lockup)
        SDL_DestroyTexture(w->lockup);
    if (w->mark)
        SDL_DestroyTexture(w->mark);
    if (w->mark_on_dark)
        SDL_DestroyTexture(w->mark_on_dark);
    if (w->renderer) {
        ph_ui_release_drawing(w->renderer);
        ph_icon_release(w->renderer);
        ph_text_detach(w->renderer);
    }
    if (w->renderer)
        SDL_DestroyRenderer(w->renderer);
    if (w->window)
        SDL_DestroyWindow(w->window);
    free(w);
}
