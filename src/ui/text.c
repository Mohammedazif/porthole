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
    unsigned first;
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

/* Decode one scalar. Invalid or truncated sequences consume one byte. */
static unsigned next_character(const char **cursor)
{
    const unsigned char *p = (const unsigned char *)*cursor;
    unsigned c = *p++;
    *cursor = (const char *)p;
    if (c < 128)
        return c;
    int count = c >= 0xF0 && c <= 0xF4  ? 3
                : c >= 0xE0 && c < 0xF0 ? 2
                : c >= 0xC2 && c < 0xE0 ? 1
                                        : 0;
    if (!count)
        return '?';
    unsigned value = c & (0x7Fu >> (count + 1));
    for (int i = 0; i < count; i++) {
        if ((p[i] & 0xC0) != 0x80)
            return '?';
        value = (value << 6) | (p[i] & 0x3F);
    }
    if ((count == 2 && value < 0x800) || (count == 3 && value < 0x10000) ||
        (value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF)
        return '?';
    *cursor = (const char *)p + count;
    return value;
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

static unsigned supported(unsigned c, int style)
{
    return c >= 32 && (c < 127 || FT_Get_Char_Index(faces[style], c)) ? c : '?';
}
static float advance(unsigned c, int size, int style)
{
    if (!load_faces())
        return .6f * (float)size;
    c = supported(c, style);
    if (c < 127)
        return advances[style][c - 32] * (float)size;
    if (FT_Load_Char(faces[style], c, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING))
        return .6f * (float)size;
    return (float)faces[style]->glyph->metrics.horiAdvance * (float)size /
           (float)faces[style]->units_per_EM;
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

static raster_font *rasterize(font_context *context, int pixels, int style, unsigned first)
{
    for (raster_font *r = context->sizes; r; r = r->next)
        if (r->pixels == pixels && r->style == style && r->first == first)
            return r;
    raster_font *r = calloc(1, sizeof *r);
    FT_Face face = faces[style];
    int cell = (first == 32 ? pixels : 2 * pixels) + 8, row = pixels * 2 + 4;
    int width = cell * 16, height = row * 6;
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!r || !surface || FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pixels)) {
        free(r); if (surface) SDL_FreeSurface(surface); return NULL;
    }
    SDL_FillRect(surface, NULL, 0);
    for (unsigned int i = 0; i < 95; i++) {
        if (FT_Load_Char(face, i + first, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT))
            continue;
        FT_GlyphSlot g = face->glyph;
        int x = (int)(i % 16u) * cell, y = (int)(i / 16u) * row;
        r->glyph[i] = (SDL_Rect){x, y, SDL_min(cell, (int)g->bitmap.width),
                                 SDL_min(row, (int)g->bitmap.rows)};
        r->bearing_x[i] = g->bitmap_left; r->bearing_y[i] = g->bitmap_top;
        for (unsigned int gy = 0; gy < (unsigned)r->glyph[i].h; gy++) {
            unsigned char *dst = (unsigned char *)surface->pixels + (y + (int)gy) * surface->pitch + x * 4;
            const unsigned char *src = g->bitmap.buffer + (int)gy * g->bitmap.pitch;
            for (unsigned int gx = 0; gx < (unsigned)r->glyph[i].w; gx++) {
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
    r->first = first;
    r->pixels = pixels;
    r->style = style;
    r->next = context->sizes;
    context->sizes = r;
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
        unsigned c = next_character(&text);
        if (c == '\n') {
            if (x > widest)
                widest = x;
            x = 0;
        } else
            x += advance(c, size, style);
    }
    return (int)((x > widest ? x : widest) + 0.99f);
}

static int layout(font_context *context, SDL_Color color, SDL_Renderer *renderer, const char *text,
                  int start_x, int start_y, int width, int scale)
{
    int size = font_size(scale), style = font_style(scale), line = ph_text_line_height(scale), y = start_y;
    float x = 0, sx = 1, sy = 1;
    if (renderer) SDL_RenderGetScale(renderer, &sx, &sy);
    if (sx <= 0) sx = 1;
    if (sy <= 0) sy = 1;
    if (!text || width < 1) return start_y;
    int pixels = SDL_clamp((int)lroundf(size * sy), 1, 256);
    raster_font *raster = NULL;
    while (*text) {
        if (*text == '\n') { x = 0; y += line; text++; continue; }
        if (*text == ' ') { if (x > 0) x += advance(' ', size, style); text++; continue; }
        size_t word = strcspn(text, " \n");
        float word_width = 0;
        for (const char *p = text; p < text + word;)
            word_width += advance(next_character(&p), size, style);
        if (x > 0 && x + word_width > (float)width) { x = 0; y += line; }
        for (const char *p = text; p < text + word;) {
            unsigned c = next_character(&p);
            float step = advance(c, size, style);
            c = faces_ready ? supported(c, style) : '?';
            unsigned first = 32 + ((c - 32) / 95) * 95, index = c - first;
            if (context && (!raster || raster->first != first)) {
                raster = rasterize(context, pixels, style, first);
                if (raster) {
                    SDL_SetTextureColorMod(raster->atlas, color.r, color.g, color.b);
                    SDL_SetTextureAlphaMod(raster->atlas, color.a);
                }
            }
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
    return layout(NULL, (SDL_Color){0}, NULL, text, 0, 0, width, scale);
}

int ph_text_draw(SDL_Renderer *renderer, const char *text, int x, int y, int width, int scale,
                 SDL_Color color)
{
    font_context *f;
    for (f = fonts; f; f = f->next)
        if (f->renderer == renderer) break;
    if (!f) return y;
    return layout(f, color, renderer, text, x, y, width, scale);
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
