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
struct ph_hid_link { bool stopping; };
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
static bool modal_mode, setup_visible, setup_seen, keyboard_held, modal_checked;
static unsigned int modal_stage, modal_leaks, premodal_keys, resumed_keys;
static unsigned int dragged_reports;
static Uint64 first_tick;
/* The paced fixture applies reports to the simulated phone only at delivery
 * time. Enqueue success is deliberately different from notification delivery. */
enum paced_kind { PACED_MOUSE, PACED_KEY, PACED_SYNC, PACED_DELAY };
typedef struct paced_report {
    enum paced_kind kind;
    ph_hid_mouse mouse;
    ph_hid_keyboard key;
    uint32_t sync;
    uint16_t delay;
} paced_report;
static paced_report delivery[2048];
static size_t delivery_head, delivery_count;
static Uint64 delivery_at, canceled_at, released_at, injected_at;
static bool paced_mode, after_ack, freeze_mode, freeze_video, cancel_mode;
static bool fault_queued, fault_delivered, queue_overflow, draining_stop;
static bool delivery_applying;
static bool filtered_mode, calibration_mode, calibration_seen, calibration_running;
static bool render_stall_mode, render_stalled;
static unsigned int filtered_events;
static unsigned int fault_stage, link_starts, stop_requests, delivered_after_cancel;
static unsigned int frame_delay_ms;
static const char *fault_name;
static ph_receiver_callbacks receiver_cb;
static bool delayed_frame;
static Uint64 frame_due;
static double delayed_x, delayed_y;
static uint8_t delayed_buttons;

static void apply_delivery(const paced_report *report);
static void pump_delivery(void);

static int filter_app_events(void *user, SDL_Event *event)
{
    (void)user;
    if (event->type >= SDL_USEREVENT) { filtered_events++; return 0; }
    return 1;
}

static bool enqueue(paced_report report)
{
    if (delivery_count == sizeof delivery / sizeof delivery[0]) {
        queue_overflow = true;
        return false;
    }
    size_t tail = (delivery_head + delivery_count++) % (sizeof delivery / sizeof delivery[0]);
    delivery[tail] = report;
    if (!delivery_at) delivery_at = SDL_GetTicks64() + 17;
    return true;
}

static void quit(void)
{
    SDL_Event event = {.type = SDL_QUIT};
    SDL_PushEvent(&event);
}

