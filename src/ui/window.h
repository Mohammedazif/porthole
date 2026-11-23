#ifndef PORTHOLE_UI_WINDOW_H
#define PORTHOLE_UI_WINDOW_H

/* The mirror window (SDL2): shows the phone-screen part of decoded frames letterboxed and
 * maps window points to positions on the phone screen. Main thread only. */

#include <stdbool.h>

#include <SDL.h>

#include "video/crop.h"
#include "ui/workspace.h"
#include "control/setup.h"
#include "core/metrics.h"

/* Client pixels reserved above the picture for the branded title bar. */
#define PH_WINDOW_BAR_PX 32

struct AVFrame;

typedef struct ph_window ph_window;

ph_window *ph_window_create(const char *title);

/* Creation stays hidden until the connect screen has its address and name. */
void ph_window_show(ph_window *w);
void ph_window_refresh_folder(ph_window *w);
ph_workspace *ph_window_workspace(ph_window *w);
void ph_window_set_companion(ph_window *w, bool connected, bool recording, int volume);
void ph_window_set_incoming(ph_window *window, const char *name, uint64_t bytes, uint64_t total);
void ph_window_set_receiving(ph_window *w, int done, int total);
ph_workspace_action ph_window_take_workspace_action(ph_window *w, uint64_t *item);

void ph_window_set_title(ph_window *w, const char *title);

/* Local connection/setup guidance; this never sends input to the phone. */
void ph_window_set_status(ph_window *w, bool mirroring, bool control_ready, const char *detail);

/* Replaces the status line without changing whether the mirror or the controls are up. */
void ph_window_set_detail(ph_window *w, const char *detail);

/* Shown in help so the iPhone app can connect. Empty hides the line. */
void ph_window_set_pull_hint(ph_window *w, const char *hint);

/* True once, after New code is clicked on the connect screen. */
bool ph_window_take_new_code(ph_window *w);

/* One click from Settings. NONE when nothing is waiting. */
typedef enum ph_window_action {
    PH_WINDOW_ACTION_NONE = 0,
    PH_WINDOW_ACTION_FEED,
    PH_WINDOW_ACTION_FORGET,
    PH_WINDOW_ACTION_PAUSE,
    PH_WINDOW_ACTION_MUTE,
    PH_WINDOW_ACTION_QUIT,
    PH_WINDOW_ACTION_SAVE,
    PH_WINDOW_ACTION_PICK_FOLDER,
    PH_WINDOW_ACTION_SAVE_DOWNLOADS
} ph_window_action;

ph_window_action ph_window_take_action(ph_window *w);
bool ph_window_ctrl_is_command(const ph_window *w);
bool ph_window_release_on_blur(const ph_window *w);
bool ph_window_next_relative(const ph_window *w);
void ph_window_set_prefs(ph_window *w, bool ctrl_command, bool release_blur, bool next_relative);

/* A line in the Transfers panel. active is the one in progress; NULL clears it.
 * set_active returns true when the line changed. show_transfers opens that panel. */
void ph_window_note_transfer(ph_window *w, const char *line);
/* After the user resized the phone window, fit the other side to the phone's shape. */
void ph_window_keep_aspect(ph_window *w);

/* Feed scrolling, paused controls and PC sound, shown in Settings. */
void ph_window_set_live_state(ph_window *w, bool feed, bool paused, bool sound_on);
bool ph_window_media_keys(const ph_window *w);
void ph_window_set_media_keys(ph_window *w, bool on);
/* Settings rows, top to bottom: pointer, wheel, pause, guide, Ctrl, media keys,
 * release keys, sound, save folder, copy pairing, forget, quit. side picks a
 * two-way choice's half. Row -1 is Done. Centre of that control, for tests. */
bool ph_window_settings_target(ph_window *w, int row, int side, int *x, int *y);

/* The name the iPhone lists this PC under. */
void ph_window_set_pc_name(ph_window *w, const char *name);
/* The connected phone's own name. Empty shows "Connected". */
void ph_window_set_phone_name(ph_window *w, const char *name);
/* False after the pairing code has been used once. The digits are hidden. */
void ph_window_set_code_open(ph_window *w, bool open);

typedef enum ph_window_target {
    PH_TARGET_COPY, PH_TARGET_NEW_CODE, PH_TARGET_SETUP, PH_TARGET_HELP, PH_TARGET_STATS,
    PH_TARGET_SETTINGS
} ph_window_target;
/* Centre of a connect-screen control, for tests. False while the picture shows. */
bool ph_window_connect_target(ph_window *w, ph_window_target target, int *x, int *y);

/* Puts the pairing line on the clipboard; whether there is one to copy. */
void ph_window_copy_pairing(ph_window *w);
bool ph_window_has_pairing(const ph_window *w);
void ph_window_open_settings(ph_window *w);

/* 0 to 100 of the file moving now; -1 when unknown. */
void ph_window_set_transfer_percent(ph_window *w, int percent);
bool ph_window_set_active_transfer(ph_window *w, const char *line);
void ph_window_show_transfers(ph_window *w);
bool ph_window_take_cancel(ph_window *w);

/* Whether both the companion link and the screen-sharing listener started. */
void ph_window_set_share_available(ph_window *w, bool available);
void ph_window_set_clipboard_available(ph_window *w, bool available);
void ph_window_note_received(ph_window *w, unsigned files);
void ph_window_open_received_history(ph_window *w);
void ph_window_open_pairing(ph_window *w);
void ph_window_set_history_profile(ph_window *w, const char *profile);

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

/* Consumes local toolbar/button interactions and modal scrolling. The phone never
 * sees these events. Button actions queue the same shortcuts as F1/F2/F3. */
bool ph_window_chrome_event(ph_window *w, const SDL_Event *event);

/* Size of the phone screen in stream pixels (0x0 if none yet). */
void ph_window_video_size(const ph_window *w, int *width, int *height);

/* SDL window ID, for matching input events to this window. */
unsigned int ph_window_id(const ph_window *w);

/* The window's built-in text, for a small overlay that is not the mirror. */
int ph_ui_text_height(const char *text, int width, int scale);
int ph_ui_draw_text(SDL_Renderer *renderer, const char *text, int x, int y, int width, int scale,
                    SDL_Color color);

void ph_window_destroy(ph_window *w);

#endif
