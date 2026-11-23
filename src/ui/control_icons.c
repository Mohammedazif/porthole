#include "ui/control_icons.h"
#include <math.h>
#include <stdlib.h>

typedef struct {
    short x, y;
} glyph_point;
#include "ui/generated/control_paths.h"

typedef struct glyph_cache {
    struct glyph_cache *next;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    ph_control_icon kind;
    int size;
} glyph_cache;
static glyph_cache *cache;

static SDL_Texture *render_icon(SDL_Renderer *renderer, ph_control_icon kind, int size)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s)
        return NULL;
    SDL_FillRect(s, NULL, 0);
    const glyph_point *points = shapes[kind].p;
    float scale = size / 24.f, radius = .825f * scale;
    for (size_t i = 1; i < shapes[kind].n; i++) {
        if (points[i].x < 0 || points[i - 1].x < 0)
            continue;
        float ax = points[i - 1].x * scale / 16, ay = points[i - 1].y * scale / 16;
        float bx = points[i].x * scale / 16, by = points[i].y * scale / 16, dx = bx - ax,
              dy = by - ay, len = dx * dx + dy * dy;
        int top = SDL_max(0, (int)floorf(fminf(ay, by) - radius - 1)),
            bottom = SDL_min(size, (int)ceilf(fmaxf(ay, by) + radius + 1));
        int left = SDL_max(0, (int)floorf(fminf(ax, bx) - radius - 1)),
            right = SDL_min(size, (int)ceilf(fmaxf(ax, bx) + radius + 1));
        for (int y = top; y < bottom; y++)
            for (int x = left; x < right; x++) {
                float t =
                    len ? SDL_clamp(((x + .5f - ax) * dx + (y + .5f - ay) * dy) / len, 0.f, 1.f)
                        : 0;
                float d = hypotf(x + .5f - ax - t * dx, y + .5f - ay - t * dy);
                Uint8 alpha = (Uint8)(255 * SDL_clamp(radius + .5f - d, 0.f, 1.f));
                Uint8 *pixel = (Uint8 *)s->pixels + y * s->pitch + 4 * x;
                pixel[0] = pixel[1] = pixel[2] = 255;
                pixel[3] = SDL_max(alpha, pixel[3]);
            }
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, s);
    SDL_FreeSurface(s);
    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);
    }
    return texture;
}
void ph_icon_draw(SDL_Renderer *renderer, ph_control_icon kind, SDL_Rect box, SDL_Color color)
{
    float sx, sy;
    SDL_RenderGetScale(renderer, &sx, &sy);
    int size = SDL_clamp((int)lroundf(fmaxf(sx, sy) * box.w), 8, 144);
    glyph_cache *entry = cache;
    while (entry && !(entry->renderer == renderer && entry->kind == kind && entry->size == size))
        entry = entry->next;
    if (!entry) {
        entry = calloc(1, sizeof *entry);
        if (!entry)
            return;
        entry->texture = render_icon(renderer, kind, size);
        if (!entry->texture) {
            free(entry);
            return;
        }
        entry->renderer = renderer;
        entry->kind = kind;
        entry->size = size;
        entry->next = cache;
        cache = entry;
    }
    SDL_SetTextureColorMod(entry->texture, color.r, color.g, color.b);
    SDL_RenderCopy(renderer, entry->texture, NULL, &box);
}
void ph_icon_release(SDL_Renderer *renderer)
{
    glyph_cache **slot = &cache;
    while (*slot) {
        glyph_cache *entry = *slot;
        if (entry->renderer == renderer) {
            *slot = entry->next;
            SDL_DestroyTexture(entry->texture);
            free(entry);
        } else
            slot = &entry->next;
    }
}
