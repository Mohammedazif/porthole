/* Run the real app/helper lifecycle with headless video and no Bluetooth hardware. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL.h>

#include "app.h"
#include "airplay/receiver.h"
#include "ui/window.h"
#include "ui/drag_ghost.h"
#include "video/decoder.h"

struct ph_window { int unused; };
struct ph_decoder { int unused; };
struct ph_receiver { int unused; };
static ph_window window;
static ph_decoder decoder;
static ph_receiver receiver;
static int windows_created, decoders_started, receivers_started;
static int decoders_stopped, receivers_stopped, ready_count;
static bool timed_out, ack_timeout_mode, healthy_no_video, motion_injected;
static Uint64 first_tick, motion_at, recovered_at;

static void quit(void)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_QUIT;
    SDL_PushEvent(&event);
}

void ph_window_show(ph_window *w) { (void)w; }
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
void ph_window_open_received_history(ph_window *w) { (void)w; }
void ph_window_open_pairing(ph_window *w) { (void)w; }
void ph_window_set_history_profile(ph_window *w, const char *profile)
{
    (void)w;
    (void)profile;
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
    windows_created++;
    return &window;
}

void ph_window_set_detail(ph_window *w, const char *detail) { (void)w, (void)detail; }
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
void ph_window_display_changed(ph_window *w) { (void)w; }
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

void ph_window_set_title(ph_window *w, const char *title)
{
    (void)w;
    if (strstr(title, "[Bluetooth control: on]") && ++ready_count == 2) {
        recovered_at = SDL_GetTicks64();
        quit();
    }
}
void ph_window_set_status(ph_window *w, bool mirroring, bool ready, const char *detail)
{
    (void)w, (void)mirroring, (void)ready, (void)detail;
}
void ph_window_toggle_help(ph_window *w) { (void)w; }
bool ph_window_help_visible(const ph_window *w) { (void)w; return false; }
void ph_window_set_setup(ph_window *w, const ph_setup *setup) { (void)w; (void)setup; }
ph_setup_action ph_window_setup_click(const ph_window *w, int x, int y)
{ (void)w; (void)x; (void)y; return PH_SETUP_ACTION_NONE; }
void ph_window_toggle_diagnostics(ph_window *w) { (void)w; }
void ph_window_toggle_statistics(ph_window *w) { (void)w; }
void ph_window_set_input_policy(ph_window *w, bool ready, ph_control_policy policy)
{
    (void)w;
    (void)ready;
    (void)policy;
}
void ph_window_set_metrics(ph_window *w, const ph_metrics_snapshot *m, unsigned int queued,
                           uint64_t discarded, uint64_t superseded)
{ (void)w; (void)m; (void)queued; (void)discarded; (void)superseded; }

void ph_window_tick(ph_window *w)
{
    (void)w;
    if (!first_tick)
        first_tick = SDL_GetTicks64();
    if (SDL_GetTicks64() - first_tick > (ack_timeout_mode ? 12000U : 10000U)) {
        timed_out = true;
        quit();
    }
    if (ack_timeout_mode && ready_count == 1 && !motion_injected) {
        motion_injected = true;
        motion_at = SDL_GetTicks64();
        SDL_Event event;
        SDL_zero(event);
        event.type = SDL_MOUSEMOTION;
        event.motion.windowID = 1;
        event.motion.x = 250;
        event.motion.y = 700;
        SDL_PushEvent(&event);
    }
    /* The actual helper process acknowledges every paced fence, while video
     * stays absent. Video confirmation failure must not kill healthy input. */
    if (healthy_no_video && motion_injected && SDL_GetTicks64() - motion_at >= 4600)
        quit();
}

void ph_window_present(ph_window *w, const struct AVFrame *frame, const ph_rect *screen)
{
    (void)w, (void)frame, (void)screen;
}
void ph_window_redraw(ph_window *w) { (void)w; }
void ph_window_clear(ph_window *w) { (void)w; }
unsigned int ph_window_id(const ph_window *w) { (void)w; return 1; }
void ph_window_destroy(ph_window *w) { (void)w; }
bool ph_window_chrome_event(ph_window *w, const SDL_Event *event)
{
    (void)w;
    (void)event;
    return false;
}

bool ph_window_to_video(const ph_window *w, int x, int y, double *vx, double *vy)
{
    (void)w;
    if (!ack_timeout_mode || x < 0 || x >= 500 || y < 0 || y >= 1080)
        return false;
    *vx = (x + .5) / 500;
    *vy = (y + .5) / 1080;
    return true;
}
void ph_window_video_size(const ph_window *w, int *width, int *height)
{
    (void)w;
    *width = ack_timeout_mode ? 500 : 0;
    *height = ack_timeout_mode ? 1080 : 0;
}

ph_decoder *ph_decoder_start(void (*on_frame)(void *user), void *user)
{
    (void)on_frame, (void)user;
    decoders_started++;
    return &decoder;
}
void ph_decoder_push(ph_decoder *d, const uint8_t *data, size_t len)
{
    (void)d, (void)data, (void)len;
}
void ph_decoder_push_timestamped(ph_decoder *d, const uint8_t *data, size_t len, uint64_t pts_ns)
{
    (void)pts_ns;
    ph_decoder_push(d, data, len);
}
void ph_decoder_reset(ph_decoder *d) { (void)d; }
bool ph_decoder_latest(ph_decoder *d, struct AVFrame *dst, uint64_t *seq)
{
    (void)d, (void)dst, (void)seq;
    return false;
}
bool ph_decoder_latest_timed(ph_decoder *d, struct AVFrame *dst, uint64_t *seq,
                             ph_frame_timing *timing)
{
    memset(timing, 0, sizeof *timing);
    return ph_decoder_latest(d, dst, seq);
}
void ph_decoder_get_stats(ph_decoder *d, ph_decoder_stats *stats)
{
    (void)d;
    memset(stats, 0, sizeof *stats);
}
void ph_decoder_stop(ph_decoder *d) { (void)d; decoders_stopped++; }

