#include "app.h"

#include <stdatomic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <libavutil/frame.h>

#include "airplay/receiver.h"
#include "audio/player.h"
#include "control/input.h"
#include "control/pointer.h"
#include "control/scroll.h"
#include "control/setup.h"
#include "core/config.h"
#include "core/log.h"
#include "core/metrics.h"
#include "core/version.h"
#include "hid/link.h"
#include "ui/window.h"
#include "video/crop.h"
#include "video/cursor.h"
#include "video/decoder.h"
#include "video/scrollbar.h"

#define BLE_RETRY_MS 5000

enum { EV_FRAME, EV_CONNECTED, EV_DISCONNECTED, EV_BLE, EV_COUNT };

typedef struct app {
    const ph_app_options *opt;
    ph_window *win;
    ph_decoder *dec;
    ph_receiver *rx;
    ph_audio *audio;
    ph_hid_link *link;
    bool link_stopping;
    Uint32 ev_base;
    atomic_bool frame_pending; /* coalesces "new frame" events */
    double portrait_aspect;    /* phone screen width/height when upright; 0 until seen */

    char phone_name[128];      /* from the mirroring session; empty when not mirroring */
    char ble_status[64];       /* last state reported by the Bluetooth helper */
    ph_input input;
    ph_pointer pointer;
    ph_scroll wheel;
    AVFrame *view_frame, *pointer_baseline;
    ph_rect view_screen, baseline_screen;
    int picture_w, picture_h;
    Uint64 view_seen_at, candidate_since;
    uint64_t view_serial, checked_serial;
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
} app;

/* ------------------------------------------------------------ cross-thread notifications */

static void push_event(app *a, int which, void *data)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = a->ev_base + (Uint32)which;
    e.user.data1 = data;
    if (SDL_PushEvent(&e) != 1)
        free(data);
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
    if (!atomic_exchange(&a->frame_pending, true))
        push_event(a, EV_FRAME, NULL);
}

static void on_video(void *user, const uint8_t *data, size_t len, uint64_t timestamp_ns)
{
    app *a = user;
    (void)timestamp_ns; /* shown as soon as decoded: lowest latency */
    ph_decoder_push(a->dec, data, len);
}

static void on_connected(void *user, const char *name, const char *model)
{
    (void)model;
    push_event(user, EV_CONNECTED, strdup(name && *name ? name : "iPhone"));
}

