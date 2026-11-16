/* Run real SDL event routing, pointer feedback, and click dispatch headlessly.
 * The fake phone sends its FINAL image before the HID ACK, then becomes idle.
 * A pointer controller that waits for another decoded-frame event will stall. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#include "app.h"
#include "airplay/receiver.h"
#include "hid/link.h"
#include "ui/window.h"
#include "video/decoder.h"

#define PHONE_WIDTH 500
#define PHONE_HEIGHT 1080
#define TARGET_X 250
#define TARGET_Y 700
#define DRAG_END_X 350
#define DRAG_END_Y 500

struct ph_window { int unused; };
struct ph_decoder { int unused; };
struct ph_receiver { int unused; };
struct ph_hid_link { int unused; };
static ph_window window;
static ph_decoder decoder;
static ph_receiver receiver;
static ph_hid_link fake_link;

static void (*frame_ready)(void *user);
static void *frame_user;
static ph_hid_link_status_fn link_status;
static void *link_user;
static AVFrame *latest;
static uint64_t frame_serial;
static double phone_x = 400, phone_y = 800;
static double pressed_x, pressed_y, released_x, released_y;
static uint8_t held_buttons;
static bool mirroring, control_ready, initial_frame, injected, timed_out;
static bool moved_since_frame, anchored_with_button, frame_failure;
static unsigned int presses, releases, movement_frames, movement_acks;
static unsigned int frames_after_last_ack;
static bool ack_emitted;
static bool drag_mode, release_waiting_sync, release_synced;
static unsigned int dragged_reports;
static Uint64 first_tick;

static void quit(void)
{
    SDL_Event event = {.type = SDL_QUIT};
    SDL_PushEvent(&event);
}

static void publish_frame(void)
{
    if (ack_emitted)
        frames_after_last_ack++;
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        frame_failure = true;
        quit();
        return;
    }
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = PHONE_WIDTH;
    frame->height = PHONE_HEIGHT;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        frame_failure = true;
        quit();
        return;
    }
    for (int y = 0; y < PHONE_HEIGHT; y++) {
        uint8_t *row = frame->data[0] + y * frame->linesize[0];
        for (int x = 0; x < PHONE_WIDTH; x++) {
            double radius = held_buttons & PH_HID_BTN_LEFT ? 9.5 : 11;
            double coverage = fmax(0, fmin(1, radius + .5 - hypot(x - phone_x, y - phone_y)));
            row[x] = (uint8_t)lround(16 + (90 - 16) * coverage);
        }
    }
    for (int plane = 1; plane <= 2; plane++)
        for (int y = 0; y < PHONE_HEIGHT / 2; y++)
            memset(frame->data[plane] + y * frame->linesize[plane], 128, PHONE_WIDTH / 2);
    av_frame_free(&latest);
    latest = frame;
    frame_serial++;
    frame_ready(frame_user);
}

ph_window *ph_window_create(const char *title)
{
    (void)title;
    return &window;
}

void ph_window_set_title(ph_window *w, const char *title) { (void)w, (void)title; }

void ph_window_set_status(ph_window *w, bool mirror, bool ready, const char *detail)
{
    (void)w, (void)detail;
    mirroring = mirror;
    control_ready = ready;
}

void ph_window_toggle_help(ph_window *w) { (void)w; }
bool ph_window_help_visible(const ph_window *w) { (void)w; return false; }
void ph_window_redraw(ph_window *w) { (void)w; }
void ph_window_clear(ph_window *w) { (void)w; initial_frame = false; }
void ph_window_destroy(ph_window *w) { (void)w; }
unsigned int ph_window_id(const ph_window *w) { (void)w; return 1; }

void ph_window_present(ph_window *w, const AVFrame *frame, const ph_rect *screen)
{
    (void)w, (void)frame, (void)screen;
    initial_frame = true;
}

void ph_window_video_size(const ph_window *w, int *width, int *height)
{
    (void)w;
    *width = initial_frame ? PHONE_WIDTH : 0;
    *height = initial_frame ? PHONE_HEIGHT : 0;
}

bool ph_window_to_video(const ph_window *w, int x, int y, double *vx, double *vy)
{
    (void)w;
    if (!initial_frame || !control_ready || x < 0 || x >= PHONE_WIDTH
        || y < 0 || y >= PHONE_HEIGHT)
        return false;
    *vx = (x + .5) / PHONE_WIDTH;
    *vy = (y + .5) / PHONE_HEIGHT;
    return true;
}

void ph_window_tick(ph_window *w)
{
    (void)w;
    if (!first_tick)
        first_tick = SDL_GetTicks64();
    if (SDL_GetTicks64() - first_tick > 4000) {
        timed_out = true;
        quit();
    }
    if (!injected && mirroring && control_ready && initial_frame) {
        injected = true;
        SDL_Event motion = {.type = SDL_MOUSEMOTION};
        motion.motion.windowID = 1;
        motion.motion.x = TARGET_X;
        motion.motion.y = TARGET_Y;
        SDL_PushEvent(&motion);
        SDL_Event button = {.type = SDL_MOUSEBUTTONDOWN};
        button.button.windowID = 1;
        button.button.button = SDL_BUTTON_LEFT;
        button.button.state = SDL_PRESSED;
        button.button.x = TARGET_X;
        button.button.y = TARGET_Y;
        SDL_PushEvent(&button);
        if (drag_mode) {
            motion.motion.state = SDL_BUTTON_LMASK;
            motion.motion.x = DRAG_END_X;
            motion.motion.y = DRAG_END_Y;
            SDL_PushEvent(&motion);
            button.button.x = DRAG_END_X;
            button.button.y = DRAG_END_Y;
        }
        button.type = SDL_MOUSEBUTTONUP;
        button.button.state = SDL_RELEASED;
        SDL_PushEvent(&button);
    }
}

ph_decoder *ph_decoder_start(void (*on_frame)(void *user), void *user)
{
    frame_ready = on_frame;
    frame_user = user;
    return &decoder;
}
void ph_decoder_push(ph_decoder *d, const uint8_t *data, size_t len)
{
    (void)d, (void)data, (void)len;
}
void ph_decoder_reset(ph_decoder *d) { (void)d; }

bool ph_decoder_latest(ph_decoder *d, AVFrame *dst, uint64_t *seq)
{
    (void)d;
    if (!latest || *seq == frame_serial || av_frame_ref(dst, latest) < 0)
        return false;
    *seq = frame_serial;
    return true;
}

void ph_decoder_stop(ph_decoder *d) { (void)d; av_frame_free(&latest); }

ph_receiver *ph_receiver_start(const ph_receiver_options *opt, const ph_receiver_callbacks *cb)
{
    (void)opt;
    cb->connected(cb->user, "Idle-frame phone", "test");
    publish_frame();
    return &receiver;
}
void ph_receiver_poll(ph_receiver *r) { (void)r; }
void ph_receiver_stop(ph_receiver *r) { (void)r; }

ph_hid_link *ph_hid_link_start(char *const argv[], ph_hid_link_status_fn on_status, void *user)
{
    (void)argv;
    link_status = on_status;
    link_user = user;
    on_status(user, "subscribed");
    return &fake_link;
}
void ph_hid_link_stop(ph_hid_link *l) { (void)l; }
bool ph_hid_link_keyboard(ph_hid_link *l, const ph_hid_keyboard *k)
{
    (void)l, (void)k;
    return true;
}
bool ph_hid_link_consumer(ph_hid_link *l, uint16_t usage)
{
    (void)l, (void)usage;
    return true;
}
bool ph_hid_link_delay(ph_hid_link *l, uint16_t milliseconds)
{
    (void)l, (void)milliseconds;
    return true;
}

static double travel(int value)
{
    double magnitude = fabs((double)value);
    double pixels = .25 * magnitude + .055 * magnitude * magnitude;
    return value < 0 ? -pixels : pixels;
}

bool ph_hid_link_mouse(ph_hid_link *l, const ph_hid_mouse *m)
{
    (void)l;
    if (m->dx || m->dy) {
        moved_since_frame = true;
        phone_x = fmin(PHONE_WIDTH - 16, fmax(16, phone_x + travel(m->dx)));
        phone_y = fmin(PHONE_HEIGHT - 16, fmax(16, phone_y + travel(m->dy)));
        if (m->buttons & PH_HID_BTN_LEFT)
            dragged_reports++;
    }
    if (m->dx == -127 && m->dy == -127 && m->buttons)
        anchored_with_button = true;
    if (!(held_buttons & PH_HID_BTN_LEFT) && (m->buttons & PH_HID_BTN_LEFT)) {
        presses++;
        pressed_x = phone_x;
        pressed_y = phone_y;
    }
    if ((held_buttons & PH_HID_BTN_LEFT) && !(m->buttons & PH_HID_BTN_LEFT)) {
        releases++;
        released_x = phone_x;
        released_y = phone_y;
        release_waiting_sync = true;
        quit();
    }
    held_buttons = m->buttons;
    return true;
}

bool ph_hid_link_sync(ph_hid_link *l, uint32_t id)
{
    (void)l;
    if (moved_since_frame) {
        /* Deliver this decoded image before the app receives the ACK event.
         * Deliberately never publish a frame after that ACK for this movement. */
        ack_emitted = false;
        publish_frame();
        movement_frames++;
        moved_since_frame = false;
        movement_acks++;
    }
    char status[32];
    snprintf(status, sizeof status, "sync %u", id);
    link_status(link_user, status);
    if (release_waiting_sync) {
        release_synced = true;
        release_waiting_sync = false;
    }
    ack_emitted = true;
    return true;
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--drag")))
        return 1;
    drag_mode = argc == 2;
    char config[] = "/tmp/porthole-pointer-XXXXXX";
    if (!mkdtemp(config))
        return 1;
    setenv("XDG_CONFIG_HOME", config, 1);
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    ph_app_options options = {.name = "Idle-frame test", .bluetooth = true,
                              .ble_helper = "unused fake helper"};
    int result = ph_app_run(&options);
    char saved_calibration[sizeof config + 32];
    snprintf(saved_calibration, sizeof saved_calibration, "%s/porthole/pointer-scale", config);
    unlink(saved_calibration);
    char app_config[sizeof config + 16];
    snprintf(app_config, sizeof app_config, "%s/porthole", config);
    rmdir(app_config);
    rmdir(config);
    double error = hypot(pressed_x - (TARGET_X + .5), pressed_y - (TARGET_Y + .5));
    double end_x = drag_mode ? DRAG_END_X + .5 : TARGET_X + .5;
    double end_y = drag_mode ? DRAG_END_Y + .5 : TARGET_Y + .5;
    double release_error = hypot(released_x - end_x, released_y - end_y);
    if (result || timed_out || frame_failure || !injected || presses != 1 || releases != 1
        || held_buttons || anchored_with_button || error > 5 || release_error > 5
        || !release_synced || (drag_mode && !dragged_reports)
        || movement_frames < (drag_mode ? 3U : 2U)
        || movement_frames != movement_acks || frames_after_last_ack) {
        fprintf(stderr, "idle-frame pointer failed: rc=%d timeout=%d frame_error=%d injected=%d "
                        "presses=%u releases=%u held=%u error=%.2f release_error=%.2f "
                        "release_synced=%d dragged_reports=%u anchored_held=%d "
                        "movement_frames=%u movement_acks=%u post_ack_frames=%u\n",
                result, timed_out, frame_failure, injected, presses, releases, held_buttons,
                error, release_error, release_synced, dragged_reports, anchored_with_button,
                movement_frames, movement_acks,
                frames_after_last_ack);
        return 1;
    }
    puts(drag_mode ? "SDL drag aligned and released with the smaller pressed cursor"
                   : "SDL click aligned using the retained pre-ACK frame, without another video frame");
    return 0;
}
