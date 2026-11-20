#include "app.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

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
#include "files/hostclip.h"
#include "files/inbox.h"
#include "files/origin.h"
#include "files/picture_hash.h"
#include "broadcast/screen.h"
#include "files/pull.h"
#include "files/xdnd.h"
#include "ui/drag_ghost.h"
#include "ui/transfer_card.h"
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

enum { EV_FRAME, EV_CONNECTED, EV_DISCONNECTED, EV_BLE, EV_INBOX, EV_INBOX_NOTE, EV_COUNT };

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
    Uint64 keyboard_lift_at; /* non-zero: release the keyboard-toggle key at this time */
    ph_scroll wheel;
    ph_mouse_input mouse_input;
    bool wheel_point_valid;
    double wheel_point_x, wheel_point_y, wheel_point_aspect;
    ph_gesture gesture;
    bool feed_mode, feed_have_wheel;
    bool gesture_keys[256];
    Uint64 feed_wheel_at;
    AVFrame *view_frame, *pointer_baseline, *pointer_reference, *pointer_after;
    /* The picture under the press. A drag inside the picture swipes the phone,
     * so the frame on screen when the drag leaves can be a different photo. */
    AVFrame *pull_frame;
    ph_rect pull_picture; /* what the phone was asked about, for the drag card */
    int pull_px, pull_py;
    bool pull_snap;
    /* A bare photo or video that iOS sends instead of the phone screen is already the
     * whole picture; edge finding is only for thumbnails on the phone layout. */
    bool pull_presentation;
    ph_rect pull_screen;
    /* The phone's own screen from the app's broadcast extension, preferred over
     * AirPlay video like USB is. */
    ph_screen *screen;
    ph_decoder *screen_dec; /* its own decoder, so AirPlay's keeps its place underneath */
    uint64_t screen_seq;
    atomic_bool screen_on, airplay_on;
    /* A press on one photo is held back from the phone. A quick move is replayed
     * as a swipe when the button comes up. Held still, then dragged, it lifts
     * that photo onto the PC. The finger stays up until then, so Photos cannot
     * shrink the picture and leave it in a corner. */
    double press_phone_x, press_phone_y; /* where the held left press began on the phone */
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
    ph_pull pull;
    ph_drag_ghost *ghost;
    ph_transfer_card *card;
    int place_x, place_y;
    Uint64 place_at;
    ph_origin *origin;
    Uint64 clip_poll_at;
    char pull_dir[640];
    /* Original file for this drag; empty while the phone is looking. */
    char pull_path[1 << 16];
    /* Files shared to Porthole from the iPhone's Share sheet; they go to Downloads. */
    ph_inbox *inbox;
    int pull_note;
    bool pull_hold; /* button is up; wait for the phone before offering the file */
    Uint64 pull_hold_at;
    int hold_gx, hold_gy; /* where a file drag was let go before the file came */
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
    if (!show) {
        /* While the broadcast is the picture, leave AirPlay undecoded. A second
         * 1920x1080 stream is what makes the PC lag. It resumes at the next keyframe. */
        return;
    }
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
    app *a = user;
    (void)model;
    atomic_store(&a->airplay_on, true);
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
    app *a = user;
    on_audio_flush(user);
    atomic_store(&a->airplay_on, false);
    if (!atomic_load(&a->screen_on)) /* the broadcast keeps the phone connected */
        push_event(user, EV_DISCONNECTED, NULL);
}

static bool screen_allow(void *user, const char *address)
{
    app *a = user;
    char phone[64];
    return ph_origin_peer(a->origin, phone, sizeof phone) && strcmp(phone, address) == 0;
}

static void inbox_progress(void *user, int done, int total)
{
    char line[96];

    snprintf(line, sizeof line, done < total ? "Receiving %d of %d from the iPhone"
                                             : "Received %d of %d from the iPhone",
             done, total);
    push_event(user, EV_INBOX_NOTE, strdup(line));
}

static void inbox_received(void *user, const char *paths, int count)
{
    (void)count;
    push_event(user, EV_INBOX, strdup(paths));
}

static void screen_connected(void *user, const char *address)
{
    app *a = user;
    (void)address;
    /* Made on the first broadcast, on this same thread that then feeds it; screen_on
     * is set after, so the main thread only reads it once it exists. */
    if (!a->screen_dec) {
        a->screen_dec = ph_decoder_start(on_frame, a);
        /* Same longer side as AirPlay's portrait mirror. The broadcast arrives
         * near 888x1920, and drawing that on this machine falls behind. */
        ph_decoder_limit_edge(a->screen_dec, 1080);
    }
    if (!a->screen_dec)
        return;
    ph_decoder_reset(a->dec);
    atomic_store(&a->screen_on, true);
    pthread_mutex_lock(&a->src_lock);
    a->src.usb_unplugged = false; /* the broadcast takes the cable's place */
    pthread_mutex_unlock(&a->src_lock);
    if (!atomic_load(&a->airplay_on))
        push_event(user, EV_CONNECTED, strdup("iPhone (screen broadcast)"));
}

static void screen_video(void *user, const uint8_t *annexb, size_t len, bool keyframe)
{
    app *a = user;
    ph_src before, after;

    pthread_mutex_lock(&a->src_lock);
    before = a->src.current;
    after = ph_src_switch_usb_frame(&a->src, keyframe, ph_metrics_now_ms());
    pthread_mutex_unlock(&a->src_lock);
    if (before != PH_SRC_USB && after == PH_SRC_USB)
        PH_LOGI("showing the broadcast screen");
    if (after != PH_SRC_USB || !a->screen_dec)
        return;
    if (a->polish)
        ph_polish_on_video(a->polish, annexb, len, 0);
    ph_decoder_push(a->screen_dec, annexb, len);
}

