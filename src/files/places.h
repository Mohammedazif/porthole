#ifndef PORTHOLE_FILES_PLACES_H
#define PORTHOLE_FILES_PLACES_H

/* Where files go on this PC, and how the desktop hands files to Porthole. */

#include <stdbool.h>
#include <stddef.h>

typedef enum ph_place {
    PH_PLACE_DOWNLOADS,
    PH_PLACE_DESKTOP
} ph_place;

/* The user's Downloads or Desktop folder, created when missing. UTF-8, no trailing
 * separator. Windows asks the shell, so a moved Downloads folder is honoured. */
bool ph_place_dir(ph_place place, char *out, size_t cap);

/* Where files shared from the iPhone are saved. Downloads until the user picks
 * a folder. The choice is remembered. */
bool ph_place_save_dir(char *out, size_t cap);
/* "Downloads", "Desktop", or the chosen folder's own name. */
bool ph_place_save_label(char *out, size_t cap);
/* True when the saved place is not Downloads. */
bool ph_place_save_is_custom(void);
/* NULL restores Downloads. An absolute folder is remembered; Downloads and Desktop
 * are remembered as those places, so a moved system folder still follows. */
bool ph_place_set_save_dir(const char *dir);
/* Modal folder picker. False if the user cancels or this system has no picker. */
bool ph_place_pick_folder(char *out, size_t cap);

/* One regular file the user copied (Ctrl+C in Explorer or a file manager). UTF-8. */
bool ph_place_clipboard_file(char *out, size_t cap);

/* Adds "iPhone (Porthole)" to Explorer's Send to menu, pointing at this exe with
 * --send. Windows only; does nothing elsewhere. Safe to call on every start. */
void ph_place_install_send_to(void);

/* Hands each path to the running Porthole over 127.0.0.1. Returns how many it took. */
int ph_place_send_to_running(int count, char **paths);

/* porthole --send FILE...: hands the files over and says so when it could not.
 * Returns the process exit code. */
int ph_place_send_command(int count, char **paths);

#endif
