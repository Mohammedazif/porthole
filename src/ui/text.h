#ifndef PORTHOLE_UI_TEXT_H
#define PORTHOLE_UI_TEXT_H

#include <stdbool.h>
#include <SDL.h>

/* Each renderer caches glyphs rasterized at the actual pixel size. The typeface is
 * embedded; no font installation is needed. Metrics use logical window points. */
bool ph_text_attach(SDL_Renderer *renderer);
void ph_text_detach(SDL_Renderer *renderer);
int ph_text_width(const char *text, int scale);
int ph_text_line_height(int scale);
int ph_text_height(const char *text, int width, int scale);
int ph_text_draw(SDL_Renderer *renderer, const char *text, int x, int y, int width, int scale,
                 SDL_Color color);

#endif
