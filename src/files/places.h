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
