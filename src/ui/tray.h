#ifndef PORTHOLE_UI_TRAY_H
#define PORTHOLE_UI_TRAY_H

/* Tray action state and an XDG autostart desktop file. No StatusNotifier and no icon. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum ph_tray_action {
    PH_TRAY_SHOW = 0,
    PH_TRAY_TOGGLE_MUTE,
    PH_TRAY_TOGGLE_RECORD,
    PH_TRAY_QUIT
} ph_tray_action;

typedef struct ph_tray ph_tray;

ph_tray *ph_tray_create(void);
void ph_tray_destroy(ph_tray *tray);
void ph_tray_invoke(ph_tray *tray, ph_tray_action action);

bool ph_tray_muted(const ph_tray *tray);
bool ph_tray_recording(const ph_tray *tray);
bool ph_tray_quit_requested(const ph_tray *tray);
bool ph_tray_show_requested(const ph_tray *tray);
void ph_tray_clear_show(ph_tray *tray);

/* Hidden is the inverse of enable. Rejects a NULL exe path. */
bool ph_autostart_desktop(char *out, size_t out_len, const char *exe_path, bool enable);

#ifdef _WIN32
/* NOTIFYICONDATA.uCallbackMessage is PH_TRAY_WM_CALLBACK (WM_APP + 1). */
#define PH_TRAY_WM_CALLBACK 0x8001u
#define PH_TRAY_WM_COMMAND 0x0111u
#define PH_TRAY_WM_LBUTTONUP 0x0202u
#define PH_TRAY_WM_LBUTTONDBLCLK 0x0203u
#define PH_TRAY_CMD_SHOW 1u
#define PH_TRAY_CMD_TOGGLE_MUTE 2u
#define PH_TRAY_CMD_TOGGLE_RECORD 3u
#define PH_TRAY_CMD_QUIT 4u

bool ph_tray_win32_message(ph_tray *tray, unsigned message, uintptr_t wparam, intptr_t lparam);
#endif

#endif