ph_receiver *ph_receiver_start(const ph_receiver_options *opt, const ph_receiver_callbacks *cb)
{
    (void)opt;
    receivers_started++;
    cb->connected(cb->user, "Test Phone", "test");
    return &receiver;
}
void ph_receiver_disconnect(ph_receiver *r)
{
    (void)r;
}
void ph_receiver_poll(ph_receiver *r) { (void)r; }
void ph_receiver_stop(ph_receiver *r) { (void)r; receivers_stopped++; }

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--ack-timeout")
                    && strcmp(argv[1], "--healthy-no-video")))
        return 1;
    ack_timeout_mode = argc == 2;
    healthy_no_video = argc == 2 && !strcmp(argv[1], "--healthy-no-video");
    char config[] = "/tmp/porthole-reconnect-config-XXXXXX";
    if (!mkdtemp(config)) return 1;
    setenv("XDG_CONFIG_HOME", config, 1);
    char counter[] = "/tmp/porthole-reconnect-XXXXXX";
    int fd = mkstemp(counter);
    if (fd < 0)
        return 1;
    close(fd);
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("PORTHOLE_TEST_BLE_COUNTER", counter, 1);
    ph_app_options options = {.name = "Retry test", .bluetooth = true,
                              .ble_helper = PORTHOLE_TEST_BLE_HELPER};
    char ack_helper[] = "/tmp/porthole-no-ack-XXXXXX";
    if (ack_timeout_mode) {
        fd = mkstemp(ack_helper);
        FILE *script = fd < 0 ? NULL : fdopen(fd, "w");
        if (!script) {
            if (fd >= 0)
                close(fd);
            unlink(ack_helper);
            unlink(counter);
            return 1;
        }
        /* Remains alive and consumes every report, but never acknowledges any
         * fence. Only the app's timeout recovery will close its stdin. */
        fputs("import os, sys, time\nfrom pathlib import Path\n"
              "p = Path(os.environ['PORTHOLE_TEST_BLE_COUNTER'])\n"
              "p.write_text(str(int(p.read_text() or '0') + 1))\n"
              "print('ready\\nsubscribed', flush=True)\n", script);
        if (healthy_no_video)
            fputs("while True:\n"
                  " h = sys.stdin.buffer.read(2)\n"
                  " if not h: break\n"
                  " if len(h) != 2: raise SystemExit(4)\n"
                  " data = sys.stdin.buffer.read(h[1])\n"
                  " if len(data) != h[1]: raise SystemExit(5)\n"
                  " if h[0] == 18: time.sleep(int.from_bytes(data, 'little') / 1000)\n"
                  " else: time.sleep(0.017)\n"
                  " if h[0] == 16:\n"
                  "  sync = int.from_bytes(data, 'little')\n"
                  "  print('sync', sync, flush=True)\n"
                  "  if sync > 2147483649: print('sync', sync - 1, flush=True)\n", script);
        else
            fputs("sys.stdin.buffer.read()\n", script);
        if (fclose(script)) {
            unlink(ack_helper);
            unlink(counter);
            return 1;
        }
        options.ble_helper = ack_helper;
    }
    int result = ph_app_run(&options);
    if (ack_timeout_mode)
        unlink(ack_helper);
    int attempts = 0;
    FILE *file = fopen(counter, "r");
    if (file) {
        if (fscanf(file, "%d", &attempts) != 1)
            attempts = 0;
        fclose(file);
    }
    unlink(counter);
    char config_dir[sizeof config + 16];
    snprintf(config_dir, sizeof config_dir, "%s/porthole", config);
    rmdir(config_dir);
    rmdir(config);
    int expected_attempts = healthy_no_video ? 1 : 2;
    if (result || timed_out || attempts != expected_attempts || ready_count != expected_attempts || windows_created != 1
        || decoders_started != 1 || receivers_started != 1 || decoders_stopped != 1
        || receivers_stopped != 1
        || (ack_timeout_mode && !motion_injected)
        || (ack_timeout_mode && !healthy_no_video && recovered_at - motion_at < 7500)) {
        fprintf(stderr, "reconnect failed: rc=%d timeout=%d helpers=%d ready=%d windows=%d "
                        "decoders=%d/%d receivers=%d/%d\n", result, timed_out, attempts,
                ready_count, windows_created, decoders_started, decoders_stopped,
                receivers_started, receivers_stopped);
        if (ack_timeout_mode)
            fprintf(stderr, "ACK timeout mode: injected=%d recovery_elapsed=%llu ms\n",
                    motion_injected, (unsigned long long)(recovered_at - motion_at));
        return 1;
    }
    puts(healthy_no_video ? "Real paced helper remained connected despite missing video and stale duplicate ACKs"
         : ack_timeout_mode ? "Missing helper ACK triggered control recovery without restarting mirroring"
                          : "Bluetooth recovered without restarting mirroring");
    return 0;
}
