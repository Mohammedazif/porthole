#ifndef PORTHOLE_UI_CLIPBOARD_PREVIEW_H
#define PORTHOLE_UI_CLIPBOARD_PREVIEW_H

#include <SDL.h>
#include <stdbool.h>

bool ph_clipboard_png_size(const void *data, size_t len, uint32_t *width, uint32_t *height);
SDL_Texture *ph_clipboard_thumbnail(SDL_Renderer *renderer, const void *data, size_t len);

#endif
