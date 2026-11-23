/* Exercise the real software-rendered UI, including its modal/phone boundary.
 * Optional --screenshots DIR writes actual app renders for visual review. */
#ifdef _WIN32
#define COBJMACROS
#include <oleacc.h>
#include <SDL_syswm.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>

#include "control/setup.h"
#include "core/config.h"
#include "core/secure_link.h"
#include "core/upath.h"
#include "files/places.h"
#include "temp_path.h"
#include "core/version.h"
#include "ui/control_icons.h"
#include "ui/drawing.h"
#include "ui/text.h"
#include "ui/transfer_card.h"
#include "ui/vendor/qrcodegen.h"
#include "ui/window.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s (%s)\n", __FILE__, __LINE__, #condition, SDL_GetError()); \
    exit(1); } } while (0)

static void screenshot_renderer(SDL_Renderer *renderer, const char *dir, const char *name)
{
    if (!dir) return;
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

static void screenshot(ph_window *window, const char *dir, const char *name)
{
    screenshot_renderer(SDL_GetRenderer(SDL_GetWindowFromID(ph_window_id(window))), dir, name);
}

static Uint8 pixel_red(const SDL_Surface *surface, int x, int y)
{
    return ((const Uint8 *)surface->pixels)[y * surface->pitch + x * 4];
}

static void test_scaled_corners_and_icons(SDL_Surface *surface, SDL_Renderer *renderer, float dpi)
{
    CHECK(SDL_RenderSetScale(renderer, dpi, dpi) == 0);
    CHECK(SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255) == 0);
    CHECK(SDL_RenderClear(renderer) == 0);
    SDL_Rect clip = {0, 0, 96, 62};
    CHECK(SDL_RenderSetClipRect(renderer, &clip) == 0);
    ph_ui_round_rect(renderer, (SDL_Rect){12, 12, 48, 28}, 8, (SDL_Color){0, 0, 0, 255});
    ph_ui_round_rect(renderer, (SDL_Rect){92, 42, 20, 20}, 6, (SDL_Color){0, 0, 0, 255});
    ph_icon_draw(renderer, PH_ICON_CLOSE, (SDL_Rect){68, 12, 20, 20}, (SDL_Color){0, 0, 0, 255});
    SDL_Rect actual;
    SDL_RenderGetClipRect(renderer, &actual);
    CHECK(SDL_RenderIsClipEnabled(renderer) && SDL_RectEquals(&clip, &actual));
    float sx, sy;
    SDL_RenderGetScale(renderer, &sx, &sy);
    CHECK(sx == dpi && sy == dpi);
    SDL_RenderPresent(renderer);

    int left = (int)lroundf(12 * dpi), top = left, right = (int)lroundf(60 * dpi);
    int radius = (int)lroundf(8 * dpi);
    CHECK(pixel_red(surface, left, top) == 255);
    for (int x = left + radius; x < right - radius; x++)
        CHECK(pixel_red(surface, x, top) == 0);
    CHECK(pixel_red(surface, (int)(101 * dpi), (int)(53 * dpi)) == 255);
    int corner_pixels = 0, icon_pixels = 0;
    for (int y = 0; y < radius; y++)
        for (int x = 0; x < radius; x++) {
            Uint8 red = pixel_red(surface, left + x, top + y);
            CHECK(red == pixel_red(surface, right - 1 - x, top + y));
            corner_pixels += red > 0 && red < 255;
        }
    for (int y = (int)(12 * dpi); y < (int)(32 * dpi); y++)
        for (int x = (int)(68 * dpi); x < (int)(88 * dpi); x++) {
            Uint8 red = pixel_red(surface, x, y);
            icon_pixels += red > 0 && red < 255;
        }
    CHECK(corner_pixels >= radius && icon_pixels > 8);
    CHECK(SDL_RenderSetClipRect(renderer, NULL) == 0);
}

