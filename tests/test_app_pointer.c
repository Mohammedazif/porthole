/* Run real SDL event routing, pointer feedback, and click dispatch headlessly.
 * The fake phone sends its FINAL image before the HID ACK, then becomes idle.
 * A pointer controller that waits for another decoded-frame event will stall. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include "temp_path.h"

#include <SDL.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#include "app.h"
#include "airplay/receiver.h"
#include "hid/link.h"
#include "ui/window.h"
#include "ui/drag_ghost.h"
#include "video/decoder.h"

#define PHONE_WIDTH 500
#define PHONE_HEIGHT 1080
#define TARGET_X 250
#define TARGET_Y 700
#define DRAG_END_X 350
#define DRAG_END_Y 500
#define REENTER_X 150
#define REENTER_Y 600

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
static atomic_uint_fast64_t frame_serial;
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
static bool confirmation_mode, changing_mode, stale_mode, feedback_waiting, feedback_bad;
static bool probe_pending;
static bool stream_mode, filter_one_frame;
static atomic_bool stream_stop;
static SDL_Thread *stream_thread;
static unsigned int filtered_frames;
static unsigned int probe_stage, confirmation_count;
static Uint64 confirmation_ack_at, probe_fresh_at, confirmation_waits[64];
static unsigned int filtered_events;
static unsigned int fault_stage, link_starts, stop_requests, delivered_after_cancel;
static bool connection_lifecycle, lifecycle_bad, lifecycle_exit;
static unsigned int lifecycle_stage;
static Uint64 lifecycle_at;
static unsigned int frame_delay_ms;
static const char *fault_name;
static ph_receiver_callbacks receiver_cb;
static bool delayed_frame;
static Uint64 frame_due;
static double delayed_x, delayed_y;
static uint8_t delayed_buttons;
static bool reentry_mode, reentry_focus, cancel_after_ack;
static unsigned int reentry_stage, interrupted_reports, anchor_reports;
static Uint64 last_move_ack_at, interrupted_at;
/* These modes exercise SDL -> app -> HID routing, independently of the module
 * unit tests. Keep report observations separate from the phone's motion model. */
enum controls_kind { CONTROLS_NONE, CONTROLS_MEDIA, CONTROLS_FUNCTIONS,
                     CONTROLS_MOUSE, CONTROLS_WHEEL, CONTROLS_FEED, CONTROLS_POINTER_POSITION,
                     CONTROLS_PRESENTATION };
static enum controls_kind controls_mode;
static unsigned int controls_stage, help_toggles, diagnostics_toggles;
static Uint64 controls_at;
static bool controls_bad, controls_done;
static uint16_t consumer_reports[32];
static unsigned int consumer_count, function_down[4], function_up[4];
static bool function_held[4];
static uint8_t keyboard_modifiers;
static unsigned int middle_down, middle_up, extra_down, extra_up;
static unsigned int wheel_reports, pan_reports;
static int wheel_sum, pan_sum;
static unsigned int wheel_before_shift;
static unsigned int control_mouse_reports;
static int presentation_width = PHONE_WIDTH, presentation_height = PHONE_HEIGHT;
static int presented_width, presented_height;
static bool presentation_blocked, landscape_accepted, absolute_presentation;
static unsigned int presentation_leaks, abs_reports, abs_presses;
enum photo_case { PHOTO_NONE, PHOTO_DRAG, PHOTO_JITTER,
                  PHOTO_SWIPE, PHOTO_TAP, PHOTO_ESCAPE, PHOTO_FOCUS, PHOTO_FAST_EXIT,
                  PHOTO_NATIVE_EXIT, PHOTO_FULL, PHOTO_CTRL, PHOTO_CTRL_TAP };
static enum photo_case photo_mode;
static unsigned int photo_stage, photo_requests, photo_presses, photo_releases;
static Uint64 photo_at;
static int photo_x = TARGET_X, photo_y = TARGET_Y;
/* Read only by the Linux-only SDL_GetMouseState wrap. */
static __attribute__((unused)) uint8_t photo_buttons, photo_phone_buttons;
static double photo_release_x, photo_release_y;
#ifdef PH_TEST_WRAP_GLOBAL_MOUSE
static SDL_Window *coordinate_window;
static unsigned int desktop_queries;
Uint32 __real_SDL_GetGlobalMouseState(int *x, int *y);
Uint32 __wrap_SDL_GetGlobalMouseState(int *x, int *y);
Uint32 __real_SDL_GetMouseState(int *x, int *y);
Uint32 __wrap_SDL_GetMouseState(int *x, int *y);
Uint32 __wrap_SDL_GetMouseState(int *x, int *y)
{
    if (!photo_mode)
        return __real_SDL_GetMouseState(x, y);
    if (x) *x = photo_x;
    if (y) *y = photo_y;
    return photo_buttons;
}
Uint32 __wrap_SDL_GetGlobalMouseState(int *x, int *y)
{
    if (controls_mode != CONTROLS_POINTER_POSITION)
        return __real_SDL_GetGlobalMouseState(x, y);
    desktop_queries++;
    if (x) *x = 950;
    if (y) *y = 1300;
    return 0;
}
#endif

static void apply_delivery(const paced_report *report);
static void pump_delivery(void);

static int stream_frames(void *unused)
{
    (void)unused;
    while (!atomic_load(&stream_stop)) {
        SDL_Delay(16);
        atomic_fetch_add(&frame_serial, 1); /* a repeated decoded image at 60 fps */
        frame_ready(frame_user);
    }
    return 0;
}

static int filter_first_frame(void *user, SDL_Event *event)
{
    (void)user;
    if (event->type >= SDL_USEREVENT && event->user.data1 == NULL && !filtered_frames) {
        filtered_frames++;
        return 0;
    }
    return 1;
}

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
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_QUIT;
    SDL_PushEvent(&event);
}

static void push_key(SDL_Scancode scancode, bool down, SDL_Keymod modifiers)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.windowID = 1;
    event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.key.keysym.scancode = scancode;
    event.key.keysym.mod = modifiers;
    SDL_PushEvent(&event);
}

static void push_motion(int x, int y)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_MOUSEMOTION;
    event.motion.windowID = 1; event.motion.x = x; event.motion.y = y;
    SDL_PushEvent(&event);
}

static void push_window(Uint8 action)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = 1; event.window.event = action;
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
    frame->width = controls_mode == CONTROLS_PRESENTATION ? presentation_width : PHONE_WIDTH;
    frame->height = controls_mode == CONTROLS_PRESENTATION ? presentation_height : PHONE_HEIGHT;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        frame_failure = true;
        quit();
        return;
    }
    for (int y = 0; y < frame->height; y++) {
        uint8_t *row = frame->data[0] + y * frame->linesize[0];
        for (int x = 0; x < frame->width; x++) {
            double radius = held_buttons & PH_HID_BTN_LEFT ? 9.5 : 11;
            double coverage = fmax(0, fmin(1, radius + .5 - hypot(x - phone_x, y - phone_y)));
            /* Full-width nonblack content distinguishes presentation from
             * portrait content letterboxed inside a TV-shaped frame. */
            int background = controls_mode == CONTROLS_PRESENTATION && frame->width > frame->height
                             ? 60 : 16;
            row[x] = (uint8_t)lround(background + (90 - background) * coverage);
            if (photo_mode && photo_mode != PHOTO_NATIVE_EXIT && photo_mode != PHOTO_FULL
                && photo_mode != PHOTO_CTRL
                && x >= 50 && x < 450 && y >= 150 && y < 1000)
                row[x] = 160; /* one distinct photo inside the phone screen */
        }
    }
    for (int plane = 1; plane <= 2; plane++)
        for (int y = 0; y < frame->height / 2; y++)
            memset(frame->data[plane] + y * frame->linesize[plane], 128, (size_t)frame->width / 2);
    av_frame_free(&latest);
    latest = frame;
    frame_serial++;
    frame_ready(frame_user);
}

