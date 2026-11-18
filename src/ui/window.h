#ifndef PORTHOLE_UI_WINDOW_H
#define PORTHOLE_UI_WINDOW_H

/* The mirror window (SDL2): shows the phone-screen part of decoded frames letterboxed and
 * maps window points to positions on the phone screen. Main thread only. */

#include <stdbool.h>

#include <SDL.h>

#include "video/crop.h"
#include "control/setup.h"
#include "core/metrics.h"

/* Client pixels reserved above the picture for the branded title bar. */
#define PH_WINDOW_BAR_PX 80

struct AVFrame;

typedef struct ph_window ph_window;

ph_window *ph_window_create(const char *title);

void ph_window_set_title(ph_window *w, const char *title);

/* Local connection/setup guidance; this never sends input to the phone. */
void ph_window_set_status(ph_window *w, bool mirroring, bool control_ready, const char *detail);

/* Replaces the status line without changing whether the mirror or the controls are up. */
void ph_window_set_detail(ph_window *w, const char *detail);

/* Shown in help so the iPhone app can connect. Empty hides the line. */
void ph_window_set_pull_hint(ph_window *w, const char *hint);

/* F1 opens/closes local help. Picture coordinates are rejected while it is open. */
void ph_window_toggle_help(ph_window *w);
bool ph_window_help_visible(const ph_window *w);

/* F2 guided setup is local and consumes input while visible. */
void ph_window_set_setup(ph_window *w, const ph_setup *setup);
ph_setup_action ph_window_setup_click(const ph_window *w, int x, int y);
/* F3 diagnostics never intercepts phone input. All timings are local measurements. */
void ph_window_toggle_diagnostics(ph_window *w);
void ph_window_set_metrics(ph_window *w, const ph_metrics_snapshot *metrics,
                           unsigned int queued_packets, uint64_t discarded_packets,
                           uint64_t superseded_frames);

/* Uploads a YUV 4:2:0 frame and shows the `screen` part of it (the phone screen inside the
 * frame, see video/crop.h). The window is resized to fit when the screen size changes and
 * stays changed for a moment (so rotation animations don't make it jump). */
void ph_window_present(ph_window *w, const struct AVFrame *frame, const ph_rect *screen);

/* Applies pending resizes; call regularly from the event loop. */
void ph_window_tick(ph_window *w);

/* Redraws the last frame (after resize/expose) or a blank window if there is none. */
void ph_window_redraw(ph_window *w);

/* Forgets the last frame and shows a blank window (e.g. when mirroring stops). */
void ph_window_clear(ph_window *w);

/* Maps a point in window coordinates to the phone screen as fractions in [0,1].
 * Returns false on the title bar, in letterbox bars, while local help is open, or when
 * controls are unavailable. */
bool ph_window_to_video(const ph_window *w, int x, int y, double *vx, double *vy);

/* Same mapping without needing Bluetooth control: for reading the picture, such as
 * dragging a photo out, never for sending input to the phone. */
bool ph_window_picture_point(const ph_window *w, int x, int y, double *vx, double *vy);

/* True when the event is on the title bar. The phone does not see those events. */
bool ph_window_chrome_event(ph_window *w, const SDL_Event *event);

/* Size of the phone screen in stream pixels (0x0 if none yet). */
void ph_window_video_size(const ph_window *w, int *width, int *height);

/* SDL window ID, for matching input events to this window. */
unsigned int ph_window_id(const ph_window *w);

void ph_window_destroy(ph_window *w);

#endif
