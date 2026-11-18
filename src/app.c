#include "app.h"

#include <signal.h>
#include <stdatomic.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <libavutil/frame.h>

#include "airplay/receiver.h"
#include "audio/player.h"
#include "control/input.h"
#include "control/gesture.h"
#include "control/mouse_input.h"
#include "control/abs_pointer.h"
#include "control/pointer.h"
#include "control/scroll.h"
#include "control/setup.h"
#include "core/config.h"
#include "core/log.h"
#include "core/metrics.h"
#include "core/version.h"
#include "hid/link.h"
#include "polish/runtime.h"
#include "ui/window.h"
#include "usb/capture.h"
#include "usb/packet.h"
#include "usb/sample.h"
#include "usb/switch.h"
#include "video/codecs.h"
#include "video/crop.h"
#include "video/cursor.h"
#include "video/decoder.h"
#include "video/scrollbar.h"

#define BLE_RETRY_MS 5000

static volatile sig_atomic_t stop_requested;

static void request_stop(int sig)
{
    (void)sig;
    stop_requested = 1;
}

enum { EV_FRAME, EV_CONNECTED, EV_DISCONNECTED, EV_BLE, EV_COUNT };

typedef struct app {
    const ph_app_options *opt;
    ph_window *win;
    ph_decoder *dec;
    ph_receiver *rx;
    ph_audio *audio;
    ph_polish *polish;
    ph_hid_link *link;
    bool link_stopping;
    Uint32 ev_base;
    atomic_bool frame_pending; /* coalesces "new frame" events */
    double portrait_aspect;    /* phone screen width/height when upright; 0 until seen */
    ph_video_crop_state crop;  /* confirmed crop through ambiguous dark frames */
    bool presentation_mode;   /* displayed shape differs from the known phone UI */

    char phone_name[128];      /* from the mirroring session; empty when not mirroring */
    char ble_status[64];       /* last state reported by the Bluetooth helper */
    ph_input input;
    ph_pointer pointer;
    ph_abs abs; /* used instead of `pointer` in absolute mode */
    bool absolute;
    SDL_Scancode abs_panel; /* Alt key whose panel is open (toggle closes it), or 0 */
    Uint64 abs_panel_at;
    ph_scroll wheel;
    ph_mouse_input mouse_input;
    bool wheel_point_valid;
    double wheel_point_x, wheel_point_y, wheel_point_aspect;
    ph_gesture gesture;
    bool feed_mode, feed_have_wheel;
    bool gesture_keys[256];
    Uint64 feed_wheel_at;
    AVFrame *view_frame, *pointer_baseline, *pointer_reference, *pointer_after;
    ph_rect view_screen, baseline_screen, reference_screen;
    Uint64 reference_at;
    int picture_w, picture_h;
    Uint64 view_seen_at, candidate_since;
    uint64_t view_serial, checked_serial, fallback_serial, after_serial;
    uint32_t after_sync;
    ph_rect after_screen;
    bool content_moving; /* latest checked picture differs widely from the baseline */
    double seen_target_x, seen_target_y;
    Uint64 target_moved_at; /* last change of the PC-requested pointer position */
    uint32_t fallback_sync;
    uint32_t checked_sync, candidate_sync;
    bool checked_found;
    double checked_x, checked_y, candidate_x, candidate_y;
    Uint64 ble_retry_at;
    ph_setup setup;
    bool calibration_saved;
    double previous_scale_x, previous_scale_y;
    ph_metrics metrics;
    ph_frame_timing frame_timing;
    bool input_pending;
    uint64_t input_at;
    Uint64 metrics_at, metrics_log_at;
    ph_src_switch src;
    pthread_mutex_t src_lock;
    pthread_t usb_thread;
    atomic_bool usb_stop;
    bool usb_started;
} app;

/* ------------------------------------------------------------ cross-thread notifications */

static bool push_event(app *a, int which, void *data)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = a->ev_base + (Uint32)which;
    e.user.data1 = data;
    if (SDL_PushEvent(&e) != 1) {
        free(data);
        return false;
    }
    return true;
}

static void discard_pending_events(app *a)
{
    if (a->ev_base == (Uint32)-1)
        return;
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, a->ev_base,
                          a->ev_base + EV_COUNT - 1) > 0)
        free(e.user.data1);
}

static void on_frame(void *user)
{
    app *a = user;
    if (!atomic_exchange(&a->frame_pending, true) && !push_event(a, EV_FRAME, NULL))
        atomic_store(&a->frame_pending, false); /* a rejected frame may be retried */
}

static void on_video(void *user, const uint8_t *data, size_t len, uint64_t timestamp_ns)
{
    app *a = user;
    bool show;

    pthread_mutex_lock(&a->src_lock);
    show = ph_src_switch_wifi_frame(&a->src, ph_usb_annexb_keyframe(data, len),
                                    ph_metrics_now_ms()) == PH_SRC_WIFI;
    pthread_mutex_unlock(&a->src_lock);
    if (!show)
        return;
    if (a->audio)
        ph_audio_note_video_pts(a->audio, timestamp_ns);
    if (a->polish && !ph_polish_accepts_video(a->polish))
        return;
    if (a->polish)
        ph_polish_on_video(a->polish, data, len, timestamp_ns);
    ph_decoder_push(a->dec, data, len);
}

static bool on_admit(void *user, const char *device_id, const char *name, const char *model)
{
    app *a = user;
    if (!a->polish)
        return true;
    return ph_polish_admit(a->polish, device_id, name, model);
}

static void on_connected(void *user, const char *name, const char *model)
{
    (void)model;
    push_event(user, EV_CONNECTED, strdup(name && *name ? name : "iPhone"));
}

static void on_audio(void *user, unsigned char compression, const uint8_t *data, size_t len,
                     uint64_t timestamp_ns)
{
    app *a = user;
    if (a->audio)
        ph_audio_push_timed(a->audio, compression, data, len, timestamp_ns);
}

static void on_audio_format(void *user, unsigned char compression, unsigned short samples)
{
    app *a = user;
    ph_audio_configure(a->audio, compression, samples);
}

static void on_audio_volume(void *user, float decibels)
{
    app *a = user;
    ph_audio_set_volume(a->audio, decibels);
}

static void on_audio_flush(void *user)
{
    app *a = user;
    ph_audio_flush(a->audio);
}

static void on_disconnected(void *user)
{
    on_audio_flush(user);
    push_event(user, EV_DISCONNECTED, NULL);
}

static void on_ble_status(void *user, const char *line)
{
    push_event(user, EV_BLE, strdup(line));
}

/* ------------------------------------------------------------ main thread */

static void update_title(app *a)
{
    char title[256];
    const char *control = a->opt->bluetooth && a->opt->ble_helper ? a->ble_status : "off";

    if (a->phone_name[0])
        snprintf(title, sizeof title, "%s - %s  [Bluetooth control: %s]%s", PORTHOLE_NAME,
                 a->phone_name, control, a->presentation_mode ? " [Different display layout]"
                                       : a->feed_mode ? " [Feed wheel]" : "");
    else
        snprintf(title, sizeof title,
                 "%s - on your iPhone: Control Center > Screen Mirroring > %s", PORTHOLE_NAME,
                 a->opt->name);
    {
        char pin[8];
        size_t used = strlen(title);
        if (a->polish && ph_polish_copy_pin(a->polish, pin, sizeof pin) && used + 1 < sizeof title)
            snprintf(title + used, sizeof title - used, "  PIN %s", pin);
    }
    ph_window_set_title(a->win, title);
    ph_window_set_status(a->win, a->phone_name[0] != 0,
                         !a->presentation_mode && !strcmp(control, "on"),
                         a->presentation_mode ? "Return to phone view" : control);
    ph_setup_update(&a->setup, a->input.enabled,
                     a->phone_name[0] && a->view_frame && a->view_frame->data[0]);
    ph_window_set_setup(a->win, &a->setup);
}

static void reset_mouse_input(app *a)
{
    ph_mouse_input_reset(&a->mouse_input);
    a->wheel_point_valid = false;
}

static void cancel_control_intent(app *a, Uint64 now)
{
    if (a->absolute)
        ph_abs_release_all(&a->abs, now);
    ph_gesture_cancel(&a->gesture, now);
    ph_pointer_cancel(&a->pointer, now);
    ph_scroll_cancel(&a->wheel);
    reset_mouse_input(a);
    a->feed_have_wheel = false;
}

static bool start_control_gesture(app *a, ph_gesture_kind kind,
                                  double x1, double y1, double x2, double y2,
                                  uint32_t duration)
{
    if (a->polish && ph_polish_precise(a->polish))
        return false; /* This scheduler owns the Bluetooth pointer, not the touch backend. */
    if (!a->input.enabled || !a->phone_name[0] || a->presentation_mode || a->setup.visible
        || ph_window_help_visible(a->win) || a->input.buttons
        || a->pointer.buttons || a->pointer.requested_buttons
        || ph_gesture_active(&a->gesture))
        return false;
    ph_gesture_request request = {.kind = kind, .start_x = x1, .start_y = y1,
        .end_x = x2, .end_y = y2, .width = a->picture_w, .height = a->picture_h,
        .duration_ms = duration};
    ph_scroll_cancel(&a->wheel);
    reset_mouse_input(a);
    return ph_gesture_start(&a->gesture, &request, SDL_GetTicks64());
}