void ph_window_show(ph_window *w) { (void)w; }
void ph_window_open_pairing(ph_window *w) { (void)w; }
void ph_window_open_received_history(ph_window *w) { (void)w; }
void ph_window_set_history_profile(ph_window *w, const char *profile)
{
    (void)w;
    (void)profile;
}
void ph_window_set_clipboard_available(ph_window *w, bool available)
{
    (void)w;
    (void)available;
}
void ph_window_note_received(ph_window *w, unsigned files)
{
    (void)w;
    (void)files;
}

void ph_window_refresh_folder(ph_window *w)
{
    (void)w;
}
ph_workspace *ph_window_workspace(ph_window *w)
{
    (void)w;
    return NULL;
}
void ph_window_set_companion(ph_window *w, bool connected, bool recording, int volume)
{
    (void)w;
    (void)connected;
    (void)recording;
    (void)volume;
}
void ph_window_set_incoming(ph_window *w, const char *name, uint64_t bytes, uint64_t total)
{
    (void)w;
    (void)name;
    (void)bytes;
    (void)total;
}
void ph_window_set_receiving(ph_window *w, int done, int total)
{
    (void)w;
    (void)done;
    (void)total;
}
ph_workspace_action ph_window_take_workspace_action(ph_window *w, uint64_t *id)
{
    (void)w;
    *id = 0;
    return PH_WORKSPACE_NONE;
}

ph_window *ph_window_create(const char *title)
{
    (void)title;
#ifdef PH_TEST_WRAP_GLOBAL_MOUSE
    if (controls_mode == CONTROLS_POINTER_POSITION) {
        coordinate_window = SDL_CreateWindow("coordinate regression", 100, 100,
                                             PHONE_WIDTH, PHONE_HEIGHT, SDL_WINDOW_HIDDEN);
        if (!coordinate_window) return NULL;
        SDL_WarpMouseInWindow(coordinate_window, 50, 50);
        SDL_SetWindowPosition(coordinate_window, 700, 600);
    }
#endif
    return &window;
}

void ph_window_set_title(ph_window *w, const char *title) { (void)w, (void)title; }
void ph_window_set_detail(ph_window *w, const char *detail)
{
    (void)w;
    if (!photo_mode) return;
    if (strstr(detail, "Asking the iPhone") || strstr(detail, "Open Porthole on the iPhone"))
        photo_requests++;
}
void ph_window_set_pull_hint(ph_window *w, const char *hint) { (void)w, (void)hint; }
void ph_window_set_share_available(ph_window *w, bool available) { (void)w; (void)available; }
void ph_window_set_pairing_status(ph_window *w, ph_origin_start_status status)
{
    (void)w;
    (void)status;
}
bool ph_window_take_new_code(ph_window *w) { (void)w; return false; }
ph_window_action ph_window_take_action(ph_window *w) { (void)w; return PH_WINDOW_ACTION_NONE; }
bool ph_window_ctrl_is_command(const ph_window *w) { (void)w; return true; }
bool ph_window_release_on_blur(const ph_window *w) { (void)w; return true; }
bool ph_window_next_relative(const ph_window *w) { (void)w; return false; }
void ph_window_set_prefs(ph_window *w, bool ctrl, bool blur, bool relative)
{ (void)w; (void)ctrl; (void)blur; (void)relative; }
void ph_window_note_transfer(ph_window *w, const char *line) { (void)w; (void)line; }
void ph_window_copy_pairing(ph_window *w) { (void)w; }
void ph_window_keep_aspect(ph_window *w) { (void)w; }
void ph_window_set_live_state(ph_window *w, bool feed, bool paused, bool sound_on)
{ (void)w; (void)feed; (void)paused; (void)sound_on; }
bool ph_window_media_keys(const ph_window *w) { (void)w; return true; }
void ph_window_set_media_keys(ph_window *w, bool on) { (void)w; (void)on; }
void ph_window_set_pc_name(ph_window *w, const char *name) { (void)w; (void)name; }
void ph_window_set_phone_name(ph_window *w, const char *name) { (void)w; (void)name; }
void ph_window_set_code_open(ph_window *w, bool open) { (void)w; (void)open; }
bool ph_window_has_pairing(const ph_window *w) { (void)w; return false; }
void ph_window_open_settings(ph_window *w) { (void)w; }
void ph_window_set_bluetooth_peer(ph_window *w, const char *name) { (void)w; (void)name; }
void ph_window_set_transfer_percent(ph_window *w, int percent) { (void)w; (void)percent; }
bool ph_window_set_active_transfer(ph_window *w, const char *line)
{ (void)w; (void)line; return false; }
void ph_window_show_transfers(ph_window *w) { (void)w; }
bool ph_window_take_cancel(ph_window *w) { (void)w; return false; }

void ph_decoder_limit_edge(ph_decoder *d, int long_edge) { (void)d, (void)long_edge; }
ph_drag_ghost *ph_drag_ghost_create(void) { return NULL; }
void ph_drag_ghost_destroy(ph_drag_ghost *g) { (void)g; }
bool ph_drag_ghost_show(ph_drag_ghost *g, const struct AVFrame *frame, ph_rect rect) { (void)g, (void)frame, (void)rect; return false; }
void ph_drag_ghost_caption(ph_drag_ghost *g, const char *text, bool aimed) { (void)g, (void)text, (void)aimed; }
void ph_drag_ghost_move(ph_drag_ghost *g, int root_x, int root_y) { (void)g, (void)root_x, (void)root_y; }
void ph_drag_ghost_hide(ph_drag_ghost *g) { (void)g; }
SDL_Window *ph_drag_ghost_window(const ph_drag_ghost *g) { (void)g; return NULL; }
bool ph_window_to_video(const ph_window *w, int x, int y, double *vx, double *vy);
bool ph_window_picture_point(const ph_window *w, int x, int y, double *vx, double *vy)
{
    return ph_window_to_video(w, x, y, vx, vy);
}

void ph_window_set_status(ph_window *w, bool mirror, bool ready, const char *detail)
{
    (void)w, (void)detail;
    mirroring = mirror;
    control_ready = ready;
}

void ph_window_toggle_help(ph_window *w) { (void)w; help_toggles++; }
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
void ph_window_toggle_diagnostics(ph_window *w) { (void)w; diagnostics_toggles++; }
void ph_window_toggle_statistics(ph_window *w) { ph_window_toggle_diagnostics(w); }
void ph_window_set_input_policy(ph_window *w, bool ready, ph_control_policy policy)
{
    (void)w;
    (void)ready;
    (void)policy;
}
void ph_window_set_metrics(ph_window *w, const ph_metrics_snapshot *m, unsigned int queued,
                           uint64_t discarded, uint64_t superseded)
{ (void)w; (void)m; (void)queued; (void)discarded; (void)superseded; }
void ph_window_redraw(ph_window *w) { (void)w; }
void ph_window_clear(ph_window *w) { (void)w; initial_frame = false; }
void ph_window_destroy(ph_window *w)
{
    (void)w;
#ifdef PH_TEST_WRAP_GLOBAL_MOUSE
    if (coordinate_window) SDL_DestroyWindow(coordinate_window);
#endif
}
unsigned int ph_window_id(const ph_window *w) { (void)w; return 1; }

