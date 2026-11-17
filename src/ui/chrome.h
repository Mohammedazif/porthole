#ifndef PORTHOLE_UI_CHROME_H
#define PORTHOLE_UI_CHROME_H

/* Applies a placement to an SDL window. Compiled only into the SDL app. */

#include "ui/placement.h"

typedef struct SDL_Window SDL_Window;

/* Fullscreen wins over one_to_one for the SDL size. The flag stays in the placement. */
void ph_chrome_apply(SDL_Window *window, const ph_placement *placement);

#endif