static void handle_ble_status(app *a, const char *line)
{
    const char *state = NULL;

    if (!strcmp(line, "ready"))
#ifdef _WIN32
        state = "starting Bluetooth advertising";
#else
        state = "waiting for iPhone";
#endif
    else if (!strcmp(line, "advertising"))
        state = "waiting for iPhone";
    else if (!strcmp(line, "connected"))
        state = "connecting";
    else if (!strcmp(line, "subscribed"))
        state = "on";
    else if (!strcmp(line, "disconnected"))
        state = "waiting for iPhone";
    else if (!strcmp(line, "exited"))
        state = "unavailable; retrying";
    else if (!strncmp(line, "error", 5))
        state = "error (see log)";

    if (!strncmp(line, "sync", 4)) {
        unsigned int id;
        if (sscanf(line, "sync %u", &id) == 1) {
            ph_metrics_hid_ack(&a->metrics, (uint32_t)id, ph_metrics_now_ms());
            Uint64 now = SDL_GetTicks64();
            ph_pointer_ack(&a->pointer, (uint32_t)id, now);
            ph_scroll_ack_at(&a->wheel, (uint32_t)id, now);
            /* The main loop checks timeouts after queued ACK events are handled. */
        }
        PH_LOGD("Bluetooth: %s", line);
        return; /* internal bookkeeping */
    }
    if (!strncmp(line, "error", 5) || !strcmp(line, "exited"))
        PH_LOGW("Bluetooth helper: %s", line);
    else
        PH_LOGI("Bluetooth: %s", line);
#ifdef _WIN32
    /* Windows input belongs to one ready peer per helper pipe. Recover native
     * or protocol errors through a fresh process, not buffered old reports. */
    if (!strncmp(line, "error", 5) && a->link && !a->link_stopping) {
        a->link_stopping = true;
        ph_hid_link_request_stop(a->link);
    }
    if (a->link_stopping && strcmp(line, "exited"))
        state = "recovering";
#endif
    /* "connected" is followed by "subscribed" once reports flow; keep "on" if already on. */
    if (state && !(!strcmp(line, "connected") && !strcmp(a->ble_status, "on")))
        snprintf(a->ble_status, sizeof a->ble_status, "%s", state);
    ph_input_set_enabled(&a->input, !a->link_stopping && !strcmp(a->ble_status, "on"));
    ph_pointer_set_enabled(&a->pointer, a->input.enabled && a->phone_name[0]
                            && !a->presentation_mode, SDL_GetTicks64());
    if (!a->input.enabled) {
        ph_gesture_cancel(&a->gesture, SDL_GetTicks64());
        reset_mouse_input(a);
        ph_scroll_reset(&a->wheel);
        a->feed_mode = a->feed_have_wheel = false;
    }
    if (!strcmp(line, "exited")) {
        ph_hid_link_stop(a->link);
        a->link = NULL;
        a->link_stopping = false;
        ph_pointer_transport_reset(&a->pointer, SDL_GetTicks64());
        ph_scroll_transport_reset(&a->wheel);
        a->ble_retry_at = SDL_GetTicks64() + BLE_RETRY_MS;
    }
    update_title(a);
}

/* ---- sending input to the phone */

static void record_input_handoff(app *a)
{
    if (a->input_pending) {
        ph_metrics_input(&a->metrics, a->input_at, ph_metrics_now_ms());
        a->input_pending = false;
    }
}

static void mark_input(app *a, uint32_t timestamp)
{
    uint32_t age = SDL_GetTicks() - timestamp;
    uint64_t now = ph_metrics_now_ms();
    a->input_at = now >= age ? now - age : now;
    a->input_pending = true;
}

static void send_keys(void *user, const ph_hid_keyboard *keys)
{
    app *a = user;
    if (a->link && ph_hid_link_keyboard(a->link, keys))
        record_input_handoff(a);
}

static void send_consumer(void *user, uint16_t usage)
{
    app *a = user;
    if (a->link && ph_hid_link_consumer(a->link, usage))
        record_input_handoff(a);
}

static void send_abs(void *user, uint8_t buttons, double x, double y)
{
    app *a = user;
    if (a->link && ph_hid_link_abs_mouse(a->link, buttons, x, y))
        record_input_handoff(a);
}

static void send_mouse(void *user, const ph_hid_mouse *mouse)
{
    app *a = user;
    if (a->absolute)
        return; /* the absolute report map has no relative mouse */
    if (a->link) {
        ph_hid_mouse merged = *mouse;
        merged.buttons |= a->input.buttons | a->pointer.buttons;
        if (ph_hid_link_mouse(a->link, &merged))
            record_input_handoff(a);
    }
}

static void send_delay(void *user, uint16_t milliseconds)
{
    app *a = user;
    if (a->link)
        ph_hid_link_delay(a->link, milliseconds);
}

static void send_sync(void *user, uint32_t id)
{
    app *a = user;
    if (a->link && ph_hid_link_sync(a->link, id))
        ph_metrics_hid_sent(&a->metrics, id, ph_metrics_now_ms());
}

static void send_pointer_sync(void *user, uint32_t id)
{
    app *a = user;
    if (a->pointer_reference) {
        if (a->pointer.anchoring || a->pointer.buttons
            || a->pointer.phase != PH_POINTER_MOVING) {
            av_frame_unref(a->pointer_reference);
        } else if (a->view_frame && a->view_frame->data[0]
                   && a->pointer.known && a->pointer.learn_move
                   && (!a->pointer_reference->data[0]
                       || hypot(a->pointer.sent_dx, a->pointer.sent_dy) >= 20)) {
            /* Keep the background from before a larger move while subsequent
             * small corrections overlap the dot. Each still gets its own
             * dispatch baseline, ACK and changed-picture check. */
            av_frame_unref(a->pointer_reference);
            if (av_frame_ref(a->pointer_reference, a->view_frame) == 0) {
                a->reference_screen = a->view_screen;
                a->reference_at = SDL_GetTicks64();
            }
        }
    }
    if (a->view_frame && a->pointer_baseline) {
        av_frame_unref(a->pointer_baseline);
        if (a->view_frame->data[0] && av_frame_ref(a->pointer_baseline, a->view_frame) == 0)
            a->baseline_screen = a->view_screen;
    }
    send_sync(user, id);
}

static bool position_for_scroll(void *user, double x, double y, double aspect)
{
    app *a = user;
    bool ready = ph_pointer_scroll_target(&a->pointer, x, y, aspect, SDL_GetTicks64());
    if (a->pointer.failed)
        ph_scroll_cancel(&a->wheel);
    return ready;
}

static bool pointer_coordinates(app *a, int x, int y, double *nx, double *ny, double *aspect)
{
    int width, height;
    if (!a->link || !a->input.enabled || !a->phone_name[0] || a->presentation_mode
        || a->input.buttons || a->setup.visible)
        return false;
    ph_window_video_size(a->win, &width, &height);
    if (width <= 0 || height <= 0 || !ph_window_to_video(a->win, x, y, nx, ny))
        return false;
    *aspect = (double)width / height;
    return true;
}

static bool current_pointer_coordinates(app *a, double *nx, double *ny, double *aspect)
{
    SDL_Window *window = SDL_GetWindowFromID(ph_window_id(a->win));
    if (!window)
        return false;
    /* A window can move underneath a stationary cursor without updating
     * SDL_GetMouseState's cached client coordinates. Query the OS for hotkeys
     * so a double-tap cannot target the cursor's former window position. */
    int global_x, global_y, window_x, window_y, window_w, window_h;
    SDL_ClearError();
    SDL_GetGlobalMouseState(&global_x, &global_y);
    if (*SDL_GetError())
        return false;
    SDL_GetWindowPosition(window, &window_x, &window_y);
    SDL_GetWindowSize(window, &window_w, &window_h);
    double local_x = (double)global_x - window_x, local_y = (double)global_y - window_y;
    if (local_x < 0 || local_y < 0 || local_x >= window_w || local_y >= window_h)
        return false;
    return pointer_coordinates(a, (int)local_x, (int)local_y, nx, ny, aspect);
}

/* Absolute mode: window position -> phone picture fraction. */
static bool abs_coordinates(app *a, int x, int y, double *nx, double *ny)
{
    if (!a->link || !a->input.enabled || !a->phone_name[0] || a->presentation_mode
        || a->setup.visible)
        return false;
    return ph_window_to_video(a->win, x, y, nx, ny);
}

static void mouse_motion(app *a, const SDL_MouseMotionEvent *event)
{
    if (a->absolute) {
        double x, y;
        if (abs_coordinates(a, event->x, event->y, &x, &y)) {
            mark_input(a, event->timestamp);
            ph_abs_move(&a->abs, x, y, SDL_GetTicks64());
        }
        return;
    }
    double x, y, aspect;
    if (ph_gesture_active(&a->gesture))
        return; /* A deliberate gesture owns the pointer until release or cancellation. */
    if (pointer_coordinates(a, event->x, event->y, &x, &y, &aspect)) {
        mark_input(a, event->timestamp);
        /* Moving to a new location replaces wheel work aimed at the old location. */
        double wheel_x = a->wheel_point_valid ? a->wheel_point_x : a->wheel.target_x;
        double wheel_y = a->wheel_point_valid ? a->wheel_point_y : a->wheel.target_y;
        double dx = x - wheel_x, dy = (y - wheel_y) / aspect;
        if (dx * dx + dy * dy > 0.0001) {
            reset_mouse_input(a);
            ph_scroll_cancel(&a->wheel);
        }
        if (a->wheel.pending_x || a->wheel.pending_y || a->wheel.in_flight
            || a->wheel.waiting_frame)
            ph_pointer_scroll_target(&a->pointer, x, y, aspect, SDL_GetTicks64());
        else
            ph_pointer_target(&a->pointer, x, y, aspect, SDL_GetTicks64());
    }
}