static void screen_disconnected(void *user)
{
    app *a = user;

    atomic_store(&a->screen_on, false);
    pthread_mutex_lock(&a->src_lock);
    ph_src_switch_usb_unplugged(&a->src, ph_metrics_now_ms());
    pthread_mutex_unlock(&a->src_lock);
    if (a->screen_dec)
        ph_decoder_reset(a->screen_dec); /* the next broadcast starts clean */
    if (a->dec)
        ph_decoder_reset(a->dec); /* AirPlay was not decoded during the broadcast */
    if (!atomic_load(&a->airplay_on))
        push_event(user, EV_DISCONNECTED, NULL);
}

static void on_ble_status(void *user, const char *line)
{
    push_event(user, EV_BLE, strdup(line));
}

/* ------------------------------------------------------------ main thread */

static void update_title(app *a)
{
    char title[320];
    const char *control = a->opt->bluetooth && a->opt->ble_helper ? a->ble_status : "off";

    /* The relative pointer has to see the AssistiveTouch dot, so a photo or
     * video that is not the phone screen pauses it. The absolute pointer does
     * not: a click is that spot on the phone. */
    bool layout_pauses_pointer = a->presentation_mode && !a->absolute;
    if (a->phone_name[0])
        snprintf(title, sizeof title, "%s - %s  [Bluetooth control: %s]%s", PORTHOLE_NAME,
                 a->phone_name, control, layout_pauses_pointer ? " [Different display layout]"
                                       : a->feed_mode ? " [Feed wheel]" : "");
    else
        snprintf(title, sizeof title,
                 "%s - Connect in the iPhone app, then tap Share screen", PORTHOLE_NAME);
    {
        char pin[8];
        size_t used = strlen(title);
        if (a->polish && ph_polish_copy_pin(a->polish, pin, sizeof pin) && used + 1 < sizeof title)
            snprintf(title + used, sizeof title - used, "  PIN %s", pin);
    }
    ph_window_set_title(a->win, title);
    ph_window_set_status(a->win, a->phone_name[0] != 0,
                         !layout_pauses_pointer && !strcmp(control, "on"),
                         layout_pauses_pointer ? "Return to phone view" : control);
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
    bool was_on = !strcmp(a->ble_status, "on");

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
    if (!strcmp(line, "subscribed") && !was_on && a->input.enabled)
        PH_LOGI("Bluetooth keyboard ready. Alt+K shows or hides the iPhone keyboard");
    ph_pointer_set_enabled(&a->pointer, a->input.enabled && a->phone_name[0]
                            && !a->presentation_mode, SDL_GetTicks64());
    if (!a->input.enabled) {
        ph_gesture_cancel(&a->gesture, SDL_GetTicks64());
        reset_mouse_input(a);
        ph_scroll_reset(&a->wheel);
        a->feed_mode = a->feed_have_wheel = false;
        a->keyboard_lift_at = 0;
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

/* Absolute mode: window position -> phone picture fraction, including a photo
 * or video whose shape is not the phone screen. */
static bool abs_coordinates(app *a, int x, int y, double *nx, double *ny)
{
    if (!a->link || !a->input.enabled || !a->phone_name[0] || a->setup.visible)
        return false;
    return ph_window_to_video(a->win, x, y, nx, ny);
}

static bool on_picture(const app *a, int x, int y)
{
    double vx, vy;
    return ph_window_picture_point(a->win, x, y, &vx, &vy);
}

static bool ensure_pull_dir(app *a)
{
    char root[480];

    if (a->pull_dir[0])
        return true;
    if (!ph_config_dir(root, sizeof root))
        return false;
    if (snprintf(a->pull_dir, sizeof a->pull_dir, "%s/pull", root) >= (int)sizeof a->pull_dir)
        return false;
#ifdef _WIN32
    if (_mkdir(a->pull_dir) != 0 && errno != EEXIST)
        return false;
#else
    if (mkdir(a->pull_dir, 0700) != 0 && errno != EEXIST)
        return false;
#endif
    return true;
}

static uint64_t hash_rect(const AVFrame *f, ph_rect s)
{
    const uint8_t *plane;

    if (!f || !f->data[0] || s.w < 2 || s.h < 2)
        return 0;
    if (s.x < 0 || s.y < 0 || s.x + s.w > f->width || s.y + s.h > f->height)
        return 0;
    plane = f->data[0] + (size_t)s.y * (size_t)f->linesize[0] + (size_t)s.x;
    return ph_picture_dhash(plane, s.w, s.h, f->linesize[0]);
}

static bool thumb_rect(const AVFrame *f, ph_rect s,
                       uint8_t out[PH_PICTURE_THUMB * PH_PICTURE_THUMB])
{
    const uint8_t *plane;

    if (!f || !f->data[0] || s.w < 2 || s.h < 2)
        return false;
    if (s.x < 0 || s.y < 0 || s.x + s.w > f->width || s.y + s.h > f->height)
        return false;
    plane = f->data[0] + (size_t)s.y * (size_t)f->linesize[0] + (size_t)s.x;
    return ph_picture_thumb(plane, s.w, s.h, f->linesize[0], out) != 0;
}

static bool detail_rect(const AVFrame *f, ph_rect s,
                        uint8_t out[PH_PICTURE_DETAIL * PH_PICTURE_DETAIL])
{
    const uint8_t *plane;

    if (!f || !f->data[0] || s.w < 2 || s.h < 2)
        return false;
    if (s.x < 0 || s.y < 0 || s.x + s.w > f->width || s.y + s.h > f->height)
        return false;
    plane = f->data[0] + (size_t)s.y * (size_t)f->linesize[0] + (size_t)s.x;
    return ph_picture_cells(plane, s.w, s.h, f->linesize[0], PH_PICTURE_DETAIL, out) != 0;
}

static bool frame_planes(const AVFrame *f, ph_yuv_planes *planes)
{
    if (!f || !f->data[0] || !f->data[1] || !f->data[2])
        return false;
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P)
        return false;
    *planes = (ph_yuv_planes){f->data[0], f->data[1], f->data[2], f->linesize[0], f->linesize[1],
                              f->width, f->height};
    return true;
}

/* Paints a rect out of a luma picture: each pixel takes the picture just outside the
 * rect, from every side that has some, weighted by nearness. */
static void paint_out(uint8_t *px, int w, int h, ph_rect b)
{
    for (int y = b.y; y < b.y + b.h; y++) {
        for (int x = b.x; x < b.x + b.w; x++) {
            double sum = 0, weight = 0;

            if (b.y > 0)
                sum += px[(b.y - 1) * w + x] / (double)(y - b.y + 1), weight += 1.0 / (y - b.y + 1);
            if (b.y + b.h < h)
                sum += px[(b.y + b.h) * w + x] / (double)(b.y + b.h - y),
                    weight += 1.0 / (b.y + b.h - y);
            if (b.x > 0)
                sum += px[y * w + b.x - 1] / (double)(x - b.x + 1), weight += 1.0 / (x - b.x + 1);
            if (b.x + b.w < w)
                sum += px[y * w + b.x + b.w] / (double)(b.x + b.w - x),
                    weight += 1.0 / (b.x + b.w - x);
            if (weight > 0)
                px[y * w + x] = (uint8_t)(sum / weight + .5);
        }
    }
}

/* The thumb and detail cells of a picture on screen. Select mode's checkmark is not
 * part of the picture, so it is painted out first: left in, it alone can make the
 * right photo lose to a different one. */
static bool picture_samples(const AVFrame *f, ph_rect s, uint8_t thumb[PH_PICTURE_THUMB * PH_PICTURE_THUMB],
                            uint8_t detail[PH_PICTURE_DETAIL * PH_PICTURE_DETAIL], bool *have_detail)
{
    ph_yuv_planes planes;
    ph_rect badges[4];
    uint8_t *copy;
    int n;
    bool ok;

    *have_detail = false;
    if (!f || !f->data[0] || s.w < 2 || s.h < 2 || s.x < 0 || s.y < 0
        || s.x + s.w > f->width || s.y + s.h > f->height)
        return false;
    n = frame_planes(f, &planes) ? ph_video_badges_in(&planes, s, badges, 4) : 0;
    if (n == 0 || !(copy = malloc((size_t)s.w * (size_t)s.h))) {
        ok = thumb_rect(f, s, thumb);
        *have_detail = ok && detail_rect(f, s, detail);
        return ok;
    }
    for (int y = 0; y < s.h; y++)
        memcpy(copy + (size_t)y * s.w, f->data[0] + (size_t)(s.y + y) * f->linesize[0] + s.x, (size_t)s.w);
    for (int i = 0; i < n; i++) {
        badges[i].x -= s.x;
        badges[i].y -= s.y;
        paint_out(copy, s.w, s.h, badges[i]);
    }
    ok = ph_picture_thumb(copy, s.w, s.h, s.w, thumb) != 0;
    *have_detail = ok && ph_picture_cells(copy, s.w, s.h, s.w, PH_PICTURE_DETAIL, detail) != 0;
    free(copy);
    PH_LOGD("photo pull: painted out %d checkmark%s", n, n == 1 ? "" : "s");
    return ok;
}

/* Window point of the press, in the decoded frame. */
static bool press_video_pixel(const app *a, int *px, int *py)
{
    double vx, vy;
    ph_rect s = a->view_screen;

    if (!a->view_frame || s.w < 2 || s.h < 2)
        return false;
    if (!ph_window_picture_point(a->win, a->pull.press_x, a->pull.press_y, &vx, &vy))
        return false;
    *px = s.x + (int)(vx * (double)s.w);
    *py = s.y + (int)(vy * (double)s.h);
    if (*px < 0 || *py < 0 || *px >= a->view_frame->width || *py >= a->view_frame->height)
        return false;
    return true;
}

static void drop_pull_snapshot(app *a)
{
    a->pull_snap = false;
    if (a->pull_frame)
        av_frame_unref(a->pull_frame);
}

/* The photo under the armed press. A bare picture is that whole picture; a phone
 * layout uses the thumbnail or photo whose edges are black gaps. */
static bool pull_item(const app *a, ph_rect *out)
{
    ph_yuv_planes planes;
    ph_rect picture;

    if (!out || !a->pull_snap || !frame_planes(a->pull_frame, &planes))
        return false;
    if (a->pull_presentation && a->pull_screen.x >= 0 && a->pull_screen.y >= 0
        && a->pull_screen.x + a->pull_screen.w <= a->pull_frame->width
        && a->pull_screen.y + a->pull_screen.h <= a->pull_frame->height)
        picture = a->pull_screen;
    else
        picture = ph_video_item_rect(&planes, a->pull_px, a->pull_py);
    if (picture.w < 64 || picture.h < 64)
        return false;
    *out = picture;
    return true;
}

/* The home screen and a full-bleed wallpaper grow out to every edge. */
static bool fills_frame(ph_rect item, int fw, int fh)
{
    const int edge = 24;

    if (fw < 64 || fh < 64 || item.w < 64 || item.h < 64)
        return true;
    return item.x <= edge && item.y <= edge && item.x + item.w >= fw - edge
           && item.y + item.h >= fh - edge;
}

static void carry_ghost(app *a, ph_rect picture)
{
    int gx, gy;

    if (!a->ghost || !ph_drag_ghost_show(a->ghost, a->pull_frame, picture))
        return;
    ph_xdnd_skip(ph_drag_ghost_window(a->ghost));
    a->place_at = 0;
    SDL_GetGlobalMouseState(&gx, &gy);
    ph_drag_ghost_move(a->ghost, gx, gy);
    SDL_CaptureMouse(SDL_TRUE);
}

static void place_ghost(app *a, int gx, int gy)
{
    char name[96];
    char line[128];
    Uint64 now = SDL_GetTicks64();

    if (!a->ghost)
        return;
    ph_drag_ghost_move(a->ghost, gx, gy);
    if (a->pull_note == 2)
        return; /* still "Finding it on the iPhone..."; there is no file to place yet */
    if (a->place_at && gx == a->place_x && gy == a->place_y && now - a->place_at < 80)
        return;
    a->place_x = gx;
    a->place_y = gy;
    a->place_at = now;
    if (ph_xdnd_place(SDL_GetWindowFromID(ph_window_id(a->win)), gx, gy, name, sizeof name)) {
        snprintf(line, sizeof line, "Place in %s", name);
        ph_drag_ghost_caption(a->ghost, line, true);
    } else {
        ph_drag_ghost_caption(a->ghost, "Drag onto the desktop or a folder", false);
    }
}

static void hide_ghost(app *a)
{
    ph_drag_ghost_hide(a->ghost);
    ph_xdnd_skip(NULL);
}

/* Keeps the frame and point under a Ctrl press for begin_pull. The photo under the
 * pointer is taken when its edges can be found. Otherwise it is the whole picture:
 * a screenshot, a photo or video filling the screen, or the bare picture AirPlay sends. */
static void snap_pull(app *a)
{
    ph_rect item;

    drop_pull_snapshot(a);
    if (!a->pull.armed)
        return;
    if (!a->pull_frame || !a->view_frame || !a->view_frame->data[0]
        || !press_video_pixel(a, &a->pull_px, &a->pull_py)
        || av_frame_ref(a->pull_frame, a->view_frame) != 0) {
        a->pull.armed = false;
        return;
    }
    a->pull_snap = true;
    /* iOS sends the live screen as a portrait stream. A picture centred in a landscape
     * TV frame is media even when it has the phone's shape, such as a screenshot. */
    a->pull_presentation = a->presentation_mode
                           || (a->view_frame->width > a->view_frame->height
                               && a->view_screen.w + 16 < a->view_frame->width);
    a->pull_screen = a->view_screen;
    ph_pull_allow_inside(&a->pull);
    if (!a->pull_presentation
        && (!pull_item(a, &item)
            || fills_frame(item, a->pull_frame->width, a->pull_frame->height))) {
        ph_yuv_planes planes;
        ph_grid grid;

        /* Photos' grid of thumbnails: ask_pull takes the exact cell. */
        if (frame_planes(a->pull_frame, &planes)
            && ph_video_grid(&planes, a->pull_px, a->pull_py, &grid)) {
            PH_LOGD("photo pull: Ctrl+drag takes this thumbnail");
            return;
        }
        a->pull_presentation = true;
        PH_LOGD("photo pull: Ctrl+drag takes the whole screen picture");
        return;
    }
    PH_LOGD("photo pull: Ctrl+drag takes this picture");
}

static bool copy_file(const char *src, const char *dest)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    char buf[65536];
    size_t n;

    if (!in)
        return false;
    out = fopen(dest, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            remove(dest);
            return false;
        }
    }
    fclose(in);
    if (fclose(out) != 0) {
        remove(dest);
        return false;
    }
    return true;
}

