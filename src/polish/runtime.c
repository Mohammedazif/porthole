#include "polish/runtime.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

#include <libavutil/frame.h>

#include "audio/player.h"
#include "control/paste.h"
#include "core/config.h"
#include "core/log.h"
#include "media/record.h"
#include "media/shot.h"
#include "security/gate.h"
#include "session/devices.h"
#include "touch/wda.h"
#include "touch/wda_net.h"
#include "ui/chrome.h"
#include "ui/tray.h"
#include "video/codecs.h"

#define PASTE_MAX 2048

struct ph_polish {
    pthread_mutex_t lock;
    ph_gate *gate;
    ph_devices *devices;
    ph_tray *tray;
    ph_placement place;
    ph_audio *audio;
    ph_record *record;
    ph_wda *wda;
    ph_wda_tcp *tcp;
    char allow_path[512];
    char current_id[129];
    char pin[8];
    char typed[5];
    int layout_w, layout_h;
    bool precise;
    bool pin_required;
    bool logged_hevc_record;
    double press_x, press_y;
    bool pressed;
    ph_paste_event paste[PASTE_MAX];
    size_t paste_i, paste_n;
};

static bool keyframe_annexb(const uint8_t *data, size_t len)
{
    size_t i = 0;
    if (!data)
        return false;
    while (i + 3 < len) {
        size_t start = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            start = 3;
        else if (i + 3 < len && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
                 data[i + 3] == 1)
            start = 4;
        if (!start) {
            i++;
            continue;
        }
        if (i + start < len) {
            unsigned avc = data[i + start] & 0x1Fu;
            unsigned hevc = (data[i + start] >> 1) & 0x3Fu;
            if (avc == 5 || avc == 7 || hevc == 19 || hevc == 20 || hevc == 32)
                return true;
        }
        i += start + 1;
    }
    return false;
}

static void capture_window(SDL_Window *window, ph_placement *place)
{
    if (!window)
        return;
    SDL_GetWindowPosition(window, &place->x, &place->y);
    SDL_GetWindowSize(window, &place->w, &place->h);
    place->fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    place->valid = place->w > 0 && place->h > 0;
}

static void write_autostart(const char *exe)
{
#ifdef _WIN32
    (void)exe;
    PH_LOGW("--autostart writes a Linux desktop entry and is unavailable on Windows");
#else
    char body[1024];
    char path[512];
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CONFIG_HOME");
    FILE *file;

    if (!exe || !exe[0] || !ph_autostart_desktop(body, sizeof body, exe, true))
        return;
    if (xdg && xdg[0])
        snprintf(path, sizeof path, "%s/autostart", xdg);
    else if (home && home[0])
        snprintf(path, sizeof path, "%s/.config/autostart", home);
    else
        return;
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return;
    {
        char file_path[700];
        snprintf(file_path, sizeof file_path, "%s/porthole.desktop", path);
        file = fopen(file_path, "w");
        if (!file)
            return;
        fputs(body, file);
        fclose(file);
        PH_LOGI("autostart file %s", file_path);
    }
#endif
}

static void parse_wda(const char *text, char *host, size_t host_len, int *port)
{
    const char *colon;
    size_t n;

    *port = 8100;
    if (!text || !text[0]) {
        host[0] = '\0';
        return;
    }
    colon = strrchr(text, ':');
    if (!colon || colon == text) {
        snprintf(host, host_len, "%s", text);
        return;
    }
    n = (size_t)(colon - text);
    if (n >= host_len)
        n = host_len - 1;
    memcpy(host, text, n);
    host[n] = '\0';
    *port = atoi(colon + 1);
    if (*port <= 0 || *port > 65535)
        *port = 8100;
}

ph_polish *ph_polish_create(const ph_polish_options *opt)
{
    ph_polish *polish = calloc(1, sizeof *polish);
    char line[160];

    if (!polish)
        return NULL;
    pthread_mutex_init(&polish->lock, NULL);
    polish->gate = ph_gate_create();
    polish->devices = ph_devices_create();
    polish->tray = ph_tray_create();
    polish->place.valid = false;
    if (!polish->gate || !polish->devices || !polish->tray) {
        ph_polish_destroy(polish);
        return NULL;
    }
    polish->pin_required = opt && opt->pin_required;
    ph_gate_set_open(polish->gate, !polish->pin_required);
    if (ph_config_path("allow-list", polish->allow_path, sizeof polish->allow_path))
        ph_gate_load(polish->gate, polish->allow_path);
    if (ph_config_read_line("window-placement", line, sizeof line))
        ph_placement_parse(line, &polish->place);
    if (opt && opt->autostart)
        write_autostart(opt->exe_path);
    if (opt && opt->precise_touch && opt->precise_touch[0]) {
        char host[256];
        int port = 8100;
        parse_wda(opt->precise_touch, host, sizeof host, &port);
        polish->wda = ph_wda_create(host, port);
        if (polish->wda)
            polish->tcp = ph_wda_tcp_open(polish->wda, host, port);
        if (polish->tcp && ph_wda_create_session(polish->wda) == 0) {
            polish->precise = true;
            PH_LOGI("precise touch via WebDriverAgent at %s:%d", host, port);
        } else {
            PH_LOGW("precise touch unavailable; mouse control stays on Bluetooth");
            ph_wda_tcp_close(polish->tcp);
            polish->tcp = NULL;
            ph_wda_destroy(polish->wda);
            polish->wda = NULL;
        }
    }
    return polish;
}