static void mouse_button(app *a, const SDL_MouseButtonEvent *event)
{
    double x, y, aspect;
    uint8_t button = ph_mouse_input_button(event->button);
    if (!button)
        return;
    if (a->absolute) {
        bool down = event->type == SDL_MOUSEBUTTONDOWN;
        x = a->abs.want_x;
        y = a->abs.want_y;
        if (button > 0x04)
            return; /* the absolute map has three buttons */
        if (!abs_coordinates(a, event->x, event->y, &x, &y) && down)
            return; /* a press outside the picture does nothing; a release always lifts */
        mark_input(a, event->timestamp);
        if (down)
            a->abs_panel = 0; /* clicking may have closed or left the panel */
        ph_abs_button(&a->abs, button, down, x, y, SDL_GetTicks64());
        SDL_CaptureMouse(a->abs.held ? SDL_TRUE : SDL_FALSE);
        return;
    }
    if (ph_gesture_active(&a->gesture)) {
        cancel_control_intent(a, SDL_GetTicks64());
        SDL_CaptureMouse(SDL_FALSE);
        return; /* Cancel; do not reinterpret this press at the gesture's endpoint. */
    }
    if (a->polish && ph_polish_precise(a->polish) && event->button == SDL_BUTTON_LEFT) {
        double nx, ny;
        int vw = 0, vh = 0;
        ph_window_video_size(a->win, &vw, &vh);
        if (ph_window_to_video(a->win, event->x, event->y, &nx, &ny) &&
            ph_polish_click(a->polish, event->type == SDL_MOUSEBUTTONDOWN, nx, ny, vw, vh))
            return;
    }
    if (!pointer_coordinates(a, event->x, event->y, &x, &y, &aspect)) {
        a->input_pending = false;
        if (event->type == SDL_MOUSEBUTTONUP) {
            ph_pointer_cancel(&a->pointer, SDL_GetTicks64());
            SDL_CaptureMouse(SDL_FALSE);
        }
        return;
    }
    mark_input(a, event->timestamp);
    reset_mouse_input(a);
    ph_scroll_cancel(&a->wheel);
    ph_scroll_forget_bounds(&a->wheel);
    ph_pointer_button(&a->pointer, button, event->type == SDL_MOUSEBUTTONDOWN,
                      x, y, aspect, SDL_GetTicks64());
    SDL_CaptureMouse(event->type == SDL_MOUSEBUTTONDOWN ? SDL_TRUE : SDL_FALSE);
}

static void scroll(app *a, const SDL_MouseWheelEvent *event)
{
    int x, y, width, height;
    double nx, ny;
    if (a->absolute) {
#if SDL_VERSION_ATLEAST(2, 26, 0)
        x = event->mouseX;
        y = event->mouseY;
#else
        SDL_GetMouseState(&x, &y);
#endif
        double notches = event->y;
#if SDL_VERSION_ATLEAST(2, 0, 18)
        if (event->preciseY != 0)
            notches = event->preciseY;
#endif
        if (event->direction == SDL_MOUSEWHEEL_FLIPPED)
            notches = -notches;
        if (abs_coordinates(a, x, y, &nx, &ny) && notches != 0) {
            mark_input(a, event->timestamp);
            a->abs_panel = 0;
            ph_abs_wheel(&a->abs, notches, nx, ny, a->feed_mode, SDL_GetTicks64());
        }
        return;
    }
    if (!a->link || !a->input.enabled || !a->phone_name[0] || a->presentation_mode || a->setup.visible)
        return;
#if SDL_VERSION_ATLEAST(2, 26, 0)
    x = event->mouseX;
    y = event->mouseY;
#else
    SDL_GetMouseState(&x, &y);
#endif
    ph_window_video_size(a->win, &width, &height);
    if (width > 0 && height > 0 && ph_window_to_video(a->win, x, y, &nx, &ny)) {
        double aspect = (double)width / height;
        double dx = nx - a->wheel_point_x, dy = (ny - a->wheel_point_y) / aspect;
        if (a->wheel_point_valid && (fabs(aspect - a->wheel_point_aspect) > .0001
            || dx * dx + dy * dy > .0001))
            reset_mouse_input(a);
        /* Record even a fractional event's location. The scroll controller has
         * no target until a whole notch arrives; tiny intervening mouse motion
         * must not compare against its unrelated old/default target. */
        a->wheel_point_valid = true;
        a->wheel_point_x = nx;
        a->wheel_point_y = ny;
        a->wheel_point_aspect = aspect;
        mark_input(a, event->timestamp);
        double wheel_x = event->x, wheel_y = event->y;
#if SDL_VERSION_ATLEAST(2, 0, 18)
        /* Older event producers may fill only the integer compatibility fields. */
        if (event->preciseX != 0 || event->x == 0)
            wheel_x = event->preciseX;
        if (event->preciseY != 0 || event->y == 0)
            wheel_y = event->preciseY;
#endif
        if (a->feed_mode) {
            if (!isfinite(wheel_y) || wheel_y == 0)
                return;
            Uint64 now = SDL_GetTicks64();
            bool new_burst = !a->feed_have_wheel || now - a->feed_wheel_at >= 500;
            a->feed_have_wheel = true;
            a->feed_wheel_at = now;
            if (ph_gesture_active(&a->gesture) || !new_burst || a->input.buttons
                || a->pointer.requested_buttons || a->pointer.buttons)
                return;
            bool next = (event->direction == SDL_MOUSEWHEEL_FLIPPED ? -wheel_y : wheel_y) < 0;
            start_control_gesture(a, PH_GESTURE_SWIPE, .5, next ? .8 : .2,
                                   .5, next ? .2 : .8, 300);
            return;
        }
        if (ph_gesture_active(&a->gesture) || a->input.buttons
            || a->pointer.requested_buttons || a->pointer.buttons)
            return;
        int notches_x, notches_y;
        if (!ph_mouse_input_wheel(&a->mouse_input, wheel_x, wheel_y,
                                  event->direction == SDL_MOUSEWHEEL_FLIPPED,
                                  (SDL_GetModState() & KMOD_SHIFT) != 0,
                                  &notches_x, &notches_y))
            return;
        PH_LOGD("wheel at PC (%d,%d), phone (%.3f,%.3f)", x, y, nx, ny);
        ph_scroll_request(&a->wheel, nx, ny, (double)width / height,
                          notches_x, notches_y);
    } else {
        reset_mouse_input(a);
        ph_scroll_cancel(&a->wheel);
    }
}

static const char *setup_marker(const app *a)
{
    return a->opt->bluetooth && a->opt->ble_helper ? "setup-complete-controls" : "setup-complete-mirror";
}

static void finish_calibration(app *a, bool success)
{
    if (!a->calibration_saved)
        return;
    bool valid = isfinite(a->pointer.scale_x) && isfinite(a->pointer.scale_y)
                 && a->pointer.scale_x >= .05 && a->pointer.scale_x <= 20
                 && a->pointer.scale_y >= .05 && a->pointer.scale_y <= 20;
    if (success && valid) {
        char line[96];
        snprintf(line, sizeof line, "%.6f %.6f", a->pointer.scale_x, a->pointer.scale_y);
        if (!ph_config_write_line("pointer-scale", line)) {
            ph_setup_abort(&a->setup, PH_SETUP_SAVE_FAILED);
            success = false;
        } else {
            PH_LOGI("guided calibration confirmed: x=%.3f y=%.3f", a->pointer.scale_x,
                     a->pointer.scale_y);
        }
    } else if (success) {
        ph_setup_abort(&a->setup, PH_SETUP_POINTER_UNCONFIRMED);
        success = false;
    }
    if (!success)
        ph_pointer_set_scale(&a->pointer, a->previous_scale_x, a->previous_scale_y);
    PH_LOGD("pointer cancel: calibration %s", success ? "complete" : "rollback");
    ph_pointer_cancel(&a->pointer, SDL_GetTicks64());
    a->calibration_saved = false;
}

static void open_setup(app *a)
{
    a->input_pending = false;
    cancel_control_intent(a, SDL_GetTicks64());
    ph_input_release_all(&a->input);
    PH_LOGD("pointer cancel: guided setup opened");
    ph_pointer_cancel(&a->pointer, SDL_GetTicks64());
    ph_scroll_cancel(&a->wheel);
    SDL_CaptureMouse(SDL_FALSE);
    ph_setup_open(&a->setup, a->opt->bluetooth && a->opt->ble_helper);
    update_title(a);
}

static void setup_action(app *a, ph_setup_action action)
{
    if (action == PH_SETUP_ACTION_NONE)
        return;
    if (action == PH_SETUP_ACTION_CLOSE || action == PH_SETUP_ACTION_BACK) {
        if (a->setup.calibrating)
            ph_setup_abort(&a->setup, PH_SETUP_CANCELED);
        finish_calibration(a, false);
        if (action == PH_SETUP_ACTION_CLOSE)
            ph_setup_close(&a->setup);
        else
            ph_setup_back(&a->setup);
    } else if (action == PH_SETUP_ACTION_CALIBRATE) {
        if (!a->presentation_mode && ph_setup_start_calibration(&a->setup, SDL_GetTicks64())) {
            a->previous_scale_x = a->pointer.scale_x > 0 ? a->pointer.scale_x : 1;
            a->previous_scale_y = a->pointer.scale_y > 0 ? a->pointer.scale_y : 1;
            a->calibration_saved = true;
            PH_LOGD("pointer reset: guided calibration started");
            ph_pointer_reset(&a->pointer, SDL_GetTicks64());
            ph_scroll_cancel(&a->wheel);
        }
    } else if (ph_setup_next(&a->setup) && a->setup.completed) {
        if (!ph_config_write_line(setup_marker(a), "1"))
            PH_LOGW("couldn't save guided setup completion");
    }
    ph_window_set_setup(a->win, &a->setup);
}

static void tick_setup(app *a)
{
    ph_setup_update(&a->setup, a->input.enabled,
                     a->phone_name[0] && a->view_frame && a->view_frame->data[0]);
    if (a->setup.calibrating) {
        double x, y;
        if (a->picture_w > 0 && a->picture_h > 0 && ph_setup_target(&a->setup, &x, &y)) {
            double aspect = (double)a->picture_w / a->picture_h;
            Uint64 now = SDL_GetTicks64();
            ph_pointer_target(&a->pointer, x, y, aspect, now);
            ph_setup_observe(&a->setup, ph_pointer_ready(&a->pointer, x, y, aspect),
                              a->pointer.failed, now);
        }
    }
    if (a->calibration_saved && !a->setup.calibrating)
        finish_calibration(a, a->setup.calibrated);
    ph_window_set_setup(a->win, &a->setup);
}