static bool desktop_copy(const char *src, char *dest, size_t cap)
{
    const char *home = getenv("HOME");
    const char *base = strrchr(src, '/');

    if (!dest || cap < 2)
        return false;
    dest[0] = 0;
    if (!home || !home[0] || !base || !base[1])
        return false;
    if (snprintf(dest, cap, "%s/Desktop/%s", home, base + 1) >= (int)cap) {
        dest[0] = 0;
        return false;
    }
    if (!strcmp(src, dest))
        return true;
    if (!copy_file(src, dest)) {
        dest[0] = 0;
        return false;
    }
    return true;
}

/* Copies each file, one per line, to the Desktop. Returns how many arrived there.
 * first receives the path of the first one that landed. */
static int desktop_copy_all(char *paths, char *first, size_t first_cap)
{
    int copied = 0;

    if (first && first_cap)
        first[0] = 0;
    for (char *line = paths, *next; line && *line; line = next) {
        char dest[1024];

        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (desktop_copy(line, dest, sizeof dest)) {
            copied++;
            if (first && first_cap && !first[0])
                snprintf(first, first_cap, "%s", dest);
            PH_LOGI("photo pull: original on the Desktop (%s)", dest);
        } else {
            PH_LOGW("photo pull: original stayed at %s", line);
        }
    }
    return copied;
}