static void push_key(SDL_Scancode scancode, bool down, SDL_Keymod modifiers)
{
    SDL_Event event = {.type = down ? SDL_KEYDOWN : SDL_KEYUP};
    event.key.windowID = 1;
    event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.key.keysym.scancode = scancode;
    event.key.keysym.mod = modifiers;
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
void ph_window_set_setup(ph_window *w, const ph_setup *setup)
{
    (void)w;
    setup_visible = setup->visible;
    setup_seen |= setup_visible;
    calibration_running = setup->calibrating;
    calibration_seen |= calibration_running;
}
ph_setup_action ph_window_setup_click(const ph_window *w, int x, int y)
{ (void)w; (void)x; (void)y; return PH_SETUP_ACTION_NONE; }
void ph_window_toggle_diagnostics(ph_window *w) { (void)w; }
void ph_window_set_metrics(ph_window *w, const ph_metrics_snapshot *m, unsigned int queued,
                           uint64_t discarded, uint64_t superseded)
{ (void)w; (void)m; (void)queued; (void)discarded; (void)superseded; }
void ph_window_redraw(ph_window *w) { (void)w; }
void ph_window_clear(ph_window *w) { (void)w; initial_frame = false; }
void ph_window_destroy(ph_window *w) { (void)w; }
unsigned int ph_window_id(const ph_window *w) { (void)w; return 1; }

void ph_window_present(ph_window *w, const AVFrame *frame, const ph_rect *screen)
{
    (void)w, (void)frame, (void)screen;
    if (render_stall_mode && movement_frames && !render_stalled) {
        render_stalled = true;
        /* The matching ACK is already queued behind this frame. A long UI
         * stall must drain that ACK before transport timeout evaluation. */
        SDL_Delay(3200);
    }
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
    if (SDL_GetTicks64() - first_tick > (paced_mode ? 11000U : 4000U)) {
        timed_out = true;
        quit();
    }
    if (filtered_mode && SDL_GetTicks64() - first_tick > 60) { quit(); return; }
    if (calibration_mode && mirroring && control_ready && initial_frame && !injected) {
        injected = true;
        push_key(SDL_SCANCODE_F2, true, KMOD_NONE);
        push_key(SDL_SCANCODE_F2, false, KMOD_NONE);
        for (int i = 0; i < 4; i++) {
            push_key(SDL_SCANCODE_RETURN, true, KMOD_NONE);
            push_key(SDL_SCANCODE_RETURN, false, KMOD_NONE);
        }
        return;
    }
    if (calibration_mode && calibration_running && delivery_count && !fault_stage) {
        fault_stage = 1;
        if (!strcmp(fault_name, "calibration-escape")) {
            push_key(SDL_SCANCODE_ESCAPE, true, KMOD_NONE);
            push_key(SDL_SCANCODE_ESCAPE, false, KMOD_NONE);
        } else {
            SDL_Event focus = {.type = SDL_WINDOWEVENT};
            focus.window.windowID = 1;
            focus.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
            SDL_PushEvent(&focus);
        }
    }
    if (calibration_mode && fault_stage && !calibration_running && !delivery_count) {
        fault_delivered = true;
        quit();
        return;
    }
    if (calibration_mode) return;
    if (paced_mode && delayed_frame && SDL_GetTicks64() >= frame_due) {
        double saved_x = phone_x, saved_y = phone_y;
        uint8_t saved_buttons = held_buttons;
        phone_x = delayed_x; phone_y = delayed_y; held_buttons = delayed_buttons;
        publish_frame();
        phone_x = saved_x; phone_y = saved_y; held_buttons = saved_buttons;
        delayed_frame = false;
    }
    if ((cancel_mode || freeze_mode) && presses && !fault_stage) {
        fault_stage = 1;
        if (cancel_mode) push_key(SDL_SCANCODE_B, true, KMOD_NONE);
        SDL_Event motion = {.type = SDL_MOUSEMOTION};
        motion.motion.windowID = 1;
        motion.motion.state = SDL_BUTTON_LMASK;
        motion.motion.x = DRAG_END_X;
        motion.motion.y = DRAG_END_Y;
        SDL_PushEvent(&motion);
        if (freeze_mode) freeze_video = true;
    }
    if (fault_stage == 1 && dragged_reports && delivery_count) {
        fault_stage = 2;
        canceled_at = SDL_GetTicks64();
        if (cancel_mode) {
            fault_queued = true;
            if (!strcmp(fault_name, "disconnect")) receiver_cb.disconnected(receiver_cb.user);
            else if (!strcmp(fault_name, "close")) quit();
            else if (!strcmp(fault_name, "modal")) push_key(SDL_SCANCODE_F2, true, KMOD_NONE);
            else {
                SDL_Event focus = {.type = SDL_WINDOWEVENT};
                focus.window.windowID = 1;
                focus.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
                SDL_PushEvent(&focus);
            }
        }
    }
    if ((cancel_mode || freeze_mode) && releases && !held_buttons && !keyboard_held
        && !delivery_count && !delayed_frame) {
        fault_delivered = true;
        if (!freeze_mode || SDL_GetTicks64() - released_at > 3300) quit();
    }
    if (paced_mode && !cancel_mode && !freeze_mode && releases && !delivery_count) quit();
    if (modal_mode && mirroring && control_ready && initial_frame) {
        if (modal_stage == 0) {
            modal_stage = 1;
            /* Prove the keyboard channel is live before opening the local
             * modal, and that opening it releases this held phone key. */
            push_key(SDL_SCANCODE_B, true, KMOD_NONE);
            push_key(SDL_SCANCODE_F2, true, KMOD_NONE);
            push_key(SDL_SCANCODE_F2, false, KMOD_NONE);
            push_key(SDL_SCANCODE_A, true, KMOD_NONE);
            push_key(SDL_SCANCODE_A, false, KMOD_NONE);
            push_key(SDL_SCANCODE_LALT, true, KMOD_LALT);
            push_key(SDL_SCANCODE_H, true, KMOD_LALT);
            push_key(SDL_SCANCODE_H, false, KMOD_LALT);
            push_key(SDL_SCANCODE_LALT, false, KMOD_NONE);
            SDL_Event motion = {.type = SDL_MOUSEMOTION};
            motion.motion.windowID = 1;
            motion.motion.x = TARGET_X;
            motion.motion.y = TARGET_Y;
            SDL_PushEvent(&motion);
            SDL_Event wheel = {.type = SDL_MOUSEWHEEL};
            wheel.wheel.windowID = 1;
            wheel.wheel.y = -1;
            SDL_PushEvent(&wheel);
            SDL_Event button = {.type = SDL_MOUSEBUTTONDOWN};
            button.button.windowID = 1;
            button.button.button = SDL_BUTTON_LEFT;
            button.button.x = TARGET_X;
            button.button.y = TARGET_Y;
            SDL_PushEvent(&button);
            button.type = SDL_MOUSEBUTTONUP;
            SDL_PushEvent(&button);
            return;
        }
        if (modal_stage == 1) {
            modal_checked = setup_seen && setup_visible && !keyboard_held && premodal_keys > 0;
            if (!modal_checked || modal_leaks) {
                quit();
                return;
            }
            modal_stage = 2;
            push_key(SDL_SCANCODE_ESCAPE, true, KMOD_NONE);
            push_key(SDL_SCANCODE_ESCAPE, false, KMOD_NONE);
            push_key(SDL_SCANCODE_A, true, KMOD_NONE);
            push_key(SDL_SCANCODE_A, false, KMOD_NONE);
            return;
        }
        if (modal_stage == 2) {
            if (setup_visible || !resumed_keys) {
                quit();
                return;
            }
            modal_stage = 3; /* ordinary click below must work after closing F2 */
        }
    }
    if (!injected && mirroring && control_ready && initial_frame) {
        injected = true;
        injected_at = SDL_GetTicks64();
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
        if (drag_mode && !cancel_mode && !freeze_mode) {
            motion.motion.state = SDL_BUTTON_LMASK;
            motion.motion.x = DRAG_END_X;
            motion.motion.y = DRAG_END_Y;
            SDL_PushEvent(&motion);
            button.button.x = DRAG_END_X;
            button.button.y = DRAG_END_Y;
        }
        if (!cancel_mode && !freeze_mode) {
            button.type = SDL_MOUSEBUTTONUP;
            button.button.state = SDL_RELEASED;
            SDL_PushEvent(&button);
        }
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

bool ph_decoder_latest_timed(ph_decoder *d, AVFrame *dst, uint64_t *seq, ph_frame_timing *timing)
{
    memset(timing, 0, sizeof *timing);
    return ph_decoder_latest(d, dst, seq);
}

void ph_decoder_get_stats(ph_decoder *d, ph_decoder_stats *stats)
{
    (void)d;
    memset(stats, 0, sizeof *stats);
}

void ph_decoder_stop(ph_decoder *d) { (void)d; av_frame_free(&latest); }

ph_receiver *ph_receiver_start(const ph_receiver_options *opt, const ph_receiver_callbacks *cb)
{
    (void)opt;
    receiver_cb = *cb;
    if (filtered_mode) SDL_SetEventFilter(filter_app_events, NULL);
    cb->connected(cb->user, "Idle-frame phone", "test");
    publish_frame();
    return &receiver;
}
void ph_receiver_poll(ph_receiver *r) { (void)r; }
void ph_receiver_stop(ph_receiver *r) { (void)r; }

ph_hid_link *ph_hid_link_start(char *const argv[], ph_hid_link_status_fn on_status, void *user)
{
    (void)argv;
    link_starts++;
    link_status = on_status;
    link_user = user;
    fake_link.stopping = false;
    on_status(user, "subscribed");
    return &fake_link;
}
void ph_hid_link_stop(ph_hid_link *l)
{
    if (!l) return;
    /* A real helper drains previously accepted releases at EOF. Do not make
     * shutdown look safe merely by dropping queued reports in this fixture. */
    draining_stop = true;
    Uint64 deadline = SDL_GetTicks64() + 2500;
    while (delivery_count && SDL_GetTicks64() < deadline) { pump_delivery(); SDL_Delay(1); }
    l->stopping = true;
    if (paced_mode) link_status(link_user, "exited"); /* producer callback after loop exit */
}
void ph_hid_link_request_stop(ph_hid_link *l)
{ if (l) { stop_requests++; l->stopping = true; } }
bool ph_hid_link_poll(ph_hid_link *l)
{
    pump_delivery();
    return !l || l->stopping;
}
bool ph_hid_link_keyboard(ph_hid_link *l, const ph_hid_keyboard *k)
{
    (void)l;
    if (paced_mode) return enqueue((paced_report){.kind = PACED_KEY, .key = *k});
    bool nonzero = k->modifiers != 0;
    for (size_t i = 0; i < sizeof k->keys; i++)
        nonzero |= k->keys[i] != 0;
    keyboard_held = nonzero;
    if (nonzero) {
        if (setup_visible)
            modal_leaks++;
        else if (modal_stage == 1)
            premodal_keys++;
        else if (modal_stage == 2)
            resumed_keys++;
    }
    return true;
}
bool ph_hid_link_consumer(ph_hid_link *l, uint16_t usage)
{
    (void)l;
    if (setup_visible && usage)
        modal_leaks++;
    return true;
}
bool ph_hid_link_delay(ph_hid_link *l, uint16_t milliseconds)
{
    (void)l, (void)milliseconds;
    if (paced_mode) return enqueue((paced_report){.kind = PACED_DELAY, .delay = milliseconds});
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
    if (paced_mode) return enqueue((paced_report){.kind = PACED_MOUSE, .mouse = *m});
    if (setup_visible && (m->buttons || m->dx || m->dy || m->wheel || m->pan))
        modal_leaks++;
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
        if (!delivery_applying) quit();
    }
    held_buttons = m->buttons;
    return true;
}

bool ph_hid_link_sync(ph_hid_link *l, uint32_t id)
{
    (void)l;
    if (paced_mode) return enqueue((paced_report){.kind = PACED_SYNC, .sync = id});
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

static void apply_delivery(const paced_report *report)
{
    /* Reuse the original physical model only on delivery, never on enqueue. */
    paced_mode = false;
    delivery_applying = true;
    if (report->kind == PACED_MOUSE) {
        unsigned int before = releases;
        if (canceled_at && (report->mouse.dx || report->mouse.dy)) delivered_after_cancel++;
        bool saved_cancel = cancel_mode, saved_freeze = freeze_mode;
        cancel_mode = freeze_mode = true; /* suppress original immediate quit below */
        ph_hid_link_mouse(&fake_link, &report->mouse);
        cancel_mode = saved_cancel; freeze_mode = saved_freeze;
        if (releases != before) released_at = SDL_GetTicks64();
    } else if (report->kind == PACED_KEY) {
        ph_hid_link_keyboard(&fake_link, &report->key);
    } else if (report->kind == PACED_SYNC) {
        if (moved_since_frame) {
            moved_since_frame = false;
            movement_acks++;
            if (!freeze_video && !draining_stop) {
                if (after_ack) {
                    delayed_frame = true; frame_due = SDL_GetTicks64() + frame_delay_ms;
                    delayed_x = phone_x; delayed_y = phone_y; delayed_buttons = held_buttons;
                } else {
                    ack_emitted = false; publish_frame(); movement_frames++;
                }
            }
        }
        char status[32]; snprintf(status, sizeof status, "sync %u", report->sync);
        link_status(link_user, status);
        /* Old and duplicate acknowledgements are intentionally interleaved. */
        if (report->sync > 0x80000001U) {
            snprintf(status, sizeof status, "sync %u", report->sync - 1);
            link_status(link_user, status);
        }
        ack_emitted = true;
        if (release_waiting_sync) { release_synced = true; release_waiting_sync = false; }
    }
    paced_mode = true;
    delivery_applying = false;
}

static void pump_delivery(void)
{
    if (!paced_mode) return;
    Uint64 now = SDL_GetTicks64();
    if (!delivery_count || now < delivery_at) return;
    paced_report report = delivery[delivery_head];
    delivery_head = (delivery_head + 1) % (sizeof delivery / sizeof delivery[0]);
    delivery_count--;
    delivery_at = now + (report.kind == PACED_DELAY ? report.delay : 17);
    apply_delivery(&report);
}

int main(int argc, char **argv)
{
    if (argc > 2)
        return 1;
    drag_mode = argc == 2 && (!strcmp(argv[1], "--drag") || !strcmp(argv[1], "--paced-drag"));
    modal_mode = argc == 2 && !strcmp(argv[1], "--modal");
    paced_mode = argc == 2 && (!strcmp(argv[1], "--paced") || !strcmp(argv[1], "--paced-after-ack")
        || !strcmp(argv[1], "--paced-drag")
        || !strcmp(argv[1], "--render-stall")
        || !strcmp(argv[1], "--freeze-drag") || !strcmp(argv[1], "--disconnect-drag")
        || !strcmp(argv[1], "--focus-drag") || !strcmp(argv[1], "--close-drag")
        || !strcmp(argv[1], "--modal-drag") || !strcmp(argv[1], "--calibration-focus")
        || !strcmp(argv[1], "--calibration-escape"));
    filtered_mode = argc == 2 && !strcmp(argv[1], "--filtered-events");
    render_stall_mode = argc == 2 && !strcmp(argv[1], "--render-stall");
    calibration_mode = argc == 2 && (!strcmp(argv[1], "--calibration-focus")
        || !strcmp(argv[1], "--calibration-escape"));
    if (argc == 2 && !drag_mode && !modal_mode && !paced_mode && !filtered_mode) return 1;
    freeze_mode = argc == 2 && !strcmp(argv[1], "--freeze-drag");
    cancel_mode = argc == 2 && (!strcmp(argv[1], "--disconnect-drag")
        || !strcmp(argv[1], "--focus-drag") || !strcmp(argv[1], "--close-drag")
        || !strcmp(argv[1], "--modal-drag"));
    fault_name = argc == 2 && !strcmp(argv[1], "--disconnect-drag") ? "disconnect"
        : argc == 2 && !strcmp(argv[1], "--close-drag") ? "close"
        : argc == 2 && !strcmp(argv[1], "--modal-drag") ? "modal"
        : calibration_mode ? argv[1] + 2 : "focus";
    after_ack = argc == 2 && (!strcmp(argv[1], "--paced-after-ack") || !strcmp(argv[1], "--paced-drag"));
    frame_delay_ms = 80;
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
    bool calibration_written = access(saved_calibration, F_OK) == 0;
    unlink(saved_calibration);
    char app_config[sizeof config + 16];
    snprintf(app_config, sizeof app_config, "%s/porthole", config);
    rmdir(app_config);
    rmdir(config);
    if (filtered_mode) {
        bool passed = !result && !timed_out && filtered_events >= 2 && !injected && link_starts == 1;
        fprintf(passed ? stdout : stderr, "filtered app events: pass=%d rejected=%u helper_starts=%u\n",
                passed, filtered_events, link_starts);
        return passed ? 0 : 1;
    }
    if (calibration_mode) {
        bool passed = !result && !timed_out && !queue_overflow && calibration_seen
            && !calibration_running && fault_delivered && !presses && !held_buttons
            && !keyboard_held && !delivery_count && link_starts == 1 && !stop_requests
            && !calibration_written;
        fprintf(passed ? stdout : stderr,
                "paced calibration %s: pass=%d seen=%d running=%d canceled=%d queued=%zu restarts=%u\n",
                fault_name, passed, calibration_seen, calibration_running, fault_delivered,
                delivery_count, stop_requests);
        return passed ? 0 : 1;
    }
    if (paced_mode) {
        bool faults = cancel_mode || freeze_mode;
        double start_error = hypot(pressed_x - (TARGET_X + .5), pressed_y - (TARGET_Y + .5));
        double end_error = hypot(released_x - (drag_mode ? DRAG_END_X + .5 : TARGET_X + .5),
                                 released_y - (drag_mode ? DRAG_END_Y + .5 : TARGET_Y + .5));
        bool released = releases == 1 && !held_buttons && !keyboard_held && !delivery_count;
        bool passed = !result && !timed_out && !frame_failure && !queue_overflow
            && link_starts == 1 && !stop_requests && presses == 1 && released && release_synced
            && start_error <= 5;
        if (!faults) passed &= end_error <= 5 && (!drag_mode || dragged_reports);
        if (render_stall_mode) passed &= render_stalled;
        if (faults) passed &= fault_stage == 2 && (freeze_mode || fault_queued)
            && released_at >= canceled_at && released_at - canceled_at <= 1000;
        if (cancel_mode && strcmp(fault_name, "close")) passed &= fault_delivered;
        fprintf(passed ? stdout : stderr,
                "paced app %s: pass=%d rc=%d timeout=%d helper_starts=%u stops=%u "
                "presses=%u releases=%u held=%u key=%d queued=%zu release_synced=%d "
                "%s=%llu ms queued_motion_after_cancel=%u stage=%u start_error=%.2f end_error=%.2f\n",
                argc == 2 ? argv[1] : "", passed, result, timed_out, link_starts, stop_requests,
                presses, releases, held_buttons, keyboard_held, delivery_count, release_synced,
                faults ? "cancel_to_release" : "input_to_release",
                (unsigned long long)(released_at - (faults ? canceled_at : injected_at)),
                delivered_after_cancel, fault_stage, start_error, end_error);
        return passed ? 0 : 1;
    }
    double error = hypot(pressed_x - (TARGET_X + .5), pressed_y - (TARGET_Y + .5));
    double end_x = drag_mode ? DRAG_END_X + .5 : TARGET_X + .5;
    double end_y = drag_mode ? DRAG_END_Y + .5 : TARGET_Y + .5;
    double release_error = hypot(released_x - end_x, released_y - end_y);
    if (result || timed_out || frame_failure || !injected || presses != 1 || releases != 1
        || held_buttons || anchored_with_button || error > 5 || release_error > 5
        || !release_synced || (drag_mode && !dragged_reports)
        || movement_frames < (drag_mode ? 3U : 2U)
        || movement_frames != movement_acks || frames_after_last_ack
        || (modal_mode && (!modal_checked || modal_leaks || !resumed_keys || modal_stage != 3))) {
        fprintf(stderr, "idle-frame pointer failed: rc=%d timeout=%d frame_error=%d injected=%d "
                        "presses=%u releases=%u held=%u error=%.2f release_error=%.2f "
                        "release_synced=%d dragged_reports=%u anchored_held=%d "
                        "movement_frames=%u movement_acks=%u post_ack_frames=%u\n",
                result, timed_out, frame_failure, injected, presses, releases, held_buttons,
                error, release_error, release_synced, dragged_reports, anchored_with_button,
                movement_frames, movement_acks,
                frames_after_last_ack);
        if (modal_mode)
            fprintf(stderr, "setup modal failed: stage=%u seen=%d visible=%d checked=%d "
                            "leaks=%u held_key=%d premodal_keys=%u resumed_keys=%u\n",
                    modal_stage, setup_seen, setup_visible, modal_checked, modal_leaks,
                    keyboard_held, premodal_keys, resumed_keys);
        return 1;
    }
    puts(modal_mode ? "F2 setup releases keys, suppresses phone input and resumes controls on close"
                   : drag_mode ? "SDL drag aligned and released with the smaller pressed cursor"
                   : "SDL click aligned using the retained pre-ACK frame, without another video frame");
    return 0;
}
