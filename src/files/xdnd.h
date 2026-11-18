#ifndef PORTHOLE_FILES_XDND_H
#define PORTHOLE_FILES_XDND_H

#include <stdbool.h>

#include <SDL.h>

/* Offers one local file to the window under the cursor. The path can be replaced
 * until the button goes up, which is when the other window reads it. */
bool ph_xdnd_begin(SDL_Window *window, const char *path);
void ph_xdnd_set_path(const char *path);
void ph_xdnd_motion(int root_x, int root_y);
/* True when another window accepted the file and will read it. */
bool ph_xdnd_drop(void);
void ph_xdnd_cancel(void);
bool ph_xdnd_busy(void);
void ph_xdnd_syswm(const void *msg);

#endif