/* Moves each file, one per line, into Downloads without replacing anything there:
 * a name already taken gets 2-, 3-, ... in front. Returns how many arrived.
 * first receives the path of the first one that landed. */
static int downloads_move_all(char *paths, char *first, size_t first_cap)
{
    const char *home = getenv("HOME");
    char dir[1024];
    int moved = 0;

    if (first && first_cap)
        first[0] = 0;
    if (!home || !home[0] || snprintf(dir, sizeof dir, "%s/Downloads", home) >= (int)sizeof dir)
        return 0;
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif

    for (char *line = paths, *next; line && *line; line = next) {
        const char *base = strrchr(line, '/');
        char dest[1024];
        bool ok = false;

        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (!base || !base[1])
            continue;
        for (int n = 1; n < 1000 && !ok; n++) {
            FILE *taken;
            int len = n == 1 ? snprintf(dest, sizeof dest, "%s/%s", dir, base + 1)
                             : snprintf(dest, sizeof dest, "%s/%d-%s", dir, n, base + 1);

            if (len >= (int)sizeof dest)
                break;
            if ((taken = fopen(dest, "rb"))) {
                fclose(taken);
                continue;
            }
            ok = rename(line, dest) == 0 || (copy_file(line, dest) && remove(line) == 0);
            break;
        }
        if (ok) {
            moved++;
            if (first && first_cap && !first[0])
                snprintf(first, first_cap, "%s", dest);
            PH_LOGI("share: in Downloads: %s", dest);
        } else {
            PH_LOGW("share: stayed at %s", line);
        }
    }
    return moved;
}