void ph_window_present(ph_window *w, const AVFrame *frame, const ph_rect *screen)
{
    (void)w, (void)frame;
    if (controls_mode == CONTROLS_PRESENTATION) {
        presented_width = screen->w;
        presented_height = screen->h;
    }
    if (stream_mode) SDL_Delay(10); /* render work under a continuing 60 fps stream */
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
    *width = initial_frame ? (controls_mode == CONTROLS_PRESENTATION ? presented_width : PHONE_WIDTH) : 0;
    *height = initial_frame ? (controls_mode == CONTROLS_PRESENTATION ? presented_height : PHONE_HEIGHT) : 0;
}

bool ph_window_chrome_event(ph_window *w, const SDL_Event *event)
{
    (void)w;
    (void)event;
    return false;
}

bool ph_window_to_video(const ph_window *w, int x, int y, double *vx, double *vy)
{
    (void)w;
    int width, height;
    ph_window_video_size(w, &width, &height);
    if (!initial_frame || (!control_ready && controls_mode != CONTROLS_PRESENTATION)
        || x < 0 || x >= width || y < 0 || y >= height)
        return false;
    *vx = (x + .5) / width;
    *vy = (y + .5) / height;
    return true;
}

static void push_button(Uint8 button, bool down)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    event.button.windowID = 1;
    event.button.button = button;
    event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.button.x = TARGET_X;
    event.button.y = TARGET_Y;
    SDL_PushEvent(&event);
}

static void photo_button(bool down)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    photo_buttons = down ? SDL_BUTTON_LMASK : 0;
    event.button.windowID = 1;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.button.x = photo_x;
    event.button.y = photo_y;
    SDL_PushEvent(&event);
}

static void photo_motion(int x, int y)
{
    photo_x = x;
    photo_y = y;
    push_motion(x, y);
}

/* Ctrl at the press makes it a file drag; every other press is the phone's. */
static bool photo_ctrl(void)
{
    return photo_mode == PHOTO_CTRL || photo_mode == PHOTO_JITTER || photo_mode == PHOTO_ESCAPE
           || photo_mode == PHOTO_FOCUS || photo_mode == PHOTO_CTRL_TAP;
}

static void tick_photo(void)
{
    Uint64 now = SDL_GetTicks64();
    if (!mirroring || !control_ready || !initial_frame) return;
    if (!photo_stage) {
        injected = true;
        photo_motion(TARGET_X, TARGET_Y);
        SDL_SetModState(photo_ctrl() ? KMOD_LCTRL : KMOD_NONE);
        photo_button(true);
        photo_at = now;
        photo_stage = 1;
    } else if (photo_stage == 1 && now - photo_at >= 60) {
        if (photo_mode == PHOTO_SWIPE || photo_mode == PHOTO_JITTER || photo_mode == PHOTO_FAST_EXIT)
            photo_motion(TARGET_X + 12, TARGET_Y + 12); /* moves at once */
        if (photo_mode == PHOTO_NATIVE_EXIT) {
            photo_motion(400, 950);
        } else if (photo_mode == PHOTO_ESCAPE) {
            push_key(SDL_SCANCODE_ESCAPE, true, KMOD_LCTRL);
            push_key(SDL_SCANCODE_ESCAPE, false, KMOD_LCTRL);
        } else if (photo_mode == PHOTO_FOCUS) {
            push_window(SDL_WINDOWEVENT_FOCUS_LOST);
        }
        photo_stage = 2;
    } else if (photo_stage == 2 && now - photo_at >=
               (photo_mode == PHOTO_DRAG || photo_mode == PHOTO_CTRL ? 650U : 180U)) {
        if (photo_mode == PHOTO_NATIVE_EXIT)
            push_window(SDL_WINDOWEVENT_LEAVE);
        else if (photo_mode == PHOTO_DRAG)
            photo_motion(TARGET_X + 40, TARGET_Y);
        else if (photo_mode == PHOTO_SWIPE)
            photo_motion(TARGET_X + 80, TARGET_Y + 60);
        else if (photo_mode != PHOTO_TAP && photo_mode != PHOTO_CTRL_TAP)
            photo_motion(PHONE_WIDTH + 80, TARGET_Y + 100); /* out of the window */
        photo_button(false);
        SDL_SetModState(KMOD_NONE);
        photo_stage = 3;
        photo_at = now;
    } else if (photo_stage == 3 && now - photo_at >= 900) {
        photo_stage = 4;
        quit();
    }
}

static void push_wheel(float y)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_MOUSEWHEEL;
    event.wheel.windowID = 1;
    event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
#if SDL_VERSION_ATLEAST(2, 0, 18)
    event.wheel.preciseY = y;
#endif
#if SDL_VERSION_ATLEAST(2, 26, 0)
    event.wheel.mouseX = TARGET_X;
    event.wheel.mouseY = TARGET_Y;
#endif
    event.wheel.y = (int)y;
    SDL_PushEvent(&event);
}

static void push_presentation_inputs(void)
{
    push_motion(TARGET_X, TARGET_Y);
    push_button(SDL_BUTTON_LEFT, true);
    push_button(SDL_BUTTON_LEFT, false);
    push_wheel(1);
    push_key(SDL_SCANCODE_LALT, true, KMOD_LALT);
    push_key(SDL_SCANCODE_PAGEDOWN, true, KMOD_LALT);
    push_key(SDL_SCANCODE_PAGEDOWN, false, KMOD_LALT);
    push_key(SDL_SCANCODE_LALT, false, KMOD_NONE);
}

static void tick_absolute_presentation(void)
{
    Uint64 now = SDL_GetTicks64();
    if (!mirroring || !initial_frame) return;
    if (!controls_stage && control_ready && presented_width == PHONE_WIDTH
        && presented_height == PHONE_HEIGHT) {
        injected = true;
        presentation_blocked = true;
        presentation_width = 1920;
        presentation_height = 1080;
        controls_stage = 1;
        publish_frame();
    } else if (controls_stage == 1 && presented_width == 1920 && presented_height == 1080) {
        controls_bad |= !control_ready;
        push_motion(TARGET_X, TARGET_Y);
        push_button(SDL_BUTTON_LEFT, true);
        push_button(SDL_BUTTON_LEFT, false);
        controls_stage = 2;
        controls_at = now;
    } else if (controls_stage == 2 && now - controls_at >= 400) {
        controls_bad |= presentation_leaks != 0 || !control_ready || abs_presses < 1;
        controls_done = true;
        quit();
    }
}