static void test_scaled_workspace(ph_history *history, const char *dir)
{
    ph_workspace *workspace = ph_workspace_create();
    CHECK(workspace);
    ph_workspace_set_history(workspace, history);
    enum { WIDTH = 1120, HEIGHT = 650 };
    const int scales[] = {100, 125, 150, 200};
    for (size_t i = 0; i < SDL_arraysize(scales); i++) {
        int percent = scales[i], x, y;
        SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
            0, WIDTH * percent / 100, HEIGHT * percent / 100, 32, SDL_PIXELFORMAT_RGBA32);
        CHECK(surface);
        SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(surface);
        CHECK(renderer && ph_text_attach(renderer));
        test_scaled_corners_and_icons(surface, renderer, percent / 100.f);
        SDL_Rect area = {0, 0, WIDTH, HEIGHT};
        ph_workspace_live live = {.pc = "Porthole Windows", .address = "192.0.2.42:47731",
                                  .code = "SAMPLE42", .share_available = true};
        ph_workspace_draw(workspace, renderer, area, PH_WORKSPACE_HOME, &live);
        CHECK(ph_workspace_target(workspace, PH_WORKSPACE_COPY_ADDRESS, &x, &y) && x < WIDTH * 2 / 3);
        CHECK(ph_workspace_target(workspace, PH_WORKSPACE_SETUP, &x, &y) && x > WIDTH / 2);
        SDL_RenderPresent(renderer);
        char label[40];
        snprintf(label, sizeof label, "connect-%d-percent", percent);
        screenshot_renderer(renderer, dir, label);

        live.paired = live.connected = live.clipboard_available = true;
        snprintf(live.phone, sizeof live.phone, "iPhone");
        ph_workspace_draw(workspace, renderer, area, PH_WORKSPACE_HOME, &live);
        CHECK(ph_workspace_target(workspace, PH_WORKSPACE_SEND_FILES, &x, &y) && x > WIDTH / 2);
        CHECK(ph_workspace_target(workspace, PH_WORKSPACE_CLIPBOARD_OPEN, &x, &y));
        SDL_RenderPresent(renderer);
        snprintf(label, sizeof label, "connected-%d-percent", percent);
        screenshot_renderer(renderer, dir, label);
        ph_ui_release_drawing(renderer);
        ph_icon_release(renderer);
        ph_text_detach(renderer);
        SDL_DestroyRenderer(renderer);
        SDL_FreeSurface(surface);
    }
    ph_workspace_destroy(workspace);
}

static void click_at(ph_window *window, int x, int y)
{
    SDL_Event event = {0};
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.windowID = ph_window_id(window);
    event.button.x = x;
    event.button.y = y;
    CHECK(ph_window_chrome_event(window, &event));
    event.type = SDL_MOUSEBUTTONUP;
    CHECK(ph_window_chrome_event(window, &event));
}

static void click_workspace(ph_window *window, ph_workspace_action action)
{
    int x, y;
    CHECK(ph_workspace_target(ph_window_workspace(window), action, &x, &y));
    click_at(window, x, y);
}

static const unsigned char clipboard_image[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x02, 0x00, 0x00, 0x00, 0xfd, 0xd4, 0x9a, 0x73, 0x00, 0x00, 0x00,
    0x16, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x78, 0xe6, 0x66, 0x23,
    0xb2, 0x20, 0x80, 0xc1, 0x3c, 0xef, 0xe1, 0xaf, 0x43, 0xba, 0x00, 0x26,
    0x13, 0x05, 0xdc, 0xeb, 0x07, 0xdf, 0x59, 0x00, 0x00, 0x00, 0x00, 0x49,
    0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

static void check_clipboard_thumbnail(SDL_Renderer *renderer)
{
    SDL_Rect thumbnail = {78, 230, 120, 114};
    SDL_Surface *pixels = SDL_CreateRGBSurfaceWithFormat(
        0, thumbnail.w, thumbnail.h, 32, SDL_PIXELFORMAT_RGBA32);
    CHECK(pixels);
    CHECK(SDL_RenderReadPixels(renderer, &thumbnail, pixels->format->format,
                               pixels->pixels, pixels->pitch) == 0);
    int colored = 0;
    for (int y = 0; y < pixels->h; y++)
        for (int x = 0; x < pixels->w; x++) {
            const Uint8 *point = (const Uint8 *)pixels->pixels + y * pixels->pitch + x * 4;
            colored += point[0] > 200 && point[1] < 95 && point[2] < 95;
        }
    SDL_FreeSurface(pixels);
    CHECK(colored > 100);
}

static int section_y(int index)
{
    return PH_WINDOW_BAR_PX + 47 + 18 + index * 41 + 19;
}

static void capture_settings_pages(ph_window *w, const char *dir, const char *prefix)
{
    const char *names[] = {"appearance", "screen-and-audio", "controls", "transfers",
                          "notifications", "help", "connection-statistics", "about"};
    SDL_Window *native = SDL_GetWindowFromID(ph_window_id(w));
    ph_workspace *workspace = ph_window_workspace(w);
    SDL_SetWindowSize(native, 1280, 800);
    for (int i = 0; i < (int)SDL_arraysize(names); i++) {
        ph_window_redraw(w);
        click_at(w, 65, section_y(i));
        CHECK(ph_workspace_current_section(workspace) == i);
        char name[64];
        snprintf(name, sizeof name, "%s%s", prefix, names[i]);
        screenshot(w, dir, name);
    }
}

static void capture_settings(ph_window *w, const char *dir)
{
    ph_workspace *workspace = ph_window_workspace(w);
    capture_settings_pages(w, dir, "");
    int section = ph_workspace_current_section(workspace);
    ph_window_toggle_statistics(w);
    CHECK(ph_workspace_current_section(workspace) == 6 && ph_window_help_visible(w));
    ph_window_toggle_statistics(w);
    CHECK(ph_workspace_current_section(workspace) == section && ph_window_help_visible(w));
    click_workspace(w, PH_WORKSPACE_CLOSE);
    CHECK(!ph_window_help_visible(w));
    ph_window_toggle_help(w);
    CHECK(ph_workspace_current_section(workspace) == 5);
    click_at(w, 350, 335);
    screenshot(w, dir, "help-guide-open");
    click_at(w, 350, 263);
    screenshot(w, dir, "keyboard-shortcuts");
    ph_window_toggle_help(w);
    CHECK(!ph_window_help_visible(w));
}

static void workspace_key(ph_window *window, SDL_Scancode key, SDL_Keymod mod)
{
    SDL_Event event = {0};
    event.type = SDL_KEYDOWN;
    event.key.windowID = ph_window_id(window);
    event.key.keysym.scancode = key;
    event.key.keysym.sym = SDL_GetKeyFromScancode(key);
    event.key.keysym.mod = mod;
    CHECK(ph_window_chrome_event(window, &event));
}

static void assert_setup_instructions(SDL_Window *window)
{
    SDL_Renderer *renderer = SDL_GetRenderer(window);
    SDL_Rect body = {50, 214, 350, 210};
    uint32_t *pixels = malloc((size_t)body.w * (size_t)body.h * sizeof *pixels);
    SDL_PixelFormat *format = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA8888);
    CHECK(pixels && format);
    CHECK(SDL_RenderReadPixels(renderer, &body, format->format, pixels,
                                body.w * (int)sizeof *pixels) == 0);
    int lines = 0;
    for (int y = 0; y < body.h; y++) {
        int dark = 0;
        for (int x = 0; x < body.w; x++) {
            Uint8 r, g, b;
            SDL_GetRGB(pixels[y * body.w + x], format, &r, &g, &b);
            dark += r < 96 && g < 96 && b < 96;
        }
        if (dark > 24)
            lines++;
    }
    CHECK(lines > 10);
    SDL_FreeFormat(format);
    free(pixels);
}