static void tick_metrics(app *a)
{
    Uint64 now = SDL_GetTicks64();
    if (now < a->metrics_at)
        return;
    a->metrics_at = now + 250;
    ph_metrics_snapshot snapshot;
    ph_metrics_snapshot_get(&a->metrics, ph_metrics_now_ms(), &snapshot);
    ph_decoder_stats stats;
    ph_decoder_get_stats(a->dec, &stats);
    ph_window_set_metrics(a->win, &snapshot, stats.queued_packets, stats.packets_discarded,
                           stats.frames_superseded);
    if (a->opt->diagnostics && now >= a->metrics_log_at) {
        a->metrics_log_at = now + 5000;
        PH_LOGI("local diagnostics: FPS %.1f; receive/decode %.1f ms; receive/display %.1f ms; "
                "input handoff %.1f ms; helper ACK %.1f ms; pending %zu; BT interval unknown",
                snapshot.fps_known ? snapshot.fps : -1,
                snapshot.receive_to_decode.known ? snapshot.receive_to_decode.last_ms : -1,
                snapshot.receive_to_present.known ? snapshot.receive_to_present.last_ms : -1,
                snapshot.input_handoff.known ? snapshot.input_handoff.last_ms : -1,
                snapshot.helper_ack.known ? snapshot.helper_ack.last_ms : -1,
                snapshot.pending_acks);
    }
}

static uint16_t pc_consumer_usage(SDL_Scancode scancode)
{
    switch (scancode) {
    case SDL_SCANCODE_VOLUMEUP: return PH_HID_CONSUMER_VOLUME_UP;
    case SDL_SCANCODE_VOLUMEDOWN: return PH_HID_CONSUMER_VOLUME_DOWN;
    case SDL_SCANCODE_MUTE:
    case SDL_SCANCODE_AUDIOMUTE: return PH_HID_CONSUMER_MUTE;
    case SDL_SCANCODE_AUDIOPLAY: return PH_HID_CONSUMER_PLAY_PAUSE;
    case SDL_SCANCODE_AUDIONEXT: return PH_HID_CONSUMER_NEXT_TRACK;
    case SDL_SCANCODE_AUDIOPREV: return PH_HID_CONSUMER_PREV_TRACK;
    case SDL_SCANCODE_AUDIOSTOP: return PH_HID_CONSUMER_STOP;
    case SDL_SCANCODE_EJECT: return PH_HID_CONSUMER_EJECT;
    default: return 0;
    }
}

static uint8_t pc_modifiers(SDL_Keymod mods)
{
    uint8_t result = 0;
    if (mods & KMOD_LCTRL) result |= PH_HID_MOD_LCTRL;
    if (mods & KMOD_RCTRL) result |= PH_HID_MOD_RCTRL;
    if (mods & KMOD_LSHIFT) result |= PH_HID_MOD_LSHIFT;
    if (mods & KMOD_RSHIFT) result |= PH_HID_MOD_RSHIFT;
    if (mods & KMOD_LALT) result |= PH_HID_MOD_LALT;
    if (mods & KMOD_RALT) result |= PH_HID_MOD_RALT;
    if (mods & KMOD_LGUI) result |= PH_HID_MOD_LGUI;
    if (mods & KMOD_RGUI) result |= PH_HID_MOD_RGUI;
    return result;
}

static void handle_key(app *a, const SDL_KeyboardEvent *e)
{
    SDL_Scancode sc = e->keysym.scancode;
    bool down = e->type == SDL_KEYDOWN;
    bool plain = !(e->keysym.mod & (KMOD_SHIFT | KMOD_CTRL | KMOD_ALT | KMOD_GUI));
    if ((unsigned)sc < 256 && a->gesture_keys[sc]) {
        if (!down)
            a->gesture_keys[sc] = false;
        return;
    }
    if (down && sc == SDL_SCANCODE_F2 && plain && !a->input.down[sc]) {
        a->gesture_keys[sc] = true;
        if (e->type == SDL_KEYDOWN && !e->repeat) {
            a->input_pending = false;
            if (a->setup.visible)
                setup_action(a, PH_SETUP_ACTION_CLOSE);
            else
                open_setup(a);
        }
        return;
    }
    if (down && sc == SDL_SCANCODE_F3 && plain && !a->input.down[sc]) {
        a->gesture_keys[sc] = true;
        if (e->type == SDL_KEYDOWN && !e->repeat)
            ph_window_toggle_diagnostics(a->win);
        return;
    }
    if (a->polish && ph_polish_key(a->polish, e, &a->input,
                                   SDL_GetWindowFromID(ph_window_id(a->win)), a->picture_w,
                                   a->picture_h, a->view_frame,
                                   !a->setup.visible && !ph_window_help_visible(a->win)))
        return;
    if (down && sc == SDL_SCANCODE_F1 && plain && !a->input.down[sc]) {
        a->gesture_keys[sc] = true;
        if (e->type == SDL_KEYDOWN && !e->repeat) {
            a->input_pending = false;
            if (a->setup.visible)
                setup_action(a, PH_SETUP_ACTION_CLOSE);
            PH_LOGD("pointer cancel: help toggled");
            cancel_control_intent(a, SDL_GetTicks64());
            SDL_CaptureMouse(SDL_FALSE);
            ph_input_release_all(&a->input);
            ph_scroll_cancel(&a->wheel);
            ph_window_toggle_help(a->win);
        }
        return;
    }
    if (a->setup.visible) {
        if (e->type == SDL_KEYDOWN && !e->repeat) {
            if (sc == SDL_SCANCODE_ESCAPE)
                setup_action(a, PH_SETUP_ACTION_CLOSE);
            else if (sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_LEFT)
                setup_action(a, PH_SETUP_ACTION_BACK);
            else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER)
                setup_action(a, a->setup.step == PH_SETUP_CALIBRATE && !a->setup.calibrated
                                    ? PH_SETUP_ACTION_CALIBRATE : PH_SETUP_ACTION_NEXT);
        }
        return;
    }
    if (ph_window_help_visible(a->win))
        return;
    if (down && sc == SDL_SCANCODE_F4 && plain && !a->input.down[sc]) {
        a->gesture_keys[sc] = true;
        if (down && !e->repeat) {
            cancel_control_intent(a, SDL_GetTicks64());
            a->feed_mode = !a->feed_mode;
            update_title(a);
            PH_LOGI("feed wheel: %s", a->feed_mode ? "on (one swipe per wheel burst)" : "off");
        }
        return;
    }
    if (down && sc == SDL_SCANCODE_ESCAPE && (ph_gesture_active(&a->gesture)
        || a->pointer.buttons || a->pointer.requested_buttons || a->input.buttons)) {
        cancel_control_intent(a, SDL_GetTicks64());
        ph_input_release_all(&a->input);
        SDL_CaptureMouse(SDL_FALSE);
        a->gesture_keys[sc] = true;
        return;
    }
    if (down && a->absolute && (e->keysym.mod & KMOD_LALT) && a->phone_name[0]
        && a->input.enabled) {
        /* Exact edge gestures replace AssistiveTouch's mapped buttons 3-6. */
        Uint64 now = SDL_GetTicks64();
        double px = a->abs.want_x, py = a->abs.want_y;
        bool shift = (e->keysym.mod & KMOD_SHIFT) != 0, handled = true, sent = true;
        bool panel = sc == SDL_SCANCODE_S || sc == SDL_SCANCODE_C || sc == SDL_SCANCODE_N;
        bool close = panel && !e->repeat && a->abs_panel == sc && now - a->abs_panel_at < 60000;
        if (!e->repeat && (sc == SDL_SCANCODE_H || close)) {
            /* Home swipe; a second press of the same panel key closes that panel
             * the same way. The consumer Home key cannot replace it: with the
             * absolute map, iOS 26.6.1 opened Spotlight for it instead of Home. */
            sent = ph_abs_home(&a->abs, now);
            a->abs_panel = 0;
            a->gesture_keys[sc] = true;
            mark_input(a, e->timestamp);
            if (!sent)
                PH_LOGD("gesture skipped: a mouse button is held");
            return;
        }
        switch (sc) {
        case SDL_SCANCODE_H: break; /* key repeat */
        case SDL_SCANCODE_S: sent = ph_abs_app_switcher(&a->abs, now); break;
        case SDL_SCANCODE_C: sent = ph_abs_control_center(&a->abs, now); break;
        case SDL_SCANCODE_N: sent = ph_abs_notification_center(&a->abs, now); break;
        case SDL_SCANCODE_D: sent = ph_abs_tap(&a->abs, px, py, 2, 60, now); break;
        case SDL_SCANCODE_L: sent = ph_abs_tap(&a->abs, px, py, 1, 1000, now); break;
        case SDL_SCANCODE_PAGEDOWN: sent = ph_abs_swipe(&a->abs, .5, .8, .5, .2, 300, 0, now); break;
        case SDL_SCANCODE_PAGEUP: sent = ph_abs_swipe(&a->abs, .5, .2, .5, .8, 300, 0, now); break;
        case SDL_SCANCODE_U: sent = ph_abs_swipe(&a->abs, .5, .98, .5, .35, 300, 0, now); break;
        case SDL_SCANCODE_UP:
        case SDL_SCANCODE_DOWN:
        case SDL_SCANCODE_LEFT:
        case SDL_SCANCODE_RIGHT:
            if (!shift) {
                handled = false; /* plain Alt+arrows: volume / Back */
                break;
            }
            sent = ph_abs_swipe(&a->abs,
                                sc == SDL_SCANCODE_LEFT ? .8 : sc == SDL_SCANCODE_RIGHT ? .2 : .5,
                                sc == SDL_SCANCODE_UP ? .8 : sc == SDL_SCANCODE_DOWN ? .2 : .5,
                                sc == SDL_SCANCODE_LEFT ? .2 : sc == SDL_SCANCODE_RIGHT ? .8 : .5,
                                sc == SDL_SCANCODE_UP ? .2 : sc == SDL_SCANCODE_DOWN ? .8 : .5,
                                300, 0, now);
            break;
        case SDL_SCANCODE_P: {
            /* Screenshot: Cmd+Shift+3 with the hardware keyboard. */
            ph_hid_keyboard shot = {.modifiers = PH_HID_MOD_LGUI | PH_HID_MOD_LSHIFT,
                                    .keys = {0x20}};
            send_keys(a, &shot);
            memset(&shot, 0, sizeof shot);
            send_keys(a, &shot);
            break;
        }
        default: handled = false; break;
        }
        /* Only a handled shortcut changes the open-panel record: Alt itself
         * arrives here too, and clearing on it broke the second-press close. */
        if (handled && !e->repeat) {
            a->abs_panel = panel && sent ? sc : 0;
            a->abs_panel_at = now;
        }
        if (handled) {
            a->gesture_keys[sc] = true;
            if (!e->repeat)
                mark_input(a, e->timestamp);
            if (!sent)
                PH_LOGD("gesture skipped: a mouse button is held");
            return;
        }
    }
    if (down && (e->keysym.mod & KMOD_LALT)) {
        ph_gesture_kind kind = PH_GESTURE_SWIPE;
        double x1 = .5, y1 = .8, x2 = .5, y2 = .2;
        uint32_t duration = 300;
        bool gesture_key = true;
        switch (sc) {
        case SDL_SCANCODE_PAGEDOWN: break;
        case SDL_SCANCODE_PAGEUP: y1 = .2; y2 = .8; break;
        case SDL_SCANCODE_U: y1 = .98; y2 = .35; break;
        case SDL_SCANCODE_UP: gesture_key = (e->keysym.mod & KMOD_SHIFT) != 0; break;
        case SDL_SCANCODE_DOWN:
            gesture_key = (e->keysym.mod & KMOD_SHIFT) != 0; y1 = .2; y2 = .8; break;
        case SDL_SCANCODE_LEFT:
            gesture_key = (e->keysym.mod & KMOD_SHIFT) != 0;
            x1 = .8; x2 = .2; y1 = y2 = .5; break;
        case SDL_SCANCODE_RIGHT:
            gesture_key = (e->keysym.mod & KMOD_SHIFT) != 0;
            x1 = .2; x2 = .8; y1 = y2 = .5; break;
        case SDL_SCANCODE_D:
        case SDL_SCANCODE_L: {
            double aspect;
            gesture_key = current_pointer_coordinates(a, &x1, &y1, &aspect);
            x2 = x1; y2 = y1;
            kind = sc == SDL_SCANCODE_D ? PH_GESTURE_DOUBLE_TAP : PH_GESTURE_HOLD;
            duration = 1000;
            break;
        }
        default: gesture_key = false; break;
        }
        if (gesture_key) {
            a->gesture_keys[sc] = true;
            if (!e->repeat) {
                mark_input(a, e->timestamp);
                if (!start_control_gesture(a, kind, x1, y1, x2, y2, duration))
                    PH_LOGD("gesture skipped: controls or pointer busy");
            }
            return;
        }
    }
    if (e->type == SDL_KEYDOWN) {
        if (ph_gesture_active(&a->gesture))
            ph_gesture_cancel(&a->gesture, SDL_GetTicks64());
        reset_mouse_input(a);
        ph_scroll_cancel(&a->wheel); /* a new action cancels queued wheel batches */
        ph_scroll_forget_bounds(&a->wheel);
        if ((e->keysym.mod & KMOD_LALT) || sc == SDL_SCANCODE_LALT) {
            PH_LOGD("pointer cancel: Alt shortcut");
            cancel_control_intent(a, SDL_GetTicks64());
            SDL_CaptureMouse(SDL_FALSE);
        }
    }
    uint16_t consumer = pc_consumer_usage(sc);
    if (a->phone_name[0] && consumer) {
        if (!down || !(e->keysym.mod & KMOD_LALT)) {
            mark_input(a, e->timestamp);
            ph_input_consumer_key(&a->input, consumer, down, e->repeat != 0);
        }
        return;
    }
    if (a->phone_name[0] && (unsigned)sc < 256) {
        mark_input(a, e->timestamp);
        ph_input_key(&a->input, (uint8_t)sc, e->type == SDL_KEYDOWN, e->repeat != 0,
                     pc_modifiers((SDL_Keymod)e->keysym.mod));
    }
}