void ph_polish_destroy(ph_polish *polish)
{
    if (!polish)
        return;
    if (polish->record)
        ph_record_close(polish->record);
    if (polish->allow_path[0])
        ph_gate_save(polish->gate, polish->allow_path);
    ph_wda_tcp_close(polish->tcp);
    ph_wda_destroy(polish->wda);
    ph_tray_destroy(polish->tray);
    ph_devices_destroy(polish->devices);
    ph_gate_destroy(polish->gate);
    pthread_mutex_destroy(&polish->lock);
    free(polish);
}

void ph_polish_set_audio(ph_polish *polish, ph_audio *audio)
{
    if (polish)
        polish->audio = audio;
}

void ph_polish_apply_saved(ph_polish *polish, SDL_Window *window)
{
    if (!polish || !polish->place.valid)
        return;
    ph_chrome_apply(window, &polish->place);
}

void ph_polish_save_window(ph_polish *polish, SDL_Window *window)
{
    char line[160];

    if (!polish)
        return;
    capture_window(window, &polish->place);
    if (!polish->place.valid)
        return;
    if (ph_placement_format(&polish->place, line, sizeof line))
        ph_config_write_line("window-placement", line);
}

void ph_polish_layout(ph_polish *polish, SDL_Window *window, int video_w, int video_h)
{
    SDL_Rect bounds;
    int display;

    if (!polish || !polish->place.one_to_one || polish->place.fullscreen || !window)
        return;
    if (video_w <= 0 || video_h <= 0)
        return;
    if (polish->layout_w == video_w && polish->layout_h == video_h)
        return;
    display = SDL_GetWindowDisplayIndex(window);
    if (display < 0)
        display = 0;
    if (SDL_GetDisplayUsableBounds(display, &bounds) != 0) {
        bounds.x = bounds.y = 0;
        bounds.w = video_w;
        bounds.h = video_h;
    }
    {
        ph_window_insets insets = {0, 0, 0, 0};
        int top = 0, left = 0, bottom = 0, right = 0;
        int cur_x = 0, cur_y = 0, cur_w = 0, cur_h = 0;
        int inner_w, inner_h;
        bool center = polish->layout_w <= 0 ||
                      ((video_w > video_h) != (polish->layout_w > polish->layout_h));

        if (SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right) == 0 && top >= 0 &&
            left >= 0 && bottom >= 0 && right >= 0) {
            insets.top = top;
            insets.left = left;
            insets.bottom = bottom;
            insets.right = right;
        }
        inner_w = bounds.w - insets.left - insets.right;
        inner_h = bounds.h - insets.top - insets.bottom;
        if (inner_w < 1)
            inner_w = 1;
        if (inner_h < 1)
            inner_h = 1;
        SDL_GetWindowPosition(window, &cur_x, &cur_y);
        SDL_GetWindowSize(window, &cur_w, &cur_h);
        ph_chrome_one_to_one(video_w, video_h, inner_w, inner_h, &polish->place.w, &polish->place.h);
        ph_chrome_place_client(polish->place.w, polish->place.h, bounds.x, bounds.y, bounds.w,
                               bounds.h, &insets, cur_x, cur_y, cur_w, cur_h, center,
                               &polish->place.x, &polish->place.y);
    }
    polish->place.valid = true;
    polish->layout_w = video_w;
    polish->layout_h = video_h;
    ph_chrome_apply(window, &polish->place);
}

