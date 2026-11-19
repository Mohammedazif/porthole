#ifndef PORTHOLE_FILES_XDND_H
#define PORTHOLE_FILES_XDND_H

#include <stdbool.h>
#include <stddef.h>

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

/* Title of the other window under the cursor. False while the pointer is still
 * over this app, or when no window there accepts a dropped file. */
bool ph_xdnd_place(SDL_Window *window, int root_x, int root_y, char *buf, size_t n);
/* A window that follows the cursor and must not hide the drop target. */
void ph_xdnd_skip(SDL_Window *window);

#endif