#ifdef _WIN32
static void test_windows_accessibility(ph_window *w, SDL_Window *window)
{
    ph_window_show(w);
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    CHECK(SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_WINDOWS);
    IAccessible *root = NULL;
    CHECK(SUCCEEDED(AccessibleObjectFromWindow(info.info.win.window, OBJID_CLIENT, &IID_IAccessible,
                                               (void **)&root)) && root);
    LONG count = 0;
    CHECK(IAccessible_get_accChildCount(root, &count) == S_OK && count > 3);
    bool copied_address = false;
    for (LONG i = 1; i <= count; i++) {
        VARIANT child;
        VariantInit(&child);
        V_VT(&child) = VT_I4;
        V_I4(&child) = i;
        BSTR name = NULL;
        CHECK(IAccessible_get_accName(root, child, &name) == S_OK && name);
        bool address = wcscmp(name, L"Copy address") == 0;
        SysFreeString(name);
        if (!address)
            continue;
        VARIANT role;
        CHECK(IAccessible_get_accRole(root, child, &role) == S_OK &&
              V_I4(&role) == ROLE_SYSTEM_PUSHBUTTON);
        BSTR value = NULL;
        CHECK(IAccessible_get_accValue(root, child, &value) == S_OK && value);
        CHECK(!wcscmp(value, L"192.0.2.42:47731"));
        SysFreeString(value);
        CHECK(IAccessible_accDoDefaultAction(root, child) == S_OK);
        char *text = SDL_GetClipboardText();
        CHECK(text && !strcmp(text, "192.0.2.42:47731"));
        SDL_free(text);
        copied_address = true;
        break;
    }
    CHECK(copied_address);
    IAccessible_Release(root);
}

static void click_role(ph_window *w, const char *name)
{
    ph_workspace *workspace = ph_window_workspace(w);
    for (int i = 0; i < ph_workspace_accessible_count(workspace); i++) {
        ph_workspace_control control;
        if (!ph_workspace_accessible_control(workspace, i, &control) || strcmp(control.name, name))
            continue;
        click_at(w, control.bounds.x + control.bounds.w / 2,
                 control.bounds.y + control.bounds.h / 2);
        return;
    }
    fprintf(stderr, "Missing Windows UI control: %s\n", name);
    exit(1);
}

static void close_workspace_view(ph_window *w)
{
    click_workspace(w, PH_WORKSPACE_CLOSE);
}