static void end_pull(app *a)
{
    bool placed = false;
    bool landed = false;
    int note = a->pull_note;
    static char saved[sizeof ((app *)0)->pull_path];
    char shown[1024];
    const char *offer = NULL;

    shown[0] = 0;
    snprintf(saved, sizeof saved, "%s", a->pull_path);
    if (ph_xdnd_busy())
        placed = ph_xdnd_drop();
    if (saved[0] && note == 3 && !placed)
        landed = desktop_copy_all(saved, shown, sizeof shown) > 0;
    if (note == 3 && landed)
        offer = shown;
    else if (note == 3 && saved[0])
        offer = saved;
    ph_pull_release(&a->pull);
    drop_pull_snapshot(a);
    hide_ghost(a);
    a->pull_note = 0;
    a->pull_hold = false;
    a->pull_path[0] = 0;
    update_title(a);
    if (offer && a->card)
        ph_transfer_card_show(a->card, offer, 1, 1, landed, SDL_GetTicks64());
    if (note == 3)
        ph_window_set_detail(a->win, placed ? "Pulled the original photo"
                                            : "Original photo saved on the Desktop");
    else if (note == 4)
        ph_window_set_detail(a->win, "Not a photo or video from this iPhone's library");
    else if (note == 1)
        ph_window_set_detail(a->win, "Open Porthole on the iPhone, then drag the photo again");
    else if (note == 5)
        ph_window_set_detail(a->win, "Drag one photo, not the page around it");
    else if (note == 2)
        ph_window_set_detail(a->win, "iPhone did not answer");
}

static void start_file_drag(app *a)
{
    int gx, gy;

    if (!a->pull_path[0])
        return;
    if (ph_xdnd_busy()) {
        ph_xdnd_set_path(a->pull_path);
        return;
    }
    SDL_GetGlobalMouseState(&gx, &gy);
    if (a->pull_hold) { /* it lands where it was dropped */
        gx = a->hold_gx;
        gy = a->hold_gy;
    }
    if (ph_xdnd_begin(SDL_GetWindowFromID(ph_window_id(a->win)), a->pull_path))
        ph_xdnd_motion(gx, gy);
}

static bool ask_rect(app *a, ph_rect picture, bool with_screen);

/* Takes the phone's answer, before the drag or during it. */
static void note_origin(app *a)
{
    char path[768];
    ph_origin_result result;

    if ((!a->pull.pulling && !a->pull.armed) || a->pull_note != 2)
        return;
    result = ph_origin_take(a->origin, path, sizeof path);
    if (result != PH_ORIGIN_FILE && result != PH_ORIGIN_MISS)
        return;
    if (result == PH_ORIGIN_FILE)
        snprintf(a->pull_path, sizeof a->pull_path, "%s", path);
    if (a->pull_path[0]) {
        a->pull_note = 3;
        ph_window_set_detail(a->win, a->pull.pulling ? "Pulling the original file"
                                                     : "Found it. Drag it out of the window");
        if (!a->pull.pulling)
            return;
        start_file_drag(a);
        if (a->pull_hold)
            end_pull(a);
    } else {
        a->pull_note = 4;
        hide_ghost(a);
        ph_window_set_detail(a->win, "Not a photo or video from this iPhone's library");
        if (a->pull_hold)
            end_pull(a);
    }
}

static void track_pull(app *a)
{
    int gx, gy;

    if (!a->pull.pulling && !ph_xdnd_busy())
        return;
    if (a->pull_hold) {
        /* Dropped already: the card and the drop target stay where it was let go. */
        note_origin(a);
        return;
    }
    SDL_GetGlobalMouseState(&gx, &gy);
    ph_xdnd_motion(gx, gy);
    place_ghost(a, gx, gy);
    if (a->pull.pulling)
        SDL_CaptureMouse(SDL_TRUE);
    note_origin(a);
}

/* Asks the phone about one picture in the pull frame. with_screen also sends the
 * whole phone screen, for a picture whose edges may be wrong or missing. */
static bool ask_rect(app *a, ph_rect picture, bool with_screen)
{
    uint64_t hash;
    double aspect;
    uint8_t thumb[PH_PICTURE_THUMB * PH_PICTURE_THUMB];
    uint8_t detail[PH_PICTURE_DETAIL * PH_PICTURE_DETAIL];
    static ph_origin_screen screen;
    bool have_thumb, have_detail, have_screen;

    /* The phone hashes a 64-pixel copy, so a dHash of single video pixels is mostly
     * texture noise. Hashing the averaged thumb samples what the phone samples. */
    have_thumb = picture_samples(a->pull_frame, picture, thumb, detail, &have_detail);
    hash = have_thumb ? ph_picture_dhash(thumb, PH_PICTURE_THUMB, PH_PICTURE_THUMB, PH_PICTURE_THUMB)
                      : hash_rect(a->pull_frame, picture);
    aspect = (double)picture.w / (double)picture.h;
    /* On the phone's own portrait screen, also send the whole screen: the phone can
     * place each library item the way Photos shows it, which needs no edges at all. */
    have_screen = with_screen && have_detail && a->pull_screen.w > 1
                  && a->pull_screen.h > a->pull_screen.w
                  && thumb_rect(a->pull_frame, a->pull_screen, screen.thumb)
                  && detail_rect(a->pull_frame, a->pull_screen, screen.detail);
    screen.aspect = have_screen ? (double)a->pull_screen.w / (double)a->pull_screen.h : 0;
    if (!ensure_pull_dir(a)
        || !ph_origin_ask(a->origin, hash, aspect, have_thumb ? thumb : NULL,
                          have_detail ? detail : NULL, have_screen ? &screen : NULL,
                          a->pull_dir))
        return false;
    PH_LOGI("photo pull: item %dx%d aspect %.4f hash %016llx%s", picture.w, picture.h, aspect,
            (unsigned long long)hash, have_screen ? " (and the whole screen)" : "");
    return true;
}

