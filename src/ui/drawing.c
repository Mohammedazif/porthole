#include "ui/drawing.h"

#include <math.h>

typedef struct {
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int rx, ry;
} rounded_mask;

static rounded_mask masks[32];
static unsigned next_mask;

static SDL_Texture *corner_mask(SDL_Renderer *renderer, int rx, int ry)
{
    for (size_t i = 0; i < SDL_arraysize(masks); i++)
        if (masks[i].renderer == renderer && masks[i].rx == rx && masks[i].ry == ry)
            return masks[i].texture;

    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, 2 * rx, 2 * ry, 32,
                                                          SDL_PIXELFORMAT_RGBA32);
    if (!surface)
        return NULL;
    for (int y = 0; y < ry; y++)
        for (int x = 0; x < rx; x++) {
            int covered = 0;
            /* Rasterize a quarter ellipse in output pixels, with 16 samples per pixel. */
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float dx = (rx - x - (sx + .5f) / 4) / rx;
                    float dy = (ry - y - (sy + .5f) / 4) / ry;
                    covered += dx * dx + dy * dy <= 1;
                }
            Uint8 alpha = (Uint8)((covered * 255 + 8) / 16);
            for (int bottom = 0; bottom < 2; bottom++)
                for (int right = 0; right < 2; right++) {
                    int px = right ? 2 * rx - 1 - x : x;
                    int py = bottom ? 2 * ry - 1 - y : y;
                    Uint8 *pixel = (Uint8 *)surface->pixels + py * surface->pitch + 4 * px;
                    pixel[0] = pixel[1] = pixel[2] = 255;
                    pixel[3] = alpha;
                }
        }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    if (!texture)
        return NULL;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest);
    rounded_mask *slot = &masks[next_mask++ % SDL_arraysize(masks)];
    if (slot->texture)
        SDL_DestroyTexture(slot->texture);
    *slot = (rounded_mask){renderer, texture, rx, ry};
    return texture;
}

SDL_FRect ph_ui_pixel_rect(int x, int y, int w, int h, float sx, float sy)
{
    /* Keep float roundoff from shifting a snapped edge back one pixel in SDL's software renderer. */
    return (SDL_FRect){(x + .001f) / sx, (y + .001f) / sy, (w + .001f) / sx, (h + .001f) / sy};
}

void ph_ui_round_rect(SDL_Renderer *renderer, SDL_Rect box, int radius, SDL_Color color)
{
    if (box.w < 1 || box.h < 1)
        return;
    float sx, sy;
    SDL_RenderGetScale(renderer, &sx, &sy);
    int x = (int)lroundf(box.x * sx), y = (int)lroundf(box.y * sy);
    int w = (int)lroundf((box.x + box.w) * sx) - x;
    int h = (int)lroundf((box.y + box.h) * sy) - y;
    int rx = SDL_min((int)lroundf(radius * sx), w / 2);
    int ry = SDL_min((int)lroundf(radius * sy), h / 2);
    SDL_Texture *mask = rx > 0 && ry > 0 ? corner_mask(renderer, rx, ry) : NULL;
    SDL_BlendMode previous;
    SDL_GetRenderDrawBlendMode(renderer, &previous);
    SDL_SetRenderDrawBlendMode(renderer, color.a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    if (mask) {
        if (w > 2 * rx) {
            SDL_FRect center = ph_ui_pixel_rect(x + rx, y, w - 2 * rx, h, sx, sy);
            SDL_RenderFillRectF(renderer, &center);
        }
        if (h > 2 * ry) {
            SDL_FRect sides[] = {ph_ui_pixel_rect(x, y + ry, rx, h - 2 * ry, sx, sy),
                                 ph_ui_pixel_rect(x + w - rx, y + ry, rx, h - 2 * ry, sx, sy)};
            SDL_RenderFillRectsF(renderer, sides, 2);
        }
        SDL_SetTextureColorMod(mask, color.r, color.g, color.b);
        SDL_SetTextureAlphaMod(mask, color.a);
        for (int bottom = 0; bottom < 2; bottom++)
            for (int right = 0; right < 2; right++) {
                SDL_Rect source = {right * rx, bottom * ry, rx, ry};
                SDL_FRect dest = ph_ui_pixel_rect(x + right * (w - rx), y + bottom * (h - ry),
                                                  rx, ry, sx, sy);
                SDL_RenderCopyF(renderer, mask, &source, &dest);
            }
    } else if (w > 0 && h > 0) {
        SDL_FRect fill = ph_ui_pixel_rect(x, y, w, h, sx, sy);
        SDL_RenderFillRectF(renderer, &fill);
    }
    SDL_SetRenderDrawBlendMode(renderer, previous);
}

void ph_ui_release_drawing(SDL_Renderer *renderer)
{
    for (size_t i = 0; i < SDL_arraysize(masks); i++)
        if (masks[i].renderer == renderer) {
            SDL_DestroyTexture(masks[i].texture);
            masks[i] = (rounded_mask){0};
        }
}
