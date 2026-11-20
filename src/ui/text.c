#include "ui/text.h"
#include "ui/window.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "ui/generated/font_face.h"

typedef struct raster_font {
    int pixels;
    SDL_Texture *atlas;
    SDL_Rect glyph[95];
    int bearing_x[95], bearing_y[95];
    struct raster_font *next;
} raster_font;

typedef struct font_context {
    SDL_Renderer *renderer;
    raster_font *sizes;
    struct font_context *next;
} font_context;

/* Main-thread-only renderer/face ownership. Each size is rasterized once at the
 * actual display DPI; glyph bitmaps are copied one-for-one to physical pixels. */
static font_context *fonts;
static FT_Library library;
static FT_Face face;
static float advances[95];

static int font_size(int scale)
{
    if (scale <= 1) return 13;
    if (scale == 2) return 14;
    if (scale == 3) return 20;
    if (scale > 8) scale = 8;
    return 26 + (scale - 4) * 6;
}

static unsigned int char_index(unsigned char c)
{
    return (c >= 32 && c <= 126 ? c : (unsigned char)'?') - 32u;
}

static bool load_face(void)
{
    if (face) return true;
    if (FT_Init_FreeType(&library)) return false;
    if (FT_New_Memory_Face(library, font_face_bytes, (FT_Long)sizeof font_face_bytes, 0, &face)) {
        FT_Done_FreeType(library); library = NULL; return false;
    }
    for (unsigned int i = 0; i < 95; i++) {
        FT_Load_Char(face, i + 32, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING);
        advances[i] = (float)face->glyph->metrics.horiAdvance / (float)face->units_per_EM;
    }
    return true;
}

static float advance(unsigned char c, int size)
{
    return (load_face() ? advances[char_index(c)] : .6f) * (float)size;
}

bool ph_text_attach(SDL_Renderer *renderer)
{
    for (font_context *f = fonts; f; f = f->next)
        if (f->renderer == renderer) return true;
    if (!renderer || !load_face()) return false;
    font_context *f = calloc(1, sizeof *f);
    if (!f) return false;
    f->renderer = renderer; f->next = fonts; fonts = f;
    return true;
}