static void tick_presentation(void)
{
    Uint64 now = SDL_GetTicks64();
    if (!mirroring || !initial_frame) return;
    if (!controls_stage && control_ready && presented_width == PHONE_WIDTH
        && presented_height == PHONE_HEIGHT) {
        injected = true;
        presentation_blocked = true;
        presentation_width = 1920;
        presentation_height = 1080;
        controls_stage = 1;
        publish_frame();
    } else if (controls_stage == 1 && presented_width == 1920 && presented_height == 1080) {
        push_presentation_inputs();
        controls_stage = 2;
        controls_at = now;
    } else if (controls_stage == 2 && now - controls_at >= 200) {
        controls_bad |= presentation_leaks != 0 || presses || releases || held_buttons;
        link_status(link_user, "subscribed"); /* status refresh must retain the geometry pause */
        controls_stage = 3;
        controls_at = now;
    } else if (controls_stage == 3 && now - controls_at >= 40) {
        push_presentation_inputs();
        controls_stage = 4;
        controls_at = now;
    } else if (controls_stage == 4 && now - controls_at >= 250) {
        controls_bad |= presentation_leaks != 0 || presses || releases || held_buttons;
        presentation_width = 1920;
        presentation_height = 888; /* native rotated shape, including sub-2px rounding */
        controls_stage = 5;
        publish_frame();
    } else if (controls_stage == 5 && presented_width == 1920 && presented_height == 888) {
        presentation_blocked = false;
        control_mouse_reports = 0;
        push_motion(TARGET_X, TARGET_Y);
        controls_stage = 6;
    } else if (controls_stage == 6 && control_mouse_reports) {
        landscape_accepted = true;
        push_window(SDL_WINDOWEVENT_FOCUS_LOST);
        controls_stage = 7;
        controls_at = now;
    } else if (controls_stage == 7 && now - controls_at >= 200 && !delivery_count && !delayed_frame) {
        presentation_width = PHONE_WIDTH;
        presentation_height = PHONE_HEIGHT;
        publish_frame();
        controls_stage = 8;
    } else if (controls_stage == 8 && control_ready && presented_width == PHONE_WIDTH
               && presented_height == PHONE_HEIGHT) {
        push_window(SDL_WINDOWEVENT_FOCUS_GAINED);
        push_window(SDL_WINDOWEVENT_ENTER);
        push_motion(TARGET_X, TARGET_Y);
        push_button(SDL_BUTTON_LEFT, true);
        push_button(SDL_BUTTON_LEFT, false);
        controls_stage = 9;
    } else if (controls_stage == 9 && releases == 1 && !delivery_count) {
        controls_at = now;
        controls_stage = 10;
    } else if (controls_stage == 10 && now - controls_at >= 300) {
        controls_bad |= presses != 1 || releases != 1 || held_buttons || keyboard_held;
        controls_done = true;
        quit();
    }
}

static void tick_controls(void)
{
    Uint64 now = SDL_GetTicks64();
    if (controls_mode == CONTROLS_PRESENTATION) {
        if (absolute_presentation) tick_absolute_presentation();
        else tick_presentation();
        return;
    }
    if (!mirroring || !control_ready || !initial_frame) return;
    if (!controls_stage) {
        injected = true;
        consumer_count = 0; /* exclude the initial neutral subscription report */
        control_mouse_reports = 0;
        controls_stage = 1;
        controls_at = now;
        if (controls_mode == CONTROLS_MEDIA) {
            /* AUDIOPLAY is outside the 256-entry keyboard usage table. */
            push_key(SDL_SCANCODE_AUDIOPLAY, true, KMOD_NONE);
            push_key(SDL_SCANCODE_AUDIOPLAY, false, KMOD_NONE);
            push_key(SDL_SCANCODE_VOLUMEUP, true, KMOD_NONE);
        } else if (controls_mode == CONTROLS_FUNCTIONS) {
            for (int i = 0; i < 4; i++) {
                push_key(SDL_SCANCODE_LCTRL, true, KMOD_LCTRL);
                push_key((SDL_Scancode)(SDL_SCANCODE_F1 + i), true, KMOD_LCTRL);
                /* Key ownership must survive releasing Ctrl first. */
                push_key(SDL_SCANCODE_LCTRL, false, KMOD_NONE);
                push_key((SDL_Scancode)(SDL_SCANCODE_F1 + i), false, KMOD_NONE);
            }
        } else if (controls_mode == CONTROLS_MOUSE) {
            push_button(SDL_BUTTON_MIDDLE, true);
        } else if (controls_mode == CONTROLS_WHEEL) {
            SDL_SetModState(KMOD_NONE);
            push_wheel(.25f);
        } else if (controls_mode == CONTROLS_POINTER_POSITION) {
#ifdef PH_TEST_WRAP_GLOBAL_MOUSE
            int x, y;
            SDL_GetMouseState(&x, &y);
            controls_bad |= x != 50 || y != 50;
#endif
            push_key(SDL_SCANCODE_LALT, true, KMOD_LALT);
            push_key(SDL_SCANCODE_D, true, KMOD_LALT);
            push_key(SDL_SCANCODE_D, false, KMOD_LALT);
            push_key(SDL_SCANCODE_LALT, false, KMOD_NONE);
        } else if (controls_mode == CONTROLS_FEED) {
            push_key(SDL_SCANCODE_F4, true, KMOD_NONE);
            push_key(SDL_SCANCODE_F4, false, KMOD_NONE);
            for (int i = 0; i < 12; i++) push_wheel(-1);
        }
        return;
    }
    if (controls_mode == CONTROLS_MEDIA) {
        if (controls_stage == 1) {
            controls_bad |= consumer_count != 3
                || consumer_reports[0] != PH_HID_CONSUMER_PLAY_PAUSE
                || consumer_reports[1] != 0
                || consumer_reports[2] != PH_HID_CONSUMER_VOLUME_UP;
            push_window(SDL_WINDOWEVENT_FOCUS_LOST);
            controls_stage = 2;
        } else {
            controls_bad |= consumer_count != 4 || consumer_reports[3] != 0;
            controls_done = true;
        }
    } else if (controls_mode == CONTROLS_FUNCTIONS) {
        /* Observe release before shutdown's release-all can repair a stuck key. */
        for (int i = 0; i < 4; i++)
            controls_bad |= function_down[i] != 1 || function_up[i] != 1 || function_held[i];
        controls_bad |= keyboard_held || keyboard_modifiers || help_toggles
                        || diagnostics_toggles || setup_seen;
        controls_done = true;
    } else if (controls_mode == CONTROLS_MOUSE) {
        if (controls_stage == 1 && held_buttons == PH_HID_BTN_MIDDLE) {
            push_button(SDL_BUTTON_X1, true);
            controls_stage = 2;
        } else if (controls_stage == 2 && held_buttons == (PH_HID_BTN_MIDDLE | PH_HID_BTN_4)) {
            push_button(SDL_BUTTON_MIDDLE, false);
            controls_stage = 3;
        } else if (controls_stage == 3 && held_buttons == PH_HID_BTN_4) {
            push_button(SDL_BUTTON_X1, false);
            controls_stage = 4;
        } else if (controls_stage == 4 && !held_buttons && !delivery_count) {
            controls_done = true;
        }
    } else if (controls_mode == CONTROLS_WHEEL) {
        if (controls_stage < 4 && now - controls_at >= 40) {
            controls_bad |= wheel_reports != 0 || pan_reports != 0;
            push_motion(TARGET_X + 1, TARGET_Y); /* sub-slop pointer jitter */
            push_wheel(.25f);
            controls_stage++;
            controls_at = now;
        } else if (controls_stage == 4 && wheel_reports && now - controls_at >= 350
                   && !delivery_count && !delayed_frame) {
            controls_bad |= pan_reports != 0 || wheel_sum >= 0;
            wheel_before_shift = wheel_reports;
            SDL_SetModState(KMOD_LSHIFT);
            push_wheel(1);
            controls_stage = 5;
            controls_at = now;
        } else if (controls_stage == 5 && pan_reports && now - controls_at >= 350
                   && !delivery_count && !delayed_frame) {
            controls_bad |= wheel_reports != wheel_before_shift || pan_sum <= 0;
            controls_done = true;
        }
    } else if (controls_mode == CONTROLS_POINTER_POSITION) {
        if (controls_stage == 1 && releases == 2 && !delivery_count) {
            controls_stage = 2;
            controls_at = now;
        } else if (controls_stage == 2 && now - controls_at >= 300) {
            controls_bad |= presses != 2 || releases != 2 || held_buttons || keyboard_held;
            controls_done = true;
        }
    } else if (controls_mode == CONTROLS_FEED) {
        if (controls_stage == 1 && releases && !delivery_count) {
            controls_stage = 2;
            controls_at = now;
        } else if (controls_stage == 2 && now - controls_at >= 800) {
            controls_done = true; /* a burst must not leave a second queued swipe */
        }
    }
    if (controls_done) quit();
}