/* Asks the phone about the picture under a Ctrl press at once, before any drag, so
 * the card only appears for something the phone has: the home screen or an app page
 * never shows one. One item per drag; several go through the iPhone's Share sheet to
 * Porthole, which saves them in Downloads. Nothing here reaches the phone's screen. */
static void ask_pull(app *a)
{
    ph_yuv_planes planes;
    ph_rect picture;

    a->pull_path[0] = 0;
    a->pull_hold = false;
    a->pull_picture = (ph_rect){0};
    ph_origin_take(a->origin, NULL, 0); /* a late reply belongs to the previous press */
    bool on_grid = false;
    ph_grid grid;

    if (!a->pull_snap || !frame_planes(a->pull_frame, &planes)) {
        a->pull_note = 5;
        ph_window_set_detail(a->win, "Nothing to take here");
        return;
    }
    /* In Photos' library grid the cell is exact, at any zoom, whatever the colours. */
    if (!a->pull_presentation && ph_video_grid(&planes, a->pull_px, a->pull_py, &grid)) {
        ph_rect cell = ph_grid_cell(&grid, a->pull_px, a->pull_py, a->pull_frame->width,
                                    a->pull_frame->height);
        if (cell.w > 0) {
            picture = cell;
            on_grid = true;
            PH_LOGD("photo pull: grid of %d columns, cell %dx%d", grid.columns, cell.w, cell.h);
        }
    }
    if (!on_grid && !pull_item(a, &picture)) {
        a->pull_note = 5;
        ph_window_set_detail(a->win, "Nothing to take here");
        PH_LOGW("photo pull: no picture under the press");
        return;
    }
    a->pull_picture = picture;
    if (!ask_rect(a, picture, !on_grid)) {
        a->pull_note = 1;
        ph_window_set_detail(a->win, "Open Porthole on the iPhone, then try again");
        PH_LOGW("photo pull: iPhone is not connected");
        return;
    }
    a->pull_note = 2;
    ph_window_set_detail(a->win, "Asking the iPhone for this photo");
}

/* The Ctrl press became a drag. The card follows the pointer while the phone is still
 * looking or has the file; after a refusal there is nothing to carry. */
static void begin_pull(app *a)
{
    ph_abs_release_all(&a->abs, SDL_GetTicks64());
    ph_pointer_cancel(&a->pointer, SDL_GetTicks64());
    SDL_CaptureMouse(SDL_TRUE);
    note_origin(a);
    if (a->pull_note != 2 && a->pull_note != 3)
        return;
    carry_ghost(a, a->pull_picture);
    if (a->pull_note == 2)
        ph_drag_ghost_caption(a->ghost, "Finding it on the iPhone...", false);
    else
        start_file_drag(a);
}

/* True on the motion that turns a Ctrl press into a file drag. */
static bool file_pull_ready(app *a, int x, int y)
{
    return a->pull.armed && ph_pull_motion(&a->pull, on_picture(a, x, y), x, y);
}

static void follow_pull(app *a)
{
    int x, y;

    if (a->pull_hold && SDL_GetTicks64() - a->pull_hold_at > 300000) {
        PH_LOGW("photo pull: iPhone did not answer");
        end_pull(a);
    }
    if (a->pull.armed) {
        note_origin(a);
        SDL_GetMouseState(&x, &y);
        if (file_pull_ready(a, x, y))
            begin_pull(a);
    }
    if (a->pull.pulling || ph_xdnd_busy())
        track_pull(a);
    else if (!a->pull.armed)
        SDL_EventState(SDL_SYSWMEVENT, SDL_DISABLE);
}

static void finish_pull(app *a)
{
    note_origin(a);
    if (a->pull.pulling && a->pull_note == 2) {
        /* Dropped before the phone answered. The card goes at once, since most of these
         * are the home screen or a page the phone will refuse; a file that does come
         * lands where it was dropped. Escape gives up. */
        a->pull_hold = true;
        a->pull_hold_at = SDL_GetTicks64();
        SDL_GetGlobalMouseState(&a->hold_gx, &a->hold_gy);
        hide_ghost(a);
        ph_window_set_detail(a->win, "Checking with the iPhone... (Esc cancels)");
        return;
    }
    end_pull(a);
}

