/* Run the real app/helper lifecycle with headless video and no Bluetooth hardware. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL.h>

#include "app.h"
#include "airplay/receiver.h"
#include "ui/window.h"
#include "video/decoder.h"

struct ph_window { int unused; };
struct ph_decoder { int unused; };
struct ph_receiver { int unused; };
static ph_window window;
static ph_decoder decoder;
static ph_receiver receiver;
static int windows_created, decoders_started, receivers_started;
static int decoders_stopped, receivers_stopped, ready_count;
static bool timed_out;
static Uint64 first_tick;

static void quit(void)
{
    SDL_Event event = {.type = SDL_QUIT};
    SDL_PushEvent(&event);
}

ph_window *ph_window_create(const char *title)
{
    (void)title;
    windows_created++;
    return &window;
}

void ph_window_set_title(ph_window *w, const char *title)
{
    (void)w;
    if (strstr(title, "[Bluetooth control: on]") && ++ready_count == 2)
        quit();
}
void ph_window_set_status(ph_window *w, bool mirroring, bool ready, const char *detail)
{
    (void)w, (void)mirroring, (void)ready, (void)detail;
}
void ph_window_toggle_help(ph_window *w) { (void)w; }
bool ph_window_help_visible(const ph_window *w) { (void)w; return false; }

void ph_window_tick(ph_window *w)
{
    (void)w;
    if (!first_tick)
        first_tick = SDL_GetTicks64();
    if (SDL_GetTicks64() - first_tick > 10000) {
        timed_out = true;
        quit();
    }
}

void ph_window_present(ph_window *w, const struct AVFrame *frame, const ph_rect *screen)
{
    (void)w, (void)frame, (void)screen;
}
void ph_window_redraw(ph_window *w) { (void)w; }
void ph_window_clear(ph_window *w) { (void)w; }
unsigned int ph_window_id(const ph_window *w) { (void)w; return 1; }
void ph_window_destroy(ph_window *w) { (void)w; }
bool ph_window_to_video(const ph_window *w, int x, int y, double *vx, double *vy)
{
    (void)w, (void)x, (void)y, (void)vx, (void)vy;
    return false;
}
void ph_window_video_size(const ph_window *w, int *width, int *height)
{
    (void)w;
    *width = *height = 0;
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
void ph_decoder_reset(ph_decoder *d) { (void)d; }
bool ph_decoder_latest(ph_decoder *d, struct AVFrame *dst, uint64_t *seq)
{
    (void)d, (void)dst, (void)seq;
    return false;
}
void ph_decoder_stop(ph_decoder *d) { (void)d; decoders_stopped++; }

ph_receiver *ph_receiver_start(const ph_receiver_options *opt, const ph_receiver_callbacks *cb)
{
    (void)opt;
    receivers_started++;
    cb->connected(cb->user, "Test Phone", "test");
    return &receiver;
}
void ph_receiver_poll(ph_receiver *r) { (void)r; }
void ph_receiver_stop(ph_receiver *r) { (void)r; receivers_stopped++; }

int main(void)
{
    char counter[] = "/tmp/porthole-reconnect-XXXXXX";
    int fd = mkstemp(counter);
    if (fd < 0)
        return 1;
    close(fd);
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("PORTHOLE_TEST_BLE_COUNTER", counter, 1);
    ph_app_options options = {.name = "Retry test", .bluetooth = true,
                              .ble_helper = PORTHOLE_TEST_BLE_HELPER};
    int result = ph_app_run(&options);
    int attempts = 0;
    FILE *file = fopen(counter, "r");
    if (file) {
        if (fscanf(file, "%d", &attempts) != 1)
            attempts = 0;
        fclose(file);
    }
    unlink(counter);
    if (result || timed_out || attempts != 2 || ready_count != 2 || windows_created != 1
        || decoders_started != 1 || receivers_started != 1 || decoders_stopped != 1
        || receivers_stopped != 1) {
        fprintf(stderr, "reconnect failed: rc=%d timeout=%d helpers=%d ready=%d windows=%d "
                        "decoders=%d/%d receivers=%d/%d\n", result, timed_out, attempts,
                ready_count, windows_created, decoders_started, decoders_stopped,
                receivers_started, receivers_stopped);
        return 1;
    }
    puts("Bluetooth recovered without restarting mirroring");
    return 0;
}