void ph_window_tick(ph_window *w)
{
    if (connection_lifecycle) {
        (void)w;
        Uint64 now = SDL_GetTicks64();
        if (!first_tick)
            first_tick = lifecycle_at = now;
        if (now - first_tick > 4000) {
            lifecycle_bad = true;
            quit();
            return;
        }
        if (lifecycle_stage == 0) {
            lifecycle_bad |= link_starts != 0;
            if (now - lifecycle_at >= 200) {
                receiver_cb.connected(receiver_cb.user, "Lifecycle phone", "test");
                lifecycle_stage = 1;
                lifecycle_at = now;
            }
        } else if (lifecycle_stage == 1 && now - lifecycle_at >= 200) {
            lifecycle_bad |= link_starts != 1 || !control_ready;
            receiver_cb.disconnected(receiver_cb.user);
            lifecycle_stage = 2;
            lifecycle_at = now;
        } else if (lifecycle_stage == 2 && now - lifecycle_at >= 200) {
            lifecycle_bad |= stop_requests != 1 || link_starts != 1 || control_ready;
            receiver_cb.connected(receiver_cb.user, "Lifecycle phone", "test");
            lifecycle_stage = 3;
            lifecycle_at = now;
        } else if (lifecycle_stage == 3 && now - lifecycle_at >= 200) {
            lifecycle_bad |= link_starts != 2 || !control_ready;
            lifecycle_stage = 4;
            quit();
        }
        return;
    }
    (void)w;
    if (!first_tick)
        first_tick = SDL_GetTicks64();
    if (SDL_GetTicks64() - first_tick > (paced_mode ? 11000U : 4000U)) {
        timed_out = true;
        quit();
    }
    if (photo_mode) { tick_photo(); return; }
    if (controls_mode) { tick_controls(); return; }
    if (probe_pending) {
        Uint64 elapsed = SDL_GetTicks64() - confirmation_ack_at;
        if (changing_mode && probe_stage == 0 && elapsed >= 20) {
            double saved_x = phone_x;
            phone_x += 6; /* a plausible but still moving cursor */
            publish_frame();
            phone_x = saved_x;
            probe_stage = 1;
        } else if ((changing_mode && elapsed >= 40) || (stale_mode && elapsed >= 180)) {
            publish_frame();
            probe_stage = 2;
            probe_fresh_at = SDL_GetTicks64();
            probe_pending = false;
        }
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
            SDL_Event focus;
            SDL_zero(focus);
            focus.type = SDL_WINDOWEVENT;
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
    if (reentry_mode && mirroring && control_ready && initial_frame) {
        if (reentry_stage == 0) {
            injected = true;
            push_motion(TARGET_X, TARGET_Y); /* establish a warm confirmed model */
            reentry_stage = 1;
        } else if (reentry_stage == 1 && !delivery_count && !delayed_frame && last_move_ack_at
                   && SDL_GetTicks64() - last_move_ack_at >= 180
                   && hypot(phone_x - (TARGET_X + .5), phone_y - (TARGET_Y + .5)) <= 5) {
            push_motion(400, 250); /* cancellation must discard this old intent */
            interrupted_at = SDL_GetTicks64();
            reentry_stage = 2;
        } else if (reentry_stage == 2 &&
                   (cancel_after_ack ? last_move_ack_at > interrupted_at && delayed_frame
                                     : interrupted_reports && delivery_count)) {
            canceled_at = SDL_GetTicks64();
            push_window(reentry_focus ? SDL_WINDOWEVENT_FOCUS_LOST : SDL_WINDOWEVENT_LEAVE);
            reentry_stage = 3;
        } else if (reentry_stage == 3) {
            /* The canceled endpoint is not yet visible. A new target may wait
             * for that physical endpoint, but must not finish the old intent. */
            push_window(reentry_focus ? SDL_WINDOWEVENT_FOCUS_GAINED : SDL_WINDOWEVENT_ENTER);
            push_motion(REENTER_X, REENTER_Y);
            SDL_Event button;
            SDL_zero(button);
            button.type = SDL_MOUSEBUTTONDOWN;
            button.button.windowID = 1; button.button.button = SDL_BUTTON_LEFT;
            button.button.state = SDL_PRESSED;
            button.button.x = REENTER_X; button.button.y = REENTER_Y;
            SDL_PushEvent(&button);
            button.type = SDL_MOUSEBUTTONUP; button.button.state = SDL_RELEASED;
            SDL_PushEvent(&button);
            reentry_stage = 4;
        } else if (reentry_stage == 4 && releases && !delivery_count && !delayed_frame) {
            quit();
        }
        return;
    }
    if ((cancel_mode || freeze_mode) && presses && !fault_stage) {
        fault_stage = 1;
        if (cancel_mode) push_key(SDL_SCANCODE_B, true, KMOD_NONE);
        SDL_Event motion;
        SDL_zero(motion);
        motion.type = SDL_MOUSEMOTION;
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
                SDL_Event focus;
                SDL_zero(focus);
                focus.type = SDL_WINDOWEVENT;
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
            SDL_Event motion;
            SDL_zero(motion);
            motion.type = SDL_MOUSEMOTION;
            motion.motion.windowID = 1;
            motion.motion.x = TARGET_X;
            motion.motion.y = TARGET_Y;
            SDL_PushEvent(&motion);
            SDL_Event wheel;
            SDL_zero(wheel);
            wheel.type = SDL_MOUSEWHEEL;
            wheel.wheel.windowID = 1;
            wheel.wheel.y = -1;
            SDL_PushEvent(&wheel);
            SDL_Event button;
            SDL_zero(button);
            button.type = SDL_MOUSEBUTTONDOWN;
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
        SDL_Event motion;
        SDL_zero(motion);
        motion.type = SDL_MOUSEMOTION;
        motion.motion.windowID = 1;
        motion.motion.x = TARGET_X;
        motion.motion.y = TARGET_Y;
        SDL_PushEvent(&motion);
        SDL_Event button;
        SDL_zero(button);
        button.type = SDL_MOUSEBUTTONDOWN;
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

void ph_decoder_stop(ph_decoder *d)
{
    (void)d;
    atomic_store(&stream_stop, true);
    if (stream_thread) SDL_WaitThread(stream_thread, NULL);
    av_frame_free(&latest);
}

ph_receiver *ph_receiver_start(const ph_receiver_options *opt, const ph_receiver_callbacks *cb)
{
    (void)opt;
    receiver_cb = *cb;
    if (connection_lifecycle)
        return &receiver;
    if (filtered_mode) SDL_SetEventFilter(filter_app_events, NULL);
    if (filter_one_frame) SDL_SetEventFilter(filter_first_frame, NULL);
    cb->connected(cb->user, "Idle-frame phone", "test");
    const uint8_t idr[] = {0, 0, 0, 1, 0x65, 0x80};
    cb->video(cb->user, idr, sizeof idr, 0);
    publish_frame();
    /* A frame before this connect is cleared, and the next one cancels the pointer. */
    if (stream_mode && !stream_thread)
        stream_thread = SDL_CreateThread(stream_frames, "test-video-60fps", NULL);
    return &receiver;
}
void ph_receiver_disconnect(ph_receiver *r)
{
    (void)r;
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
    lifecycle_exit = false;
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
    if (connection_lifecycle && l && l->stopping && !lifecycle_exit) {
        lifecycle_exit = true;
        link_status(link_user, "exited");
    }
    return !l || l->stopping;
}
bool ph_hid_link_keyboard(ph_hid_link *l, const ph_hid_keyboard *k)
{
    (void)l;
    if (paced_mode) return enqueue((paced_report){.kind = PACED_KEY, .key = *k});
    if (controls_mode == CONTROLS_FUNCTIONS) {
        keyboard_modifiers = k->modifiers;
        for (int i = 0; i < 4; i++) {
            bool held = false;
            for (size_t j = 0; j < sizeof k->keys; j++)
                held |= k->keys[j] == (uint8_t)(0x3a + i);
            if (held && !function_held[i]) function_down[i]++;
            if (!held && function_held[i]) function_up[i]++;
            function_held[i] = held;
        }
    }
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
/* Absolute reports: these app tests run the legacy relative pointer, so any
 * absolute report here is a routing mistake. */
static unsigned int unexpected_abs_reports;
bool ph_hid_link_abs_mouse(ph_hid_link *l, uint8_t buttons, double x, double y)
{
    (void)l;
    (void)x;
    (void)y;
    if (photo_mode) {
        if ((buttons & 1) && !(photo_phone_buttons & 1)) photo_presses++;
        if (!(buttons & 1) && (photo_phone_buttons & 1)) {
            photo_releases++;
            photo_release_x = x;
            photo_release_y = y;
        }
        photo_phone_buttons = buttons;
        return true;
    }
    if (absolute_presentation) {
        abs_reports++;
        if (buttons & 1)
            abs_presses++;
        return true;
    }
    (void)buttons;
    unexpected_abs_reports++;
    return true;
}

bool ph_hid_link_consumer(ph_hid_link *l, uint16_t usage)
{
    (void)l;
    if (controls_mode == CONTROLS_MEDIA) {
        if (consumer_count < sizeof consumer_reports / sizeof consumer_reports[0])
            consumer_reports[consumer_count++] = usage;
        else controls_bad = true;
    }
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
    if (controls_mode == CONTROLS_PRESENTATION && presentation_blocked
        && (m->buttons || m->dx || m->dy || m->wheel || m->pan))
        presentation_leaks++; /* observe transport acceptance, before paced delivery */
    if (paced_mode) return enqueue((paced_report){.kind = PACED_MOUSE, .mouse = *m});
    if (controls_mode) {
        if (m->buttons || m->dx || m->dy || m->wheel || m->pan)
            control_mouse_reports++;
        if (!(held_buttons & PH_HID_BTN_MIDDLE) && (m->buttons & PH_HID_BTN_MIDDLE)) middle_down++;
        if ((held_buttons & PH_HID_BTN_MIDDLE) && !(m->buttons & PH_HID_BTN_MIDDLE)) middle_up++;
        if (!(held_buttons & PH_HID_BTN_4) && (m->buttons & PH_HID_BTN_4)) extra_down++;
        if ((held_buttons & PH_HID_BTN_4) && !(m->buttons & PH_HID_BTN_4)) extra_up++;
        if (m->wheel) { wheel_reports++; wheel_sum += m->wheel; }
        if (m->pan) { pan_reports++; pan_sum += m->pan; }
    }
    if (feedback_waiting && (m->dx || m->dy || m->buttons)) {
        Uint64 now = SDL_GetTicks64(), elapsed = now - confirmation_ack_at;
        if (confirmation_count < sizeof confirmation_waits / sizeof confirmation_waits[0])
            confirmation_waits[confirmation_count++] = elapsed;
        else feedback_bad = true;
        if (elapsed < 32 || probe_stage != 2 || now - probe_fresh_at < 32)
            feedback_bad = true;
        feedback_waiting = false;
    }
    if (setup_visible && (m->buttons || m->dx || m->dy || m->wheel || m->pan))
        modal_leaks++;
    if (m->dx || m->dy) {
        moved_since_frame = true;
        if (reentry_mode && reentry_stage == 2 && m->dx != -127 && m->dy != -127)
            interrupted_reports++;
        phone_x = fmin(PHONE_WIDTH - 16, fmax(16, phone_x + travel(m->dx)));
        phone_y = fmin(PHONE_HEIGHT - 16, fmax(16, phone_y + travel(m->dy)));
        if (m->buttons & PH_HID_BTN_LEFT)
            dragged_reports++;
    }
    if (m->dx == -127 && m->dy == -127 && m->buttons)
        anchored_with_button = true;
    if (m->dx == -127 && m->dy == -127) anchor_reports++;
    if (!(held_buttons & PH_HID_BTN_LEFT) && (m->buttons & PH_HID_BTN_LEFT)) {
        presses++;
        if (controls_mode == CONTROLS_POINTER_POSITION)
            controls_bad |= hypot(phone_x - (TARGET_X + .5), phone_y - (TARGET_Y + .5)) > 5;
        pressed_x = phone_x;
        pressed_y = phone_y;
    }
    if ((held_buttons & PH_HID_BTN_LEFT) && !(m->buttons & PH_HID_BTN_LEFT)) {
        releases++;
        released_x = phone_x;
        released_y = phone_y;
        release_waiting_sync = true;
        if (!delivery_applying && !controls_mode) quit();
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
        bool retain_stale = stale_mode && movement_frames != 0;
        if (!retain_stale) publish_frame();
        movement_frames++;
        moved_since_frame = false;
        movement_acks++;
        if (confirmation_mode || changing_mode || stale_mode) {
            feedback_waiting = true;
            confirmation_ack_at = SDL_GetTicks64();
            probe_fresh_at = confirmation_ack_at;
            probe_pending = changing_mode || retain_stale;
            probe_stage = probe_pending ? 0 : 2;
        }
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
            last_move_ack_at = SDL_GetTicks64();
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
    connection_lifecycle = argc == 2 && !strcmp(argv[1], "--connection-lifecycle");
    if (argc > 2)
        return 1;
    if (argc == 2) {
        if (!strcmp(argv[1], "--photo-drag")) photo_mode = PHOTO_DRAG;
        else if (!strcmp(argv[1], "--photo-jitter")) photo_mode = PHOTO_JITTER;
        else if (!strcmp(argv[1], "--photo-swipe")) photo_mode = PHOTO_SWIPE;
        else if (!strcmp(argv[1], "--photo-tap")) photo_mode = PHOTO_TAP;
        else if (!strcmp(argv[1], "--photo-escape")) photo_mode = PHOTO_ESCAPE;
        else if (!strcmp(argv[1], "--photo-focus")) photo_mode = PHOTO_FOCUS;
        else if (!strcmp(argv[1], "--photo-fast-exit")) photo_mode = PHOTO_FAST_EXIT;
        else if (!strcmp(argv[1], "--photo-native-exit")) photo_mode = PHOTO_NATIVE_EXIT;
        else if (!strcmp(argv[1], "--photo-full")) photo_mode = PHOTO_FULL;
        else if (!strcmp(argv[1], "--photo-ctrl")) photo_mode = PHOTO_CTRL;
        else if (!strcmp(argv[1], "--photo-ctrl-tap")) photo_mode = PHOTO_CTRL_TAP;
        if (!strcmp(argv[1], "--controls-media")) controls_mode = CONTROLS_MEDIA;
        else if (!strcmp(argv[1], "--controls-functions")) controls_mode = CONTROLS_FUNCTIONS;
        else if (!strcmp(argv[1], "--controls-mouse")) controls_mode = CONTROLS_MOUSE;
        else if (!strcmp(argv[1], "--controls-wheel")) controls_mode = CONTROLS_WHEEL;
        else if (!strcmp(argv[1], "--controls-feed")) controls_mode = CONTROLS_FEED;
        else if (!strcmp(argv[1], "--controls-presentation")) controls_mode = CONTROLS_PRESENTATION;
        else if (!strcmp(argv[1], "--controls-presentation-absolute")) {
            controls_mode = CONTROLS_PRESENTATION;
            absolute_presentation = true;
        }
#ifdef PH_TEST_WRAP_GLOBAL_MOUSE
        else if (!strcmp(argv[1], "--controls-pointer-position")) controls_mode = CONTROLS_POINTER_POSITION;
#endif
    }
    drag_mode = argc == 2 && (!strcmp(argv[1], "--drag") || !strcmp(argv[1], "--paced-drag"));
    modal_mode = argc == 2 && !strcmp(argv[1], "--modal");
    paced_mode = argc == 2 && (!strcmp(argv[1], "--paced") || !strcmp(argv[1], "--paced-after-ack")
        || !strcmp(argv[1], "--paced-drag")
        || !strcmp(argv[1], "--render-stall")
        || !strcmp(argv[1], "--freeze-drag") || !strcmp(argv[1], "--disconnect-drag")
        || !strcmp(argv[1], "--focus-drag") || !strcmp(argv[1], "--close-drag")
        || !strcmp(argv[1], "--modal-drag") || !strcmp(argv[1], "--calibration-focus")
        || !strcmp(argv[1], "--calibration-escape")
        || !strcmp(argv[1], "--cancel-move-leave") || !strcmp(argv[1], "--cancel-move-focus")
        || !strcmp(argv[1], "--cancel-video-leave") || !strcmp(argv[1], "--cancel-video-focus"));
    if (controls_mode == CONTROLS_MOUSE || controls_mode == CONTROLS_WHEEL
        || controls_mode == CONTROLS_FEED || controls_mode == CONTROLS_POINTER_POSITION
        || controls_mode == CONTROLS_PRESENTATION) paced_mode = true;
    reentry_mode = argc == 2 && (!strcmp(argv[1], "--cancel-move-leave")
        || !strcmp(argv[1], "--cancel-move-focus") || !strcmp(argv[1], "--cancel-video-leave")
        || !strcmp(argv[1], "--cancel-video-focus"));
    reentry_focus = argc == 2 && (!strcmp(argv[1], "--cancel-move-focus")
        || !strcmp(argv[1], "--cancel-video-focus"));
    cancel_after_ack = argc == 2 && (!strcmp(argv[1], "--cancel-video-leave")
        || !strcmp(argv[1], "--cancel-video-focus"));
    filtered_mode = argc == 2 && !strcmp(argv[1], "--filtered-events");
    confirmation_mode = argc == 2 && !strcmp(argv[1], "--confirmation-latency");
    changing_mode = argc == 2 && !strcmp(argv[1], "--changing-confirmation");
    stale_mode = argc == 2 && !strcmp(argv[1], "--stale-confirmation");
    stream_mode = argc == 2 && (!strcmp(argv[1], "--stream-confirmation") || !strcmp(argv[1], "--filtered-frame"));
    filter_one_frame = argc == 2 && !strcmp(argv[1], "--filtered-frame");
    render_stall_mode = argc == 2 && !strcmp(argv[1], "--render-stall");
    calibration_mode = argc == 2 && (!strcmp(argv[1], "--calibration-focus")
        || !strcmp(argv[1], "--calibration-escape"));
    if (argc == 2 && !drag_mode && !modal_mode && !paced_mode && !filtered_mode &&
        !confirmation_mode && !changing_mode && !stale_mode && !stream_mode && !controls_mode &&
        !photo_mode && !connection_lifecycle)
        return 1;
    freeze_mode = argc == 2 && !strcmp(argv[1], "--freeze-drag");
    cancel_mode = argc == 2 && (!strcmp(argv[1], "--disconnect-drag")
        || !strcmp(argv[1], "--focus-drag") || !strcmp(argv[1], "--close-drag")
        || !strcmp(argv[1], "--modal-drag"));
    fault_name = argc == 2 && !strcmp(argv[1], "--disconnect-drag") ? "disconnect"
        : argc == 2 && !strcmp(argv[1], "--close-drag") ? "close"
        : argc == 2 && !strcmp(argv[1], "--modal-drag") ? "modal"
        : calibration_mode ? argv[1] + 2 : "focus";
    after_ack = reentry_mode || (argc == 2 && (!strcmp(argv[1], "--paced-after-ack")
        || !strcmp(argv[1], "--paced-drag")));
    frame_delay_ms = reentry_mode ? 120 : 80;
    char config[1024];
    if (!ph_test_temp_dir(config, sizeof config))
        return 1;
#ifdef _WIN32
    SDL_setenv("APPDATA", config, 1);
#else
    SDL_setenv("XDG_CONFIG_HOME", config, 1);
#endif
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    ph_app_options options = {.name = "Idle-frame test", .bluetooth = true,
                              .ble_helper = "unused fake helper",
                              .absolute_pointer = absolute_presentation || photo_mode};
    int result = ph_app_run(&options);
    if (unexpected_abs_reports) {
        fprintf(stderr, "relative-mode run sent %u absolute reports\n", unexpected_abs_reports);
        return 1;
    }
    char saved_calibration[sizeof config + 32];
    snprintf(saved_calibration, sizeof saved_calibration, "%s/porthole/pointer-scale", config);
    bool calibration_written = access(saved_calibration, F_OK) == 0;
    unlink(saved_calibration);
    char app_config[sizeof config + 16];
    snprintf(app_config, sizeof app_config, "%s/porthole", config);
    rmdir(app_config);
    rmdir(config);
    if (connection_lifecycle) {
        bool passed = !result && !lifecycle_bad && lifecycle_stage == 4 && link_starts == 2 &&
                      stop_requests == 1;
        fprintf(passed ? stdout : stderr,
                "control lifecycle: pass=%d stage=%u starts=%u stops=%u\n", passed, lifecycle_stage,
                link_starts, stop_requests);
        return passed ? 0 : 1;
    }
    if (photo_mode) {
        /* Without Ctrl the phone gets every press, even one dragged out of the
         * window, and no file is asked for. With Ctrl the phone gets nothing. */
        bool is_gesture = !photo_ctrl();
        /* The original is requested once a Ctrl drag crosses the threshold.
         * A stationary press, Escape, or loss of focus may cancel before it. */
        bool is_file = photo_mode == PHOTO_CTRL || photo_mode == PHOTO_JITTER;
        bool passed = !result && !timed_out && !frame_failure && injected && photo_stage == 4
            && !photo_phone_buttons && photo_presses == (is_gesture ? 1U : 0U)
            && photo_releases == photo_presses && (photo_requests > 0) == is_file;
        if (photo_mode == PHOTO_NATIVE_EXIT)
            passed &= fabs(photo_release_x - (TARGET_X + .5) / PHONE_WIDTH) < .001
                      && fabs(photo_release_y - (TARGET_Y + .5) / PHONE_HEIGHT) < .001;
        fprintf(passed ? stdout : stderr,
                "photo gesture: pass=%d presses=%u releases=%u requests=%u case=%d\n",
                passed, photo_presses, photo_releases, photo_requests, photo_mode);
        return passed ? 0 : 1;
    }
    if (controls_mode) {
        SDL_SetModState(KMOD_NONE);
        bool passed = !result && !timed_out && !frame_failure && !queue_overflow
            && injected && controls_done && !controls_bad && !held_buttons
            && !keyboard_held && !delivery_count && link_starts == 1 && !stop_requests;
        if (controls_mode == CONTROLS_FUNCTIONS) {
            for (int i = 0; i < 4; i++)
                passed &= function_down[i] == 1 && function_up[i] == 1 && !function_held[i];
            passed &= !keyboard_modifiers && !help_toggles && !diagnostics_toggles && !setup_seen;
        } else if (controls_mode == CONTROLS_MOUSE) {
            passed &= middle_down == 1 && middle_up == 1 && extra_down == 1 && extra_up == 1
                && !presses && !releases;
        } else if (controls_mode == CONTROLS_WHEEL) {
            passed &= wheel_reports > 0 && pan_reports > 0 && !presses && !releases;
        } else if (controls_mode == CONTROLS_PRESENTATION && absolute_presentation) {
            passed &= abs_presses >= 1 && abs_reports >= 1 && !presentation_leaks && control_ready;
            fprintf(passed ? stdout : stderr,
                    "presentation absolute: pass=%d reports=%u presses=%u leaks=%u ready=%d\n",
                    passed, abs_reports, abs_presses, presentation_leaks, control_ready);
        } else if (controls_mode == CONTROLS_PRESENTATION) {
            passed &= landscape_accepted && !presentation_leaks && presses == 1 && releases == 1
                && release_synced && !wheel_reports && !pan_reports
                && hypot(pressed_x - (TARGET_X + .5), pressed_y - (TARGET_Y + .5)) <= 5;
            fprintf(passed ? stdout : stderr,
                    "presentation geometry: pass=%d external_leaks=%u native_landscape=%d "
                    "final_crop=%dx%d click=(%.1f,%.1f)\n",
                    passed, presentation_leaks, landscape_accepted,
                    presented_width, presented_height, pressed_x, pressed_y);
        } else if (controls_mode == CONTROLS_POINTER_POSITION) {
            passed &= presses == 2 && releases == 2 && release_synced && !wheel_reports && !pan_reports;
#ifdef PH_TEST_WRAP_GLOBAL_MOUSE
            passed &= desktop_queries > 0;
#endif
        } else if (controls_mode == CONTROLS_FEED) {
            passed &= presses == 1 && releases == 1 && dragged_reports && release_synced
                && !wheel_reports && !pan_reports && pressed_y - released_y > PHONE_HEIGHT * .5
                && fabs(pressed_x - released_x) <= 5;
        }
        fprintf(passed ? stdout : stderr,
                "SDL controls %s: pass=%d stage=%u timeout=%d bad=%d consumer=%u "
                "F1..4_down=%u/%u/%u/%u up=%u/%u/%u/%u middle=%u/%u extra=%u/%u "
                "wheel=%u(%d) pan=%u(%d) presses=%u releases=%u drag_dy=%.1f queued=%zu\n",
                argv[1], passed, controls_stage, timed_out, controls_bad, consumer_count,
                function_down[0], function_down[1], function_down[2], function_down[3],
                function_up[0], function_up[1], function_up[2], function_up[3],
                middle_down, middle_up, extra_down, extra_up, wheel_reports, wheel_sum,
                pan_reports, pan_sum, presses, releases, pressed_y - released_y, delivery_count);
        return passed ? 0 : 1;
    }
    if (reentry_mode) {
        double error = hypot(pressed_x - (REENTER_X + .5), pressed_y - (REENTER_Y + .5));
        bool passed = !result && !timed_out && !frame_failure && !queue_overflow
            && reentry_stage == 4 && interrupted_reports && canceled_at && presses == 1
            && releases == 1 && !held_buttons && !keyboard_held && !delivery_count
            && !anchored_with_button && release_synced && !stop_requests && link_starts == 1
            && anchor_reports == 8 && error <= 5;
        fprintf(passed ? stdout : stderr,
                "canceled unpressed motion %s: pass=%d stage=%u anchors=%u interrupted_reports=%u "
                "presses=%u releases=%u held=%u error=%.2f timeout=%d helper_stops=%u\n",
                argv[1], passed, reentry_stage, anchor_reports, interrupted_reports,
                presses, releases, held_buttons, error, timed_out, stop_requests);
        return passed ? 0 : 1;
    }
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
        unsigned expected_stops = cancel_mode && !strcmp(fault_name, "disconnect") ? 1U : 0U;
        double start_error = hypot(pressed_x - (TARGET_X + .5), pressed_y - (TARGET_Y + .5));
        double end_error = hypot(released_x - (drag_mode ? DRAG_END_X + .5 : TARGET_X + .5),
                                 released_y - (drag_mode ? DRAG_END_Y + .5 : TARGET_Y + .5));
        bool released = releases == 1 && !held_buttons && !keyboard_held && !delivery_count;
        bool passed = !result && !timed_out && !frame_failure && !queue_overflow &&
                      link_starts == 1 && stop_requests == expected_stops && presses == 1 &&
                      released && release_synced && start_error <= 5;
        if (!faults) passed &= end_error <= 5 && (!drag_mode || dragged_reports);
        if (render_stall_mode) passed &= render_stalled;
        if (faults) passed &= fault_stage == 2 && (freeze_mode || fault_queued)
            /* Frozen video may wait one bounded 250 ms extension past the
             * fixed 700 ms held confirmation before releasing the button. */
            && released_at >= canceled_at && released_at - canceled_at <= (freeze_mode ? 1250 : 1000);
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
    if (confirmation_mode || changing_mode || stale_mode) {
        Uint64 minimum = UINT64_MAX, maximum = 0, sum = 0;
        for (unsigned int i = 0; i < confirmation_count; i++) {
            Uint64 wait = confirmation_waits[i];
            if (wait < minimum) minimum = wait;
            if (wait > maximum) maximum = wait;
            sum += wait;
        }
        double mean = confirmation_count ? (double)sum / confirmation_count : 0;
        bool passed = confirmation_count >= 2 && !feedback_bad && minimum >= 32;
        /* The corner detector is cheap; a later full changed-frame search can
         * cost more under ASAN. Isolate the minimum ACK/stability interval. */
        if (confirmation_mode) passed &= minimum <= 56;
        fprintf(passed ? stdout : stderr,
                "confirmation %s: pass=%d samples=%u ACK_to_progress_ms=%llu..%llu mean=%.1f safety_violation=%d\n",
                argv[1], passed, confirmation_count, (unsigned long long)minimum,
                (unsigned long long)maximum, mean, feedback_bad);
        if (!passed) return 1;
    }
    double error = hypot(pressed_x - (TARGET_X + .5), pressed_y - (TARGET_Y + .5));
    double end_x = drag_mode ? DRAG_END_X + .5 : TARGET_X + .5;
    double end_y = drag_mode ? DRAG_END_Y + .5 : TARGET_Y + .5;
    double release_error = hypot(released_x - end_x, released_y - end_y);
    if (stream_mode && (!stream_thread || (filter_one_frame && filtered_frames != 1))) return 1;
    if (result || timed_out || frame_failure || !injected || presses != 1 || releases != 1
        || held_buttons || anchored_with_button || error > 5 || release_error > 5
        || !release_synced || (drag_mode && !dragged_reports)
        || movement_frames < (drag_mode ? 3U : 2U)
        || movement_frames != movement_acks || ((!changing_mode && !stale_mode) && frames_after_last_ack)
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