static void on_audio(void *user, unsigned char compression, const uint8_t *data, size_t len)
{
    app *a = user;
    ph_audio_push(a->audio, compression, data, len);
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
        snprintf(title, sizeof title, "%s - %s  [Bluetooth control: %s]", PORTHOLE_NAME,
                 a->phone_name, control);
    else
        snprintf(title, sizeof title,
                 "%s - on your iPhone: Control Center > Screen Mirroring > %s", PORTHOLE_NAME,
                 a->opt->name);
    ph_window_set_title(a->win, title);
    ph_window_set_status(a->win, a->phone_name[0] != 0, !strcmp(control, "on"), control);
    ph_setup_update(&a->setup, a->input.enabled,
                     a->phone_name[0] && a->view_frame && a->view_frame->data[0]);
    ph_window_set_setup(a->win, &a->setup);
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
    ph_pointer_set_enabled(&a->pointer, a->input.enabled && a->phone_name[0], SDL_GetTicks64());
    if (!a->input.enabled)
        ph_scroll_reset(&a->wheel);
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

static void send_mouse(void *user, const ph_hid_mouse *mouse)
{
    app *a = user;
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
    if (!a->link || !a->input.enabled || !a->phone_name[0] || a->input.buttons || a->setup.visible)
        return false;
    ph_window_video_size(a->win, &width, &height);
    if (width <= 0 || height <= 0 || !ph_window_to_video(a->win, x, y, nx, ny))
        return false;
    *aspect = (double)width / height;
    return true;
}

static void mouse_motion(app *a, const SDL_MouseMotionEvent *event)
{
    double x, y, aspect;
    if (pointer_coordinates(a, event->x, event->y, &x, &y, &aspect)) {
        mark_input(a, event->timestamp);
        /* Moving to a new location replaces wheel work aimed at the old location. */
        double dx = x - a->wheel.target_x, dy = (y - a->wheel.target_y) / aspect;
        if (dx * dx + dy * dy > 0.0001)
            ph_scroll_cancel(&a->wheel);
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
    uint8_t button = event->button == SDL_BUTTON_LEFT ? PH_HID_BTN_LEFT
                     : event->button == SDL_BUTTON_RIGHT ? PH_HID_BTN_RIGHT : 0;
    if (!button)
        return;
    if (!pointer_coordinates(a, event->x, event->y, &x, &y, &aspect)) {
        a->input_pending = false;
        if (event->type == SDL_MOUSEBUTTONUP) {
            ph_pointer_cancel(&a->pointer, SDL_GetTicks64());
            SDL_CaptureMouse(SDL_FALSE);
        }
        return;
    }
    mark_input(a, event->timestamp);
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
    if (!a->link || !a->input.enabled || !a->phone_name[0] || a->input.buttons || a->setup.visible
        || a->pointer.requested_buttons || a->pointer.buttons)
        return;
#if SDL_VERSION_ATLEAST(2, 26, 0)
    x = event->mouseX;
    y = event->mouseY;
#else
    SDL_GetMouseState(&x, &y);
#endif
    ph_window_video_size(a->win, &width, &height);
    if (width > 0 && height > 0 && ph_window_to_video(a->win, x, y, &nx, &ny)) {
        mark_input(a, event->timestamp);
        int sign = event->direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
        PH_LOGD("wheel at PC (%d,%d), phone (%.3f,%.3f)", x, y, nx, ny);
        ph_scroll_request(&a->wheel, nx, ny, (double)width / height,
                          sign * event->x, sign * event->y);
    } else {
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
        if (ph_setup_start_calibration(&a->setup, SDL_GetTicks64())) {
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
    if (sc == SDL_SCANCODE_F2) {
        if (e->type == SDL_KEYDOWN && !e->repeat) {
            a->input_pending = false;
            if (a->setup.visible)
                setup_action(a, PH_SETUP_ACTION_CLOSE);
            else
                open_setup(a);
        }
        return;
    }
    if (sc == SDL_SCANCODE_F3) {
        if (e->type == SDL_KEYDOWN && !e->repeat)
            ph_window_toggle_diagnostics(a->win);
        return;
    }
    if (sc == SDL_SCANCODE_F1) {
        if (e->type == SDL_KEYDOWN && !e->repeat) {
            a->input_pending = false;
            if (a->setup.visible)
                setup_action(a, PH_SETUP_ACTION_CLOSE);
            PH_LOGD("pointer cancel: help toggled");
            ph_pointer_cancel(&a->pointer, SDL_GetTicks64());
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
    if (e->type == SDL_KEYDOWN) {
        ph_scroll_cancel(&a->wheel); /* a new action cancels queued wheel batches */
        ph_scroll_forget_bounds(&a->wheel);
        if ((e->keysym.mod & KMOD_LALT) || sc == SDL_SCANCODE_LALT) {
            PH_LOGD("pointer cancel: Alt shortcut");
            ph_pointer_cancel(&a->pointer, SDL_GetTicks64());
            SDL_CaptureMouse(SDL_FALSE);
        }
    }
    if (a->phone_name[0] && (unsigned)sc < 256) {
        mark_input(a, e->timestamp);
        ph_input_key(&a->input, (uint8_t)sc, e->type == SDL_KEYDOWN, e->repeat != 0,
                     pc_modifiers((SDL_Keymod)e->keysym.mod));
    }
}

static void release_everything(app *a)
{
    ph_input_release_all(&a->input);
    ph_pointer_reset(&a->pointer, SDL_GetTicks64());
    ph_scroll_reset(&a->wheel);
    SDL_CaptureMouse(SDL_FALSE);
}

/* AirPlay can stop sending frames once the pointer is still. Revisit the latest
 * decoded image after the HID ACK rather than requiring another encoded frame. */
static void observe_pointer(app *a)
{
    const AVFrame *f = a->view_frame;
    Uint64 now = SDL_GetTicks64();
    double hx, hy, radius;
    if (!f || !f->data[0] || a->pointer.phase != PH_POINTER_WAIT_VIDEO
        || now - a->pointer.wait_since < 32
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
        }
        a->checked_sync = a->pointer.sync_id;
        a->checked_serial = a->view_serial;
        a->checked_found = found;
        a->checked_x = px;
        a->checked_y = py;
    }
    if (!a->checked_found)
        return;
    double dx = a->checked_x - a->candidate_x, dy = a->checked_y - a->candidate_y;
    if (a->candidate_sync != a->pointer.sync_id || dx * dx + dy * dy > 4) {
        a->candidate_sync = a->pointer.sync_id;
        a->candidate_since = now;
    }
    a->candidate_x = a->checked_x;
    a->candidate_y = a->checked_y;
    if (now - a->candidate_since < 32)
        return;
    PH_LOGD("phone pointer observed at (%.1f,%.1f)", a->candidate_x, a->candidate_y);
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
    ph_rect screen = ph_video_content_rect(&planes, a->portrait_aspect);
    if (screen.w != a->picture_w || screen.h != a->picture_h) {
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
    ph_metrics_frame(&a->metrics, a->frame_timing.valid ? a->frame_timing.received_ms : 0,
                      a->frame_timing.valid ? a->frame_timing.decoded_ms : 0, ph_metrics_now_ms());
}

static void clear_view(app *a)
{
    av_frame_unref(a->view_frame);
    av_frame_unref(a->pointer_baseline);
    a->picture_w = a->picture_h = 0;
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
            ph_pointer_cancel(&a->pointer, SDL_GetTicks64());
            ph_scroll_cancel(&a->wheel);
            SDL_CaptureMouse(SDL_FALSE);
        } else if (e->window.event == SDL_WINDOWEVENT_LEAVE) {
            PH_LOGD("pointer cancel: mouse left window");
            if (a->setup.calibrating)
                break; /* calibration is autonomous; the mouse may be outside the picture */
            ph_pointer_cancel(&a->pointer, SDL_GetTicks64());
            ph_scroll_cancel(&a->wheel);
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

int ph_app_run(const ph_app_options *opt)
{
    app a = {.opt = opt};
    a.input.output = (ph_input_output){&a, send_keys, send_mouse, send_consumer};
    a.pointer.output = (ph_pointer_output){&a, send_mouse, send_delay, send_pointer_sync};
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
    ph_receiver_options rx_opt = {
        .name = opt->name,
        .width = 1920,
        .height = 1080,
        .max_fps = 60,
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
    a.ev_base = SDL_RegisterEvents(EV_COUNT);
    frame = av_frame_alloc();
    a.view_frame = av_frame_alloc();
    a.pointer_baseline = av_frame_alloc();
    a.win = ph_window_create(PORTHOLE_NAME);
    a.dec = ph_decoder_start(on_frame, &a);
    if (opt->audio)
        a.audio = ph_audio_create();
    if (a.ev_base == (Uint32)-1 || !frame || !a.view_frame || !a.pointer_baseline
        || !a.win || !a.dec)
        goto done;
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

    while (running) {
        SDL_Event e;
        /* Button holds and video confirmation are timed in this loop: run it more
         * often while input is on its way to the phone. */
        bool busy = a.pointer.phase != PH_POINTER_IDLE || a.pointer.count
                    || a.wheel.in_flight || a.wheel.waiting_frame;
        if (SDL_WaitEventTimeout(&e, busy ? 4 : 16)) {
            do {
                running = handle_event(&a, &e, frame, &seq) && running;
            } while (SDL_PollEvent(&e));
        }
        ph_window_tick(a.win);
        observe_pointer(&a);
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
    if (a.calibration_saved)
        finish_calibration(&a, false);
    if (a.pointer.observed_at && a.pointer.known && !a.pointer.failed
        && a.pointer.phase == PH_POINTER_IDLE
        && a.pointer.scale_x >= 0.05 && a.pointer.scale_x <= 20
        && a.pointer.scale_y >= 0.05 && a.pointer.scale_y <= 20) {
        char learned[96];
        snprintf(learned, sizeof learned, "%.6f %.6f", a.pointer.scale_x, a.pointer.scale_y);
        ph_config_write_line("pointer-scale", learned);
    }
    release_everything(&a);
    ph_hid_link_stop(a.link);
    ph_receiver_stop(a.rx); /* stop the network first so no more video arrives */
    ph_audio_destroy(a.audio);
    ph_decoder_stop(a.dec);
    discard_pending_events(&a); /* all event producers have stopped */
    ph_window_destroy(a.win);
    av_frame_free(&frame);
    av_frame_free(&a.view_frame);
    av_frame_free(&a.pointer_baseline);
    SDL_Quit();
    return rc;
}