static raster_font *rasterize(font_context *context, int pixels)
{
    for (raster_font *r = context->sizes; r; r = r->next)
        if (r->pixels == pixels) return r;
    raster_font *r = calloc(1, sizeof *r);
    int cell = pixels + 8, row = pixels * 2 + 4;
    int width = cell * 16, height = row * 6;
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!r || !surface || FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pixels)) {
        free(r); if (surface) SDL_FreeSurface(surface); return NULL;
    }
    SDL_FillRect(surface, NULL, 0);
    for (unsigned int i = 0; i < 95; i++) {
        if (FT_Load_Char(face, i + 32, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) continue;
        FT_GlyphSlot g = face->glyph;
        int x = (int)(i % 16u) * cell, y = (int)(i / 16u) * row;
        r->glyph[i] = (SDL_Rect){x, y, (int)g->bitmap.width, (int)g->bitmap.rows};
        r->bearing_x[i] = g->bitmap_left; r->bearing_y[i] = g->bitmap_top;
        for (unsigned int gy = 0; gy < g->bitmap.rows; gy++) {
            unsigned char *dst = (unsigned char *)surface->pixels + (y + (int)gy) * surface->pitch + x * 4;
            const unsigned char *src = g->bitmap.buffer + (int)gy * g->bitmap.pitch;
            for (unsigned int gx = 0; gx < g->bitmap.width; gx++) {
                dst[gx * 4] = dst[gx * 4 + 1] = dst[gx * 4 + 2] = 255;
                dst[gx * 4 + 3] = src[gx];
            }
        }
    }
    r->atlas = SDL_CreateTextureFromSurface(context->renderer, surface);
    SDL_FreeSurface(surface);
    if (!r->atlas) { free(r); return NULL; }
    SDL_SetTextureBlendMode(r->atlas, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(r->atlas, SDL_ScaleModeNearest);
    r->pixels = pixels; r->next = context->sizes; context->sizes = r;
    return r;
}

void ph_text_detach(SDL_Renderer *renderer)
{
    font_context **link = &fonts;
    while (*link) {
        font_context *f = *link;
        if (f->renderer == renderer) {
            *link = f->next;
            while (f->sizes) {
                raster_font *r = f->sizes; f->sizes = r->next;
                SDL_DestroyTexture(r->atlas); free(r);
            }
            free(f);
            if (!fonts) { FT_Done_Face(face); FT_Done_FreeType(library); face = NULL; library = NULL; }
            return;
        }
        link = &f->next;
    }
}

int ph_text_line_height(int scale)
{
    return font_size(scale) * 3 / 2;
}

int ph_text_width(const char *text, int scale)
{
    float x = 0, widest = 0;
    int size = font_size(scale);
    if (!text) return 0;
    while (*text) {
        if (*text == '\n') { if (x > widest) widest = x; x = 0; }
        else x += advance((unsigned char)*text, size);
        text++;
    }
    return (int)((x > widest ? x : widest) + 0.99f);
}

static int layout(raster_font *raster, SDL_Renderer *renderer, const char *text,
                   int start_x, int start_y, int width, int scale)
{
    int size = font_size(scale), line = ph_text_line_height(scale), y = start_y;
    float x = 0, sx = 1, sy = 1;
    if (renderer) SDL_RenderGetScale(renderer, &sx, &sy);
    if (sx <= 0) sx = 1;
    if (sy <= 0) sy = 1;
    if (!text || width < 1) return start_y;
    while (*text) {
        if (*text == '\n') { x = 0; y += line; text++; continue; }
        if (*text == ' ') { if (x > 0) x += advance(' ', size); text++; continue; }
        size_t word = strcspn(text, " \n");
        float word_width = 0;
        for (size_t i = 0; i < word; i++) word_width += advance((unsigned char)text[i], size);
        if (x > 0 && x + word_width > (float)width) { x = 0; y += line; }
        for (size_t i = 0; i < word; i++) {
            unsigned int index = char_index((unsigned char)text[i]);
            float step = advance((unsigned char)text[i], size);
            if (x > 0 && x + step > (float)width) { x = 0; y += line; }
            if (raster && raster->glyph[index].w && raster->glyph[index].h) {
                SDL_Rect src = raster->glyph[index];
                float ascent = (float)face->ascender * (float)size / (float)face->units_per_EM;
                SDL_FRect dst = {
                    (roundf(((float)start_x + x) * sx) + (float)raster->bearing_x[index]) / sx,
                    (roundf(((float)y + ascent) * sy) - (float)raster->bearing_y[index]) / sy,
                    (float)src.w / sx, (float)src.h / sy
                };
                SDL_RenderCopyF(renderer, raster->atlas, &src, &dst);
            }
            x += step;
        }
        text += word;
    }
    return y + line;
}

int ph_text_height(const char *text, int width, int scale)
{
    return layout(NULL, NULL, text, 0, 0, width, scale);
}

int ph_text_draw(SDL_Renderer *renderer, const char *text, int x, int y, int width, int scale,
                 SDL_Color color)
{
    font_context *f;
    for (f = fonts; f; f = f->next)
        if (f->renderer == renderer) break;
    if (!f) return y;
    float sx, sy;
    SDL_RenderGetScale(renderer, &sx, &sy);
    int pixels = (int)lroundf((float)font_size(scale) * (sy > 0 ? sy : 1));
    if (pixels < 1) pixels = 1;
    if (pixels > 256) pixels = 256;
    raster_font *r = rasterize(f, pixels);
    if (!r) return y;
    SDL_SetTextureColorMod(r->atlas, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(r->atlas, color.a);
    return layout(r, renderer, text, x, y, width, scale);
}

int ph_ui_text_height(const char *text, int width, int scale)
{
    return ph_text_height(text, width, scale);
}

int ph_ui_draw_text(SDL_Renderer *renderer, const char *text, int x, int y, int width, int scale,
                    SDL_Color color)
{
    return ph_text_draw(renderer, text, x, y, width, scale, color);
}
