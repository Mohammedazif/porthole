/* Checks that black side bars of TV-shaped frames are cropped, and nothing else is. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video/crop.h"

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

typedef struct frame {
    int w, h;
    uint8_t *y, *u, *v;
} frame;

static frame make_frame(int w, int h, uint8_t luma)
{
    frame f = {w, h, malloc((size_t)w * h), malloc((size_t)w * h / 4), malloc((size_t)w * h / 4)};
    memset(f.y, luma, (size_t)w * h);
    memset(f.u, 128, (size_t)w * h / 4);
    memset(f.v, 128, (size_t)w * h / 4);
    return f;
}

/* Paints columns [x0, x1) with a luma value (chroma stays neutral). */
static void paint(frame *f, int x0, int x1, uint8_t luma)
{
    for (int y = 0; y < f->h; y++)
        memset(f->y + (size_t)y * f->w + x0, luma, (size_t)(x1 - x0));
}

static ph_rect detect(const frame *f, double aspect)
{
    ph_yuv_planes p = {f->y, f->u, f->v, f->w, f->w / 2, f->w, f->h};
    return ph_video_content_rect(&p, aspect);
}

static void free_frame(frame *f)
{
    free(f->y);
    free(f->u);
    free(f->v);
}

static int is_full(ph_rect r, const frame *f)
{
    return r.x == 0 && r.y == 0 && r.w == f->w && r.h == f->h;
}

int main(void)
{
    /* Portrait frames are never cropped, even if their edges are black (dark mode). */
    {
        frame f = make_frame(500, 1080, 16);
        paint(&f, 100, 400, 200);
        CHECK(is_full(detect(&f, 0.463), &f));
        CHECK(is_full(detect(&f, 0), &f));
        free_frame(&f);
    }
    /* Pillarboxed 1920x1080: black bars, phone screen in the middle. */
    {
        frame f = make_frame(1920, 1080, 16);
        paint(&f, 710, 1210, 180);
        ph_rect r = detect(&f, 0.463);
        CHECK(r.x == 710 && r.w == 500 && r.y == 0 && r.h == 1080);
        r = detect(&f, 0); /* shape unknown: found by scanning */
        CHECK(r.x == 710 && r.w == 500);
        free_frame(&f);
    }
    /* Pillarboxed, phone in dark mode (screen mostly black): known shape still crops. */
    {
        frame f = make_frame(1920, 1080, 16);
        paint(&f, 900, 1000, 220); /* a little bright content in the middle only */
        ph_rect r = detect(&f, 0.463);
        CHECK(r.x == 710 && r.w == 500);
        free_frame(&f);
    }
    /* Real landscape content filling the frame: no crop. */
    {
        frame f = make_frame(1920, 1080, 90);
        CHECK(is_full(detect(&f, 0.463), &f));
        CHECK(is_full(detect(&f, 0), &f));
        free_frame(&f);
    }
    /* Landscape dark-mode app: black on the left, content on the right: no crop. */
    {
        frame f = make_frame(1920, 1080, 16);
        paint(&f, 1300, 1900, 200);
        CHECK(is_full(detect(&f, 0.463), &f));
        CHECK(is_full(detect(&f, 0), &f));
        free_frame(&f);
    }
    /* Off-centre content with unknown shape: not treated as pillarboxing. */
    {
        frame f = make_frame(1920, 1080, 16);
        paint(&f, 200, 700, 200);
        CHECK(is_full(detect(&f, 0), &f));
        free_frame(&f);
    }

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all crop checks passed\n");
    return 0;
}
