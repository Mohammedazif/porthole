/* Exercise the real software-rendered UI, including its modal/phone boundary.
 * Optional --screenshots DIR writes actual app renders for visual review. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>

#include "control/setup.h"
#include "core/version.h"
#include "ui/text.h"
#include "ui/vendor/qrcodegen.h"
#include "ui/window.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s (%s)\n", __FILE__, __LINE__, #condition, SDL_GetError()); \
    exit(1); } } while (0)

static void screenshot(ph_window *window, const char *dir, const char *name)
{
    if (!dir) return;
    SDL_Renderer *renderer = SDL_GetRenderer(SDL_GetWindowFromID(ph_window_id(window)));
    int width, height;
    CHECK(SDL_GetRendererOutputSize(renderer, &width, &height) == 0);
    AVFrame *frame = av_frame_alloc();
    CHECK(frame);
    frame->width = width; frame->height = height; frame->format = AV_PIX_FMT_RGBA;
    CHECK(av_frame_get_buffer(frame, 32) == 0);
    CHECK(SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGBA32,
                                frame->data[0], frame->linesize[0]) == 0);
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    CHECK(codec);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    CHECK(ctx);
    ctx->width = width; ctx->height = height; ctx->pix_fmt = AV_PIX_FMT_RGBA;
    ctx->time_base = (AVRational){1, 1};
    CHECK(avcodec_open2(ctx, codec, NULL) == 0);
    AVPacket *packet = av_packet_alloc();
    CHECK(packet && avcodec_send_frame(ctx, frame) == 0);
    CHECK(avcodec_receive_packet(ctx, packet) == 0);
    char path[1024];
    CHECK(snprintf(path, sizeof path, "%s/%s.png", dir, name) < (int)sizeof path);
    FILE *file = fopen(path, "wb");
    CHECK(file);
    CHECK(fwrite(packet->data, 1, (size_t)packet->size, file) == (size_t)packet->size);
    CHECK(fclose(file) == 0);
    av_packet_free(&packet); avcodec_free_context(&ctx); av_frame_free(&frame);
}

static void click_shortcut(ph_window *w, int x, int y, SDL_Scancode expected)
{
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    SDL_Event event = {.type = SDL_MOUSEBUTTONDOWN};
    event.button.windowID = ph_window_id(w);
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = x; event.button.y = y;
    CHECK(ph_window_chrome_event(w, &event));
    event.type = SDL_MOUSEBUTTONUP;
    CHECK(ph_window_chrome_event(w, &event));
    SDL_Event queued;
    unsigned int down = 0, up = 0;
    while (SDL_PollEvent(&queued)) {
        if (queued.type == SDL_KEYDOWN || queued.type == SDL_KEYUP) {
            CHECK(queued.key.windowID == ph_window_id(w));
            CHECK(queued.key.keysym.scancode == expected);
            if (queued.type == SDL_KEYDOWN) down++; else up++;
        }
    }
    CHECK(down == 1 && up == 1);
}

int main(int argc, char **argv)
{
    const char *dir = argc == 3 && !strcmp(argv[1], "--screenshots") ? argv[2] : NULL;
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_RENDER_DRIVER", "software", 1);
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0);
    ph_window *w = ph_window_create("Porthole v" PORTHOLE_VERSION);
    CHECK(w);
    SDL_Window *native = SDL_GetWindowFromID(ph_window_id(w));
    SDL_SetWindowSize(native, 460, 740);
    ph_window_set_pull_hint(w, "192.0.2.42:47731  SAMPLE42");
    ph_window_set_pc_name(w, "Porthole Windows");
    ph_window_set_share_available(w, true);
    ph_window_redraw(w);
    screenshot(w, dir, "01-connect");

    int tx, ty;
    click_shortcut(w, 320, 16, SDL_SCANCODE_F1);
    CHECK(ph_window_connect_target(w, PH_TARGET_SETUP, &tx, &ty));
    click_shortcut(w, tx, ty, SDL_SCANCODE_F2);
    CHECK(ph_window_connect_target(w, PH_TARGET_HELP, &tx, &ty));
    click_shortcut(w, tx, ty, SDL_SCANCODE_F1);
    CHECK(ph_window_connect_target(w, PH_TARGET_STATS, &tx, &ty));
    click_shortcut(w, tx, ty, SDL_SCANCODE_F3);
    SDL_Event copy = {.type = SDL_MOUSEBUTTONDOWN};
    CHECK(ph_window_connect_target(w, PH_TARGET_COPY, &tx, &ty));
    copy.button.button = SDL_BUTTON_LEFT; copy.button.x = tx; copy.button.y = ty;
    CHECK(ph_window_chrome_event(w, &copy));
    copy.type = SDL_MOUSEBUTTONUP;
    CHECK(ph_window_chrome_event(w, &copy));
    char *clipboard = SDL_GetClipboardText();
    CHECK(clipboard && !strcmp(clipboard, "192.0.2.42:47731  SAMPLE42"));
    SDL_free(clipboard);
    SDL_Event fresh = {.type = SDL_MOUSEBUTTONDOWN};
    CHECK(ph_window_connect_target(w, PH_TARGET_NEW_CODE, &tx, &ty));
    fresh.button.button = SDL_BUTTON_LEFT; fresh.button.x = tx; fresh.button.y = ty;
    CHECK(ph_window_chrome_event(w, &fresh));
    fresh.type = SDL_MOUSEBUTTONUP;
    CHECK(ph_window_chrome_event(w, &fresh));
    CHECK(ph_window_take_new_code(w));
    CHECK(!ph_window_take_new_code(w));

    {
        uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
        uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
        CHECK(qrcodegen_encodeText("porthole://192.0.2.42:47731/SAMPLE42", temp, qr,
                                   qrcodegen_Ecc_MEDIUM, 1, 8, qrcodegen_Mask_AUTO, true));
        CHECK(qrcodegen_getSize(qr) >= 21);
        CHECK(qrcodegen_getModule(qr, 0, 0));
    }
    SDL_Event gear = {.type = SDL_MOUSEBUTTONDOWN};
    gear.button.button = SDL_BUTTON_LEFT;
    CHECK(ph_window_connect_target(w, PH_TARGET_SETTINGS, &tx, &ty));
    gear.button.x = tx; gear.button.y = ty;
    CHECK(ph_window_chrome_event(w, &gear));
    gear.type = SDL_MOUSEBUTTONUP;
    CHECK(ph_window_chrome_event(w, &gear));
    CHECK(ph_window_take_action(w) == PH_WINDOW_ACTION_NONE);
    screenshot(w, dir, "11-settings");
    SDL_Event row = {.type = SDL_MOUSEBUTTONDOWN};
    row.button.button = SDL_BUTTON_LEFT;
    CHECK(ph_window_settings_target(w, 1, 1, &tx, &ty)); /* Wheel scrolling: Feed */
    row.button.x = tx; row.button.y = ty;
    CHECK(ph_window_chrome_event(w, &row));
    row.type = SDL_MOUSEBUTTONUP;
    CHECK(ph_window_chrome_event(w, &row));
    CHECK(ph_window_take_action(w) == PH_WINDOW_ACTION_FEED);
    row.type = SDL_MOUSEBUTTONDOWN;                       /* Feed again: already on */
    CHECK(ph_window_chrome_event(w, &row));
    row.type = SDL_MOUSEBUTTONUP;
    CHECK(ph_window_chrome_event(w, &row));
    CHECK(ph_window_take_action(w) == PH_WINDOW_ACTION_NONE);
    CHECK(ph_window_settings_target(w, 5, 0, &tx, &ty));  /* Media keys switch */
    CHECK(ph_window_media_keys(w));
    row.type = SDL_MOUSEBUTTONDOWN; row.button.x = tx; row.button.y = ty;
    CHECK(ph_window_chrome_event(w, &row));
    row.type = SDL_MOUSEBUTTONUP;
    CHECK(ph_window_chrome_event(w, &row));
    CHECK(!ph_window_media_keys(w));
    CHECK(ph_window_take_action(w) == PH_WINDOW_ACTION_SAVE);
    CHECK(ph_window_settings_target(w, -1, 0, &tx, &ty)); /* Done */
    gear.type = SDL_MOUSEBUTTONDOWN; gear.button.x = tx; gear.button.y = ty;
    CHECK(ph_window_chrome_event(w, &gear));
    gear.type = SDL_MOUSEBUTTONUP;
    CHECK(ph_window_chrome_event(w, &gear));
    CHECK(ph_window_take_action(w) == PH_WINDOW_ACTION_NONE);
    CHECK(!ph_window_settings_target(w, 99, 0, &tx, &ty));
    ph_window_set_active_transfer(w, "Sending photo.jpg");
    CHECK(!ph_window_set_active_transfer(w, "Sending photo.jpg"));
    ph_window_note_transfer(w, "Sent photo.jpg");
    CHECK(!ph_window_take_cancel(w));

    SDL_Renderer *renderer = SDL_GetRenderer(native);
    const char *paragraph = "Readable guidance wraps consistently, including longlonglongwords.";
    int wrapped = ph_ui_text_height(paragraph, 130, 2);
    CHECK(wrapped > ph_ui_text_height(paragraph, 700, 2));
    CHECK(ph_ui_draw_text(renderer, paragraph, 10, 17, 130, 2,
                           (SDL_Color){0, 0, 0, 255}) == 17 + wrapped);
    /* Drawing at fractional and integer DPI must preserve layout and renderer state. */
    const float dpis[] = {1.0f, 1.25f, 1.5f, 2.0f};
    for (size_t i = 0; i < sizeof dpis / sizeof dpis[0]; i++) {
        SDL_RenderSetScale(renderer, dpis[i], dpis[i]);
        CHECK(ph_ui_draw_text(renderer, paragraph, 10, 17, 130, 2,
                               (SDL_Color){0, 0, 0, 255}) == 17 + wrapped);
        float sx, sy;
        SDL_RenderGetScale(renderer, &sx, &sy);
        CHECK(sx == dpis[i] && sy == dpis[i]);
    }
    SDL_RenderSetScale(renderer, 1, 1);

    SDL_SetWindowSize(native, 320, 480);
    ph_window_redraw(w); screenshot(w, dir, "02-compact");
    SDL_SetWindowSize(native, 352, 796);
    ph_window_redraw(w); screenshot(w, dir, "02b-phone-sized-connect");
    SDL_SetWindowSize(native, 960, 640);
    ph_window_redraw(w); screenshot(w, dir, "03-wide");
    SDL_SetWindowSize(native, 460, 740);
    ph_setup setup;
    ph_setup_open(&setup, true);
    ph_window_set_setup(w, &setup);
    screenshot(w, dir, "04-setup");
    SDL_SetWindowSize(native, 960, 680); ph_window_redraw(w);
    screenshot(w, dir, "04b-setup-wide");
    SDL_SetWindowSize(native, 460, 740); ph_window_redraw(w);
    CHECK(ph_window_setup_click(w, 220, 694) == PH_SETUP_ACTION_NONE); /* first Back */
    CHECK(ph_window_setup_click(w, 370, 694) == PH_SETUP_ACTION_NEXT);
    setup.step = PH_SETUP_BLUETOOTH;
    ph_window_set_setup(w, &setup);
    CHECK(ph_window_setup_click(w, 370, 694) == PH_SETUP_ACTION_NONE); /* disconnected Next */
    ph_setup_update(&setup, true, false);
    ph_window_set_setup(w, &setup);
    CHECK(ph_window_setup_click(w, 370, 694) == PH_SETUP_ACTION_NEXT);
    setup.visible = false;
    ph_window_set_setup(w, &setup);
    ph_window_toggle_help(w);
    screenshot(w, dir, "05-help");
    SDL_Event wheel = {.type = SDL_MOUSEWHEEL};
    wheel.wheel.y = -20;
    CHECK(ph_window_chrome_event(w, &wheel));
    screenshot(w, dir, "06-help-scrolled");
    SDL_Event key = {.type = SDL_KEYDOWN};
    key.key.keysym.scancode = SDL_SCANCODE_HOME;
    CHECK(ph_window_chrome_event(w, &key));
    ph_window_toggle_help(w);

    AVFrame *frame = av_frame_alloc();
    CHECK(frame);
    frame->width = 360; frame->height = 780; frame->format = AV_PIX_FMT_YUV420P;
    CHECK(av_frame_get_buffer(frame, 32) == 0);
    for (int plane = 0; plane < 3; plane++)
        for (int y = 0; y < (plane ? frame->height / 2 : frame->height); y++)
            memset(frame->data[plane] + y * frame->linesize[plane], plane ? 128 : 96,
                     (size_t)(plane ? frame->width / 2 : frame->width));
    ph_rect screen = {0, 0, 360, 780};
    ph_window_set_status(w, true, true, "on");
    ph_window_present(w, frame, &screen);
    SDL_SetWindowSize(native, 460, 740); ph_window_redraw(w);
    double vx, vy;
    CHECK(ph_window_to_video(w, 230, 386, &vx, &vy));
    CHECK(vx > .49 && vx < .51 && vy > .49 && vy < .51);
    CHECK(!ph_window_to_video(w, 230, 16, &vx, &vy));
    ph_window_toggle_help(w);
    CHECK(!ph_window_to_video(w, 230, 410, &vx, &vy));
    ph_window_toggle_help(w);
    setup.visible = true; ph_window_set_setup(w, &setup);
    CHECK(!ph_window_picture_point(w, 230, 410, &vx, &vy));
    setup.visible = false; ph_window_set_setup(w, &setup);
    ph_window_set_status(w, true, false, "Bluetooth disconnected");
    CHECK(!ph_window_to_video(w, 230, 410, &vx, &vy));
    CHECK(ph_window_picture_point(w, 230, 410, &vx, &vy));
    screenshot(w, dir, "07-viewing-only");
    {
        /* Phone-sized window, then Controls and Transfers open beside the picture. */
        int ww, wh;
        SDL_Event click = {.type = SDL_MOUSEBUTTONDOWN};
        click.button.button = SDL_BUTTON_LEFT;
        click.button.clicks = 1;
        SDL_SetWindowSize(native, 360, 812);
        ph_window_note_transfer(w, "Received 3 items");
        ph_window_note_transfer(w, "Sent Q3 report.pdf");
        ph_window_note_transfer(w, "Could not send Site photos.zip");
        ph_window_redraw(w);
        screenshot(w, dir, "09-phone-sized");
        click.button.x = 360 - 120 - 9 - 64 + 15; click.button.y = 16;  /* Controls */
        CHECK(ph_window_chrome_event(w, &click));
        click.type = SDL_MOUSEBUTTONUP;
        CHECK(ph_window_chrome_event(w, &click));
        SDL_GetWindowSize(native, &ww, &wh);
        CHECK(ww == 360 + 60);
        click.type = SDL_MOUSEBUTTONDOWN;
        click.button.x = ww - 120 - 9 - 32 + 15;                       /* Transfers */
        CHECK(ph_window_chrome_event(w, &click));
        click.type = SDL_MOUSEBUTTONUP;
        CHECK(ph_window_chrome_event(w, &click));
        SDL_GetWindowSize(native, &ww, &wh);
        CHECK(ww == 360 + 60 + 300);
        ph_window_set_active_transfer(w, "Sending IMG_4790.MOV");
        SDL_Event hover = {.type = SDL_MOUSEMOTION};
        hover.motion.x = 360 + 30; hover.motion.y = 32 + 10 + 20;      /* Home */
        ph_window_chrome_event(w, &hover);
        ph_window_redraw(w);
        screenshot(w, dir, "10-controls-transfers");
        click.type = SDL_MOUSEBUTTONDOWN;
        click.button.x = ww - 120 - 9 - 64 + 15;                        /* close Controls */
        CHECK(ph_window_chrome_event(w, &click));
        click.type = SDL_MOUSEBUTTONUP;
        CHECK(ph_window_chrome_event(w, &click));
        click.type = SDL_MOUSEBUTTONDOWN;
        click.button.x = ww - 60 - 120 - 9 - 32 + 15;                   /* close Transfers */
        CHECK(ph_window_chrome_event(w, &click));
        click.type = SDL_MOUSEBUTTONUP;
        CHECK(ph_window_chrome_event(w, &click));
        SDL_GetWindowSize(native, &ww, &wh);
        CHECK(ww == 360);
        ph_window_set_active_transfer(w, "");
    }
    av_frame_free(&frame);
    ph_window_clear(w);
    ph_window_set_pull_hint(w, "");
    ph_window_set_share_available(w, false);
    screenshot(w, dir, "08-sharing-unavailable");
    copy.type = SDL_MOUSEBUTTONDOWN;
    CHECK(!ph_window_chrome_event(w, &copy)); /* unavailable Copy cannot activate */
    ph_window_destroy(w);

    /* A fresh renderer must receive its own font atlas after the previous one dies. */
    w = ph_window_create("Porthole"); CHECK(w); ph_window_destroy(w);
    SDL_Quit();
    puts("UI: toolbar, setup gates, scrolling, fonts, renderer lifetime and phone mapping passed");
    return 0;
}