bool ph_polish_admit(ph_polish *polish, const char *device_id, const char *name,
                     const char *model)
{
    ph_gate_decision decision;
    bool admit = false;

    if (!polish)
        return true;
    if (!device_id || !device_id[0])
        return !polish->pin_required;
    pthread_mutex_lock(&polish->lock);
    decision = ph_gate_decide(polish->gate, device_id, name, model);
    if (decision == PH_GATE_ADMIT) {
        snprintf(polish->current_id, sizeof polish->current_id, "%s", device_id ? device_id : "");
        ph_devices_upsert(polish->devices, device_id, name, model, PH_TRANSPORT_WIFI);
        polish->pin[0] = '\0';
        polish->typed[0] = '\0';
        if (polish->allow_path[0])
            ph_gate_save(polish->gate, polish->allow_path);
        admit = true;
    } else if (decision == PH_GATE_PIN) {
        const char *pin = ph_gate_pending_pin(polish->gate);
        snprintf(polish->pin, sizeof polish->pin, "%s", pin ? pin : "");
        polish->typed[0] = '\0';
        PH_LOGI("allow %s with PIN %s (type the four digits in the mirror window)",
                name && name[0] ? name : device_id, polish->pin);
    } else {
        polish->pin[0] = '\0';
        PH_LOGW("rejected connection from %s", device_id ? device_id : "?");
    }
    pthread_mutex_unlock(&polish->lock);
    return admit;
}

void ph_polish_disconnected(ph_polish *polish)
{
    if (!polish)
        return;
    pthread_mutex_lock(&polish->lock);
    if (polish->current_id[0])
        ph_devices_disconnect(polish->devices, polish->current_id);
    polish->current_id[0] = '\0';
    pthread_mutex_unlock(&polish->lock);
}

bool ph_polish_accepts_video(ph_polish *polish)
{
    const char *active;
    bool ok = true;

    if (!polish)
        return true;
    pthread_mutex_lock(&polish->lock);
    active = ph_devices_active_id(polish->devices);
    if (active && polish->current_id[0])
        ok = strcmp(active, polish->current_id) == 0;
    pthread_mutex_unlock(&polish->lock);
    return ok;
}

void ph_polish_usb_present(ph_polish *polish, bool present)
{
    if (!polish)
        return;
    pthread_mutex_lock(&polish->lock);
    if (present) {
        ph_devices_upsert(polish->devices, "usb", "USB screen", "", PH_TRANSPORT_USB);
        ph_devices_select(polish->devices, "usb");
    } else {
        ph_devices_disconnect(polish->devices, "usb");
    }
    pthread_mutex_unlock(&polish->lock);
}

bool ph_polish_accepts_usb(ph_polish *polish)
{
    const char *active;
    bool ok = false;

    if (!polish)
        return true;
    pthread_mutex_lock(&polish->lock);
    active = ph_devices_active_id(polish->devices);
    ok = active && strcmp(active, "usb") == 0;
    pthread_mutex_unlock(&polish->lock);
    return ok;
}

bool ph_polish_copy_pin(ph_polish *polish, char *out, size_t out_len)
{
    bool have = false;

    if (!polish || !out || out_len == 0)
        return false;
    pthread_mutex_lock(&polish->lock);
    if (polish->pin[0]) {
        snprintf(out, out_len, "%s", polish->pin);
        have = true;
    }
    pthread_mutex_unlock(&polish->lock);
    return have;
}

void ph_polish_on_video(ph_polish *polish, const uint8_t *annexb, size_t len, uint64_t pts_ns)
{
    if (!polish || !annexb || len == 0)
        return;
    pthread_mutex_lock(&polish->lock);
    if (polish->record) {
        if (ph_codec_from_annexb(annexb, len) == PH_CODEC_H265) {
            if (!polish->logged_hevc_record) {
                PH_LOGW("recording stores H.264 only; this H.265 stream is shown but not saved");
                polish->logged_hevc_record = true;
            }
        } else {
            ph_record_video(polish->record, annexb, len, pts_ns, keyframe_annexb(annexb, len));
        }
    }
    pthread_mutex_unlock(&polish->lock);
}

static void toggle_fullscreen(ph_polish *polish, SDL_Window *window)
{
    capture_window(window, &polish->place);
    polish->place.fullscreen = !polish->place.fullscreen;
    polish->place.valid = true;
    ph_chrome_apply(window, &polish->place);
}

static void toggle_top(ph_polish *polish, SDL_Window *window)
{
    capture_window(window, &polish->place);
    polish->place.always_on_top = !polish->place.always_on_top;
    polish->place.valid = true;
    ph_chrome_apply(window, &polish->place);
    PH_LOGI("always on top: %s", polish->place.always_on_top ? "on" : "off");
}