static void windows_visual_review(ph_window *w, SDL_Window *native, const char *dir)
{
    test_windows_accessibility(w, native);
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    CHECK(SDL_GetWindowWMInfo(native, &info) && info.subsystem == SDL_SYSWM_WINDOWS);
    SDL_RendererInfo renderer;
    CHECK(SDL_GetRendererInfo(SDL_GetRenderer(native), &renderer) == 0);
    printf("Windows native SDL backend: %s, renderer: %s, window DPI: %u\n",
           SDL_GetCurrentVideoDriver(), renderer.name, (unsigned)GetDpiForWindow(info.info.win.window));

    ph_workspace *workspace = ph_window_workspace(w);
    SDL_SetWindowSize(native, 1280, 800);
    ph_window_redraw(w);
    click_role(w, "Settings");
    click_role(w, "General");
    click_role(w, "Light");
    CHECK(!ph_workspace_dark(workspace));
    close_workspace_view(w);
    screenshot(w, dir, "connect");
    const int sizes[][2] = {{1024, 680}, {460, 740}};
    const char *sizes_names[] = {"connect-1024", "connect-compact"};
    for (size_t i = 0; i < SDL_arraysize(sizes_names); i++) {
        SDL_SetWindowSize(native, sizes[i][0], sizes[i][1]);
        ph_window_redraw(w);
        screenshot(w, dir, sizes_names[i]);
    }
    puts("Pairing window and accessible copy action passed");

    ph_window_set_code_open(w, false);
    ph_window_set_companion(w, true, false, 72);
    ph_window_set_phone_name(w, "iPhone");
    ph_window_set_clipboard_available(w, true);
    ph_history *history = ph_history_open(NULL);
    CHECK(history);
    ph_workspace_set_history(workspace, history);
    SDL_SetWindowSize(native, 1120, 680);
    ph_window_open_received_history(w);
    screenshot(w, dir, "received-history-empty");
    close_workspace_view(w);
    ph_file_entry sent = {.name = "Boarding pass.pdf", .bytes = 624000, .confirmed = 624000,
                          .state = PH_TRANSFER_COMPLETE};
    ph_file_entry received = {.name = "Café photo.png", .incoming = true, .bytes = 3800000,
                              .confirmed = 3800000, .state = PH_TRANSFER_COMPLETE};
    char folder[1024];
    CHECK(ph_test_temp_dir(folder, sizeof folder));
    CHECK(snprintf(received.path, sizeof received.path, "%s/%s", folder, received.name)
          < (int)sizeof received.path);
    FILE *saved = ph_fopen(received.path, "wb");
    CHECK(saved && fwrite(clipboard_image, sizeof clipboard_image, 1, saved) == 1);
    CHECK(fclose(saved) == 0);
    CHECK(ph_history_add_file(history, &sent) && ph_history_add_file(history, &received));
    SDL_SetWindowSize(native, 1120, 680);
    ph_window_redraw(w);
    screenshot(w, dir, "connected");
    ph_window_set_receiving(w, 0, 1);
    ph_window_set_incoming(w, "Presentation.pdf", 1800000, 4000000);
    screenshot(w, dir, "receiving");
    ph_window_set_incoming(w, "", 0, 0);
    ph_window_set_receiving(w, 0, 0);
    ph_window_open_received_history(w);
    screenshot(w, dir, "received-history");
    click_role(w, received.name);
    screenshot(w, dir, "received-file-detail");
    click_workspace(w, PH_WORKSPACE_REVEAL);
    CHECK(ph_window_take_workspace_action(w, NULL) == PH_WORKSPACE_REVEAL);
    CHECK(ph_place_reveal(received.path, true));
    close_workspace_view(w);
    puts("File history and Explorer reveal with a Unicode filename passed");

    ph_transfer_card *receipt = ph_transfer_card_create();
    CHECK(receipt);
    SDL_Window *receipt_window = SDL_GetWindowFromID(ph_transfer_card_id(receipt));
    CHECK(receipt_window && (SDL_GetWindowFlags(receipt_window) & SDL_WINDOW_ALLOW_HIGHDPI));
    Uint64 now = SDL_GetTicks64();
    ph_transfer_card_place(receipt, "Downloads");
    ph_transfer_card_show(receipt, received.path, 1, 1, true, now);
    ph_transfer_card_tick(receipt, now + 500);
    screenshot_renderer(SDL_GetRenderer(receipt_window), dir, "receipt-light");
    ph_transfer_card_set_theme(receipt, true);
    ph_transfer_card_tick(receipt, now + 501);
    screenshot_renderer(SDL_GetRenderer(receipt_window), dir, "receipt-dark");
    ph_transfer_card_destroy(receipt);

    SDL_SetWindowSize(native, 1280, 800);
    ph_window_open_settings(w);
    capture_settings(w, dir);
    ph_window_toggle_help(w);
    click_role(w, "Guides");
    click_role(w, "Search help and shortcuts");
    SDL_Event input = {.type = SDL_TEXTINPUT};
    input.text.windowID = ph_window_id(w);
    snprintf(input.text.text, sizeof input.text.text, "broadcast");
    CHECK(ph_window_chrome_event(w, &input));
    screenshot(w, dir, "help-search-broadcast");
    workspace_key(w, SDL_SCANCODE_A, KMOD_LCTRL);
    snprintf(input.text.text, sizeof input.text.text, "no-matching-guide");
    CHECK(ph_window_chrome_event(w, &input));
    screenshot(w, dir, "help-search-no-results");
    click_role(w, "Controls");
    screenshot(w, dir, "controls-with-setup-button");
    ph_setup setup;
    ph_setup_open(&setup, true);
    ph_window_set_setup(w, &setup);
    screenshot(w, dir, "setup-from-settings");
    CHECK(ph_window_setup_click(w, 1255, PH_WINDOW_BAR_PX + 24) == PH_SETUP_ACTION_CLOSE);
    ph_setup_close(&setup);
    ph_window_set_setup(w, &setup);
    CHECK(ph_window_help_visible(w) && ph_workspace_current_section(workspace) == 2);
    click_role(w, "Enable PC controls (on)");
    CHECK(ph_window_take_action(w) == PH_WINDOW_ACTION_PAUSE);
    click_role(w, "Feed");
    CHECK(ph_window_take_action(w) == PH_WINDOW_ACTION_FEED);
    click_role(w, "Relative");
    CHECK(ph_window_take_action(w) == PH_WINDOW_ACTION_SAVE);
    click_role(w, "Screen and audio");
    click_role(w, "Keep the iPhone aspect ratio (on)");
    CHECK(!ph_workspace_keep_aspect(workspace));
    click_role(w, "Keep the iPhone aspect ratio (off)");
    CHECK(ph_workspace_keep_aspect(workspace));
    click_role(w, "Notifications");
    click_role(w, "Transfer notifications (on)");
    CHECK(!ph_workspace_notifications(workspace));
    click_role(w, "Transfer notifications (off)");
    CHECK(ph_workspace_notifications(workspace));
    click_role(w, "Transfers");
    click_role(w, "Clipboard sync (on)");
    CHECK(!ph_workspace_clip_send(workspace) && !ph_workspace_clip_receive(workspace));
    click_role(w, "Clipboard sync (off)");
    CHECK(ph_workspace_clip_send(workspace) && ph_workspace_clip_receive(workspace));
    click_role(w, "Downloads");
    CHECK(ph_window_take_action(w) == PH_WINDOW_ACTION_PICK_FOLDER);
    CHECK(ph_history_add_clip(history, 'T', "Clipboard sample", 16, true));
    CHECK(ph_history_add_clip(history, 'P', clipboard_image, sizeof clipboard_image, false));
    click_workspace(w, PH_WORKSPACE_HISTORY_OPEN);
    close_workspace_view(w);
    CHECK(ph_window_help_visible(w) && ph_workspace_current_section(workspace) == 3);
    click_workspace(w, PH_WORKSPACE_CLIPBOARD_OPEN);
    screenshot(w, dir, "clipboard-history-with-thumbnail");
    close_workspace_view(w);
    CHECK(ph_window_help_visible(w) && ph_workspace_current_section(workspace) == 3);
    click_role(w, "General");
    if (!ph_config_launch_managed() && !ph_config_launch_at_sign_in()) {
        click_role(w, "Launch at sign-in (off)");
        bool enabled = ph_config_launch_at_sign_in();
        if (enabled)
            click_role(w, "Launch at sign-in (on)");
        bool removed = !ph_config_launch_at_sign_in();
        CHECK(enabled && removed);
    }
    click_role(w, "Dark");
    CHECK(ph_workspace_dark(workspace));
    capture_settings_pages(w, dir, "dark-");
    click_role(w, "General");
    click_role(w, "System");
    char prefs[96];
    CHECK(ph_config_read_line("desktop-preferences", prefs, sizeof prefs) && prefs[0] == '2');
    click_role(w, "Light");
    close_workspace_view(w);
    puts("All Settings pages, themes, Help search and preference actions passed");

    AVFrame *frame = av_frame_alloc();
    CHECK(frame);
    frame->width = 360;
    frame->height = 780;
    frame->format = AV_PIX_FMT_YUV420P;
    CHECK(av_frame_get_buffer(frame, 32) == 0);
    for (int plane = 0; plane < 3; plane++) {
        for (int y = 0; y < (plane ? frame->height / 2 : frame->height); y++)
            memset(frame->data[plane] + y * frame->linesize[plane], plane ? 128 : 100,
                   (size_t)(plane ? frame->width / 2 : frame->width));
    }
    ph_rect screen = {0, 0, 360, 780};
    ph_window_set_status(w, true, true, "on");
    ph_window_present(w, frame, &screen);
    screenshot(w, dir, "sharing");
    ph_window_open_settings(w);
    ph_workspace_open_section(workspace, 0);
    ph_window_redraw(w);
    screenshot(w, dir, "sharing-settings");
    ph_workspace_open_section(workspace, 2);
    ph_window_redraw(w);
    screenshot(w, dir, "sharing-controls");
    close_workspace_view(w);
    CHECK(!ph_window_help_visible(w));
    ph_metrics_snapshot metrics = {.fps_known = true, .fps = 60,
                                   .receive_to_present = {.known = true, .mean_ms = 7.2}};
    ph_window_set_metrics(w, &metrics, 0, 0, 0);
    ph_window_toggle_statistics(w);
    CHECK(ph_workspace_current_section(workspace) == 6);
    screenshot(w, dir, "sharing-statistics");
    close_workspace_view(w);
    av_frame_free(&frame);
    test_scaled_workspace(history, dir);
    ph_workspace_set_history(workspace, NULL);
    ph_history_close(history);
    CHECK(ph_remove(received.path) == 0);
    puts("Screen sharing fixture and 100%, 125%, 150%, 200% raster checks passed");
}
#endif