static void release_everything(app *a)
{
    if (a->absolute)
        ph_abs_release_all(&a->abs, SDL_GetTicks64());
    ph_gesture_cancel(&a->gesture, SDL_GetTicks64());
    a->feed_mode = a->feed_have_wheel = false;
    memset(a->gesture_keys, 0, sizeof a->gesture_keys);
    reset_mouse_input(a);
    ph_input_release_all(&a->input);
    ph_pointer_reset(&a->pointer, SDL_GetTicks64());
    ph_scroll_reset(&a->wheel);
    SDL_CaptureMouse(SDL_FALSE);
}

/* AirPlay can stop sending frames once the pointer is still. Revisit the latest
 * decoded image after the HID ACK rather than requiring another encoded frame. */
/* Diagnostics only, off unless PORTHOLE_CURSOR_TRACE_DIR is set: saves the exact
 * luma frames and pointer state for detector misses (at most 150), so misses
 * over real content can be replayed offline. Never alters control decisions. */
static void trace_cursor_miss(app *a, const ph_yuv_planes *p, const ph_rect *screen)
{
    static unsigned int captures;
    static const char *dir;
    static bool checked;
    if (!checked) {
        dir = getenv("PORTHOLE_CURSOR_TRACE_DIR");
        checked = true;
    }
    if (!dir || !*dir || captures >= 150 || a->checked_found || !a->pointer_baseline->data[0])
        return;
    captures++;
    char path[1024];
    snprintf(path, sizeof path, "%s/%04u.txt", dir, captures);
    FILE *meta = fopen(path, "w");
    if (!meta)
        return;
    fprintf(meta, "now=%llu sent=%llu ack=%llu frame_seen=%llu sync=%u anchoring=%d "
                  "hint=%.4f,%.4f radius=%.4f actual=%.4f,%.4f start=%.4f,%.4f "
                  "sent_delta=%.2f,%.2f target=%.4f,%.4f known=%d buttons=%u "
                  "recoveries=%u reacquiring=%d scale=%.3f,%.3f screen=%d,%d,%d,%d\n",
            (unsigned long long)SDL_GetTicks64(), (unsigned long long)a->pointer.sent_at,
            (unsigned long long)a->pointer.wait_since, (unsigned long long)a->view_seen_at,
            a->pointer.sync_id, a->pointer.anchoring, a->pointer.hint_x, a->pointer.hint_y,
            a->pointer.search_radius, a->pointer.actual_x, a->pointer.actual_y,
            a->pointer.sent_start_x, a->pointer.sent_start_y, a->pointer.sent_dx,
            a->pointer.sent_dy, a->pointer.target_x, a->pointer.target_y, a->pointer.known,
            a->pointer.buttons, a->pointer.recoveries, a->pointer.reacquiring,
            a->pointer.scale_x, a->pointer.scale_y, screen->x, screen->y, screen->w, screen->h);
    fclose(meta);
    for (int old = 0; old < 2; old++) {
        ph_rect crop = old ? a->baseline_screen : *screen;
        const uint8_t *data = old ? a->pointer_baseline->data[0] : p->y;
        int stride = old ? a->pointer_baseline->linesize[0] : p->y_stride;
        snprintf(path, sizeof path, "%s/%04u-%s.pgm", dir, captures, old ? "before" : "after");
        FILE *file = fopen(path, "wb");
        if (!file)
            continue;
        fprintf(file, "P5\n%d %d\n255\n", crop.w, crop.h);
        for (int row = 0; row < crop.h; row++)
            fwrite(data + (crop.y + row) * stride + crop.x, 1, (size_t)crop.w, file);
        fclose(file);
    }
}

/* True when much of the search area (outside the dot's start and predicted end)
 * changed since the movement's baseline: playing video, scrolling, animation. */
static bool content_moving(const app *a, const ph_yuv_planes *p, const ph_rect *screen,
                           double hx, double hy, double radius)
{
    const AVFrame *b = a->pointer_baseline;
    if (!b->data[0] || a->baseline_screen.w != screen->w || a->baseline_screen.h != screen->h)
        return false;
    double dot = fmin(screen->w, screen->h) * 11.0 / 500 + 4;
    double reach = fmax(radius, fmin(screen->w, screen->h) * 60.0 / 500) + dot;
    double sx = a->pointer.sent_start_x * screen->w, sy = a->pointer.sent_start_y * screen->h;
    int left = (int)fmax(0, hx - reach), right = (int)fmin(screen->w - 1, hx + reach);
    int top = (int)fmax(0, hy - reach), bottom = (int)fmin(screen->h - 1, hy + reach);
    int changed = 0, samples = 0;
    for (int y = top; y <= bottom; y += 3) {
        for (int x = left; x <= right; x += 3) {
            if (hypot(x - hx, y - hy) <= dot || hypot(x - sx, y - sy) <= dot)
                continue;
            int now_luma = p->y[(screen->y + y) * p->y_stride + screen->x + x];
            int then_luma = b->data[0][(a->baseline_screen.y + y) * b->linesize[0]
                                       + a->baseline_screen.x + x];
            changed += abs(now_luma - then_luma) >= 8;
            samples++;
        }
    }
    return samples >= 50 && changed * 100 > samples * 8;
}