static void toggle_pixels(ph_polish *polish, SDL_Window *window, int video_w, int video_h)
{
    capture_window(window, &polish->place);
    polish->place.one_to_one = !polish->place.one_to_one;
    polish->place.fullscreen = false;
    polish->layout_w = 0;
    polish->layout_h = 0;
    if (polish->place.one_to_one)
        ph_polish_layout(polish, window, video_w, video_h);
    else
        ph_chrome_apply(window, &polish->place);
}

static void queue_paste(ph_polish *polish)
{
    char text[8192];
    int n = ph_paste_clipboard(text, sizeof text);

    if (n < 0) {
        PH_LOGW("clipboard paste unavailable");
        return;
    }
    polish->paste_n = ph_paste_encode(text, polish->paste, PASTE_MAX);
    polish->paste_i = 0;
    PH_LOGI("pasting %d clipboard characters", n);
}

static bool take_pin_digit(ph_polish *polish, SDL_Keycode key)
{
    int digit = -1;
    const char *pending;

    pthread_mutex_lock(&polish->lock);
    pending = polish->pin[0] ? polish->pin : NULL;
    if (!pending) {
        pthread_mutex_unlock(&polish->lock);
        return false;
    }
    if (key >= SDLK_0 && key <= SDLK_9)
        digit = (int)(key - SDLK_0);
    else if (key == SDLK_KP_0)
        digit = 0;
    else if (key >= SDLK_KP_1 && key <= SDLK_KP_9)
        digit = (int)(key - SDLK_KP_1) + 1;
    else if (key == SDLK_BACKSPACE) {
        size_t n = strlen(polish->typed);
        if (n)
            polish->typed[n - 1] = '\0';
        pthread_mutex_unlock(&polish->lock);
        return true;
    } else {
        pthread_mutex_unlock(&polish->lock);
        return false;
    }
    if (strlen(polish->typed) < 4) {
        size_t n = strlen(polish->typed);
        polish->typed[n] = (char)('0' + digit);
        polish->typed[n + 1] = '\0';
    }
    if (strlen(polish->typed) == 4) {
        const char *id = ph_gate_pending_id(polish->gate);
        bool ok = id && ph_gate_submit_pin(polish->gate, id, polish->typed);
        polish->typed[0] = '\0';
        if (ok) {
            polish->pin[0] = '\0';
            if (polish->allow_path[0])
                ph_gate_save(polish->gate, polish->allow_path);
            PH_LOGI("device allowed; start screen mirroring again");
        } else {
            PH_LOGW("PIN did not match");
        }
    }
    pthread_mutex_unlock(&polish->lock);
    return true;
}

static void cycle_device(ph_polish *polish)
{
    int count, active = -1, i;
    ph_device_view view;

    pthread_mutex_lock(&polish->lock);
    count = ph_devices_count(polish->devices);
    if (count <= 0) {
        pthread_mutex_unlock(&polish->lock);
        return;
    }
    for (i = 0; i < count; i++) {
        if (ph_devices_at(polish->devices, i, &view) && view.active)
            active = i;
    }
    for (i = 1; i <= count; i++) {
        int next = (active + i) % count;
        if (ph_devices_at(polish->devices, next, &view) && view.connected &&
            ph_devices_select(polish->devices, view.id)) {
            PH_LOGI("showing %s (%s)", view.name ? view.name : view.id,
                    view.transport == PH_TRANSPORT_USB
                        ? "USB"
                        : (view.kind == PH_DEVICE_IPAD ? "iPad" : "iPhone"));
            break;
        }
    }
    pthread_mutex_unlock(&polish->lock);
}

static bool open_record(ph_polish *polish)
{
    char dir[512], path[640];

    if (!ph_config_dir(dir, sizeof dir))
        return false;
    snprintf(path, sizeof path, "%s/porthole-%lld.mp4", dir, (long long)time(NULL));
    {
        ph_record *opened = ph_record_open(path);
        if (!opened)
            return false;
        pthread_mutex_lock(&polish->lock);
        polish->record = opened;
        polish->logged_hevc_record = false;
        pthread_mutex_unlock(&polish->lock);
    }
    ph_tray_invoke(polish->tray, PH_TRAY_TOGGLE_RECORD);
    PH_LOGI("recording %s", path);
    return true;
}

static void close_record(ph_polish *polish)
{
    ph_record *record;

    pthread_mutex_lock(&polish->lock);
    record = polish->record;
    polish->record = NULL;
    pthread_mutex_unlock(&polish->lock);
    ph_record_close(record);
    if (ph_tray_recording(polish->tray))
        ph_tray_invoke(polish->tray, PH_TRAY_TOGGLE_RECORD);
    PH_LOGI("recording stopped");
}