static void mouse_motion(app *a, const SDL_MouseMotionEvent *event)
{
    if (file_pull_ready(a, event->x, event->y))
        begin_pull(a);
    if (a->pull.pulling) {
        track_pull(a);
        return;
    }
    if (a->pull.armed)
        return; /* a Ctrl press waits for its drag and never reaches the phone */
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
    if (a->pull_hold)
        return;
    /* Plain clicks and drags always control the phone. Ctrl+drag takes the original
     * file of whatever is under the pointer, and that press never reaches the phone. */
    if (event->button == SDL_BUTTON_LEFT) {
        if (event->type == SDL_MOUSEBUTTONDOWN) {
            bool lift = (SDL_GetModState() & KMOD_CTRL) && !a->setup.visible
                        && !ph_window_help_visible(a->win);

            ph_pull_press(&a->pull, lift, on_picture(a, event->x, event->y), event->x, event->y);
            snap_pull(a);
            if (a->pull.armed) {
                if (a->absolute && (a->abs.held || a->abs.pending || a->abs.buttons))
                    ph_abs_release_all(&a->abs, SDL_GetTicks64());
                mark_input(a, event->timestamp);
                SDL_CaptureMouse(SDL_TRUE);
                ask_pull(a);
                return;
            }
        } else if (file_pull_ready(a, event->x, event->y)) {
            begin_pull(a);
            finish_pull(a);
            return;
        } else if (a->pull.pulling) {
            finish_pull(a);
            return;
        } else if (a->pull.armed) {
            /* A Ctrl click without a drag: nothing reaches the phone. A late answer is
             * dropped by the next ask. */
            ph_pull_release(&a->pull);
            drop_pull_snapshot(a);
            SDL_CaptureMouse(SDL_FALSE);
            a->pull_note = 0;
            a->pull_path[0] = 0;
            return;
        } else {
            ph_pull_release(&a->pull);
            drop_pull_snapshot(a);
        }
    }
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
        if (down && event->button == SDL_BUTTON_LEFT) {
            a->press_phone_x = x;
            a->press_phone_y = y;
        }
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
    if (ph_window_help_visible(a->win)) {
        if (down && sc == SDL_SCANCODE_ESCAPE && !e->repeat) {
            a->gesture_keys[sc] = true;
            ph_window_toggle_help(a->win);
        }
        return;
    }
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
    if (down && sc == SDL_SCANCODE_ESCAPE && (a->pull.armed || a->pull.pulling || a->pull_hold
        || ph_gesture_active(&a->gesture)
        || a->pointer.buttons || a->pointer.requested_buttons || a->input.buttons
        || (a->absolute && (a->abs.held || a->abs.buttons || a->abs.pending)))) {
        cancel_control_intent(a, SDL_GetTicks64());
        ph_xdnd_cancel();
        ph_pull_release(&a->pull);
        drop_pull_snapshot(a);
        hide_ghost(a);
        a->pull_hold = false;
        a->pull_note = 0;
        a->pull_path[0] = 0;
        ph_input_release_all(&a->input);
        SDL_CaptureMouse(SDL_FALSE);
        a->gesture_keys[sc] = true;
        return;
    }
    /* Eject toggles the iPhone keyboard, and iOS applies it while a text field is focused. */
    if (down && !e->repeat && sc == SDL_SCANCODE_K && (e->keysym.mod & KMOD_LALT)) {
        if (a->phone_name[0] && a->input.enabled && a->link && !a->keyboard_lift_at) {
            send_consumer(a, PH_HID_CONSUMER_EJECT);
            a->keyboard_lift_at = SDL_GetTicks64() + 50;
            PH_LOGI("iPhone keyboard: toggled");
        }
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
            /* Home swipe closes Control Center and Notification Center back in
             * the app. From the App Switcher that swipe goes to the Home Screen,
             * so tap the front card. The consumer Home key cannot replace the
             * swipe: with the absolute map, iOS 26.6.1 opened Spotlight for it. */
            sent = sc == SDL_SCANCODE_S ? ph_abs_leave_app_switcher(&a->abs, now)
                                        : ph_abs_home(&a->abs, now);
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
                presentation ? (a->absolute ? "photo or video; absolute pointer still active"
                                            : "paused; display layout differs from phone")
                             : "phone layout restored",
                screen.w, screen.h, a->portrait_aspect);
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
        {
            bool broadcast;
            pthread_mutex_lock(&a->src_lock);
            broadcast = atomic_load(&a->screen_on) && a->src.current == PH_SRC_USB;
            pthread_mutex_unlock(&a->src_lock);
            /* Each decoder counts its own pictures. */
            if (broadcast ? ph_decoder_latest_timed(a->screen_dec, frame, &a->screen_seq,
                                                    &a->frame_timing)
                          : ph_decoder_latest_timed(a->dec, frame, seq, &a->frame_timing)) {
                show_frame(a, frame);
                av_frame_unref(frame);
            }
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
    case EV_INBOX_NOTE: {
        int done = 0, total = 0;
        const char *text = e->user.data1;

        if (text && sscanf(text, "Receiving %d of %d", &done, &total) == 2 && total > 0 && a->card
            && done < total)
            ph_transfer_card_show(a->card, NULL, done, total, false, SDL_GetTicks64());
        ph_window_set_detail(a->win, text);
        free(e->user.data1);
        break;
    }
    case EV_INBOX: {
        char line[128];
        char dest[1024];
        int count = 0, saved;

        if (!e->user.data1)
            break;
        for (const char *c = e->user.data1; *c; c++)
            count += *c == '\n';
        count++;
        /* Shared from the iPhone's Share sheet: straight into Downloads, like AirDrop. */
        saved = downloads_move_all(e->user.data1, dest, sizeof dest);
        free(e->user.data1);
        if (a->card) {
            if (saved > 0 && dest[0])
                ph_transfer_card_show(a->card, dest, saved, saved, true, SDL_GetTicks64());
            else
                ph_transfer_card_dismiss(a->card, SDL_GetTicks64());
        }
        snprintf(line, sizeof line, "Saved %d item%s from the iPhone in Downloads", saved,
                 saved == 1 ? "" : "s");
        ph_window_set_detail(a->win, line);
        PH_LOGI("share: saved %d of %d item(s) in Downloads", saved, count);
        break;
    }
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

    if (a->card && ph_transfer_card_event(a->card, e, SDL_GetTicks64()))
        return true;

    switch (e->type) {
    case SDL_QUIT:
        ph_xdnd_cancel();
        return false;
    case SDL_SYSWMEVENT:
        ph_xdnd_syswm(e->syswm.msg);
        break;
    case SDL_WINDOWEVENT:
        if (e->window.windowID != win_id)
            break;
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            e->window.event == SDL_WINDOWEVENT_EXPOSED)
            ph_window_redraw(a->win);
        else if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            /* A file drag already let go of the phone; a press that is only armed
             * is still a phone drag and must be released like one. */
            if (a->pull.pulling)
                break;
            ph_pull_release(&a->pull);
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
            if (a->pull.armed || a->pull.pulling)
                break;
            PH_LOGD("pointer cancel: mouse left window");
            if (a->setup.calibrating)
                break; /* calibration is autonomous; the mouse may be outside the picture */
            if (a->absolute && a->abs.held) {
                /* An unrecognized dark photo may have started as a native
                 * drag. Return it before lifting instead of committing a
                 * Photos dismissal at the edge of the window. */
                ph_abs_cancel_drag(&a->abs, a->press_phone_x, a->press_phone_y, SDL_GetTicks64());
                SDL_CaptureMouse(SDL_FALSE);
                break;
            }
            cancel_control_intent(a, SDL_GetTicks64());
            SDL_CaptureMouse(SDL_FALSE);
        }
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (e->key.windowID == win_id && !ph_window_chrome_event(a->win, e))
            handle_key(a, &e->key);
        break;
    case SDL_MOUSEWHEEL:
        if (e->wheel.windowID == win_id && !ph_window_chrome_event(a->win, e))
            scroll(a, &e->wheel);
        break;
    case SDL_MOUSEMOTION:
        if (e->motion.windowID == win_id && !ph_window_chrome_event(a->win, e))
            mouse_motion(a, &e->motion);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (e->button.windowID == win_id) {
            if (ph_window_chrome_event(a->win, e))
                break;
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

static void sync_clipboard(app *a)
{
    unsigned flags;
    char kind = 0;
    void *data = NULL;
    size_t len = 0;
    Uint64 now;

    ph_hostclip_pump();
    flags = ph_origin_clip_flags(a->origin);
    if (ph_origin_take_clip(a->origin, &kind, &data, &len)) {
        ph_hostclip_apply(kind, data, len);
        free(data);
    }
    if (flags & PH_ORIGIN_CLIP_SENT)
        ph_window_set_detail(a->win, "On the iPhone, paste with Ctrl+V");
    if (flags & PH_ORIGIN_CLIP_GOT)
        ph_window_set_detail(a->win, "iPhone clipboard is on this PC");
    now = SDL_GetTicks64();
    if (now - a->clip_poll_at < 400)
        return;
    a->clip_poll_at = now;
    if (!ph_hostclip_changed(&kind, &data, &len))
        return;
    ph_origin_put_clip(a->origin, kind, data, len);
    free(data);
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
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
#endif
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
    a.pull_frame = av_frame_alloc();
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
    ph_hostclip_init();
    a.ghost = ph_drag_ghost_create();
    if (!a.ghost)
        PH_LOGW("photo drag image unavailable");
    a.card = ph_transfer_card_create();
    if (!a.card)
        PH_LOGW("transfer card unavailable");
    if (a.polish)
        ph_polish_apply_saved(a.polish, SDL_GetWindowFromID(ph_window_id(a.win)));
    update_title(&a);
    {
        char hint[96];
        a.origin = ph_origin_start(hint, sizeof hint);
        if (a.origin && hint[0])
            ph_window_set_pull_hint(a.win, hint);
    }
    {
        ph_screen_callbacks screen_cb = {
            .user = &a,
            .allow = screen_allow,
            .connected = screen_connected,
            .video = screen_video,
            .disconnected = screen_disconnected,
        };
        atomic_init(&a.screen_on, false);
        atomic_init(&a.airplay_on, false);
        a.screen = ph_screen_start(&screen_cb);
        ph_window_set_share_available(a.win, a.origin && a.screen);
    }
    if (ensure_pull_dir(&a)) {
        ph_inbox_callbacks inbox_cb = {
            .user = &a,
            .allow = screen_allow, /* only the phone whose photo link gave the code */
            .progress = inbox_progress,
            .received = inbox_received,
        };
        a.inbox = ph_inbox_start(a.pull_dir, &inbox_cb);
    }
    if (opt->diagnostics)
        ph_window_toggle_diagnostics(a.win);
    char setup_done[16];
    if (opt->setup || (opt->auto_setup && !ph_config_read_line(setup_marker(&a), setup_done,
                                                              sizeof setup_done)))
        open_setup(&a);

    a.rx = ph_receiver_start(&rx_opt, &rx_cb);
    if (!a.rx)
        goto done;
    PH_LOGI("waiting for the iPhone: connect in Porthole, then tap Share screen");
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
                    || a.keyboard_lift_at
                    || (a.absolute && (ph_abs_busy(&a.abs) || a.abs.wheel != 0
                                       || (a.abs.want_valid && (a.abs.x != a.abs.want_x
                                                                || a.abs.y != a.abs.want_y))))
                    || ph_transfer_card_animating(a.card);
        if (SDL_WaitEventTimeout(&e, busy ? 4 : 16)) {
            do {
                running = handle_event(&a, &e, frame, &seq) && running;
            } while (SDL_PollEvent(&e));
        }
        ph_window_tick(a.win);
        sync_clipboard(&a);
        follow_pull(&a);
        ph_transfer_card_tick(a.card, SDL_GetTicks64());
        if (a.keyboard_lift_at && SDL_GetTicks64() >= a.keyboard_lift_at) {
            a.keyboard_lift_at = 0;
            send_consumer(&a, 0);
        }
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
    ph_xdnd_cancel();
    hide_ghost(&a);
    ph_transfer_card_destroy(a.card);
    a.card = NULL;
    ph_drag_ghost_destroy(a.ghost);
    a.ghost = NULL;
    ph_screen_stop(a.screen); /* before the origin its allow callback reads */
    ph_inbox_stop(a.inbox);
    ph_origin_stop(a.origin);
    ph_hostclip_shutdown();
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
    if (a.screen_dec)
        ph_decoder_stop(a.screen_dec);
    discard_pending_events(&a); /* all event producers have stopped */
    ph_window_destroy(a.win);
    av_frame_free(&frame);
    av_frame_free(&a.view_frame);
    av_frame_free(&a.pointer_baseline);
    av_frame_free(&a.pointer_reference);
    av_frame_free(&a.pointer_after);
    av_frame_free(&a.pull_frame);
    SDL_Quit();
    return rc;
}
