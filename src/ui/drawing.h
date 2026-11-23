#ifndef PORTHOLE_UI_DRAWING_H
#define PORTHOLE_UI_DRAWING_H

#include <SDL.h>

SDL_FRect ph_ui_pixel_rect(int x, int y, int w, int h, float sx, float sy);
void ph_ui_round_rect(SDL_Renderer *renderer, SDL_Rect box, int radius, SDL_Color color);
void ph_ui_release_drawing(SDL_Renderer *renderer);

#endif
