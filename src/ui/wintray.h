#ifndef PORTHOLE_UI_WINTRAY_H
#define PORTHOLE_UI_WINTRAY_H

/* The Porthole icon in the Windows notification area and its menu. Elsewhere the
 * calls do nothing and create returns NULL. */

#include <stdbool.h>

typedef enum ph_wintray_command {
    PH_WINTRAY_NONE = 0,
    PH_WINTRAY_SHOW,
    PH_WINTRAY_FEED,
    PH_WINTRAY_PAUSE,
    PH_WINTRAY_SOUND,
    PH_WINTRAY_COPY,
    PH_WINTRAY_SETTINGS,
    PH_WINTRAY_RECEIVED,
    PH_WINTRAY_SHOW_RECEIVED_FILE,
    PH_WINTRAY_QUIT
} ph_wintray_command;

typedef struct ph_wintray ph_wintray;

ph_wintray *ph_wintray_create(void);
void ph_wintray_destroy(ph_wintray *tray);

/* What the menu ticks, and the icon's hover text. */
void ph_wintray_set_state(ph_wintray *tray, bool feed, bool paused, bool sound_on,
                          bool can_copy, const char *tip);

/* A Windows notification from the tray icon.
 * False when there is no tray icon. */
bool ph_wintray_notify(ph_wintray *tray, const char *title, const char *text);

/* The command picked since the last call. Messages arrive while SDL pumps events. */
ph_wintray_command ph_wintray_take(ph_wintray *tray);

#endif