static void click_shortcut(ph_window *w, int x, int y, SDL_Scancode expected)
{
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONDOWN;
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
#ifdef _WIN32
    bool test_accessibility = argc == 2 && !strcmp(argv[1], "--accessibility");
    bool native_review = argc == 3 && !strcmp(argv[1], "--windows-visual-review");
    if (native_review) {
        dir = argv[2];
        SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
        SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
        SDL_setenv("SDL_VIDEODRIVER", "windows", 1);
    }
#else
    const bool test_accessibility = false;
    const bool native_review = false;
#endif
    char config[1024];
    CHECK(ph_test_temp_dir(config, sizeof config));
#ifdef _WIN32
    SDL_setenv("APPDATA", config, 1);
#else
    SDL_setenv("XDG_CONFIG_HOME", config, 1);
#endif
    if (!test_accessibility && !native_review)
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
#ifdef _WIN32
    if (native_review) {
        windows_visual_review(w, native, dir);
        ph_window_destroy(w);
        SDL_Quit();
        return 0;
    }
    if (test_accessibility) {
        test_windows_accessibility(w, native);
        ph_window_destroy(w);
        SDL_Quit();
        puts("Windows accessibility: named controls, roles, values and actions passed");
        return 0;
    }
#endif
    screenshot(w, dir, "01-connect");

    int tx, ty;
    ph_workspace *workspace = ph_window_workspace(w);
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_SETUP, &tx, &ty));
    click_shortcut(w, tx, ty, SDL_SCANCODE_F2);
    SDL_Event copy;
    SDL_zero(copy);
    copy.type = SDL_MOUSEBUTTONDOWN;
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_COPY_ADDRESS, &tx, &ty));
    copy.button.button = SDL_BUTTON_LEFT; copy.button.x = tx; copy.button.y = ty;
    CHECK(ph_window_chrome_event(w, &copy));
    copy.type = SDL_MOUSEBUTTONUP;
    CHECK(ph_window_chrome_event(w, &copy));
    char *clipboard = SDL_GetClipboardText();
    CHECK(clipboard && !strcmp(clipboard, "192.0.2.42:47731"));
    SDL_free(clipboard);
    SDL_Event fresh;
    SDL_zero(fresh);
    fresh.type = SDL_MOUSEBUTTONDOWN;
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_NEW_CODE, &tx, &ty));
    fresh.button.button = SDL_BUTTON_LEFT; fresh.button.x = tx; fresh.button.y = ty;
    CHECK(ph_window_chrome_event(w, &fresh));
    fresh.type = SDL_MOUSEBUTTONUP;
    CHECK(ph_window_chrome_event(w, &fresh));
    CHECK(ph_window_take_new_code(w));
    CHECK(!ph_window_take_new_code(w));

    {
        uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
        uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
        char uri[200];
        snprintf(uri, sizeof uri, "porthole://192.0.2.42:47731/SAMPLE42?pin=%s",
                 ph_tls_fingerprint());
        CHECK(strlen(ph_tls_fingerprint()) == 64);
        CHECK(qrcodegen_encodeText(uri, temp, qr,
                                   qrcodegen_Ecc_MEDIUM, 1, 8, qrcodegen_Mask_AUTO, true));
        CHECK(qrcodegen_getSize(qr) >= 21);
        CHECK(qrcodegen_getModule(qr, 0, 0));
    }
    SDL_Event gear;
    SDL_zero(gear);
    gear.type = SDL_MOUSEBUTTONDOWN;
    gear.button.button = SDL_BUTTON_LEFT;
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_SETTINGS_OPEN, &tx, &ty));
    gear.button.x = tx; gear.button.y = ty;
    CHECK(ph_window_chrome_event(w, &gear));
    gear.type = SDL_MOUSEBUTTONUP;
    CHECK(ph_window_chrome_event(w, &gear));
    CHECK(ph_window_take_action(w) == PH_WINDOW_ACTION_NONE);
    screenshot(w, dir, "11-settings");
    SDL_SetWindowSize(native, 960, 720);
    ph_window_redraw(w);
    click_at(w, 65, section_y(2));
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_FEED, &tx, &ty));
    click_at(w, tx, ty);
    CHECK(ph_window_take_action(w) == PH_WINDOW_ACTION_FEED);
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_MEDIA, &tx, &ty));
    click_at(w, tx, ty);
    CHECK(!ph_window_media_keys(w));
    CHECK(ph_window_take_action(w) == PH_WINDOW_ACTION_SAVE);
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_CLOSE, &tx, &ty));
    click_at(w, tx, ty);
    CHECK(!ph_window_help_visible(w));
    ph_window_set_code_open(w, false);
    ph_window_set_companion(w, true, false, 100);
    ph_window_set_phone_name(w, "iPhone");
    ph_history *history = ph_history_open(NULL);
    ph_workspace_set_history(workspace, history);
    ph_file_entry received = {.incoming = true,
                              .state = PH_TRANSFER_COMPLETE,
                              .name = "Sample photo.heic",
                              .bytes = 3800000,
                              .confirmed = 3800000};
    CHECK(snprintf(received.path, sizeof received.path, "%s/moved-folder/Sample photo.heic",
                   config) < (int)sizeof received.path);
    CHECK(ph_history_add_file(history, &received) != 0);
    ph_window_redraw(w);
    screenshot(w, dir, "12-connected");
    ph_window_set_receiving(w, 0, 1);
    ph_window_set_incoming(w, "Presentation.pdf", 2000000, 4000000);
    screenshot(w, dir, "14-receiving");
    ph_window_set_active_transfer(w, "Sending example.txt");
    static const char history_profile[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char unread_key[80];
    snprintf(unread_key, sizeof unread_key, "unread-%s", history_profile);
    ph_window_set_history_profile(w, history_profile);
    ph_window_note_received(w, 2);
    char count[16];
    CHECK(ph_config_read_line(unread_key, count, sizeof count) && !strcmp(count, "2"));
    ph_window_set_history_profile(w, NULL);
    ph_window_set_history_profile(w, history_profile);
    const int widths[] = {320, 960};
    for (size_t i = 0; i < SDL_arraysize(widths); ++i) {
        SDL_SetWindowSize(native, widths[i], 480);
        ph_window_redraw(w);
        CHECK(ph_workspace_target(workspace, PH_WORKSPACE_HISTORY_OPEN, &tx, &ty));
        CHECK(ph_workspace_target(workspace, PH_WORKSPACE_REVEAL, &tx, &ty));
        CHECK(ph_workspace_target(workspace, PH_WORKSPACE_CANCEL_SEND, &tx, &ty));
        click_at(w, tx, ty);
        CHECK(ph_window_take_cancel(w));
    }
    SDL_SetWindowSize(native, 960, 720);
    ph_window_set_active_transfer(w, "");
    ph_window_set_incoming(w, "", 0, 0);
    ph_window_set_receiving(w, 0, 0);
    ph_window_open_received_history(w);
    CHECK(ph_config_read_line(unread_key, count, sizeof count) && !strcmp(count, "0"));
    /* Select the first row on File history. The recorded parent has moved. */
    click_at(w, 122, PH_WINDOW_BAR_PX + 160 + 10 + 44 + 14);
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_LOCATE, &tx, &ty));
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_CLOSE, &tx, &ty));
    click_at(w, tx, ty);
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_SETTINGS_OPEN, &tx, &ty));
    click_at(w, tx, ty);
    ph_workspace_open_section(workspace, 0);
    ph_window_redraw(w);
    click_at(w, 550, 326);
    CHECK(ph_workspace_dark(workspace));
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_CLOSE, &tx, &ty));
    click_at(w, tx, ty);
    screenshot(w, dir, "15-connected-dark");
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_SETTINGS_OPEN, &tx, &ty));
    click_at(w, tx, ty);
    ph_workspace_open_section(workspace, 8);
    ph_session_state devices = {
        .count = 2,
        .devices = {
            {.id = "phone:one", .name = "First iPhone", .connected = true, .active = true,
             .companion = true},
            {.id = "phone:two", .name = "Second iPhone", .companion = true},
        },
    };
    CHECK(ph_workspace_set_sessions(workspace, &devices));
    ph_window_redraw(w);
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_DEVICE_SELECT, &tx, &ty));
    click_at(w, tx, ty);
    CHECK(ph_window_take_workspace_action(w, NULL) == PH_WORKSPACE_DEVICE_SELECT);
    CHECK(!strcmp(ph_workspace_device(workspace), "phone:two"));
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_ADD_PHONE, &tx, &ty));
    click_at(w, tx, ty);
    CHECK(ph_window_take_workspace_action(w, NULL) == PH_WORKSPACE_ADD_PHONE);
    screenshot(w, dir, "16-devices-dark");
    ph_workspace_open_section(workspace, 0);
    ph_window_redraw(w);
    click_at(w, 330, 326);
    CHECK(!ph_workspace_dark(workspace));
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_CLOSE, &tx, &ty));
    click_at(w, tx, ty);

    ph_window_open_settings(w);
    capture_settings(w, dir);
    SDL_SetWindowSize(native, 960, 720);
    ph_window_open_settings(w);
    ph_workspace_open_section(workspace, 3);
    ph_window_redraw(w);
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_CLIPBOARD_OPEN, &tx, &ty));
    CHECK(ph_history_add_clip(history, 'T', "Clipboard sample", 16, true) != 0);
    click_at(w, tx, ty);
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_CLIP_COPY, &tx, &ty));
    CHECK(!ph_workspace_target(workspace, PH_WORKSPACE_CLIP_SEND, &tx, &ty));
    ph_window_set_clipboard_available(w, true);
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_CLIP_SEND, &tx, &ty));
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_CLIP_COPY, &tx, &ty));
    SDL_Event changing = {0};
    changing.type = SDL_MOUSEBUTTONDOWN;
    changing.button.button = SDL_BUTTON_LEFT;
    changing.button.x = tx;
    changing.button.y = ty;
    CHECK(ph_window_chrome_event(w, &changing));
    CHECK(ph_history_add_clip(history, 'T', "Another item", 12, true) != 0);
    ph_window_redraw(w);
    changing.type = SDL_MOUSEBUTTONUP;
    CHECK(ph_window_chrome_event(w, &changing));
    CHECK(ph_window_take_workspace_action(w, NULL) == PH_WORKSPACE_NONE);
    click_at(w, tx, ty);
    uint64_t item = 0;
    CHECK(ph_window_take_workspace_action(w, &item) == PH_WORKSPACE_CLIP_COPY &&
          item == ph_history_clip(history, 0)->id);
    CHECK(ph_history_add_clip(history, 'P', clipboard_image, sizeof clipboard_image, false));
    ph_window_redraw(w);
    check_clipboard_thumbnail(SDL_GetRenderer(native));
    screenshot(w, dir, "13-clipboard");
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_CLOSE, &tx, &ty));
    workspace_key(w, SDL_SCANCODE_TAB, KMOD_NONE);
    CHECK(ph_workspace_owns_keyboard(workspace));