static void save_shot(const struct AVFrame *frame)
{
    char dir[512], path[640];

    if (!frame || !frame->data[0]) {
        PH_LOGW("no picture to save yet");
        return;
    }
    if (!ph_config_dir(dir, sizeof dir))
        return;
    snprintf(path, sizeof path, "%s/porthole-%lld.png", dir, (long long)time(NULL));
    if (ph_shot_write_png(frame, path) == 0)
        PH_LOGI("saved %s", path);
    else
        PH_LOGW("couldn't save %s", path);
}

bool ph_polish_key(ph_polish *polish, const SDL_KeyboardEvent *event, ph_input *input,
                   SDL_Window *window, int video_w, int video_h, const struct AVFrame *frame,
                   bool hotkeys)
{
    SDL_Keycode key;
    SDL_Scancode sc;
    bool alt;

    (void)input;
    if (!polish || !event)
        return false;
    if (event->type != SDL_KEYDOWN || event->repeat)
        return false;
    key = event->keysym.sym;
    sc = event->keysym.scancode;
    if (take_pin_digit(polish, key))
        return true;
    if (!hotkeys)
        return false;
    alt = (event->keysym.mod & KMOD_LALT) != 0;
    if (sc == SDL_SCANCODE_F8) {
        save_shot(frame);
        return true;
    }
    if (sc == SDL_SCANCODE_F9) {
        if (polish->record)
            close_record(polish);
        else if (!open_record(polish))
            PH_LOGW("couldn't start recording");
        return true;
    }
    if (sc == SDL_SCANCODE_F6) {
        cycle_device(polish);
        return true;
    }
    if (!alt)
        return false;
    if (sc == SDL_SCANCODE_RETURN) {
        toggle_fullscreen(polish, window);
        return true;
    }
    if (sc == SDL_SCANCODE_T) {
        toggle_top(polish, window);
        return true;
    }
    if (sc == SDL_SCANCODE_1) {
        toggle_pixels(polish, window, video_w, video_h);
        return true;
    }
    if (sc == SDL_SCANCODE_V) {
        queue_paste(polish);
        return true;
    }
    if (sc == SDL_SCANCODE_M) {
        bool muted;
        ph_tray_invoke(polish->tray, PH_TRAY_TOGGLE_MUTE);
        muted = ph_tray_muted(polish->tray);
        if (polish->audio)
            ph_audio_set_muted(polish->audio, muted);
        PH_LOGI("PC audio %s", muted ? "muted" : "on");
        return true;
    }
    if (sc == SDL_SCANCODE_EQUALS || sc == SDL_SCANCODE_MINUS) {
        if (polish->audio)
            ph_audio_adjust_volume_db(polish->audio, sc == SDL_SCANCODE_EQUALS ? 2.f : -2.f);
        return true;
    }
    if (sc == SDL_SCANCODE_COMMA || sc == SDL_SCANCODE_PERIOD) {
        struct ph_audio_settings settings;
        int delta = sc == SDL_SCANCODE_PERIOD ? 10 : -10;
        if (polish->audio && ph_audio_settings(polish->audio, &settings))
            ph_audio_set_sync_offset_ms(polish->audio, settings.sync_offset_ms + delta);
        return true;
    }
    return false;
}

void ph_polish_tick(ph_polish *polish, ph_input *input)
{
    ph_paste_event event;

    if (!polish || !input || polish->paste_i >= polish->paste_n)
        return;
    if (!input->enabled)
        return;
    event = polish->paste[polish->paste_i++];
    ph_input_type(input, event.usage, event.modifiers);
}

bool ph_polish_quit(const ph_polish *polish)
{
    return polish && ph_tray_quit_requested(polish->tray);
}

bool ph_polish_precise(const ph_polish *polish)
{
    return polish && polish->precise;
}

bool ph_polish_click(ph_polish *polish, bool down, double nx, double ny, int video_w, int video_h)
{
    double x, y;

    if (!polish || !polish->precise || !polish->wda || video_w <= 0 || video_h <= 0)
        return false;
    x = nx * video_w;
    y = ny * video_h;
    if (down) {
        polish->press_x = x;
        polish->press_y = y;
        polish->pressed = true;
        return true;
    }
    if (!polish->pressed)
        return false;
    polish->pressed = false;
    if ((x - polish->press_x) * (x - polish->press_x) +
            (y - polish->press_y) * (y - polish->press_y) < 64.0)
        return ph_wda_tap(polish->wda, polish->press_x, polish->press_y) == 0;
    return ph_wda_drag(polish->wda, polish->press_x, polish->press_y, x, y, 0.25) == 0;
}
