#include "ui/text.h"
#include "ui/window.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H

#include "ui/generated/font_face.h"

typedef struct raster_font {
    int pixels;
    int style;
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

/* Main-thread-only renderer/face ownership. Each size and weight is rasterized once at
 * the actual display DPI; glyph bitmaps are copied one-for-one to physical pixels. */
static font_context *fonts;
static FT_Library library;
static FT_Face faces[PH_FONT_STYLES];
static float advances[PH_FONT_STYLES][95];
static bool faces_ready;

/* Legacy scales 1..8 are regular Geist; PH_TEXT(px, style) picks both. */
static int font_size(int scale)
{
    if (scale >= 1000)
        return scale % 100 > 0 ? scale % 100 : 13;
    if (scale <= 1) return 13;
    if (scale == 2) return 14;
    if (scale == 3) return 20;
    if (scale > 8) scale = 8;
    return 26 + (scale - 4) * 6;
}

static int font_style(int scale)
{
    int style = scale >= 1000 ? (scale - 1000) / 100 : PH_FONT_REGULAR;
    return style >= 0 && style < PH_FONT_STYLES ? style : PH_FONT_REGULAR;
}

static unsigned int char_index(unsigned char c)
{
    return (c >= 32 && c <= 126 ? c : (unsigned char)'?') - 32u;
}

static void unload_faces(void)
{
    for (int i = 0; i < PH_FONT_STYLES; i++) {
        if (faces[i])
            FT_Done_Face(faces[i]);
        faces[i] = NULL;
    }
    if (library)
        FT_Done_FreeType(library);
    library = NULL;
    faces_ready = false;
}

static bool load_faces(void)
{
    static const struct { int mono; FT_Fixed weight; } wanted[PH_FONT_STYLES] = {
        [PH_FONT_REGULAR] = {0, 400}, [PH_FONT_MEDIUM] = {0, 500}, [PH_FONT_SEMIBOLD] = {0, 600},
        [PH_FONT_MONO] = {1, 400}, [PH_FONT_MONO_MEDIUM] = {1, 500},
    };

    if (faces_ready) return true;
    if (FT_Init_FreeType(&library)) return false;
    for (int i = 0; i < PH_FONT_STYLES; i++) {
        const unsigned char *bytes = wanted[i].mono ? font_mono_bytes : font_sans_bytes;
        FT_Long size = wanted[i].mono ? (FT_Long)sizeof font_mono_bytes : (FT_Long)sizeof font_sans_bytes;
        FT_MM_Var *axes = NULL;

        if (FT_New_Memory_Face(library, bytes, size, 0, &faces[i])) {
            unload_faces();
            return false;
        }
        /* Variable fonts: set the weight axis. A font without axes keeps its own weight. */
        if (FT_Get_MM_Var(faces[i], &axes) == 0 && axes) {
            FT_Fixed coords[8];
            FT_UInt n = axes->num_axis < 8 ? axes->num_axis : 8;
            for (FT_UInt a = 0; a < n; a++) {
                coords[a] = axes->axis[a].def;
                if (axes->axis[a].tag == FT_MAKE_TAG('w', 'g', 'h', 't'))
                    coords[a] = wanted[i].weight * 65536;
            }
            FT_Set_Var_Design_Coordinates(faces[i], n, coords);
            FT_Done_MM_Var(library, axes);
        }
        for (unsigned int c = 0; c < 95; c++) {
            FT_Load_Char(faces[i], c + 32, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING);
            advances[i][c] = (float)faces[i]->glyph->metrics.horiAdvance / (float)faces[i]->units_per_EM;
        }
    }
    faces_ready = true;
    return true;
}

static float advance(unsigned char c, int size, int style)
{
    return (load_faces() ? advances[style][char_index(c)] : .6f) * (float)size;
}

bool ph_text_attach(SDL_Renderer *renderer)
{
    for (font_context *f = fonts; f; f = f->next)
        if (f->renderer == renderer) return true;
    if (!renderer || !load_faces()) return false;
    font_context *f = calloc(1, sizeof *f);
    if (!f) return false;
    f->renderer = renderer; f->next = fonts; fonts = f;
    return true;
}

static raster_font *rasterize(font_context *context, int pixels, int style)
{
    for (raster_font *r = context->sizes; r; r = r->next)
        if (r->pixels == pixels && r->style == style) return r;
    raster_font *r = calloc(1, sizeof *r);
    FT_Face face = faces[style];
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
    r->pixels = pixels; r->style = style; r->next = context->sizes; context->sizes = r;
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
            if (!fonts) unload_faces();
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
    int size = font_size(scale), style = font_style(scale);
    if (!text) return 0;
    while (*text) {
        if (*text == '\n') { if (x > widest) widest = x; x = 0; }
        else x += advance((unsigned char)*text, size, style);
        text++;
    }
    return (int)((x > widest ? x : widest) + 0.99f);
}

static int layout(raster_font *raster, SDL_Renderer *renderer, const char *text,
                   int start_x, int start_y, int width, int scale)
{
    int size = font_size(scale), style = font_style(scale), line = ph_text_line_height(scale), y = start_y;
    float x = 0, sx = 1, sy = 1;
    if (renderer) SDL_RenderGetScale(renderer, &sx, &sy);
    if (sx <= 0) sx = 1;
    if (sy <= 0) sy = 1;
    if (!text || width < 1) return start_y;
    while (*text) {
        if (*text == '\n') { x = 0; y += line; text++; continue; }
        if (*text == ' ') { if (x > 0) x += advance(' ', size, style); text++; continue; }
        size_t word = strcspn(text, " \n");
        float word_width = 0;
        for (size_t i = 0; i < word; i++) word_width += advance((unsigned char)text[i], size, style);
        if (x > 0 && x + word_width > (float)width) { x = 0; y += line; }
        for (size_t i = 0; i < word; i++) {
            unsigned int index = char_index((unsigned char)text[i]);
            float step = advance((unsigned char)text[i], size, style);
            if (x > 0 && x + step > (float)width) { x = 0; y += line; }
            if (raster && raster->glyph[index].w && raster->glyph[index].h) {
                SDL_Rect src = raster->glyph[index];
                float ascent = (float)faces[style]->ascender * (float)size / (float)faces[style]->units_per_EM;
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
    raster_font *r = rasterize(f, pixels, font_style(scale));
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