static void observe_pointer(app *a)
{
    const AVFrame *f = a->view_frame;
    Uint64 now = SDL_GetTicks64();
    if (fabs(a->pointer.target_x - a->seen_target_x) > 1e-4
        || fabs(a->pointer.target_y - a->seen_target_y) > 1e-4) {
        a->seen_target_x = a->pointer.target_x;
        a->seen_target_y = a->pointer.target_y;
        a->target_moved_at = now;
    }
    double hx, hy, radius;
    if (!f || !f->data[0] || a->pointer.phase != PH_POINTER_WAIT_VIDEO
        || atomic_load(&a->frame_pending) /* inspect already-arrived video first */
        || (!a->pointer.anchoring && a->view_seen_at < a->pointer.sent_at)
        || !ph_pointer_hint(&a->pointer, &hx, &hy, &radius))
        return;
    ph_rect screen = a->view_screen;
    ph_yuv_planes planes = {f->data[0], f->data[1], f->data[2], f->linesize[0], f->linesize[1],
                            f->width, f->height};
    if (a->checked_sync != a->pointer.sync_id || a->checked_serial != a->view_serial) {
        double px = 0, py = 0;
        bool found = false;
        if (a->pointer.anchoring) {
            found = ph_video_cursor(&planes, &screen, hx * screen.w, hy * screen.h,
                                     radius * screen.w, &px, &py);
        } else if (a->pointer_baseline->data[0]) {
            AVFrame *b = a->pointer_baseline;
            ph_yuv_planes baseline = {b->data[0], b->data[1], b->data[2], b->linesize[0],
                                      b->linesize[1], b->width, b->height};
            found = ph_video_cursor_changed(&planes, &screen, &baseline, &a->baseline_screen,
                                             hx * screen.w, hy * screen.h,
                                             radius * screen.w, &px, &py);
            if (!found && a->pointer.known && a->pointer.learn_move && !a->pointer.buttons
                && a->pointer_reference->data[0] && now >= a->reference_at
                && now - a->reference_at <= 2000
                && hypot(a->pointer.sent_dx, a->pointer.sent_dy) < 20) {
                AVFrame *ref = a->pointer_reference;
                ph_yuv_planes reference = {ref->data[0], ref->data[1], ref->data[2],
                                           ref->linesize[0], ref->linesize[1],
                                           ref->width, ref->height};
                found = ph_video_cursor_referenced(&planes, &screen, &baseline, &a->baseline_screen,
                             &reference, &a->reference_screen,
                             a->pointer.sent_start_x * screen.w,
                             a->pointer.sent_start_y * screen.h,
                             hx * screen.w, hy * screen.h, radius * screen.w, &px, &py);
            }
        }
        a->content_moving = !a->pointer.anchoring
                            && content_moving(a, &planes, &screen, hx * screen.w, hy * screen.h,
                                              radius * screen.w);
        if (a->content_moving)
            found = false; /* a single picture's match is not trusted on moving content */
        if (!a->pointer.anchoring && a->pointer_baseline->data[0]) {
            /* Playing video changes the whole picture, so the changed search
             * above often matches some other moving shape. Once a second
             * picture after this movement exists, pixels still changing
             * between them are video motion: mask them out and let only the
             * stopped dot count. On recorded Shorts this kept correct matches
             * (51 vs 50 of 150) and cut wrong positions from 54 to 6. A still
             * screen sends no second picture and keeps the result above. */
            if (a->after_sync == a->pointer.sync_id && a->after_serial != a->view_serial
                && a->pointer_after->data[0] && a->after_screen.w == screen.w
                && a->after_screen.h == screen.h && a->after_screen.x == screen.x
                && a->after_screen.y == screen.y) {
                AVFrame *b = a->pointer_baseline, *l = a->pointer_after;
                ph_yuv_planes baseline = {b->data[0], b->data[1], b->data[2], b->linesize[0],
                                          b->linesize[1], b->width, b->height};
                ph_yuv_planes later = {l->data[0], l->data[1], l->data[2], l->linesize[0],
                                       l->linesize[1], l->width, l->height};
                double width = 500.0 * fmax(1, a->pointer.aspect);
                double travel = hypot(a->pointer.sent_dx, a->pointer.sent_dy);
                double near = fmin(radius * screen.w, (40 + 0.35 * travel) * screen.w / width);
                found = ph_video_cursor_settled(&planes, &screen, &later, NULL, &baseline,
                                                &a->baseline_screen, 2, hx * screen.w,
                                                hy * screen.h, near, &px, &py)
                        || ph_video_cursor_settled(&planes, &screen, &later, NULL, &baseline,
                                                   &a->baseline_screen, 1, hx * screen.w,
                                                   hy * screen.h, near, &px, &py);
            }
            av_frame_unref(a->pointer_after);
            if (av_frame_ref(a->pointer_after, f) == 0) {
                a->after_sync = a->pointer.sync_id;
                a->after_serial = a->view_serial;
                a->after_screen = screen;
            }
        }
        if (!found)
            a->candidate_sync = 0; /* a newer picture without the dot voids an older match */
        a->checked_sync = a->pointer.sync_id;
        a->checked_serial = a->view_serial;
        a->checked_found = found;
        a->checked_x = px;
        a->checked_y = py;
        trace_cursor_miss(a, &planes, &screen);
    }
    if (!a->checked_found && !a->pointer.anchoring && !a->pointer.buttons
        && now >= a->pointer.wait_since && now - a->pointer.wait_since >= 300
        && (a->fallback_sync != a->pointer.sync_id || a->fallback_serial != a->view_serial)) {
        /* The changed-frame search cannot see a dot that did not visibly move
         * (a tiny correction, or one AirPlay has not re-encoded). Live traces
         * showed it sitting on the prediction while the app waited to probe.
         * Late, and only within 6 px of the prediction, accept the dot itself:
         * a stale frame's old dot can then be off by at most that much. */
        double fx = 0, fy = 0;
        a->fallback_sync = a->pointer.sync_id;
        a->fallback_serial = a->view_serial;
        /* Only on content that is still around the prediction. Over playing
         * video the shape search alone reported a "dot" at 16-55% of empty spots
         * in recorded Shorts frames. */
        bool still = false;
        if (a->pointer_baseline->data[0] && a->baseline_screen.w == screen.w
            && a->baseline_screen.h == screen.h) {
            const AVFrame *b = a->pointer_baseline;
            int cx = (int)lround(hx * screen.w), cy = (int)lround(hy * screen.h);
            int reach = (int)ceil(fmin(screen.w, screen.h) * 30.0 / 500), moved = 0, total = 0;
            for (int yy = cy - reach; yy <= cy + reach; yy++) {
                if (yy < 0 || yy >= screen.h)
                    continue;
                for (int xx = cx - reach; xx <= cx + reach; xx++) {
                    if (xx < 0 || xx >= screen.w)
                        continue;
                    int now_luma = planes.y[(screen.y + yy) * planes.y_stride + screen.x + xx];
                    int then_luma = b->data[0][(a->baseline_screen.y + yy) * b->linesize[0]
                                               + a->baseline_screen.x + xx];
                    moved += abs(now_luma - then_luma) >= 8;
                    total++;
                }
            }
            still = total > 0 && moved * 50 <= total; /* at most 2% changed */
        }
        if (still && ph_video_cursor(&planes, &screen, hx * screen.w, hy * screen.h,
                                     6.0 * fmin(screen.w, screen.h) / 500, &fx, &fy)) {
            PH_LOGD("phone pointer found unchanged at its predicted position");
            a->pointer.learn_move = false; /* not a measurement of this move */
            a->checked_found = true;
            a->checked_x = fx;
            a->checked_y = fy;
        }
    }
    if (!a->checked_found) {
        /* Moving content and no trusted sighting yet: rather than probing, sweeping
         * or chasing look-alikes, take the acknowledged movement's prediction.
         * While the PC mouse keeps moving, do so as soon as the ACK has settled
         * (waiting for video made each step 170-300 ms on video, against 87 ms on
         * still screens). Once it stops, give real sightings 250 ms first. */
        bool mouse_moving = now - a->target_moved_at < 150;
        if (a->content_moving && now >= a->pointer.wait_since
            && now - a->pointer.wait_since >= (mouse_moving ? 40 : 250))
            ph_pointer_assume(&a->pointer, !mouse_moving, now);
        return;
    }
    double dx = a->checked_x - a->candidate_x, dy = a->checked_y - a->candidate_y;
    if (a->candidate_sync != a->pointer.sync_id || dx * dx + dy * dy > 4) {
        a->candidate_sync = a->pointer.sync_id;
        a->candidate_since = now;
    }
    a->candidate_x = a->checked_x;
    a->candidate_y = a->checked_y;
    /* Start tracking only after the fence ACK (WAIT_VIDEO), but overlap its
     * settling interval with candidate stability. ph_pointer_observe retains
     * the independent ACK-age check; neither 32 ms safety condition is removed. */
    if (now - a->candidate_since < 32)
        return;
    PH_LOGD("phone pointer observed at (%.1f,%.1f)%s", a->candidate_x, a->candidate_y,
            a->content_moving ? " over moving content" : "");
    if (a->content_moving)
        a->pointer.learn_move = false; /* look-alikes must never retune the gain */
    ph_pointer_observe(&a->pointer, true, a->candidate_x / screen.w,
                        a->candidate_y / screen.h, now);
}

/* Shows a decoded frame, cropping the black side bars iOS adds to TV-shaped frames. */
static void show_frame(app *a, const AVFrame *f)
{
    ph_yuv_planes planes = {f->data[0], f->data[1], f->data[2], f->linesize[0], f->linesize[1],
                            f->width, f->height};
    if (f->width < f->height) {
        double aspect = (double)f->width / f->height;
        if (aspect != a->portrait_aspect)
            PH_LOGD("phone screen aspect %.4f", aspect);
        a->portrait_aspect = aspect;
    }
    ph_rect screen = ph_video_content_rect_stable(&planes, a->portrait_aspect, &a->crop);
    bool presentation = !ph_video_matches_phone_screen(screen, a->portrait_aspect);
    if (presentation != a->presentation_mode) {
        av_frame_unref(a->pointer_reference);
        a->presentation_mode = presentation;
        cancel_control_intent(a, SDL_GetTicks64());
        ph_pointer_set_enabled(&a->pointer, a->input.enabled && a->phone_name[0]
                                && !presentation, SDL_GetTicks64());
        SDL_CaptureMouse(SDL_FALSE);
        PH_LOGI("pointer mapping: %s (%dx%d, phone aspect %.4f)",
                presentation ? "paused; display layout differs from phone"
                             : "phone layout restored", screen.w, screen.h, a->portrait_aspect);
        update_title(a);
    }
    if (screen.w != a->picture_w || screen.h != a->picture_h) {
        av_frame_unref(a->pointer_reference);
        ph_gesture_cancel(&a->gesture, SDL_GetTicks64());
        reset_mouse_input(a);
        PH_LOGD("pointer reset: picture size %dx%d -> %dx%d", a->picture_w, a->picture_h,
                 screen.w, screen.h);
        ph_pointer_reset(&a->pointer, SDL_GetTicks64());
        ph_scroll_reset(&a->wheel);
        a->picture_w = screen.w;
        a->picture_h = screen.h;
    }
    av_frame_unref(a->view_frame);
    av_frame_ref(a->view_frame, f);
    a->view_screen = screen;
    a->view_seen_at = SDL_GetTicks64();
    a->view_serial++;
    observe_pointer(a);
    ph_rect bar;
    bool visible = ph_video_scrollbar(&planes, &screen, &bar);
    int previous_edge = a->wheel.blocked_y;
    ph_scroll_observe(&a->wheel, visible,
                       visible ? (double)bar.y / screen.h : 0,
                       visible ? (double)(bar.y + bar.h) / screen.h : 0,
                       SDL_GetTicks64());
    if (a->wheel.blocked_y && previous_edge != a->wheel.blocked_y)
        PH_LOGD("wheel stopped at %s boundary", a->wheel.blocked_y > 0 ? "top" : "bottom");
    ph_window_present(a->win, f, &screen);
    if (a->polish)
        ph_polish_layout(a->polish, SDL_GetWindowFromID(ph_window_id(a->win)), screen.w, screen.h);
    ph_metrics_frame(&a->metrics, a->frame_timing.valid ? a->frame_timing.received_ms : 0,
                      a->frame_timing.valid ? a->frame_timing.decoded_ms : 0, ph_metrics_now_ms());
}

