#ifndef PH_UI_DISPLAY_SCALE_H
#define PH_UI_DISPLAY_SCALE_H

#include <SDL.h>
#include <math.h>

/* Additional content scaling for backends where SDL reports unscaled window units. */
float ph_display_content_scale(SDL_Window *window);

static inline int ph_display_units(int points, float scale)
{
    return (int)lroundf(points * scale);
}

static inline int ph_display_point(int units, float scale)
{
    return (int)floorf(units / scale);
}

#endif
