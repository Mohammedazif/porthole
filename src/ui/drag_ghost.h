#ifndef PORTHOLE_UI_DRAG_GHOST_H
#define PORTHOLE_UI_DRAG_GHOST_H

/* A small copy of one photo that follows the cursor while it is dragged out. */

#include <stdbool.h>

#include <SDL.h>

#include "video/crop.h"

struct AVFrame;

typedef struct ph_drag_ghost ph_drag_ghost;

ph_drag_ghost *ph_drag_ghost_create(void);
void ph_drag_ghost_destroy(ph_drag_ghost *g);

bool ph_drag_ghost_show(ph_drag_ghost *g, const struct AVFrame *frame, ph_rect rect);
void ph_drag_ghost_caption(ph_drag_ghost *g, const char *text, bool aimed);
void ph_drag_ghost_move(ph_drag_ghost *g, int root_x, int root_y);
void ph_drag_ghost_hide(ph_drag_ghost *g);
SDL_Window *ph_drag_ghost_window(const ph_drag_ghost *g);

#endif