static void clear_view(app *a)
{
    ph_gesture_cancel(&a->gesture, SDL_GetTicks64());
    reset_mouse_input(a);
    a->feed_mode = a->feed_have_wheel = false;
    av_frame_unref(a->view_frame);
    av_frame_unref(a->pointer_baseline);
    av_frame_unref(a->pointer_reference);
    av_frame_unref(a->pointer_after);
    a->after_sync = 0;
    a->picture_w = a->picture_h = 0;
    a->presentation_mode = false;
    a->portrait_aspect = 0;
    ph_video_crop_reset(&a->crop);
    a->view_seen_at = a->candidate_since = 0;
    a->checked_sync = a->candidate_sync = 0;
    a->checked_found = false;
    memset(&a->frame_timing, 0, sizeof a->frame_timing);
    a->input_pending = false;
}

static void handle_app_event(app *a, const SDL_Event *e, AVFrame *frame, uint64_t *seq)
{
    switch ((int)(e->type - a->ev_base)) {
    case EV_FRAME:
        atomic_store(&a->frame_pending, false);
        if (ph_decoder_latest_timed(a->dec, frame, seq, &a->frame_timing)) {
            show_frame(a, frame);
            av_frame_unref(frame);
        }
        break;
    case EV_CONNECTED:
        clear_view(a);
        ph_pointer_reset(&a->pointer, SDL_GetTicks64());
        ph_scroll_reset(&a->wheel);
        snprintf(a->phone_name, sizeof a->phone_name, "%s", (const char *)e->user.data1);
        ph_decoder_reset(a->dec);
        ph_pointer_set_enabled(&a->pointer, a->input.enabled, SDL_GetTicks64());
        update_title(a);
        free(e->user.data1);
        break;
    case EV_DISCONNECTED:
        release_everything(a);
        clear_view(a);
        ph_decoder_reset(a->dec);
        ph_pointer_set_enabled(&a->pointer, false, SDL_GetTicks64());
        a->phone_name[0] = '\0';
        pthread_mutex_lock(&a->src_lock);
        ph_src_switch_wifi_lost(&a->src, ph_metrics_now_ms());
        pthread_mutex_unlock(&a->src_lock);
        if (a->polish)
            ph_polish_disconnected(a->polish);
        ph_window_clear(a->win);
        update_title(a);
        break;
    case EV_BLE:
        handle_ble_status(a, (const char *)e->user.data1);
        free(e->user.data1);
        break;
    default:
        break;
    }
}

static void start_bluetooth(app *a)
{
#ifdef _WIN32
    char *argv[] = {(char *)a->opt->ble_helper, NULL};
#else
    char *argv[] = {"python3", "-u", (char *)a->opt->ble_helper, NULL};
#endif

    if (!a->opt->bluetooth || !a->opt->ble_helper)
        return;
    a->ble_retry_at = 0;
#ifndef _WIN32
    setenv("PORTHOLE_BLE_NAME", a->opt->name, 1);
    if (a->opt->absolute_pointer)
        setenv("PORTHOLE_HID_ABSOLUTE", "replace", 1);
    else
        unsetenv("PORTHOLE_HID_ABSOLUTE");
#endif
    snprintf(a->ble_status, sizeof a->ble_status, "starting");
    a->link = ph_hid_link_start(argv, on_ble_status, a);
    if (!a->link) {
        PH_LOGW("continuing without Bluetooth control (view only)");
        a->ble_retry_at = SDL_GetTicks64() + BLE_RETRY_MS;
    }
}

/* Returns false when the app should quit. */
static bool handle_event(app *a, const SDL_Event *e, AVFrame *frame, uint64_t *seq)
{
    unsigned int win_id = ph_window_id(a->win);

    switch (e->type) {
    case SDL_QUIT:
        return false;
    case SDL_WINDOWEVENT:
        if (e->window.windowID != win_id)
            break;
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            e->window.event == SDL_WINDOWEVENT_EXPOSED)
            ph_window_redraw(a->win);
        else if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            a->input_pending = false;
            PH_LOGD("pointer cancel: window focus lost");
            if (a->setup.calibrating) {
                ph_setup_abort(&a->setup, PH_SETUP_CANCELED);
                finish_calibration(a, false);
                ph_window_set_setup(a->win, &a->setup);
            }
            ph_input_release_all(&a->input);
            cancel_control_intent(a, SDL_GetTicks64());
            memset(a->gesture_keys, 0, sizeof a->gesture_keys);
            SDL_CaptureMouse(SDL_FALSE);
        } else if (e->window.event == SDL_WINDOWEVENT_LEAVE) {
            PH_LOGD("pointer cancel: mouse left window");
            if (a->setup.calibrating)
                break; /* calibration is autonomous; the mouse may be outside the picture */
            if (a->absolute && a->abs.held)
                break; /* a captured absolute drag continues outside the window */
            cancel_control_intent(a, SDL_GetTicks64());
            SDL_CaptureMouse(SDL_FALSE);
        }
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (e->key.windowID == win_id)
            handle_key(a, &e->key);
        break;
    case SDL_MOUSEWHEEL:
        if (e->wheel.windowID == win_id)
            scroll(a, &e->wheel);
        break;
    case SDL_MOUSEMOTION:
        if (e->motion.windowID == win_id)
            mouse_motion(a, &e->motion);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (e->button.windowID == win_id) {
            if (a->setup.visible) {
                if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT)
                    setup_action(a, ph_window_setup_click(a->win, e->button.x, e->button.y));
            } else {
                mouse_button(a, &e->button);
            }
        }
        break;
    default:
        if (e->type >= a->ev_base && e->type < a->ev_base + EV_COUNT)
            handle_app_event(a, e, frame, seq);
        break;
    }
    return true;
}

#ifdef PH_USB_HAVE_LIBUSB
static void usb_pause(app *a, int ms)
{
    int left = ms;

    while (left > 0 && !atomic_load(&a->usb_stop)) {
        int step = left > 50 ? 50 : left;
        SDL_Delay((Uint32)step);
        left -= step;
    }
}

static void usb_bad_sample(void)
{
    static atomic_int logged;

    if (atomic_exchange(&logged, 1) == 0)
        PH_LOGW("a USB video sample was not Annex-B or length-prefixed NALs");
}

static void take_usb_sample(app *a, const ph_usb_sample *sample, uint8_t *annexb, size_t cap)
{
    size_t n;
    ph_src before, after;

    if (!sample->video) {
        static atomic_int logged;
        if (atomic_exchange(&logged, 1) == 0)
            PH_LOGI("USB audio stays on the phone; the PC plays Wi-Fi audio");
        return;
    }
    if (!sample->data || sample->len == 0)
        return;
    n = ph_usb_to_annexb(sample->data, sample->len, annexb, cap);
    if (n == 0) {
        usb_bad_sample();
        return;
    }
    pthread_mutex_lock(&a->src_lock);
    before = a->src.current;
    after = ph_src_switch_usb_frame(&a->src, ph_usb_annexb_keyframe(annexb, n), ph_metrics_now_ms());
    pthread_mutex_unlock(&a->src_lock);
    if (before != PH_SRC_USB && after == PH_SRC_USB) {
        if (a->polish)
            ph_polish_usb_present(a->polish, true);
        ph_decoder_reset(a->dec);
        PH_LOGI("showing the USB picture");
    }
    if (after != PH_SRC_USB)
        return;
    if (a->polish && !ph_polish_accepts_usb(a->polish))
        return;
    if (a->polish)
        ph_polish_on_video(a->polish, annexb, n, 0);
    ph_decoder_push(a->dec, annexb, n);
}

static void usb_unplugged(app *a)
{
    ph_src after;

    pthread_mutex_lock(&a->src_lock);
    after = ph_src_switch_usb_unplugged(&a->src, ph_metrics_now_ms());
    pthread_mutex_unlock(&a->src_lock);
    if (a->polish)
        ph_polish_usb_present(a->polish, false);
    if (a->dec)
        ph_decoder_reset(a->dec);
    if (after == PH_SRC_WIFI)
        PH_LOGI("USB unplugged; showing Wi-Fi");
    else
        PH_LOGI("USB unplugged");
}

#endif

