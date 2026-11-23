#ifndef PORTHOLE_POLISH_RUNTIME_H
#define PORTHOLE_POLISH_RUNTIME_H

/* Everyday-use controls: window chrome, paste, allow-list, recording, optional touch.
 * Pointer recovery, latency diagnostics, and Wi-Fi discovery stay elsewhere. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <SDL.h>

#include "control/input.h"
#include "session/devices.h"
#include "usb/switch.h"

struct AVFrame;
struct ph_audio;

typedef struct ph_polish ph_polish;

typedef struct ph_polish_options {
    bool pin_required;
    bool autostart;
    const char *precise_touch; /* host:port, NULL when off */
    const char *exe_path;
} ph_polish_options;

ph_polish *ph_polish_create(const ph_polish_options *opt);
void ph_polish_destroy(ph_polish *polish);

void ph_polish_set_audio(ph_polish *polish, struct ph_audio *audio);
void ph_polish_apply_saved(ph_polish *polish, SDL_Window *window);
void ph_polish_save_window(ph_polish *polish, SDL_Window *window);
void ph_polish_layout(ph_polish *polish, SDL_Window *window, int video_w, int video_h);

/* Network thread. True admits the device. */
bool ph_polish_admit(ph_polish *polish, const char *device_id, const char *name,
                     const char *model);
void ph_polish_disconnected(ph_polish *polish);
bool ph_polish_accepts_video(ph_polish *polish);
/* USB screen uses the device id "usb". Present selects it; absent drops it. */
void ph_polish_usb_present(ph_polish *polish, bool present);
bool ph_polish_accepts_usb(ph_polish *polish);
void ph_polish_sessions(ph_polish *polish, ph_session_state *out);
bool ph_polish_select(ph_polish *polish, const char *id);
bool ph_polish_rename(ph_polish *polish, const char *id, const char *name);
bool ph_polish_forget(ph_polish *polish, const char *id);
bool ph_polish_resolve_request(ph_polish *polish, const char *id, const char *pin);
bool ph_polish_require_pin(ph_polish *polish, bool required);
bool ph_polish_copy_pin(ph_polish *polish, char *out, size_t out_len);

void ph_polish_on_video(ph_polish *polish, ph_src source, const uint8_t *annexb, size_t len,
                        uint64_t pts_ns);

/* True when the key was local and must not reach the phone. */
bool ph_polish_key(ph_polish *polish, const SDL_KeyboardEvent *event, ph_input *input,
                   SDL_Window *window, int video_w, int video_h, const struct AVFrame *frame,
                   bool hotkeys);

void ph_polish_tick(ph_polish *polish, ph_input *input);

/* Types the PC clipboard into the focused field when it is plain US-QWERTY. */
bool ph_polish_type_clipboard(ph_polish *polish);
bool ph_polish_capture_folder(char *path, size_t capacity);
const char *ph_polish_capture_notice(ph_polish *polish);
void ph_polish_stop_recording(ph_polish *polish);
bool ph_polish_recording(ph_polish *polish);
bool ph_polish_quit(const ph_polish *polish);
bool ph_polish_precise(const ph_polish *polish);

/* True when precise touch handled the button and the Bluetooth pointer should stay put. */
bool ph_polish_click(ph_polish *polish, bool down, double nx, double ny, int video_w,
                     int video_h);

#endif
