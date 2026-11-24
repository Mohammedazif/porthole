#include "app.h"

#include <errno.h>
#include <openssl/rand.h>
#include <signal.h>
#include <stdatomic.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <sys/stat.h>

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
#include "files/places.h"
#include "core/upath.h"
#ifdef _WIN32
#include <SDL_syswm.h>
#include <commctrl.h>
/* windows.h (via upath.h) defines these as old segment keywords. */
#undef near
#undef far
#endif
#include "broadcast/screen.h"
#include "files/pull.h"
#include "files/xdnd.h"
#include "ui/drag_ghost.h"
#include "ui/transfer_card.h"
#include "ui/wintray.h"
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

#ifdef _WIN32
static LRESULT CALLBACK windows_shutdown(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                         UINT_PTR id, DWORD_PTR context)
{
    (void)context;
    /* Restart Manager asks first, then confirms shutdown. Its fallback is WM_CLOSE. */
    switch (message) {
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (!wp)
            return 0;
        /* fall through */
    case WM_CLOSE:
        request_stop(0);
        EndMenu();
        return 0;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, windows_shutdown, id);
        break;
    }
    return DefSubclassProc(window, message, wp, lp);
}

static void watch_windows_shutdown(SDL_Window *window)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS ||
        !SetWindowSubclass(info.info.win.window, windows_shutdown, 1, 0))
        PH_LOGW("could not attach the Windows shutdown handler");
}
#endif

enum {
    EV_FRAME,
    EV_CONNECTED,
    EV_DISCONNECTED,
    EV_BLE,
    EV_INBOX_NOTE,
    EV_INBOX_SAVED,
    EV_INBOX_ITEM,
    EV_COUNT
};

typedef struct app {
    const ph_app_options *opt;
    ph_window *win;
    ph_history *history;
    unsigned history_generation;
    char history_profile[65];
    bool companion_connected;
    char request_id[129];
    Uint64 session_poll;
    ph_decoder *dec;
    ph_receiver *rx;
    ph_audio *audio;
    ph_polish *polish;
    ph_hid_link *link;
    bool link_stopping;
    bool link_policy_stopped;
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
    bool controls_paused;
    uint8_t mouse_abs_down; /* phone buttons pressed by the PC mouse, not by shortcuts */
    bool gesture_keys[256];
    bool remote_clipboard_pending;
    char control_proof[17];
    unsigned proof_step;
    Uint64 proof_next_at, proof_grace_until;
    bool paste_when_ready; /* Ctrl+V is waiting for the iPhone copy, then types it */
    Uint64 paste_armed_at;
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
    bool airplay_selected; /* protected by src_lock */
    pthread_mutex_t src_lock;
    pthread_t usb_thread;
    atomic_bool usb_stop;
    atomic_bool usb_pause_requested;
    bool usb_started;
    ph_pull pull;
    ph_drag_ghost *ghost;
    ph_transfer_card *card;
    ph_wintray *tray;
    char notified_file[1024];
    int place_x, place_y;
    Uint64 place_at;
    _Atomic(ph_origin *) origin;
    Uint64 clip_poll_at, address_poll_at;
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

static bool controls_wanted(app *a);
static bool may_forward_controls(app *a);

/* ------------------------------------------------------------ cross-thread notifications */

static _Thread_local unsigned channel_epoch;

static bool push_event(app *a, int which, void *data)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = a->ev_base + (Uint32)which;
    e.user.data1 = data;
    e.user.code = (Sint32)channel_epoch;
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

    pthread_mutex_lock(&a->src_lock);
    ph_src active = ph_src_switch_wifi_frame(&a->src, ph_usb_annexb_keyframe(data, len),
                                             ph_metrics_now_ms());
    if (active == PH_SRC_WIFI && (!a->polish || ph_polish_accepts_video(a->polish))) {
        if (a->audio)
            ph_audio_note_video_pts(a->audio, timestamp_ns);
        ph_polish_on_video(a->polish, PH_SRC_WIFI, data, len, timestamp_ns);
        ph_decoder_push(a->dec, data, len);
    }
    pthread_mutex_unlock(&a->src_lock);
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
    pthread_mutex_lock(&a->src_lock);
    a->airplay_selected = false;
    ph_src_switch_wifi_lost(&a->src, ph_metrics_now_ms());
    pthread_mutex_unlock(&a->src_lock);
    ph_polish_disconnected(a->polish);
    push_event(user, EV_DISCONNECTED, NULL);
}

static bool authorize_channel(void *user, const char *ticket)
{
    app *a = user;
    unsigned epoch = ph_origin_generation(a->origin, ticket);
    if (!epoch)
        return false;
    channel_epoch = epoch;
    return true;
}

static bool authorize_inbox(void *user, const char *ticket)
{
    app *a = user;
    unsigned epoch = ph_origin_inbox_generation(a->origin, ticket);
    if (!epoch)
        return false;
    channel_epoch = epoch;
    return true;
}

static void inbox_progress(void *user, int done, int total)
{
    char line[96];

    snprintf(line, sizeof line, done < total ? "Receiving %d of %d from the iPhone"
                                             : "Received %d of %d from the iPhone",
             done, total);
    if (done < 0)
        snprintf(line, sizeof line, "Transfer interrupted. Retry from your iPhone.");
    push_event(user, EV_INBOX_NOTE, strdup(line));
}

static void inbox_item(void *user, const ph_inbox_item *item)
{
    ph_inbox_item *copy = malloc(sizeof *copy);
    if (copy) {
        *copy = *item;
        push_event(user, EV_INBOX_ITEM, copy);
    }
}

static void inbox_saved(void *user, const char *path)
{
    push_event(user, EV_INBOX_SAVED, strdup(path));
}

static bool inbox_profile(void *user, const char *ticket, char out[65])
{
    app *a = user;
    return ph_origin_receive_profile(a->origin, ticket, out);
}

static void screen_connected(void *user, const char *address)
{
    app *a = user;
    (void)address;
    pthread_mutex_lock(&a->src_lock);
    if (!a->screen_dec) {
        a->screen_dec = ph_decoder_start(on_frame, a);
        /* Same longer side as AirPlay's portrait mirror. The broadcast arrives
         * near 888x1920, and drawing that on this machine falls behind. */
        ph_decoder_limit_edge(a->screen_dec, 1080);
    }
    bool started = a->screen_dec != NULL;
    if (started)
        atomic_store(&a->screen_on, true);
    bool announce = started && a->src.current != PH_SRC_USB && !atomic_load(&a->airplay_on);
    pthread_mutex_unlock(&a->src_lock);
    if (started)
        ph_origin_screen_live(a->origin, channel_epoch, true);
    if (announce)
        push_event(user, EV_CONNECTED, strdup("Connected"));
}

static void screen_video(void *user, const uint8_t *annexb, size_t len, bool keyframe)
{
    app *a = user;
    pthread_mutex_lock(&a->src_lock);
    ph_src before = a->src.current;
    if (a->screen_dec && !a->airplay_selected &&
        ph_src_switch_broadcast_frame(&a->src, keyframe, ph_metrics_now_ms())) {
        if (before != PH_SRC_BROADCAST) {
            ph_decoder_reset(a->dec);
            ph_audio_flush(a->audio);
            PH_LOGI("showing the broadcast screen");
        }
        ph_polish_on_video(a->polish, PH_SRC_BROADCAST, annexb, len, 0);
        ph_decoder_push(a->screen_dec, annexb, len);
    }
    pthread_mutex_unlock(&a->src_lock);
}

static void screen_audio(void *user, const uint8_t *pcm, size_t len)
{
    app *a = user;
    pthread_mutex_lock(&a->src_lock);
    if (a->src.current == PH_SRC_BROADCAST && !atomic_load(&a->airplay_on))
        ph_audio_push_broadcast(a->audio, pcm, len);
    pthread_mutex_unlock(&a->src_lock);
}

static void screen_disconnected(void *user)
{
    app *a = user;

    ph_origin_screen_live(a->origin, channel_epoch, false);
    pthread_mutex_lock(&a->src_lock);
    if (a->src.current == PH_SRC_BROADCAST)
        ph_audio_flush(a->audio);
    atomic_store(&a->screen_on, false);
    ph_src next = ph_src_switch_broadcast_lost(&a->src, ph_metrics_now_ms());
    if (a->screen_dec)
        ph_decoder_reset(a->screen_dec); /* the next broadcast starts clean */
    pthread_mutex_unlock(&a->src_lock);
    if (next == PH_SRC_NONE && !atomic_load(&a->airplay_on))
        push_event(user, EV_DISCONNECTED, NULL);
}

static void on_ble_status(void *user, const char *line)
{
    push_event(user, EV_BLE, strdup(line));
}

/* ------------------------------------------------------------ main thread */

static bool setup_controls_ready(const app *a)
{
    return a->setup.step == PH_SETUP_BLUETOOTH
               ? a->link && !a->link_stopping && !strcmp(a->ble_status, "on")
               : a->input.enabled;
}