static void *usb_thread_main(void *user)
{
    app *a = user;

#ifndef PH_USB_HAVE_LIBUSB
    (void)a;
    PH_LOGI("USB screen capture needs libusb-1.0");
    return NULL;
#else
    uint8_t *copy_buf = malloc(PH_USB_FRAME_MAX);
    uint8_t *annexb = malloc(PH_USB_FRAME_MAX);
    bool announced = false;
    bool wrote_inf = false;

    if (!copy_buf || !annexb) {
        free(copy_buf);
        free(annexb);
        PH_LOGE("couldn't allocate the USB screen buffer");
        return NULL;
    }
    while (!atomic_load(&a->usb_stop)) {
        ph_usb_capture *cap = ph_usb_libusb_open();
        if (!cap) {
            char path[1100];
            if (!announced) {
                PH_LOGI("waiting for an iPhone on USB");
                announced = true;
            }
            if (!wrote_inf && ph_config_path("porthole-winusb.inf", path, sizeof path) &&
                ph_usb_write_winusb_inf(path) == 0) {
                PH_LOGI("wrote a WinUSB driver for the capture interface: %s", path);
                wrote_inf = true;
            }
            usb_pause(a, 2000);
            continue;
        }
        pthread_mutex_lock(&a->src_lock);
        a->src.usb_unplugged = false;
        pthread_mutex_unlock(&a->src_lock);
        PH_LOGI("USB screen interface open");
        while (!atomic_load(&a->usb_stop)) {
            ph_usb_sample sample;
            int rc = ph_usb_capture_poll(cap, &sample, copy_buf, PH_USB_FRAME_MAX);
            if (atomic_load(&a->usb_stop))
                break;
            if (rc < 0) {
                usb_unplugged(a);
                break;
            }
            if (rc > 0)
                take_usb_sample(a, &sample, annexb, PH_USB_FRAME_MAX);
        }
        ph_usb_capture_close(cap);
    }
    free(copy_buf);
    free(annexb);
    return NULL;
#endif
}

int ph_app_run(const ph_app_options *opt)
{
    app a = {.opt = opt};
    a.input.output = (ph_input_output){&a, send_keys, send_mouse, send_consumer};
    a.pointer.output = (ph_pointer_output){&a, send_mouse, send_delay, send_pointer_sync};
    a.absolute = opt->absolute_pointer;
    ph_abs_init(&a.abs, (ph_abs_output){&a, send_abs});
    if (a.absolute)
        PH_LOGI("absolute pointer: the PC mouse position maps straight to the phone screen");
    ph_gesture_init(&a.gesture, &a.pointer);
    a.wheel.output = (ph_scroll_output){&a, send_mouse, send_delay, send_sync};
    a.wheel.position = position_for_scroll;
    a.wheel.feedback = true;
    char calibration[128], extra;
    double scale_x, scale_y;
    if (ph_config_read_line("pointer-scale", calibration, sizeof calibration)) {
        if (sscanf(calibration, "%lf %lf %c", &scale_x, &scale_y, &extra) == 2
            && ph_scroll_set_scale(&a.wheel, scale_x, scale_y)
            && ph_pointer_set_scale(&a.pointer, scale_x, scale_y))
            PH_LOGI("pointer calibration: x=%.3f y=%.3f", scale_x, scale_y);
        else
            PH_LOGW("invalid pointer calibration; using the default movement model");
    }
    ph_codec_advertise picture = ph_codec_advertise_for(opt->four_k);
    ph_receiver_options rx_opt = {
        .name = opt->name,
        .width = picture.width,
        .height = picture.height,
        .max_fps = picture.max_fps,
        .fixed_ports = opt->fixed_ports,
        .debug = opt->debug,
    };
    ph_receiver_callbacks rx_cb = {
        .user = &a,
        .video = on_video,
        .audio = on_audio,
        .audio_format = on_audio_format,
        .audio_volume = on_audio_volume,
        .audio_flush = on_audio_flush,
        .connected = on_connected,
        .disconnected = on_disconnected,
        .admit = on_admit,
    };
    AVFrame *frame = NULL;
    uint64_t seq = 0;
    int rc = 1;
    bool running = true;

    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        PH_LOGE("couldn't initialise SDL: %s", SDL_GetError());
        return 1;
    }
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);
    pthread_mutex_init(&a.src_lock, NULL);
    ph_src_switch_init(&a.src);
    a.ev_base = SDL_RegisterEvents(EV_COUNT);
    frame = av_frame_alloc();
    a.view_frame = av_frame_alloc();
    a.pointer_baseline = av_frame_alloc();
    a.pointer_reference = av_frame_alloc();
    a.pointer_after = av_frame_alloc();
    a.win = ph_window_create(PORTHOLE_NAME);
    a.dec = ph_decoder_start(on_frame, &a);
    if (opt->audio)
        a.audio = ph_audio_create();
    a.polish = ph_polish_create(&(ph_polish_options){.pin_required = opt->pin_required,
                                                     .autostart = opt->autostart,
                                                     .precise_touch = opt->precise_touch,
                                                     .exe_path = opt->exe_path});
    if (a.audio && opt->audio_delay_ms)
        ph_audio_set_sync_offset_ms(a.audio, opt->audio_delay_ms);
    if (a.polish)
        ph_polish_set_audio(a.polish, a.audio);
    if (a.ev_base == (Uint32)-1 || !frame || !a.view_frame || !a.pointer_baseline || !a.pointer_reference || !a.pointer_after
        || !a.win || !a.dec)
        goto done;
    if (a.polish)
        ph_polish_apply_saved(a.polish, SDL_GetWindowFromID(ph_window_id(a.win)));
    update_title(&a);
    if (opt->diagnostics)
        ph_window_toggle_diagnostics(a.win);
    char setup_done[16];
    if (opt->setup || (opt->auto_setup && !ph_config_read_line(setup_marker(&a), setup_done,
                                                              sizeof setup_done)))
        open_setup(&a);

    a.rx = ph_receiver_start(&rx_opt, &rx_cb);
    if (!a.rx)
        goto done;
    PH_LOGI("waiting for the iPhone: Control Center > Screen Mirroring > %s", opt->name);
    start_bluetooth(&a);
    update_title(&a);
    if (opt->usb && pthread_create(&a.usb_thread, NULL, usb_thread_main, &a) == 0)
        a.usb_started = true;
    else if (opt->usb)
        PH_LOGW("couldn't start USB screen capture");

    while (running && !stop_requested) {
        SDL_Event e;
        /* Button holds and video confirmation are timed in this loop: run it more
         * often while input is on its way to the phone. */
        bool busy = a.pointer.phase != PH_POINTER_IDLE || a.pointer.count
                    || a.wheel.in_flight || a.wheel.waiting_frame || ph_gesture_active(&a.gesture)
                    || (a.absolute && (ph_abs_busy(&a.abs) || a.abs.wheel != 0
                                       || (a.abs.want_valid && (a.abs.x != a.abs.want_x
                                                                || a.abs.y != a.abs.want_y))));
        if (SDL_WaitEventTimeout(&e, busy ? 4 : 16)) {
            do {
                running = handle_event(&a, &e, frame, &seq) && running;
            } while (SDL_PollEvent(&e));
        }
        ph_window_tick(a.win);
        if (a.absolute)
            ph_abs_tick(&a.abs, SDL_GetTicks64());
        if (a.audio)
            ph_audio_pump(a.audio, ph_metrics_now_ms());
        if (a.polish) {
            ph_polish_tick(a.polish, &a.input);
            if (ph_polish_quit(a.polish))
                running = false;
        }
        observe_pointer(&a);
        bool gesture_running = ph_gesture_active(&a.gesture);
        ph_gesture_tick(&a.gesture, a.picture_w, a.picture_h, SDL_GetTicks64());
        if (gesture_running && !ph_gesture_active(&a.gesture))
            PH_LOGD("gesture finished: result %d", (int)ph_gesture_get_result(&a.gesture));
        ph_pointer_tick(&a.pointer, SDL_GetTicks64());
        ph_scroll_tick(&a.wheel, SDL_GetTicks64());
        if (a.link)
            ph_hid_link_poll(a.link);
        tick_setup(&a);
        tick_metrics(&a);
        if (a.link && !a.link_stopping
            && (ph_pointer_transport_stalled(&a.pointer) || a.wheel.failed)) {
            PH_LOGW("%s acknowledgement timed out; restarting control",
                    ph_pointer_transport_stalled(&a.pointer) ? "pointer" : "scroll");
            ph_input_set_enabled(&a.input, false);
            ph_pointer_set_enabled(&a.pointer, false, SDL_GetTicks64());
            ph_hid_link_request_stop(a.link);
            a.link_stopping = true;
            snprintf(a.ble_status, sizeof a.ble_status, "recovering; retrying");
            a.ble_retry_at = 0; /* wait for the old pipe to exit before replacing it */
            update_title(&a);
        }
        ph_receiver_poll(a.rx);
        if (a.ble_retry_at && SDL_GetTicks64() >= a.ble_retry_at) {
            start_bluetooth(&a);
            update_title(&a);
        }
    }
    rc = 0;

done:
    if (a.usb_started) {
        atomic_store(&a.usb_stop, true);
        pthread_join(a.usb_thread, NULL);
    }
    pthread_mutex_destroy(&a.src_lock);
    if (a.calibration_saved)
        finish_calibration(&a, false);
    if (!a.pointer.failed
        && ph_pointer_ready(&a.pointer, a.pointer.target_x, a.pointer.target_y, a.pointer.aspect)
        && a.pointer.scale_x >= 0.05 && a.pointer.scale_x <= 20
        && a.pointer.scale_y >= 0.05 && a.pointer.scale_y <= 20) {
        char learned[96];
        snprintf(learned, sizeof learned, "%.6f %.6f", a.pointer.scale_x, a.pointer.scale_y);
        ph_config_write_line("pointer-scale", learned);
    }
    release_everything(&a);
    ph_hid_link_stop(a.link);
    ph_receiver_stop(a.rx); /* stop the network first so no more video arrives */
    if (a.polish && a.win)
        ph_polish_save_window(a.polish, SDL_GetWindowFromID(ph_window_id(a.win)));
    ph_polish_destroy(a.polish);
    ph_audio_destroy(a.audio);
    ph_decoder_stop(a.dec);
    discard_pending_events(&a); /* all event producers have stopped */
    ph_window_destroy(a.win);
    av_frame_free(&frame);
    av_frame_free(&a.view_frame);
    av_frame_free(&a.pointer_baseline);
    av_frame_free(&a.pointer_reference);
    av_frame_free(&a.pointer_after);
    SDL_Quit();
    return rc;
}