#ifdef _WIN32
    ph_workspace_control focused = {0};
    for (int i = 0; i < ph_workspace_accessible_count(workspace); ++i) {
        ph_workspace_control control;
        if (ph_workspace_accessible_control(workspace, i, &control) && control.focused)
            focused = control;
    }
    if (focused.action != PH_WORKSPACE_CLOSE)
        fprintf(stderr, "Clipboard Tab focus: %s (action %d)\n", focused.name, focused.action);
    CHECK(focused.action == PH_WORKSPACE_CLOSE);
#endif
    workspace_key(w, SDL_SCANCODE_RETURN, KMOD_NONE);
    CHECK(ph_window_help_visible(w) && ph_workspace_current_section(workspace) == 3);
    click_workspace(w, PH_WORKSPACE_CLOSE);
    CHECK(!ph_window_help_visible(w));
    ph_window_set_code_open(w, true);
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
    SDL_SetWindowSize(native, 1280, 800);
    ph_window_redraw(w); screenshot(w, dir, "connect");
    SDL_SetWindowSize(native, 460, 740);
    ph_setup setup;
    ph_setup_open(&setup, true);
    ph_window_set_setup(w, &setup);
    assert_setup_instructions(native);
    screenshot(w, dir, "04-setup");
    SDL_SetWindowSize(native, 960, 680); ph_window_redraw(w);
    screenshot(w, dir, "04b-setup-wide");
    SDL_SetWindowSize(native, 460, 740); ph_window_redraw(w);
    CHECK(ph_window_setup_click(w, 220, 694) == PH_SETUP_ACTION_BACK);
    ph_setup_back(&setup);
    CHECK(!setup.visible);
    ph_setup_open(&setup, true);
    ph_window_set_setup(w, &setup);
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
    SDL_SetWindowSize(native, 460, 480);
    ph_window_redraw(w);
    screenshot(w, dir, "05-help");
    workspace_key(w, SDL_SCANCODE_PAGEDOWN, KMOD_NONE);
    screenshot(w, dir, "06-help-scrolled");
    SDL_Event key;
    SDL_zero(key);
    key.type = SDL_KEYDOWN;
    key.key.keysym.scancode = SDL_SCANCODE_HOME;
    ph_window_chrome_event(w, &key);
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
    ph_window_open_pairing(w);
    CHECK(ph_workspace_target(workspace, PH_WORKSPACE_COPY_ADDRESS, &tx, &ty));
    CHECK(!ph_window_picture_point(w, 230, 390, &vx, &vy));
    ph_window_set_code_open(w, false);
    CHECK(ph_window_picture_point(w, 230, 390, &vx, &vy));
    ph_window_set_code_open(w, true);
    ph_window_toggle_diagnostics(w);
    CHECK(ph_window_help_visible(w));
    CHECK(!ph_window_picture_point(w, 230, 390, &vx, &vy));
    click_at(w, 230, 390);
    workspace_key(w, SDL_SCANCODE_ESCAPE, KMOD_NONE);
    CHECK(!ph_window_help_visible(w));
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
        SDL_Event click;
        SDL_zero(click);
        click.type = SDL_MOUSEBUTTONDOWN;
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
        SDL_Event hover;
        SDL_zero(hover);
        hover.type = SDL_MOUSEMOTION;
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
    CHECK(!ph_workspace_target(workspace, PH_WORKSPACE_COPY_ADDRESS, &tx, &ty));
    test_scaled_workspace(history, dir);
    ph_history_close(history);
    ph_window_destroy(w);

    /* A fresh renderer must receive its own font atlas after the previous one dies. */
    w = ph_window_create("Porthole"); CHECK(w); ph_window_destroy(w);
    SDL_Quit();
    puts("UI: toolbar, setup gates, scrolling, fonts, renderer lifetime and phone mapping passed");
    return 0;
}