static void update_title(app *a)
{
    char title[320];
    const char *control = a->opt->bluetooth && a->opt->ble_helper ? a->ble_status : "off";
    bool bluetooth_ready = !strcmp(control, "on");
    ph_control_policy policy = ph_origin_control_policy(a->origin);
    if (bluetooth_ready && policy.managed && !policy.verified)
        control = "Verify Bluetooth on iPhone";
    else if (bluetooth_ready && !a->input.enabled)
        control = "Waiting for screen sharing";

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
    ph_window_set_input_policy(a->win, bluetooth_ready, policy);
    ph_window_set_status(a->win, a->phone_name[0] != 0,
                         !layout_pauses_pointer && a->input.enabled,
                         layout_pauses_pointer ? "Return to phone view" : control);
    ph_setup_update(&a->setup, setup_controls_ready(a),
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
        state = a->link_policy_stopped ? "controls inactive" : "unavailable; retrying";
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
    if (!strncmp(line, "error", 5) || (!strcmp(line, "exited") && !a->link_policy_stopped))
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
    if (was_on && strcmp(a->ble_status, "on")) {
        ph_origin_reset_control_verification(a->origin);
        a->proof_grace_until = 0;
    }
    ph_input_set_enabled(&a->input, may_forward_controls(a));
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
        a->ble_retry_at = a->link_policy_stopped ? 0 : SDL_GetTicks64() + BLE_RETRY_MS;
        a->link_policy_stopped = false;
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

static bool desktop_copy(const char *src, char *dest, size_t cap)
{
    char desktop[1024];

    return ph_place_dir(PH_PLACE_DESKTOP, desktop, sizeof desktop) &&
           ph_place_copy_unique(src, desktop, ph_path_base(src), dest, cap);
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

/* Moves each file, one per line, into the folder chosen in Settings (Downloads
 * until the user picks another) without replacing anything there: a name already
 * taken gets 2-, 3-, ... in front. Returns how many arrived.
 * first receives the path of the first one that landed. names receives each
 * saved file's own name, one per line, when it fits. */
/* Each pairing has its own local history, independent of device names or network addresses. */
static void sync_pairing_history(app *a)
{
    char profile[65] = "", name[80], path[1100];
    unsigned generation = ph_origin_generation(a->origin, NULL);
    bool selected = ph_origin_selected_id(a->origin, profile);
    if (a->history && a->history_generation == generation &&
        !strcmp(a->history_profile, profile))
        return;
    bool persistent = false;
    if (selected) {
        snprintf(name, sizeof name, "history-%.32s.bin", profile);
        persistent = ph_config_path(name, path, sizeof path);
        char bound[2];
        if (persistent &&
            !ph_config_read_line("history-profile-bound", bound, sizeof bound)) {
            char old_id[40], old_path[1100];
            if (ph_config_read_line("history-profile", old_id, sizeof old_id) &&
                strlen(old_id) == 32 && strspn(old_id, "0123456789abcdef") == 32) {
                snprintf(name, sizeof name, "history-%s.bin", old_id);
                if (ph_config_path(name, old_path, sizeof old_path) && ph_path_exists(old_path) &&
                    !ph_path_exists(path) && strcmp(old_path, path))
                    ph_publish_new(old_path, path);
            }
            ph_config_write_line("history-profile-bound", "1");
        }
    }
    ph_hostclip_cancel_pending();
    a->remote_clipboard_pending = false;
    ph_window_set_incoming(a->win, "", 0, 0);
    ph_window_set_receiving(a->win, 0, 0);
    ph_window_set_active_transfer(a->win, "");
    ph_history *history = ph_history_open(persistent ? path : NULL);
    ph_window_set_history_profile(a->win, selected ? profile : NULL);
    ph_workspace_set_history(ph_window_workspace(a->win), history);
    ph_history_close(a->history);
    a->history = history;
    a->history_generation = generation;
    snprintf(a->history_profile, sizeof a->history_profile, "%s", profile);
}

static void record_transfer(app *a, const char *path, bool incoming, ph_transfer_state state)
{
    long long bytes = ph_file_size(path);
    ph_file_entry file = {
        .incoming = incoming, .state = state, .bytes = bytes > 0 ? (uint64_t)bytes : 0};
    if (state == PH_TRANSFER_COMPLETE)
        file.confirmed = file.bytes;
    ph_origin_phone_name(a->origin, file.device, sizeof file.device);
    size_t path_length = strlen(path);
    if (path_length < sizeof file.path)
        memcpy(file.path, path, path_length + 1);
    snprintf(file.name, sizeof file.name, "%.*s", (int)sizeof file.name - 1, ph_path_base(path));
    if (a->history && !ph_history_add_file(a->history, &file))
        PH_LOGW("Unable to save transfer history");
}
/* A Windows notification for files that arrived; false where there is no tray icon,
 * and the card shows instead. where is "Downloads" or "the Desktop". */
static bool tray_notice(app *a, const char *path, int count, const char *where)
{
    char title[128], text[256], shown[96];
    const char *name = path ? ph_path_base(path) : "";
    size_t i = 0;
    SDL_Window *window = SDL_GetWindowFromID(ph_window_id(a->win));

    if (!a->tray || !(SDL_GetWindowFlags(window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) ||
        !ph_workspace_notifications(ph_window_workspace(a->win)) || !path || !path[0])
        return false;
    for (; name[i] && i + 1 < sizeof shown; i++)
        shown[i] = name[i];
    shown[i] = '\0';
    if (count > 1) {
        snprintf(title, sizeof title, "%d files received from iPhone", count);
        snprintf(text, sizeof text, "%s and %d more, in %.40s", shown, count - 1, where ? where : "");
    } else {
        snprintf(title, sizeof title, "%s received from iPhone", shown);
        snprintf(text, sizeof text, "Saved to %.40s. Show in folder.", where ? where : "Downloads");
    }
    if (!ph_wintray_notify(a->tray, title, text))
        return false;
    if (strlen(path) < sizeof a->notified_file)
        memcpy(a->notified_file, path, strlen(path) + 1);
    else
        a->notified_file[0] = '\0';
    return true;
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
    if (offer) {
        if (!strchr(offer, '\n'))
            record_transfer(a, offer, true, PH_TRANSFER_COMPLETE);
        if (landed)
            tray_notice(a, offer, 1, "the Desktop");
        if (a->card) {
            ph_transfer_card_place(a->card, landed ? "Desktop" : NULL);
            ph_transfer_card_show(a->card, offer, 1, 1, landed, SDL_GetTicks64());
        }
    }
    if (note == 3)
        ph_window_set_detail(a->win, placed ? "Original file offered to the drop destination."
                                            : landed ? "Original file saved to Desktop."
                                                     : "Original available in File history. Could not save to Desktop.");
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

/* Request one original after the deliberate drag crosses the gesture threshold. */
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
    ask_pull(a);
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
        ph_origin_cancel_pull(a->origin);
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
            /* A Ctrl click without a drag never requests an original. */
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
        a->mouse_abs_down = down ? (uint8_t)(a->mouse_abs_down | button)
                                 : (uint8_t)(a->mouse_abs_down & ~button);
        /* Keep the mouse captured from the press itself. The phone press is sent about
         * 120 ms later, and releasing capture until then let Windows hand a release
         * outside the window to another app, leaving the iPhone pressed. */
        SDL_CaptureMouse(a->mouse_abs_down || a->abs.held ? SDL_TRUE : SDL_FALSE);
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
    ph_setup_update(&a->setup, setup_controls_ready(a),
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

static bool clipboard_file(char *out, size_t cap);
static bool queue_pc_file(app *a, const char *path);

static uint8_t pc_modifiers(const app *a, SDL_Keymod mods)
{
    uint8_t result = 0;
    (void)a;
    /* Ctrl stays Ctrl here. input.c turns it into Command when that setting is on.
     * Doing it in both places put Control back on the iPhone, so Ctrl+C and Ctrl+A
     * never arrived as Command. */
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
    if (down && !(sc == SDL_SCANCODE_V && (e->keysym.mod & KMOD_CTRL)
                  && !(e->keysym.mod & (KMOD_ALT | KMOD_GUI))))
        a->paste_when_ready = false;
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
            ph_window_toggle_statistics(a->win);
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
        ph_origin_cancel_pull(a->origin);
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
    uint16_t consumer = ph_window_media_keys(a->win) ? pc_consumer_usage(sc) : 0;
    if (a->phone_name[0] && consumer) {
        if (!down || !(e->keysym.mod & KMOD_LALT)) {
            mark_input(a, e->timestamp);
            ph_input_consumer_key(&a->input, consumer, down, e->repeat != 0);
        }
        return;
    }
    if (a->phone_name[0] && (unsigned)sc < 256) {
        a->input.swap_ctrl_cmd = ph_window_ctrl_is_command(a->win);
        a->input.swap_set = true;
        mark_input(a, e->timestamp);
        if (down && !e->repeat && sc == SDL_SCANCODE_V && (e->keysym.mod & KMOD_CTRL)
            && !(e->keysym.mod & (KMOD_ALT | KMOD_GUI))) {
            char path[1024];
            a->paste_when_ready = false;
            if (clipboard_file(path, sizeof path)) {
                a->gesture_keys[sc] = true;
                queue_pc_file(a, path);
                return;
            }
            if (ph_origin_clip_waiting(a->origin)) {
                a->gesture_keys[sc] = true;
                a->paste_when_ready = true;
                a->paste_armed_at = SDL_GetTicks64();
                ph_window_set_detail(a->win, "Pasting the iPhone copy");
                return;
            }
            if (a->polish && ph_polish_type_clipboard(a->polish)) {
                a->gesture_keys[sc] = true;
                ph_window_set_detail(a->win, "Pasted into the iPhone");
                return;
            }
        }
        ph_input_key(&a->input, (uint8_t)sc, e->type == SDL_KEYDOWN, e->repeat != 0,
                     pc_modifiers(a, (SDL_Keymod)e->keysym.mod));
        if (down && !e->repeat && sc == SDL_SCANCODE_C && (e->keysym.mod & KMOD_CTRL)
            && !(e->keysym.mod & (KMOD_ALT | KMOD_GUI)) && a->origin &&
            ph_workspace_clip_receive(ph_window_workspace(a->win)) &&
            (ph_origin_clip_flags(a->origin) & PH_ORIGIN_CLIP_READY)) {
            ph_origin_ask_clip(a->origin);
            ph_window_set_detail(a->win, "Copying from the iPhone");
        }
    }
}

static void release_everything(app *a)
{
    if (a->absolute)
        ph_abs_release_all(&a->abs, SDL_GetTicks64());
    ph_gesture_cancel(&a->gesture, SDL_GetTicks64());
    a->feed_have_wheel = false;
    memset(a->gesture_keys, 0, sizeof a->gesture_keys);
    a->paste_when_ready = false;
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

static void observe_pointer(app *a, bool defer_to_queued_frame)
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
    /* show_frame passes false so the picture just stored is read while the next is queued. */
    if (!f || !f->data[0] || a->pointer.phase != PH_POINTER_WAIT_VIDEO
        || (defer_to_queued_frame && atomic_load(&a->frame_pending))
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
    observe_pointer(a, false);
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
            pthread_mutex_lock(&a->src_lock);
            bool broadcast = a->src.current == PH_SRC_BROADCAST;
            bool received = broadcast
                ? ph_decoder_latest_timed(a->screen_dec, frame, &a->screen_seq, &a->frame_timing)
                : a->src.current != PH_SRC_NONE &&
                  ph_decoder_latest_timed(a->dec, frame, seq, &a->frame_timing);
            pthread_mutex_unlock(&a->src_lock);
            if (received) {
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
        if (a->phone_name[0] && strcmp(a->phone_name, "iPhone (screen broadcast)") != 0
            && strcmp(a->phone_name, "Connected") != 0)
            ph_window_set_phone_name(a->win, a->phone_name);
        ph_decoder_reset(a->dec);
        ph_pointer_set_enabled(&a->pointer, a->input.enabled, SDL_GetTicks64());
        update_title(a);
        free(e->user.data1);
        break;
    case EV_DISCONNECTED: {
        pthread_mutex_lock(&a->src_lock);
        bool showing_other = a->src.current == PH_SRC_USB || atomic_load(&a->screen_on)
                             || atomic_load(&a->airplay_on);
        pthread_mutex_unlock(&a->src_lock);
        if (showing_other)
            break;
        ph_polish_stop_recording(a->polish);
        release_everything(a);
        clear_view(a);
        ph_decoder_reset(a->dec);
        ph_pointer_set_enabled(&a->pointer, false, SDL_GetTicks64());
        a->phone_name[0] = '\0';
        ph_window_clear(a->win);
        update_title(a);
        break;
    }
    case EV_BLE:
        handle_ble_status(a, (const char *)e->user.data1);
        free(e->user.data1);
        break;
    case EV_INBOX_NOTE: {
        if ((unsigned)e->user.code != ph_origin_generation(a->origin, NULL)) {
            free(e->user.data1);
            break;
        }
        int done = 0, total = 0;
        const char *text = e->user.data1;

        if (text && sscanf(text, "Receiving %d of %d", &done, &total) == 2)
            ph_window_set_receiving(a->win, done, total);
        else
            ph_window_set_receiving(a->win, 0, 0);
        if (total > 0 && a->card && !a->tray && done < total)
            ph_transfer_card_show(a->card, NULL, done, total, false, SDL_GetTicks64());
        ph_window_set_detail(a->win, text);
        free(e->user.data1);
        break;
    }
    case EV_INBOX_ITEM: {
        ph_inbox_item *item = e->user.data1;
        if (!item)
            break;
        if ((unsigned)e->user.code == ph_origin_generation(a->origin, NULL)) {
            sync_pairing_history(a);
            bool active = item->state == PH_INBOX_RECEIVING;
            ph_window_set_incoming(a->win, active ? item->name : "", item->bytes, item->total);
            ph_transfer_state state = active                           ? PH_TRANSFER_ACTIVE
                                      : item->state == PH_INBOX_STORED ? PH_TRANSFER_COMPLETE
                                                                       : PH_TRANSFER_PAUSED;
            const ph_file_entry *existing = NULL;
            for (size_t i = 0; i < ph_history_file_count(a->history); i++) {
                const ph_file_entry *entry = ph_history_file(a->history, i);
                if (entry->incoming && !strcmp(entry->transfer_id, item->id)) {
                    existing = entry;
                    break;
                }
            }
            if (existing) {
                if (existing->state != state || !active ||
                    (item->digest[0] && strcmp(existing->digest, item->digest))) {
                    ph_file_entry updated = *existing;
                    updated.state = state;
                    updated.confirmed = item->bytes;
                    if (item->path[0])
                        snprintf(updated.path, sizeof updated.path, "%s", item->path);
                    if (item->digest[0])
                        memcpy(updated.digest, item->digest, sizeof updated.digest);
                    ph_history_replace_file(a->history, existing->id, &updated);
                }
            } else {
                ph_file_entry entry = {.incoming = true,
                                       .bytes = item->total,
                                       .confirmed = item->bytes,
                                       .state = state};
                snprintf(entry.name, sizeof entry.name, "%s", item->name);
                snprintf(entry.path, sizeof entry.path, "%s", item->path);
                memcpy(entry.transfer_id, item->id, sizeof entry.transfer_id);
                memcpy(entry.digest, item->digest, sizeof entry.digest);
                ph_origin_phone_name(a->origin, entry.device, sizeof entry.device);
                ph_history_add_file(a->history, &entry);
            }
            if (item->state == PH_INBOX_INTERRUPTED)
                ph_window_set_receiving(a->win, 0, 0);
            if (item->message[0])
                ph_window_set_detail(a->win, item->message);
        }
        free(item);
        break;
    }
    case EV_INBOX_SAVED: {
        char *path = e->user.data1;
        if (path) {
            if ((unsigned)e->user.code != ph_origin_generation(a->origin, NULL)) {
                free(path);
                break;
            }
            char label[128] = "", note[128];
            ph_place_save_label(label, sizeof label);
            ph_window_note_received(a->win, 1);
            bool notified = tray_notice(a, path, 1, label);
            if (!notified && a->card && ph_workspace_notifications(ph_window_workspace(a->win))) {
                ph_transfer_card_place(a->card, label[0] ? label : NULL);
                ph_transfer_card_show(a->card, path, 1, 1, true, SDL_GetTicks64());
            }
            snprintf(note, sizeof note, "Received %.100s", ph_path_base(path));
            ph_window_note_transfer(a->win, note);
            ph_window_set_detail(a->win, note);
            free(path);
        }
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

    if (!a->opt->bluetooth || !a->opt->ble_helper || a->link)
        return;
    a->ble_retry_at = 0;
    a->link_policy_stopped = false;
#ifndef _WIN32
    setenv("PORTHOLE_BLE_NAME", a->opt->name, 1);
    if (a->opt->absolute_pointer)
        setenv("PORTHOLE_HID_ABSOLUTE", "replace", 1);
    else
        unsetenv("PORTHOLE_HID_ABSOLUTE");
#else
    /* The WinRT helper inherits this and publishes the absolute mouse report. */
    _putenv_s("PORTHOLE_HID_ABSOLUTE", a->opt->absolute_pointer ? "replace" : "");
#endif
    snprintf(a->ble_status, sizeof a->ble_status, "starting");
    a->link = ph_hid_link_start(argv, on_ble_status, a);
    if (!a->link) {
        PH_LOGW("continuing without Bluetooth control (view only)");
        a->ble_retry_at = SDL_GetTicks64() + BLE_RETRY_MS;
    }
}

static bool controls_wanted(app *a)
{
    ph_control_policy policy = ph_origin_broadcast_control_policy(a->origin);
    pthread_mutex_lock(&a->src_lock);
    ph_src selected = a->src.current;
    pthread_mutex_unlock(&a->src_lock);
    bool airplay = atomic_load(&a->airplay_on);
    bool broadcast = atomic_load(&a->screen_on);
    /* The companion authorizes its own broadcast. The cable or AirPlay could
     * belong to a different phone, so don't forward its companion controls. */
    if (policy.managed && (selected == PH_SRC_USB || selected == PH_SRC_WIFI ||
                           (selected == PH_SRC_NONE && airplay && !broadcast)))
        return false;
    bool sharing = policy.managed ? broadcast : broadcast || airplay || selected == PH_SRC_USB;
    bool pairing = (a->setup.visible && a->setup.controls && a->setup.step >= PH_SETUP_BLUETOOTH) ||
                   ph_origin_verifying_controls(a->origin) ||
                   (ph_origin_controls_verified(a->origin) && SDL_GetTicks64() < a->proof_grace_until);
    return ph_control_connection_wanted(policy, sharing, pairing, a->controls_paused);
}

static bool may_forward_controls(app *a)
{
    if (!a->link || a->link_stopping || strcmp(a->ble_status, "on") || !controls_wanted(a))
        return false;
    ph_control_policy policy = ph_origin_broadcast_control_policy(a->origin);
    return !policy.managed ||
           (policy.verified && (!policy.screen_only || atomic_load(&a->screen_on)));
}

/* The number crosses the Bluetooth keyboard link only. Verification is tied to
 * both its current dispatch generation and the authenticated companion session. */
static void tick_control_proof(app *a)
{
    bool waiting = ph_origin_verifying_controls(a->origin);
    if (!waiting || !a->link || a->link_stopping || strcmp(a->ble_status, "on") ||
        (a->control_proof[0] && !ph_origin_control_proof_current(a->origin, a->control_proof))) {
        if (a->control_proof[0] && a->link && !a->link_stopping)
            ph_hid_link_keyboard(a->link, &(ph_hid_keyboard){0});
        memset(a->control_proof, 0, sizeof a->control_proof);
        return;
    }
    Uint64 now = SDL_GetTicks64();
    if (!a->control_proof[0]) {
        uint64_t random;
        if (RAND_bytes((unsigned char *)&random, sizeof random) != 1)
            return;
        snprintf(a->control_proof, sizeof a->control_proof, "%016llu",
                 (unsigned long long)(random % UINT64_C(10000000000000000)));
        if (!ph_origin_issue_control_proof(a->origin, a->control_proof)) {
            memset(a->control_proof, 0, sizeof a->control_proof);
            return;
        }
        a->proof_step = 0;
        a->proof_next_at = now;
        a->proof_grace_until = now + 42000;
    }
    if (a->proof_step >= 32 || now < a->proof_next_at)
        return;
    ph_hid_keyboard report = {0};
    if ((a->proof_step & 1) == 0) {
        unsigned digit = (unsigned)(a->control_proof[a->proof_step / 2] - '0');
        report.keys[0] = (uint8_t)(digit ? 0x58 + digit : 0x62); /* keypad digits */
    }
    if (!ph_hid_link_keyboard(a->link, &report)) {
        ph_origin_reset_control_verification(a->origin);
        memset(a->control_proof, 0, sizeof a->control_proof);
        return;
    }
    a->proof_step++;
    a->proof_next_at = now + 24;
}

static void tick_control_connection(app *a)
{
    if (!controls_wanted(a)) {
        a->ble_retry_at = 0;
        if (a->link && !a->link_stopping) {
            release_everything(a);
            ph_input_set_enabled(&a->input, false);
            ph_pointer_set_enabled(&a->pointer, false, SDL_GetTicks64());
            a->link_stopping = true;
            a->link_policy_stopped = true;
            ph_origin_reset_control_verification(a->origin);
            a->proof_grace_until = 0;
            memset(a->control_proof, 0, sizeof a->control_proof);
            ph_hid_link_request_stop(a->link);
            snprintf(a->ble_status, sizeof a->ble_status, "controls inactive");
            update_title(a);
        } else if (!a->link && a->opt->bluetooth && a->opt->ble_helper &&
                   strcmp(a->ble_status, "controls inactive")) {
            snprintf(a->ble_status, sizeof a->ble_status, "controls inactive");
            update_title(a);
        }
        return;
    }
    if (!a->link && (!a->ble_retry_at || SDL_GetTicks64() >= a->ble_retry_at)) {
        start_bluetooth(a);
        update_title(a);
    }
    tick_control_proof(a);
    bool permitted = may_forward_controls(a);
    if (permitted != a->input.enabled) {
        if (!permitted)
            release_everything(a);
        ph_input_set_enabled(&a->input, permitted);
        ph_pointer_set_enabled(&a->pointer, permitted && a->phone_name[0] &&
                                !a->presentation_mode, SDL_GetTicks64());
        update_title(a);
    }
}

/* Returns false when the app should quit. */
static bool handle_event(app *a, const SDL_Event *e, AVFrame *frame, uint64_t *seq)
{
    unsigned int win_id = ph_window_id(a->win);

    if (e->type == SDL_WINDOWEVENT && e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
        ph_workspace_refresh_files(ph_window_workspace(a->win));
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
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
            ph_window_keep_aspect(a->win);
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            e->window.event == SDL_WINDOWEVENT_EXPOSED ||
            e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
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
            if (ph_window_release_on_blur(a->win))
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
    case SDL_TEXTINPUT:
        if (e->text.windowID == win_id)
            ph_window_chrome_event(a->win, e);
        break;
    case SDL_DROPFILE:
        if (e->drop.file) {
            queue_pc_file(a, e->drop.file);
            SDL_free(e->drop.file);
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
        if (e->type == SDL_MOUSEBUTTONDOWN)
            a->paste_when_ready = false;
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
    if (a->airplay_selected) {
        pthread_mutex_unlock(&a->src_lock);
        return;
    }
    before = a->src.current;
    after = ph_src_switch_usb_frame(&a->src, ph_usb_annexb_keyframe(annexb, n), ph_metrics_now_ms());
    if (before != PH_SRC_USB && after == PH_SRC_USB) {
        if (a->polish)
            ph_polish_usb_present(a->polish, true);
        if (a->screen_dec)
            ph_decoder_reset(a->screen_dec);
        ph_decoder_reset(a->dec);
        PH_LOGI("showing the USB picture");
    }
    if (after == PH_SRC_USB && (!a->polish || ph_polish_accepts_usb(a->polish))) {
        ph_polish_on_video(a->polish, PH_SRC_USB, annexb, n, 0);
        ph_decoder_push(a->dec, annexb, n);
    }
    pthread_mutex_unlock(&a->src_lock);
}

static void usb_unplugged(app *a)
{
    ph_src after;

    pthread_mutex_lock(&a->src_lock);
    bool was_usb = a->src.current == PH_SRC_USB;
    after = ph_src_switch_usb_unplugged(&a->src, ph_metrics_now_ms());
    if (a->polish)
        ph_polish_usb_present(a->polish, false);
    if (was_usb && a->dec)
        ph_decoder_reset(a->dec);
    pthread_mutex_unlock(&a->src_lock);
    if (was_usb && after == PH_SRC_NONE && !atomic_load(&a->screen_on) &&
        !atomic_load(&a->airplay_on))
        push_event(a, EV_DISCONNECTED, NULL);
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
        if (atomic_load(&a->usb_pause_requested)) {
            usb_pause(a, 100);
            continue;
        }
        ph_usb_capture *cap = ph_usb_libusb_open(&a->usb_pause_requested);
        if (!cap) {
            if (atomic_load(&a->usb_stop))
                break;
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
        while (!atomic_load(&a->usb_pause_requested)) {
            ph_usb_sample sample;
            int rc = ph_usb_capture_poll(cap, &sample, copy_buf, PH_USB_FRAME_MAX);
            if (rc < 0 || atomic_load(&a->usb_pause_requested))
                break;
            if (rc > 0)
                take_usb_sample(a, &sample, annexb, PH_USB_FRAME_MAX);
        }
        ph_usb_capture_close(cap);
        if (!atomic_load(&a->usb_stop))
            usb_unplugged(a);
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

    if (a->origin && a->win) {
        char named[64];
        ph_window_set_code_open(a->win, ph_origin_code_open(a->origin));
        ph_origin_phone_name(a->origin, named, sizeof named);
        if (named[0])
            ph_window_set_phone_name(a->win, named);
    }
    ph_workspace *workspace = ph_window_workspace(a->win);
    bool allow_receive = ph_workspace_clip_receive(workspace);
    flags = ph_origin_clip_flags(a->origin);
    if (a->remote_clipboard_pending && (!allow_receive || !(flags & PH_ORIGIN_CLIP_READY)))
        ph_hostclip_cancel_pending();
    int clipboard_result = ph_hostclip_pump();
    if (clipboard_result > 0)
        ph_window_set_detail(a->win, a->remote_clipboard_pending
                                        ? "iPhone image copied to the PC clipboard."
                                        : "Image copied to the PC clipboard.");
    else if (clipboard_result < 0)
        ph_window_set_detail(a->win, "Unable to copy the image to the PC clipboard.");
    if (!ph_hostclip_pending())
        a->remote_clipboard_pending = false;
    ph_window_set_clipboard_available(a->win, (flags & PH_ORIGIN_CLIP_READY) != 0);
    bool applied = false;
    if (ph_origin_take_clip(a->origin, &kind, &data, &len)) {
        if (allow_receive) {
            applied = ph_hostclip_apply(kind, data, len);
            a->remote_clipboard_pending = applied && ph_hostclip_pending();
            uint64_t saved = ph_history_add_clip(a->history, kind, data, len, true);
            if (!applied || !saved)
                ph_window_set_detail(a->win, !applied && saved
                    ? "Item saved to history. The PC clipboard is unavailable."
                    : "Unable to save the clipboard item in history.");
            if (a->win)
                ph_window_redraw(a->win);
        }
        if (applied && a->paste_when_ready && !ph_origin_clip_waiting(a->origin)) {
            a->paste_when_ready = false;
            if (kind == 'T' && a->polish && ph_polish_type_clipboard(a->polish))
                ph_window_set_detail(a->win, "Pasted into the iPhone");
            else if (kind == 'P')
                ph_window_set_detail(a->win, "The iPhone copy is a picture");
            else
                ph_window_set_detail(a->win, "That copy cannot be typed in");
        }
        free(data);
    }
    if (a->paste_when_ready && SDL_GetTicks64() - a->paste_armed_at > 2000) {
        a->paste_when_ready = false;
        ph_window_set_detail(a->win, "The iPhone copy did not arrive");
    }
    if (flags & PH_ORIGIN_CLIP_SENT)
        ph_window_set_detail(a->win, "On the iPhone, paste with Ctrl+V");
    if (applied && !a->remote_clipboard_pending && (flags & PH_ORIGIN_CLIP_GOT)) {
        char pc[64], message[128];
        ph_origin_pc_name(pc, sizeof pc);
        snprintf(message, sizeof message, "Clipboard received on %s", pc);
        ph_window_set_detail(a->win, message);
    }
    now = SDL_GetTicks64();
    if (now - a->clip_poll_at < 400)
        return;
    a->clip_poll_at = now;
    if (!ph_hostclip_changed(&kind, &data, &len))
        return;
    if (a->companion_connected && ph_workspace_clip_send(workspace) &&
        ph_origin_put_clip(a->origin, kind, data, len)) {
        ph_history_add_clip(a->history, kind, data, len, false);
        if (a->win)
            ph_window_redraw(a->win);
    }
    free(data);
}

static void percent_decode(const char *in, char *out, size_t cap)
{
    size_t n = 0;

    while (*in && n + 1 < cap) {
        if (in[0] == '%' && in[1] && in[2]) {
            char hex[3] = {in[1], in[2], 0};
            char *end = NULL;
            long value = strtol(hex, &end, 16);
            if (end && end != hex && !*end && value >= 0 && value <= 255) {
                out[n++] = (char)value;
                in += 3;
                continue;
            }
        }
        out[n++] = *in++;
    }
    out[n] = 0;
}

static bool clipboard_file(char *out, size_t cap)
{
    char *text;

    /* Explorer copies files as a file list, not as text. */
    if (ph_place_clipboard_file(out, cap))
        return true;
    text = SDL_GetClipboardText();
    char *start, *end, decoded[1024];
    bool ok = false;

    if (!text)
        return false;
    start = text;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        *--end = 0;
    if (start[0] && !strchr(start, '\n')) {
        const char *path = NULL;
        if (!strncmp(start, "file://", 7)) {
            path = start + 7;
            if (strncmp(path, "localhost", 9) == 0)
                path += 9;
            else if (path[0] != '/') {
                const char *slash = strchr(path, '/');
                path = slash ? slash : path;
            }
            percent_decode(path, decoded, sizeof decoded);
            path = decoded;
        } else if (start[0] == '/') {
            path = start;
        }
        if (path && ph_file_size(path) >= 0 && strlen(path) < cap) {
            snprintf(out, cap, "%s", path);
            ok = true;
        }
    }
    SDL_free(text);
    return ok;
}

static void track_push(app *a);

static bool queue_pc_file(app *a, const char *path)
{
    if (!a->origin || !ph_origin_put_file(a->origin, path)) {
        ph_window_set_detail(a->win, "That file cannot be sent right now");
        return false;
    }
    track_push(a); /* Persist a queued row as soon as the user submits it. */
    return true;
}

static bool queue_selected_file(void *user, const char *path)
{
    return queue_pc_file(user, path);
}

static void track_push(app *a)
{
    char name[96], line[120];

    if (!a->origin)
        return;
    for (unsigned i = 0; i < 48; i++) {
        ph_origin_send send;
        ph_origin_send_state state = ph_origin_take_send(a->origin, &send);
        if (state == PH_SEND_NONE)
            break;
        ph_file_entry entry = {
            .bytes = send.bytes,
            .confirmed = send.confirmed,
            .state = state == PH_SEND_STORED   ? PH_TRANSFER_COMPLETE
                     : state == PH_SEND_QUEUED ? PH_TRANSFER_QUEUED
                     : state == PH_SEND_ACTIVE ? PH_TRANSFER_ACTIVE
                     : state == PH_SEND_CHANGED ? PH_TRANSFER_CHANGED
                     : state == PH_SEND_CANCELED ? PH_TRANSFER_CANCELED
                                                 : PH_TRANSFER_FAILED
        };
        snprintf(entry.name, sizeof entry.name, "%s", send.name);
        memcpy(entry.path, send.path, sizeof entry.path);
        memcpy(entry.transfer_id, send.id, sizeof entry.transfer_id);
        memcpy(entry.digest, send.digest, sizeof entry.digest);
        memcpy(entry.resume_key, send.resume_key, sizeof entry.resume_key);
        ph_origin_phone_name(a->origin, entry.device, sizeof entry.device);
        uint64_t history_id = 0;
        for (size_t j = 0; j < ph_history_file_count(a->history); j++) {
            const ph_file_entry *previous = ph_history_file(a->history, j);
            if (!previous->incoming &&
                (previous->id == send.history_id || !strcmp(previous->transfer_id, send.id))) {
                history_id = previous->id;
                if (!entry.device[0])
                    memcpy(entry.device, previous->device, sizeof entry.device);
                break;
            }
        }
        bool saved = history_id ? ph_history_replace_file(a->history, history_id, &entry)
                                : ph_history_add_file(a->history, &entry) != 0;
        if (!saved)
            PH_LOGW("Unable to save transfer history");
        if (state == PH_SEND_QUEUED || state == PH_SEND_ACTIVE)
            continue;
        snprintf(name, sizeof name, "%s", send.name);
        if (state == PH_SEND_STORED)
            snprintf(line, sizeof line, "Sent %s", name);
        else if (state == PH_SEND_CANCELED)
            snprintf(line, sizeof line, "Cancelled %s", name);
        else if (state == PH_SEND_CHANGED)
            snprintf(line, sizeof line, "Source changed: %.96s", name);
        else
            snprintf(line, sizeof line, "Could not send %s", name);
        ph_window_note_transfer(a->win, line);
        ph_window_set_detail(a->win, line);
    }
    if (!ph_origin_push_busy(a->origin, name, sizeof name))
        return;
    snprintf(line, sizeof line, "Sending %s", name[0] ? name : "file");
    ph_window_set_transfer_percent(a->win, ph_origin_push_percent(a->origin));
    if (ph_window_set_active_transfer(a->win, line)) {
        ph_window_show_transfers(a->win);
        ph_window_set_detail(a->win, line);
    }
}

static bool retry_history_file(app *a, const ph_file_entry *entry, bool restart)
{
    if (entry->incoming)
        return false;
    ph_origin_send send = {.history_id = entry->id, .bytes = entry->bytes,
                           .confirmed = entry->confirmed};
    memcpy(send.path, entry->path, sizeof send.path);
    snprintf(send.name, sizeof send.name, "%.95s", entry->name);
    memcpy(send.id, entry->transfer_id, sizeof send.id);
    memcpy(send.digest, entry->digest, sizeof send.digest);
    memcpy(send.resume_key, entry->resume_key, sizeof send.resume_key);
    if (!ph_origin_retry_file(a->origin, &send, restart))
        return false;
    track_push(a);
    return true;
}

static void save_window_prefs(app *a)
{
    bool ok = ph_config_write_line("ctrl-command",
                                   ph_window_ctrl_is_command(a->win) ? "1" : "0")
        && ph_config_write_line("release-on-blur",
                                ph_window_release_on_blur(a->win) ? "1" : "0")
        && ph_config_write_line("pointer-mode",
                                ph_window_next_relative(a->win) ? "relative" : "absolute")
        && ph_config_write_line("media-keys", ph_window_media_keys(a->win) ? "1" : "0");
    ph_window_set_detail(a->win, ok ? "Settings saved." : "Could not save these settings");
}

static void do_window_action(app *a, ph_window_action action, bool *running)
{
    switch (action) {
    case PH_WINDOW_ACTION_NONE:
        break;
    case PH_WINDOW_ACTION_FEED:
        cancel_control_intent(a, SDL_GetTicks64());
        a->feed_mode = !a->feed_mode;
        update_title(a);
        PH_LOGI("feed wheel: %s", a->feed_mode ? "on (one swipe per wheel burst)" : "off");
        break;
    case PH_WINDOW_ACTION_FORGET:
        if (a->origin)
            ph_origin_forget(a->origin);
        ph_window_set_detail(a->win, "iPhone removed. Enter the connection code to pair again.");
        break;
    case PH_WINDOW_ACTION_PAUSE:
        a->controls_paused = !a->controls_paused;
        ph_config_write_line("controls-paused", a->controls_paused ? "1" : "0");
        if (a->controls_paused) {
            ph_input_release_all(&a->input);
            if (a->absolute)
                ph_abs_release_all(&a->abs, SDL_GetTicks64());
            cancel_control_intent(a, SDL_GetTicks64());
        }
        ph_input_set_enabled(&a->input, may_forward_controls(a));
        ph_pointer_set_enabled(&a->pointer, a->input.enabled && a->phone_name[0]
                               && !a->presentation_mode, SDL_GetTicks64());
        update_title(a);
        ph_window_set_detail(a->win, a->controls_paused ? "PC controls disabled"
                                        : a->input.enabled ? "PC controls enabled"
                                                           : "PC controls requested");
        break;
    case PH_WINDOW_ACTION_MUTE:
        if (a->audio) {
            struct ph_audio_settings settings;
            bool muted = false;
            if (ph_audio_settings(a->audio, &settings))
                muted = settings.muted;
            ph_audio_set_muted(a->audio, !muted);
            ph_window_set_detail(a->win, muted ? "Sound on" : "Sound muted");
        }
        break;
    case PH_WINDOW_ACTION_QUIT:
        *running = false;
        break;
    case PH_WINDOW_ACTION_SAVE:
        save_window_prefs(a);
        break;
    case PH_WINDOW_ACTION_PICK_FOLDER: {
        char dir[1024], label[64];
        if (!ph_place_pick_folder(dir, sizeof dir)) {
            ph_window_set_detail(a->win, "Still saving in the current folder");
            break;
        }
        if (!ph_place_set_save_dir(dir) || !ph_place_save_label(label, sizeof label)) {
            ph_window_set_detail(a->win, "That folder cannot be used");
            break;
        }
        snprintf(dir, sizeof dir, "Files from the iPhone save in %s", label);
        ph_window_set_detail(a->win, dir);
        ph_window_refresh_folder(a->win);
        break;
    }
    case PH_WINDOW_ACTION_SAVE_DOWNLOADS:
        ph_place_set_save_dir(NULL);
        ph_window_set_detail(a->win, "Files from the iPhone save in Downloads");
        ph_window_redraw(a->win);
        break;
    }
}

static void retry_pairing(app *a)
{
    char hint[96];
    if (a->origin) {
        if (ph_origin_refresh_address(a->origin, hint, sizeof hint))
            ph_window_set_pull_hint(a->win, hint);
        return;
    }
    ph_origin_start_status status;
    a->origin = ph_origin_start(hint, sizeof hint, &status);
    ph_window_set_pairing_status(a->win, status);
    ph_window_set_share_available(a->win, a->origin && a->screen);
    if (a->origin) {
        ph_window_set_pull_hint(a->win, hint);
        ph_window_set_code_open(a->win, ph_origin_code_open(a->origin));
        sync_pairing_history(a);
    }
}

static void apply_window_action(app *a, bool *running)
{
    do_window_action(a, ph_window_take_action(a->win), running);
    uint64_t id = 0;
    ph_workspace_action action = ph_window_take_workspace_action(a->win, &id);
    if (action == PH_WORKSPACE_NONE)
        return;
    char path[1024];
    SDL_Window *native = SDL_GetWindowFromID(ph_window_id(a->win));
    ph_workspace *ui = ph_window_workspace(a->win);
    const char *device = ph_workspace_device(ui), *value = ph_workspace_value(ui);
    const char *phone_id = strncmp(device, "phone:", 6) == 0 ? device + 6 : NULL;
    if (action == PH_WORKSPACE_RETRY_PAIRING) {
        retry_pairing(a);
    } else if (action == PH_WORKSPACE_ADD_PHONE) {
        if (a->origin && ph_origin_code_open(a->origin) && !ph_origin_code_expired(a->origin)) {
            ph_window_open_pairing(a->win);
        } else if (ph_origin_add_phone(a->origin, path, sizeof path)) {
            ph_inbox_disconnect(a->inbox);
            ph_screen_disconnect(a->screen);
            sync_pairing_history(a);
            ph_window_set_phone_name(a->win, "");
            ph_window_set_pull_hint(a->win, path);
            ph_window_set_code_open(a->win, true);
            ph_window_open_pairing(a->win);
        } else {
            ph_window_set_detail(a->win, "Could not add an iPhone. Check available device slots and local storage.");
        }
    } else if (action == PH_WORKSPACE_DEVICE_SELECT) {
        release_everything(a);
        if (phone_id) {
            if (ph_origin_choose_phone(a->origin, phone_id)) {
                sync_pairing_history(a);
                ph_window_set_code_open(a->win, false);
                ph_origin_phone_name(a->origin, path, sizeof path);
                ph_window_set_phone_name(a->win, path);
                ph_window_set_detail(a->win, "Open Porthole on the selected iPhone to connect.");
            } else {
                ph_window_set_detail(a->win, "Could not select this iPhone.");
            }
        } else {
            bool usb = !strcmp(device, "usb");
            bool selected = ph_polish_select(a->polish, device);
            if (usb)
                atomic_store(&a->usb_pause_requested, false);
            if (usb || selected) {
                pthread_mutex_lock(&a->src_lock);
                a->airplay_selected = !usb && atomic_load(&a->airplay_on);
                if (a->airplay_selected && a->src.current != PH_SRC_WIFI) {
                    a->src.current = PH_SRC_NONE;
                    ph_decoder_reset(a->dec);
                }
                pthread_mutex_unlock(&a->src_lock);
            }
        }
    } else if (action == PH_WORKSPACE_DEVICE_RENAME) {
        if (phone_id) {
            if (!ph_origin_rename_phone(a->origin, phone_id, value))
                ph_window_set_detail(a->win, "Could not save this device name.");
        } else if (*device) {
            if (!ph_polish_rename(a->polish, device, value))
                ph_window_set_detail(a->win, "Could not save this device name.");
        }
    } else if (action == PH_WORKSPACE_PIN_REQUIRED) {
        ph_session_state state;
        ph_polish_sessions(a->polish, &state);
        if (!ph_polish_require_pin(a->polish, !state.pin_required)) {
            ph_window_set_detail(a->win, "Could not save the Screen Mirroring access setting.");
        } else if (!state.pin_required) {
            ph_receiver_disconnect(a->rx);
        }
    } else if (action == PH_WORKSPACE_PIN_APPROVE || action == PH_WORKSPACE_PIN_REJECT) {
        bool ok = ph_polish_resolve_request(a->polish, device,
                                            action == PH_WORKSPACE_PIN_APPROVE ? value : NULL);
        ph_window_set_detail(a->win, !ok ? "PIN verification failed."
                                     : action == PH_WORKSPACE_PIN_APPROVE
                                         ? "Device approved. Start Screen Mirroring again."
                                         : "Access request rejected.");
    } else if (action == PH_WORKSPACE_DEVICE_DISCONNECT || action == PH_WORKSPACE_DEVICE_FORGET) {
        bool forget = action == PH_WORKSPACE_DEVICE_FORGET;
        if (phone_id || !*device) {
            char selected[65] = "";
            ph_origin_selected_id(a->origin, selected);
            if (phone_id && strcmp(phone_id, selected)) {
                if (!forget || !ph_origin_remove_saved_phone(a->origin, phone_id))
                    ph_window_set_detail(a->win, "Could not remove this device.");
            } else if (forget && ph_origin_renew(a->origin, path, sizeof path)) {
                ph_inbox_disconnect(a->inbox);
                ph_screen_disconnect(a->screen);
                sync_pairing_history(a);
                ph_window_set_phone_name(a->win, "");
                ph_window_set_pull_hint(a->win, path);
                ph_window_set_code_open(a->win, true);
                ph_window_open_pairing(a->win);
            } else if (!forget) {
                ph_inbox_disconnect(a->inbox);
                ph_screen_disconnect(a->screen);
                ph_origin_forget(a->origin);
            } else {
                ph_window_set_detail(a->win, "Could not revoke this pairing. Check local storage.");
            }
        } else if (!strcmp(device, "usb")) {
            atomic_store(&a->usb_pause_requested, true);
        } else {
            ph_session_state state;
            ph_polish_sessions(a->polish, &state);
            for (int i = 0; i < state.count; i++)
                if (!strcmp(state.devices[i].id, device) && state.devices[i].connected &&
                    !state.devices[i].usb)
                    ph_receiver_disconnect(a->rx);
            if (forget && !ph_polish_forget(a->polish, device))
                ph_window_set_detail(a->win, "Could not remove this device. Check local storage.");
        }
    } else if (action == PH_WORKSPACE_CLEAR_ALL_CLIPBOARD) {
        bool ok = ph_history_clear_saved_clips(a->history);
        ph_window_set_detail(a->win, ok ? "Clipboard history cleared for all pairings."
                                       : "Some clipboard history could not be cleared.");
        if (!ok)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Clipboard history",
                                     "Some clipboard history could not be cleared. Check that the configuration folder is available.", native);
    } else if (action == PH_WORKSPACE_SEND_FILES) {
        ph_place_pick_files(queue_selected_file, a);
    } else if (action == PH_WORKSPACE_HIDE) {
        if (a->tray && ph_workspace_tray(ph_window_workspace(a->win)))
            SDL_HideWindow(native);
        else
            *running = false;
    } else if (action == PH_WORKSPACE_VOLUME_UP || action == PH_WORKSPACE_VOLUME_DOWN) {
        if (a->audio)
            ph_audio_adjust_volume_db(a->audio, action == PH_WORKSPACE_VOLUME_UP ? 3.0f : -3.0f);
    } else if (action == PH_WORKSPACE_DISCONNECT) {
        ph_origin_forget(a->origin);
    } else if (action == PH_WORKSPACE_CAPTURES) {
        if (!ph_polish_capture_folder(path, sizeof path) || !ph_place_reveal(path, false))
            ph_window_set_detail(a->win, "Unable to open the Captures folder.");
    } else if (action == PH_WORKSPACE_RECORD || action == PH_WORKSPACE_CAPTURE ||
               action == PH_WORKSPACE_FULLSCREEN || action == PH_WORKSPACE_ALWAYS_ON_TOP) {
        SDL_KeyboardEvent e = {.type = SDL_KEYDOWN};
        e.keysym.scancode = action == PH_WORKSPACE_RECORD    ? SDL_SCANCODE_F9
                            : action == PH_WORKSPACE_CAPTURE ? SDL_SCANCODE_F8
                            : action == PH_WORKSPACE_ALWAYS_ON_TOP ? SDL_SCANCODE_T
                                                                  : SDL_SCANCODE_RETURN;
        if (action == PH_WORKSPACE_FULLSCREEN || action == PH_WORKSPACE_ALWAYS_ON_TOP)
            e.keysym.mod = KMOD_LALT;
        ph_polish_key(a->polish, &e, &a->input, native, a->picture_w, a->picture_h, a->view_frame,
                      true);
        ph_window_redraw(a->win);
    } else if (action == PH_WORKSPACE_CLIP_COPY || action == PH_WORKSPACE_CLIP_SEND) {
        for (size_t i = 0; i < ph_history_clip_count(a->history); i++) {
            const ph_clip_entry *c = ph_history_clip(a->history, i);
            if (c->id != id)
                continue;
            if (action == PH_WORKSPACE_CLIP_COPY) {
                bool copying = ph_hostclip_apply(c->kind, c->data, c->size);
                a->remote_clipboard_pending = false;
                ph_window_set_detail(a->win, !copying ? "Unable to write to the PC clipboard."
                                  : ph_hostclip_pending() ? "Preparing image for the PC clipboard…"
                                                          : "Copied to clipboard.");
            } else if (!a->companion_connected ||
                     !ph_workspace_clip_send(ph_window_workspace(a->win)) ||
                     !ph_origin_put_clip(a->origin, c->kind, c->data, c->size))
                ph_window_set_detail(
                    a->win,
                    "Unable to send. Check the connection and clipboard settings on both devices.");
            break;
        }
    } else if (action == PH_WORKSPACE_REVEAL && !id) {
        if (!ph_place_save_dir(path, sizeof path) || !ph_place_reveal(path, false))
            ph_window_set_detail(a->win,
                                 "Receive folder unavailable. Choose a folder in Settings.");
    } else if (action == PH_WORKSPACE_REVEAL || action == PH_WORKSPACE_LOCATE ||
               action == PH_WORKSPACE_RETRY || action == PH_WORKSPACE_RESTART) {
        for (size_t i = 0; i < ph_history_file_count(a->history); i++) {
            const ph_file_entry *f = ph_history_file(a->history, i);
            if (f->id != id)
                continue;
            if (action == PH_WORKSPACE_RESTART || action == PH_WORKSPACE_RETRY) {
                if (!retry_history_file(a, f, action == PH_WORKSPACE_RESTART))
                    ph_window_set_detail(
                        a->win,
                        "Unable to resume the transfer. Check the connection and source file.");
            } else if (action == PH_WORKSPACE_REVEAL) {
                if (!ph_place_reveal(f->path, true)) {
                    ph_workspace_refresh_files(ui);
                    ph_file_availability status = ph_history_file_availability(f);
                    ph_window_set_detail(a->win, status == PH_FILE_MISSING
                                                     ? "File not found. Locate it in File history."
                                                 : status == PH_FILE_LOCATION_UNAVAILABLE
                                                     ? "The saved location is unavailable."
                                                     : "File Explorer could not open the saved location.");
                }
            } else if (ph_place_pick_file(path, sizeof path)) {
                if (ph_history_match_file(f, path)) {
                    if (!ph_history_update_file(a->history, id, f->state, f->confirmed, path))
                        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Locate file",
                                                 "The new file location could not be saved.", native);
                    ph_workspace_refresh_files(ui);
                } else
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Locate file",
                                             "The selected file does not match this transfer.", native);
            }
            break;
        }
    }
    ph_window_redraw(a->win);
}

/* The notification-area menu on Windows: same actions as the window's Settings. */
static void follow_tray(app *a, bool *running)
{
    struct ph_audio_settings settings;
    bool sound_on = true;
    char tip[96];
    SDL_Window *native = SDL_GetWindowFromID(ph_window_id(a->win));

    if (!a->tray)
        return;
    switch (ph_wintray_take(a->tray)) {
    case PH_WINTRAY_NONE:
        break;
    case PH_WINTRAY_SHOW:
        SDL_ShowWindow(native);
        SDL_RestoreWindow(native);
        SDL_RaiseWindow(native);
        break;
    case PH_WINTRAY_FEED:
        do_window_action(a, PH_WINDOW_ACTION_FEED, running);
        break;
    case PH_WINTRAY_PAUSE:
        do_window_action(a, PH_WINDOW_ACTION_PAUSE, running);
        break;
    case PH_WINTRAY_SOUND:
        do_window_action(a, PH_WINDOW_ACTION_MUTE, running);
        break;
    case PH_WINTRAY_COPY:
        ph_window_copy_pairing(a->win);
        break;
    case PH_WINTRAY_SHOW_RECEIVED_FILE:
        if (a->notified_file[0] && ph_place_reveal(a->notified_file, true))
            break;
        /* Fall back to transfer history when the saved path is unavailable. */
        /* fall through */
    case PH_WINTRAY_RECEIVED:
        SDL_ShowWindow(native);
        SDL_RestoreWindow(native);
        SDL_RaiseWindow(native);
        release_everything(a);
        ph_window_open_received_history(a->win);
        break;
    case PH_WINTRAY_SETTINGS:
        SDL_ShowWindow(native);
        SDL_RestoreWindow(native);
        SDL_RaiseWindow(native);
        ph_window_open_settings(a->win);
        break;
    case PH_WINTRAY_QUIT:
        *running = false;
        return;
    }
    if (a->audio && ph_audio_settings(a->audio, &settings))
        sound_on = !settings.muted;
    snprintf(tip, sizeof tip, "Porthole%s", a->phone_name[0] ? " - iPhone connected" : "");
    ph_wintray_set_state(a->tray, a->feed_mode, a->controls_paused, sound_on,
                         ph_window_has_pairing(a->win), tip);
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
    {
        char line[32];
        bool ctrl = true, blur = true, next_relative = false;
        if (ph_config_read_line("ctrl-command", line, sizeof line) && line[0])
            ctrl = strcmp(line, "0") != 0;
        if (ph_config_read_line("controls-paused", line, sizeof line))
            a.controls_paused = !strcmp(line, "1");
        if (ph_config_read_line("release-on-blur", line, sizeof line) && line[0])
            blur = strcmp(line, "0") != 0;
        if (ph_config_read_line("pointer-mode", line, sizeof line))
            next_relative = strcmp(line, "relative") == 0;
        ph_window_set_prefs(a.win, ctrl, blur, next_relative);
        if (ph_config_read_line("media-keys", line, sizeof line) && line[0])
            ph_window_set_media_keys(a.win, strcmp(line, "0") != 0);
    }
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
#ifdef _WIN32
    watch_windows_shutdown(SDL_GetWindowFromID(ph_window_id(a.win)));
#endif
    ph_hostclip_init();
    ph_place_install_send_to();
    a.tray = ph_wintray_create();
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
        char pc[64];
        ph_origin_pc_name(pc, sizeof pc);
        ph_window_set_pc_name(a.win, pc);
    }
    {
        ph_screen_callbacks screen_cb = {
            .user = &a,
            .authorize = authorize_channel,
            .connected = screen_connected,
            .video = screen_video,
            .audio = screen_audio,
            .disconnected = screen_disconnected,
        };
        atomic_init(&a.screen_on, false);
        atomic_init(&a.airplay_on, false);
        a.screen = ph_screen_start(&screen_cb);
        retry_pairing(&a);
    }
    if (ensure_pull_dir(&a)) {
        ph_inbox_callbacks inbox_cb = {
            .user = &a,
            .authorize = authorize_inbox,
            .progress = inbox_progress,
            .profile = inbox_profile,
            .item = inbox_item,
            .saved = inbox_saved,
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
    tick_control_connection(&a);
    update_title(&a);
    ph_window_show(a.win);
    if (opt->usb && pthread_create(&a.usb_thread, NULL, usb_thread_main, &a) == 0)
        a.usb_started = true;
    else if (opt->usb)
        PH_LOGW("couldn't start USB screen capture");

    while (running && !stop_requested) {
        SDL_Event e;
        tick_control_connection(&a);
        /* Button holds and video confirmation are timed in this loop: run it more
         * often while input is on its way to the phone. */
        bool busy = a.pointer.phase != PH_POINTER_IDLE || a.pointer.count
                    || a.wheel.in_flight || a.wheel.waiting_frame || ph_gesture_active(&a.gesture)
                    || a.keyboard_lift_at
                    || (a.absolute && (ph_abs_busy(&a.abs) || a.abs.wheel != 0
                                       || (a.abs.want_valid && (a.abs.x != a.abs.want_x
                                                                || a.abs.y != a.abs.want_y))))
                    || ph_transfer_card_animating(a.card);
        bool was_modal = ph_window_help_visible(a.win);
        bool have_event = SDL_WaitEventTimeout(&e, busy ? 4 : 16) != 0;
        if (!was_modal && ph_window_help_visible(a.win))
            release_everything(&a);
        if (have_event) {
            do {
                bool modal = ph_window_help_visible(a.win);
                running = handle_event(&a, &e, frame, &seq) && running;
                apply_window_action(&a, &running);
                if (!modal && ph_window_help_visible(a.win))
                    release_everything(&a);
            } while (running && !stop_requested && SDL_PollEvent(&e));
        }
        if (!running || stop_requested)
            break;
        if (SDL_GetTicks64() - a.session_poll >= 250) {
            ph_session_state state;
            ph_polish_sessions(a.polish, &state);
            ph_origin_sessions(a.origin, &state);
            if (ph_workspace_set_sessions(ph_window_workspace(a.win), &state)) {
                if (state.request_id[0] && strcmp(state.request_id, a.request_id)) {
                    release_everything(&a);
                    ph_workspace_open_section(ph_window_workspace(a.win), PH_SETTINGS_DEVICES);
                    ph_window_open_settings(a.win);
                }
                snprintf(a.request_id, sizeof a.request_id, "%s", state.request_id);
                ph_window_redraw(a.win);
            }
            a.session_poll = SDL_GetTicks64();
        }
        ph_window_tick(a.win);
        if (SDL_GetTicks64() - a.address_poll_at >= 5000) {
            char hint[96];
            if (ph_origin_refresh_address(a.origin, hint, sizeof hint))
                ph_window_set_pull_hint(a.win, hint);
            a.address_poll_at = SDL_GetTicks64();
        }
        bool replace_pairing = ph_window_take_new_code(a.win);
        if ((replace_pairing || ph_origin_code_expired(a.origin)) && a.origin) {
            char hint[96];
            if (ph_origin_renew(a.origin, hint, sizeof hint) && hint[0]) {
                ph_inbox_disconnect(a.inbox);
                ph_screen_disconnect(a.screen);
                sync_pairing_history(&a);
                ph_window_set_phone_name(a.win, "");
                ph_window_set_pull_hint(a.win, hint);
            }
        }
        sync_pairing_history(&a);
        apply_window_action(&a, &running);
        follow_tray(&a, &running);
        if (!running || stop_requested)
            break;
#ifdef _WIN32
        /* A release Windows gave to another window must still lift the phone's
         * button and end a photo drag. Shortcut gestures hold buttons without the
         * mouse, so only presses that came from the mouse are lifted here. */
        {
            Uint32 held = SDL_GetGlobalMouseState(NULL, NULL)
                          & (SDL_BUTTON_LMASK | SDL_BUTTON_MMASK | SDL_BUTTON_RMASK);
            if (a.absolute && a.mouse_abs_down && !held) {
                for (uint8_t bit = 1; bit <= 4; bit <<= 1)
                    if (a.mouse_abs_down & bit)
                        ph_abs_button(&a.abs, bit, false, a.abs.want_x, a.abs.want_y, SDL_GetTicks64());
                a.mouse_abs_down = 0;
                SDL_CaptureMouse(SDL_FALSE);
                PH_LOGW("pointer: the release went to another window; lifted the iPhone button");
            }
            if (a.pull.pulling && !a.pull_hold && !(held & SDL_BUTTON_LMASK))
                finish_pull(&a);
        }
#endif
        {
            struct ph_audio_settings sound = {0};
            bool sound_on = !a.audio || !ph_audio_settings(a.audio, &sound) || !sound.muted;
            ph_control_policy policy = ph_origin_control_policy(a.origin);
            a.companion_connected = policy.connected;
            ph_window_set_input_policy(a.win, a.link && !a.link_stopping &&
                                              !strcmp(a.ble_status, "on"), policy);
            ph_window_set_companion(a.win, a.companion_connected, ph_polish_recording(a.polish),
                                    (int)lroundf(100 * powf(10, sound.volume_db / 20)));
            ph_window_set_live_state(a.win, a.feed_mode, a.controls_paused, sound_on);
        }
        if (ph_window_take_cancel(a.win) && a.origin)
            ph_origin_cancel_push(a.origin);
        const char *capture_notice = ph_polish_capture_notice(a.polish);
        if (capture_notice)
            ph_window_set_detail(a.win, capture_notice);
        track_push(&a);
        sync_clipboard(&a);
        follow_pull(&a);
        ph_transfer_card_set_theme(a.card, ph_workspace_dark(ph_window_workspace(a.win)));
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
        observe_pointer(&a, true);
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
        tick_control_connection(&a);
    }
    rc = 0;

done:
    ph_xdnd_cancel();
    hide_ghost(&a);
    ph_transfer_card_destroy(a.card);
    ph_wintray_destroy(a.tray);
    a.card = NULL;
    ph_drag_ghost_destroy(a.ghost);
    a.ghost = NULL;
    ph_screen_stop(a.screen); /* before the origin its allow callback reads */
    ph_inbox_stop(a.inbox);
    ph_origin_stop(atomic_exchange(&a.origin, NULL));
    ph_hostclip_shutdown();
    if (a.usb_started) {
        atomic_store(&a.usb_stop, true);
        atomic_store(&a.usb_pause_requested, true);
        pthread_join(a.usb_thread, NULL);
    }
    ph_receiver_stop(a.rx); /* join AirPlay callbacks before destroying their source lock */
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
    if (a.polish && a.win)
        ph_polish_save_window(a.polish, SDL_GetWindowFromID(ph_window_id(a.win)));
    ph_polish_destroy(a.polish);
    ph_audio_destroy(a.audio);
    ph_decoder_stop(a.dec);
    if (a.screen_dec)
        ph_decoder_stop(a.screen_dec);
    discard_pending_events(&a); /* all event producers have stopped */
    ph_window_destroy(a.win);
    ph_history_close(a.history);
    av_frame_free(&frame);
    av_frame_free(&a.view_frame);
    av_frame_free(&a.pointer_baseline);
    av_frame_free(&a.pointer_reference);
    av_frame_free(&a.pointer_after);
    av_frame_free(&a.pull_frame);
    SDL_Quit();
    return rc;
}
