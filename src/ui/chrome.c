#include "ui/chrome.h"

#include <SDL.h>

void ph_chrome_apply(SDL_Window *window, const ph_placement *placement)
{
    if (!window || !placement || !placement->valid)
        return;
    /* Fullscreen owns the SDL size; one_to_one remains in the placement for later. */
    if (placement->fullscreen) {
        (void)SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        (void)SDL_SetWindowAlwaysOnTop(window, placement->always_on_top ? SDL_TRUE : SDL_FALSE);
        return;
    }
    (void)SDL_SetWindowFullscreen(window, 0);
    if (placement->w > 0 && placement->h > 0)
        (void)SDL_SetWindowSize(window, placement->w, placement->h);
    (void)SDL_SetWindowPosition(window, placement->x, placement->y);
    (void)SDL_SetWindowAlwaysOnTop(window, placement->always_on_top ? SDL_TRUE : SDL_FALSE);
}
